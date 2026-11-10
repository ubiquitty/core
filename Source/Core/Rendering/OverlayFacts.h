// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/SwiftInterop.h"
#include "Core/Screen/HistoryFacts.h"
#include "Core/Screen/ScrollSearch.h"

#include <array>
#include <cstdint>

// The view tiers a frame draws over its grid without the grid holding them —
// search matches, trigger highlights and the mode-1001 hilite band — as the
// model's writer publishes them beside its snapshot. A frontend turns them into
// the visible rows of whichever capture it is building
// (`OverlaySpanGather.h`), so drawing them never reads the model.
//
// Rows are absolute rows numbered as the model numbered them when the facts
// were gathered; each fact that needs it carries that numbering, and the
// conversion reads the rows against the capture's own.

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Rendering {

/// One highlighted run on one row: an absolute row and inclusive columns.
struct OverlaySegment {
    int absoluteRow = 0;
    int column0 = 0;
    int column1 = 0;

    friend bool operator==(const OverlaySegment&, const OverlaySegment&) = default;
};

/// The most trigger highlights a terminal holds, which `Terminal`'s ring
/// capacity matches.
constexpr int maxTriggerOverlaySegments = 64;

/// The search matches a frame highlights, as the writer last resolved them.
///
/// The focused match's segments come first, then every other match's, each in
/// the order the terminal's search projection stores them. Empty while no
/// match is focused, as the live gathers are.
struct SearchOverlayFacts {
    /// Counts publications whose content differed from the one before; zero
    /// before the first.
    std::uint64_t generation = 0;
    /// The numbering the segments' rows were resolved under.
    Screen::RowNumbering numbering {};
    /// The search projection's count of focus changes, for a reader that
    /// animates the focused match's arrival.
    std::uint32_t activeMatchEpoch = 0;
    int focusedCount = 0;
    int otherCount = 0;
    std::array<OverlaySegment, Screen::scrollSearchMaxSegments> segments {};
};

/// The trigger highlights and the mode-1001 hilite band a frame draws.
struct MarkOverlayFacts {
    /// Counts publications whose content differed from the one before; zero
    /// before the first.
    std::uint64_t generation = 0;
    /// The trigger ring, oldest first. Rows are as each highlight recorded
    /// them, the screen's top row plus the screen row, and are never
    /// renumbered: the live gather reads them the same way.
    int triggerCount = 0;
    std::array<OverlaySegment, maxTriggerOverlaySegments> triggers {};
    /// The host's band, in its own 1-based screen coordinates; all zero while
    /// no band is set.
    bool isHiliteValid = false;
    int hiliteStartColumn = 0;
    int hiliteStartRow = 0;
    int hiliteFirstRow = 0;
    int hiliteLastRow = 0;
};

/// Gathers the search matches the terminal's projection holds now.
///
/// The writer's half, under the model lock: the projection resolves its
/// segments again when the rows were renumbered since it last did, as it does
/// for any reader.
///
/// - Parameters:
///   - terminal: The terminal whose search state is read.
///   - out: Receives the matches; `generation` is left as it was.
void gatherSearchOverlayFacts(const Terminal& terminal, SearchOverlayFacts& out);

/// Gathers the trigger highlights and the hilite band the terminal holds now.
///
/// - Parameters:
///   - terminal: The terminal whose trigger ring and modes are read.
///   - out: Receives the marks; `generation` is left as it was.
void gatherMarkOverlayFacts(const Terminal& terminal, MarkOverlayFacts& out);

/// Whether two gatherings hold the same matches, generations aside.
[[nodiscard]] bool hasSameContent(const SearchOverlayFacts& left, const SearchOverlayFacts& right);

/// Whether two gatherings hold the same marks, generations aside.
[[nodiscard]] bool hasSameContent(const MarkOverlayFacts& left, const MarkOverlayFacts& right);

} // namespace Ubiquitty::Rendering
