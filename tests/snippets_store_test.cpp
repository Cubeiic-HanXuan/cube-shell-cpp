// snippets_store_test.cpp — unit tests for SnippetsStore.

#include <QCoreApplication>
#include <QDebug>
#include <QTemporaryDir>

#include "config/snippets_store.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 占位符提取：去重、按出现顺序、非 \w 不识别。
static void testExtractParams()
{
    CHECK(SnippetsStore::extractParams(QStringLiteral("ls {path}")) == QStringList{QStringLiteral("path")});
    CHECK(SnippetsStore::extractParams(QStringLiteral("scp {f} {host}:/{f}"))
          == (QStringList{QStringLiteral("f"), QStringLiteral("host")}));   // {f} 去重
    CHECK(SnippetsStore::extractParams(QStringLiteral("echo hello")).isEmpty());
    CHECK(SnippetsStore::extractParams(QStringLiteral("{} {  } {a-b}")).isEmpty()); // 空/含非\w 不识别
}

// 参数展开：命中替换，缺值保留占位符原样。
static void testExpand()
{
    QHash<QString, QString> v;
    v.insert(QStringLiteral("path"), QStringLiteral("/var/log"));
    v.insert(QStringLiteral("n"), QStringLiteral("3"));
    CHECK(SnippetsStore::expand(QStringLiteral("tail -n {n} {path}/a.log"), v)
          == QStringLiteral("tail -n 3 /var/log/a.log"));
    // 缺 {host} 的值：占位符原样保留，不悄悄下发半截命令。
    CHECK(SnippetsStore::expand(QStringLiteral("ssh {host} {path}"), v)
          == QStringLiteral("ssh {host} /var/log"));
    CHECK(SnippetsStore::expand(QStringLiteral("plain"), v) == QStringLiteral("plain"));
}

// CRUD 往返 + id 稳定性。
static void testCrud()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    SnippetsStore store(dir.filePath(QStringLiteral("snippets.json")));

    CHECK(store.load().isEmpty());   // 缺失文件 = 空表

    Snippet s;
    s.name = QStringLiteral("看日志");
    s.group = QStringLiteral("运维");
    s.body = QStringLiteral("tail -f {path}");
    s.shortcut = QStringLiteral("Ctrl+Alt+L");
    QString err;
    CHECK(store.upsert(s, &err));

    QList<Snippet> all = store.load();
    CHECK(all.size() == 1);
    CHECK(!all[0].id.isEmpty());                 // upsert 自动补 id
    CHECK(all[0].name == QStringLiteral("看日志"));
    CHECK(all[0].appendNewline);                 // 默认补回车
    CHECK(store.groups() == QStringList{QStringLiteral("运维")});

    // 编辑：同 id 覆盖，不新增。
    Snippet edited = all[0];
    edited.body = QStringLiteral("tail -n 100 {path}");
    CHECK(store.upsert(edited, &err));
    all = store.load();
    CHECK(all.size() == 1);
    CHECK(all[0].id == edited.id);               // id 不变
    CHECK(all[0].body == QStringLiteral("tail -n 100 {path}"));

    // 删除幂等。
    CHECK(store.remove(edited.id, &err));
    CHECK(store.load().isEmpty());
    CHECK(store.remove(edited.id, &err));        // 再删一次也不报错
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testExtractParams();
    testExpand();
    testCrud();

    if (failures) {
        qWarning() << "snippets_store_test:" << failures << "failure(s)";
        return 1;
    }
    qDebug() << "snippets_store_test: all passed";
    return 0;
}
