// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// LogicalPosition — a place in a logical line that keeps its text while the
// rows under it are renumbered.
//
// A position names a line and a count of the line's written cells before it,
// never a physical row. A resize rewraps the line into other rows, eviction
// renumbers every absolute row by one, an IL moves the line down the screen;
// the position still names the same characters, and `resolvePosition` says
// where they are now. When the characters are gone the answer is an explicit
// failure, and the consumer drops that one thing rather than guessing.
//
// A line is named one of two ways:
//
// - A history line by its logical line number (Core/Screen/HistoryLines.h)
//   offset by a piecewise bias, so a destructive clear that renumbers the
//   survivors leaves their names alone. History lines are immutable, so the
//   name is derived and nothing is stored for it.
// - A line on the screen by a pin: a record the buffer keeps of where the
//   line's first row is, and of how many times it was erased. A pin's name
//   has `pinnedLineIDBit` set and is never reused, so it cannot collide with a
//   derived name. Pins cost nothing on the write paths: a scroll leaves them
//   alone because a pin names its row by row ordinal, which a scroll does not
//   change, and only the cold paths that move rows in place, erase them or
//   rewrap them update the few pins that exist.
//
// A history line a pull-back or a rewrap moves onto the screen keeps its
// derived name on a carried pin, so a search hit or a selection made in
// history follows it through edits there.
//
// A row of the alternate screen can also be pinned on its own, by
// `captureAlternateRow`, for a consumer that keeps screen cells rather than
// characters: a program there repaints its rows in place.

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Screen {

/// Which buffer a line belongs to; each names its lines on its own.
enum class Buffer : std::uint8_t {
    Primary,
    Alternate,
};

/// The bit a pinned line's name carries, which no derived name has.
inline constexpr std::uint64_t pinnedLineIDBit = std::uint64_t { 1 } << 63;

/// A line's name in its buffer: never reused for another line.
struct LineID {
    std::uint64_t value = 0;

    /// Whether the name is a pin's rather than derived from a line number.
    [[nodiscard]] constexpr bool isPinned() const { return (value & pinnedLineIDBit) != 0; }

    friend constexpr auto operator<=>(const LineID&, const LineID&) = default;
};

/// A place in a logical line, independent of how the line is wrapped.
struct LogicalPosition {
    LineID line;
    /// Written cells before the place in the joined logical line: the cells
    /// something wrote, without the gaps a tab or a cursor move left, which a
    /// rewrap can strand at a row's end and the next rewrap drops. So the
    /// count is the same at every width. A grapheme is its lead cell. Never
    /// past the line's written cells: a place past them is `voidCells` beyond.
    std::uint32_t cell = 0;
    /// The line's erase revision when the position was captured; a history
    /// line's is zero.
    std::uint32_t revision = 0;
    /// Letters into a proportional run's cell, as a selection caret needs.
    std::uint16_t subCell = 0;
    /// Unwritten columns between the place and the written cell before it on
    /// its row, for a place that is on no written cell: in a gap, kept short
    /// of the written cell after the gap; or in the void past the line's end,
    /// a selection dragged past a short line or a whole-line selection's far
    /// end, clamped to the row's width so the place stays at the line's end
    /// through a narrowing rewrap.
    std::uint16_t voidCells = 0;
    Buffer buffer = Buffer::Primary;

    friend constexpr bool operator==(const LogicalPosition&, const LogicalPosition&) = default;
};
static_assert(sizeof(LogicalPosition) == 24, "a position stays three words, so consumers hold it as plain data");

/// Where a position's characters are now.
struct PhysicalPosition {
    /// History rows then screen rows, as absolute rows are counted on the
    /// buffer the position names; the alternate buffer has no history.
    int absoluteRow = 0;
    int column = 0;

    friend constexpr bool operator==(const PhysicalPosition&, const PhysicalPosition&) = default;
};

/// Why a position names nothing any more.
enum class PositionFailure : std::uint8_t {
    /// The line aged out of the history, or a clear removed it from the front.
    Evicted,
    /// The line's revision moved: ED, EL, ECH, ICH, DCH, DECIC, DECDC, a
    /// selective or rectangular erase, a rectangle move through it, or a
    /// replaced row.
    Erased,
    /// The line left an in-place band, a clear or a deletion removed it, or
    /// the name is one this buffer never gave out.
    Destroyed,
    /// The buffer the position names is not the one shown.
    OtherBuffer,
};

/// Whether a resolution holds a position to the line's revision.
enum class RevisionCheck : std::uint8_t {
    /// An erase on the line invalidates the position: a selection or a search
    /// hit names characters, and erased characters are gone.
    Exact,
    /// The position follows the line through erases: a prompt anchor names a
    /// row of the shell's, which the shell redraws at will.
    IgnoreErases,
};

/// Captures a cell as a position.
///
/// A cell in history is named by its line's derived name. A cell on the screen
/// pins its line, so the name follows the line through in-place moves and
/// notices its erasure. A column on no written cell, in a gap or past the
/// line's end, is kept as `voidCells`; a column on a wide character's trailing
/// half, or in the pad a wide character left at a margin, names the character.
///
/// The caller holds the model lock.
///
/// - Parameters:
///   - terminal: The terminal; the active buffer is the one captured on.
///   - absoluteRow: History rows then screen rows, as the active buffer counts
///     them.
///   - column: The column on that row.
///   - subCell: Letters into a proportional run's cell.
/// - Returns: The position, or why the cell cannot be named: `Evicted` for a
///   history row whose line's beginning is gone, `Destroyed` for a row past
///   the buffer or a buffer that refused a pin.
[[nodiscard]] std::expected<LogicalPosition, PositionFailure> capturePosition(const Ubiquitty::Terminal& terminal, int absoluteRow, int column, std::uint16_t subCell = 0);

/// Resolves a position to the row and column holding its cell now.
///
/// A place past the line's written cells, which a line that grew shorter or a
/// place captured in the void leaves, resolves at the line's end, never as a
/// failure: a selection dragged into the blank space right of the text keeps
/// working. A pinned line a wrap has since joined to the line above still
/// resolves, from its own first row, because its characters are still there.
///
/// The cost is one binary search over the history's line index and a walk of
/// the line's rows, or a scan of the buffer's pins for a pinned line.
///
/// The caller holds the model lock.
///
/// - Parameters:
///   - terminal: The terminal.
///   - position: A position captured from this terminal.
///   - check: Whether an erase of the line invalidates the position.
/// - Returns: The place, or why it names nothing.
[[nodiscard]] std::expected<PhysicalPosition, PositionFailure> resolvePosition(const Ubiquitty::Terminal& terminal, const LogicalPosition& position, RevisionCheck check = RevisionCheck::Exact);

/// Names a row of the alternate screen, rather than the characters on it.
///
/// A full-screen program repaints the alternate screen in place, erasing and
/// rewriting rows as it pleases, so what a place there keeps is its row. The
/// row is pinned on its own, even where it continues a line, and the pin
/// follows only the in-place moves that carry it: a region scroll, IL, DL, a
/// scroll of the whole screen, a sync scroll op. Erases and rewrites leave it.
///
/// The caller holds the model lock.
///
/// - Parameters:
///   - terminal: The terminal, showing its alternate screen.
///   - row: The screen row.
/// - Returns: The position, or `OtherBuffer` while the primary screen is
///   shown, or `Destroyed` for a row past the screen or a buffer that refused
///   a pin.
[[nodiscard]] std::expected<LogicalPosition, PositionFailure> captureAlternateRow(const Ubiquitty::Terminal& terminal, int row);

/// Resolves a row `captureAlternateRow` named to the screen row it is on now.
///
/// A DECSLRM band that scrolled part of the row moved the cells in its columns
/// to another row, so a column inside the band resolves where those cells went.
///
/// The caller holds the model lock.
///
/// - Parameters:
///   - terminal: The terminal.
///   - position: A position from `captureAlternateRow`.
///   - column: The column on the row the caller asks about.
/// - Returns: The row, or `OtherBuffer` while the primary screen is shown, or
///   `Destroyed` once a move carried the row, or the band's cells at `column`,
///   off its band or past the screen.
[[nodiscard]] std::expected<int, PositionFailure> resolveAlternateRow(const Ubiquitty::Terminal& terminal, const LogicalPosition& position, int column);

/// Names a cell of a history line by the line's number, for a search hit.
///
/// Handing out a name obliges the terminal to keep it true from then on: its
/// name records are made on the first name it gives. They are bookkeeping
/// about lines, not part of what the terminal shows, so a reader holding the
/// terminal as const may hand names out.
///
/// - Parameters:
///   - terminal: The terminal whose primary history holds the line.
///   - lineNumber: The line's logical number (Core/Screen/HistoryLines.h).
///   - cell: Written cells before the place.
/// - Returns: The position; it resolves only while the line is history or
///   was carried onto the screen by a pull-back. `Destroyed` if the identity
///   or a required pin cannot be allocated; no untracked name is returned.
[[nodiscard]] std::expected<LogicalPosition, PositionFailure> historyPosition(const Ubiquitty::Terminal& terminal, std::uint64_t lineNumber, std::uint32_t cell);

/// Returns the logical line number of the primary line a name names now.
///
/// - Parameters:
///   - terminal: The terminal.
///   - line: A name from a primary-buffer position.
/// - Returns: The number, or nothing for a destroyed pin, a pin whose row
///   continues a line, or a name in no segment.
[[nodiscard]] std::optional<std::uint64_t> lineNumberOf(const Ubiquitty::Terminal& terminal, LineID line);

} // namespace Ubiquitty::Screen
