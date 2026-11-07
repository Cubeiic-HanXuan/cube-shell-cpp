#pragma once

#include "AiInteractionTypes.h"
#include "CommandSafetyChecker.h"

#include <QElapsedTimer>
#include <QObject>
#include <QSet>

namespace cubeshell {

struct AiConversationEntry {
    enum Kind { Message, Command, Notice } kind = Message;
    quint64 id = 0;
    int revision = 0;
    QString text;
    QString reasoning;
    AiCommand command;
    AiCommandResult result;
    AiStepState state = AiStepState::Pending;
    bool finalAnswer = false;
    bool livePreviewLimited = false;
};

struct AiConversationTurn {
    quint64 id = 0;
    QString userText;
    QString model;
    QList<AiConversationEntry> entries;
    AiPhase phase = AiPhase::Waiting;
    AiTurnOutcome outcome = AiTurnOutcome::Incomplete;
    QString finalText;
    bool localReport = false;
    QElapsedTimer clock;
    qint64 durationMs = 0;
    // View state survives switching SSH tabs; it is never sent to the model.
    bool processCollapsed = false;
    QSet<quint64> expandedEntries;
};

// Session-owned, in-memory transcript. Recording never depends on a visible panel.
class AiConversationModel : public QObject {
    Q_OBJECT
public:
    explicit AiConversationModel(QObject *parent = nullptr);
    const QList<AiConversationTurn> &turns() const { return m_turns; }
    const AiConversationTurn *currentTurn() const;
    bool isBusy() const;
    quint64 beginTurn(const QString &input, const QString &model);
    quint64 beginMessage();
    void appendDelta(quint64 messageId, const QString &text, const QString &reasoning);
    void finalizeMessage(quint64 messageId, const QString &text, bool finalAnswer = false);
    void setPhase(AiPhase phase);
    quint64 addCommand(AiCommand command);
    void setCommandState(quint64 stepId, AiStepState state, const QString &reason = {});
    void setCommandResult(const AiCommandResult &result);
    void appendCommandOutput(quint64 stepId, const QString &text);
    void addNotice(const QString &text);
    void finishTurn(AiTurnOutcome outcome, const QString &text, bool localReport = false);
    QString factualSummary(const QString &reason) const;
    void setProcessCollapsed(quint64 turnId, bool collapsed);
    void setEntryExpanded(quint64 turnId, quint64 entryId, bool expanded);
    void clear();

signals:
    void changed(quint64 turnId);
    void reset();

private:
    AiConversationEntry *entry(quint64 id, AiConversationTurn **turn = nullptr);
    QList<AiConversationTurn> m_turns;
    quint64 m_nextId = 1;
};

} // namespace cubeshell
