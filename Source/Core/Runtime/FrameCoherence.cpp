// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/FrameCoherence.h"

#include "Core/Modes/SyncOutput.h"
#include "Core/Terminal.h"
#include "Core/VT/ParseState.h"

#include <algorithm>
#include <limits>

namespace Ubiquitty::Runtime {

namespace {

bool isCoherenceContinuationValid(const FrameCoherence::Continuation& state)
{
    if (state.bracket > FrameCoherence::Bracket::Persistent || state.lastPassEnd > PassEnd::Pending)
        return false;
    if (state.hasTakenFrame ? (state.frameRows <= 0 || state.frameColumns <= 0)
                            : (state.frameRows || state.frameColumns || state.lastFrameMicroseconds))
        return false;
    if (!state.hasPassEnded && (state.lastPassEndMicroseconds || state.lastPassEnd != PassEnd::WouldBlock))
        return false;
    const auto canAdd = [](std::uint64_t time, std::uint64_t interval) {
        return time <= std::numeric_limits<std::uint64_t>::max() - interval;
    };
    if (state.isDirty && (!canAdd(state.dirtySinceMicroseconds, state.tuning.deadlineMicroseconds) || !canAdd(state.dirtySinceMicroseconds, state.tuning.frameIntervalMicroseconds)))
        return false;
    if (state.hasTakenFrame && state.tuning.isPaced && !canAdd(state.lastFrameMicroseconds, state.tuning.frameIntervalMicroseconds))
        return false;
    if (state.hasPassEnded && !canAdd(state.lastPassEndMicroseconds, state.tuning.quiescenceMicroseconds))
        return false;
    return state.bracket != FrameCoherence::Bracket::Holding || !state.tuning.isPersistentBracketEnabled
        || canAdd(state.bracketOpenedMicroseconds, state.tuning.deadlineMicroseconds);
}

// One tick of the 60 Hz clock a 2026 block ages against.
constexpr std::uint64_t microsecondsPerTick = 1'000'000 / 60;

// Time left until `deadline`, or zero once it has passed.
constexpr std::uint64_t remainingUntil(std::uint64_t deadline, std::uint64_t now)
{
    return deadline > now ? deadline - now : 0;
}

} // namespace

bool FrameCoherence::isValidContinuation(const Continuation& state)
{
    return isCoherenceContinuationValid(state);
}

std::optional<FrameCoherence::Continuation> FrameCoherence::continuation() const
{
    Continuation state {
        .tuning = m_tuning,
        .dirtySinceMicroseconds = m_dirtySinceMicroseconds,
        .lastPassEndMicroseconds = m_lastPassEndMicroseconds,
        .lastFrameMicroseconds = m_lastFrameMicroseconds,
        .bracketOpenedMicroseconds = m_bracketOpenedMicroseconds,
        .bracketHideCount = m_bracketHideCount,
        .cursorHideCountAtLastFrame = m_cursorHideCountAtLastFrame,
        .syncCloseCountAtLastFrame = m_syncCloseCountAtLastFrame,
        .syncCloseCountAtLastPass = m_syncCloseCountAtLastPass,
        .frameRows = m_frameRows,
        .frameColumns = m_frameColumns,
        .bracket = m_bracket,
        .lastPassEnd = m_lastPassEnd,
        .isDirty = m_isDirty,
        .hasTakenFrame = m_hasTakenFrame,
        .hasPassEnded = m_hasPassEnded,
        .wasCursorVisibleAtLastFrame = m_wasCursorVisibleAtLastFrame,
        .wasComposingAtLastPass = m_wasComposingAtLastPass,
        .didExpireSinceLastFrame = m_didExpireSinceLastFrame,
    };
    return isCoherenceContinuationValid(state) ? std::optional(state) : std::nullopt;
}

bool FrameCoherence::restoreContinuation(const Continuation& state)
{
    if (state.tuning != m_tuning || !isCoherenceContinuationValid(state))
        return false;
    m_dirtySinceMicroseconds = state.dirtySinceMicroseconds;
    m_lastPassEndMicroseconds = state.lastPassEndMicroseconds;
    m_lastFrameMicroseconds = state.lastFrameMicroseconds;
    m_bracketOpenedMicroseconds = state.bracketOpenedMicroseconds;
    m_bracketHideCount = state.bracketHideCount;
    m_cursorHideCountAtLastFrame = state.cursorHideCountAtLastFrame;
    m_syncCloseCountAtLastFrame = state.syncCloseCountAtLastFrame;
    m_syncCloseCountAtLastPass = state.syncCloseCountAtLastPass;
    m_frameRows = state.frameRows;
    m_frameColumns = state.frameColumns;
    m_bracket = state.bracket;
    m_lastPassEnd = state.lastPassEnd;
    m_isDirty = state.isDirty;
    m_hasTakenFrame = state.hasTakenFrame;
    m_hasPassEnded = state.hasPassEnded;
    m_wasCursorVisibleAtLastFrame = state.wasCursorVisibleAtLastFrame;
    m_wasComposingAtLastPass = state.wasComposingAtLastPass;
    m_didExpireSinceLastFrame = state.didExpireSinceLastFrame;
    return true;
}

void FrameCoherence::noteDrainPass(const Terminal& terminal, std::uint64_t nowMicroseconds, PassEnd end)
{
    if (!m_isDirty) {
        m_isDirty = true;
        m_dirtySinceMicroseconds = nowMicroseconds;
    }
    // Whoever else asks the terminal may have expired the block since the last
    // pass; this pass is about to forget it was open.
    noteBlockExpiry(terminal);
    m_lastPassEnd = end;
    m_lastPassEndMicroseconds = nowMicroseconds;
    m_hasPassEnded = true;
    m_wasComposingAtLastPass = terminal.modes.syncOutput;
    m_syncCloseCountAtLastPass = terminal.syncOutputCloseCount;
    observeCursor(terminal, nowMicroseconds);
}

void FrameCoherence::noteMidReadClose(const Terminal& terminal, std::uint64_t nowMicroseconds)
{
    if (!m_isDirty) {
        m_isDirty = true;
        m_dirtySinceMicroseconds = nowMicroseconds;
    }
    observeCursor(terminal, nowMicroseconds);
}

void FrameCoherence::noteHostChange(std::uint64_t nowMicroseconds)
{
    if (m_isDirty)
        return;
    m_isDirty = true;
    m_dirtySinceMicroseconds = nowMicroseconds;
}

void FrameCoherence::observeCursor(const Terminal& terminal, std::uint64_t nowMicroseconds)
{
    if (terminal.modes.isCursorVisible) {
        m_bracket = Bracket::None;
        return;
    }
    // A hide counted since the bracket opened means the cursor was shown in
    // between: that bracket closed and this is a new one.
    if (m_bracket != Bracket::None && terminal.cursorHideCount == m_bracketHideCount)
        return;
    const bool didHideSinceLastFrame = m_wasCursorVisibleAtLastFrame || terminal.cursorHideCount != m_cursorHideCountAtLastFrame;
    if (m_bracket == Bracket::None && !didHideSinceLastFrame)
        return;
    m_bracket = Bracket::Holding;
    m_bracketOpenedMicroseconds = nowMicroseconds;
    m_bracketHideCount = terminal.cursorHideCount;
}

void FrameCoherence::agePersistence(std::uint64_t nowMicroseconds)
{
    if (m_bracket != Bracket::Holding || !m_tuning.isPersistentBracketEnabled
        || nowMicroseconds - m_bracketOpenedMicroseconds < m_tuning.deadlineMicroseconds)
        return;
    // The bracket held the frame for the whole deadline: the frame it releases
    // is the deadline's, not a coherent one.
    m_bracket = Bracket::Persistent;
    m_didExpireSinceLastFrame = true;
}

void FrameCoherence::noteBlockExpiry(const Terminal& terminal)
{
    if (m_wasComposingAtLastPass && !terminal.modes.syncOutput && terminal.syncOutputCloseCount == m_syncCloseCountAtLastPass)
        m_didExpireSinceLastFrame = true;
}

std::uint64_t FrameCoherence::pacingRemaining(std::uint64_t nowMicroseconds) const
{
    if (!m_tuning.isPaced || !m_hasTakenFrame)
        return 0;
    return remainingUntil(m_lastFrameMicroseconds + m_tuning.frameIntervalMicroseconds, nowMicroseconds);
}

bool FrameCoherence::canTakeBudgetedFrame() const
{
    return m_hasPassEnded && (m_lastPassEnd == PassEnd::Capacity || (m_lastPassEnd == PassEnd::TimeBudget && m_bracket != Bracket::Holding));
}

FrameAdmission FrameCoherence::admit(Terminal& terminal, std::uint64_t nowMicroseconds, Ticks ticks, ReadReadiness readiness)
{
    if (!m_isDirty)
        return {};
    observeCursor(terminal, nowMicroseconds);
    agePersistence(nowMicroseconds);
    // The block's own budget bounds it, not the deadline: a program composing
    // under 2026 has said no intermediate state may be shown.
    if (Modes::SyncOutput::shouldWithholdFrame(terminal, ticks))
        return {};
    noteBlockExpiry(terminal);
    if (pacingRemaining(nowMicroseconds) > 0)
        return {};
    const bool isStructural = !m_hasTakenFrame || terminal.rows != m_frameRows || terminal.columns != m_frameColumns;
    const bool isAtGround = VT::isParserAtGround(&terminal);

    if (canTakeBudgetedFrame()) {
        const FrameClass floodClass = isStructural       ? FrameClass::Structural
            : (!isAtGround || m_didExpireSinceLastFrame) ? FrameClass::Forced
                                                         : FrameClass::Flood;
        return { true, floodClass };
    }

    if (isAtGround && m_bracket != Bracket::Holding) {
        const bool didCloseBlock = terminal.syncOutputCloseCount != m_syncCloseCountAtLastFrame;
        const bool isQuiescent = readiness == ReadReadiness::Quiet
            && (!m_hasPassEnded || nowMicroseconds - m_lastPassEndMicroseconds >= m_tuning.quiescenceMicroseconds);
        const bool isWriterStillWriting = nowMicroseconds - m_dirtySinceMicroseconds >= m_tuning.frameIntervalMicroseconds;
        if (didCloseBlock || isQuiescent || isWriterStillWriting) {
            const FrameClass coherentClass = isStructural ? FrameClass::Structural
                : m_didExpireSinceLastFrame               ? FrameClass::Forced
                : didCloseBlock                           ? FrameClass::Boundary
                : isQuiescent                             ? FrameClass::Settled
                                                          : FrameClass::Flood;
            return { true, coherentClass };
        }
    }
    if (nowMicroseconds - m_dirtySinceMicroseconds >= m_tuning.deadlineMicroseconds)
        return { true, isStructural ? FrameClass::Structural : FrameClass::Forced };
    return {};
}

void FrameCoherence::noteFrameTaken(const Terminal& terminal, std::uint64_t nowMicroseconds)
{
    m_isDirty = false;
    // The elapsed budget releases this dirty epoch only. Keep the last read's
    // timestamp for quiescence, but do not lend its parser bypass to a later
    // host change or a synchronized close inside the next read.
    if (m_lastPassEnd == PassEnd::TimeBudget)
        m_lastPassEnd = PassEnd::Pending;
    m_hasTakenFrame = true;
    m_didExpireSinceLastFrame = false;
    m_frameRows = terminal.rows;
    m_frameColumns = terminal.columns;
    m_lastFrameMicroseconds = nowMicroseconds;
    m_wasCursorVisibleAtLastFrame = terminal.modes.isCursorVisible;
    m_cursorHideCountAtLastFrame = terminal.cursorHideCount;
    m_syncCloseCountAtLastFrame = terminal.syncOutputCloseCount;
    m_wasComposingAtLastPass = terminal.modes.syncOutput;
    m_syncCloseCountAtLastPass = terminal.syncOutputCloseCount;
}

std::optional<std::uint64_t> FrameCoherence::microsecondsUntilAsk(const Terminal& terminal, std::uint64_t nowMicroseconds, Ticks ticks) const
{
    if (!m_isDirty)
        return std::nullopt;
    // A block that closes arrives as bytes, which wake the caller; only its
    // expiry needs a timer. A block not yet aged is stamped by the next ask.
    if (Modes::SyncOutput::isActive(&terminal))
        return static_cast<std::uint64_t>(Modes::SyncOutput::ticksUntilHoldExpires(terminal, ticks)) * microsecondsPerTick;

    // Each condition still holding the frame names when it lets go; one that
    // waits on bytes (a parser mid-sequence without a budget yield) lets go at the deadline. The
    // pacing then holds whatever is released before the interval is up.
    std::uint64_t soonest = remainingUntil(m_dirtySinceMicroseconds + m_tuning.deadlineMicroseconds, nowMicroseconds);
    if (canTakeBudgetedFrame()) {
        soonest = 0;
    } else if (VT::isParserAtGround(&terminal) && m_bracket != Bracket::Holding) {
        const bool didCloseBlock = terminal.syncOutputCloseCount != m_syncCloseCountAtLastFrame;
        const std::uint64_t quiet = m_hasPassEnded ? remainingUntil(m_lastPassEndMicroseconds + m_tuning.quiescenceMicroseconds, nowMicroseconds) : 0;
        const std::uint64_t stillWriting = remainingUntil(m_dirtySinceMicroseconds + m_tuning.frameIntervalMicroseconds, nowMicroseconds);
        soonest = didCloseBlock ? 0 : std::min({ soonest, quiet, stillWriting });
    } else if (m_bracket == Bracket::Holding && m_tuning.isPersistentBracketEnabled) {
        soonest = std::min(soonest, remainingUntil(m_bracketOpenedMicroseconds + m_tuning.deadlineMicroseconds, nowMicroseconds));
    }
    return std::max(soonest, pacingRemaining(nowMicroseconds));
}

} // namespace Ubiquitty::Runtime
