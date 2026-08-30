#pragma once

// KubeResourceParser.h — kubectl `-o json` 输出的纯解析层 + 资源类型注册表。
//
// 对应 docs/Kubernetes功能实现方案.md §3：
//   - 资源类型注册表（KubeResourceKind）：树模型与命令拼装都由描述表驱动，
//     新增资源类型 = 注册表加一行 + 一个 summarize 分支 + 测试；
//   - 解析全部是纯函数（QJsonDocument），与 DockerManager::parsePsJsonLines
//     同一套路，不依赖 kubectl / 网络 / 子进程，可整体单测。
//
// 本文件不引 QProcess / CommandExecutor，CUBESHELL_WITH_LOCALPROC=OFF（鸿蒙）
// 也照常编译（对标 docker/ComposeYaml 的纯数据层定位）。

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace cubeshell {

// --- 资源类型注册表条目 ---
struct KubeResourceKind {
    QString apiPlural;   // kubectl get 的复数名："pods" / "deployments" ...
    QString displayName; // 树中分组名（英文 key，UI 侧 tr()）
    bool namespaced = true;  // nodes / persistentvolumes / storageclasses / namespaces 为 false
    QString group;       // 顶层分组："Workloads" / "Network" / "Config" / "Storage" / "Cluster"
    bool scalable = false;    // 支持 kubectl scale（deployments/statefulsets/replicasets）
    bool restartable = false; // 支持 rollout restart（deployments/statefulsets/daemonsets）
};

// `kubectl config view -o json` 里 contexts 数组的一条上下文。
// （`config get-contexts` 不支持 -o json，故走 config view。）
struct KubeContextInfo {
    QString name;
    QString cluster;
    QString user;
    QString namespace_; // 默认命名空间（可为空 → "default"）
    bool isCurrent = false;
};

// 资源对象的统一标识：右键菜单 / 操作派发都靠它，不区分具体资源种类。
struct KubeObjectRef {
    QString apiPlural;   // "pods"
    QString name;
    QString namespace_;  // 非 namespaced 资源为空

    bool operator==(const KubeObjectRef &other) const
    {
        return apiPlural == other.apiPlural && name == other.name
               && namespace_ == other.namespace_;
    }
};

// 资源树的统一行模型：每种资源解析后都收敛到这个结构。
struct KubeResourceRow {
    QString name;
    QString namespace_;               // 非 namespaced 为空
    QString status;                   // 统一状态摘要（"1/1 Running" / "Bound" ...）
    QString age;                      // 由 creationTimestamp 换算（"3d" / "5h" ...）
    QHash<QString, QString> extra;    // kind 展示列：node/ip/ports/provisioner...
    QStringList containers;           // pods 专有：容器名列表（日志/终端子菜单）
    QString nodeName;                 // pods 专有
};

namespace KubeResourceParser {

// 顶层分组的固定顺序（树按此排列）。
QStringList groupOrder();
// 全量资源类型注册表（按 groupOrder + 表内顺序）。
QList<KubeResourceKind> allKinds();
// 按 apiPlural 查注册表；未命中返回 nullptr。
const KubeResourceKind *findKind(const QString &apiPlural);
// 某顶层分组下的 kind 列表（保持注册表顺序）。
QList<KubeResourceKind> kindsInGroup(const QString &group);

// 解析 `kubectl config view -o json` 的 contexts / current-context。
// 容忍 sudo 提示等前缀垃圾行（从第一个 '{' 起解析）；失败返回空列表。
QList<KubeContextInfo> parseContextsJson(const QByteArray &json);

// 解析 `kubectl get namespaces -o json` 为名字列表（保持返回顺序）。
QStringList parseNamespacesJson(const QByteArray &json);

// 解析 `kubectl get <apiPlural> -o json` 为统一行模型。
// now 由调用方传入（KubeManager 取 currentDateTimeUtc），保证 AGE 换算可测。
QList<KubeResourceRow> parseResourceListJson(const QString &apiPlural,
                                             const QByteArray &json,
                                             const QDateTime &now);

// kubectl 风格 AGE：<2min→"Ns"，<2h→"Nm"，<2d→"Nh"，否则"Nd"。
// 未来时间/无效时间返回 "-"。
QString formatAge(const QDateTime &creationTimestamp, const QDateTime &now);

// 从 metadata.creationTimestamp（ISO8601）解析；无效返回 invalid QDateTime。
QDateTime parseCreationTimestamp(const QJsonObject &metadata);

} // namespace KubeResourceParser

} // namespace cubeshell

// 值类型要跨线程随队列信号投递（KubeManager 的信号发自工作线程）。
Q_DECLARE_METATYPE(cubeshell::KubeContextInfo)
Q_DECLARE_METATYPE(cubeshell::KubeObjectRef)
Q_DECLARE_METATYPE(cubeshell::KubeResourceRow)
