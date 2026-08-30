// SnippetsDialog.cpp — see SnippetsDialog.h.

#include "SnippetsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace cubeshell {

SnippetsDialog::SnippetsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("片段（Snippets）"));
    resize(680, 460);
    setModal(false);

    auto *layout = new QVBoxLayout(this);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("搜索片段…"));
    connect(m_search, &QLineEdit::textChanged, this, [this]() { rebuildTree(); });
    layout->addWidget(m_search);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("名称"), tr("快捷键"), tr("内容")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    // 双击 = 下发（比先选中再点按钮快）。
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *, int) {
        Snippet s;
        if (selectedSnippet(&s))
            emit runSnippetRequested(s);
    });
    layout->addWidget(m_tree, 1);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *addBtn = buttons->addButton(tr("新建"), QDialogButtonBox::ActionRole);
    QPushButton *editBtn = buttons->addButton(tr("编辑"), QDialogButtonBox::ActionRole);
    QPushButton *delBtn = buttons->addButton(tr("删除"), QDialogButtonBox::ActionRole);
    QPushButton *runBtn = buttons->addButton(tr("下发到当前会话"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(addBtn, &QPushButton::clicked, this, &SnippetsDialog::addSnippet);
    connect(editBtn, &QPushButton::clicked, this, &SnippetsDialog::editSelected);
    connect(delBtn, &QPushButton::clicked, this, &SnippetsDialog::removeSelected);
    connect(runBtn, &QPushButton::clicked, this, [this]() {
        Snippet s;
        if (selectedSnippet(&s))
            emit runSnippetRequested(s);
        else
            m_status->setText(tr("请先选中一个片段"));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    rebuildTree();
}

void SnippetsDialog::rebuildTree()
{
    const QString needle = m_search->text().trimmed();
    m_tree->clear();

    // 分组 -> 顶层节点；未分组直接挂顶层。先按分组聚类。
    const QList<Snippet> all = m_store.load();
    QHash<QString, QTreeWidgetItem *> groupNodes;
    for (const Snippet &s : all) {
        if (!needle.isEmpty() && !s.name.contains(needle, Qt::CaseInsensitive)
            && !s.body.contains(needle, Qt::CaseInsensitive)
            && !s.group.contains(needle, Qt::CaseInsensitive))
            continue;

        QTreeWidgetItem *parent = nullptr;
        if (!s.group.isEmpty()) {
            parent = groupNodes.value(s.group);
            if (!parent) {
                parent = new QTreeWidgetItem({s.group, QString(), QString()});
                parent->setFlags(parent->flags() & ~Qt::ItemIsSelectable);
                m_tree->addTopLevelItem(parent);
                groupNodes.insert(s.group, parent);
            }
        }
        // 内容预览：多行压成一行，避免把行高撑爆。
        QString preview = s.body;
        preview.replace(QLatin1Char('\n'), QStringLiteral(" ⏎ "));
        auto *item = new QTreeWidgetItem({s.name, s.shortcut, preview});
        item->setData(0, Qt::UserRole, s.id);
        item->setToolTip(2, s.body);
        if (parent)
            parent->addChild(item);
        else
            m_tree->addTopLevelItem(item);
    }
    m_tree->expandAll();
}

bool SnippetsDialog::selectedSnippet(Snippet *out) const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item)
        return false;
    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty())
        return false;   // 分组节点
    for (const Snippet &s : m_store.load()) {
        if (s.id == id) {
            if (out)
                *out = s;
            return true;
        }
    }
    return false;
}

void SnippetsDialog::addSnippet()
{
    Snippet s;
    if (!editSnippet(&s, true))
        return;
    QString err;
    if (m_store.upsert(s, &err)) {
        rebuildTree();
        emit snippetsChanged();
    } else {
        QMessageBox::warning(this, tr("保存失败"), err);
    }
}

void SnippetsDialog::editSelected()
{
    Snippet s;
    if (!selectedSnippet(&s)) {
        m_status->setText(tr("请先选中一个片段"));
        return;
    }
    if (!editSnippet(&s, false))
        return;
    QString err;
    if (m_store.upsert(s, &err)) {
        rebuildTree();
        emit snippetsChanged();
    } else {
        QMessageBox::warning(this, tr("保存失败"), err);
    }
}

void SnippetsDialog::removeSelected()
{
    Snippet s;
    if (!selectedSnippet(&s)) {
        m_status->setText(tr("请先选中一个片段"));
        return;
    }
    if (QMessageBox::question(this, tr("删除片段"),
                              tr("确定删除片段「%1」吗？").arg(s.name))
        != QMessageBox::Yes)
        return;
    QString err;
    if (m_store.remove(s.id, &err)) {
        rebuildTree();
        emit snippetsChanged();
    } else {
        QMessageBox::warning(this, tr("删除失败"), err);
    }
}

// 新建/编辑共用的表单：名称、分组、快捷键、内容、是否补回车。
bool SnippetsDialog::editSnippet(Snippet *s, bool isNew)
{
    QDialog dlg(this);
    dlg.setWindowTitle(isNew ? tr("新建片段") : tr("编辑片段"));
    auto *form = new QFormLayout(&dlg);

    auto *name = new QLineEdit(s->name, &dlg);
    auto *group = new QLineEdit(s->group, &dlg);
    group->setPlaceholderText(tr("可选，用于分组归类"));
    auto *shortcut = new QKeySequenceEdit(QKeySequence::fromString(s->shortcut), &dlg);
    auto *body = new QPlainTextEdit(s->body, &dlg);
    body->setPlaceholderText(tr("命令内容；用 {参数名} 表示下发前要填的占位参数"));
    auto *newline = new QCheckBox(tr("下发后补回车"), &dlg);
    newline->setChecked(s->appendNewline);

    form->addRow(tr("名称："), name);
    form->addRow(tr("分组："), group);
    form->addRow(tr("快捷键："), shortcut);
    form->addRow(tr("内容："), body);
    form->addRow(QString(), newline);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);

    if (dlg.exec() != QDialog::Accepted)
        return false;
    if (name->text().trimmed().isEmpty() || body->toPlainText().isEmpty()) {
        QMessageBox::warning(this, tr("片段"), tr("名称和内容不能为空。"));
        return false;
    }
    s->name = name->text().trimmed();
    s->group = group->text().trimmed();
    s->shortcut = shortcut->keySequence().toString();
    s->body = body->toPlainText();
    s->appendNewline = newline->isChecked();
    return true;
}

// 参数填写：无占位符直接通过；有占位符逐行填，空值保留占位符原样（见 expand）。
bool SnippetsDialog::promptParams(QWidget *parent, const Snippet &snippet,
                                  QHash<QString, QString> *values)
{
    const QStringList params = SnippetsStore::extractParams(snippet.body);
    if (params.isEmpty())
        return true;

    QDialog dlg(parent);
    dlg.setWindowTitle(tr("填写参数：%1").arg(snippet.name));
    auto *form = new QFormLayout(&dlg);
    QHash<QString, QLineEdit *> edits;
    for (const QString &p : params) {
        auto *edit = new QLineEdit(&dlg);
        edit->setPlaceholderText(QStringLiteral("{%1}").arg(p));
        form->addRow(p + QLatin1Char(':'), edit);
        edits.insert(p, edit);
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);

    if (dlg.exec() != QDialog::Accepted)
        return false;
    for (const QString &p : params)
        (*values)[p] = edits.value(p)->text();
    return true;
}

} // namespace cubeshell
