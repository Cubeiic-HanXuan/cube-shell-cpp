// BastionClient.cpp — JumpServer connection orchestration. See BastionClient.h.
//
// 对应Python: core/url_dispatch/bastion_client.py

#include "BastionClient.h"

#include <QUrl>

namespace cubeshell {

// 对应Python: core/url_dispatch/bastion_client.py::scan_argv_for_url
UrlConnectionInfo scanArgvForUrl(const QStringList &argv)
{
    auto isSchemeUrl = [](const QString &s) {
        if (s.startsWith(QLatin1String("jms://"))
                || s.startsWith(QLatin1String("ssh://"))
                || s.startsWith(QLatin1String("cubeshell://")))
            return true;
#ifdef CUBESHELL_WITH_RDP
        // rdp:// / rdp+ntlm-password://（命令行直开 RDP 标签，由主窗口分发）
        if (s.startsWith(QLatin1String("rdp://")) || s.startsWith(QLatin1String("rdp+")))
            return true;
#endif
        return false;
    };

    // argv[1:] — 跳过程序名
    for (int i = 1; i < argv.size(); ++i) {
        const QString &arg = argv.at(i);
        if (isSchemeUrl(arg))
            return parseUrl(arg);
        // 处理 -url flag 格式 (macOS .app 传参)
        if (arg == QLatin1String("-url") && i + 1 < argv.size()) {
            const QString &urlVal = argv.at(i + 1);
            if (isSchemeUrl(urlVal))
                return parseUrl(urlVal);
        }
    }

    UrlConnectionInfo none;
    none.error = QStringLiteral("no URL found in argv");
    return none;
}

BastionClient::BastionClient(QObject *parent)
    : QObject(parent)
{
}

// 对应Python: core/url_dispatch/bastion_client.py::BastionClient.handle_url
bool BastionClient::handleUrl(const QString &url)
{
    UrlConnectionInfo info;
    if (url.startsWith(QLatin1String("jms://")))
        info = parseJmsUrl(url);
    else if (url.startsWith(QLatin1String("ssh://")))
        info = parseSshUrl(url);

    if (!info.valid)
        return false;
    return autoConnect(info).valid;
}

// 对应Python: core/url_dispatch/bastion_client.py::BastionClient.auto_connect (参数解析部分)
BastionConnectParams BastionClient::resolveConnectParams(const UrlConnectionInfo &info)
{
    BastionConnectParams params;
    if (!info.valid) {
        params.error = QStringLiteral("no connection info");
        return params;
    }

    QString host = info.host;
    int port = info.port;
    QString user = info.user;
    QString password = info.password;

    // JumpServer token 模式：通过 token 连接 JumpServer SSH 代理（koko）。
    // 仅在 legacy query-string 结果中出现：无 host 但有 token + server。
    if (host.isEmpty() && !info.token.isEmpty() && !info.server.isEmpty()) {
        // urlparse(server).hostname — server 需是完整 URL（如 https://jms.example.com）
        const QUrl serverUrl(info.server, QUrl::TolerantMode);
        host = serverUrl.host();
        if (host.isEmpty()) {
            params.error = QStringLiteral("Failed to parse JumpServer server URL: ") + info.server;
            return params;
        }
        port = 2222; // JumpServer koko SSH 代理默认端口
        user = QStringLiteral("JMS-") + info.token;
        password.clear();
    }

    if (host.isEmpty()) {
        params.error = QStringLiteral("auto_connect: no host provided, skipping");
        return params;
    }

    params.host = host;
    params.port = port;
    params.user = user;         // Python: user or ''
    params.password = password; // Python: password or ''
    params.keyType = info.keyType;
    params.keyFile = info.keyFile;
    // 构造 Tab 名称: f"{user}@{host}" if user else host
    params.tabName = user.isEmpty() ? host
                                    : QStringLiteral("%1@%2").arg(user, host);
    params.valid = true;
    return params;
}

// 对应Python: core/url_dispatch/bastion_client.py::BastionClient.auto_connect
BastionConnectParams BastionClient::autoConnect(const UrlConnectionInfo &info)
{
    const BastionConnectParams params = resolveConnectParams(info);
    // Tab 创建/连接状态/SSH 发起/10s 超时保护由信号接收方（主窗口）负责。
    if (params.valid)
        emit connectRequested(params);
    return params;
}

} // namespace cubeshell
