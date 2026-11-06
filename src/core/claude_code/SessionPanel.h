#pragma once

// SessionPanel.h — Claude Code 会话 Tab：分组列表与详情页的导航容器。
//
// 列表负责搜索、分组和选中态；SessionDetailPanel 负责按需读取与展示历史。
// 用数据源编号和请求编号隔离异步结果；恢复命令交给上层按本地 / 远程来源路由。

#include <QWidget>
#include <QHash>
#include "claude_code/ClaudeCodeBackend.h"

class QLabel;
class QAction;
class QPushButton;
class QLineEdit;
class QComboBox;
class QTreeWidget;
class QTreeWidgetItem;
class QStackedWidget;

namespace cubeshell {
class SessionDetailPanel;

class SessionPanel : public QWidget {
    Q_OBJECT
public:
    explicit SessionPanel(ClaudeCodeBackend *backend, QWidget *parent = nullptr);
public slots:
    // 异步刷新当前数据源的列表；已有请求在途时不重复提交。
    void refresh();
signals:
    // 仅生成命令；由 ClaudeCodePanel 按来源转发给本地或 SSH 终端入口。
    void openTerminalRequested(const QString &command);
protected:
    void changeEvent(QEvent *event) override;
private:
    void buildUi();
    // 样式与字体尺寸分开更新，分组下拉框及双行列表随应用字体自适应。
    void applyTheme();
    void updateMetrics();
    // 重建分组树时保留展开态；项目默认收起，时间分组默认展开。
    void renderTree();
    void openItem(QTreeWidgetItem *item);
    void openSession(int index);
    void resumeSession(const ClaudeSessionInfo &session);
    // 切换来源后清除旧列表 / 详情及分组状态，不让本地和远程历史混用。
    void sourceChanged();
    void showContextMenu(const QPoint &position);
    QString resumeCommand(const ClaudeSessionInfo &session) const;
    // 只有来源和请求编号都匹配时才接收列表，并按文件身份恢复选中项。
    void onSessionsLoaded(quint64 source, quint64 request,
                          const QList<ClaudeSessionInfo> &sessions, const QString &error);
    QList<int> filteredSessions() const; // 返回 m_sessions 中的索引，供列表与详情导航共用
    int selectedIndex() const;

    ClaudeCodeBackend *m_backend;
    QList<ClaudeSessionInfo> m_sessions;
    QHash<QString, bool> m_expandedGroups; // 键带 project: / time: 前缀，避免两种分组相互覆盖
    quint64 m_source = 0;
    quint64 m_request = 0;
    bool m_pending = false;
    int m_detailIndex = -1;
    QString m_reloadDetail; // 详情页请求刷新时记录相对路径，列表更新后重新定位
    QString m_error;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_listPage = nullptr;
    SessionDetailPanel *m_detail = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_grouping = nullptr;
    QPushButton *m_btnRefresh = nullptr;
    QPushButton *m_resume = nullptr;
    QAction *m_searchIcon = nullptr;
    bool m_stylePending = false;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_count = nullptr;
    QLabel *m_statusLabel = nullptr;
};
} // namespace cubeshell
