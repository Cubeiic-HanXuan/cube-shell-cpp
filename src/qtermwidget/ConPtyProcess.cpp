// ConPtyProcess.cpp — Windows ConPTY 进程封装实现（其它平台整体编译为空）

#include "ConPtyProcess.h"

#ifdef Q_OS_WIN

#include "tools.h"

#include <QDir>
#include <QProcessEnvironment>
#include <QMap>

#include <vector>

namespace Konsole {

namespace {

// 管道空闲时的轮询间隔：ConPTY 的输出管道由 conhost 持有，子进程退出并不会让
// ReadFile 返回，必须额外等待进程句柄才能感知退出（Windows Terminal 的
// ConptyConnection 同样单独 wait 客户端进程句柄）。
constexpr DWORD kIdlePollIntervalMs = 10;

// ConPTY 三个入口点自 Windows 10 1809 起才存在于 kernel32.dll。静态导入会让
// 程序在更早的系统上直接加载失败（"找不到入口点"），因此统一动态解析，缺失时
// 退化为一次明确的告警。
using CreatePseudoConsoleFn = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
using ResizePseudoConsoleFn = HRESULT(WINAPI *)(HPCON, COORD);
using ClosePseudoConsoleFn = void(WINAPI *)(HPCON);

struct ConPtyApi {
    CreatePseudoConsoleFn createPseudoConsole = nullptr;
    ResizePseudoConsoleFn resizePseudoConsole = nullptr;
    ClosePseudoConsoleFn closePseudoConsole = nullptr;

    bool isAvailable() const
    {
        return createPseudoConsole && resizePseudoConsole && closePseudoConsole;
    }
};

const ConPtyApi &conPtyApi()
{
    static const ConPtyApi api = [] {
        ConPtyApi resolved;
        if (HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll")) {
            resolved.createPseudoConsole =
                reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(kernel32, "CreatePseudoConsole"));
            resolved.resizePseudoConsole =
                reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(kernel32, "ResizePseudoConsole"));
            resolved.closePseudoConsole =
                reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(kernel32, "ClosePseudoConsole"));
        }
        if (!resolved.isAvailable())
            qCWarning(qtermwidgetLogger) << "ConPTY API unavailable; Windows 10 1809 (build 17763) or later is required.";
        return resolved;
    }();
    return api;
}

QString lastErrorText(DWORD error)
{
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString message = QStringLiteral("error %1").arg(error);
    if (length > 0 && buffer) {
        message += QLatin1String(": ") + QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
    }
    if (buffer)
        LocalFree(buffer);
    return message;
}

void closeHandleIfValid(HANDLE &handle)
{
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr)
        CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
}

// CreateProcessW 的命令行需要自行拼接与转义；仅当参数含空白或为空时加引号。
QString quoteArgument(const QString &argument)
{
    if (argument.isEmpty())
        return QStringLiteral("\"\"");
    if (!argument.contains(QLatin1Char(' ')) && !argument.contains(QLatin1Char('\t')))
        return argument;
    if (argument.startsWith(QLatin1Char('"')) && argument.endsWith(QLatin1Char('"')))
        return argument;

    QString quoted = argument;
    quoted.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

QString buildCommandLine(const QString &program, const QStringList &args)
{
    QString commandLine = quoteArgument(QDir::toNativeSeparators(program));
    for (const QString &arg : args)
        commandLine += QLatin1Char(' ') + quoteArgument(arg);
    return commandLine;
}

// 构造 CREATE_UNICODE_ENVIRONMENT 所需的环境块：每项以 NUL 结尾，整块以双 NUL
// 结尾。当前进程环境作为基底（否则子进程会缺少 PATH / SystemRoot 而无法启动），
// 调用方传入的项覆盖同名变量，最后补齐 TERM / COLORTERM。
std::vector<wchar_t> buildEnvironmentBlock(const QStringList &environment)
{
    // Windows 变量名不区分大小写，且环境块约定按变量名排序，故以大写名为键。
    QMap<QString, QString> merged;

    const QProcessEnvironment systemEnvironment = QProcessEnvironment::systemEnvironment();
    const QStringList systemKeys = systemEnvironment.keys();
    for (const QString &key : systemKeys)
        merged.insert(key.toUpper(), key + QLatin1Char('=') + systemEnvironment.value(key));

    for (const QString &pair : environment) {
        const int pos = pair.indexOf(QLatin1Char('='));
        if (pos <= 0)
            continue;
        merged.insert(pair.left(pos).toUpper(), pair);
    }

    if (!merged.contains(QStringLiteral("TERM")))
        merged.insert(QStringLiteral("TERM"), QStringLiteral("TERM=xterm-256color"));
    if (!merged.contains(QStringLiteral("COLORTERM")))
        merged.insert(QStringLiteral("COLORTERM"), QStringLiteral("COLORTERM=truecolor"));

    std::vector<wchar_t> block;
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it) {
        const std::wstring entry = it.value().toStdWString();
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    // 空块也必须是双 NUL，否则 CreateProcessW 会读越界。
    if (block.empty())
        block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

} // namespace

ConPtyProcess::ConPtyProcess(QObject *parent)
    : QObject(parent)
{
}

ConPtyProcess::~ConPtyProcess()
{
    cleanup();
}

bool ConPtyProcess::start(const QString &program, const QStringList &args,
                          const QStringList &env, const QString &workingDir,
                          int columns, int rows)
{
    if (m_running) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::start - a process is already running.";
        return false;
    }

    const ConPtyApi &api = conPtyApi();
    if (!api.isAvailable())
        return false;

    if (columns <= 0)
        columns = 80;
    if (rows <= 0)
        rows = 24;

    // ConPTY 输入管道：应用写 hPipeIn_W，ConPTY 读 hPipeIn_R。
    // ConPTY 输出管道：ConPTY 写 hPipeOut_W，应用读 hPipeOut_R。
    HANDLE hPipePTYIn_R = INVALID_HANDLE_VALUE;
    HANDLE hPipePTYIn_W = INVALID_HANDLE_VALUE;
    HANDLE hPipePTYOut_R = INVALID_HANDLE_VALUE;
    HANDLE hPipePTYOut_W = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&hPipePTYIn_R, &hPipePTYIn_W, nullptr, 0)) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::start - CreatePipe (input) failed:" << lastErrorText(GetLastError());
        return false;
    }
    if (!CreatePipe(&hPipePTYOut_R, &hPipePTYOut_W, nullptr, 0)) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::start - CreatePipe (output) failed:" << lastErrorText(GetLastError());
        closeHandleIfValid(hPipePTYIn_R);
        closeHandleIfValid(hPipePTYIn_W);
        return false;
    }

    const COORD size{static_cast<SHORT>(columns), static_cast<SHORT>(rows)};
    const HRESULT hr = api.createPseudoConsole(size, hPipePTYIn_R, hPipePTYOut_W, 0, &m_hPC);

    // CreatePseudoConsole 成功后伪控制台已复制/接管这两端，应用侧立即释放；
    // 失败时同样需要释放，避免句柄泄漏。
    closeHandleIfValid(hPipePTYIn_R);
    closeHandleIfValid(hPipePTYOut_W);

    if (FAILED(hr) || !m_hPC) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::start - CreatePseudoConsole failed, hr = 0x"
                                     << QString::number(static_cast<quint32>(hr), 16);
        m_hPC = nullptr;
        closeHandleIfValid(hPipePTYIn_W);
        closeHandleIfValid(hPipePTYOut_R);
        return false;
    }

    m_hPipeIn = hPipePTYIn_W;
    m_hPipeOut = hPipePTYOut_R;

    // 携带 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 的扩展启动信息。
    STARTUPINFOEXW startupInfo{};
    startupInfo.StartupInfo.cb = static_cast<DWORD>(sizeof(STARTUPINFOEXW));

    SIZE_T attributeListSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);
    if (attributeListSize == 0) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::start - InitializeProcThreadAttributeList (size query) failed:"
                                     << lastErrorText(GetLastError());
        cleanup();
        return false;
    }

    std::vector<char> attributeListBuffer(attributeListSize);
    startupInfo.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeListBuffer.data());

    if (!InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &attributeListSize)) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::start - InitializeProcThreadAttributeList failed:"
                                     << lastErrorText(GetLastError());
        cleanup();
        return false;
    }

    if (!UpdateProcThreadAttribute(startupInfo.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   m_hPC, sizeof(HPCON), nullptr, nullptr)) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::start - UpdateProcThreadAttribute failed:"
                                     << lastErrorText(GetLastError());
        DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        cleanup();
        return false;
    }

    std::vector<wchar_t> environmentBlock = buildEnvironmentBlock(env);

    // CreateProcessW 可能就地修改命令行缓冲区，必须传可写内存。
    std::wstring commandLine = buildCommandLine(program, args).toStdWString();
    std::vector<wchar_t> commandLineBuffer(commandLine.begin(), commandLine.end());
    commandLineBuffer.push_back(L'\0');

    std::wstring nativeWorkingDir;
    if (!workingDir.isEmpty())
        nativeWorkingDir = QDir::toNativeSeparators(workingDir).toStdWString();

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(nullptr,
                                        commandLineBuffer.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                        environmentBlock.data(),
                                        nativeWorkingDir.empty() ? nullptr : nativeWorkingDir.c_str(),
                                        &startupInfo.StartupInfo,
                                        &processInfo);

    DeleteProcThreadAttributeList(startupInfo.lpAttributeList);

    if (!created) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::start - CreateProcessW failed for" << program << ":"
                                     << lastErrorText(GetLastError());
        cleanup();
        return false;
    }

    m_hProcess = processInfo.hProcess;
    m_hThread = processInfo.hThread;
    m_exitCode = 0;
    m_running = true;

    qCDebug(qtermwidgetLogger) << "ConPtyProcess::start -" << program << "pid" << processInfo.dwProcessId
                               << QStringLiteral("(%1x%2)").arg(columns).arg(rows);

    // 读线程负责搬运输出：readyRead / finished 跨线程发射，Qt 自动以
    // QueuedConnection 投递到接收者（主线程的 Pty）所在线程。
    m_readThread = QThread::create(&ConPtyProcess::readLoop, this);
    m_readThread->setObjectName(QStringLiteral("ConPtyReader"));
    connect(m_readThread, &QThread::finished, m_readThread, &QObject::deleteLater);
    m_readThread->start();

    return true;
}

void ConPtyProcess::readLoop()
{
    char buffer[4096];
    const HANDLE pipe = m_hPipeOut;

    while (m_running) {
        // 先 Peek 再 Read：只读已就绪的字节，ReadFile 因此不会长时间阻塞，
        // 循环得以同时照看子进程是否已退出。
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED)
                qCDebug(qtermwidgetLogger) << "ConPtyProcess::readLoop - pipe closed, error =" << error;
            else
                qCWarning(qtermwidgetLogger) << "ConPtyProcess::readLoop - PeekNamedPipe failed:" << lastErrorText(error);
            break;
        }

        if (available > 0) {
            DWORD bytesRead = 0;
            const DWORD toRead = qMin<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
            if (!ReadFile(pipe, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED)
                    qCDebug(qtermwidgetLogger) << "ConPtyProcess::readLoop - pipe closed, error =" << error;
                else
                    qCWarning(qtermwidgetLogger) << "ConPtyProcess::readLoop - ReadFile failed:" << lastErrorText(error);
                break;
            }
            Q_EMIT readyRead(QByteArray(buffer, static_cast<int>(bytesRead)));
            continue;
        }

        // 暂无输出：等待子进程退出，顺带充当轮询间隔。
        const DWORD waitResult = (m_hProcess != INVALID_HANDLE_VALUE)
            ? WaitForSingleObject(m_hProcess, kIdlePollIntervalMs)
            : WAIT_OBJECT_0;
        if (waitResult == WAIT_OBJECT_0) {
            // 子进程已退出，但管道里可能还有最后一段输出，排空后再结束。
            DWORD remaining = 0;
            if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &remaining, nullptr) && remaining > 0)
                continue;
            break;
        }
    }

    DWORD code = 0;
    if (m_hProcess != INVALID_HANDLE_VALUE) {
        // 进程可能尚未完全退出，稍等一下再取退出码，避免拿到 STILL_ACTIVE。
        WaitForSingleObject(m_hProcess, 1000);
        if (!GetExitCodeProcess(m_hProcess, &code))
            code = 0;
    }
    m_exitCode = static_cast<int>(code);
    m_running = false;

    Q_EMIT finished(m_exitCode);
}

qint64 ConPtyProcess::write(const char *data, qint64 len)
{
    if (!data || len <= 0)
        return 0;
    if (m_hPipeIn == INVALID_HANDLE_VALUE) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::write - input pipe is closed.";
        return -1;
    }

    DWORD bytesWritten = 0;
    if (!WriteFile(m_hPipeIn, data, static_cast<DWORD>(len), &bytesWritten, nullptr)) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::write - WriteFile failed:" << lastErrorText(GetLastError());
        return -1;
    }
    return static_cast<qint64>(bytesWritten);
}

bool ConPtyProcess::resize(int columns, int rows)
{
    if (!m_hPC)
        return false;
    if (columns <= 0 || rows <= 0)
        return false;

    const ConPtyApi &api = conPtyApi();
    if (!api.isAvailable())
        return false;

    const COORD size{static_cast<SHORT>(columns), static_cast<SHORT>(rows)};
    const HRESULT hr = api.resizePseudoConsole(m_hPC, size);
    if (FAILED(hr)) {
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::resize - ResizePseudoConsole failed, hr = 0x"
                                     << QString::number(static_cast<quint32>(hr), 16);
        return false;
    }
    return true;
}

bool ConPtyProcess::isRunning() const
{
    return m_running.load();
}

int ConPtyProcess::exitCode() const
{
    return m_exitCode;
}

void ConPtyProcess::terminate()
{
    if (!m_running)
        return;

    // ConPTY 下没有 POSIX 的 SIGINT，向输入管道写入 Ctrl+C（0x03）由伪控制台
    // 翻译为控制台中断事件，是让前台程序优雅退出的正确方式。
    if (m_hPipeIn != INVALID_HANDLE_VALUE) {
        const char ctrlC = '\x03';
        DWORD bytesWritten = 0;
        if (WriteFile(m_hPipeIn, &ctrlC, 1, &bytesWritten, nullptr) && bytesWritten == 1)
            return;
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::terminate - writing Ctrl+C failed:" << lastErrorText(GetLastError());
    }

    if (m_hProcess != INVALID_HANDLE_VALUE && !TerminateProcess(m_hProcess, 0))
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::terminate - TerminateProcess failed:" << lastErrorText(GetLastError());
}

void ConPtyProcess::kill()
{
    if (m_hProcess == INVALID_HANDLE_VALUE)
        return;
    if (!TerminateProcess(m_hProcess, 1))
        qCWarning(qtermwidgetLogger) << "ConPtyProcess::kill - TerminateProcess failed:" << lastErrorText(GetLastError());
}

void ConPtyProcess::cleanup()
{
    m_running = false;

    // 顺序很重要：先取消挂起的 ReadFile 并关闭伪控制台（会关闭 ConPTY 侧的管道
    // 端，使读端立刻返回 broken pipe），再等待读线程结束，最后才关闭句柄——
    // 否则读线程可能操作已关闭的句柄。
    if (m_hPipeOut != INVALID_HANDLE_VALUE)
        CancelIoEx(m_hPipeOut, nullptr);

    if (m_hPC) {
        conPtyApi().closePseudoConsole(m_hPC);
        m_hPC = nullptr;
    }

    if (m_readThread) {
        if (!m_readThread->wait(3000)) {
            qCWarning(qtermwidgetLogger) << "ConPtyProcess::cleanup - read thread did not finish in time, terminating it.";
            m_readThread->terminate();
            m_readThread->wait(500);
        }
        // 线程对象通过 finished -> deleteLater 自行销毁。
        m_readThread = nullptr;
    }

    closeHandleIfValid(m_hPipeOut);
    closeHandleIfValid(m_hPipeIn);
    closeHandleIfValid(m_hThread);
    closeHandleIfValid(m_hProcess);
}

} // namespace Konsole

#endif // Q_OS_WIN
