#include "session_recorder.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace cubeshell {

namespace {
// 时间戳前缀格式，与审计场景习惯一致：[HH:MM:SS.zzz] 。
QString timestampPrefix()
{
    return QStringLiteral("[%1] ")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")));
}
} // namespace

SessionRecorder::~SessionRecorder()
{
    stop();
}

bool SessionRecorder::start(const QString &path, const Options &opt, QString *errOut)
{
    QMutexLocker locker(&m_mutex);
    if (m_active) {
        m_file.close();
        m_active = false;
    }
    if (path.isEmpty()) {
        if (errOut)
            *errOut = QStringLiteral("empty log path");
        return false;
    }

    // 目录不存在时先建出来，否则 QFile::open 会失败在一个用户看不懂的点上。
    const QFileInfo info(path);
    const QDir dir = info.absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (errOut)
            *errOut = QStringLiteral("cannot create log dir: %1").arg(dir.absolutePath());
        return false;
    }

    m_file.setFileName(path);
    // Append：同一目标多次连接的记录累积在一个文件里，不互相覆盖。
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (errOut)
            *errOut = m_file.errorString();
        return false;
    }
    m_opt = opt;
    m_atLineStart = true;
    m_active = true;
    return true;
}

void SessionRecorder::stop()
{
    QMutexLocker locker(&m_mutex);
    if (!m_active)
        return;
    m_file.flush();
    m_file.close();
    m_active = false;
}

bool SessionRecorder::isActive() const
{
    QMutexLocker locker(&m_mutex);
    return m_active;
}

QString SessionRecorder::filePath() const
{
    QMutexLocker locker(&m_mutex);
    return m_active ? m_file.fileName() : QString();
}

void SessionRecorder::writeRaw(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    QMutexLocker locker(&m_mutex);
    if (!m_active)
        return;

    if (!m_opt.addTimestamps) {
        m_file.write(data);
    } else {
        // 逐行插时间戳：行首（含首行、每个 \n 之后）先写 [HH:MM:SS.zzz]。
        // 按 \n 切分，\r 不当作行边界（终端原始流里 \r 是回车不是换行）。
        qsizetype start = 0;
        if (m_atLineStart) {
            m_file.write(timestampPrefix().toUtf8());
            m_atLineStart = false;
        }
        while (true) {
            const qsizetype nl = data.indexOf('\n', start);
            if (nl < 0) {
                m_file.write(data.constData() + start, data.size() - start);
                break;
            }
            // 写含 \n 的这一行，然后为下一行行首补时间戳。
            m_file.write(data.constData() + start, nl - start + 1);
            m_file.write(timestampPrefix().toUtf8());
            start = nl + 1;
        }
        // 结尾若恰好是 \n，则下一块从行首开始（但我们刚已写过时间戳，
        // 下一块不用再补——所以这里把 m_atLineStart 置 false，保持上一行
        // 已补过时间戳的状态）。简化为：是否处于行首取决于最后写的字节。
        m_atLineStart = false;
    }

    m_file.flush();   // 崩溃时也要留下已收到的数据
    rotateIfNeededLocked();
}

void SessionRecorder::rotateIfNeededLocked()
{
    if (!m_active || m_opt.maxBytes <= 0)
        return;
    if (m_file.pos() < m_opt.maxBytes)
        return;

    const QString base = m_file.fileName();
    m_file.flush();
    m_file.close();

    // 经典分卷：base.N 依次后移，最老的一卷丢弃，base 变为 base.1，重开新 base。
    const int keep = qMax(1, m_opt.backupCount);
    QFile::remove(QStringLiteral("%1.%2").arg(base).arg(keep));
    for (int i = keep - 1; i >= 1; --i) {
        QFile::rename(QStringLiteral("%1.%2").arg(base).arg(i),
                      QStringLiteral("%1.%2").arg(base).arg(i + 1));
    }
    QFile::rename(base, QStringLiteral("%1.1").arg(base));

    // 重开新卷（WriteOnly 截断）。失败则放弃录制——不阻断数据流。
    m_file.setFileName(base);
    if (!m_file.open(QIODevice::WriteOnly))
        m_active = false;
    m_atLineStart = true;
}

QString SessionRecorder::sanitizeTag(const QString &tag)
{
    QString out;
    out.reserve(tag.size());
    for (const QChar c : tag) {
        if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')
            || c == QLatin1Char('.'))
            out.append(c);
        else
            out.append(QLatin1Char('_'));
    }
    if (out.isEmpty())
        out = QStringLiteral("session");
    return out;
}

QString SessionRecorder::autoFileName(const QString &tag, const QString &dir)
{
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    return QDir(dir).filePath(
        QStringLiteral("%1-%2.log").arg(sanitizeTag(tag), stamp));
}

} // namespace cubeshell
