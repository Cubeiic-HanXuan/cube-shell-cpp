// KubeResourceParser.cpp — see KubeResourceParser.h.
// 对应 docs/Kubernetes功能实现方案.md §3.2：kubectl -o json 的纯解析层。

#include "kube/KubeResourceParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

namespace cubeshell {

// ---------------------------------------------------------------------------
// 资源类型注册表
// ---------------------------------------------------------------------------

namespace {

// 注册表只存英文 key；displayName 的翻译在 UI 侧 tr()。
// 顺序即树内顺序。
const KubeResourceKind kKinds[] = {
    // Workloads
    {QStringLiteral("pods"),         QStringLiteral("Pods"),         true,  QStringLiteral("Workloads"), false, false},
    {QStringLiteral("deployments"),  QStringLiteral("Deployments"),  true,  QStringLiteral("Workloads"), true,  true},
    {QStringLiteral("statefulsets"), QStringLiteral("StatefulSets"), true,  QStringLiteral("Workloads"), true,  true},
    {QStringLiteral("daemonsets"),   QStringLiteral("DaemonSets"),   true,  QStringLiteral("Workloads"), false, true},
    {QStringLiteral("replicasets"),  QStringLiteral("ReplicaSets"),  true,  QStringLiteral("Workloads"), true,  false},
    {QStringLiteral("jobs"),         QStringLiteral("Jobs"),         true,  QStringLiteral("Workloads"), false, false},
    {QStringLiteral("cronjobs"),     QStringLiteral("CronJobs"),     true,  QStringLiteral("Workloads"), false, false},
    // Network
    {QStringLiteral("services"),     QStringLiteral("Services"),     true,  QStringLiteral("Network"),   false, false},
    {QStringLiteral("ingresses"),    QStringLiteral("Ingresses"),    true,  QStringLiteral("Network"),   false, false},
    {QStringLiteral("endpoints"),    QStringLiteral("Endpoints"),    true,  QStringLiteral("Network"),   false, false},
    // Config
    {QStringLiteral("configmaps"),   QStringLiteral("ConfigMaps"),   true,  QStringLiteral("Config"),    false, false},
    {QStringLiteral("secrets"),      QStringLiteral("Secrets"),      true,  QStringLiteral("Config"),    false, false},
    // Storage
    {QStringLiteral("persistentvolumeclaims"), QStringLiteral("PersistentVolumeClaims"), true,  QStringLiteral("Storage"), false, false},
    {QStringLiteral("persistentvolumes"),      QStringLiteral("PersistentVolumes"),      false, QStringLiteral("Storage"), false, false},
    {QStringLiteral("storageclasses"),         QStringLiteral("StorageClasses"),         false, QStringLiteral("Storage"), false, false},
    // Cluster
    {QStringLiteral("nodes"),        QStringLiteral("Nodes"),        false, QStringLiteral("Cluster"),   false, false},
    {QStringLiteral("namespaces"),   QStringLiteral("Namespaces"),   false, QStringLiteral("Cluster"),   false, false},
    {QStringLiteral("events"),       QStringLiteral("Events"),       true,  QStringLiteral("Cluster"),   false, false},
};

QJsonObject obj(const QJsonObject &o, const QString &key)
{
    return o.value(key).toObject();
}

QJsonArray arr(const QJsonObject &o, const QString &key)
{
    return o.value(key).toArray();
}

QString str(const QJsonObject &o, const QString &key)
{
    return o.value(key).toString();
}

QStringList imageList(const QJsonObject &podSpec)
{
    QStringList images;
    const QJsonArray containers = arr(podSpec, QStringLiteral("containers"));
    for (const QJsonValue &c : containers) {
        const QString image = str(c.toObject(), QStringLiteral("image"));
        if (!image.isEmpty())
            images.append(image);
    }
    return images;
}

// --- per-kind summarize：填充 row.status / row.extra（pods 另填 containers/nodeName） ---
// ageOverride 非 invalid 时替代 metadata.creationTimestamp 作为 AGE 基准（events）。
void summarize(const QString &apiPlural, const QJsonObject &item,
               const QDateTime &now, KubeResourceRow &row, QDateTime *ageOverride)
{
    const QJsonObject spec = obj(item, QStringLiteral("spec"));
    const QJsonObject status = obj(item, QStringLiteral("status"));

    if (apiPlural == QLatin1String("pods")) {
        const QJsonArray specContainers = arr(spec, QStringLiteral("containers"));
        QStringList names;
        for (const QJsonValue &c : specContainers) {
            const QString n = str(c.toObject(), QStringLiteral("name"));
            if (!n.isEmpty())
                names.append(n);
        }
        row.containers = names;
        row.nodeName = str(spec, QStringLiteral("nodeName"));

        // ready/total：containerStatuses 里 ready==true 的计数 / spec 容器总数。
        const QJsonArray containerStatuses = arr(status, QStringLiteral("containerStatuses"));
        int ready = 0;
        int restarts = 0;
        QString waitingReason;
        for (const QJsonValue &cs : containerStatuses) {
            const QJsonObject cso = cs.toObject();
            if (cso.value(QStringLiteral("ready")).toBool())
                ++ready;
            restarts += cso.value(QStringLiteral("restartCount")).toInt();
            // CrashLoopBackOff / ImagePullBackOff 等：waiting.reason 优先于 phase。
            const QJsonObject state = obj(cso, QStringLiteral("state"));
            const QString reason = str(obj(state, QStringLiteral("waiting")),
                                       QStringLiteral("reason"));
            if (waitingReason.isEmpty() && !reason.isEmpty())
                waitingReason = reason;
        }
        const int total = specContainers.isEmpty()
            ? containerStatuses.size() : specContainers.size();

        QString phase = str(status, QStringLiteral("phase"));
        if (!waitingReason.isEmpty())
            phase = waitingReason;
        // 删除中的 Pod：metadata.deletionTimestamp 存在时 kubectl 显示 Terminating。
        if (!str(obj(item, QStringLiteral("metadata")),
                 QStringLiteral("deletionTimestamp")).isEmpty())
            phase = QStringLiteral("Terminating");

        row.status = QStringLiteral("%1/%2 %3").arg(ready).arg(total).arg(phase);
        row.extra.insert(QStringLiteral("node"), row.nodeName);
        row.extra.insert(QStringLiteral("ip"), str(status, QStringLiteral("podIP")));
        row.extra.insert(QStringLiteral("restarts"), QString::number(restarts));
        return;
    }

    if (apiPlural == QLatin1String("deployments")
        || apiPlural == QLatin1String("statefulsets")
        || apiPlural == QLatin1String("replicasets")) {
        const int desired = spec.contains(QStringLiteral("replicas"))
            ? spec.value(QStringLiteral("replicas")).toInt() : 1;
        const int readyCount = status.value(QStringLiteral("readyReplicas")).toInt();
        row.status = QStringLiteral("%1/%2").arg(readyCount).arg(desired);
        row.extra.insert(QStringLiteral("images"),
                         imageList(obj(obj(spec, QStringLiteral("template")),
                                       QStringLiteral("spec"))).join(QStringLiteral(", ")));
        return;
    }

    if (apiPlural == QLatin1String("daemonsets")) {
        const int desired = status.value(QStringLiteral("desiredNumberScheduled")).toInt();
        const int readyCount = status.value(QStringLiteral("numberReady")).toInt();
        row.status = QStringLiteral("%1/%2").arg(readyCount).arg(desired);
        row.extra.insert(QStringLiteral("images"),
                         imageList(obj(obj(spec, QStringLiteral("template")),
                                       QStringLiteral("spec"))).join(QStringLiteral(", ")));
        return;
    }

    if (apiPlural == QLatin1String("jobs")) {
        // conditions 里的终态优先；否则 active/completions。
        const QJsonArray conditions = arr(status, QStringLiteral("conditions"));
        QString finalState;
        for (const QJsonValue &c : conditions) {
            const QJsonObject co = c.toObject();
            if (str(co, QStringLiteral("status")) == QLatin1String("True")) {
                const QString type = str(co, QStringLiteral("type"));
                if (type == QLatin1String("Complete") || type == QLatin1String("Failed"))
                    finalState = type;
            }
        }
        const int completions = spec.contains(QStringLiteral("completions"))
            ? spec.value(QStringLiteral("completions")).toInt() : 1;
        const int succeeded = status.value(QStringLiteral("succeeded")).toInt();
        if (!finalState.isEmpty())
            row.status = QStringLiteral("%1/%2 %3").arg(succeeded).arg(completions).arg(finalState);
        else
            row.status = QStringLiteral("%1/%2 Running").arg(succeeded).arg(completions);
        return;
    }

    if (apiPlural == QLatin1String("cronjobs")) {
        const bool suspend = spec.value(QStringLiteral("suspend")).toBool();
        row.status = suspend ? QStringLiteral("Suspended") : QStringLiteral("Active");
        row.extra.insert(QStringLiteral("schedule"), str(spec, QStringLiteral("schedule")));
        const QString last = str(status, QStringLiteral("lastScheduleTime"));
        if (!last.isEmpty()) {
            const QDateTime lastDt = QDateTime::fromString(last, Qt::ISODate);
            if (lastDt.isValid())
                row.extra.insert(QStringLiteral("lastSchedule"),
                                 KubeResourceParser::formatAge(lastDt, now));
        }
        return;
    }

    if (apiPlural == QLatin1String("services")) {
        const QString type = str(spec, QStringLiteral("type"));
        row.status = type.isEmpty() ? QStringLiteral("ClusterIP") : type;
        row.extra.insert(QStringLiteral("clusterIP"), str(spec, QStringLiteral("clusterIP")));
        QStringList ports;
        const QJsonArray portArr = arr(spec, QStringLiteral("ports"));
        for (const QJsonValue &p : portArr) {
            const QJsonObject po = p.toObject();
            QString portStr = QString::number(po.value(QStringLiteral("port")).toInt());
            const int target = po.value(QStringLiteral("targetPort")).toInt();
            if (target > 0 && target != po.value(QStringLiteral("port")).toInt())
                portStr += QStringLiteral(":%1").arg(target);
            const QString proto = str(po, QStringLiteral("protocol"));
            portStr += QStringLiteral("/%1").arg(proto.isEmpty() ? QStringLiteral("TCP") : proto);
            ports.append(portStr);
        }
        row.extra.insert(QStringLiteral("ports"), ports.join(QStringLiteral(",")));
        // 外部地址：status.loadBalancer.ingress[0].ip/hostname 或 spec.externalIPs。
        QString external;
        const QJsonArray lbIngress = arr(obj(status, QStringLiteral("loadBalancer")),
                                         QStringLiteral("ingress"));
        if (!lbIngress.isEmpty()) {
            const QJsonObject first = lbIngress.first().toObject();
            external = str(first, QStringLiteral("ip"));
            if (external.isEmpty())
                external = str(first, QStringLiteral("hostname"));
        }
        if (external.isEmpty()) {
            const QJsonArray extIps = arr(spec, QStringLiteral("externalIPs"));
            if (!extIps.isEmpty())
                external = extIps.first().toString();
        }
        row.extra.insert(QStringLiteral("externalIP"), external);
        return;
    }

    if (apiPlural == QLatin1String("ingresses")) {
        QStringList hosts;
        const QJsonArray rules = arr(spec, QStringLiteral("rules"));
        for (const QJsonValue &r : rules) {
            const QString host = str(r.toObject(), QStringLiteral("host"));
            if (!host.isEmpty())
                hosts.append(host);
        }
        row.extra.insert(QStringLiteral("hosts"), hosts.join(QStringLiteral(",")));
        QString address;
        const QJsonArray lbIngress = arr(obj(status, QStringLiteral("loadBalancer")),
                                         QStringLiteral("ingress"));
        if (!lbIngress.isEmpty()) {
            const QJsonObject first = lbIngress.first().toObject();
            address = str(first, QStringLiteral("ip"));
            if (address.isEmpty())
                address = str(first, QStringLiteral("hostname"));
        }
        row.extra.insert(QStringLiteral("address"), address);
        row.status = address;
        return;
    }

    if (apiPlural == QLatin1String("endpoints")) {
        QStringList addrs;
        const QJsonArray subsets = arr(item, QStringLiteral("subsets"));
        for (const QJsonValue &s : subsets) {
            const QJsonObject so = s.toObject();
            QStringList ports;
            const QJsonArray portArr = arr(so, QStringLiteral("ports"));
            for (const QJsonValue &p : portArr)
                ports.append(QString::number(p.toObject().value(QStringLiteral("port")).toInt()));
            const QString portSuffix = ports.isEmpty()
                ? QString() : QStringLiteral(":%1").arg(ports.join(QStringLiteral(":")));
            const QJsonArray addresses = arr(so, QStringLiteral("addresses"));
            for (const QJsonValue &a : addresses) {
                const QString ip = str(a.toObject(), QStringLiteral("ip"));
                if (!ip.isEmpty())
                    addrs.append(ip + portSuffix);
            }
        }
        row.extra.insert(QStringLiteral("endpoints"), addrs.join(QStringLiteral(",")));
        row.status = QStringLiteral("%1 端点").arg(addrs.size());
        return;
    }

    if (apiPlural == QLatin1String("configmaps")) {
        const int dataCount = obj(item, QStringLiteral("data")).size()
                              + obj(item, QStringLiteral("binaryData")).size();
        row.status = QStringLiteral("%1 项").arg(dataCount);
        return;
    }

    if (apiPlural == QLatin1String("secrets")) {
        row.status = str(item, QStringLiteral("type"));
        row.extra.insert(QStringLiteral("data"),
                         QString::number(obj(item, QStringLiteral("data")).size()));
        return;
    }

    if (apiPlural == QLatin1String("persistentvolumeclaims")) {
        row.status = str(status, QStringLiteral("phase"));
        row.extra.insert(QStringLiteral("volume"), str(spec, QStringLiteral("volumeName")));
        row.extra.insert(QStringLiteral("capacity"),
                         str(obj(status, QStringLiteral("capacity")),
                             QStringLiteral("storage")));
        row.extra.insert(QStringLiteral("storageClass"),
                         str(spec, QStringLiteral("storageClassName")));
        return;
    }

    if (apiPlural == QLatin1String("persistentvolumes")) {
        row.status = str(status, QStringLiteral("phase"));
        row.extra.insert(QStringLiteral("capacity"),
                         str(obj(spec, QStringLiteral("capacity")),
                             QStringLiteral("storage")));
        const QJsonObject claimRef = obj(spec, QStringLiteral("claimRef"));
        if (!claimRef.isEmpty()) {
            row.extra.insert(QStringLiteral("claim"),
                             QStringLiteral("%1/%2")
                                 .arg(str(claimRef, QStringLiteral("namespace")),
                                      str(claimRef, QStringLiteral("name"))));
        }
        row.extra.insert(QStringLiteral("storageClass"),
                         str(spec, QStringLiteral("storageClassName")));
        row.extra.insert(QStringLiteral("reclaimPolicy"),
                         str(spec, QStringLiteral("persistentVolumeReclaimPolicy")));
        return;
    }

    if (apiPlural == QLatin1String("storageclasses")) {
        row.status = str(item, QStringLiteral("provisioner"));
        row.extra.insert(QStringLiteral("reclaimPolicy"),
                         str(item, QStringLiteral("reclaimPolicy")));
        row.extra.insert(QStringLiteral("volumeBindingMode"),
                         str(item, QStringLiteral("volumeBindingMode")));
        return;
    }

    if (apiPlural == QLatin1String("nodes")) {
        // Ready condition：status.conditions 里 type==Ready 的 status。
        QString readyState = QStringLiteral("Unknown");
        const QJsonArray conditions = arr(status, QStringLiteral("conditions"));
        for (const QJsonValue &c : conditions) {
            const QJsonObject co = c.toObject();
            if (str(co, QStringLiteral("type")) == QLatin1String("Ready")) {
                readyState = str(co, QStringLiteral("status")) == QLatin1String("True")
                    ? QStringLiteral("Ready") : QStringLiteral("NotReady");
                break;
            }
        }
        row.status = readyState;
        // 角色：labels 里 node-role.kubernetes.io/<role> 的 <role> 部分。
        QStringList roles;
        const QJsonObject labels = obj(obj(item, QStringLiteral("metadata")),
                                       QStringLiteral("labels"));
        const QString rolePrefix = QStringLiteral("node-role.kubernetes.io/");
        for (auto it = labels.constBegin(); it != labels.constEnd(); ++it) {
            if (it.key().startsWith(rolePrefix))
                roles.append(it.key().mid(rolePrefix.size()));
        }
        row.extra.insert(QStringLiteral("roles"),
                         roles.isEmpty() ? QStringLiteral("<none>") : roles.join(QStringLiteral(",")));
        row.extra.insert(QStringLiteral("version"),
                         str(obj(status, QStringLiteral("nodeInfo")),
                             QStringLiteral("kubeletVersion")));
        const QJsonArray addresses = arr(status, QStringLiteral("addresses"));
        for (const QJsonValue &a : addresses) {
            const QJsonObject ao = a.toObject();
            if (str(ao, QStringLiteral("type")) == QLatin1String("InternalIP")) {
                row.extra.insert(QStringLiteral("internalIP"),
                                 str(ao, QStringLiteral("address")));
                break;
            }
        }
        return;
    }

    if (apiPlural == QLatin1String("namespaces")) {
        row.status = str(status, QStringLiteral("phase"));
        return;
    }

    if (apiPlural == QLatin1String("events")) {
        row.status = str(item, QStringLiteral("type")); // "Warning" / "Normal"
        row.extra.insert(QStringLiteral("reason"), str(item, QStringLiteral("reason")));
        const QJsonObject involved = obj(item, QStringLiteral("involvedObject"));
        row.extra.insert(QStringLiteral("object"),
                         QStringLiteral("%1/%2")
                             .arg(str(involved, QStringLiteral("kind")),
                                  str(involved, QStringLiteral("name"))));
        row.extra.insert(QStringLiteral("message"), str(item, QStringLiteral("message")));
        const int count = item.value(QStringLiteral("count")).toInt();
        if (count > 0)
            row.extra.insert(QStringLiteral("count"), QString::number(count));
        // AGE 基准用 lastTimestamp（事件发生时间），没有才回退 creationTimestamp。
        const QString last = str(item, QStringLiteral("lastTimestamp"));
        if (!last.isEmpty()) {
            const QDateTime lastDt = QDateTime::fromString(last, Qt::ISODate);
            if (lastDt.isValid())
                *ageOverride = lastDt;
        }
        return;
    }

    // 未特化的资源：status.phase 兜底。
    row.status = str(status, QStringLiteral("phase"));
}

// 从第一个 '{' 起解析 JSON 文档（容忍 sudo 提示/stderr 混入的前缀行）。
QJsonDocument parseTolerant(const QByteArray &json)
{
    QByteArray trimmed = json.trimmed();
    const int start = trimmed.indexOf('{');
    if (start > 0)
        trimmed = trimmed.mid(start);
    QJsonParseError err;
    return QJsonDocument::fromJson(trimmed, &err);
}

} // namespace

// ---------------------------------------------------------------------------
// 注册表访问
// ---------------------------------------------------------------------------

QStringList KubeResourceParser::groupOrder()
{
    return {QStringLiteral("Workloads"), QStringLiteral("Network"),
            QStringLiteral("Config"), QStringLiteral("Storage"),
            QStringLiteral("Cluster")};
}

QList<KubeResourceKind> KubeResourceParser::allKinds()
{
    QList<KubeResourceKind> result;
    result.reserve(int(std::size(kKinds)));
    for (const KubeResourceKind &kind : kKinds)
        result.append(kind);
    return result;
}

const KubeResourceKind *KubeResourceParser::findKind(const QString &apiPlural)
{
    for (const KubeResourceKind &kind : kKinds) {
        if (kind.apiPlural == apiPlural)
            return &kind;
    }
    return nullptr;
}

QList<KubeResourceKind> KubeResourceParser::kindsInGroup(const QString &group)
{
    QList<KubeResourceKind> result;
    for (const KubeResourceKind &kind : kKinds) {
        if (kind.group == group)
            result.append(kind);
    }
    return result;
}

// ---------------------------------------------------------------------------
// 上下文 / 命名空间
// ---------------------------------------------------------------------------

QList<KubeContextInfo> KubeResourceParser::parseContextsJson(const QByteArray &json)
{
    QList<KubeContextInfo> result;
    const QJsonDocument doc = parseTolerant(json);
    if (!doc.isObject())
        return result;
    const QJsonObject root = doc.object();
    const QString current = str(root, QStringLiteral("current-context"));
    const QJsonArray contexts = arr(root, QStringLiteral("contexts"));
    for (const QJsonValue &value : contexts) {
        const QJsonObject entry = value.toObject();
        KubeContextInfo info;
        info.name = str(entry, QStringLiteral("name"));
        const QJsonObject ctx = obj(entry, QStringLiteral("context"));
        info.cluster = str(ctx, QStringLiteral("cluster"));
        info.user = str(ctx, QStringLiteral("user"));
        info.namespace_ = str(ctx, QStringLiteral("namespace"));
        info.isCurrent = (!current.isEmpty() && info.name == current);
        if (!info.name.isEmpty())
            result.append(info);
    }
    return result;
}

QStringList KubeResourceParser::parseNamespacesJson(const QByteArray &json)
{
    QStringList result;
    const QJsonDocument doc = parseTolerant(json);
    if (!doc.isObject())
        return result;
    const QJsonArray items = arr(doc.object(), QStringLiteral("items"));
    for (const QJsonValue &value : items) {
        const QString name = str(obj(value.toObject(), QStringLiteral("metadata")),
                                 QStringLiteral("name"));
        if (!name.isEmpty())
            result.append(name);
    }
    return result;
}

// ---------------------------------------------------------------------------
// 资源列表
// ---------------------------------------------------------------------------

QDateTime KubeResourceParser::parseCreationTimestamp(const QJsonObject &metadata)
{
    const QString ts = str(metadata, QStringLiteral("creationTimestamp"));
    if (ts.isEmpty())
        return QDateTime();
    return QDateTime::fromString(ts, Qt::ISODate);
}

QString KubeResourceParser::formatAge(const QDateTime &creationTimestamp,
                                      const QDateTime &now)
{
    if (!creationTimestamp.isValid() || !now.isValid())
        return QStringLiteral("-");
    qint64 secs = creationTimestamp.secsTo(now);
    if (secs < 0)
        return QStringLiteral("-");
    if (secs < 120)
        return QStringLiteral("%1s").arg(secs);
    const qint64 mins = secs / 60;
    if (mins < 120)
        return QStringLiteral("%1m").arg(mins);
    const qint64 hours = mins / 60;
    if (hours < 48)
        return QStringLiteral("%1h").arg(hours);
    return QStringLiteral("%1d").arg(hours / 24);
}

QList<KubeResourceRow> KubeResourceParser::parseResourceListJson(
    const QString &apiPlural, const QByteArray &json, const QDateTime &now)
{
    QList<KubeResourceRow> result;
    const QJsonDocument doc = parseTolerant(json);
    if (!doc.isObject())
        return result;
    const QJsonArray items = arr(doc.object(), QStringLiteral("items"));
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        const QJsonObject metadata = obj(item, QStringLiteral("metadata"));
        KubeResourceRow row;
        row.name = str(metadata, QStringLiteral("name"));
        if (row.name.isEmpty())
            continue;
        row.namespace_ = str(metadata, QStringLiteral("namespace"));
        QDateTime ageOverride;
        summarize(apiPlural, item, now, row, &ageOverride);
        const QDateTime base = ageOverride.isValid()
            ? ageOverride : parseCreationTimestamp(metadata);
        row.age = formatAge(base, now);
        result.append(row);
    }
    return result;
}

} // namespace cubeshell
