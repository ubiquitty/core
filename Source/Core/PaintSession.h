// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/PaintRun.h"
#include "Core/Screen/PendingScroll.h"

namespace Ubiquitty {

class CellGrid; // forward

// RAII paint session. Move-only. Caller iterates pending scrolls
// (must drain first — m_bbShadow correctness depends on it), then
// dirty runs. Destructor runs invariant checks (debug only).
//
// Typical use:
//
//   {
//       auto ses = display.beginPaint();
//       PendingScroll s;
//       while (ses.nextPendingScroll(&s)) {
//           pal->scrollBB(s);   // blit the back-buffer
//           ses.ackScroll(s);   // shift m_bbShadow
//       }
//       PaintRun r;
//       while (ses.nextDirtyRun(&r)) {
//           pal->DrawRun(r);    // paint glyphs into BB
//           pal->FlushRun(r);   // blit BB → window
//           ses.ackRun(r);      // copy cells_ → m_bbShadow
//       }
//   }  // dtor verifies all yielded runs were Ack'd (DEBUG)
class PaintSession {
public:
    PaintSession(const PaintSession&) = delete;
    PaintSession& operator=(const PaintSession&) = delete;
    PaintSession(PaintSession&& o) noexcept;
    PaintSession& operator=(PaintSession&& o) noexcept;
    ~PaintSession();

    [[nodiscard]] bool nextPendingScroll(Screen::PendingScroll* out);

    /// Marks a queued scroll as applied, so the next `nextPendingScroll` moves on.
    ///
    /// A scroll left unacknowledged is re-yielded, which is what keeps a frontend
    /// that drops a frame from silently losing the shift.
    ///
    /// - Parameter s: The scroll a previous `nextPendingScroll` yielded.
    void ackScroll(const Screen::PendingScroll& s);

    [[nodiscard]] bool nextDirtyRun(PaintRun* out);
    void ackRun(const PaintRun&);

    /// Yields the next row that differs from the shadow, whole.
    ///
    /// The run walk above splits a row at every pen change, because a backend drawing glyphs needs a run
    /// it can draw in one call. A consumer that only wants to know which rows changed pays that splitting
    /// for nothing: the row is re-entered per run, each run acknowledged separately, and the pen compared
    /// per cell to decide where to stop. This asks the row the one question such a consumer has.
    ///
    /// The two walks share the cursor, so a session uses one or the other. Mixing them would have one
    /// walk resume where the other left off — a row half-offered as runs and then offered whole, or the
    /// reverse — and a debug build refuses it rather than leaving it to be remembered.
    ///
    /// - Parameter out: Receives the row.
    /// - Returns: Whether a row was yielded.
    [[nodiscard]] bool nextDirtyRow(Row* out);

    /// Marks a whole row as painted, advancing the shadow across it.
    ///
    /// - Parameter row: A row a previous ``nextDirtyRow`` yielded.
    void ackRow(Row row);

    [[nodiscard]] bool isFullRedraw() const { return m_fullRedraw; }

private:
    explicit PaintSession(CellGrid* d);
    // Non-owning back-pointer to the CellGrid that begot this session.
    // The session cannot outlive the CellGrid; PaintSession is intended
    // to live within a single paint scope on the stack.
    CellGrid* m_display;
    int m_cursorRow;
    int m_cursorColumn;
    bool m_fullRedraw;
    bool m_moved;
    // Set once this session acks a run or a scroll — i.e. it advanced the
    // shadow baseline. The destructor bumps CellGrid::m_damageGeneration when
    // set, so a peer snapshot buffer sharing the one shadow can detect that a
    // drain it did not perform moved the baseline (dual-buffer divergence).
    bool m_consumedDamage;
    // Set once nextDirtyRun has examined the last row. Only a walk that reached
    // the end has offered every divergent cell, so only then may the shadow be
    // called valid and only then is a captured full-redraw request discharged.
    bool m_hasWalkedToEnd;
#ifndef NDEBUG
    int m_yieldedRuns;
    int m_ackedRuns;
    // Which walk this session has begun. The two share a cursor, so a session
    // that used both would have one resume from where the other stopped.
    /// Which of the two walks a session has begun.
    enum class WalkKind : std::uint8_t {
        /// Neither, so either may start.
        None,
        /// Runs, split at pen changes for a backend that draws glyphs.
        Runs,
        /// Whole rows, for a consumer that wants which rows changed.
        Rows,
    };
    WalkKind m_walkKind { WalkKind::None };
#endif
    friend class CellGrid;
};

} // namespace Ubiquitty
