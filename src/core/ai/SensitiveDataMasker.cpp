// SensitiveDataMasker.cpp — see SensitiveDataMasker.h.

#include "SensitiveDataMasker.h"

#include <QRegularExpression>

namespace cubeshell {

namespace {

// 私钥 / 证书 PEM 块：从 BEGIN 到 END 整段替换（含中间 base64 体）。
// DotMatchesEverything 让 "." 跨行，非贪婪确保只到最近一个 END。
const QRegularExpression &pemBlockRe()
{
    static const QRegularExpression re(QStringLiteral(
        "-----BEGIN [A-Z0-9 ]*-----.*?-----END [A-Z0-9 ]*-----"),
        QRegularExpression::DotMatchesEverythingOption);
    return re;
}

// 敏感键值赋值：key 后面跟 = : 或空白，再到值。值截止到空白/逗号/分号/引号边界。
// 覆盖常见写法：password=xxx、passwd: xxx、"secret": "xxx"、token xxx、
//              AWS_SECRET_ACCESS_KEY=xxx、api_token=xxx 等。大小写不敏感。
// key 允许字母/数字/下划线/连字符/点组合（末段命中敏感词即可），并容忍 JSON
// 的引号包裹（"secret": "..."）；(?:^|[^A-Za-z0-9]) 取代 \b 使复合键名也能命中。
const QRegularExpression &secretAssignRe()
{
    static const QRegularExpression re(QStringLiteral(
        "(?i)(?:^|[^A-Za-z0-9])\"?"
        "([A-Za-z0-9_.-]*"
        "(?:passwd|password|passphrase|secret|secret_key|api[_-]?key|"
        "access[_-]?key|private[_-]?key|auth[_-]?token|token|credential|"
        "client[_-]?secret)"
        "[A-Za-z0-9_.-]*)\"?"
        "\\s*([=:]|=>)\\s*"
        "(\"[^\"\\r\\n]*\"|'[^'\\r\\n]*'|[^\\s,;\"'\\r\\n]+)"));
    return re;
}

// IPv4 地址：四段 0-255，词边界防止误伤版本号（如 1.2.3）之外的正常数字串。
const QRegularExpression &ipv4Re()
{
    static const QRegularExpression re(QStringLiteral(
        "\\b(?:(?:25[0-5]|2[0-4]\\d|1?\\d?\\d)\\.){3}"
        "(?:25[0-5]|2[0-4]\\d|1?\\d?\\d)\\b"));
    return re;
}

// IPv6 地址（简化匹配）：至少两组十六进制以冒号相连，或含 "::" 压缩形式。
// 宽进严出，宁可多打码也不漏。
const QRegularExpression &ipv6Re()
{
    static const QRegularExpression re(QStringLiteral(
        "\\b(?:[0-9A-Fa-f]{1,4}:){2,7}[0-9A-Fa-f]{1,4}\\b"
        "|\\b(?:[0-9A-Fa-f]{1,4}:)?::(?:[0-9A-Fa-f]{1,4}:?)*\\b"));
    return re;
}

} // namespace

QString SensitiveDataMasker::mask(const QString &text)
{
    if (text.isEmpty())
        return text;

    QString out = text;

    // 顺序：先整块移除 PEM，再处理键值，最后 IP。
    out.replace(pemBlockRe(), QStringLiteral("***[已脱敏:私钥/证书]***"));

    // 保留 key 与分隔符，只打码值，便于模型理解「这里本有个密码」。
    out.replace(secretAssignRe(), QStringLiteral("\\1\\2***[已脱敏]***"));

    out.replace(ipv4Re(), QStringLiteral("***[已脱敏:IP]***"));
    out.replace(ipv6Re(), QStringLiteral("***[已脱敏:IP]***"));

    return out;
}

} // namespace cubeshell
