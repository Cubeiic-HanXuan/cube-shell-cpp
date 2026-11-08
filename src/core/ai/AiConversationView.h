#pragma once

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
    static constexpr int HistoryPageSize = 20;
    explicit AiConversationView(QWidget *parent = nullptr);
    void setModel(AiConversationModel *model);
    AiConversationModel *model() const { return m_model; }
    void flush(); // explicit full refresh of the loaded page
    void scrollToLatest();
    void loadEarlier();
protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
private:
    void clearWidgets();
    void renderPending(bool force);
    void scheduleRestore();
    bool nearViewport(const QWidget *widget) const;
    void updateJumpButton();
    QPointer<AiConversationModel> m_model;
    QWidget *m_content;
    QVBoxLayout *m_layout;
    QToolButton *m_jump;
    QToolButton *m_loadEarlier;
    QLabel *m_empty;
    QTimer m_renderTimer;
    QTimer m_clockTimer;
    QTimer m_followTimer;
    QTimer m_rebuildTimer;
    QTimer m_historyTimer;
    QHash<quint64, AiTurnWidget *> m_turnWidgets;
    QHash<AiConversationModel *, int> m_positions;
    QHash<AiConversationModel *, int> m_historyStarts;
    QHash<AiConversationModel *, bool> m_followStates;
    QSet<AiConversationModel *> m_observedModels;
    QMetaObject::Connection m_changeConnection, m_resetConnection;
    QSet<quint64> m_dirty;
    int m_firstLoaded = 0;
    int m_pendingPosition = -2; // -2: none; -1: latest; >=0: saved scrollbar value
    quint64 m_generation = 0;
    bool m_following = true;
    bool m_adjusting = false;
    bool m_loadingHistory = false;
};
} // namespace cubeshell
