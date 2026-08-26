// RdpClipboard.cpp — cliprdr 通道实现。设计与线程模型见 RdpClipboard.h 文件头。

#include "RdpClipboard.h"

#include "RdpClient.h"   // rdpDebugLog（与建连日志同一份文件，便于按时间线对读）

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutexLocker>
#include <QUrl>

#include <cstdlib>   // free()：winpr 的 ClipboardGetData 返回 malloc 内存

// winsock2.h 必须早于任何 windows.h（WinPR 头会间接引入）——同 RdpClient.cpp。
#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <windows.h>
#endif

#ifdef CUBESHELL_HAVE_FREERDP
#include <freerdp/channels/cliprdr.h>
#include <freerdp/client/cliprdr.h>
#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
#include <freerdp/client/client_cliprdr_file.h>
#endif
#include <winpr/clipboard.h>
#include <winpr/error.h>
#include <winpr/user.h>
#endif

namespace cubeshell {

namespace {

// ---------------------------------------------------------------------------
// FILEDESCRIPTORW（[MS-RDPECLIP] 2.2.5.2.3.1）—— 定长 592 字节
//   flags 4 | reserved1 32 | fileAttributes 4 | reserved2 16 |
//   lastWriteTime 8 | fileSizeHigh 4 | fileSizeLow 4 | fileName 520
// 常量在此本地定义而不取 FreeRDP 头：解析函数要在没有 FreeRDP 的构建里
// 也能编译（命令行后备后端），且这样布局一眼可核对。
// ---------------------------------------------------------------------------
constexpr int kDescriptorSize     = 592;
constexpr int kOffsetFlags        = 0;
constexpr int kOffsetAttributes   = 36;
constexpr int kOffsetFileSizeHigh = 64;
constexpr int kOffsetFileSizeLow  = 68;
constexpr int kOffsetFileName     = 72;
constexpr int kMaxFileNameChars   = 260;   // 520 字节 / 2

constexpr quint32 kFdAttributes  = 0x00000004;   // FD_ATTRIBUTES
constexpr quint32 kFdFileSize    = 0x00000040;   // FD_FILESIZE
constexpr quint32 kAttrDirectory = 0x00000010;   // FILE_ATTRIBUTE_DIRECTORY

// 一次剪贴板里的文件数上限。远端是不可信输入，畸形 cItems 不能让我们照着
// 分配内存。65535 远超任何真实的「选中一批文件」操作。
constexpr quint32 kMaxDescriptors = 65535;

// 取回文件时每次向远端请求的字节数。虚拟通道内部还会再分片，这里只是控制
// 往返次数与内存占用的折中。
constexpr quint32 kFetchChunkBytes = 256 * 1024;

quint32 readLE32(const QByteArray &data, int offset)
{
    const auto *b = reinterpret_cast<const quint8 *>(data.constData());
    return quint32(b[offset]) | (quint32(b[offset + 1]) << 8)
           | (quint32(b[offset + 2]) << 16) | (quint32(b[offset + 3]) << 24);
}

// UTF-16LE 定长字段 → QString。协议固定小端，故逐对字节手工组装而不是强转
// char16_t*：既避开对齐问题，也不依赖主机字节序。
QString readUtf16Le(const QByteArray &data, int offset, int maxChars)
{
    const auto *b = reinterpret_cast<const quint8 *>(data.constData()) + offset;
    QString out;
    out.reserve(maxChars);
    for (int i = 0; i < maxChars; ++i) {
        const ushort ch = ushort(b[i * 2]) | ushort(ushort(b[i * 2 + 1]) << 8);
        if (ch == 0)
            break;   // 字段是定长的，null 之后全是填充
        out.append(QChar(ch));
    }
    return out;
}

void writeLE32(char *dst, quint32 value)
{
    dst[0] = char(value & 0xff);
    dst[1] = char((value >> 8) & 0xff);
    dst[2] = char((value >> 16) & 0xff);
    dst[3] = char((value >> 24) & 0xff);
}

// 本机路径 → 能公告给远端的规范绝对路径；rejected 收下被丢掉的条目及原因。
//
// canonicalFilePath 这一步不是洁癖。macOS 上 Finder 复制文件放进剪贴板的往往是
// file:///.file/id=6571367.2624455 这类 FSRef 魔法 URL，Qt 原样交给我们；把它
// 塞进 uri-list 交给 winpr 合成器，stat 不到就一条描述符也产不出来，日志里只留
// 一行「uri-list 1 项 → 描述符 0 字节」，界面上毫无提示。canonical 会把这类路径
// 解析成真实路径，同时顺手滤掉已经不存在、以及读不到的条目（macOS 上桌面/下载
// 目录受 TCC 管控，未授权时 stat 就是失败的，早点说出来比让人怀疑远端好）。
QStringList resolveLocalFiles(const QStringList &paths, QStringList *rejected)
{
    QStringList out;
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        const QFileInfo info(path);
        const QString canonical = info.canonicalFilePath();
        const char *reason = nullptr;
        if (canonical.isEmpty())
            reason = "路径无法解析或已不存在";
        else if (!QFileInfo(canonical).isReadable())
            reason = "本机不可读（macOS 可能需要授予文件访问权限）";
        if (reason) {
            if (rejected)
                rejected->append(QStringLiteral("%1（%2）")
                                     .arg(path, QString::fromUtf8(reason)));
            continue;
        }
        if (!out.contains(canonical))
            out.append(canonical);
    }
    return out;
}

// destDir + 已过滤的相对名 → 落盘绝对路径；万一还是跑出了 destDir 就返回空。
// sanitizeRemoteName 已经挡过一轮，这里是第二道闸门——这是唯一按远端给的名字
// 往本地磁盘写的路径，值得多花一次 cleanPath。
QString resolveFetchTarget(const QString &destDir, const QString &relative)
{
    if (relative.isEmpty() || destDir.isEmpty())
        return {};
    const QString root = QDir::cleanPath(QDir(destDir).absolutePath());
    const QString target = QDir::cleanPath(QDir(root).absoluteFilePath(relative));
    if (target == root || !target.startsWith(root + QLatin1Char('/')))
        return {};
    return target;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl —— FreeRDP 状态与跨线程状态。见头文件 pimpl 说明。
// ---------------------------------------------------------------------------

struct RdpClipboard::Impl {
    // --- 主线程写 / worker+通道线程读，由 RdpClipboard::m_mutex 保护 ---
    RdpClipboardSnapshot snapshot;
    bool announcePending = false;      // 快照变了，待向远端公告
    bool fetchRequested = false;       // 用户点了「取回」
    bool cancelRequested = false;      // 主线程要求中止正在进行的取回
    QString fetchDestDir;
    QList<RdpRemoteFile> remoteFiles;  // 远端最近公告的清单

#ifdef CUBESHELL_HAVE_FREERDP
    // cliprdr 的回调跑在**通道自己的线程**上（channel_client_thread_proc →
    // cliprdr_order_recv → 回调），不是 RdpClient 的 worker 线程。本文件早先假设
    // 它们同在 worker 线程，于是把下面这批字段当成单线程独占——那是错的：
    // attach/detach/abandon/pump 在 worker 线程跑，回调在通道线程跑，两边同时
    // 读写同一份 fetch 状态机与通道指针。
    // channelMutex 就是把这两侧串起来的那把锁。锁序固定 channelMutex → m_mutex，
    // 不得反向（主线程入口只拿 m_mutex，永远不拿 channelMutex，故不会成环）。
    QMutex channelMutex;

    // --- 以下全部由 channelMutex 保护 ---
    CliprdrClientContext *cliprdr = nullptr;
    wClipboard *clipboard = nullptr;
#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
    CliprdrFileContext *fileContext = nullptr;
#endif

    bool ready = false;                 // 已收到 MonitorReady 并发出能力集

    UINT32 fmtUriList = 0;              // "text/uri-list"（喂给 winpr 合成器）
    UINT32 fmtFileDescriptor = 0;       // "FileGroupDescriptorW"
    UINT32 fmtFileContents = 0;         // "FileContents"
    UINT32 remoteGeneralFlags = 0;
    UINT32 pendingRequestFormat = 0;    // 我们发出去、还没收到响应的格式 id
    UINT32 remoteDescriptorFormat = 0;  // 远端公告 FileGroupDescriptorW 用的 id
    UINT32 nextStreamId = 1;

    // cliprdr_file_context_init() 会往 cliprdr context 上装它自己的处理器，
    // 其中可能包含 ServerFileContentsResponse（它的 FUSE 路径要用）。我们把原值
    // 存下来，遇到不是自己发的 streamId 就转回给它，不抢它的活。
    pcCliprdrServerFileContentsResponse chainedFileContentsResponse = nullptr;

    // --- 远端 → 本机文件取回状态机（同样由 channelMutex 保护：pump() 在 worker
    //     线程发起，之后每一块数据都是通道线程在回调里推进）---
    struct Fetch {
        bool active = false;
        QString destDir;
        QList<RdpRemoteFile> files;
        int index = -1;             // 当前处理到 files 的哪一项
        QFile file;                 // 当前落盘目标
        quint64 written = 0;        // 当前文件已落盘字节
        quint64 currentSize = 0;
        bool awaitingSize = false;  // 描述符没带 FD_FILESIZE，正在问远端
        UINT32 streamId = 0;        // 最近一次 FileContents 请求的 streamId
        QStringList donePaths;
        qint64 totalBytes = 0;
        qint64 receivedBytes = 0;

        // QFile 不可赋值，所以显式 reset 而不是 `f = Fetch{}`。
        void reset()
        {
            if (file.isOpen())
                file.close();
            file.setFileName(QString());
            active = false;
            destDir.clear();
            files.clear();
            index = -1;
            written = 0;
            currentSize = 0;
            awaitingSize = false;
            streamId = 0;
            donePaths.clear();
            totalBytes = 0;
            receivedBytes = 0;
        }
    } fetch;
#endif
};

// ---------------------------------------------------------------------------
// 纯函数（无状态，单测直接调）
// ---------------------------------------------------------------------------

QList<RdpRemoteFile> RdpClipboard::parseFileDescriptors(const QByteArray &data,
                                                        QString *error)
{
    if (error)
        error->clear();
    const auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return QList<RdpRemoteFile>();
    };

    if (data.size() < 4)
        return fail(QStringLiteral("文件清单不足 4 字节（%1）").arg(data.size()));

    const quint32 count = readLE32(data, 0);
    if (count == 0)
        return {};   // 合法：远端把剪贴板清空了
    if (count > kMaxDescriptors)
        return fail(QStringLiteral("文件数 %1 超出上限 %2").arg(count).arg(kMaxDescriptors));

    const qint64 needed = qint64(4) + qint64(count) * kDescriptorSize;
    if (qint64(data.size()) < needed)
        return fail(QStringLiteral("文件清单声明 %1 项需 %2 字节，实际只有 %3")
                        .arg(count)
                        .arg(needed)
                        .arg(data.size()));

    QList<RdpRemoteFile> files;
    files.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) {
        const int base = 4 + int(i) * kDescriptorSize;
        const quint32 flags = readLE32(data, base + kOffsetFlags);
        const quint32 attrs = readLE32(data, base + kOffsetAttributes);

        RdpRemoteFile entry;
        entry.name = sanitizeRemoteName(
            readUtf16Le(data, base + kOffsetFileName, kMaxFileNameChars));
        // 名字过不了闸门就整条丢掉——绝不按远端给的路径往本地写。
        // listIndex 记原始下标，丢弃造成的错位不会带到 FileContents 请求上。
        if (entry.name.isEmpty())
            continue;

        entry.listIndex = i;
        entry.isDirectory = (flags & kFdAttributes) && (attrs & kAttrDirectory);
        if (flags & kFdFileSize) {
            entry.size = (quint64(readLE32(data, base + kOffsetFileSizeHigh)) << 32)
                         | quint64(readLE32(data, base + kOffsetFileSizeLow));
            entry.sizeKnown = true;
        }
        files.append(entry);
    }
    return files;
}

QString RdpClipboard::sanitizeRemoteName(const QString &raw)
{
    if (raw.isEmpty())
        return {};

    QString s = raw;
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));   // 远端是 Windows

    // 控制字符（含 NUL/换行）一律判非法：出现在文件名里只可能是攻击或损坏。
    for (const QChar c : s) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f)
            return {};
    }
    // 绝对路径与盘符：必须是相对路径才允许落到目标目录下。
    if (s.startsWith(QLatin1Char('/')))
        return {};
    if (s.size() >= 2 && s.at(1) == QLatin1Char(':'))
        return {};

    const QStringList parts = s.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};

    QStringList out;
    out.reserve(parts.size());
    for (const QString &part : parts) {
        // 路径穿越。"." 也挡掉——它没有正当用途，放过去只是增加解释成本。
        if (part == QLatin1String(".") || part == QLatin1String(".."))
            return {};
        // Windows 保留字符。远端是 Windows、本机可能不是，统一按最严的一套挡，
        // 免得在某个平台上生成诡异文件名。
        for (const QChar c : part) {
            if (QStringLiteral(":*?\"<>|").contains(c))
                return {};
        }
        // 尾部点/空格：Windows 会静默吃掉，导致落盘名与远端名不一致。
        if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')))
            return {};
        out.append(part);
    }
    return out.join(QLatin1Char('/'));
}

QByteArray RdpClipboard::packFileList(const QByteArray &descriptors, QString *error)
{
    const int size = descriptors.size();
    if (size == 0) {
        if (error)
            *error = QStringLiteral("描述符为空");
        return {};
    }
    if (size % kDescriptorSize == 4) {
        // 已经带头：核对 cItems 与字节数自洽，然后原样透传（别重复加头）。
        const quint32 count = readLE32(descriptors, 0);
        if (quint64(count) * kDescriptorSize + 4 != quint64(size)) {
            if (error)
                *error = QStringLiteral("自带 cItems=%1 与 %2 字节不自洽")
                             .arg(count)
                             .arg(size);
            return {};
        }
        return descriptors;
    }
    if (size % kDescriptorSize != 0) {
        if (error)
            *error = QStringLiteral("%1 字节不是 %2 的整数倍").arg(size).arg(kDescriptorSize);
        return {};
    }
    const quint32 count = quint32(size / kDescriptorSize);
    if (count > kMaxDescriptors) {
        if (error)
            *error = QStringLiteral("条目数 %1 超上限 %2").arg(count).arg(kMaxDescriptors);
        return {};
    }
    QByteArray out;
    out.resize(4);
    writeLE32(out.data(), count);
    out.append(descriptors);
    return out;
}

QByteArray RdpClipboard::buildUriList(const QStringList &paths)
{
    // text/uri-list：每行一个 URI，CRLF 分隔。winpr 的剪贴板合成器吃这个格式，
    // 由它产出 FileGroupDescriptorW（省掉手写 packed file list 序列化）。
    QByteArray out;
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        out += QUrl::fromLocalFile(path).toEncoded();
        out += "\r\n";
    }
    return out;
}

quint32 RdpClipboard::chunkWriteLen(quint64 declaredSize, quint64 written, quint32 respLen)
{
    // 远端是不可信输入，多给的一律截掉：按它说的长度写会冲破声明的文件大小，
    // 而"文件比声明的大"恰恰是取回损坏时唯一还能被察觉的痕迹（见头文件注释）。
    if (written >= declaredSize)
        return 0;
    return quint32(qMin<quint64>(respLen, declaredSize - written));
}

// ---------------------------------------------------------------------------
// 构造 / 主线程 API
// ---------------------------------------------------------------------------

RdpClipboard::RdpClipboard(QObject *parent)
    : QObject(parent)
    , m_impl(new Impl)
{
}

RdpClipboard::~RdpClipboard()
{
#ifdef CUBESHELL_HAVE_FREERDP
    // 兜底：正常路径下 RdpClient 的 worker 会在会话收尾时 abandon()。真漏了的话
    // 在这里补——用 abandon 而非 detach，析构时通道大概率已经不在了。
    abandon();
#endif
    delete m_impl;
}

void RdpClipboard::setLocalSnapshot(const RdpClipboardSnapshot &snapshot)
{
    QMutexLocker lock(&m_mutex);
    // 内容没变就不重复公告：QClipboard::dataChanged 在某些平台上会连发几次，
    // 每次都往通道里推一遍 FormatList 会让远端剪贴板反复失效。
    if (m_impl->snapshot.text == snapshot.text
        && m_impl->snapshot.files == snapshot.files)
        return;
    m_impl->snapshot = snapshot;
    m_impl->announcePending = true;
}

QList<RdpRemoteFile> RdpClipboard::remoteFiles() const
{
    QMutexLocker lock(&m_mutex);
    return m_impl->remoteFiles;
}

void RdpClipboard::fetchRemoteFiles(const QString &destDir)
{
    QMutexLocker lock(&m_mutex);
    if (m_impl->remoteFiles.isEmpty())
        return;
    m_impl->fetchDestDir = destDir;
    m_impl->fetchRequested = true;
}

void RdpClipboard::cancelFetch()
{
    QMutexLocker lock(&m_mutex);
    // 还没派发就直接撤掉请求；已经在传了则交给 pump() 去中止——中止要动 fetch
    // 状态机和文件句柄，那些由 channelMutex 保护，主线程不许碰（锁序是
    // channelMutex → m_mutex，这里反向拿就会成环）。
    m_impl->fetchRequested = false;
    m_impl->cancelRequested = true;
}

void RdpClipboard::resetSessionState()
{
    QMutexLocker lock(&m_mutex);
    m_impl->remoteFiles.clear();
    m_impl->fetchRequested = false;
    m_impl->cancelRequested = false;
    m_impl->fetchDestDir.clear();
    // 快照不清：那是本机剪贴板的内容，与会话生死无关。重连后要能立刻再公告，
    // 所以顺手把 announcePending 抬起来。
    m_impl->announcePending = !m_impl->snapshot.isEmpty();
}

// ---------------------------------------------------------------------------
// 以下全部是 FreeRDP 相关；没有库时 attach/detach/pump 是空操作。
// ---------------------------------------------------------------------------

#ifndef CUBESHELL_HAVE_FREERDP

void RdpClipboard::attach(void *) {}
void RdpClipboard::detach(void *) {}
void RdpClipboard::abandon() {}
void RdpClipboard::pump() {}

#else

namespace {

// --- CliprdrClientContext → RdpClipboard 注册表 -----------------------------
// 为什么不用 cliprdr->custom 存 this（这正是「点连接必崩」的根因）：
// cliprdr_file_context_init(file, cliprdr) 内部会执行 cliprdr->custom = file
//（FreeRDP 3.30.0 反汇编：str x0, [x1, #0x8]，偏移 8 就是 custom），而它同时装上的
// ServerFileContentsRequest 处理器又要靠 custom 取回 file context
//（ldr x20, [x0, #0x8]）。也就是说 custom 是 FreeRDP 文件上下文的私有字段，
// 我们和它抢：
//   · 我们先写 this、init 后写覆盖 → 我们的回调把 file context 当 RdpClipboard*
//     用，m_impl 读到的是结构体中段的一个函数指针，随后 d->fileContext 就是野值，
//     第一条 remote_set_flags 的存储直接 SIGBUS（实测三次崩溃地址完全一致）。
//   · 反过来我们在 init 之后再写 this → 换成 FreeRDP 的文件处理器崩。
// 也别想着「既然 init 后 custom 就是 fileContext，那就
// cliprdr_file_context_get_context(ctx->custom) 绕回 this」——那条路把一个没有
// 文档保证的等式当接口用，且 file_context_new 失败或 HAVE_FREERDP_CLIENT 关掉时
// custom 压根不是 fileContext，静默错到别处去。
// 所以 custom 完全归 FreeRDP，我们自己另开一张表。用表而不是全局单例，是因为
// 一个进程里可以同时开多个 RDP 标签页，每个会话一条 cliprdr 通道。
struct ClipboardRegistry {
    QMutex mutex;
    QHash<CliprdrClientContext *, RdpClipboard *> map;
};

ClipboardRegistry &registry()
{
    static ClipboardRegistry instance;
    return instance;
}

UINT sendFormatListResponse(CliprdrClientContext *cliprdr, bool ok)
{
    CLIPRDR_FORMAT_LIST_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    response.common.dataLen = 0;
    return cliprdr->ClientFormatListResponse(cliprdr, &response);
}

// data == nullptr 表示「拿不到这个格式」，按协议回 CB_RESPONSE_FAIL。
UINT sendDataResponse(CliprdrClientContext *cliprdr, const BYTE *data, UINT32 size)
{
    CLIPRDR_FORMAT_DATA_RESPONSE response = {};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = data ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    response.common.dataLen = data ? size : 0;
    response.requestedFormatData = data;
    return cliprdr->ClientFormatDataResponse(cliprdr, &response);
}

UINT32 chunkFor(quint64 total, quint64 written)
{
    return UINT32(qMin<quint64>(total - written, kFetchChunkBytes));
}

} // namespace

// cliprdr 的 C 回调集中在这里（RdpClipboard 的 friend），全部跑在 worker 线程。
// 用一个 struct 收口是为了让回调能直接读写 Impl，同时把 FreeRDP 类型完全挡在
// 头文件外面。
struct RdpClipboardBridge {
    // 查注册表而不是读 ctx->custom（原因见上面 ClipboardRegistry 的说明）。
    // 返回的对象由 RdpClient 持有，生命周期长于会话：worker 线程停下、FreeRDP
    // 上下文释放之后才轮到它析构，所以这里拿到的指针不会悬空。会话是否还挂着
    // 通道，由调用方在拿到 channelMutex 之后查 d->cliprdr 判定。
    static RdpClipboard *self(CliprdrClientContext *ctx)
    {
        if (!ctx)
            return nullptr;
        ClipboardRegistry &reg = registry();
        QMutexLocker lock(&reg.mutex);
        return reg.map.value(ctx, nullptr);
    }

    // QString → CF_UNICODETEXT 载荷：UTF-16LE + NUL 结尾，换行统一成 CRLF
    // （远端是 Windows，只给 \n 的话记事本里会连成一行）。
    static QByteArray toUnicodeText(const QString &text)
    {
        QString normalized = text;
        normalized.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        normalized.replace(QLatin1Char('\n'), QLatin1String("\r\n"));

        QByteArray out;
        out.reserve((normalized.size() + 1) * 2);
        for (const QChar c : normalized) {
            const ushort u = c.unicode();
            out.append(char(u & 0xff));
            out.append(char((u >> 8) & 0xff));
        }
        out.append('\0');   // CF_UNICODETEXT 要求 NUL 结尾
        out.append('\0');
        return out;
    }

    static QString fromUnicodeText(const QByteArray &data)
    {
        QString out;
        const int chars = data.size() / 2;
        out.reserve(chars);
        const auto *b = reinterpret_cast<const quint8 *>(data.constData());
        for (int i = 0; i < chars; ++i) {
            const ushort u = ushort(b[i * 2]) | ushort(ushort(b[i * 2 + 1]) << 8);
            if (u == 0)
                break;
            out.append(QChar(u));
        }
        out.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        return out;
    }

    // ---------------- 本机 → 远端 ----------------

    static UINT sendCapabilities(RdpClipboard *c)
    {
        RdpClipboard::Impl *d = c->m_impl;
        CLIPRDR_GENERAL_CAPABILITY_SET general = {};
        general.capabilitySetType = CB_CAPSTYPE_GENERAL;
        general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
        general.version = CB_CAPS_VERSION_2;
        // 刻意不宣告 CB_CAN_LOCK_CLIPDATA：我们没实现 Lock/UnlockClipboardData，
        // 宣告了远端就会发锁请求而我们不应答。
        general.generalFlags = CB_USE_LONG_FORMAT_NAMES | CB_STREAM_FILECLIP_ENABLED
                               | CB_FILECLIP_NO_FILE_PATHS | CB_HUGE_FILE_SUPPORT_ENABLED;
#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
        if (d->fileContext)
            general.generalFlags |= cliprdr_file_context_current_flags(d->fileContext);
#endif

        CLIPRDR_CAPABILITIES caps = {};
        caps.common.msgType = CB_CLIP_CAPS;
        caps.cCapabilitiesSets = 1;
        // GENERAL 集合以 CLIPRDR_CAPABILITY_SET 的两个字段开头，FreeRDP 自家客户端
        // 也是这样向上转型传的。
        caps.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET *>(&general);
        return d->cliprdr->ClientCapabilities(d->cliprdr, &caps);
    }

    static UINT publishSnapshot(RdpClipboard *c, const RdpClipboardSnapshot &snap)
    {
        RdpClipboard::Impl *d = c->m_impl;
        if (!d->cliprdr)
            return CHANNEL_RC_OK;

        // 文件方向要把同一份 uri-list 喂给两个消费方：
        //   · wClipboard      —— 合成 FileGroupDescriptorW（远端要清单时用）
        //   · CliprdrFileContext —— 响应 FileContents（远端要字节时用）
        // 缺一个就是「看得见文件但拷不出内容」或反之。
        //
        // 两边是各自按同一份 uri-list 做同一趟目录遍历，所以描述符下标与远端回来
        // 要内容时用的 listIndex 天然对齐。实测一个含子目录的树，winpr 给出的名字
        // 顺序是 deep / deep\two.txt / deep\nested / deep\nested\three.txt /
        // deep\one.txt，而 FileContents 的 0..4 号返回的正是同样顺序的内容——
        // 都是文件系统 readdir 顺序，不是排序后的顺序。**别把其中一边换成自己
        // 生成的列表**：顺序一错，远端就会拿到别的文件的字节。
        QStringList rejected;
        const QStringList files = resolveLocalFiles(snap.files, &rejected);
        if (!rejected.isEmpty())
            rdpDebugLog(QStringLiteral("cliprdr: %1 个本机条目不可用，已跳过：%2")
                            .arg(rejected.size())
                            .arg(rejected.join(QStringLiteral("; "))));

        const QByteArray uriList = RdpClipboard::buildUriList(files);
        bool filesReady = false;
        QString descError;
        if (!files.isEmpty() && d->clipboard && d->fmtUriList && d->fmtFileDescriptor) {
            if (ClipboardSetData(d->clipboard, d->fmtUriList, uriList.constData(),
                                 UINT32(uriList.size()))) {
                // 合成器能否真产出描述符只有试一次才知道（winpr 对 uri-list 的
                // 期望格式没有文档保证）。探一次并记日志，出问题好定位。
                UINT32 probe = 0;
                void *desc = ClipboardGetData(d->clipboard, d->fmtFileDescriptor, &probe);
                if (desc) {
                    // 拷贝而非 fromRawData：packFileList 的「已带头」分支会原样
                    // 返回入参，零拷贝视图在下一行 free(desc) 后就悬空了。
                    const QByteArray bare(static_cast<const char *>(desc), int(probe));
                    filesReady = !RdpClipboard::packFileList(bare, &descError).isEmpty();
                    free(desc);
                } else {
                    descError = QStringLiteral("合成器未产出描述符");
                }
                rdpDebugLog(QStringLiteral("cliprdr: uri-list %1 项 [%2] → 描述符 %3 字节%4")
                                .arg(files.size())
                                .arg(QString::fromUtf8(uriList).simplified())
                                .arg(probe)
                                .arg(filesReady
                                         ? QString()
                                         : QStringLiteral("，不可用：") + descError));
            } else {
                descError = QStringLiteral("ClipboardSetData(uri-list) 失败");
                rdpDebugLog(QStringLiteral("cliprdr: ") + descError);
            }
        }
        // 有文件却公告不出去，以前是**静默**的：不往格式列表里放文件格式就完事，
        // 远端粘贴时空空如也，日志里也只有一行「描述符 0 字节」，只能怀疑远端。
        // 这类失败必须报出来。
        if (!snap.files.isEmpty() && !filesReady) {
            if (descError.isEmpty())
                descError = rejected.isEmpty() ? QStringLiteral("剪贴板通道未就绪")
                                               : QStringLiteral("本机文件不可用");
            emit c->errorOccurred(
                RdpClipboard::tr("无法把本机文件公告给远端主机：%1").arg(descError));
        }
#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
        if (d->fileContext) {
            // 用解析后的 files，不是原始 snap.files —— 内容那条腿必须和清单那条腿
            // 看到完全相同的一份 uri-list，否则下标对不上。
            const bool empty = files.isEmpty();
            if (!cliprdr_file_context_update_client_data(d->fileContext,
                                                        empty ? "" : uriList.constData(),
                                                        empty ? 0 : size_t(uriList.size())))
                rdpDebugLog(QStringLiteral("cliprdr: update_client_data 失败，"
                                           "远端将取不到文件内容"));
        }
#endif

        // 公告格式。formatName 是裸指针，缓冲区必须活到 ClientFormatList 返回。
        QByteArray descName("FileGroupDescriptorW");
        QByteArray contentsName("FileContents");
        CLIPRDR_FORMAT formats[3] = {};
        UINT32 numFormats = 0;
        if (!snap.text.isEmpty()) {
            formats[numFormats].formatId = CF_UNICODETEXT;
            formats[numFormats].formatName = nullptr;   // 标准格式无需名字
            ++numFormats;
        }
        if (filesReady) {
            formats[numFormats].formatId = d->fmtFileDescriptor;
            formats[numFormats].formatName = descName.data();
            ++numFormats;
            formats[numFormats].formatId = d->fmtFileContents;
            formats[numFormats].formatName = contentsName.data();
            ++numFormats;
        }

#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
        if (d->fileContext) {
            const UINT rc = cliprdr_file_context_notify_new_client_format_list(d->fileContext);
            if (rc != CHANNEL_RC_OK)
                return rc;
        }
#endif
        // numFormats==0 是合法的空公告（本机剪贴板里没有能同步的内容）。
        CLIPRDR_FORMAT_LIST list = {};
        list.common.msgType = CB_FORMAT_LIST;
        list.numFormats = numFormats;
        list.formats = numFormats ? formats : nullptr;
        return d->cliprdr->ClientFormatList(d->cliprdr, &list);
    }

    // 远端要粘贴：就地用快照应答，不回主线程等（见头文件线程模型）。
    static UINT serverFormatDataRequest(CliprdrClientContext *ctx,
                                        const CLIPRDR_FORMAT_DATA_REQUEST *req)
    {
        RdpClipboard *c = self(ctx);
        if (!c || !req)
            return ERROR_INTERNAL_ERROR;
        RdpClipboard::Impl *d = c->m_impl;
        QMutexLocker chan(&d->channelMutex);
        if (!d->cliprdr)
            return CHANNEL_RC_OK;

        RdpClipboardSnapshot snap;
        {
            // 锁序 channelMutex → m_mutex，不得反向（见 Impl::channelMutex）。
            QMutexLocker lock(&c->m_mutex);
            snap = d->snapshot;
        }

        if (req->requestedFormatId == CF_UNICODETEXT) {
            if (snap.text.isEmpty())
                return sendDataResponse(d->cliprdr, nullptr, 0);
            const QByteArray payload = toUnicodeText(snap.text);
            return sendDataResponse(d->cliprdr,
                                    reinterpret_cast<const BYTE *>(payload.constData()),
                                    UINT32(payload.size()));
        }

        if (d->fmtFileDescriptor && req->requestedFormatId == d->fmtFileDescriptor) {
            UINT32 size = 0;
            void *data = d->clipboard
                             ? ClipboardGetData(d->clipboard, d->fmtFileDescriptor, &size)
                             : nullptr;
            if (!data) {
                rdpDebugLog(QStringLiteral("cliprdr: FileGroupDescriptorW 合成失败"
                                           "（本机 %1 项）")
                                .arg(snap.files.size()));
                return sendDataResponse(d->cliprdr, nullptr, 0);
            }
            // winpr 给的是不带 cItems 头的裸数组，必须补头再发（见 packFileList）。
            QString err;
            const QByteArray payload = RdpClipboard::packFileList(
                QByteArray(static_cast<const char *>(data), int(size)), &err);
            free(data);
            if (payload.isEmpty()) {
                rdpDebugLog(QStringLiteral("cliprdr: 描述符不可用（%1），回 FAIL").arg(err));
                return sendDataResponse(d->cliprdr, nullptr, 0);
            }
            rdpDebugLog(QStringLiteral("cliprdr: 应答 FileGroupDescriptorW，%1 个条目 / %2 字节")
                            .arg((payload.size() - 4) / kDescriptorSize)
                            .arg(payload.size()));
            return sendDataResponse(d->cliprdr,
                                    reinterpret_cast<const BYTE *>(payload.constData()),
                                    UINT32(payload.size()));
        }

        rdpDebugLog(QStringLiteral("cliprdr: 远端请求了未公告的格式 %1，拒绝")
                        .arg(req->requestedFormatId));
        return sendDataResponse(d->cliprdr, nullptr, 0);
    }

    // ---------------- 协商 ----------------

    static UINT monitorReady(CliprdrClientContext *ctx, const CLIPRDR_MONITOR_READY *)
    {
        RdpClipboard *c = self(ctx);
        if (!c)
            return ERROR_INTERNAL_ERROR;
        QMutexLocker chan(&c->m_impl->channelMutex);
        if (!c->m_impl->cliprdr)
            return CHANNEL_RC_OK;   // 会话已拆除，这是拆通道期间漏进来的
        const UINT rc = sendCapabilities(c);
        if (rc != CHANNEL_RC_OK)
            return rc;
        c->m_impl->ready = true;

        // 通道刚就绪：把主线程已经攒下的快照立刻公告出去——用户很可能在建连
        // 之前就复制好了东西。
        RdpClipboardSnapshot snap;
        {
            QMutexLocker lock(&c->m_mutex);
            c->m_impl->announcePending = false;
            snap = c->m_impl->snapshot;
        }
        rdpDebugLog(QStringLiteral("cliprdr: MonitorReady，公告初始快照"
                                   "（文本 %1 字符 / 文件 %2 个）")
                        .arg(snap.text.size())
                        .arg(snap.files.size()));
        return publishSnapshot(c, snap);
    }

    static UINT serverCapabilities(CliprdrClientContext *ctx,
                                   const CLIPRDR_CAPABILITIES *caps)
    {
        RdpClipboard *c = self(ctx);
        if (!c || !caps)
            return ERROR_INTERNAL_ERROR;
        RdpClipboard::Impl *d = c->m_impl;
        QMutexLocker chan(&d->channelMutex);
        if (!d->cliprdr)
            return CHANNEL_RC_OK;

        // 只读第一个能力集，不按 capabilitySetLength 走完 cCapabilitiesSets 项：
        // CLIPRDR_CAPABILITIES 里没有随附缓冲区长度，而 cCapabilitiesSets 与每项的
        // capabilitySetLength 都是从线上读来的不可信值——照着它们步进就是一次无从
        // 校验的越界读。GENERAL 是 [MS-RDPECLIP] 2.2.2.1.1.1 里唯一定义的类型，
        // 也是我们唯一要的东西，够了。
        d->remoteGeneralFlags = 0;
        const auto *set = reinterpret_cast<const CLIPRDR_CAPABILITY_SET *>(caps->capabilitySets);
        if (set && caps->cCapabilitiesSets > 0
            && set->capabilitySetType == CB_CAPSTYPE_GENERAL
            && set->capabilitySetLength >= CB_CAPSTYPE_GENERAL_LEN) {
            d->remoteGeneralFlags =
                reinterpret_cast<const CLIPRDR_GENERAL_CAPABILITY_SET *>(set)->generalFlags;
        }

#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
        if (d->fileContext) {
            if (!cliprdr_file_context_remote_set_flags(d->fileContext, d->remoteGeneralFlags))
                rdpDebugLog(QStringLiteral("cliprdr: remote_set_flags 失败"));
        }
#endif
        rdpDebugLog(QStringLiteral("cliprdr: 远端能力 flags=0x%1")
                        .arg(d->remoteGeneralFlags, 0, 16));
        return CHANNEL_RC_OK;
    }

    static UINT serverFormatListResponse(CliprdrClientContext *ctx,
                                         const CLIPRDR_FORMAT_LIST_RESPONSE *rsp)
    {
        Q_UNUSED(ctx);
        if (rsp && (rsp->common.msgFlags & CB_RESPONSE_FAIL))
            rdpDebugLog(QStringLiteral("cliprdr: 远端拒绝了我们的格式公告"));
        return CHANNEL_RC_OK;
    }

    // ---------------- 远端 → 本机 ----------------

    static UINT requestFormat(RdpClipboard *c, UINT32 formatId)
    {
        RdpClipboard::Impl *d = c->m_impl;
        CLIPRDR_FORMAT_DATA_REQUEST req = {};
        req.common.msgType = CB_FORMAT_DATA_REQUEST;
        req.requestedFormatId = formatId;
        d->pendingRequestFormat = formatId;
        return d->cliprdr->ClientFormatDataRequest(d->cliprdr, &req);
    }

    static UINT serverFormatList(CliprdrClientContext *ctx, const CLIPRDR_FORMAT_LIST *list)
    {
        RdpClipboard *c = self(ctx);
        if (!c || !list)
            return ERROR_INTERNAL_ERROR;
        RdpClipboard::Impl *d = c->m_impl;
        QMutexLocker chan(&d->channelMutex);
        if (!d->cliprdr)
            return CHANNEL_RC_OK;

        // 远端换了剪贴板内容：上一轮的清单作废，正在进行的取回也不再有效
        //（listIndex 指向的是旧清单）。
        if (d->fetch.active)
            failFetch(c, RdpClipboard::tr("远端剪贴板已变化，文件取回中止"));
        {
            // 锁序 channelMutex → m_mutex，不得反向（见 Impl::channelMutex）。
            QMutexLocker lock(&c->m_mutex);
            d->remoteFiles.clear();
        }

        UINT32 descriptorId = 0;
        bool hasText = false;
        for (UINT32 i = 0; i < list->numFormats; ++i) {
            const CLIPRDR_FORMAT &fmt = list->formats[i];
            if (fmt.formatId == CF_UNICODETEXT)
                hasText = true;
            if (fmt.formatName
                && qstrcmp(fmt.formatName, "FileGroupDescriptorW") == 0)
                descriptorId = fmt.formatId;
        }

        // 先应答 FormatListResponse：cliprdr 主模块不会替我们发，不发远端会一直
        // 等，后续 FormatDataRequest 全部阻塞。
        UINT rc = sendFormatListResponse(d->cliprdr, true);
        if (rc != CHANNEL_RC_OK)
            return rc;

#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
        if (d->fileContext) {
            rc = cliprdr_file_context_notify_new_server_format_list(d->fileContext);
            if (rc != CHANNEL_RC_OK)
                return rc;
        }
#endif

        // 一次只能有一个未完成的 FormatDataRequest。文件优先：描述符只是元数据，
        // 很便宜，拿到才知道有几个文件、才能在 UI 上把「取回」亮出来。
        d->remoteDescriptorFormat = descriptorId;
        if (descriptorId)
            return requestFormat(c, descriptorId);

        emit c->remoteFilesAvailable(0);
        if (hasText)
            return requestFormat(c, CF_UNICODETEXT);
        return CHANNEL_RC_OK;
    }

    static UINT serverFormatDataResponse(CliprdrClientContext *ctx,
                                         const CLIPRDR_FORMAT_DATA_RESPONSE *rsp)
    {
        RdpClipboard *c = self(ctx);
        if (!c || !rsp)
            return ERROR_INTERNAL_ERROR;
        RdpClipboard::Impl *d = c->m_impl;
        QMutexLocker chan(&d->channelMutex);
        if (!d->cliprdr)
            return CHANNEL_RC_OK;
        const UINT32 requested = d->pendingRequestFormat;
        d->pendingRequestFormat = 0;

        if ((rsp->common.msgFlags & CB_RESPONSE_FAIL) || !rsp->requestedFormatData) {
            rdpDebugLog(QStringLiteral("cliprdr: 远端拒绝了格式 %1 的数据请求")
                            .arg(requested));
            if (requested && requested == d->remoteDescriptorFormat)
                emit c->remoteFilesAvailable(0);
            return CHANNEL_RC_OK;
        }

        const QByteArray payload(reinterpret_cast<const char *>(rsp->requestedFormatData),
                                 int(rsp->common.dataLen));

        if (requested == CF_UNICODETEXT) {
            emit c->textReceived(fromUnicodeText(payload));
            return CHANNEL_RC_OK;
        }

        if (requested && requested == d->remoteDescriptorFormat) {
            QString error;
            const QList<RdpRemoteFile> files =
                RdpClipboard::parseFileDescriptors(payload, &error);
            if (!error.isEmpty()) {
                rdpDebugLog(QStringLiteral("cliprdr: 文件清单解析失败：%1").arg(error));
                emit c->errorOccurred(
                    RdpClipboard::tr("远端文件清单无法解析：%1").arg(error));
                emit c->remoteFilesAvailable(0);
                return CHANNEL_RC_OK;
            }
#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
            // 原始清单也交给 FileContext：它内部维护 clipDataId/文件表，
            // 后续 FileContents 往返要靠它对上号。
            if (d->fileContext && d->clipboard) {
                if (!cliprdr_file_context_update_server_data(d->fileContext, d->clipboard,
                                                            payload.constData(),
                                                            size_t(payload.size())))
                    rdpDebugLog(QStringLiteral("cliprdr: update_server_data 失败"
                                               "（不影响显式取回）"));
            }
#endif
            {
                QMutexLocker lock(&c->m_mutex);
                d->remoteFiles = files;
            }
            rdpDebugLog(QStringLiteral("cliprdr: 远端公告 %1 个文件").arg(files.size()));
            emit c->remoteFilesAvailable(int(files.size()));
            return CHANNEL_RC_OK;
        }

        rdpDebugLog(QStringLiteral("cliprdr: 收到未预期的数据响应（格式 %1）")
                        .arg(requested));
        return CHANNEL_RC_OK;
    }

    // ---------------- 文件取回状态机 ----------------

    static UINT requestFileChunk(RdpClipboard *c, UINT32 dwFlags, quint64 offset, UINT32 bytes)
    {
        RdpClipboard::Impl *d = c->m_impl;
        RdpClipboard::Impl::Fetch &f = d->fetch;
        f.streamId = d->nextStreamId++;

        CLIPRDR_FILE_CONTENTS_REQUEST req = {};
        req.common.msgType = CB_FILECONTENTS_REQUEST;
        req.streamId = f.streamId;
        // 原始描述符下标，不是 files 的下标——解析时丢过条目，两者会错位。
        req.listIndex = f.files.at(f.index).listIndex;
        req.dwFlags = dwFlags;
        req.nPositionLow = UINT32(offset & 0xffffffffu);
        req.nPositionHigh = UINT32(offset >> 32);
        req.cbRequested = bytes;
        req.haveClipDataId = FALSE;
        req.clipDataId = 0;
        return d->cliprdr->ClientFileContentsRequest(d->cliprdr, &req);
    }

    static void startFetch(RdpClipboard *c, const QString &destDir)
    {
        RdpClipboard::Impl *d = c->m_impl;
        if (d->fetch.active)
            return;   // 已在取，忽略重复点击

        QList<RdpRemoteFile> files;
        {
            QMutexLocker lock(&c->m_mutex);
            files = d->remoteFiles;
        }
        if (files.isEmpty()) {
            emit c->errorOccurred(RdpClipboard::tr("远端剪贴板里没有可取回的文件"));
            return;
        }
        if (destDir.isEmpty() || !QDir(destDir).exists()) {
            emit c->errorOccurred(RdpClipboard::tr("取回目录不存在：%1").arg(destDir));
            return;
        }

        RdpClipboard::Impl::Fetch &f = d->fetch;
        f.reset();
        f.active = true;
        f.destDir = destDir;
        f.files = files;
        f.index = -1;
        // 只把已知大小的计入总量；未知的在问到 size 时再累加，进度会往上跳一下，
        // 但比显示一个假的总量诚实。
        for (const RdpRemoteFile &entry : files) {
            if (!entry.isDirectory && entry.sizeKnown)
                f.totalBytes += qint64(entry.size);
        }
        rdpDebugLog(QStringLiteral("cliprdr: 开始取回 %1 项 → %2")
                        .arg(files.size())
                        .arg(destDir));
        advanceFetch(c);
    }

    // 推进到下一个待取条目；没有了就收尾。
    static void advanceFetch(RdpClipboard *c)
    {
        RdpClipboard::Impl::Fetch &f = c->m_impl->fetch;

        for (;;) {
            if (f.file.isOpen())
                f.file.close();
            f.written = 0;
            f.currentSize = 0;
            f.awaitingSize = false;

            ++f.index;
            if (f.index >= f.files.size()) {
                finishFetch(c);
                return;
            }
            const RdpRemoteFile &entry = f.files.at(f.index);

            const QString target = resolveFetchTarget(f.destDir, entry.name);
            if (target.isEmpty()) {
                // sanitizeRemoteName 之后仍然跑出了 destDir——不该发生，
                // 真发生就是解析或过滤有洞，宁可整批中止。
                failFetch(c, RdpClipboard::tr("远端文件名不安全，已中止：%1")
                                 .arg(entry.name));
                return;
            }

            if (entry.isDirectory) {
                if (!QDir().mkpath(target)) {
                    failFetch(c, RdpClipboard::tr("无法创建目录：%1").arg(target));
                    return;
                }
                continue;   // 目录不用向远端要内容
            }

            if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
                failFetch(c, RdpClipboard::tr("无法创建目录：%1")
                                 .arg(QFileInfo(target).absolutePath()));
                return;
            }
            f.file.setFileName(target);
            if (!f.file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                failFetch(c, RdpClipboard::tr("无法写入 %1：%2")
                                 .arg(target, f.file.errorString()));
                return;
            }

            if (entry.sizeKnown) {
                f.currentSize = entry.size;
                if (f.currentSize == 0) {
                    f.file.close();
                    f.donePaths << target;
                    continue;   // 空文件：不用往返
                }
                if (requestFileChunk(c, FILECONTENTS_RANGE, 0, chunkFor(f.currentSize, 0))
                    != CHANNEL_RC_OK)
                    failFetch(c, RdpClipboard::tr("请求文件内容失败：%1").arg(entry.name));
                return;
            }

            // 描述符没带 FD_FILESIZE → 先问一次大小（响应是 8 字节小端 UINT64）。
            f.awaitingSize = true;
            if (requestFileChunk(c, FILECONTENTS_SIZE, 0, 8) != CHANNEL_RC_OK)
                failFetch(c, RdpClipboard::tr("请求文件大小失败：%1").arg(entry.name));
            return;
        }
    }

    static UINT serverFileContentsResponse(CliprdrClientContext *ctx,
                                           const CLIPRDR_FILE_CONTENTS_RESPONSE *rsp)
    {
        RdpClipboard *c = self(ctx);
        if (!c || !rsp)
            return ERROR_INTERNAL_ERROR;
        RdpClipboard::Impl *d = c->m_impl;
        QMutexLocker chan(&d->channelMutex);
        if (!d->cliprdr)
            return CHANNEL_RC_OK;
        RdpClipboard::Impl::Fetch &f = d->fetch;

        // 不是我们发的 streamId：转回 cliprdr_file_context_init 装的处理器
        //（它的 FUSE 路径也在用这个通道），别把别人的响应吃掉。
        // 转发时仍持 channelMutex：这把锁只有本文件在拿，而 FreeRDP 的处理器
        // 只会往线上写、不会回调我们，所以加锁方向恒为「我们 → FreeRDP」，不成环。
        if (!f.active || rsp->streamId != f.streamId) {
            if (d->chainedFileContentsResponse)
                return d->chainedFileContentsResponse(ctx, rsp);
            return CHANNEL_RC_OK;
        }

        const QString currentName = f.files.at(f.index).name;
        if ((rsp->common.msgFlags & CB_RESPONSE_FAIL) || !rsp->requestedData) {
            failFetch(c, RdpClipboard::tr("远端拒绝提供文件内容：%1").arg(currentName));
            return CHANNEL_RC_OK;
        }
        // 数据长度取 cbRequested，**不是** common.dataLen —— 后者是整个 PDU 载荷长度，
        // 含开头那 4 字节 streamId（[MS-RDPECLIP] 2.2.5.4）。用 dataLen 的后果不是
        // 报错而是静默损坏：每块多写 4 字节越界内容，下一块又从多算过的 written 处
        // 去要，于是每个 chunk 接缝上恰好 4 个真字节被替换、文件还整整长出 4 字节。
        // 实测 13 MB 的 zip 取回后 50 个接缝全坏，可只有跨接缝的条目 CRC 报错，
        // 小文件仅尾部多 4 字节——"看起来能打开"是这类损坏最难被发现的原因。
        const UINT32 len = rsp->cbRequested;

        if (f.awaitingSize) {
            if (len < 8) {
                failFetch(c, RdpClipboard::tr("远端返回的文件大小无效：%1").arg(currentName));
                return CHANNEL_RC_OK;
            }
            quint64 size = 0;
            for (int i = 7; i >= 0; --i)
                size = (size << 8) | rsp->requestedData[i];
            f.awaitingSize = false;
            f.currentSize = size;
            f.totalBytes += qint64(size);   // 之前未计入（描述符没带大小）
            if (size == 0) {
                const QString path = f.file.fileName();
                f.file.close();
                f.donePaths << path;
                advanceFetch(c);
                return CHANNEL_RC_OK;
            }
            if (requestFileChunk(c, FILECONTENTS_RANGE, 0, chunkFor(size, 0)) != CHANNEL_RC_OK)
                failFetch(c, RdpClipboard::tr("请求文件内容失败：%1").arg(currentName));
            return CHANNEL_RC_OK;
        }

        if (len == 0) {
            // 远端说没有更多数据，但我们还没写够声明的字节数。
            failFetch(c, RdpClipboard::tr("文件 %1 传输提前结束（%2/%3 字节）")
                             .arg(currentName)
                             .arg(f.written)
                             .arg(f.currentSize));
            return CHANNEL_RC_OK;
        }

        // 落盘长度再过一次闸门：远端多给就截掉，绝不写超过声明的大小。
        const UINT32 usable = RdpClipboard::chunkWriteLen(f.currentSize, f.written, len);
        if (usable != len)
            rdpDebugLog(QStringLiteral("cliprdr: %1 第 %2 字节起多给了 %3 字节，已截断")
                            .arg(currentName)
                            .arg(f.written)
                            .arg(len - usable));

        if (f.file.write(reinterpret_cast<const char *>(rsp->requestedData), usable)
            != qint64(usable)) {
            failFetch(c, RdpClipboard::tr("写入 %1 失败：%2")
                             .arg(f.file.fileName(), f.file.errorString()));
            return CHANNEL_RC_OK;
        }
        f.written += usable;
        f.receivedBytes += usable;
        emit c->fetchProgress(f.receivedBytes, f.totalBytes);

        if (f.written >= f.currentSize) {
            const QString path = f.file.fileName();
            f.file.close();
            // 落盘大小必须与远端声明的一致。这道闸门不是形式主义：上面 cbRequested
            // 那个坑第一次就是在这里露头的——文件比声明大 4 字节，而内容"能打开"。
            // 与其把一个看起来正常的坏文件塞进剪贴板，不如让这次取回失败。
            const qint64 onDisk = QFileInfo(path).size();
            if (onDisk != qint64(f.currentSize)) {
                failFetch(c, RdpClipboard::tr("取回 %1 后大小不符：落盘 %2 字节，远端声明 %3 字节")
                                 .arg(currentName)
                                 .arg(onDisk)
                                 .arg(f.currentSize));
                return CHANNEL_RC_OK;
            }
            f.donePaths << path;
            advanceFetch(c);
            return CHANNEL_RC_OK;
        }
        if (requestFileChunk(c, FILECONTENTS_RANGE, f.written,
                             chunkFor(f.currentSize, f.written))
            != CHANNEL_RC_OK)
            failFetch(c, RdpClipboard::tr("请求下一块数据失败：%1").arg(currentName));
        return CHANNEL_RC_OK;
    }

    static void finishFetch(RdpClipboard *c)
    {
        RdpClipboard::Impl::Fetch &f = c->m_impl->fetch;
        const QStringList paths = f.donePaths;
        f.reset();
        rdpDebugLog(QStringLiteral("cliprdr: 取回完成，%1 个文件").arg(paths.size()));
        emit c->remoteFilesFetched(paths);
    }

    static void failFetch(RdpClipboard *c, const QString &why)
    {
        RdpClipboard::Impl::Fetch &f = c->m_impl->fetch;
        // 半个文件留在盘上比不留更坑人（用户会以为传完了），删掉当前这个。
        // 已完成的保留：它们是完整的。
        if (f.file.isOpen()) {
            const QString partial = f.file.fileName();
            f.file.close();
            QFile::remove(partial);
        }
        f.reset();
        rdpDebugLog(QStringLiteral("cliprdr: 取回失败：%1").arg(why));
        emit c->errorOccurred(why);
    }

    // ---------------- 收尾 ----------------

    // abandon() 的实体。调用方必须已持 channelMutex——detach() 要在同一个临界区里
    // 先 uninit、摘回调，再走这里，中间不能让通道线程插进来；而 QMutex 不可重入，
    // 所以不能让 detach() 去调那个会自己加锁的 abandon()。
    // **绝不回写 cliprdr context**：走到这里时它可能已经被插件释放了。
    static void abandonLocked(RdpClipboard *c)
    {
        RdpClipboard::Impl *d = c->m_impl;

        if (d->fetch.active)
            failFetch(c, RdpClipboard::tr("连接已断开，文件取回中止"));

        // 先摘注册表：之后通道线程再进来查表就拿不到本对象，不会看到半清空的状态。
        if (d->cliprdr) {
            ClipboardRegistry &reg = registry();
            QMutexLocker lock(&reg.mutex);
            reg.map.remove(d->cliprdr);
        }

#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
        if (d->fileContext) {
            // 只 free 不 uninit：free 动的是 FileContext 自己的内存，而 uninit 要回写
            // cliprdr context —— 那个指针在这里未必还有效。优雅路径已在 detach()
            // 里 uninit 过了。
            cliprdr_file_context_free(d->fileContext);
            d->fileContext = nullptr;
        }
#endif
        if (d->clipboard) {
            ClipboardDestroy(d->clipboard);
            d->clipboard = nullptr;
        }

        d->cliprdr = nullptr;
        d->chainedFileContentsResponse = nullptr;
        d->ready = false;
        d->remoteDescriptorFormat = 0;
        d->pendingRequestFormat = 0;
        d->fmtUriList = 0;
        d->fmtFileDescriptor = 0;
        d->fmtFileContents = 0;

        c->resetSessionState();   // 只拿 m_mutex，锁序 channelMutex → m_mutex，不成环
    }
};

// ---------------------------------------------------------------------------
// worker 线程：通道装卸与消息泵
// ---------------------------------------------------------------------------

void RdpClipboard::attach(void *cliprdrContext)
{
    auto *cliprdr = static_cast<CliprdrClientContext *>(cliprdrContext);
    if (!cliprdr)
        return;

    QMutexLocker chan(&m_impl->channelMutex);
    if (m_impl->cliprdr == cliprdr)
        return;   // 幂等：ChannelConnected 可能重复到达

    m_impl->cliprdr = cliprdr;
    m_impl->ready = false;
    // 登记必须在装回调之前：回调一旦装上，通道线程随时可能进来查表。
    // **不写 cliprdr->custom** —— 那是 FreeRDP 文件上下文的字段，抢了必崩
    //（见 ClipboardRegistry 的说明）。
    {
        ClipboardRegistry &reg = registry();
        QMutexLocker lock(&reg.mutex);
        reg.map.insert(cliprdr, this);
    }

    m_impl->clipboard = ClipboardCreate();
    if (!m_impl->clipboard) {
        rdpDebugLog(QStringLiteral("cliprdr: ClipboardCreate 失败，剪贴板同步不可用"));
        emit errorOccurred(tr("剪贴板初始化失败，本次会话不同步剪贴板"));
        return;
    }
    m_impl->fmtUriList = ClipboardRegisterFormat(m_impl->clipboard, "text/uri-list");
    m_impl->fmtFileDescriptor =
        ClipboardRegisterFormat(m_impl->clipboard, "FileGroupDescriptorW");
    m_impl->fmtFileContents = ClipboardRegisterFormat(m_impl->clipboard, "FileContents");

#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
    m_impl->fileContext = cliprdr_file_context_new(this);
    if (m_impl->fileContext) {
        if (!cliprdr_file_context_init(m_impl->fileContext, cliprdr))
            rdpDebugLog(QStringLiteral("cliprdr: file_context_init 失败"));
        // init 会往 context 上装它自己的处理器。装我们的之前先把它占了哪些槽位
        // 记进日志（不靠假设），并存下 ServerFileContentsResponse —— 那条要串联，
        // 不能独占（见头文件）。
        rdpDebugLog(QStringLiteral("cliprdr: init 后槽位 FCReq=%1 FCRsp=%2 FmtList=%3 "
                                   "FmtDataReq=%4 local_support=%5 flags=0x%6")
                        .arg(cliprdr->ServerFileContentsRequest ? 1 : 0)
                        .arg(cliprdr->ServerFileContentsResponse ? 1 : 0)
                        .arg(cliprdr->ServerFormatList ? 1 : 0)
                        .arg(cliprdr->ServerFormatDataRequest ? 1 : 0)
                        .arg(cliprdr_file_context_has_local_support(m_impl->fileContext) ? 1 : 0)
                        .arg(cliprdr_file_context_current_flags(m_impl->fileContext), 0, 16));
        m_impl->chainedFileContentsResponse = cliprdr->ServerFileContentsResponse;
    } else {
        rdpDebugLog(QStringLiteral("cliprdr: file_context_new 失败，文件方向不可用"));
    }
#endif

    cliprdr->MonitorReady = RdpClipboardBridge::monitorReady;
    cliprdr->ServerCapabilities = RdpClipboardBridge::serverCapabilities;
    cliprdr->ServerFormatList = RdpClipboardBridge::serverFormatList;
    cliprdr->ServerFormatListResponse = RdpClipboardBridge::serverFormatListResponse;
    cliprdr->ServerFormatDataRequest = RdpClipboardBridge::serverFormatDataRequest;
    cliprdr->ServerFormatDataResponse = RdpClipboardBridge::serverFormatDataResponse;
    cliprdr->ServerFileContentsResponse = RdpClipboardBridge::serverFileContentsResponse;
    // ServerFileContentsRequest 刻意不接管：远端要本机文件内容时，由
    // cliprdr_file_context_init 装的处理器按 update_client_data 给的 uri-list
    // 直接读本地文件，我们没有能做得更好的地方。

    rdpDebugLog(QStringLiteral("cliprdr: 通道已接入"));
}

void RdpClipboard::detach(void *cliprdrContext)
{
    auto *cliprdr = static_cast<CliprdrClientContext *>(cliprdrContext);

    QMutexLocker chan(&m_impl->channelMutex);
    // 已经拆过，或者拆的是另一条（重连时新旧通道可能短暂共存）：不要动。
    if (!m_impl->cliprdr || (cliprdr && cliprdr != m_impl->cliprdr))
        return;
    cliprdr = m_impl->cliprdr;

#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
    // uninit 要拿 cliprdr context，只有在这条优雅路径上才敢调——abandon() 里
    // 通道可能已经没了。
    if (m_impl->fileContext) {
        if (!cliprdr_file_context_uninit(m_impl->fileContext, cliprdr))
            rdpDebugLog(QStringLiteral("cliprdr: file_context_uninit 失败"));
    }
#endif
    // 摘掉我们装的那几个槽位：FreeRDP 拆通道期间还可能派消息进来，留着就等于允许
    // 在会话状态已清空之后再进一次回调。
    // custom **不动** —— 那是 FreeRDP 文件上下文的字段，从来不归我们
    //（见 ClipboardRegistry 的说明）；我们的登记在 abandonLocked 里从表里摘。
    cliprdr->MonitorReady = nullptr;
    cliprdr->ServerCapabilities = nullptr;
    cliprdr->ServerFormatList = nullptr;
    cliprdr->ServerFormatListResponse = nullptr;
    cliprdr->ServerFormatDataRequest = nullptr;
    cliprdr->ServerFormatDataResponse = nullptr;
    // 这一格原本可能是 FreeRDP 的（实测 3.30 上为空），还回去而不是清零。
    cliprdr->ServerFileContentsResponse = m_impl->chainedFileContentsResponse;

    RdpClipboardBridge::abandonLocked(this);
    rdpDebugLog(QStringLiteral("cliprdr: 通道已拆除"));
}

void RdpClipboard::abandon()
{
    QMutexLocker chan(&m_impl->channelMutex);
    RdpClipboardBridge::abandonLocked(this);
}

void RdpClipboard::pump()
{
    // 这里是 worker 线程；回调在通道线程。两侧都要动通道与 fetch 状态机，靠
    // channelMutex 串起来。
    QMutexLocker chan(&m_impl->channelMutex);
    // 没协商完就先不发：请求留在标志位上，下一轮 pump 再来。
    if (!m_impl->cliprdr || !m_impl->ready)
        return;

    bool announce = false;
    RdpClipboardSnapshot snap;
    bool fetch = false;
    bool cancel = false;
    QString dest;
    {
        QMutexLocker lock(&m_mutex);
        if (m_impl->announcePending) {
            m_impl->announcePending = false;
            announce = true;
            snap = m_impl->snapshot;
        }
        if (m_impl->cancelRequested) {
            m_impl->cancelRequested = false;
            cancel = true;
        }
        if (m_impl->fetchRequested) {
            m_impl->fetchRequested = false;
            fetch = true;
            dest = m_impl->fetchDestDir;
        }
    }

    // 中止先于发起：同一轮里既要求中止又要求重新取回时（超限兜底之后用户马上
    // 点了手动取回），顺序反了会把新取回立刻掐掉。
    if (cancel && m_impl->fetch.active)
        RdpClipboardBridge::failFetch(this, tr("已停止文件取回"));

    if (announce) {
        const UINT rc = RdpClipboardBridge::publishSnapshot(this, snap);
        if (rc != CHANNEL_RC_OK)
            rdpDebugLog(QStringLiteral("cliprdr: 公告格式失败 rc=0x%1").arg(rc, 0, 16));
    }
    if (fetch)
        RdpClipboardBridge::startFetch(this, dest);
}

#endif // CUBESHELL_HAVE_FREERDP

} // namespace cubeshell
