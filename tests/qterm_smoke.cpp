// Headless smoke test for the ported qtermwidget library.
//
// Runs a shell command with known output, pumps the event loop, then checks:
//   1. the widget grabs to a non-null image (render pipeline alive), and
//   2. the underlying Screen model actually contains the echoed text
//      (session + emulation + screen pipeline alive).
// Exits 0 only if both hold.

#include <QApplication>
#include <QImage>
#include <QTimer>
#include <QDebug>

#include "qtermwidget.h"
#include "Session.h"
#include "Emulation.h"
#include "Screen.h"
#include "Character.h"
#include "TerminalCharacterDecoder.h"

using namespace Konsole;

// Dump the visible text of a screen as a single string (for assertion).
static QString screenText(Screen *screen)
{
    QString result;
    QTextStream stream(&result);
    PlainTextDecoder decoder;
    decoder.begin(&stream);
    screen->writeLinesToStream(&decoder, 0, screen->getLines() + screen->getHistLines());
    decoder.end();
    stream.flush();
    return result;
}

// clear() 必须既擦内容又把光标送回左上角。
//
// 回归用例：Screen::clearEntireScreen() 只擦内容不动光标（这是 VT 的 ED /
// ESC[2J 语义，必须保持），QTermWidget::clear() 早先只调它，于是"清屏"按钮
// 按下后光标停在原处。SSH 会话看不出来——shell 会重画提示符把光标带回去；
// 串口对面是裸设备，不重画，光标就一直悬在原来的行列上。
static bool clearHomesCursor()
{
    // startnow=0：不拉起本地 shell，与串口面板的用法一致。
    QTermWidget term(0);
    term.resize(640, 480);
    Session *session = term.session();
    Emulation *emu = session ? session->emulation() : nullptr;
    if (!emu || !emu->currentScreen())
        return false;

    // 灌几行进去，把光标推离原点。
    const QByteArray blob = "line one\r\nline two\r\nline three\r\nABC";
    session->onReceiveBlock(blob.constData(), blob.size());

    Screen *screen = emu->currentScreen();
    const int beforeX = screen->getCursorX(), beforeY = screen->getCursorY();
    if (beforeX == 0 && beforeY == 0) {
        qWarning() << "clearHomesCursor: 前提不成立，光标本来就在原点";
        return false;
    }

    term.clear();
    Screen *after = emu->currentScreen();
    const int x = after->getCursorX(), y = after->getCursorY();
    qInfo() << "clear(): 光标" << beforeX << beforeY << "->" << x << y;
    return x == 0 && y == 0;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const bool clearOk = clearHomesCursor();

    QTermWidget term(0);
    term.resize(640, 480);
    term.setShellProgram(QStringLiteral("/bin/sh"));
    term.setArgs({QStringLiteral("-c"), QStringLiteral("echo CUBESHELL_RENDER_OK; echo line2; sleep 2")});
    term.show();
    term.startShellProgram();

    QTimer::singleShot(1800, &app, [&]() {
        QImage img = term.grab().toImage();
        img.save(QStringLiteral("/tmp/qterm_smoke.png"));

        Session *session = term.session();
        Emulation *emu = session ? session->emulation() : nullptr;
        Screen *screen = emu ? emu->currentScreen() : nullptr;

        const QString text = screen ? screenText(screen) : QString();
        qInfo() << "image:" << img.size() << "nonNull:" << !img.isNull();
        qInfo() << "screen text:\n" << text;

        const bool renderOk = !img.isNull() && img.width() > 100;
        const bool modelOk = text.contains(QStringLiteral("CUBESHELL_RENDER_OK"));

        qInfo() << "renderOk:" << renderOk << "modelOk:" << modelOk << "clearOk:" << clearOk;
        QApplication::exit((renderOk && modelOk && clearOk) ? 0 : 1);
    });

    return app.exec();
}
