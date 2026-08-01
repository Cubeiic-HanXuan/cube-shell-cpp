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

    // 对应C++: widget.closeButton
    m_closeButton = new QToolButton(this);
    m_closeButton->setText(QStringLiteral("X"));
    m_closeButton->setIcon(QIcon::fromTheme(QStringLiteral("dialog-close")));
    m_layout->addWidget(m_closeButton);

    // 对应C++: widget.findLabel
    m_findLabel = new QLabel(tr("Find:"), this);
    m_layout->addWidget(m_findLabel);

    // 对应C++: widget.searchTextEdit
    m_searchTextEdit = new QLineEdit(this);
    m_layout->addWidget(m_searchTextEdit);

    // 对应C++: widget.findPreviousButton
    m_findPreviousButton = new QToolButton(this);
    m_findPreviousButton->setText(QStringLiteral("<"));
    m_findPreviousButton->setIcon(QIcon::fromTheme(QStringLiteral("go-previous")));
    m_layout->addWidget(m_findPreviousButton);

    // 对应C++: widget.findNextButton
    m_findNextButton = new QToolButton(this);
    m_findNextButton->setText(QStringLiteral(">"));
    m_findNextButton->setIcon(QIcon::fromTheme(QStringLiteral("go-next")));
    m_layout->addWidget(m_findNextButton);

    // 对应C++: widget.optionsButton
    m_optionsButton = new QToolButton(this);
    m_optionsButton->setText(QStringLiteral("..."));
    m_optionsButton->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system")));
    m_optionsButton->setPopupMode(QToolButton::InstantPopup);
    m_layout->addWidget(m_optionsButton);

    setWindowTitle(QStringLiteral("SearchBar"));
    resize(399, 40);
}

// 对应C++构造函数中的connect语句
void SearchBar::connectSignals()
{
    // 对应C++: connect(widget.closeButton, &QAbstractButton::clicked, this, &SearchBar::hide);
    connect(m_closeButton, &QAbstractButton::clicked, this, &SearchBar::hide);

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
    m_matchCaseMenuEntry = optionsMenu->addAction(tr("Match case"));
    m_matchCaseMenuEntry->setCheckable(true);
    m_matchCaseMenuEntry->setChecked(true);
    connect(m_matchCaseMenuEntry, &QAction::toggled, this, &SearchBar::searchCriteriaChanged);

    // 对应C++: m_useRegularExpressionMenuEntry = optionsMenu->addAction(tr("Regular expression"));
    m_useRegularExpressionMenuEntry = optionsMenu->addAction(tr("Regular expression"));
    m_useRegularExpressionMenuEntry->setCheckable(true);
    connect(m_useRegularExpressionMenuEntry, &QAction::toggled, this, &SearchBar::searchCriteriaChanged);

    // 对应C++: m_highlightMatchesMenuEntry = optionsMenu->addAction(tr("Highlight all matches"));
    m_highlightMatchesMenuEntry = optionsMenu->addAction(tr("Highlight all matches"));
    m_highlightMatchesMenuEntry->setCheckable(true);
    m_highlightMatchesMenuEntry->setChecked(true);
    connect(m_highlightMatchesMenuEntry, &QAction::toggled, this, &SearchBar::highlightMatchesChanged);
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
    if (QWidget *p = parentWidget()) {
        p->setFocus(Qt::OtherFocusReason);
    }
}

// 对应C++: void SearchBar::noMatchFound()
void SearchBar::noMatchFound()
{
    // 对应C++: QPalette palette;
    // palette.setColor(widget.searchTextEdit->backgroundRole(), QColor(255, 128, 128));
    // widget.searchTextEdit->setPalette(palette);
    QPalette palette;
    palette.setColor(m_searchTextEdit->backgroundRole(), QColor(255, 128, 128));
    m_searchTextEdit->setPalette(palette);
}

// 对应C++: void SearchBar::keyReleaseEvent(QKeyEvent *keyEvent)
void SearchBar::keyReleaseEvent(QKeyEvent *keyEvent)
{
    if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
        if (keyEvent->modifiers() == Qt::ShiftModifier) {
            Q_EMIT findPrevious();
        } else {
            Q_EMIT findNext();
        }
    } else if (keyEvent->key() == Qt::Key_Escape) {
        hide();
    } else {
        QWidget::keyReleaseEvent(keyEvent);
    }
}

// 对应C++: void SearchBar::clearBackgroundColor()
void SearchBar::clearBackgroundColor()
{
    // 对应C++: widget.searchTextEdit->setPalette(QWidget::window()->palette());
    if (window()) {
        m_searchTextEdit->setPalette(window()->palette());
    }
}
