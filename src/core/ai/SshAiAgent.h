#pragma once

#include "AiChatWorker.h"
#include "AiConversationModel.h"
#include "ChatHistory.h"

#include <QThread>
#include <atomic>
#include <functional>

class QTermWidget;

namespace cubeshell {
class CommandExecutor;
class TerminalExecutor;

// Optional execution adapter also permits deterministic, network-free tests.
using AiCommandRunner = std::function<AiCommandResult(const AiCommand &)>;

class AiCommandExecThread : public QThread {
    Q_OBJECT
public:
    AiCommandExecThread(CommandExecutor *executor, TerminalExecutor *terminalExecutor,
                        const QList<AiCommand> &commands, QObject *parent = nullptr,
                        AiCommandRunner runner = {});
    void requestStop();
signals:
    void commandStarted(const cubeshell::AiCommand &command);
    void commandFinished(const cubeshell::AiCommandResult &result);
    void allFinished(const QList<cubeshell::AiCommandResult> &results);
    void execError(const QString &message);
    void progress(int current, int total);
protected:
    void run() override;
private:
    CommandExecutor *m_executor;
    TerminalExecutor *m_terminalExecutor;
    QList<AiCommand> m_commands;
    AiCommandRunner m_runner;
    std::atomic<bool> m_stopFlag{false};
};

class SshAiAgent : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxVerifyRounds = 3;
    static constexpr int kMaxDiagnoseRounds = 3;
    explicit SshAiAgent(CommandExecutor *executor,
                        const AiPreferences &prefs = AiPreferences::load(),
                        QObject *parent = nullptr, AiCommandRunner runner = {});
    ~SshAiAgent() override;
    void setPreferences(const AiPreferences &prefs);
    AiPreferences preferences() const { return m_prefs; }
    void setTerminal(QTermWidget *terminal);
    TerminalExecutor *terminalExecutor() const { return m_terminalExecutor; }
    void setExecutor(CommandExecutor *executor);
    void connectionLost();
    void processUserInput(const QString &input);
    void executeCommands(const QList<AiCommand> &commands);
    void rejectPendingCommands(const QString &reason = QString());
    QList<AiCommand> pendingCommands() const { return m_pendingCommands; }
    void diagnoseError(const QString &command, const QString &stdoutText,
                       const QString &stderrText, int exitCode);
    void stop();
    void shutdown(int waitMs = 2000);
    void clearConversation();
    void setServerProfile(const QString &profile);
    ChatHistory *conversation() { return &m_conversation; }
    AiConversationModel *displayModel() const { return m_display; }
    bool isBusy() const { return m_display->isBusy() || m_execThread; }
    static QJsonArray toolsDefinition();
    static QList<AiCommand> extractCommandsFromText(const QString &text);

signals:
    void commandReady(const QList<cubeshell::AiCommand> &commands);
    void executionStarted(const QString &cmd);
    void executionFinished(const cubeshell::AiCommandResult &result);
    void aiMessage(const QString &reasoning, const QString &content);
    void errorOccurred(const QString &message);
    void thinkingStarted();
    void thinkingFinished();
    void executionProgress(int current, int total);
    void diagnosingStarted();
    void taskSummary(const QString &summary);
    void commandOutput(const QString &text);
    void turnFinished();
    void executionIdle();

private:
    void requestFollowup(const QString &prompt, AiPhase phase);
    void startRequest(AiPhase phase);
    void onAiCompleted(const AiRequestResult &result);
    void onAllExecFinished();
    void finishTurn(AiTurnOutcome outcome, const QString &text, bool local = false);
    void finalizeTask(const QString &reason);
    void finishLocally(AiTurnOutcome outcome, const QString &reason);
    void cancelTask(AiTurnOutcome outcome, const QString &reason);
    void closeToolBatch();
    QList<AiCommand> parseToolCalls(const QJsonArray &calls, QString *error);
    QList<AiCommand> checkCommandsSafety(const QList<AiCommand> &commands) const;
    void verifyGoalCompletion();
    QString buildSystemPrompt() const;

    CommandExecutor *m_executor;
    AiPreferences m_prefs;
    AiChatWorker *m_aiWorker;
    ChatHistory m_conversation;
    CommandSafetyChecker m_safetyChecker;
    TerminalExecutor *m_terminalExecutor;
    AiConversationModel *m_display;
    AiCommandExecThread *m_execThread = nullptr;
    AiCommandRunner m_runner;
    QString m_originalGoal;
    int m_goalVerifyCount = 0;
    int m_diagnoseCount = 0;
    bool m_finalizing = false;
    bool m_cancelled = false;
    quint64 m_generation = 0;
    quint64 m_requestId = 0;
    quint64 m_messageId = 0;
    quint64 m_runningStep = 0;
    QList<AiCommand> m_pendingCommands;
    QList<AiCommand> m_batchCommands;
    QJsonArray m_pendingToolCalls;
    QString m_serverProfile;
};
} // namespace cubeshell
