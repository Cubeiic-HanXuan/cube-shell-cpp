// RdpClient.cpp — RDP 客户端实现（FreeRDP 库 / 命令行双后端）。
// 对应Python: core/rdp/rdp_client.py

#include "RdpClient.h"

#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>

// winsock2.h 必须早于任何 windows.h（FreeRDP/WinPR 头会间接引入），
// 否则 winsock.h 先被拉进来会与 winsock2 的宏/结构体定义冲突。
#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <windows.h>
// 异常处理器里只能用 C 风格 IO（见下），用 <stdio.h> 而非 <cstdio> 以确保
// fopen/fprintf 落在全局命名空间。
#  include <stdio.h>
#endif

#ifdef CUBESHELL_HAVE_FREERDP
#include <freerdp/error.h>
#include <freerdp/freerdp.h>
#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
#include <freerdp/client.h>
#endif
#include <freerdp/gdi/gdi.h>
#include <freerdp/version.h>
#include <winpr/synch.h>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>
#include <QTextStream>
#include <QThread>
#include <exception>
#endif

namespace cubeshell {

// Windows 下 Winsock 的一次性初始化。FreeRDP 自身不调用 WSAStartup，
// 未初始化时其 getaddrinfo() 会直接失败，连数字 IP 也会报
// ERRCONNECT_DNS_NAME_NOT_FOUND。对齐 SshClient::ensureLibssh2Init 的做法。
static void ensureFreeRDPInit()
{
    static bool inited = false;
    if (!inited) {
#ifdef Q_OS_WIN
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        inited = true;
    }
}

#ifdef CUBESHELL_HAVE_FREERDP
// 崩溃落盘日志：每个关键点写一行并立即 flush，崩溃后日志最后一行即为最后
// 执行到的位置。进程在 FreeRDP/OpenSSL 库内以 SEH（0xc0000005）死掉时，
// C++ catch 与 Qt 信号都来不及反应，这份日志 + 下面的向量化异常处理器（VEH）
// 是定位现场的唯一手段（曾靠它定位到 MSVC ARM64 误编译 OpenSSL 的崩溃，
// 见 vcpkg-ports/openssl/windows/portfile.cmake 的说明）。
// 日志路径集中一处算好：Windows 的向量化异常处理器里不能构造 QString/QFile
// （异常上下文中走 Qt 分配器不安全），需要同一份路径的纯 C 串副本。
static const QString &rdpLogPath()
{
    static const QString path = [] {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        // 目录不存在时 QFile::open 会静默失败，表现为“没有日志”而非崩溃信息，先建目录。
        if (!dir.isEmpty() && QDir().mkpath(dir))
            return dir + QStringLiteral("/rdp_debug.log");
        return QDir::temp().filePath(QStringLiteral("rdp_debug.log"));
    }();
    return path;
}

#ifdef Q_OS_WIN
// 崩溃现场标记：每次 rdpLog 写日志时同步更新这个纯 C 串副本（异常上下文中不能
// 构造 QString）。VEH 触发时把它一并落盘，日志里“崩溃码 + 最后执行到的位置”
// 配对出现，一眼定位故障点。异常处理器只读它，不写。
// 声明须在 rdpLog 之前（rdpLog 内会写它）。
static char g_rdpLastStep[160] = {};
#endif

static void rdpLog(const QString &msg)
{
#ifdef Q_OS_WIN
    // 同步更新崩溃现场标记（纯 C 串，VEH 里安全可读）。只取消息本体，不含时间戳。
    const QByteArray local = msg.toLocal8Bit();
    qstrncpy(g_rdpLastStep, local.constData(), sizeof(g_rdpLastStep));
#endif
    QFile f(rdpLogPath());
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
           << " | " << msg << "\n";
        ts.flush();
    }
}

#ifdef Q_OS_WIN
// 0xc0000005 是 SEH 异常，C++ 的 catch(...) 抓不到；__try/__except 又不允许与
// 带析构的局部对象同处一个函数（runConnection 里满是 QString/QByteArray）。
// 故改挂向量化异常处理器：它在异常分发的最早阶段被调用，返回
// EXCEPTION_CONTINUE_SEARCH 不改变原有处理流程，只把异常码与出错地址落盘，
// 崩溃后即可从日志读到确切的故障指令地址。
// 处理器运行在异常上下文中，不可使用 Qt 容器与堆分配，全程 C 风格 IO。
// 注意 VEH 是进程级且先于任何 handler 收到“首次机会”异常，日志里可能混入
// 被正常捕获处理的异常（如 C++ 抛出的 0xE06D7363），只有崩溃后紧跟进程消失
// 的那条才是真正的现场。
// 覆盖范围：从 freerdp_connect 之前一直挂到会话结束（事件循环 + 断连 + 上下文
// 释放）之后。静默退出的崩溃恰恰发生在连接成功后的会话期——那里是 C++ catch
// 抓不到的 SEH 区域，也是此前唯一没有防护罩覆盖的代码路径，故必须全程安装。
static char g_rdpLogPathAnsi[MAX_PATH] = {};

static LONG WINAPI rdpVectoredHandler(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord && g_rdpLogPathAnsi[0]) {
        FILE *f = fopen(g_rdpLogPathAnsi, "a");
        if (f) {
            fprintf(f, "!!! VECTORED EXCEPTION: code=0x%08lX addr=%p last=\"%s\"\n",
                    static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                    ep->ExceptionRecord->ExceptionAddress,
                    g_rdpLastStep);
            fflush(f);
            fclose(f);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// 处理器安装：路径转换必须在安装之前、异常之外完成（见上）。
static void *rdpInstallVectoredHandler()
{
    const QByteArray local = rdpLogPath().toLocal8Bit();
    qstrncpy(g_rdpLogPathAnsi, local.constData(), sizeof(g_rdpLogPathAnsi));
    return AddVectoredExceptionHandler(1, &rdpVectoredHandler);
}

// 处理器拆除：与安装成对，会话彻底结束（上下文已释放）后调用。
static void rdpRemoveVectoredHandler(void *veh)
{
    if (veh)
        RemoveVectoredExceptionHandler(veh);
}
#endif // Q_OS_WIN
#endif // CUBESHELL_HAVE_FREERDP

// ---------------------------------------------------------------------------
// normalizeRdpHost
// ---------------------------------------------------------------------------
// 设备表/URL 分发路径下的 host 不经表单回读（main_window 直接
// client->connectToHost(resolved)），存盘值里的空白、内联端口、整条
// rdp:// URL 都会原样落到 getaddrinfo()上，此处统一洗干净。
RdpHostPort normalizeRdpHost(const QString &rawHost, int defaultPort)
{
    RdpHostPort out;
    out.port = defaultPort;

    QString s = rawHost.trimmed();
    // 整条 URL 被粘进主机框：剥 rdp:// / rdp+ntlm-password:// 前缀
    const int schemeEnd = s.indexOf(QLatin1String("://"));
    if (schemeEnd > 0)
        s = s.mid(schemeEnd + 3);
    // 剥 userinfo（user[:pwd]@）；IPv6 字面量本身不含 '@'，故可直接切
    const int at = s.lastIndexOf(QLatin1Char('@'));
    if (at >= 0)
        s = s.mid(at + 1);
    // 剥路径/参数尾巴
    const int slash = s.indexOf(QLatin1Char('/'));
    if (slash >= 0)
        s = s.left(slash);
    // 内嵌空白与控制字符（复制粘贴常带 \n / \t / U+00A0）全部剔除
    QString cleaned;
    cleaned.reserve(s.size());
    for (const QChar c : s) {
        if (!c.isSpace() && c.category() != QChar::Other_Control)
            cleaned.append(c);
    }
    s = cleaned;

    // 内联端口优先（与 SSH 侧 parseHostPort 一致：主机串写了端口就听它）
    const auto takePort = [&out](const QString &text) {
        bool ok = false;
        const int p = text.toInt(&ok);
        if (ok && p > 0 && p <= 65535)
            out.port = p;
    };

    if (s.startsWith(QLatin1Char('['))) {           // [IPv6][:port]
        const int end = s.indexOf(QLatin1Char(']'));
        if (end > 0) {
            out.host = s.mid(1, end - 1);
            const QString rest = s.mid(end + 1);
            if (rest.startsWith(QLatin1Char(':')))
                takePort(rest.mid(1));
            return out;
        }
    }
    if (s.count(QLatin1Char(':')) == 1) {           // host:port
        const int idx = s.indexOf(QLatin1Char(':'));
        out.host = s.left(idx);
        takePort(s.mid(idx + 1));
        return out;
    }
    out.host = s;                                   // 裸主机名 / 裸 IPv6
    return out;
}

// ---------------------------------------------------------------------------
// buildRdpUrl —— 已删除
// ---------------------------------------------------------------------------
// 原实现把明文密码 percent-encode 进 rdp:// URL（唯一调用点是 macOS `open`
// 兜底），属不安全的死代码，随 RDP 命令行密码泄露修复一并移除。
// 对应Python: core/rdp/rdp_client.py::build_rdp_url（同样不应再使用）

// ---------------------------------------------------------------------------
// FreeRDP 后端（find_package(FreeRDP) 命中时编译）
// ---------------------------------------------------------------------------
#ifdef CUBESHELL_HAVE_FREERDP

// 连接管理 worker：阻塞式 FreeRDP 事件循环跑在独立线程，帧经 Qt 信号
// 回主线程（自动 QueuedConnection）。
// 对应Python: RDPInterfaceThread（asyncio 事件循环线程 + result 信号）
class RdpClient::FreeRdpWorker : public QThread {
public:
    FreeRdpWorker(RdpClient *client, const RdpSettings &settings)
        : QThread(client)
        , m_client(client)
        , m_settings(settings)
    {
    }

    // 对应Python: RDPInterfaceThread.stop（终止连接并退出事件循环）
    void requestStop()
    {
        requestInterruption();
        if (m_instance) {
#if FREERDP_VERSION_MAJOR >= 3
            freerdp_abort_connect_context(m_instance->context);
#else
            freerdp_abort_connect(m_instance);
#endif
        }
    }

    // 输入事件的队列传输载体（tagged union 风格；跨线程只做值拷贝）。
    // 对应Python: in_q 中流转的 RDP_KEYBOARD_SCANCODE / RDP_KEYBOARD_UNICODE /
    // RDP_MOUSE / RDP_CLIPBOARD_DATA_TXT 数据对象
    struct InputEvent {
        enum class Type { KeyScancode, KeyUnicode, Mouse, ClipboardText };
        Type type = Type::KeyScancode;
        RdpKeyEvent key;                // Type::KeyScancode
        RdpUnicodeKeyEvent unicodeKey;  // Type::KeyUnicode
        RdpMouseEvent mouse;            // Type::Mouse
        QString clipboardText;          // Type::ClipboardText
    };

    // 主线程入队（QMutex 保护），Worker 线程在事件循环中 poll 出队。
    // 对应Python: in_q.put(...)（GUI 线程 → inputhandler 线程的队列写入）
    void enqueueInput(const InputEvent &event)
    {
        QMutexLocker locker(&m_inputMutex);
        m_inputQueue.enqueue(event);
    }

    // 断连前清空未派发事件，避免 requestStop 后残留输入打到已关闭连接。
    void clearInputQueue()
    {
        QMutexLocker locker(&m_inputMutex);
        m_inputQueue.clear();
    }

protected:
    // FreeRDP 的回调由库代码在本线程内调用，C++ 异常一旦穿过中间的 C 栈帧
    // 逃出 run()，Qt 会直接 terminate 整个进程，UI 侧连一个错误信号都收不到。
    // 此处兜住并转成 errorOccurred + disconnected，让面板能正常收尾。
    // 注意：这只覆盖 C++ 异常；会话期 FreeRDP 库内的访问冲突是 SEH（0xc0000005），
    // catch 抓不到，须靠 runConnection 里全程挂的向量化异常处理器（VEH）落盘定位。
    void run() override
    {
        try {
            runConnection();
        } catch (const std::exception &e) {
            rdpLog(QStringLiteral("EXCEPTION: %1").arg(QString::fromLocal8Bit(e.what())));
            emit m_client->errorOccurred(QStringLiteral("RDP 内部异常：%1")
                                             .arg(QString::fromLocal8Bit(e.what())));
            emit m_client->disconnected();
        } catch (...) {
            rdpLog(QStringLiteral("UNKNOWN EXCEPTION"));
            emit m_client->errorOccurred(QStringLiteral("RDP 未知内部异常"));
            emit m_client->disconnected();
        }
    }

private:
    void runConnection()
    {
        rdpLog(QStringLiteral("runConnection: start"));
#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
        // 通道表、默认 settings、监视器数组等内部状态在 FreeRDP 3 里由高层客户端
        // API 建立：裸 freerdp_new() + freerdp_context_new() 只得到半成品上下文，
        // PreConnect 返回后核心紧接着做 monitor 校验与 utils_reload_channels()，
        // 会崩在库内（0xc0000005）。故优先走 freerdp_client_context_new()，它还会
        // 把 instance->LoadChannels 挂成 freerdp_client_load_channels。
        RDP_CLIENT_ENTRY_POINTS entryPoints = {};
        entryPoints.Size = sizeof(RDP_CLIENT_ENTRY_POINTS);
        entryPoints.Version = RDP_CLIENT_INTERFACE_VERSION;
        entryPoints.ContextSize = sizeof(WorkerContext);
        // GlobalInit/GlobalUninit 与 ClientNew/ClientFree 库内均以 IFCALL 调用，
        // 本客户端没有额外的全局/实例级初始化需求，留空即可。
        rdpContext *context = freerdp_client_context_new(&entryPoints);
        if (!context) {
            rdpLog(QStringLiteral("runConnection: freerdp_client_context_new FAILED"));
            emit m_client->errorOccurred(QStringLiteral("freerdp_client_context_new 失败"));
            return;
        }
        rdpLog(QStringLiteral("runConnection: freerdp_client_context_new OK"));
        freerdp *instance = context->instance;
#else
        // 构建里没有 freerdp-client（无高层 API 可链接）时退回低层建栈路径。
        freerdp *instance = freerdp_new();
        if (!instance) {
            rdpLog(QStringLiteral("runConnection: freerdp_new FAILED"));
            emit m_client->errorOccurred(QStringLiteral("freerdp_new 失败"));
            return;
        }
        rdpLog(QStringLiteral("runConnection: freerdp_new OK"));
        instance->ContextSize = sizeof(WorkerContext);
        if (!freerdp_context_new(instance)) {
            rdpLog(QStringLiteral("runConnection: freerdp_context_new FAILED"));
            freerdp_free(instance);
            emit m_client->errorOccurred(QStringLiteral("freerdp_context_new 失败"));
            return;
        }
        rdpLog(QStringLiteral("runConnection: context_new OK"));
#endif
        // 回调一律在上下文建好之后挂：高层 API 在 ContextNew 阶段会写入
        // client_cli_* 系列默认回调（命令行交互式凭据/证书确认），先挂会被覆盖。
        instance->PreConnect = &FreeRdpWorker::preConnect;
        instance->PostConnect = &FreeRdpWorker::postConnect;
        instance->PostDisconnect = &FreeRdpWorker::postDisconnect;
        // 凭据与证书回调必挂：NLA/TLS 协商阶段库内经 IFCALLRET 直接调用，
        // 缺一个就会在 freerdp_connect() 内部解空指针（0xc0000005）。
#if FREERDP_VERSION_MAJOR >= 3
        instance->AuthenticateEx = &FreeRdpWorker::authenticateEx;
#else
        instance->Authenticate = &FreeRdpWorker::authenticate;
#endif
        instance->VerifyCertificateEx = &FreeRdpWorker::verifyCertificateEx;
        instance->VerifyChangedCertificateEx = &FreeRdpWorker::verifyChangedCertificateEx;

        WorkerContext *wctx = reinterpret_cast<WorkerContext *>(instance->context);
        wctx->worker = this;
        m_instance = instance;

        // 连接参数（对应 Python 侧 build_rdp_url 携带的 host/port/user/pwd/domain
        // 与 iosettings 的分辨率）
        rdpSettings *settings = instance->context->settings;
        // ServerHostname 会被原样交给 getaddrinfo()：先归一化，再校验非空，
        // 否则失败只会表现为一个含义误导的 DNS_NAME_NOT_FOUND 错误码。
        const RdpHostPort target = normalizeRdpHost(m_settings.host, m_settings.port);
        if (target.host.isEmpty()) {
            rdpLog(QStringLiteral("runConnection: empty host, abort"));
            emit m_client->errorOccurred(
                QStringLiteral("RDP 主机地址为空（原始值：\"%1\"）").arg(m_settings.host));
            releaseInstance(instance);
            m_instance = nullptr;
            return;
        }
        const QByteArray hostUtf8 = target.host.toUtf8();
        freerdp_settings_set_string(settings, FreeRDP_ServerHostname,
                                    hostUtf8.constData());
        freerdp_settings_set_uint32(settings, FreeRDP_ServerPort,
                                    static_cast<quint32>(target.port));
        if (!m_settings.username.isEmpty())
            freerdp_settings_set_string(settings, FreeRDP_Username,
                                        m_settings.username.toUtf8().constData());
        if (!m_settings.password.isEmpty())
            freerdp_settings_set_string(settings, FreeRDP_Password,
                                        m_settings.password.toUtf8().constData());
        if (!m_settings.domain.isEmpty())
            freerdp_settings_set_string(settings, FreeRDP_Domain,
                                        m_settings.domain.toUtf8().constData());
        // 分辨率：0/负数或越界值同样会被上述 monitor 校验拿去构造监视器定义，
        // 故夹到协议允许区间并对齐（宽 4 的倍数、高 2 的倍数，与
        // RdpPanel::alignResolution 一致），再显式写入。
        int width = m_settings.width > 0 ? m_settings.width : 1920;
        int height = m_settings.height > 0 ? m_settings.height : 1080;
        width = qBound(200, width, 8192) & ~3;
        height = qBound(200, height, 8192) & ~1;
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
                                    static_cast<quint32>(width));
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
                                    static_cast<quint32>(height));
        // 32bpp 与下面 gdi_init 的 PIXEL_FORMAT_BGRX32 对齐
        freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
        // 监视器：单屏、非全屏，且监视器表自己预填好，不把分配交给库。
        // PreConnect 返回后核心依次跑 freerdp_settings_enforce_monitor_exists()
        // 与 freerdp_settings_check_client_after_preconnect()，后者按 MonitorCount
        // 逐条取 MonitorDefArray[i] 直接解引用（越界时 get_pointer_array 返回
        // nullptr，库内无判空），故这里让 MonitorCount 与数组长度严格一致：
        // set_pointer_len 的 len 是元素个数（data 传 nullptr 即按零值分配），
        // set_pointer_array 受 MonitorDefArraySize 边界检查，写不进去时返回 FALSE。
        freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_Fullscreen, FALSE);
        rdpMonitor monitor = {};
        monitor.x = 0;
        monitor.y = 0;
        monitor.width = width;
        monitor.height = height;
        monitor.is_primary = TRUE;
        monitor.orig_screen = 0;
        // 缩放因子写死 100（[MS-RDPBCGR] 要求 desktopScaleFactor ∈ [100,500]），
        // 与本客户端不做 HiDPI 缩放的 GDI 渲染路径一致。
        monitor.attributes.desktopScaleFactor = 100;
        monitor.attributes.deviceScaleFactor = 100;
        const BOOL monitorReady =
            freerdp_settings_set_pointer_len(settings, FreeRDP_MonitorDefArray, nullptr, 1)
            && freerdp_settings_set_pointer_array(settings, FreeRDP_MonitorDefArray, 0, &monitor);
        // 预填失败就退回 0：库在非全屏非多屏下仍会按桌面尺寸重建单监视器，
        // 别留下 count=1 却没有对应条目的不一致状态。
        freerdp_settings_set_uint32(settings, FreeRDP_MonitorCount, monitorReady ? 1 : 0);
        rdpLog(QStringLiteral("runConnection: monitor configured: %1x%2 (%3)")
                   .arg(width)
                   .arg(height)
                   .arg(monitorReady ? QStringLiteral("MonitorCount=1")
                                     : QStringLiteral("prefill FAILED, MonitorCount=0")));
        // 图形管线（EGFX/H.264）显式关掉：本客户端只挂了 GDI 的 Begin/EndPaint，
        // 未初始化 gfx 编解码上下文，通道一旦协商成功，后续按空上下文取用即崩。
        freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE);
        // 安全层协商：NLA(NTLM)/TLS/RDP 三层全开由服务端挑选，对应 Python 侧
        // build_rdp_url 的 rdp+ntlm-password（NLA）语义。三者与 Authentication
        // 同为 FreeRDP 3 的默认值，显式写出以免换库版本/发行版后默认值漂移。
        // Windows ARM64 特别注意：NLA/TLS 走 OpenSSL，MSVC ARM64 Release 会误编译
        // OpenSSL（上游 #26239/#27030），握手期栈/寄存器被破坏后崩在 libssl 内
        // （0xC0000005，曾定位于 tls_parse_all_extensions）。修复在构建侧：
        // vcpkg-triplets/ 的 overlay triplet 加 NO_INTERLOCKEDOR64，
        // vcpkg-ports/openssl 的 overlay port 把 Release 优化降为 /Od。
        // 此前“TLS 上限压 1.2 可绕过”的诊断是错误的（该服务器本就只协商
        // TLS 1.2，崩溃依旧），故不再限制协议版本。
        freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_Authentication, TRUE);
        // 嵌入式面板渲染：软件 GDI，忽略自签证书（与 Python 侧堡垒机场景一致）
        freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE);

        rdpLog(QStringLiteral("runConnection: settings configured w=%1 h=%2, "
                              "about to call freerdp_connect NOW")
                   .arg(width)
                   .arg(height));
#ifdef Q_OS_WIN
        // 从建连前一直挂到会话结束（事件循环 + 断连 + 上下文释放）之后才拆除。
        // 静默退出正是发生在连接成功后的会话期，若像原来那样 connected 一发就拆，
        // 那段 SEH 崩溃就完全无人记录、进程直接消失。
        void *veh = rdpInstallVectoredHandler();
#endif
        const BOOL connected = freerdp_connect(instance);
        rdpLog(QStringLiteral("runConnection: freerdp_connect returned %1")
                   .arg(connected ? QStringLiteral("TRUE") : QStringLiteral("FALSE")));
        if (!connected) {
            emit m_client->errorOccurred(connectErrorMessage(instance, target));
            releaseInstance(instance);
            m_instance = nullptr;
#ifdef Q_OS_WIN
            rdpRemoveVectoredHandler(veh);
#endif
            return;
        }

        emit m_client->connected();

        // 事件循环。对应Python: rdpconnection() 的 ext_out_queue 消费循环 +
        // inputhandler 线程的 in_q 消费（lines 420-426），C++ 侧合并为单循环：
        // 100ms 超时唤醒后 poll 输入队列并调用 FreeRDP input API 派发。
        rdpLog(QStringLiteral("runConnection: entering event loop"));
        HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
        int loopCount = 0;
        while (!isInterruptionRequested()) {
            const DWORD count = freerdp_get_event_handles(instance->context, handles,
                                                          MAXIMUM_WAIT_OBJECTS);
            // 只记前 10 轮，避免日志无限膨胀
            if (loopCount < 10) {
                rdpLog(QStringLiteral("runConnection: loop iteration %1, handle count=%2")
                           .arg(loopCount)
                           .arg(count));
                loopCount++;
            }
            // 句柄集合可能在建连收尾/通道重建的间隙短暂为空，此时 break 会让
            // 循环在连接仍活着时提前退出，随后 freerdp_disconnect() 打在半成品
            // 状态上直接崩在库内。改为短睡重试。
            if (count == 0) {
                QThread::msleep(10);
                continue;
            }
            const DWORD status = WaitForMultipleObjects(count, handles, FALSE, 100);
            if (status == WAIT_FAILED) {
                rdpLog(QStringLiteral("runConnection: WaitForMultipleObjects WAIT_FAILED"));
                break;
            }
            // 对应Python: loop.call_soon_threadsafe(conn.ext_in_queue.put_nowait, data)
            flushInputQueue(instance);
            if (!freerdp_check_event_handles(instance->context)) {
                rdpLog(QStringLiteral("runConnection: freerdp_check_event_handles FALSE"));
                break;
            }
#if FREERDP_VERSION_MAJOR >= 3
            if (freerdp_shall_disconnect_context(instance->context)) {
                rdpLog(QStringLiteral("runConnection: shall_disconnect TRUE"));
                break;
            }
#else
            if (freerdp_shall_disconnect(instance)) {
                rdpLog(QStringLiteral("runConnection: shall_disconnect TRUE"));
                break;
            }
#endif
        }

        rdpLog(QStringLiteral("runConnection: event loop exited"));

        // 循环退出可能源于服务端断开/协议错误，先取出错误码报给 UI，再断连。
        const UINT32 lastError = freerdp_get_last_error(instance->context);
        if (lastError != FREERDP_ERROR_SUCCESS) {
            const char *errName = freerdp_get_last_error_name(lastError);
            rdpLog(QStringLiteral("runConnection: lastError=0x%1 (%2)")
                       .arg(lastError, 8, 16, QLatin1Char('0'))
                       .arg(QString::fromUtf8(errName ? errName : "UNKNOWN")));
            emit m_client->errorOccurred(
                QStringLiteral("RDP 连接断开: %1 (0x%2)")
                    .arg(QString::fromUtf8(errName ? errName : "UNKNOWN"))
                    .arg(lastError, 8, 16, QLatin1Char('0')));
        }

        rdpLog(QStringLiteral("runConnection: calling freerdp_disconnect"));
        freerdp_disconnect(instance);
        rdpLog(QStringLiteral("runConnection: freerdp_disconnect done"));
        releaseInstance(instance);
        m_instance = nullptr;
#ifdef Q_OS_WIN
        // 会话彻底结束、上下文已释放，此处才拆除异常处理器。
        rdpRemoveVectoredHandler(veh);
#endif
        rdpLog(QStringLiteral("runConnection: cleanup done, emitting disconnected"));
        emit m_client->disconnected();
    }

private:
    // FreeRDP 实例扩展上下文：附带 worker 回指针供静态回调取回 this。
    // 走高层 API 时首成员必须是 rdpClientContext（库内按此布局访问 thread/
    // 触控等字段）；rdpContext 又是 rdpClientContext 的首成员，故回调里的
    // rdpContext* → WorkerContext* 强转在两种布局下都成立。
    struct WorkerContext {
#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
        rdpClientContext clientContext;
#else
        rdpContext context;
#endif
        FreeRdpWorker *worker;
    };

    // 上下文销毁：与创建路径成对。高层 API 建出的实例须走
    // freerdp_client_context_free()（内部含 pClientEntryPoints 释放与
    // GlobalUninit 调用），低层路径仍是 context_free + free 组合。
    static void releaseInstance(freerdp *instance)
    {
        if (!instance)
            return;
#ifdef CUBESHELL_HAVE_FREERDP_CLIENT
        freerdp_client_context_free(instance->context);
#else
        freerdp_context_free(instance);
        freerdp_free(instance);
#endif
    }

    // 建连失败诊断：错误码之外带上 FreeRDP 的符号名与描述，DNS 类错误再补一句
    // 实际送给解析器的主机串（加引号，肉眼可见夹带的空白/端口/多余字符）。
    static QString connectErrorMessage(freerdp *instance, const RdpHostPort &target)
    {
        const quint32 err = freerdp_get_last_error(instance->context);
        QString message = QStringLiteral("RDP 连接失败 %1:%2")
                              .arg(target.host)
                              .arg(target.port);
        if (err == 0)
            return message + QStringLiteral("（FreeRDP 未报告错误码）");

        const char *name = freerdp_get_last_error_name(err);
        message += QStringLiteral(" (%1 0x%2)")
                       .arg(QString::fromUtf8(name ? name : "UNKNOWN"))
                       .arg(err, 8, 16, QLatin1Char('0'));
        const char *text = freerdp_get_last_error_string(err);
        if (text && *text)
            message += QLatin1Char('\n') + QString::fromUtf8(text);
        if (err == FREERDP_ERROR_DNS_NAME_NOT_FOUND || err == FREERDP_ERROR_DNS_ERROR) {
            message += QStringLiteral("\n主机名解析失败：实际送入解析器的地址为 "
                                      "\"%1\"，请确认它是纯主机名或 IP。")
                           .arg(target.host);
        }
        return message;
    }

    // 建连前回调：只做 settings 等基础状态校验。通道加载不在此处做——
    // freerdp_client_context_new() 已把 instance->LoadChannels 挂成
    // freerdp_client_load_channels，核心在 PreConnect 返回后的
    // utils_reload_channels() 里会先释放旧通道、重建通道对象再调它一次；
    // 若这里也调一次，等于双重初始化，第一次留下的状态在库内 free+重建后失效，
    // Windows 上会崩在库内（0xc0000005）。
    static BOOL preConnect(freerdp *instance)
    {
        rdpLog(QStringLiteral("preConnect called"));
        if (!instance || !instance->context || !instance->context->settings) {
            rdpLog(QStringLiteral("preConnect: null instance/context/settings, returning FALSE"));
            return FALSE;
        }
        rdpLog(QStringLiteral("preConnect returning TRUE"));
        return TRUE;
    }

    // 建连完成：初始化软件 GDI，挂 Begin/EndPaint 钩子取帧、DesktopResize 跟随改尺寸。
    static BOOL postConnect(freerdp *instance)
    {
        rdpLog(QStringLiteral("postConnect called"));
        rdpLog(QStringLiteral("postConnect: calling gdi_init"));
        if (!gdi_init(instance, PIXEL_FORMAT_BGRX32)) {
            rdpLog(QStringLiteral("postConnect: gdi_init FAILED"));
            return FALSE;
        }
        rdpLog(QStringLiteral("postConnect: gdi_init OK, setting callbacks"));
        rdpUpdate *update = instance->context->update;
        // BeginPaint 必挂：fastpath_recv_updates() 无条件经 IFCALLRET 调用它，
        // 留空指针会在服务端送来第一帧时崩在库内。
        update->BeginPaint = &FreeRdpWorker::beginPaint;
        update->EndPaint = &FreeRdpWorker::endPaint;
        update->DesktopResize = &FreeRdpWorker::desktopResize;
        rdpLog(QStringLiteral("postConnect: all callbacks set, returning TRUE"));
        return TRUE;
    }

    // 断连清理：释放 postConnect 里申请的 GDI（此回调在通道拆除前被调用）。
    static void postDisconnect(freerdp *instance)
    {
        rdpLog(QStringLiteral("postDisconnect called"));
        if (!instance || !instance->context)
            return;
        // postConnect 里 gdi_init 失败时 gdi 为空，此时 gdi_free 会解空指针
        if (instance->context->gdi)
            gdi_free(instance);
        rdpLog(QStringLiteral("postDisconnect done"));
    }

    // 凭据回调：所需凭据不全或被服务端拒绝时由库调用。用户名/口令/域已在
    // runConnection 里写进 settings，这里不改传入串、直接返回 TRUE 表示
    // “就用当前凭据继续”，FreeRDP 随后从 settings 取值。
    // 注意 FreeRDP 3 的 Authenticate(offset 50) 自 3.25 起标记废弃，由多一个
    // reason 参数的 AuthenticateEx(offset 69) 取代，两者签名不同，按版本分挂。
#if FREERDP_VERSION_MAJOR >= 3
    static BOOL authenticateEx(freerdp *instance, char **username, char **password,
                               char **domain, rdp_auth_reason reason)
    {
        Q_UNUSED(instance)
        Q_UNUSED(username)
        Q_UNUSED(password)
        Q_UNUSED(domain)
        rdpLog(QStringLiteral("authenticateEx called, reason=%1")
                   .arg(static_cast<int>(reason)));
        return TRUE;
    }
#else
    static BOOL authenticate(freerdp *instance, char **username, char **password,
                             char **domain)
    {
        Q_UNUSED(instance)
        Q_UNUSED(username)
        Q_UNUSED(password)
        Q_UNUSED(domain)
        rdpLog(QStringLiteral("authenticate called"));
        return TRUE;
    }
#endif

    // 未知证书确认：返回 1=接受并写入指纹库，2=仅本次会话接受，0=拒绝。
    // 已设 IgnoreCertificate=TRUE（堡垒机自签场景），落盘指纹没有意义且要写
    // 用户配置目录，故一律返回 2：接受当次连接，不产生持久化副作用。
    static DWORD verifyCertificateEx(freerdp *instance, const char *host, UINT16 port,
                                     const char *commonName, const char *subject,
                                     const char *issuer, const char *fingerprint,
                                     DWORD flags)
    {
        Q_UNUSED(instance)
        Q_UNUSED(commonName)
        Q_UNUSED(subject)
        Q_UNUSED(issuer)
        Q_UNUSED(flags)
        rdpLog(QStringLiteral("verifyCertificateEx called for %1:%2 fingerprint=%3")
                   .arg(QString::fromUtf8(host ? host : ""))
                   .arg(port)
                   .arg(QString::fromUtf8(fingerprint ? fingerprint : "")));
        return 2;
    }

    // 证书与已存指纹不一致时的确认，与 verifyCertificateEx 同策略。
    static DWORD verifyChangedCertificateEx(freerdp *instance, const char *host, UINT16 port,
                                            const char *commonName, const char *subject,
                                            const char *issuer, const char *newFingerprint,
                                            const char *oldSubject, const char *oldIssuer,
                                            const char *oldFingerprint, DWORD flags)
    {
        Q_UNUSED(instance)
        Q_UNUSED(commonName)
        Q_UNUSED(subject)
        Q_UNUSED(issuer)
        Q_UNUSED(newFingerprint)
        Q_UNUSED(oldSubject)
        Q_UNUSED(oldIssuer)
        Q_UNUSED(oldFingerprint)
        Q_UNUSED(flags)
        rdpLog(QStringLiteral("verifyChangedCertificateEx called for %1:%2")
                   .arg(QString::fromUtf8(host ? host : ""))
                   .arg(port));
        return 2;
    }

    // 每帧更新开始：复位失效区域累积状态，与下面的 endPaint 成对。
    // primary->hdc->hwnd->invalid 这条指针链上任一环为空都会崩在库内，逐级校验。
    static BOOL beginPaint(rdpContext *context)
    {
        rdpGdi *gdi = context ? context->gdi : nullptr;
        if (!gdi || !gdi->primary || !gdi->primary->hdc
            || !gdi->primary->hdc->hwnd || !gdi->primary->hdc->hwnd->invalid) {
            rdpLog(QStringLiteral("beginPaint: null gdi pointer chain, returning FALSE"));
            return FALSE;
        }
        gdi->primary->hdc->hwnd->invalid->null = TRUE;
        gdi->primary->hdc->hwnd->ninvalid = 0;
        return TRUE;
    }

    // 服务端改变桌面尺寸：settings 里的新尺寸已由核心写好，重建 GDI 缓冲。
    // 用 gdi_resize 而非 gdi_free+gdi_init：后者会连带拆掉 postConnect 挂的
    // 回调与 gfx/video 通道上下文，而本回调正跑在库的更新栈里。
    // gdi_resize 在 3.28 之前的 FreeRDP 3 头里未导出（如 Windows vcpkg 的 3.26），
    // 低版本回退到 gdi_free + gdi_init。
    static BOOL desktopResize(rdpContext *context)
    {
        if (!context) {
            rdpLog(QStringLiteral("desktopResize called with null context"));
            return FALSE;
        }
        rdpGdi *gdi = context->gdi;
        rdpSettings *settings = context->settings;
        if (!gdi || !settings) {
            rdpLog(QStringLiteral("desktopResize: null gdi/settings, returning FALSE"));
            return FALSE;
        }
        const UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
        const UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
        rdpLog(QStringLiteral("desktopResize called w=%1 h=%2").arg(width).arg(height));
#if defined(FREERDP_VERSION_MAJOR) && (FREERDP_VERSION_MAJOR > 3 || \
    (FREERDP_VERSION_MAJOR == 3 && FREERDP_VERSION_MINOR >= 28))
        const BOOL resized = gdi_resize(gdi, width, height);
        rdpLog(QStringLiteral("desktopResize: gdi_resize returned %1")
                   .arg(resized ? QStringLiteral("TRUE") : QStringLiteral("FALSE")));
        return resized;
#else
        rdpLog(QStringLiteral("desktopResize: gdi_free + gdi_init fallback"));
        gdi_free(context->instance);
        if (!gdi_init(context->instance, PIXEL_FORMAT_BGRX32)) {
            rdpLog(QStringLiteral("desktopResize: gdi_init FAILED"));
            return FALSE;
        }
        rdpLog(QStringLiteral("desktopResize: gdi re-init OK"));
        return TRUE;
#endif
    }

    // 服务端矩形更新落到 GDI 缓冲后回调：整帧转 QImage 发给面板。
    // 对应Python: RDPInterfaceThread.rdpconnection 里 RDPDATATYPE.VIDEO →
    // result.emit(RDPImage)（帧合并/60fps 节流由面板侧处理，同 Python 版）
    static BOOL endPaint(rdpContext *context)
    {
        if (!context)
            return TRUE;
        rdpGdi *gdi = context->gdi;
        if (!gdi || !gdi->primary_buffer) {
            rdpLog(QStringLiteral("endPaint: no gdi/primary_buffer, skip frame"));
            return TRUE;
        }
        FreeRdpWorker *worker = reinterpret_cast<WorkerContext *>(context)->worker;
        if (!worker) {
            rdpLog(QStringLiteral("endPaint: worker back-pointer NULL, skip frame"));
            return TRUE;
        }
        const QImage frame(gdi->primary_buffer,
                           static_cast<int>(gdi->width),
                           static_cast<int>(gdi->height),
                           static_cast<int>(gdi->stride),
                           QImage::Format_RGB32);
        emit worker->m_client->frameUpdated(frame.copy());
        return TRUE;
    }

    // Worker 线程侧：取空队列并逐条调用 FreeRDP input API 派发到服务端。
    // 对应Python: aardwolf 内部对 ext_in_queue 中键鼠事件的消费处理
    void flushInputQueue(freerdp *instance)
    {
        QQueue<InputEvent> pending;
        {
            QMutexLocker locker(&m_inputMutex);
            pending.swap(m_inputQueue);
        }
        if (pending.isEmpty())
            return;
        // FreeRDP 2/3 均经 instance->context->input 访问输入接口
        rdpInput *input = instance->context->input;
        if (!input)
            return;
        while (!pending.isEmpty()) {
            const InputEvent ev = pending.dequeue();
            switch (ev.type) {
            case InputEvent::Type::KeyScancode: {
                // 对应Python: RDP_KEYBOARD_SCANCODE（keyCode/is_extended/is_pressed）
                // 按下为 0（KBD_FLAGS_DOWN 语义为重复按键，首次按下不置位）
                UINT16 flags = ev.key.isPressed ? 0 : KBD_FLAGS_RELEASE;
                if (ev.key.isExtended)
                    flags |= KBD_FLAGS_EXTENDED;
#if FREERDP_VERSION_MAJOR >= 3
                // FreeRDP 3 的 code 参数收窄为 UINT8（扩展位走 flags）
                freerdp_input_send_keyboard_event(input, flags,
                                                  static_cast<UINT8>(ev.key.scancode));
#else
                freerdp_input_send_keyboard_event(input, flags, ev.key.scancode);
#endif
                break;
            }
            case InputEvent::Type::KeyUnicode: {
                // 对应Python: RDP_KEYBOARD_UNICODE（char/is_pressed）
                const UINT16 flags = ev.unicodeKey.isPressed ? 0 : KBD_FLAGS_RELEASE;
                freerdp_input_send_unicode_keyboard_event(
                    input, flags, ev.unicodeKey.character.unicode());
                break;
            }
            case InputEvent::Type::Mouse:
                // 对应Python: RDP_MOUSE（xPos/yPos/button/is_pressed → PTR_FLAGS_*）
                freerdp_input_send_mouse_event(
                    input, ev.mouse.flags,
                    static_cast<UINT16>(ev.mouse.x),
                    static_cast<UINT16>(ev.mouse.y));
                break;
            case InputEvent::Type::ClipboardText:
                // 对应Python: RDP_CLIPBOARD_DATA_TXT（CF_UNICODETEXT）
                // TODO: 需集成 FreeRDP cliprdr channel（格式协商 + 数据响应
                // 回调），实现复杂，此处预留接口暂不派发。
                break;
            }
        }
    }

    RdpClient *m_client = nullptr;
    RdpSettings m_settings;
    freerdp *m_instance = nullptr;
    // 线程安全输入队列（值语义元素，Worker 析构时随成员自动安全释放）。
    // 对应Python: RDPWidget 里的 in_q = queue.Queue()
    QMutex m_inputMutex;
    QQueue<InputEvent> m_inputQueue;
};

#endif // CUBESHELL_HAVE_FREERDP

// ---------------------------------------------------------------------------
// RdpClient
// ---------------------------------------------------------------------------
RdpClient::RdpClient(QObject *parent)
    : QObject(parent)
{
    // 状态机集中维护：两种后端都只发 connected/disconnected/errorOccurred，
    // state 由这里统一推进，避免重连时重复挂接。
    connect(this, &RdpClient::connected, this,
            [this]() { setState(State::Connected); });
    connect(this, &RdpClient::disconnected, this,
            [this]() { setState(State::Disconnected); });
    connect(this, &RdpClient::errorOccurred, this, [this](const QString &) {
        if (m_state == State::Connecting)
            setState(State::Disconnected);
    });
}

RdpClient::~RdpClient()
{
    disconnectFromHost();
}

RdpClient::Backend RdpClient::backend()
{
#ifdef CUBESHELL_HAVE_FREERDP
    return Backend::FreeRdp;
#else
    return Backend::CommandLine;
#endif
}

QString RdpClient::commandLineProgram()
{
#if defined(Q_OS_WIN)
    // Windows 自带 mstsc（注意：mstsc 不接受命令行密码，凭据由系统弹窗）
    return QStandardPaths::findExecutable(QStringLiteral("mstsc"));
#else
    // FreeRDP CLI 候选名：macOS 上 brew 的 xfreerdp 依赖 XQuartz（X11），
    // sdl-freerdp 原生可用，故 macOS 优先 sdl-freerdp；Linux 优先 xfreerdp。
#if defined(Q_OS_MACOS)
    const QStringList names = {
        QStringLiteral("sdl-freerdp"), QStringLiteral("sdl3-freerdp"),
        QStringLiteral("xfreerdp3"), QStringLiteral("xfreerdp")};
#else
    const QStringList names = {
        QStringLiteral("xfreerdp3"), QStringLiteral("xfreerdp"),
        QStringLiteral("sdl-freerdp"), QStringLiteral("sdl3-freerdp")};
#endif
    // GUI 应用（Finder/Dock 启动）PATH 通常不含 Homebrew 前缀，需显式补查。
    const QStringList extraPaths = {QStringLiteral("/opt/homebrew/bin"),
                                    QStringLiteral("/usr/local/bin")};
    for (const QString &name : names) {
        QString path = QStandardPaths::findExecutable(name);
        if (path.isEmpty())
            path = QStandardPaths::findExecutable(name, extraPaths);
        if (!path.isEmpty())
            return path;
    }
#if defined(Q_OS_MACOS)
    // 最后回退：`open rdp://...` 交给 Windows App（Microsoft Remote Desktop）
    return QStandardPaths::findExecutable(QStringLiteral("open"));
#else
    return QString();
#endif
#endif
}

QString RdpClient::resolveCommandLineArgs(QStringList *args) const
{
    const QString program = commandLineProgram();
    if (program.isEmpty())
        return QString();

    // 与 buildRdpUrl 一致：主机串归一化后，裸 IPv6 地址加方括号，再拼 host:port
    // （同时覆盖 open 的 rdp:// URL、xfreerdp 的 /v: 与 mstsc 的 /v:）
    const RdpHostPort target = normalizeRdpHost(m_settings.host, m_settings.port);
    QString host = target.host;
    if (host.count(QLatin1Char(':')) >= 2 && !host.startsWith(QLatin1Char('[')))
        host = QLatin1Char('[') + host + QLatin1Char(']');
    const QString hostPort =
        QStringLiteral("%1:%2").arg(host).arg(target.port);

#if defined(Q_OS_WIN)
    // mstsc /v:host:port [/f | /w:.. /h:..]
    *args << QStringLiteral("/v:%1").arg(hostPort);
    if (m_settings.fullscreen) {
        *args << QStringLiteral("/f");
    } else {
        *args << QStringLiteral("/w:%1").arg(m_settings.width)
              << QStringLiteral("/h:%1").arg(m_settings.height);
    }
#else
    if (program.endsWith(QLatin1String("/open"))) {
        // Microsoft Remote Desktop URI scheme（标准 rdp://host:port，
        // 无法携带密码；`full address=s:` 是 .rdp 文件语法，open 不识别）
        *args << QStringLiteral("rdp://") + hostPort;
    } else {
        // xfreerdp /v:host:port /u:user /d:domain /size:WxH /cert:ignore
        // 密码绝不放命令行：/p:%1 会让密码在整个连接期间对同机任意用户的
        // `ps -ef` 可见。FreeRDP 官方推荐用 /from-stdin:force 从 stdin 读密码
        //（连接前一次性读入），见 connectToHost() 里 start() 后的写管道。
        *args << QStringLiteral("/v:%1").arg(hostPort);
        if (!m_settings.username.isEmpty())
            *args << QStringLiteral("/u:%1").arg(m_settings.username);
        if (!m_settings.domain.isEmpty())
            *args << QStringLiteral("/d:%1").arg(m_settings.domain);
        if (m_settings.fullscreen)
            *args << QStringLiteral("/f");
        else
            *args << QStringLiteral("/size:%1x%2").arg(m_settings.width)
                                                  .arg(m_settings.height);
        *args << QStringLiteral("/cert:ignore") << QStringLiteral("+clipboard");
        if (!m_settings.password.isEmpty())
            *args << QStringLiteral("/from-stdin:force");
    }
#endif
    return program;
}

QStringList RdpClient::commandLineArgsForTest() const
{
    QStringList args;
    const QString program = resolveCommandLineArgs(&args);
    if (program.isEmpty())
        return {};
    return QStringList{program} + args;
}

void RdpClient::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void RdpClient::connectToHost(const RdpSettings &settings)
{
    ensureFreeRDPInit();
    if (m_state != State::Disconnected)
        disconnectFromHost();
    m_settings = settings;
    setState(State::Connecting);

#ifdef CUBESHELL_HAVE_FREERDP
    rdpLog(QStringLiteral("connectToHost: starting worker thread"));
    m_worker = new FreeRdpWorker(this, settings);
    // 线程自己跑完（建连失败、服务端主动断开）时回收它：
    //  · 不回收 → 下次 connectToHost 直接覆盖 m_worker，旧 QThread 对象永远
    //    挂在 this 的子对象链上，反复重连就一路攒；
    //  · 且 disconnectFromHost 会对着一条已经结束的线程白等 wait(3000)。
    // "应用新分辨率 = 重连" 会在同一面板里反复建连，这条必须先干净。
    // finished 从 worker 线程发出，经 this 回主线程；捕获的是这一条 worker，
    // 只在 m_worker 还指着它时置空（disconnectFromHost 可能已换过一轮）。
    FreeRdpWorker *worker = m_worker;
    connect(worker, &QThread::finished, this, [this, worker]() {
        rdpLog(QStringLiteral("worker finished: reclaiming thread object"));
        if (m_worker == worker)
            m_worker = nullptr;
        worker->deleteLater();
    }, Qt::QueuedConnection);
    m_worker->start();
#else
    QStringList args;
    const QString program = resolveCommandLineArgs(&args);
    if (program.isEmpty()) {
        setState(State::Disconnected);
        QString message = QStringLiteral(
            "未找到可用的 RDP 客户端（sdl-freerdp/sdl3-freerdp/"
            "xfreerdp3/xfreerdp/mstsc），且本构建未启用 FreeRDP 库");
#if defined(Q_OS_MACOS)
        message += QStringLiteral("\n建议安装：brew install freerdp");
#endif
        emit errorOccurred(message);
        return;
    }

#if defined(Q_OS_MACOS)
    if (program.endsWith(QLatin1String("/open"))) {
        // open 秒退且无法携带密码，不作为受管进程跟踪：直接分离启动系统
        // RDP 客户端，并向用户说明降级原因，避免出现裸的“退出码 1”。
        QProcess::startDetached(program, args);
        setState(State::Disconnected);
        emit errorOccurred(
            QStringLiteral("未找到 xfreerdp/sdl-freerdp，已尝试调用系统 RDP "
                           "客户端（密码需手动输入，画面在外部窗口）。\n"
                           "建议安装 FreeRDP 获得完整体验：brew install freerdp"));
        return;
    }
#endif

    m_process = new QProcess(this);
    m_lastProcessError.clear();
    connect(m_process, &QProcess::started, this, [this]() {
        emit connected();
    });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                const QString message = m_process
                    ? m_process->errorString()
                    : QStringLiteral("未知错误");
                if (error == QProcess::FailedToStart) {
                    // FailedToStart 不会再发射 finished，在此直接报错收尾
                    emit errorOccurred(message);
                    emit disconnected();
                } else {
                    // 其余错误（如 Crashed）只记录，由 finished 槽统一
                    // 发射一次，避免 UI 收到重复的错误/断开信号
                    m_lastProcessError = message;
                }
            });
    connect(m_process,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (exitCode != 0 || exitStatus == QProcess::CrashExit) {
                    QString message =
                        QStringLiteral("RDP 客户端退出，代码 %1").arg(exitCode);
                    if (!m_lastProcessError.isEmpty())
                        message += QStringLiteral("（%1）").arg(m_lastProcessError);
                    // 附带 stderr 尾部（截断），便于定位认证/证书类失败原因
                    if (m_process) {
                        QString detail = QString::fromLocal8Bit(
                            m_process->readAllStandardError()).trimmed();
                        if (detail.size() > 600)
                            detail = QStringLiteral("…") + detail.right(600);
                        if (!detail.isEmpty())
                            message += QStringLiteral("\n") + detail;
                    }
                    emit errorOccurred(message);
                }
                m_lastProcessError.clear();
                emit disconnected();
            });
    m_process->start(program, args);
    // 密码经 stdin 传给 xfreerdp（/from-stdin:force 在连接前读一行密码）。
    // 只写一行——用户名/域已通过 /u: /d: 提供，FreeRDP 不会再去 stdin 读它们，
    // 多写只会留在管道里没人消费。写完立即关写端，避免进程挂起等输入。
    if (!m_settings.password.isEmpty()) {
        m_process->write(m_settings.password.toUtf8());
        m_process->write("\n");
        m_process->closeWriteChannel();
    }
#endif
}

void RdpClient::disconnectFromHost()
{
#ifdef CUBESHELL_HAVE_FREERDP
    if (m_worker) {
        rdpLog(QStringLiteral("disconnectFromHost: stopping worker"));
        // 先清空未派发输入，再终止事件循环，避免残留事件打到断连过程
        m_worker->clearInputQueue();
        m_worker->requestStop();
        const bool finished = m_worker->wait(3000);
        rdpLog(QStringLiteral("disconnectFromHost: worker wait %1")
                   .arg(finished ? QStringLiteral("finished") : QStringLiteral("TIMED OUT")));
        m_worker->deleteLater();
        m_worker = nullptr;
    }
#endif
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->terminate();
            if (!m_process->waitForFinished(1500))
                m_process->kill();
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
    setState(State::Disconnected);
}

// ---------------------------------------------------------------------------
// 输入派发：主线程 → Worker 线程队列 → FreeRDP input API
// 对应Python: GUI 线程 in_q.put(...) → inputhandler 线程 → conn.ext_in_queue
// 命令行后备路径下全部为空实现（外部窗口客户端自有输入处理）。
// ---------------------------------------------------------------------------

// 对应Python: in_q.put(RDP_KEYBOARD_SCANCODE)
void RdpClient::sendKeyEvent(const RdpKeyEvent &event)
{
#ifdef CUBESHELL_HAVE_FREERDP
    if (m_worker) {
        FreeRdpWorker::InputEvent ev;
        ev.type = FreeRdpWorker::InputEvent::Type::KeyScancode;
        ev.key = event;
        m_worker->enqueueInput(ev);
    }
#else
    Q_UNUSED(event);
#endif
}

// 对应Python: in_q.put(RDP_KEYBOARD_UNICODE)
void RdpClient::sendKeyUnicode(const RdpUnicodeKeyEvent &event)
{
#ifdef CUBESHELL_HAVE_FREERDP
    if (m_worker) {
        FreeRdpWorker::InputEvent ev;
        ev.type = FreeRdpWorker::InputEvent::Type::KeyUnicode;
        ev.unicodeKey = event;
        m_worker->enqueueInput(ev);
    }
#else
    Q_UNUSED(event);
#endif
}

// 对应Python: in_q.put(RDP_MOUSE)
void RdpClient::sendMouseEvent(const RdpMouseEvent &event)
{
#ifdef CUBESHELL_HAVE_FREERDP
    if (m_worker) {
        FreeRdpWorker::InputEvent ev;
        ev.type = FreeRdpWorker::InputEvent::Type::Mouse;
        ev.mouse = event;
        m_worker->enqueueInput(ev);
    }
#else
    Q_UNUSED(event);
#endif
}

// 对应Python: in_q.put(RDP_CLIPBOARD_DATA_TXT)
void RdpClient::sendClipboardText(const QString &text)
{
#ifdef CUBESHELL_HAVE_FREERDP
    if (m_worker) {
        FreeRdpWorker::InputEvent ev;
        ev.type = FreeRdpWorker::InputEvent::Type::ClipboardText;
        ev.clipboardText = text;
        m_worker->enqueueInput(ev);
    }
#else
    Q_UNUSED(text);
#endif
}

// 对应Python: clipboard_send_files（conn.set_current_clipboard_files）
void RdpClient::clipboardSendFiles(const QStringList &paths)
{
    // TODO: 需 FreeRDP cliprdr channel 的文件列表格式（CF_HDROP/FileGroup
    // Descriptor）支持，实现复杂，此处预留接口暂不派发。
    Q_UNUSED(paths);
}

} // namespace cubeshell
