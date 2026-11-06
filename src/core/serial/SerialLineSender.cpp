#include "SerialLineSender.h"

#include "SerialBridge.h"
#include "SerialClient.h"

namespace cubeshell {

QStringList SerialLineSender::splitLines(QString text)
{
    if (text.isEmpty())
        return {};
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    // 最后一个换行终止前一条命令；连续换行产生的真实空行仍然保留。
    if (text.endsWith(QLatin1Char('\n')))
        lines.removeLast();
    return lines;
}

SerialLineSender::SerialLineSender(SerialClient *client, SerialBridge *bridge, QObject *parent)
    : QObject(parent), m_client(client), m_bridge(bridge)
{
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &SerialLineSender::advance);
    connect(client, &SerialClient::bytesWritten, this, &SerialLineSender::onBytesWritten);
    connect(client, &SerialClient::connected, this, &SerialLineSender::changed);
    connect(client, &SerialClient::disconnected, this, [this]() {
        abort(tr("串口已断开；重连后不会自动续发，请检查设备状态。"));
    });
    connect(client, &SerialClient::errorOccurred, this, [this](const QString &message) {
        abort(tr("发送已停止：%1").arg(message));
    });
    connect(client, &QObject::destroyed, this, [this]() {
        abort(tr("串口会话已关闭。"));
    });
    connect(bridge, &QObject::destroyed, this, [this]() {
        abort(tr("串口终端已关闭。"));
    });
    connect(bridge, &SerialBridge::terminalInputReceived, this, [this]() {
        if (m_state == State::Automatic) {
            pause();
            m_message = tr("检测到普通终端输入，自动发送已暂停。请检查设备状态后继续。");
            emit changed();
        }
    });
}

SerialLineSender::~SerialLineSender()
{
    m_timer.stop();
}

bool SerialLineSender::isConnected() const
{
    return m_client && m_bridge && m_client->isOpen();
}

bool SerialLineSender::isBusy() const
{
    return m_state != State::Idle || m_inFlight >= 0 || m_submitting;
}

bool SerialLineSender::canSendManually() const
{
    return !isBusy() && isConnected() && m_client->bytesToWrite() == 0;
}

int SerialLineSender::nextPendingLine() const
{
    for (int i = 0; i < m_lines.size(); ++i) {
        if (m_lines[i].state == LineState::Pending)
            return i;
    }
    return -1;
}

int SerialLineSender::sentCount() const
{
    int count = 0;
    for (const Line &line : m_lines) {
        if (line.state == LineState::Sent)
            ++count;
    }
    return count;
}

bool SerialLineSender::reject(const QString &message)
{
    m_message = message;
    emit changed();
    return false;
}

bool SerialLineSender::setText(const QString &text)
{
    if (isBusy())
        return reject(tr("请先停止发送，等待已提交数据写出后再编辑。"));
    if (text.size() > kMaxTextCharacters)
        return reject(tr("文本过大，最多支持 %1 个字符。请分批载入。").arg(kMaxTextCharacters));
    const QStringList parts = splitLines(text);
    if (parts.size() > kMaxLines)
        return reject(tr("最多支持 %1 行命令，请分批载入。").arg(kMaxLines));
    m_lines.clear();
    m_lines.reserve(parts.size());
    for (const QString &part : parts)
        m_lines.append({part, LineState::Pending});
    m_message.clear();
    emit changed();
    return true;
}

bool SerialLineSender::setLineText(int index, const QString &text)
{
    if (isBusy() || index < 0 || index >= m_lines.size()
        || m_lines[index].state != LineState::Pending)
        return false;
    if (text.contains(QLatin1Char('\r')) || text.contains(QLatin1Char('\n')))
        return reject(tr("每个单元格只能包含一行命令；多行内容请使用“编辑文本”。"));
    qsizetype size = text.size();
    for (int i = 0; i < m_lines.size(); ++i) {
        if (i != index)
            size += m_lines[i].text.size();
    }
    if (size > kMaxTextCharacters)
        return reject(tr("文本过大，请分批发送。"));
    m_lines[index].text = text;
    m_message.clear();
    emit changed();
    return true;
}

bool SerialLineSender::sendLine(int index)
{
    if (index < 0 || index >= m_lines.size() || m_lines[index].state != LineState::Pending)
        return false;
    if (!canSendManually())
        return reject(tr("串口未连接或发送尚未结束，请稍后重试。"));
    m_message.clear();
    return submitLine(index);
}

bool SerialLineSender::sendNext()
{
    return sendLine(nextPendingLine());
}

bool SerialLineSender::startAutomatic(int intervalMs)
{
    if (isBusy() || !isConnected() || nextPendingLine() < 0)
        return false;
    m_intervalMs = qBound(10, intervalMs, 60000);
    m_state = State::Automatic;
    m_message.clear();
    m_timer.start(0);
    emit changed();
    return true;
}

void SerialLineSender::pause()
{
    if (m_state != State::Automatic)
        return;
    m_timer.stop();
    m_waitingForDrain = false;
    m_state = State::Paused;
    m_message = tr("已暂停，已提交的数据仍会继续写出。");
    emit changed();
}

void SerialLineSender::resume()
{
    if (m_state != State::Paused || !isConnected())
        return;
    m_state = State::Automatic;
    m_message.clear();
    if (m_inFlight < 0)
        m_timer.start(m_intervalMs);
    emit changed();
}

void SerialLineSender::stop()
{
    const bool wasBusy = isBusy();
    m_timer.stop();
    m_waitingForDrain = false;
    m_state = State::Idle;
    // 不清串口驱动缓冲。已经提交的一行仍跟踪到排空，但绝不推进下一行。
    if (wasBusy)
        m_message = tr("已停止，未发送的命令已保留；已提交的数据无法撤回。");
    emit changed();
}

void SerialLineSender::abort(const QString &message)
{
    m_timer.stop();
    m_waitingForDrain = false;
    m_state = State::Idle;
    if (m_inFlight >= 0) {
        m_lines[m_inFlight].state = LineState::Uncertain;
        m_inFlight = -1;
    }
    m_message = message;
    emit changed();
}

void SerialLineSender::advance()
{
    if (m_state != State::Automatic || m_inFlight >= 0)
        return;
    if (!isConnected()) {
        abort(tr("串口未连接，自动发送已停止。"));
        return;
    }
    if (m_client->bytesToWrite() > 0) {
        m_waitingForDrain = true;
        return;
    }
    m_waitingForDrain = false;
    const int index = nextPendingLine();
    if (index < 0) {
        m_state = State::Idle;
        emit changed();
        return;
    }
    submitLine(index);
}

bool SerialLineSender::submitLine(int index)
{
    m_inFlight = index;
    m_lines[index].state = LineState::Sending;
    // write() 可同步发 errorOccurred。先标记当前行，失败回调才能把它置为不确定；
    // 提交期间禁止编辑队列，也不让同步 bytesWritten 回调提前推进下一行。
    m_submitting = true;
    const bool accepted = m_bridge->sendText(m_lines[index].text + QLatin1Char('\r'));
    m_submitting = false;
    if (m_inFlight != index) {
        emit changed();
        return false;   // 同步错误/断开已处理，不能再将该行标成成功。
    }
    if (!accepted) {
        abort(tr("写入未完整确认，发送已停止。请检查设备，勿直接重复发送当前行。"));
        return false;
    }
    emit changed();
    onBytesWritten();   // 兼容 write 返回时 Qt 缓冲已经排空的情况。
    return true;
}

void SerialLineSender::onBytesWritten()
{
    if (m_submitting || !isConnected() || m_client->bytesToWrite() > 0)
        return;
    if (m_inFlight >= 0) {
        m_lines[m_inFlight].state = LineState::Sent;
        m_inFlight = -1;
        if (nextPendingLine() < 0) {
            m_state = State::Idle;
            m_message = tr("没有待发送的行。已发送仅表示本机写出，不代表设备执行成功。");
        } else if (m_state == State::Automatic) {
            m_timer.start(m_intervalMs);
        }
    } else if (m_waitingForDrain && m_state == State::Automatic) {
        m_waitingForDrain = false;
        m_timer.start(m_intervalMs);
    }
    emit changed();
}

} // namespace cubeshell
