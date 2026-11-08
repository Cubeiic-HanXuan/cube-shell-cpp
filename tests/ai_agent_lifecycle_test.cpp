#include "ai_test_support.h"
#include "ai/AiChatWorker.h"
#include "ai/ChatHistory.h"
#include "ai/SshAiAgent.h"

#include <QDebug>
#include <QNetworkProxy>
#include <QSemaphore>
#include <QStandardPaths>
#include <atomic>

using namespace cubeshell;
using namespace aitest;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { qWarning() << "FAIL" << #x << "line" << __LINE__; ++failures; } } while (0)

static AiCommandResult success(const AiCommand &)
{
    AiCommandResult result; result.exitCode = 0; result.stdoutText = QStringLiteral("synthetic output"); return result;
}

static void testWorkerResponses()
{
    struct Case { Response response; bool stream; AiRequestOutcome outcome; };
    QList<Case> cases{
        {text(QStringLiteral("最终结果 中文")), true, AiRequestOutcome::Complete},
        {text(QStringLiteral("最终结果 中文"), false), false, AiRequestOutcome::Complete},
        {text(QStringLiteral("最终结果 中文"), false), true, AiRequestOutcome::Complete},
        {{"data: [DONE]\n\n"}, true, AiRequestOutcome::Empty},
        {{frame({{"reasoning_content", "reasoning only"}}, "stop") + "data: [DONE]\n\n"}, true, AiRequestOutcome::Empty},
        {text({}, true, "length"), true, AiRequestOutcome::Truncated},
        {text("partial", true, "content_filter"), true, AiRequestOutcome::Failed},
        {text("unknown", true, "unexpected_reason"), true, AiRequestOutcome::Failed},
        {{"event: error\ndata: {\"error\":{\"message\":\"failure\"}}\n\n"}, true, AiRequestOutcome::Failed},
        {{"{\"error\":{\"message\":\"failure\"}}", "application/json"}, false, AiRequestOutcome::Failed},
        {{"data: malformed\n\ndata: [DONE]\n\n"}, true, AiRequestOutcome::Failed},
        {{"server error", "application/json", 500}, true, AiRequestOutcome::Failed},
        {text(QStringLiteral("兼容无完成原因"), true, {}), true, AiRequestOutcome::Complete},
    };
    for (const auto &test : cases) {
        Server server; CHECK(server.isListening());
        server.respond = [test](const auto &, int) { return test.response; };
        AiChatWorker worker; worker.setPreferences(server.preferences(test.stream));
        QList<AiRequestResult> results;
        QObject::connect(&worker, &AiChatWorker::requestFinished, &worker, [&](const AiRequestResult &r) { results.append(r); });
        worker.start({});
        CHECK(waitFor([&] { return !results.isEmpty(); }));
        CHECK(results.size() == 1);
        if (!results.isEmpty()) CHECK(results.first().outcome == test.outcome);
        CHECK(!worker.isRunning());
    }
    // UTF-8, CRLF and tool JSON split at every byte, with a missing final blank line.
    Server server;
    auto response = text(QStringLiteral("分包：完成 ✓")); response.chunkSize = 1;
    server.respond = [response](const auto &, int) { return response; };
    AiChatWorker worker; worker.setPreferences(server.preferences());
    QString streamed; QList<AiRequestResult> results;
    QObject::connect(&worker, &AiChatWorker::deltaReceived, &worker, [&](const QString &s, const QString &) { streamed += s; });
    QObject::connect(&worker, &AiChatWorker::requestFinished, &worker, [&](const AiRequestResult &r) { results.append(r); });
    worker.start({}); CHECK(waitFor([&] { return results.size() == 1; }));
    CHECK(streamed == QStringLiteral("分包：完成 ✓"));
    CHECK(results.value(0).text == streamed);
}

static void testWorkerCancelAndReentry()
{
    Server server;
    server.respond = [](const auto &, int index) {
        if (index > 1) return text("SECOND");
        Response response{frame({{"content", "about to run"}, {"tool_calls", QJsonArray{tool("pending")}}})};
        response.holdOpen = true; return response;
    };
    AiChatWorker worker; worker.setPreferences(server.preferences());
    QList<AiRequestResult> results; int completed = 0;
    QObject::connect(&worker, &AiChatWorker::completed, &worker, [&] { ++completed; });
    QObject::connect(&worker, &AiChatWorker::deltaReceived, &worker, [&](const QString &text, const QString &) {
        if (text == "about to run") worker.stop();
    });
    QObject::connect(&worker, &AiChatWorker::requestFinished, &worker, [&](const AiRequestResult &r) {
        results.append(r);
        if (results.size() == 1) worker.start({}); // synchronous replacement from completion
    });
    worker.start({}); CHECK(waitFor([&] { return results.size() == 2; }));
    CHECK(results.value(0).outcome == AiRequestOutcome::Cancelled);
    CHECK(results.value(1).text == "SECOND");
    CHECK(completed == 1); CHECK(server.requests.size() == 2);
}

static void approveAutomatically(SshAiAgent &agent)
{
    QObject::connect(&agent, &SshAiAgent::commandReady, &agent, [&agent](const QList<AiCommand> &commands) {
        agent.executeCommands(commands);
    });
}

static void testFinalizationBudget()
{
    Server server;
    server.respond = [](const QJsonObject &, int index) {
        if (index <= 4) return tools({tool(QStringLiteral("call_%1").arg(index))});
        return text("FINAL_REPORT");
    };
    std::atomic<int> executions{0};
    SshAiAgent agent(nullptr, server.preferences(), nullptr, [&](const AiCommand &command) {
        ++executions; return success(command);
    });
    approveAutomatically(agent);
    agent.processUserInput("GOAL"); CHECK(waitFor([&] { return !agent.isBusy(); }));
    CHECK(executions == 4); CHECK(server.requests.size() == 5);
    CHECK(!server.requests.last().contains("tools"));
    CHECK(agent.displayModel()->currentTurn()->finalText == "FINAL_REPORT");
    CHECK(agent.displayModel()->currentTurn()->outcome == AiTurnOutcome::Incomplete);
    QSet<QString> pending;
    for (const auto &v : server.requests.last().value("messages").toArray()) {
        const auto message = v.toObject();
        for (const auto &call : message.value("tool_calls").toArray()) pending.insert(call.toObject().value("id").toString());
        if (message.value("role") == "tool") CHECK(pending.remove(message.value("tool_call_id").toString()));
    }
    CHECK(pending.isEmpty());
}

static void testNewGoalAndNonstream()
{
    Server server;
    server.respond = [](const QJsonObject &request, int index) {
        if (index % 2) {
            auto call = tool(QStringLiteral("call_%1").arg(index)); call.remove("index");
            QJsonObject message{{"role", "assistant"}, {"content", ""}, {"tool_calls", QJsonArray{call}}};
            return Response{QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", message}, {"finish_reason", "tool_calls"}}}}}).toJson(), "application/json"};
        }
        CHECK(request.value("messages").toArray().last().toObject().value("content").toString().contains(index == 2 ? "GOAL_A" : "GOAL_B"));
        return text(index == 2 ? "REPORT_A" : "REPORT_B", false);
    };
    SshAiAgent agent(nullptr, server.preferences(false), nullptr, success);
    approveAutomatically(agent);
    agent.processUserInput("GOAL_A"); CHECK(waitFor([&] { return !agent.isBusy(); }));
    agent.processUserInput("GOAL_B"); CHECK(waitFor([&] { return !agent.isBusy(); }));
    CHECK(server.requests.size() == 4);
    CHECK(agent.displayModel()->turns().size() == 2);
    CHECK(agent.displayModel()->turns().first().finalText == "REPORT_A");
    CHECK(agent.displayModel()->turns().last().finalText == "REPORT_B");
}

static void testSummaryFallbacks()
{
    for (int mode = 0; mode < 3; ++mode) {
        Server server;
        server.respond = [mode](const QJsonObject &, int index) {
            if (index == 1) return tools({tool("command")});
            if (index == 2) return Response{frame({{"reasoning_content", "thinking"}}, "stop") + "data: [DONE]\n\n"};
            if (mode == 0) return Response{"data: [DONE]\n\n"};
            if (mode == 1) return tools({tool("must_not_execute")});
            return text("最终报告\n```bash\nprintf do_not_run\n```", true, "length");
        };
        std::atomic<int> executions{0};
        SshAiAgent agent(nullptr, server.preferences(), nullptr, [&](const AiCommand &c) { ++executions; return success(c); });
        approveAutomatically(agent);
        agent.processUserInput("report"); CHECK(waitFor([&] { return !agent.isBusy(); }));
        CHECK(executions == 1); CHECK(server.requests.size() == 3);
        CHECK(!server.requests.last().contains("tools"));
        CHECK(agent.displayModel()->currentTurn()->localReport);
        CHECK(!agent.displayModel()->currentTurn()->finalText.isEmpty());
    }
}

static void testMultipleCallsAndApproval()
{
    Server server;
    server.respond = [](const auto &, int index) {
        return index == 1 ? tools({tool("one", "printf first", 0), tool("two", "printf second", 1)}) : text("DONE");
    };
    std::atomic<int> executions{0};
    SshAiAgent agent(nullptr, server.preferences(), nullptr, [&](const AiCommand &c) { ++executions; return success(c); });
    QObject::connect(&agent, &SshAiAgent::commandReady, &agent, [&](const QList<AiCommand> &commands) {
        CHECK(commands.size() == 2); CHECK(commands.at(0).stepId != commands.at(1).stepId);
        agent.executeCommands({commands.last()}); // explicitly skip first call
        agent.executeCommands(commands);          // duplicate approval must not dispatch again
    });
    agent.processUserInput("multi"); CHECK(waitFor([&] { return !agent.isBusy(); }));
    CHECK(executions == 1);
    int toolResults = 0;
    for (const auto &value : server.requests.last().value("messages").toArray())
        toolResults += value.toObject().value("role") == "tool";
    CHECK(toolResults == 2);
    int activities = 0, skipped = 0, succeeded = 0;
    for (const auto &entry : agent.displayModel()->currentTurn()->entries) {
        if (entry.kind == AiConversationEntry::Activity) {
            ++activities;
            CHECK(entry.activity.name == "execute_ssh_commands");
            CHECK(!entry.activity.input.isEmpty()); CHECK(!entry.activity.output.isEmpty());
            if (entry.activity.callId == "one") CHECK(entry.state == AiStepState::Skipped);
            if (entry.activity.callId == "two") CHECK(entry.state == AiStepState::Succeeded);
        }
        if (entry.kind == AiConversationEntry::Command) {
            CHECK(entry.parentEntryId != 0);
            skipped += entry.state == AiStepState::Skipped;
            succeeded += entry.state == AiStepState::Succeeded;
        }
    }
    CHECK(activities == 2); CHECK(skipped == 1); CHECK(succeeded == 1);

    Server bad;
    auto badTool = tool("bad"); badTool.insert("function", QJsonObject{{"name", "execute_ssh_commands"}, {"arguments", "{"}});
    bad.respond = [badTool](const auto &, int) { return tools({badTool, tool("valid", "printf later", 1)}); };
    SshAiAgent broken(nullptr, bad.preferences(), nullptr, [&](const AiCommand &c) { ++executions; return success(c); });
    approveAutomatically(broken); broken.processUserInput("invalid");
    CHECK(waitFor([&] { return !broken.isBusy(); })); CHECK(executions == 1);
    CHECK(broken.displayModel()->currentTurn()->localReport);
}

static void testCancelSingleFlightAndSafety()
{
    Server server; server.respond = [](const auto &, int) { return tools({tool("cancel")}); };
    QSemaphore release;
    std::atomic<bool> entered{false};
    SshAiAgent agent(nullptr, server.preferences(), nullptr, [&](const AiCommand &c) {
        entered = true; release.acquire(); return success(c);
    });
    int completions = 0;
    QObject::connect(&agent, &SshAiAgent::turnFinished, &agent, [&] { ++completions; });
    approveAutomatically(agent); agent.processUserInput("first");
    CHECK(waitFor([&] { return entered.load(); }));
    agent.stop(); CHECK(agent.isBusy());
    agent.processUserInput("must not overlap");
    release.release(); CHECK(waitFor([&] { return !agent.isBusy(); }));
    drain(); CHECK(server.requests.size() == 1); CHECK(completions == 1);
    CHECK(agent.displayModel()->currentTurn()->outcome == AiTurnOutcome::Cancelled);
    CHECK(agent.displayModel()->turns().size() == 1);

    Server blocked; blocked.respond = [](const auto &, int) { return tools({tool("prohibited", "rm -rf /")}); };
    int count = 0;
    SshAiAgent safety(nullptr, blocked.preferences(), nullptr, [&](const AiCommand &c) { ++count; return success(c); });
    approveAutomatically(safety); safety.processUserInput("synthetic forbidden fixture");
    CHECK(waitFor([&] { return !safety.isBusy(); })); CHECK(count == 0);
    CHECK(safety.displayModel()->currentTurn()->localReport);
}

static void testDiagnosisLimitAndHistory()
{
    Server server; server.respond = [](const QJsonObject &request, int index) {
        return request.contains("tools") ? tools({tool(QStringLiteral("f%1").arg(index))}) : text("FAILURE_REPORT");
    };
    int executions = 0;
    SshAiAgent agent(nullptr, server.preferences(), nullptr, [&](const AiCommand &) {
        ++executions; AiCommandResult result; result.exitCode = 2; result.stderrText = "synthetic failure"; return result;
    });
    approveAutomatically(agent); agent.processUserInput("diagnose");
    CHECK(waitFor([&] { return !agent.isBusy(); }));
    CHECK(executions == 4); CHECK(server.requests.size() == 5);
    CHECK(agent.displayModel()->currentTurn()->finalText == "FAILURE_REPORT");
    ChatHistory history({}, 1);
    history.addUserMessage("old");
    history.addAssistantToolCalls({}, QJsonArray{QJsonObject{{"id", "a"}}, QJsonObject{{"id", "b"}}});
    history.addToolResult("a", "password=fixture_secret"); history.addToolResult("b", "complete");
    const auto messages = history.buildMessages();
    CHECK(messages.size() == 3);
    CHECK(messages.first().toObject().value("role") == "assistant");
    CHECK(!QJsonDocument(messages).toJson().contains("fixture_secret"));
}

static void testReportCodeAndCancellation()
{
    Server server;
    const QString report = "REPORT_WITH_EXAMPLE\n```bash\nprintf do_not_execute\n```";
    server.respond = [report](const auto &, int index) { return index == 1 ? tools({tool("once")}) : text(report); };
    int executions = 0, finished = 0;
    SshAiAgent agent(nullptr, server.preferences(), nullptr, [&](const AiCommand &c) { ++executions; return success(c); });
    approveAutomatically(agent);
    QObject::connect(&agent, &SshAiAgent::turnFinished, &agent, [&] { ++finished; });
    agent.processUserInput("report code example"); CHECK(waitFor([&] { return !agent.isBusy(); }));
    CHECK(executions == 1); CHECK(finished == 1); CHECK(server.requests.size() == 2);
    CHECK(agent.displayModel()->currentTurn()->finalText == report);

    Server partial;
    partial.respond = [](const auto &, int index) {
        if (index > 1) return text("NEW_TASK_REPORT");
        Response response{frame({{"content", "partial"}, {"tool_calls", QJsonArray{tool("unapproved")}}})};
        response.holdOpen = true; return response;
    };
    SshAiAgent cancelled(nullptr, partial.preferences(), nullptr, [&](const AiCommand &c) { ++executions; return success(c); });
    approveAutomatically(cancelled);
    QObject::connect(&cancelled, &SshAiAgent::aiMessage, &cancelled, [&](const QString &, const QString &content) {
        if (content == "partial") cancelled.connectionLost();
    });
    cancelled.processUserInput("old"); CHECK(waitFor([&] { return !cancelled.isBusy(); }));
    CHECK(cancelled.displayModel()->currentTurn()->finalText.contains(QStringLiteral("连接中断")));
    CHECK(executions == 1); CHECK(partial.requests.size() == 1);
    cancelled.processUserInput("new");
    AiRequestResult late; late.requestId = 1; late.text = "STALE_REPORT";
    cancelled.findChild<AiChatWorker *>()->requestFinished(late);
    CHECK(waitFor([&] { return !cancelled.isBusy(); }));
    CHECK(cancelled.displayModel()->currentTurn()->finalText == "NEW_TASK_REPORT");

    Server summary;
    summary.respond = [](const QJsonObject &request, int) {
        CHECK(!request.contains("tools")); CHECK(!request.contains("tool_choice"));
        CHECK(!request.contains("functions")); CHECK(!request.contains("function_call"));
        return text("SUMMARY_ONLY");
    };
    AiChatWorker worker; worker.setPreferences(summary.preferences()); worker.setSummaryOnly(true);
    worker.setExtraBody({{"tools", SshAiAgent::toolsDefinition()}, {"tool_choice", "required"},
                         {"functions", QJsonArray{}}, {"function_call", "auto"}});
    worker.start({}); CHECK(waitFor([&] { return !worker.isRunning(); })); CHECK(summary.requests.size() == 1);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    qputenv("AI_API_KEY", "synthetic-local-fixture-only");
    testWorkerResponses(); testWorkerCancelAndReentry(); testFinalizationBudget();
    testNewGoalAndNonstream(); testSummaryFallbacks(); testMultipleCallsAndApproval();
    testCancelSingleFlightAndSafety(); testDiagnosisLimitAndHistory(); testReportCodeAndCancellation();
    qInfo() << "AI lifecycle failures:" << failures;
    return failures ? 1 : 0;
}
