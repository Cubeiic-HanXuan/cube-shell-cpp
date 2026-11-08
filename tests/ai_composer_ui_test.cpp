#include "ai/AiChatPanel.h"
#include "util/ThemeManager.h"
#include "widgets/ConversationWidgets.h"
#ifdef CUBESHELL_WITH_VOICE
#include "ai/VoiceInput.h"
#endif

#include <QApplication>
#include <QAbstractItemView>
#include <QAbstractItemDelegate>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFrame>
#include <QImage>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionViewItem>
#include <QThread>
#include <QToolButton>

using namespace cubeshell;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { qWarning() << "FAIL" << #x << "line" << __LINE__; ++failures; } } while (0)

static void settle()
{
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 60) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

static void enter(QPlainTextEdit *editor, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, modifiers, QStringLiteral("\n"));
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, modifiers);
    QApplication::sendEvent(editor, &press); QApplication::sendEvent(editor, &release);
    settle();
}

static void checkModeTextFits(QComboBox *mode)
{
    QStyleOptionComboBox option;
    option.initFrom(mode);
    option.currentText = mode->currentText();
    const QRect textRect = mode->style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, mode);
    const QRect arrowRect = mode->style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, mode);
    CHECK(!textRect.intersects(arrowRect));
    CHECK(textRect.height() >= mode->fontMetrics().height());
    for (int i = 0; i < mode->count(); ++i) {
        const int textWidth = mode->fontMetrics().horizontalAdvance(mode->itemText(i));
        if (textRect.width() < textWidth)
            qWarning() << "Mode text clipped:" << mode->itemText(i) << "available" << textRect.width() << "required" << textWidth
                       << "control" << mode->size() << "hint" << mode->sizeHint() << "minimum" << mode->minimumSizeHint()
                       << "panel" << mode->window()->size();
        CHECK(textRect.width() >= textWidth);
    }
}

static void testLayoutAndPreviews()
{
    const QFont original = QApplication::font();
    const QString output = qEnvironmentVariable("CUBESHELL_AI_PREVIEW_DIR");
    if (!output.isEmpty()) CHECK(QDir().mkpath(output));
    for (bool dark : {true, false}) {
        ThemeManager::applyTheme(qApp, dark ? ThemeManager::Appearance::Dark : ThemeManager::Appearance::Light);
        AiChatPanel panel; panel.resize(380, 600); panel.show(); settle();
        auto *frame = panel.findChild<QFrame *>("aiInputFrame");
        auto *editor = panel.findChild<QPlainTextEdit *>("aiInput");
        auto *send = panel.findChild<QPushButton *>("aiSend");
        auto *stop = panel.findChild<QPushButton *>("aiStop");
        auto *mode = panel.findChild<QComboBox *>("aiChatMode");
        CHECK(frame && editor && send && stop && mode);
        if (!frame || !editor || !send || !stop || !mode) return;
        CHECK(send->text().isEmpty()); CHECK(!send->icon().isNull());
        CHECK(!send->accessibleName().isEmpty()); CHECK(!send->isEnabled());
        for (double scale : {1.0, 1.5, 2.0}) {
            auto font = original;
            if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * scale));
            else font.setPointSizeF(font.pointSizeF() * scale);
            QApplication::setFont(font);
            panel.setFont(font);
            for (int width : {300, 320, 380, 620, 900}) {
                panel.resize(width, 600); editor->clear(); editor->setFocus(); settle();
                CHECK(frame->property("inputFocused").toBool());
                CHECK(frame->width() <= panel.width() - 16);
                CHECK(frame->height() < panel.height() * .45);
                CHECK(send->width() == send->height());
                CHECK(send->height() == qMax(32, panel.fontMetrics().height() + 8));
                const int belowEditor = frame->height() - editor->geometry().bottom() - 1;
                CHECK(belowEditor <= qMax(send->height(), mode->height()) + 12);
                if (scale == 1.0) {
                    if (belowEditor > 44) qWarning() << "Toolbar height" << belowEditor << "panel font" << panel.fontMetrics().height()
                                                    << "mode font" << mode->fontMetrics().height() << "mode size" << mode->size()
                                                    << "mode hint" << mode->sizeHint();
                    CHECK(belowEditor <= 44);
                }
                checkModeTextFits(mode);
                const QList<QWidget *> controls = {mode, send, panel.findChild<QToolButton *>("aiClear"), panel.findChild<QToolButton *>("aiMic")};
                for (auto *control : controls) if (control && control->isVisible()) {
                    const QRect bounds(control->mapTo(frame, QPoint()), control->size());
                    CHECK(frame->rect().contains(bounds));
                }
                const int resting = editor->height();
                const int restingViewportWidth = editor->viewport()->width();
                editor->setPlainText(QStringLiteral("一行输入内容\n").repeated(30)); settle();
                CHECK(editor->height() >= resting);
                CHECK(frame->height() < panel.height() * .45);
                CHECK(editor->verticalScrollBar()->maximum() > 0);
                editor->clear(); settle();
                if (editor->height() != resting)
                    qWarning() << "Resting height changed" << dark << scale << width << "heights" << resting << editor->height()
                               << "viewport widths" << restingViewportWidth << editor->viewport()->width();
                CHECK(editor->height() == resting);
                if (!output.isEmpty() && scale == 1.0 && (width == 380 || width == 620)) {
                    CHECK(frame->grab().save(output + QStringLiteral("/composer-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
                }
            }
        }
    }
    QApplication::setFont(original);
}

static void testModePopup()
{
    const QFont original = QApplication::font();
    const QString output = qEnvironmentVariable("CUBESHELL_AI_PREVIEW_DIR");
    for (bool dark : {true, false}) {
        ThemeManager::applyTheme(qApp, dark ? ThemeManager::Appearance::Dark : ThemeManager::Appearance::Light);
        for (double scale : {1.0, 1.5, 2.0}) {
            auto font = original;
            if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * scale));
            else font.setPointSizeF(font.pointSizeF() * scale);
            QApplication::setFont(font);
            AiChatPanel panel; panel.resize(320, 600); panel.show(); settle();
            auto *mode = panel.findChild<QComboBox *>("aiChatMode");
            CHECK(mode && !mode->accessibleName().isEmpty());
            if (!mode) continue;
            checkModeTextFits(mode);
            mode->showPopup(); settle();
            auto *view = mode->view();
            const auto colors = sessionui::colors(panel.palette());
            CHECK(view->palette().color(QPalette::Base) == colors.surface);
            CHECK(view->palette().color(QPalette::Text) == colors.text);
            CHECK(view->palette().color(QPalette::Highlight) == colors.accentSoft);
            CHECK(view->isVisible());
            CHECK(view->width() >= mode->width());
            CHECK(view->fontMetrics().height() >= QFontMetrics(font).height());
            for (int i = 0; i < mode->count(); ++i) {
                const QModelIndex index = mode->model()->index(i, mode->modelColumn());
                QStyleOptionViewItem option;
                option.initFrom(view);
                option.widget = view;
                const QSize hint = view->itemDelegate()->sizeHint(option, index);
                const QRect row = view->visualRect(index);
                CHECK(view->viewport()->rect().contains(row));
                CHECK(row.width() >= hint.width());
                CHECK(row.height() >= hint.height());
                CHECK(row.height() >= view->fontMetrics().height() + 12);
            }
            const QRect currentRow = view->visualRect(view->currentIndex());
            const QPoint sample = currentRow.topLeft() + QPoint(4, currentRow.height() / 2);
            const auto image = view->viewport()->grab().toImage();
            CHECK(image.pixelColor(qRound(sample.x() * image.devicePixelRatio()),
                                   qRound(sample.y() * image.devicePixelRatio())).rgb() == colors.accentSoft.rgb());
            if (!output.isEmpty() && scale == 1.0)
                CHECK(view->grab().save(output + QStringLiteral("/mode-popup-%1.png").arg(dark ? "dark" : "light")));
            QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
            QKeyEvent confirm(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(view, &down);
            QApplication::sendEvent(view, &confirm); settle();
            CHECK(!view->isVisible());
            CHECK(panel.chatMode() == AiChatPanel::ChatMode::PlainChat);
            checkModeTextFits(mode);
        }
    }
    QApplication::setFont(original);
}

static void testInputAndBusyState()
{
    AiChatPanel panel; panel.resize(380, 600); panel.show(); settle();
    auto *editor = panel.findChild<QPlainTextEdit *>("aiInput");
    auto *frame = panel.findChild<QFrame *>("aiInputFrame");
    auto *send = panel.findChild<QPushButton *>("aiSend");
    auto *stop = panel.findChild<QPushButton *>("aiStop");
    auto *clear = panel.findChild<QToolButton *>("aiClear");
    auto *mode = panel.findChild<QComboBox *>("aiChatMode");
    QStringList sent; int stops = 0, clears = 0, modeChanges = 0;
    QObject::connect(&panel, &AiChatPanel::userMessageSent, &panel, [&](const QString &text) { sent.append(text); });
    QObject::connect(&panel, &AiChatPanel::stopRequested, &panel, [&] { ++stops; panel.setBusy(false); });
    QObject::connect(&panel, &AiChatPanel::clearRequested, &panel, [&] { ++clears; });
    QObject::connect(&panel, &AiChatPanel::chatModeChanged, &panel, [&] { ++modeChanges; });
    editor->setPlainText("  "); settle(); CHECK(!send->isEnabled());
    editor->setPlainText("first"); editor->moveCursor(QTextCursor::End); enter(editor, Qt::ShiftModifier);
    CHECK(sent.isEmpty()); CHECK(editor->toPlainText().contains('\n'));
    editor->insertPlainText("second"); enter(editor);
    CHECK(sent.size() == 1); CHECK(sent.value(0) == "first\nsecond"); CHECK(!send->isEnabled());
    editor->setPlainText("next"); settle();
    const QPoint sendPosition = send->mapTo(frame, QPoint());
    const int frameHeight = frame->height();
    panel.setBusy(true); settle();
    CHECK(stop->isVisible() && !send->isVisible()); CHECK(!clear->isEnabled());
    CHECK(stop->size() == send->size()); CHECK(stop->mapTo(frame, QPoint()) == sendPosition);
    CHECK(frame->height() == frameHeight); CHECK(!editor->isReadOnly());
    enter(editor); CHECK(sent.size() == 1); CHECK(editor->toPlainText() == "next");
    stop->click(); settle(); CHECK(stops == 1 && send->isVisible() && send->isEnabled());
    send->click(); settle(); CHECK(sent.size() == 2); CHECK(sent.last() == "next");
    clear->click(); CHECK(clears == 1);
    mode->setCurrentIndex(1); CHECK(modeChanges == 1); CHECK(panel.chatMode() == AiChatPanel::ChatMode::PlainChat);
}

#ifdef CUBESHELL_WITH_VOICE
static void testVoiceButtonStates()
{
    AiChatPanel panel; panel.resize(380, 600); panel.show(); settle();
    auto *voice = panel.findChild<VoiceInput *>();
    auto *mic = panel.findChild<QToolButton *>("aiMic");
    auto *editor = panel.findChild<QPlainTextEdit *>("aiInput");
    CHECK(voice && mic && editor);
    if (!voice || !mic || !editor) return;
    const QString placeholder = editor->placeholderText();
    editor->setPlainText(QStringLiteral("保留已有草稿"));
    // Exercise only the UI signal contract; never request a real microphone in this test.
    voice->stateChanged(VoiceInput::State::RequestingPermission); settle();
    CHECK(!mic->isEnabled() && !mic->isChecked());
    voice->stateChanged(VoiceInput::State::Recording); settle();
    CHECK(mic->isEnabled() && mic->isChecked());
    voice->stateChanged(VoiceInput::State::Processing); settle();
    CHECK(!mic->isEnabled() && !mic->isChecked());
    voice->stateChanged(VoiceInput::State::Idle);
    // A click checks the button before a preflight error, without a state transition.
    mic->setChecked(true);
    voice->errorOccurred(QStringLiteral("合成的语音错误")); settle();
    CHECK(mic->isEnabled() && !mic->isChecked());
    CHECK(editor->placeholderText() == placeholder);
    CHECK(editor->toPlainText() == QStringLiteral("保留已有草稿"));
    voice->textRecognized(QString()); settle();
    CHECK(editor->toPlainText() == QStringLiteral("保留已有草稿"));
}
#endif

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    testLayoutAndPreviews(); testModePopup(); testInputAndBusyState();
#ifdef CUBESHELL_WITH_VOICE
    testVoiceButtonStates();
#endif
    qInfo() << "AI composer failures:" << failures;
    return failures ? 1 : 0;
}
