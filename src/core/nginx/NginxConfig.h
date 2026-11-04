#pragma once

// NginxConfig.h — nginx 配置的结构化数据模型 + 站点「表单模型」。
//
// 本项目 Docker/Kube 两模块把解析交给 CLI（docker ps --format json /
// kubectl -o json）；nginx 没有等价物（nginx -T 只 dump 文本），因此这里自带
// 一套轻量模型：
//   * Directive       —— 指令树节点，与原文等价（简单指令 / 块指令）；
//   * ServerView      —— 一个 server {} 的只读摘要（列表 / 概览用）；
//   * SiteView        —— 一个配置文件的摘要（站点列表用）；
//   * SiteSpec        —— 可视化编辑器的双向绑定表单模型；
//   * LocationSpec    —— 表单里的逐条 location。
// 纯数据层，无条件编译（同 kube/KubeResourceParser、docker/ComposeYaml）。

#include <QList>
#include <QString>
#include <QStringList>

namespace cubeshell {
namespace nginx {

// 托管标记：本工具生成的文件头部写入此注释，SiteView::managed 据此判定，
// 用于区分「本工具托管的站点」与「用户手写的配置」。
inline constexpr const char kManagedMarker[] = "Managed by CubeShell";

// 一条 nginx 指令。- name args; - 或 - name args { children } -。
struct Directive {
    QString name;
    QStringList args;
    bool hasBlock = false;
    QList<Directive> children;
    int line = 0;

    // 在直接子指令里找 name 对应的第一条（可用于 proxy_pass / root 等）。
    const Directive *findChild(const QString &childName) const;
    // 同上，返回所有同名子指令。
    QList<Directive> findChildren(const QString &childName) const;
};

// location 的摘要视图。
struct LocationView {
    QString modifier; // "" (前缀) / "=" / "~" / "~*" / "^~"
    QString pattern;  // "/" / "/api/" / "~ \.php$" 等
    QString kind;     // "proxy" / "static" / "redirect" / "raw"
    QString target;   // proxy_pass URL / root / alias / try_files 首值 / return 目标
};

// 一个 server {} 的摘要。
struct ServerView {
    QString filePath;
    int index = 0;        // 文件内第几个 server（从 0 起）
    bool parseOk = true;
    QString error;

    QList<int> listenPorts;
    bool listenSsl = false;
    bool defaultServer = false;
    QStringList serverNames;
    QString root;
    QStringList indexFiles;
    QString sslCertificate;
    QString sslCertificateKey;
    QString clientMaxBodySize;
    QString serverProxyPass; // server 级 proxy_pass（整站反代）
    QList<LocationView> locations;
};

// 一个配置文件（站点）的摘要。
struct SiteView {
    QString filePath;
    QString fileName;
    QString dirKind;   // "conf.d" / "sites-enabled" / "sites-available" / "nginx.conf" / "other"
    bool enabled = true;
    bool managed = false;
    bool parseOk = true;
    QString error;
    int serverCount = 0;

    // 汇总（多 server 时取并集 / 首个非空）。
    QStringList serverNames;
    QList<int> listenPorts;
    bool ssl = false;
    bool isProxy = false;
    QString rootOrTarget; // root 路径或 proxy_pass 目标（列表「目标」列）

    QList<ServerView> servers;
};

// ---- 表单模型（可视化编辑器） ----

struct LocationSpec {
    QString modifier; // "" / "=" / "~" / "~*" / "^~"
    QString pattern;
    QString kind;       // "proxy" / "static" / "spa" / "redirect" / "raw"
    QString proxyPass;  // kind == proxy
    QString root;       // kind == static
    QString alias;      // kind == static（与 root 二选一）
    QString tryFiles;   // kind == spa（默认 "$uri $uri/ /index.html"）
    QString returnTarget; // kind == redirect（"301 https://..." 的 "https://..." 部分）
    QString extra;      // 追加的原始指令行（多行）
};

struct SiteSpec {
    QString fileName;          // 文件名（不含目录），如 "app.conf"
    QStringList serverNames;
    int listenPort = 80;
    bool listenSsl = false;
    bool defaultServer = false;
    QString sslCertificate;
    QString sslCertificateKey;
    bool forceHttps = false;   // 生成 80 → 443 跳转 server

    QString root;
    QStringList indexFiles;
    bool spaFallback = false;  // location / 用 try_files ... /index.html
    QString clientMaxBodySize;

    bool proxyAll = false;     // 整站反向代理（location / → proxy_pass）
    QString proxyPass;
    bool proxyWebSocket = true;

    QList<LocationSpec> locations;
    QStringList extraServerDirectives; // server 级追加指令
};

} // namespace nginx
} // namespace cubeshell
