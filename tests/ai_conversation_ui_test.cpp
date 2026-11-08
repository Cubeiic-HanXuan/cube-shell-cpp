#include "ai_test_support.h"
#include "ai/AiConversationView.h"
#include "ai/AiChatPanel.h"
#include "ai/MarkdownRenderer.h"
#include "ai/MarkdownView.h"
#include "widgets/CodeHighlighter.h"
#include <QAction>
#include <QTextBlock>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextTable>
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

static void testRichMarkdown()
{
    MarkdownView view; view.resize(340, 900); view.show();
    const QString source = QStringLiteral(
        "## Deploy\n\nUse `nginx -t` and [documentation][ref].\n\n"
        "1. First\n\n2. Second\n   - nested\n\n> A quoted note\n\n"
        "- [x] validated\n- [ ] pending\n\n"
        "| Name | State |\n|---|---|\n| nginx | ready |\n\n"
        "```python\n# comment\nurl = \"https://example.com/#fragment\"\nif True:\n    print(42)\n```\n\n"
        "Between blocks.\n\n~~~json\n{\"success\": true}\n~~~\n\n"
        "[ref]: https://example.com/docs\n");
    view.setMarkdownText(source); drain();
    CHECK(view.toPlainText().contains("nginx -t"));
    CHECK(view.toPlainText().contains("nested"));
    CHECK(view.toHtml().contains("<table"));
    CHECK(view.toHtml().contains("https://example.com/docs"));
    auto copies = view.findChildren<QToolButton *>("aiCopyCode");
    CHECK(copies.size() == 2);
    bool pythonCopied = false, jsonCopied = false, highlighted = false;
    for (auto *copy : copies) {
        CHECK(copy->isVisible());
        copy->click();
        CHECK(copy->text() == QStringLiteral("已复制"));
        const QString copied = QApplication::clipboard()->text();
        pythonCopied |= copied == "# comment\nurl = \"https://example.com/#fragment\"\nif True:\n    print(42)";
        jsonCopied |= copied == "{\"success\": true}";
        if (copy->toolTip().contains("python"))
            CHECK(copy->y() < view.cursorRect(view.document()->find("# comment")).top());
    }
    CHECK(pythonCopied); CHECK(jsonCopied);
    for (auto block = view.document()->begin(); block.isValid(); block = block.next()) {
        if (block.text().contains("print(42)")) highlighted = block.layout()->formats().size() >= 2;
        if (block.text().startsWith("url =")) {
            const auto formats = block.layout()->formats();
            // '#' and '//' inside the quoted URL must not turn it into a comment.
            CHECK(formats.size() == 1);
            if (!formats.isEmpty()) CHECK(formats.first().start == 6 && formats.first().length == 31);
        }
    }
    CHECK(highlighted);
    // Selection remains stable while streaming, including on the final delta.
    QTextCursor selection(view.document()); selection.setPosition(0); selection.setPosition(6, QTextCursor::KeepAnchor);
    view.setTextCursor(selection);
    view.setMarkdownText(source + "\nLAST_DELTA");
    CHECK(view.textCursor().selectedText() == "Deploy"); CHECK(!view.toPlainText().contains("LAST_DELTA"));
    selection.clearSelection(); view.setTextCursor(selection); drain();
    CHECK(view.toPlainText().contains("LAST_DELTA"));
    // A short fence inside a longer fence is literal, and tilde fences work.
    view.setMarkdownText("````markdown\n```sh\nprintf hi\n```\n````\n\n~~~sh\nprintf bye\n~~~"); drain();
    copies = view.findChildren<QToolButton *>("aiCopyCode"); CHECK(copies.size() == 2);
    bool literalFence = false;
    for (auto *copy : copies) { copy->click(); literalFence |= QApplication::clipboard()->text() == "```sh\nprintf hi\n```"; }
    CHECK(literalFence);
    // Adjacent fences of the same language must retain separate copy actions.
    view.setMarkdownText("```sh\nprintf first\n```\n\n```sh\nprintf second\n```"); drain();
    copies = view.findChildren<QToolButton *>("aiCopyCode"); CHECK(copies.size() == 2);
    QStringList separateCode;
    for (auto *copy : copies) { copy->click(); separateCode << QApplication::clipboard()->text(); }
    CHECK(separateCode.contains("printf first")); CHECK(separateCode.contains("printf second"));
    // Unfinished streaming code is still readable and copyable.
    view.setMarkdownText("```sh\nprintf '<tag>'\n**literal**"); drain();
    copies = view.findChildren<QToolButton *>("aiCopyCode"); CHECK(copies.size() == 1);
    if (!copies.isEmpty()) { copies.first()->click(); CHECK(QApplication::clipboard()->text() == "printf '<tag>'\n**literal**"); }
    view.setMarkdownText("[local](file:///etc/passwd) [run](javascript:alert(1))\n\n<img src='file:///etc/passwd'>");
    for (auto block = view.document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) CHECK(!it.fragment().charFormat().isAnchor());
    // Long tokens wrap within the document rather than clipping at panel edges.
    view.setMarkdownText("```bash\nprintf " + QString(300, 'x') + "\n```\n\n|Header|Value|\n|---|---|\n|Key|" + QString(150, 'y') + "|");
    view.resize(300, view.heightForWidth(300)); drain();
    CHECK(view.document()->size().width() <= 300);
    for (auto *copy : view.findChildren<QToolButton *>("aiCopyCode")) CHECK(copy->geometry().right() <= view.viewport()->width());
}

static void testActivities()
{
    AiConversationModel model;
    AiConversationView view; view.resize(380, 700); view.setModel(&model); view.show();
    const auto turn = model.beginTurn("typed fixture", "fixture");
    AiActivity tool; tool.callId = "tool-1"; tool.name = "execute_ssh_commands";
    tool.summary = "检查服务"; tool.input = "{\"commands\": []}";
    const auto toolId = model.beginActivity(tool);
    CHECK(toolId != 0); CHECK(model.beginActivity(tool) == toolId);
    AiCommand cmd; cmd.cmd = "printf hello"; cmd.toolCallId = tool.callId;
    const auto commandId = model.addCommand(cmd);
    CHECK(model.currentTurn()->entries.last().parentEntryId == toolId);
    model.setCommandState(commandId, AiStepState::Running);
    CHECK(model.currentTurn()->entries.first().state == AiStepState::Running);
    AiCommandResult result; result.stepId = commandId; result.exitCode = 0; result.durationMs = 127;
    model.setCommandResult(result);
    CHECK(model.currentTurn()->entries.first().state == AiStepState::Succeeded);
    model.updateActivity(toolId, AiStepState::Succeeded, "TOOL_RESULT", 127);
    AiActivity agent; agent.kind = AiActivityKind::SubAgent; agent.callId = "agent-call";
    agent.name = "Explore"; agent.agent = "config-reader"; agent.summary = "检查配置"; agent.input = "只读检查 nginx"; agent.inputLanguage = "text";
    const auto agentId = model.beginActivity(agent, AiStepState::Running);
    AiActivity message; message.kind = AiActivityKind::AgentMessage; message.callId = "message-call";
    message.parentCallId = agent.callId; message.name = "SendMessage"; message.agent = "main"; message.recipient = "config-reader";
    message.input = "请核对监听端口"; message.inputLanguage = "text";
    model.beginActivity(message, AiStepState::Succeeded);
    CHECK(model.currentTurn()->entries.last().parentEntryId == agentId);
    AiActivity mcp; mcp.kind = AiActivityKind::Mcp; mcp.callId = "mcp-call"; mcp.server = "ops"; mcp.name = "inspect_service";
    mcp.input = "{\"service\":\"nginx\"}";
    const auto mcpId = model.beginActivity(mcp, AiStepState::Running);
    model.updateActivity(mcpId, AiStepState::Failed, "MCP_FAILURE", 35);
    AiActivity skill; skill.kind = AiActivityKind::Skill; skill.callId = "skill-call"; skill.name = "nginx-check";
    skill.summary = "加载检查步骤"; skill.inputLanguage = "text"; skill.input = "SKILL_INPUT";
    const auto skillId = model.beginActivity(skill, AiStepState::Succeeded);
    model.addNotice("只有提示、没有助手正文的记录也应显示");
    view.flush(); drain();
    for (auto id : {toolId, agentId, mcpId, skillId}) {
        auto *toggle = view.findChild<QToolButton *>(QStringLiteral("aiActivityToggle_%1").arg(id));
        CHECK(toggle && toggle->isVisible());
        if (toggle) toggle->click();
    }
    view.flush(); drain();
    bool output = false, error = false, input = false;
    for (auto *editor : view.findChildren<QPlainTextEdit *>()) {
        if (!editor->isVisible()) continue;
        output |= editor->toPlainText() == "TOOL_RESULT";
        error |= editor->toPlainText() == "MCP_FAILURE";
        input |= editor->toPlainText() == "SKILL_INPUT";
    }
    CHECK(output); CHECK(error); CHECK(input);
    auto *stats = view.findChild<QLabel *>(QStringLiteral("aiProcessStats_%1").arg(turn));
    CHECK(stats && stats->text().contains("MCP") && stats->text().contains("Skill") && stats->text().contains("Agent"));
    auto *copy = view.findChild<QToolButton *>(QStringLiteral("aiCopyEntry_%1").arg(commandId));
    CHECK(copy); if (copy) { copy->click(); CHECK(QApplication::clipboard()->text() == "printf hello"); }
    const auto previewDir = qEnvironmentVariable("CUBESHELL_AI_PREVIEW_DIR");
    if (!previewDir.isEmpty()) {
        CHECK(QDir().mkpath(previewDir));
        view.resize(620, 1900); drain(); view.verticalScrollBar()->setValue(0);
        CHECK(view.grab().save(previewDir + "/ai-activities.png"));
    }
    model.finishTurn(AiTurnOutcome::Cancelled, "Stopped");
    model.updateActivity(agentId, AiStepState::Succeeded, "LATE_RESULT");
    CHECK(model.currentTurn()->entries.at(2).state == AiStepState::Cancelled);
    model.beginTurn("new turn", "fixture");
    CHECK(model.beginActivity(tool) != toolId);
    AiConversationModel noticeOnly; view.setModel(&noticeOnly);
    const auto noticeTurn = noticeOnly.beginTurn("notice", "fixture");
    noticeOnly.addNotice("NOTICE_ONLY"); view.flush(); drain();
    auto *process = view.findChild<QToolButton *>(QStringLiteral("aiProcessToggle_%1").arg(noticeTurn));
    CHECK(process && process->isVisible());
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
    auto *plainCopy = view.findChild<QAction *>(QStringLiteral("aiCopyPlainText_%1").arg(turn));
    CHECK(plainCopy); if (plainCopy) {
        plainCopy->trigger();
        CHECK(QApplication::clipboard()->text().contains("FINAL_MARKER"));
        CHECK(!QApplication::clipboard()->text().contains("|---|---|"));
    }
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
    model.appendDelta(active, "\n\n## Responsive Markdown\n\n> A longer quoted note that wraps on narrow panels.\n\n"
                              "```bash\nprintf 'a long command that must wrap rather than clipping in a narrow panel'\n```\n\n"
                              "|Name|Result|\n|---|---|\n|fixture|complete|", {});
    view.flush(); drain();
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
                for (auto *browser : view.findChildren<QTextBrowser *>()) {
                    if (dynamic_cast<MarkdownView *>(browser) && browser->isVisible())
                        CHECK(browser->document()->size().height() <= browser->viewport()->height() + 2);
                }
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
            const QString report = QStringLiteral("## 检查结果\n\n**Nginx 当前未运行，80 端口没有监听。**\n\n"
                "| 检查项 | 状态 |\n|---|---|\n| 配置语法 | 检查通过 |\n| 服务进程 | `inactive (dead)` |\n| 80 端口 | 未监听 |\n\n"
                "> 本次仅做只读检查，没有修改服务器配置。\n\n"
                "### 建议下一步\n\n确认服务是否需要启动；启动前可先检查最近的服务日志。\n\n"
                "```bash\n# 查看最近的启动日志\njournalctl -u nginx --since \"30 min ago\"\nsystemctl status nginx --no-pager\n```\n\n"
                "- [x] 完成配置检查\n- [ ] 确认异常退出原因\n\n参考 [Nginx 文档](https://nginx.org/en/docs/)。");
            const auto finalMessage = model.beginMessage(); model.finalizeMessage(finalMessage, report, true);
            model.finishTurn(AiTurnOutcome::Complete, report); view.flush(); drain();
            const int fullHeight = qMin(2400, view.widget()->sizeHint().height() + 20);
            view.resize(width, fullHeight); drain();
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
    testMarkdown(); testRichMarkdown(); testActivities(); testTranscript(); testScrollingAndThemes(); testTerminalCompletion(); exportPreviews();
    qInfo() << "AI UI/terminal failures:" << failures;
    return failures ? 1 : 0;
}
