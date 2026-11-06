// 离屏 Skills 页面回归：临时配置、真实异步后端、不调用 CLI / 真 SSH。
#include "claude_code/ClaudeCodePanel.h"
#include "claude_code/ClaudeCodeBackend.h"
#include "claude_code/SkillsPanel.h"
#include "claude_code/SkillEditDialog.h"
#include "ssh/CommandExecutor.h"
#include "util/ThemeManager.h"
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <functional>

using namespace cubeshell;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { qWarning() << "FAIL:" << #c << "line" << __LINE__; ++failures; } } while (0)
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
    for (int i = 0; i < 12; ++i) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2); }
}
static void put(const QString &path, const QByteArray &bytes)
{
    CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile f(path); CHECK(f.open(QIODevice::WriteOnly)); CHECK(f.write(bytes) == bytes.size());
}
static QByteArray get(const QString &path)
{
    QFile f(path); CHECK(f.open(QIODevice::ReadOnly)); return f.readAll();
}
static void confirmNext()
{
    QTimer::singleShot(0, [] {
        for (auto *w : QApplication::topLevelWidgets())
            if (auto *box = qobject_cast<QMessageBox *>(w))
                if (auto *yes = box->button(QMessageBox::Yes)) yes->click();
    });
}
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const auto home = temp.path() + "/config";
    qputenv("CLAUDE_CONFIG_DIR", home.toUtf8());
    const auto original = QByteArray("---\r\nname: Alpha\r\ndescription: 中文搜索关键词\r\ncustom: keep-me\r\n---\r\n\r\nOriginal body\r\n");
    put(home + "/skills/alpha/SKILL.md", original);
    put(home + "/skills/alpha/references/data.md", "references");
    put(home + "/skills/zeta/SKILL.md", "---\nname: Zeta\ndescription: secondary\n---\nzeta body\n");
    put(home + "/skills-disabled/disabled/SKILL.md", "# disabled\n");
    ThemeManager::applyTheme(&app, ThemeManager::Appearance::Dark);

    ClaudeCodePanel container;
    auto *panel = container.findChild<SkillsPanel *>();
    CHECK(panel);
    if (!panel) return 1;
    for (auto *tabs : container.findChildren<QTabWidget *>())
        if (tabs->indexOf(panel) >= 0) tabs->setCurrentWidget(panel);
    container.resize(1200, 850);
    container.show();
    auto *tree = panel->findChild<QTreeWidget *>("skillList");
    auto *search = panel->findChild<QLineEdit *>("skillSearch");
    auto *scope = panel->findChild<QComboBox *>("skillScope");
    auto *project = panel->findChild<QLineEdit *>("skillProject");
    auto *preview = panel->findChild<QPlainTextEdit *>("skillPreview");
    auto *edit = panel->findChild<QPushButton *>("skillEdit");
    auto *add = panel->findChild<QPushButton *>("skillAdd");
    auto *importButton = panel->findChild<QPushButton *>("skillImport");
    auto *toggle = panel->findChild<QPushButton *>("skillToggle");
    auto *remove = panel->findChild<QPushButton *>("skillDelete");
    auto *status = panel->findChild<QLabel *>("skillStatus");
    CHECK(tree && search && scope && project && preview && edit && add && importButton && toggle && remove && status);
    if (!tree || !search || !scope || !project || !preview || !edit || !add || !importButton || !toggle || !remove || !status) return 1;
    CHECK(waitFor([&] { return tree->topLevelItemCount() == 3 && add->isEnabled(); }));
    // 隐藏导入箭头不改变菜单入口，两种导入方式仍可用。
    auto *importMenu = importButton->menu();
    CHECK(importMenu);
    if (importMenu) {
        CHECK(importMenu->actions().size() == 2);
        bool opened = false;
        const auto connection = QObject::connect(importMenu, &QMenu::aboutToShow, importMenu, [&] {
            opened = true;
            QTimer::singleShot(0, importMenu, &QMenu::close);
        });
        importButton->click();
        CHECK(opened);
        QObject::disconnect(connection);
        settle();
    }
    CHECK(!edit->isEnabled());
    CHECK(!project->isVisible());
    search->setText(QStringLiteral("中文搜索"));
    CHECK(!tree->topLevelItem(0)->isHidden());
    int visible = 0;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) visible += !tree->topLevelItem(i)->isHidden();
    CHECK(visible == 1);
    tree->setCurrentItem(tree->topLevelItem(0));
    CHECK(waitFor([&] { return edit->isEnabled(); }));
    CHECK(preview->toPlainText().contains("Original body"));
    search->setText("no-match");
    CHECK(!edit->isEnabled()); CHECK(preview->toPlainText().isEmpty());
    search->clear();
    // 排序后仍按稳定身份读取/修改，而非按之前的行号。
    tree->sortByColumn(0, Qt::DescendingOrder);
    QTreeWidgetItem *alpha = nullptr;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->data(0, Qt::UserRole).value<ClaudeSkillInfo>().directory == "alpha") alpha = tree->topLevelItem(i);
    CHECK(alpha); tree->setCurrentItem(alpha);
    CHECK(waitFor([&] { return edit->isEnabled(); }));
    edit->click(); settle();
    auto *dialog = panel->findChild<SkillEditDialog *>();
    CHECK(dialog);
    if (dialog) {
        auto *text = dialog->findChild<QPlainTextEdit *>("skillDocument");
        auto *save = dialog->findChild<QPushButton *>("skillSave");
        CHECK(text && save);
        text->appendPlainText("Added instructions");
        save->click();
        CHECK(waitFor([&] { return get(home + "/skills/alpha/SKILL.md").contains("Added instructions") && edit->isEnabled(); }));
        CHECK(get(home + "/skills/alpha/SKILL.md").contains("custom: keep-me\r\n"));
        CHECK(get(home + "/skills/zeta/SKILL.md").contains("zeta body"));
    }
    // 冲突不能覆盖外部修改，失败后原编辑文本保留。
    edit->click(); settle();
    dialog = panel->findChild<SkillEditDialog *>();
    CHECK(dialog);
    if (dialog) {
        auto *text = dialog->findChild<QPlainTextEdit *>("skillDocument");
        auto *save = dialog->findChild<QPushButton *>("skillSave");
        text->appendPlainText("UNSAVED-DRAFT");
        put(home + "/skills/alpha/SKILL.md", "# External change\n");
        save->click();
        CHECK(waitFor([&] { return dialog->findChild<QLabel *>("skillEditError")->text().contains(QStringLiteral("外部修改")); }));
        CHECK(dialog->isVisible()); CHECK(text->toPlainText().contains("UNSAVED-DRAFT"));
        CHECK(get(home + "/skills/alpha/SKILL.md") == "# External change\n");
        dialog->done(QDialog::Rejected); settle();
    }
    panel->refresh(); CHECK(waitFor([&] { return edit->isEnabled(); }));
    confirmNext(); toggle->click();
    CHECK(waitFor([&] { return QFileInfo::exists(home + "/skills-disabled/alpha/SKILL.md") && toggle->text() == QStringLiteral("启用") && toggle->isEnabled(); }));
    toggle->click();
    CHECK(waitFor([&] { return QFileInfo::exists(home + "/skills/alpha/SKILL.md") && toggle->text() == QStringLiteral("停用") && toggle->isEnabled(); }));

    // 新建使用真实异步保存接口，并选择新建的条目。
    add->click(); settle();
    dialog = panel->findChild<SkillEditDialog *>(); CHECK(dialog);
    if (dialog) {
        dialog->findChild<QLineEdit *>("skillDirectory")->setText("new-skill");
        dialog->findChild<QPushButton *>("skillSave")->click();
        CHECK(waitFor([&] { return QFileInfo::exists(home + "/skills/new-skill/SKILL.md") && edit->isEnabled(); }));
        CHECK(!tree->selectedItems().isEmpty());
        if (!tree->selectedItems().isEmpty()) CHECK(tree->selectedItems().first()->data(0, Qt::UserRole).value<ClaudeSkillInfo>().directory == "new-skill");
    }
    confirmNext(); remove->click();
    CHECK(waitFor([&] { return !QFileInfo::exists(home + "/skills/new-skill") && add->isEnabled(); }));
    CHECK(QFileInfo::exists(home + "/skills/alpha/SKILL.md"));

    // 项目路径变更立即清除旧数据，空项目路径不能落入本机当前工作目录。
    scope->setCurrentIndex(1);
    CHECK(tree->topLevelItemCount() == 0); CHECK(!add->isEnabled()); CHECK(project->isVisible());
    const auto projectPath = temp.path() + "/workspace";
    put(projectPath + "/.claude/skills/project-only/SKILL.md", "# PROJECT-ONLY\n");
    project->setText(projectPath); panel->refresh();
    CHECK(waitFor([&] { return tree->topLevelItemCount() == 1 && add->isEnabled(); }));
    tree->setCurrentItem(tree->topLevelItem(0));
    CHECK(waitFor([&] { return preview->toPlainText().contains("PROJECT-ONLY"); }));
    CHECK(!preview->toPlainText().contains("External change"));
    project->setText(temp.path() + "/does-not-exist"); panel->refresh();
    CHECK(waitFor([&] { return status->text().contains(QStringLiteral("不存在")); }));
    CHECK(!add->isEnabled()); CHECK(tree->topLevelItemCount() == 0);

    // 原来源的迟到列表/详情不能填入新来源；失联远程不会回落读取本地 Skills。
    scope->setCurrentIndex(0);
    CHECK(waitFor([&] { return tree->topLevelItemCount() == 3 && add->isEnabled(); }));
    auto *backend = container.backend();
    const auto oldSource = backend->sessionSource();
    quint64 lastRequest = 0;
    QObject::connect(backend, &ClaudeCodeBackend::skillsListed, panel,
        [&](quint64 source, quint64 request, const auto &, const auto &, const auto &) { if (source == oldSource) lastRequest = request; }, Qt::QueuedConnection);
    panel->refresh(); CHECK(waitFor([&] { return lastRequest != 0; }));
    CommandExecutor disconnected(nullptr);
    backend->setRemoteExecutor(&disconnected);
    CHECK(tree->topLevelItemCount() == 0); CHECK(preview->toPlainText().isEmpty());
    ClaudeSkillInfo stale; stale.directory = "stale"; stale.name = "STALE";
    backend->skillsListed(oldSource, lastRequest, {stale}, home, {});
    panel->refresh();
    CHECK(waitFor([&] { return status->text().contains(QStringLiteral("SSH 连接不可用")); }));
    CHECK(tree->topLevelItemCount() == 0); CHECK(!add->isEnabled());
    backend->setRemoteExecutor(nullptr); panel->refresh();
    CHECK(waitFor([&] { return tree->topLevelItemCount() == 3 && add->isEnabled(); }));

    tree->setCurrentItem(tree->topLevelItem(0)); CHECK(waitFor([&] { return edit->isEnabled(); }));
    edit->click(); settle(); dialog = panel->findChild<SkillEditDialog *>(); CHECK(dialog);
    if (dialog) {
        dialog->findChild<QPlainTextEdit *>("skillDocument")->appendPlainText("OLD SOURCE DRAFT");
        backend->setRemoteExecutor(&disconnected);
        CHECK(!dialog->findChild<QPushButton *>("skillSave")->isEnabled());
        CHECK(dialog->findChild<QPlainTextEdit *>("skillDocument")->toPlainText().contains("OLD SOURCE DRAFT"));
        dialog->done(QDialog::Rejected); settle();
    }
    backend->setRemoteExecutor(nullptr); panel->refresh();
    CHECK(waitFor([&] { return add->isEnabled(); }));
    const auto originalFont = panel->font();
    for (auto theme : {ThemeManager::Appearance::Dark, ThemeManager::Appearance::Light}) {
        ThemeManager::applyTheme(&app, theme);
        for (double scale : {1.0, 1.5, 1.0}) {
            auto font = originalFont;
            if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * scale));
            else font.setPointSizeF(font.pointSizeF() * scale);
            panel->setFont(font);
            for (int width : {800, 1200}) {
                container.resize(width, 850);
                for (int index : {0, 1}) {
                    scope->setCurrentIndex(index); settle();
                    QStyleOptionComboBox option;
                    option.initFrom(scope);
                    option.currentText = scope->currentText();
                    const auto textRect = scope->style()->subControlRect(
                        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, scope);
                    for (int item = 0; item < scope->count(); ++item)
                        CHECK(textRect.width() >= scope->fontMetrics().horizontalAdvance(scope->itemText(item))
                            + scope->fontMetrics().horizontalAdvance(QLatin1Char('M')));
                    QStyleOptionButton buttonOption;
                    buttonOption.initFrom(importButton);
                    buttonOption.features = QStyleOptionButton::HasMenu;
                    CHECK(importButton->style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &buttonOption, importButton) == 0);
                    CHECK(search->width() > 100); CHECK(add->height() >= add->fontMetrics().height());
                    CHECK(tree->isVisible()); CHECK(tree->viewport()->width() > 100);
                }
            }
        }
    }
    qInfo() << "claude_skill_ui_test failures:" << failures;
    return failures ? 1 : 0;
}
