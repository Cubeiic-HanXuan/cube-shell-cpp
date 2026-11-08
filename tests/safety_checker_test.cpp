// Quote-aware shell assessment, risk aggregation, read-only parameter policies,
// conservative fallbacks, and text-fallback command extraction.
// All command strings (including destructive fixtures) are DATA ONLY: no SSH or
// shell command is executed by this test.

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

    // lsblk has its own query rule; a similarly named executable is not trusted.
    CHECK(checker().check(QStringLiteral("lsblk")).riskLevel == RiskLevel::Safe);
    CHECK(checker().check(QStringLiteral("ls-custom")).riskLevel == RiskLevel::Medium);

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

// Unknown commands used to default to auto-executed Low. Absence from a
// blacklist is not evidence of read-only behavior.
static void testUnknownRequiresConfirmation()
{
    const QStringList unknown = {
        QStringLiteral("cd /opt && make"),
        QStringLiteral("tar -czf backup.tgz data/"),
        QStringLiteral("git pull"),
        QStringLiteral("/tmp/ls"),
        QStringLiteral("python3 -c 'print(1)'"),
    };
    for (const QString &cmd : unknown) {
        const SafetyCheckResult r = checker().check(cmd);
        CHECK(r.riskLevel == RiskLevel::Medium);
        CHECK(r.isAllowed);
        CHECK(!r.warnings.isEmpty());
    }
}

static void expectRisk(const QString &command, RiskLevel expected)
{
    const auto actual = checker().check(command);
    if (actual.riskLevel != expected) {
        qWarning() << "Risk mismatch:" << command << "expected" << riskLevelName(expected)
                   << "actual" << riskLevelName(actual.riskLevel) << actual.reason;
        ++failures;
    }
    CHECK(actual.isAllowed == (expected != RiskLevel::Critical));
    if (expected == RiskLevel::Safe) CHECK(actual.warnings.isEmpty());
}

static void testStructuredCommands()
{
    const QStringList queries = {
        "sudo ls -la /var/log", "sudo -n systemctl status nginx --no-pager",
        "sudo -u root journalctl -u nginx -n 50 --no-pager", "sudo -- lsblk -f",
        "env LC_ALL=C LANG='en_US.UTF-8' /usr/bin/ls -la", "LC_ALL='C' grep error app.log",
        "command -- ls", "command -v reboot", "timeout 5s df -h",
        "grep -E 'reboot|shutdown|mkfs|sudo' /var/log/syslog",
        "cat /var/log/reboot.log", "ls /tmp/mkfs /tmp/wipefs",
        "echo 'rm -rf /'", "printf '%s\\n' 'chmod -R 777 /'",
        "echo 'a > b; reboot && rm -rf /'", "echo \"literal > & | ;\"",
        "grep '$(reboot)' app.log", "echo \\$HOME",
        "echo \\; echo",
        "ls | grep log | head -n 5", "ps aux | grep nginx || true",
        "ls && df -h; free -m", "ls\ndf -h\n", "ls;", "ls # reboot\ndf -h",
        "ls \\\n -la", "ls\t-la", "grep '#' /etc/hosts", "echo 'line1\nreboot'",
        "cat /etc/os-release 2>/dev/null", "cat /etc/os-release 2>&1 | head",
        "cat < /etc/hosts", "2>/dev/null ls", "ls >/dev/null 2>&1",
        "ls &>/dev/null", "ls 1>&2", "ls >> /dev/null",
        "find /var/log -type f -name '*.log' -print", "find /tmp -maxdepth 1 -ls",
        "sed -n '1,80p' /etc/hosts", "sed -n '$p' /etc/hosts",
        "awk '{print $1}' /etc/hosts", "awk -F : '{print $1, $3}' /etc/passwd",
        "sort /etc/hosts", "uniq -c /tmp/input", "uniq -f 1 /tmp/input",
        "date", "date +%F", "date -d 'yesterday' +%F", "hostname -I", "file /etc/hosts",
        "systemctl --no-pager status nginx", "systemctl is-active nginx",
        "systemctl --type service list-units", "systemctl list-unit-files --state=enabled",
        "systemctl show nginx -p MainPID", "systemctl cat nginx", "service nginx status",
        "journalctl --since '30 min ago' --no-pager", "journalctl --disk-usage",
        "ip -br addr show", "ip -4 route show", "ip link show", "ip route get 1.1.1.1",
        "ss -lntp", "ss -lntp '( sport = :80 )'", "ifconfig", "ifconfig eth0",
        "sudo iptables -t filter -L -n -v --line-numbers", "iptables -nvL", "iptables -S",
        "ufw status verbose", "ufw show raw", "nmcli -t -f NAME connection show",
        "docker ps -a", "docker inspect nginx", "docker logs --tail 20 nginx",
        "docker container ls", "docker image inspect nginx", "docker compose ps",
        "kubectl -n default get pods -o wide", "kubectl describe pod nginx", "kubectl logs nginx",
        "git --no-pager -C /opt/app status --short", "git rev-parse HEAD",
    };
    for (const auto &cmd : queries) expectRisk(cmd, RiskLevel::Safe);

    const QStringList confirmation = {
        "ls > /tmp/output", "cat /etc/hosts >> /tmp/output", "ls &> /tmp/output",
        "cat /etc/hosts >| /tmp/output", "cat <> /tmp/output", "cat 2> /tmp/error",
        "cat /etc/hosts | tee /tmp/copy", "ls && touch /tmp/marker", "ls; rm /tmp/marker",
        "ls\nrm /tmp/marker", "ls | sh", "find /tmp -delete",
        "find /tmp -exec touch /tmp/marker \\;", "find /tmp -execdir ls {} +",
        "find /tmp -fprint /tmp/list", "find /tmp -fprintf /tmp/list '%p'",
        "sort -o /tmp/output /tmp/input", "sort --output=/tmp/output /tmp/input",
        "sort --compress-program=helper /tmp/input", "uniq /tmp/input /tmp/output",
        "sed -i.bak 's/a/b/' /tmp/file", "sed --in-place 's/a/b/' /tmp/file",
        "sed -n 'w /tmp/output' /tmp/file", "sed 's/a/b/e' /tmp/file",
        "awk '{system(\"id\")}' /tmp/file", "awk '{print > \"/tmp/output\"}' /tmp/file",
        "date -s '2027-01-01'", "date 010100002027", "hostname new-host",
        "journalctl --vacuum-time=1d", "journalctl --rotate", "file -C -m custom.magic",
        "systemctl start nginx", "sudo systemctl restart nginx", "systemctl enable --now nginx",
        "systemctl --root=/tmp/root status nginx", "service nginx restart",
        "docker restart nginx", "docker compose up -d", "kubectl delete pod nginx",
        "kubectl get --raw=/some/api", "git -c core.fsmonitor=helper status",
        "sudo -i ls", "sudo -E ls", "env -S 'ls -la'", "env LD_PRELOAD=/tmp/a.so ls",
        "PATH=/tmp ls", "PAGER=helper systemctl status nginx", "BASH_ENV=/tmp/profile bash -c ls",
        "echo $(whoami)", "echo `whoami`", "ls $HOME", "${COMMAND} /tmp/file",
        "echo ${HOME:-$(whoami)}", "ls *.log", "cat <(ls)", "ls &", "(ls)",
        "sh -c 'ls -la'", "bash -lc 'ls'", "ls &&", "ls |", "ls 'unterminated",
        "ls \\", "ls >", "cat <<EOF\nreboot\nEOF", "cat <<< 'reboot'",
    };
    for (const auto &cmd : confirmation) expectRisk(cmd, RiskLevel::Medium);

    const QStringList high = {
        "ls && reboot", "echo hi; shutdown -h now", "ls\nip addr flush dev eth0",
        "ip -4 route replace default via 10.0.0.1", "iptables -L -Z", "iptables -F",
        "iptables -L --modprobe=/tmp/helper", "ufw disable", "nmcli connection modify eth0 ipv4.method auto",
        "ss -K dst 10.0.0.1", "systemctl --no-pager reboot", "systemctl isolate rescue.target",
    };
    for (const auto &cmd : high) expectRisk(cmd, RiskLevel::High);

    const QStringList blocked = {
        "ls && rm -rf /", "cat /etc/hosts | /sbin/mkfs.ext4 /dev/sdb",
        "sudo /bin/rm --recursive --force -- /", "rm -r -f '/'", "r\\m -rf /",
        "rm -rf /tmp/..", "rm -rf /*", "chown -R root:root /",
        "dd of=/dev/nvme0n1 if=/dev/zero", "dd of=/dev/vda if=/tmp/input",
        "cat /tmp/input >/dev/nvme0n1", "cat /tmp/input > /dev/mmcblk0",
        "ls; wipefs -a /dev/sda", "sh -c 'rm -rf /'", "echo $(rm -rf /)",
        "echo \"$(echo x; rm -rf /)\"", "echo `rm -rf /`", "echo ${X:-$(rm -rf /)}",
    };
    for (const auto &cmd : blocked) expectRisk(cmd, RiskLevel::Critical);

    // Risk aggregation must not depend on command order or pipeline position.
    for (const auto &write : {"touch /tmp/file", "systemctl restart nginx", "find /tmp -delete"}) {
        for (const auto &join : {";", "&&", "||", "|", "\n"}) {
            expectRisk(QStringLiteral("ls %1 %2").arg(join, write), RiskLevel::Medium);
            expectRisk(QStringLiteral("%1 %2 ls").arg(write, join), RiskLevel::Medium);
        }
    }
    expectRisk("cd /var/log && ls", RiskLevel::Low);
    expectRisk("top -b -n 1", RiskLevel::Safe);
    expectRisk("dmesg --ctime | head", RiskLevel::Safe);
    expectRisk("dmesg --clear", RiskLevel::Medium);
    expectRisk("printf -v PATH /tmp", RiskLevel::Medium);
    expectRisk("printf -vPATH /tmp", RiskLevel::Medium);
    expectRisk("printf -- '%n' PATH", RiskLevel::Medium);
    expectRisk("sed -n 'p' --expression='w /tmp/output' /tmp/input", RiskLevel::Medium);
    expectRisk("awk '{print $1}' -f /tmp/extra.awk", RiskLevel::Medium);
    expectRisk("gawk '{print}' -l /tmp/extension.so", RiskLevel::Medium);
    expectRisk("'LC_ALL=C' ls", RiskLevel::Medium);
    expectRisk("LC_ALL\\=C ls", RiskLevel::Medium);
    expectRisk("LANG=C 'LC_ALL=C' ls", RiskLevel::Medium);
    expectRisk("echo 'LC_ALL=C'", RiskLevel::Safe);
    expectRisk("env 'LC_ALL=C' ls", RiskLevel::Safe);
    expectRisk("find /tmp -exec rm -rf / \\;", RiskLevel::Critical);
    expectRisk("rm -rf / <<EOF\ntext\nEOF", RiskLevel::Critical);
    expectRisk("echo $(echo $(echo 'mkfs reboot'))", RiskLevel::Medium);
    expectRisk("echo $(echo $(rm -rf /))", RiskLevel::Critical);
    expectRisk(QString("ls ") + QChar(0) + " -la", RiskLevel::Medium);
    expectRisk(QString("ls") + QChar(0x00a0) + "-la", RiskLevel::Medium);
    expectRisk("ls\r-la", RiskLevel::Medium);
    QString nested = "ls";
    for (int i = 0; i < 24; ++i) nested = "echo $(" + nested + ")";
    expectRisk(nested, RiskLevel::Medium);
    expectRisk(QString(65537, 'x'), RiskLevel::Medium);
    CHECK(checker().check("sudo ls").reason.contains("sudo"));
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
    testUnknownRequiresConfirmation();
    testStructuredCommands();
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
