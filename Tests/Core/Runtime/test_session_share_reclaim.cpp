// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScreenPlaneFixture.h"
#include "WorkspaceStructureFixture.h"

// test_session_share_reclaim.cpp — a local session shared into the daemon, and
// taken back (TerminalSession::shareIntoLocalDaemon, requestReclaim).
//
// What sharing promises, each held to here against a real PTY and a child
// that is still running:
//
//   One reader. The daemon's duplicate of the master drives the shell from the
//   handoff on, so this side's reader stops before the master crosses and reads
//   nothing while the session is shared. Two readers would each take a share of
//   the shell's output and neither screen would be whole.
//
//   The shell stays this process's child, and its stream stays open. A reclaim
//   resumes the same stream — the output the shell wrote meanwhile is still in
//   the PTY for the resumed reader — with no reconnect and no new shell.
//
//   Only an answer to this session's own request hands it back. An
//   acknowledgement nobody asked for would resume a second reader beside the
//   daemon's.
//
// The daemon is a stub carrier and the descriptor handoff a function that
// records what it was given: the handoff is the platform's, and the daemon's
// side of both steps is Tests/Host/adopt_session.cpp's.

#include "Core/Protocol/Control/Reclaim.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/EffectCodec.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/SessionShare.h"
#include "Core/Session/ShareTerminalLimits.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Transport/Channel.h"
#include "PAL/UnixFileDescriptor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using Ubiquitty::Runtime::TerminalSession;

/// A carrier that reaches no daemon: the case plays the daemon's half by hand.
class StubChannel final : public Ubiquitty::Transport::Channel {
public:
    explicit StubChannel(StubChannel** liveSlot)
        : m_liveSlot(liveSlot)
    {
    }

    ~StubChannel() override
    {
        if (m_liveSlot && *m_liveSlot == this)
            *m_liveSlot = nullptr;
    }

    bool start(Ubiquitty::Transport::ChannelObserver* observer) override
    {
        m_observer = observer;
        return true;
    }
    void send(std::span<const std::uint8_t> bytes) override { sent.emplace_back(bytes.begin(), bytes.end()); }
    void connect() { m_observer->onConnect(); }
    void receive(const std::vector<std::uint8_t>& bytes) { m_observer->onData(bytes); }
    void loseTransport() { m_observer->onClose("handover"); }
    void stop() override { }
    [[nodiscard]] Ubiquitty::Transport::Origin origin() const override
    {
        return Ubiquitty::Transport::Origin { Ubiquitty::Transport::OriginKind::Daemon, "" };
    }
    [[nodiscard]] Ubiquitty::Transport::Health health() const override { return Ubiquitty::Transport::Health::Connecting; }
    [[nodiscard]] std::string description() const override { return "stub"; }

    std::vector<std::vector<std::uint8_t>> sent;

private:
    StubChannel** m_liveSlot = nullptr;
    Ubiquitty::Transport::ChannelObserver* m_observer = nullptr;
};

class StubFactory final : public Ubiquitty::Session::ChannelFactory {
public:
    void createChannel(Ubiquitty::Session::TransportMode, Done done) override
    {
        ++createCount;
        auto channel = std::make_unique<StubChannel>(&lastChannel);
        lastChannel = channel.get();
        done(std::move(channel), false);
    }

    [[nodiscard]] std::uintptr_t address() { return reinterpret_cast<std::uintptr_t>(static_cast<Ubiquitty::Session::ChannelFactory*>(this)); }

    int createCount = 0;
    StubChannel* lastChannel = nullptr;
};

std::vector<Ubiquitty::Runtime::PostedWorkToken>& postedWork()
{
    static std::vector<Ubiquitty::Runtime::PostedWorkToken> queue;
    return queue;
}

void queuePostedWork(Ubiquitty::Runtime::PostedWorkToken token) { postedWork().push_back(token); }
void queueDelayedWork(double, Ubiquitty::Runtime::PostedWorkToken token) { postedWork().push_back(token); }

Ubiquitty::Runtime::PostedWorkLoop queueingLoop() { return { .runSoon = &queuePostedWork, .runAfter = &queueDelayedWork }; }

// Another loop, with entry points of its own and a queue of its own, so a
// session handed it posts to a loop no anchor's connection posts to.
std::vector<Ubiquitty::Runtime::PostedWorkToken>& otherPostedWork()
{
    static std::vector<Ubiquitty::Runtime::PostedWorkToken> queue;
    return queue;
}

void queueOtherPostedWork(Ubiquitty::Runtime::PostedWorkToken token) { otherPostedWork().push_back(token); }
void queueOtherDelayedWork(double, Ubiquitty::Runtime::PostedWorkToken token) { otherPostedWork().push_back(token); }

Ubiquitty::Runtime::PostedWorkLoop otherQueueingLoop() { return { .runSoon = &queueOtherPostedWork, .runAfter = &queueOtherDelayedWork }; }

void pumpPostedWork(int rounds = 8)
{
    for (int round = 0; round < rounds && !postedWork().empty(); ++round) {
        std::vector<Ubiquitty::Runtime::PostedWorkToken> batch;
        batch.swap(postedWork());
        for (const Ubiquitty::Runtime::PostedWorkToken token : batch)
            Ubiquitty::Runtime::runPostedSessionWork(token);
    }
}

void discardPostedWork()
{
    for (auto* queue : { &postedWork(), &otherPostedWork() }) {
        for (const Ubiquitty::Runtime::PostedWorkToken token : *queue)
            Ubiquitty::Runtime::discardPostedSessionWork(token);
        queue->clear();
    }
}

/// What the stand-in for the platform's descriptor handoff was given.
struct HandoffRecord {
    int callCount = 0;
    std::string socketPath;
    // The platform receives a complete raw terminal body, without Local framing.
    std::vector<std::uint8_t> frame;
    int masterFileDescriptor = -1;
    Ubiquitty::Session::ShareHandoffLimits limits;
    /// What the daemon answers: the adopted id, or zero for a refusal.
    std::uint32_t answer = 0;
    /// The daemon process that adopts it.
    std::uint64_t daemonInstanceID = 17;
    /// The reason a refusal names, or none for a daemon that never answered.
    std::optional<Ubiquitty::Session::AdoptRejection> rejection;
};

HandoffRecord& handoffRecord()
{
    static HandoffRecord record;
    return record;
}

Ubiquitty::Session::ShareHandoffResult recordHandoff(const char* socketPath, std::span<const std::byte> frame, int masterFileDescriptor,
    const Ubiquitty::Session::ShareHandoffLimits& limits)
{
    HandoffRecord& record = handoffRecord();
    ++record.callCount;
    record.socketPath = socketPath;
    record.frame.clear();
    for (const std::byte value : frame)
        record.frame.push_back(static_cast<std::uint8_t>(value));
    record.masterFileDescriptor = masterFileDescriptor;
    record.limits = limits;
    using namespace Ubiquitty::Session;
    if (!record.answer && record.rejection) {
        auto attempt = std::make_unique<AdoptClientAttempt>(AdoptOperation { { 1, 2 }, 17 });
        const auto rejected = attempt->observe({ attempt->operation(), AdoptDisposition::Rejected, {}, record.rejection });
        CHECK(rejected.has_value());
        return { .ownership = ShareOwnership::Local, .adopted = {}, .attempt = std::move(attempt) };
    }
    if (!record.answer)
        return {};
    auto attempt = std::make_unique<AdoptClientAttempt>(AdoptOperation { { 1, 2 }, record.daemonInstanceID });
    const AdoptTarget target { record.answer, { record.answer, record.daemonInstanceID } };
    const auto prepared = attempt->observe({ attempt->operation(), AdoptDisposition::Prepared, target, {} });
    CHECK(prepared.has_value());
    if (!prepared)
        return { .ownership = ShareOwnership::Unresolved, .adopted = {}, .attempt = std::move(attempt) };
    const auto commit = attempt->beginCommit();
    CHECK(commit.has_value());
    if (!commit)
        return { .ownership = ShareOwnership::Unresolved, .adopted = {}, .attempt = std::move(attempt) };
    const auto committed = attempt->observe({ attempt->operation(), AdoptDisposition::Committed, target, {} });
    CHECK(committed.has_value());
    return { .ownership = committed ? ShareOwnership::Committed : ShareOwnership::Unresolved,
        .adopted = { record.answer, record.daemonInstanceID },
        .attempt = std::move(attempt) };
}

std::uintptr_t handoffAddress()
{
    const Ubiquitty::Session::AdoptHandoff handoff = &recordHandoff;
    return reinterpret_cast<std::uintptr_t>(handoff);
}

template<typename Condition>
bool waitFor(Condition condition, int budgetMilliseconds)
{
    for (int waited = 0; waited < budgetMilliseconds; waited += 10) {
        if (condition())
            return true;
        ::usleep(10 * 1000);
    }
    return condition();
}

/// Whether any visible row of a stopped session's model carries `text`.
bool screenContains(TerminalSession& session, const std::string& text)
{
    const Ubiquitty::Terminal* term = session.terminal();
    if (term == nullptr)
        return false;
    for (int row = 0; row < term->rows; ++row) {
        std::string line;
        for (int column = 0; column < term->columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(term, 0, row, column);
            const std::uint32_t codePoint = cell ? cell->codePoint : 0;
            line.push_back(codePoint >= 32 && codePoint < 127 ? static_cast<char>(codePoint) : ' ');
        }
        if (line.find(text) != std::string::npos)
            return true;
    }
    return false;
}

/// A session reading a live shell that prints a line for each one it reads.
///
/// - Returns: The session, or null having failed the case.
[[nodiscard]] TerminalSession* makeReadingSession()
{
    TerminalSession* session = TerminalSession::make();
    CHECK_MSG(session != nullptr, "TerminalSession::make() returned nothing");
    if (session == nullptr)
        return nullptr;
    session->start(24, 80);
    const bool isLaunched = session->launchShellCommand(
        "printf 'ready\\n'; while IFS= read -r line; do printf 'echoed-%s\\n' \"$line\"; done");
    CHECK_MSG(isLaunched, "the shell did not launch");
    CHECK_MSG(isLaunched && session->startIngest(), "the reader did not start");
    CHECK_MSG(waitFor([&] { return session->takeDrainedBytes() > 0; }, 5000), "the shell's first line was never read");
    return session;
}

/// Writes one line into the PTY as the shell's typist, past the session.
void typeIntoMaster(TerminalSession& session, const std::string& line, int daemonMaster = -1)
{
    const std::string bytes = line + "\n";
    const int descriptor = daemonMaster >= 0 ? daemonMaster : session.fileDescriptor();
    CHECK(::write(descriptor, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
}

/// Plays the daemon's side of attaching the adopted session: the carrier
/// connects, the handshake names the session, and the workspace admits it.
void acknowledgeAdoptedSession(StubFactory& factory, std::uint32_t sessionID)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    REQUIRE(sessionID == handoffRecord().answer);
    factory.lastChannel->connect();
    pumpPostedWork();
    UbiquittyTest::HostScreen host(sessionID, 17);
    auto& connection = host.plane().connection();
    connection.setGreetingAdmission([sessionID](const Sync::Greet& greeting) -> std::expected<Sync::HostIdentity, Sync::Bye> {
        const auto& request = greeting.request;
        if (request.target != Sync::AdmissionTarget::Workspace || request.sessionID != sessionID || !request.isAttachOnly
            || (request.workspaceID && request.workspaceID != 5) || request.expectedDaemonInstanceID != 17)
            return std::unexpected(Sync::Bye { .reason = Sync::ByeReason::SessionGone, .detail = "the fixture holds only the exact locally adopted session" });
        Sync::HostIdentity identity;
        identity.selection = { .target = Sync::AdmissionTarget::Workspace,
            .sessionID = sessionID,
            .workspaceID = 5,
            .daemonInstanceID = 17,
            .socketName = "ubq-share-test.sock",
            .buildFlags = 0,
            .hostPlatform = "fixture" };
        identity.mayDrive = true;
        identity.maxRole = Sync::Role::Owner;
        identity.verifiedKind = Sync::ClientKind::Human;
        return identity;
    });
    const auto writes = factory.lastChannel->sent;
    for (const auto& bytes : writes)
        connection.receiveStream(bytes, host.plane().now());
    host.passOver(writes.size());
    REQUIRE(connection.state() == Sync::ConnectionState::Attached);
    factory.lastChannel->receive(host.flush());
    pumpPostedWork();
    if (factory.lastChannel == nullptr)
        return;

    // The sole Welcome selects this workspace; its structure reaches the
    // client before the host binds the adopted pane.
    const UbiquittyTest::StructurePane only { .pane = 1, .sessionID = sessionID };
    CHECK(UbiquittyTest::offerStructure(host.plane().connection(), UbiquittyTest::rowWorkspace(5, {}, { &only, 1 }, 17)));
    factory.lastChannel->receive(host.flush());
    pumpPostedWork();
    host.receive(factory.lastChannel->sent);
    factory.lastChannel->receive(host.flush());
    pumpPostedWork();
}

/// Reads the exact canonical call rather than accepting a raw reclaim tag.
std::optional<Ubiquitty::Protocol::Control::DecodedRequest> lastReclaimCall(const StubFactory& factory)
{
    if (!factory.lastChannel)
        return std::nullopt;
    namespace Sync = Ubiquitty::Protocol::Sync;
    Sync::FrameDecoder decoder;
    std::optional<Ubiquitty::Protocol::Control::DecodedRequest> found;
    for (const auto& write : factory.lastChannel->sent) {
        decoder.append(write);
        while (const auto frame = decoder.next()) {
            if (frame->kind != Sync::FrameKind::Call)
                continue;
            CHECK_EQ(frame->channel, 0u);
            const auto call = Sync::decodeCall(frame->body);
            if (!call)
                continue;
            auto request = Sync::decodeCallRequest(call->text);
            if (request.outcome == Ubiquitty::Protocol::Control::RequestOutcome::Dispatch && request.method == "pane.reclaim")
                found = std::move(request);
        }
    }
    CHECK(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
    return found;
}

/// Sends only the exact correlated Result; it returns no descriptor.
void receiveReclaimResult(StubFactory& factory, std::uint32_t sessionID, std::uint64_t daemon = 17, std::uint32_t handle = 1, std::uint64_t attachmentLow = 1)
{
    const auto request = lastReclaimCall(factory);
    const auto id = Ubiquitty::Codec::JSON::Value::string(request ? request->id.asString() : "reclaim:unrequested");
    const Ubiquitty::Protocol::Control::ReclaimTarget target { .handle = handle, .sessionID = sessionID, .daemonInstanceID = daemon, .hostInstanceID = { daemon, 0 }, .attachmentID = { attachmentLow, daemon }, .paneID = { sessionID, 17 } };
    namespace Sync = Ubiquitty::Protocol::Sync;
    Sync::Result result;
    result.text = Ubiquitty::Codec::JSON::serialize(Ubiquitty::Protocol::Control::reclaimReleasedResult(&id, target));
    std::vector<std::uint8_t> body, frame;
    Sync::encode(body, result);
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Result, 0, body));
    factory.lastChannel->receive(frame);
    pumpPostedWork();
}

} // namespace

TEST(aSharedSessionHasOneReaderAndResumesItsOwnStreamOnReclaim)
{
    discardPostedWork();
    TerminalSession* session = makeReadingSession();
    if (session == nullptr)
        return;
    REQUIRE(session->isShareableLocalSession());

    handoffRecord() = {};
    handoffRecord().answer = 42;
    const int originalMaster = session->fileDescriptor();
    StubFactory factory;
    const std::uint32_t sessionID = session->shareIntoLocalDaemonOverCarrier(
        "/tmp/ubq-share-test.sock", handoffAddress(), factory.address(), queueingLoop());
    CHECK_EQ(sessionID, 42u);
    CHECK_EQ(handoffRecord().callCount, 1);
    CHECK_STR_EQ(handoffRecord().socketPath, "/tmp/ubq-share-test.sock");
    CHECK_MSG(handoffRecord().masterFileDescriptor == originalMaster,
        "the handoff was given something other than the session's own master");
    CHECK(session->fileDescriptor() == -1);

    // The sender is given the complete private terminal, not one-phase frames.
    auto adoptMessage = Ubiquitty::Protocol::Sync::decodeHandoverTerminal(handoffRecord().frame,
        Ubiquitty::Session::localShareTerminalLimits(Ubiquitty::Protocol::Local::maxAdoptBodyBytes));
    REQUIRE(adoptMessage && adoptMessage->terminal);
    CHECK(handoffRecord().limits.maxRecordBytes > handoffRecord().frame.size());
    CHECK(handoffRecord().limits.resolutionAttempts == 2);
    CHECK_EQ(adoptMessage->generation, 1u);
    CHECK_EQ(adoptMessage->terminal->columns, 80);
    CHECK_EQ(adoptMessage->terminal->rows, 24);

    CHECK_MSG(session->isSharedIntoLocalDaemon(), "a session the daemon adopted does not say it is shared");
    CHECK(session->remoteDaemonInstanceID() == 17);
    CHECK_MSG(session->sharedSessionID() == 42, "a shared session does not name the id the daemon adopted it under");
    CHECK_MSG(session->sharedBridgeCommand().empty(), "a share over the embedder's carrier named a bridge of its own");
    CHECK_MSG(session->isRemoteAttached(), "a shared session is not the adopted session's thin client");
    CHECK_MSG(!session->isShareableLocalSession(), "a shared session offers to be shared again");

    // One reader: the shell answers a line typed into its PTY, and nothing on
    // this side reads the answer while the daemon drives the session.
    (void)session->takeDrainedBytes();
    typeIntoMaster(*session, "while-shared", handoffRecord().masterFileDescriptor);
    ::usleep(300 * 1000);
    CHECK_MSG(session->takeDrainedBytes() == 0, "this side still read the PTY while the daemon was its reader");

    acknowledgeAdoptedSession(factory, sessionID);
    REQUIRE(factory.lastChannel != nullptr);
    CHECK_MSG(session->isReclaimableSharedSession(), "a connected shared session cannot be taken back");

    // A split or a zoom would ask the daemon's workspace for panes this
    // window does not show; closing the pane is still the window's.
    CHECK(session->requestPaneVerb(TerminalSession::PaneVerb::SplitBeside) == TerminalSession::PaneVerbOutcome::Unsupported);
    CHECK(session->requestPaneVerb(TerminalSession::PaneVerb::Zoom) == TerminalSession::PaneVerbOutcome::Unsupported);
    CHECK(session->requestPaneVerb(TerminalSession::PaneVerb::Close) == TerminalSession::PaneVerbOutcome::LocalActionRequired);

    // An acknowledgement nobody asked for hands nothing back.
    receiveReclaimResult(factory, sessionID);
    CHECK_MSG(session->isSharedIntoLocalDaemon() && session->isRemoteAttached(),
        "an unrequested reclaim Result resumed this side's reader beside the daemon's");
    REQUIRE(factory.lastChannel != nullptr);

    CHECK(session->requestReclaim());
    pumpPostedWork();
    REQUIRE(factory.lastChannel != nullptr && !factory.lastChannel->sent.empty());
    const auto request = lastReclaimCall(factory);
    REQUIRE(request);
    const auto reclaim = Ubiquitty::Protocol::Control::readReclaimRequest(request->params);
    CHECK(reclaim && reclaim->handle == 1 && reclaim->daemonInstanceID == 17);

    // The daemon hands it back: the session is local again, with no new
    // carrier asked for, and the reader resumes on the stream it kept.
    const int carriersBeforeReclaim = factory.createCount;
    receiveReclaimResult(factory, sessionID);
    CHECK_MSG(!session->isSharedIntoLocalDaemon(), "the reclaim left the session shared");
    CHECK_MSG(!session->isRemoteAttached(), "the reclaim left the thin client attached");
    CHECK(session->remoteState() == Ubiquitty::Session::State::Idle);
    CHECK_EQ(factory.createCount, carriersBeforeReclaim);
    CHECK_MSG(waitFor([&] { return session->takeDrainedBytes() > 0; }, 5000),
        "the resumed reader drained nothing the shell wrote while shared");
    typeIntoMaster(*session, "after-reclaim");
    CHECK_MSG(waitFor([&] { return session->takeDrainedBytes() > 0; }, 5000), "the resumed reader stopped reading");

    // The shell that answers is the one that was shared: still this process's
    // child, still running, and shareable again.
    session->stopIngest();
    CHECK_MSG(screenContains(*session, "echoed-while-shared"), "the line the shell answered while shared never reached the screen");
    CHECK_MSG(screenContains(*session, "echoed-after-reclaim"), "the shell stopped answering after the reclaim");
    CHECK_MSG(session->isShareableLocalSession(), "the reclaimed shell is gone or no longer this process's");

    Ubiquitty::PAL::UnixFileDescriptor retainedMaster(::dup(session->fileDescriptor()));
    CHECK(retainedMaster.isValid());
    session->close();
    releaseTerminalSession(session);
    CHECK_MSG(waitFor([&] {
        std::array<char, 1024> bytes;
        const auto count = ::read(retainedMaster.get(), bytes.data(), bytes.size());
        return count == 0 || (count < 0 && errno == EIO);
    },
                  3000),
        "closing a reclaimed session must end its locally owned shell");
    discardPostedWork();
}

TEST(aSharedSessionReportsItsAdmittedSuccessorInsteadOfTheOriginalAdoptHost)
{
    discardPostedWork();
    StubFactory factory;
    const auto cleanup = [](TerminalSession* session) {
        session->close();
        releaseTerminalSession(session);
        discardPostedWork();
    };
    std::unique_ptr<TerminalSession, decltype(cleanup)> session(makeReadingSession(), cleanup);
    REQUIRE(session);
    handoffRecord() = {};
    handoffRecord().answer = 42;
    REQUIRE(session->shareIntoLocalDaemonOverCarrier(
                "/tmp/ubq-share-test.sock", handoffAddress(), factory.address(), queueingLoop())
        == 42u);
    acknowledgeAdoptedSession(factory, 42);
    REQUIRE(factory.lastChannel);
    CHECK_EQ(session->remoteDaemonInstanceID(), 17u);
    factory.lastChannel->loseTransport();
    pumpPostedWork();
    REQUIRE(factory.lastChannel);
    factory.lastChannel->connect();
    pumpPostedWork();
    namespace Sync = Ubiquitty::Protocol::Sync;
    std::vector<std::uint8_t> envelope;
    const auto append = [&](Sync::FrameKind kind, const auto& message) {
        std::vector<std::uint8_t> body;
        Sync::encode(body, message);
        REQUIRE(Sync::appendFrame(envelope, kind, 0, body));
    };
    Sync::Welcome welcome;
    welcome.wireVersion = Sync::wireVersion;
    welcome.hostInstanceID = { 23, 0 };
    welcome.attachmentID = { 2, 23 };
    welcome.resumeSecret = { 3, 23 };
    welcome.selection = { .target = Sync::AdmissionTarget::Workspace,
        .sessionID = 42,
        .workspaceID = 5,
        .daemonInstanceID = 23,
        .socketName = "ubq-share-test.sock",
        .buildFlags = 0,
        .hostPlatform = "fixture" };
    welcome.predecessorHostInstanceID = Sync::ID128 { 17, 0 };
    welcome.resumed = { { .paneID = { 42, 17 }, .outcome = Sync::ResumeOutcome::Diffed, .inputWritten = 0 } };
    append(Sync::FrameKind::Welcome, welcome);
    REQUIRE(envelope.size() > 1);
    const std::size_t split = envelope.size() - 1;
    factory.lastChannel->receive({ envelope.begin(), envelope.begin() + static_cast<std::ptrdiff_t>(split) });
    pumpPostedWork();
    CHECK_EQ(session->remoteDaemonInstanceID(), 17u);
    REQUIRE(factory.lastChannel);
    const UbiquittyTest::StructurePane only { .pane = 1, .sessionID = 42 };
    Sync::WorkspaceStructureHead structure { .workspaceID = 5, .revision = 2 };
    REQUIRE(Ubiquitty::Session::encodeWorkspaceStructure(structure.record, UbiquittyTest::rowWorkspace(5, {}, { &only, 1 }, 17, 2)));
    append(Sync::FrameKind::WorkspaceStructure, structure);
    append(Sync::FrameKind::PaneBind, Sync::PaneBind { .handle = 11, .paneID = { 42, 17 } });
    factory.lastChannel->receive({ envelope.begin() + static_cast<std::ptrdiff_t>(split), envelope.end() });
    pumpPostedWork();
    CHECK(session->remoteState() == Ubiquitty::Session::State::Connected);
    CHECK_EQ(session->remoteDaemonInstanceID(), 23u);
    CHECK_EQ(session->sharedSessionID(), 42u);
    CHECK(session->isReclaimableSharedSession());
    CHECK(session->requestReclaim());
    receiveReclaimResult(factory, 42, 23, 11, 2);
    CHECK(!session->isSharedIntoLocalDaemon());
}

TEST(aTransferredShellSurvivesClosingItsOriginalSession)
{
    TerminalSession* session = makeReadingSession();
    if (!session) {
        return;
    }
    handoffRecord() = {};
    handoffRecord().answer = 57;
    CHECK_EQ(session->handOverToLocalDaemon("/tmp/ubq-share-test.sock", handoffAddress()), 57u);
    Ubiquitty::PAL::UnixFileDescriptor daemonMaster(::dup(handoffRecord().masterFileDescriptor));
    CHECK(daemonMaster.isValid());
    session->close();
    releaseTerminalSession(session);
    ::usleep(100 * 1000);

    constexpr std::string_view input = "after-client-close\n";
    CHECK(::write(daemonMaster.get(), input.data(), input.size()) == static_cast<ssize_t>(input.size()));
    std::string output;
    CHECK_MSG(waitFor([&] {
        std::array<char, 1024> bytes;
        const auto count = ::read(daemonMaster.get(), bytes.data(), bytes.size());
        if (count > 0) {
            output.append(bytes.data(), static_cast<std::size_t>(count));
        }
        return output.find("echoed-after-client-close") != std::string::npos;
    },
                  3000),
        "the daemon's endpoint must still reach the original running shell after its client closes");
}

/// A quit hands the shell over and leaves nothing attached: the application is
/// ending, and the next launch rejoins the shell by the id the daemon gave.
TEST(aQuitHandsTheShellToTheDaemonAndAttachesNoClient)
{
    discardPostedWork();
    TerminalSession* session = makeReadingSession();
    if (session == nullptr)
        return;

    handoffRecord() = {};
    handoffRecord().answer = 57;
    const int originalMaster = session->fileDescriptor();
    CHECK_EQ(session->handOverToLocalDaemon("/tmp/ubq-share-test.sock", handoffAddress()), 57u);
    CHECK_EQ(handoffRecord().callCount, 1);
    CHECK_STR_EQ(handoffRecord().socketPath, "/tmp/ubq-share-test.sock");
    CHECK_MSG(handoffRecord().masterFileDescriptor == originalMaster,
        "the hand-over was given something other than the session's own master");
    CHECK(session->fileDescriptor() == -1);
    CHECK_MSG(session->isSharedIntoLocalDaemon() && session->sharedSessionID() == 57,
        "the session does not name the daemon session its shell went to");
    CHECK_MSG(!session->isRemoteAttached(), "a quit's hand-over attached a client to the session it handed away");
    CHECK_MSG(postedWork().empty(), "a quit's hand-over started a connection");

    // One reader: the daemon's.
    (void)session->takeDrainedBytes();
    typeIntoMaster(*session, "after-quit", handoffRecord().masterFileDescriptor);
    ::usleep(300 * 1000);
    CHECK_MSG(session->takeDrainedBytes() == 0, "this side still read the PTY after handing it over");

    // Handed over once: a second quit capture has nothing left to hand.
    CHECK_EQ(session->handOverToLocalDaemon("/tmp/ubq-share-test.sock", handoffAddress()), 0u);
    CHECK_EQ(handoffRecord().callCount, 1);

    session->close();
    releaseTerminalSession(session);

    // A daemon that refuses leaves the shell local and read.
    TerminalSession* refused = makeReadingSession();
    if (refused != nullptr) {
        handoffRecord() = {};
        handoffRecord().answer = 0;
        CHECK_EQ(refused->handOverToLocalDaemon("/tmp/ubq-share-test.sock", handoffAddress()), 0u);
        CHECK(!refused->isSharedIntoLocalDaemon());
        typeIntoMaster(*refused, "after-refused-quit");
        CHECK_MSG(waitFor([&] { return refused->takeDrainedBytes() > 0; }, 5000), "a refused hand-over left the reader stopped");
        refused->close();
        releaseTerminalSession(refused);
    }
    discardPostedWork();
}

/// A plain tmux client remains a local PTY owner while its foreground program
/// uses the terminal, so the quit handoff must offer that master to the host.
TEST(aPlainTmuxClientCanBeHandedToTheLocalHost)
{
    if (std::system("command -v tmux >/dev/null 2>&1") != 0)
        return;
    discardPostedWork();
    const std::string socketName = "ubq-share-" + std::to_string(::getpid());
    const std::string command = "tmux -L " + socketName + " -f /dev/null new-session -s handoff";
    TerminalSession* session = TerminalSession::make();
    REQUIRE(session != nullptr);
    session->start(24, 80);
    REQUIRE(session->launchShellCommand(command));
    REQUIRE(session->startIngest());
    CHECK_MSG(waitFor([&] { return session->takeDrainedBytes() > 0; }, 5000), "the plain tmux client drew its first screen");
    CHECK_MSG(session->isShareableLocalSession(), "a plain tmux client lost its local PTY owner");

    handoffRecord() = {};
    handoffRecord().answer = 73;
    CHECK_EQ(session->handOverToLocalDaemon("/tmp/ubq-share-test.sock", handoffAddress()), 73u);
    CHECK_EQ(handoffRecord().callCount, 1);
    CHECK_MSG(session->isSharedIntoLocalDaemon(), "the tmux PTY was not adopted");

    session->close();
    releaseTerminalSession(session);
    const std::string stop = "tmux -L " + socketName + " kill-server >/dev/null 2>&1";
    (void)std::system(stop.c_str());
    discardPostedWork();
}

TEST(aRefusedHandoffLeavesTheSessionLocalAndReading)
{
    discardPostedWork();
    TerminalSession* session = makeReadingSession();
    if (session == nullptr)
        return;

    handoffRecord() = {};
    handoffRecord().answer = 0;
    StubFactory factory;
    CHECK_EQ(session->shareIntoLocalDaemonOverCarrier(
                 "/tmp/ubq-share-test.sock", handoffAddress(), factory.address(), queueingLoop()),
        0u);
    CHECK_EQ(handoffRecord().callCount, 1);
    CHECK(!session->isSharedIntoLocalDaemon());
    CHECK(!session->isRemoteAttached());
    CHECK_MSG(factory.createCount == 0, "a refused share still attached a thin client");

    typeIntoMaster(*session, "after-refusal");
    CHECK_MSG(waitFor([&] { return session->takeDrainedBytes() > 0; }, 5000), "a refused share left the reader stopped");
    session->stopIngest();
    CHECK(screenContains(*session, "echoed-after-refusal"));

    session->close();
    releaseTerminalSession(session);
    discardPostedWork();
}

TEST(aSessionWithNothingToHandOverIsNotShared)
{
    discardPostedWork();
    handoffRecord() = {};
    handoffRecord().answer = 42;

    // No child: nothing for the daemon to adopt.
    TerminalSession* idle = TerminalSession::make();
    REQUIRE(idle != nullptr);
    idle->start(24, 80);
    CHECK(!idle->isShareableLocalSession());
    StubFactory idleFactory;
    CHECK_EQ(idle->shareIntoLocalDaemonOverCarrier("/tmp/ubq-share-test.sock", handoffAddress(), idleFactory.address(), queueingLoop()),
        0u);
    releaseTerminalSession(idle);

    // A thin client already: its model mirrors a host, and it has no master.
    TerminalSession* mirror = TerminalSession::make();
    REQUIRE(mirror != nullptr);
    mirror->start(24, 80);
    StubFactory mirrorFactory;
    REQUIRE(mirror->attachRemoteOverCarrier(mirrorFactory.address(), 0, queueingLoop()));
    CHECK(!mirror->isShareableLocalSession());
    CHECK(!mirror->isReclaimableSharedSession());
    CHECK(!mirror->requestReclaim());
    releaseTerminalSession(mirror);

    // Neither a socket nor a handoff: nothing to share through.
    TerminalSession* session = makeReadingSession();
    if (session != nullptr) {
        StubFactory factory;
        CHECK_EQ(session->shareIntoLocalDaemonOverCarrier("", handoffAddress(), factory.address(), queueingLoop()), 0u);
        CHECK_EQ(session->shareIntoLocalDaemonOverCarrier("/tmp/ubq-share-test.sock", 0, factory.address(), queueingLoop()), 0u);
        CHECK(session->shareIntoLocalDaemon("/usr/bin/false", 0, queueingLoop())
            == Ubiquitty::Session::ShareOutcome::NotShareable);
        CHECK(!session->isSharedIntoLocalDaemon());
        session->close();
        releaseTerminalSession(session);
    }
    CHECK_MSG(handoffRecord().callCount == 0, "a share with nothing to hand over reached the daemon");
    discardPostedWork();
}

/// The binary a share reaches the daemon through decides the socket, and a
/// daemon that is not there decides nothing: each refusal leaves the session
/// local and reading, and none of them reaches the handoff.
TEST(aShareWithNoDaemonToReachLeavesTheSessionLocal)
{
    discardPostedWork();
    handoffRecord() = {};
    handoffRecord().answer = 42;
    const char* previous = std::getenv("UBIQUITTY_HOME");
    const std::string saved = previous ? previous : "";
    const std::string home = "/tmp/ubq-share-reclaim-" + std::to_string(::getpid());
    ::setenv("UBIQUITTY_HOME", home.c_str(), 1);

    TerminalSession* session = makeReadingSession();
    if (session != nullptr) {
        // No identity: the binary cannot be read, so it names no socket.
        CHECK(session->shareIntoLocalDaemon("/nonexistent/ubiquitty-host-share", handoffAddress(), queueingLoop())
            == Ubiquitty::Session::ShareOutcome::NoDaemonSocket);
        // An identity, and a start that fails: nothing answers on its socket.
        CHECK(session->shareIntoLocalDaemon("/usr/bin/false", handoffAddress(), queueingLoop())
            == Ubiquitty::Session::ShareOutcome::DaemonDidNotStart);
        CHECK(!session->isSharedIntoLocalDaemon());
        CHECK(!session->isRemoteAttached());
        typeIntoMaster(*session, "still-local");
        CHECK_MSG(waitFor([&] { return session->takeDrainedBytes() > 0; }, 5000), "a failed share left the reader stopped");
        session->close();
        releaseTerminalSession(session);
    }
    CHECK_MSG(handoffRecord().callCount == 0, "a share with no daemon to reach still handed the master over");

    if (previous)
        ::setenv("UBIQUITTY_HOME", saved.c_str(), 1);
    else
        ::unsetenv("UBIQUITTY_HOME");
    discardPostedWork();
}

/// A daemon at its session cap rejects the share as full, and the outcome
/// says so rather than the generic refusal: nothing about the window was
/// wrong. A rejection for any other reason stays the generic one. Either way
/// the session stays local and reading.
TEST(aFullHostRejectsAShareAsFull)
{
    discardPostedWork();
    const char* previous = std::getenv("UBIQUITTY_HOME");
    const std::string saved = previous ? previous : "";
    const std::string home = "/tmp/ubq-share-full-" + std::to_string(::getpid());
    ::mkdir(home.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", home.c_str(), 1);

    // The binary's daemon, answering on the socket a share looks for.
    const std::string binary = "/usr/bin/true";
    const std::string socketPath = Ubiquitty::Session::localDaemonSocketPath(binary);
    REQUIRE(!socketPath.empty());
    ::unlink(socketPath.c_str());
    const Ubiquitty::PAL::UnixFileDescriptor listener(::socket(AF_UNIX, SOCK_STREAM, 0));
    REQUIRE(listener.get() >= 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketPath.c_str());
    REQUIRE(::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener.get(), 4) == 0);

    using Ubiquitty::Session::AdoptRejection;
    using Ubiquitty::Session::ShareOutcome;
    const std::pair<AdoptRejection, ShareOutcome> answers[] = {
        { AdoptRejection::HostFull, ShareOutcome::HostFull },
        { AdoptRejection::InvalidCandidate, ShareOutcome::HandoffRefused },
    };
    for (const auto& [rejection, outcome] : answers) {
        TerminalSession* session = makeReadingSession();
        if (session == nullptr)
            break;
        handoffRecord() = {};
        handoffRecord().rejection = rejection;
        CHECK(session->shareIntoLocalDaemon(binary, handoffAddress(), queueingLoop()) == outcome);
        CHECK_EQ(handoffRecord().callCount, 1);
        CHECK(!session->isSharedIntoLocalDaemon());
        CHECK(!session->isRemoteAttached());
        typeIntoMaster(*session, "after-full");
        CHECK_MSG(waitFor([&] { return session->takeDrainedBytes() > 0; }, 5000), "a rejected share left the reader stopped");
        session->close();
        releaseTerminalSession(session);
        discardPostedWork();
    }

    ::unlink(socketPath.c_str());
    ::rmdir(home.c_str());
    if (previous)
        ::setenv("UBIQUITTY_HOME", saved.c_str(), 1);
    else
        ::unsetenv("UBIQUITTY_HOME");
    discardPostedWork();
}

// ---- a shared window on one carrier ----
//
// Share This Window hands each pane's PTY to the daemon on its own, and the
// window's panes then ride one connection: the first pane attaches and
// publishes the window, and every other pane joins that connection as the
// host places its session in the published workspace. The cases count what
// a window of four panes costs the daemon — carriers, greetings, admissions
// and pane bindings — and hold each pane to its own input, questions,
// effects and reclaim on the shared carrier.

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;

/// The local daemon's end of one attachment of a shared window, without a
/// daemon.
///
/// It admits the anchor's adopted session in a workspace of its own, answers
/// the window's publication, lays the window out in the published workspace,
/// and binds each of its panes to the attachment, in the order the daemon's
/// publication does: the structure first, then the panes it names. Hosts made
/// for one daemon share its panes, as the daemon binds a session's one pane
/// source into every attachment: one program, one input stream, and one
/// effect log whose sequences every attachment shares.
class SharedWindowHost {
public:
    /// Creates the host end of the first attachment to a daemon.
    ///
    /// - Parameters:
    ///   - anchorSessionID: The session the anchor's Greet names.
    ///   - workspaceID: The workspace its Welcome admits it to.
    ///   - daemonInstanceID: The daemon process the Welcome names.
    SharedWindowHost(std::uint32_t anchorSessionID, std::uint32_t workspaceID, std::uint64_t daemonInstanceID = 17)
        : SharedWindowHost(anchorSessionID, workspaceID, daemonInstanceID, std::make_shared<SharedDaemon>())
    {
    }

    /// Creates the host end of another attachment to the daemon an existing
    /// host stands for, sharing its panes.
    ///
    /// - Parameters:
    ///   - anchorSessionID: The session this attachment's Greet names.
    ///   - workspaceID: The workspace its Welcome admits it to.
    ///   - sameDaemon: A host of the same daemon.
    SharedWindowHost(std::uint32_t anchorSessionID, std::uint32_t workspaceID, SharedWindowHost& sameDaemon)
        : SharedWindowHost(anchorSessionID, workspaceID, sameDaemon.m_daemonInstanceID, sameDaemon.m_daemon)
    {
    }
    SharedWindowHost(const SharedWindowHost&) = delete;
    SharedWindowHost& operator=(const SharedWindowHost&) = delete;

    /// Reads every frame the client wrote on a carrier since the last call.
    ///
    /// - Parameter channel: The carrier.
    void receive(const StubChannel& channel)
    {
        for (; m_consumed < channel.sent.size(); ++m_consumed) {
            const std::vector<std::uint8_t>& bytes = channel.sent[m_consumed];
            m_clientFrames.append(bytes);
            while (const auto frame = m_clientFrames.next()) {
                if (frame->kind == Sync::FrameKind::Greet)
                    ++m_greets;
            }
            m_connection.receiveStream(bytes, advance());
        }
        for (Sync::ReceivedCall& call : m_connection.takeCalls()) {
            // As the daemon does: an attachment holds one reclaim awaiting
            // release, and another it sends meanwhile is refused as busy.
            const auto request = Sync::decodeCallRequest(call.text);
            if (request.outcome == Ubiquitty::Protocol::Control::RequestOutcome::Dispatch && request.method == "pane.reclaim"
                && pendingCalls("pane.reclaim") != 0) {
                ++m_refusedReclaims;
                const auto refusal = Ubiquitty::Protocol::Control::rpcError(request.idOrNull(), Ubiquitty::Protocol::Control::ErrorCode::Busy,
                    "this attachment already has a reclaim awaiting release");
                CHECK(m_connection.answerCall(call.callIndex, Ubiquitty::Codec::JSON::serialize(refusal)) == Sync::CallAnswer::Sent);
                continue;
            }
            m_calls.push_back(std::move(call));
        }
    }

    /// Services the connection and returns what the host sends.
    ///
    /// - Returns: Canonical envelopes, each whole.
    [[nodiscard]] std::vector<std::uint8_t> flush()
    {
        const std::uint64_t now = advance();
        m_connection.service(now);
        std::vector<std::uint8_t> out;
        std::vector<std::uint8_t> envelope;
        while (m_connection.popStreamFrame(envelope, now)) {
            out.insert(out.end(), envelope.begin(), envelope.end());
            envelope.clear();
        }
        m_hostFrames.append(out);
        while (const auto frame = m_hostFrames.next()) {
            if (frame->kind == Sync::FrameKind::Welcome)
                ++m_welcomes;
            if (frame->kind != Sync::FrameKind::PaneBind)
                continue;
            if (const auto bind = Sync::decodePaneBind(frame->body))
                m_bindings.push_back(bind->paneID);
        }
        return out;
    }

    /// Admits a session to the workspace the window publishes.
    ///
    /// - Parameter sessionID: The session.
    void admit(std::uint32_t sessionID)
    {
        if (std::ranges::find(m_sessions, sessionID) == m_sessions.end())
            m_sessions.push_back(sessionID);
    }

    /// Offers a workspace's structure, as the host does before it binds the
    /// panes the structure first names.
    ///
    /// - Parameter workspace: The workspace, its revision set.
    void lay(const Ubiquitty::Session::TreeWorkspace& workspace)
    {
        m_workspaceID = workspace.id;
        CHECK(UbiquittyTest::offerStructure(m_connection, workspace));
    }

    /// Binds a session's pane to this attachment, publishing it when its
    /// program printed or it holds no frame yet.
    ///
    /// The daemon publishes a pane when its program writes, never because
    /// another attachment bound it: a new generation holds the pane's
    /// effects on every attachment until that attachment's client has
    /// acknowledged the new picture.
    ///
    /// - Parameters:
    ///   - sessionID: The session.
    ///   - text: What its program has printed, written before the publish.
    void bind(std::uint32_t sessionID, const std::string& text = {})
    {
        Pane& pane = paneFor(sessionID);
        if (!text.empty())
            Ubiquitty::processBytes(&pane.terminal, text);
        std::uint32_t& handle = m_handles[sessionID];
        if (handle == 0) {
            handle = m_connection.bindPane(*pane.source);
            if (handle != 0)
                UbiquittyTest::showPane(m_connection, handle, advance());
        }
        if (!text.empty() || !pane.source->isFrameHeld())
            pane.source->publish(Sync::FrameClass::Settled, m_daemon->now);
    }

    /// Unbinds a session's pane, as the daemon does once it hands the
    /// session back.
    ///
    /// - Parameter sessionID: The session.
    void unbind(std::uint32_t sessionID)
    {
        std::uint32_t& handle = m_handles[sessionID];
        if (handle != 0)
            m_connection.unbindPane(handle, 0);
        handle = 0;
        std::erase(m_sessions, sessionID);
    }

    /// Answers the window's publication.
    ///
    /// - Parameter workspaceID: The workspace the window was published as,
    ///   or zero to refuse it.
    /// - Returns: Whether a publication was pending and the answer went out.
    [[nodiscard]] bool answerShare(std::uint32_t workspaceID)
    {
        for (auto call = m_calls.begin(); call != m_calls.end(); ++call) {
            const auto request = Sync::decodeCallRequest(call->text);
            if (request.outcome != Ubiquitty::Protocol::Control::RequestOutcome::Dispatch || request.method != "workspace.share")
                continue;
            const std::string body = workspaceID ? std::string(R"("result":{"workspace":)") + std::to_string(workspaceID) + "}"
                                                 : std::string(R"("error":{"code":-32000,"message":"The fixture refused the shared window."})");
            const std::string response = R"({"jsonrpc":"2.0","id":")" + request.id.asString() + R"(",)" + body + "}";
            const bool isSent = m_connection.answerCall(call->callIndex, response) == Sync::CallAnswer::Sent;
            m_calls.erase(call);
            return isSent;
        }
        return false;
    }

    /// Hands back a session whose reclaim the client asked for, answering
    /// exactly the call that named its pane.
    ///
    /// - Parameter sessionID: The session.
    /// - Returns: Whether a reclaim of it was pending and the answer went out.
    [[nodiscard]] bool releaseReclaim(std::uint32_t sessionID)
    {
        const std::uint32_t handle = m_handles[sessionID];
        for (auto call = m_calls.begin(); call != m_calls.end(); ++call) {
            const auto request = Sync::decodeCallRequest(call->text);
            if (request.outcome != Ubiquitty::Protocol::Control::RequestOutcome::Dispatch || request.method != "pane.reclaim")
                continue;
            const auto reclaim = Ubiquitty::Protocol::Control::readReclaimRequest(request.params);
            if (!reclaim || reclaim->handle != handle)
                continue;
            const Ubiquitty::Protocol::Control::ReclaimTarget target { .handle = handle,
                .sessionID = sessionID,
                .daemonInstanceID = m_daemonInstanceID,
                .hostInstanceID = { m_daemonInstanceID, 0 },
                .attachmentID = { m_attachmentNumber, m_daemonInstanceID },
                .paneID = { sessionID, m_daemonInstanceID } };
            const bool isSent = m_connection.answerCall(call->callIndex,
                                    Ubiquitty::Codec::JSON::serialize(Ubiquitty::Protocol::Control::reclaimReleasedResult(request.idOrNull(), target)))
                == Sync::CallAnswer::Sent;
            m_calls.erase(call);
            return isSent;
        }
        return false;
    }

    /// Counts the calls of one method the client sent and nothing answered.
    ///
    /// - Parameter method: The method.
    /// - Returns: The count.
    [[nodiscard]] std::size_t pendingCalls(std::string_view method) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(m_calls, [&](const Sync::ReceivedCall& call) {
            const auto request = Sync::decodeCallRequest(call.text);
            return request.outcome == Ubiquitty::Protocol::Control::RequestOutcome::Dispatch && request.method == method;
        }));
    }

    /// Asks the person at a session's pane whether an agent may attach.
    ///
    /// - Parameters:
    ///   - sessionID: The session.
    ///   - requestID: The question's identity.
    void askConsent(std::uint32_t sessionID, std::uint32_t requestID)
    {
        Sync::ConsentQuestion question;
        question.requestID = requestID;
        question.sessionID = sessionID;
        const auto notify = Sync::consentQuestionNotify(question);
        REQUIRE(notify);
        CHECK(m_connection.sendNotify(*notify));
    }

    /// Rings a bound session's bell, the effect its program raises.
    ///
    /// - Parameter sessionID: The session.
    void ringBell(std::uint32_t sessionID)
    {
        CHECK(paneFor(sessionID).source->appendEffect(Sync::BellEffect {}, m_connection.clientInstance()).has_value());
    }

    /// What a session's program was written, in order, through every
    /// attachment to the daemon.
    [[nodiscard]] std::string typed(std::uint32_t sessionID) { return paneFor(sessionID).typed.bytes(); }
    /// How many greetings, one per carrier, reached this host.
    [[nodiscard]] unsigned greets() const { return m_greets; }
    /// How many admissions this host sent.
    [[nodiscard]] unsigned welcomes() const { return m_welcomes; }
    /// How many reclaims arrived while another awaited release, and were refused.
    [[nodiscard]] unsigned refusedReclaims() const { return m_refusedReclaims; }
    /// The pane each binding this host sent was for, in order.
    [[nodiscard]] const std::vector<Sync::ID128>& bindings() const { return m_bindings; }

private:
    /// One session's pane as the daemon holds it, whichever attachments
    /// bind it.
    struct Pane {
        std::uint32_t sessionID = 0;
        Ubiquitty::Terminal terminal;
        UbiquittyTest::TypedInput typed;
        std::unique_ptr<Sync::PaneSource> source;
    };

    /// What every attachment to one daemon shares: its panes, its clock, and
    /// the numbering of its attachments.
    struct SharedDaemon {
        std::vector<std::unique_ptr<Pane>> panes;
        std::uint64_t now = 1'000'000;
        std::uint64_t attachments = 0;
    };

    /// Creates the host end of one attachment to a daemon.
    ///
    /// - Parameters:
    ///   - anchorSessionID: The session this attachment's Greet names.
    ///   - workspaceID: The workspace its Welcome admits it to.
    ///   - daemonInstanceID: The daemon process the Welcome names.
    ///   - daemon: What the daemon's attachments share.
    SharedWindowHost(std::uint32_t anchorSessionID, std::uint32_t workspaceID, std::uint64_t daemonInstanceID, std::shared_ptr<SharedDaemon> daemon)
        : m_daemon(std::move(daemon))
        , m_attachmentNumber(++m_daemon->attachments)
        , m_workspaceID(workspaceID)
        , m_daemonInstanceID(daemonInstanceID)
        , m_connection(Sync::measuredHostPolicy(0), admitted(anchorSessionID, workspaceID, daemonInstanceID, m_attachmentNumber))
    {
        m_sessions.push_back(anchorSessionID);
        // Owned by the connection this object owns, so it never outlives it.
        m_connection.setGreetingAdmission([this](const Sync::Greet& greeting) -> std::expected<Sync::HostIdentity, Sync::Bye> {
            const Sync::AdmissionRequest& request = greeting.request;
            if (request.target != Sync::AdmissionTarget::Workspace || std::ranges::find(m_sessions, request.sessionID) == m_sessions.end()
                || (request.workspaceID && request.workspaceID != m_workspaceID) || request.expectedDaemonInstanceID != m_daemonInstanceID)
                return std::unexpected(Sync::Bye { .reason = Sync::ByeReason::SessionGone, .detail = "the fixture holds only the window's adopted sessions" });
            return admitted(request.sessionID, m_workspaceID, m_daemonInstanceID, m_attachmentNumber);
        });
    }

    /// The daemon's pane for a session, made on first use by any of its
    /// attachments.
    Pane& paneFor(std::uint32_t sessionID)
    {
        for (const std::unique_ptr<Pane>& pane : m_daemon->panes) {
            if (pane->sessionID == sessionID)
                return *pane;
        }
        auto pane = std::make_unique<Pane>();
        pane->sessionID = sessionID;
        Ubiquitty::hardReset(pane->terminal);
        Ubiquitty::resize(pane->terminal, 24, 80);
        pane->source = std::make_unique<Sync::PaneSource>(pane->terminal, Sync::ID128 { .low = sessionID, .high = m_daemonInstanceID });
        pane->source->setInputWriter(pane->typed);
        m_daemon->panes.push_back(std::move(pane));
        return *m_daemon->panes.back();
    }

    /// What the host admits a person attached to one of the window's
    /// sessions as, on one attachment of its own.
    static Sync::HostIdentity admitted(std::uint32_t sessionID, std::uint32_t workspaceID, std::uint64_t daemonInstanceID, std::uint64_t attachmentNumber)
    {
        Sync::HostIdentity identity;
        identity.hostInstanceID = { .low = daemonInstanceID, .high = 0 };
        identity.attachmentID = { .low = attachmentNumber, .high = daemonInstanceID };
        identity.resumeSecret = { .low = 2 + attachmentNumber, .high = daemonInstanceID };
        identity.mayDrive = true;
        identity.maxRole = Sync::Role::Owner;
        identity.verifiedKind = Sync::ClientKind::Human;
        identity.selection = { .target = Sync::AdmissionTarget::Workspace,
            .sessionID = sessionID,
            .workspaceID = workspaceID,
            .daemonInstanceID = daemonInstanceID,
            .socketName = "ubq-share-test.sock",
            .buildFlags = 0,
            .hostPlatform = "fixture" };
        return identity;
    }

    std::uint64_t advance() { return m_daemon->now += 20'000; }

    // Held ahead of the connection, so the connection that binds the
    // daemon's panes goes first.
    std::shared_ptr<SharedDaemon> m_daemon;
    std::uint64_t m_attachmentNumber = 0;
    std::uint32_t m_workspaceID = 0;
    std::uint64_t m_daemonInstanceID = 0;
    Sync::HostConnection m_connection;
    std::vector<std::uint32_t> m_sessions;
    /// The handle this attachment bound each session's pane under.
    std::map<std::uint32_t, std::uint32_t> m_handles;
    std::vector<Sync::ReceivedCall> m_calls;
    std::vector<Sync::ID128> m_bindings;
    Sync::FrameDecoder m_clientFrames;
    Sync::FrameDecoder m_hostFrames;
    std::size_t m_consumed = 0;
    unsigned m_greets = 0;
    unsigned m_welcomes = 0;
    unsigned m_refusedReclaims = 0;
};

/// The workspace a window of four panes is published as: in its first
/// native window, a tab of two panes side by side and a second tab; in its
/// second native window, one tab.
Ubiquitty::Session::TreeWorkspace publishedWindow(std::uint32_t workspaceID, const std::array<std::uint32_t, 4>& sessions, std::uint64_t revision)
{
    namespace Tree = Ubiquitty::Session;
    const UbiquittyTest::StructurePane split[] = { { .pane = 1, .sessionID = sessions[0], .columns = 39, .rows = 24 },
        { .pane = 2, .sessionID = sessions[1], .columns = 40, .rows = 24 } };
    const UbiquittyTest::StructurePane second { .pane = 3, .sessionID = sessions[2] };
    const UbiquittyTest::StructurePane other { .pane = 4, .sessionID = sessions[3] };
    Tree::TreeWorkspace workspace = UbiquittyTest::rowWorkspace(workspaceID, "Shared Window", split, 17, revision);
    Tree::TreeWorkspace secondTab = UbiquittyTest::rowWorkspace(workspaceID, {}, { &second, 1 }, 17, revision);
    Tree::TreeTab tab = std::move(secondTab.windows[0].tabs[0]);
    tab.id = Tree::Layout::TabID { 2 };
    tab.isHostActive = false;
    workspace.windows[0].tabs.push_back(std::move(tab));
    Tree::TreeWorkspace otherWindow = UbiquittyTest::rowWorkspace(workspaceID, {}, { &other, 1 }, 17, revision);
    Tree::TreeWindow window = std::move(otherWindow.windows[0]);
    window.id = Tree::Layout::SurfaceID { 2 };
    window.isHostActive = false;
    window.tabs[0].id = Tree::Layout::TabID { 3 };
    workspace.windows.push_back(std::move(window));
    return workspace;
}

/// The last greeting a carrier carried.
std::optional<Sync::Greet> lastGreet(const StubChannel& channel)
{
    Sync::FrameDecoder decoder;
    std::optional<Sync::Greet> greet;
    for (const std::vector<std::uint8_t>& bytes : channel.sent) {
        decoder.append(bytes);
        while (const auto frame = decoder.next()) {
            if (frame->kind == Sync::FrameKind::Greet)
                greet = Sync::decodeGreet(frame->body);
        }
    }
    return greet;
}

/// How a publication is answered.
enum class Publication : std::uint8_t {
    Confirmed,
    Refused,
};

/// A window of four running shells, each committed to the daemon on its own:
/// the first pane anchors the window's connection, the other three join it.
struct SharedWindow {
    static constexpr std::array<std::uint32_t, 4> sessions { 42, 43, 44, 45 };
    static constexpr std::uint32_t ownWorkspaceID = 5;
    static constexpr std::uint32_t publishedWorkspaceID = 9;

    StubFactory factory;
    std::array<TerminalSession*, 4> panes {};
    std::unique_ptr<SharedWindowHost> host;
    const std::string bridge = "fixture-local-bridge";
    bool isReady = false;

    SharedWindow()
    {
        discardPostedWork();
        handoffRecord() = {};
        for (std::size_t index = 0; index < panes.size(); ++index) {
            panes[index] = makeReadingSession();
            if (panes[index] == nullptr)
                return;
            handoffRecord().answer = sessions[index];
            if (panes[index]->handOverToLocalDaemon("/tmp/ubq-share-test.sock", handoffAddress()) != sessions[index])
                return;
            panes[index]->setLocalAdoptionBridgeCommand(bridge);
        }
        isReady = true;
    }

    ~SharedWindow()
    {
        for (TerminalSession*& pane : panes) {
            if (pane == nullptr)
                continue;
            pane->close();
            releaseTerminalSession(pane);
            pane = nullptr;
        }
        discardPostedWork();
    }

    SharedWindow(const SharedWindow&) = delete;
    SharedWindow& operator=(const SharedWindow&) = delete;

    TerminalSession& anchor() { return *panes[0]; }

    /// Carries what the client wrote to the host, and what the host sends back.
    void exchange()
    {
        if (factory.lastChannel == nullptr || !host)
            return;
        host->receive(*factory.lastChannel);
        factory.lastChannel->receive(host->flush());
        pumpPostedWork();
    }

    /// Attaches the anchor in a workspace of its own, the other panes waiting
    /// to join it.
    void attach()
    {
        REQUIRE(anchor().attachSharedLocalWindow(factory.address(), queueingLoop()));
        for (std::size_t index = 1; index < panes.size(); ++index) {
            CHECK_MSG(panes[index]->joinSharedLocalWindow(anchor(), queueingLoop()) == TerminalSession::SharedWindowJoin::Awaiting,
                "a committed pane did not wait to join its window's connection");
        }
        REQUIRE(factory.lastChannel != nullptr);
        factory.lastChannel->connect();
        pumpPostedWork();
        host = std::make_unique<SharedWindowHost>(sessions[0], ownWorkspaceID);
        exchange();
        const UbiquittyTest::StructurePane only { .pane = 1, .sessionID = sessions[0] };
        host->lay(UbiquittyTest::rowWorkspace(ownWorkspaceID, {}, { &only, 1 }, 17));
        exchange();
        host->bind(sessions[0], "pane-42");
        exchange();
    }

    /// The window's arrangement, naming every session it holds.
    static Ubiquitty::Session::SharedWorkspace arrangement()
    {
        Ubiquitty::Session::SharedWorkspace shared;
        Ubiquitty::Session::WorkspaceWindow window;
        window.tabs.push_back({ "shell", Ubiquitty::Session::PaneNode::makeLeaf({}), 0 });
        shared.arrangement.windows.push_back(std::move(window));
        shared.sessions.assign(sessions.begin(), sessions.end());
        return shared;
    }

    /// Publishes the window through its anchor. A confirmed publication lays
    /// every pane out in the published workspace in the same update as the
    /// answer, beside whatever `alongside` adds to it.
    void publish(Publication answer = Publication::Confirmed, const std::function<void(SharedWindowHost&)>& alongside = {})
    {
        REQUIRE(anchor().publishSharedWorkspace(arrangement()));
        exchange();
        REQUIRE(host->answerShare(answer == Publication::Confirmed ? publishedWorkspaceID : 0));
        if (answer == Publication::Confirmed) {
            for (const std::uint32_t session : sessions)
                host->admit(session);
            host->lay(publishedWindow(publishedWorkspaceID, sessions, 2));
        }
        if (alongside)
            alongside(*host);
        factory.lastChannel->receive(host->flush());
        pumpPostedWork();
    }

    /// Binds every joined pane, each showing a picture of its own.
    void bindMembers()
    {
        for (std::size_t index = 1; index < panes.size(); ++index)
            host->bind(sessions[index], "pane-" + std::to_string(sessions[index]));
        exchange();
        exchange();
    }
};

} // namespace

TEST(aSharedWindowRidesOneCarrierForEveryTabSplitAndWindow)
{
    SharedWindow window;
    REQUIRE(window.isReady);
    // Committed, a pane has no client and nothing to send input to, so no
    // keystroke can reach its program twice across the move to the window's
    // connection.
    for (TerminalSession* pane : window.panes) {
        CHECK(pane->isSharedIntoLocalDaemon());
        CHECK(!pane->isRemoteAttached());
        CHECK(!pane->hasHostInputSink());
        CHECK(pane->fileDescriptor() == -1);
    }
    CHECK_MSG(postedWork().empty(), "committing a handoff started a connection");
    constexpr std::string_view early = "before-the-join\n";
    window.panes[1]->sendInput({ early.data(), early.size() });

    window.attach();
    CHECK_EQ(window.factory.createCount, 1);
    REQUIRE(window.anchor().remoteState() == Ubiquitty::Session::State::Connected);
    for (std::size_t index = 1; index < window.panes.size(); ++index)
        CHECK_MSG(!window.panes[index]->isRemoteAttached(), "a pane joined before the host placed its session in the anchor's workspace");

    // A question the host asks about a joining pane in the update that
    // places it is that pane's, and nobody answers it as unshown. The host
    // queues the question ahead of the structure, so the carrier reads it
    // first and routes it while the panes are joining.
    window.publish(Publication::Confirmed, [](SharedWindowHost& host) { host.askConsent(SharedWindow::sessions[1], 123); });
    for (TerminalSession* pane : window.panes) {
        CHECK(pane->isSharedIntoLocalDaemon());
        CHECK_MSG(pane->isRemoteAttached(), "a pane of the published window has no client");
        CHECK_MSG(pane->sharesRemoteWorkspaceConnection(window.anchor()), "a pane of the window rides a connection of its own");
        CHECK(pane->remoteState() == Ubiquitty::Session::State::Connected);
    }
    CHECK_EQ(window.panes[1]->pendingConsentRequestID(), 123u);
    CHECK_EQ(window.panes[0]->pendingConsentRequestID(), 0u);
    CHECK_EQ(window.panes[2]->pendingConsentRequestID(), 0u);
    CHECK_EQ(window.panes[3]->pendingConsentRequestID(), 0u);
    CHECK(!window.anchor().sharedWorkspaceResult().isPending);
    CHECK_EQ(window.anchor().sharedWorkspaceResult().workspaceID, SharedWindow::publishedWorkspaceID);

    window.bindMembers();
    CHECK_MSG(window.host->pendingCalls("consent.respond") == 0u, "a question the window's pane was shown was answered for it");
    // One carrier, one greeting, one admission, and one binding per pane.
    CHECK_EQ(window.factory.createCount, 1);
    CHECK_EQ(window.host->greets(), 1u);
    CHECK_EQ(window.host->welcomes(), 1u);
    CHECK_EQ(window.host->bindings().size(), window.sessions.size());
    for (const std::uint32_t session : SharedWindow::sessions) {
        const Sync::ID128 pane { .low = session, .high = 17 };
        CHECK_MSG(std::ranges::count(window.host->bindings(), pane) == 1, "a pane of the window was bound other than once");
    }
    for (std::size_t index = 0; index < window.panes.size(); ++index)
        CHECK_MSG(screenContains(*window.panes[index], "pane-" + std::to_string(SharedWindow::sessions[index])), "a pane does not show its own session");

    // Input, once each, and only for the pane it was typed in.
    for (std::size_t index = 0; index < window.panes.size(); ++index) {
        const std::string line = "typed-" + std::to_string(SharedWindow::sessions[index]) + "\n";
        window.panes[index]->sendInput({ line.data(), line.size() });
    }
    window.exchange();
    for (const std::uint32_t session : SharedWindow::sessions)
        CHECK_STR_EQ(window.host->typed(session), "typed-" + std::to_string(session) + "\n");

    // An effect reaches the one pane whose program raised it.
    window.host->ringBell(SharedWindow::sessions[2]);
    window.exchange();
    CHECK_EQ(window.panes[2]->takeBellsRung(), 1u);
    CHECK_EQ(window.panes[0]->takeBellsRung(), 0u);
    CHECK_EQ(window.panes[1]->takeBellsRung(), 0u);
    CHECK_EQ(window.panes[3]->takeBellsRung(), 0u);

    // The connection is this workspace's in this process: the same authority
    // finds it already attached, and another authority is not it.
    Ubiquitty::Session::WorkspaceAttachRequest request;
    request.authority = { .endpointIdentity = window.bridge, .principal = {}, .daemonSocket = "ubq-share-test.sock", .daemonInstanceID = 17 };
    request.workspaceID = SharedWindow::publishedWorkspaceID;
    auto otherDaemon = request;
    otherDaemon.authority.daemonInstanceID = 23;
    auto otherBridge = request;
    otherBridge.authority.endpointIdentity = "another-local-bridge";
    for (TerminalSession* pane : window.panes) {
        CHECK(pane->matchesRemoteWorkspace(request));
        CHECK(!pane->matchesRemoteWorkspace(otherDaemon));
        CHECK(!pane->matchesRemoteWorkspace(otherBridge));
    }
    TerminalSession* projection = TerminalSession::make();
    REQUIRE(projection != nullptr);
    projection->start(24, 80);
    CHECK(projection->attachRemote(window.bridge, "", "", request, "", false, queueingLoop())
        == TerminalSession::RemoteWorkspaceAttachOutcome::AlreadyAttached);
    CHECK(!projection->isRemoteAttached());
    releaseTerminalSession(projection);
    CHECK_EQ(window.factory.createCount, 1);

    // One reconnect resumes every pane, and greets the published workspace.
    window.factory.lastChannel->loseTransport();
    pumpPostedWork();
    REQUIRE(window.factory.lastChannel != nullptr);
    window.factory.lastChannel->connect();
    pumpPostedWork();
    CHECK_EQ(window.factory.createCount, 2);
    const auto greet = lastGreet(*window.factory.lastChannel);
    REQUIRE(greet.has_value());
    CHECK_EQ(greet->request.workspaceID, SharedWindow::publishedWorkspaceID);
    CHECK_EQ(greet->request.sessionID, SharedWindow::sessions[0]);
    CHECK_MSG(greet->resume.has_value() && greet->resume->panes.size() == SharedWindow::sessions.size(),
        "the reconnect does not resume every pane of the window on its one attachment");
    if (greet->resume) {
        const Sync::ID128 rung { .low = SharedWindow::sessions[2], .high = 17 };
        const auto resumed = std::ranges::find(greet->resume->panes, rung, &Sync::PaneResume::paneID);
        CHECK_MSG(resumed != greet->resume->panes.end() && resumed->effectsAcknowledged != 0, "the bell's receipt did not carry across the reconnect");
    }
    for (TerminalSession* pane : window.panes)
        CHECK(pane->remoteState() != Ubiquitty::Session::State::Failed);
}

TEST(closingTheFirstPaneLeavesTheWindowOnItsSharedCarrier)
{
    SharedWindow window;
    REQUIRE(window.isReady);
    window.attach();
    window.publish();
    window.bindMembers();
    for (TerminalSession* pane : window.panes)
        REQUIRE(pane->sharesRemoteWorkspaceConnection(window.anchor()));

    // The anchor goes; what it attached is the window's, and stays.
    TerminalSession* first = std::exchange(window.panes[0], nullptr);
    first->close();
    releaseTerminalSession(first);
    pumpPostedWork();
    for (std::size_t index = 1; index < window.panes.size(); ++index) {
        TerminalSession& pane = *window.panes[index];
        CHECK(pane.isSharedIntoLocalDaemon());
        CHECK(pane.isRemoteAttached());
        CHECK(pane.remoteState() == Ubiquitty::Session::State::Connected);
        CHECK(pane.sharesRemoteWorkspaceConnection(*window.panes[1]));
    }
    constexpr std::string_view line = "after-the-first-closed\n";
    window.panes[2]->sendInput({ line.data(), line.size() });
    window.exchange();
    CHECK_STR_EQ(window.host->typed(SharedWindow::sessions[2]), line);
    CHECK_EQ(window.factory.createCount, 1);

    // Its factory and the loop it posts to outlive it: the connection still
    // dials again, once, for the panes that remain.
    window.factory.lastChannel->loseTransport();
    pumpPostedWork();
    REQUIRE(window.factory.lastChannel != nullptr);
    window.factory.lastChannel->connect();
    pumpPostedWork();
    CHECK_EQ(window.factory.createCount, 2);
    const auto greet = lastGreet(*window.factory.lastChannel);
    REQUIRE(greet.has_value());
    CHECK_EQ(greet->request.workspaceID, SharedWindow::publishedWorkspaceID);
    for (std::size_t index = 1; index < window.panes.size(); ++index)
        CHECK(window.panes[index]->remoteState() != Ubiquitty::Session::State::Failed);
}

TEST(eachPaneOfASharedWindowIsReclaimedAloneAndTheRestStayOnItsCarrier)
{
    SharedWindow window;
    REQUIRE(window.isReady);
    window.attach();
    window.publish();
    window.bindMembers();

    // Both panes ask on the one carrier. The host holds one reclaim per
    // attachment, so the second ask waits for the first's answer, and each
    // answer hands back only the pane it names.
    REQUIRE(window.panes[1]->requestReclaim());
    REQUIRE(window.panes[2]->requestReclaim());
    window.exchange();
    CHECK_EQ(window.host->pendingCalls("pane.reclaim"), 1u);
    REQUIRE(window.host->releaseReclaim(SharedWindow::sessions[1]));
    window.factory.lastChannel->receive(window.host->flush());
    pumpPostedWork();
    CHECK(!window.panes[1]->isSharedIntoLocalDaemon());
    CHECK(!window.panes[1]->isRemoteAttached());
    CHECK_MSG(window.panes[2]->isSharedIntoLocalDaemon() && window.panes[2]->isRemoteAttached(), "a pane was handed back by another pane's answer");
    CHECK(window.panes[0]->remoteState() == Ubiquitty::Session::State::Connected);
    CHECK(window.panes[3]->remoteState() == Ubiquitty::Session::State::Connected);
    typeIntoMaster(*window.panes[1], "after-reclaim");
    CHECK_MSG(waitFor([&] { return window.panes[1]->takeDrainedBytes() > 0; }, 5000), "the reclaimed pane's reader did not resume");
    window.host->unbind(SharedWindow::sessions[1]);
    window.host->receive(*window.factory.lastChannel);
    CHECK_EQ(window.host->pendingCalls("pane.reclaim"), 1u);
    REQUIRE(window.host->releaseReclaim(SharedWindow::sessions[2]));
    window.host->unbind(SharedWindow::sessions[2]);
    window.factory.lastChannel->receive(window.host->flush());
    pumpPostedWork();
    CHECK(!window.panes[2]->isSharedIntoLocalDaemon());

    // The anchor is reclaimed too; the pane left keeps the carrier, and its
    // next greeting names a session the workspace still holds.
    REQUIRE(window.anchor().requestReclaim());
    window.exchange();
    REQUIRE(window.host->releaseReclaim(SharedWindow::sessions[0]));
    window.host->unbind(SharedWindow::sessions[0]);
    window.factory.lastChannel->receive(window.host->flush());
    pumpPostedWork();
    CHECK(!window.anchor().isSharedIntoLocalDaemon());
    CHECK(!window.anchor().isRemoteAttached());
    const UbiquittyTest::StructurePane last { .pane = 4, .sessionID = SharedWindow::sessions[3] };
    window.host->lay(UbiquittyTest::rowWorkspace(SharedWindow::publishedWorkspaceID, "Shared Window", { &last, 1 }, 17, 3));
    window.exchange();
    TerminalSession& remaining = *window.panes[3];
    CHECK(remaining.isSharedIntoLocalDaemon());
    CHECK(remaining.isRemoteAttached());
    CHECK(remaining.remoteState() == Ubiquitty::Session::State::Connected);
    CHECK_EQ(window.factory.createCount, 1);
    CHECK_MSG(window.host->refusedReclaims() == 0u, "a pane's reclaim reached the host while another awaited release");
    window.factory.lastChannel->loseTransport();
    pumpPostedWork();
    REQUIRE(window.factory.lastChannel != nullptr);
    window.factory.lastChannel->connect();
    pumpPostedWork();
    const auto greet = lastGreet(*window.factory.lastChannel);
    REQUIRE(greet.has_value());
    CHECK_EQ(greet->request.sessionID, SharedWindow::sessions[3]);
    CHECK_EQ(greet->request.workspaceID, SharedWindow::publishedWorkspaceID);
    CHECK(remaining.remoteState() != Ubiquitty::Session::State::Failed);
}

TEST(aCommittedPaneOfAnotherAuthorityKeepsACarrierOfItsOwn)
{
    StubFactory own;
    SharedWindow window;
    REQUIRE(window.isReady);
    REQUIRE(window.anchor().attachSharedLocalWindow(window.factory.address(), queueingLoop()));

    // A shell another daemon process adopted.
    TerminalSession* elsewhere = makeReadingSession();
    REQUIRE(elsewhere != nullptr);
    handoffRecord().answer = 77;
    handoffRecord().daemonInstanceID = 23;
    const std::uint32_t elsewhereSessionID = elsewhere->handOverToLocalDaemon("/tmp/ubq-share-test.sock", handoffAddress());
    handoffRecord().daemonInstanceID = 17;
    CHECK_EQ(elsewhereSessionID, 77u);
    elsewhere->setLocalAdoptionBridgeCommand(window.bridge);
    CHECK(elsewhere->joinSharedLocalWindow(window.anchor(), queueingLoop()) == TerminalSession::SharedWindowJoin::Refused);

    // The same daemon reached through another bridge.
    window.panes[1]->setLocalAdoptionBridgeCommand("another-local-bridge");
    CHECK(window.panes[1]->joinSharedLocalWindow(window.anchor(), queueingLoop()) == TerminalSession::SharedWindowJoin::Refused);
    // A loop the anchor's connection does not post to.
    CHECK(window.panes[2]->joinSharedLocalWindow(window.anchor(), otherQueueingLoop()) == TerminalSession::SharedWindowJoin::Refused);
    // A pane that anchors no window.
    CHECK(window.panes[3]->joinSharedLocalWindow(*window.panes[2], queueingLoop()) == TerminalSession::SharedWindowJoin::Refused);
    CHECK(window.panes[3]->joinSharedLocalWindow(window.anchor(), queueingLoop()) == TerminalSession::SharedWindowJoin::Awaiting);

    // A refused pane is still shared, and attaches on a carrier of its own.
    CHECK(elsewhere->isSharedIntoLocalDaemon());
    CHECK(!elsewhere->isRemoteAttached());
    CHECK(elsewhere->attachSharedLocalSession(own.address(), queueingLoop()));
    CHECK_EQ(own.createCount, 1);
    CHECK_EQ(window.factory.createCount, 1);
    CHECK(!elsewhere->sharesRemoteWorkspaceConnection(window.anchor()));
    elsewhere->close();
    releaseTerminalSession(elsewhere);
}

TEST(aRefusedPublicationLeavesEveryCommittedPaneSharedAndAttachable)
{
    StubFactory own;
    SharedWindow window;
    REQUIRE(window.isReady);
    window.attach();
    window.publish(Publication::Refused);
    const auto& result = window.anchor().sharedWorkspaceResult();
    CHECK(!result.isPending);
    CHECK_EQ(result.workspaceID, 0u);
    CHECK(!result.detail.empty());

    // No grouping failure hands a committed PTY back: each pane stays the
    // daemon's, unread here, and waits for a client.
    for (std::size_t index = 1; index < window.panes.size(); ++index) {
        TerminalSession& pane = *window.panes[index];
        CHECK(pane.isSharedIntoLocalDaemon());
        CHECK(!pane.isRemoteAttached());
        CHECK(pane.fileDescriptor() == -1);
        CHECK(!pane.hasHostInputSink());
    }
    CHECK(window.anchor().remoteState() == Ubiquitty::Session::State::Connected);

    // The frontend gives each a connection of its own, as it does on any
    // failure, and the window's connection lets it go.
    CHECK(window.panes[1]->attachSharedLocalSession(own.address(), queueingLoop()));
    CHECK_EQ(own.createCount, 1);
    window.exchange();
    CHECK(window.panes[1]->isRemoteAttached());
    CHECK(!window.panes[1]->sharesRemoteWorkspaceConnection(window.anchor()));
    CHECK(window.panes[1]->joinSharedLocalWindow(window.anchor(), queueingLoop()) == TerminalSession::SharedWindowJoin::Refused);
    CHECK_EQ(window.factory.createCount, 1);
}

// A pane attached on a connection of its own before its window's workspace was
// confirmed, as a share retried after some panes attached, a cancelled quit
// or a handoff resolved later leaves it, moves onto the window's connection
// once both carry that workspace. It waits while a question it was asked on
// its own connection is open, and answers it there; its program reads what was
// typed before the move and after it once each, in order; its effects go on
// reaching it through the connection it rides until the move, and one that
// connection delivered is not shown again when the window's connection
// delivers it after; its picture stays; and the attachment it left detaches.
// Both attachments reach one daemon, which holds one pane source per session
// and so one effect log, numbered per pane, that every attachment reads from
// the point it bound the pane. Each attachment is a client instance of its
// own, and an effect is posted on the attachment the daemon elected for it:
// the one that typed into the session last.
TEST(aPaneOnACarrierOfItsOwnMovesOntoItsWindowsCarrierOnceSettled)
{
    StubFactory own;
    SharedWindow window;
    REQUIRE(window.isReady);
    TerminalSession& anchor = window.anchor();
    TerminalSession& moving = *window.panes[1];
    const std::uint32_t anchorSession = SharedWindow::sessions[0];
    const std::uint32_t movingSession = SharedWindow::sessions[1];

    // Each attached alone, in a workspace of its own.
    REQUIRE(anchor.attachSharedLocalSession(window.factory.address(), queueingLoop()));
    REQUIRE(window.factory.lastChannel != nullptr);
    window.factory.lastChannel->connect();
    pumpPostedWork();
    window.host = std::make_unique<SharedWindowHost>(anchorSession, SharedWindow::ownWorkspaceID);
    window.exchange();
    const UbiquittyTest::StructurePane anchorOnly { .pane = 1, .sessionID = anchorSession };
    window.host->lay(UbiquittyTest::rowWorkspace(SharedWindow::ownWorkspaceID, {}, { &anchorOnly, 1 }, 17));
    window.exchange();
    window.host->bind(anchorSession, "pane-42");
    window.exchange();

    REQUIRE(moving.attachSharedLocalSession(own.address(), queueingLoop()));
    REQUIRE(own.lastChannel != nullptr);
    own.lastChannel->connect();
    pumpPostedWork();
    constexpr std::uint32_t movingOwnWorkspaceID = 6;
    SharedWindowHost alone(movingSession, movingOwnWorkspaceID, *window.host);
    const auto exchangeAlone = [&] {
        if (own.lastChannel == nullptr)
            return;
        alone.receive(*own.lastChannel);
        own.lastChannel->receive(alone.flush());
        pumpPostedWork();
    };
    exchangeAlone();
    const UbiquittyTest::StructurePane movingOnly { .pane = 1, .sessionID = movingSession };
    alone.lay(UbiquittyTest::rowWorkspace(movingOwnWorkspaceID, {}, { &movingOnly, 1 }, 17));
    exchangeAlone();
    alone.bind(movingSession, "pane-43");
    exchangeAlone();
    exchangeAlone();
    REQUIRE(moving.remoteState() == Ubiquitty::Session::State::Connected);
    REQUIRE(!moving.sharesRemoteWorkspaceConnection(anchor));

    constexpr std::string_view early = "before-the-move\n";
    moving.sendInput({ early.data(), early.size() });
    exchangeAlone();
    CHECK_STR_EQ(alone.typed(movingSession), "before-the-move\n");
    alone.ringBell(movingSession);
    exchangeAlone();
    CHECK_EQ(moving.takeBellsRung(), 1u);

    // The window is published through the anchor's connection, and the
    // host places both sessions in its workspace; each connection follows.
    auto shared = SharedWindow::arrangement();
    shared.sessions = { anchorSession, movingSession };
    REQUIRE(anchor.publishSharedWorkspace(shared));
    window.exchange();
    REQUIRE(window.host->answerShare(SharedWindow::publishedWorkspaceID));
    const UbiquittyTest::StructurePane both[] = { { .pane = 1, .sessionID = anchorSession, .columns = 39, .rows = 24 },
        { .pane = 2, .sessionID = movingSession, .columns = 40, .rows = 24 } };
    window.host->admit(movingSession);
    window.host->lay(UbiquittyTest::rowWorkspace(SharedWindow::publishedWorkspaceID, "Shared Window", both, 17, 2));
    window.factory.lastChannel->receive(window.host->flush());
    pumpPostedWork();
    REQUIRE(anchor.sharedWorkspaceResult().workspaceID == SharedWindow::publishedWorkspaceID);
    alone.admit(anchorSession);
    alone.lay(UbiquittyTest::rowWorkspace(SharedWindow::publishedWorkspaceID, "Shared Window", both, 17, 2));
    exchangeAlone();

    // A question its own connection asked is open when the window gathers.
    alone.askConsent(movingSession, 77);
    exchangeAlone();
    REQUIRE(moving.pendingConsentRequestID() == 77u);

    REQUIRE(anchor.anchorSharedLocalWindow(SharedWindow::publishedWorkspaceID));
    CHECK(anchor.isOnIndexedWorkspaceConnection());
    CHECK(moving.joinSharedLocalWindow(anchor, queueingLoop()) == TerminalSession::SharedWindowJoin::Awaiting);
    window.host->bind(movingSession);
    window.exchange();
    window.exchange();
    pumpPostedWork();
    CHECK_MSG(!moving.sharesRemoteWorkspaceConnection(anchor), "a pane moved with a question open on the connection it rode");
    CHECK_EQ(moving.pendingConsentRequestID(), 77u);

    // Its program rings a bell both attachments owe it, elected for the
    // pane's own connection, which it was typed into; that connection rings
    // it while the pane waits to move. A bell waits for the client to have
    // acknowledged the pane's newest picture, and a client holds its
    // acknowledgement for an interval of its own steady clock, so the passes
    // are spread over real time as a live connection's would be.
    alone.ringBell(movingSession);
    std::uint32_t rungWhileWaiting = 0;
    const auto ringsWhileWaiting = [&] {
        exchangeAlone();
        rungWhileWaiting += moving.takeBellsRung();
        return rungWhileWaiting != 0;
    };
    (void)waitFor(ringsWhileWaiting, 500);
    CHECK_MSG(rungWhileWaiting == 1u, "a bell rung while the pane waited to move did not ring on the connection it rode");
    CHECK_MSG(!moving.sharesRemoteWorkspaceConnection(anchor), "a pane moved with a question open on the connection it rode");

    // Answered where it was asked, the pane moves.
    moving.respondToConsent(moving.pendingConsentToken(), 0);
    exchangeAlone();
    CHECK_MSG(alone.pendingCalls("consent.respond") == 1u, "the answer did not go on the connection that asked");
    CHECK_EQ(window.host->pendingCalls("consent.respond"), 0u);
    pumpPostedWork();
    CHECK_MSG(moving.sharesRemoteWorkspaceConnection(anchor), "a settled pane stayed on a connection of its own");
    // The window's connection delivers the same bell once its client has
    // acknowledged the pane's new picture, over passes spread as above.
    std::uint32_t rungAgain = 0;
    const auto drainsTheWindowsConnection = [&] {
        window.exchange();
        rungAgain += moving.takeBellsRung();
        return false;
    };
    (void)waitFor(drainsTheWindowsConnection, 300);
    CHECK(moving.remoteState() == Ubiquitty::Session::State::Connected);
    CHECK(screenContains(moving, "pane-43"));
    CHECK_MSG(rungAgain == 0u, "a bell the connection it left had rung was rung again");

    // Its input goes on the window's connection, and the program reads each
    // line once.
    constexpr std::string_view late = "after-the-move\n";
    moving.sendInput({ late.data(), late.size() });
    window.exchange();
    CHECK_STR_EQ(window.host->typed(movingSession), "before-the-move\nafter-the-move\n");

    // A bell rung after the move, elected for the window's connection, which
    // was typed into last, rings there.
    window.host->ringBell(movingSession);
    std::uint32_t rungAfterMove = 0;
    const auto ringsAfterMove = [&] {
        window.exchange();
        rungAfterMove += moving.takeBellsRung();
        return rungAfterMove != 0;
    };
    (void)waitFor(ringsAfterMove, 1000);
    window.exchange();
    rungAfterMove += moving.takeBellsRung();
    CHECK_MSG(rungAfterMove == 1u, "a bell rung after the move did not ring once");

    // One carrier each was made, and the one it left has let go.
    CHECK_EQ(window.factory.createCount, 1);
    CHECK_EQ(own.createCount, 1);
    CHECK_MSG(own.lastChannel == nullptr, "the attachment the pane left stayed open");
    CHECK(anchor.remoteState() == Ubiquitty::Session::State::Connected);
}

// A window added to a workspace this process already carries publishes
// through a provisional connection of its own, since the host takes a
// publication only from a pane it groups and the owner carrying the
// destination holds none of the window's. Once the host confirms the
// destination, that connection retires into the owner the registry indexes
// for it; the window's pane moves there once the owner places and pictures it,
// and the provisional attachment detaches.
TEST(aWindowAddedToACarriedWorkspaceMovesOntoItsOwner)
{
    StubFactory provisional;
    SharedWindow window;
    REQUIRE(window.isReady);
    window.attach();
    window.publish();
    window.bindMembers();
    REQUIRE(window.anchor().isOnIndexedWorkspaceConnection());

    // A second window, its one shell committed to the same daemon.
    constexpr std::uint32_t addedSession = 46;
    TerminalSession* added = makeReadingSession();
    REQUIRE(added != nullptr);
    handoffRecord().answer = addedSession;
    REQUIRE(added->handOverToLocalDaemon("/tmp/ubq-share-test.sock", handoffAddress()) == addedSession);
    added->setLocalAdoptionBridgeCommand(window.bridge);
    REQUIRE(added->attachSharedLocalWindow(provisional.address(), queueingLoop()));
    REQUIRE(provisional.lastChannel != nullptr);
    provisional.lastChannel->connect();
    pumpPostedWork();
    constexpr std::uint32_t addedOwnWorkspaceID = 7;
    SharedWindowHost addedHost(addedSession, addedOwnWorkspaceID, *window.host);
    const auto exchangeAdded = [&] {
        if (provisional.lastChannel == nullptr)
            return;
        addedHost.receive(*provisional.lastChannel);
        provisional.lastChannel->receive(addedHost.flush());
        pumpPostedWork();
    };
    exchangeAdded();
    const UbiquittyTest::StructurePane addedOnly { .pane = 1, .sessionID = addedSession };
    addedHost.lay(UbiquittyTest::rowWorkspace(addedOwnWorkspaceID, {}, { &addedOnly, 1 }, 17));
    exchangeAdded();
    addedHost.bind(addedSession, "pane-46");
    exchangeAdded();
    REQUIRE(added->remoteState() == Ubiquitty::Session::State::Connected);
    CHECK(!added->isOnIndexedWorkspaceConnection());

    // Added to the carried workspace through its own connection.
    Ubiquitty::Session::SharedWorkspace shared = SharedWindow::arrangement();
    shared.sessions = { addedSession };
    shared.destinationWorkspaceID = SharedWindow::publishedWorkspaceID;
    REQUIRE(added->publishSharedWorkspace(shared));
    exchangeAdded();
    REQUIRE(addedHost.answerShare(SharedWindow::publishedWorkspaceID));
    const UbiquittyTest::StructurePane grown[] = { { .pane = 1, .sessionID = SharedWindow::sessions[0], .columns = 16, .rows = 24 },
        { .pane = 2, .sessionID = SharedWindow::sessions[1], .columns = 16, .rows = 24 },
        { .pane = 3, .sessionID = SharedWindow::sessions[2], .columns = 15, .rows = 24 },
        { .pane = 4, .sessionID = SharedWindow::sessions[3], .columns = 15, .rows = 24 },
        { .pane = 5, .sessionID = addedSession, .columns = 15, .rows = 24 } };
    const auto destination = UbiquittyTest::rowWorkspace(SharedWindow::publishedWorkspaceID, "Shared Window", grown, 17, 3);
    addedHost.lay(destination);
    provisional.lastChannel->receive(addedHost.flush());
    pumpPostedWork();
    REQUIRE(added->sharedWorkspaceResult().workspaceID == SharedWindow::publishedWorkspaceID);
    CHECK_MSG(!added->sharesRemoteWorkspaceConnection(window.anchor()), "a pane moved before the owner placed it");

    // The owner's host places and pictures the added pane.
    window.host->admit(addedSession);
    window.host->lay(destination);
    window.exchange();
    window.host->bind(addedSession);
    window.exchange();
    window.exchange();
    pumpPostedWork();
    CHECK_MSG(added->sharesRemoteWorkspaceConnection(window.anchor()), "the added window kept a connection of its own");
    window.exchange();
    pumpPostedWork();
    CHECK(added->isOnIndexedWorkspaceConnection());
    CHECK(added->remoteState() == Ubiquitty::Session::State::Connected);
    CHECK(screenContains(*added, "pane-46"));
    CHECK_MSG(provisional.lastChannel == nullptr, "the provisional attachment stayed open");
    CHECK_EQ(provisional.createCount, 1);
    CHECK_EQ(window.factory.createCount, 1);

    constexpr std::string_view typed = "added-line\n";
    added->sendInput({ typed.data(), typed.size() });
    window.exchange();
    CHECK_STR_EQ(window.host->typed(addedSession), "added-line\n");
    for (TerminalSession* pane : window.panes)
        CHECK(pane->sharesRemoteWorkspaceConnection(window.anchor()));

    added->close();
    releaseTerminalSession(added);
}

UBIQUITTY_TEST_MAIN()
