// agent_forward_test.cpp — agent forwarding 回归测试（headless）。
//
// 背景：在开了 agent forwarding 的 shell 里执行 ssh-add -l，sshd 会回开一条
// "auth-agent@openssh.com" 通道。libssh2 在处理这个入站包时给
// libssh2_channel_read 返回了真实错误码，SshClient::readChannel 把它和 EOF
// 一起归类成"空数据"，bridge 随即判定 channel closed，UI 弹出"连接已断开"
// 覆盖层——shell 其实还活着。
//
// 本测试对着 tests/docker/ssh-enhance 环境（127.0.0.1:2401）复现该序列：
//   开 shell（agent forwarding on）→ 执行 ssh-add -l → 之后必须还能执行命令。
//
// 环境变量：CUBESSH_AGENT_PORT（默认 2401）/ CUBESSH_USER / CUBESSH_PASS。
// 端口不通 → exit 2（SKIP，与 ssh_integration_test 同一约定）。

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>

#include "ssh/SshClient.h"

#include <QTcpSocket>

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const quint16 port = quint16(qEnvironmentVariableIntValue("CUBESSH_AGENT_PORT") ?: 2401);
    const QString user = qEnvironmentVariable("CUBESSH_USER", QStringLiteral("testuser"));
    const QString pass = qEnvironmentVariable("CUBESSH_PASS", QStringLiteral("testpass123"));

    {
        QTcpSocket probe;
        probe.connectToHost(QStringLiteral("127.0.0.1"), port);
        if (!probe.waitForConnected(1500)) {
            qWarning() << "agent_forward_test: 127.0.0.1:" << port
                       << "unreachable — SKIP (start tests/docker/ssh-enhance)";
            return 2;
        }
    }

    SshClient client;
    client.setHost(QStringLiteral("127.0.0.1"), port);
    client.setUsername(user);
    // CUBESSH_AGENT_AUTH=1 时走 ssh-agent 认证（宿主机 agent 里要有测试密钥），
    // 否则密码认证。两条路径都要过——用户报告两种登录下现象不同。
    if (qEnvironmentVariableIntValue("CUBESSH_AGENT_AUTH"))
        client.setCredentialKind(SshCredentialKind::SshAgent);
    else
        client.setPassword(pass);
    client.setAgentForwarding(true);

    SshError err;
    if (!client.connectToHost(nullptr, err)) {
        qWarning() << "connect failed:" << err.message;
        return 1;
    }
    if (!client.openShell("xterm-256color", 120, 30, err)) {
        qWarning() << "openShell failed:" << err.message;
        return 1;
    }

    // 从 shell 读直到看到 marker 或超时。返回 false = 通道提前 EOF/出错。
    const auto readUntil = [&client](const QByteArray &marker, int timeoutMs,
                                     QByteArray *acc) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            bool wouldBlock = false;
            const QByteArray chunk = client.readChannel(4096, &wouldBlock);
            if (wouldBlock) {
                client.waitReadable(100);
                continue;
            }
            if (chunk.isEmpty())
                return false;   // EOF / 读错误——正是要回归的故障
            *acc += chunk;
            if (acc->contains(marker))
                return true;
        }
        return acc->contains(marker);
    };

    QByteArray buf;
    client.writeChannel("echo READY-$?\n");
    CHECK(readUntil("READY-0", 10000, &buf));

    // 触发 sshd 回开 auth-agent@openssh.com 通道。agent forwarding 被请求但
    // 客户端没有 authagent 回调时，libssh2 会回 open-failure——远端 ssh-add
    // 报错无所谓，**shell 通道不许死**。
    //
    // 注意 marker 写法：marker 拆成两段（"AGENT""_RC"），否则终端的命令回显
    // 里就带着完整 marker，readUntil 会在命令真正执行前就误命中。
    buf.clear();
    client.writeChannel("echo SOCK=[$SSH_AUTH_SOCK]; ssh-add -l </dev/null; echo \"AGENT\"\"_RC-$?\"\n");
    const bool gotRc = readUntil("AGENT_RC-", 15000, &buf);
    CHECK(gotRc);
    if (!gotRc) {
        qWarning() << "channel died after ssh-add -l; transport alive ="
                   << client.isTransportAlive();
    }
    qDebug() << "--- ssh-add output:" << QString::fromUtf8(buf);

    // 二级跳：经转发的 agent 从 target 再 ssh 到 target-noagent。
    // 没转发权限时这条会认证失败（rc != 0），同样不许弄死 shell 通道。
    if (gotRc) {
        buf.clear();
        client.writeChannel("ssh -o StrictHostKeyChecking=no -o BatchMode=yes "
                            "-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 "
                            "target-noagent hostname; echo \"HOP\"\"_RC-$?\"\n");
        const bool gotHop = readUntil("HOP_RC-", 20000, &buf);
        CHECK(gotHop);
        qDebug() << "--- hop output:" << QString::fromUtf8(buf);
    }

    // 再确认 shell 还能正常干活。
    buf.clear();
    client.writeChannel("echo \"STILL\"\"-ALIVE\"\n");
    CHECK(readUntil("STILL-ALIVE", 10000, &buf));

    client.disconnectFromHost();

    if (failures) {
        qWarning() << "agent_forward_test:" << failures << "failure(s)";
        return 1;
    }
    qDebug() << "agent_forward_test: all passed";
    return 0;
}
