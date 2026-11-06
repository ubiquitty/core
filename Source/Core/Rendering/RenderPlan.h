// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "Base/SwiftInterop.h"

// RenderPlan — portable frame-damage model for a retained-mode renderer:
// compare this frame's rows against the previous frame's by a per-row digest
// and rebuild only what moved. Pure policy, no GPU/platform dependency.
// Pure C++23, no exceptions/RTTI, host-testable.

namespace Ubiquitty {

struct Cell; // Core/Cell.h; only its layout is read

namespace Rendering {

// 64-bit content+style digest of one visible row. Equality is a cache hint,
// not a correctness boundary: the tracker periodically forces a full rebuild
// so a collision cannot suppress pixels indefinitely.
using RowDigest = std::uint64_t;

/// Hashes a row's raw bytes, folded with a salt.
///
/// A `Cell` array is fixed-size and zero-padded, so its byte image is a
/// faithful content key. `std::as_bytes` over the row is what callers pass:
/// the element count and the element size travel together, where a hand
/// written `columns * sizeof(Cell)` can disagree with the pointer beside it.
///
/// - Parameters:
///   - bytes: The row image.
///   - salt: Every per-row influence absent from the cell bytes — line
///     attribute, baked cursor and blink bucket, hovered-link coverage,
///     blink phase. A frame-global input belongs in the epoch, not here.
/// - Returns: A digest that changes when the content does.
RowDigest rowDigest(std::span<const std::byte> bytes, std::uint64_t salt);

// Inputs a retained-mode renderer hands the planner once per frame. Portable
// scalar/Core state only — no GPU or platform type.
struct PlanInputs {
    const Cell* cells = nullptr; // rows*columns, row-major, display order
    // Alternative to `cells`: individually contiguous rows, not one block. When
    // non-null takes precedence; rowPointers[r] is a contiguous `columns`-cell
    // row, same digest as the equivalent flat `cells`.
    const Cell* const* rowPointers = nullptr; // length rows, or null
    int rows = 0;
    int columns = 0;
    const std::uint8_t* lineAttributes = nullptr; // length rows, or null
    // Marks move with history rows during a pan even though Cell bytes don't change.
    const std::uint8_t* rowMarks = nullptr; // length rows, or null

    // Focused block cursor baked into a cell background. cursorRow < 0 ⇒ no
    // baked cursor this frame; the rest fold into that row only.
    int cursorRow = -1;
    int cursorColumn = 0;
    std::uint32_t cursorFadeBucket = 0;
    bool cursorInvert = false;

    // Hovered-link underline run in visible-grid space; hoverRow0 > hoverRow1
    // (the defaults) means no hover.
    int hoverRow0 = 0, hoverRow1 = -1;
    int hoverColumn0 = 0, hoverColumn1 = -1;

    // The focused search match's run, in the same shape and for the same
    // reason. Its highlight is baked into the cell — the builder overrides the
    // background of every cell in the run — where the selection and the
    // unfocused matches ride overlay quads rebuilt each frame. So a match that
    // moves onto a row whose bytes did not change leaves that row clean, the
    // cell pass never repaints it, and the pill's flat fill is missing under
    // the glyphs while the ring the overlay draws still shows its ends.
    int matchRow0 = 0, matchRow1 = -1;
    int matchColumn0 = 0, matchColumn1 = -1;

    // Opaque frame-global blob hashed wholesale into the epoch: any byte change
    // forces a Full frame.
    const void* globalState = nullptr;
    std::size_t globalStateLength = 0;

    // Frame's monotonic tick; folded into the epoch only when the grid holds
    // blinking (SGR 5) cells, so a blink phase flip forces a rebuild.
    std::uint32_t blinkClock = 0;

    // Gate on the per-cell blink scan; default true ⇒ scan every frame. Must
    // over-approximate: false only when blink truly cannot be present, else a
    // blink flip would be missed.
    bool blinkPresentHint = true;
};

// Fill `digestsOut` (length in.rows) with this frame's per-row digests and
// return the epoch to pass FrameDamageTracker::plan().
std::uint64_t buildFrameDigests(const PlanInputs& in, RowDigest* digestsOut);

// How this frame relates to the last committed one.
enum class FrameKind {
    Full, // dims or frame epoch changed, or first frame — rebuild all.
    Unchanged, // every row digest matches — reuse the whole prior frame.
    Dirty, // an arbitrary subset of rows changed (rebuild those).
    Scrolled, // the prior frame shifted uniformly by `scrollDelta` rows;
              // only the freshly-exposed band differs.
};

// The damage plan for one frame. `dirtyRows` (length `rows`) is 1 where the
// renderer must rebuild row r, 0 where it may reuse. All-0 for Unchanged,
// all-1 for Full, freshly-exposed band for Scrolled. `dirtyFirst..dirtyLast`
// is the inclusive bounding range of set bits (dirtyLast < dirtyFirst ⇒ none).
// Pointer owned by the tracker, valid until its next plan()/reset()/commit().
struct FramePlan {
    FrameKind kind = FrameKind::Full;
    int rows = 0;
    int scrollDelta = 0; // Scrolled only: >0 content moved
                         // up by N (new row r shows old r+N).
    int dirtyFirst = 0;
    int dirtyLast = -1;
    const std::uint8_t* dirtyRows = nullptr;
};

// Holds the previous frame's row digests and classifies each new frame against
// them. Single-threaded. Separate plan()/commit() lets the renderer bail
// mid-frame (drawable lost) without poisoning the baseline.
class UBIQUITTY_NONCOPYABLE FrameDamageTracker {
public:
    static constexpr int collisionRecoveryFrameInterval = 256;

    FrameDamageTracker() = default;
    FrameDamageTracker(const FrameDamageTracker&) = delete;
    FrameDamageTracker& operator=(const FrameDamageTracker&) = delete;

    // Classify `rows` row digests against the last committed frame. `epoch` is a
    // monotonic stamp the caller bumps whenever any frame-global input changed
    // (forces Full). Returned plan references storage valid until the next call.
    const FramePlan& plan(const RowDigest* digests, int rows, std::uint64_t epoch);

    // Promote the digests last passed to plan() to the new baseline. Call only
    // after the renderer has consumed the plan.
    void commit();

    // Force the next frame to classify Full (device loss, resize, cache drop).
    void reset();

    // Convenience: did the last plan() mark row r dirty?
    [[nodiscard]] bool rowDirty(int r) const { return r >= 0 && r < static_cast<int>(m_dirty.size()) && m_dirty[r] != 0; }

private:
    // Largest uniform scroll the detector recognises; bigger jumps fall back to
    // Dirty/Full, which is always correct.
    static constexpr int maxScrollRows = 256;

    std::vector<RowDigest> m_prev; // last committed baseline
    std::vector<RowDigest> m_staged; // digests handed to plan(), pending commit
    std::vector<std::uint8_t> m_dirty; // per-row rebuild flags (plan output)
    std::uint64_t m_prevEpoch = ~std::uint64_t { 0 };
    std::uint64_t m_stagedEpoch = 0; // epoch handed to plan(), pending commit
    int m_committedFramesSinceFull = 0;
    bool m_stagedWasFull = false;
    bool m_havePrev = false;
    FramePlan m_plan;
};

} // namespace Rendering
} // namespace Ubiquitty
