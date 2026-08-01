// ServerProfileBuilder.cpp — see ServerProfileBuilder.h.
//
// 对应Python: core/ai/server_profile.py

#include "ServerProfileBuilder.h"

#include "ssh/CommandExecutor.h"

#include <QMap>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtConcurrent>

#include <algorithm>

namespace cubeshell {

namespace {

// 单条复合命令，一次 SSH 往返取回全部信息（Python 侧为逐项 exec，往返 10+ 次）。
// 对应Python: ServerProfileBuilder._detect_* 系列的合并版本
const char *const kProbeScript =
    "echo \"===OS===\"; cat /etc/os-release 2>/dev/null || uname -a;"
    "echo \"===CPU===\"; cat /proc/loadavg 2>/dev/null;"
    "echo \"===MEM===\"; free -h 2>/dev/null;"
    "echo \"===DISK===\"; df -h / 2>/dev/null;"
    "echo \"===USER===\"; whoami 2>/dev/null;"
    "echo \"===PKG===\"; (which apt yum dnf pacman zypper 2>/dev/null) | head -5;"
    "echo \"===SVC===\"; systemctl list-units --type=service --state=running"
    " --no-pager 2>/dev/null | head -30;"
    "echo \"===NET===\"; ss -tlnp 2>/dev/null | head -20";

// 按 ===NAME=== 分隔符切分探测输出。未出现的段返回空列表。
QMap<QString, QStringList> splitSections(const QString &raw)
{
    static const QRegularExpression headerRe(
        QStringLiteral("^===([A-Z]+)===$"));

    QMap<QString, QStringList> sections;
    QString current;
    const QStringList lines = raw.split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        const QRegularExpressionMatch m = headerRe.match(line);
        if (m.hasMatch()) {
            current = m.captured(1);
            sections.insert(current, QStringList());
            continue;
        }
        if (current.isEmpty() || line.isEmpty())
            continue;
        sections[current].append(line);
    }
    return sections;
}

// os-release 的 KEY="value" → value（去引号）
QString osReleaseValue(const QStringList &lines, const QString &key)
{
    const QString prefix = key + QLatin1Char('=');
    for (const QString &line : lines) {
        if (!line.startsWith(prefix))
            continue;
        QString value = line.mid(prefix.size()).trimmed();
        if (value.size() >= 2
            && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))) {
            value = value.mid(1, value.size() - 2);
        }
        return value;
    }
    return QString();
}

// 对应Python: _detect_os_info（PRETTY_NAME → NAME+VERSION → uname 回退）
QString describeOs(const QStringList &lines)
{
    if (lines.isEmpty())
        return QString();

    const QString pretty = osReleaseValue(lines, QStringLiteral("PRETTY_NAME"));
    if (!pretty.isEmpty())
        return pretty;

    const QString name = osReleaseValue(lines, QStringLiteral("NAME"));
    const QString version = osReleaseValue(lines, QStringLiteral("VERSION"));
    if (!name.isEmpty())
        return version.isEmpty() ? name : name + QLatin1Char(' ') + version;

    // /etc/os-release 缺失时该段是 uname -a 的整行输出
    return lines.first();
}

// 对应Python: _fallback_cpu_stats（/proc/loadavg 的 1/5/15 分钟负载）
QString describeLoad(const QStringList &lines)
{
    if (lines.isEmpty())
        return QString();
    const QStringList parts = lines.first().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 3)
        return lines.first();
    return QStringLiteral("%1 / %2 / %3 (1/5/15 分钟)")
        .arg(parts.at(0), parts.at(1), parts.at(2));
}

// 对应Python: _fallback_memory_stats（这里直接用 free -h 的人类可读列）
QString describeMemory(const QStringList &lines)
{
    for (const QString &line : lines) {
        if (!line.startsWith(QLatin1String("Mem:"), Qt::CaseInsensitive))
            continue;
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 3)
            break;
        // free -h: Mem: total used free shared buff/cache available
        const QString available = parts.size() >= 7 ? parts.at(6) : parts.at(3);
        return QStringLiteral("总 %1，已用 %2，可用 %3")
            .arg(parts.at(1), parts.at(2), available);
    }
    return QString();
}

// 对应Python: _fallback_disk_stats（df 的 Size/Used/Use% 列）
QString describeDisk(const QStringList &lines)
{
    for (const QString &line : lines) {
        // 跳过表头
        if (line.startsWith(QLatin1String("Filesystem"), Qt::CaseInsensitive))
            continue;
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 5)
            continue;
        return QStringLiteral("总 %1，已用 %2 (%3)")
            .arg(parts.at(1), parts.at(2), parts.at(4));
    }
    return QString();
}

// which 输出的完整路径 → 去重后的命令名列表
// 对应Python: _detect_package_manager 的命令探测回退分支
QString describePackageManagers(const QStringList &lines)
{
    QStringList managers;
    for (const QString &line : lines) {
        const QString name = line.section(QLatin1Char('/'), -1).trimmed();
        if (!name.isEmpty() && !managers.contains(name))
            managers.append(name);
    }
    return managers.join(QStringLiteral(", "));
}

// 对应Python: _detect_running_services（systemctl 输出取 UNIT 列，去 .service 后缀）
QString describeServices(const QStringList &lines)
{
    QStringList services;
    for (const QString &line : lines) {
        // 表头与 head 截断后可能残留的图例行
        if (line.startsWith(QLatin1String("UNIT"))
            || line.startsWith(QLatin1String("LOAD"))
            || line.startsWith(QLatin1String("ACTIVE"))
            || line.startsWith(QLatin1String("SUB"))
            || line.startsWith(QLatin1String("Legend")))
            continue;
        QString unit = line.split(QLatin1Char(' '), Qt::SkipEmptyParts).value(0);
        // list-units 非 --plain 模式会前置 "●" 状态符
        if (unit == QLatin1String("\u25CF") || unit == QLatin1String("*"))
            unit = line.split(QLatin1Char(' '), Qt::SkipEmptyParts).value(1);
        if (!unit.endsWith(QLatin1String(".service")))
            continue;
        unit.chop(int(sizeof(".service") - 1));
        if (!unit.isEmpty() && !services.contains(unit))
            services.append(unit);
    }
    return services.join(QStringLiteral(", "));
}

// 对应Python: _detect_open_ports（本地地址列尾部的 :port，排序去重）
QString describePorts(const QStringList &lines)
{
    static const QRegularExpression portRe(QStringLiteral(":(\\d+)$"));

    QSet<int> ports;
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("State"))
            || line.startsWith(QLatin1String("Netid"))
            || line.startsWith(QLatin1String("Proto")))
            continue;
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const QRegularExpressionMatch m = portRe.match(part);
            if (!m.hasMatch())
                continue;
            const int port = m.captured(1).toInt();
            if (port >= 1 && port <= 65535)
                ports.insert(port);
            break;  // 只取每行第一个地址列（本地地址）
        }
    }

    QList<int> sorted = ports.values();
    std::sort(sorted.begin(), sorted.end());
    QStringList text;
    text.reserve(sorted.size());
    for (int port : sorted)
        text.append(QString::number(port));
    return text.join(QStringLiteral(", "));
}

} // namespace

ServerProfileBuilder::ServerProfileBuilder(CommandExecutor *executor,
                                           QObject *parent)
    : QObject(parent)
    , m_executor(executor)
{
}

ServerProfileBuilder::~ServerProfileBuilder()
{
    // 线程池任务持 QPointer 回调本对象，析构前必须等其退出。
    if (m_probe.isRunning())
        m_probe.waitForFinished();
}

bool ServerProfileBuilder::isReady() const
{
    return m_hasBuilt && m_buildTime.isValid()
           && m_buildTime.elapsed() < kProfileTtlMs;
}

void ServerProfileBuilder::emitProfileDeferred()
{
    QPointer<ServerProfileBuilder> guard(this);
    QMetaObject::invokeMethod(this, [guard]() {
        if (guard)
            emit guard->profileReady(guard->m_profile);
    }, Qt::QueuedConnection);
}

// 对应Python: ServerProfile.build_async
void ServerProfileBuilder::buildAsync()
{
    if (m_building)
        return;
    if (!m_executor) {
        m_profile.clear();
        emitProfileDeferred();
        return;
    }
    // 缓存命中：复用上次结果，不再打扰远端
    if (isReady()) {
        emitProfileDeferred();
        return;
    }

    m_building = true;

    CommandExecutor *executor = m_executor;
    QPointer<ServerProfileBuilder> guard(this);
    m_probe = QtConcurrent::run([executor, guard]() {
        // exec() 阻塞：必须在线程池线程上跑，否则冻结 UI
        const ExecResult res = executor->exec(QString::fromLatin1(kProbeScript),
                                              /*pty=*/false, kProbeTimeoutMs);
        // 探测失败（断连/超时）时回传空串 → 空画像，不影响主流程
        const QString output = res.ok() ? res.stdoutText : QString();
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard, [guard, output]() {
            if (guard)
                guard->onProbeFinished(output);
        });
    });
}

// 解析分段输出并格式化为可读画像。
// 对应Python: ServerProfileBuilder.build_system_context_prompt
void ServerProfileBuilder::onProbeFinished(const QString &result)
{
    m_building = false;
    m_hasBuilt = true;
    m_buildTime.restart();

    const QMap<QString, QStringList> sections = splitSections(result);

    QStringList lines;
    const auto appendField = [&lines](const QString &label, const QString &value) {
        if (!value.isEmpty())
            lines.append(QStringLiteral("- %1: %2").arg(label, value));
    };

    appendField(QStringLiteral("操作系统"),
                describeOs(sections.value(QStringLiteral("OS"))));
    appendField(QStringLiteral("CPU 负载"),
                describeLoad(sections.value(QStringLiteral("CPU"))));
    appendField(QStringLiteral("内存"),
                describeMemory(sections.value(QStringLiteral("MEM"))));
    appendField(QStringLiteral("磁盘 (/)"),
                describeDisk(sections.value(QStringLiteral("DISK"))));
    appendField(QStringLiteral("当前用户"),
                sections.value(QStringLiteral("USER")).value(0));
    appendField(QStringLiteral("包管理器"),
                describePackageManagers(sections.value(QStringLiteral("PKG"))));
    appendField(QStringLiteral("运行中的服务 (前30)"),
                describeServices(sections.value(QStringLiteral("SVC"))));
    appendField(QStringLiteral("监听端口 (前20)"),
                describePorts(sections.value(QStringLiteral("NET"))));

    // 一条字段都没解析出来时保持空画像，避免注入无意义的标题
    m_profile = lines.isEmpty()
                    ? QString()
                    : QStringLiteral("## 服务器环境\n") + lines.join(QLatin1Char('\n'));

    emit profileReady(m_profile);
}

} // namespace cubeshell
