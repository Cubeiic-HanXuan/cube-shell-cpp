// Serial port unit test: 换行转换纯函数、SerialSettings 默认值与帧格式串、
// 端口枚举可调用性、DeviceEntry 串口字段的 JSON 往返与旧配置兼容。
//
// 真实收发需要硬件，不进单测；用 socat(Unix) / com0com(Windows) 建虚拟串口对
// 手动验证（见 plan 的验证章节）。

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "config/DeviceConfigStore.h"
#include "serial/SerialBridge.h"
#include "serial/SerialClient.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 换行转换是 SerialBridge 的静态纯函数，直接测。
static void testNewlineConversion()
{
    using NL = SerialSettings::NewlineMode;

    // Cr 模式原样透传（终端回车键本来就产生 \r）。
    CHECK(SerialBridge::applyNewlineMode(QByteArray("a\rb"), NL::Cr)
          == QByteArray("a\rb"));

    // Lf：\r → \n
    CHECK(SerialBridge::applyNewlineMode(QByteArray("a\rb"), NL::Lf)
          == QByteArray("a\nb"));

    // CrLf：\r → \r\n
    CHECK(SerialBridge::applyNewlineMode(QByteArray("a\rb"), NL::CrLf)
          == QByteArray("a\r\nb"));

    // 已经是 \r\n 的输入（粘贴文本）不重复加 LF。
    CHECK(SerialBridge::applyNewlineMode(QByteArray("a\r\nb"), NL::CrLf)
          == QByteArray("a\r\nb"));
    CHECK(SerialBridge::applyNewlineMode(QByteArray("a\r\nb"), NL::Lf)
          == QByteArray("a\nb"));

    // 非 \r 字节不受影响，含 \n 单独出现时原样保留。
    CHECK(SerialBridge::applyNewlineMode(QByteArray("a\nb"), NL::CrLf)
          == QByteArray("a\nb"));

    // 多个 \r 连续。
    CHECK(SerialBridge::applyNewlineMode(QByteArray("\r\r"), NL::CrLf)
          == QByteArray("\r\n\r\n"));
    CHECK(SerialBridge::applyNewlineMode(QByteArray("\r\r"), NL::Lf)
          == QByteArray("\n\n"));

    // 空输入。
    CHECK(SerialBridge::applyNewlineMode(QByteArray(), NL::CrLf).isEmpty());

    // 二进制安全：含 \0 的数据不被截断。
    const QByteArray bin("a\0\rb", 4);
    CHECK(SerialBridge::applyNewlineMode(bin, NL::Lf) == QByteArray("a\0\nb", 4));
}

static void testSettingsDefaults()
{
    SerialSettings s;
    CHECK(s.portName.isEmpty());
    CHECK(s.baudRate == 115200);
    CHECK(s.dataBits == QSerialPort::Data8);
    CHECK(s.parity == QSerialPort::NoParity);
    CHECK(s.stopBits == QSerialPort::OneStop);
    CHECK(s.flowControl == QSerialPort::NoFlowControl);
    CHECK(s.newlineMode == SerialSettings::NewlineMode::Cr);
    CHECK(s.localEcho == false);

    // 帧格式紧凑串（状态栏展示用）。
    CHECK(s.frameFormat() == QStringLiteral("8N1"));

    s.dataBits = QSerialPort::Data7;
    s.parity = QSerialPort::EvenParity;
    s.stopBits = QSerialPort::TwoStop;
    CHECK(s.frameFormat() == QStringLiteral("7E2"));

    s.stopBits = QSerialPort::OneAndHalfStop;
    s.parity = QSerialPort::OddParity;
    CHECK(s.frameFormat() == QStringLiteral("7O1.5"));
}

// 端口枚举在无硬件/CI 环境下必须能安全调用并返回空表，不能崩。
static void testAvailablePorts()
{
    const QList<SerialPortDesc> ports = SerialClient::availablePorts();
    qInfo() << "available serial ports:" << ports.size();
    for (const SerialPortDesc &p : ports) {
        // 枚举出来的端口必须有名字，displayName 不能为空。
        CHECK(!p.portName.isEmpty());
        CHECK(!p.displayName().isEmpty());
        qInfo() << "  -" << p.displayName();
    }

    // 无描述时 displayName 退化为纯端口名。
    SerialPortDesc bare;
    bare.portName = QStringLiteral("COM9");
    CHECK(bare.displayName() == QStringLiteral("COM9"));
    bare.description = QStringLiteral("USB-SERIAL CH340");
    CHECK(bare.displayName() == QStringLiteral("COM9 — USB-SERIAL CH340"));
}

// SerialClient 的构造/析构与未连接状态下的行为（无硬件也能测）。
static void testClientIdleBehaviour()
{
    SerialClient client;
    CHECK(client.state() == SerialClient::State::Disconnected);
    CHECK(!client.isOpen());
    CHECK(!client.isLogging());

    // 未打开时写入应失败而不是崩。
    CHECK(!client.write(QByteArray("hello")));

    // 空端口名 open() 必须失败。
    SerialSettings s;
    CHECK(!client.open(s));
    CHECK(!client.isOpen());

    // 不存在的端口 open() 失败（不同平台错误码不同，只断言失败）。
    s.portName = QStringLiteral("__cubeshell_no_such_port__");
    CHECK(!client.open(s));
    CHECK(!client.isOpen());

    // close() 在未打开时是空操作，不应崩。
    client.close();
    CHECK(!client.isOpen());
}

static void testLogFile()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());

    SerialClient client;
    // 空路径 = 停止录制，视为成功。
    CHECK(client.setLogFile(QString()));
    CHECK(!client.isLogging());

    const QString path = dir.filePath(QStringLiteral("sub/serial.log"));
    // 目录不存在时应自动创建。
    CHECK(client.setLogFile(path));
    CHECK(client.isLogging());
    CHECK(client.logFilePath() == path);
    CHECK(QFile::exists(path));

    // 关闭录制。
    CHECK(client.setLogFile(QString()));
    CHECK(!client.isLogging());
    CHECK(client.logFilePath().isEmpty());
}

// DeviceEntry 的串口字段 JSON 往返 + 旧配置（无串口键）的默认回落。
static void testDeviceEntryRoundTrip()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jsonPath = dir.filePath(QStringLiteral("devices.json"));

    DeviceEntry serialDev;
    serialDev.name = QStringLiteral("单片机");
    serialDev.protocol = QStringLiteral("serial");
    serialDev.portName = QStringLiteral("/dev/ttyUSB0");
    serialDev.baudRate = 9600;
    serialDev.dataBits = 7;
    serialDev.parity = QStringLiteral("even");
    serialDev.stopBits = QStringLiteral("2");
    serialDev.flowControl = QStringLiteral("hardware");
    serialDev.newlineMode = QStringLiteral("crlf");
    serialDev.localEcho = true;

    CHECK(serialDev.isSerial());
    CHECK(!serialDev.isRdp());

    // SSH 设备不应被误判为串口。
    DeviceEntry sshDev;
    sshDev.name = QStringLiteral("web");
    sshDev.host = QStringLiteral("10.0.0.1:22");
    sshDev.username = QStringLiteral("root");
    CHECK(!sshDev.isSerial());
    CHECK(!sshDev.isRdp());

    DeviceConfigStore store;
    store.addDevice(serialDev);
    store.addDevice(sshDev);
    QString err;
    CHECK(store.saveJson(jsonPath, &err));

    DeviceConfigStore reloaded;
    CHECK(reloaded.loadJson(jsonPath, &err));
    const DeviceEntry *got = reloaded.find(QStringLiteral("单片机"));
    CHECK(got != nullptr);
    if (got) {
        CHECK(got->isSerial());
        CHECK(got->portName == QStringLiteral("/dev/ttyUSB0"));
        CHECK(got->baudRate == 9600);
        CHECK(got->dataBits == 7);
        CHECK(got->parity == QStringLiteral("even"));
        CHECK(got->stopBits == QStringLiteral("2"));
        CHECK(got->flowControl == QStringLiteral("hardware"));
        CHECK(got->newlineMode == QStringLiteral("crlf"));
        CHECK(got->localEcho == true);
    }
    // SSH 条目不受影响。
    const DeviceEntry *gotSsh = reloaded.find(QStringLiteral("web"));
    CHECK(gotSsh != nullptr);
    if (gotSsh) {
        CHECK(!gotSsh->isSerial());
        CHECK(gotSsh->protocol == QStringLiteral("ssh"));
        CHECK(gotSsh->username == QStringLiteral("root"));
    }
}

// 旧版 JSON（没有任何串口键）读入后必须落到结构体默认值，不能是 0/空串。
static void testLegacyJsonFallback()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString jsonPath = dir.filePath(QStringLiteral("legacy.json"));

    // 手写一份 RDP 时代的 JSON：只有 name/host/protocol 等旧键。
    QFile f(jsonPath);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(R"([{"name":"旧设备","username":"root","password":"p",)"
            R"("host":"192.168.1.5:22","port":22,"keyType":"","keyFile":"",)"
            R"("protocol":"ssh","domain":"","auth":"ntlm"}])");
    f.close();

    DeviceConfigStore store;
    QString err;
    CHECK(store.loadJson(jsonPath, &err));
    const DeviceEntry *e = store.find(QStringLiteral("旧设备"));
    CHECK(e != nullptr);
    if (e) {
        CHECK(e->protocol == QStringLiteral("ssh"));
        CHECK(!e->isSerial());
        // 串口键缺失 → 默认值（而非 0 / 空串）。
        CHECK(e->portName.isEmpty());
        CHECK(e->baudRate == 115200);
        CHECK(e->dataBits == 8);
        CHECK(e->parity == QStringLiteral("none"));
        CHECK(e->stopBits == QStringLiteral("1"));
        CHECK(e->flowControl == QStringLiteral("none"));
        CHECK(e->newlineMode == QStringLiteral("cr"));
        CHECK(e->localEcho == false);
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testNewlineConversion();
    testSettingsDefaults();
    testAvailablePorts();
    testClientIdleBehaviour();
    testLogFile();
    testDeviceEntryRoundTrip();
    testLegacyJsonFallback();

    if (failures == 0)
        qInfo() << "serial_test: all checks passed";
    else
        qWarning() << "serial_test:" << failures << "check(s) failed";
    return failures == 0 ? 0 : 1;
}
