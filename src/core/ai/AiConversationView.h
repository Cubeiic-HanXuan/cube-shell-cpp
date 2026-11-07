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
    explicit AiConversationView(QWidget *parent = nullptr);
    void setModel(AiConversationModel *model);
    AiConversationModel *model() const { return m_model; }
    void flush();
    void scrollToLatest();
protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void clearWidgets();
    void updateJumpButton();
    QPointer<AiConversationModel> m_model;
    QWidget *m_content;
    QVBoxLayout *m_layout;
    QToolButton *m_jump;
    QLabel *m_empty;
    QTimer m_renderTimer;
    QTimer m_clockTimer;
    QHash<quint64, AiTurnWidget *> m_turnWidgets;
    QHash<AiConversationModel *, int> m_positions;
    QSet<AiConversationModel *> m_observedModels;
    QMetaObject::Connection m_changeConnection, m_resetConnection;
    QSet<quint64> m_dirty;
    bool m_following = true;
    bool m_adjusting = false;
};
} // namespace cubeshell
