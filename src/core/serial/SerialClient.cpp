// SerialClient.cpp — 串口会话客户端。见 SerialClient.h。

#include "SerialClient.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSerialPortInfo>
#include <QTimer>

namespace cubeshell {

namespace {

// 热插拔轮询间隔。1s 足够跟上人拔插 USB 的动作，开销可忽略
//（QSerialPortInfo::availablePorts 只读一次系统设备表）。
constexpr int kPollIntervalMs = 1000;

// 校验位 → 单字母（"8N1" 里的 N）。
QChar parityLetter(QSerialPort::Parity parity)
{
    switch (parity) {
    case QSerialPort::NoParity:    return QLatin1Char('N');
    case QSerialPort::EvenParity:  return QLatin1Char('E');
    case QSerialPort::OddParity:   return QLatin1Char('O');
    case QSerialPort::MarkParity:  return QLatin1Char('M');
    case QSerialPort::SpaceParity: return QLatin1Char('S');
    default:                       return QLatin1Char('?');
    }
}

// 停止位 → 展示串（1.5 位是 Windows 专有，用 "1.5" 而非整数）。
QString stopBitsText(QSerialPort::StopBits stopBits)
{
    switch (stopBits) {
    case QSerialPort::OneStop:        return QStringLiteral("1");
    case QSerialPort::OneAndHalfStop: return QStringLiteral("1.5");
    case QSerialPort::TwoStop:        return QStringLiteral("2");
    default:                          return QStringLiteral("?");
    }
}

} // namespace

// --- SerialSettings ---

QString SerialSettings::frameFormat() const
{
    return QStringLiteral("%1%2%3")
        .arg(int(dataBits))
        .arg(parityLetter(parity))
        .arg(stopBitsText(stopBits));
}

// --- SerialPortDesc ---

QString SerialPortDesc::displayName() const
{
    if (description.isEmpty())
        return portName;
    return QStringLiteral("%1 — %2").arg(portName, description);
}

// --- SerialClient ---

SerialClient::SerialClient(QObject *parent)
    : QObject(parent)
    , m_port(new QSerialPort(this))
{
    connect(m_port, &QSerialPort::readyRead, this, &SerialClient::onReadyRead);
    connect(m_port, &QSerialPort::errorOccurred, this, &SerialClient::onSerialError);

    // 热插拔轮询常开：未连接时也要让 UI 的端口下拉框跟着插拔刷新。
    m_knownPorts = currentPortNames();
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &SerialClient::pollPorts);
    m_pollTimer->start();
}

SerialClient::~SerialClient()
{
    // 先停轮询，避免析构途中 pollPorts 再回调进来。
    if (m_pollTimer)
        m_pollTimer->stop();
    if (m_port && m_port->isOpen())
        m_port->close();
    closeLogFile();
}

bool SerialClient::isOpen() const
{
    return m_port && m_port->isOpen();
}

QList<SerialPortDesc> SerialClient::availablePorts()
{
    QList<SerialPortDesc> result;
    const auto infos = QSerialPortInfo::availablePorts();
    result.reserve(infos.size());
    for (const QSerialPortInfo &info : infos) {
        SerialPortDesc desc;
        desc.portName = info.portName();
        desc.description = info.description();
        desc.manufacturer = info.manufacturer();
        desc.hasVidPid = info.hasVendorIdentifier() && info.hasProductIdentifier();
        if (desc.hasVidPid) {
            desc.vendorId = info.vendorIdentifier();
            desc.productId = info.productIdentifier();
        }
        result.append(desc);
    }
    return result;
}

QStringList SerialClient::currentPortNames()
{
    QStringList names;
    const auto infos = QSerialPortInfo::availablePorts();
    names.reserve(infos.size());
    for (const QSerialPortInfo &info : infos)
        names.append(info.portName());
    names.sort();   // 排序后才能直接用 != 比对集合是否变化
    return names;
}

bool SerialClient::open(const SerialSettings &settings)
{
    if (isOpen())
        close();

    if (settings.portName.isEmpty()) {
        emit errorOccurred(tr("未指定串口设备。"));
        return false;
    }

    m_settings = settings;

    m_port->setPortName(settings.portName);
    m_port->setBaudRate(settings.baudRate);
    m_port->setDataBits(settings.dataBits);
    m_port->setParity(settings.parity);
    m_port->setStopBits(settings.stopBits);
    m_port->setFlowControl(settings.flowControl);

    if (!m_port->open(QIODevice::ReadWrite)) {
        // QSerialPort::errorString() 已本地化，直接透出比自造文案更准确
        //（区分"权限不足"/"设备被占用"/"设备不存在"）。
        emit errorOccurred(tr("打开串口 %1 失败：%2")
                               .arg(settings.portName, m_port->errorString()));
        return false;
    }

    // 打开后清空硬件缓冲里的残留字节，避免上一次会话的尾巴刷进新终端。
    m_port->clear();

    setState(State::Connected);
    emit connected();
    return true;
}

void SerialClient::close()
{
    if (!isOpen()) {
        // 已关闭：不重复发 disconnected()，否则 UI 的断开提示会出现两次。
        return;
    }
    m_port->close();
    setState(State::Disconnected);
    emit disconnected();
}

bool SerialClient::write(const QByteArray &data)
{
    if (!isOpen() || data.isEmpty())
        return false;
    const qint64 written = m_port->write(data);
    if (written < 0) {
        emit errorOccurred(tr("写串口失败：%1").arg(m_port->errorString()));
        return false;
    }
    return written == data.size();
}

void SerialClient::onReadyRead()
{
    const QByteArray data = m_port->readAll();
    if (data.isEmpty())
        return;

    // 日志先落盘再发信号：保证录制的是未经任何处理的原始字节流。
    if (m_logFile) {
        m_logFile->write(data);
        m_logFile->flush();   // 崩溃时也要留下已收到的数据
    }

    emit dataReceived(data);
}

void SerialClient::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;

    // ResourceError = 设备被拔掉/驱动卸载；PermissionError 在已打开的端口上
    // 出现同样意味着链路没了。两者都当作断开处理，而不只是报个错就放任
    // 一个已失效的句柄留着。
    const bool fatal = (error == QSerialPort::ResourceError
                        || error == QSerialPort::PermissionError
                        || error == QSerialPort::DeviceNotFoundError);

    if (fatal && isOpen()) {
        // 先取 errorString()：close() 会重置错误状态，取晚了就只剩空串。
        const QString reason = m_port->errorString();
        const QString name = m_settings.portName;
        m_port->close();
        setState(State::Disconnected);
        emit errorOccurred(tr("串口 %1 已断开：%2").arg(name, reason));
        emit disconnected();
        return;
    }

    // 非致命错误（如超时、帧错误）只报告，不动连接状态。
    // 未打开时的错误多为 open() 失败的二次通报，open() 已发过消息，此处跳过。
    if (isOpen())
        emit errorOccurred(m_port->errorString());
}

void SerialClient::pollPorts()
{
    const QStringList names = currentPortNames();
    if (names != m_knownPorts) {
        m_knownPorts = names;
        emit portsChanged();
    }

    // 当前连着的端口从系统里消失 → 设备被拔了。
    // 有些平台/驱动不会触发 QSerialPort::errorOccurred(ResourceError)，
    // 故这里做第二道检测，避免终端停在"已连接"的假状态上。
    if (isOpen() && !names.contains(m_settings.portName)) {
        const QString name = m_settings.portName;
        m_port->close();
        setState(State::Disconnected);
        emit errorOccurred(tr("串口 %1 已移除。").arg(name));
        emit disconnected();
    }
}

void SerialClient::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(m_state);
}

bool SerialClient::setLogFile(const QString &path)
{
    closeLogFile();

    if (path.isEmpty())
        return true;   // 空路径 = 停止录制，不算失败

    // 目标目录不存在时先建出来，否则 QFile::open 会失败在一个用户看不懂的点上。
    const QFileInfo info(path);
    const QDir dir = info.absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        emit errorOccurred(tr("无法创建日志目录：%1").arg(dir.absolutePath()));
        return false;
    }

    auto *file = new QFile(path);
    // Append：同一端口多次连接的记录累积在一个文件里，不互相覆盖。
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append)) {
        emit errorOccurred(tr("无法打开日志文件 %1：%2").arg(path, file->errorString()));
        delete file;
        return false;
    }
    m_logFile = file;
    return true;
}

QString SerialClient::logFilePath() const
{
    return m_logFile ? m_logFile->fileName() : QString();
}

void SerialClient::closeLogFile()
{
    if (!m_logFile)
        return;
    m_logFile->close();
    delete m_logFile;
    m_logFile = nullptr;
}

} // namespace cubeshell
