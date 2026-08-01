#pragma once

// BastionClient.h — JumpServer (bastion host) connection orchestration.
//
// C++ port of core/url_dispatch/bastion_client.py. Turns a parsed
// UrlConnectionInfo into concrete SSH connection parameters (KoKo proxy
// host/port, "JMS-<token>" username, token value as password) and hands
// them to the application through a signal / callback — the C++ analogue of
// Python's BastionClient driving main_window.add_new_tab() +
// _connect_with_qterm_widget().
//
// The Python module has no HTTP API calls (asset info comes entirely from
// the URL payload), so no QNetworkAccessManager is needed here.

#include <QObject>
#include <QString>
#include <QStringList>

#include "url_dispatch/UrlHandler.h"

namespace cubeshell {

// The final SSH connection parameters BastionClient derives from a
// connection_info. Field names map to the arguments Python passes to
// main_window._connect_with_qterm_widget(host, port, user, password,
// key_type, key_file, terminal) plus the tab title.
// 对应Python: core/url_dispatch/bastion_client.py::BastionClient.auto_connect
struct BastionConnectParams {
    bool valid = false;  // false == Python auto_connect returning early
    QString error;       // reason when valid == false

    QString host;
    int     port = 22;
    QString user;        // may be empty (Python passes user or '')
    QString password;    // may be empty
    QString keyType;     // 'key_type', normally empty on URL flows
    QString keyFile;     // 'key_file'
    QString tabName;     // "user@host" if user else host
};

// 对应Python: core/url_dispatch/bastion_client.py::scan_argv_for_url
// Scans argv (excluding argv[0]) for a jms:// / ssh:// / cubeshell:// URL,
// either as a bare argument or following a "-url" flag (macOS .app launch).
// Returns an invalid UrlConnectionInfo when nothing matches.
UrlConnectionInfo scanArgvForUrl(const QStringList &argv);

class BastionClient : public QObject {
    Q_OBJECT
public:
    // Python 版持有 main_window 引用直接操作 UI；C++ 版通过 connectRequested
    // 信号解耦，由上层（主窗口）连接该信号并创建 Tab / 发起 SSH 连接。
    // 对应Python: core/url_dispatch/bastion_client.py::BastionClient.__init__
    explicit BastionClient(QObject *parent = nullptr);

    // 对应Python: core/url_dispatch/bastion_client.py::BastionClient.handle_url
    // Parses a jms:// or ssh:// URL and, on success, requests an automatic
    // connection. Other schemes are ignored (mirrors Python's if/elif).
    // Returns true when a connection was requested.
    bool handleUrl(const QString &url);

    // 对应Python: core/url_dispatch/bastion_client.py::BastionClient.auto_connect
    // Emits connectRequested() with the resolved parameters when they are
    // valid. Returns the resolved params either way (for tests / callers).
    BastionConnectParams autoConnect(const UrlConnectionInfo &info);

    // Pure parameter resolution, split out from auto_connect so it can be
    // unit-tested without a UI:
    //   * JumpServer token mode (no host, but token + server present):
    //     host = urlparse(server).hostname, port = 2222 (KoKo SSH proxy),
    //     user = "JMS-<token>", password = "".
    //   * otherwise pass host/port/user/password/key_* through unchanged.
    //   * no resolvable host -> invalid (Python logs and returns).
    // 对应Python: core/url_dispatch/bastion_client.py::BastionClient.auto_connect (参数解析部分)
    static BastionConnectParams resolveConnectParams(const UrlConnectionInfo &info);

signals:
    // Fired when auto_connect resolved valid parameters. The receiver is
    // responsible for the UI side (new tab, connecting state, SSH connect,
    // 10 s timeout guard) that Python's auto_connect performs inline.
    void connectRequested(const cubeshell::BastionConnectParams &params);
};

} // namespace cubeshell
