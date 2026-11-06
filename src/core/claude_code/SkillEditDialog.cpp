#include "claude_code/SkillEditDialog.h"
#include "claude_code/ClaudeSkillData.h"
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTextDocument>
#include <QVBoxLayout>

namespace cubeshell {
SkillEditDialog::SkillEditDialog(const QString &directory, const QByteArray &bytes, bool creating, QWidget *parent)
    : QDialog(parent), m_original(bytes), m_creating(creating)
{
    setObjectName(QStringLiteral("skillEditDialog"));
    setWindowTitle(creating ? tr("新建 Skill") : tr("编辑 Skill — %1").arg(directory));
    resize(820, 620);
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_name = new QLineEdit(directory, this);
    m_name->setObjectName(QStringLiteral("skillDirectory"));
    m_name->setReadOnly(!creating);
    m_name->setPlaceholderText(QStringLiteral("my-skill"));
    form->addRow(tr("目录名称："), m_name);
    layout->addLayout(form);
    auto *hint = new QLabel(tr("直接编辑 SKILL.md。YAML 字段与正文原样保留；description 建议说明使用场景。\n"
                              "disable-model-invocation: true 仅禁止自动调用，并不等于完全停用。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_editor = new QPlainTextEdit(this);
    m_editor->setObjectName(QStringLiteral("skillDocument"));
    auto font = this->font();
    font.setFamilies(QFontDatabase::systemFont(QFontDatabase::FixedFont).families());
    m_editor->setFont(font);
    m_editor->setPlainText(QString::fromUtf8(bytes));
    m_initialText = m_editor->toPlainText();
    m_editor->document()->setModified(false);
    layout->addWidget(m_editor, 1);
    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("skillEditError"));
    m_error->setTextFormat(Qt::PlainText);
    m_error->setWordWrap(true);
    m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    m_save = buttons->button(QDialogButtonBox::Save);
    m_save->setText(tr("保存"));
    m_save->setObjectName(QStringLiteral("skillSave"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &SkillEditDialog::submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &SkillEditDialog::reject);
    connect(m_name, &QLineEdit::textChanged, this, [this] {
        // 新建模板尚未编辑时同步 name；用户已编写的 frontmatter 不自动改写。
        if (m_creating && m_editor->toPlainText() == m_initialText && skills::validNewName(m_name->text().trimmed())) {
            m_original = skills::documentTemplate(m_name->text().trimmed());
            m_initialText = QString::fromUtf8(m_original);
            m_editor->setPlainText(m_initialText);
            m_editor->document()->setModified(false);
        }
        validate();
    });
    connect(m_editor, &QPlainTextEdit::textChanged, this, &SkillEditDialog::validate);
    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, &SkillEditDialog::submit);
    for (auto *button : findChildren<QPushButton *>()) button->setCursor(Qt::PointingHandCursor);
    validate();
}
QByteArray SkillEditDialog::content() const
{
    auto text = m_editor->toPlainText();
    if (text == m_initialText) return m_original;
    if (m_original.contains("\r\n")) text.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    return text.toUtf8();
}
void SkillEditDialog::validate()
{
    if (m_invalid || m_pending) return;
    auto error = skills::validateDocument(content());
    if (m_creating && !skills::validNewName(m_name->text().trimmed()))
        error = tr("目录名须为 1–64 位小写字母、数字和连字符，且不能是保留名称。");
    m_error->setText(error);
    m_save->setEnabled(error.isEmpty());
}
void SkillEditDialog::submit()
{
    if (m_pending || m_invalid) return;
    validate();
    if (!m_save->isEnabled()) return;
    m_pending = true;
    m_save->setEnabled(false);
    m_editor->setReadOnly(true);
    m_name->setReadOnly(true);
    m_error->setText(tr("正在保存…"));
    emit saveRequested(m_name->text().trimmed(), content());
}
void SkillEditDialog::finishSave(const QString &error)
{
    m_pending = false;
    if (m_invalid) return;
    if (error.isEmpty()) { accept(); return; }
    m_editor->setReadOnly(false);
    m_name->setReadOnly(!m_creating);
    validate();
    m_error->setText(error); // 保留原文；网络错误可重试，冲突需重新打开，不静默覆盖
}
void SkillEditDialog::invalidateSource()
{
    m_invalid = true;
    m_pending = false;
    m_save->setEnabled(false);
    m_editor->setReadOnly(false); // 允许复制或整理草稿，但不能写到新来源
    m_error->setText(tr("连接或作用域已改变，当前草稿不能保存。请复制需要的内容，关闭后重新打开技能。"));
}
void SkillEditDialog::reject()
{
    if (m_pending) return;
    if (m_editor->document()->isModified() || (m_creating && !m_name->text().isEmpty())) {
        if (QMessageBox::question(this, tr("放弃修改"), tr("有未保存的内容，确定关闭吗？"),
                                  QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Discard)
            return;
    }
    QDialog::reject();
}
} // namespace cubeshell
