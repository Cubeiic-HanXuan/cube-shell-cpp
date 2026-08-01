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

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

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

        qInfo() << "renderOk:" << renderOk << "modelOk:" << modelOk;
        QApplication::exit((renderOk && modelOk) ? 0 : 1);
    });

    return app.exec();
}
