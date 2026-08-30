// session_recorder_test.cpp — unit tests for SessionRecorder.

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "terminal/session_recorder.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static QString readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// 原始字节直写（无时间戳、不轮转）：写啥落啥，Append 累积。
static void testRawPassthrough()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.log"));

    SessionRecorder rec;
    SessionRecorder::Options opt;
    QString err;
    CHECK(rec.start(path, opt, &err));
    CHECK(rec.isActive());
    rec.writeRaw("hello\r\n");
    rec.writeRaw("world\n");
    rec.stop();
    CHECK(!rec.isActive());
    CHECK(readAll(path) == QStringLiteral("hello\r\nworld\n"));

    // Append：再次 start 同一路径，内容累积不覆盖。
    CHECK(rec.start(path, opt, &err));
    rec.writeRaw("again\n");
    rec.stop();
    CHECK(readAll(path) == QStringLiteral("hello\r\nworld\nagain\n"));
}

// 行首时间戳：首行与每个 \n 之后都补 [HH:MM:SS.zzz] 。
static void testTimestamps()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ts.log"));

    SessionRecorder rec;
    SessionRecorder::Options opt;
    opt.addTimestamps = true;
    QString err;
    CHECK(rec.start(path, opt, &err));
    // 跨块边界：第一块以 \n 结尾，第二块不该重复补时间戳。
    rec.writeRaw("line1\n");
    rec.writeRaw("line2\npartial");
    rec.stop();

    const QString content = readAll(path);
    const QStringList lines = content.split(QLatin1Char('\n'));
    // line1 / line2 / partial 三行都该有时间戳前缀。
    CHECK(lines.size() >= 3);
    CHECK(lines[0].startsWith(QLatin1Char('[')) && lines[0].endsWith(QLatin1String("] line1")));
    CHECK(lines[1].startsWith(QLatin1Char('[')) && lines[1].endsWith(QLatin1String("] line2")));
    CHECK(lines[2].startsWith(QLatin1Char('[')) && lines[2].endsWith(QLatin1String("] partial")));
}

// 按大小轮转：超限后 base 变 base.1，新 base 重开，保留卷数受控。
static void testRotation()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("rot.log"));

    SessionRecorder rec;
    SessionRecorder::Options opt;
    opt.maxBytes = 16;      // 16 字节就轮转
    opt.backupCount = 2;
    QString err;
    CHECK(rec.start(path, opt, &err));

    // 写 5 批、每批 10 字节 → 触发多次轮转。
    for (int i = 0; i < 5; ++i)
        rec.writeRaw(QByteArray(10, 'a' + i));
    rec.stop();

    // 当前卷 + 至多 backupCount 个历史卷。
    CHECK(QFile::exists(path));
    CHECK(QFile::exists(path + QStringLiteral(".1")));
    CHECK(QFile::exists(path + QStringLiteral(".2")));
    CHECK(!QFile::exists(path + QStringLiteral(".3")));   // 超出 backupCount 的最老卷已删
}

// sanitizeTag：路径分隔、IPv6 冒号、空白都替换成 '_'。
static void testSanitize()
{
    CHECK(SessionRecorder::sanitizeTag(QStringLiteral("web-01")) == QStringLiteral("web-01"));
    CHECK(SessionRecorder::sanitizeTag(QStringLiteral("2001:db8::1")) == QStringLiteral("2001_db8__1"));
    CHECK(SessionRecorder::sanitizeTag(QStringLiteral("a/b c")) == QStringLiteral("a_b_c"));
    CHECK(SessionRecorder::sanitizeTag(QString()) == QStringLiteral("session"));
}

// 自动命名：落在指定目录、含 tag 与 .log 后缀。
static void testAutoFileName()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString p = SessionRecorder::autoFileName(QStringLiteral("web-01"), dir.path());
    CHECK(p.startsWith(dir.path()));
    CHECK(p.contains(QStringLiteral("web-01")));
    CHECK(p.endsWith(QStringLiteral(".log")));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testRawPassthrough();
    testTimestamps();
    testRotation();
    testSanitize();
    testAutoFileName();

    if (failures) {
        qWarning() << "session_recorder_test:" << failures << "failure(s)";
        return 1;
    }
    qDebug() << "session_recorder_test: all passed";
    return 0;
}
