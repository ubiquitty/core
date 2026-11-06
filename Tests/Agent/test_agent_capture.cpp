// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Agent/AgentSession.h"
#include "Core/Codec/PNGDecoder.h"
#include "Core/Codec/ZlibInflate.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"
#include "PAL/UnixFileDescriptor.h"
#include "ScreenPlaneFixture.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Ubiquitty::Agent {

/// Installs only the private fixture's carrier, never discovering a user host.
struct AgentCaptureAccess {
    static bool connect(AgentSession& agent, PAL::UnixFileDescriptor client)
    {
        const int flags = ::fcntl(client.get(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(client.get(), F_SETFL, flags | O_NONBLOCK) != 0)
            return false;
        agent.m_socketFileDescriptor = std::move(client);
        ++agent.m_captureConnectionGeneration;
        resize(*agent.m_term, 2, 8, Modes::ReflowMode::Fixed);
        Protocol::Sync::HandshakeRequest hello;
        hello.columns = 8;
        hello.rows = 2;
        hello.sessionID = 17;
        hello.expectedDaemonInstanceID = 91;
        hello.agentLabel = "capture-fixture";
        std::vector<std::uint8_t> frame;
        agent.m_core->buildHello(hello, frame);
        return agent.writeFrame(frame);
    }

    static void exhaustRequestIDs(AgentSession& agent) { agent.m_nextCaptureRequestID = std::numeric_limits<std::uint64_t>::max(); }

    static Terminal& mirror(AgentSession& agent) { return *agent.m_term; }

    static void completeCommand(AgentSession& agent, const Protocol::Sync::CommandFinishedEffect& command)
    {
        const Protocol::Sync::EffectView effect { .paneID = { 17, 91 }, .generation = 19, .sequence = agent.m_commandDones.size() + 1, .primary = { 2, 3 }, .kind = Protocol::Sync::EffectKind::CommandFinished };
        agent.onCommandDone(effect, command, Protocol::Sync::EffectDisposition::Ignore);
    }

    static std::optional<std::pair<std::int32_t, std::string>> completedCommand(const AgentSession& agent, std::size_t index)
    {
        if (index >= agent.m_commandDones.size())
            return std::nullopt;
        const auto& command = agent.m_commandDones[index];
        return std::pair { command.exitCode, command.currentDirectory };
    }
};

} // namespace Ubiquitty::Agent

namespace {

using Ubiquitty::Agent::AgentCaptureAccess;
using Ubiquitty::Agent::AgentSession;
using Ubiquitty::Codec::JSON::Value;
using Ubiquitty::Protocol::Control::CaptureReplyStatus;
namespace Control = Ubiquitty::Protocol::Control;
namespace JSON = Ubiquitty::Codec::JSON;
namespace Sync = Ubiquitty::Protocol::Sync;

enum class ReplyMode : std::uint8_t {
    Complete,
    ForbiddenThenComplete,
    ForbiddenAfterFirstCapture,
    Incomplete,
    WrongPane,
    Unbind,
    ChangedOnSecondCapture,
    LateThenComplete,
    PendingThenComplete,
    Listing,
    ListingRefused,
    InputUnconfirmed,
    Terminated,
    TerminationDenied,
    TerminationTimedOut,
    TerminationWrongTarget,
    TerminationWrongID,
    TerminationSilent,
};

/// A program that has stopped reading retains input without acknowledging it.
class UnreadInput final : public Sync::InputWriter {
public:
    Sync::WriteStatus write(Sync::ID128, const Sync::InputIntent&) override { return Sync::WriteStatus::Refused; }
};

/// The real screen protocol and a deterministic Call answerer on a socket pair.
/// All protocol state belongs to its worker; only counters cross the thread.
class CaptureHost {
public:
    CaptureHost(AgentSession& agent, ReplyMode mode)
        : m_mode(mode)
        , m_plane(m_terminal, 17, 91)
    {
        Ubiquitty::resize(m_terminal, 2, 8, Ubiquitty::Modes::ReflowMode::Fixed);
        if (mode == ReplyMode::InputUnconfirmed) {
            Ubiquitty::processBytes(&m_terminal, "> ");
            m_plane.source().setInputWriter(m_unreadInput);
        }
        std::array<int, 2> pair { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) != 0) {
            m_failed = true;
            return;
        }
        m_peer = Ubiquitty::PAL::UnixFileDescriptor(pair[0]);
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        if (::setsockopt(m_peer.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
            ::close(pair[1]);
            m_failed = true;
            return;
        }
#endif
        if (!AgentCaptureAccess::connect(agent, Ubiquitty::PAL::UnixFileDescriptor(pair[1]))) {
            m_failed = true;
            return;
        }
        m_worker = std::thread([this] { serve(); });
    }

    ~CaptureHost() { finish(); }

    void finish()
    {
        m_stopping = true;
        if (m_peer)
            ::shutdown(m_peer.get(), SHUT_RDWR);
        if (m_worker.joinable())
            m_worker.join();
    }

    bool isOkay() const { return !m_failed; }
    unsigned calls() const { return m_calls; }
    unsigned unsubscribes() const { return m_unsubscribes; }
    const std::vector<std::string>& requestIDsAfterFinish() const { return m_requestIDs; }

private:
    bool send(std::span<const std::uint8_t> bytes)
    {
        while (!bytes.empty()) {
            constexpr int flags =
#ifdef MSG_NOSIGNAL
                MSG_NOSIGNAL;
#else
                0;
#endif
            const ssize_t written = ::send(m_peer.get(), bytes.data(), bytes.size(), flags);
            if (written > 0) {
                bytes = bytes.subspan(static_cast<std::size_t>(written));
                continue;
            }
            if (written < 0 && errno == EINTR)
                continue;
            if (!m_stopping)
                m_failed = true;
            return false;
        }
        return true;
    }

    void answer(const Sync::ReceivedCall& call, const Control::DecodedRequest& request, std::string text, ReplyMode mode)
    {
        Value response;
        if (mode == ReplyMode::ForbiddenThenComplete) {
            response = Control::rpcError(request.idOrNull(), Control::ErrorCode::Forbidden, "pane.capture needs region");
        } else {
            Value result = Value::object();
            result.set("pane", Value::number(m_plane.handle() + (mode == ReplyMode::WrongPane ? 1 : 0)));
            const std::string source = request.params.stringOr("source", "screen");
            if (source == "screen") {
                result.set("rows", Value::number(2));
            } else {
                const int first = source == "tail" && request.params.numberOr("lines") == 1 ? 1 : 0;
                result.set("source", Value::string(source));
                result.set("first", Value::number(first));
                result.set("last", Value::number(1));
                result.set("totalRows", Value::number(2));
                if (first == 1)
                    text = "answer\n";
            }
            result.set("columns", Value::number(8));
            result.set("complete", Value::boolean(mode != ReplyMode::Incomplete && mode != ReplyMode::PendingThenComplete));
            result.set("text", Value::string(std::move(text)));
            if (mode == ReplyMode::PendingThenComplete) {
                Value richText = Value::object();
                richText.set("offsetUnit", Value::string("unicode-scalar"));
                richText.set("coordinateUnit", Value::string("point"));
                richText.set("layoutPending", Value::boolean(true));
                richText.set("cellWidth", Value::number(8));
                richText.set("cellHeight", Value::number(16));
                richText.set("blocks", Value::array());
                result.set("richText", std::move(richText));
            }
            response = Control::rpcResult(request.idOrNull(), std::move(result));
        }
        if (m_plane.connection().answerCall(call.callIndex, JSON::serialize(response)) != Sync::CallAnswer::Sent)
            m_failed = true;
    }

    void serve()
    {
        std::array<std::uint8_t, 65536> bytes {};
        while (!m_stopping) {
            const ssize_t count = ::read(m_peer.get(), bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return;
            const std::span<const std::uint8_t> input(bytes.data(), static_cast<std::size_t>(count));
            m_plane.receive(input);
            if (m_plane.connection().state() == Sync::ConnectionState::Closed) {
                if (m_plane.connection().byeReason() != Sync::ByeReason::Normal && !m_stopping)
                    m_failed = true;
                return;
            }
            for (const auto& call : m_plane.connection().takeCalls()) {
                const auto request = Control::decodeRequest(call.text, {});
                if (m_mode >= ReplyMode::Terminated) {
                    if (request.method != "session.terminate" || !request.hasID || !request.id.isString()
                        || request.params.numberOr("session") != 19) {
                        m_failed = true;
                        return;
                    }
                    ++m_calls;
                    m_requestIDs.push_back(request.id.asString());
                    if (m_mode == ReplyMode::TerminationSilent)
                        continue;
                    Value result = Value::object();
                    result.set("session", Value::number(m_mode == ReplyMode::TerminationWrongTarget ? 20 : 19));
                    result.set("outcome", Value::string(m_mode == ReplyMode::TerminationDenied ? "denied" : m_mode == ReplyMode::TerminationTimedOut ? "timed-out"
                                                                                                                                                     : "terminated"));
                    const Value wrong = Value::string("another-request");
                    (void)m_plane.connection().answerCall(call.callIndex, JSON::serialize(Control::rpcResult(m_mode == ReplyMode::TerminationWrongID ? &wrong : request.idOrNull(), std::move(result))));
                    continue;
                }
                if (request.method == "host.sessions.unsubscribe") {
                    ++m_unsubscribes;
                    (void)m_plane.connection().answerCall(call.callIndex, JSON::serialize(Control::rpcResult(request.idOrNull(), Value::object())));
                    continue;
                }
                if (m_mode == ReplyMode::Listing || m_mode == ReplyMode::ListingRefused) {
                    if (request.method != "host.sessions.subscribe" || !request.hasID || !request.id.isString()) {
                        m_failed = true;
                        return;
                    }
                    ++m_calls;
                    m_requestIDs.push_back(request.id.asString());
                    Value response;
                    if (m_mode == ReplyMode::ListingRefused) {
                        response = Control::rpcError(request.idOrNull(), Control::ErrorCode::Forbidden, "listing needs control");
                    } else {
                        Value result = Value::object();
                        result.set("workspaces", Value::array());
                        result.set("structureRevisions", Value::array());
                        result.set("paneRevisions", Value::array());
                        result.set("panes", Value::number(0));
                        result.set("hostNowUnixMilliseconds", Value::number(1000));
                        response = Control::rpcResult(request.idOrNull(), std::move(result));
                    }
                    (void)m_plane.connection().answerCall(call.callIndex, JSON::serialize(response));
                    continue;
                }
                const auto* pane = request.params.find("pane");
                if (call.admission != Sync::CallAdmission::Admitted || request.outcome != Control::RequestOutcome::Dispatch
                    || request.method != "pane.capture" || !request.hasID || !request.id.isString()
                    || !pane || !pane->isNumber() || pane->asNumber() != m_plane.handle()) {
                    m_failed = true;
                    return;
                }
                const unsigned ordinal = ++m_calls;
                m_requestIDs.push_back(request.id.asString());
                if (m_mode == ReplyMode::LateThenComplete && ordinal == 1) {
                    m_heldCall = call;
                    continue;
                }
                if (m_mode == ReplyMode::Unbind)
                    m_plane.connection().unbindPane(m_plane.handle(), 0);
                if (m_heldCall) {
                    const auto retired = Control::decodeRequest(m_heldCall->text, {});
                    answer(*m_heldCall, retired, "retired\nreply\n", ReplyMode::Complete);
                    m_heldCall.reset();
                }
                const ReplyMode mode = m_mode == ReplyMode::ForbiddenAfterFirstCapture && ordinal > 1                         ? ReplyMode::ForbiddenThenComplete
                    : (m_mode == ReplyMode::ForbiddenThenComplete || m_mode == ReplyMode::PendingThenComplete) && ordinal > 1 ? ReplyMode::Complete
                                                                                                                              : m_mode;
                answer(call, request, m_mode == ReplyMode::ChangedOnSecondCapture && ordinal == 1 ? "waiting\n\n" : "fresh\nanswer\n", mode);
            }
            std::vector<std::uint8_t> output;
            m_plane.publish(output);
            if (!send(output))
                return;
        }
    }

    ReplyMode m_mode;
    std::atomic<unsigned> m_unsubscribes { 0 };
    Ubiquitty::Terminal m_terminal;
    UnreadInput m_unreadInput;
    UbiquittyTest::ScreenPlaneHost m_plane;
    Ubiquitty::PAL::UnixFileDescriptor m_peer;
    std::thread m_worker;
    std::atomic<bool> m_stopping = false;
    std::atomic<bool> m_failed = false;
    std::atomic<unsigned> m_calls = 0;
    std::vector<std::string> m_requestIDs;
    std::optional<Sync::ReceivedCall> m_heldCall;
};

} // namespace

TEST(agentRetainsCommandStateAfterTheBorrowedEffectReturns)
{
    // No carrier is opened: this case exercises only the synchronous typed
    // observer and the agent's own queue, never CaptureHost or a poll loop.
    AgentSession agent;
    std::string directory = "/work/✓";
    std::string line = "false";
    AgentCaptureAccess::completeCommand(agent, { .exitStatus = 17, .durationMicroseconds = 42, .currentDirectory = directory, .commandLine = line });
    directory.assign("overwritten");
    line.assign("overwritten");
    CHECK_EQ(agent.lastExitCode(), 17);
    CHECK(agent.lastCommandLine() == "false");
    const auto first = AgentCaptureAccess::completedCommand(agent, 0);
    REQUIRE(first);
    CHECK(first->first == 17 && first->second == "/work/✓");

    AgentCaptureAccess::completeCommand(agent, { .exitStatus = 0, .currentDirectory = "/next", .commandLine = {} });
    const auto second = AgentCaptureAccess::completedCommand(agent, 1);
    REQUIRE(second);
    CHECK(second->first == 0 && second->second == "/next");
    CHECK_EQ(agent.lastExitCode(), 0);
    CHECK(agent.lastCommandLine() == "false");
    CHECK(!AgentCaptureAccess::completedCommand(agent, 2));
    CHECK(AgentCaptureAccess::completedCommand(agent, 0) == first);
}

TEST(agentCaptureWaitsForCoherenceAndUsesCallTextInsteadOfTheReplica)
{
    AgentSession agent;
    CHECK(!agent.captureScreen(0));
    CaptureHost host(agent, ReplyMode::Complete);
    CHECK(!agent.captureScreen(0)); // a carrier alone is not a coherent screen
    CHECK(agent.waitForFirstScreen(1000));
    CHECK(agent.screenText() == "\n");
    const auto captured = agent.captureScreen(1000);
    CHECK(captured && captured->status == CaptureReplyStatus::Complete && captured->text == "fresh\nanswer");
    CHECK(agent.screenText() == "\n"); // no mirror/cache substitution
    CHECK_EQ(host.calls(), 1U);
    host.finish();
    CHECK(host.isOkay());
}

TEST(agentRangesAndLastCommandUseOnlyTypedCallsAndKeepRowTerminators)
{
    AgentSession agent;
    CaptureHost host(agent, ReplyMode::Complete);
    CHECK(agent.waitForAttach(1000));
    std::string text;
    CHECK(agent.readLastCommand(text, 1000) && text == "fresh\nanswer\n");
    CHECK(agent.readRegion(-1, INT64_MAX, text, 1000) && text == "fresh\nanswer\n");
    const auto tail = agent.captureText({ .source = Control::CaptureSource::Tail, .lines = 1 }, 1000);
    CHECK(tail && tail->status == CaptureReplyStatus::Complete && tail->text == "answer\n");
    CHECK_EQ(host.calls(), 3U);
    host.finish();
    CHECK(host.isOkay());
}

TEST(agentRunStartupUsesItsDeadlineAndDoesNotLeaveItOnTheConnection)
{
    AgentSession agent;
    CaptureHost host(agent, ReplyMode::Complete);
    REQUIRE(agent.waitForFirstScreen(1000));
    REQUIRE(agent.screenText() == "\n");
    const auto started = std::chrono::steady_clock::now();
    const auto result = agent.runCommandWithExit("echo never-sent", 100);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(result.completion == AgentSession::RunCompletion::TimedOut && !result.capture && !result.exitStatus);
    CHECK(elapsed < std::chrono::milliseconds(800));
    CHECK_EQ(host.calls(), 0U);
    const auto captured = agent.captureScreen(1000);
    CHECK(captured && captured->status == CaptureReplyStatus::Complete);
    host.finish();
    CHECK(host.isOkay());
}

TEST(agentRunPreservesUnconfirmedInputWithAndWithoutAnOverallDeadline)
{
    for (const auto timeout : { std::optional<int> { 100 }, std::optional<int> {} }) {
        AgentSession agent;
        CaptureHost host(agent, ReplyMode::InputUnconfirmed);
        REQUIRE(agent.waitForFirstScreen(1000));
        REQUIRE(agent.screenText().find('>') != std::string::npos);
        const auto result = agent.runCommandWithExit("echo unread", timeout);
        CHECK(result.inputDelivery == AgentSession::InputDelivery::Unconfirmed);
        CHECK(result.completion == (timeout ? AgentSession::RunCompletion::TimedOut : AgentSession::RunCompletion::Unavailable));
        CHECK(!result.capture && !result.exitStatus);
        CHECK_EQ(host.calls(), 0U);
        host.finish();
        CHECK(host.isOkay());
    }
}

TEST(agentRunPreservesInputThatCouldNotBeSent)
{
    AgentSession agent;
    const auto result = agent.runCommandWithExit("echo unsent", std::nullopt);
    CHECK(result.completion == AgentSession::RunCompletion::Unavailable);
    CHECK(result.inputDelivery == AgentSession::InputDelivery::NotSent);
    CHECK(!result.capture && !result.exitStatus);
}

TEST(agentCaptureRefusalPreservesTheConnectionWithoutExposingCachedText)
{
    AgentSession agent;
    CaptureHost host(agent, ReplyMode::ForbiddenThenComplete);
    CHECK(agent.waitForFirstScreen(1000));
    const auto denied = agent.captureScreen(1000);
    CHECK(denied && denied->status == CaptureReplyStatus::Refused && denied->text.empty());
    CHECK(denied && denied->errorCode == std::to_underlying(Control::ErrorCode::Forbidden));
    const auto allowed = agent.captureScreen(1000);
    CHECK(allowed && allowed->status == CaptureReplyStatus::Complete && allowed->text == "fresh\nanswer");
    CHECK_EQ(host.calls(), 2U);
    host.finish();
    CHECK(host.isOkay());
}

TEST(agentCaptureWaitsForNativeGeometryWithinItsOriginalDeadline)
{
    AgentSession agent;
    CaptureHost host(agent, ReplyMode::PendingThenComplete);
    REQUIRE(agent.waitForFirstScreen(1000));
    const auto capture = agent.captureScreen(1000);
    CHECK(capture && capture->status == CaptureReplyStatus::Complete && capture->text == "fresh\nanswer");
    CHECK_EQ(host.calls(), 2U);
    host.finish();
    CHECK(host.isOkay());
}

TEST(agentContinuousReadUsesFreshCallsAndStopsOnRefusal)
{
    {
        AgentSession agent;
        CaptureHost host(agent, ReplyMode::ChangedOnSecondCapture);
        REQUIRE(agent.waitForFirstScreen(1000));
        bool found = false;
        const auto captured = agent.waitForText("answer", 1500, found);
        CHECK(found && captured && captured->status == CaptureReplyStatus::Complete && captured->text == "fresh\nanswer");
        CHECK_EQ(host.calls(), 2U);
        CHECK(agent.screenText() == "\n");
        host.finish();
        CHECK(host.isOkay());
    }
    {
        AgentSession agent;
        CaptureHost host(agent, ReplyMode::ForbiddenThenComplete);
        REQUIRE(agent.waitForFirstScreen(1000));
        bool found = true;
        const auto denied = agent.waitForText("\n", 1000, found);
        CHECK(!found && denied && denied->status == CaptureReplyStatus::Refused && denied->text.empty());
        CHECK_EQ(host.calls(), 1U);
        host.finish();
        CHECK(host.isOkay());
    }
}

TEST(agentContinuousReadReturnsOnlyTheLastCompleteCaptureAtItsDeadline)
{
    AgentSession agent;
    CaptureHost host(agent, ReplyMode::Complete);
    REQUIRE(agent.waitForFirstScreen(1000));
    bool found = true;
    const auto captured = agent.waitForText("absent", 80, found);
    CHECK(!found && captured && captured->status == CaptureReplyStatus::Complete && captured->text == "fresh\nanswer");
    CHECK_EQ(host.calls(), 1U);
    CHECK(!agent.waitForText("", 1000, found));
    CHECK(!found && host.calls() == 1);
    host.finish();
    CHECK(host.isOkay());
}

TEST(agentContinuousReadDoesNotCrossAnAuthorizationRefusalAfterAValidCapture)
{
    AgentSession agent;
    CaptureHost host(agent, ReplyMode::ForbiddenAfterFirstCapture);
    REQUIRE(agent.waitForFirstScreen(1000));
    bool found = true;
    const auto refused = agent.waitForText("absent", 1500, found);
    CHECK(!found && refused && refused->status == CaptureReplyStatus::Refused && refused->text.empty());
    CHECK_EQ(host.calls(), 2U);
    host.finish();
    CHECK(host.isOkay());
}

TEST(agentCaptureRejectsIncompleteWrongTargetAndRetiredBinding)
{
    for (const auto mode : { ReplyMode::Incomplete, ReplyMode::WrongPane, ReplyMode::Unbind }) {
        AgentSession agent;
        CaptureHost host(agent, mode);
        CHECK(agent.waitForFirstScreen(1000));
        const auto answer = agent.captureScreen(1000);
        if (mode == ReplyMode::Unbind) {
            CHECK(!answer);
        } else {
            const auto expected = mode == ReplyMode::Incomplete ? CaptureReplyStatus::Incomplete : CaptureReplyStatus::Invalid;
            CHECK(answer && answer->status == expected && answer->text.empty());
        }
        CHECK_EQ(host.calls(), 1U);
        host.finish();
        CHECK(host.isOkay());
    }
}

TEST(agentCaptureTimeoutRetiresItsIDAndIgnoresTheLateAnswer)
{
    AgentSession agent;
    CaptureHost host(agent, ReplyMode::LateThenComplete);
    CHECK(agent.waitForFirstScreen(1000));
    CHECK(!agent.captureScreen(0)); // sends once, then retires without waiting
    const auto next = agent.captureScreen(1000);
    CHECK(next && next->status == CaptureReplyStatus::Complete && next->text == "fresh\nanswer");
    CHECK_EQ(host.calls(), 2U);
    host.finish();
    CHECK(host.isOkay());
    const auto& ids = host.requestIDsAfterFinish();
    CHECK(ids.size() == 2 && ids[0] != ids[1]);
}

TEST(agentCaptureNeverReusesExhaustedIDsOrReadsAfterDetach)
{
    AgentSession agent;
    CaptureHost host(agent, ReplyMode::Complete);
    CHECK(agent.waitForFirstScreen(1000));
    AgentCaptureAccess::exhaustRequestIDs(agent);
    CHECK(!agent.captureScreen(0));
    agent.detach();
    CHECK(!agent.captureScreen(0));
    host.finish();
    CHECK_EQ(host.calls(), 0U);
    CHECK(host.isOkay());
}

TEST(agentDirectoryUsesUniqueTypedSubscriptionsWithoutLegacyRows)
{
    AgentSession agent;
    CaptureHost host(agent, ReplyMode::Listing);
    REQUIRE(agent.waitForFirstScreen(1000));
    std::vector<Ubiquitty::Session::SessionRow> rows(1);
    CHECK(agent.listSessions(rows, 1000));
    CHECK(rows.empty());
    CHECK(agent.listSessions(rows, 1000));
    CHECK(rows.empty());
    agent.pumpUntil(1000, [&] { return host.unsubscribes() == 2; });
    CHECK_EQ(host.calls(), 2U);
    CHECK_EQ(host.unsubscribes(), 2U);
    host.finish();
    CHECK(host.isOkay());
    const auto& ids = host.requestIDsAfterFinish();
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] != ids[1]);
}

TEST(agentDirectoryRefusalClearsRowsAndRetiresItsSubscription)
{
    AgentSession agent;
    CaptureHost host(agent, ReplyMode::ListingRefused);
    REQUIRE(agent.waitForFirstScreen(1000));
    std::vector<Ubiquitty::Session::SessionRow> rows(1);
    rows.front().id = 123;
    CHECK(!agent.listSessions(rows, 1000));
    CHECK(rows.empty());
    agent.pumpUntil(1000, [&] { return host.unsubscribes() == 1; });
    CHECK_EQ(host.unsubscribes(), 1U);
    host.finish();
    CHECK(host.isOkay());
}

TEST(agentTerminationRequiresAnExactTypedResultAndNeverInfersSuccess)
{
    for (const auto& [mode, outcome] : std::array {
             std::pair { ReplyMode::Terminated, AgentSession::TerminateOutcome::Terminated },
             std::pair { ReplyMode::TerminationDenied, AgentSession::TerminateOutcome::Denied },
             std::pair { ReplyMode::TerminationTimedOut, AgentSession::TerminateOutcome::TimedOut },
             std::pair { ReplyMode::TerminationWrongTarget, AgentSession::TerminateOutcome::Failed },
             std::pair { ReplyMode::TerminationWrongID, AgentSession::TerminateOutcome::TimedOut },
             std::pair { ReplyMode::TerminationSilent, AgentSession::TerminateOutcome::TimedOut } }) {
        AgentSession agent;
        CaptureHost host(agent, mode);
        REQUIRE(agent.waitForFirstScreen(1000));
        CHECK(agent.terminateSession(19, 1000) == outcome);
        CHECK_EQ(host.calls(), 1U);
        host.finish();
        CHECK(host.isOkay());
    }
}

// A kitty Unicode placeholder names its tile through marks the mirror keeps
// in its cluster pool, past the one mark a cell holds inline, so a screenshot
// that read the copied cells alone placed no tile. Image 7 is a red texel and
// a green one cut into a one-row grid of two cells; the two placeholder cells
// on the first row show one each, at the grid's place beside the prompt-mark
// gutter.
TEST(agentScreenshotDrawsTheTilesKittyPlaceholdersName)
{
    AgentSession agent;
    Ubiquitty::Terminal& mirror = AgentCaptureAccess::mirror(agent);
    // Two rows: the fewest a resize gives (Core/GridBounds.h, minRows). The
    // screenshot's size follows from them, and the decode's scratch from that.
    Ubiquitty::resize(mirror, 2, 4, Ubiquitty::Modes::ReflowMode::Fixed);
    REQUIRE(mirror.rows == 2 && mirror.columns == 4);
    Ubiquitty::processBytes(&mirror, "\x1b_Ga=T,U=1,i=7,f=24,s=2,v=1,c=2,r=1,q=2;/wAAAP8A\x1b\\");
    // U+10EEEE with row 0 (U+0305) and column 0 (U+0305) or 1 (U+030D); the
    // foreground's palette index is the image id.
    Ubiquitty::processBytes(&mirror, "\x1b[H\x1b[38;5;7m"
                                     "\xF4\x8E\xBB\xAE\xCC\x85\xCC\x85"
                                     "\xF4\x8E\xBB\xAE\xCC\x85\xCC\x8D"
                                     "\x1b[0m");

    const std::vector<std::uint8_t> png = agent.renderPNG();
    REQUIRE(!png.empty());
    // The canvas: a 5-pixel gutter each side of two rows of four 8x16 cells.
    constexpr int gutter = 5;
    constexpr int width = 2 * gutter + 4 * 8;
    constexpr int height = 2 * 16;
    std::vector<std::uint8_t> scratch(static_cast<std::size_t>(width) * height * 4 + static_cast<std::size_t>(height) * (1 + width * 4));
    auto inflater = std::make_unique<Ubiquitty::Codec::Zlib::Inflater>();
    const Ubiquitty::Codec::PNG::DecodeResult decoded = Ubiquitty::Codec::PNG::decode(png, scratch, *inflater);
    REQUIRE(decoded.status == Ubiquitty::Codec::PNG::DecodeStatus::Success);
    REQUIRE(decoded.width == width && decoded.height == height);
    const auto pixel = [&](int x, int y) {
        const std::size_t at = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * static_cast<std::size_t>(decoded.bytesPerPixel);
        return std::array<int, 3> { decoded.pixels[at], decoded.pixels[at + 1], decoded.pixels[at + 2] };
    };
    CHECK_MSG((pixel(gutter + 3, 8) == std::array<int, 3> { 255, 0, 0 }), "the first placeholder cell shows the image's red tile");
    CHECK_MSG((pixel(gutter + 8 + 3, 8) == std::array<int, 3> { 0, 255, 0 }), "the second shows its green tile");
}

UBIQUITTY_TEST_MAIN()
