// Serial port unit test: 换行转换纯函数、SerialSettings 默认值与帧格式串、
// 端口枚举可调用性、下拉框取值（含手输）、DeviceEntry 串口字段的 JSON 往返
// 与旧配置兼容。
//
// 真实收发需要硬件，不进单测；用 socat(Unix) / com0com(Windows) 建虚拟串口对
// 手动验证（见 plan 的验证章节）。

#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <util.h>       // openpty
#else
#include <pty.h>        // openpty
#endif
#endif

#include "config/DeviceConfigStore.h"
#include "dialogs/SerialConnectDialog.h"   // serialcombo::portNameOf / baudRateOf
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

// 接收方向：孤立的 LF 补 CR（对应 PuTTY 的 Implicit CR in every LF）。
static void testRxImplicitCr()
{
    bool prevWasCr = false;

    // 孤立 LF：补 CR，让光标回到行首。
    CHECK(SerialBridge::applyRxImplicitCr(QByteArray("abc\ndef"), prevWasCr)
          == QByteArray("abc\r\ndef"));
    CHECK(!prevWasCr);   // 块尾是 'f'

    // 已经是 \r\n 的不动。
    prevWasCr = false;
    CHECK(SerialBridge::applyRxImplicitCr(QByteArray("abc\r\ndef"), prevWasCr)
          == QByteArray("abc\r\ndef"));
    CHECK(!prevWasCr);

    // 块尾是 \r：prevWasCr 应被设为 true，以便下一块若以 \n 开头能识别出是同一个 CRLF。
    prevWasCr = false;
    CHECK(SerialBridge::applyRxImplicitCr(QByteArray("line\r"), prevWasCr)
          == QByteArray("line\r"));
    CHECK(prevWasCr);

    // 跨块边界：上一块以 \r 结尾，这一块以 \n 开头 —— 那是同一个 \r\n 被拆开了，不能补 CR。
    prevWasCr = true;
    CHECK(SerialBridge::applyRxImplicitCr(QByteArray("\nmore"), prevWasCr)
          == QByteArray("\nmore"));
    CHECK(!prevWasCr);   // 块尾是 'e'

    // 跨块边界但下一块开头不是 \n，上一块的 \r 是孤立的 —— 不过这个函数只处理 LF，不管历史的 CR。
    prevWasCr = true;
    CHECK(SerialBridge::applyRxImplicitCr(QByteArray("abc\ndef"), prevWasCr)
          == QByteArray("abc\r\ndef"));
    CHECK(!prevWasCr);

    // 多个孤立 LF。
    prevWasCr = false;
    CHECK(SerialBridge::applyRxImplicitCr(QByteArray("a\nb\nc"), prevWasCr)
          == QByteArray("a\r\nb\r\nc"));
    CHECK(!prevWasCr);

    // 连续 \r\n\r\n。
    prevWasCr = false;
    CHECK(SerialBridge::applyRxImplicitCr(QByteArray("\r\n\r\n"), prevWasCr)
          == QByteArray("\r\n\r\n"));
    CHECK(!prevWasCr);

    // 空输入不该改动 prevWasCr。
    prevWasCr = true;
    CHECK(SerialBridge::applyRxImplicitCr(QByteArray(), prevWasCr)
          == QByteArray());
    CHECK(prevWasCr);

    // 现实场景模拟：echo "line two\n" 发来的是 "line two\n"，分三次读到。
    // 第一块 "line "，第二块 "two"，第三块 "\n" —— 只有最后一块触发补 CR。
    prevWasCr = false;
    QByteArray out1 = SerialBridge::applyRxImplicitCr(QByteArray("line "), prevWasCr);
    CHECK(out1 == QByteArray("line "));
    CHECK(!prevWasCr);

    QByteArray out2 = SerialBridge::applyRxImplicitCr(QByteArray("two"), prevWasCr);
    CHECK(out2 == QByteArray("two"));
    CHECK(!prevWasCr);

    QByteArray out3 = SerialBridge::applyRxImplicitCr(QByteArray("\n"), prevWasCr);
    CHECK(out3 == QByteArray("\r\n"));
    CHECK(!prevWasCr);

    // 连起来：line two\r\n，屏幕上光标会回到行首。
    CHECK((out1 + out2 + out3) == QByteArray("line two\r\n"));

    qInfo() << "testRxImplicitCr: all checks passed";
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
    CHECK(s.rxImplicitCr == true);   // 默认开启，避免阶梯输出

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

// 可编辑下拉框的取值解析。回归用例：手输值曾被下拉框第一项覆盖。
//
// 起因是可编辑 QComboBox 在用户打字时**不会**改动 currentIndex，
// currentData() 仍返回上一次选中项的值；端口下拉框的显示文本又与 userData
// 刻意不同（"COM3 — CH340" vs "COM3"），于是手输的 /dev/ttys004 会被
// 静默换成列表第一项的端口名保存下去。
static void testComboResolution()
{
    // --- 端口：显示文本与 userData 不同，模拟真实枚举结果 ---
    QComboBox port;
    port.setEditable(true);
    port.setInsertPolicy(QComboBox::NoInsert);
    port.addItem(QStringLiteral("cu.debug-console"), QStringLiteral("cu.debug-console"));
    port.addItem(QStringLiteral("cu.usbserial-0001 — CP2102"),
                 QStringLiteral("cu.usbserial-0001"));

    // 选中列表项：取 userData（纯端口名），不带描述后缀。
    port.setCurrentIndex(1);
    CHECK(serialcombo::portNameOf(&port) == QStringLiteral("cu.usbserial-0001"));
    port.setCurrentIndex(0);
    CHECK(serialcombo::portNameOf(&port) == QStringLiteral("cu.debug-console"));

    // 手输设备路径：currentIndex 仍停在 0，必须返回手输的文本。
    port.setCurrentText(QStringLiteral("/dev/ttys004"));
    CHECK(port.currentIndex() == 0);   // 前提成立才说明这个用例测到了点子上
    CHECK(serialcombo::portNameOf(&port) == QStringLiteral("/dev/ttys004"));

    // 手输后再选回列表项，仍应回到 userData。
    port.setCurrentIndex(1);
    CHECK(serialcombo::portNameOf(&port) == QStringLiteral("cu.usbserial-0001"));

    // 首尾空格去掉；纯空白等价于没填，交给 validate 拦截。
    port.setCurrentText(QStringLiteral("  /dev/ttys006  "));
    CHECK(serialcombo::portNameOf(&port) == QStringLiteral("/dev/ttys006"));
    port.setCurrentText(QStringLiteral("   "));
    CHECK(serialcombo::portNameOf(&port).isEmpty());

    // "(未检测到串口)" 占位项：data 为空，返回空串而不是占位文本。
    QComboBox empty;
    empty.setEditable(true);
    empty.addItem(QStringLiteral("(未检测到串口)"), QString());
    CHECK(serialcombo::portNameOf(&empty).isEmpty());
    empty.setCurrentText(QStringLiteral("/dev/ttys008"));
    CHECK(serialcombo::portNameOf(&empty) == QStringLiteral("/dev/ttys008"));

    CHECK(serialcombo::portNameOf(nullptr).isEmpty());

    // --- 波特率：同一个坑，非标准速率手输不能被选中项覆盖 ---
    QComboBox baud;
    baud.setEditable(true);
    baud.setInsertPolicy(QComboBox::NoInsert);
    baud.addItem(QStringLiteral("9600"), 9600);
    baud.addItem(QStringLiteral("115200"), 115200);
    baud.setCurrentIndex(1);
    CHECK(serialcombo::baudRateOf(&baud) == 115200);
    baud.setCurrentText(QStringLiteral("31250"));   // MIDI 波特率，不在预设里
    CHECK(serialcombo::baudRateOf(&baud) == 31250);
    baud.setCurrentText(QStringLiteral("abc"));     // 非法输入回落默认值
    CHECK(serialcombo::baudRateOf(&baud) == 115200);
}

#ifdef Q_OS_UNIX
// 虚拟串口（PTY）连接不被热插拔轮询误杀。
//
// 回归用例：pollPorts() 原先把"端口不在 QSerialPortInfo 枚举里"一律当成
// 设备被拔出。而 socat / openpty 造的虚拟串口从来不会被枚举，于是连上
// 约 1 秒后就被自动断开，报"串口 /dev/ttysNNN 已断开"。
//
// 这里用 openpty 造一对真伪终端（等价于 socat 的两端），让 SerialClient
// 打开 slave 侧，跑满 2 个轮询周期后确认仍然连着且能收发。
static void testVirtualPortSurvivesPolling()
{
    int master = -1, slave = -1;
    char slaveName[256] = {0};
    if (openpty(&master, &slave, slaveName, nullptr, nullptr) != 0) {
        qWarning() << "openpty failed, skipping virtual port test";
        return;
    }
    ::close(slave);   // slave 交给 QSerialPort 打开，避免两个 fd 抢数据

    const QString portPath = QString::fromUtf8(slaveName);
    // 前提确认：PTY 确实不在枚举表里，否则这个用例就没测到点子上。
    bool enumerated = false;
    for (const SerialPortDesc &p : SerialClient::availablePorts()) {
        if (p.portName == portPath || portPath.endsWith(p.portName))
            enumerated = true;
    }
    CHECK(!enumerated);

    SerialClient client;
    QString lastError;
    int disconnectCount = 0;
    QByteArray received;
    QObject::connect(&client, &SerialClient::disconnected,
                     [&]() { ++disconnectCount; });
    QObject::connect(&client, &SerialClient::errorOccurred,
                     [&](const QString &m) { lastError = m; });
    QObject::connect(&client, &SerialClient::dataReceived,
                     [&](const QByteArray &d) { received += d; });

    SerialSettings s;
    s.portName = portPath;
    CHECK(client.open(s));
    CHECK(client.isOpen());

    // 轮询周期是 1s，等 2.5s 覆盖两次以上。
    QEventLoop loop;
    QTimer::singleShot(2500, &loop, &QEventLoop::quit);
    loop.exec();

    // 核心断言：轮询跑过之后连接仍在，且没有发出过断开/移除通知。
    CHECK(client.isOpen());
    CHECK(client.state() == SerialClient::State::Connected);
    CHECK(disconnectCount == 0);
    if (!lastError.isEmpty())
        qWarning() << "unexpected error during polling:" << lastError;
    CHECK(lastError.isEmpty());

    // 仍然能真实收发。
    const QByteArray payload("still alive\r\n");
    CHECK(::write(master, payload.constData(), payload.size()) == payload.size());
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    loop.exec();
    CHECK(received.contains(QByteArray("still alive")));

    client.close();
    CHECK(disconnectCount == 1);   // 主动关闭才发一次
    ::close(master);
}
#endif // Q_OS_UNIX

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
    // 下拉框取值用例要构造 QComboBox，需要 QApplication；无显示环境下走
    // offscreen 平台插件，CI 里也能跑。
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);

    testNewlineConversion();
    testRxImplicitCr();
    testSettingsDefaults();
    testAvailablePorts();
    testComboResolution();
#ifdef Q_OS_UNIX
    testVirtualPortSurvivesPolling();
#endif
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
