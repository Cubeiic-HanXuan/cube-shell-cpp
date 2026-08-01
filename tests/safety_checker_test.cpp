// CommandSafetyChecker unit test: CRITICAL/HIGH/SAFE/MEDIUM/LOW rule
// coverage, env-prefix stripping, batch checking, plus the SshAiAgent
// text-fallback command extraction (CJK / list-item / comment filtering).
// Pure logic — no network, no SSH.
//
// 对应Python: core/ai/safety.py + ssh_agent.py::_extract_commands_from_text
// 的行为对照

#include <QDebug>
#include <QString>
#include <QStringList>

#include "ai/CommandSafetyChecker.h"
#include "ai/SshAiAgent.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static const CommandSafetyChecker &checker()
{
    static const CommandSafetyChecker instance;
    return instance;
}

// CRITICAL 黑名单：阻止且 isAllowed=false
static void testCriticalRules()
{
    const QStringList blocked = {
        QStringLiteral("rm -rf /"),
        QStringLiteral("rm -fr / "),
        QStringLiteral("sudo rm -rf /*"),
        QStringLiteral("dd if=/dev/zero of=/dev/sda"),
        QStringLiteral("mkfs.ext4 /dev/sdb1"),
        QStringLiteral("wipefs -a /dev/sda"),
        QStringLiteral(":(){ :|: & };:"),
        QStringLiteral("chmod -R 777 /"),
        QStringLiteral("chown -R nobody:nobody /"),
        QStringLiteral("cat foo > /dev/sda"),
    };
    for (const QString &cmd : blocked) {
        const SafetyCheckResult r = checker().check(cmd);
        CHECK(r.riskLevel == RiskLevel::Critical);
        CHECK(!r.isAllowed);
        CHECK(r.reason.startsWith(QStringLiteral("命令被阻止")));
        CHECK(!r.warnings.isEmpty());
    }

    // 非根目录的 rm -rf 不该被 CRITICAL 拦截
    const SafetyCheckResult ok = checker().check(
        QStringLiteral("rm -rf /tmp/build"));
    CHECK(ok.riskLevel != RiskLevel::Critical);
    CHECK(ok.isAllowed);
}

// HIGH：允许但强制确认
static void testHighRules()
{
    const QStringList high = {
        QStringLiteral("shutdown -h now"),
        QStringLiteral("reboot"),
        QStringLiteral("halt"),
        QStringLiteral("poweroff"),
        QStringLiteral("iptables -F"),
        QStringLiteral("ufw disable"),
        QStringLiteral("ifconfig eth0 down"),
        QStringLiteral("ip link set eth0 down"),
        QStringLiteral("ip addr add 10.0.0.5/24 dev eth0"),
        QStringLiteral("ip route add default via 10.0.0.1"),
        QStringLiteral("nmcli con modify eth0 ipv4.method auto"),
    };
    for (const QString &cmd : high) {
        const SafetyCheckResult r = checker().check(cmd);
        CHECK(r.riskLevel == RiskLevel::High);
        CHECK(r.isAllowed);
        CHECK(r.reason.startsWith(QStringLiteral("高风险操作")));
    }
}

// SAFE 白名单：只读命令自动放行（含 env 前缀剥离与多词前缀）
static void testSafeWhitelist()
{
    const QStringList safe = {
        QStringLiteral("ls -la /var/log"),
        QStringLiteral("cat /etc/os-release"),
        QStringLiteral("df -h"),
        QStringLiteral("ps aux"),
        QStringLiteral("uptime"),
        QStringLiteral("ip addr"),                      // 多词前缀精确等于
        QStringLiteral("ip route show"),                // 多词前缀 + 参数
        QStringLiteral("systemctl status nginx"),
        QStringLiteral("LC_ALL=C ls -la"),              // env 前缀剥离
        QStringLiteral("LC_ALL=C LANG=en_US.UTF-8 grep foo bar.txt"),
    };
    for (const QString &cmd : safe) {
        const SafetyCheckResult r = checker().check(cmd);
        CHECK(r.riskLevel == RiskLevel::Safe);
        CHECK(r.isAllowed);
    }

    // "lsblk" 不能被 "ls" 前缀误放行（前缀需整词匹配）
    const SafetyCheckResult lsblk = checker().check(QStringLiteral("lsblk"));
    CHECK(lsblk.riskLevel != RiskLevel::Safe);

    // 空命令按 Safe 处理
    const SafetyCheckResult empty = checker().check(QStringLiteral("  "));
    CHECK(empty.riskLevel == RiskLevel::Safe);
    CHECK(empty.isAllowed);
}

// MEDIUM：需用户确认
static void testMediumRules()
{
    const QStringList medium = {
        QStringLiteral("sudo systemctl start nginx"),   // sudo 提权
        QStringLiteral("echo hi > /tmp/out.txt"),       // 覆写重定向
        QStringLiteral("echo hi >> /tmp/out.txt"),      // 追加重定向
        QStringLiteral("echo conf | tee /etc/app.conf"),
        QStringLiteral("sed -i 's/a/b/' /etc/hosts"),
        QStringLiteral("systemctl restart nginx"),
        QStringLiteral("service nginx restart"),
        QStringLiteral("apt-get install -y curl"),
        QStringLiteral("pip install requests"),
        QStringLiteral("npm install express"),
    };
    for (const QString &cmd : medium) {
        const SafetyCheckResult r = checker().check(cmd);
        CHECK(r.riskLevel == RiskLevel::Medium);
        CHECK(r.isAllowed);
        CHECK(r.reason.startsWith(QStringLiteral("中等风险操作")));
    }
}

// LOW：未匹配任何规则的默认档
static void testLowDefault()
{
    const QStringList low = {
        QStringLiteral("cd /opt && make"),
        QStringLiteral("tar -czf backup.tgz data/"),
        QStringLiteral("git pull"),
    };
    for (const QString &cmd : low) {
        const SafetyCheckResult r = checker().check(cmd);
        CHECK(r.riskLevel == RiskLevel::Low);
        CHECK(r.isAllowed);
    }
}

// checkBatch：逐条结果与单条一致
static void testBatch()
{
    const QStringList cmds = {
        QStringLiteral("ls"),
        QStringLiteral("reboot"),
        QStringLiteral("rm -rf /"),
    };
    const QList<SafetyCheckResult> results = checker().checkBatch(cmds);
    CHECK(results.size() == 3);
    CHECK(results.at(0).riskLevel == RiskLevel::Safe);
    CHECK(results.at(1).riskLevel == RiskLevel::High);
    CHECK(results.at(2).riskLevel == RiskLevel::Critical);
}

// riskLevel 辅助函数（名称/标签/颜色，与 Python 侧映射一致）
static void testRiskLevelHelpers()
{
    CHECK(riskLevelName(RiskLevel::Safe) == QStringLiteral("safe"));
    CHECK(riskLevelName(RiskLevel::Critical) == QStringLiteral("critical"));
    CHECK(riskLevelLabel(RiskLevel::Medium) == QStringLiteral("中"));
    CHECK(riskLevelLabel(RiskLevel::Critical) == QStringLiteral("危险"));
    CHECK(riskLevelColor(RiskLevel::Safe) == QStringLiteral("#4CAF50"));
    CHECK(riskLevelColor(RiskLevel::High) == QStringLiteral("#F44336"));
}

// 文本 fallback 命令提取：```bash 代码块 + CJK/注释/列表项过滤
// 对应Python: SSHAIAgent._extract_commands_from_text
static void testExtractCommandsFromText()
{
    const QString text = QStringLiteral(
        "先检查磁盘占用：\n"
        "```bash\n"
        "# 查看磁盘\n"
        "df -h\n"
        "- 这是一个列表项\n"
        "1. 有序列表项\n"
        "-\n"
        "查看内存使用情况\n"
        "free -m\n"
        "```\n"
        "然后重启服务：\n"
        "```sh\n"
        "systemctl restart nginx\n"
        "```\n");
    const QList<AiCommand> cmds = SshAiAgent::extractCommandsFromText(text);
    CHECK(cmds.size() == 3);
    if (cmds.size() == 3) {
        CHECK(cmds.at(0).cmd == QStringLiteral("df -h"));
        CHECK(cmds.at(1).cmd == QStringLiteral("free -m"));
        CHECK(cmds.at(2).cmd == QStringLiteral("systemctl restart nginx"));
    }

    // 无代码块 → 无命令
    CHECK(SshAiAgent::extractCommandsFromText(
              QStringLiteral("这只是普通说明文本。")).isEmpty());
}

int main()
{
    testCriticalRules();
    testHighRules();
    testSafeWhitelist();
    testMediumRules();
    testLowDefault();
    testBatch();
    testRiskLevelHelpers();
    testExtractCommandsFromText();

    if (failures) {
        qWarning() << failures << "check(s) failed";
        return 1;
    }
    qInfo() << "safety_checker_test: all checks passed";
    return 0;
}
