// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Screen/RowForm.h — the codec between a row of cells and its row form.
//
// The form is the one Sync carries: the row as UTF-8 text plus run-length
// pens, links and cluster references over units. Its grammar and rules are
// stated with the row record in `Core/Protocol/Sync/RowForm.h` and specified
// in Docs/sync.md §7.4. The Sync planes write it with `writeRowForm` and
// read it with `readRowForm`, and so does the scrollback store when it folds
// a cold block; how the form *names* what a cell refers to — a pen, a link, a
// long cluster — is the one thing the two differ in, so the names are a
// parameter and the walk over the cells is written once. A pen is an id in
// the reader's pen table; a link is whatever `Names::link` said for the
// cell's link id, 0 for none; a cluster reference is what `Names::cluster`
// answered when it was not zero.
//
// Every cell is reproduced byte for byte: a decoder fills exactly the cells
// the encoder read. The encoder refuses a row it cannot say exactly, which is
// a cell whose code point is not a Unicode scalar value.
//
// The names a writer supplies:
//
//     struct Names {
//         std::uint64_t pen(const Pen&);                 // the value `runs` carries
//         std::uint64_t link(std::uint16_t linkID);      // the value `links` carries, 0 for none
//         std::optional<std::uint64_t> cluster(const Cell& lead);
//                                     // the value `clusterRefs` carries, 0 for no
//                                     // entry, nullopt when the row cannot be said
//     };
//

#include "Core/Cell.h"
#include "Core/Pen.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Screen {

using Ubiquitty::Protocol::Sync::WireReader;
using Ubiquitty::Protocol::Sync::WireWriter;

/// A wire count's ceiling in this platform's sizes: a bound past what a
/// `std::size_t` holds is no tighter than the largest one, which a 32-bit
/// target has where a 64-bit one has the bound itself.
///
/// - Parameter ceiling: The structural ceiling, in the wire's 64-bit units.
/// - Returns: The same ceiling, saturated to `std::size_t`.
[[nodiscard]] constexpr std::size_t countCeiling(std::uint64_t ceiling)
{
    return static_cast<std::size_t>(std::min<std::uint64_t>(ceiling, std::numeric_limits<std::size_t>::max()));
}

/// The largest link id a cell carries: sixteen bits.
inline constexpr std::uint64_t maxLinkID = 0xFFFF;

/// The largest cluster id a cell carries: fifteen bits, the sixteenth holding
/// the inline combining count.
inline constexpr std::uint64_t maxClusterID = (1u << (16 - cellCombiningLengthBits)) - 1;

/// The most code points one cell holds inline: its base and its marks.
inline constexpr std::uint64_t maxUnitCodePoints = 1 + cellMaxCombining;

/// Whether two cells are the same bytes.
[[nodiscard]] inline bool isSameCell(const Cell& a, const Cell& b)
{
    return std::memcmp(&a, &b, sizeof(Cell)) == 0;
}

/// The continuation cell the Core writes beside a wide lead.
///
/// - Parameter lead: A cell whose pen carries `Wide`.
/// - Returns: The lead's pen without `Wide` and `Emoji`, with
///   `WideContinuation`; code point 0, no combining, no cluster, the lead's link.
[[nodiscard]] inline Cell derivedContinuation(const Cell& lead)
{
    using Ubiquitty::VT::Attribute;
    Cell continuation = lead;
    continuation.codePoint = wideContinuationSentinelCodePoint;
    continuation.combining = {};
    continuation.clusterPacked = 0;
    continuation.pen.attributes.remove({ Attribute::Wide, Attribute::Emoji });
    continuation.pen.attributes.add(Attribute::WideContinuation);
    return continuation;
}

/// A run-length list over units, appended to as units are walked.
class RunList {
public:
    /// Extends the last run when it holds `value`, and starts one otherwise.
    ///
    /// - Parameter value: The next unit's value.
    void add(std::uint64_t value)
    {
        if (!m_runs.empty() && m_runs.back().second == value) {
            ++m_runs.back().first;
            return;
        }
        m_runs.emplace_back(1, value);
    }

    /// Whether every unit holds `value`, as an empty list does.
    ///
    /// - Parameter value: The value.
    /// - Returns: Whether no run holds another.
    [[nodiscard]] bool isAll(std::uint64_t value) const { return m_runs.empty() || (m_runs.size() == 1 && m_runs.front().second == value); }

    /// Writes the list: its count, then (units, value) per run.
    ///
    /// - Parameter writer: The body being written.
    void write(WireWriter& writer) const
    {
        writer.varint(m_runs.size());
        for (const auto& [units, value] : m_runs) {
            writer.varint(units);
            writer.varint(value);
        }
    }

private:
    std::vector<std::pair<std::uint64_t, std::uint64_t>> m_runs;
};

/// Whether a gap-coded list carries a value with each unit.
enum class EntryValue : std::uint8_t {
    Absent,
    Present,
};

/// A list of (unit, value) entries written gap-coded.
class GapList {
public:
    /// Lists a unit with a value; units are added in ascending order.
    ///
    /// - Parameters:
    ///   - unit: The unit.
    ///   - value: Its value.
    void add(std::uint64_t unit, std::uint64_t value) { m_entries.emplace_back(unit, value); }
    /// Lists a unit that carries no value.
    ///
    /// - Parameter unit: The unit.
    void add(std::uint64_t unit) { m_entries.emplace_back(unit, 0); }

    /// Writes the list: its count, then each unit's gap from the one before
    /// and, when `entryValue` says so, its value.
    ///
    /// - Parameters:
    ///   - writer: The body being written.
    ///   - entryValue: Whether each entry's value is written.
    void write(WireWriter& writer, EntryValue entryValue) const
    {
        writer.varint(m_entries.size());
        std::uint64_t previous = 0;
        for (const auto& [unit, value] : m_entries) {
            writer.varint(unit - previous);
            previous = unit;
            if (entryValue == EntryValue::Present)
                writer.varint(value);
        }
    }

private:
    std::vector<std::pair<std::uint64_t, std::uint64_t>> m_entries;
};

/// Writes a row's form: its extent, text, runs, links, clusters, cluster
/// references and kept wide leads.
///
/// - Parameters:
///   - cells: The row's cells; trailing canonical blanks are not written.
///   - names: How the form names pens, links and clusters; see the file comment.
///   - writer: The form is appended here.
/// - Returns: False, with nothing written, when the row cannot be said
///   exactly: a code point that is not a scalar value, or a cluster `names`
///   refuses.
template<typename Names>
[[nodiscard]] bool writeRowForm(std::span<const Cell> cells, Names& names, WireWriter& writer)
{
    using Ubiquitty::Protocol::Sync::appendUTF8;
    using Ubiquitty::Protocol::Sync::isScalarValue;
    using Ubiquitty::VT::Attribute;
    std::size_t extent = cells.size();
    while (extent > 0 && isSameCell(cells[extent - 1], defaultBlankCell))
        --extent;

    std::vector<std::uint8_t> text;
    text.reserve(extent);
    RunList runs;
    RunList links;
    GapList clusters;
    GapList clusterRefs;
    GapList wideKept;
    std::uint64_t unit = 0;
    std::uint64_t previousPenValue = 0;
    const Pen* previousPen = nullptr;
    for (std::size_t column = 0; column < extent; ++unit) {
        const Cell& lead = cells[column];
        std::size_t span = 1;
        if (lead.pen.attributes.contains(Attribute::Wide)) {
            if (column + 1 < extent && isSameCell(cells[column + 1], derivedContinuation(lead)))
                span = 2;
            else
                wideKept.add(unit);
        }

        if (!isScalarValue(lead.codePoint))
            return false;
        appendUTF8(text, lead.codePoint);
        const int combining = lead.combiningLength();
        for (int index = 0; index < combining; ++index) {
            const std::uint32_t mark = lead.combining[static_cast<std::size_t>(index)];
            if (!isScalarValue(mark))
                return false;
            appendUTF8(text, mark);
        }
        if (combining != 0)
            clusters.add(unit, static_cast<std::uint64_t>(1 + combining));

        if (lead.clusterID() != 0) {
            const std::optional<std::uint64_t> cluster = names.cluster(lead);
            if (!cluster)
                return false;
            if (*cluster != 0)
                clusterRefs.add(unit, *cluster);
        }

        if (!previousPen || std::memcmp(previousPen, &lead.pen, sizeof(Pen)) != 0) {
            previousPenValue = names.pen(lead.pen);
            previousPen = &lead.pen;
        }
        runs.add(previousPenValue);

        const std::uint16_t link = lead.linkID();
        links.add(link != 0 ? names.link(link) : 0);

        column += span;
    }

    writer.varint(extent);
    writer.bytes(text);
    runs.write(writer);
    if (links.isAll(0))
        writer.varint(0);
    else
        links.write(writer);
    clusters.write(writer, EntryValue::Present);
    clusterRefs.write(writer, EntryValue::Present);
    wideKept.write(writer, EntryValue::Absent);
    return true;
}

/// A decoded gap list, consumed in unit order.
class GapCursor {
public:
    /// Reads the list, checking that its units ascend and stay below `units`
    /// and its values stay at or below `maxValue`.
    ///
    /// - Parameters:
    ///   - reader: Positioned at the list.
    ///   - units: How many units the row has; every entry's unit is below it.
    ///   - entryValue: Whether each entry carries a value.
    ///   - maxValue: The largest value an entry may carry.
    /// - Returns: Whether the reader is still well.
    bool read(WireReader& reader, std::uint64_t units, EntryValue entryValue, std::uint64_t maxValue)
    {
        const std::size_t count = reader.count(countCeiling(units), entryValue == EntryValue::Present ? 2 : 1);
        m_entries.reserve(count);
        std::uint64_t unit = 0;
        for (std::size_t index = 0; index < count && reader.ok(); ++index) {
            const std::uint64_t gap = reader.varint(units);
            // Entries are strictly ascending, so only the first may sit at a gap of zero.
            if (index > 0 && gap == 0)
                reader.fail();
            unit += gap;
            if (unit >= units)
                reader.fail();
            const std::uint64_t value = entryValue == EntryValue::Present ? reader.varint(maxValue) : 0;
            m_entries.emplace_back(unit, value);
        }
        return reader.ok();
    }

    /// Takes the entry listed for a unit, when the next entry is its.
    ///
    /// - Parameter unit: The unit being decoded; units are asked in order.
    /// - Returns: The entry's value, or `nullopt` when no entry names the unit.
    std::optional<std::uint64_t> take(std::uint64_t unit)
    {
        if (m_next < m_entries.size() && m_entries[m_next].first == unit)
            return m_entries[m_next++].second;
        return std::nullopt;
    }

private:
    std::vector<std::pair<std::uint64_t, std::uint64_t>> m_entries;
    std::size_t m_next = 0;
};

/// A decoded run list, consumed one unit at a time.
class RunCursor {
public:
    /// Reads the runs, checking that they cover at most `maxUnits` units and
    /// carry values at or below `maxValue`.
    ///
    /// - Parameters:
    ///   - reader: Positioned at the list.
    ///   - maxUnits: The most units the runs may cover between them.
    ///   - maxValue: The largest value a run may carry.
    /// - Returns: How many units the runs cover, or 0 once the reader has failed.
    std::uint64_t read(WireReader& reader, std::uint64_t maxUnits, std::uint64_t maxValue)
    {
        const std::size_t count = reader.count(countCeiling(maxUnits), 2);
        std::uint64_t covered = 0;
        for (std::size_t index = 0; index < count && reader.ok(); ++index) {
            const std::uint64_t length = reader.varint(maxUnits);
            const std::uint64_t value = reader.varint(maxValue);
            covered += length;
            if (length == 0 || covered > maxUnits)
                reader.fail();
            m_runs.emplace_back(length, value);
        }
        return reader.ok() ? covered : 0;
    }

    /// Takes the next unit's value.
    ///
    /// - Returns: The value of the run the next unit falls in, or 0 past the
    ///   last run.
    std::uint64_t next()
    {
        if (m_index >= m_runs.size())
            return 0;
        const std::uint64_t value = m_runs[m_index].second;
        if (++m_used == m_runs[m_index].first) {
            ++m_index;
            m_used = 0;
        }
        return value;
    }

private:
    std::vector<std::pair<std::uint64_t, std::uint64_t>> m_runs;
    std::size_t m_index = 0;
    std::uint64_t m_used = 0;
};

/// Reads one row form into a row of `cells.size()` cells.
///
/// Reads the whole form and refuses it, leaving `cells` in an unspecified
/// state, when any count, length, id or sum is out of bounds: the extent past
/// the width, text that does not split into the units the runs cover, a pen id
/// past `pens`, or a cluster id `resolves` refuses.
///
/// - Parameters:
///   - reader: Positioned at the form.
///   - pens: The pens the form's ids index.
///   - resolves: Whether a non-zero cluster id names something the reader
///     holds, as `bool(std::uint64_t id)`.
///   - cells: The row to fill, one cell per column.
/// - Returns: Whether the form was well formed; the reader is failed otherwise.
template<typename Pens, typename Resolves>
[[nodiscard]] bool readRowForm(WireReader& reader, const Pens& pens, Resolves&& resolves, std::span<Cell> cells)
{
    using Ubiquitty::Protocol::Sync::nextScalarValue;
    using Ubiquitty::VT::Attribute;
    const std::uint64_t extent = reader.varint(cells.size());
    // Every cell before the extent is at least one byte of text.
    const std::string_view text = reader.utf8(cells.size() * 4 * maxUnitCodePoints);
    if (!reader.ok())
        return false;

    // The units are the text's code points, less the marks `clusters` folds
    // into a unit; the text has at least as many code points as units.
    std::uint64_t codePoints = 0;
    for (std::size_t offset = 0; offset < text.size(); ++codePoints)
        (void)nextScalarValue(text, offset);
    if (codePoints > extent * maxUnitCodePoints) {
        reader.fail();
        return false;
    }

    // The unit count is not on the wire: the runs cover every unit, so it is
    // what they sum to, and every later list is checked against it.
    RunCursor runs;
    const std::uint64_t units = runs.read(reader, extent, pens.empty() ? 0 : pens.size() - 1);
    if (!reader.ok() || units > codePoints || (units > 0 && pens.empty())) {
        reader.fail();
        return false;
    }
    RunCursor links;
    const std::uint64_t linkedUnits = links.read(reader, units, maxLinkID);
    GapCursor clusters;
    GapCursor clusterRefs;
    GapCursor wideKept;
    if (!reader.ok() || (linkedUnits != units && linkedUnits != 0) || !clusters.read(reader, units, EntryValue::Present, maxUnitCodePoints)
        || !clusterRefs.read(reader, units, EntryValue::Present, maxClusterID) || !wideKept.read(reader, units, EntryValue::Absent, 0)) {
        reader.fail();
        return false;
    }

    std::size_t offset = 0;
    std::size_t column = 0;
    for (std::uint64_t unit = 0; unit < units; ++unit) {
        if (column >= extent || offset >= text.size()) {
            reader.fail();
            return false;
        }
        Cell cell = defaultBlankCell;
        const std::optional<std::uint64_t> listedCodePoints = clusters.take(unit);
        const std::uint64_t unitCodePoints = listedCodePoints.value_or(1);
        if (listedCodePoints && unitCodePoints < 2) {
            reader.fail();
            return false;
        }
        cell.codePoint = nextScalarValue(text, offset);
        for (std::uint64_t index = 1; index < unitCodePoints; ++index) {
            if (offset >= text.size()) {
                reader.fail();
                return false;
            }
            cell.combining[static_cast<std::size_t>(index - 1)] = nextScalarValue(text, offset);
        }
        cell.setCombiningLength(static_cast<std::uint8_t>(unitCodePoints - 1));

        const std::uint64_t penID = runs.next();
        if (penID >= pens.size()) {
            reader.fail();
            return false;
        }
        cell.pen = pens[static_cast<std::size_t>(penID)];

        cell.setLinkID(static_cast<std::uint16_t>(links.next()));

        if (const std::optional<std::uint64_t> cluster = clusterRefs.take(unit)) {
            if (*cluster == 0 || !resolves(*cluster)) {
                reader.fail();
                return false;
            }
            cell.setClusterID(static_cast<std::uint16_t>(*cluster));
        }

        cells[column++] = cell;
        const bool isKept = wideKept.take(unit).has_value();
        if (isKept && !cell.pen.attributes.contains(Attribute::Wide)) {
            reader.fail();
            return false;
        }
        if (cell.pen.attributes.contains(Attribute::Wide) && !isKept) {
            if (column >= extent) {
                reader.fail();
                return false;
            }
            cells[column++] = derivedContinuation(cell);
        }
    }
    if (column != extent || offset != text.size()) {
        reader.fail();
        return false;
    }
    for (std::size_t index = column; index < cells.size(); ++index)
        cells[index] = defaultBlankCell;
    return true;
}

} // namespace Ubiquitty::Screen
