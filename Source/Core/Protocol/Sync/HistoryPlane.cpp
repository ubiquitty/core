// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/HistoryPlane.h"

#include "Core/CellContent.h"
#include "Core/ClusterPool.h"
#include "Core/Color.h"
#include "Core/LineStore.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/RunTextStore.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

// One link as a table entry: the host's id for it, the key it was interned
// under and its target.
struct LinkEntry {
    std::uint16_t hostID = 0;
    std::string key;
    std::string uri;
};

// One long cluster as a table entry: the host's pool id for it and its code
// points.
struct ClusterEntry {
    std::uint16_t hostID = 0;
    std::span<const std::uint32_t> codePoints;
};

struct HistoryDecodeBudget {
    std::size_t remaining;

    bool take(std::size_t count, std::size_t bytes = 1)
    {
        if (bytes && count > remaining / bytes)
            return false;
        remaining -= count * bytes;
        return true;
    }
};

// Retained line content accumulates across the body. The row-form lists,
// scratch cells and canonical-hash tables coexist only for the current line.
struct HistoryLineDecodeCost {
    std::size_t retained = 2 * sizeof(DecodedLine);
    std::size_t temporary = 64;

    static bool add(std::size_t& value, std::size_t count, std::size_t bytes = 1)
    {
        HistoryDecodeBudget room { std::numeric_limits<std::size_t>::max() - value };
        if (!room.take(count, bytes))
            return false;
        value = std::numeric_limits<std::size_t>::max() - room.remaining;
        return true;
    }

    bool cells(std::size_t count)
    {
        // Both row walks build run and gap lists; allow their geometric
        // growth and the canonical byte vector beside the decoded scratch.
        constexpr std::size_t workingBytes = sizeof(Cell) + 24 * sizeof(std::pair<std::uint64_t, std::uint64_t>) + 64;
        return add(retained, count, sizeof(Cell)) && add(temporary, count, workingBytes);
    }

    bool link(std::size_t keyBytes, std::size_t uriBytes)
    {
        // String constructors may align heap capacities above length + 1.
        return add(retained, 1, sizeof(LinkDefinition) + 4) && add(retained, keyBytes, 2) && add(retained, uriBytes, 2)
            && add(temporary, 4, sizeof(LinkEntry) + sizeof(std::uint16_t))
            && add(temporary, keyBytes, 8) && add(temporary, uriBytes, 8) && add(temporary, 16);
    }

    bool cluster(std::size_t count)
    {
        return add(retained, 1, sizeof(std::vector<std::uint32_t>) + sizeof(std::uint16_t))
            && add(retained, count, sizeof(std::uint32_t))
            && add(temporary, 4, sizeof(std::vector<std::uint32_t>) + sizeof(std::uint16_t))
            && add(temporary, count, sizeof(std::uint32_t) + 10);
    }

    [[nodiscard]] bool fits(const HistoryDecodeBudget& budget) const
    {
        return retained <= budget.remaining && temporary <= budget.remaining - retained;
    }
};

// Whether a table writes the host's ids beside its entries: a body does, so
// the client can resolve them through the tables it mirrors; the canonical
// form a hash covers does not, since ids are not what a line shows.
enum class TableIDs : std::uint8_t {
    Omitted,
    Written,
};

// What a host cell names, read from the host terminal.
class HostResolver {
public:
    explicit HostResolver(const Ubiquitty::Terminal& terminal)
        : m_terminal(terminal)
    {
    }

    [[nodiscard]] std::optional<LinkEntry> link(std::uint16_t id) const
    {
        std::optional<LinkDefinition> definition = linkDefinition(m_terminal, id);
        if (!definition)
            return std::nullopt;
        return LinkEntry { .hostID = id, .key = std::move(definition->key), .uri = std::move(definition->uri) };
    }

    // A proportional run's text lives in a store the history plane does not
    // carry; its anchor travels as the cell it is, naming no cluster.
    [[nodiscard]] ClusterEntry cluster(const Cell& lead) const
    {
        if (Ubiquitty::isRunTextAnchor(&m_terminal, lead))
            return {};
        return { .hostID = lead.clusterID(), .codePoints = m_terminal.clusterPool.sequence(lead.clusterID()) };
    }

private:
    const Ubiquitty::Terminal& m_terminal;
};

// What a decoded cell names, read from the tables of the body it came in.
class BodyResolver {
public:
    BodyResolver(std::span<const LinkEntry> links, std::span<const std::vector<std::uint32_t>> clusters, std::span<const std::uint16_t> clusterHostIDs)
        : m_links(links)
        , m_clusters(clusters)
        , m_clusterHostIDs(clusterHostIDs)
    {
    }

    [[nodiscard]] std::optional<LinkEntry> link(std::uint16_t id) const
    {
        if (id == 0 || id > m_links.size())
            return std::nullopt;
        return m_links[id - 1];
    }

    [[nodiscard]] ClusterEntry cluster(const Cell& lead) const
    {
        const std::uint16_t id = lead.clusterID();
        if (id == 0 || id > m_clusters.size())
            return {};
        return { .hostID = m_clusterHostIDs[id - 1], .codePoints = m_clusters[id - 1] };
    }

private:
    std::span<const LinkEntry> m_links;
    std::span<const std::vector<std::uint32_t>> m_clusters;
    std::span<const std::uint16_t> m_clusterHostIDs;
};

// Names pens, links and clusters by tables built in the order they are first
// named. One instance across a body's lines builds the body's tables; a fresh
// one per line builds the canonical tables its hash covers.
template<typename Resolver>
class TableNames {
public:
    explicit TableNames(const Resolver& resolver)
        : m_resolver(resolver)
    {
    }

    std::uint64_t pen(const Pen& pen)
    {
        for (std::size_t index = 0; index < m_pens.size(); ++index) {
            if (std::memcmp(&m_pens[index], &pen, sizeof(Pen)) == 0)
                return index;
        }
        m_pens.push_back(pen);
        m_tableBytes += penWireBytes;
        countDecodedTableBytes(1, sizeof(Pen));
        return m_pens.size() - 1;
    }

    std::uint64_t link(std::uint16_t id)
    {
        if (const auto found = std::ranges::find(m_linkIDs, id); found != m_linkIDs.end())
            return static_cast<std::uint64_t>(found - m_linkIDs.begin()) + 1;
        std::optional<LinkEntry> entry = m_resolver.link(id);
        if (!entry)
            return 0;
        m_tableBytes += 12 + entry->key.size() + entry->uri.size();
        countDecodedTableBytes(1, sizeof(LinkEntry) + 4);
        countDecodedTableBytes(entry->key.size(), 2);
        countDecodedTableBytes(entry->uri.size(), 2);
        m_linkIDs.push_back(id);
        m_links.push_back(std::move(*entry));
        return m_links.size();
    }

    std::optional<std::uint64_t> cluster(const Cell& lead)
    {
        const std::uint16_t id = lead.clusterID();
        if (const auto found = std::ranges::find(m_clusterIDs, id); found != m_clusterIDs.end())
            return static_cast<std::uint64_t>(found - m_clusterIDs.begin()) + 1;
        const ClusterEntry entry = m_resolver.cluster(lead);
        if (entry.codePoints.empty())
            return 0;
        m_tableBytes += 7 + 5 * entry.codePoints.size();
        countDecodedTableBytes(1, sizeof(std::vector<std::uint32_t>) + sizeof(std::uint16_t) + 1);
        countDecodedTableBytes(entry.codePoints.size(), sizeof(std::uint32_t));
        m_clusterIDs.push_back(id);
        m_clusterHostIDs.push_back(entry.hostID);
        m_clusters.emplace_back(entry.codePoints.begin(), entry.codePoints.end());
        return m_clusters.size();
    }

    // A bound on the bytes the tables take written.
    [[nodiscard]] std::size_t tableBytes() const { return m_tableBytes; }

    [[nodiscard]] bool isWithinDecodeLimits() const
    {
        return m_pens.size() <= maxPensPerEpoch && m_links.size() <= maxLinksPerEpoch && m_clusters.size() <= maxClustersPerEpoch;
    }

    [[nodiscard]] std::optional<std::size_t> decodedTableBytes() const
    {
        return m_decodedTableBytes;
    }

    [[nodiscard]] std::optional<HistoryLineDecodeCost> decodedLineCost(std::size_t cells) const
    {
        HistoryLineDecodeCost cost;
        if (!cost.cells(cells))
            return std::nullopt;
        for (const LinkEntry& entry : m_links) {
            if (!cost.link(entry.key.size(), entry.uri.size()))
                return std::nullopt;
        }
        for (const auto& cluster : m_clusters) {
            if (!cost.cluster(cluster.size()))
                return std::nullopt;
        }
        return cost;
    }

    struct Mark {
        std::size_t pens;
        std::size_t links;
        std::size_t clusters;
        std::size_t tableBytes;
        std::optional<std::size_t> decodedTableBytes;
    };

    [[nodiscard]] Mark mark() const { return { m_pens.size(), m_links.size(), m_clusters.size(), m_tableBytes, m_decodedTableBytes }; }

    // Forgets every entry named since `mark`.
    void rollBack(const Mark& mark)
    {
        m_pens.resize(mark.pens);
        m_links.resize(mark.links);
        m_linkIDs.resize(mark.links);
        m_clusters.resize(mark.clusters);
        m_clusterIDs.resize(mark.clusters);
        m_clusterHostIDs.resize(mark.clusters);
        m_tableBytes = mark.tableBytes;
        m_decodedTableBytes = mark.decodedTableBytes;
    }

    void writeTables(WireWriter& writer, TableIDs ids) const
    {
        writer.varint(m_pens.size());
        for (const Pen& pen : m_pens)
            writePen(writer, pen);
        writer.varint(m_links.size());
        for (const LinkEntry& entry : m_links) {
            if (ids == TableIDs::Written)
                writer.u16(entry.hostID);
            writer.bytes({ reinterpret_cast<const std::uint8_t*>(entry.key.data()), entry.key.size() });
            writer.bytes({ reinterpret_cast<const std::uint8_t*>(entry.uri.data()), entry.uri.size() });
        }
        writer.varint(m_clusters.size());
        for (std::size_t index = 0; index < m_clusters.size(); ++index) {
            if (ids == TableIDs::Written)
                writer.u16(m_clusterHostIDs[index]);
            writer.varint(m_clusters[index].size());
            for (const std::uint32_t codePoint : m_clusters[index])
                writer.varint(codePoint);
        }
    }

    // Where a cell's link or cluster id landed in these tables, one past the
    // index; 0 for one these tables do not hold.
    [[nodiscard]] std::uint16_t linkIndex(std::uint16_t id) const
    {
        const auto found = std::ranges::find(m_linkIDs, id);
        return found == m_linkIDs.end() ? 0 : static_cast<std::uint16_t>(found - m_linkIDs.begin() + 1);
    }

    [[nodiscard]] std::uint16_t clusterIndex(std::uint16_t id) const
    {
        const auto found = std::ranges::find(m_clusterIDs, id);
        return found == m_clusterIDs.end() ? 0 : static_cast<std::uint16_t>(found - m_clusterIDs.begin() + 1);
    }

    [[nodiscard]] std::span<const LinkEntry> links() const { return m_links; }
    [[nodiscard]] std::span<const std::vector<std::uint32_t>> clusters() const { return m_clusters; }
    [[nodiscard]] std::span<const std::uint16_t> clusterHostIDs() const { return m_clusterHostIDs; }

private:
    void countDecodedTableBytes(std::size_t count, std::size_t bytes = 1)
    {
        if (m_decodedTableBytes && !HistoryLineDecodeCost::add(*m_decodedTableBytes, count, bytes))
            m_decodedTableBytes.reset();
    }

    const Resolver& m_resolver;
    std::vector<Pen> m_pens;
    std::vector<std::uint16_t> m_linkIDs;
    std::vector<LinkEntry> m_links;
    std::vector<std::uint16_t> m_clusterIDs;
    std::vector<std::uint16_t> m_clusterHostIDs;
    std::vector<std::vector<std::uint32_t>> m_clusters;
    std::size_t m_tableBytes = 0;
    std::optional<std::size_t> m_decodedTableBytes { 1 + sizeof(std::size_t) };
};

// The canonical hash of a line's cells, with the tables the line itself names.
template<typename Resolver>
std::optional<std::uint64_t> canonicalHash(std::span<const Cell> cells, std::uint8_t lineAttribute, TableNames<Resolver>& names)
{
    std::vector<std::uint8_t> bytes;
    constexpr std::size_t minCapacity = 64;
    constexpr std::size_t estimatedBytesPerCell = 2;
    if (cells.size() > (bytes.max_size() - minCapacity) / estimatedBytesPerCell)
        return std::nullopt;
    bytes.reserve(cells.size() * estimatedBytesPerCell + minCapacity);
    WireWriter writer(bytes);
    writer.u8(lineAttribute);
    if (!Screen::writeRowForm(cells, names, writer))
        return std::nullopt;
    names.writeTables(writer, TableIDs::Omitted);
    return blockHash(bytes);
}

std::optional<JoinedLine> joinRows(const Ubiquitty::Terminal& terminal, const Ubiquitty::LineStore::LogicalLineRows& rows)
{
    JoinedLine line;
    for (int index = rows.firstIndex; index < rows.firstIndex + rows.rowCount; ++index) {
        const Ubiquitty::Screen::ScrollLine* row = Ubiquitty::Screen::scrollbackLine(&terminal, index);
        if (!row || row->length < 0)
            return std::nullopt;
        if (index == rows.firstIndex) {
            line.lineAttribute = row->lineAttribute;
            line.mark = row->mark;
        }
        std::size_t length = static_cast<std::size_t>(Ubiquitty::Screen::writtenLength(*row));
        const std::size_t room = maxHistoryLineCells - line.cells.size();
        if (length > room) {
            line.flags |= historyLineTruncated;
            length = room;
        }
        line.cells.insert(line.cells.end(), row->cells, row->cells + length);
        if (line.flags & historyLineTruncated)
            break;
    }
    return line;
}

} // namespace

std::optional<JoinedLine> joinedLine(const Ubiquitty::Terminal& terminal, std::uint64_t number)
{
    const std::optional<Ubiquitty::LineStore::LogicalLineRows> rows = Ubiquitty::Screen::logicalLineRows(terminal, number);
    if (!rows)
        return std::nullopt;
    return joinRows(terminal, *rows);
}

namespace {

std::optional<std::uint64_t> hashOf(const Ubiquitty::Terminal& terminal, const JoinedLine& line)
{
    const HostResolver resolver(terminal);
    TableNames<HostResolver> names(resolver);
    return canonicalHash<HostResolver>(line.cells, line.lineAttribute, names);
}

} // namespace

namespace {

// Extends a line's hash over its runs' text, given as (column, text) pairs in
// order of column: the line hash itself when there is none.
template<typename Runs>
std::uint64_t contentHashOver(std::uint64_t lineHash, const Runs& runs)
{
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    for (const auto& [column, text] : runs) {
        if (bytes.empty())
            writer.u64(lineHash);
        writer.varint(column);
        writer.varint(text.size());
        for (const std::uint32_t codePoint : text)
            writer.varint(codePoint);
    }
    return bytes.empty() ? lineHash : blockHash(bytes);
}

// The runs a host line's cells anchor, with their text.
std::vector<std::pair<std::size_t, std::span<const std::uint32_t>>> hostRuns(const Ubiquitty::Terminal& terminal, std::span<const Cell> cells)
{
    std::vector<std::pair<std::size_t, std::span<const std::uint32_t>>> runs;
    for (std::size_t column = 0; column < cells.size(); ++column) {
        if (const std::span<const std::uint32_t> text = Ubiquitty::runTextCodePoints(&terminal, cells[column]); !text.empty())
            runs.emplace_back(column, text);
    }
    return runs;
}

std::optional<std::uint64_t> contentHashOf(const Ubiquitty::Terminal& terminal, const JoinedLine& line)
{
    const std::optional<std::uint64_t> hash = hashOf(terminal, line);
    if (!hash)
        return std::nullopt;
    return contentHashOver(*hash, hostRuns(terminal, line.cells));
}

} // namespace

std::uint64_t DecodedLine::contentHash() const
{
    std::vector<std::pair<std::size_t, std::span<const std::uint32_t>>> runs;
    runs.reserve(runTexts.size());
    for (const RunText& run : runTexts)
        runs.emplace_back(run.column, run.codePoints);
    return contentHashOver(lineHash, runs);
}

std::optional<std::uint64_t> logicalLineContentHash(const Ubiquitty::Terminal& terminal, std::uint64_t number)
{
    const std::optional<JoinedLine> line = joinedLine(terminal, number);
    if (!line)
        return std::nullopt;
    return contentHashOf(terminal, *line);
}

std::optional<std::uint64_t> logicalLineHash(const Ubiquitty::Terminal& terminal, std::uint64_t number)
{
    const std::optional<JoinedLine> line = joinedLine(terminal, number);
    if (!line)
        return std::nullopt;
    return hashOf(terminal, *line);
}

std::optional<JoinedLine> joinedOpenLine(const Ubiquitty::Terminal& terminal)
{
    const std::optional<Ubiquitty::LineStore::LogicalLineRows> rows = Ubiquitty::Screen::openLineRows(terminal);
    if (!rows)
        return std::nullopt;
    std::optional<JoinedLine> line = joinRows(terminal, *rows);
    // An open line with no cells is the withdrawal; one that shows nothing is
    // none.
    if (!line || line->cells.empty())
        return std::nullopt;
    line->flags |= historyLineOpen;
    return line;
}

std::optional<OpenLine> openLineHash(const Ubiquitty::Terminal& terminal)
{
    const std::optional<JoinedLine> line = joinedOpenLine(terminal);
    if (!line)
        return std::nullopt;
    const std::optional<std::uint64_t> hash = hashOf(terminal, *line);
    if (!hash)
        return std::nullopt;
    return OpenLine { .number = Ubiquitty::Screen::historyAddress(terminal).head, .lineHash = *hash, .contentHash = contentHashOver(*hash, hostRuns(terminal, line->cells)) };
}

namespace {

// Appends one joined line to a body under construction: its flags, its record
// named through the body's tables, and its hash.
bool appendJoined(const Ubiquitty::Terminal& terminal, const JoinedLine& joined, TableNames<HostResolver>& bodyNames, std::vector<std::uint8_t>& output, HistoryLineDecodeCost* decodedCost = nullptr)
{
    const HostResolver resolver(terminal);
    TableNames<HostResolver> lineNames(resolver);
    const std::optional<std::uint64_t> hash = canonicalHash<HostResolver>(joined.cells, joined.lineAttribute, lineNames);
    if (!hash)
        return false;
    if (decodedCost) {
        const auto cost = lineNames.decodedLineCost(joined.cells.size());
        if (!cost)
            return false;
        *decodedCost = *cost;
    }
    WireWriter writer(output);
    writer.u8(joined.flags);
    writer.u8(joined.lineAttribute);
    writer.u8(0);
    writer.u8(joined.mark);
    if (!Screen::writeRowForm(joined.cells, bodyNames, writer))
        return false;
    writer.u64(*hash);
    return true;
}

// One proportional run's text, as a body's `runTexts` names it.
struct BodyRunText {
    std::size_t line = 0;
    std::uint32_t column = 0;
    std::uint16_t id = 0;
    std::span<const std::uint32_t> codePoints;
};

// The run texts a body's lines name, and a bound on the bytes they take.
class BodyRunTexts {
public:
    // Notes the runs of the body's line `line`.
    void note(const Ubiquitty::Terminal& terminal, std::span<const Cell> cells, std::size_t line)
    {
        for (std::size_t column = 0; column < cells.size(); ++column) {
            const std::span<const std::uint32_t> text = Ubiquitty::runTextCodePoints(&terminal, cells[column]);
            if (text.empty())
                continue;
            if (text.size() > static_cast<std::size_t>(runTextMaximumCodePoints)
                || !cells[column].pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)
                || std::ranges::any_of(text, [](std::uint32_t codePoint) { return !isScalarValue(codePoint); })) {
                m_decodedBytes.reset();
                return;
            }
            m_entries.push_back({ .line = line, .column = static_cast<std::uint32_t>(column), .id = cells[column].clusterID(), .codePoints = text });
            m_bytes += 5 + 5 + 2 + 5 + 5 * text.size();
            if (m_decodedBytes) {
                if (text.size() > RunTextStore::maxLiveCodePoints - m_decodedCodePoints
                    || !HistoryLineDecodeCost::add(*m_decodedBytes, 3, sizeof(DecodedLine::RunText))
                    || !HistoryLineDecodeCost::add(*m_decodedBytes, text.size(), sizeof(std::uint32_t)))
                    m_decodedBytes.reset();
                else
                    m_decodedCodePoints += text.size();
            }
        }
    }

    struct Mark {
        std::size_t entries;
        std::size_t bytes;
        std::optional<std::size_t> decodedBytes;
        std::size_t decodedCodePoints;
    };

    [[nodiscard]] Mark mark() const { return { m_entries.size(), m_bytes, m_decodedBytes, m_decodedCodePoints }; }

    void rollBack(const Mark& mark)
    {
        m_entries.resize(mark.entries);
        m_bytes = mark.bytes;
        m_decodedBytes = mark.decodedBytes;
        m_decodedCodePoints = mark.decodedCodePoints;
    }

    // A bound on the bytes the section takes written, its count included.
    [[nodiscard]] std::size_t bytes() const { return m_entries.empty() ? 0 : 5 + m_bytes; }

    [[nodiscard]] std::optional<std::size_t> decodedStorageBytes() const
    {
        return m_entries.size() <= static_cast<std::size_t>(RunTextStore::maxEntries) ? m_decodedBytes : std::nullopt;
    }

    void write(WireWriter& writer) const
    {
        if (m_entries.empty())
            return;
        writer.varint(m_entries.size());
        for (const BodyRunText& entry : m_entries) {
            writer.varint(entry.line);
            writer.varint(entry.column);
            writer.u16(entry.id);
            writer.varint(entry.codePoints.size());
            for (const std::uint32_t codePoint : entry.codePoints)
                writer.varint(codePoint);
        }
    }

private:
    std::vector<BodyRunText> m_entries;
    std::size_t m_bytes = 0;
    std::optional<std::size_t> m_decodedBytes { 0 };
    std::size_t m_decodedCodePoints = 0;
};

bool admitsDecodedHistory(const TableNames<HostResolver>& tables, const BodyRunTexts& runs, std::size_t retained, const HistoryLineDecodeCost& line, std::size_t maxDecodedBytes)
{
    const auto tableBytes = tables.decodedTableBytes();
    const auto runBytes = runs.decodedStorageBytes();
    HistoryDecodeBudget budget { maxDecodedBytes };
    return tables.isWithinDecodeLimits() && tableBytes && runBytes && budget.take(*tableBytes) && budget.take(*runBytes)
        && budget.take(retained) && line.fits(budget);
}

// Appends history line `number` to a body under construction, as the body's
// line `index`.
bool appendLine(const Ubiquitty::Terminal& terminal, std::uint64_t number, std::size_t index, TableNames<HostResolver>& bodyNames, BodyRunTexts& runTexts, std::vector<std::uint8_t>& output, HistoryLineDecodeCost* decodedCost = nullptr)
{
    const std::optional<JoinedLine> line = joinedLine(terminal, number);
    if (!line || !appendJoined(terminal, *line, bodyNames, output, decodedCost))
        return false;
    runTexts.note(terminal, line->cells, index);
    return true;
}

// Three table counts and the line count, each a varint of at most five bytes.
constexpr std::size_t bodyCountBytes = 4 * 5;

} // namespace

bool HistoryPublisher::capture(const Ubiquitty::Terminal& terminal)
{
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(terminal);
    const bool isSameEpoch = m_hasCaptured && address.epoch == m_address.epoch;
    if (!isSameEpoch)
        m_hashes.clear();
    // The first capture of an epoch has no "since": nothing is an append.
    std::uint64_t from = isSameEpoch ? std::min(m_address.head, address.head) : address.head;
    m_capturedAppends = { from, address.head };
    m_address = address;
    m_hasCaptured = true;

    // Hash the newest lines; a re-advance over withdrawn numbers rehashes
    // them, since what came back may differ from what left.
    std::uint64_t hashFrom = isSameEpoch ? from : address.oldest;
    if (address.head - hashFrom > maxPublishedHashes)
        hashFrom = address.head - maxPublishedHashes;
    hashFrom = std::max(hashFrom, address.oldest);
    // Numbers at or above the head stay: a pull-back withdrew them, and a
    // shrink may push the same lines out again. A number that comes back with
    // another hash is a rewritten line, which every attachment holding it is
    // sent again.
    std::vector<PublishedHash> replaced;
    std::erase_if(m_hashes, [&](const PublishedHash& entry) {
        if (entry.number < hashFrom || entry.number >= address.head)
            return false;
        replaced.push_back(entry);
        return true;
    });
    const auto insertAt = std::ranges::lower_bound(m_hashes, hashFrom, {}, &PublishedHash::number);
    std::vector<PublishedHash> fresh;
    bool isWhole = true;
    bool didRewrite = false;
    m_capturedLinkIDs.clear();
    for (std::uint64_t number = hashFrom; number < address.head; ++number) {
        const std::optional<JoinedLine> line = joinedLine(terminal, number);
        const std::optional<std::uint64_t> hash = line ? contentHashOf(terminal, *line) : std::nullopt;
        if (line) {
            for (const Cell& cell : line->cells) {
                if (const std::uint16_t link = cell.linkID(); link != 0 && (m_capturedLinkIDs.empty() || m_capturedLinkIDs.back() != link))
                    m_capturedLinkIDs.push_back(link);
            }
        }
        if (!hash) {
            isWhole = false;
            break;
        }
        const auto previous = std::ranges::find(replaced, number, &PublishedHash::number);
        PublishedHash entry { .number = number, .hash = *hash, .revision = previous != replaced.end() ? previous->revision : 0 };
        if (previous != replaced.end() && previous->hash != *hash) {
            entry.revision = m_revision + 1;
            didRewrite = true;
        }
        fresh.push_back(entry);
    }
    if (didRewrite)
        ++m_revision;
    m_hashes.insert(insertAt, fresh.begin(), fresh.end());
    while (m_hashes.size() > maxPublishedHashes)
        m_hashes.pop_front();

    captureOpenLine(terminal, address);

    m_capturedBody.clear();
    m_capturedBodyLines = 0;
    const std::uint64_t appends = m_capturedAppends.to - m_capturedAppends.from;
    if (isWhole && appends > 0 && appends <= maxCapturedLines) {
        std::vector<std::uint8_t> body;
        const std::size_t written = writeBody(terminal, m_capturedAppends, HistoryOrder::Ascending, maxFrameBodyBytes, body);
        if (written == appends) {
            m_capturedBody = std::move(body);
            m_capturedBodyLines = written;
        }
    }
    return isWhole;
}

std::optional<HistoryPublisher::Continuation> HistoryPublisher::continuation() const
{
    if (!m_hasCaptured)
        return std::nullopt;
    return Continuation { .address = m_address, .capturedAppends = m_capturedAppends, .hashes = { m_hashes.begin(), m_hashes.end() }, .revision = m_revision, .capturedLinkIDs = m_capturedLinkIDs };
}

bool HistoryPublisher::restore(const Ubiquitty::Terminal& terminal, Continuation continuation)
{
    const auto address = Ubiquitty::Screen::historyAddress(terminal);
    if (address.epoch != continuation.address.epoch || address.oldest != continuation.address.oldest
        || address.head != continuation.address.head || address.clearedThrough != continuation.address.clearedThrough
        || continuation.hashes.size() > maxPublishedHashes || continuation.capturedAppends.from > continuation.capturedAppends.to
        || continuation.capturedAppends.to != address.head
        || continuation.revision == std::numeric_limits<std::uint64_t>::max()
        || continuation.capturedLinkIDs.size() > (maxPublishedHashes + 1) * maxHistoryLineCells)
        return false;
    const auto maxHead = terminal.lineStore ? terminal.lineStore->maxHeadLineNumber() : address.head;
    for (std::size_t index = 0; index < continuation.hashes.size(); ++index) {
        const auto& entry = continuation.hashes[index];
        if (entry.number >= maxHead || entry.revision > continuation.revision
            || (index && entry.number <= continuation.hashes[index - 1].number))
            return false;
        if (entry.number >= address.oldest && entry.number < address.head) {
            const auto line = joinedLine(terminal, entry.number);
            const auto hash = line ? contentHashOf(terminal, *line) : std::nullopt;
            if (!hash || *hash != entry.hash)
                return false;
        }
    }
    if (std::ranges::find(continuation.capturedLinkIDs, 0) != continuation.capturedLinkIDs.end())
        return false;
    HistoryPublisher candidate;
    candidate.m_address = address;
    candidate.m_hasCaptured = true;
    candidate.m_capturedAppends = continuation.capturedAppends;
    candidate.m_hashes.assign(continuation.hashes.begin(), continuation.hashes.end());
    candidate.m_revision = continuation.revision;
    candidate.captureOpenLine(terminal, address);
    candidate.m_capturedLinkIDs = std::move(continuation.capturedLinkIDs);
    *this = std::move(candidate);
    return true;
}

void HistoryPublisher::captureOpenLine(const Ubiquitty::Terminal& terminal, const Ubiquitty::Screen::HistoryAddress& address)
{
    const std::optional<Ubiquitty::LineStore::LogicalLineRows> rows = Ubiquitty::Screen::openLineRows(terminal);
    if (!rows) {
        m_openLineKey.reset();
        m_openLine.reset();
        return;
    }
    // A hot line that only grows on the screen leaves its rows in history as
    // they were: no rehash until one of them moves.
    const OpenLineKey key { .epoch = address.epoch, .head = address.head, .rowCount = rows->rowCount, .rowsAppended = Ubiquitty::Screen::scrollbackTotalAppended(&terminal), .columns = terminal.columns };
    if (m_openLineKey == key)
        return;
    m_openLineKey = key;
    const std::optional<JoinedLine> line = joinedOpenLine(terminal);
    const std::optional<std::uint64_t> hash = line ? hashOf(terminal, *line) : std::nullopt;
    m_openLine = hash ? std::optional<OpenLine>(OpenLine { .number = address.head, .lineHash = *hash, .contentHash = contentHashOver(*hash, hostRuns(terminal, line->cells)) }) : std::nullopt;
    if (!line)
        return;
    for (const Cell& cell : line->cells) {
        if (const std::uint16_t link = cell.linkID(); link != 0 && (m_capturedLinkIDs.empty() || m_capturedLinkIDs.back() != link))
            m_capturedLinkIDs.push_back(link);
    }
}

std::size_t HistoryPublisher::writeBody(const Ubiquitty::Terminal& terminal, LineRange range, HistoryOrder order, std::size_t maxBytes, std::vector<std::uint8_t>& output, std::size_t maxDecodedBytes)
{
    if (order == HistoryOrder::Ascending && maxDecodedBytes >= historyBodyLimits.maxDecodedBytes && m_capturedBodyLines > 0 && range == m_capturedAppends && m_capturedBody.size() <= maxBytes) {
        output.insert(output.end(), m_capturedBody.begin(), m_capturedBody.end());
        return m_capturedBodyLines;
    }
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(terminal);
    range.from = std::max(range.from, address.oldest);
    range.to = std::min(range.to, address.head);
    if (range.from >= range.to)
        return 0;

    const HostResolver resolver(terminal);
    TableNames<HostResolver> bodyNames(resolver);
    BodyRunTexts bodyRunTexts;
    std::vector<std::uint8_t> lines;
    std::size_t lineCount = 0;
    std::size_t decodedBytes = 0;
    std::size_t temporaryBytes = 0;
    const std::uint64_t count = range.to - range.from;
    for (std::uint64_t offset = 0; offset < count; ++offset) {
        const std::uint64_t number = order == HistoryOrder::Ascending ? range.from + offset : range.to - 1 - offset;
        const auto mark = bodyNames.mark();
        const auto runMark = bodyRunTexts.mark();
        const std::size_t before = lines.size();
        HistoryLineDecodeCost decodedCost;
        const bool didAppend = lineCount < historyBodyLimits.maxLines && appendLine(terminal, number, lineCount, bodyNames, bodyRunTexts, lines, &decodedCost);
        // The decoder admits every row header and body table before its first
        // row: the largest working set must fit beside that final retained sum.
        decodedCost.temporary = std::max(temporaryBytes, decodedCost.temporary);
        if (!didAppend || bodyCountBytes + bodyNames.tableBytes() + lines.size() + bodyRunTexts.bytes() > maxBytes
            || !admitsDecodedHistory(bodyNames, bodyRunTexts, decodedBytes, decodedCost, maxDecodedBytes)) {
            lines.resize(before);
            bodyNames.rollBack(mark);
            bodyRunTexts.rollBack(runMark);
            break;
        }
        decodedBytes += decodedCost.retained;
        temporaryBytes = decodedCost.temporary;
        ++lineCount;
    }
    if (lineCount == 0)
        return 0;
    WireWriter writer(output);
    bodyNames.writeTables(writer, TableIDs::Written);
    writer.varint(lineCount);
    writer.raw(lines);
    bodyRunTexts.write(writer);
    return lineCount;
}

bool HistoryPublisher::writeOpenLine(const Ubiquitty::Terminal& terminal, std::size_t maxBytes, std::vector<std::uint8_t>& output) const
{
    // Without an open line the body is the withdrawal: an open line with no
    // cells.
    std::optional<JoinedLine> line = m_openLine ? joinedOpenLine(terminal) : std::nullopt;
    if (!line)
        line = JoinedLine { .cells = {}, .flags = historyLineOpen, .lineAttribute = 0, .mark = 0 };
    const HostResolver resolver(terminal);
    TableNames<HostResolver> bodyNames(resolver);
    BodyRunTexts bodyRunTexts;
    std::vector<std::uint8_t> lines;
    HistoryLineDecodeCost decodedCost;
    if (!appendJoined(terminal, *line, bodyNames, lines, &decodedCost))
        return false;
    bodyRunTexts.note(terminal, line->cells, 0);
    if (bodyCountBytes + bodyNames.tableBytes() + lines.size() + bodyRunTexts.bytes() > maxBytes
        || !admitsDecodedHistory(bodyNames, bodyRunTexts, 0, decodedCost, historyBodyLimits.maxDecodedBytes))
        return false;
    WireWriter writer(output);
    bodyNames.writeTables(writer, TableIDs::Written);
    writer.varint(1);
    writer.raw(lines);
    bodyRunTexts.write(writer);
    return true;
}

std::optional<std::uint64_t> HistoryPublisher::publishedHash(std::uint64_t number) const
{
    const auto found = std::ranges::lower_bound(m_hashes, number, {}, &PublishedHash::number);
    if (found == m_hashes.end() || found->number != number)
        return std::nullopt;
    return found->hash;
}

std::vector<std::uint64_t> HistoryPublisher::rewrittenSince(std::uint64_t revision, std::uint64_t below) const
{
    std::vector<std::uint64_t> numbers;
    if (revision >= m_revision)
        return numbers;
    for (const PublishedHash& entry : m_hashes) {
        if (entry.number >= below)
            break;
        if (entry.revision > revision)
            numbers.push_back(entry.number);
    }
    return numbers;
}

namespace {

// Reads a body's `runTexts` into the lines they name: each names a sized
// anchor of its line, in order of line and column, with a text of one to
// `runTextMaximumCodePoints` scalar values, all of them together within what a
// run-text store holds.
bool decodeRunTexts(WireReader& reader, std::vector<DecodedLine>& lines, HistoryDecodeBudget& budget)
{
    // An entry is at least its line, column, id and length.
    const std::size_t count = reader.count(static_cast<std::size_t>(RunTextStore::maxEntries), 1 + 1 + 2 + 1);
    std::size_t total = 0;
    std::size_t previousLine = 0;
    std::uint64_t previousColumn = 0;
    for (std::size_t index = 0; index < count && reader.ok(); ++index) {
        // Each read is bounded by a size, so it fits a size on any target.
        const auto line = static_cast<std::size_t>(reader.varint(lines.size()));
        const auto column = static_cast<std::size_t>(reader.varint(maxHistoryLineCells));
        const std::uint16_t id = reader.u16();
        if (!reader.ok() || line >= lines.size() || column >= lines[line].cells.size() || id == 0 || id > RunTextStore::maxEntries
            || (index > 0 && (line < previousLine || (line == previousLine && column <= previousColumn))))
            return false;
        const Cell& anchor = lines[line].cells[column];
        if (anchor.pen.extendedAttributes.textSizeSpecID() == 0 || !anchor.pen.attributes.contains(Ubiquitty::VT::Attribute::Wide))
            return false;
        DecodedLine::RunText text { .column = static_cast<std::uint32_t>(column), .hostID = id, .codePoints = {} };
        const auto points = reader.count(static_cast<std::size_t>(runTextMaximumCodePoints), 1);
        if (!reader.ok() || !points || points > RunTextStore::maxLiveCodePoints - total
            || !budget.take(3, sizeof(DecodedLine::RunText)) || !budget.take(points, sizeof(std::uint32_t)))
            return false;
        text.codePoints.resize(points);
        total += points;
        for (std::uint32_t& codePoint : text.codePoints) {
            const std::uint64_t value = reader.varint(0x10FFFF);
            if (!isScalarValue(static_cast<std::uint32_t>(value)))
                return false;
            codePoint = static_cast<std::uint32_t>(value);
        }
        lines[line].runTexts.push_back(std::move(text));
        previousLine = line;
        previousColumn = column;
    }
    return reader.ok();
}

} // namespace

bool decodeHistoryBody(std::span<const std::uint8_t> body, std::uint64_t firstLine, HistoryOrder order, const HistoryBodyLimits& limits, std::vector<DecodedLine>& lines)
{
    if (body.size() > limits.maxBodyBytes)
        return false;
    WireReader reader(body);
    HistoryDecodeBudget budget { limits.maxDecodedBytes };
    const auto penCount = reader.count(maxPensPerEpoch, penWireBytes);
    if (!reader.ok() || !budget.take(penCount, sizeof(Pen)))
        return false;
    std::vector<Pen> pens(penCount);
    for (Pen& pen : pens)
        pen = readPen(reader);
    const auto linkCount = reader.count(maxLinksPerEpoch, 4);
    if (!reader.ok() || !budget.take(linkCount, sizeof(LinkEntry)))
        return false;
    std::vector<LinkEntry> links(linkCount);
    for (LinkEntry& link : links) {
        link.hostID = reader.u16();
        if (link.hostID == 0)
            reader.fail();
        const auto keyBytes = reader.bytes(Ubiquitty::VT::maxLinkKeyLength - 1);
        const auto uriBytes = reader.bytes(Ubiquitty::VT::maxLinkURILength - 1);
        if (!reader.ok())
            return false;
        const std::string_view key(reinterpret_cast<const char*>(keyBytes.data()), keyBytes.size());
        const std::string_view uri(reinterpret_cast<const char*>(uriBytes.data()), uriBytes.size());
        if (key.find('\0') != std::string_view::npos || uri.find('\0') != std::string_view::npos
            || !budget.take(key.size(), 2) || !budget.take(uri.size(), 2) || !budget.take(4))
            return false;
        link.key = std::string(key);
        link.uri = std::string(uri);
    }
    const auto clusterCount = reader.count(maxClustersPerEpoch, 4);
    if (!reader.ok() || !budget.take(clusterCount, sizeof(std::vector<std::uint32_t>) + sizeof(std::uint16_t)))
        return false;
    std::vector<std::vector<std::uint32_t>> clusters(clusterCount);
    std::vector<std::uint16_t> clusterHostIDs(clusters.size());
    for (std::size_t index = 0; index < clusters.size() && reader.ok(); ++index) {
        std::vector<std::uint32_t>& cluster = clusters[index];
        clusterHostIDs[index] = reader.u16();
        if (clusterHostIDs[index] == 0)
            reader.fail();
        const auto points = reader.count(ClusterPool::maxCodePoints, 1);
        if (!reader.ok() || !budget.take(points, sizeof(std::uint32_t)))
            return false;
        cluster.resize(points);
        for (std::uint32_t& codePoint : cluster) {
            const std::uint64_t value = reader.varint(0x10FFFF);
            if (!isScalarValue(static_cast<std::uint32_t>(value)))
                reader.fail();
            codePoint = static_cast<std::uint32_t>(value);
        }
        if (cluster.empty())
            reader.fail();
    }
    // A line is at least its flags, its record's three bytes and five empty
    // lists, and its hash.
    const std::size_t lineCount = reader.count(limits.maxLines, 1 + 3 + 6 + 8);
    if (!reader.ok())
        return false;
    if (order == HistoryOrder::Descending && lineCount > 0 && firstLine < lineCount - 1)
        return false;
    if (order == HistoryOrder::Ascending && lineCount > 0 && lineCount - 1 > std::numeric_limits<std::uint64_t>::max() - firstLine)
        return false;
    if (!budget.take(clusters.size() + 1) || !budget.take(sizeof(std::size_t)) || !budget.take(lineCount, 2 * sizeof(DecodedLine)))
        return false;

    RowReferences references;
    references.clusters.assign(clusters.size() + 1, true);
    references.clusters[0] = false;
    const BodyResolver resolver(links, clusters, clusterHostIDs);
    std::vector<DecodedLine> decoded;
    decoded.reserve(lineCount);
    for (std::size_t index = 0; index < lineCount; ++index) {
        DecodedLine line;
        line.number = order == HistoryOrder::Ascending ? firstLine + index : firstLine - index;
        line.flags = reader.u8();
        if (line.number == std::numeric_limits<std::uint64_t>::max() && !(line.flags & historyLineOpen))
            return false;
        WireReader extentReader = reader;
        (void)extentReader.take(3);
        const auto rowExtent = static_cast<std::size_t>(extentReader.varint(limits.maxLineCells));
        HistoryLineDecodeCost cost { .retained = 0, .temporary = 64 };
        if (!extentReader.ok() || !cost.cells(rowExtent) || !cost.fits(budget))
            return false;
        std::vector<Cell> scratch(rowExtent, defaultBlankCell);
        RowAttributes attributes;
        if (!reader.ok() || (line.flags & ~(historyLineTruncated | historyLineOpen)) != 0 || !decodeRowRecord(reader, pens, references, scratch, attributes) || attributes.isWrapped)
            return false;
        line.lineHash = reader.u64();
        if (!reader.ok())
            return false;
        line.lineAttribute = attributes.lineAttribute;
        line.mark = attributes.mark;
        std::size_t extent = scratch.size();
        while (extent > 0 && Screen::isSameCell(scratch[extent - 1], defaultBlankCell))
            --extent;
        std::array<bool, maxLinksPerEpoch + 1> usedLinks {};
        std::array<bool, maxClustersPerEpoch + 1> usedClusters {};
        for (const Cell& cell : std::span(scratch).first(extent)) {
            const auto link = cell.linkID();
            const auto cluster = cell.clusterID();
            if (link > links.size() || cluster > clusters.size())
                return false;
            if (link && !usedLinks[link]) {
                usedLinks[link] = true;
                if (!cost.link(links[link - 1].key.size(), links[link - 1].uri.size()))
                    return false;
            }
            if (cluster && !usedClusters[cluster]) {
                usedClusters[cluster] = true;
                if (!cost.cluster(clusters[cluster - 1].size()))
                    return false;
            }
        }
        if (!cost.fits(budget) || !budget.take(cost.retained))
            return false;
        line.cells.assign(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(extent));
        TableNames<BodyResolver> lineNames(resolver);
        const std::optional<std::uint64_t> hash = canonicalHash<BodyResolver>(line.cells, line.lineAttribute, lineNames);
        if (!hash || *hash != line.lineHash)
            return false;
        // The line keeps the entries it names, renumbered to its own tables.
        for (Cell& cell : line.cells) {
            if (const std::uint16_t link = cell.linkID(); link != 0)
                cell.setLinkID(lineNames.linkIndex(link));
            if (const std::uint16_t cluster = cell.clusterID(); cluster != 0)
                cell.setClusterID(lineNames.clusterIndex(cluster));
        }
        line.links.reserve(lineNames.links().size());
        for (const LinkEntry& link : lineNames.links())
            line.links.push_back(LinkDefinition { .id = link.hostID, .key = link.key, .uri = link.uri });
        line.clusters.assign(lineNames.clusters().begin(), lineNames.clusters().end());
        line.clusterHostIDs.assign(lineNames.clusterHostIDs().begin(), lineNames.clusterHostIDs().end());
        decoded.push_back(std::move(line));
    }
    if (!reader.isAtEnd() && !decodeRunTexts(reader, decoded, budget))
        return false;
    if (!reader.isAtEnd())
        return false;
    if (decoded.size() > lines.max_size() - lines.size())
        return false;
    lines.reserve(lines.size() + decoded.size());
    std::ranges::move(decoded, std::back_inserter(lines));
    return true;
}

bool encodeHistoryLine(const DecodedLine& line, std::size_t maxBytes, std::vector<std::uint8_t>& output)
{
    if (line.cells.size() > maxHistoryLineCells || line.clusters.size() != line.clusterHostIDs.size()
        || line.links.size() > maxLinksPerEpoch || line.clusters.size() > maxClustersPerEpoch
        || line.runTexts.size() > static_cast<std::size_t>(RunTextStore::maxEntries)
        || (line.flags & ~(historyLineTruncated | historyLineOpen)) != 0)
        return false;
    std::size_t tableBytes = maxHistoryTransferBytes;
    for (const auto& link : line.links) {
        if (!link.id || link.key.size() > tableBytes)
            return false;
        tableBytes -= link.key.size();
        if (link.uri.size() > tableBytes)
            return false;
        tableBytes -= link.uri.size();
    }
    for (std::size_t index = 0; index < line.clusters.size(); ++index) {
        const auto& cluster = line.clusters[index];
        if (!line.clusterHostIDs[index] || cluster.empty() || cluster.size() > ClusterPool::maxCodePoints
            || std::ranges::any_of(cluster, [](auto point) { return !isScalarValue(point); }))
            return false;
    }
    std::size_t textPoints = 0;
    for (std::size_t index = 0; index < line.runTexts.size(); ++index) {
        const auto& run = line.runTexts[index];
        if (!run.hostID || run.hostID > RunTextStore::maxEntries || run.column >= line.cells.size()
            || (index && run.column <= line.runTexts[index - 1].column) || run.codePoints.empty()
            || run.codePoints.size() > runTextMaximumCodePoints || run.codePoints.size() > RunTextStore::maxLiveCodePoints - textPoints
            || std::ranges::any_of(run.codePoints, [](auto point) { return !isScalarValue(point); }))
            return false;
        const auto& anchor = line.cells[run.column];
        if (!anchor.pen.extendedAttributes.textSizeSpecID() || !anchor.pen.attributes.contains(Ubiquitty::VT::Attribute::Wide))
            return false;
        textPoints += run.codePoints.size();
    }
    for (const auto& cell : line.cells) {
        if (cell.linkID() > line.links.size() || cell.clusterID() > line.clusters.size())
            return false;
    }
    std::vector<LinkEntry> links;
    links.reserve(line.links.size());
    for (const auto& link : line.links)
        links.push_back({ .hostID = link.id, .key = link.key, .uri = link.uri });
    const BodyResolver resolver(links, line.clusters, line.clusterHostIDs);
    TableNames<BodyResolver> names(resolver);
    const auto hash = canonicalHash(line.cells, line.lineAttribute, names);
    if (!hash || *hash != line.lineHash)
        return false;
    std::vector<std::uint8_t> row;
    WireWriter rowWriter(row);
    rowWriter.u8(line.flags);
    rowWriter.u8(line.lineAttribute);
    rowWriter.u8(0);
    rowWriter.u8(line.mark);
    if (!Screen::writeRowForm(line.cells, names, rowWriter))
        return false;
    rowWriter.u64(*hash);
    std::vector<std::uint8_t> body;
    WireWriter writer(body);
    names.writeTables(writer, TableIDs::Written);
    writer.varint(1);
    writer.raw(row);
    if (!line.runTexts.empty()) {
        writer.varint(line.runTexts.size());
        for (const auto& run : line.runTexts) {
            writer.varint(0);
            writer.varint(run.column);
            writer.u16(run.hostID);
            writer.varint(run.codePoints.size());
            for (const auto point : run.codePoints)
                writer.varint(point);
        }
    }
    if (body.size() > std::min(maxBytes, maxHistoryTransferBytes))
        return false;
    output = std::move(body);
    return true;
}

} // namespace Ubiquitty::Protocol::Sync
