#include "TextEditor.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

#include "CodeEditor.h"

namespace cubeshell {

// 对应Python: ui/text_editor.py::Ui_MainWindow.setupUi
TextEditor::TextEditor(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(tr("文本编辑"));
    resize(1280, 720);

    m_editor = new CodeEditor(this);

    // 查找/替换工具条（默认隐藏，Ctrl+F 打开）
    m_searchBar = new QWidget(this);
    m_findEdit = new QLineEdit(m_searchBar);
    m_findEdit->setPlaceholderText(tr("查找"));
    m_replaceEdit = new QLineEdit(m_searchBar);
    m_replaceEdit->setPlaceholderText(tr("替换为"));
    m_regexCheck = new QCheckBox(tr("正则"), m_searchBar);
    m_caseCheck = new QCheckBox(tr("区分大小写"), m_searchBar);
    auto *findNextBtn = new QPushButton(tr("下一个"), m_searchBar);
    auto *findPrevBtn = new QPushButton(tr("上一个"), m_searchBar);
    auto *replaceBtn = new QPushButton(tr("替换"), m_searchBar);
    auto *replaceAllBtn = new QPushButton(tr("全部替换"), m_searchBar);
    auto *closeBtn = new QPushButton(QStringLiteral("×"), m_searchBar);
    closeBtn->setFixedWidth(24);
    m_status = new QLabel(m_searchBar);

    auto *barLayout = new QHBoxLayout(m_searchBar);
    barLayout->setContentsMargins(4, 2, 4, 2);
    barLayout->addWidget(m_findEdit, 1);
    barLayout->addWidget(m_replaceEdit, 1);
    barLayout->addWidget(m_regexCheck);
    barLayout->addWidget(m_caseCheck);
    barLayout->addWidget(findPrevBtn);
    barLayout->addWidget(findNextBtn);
    barLayout->addWidget(replaceBtn);
    barLayout->addWidget(replaceAllBtn);
    barLayout->addWidget(m_status);
    barLayout->addWidget(closeBtn);
    m_searchBar->setVisible(false);

    auto *saveBtn = new QPushButton(tr("保存"), this);
    auto *topRow = new QHBoxLayout;
    topRow->setContentsMargins(4, 2, 4, 2);
    topRow->addWidget(saveBtn);
    topRow->addStretch(1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(topRow);
    layout->addWidget(m_searchBar);
    layout->addWidget(m_editor, 1);

    connect(saveBtn, &QPushButton::clicked, this, &TextEditor::onSave);
    connect(findNextBtn, &QPushButton::clicked, this, &TextEditor::onFindNext);
    connect(findPrevBtn, &QPushButton::clicked, this, &TextEditor::onFindPrev);
    connect(replaceBtn, &QPushButton::clicked, this, &TextEditor::onReplace);
    connect(replaceAllBtn, &QPushButton::clicked, this, &TextEditor::onReplaceAll);
    connect(closeBtn, &QPushButton::clicked, this, &TextEditor::toggleSearchBar);
    connect(m_findEdit, &QLineEdit::returnPressed, this, &TextEditor::onFindNext);

    // 快捷键：Ctrl+S 保存 / Ctrl+F 查找（对应Python: text_editor.ui action Ctrl+S）
    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, &TextEditor::onSave);
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, this, &TextEditor::toggleSearchBar);
}

void TextEditor::setPlainText(const QString &text)
{
    m_editor->setPlainText(text);
    m_editor->document()->setModified(false);
}

QString TextEditor::toPlainText() const
{
    return m_editor->toPlainText();
}

void TextEditor::setFileLabel(const QString &label)
{
    m_fileLabel = label;
    setWindowTitle(label.isEmpty() ? tr("文本编辑")
                                   : tr("文本编辑 - %1").arg(label));
}

bool TextEditor::isModified() const
{
    return m_editor->document()->isModified();
}

// 对应Python: cube-shell.py 中编辑器保存回写逻辑（saving 信号）
void TextEditor::onSave()
{
    emit saveRequested(m_editor->toPlainText());
    m_editor->document()->setModified(false);
}

void TextEditor::onFindNext()
{
    if (m_findEdit->text().isEmpty())
        return;
    const bool found = m_editor->findText(m_findEdit->text(), m_regexCheck->isChecked(),
                                          m_caseCheck->isChecked(), false);
    m_status->setText(found ? QString() : tr("未找到"));
}

void TextEditor::onFindPrev()
{
    if (m_findEdit->text().isEmpty())
        return;
    const bool found = m_editor->findText(m_findEdit->text(), m_regexCheck->isChecked(),
                                          m_caseCheck->isChecked(), true);
    m_status->setText(found ? QString() : tr("未找到"));
}

void TextEditor::onReplace()
{
    if (m_findEdit->text().isEmpty())
        return;
    m_editor->replaceText(m_findEdit->text(), m_replaceEdit->text(),
                          m_regexCheck->isChecked(), m_caseCheck->isChecked());
}

void TextEditor::onReplaceAll()
{
    if (m_findEdit->text().isEmpty())
        return;
    const int n = m_editor->replaceAll(m_findEdit->text(), m_replaceEdit->text(),
                                       m_regexCheck->isChecked(), m_caseCheck->isChecked());
    m_status->setText(tr("已替换 %1 处").arg(n));
}

void TextEditor::toggleSearchBar()
{
    const bool show = !m_searchBar->isVisible();
    m_searchBar->setVisible(show);
    if (show)
        m_findEdit->setFocus();
}

void TextEditor::keyPressEvent(QKeyEvent *event)
{
    // Esc 关闭查找条
    if (event->key() == Qt::Key_Escape && m_searchBar->isVisible()) {
        m_searchBar->setVisible(false);
        return;
    }
    QWidget::keyPressEvent(event);
}

void TextEditor::closeEvent(QCloseEvent *event)
{
    if (isModified()) {
        const auto ret = QMessageBox::question(
            this, tr("未保存的修改"),
            tr("文档已修改，关闭前是否保存？"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (ret == QMessageBox::Save) {
            onSave();
        } else if (ret == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

} // namespace cubeshell
