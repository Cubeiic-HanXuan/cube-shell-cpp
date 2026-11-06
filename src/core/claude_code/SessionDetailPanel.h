#pragma once

// SessionDetailPanel.h — 单个历史会话的详情控制器。
//
// 组织元数据、虚拟时间线、对话目录、搜索与导出；只消费后端返回的只读快照。
// 连续读取、搜索后续页与跳转末尾共用分页入口，切换会话后丢弃旧请求的结果。

#include <QWidget>
#include "claude_code/ClaudeSessionStore.h"

class QAction;
class QLabel;
class QPushButton;
class QToolButton;
class QLineEdit;
class QListWidget;
class QButtonGroup;
class QFrame;
class QSplitter;

namespace cubeshell {
class ClaudeCodeBackend;
class SessionTimelineView;

class SessionDetailPanel : public QWidget {
    Q_OBJECT
public:
    explicit SessionDetailPanel(ClaudeCodeBackend *backend, QWidget *parent = nullptr);
    // 清空旧会话并读取首批内容；后续由滚动、搜索或「读取全部」继续分页。
    void openSession(const ClaudeSessionInfo &session);
    // 清空快照并递增请求编号，使已排队的读取 / 导出回调失效。
    void reset();
    void setNavigationEnabled(bool previous, bool next);
    void focusSearch();
    const ClaudeSessionData &data() const { return *m_data; }
    SessionTimelineView *timeline() const { return m_timeline; }

signals:
    void backRequested();
    void refreshRequested();
    void navigateRequested(int delta);
    void resumeRequested(const cubeshell::ClaudeSessionInfo &session);

protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildUi();
    void applyTheme();
    // 自动窄屏布局与用户手动显示目录的选择分开处理。
    void applyResponsiveLayout();
    void loadMore();
    // 接收当前请求的快照，再按首屏补齐 / 连续读取状态决定是否继续。
    void onPage(quint64 source, quint64 request, const ClaudeSessionPage &page,
                const ClaudeSessionSnapshot &snapshot);
    void updateMetadata();
    void updateDirectory();
    void setFilter(int filter);
    // 全文搜索先读完文件快照，再按匹配条目循环定位，不只搜索已加载的首批内容。
    void findText(bool backwards);
    void exportFile();
    // 暂停只阻止后续页，不打断在途请求，也不取消同一存储的独立导出任务。
    void readAllOrPause();

    ClaudeCodeBackend *m_backend;
    ClaudeSessionInfo m_session;
    ClaudeSessionSnapshot m_data = std::make_shared<ClaudeSessionData>();
    quint64 m_source = 0;        // 数据源身份，本地 / 不同 SSH 连接间切换时变化
    quint64 m_request = 0;       // 详情页读取请求编号，屏蔽旧页及延迟续读回调
    quint64 m_exportRequest = 0; // 导出独立编号，不与详情分页相互覆盖
    qint64 m_offset = 0;
    bool m_pending = false;     // 同一详情页只允许一个分页请求在途
    bool m_eof = false;
    bool m_loadAll = false;     // 连续读到文件快照末尾，可由用户暂停
    bool m_autoFill = false;    // 首次打开时补足少量可展示条目，避免首屏只有元数据
    bool m_jumpToEnd = false;   // 完整读取后再滚到最新内容
    bool m_searchAfterLoad = false;
    bool m_searchBackwards = false;
    bool m_directoryWanted = true;
    bool m_directoryOverride = false; // 手动切换目录后，不再仅按窄屏阈值自动隐藏
    bool m_rebuildingView = false; // 快照更新期间不响应时间线的 needMore，防止重入
    bool m_themePending = false;
    int m_searchIndex = -1; // 当前命中的条目索引；搜索按条目而非单个词的位置计数
    QString m_loadError;
    QLabel *m_title = nullptr;
    QLabel *m_project = nullptr;
    QLabel *m_metadata = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_matchLabel = nullptr;
    QLabel *m_directoryCount = nullptr;
    QFrame *m_header = nullptr;
    QFrame *m_searchBar = nullptr;
    QFrame *m_directoryPanel = nullptr;
    QSplitter *m_splitter = nullptr;
    QToolButton *m_previous = nullptr;
    QToolButton *m_next = nullptr;
    QToolButton *m_directoryToggle = nullptr;
    QToolButton *m_failures = nullptr;
    QPushButton *m_resume = nullptr;
    QPushButton *m_loadMore = nullptr;
    QToolButton *m_readAll = nullptr;
    QAction *m_exportAction = nullptr;
    QButtonGroup *m_filterGroup = nullptr;
    QLineEdit *m_search = nullptr;
    SessionTimelineView *m_timeline = nullptr;
    QListWidget *m_directory = nullptr;
};
} // namespace cubeshell
