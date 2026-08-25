// StatusBoxItem — MobaXterm 风格状态栏小方块组件实现。
// 对应Python: cube-shell.py::StatusBoxItem（L505-552），样式/边距逐项复刻。

#include "status_box_item.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>

namespace cubeshell {

StatusBoxItem::StatusBoxItem(const QString &iconColor, const QString &iconChar,
                             const QString &text, QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("statusBoxItem"));
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 1, 6, 1);
    layout->setSpacing(4);

    // 彩色图标标签（固定尺寸的彩色小方块，白色字符）
    m_iconLabel = new QLabel(iconChar, this);
    m_iconLabel->setFixedSize(18, 16);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setStyleSheet(QStringLiteral(
        "background-color: %1; color: white; "
        "border-radius: 2px; font-size: 9px; font-weight: bold; "
        "border: none; padding: 0px;").arg(iconColor));
    layout->addWidget(m_iconLabel);

    // 文字标签。颜色用 palette(window-text) 跟随主题调色板：亮色主题下为深色、
    // 暗色主题下为浅色，随 ThemeManager 应用主题自动切换。
    // （此前硬编码 #cccccc，亮色主题浅底上几乎不可见。）
    m_textLabel = new QLabel(text, this);
    m_textLabel->setStyleSheet(QStringLiteral(
        "color: palette(window-text); background: transparent; "
        "border: none; font-size: 11px; padding: 0px;"));
    layout->addWidget(m_textLabel);
}

void StatusBoxItem::setText(const QString &text)
{
    m_textLabel->setText(text);
}

QString StatusBoxItem::text() const
{
    return m_textLabel->text();
}

void StatusBoxItem::setTextColor(const QString &color)
{
    m_textLabel->setStyleSheet(QStringLiteral(
        "color: %1; background: transparent; border: none; "
        "font-size: 11px; padding: 0px;").arg(color));
}

} // namespace cubeshell
