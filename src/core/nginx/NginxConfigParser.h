#pragma once

// NginxConfigParser.h — nginx 配置文本的解析 / 抽取 / 生成（纯函数，可单测）。
//
// 见 docs/Nginx可视化管理实现方案.md §3。三类能力：
//   1. 解析：tokenize + parse → 指令树（未知指令也保留，保证回写不丢内容）；
//   2. 抽取：指令树 → ServerView / SiteView 摘要（列表与概览用）；
//   3. 生成：SiteSpec 表单模型 → 完整配置文件文本（可视化编辑器保存用）。
//
// 取舍：解析保真到「指令级」而非「字符级」——注释与原始空白的逐字保留只由
// 「文本模式」保证；表单模式以指令树为准重建文本。

#include <QList>
#include <QString>
#include <QStringList>

#include "nginx/NginxConfig.h"

namespace cubeshell {
namespace nginx {

class NginxConfigParser {
public:
    // --- 词法 / 语法 ---

    // 把文本解析为顶层指令列表。失败时 *error 填入带行号的说明。
    static bool parse(const QString &text, QList<Directive> *out, QString *error);

    // 从顶层指令中抽取 server 块（顶层没有则把包在 http {} 等块里的下沉一层）。
    static QList<Directive> extractServerDirectives(const QList<Directive> &top);

    // --- 摘要 ---

    // 文本 + 文件路径 → 全部 server 摘要。解析失败时 *error 有值且返回空。
    static QList<ServerView> parseServers(const QString &text, const QString &filePath,
                                          QString *error = nullptr);

    // 文本 + 文件路径 → 站点摘要（含文件名 / 目录类别 / 托管与启用判定）。
    static SiteView parseSite(const QString &text, const QString &filePath);

    // 单个 server 指令 → 摘要。
    static ServerView toServerView(const Directive &server, const QString &filePath, int index);

    // --- 生成 ---

    // 指令树 → 文本（缩进 4 空格；忠实回写所有指令）。
    static QString serialize(const QList<Directive> &directives, int indentLevel = 0);

    // 表单模型 → 完整配置文件文本（含托管标记头）。
    static QString renderSite(const SiteSpec &spec);

    // 已解析的 server → 尽量回填表单模型；未知指令进 extra 列表。
    static bool fillSpecFromServer(const Directive &server, SiteSpec *out);

    // --- 纯辅助（可单测） ---

    // 把单个参数序列化为 nginx 安全文本（含空白或结构记号时加双引号）。
    static QString quoteArg(const QString &arg);

    // listen 指令 → 端口列表（容忍 [::]:80 / 0.0.0.0:8080 / 80 default_server）。
    static QList<int> listenPortsOf(const Directive &listen);
    // listen 是否含 ssl / default_server 标志。
    static bool listenHasFlag(const Directive &listen, const QString &flag);

    // location 指令的修饰符（"", "=", "~", "~*", "^~"）与匹配模式。
    static QString locationModifier(const Directive &location);
    static QString locationPattern(const Directive &location);

private:
    // 从 server 指令中解析出 LocationView。
    static LocationView toLocationView(const Directive &location);
};

} // namespace nginx
} // namespace cubeshell
