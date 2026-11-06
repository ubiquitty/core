// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#ifndef UBIQUITTY_TEST_WITHOUT_CORE
#define UBIQUITTY_TEST_WITHOUT_CORE
#endif
#include "Core/Keys/KeyTranslation.h"
#include "Core/Session/Predictor.h"
#include "Harness.h"

#include <array>
#include <cstring>
#include <limits>

using namespace Ubiquitty;
using namespace Ubiquitty::Session;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

constexpr PredictionExposurePolicy testPolicy {
    .showAboveMicroseconds = 30'000,
    .hideAtMicroseconds = 20'000,
    .flagAboveMicroseconds = 80'000,
    .clearFlagAtMicroseconds = 50'000,
    .glitchAfterMicroseconds = 250'000,
    .forceFlagAfterMicroseconds = 5'000'000,
};

Keys::KeyEvent printable(std::uint32_t codePoint)
{
    Keys::KeyEvent key;
    key.keyCode = codePoint;
    key.textLength = 1;
    key.textCodePoints[0] = codePoint;
    return key;
}

Keys::KeyEvent functional(Keys::VirtualKey identity)
{
    Keys::KeyEvent key;
    key.virtualKey = identity;
    return key;
}

struct Fixture {
    std::array<Cell, 16> cells;
    std::array<std::uint8_t, 16> known;
    PredictionSnapshot snapshot;
    Predictor predictor;

    explicit Fixture(std::size_t limit = maxPredictionEdits)
    {
        cells.fill(defaultBlankCell);
        known.fill(1);
        snapshot.identity = { .hostInstance = { 1, 1 }, .clientInstance = { 2, 2 }, .attachment = { 4, 4 }, .pane = { 3, 3 }, .connectionEpoch = 1 };
        snapshot.generation = 1;
        snapshot.promptEpoch = 1;
        snapshot.rows = 2;
        snapshot.columns = static_cast<int>(cells.size());
        snapshot.cursorColumn = 3;
        snapshot.row = cells;
        snapshot.knownCells = known;
        snapshot.echoContext.flags.set(Sync::EchoFlag::AtPrompt, true);
        snapshot.hasEchoContext = true;
        snapshot.isCursorVisible = true;
        snapshot.canPredictSimpleEditing = true;
        snapshot.inputPen = defaultBlankCell.pen;
        CHECK(predictor.configure(limit, testPolicy).has_value());
        CHECK(predictor.acceptFrame(snapshot, Sync::FrameClass::Settled, 0).has_value());
    }

    std::expected<PredictionInputOutcome, PredictionError> type(std::uint64_t sequence, std::uint32_t scalar, std::uint64_t now)
    {
        const auto key = printable(scalar);
        return predictor.predictKeys(sequence, std::span { &key, 1 }, snapshot, now);
    }

    std::expected<PredictionInputOutcome, PredictionError> key(std::uint64_t sequence, const Keys::KeyEvent& event, std::uint64_t now)
    {
        return predictor.predictKeys(sequence, std::span { &event, 1 }, snapshot, now);
    }

    std::expected<PredictionReconciliation, PredictionError> frame(std::uint64_t settled, std::uint64_t now, Sync::FrameClass frameClass = Sync::FrameClass::Settled)
    {
        ++snapshot.generation;
        snapshot.inputSettled = settled;
        return predictor.acceptFrame(snapshot, frameClass, now);
    }

    void confirmEpoch()
    {
        CHECK(type(1, 'a', 1'000).has_value());
        cells[3].codePoint = 'a';
        snapshot.cursorColumn = 4;
        CHECK(frame(1, 41'000).has_value());
        CHECK(predictor.hasConfirmedEpoch());
    }
};

} // namespace

TEST(aChangedNonblankGlyphInAKnownBlankConfirmsButNeverMutatesTheModel)
{
    Fixture fixture;
    const auto before = fixture.cells;
    const auto input = fixture.type(1, 'a', 1'000);
    REQUIRE(input);
    CHECK(*input == PredictionInputOutcome::Tentative);
    CHECK(fixture.predictor.overlay(1'000'000).cells.empty());
    CHECK(!fixture.predictor.nextWakeDelay(1'000'000));
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    CHECK(std::memcmp(before.data(), fixture.cells.data(), sizeof(before)) == 0);
    fixture.cells[3].codePoint = 'a';
    fixture.snapshot.cursorColumn = 4;
    const auto frame = fixture.frame(1, 1'001'000);
    REQUIRE(frame);
    CHECK_EQ(frame->settledInputCount, 1u);
    CHECK_EQ(frame->echoLatencyMicroseconds.value_or(0), 1'000'000u);
    CHECK(fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    REQUIRE(fixture.type(2, 'b', 1'002'000));
    const auto overlay = fixture.predictor.overlay(1'002'000);
    REQUIRE(overlay.cells.size() == 1);
    REQUIRE(overlay.cursor);
    CHECK_EQ(overlay.cells[0].cell.codePoint, static_cast<std::uint32_t>('b'));
    CHECK_EQ(overlay.cells[0].column, 4);
    CHECK_EQ(overlay.cursor->column, 5);
    CHECK_EQ(fixture.cells[4].codePoint, 0u);
}

TEST(anEarlyMatchingEchoDoesNotConfirmBeforeInputSettled)
{
    Fixture fixture;
    REQUIRE(fixture.type(1, 'a', 1'000));
    fixture.cells[3].codePoint = 'a';
    fixture.snapshot.cursorColumn = 4;
    const auto early = fixture.frame(0, 6'000);
    REQUIRE(early);
    CHECK_EQ(early->settledInputCount, 0u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 1u);
    CHECK(fixture.predictor.overlay(300'000).cells.empty());
    const auto settled = fixture.frame(1, 301'000);
    REQUIRE(settled);
    CHECK(fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(settled->settledInputCount, 1u);
    const auto duplicate = fixture.predictor.acceptFrame(fixture.snapshot, Sync::FrameClass::Settled, 302'000);
    REQUIRE(duplicate);
    CHECK(duplicate->outcome == PredictionFrameOutcome::Unchanged);
    CHECK_EQ(duplicate->settledInputCount, 0u);
    CHECK(!duplicate->echoLatencyMicroseconds);
}

TEST(typingOverIdenticalTextAndShiftingItDoesNotEarnConfidence)
{
    Fixture fixture;
    fixture.cells[3].codePoint = 'a';
    REQUIRE(fixture.type(1, 'a', 1'000));
    fixture.cells[4].codePoint = 'a';
    fixture.snapshot.cursorColumn = 4;
    REQUIRE(fixture.frame(1, 41'000));
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    REQUIRE(fixture.type(2, 'b', 42'000));
    CHECK(fixture.predictor.overlay(1'000'000).cells.empty());
}

TEST(blankInsertionAndSimpleBackspaceCannotConfirmAnEpoch)
{
    Fixture space;
    REQUIRE(space.type(1, ' ', 1'000));
    space.cells[3].codePoint = ' ';
    space.snapshot.cursorColumn = 4;
    REQUIRE(space.frame(1, 41'000));
    CHECK(!space.predictor.hasConfirmedEpoch());

    Fixture erase;
    erase.cells[2].codePoint = 'a';
    REQUIRE(erase.key(1, functional(Keys::VirtualKey::Backspace), 1'000));
    erase.cells[2] = defaultBlankCell;
    erase.snapshot.cursorColumn = 2;
    REQUIRE(erase.frame(1, 41'000));
    CHECK(!erase.predictor.hasConfirmedEpoch());
    CHECK(erase.predictor.overlay(1'000'000).cells.empty());
}

TEST(insertionAndBackspaceShiftOnlyTheBoundedKnownSimpleTail)
{
    Fixture fixture;
    fixture.confirmEpoch();
    fixture.cells[4].codePoint = 'x';
    fixture.cells[5].codePoint = 'y';
    REQUIRE(fixture.frame(1, 42'000));
    REQUIRE(fixture.type(2, 'b', 43'000));
    auto overlay = fixture.predictor.overlay(43'000);
    REQUIRE(overlay.cells.size() == 3);
    CHECK_EQ(fixture.cells[4].codePoint, static_cast<std::uint32_t>('x'));
    fixture.cells[4].codePoint = 'b';
    fixture.cells[5].codePoint = 'x';
    fixture.cells[6].codePoint = 'y';
    fixture.snapshot.cursorColumn = 5;
    REQUIRE(fixture.frame(2, 83'000));
    REQUIRE(fixture.key(3, functional(Keys::VirtualKey::Backspace), 84'000));
    overlay = fixture.predictor.overlay(84'000);
    REQUIRE(overlay.cells.size() == 3);
    REQUIRE(overlay.cursor);
    CHECK_EQ(overlay.cursor->column, 4);
    fixture.cells[4].codePoint = 'x';
    fixture.cells[5].codePoint = 'y';
    fixture.cells[6] = defaultBlankCell;
    fixture.snapshot.cursorColumn = 4;
    REQUIRE(fixture.frame(3, 124'000));
    CHECK(fixture.predictor.hasConfirmedEpoch());
}

TEST(aRefutedHiddenOrDisplayedPredictionDropsTheWholeEpoch)
{
    for (int isConfirmed = 0; isConfirmed < 2; ++isConfirmed) {
        Fixture fixture;
        if (isConfirmed)
            fixture.confirmEpoch();
        const auto sequence = isConfirmed ? 2u : 1u;
        const auto now = isConfirmed ? 42'000u : 1'000u;
        REQUIRE(fixture.type(sequence, 'q', now));
        fixture.cells[static_cast<std::size_t>(fixture.snapshot.cursorColumn)].codePoint = 'z';
        ++fixture.snapshot.cursorColumn;
        const auto frame = fixture.frame(sequence, now + 40'000);
        REQUIRE(frame);
        CHECK(frame->outcome == PredictionFrameOutcome::Refuted);
        CHECK(!fixture.predictor.hasConfirmedEpoch());
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
        CHECK(fixture.predictor.overlay(now + 1'000'000).cells.empty());
    }
}

TEST(allHardContextVetoesErasePendingBytesAndRequireFreshConfirmation)
{
    for (int veto = 0; veto < 10; ++veto) {
        Fixture fixture;
        fixture.confirmEpoch();
        REQUIRE(fixture.type(2, 's', 42'000));
        switch (veto) {
        case 0:
            fixture.snapshot.hasEchoContext = false;
            break;
        case 1:
            fixture.snapshot.echoContext.flags.set(Sync::EchoFlag::SecureEntry, true);
            break;
        case 2:
            fixture.snapshot.echoContext.flags.set(Sync::EchoFlag::AtPrompt, false);
            break;
        case 3:
            fixture.snapshot.echoContext.flags.set(Sync::EchoFlag::ForeignInput, true);
            break;
        case 4:
            fixture.snapshot.screen = Modes::ScreenChoice::Alternate;
            break;
        case 5:
            fixture.snapshot.hasExplicitSecureInput = true;
            break;
        case 6:
            fixture.snapshot.isCursorVisible = false;
            break;
        case 7:
            fixture.snapshot.canPredictSimpleEditing = false;
            break;
        case 8:
            fixture.snapshot.inputPen.attributes.set(VT::Attribute::Hidden, true);
            break;
        case 9:
            fixture.snapshot.echoContext.flags = Sync::EchoFlags::fromRaw(0x80);
            break;
        }
        REQUIRE(fixture.frame(1, 43'000));
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
        CHECK(!fixture.predictor.hasConfirmedEpoch());
        const auto outcome = fixture.type(3, 'e', 44'000);
        REQUIRE(outcome);
        CHECK(*outcome == PredictionInputOutcome::Vetoed);
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
        CHECK(fixture.predictor.overlay(1'000'000).cells.empty());
        CHECK(!fixture.predictor.nextWakeDelay(1'000'000));
    }
}

TEST(lateSafeContextNeverDisplaysPreviouslySuppressedInput)
{
    Fixture fixture;
    fixture.snapshot.hasEchoContext = false;
    REQUIRE(fixture.frame(0, 1'000));
    REQUIRE(fixture.type(1, 's', 2'000));
    REQUIRE(fixture.type(2, 'e', 3'000));
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    fixture.snapshot.hasEchoContext = true;
    REQUIRE(fixture.frame(0, 4'000));
    REQUIRE(fixture.type(3, 'c', 5'000));
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    REQUIRE(fixture.frame(3, 6'000));
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    CHECK(fixture.predictor.overlay(1'000'000).cells.empty());
    REQUIRE(fixture.type(4, 'r', 1'001'000));
    CHECK(!fixture.predictor.hasConfirmedEpoch());
}

TEST(contextPromptStructuralAndCursorChangesInvalidateConfidence)
{
    for (int change = 0; change < 5; ++change) {
        Fixture fixture;
        fixture.confirmEpoch();
        REQUIRE(fixture.type(2, 'b', 42'000));
        auto frameClass = Sync::FrameClass::Settled;
        switch (change) {
        case 0:
            ++fixture.snapshot.echoContext.modesRevision;
            break;
        case 1:
            ++fixture.snapshot.echoContext.foregroundGeneration;
            break;
        case 2:
            ++fixture.snapshot.promptEpoch;
            break;
        case 3:
            frameClass = Sync::FrameClass::Structural;
            break;
        case 4:
            fixture.snapshot.cursorColumn = 1;
            break;
        }
        REQUIRE(fixture.frame(1, 43'000, frameClass));
        CHECK(!fixture.predictor.hasConfirmedEpoch());
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
        CHECK(fixture.predictor.overlay(1'000'000).cells.empty());
    }
}

TEST(anUnmodelledKeyCannotExposeASyntheticSecretOrUseStaleConfirmation)
{
    Fixture fixture;
    fixture.confirmEpoch();
    const auto arrow = fixture.key(2, functional(Keys::VirtualKey::UpArrow), 42'000);
    REQUIRE(arrow);
    CHECK(*arrow == PredictionInputOutcome::Invalidated);
    REQUIRE(fixture.type(3, 's', 43'000));
    REQUIRE(fixture.type(4, 'e', 44'000));
    REQUIRE(fixture.type(5, 'c', 45'000));
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    REQUIRE(fixture.frame(1, 46'000));
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    REQUIRE(fixture.frame(5, 47'000));
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    CHECK(fixture.predictor.overlay(1'000'000).cells.empty());
    REQUIRE(fixture.type(6, 'a', 1'001'000));
    fixture.cells[4].codePoint = 'a';
    fixture.snapshot.cursorColumn = 5;
    REQUIRE(fixture.frame(6, 1'041'000));
    CHECK(fixture.predictor.hasConfirmedEpoch());
}

TEST(nonKeyInputExplicitlyEndsTheEpochUnderItsOwnSequence)
{
    Fixture fixture;
    fixture.confirmEpoch();
    REQUIRE(fixture.predictor.unmodelledInput(2, 42'000));
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    REQUIRE(fixture.type(3, 'x', 43'000));
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    CHECK(!fixture.predictor.nextWakeDelay(43'000));
}

TEST(wideCombiningClusterAndUnmodelledKeyBatchesAreNeverSpeculated)
{
    for (int variant = 0; variant < 10; ++variant) {
        Fixture fixture;
        fixture.confirmEpoch();
        auto key = printable('x');
        switch (variant) {
        case 0:
            key = printable(0x4e2d);
            break;
        case 1:
            key = printable(0x0301);
            break;
        case 2:
            key = printable(0x200d);
            break;
        case 3:
            key.textLength = 2;
            key.textCodePoints[1] = 0x0301;
            break;
        case 4:
            key = functional(Keys::VirtualKey::Tab);
            break;
        case 5:
            key = functional(Keys::VirtualKey::Return);
            break;
        case 6:
            key.modifiers = Keys::Keymap::keymapModifierCtrl;
            break;
        case 7:
            key.wasModified = true;
            break;
        case 8:
            key.textLength = Keys::maxKeyEventTextLength + 1;
            break;
        case 9:
            key = printable(0xd800);
            break;
        }
        const auto result = fixture.key(2, key, 42'000);
        REQUIRE(result);
        CHECK(*result == PredictionInputOutcome::Invalidated);
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
        CHECK(!fixture.predictor.hasConfirmedEpoch());
    }
}

TEST(singleWidthUnicodeAndShiftedCommittedTextUseTheAuthoritativeWidthPolicy)
{
    Fixture fixture;
    auto key = printable(0x00e9);
    key.modifiers = Keys::Keymap::keymapModifierShift;
    key.wasModified = true;
    REQUIRE(fixture.key(1, key, 1'000));
    CHECK_EQ(fixture.predictor.pendingEditCount(), 1u);
    fixture.cells[3].codePoint = 0x00e9;
    fixture.snapshot.cursorColumn = 4;
    REQUIRE(fixture.frame(1, 41'000));
    CHECK(fixture.predictor.hasConfirmedEpoch());
}

TEST(unknownWideAndDecoratedCellsCannotBeUsedAsPredictionEvidence)
{
    for (int variant = 0; variant < 6; ++variant) {
        Fixture fixture;
        switch (variant) {
        case 0:
            fixture.known[3] = 0;
            break;
        case 1:
            fixture.cells[3].pen.attributes.set(VT::Attribute::Wide, true);
            break;
        case 2:
            fixture.cells[3].pen.attributes.set(VT::Attribute::WideContinuation, true);
            break;
        case 3:
            fixture.cells[3].setCombiningLength(1);
            break;
        case 4:
            fixture.cells[3].setClusterID(1);
            break;
        case 5:
            fixture.cells[3].pen.extendedAttributes.setTextSizeSpecID(1);
            break;
        }
        REQUIRE(fixture.type(1, 'a', 1'000));
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
        CHECK(!fixture.predictor.hasConfirmedEpoch());
    }
    Fixture unknownConfirmation;
    REQUIRE(unknownConfirmation.type(1, 'a', 1'000));
    unknownConfirmation.cells[3].codePoint = 'a';
    unknownConfirmation.snapshot.cursorColumn = 4;
    unknownConfirmation.known[3] = 0;
    const auto result = unknownConfirmation.frame(1, 41'000);
    REQUIRE(result);
    CHECK(result->outcome == PredictionFrameOutcome::Refuted);
    CHECK(!unknownConfirmation.predictor.hasConfirmedEpoch());
}

TEST(lastColumnAndCrossRowBackspaceAreUnmodelledBoundaries)
{
    Fixture edge;
    edge.snapshot.cursorColumn = edge.snapshot.columns - 1;
    REQUIRE(edge.frame(0, 1'000));
    REQUIRE(edge.type(1, 'a', 2'000));
    CHECK_EQ(edge.predictor.pendingEditCount(), 0u);
    Fixture backspace;
    backspace.snapshot.cursorColumn = 0;
    REQUIRE(backspace.frame(0, 1'000));
    REQUIRE(backspace.key(1, functional(Keys::VirtualKey::Backspace), 2'000));
    CHECK_EQ(backspace.predictor.pendingEditCount(), 0u);

    Fixture inward;
    inward.snapshot.cursorColumn = inward.snapshot.columns - 1;
    inward.cells[inward.cells.size() - 2].codePoint = 'a';
    REQUIRE(inward.frame(0, 1'000));
    REQUIRE(inward.key(1, functional(Keys::VirtualKey::Backspace), 2'000));
    CHECK_EQ(inward.predictor.pendingEditCount(), 1u);
}

TEST(resourceBoundsRefuseWholeBatchesWithoutKeepingAPartialPrediction)
{
    Fixture fixture(2);
    const std::array keys { printable('a'), printable('b') };
    REQUIRE(fixture.predictor.predictKeys(1, keys, fixture.snapshot, 1'000));
    CHECK_EQ(fixture.predictor.pendingEditCount(), 2u);
    const auto excess = fixture.type(2, 'c', 2'000);
    REQUIRE(!excess);
    CHECK(excess.error() == PredictionError::CapacityExceeded);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    CHECK(fixture.predictor.overlay(1'000'000).cells.empty());
    Predictor invalid;
    CHECK(!invalid.configure(0, testPolicy));
    CHECK(!invalid.configure(maxPredictionEdits + 1, testPolicy));
}

TEST(foreignScopesAndOldConnectionEpochsNeverConfirmPendingText)
{
    for (int scope = 0; scope < 5; ++scope) {
        Fixture fixture;
        REQUIRE(fixture.type(1, 'a', 1'000));
        fixture.cells[3].codePoint = 'a';
        fixture.snapshot.cursorColumn = 4;
        switch (scope) {
        case 0:
            ++fixture.snapshot.identity.hostInstance.low;
            break;
        case 1:
            ++fixture.snapshot.identity.clientInstance.low;
            break;
        case 2:
            ++fixture.snapshot.identity.pane.low;
            break;
        case 3:
            ++fixture.snapshot.identity.connectionEpoch;
            break;
        case 4:
            ++fixture.snapshot.identity.attachment.low;
            break;
        }
        const auto result = fixture.frame(1, 41'000);
        REQUIRE(!result);
        CHECK(result.error() == PredictionError::ForeignIdentity);
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
        CHECK(!fixture.predictor.hasConfirmedEpoch());
    }
}

TEST(staleFramesFutureReceiptsAndDuplicateInputCannotSupplyConfidence)
{
    Fixture future;
    REQUIRE(future.type(1, 'a', 1'000));
    CHECK(!future.frame(2, 2'000));
    CHECK_EQ(future.predictor.pendingEditCount(), 0u);
    Fixture duplicate;
    REQUIRE(duplicate.type(1, 'a', 1'000));
    CHECK(!duplicate.type(1, 'a', 2'000));
    CHECK_EQ(duplicate.predictor.pendingEditCount(), 0u);
    Fixture gap;
    CHECK(!gap.type(2, 'a', 1'000));
    Fixture stale;
    stale.confirmEpoch();
    REQUIRE(stale.type(2, 'b', 42'000));
    --stale.snapshot.generation;
    const auto result = stale.predictor.acceptFrame(stale.snapshot, Sync::FrameClass::Settled, 43'000);
    REQUIRE(!result);
    CHECK_EQ(stale.predictor.pendingEditCount(), 0u);
    CHECK(!stale.predictor.hasConfirmedEpoch());
}

TEST(sequenceExhaustionAndBackwardsClocksNeverWrapIntoAValidEpoch)
{
    Fixture exhausted;
    exhausted.predictor.invalidate(PredictionInvalidation::Reconnect);
    exhausted.snapshot.inputSettled = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(exhausted.frame(exhausted.snapshot.inputSettled, 1'000));
    CHECK(!exhausted.type(0, 'a', 2'000));
    CHECK_EQ(exhausted.predictor.pendingEditCount(), 0u);
    Fixture clock;
    clock.confirmEpoch();
    REQUIRE(clock.type(2, 'b', 42'000));
    const auto result = clock.type(3, 'c', 41'999);
    REQUIRE(!result);
    CHECK(result.error() == PredictionError::ClockRegressed);
    CHECK_EQ(clock.predictor.pendingEditCount(), 0u);
    CHECK(!clock.predictor.hasConfirmedEpoch());
    Fixture renderingClock;
    renderingClock.confirmEpoch();
    REQUIRE(renderingClock.type(2, 'b', 42'000));
    CHECK(renderingClock.predictor.overlay(41'999).cells.empty());
    CHECK_EQ(renderingClock.predictor.pendingEditCount(), 0u);
}

TEST(aReconnectionRequiresExplicitInvalidationAndFreshIdentity)
{
    Fixture fixture;
    fixture.confirmEpoch();
    REQUIRE(fixture.type(2, 'b', 42'000));
    fixture.predictor.invalidate(PredictionInvalidation::Reconnect);
    ++fixture.snapshot.identity.connectionEpoch;
    REQUIRE(fixture.frame(1, 43'000));
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    REQUIRE(fixture.type(2, 'c', 44'000));
    CHECK(fixture.predictor.overlay(1'000'000).cells.empty());
}

TEST(acknowledgingAWholeKeyBatchCountsOneSendToSettledSample)
{
    Fixture fixture;
    const std::array keys { printable('a'), printable('b') };
    REQUIRE(fixture.predictor.predictKeys(1, keys, fixture.snapshot, 1'000));
    fixture.cells[3].codePoint = 'a';
    fixture.cells[4].codePoint = 'b';
    fixture.snapshot.cursorColumn = 5;
    const auto frame = fixture.frame(1, 41'000);
    REQUIRE(frame);
    CHECK_EQ(frame->settledInputCount, 1u);
    CHECK_EQ(frame->echoLatencyMicroseconds.value_or(0), 40'000u);
    CHECK(fixture.predictor.hasConfirmedEpoch());
}

TEST(keyReleasesDoNotInventTextOrDestroyAConfirmedEpoch)
{
    Fixture fixture;
    fixture.confirmEpoch();
    auto release = printable('a');
    release.eventType = Keys::KeyEventType::Release;
    const auto result = fixture.key(2, release, 42'000);
    REQUIRE(result);
    CHECK(*result == PredictionInputOutcome::Ignored);
    CHECK(fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    CHECK(!fixture.predictor.nextWakeDelay(42'000));
}

TEST(disablingDisplayKeepsPredictionPrivateAndEndingAPaneErasesIt)
{
    Fixture fixture;
    REQUIRE(fixture.predictor.configure(maxPredictionEdits, std::nullopt));
    REQUIRE(fixture.frame(0, 1'000));
    REQUIRE(fixture.type(1, 'a', 2'000));
    fixture.cells[3].codePoint = 'a';
    fixture.snapshot.cursorColumn = 4;
    REQUIRE(fixture.frame(1, 42'000));
    REQUIRE(fixture.type(2, 'b', 43'000));
    CHECK(fixture.predictor.overlay(10'000'000).cells.empty());
    CHECK(!fixture.predictor.nextWakeDelay(10'000'000));
    fixture.predictor.invalidate(PredictionInvalidation::PaneEnded);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
}

TEST(aGapRetiresConfidenceButCanRecoverAfterTheAdmittedInputSettles)
{
    Fixture fixture;
    const auto gap = fixture.type(3, 'a', 1'000);
    REQUIRE(!gap);
    CHECK(gap.error() == PredictionError::InvalidSequence);
    REQUIRE(fixture.frame(3, 41'000));
    REQUIRE(fixture.type(4, 'b', 42'000));
    CHECK_EQ(fixture.predictor.pendingEditCount(), 1u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
}

TEST(theLastInputSequenceCanSettleButCannotWrap)
{
    Fixture fixture;
    fixture.predictor.invalidate(PredictionInvalidation::Reconnect);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(fixture.frame(maximum - 1, 1'000));
    REQUIRE(fixture.type(maximum, 'a', 2'000));
    fixture.cells[3].codePoint = 'a';
    fixture.snapshot.cursorColumn = 4;
    REQUIRE(fixture.frame(maximum, 42'000));
    CHECK(fixture.predictor.hasConfirmedEpoch());
    const auto wrapped = fixture.type(0, 'b', 43'000);
    REQUIRE(!wrapped);
    CHECK(wrapped.error() == PredictionError::InvalidSequence);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
}

TEST(sameGenerationHardVetoesAndStructuralFramesStillErasePendingText)
{
    for (int variant = 0; variant < 2; ++variant) {
        Fixture fixture;
        fixture.confirmEpoch();
        REQUIRE(fixture.type(2, 'b', 42'000));
        auto frameClass = Sync::FrameClass::Settled;
        if (variant)
            frameClass = Sync::FrameClass::Structural;
        else
            fixture.snapshot.hasEchoContext = false;
        REQUIRE(fixture.predictor.acceptFrame(fixture.snapshot, frameClass, 43'000));
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
        CHECK(!fixture.predictor.hasConfirmedEpoch());
    }
}

TEST(aConfirmedFastLinkExposesOnlyTheStillPendingOverlayAtItsGlitchDeadline)
{
    Fixture fixture;
    REQUIRE(fixture.type(1, 'a', 1'000));
    fixture.cells[3].codePoint = 'a';
    fixture.snapshot.cursorColumn = 4;
    REQUIRE(fixture.frame(1, 6'000));
    CHECK(fixture.predictor.hasConfirmedEpoch());
    REQUIRE(fixture.type(2, 'b', 7'000));
    CHECK(fixture.predictor.overlay(256'999).cells.empty());
    const auto overlay = fixture.predictor.overlay(257'000);
    REQUIRE(overlay.cells.size() == 1);
    CHECK_EQ(overlay.cells[0].cell.codePoint, static_cast<std::uint32_t>('b'));
    CHECK(!overlay.treatment.isFlagged);
    CHECK_EQ(fixture.cells[4].codePoint, 0u);
}

TEST(malformedSnapshotsRefuseBeforeReadingAnyBorrowedCell)
{
    for (int variant = 0; variant < 10; ++variant) {
        Fixture fixture;
        REQUIRE(fixture.type(1, 'a', 1'000));
        switch (variant) {
        case 0:
            fixture.snapshot.rows = 0;
            break;
        case 1:
            fixture.snapshot.columns = -1;
            break;
        case 2:
            fixture.snapshot.cursorColumn = fixture.snapshot.columns;
            break;
        case 3:
            fixture.snapshot.cursorRow = -1;
            break;
        case 4:
            fixture.snapshot.row = {};
            break;
        case 5:
            fixture.snapshot.knownCells = std::span { fixture.known }.first(1);
            break;
        case 6:
            fixture.known[0] = 2;
            break;
        case 7:
            fixture.snapshot.identity.attachment = {};
            break;
        case 8:
            fixture.snapshot.promptEpoch = 0;
            break;
        case 9:
            fixture.snapshot.columns = std::numeric_limits<int>::max();
            break;
        }
        const auto result = fixture.frame(0, 2'000);
        REQUIRE(!result);
        CHECK(result.error() == PredictionError::InvalidSnapshot);
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    }
}

TEST(explicitOwnerInvalidationAlwaysDropsRenderedAndHiddenState)
{
    constexpr std::array reasons { PredictionInvalidation::PromptChanged, PredictionInvalidation::Resize,
        PredictionInvalidation::CursorMoved, PredictionInvalidation::SecureInput,
        PredictionInvalidation::UnmodelledInput, PredictionInvalidation::SettingChanged };
    for (const auto reason : reasons) {
        Fixture fixture;
        fixture.confirmEpoch();
        REQUIRE(fixture.type(2, 'b', 42'000));
        fixture.predictor.invalidate(reason);
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
        CHECK(!fixture.predictor.hasConfirmedEpoch());
        CHECK(fixture.predictor.overlay(43'000).cells.empty());
        CHECK(!fixture.predictor.nextWakeDelay(43'000));
    }
}

TEST(anEarlyUnsettledDeleteDoesNotRefuteItsSettledInsertion)
{
    Fixture fixture;
    fixture.confirmEpoch();
    REQUIRE(fixture.type(2, 'b', 42'000));
    REQUIRE(fixture.key(3, functional(Keys::VirtualKey::Backspace), 43'000));
    const auto earlyDelete = fixture.frame(2, 83'000);
    REQUIRE(earlyDelete);
    CHECK(earlyDelete->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(earlyDelete->settledInputCount, 1u);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 1u);
    CHECK(fixture.predictor.hasConfirmedEpoch());
    const auto settledDelete = fixture.frame(3, 123'000);
    REQUIRE(settledDelete);
    CHECK(settledDelete->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(settledDelete->settledInputCount, 1u);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
}

TEST(anEarlyDeleteErasingTheOnlySettledGlyphCannotConfirmAHiddenEpoch)
{
    Fixture fixture;
    REQUIRE(fixture.type(1, 'a', 1'000));
    REQUIRE(fixture.key(2, functional(Keys::VirtualKey::Backspace), 2'000));
    const auto earlyDelete = fixture.frame(1, 41'000);
    REQUIRE(earlyDelete);
    CHECK(earlyDelete->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 1u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    const auto repeatedEarlyDelete = fixture.frame(1, 42'000);
    REQUIRE(repeatedEarlyDelete);
    CHECK(repeatedEarlyDelete->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(repeatedEarlyDelete->settledInputCount, 0u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    const auto settledDelete = fixture.frame(2, 81'000);
    REQUIRE(settledDelete);
    CHECK(settledDelete->outcome == PredictionFrameOutcome::Reconciled);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
}

TEST(ambiguousEarlyOverwriteCannotBorrowCreditFromTheSameUnsettledGlyph)
{
    Fixture fixture;
    REQUIRE(fixture.type(1, 'a', 1'000));
    REQUIRE(fixture.key(2, functional(Keys::VirtualKey::Backspace), 2'000));
    REQUIRE(fixture.type(3, 'a', 3'000));
    fixture.cells[3].codePoint = 'a';
    fixture.snapshot.cursorColumn = 4;
    const auto first = fixture.frame(1, 41'000);
    REQUIRE(first);
    CHECK(first->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 2u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    const auto second = fixture.frame(2, 42'000);
    REQUIRE(second);
    CHECK(second->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 1u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    const auto third = fixture.frame(3, 43'000);
    REQUIRE(third);
    CHECK(third->outcome == PredictionFrameOutcome::Reconciled);
    CHECK(fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
}

TEST(earlyOverlappingTailShiftsPreserveOriginalEvidenceAcrossSettlement)
{
    Fixture fixture;
    fixture.cells[3].codePoint = 'x';
    fixture.cells[4].codePoint = 'y';
    REQUIRE(fixture.type(1, 'a', 1'000));
    REQUIRE(fixture.key(2, functional(Keys::VirtualKey::Backspace), 2'000));
    REQUIRE(fixture.type(3, 'b', 3'000));
    fixture.cells[3].codePoint = 'b';
    fixture.cells[4].codePoint = 'x';
    fixture.cells[5].codePoint = 'y';
    fixture.snapshot.cursorColumn = 4;
    const auto first = fixture.frame(1, 41'000);
    REQUIRE(first);
    CHECK(first->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 6u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    const auto second = fixture.frame(2, 42'000);
    REQUIRE(second);
    CHECK(second->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 3u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    const auto third = fixture.frame(3, 43'000);
    REQUIRE(third);
    CHECK(third->outcome == PredictionFrameOutcome::Reconciled);
    CHECK(fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
}

TEST(aSurvivingSettledGlyphCanConfirmDespiteALaterNonoverlappingEarlyEcho)
{
    Fixture fixture;
    REQUIRE(fixture.type(1, 'a', 1'000));
    REQUIRE(fixture.type(2, 'b', 2'000));
    fixture.cells[3].codePoint = 'a';
    fixture.cells[4].codePoint = 'b';
    fixture.snapshot.cursorColumn = 5;
    const auto result = fixture.frame(1, 41'000);
    REQUIRE(result);
    CHECK(result->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(result->settledInputCount, 1u);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 1u);
    CHECK(fixture.predictor.hasConfirmedEpoch());
    const auto overlay = fixture.predictor.overlay(42'000);
    REQUIRE(overlay.cells.size() == 1);
    CHECK_EQ(overlay.cells[0].column, 4);
    CHECK_EQ(overlay.cells[0].cell.codePoint, static_cast<std::uint32_t>('b'));
}

TEST(reconciliationNeverMixesIncompatiblePerCellPrefixes)
{
    Fixture fixture;
    REQUIRE(fixture.type(1, 'a', 1'000));
    REQUIRE(fixture.type(2, 'b', 2'000));
    REQUIRE(fixture.key(3, functional(Keys::VirtualKey::Backspace), 3'000));
    fixture.cells[4].codePoint = 'b';
    fixture.snapshot.cursorColumn = 4;
    const auto result = fixture.frame(1, 41'000);
    REQUIRE(result);
    CHECK(result->outcome == PredictionFrameOutcome::Refuted);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
}

TEST(partialEarlyEchoWithinOneNumberedKeyBatchDoesNotRefuteOrConfirm)
{
    Fixture fixture;
    const std::array keys { printable('a'), printable('b') };
    REQUIRE(fixture.predictor.predictKeys(1, keys, fixture.snapshot, 1'000));
    fixture.cells[3].codePoint = 'a';
    fixture.snapshot.cursorColumn = 4;
    const auto partial = fixture.frame(0, 6'000);
    REQUIRE(partial);
    CHECK(partial->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(partial->settledInputCount, 0u);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 2u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    fixture.cells[4].codePoint = 'b';
    fixture.snapshot.cursorColumn = 5;
    const auto settled = fixture.frame(1, 41'000);
    REQUIRE(settled);
    CHECK(settled->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(settled->settledInputCount, 1u);
    CHECK(fixture.predictor.hasConfirmedEpoch());
}

TEST(oneNumberedBatchCanAppendThenCancelWithoutEarningEchoCredit)
{
    Fixture fixture;
    const std::array keys { printable('a'), functional(Keys::VirtualKey::Backspace) };
    REQUIRE(fixture.predictor.predictKeys(1, keys, fixture.snapshot, 1'000));
    fixture.cells[3].codePoint = 'a';
    fixture.snapshot.cursorColumn = 4;
    const auto partial = fixture.frame(0, 6'000);
    REQUIRE(partial);
    CHECK(partial->outcome == PredictionFrameOutcome::Reconciled);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
    fixture.cells[3] = defaultBlankCell;
    fixture.snapshot.cursorColumn = 3;
    const auto cancelled = fixture.frame(1, 41'000);
    REQUIRE(cancelled);
    CHECK(cancelled->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(cancelled->settledInputCount, 1u);
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
}

TEST(releaseOnlyBatchesUseTheWireWorkBoundNotThePendingEditAllowance)
{
    Fixture fixture(1);
    fixture.confirmEpoch();
    auto release = printable('a');
    release.eventType = Keys::KeyEventType::Release;
    const std::array releases { release, release };
    const auto result = fixture.predictor.predictKeys(2, releases, fixture.snapshot, 42'000);
    REQUIRE(result);
    CHECK(*result == PredictionInputOutcome::Ignored);
    CHECK(fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    std::array<Keys::KeyEvent, Sync::maxInputEventsPerFrame + 1> excessive;
    excessive.fill(release);
    const auto refused = fixture.predictor.predictKeys(3, excessive, fixture.snapshot, 43'000);
    REQUIRE(!refused);
    CHECK(refused.error() == PredictionError::CapacityExceeded);
    CHECK(!fixture.predictor.hasConfirmedEpoch());
}

TEST(aWrittenSpaceEraseReconcilesWithoutBlankCreditOrAuthoritativeNormalization)
{
    for (int variant = 0; variant < 2; ++variant) {
        Fixture fixture;
        std::uint64_t sequence = 1;
        if (variant) {
            fixture.confirmEpoch();
            sequence = 2;
        } else {
            fixture.cells[2].codePoint = 'a';
        }
        const int erasedColumn = fixture.snapshot.cursorColumn - 1;
        REQUIRE(fixture.key(sequence, functional(Keys::VirtualKey::Backspace), 42'000));
        fixture.cells[static_cast<std::size_t>(erasedColumn)].codePoint = ' ';
        fixture.snapshot.cursorColumn = erasedColumn;
        const auto before = fixture.cells;
        const auto result = fixture.frame(sequence, 82'000);
        REQUIRE(result);
        CHECK(result->outcome == PredictionFrameOutcome::Reconciled);
        CHECK_EQ(result->settledInputCount, 1u);
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
        CHECK(fixture.predictor.hasConfirmedEpoch() == (variant != 0));
        CHECK(std::memcmp(before.data(), fixture.cells.data(), sizeof(before)) == 0);
        CHECK_EQ(fixture.cells[static_cast<std::size_t>(erasedColumn)].codePoint, static_cast<std::uint32_t>(' '));
    }
}

TEST(blankEquivalenceRequiresIdenticalMetadataAndKnownSimpleCells)
{
    for (int variant = 0; variant < 6; ++variant) {
        Fixture fixture;
        fixture.cells[2].codePoint = 'a';
        REQUIRE(fixture.key(1, functional(Keys::VirtualKey::Backspace), 1'000));
        fixture.cells[2].codePoint = ' ';
        fixture.snapshot.cursorColumn = 2;
        switch (variant) {
        case 0:
            fixture.cells[2].pen.foregroundIndex = 1;
            break;
        case 1:
            fixture.cells[2].pen.attributes.set(VT::Attribute::Bold, true);
            break;
        case 2:
            fixture.cells[2].setLinkID(1);
            break;
        case 3:
            fixture.cells[2].setClusterID(1);
            break;
        case 4:
            fixture.cells[2].combining[0] = 'a';
            break;
        case 5:
            fixture.known[2] = 0;
            break;
        }
        const auto result = fixture.frame(1, 41'000);
        REQUIRE(result);
        CHECK(result->outcome == PredictionFrameOutcome::Refuted);
        CHECK(!fixture.predictor.hasConfirmedEpoch());
        CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    }
}

TEST(sameGlyphWithDifferentPenOrStyleRefutesHiddenAndConfirmedEpochs)
{
    for (int confirmed = 0; confirmed < 2; ++confirmed) {
        for (int variant = 0; variant < 8; ++variant) {
            Fixture fixture;
            std::uint64_t sequence = 1;
            if (confirmed) {
                fixture.confirmEpoch();
                sequence = 2;
            }
            const int column = fixture.snapshot.cursorColumn;
            REQUIRE(fixture.type(sequence, 'b', 42'000));
            CHECK(fixture.predictor.overlay(42'000).cells.empty() == !confirmed);
            auto& echoed = fixture.cells[static_cast<std::size_t>(column)];
            echoed.codePoint = 'b';
            switch (variant) {
            case 0:
                echoed.pen.foregroundIndex = 1;
                break;
            case 1:
                echoed.pen.backgroundIndex = 2;
                break;
            case 2:
                echoed.pen.attributes.set(VT::Attribute::Bold, true);
                break;
            case 3:
                echoed.pen.attributes.set(VT::Attribute::Reverse, true);
                break;
            case 4:
                echoed.pen.attributes.set(VT::Attribute::Italic, true);
                break;
            case 5:
                echoed.pen.attributes.set(VT::Attribute::Underline, true);
                penSetUnderlineShape(&echoed.pen, VT::underlineDouble);
                break;
            case 6:
                penSetFontStyle(&echoed.pen, VT::fontFraktur);
                break;
            case 7:
                echoed.pen.foregroundIndex = 1;
                fixture.snapshot.inputPen = echoed.pen;
                break;
            }
            fixture.snapshot.cursorColumn = column + 1;
            const auto before = fixture.cells;
            const auto result = fixture.frame(sequence, 82'000);
            REQUIRE(result);
            CHECK(result->outcome == PredictionFrameOutcome::Refuted);
            CHECK(!fixture.predictor.hasConfirmedEpoch());
            CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
            CHECK(fixture.predictor.overlay(83'000).cells.empty());
            CHECK(!fixture.predictor.nextWakeDelay(83'000));
            CHECK(std::memcmp(before.data(), fixture.cells.data(), sizeof(before)) == 0);
        }
    }
}

TEST(aMatchingCompleteCellWithANondefaultInputPenCanConfirmWithoutModelMutation)
{
    Fixture fixture;
    fixture.snapshot.inputPen.foregroundIndex = 1;
    fixture.snapshot.inputPen.backgroundIndex = 2;
    fixture.snapshot.inputPen.attributes.set(VT::Attribute::Bold, true);
    fixture.snapshot.inputPen.attributes.set(VT::Attribute::Reverse, true);
    fixture.snapshot.inputPen.attributes.set(VT::Attribute::Underline, true);
    penSetUnderlineShape(&fixture.snapshot.inputPen, VT::underlineDouble);
    penSetFontStyle(&fixture.snapshot.inputPen, VT::fontFraktur);
    REQUIRE(fixture.type(1, 'b', 1'000));
    fixture.cells[3].codePoint = 'b';
    fixture.cells[3].pen = fixture.snapshot.inputPen;
    fixture.snapshot.cursorColumn = 4;
    const auto before = fixture.cells;
    const auto result = fixture.frame(1, 41'000);
    REQUIRE(result);
    CHECK(result->outcome == PredictionFrameOutcome::Reconciled);
    CHECK_EQ(result->settledInputCount, 1u);
    CHECK(fixture.predictor.hasConfirmedEpoch());
    CHECK_EQ(fixture.predictor.pendingEditCount(), 0u);
    CHECK(std::memcmp(before.data(), fixture.cells.data(), sizeof(before)) == 0);
}

UBIQUITTY_TEST_MAIN()
