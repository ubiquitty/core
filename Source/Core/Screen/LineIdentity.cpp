// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// LineIdentity — see LineIdentity.h and LogicalPosition.h. Every function here
// runs under the model lock, and none runs per byte or per frame: capture and
// resolution are a selection's or a search's, and the maintenance hooks are
// the cold paths that move, erase or rewrap rows.

#include "Core/Screen/LineIdentity.h"

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/LineStore.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollLine.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace Ubiquitty::Screen {

namespace {

// One buffer's rows as positions see them: the stored rows oldest first, then
// the screen's, under one index. Row `a`'s ordinal is `front + a`, which is the
// row's store ordinal on the primary buffer and its screen row on the
// alternate, where nothing is stored.
class RowSpace {
public:
    RowSpace(const Ubiquitty::Terminal& terminal, Buffer buffer)
        : m_grid(terminal.grid.get())
        , m_store(buffer == Buffer::Primary ? terminal.lineStore.get() : nullptr)
        , m_screen(buffer == Buffer::Alternate ? ScreenMode::Alternate : ScreenMode::Primary)
        , m_rows(terminal.grid ? std::max(terminal.rows, 0) : 0)
        , m_columns(std::max(terminal.columns, 1))
        , m_history(m_store ? m_store->count() : 0)
        , m_front(m_store ? m_store->frontOrdinal() : 0)
    {
    }

    [[nodiscard]] int total() const { return m_history + m_rows; }
    [[nodiscard]] int history() const { return m_history; }
    [[nodiscard]] int rows() const { return m_rows; }
    [[nodiscard]] int columns() const { return m_columns; }
    [[nodiscard]] std::uint64_t front() const { return m_front; }
    [[nodiscard]] std::uint64_t base() const { return m_front + static_cast<std::uint64_t>(m_history); }
    [[nodiscard]] const Ubiquitty::LineStore* store() const { return m_store; }
    [[nodiscard]] std::uint64_t ordinalOf(int row) const { return m_front + static_cast<std::uint64_t>(row); }

    // The row an ordinal names, or nothing when it is evicted or past the
    // screen's last row.
    [[nodiscard]] std::optional<int> rowOf(std::uint64_t ordinal) const
    {
        if (ordinal < m_front || ordinal - m_front >= static_cast<std::uint64_t>(total()))
            return std::nullopt;
        return static_cast<int>(ordinal - m_front);
    }

    [[nodiscard]] bool isWrapped(int row) const
    {
        if (row < m_history)
            return m_store->isWrapped(row);
        return m_grid->rowMetaAt(screenIndex(), row - m_history).has(RowFlag::Wrapped);
    }

    // The front row starts a line unless its line's first rows were evicted;
    // with nothing stored the screen's first row is the one the store will
    // take next, and the same continuity answers for it.
    [[nodiscard]] bool startsLine(int row) const
    {
        if (row > 0)
            return !isWrapped(row - 1);
        return !m_store || m_store->startsLine(0);
    }

    // A row's cells up to its last written one: the cells past it are the
    // row's blank tail, or the pad a wide character left at a margin, which
    // the reflow drops at a join. Valid until another row is read.
    struct Cells {
        const Ubiquitty::Cell* data = nullptr;
        int length = 0;

        [[nodiscard]] bool isWritten(int column) const { return column >= 0 && column < length && !isUnwrittenCell(data[column]); }
    };

    [[nodiscard]] Cells cellsOf(int row) const
    {
        if (row < m_history) {
            const ScrollLine* line = m_store->at(row);
            return line ? Cells { line->cells, Screen::writtenLength(*line) } : Cells {};
        }
        const int screenRow = row - m_history;
        const RowView view = m_grid->rowCells(m_screen, Ubiquitty::Row(screenRow));
        if (!view.data)
            return {};
        int length = std::min(m_grid->rowContentWidth(m_screen, Ubiquitty::Row(screenRow)), view.count);
        while (length > 0 && isUnwrittenCell(view.data[length - 1]))
            --length;
        return { view.data, length };
    }

    // The cells of a row that something wrote, which is what a position
    // counts. A gap a tab or a cursor move left is not counted: a rewrap can
    // leave it at a row's end, where the next rewrap drops it.
    [[nodiscard]] int writtenCount(int row, int from = 0, int to = std::numeric_limits<int>::max()) const
    {
        const Cells cells = cellsOf(row);
        int count = 0;
        for (int column = std::max(from, 0); column < std::min(to, cells.length); ++column)
            count += cells.isWritten(column) ? 1 : 0;
        return count;
    }

    // A cell a wide or sized character's trailing columns stand on names the
    // character, so a column on one moves left to the run's lead.
    [[nodiscard]] int leadColumn(int row, int column) const
    {
        const Ubiquitty::Cell* cells = nullptr;
        int count = 0;
        if (row < m_history) {
            const ScrollLine* line = m_store->at(row);
            if (line) {
                cells = line->cells;
                count = line->length;
            }
        } else {
            const RowView view = m_grid->rowCells(m_screen, Ubiquitty::Row(row - m_history));
            cells = view.data;
            count = view.count;
        }
        while (column > 0 && column < count && cells && cells[column].pen.attributes.contains(VT::Attribute::WideContinuation))
            --column;
        return column;
    }

    // The first row of the line holding `row`.
    [[nodiscard]] int lineStart(int row) const
    {
        while (row > 0 && isWrapped(row - 1))
            --row;
        return row;
    }

    // The last row of the line starting at `row`.
    [[nodiscard]] int lineEnd(int row) const
    {
        const int last = total() - 1;
        while (row < last && isWrapped(row))
            ++row;
        return row;
    }

private:
    [[nodiscard]] int screenIndex() const { return m_screen == ScreenMode::Alternate ? 1 : 0; }

    const Ubiquitty::CellGrid* m_grid;
    const Ubiquitty::LineStore* m_store;
    ScreenMode m_screen;
    int m_rows;
    int m_columns;
    int m_history;
    std::uint64_t m_front;
};

[[nodiscard]] std::uint64_t headNumber(const Ubiquitty::Terminal& terminal)
{
    return terminal.lineStore ? terminal.lineStore->headLineNumber() : terminal.historyEpochBase;
}

// The logical number of the line starting at `row` of the primary buffer: a
// stored line's own, a screen line's the one it takes when it completes.
[[nodiscard]] std::optional<std::uint64_t> lineNumberAt(const Ubiquitty::Terminal& terminal, const RowSpace& space, int row)
{
    if (row < space.history()) {
        const auto position = space.store()->positionAt(row);
        if (!position)
            return std::nullopt;
        return position->line;
    }
    std::uint64_t number = headNumber(terminal);
    for (int screenRow = space.history(); screenRow < row; ++screenRow)
        number += space.isWrapped(screenRow) ? 0 : 1;
    return number;
}

// The first row of primary line `number`, or nothing when no row holds it.
[[nodiscard]] std::optional<int> firstRowOfLine(const Ubiquitty::Terminal& terminal, const RowSpace& space, std::uint64_t number)
{
    const HistoryAddress address = historyAddress(terminal);
    if (number < address.oldest || number < address.clearedThrough)
        return std::nullopt;
    const Ubiquitty::LineStore* store = space.store();
    if (number < address.head) {
        const auto rows = store ? store->logicalLine(number) : std::nullopt;
        if (!rows)
            return std::nullopt;
        return rows->firstIndex;
    }
    if (number == address.head && store) {
        if (const auto open = store->openLine())
            return open->firstIndex;
    }
    std::uint64_t current = address.head;
    for (int row = space.history(); row < space.total(); ++row) {
        if (space.startsLine(row) && current == number)
            return row;
        if (!space.isWrapped(row))
            ++current;
        if (current > number)
            break;
    }
    return std::nullopt;
}

// The terminal's line names, made on the first name handed out. Null only
// when the allocation is refused, which refuses the capture.
[[nodiscard]] LineIdentity* ensureIdentity(const Ubiquitty::Terminal& terminal)
{
    if (!terminal.lineIdentity)
        terminal.lineIdentity.reset(new (std::nothrow) LineIdentity);
    return terminal.lineIdentity.get();
}

[[nodiscard]] std::uint64_t pinnedName(BufferPins& pins)
{
    return pinnedLineIDBit | (pins.nextSerial++ & ~pinnedLineIDBit);
}

void bumpRevision(BufferPins& pins, LinePin& pin)
{
    pin.revision = pins.nextRevision++;
    if (pins.nextRevision == 0)
        pins.nextRevision = 1;
}

[[nodiscard]] LinePin* findPinNamed(BufferPins& pins, std::uint64_t name)
{
    for (LinePin& pin : pins.pins) {
        if (pin.id == name)
            return &pin;
    }
    return nullptr;
}

[[nodiscard]] const LinePin* findPinNamed(const BufferPins& pins, std::uint64_t name)
{
    for (const LinePin& pin : pins.pins) {
        if (pin.id == name)
            return &pin;
    }
    return nullptr;
}

// The band child of a split pin whose columns hold `column`.
[[nodiscard]] const LinePin* bandAt(const BufferPins& pins, const LinePin& parent, int column)
{
    for (const LinePin& pin : pins.pins) {
        if (pin.parentID == parent.id && column >= pin.bandLeft && column <= pin.bandRight)
            return &pin;
    }
    return nullptr;
}

[[nodiscard]] bool hasBandChildren(const BufferPins& pins, const LinePin& parent)
{
    return std::ranges::any_of(pins.pins, [&](const LinePin& pin) { return pin.parentID == parent.id; });
}

// Removes what no position can still need first: destroyed and evicted pins,
// pins of rows past the screen, and carried pins back in history unchanged
// under the name history derives for them anyway.
void prunePins(const Ubiquitty::Terminal& terminal, Buffer buffer, BufferPins& pins)
{
    const RowSpace space(terminal, buffer);
    // A band pin answers for its parent's band columns, destroyed or not, for
    // as long as the parent can be asked.
    const auto hasLiveParent = [&](const LinePin& pin) {
        if (!pin.isBandPin())
            return false;
        const LinePin* parent = findPinNamed(pins, pin.parentID);
        return parent && parent->state != PinState::Destroyed && space.rowOf(parent->ordinal).has_value();
    };
    const auto isRedundant = [&](const LinePin& pin) {
        if (hasLiveParent(pin))
            return false;
        if (pin.state == PinState::Destroyed || pin.isBandPin())
            return true;
        const auto row = space.rowOf(pin.ordinal);
        if (!row)
            return true;
        if (pin.state != PinState::Carried || pin.revision != 0 || *row >= space.history() || !space.startsLine(*row))
            return false;
        const auto number = lineNumberAt(terminal, space, *row);
        return number && derivedName(terminal.lineIdentity.get(), *number) == pin.id;
    };
    // Judged before any pin moves, since a band pin's verdict reads its parent.
    std::array<bool, BufferPins::maxPins> isKept {};
    const std::size_t count = std::min(pins.pins.size(), BufferPins::maxPins);
    for (std::size_t index = 0; index < count; ++index)
        isKept[index] = !isRedundant(pins.pins[index]);
    std::size_t kept = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (isKept[index])
            pins.pins[kept++] = pins.pins[index];
    }
    while (pins.pins.size() > kept)
        pins.pins.pop_back();
}

// Adds a pin, pruning and then giving up the oldest departed pin when the
// table is full. A band pin goes only with its parent: without it, a walk of
// the parent's row would count the cells that moved into the band.
[[nodiscard]] LinePin* addPin(const Ubiquitty::Terminal& terminal, Buffer buffer, BufferPins& pins, const LinePin& pin)
{
    if (pins.pins.size() >= BufferPins::maxPins)
        prunePins(terminal, buffer, pins);
    if (pins.pins.size() >= BufferPins::maxPins) {
        const std::uint64_t base = RowSpace(terminal, buffer).base();
        std::size_t oldest = pins.pins.size();
        for (std::size_t index = 0; index < pins.pins.size(); ++index) {
            const LinePin& candidate = pins.pins[index];
            if (!candidate.isBandPin() && candidate.ordinal < base && (oldest == pins.pins.size() || candidate.ordinal < pins.pins[oldest].ordinal))
                oldest = index;
        }
        if (oldest == pins.pins.size())
            return nullptr;
        const std::uint64_t givenUp = pins.pins[oldest].id;
        std::size_t kept = 0;
        for (std::size_t index = 0; index < pins.pins.size(); ++index) {
            if (pins.pins[index].id != givenUp && pins.pins[index].parentID != givenUp)
                pins.pins[kept++] = pins.pins[index];
        }
        while (pins.pins.size() > kept)
            pins.pins.pop_back();
    }
    if (!pins.pins.tryPushBack(pin))
        return nullptr;
    return &pins.pins[pins.pins.size() - 1];
}

// The pin of the line whose first row has `ordinal`, made when there is none.
[[nodiscard]] LinePin* pinLine(const Ubiquitty::Terminal& terminal, Buffer buffer, std::uint64_t ordinal)
{
    LineIdentity* identity = ensureIdentity(terminal);
    if (!identity)
        return nullptr;
    BufferPins& pins = identity->pinsOf(buffer);
    // A pin a band scroll split answers for other cells in its band columns
    // now, so a capture there takes a pin of its own.
    for (LinePin& pin : pins.pins) {
        if (pin.state != PinState::Destroyed && pin.ordinal == ordinal && !pin.isBandPin() && pin.cellOffset == 0 && !hasBandChildren(pins, pin))
            return &pin;
    }
    LinePin pin;
    pin.id = pinnedName(pins);
    pin.ordinal = ordinal;
    return addPin(terminal, buffer, pins, pin);
}

void noteNameHandedOut(LineIdentity& identity, std::uint64_t name)
{
    identity.minFreshName = std::max(identity.minFreshName, name + 1);
}

// Whether a live pin other than one on `ordinal` answers to a derived name.
[[nodiscard]] bool isNameHeldElsewhere(const LineIdentity& identity, std::uint64_t name, std::uint64_t ordinal)
{
    const LinePin* holder = findPinNamed(identity.primary, name);
    return holder && holder->state != PinState::Destroyed && holder->ordinal != ordinal;
}

// Pins a primary line under the derived name history gave it, unless a pin
// already carries that name. Only a terminal that handed out names carries
// lines: without one no position can hold a derived name.
//
// A carried line's number can change on the screen, by a wrap or an erase
// above it, so whoever carries lines closes their names afterwards with
// ``startFreshLineNames``: no later line derives a carried name again.
void carryLine(Ubiquitty::Terminal& terminal, std::uint64_t number, std::uint64_t ordinal)
{
    // A number below the minimum has no name to carry: its line lost it.
    if (!terminal.lineIdentity || number < terminal.lineIdentity->minNumber)
        return;
    BufferPins& pins = terminal.lineIdentity->primary;
    const std::uint64_t name = derivedName(terminal.lineIdentity.get(), number);
    noteNameHandedOut(*terminal.lineIdentity, name);
    // A pin already carrying the name is on this line, which it follows, or
    // on the line that held the name first, which keeps it.
    if (findPinNamed(pins, name))
        return;
    LinePin pin;
    pin.id = name;
    pin.ordinal = ordinal;
    pin.state = PinState::Carried;
    (void)addPin(terminal, Buffer::Primary, pins, pin);
}

// The screen rows a pinned line covers, and the screen row of its first row
// when that row is on the screen rather than stored.
struct ScreenExtent {
    int startRow = -1;
    int first = 0;
    int last = -1;
};

[[nodiscard]] std::optional<ScreenExtent> screenExtentOf(const RowSpace& space, const LinePin& pin, int openLineFirst)
{
    const auto row = space.rowOf(pin.ordinal);
    if (!row)
        return std::nullopt;
    ScreenExtent extent;
    if (*row >= space.history()) {
        extent.startRow = *row - space.history();
        extent.first = extent.startRow;
    } else if (*row == openLineFirst) {
        extent.first = 0;
    } else {
        return std::nullopt;
    }
    extent.last = pin.isBandPin() ? extent.first : space.lineEnd(space.history() + extent.first) - space.history();
    return extent;
}

[[nodiscard]] int openLineFirstRow(const RowSpace& space)
{
    if (!space.store() || !space.history() || !space.isWrapped(space.history() - 1))
        return -1;
    return space.lineStart(space.history() - 1);
}

// The column a place in a gap stands on: `voidCells` past the run's start,
// short of the written cell at `gapEnd` that ends the gap, or anywhere up to
// the last column in the void past the line when `gapEnd` is negative.
[[nodiscard]] int placeInGap(int runStart, int gapEnd, std::uint16_t voidCells, int lastColumn)
{
    int column = runStart + voidCells;
    if (gapEnd >= 0) {
        if (voidCells == 0)
            column = gapEnd;
        else if (column >= gapEnd)
            column = runStart < gapEnd ? gapEnd - 1 : gapEnd;
    }
    return std::clamp(column, 0, lastColumn);
}

// Where a walk ends: a place, or the band child whose cells hold it and how
// many of the band's written cells come before it.
struct Walk {
    PhysicalPosition place;
    const LinePin* band = nullptr;
    std::uint64_t intoBand = 0;
};

// Walks a line from a row to the cell a position names, `skipped` written
// cells of the line already behind that row. A place past the line's written
// cells stands at its end: the void a selection was dragged into, or text a
// rewrite shortened, still has a place on the line's last row.
//
// A split pin's row holds other cells in its band columns now; the walk counts
// the band's cells as the split recorded them, and hands a place among them
// to the band child.
[[nodiscard]] Walk walkToCell(const RowSpace& space, int first, std::uint64_t skipped, const LogicalPosition& position, const BufferPins* pins = nullptr, const LinePin* parent = nullptr)
{
    const std::uint64_t target = skipped + position.cell;
    const int last = space.total() - 1;
    const int lastColumn = space.columns() - 1;
    std::uint64_t count = 0;
    for (int row = first;; ++row) {
        const RowSpace::Cells cells = space.cellsOf(row);
        const bool isLastRow = row >= last || !space.isWrapped(row);
        const bool hasBands = row == first && parent && pins;
        const int end = hasBands ? lastColumn + 1 : cells.length;
        // The first column after the row's previous written cell.
        int runStart = 0;
        for (int column = 0; column < end; ++column) {
            if (hasBands) {
                if (const LinePin* band = bandAt(*pins, *parent, column)) {
                    if (target < count + band->bandWritten)
                        return { .place = {}, .band = band, .intoBand = target - count };
                    count += band->bandWritten;
                    if (band->bandWritten)
                        runStart = band->bandRight + 1;
                    column = band->bandRight;
                    continue;
                }
            }
            if (!cells.isWritten(column))
                continue;
            if (count == target)
                return { .place = { .absoluteRow = row, .column = placeInGap(runStart, column, position.voidCells, lastColumn) }, .band = nullptr, .intoBand = 0 };
            ++count;
            runStart = column + 1;
        }
        if (isLastRow)
            return { .place = { .absoluteRow = row, .column = placeInGap(runStart, -1, position.voidCells, lastColumn) }, .band = nullptr, .intoBand = 0 };
    }
}

// Walks a band child's columns to the written cell a place names.
[[nodiscard]] PhysicalPosition walkInBand(const RowSpace& space, int row, const LinePin& band, std::uint64_t intoBand, std::uint16_t voidCells)
{
    const RowSpace::Cells cells = space.cellsOf(row);
    std::uint64_t count = 0;
    int runStart = band.bandLeft;
    for (int column = band.bandLeft; column <= band.bandRight; ++column) {
        if (!cells.isWritten(column))
            continue;
        if (count == intoBand)
            return { .absoluteRow = row, .column = placeInGap(runStart, column, voidCells, band.bandRight) };
        ++count;
        runStart = column + 1;
    }
    // Printing over the band since left it fewer written cells: its end.
    return { .absoluteRow = row, .column = std::min<int>(runStart, band.bandRight) };
}

// Checks that a pin still names a row, at the revision a position holds.
[[nodiscard]] std::expected<int, PositionFailure> pinnedRow(const RowSpace& space, const LinePin& pin, const LogicalPosition& position, RevisionCheck check)
{
    if (pin.state == PinState::Destroyed)
        return std::unexpected(PositionFailure::Destroyed);
    if (check == RevisionCheck::Exact && pin.revision != position.revision)
        return std::unexpected(PositionFailure::Erased);
    if (pin.ordinal < space.front())
        return std::unexpected(PositionFailure::Evicted);
    const auto row = space.rowOf(pin.ordinal);
    if (!row)
        return std::unexpected(PositionFailure::Destroyed);
    return *row;
}

// Resolves a position through a pin: its band child for a cell a band scroll
// moved, else the pin's own row.
[[nodiscard]] std::expected<PhysicalPosition, PositionFailure> resolveOnPin(const RowSpace& space, const BufferPins& pins, const LinePin& pin, const LogicalPosition& position, RevisionCheck check)
{
    // Positions hold a parent's name, never a band child's.
    if (pin.isBandPin())
        return std::unexpected(PositionFailure::Destroyed);
    const auto row = pinnedRow(space, pin, position, check);
    if (!row)
        return std::unexpected(row.error());
    const bool isSplit = hasBandChildren(pins, pin);
    const Walk walk = walkToCell(space, *row, pin.cellOffset, position, isSplit ? &pins : nullptr, isSplit ? &pin : nullptr);
    if (!walk.band)
        return walk.place;
    const auto bandRow = pinnedRow(space, *walk.band, position, check);
    if (!bandRow)
        return std::unexpected(bandRow.error());
    return walkInBand(space, *bandRow, *walk.band, walk.intoBand, position.voidCells);
}

[[nodiscard]] std::expected<PhysicalPosition, PositionFailure> resolveInBuffer(const Ubiquitty::Terminal& terminal, const LogicalPosition& position, RevisionCheck check)
{
    if (!terminal.grid)
        return std::unexpected(PositionFailure::Destroyed);
    const RowSpace space(terminal, position.buffer);
    const LineIdentity* identity = terminal.lineIdentity.get();
    if (const LinePin* pin = identity ? findPinNamed(identity->pinsOf(position.buffer), position.line.value) : nullptr)
        return resolveOnPin(space, identity->pinsOf(position.buffer), *pin, position, check);
    if (position.line.isPinned() || position.buffer == Buffer::Alternate)
        return std::unexpected(PositionFailure::Destroyed);
    if (check == RevisionCheck::Exact && position.revision != 0)
        return std::unexpected(PositionFailure::Erased);
    const auto number = numberForDerivedName(identity, position.line.value);
    if (!number)
        return std::unexpected(number.error());
    const HistoryAddress address = historyAddress(terminal);
    if (*number < address.clearedThrough || *number < address.oldest)
        return std::unexpected(PositionFailure::Evicted);
    if (*number > address.head)
        return std::unexpected(PositionFailure::Destroyed);
    const auto first = firstRowOfLine(terminal, space, *number);
    if (!first)
        return std::unexpected(*number < address.head ? PositionFailure::Evicted : PositionFailure::Destroyed);
    // A line whose first rows were evicted has lost the cells its positions
    // count from.
    if (!space.startsLine(*first))
        return std::unexpected(PositionFailure::Evicted);
    // A derived name on the screen is the open line's, whose first rows are
    // stored; every other screen line is reached through a pin.
    if (*first >= space.history())
        return std::unexpected(PositionFailure::Destroyed);
    return walkToCell(space, *first, 0, position).place;
}

// Drops segments wholly below the oldest number a name can still need, and
// gives up the oldest segment when the list is full.
void pruneSegments(const Ubiquitty::Terminal& terminal, LineIdentity& identity)
{
    const HistoryAddress address = historyAddress(terminal);
    const std::uint64_t floor = std::max({ address.oldest, address.clearedThrough, identity.minNumber });
    std::size_t firstKept = 0;
    while (firstKept + 1 < identity.segments.size() && identity.segments[firstKept + 1].firstNumber <= floor)
        ++firstKept;
    if (firstKept)
        identity.segments.erase(identity.segments.begin(), identity.segments.begin() + static_cast<std::ptrdiff_t>(firstKept));
    if (identity.segments.size() >= LineIdentity::maxSegments) {
        identity.minNumber = identity.segments[1].firstNumber;
        identity.segments.erase(identity.segments.begin());
    }
}

[[nodiscard]] bool ensureSegments(LineIdentity& identity)
{
    if (!identity.segments.empty())
        return true;
    if (!identity.segments.tryReserve(4))
        return false;
    return static_cast<bool>(identity.segments.tryPushBack(NameSegment {}));
}

[[nodiscard]] std::uint64_t maxHeadNumber(const Ubiquitty::Terminal& terminal)
{
    const std::uint64_t head = headNumber(terminal);
    return terminal.lineStore ? std::max(terminal.lineStore->maxHeadLineNumber(), head) : head;
}

// The lowest name no line can hold yet: above every stored line's name, every
// name handed out, and every number the store ever reached.
[[nodiscard]] std::uint64_t freshNameFloor(const Ubiquitty::Terminal& terminal, const LineIdentity& identity)
{
    const std::uint64_t head = headNumber(terminal);
    // Only a number a segment covers derives a name; one below them has none.
    const auto isDerivable = [&identity](std::uint64_t number) {
        return number >= identity.minNumber && (identity.segments.empty() || number >= identity.segments[0].firstNumber);
    };
    std::uint64_t floor = identity.minFreshName;
    if (const std::uint64_t reached = maxHeadNumber(terminal); isDerivable(reached))
        floor = std::max(floor, derivedName(&identity, reached) + 1);
    if (head > 0 && isDerivable(head - 1))
        floor = std::max(floor, derivedName(&identity, head - 1) + 1);
    return floor;
}

} // namespace

Buffer activeBuffer(const Ubiquitty::Terminal& terminal)
{
    return terminal.screens.isUsingAlternateScreen ? Buffer::Alternate : Buffer::Primary;
}

bool hasActivePins(const Ubiquitty::Terminal& terminal)
{
    return terminal.lineIdentity && !terminal.lineIdentity->pinsOf(activeBuffer(terminal)).isEmpty();
}

std::uint64_t derivedName(const LineIdentity* identity, std::uint64_t number)
{
    if (!identity || identity->segments.empty())
        return number & ~pinnedLineIDBit;
    // The last segment starting at or before the number.
    std::size_t low = 0;
    std::size_t high = identity->segments.size();
    while (high - low > 1) {
        const std::size_t middle = low + (high - low) / 2;
        if (identity->segments[middle].firstNumber <= number)
            low = middle;
        else
            high = middle;
    }
    // A number below every segment has no name; it never reaches a position.
    const std::int64_t name = static_cast<std::int64_t>(number) + identity->segments[low].bias;
    return name < 0 ? 0 : static_cast<std::uint64_t>(name) & ~pinnedLineIDBit;
}

std::expected<std::uint64_t, PositionFailure> numberForDerivedName(const LineIdentity* identity, std::uint64_t name)
{
    if (name & pinnedLineIDBit)
        return std::unexpected(PositionFailure::Destroyed);
    if (!identity)
        return name;
    if (identity->segments.empty()) {
        if (name < identity->minNumber)
            return std::unexpected(PositionFailure::Evicted);
        return name;
    }
    // Names ascend with segments, so the segment holding a name is the last
    // whose first name is at or below it.
    const auto firstName = [identity](std::size_t index) {
        return static_cast<std::int64_t>(identity->segments[index].firstNumber) + identity->segments[index].bias;
    };
    const auto value = static_cast<std::int64_t>(name);
    if (value < firstName(0))
        return std::unexpected(PositionFailure::Evicted);
    std::size_t low = 0;
    std::size_t high = identity->segments.size();
    while (high - low > 1) {
        const std::size_t middle = low + (high - low) / 2;
        if (firstName(middle) <= value)
            low = middle;
        else
            high = middle;
    }
    const auto number = static_cast<std::uint64_t>(value - identity->segments[low].bias);
    if (low + 1 < identity->segments.size() && number >= identity->segments[low + 1].firstNumber)
        return std::unexpected(PositionFailure::Destroyed);
    if (number < identity->minNumber)
        return std::unexpected(PositionFailure::Evicted);
    return number;
}

std::expected<LogicalPosition, PositionFailure> capturePosition(const Ubiquitty::Terminal& terminal, int absoluteRow, int column, std::uint16_t subCell)
{
    if (!terminal.grid || terminal.rows <= 0 || terminal.columns <= 0)
        return std::unexpected(PositionFailure::Destroyed);
    const Buffer buffer = activeBuffer(terminal);
    const RowSpace space(terminal, buffer);
    if (absoluteRow < 0 || absoluteRow >= space.total())
        return std::unexpected(PositionFailure::Destroyed);
    const int first = space.lineStart(absoluteRow);
    if (!space.startsLine(first))
        return std::unexpected(PositionFailure::Evicted);
    std::uint64_t cell = 0;
    for (int row = first; row < absoluteRow; ++row)
        cell += static_cast<std::uint64_t>(space.writtenCount(row));
    const int lead = space.leadColumn(absoluteRow, std::clamp(column, 0, space.columns() - 1));
    const RowSpace::Cells cells = space.cellsOf(absoluteRow);
    LogicalPosition position;
    position.subCell = subCell;
    position.buffer = buffer;
    for (int before = 0; before < std::min(lead, cells.length); ++before)
        cell += cells.isWritten(before) ? 1 : 0;
    // Past the written cells of a row that wraps is the pad a wide character
    // left at the margin, which names the next row's first written cell. Any
    // other unwritten place, in a gap or in the void past the line, is kept as
    // the columns it stands past the written cell before it on its row.
    const bool isPad = lead >= cells.length && space.isWrapped(absoluteRow);
    if (!cells.isWritten(lead) && !isPad) {
        int runStart = std::min(lead, cells.length);
        while (runStart > 0 && !cells.isWritten(runStart - 1))
            --runStart;
        position.voidCells = static_cast<std::uint16_t>(std::min(lead - runStart, static_cast<int>(std::numeric_limits<std::uint16_t>::max())));
    }
    if (cell > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(PositionFailure::Destroyed);
    position.cell = static_cast<std::uint32_t>(cell);
    LineIdentity* identity = ensureIdentity(terminal);
    if (!identity)
        return std::unexpected(PositionFailure::Destroyed);
    if (absoluteRow < space.history() && space.lineEnd(first) < space.history()) {
        // A stored line is immutable: its derived name is the whole of what
        // the position needs, unless a carried line whose numbering a wrap
        // moved already answers to that name.
        const auto number = lineNumberAt(terminal, space, first);
        if (!number || *number < identity->minNumber)
            return std::unexpected(PositionFailure::Evicted);
        const std::uint64_t name = derivedName(identity, *number);
        if (!isNameHeldElsewhere(*identity, name, space.ordinalOf(first))) {
            noteNameHandedOut(*identity, name);
            position.line = LineID { name };
            position.revision = 0;
            return position;
        }
    }
    const LinePin* pin = pinLine(terminal, buffer, space.ordinalOf(first));
    if (!pin)
        return std::unexpected(PositionFailure::Destroyed);
    position.line = LineID { pin->id };
    position.revision = pin->revision;
    return position;
}

std::expected<PhysicalPosition, PositionFailure> resolvePosition(const Ubiquitty::Terminal& terminal, const LogicalPosition& position, RevisionCheck check)
{
    if (position.buffer != activeBuffer(terminal))
        return std::unexpected(PositionFailure::OtherBuffer);
    return resolveInBuffer(terminal, position, check);
}

std::expected<LogicalPosition, PositionFailure> captureAlternateRow(const Ubiquitty::Terminal& terminal, int row)
{
    if (activeBuffer(terminal) != Buffer::Alternate)
        return std::unexpected(PositionFailure::OtherBuffer);
    const RowSpace space(terminal, Buffer::Alternate);
    if (!terminal.grid || row < 0 || row >= space.total())
        return std::unexpected(PositionFailure::Destroyed);
    // Every move treats a pin as a line beginning at its row, so a pin on a
    // row that continues a line moves exactly when that row does.
    const LinePin* pin = pinLine(terminal, Buffer::Alternate, space.ordinalOf(row));
    if (!pin)
        return std::unexpected(PositionFailure::Destroyed);
    LogicalPosition position;
    position.line = LineID { pin->id };
    position.revision = pin->revision;
    position.buffer = Buffer::Alternate;
    return position;
}

std::expected<int, PositionFailure> resolveAlternateRow(const Ubiquitty::Terminal& terminal, const LogicalPosition& position, int column)
{
    if (activeBuffer(terminal) != Buffer::Alternate)
        return std::unexpected(PositionFailure::OtherBuffer);
    if (position.buffer != Buffer::Alternate)
        return std::unexpected(PositionFailure::Destroyed);
    const LineIdentity* identity = terminal.lineIdentity.get();
    const LinePin* pin = identity && terminal.grid ? findPinNamed(identity->alternate, position.line.value) : nullptr;
    if (!pin || pin->isBandPin())
        return std::unexpected(PositionFailure::Destroyed);
    const RowSpace space(terminal, Buffer::Alternate);
    if (const LinePin* band = bandAt(identity->alternate, *pin, column))
        return pinnedRow(space, *band, position, RevisionCheck::IgnoreErases);
    return pinnedRow(space, *pin, position, RevisionCheck::IgnoreErases);
}

std::expected<LogicalPosition, PositionFailure> historyPosition(const Ubiquitty::Terminal& terminal, std::uint64_t lineNumber, std::uint32_t cell)
{
    // A name is valid only if its continuity can be maintained.
    LineIdentity* identity = ensureIdentity(terminal);
    if (!identity)
        return std::unexpected(PositionFailure::Destroyed);
    LogicalPosition position;
    position.line = LineID { derivedName(identity, lineNumber) };
    position.cell = cell;
    position.buffer = Buffer::Primary;
    noteNameHandedOut(*identity, position.line.value);
    if (identity->primary.isEmpty())
        return position;
    // A carried line a wrap renumbered may answer to this line's name; the
    // line then takes a pin of its own.
    const auto rows = logicalLineRows(terminal, lineNumber);
    if (!rows || !terminal.lineStore)
        return position;
    const std::uint64_t ordinal = terminal.lineStore->frontOrdinal() + static_cast<std::uint64_t>(rows->firstIndex);
    if (!isNameHeldElsewhere(*identity, position.line.value, ordinal))
        return position;
    const LinePin* pin = pinLine(terminal, Buffer::Primary, ordinal);
    if (!pin)
        return std::unexpected(PositionFailure::Destroyed);
    position.line = LineID { pin->id };
    position.revision = pin->revision;
    return position;
}

std::optional<std::uint64_t> lineNumberOf(const Ubiquitty::Terminal& terminal, LineID line)
{
    const LineIdentity* identity = terminal.lineIdentity.get();
    if (const LinePin* pin = identity ? findPinNamed(identity->primary, line.value) : nullptr) {
        if (pin->state == PinState::Destroyed)
            return std::nullopt;
        const RowSpace space(terminal, Buffer::Primary);
        const auto row = space.rowOf(pin->ordinal);
        if (!row || !space.startsLine(*row))
            return std::nullopt;
        return lineNumberAt(terminal, space, *row);
    }
    const auto number = numberForDerivedName(identity, line.value);
    if (!number)
        return std::nullopt;
    return *number;
}

void notePositionsShiftedInPlace(Ubiquitty::Terminal& terminal, int top, int bottom, int rows)
{
    const Buffer buffer = activeBuffer(terminal);
    if (!hasActivePins(terminal) || rows == 0 || bottom < top || !terminal.grid)
        return;
    BufferPins& pins = terminal.lineIdentity->pinsOf(buffer);
    const RowSpace space(terminal, buffer);
    const int openLineFirst = openLineFirstRow(space);
    bool hasCarriedLine = false;
    for (LinePin& pin : pins.pins) {
        if (pin.state == PinState::Destroyed)
            continue;
        const auto extent = screenExtentOf(space, pin, openLineFirst);
        if (!extent)
            continue;
        hasCarriedLine = hasCarriedLine || pin.state == PinState::Carried;
        if (extent->startRow >= top && extent->startRow <= bottom) {
            const int moved = extent->startRow - rows;
            if (moved < top || moved > bottom) {
                pin.state = PinState::Destroyed;
                continue;
            }
            pin.ordinal = space.base() + static_cast<std::uint64_t>(moved);
            if (extent->last > bottom)
                bumpRevision(pins, pin);
        } else if (extent->first <= bottom && extent->last >= top) {
            bumpRevision(pins, pin);
        }
    }
    // The move changes the numbers lines below the band's top will take, so a
    // carried line's history name could go to another line.
    if (buffer == Buffer::Primary && hasCarriedLine)
        startFreshLineNames(terminal);
}

void notePositionsShiftedInBand(Ubiquitty::Terminal& terminal, int top, int bottom, int left, int right, int rows)
{
    const Buffer buffer = activeBuffer(terminal);
    if (!hasActivePins(terminal) || rows == 0 || bottom < top || right < left || !terminal.grid)
        return;
    BufferPins& pins = terminal.lineIdentity->pinsOf(buffer);
    const RowSpace space(terminal, buffer);
    const int openLineFirst = openLineFirstRow(space);
    const auto movedRow = [&](int startRow) -> std::optional<int> {
        const int moved = startRow - rows;
        if (moved < top || moved > bottom)
            return std::nullopt;
        return moved;
    };
    // Band pins made below, added once the loop is done, since adding one may
    // prune and so move the pins the loop walks.
    FallibleVector<LinePin> splits;
    for (LinePin& pin : pins.pins) {
        if (pin.state == PinState::Destroyed)
            continue;
        const auto extent = screenExtentOf(space, pin, openLineFirst);
        if (!extent || extent->last < top || extent->first > bottom)
            continue;
        if (pin.isBandPin()) {
            if (pin.bandRight < left || pin.bandLeft > right)
                continue;
            if (pin.bandLeft < left || pin.bandRight > right) {
                bumpRevision(pins, pin);
                continue;
            }
            if (const auto moved = movedRow(extent->startRow))
                pin.ordinal = space.base() + static_cast<std::uint64_t>(*moved);
            else
                pin.state = PinState::Destroyed;
            continue;
        }
        // Only a line of one row splits by columns: the rows of a longer one
        // move apart, and its cells no longer read as one line.
        const bool isOneRow = extent->startRow >= 0 && extent->first == extent->last && pin.cellOffset == 0
            && space.startsLine(space.history() + extent->startRow);
        if (!isOneRow) {
            bumpRevision(pins, pin);
            continue;
        }
        // Split off these columns already, the row's band holds other cells,
        // which positions on it never name. A band that cuts an earlier one
        // would move apart cells the parent no longer holds the count of.
        bool isSplit = false;
        bool isCut = false;
        for (const LinePin& other : pins.pins) {
            if (other.parentID != pin.id || other.bandRight < left || other.bandLeft > right)
                continue;
            if (other.bandLeft == left && other.bandRight == right)
                isSplit = true;
            else
                isCut = true;
        }
        if (isCut) {
            bumpRevision(pins, pin);
            continue;
        }
        if (isSplit)
            continue;
        LinePin band;
        band.parentID = pin.id;
        band.bandLeft = static_cast<std::int16_t>(left);
        band.bandRight = static_cast<std::int16_t>(right);
        band.bandWritten = static_cast<std::uint16_t>(space.writtenCount(space.history() + extent->startRow, left, right + 1));
        band.revision = pin.revision;
        if (const auto moved = movedRow(extent->startRow)) {
            band.ordinal = space.base() + static_cast<std::uint64_t>(*moved);
        } else {
            // The band's cells on this row left the band: positions on them
            // must fail rather than read what moved in.
            band.ordinal = pin.ordinal;
            band.state = PinState::Destroyed;
        }
        if (!splits.tryPushBack(band))
            bumpRevision(pins, pin);
    }
    for (LinePin& band : splits) {
        band.id = pinnedName(pins);
        if (!addPin(terminal, buffer, pins, band)) {
            if (LinePin* parent = findPinNamed(pins, band.parentID))
                bumpRevision(pins, *parent);
        }
    }
}

void notePositionsRewritten(Ubiquitty::Terminal& terminal, int top, int bottom)
{
    const Buffer buffer = activeBuffer(terminal);
    if (!hasActivePins(terminal) || bottom < top || !terminal.grid)
        return;
    BufferPins& pins = terminal.lineIdentity->pinsOf(buffer);
    const RowSpace space(terminal, buffer);
    const int openLineFirst = openLineFirstRow(space);
    for (LinePin& pin : pins.pins) {
        if (pin.state == PinState::Destroyed)
            continue;
        const auto extent = screenExtentOf(space, pin, openLineFirst);
        if (extent && extent->first <= bottom && extent->last >= top)
            bumpRevision(pins, pin);
    }
}

void notePositionsScreenReplaced(Ubiquitty::Terminal& terminal, Buffer buffer)
{
    if (!terminal.lineIdentity || !terminal.grid)
        return;
    BufferPins& pins = terminal.lineIdentity->pinsOf(buffer);
    if (pins.isEmpty())
        return;
    const RowSpace space(terminal, buffer);
    const int openLineFirst = openLineFirstRow(space);
    for (LinePin& pin : pins.pins) {
        if (pin.state != PinState::Destroyed && screenExtentOf(space, pin, openLineFirst))
            bumpRevision(pins, pin);
    }
}

void notePositionsHistoryTailDeleted(Ubiquitty::Terminal& terminal, std::uint64_t previousNextOrdinal)
{
    if (!terminal.lineIdentity)
        return;
    BufferPins& pins = terminal.lineIdentity->primary;
    const std::uint64_t base = terminal.lineStore ? terminal.lineStore->nextOrdinal() : 0;
    if (previousNextOrdinal > base) {
        const std::uint64_t deleted = previousNextOrdinal - base;
        for (LinePin& pin : pins.pins) {
            if (pin.state == PinState::Destroyed)
                continue;
            if (pin.ordinal >= previousNextOrdinal)
                pin.ordinal -= deleted;
            else if (pin.ordinal >= base)
                pin.state = PinState::Destroyed;
        }
    }
    // The deleted lines' numbers go to lines that are still on the screen.
    startFreshLineNames(terminal);
}

void notePositionsPulledFromHistory(Ubiquitty::Terminal& terminal, std::uint64_t previousNextOrdinal)
{
    if (!terminal.lineIdentity || !terminal.grid || !terminal.lineStore)
        return;
    const RowSpace space(terminal, Buffer::Primary);
    if (space.base() >= previousNextOrdinal)
        return;
    const int openLineFirst = openLineFirstRow(space);
    int row = openLineFirst >= 0 ? openLineFirst : space.history();
    std::uint64_t number = headNumber(terminal);
    bool hasCarried = false;
    while (row < space.total() && space.ordinalOf(row) < previousNextOrdinal) {
        carryLine(terminal, number, space.ordinalOf(row));
        hasCarried = true;
        row = space.lineEnd(row) + 1;
        ++number;
    }
    if (hasCarried)
        startFreshLineNames(terminal);
}

void notePositionsBlankRowsInsertedAtSeam(Ubiquitty::Terminal& terminal, int pulledRows, int insertedRows)
{
    if (!terminal.lineIdentity || !terminal.grid || insertedRows <= 0)
        return;
    const RowSpace space(terminal, Buffer::Primary);
    const std::uint64_t pulledFrom = space.base() - static_cast<std::uint64_t>(std::clamp(pulledRows, 0, space.history()));
    // The lines whose first rows the grow moves keep their history names on a
    // pin before the rows leave the store, at the ordinals they will have.
    if (pulledFrom < space.base()) {
        for (int row = static_cast<int>(pulledFrom - space.front()); row < space.history(); row = space.lineEnd(row) + 1) {
            if (!space.startsLine(row))
                continue;
            if (const auto number = lineNumberAt(terminal, space, row))
                carryLine(terminal, *number, space.ordinalOf(row));
        }
    }
    for (LinePin& pin : terminal.lineIdentity->primary.pins) {
        if (pin.state != PinState::Destroyed && pin.ordinal >= pulledFrom)
            pin.ordinal += static_cast<std::uint64_t>(insertedRows);
    }
}

void notePositionsRenumbered(Ubiquitty::Terminal& terminal, std::uint64_t newBase)
{
    LineIdentity* identity = terminal.lineIdentity.get();
    if (!identity || !terminal.lineStore || !ensureSegments(*identity))
        return;
    const Ubiquitty::LineStore::FrontLine front = terminal.lineStore->frontLine();
    const std::uint64_t oldFront = front.number;
    if (newBase < oldFront)
        return;
    // A clear that cut the front line kept its last rows, which the
    // renumbering makes a line of their own: the cells its positions count
    // from are gone, so its names go too.
    const bool isFrontCut = front.continuity == Ubiquitty::LineStore::LineContinuity::ContinuesLine;
    const std::uint64_t delta = newBase - oldFront;
    std::size_t holding = 0;
    while (holding + 1 < identity->segments.size() && identity->segments[holding + 1].firstNumber <= oldFront)
        ++holding;
    if (holding)
        identity->segments.erase(identity->segments.begin(), identity->segments.begin() + static_cast<std::ptrdiff_t>(holding));
    identity->segments[0].firstNumber = oldFront;
    for (NameSegment& segment : identity->segments) {
        segment.firstNumber += delta;
        segment.bias -= static_cast<std::int64_t>(delta);
    }
    identity->minNumber = std::max(identity->minNumber + delta, newBase + (isFrontCut ? 1 : 0));
}

void startFreshLineNames(Ubiquitty::Terminal& terminal)
{
    LineIdentity* identity = terminal.lineIdentity.get();
    if (!identity || !ensureSegments(*identity))
        return;
    const std::uint64_t head = headNumber(terminal);
    // The open line's stored rows are named under the segment that ends here;
    // a pin keeps that name on them.
    if (terminal.lineStore && terminal.grid) {
        const RowSpace space(terminal, Buffer::Primary);
        const int openLineFirst = openLineFirstRow(space);
        if (openLineFirst >= 0 && space.startsLine(openLineFirst))
            carryLine(terminal, head, space.ordinalOf(openLineFirst));
    }
    const std::uint64_t fresh = freshNameFloor(terminal, *identity);
    while (identity->segments.size() > 1 && identity->segments[identity->segments.size() - 1].firstNumber >= head)
        identity->segments.pop_back();
    const NameSegment segment { .firstNumber = head, .bias = static_cast<std::int64_t>(fresh) - static_cast<std::int64_t>(head) };
    if (identity->segments[0].firstNumber >= head) {
        identity->segments[0] = segment;
        return;
    }
    pruneSegments(terminal, *identity);
    if (!identity->segments.tryPushBack(segment)) {
        // Without the segment a later line could take a name already held, so
        // every line from `head` on gives its names up instead.
        identity->minNumber = std::max(identity->minNumber, head);
    }
}

void forgetPositions(Ubiquitty::Terminal& terminal, Buffer buffer)
{
    if (!terminal.lineIdentity)
        return;
    for (LinePin& pin : terminal.lineIdentity->pinsOf(buffer).pins)
        pin.state = PinState::Destroyed;
}

void notePositionsResized(Ubiquitty::Terminal& terminal)
{
    if (!terminal.lineIdentity)
        return;
    for (const Buffer buffer : { Buffer::Primary, Buffer::Alternate }) {
        BufferPins& pins = terminal.lineIdentity->pinsOf(buffer);
        if (pins.isEmpty())
            continue;
        const RowSpace space(terminal, buffer);
        for (LinePin& pin : pins.pins) {
            if (pin.state != PinState::Destroyed && pin.ordinal >= space.front() && !space.rowOf(pin.ordinal))
                pin.state = PinState::Destroyed;
        }
    }
}

ScreenPinsAcrossHistoryRebuild::ScreenPinsAcrossHistoryRebuild(Ubiquitty::Terminal& terminal)
    : m_terminal(terminal)
    , m_nextOrdinal(terminal.lineStore ? terminal.lineStore->nextOrdinal() : 0)
    , m_appended(terminal.lineStore ? terminal.lineStore->totalAppended() : 0)
    , m_count(terminal.lineStore ? terminal.lineStore->count() : 0)
{
}

ScreenPinsAcrossHistoryRebuild::~ScreenPinsAcrossHistoryRebuild()
{
    if (!m_terminal.lineIdentity)
        return;
    const Ubiquitty::LineStore* store = m_terminal.lineStore.get();
    const std::uint64_t nextOrdinal = store ? store->nextOrdinal() : 0;
    const bool isUnchanged = nextOrdinal == m_nextOrdinal && (store ? store->totalAppended() : 0) == m_appended && (store ? store->count() : 0) == m_count;
    if (isUnchanged)
        return;
    for (LinePin& pin : m_terminal.lineIdentity->primary.pins) {
        if (pin.state == PinState::Destroyed)
            continue;
        if (pin.ordinal >= m_nextOrdinal)
            pin.ordinal = pin.ordinal - m_nextOrdinal + nextOrdinal;
        else
            pin.state = PinState::Destroyed;
    }
}

void beginPositionReflow(Ubiquitty::Terminal& terminal, PositionReflow& reflow)
{
    reflow.places.clear();
    reflow.headBefore = headNumber(terminal);
    reflow.nextOrdinalBefore = terminal.lineStore ? terminal.lineStore->nextOrdinal() : 0;
    reflow.hadOpenLine = terminal.lineStore && terminal.lineStore->openLine().has_value();
    reflow.isActive = terminal.lineIdentity != nullptr;
    if (!terminal.lineIdentity || !terminal.grid)
        return;
    BufferPins& pins = terminal.lineIdentity->primary;
    if (pins.isEmpty())
        return;
    if (!reflow.places.tryReserve(pins.pins.size())) {
        forgetPositions(terminal, Buffer::Primary);
        return;
    }
    const RowSpace space(terminal, Buffer::Primary);
    // A split row's band columns mean nothing at another width: its cells
    // there are another line's, so the line is erased and its bands go.
    for (LinePin& pin : pins.pins) {
        if (pin.state != PinState::Destroyed && !pin.isBandPin() && hasBandChildren(pins, pin))
            bumpRevision(pins, pin);
    }
    for (LinePin& pin : pins.pins) {
        if (pin.isBandPin()) {
            pin.parentID = 0;
            pin.state = PinState::Destroyed;
        }
    }
    for (LinePin& pin : pins.pins) {
        PositionReflow::Place place;
        bool isPlaced = false;
        // A band pin stands for columns, which a rewrap gives no meaning to.
        if (pin.state != PinState::Destroyed && !pin.isBandPin()) {
            if (const auto row = space.rowOf(pin.ordinal)) {
                // A row a wrap has joined to the line above is placed by its
                // offset into that line, which the rewrap keeps.
                const int first = space.lineStart(*row);
                std::uint64_t offset = pin.cellOffset;
                for (int prior = first; prior < *row; ++prior)
                    offset += static_cast<std::uint64_t>(space.writtenCount(prior));
                if (const auto number = space.startsLine(first) ? lineNumberAt(terminal, space, first) : std::nullopt; number && offset <= std::numeric_limits<std::uint32_t>::max()) {
                    place = { .number = *number, .cellOffset = static_cast<std::uint32_t>(offset) };
                    isPlaced = true;
                }
            }
        }
        if (!isPlaced)
            pin.state = PinState::Destroyed;
        (void)reflow.places.tryPushBack(place);
    }
}

void finishPositionReflow(Ubiquitty::Terminal& terminal, PositionReflow& reflow)
{
    if (!reflow.isActive || !terminal.grid || !terminal.lineIdentity)
        return;
    reflow.isActive = false;
    BufferPins& pins = terminal.lineIdentity->primary;
    const RowSpace space(terminal, Buffer::Primary);
    for (std::size_t index = 0; index < pins.pins.size() && index < reflow.places.size(); ++index) {
        LinePin& pin = pins.pins[index];
        if (pin.state == PinState::Destroyed)
            continue;
        const auto first = firstRowOfLine(terminal, space, reflow.places[index].number);
        if (!first) {
            pin.state = PinState::Destroyed;
            continue;
        }
        pin.ordinal = space.ordinalOf(*first);
        pin.cellOffset = reflow.places[index].cellOffset;
    }
    // A rewrap that widens moves the newest history lines onto the screen,
    // where they can be edited; a pin keeps each one's history name.
    const std::uint64_t head = headNumber(terminal);
    const std::uint64_t carriedEnd = reflow.headBefore + (reflow.hadOpenLine ? 1 : 0);
    const int openLineFirst = openLineFirstRow(space);
    bool hasCarried = false;
    for (std::uint64_t number = head; number < carriedEnd; ++number) {
        const auto first = firstRowOfLine(terminal, space, number);
        if (first && (*first >= space.history() || *first == openLineFirst)) {
            carryLine(terminal, number, space.ordinalOf(*first));
            hasCarried = true;
        }
    }
    if (hasCarried)
        startFreshLineNames(terminal);
    reflow.places.clear();
}

LineIdentityContinuation lineIdentityContinuation(const Ubiquitty::Terminal& terminal)
{
    const LineIdentity* identity = terminal.lineIdentity.get();
    if (!identity)
        return {};
    return {
        .hasNames = true,
        .freshName = freshNameFloor(terminal, *identity),
        .primarySerial = identity->primary.nextSerial,
        .alternateSerial = identity->alternate.nextSerial,
    };
}

void restoreLineIdentity(Ubiquitty::Terminal& terminal, const LineIdentityContinuation& continuation)
{
    if (!continuation.hasNames)
        return;
    LineIdentity* identity = ensureIdentity(terminal);
    if (!identity || !ensureSegments(*identity))
        return;
    identity->primary.nextSerial = continuation.primarySerial;
    identity->alternate.nextSerial = continuation.alternateSerial;
    const std::uint64_t head = headNumber(terminal);
    identity->segments[0] = NameSegment { .firstNumber = head, .bias = static_cast<std::int64_t>(continuation.freshName) - static_cast<std::int64_t>(head) };
    identity->minNumber = head;
    identity->minFreshName = continuation.freshName;
}

} // namespace Ubiquitty::Screen
