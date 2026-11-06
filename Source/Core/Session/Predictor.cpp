// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/Predictor.h"

#include "Core/Keys/KeyTranslation.h"
#include "Core/Keys/Keymap.h"
#include "Core/Unicode/GraphemeBreak.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace Ubiquitty::Session {
namespace {

/// Erases owned speculative bytes even when the object is about to die.
void eraseBytes(std::span<std::byte> bytes)
{
    // A volatile byte view writes object storage without invoking Cell mutation.
    volatile std::byte* destination = bytes.data();
    for (std::size_t index = 0; index < bytes.size(); ++index)
        destination[index] = std::byte { 0 };
}

bool isPlainScalar(std::uint32_t codePoint, Unicode::Width::AmbiguousWidth ambiguous)
{
    if (codePoint < Keys::Keymap::minPrintableASCII || (codePoint >= 0x7f && codePoint <= 0x9f)
        || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff))
        return false;
    return Unicode::Width::forCodePoint(codePoint, ambiguous) == 1
        && !(Unicode::graphemeBreakRawByte(codePoint) & Unicode::detail::propGraphemeRelevantMask);
}

bool matchesVisibleCell(const Cell& expected, const Cell& actual)
{
    const auto isBlank = [](std::uint32_t codePoint) { return !codePoint || codePoint == Keys::Keymap::minPrintableASCII; };
    if (expected.codePoint != actual.codePoint && (!isBlank(expected.codePoint) || !isBlank(actual.codePoint)))
        return false;
    // Matching glyphs alone are insufficient: every pen, attribute, link and
    // cluster byte must agree before the projected cell can be reconciled.
    // A simple erase may write a space instead of an unwritten blank. This
    // rendering-only equivalence neither changes authoritative extent nor
    // supplies nonblank evidence.
    const auto expectedMetadata = std::as_bytes(std::span { &expected, 1 }).subspan(sizeof(expected.codePoint));
    const auto actualMetadata = std::as_bytes(std::span { &actual, 1 }).subspan(sizeof(actual.codePoint));
    return std::ranges::equal(expectedMetadata, actualMetadata);
}

} // namespace

Predictor::~Predictor()
{
    erasePending();
}

std::expected<void, PredictionError> Predictor::configure(std::size_t editLimit, std::optional<PredictionExposurePolicy> policy)
{
    invalidate(PredictionInvalidation::SettingChanged);
    m_editLimit = 0;
    if (!editLimit || editLimit > maxPredictionEdits)
        return std::unexpected(PredictionError::InvalidLimits);
    if (!m_exposure.configure(policy))
        return std::unexpected(PredictionError::InvalidExposurePolicy);
    m_editLimit = editLimit;
    return {};
}

bool Predictor::isValidSnapshot(const PredictionSnapshot& snapshot)
{
    if (snapshot.identity.hostInstance == Protocol::Sync::ID128 {}
        || snapshot.identity.clientInstance == Protocol::Sync::ID128 {}
        || snapshot.identity.attachment == Protocol::Sync::ID128 {}
        || snapshot.identity.pane == Protocol::Sync::ID128 {} || !snapshot.identity.connectionEpoch
        || !snapshot.generation || !snapshot.promptEpoch
        || snapshot.rows <= 0 || snapshot.rows > maxRows || snapshot.columns <= 0 || snapshot.columns > maxColumns
        || snapshot.cursorRow < 0 || snapshot.cursorRow >= snapshot.rows
        || snapshot.cursorColumn < 0 || snapshot.cursorColumn >= snapshot.columns
        || snapshot.row.size() != static_cast<std::size_t>(snapshot.columns)
        || (!snapshot.knownCells.empty() && snapshot.knownCells.size() != snapshot.row.size())
        || (snapshot.screen != Modes::ScreenChoice::Primary && snapshot.screen != Modes::ScreenChoice::Alternate)
        || (snapshot.ambiguousWidth != Unicode::Width::AmbiguousWidth::Narrow && snapshot.ambiguousWidth != Unicode::Width::AmbiguousWidth::Wide))
        return false;
    return std::ranges::all_of(snapshot.knownCells, [](std::uint8_t known) { return known <= 1; });
}

bool Predictor::isSafeContext(const PredictionSnapshot& snapshot)
{
    using Protocol::Sync::EchoFlag;
    constexpr auto knownFlags = std::to_underlying(EchoFlag::SecureEntry) | std::to_underlying(EchoFlag::ForeignInput) | std::to_underlying(EchoFlag::AtPrompt);
    Cell input = defaultBlankCell;
    input.codePoint = 'a';
    input.pen = snapshot.inputPen;
    return snapshot.hasEchoContext && !snapshot.hasExplicitSecureInput && snapshot.isCursorVisible
        && snapshot.canPredictSimpleEditing && snapshot.screen == Modes::ScreenChoice::Primary
        && isSimpleCell(input, snapshot.ambiguousWidth)
        && !(snapshot.echoContext.flags.toRaw() & ~knownFlags)
        && snapshot.echoContext.flags.contains(EchoFlag::AtPrompt)
        && !snapshot.echoContext.flags.contains(EchoFlag::SecureEntry)
        && !snapshot.echoContext.flags.contains(EchoFlag::ForeignInput);
}

bool Predictor::isKnownCell(const PredictionSnapshot& snapshot, int column)
{
    assert(column >= 0 && column < snapshot.columns);
    return snapshot.knownCells.empty() || snapshot.knownCells[static_cast<std::size_t>(column)];
}

bool Predictor::isSimpleCell(const Cell& cell, Unicode::Width::AmbiguousWidth ambiguous)
{
    return !cell.combiningLength() && !cell.clusterID() && !cell.linkID()
        && !cell.pen.attributes.contains(VT::Attribute::Wide)
        && !cell.pen.attributes.contains(VT::Attribute::WideContinuation)
        && !cell.pen.attributes.contains(VT::Attribute::Hidden)
        && !cell.pen.attributes.contains(VT::Attribute::Emoji)
        && !cell.pen.attributes.contains(VT::Attribute::ForceEmojiPresentation)
        && !cell.pen.extendedAttributes.textSizeSpecID() && !penProportional(&cell.pen) && !penRichTextCovered(&cell.pen)
        && (!cell.codePoint || isPlainScalar(cell.codePoint, ambiguous));
}

bool Predictor::matchesContext(const PredictionSnapshot& snapshot) const
{
    return m_identity == snapshot.identity && m_echoContext == snapshot.echoContext
        && m_promptEpoch == snapshot.promptEpoch && m_rows == snapshot.rows && m_columns == snapshot.columns
        && m_row == snapshot.cursorRow && m_ambiguousWidth == snapshot.ambiguousWidth;
}

bool Predictor::isSameSnapshot(const PredictionSnapshot& snapshot) const
{
    return m_hasBaseline && matchesContext(snapshot) && m_generation == snapshot.generation
        && m_lastSettled == snapshot.inputSettled && m_authoritativeColumn == snapshot.cursorColumn;
}

std::expected<void, PredictionError> Predictor::noteTime(std::uint64_t now)
{
    if (m_lastNow && now < *m_lastNow) {
        invalidate(PredictionInvalidation::UnmodelledInput);
        m_lastNow = now;
        return std::unexpected(PredictionError::ClockRegressed);
    }
    m_lastNow = now;
    return {};
}

std::expected<void, PredictionError> Predictor::admitSequence(std::uint64_t inputSeq)
{
    if (!inputSeq || m_lastInputSeq == std::numeric_limits<std::uint64_t>::max()
        || inputSeq != m_lastInputSeq + 1) {
        invalidate(PredictionInvalidation::UnmodelledInput);
        // A gap means this observer missed admitted input, not that it may
        // reuse its numbers. Keep the gap as a barrier until a fresh frame.
        m_lastInputSeq = std::max(m_lastInputSeq, inputSeq);
        return std::unexpected(PredictionError::InvalidSequence);
    }
    m_lastInputSeq = inputSeq;
    return {};
}

bool Predictor::openEpoch()
{
    erasePending();
    m_hasConfirmedEpoch = false;
    if (m_epoch == std::numeric_limits<std::uint64_t>::max()) {
        m_isEpochExhausted = true;
        m_needsFreshFrame = true;
        return false;
    }
    ++m_epoch;
    return true;
}

void Predictor::erasePending()
{
    eraseBytes(std::as_writable_bytes(std::span { m_pending }));
    eraseBytes(std::as_writable_bytes(std::span { m_overlay }));
    m_pendingCount = 0;
    m_overlayCount = 0;
    m_pendingBaseColumn = m_authoritativeColumn;
}

void Predictor::takeBaseline(const PredictionSnapshot& snapshot)
{
    m_identity = snapshot.identity;
    m_echoContext = snapshot.echoContext;
    m_generation = snapshot.generation;
    m_promptEpoch = snapshot.promptEpoch;
    m_rows = snapshot.rows;
    m_columns = snapshot.columns;
    m_row = snapshot.cursorRow;
    m_authoritativeColumn = snapshot.cursorColumn;
    m_ambiguousWidth = snapshot.ambiguousWidth;
    m_lastSettled = snapshot.inputSettled;
    m_hasBaseline = true;
    m_hasIdentity = true;
    if (!m_pendingCount) {
        m_projectedColumn = m_authoritativeColumn;
        m_pendingBaseColumn = m_authoritativeColumn;
    }
}

Cell Predictor::projectedCell(const PredictionSnapshot& snapshot, int column) const
{
    assert(column >= 0 && column < snapshot.columns);
    for (std::size_t index = m_pendingCount; index; --index) {
        if (m_pending[index - 1].column == column)
            return m_pending[index - 1].after;
    }
    return snapshot.row[static_cast<std::size_t>(column)];
}

void Predictor::rebuildOverlay()
{
    eraseBytes(std::as_writable_bytes(std::span { m_overlay }));
    m_overlayCount = 0;
    for (std::size_t index = 0; index < m_pendingCount; ++index) {
        const auto& edit = m_pending[index];
        std::size_t destination = 0;
        while (destination < m_overlayCount && m_overlay[destination].column != edit.column)
            ++destination;
        assert(destination < m_overlay.size());
        m_overlay[destination] = { .row = m_row, .column = edit.column, .cell = edit.after };
        if (destination == m_overlayCount)
            ++m_overlayCount;
    }
}

std::expected<PredictionReconciliation, PredictionError> Predictor::acceptFrame(const PredictionSnapshot& snapshot, Protocol::Sync::FrameClass frameClass, std::uint64_t now)
{
    if (const auto time = noteTime(now); !time)
        return std::unexpected(time.error());
    if (!isValidSnapshot(snapshot) || std::to_underlying(frameClass) > std::to_underlying(Protocol::Sync::FrameClass::Structural)) {
        invalidate(PredictionInvalidation::UnmodelledInput);
        return std::unexpected(PredictionError::InvalidSnapshot);
    }
    if (m_isEpochExhausted)
        return std::unexpected(PredictionError::EpochExhausted);
    if (m_hasIdentity && m_identity != snapshot.identity) {
        invalidate(PredictionInvalidation::UnmodelledInput);
        return std::unexpected(PredictionError::ForeignIdentity);
    }
    const bool isNewIdentity = !m_hasIdentity;
    if (!isNewIdentity && (snapshot.generation < m_generation || snapshot.inputSettled < m_lastSettled || snapshot.inputSettled > m_lastInputSeq)) {
        invalidate(PredictionInvalidation::UnmodelledInput);
        return std::unexpected(PredictionError::InvalidSequence);
    }
    if (!isNewIdentity && snapshot.generation == m_generation && isSafeContext(snapshot)
        && frameClass != Protocol::Sync::FrameClass::Structural) {
        if (!isSameSnapshot(snapshot)) {
            invalidate(PredictionInvalidation::UnmodelledInput);
            return std::unexpected(PredictionError::InvalidSnapshot);
        }
        return PredictionReconciliation {};
    }
    if (isNewIdentity || !m_hasBaseline || !matchesContext(snapshot) || !isSafeContext(snapshot)
        || frameClass == Protocol::Sync::FrameClass::Structural || !m_editLimit) {
        if (!openEpoch())
            return std::unexpected(PredictionError::EpochExhausted);
        if (isNewIdentity) {
            m_lastInputSeq = snapshot.inputSettled;
            m_exposure.reset();
        }
        takeBaseline(snapshot);
        m_needsFreshFrame = !isSafeContext(snapshot) || frameClass == Protocol::Sync::FrameClass::Structural || snapshot.inputSettled < m_lastInputSeq;
        return PredictionReconciliation { .outcome = PredictionFrameOutcome::Reset };
    }
    if (m_needsFreshFrame) {
        takeBaseline(snapshot);
        if (snapshot.inputSettled >= m_lastInputSeq)
            m_needsFreshFrame = false;
        return PredictionReconciliation { .outcome = PredictionFrameOutcome::Reset };
    }

    std::size_t settledCount = 0;
    while (settledCount < m_pendingCount && m_pending[settledCount].inputSeq <= snapshot.inputSettled)
        ++settledCount;

    // The frame may already contain later, unsettled keys. Match a single
    // complete key prefix, not independent cells from incompatible prefixes.
    // The first before-value of each column is its logical pre-pending state,
    // even when a previous frame showed an early echo beyond settlement.
    std::array<std::size_t, maxPredictionEdits> columnForEdit {};
    std::array<std::size_t, maxPredictionEdits> lastWriter;
    lastWriter.fill(maxPredictionEdits);
    std::array<bool, maxPredictionEdits> canConfirm {};
    eraseBytes(std::as_writable_bytes(std::span { m_overlay }));
    m_overlayCount = 0;
    for (std::size_t index = 0; index < m_pendingCount; ++index) {
        const auto& edit = m_pending[index];
        std::size_t column = 0;
        while (column < m_overlayCount && m_overlay[column].column != edit.column)
            ++column;
        if (column == m_overlayCount) {
            assert(column < m_overlay.size());
            m_overlay[column] = { .row = m_row, .column = edit.column, .cell = edit.before };
            ++m_overlayCount;
        }
        columnForEdit[index] = column;
        // Erasure, spaces, identical text and copied tail cells prove no echo.
        // A changed nonblank glyph in a known blank may prove a settled key.
        canConfirm[index] = index < settledCount && edit.canConfirm
            && edit.after.codePoint > Keys::Keymap::minPrintableASCII
            && edit.after.codePoint != m_overlay[column].cell.codePoint;
    }

    bool didMatch = false;
    const auto matchPrefix = [&](std::size_t frontier) {
        if (frontier < settledCount)
            return;
        const int cursor = frontier ? m_pending[frontier - 1].cursorAfter : m_pendingBaseColumn;
        if (snapshot.cursorColumn != cursor)
            return;
        for (std::size_t column = 0; column < m_overlayCount; ++column) {
            const auto& expected = m_overlay[column];
            assert(expected.column >= 0 && expected.column < snapshot.columns);
            const auto& actual = snapshot.row[static_cast<std::size_t>(expected.column)];
            if (!isKnownCell(snapshot, expected.column) || !isSimpleCell(actual, snapshot.ambiguousWidth)
                || !matchesVisibleCell(expected.cell, actual))
                return;
        }
        didMatch = true;
        // Confidence must survive every matching interpretation. A later
        // unsettled overwrite is not evidence for an older settled key,
        // even if it writes the same glyph at the same cursor position.
        for (std::size_t index = 0; index < settledCount; ++index)
            canConfirm[index] = canConfirm[index] && lastWriter[columnForEdit[index]] == index;
    };
    matchPrefix(0);
    for (std::size_t index = 0; index < m_pendingCount; ++index) {
        m_overlay[columnForEdit[index]].cell = m_pending[index].after;
        lastWriter[columnForEdit[index]] = index;
        if (m_pending[index].endsKey)
            matchPrefix(index + 1);
    }
    if (!didMatch) {
        if (!openEpoch())
            return std::unexpected(PredictionError::EpochExhausted);
        takeBaseline(snapshot);
        m_needsFreshFrame = snapshot.inputSettled < m_lastInputSeq;
        return PredictionReconciliation { .outcome = PredictionFrameOutcome::Refuted };
    }

    PredictionReconciliation result { .outcome = PredictionFrameOutcome::Reconciled };
    std::uint64_t previousSequence = 0;
    for (std::size_t index = 0; index < settledCount; ++index) {
        const auto& edit = m_pending[index];
        if (edit.inputSeq != previousSequence) {
            ++result.settledInputCount;
            result.echoLatencyMicroseconds = now - edit.sentAt;
            m_exposure.noteEchoLatency(*result.echoLatencyMicroseconds);
            previousSequence = edit.inputSeq;
        }
    }
    m_hasConfirmedEpoch = m_hasConfirmedEpoch || std::ranges::any_of(canConfirm, [](bool value) { return value; }); // bool-ok: reads one stored evidence bit
    const int pendingBaseColumn = settledCount ? m_pending[settledCount - 1].cursorAfter : m_pendingBaseColumn;
    const auto remaining = m_pendingCount - settledCount;
    for (std::size_t index = 0; index < remaining; ++index)
        m_pending[index] = m_pending[index + settledCount];
    eraseBytes(std::as_writable_bytes(std::span { m_pending }).subspan(remaining * sizeof(Edit)));
    m_pendingCount = remaining;
    m_pendingBaseColumn = pendingBaseColumn;
    takeBaseline(snapshot);
    rebuildOverlay();
    return result;
}

std::expected<PredictionInputOutcome, PredictionError> Predictor::predictKeys(std::uint64_t inputSeq, std::span<const Keys::KeyEvent> keys, const PredictionSnapshot& snapshot, std::uint64_t now)
{
    if (const auto time = noteTime(now); !time)
        return std::unexpected(time.error());
    if (const auto sequence = admitSequence(inputSeq); !sequence)
        return std::unexpected(sequence.error());
    if (!isValidSnapshot(snapshot) || !isSameSnapshot(snapshot)) {
        invalidate(PredictionInvalidation::UnmodelledInput);
        return std::unexpected(PredictionError::InvalidSnapshot);
    }
    if (!m_editLimit || m_needsFreshFrame || !isSafeContext(snapshot)) {
        invalidate(PredictionInvalidation::SecureInput);
        return PredictionInputOutcome::Vetoed;
    }
    if (m_isEpochExhausted)
        return std::unexpected(PredictionError::EpochExhausted);
    const auto invalidateInput = [&]() -> std::expected<PredictionInputOutcome, PredictionError> {
        invalidate(PredictionInvalidation::UnmodelledInput);
        return PredictionInputOutcome::Invalidated;
    };
    const auto capacityFailure = [&]() -> std::expected<PredictionInputOutcome, PredictionError> {
        invalidate(PredictionInvalidation::UnmodelledInput);
        return std::unexpected(PredictionError::CapacityExceeded);
    };
    if (keys.empty())
        return invalidateInput();
    if (keys.size() > Protocol::Sync::maxInputEventsPerFrame)
        return capacityFailure();
    const auto append = [&](int column, const Cell& after, int cursorAfter, bool canConfirm) { // bool-ok: stores the edit's independent evidence bit
        assert(column >= 0 && column < snapshot.columns && m_pendingCount < m_editLimit);
        if (!m_pendingCount)
            m_pendingBaseColumn = m_projectedColumn;
        auto& edit = m_pending[m_pendingCount];
        edit = { .inputSeq = inputSeq, .sentAt = now, .column = column, .cursorAfter = cursorAfter, .before = projectedCell(snapshot, column), .after = after, .canConfirm = canConfirm };
        ++m_pendingCount;
    };
    const auto previousCount = m_pendingCount;
    for (const auto& key : keys) {
        if (key.eventType == Keys::KeyEventType::Release)
            continue;
        if ((key.eventType != Keys::KeyEventType::Press && key.eventType != Keys::KeyEventType::Repeat)
            || key.textLength < 0 || key.textLength > Keys::maxKeyEventTextLength)
            return invalidateInput();
        constexpr auto printableModifiers = Keys::Keymap::keymapModifierShift | Keys::Keymap::keymapModifierCapsLock | Keys::Keymap::keymapModifierNumLock;
        if ((key.modifiers & ~printableModifiers) || (key.wasModified && !key.modifiers))
            return invalidateInput();
        const bool isBackspace = key.virtualKey == Keys::VirtualKey::Backspace && !key.textLength && !(key.modifiers & Keys::Keymap::keymapModifierShift);
        if (!isBackspace && (key.virtualKey != Keys::VirtualKey::None || key.textLength != 1 || !isPlainScalar(key.textCodePoints[0], snapshot.ambiguousWidth)))
            return invalidateInput();
        if (isBackspace && !m_projectedColumn)
            return invalidateInput();
        if (m_projectedColumn < 0 || m_projectedColumn >= snapshot.columns
            || (!isBackspace && m_projectedColumn == snapshot.columns - 1))
            return invalidateInput();

        int end = m_projectedColumn;
        for (;;) {
            if (end >= snapshot.columns || (!isBackspace && end == snapshot.columns - 1) || !isKnownCell(snapshot, end))
                return invalidateInput();
            const auto cell = projectedCell(snapshot, end);
            if (!isSimpleCell(cell, snapshot.ambiguousWidth))
                return invalidateInput();
            if (!cell.codePoint)
                break;
            if (static_cast<std::size_t>(end - m_projectedColumn) >= m_editLimit - m_pendingCount)
                return capacityFailure();
            ++end;
        }
        const auto cost = static_cast<std::size_t>(end - m_projectedColumn) + 1;
        if (cost > m_editLimit - m_pendingCount)
            return capacityFailure();
        if (isBackspace) {
            const int first = m_projectedColumn - 1;
            if (!isKnownCell(snapshot, first) || !isSimpleCell(projectedCell(snapshot, first), snapshot.ambiguousWidth))
                return invalidateInput();
            for (int column = first; column < end; ++column)
                append(column, projectedCell(snapshot, column + 1), first, false);
            m_projectedColumn = first;
        } else {
            const int next = m_projectedColumn + 1;
            for (int column = end; column > m_projectedColumn; --column)
                append(column, projectedCell(snapshot, column - 1), next, false);
            Cell inserted = defaultBlankCell;
            inserted.codePoint = key.textCodePoints[0];
            inserted.pen = snapshot.inputPen;
            if (!isSimpleCell(inserted, snapshot.ambiguousWidth))
                return invalidateInput();
            append(m_projectedColumn, inserted, next, true);
            m_projectedColumn = next;
        }
        assert(m_pendingCount);
        m_pending[m_pendingCount - 1].endsKey = true;
    }
    rebuildOverlay();
    if (m_pendingCount == previousCount)
        return PredictionInputOutcome::Ignored;
    return m_hasConfirmedEpoch ? PredictionInputOutcome::ConfirmedEpoch : PredictionInputOutcome::Tentative;
}

std::expected<void, PredictionError> Predictor::unmodelledInput(std::uint64_t inputSeq, std::uint64_t now)
{
    if (const auto time = noteTime(now); !time)
        return std::unexpected(time.error());
    if (const auto sequence = admitSequence(inputSeq); !sequence)
        return sequence;
    invalidate(PredictionInvalidation::UnmodelledInput);
    return {};
}

void Predictor::invalidate(PredictionInvalidation reason)
{
    (void)openEpoch();
    m_needsFreshFrame = true;
    if (reason == PredictionInvalidation::Reconnect || reason == PredictionInvalidation::PaneEnded) {
        m_hasBaseline = false;
        m_hasIdentity = false;
        m_identity = {};
        m_lastInputSeq = 0;
        m_lastSettled = 0;
        m_exposure.reset();
        if (reason == PredictionInvalidation::PaneEnded)
            m_editLimit = 0;
    }
}

std::optional<std::uint64_t> Predictor::visiblePendingSince() const
{
    if (!m_hasConfirmedEpoch || m_needsFreshFrame || !m_pendingCount)
        return std::nullopt;
    return m_pending[0].sentAt;
}

PredictionOverlay Predictor::overlay(std::uint64_t now)
{
    if (!noteTime(now))
        return {};
    const auto treatment = m_exposure.treatment(now, visiblePendingSince());
    if (!treatment.isVisible)
        return {};
    return { .cells = { m_overlay.data(), m_overlayCount },
        .cursor = PredictionCursor { .row = m_row, .column = m_projectedColumn },
        .epoch = m_epoch,
        .treatment = treatment };
}

std::optional<std::uint64_t> Predictor::nextWakeDelay(std::uint64_t now)
{
    if (!noteTime(now))
        return std::nullopt;
    return m_exposure.nextWakeDelay(now, visiblePendingSince());
}

} // namespace Ubiquitty::Session
