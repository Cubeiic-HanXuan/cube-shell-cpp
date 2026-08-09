// Telnet 端到端集成测试（headless）。
//
// 对着一个真实的 telnetd 走完整链路：
//   TcpClient (QTcpSocket) → TelnetProtocol (IAC 协商) → TcpBridge → Session → Screen
// 连上后（可选自动登录）发一条 echo，断言标记出现在终端的 Screen 模型里。
//
// 目标不可达时 exit(2)，CTest 记为 SKIPPED 而不是 FAILED（机制同
// ssh_integration_test，见 tests/CMakeLists.txt 的 SKIP_RETURN_CODE）。
//
// 起一个本机 telnetd 的最省事办法（详见 docs/Telnet功能测试验证说明书.md）：
//   socat TCP-LISTEN:2323,bind=127.0.0.1,reuseaddr,fork EXEC:"/bin/login -f $USER",pty,stderr,setsid
// bind=127.0.0.1 不能省——不写就监听所有网卡，等于把免密 shell 开放给局域网。
//
// 环境变量：CUBETELNET_HOST / CUBETELNET_PORT / CUBETELNET_USER / CUBETELNET_PASS。
// 填了 USER 就打开自动登录；留空则假定对端直接给 shell（socat -f 的情形）。

#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>

#include "net/TcpClient.h"
#include "ui/net_terminal_widget.h"

#include "Emulation.h"
#include "Screen.h"
#include "Session.h"
#include "qtermwidget.h"

#include <vector>

using namespace cubeshell;

static QString dumpScreen(QTermWidget *term)
{
    if (!term)
        return QString();
    Konsole::Session *s = term->session();
    if (!s || !s->emulation())
        return QString();
    Konsole::Screen *screen = s->emulation()->currentScreen();
    if (!screen)
        return QString();
    QString out;
    const int lines = screen->getLines();
    const int cols = screen->getColumns();
    // 元素个数先落到具名变量再传，理由同 ssh_integration_test.cpp：圆括号直接
    // 套 size_t(cols) 会被当成函数声明，花括号则只建出 1 个元素、写第 2 列越界。
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

    TcpSettings settings;
    settings.mode = QStringLiteral("telnet");
    settings.host = qEnvironmentVariable("CUBETELNET_HOST", QStringLiteral("127.0.0.1"));
    settings.port = quint16(qEnvironmentVariable("CUBETELNET_PORT",
                                                QStringLiteral("2323")).toUShort());
    settings.username = qEnvironmentVariable("CUBETELNET_USER");
    settings.password = qEnvironmentVariable("CUBETELNET_PASS");
    // 有用户名才走自动登录，顺带覆盖 TcpBridge 的登录状态机。
    settings.autoLogin = !settings.username.isEmpty();
    settings.connectTimeoutMs = 8000;

    auto *panel = new NetTerminalWidget(QStringLiteral("telnet"));
    panel->setSettings(settings);
    panel->resize(800, 480);
    panel->show();

    const QString marker = QStringLiteral("CUBESHELL_TELNET_OK_")
                           + QString::number(QCoreApplication::applicationPid());

    bool connected = false;
    QObject::connect(panel, &NetTerminalWidget::connected, [&]() { connected = true; });
    QObject::connect(panel, &NetTerminalWidget::connectionFailed,
                     [](const QString &msg) {
                         // 连不上不算失败：多数环境里没有 telnetd。
                         qWarning() << "SKIP: cannot reach telnet server:" << msg;
                         QCoreApplication::exit(2);
                     });

    panel->connectToHost();

    // 连上后（自动登录还要几秒过 login/Password），发一条 echo。
    QTimer::singleShot(0, [&]() {
        auto *poll = new QTimer(panel);
        QElapsedTimer elapsed;
        elapsed.start();
        QObject::connect(poll, &QTimer::timeout, [&, poll]() {
            if (!connected) {
                if (elapsed.elapsed() > 10000) {
                    poll->stop();
                    qWarning() << "SKIP: timeout waiting for connect";
                    QCoreApplication::exit(2);
                }
                return;
            }
            // 连上后再等一段，让 banner 与自动登录走完，再打命令。
            if (elapsed.elapsed() < 6000)
                return;
            poll->stop();
            if (QTermWidget *tw = panel->terminal()) {
                if (Konsole::Session *s = tw->session()) {
                    if (Konsole::Emulation *e = s->emulation()) {
                        // 走 emulation 与真实按键同一条路（会经过换行转换与
                        // IAC 转义），比直接 client()->write() 覆盖得更多。
                        const QByteArray cmd = ("echo " + marker + "\r").toUtf8();
                        e->sendData(cmd.constData(), int(cmd.size()));
                    }
                }
            }
        });
        poll->start(200);
    });

    // 命令发出后留出回显时间，再看屏幕。
    QTimer::singleShot(14000, [&]() {
        const QString screen = dumpScreen(panel->terminal());
        // 屏幕上会有两处标记（输入回显 + 命令输出），这里只要求出现过。
        if (!screen.contains(marker)) {
            qDebug().noquote() << "---- screen dump ----\n" << screen
                               << "\n---------------------";
            qWarning() << "marker NOT FOUND";
            QCoreApplication::exit(4);
            return;
        }
        qInfo() << "TELNET INTEGRATION PASS";
        if (QTermWidget *tw = panel->terminal()) {
            tw->grab().save(QStringLiteral("/tmp/telnet_integration.png"));
            qInfo() << "screenshot: /tmp/telnet_integration.png";
        }
        QCoreApplication::exit(0);
    });

    // 兜底硬超时。
    QTimer::singleShot(20000, []() {
        qWarning() << "HARD TIMEOUT";
        QCoreApplication::exit(5);
    });

    return app.exec();
}
