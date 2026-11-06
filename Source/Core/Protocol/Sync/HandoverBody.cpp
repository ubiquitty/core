// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellGrid.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Image/TerminalImageInternal.h"
#include "Core/LineStore.h"
#include "Core/Protocol/Sync/HandoverGround.h"
#include "Core/Protocol/Sync/HandoverLedger.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/RowForm.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Terminal.h"
#include "Core/Triggers/Engine.h"
#include "Core/VT/DECPSSynth.h"
#include "Core/VT/Parser.h"
#include "Core/VT/SoftFont.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

void encodeHandoverHeader(std::vector<std::uint8_t>& output, const HandoverHeader& header)
{
    WireWriter writer(output);
    const auto identity = [&](ID128 value) {
        writer.u64(value.low);
        writer.u64(value.high);
    };
    writer.u16(handoverBodyVersion);
    identity(header.predecessor);
    writer.varint(header.inherited.size());
    for (const auto& issuer : header.inherited)
        identity(issuer);
    writer.u16(header.paneCount);
    writer.u32(header.tableBytes);
}

std::expected<HandoverHeader, HandoverBodyError> decodeHandoverHeader(std::span<const std::uint8_t> bytes, const HandoverHeaderLimits& limits)
{
    WireReader reader(bytes);
    const auto version = reader.u16();
    if (reader.ok() && version > handoverBodyVersion)
        return std::unexpected(HandoverBodyError::NewerVersion);
    if (!reader.ok() || version < oldestHandoverBodyVersion)
        return std::unexpected(HandoverBodyError::Malformed);
    const auto identity = [&] { return ID128 { reader.u64(), reader.u64() }; };
    HandoverHeader header;
    header.predecessor = identity();
    const auto inheritedCount = reader.varint();
    if (!reader.ok() || header.predecessor == ID128 {})
        return std::unexpected(HandoverBodyError::Malformed);
    if (inheritedCount > maxInheritedInstances)
        return std::unexpected(HandoverBodyError::OverCapacity);
    for (std::size_t i = 0; i < inheritedCount; ++i) {
        const auto issuer = identity();
        if (!reader.ok() || issuer == ID128 {} || issuer == header.predecessor
            || std::ranges::find(header.inherited, issuer) != header.inherited.end())
            return std::unexpected(HandoverBodyError::Malformed);
        header.inherited.push_back(issuer);
    }
    header.paneCount = reader.u16();
    header.tableBytes = reader.u32();
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(HandoverBodyError::Malformed);
    if (header.paneCount > limits.maxPanes || header.tableBytes > limits.maxTableBytes)
        return std::unexpected(HandoverBodyError::OverCapacity);
    return header;
}

namespace {

// The measuring pass and the writing pass use the same field walk. No vector
// growth or input-sized allocation precedes whole-section admission.
class SectionWriter {
public:
    explicit SectionWriter(std::size_t limit, std::span<std::uint8_t> output = {})
        : m_limit(limit)
        , m_output(output)
    {
    }

    void raw(std::span<const std::uint8_t> bytes)
    {
        if (m_error || bytes.size() > m_limit - m_size) {
            m_error = HandoverBodyError::OverCapacity;
            return;
        }
        if (!m_output.empty() && !bytes.empty())
            std::memcpy(m_output.data() + m_size, bytes.data(), bytes.size());
        m_size += bytes.size();
    }

    template<typename T>
    void scalar(T value)
    {
        if constexpr (std::is_enum_v<T>)
            scalar(std::to_underlying(value));
        else if constexpr (std::is_same_v<T, bool>)
            scalar(static_cast<std::uint8_t>(value));
        else {
            static_assert(std::is_integral_v<T>);
            std::array<std::uint8_t, sizeof(T)> bytes {};
            const auto bits = static_cast<std::make_unsigned_t<T>>(value);
            for (std::size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::uint8_t>(bits >> (i * 8));
            raw(bytes);
        }
    }

    [[nodiscard]] std::size_t size() const { return m_size; }
    [[nodiscard]] std::optional<HandoverBodyError> error() const { return m_error; }

private:
    std::size_t m_limit;
    std::span<std::uint8_t> m_output;
    std::size_t m_size = 0;
    std::optional<HandoverBodyError> m_error;
};

template<typename Write>
std::expected<HandoverBytes, HandoverBodyError> encodeSection(std::size_t limit, Write&& write)
{
    SectionWriter measure(limit);
    write(measure);
    measure.scalar(std::uint64_t { 0 });
    measure.scalar(std::uint64_t { 0 });
    if (measure.error())
        return std::unexpected(*measure.error());
    HandoverBytes result { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[measure.size()]), measure.size() };
    if (!result.data)
        return std::unexpected(HandoverBodyError::AllocationRefused);
    SectionWriter writer(result.size, { result.data.get(), result.size });
    write(writer);
    const auto digest = digestOf(result.view().first(writer.size()));
    writer.scalar(digest.low);
    writer.scalar(digest.high);
    return result;
}

std::expected<std::span<const std::uint8_t>, HandoverBodyError> verifiedSection(std::span<const std::uint8_t> bytes, std::size_t limit)
{
    if (bytes.size() > limit)
        return std::unexpected(HandoverBodyError::OverCapacity);
    WireReader versionReader(bytes);
    const auto version = versionReader.u16();
    if (versionReader.ok() && version > handoverBodyVersion)
        return std::unexpected(HandoverBodyError::NewerVersion);
    if (!versionReader.ok() || version < oldestHandoverBodyVersion || bytes.size() < 18)
        return std::unexpected(HandoverBodyError::Malformed);
    const auto covered = bytes.first(bytes.size() - 16);
    WireReader digestReader(bytes.last(16));
    const auto low = digestReader.u64();
    const auto high = digestReader.u64();
    if (digestOf(covered) != Digest128 { low, high })
        return std::unexpected(HandoverBodyError::DigestMismatch);
    return covered;
}

template<typename T>
void readScalar(WireReader& reader, T& destination)
{
    if constexpr (std::is_enum_v<T>) {
        std::underlying_type_t<T> value {};
        readScalar(reader, value);
        destination = static_cast<T>(value);
    } else if constexpr (std::is_same_v<T, bool>) {
        const auto value = reader.u8();
        if (value > 1)
            reader.fail();
        destination = value != 0;
    } else {
        static_assert(std::is_integral_v<T>);
        using Unsigned = std::make_unsigned_t<T>;
        Unsigned value {};
        for (std::size_t i = 0; i < sizeof(T); ++i)
            value |= static_cast<Unsigned>(reader.u8()) << (i * 8);
        destination = std::bit_cast<T>(value);
    }
}

// Fixed widths preserve the whole Cell, including unused inline data, without
// transporting object layout or remapping an external owner reference.
constexpr std::size_t historyCellBytes = 4 + penWireBytes + 4 + 2 + 1 + 2;

void writeHistoryCell(SectionWriter& writer, const Cell& cell)
{
    writer.scalar(cell.codePoint);
    writer.raw(encodePen(cell.pen));
    writer.scalar(cell.combining[0]);
    writer.scalar(cell.linkID());
    writer.scalar(cell.combiningLength());
    writer.scalar(cell.clusterID());
}

Cell readHistoryCell(WireReader& reader)
{
    Cell cell {};
    cell.codePoint = reader.u32();
    cell.pen = readPen(reader);
    cell.combining[0] = reader.u32();
    cell.setLinkID(reader.u16());
    const auto combining = reader.u8();
    const auto cluster = reader.u16();
    if (combining > cellMaxCombining || cluster > ClusterPool::maxEntries)
        reader.fail();
    cell.setCombiningLength(combining);
    cell.setClusterID(cluster);
    return cell;
}

struct HistorySection {
    std::uint32_t epoch = 0;
    std::uint64_t epochBase = 0;
    std::uint32_t configuredRows = 0;
    std::uint32_t capacity = 0;
    std::uint32_t rows = 0;
    std::uint64_t cells = 0;
    bool hasStore = false;
    LineStore::Continuation continuation {};
    std::span<const std::uint8_t> rowBytes;
    LineStore::RestorationPlan storage;
};

HandoverBodyError historyStorageError(LineStore::StorageError error)
{
    switch (error) {
    case LineStore::StorageError::Malformed:
        return HandoverBodyError::Malformed;
    case LineStore::StorageError::OverCapacity:
        return HandoverBodyError::OverCapacity;
    case LineStore::StorageError::AllocationRefused:
        return HandoverBodyError::AllocationRefused;
    }
    std::unreachable();
}

std::expected<HandoverHistoryStoragePlan, HandoverBodyError> historyStoragePlan(const HistorySection& section, const Terminal& terminal)
{
    HandoverHistoryStoragePlan plan { .retainedCandidateBytes = section.storage.retainedBytes };
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (std::cmp_greater(section.cells, maximum / sizeof(Cell)) || std::cmp_greater(section.rows, maximum / sizeof(StoredLine)))
        return std::unexpected(HandoverBodyError::OverCapacity);
    const auto add = [](std::size_t& total, std::size_t bytes) {
        if (bytes > std::numeric_limits<std::size_t>::max() - total)
            return false;
        total += bytes;
        return true;
    };
    plan.stagingBytes = static_cast<std::size_t>(section.cells) * sizeof(Cell);
    if (!add(plan.stagingBytes, static_cast<std::size_t>(section.rows) * sizeof(StoredLine)))
        return std::unexpected(HandoverBodyError::OverCapacity);
    if (terminal.lineStore) {
        const auto previous = terminal.lineStore->ownedBackingBytes();
        if (!previous)
            return std::unexpected(historyStorageError(previous.error()));
        plan.replacedBytes = *previous;
        if (!add(plan.replacedBytes, sizeof(LineStore)))
            return std::unexpected(HandoverBodyError::OverCapacity);
    }
    plan.peakPrepareBytes = plan.retainedCandidateBytes;
    if (!add(plan.peakPrepareBytes, plan.stagingBytes) || !add(plan.peakPrepareBytes, plan.replacedBytes))
        return std::unexpected(HandoverBodyError::OverCapacity);
    return plan;
}

std::expected<void, HandoverBodyError> validateHistory(const HistorySection& section, const HandoverHistoryLimits& limits)
{
    if (section.capacity > limits.maxCapacity || section.configuredRows > limits.maxCapacity
        || section.rows > limits.maxRows || section.cells > limits.maxCells)
        return std::unexpected(HandoverBodyError::OverCapacity);
    if (!section.epoch || section.capacity > static_cast<unsigned>(std::numeric_limits<int>::max())
        || section.configuredRows > static_cast<unsigned>(std::numeric_limits<int>::max()) || section.rows > section.capacity)
        return std::unexpected(HandoverBodyError::Malformed);
    if (!section.hasStore) {
        if (section.capacity || section.rows || section.cells)
            return std::unexpected(HandoverBodyError::Malformed);
        return {};
    }
    const auto& continuation = section.continuation;
    if (!section.capacity || continuation.front.number < section.epochBase || continuation.head < continuation.front.number
        || continuation.maxHead < continuation.head || continuation.totalAppended < 0
        || static_cast<std::uint64_t>(continuation.totalAppended) < section.rows
        || (continuation.front.continuity != LineStore::LineContinuity::StartsLine && continuation.front.continuity != LineStore::LineContinuity::ContinuesLine))
        return std::unexpected(HandoverBodyError::Malformed);
    return {};
}

void writeHistory(SectionWriter& writer, const Terminal& terminal, const HistorySection& section)
{
    writer.scalar(handoverBodyVersion);
    writer.scalar(section.epoch);
    writer.scalar(section.epochBase);
    writer.scalar(section.configuredRows);
    writer.scalar(section.hasStore);
    if (!section.hasStore)
        return;
    writer.scalar(section.capacity);
    writer.scalar(section.rows);
    writer.scalar(section.cells);
    writer.scalar(section.continuation.front.number);
    writer.scalar(section.continuation.front.continuity);
    writer.scalar(section.continuation.head);
    writer.scalar(section.continuation.maxHead);
    writer.scalar(static_cast<std::int64_t>(section.continuation.totalAppended));
    for (std::uint32_t index = 0; index < section.rows && !writer.error(); ++index) {
        const auto& row = *terminal.lineStore->at(static_cast<int>(index));
        writer.scalar(static_cast<std::uint32_t>(row.length));
        writer.scalar(row.wrapped);
        writer.scalar(row.lineAttribute);
        writer.scalar(row.mark);
        writer.scalar(row.needsShaping);
        for (int column = 0; column < row.length && !writer.error(); ++column)
            writeHistoryCell(writer, row.cells[column]);
    }
}

std::expected<HistorySection, HandoverBodyError> readHistory(std::span<const std::uint8_t> bytes, const HandoverHistoryLimits& limits)
{
    const auto covered = verifiedSection(bytes, limits.maxSectionBytes);
    if (!covered)
        return std::unexpected(covered.error());
    WireReader reader(*covered);
    (void)reader.u16();
    HistorySection section;
    section.epoch = reader.u32();
    section.epochBase = reader.u64();
    section.configuredRows = reader.u32();
    readScalar(reader, section.hasStore);
    if (section.hasStore) {
        section.capacity = reader.u32();
        section.rows = reader.u32();
        section.cells = reader.u64();
        section.continuation.front.number = reader.u64();
        readScalar(reader, section.continuation.front.continuity);
        section.continuation.head = reader.u64();
        section.continuation.maxHead = reader.u64();
        std::int64_t appended = 0;
        readScalar(reader, appended);
        section.continuation.totalAppended = appended;
    }
    if (!reader.ok())
        return std::unexpected(HandoverBodyError::Malformed);
    const auto validation = validateHistory(section, limits);
    if (!validation)
        return std::unexpected(validation.error());
    const auto rowOffset = reader.offset();
    std::uint64_t cells = 0;
    std::uint64_t completed = 0;
    std::optional<LineStore::RestoreLayout> layout;
    if (section.hasStore)
        layout.emplace(static_cast<int>(section.capacity), section.continuation);
    for (std::uint32_t index = 0; index < section.rows; ++index) {
        const auto length = reader.u32();
        bool isWrapped = false;
        bool needsShaping = false;
        readScalar(reader, isWrapped);
        (void)reader.u8(); // line attribute
        (void)reader.u8(); // prompt mark
        readScalar(reader, needsShaping);
        if (!reader.ok() || length > static_cast<unsigned>(maxColumns) || length > reader.remaining() / historyCellBytes
            || length > section.cells - cells)
            return std::unexpected(HandoverBodyError::Malformed);
        if (layout) {
            if (auto measured = layout->noteRow(static_cast<int>(length), isWrapped); !measured)
                return std::unexpected(historyStorageError(measured.error()));
        }
        for (std::uint32_t column = 0; column < length; ++column)
            (void)readHistoryCell(reader);
        cells += length;
        completed += !isWrapped;
    }
    if (!reader.ok() || !reader.isAtEnd() || cells != section.cells
        || (section.hasStore && completed != section.continuation.head - section.continuation.front.number))
        return std::unexpected(HandoverBodyError::Malformed);
    section.rowBytes = covered->subspan(rowOffset);
    if (layout) {
        const auto storage = layout->plan();
        if (!storage)
            return std::unexpected(historyStorageError(storage.error()));
        section.storage = *storage;
    }
    return section;
}

struct LinkView {
    std::uint32_t hash = 0;
    std::uint16_t id = 0;
    std::string_view key;
    std::string_view uri;
};

struct LinkSection {
    bool hasTable = false;
    std::uint16_t current = 0;
    std::uint16_t next = 0;
    std::uint16_t head = 0;
    std::uint16_t count = 0;
    std::uint32_t freeCount = 0;
    std::uint16_t reclaimBackoff = 0;
    std::array<LinkView, VT::linkTableCapacity> links;
    std::span<const std::uint8_t> freeBytes;
};

template<typename FreeID>
bool validateLinks(const LinkSection& section, const Terminal& terminal, FreeID&& freeID)
{
    if (section.count > VT::linkTableCapacity || section.head >= VT::linkTableCapacity
        || (!section.hasTable && section.count) || section.freeCount > 65535 || (section.next && section.freeCount)
        || section.reclaimBackoff > 4096)
        return false;
    std::array<std::uint64_t, 1024> referenced {};
    const auto mark = [&](std::uint16_t id) { referenced[id >> 6] |= std::uint64_t { 1 } << (id & 63); };
    const auto contains = [&](std::uint16_t id) { return (referenced[id >> 6] & (std::uint64_t { 1 } << (id & 63))) != 0; };
    for (std::size_t index = 0; index < section.count; ++index) {
        const auto& link = section.links[index];
        if (!link.id || !link.hash || contains(link.id) || link.key.size() >= VT::maxLinkKeyLength
            || link.uri.empty() || link.uri.size() >= VT::maxLinkURILength
            || link.key.find('\0') != std::string_view::npos || link.uri.find('\0') != std::string_view::npos)
            return false;
        const auto target = link.key.empty() ? link.uri : link.key;
        const auto capacity = link.key.empty() ? VT::maxLinkURILength : VT::maxLinkKeyLength;
        // Intern hashes the original target before clipping its stored copy.
        // Only a shorter copy proves that the full hashed target is present.
        if (target.size() < static_cast<std::size_t>(capacity - 1) && link.hash != Attributes::Hyperlinks::dedupHash(target))
            return false;
        mark(link.id);
    }
    mark(section.current);
    const auto markCells = [&](std::span<const Cell> cells) {
        for (const auto& cell : cells)
            mark(cell.linkID());
    };
    if (terminal.grid) {
        for (int row = 0; row < terminal.grid->rows(); ++row) {
            for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
                const auto cells = terminal.grid->rowCells(screen, Row(row));
                markCells({ cells.data, static_cast<std::size_t>(cells.count) });
            }
        }
    }
    if (terminal.lineStore) {
        for (int index = 0; index < terminal.lineStore->count(); ++index) {
            const auto* row = terminal.lineStore->at(index);
            markCells({ row->cells, static_cast<std::size_t>(row->length) });
        }
    }
    if (section.next) {
        for (std::uint32_t id = section.next; id <= 65535; ++id) {
            if (contains(static_cast<std::uint16_t>(id)))
                return false;
        }
    }
    for (std::uint32_t index = 0; index < section.freeCount; ++index) {
        const auto id = freeID(index);
        if (!id || contains(id))
            return false;
        mark(id); // Also rejects a duplicate free ID.
    }
    return true;
}

void writeLinkString(SectionWriter& writer, std::string_view value)
{
    writer.scalar(static_cast<std::uint16_t>(value.size()));
    writer.raw({ reinterpret_cast<const std::uint8_t*>(value.data()), value.size() });
}

std::string_view readLinkString(WireReader& reader, std::size_t capacity)
{
    const auto length = reader.u16();
    if (length >= capacity) {
        reader.fail();
        return {};
    }
    const auto bytes = reader.take(length);
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

template<typename FreeID>
void writeLinks(SectionWriter& writer, const LinkSection& section, FreeID&& freeID)
{
    writer.scalar(handoverBodyVersion);
    writer.scalar(section.hasTable);
    writer.scalar(section.current);
    writer.scalar(section.next);
    writer.scalar(section.head);
    writer.scalar(section.count);
    writer.scalar(section.freeCount);
    for (std::uint32_t index = 0; index < section.freeCount && !writer.error(); ++index)
        writer.scalar(freeID(index));
    for (std::size_t index = 0; index < section.count && !writer.error(); ++index) {
        const auto& link = section.links[index];
        writer.scalar(link.hash);
        writer.scalar(link.id);
        writeLinkString(writer, link.key);
        writeLinkString(writer, link.uri);
    }
    writer.scalar(section.reclaimBackoff);
}

std::expected<LinkSection, HandoverBodyError> readLinks(std::span<const std::uint8_t> bytes, std::size_t maxBytes)
{
    const auto covered = verifiedSection(bytes, maxBytes);
    if (!covered)
        return std::unexpected(covered.error());
    WireReader reader(*covered);
    (void)reader.u16();
    LinkSection section;
    readScalar(reader, section.hasTable);
    section.current = reader.u16();
    section.next = reader.u16();
    section.head = reader.u16();
    section.count = reader.u16();
    section.freeCount = reader.u32();
    if (!reader.ok() || section.count > section.links.size() || section.freeCount > 65535)
        return std::unexpected(HandoverBodyError::Malformed);
    section.freeBytes = reader.take(static_cast<std::size_t>(section.freeCount) * 2);
    for (std::size_t index = 0; index < section.count && reader.ok(); ++index) {
        auto& link = section.links[index];
        link.hash = reader.u32();
        link.id = reader.u16();
        link.key = readLinkString(reader, VT::maxLinkKeyLength);
        link.uri = readLinkString(reader, VT::maxLinkURILength);
    }
    section.reclaimBackoff = reader.u16();
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(HandoverBodyError::Malformed);
    return section;
}

struct TextStoreSection {
    std::uint16_t clusters = 0;
    ClusterPool::Continuation clusterContinuation;
    std::uint16_t runSlots = 0;
    std::uint16_t freeCount = 0;
    std::uint32_t sweepThreshold = 0;
    std::uint8_t runReclaimBackoff = 0;
    std::uint8_t sizeReclaimBackoff = 0;
    std::span<const std::uint8_t> clusterBytes;
    std::span<const std::uint8_t> runBytes;
    std::span<const std::uint8_t> freeBytes;
};

std::expected<TextStoreSection, HandoverBodyError> readTextStores(std::span<const std::uint8_t> bytes, const HandoverTextStoreLimits& limits)
{
    const auto covered = verifiedSection(bytes, limits.maxSectionBytes);
    if (!covered)
        return std::unexpected(covered.error());
    WireReader reader(*covered);
    (void)reader.u16();
    TextStoreSection section;
    section.clusters = reader.u16();
    section.clusterContinuation.nextID = reader.u16();
    section.clusterContinuation.oldestSlot = reader.u16();
    section.runSlots = reader.u16();
    section.freeCount = reader.u16();
    section.sweepThreshold = reader.u32();
    if (!reader.ok() || !section.clusterContinuation.fits(section.clusters) || section.runSlots > RunTextStore::maxEntries
        || section.freeCount > section.runSlots || section.sweepThreshold < RunTextStore::sweepFloorCodePoints
        || section.sweepThreshold > 2 * RunTextStore::maxLiveCodePoints)
        return std::unexpected(HandoverBodyError::Malformed);
    if (section.clusters > limits.maxClusterEntries || section.runSlots > limits.maxRunSlots)
        return std::unexpected(HandoverBodyError::OverCapacity);
    std::uint32_t codePoints = 0;
    const auto clusterOffset = reader.offset();
    for (std::uint32_t index = 0; index < section.clusters; ++index) {
        const auto length = reader.u8();
        if (!reader.ok() || !length || length > ClusterPool::maxCodePoints || length > reader.remaining() / 4)
            return std::unexpected(HandoverBodyError::Malformed);
        if (length > limits.maxCodePoints - codePoints)
            return std::unexpected(HandoverBodyError::OverCapacity);
        codePoints += length;
        for (unsigned column = 0; column < length; ++column) {
            if (!Unicode::isScalarValue(reader.u32()))
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    section.clusterBytes = covered->subspan(clusterOffset, reader.offset() - clusterOffset);
    const auto runOffset = reader.offset();
    std::array<std::uint64_t, RunTextStore::referenceWords> emptySlots {};
    std::uint32_t runCodePoints = 0;
    std::uint32_t emptyCount = 0;
    for (std::uint32_t id = 1; id <= section.runSlots; ++id) {
        const auto length = reader.u32();
        if (!reader.ok() || length > reader.remaining() / 4 || length > RunTextStore::maxLiveCodePoints - runCodePoints)
            return std::unexpected(HandoverBodyError::Malformed);
        if (length > limits.maxCodePoints - codePoints)
            return std::unexpected(HandoverBodyError::OverCapacity);
        codePoints += length;
        runCodePoints += length;
        if (!length) {
            emptySlots[id >> 6] |= std::uint64_t { 1 } << (id & 63);
            ++emptyCount;
        }
        for (std::uint32_t column = 0; column < length; ++column) {
            if (!Unicode::isScalarValue(reader.u32()))
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    section.runBytes = covered->subspan(runOffset, reader.offset() - runOffset);
    const auto freeOffset = reader.offset();
    if (emptyCount != section.freeCount)
        return std::unexpected(HandoverBodyError::Malformed);
    for (std::uint32_t index = 0; index < section.freeCount; ++index) {
        const auto id = reader.u16();
        if (!reader.ok() || !id || id > section.runSlots)
            return std::unexpected(HandoverBodyError::Malformed);
        auto& word = emptySlots[id >> 6];
        const auto bit = std::uint64_t { 1 } << (id & 63);
        if (!(word & bit))
            return std::unexpected(HandoverBodyError::Malformed);
        word &= ~bit;
    }
    section.freeBytes = covered->subspan(freeOffset, reader.offset() - freeOffset);
    section.runReclaimBackoff = reader.u8();
    section.sizeReclaimBackoff = reader.u8();
    if (!reader.ok() || !reader.isAtEnd() || section.runReclaimBackoff > 63 || section.sizeReclaimBackoff > 64)
        return std::unexpected(HandoverBodyError::Malformed);
    return section;
}

void writeTextStores(SectionWriter& writer, const Terminal& terminal)
{
    const auto cluster = terminal.clusterPool.continuation();
    const auto run = terminal.runTextStore.continuation();
    writer.scalar(handoverBodyVersion);
    writer.scalar(static_cast<std::uint16_t>(terminal.clusterPool.size()));
    writer.scalar(cluster.nextID);
    writer.scalar(cluster.oldestSlot);
    writer.scalar(run.slotCount);
    writer.scalar(static_cast<std::uint16_t>(run.freeIDs.size()));
    writer.scalar(static_cast<std::uint32_t>(run.sweepThreshold));
    for (const auto& entry : terminal.clusterPool.entries()) {
        if (writer.error())
            return;
        writer.scalar(entry.length);
        for (std::size_t index = 0; index < entry.length; ++index)
            writer.scalar(entry.codePoints[index]);
    }
    for (const auto& entry : terminal.runTextStore.slots()) {
        if (writer.error())
            return;
        writer.scalar(static_cast<std::uint32_t>(entry.size()));
        for (const auto codePoint : entry)
            writer.scalar(codePoint);
    }
    for (const auto id : run.freeIDs)
        writer.scalar(id);
    writer.scalar(static_cast<std::uint8_t>(terminal.runTextReclaimBackoffInterns));
    writer.scalar(static_cast<std::uint8_t>(terminal.textSizeReclaimBackoffInterns));
}

template<typename Visit, typename Destination, typename Source>
void handoverParserFields(Visit&& visit, Destination& to, Source& from)
{
    visit(to.parseState, from.parseState);
    visit(to.invalidUTF8Policy, from.invalidUTF8Policy);
    visit(to.parserInputIsUTF8, from.parserInputIsUTF8);
    visit(to.inputEncoding.enc, from.inputEncoding.enc);
    visit(to.inputEncoding.pending, from.inputEncoding.pending);
    visit(to.inputEncoding.acc, from.inputEncoding.acc);
    visit(to.inputEncoding.need, from.inputEncoding.need);
    visit(to.clusterActive, from.clusterActive);
    visit(to.clusterRow, from.clusterRow);
    visit(to.clusterColumn, from.clusterColumn);
    visit(to.clusterScreen, from.clusterScreen);
    visit(to.clusterLast, from.clusterLast);
    visit(to.clusterState.riCount, from.clusterState.riCount);
    visit(to.clusterState.pictExtend, from.clusterState.pictExtend);
    visit(to.clusterState.zwjAfterPict, from.clusterState.zwjAfterPict);
    visit(to.clusterState.prevRaw, from.clusterState.prevRaw);
    visit(to.clusterState.incbChain, from.clusterState.incbChain);
    visit(to.clusterAtX, from.clusterAtX);
    visit(to.clusterAtY, from.clusterAtY);
    visit(to.clusterAtWrap, from.clusterAtWrap);
    visit(to.clusterCodepointLength, from.clusterCodepointLength);
    for (std::size_t index = 0; index < from.clusterCodePoints.size(); ++index)
        visit(to.clusterCodePoints[index], from.clusterCodePoints[index]);
}

bool isValidHandoverParser(const Terminal::Parser& parser)
{
    if ((parser.parseState != VT::Parser::State::Ground && parser.parseState != VT::Parser::State::VT52Ground)
        || parser.invalidUTF8Policy > Unicode::InvalidUTF8Policy::Latin1 || parser.inputEncoding.enc > Unicode::Encoding::Big5
        || parser.inputEncoding.need || parser.clusterScreen > 1 || parser.clusterState.incbChain > 2
        || parser.clusterCodepointLength < 0 || parser.clusterCodepointLength > ClusterPool::maxCodePoints
        || (parser.clusterActive && !parser.clusterCodepointLength) || !Unicode::isScalarValue(parser.clusterLast)
        || parser.clusterRow < 0 || parser.clusterRow >= maxRows || parser.clusterColumn < 0 || parser.clusterColumn >= maxColumns
        || parser.clusterAtX < 0 || parser.clusterAtX > maxColumns || parser.clusterAtY < 0 || parser.clusterAtY >= maxRows)
        return false;
    for (int index = 0; index < parser.clusterCodepointLength; ++index) {
        if (!Unicode::isScalarValue(parser.clusterCodePoints[static_cast<std::size_t>(index)]))
            return false;
    }
    return true;
}

struct TekHeader {
    std::int32_t primitiveCount = 0;
    std::int32_t displayCount = 0;
    Tek::PlotMode plotMode {};
    Tek::TekParse parse {};
    Tek::LineStyle lineStyle {};
    Tek::CharSize charSize {};
    std::array<char, 4> csiBytes {};
    std::uint8_t csiLength = 0;
    bool didDropPrimitive = false;
    bool didSignalPrimitiveDrop = false;
    std::int16_t currentX = 0;
    std::int16_t currentY = 0;
    std::int16_t alphaX = 0;
    std::int16_t alphaY = 0;
    std::int16_t alphaMargin = 0;
    std::int32_t pendingX = 0;
    std::int32_t pendingY = 0;
    bool coordHasHighY = false; // bool-ok: Matches Tek::State for the shared field visitor.
    bool coordHasExtra = false; // bool-ok: Matches Tek::State for the shared field visitor.
    bool coordHasLowY = false; // bool-ok: Matches Tek::State for the shared field visitor.
    bool coordHasHighX = false; // bool-ok: Matches Tek::State for the shared field visitor.
    bool isNextVectorDark = false;
    bool incrementalPenDown = false; // bool-ok: Matches Tek::State for the shared field visitor.
    bool GINActive = false;
    std::int16_t GINX = 0;
    std::int16_t GINY = 0;
    bool bypass = false; // bool-ok: Matches Tek::State for the shared field visitor.
};

template<typename Visit, typename Destination, typename Source>
void tekHeaderFields(Visit&& visit, Destination& to, Source& from)
{
    visit(to.primitiveCount, from.primitiveCount);
    visit(to.displayCount, from.displayCount);
    visit(to.plotMode, from.plotMode);
    visit(to.parse, from.parse);
    visit(to.lineStyle, from.lineStyle);
    visit(to.charSize, from.charSize);
    for (unsigned index = 0; index < 4; ++index)
        visit(to.csiBytes[index], from.csiBytes[index]);
    visit(to.csiLength, from.csiLength);
    visit(to.didDropPrimitive, from.didDropPrimitive);
    visit(to.didSignalPrimitiveDrop, from.didSignalPrimitiveDrop);
    visit(to.currentX, from.currentX);
    visit(to.currentY, from.currentY);
    visit(to.alphaX, from.alphaX);
    visit(to.alphaY, from.alphaY);
    visit(to.alphaMargin, from.alphaMargin);
    visit(to.pendingX, from.pendingX);
    visit(to.pendingY, from.pendingY);
    visit(to.coordHasHighY, from.coordHasHighY);
    visit(to.coordHasExtra, from.coordHasExtra);
    visit(to.coordHasLowY, from.coordHasLowY);
    visit(to.coordHasHighX, from.coordHasHighX);
    visit(to.isNextVectorDark, from.isNextVectorDark);
    visit(to.incrementalPenDown, from.incrementalPenDown);
    visit(to.GINActive, from.GINActive);
    visit(to.GINX, from.GINX);
    visit(to.GINY, from.GINY);
    visit(to.bypass, from.bypass);
}

template<typename Visit, typename Font>
void softFontFields(Visit&& visit, Font& font)
{
    visit(font.valid);
    visit(font.set96);
    visit(font.designIntermediate);
    visit(font.designFinal);
    visit(font.matrixWidth);
    visit(font.matrixHeight);
    visit(font.serial);
    for (auto& hasGlyph : font.glyphSet)
        visit(hasGlyph);
    for (auto& glyph : font.glyph) {
        for (auto& row : glyph)
            visit(row);
    }
}

bool isValidSoftFont(const VT::SoftFont& font)
{
    if (font.matrixWidth > VT::drcsMaxMatrixWidth || font.matrixHeight > VT::drcsMaxMatrixHeight)
        return false;
    if (!font.valid)
        return true;
    return font.serial && font.matrixWidth && font.matrixHeight
        && (!font.designIntermediate || (font.designIntermediate >= 0x20 && font.designIntermediate <= 0x2f))
        && font.designFinal >= 0x30 && font.designFinal <= 0x7e;
}

bool isValidTekHeader(const TekHeader& header)
{
    const auto x = [](int value) { return value >= 0 && value < Tek::tekGridWidth; };
    const auto y = [](int value) { return value >= 0 && value < Tek::tekGridHeight; };
    return header.primitiveCount >= 0 && header.primitiveCount <= Tek::tekMaxPrimitives
        && header.displayCount >= 0 && header.displayCount <= Tek::tekMaxPrimitives
        && header.plotMode <= Tek::PlotMode::Incremental && header.parse <= Tek::TekParse::CSI
        && header.lineStyle <= Tek::LineStyle::LongDash && header.charSize <= Tek::CharSize::Small
        && x(header.currentX) && y(header.currentY) && x(header.alphaX) && y(header.alphaY)
        && (header.alphaMargin == 0 || header.alphaMargin == Tek::tekGridWidth / 2)
        && x(header.pendingX) && x(header.pendingY) && x(header.GINX) && y(header.GINY);
}

constexpr std::size_t tekPrimitiveBytes = 15;

template<typename Visit, typename Primitive>
void tekPrimitiveFields(Visit&& visit, Primitive& primitive)
{
    visit(primitive.kind);
    visit(primitive.lineStyle);
    visit(primitive.charSize);
    visit(primitive.x0);
    visit(primitive.y0);
    visit(primitive.x1);
    visit(primitive.y1);
    visit(primitive.codePoint);
}

bool isValidTekPrimitive(const Tek::Primitive& primitive)
{
    return primitive.kind <= Tek::PrimitiveKind::Glyph && primitive.lineStyle <= Tek::LineStyle::LongDash
        && primitive.charSize <= Tek::CharSize::Small && !primitive.pad
        && primitive.x0 >= 0 && primitive.x0 < Tek::tekGridWidth && primitive.x1 >= 0 && primitive.x1 < Tek::tekGridWidth
        && primitive.y0 >= 0 && primitive.y0 < Tek::tekGridHeight && primitive.y1 >= 0 && primitive.y1 < Tek::tekGridHeight
        && Unicode::isScalarValue(primitive.codePoint);
}

struct VisualSection {
    std::optional<VT::SoftFont> font;
    TekHeader tek;
    std::span<const std::uint8_t> primitiveBytes;
};

template<typename Visit, typename Request>
void soundFields(Visit&& visit, Request& request)
{
    visit(request.volume);
    visit(request.wave);
    visit(request.channel);
    visit(request.attackMilliseconds);
    visit(request.releaseMilliseconds);
    visit(request.semitonesDown);
    visit(request.durationMilliseconds);
    visit(request.gapMilliseconds);
    for (unsigned index = 0; index < PAL::PlayTonesRequest::maxNotes; ++index) {
        visit(request.freqs[index]);
        visit(request.noteVolumes[index]);
        visit(request.noteEffects[index]);
        visit(request.noteParameters[index]);
    }
    visit(request.count);
    visit(request.decayMilliseconds);
    visit(request.sustainLevel);
    visit(request.reverbSend);
    visit(request.gridUnits);
    visit(request.lowpassHertz);
    visit(request.highpassHertz);
    visit(request.sampleSlot);
    visit(request.pan);
    visit(request.repeat);
    visit(request.sampleFrames);
}

bool isValidSoundRequest(const PAL::PlayTonesRequest& request, std::uint32_t serial)
{
    const auto envelope = [](std::uint16_t value) { return value == PAL::PlayTonesRequest::waveDefaultEnvelope || value <= VT::toneMaxEnvelopeMilliseconds; };
    if (request.count < 0 || request.count > PAL::PlayTonesRequest::maxNotes
        || request.volume > VT::toneMaxVolume || request.wave > VT::toneMaxWave || request.channel > VT::toneMaxChannel
        || !envelope(request.attackMilliseconds) || !envelope(request.releaseMilliseconds)
        || request.decayMilliseconds > VT::toneMaxEnvelopeMilliseconds || request.sustainLevel > VT::toneMaxVolume
        || request.semitonesDown > VT::toneMaxSemitonesDown || request.reverbSend > VT::toneMaxVolume
        || request.durationMilliseconds > VT::toneMaxDurationUnits * 1000u / 32u || request.gapMilliseconds > request.durationMilliseconds
        || request.lowpassHertz > VT::toneMaxCutoffHertz || request.highpassHertz > VT::toneMaxCutoffHertz
        || request.pan > VT::toneMaxPan || request.sampleSlot > VT::sampleSlotCount
        || request.sampleFrames > VT::sampleMaxFrames || (request.wave != 6 && (request.sampleFrames || serial)))
        return false;
    for (unsigned index = 0; index < PAL::PlayTonesRequest::maxNotes; ++index) {
        if (request.noteVolumes[index] > VT::toneMaxVolume || request.noteEffects[index] > VT::toneMaxEffect)
            return false;
    }
    return true;
}

struct SoundEntry {
    PAL::PlayTonesRequest request;
    std::uint32_t serial = 0;
    std::span<const std::uint8_t> pcm;
};

struct SoundSection {
    bool hasBank = false;
    std::uint32_t serial = 0;
    std::array<std::uint16_t, VT::sampleSlotCount> tags {};
    std::array<std::span<const std::uint8_t>, VT::sampleSlotCount> slots {};
    bool hasPending = false;
    std::uint8_t head = 0;
    std::uint8_t count = 0;
    bool hasResetStop = false;
    std::array<SoundEntry, VT::PendingSound::depth> entries;
    std::size_t pendingPCMBytes = 0;
};

std::expected<SoundSection, HandoverBodyError> readSounds(std::span<const std::uint8_t> bytes, const HandoverSoundLimits& limits)
{
    const auto covered = verifiedSection(bytes, limits.maxSectionBytes);
    if (!covered)
        return std::unexpected(covered.error());
    WireReader reader(*covered);
    (void)reader.u16();
    SoundSection section;
    std::size_t pcmBytes = 0;
    const auto readPCM = [&](std::uint32_t frames) -> std::expected<std::span<const std::uint8_t>, HandoverBodyError> {
        if (!reader.ok() || frames > VT::sampleMaxFrames)
            return std::unexpected(HandoverBodyError::Malformed);
        const auto size = static_cast<std::size_t>(frames) * sizeof(std::int16_t);
        if (size > limits.maxPCMBytes - pcmBytes)
            return std::unexpected(HandoverBodyError::OverCapacity);
        if (size > reader.remaining())
            return std::unexpected(HandoverBodyError::Malformed);
        const auto view = reader.take(size);
        pcmBytes += size;
        return view;
    };
    readScalar(reader, section.hasBank);
    if (section.hasBank) {
        section.serial = reader.u32();
        for (unsigned index = 0; index < VT::sampleSlotCount; ++index) {
            section.tags[index] = reader.u16();
            const auto pcm = readPCM(reader.u32());
            if (!pcm)
                return std::unexpected(pcm.error());
            section.slots[index] = *pcm;
            if (section.tags[index] && (pcm->empty() || std::find(section.tags.begin(), section.tags.begin() + index, section.tags[index]) != section.tags.begin() + index))
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    readScalar(reader, section.hasPending);
    if (section.hasPending) {
        section.head = reader.u8();
        section.count = reader.u8();
        readScalar(reader, section.hasResetStop);
        if (!reader.ok() || section.head >= VT::PendingSound::depth || section.count > VT::PendingSound::depth)
            return std::unexpected(HandoverBodyError::Malformed);
        for (unsigned index = 0; index < section.count; ++index) {
            auto& entry = section.entries[index];
            soundFields([&](auto& value) { readScalar(reader, value); }, entry.request);
            entry.serial = reader.u32();
            if (!reader.ok() || !isValidSoundRequest(entry.request, entry.serial))
                return std::unexpected(HandoverBodyError::Malformed);
            const auto pcm = readPCM(entry.request.sampleFrames);
            if (!pcm)
                return std::unexpected(pcm.error());
            if (pcm->size() > VT::PendingSound::maxPCMBytes - section.pendingPCMBytes)
                return std::unexpected(HandoverBodyError::OverCapacity);
            section.pendingPCMBytes += pcm->size();
            entry.pcm = *pcm;
        }
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(HandoverBodyError::Malformed);
    return section;
}

std::expected<VisualSection, HandoverBodyError> readVisualStores(std::span<const std::uint8_t> bytes, const HandoverVisualLimits& limits)
{
    const auto covered = verifiedSection(bytes, limits.maxSectionBytes);
    if (!covered)
        return std::unexpected(covered.error());
    WireReader reader(*covered);
    (void)reader.u16();
    bool hasFont = false;
    readScalar(reader, hasFont);
    VisualSection section;
    if (hasFont) {
        section.font.emplace();
        softFontFields([&](auto& value) { readScalar(reader, value); }, *section.font);
        if (!isValidSoftFont(*section.font))
            return std::unexpected(HandoverBodyError::Malformed);
    }
    tekHeaderFields([&](auto& value, const auto&) { readScalar(reader, value); }, section.tek, section.tek);
    if (!reader.ok() || !isValidTekHeader(section.tek))
        return std::unexpected(HandoverBodyError::Malformed);
    if (section.tek.primitiveCount > limits.maxTekPrimitives || section.tek.displayCount > limits.maxTekPrimitives)
        return std::unexpected(HandoverBodyError::OverCapacity);
    const auto count = static_cast<std::size_t>(section.tek.primitiveCount + section.tek.displayCount);
    if (count > reader.remaining() / tekPrimitiveBytes || reader.remaining() != count * tekPrimitiveBytes)
        return std::unexpected(HandoverBodyError::Malformed);
    section.primitiveBytes = covered->subspan(reader.offset());
    for (std::size_t index = 0; index < count; ++index) {
        Tek::Primitive primitive {};
        tekPrimitiveFields([&](auto& value) { readScalar(reader, value); }, primitive);
        if (!isValidTekPrimitive(primitive))
            return std::unexpected(HandoverBodyError::Malformed);
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(HandoverBodyError::Malformed);
    return section;
}

template<typename Field, typename Image>
void imageFields(Field&& field, Image& image)
{
    field(image.displayed);
    field(image.imageID);
    field(image.imageNumber);
    field(image.width);
    field(image.height);
    field(image.format);
    field(image.frameCount);
    field(image.currentFrame);
    field(image.frameClockMilliseconds);
    field(image.animationState);
    field(image.lastUsedSeq);
    field(image.contentGeneration);
    field(image.virtualPlacement);
    field(image.virtualColumns);
    field(image.virtualRows);
    field(image.virtualPlacementID);
}

template<typename Field, typename Placement>
void placementFields(Field&& field, Placement& placement)
{
    field(placement.imageID);
    field(placement.placementID);
    field(placement.hasPlacementID);
    field(placement.cursorX);
    field(placement.cursorY);
    field(placement.cellColumns);
    field(placement.cellRows);
    field(placement.pixelOffsetX);
    field(placement.pixelOffsetY);
    field(placement.sourceX);
    field(placement.sourceY);
    field(placement.sourceWidth);
    field(placement.sourceHeight);
    field(placement.drawnPixelWidth);
    field(placement.drawnPixelHeight);
    field(placement.zIndex);
    field(placement.cursorPolicy);
    field(placement.onAlternateScreen);
}

// Pixel views borrow the immutable section during validation and materialization;
// only `stored` owns pixels. Frame zero is installed as an alias exactly once.
struct ImageSection {
    std::uint32_t nextImageID = 0;
    std::uint32_t livePlacementCount = 0;
    std::uint64_t pixelBudget = 0;
    std::uint64_t useSequence = 0;
    std::uint64_t contentSequence = 0;
    std::array<Image::TerminalImage, Image::maxTerminalImageCount> stored {};
    std::array<Image::TerminalImagePlacement, Image::maxTerminalImagePlacementCount> placements {};
    std::array<std::array<std::span<const std::uint8_t>, Image::maxTerminalImageFrameCount>, Image::maxTerminalImageCount> pixels {};

    ~ImageSection()
    {
        for (auto& image : stored)
            Image::dispose(&image);
    }
};

template<typename Images>
std::expected<void, HandoverBodyError> validateImages(const Images& images, std::uint64_t budget, std::uint64_t useSequence,
    std::uint64_t contentSequence, std::uint32_t placementCount, const HandoverImageLimits& limits)
{
    if (budget > limits.maxPixelBytes)
        return std::unexpected(HandoverBodyError::OverCapacity);
    std::uint64_t total = 0;
    std::uint32_t countedPlacements = 0;
    for (std::size_t index = 0; index < images.stored.size(); ++index) {
        const auto& image = images.stored[index];
        if (!image.used)
            continue;
        if (!image.imageID || Image::isDecodedImageSizeRefused(image.width, image.height)
            || (image.format != 24 && image.format != 32)
            || !image.frameCount || image.frameCount > Image::maxTerminalImageFrameCount
            || image.currentFrame >= image.frameCount || image.animationState > Image::AnimationState::Looping
            || image.lastUsedSeq > useSequence || image.contentGeneration > contentSequence)
            return std::unexpected(HandoverBodyError::Malformed);
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (images.stored[prior].used && images.stored[prior].imageID == image.imageID)
                return std::unexpected(HandoverBodyError::Malformed);
        }
        const auto frameBytes = static_cast<std::uint64_t>(image.width) * image.height * (image.format / 8);
        if (frameBytes > Image::maxDecodedImageBytes / image.frameCount)
            return std::unexpected(HandoverBodyError::OverCapacity);
        const auto imageBytes = frameBytes * image.frameCount;
        if (imageBytes > budget - total)
            return std::unexpected(HandoverBodyError::OverCapacity);
        total += imageBytes;
        if (image.pixelLength != frameBytes)
            return std::unexpected(HandoverBodyError::Malformed);
        for (std::size_t frame = 0; frame < image.frames.size(); ++frame) {
            if (image.frames[frame].used != (frame < image.frameCount)
                || (frame < image.frameCount && image.frames[frame].pixelLength != frameBytes))
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    for (const auto& placement : images.placements) {
        if (!placement.used)
            continue;
        ++countedPlacements;
        const auto found = std::ranges::find_if(images.stored, [&](const auto& image) { return image.used && image.imageID == placement.imageID; });
        if (found == images.stored.end() || placement.cursorPolicy > 1 || placement.onAlternateScreen > 1
            || (placement.drawnPixelWidth == 0) != (placement.drawnPixelHeight == 0)
            || placement.sourceX >= found->width || placement.sourceY >= found->height
            || placement.sourceWidth > found->width - placement.sourceX || placement.sourceHeight > found->height - placement.sourceY)
            return std::unexpected(HandoverBodyError::Malformed);
    }
    if (countedPlacements != placementCount)
        return std::unexpected(HandoverBodyError::Malformed);
    return {};
}

void writeImages(SectionWriter& writer, const Terminal::Images& images)
{
    const auto field = [&](const auto& value) { writer.scalar(value); };
    writer.scalar(handoverBodyVersion);
    writer.scalar(images.kittyNextImageID);
    writer.scalar(images.livePlacementCount);
    writer.scalar(images.kittyImageBytesBudget);
    writer.scalar(images.kittyImageUseSeq);
    writer.scalar(images.kittyImageContentSeq);
    for (const auto& image : images.stored) {
        writer.scalar(image.used);
        if (!image.used)
            continue;
        imageFields(field, image);
        for (std::size_t i = 0; i < image.frameCount; ++i) {
            const auto& frame = image.frames[i];
            writer.scalar(frame.gapless);
            writer.scalar(frame.delayMilliseconds);
            writer.scalar(frame.pixelLength);
            writer.raw({ frame.pixels, frame.pixelLength });
        }
    }
    for (const auto& placement : images.placements) {
        writer.scalar(placement.used);
        if (placement.used)
            placementFields(field, placement);
    }
}

std::expected<void, HandoverBodyError> readImages(std::span<const std::uint8_t> bytes, ImageSection& section, const HandoverImageLimits& limits)
{
    if (bytes.size() > limits.maxSectionBytes)
        return std::unexpected(HandoverBodyError::OverCapacity);
    WireReader reader(bytes);
    const auto version = reader.u16();
    if (reader.ok() && version > handoverBodyVersion)
        return std::unexpected(HandoverBodyError::NewerVersion);
    if (!reader.ok() || version < oldestHandoverBodyVersion || bytes.size() < 16)
        return std::unexpected(HandoverBodyError::Malformed);
    const auto covered = bytes.first(bytes.size() - 16);
    WireReader digestReader(bytes.last(16));
    const Digest128 expected { digestReader.u64(), digestReader.u64() };
    if (digestOf(covered) != expected)
        return std::unexpected(HandoverBodyError::DigestMismatch);
    const auto field = [&](auto& value) { readScalar(reader, value); };
    field(section.nextImageID);
    field(section.livePlacementCount);
    field(section.pixelBudget);
    field(section.useSequence);
    field(section.contentSequence);
    for (std::size_t index = 0; index < section.stored.size(); ++index) {
        auto& image = section.stored[index];
        field(image.used);
        if (!image.used)
            continue;
        imageFields(field, image);
        if (!reader.ok() || !image.frameCount || image.frameCount > Image::maxTerminalImageFrameCount)
            return std::unexpected(HandoverBodyError::Malformed);
        for (std::size_t i = 0; i < image.frameCount; ++i) {
            auto& frame = image.frames[i];
            frame.used = true;
            field(frame.gapless);
            field(frame.delayMilliseconds);
            field(frame.pixelLength);
            section.pixels[index][i] = reader.take(frame.pixelLength);
        }
        image.pixelLength = image.frames[0].pixelLength;
    }
    for (auto& placement : section.placements) {
        field(placement.used);
        if (placement.used)
            placementFields(field, placement);
    }
    if (!reader.ok() || reader.offset() != covered.size())
        return std::unexpected(HandoverBodyError::Malformed);
    return validateImages(section, section.pixelBudget, section.useSequence, section.contentSequence, section.livePlacementCount, limits);
}

} // namespace

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverImages(const Terminal& terminal, const HandoverImageLimits& limits)
{
    const auto blockers = terminalHandoverBlockers(terminal);
    if (blockers.contains(HandoverBlocker::ParserMidSequence) || blockers.contains(HandoverBlocker::ImageTransferOpen) || blockers.contains(HandoverBlocker::ImageDecodePending))
        return std::unexpected(HandoverBodyError::NotAtGround);
    const auto& images = terminal.images;
    const auto validation = validateImages(images, images.kittyImageBytesBudget, images.kittyImageUseSeq, images.kittyImageContentSeq, images.livePlacementCount, limits);
    if (!validation)
        return std::unexpected(validation.error());
    for (const auto& image : images.stored) {
        if (!image.used)
            continue;
        if (!image.pixels || image.frames[0].pixels != image.pixels)
            return std::unexpected(HandoverBodyError::Malformed);
        for (std::size_t frame = 0; frame < image.frameCount; ++frame) {
            if (!image.frames[frame].pixels)
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    return encodeSection(limits.maxSectionBytes, [&](SectionWriter& writer) { writeImages(writer, images); });
}

std::expected<void, HandoverBodyError> restoreHandoverImages(std::span<const std::uint8_t> bytes, Terminal& terminal, const HandoverImageLimits& limits)
{
    const auto blockers = terminalHandoverBlockers(terminal);
    if (blockers.contains(HandoverBlocker::ParserMidSequence) || blockers.contains(HandoverBlocker::ImageTransferOpen) || blockers.contains(HandoverBlocker::ImageDecodePending))
        return std::unexpected(HandoverBodyError::NotAtGround);
    ImageSection section;
    const auto validation = readImages(bytes, section, limits);
    if (!validation)
        return validation;
    for (std::size_t index = 0; index < section.stored.size(); ++index) {
        auto& image = section.stored[index];
        if (!image.used)
            continue;
        for (std::size_t frame = 0; frame < image.frameCount; ++frame) {
            const auto source = section.pixels[index][frame];
            Image::OwnedPixels pixels(static_cast<unsigned char*>(std::malloc(source.size())));
            if (!pixels)
                return std::unexpected(HandoverBodyError::AllocationRefused);
            std::memcpy(pixels.get(), source.data(), source.size());
            image.frames[frame].pixels = pixels.release();
            if (!frame)
                image.pixels = image.frames[frame].pixels;
        }
    }
    Image::disposeAllImages(terminal);
    auto& images = terminal.images;
    images.stored = std::exchange(section.stored, {});
    images.placements = section.placements;
    images.kittyNextImageID = section.nextImageID;
    images.livePlacementCount = section.livePlacementCount;
    images.kittyImageBytesBudget = section.pixelBudget;
    images.kittyImageUseSeq = section.useSequence;
    images.kittyImageContentSeq = section.contentSequence;
    return {};
}

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverHistory(const Terminal& terminal, const HandoverHistoryLimits& limits)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    HistorySection section;
    section.epoch = terminal.historyEpoch;
    section.epochBase = terminal.historyEpochBase;
    section.configuredRows = static_cast<std::uint32_t>(terminal.scrollbackLimit);
    section.capacity = static_cast<std::uint32_t>(terminal.scrollbackCapacity);
    section.rows = static_cast<std::uint32_t>(terminal.scrollbackCount);
    section.hasStore = terminal.lineStore != nullptr;
    if (section.hasStore) {
        if (terminal.scrollbackCapacity != terminal.lineStore->capacity() || terminal.scrollbackCount != terminal.lineStore->count())
            return std::unexpected(HandoverBodyError::Malformed);
        section.continuation = terminal.lineStore->continuation();
    }
    const auto validation = validateHistory(section, limits);
    if (!validation)
        return std::unexpected(validation.error());
    std::uint64_t completed = 0;
    for (std::uint32_t index = 0; index < section.rows; ++index) {
        const auto* row = terminal.lineStore->at(static_cast<int>(index));
        if (!row || row->length < 0 || row->length > maxColumns || (row->length && !row->cells))
            return std::unexpected(HandoverBodyError::Malformed);
        const auto length = static_cast<std::uint32_t>(row->length);
        if (length > limits.maxCells - section.cells)
            return std::unexpected(HandoverBodyError::OverCapacity);
        section.cells += length;
        completed += !row->wrapped;
    }
    if (section.hasStore && completed != section.continuation.head - section.continuation.front.number)
        return std::unexpected(HandoverBodyError::Malformed);
    return encodeSection(limits.maxSectionBytes, [&](SectionWriter& writer) { writeHistory(writer, terminal, section); });
}

std::expected<HandoverHistoryStoragePlan, HandoverBodyError> handoverHistoryStoragePlan(std::span<const std::uint8_t> bytes,
    const Terminal& terminal, const HandoverHistoryLimits& limits)
{
    const auto section = readHistory(bytes, limits);
    if (!section)
        return std::unexpected(section.error());
    return historyStoragePlan(*section, terminal);
}

std::expected<void, HandoverBodyError> restoreHandoverHistory(std::span<const std::uint8_t> bytes, Terminal& terminal, const HandoverHistoryLimits& limits)
{
    return restoreHandoverHistory(bytes, terminal, limits, std::numeric_limits<std::size_t>::max());
}

std::expected<void, HandoverBodyError> restoreHandoverHistory(std::span<const std::uint8_t> bytes, Terminal& terminal,
    const HandoverHistoryLimits& limits, std::size_t maxPeakHistoryBytes)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    const auto section = readHistory(bytes, limits);
    if (!section)
        return std::unexpected(section.error());
    const auto plan = historyStoragePlan(*section, terminal);
    if (!plan)
        return std::unexpected(plan.error());
    if (plan->peakPrepareBytes > maxPeakHistoryBytes)
        return std::unexpected(HandoverBodyError::AllocationRefused);
    std::unique_ptr<LineStore> store;
    if (section->hasStore) {
        std::unique_ptr<Cell[]> cells;
        std::unique_ptr<StoredLine[]> rows;
        if (section->cells)
            cells.reset(new (std::nothrow) Cell[static_cast<std::size_t>(section->cells)]);
        if (section->cells && !cells)
            return std::unexpected(HandoverBodyError::AllocationRefused);
        if (section->rows)
            rows.reset(new (std::nothrow) StoredLine[section->rows]);
        if (section->rows && !rows)
            return std::unexpected(HandoverBodyError::AllocationRefused);
        WireReader reader(section->rowBytes);
        std::size_t offset = 0;
        for (std::uint32_t index = 0; index < section->rows; ++index) {
            auto& row = rows[index];
            row.length = static_cast<int>(reader.u32());
            readScalar(reader, row.wrapped);
            row.lineAttribute = reader.u8();
            row.mark = reader.u8();
            bool needsShaping = false;
            readScalar(reader, needsShaping);
            row.needsShaping = needsShaping;
            row.isCompact = 0;
            row.cells = row.length ? cells.get() + offset : nullptr;
            for (int column = 0; column < row.length; ++column)
                row.cells[column] = readHistoryCell(reader);
            offset += static_cast<std::size_t>(row.length);
        }
        if (!reader.ok() || !reader.isAtEnd() || offset != section->cells)
            return std::unexpected(HandoverBodyError::Malformed);
        auto restored = LineStore::tryRestore(static_cast<int>(section->capacity), { rows.get(), section->rows }, section->continuation, section->storage.retainedBytes);
        if (!restored)
            return std::unexpected(historyStorageError(restored.error()));
        store = std::move(*restored);
    }
    terminal.lineStore = std::move(store);
    // The restored store counts its rows afresh, so an ordinal a pin held
    // names another row now.
    Ubiquitty::Screen::forgetPositions(terminal, Ubiquitty::Screen::Buffer::Primary);
    terminal.historyEpoch = section->epoch;
    terminal.historyEpochBase = section->epochBase;
    terminal.scrollbackLimit = static_cast<int>(section->configuredRows);
    terminal.scrollbackCapacity = static_cast<int>(section->capacity);
    terminal.scrollbackCount = static_cast<int>(section->rows);
    terminal.scrollSearchState.reset();
    return {};
}

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverLinks(const Terminal& terminal, std::size_t maxBytes)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    LinkSection section;
    section.hasTable = terminal.linkTable != nullptr;
    section.current = terminal.currentLinkID;
    section.next = terminal.linkNextID;
    if (terminal.linkHead < 0 || terminal.linkHead >= VT::linkTableCapacity
        || terminal.linkCount < 0 || terminal.linkCount > VT::linkTableCapacity || terminal.linkFreeIDs.size() > 65535
        || terminal.linkReclaimBackoffMints < 0 || terminal.linkReclaimBackoffMints > 4096)
        return std::unexpected(HandoverBodyError::Malformed);
    section.head = static_cast<std::uint16_t>(terminal.linkHead);
    section.count = static_cast<std::uint16_t>(terminal.linkCount);
    section.freeCount = static_cast<std::uint32_t>(terminal.linkFreeIDs.size());
    section.reclaimBackoff = static_cast<std::uint16_t>(terminal.linkReclaimBackoffMints);
    if (!section.hasTable && section.count)
        return std::unexpected(HandoverBodyError::Malformed);
    for (std::size_t index = 0; index < section.count; ++index) {
        const auto slot = (section.head + index) % VT::linkTableCapacity;
        const auto& table = *terminal.linkTable;
        const auto stringIn = [](const auto& value) {
            return std::string_view(value.data(), static_cast<std::size_t>(std::find(value.begin(), value.end(), '\0') - value.begin()));
        };
        section.links[index] = { table.hashes[slot], table.ids[slot], stringIn(table.keys[slot]), stringIn(table.uris[slot]) };
    }
    const auto freeID = [&](std::uint32_t index) { return terminal.linkFreeIDs[index]; };
    if (!validateLinks(section, terminal, freeID))
        return std::unexpected(HandoverBodyError::Malformed);
    return encodeSection(maxBytes, [&](SectionWriter& writer) { writeLinks(writer, section, freeID); });
}

std::expected<void, HandoverBodyError> restoreHandoverLinks(std::span<const std::uint8_t> bytes, Terminal& terminal, std::size_t maxBytes)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    const auto section = readLinks(bytes, maxBytes);
    if (!section)
        return std::unexpected(section.error());
    const auto freeID = [&](std::uint32_t index) {
        const std::size_t offset = static_cast<std::size_t>(index) * 2;
        return static_cast<std::uint16_t>(section->freeBytes[offset] | (static_cast<std::uint16_t>(section->freeBytes[offset + 1]) << 8));
    };
    if (!validateLinks(*section, terminal, freeID))
        return std::unexpected(HandoverBodyError::Malformed);
    std::unique_ptr<Attributes::Hyperlinks::LinkTable> table;
    if (section->hasTable) {
        table.reset(new (std::nothrow) Attributes::Hyperlinks::LinkTable);
        if (!table)
            return std::unexpected(HandoverBodyError::AllocationRefused);
        for (std::size_t index = 0; index < section->count; ++index) {
            const auto slot = (section->head + index) % VT::linkTableCapacity;
            const auto& link = section->links[index];
            table->ids[slot] = link.id;
            table->hashes[slot] = link.hash;
            if (!link.key.empty())
                std::memcpy(table->keys[slot].data(), link.key.data(), link.key.size());
            std::memcpy(table->uris[slot].data(), link.uri.data(), link.uri.size());
        }
    }
    std::vector<std::uint16_t> freeIDs;
    freeIDs.reserve(section->freeCount);
    for (std::uint32_t index = 0; index < section->freeCount; ++index)
        freeIDs.push_back(freeID(index));
    terminal.linkTable = std::move(table);
    terminal.linkHead = section->head;
    terminal.linkCount = section->count;
    terminal.currentLinkID = section->current;
    terminal.linkNextID = section->next;
    terminal.linkFreeIDs = std::move(freeIDs);
    terminal.linkReclaimBackoffMints = section->reclaimBackoff;
    return {};
}

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverTextStores(const Terminal& terminal, const HandoverTextStoreLimits& limits)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    const auto& clusters = terminal.clusterPool;
    const auto& runs = terminal.runTextStore;
    if (clusters.entries().size() > limits.maxClusterEntries || runs.slots().size() > limits.maxRunSlots)
        return std::unexpected(HandoverBodyError::OverCapacity);
    const auto continuation = runs.continuation();
    if (!clusters.continuation().fits(clusters.entries().size()) || terminal.runTextReclaimBackoffInterns < 0
        || terminal.runTextReclaimBackoffInterns > 63 || terminal.textSizeReclaimBackoffInterns < 0 || terminal.textSizeReclaimBackoffInterns > 64)
        return std::unexpected(HandoverBodyError::Malformed);
    std::uint64_t codePoints = runs.liveCodePoints();
    for (const auto& entry : clusters.entries()) {
        if (!entry.length || entry.length > ClusterPool::maxCodePoints)
            return std::unexpected(HandoverBodyError::Malformed);
        codePoints += entry.length;
    }
    const auto encodedBytes = 16u + clusters.entries().size() + 4u * runs.slots().size() + 4u * codePoints + 2u * continuation.freeIDs.size() + 2u + 16u;
    if (codePoints > limits.maxCodePoints || encodedBytes > limits.maxSectionBytes)
        return std::unexpected(HandoverBodyError::OverCapacity);
    for (std::size_t index = 0; index < clusters.entries().size(); ++index) {
        const auto& entry = clusters.entries()[index];
        if (entry.id != index + 1)
            return std::unexpected(HandoverBodyError::Malformed);
        for (std::size_t column = 0; column < entry.length; ++column) {
            if (!Unicode::isScalarValue(entry.codePoints[column]))
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    for (const auto& entry : runs.slots()) {
        for (const auto codePoint : entry) {
            if (!Unicode::isScalarValue(codePoint))
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    return encodeSection(limits.maxSectionBytes, [&](SectionWriter& writer) { writeTextStores(writer, terminal); });
}

std::expected<void, HandoverBodyError> restoreHandoverTextStores(std::span<const std::uint8_t> bytes, Terminal& terminal, const HandoverTextStoreLimits& limits)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    const auto section = readTextStores(bytes, limits);
    if (!section)
        return std::unexpected(section.error());
    ClusterPool clusters;
    if (!clusters.reserve(section->clusters))
        return std::unexpected(HandoverBodyError::AllocationRefused);
    WireReader clusterReader(section->clusterBytes);
    for (std::uint32_t id = 1; id <= section->clusters; ++id) {
        const auto length = clusterReader.u8();
        std::array<std::uint32_t, ClusterPool::maxCodePoints> codePoints {};
        for (std::size_t index = 0; index < length; ++index)
            codePoints[index] = clusterReader.u32();
        if (!clusters.appendDecoded(static_cast<std::uint16_t>(id), std::span(codePoints).first(length)))
            return std::unexpected(HandoverBodyError::Malformed);
    }
    if (!clusters.restoreContinuation(section->clusterContinuation))
        return std::unexpected(HandoverBodyError::Malformed);
    RunTextStore runs;
    WireReader runReader(section->runBytes);
    for (std::uint32_t id = 1; id <= section->runSlots; ++id) {
        const auto length = runReader.u32();
        std::vector<std::uint32_t> codePoints(length);
        for (auto& codePoint : codePoints)
            codePoint = runReader.u32();
        runs.appendDecoded(static_cast<std::uint16_t>(id), codePoints);
    }
    std::vector<std::uint16_t> freeIDs(section->freeCount);
    WireReader freeReader(section->freeBytes);
    for (auto& id : freeIDs)
        id = freeReader.u16();
    if (!runs.restoreContinuation({ section->runSlots, freeIDs, section->sweepThreshold }))
        return std::unexpected(HandoverBodyError::Malformed);
    terminal.clusterPool = std::move(clusters);
    terminal.runTextStore = std::move(runs);
    terminal.runTextReclaimBackoffInterns = section->runReclaimBackoff;
    terminal.textSizeReclaimBackoffInterns = section->sizeReclaimBackoff;
    return {};
}

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverParser(const Terminal& terminal, std::size_t maxBytes)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    if (!isValidHandoverParser(terminal.parser))
        return std::unexpected(HandoverBodyError::Malformed);
    return encodeSection(maxBytes, [&](SectionWriter& writer) {
        writer.scalar(handoverBodyVersion);
        handoverParserFields([&](const auto&, const auto& value) { writer.scalar(value); }, terminal.parser, terminal.parser);
    });
}

std::expected<void, HandoverBodyError> restoreHandoverParser(std::span<const std::uint8_t> bytes, Terminal& terminal, std::size_t maxBytes)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    const auto covered = verifiedSection(bytes, maxBytes);
    if (!covered)
        return std::unexpected(covered.error());
    WireReader reader(*covered);
    (void)reader.u16();
    Terminal::Parser parser {};
    handoverParserFields([&](auto& value, const auto&) { readScalar(reader, value); }, parser, parser);
    if (!reader.ok() || !reader.isAtEnd() || !isValidHandoverParser(parser))
        return std::unexpected(HandoverBodyError::Malformed);
    handoverParserFields([](auto& destination, const auto& source) { destination = source; }, terminal.parser, parser);
    return {};
}

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverVisualStores(const Terminal& terminal, const HandoverVisualLimits& limits)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    TekHeader header;
    tekHeaderFields([](auto& destination, const auto& source) { destination = source; }, header, terminal.tek);
    if (!isValidTekHeader(header) || (terminal.softFont && !isValidSoftFont(*terminal.softFont)))
        return std::unexpected(HandoverBodyError::Malformed);
    if (header.primitiveCount > limits.maxTekPrimitives || header.displayCount > limits.maxTekPrimitives)
        return std::unexpected(HandoverBodyError::OverCapacity);
    for (const auto primitives : { std::span(terminal.tek.primitives).first(static_cast<std::size_t>(header.primitiveCount)),
             std::span(terminal.tek.displayPrimitives).first(static_cast<std::size_t>(header.displayCount)) }) {
        for (const auto& primitive : primitives) {
            if (!isValidTekPrimitive(primitive))
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    return encodeSection(limits.maxSectionBytes, [&](SectionWriter& writer) {
        writer.scalar(handoverBodyVersion);
        writer.scalar(terminal.softFont != nullptr);
        if (terminal.softFont)
            softFontFields([&](const auto& value) { writer.scalar(value); }, *terminal.softFont);
        tekHeaderFields([&](const auto&, const auto& value) { writer.scalar(value); }, header, header);
        for (const auto primitives : { std::span(terminal.tek.primitives).first(static_cast<std::size_t>(header.primitiveCount)),
                 std::span(terminal.tek.displayPrimitives).first(static_cast<std::size_t>(header.displayCount)) }) {
            for (const auto& primitive : primitives) {
                if (writer.error())
                    return;
                tekPrimitiveFields([&](const auto& value) { writer.scalar(value); }, primitive);
            }
        }
    });
}

std::expected<void, HandoverBodyError> restoreHandoverVisualStores(std::span<const std::uint8_t> bytes, Terminal& terminal, const HandoverVisualLimits& limits)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    const auto section = readVisualStores(bytes, limits);
    if (!section)
        return std::unexpected(section.error());
    std::unique_ptr<VT::SoftFont> font;
    if (section->font) {
        font.reset(new (std::nothrow) VT::SoftFont(*section->font));
        if (!font)
            return std::unexpected(HandoverBodyError::AllocationRefused);
    }
    // The fixed-capacity candidate is allocated only after every scalar and
    // list entry has passed admission, and keeps the large arrays off-stack.
    auto tek = std::unique_ptr<Tek::State>(new (std::nothrow) Tek::State {});
    if (!tek)
        return std::unexpected(HandoverBodyError::AllocationRefused);
    tekHeaderFields([](auto& destination, const auto& source) { destination = source; }, *tek, section->tek);
    WireReader reader(section->primitiveBytes);
    for (const auto primitives : { std::span(tek->primitives).first(static_cast<std::size_t>(tek->primitiveCount)),
             std::span(tek->displayPrimitives).first(static_cast<std::size_t>(tek->displayCount)) }) {
        for (auto& primitive : primitives)
            tekPrimitiveFields([&](auto& value) { readScalar(reader, value); }, primitive);
    }
    terminal.softFont = std::move(font);
    terminal.tek = *tek;
    return {};
}

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverSounds(const Terminal& terminal, const HandoverSoundLimits& limits)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    std::size_t pcmBytes = 0;
    const auto admit = [&](std::size_t frames) {
        if (frames > VT::sampleMaxFrames || frames * sizeof(std::int16_t) > limits.maxPCMBytes - pcmBytes)
            return false;
        pcmBytes += frames * sizeof(std::int16_t);
        return true;
    };
    if (terminal.sampleBank) {
        const auto& bank = *terminal.sampleBank;
        for (unsigned index = 0; index < VT::sampleSlotCount; ++index) {
            if (!admit(bank.slots[index].size()))
                return std::unexpected(HandoverBodyError::OverCapacity);
            if (bank.tags[index] && (bank.slots[index].empty() || std::find(bank.tags.begin(), bank.tags.begin() + index, bank.tags[index]) != bank.tags.begin() + index))
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    if (terminal.pendingSound) {
        const auto& pending = *terminal.pendingSound;
        if (pending.head < 0 || pending.head >= VT::PendingSound::depth || pending.count < 0 || pending.count > VT::PendingSound::depth)
            return std::unexpected(HandoverBodyError::Malformed);
        // The deferred marker is a channel-zero stop, never a hidden PCM owner.
        if (pending.resetStop.request.volume || pending.resetStop.request.channel || pending.resetStop.request.sampleFrames
            || pending.resetStop.request.samplePCM || pending.resetStop.pcm)
            return std::unexpected(HandoverBodyError::Malformed);
        std::size_t queuedBytes = 0;
        for (int index = 0; index < pending.count; ++index) {
            const auto& entry = pending.requests[(pending.head + index) % VT::PendingSound::depth];
            if (!isValidSoundRequest(entry.request, entry.sampleSerial) || entry.request.samplePCM != entry.pcm.get()
                || static_cast<bool>(entry.pcm) != (entry.request.sampleFrames != 0))
                return std::unexpected(HandoverBodyError::Malformed);
            const auto bytes = static_cast<std::size_t>(entry.request.sampleFrames) * sizeof(std::int16_t);
            if (bytes > VT::PendingSound::maxPCMBytes - queuedBytes || !admit(entry.request.sampleFrames))
                return std::unexpected(HandoverBodyError::OverCapacity);
            queuedBytes += bytes;
        }
        if (queuedBytes != pending.pcmBytes)
            return std::unexpected(HandoverBodyError::Malformed);
    }
    return encodeSection(limits.maxSectionBytes, [&](SectionWriter& writer) {
        writer.scalar(handoverBodyVersion);
        const auto writePCM = [&](std::span<const std::int16_t> pcm) {
            for (const auto frame : pcm) {
                if (writer.error())
                    return;
                writer.scalar(frame);
            }
        };
        writer.scalar(terminal.sampleBank != nullptr);
        if (terminal.sampleBank) {
            const auto& bank = *terminal.sampleBank;
            writer.scalar(bank.serial);
            for (unsigned index = 0; index < VT::sampleSlotCount && !writer.error(); ++index) {
                writer.scalar(bank.tags[index]);
                writer.scalar(static_cast<std::uint32_t>(bank.slots[index].size()));
                writePCM(bank.slots[index]);
            }
        }
        writer.scalar(terminal.pendingSound != nullptr);
        if (terminal.pendingSound) {
            const auto& pending = *terminal.pendingSound;
            writer.scalar(static_cast<std::uint8_t>(pending.head));
            writer.scalar(static_cast<std::uint8_t>(pending.count));
            writer.scalar(pending.hasResetStop);
            for (int index = 0; index < pending.count && !writer.error(); ++index) {
                const auto& entry = pending.requests[(pending.head + index) % VT::PendingSound::depth];
                soundFields([&](const auto& value) { writer.scalar(value); }, entry.request);
                writer.scalar(entry.sampleSerial);
                writePCM({ entry.pcm.get(), entry.request.sampleFrames });
            }
        }
    });
}

std::expected<void, HandoverBodyError> restoreHandoverSounds(std::span<const std::uint8_t> bytes, Terminal& terminal, const HandoverSoundLimits& limits)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    const auto section = readSounds(bytes, limits);
    if (!section)
        return std::unexpected(section.error());
    const auto readPCM = [](std::span<const std::uint8_t> bytes, std::span<std::int16_t> output) {
        WireReader reader(bytes);
        for (auto& frame : output)
            readScalar(reader, frame);
    };
    std::unique_ptr<VT::SampleBank> bank;
    if (section->hasBank) {
        bank.reset(new (std::nothrow) VT::SampleBank);
        if (!bank)
            return std::unexpected(HandoverBodyError::AllocationRefused);
        bank->serial = section->serial;
        bank->tags = section->tags;
        for (unsigned index = 0; index < VT::sampleSlotCount; ++index) {
            bank->slots[index].resize(section->slots[index].size() / sizeof(std::int16_t));
            readPCM(section->slots[index], bank->slots[index]);
        }
    }
    std::unique_ptr<VT::PendingSound> pending;
    if (section->hasPending) {
        pending.reset(new (std::nothrow) VT::PendingSound);
        if (!pending)
            return std::unexpected(HandoverBodyError::AllocationRefused);
        pending->head = section->head;
        pending->count = section->count;
        pending->pcmBytes = section->pendingPCMBytes;
        pending->hasResetStop = section->hasResetStop;
        for (unsigned index = 0; index < section->count; ++index) {
            const auto& source = section->entries[index];
            auto& entry = pending->requests[(section->head + index) % VT::PendingSound::depth];
            entry.request = source.request;
            entry.sampleSerial = source.serial;
            if (!source.pcm.empty()) {
                entry.pcm.reset(new (std::nothrow) std::int16_t[entry.request.sampleFrames]);
                if (!entry.pcm)
                    return std::unexpected(HandoverBodyError::AllocationRefused);
                readPCM(source.pcm, { entry.pcm.get(), entry.request.sampleFrames });
            }
            entry.request.samplePCM = entry.pcm.get();
        }
    }
    terminal.sampleBank = std::move(bank);
    terminal.pendingSound = std::move(pending);
    return {};
}

namespace {

// One field walk defines both directions. These are fixed-width values, not
// object-layout bytes; owning/compound fields are encoded separately below.
// A HELD field is a member Terminal holds out of line; it travels in its place
// in the walk as the inline value it replaced (HostHeld below).
#define HANDOVER_HOST_SCALARS(FIELD, HELD)   \
    FIELD(triggerWrapEvaluatedRow)           \
    FIELD(marginBellArmed)                   \
    FIELD(isScrollLaneEnabled)               \
    FIELD(isBandScrollLaneEnabled)           \
    FIELD(ambiguousWidthAnnounced)           \
    FIELD(syncOutputCloseCount)              \
    FIELD(cursorHideCount)                   \
    FIELD(sessionLogCaptureEnabled)          \
    FIELD(replyAuthority)                    \
    FIELD(allowTitleOperations)              \
    FIELD(allowFontOperations)               \
    FIELD(allowColorOperations)              \
    FIELD(allowWindowOperations)             \
    FIELD(allowTerminalCapabilityOperations) \
    FIELD(allowMouseOperations)              \
    FIELD(allowPasteControls)                \
    FIELD(passwordDetect)                    \
    FIELD(bellMode)                          \
    FIELD(drawsSizedText)                    \
    FIELD(drawsProportionalText)             \
    FIELD(currentCapabilityOrigin)           \
    FIELD(isOSC52Allowed)                    \
    FIELD(osc52ClipboardReadPolicy)          \
    FIELD(osc52ClipboardWritePolicy)         \
    FIELD(windowFocused)                     \
    FIELD(locatorRow)                        \
    FIELD(locatorColumn)                     \
    FIELD(locatorButtons)                    \
    FIELD(drawablePixelWidth)                \
    FIELD(drawablePixelHeight)               \
    FIELD(lastActivityTick)                  \
    FIELD(hasDisplayActivityTick)            \
    FIELD(displayActivityTick)               \
    FIELD(cursorHoldFrames)                  \
    FIELD(cursorBlinkPeriodMilliseconds)     \
    FIELD(cursorBlinkPref)                   \
    FIELD(cursorShapePref)                   \
    FIELD(cursorMotionModePref)              \
    FIELD(cursorMotionDurationMilliseconds)  \
    FIELD(cellPixelWidth)                    \
    FIELD(cellPixelHeight)                   \
    FIELD(textAreaPixelWidth)                \
    FIELD(textAreaPixelHeight)               \
    FIELD(screenPixelWidth)                  \
    FIELD(screenPixelHeight)                 \
    FIELD(windowIconified)                   \
    FIELD(winPosX)                           \
    FIELD(winPosY)                           \
    FIELD(fontRequestChanged)                \
    FIELD(progressGeneration)                \
    FIELD(tabStatusGeneration)               \
    FIELD(attentionGeneration)               \
    FIELD(currentDirectoryChanged)           \
    FIELD(bellPending)                       \
    FIELD(lastBellTick)                      \
    FIELD(lastCommandExitCode)               \
    FIELD(commandCompletedGeneration)        \
    FIELD(commandExitCodeRing)               \
    FIELD(commandStartedGeneration)          \
    FIELD(commandStartedAtCompletionRing)    \
    FIELD(hasCommandStartTick)               \
    FIELD(commandStartTick)                  \
    FIELD(commandDurationTicksRing)          \
    FIELD(pendingPromptRow)                  \
    FIELD(isReadingCommand)                  \
    FIELD(hasMarkedPrompt)                   \
    FIELD(commandLineGeneration)             \
    FIELD(reportingProcessGroup)             \
    FIELD(reportedHostProcessGroup)          \
    FIELD(titleOwnerProcessGroup)            \
    FIELD(foregroundProcessGroup)            \
    FIELD(progressState)                     \
    FIELD(progressValue)                     \
    FIELD(termpropProgressHint)              \
    FIELD(termpropProgressValue)             \
    FIELD(tabStatusSet)                      \
    FIELD(tabStatusR)                        \
    FIELD(tabStatusG)                        \
    FIELD(tabStatusB)                        \
    FIELD(tabStatusText)                     \
    FIELD(tabStatusTextLength)               \
    FIELD(tabColorSet)                       \
    FIELD(tabColorRed)                       \
    FIELD(tabColorGreen)                     \
    FIELD(tabColorBlue)                      \
    FIELD(hasUnseenBell)                     \
    FIELD(hasUnseenNotification)             \
    FIELD(richTextMeasurementToken)          \
    FIELD(isPasswordPromptAsserted)          \
    FIELD(isAnswerbackConcealed)             \
    FIELD(normalTextColors)                  \
    FIELD(windowFrameColors)                 \
    FIELD(lineLimit)                         \
    FIELD(implicitMovementDirection)         \
    FIELD(stringStartColumn)                 \
    HELD(applicationIdentifier)              \
    HELD(alternateTextColors)

/// A text's wire form: the live bytes, zero-filled to the capacity, then their count.
template<std::size_t Capacity>
struct HostText {
    static_assert(Capacity < 256, "the count is one byte");
    std::array<char, Capacity> bytes {};
    std::uint8_t length = 0;

    [[nodiscard]] std::string_view view() const { return { bytes.data(), length }; }
};

/// How a HELD member travels: its `Wire` form, the copy an encode takes of the
/// member, and the member a restore stages from the wire before anything is
/// applied, so a refused allocation leaves the destination untouched.
template<typename Member>
struct HostHeld;

/// The OSC 176 App ID, the one text the walk carries.
template<>
struct HostHeld<FallibleString> {
    using Wire = HostText<Terminal::applicationIdentifierCapacity>;

    static bool copy(Wire& wire, const FallibleString& held)
    {
        if (held.size() > wire.bytes.size())
            return false;
        std::ranges::copy(held.view(), wire.bytes.begin());
        wire.length = static_cast<std::uint8_t>(held.size());
        return true;
    }
    static bool stage(FallibleString& staged, const Wire& wire) { return staged.tryAssign(wire.view()).has_value(); }
};

/// DECATC's table, whole; the unassigned table while none is held.
template<>
struct HostHeld<std::unique_ptr<AlternateTextColors>> {
    using Wire = AlternateTextColors;

    static bool copy(Wire& wire, const std::unique_ptr<AlternateTextColors>& held)
    {
        wire = held ? *held : unassignedAlternateTextColors;
        return true;
    }
    static bool stage(std::unique_ptr<AlternateTextColors>& staged, const Wire& wire)
    {
        if (wire == unassignedAlternateTextColors)
            return true;
        staged.reset(new (std::nothrow) AlternateTextColors(wire));
        return staged != nullptr;
    }
};

struct HostScalars {
#define DECLARE_HOST_SCALAR(name) decltype(Terminal::name) name {};
#define DECLARE_HOST_HELD(name) HostHeld<decltype(Terminal::name)>::Wire name {};
    HANDOVER_HOST_SCALARS(DECLARE_HOST_SCALAR, DECLARE_HOST_HELD)
#undef DECLARE_HOST_HELD
#undef DECLARE_HOST_SCALAR
};

/// The HELD members as the destination will hold them.
struct HostHeldMembers {
#define IGNORE_HOST_SCALAR(name)
#define DECLARE_HOST_HELD(name) decltype(Terminal::name) name;
    HANDOVER_HOST_SCALARS(IGNORE_HOST_SCALAR, DECLARE_HOST_HELD)
#undef DECLARE_HOST_HELD
#undef IGNORE_HOST_SCALAR
};

// This joins the scalar walk with the explicit compound codecs. A new carried
// Terminal member cannot silently inherit its default in a successor.
constexpr auto handoverTerminalCoverage = std::to_array<std::string_view>({
#define NAME_HOST_SCALAR(name) #name,
    HANDOVER_HOST_SCALARS(NAME_HOST_SCALAR, NAME_HOST_SCALAR)
#undef NAME_HOST_SCALAR
        "graphicCancellation",
    "dragDropSession",
    "answerback",
    "font", "remoteHost", "commandLine", "currentDirectory", "reportedDirectory", "reportedHost", "sampledDirectory",
    "badgeText", "unseenNotificationTitle", "unseenNotificationBody", "currentFileURL", "userVariables",
    "outputBuffer", "outputLength", "bulkOutput", "sequenceGate", "typography", "triggersEngine", "triggerHighlights",
    "triggerHighlightCount", "triggerHighlightHead", "fontRequests", "integration", "extendedClipboardServedAliases",
    "lineStore", "historyEpochBase", "scrollbackLimit", "images", "linkTable", "linkCount", "linkHead", "linkNextID",
    "linkFreeIDs", "linkReclaimBackoffMints", "clusterPool", "runTextStore", "runTextReclaimBackoffInterns", "textSizeReclaimBackoffInterns",
    "softFont", "tek", "sampleBank", "pendingSound", "shellCommands", "shellCommandCount" });

static_assert([] {
    for (const auto& field : terminalHandoverLedger) {
        if (field.carriage == HandoverCarriage::Carried && std::ranges::count(handoverTerminalCoverage, field.name) != 1)
            return false;
    }
    for (const auto& field : terminalLedger) {
        if (field.carriage == Carriage::Recorded && std::ranges::count(handoverTerminalCoverage, field.name) != 1)
            return false;
    }
    return true;
}(),
    "every carried host field and recorded terminal fact requires one composition owner");

template<typename Visitor, typename State>
void hostScalars(Visitor&& visit, State& state)
{
#define VISIT_HOST_SCALAR(name) visit(state.name);
    HANDOVER_HOST_SCALARS(VISIT_HOST_SCALAR, VISIT_HOST_SCALAR)
#undef VISIT_HOST_SCALAR
}

template<typename T>
void writeHostField(SectionWriter& writer, const T& value)
{
    if constexpr (requires { value.begin(); }) {
        for (const auto& item : value)
            writeHostField(writer, item);
    } else
        writer.scalar(value);
}

template<typename T>
void readHostField(WireReader& reader, T& value)
{
    if constexpr (requires { value.begin(); }) {
        for (auto& item : value)
            readHostField(reader, item);
    } else
        readScalar(reader, value);
}

template<std::size_t Capacity>
void writeHostField(SectionWriter& writer, const HostText<Capacity>& text)
{
    writeHostField(writer, text.bytes);
    writeHostField(writer, text.length);
}

template<std::size_t Capacity>
void readHostField(WireReader& reader, HostText<Capacity>& text)
{
    readHostField(reader, text.bytes);
    readHostField(reader, text.length);
    if (text.length > Capacity)
        reader.fail();
}

constexpr auto hostStrings = std::to_array<std::pair<FallibleString Terminal::*, std::size_t>>({
    { &Terminal::answerback, Terminal::maxAnswerbackLength },
    { &Terminal::font, Terminal::maxFontLength },
    { &Terminal::remoteHost, Terminal::maxRemoteHostLength },
    { &Terminal::commandLine, Terminal::maxCommandLineLength },
    { &Terminal::currentDirectory, Terminal::maxCurrentDirectoryLength },
    { &Terminal::reportedDirectory, Terminal::maxCurrentDirectoryLength },
    { &Terminal::reportedHost, Terminal::maxRemoteHostLength },
    { &Terminal::sampledDirectory, Terminal::maxCurrentDirectoryLength },
    { &Terminal::badgeText, OSC::badgeTextCapacity },
    { &Terminal::unseenNotificationTitle, OSC::attentionTextCapacity },
    { &Terminal::unseenNotificationBody, OSC::attentionTextCapacity },
    { &Terminal::currentFileURL, Terminal::maxCurrentFileURLLength },
});

std::span<const std::uint8_t> byteView(std::string_view value)
{
    return { reinterpret_cast<const std::uint8_t*>(value.data()), value.size() };
}

void writeHostBytes(SectionWriter& writer, std::span<const std::uint8_t> bytes)
{
    writer.scalar(static_cast<std::uint32_t>(bytes.size()));
    writer.raw(bytes);
}

std::span<const std::uint8_t> readHostBytes(WireReader& reader, std::size_t limit)
{
    const auto size = reader.u32();
    if (size > limit) {
        reader.fail();
        return {};
    }
    return reader.take(size);
}

std::string_view stringView(std::span<const std::uint8_t> bytes)
{
    return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

template<typename Visit, typename Gate>
void hostGateFields(Visit&& visit, Gate& gate)
{
    visit(gate.oscAllowCount);
    visit(gate.oscAllow);
    visit(gate.oscDenyCount);
    visit(gate.oscDeny);
    visit(gate.dcsAllowCount);
    visit(gate.dcsAllow);
    visit(gate.dcsDenyCount);
    visit(gate.dcsDeny);
    visit(gate.apcAllowCount);
    visit(gate.apcAllow);
    visit(gate.apcDenyCount);
    visit(gate.apcDeny);
    visit(gate.hasOSCDenyOverflow);
    visit(gate.hasDCSDenyOverflow);
    visit(gate.hasAPCDenyOverflow);
}

bool isHostGateValid(const OSC::SequenceGate& gate)
{
    if (gate.oscAllowCount < 0 || gate.oscAllowCount > OSC::oscListCapacity
        || gate.oscDenyCount < 0 || gate.oscDenyCount > OSC::oscListCapacity
        || gate.dcsAllowCount < 0 || gate.dcsAllowCount > OSC::dcsListCapacity
        || gate.dcsDenyCount < 0 || gate.dcsDenyCount > OSC::dcsListCapacity
        || gate.apcAllowCount < 0 || gate.apcAllowCount > OSC::apcListCapacity
        || gate.apcDenyCount < 0 || gate.apcDenyCount > OSC::apcListCapacity)
        return false;
    for (int index = 0; index < gate.dcsAllowCount; ++index) {
        if (std::to_underlying(gate.dcsAllow[index]) > OSC::lastDCSKindValue)
            return false;
    }
    for (int index = 0; index < gate.dcsDenyCount; ++index) {
        if (std::to_underlying(gate.dcsDeny[index]) > OSC::lastDCSKindValue)
            return false;
    }
    for (int index = 0; index < gate.apcAllowCount; ++index) {
        if (!OSC::isAPCIntroducer(gate.apcAllow[index]))
            return false;
    }
    for (int index = 0; index < gate.apcDenyCount; ++index) {
        if (!OSC::isAPCIntroducer(gate.apcDeny[index]))
            return false;
    }
    return true;
}

template<typename Visit, typename Facts>
void typographyFields(Visit&& visit, Facts& facts)
{
    visit(facts.rendererRevision);
    visit(facts.cellWidth);
    visit(facts.cellHeight);
    visit(facts.baseline);
    visit(facts.proportionalSize);
    visit(facts.monospaceSize);
    visit(facts.rendersBlocks);
    visit(facts.shaping);
    visit(facts.fontCatalogScope);
    visit(facts.faceCount);
    for (auto& face : facts.faces) {
        visit(face.name);
        visit(face.nameLength);
        visit(face.familyClass);
        visit(face.styles);
        visit(face.ascent);
        visit(face.descent);
        visit(face.lineHeight);
        visit(face.averageAdvance);
        visit(face.advances);
    }
}

// The writer and the reader walk this one list.
template<typename Visit, typename State>
void graphicCancellationFields(Visit&& visit, State& state)
{
    visit(state.isAlternate);
    visit(state.isStatus);
    visit(state.wrapPending);
    visit(state.rows);
    visit(state.columns);
    visit(state.cursorX);
    visit(state.cursorY);
    visit(state.singleShift);
    visit(state.triggerWrapRow);
    visit(state.didAlignedTabOverflow);
    visit(state.didEmitAlignedGraphic);
    visit(state.isClusterActive);
    visit(state.clusterRow);
    visit(state.clusterColumn);
    visit(state.clusterScreen);
    visit(state.clusterLast);
    visit(state.clusterAtX);
    visit(state.clusterAtY);
    visit(state.isClusterAtWrap);
    visit(state.clusterCodePoints);
    visit(state.clusterCodepointLength);
    visit(state.clusterState.riCount);
    visit(state.clusterState.pictExtend);
    visit(state.clusterState.zwjAfterPict);
    visit(state.clusterState.prevRaw);
    visit(state.clusterState.incbChain);
    auto& previous = state.predecessor;
    visit(previous.kind);
    visit(previous.multiplier);
    visit(previous.codePoint);
    visit(previous.resolvedCodePoint);
    visit(previous.attachedSelector);
    visit(previous.controlByte);
    visit(previous.escIntermediate);
    visit(previous.drcsIntermediate);
    visit(previous.privatePrefix);
    visit(previous.intermediate);
    visit(previous.parameterCount);
    visit(previous.parameters);
    visit(previous.parameterSeparatorWasColon);
}

template<typename Visit, typename Operation>
void graphicOperationFields(Visit&& visit, Operation& operation)
{
    visit(operation.isScroll);
    visit(operation.isStatus);
    visit(operation.row);
    visit(operation.column);
    visit(operation.count);
    visit(operation.bottom);
    visit(operation.cellOffset);
    visit(operation.metaOffset);
    visit(operation.didAppendHistory);
    visit(operation.didEvictHistory);
    visit(operation.historyLength);
    visit(operation.historyOffset);
    visit(operation.historyFront);
    visit(operation.doesHistoryContinueLine);
}

void writeGraphicMetadata(SectionWriter& writer, const RowMeta& meta)
{
    writer.scalar(meta.lineAttribute);
    writer.scalar(meta.oscMark);
    writer.scalar(meta.flags.toRaw());
}

RowMeta readGraphicMetadata(WireReader& reader)
{
    RowMeta meta {};
    meta.lineAttribute = reader.u8();
    meta.oscMark = reader.u8();
    const auto flags = reader.u8();
    if (meta.lineAttribute > lineAttributeDoubleHeightBottom || flags > 15)
        reader.fail();
    meta.flags = decltype(meta.flags)::fromRaw(flags);
    return meta;
}

bool isGraphicCancellationValid(const Terminal::GraphicCancellation& state)
{
    if (!state.isValid)
        return true;
    if (state.rows < 1 || state.rows > maxRows || state.columns < 1 || state.columns > maxColumns
        || state.cursorX < 0 || state.cursorX >= state.columns || state.cursorY < 0 || state.cursorY >= state.rows
        || (state.singleShift != 0 && state.singleShift != 2 && state.singleShift != 3)
        || state.triggerWrapRow < -1 || state.triggerWrapRow >= state.rows
        || state.clusterCodepointLength < 0 || state.clusterCodepointLength > clusterPoolMaximumCodePoints
        || state.clusterScreen > 1 || state.clusterState.incbChain > 2
        || (state.isClusterActive && (state.clusterRow < 0 || state.clusterRow >= state.rows || state.clusterColumn < 0 || state.clusterColumn >= state.columns))
        || state.predecessor.kind > PrecedingItem::Kind::CSISequence || state.predecessor.parameterCount < 0
        || state.predecessor.parameterCount > VT::maxParameters || !state.predecessor.multiplier
        || state.predecessor.multiplier > PrecedingItem::maxRepeatEffects
        || state.operations.size() > 4 * maxRows + 8 || state.metadata.size() > (maxRows + 2) * (maxRows + 2)
        || state.cells.size() > 4 * (maxRows + 2) * maxColumns)
        return false;
    if (state.clusterLast > 0x10FFFF || state.predecessor.codePoint > 0x10FFFF
        || state.predecessor.resolvedCodePoint > 0x10FFFF || state.predecessor.attachedSelector > 0x10FFFF)
        return false;
    for (const auto codePoint : std::span(state.clusterCodePoints).first(static_cast<std::size_t>(state.clusterCodepointLength))) {
        if (codePoint > 0x10FFFF)
            return false;
    }
    for (const auto& operation : state.operations) {
        if (operation.row < 0 || operation.row >= state.rows || operation.column < 0 || operation.column >= state.columns
            || operation.count < 0 || operation.count > state.columns - operation.column
            || operation.cellOffset > state.cells.size() || static_cast<std::size_t>(operation.count) > state.cells.size() - operation.cellOffset
            || operation.metaOffset >= state.metadata.size() || operation.isStatus != state.isStatus
            || operation.historyLength < 0 || operation.historyLength > maxColumns
            || operation.historyOffset > state.cells.size() || static_cast<std::size_t>(operation.historyLength) > state.cells.size() - operation.historyOffset)
            return false;
        if (operation.isScroll && (operation.isStatus || operation.column != 0 || operation.count != state.columns || operation.bottom < operation.row || operation.bottom >= state.rows || static_cast<std::size_t>(operation.bottom - operation.row + 1) > state.metadata.size() - operation.metaOffset))
            return false;
        if ((operation.didAppendHistory && (!operation.isScroll || state.isAlternate || operation.row != 0 || operation.bottom != state.rows - 1))
            || (operation.didEvictHistory && !operation.didAppendHistory))
            return false;
    }
    return true;
}

void writeGraphicCancellation(SectionWriter& writer, const Terminal::GraphicCancellation& state)
{
    writer.scalar(state.isValid);
    if (!state.isValid)
        return;
    graphicCancellationFields([&](const auto& value) { writeHostField(writer, value); }, state);
    writer.scalar(static_cast<std::uint32_t>(state.cells.size()));
    for (const auto& cell : state.cells)
        writeHistoryCell(writer, cell);
    writer.scalar(static_cast<std::uint32_t>(state.metadata.size()));
    for (const auto& meta : state.metadata)
        writeGraphicMetadata(writer, meta);
    writer.scalar(static_cast<std::uint32_t>(state.operations.size()));
    for (const auto& operation : state.operations) {
        graphicOperationFields([&](const auto& value) { writeHostField(writer, value); }, operation);
        writeGraphicMetadata(writer, operation.historyMeta);
    }
}

std::expected<void, HandoverBodyError> readGraphicCancellation(WireReader& reader, Terminal::GraphicCancellation& state)
{
    readScalar(reader, state.isValid);
    if (!state.isValid || !reader.ok())
        return {};
    graphicCancellationFields([&](auto& value) { readHostField(reader, value); }, state);
    const auto cells = reader.u32();
    if (!reader.ok() || cells > 4 * (maxRows + 2) * maxColumns || cells > reader.remaining() / historyCellBytes) {
        reader.fail();
        return std::unexpected(HandoverBodyError::Malformed);
    }
    if (!state.cells.tryReserve(cells))
        return std::unexpected(HandoverBodyError::AllocationRefused);
    for (std::uint32_t index = 0; index < cells && reader.ok(); ++index)
        state.cells.push_back(readHistoryCell(reader));
    const auto metadata = reader.u32();
    if (!reader.ok() || metadata > (maxRows + 2) * (maxRows + 2) || metadata > reader.remaining() / 3) {
        reader.fail();
        return std::unexpected(HandoverBodyError::Malformed);
    }
    if (!state.metadata.tryReserve(metadata))
        return std::unexpected(HandoverBodyError::AllocationRefused);
    for (std::uint32_t index = 0; index < metadata && reader.ok(); ++index)
        state.metadata.push_back(readGraphicMetadata(reader));
    const auto operations = reader.u32();
    if (!reader.ok() || operations > 4 * maxRows + 8 || operations > reader.remaining() / 32) {
        reader.fail();
        return std::unexpected(HandoverBodyError::Malformed);
    }
    if (!state.operations.tryReserve(operations))
        return std::unexpected(HandoverBodyError::AllocationRefused);
    for (std::uint32_t index = 0; index < operations && reader.ok(); ++index) {
        auto& operation = state.operations.emplace_back();
        graphicOperationFields([&](auto& value) { readHostField(reader, value); }, operation);
        operation.historyMeta = readGraphicMetadata(reader);
    }
    state.isCapturing = false;
    if (!isGraphicCancellationValid(state))
        reader.fail();
    return {};
}

struct HostSection {
    struct Variable {
        bool used = false;
        std::string_view name;
        std::string_view value;
    };
    Terminal::GraphicCancellation graphicCancellation;
    std::optional<DragDrop::RegistrationState> dragDrop;
    HostScalars scalars;
    std::array<std::string_view, hostStrings.size()> strings;
    std::array<Variable, Terminal::userVariableCapacity> variables;
    std::span<const std::uint8_t> output;
    std::span<const std::uint8_t> bulkOutput;
    std::span<const std::uint8_t> aliases;
    OSC::SequenceGate gate;
    std::array<Terminal::TriggerHighlight, Terminal::triggerHighlightCapacity> highlights {};
    std::uint8_t highlightCount = 0;
    std::uint8_t highlightHead = 0;
    OSC::FontRequestQueue::Continuation font;
    std::uint32_t typographyEpoch = 0;
    std::uint16_t typographySubscriptionID = 0;
    bool hasTypography = false;
    RichText::TypographyFacts typography;
    bool hasTriggers = false;
    std::span<const std::uint8_t> triggers;
    bool isTmuxEnabled = false;
    bool shouldAutoDetectTmux = false;
    bool isTmuxAtLineStart = false;
    std::int32_t tmuxPane = -1;
    std::span<const std::uint8_t> trace;
    std::array<Terminal::ShellCommand, Terminal::shellCommandCapacity> shellCommands {};
    uint8_t shellCommandCount {};
};

bool isHostStateValid(const HostSection& section)
{
    if (!isGraphicCancellationValid(section.graphicCancellation))
        return false;
    if (section.shellCommandCount > section.shellCommands.size())
        return false;
    for (const auto& command : std::span(section.shellCommands).first(section.shellCommandCount)) {
        if (command.applicationIDLength > command.applicationID.size() || command.promptRow < -1
            || command.pendingPromptRow < -1 || command.role > SemanticRole::Input)
            return false;
        for (const unsigned char byte : std::span(command.applicationID).first(command.applicationIDLength)) {
            if (byte < 0x20 || byte == 0x7f)
                return false;
        }
    }
    const auto& scalar = section.scalars;
    if (scalar.lineLimit < 0 || scalar.lineLimit > maxColumns || scalar.stringStartColumn < 0 || scalar.stringStartColumn > maxColumns
        || scalar.implicitMovementDirection > VT::ImplicitMovementDirection::AgainstProgression)
        return false;
    if (scalar.replyAuthority > ReplyAuthority::Elsewhere || scalar.bellMode > BellMode::Both
        || scalar.currentCapabilityOrigin > CapabilityOrigin::Remote
        || scalar.osc52ClipboardReadPolicy > OSC::ClipboardReadPolicy::Deny
        || scalar.osc52ClipboardWritePolicy > OSC::ClipboardWritePolicy::Deny
        || scalar.cursorShapePref > 2 || scalar.cursorMotionModePref > Rendering::CursorMotionMode::Always
        || scalar.reportingProcessGroup < 0 || scalar.reportedHostProcessGroup < 0
        || scalar.titleOwnerProcessGroup < 0 || scalar.foregroundProcessGroup < 0
        || scalar.termpropProgressHint < -1 || scalar.termpropProgressHint == 0 || scalar.termpropProgressHint > 4
        || scalar.termpropProgressValue < -1 || scalar.termpropProgressValue > 100
        || scalar.progressState > 4 || scalar.progressValue > 100 || scalar.tabStatusSet > 1 || scalar.tabColorSet > 1
        || scalar.tabStatusTextLength > scalar.tabStatusText.size()
        || scalar.triggerWrapEvaluatedRow < -1 || scalar.pendingPromptRow < -1
        || !isHostGateValid(section.gate)
        || section.highlightCount > Terminal::triggerHighlightCapacity || section.highlightHead >= Terminal::triggerHighlightCapacity
        || section.font.completedGeneration != section.font.generation || section.font.queryGeneration > section.font.generation
        || !std::isfinite(section.font.actualSizePoints)
        || (section.font.actualFamily.empty() ? section.font.actualSizePoints != 0.0 : section.font.actualSizePoints <= 0.0)
        || section.font.actualFamily.size() > Terminal::maxFontLength
        || (section.hasTypography && !RichText::isAdoptable(section.typography))
        || section.tmuxPane < Tmux::invalidPaneID || section.tmuxPane > Tmux::maxTmuxPaneID
        || section.trace.size() > 32 || section.output.size() > 256
        || section.bulkOutput.size() > VT::maxBulkOutputBytes || section.aliases.size() > VT::maxExtendedClipboardAliasBytes)
        return false;
    for (std::size_t index = 0; index < hostStrings.size(); ++index) {
        if (section.strings[index].size() > hostStrings[index].second)
            return false;
    }
    for (std::size_t index = 0; index < section.variables.size(); ++index) {
        const auto& variable = section.variables[index];
        if (variable.name.size() > Terminal::maxUserVarNameLength || variable.value.size() > Terminal::maxUserVarValueLength
            || (variable.used && (variable.name.empty() || variable.value.empty())))
            return false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (variable.used && section.variables[previous].used && variable.name == section.variables[previous].name)
                return false;
        }
    }
    for (unsigned index = 0; index < section.highlightCount; ++index) {
        const auto& highlight = section.highlights[(section.highlightHead + index) % Terminal::triggerHighlightCapacity];
        if (highlight.absoluteRow < 0 || highlight.column0 < 0 || highlight.column1 < highlight.column0)
            return false;
    }
    return true;
}

void writeTriggerProgram(SectionWriter& writer, const Triggers::Program& program)
{
    writer.scalar(static_cast<std::uint8_t>(program.tokenCount));
    writer.scalar(program.caseInsensitive);
    writer.scalar(program.anchorBol);
    writer.scalar(program.isEndOfLineAnchored);
    for (int index = 0; index < program.tokenCount; ++index) {
        const auto& token = program.tokens[index];
        writer.scalar(token.op);
        writer.scalar(token.quant);
        writer.raw(token.classBitmap);
        writer.scalar(token.literal);
    }
}

Triggers::Program readTriggerProgram(WireReader& reader)
{
    Triggers::Program program {};
    program.tokenCount = reader.u8();
    readScalar(reader, program.caseInsensitive);
    readScalar(reader, program.anchorBol);
    readScalar(reader, program.isEndOfLineAnchored);
    if (program.tokenCount >= Triggers::maxPatternLength) {
        reader.fail();
        return {};
    }
    for (int index = 0; index < program.tokenCount; ++index) {
        auto& token = program.tokens[index];
        readScalar(reader, token.op);
        readScalar(reader, token.quant);
        const auto bitmap = reader.take(token.classBitmap.size());
        if (reader.ok())
            std::ranges::copy(bitmap, token.classBitmap.begin());
        readScalar(reader, token.literal);
    }
    program.tokens[program.tokenCount].op = Triggers::Program::TokenOperation::End;
    program.valid = true;
    if (!Triggers::Engine::isRestorable(program))
        reader.fail();
    return program;
}

void readHostTriggers(WireReader& reader, Triggers::Engine* candidate = nullptr)
{
    const auto ruleCount = reader.u8();
    const auto classCount = reader.u8();
    if (ruleCount > Triggers::maxRuleCount || classCount > Triggers::maxClassCount) {
        reader.fail();
        return;
    }
    for (unsigned index = 0; index < ruleCount && reader.ok(); ++index) {
        Triggers::Rule rule {};
        readScalar(reader, rule.action);
        const auto argument = readHostBytes(reader, sizeof(rule.arg) - 1);
        if (reader.ok())
            std::memcpy(rule.arg, argument.data(), argument.size());
        const auto program = readTriggerProgram(reader);
        if (rule.action > Triggers::Action::Webhook || std::ranges::find(argument, 0) != argument.end()
            || (reader.ok() && candidate && !candidate->restoreRule(rule, program)))
            reader.fail();
    }
    for (unsigned index = 0; index < classCount && reader.ok(); ++index) {
        Triggers::CharacterClass characterClass {};
        const auto name = readHostBytes(reader, sizeof(characterClass.name) - 1);
        if (reader.ok())
            std::memcpy(characterClass.name, name.data(), name.size());
        const auto program = readTriggerProgram(reader);
        if (std::ranges::find(name, 0) != name.end()
            || (reader.ok() && candidate && !candidate->restoreCharacterClass(characterClass, program)))
            reader.fail();
    }
}

void writeHostState(SectionWriter& writer, const HostSection& section, const Triggers::Engine* engine)
{
    writer.scalar(handoverBodyVersion);
    hostScalars([&](const auto& value) { writeHostField(writer, value); }, section.scalars);
    for (const auto value : section.strings)
        writeHostBytes(writer, byteView(value));
    for (const auto& variable : section.variables) {
        writer.scalar(variable.used);
        writeHostBytes(writer, byteView(variable.name));
        writeHostBytes(writer, byteView(variable.value));
    }
    writeHostBytes(writer, section.output);
    writeHostBytes(writer, section.bulkOutput);
    writeHostBytes(writer, section.aliases);
    hostGateFields([&](const auto& value) { writeHostField(writer, value); }, section.gate);
    writer.scalar(section.highlightCount);
    writer.scalar(section.highlightHead);
    for (unsigned index = 0; index < section.highlightCount; ++index) {
        const auto& highlight = section.highlights[(section.highlightHead + index) % Terminal::triggerHighlightCapacity];
        writer.scalar(highlight.absoluteRow);
        writer.scalar(highlight.column0);
        writer.scalar(highlight.column1);
    }
    writeHostBytes(writer, byteView(section.font.actualFamily));
    writer.scalar(std::bit_cast<std::uint64_t>(section.font.actualSizePoints));
    writer.scalar(section.font.generation);
    writer.scalar(section.font.completedGeneration);
    writer.scalar(section.font.queryGeneration);
    writer.scalar(section.typographyEpoch);
    writer.scalar(section.typographySubscriptionID);
    writer.scalar(section.hasTypography);
    if (section.hasTypography)
        typographyFields([&](const auto& value) { writeHostField(writer, value); }, section.typography);
    writer.scalar(section.hasTriggers);
    if (engine) {
        const auto state = engine->continuation();
        writer.scalar(static_cast<std::uint8_t>(state.rules.size()));
        writer.scalar(static_cast<std::uint8_t>(state.classes.size()));
        for (std::size_t index = 0; index < state.rules.size(); ++index) {
            writer.scalar(state.rules[index].action);
            writeHostBytes(writer, byteView(state.rules[index].arg));
            writeTriggerProgram(writer, state.rulePrograms[index]);
        }
        for (std::size_t index = 0; index < state.classes.size(); ++index) {
            writeHostBytes(writer, byteView(state.classes[index].name));
            writeTriggerProgram(writer, state.classPrograms[index]);
        }
    }
    writer.scalar(section.isTmuxEnabled);
    writer.scalar(section.shouldAutoDetectTmux);
    writer.scalar(section.isTmuxAtLineStart);
    writer.scalar(section.tmuxPane);
    writeHostBytes(writer, section.trace);
    writer.scalar(section.shellCommandCount);
    for (const auto& command : std::span(section.shellCommands).first(section.shellCommandCount)) {
        writeHostBytes(writer, byteView({ command.applicationID.data(), command.applicationIDLength }));
        writer.scalar(command.promptRow);
        writer.scalar(command.pendingPromptRow);
        writer.scalar(command.rowEpoch);
        writer.scalar(command.historyEpochBase);
        writer.scalar(command.startTick);
        writer.scalar(command.hasStartTick);
        writer.scalar(command.hasNestedCommand);
        writer.scalar(command.isReading);
        writer.scalar(command.shouldEndRoleAtLineBreak);
        writer.scalar(command.role);
    }
    writeGraphicCancellation(writer, section.graphicCancellation);
    writer.scalar(section.dragDrop.has_value());
    if (section.dragDrop) {
        const auto& drag = *section.dragDrop;
        writer.scalar(drag.acceptsDrops);
        writer.scalar(drag.isOfferingDrags);
        writer.scalar(drag.lastNativeSession);
        for (const auto identity : { drag.dropIdentity, drag.offerIdentity }) {
            writer.scalar(identity.has_value());
            writer.scalar(identity.value_or(0));
        }
        for (const auto text : { drag.acceptedMIMEs, drag.dropMachine, drag.offerMachine })
            writeHostBytes(writer, byteView(text));
    }
}

std::expected<HostSection, HandoverBodyError> readHostState(std::span<const std::uint8_t> bytes, std::size_t maxBytes)
{
    const auto covered = verifiedSection(bytes, maxBytes);
    if (!covered)
        return std::unexpected(covered.error());
    WireReader reader(*covered);
    (void)reader.u16();
    HostSection section;
    hostScalars([&](auto& value) { readHostField(reader, value); }, section.scalars);
    for (std::size_t index = 0; index < hostStrings.size(); ++index)
        section.strings[index] = stringView(readHostBytes(reader, hostStrings[index].second));
    for (auto& variable : section.variables) {
        readScalar(reader, variable.used);
        variable.name = stringView(readHostBytes(reader, Terminal::maxUserVarNameLength));
        variable.value = stringView(readHostBytes(reader, Terminal::maxUserVarValueLength));
    }
    section.output = readHostBytes(reader, 256);
    section.bulkOutput = readHostBytes(reader, VT::maxBulkOutputBytes);
    section.aliases = readHostBytes(reader, VT::maxExtendedClipboardAliasBytes);
    hostGateFields([&](auto& value) { readHostField(reader, value); }, section.gate);
    section.highlightCount = reader.u8();
    section.highlightHead = reader.u8();
    if (section.highlightCount > Terminal::triggerHighlightCapacity || section.highlightHead >= Terminal::triggerHighlightCapacity)
        return std::unexpected(HandoverBodyError::Malformed);
    for (unsigned index = 0; index < section.highlightCount; ++index) {
        auto& highlight = section.highlights[(section.highlightHead + index) % Terminal::triggerHighlightCapacity];
        readScalar(reader, highlight.absoluteRow);
        readScalar(reader, highlight.column0);
        readScalar(reader, highlight.column1);
    }
    section.font.actualFamily = stringView(readHostBytes(reader, Terminal::maxFontLength));
    section.font.actualSizePoints = std::bit_cast<double>(reader.u64());
    section.font.generation = reader.u64();
    section.font.completedGeneration = reader.u64();
    section.font.queryGeneration = reader.u64();
    section.typographyEpoch = reader.u32();
    section.typographySubscriptionID = reader.u16();
    readScalar(reader, section.hasTypography);
    if (section.hasTypography)
        typographyFields([&](auto& value) { readHostField(reader, value); }, section.typography);
    readScalar(reader, section.hasTriggers);
    if (section.hasTriggers) {
        const auto start = reader.offset();
        readHostTriggers(reader);
        if (reader.ok())
            section.triggers = covered->subspan(start, reader.offset() - start);
    }
    readScalar(reader, section.isTmuxEnabled);
    readScalar(reader, section.shouldAutoDetectTmux);
    readScalar(reader, section.isTmuxAtLineStart);
    readScalar(reader, section.tmuxPane);
    section.trace = readHostBytes(reader, 32);
    section.shellCommandCount = reader.u8();
    if (section.shellCommandCount > section.shellCommands.size())
        return std::unexpected(HandoverBodyError::Malformed);
    for (auto& command : std::span(section.shellCommands).first(section.shellCommandCount)) {
        const auto applicationID = readHostBytes(reader, command.applicationID.size());
        std::ranges::copy(applicationID, command.applicationID.begin());
        command.applicationIDLength = static_cast<uint16_t>(applicationID.size());
        readScalar(reader, command.promptRow);
        readScalar(reader, command.pendingPromptRow);
        readScalar(reader, command.rowEpoch);
        readScalar(reader, command.historyEpochBase);
        readScalar(reader, command.startTick);
        readScalar(reader, command.hasStartTick);
        readScalar(reader, command.hasNestedCommand);
        readScalar(reader, command.isReading);
        readScalar(reader, command.shouldEndRoleAtLineBreak);
        readScalar(reader, command.role);
    }
    if (auto decoded = readGraphicCancellation(reader, section.graphicCancellation); !decoded)
        return std::unexpected(decoded.error());
    bool hasDragDrop = false;
    readScalar(reader, hasDragDrop);
    if (hasDragDrop) {
        auto& drag = section.dragDrop.emplace();
        readScalar(reader, drag.acceptsDrops);
        readScalar(reader, drag.isOfferingDrags);
        drag.lastNativeSession = reader.u64();
        for (auto* identity : { &drag.dropIdentity, &drag.offerIdentity }) {
            bool hasIdentity = false;
            readScalar(reader, hasIdentity);
            const auto value = reader.u32();
            if (hasIdentity)
                *identity = value;
            else if (value)
                return std::unexpected(HandoverBodyError::Malformed);
        }
        drag.acceptedMIMEs = stringView(readHostBytes(reader, DragDrop::maxMIMEListBytes));
        drag.dropMachine = stringView(readHostBytes(reader, DragDrop::maxPacketPayloadBytes));
        drag.offerMachine = stringView(readHostBytes(reader, DragDrop::maxPacketPayloadBytes));
    }
    if (!reader.ok() || !reader.isAtEnd() || !isHostStateValid(section))
        return std::unexpected(HandoverBodyError::Malformed);
    return section;
}

struct HostBuffer {
    ByteBuffer value;
    ~HostBuffer() { value.reset(); }
};

} // namespace

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverHostState(const Terminal& terminal, std::size_t maxBytes)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    HostSection section;
    if (terminal.dragDropSession)
        section.dragDrop = terminal.dragDropSession->registrationState();
#define COPY_HOST_SCALAR(name) section.scalars.name = terminal.name;
#define COPY_HOST_HELD(name)                                                            \
    if (!HostHeld<decltype(Terminal::name)>::copy(section.scalars.name, terminal.name)) \
        return std::unexpected(HandoverBodyError::Malformed);
    HANDOVER_HOST_SCALARS(COPY_HOST_SCALAR, COPY_HOST_HELD)
#undef COPY_HOST_HELD
#undef COPY_HOST_SCALAR
    for (std::size_t index = 0; index < hostStrings.size(); ++index)
        section.strings[index] = terminal.*hostStrings[index].first;
    for (std::size_t index = 0; index < section.variables.size(); ++index) {
        const auto& variable = terminal.userVariables[index];
        section.variables[index] = { variable.used, variable.name, variable.value };
    }
    const auto bufferIsValid = [](const ByteBuffer& buffer, int limit) {
        return buffer.length >= 0 && buffer.length <= limit && buffer.length <= buffer.capacity && (!buffer.length || buffer.data);
    };
    if (terminal.outputLength < 0 || terminal.outputLength > static_cast<int>(terminal.outputBuffer.size())
        || !bufferIsValid(terminal.bulkOutput, VT::maxBulkOutputBytes)
        || !bufferIsValid(terminal.extendedClipboardServedAliases, VT::maxExtendedClipboardAliasBytes)
        || terminal.triggerHighlightCount < 0 || terminal.triggerHighlightCount > Terminal::triggerHighlightCapacity
        || terminal.triggerHighlightHead < 0 || terminal.triggerHighlightHead >= Terminal::triggerHighlightCapacity
        || terminal.integration.traceLength < 0 || terminal.integration.traceLength > 32)
        return std::unexpected(HandoverBodyError::Malformed);
    section.output = byteView({ terminal.outputBuffer.data(), static_cast<std::size_t>(terminal.outputLength) });
    section.bulkOutput = byteView({ terminal.bulkOutput.data, static_cast<std::size_t>(terminal.bulkOutput.length) });
    section.aliases = byteView({ terminal.extendedClipboardServedAliases.data, static_cast<std::size_t>(terminal.extendedClipboardServedAliases.length) });
    section.gate = terminal.sequenceGate;
    section.highlights = terminal.triggerHighlights;
    section.highlightCount = terminal.triggerHighlightCount;
    section.highlightHead = terminal.triggerHighlightHead;
    const auto font = terminal.fontRequests.continuation();
    if (!font)
        return std::unexpected(HandoverBodyError::NotAtGround);
    section.font = *font;
    section.typographyEpoch = terminal.typography.epoch;
    section.typographySubscriptionID = terminal.typography.subscriptionID;
    section.hasTypography = terminal.typography.facts != nullptr;
    if (section.hasTypography)
        section.typography = *terminal.typography.facts;
    section.hasTriggers = terminal.triggersEngine != nullptr;
    if (section.hasTriggers) {
        const auto state = terminal.triggersEngine->continuation();
        for (std::size_t index = 0; index < state.rules.size(); ++index) {
            if (state.rules[index].action > Triggers::Action::Webhook || !std::memchr(state.rules[index].arg, 0, sizeof(state.rules[index].arg))
                || !Triggers::Engine::isRestorable(state.rulePrograms[index]))
                return std::unexpected(HandoverBodyError::Malformed);
        }
        for (std::size_t index = 0; index < state.classes.size(); ++index) {
            if (!std::memchr(state.classes[index].name, 0, sizeof(state.classes[index].name))
                || !Triggers::Engine::isRestorable(state.classPrograms[index]))
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    section.isTmuxEnabled = terminal.integration.tmux.enabled;
    section.shouldAutoDetectTmux = terminal.integration.tmux.autoDetect;
    section.isTmuxAtLineStart = terminal.integration.tmux.atLineStart;
    section.tmuxPane = terminal.integration.tmux.activePaneID;
    section.trace = std::span(terminal.integration.traceBytes).first(terminal.integration.traceLength);
    if (terminal.shellCommandCount && !terminal.shellCommands)
        return std::unexpected(HandoverBodyError::Malformed);
    if (terminal.shellCommands)
        section.shellCommands = *terminal.shellCommands;
    section.shellCommandCount = terminal.shellCommandCount;
    if (terminal.graphicCancellation && terminal.graphicCancellation->isValid && terminal.parser.precedingItem.kind == PrecedingItem::Kind::Graphic) {
        const auto& source = *terminal.graphicCancellation;
        auto& destination = section.graphicCancellation;
        if (!destination.cells.tryReserve(source.cells.size()) || !destination.metadata.tryReserve(source.metadata.size())
            || !destination.operations.tryReserve(source.operations.size()))
            return std::unexpected(HandoverBodyError::AllocationRefused);
        section.graphicCancellation = *terminal.graphicCancellation;
    }
    if (!isHostStateValid(section))
        return std::unexpected(HandoverBodyError::Malformed);
    return encodeSection(maxBytes, [&](SectionWriter& writer) { writeHostState(writer, section, terminal.triggersEngine.get()); });
}

std::expected<void, HandoverBodyError> restoreHandoverHostState(std::span<const std::uint8_t> bytes, Terminal& terminal, std::size_t maxBytes)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    auto section = readHostState(bytes, maxBytes);
    if (!section)
        return std::unexpected(section.error());
    std::unique_ptr<DragDrop::Session> dragDrop;
    if (section->dragDrop) {
        dragDrop.reset(new (std::nothrow) DragDrop::Session);
        if (!dragDrop)
            return std::unexpected(HandoverBodyError::AllocationRefused);
        if (auto result = dragDrop->restoreRegistration(*section->dragDrop); !result)
            return std::unexpected(result.error() == DragDrop::Error::NoMemory ? HandoverBodyError::AllocationRefused : HandoverBodyError::Malformed);
    }
    std::array<FallibleString, hostStrings.size()> strings;
    for (std::size_t index = 0; index < strings.size(); ++index) {
        if (!strings[index].tryAssign(section->strings[index]))
            return std::unexpected(HandoverBodyError::AllocationRefused);
    }
    HostHeldMembers held;
#define IGNORE_HOST_SCALAR(name)
#define STAGE_HOST_HELD(name)                                                         \
    if (!HostHeld<decltype(Terminal::name)>::stage(held.name, section->scalars.name)) \
        return std::unexpected(HandoverBodyError::AllocationRefused);
    HANDOVER_HOST_SCALARS(IGNORE_HOST_SCALAR, STAGE_HOST_HELD)
#undef STAGE_HOST_HELD
#undef IGNORE_HOST_SCALAR
    decltype(Terminal::userVariables) variables;
    for (std::size_t index = 0; index < variables.size(); ++index) {
        const auto& source = section->variables[index];
        variables[index] = { std::string(source.name), std::string(source.value), source.used };
    }
    HostBuffer bulk;
    HostBuffer aliases;
    if ((!section->bulkOutput.empty() && !bulk.value.append(stringView(section->bulkOutput), VT::maxBulkOutputBytes))
        || (!section->aliases.empty() && !aliases.value.append(stringView(section->aliases), VT::maxExtendedClipboardAliasBytes)))
        return std::unexpected(HandoverBodyError::AllocationRefused);
    OSC::FontRequestQueue font;
    if (!font.restoreContinuation(section->font))
        return std::unexpected(HandoverBodyError::Malformed);
    std::shared_ptr<const RichText::TypographyFacts> typography;
    if (section->hasTypography) {
        auto facts = std::unique_ptr<RichText::TypographyFacts>(new (std::nothrow) RichText::TypographyFacts(section->typography));
        if (!facts)
            return std::unexpected(HandoverBodyError::AllocationRefused);
        typography = std::move(facts);
    }
    std::unique_ptr<Triggers::Engine> triggers;
    if (section->hasTriggers) {
        triggers.reset(new (std::nothrow) Triggers::Engine);
        if (!triggers)
            return std::unexpected(HandoverBodyError::AllocationRefused);
        WireReader reader(section->triggers);
        readHostTriggers(reader, triggers.get());
        if (!reader.ok() || !reader.isAtEnd())
            return std::unexpected(HandoverBodyError::Malformed);
    }
    decltype(Terminal::shellCommands) shellCommands;
    if (section->shellCommandCount) {
        shellCommands.reset(new (std::nothrow) std::array<Terminal::ShellCommand, Terminal::shellCommandCapacity>(section->shellCommands));
        if (!shellCommands)
            return std::unexpected(HandoverBodyError::AllocationRefused);
    }
    std::unique_ptr<Terminal::GraphicCancellation> graphicCancellation;
    if (section->graphicCancellation.isValid) {
        graphicCancellation.reset(new (std::nothrow) Terminal::GraphicCancellation(std::move(section->graphicCancellation)));
        if (!graphicCancellation)
            return std::unexpected(HandoverBodyError::AllocationRefused);
    }
    // No callbacks occur below; existing external bindings remain destination-owned.
    terminal.dragDropSession = std::move(dragDrop);
#define RESTORE_HOST_SCALAR(name) terminal.name = section->scalars.name;
#define RESTORE_HOST_HELD(name) terminal.name = std::move(held.name);
    HANDOVER_HOST_SCALARS(RESTORE_HOST_SCALAR, RESTORE_HOST_HELD)
#undef RESTORE_HOST_HELD
#undef RESTORE_HOST_SCALAR
#undef HANDOVER_HOST_SCALARS
    for (std::size_t index = 0; index < strings.size(); ++index)
        terminal.*hostStrings[index].first = std::move(strings[index]);
    terminal.userVariables = std::move(variables);
    terminal.shellCommands = std::move(shellCommands);
    terminal.shellCommandCount = section->shellCommandCount;
    terminal.graphicCancellation = std::move(graphicCancellation);
    std::fill(terminal.outputBuffer.begin(), terminal.outputBuffer.end(), 0);
    std::ranges::copy(section->output, terminal.outputBuffer.begin());
    terminal.outputLength = static_cast<int>(section->output.size());
    terminal.bulkOutput.reset();
    terminal.bulkOutput = std::exchange(bulk.value, {});
    terminal.extendedClipboardServedAliases.reset();
    terminal.extendedClipboardServedAliases = std::exchange(aliases.value, {});
    terminal.sequenceGate = section->gate;
    terminal.triggerHighlights = section->highlights;
    terminal.triggerHighlightCount = section->highlightCount;
    terminal.triggerHighlightHead = section->highlightHead;
    terminal.fontRequests = std::move(font);
    terminal.typography.facts = std::move(typography);
    terminal.typography.epoch = section->typographyEpoch;
    terminal.typography.subscriptionID = section->typographySubscriptionID;
    terminal.richTextLayouts = {};
    terminal.triggersEngine = std::move(triggers);
    terminal.integration.tmux.enabled = section->isTmuxEnabled;
    terminal.integration.tmux.autoDetect = section->shouldAutoDetectTmux;
    terminal.integration.tmux.atLineStart = section->isTmuxAtLineStart;
    terminal.integration.tmux.activePaneID = section->tmuxPane;
    terminal.integration.traceBytes = {};
    std::ranges::copy(section->trace, terminal.integration.traceBytes.begin());
    terminal.integration.traceLength = static_cast<int>(section->trace.size());
    terminal.integration.cachedPaneID = 0;
    terminal.integration.cachedPaneTarget = nullptr;
    return {};
}

namespace {

enum class TerminalSection : std::size_t {
    Checkpoint,
    History,
    Links,
    Text,
    Parser,
    Images,
    Visual,
    Sounds,
    Host,
    GridWidths,
    Count,
};

constexpr std::size_t terminalSectionCount = std::to_underlying(TerminalSection::Count);

constexpr std::size_t maxGridWidthBytes = 6 + 4 * maxRows + 16;

struct GridWidthSection {
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
    std::span<const std::uint8_t> widths;
};

std::expected<GridWidthSection, HandoverBodyError> readGridWidths(std::span<const std::uint8_t> bytes)
{
    const auto covered = verifiedSection(bytes, maxGridWidthBytes);
    if (!covered)
        return std::unexpected(covered.error());
    WireReader reader(*covered);
    (void)reader.u16();
    GridWidthSection result { reader.u16(), reader.u16(), {} };
    if (!result.rows || result.rows > maxRows || !result.columns || result.columns > maxColumns)
        return std::unexpected(HandoverBodyError::Malformed);
    const auto start = reader.offset();
    for (unsigned index = 0; index < 2u * result.rows; ++index) {
        if (reader.u16() > result.columns)
            return std::unexpected(HandoverBodyError::Malformed);
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(HandoverBodyError::Malformed);
    result.widths = covered->subspan(start);
    return result;
}

HandoverBodyError checkpointError(ApplyOutcome outcome)
{
    if (outcome == ApplyOutcome::AllocationRefused)
        return HandoverBodyError::AllocationRefused;
    if (outcome == ApplyOutcome::DigestMismatch)
        return HandoverBodyError::DigestMismatch;
    return HandoverBodyError::Malformed;
}

std::expected<Terminal::Parser, HandoverBodyError> admittedParser(std::span<const std::uint8_t> bytes, std::size_t limit)
{
    const auto covered = verifiedSection(bytes, limit);
    if (!covered)
        return std::unexpected(covered.error());
    WireReader reader(*covered);
    (void)reader.u16();
    Terminal::Parser parser {};
    handoverParserFields([&](auto& value, const auto&) { readScalar(reader, value); }, parser, parser);
    if (!reader.ok() || !reader.isAtEnd() || !isValidHandoverParser(parser))
        return std::unexpected(HandoverBodyError::Malformed);
    return parser;
}

bool checkpointStoresMatch(const Terminal& terminal, const TextStoreSection& text, const LinkSection& links)
{
    const auto& entries = terminal.clusterPool.entries();
    if (entries.size() != text.clusters || terminal.runTextStore.slots().size() > text.runSlots
        || terminal.linkCount != links.count || terminal.currentLinkID != links.current)
        return false;
    WireReader clusters(text.clusterBytes);
    for (const auto& entry : entries) {
        if (clusters.u8() != entry.length)
            return false;
        for (unsigned index = 0; index < entry.length; ++index) {
            if (clusters.u32() != entry.codePoints[index])
                return false;
        }
    }
    WireReader runs(text.runBytes);
    for (std::uint32_t id = 1; id <= text.runSlots; ++id) {
        const auto length = runs.u32();
        const auto held = terminal.runTextStore.text(static_cast<std::uint16_t>(id));
        if (length != held.size())
            return false;
        for (const auto codePoint : held) {
            if (runs.u32() != codePoint)
                return false;
        }
    }
    for (std::size_t index = 0; index < links.count; ++index) {
        if (!terminal.linkTable)
            return false;
        const auto slot = (terminal.linkHead + index) % VT::linkTableCapacity;
        if (terminal.linkTable->ids[slot] != links.links[index].id
            || std::string_view(terminal.linkTable->keys[slot].data()) != links.links[index].key
            || std::string_view(terminal.linkTable->uris[slot].data()) != links.links[index].uri)
            return false;
    }
    return clusters.ok() && clusters.isAtEnd() && runs.ok() && runs.isAtEnd();
}

bool isLiveParserAnchorBounded(const Terminal& terminal)
{
    const auto& parser = terminal.parser;
    const auto* screen = terminal.screens.active;
    const bool isLive = parser.clusterActive && parser.clusterScreen == (terminal.screens.isUsingAlternateScreen ? 1 : 0)
        && parser.clusterAtX == screen->cursorX && parser.clusterAtY == screen->cursorY && parser.clusterAtWrap == screen->wrapPending;
    // Stale snapshots are valid: the next character rejects them by this same
    // cursor/screen test. Only a live snapshot can append into the lead cell.
    return !isLive || (parser.clusterRow < terminal.rows && parser.clusterColumn < terminal.columns);
}

} // namespace

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverTerminal(const Terminal& terminal, std::uint64_t generation, const HandoverTerminalLimits& limits)
{
    if (!terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::NotAtGround);
    if (!generation || !terminal.richTextBlocks.nextSerial() || !isLiveParserAnchorBounded(terminal))
        return std::unexpected(HandoverBodyError::Malformed);
    if (terminal.rows <= 0 || terminal.columns <= 0
        || std::uint64_t(terminal.rows) * terminal.columns * 2 * sizeof(Cell) > limits.maxScreenCellBytes)
        return std::unexpected(HandoverBodyError::OverCapacity);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> checkpoint;
    if (!publisher.capture(terminal) || !publisher.checkpoint(terminal, generation, checkpoint, baseline))
        return std::unexpected(HandoverBodyError::Malformed);
    if (checkpoint.size() > limits.maxCheckpointBytes)
        return std::unexpected(HandoverBodyError::OverCapacity);
    std::array<HandoverBytes, terminalSectionCount> owners;
    std::array<std::span<const std::uint8_t>, terminalSectionCount> sections;
    sections[0] = checkpoint;
    std::size_t total = 2 + 4 + 4 + 4 * terminalSectionCount + 16;
    const auto charge = [&](std::size_t bytes) {
        if (total > limits.maxRecordBytes || bytes > limits.maxRecordBytes - total || bytes > UINT32_MAX)
            return false;
        total += bytes;
        return true;
    };
    if (!charge(checkpoint.size()))
        return std::unexpected(HandoverBodyError::OverCapacity);
    const auto add = [&](TerminalSection kind, std::expected<HandoverBytes, HandoverBodyError> result) -> std::expected<void, HandoverBodyError> {
        if (!result)
            return std::unexpected(result.error());
        if (!charge(result->size))
            return std::unexpected(HandoverBodyError::OverCapacity);
        auto& owner = owners[std::to_underlying(kind)];
        owner = std::move(*result);
        sections[std::to_underlying(kind)] = owner.view();
        return {};
    };
    if (auto result = add(TerminalSection::History, encodeHandoverHistory(terminal, limits.history)); !result)
        return std::unexpected(result.error());
    if (auto result = add(TerminalSection::Links, encodeHandoverLinks(terminal, limits.maxLinkBytes)); !result)
        return std::unexpected(result.error());
    if (auto result = add(TerminalSection::Text, encodeHandoverTextStores(terminal, limits.text)); !result)
        return std::unexpected(result.error());
    if (auto result = add(TerminalSection::Parser, encodeHandoverParser(terminal, limits.maxParserBytes)); !result)
        return std::unexpected(result.error());
    if (auto result = add(TerminalSection::Images, encodeHandoverImages(terminal, limits.images)); !result)
        return std::unexpected(result.error());
    if (auto result = add(TerminalSection::Visual, encodeHandoverVisualStores(terminal, limits.visual)); !result)
        return std::unexpected(result.error());
    if (auto result = add(TerminalSection::Sounds, encodeHandoverSounds(terminal, limits.sounds)); !result)
        return std::unexpected(result.error());
    if (auto result = add(TerminalSection::Host, encodeHandoverHostState(terminal, limits.maxHostStateBytes)); !result)
        return std::unexpected(result.error());
    if (auto result = add(TerminalSection::GridWidths, encodeSection(maxGridWidthBytes, [&](SectionWriter& writer) {
            writer.scalar(handoverBodyVersion);
            writer.scalar(static_cast<std::uint16_t>(terminal.rows));
            writer.scalar(static_cast<std::uint16_t>(terminal.columns));
            for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
                for (int row = 0; row < terminal.rows; ++row)
                    writer.scalar(static_cast<std::uint16_t>(terminal.grid->rowContentWidth(screen, Row(row))));
            }
        }));
        !result)
        return std::unexpected(result.error());
    return encodeSection(limits.maxRecordBytes, [&](SectionWriter& writer) {
        writer.scalar(handoverBodyVersion);
        writer.scalar(terminal.richTextBlocks.nextSerial());
        writer.scalar(static_cast<std::uint32_t>(terminal.richTextBlocks.storageLimit()));
        for (const auto section : sections)
            writeHostBytes(writer, section);
    });
}

std::expected<HandoverTerminal, HandoverBodyError> decodeHandoverTerminal(std::span<const std::uint8_t> bytes, const HandoverTerminalLimits& limits,
    std::shared_ptr<MemoryBudget> decodeMemory)
{
    const auto covered = verifiedSection(bytes, limits.maxRecordBytes);
    if (!covered)
        return std::unexpected(covered.error());
    WireReader reader(*covered);
    const auto version = reader.u16();
    const auto nextRichTextSerial = reader.u32();
    const auto richTextStorageLimit = reader.u32();
    if (richTextStorageLimit > RichText::maxRetainedBytes)
        return std::unexpected(HandoverBodyError::OverCapacity);
    const std::array<std::size_t, terminalSectionCount> sectionLimits {
        limits.maxCheckpointBytes, limits.history.maxSectionBytes, limits.maxLinkBytes, limits.text.maxSectionBytes,
        limits.maxParserBytes, limits.images.maxSectionBytes, limits.visual.maxSectionBytes, limits.sounds.maxSectionBytes, limits.maxHostStateBytes, maxGridWidthBytes
    };
    std::array<std::span<const std::uint8_t>, terminalSectionCount> sections;
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const auto size = reader.u32();
        if (size > sectionLimits[index])
            return std::unexpected(HandoverBodyError::OverCapacity);
        sections[index] = reader.take(size);
        // The checkpoint has its own wire grammar; the remaining sections
        // share this record's handover grammar, even when two are readable.
        if (index != std::to_underlying(TerminalSection::Checkpoint)) {
            WireReader sectionReader(sections[index]);
            if (sectionReader.u16() != version || !sectionReader.ok())
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    if (!reader.ok() || !reader.isAtEnd() || !nextRichTextSerial)
        return std::unexpected(HandoverBodyError::Malformed);
    const auto section = [&](TerminalSection kind) { return sections[std::to_underlying(kind)]; };
    const auto cellBytes = checkpointCellStorageBytes(sections[0]);
    if (!cellBytes)
        return std::unexpected(HandoverBodyError::Malformed);
    if (*cellBytes > limits.maxScreenCellBytes)
        return std::unexpected(HandoverBodyError::OverCapacity);
    const auto history = readHistory(section(TerminalSection::History), limits.history);
    if (!history)
        return std::unexpected(history.error());
    const auto links = readLinks(section(TerminalSection::Links), limits.maxLinkBytes);
    if (!links)
        return std::unexpected(links.error());
    const auto text = readTextStores(section(TerminalSection::Text), limits.text);
    if (!text)
        return std::unexpected(text.error());
    const auto parser = admittedParser(section(TerminalSection::Parser), limits.maxParserBytes);
    if (!parser)
        return std::unexpected(parser.error());
    {
        ImageSection images;
        if (auto result = readImages(section(TerminalSection::Images), images, limits.images); !result)
            return std::unexpected(result.error());
    }
    if (auto result = readVisualStores(section(TerminalSection::Visual), limits.visual); !result)
        return std::unexpected(result.error());
    if (auto result = readSounds(section(TerminalSection::Sounds), limits.sounds); !result)
        return std::unexpected(result.error());
    const auto host = readHostState(section(TerminalSection::Host), limits.maxHostStateBytes);
    if (!host)
        return std::unexpected(host.error());
    const auto grid = readGridWidths(section(TerminalSection::GridWidths));
    if (!grid)
        return std::unexpected(grid.error());
    // Continuation sections have validated before a destination exists. The
    // candidate stays unpublished while its checkpoint and backing are admitted.
    HandoverTerminal result { std::unique_ptr<Terminal>(new (std::nothrow) Terminal), 0, MemoryCharge(decodeMemory) };
    if (!result.terminal)
        return std::unexpected(HandoverBodyError::AllocationRefused);
    auto& terminal = *result.terminal;
    if (result.decodeCharge.budget()) {
        // The candidate is charged before anything input-sized is allocated:
        // its checkpoint preparation and grid peak, its history store's
        // planned capacity, and every other section at its carried length.
        const auto plan = checkpointStoragePlan(sections[0]);
        if (!plan)
            return std::unexpected(checkpointError(plan.error()));
        const auto cost = checkpointStorageCost(*plan, terminal);
        if (!cost)
            return std::unexpected(checkpointError(cost.error()));
        std::size_t charge = *cost;
        const auto add = [&](std::size_t bytes) {
            if (bytes > std::numeric_limits<std::size_t>::max() - charge)
                return false;
            charge += bytes;
            return true;
        };
        bool isCounted = add(history->storage.retainedBytes);
        for (std::size_t index = 2; index < sections.size(); ++index)
            isCounted = isCounted && add(sections[index].size());
        if (!isCounted || !result.decodeCharge.resize(charge))
            return std::unexpected(HandoverBodyError::AllocationRefused);
    }
    auto checkpoint = prepareCheckpoint(sections[0], terminal,
        { .maxDecodedScreenCellBytes = limits.maxScreenCellBytes, .maxStorageCost = limits.maxCheckpointStorageCost });
    if (!checkpoint)
        return std::unexpected(checkpointError(checkpoint.error()));
    const auto generation = checkpoint->generation();
    const auto historyHead = history->hasStore ? history->continuation.head : history->epochBase;
    if (!generation || checkpoint->historyEpoch() != history->epoch || checkpoint->historyHead() != historyHead
        || checkpoint->rows() != grid->rows || checkpoint->columns() != grid->columns)
        return std::unexpected(HandoverBodyError::Malformed);
    result.generation = generation;
    // A passive replica may flatten sized cells. This candidate is a host and
    // must install its carried rendering contract before checkpoint commit.
    terminal.drawsSizedText = host->scalars.drawsSizedText;
    terminal.drawsProportionalText = host->scalars.drawsProportionalText;
    ScreenReplica replica;
    if (const auto outcome = commitCheckpoint(terminal, std::move(*checkpoint), replica); outcome != ApplyOutcome::Applied)
        return std::unexpected(checkpointError(outcome));
    if (!checkpointStoresMatch(terminal, *text, *links)
        || nextRichTextSerial < terminal.richTextBlocks.nextSerial())
        return std::unexpected(HandoverBodyError::Malformed);
    terminal.richTextBlocks.resumeSerials(nextRichTextSerial);
    // The policy follows host authority, independently of the checkpoint's
    // semantic source. Exact decoded capacities may be smaller than the
    // predecessor's spare allocation, but cannot exceed its carried quota.
    if (!terminal.richTextBlocks.setStorageLimit(richTextStorageLimit))
        return std::unexpected(HandoverBodyError::Malformed);
    WireReader widths(grid->widths);
    for (const auto screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (int row = 0; row < terminal.rows; ++row) {
            if (!terminal.grid->restoreRowContentWidth(screen, Row(row), widths.u16()))
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    if (auto restored = restoreHandoverHistory(section(TerminalSection::History), terminal, limits.history); !restored)
        return std::unexpected(restored.error());
    if (auto restored = restoreHandoverLinks(section(TerminalSection::Links), terminal, limits.maxLinkBytes); !restored)
        return std::unexpected(restored.error());
    if (auto restored = restoreHandoverTextStores(section(TerminalSection::Text), terminal, limits.text); !restored)
        return std::unexpected(restored.error());
    if (auto restored = restoreHandoverImages(section(TerminalSection::Images), terminal, limits.images); !restored)
        return std::unexpected(restored.error());
    if (auto restored = restoreHandoverVisualStores(section(TerminalSection::Visual), terminal, limits.visual); !restored)
        return std::unexpected(restored.error());
    if (auto restored = restoreHandoverSounds(section(TerminalSection::Sounds), terminal, limits.sounds); !restored)
        return std::unexpected(restored.error());
    if (auto restored = restoreHandoverHostState(section(TerminalSection::Host), terminal, limits.maxHostStateBytes); !restored)
        return std::unexpected(restored.error());
    handoverParserFields([](auto& to, const auto& from) { to = from; }, terminal.parser, *parser);
    if (!isLiveParserAnchorBounded(terminal) || !terminalHandoverBlockers(terminal).isEmpty())
        return std::unexpected(HandoverBodyError::Malformed);
    return result;
}

namespace {

void writeIdentity(SectionWriter& writer, ID128 value)
{
    writer.scalar(value.low);
    writer.scalar(value.high);
}

ID128 readIdentity(WireReader& reader)
{
    return { reader.u64(), reader.u64() };
}

void writeHistoryAddress(SectionWriter& writer, const Screen::HistoryAddress& value)
{
    writer.scalar(value.epoch);
    writer.scalar(value.oldest);
    writer.scalar(value.head);
    writer.scalar(value.clearedThrough);
}

Screen::HistoryAddress readHistoryAddress(WireReader& reader)
{
    Screen::HistoryAddress value { reader.u32(), reader.u64(), reader.u64(), reader.u64() };
    if (!value.epoch || value.oldest > value.head || value.clearedThrough > value.oldest)
        reader.fail();
    return value;
}

void writePublication(SectionWriter& writer, const PaneSource::PublicationContinuation& state,
    std::span<const std::uint8_t> imageBytes)
{
    writer.scalar(handoverBodyVersion);
    writeIdentity(writer, state.paneID);
    writer.scalar(state.generation);
    writer.scalar(state.structuralGeneration);
    writer.scalar(state.changes);
    writer.scalar(state.modesRevision);
    writer.scalar(state.publishedModesHash);
    writer.scalar(state.frameClass);
    for (const auto value : state.dirtySince)
        writer.scalar(value);
    const auto& screen = state.screen;
    writer.scalar(static_cast<std::int32_t>(screen.rows));
    writer.scalar(static_cast<std::int32_t>(screen.columns));
    writer.scalar(screen.isAlternate);
    writeHistoryAddress(writer, screen.history);
    for (const auto value : screen.blockHashes)
        writer.scalar(value);
    for (const auto& rows : screen.rowHashes) {
        writer.scalar(static_cast<std::uint32_t>(rows.size()));
        for (const auto hash : rows)
            writer.scalar(hash);
    }
    const auto& dictionaries = screen.dictionaries;
    writer.scalar(dictionaries.epoch);
    writer.scalar(static_cast<std::uint32_t>(dictionaries.checkpointed.pens));
    writer.scalar(static_cast<std::uint32_t>(dictionaries.checkpointed.links));
    writer.scalar(static_cast<std::uint32_t>(dictionaries.checkpointed.clusters));
    writer.scalar(static_cast<std::uint32_t>(dictionaries.pens.size()));
    for (const auto& pen : dictionaries.pens)
        writer.raw(encodePen(pen));
    writer.scalar(static_cast<std::uint32_t>(dictionaries.links.size()));
    for (const auto& link : dictionaries.links) {
        writer.scalar(link.id);
        writeHostBytes(writer, byteView(link.key));
        writeHostBytes(writer, byteView(link.uri));
    }
    writer.scalar(static_cast<std::uint32_t>(dictionaries.clusterHashes.size()));
    for (const auto hash : dictionaries.clusterHashes)
        writer.scalar(hash);
    const auto& history = state.history;
    writeHistoryAddress(writer, history.address);
    writer.scalar(history.capturedAppends.from);
    writer.scalar(history.capturedAppends.to);
    writer.scalar(history.revision);
    writer.scalar(static_cast<std::uint32_t>(history.hashes.size()));
    for (const auto& line : history.hashes) {
        writer.scalar(line.number);
        writer.scalar(line.hash);
        writer.scalar(line.revision);
    }
    writer.scalar(static_cast<std::uint32_t>(history.capturedLinkIDs.size()));
    for (const auto id : history.capturedLinkIDs)
        writer.scalar(id);
    writeHostBytes(writer, imageBytes);
}

std::expected<void, HandoverBodyError> readPublication(std::span<const std::uint8_t> bytes,
    const HandoverPublicationLimits& limits, PaneSource::PublicationContinuation* destination)
{
    const auto verified = verifiedSection(bytes, limits.maxSectionBytes);
    if (!verified)
        return std::unexpected(verified.error());
    WireReader reader(*verified);
    (void)reader.u16();
    PaneSource::PublicationContinuation metadata;
    metadata.paneID = readIdentity(reader);
    metadata.generation = reader.u64();
    metadata.structuralGeneration = reader.u64();
    metadata.changes = reader.u64();
    metadata.modesRevision = reader.u32();
    metadata.publishedModesHash = reader.u64();
    readScalar(reader, metadata.frameClass);
    for (auto& value : metadata.dirtySince)
        value = reader.u64();
    auto& screen = metadata.screen;
    readScalar(reader, screen.rows);
    readScalar(reader, screen.columns);
    readScalar(reader, screen.isAlternate);
    screen.history = readHistoryAddress(reader);
    for (auto& value : screen.blockHashes)
        value = reader.u64();
    if (!reader.ok() || metadata.paneID == ID128 {} || !metadata.generation
        || metadata.structuralGeneration > metadata.generation || metadata.changes < metadata.generation
        || std::to_underlying(metadata.frameClass) > std::to_underlying(FrameClass::Structural)
        || screen.rows < minRows || screen.rows > maxRows || screen.columns < minColumns || screen.columns > maxColumns)
        return std::unexpected(HandoverBodyError::Malformed);
    for (auto& rows : screen.rowHashes) {
        const auto count = reader.u32();
        if (count != static_cast<std::uint32_t>(screen.rows) || count > reader.remaining() / 8)
            return std::unexpected(HandoverBodyError::Malformed);
        if (destination)
            rows.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto hash = reader.u64();
            if (destination)
                rows.push_back(hash);
        }
    }
    auto& dictionaries = screen.dictionaries;
    dictionaries.epoch = reader.u32();
    dictionaries.checkpointed = { reader.u32(), reader.u32(), reader.u32() };
    const auto pens = reader.u32();
    if (pens > limits.maxPens)
        return std::unexpected(HandoverBodyError::OverCapacity);
    if (dictionaries.checkpointed.pens > pens || pens > reader.remaining() / penWireBytes)
        return std::unexpected(HandoverBodyError::Malformed);
    if (destination)
        dictionaries.pens.reserve(pens);
    for (std::uint32_t index = 0; index < pens; ++index) {
        const auto pen = readPen(reader);
        if (destination)
            dictionaries.pens.push_back(pen);
    }
    const auto links = reader.u32();
    if (links > limits.maxLinks)
        return std::unexpected(HandoverBodyError::OverCapacity);
    if (dictionaries.checkpointed.links > links || links > reader.remaining() / 10)
        return std::unexpected(HandoverBodyError::Malformed);
    if (destination)
        dictionaries.links.reserve(links);
    for (std::uint32_t index = 0; index < links; ++index) {
        const auto id = reader.u16();
        const auto key = readHostBytes(reader, limits.maxSectionBytes);
        const auto uri = readHostBytes(reader, limits.maxSectionBytes);
        if (!id || !reader.ok())
            return std::unexpected(HandoverBodyError::Malformed);
        if (destination)
            dictionaries.links.push_back({ id, std::string(stringView(key)), std::string(stringView(uri)) });
    }
    const auto clusters = reader.u32();
    if (clusters > limits.maxClusters)
        return std::unexpected(HandoverBodyError::OverCapacity);
    if (dictionaries.checkpointed.clusters > clusters || clusters > reader.remaining() / 8)
        return std::unexpected(HandoverBodyError::Malformed);
    if (destination)
        dictionaries.clusterHashes.reserve(clusters);
    for (std::uint32_t index = 0; index < clusters; ++index) {
        const auto hash = reader.u64();
        if (destination)
            dictionaries.clusterHashes.push_back(hash);
    }
    auto& history = metadata.history;
    history.address = readHistoryAddress(reader);
    history.capturedAppends = { reader.u64(), reader.u64() };
    history.revision = reader.u64();
    const auto hashes = reader.u32();
    if (hashes > limits.maxHistoryHashes)
        return std::unexpected(HandoverBodyError::OverCapacity);
    if (hashes > reader.remaining() / 24)
        return std::unexpected(HandoverBodyError::Malformed);
    if (destination)
        history.hashes.reserve(hashes);
    for (std::uint32_t index = 0; index < hashes; ++index) {
        const HistoryPublisher::PublishedHash line { reader.u64(), reader.u64(), reader.u64() };
        if (destination)
            history.hashes.push_back(line);
    }
    const auto linkIDs = reader.u32();
    if (linkIDs > std::numeric_limits<std::uint16_t>::max() || linkIDs > reader.remaining() / 2)
        return std::unexpected(HandoverBodyError::Malformed);
    if (destination)
        history.capturedLinkIDs.reserve(linkIDs);
    for (std::uint32_t index = 0; index < linkIDs; ++index) {
        const auto id = reader.u16();
        if (!id)
            return std::unexpected(HandoverBodyError::Malformed);
        if (destination)
            history.capturedLinkIDs.push_back(id);
    }
    const auto images = decodeImagePlacements(readHostBytes(reader, limits.maxSectionBytes));
    if (!images || !reader.ok() || !reader.isAtEnd())
        return std::unexpected(HandoverBodyError::Malformed);
    metadata.images = *images;
    if (destination)
        *destination = std::move(metadata);
    return {};
}

} // namespace

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverPublication(const PaneSource& source,
    const HandoverPublicationLimits& limits)
{
    const auto state = source.publicationContinuation();
    if (!state)
        return std::unexpected(HandoverBodyError::NotAtGround);
    const auto& dictionaries = state->screen.dictionaries;
    if (dictionaries.pens.size() > limits.maxPens || dictionaries.links.size() > limits.maxLinks
        || dictionaries.clusterHashes.size() > limits.maxClusters || state->history.hashes.size() > limits.maxHistoryHashes)
        return std::unexpected(HandoverBodyError::OverCapacity);
    std::vector<std::uint8_t> images;
    encode(images, state->images);
    auto encoded = encodeSection(limits.maxSectionBytes, [&](SectionWriter& writer) { writePublication(writer, *state, images); });
    if (!encoded)
        return encoded;
    if (auto admitted = readPublication(encoded->view(), limits, nullptr); !admitted)
        return std::unexpected(admitted.error());
    return encoded;
}

std::expected<PaneSource::PublicationContinuation, HandoverBodyError> decodeHandoverPublication(
    std::span<const std::uint8_t> bytes, const HandoverPublicationLimits& limits)
{
    if (auto admitted = readPublication(bytes, limits, nullptr); !admitted)
        return std::unexpected(admitted.error());
    PaneSource::PublicationContinuation result;
    if (auto restored = readPublication(bytes, limits, &result); !restored)
        return std::unexpected(restored.error());
    return result;
}

namespace {

template<typename T, typename Visit>
void sessionFields(T& value, Visit&& visit)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Value, SessionFacts>) {
        visit(value.sessionID, value.createdUnix, value.placement, value.lastInputClientID, value.lastHumanInputClientID,
            value.activity, value.stampedTypedInputWrites, value.seenPersonFocusGains, value.lastRecordsMilliseconds,
            value.recordsOwedAtMilliseconds, value.lastActivityStampMilliseconds, value.describedStructuralGeneration,
            value.recordedActivityGeneration, value.end, value.outputOrigin, value.driverOriginFloor,
            value.isFrameDue, value.isDriverFocusRecorded, value.pendingGeometry, value.isWindowSizeDue,
            value.geometryChangesApplied, value.presentedApplied, value.presentedDueAt, value.presentedStandIn,
            value.unpublishedSince, value.lastEmittedCommandGeneration, value.lastEmittedCommandLineGeneration,
            value.lastEmittedCommandStartedGeneration, value.wasForegroundShell, value.lastForegroundName,
            value.foregroundGeneration, value.publishedEchoFacts, value.reportedProgramStatus,
            value.reportedActivityGeneration, value.paneFacts, value.didShellExit, value.exitReason,
            value.mouseTrackingProcessGroup, value.isAdopted, value.historyLinesNotCarried, value.workspaceName,
            value.shouldEndWhenDetached, value.sshHost, value.wasRestoredAfterHostRestart, value.wasKilledBySignal,
            value.exitStatusDeadlineMilliseconds, value.observedDirectory, value.graceExpiresMilliseconds,
            value.isGraceAwaitingHuman, value.landedPastePaths);
    } else if constexpr (std::is_same_v<Value, SessionFacts::Presented>)
        visit(value.owner, value.revision, value.appliedAt);
    else if constexpr (std::is_same_v<Value, SessionFacts::Title>)
        visit(value.revision, value.currentDirectory, value.processChain, value.processChainWithArguments,
            value.foregroundName, value.shellCommandName, value.ttyName);
    else if constexpr (std::is_same_v<Value, SessionFacts::Placement>)
        visit(value.surface, value.tab, value.pane);
    else if constexpr (std::is_same_v<Value, PaneGeometry>)
        visit(value.rows, value.columns);
    else if constexpr (std::is_same_v<Value, ID128>)
        visit(value.low, value.high);
    else if constexpr (std::is_same_v<Value, PaneActivityFacts>)
        visit(value.lastOutputUnixMilliseconds, value.lastInputUnixMilliseconds);
    else if constexpr (std::is_same_v<Value, SessionEndFacts>)
        visit(value.didEnd, value.hasStatus, value.wasSignalled, value.code);
    else if constexpr (std::is_same_v<Value, PaneEchoFacts>)
        visit(value.isSecureEntry, value.isAtPrompt, value.interruptCharacters, value.foregroundGeneration);
    else if constexpr (std::is_same_v<Value, InterruptCharacters>)
        visit(value.interrupt, value.quit, value.suspend);
    else if constexpr (std::is_same_v<Value, OSC::ProgramStatus>)
        visit(value.id, value.app, value.title, value.message, value.state, value.kind, value.progress);
    else if constexpr (std::is_same_v<Value, InputPolicy>)
        visit(value.echoSettleMicroseconds, value.receiptDelayMicroseconds, value.pasteWindowBytes);
    else if constexpr (std::is_same_v<Value, SettledMark>)
        visit(value.clientInstance, value.settledSeq);
    else if constexpr (std::is_same_v<Value, InputLedger::Continuation::Write>)
        visit(value.inputSeq, value.writtenAt);
    else if constexpr (std::is_same_v<Value, InputDiscardRecord>)
        visit(value.inputWritten, value.throughSeq, value.serial);
    else if constexpr (std::is_same_v<Value, GeometryArbiter::Continuation::Hand>)
        visit(value.attachmentID, value.standing, value.viewport, value.windowViewport,
            value.typedAt, value.viewportAt, value.windowViewportAt, value.heardAt);
    else if constexpr (std::is_same_v<Value, GeometryArbiter::Continuation::Override>)
        visit(value.attachmentID, value.geometry);
    else if constexpr (std::is_same_v<Value, EffectLog::ReaderContinuation>)
        visit(value.attachmentID, value.clientInstance, value.state, value.acknowledged,
            value.sent, value.lostSinceMicroseconds);
    else if constexpr (std::is_same_v<Value, RawInput>)
        visit(value.bytes);
    else if constexpr (std::is_same_v<Value, KeyInput> || std::is_same_v<Value, PointerInput>)
        visit(value.events);
    else if constexpr (std::is_same_v<Value, FocusInput>)
        visit(value.focus);
    else if constexpr (std::is_same_v<Value, PasteInput>)
        visit(value.text, value.offset, value.end);
    else if constexpr (std::is_same_v<Value, KeyEvent>)
        visit(value.key, value.modifiers, value.kind, value.text, value.unshiftedCodePoint,
            value.shiftedCodePoint, value.baseLayoutCodePoint, value.wasModified);
    else if constexpr (std::is_same_v<Value, PointerEvent>)
        visit(value.kind, value.button, value.modifiers, value.row, value.column, value.pixelX, value.pixelY);
    else if constexpr (std::is_same_v<Value, PaneInputQueue::Pending>)
        visit(value.clientInstance, value.authority, value.inputSeq, value.intent, value.isSignal, value.endedPastes);
    else if constexpr (std::is_same_v<Value, PaneInputQueue::PasteProgress>)
        visit(value.clientInstance, value.authority, value.inputSeq, value.bytesTaken, value.bytesWritten, value.hasEnded);
    else if constexpr (std::is_same_v<Value, PaneInputQueue::Passed>)
        visit(value.clientInstance, value.inputSeq, value.writtenAt);
    else if constexpr (std::is_same_v<Value, PaneInputQueue::Continuation>)
        visit(value.pending, value.pastes, value.passed, value.heldBytes, value.typedWrites, value.isProgramGone);
    else
        static_assert(sizeof(T) == 0, "Every portable owner needs an explicit field walk");
}

template<typename T>
void writePortable(SectionWriter&, const T&);

template<typename T>
void writePortable(SectionWriter& writer, const std::optional<T>& value)
{
    writer.scalar(value.has_value());
    if (value)
        writePortable(writer, *value);
}

template<typename T>
void writePortable(SectionWriter& writer, const std::vector<T>& values)
{
    writer.scalar(static_cast<std::uint32_t>(values.size()));
    if constexpr (std::is_same_v<T, std::uint8_t>)
        writer.raw(values);
    else {
        for (const auto& value : values)
            writePortable(writer, value);
    }
}

template<typename T>
void writePortable(SectionWriter& writer, const FallibleVector<T>& values)
{
    writer.scalar(static_cast<std::uint32_t>(values.size()));
    if constexpr (std::is_same_v<T, std::uint8_t>)
        writer.raw({ values.data(), values.size() });
    else {
        for (const auto& value : values)
            writePortable(writer, value);
    }
}

template<typename T>
void writePortable(SectionWriter& writer, const T& value)
{
    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)
        writer.scalar(value);
    else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, FallibleString>)
        writeHostBytes(writer, byteView(value));
    else if constexpr (std::is_same_v<T, InputIntent>) {
        writer.scalar(static_cast<std::uint8_t>(value.index()));
        std::visit([&](const auto& intent) { writePortable(writer, intent); }, value);
    } else
        sessionFields(value, [&](const auto&... members) { (writePortable(writer, members), ...); });
}

template<typename T>
void readPortable(WireReader&, T&, const HandoverPaneLimits&, std::optional<HandoverBodyError>* = nullptr);

template<typename T>
void readPortable(WireReader& reader, std::optional<T>& value, const HandoverPaneLimits& limits, std::optional<HandoverBodyError>* allocationError = nullptr)
{
    bool isPresent = false;
    readScalar(reader, isPresent);
    if (isPresent && reader.ok()) {
        value.emplace(T {});
        readPortable(reader, *value, limits, allocationError);
    } else
        value.reset();
}

template<typename T>
std::size_t maxPortableEntries(const HandoverPaneLimits& limits)
{
    return (std::is_same_v<T, std::string> || std::is_same_v<T, FallibleString>)                    ? limits.maxLandedPastePaths
        : std::is_same_v<T, OSC::ProgramStatus>                                                     ? OSC::programStatusLimit
        : std::is_same_v<T, std::uint8_t>                                                           ? maxHeldInputBytes
        : (std::is_same_v<T, KeyEvent> || std::is_same_v<T, PointerEvent>)                          ? maxHeldInputBytes / 32
        : (std::is_same_v<T, PaneInputQueue::Pending> || std::is_same_v<T, PaneInputQueue::Passed>) ? PaneInputQueue::maxContinuationIntents
                                                                                                    : maxLastWrittenEntries;
}

void failPortableStorage(WireReader& reader, StorageError error, std::optional<HandoverBodyError>* allocationError)
{
    if (allocationError)
        *allocationError = error == StorageError::OverCapacity ? HandoverBodyError::OverCapacity : HandoverBodyError::AllocationRefused;
    reader.fail();
}

template<typename T>
void readPortable(WireReader& reader, std::vector<T>& values, const HandoverPaneLimits& limits, std::optional<HandoverBodyError>* allocationError = nullptr)
{
    const auto count = reader.u32();
    if (!reader.ok() || count > maxPortableEntries<T>(limits) || count > reader.remaining()) {
        reader.fail();
        return;
    }
    if constexpr (std::is_same_v<T, std::uint8_t>) {
        const auto bytes = reader.take(count);
        values.assign(bytes.begin(), bytes.end());
        return;
    }
    values.reserve(count);
    for (std::uint32_t index = 0; index < count && reader.ok(); ++index) {
        T value;
        readPortable(reader, value, limits, allocationError);
        values.push_back(std::move(value));
    }
}

template<typename T>
void readPortable(WireReader& reader, FallibleVector<T>& values, const HandoverPaneLimits& limits, std::optional<HandoverBodyError>* allocationError = nullptr)
{
    const auto count = reader.u32();
    if (!reader.ok() || count > maxPortableEntries<T>(limits) || count > reader.remaining()) {
        reader.fail();
        return;
    }
    if constexpr (std::is_same_v<T, std::uint8_t>) {
        const auto bytes = reader.take(count);
        if (const auto copied = values.tryAssign(bytes.begin(), bytes.end()); !copied)
            failPortableStorage(reader, copied.error(), allocationError);
        return;
    }
    if (const auto reserved = values.tryReserve(count); !reserved) {
        failPortableStorage(reader, reserved.error(), allocationError);
        return;
    }
    for (std::uint32_t index = 0; index < count && reader.ok(); ++index) {
        T value;
        readPortable(reader, value, limits, allocationError);
        if (!reader.ok())
            return;
        if (const auto appended = values.tryPushBack(std::move(value)); !appended) {
            failPortableStorage(reader, appended.error(), allocationError);
            return;
        }
    }
}

template<typename T>
void readPortable(WireReader& reader, T& value, const HandoverPaneLimits& limits, std::optional<HandoverBodyError>* allocationError)
{
    if (!reader.ok())
        return;
    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)
        readScalar(reader, value);
    else if constexpr (std::is_same_v<T, std::string>) {
        const auto bytes = readHostBytes(reader, std::min<std::size_t>(4096, limits.maxSessionBytes));
        if (reader.ok())
            value.assign(stringView(bytes));
    } else if constexpr (std::is_same_v<T, FallibleString>) {
        const auto bytes = readHostBytes(reader, std::min<std::size_t>(4096, limits.maxSessionBytes));
        if (reader.ok()) {
            if (const auto copied = value.tryAssign(stringView(bytes)); !copied)
                failPortableStorage(reader, copied.error(), allocationError);
        }
    } else if constexpr (std::is_same_v<T, InputIntent>) {
        switch (reader.u8()) {
        case 0:
            value.template emplace<RawInput>();
            break;
        case 1:
            value.template emplace<KeyInput>();
            break;
        case 2:
            value.template emplace<PointerInput>();
            break;
        case 3:
            value.template emplace<FocusInput>();
            break;
        case 4:
            value.template emplace<PasteInput>();
            break;
        default:
            reader.fail();
            return;
        }
        std::visit([&](auto& intent) { readPortable(reader, intent, limits, allocationError); }, value);
    } else
        sessionFields(value, [&](auto&... members) { (readPortable(reader, members, limits, allocationError), ...); });
}

bool isSessionFactsValid(const SessionFacts& state, const HandoverPaneLimits& limits)
{
    if (!state.sessionID || std::to_underlying(state.outputOrigin) > std::to_underlying(CapabilityOrigin::Remote)
        || std::to_underlying(state.driverOriginFloor) > std::to_underlying(CapabilityOrigin::Remote)
        || (state.firstInputPrincipal && (state.firstInputPrincipal->size() > 4096 || state.firstInputPrincipal->find('\0') != std::string::npos || (state.firstInputPrincipal->empty() && !state.hasMultipleInputPrincipals)))
        || state.reportedProgramStatus.size() > OSC::programStatusLimit || state.landedPastePaths.size() > limits.maxLandedPastePaths
        || state.lastForegroundName.size() > maxProcessNameBytes || state.workspaceName.size() > 255
        || state.sshHost.size() > 255 || state.observedDirectory.size() > 4096 || state.exitReason.size() > 256
        || state.paneFacts.currentDirectory.size() > 4096 || state.paneFacts.processChain.size() > maxProcessChainBytes
        || state.paneFacts.processChainWithArguments.size() > maxProcessChainWithArgumentsBytes
        || state.paneFacts.foregroundName.size() > maxProcessNameBytes || state.paneFacts.shellCommandName.size() > maxProcessNameBytes
        || state.paneFacts.ttyName.size() > maxProcessNameBytes)
        return false;
    if (state.pendingGeometry && (state.pendingGeometry->rows < minRows || state.pendingGeometry->rows > maxRows || state.pendingGeometry->columns < minColumns || state.pendingGeometry->columns > maxColumns))
        return false;
    for (std::size_t index = 0; index < state.reportedProgramStatus.size(); ++index) {
        if (!OSC::validProgramStatus(state.reportedProgramStatus[index]))
            return false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (state.reportedProgramStatus[prior].id == state.reportedProgramStatus[index].id)
                return false;
        }
    }
    for (const auto& path : state.landedPastePaths) {
        if (path.empty() || path.size() > 4096 || path.front() != '/' || path.find('\0') != std::string::npos)
            return false;
    }
    return true;
}

} // namespace

std::expected<HandoverBytes, HandoverBodyError> encodeHandoverSessionFacts(const SessionFacts& state, const HandoverPaneLimits& limits)
{
    if (!isSessionFactsValid(state, limits))
        return std::unexpected(HandoverBodyError::Malformed);
    return encodeSection(limits.maxSessionBytes, [&](SectionWriter& writer) {
        writer.scalar(handoverBodyVersion);
        writePortable(writer, state);
        if (state.publishedEchoFacts)
            writePortable(writer, state.publishedEchoFacts->eraseCharacter);
        writePortable(writer, state.firstInputPrincipal);
        writePortable(writer, state.hasMultipleInputPrincipals);
    });
}

std::expected<SessionFacts, HandoverBodyError> decodeHandoverSessionFacts(std::span<const std::uint8_t> bytes, const HandoverPaneLimits& limits)
{
    const auto verified = verifiedSection(bytes, limits.maxSessionBytes);
    if (!verified)
        return std::unexpected(verified.error());
    WireReader reader(*verified);
    (void)reader.u16();
    SessionFacts result;
    std::optional<HandoverBodyError> allocationError;
    readPortable(reader, result, limits, &allocationError);
    if (result.publishedEchoFacts)
        readPortable(reader, result.publishedEchoFacts->eraseCharacter, limits);
    readPortable(reader, result.firstInputPrincipal, limits, &allocationError);
    readPortable(reader, result.hasMultipleInputPrincipals, limits);
    if (allocationError)
        return std::unexpected(*allocationError);
    if (!reader.ok() || !reader.isAtEnd() || !isSessionFactsValid(result, limits))
        return std::unexpected(HandoverBodyError::Malformed);
    return result;
}

namespace {

template<typename T, std::size_t Size>
void writePrefix(SectionWriter& writer, const std::array<T, Size>& values, std::size_t count)
{
    writer.scalar(static_cast<std::uint8_t>(count));
    for (std::size_t index = 0; index < count; ++index)
        writePortable(writer, values[index]);
}

template<typename T, std::size_t Size>
void readPrefix(WireReader& reader, std::array<T, Size>& values, std::uint8_t& count, const HandoverPaneLimits& limits)
{
    count = reader.u8();
    if (count > Size) {
        reader.fail();
        return;
    }
    for (unsigned index = 0; index < count && reader.ok(); ++index)
        readPortable(reader, values[index], limits);
}

void writeInputLedger(SectionWriter& writer, const InputLedger::Continuation& state)
{
    writer.scalar(state.capacity);
    writer.scalar(state.writes);
    writer.scalar(state.discardSerial);
    writer.scalar(state.entryCount);
    for (unsigned index = 0; index < state.entryCount; ++index) {
        const auto& entry = state.entries[index];
        writePortable(writer, entry.clientInstance);
        writer.scalar(entry.inputSeq);
        writer.scalar(entry.writtenAt);
        writer.scalar(entry.settledSeq);
        writer.scalar(entry.order);
        writePrefix(writer, entry.unsettled, entry.unsettledCount);
        writePrefix(writer, entry.discards, entry.discardCount);
    }
    writePrefix(writer, state.recentWriters, state.recentWriterCount);
}

void readInputLedger(WireReader& reader, InputLedger::Continuation& state, const HandoverPaneLimits& limits)
{
    state.capacity = reader.u8();
    state.writes = reader.u64();
    state.discardSerial = reader.u64();
    state.entryCount = reader.u8();
    if (state.entryCount > state.entries.size()) {
        reader.fail();
        return;
    }
    for (unsigned index = 0; index < state.entryCount && reader.ok(); ++index) {
        auto& entry = state.entries[index];
        readPortable(reader, entry.clientInstance, limits);
        entry.inputSeq = reader.u64();
        entry.writtenAt = reader.u64();
        entry.settledSeq = reader.u64();
        entry.order = reader.u64();
        readPrefix(reader, entry.unsettled, entry.unsettledCount, limits);
        readPrefix(reader, entry.discards, entry.discardCount, limits);
    }
    readPrefix(reader, state.recentWriters, state.recentWriterCount, limits);
}

void writeGeometry(SectionWriter& writer, const GeometryArbiter::Continuation& state)
{
    writer.scalar(state.policy.coalesceMicroseconds);
    writePrefix(writer, state.hands, state.handCount);
    writePortable(writer, state.pin);
    writePortable(writer, state.pinnedBy);
    writePortable(writer, state.override);
    writePortable(writer, state.applied);
    writePortable(writer, state.appliedAt);
    writer.scalar(state.placement);
}

void readGeometry(WireReader& reader, GeometryArbiter::Continuation& state, const HandoverPaneLimits& limits)
{
    state.policy.coalesceMicroseconds = reader.u64();
    readPrefix(reader, state.hands, state.handCount, limits);
    readPortable(reader, state.pin, limits);
    readPortable(reader, state.pinnedBy, limits);
    readPortable(reader, state.override, limits);
    readPortable(reader, state.applied, limits);
    readPortable(reader, state.appliedAt, limits);
    readScalar(reader, state.placement);
}

bool areEffectPayloadsValid(const EffectLog::Continuation& state, const HandoverPaneLimits& limits)
{
    if (state.recordCount > maxEffectRecords || state.readerCount > maxEffectReaders
        || state.policy.maxRecords > maxEffectRecords || state.policy.notificationsKept > maxEffectRecords
        || state.policy.maxRetainedBytes > limits.maxEffectRetainedBytes)
        return false;
    std::size_t retained = 0;
    for (std::size_t index = 0; index < state.recordCount; ++index) {
        const auto& record = state.records[index];
        if (record.payload.size() > state.policy.maxPayloadBytes
            || record.payload.size() > limits.maxEffectRetainedBytes - retained)
            return false;
        retained += record.payload.size();
        if (record.data.size() > limits.maxEffectRetainedBytes - retained)
            return false;
        retained += record.data.size();
        const auto body = decodeEffectBody(record.kind, record.payload);
        if (!body)
            return false;
        if (const auto* clipboard = std::get_if<ClipboardWriteEffect>(&*body)) {
            if (record.primary == ID128 {} || record.target != std::to_underlying(clipboard->buffer)
                || clipboard->payload.transfer != 1 || clipboard->payload.encodedBytes != record.data.size()
                || !decodeEffectClipboard(record.data, maxEffectClipboardWriteBytes))
                return false;
        } else if (const auto* sound = std::get_if<SoundEffect>(&*body)) {
            if (record.target || record.data.size() != std::size_t(sound->sample.frames) * 2)
                return false;
            if (!record.data.empty()) {
                std::array<std::uint8_t, 32> digest;
                Crypto::sha256(record.data, digest);
                if (digest != sound->sample.sha256)
                    return false;
            }
        } else if (record.target || !record.data.empty())
            return false;
    }
    return retained <= state.policy.maxRetainedBytes;
}

void writeEffects(SectionWriter& writer, const EffectLog::Continuation& state)
{
    writePortable(writer, state.paneID);
    writer.scalar(static_cast<std::uint16_t>(state.policy.maxRecords));
    writer.scalar(static_cast<std::uint64_t>(state.policy.maxPayloadBytes));
    writer.scalar(static_cast<std::uint64_t>(state.policy.maxRetainedBytes));
    writer.scalar(static_cast<std::uint16_t>(state.policy.notificationsKept));
    writer.scalar(state.policy.lostGraceMicroseconds);
    writer.scalar(state.lastSequence);
    writer.scalar(state.lastGeneration);
    writer.scalar(state.retiredThrough);
    writer.scalar(static_cast<std::uint16_t>(state.recordCount));
    for (std::size_t index = 0; index < state.recordCount; ++index) {
        const auto& record = state.records[index];
        writePortable(writer, record.paneID);
        writer.scalar(record.generation);
        writer.scalar(record.sequence);
        writePortable(writer, record.primary);
        writer.scalar(record.kind);
        writer.scalar(record.count);
        writer.scalar(record.target);
        writeHostBytes(writer, record.payload);
        writeHostBytes(writer, record.data);
    }
    writePrefix(writer, state.readers, state.readerCount);
}

void readEffects(WireReader& reader, EffectLog::Continuation& state, const HandoverPaneLimits& limits)
{
    readPortable(reader, state.paneID, limits);
    state.policy.maxRecords = reader.u16();
    const auto payloadBytes = reader.u64();
    const auto retainedBytes = reader.u64();
    if (payloadBytes > limits.maxEffectRetainedBytes || retainedBytes > limits.maxEffectRetainedBytes) {
        reader.fail();
        return;
    }
    state.policy.maxPayloadBytes = static_cast<std::size_t>(payloadBytes);
    state.policy.maxRetainedBytes = static_cast<std::size_t>(retainedBytes);
    state.policy.notificationsKept = reader.u16();
    state.policy.lostGraceMicroseconds = reader.u64();
    state.lastSequence = reader.u64();
    state.lastGeneration = reader.u64();
    state.retiredThrough = reader.u64();
    state.recordCount = reader.u16();
    if (state.recordCount > state.records.size()) {
        reader.fail();
        return;
    }
    for (std::size_t index = 0; index < state.recordCount && reader.ok(); ++index) {
        auto& record = state.records[index];
        readPortable(reader, record.paneID, limits);
        record.generation = reader.u64();
        record.sequence = reader.u64();
        readPortable(reader, record.primary, limits);
        readScalar(reader, record.kind);
        record.count = reader.u32();
        record.target = reader.u8();
        record.payload = readHostBytes(reader, state.policy.maxPayloadBytes);
        record.data = readHostBytes(reader, state.policy.maxRetainedBytes);
    }
    std::uint8_t readerCount = 0;
    readPrefix(reader, state.readers, readerCount, limits);
    state.readerCount = readerCount;
    if (reader.ok() && !areEffectPayloadsValid(state, limits))
        reader.fail();
}

std::expected<HandoverBytes, HandoverBodyError> encodeSource(const PaneSource::Continuation& state, const HandoverPaneLimits& limits)
{
    if (!areEffectPayloadsValid(state.effects, limits))
        return std::unexpected(HandoverBodyError::Malformed);
    std::vector<std::uint8_t> facts;
    encodePaneFacts(facts, state.facts);
    if (facts.size() > maxFrameBodyBytes || decodePaneFacts(facts) != state.facts)
        return std::unexpected(HandoverBodyError::Malformed);
    return encodeSection(limits.maxSourceBytes, [&](SectionWriter& writer) {
        writer.scalar(handoverBodyVersion);
        writeInputLedger(writer, state.input);
        writeGeometry(writer, state.geometry);
        writePortable(writer, state.queue);
        writePortable(writer, state.inputPolicy);
        writer.scalar(state.inputAwaitedGeneration);
        writePortable(writer, state.echoFacts);
        writeHostBytes(writer, facts);
        writePortable(writer, state.revisedActivity);
        writer.scalar(state.status.revision);
        writePortable(writer, state.status.store.records);
        writePortable(writer, state.frameWriters);
        writePortable(writer, state.settledAtPublish);
        writePortable(writer, state.focusedDrivers);
        writePortable(writer, state.focusedPersons);
        writer.scalar(state.personFocusGains);
        writer.scalar(state.inputNow);
        writeEffects(writer, state.effects);
        writePortable(writer, state.echoFacts.eraseCharacter);
    });
}

std::expected<PaneSource::Continuation, HandoverBodyError> decodeSource(std::span<const std::uint8_t> bytes, const HandoverPaneLimits& limits)
{
    const auto verified = verifiedSection(bytes, limits.maxSourceBytes);
    if (!verified)
        return std::unexpected(verified.error());
    WireReader reader(*verified);
    (void)reader.u16();
    PaneSource::Continuation state;
    readInputLedger(reader, state.input, limits);
    readGeometry(reader, state.geometry, limits);
    readPortable(reader, state.queue, limits);
    readPortable(reader, state.inputPolicy, limits);
    state.inputAwaitedGeneration = reader.u64();
    readPortable(reader, state.echoFacts, limits);
    const auto facts = decodePaneFacts(readHostBytes(reader, maxFrameBodyBytes));
    if (!facts)
        return std::unexpected(HandoverBodyError::Malformed);
    state.facts = *facts;
    readPortable(reader, state.revisedActivity, limits);
    state.status.revision = reader.u32();
    std::optional<HandoverBodyError> allocationError;
    readPortable(reader, state.status.store.records, limits, &allocationError);
    if (allocationError)
        return std::unexpected(*allocationError);
    readPortable(reader, state.frameWriters, limits);
    readPortable(reader, state.settledAtPublish, limits);
    readPortable(reader, state.focusedDrivers, limits);
    readPortable(reader, state.focusedPersons, limits);
    state.personFocusGains = reader.u64();
    state.inputNow = reader.u64();
    readEffects(reader, state.effects, limits);
    readPortable(reader, state.echoFacts.eraseCharacter, limits);
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(HandoverBodyError::Malformed);
    for (std::size_t index = 0; index < state.status.store.records.size(); ++index) {
        const auto& record = state.status.store.records[index];
        if (!OSC::validProgramStatus(record))
            return std::unexpected(HandoverBodyError::Malformed);
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (state.status.store.records[prior].id == record.id)
                return std::unexpected(HandoverBodyError::Malformed);
        }
    }
    return state;
}

constexpr std::size_t maxPTYMetadataBytes = 2 + 2 + 4 + PAL::OutputQueue::maxPendingRuns * 12 + 16;

struct PTYSection {
    std::array<PAL::OutputQueue::Run, PAL::OutputQueue::maxPendingRuns> runs {};
    std::uint16_t runCount = 0;
    std::span<const char> bytes;
};

std::expected<PTYSection, HandoverBodyError> readPTY(std::span<const std::uint8_t> bytes, const HandoverPaneLimits& limits)
{
    const auto maximum = std::min(limits.maxAcceptedPTYBytes, PAL::OutputQueue::maxPending);
    const auto verified = verifiedSection(bytes, maximum + maxPTYMetadataBytes);
    if (!verified)
        return std::unexpected(verified.error());
    WireReader reader(*verified);
    (void)reader.u16();
    PTYSection result;
    result.runCount = reader.u16();
    if (result.runCount > result.runs.size())
        return std::unexpected(HandoverBodyError::OverCapacity);
    std::size_t total = 0;
    for (unsigned index = 0; index < result.runCount; ++index) {
        auto& run = result.runs[index];
        run.owner = reader.u64();
        run.bytes = reader.u32();
        if (!run.bytes || run.bytes > maximum - total || (index && run.owner == result.runs[index - 1].owner))
            return std::unexpected(HandoverBodyError::Malformed);
        total += run.bytes;
    }
    const auto payload = readHostBytes(reader, maximum);
    if (!reader.ok() || !reader.isAtEnd() || total != payload.size())
        return std::unexpected(HandoverBodyError::Malformed);
    result.bytes = { reinterpret_cast<const char*>(payload.data()), payload.size() };
    return result;
}

enum class PaneSection : std::size_t {
    Terminal,
    Publication,
    Source,
    Session,
    PTY,
    Count
};
constexpr std::size_t paneSectionCount = std::to_underlying(PaneSection::Count);

} // namespace

std::expected<HandoverBytes, HandoverBodyError> encodePaneHandover(const PaneSource& source,
    const SessionFacts& session, const PAL::OutputQueue::Unwritten& unwrittenPTY, const HandoverPaneLimits& limits)
{
    const auto state = source.continuation();
    if (!state)
        return std::unexpected(HandoverBodyError::NotAtGround);
    if (!session.sessionID || source.paneID().low != session.sessionID
        || session.describedStructuralGeneration > state->publication.structuralGeneration)
        return std::unexpected(HandoverBodyError::Malformed);
    if (unwrittenPTY.bytes().size() > std::min(limits.maxAcceptedPTYBytes, PAL::OutputQueue::maxPending))
        return std::unexpected(HandoverBodyError::OverCapacity);
    std::array<HandoverBytes, paneSectionCount> sections;
    const auto add = [&](PaneSection kind, std::expected<HandoverBytes, HandoverBodyError> result) -> std::expected<void, HandoverBodyError> {
        if (!result)
            return std::unexpected(result.error());
        sections[std::to_underlying(kind)] = std::move(*result);
        return {};
    };
    if (auto result = add(PaneSection::Source, encodeSource(*state, limits)); !result)
        return std::unexpected(result.error());
    if (auto result = add(PaneSection::Session, encodeHandoverSessionFacts(session, limits)); !result)
        return std::unexpected(result.error());
    if (auto result = add(PaneSection::Publication, encodeHandoverPublication(source, limits.publication)); !result)
        return std::unexpected(result.error());
    if (auto result = add(PaneSection::Terminal, encodeHandoverTerminal(source.terminal(), source.generation(), limits.terminal)); !result)
        return std::unexpected(result.error());
    if (auto result = add(PaneSection::PTY, encodeSection(std::min(limits.maxAcceptedPTYBytes, PAL::OutputQueue::maxPending) + maxPTYMetadataBytes, [&](SectionWriter& writer) {
            writer.scalar(handoverBodyVersion);
            writer.scalar(static_cast<std::uint16_t>(unwrittenPTY.runs().size()));
            for (const auto& run : unwrittenPTY.runs()) {
                writer.scalar(run.owner);
                writer.scalar(static_cast<std::uint32_t>(run.bytes));
            }
            writeHostBytes(writer, { reinterpret_cast<const std::uint8_t*>(unwrittenPTY.bytes().data()), unwrittenPTY.bytes().size() });
        }));
        !result)
        return std::unexpected(result.error());
    return encodeSection(limits.maxRecordBytes, [&](SectionWriter& writer) {
        writer.scalar(handoverBodyVersion);
        writeIdentity(writer, source.paneID());
        for (const auto& section : sections)
            writeHostBytes(writer, section.view());
    });
}

std::expected<HandoverPaneImage, HandoverBodyError> decodePaneHandover(std::span<const std::uint8_t> bytes, const HandoverPaneLimits& limits,
    std::shared_ptr<MemoryBudget> decodeMemory)
{
    const auto verified = verifiedSection(bytes, limits.maxRecordBytes);
    if (!verified)
        return std::unexpected(verified.error());
    WireReader reader(*verified);
    const auto version = reader.u16();
    const auto paneID = readIdentity(reader);
    const std::array<std::size_t, paneSectionCount> sectionLimits { limits.terminal.maxRecordBytes,
        limits.publication.maxSectionBytes, limits.maxSourceBytes, limits.maxSessionBytes,
        std::min(limits.maxAcceptedPTYBytes, PAL::OutputQueue::maxPending) + maxPTYMetadataBytes };
    std::array<std::span<const std::uint8_t>, paneSectionCount> sections;
    for (std::size_t index = 0; index < sections.size(); ++index) {
        sections[index] = readHostBytes(reader, sectionLimits[index]);
        if (!reader.ok())
            return std::unexpected(HandoverBodyError::Malformed);
        WireReader sectionReader(sections[index]);
        if (sectionReader.u16() != version || !sectionReader.ok())
            return std::unexpected(HandoverBodyError::Malformed);
    }
    if (!reader.isAtEnd() || paneID == ID128 {})
        return std::unexpected(HandoverBodyError::Malformed);
    const auto section = [&](PaneSection kind) { return sections[std::to_underlying(kind)]; };
    auto state = decodeSource(section(PaneSection::Source), limits);
    if (!state)
        return std::unexpected(state.error());
    auto session = decodeHandoverSessionFacts(section(PaneSection::Session), limits);
    if (!session)
        return std::unexpected(session.error());
    auto publication = decodeHandoverPublication(section(PaneSection::Publication), limits.publication);
    if (!publication)
        return std::unexpected(publication.error());
    const auto pending = readPTY(section(PaneSection::PTY), limits);
    if (!pending)
        return std::unexpected(pending.error());
    if (paneID != publication->paneID || paneID != state->effects.paneID || paneID.low != session->sessionID
        || session->describedStructuralGeneration > publication->structuralGeneration)
        return std::unexpected(HandoverBodyError::Malformed);
    auto terminal = decodeHandoverTerminal(section(PaneSection::Terminal), limits.terminal, std::move(decodeMemory));
    if (!terminal)
        return std::unexpected(terminal.error());
    if (terminal->generation != publication->generation)
        return std::unexpected(HandoverBodyError::Malformed);
    state->publication = std::move(*publication);
    auto source = PaneSource::restored(*terminal->terminal, std::move(*state));
    if (!source)
        return std::unexpected(HandoverBodyError::Malformed);
    auto unwritten = PAL::OutputQueue::Unwritten::copyOf(pending->bytes, std::span(pending->runs).first(pending->runCount));
    if (!unwritten)
        return std::unexpected(HandoverBodyError::AllocationRefused);
    return HandoverPaneImage { paneID, std::move(terminal->terminal), std::move(source), std::move(*session), std::move(*unwritten), std::move(terminal->decodeCharge) };
}

} // namespace Ubiquitty::Protocol::Sync
