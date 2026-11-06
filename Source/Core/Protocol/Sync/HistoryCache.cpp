// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/HistoryCache.h"

#include "Core/ClusterPool.h"
#include "Core/LineStore.h"
#include "Core/Protocol/Sync/RowForm.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Terminal.h"
#include "Core/TextSizeTable.h"
#include "Core/Unicode/Joining.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

HistoryCache::HistoryCache(ID128 hostInstanceID, ID128 paneID, std::size_t maxLines, std::size_t maxBytes)
    : m_key { .hostInstanceID = hostInstanceID, .paneID = paneID, .epoch = 1 }
    , m_maxLines(maxLines)
    , m_maxBytes(maxBytes)
{
}

bool HistoryCache::rekeyForHandover(ID128 predecessor, ID128 successor)
{
    if (predecessor == ID128 {} || successor == ID128 {} || m_key.hostInstanceID != predecessor)
        return false;
    m_key.hostInstanceID = successor;
    return true;
}

namespace {

constexpr std::uint32_t historySnapshotMagic = 0x31484353;
constexpr std::size_t historySnapshotHeaderBytes = 68;

bool admitLineStorage(const DecodedLine& line, std::size_t& remaining)
{
    const auto admit = [&](std::size_t count, std::size_t size) {
        if (size && count > remaining / size)
            return false;
        remaining -= count * size;
        return true;
    };
    if (!admit(1, sizeof(DecodedLine)) || !admit(line.cells.size(), sizeof(Cell))
        || !admit(line.links.size(), sizeof(LinkDefinition)) || !admit(line.clusters.size(), sizeof(std::vector<std::uint32_t>))
        || !admit(line.clusterHostIDs.size(), sizeof(std::uint16_t)) || !admit(line.runTexts.size(), sizeof(DecodedLine::RunText)))
        return false;
    for (const auto& link : line.links) {
        if (!admit(link.key.size(), 1) || !admit(link.uri.size(), 1))
            return false;
    }
    for (const auto& cluster : line.clusters) {
        if (!admit(cluster.size(), sizeof(std::uint32_t)))
            return false;
    }
    for (const auto& run : line.runTexts) {
        if (!admit(run.codePoints.size(), sizeof(std::uint32_t)))
            return false;
    }
    return true;
}

} // namespace

std::size_t HistoryCache::lineStorageBytes(const DecodedLine& line)
{
    const auto maximum = std::numeric_limits<std::size_t>::max();
    auto remaining = maximum;
    return admitLineStorage(line, remaining) ? maximum - remaining : maximum;
}

void HistoryCache::evict(std::size_t bytes)
{
    while (!m_lines.empty() && (m_lines.size() > m_maxLines || m_retainedBytes > bytes)) {
        m_retainedBytes -= lineStorageBytes(m_lines.begin()->second);
        m_lines.erase(m_lines.begin());
    }
    while (!m_hidden.empty() && m_retainedBytes > bytes) {
        m_retainedBytes -= lineStorageBytes(m_hidden.begin()->second);
        m_hidden.erase(m_hidden.begin());
    }
}

void HistoryCache::trimToBytes(std::size_t bytes)
{
    evict(std::min(bytes, m_maxBytes));
}

std::optional<HistorySnapshot> HistoryCache::snapshot(std::size_t maxBytes, std::size_t maxRetainedBytes) const
{
    if (maxBytes < historySnapshotHeaderBytes || m_key.hostInstanceID == ID128 {} || m_key.paneID == ID128 {} || !m_key.epoch)
        return std::nullopt;
    const auto initialRetainedBytes = maxRetainedBytes;
    std::vector<std::uint8_t> bytes;
    WireWriter writer(bytes);
    writer.u32(historySnapshotMagic);
    writer.u64(m_key.hostInstanceID.low);
    writer.u64(m_key.hostInstanceID.high);
    writer.u64(m_key.paneID.low);
    writer.u64(m_key.paneID.high);
    writer.u32(m_key.epoch);
    writer.u64(m_clearedThrough);
    writer.u64(m_head);
    writer.u64(m_hostOldest);
    const auto countOffset = writer.size();
    writer.u32(0);
    std::uint32_t count = 0;
    const auto append = [&](const DecodedLine& line) -> std::optional<bool> {
        // One kind/number/length prefix, conservatively using the longest
        // varint; no partially encoded line enters the retained snapshot.
        constexpr std::size_t prefixBytes = 1 + 8 + 10;
        auto remainingStorage = maxRetainedBytes;
        if (maxBytes - bytes.size() < prefixBytes || !admitLineStorage(line, remainingStorage))
            return false;
        std::vector<std::uint8_t> body;
        if (!encodeHistoryLine(line, maxHistoryTransferBytes, body))
            return std::nullopt;
        if (body.size() > maxBytes - bytes.size() - prefixBytes)
            return false;
        maxRetainedBytes = remainingStorage;
        writer.u8((line.flags & historyLineOpen) ? 1 : 0);
        writer.u64(line.number);
        writer.bytes(body);
        ++count;
        return true;
    };
    if (const auto* open = openLine()) {
        const auto added = append(*open);
        if (!added)
            return std::nullopt;
        // A cap may omit the open line too; it is not a partial line.
    }
    for (auto line = m_lines.rbegin(); line != m_lines.rend(); ++line) {
        if (line->first >= m_head)
            continue;
        const auto added = append(line->second);
        if (!added)
            return std::nullopt;
        if (!*added)
            break;
    }
    for (std::size_t byte = 0; byte < 4; ++byte)
        bytes[countOffset + byte] = static_cast<std::uint8_t>(count >> (8 * byte));
    return HistorySnapshot { .bytes = std::move(bytes), .retainedBytes = initialRetainedBytes - maxRetainedBytes };
}

bool HistoryCache::restoreSnapshot(std::span<const std::uint8_t> bytes, std::size_t maxBytes, std::size_t maxRetainedBytes)
{
    if (bytes.size() > maxBytes)
        return false;
    WireReader reader(bytes);
    if (reader.u32() != historySnapshotMagic)
        return false;
    const ID128 host { reader.u64(), reader.u64() };
    const ID128 pane { reader.u64(), reader.u64() };
    HistoryCache candidate(host, pane, m_maxLines, m_maxBytes);
    candidate.m_key.epoch = reader.u32();
    candidate.m_clearedThrough = reader.u64();
    candidate.m_head = reader.u64();
    candidate.m_hostOldest = reader.u64();
    const auto count = reader.u32();
    if (!reader.ok() || host != m_key.hostInstanceID || pane != m_key.paneID || !candidate.m_key.epoch
        || candidate.m_clearedThrough > candidate.m_head || candidate.m_hostOldest > candidate.m_head || count > m_maxLines + 1)
        return false;
    std::uint64_t previous = candidate.m_head;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto kind = reader.u8();
        const auto number = reader.u64();
        const auto body = reader.bytes(maxHistoryTransferBytes);
        if (!reader.ok() || kind > 1 || number < candidate.m_clearedThrough
            || (kind == 1 ? index != 0 || number != candidate.m_head : number >= previous))
            return false;
        std::vector<DecodedLine> decoded;
        const HistoryBodyLimits limits { .maxLines = 1, .maxLineCells = maxHistoryLineCells, .maxBodyBytes = maxHistoryTransferBytes };
        if (!decodeHistoryBody(body, number, HistoryOrder::Ascending, limits, decoded) || decoded.size() != 1
            || ((decoded.front().flags & historyLineOpen) != 0) != (kind == 1) || !admitLineStorage(decoded.front(), maxRetainedBytes))
            return false;
        candidate.m_retainedBytes += lineStorageBytes(decoded.front());
        if (kind == 1)
            candidate.m_openLine = std::move(decoded.front());
        else {
            previous = number;
            candidate.m_lines.emplace(number, std::move(decoded.front()));
        }
    }
    if (!reader.ok() || !reader.isAtEnd() || candidate.m_lines.size() > m_maxLines || candidate.m_retainedBytes > m_maxBytes)
        return false;
    *this = std::move(candidate);
    return true;
}

CacheInsert HistoryCache::insert(std::uint32_t epoch, std::span<const DecodedLine> lines)
{
    if (epoch != m_key.epoch)
        return CacheInsert::WrongEpoch;
    if (std::ranges::any_of(lines, [&](const DecodedLine& line) { return line.number < m_clearedThrough; }))
        return CacheInsert::BelowClearedThrough;
    for (const DecodedLine& line : lines) {
        // A line that arrives for a hidden number replaces it for good.
        if (const auto hidden = m_hidden.find(line.number); hidden != m_hidden.end()) {
            m_retainedBytes -= lineStorageBytes(hidden->second);
            m_hidden.erase(hidden);
        }
        if (const auto held = m_lines.find(line.number); held != m_lines.end())
            m_retainedBytes -= lineStorageBytes(held->second);
        m_lines.insert_or_assign(line.number, line);
        m_retainedBytes += lineStorageBytes(line);
        noteChange(line.number);
        if (m_openLine && m_openLine->number == line.number) {
            m_retainedBytes -= lineStorageBytes(*m_openLine);
            m_openLine.reset();
        }
        // Evicting as each line lands keeps the peak at the caps, however
        // many lines one transfer carries.
        evict(m_maxBytes);
    }
    return CacheInsert::Stored;
}

CacheInsert HistoryCache::setOpenLine(std::uint32_t epoch, DecodedLine line)
{
    if (epoch != m_key.epoch)
        return CacheInsert::WrongEpoch;
    if (line.number < m_clearedThrough)
        return CacheInsert::BelowClearedThrough;
    // The host sends each open line after the one it replaces and aborts the
    // transfer of any it replaced, so the newest to arrive is the host's.
    if (m_openLine)
        m_retainedBytes -= lineStorageBytes(*m_openLine);
    if (line.cells.empty()) {
        m_openLine.reset();
    } else {
        m_retainedBytes += lineStorageBytes(line);
        m_openLine = std::move(line);
        evict(m_maxBytes);
    }
    return CacheInsert::Stored;
}

HistoryPurgeOutcome HistoryCache::reset(const HistoryReset& reset, HistoryPurgeSink* sink)
{
    if (sink && sink->purge(m_key, reset.clearedThrough) != HistoryPurgeOutcome::Purged)
        return HistoryPurgeOutcome::Refused;
    m_lines.clear();
    m_hidden.clear();
    m_openLine.reset();
    m_retainedBytes = 0;
    m_key.epoch = reset.newEpoch;
    m_clearedThrough = reset.clearedThrough;
    m_head = reset.historyHead;
    m_hostOldest = reset.clearedThrough;
    invalidateProjection();
    return HistoryPurgeOutcome::Purged;
}

void HistoryCache::setHead(std::uint32_t epoch, std::uint64_t head)
{
    if (epoch != m_key.epoch)
        return;
    if (head < m_head) {
        if (m_lines.lower_bound(head) != m_lines.end())
            noteChange(head);
        for (auto line = m_lines.lower_bound(head); line != m_lines.end();) {
            m_hidden.insert_or_assign(line->first, std::move(line->second));
            line = m_lines.erase(line);
        }
    } else {
        for (auto hidden = m_hidden.begin(); hidden != m_hidden.end() && hidden->first < head;) {
            noteChange(hidden->first);
            m_lines.insert_or_assign(hidden->first, std::move(hidden->second));
            hidden = m_hidden.erase(hidden);
        }
        evict(m_maxBytes);
    }
    m_head = head;
    if (m_openLine && m_openLine->number < head) {
        m_retainedBytes -= lineStorageBytes(*m_openLine);
        m_openLine.reset();
    }
}

void HistoryCache::noteHostOldest(std::uint64_t oldest)
{
    m_hostOldest = std::max(m_hostOldest, oldest);
}

const DecodedLine* HistoryCache::line(std::uint64_t number) const
{
    const auto found = m_lines.find(number);
    return found == m_lines.end() ? nullptr : &found->second;
}

bool HistoryCache::isBeyondHostHorizon(std::uint64_t number) const
{
    return number < m_hostOldest && m_lines.contains(number);
}

std::vector<LineRange> HistoryCache::cachedRanges() const
{
    std::vector<LineRange> ranges;
    for (const auto& [number, line] : m_lines) {
        if (!ranges.empty() && ranges.back().to == number)
            ++ranges.back().to;
        else
            ranges.push_back({ number, number + 1 });
    }
    return ranges;
}

std::vector<LineRange> HistoryCache::gaps(LineRange range) const
{
    std::vector<LineRange> gaps;
    range.from = std::max({ range.from, m_clearedThrough, m_hostOldest });
    std::uint64_t next = range.from;
    for (auto line = m_lines.lower_bound(range.from); line != m_lines.end() && line->first < range.to; ++line) {
        if (line->first > next)
            gaps.push_back({ next, line->first });
        next = line->first + 1;
    }
    if (next < range.to)
        gaps.push_back({ next, range.to });
    return gaps;
}

std::uint64_t HistoryCache::contiguousHead() const
{
    if (m_lines.empty())
        return m_clearedThrough;
    return m_lines.rbegin()->first + 1;
}

namespace {

// Whether a projected line's last row ends it, or continues onto the screen as
// the open line's does.
enum class LastRow : std::uint8_t {
    EndsLine,
    ContinuesLine,
};

// Lays one decoded line into rows of `columns` cells and appends them to the
// replica's scrollback, returning the rows appended. A wide character never
// straddles the margin: the row ends on an unwritten cell and the character
// starts the next, as the Core wraps it.
//
// Links and clusters resolve through the replica's link ring and cluster pool,
// which the screen plane keeps a mirror of the host's: a cell takes the host's
// id when the replica holds that id for what the line names, and none
// otherwise. Minting ids of the replica's own would collide with the ids the
// screen plane installs later.
int appendWrapped(Ubiquitty::Terminal& replica, const DecodedLine& line, int columns, LastRow lastRow = LastRow::EndsLine)
{
    int rows = 0;
    std::vector<std::uint16_t> links(line.links.size() + 1, 0);
    for (std::size_t index = 0; index < line.links.size(); ++index) {
        const LinkDefinition& wanted = line.links[index];
        const std::optional<LinkDefinition> held = linkDefinition(replica, wanted.id);
        links[index + 1] = held && held->key == wanted.key && held->uri == wanted.uri ? wanted.id : 0;
    }
    std::vector<std::uint16_t> clusters(line.clusters.size() + 1, 0);
    for (std::size_t index = 0; index < line.clusters.size(); ++index) {
        const std::uint16_t id = index < line.clusterHostIDs.size() ? line.clusterHostIDs[index] : 0;
        clusters[index + 1] = id != 0 && std::ranges::equal(replica.clusterPool.sequence(id), line.clusters[index]) ? id : 0;
    }

    // A run's anchor takes the host's run-text id. The replica's store mirrors
    // the host's as of the last checkpoint, and a run written since, which may
    // have scrolled into history without ever being on a screen the client
    // saw, is defined here under the host's id; the host keeps an id while any
    // line of its history names it, so the text is the one the host holds
    // there. An id the replica holds for another text names none, rather
    // than another run's words; the next checkpoint settles it.
    std::vector<std::pair<std::uint32_t, std::uint16_t>> runIDs;
    runIDs.reserve(line.runTexts.size());
    for (const DecodedLine::RunText& run : line.runTexts) {
        if (replica.runTextStore.text(run.hostID).empty())
            replica.runTextStore.appendDecoded(run.hostID, run.codePoints);
        runIDs.emplace_back(run.column, std::ranges::equal(replica.runTextStore.text(run.hostID), run.codePoints) ? run.hostID : std::uint16_t { 0 });
    }

    std::vector<Cell> row;
    row.reserve(static_cast<std::size_t>(columns));
    bool isFirstRow = true;
    bool needsShaping = false;
    const std::uint16_t undrawnSpecIDs = Ubiquitty::Screen::undrawnTextSizeSpecIDs(replica, replica.textSizeTable);
    const auto flush = [&](bool isWrapped) {
        // History uses the same renderer capabilities as the visible grid.
        if (undrawnSpecIDs != 0)
            (void)Ubiquitty::Screen::flattenSizedRun(row.data(), static_cast<int>(row.size()), defaultBlankCell, nullptr, nullptr, undrawnSpecIDs, replica.textSizeTable);
        Ubiquitty::Screen::scrollbackAppend(&replica, row.data(), static_cast<int>(row.size()), line.lineAttribute, isFirstRow ? line.mark : 0, isWrapped, needsShaping);
        row.clear();
        isFirstRow = false;
        needsShaping = false;
        ++rows;
    };
    const Cell unwritten {};
    auto nextRun = runIDs.begin();
    const auto resolved = [&](Cell cell, std::size_t column) {
        if (const std::uint16_t link = cell.linkID(); link != 0)
            cell.setLinkID(link < links.size() ? links[link] : 0);
        if (const std::uint16_t cluster = cell.clusterID(); cluster != 0)
            cell.setClusterID(cluster < clusters.size() ? clusters[cluster] : 0);
        while (nextRun != runIDs.end() && nextRun->first < column)
            ++nextRun;
        if (nextRun != runIDs.end() && nextRun->first == column)
            cell.setClusterID(nextRun->second);
        return cell;
    };
    const auto place = [&](std::span<const Cell> cells) {
        if (row.size() + cells.size() > static_cast<std::size_t>(columns)) {
            while (row.size() < static_cast<std::size_t>(columns))
                row.push_back(unwritten);
            flush(true);
        }
        for (const Cell& cell : cells) {
            needsShaping = needsShaping || Ubiquitty::Unicode::mayNeedShaping(cell.codePoint);
            row.push_back(cell);
        }
    };
    std::vector<Cell> chain;
    for (std::size_t index = 0; index < line.cells.size();) {
        const Cell cell = resolved(line.cells[index], index);
        // A chain is a wide lead and the continuation cells after it: a wide
        // character's pair, a sized glyph's block, a proportional run's claim.
        // It moves whole to the next row, as the Core's rewrap moves it.
        std::size_t end = index + 1;
        if (cell.pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)) {
            while (end < line.cells.size() && line.cells[end].pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                ++end;
        }
        if (end == index + 1) {
            place(std::span(&cell, 1));
            index = end;
            continue;
        }
        chain.assign(1, cell);
        for (std::size_t next = index + 1; next < end; ++next)
            chain.push_back(resolved(line.cells[next], next));
        index = end;
        if (chain.size() <= static_cast<std::size_t>(columns)) {
            place(chain);
            continue;
        }
        // A chain wider than the client's row has no row it fits on. The host
        // clamps a run to its own width, under a specification the client's
        // mirrored table does not hold and must not mint; the client lays the
        // chain flat instead, as a client that draws no sized text does.
        (void)Ubiquitty::Screen::flattenSizedRun(chain.data(), static_cast<int>(chain.size()), defaultBlankCell, nullptr, nullptr, Ubiquitty::allTextSizeSpecIDs, replica.textSizeTable);
        for (const Cell& flat : chain)
            place(std::span(&flat, 1));
    }
    flush(lastRow == LastRow::ContinuesLine);
    return rows;
}

} // namespace

void HistoryCache::project(Ubiquitty::Terminal& replica, int columns)
{
    if (columns < 1)
        return;
    const Ubiquitty::Screen::ScreenPinsAcrossHistoryRebuild screenPins(replica);
    replica.historyEpoch = m_key.epoch;
    replica.historyEpochBase = m_clearedThrough;
    // Only lines below the head are history on the client too: an append
    // arrives ahead of the update that scrolls it off the replica's screen.
    const auto limit = m_lines.lower_bound(m_head);
    const std::uint64_t newestEnd = limit == m_lines.begin() ? 0 : std::prev(limit)->first + 1;
    // The open line follows the run only when the run reaches it.
    const DecodedLine* open = openLine();
    if (open && limit != m_lines.begin() && newestEnd != open->number)
        open = nullptr;
    const std::optional<std::pair<std::uint64_t, std::uint64_t>> wantedOpen = open ? std::optional(std::pair { open->number, open->contentHash() }) : std::nullopt;
    const int heldRows = Ubiquitty::Screen::scrollbackCount(&replica);

    // The run the replica holds is still a prefix of the newest run: append
    // what came after it.
    const bool isRunKept = m_projectedEnd > m_projectedFirst ? m_lines.contains(m_projectedEnd - 1) && newestEnd >= m_projectedEnd : limit == m_lines.begin();
    const bool isAppendOnly = m_projectedColumns == columns && m_lowestChange >= m_projectedEnd && isRunKept && m_projectedOpenRows <= heldRows;
    if (isAppendOnly) {
        auto line = m_lines.find(m_projectedEnd);
        const bool hasNewLines = line != m_lines.end() && line != limit;
        // The open line's rows end the scrollback: lines past the run go
        // before it, and another open line replaces it.
        if (m_projectedOpenRows > 0 && (hasNewLines || m_projectedOpenLine != wantedOpen)) {
            replica.lineStore->dropNewest(m_projectedOpenRows);
            replica.scrollbackCount = replica.lineStore->count();
            m_projectedOpenRows = 0;
            m_projectedOpenLine.reset();
        }
        for (; line != limit && line != m_lines.end() && line->first == m_projectedEnd; ++line) {
            appendWrapped(replica, line->second, columns);
            ++m_projectedEnd;
        }
        // Only a run that reaches the newest line is the newest run.
        if (m_projectedEnd == newestEnd) {
            if (wantedOpen && !m_projectedOpenLine) {
                m_projectedOpenRows = appendWrapped(replica, *open, columns, LastRow::ContinuesLine);
                m_projectedOpenLine = wantedOpen;
            }
            m_lowestChange = std::numeric_limits<std::uint64_t>::max();
            return;
        }
    }
    Ubiquitty::Screen::resetScrollbackStore(&replica);
    m_projectedColumns = columns;
    m_lowestChange = std::numeric_limits<std::uint64_t>::max();
    m_projectedFirst = m_projectedEnd = 0;
    m_projectedOpenLine.reset();
    m_projectedOpenRows = 0;
    const auto startNumberingAt = [&](std::uint64_t number) {
        if (!replica.lineStore) {
            replica.lineStore = std::make_unique<Ubiquitty::LineStore>();
            replica.lineStore->init(replica.scrollbackLimit);
            replica.scrollbackCapacity = replica.lineStore->capacity();
        }
        // Number before appending: an oversized logical line may evict its
        // own beginning, whose missing cells cannot become a viewport anchor.
        replica.lineStore->restartNumbering(number);
    };
    const auto appendOpen = [&] {
        if (!open)
            return;
        m_projectedOpenRows = appendWrapped(replica, *open, columns, LastRow::ContinuesLine);
        m_projectedOpenLine = wantedOpen;
    };
    if (limit == m_lines.begin()) {
        if (open)
            startNumberingAt(open->number);
        appendOpen();
        return;
    }
    // The newest run, as deep as the replica's ring holds rows for.
    auto first = std::prev(limit);
    const auto rowsOf = [&](const DecodedLine& line) { return std::max<int>(1, static_cast<int>((line.cells.size() + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns))); };
    int rows = rowsOf(first->second) + (open ? rowsOf(*open) : 0);
    while (first != m_lines.begin()) {
        const auto previous = std::prev(first);
        if (previous->first + 1 != first->first || rows + rowsOf(previous->second) > replica.scrollbackLimit)
            break;
        rows += rowsOf(previous->second);
        first = previous;
    }
    startNumberingAt(first->first);
    for (auto line = first; line != limit; ++line)
        appendWrapped(replica, line->second, columns);
    appendOpen();
    m_projectedFirst = first->first;
    m_projectedEnd = newestEnd;
}

bool applyHistorySection(Ubiquitty::Terminal& terminal, std::span<const std::uint8_t> section, std::u32string_view olderLinesNote)
{
    if (section.empty() && olderLinesNote.empty())
        return true;
    std::vector<DecodedLine> lines;
    std::uint64_t head = 1;
    if (!section.empty()) {
        WireReader reader(section);
        const std::uint64_t newestLine = reader.u64();
        // Each body continues where the one before it ended, so the lines
        // are one run with no line said twice and none skipped.
        for (std::uint64_t first = newestLine;;) {
            const std::span<const std::uint8_t> body = reader.take(reader.u32());
            if (!reader.ok())
                return false;
            const HistoryBodyLimits limits {
                .maxLines = historyBodyLimits.maxLines,
                .maxLineCells = maxHistoryLineCells,
                .maxBodyBytes = body.size(),
            };
            const std::size_t before = lines.size();
            if (!decodeHistoryBody(body, first, HistoryOrder::Descending, limits, lines) || lines.size() == before)
                return false;
            if (reader.isAtEnd())
                break;
            const std::uint64_t taken = lines.size() - before;
            if (taken > first || reader.u64() != first - taken)
                return false;
            first -= taken;
        }
        if (newestLine == std::numeric_limits<std::uint64_t>::max())
            return false;
        head = newestLine + 1;
    }
    // The note takes the number under the oldest line carried, which a
    // section that left lines out has; with no line carried it is the only
    // line there is.
    if (!olderLinesNote.empty() && (lines.empty() || lines.back().number > 0)) {
        DecodedLine note;
        note.number = lines.empty() ? 0 : lines.back().number - 1;
        note.cells.resize(olderLinesNote.size());
        for (std::size_t index = 0; index < olderLinesNote.size(); ++index)
            note.cells[index].codePoint = olderLinesNote[index];
        lines.push_back(std::move(note));
    }
    HistoryCache cache({}, {}, lines.size());
    if (cache.insert(cache.key().epoch, lines) != CacheInsert::Stored)
        return false;
    cache.setHead(cache.key().epoch, head);
    cache.project(terminal, terminal.columns);
    // The terminal numbers its own history from the start of its epoch; a
    // name already given keeps naming its line.
    if (terminal.lineStore) {
        Ubiquitty::Screen::notePositionsRenumbered(terminal, terminal.historyEpochBase);
        terminal.lineStore->restartNumbering(terminal.historyEpochBase);
    }
    return true;
}

} // namespace Ubiquitty::Protocol::Sync
