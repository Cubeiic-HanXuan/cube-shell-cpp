// SshCopyIdWorker.cpp — 见 SshCopyIdWorker.h。

#include "SshCopyIdWorker.h"

#include <QStringList>

#include "ssh/CommandExecutor.h"
#include "ssh/SshClient.h"

namespace cubeshell {

namespace {
// 把任意文本安全包进 POSIX 单引号：' → '\''（先出引号、塞入转义的单引号、再回引号）。
QString shellSingleQuote(const QString &s)
{
    return QLatin1Char('\'') + QString(s).replace(QLatin1Char('\''),
                                                  QStringLiteral("'\\''"))
           + QLatin1Char('\'');
}
} // namespace

SshCopyIdWorker::SshCopyIdWorker(const DeviceEntry &device, const QString &publicKeyLine,
                                 QObject *parent)
    : QThread(parent)
    , m_device(device)
    , m_publicKeyLine(publicKeyLine)
{
}

void SshCopyIdWorker::run()
{
    if (!m_device.isSsh()) {
        emit finishedSignal(false, QStringLiteral("目标设备不是 SSH 协议"), false);
        return;
    }

    // 公钥行的第二段是 base64 blob，去重按它匹配——改注释后重部署仍是 no-op，
    // 与真实 ssh-copy-id 的行为一致。
    const QStringList parts = m_publicKeyLine.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        emit finishedSignal(false, QStringLiteral("公钥格式无效"), false);
        return;
    }
    const QString b64 = parts.at(1);

    if (isInterruptionRequested())
        return;

    // 建连：照 ConnectionTester::testSsh 的客户端装配（含代理与认证方式）。
    SshClient ssh;
    const HostPort hp = m_device.hostPort();
    ssh.setHost(hp.host, hp.port);
    ssh.setUsername(m_device.username);
    ssh.setCredentialKind(m_device.credentialKind);
    if (m_device.usesKey())
        ssh.setPrivateKey(m_device.keyType, m_device.keyFile);
    else
        ssh.setPassword(m_device.password);
    // 内网设备常在代理后面——不带代理会连不上，而真实建连明明走得通。
    ssh.setProxyConfig(m_device.proxy);

    SshError error;
    if (!ssh.connectToHost(nullptr, error)) {
        emit finishedSignal(false,
                            error.message.isEmpty() ? QStringLiteral("连接失败") : error.message,
                            false);
        return;
    }

    if (isInterruptionRequested()) {
        ssh.disconnectFromHost();
        return;
    }

    CommandExecutor exec(&ssh);

    // 1) 备好 ~/.ssh。
    ExecResult r = exec.exec(QStringLiteral("mkdir -p ~/.ssh && chmod 700 ~/.ssh"), false);
    if (!r.ok() || r.exitCode != 0) {
        emit finishedSignal(false,
                            QStringLiteral("初始化 ~/.ssh 失败：%1")
                                .arg(r.stderrText.isEmpty() ? r.errorMessage : r.stderrText),
                            false);
        ssh.disconnectFromHost();
        return;
    }

    // 2) 去重 + 追加。grep -qF 命中即已存在；否则追加并打标记，供结果解析。
    const QString cmd = QStringLiteral(
        "touch ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && "
        "(grep -qF %1 ~/.ssh/authorized_keys 2>/dev/null && echo __CUBE_PRESENT__ || "
        "(echo %2 >> ~/.ssh/authorized_keys && echo __CUBE_ADDED__))")
        .arg(shellSingleQuote(b64), shellSingleQuote(m_publicKeyLine));

    r = exec.exec(cmd, false);
    ssh.disconnectFromHost();

    const QString outText = r.stdoutText;
    if (outText.contains(QStringLiteral("__CUBE_PRESENT__"))) {
        emit finishedSignal(true, QStringLiteral("公钥已存在于 %1，无需重复部署")
                                .arg(m_device.name), true);
    } else if (outText.contains(QStringLiteral("__CUBE_ADDED__"))) {
        emit finishedSignal(true, QStringLiteral("公钥已部署到 %1").arg(m_device.name), false);
    } else {
        emit finishedSignal(false,
                            QStringLiteral("写入 authorized_keys 失败：%1")
                                .arg(r.stderrText.isEmpty() ? r.errorMessage : r.stderrText),
                            false);
    }
}

} // namespace cubeshell
