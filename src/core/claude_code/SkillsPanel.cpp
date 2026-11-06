#include "claude_code/SkillsPanel.h"
#include "claude_code/ClaudeCodeBackend.h"
#include "claude_code/SkillEditDialog.h"

#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace cubeshell {
SkillsPanel::SkillsPanel(ClaudeCodeBackend *backend, QWidget *parent)
    : QWidget(parent), m_backend(backend), m_source(backend->sessionSource())
{
    setObjectName(QStringLiteral("skillsPanel"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    auto *location = new QHBoxLayout;
    location->addWidget(new QLabel(tr("作用域："), this));
    m_scope = new QComboBox(this);
    m_scope->setObjectName(QStringLiteral("skillScope"));
    m_scope->addItem(tr("用户全局"), QStringLiteral("user"));
    m_scope->addItem(tr("项目"), QStringLiteral("project"));
    m_scope->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    // 按字体预留文字与箭头空间，项目路径显示时也不压缩作用域。
    m_scope->setMinimumContentsLength(10);
    m_scope->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    location->addWidget(m_scope);
    m_project = new QLineEdit(this);
    m_project->setObjectName(QStringLiteral("skillProject"));
    m_project->setPlaceholderText(tr("项目绝对路径，例如 ~/workspace/my-project"));
    location->addWidget(m_project, 2);
    m_browse = new QPushButton(tr("选择目录…"), this);
    location->addWidget(m_browse);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("skillSearch"));
    m_search->setPlaceholderText(tr("搜索名称、目录或描述"));
    m_search->setClearButtonEnabled(true);
    location->addWidget(m_search, 1);
    layout->addLayout(location);

    auto *toolbar = new QHBoxLayout;
    auto button = [this, toolbar](const QString &text, const char *name) {
        auto *b = new QPushButton(text, this);
        b->setObjectName(QString::fromLatin1(name));
        b->setCursor(Qt::PointingHandCursor);
        toolbar->addWidget(b);
        return b;
    };
    m_refresh = button(tr("刷新"), "skillRefresh");
    m_add = button(tr("新建"), "skillAdd");
    m_import = button(tr("导入"), "skillImport");
    auto *importMenu = new QMenu(m_import);
    importMenu->addAction(tr("导入完整技能目录…"), this, [this] { importSkill(true); });
    importMenu->addAction(tr("导入 SKILL.md…"), this, [this] { importSkill(false); });
    m_import->setMenu(importMenu);
    m_import->setStyleSheet(QStringLiteral("QPushButton::menu-indicator { image: none; width: 0px; height: 0px; }"));
    m_edit = button(tr("编辑"), "skillEdit");
    m_toggle = button(tr("停用"), "skillToggle");
    m_export = button(tr("导出"), "skillExport");
    m_delete = button(tr("删除"), "skillDelete");
    toolbar->addStretch();
    layout->addLayout(toolbar);

    auto *splitter = new QSplitter(this);
    m_list = new QTreeWidget(splitter);
    m_list->setObjectName(QStringLiteral("skillList"));
    m_list->setHeaderLabels({tr("名称"), tr("描述"), tr("目录状态"), tr("调用方式")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setAlternatingRowColors(true);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(0, Qt::AscendingOrder);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_list->setColumnWidth(0, 180);
    auto *details = new QWidget(splitter);
    auto *detailLayout = new QVBoxLayout(details);
    detailLayout->setContentsMargins(4, 0, 0, 0);
    m_detail = new QLabel(tr("选择技能查看原文和辅助文件"), details);
    m_detail->setObjectName(QStringLiteral("skillDetail"));
    m_detail->setTextFormat(Qt::PlainText);
    m_detail->setWordWrap(true);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLayout->addWidget(m_detail);
    auto *tabs = new QTabWidget(details);
    m_document = new QPlainTextEdit(tabs);
    m_document->setObjectName(QStringLiteral("skillPreview"));
    m_document->setReadOnly(true);
    m_document->setPlaceholderText(tr("SKILL.md 原文（点击“编辑”修改）"));
    auto mono = font();
    mono.setFamilies(QFontDatabase::systemFont(QFontDatabase::FixedFont).families());
    m_document->setFont(mono);
    tabs->addTab(m_document, QStringLiteral("SKILL.md"));
    m_files = new QListWidget(tabs);
    m_files->setObjectName(QStringLiteral("skillFiles"));
    tabs->addTab(m_files, tr("辅助文件"));
    detailLayout->addWidget(tabs);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);
    m_path = new QLabel(this);
    m_path->setTextFormat(Qt::PlainText);
    m_path->setWordWrap(true);
    m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_path);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("skillStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *hint = new QLabel(tr("仅管理用户/项目自定义 Skills，插件请使用 /plugin。停用将完整目录移至同级 skills-disabled/。\n"
        "目录状态不代表会话最终权限。更改未生效时执行 /reload-skills 或重启会话；已进入上下文的指令不会被撤回。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    connect(backend, &ClaudeCodeBackend::skillsListed, this, &SkillsPanel::onListed, Qt::QueuedConnection);
    connect(backend, &ClaudeCodeBackend::skillLoaded, this, &SkillsPanel::onLoaded, Qt::QueuedConnection);
    connect(backend, &ClaudeCodeBackend::skillOperationFinished, this, &SkillsPanel::onOperation, Qt::QueuedConnection);
    connect(backend, &ClaudeCodeBackend::sessionSourceChanged, this, [this] {
        m_source = m_backend->sessionSource();
        // 项目路径属于一台机器；不把上台机器的路径复用到下一台。
        const QSignalBlocker block(m_project);
        m_project->clear();
        resetContext();
    });
    connect(m_scope, &QComboBox::currentIndexChanged, this, [this] { resetContext(); refresh(); });
    connect(m_project, &QLineEdit::textChanged, this, [this] { resetContext(); });
    connect(m_project, &QLineEdit::editingFinished, this, &SkillsPanel::refresh);
    connect(m_browse, &QPushButton::clicked, this, [this] {
        const auto context = m_context;
        const auto dir = QFileDialog::getExistingDirectory(this, tr("选择本地项目目录"), m_project->text());
        if (context == m_context && !dir.isEmpty()) { m_project->setText(dir); refresh(); }
    });
    connect(m_search, &QLineEdit::textChanged, this, &SkillsPanel::filter);
    connect(m_refresh, &QPushButton::clicked, this, &SkillsPanel::refresh);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &SkillsPanel::selectSkill);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] { openEditor(false); });
    connect(m_add, &QPushButton::clicked, this, [this] { openEditor(true); });
    connect(m_edit, &QPushButton::clicked, this, [this] { openEditor(false); });
    connect(m_toggle, &QPushButton::clicked, this, &SkillsPanel::toggleSkill);
    connect(m_delete, &QPushButton::clicked, this, &SkillsPanel::deleteSkill);
    connect(m_export, &QPushButton::clicked, this, &SkillsPanel::exportSkill);
    auto *find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, m_search, qOverload<>(&QWidget::setFocus));
    auto *reload = new QShortcut(QKeySequence::Refresh, this);
    connect(reload, &QShortcut::activated, this, &SkillsPanel::refresh);
    m_scope->setCursor(Qt::PointingHandCursor);
    m_browse->setCursor(Qt::PointingHandCursor);
    updateButtons();
}
void SkillsPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange && m_document) {
        auto mono = font();
        mono.setFamilies(QFontDatabase::systemFont(QFontDatabase::FixedFont).families());
        m_document->setFont(mono);
    }
}
ClaudeSkillScope SkillsPanel::scope() const
{
    return {m_scope->currentData().toString(), m_scope->currentData() == QLatin1String("project") ? m_project->text().trimmed() : QString()};
}
void SkillsPanel::setStatus(const QString &text) { m_status->setText(text); }
void SkillsPanel::resetContext()
{
    ++m_context;
    m_listRequest = m_detailRequest = m_writeRequest = 0;
    m_listing = m_reading = m_writing = m_ready = m_detailReady = false;
    m_current = {};
    m_restoreKey.clear();
    if (m_editor) m_editor->invalidateSource();
    const QSignalBlocker block(m_list);
    m_list->clear();
    m_document->clear();
    m_files->clear();
    m_path->clear();
    m_detail->setText(tr("选择技能查看原文和辅助文件"));
    setStatus(tr("请选择作用域并刷新；项目作用域需填写路径。"));
    updateButtons();
}
void SkillsPanel::updateButtons()
{
    const bool project = scope().scope == QLatin1String("project");
    m_project->setVisible(project);
    m_browse->setVisible(project && !m_backend->sessionSourceIsRemote());
    m_scope->setEnabled(!m_writing);
    m_project->setEnabled(!m_writing);
    m_browse->setEnabled(!m_writing);
    m_refresh->setEnabled(!m_listing && !m_writing);
    const bool idle = m_ready && !m_listing && !m_writing && !m_editor;
    m_add->setEnabled(idle);
    m_import->setEnabled(idle);
    const bool selected = idle && m_detailReady && !m_reading && !m_current.readOnly;
    m_edit->setEnabled(selected);
    m_toggle->setEnabled(selected);
    m_export->setEnabled(selected);
    m_delete->setEnabled(selected);
    m_toggle->setText(m_current.enabled ? tr("停用") : tr("启用"));
    m_list->setEnabled(!m_writing);
}
void SkillsPanel::refresh()
{
    if (m_listing || m_writing || m_editor) return;
    if (scope().scope == QLatin1String("project") && scope().projectPath.isEmpty()) {
        setStatus(tr("请先填写项目绝对路径。")); return;
    }
    if (m_restoreKey.isEmpty() && m_detailReady) m_restoreKey = m_current.key();
    m_listing = true;
    m_ready = false;
    m_reading = m_detailReady = false;
    m_detailRequest = 0;
    m_listRequest = ++m_nextRequest;
    setStatus(tr("正在读取 Skills…"));
    updateButtons();
    m_backend->refreshSkills(scope(), m_listRequest);
}
void SkillsPanel::onListed(quint64 source, quint64 request, const QList<ClaudeSkillInfo> &entries, const QString &root, const QString &error)
{
    if (source != m_source || request != m_listRequest) return;
    m_listing = false;
    m_ready = error.isEmpty();
    m_path->setText((m_connectionLabel.isEmpty()
        ? (m_backend->sessionSourceIsRemote() ? tr("远程 SSH") : tr("本地")) : m_connectionLabel)
        + QStringLiteral(" · ") + root + QStringLiteral("/skills"));
    {
        const QSignalBlocker block(m_list);
        m_list->setSortingEnabled(false);
        m_list->clear();
        for (const auto &info : entries) {
            QString state = info.enabled ? tr("启用") : tr("停用");
            if (info.readOnly) state += tr(" · 只读");
            if (!info.error.isEmpty()) state += tr(" · 异常");
            auto *item = new QTreeWidgetItem(m_list, {info.name, info.description.simplified(), state, skills::invocationLabel(info)});
            item->setData(0, Qt::UserRole, QVariant::fromValue(info));
            for (int column = 0; column < 4; ++column)
                item->setToolTip(column, info.path + (info.error.isEmpty() ? QString() : QStringLiteral("\n") + info.error));
            if (info.key() == m_restoreKey) m_list->setCurrentItem(item);
        }
        m_list->setSortingEnabled(true);
    }
    m_restoreKey.clear();
    filter();
    selectSkill();
    setStatus(error.isEmpty() ? tr("共 %1 个 Skills。新建或导入即可开始；不会执行导入内容。").arg(entries.size()) : error);
    updateButtons();
}
void SkillsPanel::filter()
{
    const auto query = m_search->text().trimmed();
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        auto *item = m_list->topLevelItem(i);
        const auto info = item->data(0, Qt::UserRole).value<ClaudeSkillInfo>();
        item->setHidden(!(info.name + QLatin1Char(' ') + info.directory + QLatin1Char(' ') + info.description).contains(query, Qt::CaseInsensitive));
        if (item->isHidden() && item->isSelected()) m_list->clearSelection();
    }
}
void SkillsPanel::selectSkill()
{
    m_detailReady = false;
    m_reading = false;
    m_current = {};
    m_document->clear();
    m_files->clear();
    m_detailRequest = ++m_nextRequest;
    if (m_list->selectedItems().isEmpty()) {
        m_detail->setText(tr("选择技能查看原文和辅助文件"));
        updateButtons(); return;
    }
    m_current = m_list->selectedItems().first()->data(0, Qt::UserRole).value<ClaudeSkillInfo>();
    m_reading = true;
    m_detail->setText(tr("正在读取 %1…").arg(m_current.directory));
    updateButtons();
    m_backend->readSkill(scope(), m_current, m_detailRequest);
}
void SkillsPanel::onLoaded(quint64 source, quint64 request, const ClaudeSkillInfo &info, const QStringList &files, const QString &error)
{
    if (source != m_source || request != m_detailRequest) return;
    m_reading = false;
    m_detailReady = !info.directory.isEmpty();
    m_current = info;
    m_document->setPlainText(QString::fromUtf8(info.content));
    m_files->addItems(files);
    QString text = info.path;
    if (info.readOnly) text += tr("\n只读：链接、不可读文件或大小超限不允许修改。");
    if (!info.error.isEmpty()) text += QStringLiteral("\n") + info.error;
    if (!error.isEmpty()) text += QStringLiteral("\n") + error;
    m_detail->setText(text);
    updateButtons();
}
quint64 SkillsPanel::beginOperation(const QString &label)
{
    m_writing = true;
    m_operation = label;
    m_writeRequest = ++m_nextRequest;
    setStatus(label + tr("…"));
    updateButtons();
    return m_writeRequest;
}
void SkillsPanel::onOperation(quint64 source, quint64 request, const QString &error)
{
    if (source != m_source || request != m_writeRequest) return;
    m_writing = false;
    if (m_editor) m_editor->finishSave(error);
    setStatus(error.isEmpty() ? m_operation + tr("完成") : m_operation + tr("失败：") + error);
    updateButtons();
    if (error.isEmpty() && !m_editor) refresh();
}
void SkillsPanel::openEditor(bool creating)
{
    if (creating ? !m_add->isEnabled() : !m_edit->isEnabled()) return;
    const auto context = m_context;
    const auto target = scope();
    const auto original = m_current;
    auto *editor = new SkillEditDialog(creating ? QStringLiteral("my-skill") : original.directory,
        creating ? skills::documentTemplate(QStringLiteral("my-skill")) : original.content, creating, this);
    m_editor = editor;
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &SkillEditDialog::saveRequested, this, [this, editor, context, target, original, creating](const QString &name, const QByteArray &content) {
        if (context != m_context || m_source != m_backend->sessionSource()) { editor->invalidateSource(); return; }
        const auto request = beginOperation(tr("保存 Skill"));
        m_restoreKey = creating ? QStringLiteral("on/") + name : original.key();
        if (creating) m_backend->createSkill(target, name, content, request);
        else m_backend->saveSkill(target, original, content, request);
    });
    connect(editor, &QDialog::finished, this, [this, editor](int result) {
        if (m_editor == editor) m_editor.clear();
        updateButtons();
        if (result == QDialog::Accepted) refresh();
    });
    editor->open();
    updateButtons();
}
void SkillsPanel::importSkill(bool directory)
{
    if (!m_import->isEnabled()) return;
    const auto context = m_context;
    const auto path = directory ? QFileDialog::getExistingDirectory(this, tr("选择包含 SKILL.md 的完整目录"))
        : QFileDialog::getOpenFileName(this, tr("选择 SKILL.md"), {}, tr("Markdown (*.md);;所有文件 (*)"));
    if (path.isEmpty() || context != m_context) return;
    const auto suggested = directory ? QFileInfo(path).fileName() : QFileInfo(path).dir().dirName();
    bool ok = false;
    const auto name = QInputDialog::getText(this, tr("导入 Skill"), tr("目标目录名（小写字母、数字、连字符；不会覆盖同名技能）："),
        QLineEdit::Normal, skills::validNewName(suggested) ? suggested : QStringLiteral("my-skill"), &ok).trimmed();
    if (!ok || name.isEmpty() || context != m_context) return;
    m_restoreKey = QStringLiteral("on/") + name;
    m_backend->importSkill(scope(), name, path, beginOperation(tr("导入 Skill")));
}
void SkillsPanel::exportSkill()
{
    if (!m_export->isEnabled()) return;
    const auto context = m_context;
    const auto info = m_current;
    const auto parent = QFileDialog::getExistingDirectory(this, tr("选择本地导出父目录（将新建技能子目录）"));
    if (parent.isEmpty() || context != m_context) return;
    m_backend->exportSkill(scope(), info, QDir(parent).filePath(info.directory), beginOperation(tr("导出 Skill")));
}
void SkillsPanel::toggleSkill()
{
    if (!m_toggle->isEnabled()) return;
    const auto context = m_context;
    const auto info = m_current;
    if (info.enabled && QMessageBox::question(this, tr("停用 Skill"),
        tr("将完整技能目录移至同级 skills-disabled/，保留所有文件，可再次启用。\n\n%1\n\n已进入会话上下文的指令不会被撤回。").arg(info.path),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (context != m_context) return;
    m_restoreKey = (info.enabled ? QStringLiteral("off/") : QStringLiteral("on/")) + info.directory;
    m_backend->setSkillEnabled(scope(), info, !info.enabled, beginOperation(info.enabled ? tr("停用 Skill") : tr("启用 Skill")));
}
void SkillsPanel::deleteSkill()
{
    if (!m_delete->isEnabled()) return;
    const auto context = m_context;
    const auto info = m_current;
    const auto source = m_connectionLabel.isEmpty()
        ? (m_backend->sessionSourceIsRemote() ? tr("远程 SSH") : tr("本地")) : m_connectionLabel;
    if (QMessageBox::question(this, tr("删除 Skill"),
        tr("确定永久删除此技能及其全部辅助文件？此操作不可撤销。\n\n来源：%1\n作用域：%2\n完整路径：%3")
            .arg(source, m_scope->currentText(), info.path),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (context != m_context) return;
    m_restoreKey.clear();
    m_backend->removeSkill(scope(), info, beginOperation(tr("删除 Skill")));
}
} // namespace cubeshell
