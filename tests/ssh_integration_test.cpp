// End-to-end SSH integration test (headless).
//
// Connects to a real sshd (default 127.0.0.1:2222, the linuxserver/openssh-server
// container), opens a pty shell, injects a command via the bridge, and asserts
// that the terminal's Screen model ends up containing the marker — proving the
// full vertical slice works against a live server:
//   SshClient (libssh2) -> SshBridge (read loop) -> Session -> Screen render.
//
// Env overrides: CUBESSH_HOST, CUBESSH_PORT, CUBESSH_USER, CUBESSH_PASS.

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>

#include "ui/ssh_terminal_widget.h"
#include "config/DeviceConfigStore.h"

#include "qtermwidget.h"
#include "Session.h"
#include "Emulation.h"
#include "Screen.h"

#include <vector>

using namespace cubeshell;

static QString dumpScreen(QTermWidget *term)
{
    Konsole::Session *s = term->session();
    if (!s || !s->emulation())
        return QString();
    Konsole::Screen *screen = s->emulation()->currentScreen();
    if (!screen)
        return QString();
    // Dump visible lines.
    QString out;
    const int lines = screen->getLines();
    const int cols = screen->getColumns();
    // 元素个数先落到具名变量再传：写成 buf(size_t(cols)) 会被当成函数声明
    //（most vexing parse），而花括号会走 initializer_list 构造，只建出 1 个
    // 值为 Character(cols) 的元素——getImage 按 cols 列写入（契约见 Screen.cpp
    // 的 Q_ASSERT(size >= mergedLines * columns)），从第 2 个元素起就越界了。
    const size_t bufLen = size_t(cols);
    std::vector<Konsole::Character> buf(bufLen);
    for (int y = 0; y < lines; ++y) {
        screen->getImage(buf.data(), cols, y, y);
        QString line;
        for (int x = 0; x < cols; ++x)
            line.append(QChar(buf[size_t(x)].character));
        out += line.trimmed() + QLatin1Char('\n');
    }
    return out;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    DeviceEntry dev;
    dev.name     = QStringLiteral("docker-sshd");
    dev.host     = qEnvironmentVariable("CUBESSH_HOST", "127.0.0.1");
    dev.port     = quint16(qEnvironmentVariable("CUBESSH_PORT", "2222").toUShort());
    dev.username = qEnvironmentVariable("CUBESSH_USER", "testuser");
    dev.password = qEnvironmentVariable("CUBESSH_PASS", "testpass123");

    auto *term = new SshTerminalWidget(dev);
    term->resize(800, 480);
    term->show();

    const QString marker = QStringLiteral("CUBESHELL_SSH_OK_") + QString::number(QCoreApplication::applicationPid());

    bool connected = false;
    QObject::connect(term, &SshTerminalWidget::connected, [&]() {
        connected = true;
    });
    QObject::connect(term, &SshTerminalWidget::connectionFailed, [&](const QString &msg) {
        qWarning() << "CONNECTION FAILED:" << msg;
        QCoreApplication::exit(2);
    });

    term->connectToHost();

    // Once connected, type a command that echoes the marker.
    QTimer::singleShot(0, [&]() {
        // Poll for connection, then drive the terminal through its emulation
        // (same path as a real keystroke).
        QElapsedTimer t; t.start();
        auto *poll = new QTimer(term);
        QObject::connect(poll, &QTimer::timeout, [&, poll]() {
            if (connected) {
                poll->stop();
                Konsole::Session *s = term->findChild<QTermWidget *>()->session();
                if (s && s->emulation()) {
                    const QByteArray cmd = ("echo " + marker + "\n").toUtf8();
                    s->emulation()->sendData(cmd.constData(), int(cmd.size()));
                }
            } else if (t.elapsed() > 15000) {
                poll->stop();
                qWarning() << "TIMEOUT waiting for connect";
                QCoreApplication::exit(3);
            }
        });
        poll->start(100);
    });

    // After a grace period, dump the screen and check for the marker.
    QTimer::singleShot(9000, [&]() {
        const QString screen = dumpScreen(term->findChild<QTermWidget *>());
        const bool found = screen.contains(marker);
        qInfo() << "marker" << (found ? "FOUND" : "NOT FOUND");
        if (!found) {
            qDebug().noquote() << "---- screen dump ----\n" << screen << "\n---------------------";
            QCoreApplication::exit(4);
        } else {
            qInfo() << "SSH INTEGRATION PASS";
            // Save a rendered screenshot as visual proof of the terminal output.
            if (QTermWidget *tw = term->findChild<QTermWidget *>()) {
                tw->grab().save(QStringLiteral("/tmp/ssh_integration.png"));
                qInfo() << "screenshot: /tmp/ssh_integration.png";
            }
            QCoreApplication::exit(0);
        }
    });

    // Hard cap.
    QTimer::singleShot(20000, []() { qWarning() << "HARD TIMEOUT"; QCoreApplication::exit(5); });

    return app.exec();
}
