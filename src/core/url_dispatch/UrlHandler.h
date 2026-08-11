#pragma once

// UrlHandler.h — URL scheme parsing (jms:// / ssh:// / cubeshell://).
//
// C++ port of core/url_dispatch/url_handler.py. Parses JumpServer deep-link
// URLs (Base64-JSON v2 format and legacy query-string format), plain ssh://
// URLs and cubeshell:// local-terminal URLs into a structured connection
// request. Pure parsing logic — no UI, no network, no system registration
// (URL scheme registration is Phase 6).
//
// Python-truthiness note: the Python side returns dicts whose values may be
// None or ''. Both map to an empty QString here, which matches how the
// Python consumers test the fields (`if not host: ...`).

#include <QString>

namespace cubeshell {

// Structured result of parsing a URL. Field names map 1:1 to the keys of the
// connection_info dict produced by url_handler.py (see the per-field notes).
// 对应Python: core/url_dispatch/url_handler.py (parse_* 返回的 connection_info dict)
struct UrlConnectionInfo {
    // Parse outcome. valid == false mirrors the Python functions returning
    // None; `error` then holds a human-readable reason (Python only logs it).
    bool valid = false;
    QString error;

    // Which scheme produced this result: "jms" | "ssh" | "telnet" | "cubeshell".
    // (Python: dict 里仅 cubeshell 有 'scheme' 键; 这里统一填充便于分发。)
    QString scheme;

    // --- common connection fields ---
    QString host;        // 'host'
    int     port = 22;   // 'port' (jms v2 default 2222 via endpoint, else 22)
    QString user;        // 'user' (jms v2: "JMS-<token.id>")
    QString password;    // 'password' (jms v2: token.value)
    QString protocol;    // 'protocol' (default "ssh" on jms paths)

    // --- jms:// v2 (Base64 JSON) extras ---
    QString assetName;   // 'asset_name' (asset.name, may be empty)

    // --- jms:// legacy (query string) extras ---
    QString token;       // 'token'
    QString server;      // 'server' (JumpServer base URL, used in token mode)
    QString assetId;     // 'asset_id' (from path "asset/<id>")

    // --- key auth passthrough (never set by URL parsing; kept so the struct
    //     can also carry --host/--user CLI style info like resolve_connection_info) ---
    QString keyType;     // 'key_type'
    QString keyFile;     // 'key_file'

    // --- rdp:// extras（仅 parseRdpUrl 填充；字段本身不做条件编译，
    //     避免 CUBESHELL_WITH_RDP 开关影响结构体布局） ---
    QString domain;      // 'domain'（userinfo 中 DOMAIN\user 形式的域前缀）

    // --- cubeshell:// fields ---
    QString action;      // 'action' (e.g. "open-local")
    QString path;        // 'path' (decoded local directory)
    QString command;     // 'command' (optional command to run, may be empty)
};

// 对应Python: core/url_dispatch/url_handler.py::parse_jms_url
// Parses a JumpServer deep link. Two formats:
//   1. Base64 JSON (v2):        jms://<base64_encoded_json>
//   2. legacy query string:     jms://ssh?token=<token>&server=<server>
// Strips the scheme and any trailing '/' the browser appends, pads the
// Base64 payload, and falls back to the legacy parser when the payload is
// not valid Base64 JSON. A payload that *is* valid Base64 JSON but lacks
// required v2 fields (endpoint.host / token.id / token.value) is a hard
// error (mirrors Python raising KeyError out of _parse_jms_base64).
UrlConnectionInfo parseJmsUrl(const QString &url);

// 对应Python: core/url_dispatch/url_handler.py::parse_ssh_url
// Parses ssh://[user[:password]@]host[:port]; user/password are
// percent-decoded. Fails (valid == false) when no host is present.
UrlConnectionInfo parseSshUrl(const QString &url);

// 对应Python: core/url_dispatch/url_handler.py::parse_cubeshell_url
// Parses cubeshell://<action>?path=<dir>[&command=<cmd>]. Fails when the
// 'path' parameter is missing, does not exist, or is not a directory.
UrlConnectionInfo parseCubeshellUrl(const QString &url);

// telnet:// URL 解析（Python 侧无对应实现，为 C++ 侧新增）。
// 解析 telnet://[user[:password]@]host[:port]；形态与 parseSshUrl 一致，
// 只是默认端口 23、protocol 填 "telnet"。无 host 时失败（valid == false）。
// 不做条件编译：TCP/Telnet 无额外依赖，所有平台（含鸿蒙）都可用。
UrlConnectionInfo parseTelnetUrl(const QString &url);

#ifdef CUBESHELL_WITH_RDP
// rdp:// URL 解析（Python 侧无对应 parse 函数——URL 由 core/rdp/rdp_client.py::
// build_rdp_url 构造后直接交 aardwolf；C++ 侧补齐逆向解析用于 URL Scheme 分发）。
// 支持 rdp://user:pwd@host:port 与 rdp+ntlm-password://domain\user:pwd@host:port
//（及其他 rdp+<auth>:// 变体）；userinfo 支持 DOMAIN\user 形式（域填入 domain
// 字段），user/password percent-decode，端口缺省 3389，IPv6 主机带方括号。
UrlConnectionInfo parseRdpUrl(const QString &url);
#endif

// 对应Python: core/url_dispatch/url_handler.py::resolve_connection_info (URL 分支)
// Dispatches on the scheme prefix; unsupported schemes yield an invalid
// result with an error message.
UrlConnectionInfo parseUrl(const QString &url);

// 把命令行里的裸目录路径参数转成 cubeshell://open-local?path=<encoded>。
// Windows 右键菜单的注册表 command 模板传的是 "%1" / "%V"（目录路径本身，
// 不是 URL），需先归一化再交给统一的 URL 分发。无目录参数时返回空串。
// 对应Python: cube-shell.py 启动段“Windows: 如果参数是本地目录路径（非 URL）”分支
QString directoryArgumentAsUrl(const QStringList &args);

} // namespace cubeshell
