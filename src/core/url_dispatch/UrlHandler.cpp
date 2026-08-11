// UrlHandler.cpp — URL scheme parsing. See UrlHandler.h.
//
// 对应Python: core/url_dispatch/url_handler.py
//
// Error-path fidelity note. The Python parser has two distinct failure modes
// inside the Base64 branch, and we reproduce both:
//   * "soft" failure (invalid Base64 / invalid JSON / endpoint-or-token falsy)
//     -> _parse_jms_base64 returns None -> parse_jms_url falls back to the
//     legacy query-string parser;
//   * "hard" failure (payload IS valid Base64 JSON but endpoint['host'] /
//     token['id'] / token['value'] raise KeyError, int(port) raises, or a
//     field has the wrong type) -> the exception escapes to parse_jms_url's
//     `except Exception` -> the whole parse returns None (no legacy fallback).

#include "UrlHandler.h"

#include <QByteArray>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStringList>
#include <QUrl>

#include "config/DeviceConfigStore.h"   // defaultPortFor

namespace cubeshell {

namespace {

// Python truthiness for a JSON value (`if not endpoint or not token`).
bool isTruthy(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Bool:   return v.toBool();
    case QJsonValue::Double: return v.toDouble() != 0.0;
    case QJsonValue::String: return !v.toString().isEmpty();
    case QJsonValue::Array:  return !v.toArray().isEmpty();
    case QJsonValue::Object: return !v.toObject().isEmpty();
    default:                 return false; // Null / Undefined
    }
}

// Stringify a JSON scalar the way Python's f-string / dict passthrough does
// (numbers -> "123", null -> "" since None and '' both map to empty QString).
QString jsonToString(const QJsonValue &v)
{
    return v.toVariant().toString();
}

// 对应Python: urllib.parse.unquote_plus (parse_qs 内部对 key/value 的解码)
QString unquotePlus(const QString &s)
{
    QByteArray raw = s.toUtf8();
    raw.replace('+', ' ');
    return QUrl::fromPercentEncoding(raw);
}

// 对应Python: urllib.parse.parse_qs + 取每个 key 的首个值 ([0])
// parse_qs defaults (keep_blank_values=False): pairs without '=' and pairs
// with an empty value are skipped entirely.
QHash<QString, QString> parseQueryFirstValues(const QString &queryString)
{
    QHash<QString, QString> out;
    const QStringList pairs = queryString.split(QLatin1Char('&'), Qt::SkipEmptyParts);
    for (const QString &pair : pairs) {
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue; // bare name, skipped by parse_qs
        const QString rawValue = pair.mid(eq + 1);
        if (rawValue.isEmpty())
            continue; // blank value, skipped by parse_qs
        const QString key = unquotePlus(pair.left(eq));
        if (!out.contains(key))
            out.insert(key, unquotePlus(rawValue)); // keep first value ([0])
    }
    return out;
}

enum class Base64ParseStatus {
    Ok,       // valid v2 payload, `out` filled
    SoftFail, // not Base64 JSON / endpoint-token falsy -> try legacy fallback
    HardFail  // structurally broken v2 payload -> whole parse fails
};

// 对应Python: core/url_dispatch/url_handler.py::_parse_jms_base64
Base64ParseStatus parseJmsBase64(const QString &payload, UrlConnectionInfo &out)
{
    // 补齐 Base64 padding（浏览器/JS 侧可能省略 '='）
    QByteArray raw = payload.toUtf8();
    const int missingPadding = raw.size() % 4;
    if (missingPadding)
        raw.append(4 - missingPadding, '=');

    // validate=True 对应 AbortOnBase64DecodingErrors（拒绝非字母表字符）
    const auto decodedResult = QByteArray::fromBase64Encoding(
        raw, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decodedResult)
        return Base64ParseStatus::SoftFail; // base64.binascii.Error

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(*decodedResult, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return Base64ParseStatus::SoftFail; // JSONDecodeError / UnicodeDecodeError
    if (!doc.isObject()) {
        // Python: data.get(...) on a list raises AttributeError -> hard fail.
        out.error = QStringLiteral("JMS payload JSON is not an object");
        return Base64ParseStatus::HardFail;
    }
    const QJsonObject data = doc.object();

    // 必须包含 endpoint 和 token 字段才认为是有效的 v2 格式
    const QJsonValue endpointV = data.value(QLatin1String("endpoint"));
    const QJsonValue tokenV = data.value(QLatin1String("token"));
    if (!isTruthy(endpointV) || !isTruthy(tokenV))
        return Base64ParseStatus::SoftFail;
    if (!endpointV.isObject() || !tokenV.isObject()) {
        // Python: endpoint['host'] on a non-dict raises TypeError -> hard fail.
        out.error = QStringLiteral("JMS 'endpoint'/'token' is not an object");
        return Base64ParseStatus::HardFail;
    }
    const QJsonObject endpoint = endpointV.toObject();
    const QJsonObject token = tokenV.toObject();

    // KeyError paths: endpoint['host'], token['id'], token['value']
    if (!endpoint.contains(QLatin1String("host"))) {
        out.error = QStringLiteral("JMS endpoint missing 'host'");
        return Base64ParseStatus::HardFail;
    }
    if (!token.contains(QLatin1String("id")) || !token.contains(QLatin1String("value"))) {
        out.error = QStringLiteral("JMS token missing 'id' or 'value'");
        return Base64ParseStatus::HardFail;
    }

    // port: int(endpoint.get('port', 2222)) — 非法值对应 ValueError/TypeError -> hard fail
    int port = 2222;
    const QJsonValue portV = endpoint.value(QLatin1String("port"));
    if (!portV.isUndefined()) {
        if (portV.isDouble()) {
            port = int(portV.toDouble());
        } else if (portV.isBool()) {
            port = portV.toBool() ? 1 : 0; // int(True/False)
        } else if (portV.isString()) {
            bool ok = false;
            port = portV.toString().trimmed().toInt(&ok);
            if (!ok) {
                out.error = QStringLiteral("JMS endpoint 'port' is not a number");
                return Base64ParseStatus::HardFail;
            }
        } else {
            out.error = QStringLiteral("JMS endpoint 'port' is not a number");
            return Base64ParseStatus::HardFail;
        }
    }

    // asset = data.get('asset', {}) — 存在但不是对象时 Python 会 AttributeError
    QString assetName;
    const QJsonValue assetV = data.value(QLatin1String("asset"));
    if (assetV.isObject()) {
        assetName = jsonToString(assetV.toObject().value(QLatin1String("name")));
    } else if (!assetV.isUndefined()) {
        out.error = QStringLiteral("JMS 'asset' is not an object");
        return Base64ParseStatus::HardFail;
    }

    out.host = jsonToString(endpoint.value(QLatin1String("host")));
    out.port = port;
    out.user = QStringLiteral("JMS-") + jsonToString(token.value(QLatin1String("id")));
    out.password = jsonToString(token.value(QLatin1String("value")));
    out.assetName = assetName;
    out.protocol = data.contains(QLatin1String("protocol"))
                       ? jsonToString(data.value(QLatin1String("protocol")))
                       : QStringLiteral("ssh");
    return Base64ParseStatus::Ok;
}

// 对应Python: core/url_dispatch/url_handler.py::_parse_jms_query_string
// The legacy parser never fails outright in Python except int(port) raising;
// with all params missing it still returns a dict full of Nones.
UrlConnectionInfo parseJmsQueryString(const QString &withoutScheme)
{
    UrlConnectionInfo out;
    out.scheme = QStringLiteral("jms");

    // 分离路径和查询参数
    QString pathPart;
    QString queryString;
    const int q = withoutScheme.indexOf(QLatin1Char('?'));
    if (q >= 0) {
        pathPart = withoutScheme.left(q);
        queryString = withoutScheme.mid(q + 1);
    } else {
        pathPart = withoutScheme;
    }

    const QHash<QString, QString> params = parseQueryFirstValues(queryString);

    // 提取路径中的信息: "asset/<id>"
    if (pathPart.contains(QLatin1Char('/'))) {
        const QStringList parts = pathPart.split(QLatin1Char('/'));
        if (parts.at(0) == QLatin1String("asset") && parts.size() > 1)
            out.assetId = parts.at(1);
    }

    out.token = params.value(QLatin1String("token"));
    out.server = params.value(QLatin1String("server"));
    out.host = params.value(QLatin1String("host"));
    // int(params.get('port', ['22'])[0]) — 非数字对应 ValueError -> 整体解析失败
    if (params.contains(QLatin1String("port"))) {
        bool ok = false;
        const int p = params.value(QLatin1String("port")).trimmed().toInt(&ok);
        if (!ok) {
            out.valid = false;
            out.error = QStringLiteral("JMS legacy 'port' is not a number");
            return out;
        }
        out.port = p;
    } else {
        out.port = 22;
    }
    // params.get('user', [params.get('account', [None])[0]])[0]
    out.user = params.contains(QLatin1String("user"))
                   ? params.value(QLatin1String("user"))
                   : params.value(QLatin1String("account"));
    out.password = params.value(QLatin1String("password"));
    out.protocol = params.contains(QLatin1String("protocol"))
                       ? params.value(QLatin1String("protocol"))
                       : QStringLiteral("ssh");
    out.valid = true;
    return out;
}

} // namespace

// 对应Python: core/url_dispatch/url_handler.py::parse_jms_url
UrlConnectionInfo parseJmsUrl(const QString &url)
{
    UrlConnectionInfo out;
    out.scheme = QStringLiteral("jms");
    if (!url.startsWith(QLatin1String("jms://"))) {
        out.error = QStringLiteral("not a jms:// URL");
        return out;
    }

    // 去掉 'jms://' 及浏览器自动追加的尾部斜线 (url[6:].rstrip('/'))
    QString withoutScheme = url.mid(6);
    while (withoutScheme.endsWith(QLatin1Char('/')))
        withoutScheme.chop(1);

    // 尝试 Base64 JSON 格式（v2）。Python 先在无 '?'/'/' 时尝试一次、随后无条件
    // 再尝试一次——两次调用的是同一函数，净效果等价于单次尝试。
    UrlConnectionInfo v2;
    v2.scheme = QStringLiteral("jms");
    switch (parseJmsBase64(withoutScheme, v2)) {
    case Base64ParseStatus::Ok:
        v2.valid = true;
        return v2;
    case Base64ParseStatus::HardFail:
        // 对应 parse_jms_url 的 except Exception -> return None（不走 legacy 回退）
        out.error = v2.error;
        return out;
    case Base64ParseStatus::SoftFail:
        break;
    }

    // Fallback: 旧格式 query string 解析
    return parseJmsQueryString(withoutScheme);
}

// 对应Python: core/url_dispatch/url_handler.py::parse_ssh_url
UrlConnectionInfo parseSshUrl(const QString &url)
{
    UrlConnectionInfo out;
    out.scheme = QStringLiteral("ssh");
    if (!url.startsWith(QLatin1String("ssh://"))) {
        out.error = QStringLiteral("not an ssh:// URL");
        return out;
    }

    const QUrl parsed(url, QUrl::TolerantMode);
    if (!parsed.isValid()) {
        out.error = QStringLiteral("failed to parse SSH URL: ") + parsed.errorString();
        return out;
    }
    const QString host = parsed.host();
    if (host.isEmpty()) {
        out.error = QStringLiteral("No host found in SSH URL");
        return out;
    }

    out.host = host;
    out.port = parsed.port(22); // parsed.port or 22
    // urllib.parse.unquote(user/password)
    out.user = parsed.userName(QUrl::FullyDecoded);
    out.password = parsed.password(QUrl::FullyDecoded);
    out.valid = true;
    return out;
}

// telnet:// URL 解析（Python 侧无对应实现，为 C++ 侧新增）。
// telnet 是 IANA 在案的标准 scheme，形态与 ssh:// 完全一致，只有默认端口不同。
UrlConnectionInfo parseTelnetUrl(const QString &url)
{
    UrlConnectionInfo out;
    out.scheme = QStringLiteral("telnet");
    if (!url.startsWith(QLatin1String("telnet://"))) {
        out.error = QStringLiteral("not a telnet:// URL");
        return out;
    }

    const QUrl parsed(url, QUrl::TolerantMode);
    if (!parsed.isValid()) {
        out.error = QStringLiteral("failed to parse Telnet URL: ") + parsed.errorString();
        return out;
    }
    const QString host = parsed.host();   // IPv6 方括号由 QUrl 剥除
    if (host.isEmpty()) {
        out.error = QStringLiteral("No host found in Telnet URL");
        return out;
    }

    out.host = host;
    out.port = parsed.port(defaultPortFor(QStringLiteral("telnet")));
    out.user = parsed.userName(QUrl::FullyDecoded);
    out.password = parsed.password(QUrl::FullyDecoded);
    out.protocol = QStringLiteral("telnet");
    out.valid = true;
    return out;
}

// 对应Python: core/url_dispatch/url_handler.py::parse_cubeshell_url
UrlConnectionInfo parseCubeshellUrl(const QString &url)
{
    UrlConnectionInfo out;
    out.scheme = QStringLiteral("cubeshell");
    if (!url.startsWith(QLatin1String("cubeshell://"))) {
        out.error = QStringLiteral("not a cubeshell:// URL");
        return out;
    }

    const QUrl parsed(url, QUrl::TolerantMode);
    out.action = parsed.host(); // parsed.netloc, e.g. 'open-local'

    // parse_qs 先解一次码，随后 Python 又对取到的值 unquote 一次（双重解码，
    // 无 '+' 处理）——这里保持同样的双重解码行为。
    const QHash<QString, QString> params =
        parseQueryFirstValues(parsed.query(QUrl::FullyEncoded));

    if (!params.contains(QLatin1String("path"))) {
        out.error = QStringLiteral("Missing 'path' parameter in cubeshell URL");
        return out;
    }
    const QString path =
        QUrl::fromPercentEncoding(params.value(QLatin1String("path")).toUtf8());

    // 验证路径存在且为目录
    if (!QFileInfo::exists(path)) {
        out.error = QStringLiteral("Path does not exist: ") + path;
        return out;
    }
    if (!QFileInfo(path).isDir()) {
        out.error = QStringLiteral("Path is not a directory: ") + path;
        return out;
    }

    out.path = path;
    if (params.contains(QLatin1String("command")))
        out.command = QUrl::fromPercentEncoding(params.value(QLatin1String("command")).toUtf8());
    out.valid = true;
    return out;
}

#ifdef CUBESHELL_WITH_RDP
// 对应Python: core/rdp/rdp_client.py::build_rdp_url 的逆向解析（见 UrlHandler.h）
UrlConnectionInfo parseRdpUrl(const QString &url)
{
    UrlConnectionInfo out;
    out.scheme = QStringLiteral("rdp");
    // 接受 rdp:// 及 rdp+<auth>://（build_rdp_url 的 "rdp+ntlm-password" 等变体）
    if (!url.startsWith(QLatin1String("rdp://"))
            && !(url.startsWith(QLatin1String("rdp+"))
                 && url.contains(QLatin1String("://")))) {
        out.error = QStringLiteral("not an rdp:// URL");
        return out;
    }

    // build_rdp_url 在 userinfo 里保留裸反斜杠（DOMAIN\user，quote safe="\\"），
    // QUrl 不接受，先转成 %5C 再解析（密码/主机段不会出现裸反斜杠）。
    QString normalized = url;
    normalized.replace(QLatin1Char('\\'), QLatin1String("%5C"));

    const QUrl parsed(normalized, QUrl::TolerantMode);
    if (!parsed.isValid()) {
        out.error = QStringLiteral("failed to parse RDP URL: ") + parsed.errorString();
        return out;
    }
    const QString host = parsed.host();   // IPv6 方括号由 QUrl 剥除
    if (host.isEmpty()) {
        out.error = QStringLiteral("No host found in RDP URL");
        return out;
    }

    out.host = host;
    out.port = parsed.port(defaultPortFor(QStringLiteral("rdp")));
    QString user = parsed.userName(QUrl::FullyDecoded);
    // DOMAIN\user → 拆出域（对应 build_rdp_url 的 f"{domain}\\{username}"）
    const int backslash = user.indexOf(QLatin1Char('\\'));
    if (backslash >= 0) {
        out.domain = user.left(backslash);
        user = user.mid(backslash + 1);
    }
    out.user = user;
    out.password = parsed.password(QUrl::FullyDecoded);
    out.protocol = QStringLiteral("rdp");
    out.valid = true;
    return out;
}
#endif // CUBESHELL_WITH_RDP

// 对应Python: core/url_dispatch/url_handler.py::resolve_connection_info (URL 分支)
UrlConnectionInfo parseUrl(const QString &url)
{
    if (url.startsWith(QLatin1String("jms://")))
        return parseJmsUrl(url);
    if (url.startsWith(QLatin1String("ssh://")))
        return parseSshUrl(url);
    if (url.startsWith(QLatin1String("cubeshell://")))
        return parseCubeshellUrl(url);
    if (url.startsWith(QLatin1String("telnet://")))
        return parseTelnetUrl(url);
#ifdef CUBESHELL_WITH_RDP
    if (url.startsWith(QLatin1String("rdp://")) || url.startsWith(QLatin1String("rdp+")))
        return parseRdpUrl(url);
#endif

    UrlConnectionInfo out;
    out.error = QStringLiteral("Unsupported URL scheme: ") + url;
    return out;
}

// 对应Python: bastion_client.py::UrlEventFilter 里的 url.startswith(...) 判断
bool isSupportedUrlScheme(const QString &url)
{
    // 与 parseUrl 的分支一一对应；改一处务必改另一处。
    if (url.startsWith(QLatin1String("jms://"))
            || url.startsWith(QLatin1String("ssh://"))
            || url.startsWith(QLatin1String("cubeshell://"))
            || url.startsWith(QLatin1String("telnet://")))
        return true;
#ifdef CUBESHELL_WITH_RDP
    if (url.startsWith(QLatin1String("rdp://")) || url.startsWith(QLatin1String("rdp+")))
        return true;
#endif
    return false;
}

// 对应Python: cube-shell.py 启动段“Windows: 如果参数是本地目录路径（非 URL）”分支
QString directoryArgumentAsUrl(const QStringList &args)
{
    for (int i = 1; i < args.size(); ++i) {   // argv[1:] — 跳过程序名
        const QString &arg = args.at(i);
        if (arg.startsWith(QLatin1Char('-')))   // 跳过选项（-url 等）
            continue;
        if (!QFileInfo(arg).isDir())
            continue;
        // safe="/" 与 workflow 内的 python3 urllib.parse.quote(..., safe='/') 一致，
        // 保留路径分隔符、转义空格等字符（parseCubeshellUrl 会解回来）。
        return QStringLiteral("cubeshell://open-local?path=")
            + QString::fromUtf8(QUrl::toPercentEncoding(arg, "/"));
    }
    return QString();
}

} // namespace cubeshell
