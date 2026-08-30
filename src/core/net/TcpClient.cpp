// TcpClient.cpp — TCP / Telnet 会话客户端。见 TcpClient.h。

#include "net/TcpClient.h"

#include <QDebug>
#include <QTcpSocket>
#include <QTimer>

#include "config/GlobalState.h"

namespace cubeshell {

// --- TcpSettings ---

QString TcpSettings::displayTarget() const
{
    // IPv6 字面量加方括号，与 formatHostPort（config/DeviceConfigStore.cpp）一致。
    const QString h = host.contains(QLatin1Char(':'))
                          ? QStringLiteral("[%1]").arg(host)
                          : host;
    return QStringLiteral("%1 %2:%3").arg(mode, h).arg(port);
}

// --- TcpClient ---

TcpClient::TcpClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
{
    connect(m_socket, &QTcpSocket::connected, this, &TcpClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &TcpClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &TcpClient::onReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred, this, &TcpClient::onSocketError);

    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, &TcpClient::onConnectTimeout);
}

TcpClient::~TcpClient()
{
    // 先停定时器，避免析构途中回调进来。
    if (m_connectTimer)
        m_connectTimer->stop();
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        // abort() 而非 disconnectFromHost()：后者会等 FIN 往返，析构路径上
        // 不该阻塞，也不该在对象半销毁时还有信号回调。
        m_socket->blockSignals(true);
        m_socket->abort();
    }
    closeLogFile();
}

bool TcpClient::isOpen() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

bool TcpClient::connectToHost(const TcpSettings &settings)
{
    if (m_state != State::Disconnected)
        disconnectFromHost();

    const QString host = settings.host.trimmed();
    if (host.isEmpty()) {
        emit errorOccurred(tr("未指定主机地址。"));
        return false;
    }
    if (settings.port == 0) {
        emit errorOccurred(tr("端口号无效。"));
        return false;
    }

    m_settings = settings;
    m_settings.host = host;
    m_errorReported = false;

    setState(State::Connecting);
    // 主机名交给 QTcpSocket 自己解析（内部走 QHostInfo 的异步查询，不阻塞）。
    m_socket->connectToHost(host, settings.port);

    if (settings.connectTimeoutMs > 0)
        m_connectTimer->start(settings.connectTimeoutMs);
    return true;
}

void TcpClient::disconnectFromHost()
{
    m_connectTimer->stop();
    if (m_state == State::Disconnected) {
        // 已断开：不重复发 disconnected()，否则 UI 的断开提示会出现两次。
        return;
    }

    // 连接中途取消时 QTcpSocket 不会发 disconnected()（从未 connected 过），
    // 故这里统一用 abort() + 手动补发，让上层只需要认一条断开路径。
    //
    // 先置状态再 abort()：已连接时 abort() 会同步触发 disconnected()，
    // onDisconnected() 见到状态已是 Disconnected 就直接返回，不会重复外发。
    setState(State::Disconnected);
    m_socket->abort();
    emit disconnected();
}

bool TcpClient::write(const QByteArray &data)
{
    if (!isOpen() || data.isEmpty())
        return false;
    const qint64 written = m_socket->write(data);
    if (written < 0) {
        emit errorOccurred(tr("写入失败：%1").arg(m_socket->errorString()));
        return false;
    }
    return written == data.size();
}

void TcpClient::onConnected()
{
    m_connectTimer->stop();
    setState(State::Connected);
    emit connected();
}

void TcpClient::onDisconnected()
{
    m_connectTimer->stop();
    if (m_state == State::Disconnected)
        return;   // disconnectFromHost() 已经处理过
    setState(State::Disconnected);
    emit disconnected();
}

void TcpClient::onReadyRead()
{
    const QByteArray data = m_socket->readAll();
    if (data.isEmpty())
        return;

    // 日志先落盘再发信号：保证录制的是未经任何处理的原始字节流
    //（含 IAC 协商序列，排查协商问题时这份原始记录才有价值）。
    if (m_recorder.isActive())
        m_recorder.writeRaw(data);

    emit dataReceived(data);
}

void TcpClient::onSocketError(QAbstractSocket::SocketError error)
{
    // RemoteHostClosedError 是对端正常关闭连接，紧跟着就会有 disconnected()，
    // 不该当成错误弹到状态栏——telnet 会话里 `exit` 走的就是这条路。
    if (error == QAbstractSocket::RemoteHostClosedError)
        return;

    if (m_errorReported)
        return;
    m_errorReported = true;

    m_connectTimer->stop();
    // errorString() 已本地化，直接透出比自造文案更准确（区分"连接被拒绝"/
    // "主机不可达"/"域名解析失败"）。
    const QString reason = m_socket->errorString();
    const bool wasActive = (m_state != State::Disconnected);

    // 置状态先于 abort()：已连接时 abort() 会同步触发 disconnected()，
    // 届时 onDisconnected() 见状态已是 Disconnected 便直接返回，不重复外发。
    setState(State::Disconnected);
    m_socket->abort();
    emit errorOccurred(tr("连接 %1:%2 失败：%3")
                           .arg(m_settings.host)
                           .arg(m_settings.port)
                           .arg(reason));
    if (wasActive)
        emit disconnected();
}

void TcpClient::onConnectTimeout()
{
    if (m_state != State::Connecting)
        return;
    m_errorReported = true;   // 随后 abort() 触发的 socket error 不再重复报

    setState(State::Disconnected);
    m_socket->abort();
    emit errorOccurred(tr("连接 %1:%2 超时（%3 秒）。")
                           .arg(m_settings.host)
                           .arg(m_settings.port)
                           .arg(m_settings.connectTimeoutMs / 1000));
    emit disconnected();
}

void TcpClient::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(m_state);
}

bool TcpClient::setLogFile(const QString &path)
{
    if (path.isEmpty()) {
        closeLogFile();
        return true;   // 空路径 = 停止录制，不算失败
    }

    // 时间戳/轮转选项取自全局设置（C++ 独有，单一真相源在 theme.json）。
    GlobalState &gs = GlobalState::instance();
    SessionRecorder::Options opt;
    opt.addTimestamps = gs.sessionLogTimestamps();
    opt.maxBytes = qint64(gs.sessionLogMaxMB()) * 1024 * 1024;
    opt.backupCount = gs.sessionLogBackupCount();

    QString err;
    if (!m_recorder.start(path, opt, &err)) {
        emit errorOccurred(tr("无法打开日志文件 %1：%2").arg(path, err));
        return false;
    }
    return true;
}

QString TcpClient::logFilePath() const
{
    return m_recorder.filePath();
}

void TcpClient::closeLogFile()
{
    m_recorder.stop();
}

} // namespace cubeshell
