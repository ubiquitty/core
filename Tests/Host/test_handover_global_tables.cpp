// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Harness.h"
#include "Platform/Host/HandoverGlobalTables.h"
#include "ScopedNothrowAllocationRefusal.h"

#include <bit>

using namespace Ubiquitty;
using namespace Ubiquitty::Host;
namespace Sync = Protocol::Sync;

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {
constexpr HandoverGlobalLimits limits { 2 * 1024 * 1024, 4096, 32, 32, 1024 * 1024, { 1024 * 1024, 65536, 1024, 1024 * 1024 } };
const auto pane = Session::backgroundPaneID(17, 23);

HandoverGlobalTables state()
{
    HandoverGlobalTables result;
    result.configuration.fontSize = 13.75;
    result.configuration.fontFamily = std::string("family\0name", 11);
    result.configuration.folderIcon = false;
    result.shellPath = "/bin/sh";
    result.paneHostName = "build-host";
    result.paneHomeDirectory = "/private/task-home";
    result.columns = 123;
    result.rows = 37;
    Session::EventLog events(3);
    for (unsigned index = 0; index < 5; ++index)
        events.append({ .kind = Session::EventKind::CommandFinished, .sessionID = 7, .paneID = 23, .exitStatus = static_cast<std::int32_t>(index) - 2, .text = "command-" + std::to_string(index), .detail = "/work" });
    result.events = events.continuation();
    Session::BackgroundEvents background(17);
    background.appendNotification(pane, 7, { .title = "first", .body = "old", .identifier = "build", .occasion = NotificationOccasion::Unfocused }, 100);
    background.appendNotification(pane, 7, { .title = "second", .body = "new", .identifier = "build", .occasion = NotificationOccasion::Invisible }, 300);
    result.backgroundEvents = background.continuation();
    result.backgroundInventoryRevision = 71;
    result.backgroundEventPath = "/private/task-home/host-0123456789abcdef.sock.background-events";
    result.backgroundEventWriteDue = 1100;
    result.writtenBackgroundEventRevision = 1;
    result.isBackgroundEventStorageAvailable = false;
    result.eventDeliveries = 911;
    result.nextScrollbackFoldMilliseconds = 123456;
    result.lastScrollbackFoldSessionID = 7;
    result.closuresOwed = { 9, 4, 9 };
    result.terminationsOwed = { { 7, { 23, 17 } }, { 11, { 29, 17 } } };
    result.hasHadSession = true;
    return result;
}

std::vector<std::uint8_t> bytesOf(const Sync::HandoverBytes& bytes) { return { bytes.view().begin(), bytes.view().end() }; }

void redigest(std::vector<std::uint8_t>& bytes)
{
    bytes.resize(bytes.size() - 16);
    const auto digest = Sync::digestOf(bytes);
    Sync::WireWriter writer(bytes);
    writer.u64(digest.low);
    writer.u64(digest.high);
}
} // namespace

TEST(globalRoundTripPreservesHostTemplateAndOriginalTimerBits)
{
    const auto original = state();
    const auto encoded = encodeHandoverGlobals(original, limits);
    REQUIRE(encoded);
    const auto restored = decodeHandoverGlobals(encoded->view(), limits);
    REQUIRE(restored);
    CHECK(restored->configuration.fontFamily == original.configuration.fontFamily);
    CHECK(restored->configuration.fontSize == original.configuration.fontSize);
    CHECK(restored->configuration.folderIcon == false);
    CHECK(!restored->configuration.titleWindowDirectory);
    CHECK_EQ(restored->columns, 123);
    CHECK_EQ(restored->rows, 37);
    CHECK(restored->shellPath == original.shellPath);
    CHECK(restored->paneHostName == original.paneHostName);
    CHECK(restored->paneHomeDirectory == original.paneHomeDirectory);
    CHECK_EQ(restored->backgroundInventoryRevision, 71u);
    CHECK(restored->backgroundEventPath == original.backgroundEventPath);
    CHECK_EQ(restored->backgroundEventWriteDue, 1100u);
    CHECK_EQ(restored->writtenBackgroundEventRevision, 1u);
    CHECK(!restored->isBackgroundEventStorageAvailable);
    CHECK_EQ(restored->eventDeliveries, 911u);
    CHECK_EQ(restored->nextScrollbackFoldMilliseconds, 123456u);
    CHECK_EQ(restored->lastScrollbackFoldSessionID, 7u);
    CHECK(restored->closuresOwed == original.closuresOwed);
    CHECK(restored->terminationsOwed == original.terminationsOwed);
    CHECK(restored->hasHadSession);
    const auto repeated = encodeHandoverGlobals(*restored, limits);
    REQUIRE(repeated);
    CHECK(bytesOf(*repeated) == bytesOf(*encoded));
}

TEST(eventRingContinuesPhysicalOverwriteAndSubscriberGapExactly)
{
    const auto original = state();
    const auto encoded = encodeHandoverGlobals(original, limits);
    REQUIRE(encoded);
    auto restored = decodeHandoverGlobals(encoded->view(), limits);
    REQUIRE(restored);
    CHECK(restored->events.ring == original.events.ring);
    CHECK_EQ(restored->events.head, 2u);
    CHECK_EQ(restored->events.nextCursor, 6u);
    Session::EventLog predecessor(3), successor(3);
    auto copy = original.events;
    REQUIRE(predecessor.restoreContinuation(std::move(copy)));
    REQUIRE(successor.restoreContinuation(std::move(restored->events)));
    CHECK_EQ(successor.since(0).dropped, 2u);
    const Session::Event next { .kind = Session::EventKind::TitleChanged, .sessionID = 7, .text = "continued", .detail = {} };
    predecessor.append(next);
    successor.append(next);
    CHECK_EQ(successor.latestCursor(), 6u);
    CHECK(successor.continuation().ring == predecessor.continuation().ring);
    CHECK_EQ(successor.continuation().head, predecessor.continuation().head);
    CHECK(successor.since(4).events == predecessor.since(4).events);
    CHECK_EQ(successor.since(0).gapCursor, predecessor.since(0).gapCursor);
}

TEST(backgroundReplacementAndExpiryContinueWithoutRevisionOrEpochReset)
{
    const auto original = state();
    const auto encoded = encodeHandoverGlobals(original, limits);
    REQUIRE(encoded);
    auto restored = decodeHandoverGlobals(encoded->view(), limits);
    REQUIRE(restored);
    CHECK(restored->backgroundEvents.events == original.backgroundEvents.events);
    CHECK_EQ(restored->backgroundEvents.revision, 3u);
    Session::BackgroundEvents predecessor(17), successor(17);
    auto copy = original.backgroundEvents;
    REQUIRE(predecessor.restoreContinuation(std::move(copy)));
    REQUIRE(successor.restoreContinuation(std::move(restored->backgroundEvents)));
    CHECK_EQ(successor.epoch(), 17u);
    CHECK_EQ(successor.revision(), predecessor.revision());
    CHECK(successor.since(0, 301).events == predecessor.since(0, 301).events);
    const NotificationRequest request { .title = "third", .body = "newest", .identifier = "build" };
    predecessor.appendNotification(pane, 7, request, 600);
    successor.appendNotification(pane, 7, request, 600);
    CHECK_EQ(successor.revision(), 5u);
    CHECK(successor.continuation().events == predecessor.continuation().events);
    CHECK_EQ(successor.since(0, 600 + Session::BackgroundEvents::lifetimeMilliseconds).events.size(), 0u);
    CHECK_EQ(successor.since(0, 600 + Session::BackgroundEvents::lifetimeMilliseconds).cursor, 3u);
}

TEST(journalContinuationRefusalPreservesBothOwners)
{
    auto original = state();
    Session::EventLog wrongCapacity(2), fresh(3);
    CHECK(!wrongCapacity.restoreContinuation(std::move(original.events)));
    CHECK_EQ(original.events.ring.size(), 3u);
    auto malformed = original.events;
    malformed.nextCursor = UINT64_MAX;
    CHECK(!fresh.restoreContinuation(std::move(malformed)));
    malformed = original.events;
    malformed.ring[0].cursor = 99;
    CHECK(!fresh.restoreContinuation(std::move(malformed)));
    REQUIRE(fresh.restoreContinuation(std::move(original.events)));
    auto another = state().events;
    CHECK(!fresh.restoreContinuation(std::move(another)));
    CHECK_EQ(another.ring.size(), 3u);
    Session::BackgroundEvents wrongEpoch(18), background(17);
    CHECK(!wrongEpoch.restoreContinuation(std::move(original.backgroundEvents)));
    CHECK_EQ(original.backgroundEvents.events.size(), 2u);
    auto invalid = original.backgroundEvents;
    invalid.events.back().sequence = 1;
    CHECK(!background.restoreContinuation(std::move(invalid)));
    CHECK_EQ(background.revision(), 0u);
    REQUIRE(background.restoreContinuation(std::move(original.backgroundEvents)));
    invalid = state().backgroundEvents;
    CHECK(!background.restoreContinuation(std::move(invalid)));
    CHECK_EQ(invalid.events.size(), 2u);
}

TEST(globalDecoderRefusesEveryTruncationAndFutureOrCorruptSections)
{
    const auto encoded = encodeHandoverGlobals(state(), limits);
    REQUIRE(encoded);
    for (std::size_t size = 0; size < encoded->size; ++size)
        CHECK(!decodeHandoverGlobals(encoded->view().first(size), limits));
    auto bytes = bytesOf(*encoded);
    bytes[0] = 2;
    const auto newer = decodeHandoverGlobals(bytes, limits);
    REQUIRE(!newer);
    CHECK(newer.error() == Sync::HandoverBodyError::NewerVersion);
    bytes = bytesOf(*encoded);
    bytes.back() ^= 1;
    CHECK(!decodeHandoverGlobals(bytes, limits));
    bytes = bytesOf(*encoded);
    bytes.push_back(0);
    CHECK(!decodeHandoverGlobals(bytes, limits));
    bytes = bytesOf(*encoded);
    // The nested preferences version remains independently checked: the
    // section after the header's version, magic and length carries its own,
    // and one past it is a newer configuration.
    constexpr std::size_t configurationVersionOffset = 10;
    const auto configurationVersion = static_cast<std::uint16_t>(bytes[configurationVersionOffset] | (bytes[configurationVersionOffset + 1] << 8));
    const auto newerVersion = static_cast<std::uint16_t>(configurationVersion + 1);
    bytes[configurationVersionOffset] = static_cast<std::uint8_t>(newerVersion & 0xFF);
    bytes[configurationVersionOffset + 1] = static_cast<std::uint8_t>(newerVersion >> 8);
    redigest(bytes);
    const auto newerConfiguration = decodeHandoverGlobals(bytes, limits);
    REQUIRE(!newerConfiguration);
    CHECK(newerConfiguration.error() == Sync::HandoverBodyError::NewerVersion);
}

TEST(globalAdmissionHonorsIndependentCountsAndConfigurationBounds)
{
    const auto encoded = encodeHandoverGlobals(state(), limits);
    REQUIRE(encoded);
    auto exact = limits;
    exact.maxEvents = 3;
    exact.maxClosures = 3;
    exact.maxTerminations = 2;
    exact.maxSectionBytes = encoded->size;
    REQUIRE(decodeHandoverGlobals(encoded->view(), exact));
    auto small = exact;
    --small.maxEvents;
    CHECK(!decodeHandoverGlobals(encoded->view(), small));
    small = exact;
    --small.maxClosures;
    CHECK(!decodeHandoverGlobals(encoded->view(), small));
    small = exact;
    --small.maxTerminations;
    CHECK(!decodeHandoverGlobals(encoded->view(), small));
    small = exact;
    --small.maxSectionBytes;
    CHECK(!decodeHandoverGlobals(encoded->view(), small));
    small = exact;
    small.configuration.maxDecodedBytes = 1;
    CHECK(!decodeHandoverGlobals(encoded->view(), small));
    small = exact;
    small.maxStringBytes = 1;
    CHECK(!decodeHandoverGlobals(encoded->view(), small));
}

TEST(emptyLogsRemainFreshAndBackgroundEvictionKeepsItsGap)
{
    auto original = state();
    original.events = Session::EventLog(1).continuation();
    Session::BackgroundEvents background(17);
    for (unsigned index = 0; index < 258; ++index)
        background.appendNotification(pane, 7, { .title = "notice", .body = {}, .identifier = {} }, index);
    original.backgroundEvents = background.continuation();
    const auto encoded = encodeHandoverGlobals(original, limits);
    REQUIRE(encoded);
    auto restored = decodeHandoverGlobals(encoded->view(), limits);
    REQUIRE(restored);
    Session::EventLog events(1);
    REQUIRE(events.restoreContinuation(std::move(restored->events)));
    CHECK_EQ(events.latestCursor(), 0u);
    events.append({ .kind = Session::EventKind::Bell, .text = {}, .detail = {} });
    CHECK_EQ(events.latestCursor(), 1u);
    Session::BackgroundEvents successor(17);
    REQUIRE(successor.restoreContinuation(std::move(restored->backgroundEvents)));
    CHECK(successor.since(0, 300).hasGap);
    CHECK_EQ(successor.since(0, 300).cursor, background.since(0, 300).cursor);
    successor.appendNotification(pane, 7, { .title = "next", .body = {}, .identifier = {} }, 301);
    CHECK_EQ(successor.continuation().droppedThrough, 3u);
    CHECK_EQ(successor.continuation().latest, 259u);
}

TEST(globalAdmissionRejectsInvalidGeometryIdentitiesAndExhaustion)
{
    auto source = state();
    source.columns = -1;
    CHECK(!encodeHandoverGlobals(source, limits));
    source = state();
    source.rows = 0;
    CHECK(!encodeHandoverGlobals(source, limits));
    source = state();
    source.terminationsOwed[0].second = {};
    CHECK(!encodeHandoverGlobals(source, limits));
    source = state();
    source.backgroundEvents.revision = UINT64_MAX;
    CHECK(!encodeHandoverGlobals(source, limits));
    source = state();
    source.backgroundInventoryRevision = UINT64_MAX;
    CHECK(!encodeHandoverGlobals(source, limits));
    source = state();
    source.events.ring[0].kind = Session::EventKind::Gap;
    CHECK(!encodeHandoverGlobals(source, limits));
}

TEST(globalExactOutputAllocationRefusalKeepsEverySourceOwner)
{
    const auto source = state();
    const auto original = encodeHandoverGlobals(source, limits);
    REQUIRE(original);
    {
        const UbiquittyTest::ScopedNothrowAllocationRefusal refusal;
        const auto refused = encodeHandoverGlobals(source, limits);
        REQUIRE(!refused);
        CHECK(refused.error() == Sync::HandoverBodyError::AllocationRefused);
    }
    const auto after = encodeHandoverGlobals(source, limits);
    REQUIRE(after);
    CHECK(bytesOf(*after) == bytesOf(*original));
}

UBIQUITTY_TEST_MAIN()
