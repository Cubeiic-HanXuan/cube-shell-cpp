// CommandExecutor / RemoteMonitor / MfaPromptHandler test.
//
// Style mirrors tests/sftp_integration_test.cpp (no gtest, main + CHECK macro).
// Part 1 is pure logic (no network): sudo prompt/flag helpers, long-running
// command detection, cancel flag, offline error paths, MFA state machine.
// Part 2 needs a live sshd (default 127.0.0.1:2222, linuxserver/openssh-server
// container). When the server is unreachable the network part is SKIPPED and
// the test exits 2 — but only after the pure-logic part passed.
// Env overrides: CUBESSH_HOST, CUBESSH_PORT, CUBESSH_USER, CUBESSH_PASS.

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include "ssh/CommandExecutor.h"
#include "ssh/MfaPromptHandler.h"
#include "ssh/RemoteMonitor.h"
#include "ssh/SshClient.h"

#include <functional>

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// Pump the event loop until pred() holds or timeoutMs elapses.
static bool waitFor(const std::function<bool()> &pred, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return pred();
}

// ──────────────────────────── Part 1: pure logic ────────────────────────────

// 对应Python: tests 对 _needs_sudo_password / _inject_sudo_stdin_flag 的语义
static void testSudoHelpers()
{
    // root never needs a password
    CHECK(!CommandExecutor::needsSudoPassword(QStringLiteral("sudo apt update"), QStringLiteral("root")));
    // no sudo in the command
    CHECK(!CommandExecutor::needsSudoPassword(QStringLiteral("ls -la"), QStringLiteral("bob")));
    // plain sudo → needs password
    CHECK(CommandExecutor::needsSudoPassword(QStringLiteral("sudo systemctl restart nginx"), QStringLiteral("bob")));
    // already has -S → no double handling
    CHECK(!CommandExecutor::needsSudoPassword(QStringLiteral("sudo -S apt update"), QStringLiteral("bob")));
    // "sudoedit" is not the word sudo
    CHECK(!CommandExecutor::needsSudoPassword(QStringLiteral("sudoedit /etc/hosts"), QStringLiteral("bob")));

    CHECK(CommandExecutor::injectSudoStdinFlag(QStringLiteral("sudo apt update"))
          == QStringLiteral("sudo -S apt update"));
    CHECK(CommandExecutor::injectSudoStdinFlag(QStringLiteral("sudo -S apt update"))
          == QStringLiteral("sudo -S apt update"));
    CHECK(CommandExecutor::injectSudoStdinFlag(QStringLiteral("echo hi && sudo reboot"))
          == QStringLiteral("echo hi && sudo -S reboot"));
    CHECK(CommandExecutor::injectSudoStdinFlag(QStringLiteral("ls"))
          == QStringLiteral("ls"));
}

// 对应Python: ssh_agent.py::_is_long_running_command 的模式表
static void testLongRunningDetection()
{
    CHECK(CommandExecutor::isLongRunningCommand(QStringLiteral("wget https://example.com/big.iso")));
    CHECK(CommandExecutor::isLongRunningCommand(QStringLiteral("curl -o out.tgz https://x")));
    CHECK(CommandExecutor::isLongRunningCommand(QStringLiteral("pip install requests")));
    CHECK(CommandExecutor::isLongRunningCommand(QStringLiteral("git clone https://x/repo.git")));
    CHECK(CommandExecutor::isLongRunningCommand(QStringLiteral("docker pull nginx")));
    CHECK(!CommandExecutor::isLongRunningCommand(QStringLiteral("ls -la /tmp")));
    CHECK(!CommandExecutor::isLongRunningCommand(QStringLiteral("curl https://x"))); // no -o/-O/pipe
    CHECK(!CommandExecutor::isLongRunningCommand(QStringLiteral("cat /proc/stat")));
}

static void testSudoPromptDetection()
{
    CHECK(CommandExecutor::looksLikeSudoPasswordPrompt(QStringLiteral("[sudo] password for bob:")));
    CHECK(CommandExecutor::looksLikeSudoPasswordPrompt(QStringLiteral("Password:")));
    CHECK(CommandExecutor::looksLikeSudoPasswordPrompt(QStringLiteral("some text\npassword for bob: ")));
    CHECK(!CommandExecutor::looksLikeSudoPasswordPrompt(QStringLiteral("downloading file 42%")));
    CHECK(!CommandExecutor::looksLikeSudoPasswordPrompt(QStringLiteral("passwordless login enabled\n")));
}

static void testCancelFlagAndOfflineErrors()
{
    SshClient client; // never connected
    CommandExecutor ex(&client);

    // cancel flag round-trip 对应Python: request_stop 的 _stop_flag
    CHECK(!ex.isCancelRequested());
    ex.cancel();
    CHECK(ex.isCancelRequested());

    // sync exec resets the flag and fails cleanly when not connected
    const ExecResult r = ex.exec(QStringLiteral("echo hi"));
    CHECK(!r.ok());
    CHECK(r.exitCode == -1);
    CHECK(!r.errorMessage.isEmpty());
    CHECK(!ex.isCancelRequested()); // reset at call entry

    // null client guard
    const ExecResult r2 = CommandExecutor::runCommand(nullptr, QStringLiteral("ls"), false, 1000);
    CHECK(!r2.ok());
    CHECK(!r2.errorMessage.isEmpty());

    // ExecResult flags drive ok()
    ExecResult f;
    f.exitCode = 0;
    CHECK(f.ok());
    f.timedOut = true;
    CHECK(!f.ok());
}

// 对应Python: cube-shell.py MFA 弹窗流程（提示→输入→重试→成功/失败）
static void testMfaStateMachine()
{
    MfaPromptHandler h;
    h.setSuccessGraceMs(0);
    h.setMaxAttempts(3);

    QStringList prompts;
    QList<int> attempts;
    QStringList codes;
    int succeeded = 0, failed = 0;
    QObject::connect(&h, &MfaPromptHandler::mfaPromptRequired,
                     [&](const QString &p, int a) { prompts << p; attempts << a; });
    QObject::connect(&h, &MfaPromptHandler::codeReady,
                     [&](const QString &c) { codes << c; });
    QObject::connect(&h, &MfaPromptHandler::mfaSucceeded, [&]() { ++succeeded; });
    QObject::connect(&h, &MfaPromptHandler::mfaFailed, [&](const QString &) { ++failed; });

    CHECK(h.state() == MfaPromptHandler::State::Idle);

    // prompt → dialog
    h.onPromptDetected(QStringLiteral("Verification code: "));
    CHECK(h.state() == MfaPromptHandler::State::WaitingForCode);
    CHECK(prompts.size() == 1 && attempts.last() == 1);

    // duplicate prompt while the dialog is open is ignored
    h.onPromptDetected(QStringLiteral("Verification code: "));
    CHECK(prompts.size() == 1);

    // empty code is not sent
    h.submitCode(QString());
    CHECK(codes.isEmpty());
    CHECK(h.state() == MfaPromptHandler::State::WaitingForCode);

    // code submitted → WaitingForResult, codeReady carries the code
    h.submitCode(QStringLiteral("123456"));
    CHECK(codes == QStringList{QStringLiteral("123456")});
    CHECK(h.state() == MfaPromptHandler::State::WaitingForResult);

    // rejected → retry with attempt 2, then 3
    h.onPromptDetected(QStringLiteral("Verification code: "));
    CHECK(attempts.last() == 2);
    h.submitCode(QStringLiteral("222222"));
    h.onPromptDetected(QStringLiteral("Verification code: "));
    CHECK(attempts.last() == 3);
    h.submitCode(QStringLiteral("333333"));

    // third rejection exhausts the budget → mfaFailed → Idle
    h.onPromptDetected(QStringLiteral("Verification code: "));
    CHECK(failed == 1);
    CHECK(h.state() == MfaPromptHandler::State::Idle);

    // fresh round: prompt → code → normal output = success (feed path,
    // watcher sees non-prompt data after the grace period)
    h.onPromptDetected(QStringLiteral("[MFA auth]: "));
    h.submitCode(QStringLiteral("654321"));
    h.feed(QByteArrayLiteral("Welcome to Ubuntu 22.04\r\nuser@host:~$ "));
    CHECK(succeeded == 1);
    CHECK(h.state() == MfaPromptHandler::State::Idle);

    // cancel path: dialog dismissed → back to Idle, nothing sent
    h.onPromptDetected(QStringLiteral("Verification code: "));
    CHECK(h.state() == MfaPromptHandler::State::WaitingForCode);
    const int sentBefore = codes.size();
    h.cancelPrompt();
    CHECK(h.state() == MfaPromptHandler::State::Idle);
    h.submitCode(QStringLiteral("999999")); // no active prompt → ignored
    CHECK(codes.size() == sentBefore);
}

// ─────────────────────── Part 2: live-server integration ───────────────────

static void testExecAgainstServer(SshClient &client)
{
    CommandExecutor ex(&client);

    // one-shot command 对应Python: ssh_func.py::exec
    ExecResult r = ex.exec(QStringLiteral("echo cube_exec_ok"));
    CHECK(r.ok());
    CHECK(r.exitCode == 0);
    CHECK(r.stdoutText.contains(QStringLiteral("cube_exec_ok")));

    // exit code propagation 对应Python: recv_exit_status
    r = ex.exec(QStringLiteral("exit 7"));
    CHECK(r.ok());
    CHECK(r.exitCode == 7);

    // stderr is captured separately
    r = ex.exec(QStringLiteral("echo out_here; echo err_here 1>&2"));
    CHECK(r.ok());
    CHECK(r.stdoutText.contains(QStringLiteral("out_here")));
    CHECK(r.stderrText.contains(QStringLiteral("err_here")));
    CHECK(!r.stdoutText.contains(QStringLiteral("err_here")));

    // timeout 对应Python: exec_command(timeout=...)
    r = ex.exec(QStringLiteral("sleep 10"), false, 1500);
    CHECK(r.timedOut);
    CHECK(!r.ok());

    // script upload + execute + cleanup 对应Python: 上传+执行模式
    const QByteArray script =
        "#!/bin/sh\n"
        "echo script_ran_with $1\n"
        "exit 3\n";
    r = ex.execScript(script, {QStringLiteral("argx")});
    CHECK(r.errorMessage.isEmpty());
    CHECK(r.exitCode == 3);
    CHECK(r.stdoutText.contains(QStringLiteral("script_ran_with argx")));
}

static void testExecStreamAgainstServer(SshClient &client)
{
    CommandExecutor ex(&client);

    QList<QByteArray> chunks;
    bool finished = false;
    int exitCode = -99;
    QString streamedOut;
    // 跨线程信号：显式 QueuedConnection（工作线程 → 本线程事件循环）
    QObject::connect(&ex, &CommandExecutor::outputChunk,
                     &ex, [&](const QByteArray &c) { chunks << c; },
                     Qt::QueuedConnection);
    QObject::connect(&ex, &CommandExecutor::streamFinished,
                     &ex, [&](int code, const QString &out, const QString &) {
                         finished = true;
                         exitCode = code;
                         streamedOut = out;
                     },
                     Qt::QueuedConnection);

    // multi-chunk output with \r progress lines (wget/curl style)
    const QString cmd = QStringLiteral(
        "for i in 1 2 3 4 5; do echo chunk_$i; sleep 0.3; done; "
        "printf 'progress 10%%\\rprogress 50%%\\rprogress 100%%\\n'");
    CHECK(ex.execStream(cmd));
    CHECK(ex.isStreaming());
    CHECK(!ex.execStream(QStringLiteral("echo nope"))); // only one stream at a time

    CHECK(waitFor([&]() { return finished; }, 30000));
    ex.waitForFinished();
    CHECK(exitCode == 0);
    // throttled (200ms) but the 0.3s cadence guarantees several emissions
    CHECK(chunks.size() >= 2);
    QByteArray joined;
    for (const QByteArray &c : chunks)
        joined += c;
    CHECK(joined.contains("chunk_1"));
    CHECK(joined.contains("chunk_5"));
    CHECK(joined.contains('\r')); // \r progress bytes forwarded verbatim
    CHECK(streamedOut.contains(QStringLiteral("chunk_5")));

    // cancellation 对应Python: request_stop
    finished = false;
    exitCode = -99;
    CHECK(ex.execStream(QStringLiteral("sleep 30")));
    QElapsedTimer t;
    t.start();
    waitFor([&]() { return t.elapsed() > 500; }, 1000);
    ex.cancel();
    CHECK(waitFor([&]() { return finished; }, 10000));
    ex.waitForFinished();
    CHECK(exitCode == -1);       // cancelled ⇒ no clean exit status
    CHECK(t.elapsed() < 15000);  // did not wait for the full sleep
}

static void testRemoteMonitorAgainstServer(SshClient &client)
{
    RemoteMonitor monitor(&client);
    monitor.setIntervalMs(1000);

    int statsCount = 0;
    RemoteStats last;
    bool infoSeen = false;
    // 跨线程信号：显式 QueuedConnection（监控线程 → 本线程事件循环）
    QObject::connect(&monitor, &RemoteMonitor::statsUpdated,
                     &monitor, [&](const RemoteStats &s) { ++statsCount; last = s; },
                     Qt::QueuedConnection);
    QObject::connect(&monitor, &RemoteMonitor::systemInfoReady,
                     &monitor, [&](const QHash<QString, QString> &) { infoSeen = true; },
                     Qt::QueuedConnection);

    monitor.start();
    CHECK(monitor.isRunning());

    // Two cycles so the second carries CPU/network deltas (each cycle also
    // spends ~1s inside iostat, matching the Python-side commands).
    CHECK(waitFor([&]() { return statsCount >= 2; }, 60000));
    CHECK(infoSeen); // emitted even when hostnamectl is missing (empty hash)
    CHECK(last.memory.total > 0);            // free -m parsed
    CHECK(!last.diskPartitions.isEmpty());   // df -h parsed
    CHECK(last.cpuValid);                    // second cycle has a CPU delta
    CHECK(!last.uptimeText.isEmpty());       // /proc/uptime parsed

    monitor.stop();
    CHECK(!monitor.isRunning());
    // stop() joined the thread — no further signals may arrive
    const int after = statsCount;
    waitFor([&]() { return false; }, 300);
    CHECK(statsCount == after);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Part 1: pure logic (always runs).
    testSudoHelpers();
    testLongRunningDetection();
    testSudoPromptDetection();
    testCancelFlagAndOfflineErrors();
    testMfaStateMachine();
    if (failures != 0) {
        qWarning() << "PURE-LOGIC FAILURES:" << failures;
        return 1;
    }
    qInfo() << "pure-logic part PASS";

    // Part 2: live server (SKIP with exit 2 when unreachable).
    const QString host = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1");
    const quint16 port = quint16(qEnvironmentVariable("CUBESSH_PORT", "2222").toUShort());
    const QString user = qEnvironmentVariable("CUBESSH_USER", "testuser");
    const QString pass = qEnvironmentVariable("CUBESSH_PASS", "testpass123");

    SshClient client;
    client.setHost(host, port);
    client.setUsername(user);
    client.setPassword(pass);

    SshError err;
    if (!client.connectToHost(nullptr, err)) {
        qWarning() << "SKIP network part — connect failed:" << err.message;
        return 2;
    }
    qInfo() << "connected to" << host << port;

    testExecAgainstServer(client);
    testExecStreamAgainstServer(client);
    testRemoteMonitorAgainstServer(client);

    client.disconnectFromHost();

    qInfo() << (failures == 0 ? "COMMAND EXECUTOR ALL PASS" : "COMMAND EXECUTOR FAILURES") << failures;
    return failures == 0 ? 0 : 1;
}
