#pragma once

// SessionWidgets.h — 会话界面的共享配色、图标、行控件与目录绘制。
//
// SessionTimelineView 管理数据和交互状态，本文件只负责单行呈现并上报操作。
// 行控件可随滚动销毁重建，因此展开状态使用条目索引/稳定键存放在 ViewState 中。

#include "claude_code/ClaudeSessionData.h"
#include "widgets/ConversationWidgets.h"
#include <QColor>
#include <QIcon>
#include <QStyledItemDelegate>
#include <QWidget>

class QAbstractButton;
class QToolButton;
class QVBoxLayout;
class QUrl;

namespace cubeshell {
namespace sessionui {
QString timeLabel(qint64 milliseconds, bool date = false);
QString durationLabel(qint64 milliseconds);
QString toolSummary(const ClaudeSessionEntry &entry);
QString messagePreview(const QString &text);

// 交互状态独立于控件生命周期：滚出视口后销毁控件，返回时仍保持原来的展开状态。
struct ViewState {
    bool expandAll = false;
    QSet<int> expandedEntries; // 相对 expandAll 的例外集合；默认全展开时，集合内条目反而收起
    QSet<int> collapsedTurns; // 以轮次首条目的索引为键，日志追加后仍稳定
    QSet<int> fullMessages;   // 已展开的长消息
    QSet<int> rawMessages;    // 显示包含附加上下文的原文
    QSet<int> parameters;     // 显示工具输入参数
    QSet<QString> expandedCode; // 「条目索引:块标识」，区分输入 / 输出 / 修改前后等代码块
    int highlightedEntry = -1;
    QString query;
};
// 同一日志条目可能对应标题 / 正文等不同行，键必须同时编码行类型与条目索引。
struct Row {
    enum Kind { UserMessage, AssistantHeading, ProcessHeading, ProcessEntry, ReplyMessage, EmptyReply };
    Kind kind = ProcessEntry;
    int entry = 0;
    int turn = 0;
    quint64 key() const { return (quint64(kind + 1) << 32) | quint32(entry + 1); }
};
// 行控件上报操作意图，由时间线统一修改 ViewState 后重建界面。
enum Action { ToggleEntry, ToggleTurn, ToggleParameters, ToggleMessage, ToggleRaw, ToggleCode };

// 详情页写入目录项的数据角色，与目录委托的自绘读取保持一致。
enum DirectoryRole { QuestionRole = Qt::UserRole + 1, ReplyRole, StatsRole, TimeRole, FailureRole, HasReplyRole };
}

class SessionRowWidget : public QWidget {
    Q_OBJECT
public:
    SessionRowWidget(const sessionui::Row &row, const ClaudeSessionSnapshot &data,
                     const ClaudeSessionTurn &turn, const sessionui::ViewState &state,
                     const sessionui::Colors &colors, QWidget *parent = nullptr);
    // 根据实际富文本布局计算高度，供虚拟时间线校正估值。
    int preferredHeight(int width);
    const sessionui::Row &row() const { return m_row; }
    // 高亮可见文本并返回最靠上的匹配区域，坐标相对于此行控件。
    QRect highlight(const QString &query);

signals:
    void actionRequested(int action, int entry, const QString &key);
    void metricsChanged();
    void copied();
    void externalLinkRequested(const QUrl &url);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void buildMessage(const ClaudeSessionEntry &entry, bool user);
    void buildStep(const ClaudeSessionEntry &entry);
    void addMarkdown(QVBoxLayout *layout, const QString &text, int entry);
    void addCode(QVBoxLayout *layout, const QString &title, const QString &text,
                 const QString &language, const QString &key);
    sessionui::Row m_row;
    ClaudeSessionSnapshot m_data;
    ClaudeSessionTurn m_turn;
    sessionui::ViewState m_state;
    sessionui::Colors m_colors;
    QVBoxLayout *m_layout = nullptr;
};

// 对话目录使用委托自绘卡片，不为每个历史轮次创建一组子控件。
class SessionDirectoryDelegate : public QStyledItemDelegate {
public:
    explicit SessionDirectoryDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
} // namespace cubeshell
