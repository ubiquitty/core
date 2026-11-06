// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Protocol/Sync/Effects.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

using namespace Ubiquitty::Protocol::Sync;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

constexpr ID128 pane { .low = 1, .high = 1 };
constexpr ID128 phone { .low = 2, .high = 1 };
constexpr ID128 desktop { .low = 3, .high = 1 };
constexpr ID128 phoneAttachment { .low = 4, .high = 1 };
constexpr ID128 desktopAttachment { .low = 5, .high = 1 };

class EffectSecrets final : public SecretSource {
public:
    ID128 nextSecret() override { return { ++m_next, 0xEFEC7 }; }

private:
    std::uint64_t m_next = 0;
};

EffectDraft draft(EffectKind kind, std::uint64_t generation = 1, ID128 primary = phone)
{
    return { .generation = generation, .primary = primary, .kind = kind, .count = 1, .target = 0, .payload = {} };
}

} // namespace

TEST(tenBellsInASkippedSpanCoalesceToOneBell)
{
    EffectLog log(pane);
    for (std::uint64_t generation = 3; generation <= 12; ++generation)
        REQUIRE(log.append(draft(EffectKind::Bell, generation)));
    const auto batch = log.readAfter(0, EffectReadMode::Skipped);
    REQUIRE(batch);
    REQUIRE(batch->size == 1);
    CHECK_EQ(batch->records[0].count, 10u);
    CHECK_EQ(batch->records[0].generation, 12u);
    CHECK_EQ(batch->records[0].sequence, 10u);
    CHECK(batch->records[0].paneID == pane);
    CHECK_EQ(batch->throughSequence, 10u);
    const auto consecutive = log.readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(consecutive);
    CHECK_EQ(consecutive->size, 10u);
}

TEST(effectTransferBytesAreImmutableChargedAndReleasedWithTheirRecord)
{
    EffectPolicy policy;
    policy.maxPayloadBytes = 2;
    policy.maxRetainedBytes = 6;
    EffectLog log(pane, policy);
    REQUIRE(log.addReader(phoneAttachment, phone, 0));
    std::array<std::uint8_t, 2> body { 1, 2 };
    std::array<std::uint8_t, 4> pixels { 3, 4, 5, 6 };
    auto sound = draft(EffectKind::Sound);
    sound.payload = body;
    sound.data = pixels;
    REQUIRE(log.append(sound));
    CHECK_EQ(log.retainedBytes(), 6u);
    body.fill(9);
    pixels.fill(9);
    const auto read = log.readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(read && read->size == 1);
    const std::array<std::uint8_t, 2> expectedBody { 1, 2 };
    const std::array<std::uint8_t, 4> expectedPixels { 3, 4, 5, 6 };
    CHECK(std::ranges::equal(read->records[0].payload, expectedBody));
    CHECK(std::ranges::equal(read->records[0].data, expectedPixels));
    CHECK(!log.append(sound));
    REQUIRE(log.noteState(phoneAttachment, AttachmentState::Lost, 10));
    log.trim(11);
    CHECK_EQ(log.retainedBytes(), 6u);
    REQUIRE(log.noteState(phoneAttachment, AttachmentState::Detached, 12));
    log.trim(12);
    CHECK_EQ(log.retainedBytes(), 0u);
    CHECK_EQ(log.retiredThrough(), 1u);
}

TEST(effectTransferAdmissionPrecedesItsSingleAllocationAndRefusalIsAtomic)
{
    EffectPolicy policy;
    policy.maxPayloadBytes = 2;
    policy.maxRetainedBytes = 5;
    EffectLog log(pane, policy);
    const std::array<std::uint8_t, 2> body { 1, 2 };
    const std::array<std::uint8_t, 4> data { 3, 4, 5, 6 };
    auto effect = draft(EffectKind::ClipboardWrite);
    effect.payload = body;
    effect.data = data;
    const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(6);
        const auto over = log.append(effect);
        CHECK(!over && over.error() == EffectRefusal::OverLimit);
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before);
    effect.data = std::span(data).first(3);
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(5);
        const auto refused = log.append(effect);
        CHECK(!refused && refused.error() == EffectRefusal::NoMemory);
    }
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), before + 1);
    CHECK_EQ(log.retainedBytes(), 0u);
    CHECK_EQ(log.newestSequence(), 0u);
    effect.kind = EffectKind::Bell;
    CHECK(!log.append(effect));
    effect.kind = EffectKind::ClipboardWrite;
    REQUIRE(log.append(effect));
    CHECK_EQ(log.retainedBytes(), 5u);
}

TEST(ownedEffectDataMovesWithoutCopyAndRefusalPreservesTheCallerOwner)
{
    EffectPolicy policy;
    policy.maxPayloadBytes = 2;
    policy.maxRetainedBytes = 6;
    EffectLog log(pane, policy);
    std::array<std::uint8_t, 2> body { 8, 9 };
    auto effect = draft(EffectKind::ClipboardWrite);
    effect.payload = body;
    auto data = std::make_unique<std::uint8_t[]>(4);
    data[0] = 3;
    auto* original = data.get();
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(2);
        const auto refused = log.appendOwnedData(effect, data, 4);
        CHECK(!refused && refused.error() == EffectRefusal::NoMemory);
        CHECK(data.get() == original);
        CHECK_EQ(log.newestSequence(), 0u);
        CHECK_EQ(log.retainedBytes(), 0u);
    }
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(4);
        REQUIRE(log.appendOwnedData(effect, data, 4));
    }
    CHECK(!data);
    const auto batch = log.readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(batch && batch->size == 1);
    CHECK(batch->records[0].data.data() == original);
    CHECK_EQ(batch->records[0].data[0], 3u);
    CHECK_EQ(log.retainedBytes(), 6u);
    auto overflow = std::make_unique<std::uint8_t[]>(1);
    auto* overflowOwner = overflow.get();
    const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    CHECK(!log.appendOwnedData(effect, overflow, 1));
    CHECK(overflow.get() == overflowOwner);
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), before);
    log.trim(1);
    CHECK_EQ(log.retainedBytes(), 0u);
    CHECK_EQ(log.retiredThrough(), 1u);
}

TEST(aSkippedSpanOmitsSoundKeepsEightNotificationsAndTheLastCommand)
{
    EffectLog log(pane);
    REQUIRE(log.append(draft(EffectKind::CommandFinished)));
    for (std::uint64_t generation = 2; generation <= 11; ++generation)
        REQUIRE(log.append(draft(EffectKind::Notification, generation)));
    REQUIRE(log.append(draft(EffectKind::CommandFinished, 12)));
    REQUIRE(log.append(draft(EffectKind::Sound, 13)));
    const auto batch = log.readAfter(0, EffectReadMode::Skipped);
    REQUIRE(batch);
    REQUIRE(batch->size == 9);
    CHECK_EQ(batch->records[0].sequence, 4u);
    CHECK(batch->records[8].kind == EffectKind::CommandFinished);
    CHECK_EQ(batch->records[8].generation, 12u);
    CHECK_EQ(batch->throughSequence, 13u);
}

TEST(coalescingKeepsClipboardTargetsAndRecipientsSeparate)
{
    EffectLog log(pane);
    for (std::uint8_t target : { 1, 2, 1 }) {
        auto effect = draft(EffectKind::ClipboardWrite);
        effect.target = target;
        REQUIRE(log.append(effect));
    }
    auto other = draft(EffectKind::ClipboardWrite, 1, desktop);
    other.target = 1;
    REQUIRE(log.append(other));
    const auto batch = log.readAfter(0, EffectReadMode::Skipped);
    REQUIRE(batch);
    REQUIRE(batch->size == 3);
    CHECK_EQ(batch->records[0].target, 2u);
    CHECK_EQ(batch->records[1].sequence, 3u);
    CHECK(batch->records[1].primary == phone);
    CHECK(batch->records[2].primary == desktop);
}

TEST(coalescedBellsNeverMixPrimaryRecipientsOrWrapTheirCount)
{
    EffectLog log(pane);
    REQUIRE(log.append(draft(EffectKind::Bell)));
    REQUIRE(log.append(draft(EffectKind::Bell, 1, desktop)));
    auto last = draft(EffectKind::Bell);
    last.count = std::numeric_limits<std::uint32_t>::max();
    REQUIRE(log.append(last));
    const auto refused = log.readAfter(0, EffectReadMode::Skipped);
    REQUIRE(!refused);
    CHECK(refused.error() == EffectRefusal::OverLimit);
    const auto suffix = log.readAfter(1, EffectReadMode::Skipped);
    REQUIRE(suffix);
    REQUIRE(suffix->size == 2);
    CHECK(suffix->records[0].primary == desktop);
    CHECK(suffix->records[1].primary == phone);
    CHECK_EQ(suffix->records[1].count, std::numeric_limits<std::uint32_t>::max());
}

TEST(effectCursorsDistinguishTwoEffectsOfOneScreenGeneration)
{
    EffectLog log(pane);
    REQUIRE(log.addReader(phoneAttachment, phone, 0));
    REQUIRE(log.append(draft(EffectKind::Notification, 10)));
    REQUIRE(log.append(draft(EffectKind::Notification, 10)));
    REQUIRE(log.noteSent(phoneAttachment, 1));
    REQUIRE(log.acknowledge(phoneAttachment, 1));
    log.trim(0);
    const auto suffix = log.readAfter(1, EffectReadMode::Consecutive);
    REQUIRE(suffix);
    REQUIRE(suffix->size == 1);
    CHECK_EQ(suffix->records[0].sequence, 2u);
    CHECK_EQ(suffix->records[0].generation, 10u);
    CHECK(!log.readAfter(0, EffectReadMode::Consecutive));
}

TEST(acknowledgementsCannotRetireUnsentOtherReaderOrLostEffects)
{
    EffectLog log(pane);
    REQUIRE(log.addReader(phoneAttachment, phone, 0));
    REQUIRE(log.addReader(desktopAttachment, desktop, 0));
    REQUIRE(log.append(draft(EffectKind::Bell)));
    CHECK(!log.acknowledge(phoneAttachment, 1));
    REQUIRE(log.noteSent(phoneAttachment, 1));
    CHECK(!log.acknowledge(desktopAttachment, 1));
    CHECK(!log.noteSent(phoneAttachment, 2));
    REQUIRE(log.acknowledge(phoneAttachment, 1));
    CHECK(!log.acknowledge(phoneAttachment, 0));
    log.trim(1);
    CHECK_EQ(log.size(), 1u);
    REQUIRE(log.noteState(desktopAttachment, AttachmentState::Lost, 10));
    CHECK(!log.noteSent(desktopAttachment, 1));
    CHECK(!log.acknowledge(desktopAttachment, 1));
    log.trim(11);
    CHECK_EQ(log.size(), 1u);
    REQUIRE(log.noteState(desktopAttachment, AttachmentState::Live, 12));
    REQUIRE(log.noteSent(desktopAttachment, 1));
    REQUIRE(log.acknowledge(desktopAttachment, 1));
    log.trim(12);
    CHECK_EQ(log.size(), 0u);
    REQUIRE(log.append(draft(EffectKind::Bell, 2)));
    CHECK_EQ(log.newestSequence(), 2u);
}

TEST(lostGraceNeverRestartsAndDetachedReadersRetainNothing)
{
    EffectPolicy policy;
    policy.lostGraceMicroseconds = 100;
    EffectLog log(pane, policy);
    REQUIRE(log.addReader(phoneAttachment, phone, 0));
    REQUIRE(log.append(draft(EffectKind::Notification)));
    REQUIRE(log.noteState(phoneAttachment, AttachmentState::Lost, 10));
    REQUIRE(log.noteState(phoneAttachment, AttachmentState::Lost, 90));
    log.trim(9);
    CHECK_EQ(log.readerCount(), 1u);
    log.trim(109);
    CHECK_EQ(log.size(), 1u);
    log.trim(110);
    CHECK_EQ(log.size(), 0u);
    CHECK_EQ(log.readerCount(), 0u);
    CHECK(!log.noteState(phoneAttachment, AttachmentState::Live, 111));
    CHECK(!log.addReader(phoneAttachment, phone, 0));
    REQUIRE(log.addReader(phoneAttachment, phone, 1));
    REQUIRE(log.append(draft(EffectKind::Bell, 2)));
    REQUIRE(log.noteState(phoneAttachment, AttachmentState::Detached, 112));
    log.trim(112);
    CHECK_EQ(log.size(), 0u);
}

TEST(resumingAnEffectReaderPreservesIdentityAndItsEmittedBoundary)
{
    EffectLog log(pane);
    REQUIRE(log.addReader(phoneAttachment, phone, 0));
    REQUIRE(log.append(draft(EffectKind::Bell)));
    REQUIRE(log.append(draft(EffectKind::Bell)));
    REQUIRE(log.noteSent(phoneAttachment, 1));
    REQUIRE(log.noteState(phoneAttachment, AttachmentState::Lost, 10));
    CHECK(!log.resumeReader(phoneAttachment, desktop, 1));
    CHECK(!log.resumeReader(phoneAttachment, phone, 2));
    CHECK(!log.acknowledge(phoneAttachment, 1));
    log.trim(11);
    CHECK_EQ(log.size(), 2u);
    REQUIRE(log.resumeReader(phoneAttachment, phone, 1));
    CHECK(!log.resumeReader(phoneAttachment, phone, 0));
    CHECK(!log.acknowledge(phoneAttachment, 2));
    log.trim(12);
    CHECK_EQ(log.size(), 1u);
    REQUIRE(log.noteSent(phoneAttachment, 2));
    REQUIRE(log.acknowledge(phoneAttachment, 2));
    log.trim(13);
    CHECK_EQ(log.size(), 0u);
}

TEST(transientReadersCannotOutliveTheirCarrierOrRetireAnotherReadersEffects)
{
    EffectLog log(pane);
    REQUIRE(log.addReader(phoneAttachment, phone, 0));
    CHECK(!log.addTransientReader(desktopAttachment, desktop, 0, {}));
    {
        const auto lifetime = std::make_shared<EffectReaderLifetime>();
        REQUIRE(log.addTransientReader(desktopAttachment, desktop, 0, lifetime));
        CHECK(!log.addTransientReader(desktopAttachment, desktop, 0, lifetime));
        REQUIRE(log.append(draft(EffectKind::Bell)));
        CHECK_EQ(log.readerCount(), 2u);
    }
    CHECK_EQ(log.readerCount(), 1u);
    CHECK(!log.noteSent(desktopAttachment, 1));
    log.trim(1);
    CHECK_EQ(log.size(), 1u);
    const auto replacement = std::make_shared<EffectReaderLifetime>();
    REQUIRE(log.addTransientReader(desktopAttachment, desktop, 1, replacement));
    REQUIRE(log.noteSent(phoneAttachment, 1));
    REQUIRE(log.acknowledge(phoneAttachment, 1));
    log.trim(2);
    CHECK_EQ(log.size(), 0u);
}

TEST(tableReconciliationUsesTheOriginalLostClockAndForgetsReleasedHoldings)
{
    EffectSecrets secrets;
    AttachmentTable table({ 31, 32 }, { .lostGraceMicroseconds = 100 }, secrets);
    AttachmentRecord& record = table.create(phone, ClientKind::Human, { .principal = "effects" }, 1, 0);
    const ID128 attachment = record.attachmentID;
    (void)record.paneFor(pane);
    EffectPolicy policy;
    policy.lostGraceMicroseconds = 100;
    EffectLog log(pane, policy);
    REQUIRE(log.addReader(attachment, phone, 0));
    REQUIRE(log.append(draft(EffectKind::Bell)));
    table.noteTransportLost(attachment, 1, 10);
    log.reconcileReaders(table, 90);
    CHECK(!log.noteSent(attachment, 1));
    log.reconcileReaders(table, 109);
    CHECK_EQ(log.size(), 1u);
    table.expire(110);
    log.reconcileReaders(table, 110);
    CHECK_EQ(log.size(), 0u);
    CHECK_EQ(log.readerCount(), 0u);
    CHECK_EQ(log.retiredThrough(), 1u);

    AttachmentRecord& current = table.create(phone, ClientKind::Human, { .principal = "effects" }, 2, 111);
    PaneBaselines& held = current.paneFor(pane);
    REQUIRE(log.addReader(current.attachmentID, phone, 1));
    REQUIRE(log.append(draft(EffectKind::Bell)));
    held.isHeld = false;
    log.reconcileReaders(table, 112);
    CHECK_EQ(log.readerCount(), 0u);
    CHECK_EQ(log.retiredThrough(), 2u);
}

TEST(primaryElectionDoesNotChangePreviouslyAcknowledgedBadgeEffects)
{
    EffectLog log(pane);
    REQUIRE(log.addReader(phoneAttachment, phone, 0));
    REQUIRE(log.addReader(desktopAttachment, desktop, 0));
    REQUIRE(log.append(draft(EffectKind::Notification)));
    const auto original = log.readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(original);
    REQUIRE(original->size == 1);
    CHECK(effectDisposition(original->records[0], desktop, ClientKind::Human, Role::Driver) == EffectDisposition::Badge);
    REQUIRE(log.noteSent(desktopAttachment, 1));
    REQUIRE(log.acknowledge(desktopAttachment, 1));
    REQUIRE(log.noteState(phoneAttachment, AttachmentState::Lost, 10));
    REQUIRE(log.append(draft(EffectKind::Notification, 2, desktop)));
    const auto successor = log.readAfter(1, EffectReadMode::Consecutive);
    REQUIRE(successor);
    REQUIRE(successor->size == 1);
    CHECK_EQ(successor->records[0].sequence, 2u);
    CHECK(effectDisposition(successor->records[0], desktop, ClientKind::Human, Role::Driver) == EffectDisposition::Post);
    CHECK(!log.acknowledge(desktopAttachment, 0));
    log.trim(11);
    CHECK_EQ(log.size(), 2u);
    const auto batch = log.readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(batch);
    REQUIRE(batch->size == 2);
    CHECK(batch->records[0].primary == phone);
    CHECK(batch->records[1].primary == desktop);
    REQUIRE(log.noteState(phoneAttachment, AttachmentState::Live, 12));
    CHECK(effectDisposition(batch->records[0], phone, ClientKind::Human, Role::Owner) == EffectDisposition::Post);
    CHECK(effectDisposition(batch->records[0], desktop, ClientKind::Human, Role::Driver) == EffectDisposition::Badge);
}

TEST(viewersAndAgentsNeverPostClipboardWritesEvenWithTheirOwnTag)
{
    EffectLog log(pane);
    REQUIRE(log.append(draft(EffectKind::ClipboardWrite)));
    const auto batch = log.readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(batch);
    const auto& effect = batch->records[0];
    CHECK(effectDisposition(effect, phone, ClientKind::Human, Role::Owner) == EffectDisposition::Post);
    CHECK(effectDisposition(effect, desktop, ClientKind::Human, Role::Driver) == EffectDisposition::Ignore);
    CHECK(effectDisposition(effect, phone, ClientKind::Viewer, Role::Owner) == EffectDisposition::Ignore);
    CHECK(effectDisposition(effect, phone, ClientKind::Human, Role::Viewer) == EffectDisposition::Ignore);
    CHECK(effectDisposition(effect, phone, ClientKind::Agent, Role::Owner) == EffectDisposition::Ignore);
    CHECK(effectDisposition(effect, phone, ClientKind::Human, Role::Agent) == EffectDisposition::Ignore);
}

TEST(payloadOwnershipAndBothStorageLimitsAreTransactional)
{
    EffectPolicy policy { .maxRecords = 2, .maxPayloadBytes = 4, .maxRetainedBytes = 6, .notificationsKept = 2, .lostGraceMicroseconds = 100 };
    EffectLog log(pane, policy);
    std::array<std::uint8_t, 4> bytes { 1, 2, 3, 4 };
    auto effect = draft(EffectKind::Notification);
    effect.payload = bytes;
    REQUIRE(log.append(effect));
    bytes[0] = 99;
    const auto refusal = log.append(effect);
    REQUIRE(!refusal);
    CHECK(refusal.error() == EffectRefusal::OverLimit);
    CHECK_EQ(log.newestSequence(), 1u);
    CHECK_EQ(log.retainedBytes(), 4u);
    const auto batch = log.readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(batch);
    CHECK_EQ(batch->records[0].payload[0], 1u);
    REQUIRE(log.append(draft(EffectKind::Bell)));
    CHECK(!log.append(draft(EffectKind::Bell)));
    CHECK_EQ(log.size(), 2u);
    log.trim(0);
    CHECK_EQ(log.retainedBytes(), 0u);
    REQUIRE(log.append(draft(EffectKind::Bell)));
    CHECK_EQ(log.newestSequence(), 3u);
}

TEST(overLimitPayloadIsRejectedBeforeAllocationAndAllocationFailureConsumesNoIdentity)
{
    EffectPolicy policy { .maxRecords = 8, .maxPayloadBytes = 4, .maxRetainedBytes = 4, .notificationsKept = 8, .lostGraceMicroseconds = 100 };
    EffectLog log(pane, policy);
    const std::array<std::uint8_t, 5> bytes { 1, 2, 3, 4, 5 };
    auto effect = draft(EffectKind::Notification);
    effect.payload = bytes;
    const auto attempts = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
    REQUIRE(!log.append(effect));
    CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount(), attempts);
    effect.payload = std::span(bytes).first(4);
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
        const auto result = log.append(effect);
        REQUIRE(!result);
        CHECK(result.error() == EffectRefusal::NoMemory);
    }
    CHECK_EQ(log.size(), 0u);
    CHECK_EQ(log.newestSequence(), 0u);
    REQUIRE(log.append(effect));
    CHECK_EQ(log.newestSequence(), 1u);
}

TEST(identityExhaustionAndMalformedInputsFailClosed)
{
    EffectLog exhausted(pane, {}, std::numeric_limits<std::uint64_t>::max());
    const auto result = exhausted.append(draft(EffectKind::Bell));
    REQUIRE(!result);
    CHECK(result.error() == EffectRefusal::ExhaustedIdentity);
    EffectLog log(pane);
    REQUIRE(log.append(draft(EffectKind::Bell, 10)));
    CHECK(!log.append(draft(EffectKind::Bell, 9)));
    auto invalid = draft(EffectKind::Bell, 10);
    invalid.count = 0;
    CHECK(!log.append(invalid));
    invalid.count = 1;
    invalid.target = 1;
    CHECK(!log.append(invalid));
    CHECK(!log.readAfter(2, EffectReadMode::Consecutive));
    CHECK(!log.addReader({}, phone, 0));
    CHECK(!log.addReader(phoneAttachment, {}, 0));
    REQUIRE(log.addReader(phoneAttachment, phone, 0));
    CHECK(!log.addReader(phoneAttachment, phone, 1));
    CHECK_EQ(log.readerCount(), 1u);
    CHECK_EQ(log.newestSequence(), 1u);
}

TEST(readerStorageIsBoundedAndPaneIdentitiesDoNotAlias)
{
    EffectLog first(pane);
    EffectLog second(desktop);
    for (std::uint64_t index = 1; index <= maxEffectReaders; ++index)
        REQUIRE(first.addReader({ .low = index, .high = 0 }, phone, 0));
    CHECK(!first.addReader({ .low = maxEffectReaders + 1, .high = 0 }, phone, 0));
    REQUIRE(first.append(draft(EffectKind::Bell)));
    REQUIRE(second.append(draft(EffectKind::Bell)));
    const auto left = first.readAfter(0, EffectReadMode::Consecutive);
    const auto right = second.readAfter(0, EffectReadMode::Consecutive);
    REQUIRE(left && right);
    CHECK_EQ(left->records[0].sequence, right->records[0].sequence);
    CHECK(left->records[0].paneID != right->records[0].paneID);
}

UBIQUITTY_TEST_MAIN()
