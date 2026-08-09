/*
    Copyright 2013 Christian Surlykke

    This file is part of Konsole.

    Konsole is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Konsole is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Konsole.  If not, see <http://www.gnu.org/licenses/>.
*/

// SearchBar.cpp — C++ port of qtermwidget/search_bar.py

#include "SearchBar.h"

#include <QAction>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPalette>
#include <QToolButton>

using namespace Konsole;

// 对应C++: SearchBar::SearchBar(QWidget *parent) : QWidget(parent)
SearchBar::SearchBar(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
    connectSignals();
    setupOptionsMenu();
    m_searchTextEdit->installEventFilter(this);
}

// 对应C++: SearchBar::~SearchBar()
SearchBar::~SearchBar() = default;

// 对应C++: widget.setupUi(this);
void SearchBar::setupUi()
{
    // 设置背景总是不透明，特别是在半透明窗口内
    // 对应C++: setAutoFillBackground(true);
    setAutoFillBackground(true);

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(6, 3, 6, 3);
    m_layout->setSpacing(4);

    // 上游 Konsole 用 QIcon::fromTheme + 单字母兜底（X / < / >）。freedesktop
    // 图标主题只在 Linux 存在，macOS/Windows 上一律解析失败，于是界面上留下
    // 一排字母按钮。这里统一改用 Unicode 三角/齿轮字形，三平台表现一致。

    // 对应C++: widget.findLabel
    m_findLabel = new QLabel(tr("查找:"), this);
    m_layout->addWidget(m_findLabel);

    // 对应C++: widget.searchTextEdit
    m_searchTextEdit = new QLineEdit(this);
    m_searchTextEdit->setClearButtonEnabled(true);
    m_searchTextEdit->setPlaceholderText(tr("输入关键字，Enter 下一个，Shift+Enter 上一个"));
    m_searchTextEdit->setMinimumWidth(180);
    m_layout->addWidget(m_searchTextEdit, 1);

    // 命中计数（第 n 个 / 共 m 个）。等宽 + 固定最小宽度，避免逐字符搜索时
    // 数字位数变化把两侧按钮推来推去。
    m_matchCountLabel = new QLabel(this);
    m_matchCountLabel->setAlignment(Qt::AlignCenter);
    m_matchCountLabel->setMinimumWidth(72);
    m_matchCountLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_layout->addWidget(m_matchCountLabel);

    // 对应C++: widget.findPreviousButton
    m_findPreviousButton = new QToolButton(this);
    m_findPreviousButton->setText(QStringLiteral("▲"));
    m_findPreviousButton->setToolTip(tr("上一个匹配 (Shift+Enter)"));
    m_findPreviousButton->setAutoRaise(true);
    m_layout->addWidget(m_findPreviousButton);

    // 对应C++: widget.findNextButton
    m_findNextButton = new QToolButton(this);
    m_findNextButton->setText(QStringLiteral("▼"));
    m_findNextButton->setToolTip(tr("下一个匹配 (Enter)"));
    m_findNextButton->setAutoRaise(true);
    m_layout->addWidget(m_findNextButton);

    // 对应C++: widget.optionsButton
    m_optionsButton = new QToolButton(this);
    m_optionsButton->setText(QStringLiteral("⚙"));
    m_optionsButton->setToolTip(tr("搜索选项"));
    m_optionsButton->setAutoRaise(true);
    m_optionsButton->setPopupMode(QToolButton::InstantPopup);
    m_layout->addWidget(m_optionsButton);

    // 对应C++: widget.closeButton（移到行尾，与应用其余关闭按钮位置一致）
    m_closeButton = new QToolButton(this);
    m_closeButton->setText(QStringLiteral("✕"));
    m_closeButton->setToolTip(tr("关闭搜索栏 (Esc)"));
    m_closeButton->setAutoRaise(true);
    m_layout->addWidget(m_closeButton);

    setWindowTitle(tr("搜索"));
}

// 对应C++构造函数中的connect语句
void SearchBar::connectSignals()
{
    // 对应C++: connect(widget.closeButton, &QAbstractButton::clicked, this, &SearchBar::hide);
    connect(m_closeButton, &QAbstractButton::clicked, this, &SearchBar::hide);

    // 搜索词清空时计数也要清掉，否则会残留上一次的“0/0”或旧数字。
    connect(m_searchTextEdit, &QLineEdit::textChanged, this, [this](const QString &t) {
        if (t.isEmpty())
            clearMatchCount();
    });

    // 对应C++: connect(widget.searchTextEdit, SIGNAL(textChanged(QString)), this, SIGNAL(searchCriteriaChanged()));
    connect(m_searchTextEdit, &QLineEdit::textChanged, this, &SearchBar::searchCriteriaChanged);

    // 对应C++: connect(widget.findPreviousButton, SIGNAL(clicked()), this, SIGNAL(findPrevious()));
    connect(m_findPreviousButton, &QAbstractButton::clicked, this, &SearchBar::findPrevious);

    // 对应C++: connect(widget.findNextButton, SIGNAL(clicked()), this, SIGNAL(findNext()));
    connect(m_findNextButton, &QAbstractButton::clicked, this, &SearchBar::findNext);

    // 对应C++: connect(this, SIGNAL(searchCriteriaChanged()), this, SLOT(clearBackgroundColor()));
    connect(this, &SearchBar::searchCriteriaChanged, this, &SearchBar::clearBackgroundColor);
}

// 对应C++构造函数中的选项菜单设置
void SearchBar::setupOptionsMenu()
{
    // 对应C++: QMenu *optionsMenu = new QMenu(widget.optionsButton);
    // widget.optionsButton->setMenu(optionsMenu);
    auto *optionsMenu = new QMenu(m_optionsButton);
    m_optionsButton->setMenu(optionsMenu);

    // 对应C++: m_matchCaseMenuEntry = optionsMenu->addAction(tr("Match case"));
    // 默认关闭：查日志时大小写不敏感更常用（error / ERROR / Error 一次捞全）。
    m_matchCaseMenuEntry = optionsMenu->addAction(tr("区分大小写"));
    m_matchCaseMenuEntry->setCheckable(true);
    m_matchCaseMenuEntry->setChecked(false);
    connect(m_matchCaseMenuEntry, &QAction::toggled, this, &SearchBar::searchCriteriaChanged);

    // 对应C++: m_useRegularExpressionMenuEntry = optionsMenu->addAction(tr("Regular expression"));
    m_useRegularExpressionMenuEntry = optionsMenu->addAction(tr("正则表达式"));
    m_useRegularExpressionMenuEntry->setCheckable(true);
    connect(m_useRegularExpressionMenuEntry, &QAction::toggled, this, &SearchBar::searchCriteriaChanged);

    // 对应C++: m_highlightMatchesMenuEntry = optionsMenu->addAction(tr("Highlight all matches"));
    m_highlightMatchesMenuEntry = optionsMenu->addAction(tr("高亮所有匹配"));
    m_highlightMatchesMenuEntry->setCheckable(true);
    m_highlightMatchesMenuEntry->setChecked(true);
    connect(m_highlightMatchesMenuEntry, &QAction::toggled, this, &SearchBar::highlightMatchesChanged);
}

void SearchBar::setSearchText(const QString &text)
{
    m_searchTextEdit->setText(text);
}

void SearchBar::setMatchCount(int index, int total)
{
    if (total <= 0) {
        // 有搜索词但一个都没命中。
        m_matchCountLabel->setText(tr("无匹配"));
        m_matchCountLabel->setToolTip(QString());
        return;
    }
    m_matchCountLabel->setText(tr("%1/%2").arg(index).arg(total));
    m_matchCountLabel->setToolTip(tr("共 %n 处匹配", nullptr, total));
}

void SearchBar::clearMatchCount()
{
    m_matchCountLabel->clear();
    m_matchCountLabel->setToolTip(QString());
}

void SearchBar::notifyWrapped(bool forwards)
{
    // HistorySearch 找不到就从另一头再扫一遍，命中位置会突然跳到反方向；
    // 不提示的话看着像搜索乱跳。只改 tooltip，不弹窗打断节奏。
    m_matchCountLabel->setToolTip(forwards ? tr("已回到开头继续查找")
                                           : tr("已回到末尾继续查找"));
}

// 对应C++: QString SearchBar::searchText()
QString SearchBar::searchText()
{
    // 对应C++: return widget.searchTextEdit->text();
    return m_searchTextEdit->text();
}

// 对应C++: bool SearchBar::useRegularExpression()
bool SearchBar::useRegularExpression()
{
    // 对应C++: return m_useRegularExpressionMenuEntry->isChecked();
    return m_useRegularExpressionMenuEntry->isChecked();
}

// 对应C++: bool SearchBar::matchCase()
bool SearchBar::matchCase()
{
    // 对应C++: return m_matchCaseMenuEntry->isChecked();
    return m_matchCaseMenuEntry->isChecked();
}

// 对应C++: bool SearchBar::highlightAllMatches()
bool SearchBar::highlightAllMatches()
{
    // 对应C++: return m_highlightMatchesMenuEntry->isChecked();
    return m_highlightMatchesMenuEntry->isChecked();
}

// 对应C++: void SearchBar::show()
void SearchBar::show()
{
    // 对应C++: QWidget::show();
    // widget.searchTextEdit->setFocus();
    // widget.searchTextEdit->selectAll();
    QWidget::show();
    m_searchTextEdit->setFocus();
    m_searchTextEdit->selectAll();
}

// 对应C++: void SearchBar::hide()
void SearchBar::hide()
{
    // 对应C++: QWidget::hide();
    // if (QWidget *p = parentWidget()) { p->setFocus(Qt::OtherFocusReason); }
    QWidget::hide();
    Q_EMIT closed();
    if (QWidget *p = parentWidget()) {
        p->setFocus(Qt::OtherFocusReason);
    }
}

// 对应C++: void SearchBar::noMatchFound()
void SearchBar::noMatchFound()
{
    // 在终端配色的基础上只改底色，否则会把 applyTerminalPalette 设好的
    // 文字色一起冲掉（深色终端里会变成红底黑字，看不清）。
    QPalette palette = m_editPalette;
    palette.setColor(m_searchTextEdit->backgroundRole(), QColor(255, 128, 128));
    palette.setColor(QPalette::Text, QColor(32, 0, 0));
    m_searchTextEdit->setPalette(palette);
}

// 对应C++: void SearchBar::keyReleaseEvent(QKeyEvent *keyEvent)
// 实际拦截在 eventFilter 里（输入框先收到按键）；这里兜住搜索栏自身有焦点的情况。
void SearchBar::keyReleaseEvent(QKeyEvent *keyEvent)
{
    if (keyEvent->key() == Qt::Key_Escape) {
        hide();
        return;
    }
    QWidget::keyReleaseEvent(keyEvent);
}

bool SearchBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_searchTextEdit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        const int key = ke->key();
        const bool back = ke->modifiers().testFlag(Qt::ShiftModifier);

        // Enter / Shift+Enter 翻下一个/上一个；F3 同义（对齐主流编辑器）。
        if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_F3) {
            if (back)
                Q_EMIT findPrevious();
            else
                Q_EMIT findNext();
            return true;
        }
        if (key == Qt::Key_Escape) {
            hide();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// 对应C++: void SearchBar::clearBackgroundColor()
void SearchBar::clearBackgroundColor()
{
    // 原版还原成 window()->palette()（应用主题色）。改成还原到
    // m_editPalette：终端配色跟应用主题经常不是一套（比如亮色应用 UI +
    // 深色终端配色方案），还原到应用主题会让输入框在深色终端里刺眼。
    m_searchTextEdit->setPalette(m_editPalette);
}

void SearchBar::applyTerminalPalette(const QColor &background, const QColor &foreground)
{
    setAutoFillBackground(true);

    // 搜索栏背景比终端背景稍微提亮/压暗一点，跟终端区分出一条边界，
    // 但不要用应用的强调色（太跳,和终端配色不搭）。
    const bool dark = background.lightness() < 128;
    QColor barBg = dark ? background.lighter(130) : background.darker(110);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, barBg);
    pal.setColor(QPalette::WindowText, foreground);
    setPalette(pal);

    m_editPalette = palette();
    m_editPalette.setColor(m_searchTextEdit->backgroundRole(), background);
    m_editPalette.setColor(QPalette::Text, foreground);
    m_searchTextEdit->setPalette(m_editPalette);

    m_matchCountLabel->setPalette(pal);
    m_findLabel->setPalette(pal);
}
