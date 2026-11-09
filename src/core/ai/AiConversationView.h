#pragma once

// AiConversationView.h — 原生 AI 时间线视图，按需创建轮次控件并合并流式刷新。
// 数据始终保存在会话模型；分页只限制控件数量，不删除历史记录或展开状态。

#include "AiConversationModel.h"
#include <QHash>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>

class QLabel;
class QToolButton;
class QVBoxLayout;

namespace cubeshell {
class AiTurnWidget;

class AiConversationView : public QScrollArea {
    Q_OBJECT
public:
    static constexpr int HistoryPageSize = 20; // 首次打开/跟随输出保留的轮数，每次向前加载也用此步长
    explicit AiConversationView(QWidget *parent = nullptr);
    // 切换前保存原会话的位置，切换后恢复新会话的分页和跟随状态；不取得模型所有权。
    void setModel(AiConversationModel *model);
    AiConversationModel *model() const { return m_model; }
    void flush(); // 强制处理已加载页的待刷新轮次，不受可见范围限制
    void scrollToLatest(); // 恢复跟随并刷新待处理数据，然后滚到布局底部
    void loadEarlier(); // 追加更早一页，以原首个轮次为锚点保持阅读位置
protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
private:
    void clearWidgets(); // 只销毁视图，递增代号使已排队的位置恢复失效
    void renderPending(bool force);
    void scheduleRestore(); // 等本轮布局完成后再恢复滚动条，避免使用旧范围
    bool nearViewport(const QWidget *widget) const; // 上下各预留一屏，降低临界滚动时的延迟
    void updateJumpButton();
    QPointer<AiConversationModel> m_model;
    QWidget *m_content;
    QVBoxLayout *m_layout;
    QToolButton *m_jump;
    QToolButton *m_loadEarlier;
    QLabel *m_empty;

    // --- 分开调度渲染、时钟与布局，避免每个输出片段都重建文档 ---
    QTimer m_renderTimer; // 合并短时间内的 changed 通知
    QTimer m_clockTimer; // 只更新活动轮次的耗时标签，不重新解析正文
    QTimer m_followTimer; // 布局范围变化后延迟跟随到底部
    QTimer m_rebuildTimer; // 合并字体/主题变化，统一重建当前页
    QTimer m_historyTimer; // 滚轮到顶后延迟加载，避免在滚轮事件中直接改布局
    QHash<quint64, AiTurnWidget *> m_turnWidgets;

    // --- 按会话保存的视图状态；模型销毁/重置时移除对应缓存 ---
    QHash<AiConversationModel *, int> m_positions;
    QHash<AiConversationModel *, int> m_historyStarts;
    QHash<AiConversationModel *, bool> m_followStates;
    QSet<AiConversationModel *> m_observedModels; // 生命周期监听只连接一次，切出会话后仍有效
    QMetaObject::Connection m_changeConnection, m_resetConnection;
    QSet<quint64> m_dirty; // 待刷新的轮次；屏外更新保留到进入附近视区时处理
    int m_firstLoaded = 0; // 当前最早加载轮次在模型列表中的索引，不是轮次 ID
    int m_pendingPosition = -2; // -2：无需恢复；-1：滚到底部；非负值：保存的滚动条位置
    quint64 m_generation = 0; // 控件重建代号，防止旧延迟回调影响新会话
    bool m_following = true; // 用户离开底部后关闭自动跟随，不抢占历史阅读位置
    bool m_adjusting = false; // 程序调整位置时，valueChanged 不改变用户的跟随意图
    bool m_loadingHistory = false; // 加载早期历史时暂停底部跟随与旧页回收
};
} // namespace cubeshell
