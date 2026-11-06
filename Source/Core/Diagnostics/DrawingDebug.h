// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The drawing-debug panel's readout: what it shows and how it reads. A frontend
// gathers the renderer's half of the facts, Core gathers the terminal's half and
// lays the whole out, so every shell's panel says the same thing in the same
// words.

#include "Base/SwiftInterop.h"

#include <cstdint>
#include <string>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Diagnostics {

/// Everything the drawing-debug panel reports about one terminal view.
struct DrawingDebugFacts {
    /// The grid, in columns and rows.
    int columns = 0;
    int rows = 0;
    /// The cursor's cell on the active screen.
    int cursorColumn = 0;
    int cursorRow = 0;
    /// Lines of history above the screen.
    int scrollbackLines = 0;
    /// How far the view is scrolled back, in rows.
    int viewOffset = 0;
    /// The face the renderer draws with, and its size.
    std::string fontName;
    double fontSizeInPoints = 0.0;
    /// The theme the palette came from.
    int themeIndex = 0;
    /// One cell's advance and height.
    double cellWidthInPoints = 0.0;
    double cellHeightInPoints = 0.0;
    /// Glyph-atlas slots in use against its capacity.
    int atlasSlotsUsed = 0;
    int atlasSlotCapacity = 0;
    /// Instances the last frame drew.
    std::uint64_t cellsPerFrame = 0;
    std::uint64_t overlaysPerFrame = 0;
    /// Scrollback search matches.
    int searchMatches = 0;
    /// The bell's configured mode, as its raw value.
    int bellMode = 0;
};

/// How often the panel re-reads its facts.
[[nodiscard]] constexpr double drawingDebugRefreshIntervalSeconds() { return 0.25; }

/// Fills the terminal's half of the facts: the grid, the cursor, the history,
/// the search matches and the bell mode.
///
/// Caller holds the model lock.
///
/// - Parameters:
///   - facts: The facts to fill; the renderer's half is left as it was.
///   - terminal: The terminal the panel reports on.
void readTerminalFacts(DrawingDebugFacts& facts, const Terminal& terminal);

/// The panel's text, one fact per line.
///
/// - Parameter facts: What to report.
/// - Returns: The readout.
[[nodiscard]] std::string drawingDebugText(const DrawingDebugFacts& facts);

} // namespace Ubiquitty::Diagnostics
