#pragma once

// StatusBoxItem — MobaXterm 风格状态栏小方块组件：彩色图标 + 文字。
// 对应Python: cube-shell.py::StatusBoxItem（L505-552）

#include <QFrame>
#include <QString>

class QLabel;

namespace cubeshell {

class StatusBoxItem : public QFrame {
    Q_OBJECT
public:
    explicit StatusBoxItem(const QString &iconColor, const QString &iconChar,
                           const QString &text = QStringLiteral("—"),
                           QWidget *parent = nullptr);

    // 更新显示文字（兼容 QLabel 的 setText API）。
    void setText(const QString &text);
    QString text() const;

    // 动态改变文字颜色（用于数值高亮）。
    void setTextColor(const QString &color);

private:
    QLabel *m_iconLabel = nullptr;
    QLabel *m_textLabel = nullptr;
};

} // namespace cubeshell
