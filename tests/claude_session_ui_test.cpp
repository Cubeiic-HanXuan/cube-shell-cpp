// claude_session_ui_test.cpp — 原生会话界面的离屏交互与布局回归。
// 合成历史写入临时目录，不执行 Claude 或 SSH；可选导出控件渲染图供检查，不截取桌面。
#include "claude_code/ClaudeCodeBackend.h"
#include "claude_code/ClaudeCodePanel.h"
#include "claude_code/SessionPanel.h"
#include "claude_code/SessionDetailPanel.h"
#include "claude_code/SessionTimelineView.h"
#include "ssh/CommandExecutor.h"
#include "util/ThemeManager.h"
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QKeyEvent>
#include <QStyleOptionComboBox>
#include <QStyle>
#include <QTextCursor>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>
#include <QDebug>
#include <functional>

using namespace cubeshell;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { qWarning() << "FAIL:" << #condition << "line" << __LINE__; ++failures; } } while (0)
static bool waitFor(const std::function<bool()> &predicate)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 8000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2);
    }
    return predicate();
}
static void settle()
{
    for (int i = 0; i < 8; ++i) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2); }
}
static QByteArray record(const QString &type, const QJsonValue &content, const QString &id, int minute = 0)
{
    return QJsonDocument(QJsonObject{{"type", type}, {"uuid", id}, {"cwd", "/workspace/cube-shell/services/archive"},
        {"timestamp", QDateTime::fromString("2026-11-05T09:08:00Z", Qt::ISODate).addSecs(minute * 60).toString(Qt::ISODate)},
        {"message", QJsonObject{{"content", content}, {"model", "Claude Code"}}}})
        .toJson(QJsonDocument::Compact) + '\n';
}
static QString sourceCode()
{
    return QStringLiteral(
        "package com.example.archive.service;\n\n"
        "import java.util.List;\n"
        "import org.springframework.stereotype.Service;\n\n"
        "@Service\n"
        "public class ArchiveQueryService {\n"
        "    private final ArchiveRepository repository;\n\n"
        "    public Page<Archive> search(Query query) {\n"
        "        // Keep the filters and response contract unchanged.\n"
        "        final int limit = query.pageSize();\n"
        "        List<Long> ids = repository.findPageIds(query, limit);\n"
        "        return repository.loadDetails(ids);\n"
        "    }\n"
        "}\n"
        "// end-of-synthetic-source");
}
static QByteArray scene()
{
    const QStringList questions{
        QStringLiteral("请优化归档查询接口。当前查询约需 2.5 秒，希望能缩短响应时间，同时保留分页、过滤条件和返回字段。先定位瓶颈，再做有依据的修改。"),
        QStringLiteral("请重点检查 SQL 执行计划，不要改变业务语义。把你发现的问题和验证方式列出来。"),
        QStringLiteral("这个索引是否会影响其他查询？请补充空结果、大数据量和边界分页的回归测试。"),
        QStringLiteral("把查询拆成两步之后，排序和分页还能保持一致吗？请验证一下。"),
        QStringLiteral("继续检查并发查询场景，确认没有引入共享状态和线程安全问题。"),
        QStringLiteral("整理本轮修改，说明为什么这样改，以及回滚时需要注意的事项。"),
        QStringLiteral("请再看一下执行计划，对比优化前后的扫描行数和耗时。"),
        QStringLiteral("补充一份简短的验收清单，方便团队在测试环境中复核。"),
        QStringLiteral("最后总结一下。本次改动有哪些明确收益，还有哪些需要在真实环境验证？")};
    QByteArray bytes;
    for (int i = 0; i < questions.size(); ++i) {
        const auto suffix = QString::number(i);
        bytes += record("user", questions[i], "user-" + suffix, i * 12);
        bytes += record("assistant", QStringLiteral("我会先确认现有查询链路和数据约束，再检查执行计划。所有改动都以返回结果一致为前提，并用回归测试验证。"), "comment-" + suffix, i * 12 + 1);
        bytes += record("assistant", QJsonArray{
            QJsonObject{{"type", "tool_use"}, {"id", "read-" + suffix}, {"name", "Read"},
                {"input", QJsonObject{{"file_path", "/workspace/services/ArchiveQueryService.java"}}}},
            QJsonObject{{"type", "tool_use"}, {"id", "bash-" + suffix}, {"name", "Bash"},
                {"input", QJsonObject{{"command", "./gradlew test --tests ArchiveQueryServiceTest"}, {"description", "运行查询回归测试"}}}},
            QJsonObject{{"type", "tool_use"}, {"id", "edit-" + suffix}, {"name", "Edit"},
                {"input", QJsonObject{{"file_path", "/workspace/services/ArchiveRepository.java"},
                    {"old_string", "return findAll(query);"}, {"new_string", "return findPageIds(query, limit);"}}}}},
            "tools-" + suffix, i * 12 + 2);
        bytes += record("user", QJsonArray{
            QJsonObject{{"type", "tool_result"}, {"tool_use_id", "read-" + suffix}, {"content", sourceCode()}},
            QJsonObject{{"type", "tool_result"}, {"tool_use_id", "bash-" + suffix}, {"is_error", i == 0},
                {"content", i == 0 ? "SYNTHETIC-FAILURE: test fixture needs initialization\nExit code 1" : "BUILD SUCCESSFUL\n12 tests passed"}},
            QJsonObject{{"type", "tool_result"}, {"tool_use_id", "edit-" + suffix}, {"content", "File updated successfully"}}},
            "results-" + suffix, i * 12 + 3);
        bytes += record("assistant", QJsonArray{QJsonObject{{"type", "thinking"}, {"thinking", ""}}}, "thinking-" + suffix, i * 12 + 4);
        if (i != 1) {
            QString reply = QStringLiteral("**本轮检查结果**\n\n已确认耗时主要集中在关联查询的扫描范围。修改限定在数据访问层，分页字段和返回结构保持不变。\n\n- 缩小主表扫描范围，保留原有排序规则。\n- 补充空结果与分页边界验证。\n- 真实数据库上的收益仍需要进一步测量。");
            if (i == 8) reply += QStringLiteral("\n\nUNIQUE-END-OF-SESSION");
            bytes += record("assistant", reply, "reply-" + suffix, i * 12 + 5);
        }
    }
    bytes += QJsonDocument(QJsonObject{{"type", "custom-title"}, {"customTitle", "优化归档查询链路，保持行为一致并补齐回归验证"}}).toJson(QJsonDocument::Compact) + '\n';
    return bytes;
}
static QString renderedText(QWidget *parent)
{
    QString text;
    for (auto *browser : parent->findChildren<QTextBrowser *>()) text += browser->toPlainText();
    for (auto *editor : parent->findChildren<QPlainTextEdit *>()) text += editor->toPlainText();
    for (auto *label : parent->findChildren<QLabel *>()) text += label->text();
    return text;
}
static int toolIndex(const ClaudeSessionData &data, const QString &id)
{
    for (int i = 0; i < data.entries().size(); ++i) if (data.entries()[i].toolId == id) return i;
    return -1;
}
// 与真实应用调色板比较，防止列表 / 详情各自写死底色；同时覆盖已隐藏的页面控件。
static void checkSessionBackground(SessionPanel &panel)
{
    const auto background = QApplication::palette().color(QPalette::Window);
    CHECK(sessionui::colors(QApplication::palette()).canvas == background);
    for (const auto *name : {"sessionListPage", "sessionTree", "sessionDetail", "sessionHeader", "sessionFooter"}) {
        auto *widget = panel.findChild<QWidget *>(QString::fromLatin1(name));
        CHECK(widget != nullptr);
        if (widget) CHECK(widget->palette().color(QPalette::Window) == background);
    }
    auto *timeline = panel.findChild<SessionTimelineView *>("sessionTimeline");
    CHECK(timeline != nullptr);
    if (timeline) CHECK(timeline->viewport()->palette().color(QPalette::Window) == background);
}
// 使用应用的真实主题资源，组合深浅主题、1 / 1.5 倍字体及宽窄窗口检查下拉框与双行行高。
static void groupingGeometryTest(SessionPanel &panel, QComboBox *combo, QLineEdit *search)
{
    const auto originalFont = panel.font();
    const auto originalSize = panel.size();
    auto *tree = panel.findChild<QTreeWidget *>("sessionTree");
    CHECK(tree != nullptr);
    if (!tree) return;
    for (auto theme : {ThemeManager::Appearance::Dark, ThemeManager::Appearance::Light}) {
        ThemeManager::applyTheme(qApp, theme);
        for (double scale : {1.0, 1.5}) {
            auto font = originalFont;
            if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * scale));
            else font.setPointSizeF(font.pointSizeF() * scale);
            panel.setFont(font);
            for (int width : {1480, 900}) {
                panel.resize(width, 940); settle();
                CHECK(panel.width() <= width);
                CHECK(search->width() > 100);
                for (int i = 0; i < combo->count(); ++i) {
                    combo->setCurrentIndex(i); settle();
                    auto *group = tree->topLevelItem(0);
                    CHECK(group != nullptr);
                    if (group) {
                        const bool expanded = group->isExpanded();
                        group->setExpanded(true); settle();
                        const int rowHeight = qMax(52, tree->fontMetrics().lineSpacing() * 2 + 18);
                        CHECK(tree->visualItemRect(group).height() == rowHeight);
                        CHECK(tree->visualItemRect(group->child(0)).height() == rowHeight);
                        CHECK(rowHeight >= tree->fontMetrics().lineSpacing() * 2 + 17);
                        group->setExpanded(expanded);
                    }
                    QStyleOptionComboBox option; option.initFrom(combo);
                    option.currentText = combo->currentText();
                    const auto field = combo->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, combo);
                    const auto arrow = combo->style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, combo);
                    const auto fm = combo->fontMetrics();
                    CHECK(field.width() >= fm.horizontalAdvance(combo->currentText()) + fm.horizontalAdvance(' '));
                    CHECK(field.height() >= fm.height());
                    CHECK(field.right() < arrow.left());
                    CHECK(combo->height() == search->height());
                    combo->showPopup(); settle();
                    const auto item = combo->view()->visualRect(combo->model()->index(i, 0));
                    CHECK(item.width() >= fm.horizontalAdvance(combo->itemText(i)) + 20);
                    CHECK(item.height() >= fm.height());
                    combo->hidePopup(); settle();
                }
            }
        }
        panel.setFont(originalFont); panel.resize(900, 760); settle();
        checkSessionBackground(panel);
        const auto artifacts = qEnvironmentVariable("CUBE_SESSION_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) CHECK(panel.grab().save(artifacts + (theme == ThemeManager::Appearance::Dark
            ? "/native-session-list-compact.png" : "/native-session-list-light.png")));
    }
    ThemeManager::applyTheme(qApp, ThemeManager::Appearance::Dark);
    panel.setFont(originalFont); panel.resize(originalSize); combo->setCurrentIndex(0); settle();
}
static void pressSpace(QWidget *widget)
{
    widget->setFocus(Qt::TabFocusReason);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(widget, &press); QApplication::sendEvent(widget, &release);
    settle();
}
// 用键盘触发展开按钮，检查重建后的焦点 / 展开态；搜索必须能进入隐藏上下文和代码块。
static void disclosureAndSearchTest()
{
    CHECK(sessionui::messagePreview("**标题** 与 `代码` [链接](https://example.invalid)") == QStringLiteral("标题 与 代码 链接"));
    CHECK(sessionui::messagePreview("<img src=\"file:///not-opened\">").contains("<img"));
    auto data = std::make_shared<ClaudeSessionData>();
    data->append(record("user", "<system-reminder>HIDDEN-CONTEXT</system-reminder>\n请修复问题", "context-user"));
    data->append(record("assistant", QJsonArray{QJsonObject{{"type", "tool_use"}, {"id", "edit"}, {"name", "Edit"},
        {"input", QJsonObject{{"file_path", "/tmp/synthetic.cpp"}, {"old_string", "before"}, {"new_string", "after"}}}}}, "edit-call"));
    data->append(record("user", QJsonArray{QJsonObject{{"type", "tool_result"}, {"tool_use_id", "edit"}, {"is_error", true}, {"content", "synthetic failure"}}}, "edit-result"));
    QString code;
    for (int i = 0; i < 30; ++i) code += QStringLiteral("// synthetic line %1\n").arg(i);
    code += QStringLiteral("// UNIQUE-FENCE-SEARCH\n");
    const auto reply = QStringLiteral("说明段落，保留定位前的上下文。\n\n").repeated(24)
        + QStringLiteral("```cpp\n") + code + QStringLiteral("```\n\n完成。\n\n```txt\nunchanged preview\n```");
    data->append(record("assistant", reply, "fenced-reply"), true);
    SessionTimelineView view; view.resize(1000, 780); view.show(); view.setSnapshot(data, true); settle();
    CHECK(!renderedText(&view).contains("HIDDEN-CONTEXT"));
    for (int cycle = 0; cycle < 2; ++cycle) {
        auto *raw = view.findChild<QToolButton *>("sessionRawToggle_0"); CHECK(raw != nullptr);
        if (raw) pressSpace(raw);
        CHECK(view.viewState().rawMessages.contains(0));
        raw = view.findChild<QToolButton *>("sessionRawToggle_0"); CHECK(raw && raw->isChecked() && raw->hasFocus());
        CHECK(renderedText(&view).contains("HIDDEN-CONTEXT"));
        if (raw) pressSpace(raw);
        CHECK(!view.viewState().rawMessages.contains(0));
        CHECK(!renderedText(&view).contains("HIDDEN-CONTEXT"));
    }
    const int edit = toolIndex(*data, "edit"); CHECK(edit >= 0);
    const auto toggleName = QStringLiteral("sessionStepToggle_%1").arg(edit);
    for (auto filter : {SessionTimelineView::Changes, SessionTimelineView::Failures}) {
        view.setFilter(filter); settle();
        auto *toggle = view.findChild<QToolButton *>(toggleName); CHECK(toggle && toggle->isChecked());
        CHECK(!view.findChildren<QPlainTextEdit *>().isEmpty());
        if (toggle) pressSpace(toggle);
        toggle = view.findChild<QToolButton *>(toggleName); CHECK(toggle && !toggle->isChecked() && toggle->hasFocus());
        CHECK(view.findChildren<QPlainTextEdit *>().isEmpty());
        view.setFilter(SessionTimelineView::All); view.setFilter(filter); settle();
        toggle = view.findChild<QToolButton *>(toggleName); CHECK(toggle && !toggle->isChecked());
        if (toggle) pressSpace(toggle);
        CHECK(!view.findChildren<QPlainTextEdit *>().isEmpty());
    }
    view.setFilter(SessionTimelineView::All);
    const int replyEntry = data->entries().size() - 1;
    view.scrollToEntry(replyEntry, "UNIQUE-FENCE-SEARCH"); settle();
    bool found = false;
    for (auto *editor : view.findChildren<QPlainTextEdit *>()) {
        if (!editor->toPlainText().contains("UNIQUE-FENCE-SEARCH")) continue;
        found = true;
        CHECK(editor->property("codeKey").toString().contains(":fence:"));
        CHECK(editor->textCursor().selectedText() == "UNIQUE-FENCE-SEARCH");
        const auto position = editor->viewport()->mapTo(view.viewport(), editor->cursorRect().center());
        CHECK(view.viewport()->rect().contains(position));
    }
    CHECK(found);
    CHECK(view.visibleWidgetCount() < 70);
    view.setFilter(SessionTimelineView::Changes); view.clear(); view.setSnapshot(data, true); settle();
    const auto *resetToggle = view.findChild<QToolButton *>(toggleName);
    CHECK(resetToggle && resetToggle->isChecked());
}
// 三千次工具调用仍只实例化视口附近的控件；跨区域滚动和折叠后保留逻辑展开状态。
static void virtualizationTest()
{
    auto data = std::make_shared<ClaudeSessionData>();
    data->append(record("user", "Large synthetic transcript", "start"));
    for (int i = 0; i < 3000; ++i) {
        const auto id = QString::number(i);
        data->append(record("assistant", QJsonArray{QJsonObject{{"type", "tool_use"}, {"id", "tool-" + id},
            {"name", "Read"}, {"input", QJsonObject{{"file_path", "/tmp/file-" + id}}}}}, "large-" + id));
    }
    data->append(record("assistant", "Large transcript final reply", "end"), true);
    SessionTimelineView view; view.resize(1050, 760); view.show();
    QElapsedTimer timer; timer.start(); view.setSnapshot(data, true); settle();
    CHECK(view.logicalRowCount() > 3000);
    CHECK(view.visibleWidgetCount() < 70);
    view.scrollToEntry(1500); settle();
    CHECK(view.visibleWidgetCount() < 70);
    auto *step = view.findChild<QToolButton *>("sessionStepToggle_1500");
    CHECK(step != nullptr);
    if (step) step->click();
    CHECK(waitFor([&] { return view.viewState().expandedEntries.contains(1500); }));
    CHECK(view.findChild<QToolButton *>("sessionStepToggle_1500") != nullptr);
    view.resize(750, 700); settle();
    CHECK(view.visibleWidgetCount() < 70);
    view.scrollToLatest(); settle();
    CHECK(view.visibleWidgetCount() < 70);
    CHECK(renderedText(&view).contains("Large transcript final reply"));
    view.scrollToEntry(1500); settle();
    CHECK(view.viewState().expandedEntries.contains(1500));
    view.setProcessesCollapsed(true); settle();
    CHECK(view.logicalRowCount() < 10);
    view.scrollToEntry(1500, "file-1499"); settle();
    CHECK(view.logicalRowCount() > 3000);
    CHECK(view.visibleWidgetCount() < 70);
    qInfo() << "Virtual timeline: 3000 tools," << view.visibleWidgetCount() << "live rows," << timer.elapsed() << "ms for navigation/resize/disclosure";
}
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ThemeManager::applyTheme(&app, ThemeManager::Appearance::Dark);
    QTemporaryDir temp; CHECK(temp.isValid()); qputenv("CLAUDE_CONFIG_DIR", temp.path().toUtf8());
    const auto dir = temp.path() + "/projects/-synthetic"; QDir().mkpath(dir);
    const QString sid = "01234567-89ab-4cde-8fab-0123456789ab";
    QFile file(dir + "/" + sid + ".jsonl"); CHECK(file.open(QIODevice::WriteOnly)); file.write(scene()); file.close();
    const QStringList titles{ "排查文件传输中断后的恢复逻辑", "为配置解析增加边界测试", "整理部署文档与回滚步骤", "检查终端快捷键在不同平台的一致性", "优化目录刷新与缓存失效策略" };
    for (int i = 0; i < titles.size(); ++i) {
        const auto id = QStringLiteral("11234567-89ab-4cde-8fab-%1").arg(i, 12, 16, QLatin1Char('0'));
        QFile extra(dir + "/" + id + ".jsonl"); CHECK(extra.open(QIODevice::WriteOnly));
        extra.write(record("user", titles[i], "title-" + id));
        extra.write(record("assistant", "已完成检查，并补充了针对边界条件的验证。", "answer-" + id));
    }
    ClaudeCodeBackend backend; SessionPanel panel(&backend); panel.resize(1480, 940); panel.show();
    auto *tree = panel.findChild<QTreeWidget *>("sessionTree");
    auto *search = panel.findChild<QLineEdit *>("sessionSearch");
    auto *detail = panel.findChild<SessionDetailPanel *>();
    auto *timeline = panel.findChild<SessionTimelineView *>("sessionTimeline");
    auto *directory = panel.findChild<QListWidget *>("sessionDirectory");
    auto *grouping = panel.findChild<QComboBox *>("sessionGrouping");
    auto *filter = panel.findChild<QButtonGroup *>("sessionDetailFilter");
    auto *back = panel.findChild<QToolButton *>("sessionBack");
    auto *resume = panel.findChild<QPushButton *>("sessionResume");
    CHECK(tree && search && detail && timeline && directory && grouping && filter && back && resume);
    if (!tree || !detail || !timeline) return 1;
    const auto artifactDir = qEnvironmentVariable("CUBE_SESSION_ARTIFACT_DIR");
    const auto saveArtifact = [&](const QString &name) {
        if (!artifactDir.isEmpty()) { QDir().mkpath(artifactDir); settle(); CHECK(panel.grab().save(artifactDir + "/" + name + ".png")); }
    };
    panel.refresh(); CHECK(waitFor([&] { return tree->topLevelItemCount() == 1; }));
    CHECK(tree->topLevelItem(0)->childCount() == 6);
    // 项目首次展示默认收起；切换分组后分别保留手动展开 / 收起的选择，时间分组沿用默认展开。
    CHECK(!tree->topLevelItem(0)->isExpanded());
    grouping->setCurrentIndex(1); CHECK(tree->topLevelItem(0)->isExpanded());
    grouping->setCurrentIndex(0); CHECK(!tree->topLevelItem(0)->isExpanded());
    tree->topLevelItem(0)->setExpanded(true);
    grouping->setCurrentIndex(1); grouping->setCurrentIndex(0);
    CHECK(tree->topLevelItem(0)->isExpanded());
    tree->topLevelItem(0)->setExpanded(false);
    grouping->setCurrentIndex(1); grouping->setCurrentIndex(0);
    CHECK(!tree->topLevelItem(0)->isExpanded());
    saveArtifact("native-session-list");
    groupingGeometryTest(panel, grouping, search);
    const quint64 source = backend.sessionSource(); backend.sessionListReady(source + 99, 1, {}, {}); settle();
    CHECK(tree->topLevelItemCount() == 1);
    search->setText("not-found"); CHECK(waitFor([&] { return tree->topLevelItemCount() == 0; }));
    search->setText("归档"); CHECK(waitFor([&] { return tree->topLevelItemCount() == 1; }));
    grouping->setCurrentIndex(1); CHECK(tree->topLevelItemCount() == 1); grouping->setCurrentIndex(0);
    CHECK(!tree->topLevelItem(0)->isExpanded());
    tree->topLevelItem(0)->setExpanded(true); settle();
    tree->itemClicked(tree->topLevelItem(0)->child(0), 0);
    CHECK(waitFor([&] { return detail->isVisible() && detail->data().turns().size() == 9; })); settle();
    CHECK(directory->count() == 9);
    CHECK(timeline->visibleWidgetCount() > 0 && timeline->visibleWidgetCount() < 70);
    const int read = toolIndex(detail->data(), "read-0");
    CHECK(read >= 0);
    auto *turnToggle = timeline->findChild<QToolButton *>("sessionTurnToggle_0"); CHECK(turnToggle != nullptr);
    if (turnToggle) turnToggle->click();
    CHECK(waitFor([&] { return timeline->viewState().collapsedTurns.contains(0); }));
    turnToggle = timeline->findChild<QToolButton *>("sessionTurnToggle_0");
    if (turnToggle) turnToggle->click();
    CHECK(waitFor([&] { return !timeline->viewState().collapsedTurns.contains(0); }));
    auto *readToggle = timeline->findChild<QToolButton *>(QStringLiteral("sessionStepToggle_%1").arg(read));
    CHECK(readToggle != nullptr); if (readToggle) readToggle->click();
    CHECK(waitFor([&] { return timeline->viewState().expandedEntries.contains(read); })); settle();
    CHECK(renderedText(timeline).contains("ArchiveQueryService"));
    saveArtifact("native-session-detail-dark");
    auto *copy = timeline->findChild<QToolButton *>(QStringLiteral("sessionCodeCopy_%1:output").arg(read));
    CHECK(copy != nullptr); if (copy) copy->click();
    CHECK(QApplication::clipboard()->text() == sourceCode());
    auto *expandCode = timeline->findChild<QToolButton *>(QStringLiteral("sessionCodeExpand_%1:output").arg(read));
    CHECK(expandCode != nullptr); if (expandCode) expandCode->click();
    CHECK(waitFor([&] { return timeline->viewState().expandedCode.contains(QStringLiteral("%1:output").arg(read)); }));
    CHECK(renderedText(timeline).contains("end-of-synthetic-source"));
    auto *params = timeline->findChild<QToolButton *>(QStringLiteral("sessionParameters_%1").arg(read));
    CHECK(params != nullptr); if (params) params->click();
    CHECK(waitFor([&] { return timeline->viewState().parameters.contains(read); }));
    CHECK(renderedText(timeline).contains("file_path"));
    // 宽窄布局与应用真实主题切换：图标更新时，键盘焦点和已展开步骤不能丢失。
    panel.resize(900, 760); settle(); CHECK(panel.width() <= 900); CHECK(!directory->isVisible());
    saveArtifact("native-session-detail-compact");
    const auto darkBack = back->icon().pixmap(18, 18).toImage();
    const auto expandedBeforeTheme = timeline->viewState().expandedEntries;
    QApplication::setActiveWindow(&panel);
    back->setFocus(Qt::TabFocusReason); settle();
    CHECK(back->hasFocus());
    ThemeManager::applyTheme(&app, ThemeManager::Appearance::Light); panel.resize(1360, 900); settle();
    checkSessionBackground(panel);
    CHECK(directory->isVisible());
    const auto light = sessionui::colors(QApplication::palette());
    CHECK(light.canvas.lightness() > 128);
    CHECK(timeline->viewport()->palette().color(QPalette::Window) == light.canvas);
    CHECK(detail->styleSheet().contains(sessionui::css(light.canvas)));
    CHECK(back->icon().pixmap(18, 18).toImage() != darkBack);
    CHECK(back->hasFocus());
    CHECK(timeline->viewState().expandedEntries == expandedBeforeTheme);
    for (auto *button : detail->findChildren<QToolButton *>()) {
        if (!button->property("sessionGlyph").isValid()) continue;
        const auto glyph = static_cast<sessionui::Glyph>(button->property("sessionGlyph").toInt());
        CHECK(button->icon().pixmap(18, 18).toImage() == sessionui::icon(glyph, light.muted).pixmap(18, 18).toImage());
    }
    saveArtifact("native-session-detail-light");
    ThemeManager::applyTheme(&app, ThemeManager::Appearance::Dark); panel.resize(1480, 940); settle();
    checkSessionBackground(panel);
    CHECK(back->icon().pixmap(18, 18).toImage() == darkBack);
    CHECK(timeline->viewState().expandedEntries == expandedBeforeTheme);
    filter->button(1)->click(); settle(); CHECK(timeline->filter() == SessionTimelineView::Conversation);
    CHECK(timeline->findChildren<QPlainTextEdit *>().isEmpty());
    filter->button(2)->click(); settle(); CHECK(timeline->filter() == SessionTimelineView::Changes);
    CHECK(renderedText(timeline).contains("return findAll(query);"));
    CHECK(renderedText(timeline).contains("return findPageIds(query, limit);"));
    auto *failuresButton = panel.findChild<QToolButton *>("sessionFailures"); failuresButton->click(); settle();
    CHECK(timeline->filter() == SessionTimelineView::Failures);
    CHECK(renderedText(timeline).contains("SYNTHETIC-FAILURE"));
    directory->itemClicked(directory->item(1)); settle(); CHECK(timeline->filter() == SessionTimelineView::All);
    CHECK(timeline->currentTurn() == 1);
    timeline->scrollToLatest(); settle(); CHECK(timeline->currentTurn() >= 7);
    CHECK(directory->currentRow() >= 7);
    detail->focusSearch(); auto *detailSearch = panel.findChild<QLineEdit *>("sessionDetailSearch");
    detailSearch->setText("SYNTHETIC-FAILURE"); detailSearch->returnPressed(); settle();
    CHECK(renderedText(timeline).contains("SYNTHETIC-FAILURE"));
    CHECK(timeline->viewState().parameters.contains(toolIndex(detail->data(), "bash-0")));
    detailSearch->setText("UNIQUE-END-OF-SESSION"); detailSearch->returnPressed(); settle();
    CHECK(renderedText(timeline).contains("UNIQUE-END-OF-SESSION")); CHECK(timeline->currentTurn() == 8);
    QString command;
    QObject::connect(&panel, &SessionPanel::openTerminalRequested, &panel, [&](const QString &value) { command = value; });
    resume->click(); CHECK(command.contains(sid)); CHECK(command.contains("claude --resume"));
    back->click(); CHECK(tree->isVisible()); CHECK(search->text() == QStringLiteral("归档"));
    tree->itemClicked(tree->topLevelItem(0)->child(0), 0); detail->reset();
    ClaudeSessionPage stale; stale.bytes = record("user", "STALE-MESSAGE", "stale"); stale.endOfFile = true;
    backend.sessionPageReady(source + 1, 1, stale, std::make_shared<ClaudeSessionData>()); settle();
    CHECK(detail->data().entries().isEmpty()); CHECK(!renderedText(timeline).contains("STALE-MESSAGE"));
    // 切换远程数据源后模拟旧来源的迟到结果，旧列表不能写回新来源界面。
    CommandExecutor executor(nullptr); const auto oldSessions = backend.listSessions();
    backend.setRemoteExecutor(&executor); CHECK(tree->topLevelItemCount() == 0);
    backend.sessionListReady(source, 1, oldSessions, {}); settle(); CHECK(tree->topLevelItemCount() == 0);
    CHECK(backend.sessionSourceIsRemote());
    ClaudeCodePanel container; container.addRemoteConnection("synthetic-remote", &executor);
    auto *mode = container.findChild<QComboBox *>(); auto *sessionsPanel = container.findChild<SessionPanel *>();
    CommandExecutor *routedSource = nullptr; int localRequests = 0;
    QObject::connect(&container, &ClaudeCodePanel::remoteSessionResumeRequested, &container,
        [&](const QString &, CommandExecutor *sourceExecutor) { routedSource = sourceExecutor; });
    QObject::connect(&container, &ClaudeCodePanel::openTerminalRequested, &container, [&](const QString &) { ++localRequests; });
    mode->setCurrentIndex(1); sessionsPanel->openTerminalRequested("synthetic-command-not-executed");
    CHECK(routedSource == &executor); CHECK(localRequests == 0);
    mode->setCurrentIndex(0); sessionsPanel->openTerminalRequested("synthetic-command-not-executed"); CHECK(localRequests == 1);
    disclosureAndSearchTest();
    virtualizationTest();
    qInfo() << "claude_session_ui_test failures:" << failures;
    return failures ? 1 : 0;
}
