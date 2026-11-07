#pragma once

#include <QJsonArray>
#include <QMetaType>
#include <QString>

namespace cubeshell {

// Transport completion, command completion and task completion are separate facts.
enum class AiRequestOutcome { Complete, Empty, Truncated, Failed, Cancelled };
struct AiRequestResult {
    quint64 requestId = 0;
    QString text;
    QJsonArray toolCalls;
    QString finishReason;
    QString error;
    AiRequestOutcome outcome = AiRequestOutcome::Complete;
};

enum class AiPhase { Waiting, Responding, AwaitingApproval, Executing,
                     Verifying, Diagnosing, Finalizing, Finished };
enum class AiTurnOutcome { Complete, Incomplete, Failed, Cancelled };
enum class AiStepState { Pending, Running, Succeeded, AllowedFailure,
                         Failed, Skipped, Cancelled, Unknown };

struct AiCommandResult {
    QString cmd;
    QString description;
    int exitCode = -1;
    QString stdoutText;
    QString stderrText;
    qint64 durationMs = 0;
    bool allowFailure = false;
    quint64 stepId = 0;
    QString toolCallId;
    // -1 means the exit status could not be observed, not a confirmed failure.
    bool resultKnown = true;
};

QString aiPhaseLabel(AiPhase phase);
QString aiStepLabel(AiStepState state);
QString aiDurationLabel(qint64 milliseconds);

} // namespace cubeshell

Q_DECLARE_METATYPE(cubeshell::AiRequestResult)
Q_DECLARE_METATYPE(cubeshell::AiCommandResult)
Q_DECLARE_METATYPE(QList<cubeshell::AiCommandResult>)
