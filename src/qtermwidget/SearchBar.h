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

#pragma once

// SearchBar.h — C++ port of qtermwidget/search_bar.py
//
// Incremental search bar widget shown at the bottom of the terminal. Emits
// search criteria; connects (in TerminalDisplay) to ScreenWindow findText.
// Ported from the Python version (converted from upstream Konsole SearchBar).

#include <QColor>
#include <QWidget>

#include <QPalette>

class QHBoxLayout;
class QToolButton;
class QLabel;
class QLineEdit;
class QAction;
class QKeyEvent;

namespace Konsole {

/**
 * 搜索栏组件 - 提供终端内容搜索功能的UI界面
 *
 * 包含搜索文本框、前进/后退导航按钮、选项菜单等。
 * 支持大小写匹配、正则表达式、高亮所有匹配等功能。
 *
 * 对应C++: class SearchBar : public QWidget
 */
class SearchBar : public QWidget
{
    Q_OBJECT

public:
    // 对应C++: explicit SearchBar(QWidget *parent = nullptr);
    explicit SearchBar(QWidget *parent = nullptr);
    // 对应C++: ~SearchBar() override;
    ~SearchBar() override;

    // 对应C++: QString searchText();
    QString searchText();
    // 对应C++: bool useRegularExpression();
    bool useRegularExpression();
    // 对应C++: bool matchCase();
    bool matchCase();
    // 对应C++: bool highlightAllMatches();
    bool highlightAllMatches();

    // 把关键字预填进输入框（右键“查找选中内容”用）。
    void setSearchText(const QString &text);

    // 跟随终端配色：搜索栏紧贴终端下沿，用应用主题色会突兀。
    void applyTerminalPalette(const QColor &background, const QColor &foreground);

public Q_SLOTS:
    // 对应C++: void show();
    void show();
    // 对应C++: void hide();
    void hide();
    // 对应C++: void noMatchFound();
    void noMatchFound();

    // 命中计数：显示“第 index 个 / 共 total 个”。index 从 1 起，0 表示未定位。
    void setMatchCount(int index, int total);
    // 清空计数（搜索词为空时）。
    void clearMatchCount();
    // 本次跳转发生了回绕，提示一次“已回到开头/末尾继续查找”。
    void notifyWrapped(bool forwards);

Q_SIGNALS:
    // 搜索条件改变
    void searchCriteriaChanged();
    // 高亮匹配改变
    void highlightMatchesChanged(bool);
    // 查找下一个
    void findNext();
    // 查找上一个
    void findPrevious();
    // 搜索栏关闭（终端据此清掉高亮过滤器）。
    void closed();

protected:
    // 对应C++: void keyReleaseEvent(QKeyEvent *keyEvent) override;
    void keyReleaseEvent(QKeyEvent *keyEvent) override;
    // Enter / Shift+Enter / Esc / F3 必须在 QLineEdit 处理之前拦下来：靠事件
    // 冒泡到父窗口不可靠（QLineEdit 对部分按键会 accept），所以装事件过滤器。
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    // 对应C++: void clearBackgroundColor();
    void clearBackgroundColor();

private:
    void setupUi();
    void connectSignals();
    void setupOptionsMenu();

    QHBoxLayout *m_layout = nullptr;
    QToolButton *m_closeButton = nullptr;
    QLabel *m_findLabel = nullptr;
    QLineEdit *m_searchTextEdit = nullptr;
    QLabel *m_matchCountLabel = nullptr;
    QToolButton *m_findPreviousButton = nullptr;
    QToolButton *m_findNextButton = nullptr;
    QToolButton *m_optionsButton = nullptr;

    QAction *m_matchCaseMenuEntry = nullptr;
    QAction *m_useRegularExpressionMenuEntry = nullptr;
    QAction *m_highlightMatchesMenuEntry = nullptr;

    // 输入框的正常配色（跟随终端主题）。“无匹配”时临时染红，
    // 条件变化后由 clearBackgroundColor() 还原成这一份。
    QPalette m_editPalette;
};

} // namespace Konsole
