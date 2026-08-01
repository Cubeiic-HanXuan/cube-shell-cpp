// RdpClient.cpp — RDP 客户端实现（FreeRDP 库 / 命令行双后端）。
// 对应Python: core/rdp/rdp_client.py

#include "RdpClient.h"

#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#ifdef CUBESHELL_HAVE_FREERDP
#include <freerdp/error.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/version.h>
#include <winpr/synch.h>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>
#include <QThread>
#endif

namespace cubeshell {

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
// buildRdpUrl
// ---------------------------------------------------------------------------
// 对应Python: core/rdp/rdp_client.py::build_rdp_url
QString buildRdpUrl(const RdpSettings &settings, const QString &auth)
{
    const QString scheme = auth == QLatin1String("ntlm")
        ? QStringLiteral("rdp+ntlm-password")
        : QStringLiteral("rdp");

    QString userinfo;
    if (!settings.username.isEmpty()) {
        const QString user = settings.domain.isEmpty()
            ? settings.username
            : settings.domain + QLatin1Char('\\') + settings.username;
        // 保留反斜杠（DOMAIN\user 形式），其余按 userinfo 规则转义
        userinfo = QString::fromUtf8(
            QUrl::toPercentEncoding(user, QByteArrayLiteral("\\")));
        if (!settings.password.isEmpty()) {
            userinfo += QLatin1Char(':')
                + QString::fromUtf8(QUrl::toPercentEncoding(settings.password));
        }
        userinfo += QLatin1Char('@');
    }

    const RdpHostPort target = normalizeRdpHost(settings.host, settings.port);
    QString host = target.host;
    // 裸 IPv6 地址加方括号
    if (host.count(QLatin1Char(':')) >= 2 && !host.startsWith(QLatin1Char('[')))
        host = QLatin1Char('[') + host + QLatin1Char(']');

    return QStringLiteral("%1://%2%3:%4").arg(scheme, userinfo, host,
                                              QString::number(target.port));
}

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
    void run() override
    {
        freerdp *instance = freerdp_new();
        if (!instance) {
            emit m_client->errorOccurred(QStringLiteral("freerdp_new 失败"));
            return;
        }
        instance->ContextSize = sizeof(WorkerContext);
        instance->PostConnect = &FreeRdpWorker::postConnect;

        if (!freerdp_context_new(instance)) {
            freerdp_free(instance);
            emit m_client->errorOccurred(QStringLiteral("freerdp_context_new 失败"));
            return;
        }
        reinterpret_cast<WorkerContext *>(instance->context)->worker = this;
        m_instance = instance;

        // 连接参数（对应 Python 侧 build_rdp_url 携带的 host/port/user/pwd/domain
        // 与 iosettings 的分辨率）
        rdpSettings *settings = instance->context->settings;
        // ServerHostname 会被原样交给 getaddrinfo()：先归一化，再校验非空，
        // 否则失败只会表现为一个含义误导的 DNS_NAME_NOT_FOUND 错误码。
        const RdpHostPort target = normalizeRdpHost(m_settings.host, m_settings.port);
        if (target.host.isEmpty()) {
            emit m_client->errorOccurred(
                QStringLiteral("RDP 主机地址为空（原始值：\"%1\"）").arg(m_settings.host));
            freerdp_context_free(instance);
            freerdp_free(instance);
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
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
                                    static_cast<quint32>(m_settings.width));
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
                                    static_cast<quint32>(m_settings.height));
        // 安全层协商：NLA(NTLM)/TLS/RDP 三层全开由服务端挑选，对应 Python 侧
        // build_rdp_url 的 rdp+ntlm-password（NLA）语义。三者与 Authentication
        // 同为 FreeRDP 3 的默认值，显式写出以免换库版本/发行版后默认值漂移。
        freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_Authentication, TRUE);
        // 32bpp 与下面 gdi_init 的 PIXEL_FORMAT_BGRX32 对齐
        freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
        // 嵌入式面板渲染：软件 GDI，忽略自签证书（与 Python 侧堡垒机场景一致）
        freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE);

        if (!freerdp_connect(instance)) {
            emit m_client->errorOccurred(connectErrorMessage(instance, target));
            freerdp_context_free(instance);
            freerdp_free(instance);
            m_instance = nullptr;
            return;
        }

        emit m_client->connected();

        // 事件循环。对应Python: rdpconnection() 的 ext_out_queue 消费循环 +
        // inputhandler 线程的 in_q 消费（lines 420-426），C++ 侧合并为单循环：
        // 100ms 超时唤醒后 poll 输入队列并调用 FreeRDP input API 派发。
        HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
        while (!isInterruptionRequested()) {
            const DWORD count = freerdp_get_event_handles(instance->context, handles,
                                                          MAXIMUM_WAIT_OBJECTS);
            if (count == 0)
                break;
            const DWORD status = WaitForMultipleObjects(count, handles, FALSE, 100);
            if (status == WAIT_FAILED)
                break;
            // 对应Python: loop.call_soon_threadsafe(conn.ext_in_queue.put_nowait, data)
            flushInputQueue(instance);
            if (!freerdp_check_event_handles(instance->context))
                break;
#if FREERDP_VERSION_MAJOR >= 3
            if (freerdp_shall_disconnect_context(instance->context))
                break;
#else
            if (freerdp_shall_disconnect(instance))
                break;
#endif
        }

        freerdp_disconnect(instance);
        freerdp_context_free(instance);
        freerdp_free(instance);
        m_instance = nullptr;
        emit m_client->disconnected();
    }

private:
    // FreeRDP 实例扩展上下文：附带 worker 回指针供静态回调取回 this。
    struct WorkerContext {
        rdpContext context;
        FreeRdpWorker *worker;
    };

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

    // 建连完成：初始化软件 GDI，挂 EndPaint 钩子取帧。
    static BOOL postConnect(freerdp *instance)
    {
        if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
            return FALSE;
        instance->context->update->EndPaint = &FreeRdpWorker::endPaint;
        return TRUE;
    }

    // 服务端矩形更新落到 GDI 缓冲后回调：整帧转 QImage 发给面板。
    // 对应Python: RDPInterfaceThread.rdpconnection 里 RDPDATATYPE.VIDEO →
    // result.emit(RDPImage)（帧合并/60fps 节流由面板侧处理，同 Python 版）
    static BOOL endPaint(rdpContext *context)
    {
        rdpGdi *gdi = context->gdi;
        if (!gdi || !gdi->primary_buffer)
            return TRUE;
        FreeRdpWorker *worker = reinterpret_cast<WorkerContext *>(context)->worker;
        if (!worker)
            return TRUE;
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
        // xfreerdp /v:host:port /u:user /p:pwd /d:domain /size:WxH /cert:ignore
        *args << QStringLiteral("/v:%1").arg(hostPort);
        if (!m_settings.username.isEmpty())
            *args << QStringLiteral("/u:%1").arg(m_settings.username);
        if (!m_settings.password.isEmpty())
            *args << QStringLiteral("/p:%1").arg(m_settings.password);
        if (!m_settings.domain.isEmpty())
            *args << QStringLiteral("/d:%1").arg(m_settings.domain);
        if (m_settings.fullscreen)
            *args << QStringLiteral("/f");
        else
            *args << QStringLiteral("/size:%1x%2").arg(m_settings.width)
                                                  .arg(m_settings.height);
        *args << QStringLiteral("/cert:ignore") << QStringLiteral("+clipboard");
    }
#endif
    return program;
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
    if (m_state != State::Disconnected)
        disconnectFromHost();
    m_settings = settings;
    setState(State::Connecting);

#ifdef CUBESHELL_HAVE_FREERDP
    m_worker = new FreeRdpWorker(this, settings);
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
#endif
}

void RdpClient::disconnectFromHost()
{
#ifdef CUBESHELL_HAVE_FREERDP
    if (m_worker) {
        // 先清空未派发输入，再终止事件循环，避免残留事件打到断连过程
        m_worker->clearInputQueue();
        m_worker->requestStop();
        m_worker->wait(3000);
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
