// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

// Value types crossing into and out of TerminalRuntime. All-scalar PODs: no
// allocation, no platform types (Swift imports arrays-free PODs directly).

namespace Ubiquitty::Runtime {

/// Which OSC 66 text the renderer of the frontend starting a session draws.
///
/// Sized blocks and proportional runs are independent renderer capabilities.
enum class DrawnText : std::uint8_t {
    GridOnly, ///< Neither: sized and proportional runs print as ordinary text, a measure query goes unanswered, and cells a peer sends flatten.
    Sized, ///< The renderer band-crops and tiles a sized block; proportional runs are refused.
    Proportional, ///< The renderer draws proportional runs; sized blocks are refused.
    SizedAndProportional, ///< The renderer draws both.
};

/// Whether `drawnText` includes sized blocks.
[[nodiscard]] constexpr bool drawsSizedText(DrawnText drawnText)
{
    return drawnText == DrawnText::Sized || drawnText == DrawnText::SizedAndProportional;
}

/// Whether `drawnText` includes proportional runs.
[[nodiscard]] constexpr bool drawsProportionalText(DrawnText drawnText)
{
    return drawnText == DrawnText::Proportional || drawnText == DrawnText::SizedAndProportional;
}

// Initial state injected at construction. The runtime never reads a Values
// singleton; a frontend assembles this.
struct RuntimeInitParameters {
    int rows = 24;
    int columns = 80;
    bool allowReflowOnInit = false; // a fresh Ubiquitty::Terminal has nothing to reflow
    // Deferred-state scroll lane:
    // eligible at-bottom floods defer per-scroll accounting to the lane flush.
    bool scrollLaneEnabled = true;
    // Whether this frontend draws OSC 66 sized text. Off leaves the terminal
    // refusing the size — TextSizing.cpp emits the run unsized — and makes a
    // replica flatten any sized cells a peer sends, because a renderer with no
    // band crop paints the whole glyph on every row the block covers. A frontend
    // answers for its own renderer here: the crop, the tiling and the box rect
    // all live in CellInstanceBuilder, so what varies across arms is whether one
    // draws them, not whether it could.
    bool drawsSizedText = false;
    // False leaves OSC 66 p=1 queries unanswered, prints runs as grid text,
    // and flattens proportional cells received through Sync.
    bool drawsProportionalText = false;
};

struct DimensionSnapshot {
    int rows = 0;
    int columns = 0;
    int scrollbackCount = 0;
    std::uint32_t generation = 0; // bumped each capture; staleness check
};

/// Grid dimensions used by title composition, in the runtime's row-first order.
struct TitleGrid {
    int rows = 0;
    int columns = 0;

    friend constexpr bool operator==(TitleGrid, TitleGrid) = default;
};

/// A remote pane uses its laid-out size until its host owns the screen plane.
[[nodiscard]] constexpr TitleGrid titleGridForScreen(bool isRemote, bool hasHostScreen, TitleGrid laidOut, TitleGrid model)
{
    return isRemote && !hasHostScreen && laidOut.rows > 0 && laidOut.columns > 0 ? laidOut : model;
}

} // namespace Ubiquitty::Runtime
