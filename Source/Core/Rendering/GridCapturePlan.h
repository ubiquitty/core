// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/Log.h"

#include <cstdint>
#include <cstdlib>
#include <string_view>

// GridCapturePlan — portable eligibility policy for a DAMAGE-FIRST snapshot
// capture. A snapshot renderer copies the live grid into a retained buffer once
// per frame under the model lock. Damage-first capture reuses the RETAINED
// buffer's prior rows and re-reads only the rows Core reports dirty — but only
// when it can PROVE the retained rows are still valid. That proof is pure scalar
// policy (no Cell / GPU / platform type), so it lives here.
//
// The hazard it guards is dual-buffer damage divergence: a snapshot renderer
// double-buffers (a live buffer and a sync/DECSET-2026 pending buffer) over one
// Core damage stream. A drain that consumes dirty runs shifts the shadow
// baseline, so whichever buffer drains a generation OWNS those runs; the other
// never applied them and its retained rows are stale for them. A monotonic damage
// generation lets each buffer prove it saw every generation since its own last
// capture — a buffer that skipped one re-reads fully.
//
// Pure C++23, no exceptions / RTTI, host-testable.

namespace Ubiquitty::Rendering {

/// What `UBIQUITTY_GRID_CACHE` asks of the retained grid.
enum class GridCacheSetting : std::uint8_t {
    On,
    Off,
    /// A value outside the documented four; the cache keeps its default, on.
    Unrecognized,
};

/// Reads a `UBIQUITTY_GRID_CACHE` value.
///
/// The documented values are `1` and `on`, which keep the retained grid (the
/// production path), and `0` and `off`, which turn it off so every frame
/// rebuilds every row for a pixel-parity or energy A/B. Unset or empty is the
/// default, on. Anything else is unrecognized rather than read by its first
/// character: `false` would otherwise keep the cache on and `0ff` turn it off,
/// and a misspelt A/B would measure the wrong arm without a word.
///
/// - Parameter environmentValue: The variable's value, or null when unset.
/// - Returns: The setting; ``GridCacheSetting/Unrecognized`` for any other value.
[[nodiscard]] constexpr GridCacheSetting parseGridCacheSetting(const char* environmentValue)
{
    if (!environmentValue || environmentValue[0] == '\0')
        return GridCacheSetting::On;
    const std::string_view value(environmentValue);
    if (value == "1" || value == "on")
        return GridCacheSetting::On;
    if (value == "0" || value == "off")
        return GridCacheSetting::Off;
    return GridCacheSetting::Unrecognized;
}

/// Whether this process keeps the retained grid between frames.
///
/// Off only for `UBIQUITTY_GRID_CACHE=0` or `off` (``parseGridCacheSetting``):
/// every capture is then a Full one and every frame rebuilds every row. Read
/// once per process, so no frame pays for the lookup and no frame sees it
/// change; an unrecognized value is reported once on stderr and leaves the
/// cache on.
///
/// - Returns: Whether the retained grid is kept.
[[nodiscard]] inline bool isRetainedGridCacheEnabled()
{
    static const bool isEnabled = [] {
        const char* value = std::getenv("UBIQUITTY_GRID_CACHE");
        const GridCacheSetting setting = parseGridCacheSetting(value);
        if (setting == GridCacheSetting::Unrecognized)
            PAL::logLine(stderr, "ubiquitty: UBIQUITTY_GRID_CACHE={} is not one of 0, 1, off or on; the grid cache stays on", value);
        return setting != GridCacheSetting::Off;
    }();
    return isEnabled;
}

// Facts a retained snapshot buffer compares against its own previous capture to
// decide whether it may update incrementally (copy only Core's dirty rows, after
// shifting any scrolled band in place) or must re-read the whole grid.
//
// `generationBeforeDrain` is CellGrid's damage generation read before this
// capture's own drain; `bufferGeneration` is the generation this buffer stored
// after its previous drain. They are equal iff no other drain (the peer buffer,
// a discarded sync drain) advanced the baseline in between — the exact condition
// under which this buffer's retained rows still reflect the shadow.
struct CaptureEligibility {
    bool trustDamage = false; // false for sync/scrollback/reset captures -> full
    bool bufferInitialized = false; // false on the first capture into this buffer
    bool isSameGrid = false; // the grid this buffer last drained is the one draining now
    std::uint64_t bufferGeneration = 0; // this buffer's generation after its last drain
    std::uint64_t generationBeforeDrain = 0; // CellGrid generation before this drain
    int lastViewOffset = 0; // this buffer's viewOffset at its last capture
    int currentViewOffset = 0;
    int lastRows = 0;
    int lastColumns = 0;
    int currentRows = 0;
    int currentColumns = 0;
    bool lastReverseVideo = false;
    bool currentReverseVideo = false;
    std::uint64_t lastPaletteEpoch = 0;
    std::uint64_t currentPaletteEpoch = 0;
    // Whether an image placement reaches the viewport, this frame and the last
    // one. Images are painted into the retained pixels with the cells, and a
    // picture is not a row: the damage drain names the rows whose cells were
    // written, and a picture that moved or left wrote none of the rows it used
    // to cover.
    bool hadImagesOnViewport = false;
    bool hasImagesOnViewport = false;
};

// True when the retained buffer may be updated incrementally rather than fully
// re-read. Every clause is a Full-copy trigger when false:
//   - trustDamage:No callers (sync pending, scrollback, shadow resets) always full;
//   - the first capture into a buffer has no retained rows to reuse;
//   - a buffer last drained from another grid holds that grid's rows, and a
//     generation is a per-grid count that two grids can share;
//   - a generation gap means the peer buffer consumed runs this buffer never
//     applied (dual-buffer divergence) -> stale -> full;
//   - a non-zero (or changed) viewOffset shows history rows that live-screen
//     damage does not describe -> full;
//   - a geometry change resized the retained buffer out from under the diff;
//   - reverse-video is frame-global and repaints every cell;
//   - a palette write recolours cells without touching them, so damage reports
//     the rows clean and the retained buffer keeps the replaced colours;
//   - an image on the viewport, now or in the previous frame, is painted into
//     the retained pixels outside the row damage: reusing them keeps the
//     picture where it was while the text moves on, and the next image lands
//     on the remains of the last.
[[nodiscard]] inline bool captureCanReuse(const CaptureEligibility& e)
{
    return e.trustDamage && e.bufferInitialized && e.isSameGrid && e.generationBeforeDrain == e.bufferGeneration && e.currentViewOffset == 0 && e.lastViewOffset == 0 && e.currentRows == e.lastRows && e.currentColumns == e.lastColumns && e.currentReverseVideo == e.lastReverseVideo && e.currentPaletteEpoch == e.lastPaletteEpoch && !e.hasImagesOnViewport && !e.hadImagesOnViewport;
}

// Facts a capture consults to decide whether the grid copy may be skipped
// outright this frame. Under fullscreen content mode (DECSET 7402) with the
// opaque Color letterbox, no pixel on glass reads the snapshot cells: the shader
// paints every cell the letterbox color and the occlusion skip emits no glyphs,
// so re-reading cells buys nothing. A capture that skips must declare its
// retained grid stale — so the first capture after the mode clears fails
// captureCanReuse and re-reads every row — and the platform frame drive must
// refuse to present a stale grid once the skip condition no longer holds.
struct GridSkipEligibility {
    bool isFullscreenContentActive = false; // DECSET 7402 active on the live terminal
    bool isLetterboxOpaque = false; // Color letterbox: every cell painted opaquely
    // The glyph-suppression half of the invisibility premise: with the
    // occlusion skip disabled (diagnostic knob keeping the full glyph path),
    // glyph ink still reaches glass, so the capture must keep the cells fresh
    // — skipping would paint stale ink and contaminate the knob's A/B baseline.
    bool isOcclusionSkipEnabled = false;
};

[[nodiscard]] inline bool captureCanSkipGrid(const GridSkipEligibility& e)
{
    return e.isFullscreenContentActive && e.isLetterboxOpaque && e.isOcclusionSkipEnabled;
}

} // namespace Ubiquitty::Rendering
