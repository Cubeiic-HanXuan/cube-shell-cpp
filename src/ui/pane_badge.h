#pragma once

// pane_badge.h — 文件浏览器路径栏左侧的“分屏 N”徽章。
//
// 多分屏时左栏只显示当前活动标签的目录，用户容易搞不清看的是哪个分屏的文件树。
// 早期版本在左栏顶部放了一整行 QLabel 标题栏（“分屏 2 · SSH: host”），既占
// 高度又多出一条 splitter 拖动手柄。改为在路径栏内嵌一个圆角小徽章：零额外
// 高度，完整信息走 tooltip，单分屏时整个隐藏。
//
// SftpBrowserWidget / LocalFileBrowserWidget 共用，故做成 header-only。

#include <QColor>
#include <QLabel>
#include <QString>
#include <QWidget>

namespace cubeshell {

// 分屏配色：按序号循环取用，让徽章颜色与分屏形成稳定关联。
inline QColor paneBadgeColor(int paneNumber)
{
    static const QColor kColors[] = {
        QColor(0x3d, 0x8b, 0xd4),   // 蓝
        QColor(0x2e, 0xa0, 0x63),   // 绿
        QColor(0xd4, 0x7b, 0x2f),   // 橙
        QColor(0x9b, 0x59, 0xb6),   // 紫
        QColor(0xc0, 0x50, 0x5a),   // 红
        QColor(0x17, 0x9b, 0x9b),   // 青
    };
    constexpr int kCount = int(sizeof(kColors) / sizeof(kColors[0]));
    if (paneNumber < 1)
        return kColors[0];
    return kColors[(paneNumber - 1) % kCount];
}

// 创建徽章标签（初始隐藏，由 updatePaneBadge 决定是否显示）。
inline QLabel *createPaneBadge(QWidget *parent)
{
    auto *badge = new QLabel(parent);
    badge->setAlignment(Qt::AlignCenter);
    badge->setVisible(false);
    return badge;
}

// 刷新徽章：paneNumber 为分屏序号（1-based），totalPanes ≤ 1 时隐藏。
// tabTitle 用于 tooltip，展示完整的“分屏 N · 标签名”。
inline void updatePaneBadge(QLabel *badge, int paneNumber, int totalPanes,
                            const QString &tabTitle)
{
    if (!badge)
        return;
    if (paneNumber < 1 || totalPanes <= 1) {
        badge->setVisible(false);
        return;
    }
    const QColor c = paneBadgeColor(paneNumber);
    badge->setText(QString::number(paneNumber));
    badge->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  background-color: %1;"
        "  color: white;"
        "  border-radius: 3px;"
        "  padding: 0px 5px;"
        "  font-size: 10px;"
        "  font-weight: bold;"
        "}").arg(c.name()));
    badge->setToolTip(tabTitle.isEmpty()
                          ? QObject::tr("分屏 %1").arg(paneNumber)
                          : QObject::tr("分屏 %1 · %2").arg(paneNumber).arg(tabTitle));
    badge->setVisible(true);
}

} // namespace cubeshell
