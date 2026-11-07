#include "ai_test_support.h"
#include "ai/AiConversationView.h"
#include "ai/AiChatPanel.h"
#include "ai/MarkdownRenderer.h"
#include "ai/TerminalExecutor.h"
#include "util/ThemeManager.h"
#include "qtermwidget.h"

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QToolButton>

using namespace cubeshell;
using namespace aitest;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { qWarning() << "FAIL" << #x << "line" << __LINE__; ++failures; } } while (0)

static void testMarkdown()
{
    const QString code = "```bash\n# literal heading\n**not bold**\nprintf '<tag>'\n";
    QTextDocument doc; doc.setHtml(MarkdownRenderer::renderBlock(code));
    CHECK(doc.toPlainText().contains("**not bold**")); CHECK(doc.toPlainText().contains("<tag>"));
    doc.setHtml(MarkdownRenderer::renderBlock("`**literal**`\n\n| A | B |\n|---|---|\n| 1 | 2 |\n"));
    CHECK(doc.toPlainText().contains("**literal**"));
    CHECK(doc.toHtml().contains("<table"));
    const QString source = "text\n```sh\nfirst\n\nlast\n```\nend";
    CHECK(MarkdownRenderer::splitBlocks(source).join("") == source);
    CHECK(MarkdownRenderer::escapeHtml("\"<x>&'").contains("&quot;"));
}

static quint64 addCommand(AiConversationModel &model, int index, bool fail = false)
{
    AiCommand command; command.cmd = QStringLiteral("printf repeated_command");
    command.description = QStringLiteral("检查项目 %1").arg(index);
    const quint64 id = model.addCommand(command);
    model.setCommandState(id, AiStepState::Running);
    AiCommandResult result; result.stepId = id; result.cmd = command.cmd;
    result.exitCode = fail ? 2 : 0; result.durationMs = 1200;
    result.stdoutText = QStringLiteral("fixture line\n").repeated(80) + "FULL_OUTPUT_END";
    result.stderrText = "STDERR_MARKER";
    model.setCommandResult(result);
    return id;
}

static void testTranscript()
{
    AiConversationModel first, second;
    AiConversationView view; view.resize(380, 600); view.show(); view.setModel(&first);
    const auto turn = first.beginTurn("检查服务器运行情况", "test-model");
    const auto message = first.beginMessage();
    first.appendDelta(message, "正在检查。", "可展示的思考摘要");
    const auto command = addCommand(first, 1, true);
    view.flush(); drain();
    auto *toggle = view.findChild<QToolButton *>(QStringLiteral("aiCommandToggle_%1").arg(command));
    CHECK(toggle); if (!toggle) return;
    CHECK(!toggle->isChecked());
    toggle->setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(toggle, &press); QApplication::sendEvent(toggle, &release);
    view.flush(); drain(); CHECK(toggle->isChecked()); CHECK(first.currentTurn()->expandedEntries.contains(command));
    bool stdoutVisible = false, stderrVisible = false;
    for (auto *editor : view.findChildren<QPlainTextEdit *>()) {
        stdoutVisible |= editor->isVisible() && editor->toPlainText().contains("FULL_OUTPUT_END");
        stderrVisible |= editor->isVisible() && editor->toPlainText().contains("STDERR_MARKER");
    }
    CHECK(stdoutVisible); CHECK(stderrVisible);
    auto *process = view.findChild<QToolButton *>(QStringLiteral("aiProcessToggle_%1").arg(turn));
    CHECK(process); process->click(); view.flush(); drain(); CHECK(!process->isChecked());
    const auto resultMessage = first.beginMessage();
    // A non-stream response has no deltas at all.
    first.finalizeMessage(resultMessage, "FINAL_MARKER\n\n|结果|状态|\n|---|---|\n|测试|完成|", true);
    first.finishTurn(AiTurnOutcome::Complete, "FINAL_MARKER\n\n|结果|状态|\n|---|---|\n|测试|完成|");
    drain();
    auto *report = view.findChild<QTextBrowser *>(QStringLiteral("aiFinalReport_%1").arg(turn));
    CHECK(report); CHECK(report->isVisible()); CHECK(report->toPlainText().count("FINAL_MARKER") == 1);
    CHECK(!process->isChecked());
    auto *copy = view.findChild<QToolButton *>(QStringLiteral("aiCopyResult_%1").arg(turn));
    CHECK(copy); copy->click(); CHECK(QApplication::clipboard()->text() == first.currentTurn()->finalText);
    view.setModel(&second);
    const auto backgroundTurn = first.beginTurn("后台任务", "test-model");
    const auto backgroundMessage = first.beginMessage();
    first.finalizeMessage(backgroundMessage, "BACKGROUND_REPORT", true);
    first.finishTurn(AiTurnOutcome::Complete, "BACKGROUND_REPORT");
    view.setModel(&first); drain();
    report = view.findChild<QTextBrowser *>(QStringLiteral("aiFinalReport_%1").arg(backgroundTurn));
    CHECK(report && report->toPlainText() == "BACKGROUND_REPORT");
    CHECK(first.turns().first().processCollapsed);
    CHECK(first.turns().first().expandedEntries.contains(command));
}

static void testScrollingAndThemes()
{
    const QFont original = QApplication::font();
    AiConversationModel model;
    AiConversationView view; view.resize(380, 500); view.setModel(&model); view.show();
    model.beginTurn("A long execution", "test-model");
    model.beginMessage();
    for (int i = 0; i < 35; ++i) addCommand(model, i, i == 4);
    const auto active = model.beginMessage();
    view.flush(); drain();
    CHECK(view.verticalScrollBar()->maximum() > 100);
    view.verticalScrollBar()->setValue(40); const int before = view.verticalScrollBar()->value();
    for (int i = 0; i < 10; ++i) model.appendDelta(active, "streaming\n", {});
    view.flush(); drain(); CHECK(view.verticalScrollBar()->value() == before);
    auto *jump = view.findChild<QToolButton *>("aiJumpToLatest"); CHECK(jump && jump->isVisible());
    jump->click(); drain(); CHECK(view.verticalScrollBar()->value() == view.verticalScrollBar()->maximum());
    for (bool dark : {false, true}) {
        ThemeManager::applyTheme(qApp, dark ? ThemeManager::Appearance::Dark : ThemeManager::Appearance::Light);
        for (double scale : {1.0, 1.5, 2.0}) {
            QFont font = original;
            if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * scale));
            else font.setPointSizeF(font.pointSizeF() * scale);
            QApplication::setFont(font);
            for (int width : {320, 380, 600, 900}) {
                view.resize(width, 600); drain();
                CHECK(view.widget()->width() <= view.viewport()->width());
                for (auto *button : view.findChildren<QToolButton *>())
                    if (button->isVisible()) CHECK(button->height() >= button->fontMetrics().height());
            }
        }
    }
    QApplication::setFont(original);
    model.finalizeMessage(active, "LAST_FRAME", true);
    model.finishTurn(AiTurnOutcome::Complete, "LAST_FRAME");
    auto *report = view.findChild<QTextBrowser *>(QStringLiteral("aiFinalReport_%1").arg(model.currentTurn()->id));
    CHECK(report && report->toPlainText() == "LAST_FRAME");
}

static void testTerminalCompletion()
{
    QTermWidget terminal(0); // no local process, no SSH connection
    TerminalExecutor executor; executor.setTerminal(&terminal);
    QTimer::singleShot(5, &executor, [&] { executor.onRawData("$ _CUBE_ID=1; printf ok\r\nok__CUBE_AI_END__:1:0\r\n"); });
    const auto first = executor.runBlocking("printf ok", 1000);
    CHECK(first.first == 0); CHECK(first.second == "ok");
    QTimer::singleShot(5, &executor, [&] { executor.onRawData("_CUBE_ID=2; test\noutput\n__CUBE_AI_END__:2:1"); });
    QTimer::singleShot(15, &executor, [&] { executor.onRawData("27\r\n"); });
    const auto split = executor.runBlocking("test", 1000);
    CHECK(split.first == 127); CHECK(split.second == "output");
    // An old queued dispatch/marker cannot finish a new run.
    const auto expired = executor.runBlocking("expired", 0); CHECK(expired.first == -1);
    QTimer::singleShot(5, &executor, [&] { executor.onRawData("__CUBE_AI_END__:3:0\n"); });
    QTimer::singleShot(15, &executor, [&] { executor.onRawData("new\n__CUBE_AI_END__:4:0\n"); });
    const auto fresh = executor.runBlocking("fresh", 1000);
    CHECK(fresh.first == 0); CHECK(fresh.second.contains("new"));
    QTimer::singleShot(5, &executor, [&] { executor.onRawData("partial data"); executor.cancel(); });
    const auto stopped = executor.runBlocking("cancel", 1000);
    CHECK(stopped.first == -1); CHECK(stopped.second.contains("partial data"));
    QTimer::singleShot(5, &executor, [&] {
        executor.onRawData(QString(600000, QLatin1Char('x')) + QStringLiteral("\n__CUBE_AI_END__:6:0\n"));
    });
    const auto capped = executor.runBlocking("large output", 1000);
    CHECK(capped.first == 0); CHECK(capped.second.startsWith(QStringLiteral("[输出过长")));

    AiConversationModel model; model.beginTurn("live output", "fixture");
    AiCommand command; command.cmd = "fixture"; const auto id = model.addCommand(command);
    model.setCommandState(id, AiStepState::Running);
    model.appendCommandOutput(id, QString(600000, QLatin1Char('x')));
    CHECK(model.currentTurn()->entries.last().result.stdoutText.size() <= 512 * 1024);
    CHECK(model.currentTurn()->entries.last().livePreviewLimited);
    AiCommandResult final; final.stepId = id; final.exitCode = 0; final.stdoutText = "FINAL_CAPTURE";
    model.setCommandResult(final);
    CHECK(!model.currentTurn()->entries.last().livePreviewLimited);
    CHECK(model.currentTurn()->entries.last().result.stdoutText == "FINAL_CAPTURE");
}

static void exportPreviews()
{
    const QString directory = qEnvironmentVariable("CUBESHELL_AI_PREVIEW_DIR");
    if (directory.isEmpty()) return;
    CHECK(QDir().mkpath(directory));
    for (bool dark : {true, false}) {
        ThemeManager::applyTheme(qApp, dark ? ThemeManager::Appearance::Dark : ThemeManager::Appearance::Light);
        for (int width : {380, 620}) {
            AiConversationModel model;
            AiConversationView view; view.resize(width, 900); view.setModel(&model); view.show();
            model.beginTurn(QStringLiteral("检查 nginx 服务和 80 端口，定位访问失败的原因。"), QStringLiteral("GLM · SSH 运维"));
            const auto message = model.beginMessage();
            model.appendDelta(message, QStringLiteral("我会先检查服务状态，再确认端口监听与配置是否有效。"), {});
            const QStringList commands = {QStringLiteral("systemctl status nginx --no-pager"), QStringLiteral("nginx -t"), QStringLiteral("ss -lntp '( sport = :80 )'")};
            for (int i = 0; i < commands.size(); ++i) {
                AiCommand command; command.cmd = commands.at(i);
                command.description = QStringLiteral("检查服务状态与配置");
                const auto id = model.addCommand(command);
                AiCommandResult result; result.stepId = id; result.cmd = command.cmd;
                result.exitCode = i == 0 ? 3 : 0; result.durationMs = i == 0 ? 812 : 135;
                result.stdoutText = i == 0 ? QStringLiteral("Active: inactive (dead)") : QStringLiteral("验证信息已收集");
                model.setCommandResult(result);
            }
            const QString report = QStringLiteral("### 检查结果\n\n**Nginx 当前未运行，80 端口没有监听。**\n\n"
                "- 配置语法检查通过，未发现配置错误。\n- 服务状态为 `inactive (dead)`。\n- 本次仅做只读检查，没有修改服务器配置。\n\n"
                "### 建议下一步\n\n确认服务是否需要启动；启动前可先检查最近的服务日志，排除异常退出原因。");
            const auto finalMessage = model.beginMessage(); model.finalizeMessage(finalMessage, report, true);
            model.finishTurn(AiTurnOutcome::Complete, report); view.flush(); drain();
            view.verticalScrollBar()->setValue(0); drain();
            CHECK(view.grab().save(directory + QStringLiteral("/ai-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
        }
    }
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    testMarkdown(); testTranscript(); testScrollingAndThemes(); testTerminalCompletion(); exportPreviews();
    qInfo() << "AI UI/terminal failures:" << failures;
    return failures ? 1 : 0;
}
