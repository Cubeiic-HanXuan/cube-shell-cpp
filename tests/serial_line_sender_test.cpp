// 多行发送：纯逻辑 + offscreen UI + Unix 虚拟串口真实字节流。不会访问真实设备。
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>

#include <functional>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <util.h>
#else
#include <pty.h>
#endif
#endif

#include "Emulation.h"
#include "Session.h"
#include "qtermwidget.h"
#include "serial/SerialBridge.h"
#include "serial/SerialClient.h"
#include "serial/SerialLineSender.h"
#include "serial_terminal_widget.h"
#include "dialogs/SerialSendDialog.h"

using namespace cubeshell;
using State = SerialLineSender::State;
using LineState = SerialLineSender::LineState;

static int failures = 0;
static int skipped = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 2000)
{
    if (condition())
        return true;
    QEventLoop loop;
    QTimer check;
    check.setInterval(5);
    QObject::connect(&check, &QTimer::timeout, &loop, [&]() {
        if (condition())
            loop.quit();
    });
    check.start();
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return condition();
}

struct Rig {
    QTermWidget term{0};
    SerialClient client;
    SerialBridge bridge{term.session(), &client};
    SerialLineSender sender{&client, &bridge};
    Rig() { bridge.start(); }
};

static void testSplitAndEditing()
{
    CHECK(SerialLineSender::splitLines(QString()).isEmpty());
    CHECK(SerialLineSender::splitLines(QStringLiteral("one")) == QStringList({"one"}));
    CHECK(SerialLineSender::splitLines(QStringLiteral("one\r\ntwo\nthree\rfour\r\n"))
          == QStringList({"one", "two", "three", "four"}));
    CHECK(SerialLineSender::splitLines(QStringLiteral("\n")) == QStringList({""}));
    CHECK(SerialLineSender::splitLines(QStringLiteral("\r\n\r\n")) == QStringList({"", ""}));
    CHECK(SerialLineSender::splitLines(QStringLiteral(" a \n\n b\t\n\n"))
          == QStringList({" a ", "", " b\t", ""}));
    CHECK(SerialLineSender::splitLines(QStringLiteral("中文\r\n  命令\n"))
          == QStringList({QStringLiteral("中文"), QStringLiteral("  命令")}));

    Rig rig;
    CHECK(rig.sender.setText(QStringLiteral("first\nsecond")));
    CHECK(!rig.sender.isConnected());
    CHECK(!rig.sender.sendNext());
    CHECK(!rig.sender.startAutomatic(10));
    CHECK(rig.sender.setLineText(0, QStringLiteral(" edited ")));
    CHECK(rig.sender.lines()[0].text == QStringLiteral(" edited "));
    CHECK(!rig.sender.setLineText(0, QStringLiteral("two\nlines")));
    CHECK(!rig.sender.setLineText(0, QStringLiteral("two\rlines")));
    CHECK(!rig.sender.setLineText(-1, QStringLiteral("bad")));
    CHECK(!rig.sender.setLineText(2, QStringLiteral("bad")));
    CHECK(!rig.sender.setText(QString(SerialLineSender::kMaxTextCharacters + 1, QLatin1Char('x'))));
    CHECK(!rig.sender.setText(QString(SerialLineSender::kMaxLines + 1, QLatin1Char('\n'))));
    CHECK(rig.sender.lines().size() == 2);   // 拒绝超限时不丢弃现有列表。
    CHECK(rig.sender.setText(QString()));
    CHECK(rig.sender.lines().isEmpty());
    CHECK(rig.sender.nextPendingLine() == -1);
}

static SerialSendDialog *openDialog(SerialTerminalWidget &widget)
{
    auto *button = widget.findChild<QPushButton *>(QStringLiteral("serialSendOpen"));
    CHECK(button);
    if (!button)
        return nullptr;
    button->click();
    auto *dialog = widget.findChild<SerialSendDialog *>();
    CHECK(dialog);
    return dialog;
}

static void testUiPreview()
{
    SerialTerminalWidget widget;
    SerialSettings settings;
    settings.portName = QStringLiteral("preview-only-not-a-device");
    settings.newlineMode = SerialSettings::NewlineMode::CrLf;
    widget.setSettings(settings);
    QApplication::clipboard()->setText(QStringLiteral("first\r\nsecond\n"));
    QMenu menu;
    menu.addAction(QStringLiteral("ordinary paste"));
    menu.addSeparator();
    widget.terminal()->contextMenuAboutToShow(&menu);
    auto *action = menu.findChild<QAction *>(QStringLiteral("serialPasteLines"));
    CHECK(action && action->isEnabled());
    if (!action)
        return;
    action->trigger();
    auto *dialog = widget.findChild<SerialSendDialog *>();
    CHECK(dialog);
    if (!dialog)
        return;
    CHECK(!dialog->isModal());
    CHECK(dialog->windowTitle().contains(settings.portName));
    auto *sender = dialog->findChild<SerialLineSender *>();
    auto *table = dialog->findChild<QTableWidget *>(QStringLiteral("serialSendLines"));
    auto *next = dialog->findChild<QPushButton *>(QStringLiteral("serialSendNext"));
    auto *automatic = dialog->findChild<QPushButton *>(QStringLiteral("serialSendAuto"));
    CHECK(sender && table && next && automatic);
    if (!sender || !table || !next || !automatic)
        return;
    CHECK(table->rowCount() == 2);
    CHECK(sender->sentCount() == 0);
    CHECK(!next->isEnabled() && !automatic->isEnabled());
    table->item(0, 1)->setText(QStringLiteral(" edited "));
    CHECK(sender->lines()[0].text == QStringLiteral(" edited "));
    table->item(0, 1)->setText(QStringLiteral("invalid\nline"));
    CHECK(table->item(0, 1)->text() == QStringLiteral(" edited "));

    // 重新打开不能悄悄重新读取剪贴板、丢掉现有编辑。
    dialog->reject();
    QApplication::clipboard()->setText(QStringLiteral("replacement"));
    CHECK(openDialog(widget) == dialog);
    CHECK(sender->lines()[0].text == QStringLiteral(" edited "));
    QTimer::singleShot(0, []() {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::No)->click();
    });
    dialog->loadClipboard();
    CHECK(sender->lines().size() == 2);
    CHECK(sender->lines()[0].text == QStringLiteral(" edited "));
    dialog->reject();

    QTermWidget ordinary(0);
    QMenu ordinaryMenu;
    ordinary.contextMenuAboutToShow(&ordinaryMenu);
    CHECK(ordinaryMenu.actions().isEmpty());   // 非串口终端不增加专用入口。
}

#ifdef Q_OS_UNIX
struct VirtualPort {
    int master = -1;
    QString name;
    VirtualPort()
    {
        int slave = -1;
        char path[256] = {};
        if (openpty(&master, &slave, path, nullptr, nullptr) != 0) {
            qWarning() << "SKIP: openpty unavailable; no real serial device is used";
            ++skipped;
            return;
        }
        ::close(slave);
        name = QString::fromUtf8(path);
        const int flags = fcntl(master, F_GETFL);
        CHECK(flags >= 0 && fcntl(master, F_SETFL, flags | O_NONBLOCK) == 0);
    }
    ~VirtualPort() { if (master >= 0) ::close(master); }
    bool open(SerialClient &client)
    {
        if (master < 0)
            return false;
        SerialSettings settings;
        settings.portName = name;
        const bool ok = client.open(settings);
        CHECK(ok);
        return ok;
    }
    QByteArray read()
    {
        QByteArray result;
        char buffer[16384];
        for (;;) {
            const ssize_t count = ::read(master, buffer, sizeof(buffer));
            if (count <= 0)
                break;
            result.append(buffer, int(count));
        }
        return result;
    }
};

static void testManualAndNewlines()
{
    VirtualPort port;
    Rig rig;
    if (!port.open(rig.client))
        return;
    int emulationSends = 0;
    QObject::connect(rig.term.session()->emulation(), &Konsole::Emulation::sendData,
                     &rig.term, [&]() { ++emulationSends; });
    CHECK(rig.sender.setText(QStringLiteral("one\ntwo\nthree")));
    pump(30);
    CHECK(port.read().isEmpty());
    CHECK(rig.sender.sendLine(1));
    CHECK(waitUntil([&]() { return !rig.sender.isBusy(); }));
    CHECK(port.read() == QByteArray("two\r"));
    CHECK(!rig.sender.sendLine(1));
    CHECK(!rig.sender.setLineText(1, QStringLiteral("do not implicitly resend")));
    CHECK(rig.sender.sendNext());
    CHECK(waitUntil([&]() { return !rig.sender.isBusy(); }));
    CHECK(port.read() == QByteArray("one\r"));
    CHECK(rig.sender.sendNext());
    CHECK(waitUntil([&]() { return !rig.sender.isBusy(); }));
    CHECK(port.read() == QByteArray("three\r"));
    CHECK(!rig.sender.sendNext());
    CHECK(rig.sender.sentCount() == 3);
    CHECK(emulationSends == 0);   // 不触发 Emulation 广播链路。

    const QVector<SerialSettings::NewlineMode> modes = {
        SerialSettings::NewlineMode::Cr, SerialSettings::NewlineMode::Lf,
        SerialSettings::NewlineMode::CrLf
    };
    const QVector<QByteArray> endings = {"\r", "\n", "\r\n"};
    QByteArray echoed;
    rig.bridge.setLocalEcho(true);
    QObject::connect(&rig.bridge, &SerialBridge::echoData, &rig.term,
                     [&](const QByteArray &data) { echoed += data; });
    for (int i = 0; i < modes.size(); ++i) {
        echoed.clear();
        rig.bridge.setNewlineMode(modes[i]);
        CHECK(rig.sender.setText(QStringLiteral("  中文\r\n\r\nx\n")));
        CHECK(rig.sender.lines().size() == 3);
        QByteArray expected;
        for (const auto &line : rig.sender.lines())
            expected += line.text.toUtf8() + endings[i];
        CHECK(rig.sender.startAutomatic(10));
        CHECK(waitUntil([&]() { return !rig.sender.isBusy(); }));
        CHECK(port.read() == expected);
        CHECK(echoed == expected);
    }
}

static void testAutomaticControl()
{
    VirtualPort port;
    Rig rig;
    if (!port.open(rig.client))
        return;
    QElapsedTimer clock;
    clock.start();
    QVector<qint64> sentAt;
    QObject::connect(&rig.sender, &SerialLineSender::changed, &rig.sender, [&]() {
        if (rig.sender.sentCount() > sentAt.size())
            sentAt.append(clock.elapsed());
    });
    CHECK(rig.sender.setText(QStringLiteral("a\nb\nc\nd")));
    CHECK(rig.sender.startAutomatic(80));
    CHECK(!rig.sender.setText(QStringLiteral("cannot overwrite")));
    CHECK(!rig.sender.setLineText(0, QStringLiteral("cannot edit")));
    CHECK(!rig.sender.sendLine(1));
    CHECK(waitUntil([&]() { return rig.sender.sentCount() == 1; }));
    rig.sender.pause();
    CHECK(rig.sender.state() == State::Paused);
    CHECK(port.read() == QByteArray("a\r"));
    pump(180);
    CHECK(port.read().isEmpty());
    CHECK(!rig.sender.sendNext());
    CHECK(!rig.sender.setText(QStringLiteral("paused still locks editing")));
    rig.sender.resume();
    CHECK(waitUntil([&]() { return rig.sender.sentCount() == 2; }));
    rig.sender.stop();
    CHECK(rig.sender.state() == State::Idle);
    CHECK(port.read() == QByteArray("b\r"));
    pump(180);
    CHECK(port.read().isEmpty());
    CHECK(rig.sender.startAutomatic(80));
    CHECK(waitUntil([&]() { return rig.sender.sentCount() == 4; }));
    CHECK(port.read() == QByteArray("c\rd\r"));
    CHECK(sentAt.size() == 4);
    if (sentAt.size() == 4) {
        CHECK(sentAt[1] - sentAt[0] >= 80);
        CHECK(sentAt[3] - sentAt[2] >= 78);
    }

    // 在首个异步回调前停止，不能残留零间隔计时器触发发送。
    CHECK(rig.sender.setText(QStringLiteral("never sent")));
    CHECK(rig.sender.startAutomatic(10));
    rig.sender.stop();
    pump(60);
    CHECK(port.read().isEmpty());
    CHECK(rig.sender.sentCount() == 0);
}

static void testOrdinaryPasteInterrupts()
{
    VirtualPort port;
    Rig rig;
    if (!port.open(rig.client))
        return;
    CHECK(rig.sender.setText(QStringLiteral("queued1\nqueued2")));
    CHECK(rig.sender.startAutomatic(150));
    CHECK(waitUntil([&]() { return rig.sender.sentCount() == 1; }));
    CHECK(port.read() == QByteArray("queued1\r"));
    QApplication::clipboard()->setText(QStringLiteral("ordinary1\r\nordinary2\n"));
    rig.term.pasteClipboard();
    CHECK(rig.sender.state() == State::Paused);
    CHECK(waitUntil([&]() { return rig.client.bytesToWrite() == 0; }));
    CHECK(port.read() == QByteArray("ordinary1\rordinary2\r"));
    pump(200);
    CHECK(port.read().isEmpty());
    CHECK(rig.sender.lines()[1].state == LineState::Pending);
    rig.sender.stop();
}

static void testDisconnectAndFailures()
{
    VirtualPort port;
    Rig rig;
    if (!port.open(rig.client))
        return;
    CHECK(rig.sender.setText(QStringLiteral("first\nsecond")));
    CHECK(rig.sender.startAutomatic(150));
    CHECK(waitUntil([&]() { return rig.sender.sentCount() == 1; }));
    CHECK(port.read() == QByteArray("first\r"));
    rig.client.close();
    CHECK(rig.sender.state() == State::Idle);
    CHECK(port.open(rig.client));
    pump(220);
    CHECK(port.read().isEmpty());
    CHECK(rig.sender.lines()[1].state == LineState::Pending);

    // 提交当前行时断开：可能部分写出，不能自动重试该行。
    CHECK(rig.sender.setText(QStringLiteral("uncertain\nnot submitted")));
    bool disconnected = false;
    const auto connection = QObject::connect(&rig.sender, &SerialLineSender::changed, &rig.sender, [&]() {
        if (!disconnected && !rig.sender.lines().isEmpty()
            && rig.sender.lines()[0].state == LineState::Sending) {
            disconnected = true;
            rig.client.close();
        }
    });
    rig.sender.sendNext();
    QObject::disconnect(connection);
    CHECK(disconnected);
    CHECK(rig.sender.lines()[0].state == LineState::Uncertain);
    CHECK(rig.sender.lines()[1].state == LineState::Pending);
    CHECK(!rig.sender.isBusy());
    port.read();
    CHECK(port.open(rig.client));
    pump(100);
    CHECK(port.read().isEmpty());
    CHECK(!rig.sender.sendLine(0));

    // bridge 停止导致同步失败；不允许“已发送”假成功。
    CHECK(rig.sender.setText(QStringLiteral("failure")));
    rig.bridge.stop();
    CHECK(!rig.sender.sendNext());
    CHECK(rig.sender.lines()[0].state == LineState::Uncertain);
    CHECK(rig.sender.sentCount() == 0);
    CHECK(!rig.sender.isBusy());
    CHECK(port.read().isEmpty());

    // 同步失败通知期间重载列表，不得在返回后继续访问旧行索引。
    CHECK(rig.sender.setText(QStringLiteral("failure again")));
    bool reset = false;
    const auto resetConnection = QObject::connect(&rig.sender, &SerialLineSender::changed, &rig.sender, [&]() {
        if (!reset && !rig.sender.isBusy() && !rig.sender.lines().isEmpty()
            && rig.sender.lines()[0].state == LineState::Uncertain) {
            reset = true;
            rig.sender.setText(QString());
        }
    });
    CHECK(!rig.sender.sendNext());
    QObject::disconnect(resetConnection);
    CHECK(reset && rig.sender.lines().isEmpty());
}

static void testBackpressure()
{
    VirtualPort port;
    Rig rig;
    if (!port.open(rig.client))
        return;
    const QByteArray backlog(2 * 1024 * 1024, 'x');
    CHECK(rig.client.write(backlog));
    CHECK(rig.sender.setText(QStringLiteral("after backlog\nlast")));
    CHECK(!rig.sender.canSendManually());
    CHECK(rig.sender.startAutomatic(30));
    pump(50);   // 不读 master，制造真实的本机发送积压。
    CHECK(rig.client.bytesToWrite() > 0);
    CHECK(rig.sender.lines()[0].state == LineState::Pending);
    QByteArray received;
    // macOS PTY 每次可读量很小，按就绪事件读取，不能靠 5ms 轮询限制接收吞吐。
    QSocketNotifier reader(port.master, QSocketNotifier::Read);
    QObject::connect(&reader, &QSocketNotifier::activated, &reader, [&]() {
        received += port.read();
    });
    CHECK(waitUntil([&]() { return !rig.sender.isBusy(); }, 5000));
    received += port.read();
    if (rig.sender.isBusy())
        qWarning() << "backpressure:" << received.size() << "received,"
                   << rig.client.bytesToWrite() << "queued, state" << int(rig.sender.state())
                   << rig.sender.message();
    CHECK(received == backlog + QByteArray("after backlog\rlast\r"));
}

static void testSessionBindingAndClose()
{
    VirtualPort firstPort, secondPort;
    if (firstPort.master < 0 || secondPort.master < 0)
        return;
    QTabWidget tabs;
    auto *first = new SerialTerminalWidget;
    auto *second = new SerialTerminalWidget;
    tabs.addTab(first, QStringLiteral("first"));
    tabs.addTab(second, QStringLiteral("second"));
    SerialSettings settings;
    settings.portName = firstPort.name;
    first->setSettings(settings);
    first->connectToPort();
    settings.portName = secondPort.name;
    second->setSettings(settings);
    second->connectToPort();
    CHECK(first->client()->isOpen() && second->client()->isOpen());
    auto *dialog = openDialog(*first);
    if (!dialog)
        return;
    auto *sender = dialog->findChild<SerialLineSender *>();
    CHECK(sender);
    if (!sender)
        return;
    CHECK(sender->setText(QStringLiteral("bound1\nbound2\nbound3")));
    tabs.setCurrentIndex(1);
    CHECK(sender->sendNext());
    CHECK(waitUntil([&]() { return !sender->isBusy(); }));
    CHECK(firstPort.read() == QByteArray("bound1\r"));
    CHECK(secondPort.read().isEmpty());
    auto *interval = dialog->findChild<QSpinBox *>(QStringLiteral("serialSendInterval"));
    auto *automatic = dialog->findChild<QPushButton *>(QStringLiteral("serialSendAuto"));
    auto *importButton = dialog->findChild<QPushButton *>(QStringLiteral("serialSendImport"));
    auto *next = dialog->findChild<QPushButton *>(QStringLiteral("serialSendNext"));
    auto *newline = first->findChild<QComboBox *>(QStringLiteral("serialNewlineMode"));
    CHECK(interval && automatic && importButton && next && newline);
    if (!interval || !automatic || !importButton || !next || !newline)
        return;
    interval->setValue(150);
    automatic->click();
    CHECK(sender->state() == State::Automatic);
    CHECK(!interval->isEnabled() && !importButton->isEnabled() && !next->isEnabled());
    CHECK(!newline->isEnabled());
    CHECK(waitUntil([&]() { return sender->sentCount() == 2; }));
    CHECK(firstPort.read() == QByteArray("bound2\r"));
    dialog->reject();   // Escape / 标题栏关闭同样经过 done()。
    CHECK(sender->state() == State::Idle);
    CHECK(interval->isEnabled() && importButton->isEnabled() && next->isEnabled());
    CHECK(newline->isEnabled());
    pump(220);
    CHECK(firstPort.read().isEmpty());
    CHECK(secondPort.read().isEmpty());
    CHECK(sender->lines()[2].state == LineState::Pending);
    CHECK(openDialog(*first) == dialog);
    pump(180);
    CHECK(firstPort.read().isEmpty());
    CHECK(sender->sentCount() == 2);

    // 父会话销毁应取消队列，不让计时器访问已销毁的 bridge/client。
    CHECK(sender->startAutomatic(100));
    tabs.removeTab(0);
    delete first;
    pump(160);
    CHECK(firstPort.read().isEmpty());
    CHECK(secondPort.read().isEmpty());
}
#endif

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    testSplitAndEditing();
    testUiPreview();
#ifdef Q_OS_UNIX
    testManualAndNewlines();
    testAutomaticControl();
    testOrdinaryPasteInterrupts();
    testDisconnectAndFailures();
    testBackpressure();
    testSessionBindingAndClose();
#else
    ++skipped;
    qInfo() << "SKIP: openpty integration cases are Unix-only; use com0com for Windows acceptance";
#endif
    qInfo() << "serial_line_sender_test:" << failures << "failures," << skipped << "skipped";
    return failures ? 1 : 0;
}
