// Shift+Tab（Backtab）必须作为按键送给远端，而不是被 Qt 焦点链吞掉。
//
// 回归背景：TerminalDisplay 覆写了 focusNextPrevChild()，但只对正向 Tab 返回
// false（"禁用 Tab 焦点导航"），反向 Shift+Tab 仍转发给 QWidget 基类。Qt 的
// QWidget::event() 在进 keyPressEvent 之前会先问焦点链：
//
//     if (!(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
//         if (k->key() == Qt::Key_Backtab
//             || (k->key() == Qt::Key_Tab && (k->modifiers() & Qt::ShiftModifier)))
//             res = focusNextPrevChild(false);
//         else if (k->key() == Qt::Key_Tab)
//             res = focusNextPrevChild(true);
//         if (res)
//             break;              // ← keyPressEvent 永远不会被调用
//     }
//
// 于是：窗口里只要还有别的可聚焦控件（cube-shell 有标签栏、工具条、文件浏览器、
// 搜索框……），Shift+Tab 就被拿去切焦点，终端一个字节都收不到。Claude Code 靠
// Backtab 的 "\x1b[Z" 轮换 accept-edits / plan / auto 模式，所以 Shift+Tab 失效。
// 正向 Tab 因为直接 return false 反而正常，这个不对称正是问题所在。
//
// 本测试复刻真实挂载形态（终端 + 一个可聚焦兄弟控件），直接给 TerminalDisplay
// 派发 Backtab，断言模拟器吐出的字节是 "\x1b[Z"。
#include <QApplication>
#include <QDebug>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "Emulation.h"
#include "Session.h"
#include "qtermwidget.h"

static QByteArray captured;

// 派发一个按键给终端视图，返回模拟器交给远端的字节。
static QByteArray sendKey(Konsole::TerminalDisplay *display,
                          int key,
                          Qt::KeyboardModifiers mods)
{
    captured.clear();
    QKeyEvent event(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(display, &event);
    QApplication::processEvents();
    return captured;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QWidget top;
    auto *layout = new QVBoxLayout(&top);
    auto *term = new QTermWidget(0, &top);
    // 焦点链里的“另一个控件”：没有它，QWidget::focusNextPrevChild 找不到落点会
    // 返回 false，Shift+Tab 就“碰巧”能到终端 —— 正是这个条件让问题只在集成环境
    // 里暴露，单控件的最小 demo 反而看不出来。
    auto *sibling = new QLineEdit(&top);
    layout->addWidget(term);
    layout->addWidget(sibling);
    top.resize(800, 600);
    top.show();

    QObject::connect(term->session()->emulation(), &Konsole::Emulation::sendData,
                     [](const char *data, int len) { captured.append(data, len); });

    term->terminalDisplay()->setFocus();
    QApplication::processEvents();

    bool ok = true;

    QTimer::singleShot(300, &app, [&]() {
        const QByteArray backtab = sendKey(term->terminalDisplay(),
                                           Qt::Key_Backtab, Qt::ShiftModifier);
        const bool backtabOk = (backtab == QByteArray("\x1b[Z"));
        qInfo().noquote() << "Shift+Tab (Key_Backtab):" << backtab.toHex(' ')
                          << (backtabOk ? "[OK]" : "[FAIL 期望 1b 5b 5a]");
        ok = backtabOk && ok;

        // 部分平台把 Shift+Tab 报成 Tab + ShiftModifier，两条路都要能出 "\x1b[Z"。
        const QByteArray tabShift = sendKey(term->terminalDisplay(),
                                            Qt::Key_Tab, Qt::ShiftModifier);
        const bool tabShiftOk = (tabShift == QByteArray("\x1b[Z"));
        qInfo().noquote() << "Shift+Tab (Key_Tab+Shift):" << tabShift.toHex(' ')
                          << (tabShiftOk ? "[OK]" : "[FAIL]");
        ok = tabShiftOk && ok;

        // 顺带确认没有把普通 Tab 改坏。
        const QByteArray tab = sendKey(term->terminalDisplay(), Qt::Key_Tab, Qt::NoModifier);
        const bool tabOk = (tab == QByteArray("\t"));
        qInfo().noquote() << "Tab (plain):" << tab.toHex(' ')
                          << (tabOk ? "[OK]" : "[FAIL 期望 09]");
        ok = tabOk && ok;

        qInfo() << (ok ? "shift_tab_test: PASS" : "shift_tab_test: FAIL");
        QApplication::exit(ok ? 0 : 1);
    });

    return app.exec();
}
