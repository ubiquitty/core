// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/SyncLink.h"
#include "Harness.h"

#include "Core/Protocol/Local/AdoptIngress.h"
#include "Core/Protocol/Sync/HandoverGround.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/HandoverAccess.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/StreamDouble.h"

#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <sys/socket.h>
#include <unistd.h>

namespace Host = Ubiquitty::Host;
namespace PAL = Ubiquitty::PAL;
namespace Protocol = Ubiquitty::Protocol;
namespace Runtime = Ubiquitty::Runtime;
namespace TestDoubles = Ubiquitty::TestDoubles;
using Access = Host::HandoverAccess;
using Screen = Host::ScreenPlaneAccess;

namespace {

struct Fixture {
    Host::Daemon daemon;
    TestDoubles::StreamDouble* stream = nullptr;
    Access::Session* session = nullptr;
    std::uint64_t started = Screen::now();

    Fixture()
    {
        auto owned = std::make_unique<TestDoubles::StreamDouble>();
        stream = owned.get();
        session = &Screen::makeSession(daemon, 24, 80, std::move(owned));
        session->coherence = Runtime::FrameCoherence({ .isPaced = false });
        Screen::publish(daemon, *session, Runtime::FrameClass::Structural);
    }

    void start()
    {
        started = Screen::now();
        CHECK(Access::begin(daemon, started));
    }

    void step(std::uint64_t elapsed) { Access::step(daemon, started + elapsed); }
};

/// Real readable/quiet poll observations, with no child process or terminal
/// scheduler involved. The write side uses the shared ownership-aware double.
class PollableStream final : public PAL::Stream {
public:
    explicit PollableStream(PAL::UnixFileDescriptor descriptor)
        : m_descriptor(std::move(descriptor))
    {
    }
    int read(std::span<char> bytes) override
    {
        if (beforeRead) {
            auto callback = std::exchange(beforeRead, {});
            callback();
        }
        const auto count = ::read(m_descriptor.get(), bytes.data(), bytes.size());
        if (count > 0)
            return static_cast<int>(count);
        if (!count)
            return std::to_underlying(PAL::StreamStatus::EndOfFile);
        return std::to_underlying(errno == EAGAIN || errno == EWOULDBLOCK ? PAL::StreamStatus::Again : PAL::StreamStatus::Fatal);
    }
    int write(std::span<const char> bytes) override { return m_writes.write(bytes); }
    int writeAll(std::span<const char> bytes) override { return m_writes.writeAll(bytes); }
    bool drainPendingWrite() override { return m_writes.drainPendingWrite(); }
    bool hasPendingWrite() const override { return m_writes.hasPendingWrite(); }
    int lastError() const override { return errno; }
    void setWindowSize(int, int, int, int) override { }
    int fileDescriptor() const override { return m_descriptor.get(); }
    PAL::ProcessFacts& process() override { return m_writes.process(); }
    std::function<void()> beforeRead;

private:
    PAL::UnixFileDescriptor m_descriptor;
    TestDoubles::StreamDouble m_writes;
};

std::array<PAL::UnixFileDescriptor, 2> socketPair()
{
    int descriptors[2] { -1, -1 };
    CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
    for (int descriptor : descriptors)
        CHECK_EQ(::fcntl(descriptor, F_SETFL, O_NONBLOCK), 0);
    return { PAL::UnixFileDescriptor(descriptors[0]), PAL::UnixFileDescriptor(descriptors[1]) };
}

} // namespace

TEST(aSplitCSIWaitsForGroundAndFreezesTheExactPublishedState)
{
    Fixture fixture;
    fixture.stream->pushInbound("\033[3");
    fixture.start();
    fixture.step(0);
    CHECK(!Access::isQuiesced(fixture.daemon));
    CHECK(!Protocol::Sync::terminalHandoverBlockers(*fixture.session->term).isEmpty());
    fixture.stream->pushInbound("1m");
    fixture.step(1'000);
    fixture.step(5'000);
    CHECK(Access::isQuiesced(fixture.daemon));
    CHECK(Protocol::Sync::terminalHandoverBlockers(*fixture.session->term).isEmpty());
    CHECK(fixture.session->screenSource->frameClass() == Protocol::Sync::FrameClass::Settled);
    CHECK(!fixture.session->isFrameDue);
    const auto generation = fixture.session->screenSource->generation();
    CHECK(generation > 0);
    const int reads = fixture.stream->readCalls;
    fixture.stream->pushInbound("after");
    fixture.step(10'000);
    CHECK_EQ(fixture.stream->readCalls, reads);
    CHECK_EQ(fixture.stream->bytesRemaining(), 5u);
    CHECK_EQ(fixture.session->screenSource->generation(), generation);
    Access::end(fixture.daemon);
    Screen::servicePTY(fixture.daemon, *fixture.session);
    Screen::publish(fixture.daemon, *fixture.session);
    CHECK_EQ(fixture.stream->bytesRemaining(), 0u);
}

TEST(aMarginedEditorEntryRemainsIncompleteAcrossReads)
{
    Fixture fixture;
    fixture.stream->pushInbound("\033[?1049h\033[2;20r\033[?69h\033[3;");
    fixture.start();
    fixture.step(0);
    CHECK(!Access::isQuiesced(fixture.daemon));
    fixture.stream->pushInbound("70s\033[2;3Heditor");
    fixture.step(1'000);
    fixture.step(5'000);
    CHECK(Access::isQuiesced(fixture.daemon));
    CHECK(Protocol::Sync::terminalHandoverBlockers(*fixture.session->term).isEmpty());
}

TEST(theGroundReadLeaseNeverConsumesTheNextSequence)
{
    Fixture fixture;
    fixture.stream->pushInbound("\033[3");
    fixture.start();
    fixture.step(0);
    fixture.stream->pushInbound("1mTAIL");
    fixture.step(100'000);
    CHECK_EQ(fixture.stream->bytesRemaining(), 5u);
    CHECK_EQ(Access::groundBytes(fixture.daemon, 0), 1u);
    fixture.step(101'000);
    CHECK_EQ(fixture.stream->bytesRemaining(), 4u);
    CHECK_EQ(Access::groundBytes(fixture.daemon, 0), 2u);
    fixture.step(110'000);
    CHECK_EQ(fixture.stream->bytesRemaining(), 4u);
    CHECK(!Access::isQuiesced(fixture.daemon));
    fixture.step(200'000);
    CHECK(Access::didFail(fixture.daemon));
    CHECK(!Access::isActive(fixture.daemon));
    Screen::servicePTY(fixture.daemon, *fixture.session);
    CHECK_EQ(fixture.stream->bytesRemaining(), 0u);
}

TEST(aPollableOneByteCompletionNeedsRealQuietBeforeSettled)
{
    Host::Daemon daemon;
    auto sockets = socketPair();
    auto& session = Screen::makeSession(daemon, 24, 80, std::make_unique<PollableStream>(std::move(sockets[0])));
    session.coherence = Runtime::FrameCoherence({ .isPaced = false });
    Screen::publish(daemon, session, Runtime::FrameClass::Structural);
    CHECK_EQ(::write(sockets[1].get(), "\033[3", 3), 3);
    const auto started = Screen::now();
    CHECK(Access::begin(daemon, started));
    Access::step(daemon, started);
    CHECK_EQ(::write(sockets[1].get(), "m", 1), 1);
    Access::step(daemon, started + 100'000);
    CHECK_EQ(Access::groundBytes(daemon, 0), 1u);
    CHECK(!Access::isQuiesced(daemon));
    Access::step(daemon, started + 103'000);
    CHECK(Access::isQuiesced(daemon));
}

TEST(aDeadlineUnfreezesEveryPaneWithoutDiscardingOutput)
{
    Fixture fixture;
    auto blocked = std::make_unique<TestDoubles::StreamDouble>();
    auto* blockedStream = blocked.get();
    auto& blockedSession = Screen::makeSession(fixture.daemon, 24, 80, std::move(blocked));
    blockedSession.coherence = Runtime::FrameCoherence({ .isPaced = false });
    Screen::publish(fixture.daemon, blockedSession, Runtime::FrameClass::Structural);
    blockedStream->pushInbound("\033[3");
    fixture.start();
    fixture.step(0);
    fixture.step(5'000);
    CHECK(Access::phase(fixture.daemon, 0) == Runtime::QuiescePhase::Ready);
    CHECK(!Access::isQuiesced(fixture.daemon));
    fixture.stream->pushInbound("held");
    fixture.step(200'000);
    CHECK(Access::didFail(fixture.daemon));
    CHECK(!Access::isActive(fixture.daemon));
    CHECK(fixture.session->isFrameDue && blockedSession.isFrameDue);
    blockedStream->pushInbound("1mdone");
    Screen::servicePTY(fixture.daemon, *fixture.session);
    Screen::servicePTY(fixture.daemon, blockedSession);
    CHECK_EQ(fixture.stream->bytesRemaining(), 0u);
    CHECK_EQ(blockedStream->bytesRemaining(), 0u);
    CHECK(Protocol::Sync::terminalHandoverBlockers(*blockedSession.term).isEmpty());
}

TEST(aDirtyFrozenPaneInvalidatesReadinessAndAbortsTheAttempt)
{
    Fixture fixture;
    fixture.start();
    fixture.step(0);
    fixture.step(5'000);
    CHECK(Access::isQuiesced(fixture.daemon));
    fixture.session->isFrameDue = true;
    CHECK(!Access::isQuiesced(fixture.daemon));
    fixture.step(6'000);
    CHECK(Access::didFail(fixture.daemon));
    CHECK(!Access::isActive(fixture.daemon));
}

TEST(aZeroByteLeaseDoesNotTouchTheStream)
{
    Fixture fixture;
    fixture.stream->pushInbound("untouched");
    const auto pass = Access::drain(fixture.daemon, *fixture.session, 0);
    CHECK_EQ(pass.bytesDrained, 0);
    CHECK(!pass.didCatchUp && !pass.didReachEnd);
    CHECK_EQ(fixture.stream->readCalls, 0);
    CHECK_EQ(fixture.stream->bytesRemaining(), 9u);
}

TEST(workInsideADrainCannotReuseAClockFromBeforeTheDeadline)
{
    Host::Daemon daemon;
    auto sockets = socketPair();
    auto stream = std::make_unique<PollableStream>(std::move(sockets[0]));
    stream->beforeRead = [] { std::this_thread::sleep_for(std::chrono::milliseconds(210)); };
    auto& session = Screen::makeSession(daemon, 24, 80, std::move(stream));
    session.coherence = Runtime::FrameCoherence({ .isPaced = false });
    Screen::publish(daemon, session, Runtime::FrameClass::Structural);
    const auto started = Screen::now();
    REQUIRE(Access::begin(daemon, started));
    Access::step(daemon, started);
    CHECK(!Access::isQuiesced(daemon));
    CHECK(Access::didFail(daemon));
    CHECK(!Access::isActive(daemon));
}

TEST(aFailedFreshCaptureNeverCountsAsAReadyPane)
{
    Fixture fixture;
    Ubiquitty::Cell invalid {};
    invalid.codePoint = 'x';
    invalid.setClusterID(1); // No definition exists for this reference.
    fixture.session->term->grid->setCell(Ubiquitty::Row(0), Ubiquitty::Column(0), invalid);
    fixture.start();
    fixture.step(0);
    fixture.step(5'000);
    CHECK(!fixture.session->screenSource->isEncodable());
    CHECK(!Access::isQuiesced(fixture.daemon));
    CHECK(Access::didFail(fixture.daemon));
    CHECK(!Access::isActive(fixture.daemon));
}

TEST(aLaterPaneCallbackInvalidatesAnAlreadyFrozenPane)
{
    Fixture fixture;
    auto sockets = socketPair();
    auto stream = std::make_unique<PollableStream>(std::move(sockets[0]));
    auto* later = stream.get();
    auto& session = Screen::makeSession(fixture.daemon, 24, 80, std::move(stream));
    session.coherence = Runtime::FrameCoherence({ .isPaced = false });
    Screen::publish(fixture.daemon, session, Runtime::FrameClass::Structural);
    CHECK_EQ(::write(sockets[1].get(), "\033[3", 3), 3);
    fixture.start();
    fixture.step(0);
    fixture.step(5'000);
    REQUIRE(Access::phase(fixture.daemon, 0) == Runtime::QuiescePhase::Ready);
    later->beforeRead = [&] { fixture.session->isFrameDue = true; };
    CHECK_EQ(::write(sockets[1].get(), "m", 1), 1);
    fixture.step(6'000);
    CHECK(!Access::isQuiesced(fixture.daemon));
    CHECK(Access::didFail(fixture.daemon));
    CHECK(!Access::isActive(fixture.daemon));
}

TEST(aDifferentPublicationCannotReplaceTheFrozenGeneration)
{
    Fixture fixture;
    fixture.start();
    fixture.step(0);
    fixture.step(5'000);
    REQUIRE(Access::isQuiesced(fixture.daemon));
    fixture.session->screenSource->publish(Protocol::Sync::FrameClass::Settled, Screen::now());
    CHECK(!Access::isQuiesced(fixture.daemon));
    fixture.step(6'000);
    CHECK(Access::didFail(fixture.daemon));
}

TEST(clientKeysStayInTheKernelUntilNormalServiceResumes)
{
    Fixture fixture;
    auto sockets = socketPair();
    const auto index = Screen::attach(fixture.daemon, fixture.session->id, std::move(sockets[0]));
    Screen::grantScope(fixture.daemon, index, { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(fixture.daemon, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(fixture.daemon, index, client);
    Screen::publish(fixture.daemon, *fixture.session, Runtime::FrameClass::Structural);
    Screen::deliver(fixture.daemon, index, client);
    REQUIRE(client.pane() != nullptr);
    const std::array<Protocol::Sync::KeyEvent, 1> keys { Protocol::Sync::KeyEvent { .text = "x", .unshiftedCodePoint = 'x', .shiftedCodePoint = 'x' } };
    CHECK(client.sendKeys(keys).has_value());
    auto frames = client.takeUpstream(Screen::now());
    CHECK(!frames.empty());
    fixture.start();
    CHECK_EQ(::write(sockets[1].get(), frames.data(), frames.size()), static_cast<ssize_t>(frames.size()));
    Access::serviceClient(fixture.daemon, index);
    CHECK(fixture.stream->written.empty());
    fixture.step(0);
    fixture.step(5'000);
    CHECK(Access::isQuiesced(fixture.daemon));
    CHECK(fixture.stream->written.empty());
    Access::end(fixture.daemon);
    Access::serviceClient(fixture.daemon, index);
    CHECK(!Screen::isDropped(fixture.daemon, index));
    CHECK(!Screen::isInputHeld(fixture.daemon, index));
    CHECK(fixture.stream->written == "x");
    Access::serviceClient(fixture.daemon, index);
    CHECK(fixture.stream->written == "x");
}

TEST(theQuestionAHandoverWaitsOnIsNamedNotTakenForABusyPane)
{
    // The person running the install is told what to finish first: an
    // agent's change waiting for approval, a capability question, and not a
    // pane that did not reach ground.
    Fixture fixture;
    CHECK(!Access::blocker(fixture.daemon));
    Access::holdCapabilityQuestion(fixture.daemon);
    CHECK(Access::blocker(fixture.daemon) == Protocol::Sync::HandoverRefuseReason::GrantPending);
    CHECK(!Access::begin(fixture.daemon, Screen::now()));
    Access::holdApprovalQuestion(fixture.daemon);
    CHECK(Access::blocker(fixture.daemon) == Protocol::Sync::HandoverRefuseReason::ApprovalPending);
    CHECK(!Access::begin(fixture.daemon, Screen::now()));
    Access::forgetQuestions(fixture.daemon);
    CHECK(!Access::blocker(fixture.daemon));
}

TEST(physicalOperationsRefuseBeforeFreezingOrConsumingAnyOwner)
{
    Fixture fixture;
    const auto index = Screen::attach(fixture.daemon, fixture.session->id);
    auto& client = Access::client(fixture.daemon, index);
    const auto refuses = [&] {
        CHECK(Access::hasBlockedOperations(fixture.daemon));
        CHECK(!Access::begin(fixture.daemon, Screen::now()));
        CHECK(!Access::isActive(fixture.daemon));
        CHECK_EQ(fixture.stream->readCalls, 0);
    };
    client.outputBuffer = { 'r', 'e', 'p', 'l', 'y' };
    refuses();
    CHECK_EQ(client.pendingOutputBytes(), 5u);
    client.clearPendingOutput();
    client.consentPending = true;
    refuses();
    CHECK(client.consentPending);
    client.consentPending = false;
    client.pendingClipboardRequests.push_back(41);
    refuses();
    CHECK_EQ(client.pendingClipboardRequests.front(), 41u);
    client.pendingClipboardRequests.clear();
    client.terminationCall.emplace();
    refuses();
    CHECK(client.terminationCall.has_value());
    client.terminationCall.reset();
    client.pendingConsentRetirements.push_back({ Protocol::Sync::ConsentKind::Attach, 73, fixture.session->id });
    refuses();
    client.pendingConsentRetirements.clear();
    client.clipboardToolRequestID = 19;
    refuses();
    client.clipboardToolRequestID = 0;
    const auto command = Protocol::Local::encodeAdoptWireCommand({ .kind = Protocol::Local::AdoptWireMessage::Status,
        .correlation = 1,
        .operation = { .operationID = { .low = 7, .high = 11 }, .daemonInstanceID = 13 } });
    REQUIRE(command.has_value());
    client.localAdopt = std::make_unique<Protocol::Local::AdoptIngress>(command->size());
    const auto admit = [](auto, auto, auto) { return true; };
    const auto deliver = [](auto) { return Protocol::Sync::FrameDelivery::Continue; };
    CHECK_EQ(client.localAdopt->consume(std::span(*command).first(Protocol::Local::adoptWireFrameHeaderBytes), admit, deliver).consumedBytes,
        Protocol::Local::adoptWireFrameHeaderBytes);
    CHECK_EQ(client.localAdopt->pendingBytes(), Protocol::Local::adoptWireFrameHeaderBytes);
    CHECK_EQ(client.localAdopt->retainedBytes(), command->size());
    refuses();
    CHECK_EQ(client.localAdopt->consume(std::span(*command).subspan(Protocol::Local::adoptWireFrameHeaderBytes), admit, deliver).consumedBytes,
        command->size() - Protocol::Local::adoptWireFrameHeaderBytes);
    CHECK_EQ(client.localAdopt->pendingBytes(), 0u);
    CHECK_EQ(client.localAdopt->retainedBytes(), command->size());
    refuses();
    client.localAdopt.reset();
    client.isReadingFrames = true;
    refuses();
    client.isReadingFrames = false;
    CHECK(!Access::hasBlockedOperations(fixture.daemon));
    CHECK(Access::begin(fixture.daemon, Screen::now()));
    Access::end(fixture.daemon);
}

TEST(anOperationCreatedDuringQuiesceInvalidatesReadinessWithoutDroppingIt)
{
    Fixture fixture;
    const auto index = Screen::attach(fixture.daemon, fixture.session->id);
    fixture.start();
    fixture.step(0);
    fixture.step(5000);
    REQUIRE(Access::isQuiesced(fixture.daemon));
    auto& client = Access::client(fixture.daemon, index);
    client.pendingClipboardRequests.push_back(59);
    CHECK(!Access::isQuiesced(fixture.daemon));
    fixture.step(6000);
    CHECK(!Access::isActive(fixture.daemon));
    CHECK(Access::didFail(fixture.daemon));
    CHECK_EQ(client.pendingClipboardRequests.size(), 1u);
    CHECK_EQ(client.pendingClipboardRequests.front(), 59u);
}

TEST(aPartialTypedFrameBlocksReplacementButAnIdleConnectionDoesNot)
{
    Protocol::Sync::HostConnection connection({}, {});
    CHECK(!connection.hasHandoverBlockedOperations());
    const std::array<std::uint8_t, 1> prefix { std::to_underlying(Protocol::Sync::FrameKind::Greet) };
    connection.receiveStream(prefix, 1);
    CHECK(connection.hasHandoverBlockedOperations());
}

TEST(anAcceptedTypedCallBlocksUntilItsOnlyAnswerLeavesTheConnection)
{
    Fixture fixture;
    const auto index = Screen::attach(fixture.daemon, fixture.session->id);
    Screen::grantScope(fixture.daemon, index, { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(fixture.daemon, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(fixture.daemon, index, client);
    Screen::publish(fixture.daemon, *fixture.session, Runtime::FrameClass::Structural);
    Screen::deliver(fixture.daemon, index, client);
    auto& connection = *Access::client(fixture.daemon, index).syncConnection;
    CHECK(!connection.hasHandoverBlockedOperations());
    REQUIRE(client.call(R"({"jsonrpc":"2.0","id":1,"method":"host.ping"})"));
    connection.receiveStream(client.takeStream(Screen::now()), Screen::now());
    CHECK(connection.hasHandoverBlockedOperations());
    CHECK(!Access::begin(fixture.daemon, Screen::now()));
    const auto calls = connection.takeCalls();
    REQUIRE(calls.size() == 1);
    CHECK(calls.front().text == R"({"jsonrpc":"2.0","id":1,"method":"host.ping"})");
    CHECK(connection.hasHandoverBlockedOperations());
    CHECK(connection.answerCall(calls.front().callIndex, R"({"jsonrpc":"2.0","id":1,"result":{}})") == Protocol::Sync::CallAnswer::Sent);
    CHECK(connection.hasHandoverBlockedOperations());
    Screen::deliver(fixture.daemon, index, client);
    const auto results = client.takeResults();
    REQUIRE(results.size() == 1);
    CHECK(results.front() == R"({"jsonrpc":"2.0","id":1,"result":{}})");
    CHECK(!connection.hasHandoverBlockedOperations());
    CHECK(!Access::hasBlockedOperations(fixture.daemon));
    CHECK(Access::begin(fixture.daemon, Screen::now()));
    Access::end(fixture.daemon);
}

TEST(aRefusedTypedCallBlocksUntilItsOnlyAnswerLeavesTheConnection)
{
    Fixture fixture;
    const auto index = Screen::attach(fixture.daemon, fixture.session->id);
    Screen::grantScope(fixture.daemon, index, { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(fixture.daemon, index);
    UbiquittyTest::ScreenClient client(24, 80);
    Screen::greet(fixture.daemon, index, client);
    Screen::publish(fixture.daemon, *fixture.session, Runtime::FrameClass::Structural);
    Screen::deliver(fixture.daemon, index, client);
    auto& connection = *Access::client(fixture.daemon, index).syncConnection;
    REQUIRE(client.call(R"({"jsonrpc":"2.0","id":1,"method":"host.ping"})"));
    connection.receiveStream(client.takeStream(Screen::now()), Screen::now());
    const auto calls = connection.takeCalls();
    REQUIRE(calls.size() == 1);
    connection.refuseCall(calls.front().callIndex, R"({"jsonrpc":"2.0","id":1,"error":{"code":-32005,"message":"busy"}})", Protocol::Sync::CallRefusal::Busy);
    CHECK(connection.hasHandoverBlockedOperations());
    CHECK(Access::hasBlockedOperations(fixture.daemon));
    CHECK(!Access::begin(fixture.daemon, Screen::now()));
    Access::end(fixture.daemon);
    Screen::deliver(fixture.daemon, index, client);
    const auto results = client.takeResults();
    REQUIRE(results.size() == 1);
    CHECK(results.front() == R"({"jsonrpc":"2.0","id":1,"error":{"code":-32005,"message":"busy"}})");
    CHECK(!connection.hasHandoverBlockedOperations());
    CHECK(!Access::hasBlockedOperations(fixture.daemon));
}

TEST(aBusyAsReadAnswerRemainsOwnedAfterEarlierAnswersLeave)
{
    using namespace Protocol::Sync;
    auto policy = UbiquittyTest::syncHostPolicy(0);
    policy.limits.maxOutstandingCalls = 1;
    UbiquittyTest::SyncLink link(0, UbiquittyTest::syncDefaultFacts(), policy);
    answerBusyAsRead(link.host());
    const auto send = [&](unsigned id) {
        std::vector<std::uint8_t> body;
        encode(body, Call { .text = R"({"jsonrpc":"2.0","method":"host.ping","id":)" + std::to_string(id) + "}" });
        link.sendToHost(FrameKind::Call, body);
    };
    send(1);
    const auto calls = link.host().takeCalls();
    REQUIRE(calls.size() == 1);
    REQUIRE(link.host().answerCall(calls.front().callIndex, R"({"jsonrpc":"2.0","id":1,"result":{}})") == CallAnswer::Sent);
    send(2);
    CHECK(link.host().takeCalls().empty());
    REQUIRE(link.host().counters().callsAnsweredBusy == 1);
    std::vector<std::uint8_t> first;
    REQUIRE(link.host().popInteractiveFrame(first));
    link.client().receiveStream(first, link.now());
    CHECK(link.host().hasHandoverBlockedOperations());
    std::vector<std::uint8_t> busy;
    REQUIRE(link.host().popInteractiveFrame(busy));
    link.client().receiveStream(busy, link.now());
    CHECK(!link.host().hasHandoverBlockedOperations());
    const auto results = link.client().takeResults();
    REQUIRE(results.size() == 2);
    CHECK(results[0].find(R"("id":1)") != std::string::npos);
    CHECK(results[1].find(R"("id":2)") != std::string::npos);
}

#ifdef UBIQUITTY_QUIC
TEST(aQUICReplyBlocksUntilItsUnsentAndUnacknowledgedOwnersDrain)
{
    Fixture fixture;
    const auto index = Screen::attach(fixture.daemon, fixture.session->id);
    auto& listener = Access::installQUIC(fixture.daemon);
    auto* connection = Host::adoptTestQUICConnection(listener, Host::makeTestQUICConnection());
    REQUIRE(connection);
    auto& client = Access::client(fixture.daemon, index);
    client.quicConnection = connection;
    const std::vector<std::uint8_t> reply { 'r', 'e', 'p', 'l', 'y' };
    client.outputBuffer = reply;
    Screen::flush(fixture.daemon, index);
    REQUIRE(!client.hasPendingOutput());
    REQUIRE(Host::testQUICConnectionPendingBytes(connection) == reply);
    REQUIRE(listener.unsentBytes(connection) == reply.size());
    CHECK(Access::hasBlockedOperations(fixture.daemon));
    CHECK(Access::blocker(fixture.daemon) == Protocol::Sync::HandoverRefuseReason::OutputUnsent);
    CHECK(!Access::begin(fixture.daemon, Screen::now()));
    Access::end(fixture.daemon);
    // Stage the actual QUIC buffer's written-but-unacknowledged boundary.
    Host::setTestQUICConnectionPending(connection, 0, reply.size());
    REQUIRE(listener.unsentBytes(connection) == 0);
    REQUIRE(listener.pendingBytes(connection) == reply.size());
    CHECK(Access::hasBlockedOperations(fixture.daemon));
    CHECK(!Access::begin(fixture.daemon, Screen::now()));
    Access::end(fixture.daemon);
    Host::testAckStreamData(connection, 0, 2);
    REQUIRE(listener.pendingBytes(connection) == reply.size() - 2);
    CHECK(Access::hasBlockedOperations(fixture.daemon));
    Host::testAckStreamData(connection, 2, reply.size() - 2);
    REQUIRE(listener.pendingBytes(connection) == 0);
    CHECK(!Access::hasBlockedOperations(fixture.daemon));
    CHECK(Access::begin(fixture.daemon, Screen::now()));
    Access::end(fixture.daemon);
}
#endif

int main(int argc, char** argv)
{
    UBIQUITTY_TEST_PIN_PREFERENCES();
    ::signal(SIGPIPE, SIG_IGN);
    return UbiquittyTest::runRegisteredCases(argc, argv);
}
