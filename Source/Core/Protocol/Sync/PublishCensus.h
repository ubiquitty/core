// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// PublishCensus.h — why each published frame was taken, and how long it waited.
//
// The coherence rule (Runtime::FrameCoherence) decides when a pane's frame is
// taken and in which class; this counts those decisions and observes, on its
// own, what the rule exists to prevent: whether the frame landed inside a
// cursor-hidden redraw, and whether it was taken with the parser mid-sequence
// or a DEC 2026 block open. That is a frame taken with the cursor hidden now and
// either shown when the last frame was taken or hidden again since. A cursor
// already hidden at the last frame opens no bracket, so a program that keeps it
// hidden is never counted. Both observations are independent of the rule's own
// state, so a frame the rule calls coherent while the census sees it torn is a
// defect in the rule that shows up here rather than agreeing with itself.
//
// The census never decides. It reads the terminal at drain-pass boundaries, the
// only instants a host can act on, and the counter `Terminal` keeps for cursor
// hides makes a hide inside one read visible there.
//
// Hold time is from the first drain pass that dirtied the pane after the last
// frame to the frame that published it.

#include "Core/Runtime/FrameCoherence.h"
#include "Core/Terminal.h"
#include "Core/VT/ParseState.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace Ubiquitty::Protocol::Sync {

/// The census's account of one published frame.
struct CensusVerdict {
    Runtime::FrameClass frameClass = Runtime::FrameClass::Settled;
    /// Whether the frame landed inside a cursor-hidden redraw.
    bool isInsideRedrawBracket = false;
    /// Whether the frame was taken with the parser mid-sequence or a DEC 2026
    /// block open: a screen no program finished drawing.
    bool isIncoherent = false;
    /// Microseconds from the first pass that dirtied the pane to this frame;
    /// zero for a frame no pass dirtied.
    std::uint64_t holdMicroseconds = 0;
};

/// Counts published frames by class and their hold times, per pane.
class PublishCensus {
public:
    /// The hold-time histogram's bucket count. Bucket 0 holds waits under
    /// 1024 µs; bucket `b` holds [2^(b+9), 2^(b+10)) µs; the last is open.
    static constexpr std::size_t holdBucketCount = 16;

    /// Notes the end of a drain pass that parsed bytes, or a change the host
    /// made to the pane itself.
    ///
    /// - Parameter nowMicroseconds: The host's clock.
    void noteDirty(std::uint64_t nowMicroseconds)
    {
        if (m_isDirty)
            return;
        m_isDirty = true;
        m_dirtySinceMicroseconds = nowMicroseconds;
    }

    /// Counts a frame the coherence rule admitted.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal, as the frame will show it.
    ///   - nowMicroseconds: The host's clock.
    ///   - frameClass: The class the rule admitted the frame in.
    /// - Returns: The frame's class, bracket and hold time.
    CensusVerdict notePublish(const Terminal& terminal, std::uint64_t nowMicroseconds, Runtime::FrameClass frameClass)
    {
        CensusVerdict verdict;
        verdict.frameClass = frameClass;
        const bool isCursorVisible = terminal.modes.isCursorVisible;
        verdict.isInsideRedrawBracket = m_hasPublished && !isCursorVisible
            && (m_wasCursorVisibleAtLastPublish || terminal.cursorHideCount != m_cursorHideCountAtLastPublish);
        verdict.isIncoherent = !VT::isParserAtGround(&terminal) || terminal.modes.syncOutput;
        if (m_isDirty && nowMicroseconds >= m_dirtySinceMicroseconds)
            verdict.holdMicroseconds = nowMicroseconds - m_dirtySinceMicroseconds;

        ++m_frameCounts[static_cast<std::size_t>(verdict.frameClass)];
        if (verdict.isInsideRedrawBracket)
            ++m_redrawBracketFrameCount;
        if (verdict.isIncoherent)
            ++m_incoherentFrameCount;
        ++m_holdHistogram[holdBucket(verdict.holdMicroseconds)];
        if (verdict.holdMicroseconds > m_longestHoldMicroseconds)
            m_longestHoldMicroseconds = verdict.holdMicroseconds;

        m_hasPublished = true;
        m_isDirty = false;
        m_wasCursorVisibleAtLastPublish = isCursorVisible;
        m_cursorHideCountAtLastPublish = terminal.cursorHideCount;
        return verdict;
    }

    /// Returns how many frames were published in a class.
    ///
    /// - Parameter frameClass: The class.
    /// - Returns: The count since the census began.
    [[nodiscard]] std::uint64_t frameCount(Runtime::FrameClass frameClass) const { return m_frameCounts[static_cast<std::size_t>(frameClass)]; }

    /// Returns how many published frames landed inside a cursor-hidden redraw.
    [[nodiscard]] std::uint64_t redrawBracketFrameCount() const { return m_redrawBracketFrameCount; }

    /// Returns how many published frames were taken with the parser
    /// mid-sequence or a DEC 2026 block open.
    [[nodiscard]] std::uint64_t incoherentFrameCount() const { return m_incoherentFrameCount; }

    /// Returns the hold-time histogram; see `holdBucketCount` for the buckets.
    [[nodiscard]] const std::array<std::uint64_t, holdBucketCount>& holdHistogram() const { return m_holdHistogram; }

    /// Returns the longest hold seen, in microseconds.
    [[nodiscard]] std::uint64_t longestHoldMicroseconds() const { return m_longestHoldMicroseconds; }

    /// Returns the histogram bucket a hold time falls in.
    ///
    /// - Parameter holdMicroseconds: The hold.
    /// - Returns: An index below `holdBucketCount`.
    [[nodiscard]] static constexpr std::size_t holdBucket(std::uint64_t holdMicroseconds)
    {
        const std::uint64_t kibimicroseconds = holdMicroseconds >> 10;
        const std::size_t bucket = static_cast<std::size_t>(std::bit_width(kibimicroseconds));
        return bucket < holdBucketCount ? bucket : holdBucketCount - 1;
    }

private:
    std::array<std::uint64_t, Runtime::frameClassCount> m_frameCounts {};
    std::array<std::uint64_t, holdBucketCount> m_holdHistogram {};
    std::uint64_t m_redrawBracketFrameCount = 0;
    std::uint64_t m_incoherentFrameCount = 0;
    std::uint64_t m_longestHoldMicroseconds = 0;
    std::uint64_t m_dirtySinceMicroseconds = 0;
    std::uint32_t m_cursorHideCountAtLastPublish = 0;
    bool m_hasPublished = false;
    bool m_isDirty = false;
    bool m_wasCursorVisibleAtLastPublish = true;
};

} // namespace Ubiquitty::Protocol::Sync
