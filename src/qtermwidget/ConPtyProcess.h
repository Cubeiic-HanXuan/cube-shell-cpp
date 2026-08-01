#pragma once

// ConPtyProcess.h — Windows ConPTY（伪控制台）进程封装
//
// POSIX 平台的伪终端由 KPty / KPtyDevice / KPtyProcess 提供；Windows 10 1809+
// 则提供 ConPTY API（CreatePseudoConsole / ResizePseudoConsole /
// ClosePseudoConsole）。本类封装其完整生命周期：
//   1. 建立两对匿名管道（应用 -> ConPTY 输入、ConPTY 输出 -> 应用）
//   2. CreatePseudoConsole 创建伪控制台并接管传入的管道端
//   3. 以 STARTUPINFOEXW + PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 启动子进程
//   4. 独立读线程阻塞读取输出，通过信号跨线程投递给主线程的 Pty 对象
//
// 该头文件整体受 Q_OS_WIN 保护，其它平台包含后为空。

#include <QtGlobal> // 提供 Q_OS_WIN；必须在条件编译之前包含

#ifdef Q_OS_WIN

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <atomic>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// 若 SDK 过旧（NTDDI_VERSION < NTDDI_WIN10_RS5）未声明 ConPTY 相关定义，则在此
// 补齐。函数本身在 .cpp 中通过 GetProcAddress 动态解析，因此不引入链接依赖。
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
typedef VOID *HPCON;
#endif

namespace Konsole {

class ConPtyProcess : public QObject {
    Q_OBJECT
public:
    explicit ConPtyProcess(QObject *parent = nullptr);
    ~ConPtyProcess() override;

    // 创建伪控制台并启动子进程。environment 为 "KEY=VALUE" 列表，会覆盖在当前
    // 进程环境之上（与 POSIX 侧 KProcess::setEnv 的语义保持一致）。
    bool start(const QString &program, const QStringList &args,
               const QStringList &env, const QString &workingDir,
               int columns, int rows);

    qint64 write(const char *data, qint64 len);
    bool resize(int columns, int rows);
    bool isRunning() const;
    int exitCode() const;
    void terminate();
    void kill();

Q_SIGNALS:
    void readyRead(const QByteArray &data);
    void finished(int exitCode);

private:
    void readLoop();
    void cleanup();

    HPCON m_hPC = nullptr;
    HANDLE m_hPipeIn = INVALID_HANDLE_VALUE;
    HANDLE m_hPipeOut = INVALID_HANDLE_VALUE;
    HANDLE m_hProcess = INVALID_HANDLE_VALUE;
    HANDLE m_hThread = INVALID_HANDLE_VALUE;

    QThread *m_readThread = nullptr;
    std::atomic<bool> m_running{false};
    int m_exitCode = 0;
};

} // namespace Konsole

#endif // Q_OS_WIN
