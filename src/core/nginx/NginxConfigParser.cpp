// NginxConfigParser.cpp — see NginxConfigParser.h.
// 见 docs/Nginx可视化管理实现方案.md §3。纯逻辑，无 IO、无网络。

#include "nginx/NginxConfigParser.h"

#include <QFileInfo>

namespace cubeshell {
namespace nginx {

// ---------------------------------------------------------------------------
// Directive 查询助手（NginxConfig.h 声明）
// ---------------------------------------------------------------------------

const Directive *Directive::findChild(const QString &childName) const
{
    for (const Directive &c : children) {
        if (c.name == childName)
            return &c;
    }
    return nullptr;
}

QList<Directive> Directive::findChildren(const QString &childName) const
{
    QList<Directive> out;
    for (const Directive &c : children) {
        if (c.name == childName)
            out.append(c);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 词法分析
// ---------------------------------------------------------------------------

namespace {

enum class Tok { Word, LBrace, RBrace, Semi, End };

struct Token {
    Tok type;
    QString text;
    int line;
};

// 词法规则：空白分隔；# 到行尾为注释（引号外才生效）；单/双引号字符串为一个
// 记号（引号内反斜杠原样保留，仅 \<quote> 表示引号本身，保证正则中的 \. 等
// 转义能在「解析→回写」间逐字往返）；{ } ; 为结构记号。
bool tokenize(const QString &text, QList<Token> *out, QString *error)
{
    int line = 1;
    int i = 0;
    const int n = text.size();
    QString buf;
    bool pending = false; // 正在积累一个词（"" 也应产出空词）

    auto flush = [&]() {
        if (pending) {
            out->append({Tok::Word, buf, line});
            buf.clear();
            pending = false;
        }
    };

    while (i < n) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\n')) {
            flush();
            ++line;
            ++i;
            continue;
        }
        if (c.isSpace()) {
            flush();
            ++i;
            continue;
        }
        if (c == QLatin1Char('#')) {
            flush();
            while (i < n && text.at(i) != QLatin1Char('\n'))
                ++i;
            continue;
        }
        if (c == QLatin1Char('{')) {
            flush();
            out->append({Tok::LBrace, QStringLiteral("{"), line});
            ++i;
            continue;
        }
        if (c == QLatin1Char('}')) {
            flush();
            out->append({Tok::RBrace, QStringLiteral("}"), line});
            ++i;
            continue;
        }
        if (c == QLatin1Char(';')) {
            flush();
            out->append({Tok::Semi, QStringLiteral(";"), line});
            ++i;
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            const QChar quote = c;
            pending = true;
            ++i;
            bool closed = false;
            while (i < n) {
                const QChar d = text.at(i);
                if (d == QLatin1Char('\\') && i + 1 < n && text.at(i + 1) == quote) {
                    // 转义引号 → 引号本身
                    buf.append(quote);
                    i += 2;
                    continue;
                }
                if (d == QLatin1Char('\\')) {
                    // 其余反斜杠原样保留（正则 \. 等）
                    buf.append(d);
                    ++i;
                    continue;
                }
                if (d == quote) {
                    closed = true;
                    ++i;
                    break;
                }
                if (d == QLatin1Char('\n'))
                    ++line;
                buf.append(d);
                ++i;
            }
            if (!closed) {
                if (error)
                    *error = QStringLiteral("第 %1 行: 引号未闭合").arg(line);
                return false;
            }
            continue;
        }
        // 普通字符（含 ( ) $ ~ * \ - . / 等）累入词
        pending = true;
        buf.append(c);
        ++i;
    }
    flush();
    out->append({Tok::End, QString(), line});
    return true;
}

// 递归下降：把记号流解析成指令树。
bool parseDirectives(const QList<Token> &toks, int *idx, bool insideBlock,
                     QList<Directive> *out, QString *error)
{
    while (*idx < toks.size()) {
        const Token &t = toks.at(*idx);
        if (t.type == Tok::End) {
            if (insideBlock) {
                if (error)
                    *error = QStringLiteral("文件结尾: 块未闭合");
                return false;
            }
            return true;
        }
        if (t.type == Tok::RBrace) {
            if (insideBlock) {
                ++(*idx);
                return true;
            }
            if (error)
                *error = QStringLiteral("第 %1 行: 多余的 '}'").arg(t.line);
            return false;
        }
        if (t.type == Tok::LBrace || t.type == Tok::Semi) {
            if (error)
                *error = QStringLiteral("第 %1 行: 意外的 '%2'").arg(t.line).arg(t.text);
            return false;
        }

        Directive d;
        d.name = t.text;
        d.line = t.line;
        ++(*idx);

        bool terminated = false;
        while (*idx < toks.size()) {
            const Token &a = toks.at(*idx);
            if (a.type == Tok::Word) {
                d.args.append(a.text);
                ++(*idx);
                continue;
            }
            if (a.type == Tok::Semi) {
                ++(*idx);
                terminated = true;
                break;
            }
            if (a.type == Tok::LBrace) {
                ++(*idx);
                d.hasBlock = true;
                if (!parseDirectives(toks, idx, true, &d.children, error))
                    return false;
                terminated = true;
                break;
            }
            if (a.type == Tok::RBrace) {
                if (error)
                    *error = QStringLiteral("第 %1 行: 指令 '%2' 缺少 ';' 或 '{'")
                                 .arg(a.line)
                                 .arg(d.name);
                return false;
            }
            // End
            if (error)
                *error = QStringLiteral("第 %1 行: 指令 '%2' 缺少 ';'")
                             .arg(d.line)
                             .arg(d.name);
            return false;
        }
        if (!terminated) {
            if (error && error->isEmpty())
                *error = QStringLiteral("第 %1 行: 指令 '%2' 未结束").arg(d.line).arg(d.name);
            return false;
        }
        out->append(d);
    }
    if (insideBlock) {
        if (error)
            *error = QStringLiteral("块未闭合");
        return false;
    }
    return true;
}

const QStringList &locationModifiers()
{
    static const QStringList mods = {QStringLiteral("="), QStringLiteral("~"),
                                     QStringLiteral("~*"), QStringLiteral("^~")};
    return mods;
}

// 把一个指令序列化成「单条指令」文本（多行块会带换行），无前导缩进。
QString serializeOne(const Directive &d, int level);

QString classifyDir(const QString &filePath)
{
    const QString name = QFileInfo(filePath).fileName();
    if (name == QLatin1String("nginx.conf"))
        return QStringLiteral("nginx.conf");
    if (filePath.contains(QLatin1String("/conf.d/")))
        return QStringLiteral("conf.d");
    if (filePath.contains(QLatin1String("/sites-enabled/")))
        return QStringLiteral("sites-enabled");
    if (filePath.contains(QLatin1String("/sites-available/")))
        return QStringLiteral("sites-available");
    return QStringLiteral("other");
}

} // namespace

// ---------------------------------------------------------------------------
// 解析 / 抽取
// ---------------------------------------------------------------------------

bool NginxConfigParser::parse(const QString &text, QList<Directive> *out, QString *error)
{
    QList<Token> toks;
    QString localErr;
    if (!tokenize(text, &toks, &localErr)) {
        if (error)
            *error = localErr;
        return false;
    }
    int idx = 0;
    const bool ok = parseDirectives(toks, &idx, false, out, error);
    return ok;
}

QList<Directive> NginxConfigParser::extractServerDirectives(const QList<Directive> &top)
{
    QList<Directive> servers;
    for (const Directive &d : top) {
        if (d.name == QLatin1String("server") && d.hasBlock)
            servers.append(d);
    }
    if (!servers.isEmpty())
        return servers;
    // 下沉一层：文件把 server 包在 http {} / stream {} 里。
    for (const Directive &d : top) {
        if (!d.hasBlock)
            continue;
        for (const Directive &c : d.children) {
            if (c.name == QLatin1String("server") && c.hasBlock)
                servers.append(c);
        }
    }
    return servers;
}

QList<int> NginxConfigParser::listenPortsOf(const Directive &listen)
{
    static const QStringList flags = {
        QStringLiteral("ssl"),        QStringLiteral("default_server"),
        QStringLiteral("default"),    QStringLiteral("http2"),
        QStringLiteral("reuseport"),  QStringLiteral("proxy_protocol"),
        QStringLiteral("deferred"),   QStringLiteral("bind"),
        QStringLiteral("so_keepalive"),
    };
    QList<int> ports;
    for (const QString &a : listen.args) {
        if (flags.contains(a) || a.contains(QLatin1Char('=')))
            continue;
        if (a.startsWith(QLatin1String("unix:")))
            continue;
        const int colon = a.lastIndexOf(QLatin1Char(':'));
        const QString portStr = colon >= 0 ? a.mid(colon + 1) : a;
        bool ok = false;
        const int p = portStr.toInt(&ok);
        if (ok && p > 0 && p < 65536 && !ports.contains(p))
            ports.append(p);
    }
    return ports;
}

bool NginxConfigParser::listenHasFlag(const Directive &listen, const QString &flag)
{
    return listen.args.contains(flag);
}

QString NginxConfigParser::locationModifier(const Directive &location)
{
    if (location.args.size() >= 2 && locationModifiers().contains(location.args.at(0)))
        return location.args.at(0);
    return QString();
}

QString NginxConfigParser::locationPattern(const Directive &location)
{
    const QString mod = locationModifier(location);
    const int start = mod.isEmpty() ? 0 : 1;
    if (location.args.size() <= start)
        return QString();
    return location.args.mid(start).join(QLatin1Char(' '));
}

LocationView NginxConfigParser::toLocationView(const Directive &location)
{
    LocationView v;
    v.modifier = locationModifier(location);
    v.pattern = locationPattern(location);

    if (const Directive *pp = location.findChild(QStringLiteral("proxy_pass"))) {
        if (!pp->args.isEmpty()) {
            v.kind = QStringLiteral("proxy");
            v.target = pp->args.first();
            return v;
        }
    }
    if (const Directive *al = location.findChild(QStringLiteral("alias"))) {
        if (!al->args.isEmpty()) {
            v.kind = QStringLiteral("static");
            v.target = al->args.first();
            return v;
        }
    }
    if (const Directive *rt = location.findChild(QStringLiteral("root"))) {
        if (!rt->args.isEmpty()) {
            v.kind = QStringLiteral("static");
            v.target = rt->args.first();
            return v;
        }
    }
    if (const Directive *tf = location.findChild(QStringLiteral("try_files"))) {
        v.kind = QStringLiteral("static");
        v.target = QStringLiteral("try_files ") + tf->args.join(QLatin1Char(' '));
        return v;
    }
    if (const Directive *ret = location.findChild(QStringLiteral("return"))) {
        v.kind = QStringLiteral("redirect");
        v.target = ret->args.join(QLatin1Char(' '));
        return v;
    }
    v.kind = QStringLiteral("raw");
    return v;
}

ServerView NginxConfigParser::toServerView(const Directive &server, const QString &filePath,
                                           int index)
{
    ServerView v;
    v.filePath = filePath;
    v.index = index;

    for (const Directive &c : server.children) {
        if (c.name == QLatin1String("listen")) {
            for (int p : listenPortsOf(c)) {
                if (!v.listenPorts.contains(p))
                    v.listenPorts.append(p);
            }
            if (listenHasFlag(c, QStringLiteral("ssl")))
                v.listenSsl = true;
            if (listenHasFlag(c, QStringLiteral("default_server"))
                || listenHasFlag(c, QStringLiteral("default")))
                v.defaultServer = true;
        } else if (c.name == QLatin1String("server_name")) {
            v.serverNames += c.args;
        } else if (c.name == QLatin1String("root")) {
            if (!c.args.isEmpty())
                v.root = c.args.first();
        } else if (c.name == QLatin1String("index")) {
            v.indexFiles += c.args;
        } else if (c.name == QLatin1String("ssl_certificate")) {
            if (!c.args.isEmpty())
                v.sslCertificate = c.args.first();
        } else if (c.name == QLatin1String("ssl_certificate_key")) {
            if (!c.args.isEmpty())
                v.sslCertificateKey = c.args.first();
        } else if (c.name == QLatin1String("client_max_body_size")) {
            if (!c.args.isEmpty())
                v.clientMaxBodySize = c.args.first();
        } else if (c.name == QLatin1String("proxy_pass")) {
            if (!c.args.isEmpty())
                v.serverProxyPass = c.args.first();
        } else if (c.name == QLatin1String("location") && c.hasBlock) {
            v.locations.append(toLocationView(c));
        }
    }
    return v;
}

QList<ServerView> NginxConfigParser::parseServers(const QString &text, const QString &filePath,
                                                  QString *error)
{
    QList<Directive> top;
    QString localErr;
    if (!parse(text, &top, &localErr)) {
        if (error)
            *error = localErr;
        return {};
    }
    const QList<Directive> servers = extractServerDirectives(top);
    QList<ServerView> out;
    int idx = 0;
    for (const Directive &s : servers)
        out.append(toServerView(s, filePath, idx++));
    return out;
}

SiteView NginxConfigParser::parseSite(const QString &text, const QString &filePath)
{
    SiteView s;
    s.filePath = filePath;
    s.fileName = QFileInfo(filePath).fileName();
    s.dirKind = classifyDir(filePath);
    s.managed = text.contains(QString::fromLatin1(kManagedMarker));
    // sites-available 下的文件默认「未启用」；管理器会用 sites-enabled 里的
    // 同名文件把它修正为已启用。
    s.enabled = (s.dirKind != QLatin1String("sites-available"));

    QString err;
    const QList<ServerView> servers = parseServers(text, filePath, &err);
    if (!err.isEmpty()) {
        s.parseOk = false;
        s.error = err;
        return s;
    }
    s.servers = servers;
    s.serverCount = servers.size();
    for (const ServerView &sv : servers) {
        for (const QString &name : sv.serverNames) {
            if (!s.serverNames.contains(name))
                s.serverNames.append(name);
        }
        for (int p : sv.listenPorts) {
            if (!s.listenPorts.contains(p))
                s.listenPorts.append(p);
        }
        s.ssl = s.ssl || sv.listenSsl;
        if (!sv.serverProxyPass.isEmpty()) {
            s.isProxy = true;
            if (s.rootOrTarget.isEmpty())
                s.rootOrTarget = sv.serverProxyPass;
        }
        for (const LocationView &lv : sv.locations) {
            if (lv.kind == QLatin1String("proxy")) {
                s.isProxy = true;
                if (s.rootOrTarget.isEmpty())
                    s.rootOrTarget = lv.target;
            }
        }
        if (s.rootOrTarget.isEmpty() && !sv.root.isEmpty())
            s.rootOrTarget = sv.root;
    }
    return s;
}

// ---------------------------------------------------------------------------
// 生成
// ---------------------------------------------------------------------------

QString NginxConfigParser::quoteArg(const QString &arg)
{
    if (arg.isEmpty())
        return QStringLiteral("\"\"");
    bool need = false;
    for (const QChar c : arg) {
        if (c.isSpace() || c == QLatin1Char('{') || c == QLatin1Char('}')
            || c == QLatin1Char(';') || c == QLatin1Char('#') || c == QLatin1Char('"')
            || c == QLatin1Char('\'')) {
            need = true;
            break;
        }
    }
    if (!need)
        return arg;
    QString esc = arg;
    esc.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return QLatin1Char('"') + esc + QLatin1Char('"');
}

QString NginxConfigParser::serialize(const QList<Directive> &directives, int indentLevel)
{
    const QString ind(indentLevel * 4, QLatin1Char(' '));
    QString out;
    for (const Directive &d : directives) {
        out += ind + d.name;
        for (const QString &a : d.args)
            out += QLatin1Char(' ') + quoteArg(a);
        if (d.hasBlock) {
            out += QLatin1String(" {\n");
            out += serialize(d.children, indentLevel + 1);
            out += ind + QLatin1String("}\n");
        } else {
            out += QLatin1String(";\n");
        }
    }
    return out;
}

namespace {
QString serializeOne(const Directive &d, int level)
{
    return NginxConfigParser::serialize(QList<Directive>{d}, level);
}
} // namespace

QString NginxConfigParser::renderSite(const SiteSpec &spec)
{
    QString out;
    out += QStringLiteral("# %1\n").arg(QString::fromLatin1(kManagedMarker));
    out += QStringLiteral("# 由 cube-shell Nginx 可视化管理生成；需要自定义时请改用「文本模式」。\n\n");

    const QString names = spec.serverNames.join(QLatin1Char(' '));
    const QString namesOrCatch = names.isEmpty() ? QStringLiteral("_") : names;

    // 强制 HTTPS：生成一个 80 → 443 的跳转 server。
    if (spec.forceHttps && spec.listenSsl) {
        out += QStringLiteral("server {\n");
        out += QStringLiteral("    listen 80;\n");
        if (!names.isEmpty())
            out += QStringLiteral("    server_name %1;\n").arg(names);
        out += QStringLiteral("    return 301 https://$host$request_uri;\n");
        out += QStringLiteral("}\n\n");
    }

    out += QStringLiteral("server {\n");
    QString listenLine = QStringLiteral("    listen %1").arg(spec.listenPort);
    if (spec.listenSsl)
        listenLine += QStringLiteral(" ssl");
    if (spec.defaultServer)
        listenLine += QStringLiteral(" default_server");
    out += listenLine + QStringLiteral(";\n");
    out += QStringLiteral("    server_name %1;\n").arg(namesOrCatch);

    if (!spec.clientMaxBodySize.isEmpty())
        out += QStringLiteral("    client_max_body_size %1;\n").arg(spec.clientMaxBodySize);
    if (!spec.root.isEmpty())
        out += QStringLiteral("    root %1;\n").arg(spec.root);
    if (!spec.indexFiles.isEmpty())
        out += QStringLiteral("    index %1;\n").arg(spec.indexFiles.join(QLatin1Char(' ')));
    if (spec.listenSsl) {
        if (!spec.sslCertificate.isEmpty())
            out += QStringLiteral("    ssl_certificate %1;\n").arg(spec.sslCertificate);
        if (!spec.sslCertificateKey.isEmpty())
            out += QStringLiteral("    ssl_certificate_key %1;\n").arg(spec.sslCertificateKey);
    }

    // 主 location。
    if (spec.proxyAll && !spec.proxyPass.isEmpty()) {
        LocationSpec root;
        root.pattern = QStringLiteral("/");
        root.kind = QStringLiteral("proxy");
        root.proxyPass = spec.proxyPass;
        if (spec.proxyWebSocket)
            root.extra = QStringLiteral(
                "proxy_set_header Host $host;\n"
                "proxy_set_header X-Real-IP $remote_addr;\n"
                "proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n"
                "proxy_set_header X-Forwarded-Proto $scheme;\n"
                "proxy_http_version 1.1;\n"
                "proxy_set_header Upgrade $http_upgrade;\n"
                "proxy_set_header Connection \"upgrade\";");
        out += QStringLiteral("    location / {\n");
        out += QStringLiteral("        proxy_pass %1;\n").arg(root.proxyPass);
        for (const QString &ln : root.extra.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            out += QStringLiteral("        ") + ln.trimmed() + QStringLiteral("\n");
        out += QStringLiteral("    }\n");
    } else if (!spec.root.isEmpty()) {
        out += QStringLiteral("    location / {\n");
        if (spec.spaFallback)
            out += QStringLiteral("        try_files $uri $uri/ /index.html;\n");
        else
            out += QStringLiteral("        try_files $uri $uri/ =404;\n");
        out += QStringLiteral("    }\n");
    }

    for (const LocationSpec &loc : spec.locations) {
        const QString mod = loc.modifier.isEmpty() ? QString()
                                                   : loc.modifier + QLatin1Char(' ');
        const QString pattern = loc.pattern.isEmpty() ? QStringLiteral("/") : loc.pattern;
        out += QStringLiteral("    location %1%2 {\n").arg(mod, pattern);
        auto body = [&out](const QString &s) {
            out += QStringLiteral("        ") + s + QStringLiteral("\n");
        };
        if (loc.kind == QLatin1String("proxy")) {
            body(QStringLiteral("proxy_pass %1;").arg(loc.proxyPass));
        } else if (loc.kind == QLatin1String("spa")) {
            const QString tf = loc.tryFiles.isEmpty()
                                   ? QStringLiteral("$uri $uri/ /index.html")
                                   : loc.tryFiles;
            body(QStringLiteral("try_files %1;").arg(tf));
            if (!loc.root.isEmpty())
                body(QStringLiteral("root %1;").arg(loc.root));
        } else if (loc.kind == QLatin1String("static")) {
            if (!loc.alias.isEmpty())
                body(QStringLiteral("alias %1;").arg(loc.alias));
            else if (!loc.root.isEmpty())
                body(QStringLiteral("root %1;").arg(loc.root));
        } else if (loc.kind == QLatin1String("redirect")) {
            body(QStringLiteral("return %1;")
                     .arg(loc.returnTarget.isEmpty() ? QStringLiteral("301 /") : loc.returnTarget));
        }
        if (!loc.extra.trimmed().isEmpty()) {
            for (const QString &ln : loc.extra.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                if (!ln.trimmed().isEmpty())
                    body(ln.trimmed());
            }
        }
        out += QStringLiteral("    }\n");
    }

    for (const QString &ln : spec.extraServerDirectives) {
        if (!ln.trimmed().isEmpty())
            out += QStringLiteral("    ") + ln.trimmed() + QStringLiteral("\n");
    }

    out += QStringLiteral("}\n");
    return out;
}

bool NginxConfigParser::fillSpecFromServer(const Directive &server, SiteSpec *out)
{
    if (!out)
        return false;

    const ServerView v = toServerView(server, QString(), 0);
    out->serverNames = v.serverNames;
    if (v.listenPorts.contains(443))
        out->listenPort = 443;
    else if (!v.listenPorts.isEmpty())
        out->listenPort = v.listenPorts.first();
    out->listenSsl = v.listenSsl;
    out->defaultServer = v.defaultServer;
    out->sslCertificate = v.sslCertificate;
    out->sslCertificateKey = v.sslCertificateKey;
    out->clientMaxBodySize = v.clientMaxBodySize;
    out->root = v.root;
    out->indexFiles = v.indexFiles;

    // 未知 server 级指令原样保留到 extra（保证不丢内容）。
    static const QStringList known = {
        QStringLiteral("listen"),          QStringLiteral("server_name"),
        QStringLiteral("root"),            QStringLiteral("index"),
        QStringLiteral("ssl_certificate"), QStringLiteral("ssl_certificate_key"),
        QStringLiteral("client_max_body_size"), QStringLiteral("location"),
    };
    for (const Directive &c : server.children) {
        if (known.contains(c.name))
            continue;
        const QString text = serializeOne(c, 0);
        for (const QString &ln : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            out->extraServerDirectives.append(ln);
    }

    // location：全部映射为 LocationSpec（不折叠成 proxyAll/spaFallback，
    // 保证「导入既有站点 → 保存」逐字无损）。未知子指令进 extra。
    for (const Directive &c : server.children) {
        if (c.name != QLatin1String("location") || !c.hasBlock)
            continue;

        LocationSpec ls;
        ls.modifier = locationModifier(c);
        ls.pattern = locationPattern(c);

        const Directive *pp = c.findChild(QStringLiteral("proxy_pass"));
        const Directive *al = c.findChild(QStringLiteral("alias"));
        const Directive *rt = c.findChild(QStringLiteral("root"));
        const Directive *tf = c.findChild(QStringLiteral("try_files"));
        const Directive *ret = c.findChild(QStringLiteral("return"));

        QStringList consumed;
        if (pp && !pp->args.isEmpty()) {
            ls.kind = QStringLiteral("proxy");
            ls.proxyPass = pp->args.first();
            consumed = {QStringLiteral("proxy_pass")};
        } else if (al && !al->args.isEmpty()) {
            ls.kind = QStringLiteral("static");
            ls.alias = al->args.first();
            consumed = {QStringLiteral("alias"), QStringLiteral("root")};
            if (rt && !rt->args.isEmpty())
                ls.root = rt->args.first();
        } else if (rt && !rt->args.isEmpty()) {
            ls.kind = QStringLiteral("static");
            ls.root = rt->args.first();
            consumed = {QStringLiteral("root")};
        } else if (tf && !tf->args.isEmpty()) {
            ls.kind = QStringLiteral("spa");
            ls.tryFiles = tf->args.join(QLatin1Char(' '));
            consumed = {QStringLiteral("try_files")};
        } else if (ret && !ret->args.isEmpty()) {
            ls.kind = QStringLiteral("redirect");
            ls.returnTarget = ret->args.join(QLatin1Char(' '));
            consumed = {QStringLiteral("return")};
        } else {
            ls.kind = QStringLiteral("raw");
        }

        QStringList extraLines;
        for (const Directive &child : c.children) {
            if (consumed.contains(child.name))
                continue;
            const QString text = serializeOne(child, 0);
            for (const QString &ln : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                extraLines.append(ln.trimmed());
        }
        ls.extra = extraLines.join(QLatin1Char('\n'));
        out->locations.append(ls);
    }
    return true;
}

} // namespace nginx
} // namespace cubeshell
