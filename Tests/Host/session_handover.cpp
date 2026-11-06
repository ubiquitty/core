// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/SessionHandover.h"
#include "TerminalFixture.h"

#include <algorithm>
#include <limits>
#include <unistd.h>

namespace Host = Ubiquitty::Host;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace PAL = Ubiquitty::PAL;
namespace Runtime = Ubiquitty::Runtime;
namespace TestDoubles = Ubiquitty::TestDoubles;
namespace Image = Ubiquitty::Image;
namespace Tek = Ubiquitty::Tek;
using Ubiquitty::CapabilityDecision;
using Ubiquitty::CapabilityOrigin;
using Ubiquitty::ClipboardBuffer;
using Ubiquitty::ClusterPool;
using Ubiquitty::NotificationOccasion;
using Ubiquitty::RunTextStore;
using Ubiquitty::Ticks;
using Screen = Host::ScreenPlaneAccess;
using Session = Host::SessionHandover::Session;
using UbiquittyTest::feed;

namespace {

constexpr Sync::HandoverTerminalLimits terminalLimits {
    64 * 1024 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024, 5 * 1024 * 1024, 256 * 1024, 4096,
    { 8192, Image::terminalImageBytesBudgetDefault }, { 4 * 1024 * 1024, 20000, 1024, 100000 },
    { 4 * 1024 * 1024, ClusterPool::maxEntries, RunTextStore::maxEntries, 1024 * 1024 },
    { 256 * 1024, Tek::tekMaxPrimitives }, { 32 * 1024 * 1024, 28 * 1024 * 1024 }
};
constexpr Host::SessionHandoverLimits limits {
    { terminalLimits, { 1024 * 1024, 65536, 4096, 4096, 4096 },
        72 * 1024 * 1024, 5 * 1024 * 1024, 1024 * 1024, 4 * 1024 * 1024, 32 * 1024 * 1024, 64 },
    { 4 * 1024 * 1024, 4096, 65536, 2 * 1024 * 1024 },
    { 4096, 64 }, { 1024 * 1024, 65536, 4096, 4 * 1024 * 1024 }, 80 * 1024 * 1024
};
constexpr Sync::ID128 client { 101, 203 };

struct StreamCounts {
    unsigned reads = 0;
    unsigned writes = 0;
    unsigned drains = 0;
    unsigned processQueries = 0;
    unsigned sizes = 0;
    unsigned restores = 0;
    unsigned destructions = 0;
    unsigned terminations = 0;
    bool operator==(const StreamCounts&) const = default;
};

/// Actual owned FIFO with a controllable delivered prefix; no OS child is used.
class QueuedStream final : public PAL::Stream {
public:
    explicit QueuedStream(std::shared_ptr<StreamCounts> counts = std::make_shared<StreamCounts>())
        : counts(std::move(counts))
    {
    }
    ~QueuedStream() override
    {
        ++counts->destructions;
        counts->terminations += closePolicy == PAL::ProcessClosePolicy::Terminate;
    }
    int read(std::span<char>) override
    {
        ++counts->reads;
        return std::to_underlying(PAL::StreamStatus::Again);
    }
    int write(std::span<const char> bytes) override
    {
        ++counts->writes;
        return accept(this, bytes);
    }
    int writeAll(std::span<const char> bytes) override
    {
        ++counts->writes;
        return queue.submit(bytes, accept, this);
    }
    std::optional<int> writeAllOwned(std::uint64_t owner, std::span<const char> bytes) override
    {
        ++counts->writes;
        return queue.submitOwned(owner, bytes, accept, this);
    }
    bool drainPendingWrite() override
    {
        ++counts->drains;
        CHECK_EQ(queue.drain(accept, this), 0);
        return queue.hasPending();
    }
    bool hasPendingWrite() const override { return queue.hasPending(); }
    std::size_t pendingWriteBytes() const override { return queue.pendingBytes(); }
    std::optional<std::size_t> pendingOwnedWriteBytes(std::uint64_t owner) const override { return queue.pendingBytes(owner); }
    std::optional<std::size_t> discardPendingWrite(std::uint64_t owner) override { return queue.discardPending(owner); }
    bool repairPendingWrite(std::uint64_t owner, std::span<const char> repair) override { return queue.repairPending(owner, repair); }
    std::optional<PAL::OutputQueue::Unwritten> copyPendingWrite() const override { return queue.copyUnwritten(); }
    bool restorePendingWrite(PAL::OutputQueue::Unwritten& pending) override
    {
        ++counts->restores;
        return queue.restoreUnwritten(pending);
    }
    int lastError() const override { return 0; }
    int fileDescriptor() const override { return -1; }
    void setWindowSize(int, int, int, int) override { ++counts->sizes; }
    void setProcessClosePolicy(PAL::ProcessClosePolicy policy) override { closePolicy = policy; }
    PAL::ProcessFacts& process() override
    {
        ++counts->processQueries;
        return facts;
    }
    static int accept(void* context, std::span<const char> bytes)
    {
        auto& stream = *static_cast<QueuedStream*>(context);
        const auto count = std::min(stream.allowance, bytes.size());
        stream.delivered.append(bytes.data(), count);
        stream.allowance -= count;
        return static_cast<int>(count);
    }
    std::shared_ptr<StreamCounts> counts;
    PAL::OutputQueue queue;
    TestDoubles::ProcessFactsDouble facts;
    std::string delivered;
    std::size_t allowance = 0;
    PAL::ProcessClosePolicy closePolicy = PAL::ProcessClosePolicy::Terminate;
};

void checkNoService(const StreamCounts& counts)
{
    CHECK_EQ(counts.reads, 0u);
    CHECK_EQ(counts.writes, 0u);
    CHECK_EQ(counts.drains, 0u);
    CHECK_EQ(counts.processQueries, 0u);
    CHECK_EQ(counts.sizes, 0u);
}

struct Fixture {
    Host::Daemon holder;
    Host::Daemon successor;
    QueuedStream* stream = nullptr;
    Session* session = nullptr;
    Sync::ID128 authority;

    Fixture()
    {
        auto owned = std::make_unique<QueuedStream>();
        stream = owned.get();
        session = &Screen::makeSession(holder, 6, 20, std::move(owned));
        authority = Screen::attachments(holder).create(client, Sync::ClientKind::Human,
                                                   { "fixture-human", true, Sync::ClientKind::Human }, 7, 100)
                        .attachmentID;
        auto inherited = Screen::attachments(holder).exportForHandover(200);
        CHECK(Screen::attachments(successor).inherit(std::move(inherited), Screen::attachments(holder).hostInstanceID(), 200));
        feed(*session->term, "before\r\n\033[?2004h");
        session->coherence = Runtime::FrameCoherence({ 40000, 2500, 17000, true, true });
        session->coherence.noteFrameTaken(*session->term, 19000);
        session->screenSource->publish(Sync::FrameClass::Settled, 19000);
        session->isFrameDue = false;
        session->configuration.fontSize = 17.25;
        session->configuration.titleWindowText = std::string("private\0title", 13);
        session->configuration.remoteCapabilities.readExfiltration = CapabilityDecision::Deny;
        session->configuration.controlEnabled = false;
        session->configuration.controlToken = "fixture-only-credential";
        session->workspaceName = "held workspace";
        session->driverOriginFloor = CapabilityOrigin::Agent;
        session->outputOrigin = CapabilityOrigin::Remote;
        session->firstInputPrincipal = "first-agent";
        session->hasMultipleInputPrincipals = true;
        session->graceExpiresMilliseconds = 43210;
        session->isGraceAwaitingHuman = true;
        session->capabilityPolicy.setCurrentIdentity("first-agent");
        constexpr std::array buffers { ClipboardBuffer::Clipboard, ClipboardBuffer::Primary };
        session->capabilityPolicy.holdClipboardWrite(CapabilityOrigin::Agent, buffers, std::string_view("a\0b", 3));
        session->capabilityPolicy.pendingClipboardWrite()->requestID = 41;
        session->capabilityPolicy.setCurrentIdentity("later-human");
        session->effects.push(Host::HostNotification { "build", "finished", "job", NotificationOccasion::Invisible, client, 19 });
    }

    void queuePaste()
    {
        stream->allowance = 2;
        CHECK(session->screenSource->queueInput(client, 1,
                  Sync::PasteInput { { 'a', '\033', '[', '2', '0', '1' }, 0, Sync::PasteEnd::More }, 20000, authority)
            == Sync::EnqueueOutcome::Queued);
        CHECK(stream->delivered == "\033[");
        CHECK(session->screenSource->queueInput(client, 2, Sync::RawInput { { 'X' } }, 21000, authority) == Sync::EnqueueOutcome::Queued);
        CHECK(session->screenSource->queueInput(client, 1,
                  Sync::PasteInput { { '~', 'b' }, 6, Sync::PasteEnd::Final }, 22000, authority)
            == Sync::EnqueueOutcome::Queued);
        session->screenSource->publish(Sync::FrameClass::Settled, 23000);
    }
};

} // namespace

TEST(actualSessionJoinPreservesEveryOwnerWithoutServicingTheCandidate)
{
    Fixture fixture;
    fixture.queuePaste();
    const auto originalPending = fixture.stream->copyPendingWrite();
    REQUIRE(originalPending && !originalPending->bytes().empty());
    const auto beforeRuntime = fixture.session->inputEncoder->continuation();
    REQUIRE(beforeRuntime && beforeRuntime->pasteCount == 1);
    CHECK_EQ(beforeRuntime->pastes[0].normalizer.heldBytes, 5u);
    const auto sourceCalls = *fixture.stream->counts;
    const auto encoded = Host::SessionHandover::capture(*fixture.session, limits);
    REQUIRE(encoded);
    CHECK(*fixture.stream->counts == sourceCalls);
    auto endpoint = std::make_unique<QueuedStream>();
    auto* stream = endpoint.get();
    auto restored = Host::SessionHandover::restore(fixture.successor, encoded->view(), std::move(endpoint), limits);
    REQUIRE(restored);
    Session& candidate = **restored;
    checkNoService(*stream->counts);
    CHECK_EQ(stream->counts->restores, 1u);
    CHECK(stream->closePolicy == PAL::ProcessClosePolicy::KeepRunning);
    CHECK(!candidate.ownsLandedPastePaths);
    CHECK(candidate.inputEncoder->continuation() == beforeRuntime);
    CHECK(candidate.coherence.continuation() == fixture.session->coherence.continuation());
    CHECK(candidate.term->capabilityPolicy.isInstalled(&candidate.capabilityPolicy));
    CHECK(candidate.term->clipboardClient.get() == &candidate.clipboardClient);
    CHECK(candidate.term->shellClient.get() == &candidate.shellClient);
    CHECK(candidate.driverOriginFloor == CapabilityOrigin::Agent);
    CHECK(candidate.outputOrigin == CapabilityOrigin::Remote);
    CHECK(candidate.firstInputPrincipal == "first-agent");
    CHECK(candidate.hasMultipleInputPrincipals);
    CHECK(candidate.configuration.fontSize == 17.25);
    CHECK(candidate.configuration.remoteCapabilities.readExfiltration == CapabilityDecision::Deny);
    CHECK(candidate.configuration.controlEnabled == false);
    CHECK(candidate.configuration.titleWindowText == fixture.session->configuration.titleWindowText);
    CHECK(candidate.configuration.controlToken == fixture.session->configuration.controlToken);
    const auto* held = candidate.capabilityPolicy.pendingClipboardWrite();
    REQUIRE(held);
    CHECK(held->identity == "first-agent");
    CHECK_EQ(held->requestID, 41u);
    CHECK(held->origin == CapabilityOrigin::Agent);
    CHECK(std::string_view(held->bytes.get(), held->byteCount) == std::string_view("a\0b", 3));
    CHECK(candidate.capabilityPolicy.currentIdentity() == "later-human");
    REQUIRE(candidate.effects.heldEffects().size() == 1);
    const auto& notice = std::get<Host::HostNotification>(candidate.effects.heldEffects().front());
    CHECK(notice.primary == client);
    CHECK_EQ(notice.pendingPrimaryClientID, 19u);
    const auto recaptured = Host::SessionHandover::capture(candidate, limits);
    REQUIRE(recaptured);
    CHECK(std::ranges::equal(encoded->view(), recaptured->view()));
    const auto pending = stream->copyPendingWrite();
    REQUIRE(pending);
    CHECK(std::ranges::equal(pending->bytes(), originalPending->bytes()));
    REQUIRE(pending->runs().size() == originalPending->runs().size());
    for (std::size_t index = 0; index < pending->runs().size(); ++index) {
        CHECK_EQ(pending->runs()[index].owner, originalPending->runs()[index].owner);
        CHECK_EQ(pending->runs()[index].bytes, originalPending->runs()[index].bytes);
    }
}

TEST(actualSessionResumesAcceptedSuffixBeforeUnsubmittedPasteAndKeysExactlyOnce)
{
    Fixture fixture;
    fixture.queuePaste();
    const std::string deliveredPrefix = fixture.stream->delivered;
    const auto encoded = Host::SessionHandover::capture(*fixture.session, limits);
    REQUIRE(encoded);
    auto endpoint = std::make_unique<QueuedStream>();
    auto* after = endpoint.get();
    auto restored = Host::SessionHandover::restore(fixture.successor, encoded->view(), std::move(endpoint), limits);
    REQUIRE(restored);
    fixture.stream->delivered.clear();
    for (auto* stream : { fixture.stream, after }) {
        stream->allowance = 4096;
        CHECK(!stream->drainPendingWrite());
    }
    for (auto* session : { fixture.session, restored->get() }) {
        session->screenSource->drainInput(30000);
        CHECK_EQ(session->screenSource->inputLedger().lastWritten(client).value_or(0), 2u);
        CHECK(!session->screenSource->hasPendingInput());
        CHECK(session->screenSource->queueInput(client, 1,
                  Sync::PasteInput { { 'a', '\033', '[', '2', '0', '1', '~', 'b' }, 0, Sync::PasteEnd::Final }, 31000, fixture.authority)
            == Sync::EnqueueOutcome::Duplicate);
        CHECK(session->screenSource->queueInput(client, 2, Sync::RawInput { { 'X' } }, 32000, fixture.authority) == Sync::EnqueueOutcome::Duplicate);
        feed(*session->term, "\r\nafter");
        session->coherence.noteDrainPass(*session->term, 33000, Runtime::PassEnd::WouldBlock);
    }
    CHECK(fixture.stream->delivered == after->delivered);
    CHECK(deliveredPrefix + after->delivered == "\033[200~a\033[201b\033[201~X");
    for (const auto now : { 35000u, 36000u }) {
        const auto ticks = Ticks::fromMilliseconds(now / 1000);
        const auto expected = fixture.session->coherence.admit(*fixture.session->term, now, ticks, Runtime::ReadReadiness::Quiet);
        const auto actual = (*restored)->coherence.admit(*(*restored)->term, now, ticks, Runtime::ReadReadiness::Quiet);
        CHECK_EQ(expected.isAdmitted, actual.isAdmitted);
        CHECK(expected.frameClass == actual.frameClass);
        CHECK((*restored)->coherence.continuation() == fixture.session->coherence.continuation());
    }
    CHECK(!after->hasPendingWrite());
}

TEST(abandonedAndRefusedSessionCandidatesCannotTerminateTheChildOrUnlinkHolderFiles)
{
    char path[] = "/tmp/ubq-session-handover-file-XXXXXX";
    const int descriptor = ::mkstemp(path);
    REQUIRE(descriptor >= 0);
    ::close(descriptor);
    {
        Fixture fixture;
        fixture.session->landedPastePaths.emplace_back(path);
        const auto encoded = Host::SessionHandover::capture(*fixture.session, limits);
        REQUIRE(encoded);
        const auto counts = std::make_shared<StreamCounts>();
        {
            auto restored = Host::SessionHandover::restore(fixture.successor, encoded->view(), std::make_unique<QueuedStream>(counts), limits);
            REQUIRE(restored);
            CHECK(::access(path, F_OK) == 0);
            CHECK_EQ((*restored)->landedPastePaths.size(), 1u);
        }
        checkNoService(*counts);
        CHECK_EQ(counts->destructions, 1u);
        CHECK_EQ(counts->terminations, 0u);
        CHECK(::access(path, F_OK) == 0);
        std::vector<std::uint8_t> corrupt(encoded->view().begin(), encoded->view().end());
        corrupt.back() ^= 1;
        const auto refusedCounts = std::make_shared<StreamCounts>();
        CHECK(!Host::SessionHandover::restore(fixture.successor, corrupt, std::make_unique<QueuedStream>(refusedCounts), limits));
        checkNoService(*refusedCounts);
        CHECK_EQ(refusedCounts->destructions, 1u);
        CHECK_EQ(refusedCounts->terminations, 0u);
        const auto occupiedCounts = std::make_shared<StreamCounts>();
        auto occupied = std::make_unique<QueuedStream>(occupiedCounts);
        CHECK_EQ(occupied->writeAll(std::span("old", 3)), 0);
        *occupiedCounts = {};
        CHECK(!Host::SessionHandover::restore(fixture.successor, encoded->view(), std::move(occupied), limits));
        checkNoService(*occupiedCounts);
        CHECK_EQ(occupiedCounts->restores, 1u);
        CHECK_EQ(occupiedCounts->terminations, 0u);
        CHECK(::access(path, F_OK) == 0);
        CHECK_EQ(fixture.stream->counts->terminations, 0u);
        CHECK(Host::SessionHandover::capture(*fixture.session, limits));
    }
    CHECK(::access(path, F_OK) != 0);
}

TEST(restoredPartialPasteWithdrawalRepairsOnlyItsDelimiterAndKeepsSystemBytes)
{
    Fixture fixture;
    fixture.queuePaste();
    const std::string deliveredPrefix = fixture.stream->delivered;
    CHECK_EQ(fixture.stream->writeAll(std::span("R", 1)), 0);
    const auto encoded = Host::SessionHandover::capture(*fixture.session, limits);
    REQUIRE(encoded);
    auto endpoint = std::make_unique<QueuedStream>();
    auto* after = endpoint.get();
    auto restored = Host::SessionHandover::restore(fixture.successor, encoded->view(), std::move(endpoint), limits);
    REQUIRE(restored);
    fixture.stream->delivered.clear();
    Screen::attachments(fixture.holder).noteDetached(fixture.authority);
    Screen::attachments(fixture.successor).noteDetached(fixture.authority);
    for (auto* session : { fixture.session, restored->get() }) {
        session->screenSource->withdrawInputAuthority(fixture.authority, 30000);
        CHECK(!session->screenSource->hasPendingInput());
        CHECK(!session->screenSource->inputLedger().lastWritten(client));
    }
    for (auto* stream : { fixture.stream, after }) {
        stream->allowance = 4096;
        CHECK(!stream->drainPendingWrite());
    }
    CHECK(fixture.stream->delivered == after->delivered);
    CHECK(deliveredPrefix + after->delivered == "\033[200~\033[201~R");
    const auto delivered = after->delivered;
    CHECK((*restored)->screenSource->queueInput(client, 3, Sync::RawInput { { 'Z' } }, 31000, fixture.authority) == Sync::EnqueueOutcome::Queued);
    CHECK(after->delivered == delivered);
    CHECK(!(*restored)->screenSource->hasPendingInput());
    CHECK(!(*restored)->screenSource->inputLedger().lastWritten(client));
}

UBIQUITTY_TEST_MAIN()
