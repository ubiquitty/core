// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// LineIdentity — the names Core gives lines, and the few records that keep a
// name on a line while rows are renumbered (Core/Screen/LogicalPosition.h).
//
// History names are derived: a line's name is its logical line number plus
// the bias of the segment the number falls in. A segment starts wherever the
// numbering of lines not yet completed stopped being trustworthy:
//
// - `retireHistory` renumbers the lines a partial clear leaves, and shifts
//   every segment by the same amount, so the survivors keep their names. The
//   lines still on the screen start a fresh segment, above every name handed
//   out, because the lines the clear deleted had names their numbers are
//   about to take.
// - An in-place move on the primary screen while a line carried from history
//   is on it starts a fresh segment too: an IL above the carried line gives
//   its number to the inserted line, which must not inherit its name.
//
// The segments are monotone in both number and name, so a name maps back to
// one number by a binary search, and a name between two segments names
// nothing.
//
// Pins are the records. A capture on the screen pins its line under a name
// with `pinnedLineIDBit`, from a counter that never repeats; a pull-back that
// carries history lines onto the screen pins each under the derived name its
// positions already hold. A pin names its line's first row by row ordinal
// (`LineStore::nextOrdinal`), which a scroll into history, an eviction, a
// pull-back and a push all leave alone, so no write path updates a pin.
// The cold paths that move rows in place, erase them, rewrap them, delete
// them or replace the buffer do, and each tests for pins first.
//
// A pin is never released by its holders: positions are plain data. Pins of
// lines evicted, destroyed or carried back into history unchanged are pruned
// when the table fills, and the oldest departed pin goes when nothing can be
// pruned; a position on a pruned pin resolves as a failure, never as another
// line.

#include "Base/FallibleVector.h"
#include "Core/GridBounds.h"
#include "Core/Screen/LogicalPosition.h"

#include <cstddef>
#include <cstdint>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Screen {

/// What a pin stands for.
enum class PinState : std::uint8_t {
    /// A line a capture on the screen pinned, under a name of the pin's own.
    Captured,
    /// A history line a pull-back carried onto the screen, under the derived
    /// name positions captured in history hold for it.
    Carried,
    /// The line was destroyed; positions on it resolve as `Destroyed`.
    Destroyed,
};

/// Where one named line is.
///
/// A position on a pin counts its cells from the pin's row, after
/// `cellOffset` written cells of the line before it: zero, unless a rewrap
/// found the pinned row continuing a line a wrap had joined it to.
///
/// A band pin stands for the cells of its parent's row that a column band
/// (DECSLRM) moved, once a band scroll has split the row. The parent's row
/// holds other cells in those columns now, so a walk of it counts the band as
/// the `bandWritten` cells it held, and a position among them resolves through
/// the band pin, on its row and in its columns. A band pin is one row, and is
/// moved only by moves of whole rows or of a band that holds its own.
struct LinePin {
    std::uint64_t id = 0;
    /// The ordinal of the pinned row: the store's ordinal space on the primary
    /// buffer, where a screen row's ordinal is the one it will be stored under;
    /// the screen row on the alternate buffer.
    std::uint64_t ordinal = 0;
    std::uint32_t cellOffset = 0;
    /// Bumped from the buffer's counter by every erase of the line's rows.
    std::uint32_t revision = 0;
    /// The parent of a band pin; zero for every other pin.
    std::uint64_t parentID = 0;
    std::int16_t bandLeft = 0;
    std::int16_t bandRight = -1;
    /// The written cells the band held when it left the parent's row.
    std::uint16_t bandWritten = 0;
    PinState state = PinState::Captured;

    [[nodiscard]] bool isBandPin() const { return parentID != 0; }
};

/// The pins of one buffer.
struct BufferPins {
    /// At least two pins for every row a screen can have, so a table full of
    /// live pins is impossible and a full table always has a departed pin to
    /// give up.
    static constexpr std::size_t maxPins = 2 * static_cast<std::size_t>(maxRows);

    FallibleVector<LinePin> pins;
    /// The next pinned name's serial; never reused, across RIS too.
    std::uint64_t nextSerial = 1;
    /// The next erase revision; zero is the revision a line has never been
    /// erased at.
    std::uint32_t nextRevision = 1;

    [[nodiscard]] bool isEmpty() const { return pins.empty(); }
};

/// One run of line numbers whose names are the numbers offset by `bias`.
struct NameSegment {
    std::uint64_t firstNumber = 0;
    std::int64_t bias = 0;
};

/// The counters a reconstruction carries, so no name is ever given twice.
struct LineIdentityContinuation {
    /// Whether any name was handed out; without one there is nothing to carry.
    bool hasNames = false;
    std::uint64_t freshName = 0;
    std::uint64_t primarySerial = 1;
    std::uint64_t alternateSerial = 1;
};

/// A terminal's line names: both buffers' pins and the primary's segments.
struct LineIdentity {
    /// The most segments kept. Each one is a destructive clear or an in-place
    /// structural change over carried lines inside the retained history; past
    /// this many the oldest is given up, and the names in it resolve as
    /// evicted.
    static constexpr std::size_t maxSegments = 1024;

    BufferPins primary;
    BufferPins alternate;
    /// Ascending, never empty once used; an empty list is one segment of bias
    /// zero from number zero.
    FallibleVector<NameSegment> segments;
    /// Numbers below this name nothing: their segment was given up, or a
    /// clear cut their line.
    std::uint64_t minNumber = 0;
    /// One past the highest derived name handed out to a position or a pin.
    /// A fresh segment's names start at or above it, since a carried line's
    /// name can sit above every number the store still derives from.
    std::uint64_t minFreshName = 0;

    [[nodiscard]] BufferPins& pinsOf(Buffer buffer) { return buffer == Buffer::Alternate ? alternate : primary; }
    [[nodiscard]] const BufferPins& pinsOf(Buffer buffer) const { return buffer == Buffer::Alternate ? alternate : primary; }
};

/// The buffer a terminal shows.
[[nodiscard]] Buffer activeBuffer(const Ubiquitty::Terminal& terminal);

/// Whether the active buffer has any pin, the test every maintenance hook's
/// caller makes before paying for the call.
[[nodiscard]] bool hasActivePins(const Ubiquitty::Terminal& terminal);

/// Returns a history line's derived name.
///
/// - Parameters:
///   - identity: The terminal's line names, or null when none was handed out,
///     which derives every name from its number alone.
///   - number: The line's logical number.
/// - Returns: The name, with `pinnedLineIDBit` clear.
[[nodiscard]] std::uint64_t derivedName(const LineIdentity* identity, std::uint64_t number);

/// Returns the number a derived name names.
///
/// - Returns: The number; `Evicted` for a name below every segment kept, and
///   `Destroyed` for a pinned name or one between two segments, which no line
///   was ever given.
[[nodiscard]] std::expected<std::uint64_t, PositionFailure> numberForDerivedName(const LineIdentity* identity, std::uint64_t name);

/// Follows full-width rows the active buffer moved inside a band.
///
/// Called before the move. A pinned line whose first row is on the band moves
/// with it, and is destroyed when the move carries that row off the band; a
/// line the band edge cuts is erased, because the rows on one side moved and
/// the rows on the other did not. On the primary buffer a carried line on the
/// screen starts a fresh segment.
///
/// - Parameters:
///   - terminal: The terminal.
///   - top: The band's first row.
///   - bottom: The band's last row.
///   - rows: How far the content moves; positive is up.
void notePositionsShiftedInPlace(Ubiquitty::Terminal& terminal, int top, int bottom, int rows);

/// Follows a column band (DECSLRM) the active buffer moved inside rows that
/// stay put.
///
/// Called before the move. A one-row pinned line the band crosses keeps its
/// row for its cells outside the band and gains a band pin for the cells in
/// it, which moves with the band and is destroyed when the move carries it off
/// the band; a band pin inside the band moves the same way. A pinned line of
/// several rows the band crosses is erased, because its rows moved apart.
///
/// - Parameters:
///   - terminal: The terminal.
///   - top: The band's first row.
///   - bottom: The band's last row.
///   - left: The band's first column.
///   - right: The band's last column.
///   - rows: How far the content moves; positive is up.
void notePositionsShiftedInBand(Ubiquitty::Terminal& terminal, int top, int bottom, int left, int right, int rows);

/// Erases the pinned lines that have a row in `[top, bottom]` of the active
/// buffer.
///
/// Their cells were erased, shifted along the row, or overwritten by a
/// rectangle copy.
///
/// - Parameters:
///   - terminal: The terminal.
///   - top: The first screen row rewritten.
///   - bottom: The last screen row rewritten.
void notePositionsRewritten(Ubiquitty::Terminal& terminal, int top, int bottom);

/// Erases every pinned line on a buffer's screen.
///
/// Its rows were replaced whole, by a clear or by a keyframe.
///
/// - Parameters:
///   - terminal: The terminal.
///   - buffer: The buffer whose screen was replaced.
void notePositionsScreenReplaced(Ubiquitty::Terminal& terminal, Buffer buffer);

/// Accounts for the newest primary history rows being deleted rather than
/// moved onto the screen, and names the lines still to complete afresh,
/// because the deleted lines' numbers go to them.
///
/// Called after the store dropped the rows: pins on them are destroyed, and
/// pins on the screen keep their rows, whose ordinals the drop lowered.
///
/// - Parameters:
///   - terminal: The terminal.
///   - previousNextOrdinal: `LineStore::nextOrdinal` before the drop.
void notePositionsHistoryTailDeleted(Ubiquitty::Terminal& terminal, std::uint64_t previousNextOrdinal);

/// Carries the history lines a pull-back moved onto the primary screen.
///
/// Called after the store dropped the rows and the screen holds them, at the
/// ordinals they had: a line whose first row was stored before the pull-back
/// gets a carried pin under its derived name, so a position captured on it in
/// history follows it through edits on the screen.
///
/// - Parameters:
///   - terminal: The terminal.
///   - previousNextOrdinal: `LineStore::nextOrdinal` before the drop.
void notePositionsPulledFromHistory(Ubiquitty::Terminal& terminal, std::uint64_t previousNextOrdinal);

/// Accounts for blank rows a bottom-anchored grow inserts between the history
/// it pulls back and the screen content it moves down. Called before the
/// store drops the pulled rows.
///
/// - Parameters:
///   - terminal: The terminal.
///   - pulledRows: History rows the grow moves onto the screen.
///   - insertedRows: Blank rows it inserts above them.
void notePositionsBlankRowsInsertedAtSeam(Ubiquitty::Terminal& terminal, int pulledRows, int insertedRows);

/// Keeps every derived name through `retireHistory`'s renumbering.
///
/// Called before the store restarts its numbering.
///
/// - Parameters:
///   - terminal: The terminal.
///   - newBase: The number the front line takes.
void notePositionsRenumbered(Ubiquitty::Terminal& terminal, std::uint64_t newBase);

/// Gives every line not yet completed a name above every name handed out.
///
/// The open line's stored rows keep their history name on a carried pin.
/// Whatever changes the number a carried or deleted line's successors take
/// calls this, so no later line derives a name a position already holds.
///
/// - Parameter terminal: The terminal.
void startFreshLineNames(Ubiquitty::Terminal& terminal);

/// Destroys every pin of a buffer.
///
/// For rows replaced by rows that are not the same lines: a restored or
/// prepended store, an undone scroll.
///
/// - Parameters:
///   - terminal: The terminal.
///   - buffer: The buffer whose pins go.
void forgetPositions(Ubiquitty::Terminal& terminal, Buffer buffer);

/// Destroys the pins a resize left below the last row.
///
/// - Parameter terminal: The terminal, at its new size.
void notePositionsResized(Ubiquitty::Terminal& terminal);

/// Keeps the primary screen's pins on their rows while the history beneath it
/// is rebuilt without the screen moving.
///
/// A Sync replica's history plane projects the lines the screen plane has
/// already scrolled away, so its store grows and is rebuilt under a screen
/// that holds still. A pin on the screen keeps its row; one in the rebuilt
/// history is destroyed, because its row now holds whatever the projection
/// put there.
class ScreenPinsAcrossHistoryRebuild {
public:
    /// Records the store's ordinals before the rebuild.
    ///
    /// - Parameter terminal: The replica whose history is rebuilt.
    explicit ScreenPinsAcrossHistoryRebuild(Ubiquitty::Terminal& terminal);
    /// Moves the screen's pins to the ordinals the rebuilt store gives their
    /// rows, and destroys the pins in the rebuilt history.
    ~ScreenPinsAcrossHistoryRebuild();
    ScreenPinsAcrossHistoryRebuild(const ScreenPinsAcrossHistoryRebuild&) = delete;
    ScreenPinsAcrossHistoryRebuild& operator=(const ScreenPinsAcrossHistoryRebuild&) = delete;

private:
    Ubiquitty::Terminal& m_terminal;
    std::uint64_t m_nextOrdinal;
    long long m_appended;
    int m_count;
};

/// The pins of a primary rewrap, held by line number while the rows move.
struct PositionReflow {
    /// One pin's place in the joined logical lines: a rewrap keeps both.
    struct Place {
        std::uint64_t number = 0;
        std::uint32_t cellOffset = 0;
    };
    FallibleVector<Place> places;
    std::uint64_t headBefore = 0;
    std::uint64_t nextOrdinalBefore = 0;
    bool hadOpenLine = false;
    bool isActive = false;
};

/// Records every primary pin's line number before a rewrap.
///
/// - Parameters:
///   - terminal: The terminal about to rewrap.
///   - reflow: Filled with each pin's place; handed to
///     ``finishPositionReflow``.
void beginPositionReflow(Ubiquitty::Terminal& terminal, PositionReflow& reflow);

/// Puts every primary pin back on its line's first row after a rewrap.
///
/// The history lines the rewrap moved onto the screen are carried, and a
/// fresh name segment starts after them.
///
/// - Parameters:
///   - terminal: The terminal, rewrapped.
///   - reflow: What ``beginPositionReflow`` recorded.
void finishPositionReflow(Ubiquitty::Terminal& terminal, PositionReflow& reflow);

/// Returns the counters a reconstruction must carry.
///
/// - Parameter terminal: The terminal about to be reconstructed.
/// - Returns: Its name counters; empty when it handed out no name.
[[nodiscard]] LineIdentityContinuation lineIdentityContinuation(const Ubiquitty::Terminal& terminal);

/// Resumes the counters after a reconstruction, before any line is named.
///
/// - Parameters:
///   - terminal: The reconstructed terminal.
///   - continuation: What ``lineIdentityContinuation`` returned.
void restoreLineIdentity(Ubiquitty::Terminal& terminal, const LineIdentityContinuation& continuation);

} // namespace Ubiquitty::Screen
