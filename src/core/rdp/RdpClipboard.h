#pragma once

// RdpClipboard.h — RDP 剪贴板重定向（[MS-RDPECLIP] cliprdr 虚拟通道）。
//
// 支持四个方向：
//   本机文本 → 远端      公告 CF_UNICODETEXT，远端粘贴时按快照应答
//   本机文件 → 远端      公告 FileGroupDescriptorW（winpr 从 text/uri-list 合成），
//                        文件内容请求交给 FreeRDP 的 CliprdrFileContext 服务
//   远端文本 → 本机      收到远端格式公告后主动请求数据，回主线程写 QClipboard
//   远端文件 → 本机      自动取回到临时目录再塞进 QClipboard（见下方"为什么是
//                        先落地再粘贴"）
//
// --- 线程模型（本文件最重要的约束）---
// 一共三个线程碰这个对象：
//   · GUI 线程    —— setLocalSnapshot / remoteFiles / fetchRemoteFiles / resetSessionState
//   · worker 线程 —— RdpClient 的事件循环：attach / detach / abandon / pump
//   · 通道线程    —— cliprdr 的 C 回调。它们跑在通道自己的线程上
//                    （channel_client_thread_proc → cliprdr_order_recv → 回调），
//                    **不是** worker 线程。
// 后两者同时读写同一份通道指针与取回状态机，所以 .cpp 里用 Impl::channelMutex
// 把它们串起来；锁序固定 channelMutex → m_mutex，GUI 入口只拿 m_mutex，不成环。
//
// QClipboard 只能在 GUI 线程访问，所以回调里**一律不碰 Qt 剪贴板**，两个方向
// 各走一条单向通道：
//   · 本机 → 远端：主线程把剪贴板内容抓成快照（setLocalSnapshot），加锁存下；
//     远端来要数据时通道线程就地用快照应答，不回主线程等。公告发生在"复制"那
//     一刻而非"粘贴"那一刻，所以回调可以同步返回。
//   · 远端 → 本机：回调里发信号，Qt 自动队列到主线程。
// 主线程发起的动作（公告、取回文件）不直接调 FreeRDP，而是排进队列由 worker
// 循环里的 pump() 派发——同 RdpClient 的 m_inputQueue/flushInputQueue 做法，
// 避免两个线程同时往通道里写。
//
// --- 为什么远端→本机文件是"先落地再粘贴"而不是透明粘贴 ---
// FreeRDP 那条透明路径依赖 FUSE（cliprdr_file_context_has_local_support()），
// macOS/Windows 上都没有。要做到"远端复制、本机 Finder 里直接粘贴"得实现
// macOS 的 NSFilePromiseProvider / Windows 的 IDataObject，是另一个量级的工作。
// 这里的做法是：远端复制文件后先取回文件**清单**（元数据，很便宜），随即自动把
// 内容传回临时目录，再把本地真实路径塞进 QClipboard——之后在 Finder 里粘贴就是
// 真文件。对用户而言就是"远端复制、本机粘贴"，与反方向一致。
//
// 自动传回但有体积上限（RdpPanel::kAutoFetchLimitBytes）：远端可能复制了几个 GB，
// 静默拉取是灾难。描述符里带 FD_FILESIZE，所以**传之前**就能算出总量并决定；
// 超限时退回让用户点按钮确认。清单里没带大小的条目（目录）算不出总量，由传输
// 中的累计闸门兜底。上限只是自动化的边界，不是能力的边界——手动取回不受限。
//
// 仅在 CUBESHELL_WITH_RDP=ON 时编译。FreeRDP 相关状态全藏在 Impl 里（pimpl），
// 让类布局与 CUBESHELL_HAVE_FREERDP 无关——那个宏是 cube_core 的 PRIVATE 定义，
// 单测编译单元里看不到，若成员随它增减就会踩 ODR。

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

namespace cubeshell {

// 本机剪贴板快照：主线程抓，worker 线程读。
struct RdpClipboardSnapshot {
    QString text;        // 纯文本；空 = 本机剪贴板里没文本
    QStringList files;   // 本地文件/目录绝对路径；空 = 没文件
    bool isEmpty() const { return text.isEmpty() && files.isEmpty(); }
};

// 远端剪贴板里的一个文件条目（从 FILEDESCRIPTORW 解析出来）。
struct RdpRemoteFile {
    QString name;              // 已过滤的相对路径，分隔符统一为 '/'
    quint64 size = 0;          // 描述符没带 FD_FILESIZE 时为 0，取回时再问远端
    bool sizeKnown = false;
    bool isDirectory = false;

    // 该条目在远端原始描述符数组里的下标。**必须**用它做 FileContents 请求的
    // listIndex：解析时会丢掉文件名非法的条目，丢一个后面全体错位，拿列表下标
    // 去请求就会取到别的文件。
    quint32 listIndex = 0;
};

class RdpClipboard : public QObject {
    Q_OBJECT
public:
    explicit RdpClipboard(QObject *parent = nullptr);
    ~RdpClipboard() override;

    // ---------------- 主线程 ----------------

    // 本机剪贴板变了：存快照并请 worker 向远端公告可用格式。
    void setLocalSnapshot(const RdpClipboardSnapshot &snapshot);

    // 远端最近一次公告的文件清单（可能为空）。
    QList<RdpRemoteFile> remoteFiles() const;

    // 把远端剪贴板里的文件取回到 destDir，完成/失败经 remoteFilesFetched /
    // errorOccurred 回报。destDir 必须已存在。
    void fetchRemoteFiles(const QString &destDir);

    // 中止正在进行的取回（结果经 errorOccurred 报"已停止"）。自动取回的兜底
    // 闸门用的：清单里没带 FD_FILESIZE 的条目事前算不出总量，只能在传输中盯着。
    void cancelFetch();

    // 通道断开或会话重连时清空状态（快照保留——那是本机的东西，与会话无关）。
    void resetSessionState();

    // ---------------- worker 线程（由 RdpClient 调用）----------------

    // 通道就绪/拆除。参数是 CliprdrClientContext*，用 void* 是为了不让 FreeRDP
    // 类型泄进头文件（见文件头 pimpl 说明）。
    // detach 必须在 ChannelDisconnected 事件里调——那是插件释放自己的 context
    // 之前最后一个还能安全回写它的时机。
    void attach(void *cliprdrContext);
    void detach(void *cliprdrContext);

    // 会话收尾兜底：只释放本对象持有的东西并清空指针，**绝不回写 cliprdr
    // context**（走到这里时它可能已经被插件释放了）。detach 走过之后是空操作。
    // 重连是热路径，漏掉这一步就会带着旧 context 进下一个会话。
    void abandon();

    // 每轮事件循环调一次：派发主线程排进来的公告/取回请求。
    void pump();

    // ---------------- 纯函数（无状态，单测直接调）----------------

    // 解析 [MS-RDPECLIP] 2.2.5.2.3 Packed File List：
    // 4 字节 cItems + cItems × 592 字节 FILEDESCRIPTORW。
    // 远端是不可信输入：字节数与 cItems 不符、文件名非法的条目一律丢弃，
    // 解析失败返回空列表并（若给了 error）写明原因。
    static QList<RdpRemoteFile> parseFileDescriptors(const QByteArray &data,
                                                     QString *error = nullptr);

    // 远端文件名 → 可安全落盘的相对路径；判定不安全时返回空串。
    // 这是唯一一条会按远端给的名字往本地磁盘写文件的路径，必须挡住
    // 路径穿越（..）、绝对路径、盘符、控制字符。
    static QString sanitizeRemoteName(const QString &raw);

    // 本地路径列表 → text/uri-list。winpr 的剪贴板合成器吃这个格式，
    // 再由它产出 FileGroupDescriptorW（省掉手写 packed file list 序列化）。
    static QByteArray buildUriList(const QStringList &paths);

    // winpr 合成出的裸描述符数组 → 合法的 Packed File List（补 4 字节 cItems 头）。
    // winpr 的 ClipboardGetData(FileGroupDescriptorW) **不带**协议要求的那个头
    //（FreeRDP 3.30.0 实测：2 个文件 = 1184 = 2×592 字节），原样发出去远端会把第一条
    // 记录的 flags 当成 cItems 去解析。已带头（size%592==4）时核对自洽后原样返回。
    // 字节数不成整条记录则返回空并写明原因——宁可回 FAIL，也不发畸形清单。
    static QByteArray packFileList(const QByteArray &descriptors, QString *error = nullptr);

    // 一块 FileContents 响应里**允许落盘**的字节数：不超过声明大小的剩余量。
    // 摘成纯函数是因为这条算术错过一次：取长度时用了 common.dataLen（含 4 字节
    // streamId 的整个 PDU 长度）而不是 cbRequested，于是每块多写 4 字节、下一块
    // 又从多算过的偏移去要，每个 256 KiB 接缝上恰好 4 个真字节被替换成越界内容。
    // 13 MB 的 zip 实测 50 个接缝全坏，但只有跨接缝的条目报 CRC 错、小文件只是尾部
    // 多 4 字节——"看起来能用"正是这种损坏最难被发现的原因。详见 .cpp 的响应回调。
    static quint32 chunkWriteLen(quint64 declaredSize, quint64 written, quint32 respLen);

signals:
    // 远端 → 本机文本，已在主线程。
    void textReceived(const QString &text);
    // 远端公告了 count 个文件（还没传内容）。count==0 表示远端剪贴板已无文件。
    void remoteFilesAvailable(int count);
    // 取回完成，localPaths 是落地后的本地绝对路径。
    void remoteFilesFetched(const QStringList &localPaths);
    // 取回进度（已完成字节 / 总字节；总字节未知时 total 为 0）。
    void fetchProgress(qint64 received, qint64 total);
    void errorOccurred(const QString &message);

private:
    // cliprdr 的 C 回调要读写 Impl 并发信号，全部集中在 .cpp 里的 Bridge 上
    // （它们跑在通道线程，见文件头线程模型）。用 friend 而不是把 FreeRDP 类型
    // 搬进头文件。
    friend struct RdpClipboardBridge;

    struct Impl;
    Impl *m_impl = nullptr;   // 永不为空；FreeRDP 缺席时里面只是空壳

    mutable QMutex m_mutex;   // 保护快照与跨线程请求标志
};

} // namespace cubeshell
