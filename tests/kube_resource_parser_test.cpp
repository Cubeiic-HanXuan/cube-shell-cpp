// KubeResourceParser unit test (pure logic — no kubectl, no network):
// 注册表访问、formatAge、parseContextsJson / parseNamespacesJson 的容错解析、
// 各资源种类的 summarize（ready 计数 / waiting.reason 优先 / conditions 终态 /
// 端口与外部地址拼装 / 节点角色 / 事件 lastTimestamp 基准）。
// 对应 docs/Kubernetes功能实现方案.md §10.1。

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QString>
#include <QStringList>

#include "kube/KubeResourceParser.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 固定 "now"，AGE 换算全部相对它断言。
static const QDateTime kNow =
    QDateTime::fromString(QStringLiteral("2024-06-01T12:00:00Z"), Qt::ISODate);

static void testRegistry()
{
    const QStringList groups = KubeResourceParser::groupOrder();
    CHECK(groups == QStringList({QStringLiteral("Workloads"), QStringLiteral("Network"),
                                 QStringLiteral("Config"), QStringLiteral("Storage"),
                                 QStringLiteral("Cluster")}));

    const KubeResourceKind *pods = KubeResourceParser::findKind(QStringLiteral("pods"));
    CHECK(pods != nullptr);
    CHECK(pods->namespaced);
    CHECK(pods->group == QStringLiteral("Workloads"));
    CHECK(!pods->scalable && !pods->restartable);

    const KubeResourceKind *deploy = KubeResourceParser::findKind(QStringLiteral("deployments"));
    CHECK(deploy != nullptr && deploy->scalable && deploy->restartable);
    const KubeResourceKind *ds = KubeResourceParser::findKind(QStringLiteral("daemonsets"));
    CHECK(ds != nullptr && !ds->scalable && ds->restartable);
    const KubeResourceKind *sts = KubeResourceParser::findKind(QStringLiteral("statefulsets"));
    CHECK(sts != nullptr && sts->scalable && sts->restartable);
    const KubeResourceKind *rs = KubeResourceParser::findKind(QStringLiteral("replicasets"));
    CHECK(rs != nullptr && rs->scalable && !rs->restartable);

    const KubeResourceKind *nodes = KubeResourceParser::findKind(QStringLiteral("nodes"));
    CHECK(nodes != nullptr && !nodes->namespaced);
    const KubeResourceKind *pv = KubeResourceParser::findKind(QStringLiteral("persistentvolumes"));
    CHECK(pv != nullptr && !pv->namespaced);
    const KubeResourceKind *pvc = KubeResourceParser::findKind(QStringLiteral("persistentvolumeclaims"));
    CHECK(pvc != nullptr && pvc->namespaced);
    // events 在 Cluster 组但按命名空间过滤（kubectl get events -n <ns>）。
    const KubeResourceKind *events = KubeResourceParser::findKind(QStringLiteral("events"));
    CHECK(events != nullptr && events->namespaced
          && events->group == QStringLiteral("Cluster"));

    CHECK(KubeResourceParser::findKind(QStringLiteral("frobnicate")) == nullptr);

    const QList<KubeResourceKind> workloadKinds =
        KubeResourceParser::kindsInGroup(QStringLiteral("Workloads"));
    CHECK(workloadKinds.size() == 7);
    CHECK(workloadKinds.first().apiPlural == QStringLiteral("pods"));

    // 注册表与分组一一对应：allKinds 数量 = 各分组之和。
    int total = 0;
    for (const QString &g : groups)
        total += KubeResourceParser::kindsInGroup(g).size();
    CHECK(total == KubeResourceParser::allKinds().size());
}

static void testFormatAge()
{
    using KubeResourceParser::formatAge;
    const QDateTime base = kNow;
    CHECK(formatAge(base.addSecs(-45), kNow) == QStringLiteral("45s"));
    CHECK(formatAge(base.addSecs(-119), kNow) == QStringLiteral("119s"));
    CHECK(formatAge(base.addSecs(-120), kNow) == QStringLiteral("2m"));
    CHECK(formatAge(base.addSecs(-90 * 60), kNow) == QStringLiteral("90m"));
    CHECK(formatAge(base.addSecs(-5 * 3600), kNow) == QStringLiteral("5h"));
    CHECK(formatAge(base.addSecs(-47 * 3600), kNow) == QStringLiteral("47h"));
    CHECK(formatAge(base.addSecs(-48 * 3600), kNow) == QStringLiteral("2d"));
    CHECK(formatAge(base.addSecs(-30 * 24 * 3600), kNow) == QStringLiteral("30d"));
    // 未来时间与无效输入 → "-"。
    CHECK(formatAge(base.addSecs(60), kNow) == QStringLiteral("-"));
    CHECK(formatAge(QDateTime(), kNow) == QStringLiteral("-"));
    CHECK(formatAge(base, QDateTime()) == QStringLiteral("-"));
}

static void testParseContexts()
{
    // sudo 提示前缀 + 缺 namespace 字段 + current-context 命中。
    const QByteArray json =
        "[sudo] password for ops:\n"
        "{\n"
        "  \"current-context\": \"prod\",\n"
        "  \"contexts\": [\n"
        "    {\"name\": \"minikube\", \"context\": {\"cluster\": \"minikube\", \"user\": \"minikube\", \"namespace\": \"apps\"}},\n"
        "    {\"name\": \"prod\", \"context\": {\"cluster\": \"prod-cluster\", \"user\": \"ops\"}},\n"
        "    {\"name\": \"\", \"context\": {\"cluster\": \"skip-me\"}}\n"
        "  ]\n"
        "}\n";
    const QList<KubeContextInfo> contexts = KubeResourceParser::parseContextsJson(json);
    CHECK(contexts.size() == 2); // 空名条目丢弃
    CHECK(contexts.at(0).name == QStringLiteral("minikube"));
    CHECK(contexts.at(0).cluster == QStringLiteral("minikube"));
    CHECK(contexts.at(0).namespace_ == QStringLiteral("apps"));
    CHECK(!contexts.at(0).isCurrent);
    CHECK(contexts.at(1).name == QStringLiteral("prod"));
    CHECK(contexts.at(1).namespace_.isEmpty());
    CHECK(contexts.at(1).isCurrent);

    // 无 current-context 字段 → 全部 isCurrent=false；坏 JSON → 空列表。
    CHECK(KubeResourceParser::parseContextsJson(
              "{\"contexts\":[{\"name\":\"a\",\"context\":{}}]}").size() == 1);
    CHECK(KubeResourceParser::parseContextsJson("not json at all").isEmpty());
    CHECK(KubeResourceParser::parseContextsJson(QByteArray()).isEmpty());
}

static void testParseNamespaces()
{
    const QByteArray json =
        "{\"items\": ["
        "  {\"metadata\": {\"name\": \"default\"}},"
        "  {\"metadata\": {\"name\": \"kube-system\"}},"
        "  {\"metadata\": {}}"
        "]}";
    const QStringList ns = KubeResourceParser::parseNamespacesJson(json);
    CHECK(ns == QStringList({QStringLiteral("default"), QStringLiteral("kube-system")}));
    CHECK(KubeResourceParser::parseNamespacesJson("garbage").isEmpty());
}

static void testParsePods()
{
    const QByteArray json =
        "{\"items\": [\n"
        // 多容器 Pod：1/2 ready，一个 CrashLoopBackOff（waiting.reason 优先于 phase）。
        "  {\"metadata\": {\"name\": \"web-7d9\", \"namespace\": \"apps\",\n"
        "                  \"creationTimestamp\": \"2024-05-29T12:00:00Z\"},\n"
        "   \"spec\": {\"nodeName\": \"node1\", \"containers\": [{\"name\": \"app\"}, {\"name\": \"sidecar\"}]},\n"
        "   \"status\": {\"phase\": \"Running\", \"podIP\": \"10.244.0.5\",\n"
        "     \"containerStatuses\": [\n"
        "       {\"name\": \"app\", \"ready\": true, \"restartCount\": 0, \"state\": {\"running\": {}}},\n"
        "       {\"name\": \"sidecar\", \"ready\": false, \"restartCount\": 7,\n"
        "        \"state\": {\"waiting\": {\"reason\": \"CrashLoopBackOff\"}}}\n"
        "     ]}},\n"
        // 正常 1/1 Running Pod。
        "  {\"metadata\": {\"name\": \"db-0\", \"namespace\": \"apps\",\n"
        "                  \"creationTimestamp\": \"2024-06-01T06:00:00Z\"},\n"
        "   \"spec\": {\"nodeName\": \"node2\", \"containers\": [{\"name\": \"mysql\"}]},\n"
        "   \"status\": {\"phase\": \"Running\", \"podIP\": \"10.244.1.9\",\n"
        "     \"containerStatuses\": [{\"name\": \"mysql\", \"ready\": true, \"restartCount\": 2,\n"
        "                              \"state\": {\"running\": {}}}]}} ,\n"
        // 删除中的 Pod：deletionTimestamp → Terminating。
        "  {\"metadata\": {\"name\": \"old-1\", \"namespace\": \"apps\",\n"
        "                  \"creationTimestamp\": \"2024-05-01T00:00:00Z\",\n"
        "                  \"deletionTimestamp\": \"2024-06-01T11:59:00Z\"},\n"
        "   \"spec\": {\"nodeName\": \"node1\", \"containers\": [{\"name\": \"x\"}]},\n"
        "   \"status\": {\"phase\": \"Running\",\n"
        "     \"containerStatuses\": [{\"name\": \"x\", \"ready\": true, \"restartCount\": 0,\n"
        "                              \"state\": {\"running\": {}}}]}}\n"
        "]}";
    const QList<KubeResourceRow> rows =
        KubeResourceParser::parseResourceListJson(QStringLiteral("pods"), json, kNow);
    CHECK(rows.size() == 3);

    CHECK(rows.at(0).name == QStringLiteral("web-7d9"));
    CHECK(rows.at(0).namespace_ == QStringLiteral("apps"));
    CHECK(rows.at(0).status == QStringLiteral("1/2 CrashLoopBackOff"));
    CHECK(rows.at(0).age == QStringLiteral("3d"));
    CHECK(rows.at(0).containers == QStringList({QStringLiteral("app"), QStringLiteral("sidecar")}));
    CHECK(rows.at(0).nodeName == QStringLiteral("node1"));
    CHECK(rows.at(0).extra.value(QStringLiteral("ip")) == QStringLiteral("10.244.0.5"));
    CHECK(rows.at(0).extra.value(QStringLiteral("restarts")) == QStringLiteral("7"));

    CHECK(rows.at(1).status == QStringLiteral("1/1 Running"));
    CHECK(rows.at(1).age == QStringLiteral("6h"));
    CHECK(rows.at(1).extra.value(QStringLiteral("restarts")) == QStringLiteral("2"));

    CHECK(rows.at(2).status == QStringLiteral("1/1 Terminating"));
    CHECK(rows.at(2).age == QStringLiteral("31d"));

    // 空输出 / 坏 JSON 容错。
    CHECK(KubeResourceParser::parseResourceListJson(
              QStringLiteral("pods"), QByteArray(), kNow).isEmpty());
    CHECK(KubeResourceParser::parseResourceListJson(
              QStringLiteral("pods"), "{broken", kNow).isEmpty());
    // 无名条目丢弃。
    CHECK(KubeResourceParser::parseResourceListJson(
              QStringLiteral("pods"), "{\"items\":[{\"metadata\":{}}]}", kNow).isEmpty());
}

static void testParseWorkloads()
{
    // Deployment：readyReplicas/desired + template 镜像列表；spec.replicas 缺省 = 1。
    const QByteArray deployJson =
        "{\"items\": ["
        "  {\"metadata\": {\"name\": \"web\", \"namespace\": \"apps\",\n"
        "                  \"creationTimestamp\": \"2024-05-30T12:00:00Z\"},\n"
        "   \"spec\": {\"replicas\": 3, \"template\": {\"spec\": {\"containers\": [\n"
        "       {\"name\": \"app\", \"image\": \"nginx:1.25\"}]}}},\n"
        "   \"status\": {\"readyReplicas\": 2}},"
        "  {\"metadata\": {\"name\": \"single\", \"namespace\": \"apps\",\n"
        "                  \"creationTimestamp\": \"2024-06-01T11:00:00Z\"},\n"
        "   \"spec\": {\"template\": {\"spec\": {\"containers\": []}}},\n"
        "   \"status\": {}}"
        "]}";
    const QList<KubeResourceRow> deploys =
        KubeResourceParser::parseResourceListJson(QStringLiteral("deployments"), deployJson, kNow);
    CHECK(deploys.size() == 2);
    CHECK(deploys.at(0).status == QStringLiteral("2/3"));
    CHECK(deploys.at(0).extra.value(QStringLiteral("images")) == QStringLiteral("nginx:1.25"));
    CHECK(deploys.at(0).age == QStringLiteral("2d"));
    CHECK(deploys.at(1).status == QStringLiteral("0/1")); // replicas 缺省 1

    // DaemonSet：numberReady/desiredNumberScheduled。
    const QByteArray dsJson =
        "{\"items\": [{\"metadata\": {\"name\": \"agent\", \"namespace\": \"kube-system\"},"
        "  \"spec\": {\"template\": {\"spec\": {\"containers\": []}}},"
        "  \"status\": {\"desiredNumberScheduled\": 4, \"numberReady\": 4}}]}";
    const QList<KubeResourceRow> dss =
        KubeResourceParser::parseResourceListJson(QStringLiteral("daemonsets"), dsJson, kNow);
    CHECK(dss.size() == 1 && dss.first().status == QStringLiteral("4/4"));

    // Job：Complete / Failed / Running 三态。
    const QByteArray jobJson =
        "{\"items\": ["
        "  {\"metadata\": {\"name\": \"done\", \"namespace\": \"apps\"}, \"spec\": {},\n"
        "   \"status\": {\"succeeded\": 1, \"conditions\": [{\"type\": \"Complete\", \"status\": \"True\"}]}},"
        "  {\"metadata\": {\"name\": \"bad\", \"namespace\": \"apps\"}, \"spec\": {},\n"
        "   \"status\": {\"conditions\": [{\"type\": \"Failed\", \"status\": \"True\"}]}},"
        "  {\"metadata\": {\"name\": \"run\", \"namespace\": \"apps\"}, \"spec\": {\"completions\": 4},\n"
        "   \"status\": {\"succeeded\": 2}}"
        "]}";
    const QList<KubeResourceRow> jobs =
        KubeResourceParser::parseResourceListJson(QStringLiteral("jobs"), jobJson, kNow);
    CHECK(jobs.size() == 3);
    CHECK(jobs.at(0).status == QStringLiteral("1/1 Complete"));
    CHECK(jobs.at(1).status == QStringLiteral("0/1 Failed"));
    CHECK(jobs.at(2).status == QStringLiteral("2/4 Running"));

    // CronJob：suspend 状态 + schedule + lastScheduleTime 换算。
    const QByteArray cjJson =
        "{\"items\": ["
        "  {\"metadata\": {\"name\": \"backup\", \"namespace\": \"apps\"},"
        "   \"spec\": {\"schedule\": \"0 2 * * *\", \"suspend\": false},"
        "   \"status\": {\"lastScheduleTime\": \"2024-06-01T02:00:00Z\"}},"
        "  {\"metadata\": {\"name\": \"paused\", \"namespace\": \"apps\"},"
        "   \"spec\": {\"schedule\": \"*/5 * * * *\", \"suspend\": true}, \"status\": {}}"
        "]}";
    const QList<KubeResourceRow> cjs =
        KubeResourceParser::parseResourceListJson(QStringLiteral("cronjobs"), cjJson, kNow);
    CHECK(cjs.size() == 2);
    CHECK(cjs.at(0).status == QStringLiteral("Active"));
    CHECK(cjs.at(0).extra.value(QStringLiteral("schedule")) == QStringLiteral("0 2 * * *"));
    CHECK(cjs.at(0).extra.value(QStringLiteral("lastSchedule")) == QStringLiteral("10h"));
    CHECK(cjs.at(1).status == QStringLiteral("Suspended"));
}

static void testParseNetwork()
{
    // Service：type / clusterIP / 端口拼装（targetPort 不同才显示）/ LB 外部地址。
    const QByteArray svcJson =
        "{\"items\": ["
        "  {\"metadata\": {\"name\": \"web\", \"namespace\": \"apps\",\n"
        "                  \"creationTimestamp\": \"2024-05-01T00:00:00Z\"},\n"
        "   \"spec\": {\"type\": \"LoadBalancer\", \"clusterIP\": \"10.96.0.10\",\n"
        "     \"ports\": [{\"port\": 80, \"targetPort\": 8080, \"protocol\": \"TCP\"},\n"
        "                 {\"port\": 443, \"protocol\": \"TCP\"}]},\n"
        "   \"status\": {\"loadBalancer\": {\"ingress\": [{\"ip\": \"1.2.3.4\"}]}}},"
        "  {\"metadata\": {\"name\": \"internal\", \"namespace\": \"apps\"},\n"
        "   \"spec\": {\"clusterIP\": \"10.96.0.20\", \"ports\": []},\n"
        "   \"status\": {}}"
        "]}";
    const QList<KubeResourceRow> svcs =
        KubeResourceParser::parseResourceListJson(QStringLiteral("services"), svcJson, kNow);
    CHECK(svcs.size() == 2);
    CHECK(svcs.at(0).status == QStringLiteral("LoadBalancer"));
    CHECK(svcs.at(0).extra.value(QStringLiteral("clusterIP")) == QStringLiteral("10.96.0.10"));
    CHECK(svcs.at(0).extra.value(QStringLiteral("ports"))
          == QStringLiteral("80:8080/TCP,443/TCP"));
    CHECK(svcs.at(0).extra.value(QStringLiteral("externalIP")) == QStringLiteral("1.2.3.4"));
    CHECK(svcs.at(1).status == QStringLiteral("ClusterIP")); // type 缺省
    CHECK(svcs.at(1).extra.value(QStringLiteral("externalIP")).isEmpty());

    // Ingress：hosts 汇总 + LB 地址。
    const QByteArray ingJson =
        "{\"items\": [{\"metadata\": {\"name\": \"web\", \"namespace\": \"apps\"},"
        "  \"spec\": {\"rules\": [{\"host\": \"a.example.com\"}, {\"host\": \"b.example.com\"}]},"
        "  \"status\": {\"loadBalancer\": {\"ingress\": [{\"hostname\": \"lb.example.com\"}]}}}]}";
    const QList<KubeResourceRow> ings =
        KubeResourceParser::parseResourceListJson(QStringLiteral("ingresses"), ingJson, kNow);
    CHECK(ings.size() == 1);
    CHECK(ings.first().extra.value(QStringLiteral("hosts"))
          == QStringLiteral("a.example.com,b.example.com"));
    CHECK(ings.first().status == QStringLiteral("lb.example.com"));

    // Endpoints：subset 地址 + 端口汇总。
    const QByteArray epJson =
        "{\"items\": [{\"metadata\": {\"name\": \"web\", \"namespace\": \"apps\"},"
        "  \"subsets\": [{\"addresses\": [{\"ip\": \"10.244.0.5\"}, {\"ip\": \"10.244.0.6\"}],"
        "                 \"ports\": [{\"port\": 8080}]}]}]}";
    const QList<KubeResourceRow> eps =
        KubeResourceParser::parseResourceListJson(QStringLiteral("endpoints"), epJson, kNow);
    CHECK(eps.size() == 1);
    CHECK(eps.first().extra.value(QStringLiteral("endpoints"))
          == QStringLiteral("10.244.0.5:8080,10.244.0.6:8080"));
    CHECK(eps.first().status == QStringLiteral("2 端点"));
}

static void testParseConfigAndStorage()
{
    // ConfigMap：data + binaryData 计数。
    const QByteArray cmJson =
        "{\"items\": [{\"metadata\": {\"name\": \"app-conf\", \"namespace\": \"apps\"},"
        "  \"data\": {\"a\": \"1\", \"b\": \"2\"}, \"binaryData\": {\"bin\": \"AA==\"}}]}";
    const QList<KubeResourceRow> cms =
        KubeResourceParser::parseResourceListJson(QStringLiteral("configmaps"), cmJson, kNow);
    CHECK(cms.size() == 1 && cms.first().status == QStringLiteral("3 项"));

    // Secret：type + data 计数。
    const QByteArray secJson =
        "{\"items\": [{\"metadata\": {\"name\": \"tls\", \"namespace\": \"apps\"},"
        "  \"type\": \"kubernetes.io/tls\", \"data\": {\"tls.crt\": \"x\", \"tls.key\": \"y\"}}]}";
    const QList<KubeResourceRow> secs =
        KubeResourceParser::parseResourceListJson(QStringLiteral("secrets"), secJson, kNow);
    CHECK(secs.size() == 1);
    CHECK(secs.first().status == QStringLiteral("kubernetes.io/tls"));
    CHECK(secs.first().extra.value(QStringLiteral("data")) == QStringLiteral("2"));

    // PVC：phase / volume / capacity / storageClass。
    const QByteArray pvcJson =
        "{\"items\": [{\"metadata\": {\"name\": \"data\", \"namespace\": \"apps\"},"
        "  \"spec\": {\"volumeName\": \"pv-1\", \"storageClassName\": \"fast\"},"
        "  \"status\": {\"phase\": \"Bound\", \"capacity\": {\"storage\": \"10Gi\"}}}]}";
    const QList<KubeResourceRow> pvcs =
        KubeResourceParser::parseResourceListJson(
            QStringLiteral("persistentvolumeclaims"), pvcJson, kNow);
    CHECK(pvcs.size() == 1);
    CHECK(pvcs.first().status == QStringLiteral("Bound"));
    CHECK(pvcs.first().extra.value(QStringLiteral("volume")) == QStringLiteral("pv-1"));
    CHECK(pvcs.first().extra.value(QStringLiteral("capacity")) == QStringLiteral("10Gi"));
    CHECK(pvcs.first().extra.value(QStringLiteral("storageClass")) == QStringLiteral("fast"));

    // PV：phase / capacity / claim(ns/name)。
    const QByteArray pvJson =
        "{\"items\": [{\"metadata\": {\"name\": \"pv-1\"},"
        "  \"spec\": {\"capacity\": {\"storage\": \"10Gi\"}, \"storageClassName\": \"fast\","
        "    \"persistentVolumeReclaimPolicy\": \"Retain\","
        "    \"claimRef\": {\"namespace\": \"apps\", \"name\": \"data\"}},"
        "  \"status\": {\"phase\": \"Bound\"}}]}";
    const QList<KubeResourceRow> pvs =
        KubeResourceParser::parseResourceListJson(
            QStringLiteral("persistentvolumes"), pvJson, kNow);
    CHECK(pvs.size() == 1);
    CHECK(pvs.first().namespace_.isEmpty()); // 非 namespaced
    CHECK(pvs.first().extra.value(QStringLiteral("claim")) == QStringLiteral("apps/data"));
    CHECK(pvs.first().extra.value(QStringLiteral("reclaimPolicy")) == QStringLiteral("Retain"));

    // StorageClass：provisioner 进状态列。
    const QByteArray scJson =
        "{\"items\": [{\"metadata\": {\"name\": \"fast\"},"
        "  \"provisioner\": \"kubernetes.io/no-provisioner\","
        "  \"reclaimPolicy\": \"Delete\", \"volumeBindingMode\": \"WaitForFirstConsumer\"}]}";
    const QList<KubeResourceRow> scs =
        KubeResourceParser::parseResourceListJson(QStringLiteral("storageclasses"), scJson, kNow);
    CHECK(scs.size() == 1);
    CHECK(scs.first().status == QStringLiteral("kubernetes.io/no-provisioner"));
    CHECK(scs.first().extra.value(QStringLiteral("volumeBindingMode"))
          == QStringLiteral("WaitForFirstConsumer"));
}

static void testParseCluster()
{
    // Node：Ready condition / 角色标签 / kubelet 版本 / InternalIP。
    const QByteArray nodeJson =
        "{\"items\": ["
        "  {\"metadata\": {\"name\": \"node1\", \"creationTimestamp\": \"2024-01-01T00:00:00Z\",\n"
        "     \"labels\": {\"node-role.kubernetes.io/control-plane\": \"\", \"other\": \"x\"}},\n"
        "   \"status\": {\"conditions\": [{\"type\": \"Ready\", \"status\": \"True\"}],\n"
        "     \"nodeInfo\": {\"kubeletVersion\": \"v1.29.0\"},\n"
        "     \"addresses\": [{\"type\": \"InternalIP\", \"address\": \"192.168.49.2\"},\n"
        "                     {\"type\": \"Hostname\", \"address\": \"node1\"}]}},"
        "  {\"metadata\": {\"name\": \"node2\", \"labels\": {}},\n"
        "   \"status\": {\"conditions\": [{\"type\": \"Ready\", \"status\": \"False\"}],\n"
        "     \"nodeInfo\": {}, \"addresses\": []}}"
        "]}";
    const QList<KubeResourceRow> nodes =
        KubeResourceParser::parseResourceListJson(QStringLiteral("nodes"), nodeJson, kNow);
    CHECK(nodes.size() == 2);
    CHECK(nodes.at(0).status == QStringLiteral("Ready"));
    CHECK(nodes.at(0).extra.value(QStringLiteral("roles")) == QStringLiteral("control-plane"));
    CHECK(nodes.at(0).extra.value(QStringLiteral("version")) == QStringLiteral("v1.29.0"));
    CHECK(nodes.at(0).extra.value(QStringLiteral("internalIP")) == QStringLiteral("192.168.49.2"));
    CHECK(nodes.at(1).status == QStringLiteral("NotReady"));
    CHECK(nodes.at(1).extra.value(QStringLiteral("roles")) == QStringLiteral("<none>"));

    // Namespace：phase。
    const QByteArray nsJson =
        "{\"items\": [{\"metadata\": {\"name\": \"default\"}, \"status\": {\"phase\": \"Active\"}}]}";
    const QList<KubeResourceRow> nss =
        KubeResourceParser::parseResourceListJson(QStringLiteral("namespaces"), nsJson, kNow);
    CHECK(nss.size() == 1 && nss.first().status == QStringLiteral("Active"));

    // Event：type/reason/object/message；AGE 以 lastTimestamp 为基准。
    const QByteArray evJson =
        "{\"items\": [{\"metadata\": {\"name\": \"web.17e\", \"namespace\": \"apps\",\n"
        "                 \"creationTimestamp\": \"2024-06-01T01:00:00Z\"},\n"
        "   \"type\": \"Warning\", \"reason\": \"BackOff\", \"count\": 5,\n"
        "   \"message\": \"Back-off restarting failed container\",\n"
        "   \"involvedObject\": {\"kind\": \"Pod\", \"name\": \"web-7d9\"},\n"
        "   \"lastTimestamp\": \"2024-06-01T11:30:00Z\"}]}";
    const QList<KubeResourceRow> evs =
        KubeResourceParser::parseResourceListJson(QStringLiteral("events"), evJson, kNow);
    CHECK(evs.size() == 1);
    CHECK(evs.first().status == QStringLiteral("Warning"));
    CHECK(evs.first().extra.value(QStringLiteral("reason")) == QStringLiteral("BackOff"));
    CHECK(evs.first().extra.value(QStringLiteral("object")) == QStringLiteral("Pod/web-7d9"));
    CHECK(evs.first().extra.value(QStringLiteral("message"))
          == QStringLiteral("Back-off restarting failed container"));
    CHECK(evs.first().extra.value(QStringLiteral("count")) == QStringLiteral("5"));
    CHECK(evs.first().age == QStringLiteral("30m")); // lastTimestamp 而非 creationTimestamp
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testRegistry();
    testFormatAge();
    testParseContexts();
    testParseNamespaces();
    testParsePods();
    testParseWorkloads();
    testParseNetwork();
    testParseConfigAndStorage();
    testParseCluster();
    if (failures == 0) {
        qInfo() << "kube_resource_parser_test: ALL PASS";
        return 0;
    }
    qWarning() << "kube_resource_parser_test:" << failures << "FAILURES";
    return 1;
}
