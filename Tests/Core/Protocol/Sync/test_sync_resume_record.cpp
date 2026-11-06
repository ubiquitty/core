// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_resume_record — the persisted resume record's rules
// (Docs/sync.md §6.9 "Keep the picture across a relaunch"): a relaunched
// client draws the stored picture and continues its attachment; the record
// holds the fresh secret before any input goes on a new connection; a
// HistoryReset purges it before anything else is applied; a picture that does
// not verify is not drawn; unsent input comes back as drafts and is never
// sent; a record past the checkpoint cap is not written.

#include "AttachmentLink.h"
#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"
#include "ScreenPlaneFixture.h"
#include "SyncLink.h"

#include "Core/OSC/Activity.h"
#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/ResumeRecord.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Session/ResumeRecordFormat.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using namespace UbiquittyTest::AttachmentLink;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

/// A store a test watches and can make lose its writes, as a process killed
/// before the write does.
class ObservedStore final : public ResumeRecordStore {
public:
    [[nodiscard]] HistoryPersistence historyPersistence() const override { return persistence; }
    [[nodiscard]] ResumeBindingOutcome bindWelcome(const Welcome& welcome, ID128 instance, const std::array<std::uint8_t, 32>& pin) override
    {
        ++bindings;
        if (onBind)
            onBind(welcome, instance, pin);
        if (isRefusingBinding)
            return ResumeBindingOutcome::Refused;
        if (requiresCertificatePin && std::ranges::all_of(pin, [](auto byte) { return byte == 0; }))
            return ResumeBindingOutcome::MemoryOnly;
        return ResumeBindingOutcome::Bound;
    }

    [[nodiscard]] ResumeWriteOutcome write(const ResumeRecord& written) override
    {
        ++writes;
        if (onWrite)
            onWrite(written);
        if (isRefusingWrites)
            return ResumeWriteOutcome::Unchanged;
        if (!isLosingWrites)
            record = written;
        return isUncertainWrite ? ResumeWriteOutcome::DurabilityUnknown : ResumeWriteOutcome::Committed;
    }

    [[nodiscard]] std::optional<ResumeRecord> read() override
    {
        ++reads;
        return record;
    }

    [[nodiscard]] ResumePurgeOutcome purge() override
    {
        ++purges;
        if (onPurge)
            onPurge();
        if (isRefusingPurges)
            return ResumePurgeOutcome::Refused;
        record.reset();
        return ResumePurgeOutcome::Purged;
    }

    std::optional<ResumeRecord> record;
    std::function<void(const ResumeRecord&)> onWrite;
    std::function<void()> onPurge;
    std::function<void(const Welcome&, ID128, const std::array<std::uint8_t, 32>&)> onBind;
    bool isRefusingBinding = false;
    bool requiresCertificatePin = false;
    bool isLosingWrites = false;
    bool isRefusingWrites = false;
    bool isUncertainWrite = false;
    bool isRefusingPurges = false;
    int reads = 0;
    int writes = 0;
    int purges = 0;
    int bindings = 0;
    HistoryPersistence persistence = HistoryPersistence::Disabled;
};

/// Observes typed presentation after its persistence barrier.
class ReceiptObserver final : public ClientPaneDelegate {
public:
    void paneEffect(std::uint32_t, const EffectView& effect, const EffectBody&, EffectDisposition) override
    {
        sequences.push_back(effect.sequence);
        if (onEffect)
            onEffect(effect);
    }

    std::vector<std::uint64_t> sequences;
    std::function<void(const EffectView&)> onEffect;
};

/// Whether a terminal's screen shows the text on one row.
bool screenShows(const Ubiquitty::Terminal& terminal, std::string_view text)
{
    for (int row = 0; row < terminal.rows; ++row) {
        std::string line;
        for (int column = 0; column < terminal.columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, row, column);
            line.push_back(cell && cell->codePoint >= 0x20 && cell->codePoint < 0x7f ? static_cast<char>(cell->codePoint) : ' ');
        }
        if (line.contains(text))
            return true;
    }
    return false;
}

/// Whether a record holds the text: in a picture it would draw, or in a
/// draft.
bool recordContains(const ResumeRecord& record, std::string_view text)
{
    for (const ResumeRecordPane& pane : record.panes) {
        if (pane.checkpoint.empty())
            continue;
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 24, 80);
        ScreenReplica replica;
        if (applyCheckpoint(terminal, pane.checkpoint, replica) == ApplyOutcome::Applied && screenShows(terminal, text))
            return true;
    }
    for (const ResumeDraft& draft : record.drafts) {
        const auto* typed = std::get_if<RawInput>(&draft.intent);
        if (typed && std::string(typed->bytes.begin(), typed->bytes.end()).contains(text))
            return true;
    }
    return false;
}

/// The host writes to its pane and publishes the frame.
void hostWrites(Host& host, std::string_view bytes, std::uint64_t now)
{
    UbiquittyTest::feed(host.pane.terminal, bytes);
    host.pane.source->publish(FrameClass::Settled, now);
}

/// The host's program posts a status record, and the host publishes the
/// screen that carries it and the records a hidden pane is sent.
void hostPosts(Host& host, std::string_view record, std::uint64_t now)
{
    UbiquittyTest::feed(host.pane.terminal, record);
    host.pane.source->noteTerminalChanged();
    host.pane.source->setStatus(statusRecordsOf(host.pane.terminal));
    host.pane.source->publish(FrameClass::Settled, now);
}

/// The state a terminal's primary status record shows.
Ubiquitty::OSC::ProgramState shownState(const Ubiquitty::Terminal& terminal)
{
    const Ubiquitty::OSC::ProgramStatus* primary = terminal.programStatus.primary();
    return primary ? primary->state : Ubiquitty::OSC::ProgramState::None;
}

constexpr std::string_view postsWorking = "\033]7501;id=job:state=working\033\\";
constexpr std::string_view postsBlocked = "\033]7501;id=job:state=blocked:kind=question\033\\";

/// A stable representation of a terminal for before/after refusal checks.
std::vector<std::uint8_t> pictureOf(const Ubiquitty::Terminal& terminal)
{
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    CHECK(publisher.capture(terminal) && publisher.checkpoint(terminal, 1, body, baseline));
    return body;
}

/// Text that fills more of the screen than a prompt: styled, wide, joined
/// and linked, so a picture that lost any of it disagrees with the host's.
constexpr std::string_view rich = "\x1b[1;31mred\x1b[0m plain e\xcc\x81 \xe4\xb8\xad\xe6\x96\x87 \x1b]8;;https://example.org\x1b\\link\x1b]8;;\x1b\\ end\r\n";

} // namespace

TEST(anSSHAttachmentWithoutACertificatePinStillBecomesLive)
{
    Host host({ 41, 42 });
    ObservedStore store;
    store.requiresCertificatePin = true;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(client.connection());
    CHECK_MSG(client.connection()->state() == ConnectionState::Attached, "SSH Welcome must not be refused because persistent TLS storage is unavailable");
    REQUIRE(client.state() == ClientAttachmentState::Live);
    REQUIRE(client.paneTerminal(paneID));
    CHECK_EQ(store.writes, 0);
    REQUIRE(client.send(paneID, raw("ssh input"), now));
    pump(host, client, now);
    CHECK(host.pane.writer.bytes() == "ssh input");
}

TEST(aSessionGoneRefusalRetiresResumeAuthorityButHostFullKeepsIt)
{
    for (const auto reason : { ByeReason::SessionGone, ByeReason::HostFull }) {
        Host host({ 41, 42 });
        ObservedStore store;
        ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        std::uint64_t now = 1;
        attachFresh(host, client, now);
        REQUIRE(store.record);
        drop(host, client, now);
        REQUIRE(client.send(paneID, raw("unsent"), now));
        (void)client.connect(now);
        std::vector<std::uint8_t> body;
        encode(body, Bye { .reason = reason, .detail = {} });
        std::vector<std::uint8_t> frame;
        REQUIRE(appendFrame(frame, FrameKind::Bye, 0, body));
        client.receiveStream(frame, ++now);
        const bool hasEnded = reason == ByeReason::SessionGone;
        CHECK(client.isEnded(paneID) == hasEnded);
        CHECK(client.held().has_value() != hasEnded);
        CHECK(store.record.has_value() != hasEnded);
        if (hasEnded) {
            CHECK(!client.send(paneID, raw("later"), now));
            REQUIRE(client.endedPanes().size() == 1);
            CHECK_EQ(client.endedPanes().front().discardedInputEvents, 1u);
        }
        client.transportLost(++now);
        client.persist();
        CHECK(store.record.has_value() != hasEnded);
    }
}

TEST(aGoneAttachmentsFailedRecordRetirementBlocksRestoration)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(store.record);
    drop(host, client, now);
    (void)client.connect(now);
    store.isRefusingPurges = true;
    std::vector<std::uint8_t> body;
    encode(body, Bye { .reason = ByeReason::SessionGone, .detail = {} });
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::Bye, 0, body));
    client.receiveStream(frame, ++now);
    CHECK(!client.held());
    client.transportLost(++now);
    CHECK(!client.restoreFromStore());
    store.isRefusingPurges = false;
    client.persist();
    CHECK(!store.record);
    CHECK(!client.restoreFromStore());
}

TEST(aWelcomeFollowedBySessionGoneCannotRestoreRetiredAuthorityOnClose)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    const Welcome welcome = *client.connection()->welcome();
    drop(host, client, now);
    (void)client.connect(now);
    std::vector<std::uint8_t> body;
    encode(body, welcome);
    std::vector<std::uint8_t> frames;
    REQUIRE(appendFrame(frames, FrameKind::Welcome, 0, body));
    body.clear();
    encode(body, Bye { .reason = ByeReason::SessionGone, .detail = {} });
    REQUIRE(appendFrame(frames, FrameKind::Bye, 0, body));
    client.receiveStream(frames, ++now);
    REQUIRE(client.connection()->byeReason() == ByeReason::SessionGone);
    CHECK(!client.held());
    CHECK(!store.record);
    client.transportLost(++now);
    client.persist();
    CHECK(!client.held());
    CHECK(!store.record);
}

TEST(aPinnedAttachmentFallsBackToMemoryAndCanPersistAgain)
{
    Host host({ 41, 42 });
    ObservedStore store;
    store.requiresCertificatePin = true;
    ReceiptObserver observer;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &observer, &store);
    const std::array<std::uint8_t, 32> pin { 9 };
    client.setCertificatePin(pin);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(store.record);
    drop(host, client, now);
    const auto writes = store.writes;
    client.setCertificatePin({});
    REQUIRE(host.pane.source->appendEffect(BellEffect {}, clientInstance));
    reconnect(host, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    REQUIRE(client.paneTerminal(paneID));
    CHECK(!store.record);
    CHECK_EQ(store.writes, writes);
    CHECK_EQ(observer.sequences.size(), 1u);
    REQUIRE(client.send(paneID, raw("one"), now));
    pump(host, client, now);
    CHECK(host.pane.writer.bytes() == "one");
    client.persist();
    CHECK_EQ(store.writes, writes);

    drop(host, client, now);
    reconnect(host, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    CHECK(!store.record);
    CHECK_EQ(store.writes, writes);
    CHECK_EQ(observer.sequences.size(), 1u);
    CHECK(host.pane.writer.bytes() == "one");
    REQUIRE(client.send(paneID, raw("two"), now));
    pump(host, client, now);
    CHECK(host.pane.writer.bytes() == "onetwo");

    drop(host, client, now);
    client.setCertificatePin(pin);
    reconnect(host, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    REQUIRE(store.record);
    CHECK(store.record->certificatePin == pin);
    CHECK(store.writes > writes);
}

TEST(memoryOnlyAdmissionRequiresRetiringThePreviousRecord)
{
    Host host({ 41, 42 });
    ObservedStore store;
    store.requiresCertificatePin = true;
    ReceiptObserver observer;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &observer, &store);
    client.setCertificatePin({ 9 });
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    drop(host, client, now);
    REQUIRE(store.record);
    const auto writes = store.writes;
    const auto picture = pictureOf(*client.paneTerminal(paneID));
    store.isRefusingPurges = true;
    client.setCertificatePin({});
    REQUIRE(client.send(paneID, raw("held"), now));
    REQUIRE(host.pane.source->appendEffect(BellEffect {}, clientInstance));
    reconnect(host, client, now);
    REQUIRE(client.connection());
    CHECK(client.connection()->state() == ConnectionState::Closed);
    CHECK(!client.connection()->welcome());
    CHECK(observer.sequences.empty());
    CHECK(host.pane.writer.bytes().empty());
    CHECK(pictureOf(*client.paneTerminal(paneID)) == picture);
    client.persist();
    CHECK_EQ(store.writes, writes);
    CHECK(store.record.has_value());
    CHECK(store.purges > 0);
}

TEST(aWelcomeBindsTheStoreBeforeAnyRecordOrEffectInTheSameRead)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ReceiptObserver observer;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &observer, &store);
    std::array<std::uint8_t, 32> pin {};
    pin[0] = 9;
    client.setCertificatePin(pin);
    store.onBind = [&](const Welcome& welcome, ID128 instance, const auto& verifiedPin) {
        CHECK(welcome.hostInstanceID == host.table.hostInstanceID());
        CHECK(welcome.principal == "alice");
        CHECK(instance == clientInstance);
        CHECK(verifiedPin == pin);
        CHECK_EQ(store.writes, 0);
        CHECK(!client.held());
    };
    store.onWrite = [&](const ResumeRecord&) { CHECK_EQ(store.bindings, 1); };
    std::uint64_t now = 1;
    host.accept();
    (void)client.connect(now);
    host.connection->receiveStream(client.takeStreamOutput(), now);
    const auto handle = host.connection->bindPane(*host.pane.source);
    REQUIRE(handle != 0);
    host.connection->service(now);
    std::vector<std::uint8_t> down;
    while (host.connection->popStreamFrame(down, now)) {
    }
    const auto effect = encodeEffect(EffectView { .paneID = paneID, .generation = 0, .sequence = 1, .primary = clientInstance, .kind = EffectKind::Bell, .count = 1, .target = 0, .payload = {} });
    REQUIRE(effect);
    const auto packet = encodeEffectPacket(paneID, 0, 1, 0, effect->view());
    REQUIRE(packet);
    REQUIRE(appendFrame(down, FrameKind::Effect, handle, packet->view()));
    client.receiveStream(down, now);
    CHECK_EQ(store.bindings, 1);
    REQUIRE(observer.sequences.size() == 1);
    REQUIRE(store.record);
    CHECK(store.record->certificatePin == pin);
}

TEST(aRefusedWelcomeCannotChangeThePictureCommitEffectsOrReleaseInput)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ReceiptObserver observer;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &observer, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    drop(host, client, now);
    REQUIRE(client.paneTerminal(paneID));
    const auto held = client.held();
    const auto picture = pictureOf(*client.paneTerminal(paneID));
    const auto writes = store.writes;
    store.isRefusingBinding = true;
    store.isRefusingPurges = true;
    REQUIRE(client.send(paneID, raw("never sent"), now));
    REQUIRE(host.pane.source->appendEffect(BellEffect {}, clientInstance));
    reconnect(host, client, now);
    REQUIRE(client.connection());
    CHECK(client.connection()->state() == ConnectionState::Closed);
    CHECK(!client.connection()->welcome());
    CHECK(observer.sequences.empty());
    CHECK(host.pane.writer.bytes().empty());
    REQUIRE(client.held() && held);
    CHECK(client.held()->resumeSecret == held->resumeSecret);
    CHECK(pictureOf(*client.paneTerminal(paneID)) == picture);
    client.persist();
    CHECK_EQ(store.writes, writes);
    CHECK(store.purges > 0);
    // A later successful retirement alone cannot make the rejected identity
    // eligible; a new authenticated Welcome must bind it first.
    store.isRefusingPurges = false;
    client.persist();
    CHECK_EQ(store.writes, writes);
    CHECK(!store.record);
    drop(host, client, now);
    store.isRefusingBinding = false;
    reconnect(host, client, now);
    CHECK(client.connection()->state() == ConnectionState::Attached);
    CHECK(store.writes > writes);
}

TEST(aColdResumeRetainsAPartialEffectReceiptBeforeItsCallback)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ReceiptObserver firstObserver;
    std::uint64_t now = 1;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &firstObserver, &store);
        attachFresh(host, first, now);
        firstObserver.onEffect = [&](const EffectView& effect) {
            REQUIRE(store.record && store.record->panes.size() == 1);
            CHECK_EQ(store.record->panes[0].effects.consumed, effect.sequence);
            CHECK_EQ(store.record->panes[0].effects.acknowledged, 0u);
            CHECK(store.record->resumeSecret == first.held()->resumeSecret);
        };
        REQUIRE(host.pane.source->appendEffect(BellEffect {}, clientInstance));
        REQUIRE(host.pane.source->appendEffect(AttentionEffect {}, clientInstance));
        bool didDeliver = false;
        for (int attempt = 0; attempt < 20 && !didDeliver; ++attempt) {
            now += 5'000;
            host.connection->service(now);
            std::vector<std::uint8_t> down;
            while (host.connection->popStreamFrame(down, now)) {
            }
            FrameDecoder decoder;
            decoder.append(down);
            while (const auto frame = decoder.next()) {
                std::vector<std::uint8_t> one;
                REQUIRE(appendFrame(one, frame->kind, frame->channel, frame->body));
                first.receiveStream(one, now);
                if (frame->kind == FrameKind::Effect) {
                    didDeliver = true;
                    break;
                }
            }
        }
        REQUIRE(didDeliver);
        REQUIRE(firstObserver.sequences.size() == 1);
        REQUIRE(store.record && store.record->panes.size() == 1);
        CHECK_EQ(store.record->panes[0].effects.consumed, 1u);
        CHECK_EQ(store.record->panes[0].effects.acknowledged, 0u);
        // Destruction has no persist call: the pre-callback commit is the
        // only record available to the replacement process.
    }
    host.lose(now);
    ReceiptObserver resumedObserver;
    ClientAttachment resumed(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &resumedObserver, &store);
    REQUIRE(resumed.restoreFromStore());
    reconnect(host, resumed, now);
    REQUIRE(resumedObserver.sequences.size() == 1);
    CHECK_EQ(resumedObserver.sequences[0], 2u);
    REQUIRE(store.record && store.record->panes.size() == 1);
    CHECK_EQ(store.record->panes[0].effects.consumed, 2u);
    CHECK_EQ(store.record->panes[0].effects.acknowledged, 2u);
    CHECK_EQ(host.pane.source->effects().retiredThrough(), 2u);
}

TEST(aRefusedEffectCommitNeverPostsOrAcknowledgesAndCanRetryAfterReconnect)
{
    for (const auto outcome : { ResumeWriteOutcome::Unchanged, ResumeWriteOutcome::DurabilityUnknown }) {
        Host host({ 41, 42 });
        ObservedStore store;
        ReceiptObserver observer;
        ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &observer, &store);
        std::uint64_t now = 1;
        attachFresh(host, client, now);
        store.isRefusingWrites = outcome == ResumeWriteOutcome::Unchanged;
        store.isUncertainWrite = outcome == ResumeWriteOutcome::DurabilityUnknown;
        REQUIRE(host.pane.source->appendEffect(BellEffect {}, clientInstance));
        pumpFor(host, client, now, 50'000);
        CHECK(observer.sequences.empty());
        REQUIRE(client.connection());
        CHECK(client.connection()->state() == ConnectionState::Closed);
        CHECK_EQ(host.pane.source->effects().retiredThrough(), 0u);
        CHECK(!store.record);
        store.isRefusingWrites = false;
        store.isUncertainWrite = false;
        drop(host, client, now);
        reconnect(host, client, now);
        REQUIRE(observer.sequences.size() == 1);
        CHECK_EQ(observer.sequences[0], 1u);
    }
}

TEST(aMalformedStoredEffectReceiptIsRefusedBeforeLookingUpATerminal)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    {
        ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, client, now);
        client.persist();
    }
    REQUIRE(store.record && store.record->panes.size() == 1);
    store.record->panes[0].effects = { 2, 1 };
    ClientAttachment restored(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    CHECK(!restored.restoreFromStore());
    CHECK(!restored.paneTerminal(paneID));
}

TEST(aColdReceiptSurvivesAnOmittedPicture)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    {
        ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, client, now);
        client.persist();
    }
    REQUIRE(store.record && store.record->panes.size() == 1);
    store.record->panes[0].effects = { 7, 9 };
    store.record->panes[0].checkpoint.clear();
    ClientAttachment restored(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    REQUIRE(restored.restoreFromStore());
    CHECK(!restored.paneTerminal(paneID));
    (void)restored.connect(now);
    const auto output = restored.takeStreamOutput();
    FrameDecoder decoder;
    decoder.append(output);
    const auto frame = decoder.next();
    REQUIRE(frame && frame->kind == FrameKind::Greet);
    const auto greet = decodeGreet(frame->body);
    REQUIRE(greet && greet->resume && greet->resume->panes.size() == 1);
    CHECK_EQ(greet->resume->panes[0].effectsAcknowledged, 7u);
    restored.persist();
    REQUIRE(store.record && store.record->panes.size() == 1);
    CHECK_EQ(store.record->panes[0].effects.consumed, 9u);
}

TEST(anEffectCommitCannotBypassFailedRecordRetirement)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ReceiptObserver observer;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &observer, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    store.isRefusingWrites = true;
    store.isRefusingPurges = true;
    REQUIRE(host.pane.source->appendEffect(BellEffect {}, clientInstance));
    pumpFor(host, client, now, 50'000);
    CHECK(observer.sequences.empty());
    const auto refusedWrites = store.writes;
    store.isRefusingWrites = false;
    drop(host, client, now);
    reconnect(host, client, now);
    CHECK(observer.sequences.empty());
    CHECK_EQ(store.writes, refusedWrites);
    CHECK_EQ(host.pane.source->effects().retiredThrough(), 0u);
    store.isRefusingPurges = false;
    drop(host, client, now);
    reconnect(host, client, now);
    REQUIRE(observer.sequences.size() == 1);
    CHECK_EQ(observer.sequences[0], 1u);
}

TEST(anEffectCallbackCanReconnectWithoutRepeatingItsDurableReceipt)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ReceiptObserver observer;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &observer, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    observer.onEffect = [&](const EffectView& effect) {
        REQUIRE(store.record && store.record->panes.size() == 1);
        CHECK_EQ(store.record->panes[0].effects.consumed, effect.sequence);
        CHECK(store.record->resumeSecret == client.held()->resumeSecret);
        if (effect.sequence == 1) {
            drop(host, client, now);
            reconnect(host, client, now);
        }
    };
    REQUIRE(host.pane.source->appendEffect(BellEffect {}, clientInstance));
    REQUIRE(host.pane.source->appendEffect(AttentionEffect {}, clientInstance));
    pumpFor(host, client, now, 100'000);
    REQUIRE(observer.sequences.size() == 2);
    CHECK_EQ(observer.sequences[0], 1u);
    CHECK_EQ(observer.sequences[1], 2u);
    CHECK(client.state() == ClientAttachmentState::Live);
    CHECK(host.connection->state() == ConnectionState::Attached);
}

// A HistoryReset purges the record before anything else of the read is
// applied, and the record written after it holds no picture of the pane
// until a frame newer than the clear: nothing of the old epoch is kept.
TEST(afterAHistoryResetTheRecordHoldsNoLineOfTheOldEpoch)
{
    Host host({ 41, 42 });
    ObservedStore store;
    store.persistence = HistoryPersistence::Enabled;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    std::string session;
    for (int line = 0; line < 40; ++line)
        session += "line " + std::to_string(line) + "\r\n";
    session += "old epoch secret";
    hostWrites(host, session, now);
    pumpFor(host, client, now, 100'000);
    client.persist();
    REQUIRE(store.record.has_value());
    REQUIRE(recordContains(*store.record, "old epoch secret"));
    REQUIRE(!store.record->panes[0].history.empty());

    // The program clears the screen and its history. Only the reset reaches
    // the client; the frame that clears the screen is still on its way.
    hostWrites(host, "\x1b"
                     "c",
        now);
    bool wasShownAtPurge = false;
    store.onPurge = [&] { wasShownAtPurge = client.paneTerminal(paneID) && screenShows(*client.paneTerminal(paneID), "old epoch secret"); };
    host.connection->service(now);
    std::vector<std::uint8_t> down;
    while (host.connection->popStreamFrame(down, now)) {
    }
    FrameDecoder decoder;
    decoder.append(down);
    bool hasReset = false;
    std::vector<std::uint8_t> rest;
    while (const std::optional<Frame> frame = decoder.next()) {
        std::vector<std::uint8_t> one;
        (void)appendFrame(one, frame->kind, frame->channel, frame->body);
        if (hasReset) {
            rest.insert(rest.end(), one.begin(), one.end());
            continue;
        }
        client.receiveStream(one, now);
        hasReset = frame->kind == FrameKind::HistoryReset;
    }
    REQUIRE(hasReset);
    CHECK(store.purges >= 1);
    // Nothing else was applied when the record went.
    CHECK(wasShownAtPurge);
    REQUIRE(store.record.has_value());
    CHECK(!recordContains(*store.record, "old epoch secret"));
    CHECK(store.record->panes[0].history.empty());
    CHECK(store.record->attachmentID == client.held()->attachmentID);

    // The frame after the clear is a picture the record may hold again.
    client.receiveStream(rest, now);
    pumpFor(host, client, now, 100'000);
    client.persist();
    REQUIRE(store.record.has_value());
    REQUIRE(store.record->panes.size() == 1);
    CHECK(!store.record->panes[0].checkpoint.empty());
    CHECK(!recordContains(*store.record, "old epoch secret"));
}

TEST(historyPersistenceIsOptInAndRestoresBeforeAnyPacket)
{
    for (const auto persistence : { HistoryPersistence::Disabled, HistoryPersistence::Enabled }) {
        Host host({ 41, 42 });
        ObservedStore store;
        store.persistence = persistence;
        ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        std::uint64_t now = 1;
        attachFresh(host, client, now);
        std::string text;
        for (int line = 0; line < 80; ++line)
            text += "history-only-" + std::to_string(line) + "\r\n";
        hostWrites(host, text, now);
        pumpFor(host, client, now, 200'000);
        REQUIRE(client.paneTerminal(paneID));
        const auto rows = Ubiquitty::Screen::scrollbackCount(client.paneTerminal(paneID));
        REQUIRE(rows > 0);
        drop(host, client, now);
        REQUIRE(store.record && store.record->panes.size() == 1);
        CHECK(store.record->panes[0].history.empty() == (persistence == HistoryPersistence::Disabled));
        if (persistence == HistoryPersistence::Enabled) {
            HistoryCache saved(store.record->hostInstanceID, paneID, UbiquittyTest::syncClientPolicy.historyCacheLines);
            REQUIRE(saved.restoreSnapshot(store.record->panes[0].history, UbiquittyTest::syncClientPolicy.maxCheckpointBytes, UbiquittyTest::syncClientPolicy.maxCheckpointBytes));
            CHECK_EQ(saved.key().epoch, store.record->panes[0].replica.historyEpoch);
            CHECK_EQ(saved.head(), store.record->panes[0].replica.historyHead);
            const auto cellBytes = checkpointCellStorageBytes(store.record->panes[0].checkpoint);
            REQUIRE(cellBytes && saved.retainedStorageBytes() > 0);
            auto bounded = UbiquittyTest::syncClientPolicy;
            bounded.maxCheckpointBytes = *cellBytes + saved.retainedStorageBytes() - 1;
            REQUIRE(bounded.maxCheckpointBytes > store.record->panes[0].history.size());
            ClientAttachment refused(bounded, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
            CHECK(!refused.restoreFromStore());
            CHECK(!refused.held() && !refused.paneTerminal(paneID));

            const auto original = store.record->panes[0].history;
            store.record->panes[0].history.back() ^= 0x80;
            ClientAttachment malformed(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
            CHECK(!malformed.restoreFromStore());
            CHECK(!malformed.held() && !malformed.paneTerminal(paneID));
            store.record->panes[0].history = original;
        }

        ClientAttachment restored(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        REQUIRE(restored.restoreFromStore());
        REQUIRE(restored.connection() == nullptr);
        REQUIRE(restored.paneTerminal(paneID));
        CHECK_EQ(Ubiquitty::Screen::scrollbackCount(restored.paneTerminal(paneID)), persistence == HistoryPersistence::Enabled ? rows : 0);
        reconnect(host, restored, now);
        REQUIRE(restored.connection() && restored.connection()->welcome());
        REQUIRE(!restored.connection()->welcome()->resumed.empty());
        CHECK(restored.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
        CHECK_EQ(Ubiquitty::Screen::scrollbackCount(restored.paneTerminal(paneID)), rows);
        if (persistence == HistoryPersistence::Enabled) {
            CHECK_EQ(host.connection->counters().historyLinesSent, 0u);
            CHECK_EQ(host.connection->counters().historyBytesSent, 0u);
        } else {
            CHECK(host.connection->counters().historyLinesSent > 0);
        }
    }
}

TEST(coldHistoryHintsSuppressOnlyHeldLinesAndNotNewAppends)
{
    for (const bool hasGap : { false, true }) {
        Host host({ 41, 42 });
        ObservedStore store;
        store.persistence = HistoryPersistence::Enabled;
        const auto policy = UbiquittyTest::syncClientPolicy;
        ClientAttachment client(policy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        std::uint64_t now = 1;
        attachFresh(host, client, now);
        std::string text;
        for (int line = 0; line < 80; ++line)
            text += "cached-" + std::to_string(line) + "\r\n";
        hostWrites(host, text, now);
        pumpFor(host, client, now, 200'000);
        drop(host, client, now);
        REQUIRE(store.record && store.record->panes.size() == 1);
        HistoryCache saved(store.record->hostInstanceID, paneID, policy.historyCacheLines);
        REQUIRE(saved.restoreSnapshot(store.record->panes[0].history, policy.maxCheckpointBytes, policy.maxCheckpointBytes));
        REQUIRE(saved.line(5));
        if (hasGap) {
            HistoryCache sparse(store.record->hostInstanceID, paneID, policy.historyCacheLines);
            REQUIRE(sparse.reset({ .newEpoch = saved.key().epoch, .clearedThrough = 0, .historyHead = saved.head() }) == HistoryPurgeOutcome::Purged);
            for (std::uint64_t number = 0; number < saved.head(); ++number) {
                if (const auto* line = saved.line(number); line && number != 5)
                    REQUIRE(sparse.insert(saved.key().epoch, { line, 1 }) == CacheInsert::Stored);
            }
            const auto snapshot = sparse.snapshot(policy.maxCheckpointBytes, policy.maxCheckpointBytes);
            REQUIRE(snapshot);
            store.record->panes[0].history = snapshot->bytes;
        }
        const auto revision = host.pane.source->history().revision();
        hostWrites(host, "new line while disconnected\r\n", now);
        REQUIRE(host.pane.source->historyAddress().head == saved.head() + 1);
        REQUIRE(host.pane.source->history().revision() == revision);

        ClientAttachment restored(policy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        REQUIRE(restored.restoreFromStore());
        reconnect(host, restored, now);
        REQUIRE(restored.connection() && restored.connection()->welcome());
        REQUIRE(restored.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
        CHECK_EQ(host.connection->counters().historyLinesSent, hasGap ? 2u : 1u);
        const HistoryCache* received = restored.connection()->historyCache(1);
        REQUIRE(received && received->head() == saved.head() + 1);
        for (std::uint64_t number = 0; number < received->head(); ++number) {
            const DecodedLine* line = received->line(number);
            REQUIRE(line);
            CHECK(line->lineHash == logicalLineHash(host.pane.terminal, number));
        }
    }
}

TEST(coldHistoryHintsCannotHideRewrittenOrClearedLines)
{
    for (const bool shouldClearHistory : { false, true }) {
        Host host({ 41, 42 });
        ObservedStore store;
        store.persistence = HistoryPersistence::Enabled;
        const auto policy = UbiquittyTest::syncClientPolicy;
        ClientAttachment client(policy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        std::uint64_t now = 1;
        attachFresh(host, client, now);
        std::string text;
        for (int line = 0; line < 100; ++line)
            text += "before-" + std::to_string(line) + "\r\n";
        hostWrites(host, text, now);
        pumpFor(host, client, now, 200'000);
        const auto before = host.pane.source->historyAddress();
        const auto revision = host.pane.source->history().revision();
        drop(host, client, now);
        if (shouldClearHistory) {
            hostWrites(host, "\x1b[3J", now);
            REQUIRE(host.pane.source->historyAddress().epoch != before.epoch);
        } else {
            Ubiquitty::resize(host.pane.terminal, 26, 80);
            host.pane.source->publish(FrameClass::Settled, now);
            REQUIRE(host.pane.source->historyAddress().head == before.head - 2);
            hostWrites(host, "\x1b[1;1Hedited\x1b[26;1H", now);
            Ubiquitty::resize(host.pane.terminal, 24, 80);
            host.pane.source->publish(FrameClass::Settled, now);
            REQUIRE(host.pane.source->historyAddress().head == before.head);
            REQUIRE(host.pane.source->history().revision() > revision);
        }
        ClientAttachment restored(policy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        REQUIRE(restored.restoreFromStore());
        reconnect(host, restored, now);
        REQUIRE(restored.connection() && restored.connection()->welcome());
        REQUIRE(restored.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
        const HistoryCache* received = restored.connection()->historyCache(1);
        REQUIRE(received);
        if (shouldClearHistory) {
            CHECK_EQ(host.connection->counters().historyResetsSent, 1u);
            CHECK_EQ(received->key().epoch, host.pane.source->historyAddress().epoch);
            CHECK(!received->line(before.head - 2));
            CHECK_EQ(Ubiquitty::Screen::scrollbackCount(restored.paneTerminal(paneID)), 0);
        } else {
            CHECK(host.connection->counters().historyLinesSent > 0);
            const DecodedLine* line = received->line(before.head - 2);
            REQUIRE(line);
            CHECK(line->lineHash == logicalLineHash(host.pane.terminal, before.head - 2));
            // The accepted new publication becomes the next retained
            // baseline: its now-current cache needs no second resend.
            drop(host, restored, now);
            reconnect(host, restored, now);
            CHECK_EQ(host.connection->counters().historyLinesSent, 0u);
            CHECK_EQ(host.connection->counters().historyBytesSent, 0u);
        }
    }
}

TEST(anEqualScreenDigestFromAnotherGenerationCannotVouchForCachedHistory)
{
    Host host({ 41, 42 });
    ObservedStore store;
    store.persistence = HistoryPersistence::Enabled;
    const auto policy = UbiquittyTest::syncClientPolicy;
    ClientAttachment client(policy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    std::string text;
    for (int line = 0; line < 100; ++line)
        text += "before-" + std::to_string(line) + "\r\n";
    hostWrites(host, text, now);
    pumpFor(host, client, now, 200'000);
    client.persist();
    REQUIRE(store.record && store.record->panes.size() == 1);
    const ResumeRecord oldRecord = *store.record;
    const auto head = host.pane.source->historyAddress().head;
    Ubiquitty::resize(host.pane.terminal, 26, 80);
    host.pane.source->publish(FrameClass::Settled, now);
    hostWrites(host, "\x1b[1;1Hedited\x1b[26;1H", now);
    Ubiquitty::resize(host.pane.terminal, 24, 80);
    host.pane.source->publish(FrameClass::Settled, now);
    pumpFor(host, client, now, 200'000);
    drop(host, client, now);
    REQUIRE(store.record && store.record->panes.size() == 1);
    REQUIRE(store.record->panes[0].replica.generation > oldRecord.panes[0].replica.generation);
    REQUIRE(replicaDigest(store.record->panes[0].replica) == replicaDigest(oldRecord.panes[0].replica));
    // A process died before its newest record committed. The old picture
    // still matches, but its cached offscreen line has since been rewritten.
    store.record = oldRecord;
    ClientAttachment restored(policy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    REQUIRE(restored.restoreFromStore());
    reconnect(host, restored, now);
    REQUIRE(restored.connection() && restored.connection()->welcome());
    REQUIRE(restored.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK(host.connection->counters().historyLinesSent > 0);
    const HistoryCache* received = restored.connection()->historyCache(1);
    REQUIRE(received);
    const DecodedLine* line = received->line(head - 2);
    REQUIRE(line);
    CHECK(line->lineHash == logicalLineHash(host.pane.terminal, head - 2));
}

TEST(aLostAckKeepsTheExactHistoryPublicationAndUnknownMetadataResends)
{
    for (const bool shouldForgetMetadata : { false, true }) {
        Host host({ 41, 42 });
        ObservedStore store;
        store.persistence = HistoryPersistence::Enabled;
        const auto policy = UbiquittyTest::syncClientPolicy;
        ClientAttachment client(policy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        std::uint64_t now = 1;
        attachFresh(host, client, now);
        std::string text;
        for (int line = 0; line < 80; ++line)
            text += "retained-" + std::to_string(line) + "\r\n";
        hostWrites(host, text, now);
        pumpFor(host, client, now, 200'000);
        host.pane.source->republish(now);
        pumpFor(host, client, now, 50'000, { .shouldCarryDown = true, .shouldCarryUp = false });
        REQUIRE(host.connection->attachment());
        REQUIRE(host.connection->attachment()->panes.size() == 1);
        PaneBaselines* held = host.connection->attachment()->panes[0].get();
        REQUIRE(held && held->paneID == paneID);
        REQUIRE(held && held->acknowledgedGeneration && !held->inFlight.empty());
        REQUIRE(held->inFlight.back().generation > *held->acknowledgedGeneration);
        REQUIRE(baselineDigest(held->inFlight.back().baseline) == baselineDigest(held->acknowledgedBaseline));
        drop(host, client, now);
        if (shouldForgetMetadata) {
            held->acknowledgedHistory = {};
            for (SentGeneration& sent : held->inFlight)
                sent.history = {};
        }
        ClientAttachment restored(policy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        REQUIRE(restored.restoreFromStore());
        reconnect(host, restored, now);
        REQUIRE(restored.connection() && restored.connection()->welcome());
        REQUIRE(restored.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
        if (shouldForgetMetadata)
            CHECK(host.connection->counters().historyLinesSent > 0);
        else {
            CHECK_EQ(host.connection->counters().historyLinesSent, 0u);
            CHECK_EQ(host.connection->counters().historyBytesSent, 0u);
        }
    }
}

TEST(fragmentedColdHistoryHintsStayWithinTheWholeGreetingLimit)
{
    Host host({ 41, 42 });
    ObservedStore store;
    store.persistence = HistoryPersistence::Enabled;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    std::string text;
    for (int line = 0; line < 240; ++line)
        text += "fragment-" + std::to_string(line) + "\r\n";
    hostWrites(host, text, now);
    pumpFor(host, client, now, 200'000);
    drop(host, client, now);
    REQUIRE(store.record && store.record->panes.size() == 1);
    const auto policy = UbiquittyTest::syncClientPolicy;
    HistoryCache saved(store.record->hostInstanceID, paneID, policy.historyCacheLines);
    REQUIRE(saved.restoreSnapshot(store.record->panes[0].history, policy.maxCheckpointBytes, policy.maxCheckpointBytes));

    const ID128 secondPane { paneID.low + 1, paneID.high };
    store.record->panes.push_back(store.record->panes[0]);
    store.record->panes[1].paneID = secondPane;
    std::vector<LineRange> ranges;
    for (auto& pane : store.record->panes) {
        HistoryCache sparse(store.record->hostInstanceID, pane.paneID, policy.historyCacheLines);
        REQUIRE(sparse.reset({ .newEpoch = saved.key().epoch, .clearedThrough = 0, .historyHead = saved.head() }) == HistoryPurgeOutcome::Purged);
        for (std::uint64_t number = 0; number < saved.head(); number += 2) {
            if (const auto* line = saved.line(number))
                REQUIRE(sparse.insert(saved.key().epoch, { line, 1 }) == CacheInsert::Stored);
        }
        ranges = sparse.cachedRanges();
        REQUIRE(ranges.size() > maxResumeCachedRanges);
        const auto snapshot = sparse.snapshot(policy.maxCheckpointBytes, policy.maxCheckpointBytes);
        REQUIRE(snapshot);
        pane.history = snapshot->bytes;
    }

    ClientAttachment restored(policy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    REQUIRE(restored.restoreFromStore());
    (void)restored.connect(now);
    const auto output = restored.takeStreamOutput();
    FrameDecoder decoder;
    decoder.append(output);
    const auto frame = decoder.next();
    REQUIRE(frame && frame->kind == FrameKind::Greet);
    const auto greet = decodeGreet(frame->body);
    REQUIRE(greet && greet->resume && greet->resume->panes.size() == 2);
    const auto& first = greet->resume->panes[0].cachedRanges;
    REQUIRE(first.size() == maxResumeCachedRanges);
    CHECK(first.front() == ranges[ranges.size() - maxResumeCachedRanges]);
    CHECK(first.back() == ranges.back());
    CHECK(greet->resume->panes[1].cachedRanges.empty());
    // Sixty-four hints can split the implicit backfill into sixty-five
    // gaps. The host preserves its range bound by conservatively resending.
    host.accept();
    host.connection->receiveStream(output, now);
    CHECK(host.connection->wantedHistory(1, HistoryPriority::Backfill).size() <= UbiquittyTest::syncHostPolicy(0).limits.maxPendingRanges);
    pumpFor(host, restored, now, 200'000);
    REQUIRE(restored.connection() && restored.connection()->welcome());
    CHECK(host.connection->counters().historyLinesSent > 0);
    const HistoryCache* filled = restored.connection()->historyCache(1);
    REQUIRE(filled);
    CHECK_EQ(filled->contiguousHead(), saved.head());
}

// A Welcome consumes the stored secret. The record holds the fresh one
// before the input that waited for the connection is sent on it.
TEST(theRecordIsRewrittenBeforeAnyInputOnTheNewConnection)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(client.send(paneID, raw("a"), now).has_value());
    pumpFor(host, client, now, 50'000);
    drop(host, client, now);
    REQUIRE(store.record.has_value());
    const ID128 consumed = store.record->resumeSecret;
    REQUIRE(client.send(paneID, raw("b"), now).has_value());

    // What waited when the record first held the fresh secret.
    std::optional<std::size_t> waitingAtRewrite;
    store.onWrite = [&](const ResumeRecord& written) {
        if (written.resumeSecret != consumed && !waitingAtRewrite)
            waitingAtRewrite = client.inputStatus(paneID).waiting;
    };
    reconnect(host, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    REQUIRE(waitingAtRewrite.has_value());
    CHECK_EQ(*waitingAtRewrite, std::size_t { 1 });
    CHECK(store.record->resumeSecret == client.held()->resumeSecret);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "ab");
}

// After a handover the record names the successor and its fresh secret
// before any input leaves on the successor's connection, so a client killed
// at that moment resumes on the successor, not on the host that left.
TEST(aHandedOverWelcomeRewritesTheRecordUnderTheSuccessorBeforeAnyInput)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(client.send(paneID, raw("a"), now).has_value());
    pumpFor(host, client, now, 50'000);
    REQUIRE(store.record.has_value());
    CHECK((store.record->hostInstanceID == ID128 { 41, 42 }));
    const ID128 consumed = store.record->resumeSecret;
    sayHandingOver(host, client, now);
    REQUIRE(client.send(paneID, raw("b"), now).has_value());
    auto successor = handOver(host, { 51, 52 }, now);
    REQUIRE(successor);

    std::optional<std::size_t> waitingAtRewrite;
    std::optional<ID128> rewrittenHost;
    store.onWrite = [&](const ResumeRecord& written) {
        if (written.resumeSecret != consumed && !waitingAtRewrite) {
            waitingAtRewrite = client.inputStatus(paneID).waiting;
            rewrittenHost = written.hostInstanceID;
        }
    };
    reconnect(*successor, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    REQUIRE(waitingAtRewrite.has_value());
    CHECK_EQ(*waitingAtRewrite, std::size_t { 1 });
    CHECK((rewrittenHost == std::optional { ID128 { 51, 52 } }));
    REQUIRE(store.record.has_value());
    CHECK((store.record->hostInstanceID == ID128 { 51, 52 }));
    CHECK(store.record->resumeSecret == client.held()->resumeSecret);
    CHECK(store.record->resumeSecret != consumed);
    CHECK_STR_EQ(host.pane.writer.bytes(), std::string("a"));
    CHECK_STR_EQ(successor->writer.bytes(), std::string("b"));
}

TEST(aRefusedFreshSecretWriteHoldsInputUntilAnExplicitSuccessfulRetry)
{
    Host host({ 41, 42 });
    ObservedStore store;
    store.isRefusingWrites = true;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    REQUIRE(!store.record);
    REQUIRE(client.send(paneID, raw("a"), now).has_value());
    pumpFor(host, client, now, 50'000);
    CHECK(host.pane.writer.bytes().empty());
    CHECK_EQ(client.inputStatus(paneID).waiting, 1u);
    const int writes = store.writes;
    store.isRefusingWrites = false;
    pumpFor(host, client, now, 50'000);
    CHECK_EQ(store.writes, writes);
    CHECK(host.pane.writer.bytes().empty());

    client.persist();
    REQUIRE(store.record);
    CHECK(store.record->resumeSecret == client.held()->resumeSecret);
    pumpFor(host, client, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "a");
    CHECK_EQ(client.inputStatus(paneID).waiting, 0u);
}

TEST(aRefusedRotatedSecretCannotUseThePreviousConnectionsCommit)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(client.send(paneID, raw("a"), now).has_value());
    pumpFor(host, client, now, 50'000);
    drop(host, client, now);
    REQUIRE(store.record);
    const ID128 consumed = store.record->resumeSecret;
    REQUIRE(client.send(paneID, raw("b"), now).has_value());
    store.isRefusingWrites = true;
    reconnect(host, client, now);
    REQUIRE(client.state() == ClientAttachmentState::Live);
    REQUIRE(client.held()->resumeSecret != consumed);
    CHECK(!store.record);
    REQUIRE(client.send(paneID, raw("c"), now).has_value());
    client.persist();
    pumpFor(host, client, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "a");
    CHECK_EQ(client.inputStatus(paneID).waiting, 2u);

    store.isRefusingWrites = false;
    client.persist();
    REQUIRE(store.record);
    CHECK(store.record->resumeSecret == client.held()->resumeSecret);
    pumpFor(host, client, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "abc");
}

TEST(inputWaitingForStorageUsesTheLostQueueBound)
{
    Host host({ 41, 42 });
    ObservedStore store;
    store.isRefusingWrites = true;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    constexpr std::size_t chunkBytes = 4'096;
    static_assert(maxLostInputBytes % chunkBytes == 0);
    for (std::size_t offset = 0; offset < maxLostInputBytes; offset += chunkBytes)
        REQUIRE(client.send(paneID, raw(std::string(chunkBytes, 'x')), now).has_value());
    const auto overflow = client.send(paneID, raw("y"), now);
    REQUIRE(!overflow.has_value());
    CHECK(overflow.error() == InputRefusal::LostQueueFull);
    pumpFor(host, client, now, 50'000);
    CHECK(host.pane.writer.bytes().empty());
    CHECK_EQ(client.inputStatus(paneID).waiting, maxLostInputBytes / chunkBytes);
}

TEST(aFailedRecordPurgeMustSucceedBeforeAnotherWriteCanReleaseInput)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(store.record);
    store.isRefusingWrites = true;
    store.isRefusingPurges = true;
    client.persist();
    REQUIRE(store.purges > 0);
    REQUIRE(store.record);
    const int writes = store.writes;
    const int purges = store.purges;
    store.isRefusingWrites = false;
    REQUIRE(client.send(paneID, raw("x"), now).has_value());
    client.persist();
    CHECK_EQ(store.writes, writes);
    CHECK(store.purges > purges);
    pumpFor(host, client, now, 50'000);
    CHECK(host.pane.writer.bytes().empty());
    CHECK_EQ(client.inputStatus(paneID).waiting, 1u);

    store.isRefusingPurges = false;
    client.persist();
    REQUIRE(store.record);
    CHECK(store.record->resumeSecret == client.held()->resumeSecret);
    pumpFor(host, client, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "x");
}

enum class HistoryRetirementCarrier {
    Reset,
    Checkpoint
};

static void checkRefusedHistoryRetirement(HistoryRetirementCarrier carrier)
{
    Host host({ 41, 42 });
    ObservedStore store;
    store.persistence = HistoryPersistence::Enabled;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    // Establish a known input receipt before the reconnect; otherwise an
    // unsent event can correctly remain in doubt after a fresh attachment.
    REQUIRE(client.send(paneID, raw("a"), now).has_value());
    pumpFor(host, client, now, 50'000);
    REQUIRE(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "a");
    std::string oldHistory;
    for (int line = 0; line < 40; ++line)
        oldHistory += "retired history\r\n";
    hostWrites(host, oldHistory, now);
    pumpFor(host, client, now, 50'000);
    client.persist();
    REQUIRE(store.record);
    REQUIRE(recordContains(*store.record, "retired history"));
    REQUIRE(!store.record->panes[0].history.empty());
    const auto savedHistory = store.record->panes[0].history;
    const auto handle = client.connection()->handleForPane(paneID);
    REQUIRE(client.connection()->historyCache(handle));
    const auto oldKey = client.connection()->historyCache(handle)->key();
    const auto oldRanges = client.connection()->historyCache(handle)->cachedRanges();
    const auto oldPicture = pictureOf(*client.paneTerminal(paneID));
    const auto oldGeneration = client.connection()->paneGeneration(handle);
    const auto oldCounters = client.connection()->counters();
    const int writes = store.writes;
    const int purges = store.purges;
    store.isRefusingPurges = true;
    store.onPurge = [&] {
        CHECK(client.connection()->historyCache(handle)->key() == oldKey);
        CHECK(pictureOf(*client.paneTerminal(paneID)) == oldPicture);
    };
    hostWrites(host, "\x1b"
                     "c",
        now);
    if (carrier == HistoryRetirementCarrier::Checkpoint) {
        std::vector<std::uint8_t> resync;
        encode(resync, Resync { .reason = ResyncReason::BaseMissing, .generation = oldGeneration, .sectionHashes = {} });
        std::vector<std::uint8_t> framed;
        REQUIRE(appendFrame(framed, FrameKind::Resync, handle, resync));
        host.connection->receiveStream(framed, now);
    }
    host.connection->service(now);
    std::vector<std::uint8_t> down;
    while (host.connection->popStreamFrame(down, now)) {
    }
    FrameDecoder decoder;
    decoder.append(down);
    std::vector<std::uint8_t> delivered;
    bool hasCheckpoint = false;
    bool hasReset = false;
    while (const auto frame = decoder.next()) {
        hasCheckpoint |= frame->kind == FrameKind::CheckpointHead;
        hasReset |= frame->kind == FrameKind::HistoryReset;
        if (carrier == HistoryRetirementCarrier::Checkpoint && frame->kind == FrameKind::HistoryReset)
            continue;
        REQUIRE(appendFrame(delivered, frame->kind, frame->channel, frame->body));
    }
    REQUIRE(hasReset);
    if (carrier == HistoryRetirementCarrier::Checkpoint)
        REQUIRE(hasCheckpoint);
    client.receiveStream(delivered, now);
    REQUIRE(store.purges > purges);
    CHECK_EQ(store.writes, writes);
    CHECK(client.connection()->state() == ConnectionState::Closed);
    CHECK(client.state() == ClientAttachmentState::Lost);
    CHECK(client.connection()->historyCache(handle)->key() == oldKey);
    CHECK(client.connection()->historyCache(handle)->cachedRanges() == oldRanges);
    CHECK(pictureOf(*client.paneTerminal(paneID)) == oldPicture);
    CHECK_EQ(client.connection()->paneGeneration(handle), oldGeneration);
    CHECK_EQ(client.connection()->counters().historyResetsApplied, oldCounters.historyResetsApplied);
    CHECK_EQ(client.connection()->counters().checkpointsApplied, oldCounters.checkpointsApplied);
    CHECK_EQ(client.connection()->counters().acksSent, oldCounters.acksSent);
    REQUIRE(store.record);
    CHECK(recordContains(*store.record, "retired history"));
    CHECK(store.record->panes[0].history == savedHistory);
    const int reads = store.reads;
    CHECK(!client.restoreFromStore());
    CHECK_EQ(store.reads, reads);
    store.onPurge = {};
    REQUIRE(client.send(paneID, raw("x"), now).has_value());
    pumpFor(host, client, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "a");

    // Reconnecting does not make the stale on-disk record eligible again.
    const int purgesBeforeResume = store.purges;
    drop(host, client, now);
    reconnect(host, client, now);
    REQUIRE(client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(client.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK(store.purges > purgesBeforeResume);
    CHECK(client.connection()->state() == ConnectionState::Closed);
    CHECK(pictureOf(*client.paneTerminal(paneID)) == oldPicture);
    CHECK_EQ(client.connection()->paneGeneration(client.connection()->handleForPane(paneID)), oldGeneration);
    CHECK_EQ(client.connection()->counters().historyResetsApplied, 0u);
    CHECK_EQ(client.connection()->counters().checkpointsApplied, 0u);
    CHECK_EQ(client.connection()->counters().updatesApplied, 0u);
    CHECK_EQ(store.writes, writes);
    CHECK(!client.restoreFromStore());
    CHECK_EQ(store.reads, reads);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "a");

    store.isRefusingPurges = false;
    client.persist();
    REQUIRE(store.record);
    CHECK(!recordContains(*store.record, "retired history"));
    CHECK(store.record->panes[0].history.empty());
    // Storage retirement does not reopen a refused connection. A reconnect
    // obtains the new epoch/picture before queued input can flow.
    drop(host, client, now);
    reconnect(host, client, now);
    CHECK(client.state() == ClientAttachmentState::Live);
    const auto newHandle = client.connection()->handleForPane(paneID);
    REQUIRE(client.connection()->historyCache(newHandle));
    CHECK_EQ(client.connection()->historyCache(newHandle)->key().epoch, host.pane.source->historyAddress().epoch);
    CHECK(!screenShows(*client.paneTerminal(paneID), "retired history"));
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "ax");
    // A second ordinary diff resume starts its fresh cache in the committed
    // epoch; it must not manufacture epoch1 or purge the same epoch again.
    const auto epoch = client.connection()->historyCache(newHandle)->key().epoch;
    const int successfulPurges = store.purges;
    drop(host, client, now);
    reconnect(host, client, now);
    CHECK_EQ(store.purges, successfulPurges);
    REQUIRE(client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(client.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(client.connection()->historyCache(client.connection()->handleForPane(paneID))->key().epoch, epoch);
}

TEST(aFailedHistoryPurgeCannotBeHiddenByASuccessfulReplacementWrite)
{
    checkRefusedHistoryRetirement(HistoryRetirementCarrier::Reset);
}

TEST(aCheckpointCannotCommitANewEpochBeforeTheOldRecordIsRetired)
{
    checkRefusedHistoryRetirement(HistoryRetirementCarrier::Checkpoint);
}

TEST(anUncertainReplacementHoldsInputAndRetirementAcrossReconnect)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(client.send(paneID, raw("a"), now).has_value());
    pumpFor(host, client, now, 50'000);
    REQUIRE(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "a");
    drop(host, client, now);
    REQUIRE(store.record);
    const ID128 previousSecret = store.record->resumeSecret;
    store.isUncertainWrite = true;
    store.isRefusingPurges = true;
    reconnect(host, client, now);
    REQUIRE(store.record);
    REQUIRE(store.record->resumeSecret != previousSecret);
    REQUIRE(store.record->resumeSecret == client.held()->resumeSecret);
    CHECK(store.purges > 0);
    REQUIRE(client.send(paneID, raw("x"), now).has_value());
    pumpFor(host, client, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "a");

    const int writes = store.writes;
    const int reads = store.reads;
    CHECK(!client.restoreFromStore());
    CHECK_EQ(store.reads, reads);
    store.isUncertainWrite = false;
    drop(host, client, now);
    reconnect(host, client, now);
    CHECK_EQ(store.writes, writes);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "a");
    CHECK_EQ(client.inputStatus(paneID).waiting, 1u);

    store.isRefusingPurges = false;
    client.persist();
    REQUIRE(store.record);
    CHECK(store.record->resumeSecret == client.held()->resumeSecret);
    pumpFor(host, client, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "ax");
}

TEST(anUncertainReplacementIsNotACommitEvenWhenItsPurgeSucceeds)
{
    Host host({ 41, 42 });
    ObservedStore store;
    store.isUncertainWrite = true;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(store.purges > 0);
    CHECK(!store.record);
    REQUIRE(client.send(paneID, raw("x"), now).has_value());
    pumpFor(host, client, now, 50'000);
    CHECK(host.pane.writer.bytes().empty());
    CHECK_EQ(client.inputStatus(paneID).waiting, 1u);

    store.isUncertainWrite = false;
    client.persist();
    pumpFor(host, client, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "x");
}

TEST(theMemoryStoreReportsItsCommitAndConfirmedRetirement)
{
    MemoryResumeRecordStore store;
    const ResumeRecord record { .hostInstanceID = { 1, 2 }, .clientInstance = { 3, 4 }, .attachmentID = { 5, 6 }, .resumeSecret = { 7, 8 }, .certificatePin = {}, .panes = {}, .drafts = {} };
    CHECK(store.write(record) == ResumeWriteOutcome::Committed);
    REQUIRE(store.read());
    CHECK(store.read()->resumeSecret == record.resumeSecret);
    CHECK(store.purge() == ResumePurgeOutcome::Purged);
    CHECK(!store.read());
    CHECK(store.purge() == ResumePurgeOutcome::Purged);
}

TEST(aHistoryPurgeWithdrawsTheCommitUntilItsReplacementIsStored)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    std::string oldHistory;
    for (int line = 0; line < 40; ++line)
        oldHistory += "before clear\r\n";
    hostWrites(host, oldHistory, now);
    pumpFor(host, client, now, 50'000);
    client.persist();
    REQUIRE(store.record);
    const int purges = store.purges;
    store.isRefusingWrites = true;
    hostWrites(host, "\x1b"
                     "c",
        now);
    pumpFor(host, client, now, 50'000);
    REQUIRE(store.purges > purges);
    REQUIRE(!store.record);
    REQUIRE(client.send(paneID, raw("x"), now).has_value());
    pumpFor(host, client, now, 50'000);
    CHECK(host.pane.writer.bytes().empty());
    CHECK_EQ(client.inputStatus(paneID).waiting, 1u);

    store.isRefusingWrites = false;
    client.persist();
    REQUIRE(store.record);
    CHECK(!recordContains(*store.record, "before clear"));
    pumpFor(host, client, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "x");
}

TEST(anAttachmentWithoutAStoreStillCarriesInputAcrossResume)
{
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
    std::uint64_t now = 1;
    REQUIRE(client.send(paneID, raw("a"), now).has_value());
    attachFresh(host, client, now);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "a");
    drop(host, client, now);
    REQUIRE(client.send(paneID, raw("b"), now).has_value());
    reconnect(host, client, now);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "ab");
}

// A process killed and launched again draws the picture it persisted before
// it connects, greets as the record's client, and continues the attachment
// from that picture: no checkpoint, and no input written twice or lost.
TEST(aKillAndRelaunchDrawsThePersistedPictureFirstAndResumesDiffed)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        // A pen the host defined and the screen no longer shows: the host
        // numbers the pens after it, and a picture numbered from the screen
        // alone would not be the one the host can diff from.
        hostWrites(host, "\x1b[32mgone\x1b[0m\r\x1b[2K", now);
        pumpFor(host, first, now, 100'000);
        hostWrites(host, rich, now);
        pumpFor(host, first, now, 100'000);
        REQUIRE(first.send(paneID, raw("x"), now).has_value());
        pumpFor(host, first, now, 50'000);
        first.persist();
        host.lose(now);
    }
    REQUIRE(store.record.has_value());

    ClientIdentity relaunched = clientIdentity();
    relaunched.clientInstance = { 99, 99 };
    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), relaunched, nullptr, &store);
    REQUIRE(second.restoreFromStore());
    CHECK(second.clientInstance() == clientInstance);
    // The picture is drawn before any connection, and the client says it has
    // none: a frontend labels it reconnecting.
    CHECK(second.state() == ClientAttachmentState::Lost);
    const Ubiquitty::Terminal* drawn = second.paneTerminal(paneID);
    REQUIRE(drawn != nullptr);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(host.pane.terminal, *drawn), "");

    hostWrites(host, "while the process was gone\r\n", now);
    reconnect(host, second, now);
    REQUIRE(second.state() == ClientAttachmentState::Live);
    REQUIRE(second.connection()->welcome().has_value());
    REQUIRE(second.connection()->welcome()->resumed.size() == 1);
    CHECK(second.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(host.connection->counters().checkpointsSent, 0u);
    CHECK_EQ(second.connection()->counters().checkpointsApplied, 0u);
    CHECK(second.paneTerminal(paneID) == drawn);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(host.pane.terminal, *drawn), "");

    REQUIRE(second.send(paneID, raw("y"), now).has_value());
    pumpFor(host, second, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "xy");
    hostWrites(host, "and after\r\n", now);
    pumpFor(host, second, now, 100'000);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(host.pane.terminal, *drawn), "");
    CHECK_EQ(second.connection()->counters().checkpointsApplied, 0u);
}

// A hidden pane's status set stands in its picture, while what is known of the
// picture is the screen the host diffs from. The two carry across a dropped
// connection together: resumed hidden and then shown, or shown while no
// connection carried it, the pane holds the status of the screen the host
// goes on from, so a host whose status went back to it while the pane was
// hidden is shown as it is, with no checkpoint.
TEST(aStatusSetStandingInAHiddenPictureGivesWayToTheScreenAcrossAResume)
{
    for (const bool isShownWhileAway : { false, true }) {
        Host host({ 41, 42 });
        ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity());
        std::uint64_t now = 1;
        attachFresh(host, client, now);
        hostPosts(host, postsWorking, now);
        pumpFor(host, client, now, 100'000);
        const Ubiquitty::Terminal* picture = client.paneTerminal(paneID);
        REQUIRE(picture != nullptr);
        CHECK(shownState(*picture) == Ubiquitty::OSC::ProgramState::Working);
        client.setView(paneID, ViewState::Dormant);
        pumpFor(host, client, now, 50'000);
        hostPosts(host, postsBlocked, now);
        pumpFor(host, client, now, 100'000);
        CHECK(shownState(*picture) == Ubiquitty::OSC::ProgramState::Blocked);

        drop(host, client, now);
        if (isShownWhileAway)
            client.setView(paneID, ViewState::Live);
        hostPosts(host, postsWorking, now);
        reconnect(host, client, now);
        REQUIRE(client.connection() && client.connection()->welcome());
        REQUIRE(client.connection()->welcome()->resumed.size() == 1);
        CHECK(client.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
        CHECK(client.paneTerminal(paneID) == picture);
        client.setView(paneID, ViewState::Live);
        pumpFor(host, client, now, 200'000);
        CHECK_MSG(shownState(*picture) == Ubiquitty::OSC::ProgramState::Working, isShownWhileAway ? "shown while away" : "resumed hidden, then shown");
        CHECK(picture->programStatus == host.pane.terminal.programStatus);
        CHECK_EQ(client.connection()->counters().checkpointsApplied, 0u);
    }
}

// The same for a picture drawn into the frontend's own terminal, which a
// resume reads to see that it still is what the replica describes: read as
// the screen left it, the status set standing in it aside, it still is, and
// the host continues it with a diff.
TEST(aStatusSetStandingInALentPictureLeavesItResumable)
{
    struct Frontend final : ClientPaneDelegate {
        Ubiquitty::Terminal terminal;
        Ubiquitty::Terminal* terminalForPane(ID128 id) override { return id == paneID ? &terminal : nullptr; }
    } frontend;
    Host host({ 41, 42 });
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &frontend);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    REQUIRE(client.paneTerminal(paneID) == &frontend.terminal);
    hostPosts(host, postsWorking, now);
    pumpFor(host, client, now, 100'000);
    client.setView(paneID, ViewState::Dormant);
    pumpFor(host, client, now, 50'000);
    hostPosts(host, postsBlocked, now);
    pumpFor(host, client, now, 100'000);
    CHECK(shownState(frontend.terminal) == Ubiquitty::OSC::ProgramState::Blocked);

    drop(host, client, now);
    hostPosts(host, postsWorking, now);
    reconnect(host, client, now);
    REQUIRE(client.connection() && client.connection()->welcome());
    REQUIRE(client.connection()->welcome()->resumed.size() == 1);
    CHECK(client.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    client.setView(paneID, ViewState::Live);
    pumpFor(host, client, now, 200'000);
    CHECK(frontend.terminal.programStatus == host.pane.terminal.programStatus);
    CHECK_EQ(client.connection()->counters().checkpointsApplied, 0u);
}

// A record written while a status set stands in a hidden pane's picture keeps
// the screen the replica describes, so a relaunch that resumes from it is
// shown the status the host goes on from.
TEST(aRecordKeepsTheScreensStatusNotTheSetStandingInIt)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        hostPosts(host, postsWorking, now);
        pumpFor(host, first, now, 100'000);
        first.setView(paneID, ViewState::Dormant);
        pumpFor(host, first, now, 50'000);
        hostPosts(host, postsBlocked, now);
        pumpFor(host, first, now, 100'000);
        REQUIRE(first.paneTerminal(paneID) != nullptr);
        CHECK(shownState(*first.paneTerminal(paneID)) == Ubiquitty::OSC::ProgramState::Blocked);
        first.persist();
        // The set still stands once the record is written.
        CHECK(shownState(*first.paneTerminal(paneID)) == Ubiquitty::OSC::ProgramState::Blocked);
        host.lose(now);
    }
    REQUIRE(store.record.has_value());
    hostPosts(host, postsWorking, now);

    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    REQUIRE(second.restoreFromStore());
    const Ubiquitty::Terminal* drawn = second.paneTerminal(paneID);
    REQUIRE(drawn != nullptr);
    CHECK(shownState(*drawn) == Ubiquitty::OSC::ProgramState::Working);
    reconnect(host, second, now);
    REQUIRE(second.connection() && second.connection()->welcome());
    REQUIRE(second.connection()->welcome()->resumed.size() == 1);
    CHECK(second.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    second.setView(paneID, ViewState::Live);
    pumpFor(host, second, now, 200'000);
    CHECK(drawn->programStatus == host.pane.terminal.programStatus);
    CHECK_EQ(second.connection()->counters().checkpointsApplied, 0u);
}

TEST(aColdPictureUsesTheFrontendsExactTerminalBeforeTheFirstPacket)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        hostWrites(host, "\x1b[32mgone\x1b[0m\r\x1b[2K", now);
        pumpFor(host, first, now, 100'000);
        hostWrites(host, rich, now);
        pumpFor(host, first, now, 100'000);
        first.persist();
        host.lose(now);
    }
    struct Frontend final : ClientPaneDelegate {
        Ubiquitty::Terminal terminal;
        Ubiquitty::Terminal* terminalForPane(ID128 id) override { return id == paneID ? &terminal : nullptr; }
    } frontend;
    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &frontend, &store);
    REQUIRE(second.restoreFromStore());
    CHECK(second.paneTerminal(paneID) == &frontend.terminal);
    CHECK(second.connection() == nullptr);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(host.pane.terminal, frontend.terminal), "");
    reconnect(host, second, now);
    REQUIRE(second.connection()->welcome());
    REQUIRE(second.connection()->welcome()->resumed.size() == 1);
    CHECK(second.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(second.connection()->counters().checkpointsApplied, 0u);
    CHECK(second.paneTerminal(paneID) == &frontend.terminal);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(host.pane.terminal, frontend.terminal), "");
}

// A record is good for one resume: its secret is consumed, and a second
// client presenting a copy of it attaches fresh.
// A platform restores a window's record before the window's terminal exists,
// into a terminal the attachment owns, and names the window's terminal only
// once its session attaches. The picture then moves into that terminal, the
// host still continues with a diff, and what it sends lands where the window
// draws.
TEST(aRestoredPictureMovesIntoTheTerminalTheFrontendNamesLater)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        hostWrites(host, rich, now);
        pumpFor(host, first, now, 100'000);
        first.persist();
        host.lose(now);
    }
    struct Frontend final : ClientPaneDelegate {
        Ubiquitty::Terminal terminal;
        bool isLending = false;
        Ubiquitty::Terminal* terminalForPane(ID128 id) override { return isLending && id == paneID ? &terminal : nullptr; }
    } frontend;
    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &frontend, &store);
    REQUIRE(second.restoreFromStore());
    const Ubiquitty::Terminal* owned = second.paneTerminal(paneID);
    REQUIRE(owned != nullptr);
    CHECK(owned != &frontend.terminal);

    frontend.isLending = true;
    hostWrites(host, "while the window was away\r\n", now);
    reconnect(host, second, now);
    REQUIRE(second.connection()->welcome());
    REQUIRE(second.connection()->welcome()->resumed.size() == 1);
    CHECK(second.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(second.connection()->counters().checkpointsApplied, 0u);
    CHECK(second.paneTerminal(paneID) == &frontend.terminal);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(host.pane.terminal, frontend.terminal), "");
    hostWrites(host, "and after it came back\r\n", now);
    pumpFor(host, second, now, 100'000);
    CHECK(screenShows(frontend.terminal, "and after it came back"));
    CHECK_EQ(second.connection()->counters().checkpointsApplied, 0u);
}

// The same move when the window's terminal is named only by the time the
// host binds the pane, after the greeting already named the restored picture.
TEST(aRestoredPictureMovesIntoTheTerminalTheFrontendNamesAtTheBind)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        hostWrites(host, rich, now);
        pumpFor(host, first, now, 100'000);
        first.persist();
        host.lose(now);
    }
    struct Frontend final : ClientPaneDelegate {
        Ubiquitty::Terminal terminal;
        bool isLending = false;
        Ubiquitty::Terminal* terminalForPane(ID128 id) override { return isLending && id == paneID ? &terminal : nullptr; }
    } frontend;
    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &frontend, &store);
    REQUIRE(second.restoreFromStore());
    host.accept();
    (void)second.connect(now);
    frontend.isLending = true;
    pumpFor(host, second, now, 200'000);
    REQUIRE(second.connection()->welcome());
    REQUIRE(second.connection()->welcome()->resumed.size() == 1);
    CHECK(second.connection()->welcome()->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK_EQ(second.connection()->counters().checkpointsApplied, 0u);
    CHECK(second.paneTerminal(paneID) == &frontend.terminal);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(host.pane.terminal, frontend.terminal), "");
    hostWrites(host, "after the bind\r\n", now);
    pumpFor(host, second, now, 100'000);
    CHECK(screenShows(frontend.terminal, "after the bind"));
}

TEST(aReplayedRecordIsRefusedAfterOneUse)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        first.persist();
        host.lose(now);
    }
    REQUIRE(store.record.has_value());
    ObservedStore copy;
    copy.record = store.record;
    const ID128 attachmentID = store.record->attachmentID;

    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    REQUIRE(second.restoreFromStore());
    reconnect(host, second, now);
    REQUIRE(second.state() == ClientAttachmentState::Live);
    CHECK(second.held()->attachmentID == attachmentID);
    CHECK(!host.connection->resumeRefusal().has_value());
    host.lose(now);

    ClientAttachment replayed(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &copy);
    REQUIRE(replayed.restoreFromStore());
    host.accept();
    (void)replayed.connect(now);
    pump(host, replayed, now);
    pump(host, replayed, now);
    CHECK(host.connection->resumeRefusal().has_value());
    REQUIRE(replayed.held().has_value());
    CHECK(replayed.held()->attachmentID != attachmentID);
}

// A process killed after the Welcome and before the rewrite holds a consumed
// secret. Its next launch is refused the resume and attaches fresh, which
// costs one checkpoint and no more.
TEST(aCrashBetweenWelcomeAndRewriteCostsOneCheckpoint)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        hostWrites(host, rich, now);
        pumpFor(host, first, now, 100'000);
        drop(host, first, now);
        REQUIRE(store.record.has_value());
        // The record written as the transport went holds the picture then.
        CHECK(recordContains(*store.record, "plain"));
        // The resume's Welcome arrives and the process dies before it writes.
        store.isLosingWrites = true;
        reconnect(host, first, now);
        REQUIRE(first.state() == ClientAttachmentState::Live);
        REQUIRE(first.held()->resumeSecret != store.record->resumeSecret);
        host.lose(now);
    }
    store.isLosingWrites = false;

    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    REQUIRE(second.restoreFromStore());
    host.accept();
    (void)second.connect(now);
    pump(host, second, now);
    pump(host, second, now);
    CHECK(host.connection->resumeRefusal().has_value());
    (void)host.connection->bindPane(*host.pane.source);
    pumpFor(host, second, now, 200'000);
    REQUIRE(second.state() == ClientAttachmentState::Live);
    CHECK_EQ(second.connection()->counters().checkpointsApplied, 1u);
    REQUIRE(second.paneTerminal(paneID) != nullptr);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(host.pane.terminal, *second.paneTerminal(paneID)), "");
    // The record holds the secret the fresh attach was given.
    REQUIRE(store.record.has_value());
    CHECK(store.record->resumeSecret == second.held()->resumeSecret);
}

// A stored picture whose body does not verify is not drawn; the rest of the
// record is used, and the host checkpoints the pane.
TEST(aStoredCheckpointWithABadDigestIsNotDrawn)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        hostWrites(host, rich, now);
        pumpFor(host, first, now, 100'000);
        first.persist();
        host.lose(now);
    }
    REQUIRE(store.record.has_value());
    REQUIRE(store.record->panes.size() == 1);
    std::vector<std::uint8_t>& body = store.record->panes[0].checkpoint;
    REQUIRE(body.size() > 64);
    body[body.size() / 2] ^= 0x5a;

    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    REQUIRE(second.restoreFromStore());
    CHECK(second.paneTerminal(paneID) == nullptr);
    reconnect(host, second, now);
    REQUIRE(second.state() == ClientAttachmentState::Live);
    CHECK_EQ(second.connection()->counters().checkpointsApplied, 1u);
    REQUIRE(second.paneTerminal(paneID) != nullptr);
    CHECK_STR_EQ(UbiquittyTest::firstDisagreement(host.pane.terminal, *second.paneTerminal(paneID)), "");
}

// Input typed and not sent when the record was written comes back as a
// draft. It is shown, never sent, and what the person types next is numbered
// after it.
TEST(draftsAreNeverSent)
{
    Host host({ 41, 42 });
    ObservedStore store;
    std::uint64_t now = 1;
    std::uint64_t draftSeq = 0;
    {
        ClientAttachment first(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
        attachFresh(host, first, now);
        drop(host, first, now);
        draftSeq = first.send(paneID, raw("rm -rf build\r"), now).value_or(0);
        REQUIRE(draftSeq != 0);
        first.persist();
    }
    REQUIRE(store.record.has_value());
    REQUIRE(recordContains(*store.record, "rm -rf build"));

    ClientAttachment second(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    REQUIRE(second.restoreFromStore());
    REQUIRE(second.drafts().size() == 1);
    CHECK(second.drafts()[0].paneID == paneID);
    CHECK_EQ(second.pendingCount(paneID), 0u);
    reconnect(host, second, now);
    REQUIRE(second.state() == ClientAttachmentState::Live);
    pumpFor(host, second, now, 100'000);
    CHECK(host.pane.writer.bytes().empty());
    // A draft survives another relaunch until the person dismisses it.
    CHECK(recordContains(*store.record, "rm -rf build"));

    const std::uint64_t next = second.send(paneID, raw("z"), now).value_or(0);
    CHECK(next > draftSeq);
    pumpFor(host, second, now, 50'000);
    CHECK(std::string(host.pane.writer.bytes().begin(), host.pane.writer.bytes().end()) == "z");
    second.discardDrafts();
    CHECK(second.drafts().empty());
    CHECK(!recordContains(*store.record, "rm -rf build"));
}

// A record whose pictures pass the checkpoint cap is not written, and none
// stays behind with a secret a Welcome consumed.
TEST(aRecordOverTheCheckpointLimitIsNotWritten)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    ClientAttachment probe(policy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, probe, now);
    probe.persist();
    REQUIRE(store.record.has_value());
    const std::size_t small = pictureBytes(*store.record);
    REQUIRE(small > 0);
    drop(host, probe, now);

    // A cap the prompt's picture fits and a full screen's does not.
    policy.maxCheckpointBytes = small + 512;
    ObservedStore bounded;
    ClientIdentity other = clientIdentity();
    other.clientInstance = { 70, 80 };
    ClientAttachment client(policy, measuredLivenessPolicy(), other, nullptr, &bounded);
    Host second({ 43, 44 });
    attachFresh(second, client, now);
    client.persist();
    REQUIRE(bounded.record.has_value());
    std::string screen;
    for (int row = 0; row < 23; ++row) {
        for (int column = 0; column < 79; ++column)
            screen.push_back(static_cast<char>('!' + (row * 7 + column * 3) % 90));
        screen += "\r\n";
    }
    hostWrites(second, screen, now);
    pumpFor(second, client, now, 200'000);
    REQUIRE(client.paneTerminal(paneID) != nullptr);
    REQUIRE(UbiquittyTest::firstDisagreement(second.pane.terminal, *client.paneTerminal(paneID)).empty());
    client.persist();
    CHECK(!bounded.record.has_value());
    REQUIRE(client.send(paneID, raw("x"), now).has_value());
    pumpFor(second, client, now, 50'000);
    CHECK(second.pane.writer.bytes().empty());
    CHECK_EQ(client.inputStatus(paneID).waiting, 1u);
}

// A suspending client writes its record before it tells the host: every pane
// it shows goes Dormant, the record is stored while the connection is still
// open, and the Bye is the last thing sent.
TEST(theRecordIsWrittenBeforeTheByeLeaves)
{
    Host host({ 41, 42 });
    ObservedStore store;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    std::uint64_t now = 1;
    attachFresh(host, client, now);
    hostWrites(host, rich, now);
    pumpFor(host, client, now, 100'000);
    (void)client.takeStreamOutput();

    const int writesBefore = store.writes;
    bool wasOpenAtWrite = false;
    store.onWrite = [&](const ResumeRecord&) { wasOpenAtWrite = client.connection() && client.connection()->state() == ConnectionState::Attached; };
    client.suspend();
    CHECK(store.writes > writesBefore);
    CHECK(wasOpenAtWrite);
    REQUIRE(store.record.has_value());
    CHECK(recordContains(*store.record, "plain"));
    CHECK(client.connection()->state() == ConnectionState::Closed);

    std::vector<FrameKind> kinds;
    FrameDecoder decoder;
    decoder.append(client.takeStreamOutput());
    while (const std::optional<Frame> frame = decoder.next())
        kinds.push_back(frame->kind);
    REQUIRE(kinds.size() >= 2);
    CHECK(kinds.back() == FrameKind::Bye);
    CHECK(std::ranges::find(kinds, FrameKind::View) != kinds.end());
    // The view the client set is its own: the record resumes the pane shown.
    REQUIRE(store.record->panes.size() == 1);
    CHECK(store.record->panes[0].view != std::optional { ViewState::Dormant });
}

TEST(coldRestorePreflightsEveryDestinationBeforePublishingAnyPane)
{
    const ID128 secondPane { paneID.low + 1, paneID.high };
    ObservedStore store;
    store.record = ResumeRecord { .hostInstanceID = { 41, 42 }, .clientInstance = { 43, 44 }, .attachmentID = { 45, 46 }, .resumeSecret = { 47, 48 }, .certificatePin = {}, .panes = {}, .drafts = {} };
    for (const auto& [id, rows, columns, text] : {
             std::tuple { paneID, 32, 48, "first stored pane" },
             std::tuple { secondPane, 64, 96, "second stored pane" } }) {
        Ubiquitty::Terminal source;
        Ubiquitty::resize(source, rows, columns);
        UbiquittyTest::feed(source, text);
        ResumeRecordPane stored { .paneID = id, .lastInputSeq = 0, .view = {}, .checkpoint = pictureOf(source), .checkpointDigest = {}, .replica = {} };
        stored.checkpointDigest = digestOf(stored.checkpoint);
        Ubiquitty::Terminal passive;
        REQUIRE(applyCheckpoint(passive, stored.checkpoint, stored.replica) == ApplyOutcome::Applied);
        store.record->panes.push_back(std::move(stored));
    }
    struct Frontend final : ClientPaneDelegate {
        Ubiquitty::Terminal first;
        Ubiquitty::Terminal second;
        bool isAliasing = false;
        int calls = 0;
        Ubiquitty::Terminal* terminalForPane(ID128 id) override
        {
            ++calls;
            return id == paneID || isAliasing ? &first : &second;
        }
    } frontend;
    UbiquittyTest::feed(frontend.first, "first before");
    UbiquittyTest::feed(frontend.second, "second before");
    const auto beforeFirst = pictureOf(frontend.first);
    const auto beforeSecond = pictureOf(frontend.second);
    auto policy = UbiquittyTest::syncClientPolicy;
    policy.maxCheckpointBytes = 2 * 64 * 96 * sizeof(Ubiquitty::Cell);
    ClientAttachment tooLarge(policy, measuredLivenessPolicy(), clientIdentity(), &frontend, &store);
    CHECK(!tooLarge.restoreFromStore());
    CHECK(frontend.calls == 0);
    CHECK(!tooLarge.held());
    CHECK(pictureOf(frontend.first) == beforeFirst);

    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &frontend, &store);
    frontend.isAliasing = true;
    CHECK(!client.restoreFromStore());
    CHECK(pictureOf(frontend.first) == beforeFirst);
    CHECK(!client.held());
    frontend.isAliasing = false;
    const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse(64 * 96 * sizeof(Ubiquitty::Cell));
        CHECK(!client.restoreFromStore());
    }
    REQUIRE(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() > refusals);
    CHECK(pictureOf(frontend.first) == beforeFirst);
    CHECK(pictureOf(frontend.second) == beforeSecond);
    CHECK(!client.held());
    CHECK(client.paneTerminal(paneID) == nullptr);
    CHECK(client.paneTerminal(secondPane) == nullptr);
    REQUIRE(client.restoreFromStore());
    CHECK(client.paneTerminal(paneID) == &frontend.first);
    CHECK(client.paneTerminal(secondPane) == &frontend.second);
    CHECK(screenShows(frontend.first, "first stored pane"));
    CHECK(screenShows(frontend.second, "second stored pane"));
}

TEST(coldRestoreRejectsDuplicatePaneIDsBeforeCreatingOwnedPictures)
{
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "stored picture");
    ResumeRecordPane pane { .paneID = paneID, .lastInputSeq = 0, .view = {}, .checkpoint = pictureOf(source), .checkpointDigest = {}, .replica = {} };
    pane.checkpointDigest = digestOf(pane.checkpoint);
    Ubiquitty::Terminal passive;
    REQUIRE(applyCheckpoint(passive, pane.checkpoint, pane.replica) == ApplyOutcome::Applied);
    ObservedStore store;
    store.record = ResumeRecord { .hostInstanceID = { 41, 42 }, .clientInstance = { 43, 44 }, .attachmentID = { 45, 46 }, .resumeSecret = { 47, 48 }, .certificatePin = {}, .panes = { pane, pane }, .drafts = {} };
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, &store);
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    CHECK(!client.restoreFromStore());
    CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == attempts);
    CHECK(!client.held());
    CHECK(client.paneTerminal(paneID) == nullptr);
    // The same owner remains usable after rejecting the complete invalid record.
    store.record->panes.pop_back();
    REQUIRE(client.restoreFromStore());
    REQUIRE(client.paneTerminal(paneID));
    CHECK(screenShows(*client.paneTerminal(paneID), "stored picture"));
}

TEST(coldRestoreRejectsDuplicatePaneIDsBeforeLendingFrontendPictures)
{
    Ubiquitty::Terminal source;
    UbiquittyTest::feed(source, "stored picture");
    ResumeRecordPane pane { .paneID = paneID, .lastInputSeq = 0, .view = {}, .checkpoint = pictureOf(source), .checkpointDigest = {}, .replica = {} };
    pane.checkpointDigest = digestOf(pane.checkpoint);
    Ubiquitty::Terminal passive;
    REQUIRE(applyCheckpoint(passive, pane.checkpoint, pane.replica) == ApplyOutcome::Applied);
    ObservedStore store;
    store.record = ResumeRecord { .hostInstanceID = { 41, 42 }, .clientInstance = { 43, 44 }, .attachmentID = { 45, 46 }, .resumeSecret = { 47, 48 }, .certificatePin = {}, .panes = { pane, pane }, .drafts = {} };
    struct Frontend final : ClientPaneDelegate {
        Ubiquitty::Terminal terminal;
        int calls = 0;
        Ubiquitty::Terminal* terminalForPane(ID128) override
        {
            ++calls;
            return &terminal;
        }
    } frontend;
    UbiquittyTest::feed(frontend.terminal, "prior frontend picture");
    const auto before = pictureOf(frontend.terminal);
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), &frontend, &store);
    CHECK(!client.restoreFromStore());
    CHECK(frontend.calls == 0);
    CHECK(!client.held());
    CHECK(client.paneTerminal(paneID) == nullptr);
    CHECK(pictureOf(frontend.terminal) == before);
    store.record->panes.pop_back();
    REQUIRE(client.restoreFromStore());
    CHECK(frontend.calls == 1);
    CHECK(client.paneTerminal(paneID) == &frontend.terminal);
    CHECK(screenShows(frontend.terminal, "stored picture"));
}

UBIQUITTY_TEST_MAIN()

namespace {

/// The bytes a client without Core in its process keeps in a file.
class FileBytes final : public Ubiquitty::Session::ResumeRecordSlot {
public:
    std::optional<std::vector<std::uint8_t>> bytes;
    bool write(std::span<const std::uint8_t> value) override
    {
        bytes.emplace(value.begin(), value.end());
        return true;
    }
    std::optional<std::vector<std::uint8_t>> read(std::size_t maxBytes) override { return bytes && bytes->size() <= maxBytes ? bytes : std::nullopt; }
    bool erase() override
    {
        bytes.reset();
        return true;
    }
};

void putLittle(std::vector<std::uint8_t>& out, std::uint64_t value, int width)
{
    for (int index = 0; index < width; ++index)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
}

std::uint64_t takeLittle(std::span<const std::uint8_t> bytes, std::size_t& at, int width)
{
    std::uint64_t value = 0;
    for (int index = 0; index < width && at < bytes.size(); ++index)
        value |= static_cast<std::uint64_t>(bytes[at++]) << (8 * index);
    return value;
}

} // namespace

// An editor speaks the wire itself and keeps its picture as the host's own
// bytes. It hands the host binary a PictureRequest, which Core turns into the
// same record a Core client keeps, and reads back a ResumeReply: the picture
// to draw before any packet and the tail its own Greet ends with. The host
// continues the attachment from that Greet with a diff.
TEST(aClientWithoutCoreStoresTheHostsPictureAndResumesDiffedFromItsOwnGreet)
{
    namespace Session = Ubiquitty::Session;
    Host host({ 41, 42 });
    std::uint64_t now = 1;
    ClientAttachment client(UbiquittyTest::syncClientPolicy, measuredLivenessPolicy(), clientIdentity(), nullptr, nullptr);
    attachFresh(host, client, now);
    hostWrites(host, rich, now);
    pumpFor(host, client, now, 100'000);
    REQUIRE(client.send(paneID, raw("x"), now).has_value());
    pumpFor(host, client, now, 50'000);
    const auto held = client.held();
    REQUIRE(held.has_value());
    REQUIRE(held->panes.size() == 1);
    // What the editor holds is the body the host sent for the generation it took.
    const std::vector<std::uint8_t> picture = UbiquittyTest::checkpointBody(host.pane.terminal, host.pane.source->generation());

    std::vector<std::uint8_t> request;
    for (const ID128 id : { held->hostInstanceID, clientInstance, held->attachmentID, held->resumeSecret, paneID }) {
        putLittle(request, id.low, 8);
        putLittle(request, id.high, 8);
    }
    putLittle(request, held->panes[0].lastInputSeq, 8);
    request.push_back(0xff);
    putLittle(request, 2, 8);
    putLittle(request, 3, 8);
    putLittle(request, picture.size(), 4);
    request.insert(request.end(), picture.begin(), picture.end());
    const std::size_t budget = measuredClientPolicy().maxCheckpointBytes;
    CHECK(!Session::recordFromPictureRequest(std::span(request).first(request.size() - 1), budget));
    auto broken = request;
    broken.back() ^= 0x55;
    CHECK(!Session::recordFromPictureRequest(broken, budget));
    const auto record = Session::recordFromPictureRequest(request, budget);
    REQUIRE(record.has_value());
    FileBytes file;
    Session::EncodedResumeRecordStore store(file, budget);
    REQUIRE(store.write(*record) == ResumeWriteOutcome::Committed);
    client.transportLost(now);
    host.lose(now);

    const auto reply = Session::resumeReplyFromStore(store);
    REQUIRE(reply.has_value());
    std::size_t at = 0;
    const ID128 instance { takeLittle(*reply, at, 8), takeLittle(*reply, at, 8) };
    CHECK(instance == clientInstance);
    const std::size_t tailSize = takeLittle(*reply, at, 4);
    REQUIRE(at + tailSize <= reply->size());
    const std::vector<std::uint8_t> tail(reply->begin() + static_cast<std::ptrdiff_t>(at), reply->begin() + static_cast<std::ptrdiff_t>(at + tailSize));
    at += tailSize;
    REQUIRE(takeLittle(*reply, at, 2) == 1u);
    CHECK((ID128 { takeLittle(*reply, at, 8), takeLittle(*reply, at, 8) } == paneID));
    CHECK_EQ(takeLittle(*reply, at, 8), held->panes[0].lastInputSeq);
    CHECK_EQ(takeLittle(*reply, at, 8), 2u);
    CHECK_EQ(takeLittle(*reply, at, 8), 3u);
    const std::size_t drawnSize = takeLittle(*reply, at, 4);
    REQUIRE(at + drawnSize == reply->size());
    CHECK(std::ranges::equal(std::span(*reply).subspan(at), picture));

    hostWrites(host, "while the editor was gone\r\n", now);
    Greet greet;
    greet.wireVersion = wireVersion;
    greet.clientInstance = clientInstance;
    greet.facts = UbiquittyTest::syncDefaultFacts();
    std::vector<std::uint8_t> body;
    encode(body, greet);
    REQUIRE(!body.empty());
    REQUIRE(body.back() == 0);
    body.pop_back();
    body.insert(body.end(), tail.begin(), tail.end());
    const auto decoded = decodeGreet(body);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->resume.has_value());
    CHECK(decoded->resume->attachmentID == held->attachmentID);
    CHECK(decoded->resume->resumeSecret == std::optional { held->resumeSecret });

    host.accept();
    std::vector<std::uint8_t> frame;
    REQUIRE(appendFrame(frame, FrameKind::Greet, 0, body));
    host.connection->receiveStream(frame, now);
    host.connection->service(now);
    std::vector<std::uint8_t> down;
    while (host.connection->popStreamFrame(down, now)) {
    }
    FrameDecoder decoder;
    decoder.append(down);
    std::optional<Welcome> welcome;
    while (const auto next = decoder.next()) {
        if (next->kind == FrameKind::Welcome)
            welcome = decodeWelcome(next->body);
    }
    REQUIRE(welcome.has_value());
    REQUIRE(welcome->resumed.size() == 1);
    CHECK(welcome->resumed[0].outcome == ResumeOutcome::Diffed);
    CHECK(welcome->resumed[0].inputWritten == std::optional<std::uint64_t> { held->panes[0].lastInputSeq });
    CHECK(host.connection->clientInstance() == clientInstance);
    CHECK_EQ(host.connection->counters().checkpointsSent, 0u);
}
