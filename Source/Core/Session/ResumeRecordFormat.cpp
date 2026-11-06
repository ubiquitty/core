// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/ResumeRecordFormat.h"

#include "Core/Protocol/Sync/ClientAttachment.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace Ubiquitty::Session {
namespace {

using namespace Protocol::Sync;

constexpr std::uint32_t recordMagic = 0x52514255; // UBQR, little endian.
constexpr std::uint16_t recordFormat = 1;

// The format uses little-endian fixed-width integers; bytes/lists have u32
// lengths. Header: magic:u32 format:u16 wire:u16, four ID128 values, pin[32].
// Pane: id, lastInput:u64, view:u8 (ff = absent), effectsAcknowledged:u64,
// effectsConsumed:u64, history:bytes, checkpoint:bytes; a nonempty
// picture adds digest:u128 and its original host replica: generation:u64,
// epoch:u32, maxPens:u64, pens:[screen-plane Pen], rowHashes:[u64] twice,
// blockHashes:u64[stateBlockCount], historyEpoch:u32, historyHead:u64, digest.
// Draft: paneID, variant:u8 (Raw/Keys/Pointer/Focus/Paste), then that intent's
// fields. Unlike live messages there is no input sequence: drafts cannot be
// mistaken for admitted input. Counts and text widths otherwise match Input.

// Fixed-width lengths keep measurement and writing identical. A null output
// measures only; no append may pass the caller's budget or wrap the offset.
class RecordWriter {
public:
    explicit RecordWriter(std::size_t limit, std::uint8_t* output = nullptr)
        : m_limit(std::min<std::size_t>(limit, std::numeric_limits<std::uint32_t>::max()))
        , m_output(output)
    {
    }

    void raw(std::span<const std::uint8_t> bytes)
    {
        if (error)
            return;
        if (bytes.size() > m_limit - m_size) {
            error = ResumeRecordError::TooLarge;
            return;
        }
        if (m_output && !bytes.empty())
            std::memcpy(m_output + m_size, bytes.data(), bytes.size());
        m_size += bytes.size();
    }

    void number(std::uint64_t value, std::size_t width)
    {
        std::array<std::uint8_t, 8> bytes {};
        for (std::size_t index = 0; index < width; ++index)
            bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
        raw(std::span(bytes).first(width));
    }

    void bytes(std::span<const std::uint8_t> value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            error = ResumeRecordError::TooLarge;
            return;
        }
        number(value.size(), 4);
        raw(value);
    }

    void identity(ID128 value)
    {
        number(value.low, 8);
        number(value.high, 8);
    }
    void digest(Digest128 value)
    {
        number(value.low, 8);
        number(value.high, 8);
    }
    [[nodiscard]] std::size_t size() const { return m_size; }
    std::optional<ResumeRecordError> error;

private:
    std::size_t m_limit;
    std::uint8_t* m_output;
    std::size_t m_size = 0;
};

std::span<const std::uint8_t> textBytes(std::string_view text)
{
    return { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() };
}

std::size_t takeCount(WireReader& reader, std::size_t maximum, std::size_t elementBytes)
{
    const std::size_t count = reader.u32();
    if (count > maximum || count > reader.remaining() / elementBytes) {
        reader.fail();
        return 0;
    }
    return count;
}

std::span<const std::uint8_t> takeBytes(WireReader& reader, std::size_t maximum)
{
    return reader.take(takeCount(reader, maximum, 1));
}

ID128 takeIdentity(WireReader& reader) { return { reader.u64(), reader.u64() }; }
Digest128 takeDigest(WireReader& reader) { return { reader.u64(), reader.u64() }; }

bool hasPane(const ResumeRecord& record, ID128 paneID)
{
    return std::ranges::any_of(record.panes, [paneID](const auto& pane) { return pane.paneID == paneID; });
}

bool hasPane(const ResumeRecordView& record, ID128 paneID)
{
    return std::ranges::any_of(std::span(record.panes).first(record.paneCount), [paneID](const auto& pane) { return pane.paneID == paneID; });
}

bool isValidPenCapacity(std::uint64_t capacity, std::uint64_t count)
{
    return count <= capacity && capacity <= std::uint64_t { std::numeric_limits<std::uint32_t>::max() } + maxPensPerEpoch;
}

void writeRecord(RecordWriter& writer, const ResumeRecord& record)
{
    writer.number(recordMagic, 4);
    writer.number(recordFormat, 2);
    writer.number(wireVersion, 2);
    writer.identity(record.hostInstanceID);
    writer.identity(record.clientInstance);
    writer.identity(record.attachmentID);
    writer.identity(record.resumeSecret);
    writer.raw(record.certificatePin);
    if (record.hostInstanceID == ID128 {} || record.clientInstance == ID128 {} || record.attachmentID == ID128 {} || record.resumeSecret == ID128 {} || record.panes.size() > maxPanesPerConnection || record.drafts.size() > maxLostInputBytes) {
        writer.error = ResumeRecordError::Malformed;
        return;
    }
    writer.number(record.panes.size(), 4);
    // A pen's wire representation is shared with the screen plane. Its
    // scratch allocation is a fixed program constant, never an input count.
    std::vector<std::uint8_t> penBytes;
    penBytes.reserve(penWireBytes);
    for (std::size_t index = 0; index < record.panes.size() && !writer.error; ++index) {
        const ResumeRecordPane& pane = record.panes[index];
        if (pane.paneID == ID128 {} || std::ranges::any_of(std::span(record.panes).first(index), [&](const auto& prior) { return prior.paneID == pane.paneID; }) || (pane.view && *pane.view > ViewState::RecordsOnly) || pane.effects.consumed < pane.effects.acknowledged) {
            writer.error = ResumeRecordError::Malformed;
            return;
        }
        writer.identity(pane.paneID);
        writer.number(pane.lastInputSeq, 8);
        writer.number(pane.view ? std::to_underlying(*pane.view) : 0xff, 1);
        writer.number(pane.effects.acknowledged, 8);
        writer.number(pane.effects.consumed, 8);
        writer.bytes(pane.history);
        writer.bytes(pane.checkpoint);
        if (writer.error)
            return;
        if (pane.checkpoint.empty())
            continue;
        writer.digest(pane.checkpointDigest);
        const ScreenReplica& replica = pane.replica;
        WireReader picture(pane.checkpoint);
        const auto pictureGeneration = picture.u64();
        (void)picture.u32();
        const auto rows = picture.u16();
        const auto columns = picture.u16();
        if (!picture.ok() || pictureGeneration != replica.generation || rows == 0 || rows > maxRows || columns == 0 || columns > maxColumns || replica.rowHashes()[0].size() != rows || replica.rowHashes()[1].size() != rows) {
            writer.error = ResumeRecordError::Malformed;
            return;
        }
        if (digestOf(pane.checkpoint) != pane.checkpointDigest || replica.pens().size() > std::numeric_limits<std::uint32_t>::max() || !isValidPenCapacity(replica.maxPens, replica.pens().size())) {
            writer.error = ResumeRecordError::Malformed;
            return;
        }
        writer.number(replica.generation, 8);
        writer.number(replica.dictionaryEpoch, 4);
        writer.number(replica.maxPens, 8);
        writer.number(replica.pens().size(), 4);
        for (const Pen& pen : replica.pens()) {
            penBytes.clear();
            WireWriter penWriter(penBytes);
            writePen(penWriter, pen);
            writer.raw(penBytes);
            if (writer.error)
                return;
        }
        for (const auto rows : replica.rowHashes()) {
            if (rows.empty() || rows.size() > static_cast<std::size_t>(maxRows)) {
                writer.error = ResumeRecordError::Malformed;
                return;
            }
            writer.number(rows.size(), 4);
            for (const std::uint64_t hash : rows)
                writer.number(hash, 8);
        }
        for (const std::uint64_t hash : replica.blockHashes)
            writer.number(hash, 8);
        writer.number(replica.historyEpoch, 4);
        writer.number(replica.historyHead, 8);
        writer.digest(replica.stateDigest);
    }
    writer.number(record.drafts.size(), 4);
    std::size_t draftBytes = 0;
    for (const ResumeDraft& draft : record.drafts) {
        if (writer.error)
            return;
        if (!hasPane(record, draft.paneID)) {
            writer.error = ResumeRecordError::Malformed;
            return;
        }
        writer.identity(draft.paneID);
        writer.number(draft.intent.index(), 1);
        std::size_t cost = 0;
        std::visit([&](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<Input, RawInput>) {
                cost = std::max<std::size_t>(input.bytes.size(), 1);
                writer.bytes(input.bytes);
            } else if constexpr (std::is_same_v<Input, PasteInput>) {
                if (input.text.size() > maxLostInputBytes || input.offset > maxPasteBytes || input.text.size() > maxPasteBytes - input.offset || input.end > PasteEnd::Aborted || !isValidUTF8(input.text))
                    writer.error = ResumeRecordError::Malformed;
                cost = input.text.size() + 12;
                writer.number(input.offset, 8);
                writer.number(std::to_underlying(input.end), 1);
                writer.bytes(input.text);
            } else if constexpr (std::is_same_v<Input, FocusInput>) {
                cost = 3;
                if (input.focus > Modes::FocusState::Focused)
                    writer.error = ResumeRecordError::Malformed;
                writer.number(std::to_underlying(input.focus), 1);
            } else {
                if (input.events.empty() || input.events.size() > maxInputEventsPerFrame) {
                    writer.error = ResumeRecordError::Malformed;
                    return;
                }
                writer.number(input.events.size(), 4);
                cost = input.events.size() * 32;
                for (const auto& event : input.events) {
                    if constexpr (std::is_same_v<Input, KeyInput>) {
                        if (event.key > std::numeric_limits<std::uint16_t>::max() || event.kind > KeyEventKind::Release || event.text.size() > maxKeyEventTextBytes || !isValidUTF8(textBytes(event.text)))
                            writer.error = ResumeRecordError::Malformed;
                        cost += event.text.size();
                        writer.number(event.key, 4);
                        writer.number(event.modifiers, 2);
                        writer.number(std::to_underlying(event.kind), 1);
                        writer.bytes(textBytes(event.text));
                        writer.number(event.unshiftedCodePoint, 4);
                        writer.number(event.shiftedCodePoint, 4);
                        writer.number(event.baseLayoutCodePoint, 4);
                        writer.number(event.wasModified ? 1 : 0, 1);
                    } else {
                        if (event.kind > PointerEventKind::Motion)
                            writer.error = ResumeRecordError::Malformed;
                        writer.number(std::to_underlying(event.kind), 1);
                        writer.number(event.button, 1);
                        writer.number(event.modifiers, 2);
                        writer.number(event.row, 2);
                        writer.number(event.column, 2);
                        writer.number(event.pixelX, 2);
                        writer.number(event.pixelY, 2);
                    }
                }
            }
        },
            draft.intent);
        if (cost > maxLostInputBytes - draftBytes) {
            writer.error = ResumeRecordError::TooLarge;
            return;
        }
        draftBytes += cost;
    }
}

/// Admits requested element storage without multiplying unchecked counts.
struct MaterializedBudget {
    std::size_t remaining;
    bool isExceeded = false;

    bool admit(std::size_t count, std::size_t width = 1)
    {
        if (isExceeded || count > remaining / width) {
            isExceeded = true;
            return false;
        }
        remaining -= count * width;
        return true;
    }
};

void readDrafts(WireReader& reader, const ResumeRecordView& record, std::vector<ResumeDraft>* output = nullptr, MaterializedBudget* storage = nullptr)
{
    std::size_t budget = maxLostInputBytes;
    for (std::size_t index = 0; index < record.draftCount && reader.ok(); ++index) {
        const auto paneID = takeIdentity(reader);
        if (!hasPane(record, paneID))
            reader.fail();
        const std::uint8_t kind = reader.u8();
        std::size_t cost = 0;
        InputIntent intent;
        if (kind == 0) {
            const auto bytes = takeBytes(reader, budget);
            cost = std::max<std::size_t>(bytes.size(), 1);
            if (storage && !storage->admit(bytes.size()))
                reader.fail();
            if (output)
                intent = RawInput { { bytes.begin(), bytes.end() } };
        } else if (kind == 4) {
            const std::uint64_t offset = reader.u64();
            const auto end = reader.u8();
            if (end > std::to_underlying(PasteEnd::Aborted))
                reader.fail();
            const auto text = takeBytes(reader, budget);
            if (offset > maxPasteBytes || text.size() > maxPasteBytes - offset || !isValidUTF8(text))
                reader.fail();
            cost = text.size() + 12;
            if (storage && !storage->admit(text.size()))
                reader.fail();
            if (output)
                intent = PasteInput { { text.begin(), text.end() }, offset, static_cast<PasteEnd>(end) };
        } else if (kind == 3) {
            const auto focus = reader.u8();
            if (focus > 1)
                reader.fail();
            cost = 3;
            if (output)
                intent = FocusInput { static_cast<Modes::FocusState>(focus) };
        } else if (kind == 1 || kind == 2) {
            const std::size_t count = takeCount(reader, maxInputEventsPerFrame, kind == 1 ? 24 : 12);
            if (count == 0)
                reader.fail();
            cost = count * 32;
            if (storage && !storage->admit(count, kind == 1 ? sizeof(KeyEvent) : sizeof(PointerEvent)))
                reader.fail();
            if (output) {
                if (kind == 1) {
                    intent = KeyInput {};
                    std::get<KeyInput>(intent).events.reserve(count);
                } else {
                    intent = PointerInput {};
                    std::get<PointerInput>(intent).events.reserve(count);
                }
            }
            for (std::size_t event = 0; event < count && reader.ok(); ++event) {
                if (kind == 1) {
                    KeyEvent key;
                    key.key = reader.u32();
                    if (key.key > std::numeric_limits<std::uint16_t>::max())
                        reader.fail();
                    key.modifiers = reader.u16();
                    const auto eventKind = reader.u8();
                    if (eventKind > std::to_underlying(KeyEventKind::Release))
                        reader.fail();
                    key.kind = static_cast<KeyEventKind>(eventKind);
                    const auto text = takeBytes(reader, maxKeyEventTextBytes);
                    if (!isValidUTF8(text))
                        reader.fail();
                    cost += text.size();
                    if (storage && !storage->admit(text.size() + 1))
                        reader.fail();
                    key.unshiftedCodePoint = reader.u32();
                    key.shiftedCodePoint = reader.u32();
                    key.baseLayoutCodePoint = reader.u32();
                    const auto wasModified = reader.u8();
                    if (wasModified > 1)
                        reader.fail();
                    key.wasModified = wasModified == 1;
                    if (output) {
                        key.text.assign(reinterpret_cast<const char*>(text.data()), text.size());
                        std::get<KeyInput>(intent).events.push_back(std::move(key));
                    }
                } else {
                    PointerEvent pointer;
                    const auto eventKind = reader.u8();
                    if (eventKind > std::to_underlying(PointerEventKind::Motion))
                        reader.fail();
                    pointer.kind = static_cast<PointerEventKind>(eventKind);
                    pointer.button = reader.u8();
                    pointer.modifiers = reader.u16();
                    pointer.row = reader.u16();
                    pointer.column = reader.u16();
                    pointer.pixelX = reader.u16();
                    pointer.pixelY = reader.u16();
                    if (output)
                        std::get<PointerInput>(intent).events.push_back(pointer);
                }
            }
        } else {
            reader.fail();
        }
        if (cost > budget)
            reader.fail();
        else
            budget -= cost;
        if (reader.ok() && output)
            output->push_back({ paneID, std::move(intent) });
    }
}

/// Reads one original host replica, optionally retaining its bounded fields.
std::expected<void, ResumeRecordError> readReplica(WireReader& reader, ScreenReplica* output, MaterializedBudget* budget)
{
    ScreenReplica replica;
    replica.generation = reader.u64();
    replica.dictionaryEpoch = reader.u32();
    const auto maxPens = reader.u64();
    if (maxPens > std::numeric_limits<std::size_t>::max()) {
        reader.fail();
        return std::unexpected(ResumeRecordError::Malformed);
    }
    replica.maxPens = static_cast<std::size_t>(maxPens);
    const auto pens = takeCount(reader, std::numeric_limits<std::uint32_t>::max(), penWireBytes);
    const auto penBegin = reader.offset();
    for (std::size_t index = 0; index < pens && reader.ok(); ++index)
        (void)readPen(reader);
    const auto penBytes = reader.consumed(penBegin, reader.offset());
    std::array<std::size_t, 2> rowCounts;
    std::array<std::span<const std::uint8_t>, 2> rowBytes;
    for (std::size_t screen = 0; screen < 2; ++screen) {
        const auto count = takeCount(reader, maxRows, sizeof(std::uint64_t));
        rowCounts[screen] = count;
        rowBytes[screen] = reader.take(count * sizeof(std::uint64_t));
    }
    for (auto& hash : replica.blockHashes)
        hash = reader.u64();
    replica.historyEpoch = reader.u32();
    replica.historyHead = reader.u64();
    replica.stateDigest = takeDigest(reader);
    if (!reader.ok())
        return std::unexpected(ResumeRecordError::Malformed);
    const auto storageBytes = ReplicaStorageBuilder::storageBytesForCapacity(pens, rowCounts);
    if (!storageBytes || (budget && !budget->admit(*storageBytes)))
        return std::unexpected(ResumeRecordError::TooLarge);
    if (output) {
        auto storage = ReplicaStorageBuilder::create(pens, rowCounts, replica.maxPens);
        if (!storage)
            return std::unexpected(storage.error() == ReplicaStorageError::AllocationRefused ? ResumeRecordError::AllocationFailed : ResumeRecordError::TooLarge);
        WireReader penReader(penBytes);
        for (auto& pen : storage->pens())
            pen = readPen(penReader);
        for (std::size_t screen = 0; screen < 2; ++screen) {
            WireReader hashes(rowBytes[screen]);
            for (auto& hash : storage->rowHashes()[screen])
                hash = hashes.u64();
        }
        replica.storage = std::move(*storage).publish();
        *output = std::move(replica);
    }
    return {};
}

std::expected<std::size_t, ResumeRecordError> measureMaterialization(const ResumeRecordView& view)
{
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    MaterializedBudget budget { maximum };
    if (!budget.admit(1, sizeof(ResumeRecord)) || !budget.admit(view.paneCount, sizeof(ResumeRecordPane)) || !budget.admit(view.draftCount, sizeof(ResumeDraft)))
        return std::unexpected(ResumeRecordError::TooLarge);
    for (const auto& pane : std::span(view.panes).first(view.paneCount)) {
        if (!budget.admit(pane.checkpoint.size()) || !budget.admit(pane.history.size()))
            return std::unexpected(ResumeRecordError::TooLarge);
        if (!pane.replica.empty()) {
            WireReader reader(pane.replica);
            if (auto result = readReplica(reader, nullptr, &budget); !result)
                return std::unexpected(result.error());
            if (budget.isExceeded)
                return std::unexpected(ResumeRecordError::TooLarge);
            if (!reader.ok() || !reader.isAtEnd())
                return std::unexpected(ResumeRecordError::Malformed);
        }
    }
    WireReader drafts(view.drafts);
    readDrafts(drafts, view, nullptr, &budget);
    if (budget.isExceeded)
        return std::unexpected(ResumeRecordError::TooLarge);
    if (!drafts.ok() || !drafts.isAtEnd())
        return std::unexpected(ResumeRecordError::Malformed);
    return maximum - budget.remaining;
}

} // namespace

std::expected<EncodedResumeRecord, ResumeRecordError> encodeResumeRecord(const Protocol::Sync::ResumeRecord& record, std::size_t maxBytes)
{
    RecordWriter measure(maxBytes);
    writeRecord(measure, record);
    if (measure.error)
        return std::unexpected(*measure.error);
    EncodedResumeRecord encoded { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[measure.size()]), measure.size() };
    if (!encoded.bytes)
        return std::unexpected(ResumeRecordError::AllocationFailed);
    RecordWriter writer(encoded.size, encoded.bytes.get());
    writeRecord(writer, record);
    if (writer.error)
        return std::unexpected(*writer.error);
    return encoded;
}

std::expected<ResumeRecordView, ResumeRecordError> decodeResumeRecord(std::span<const std::uint8_t> bytes, std::size_t maxBytes)
{
    if (bytes.size() > maxBytes || bytes.size() > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected(ResumeRecordError::TooLarge);
    WireReader reader(bytes);
    if (reader.u32() != recordMagic)
        return std::unexpected(ResumeRecordError::Malformed);
    const auto format = reader.u16();
    const auto version = reader.u16();
    if (!reader.ok())
        return std::unexpected(ResumeRecordError::Malformed);
    if (format != recordFormat || version != wireVersion)
        return std::unexpected(ResumeRecordError::VersionMismatch);
    ResumeRecordView record;
    record.hostInstanceID = takeIdentity(reader);
    record.clientInstance = takeIdentity(reader);
    record.attachmentID = takeIdentity(reader);
    record.resumeSecret = takeIdentity(reader);
    const auto pin = reader.take(record.certificatePin.size());
    if (reader.ok())
        std::ranges::copy(pin, record.certificatePin.begin());
    if (record.hostInstanceID == ID128 {} || record.clientInstance == ID128 {} || record.attachmentID == ID128 {} || record.resumeSecret == ID128 {})
        reader.fail();
    const auto count = takeCount(reader, maxPanesPerConnection, 45);
    for (std::size_t index = 0; index < count && reader.ok(); ++index) {
        ResumePaneView& pane = record.panes[index];
        pane.paneID = takeIdentity(reader);
        if (pane.paneID == ID128 {} || hasPane(record, pane.paneID))
            reader.fail();
        ++record.paneCount;
        pane.lastInputSeq = reader.u64();
        const std::uint8_t view = reader.u8();
        if (view <= std::to_underlying(ViewState::RecordsOnly))
            pane.view = static_cast<ViewState>(view);
        else if (view != 0xff)
            reader.fail();
        pane.effects = { .acknowledged = reader.u64(), .consumed = reader.u64() };
        if (pane.effects.consumed < pane.effects.acknowledged)
            reader.fail();
        pane.history = takeBytes(reader, maxBytes);
        pane.checkpoint = takeBytes(reader, maxBytes);
        if (pane.checkpoint.empty())
            continue;
        pane.checkpointDigest = takeDigest(reader);
        if (digestOf(pane.checkpoint) != pane.checkpointDigest)
            reader.fail();
        const std::size_t begin = reader.offset();
        const std::uint64_t generation = reader.u64();
        (void)reader.u32(); // Original dictionary epoch.
        const std::uint64_t maxPens = reader.u64();
        const std::size_t pens = takeCount(reader, std::numeric_limits<std::uint32_t>::max(), penWireBytes);
        if (!isValidPenCapacity(maxPens, pens))
            reader.fail();
        (void)reader.take(pens * penWireBytes);
        WireReader picture(pane.checkpoint);
        if (picture.u64() != generation)
            reader.fail();
        (void)picture.u32(); // The picture has its own dictionary epoch.
        const std::size_t rows = picture.u16();
        const std::size_t columns = picture.u16();
        if (!picture.ok() || rows == 0 || rows > static_cast<std::size_t>(maxRows) || columns == 0 || columns > static_cast<std::size_t>(maxColumns))
            reader.fail();
        for (int screen = 0; screen < 2; ++screen) {
            const auto hashes = takeCount(reader, static_cast<std::size_t>(maxRows), 8);
            if (hashes != rows)
                reader.fail();
            (void)reader.take(hashes * 8);
        }
        (void)reader.take(stateBlockCount * 8 + 4 + 8 + 16);
        pane.replica = reader.consumed(begin, reader.offset());
    }
    record.draftCount = takeCount(reader, maxLostInputBytes, 18);
    const std::size_t draftsBegin = reader.offset();
    readDrafts(reader, record);
    record.drafts = reader.consumed(draftsBegin, reader.offset());
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(ResumeRecordError::Malformed);
    const auto storage = measureMaterialization(record);
    if (!storage)
        return std::unexpected(storage.error());
    record.materializedBytes = *storage;
    return record;
}

std::expected<ResumeRecord, ResumeRecordError> materializeResumeRecord(std::span<const std::uint8_t> bytes, std::size_t maxBytes, std::size_t maxMaterializedBytes)
{
    const auto view = decodeResumeRecord(bytes, maxBytes);
    if (!view)
        return std::unexpected(view.error());
    if (view->materializedBytes > maxMaterializedBytes)
        return std::unexpected(ResumeRecordError::TooLarge);
    ResumeRecord record { .hostInstanceID = view->hostInstanceID, .clientInstance = view->clientInstance, .attachmentID = view->attachmentID, .resumeSecret = view->resumeSecret, .certificatePin = view->certificatePin, .panes = {}, .drafts = {} };
    record.panes.reserve(view->paneCount);
    record.drafts.reserve(view->draftCount);
    for (const auto& pane : std::span(view->panes).first(view->paneCount)) {
        ResumeRecordPane owned { .paneID = pane.paneID, .lastInputSeq = pane.lastInputSeq, .view = pane.view, .checkpoint = { pane.checkpoint.begin(), pane.checkpoint.end() }, .checkpointDigest = pane.checkpointDigest, .replica = {}, .effects = pane.effects, .history = { pane.history.begin(), pane.history.end() } };
        if (!pane.replica.empty()) {
            WireReader reader(pane.replica);
            if (auto result = readReplica(reader, &owned.replica, nullptr); !result)
                return std::unexpected(result.error());
            assert(reader.ok() && reader.isAtEnd());
        }
        record.panes.push_back(std::move(owned));
    }
    WireReader drafts(view->drafts);
    readDrafts(drafts, *view, &record.drafts);
    assert(drafts.ok() && drafts.isAtEnd());
    return record;
}

EncodedResumeRecordStore::EncodedResumeRecordStore(ResumeRecordSlot& slot, std::size_t maxBytes)
    : m_slot(slot)
    , m_maxBytes(maxBytes)
{
}

ResumeWriteOutcome EncodedResumeRecordStore::write(const ResumeRecord& record)
{
    const auto encoded = encodeResumeRecord(record, m_maxBytes);
    if (!encoded)
        return ResumeWriteOutcome::Unchanged;
    return m_slot.write(encoded->view()) ? ResumeWriteOutcome::Committed : ResumeWriteOutcome::Unchanged;
}

std::optional<ResumeRecord> EncodedResumeRecordStore::read()
{
    const auto bytes = m_slot.read(m_maxBytes);
    if (!bytes)
        return std::nullopt;
    auto record = materializeResumeRecord(*bytes, m_maxBytes, m_maxBytes);
    if (!record)
        return std::nullopt;
    return std::move(*record);
}

ResumePurgeOutcome EncodedResumeRecordStore::purge()
{
    return m_slot.erase() ? ResumePurgeOutcome::Purged : ResumePurgeOutcome::Refused;
}

std::expected<ResumeRecord, ResumeRecordError> recordFromPictureRequest(std::span<const std::uint8_t> bytes, std::size_t maxBytes)
{
    WireReader reader(bytes);
    ResumeRecord record {};
    record.hostInstanceID = takeIdentity(reader);
    record.clientInstance = takeIdentity(reader);
    record.attachmentID = takeIdentity(reader);
    record.resumeSecret = takeIdentity(reader);
    ResumeRecordPane pane {};
    pane.paneID = takeIdentity(reader);
    pane.lastInputSeq = reader.u64();
    const std::uint8_t view = reader.u8();
    if (view != 0xff) {
        if (view > std::to_underlying(ViewState::RecordsOnly))
            reader.fail();
        pane.view = static_cast<ViewState>(view);
    }
    pane.effects.acknowledged = reader.u64();
    pane.effects.consumed = reader.u64();
    const auto checkpoint = takeBytes(reader, maxBytes);
    if (!reader.ok() || !reader.isAtEnd() || pane.paneID == ID128 {} || record.clientInstance == ID128 {} || pane.effects.consumed < pane.effects.acknowledged)
        return std::unexpected(ResumeRecordError::Malformed);
    if (!checkpoint.empty()) {
        const auto policy = measuredClientPolicy();
        auto scratch = std::unique_ptr<Ubiquitty::Terminal>(new (std::nothrow) Ubiquitty::Terminal);
        if (!scratch)
            return std::unexpected(ResumeRecordError::AllocationFailed);
        if (applyCheckpoint(*scratch, checkpoint, pane.replica, { .maxDecodedScreenCellBytes = policy.maxDecodedScreenCellBytes, .maxStorageCost = policy.maxCheckpointStorageCost }) != ApplyOutcome::Applied)
            return std::unexpected(ResumeRecordError::Malformed);
        pane.replica.scratch.reset();
        pane.replica.echoContext.reset();
        pane.checkpoint.assign(checkpoint.begin(), checkpoint.end());
        pane.checkpointDigest = digestOf(pane.checkpoint);
    }
    record.panes.push_back(std::move(pane));
    return record;
}

std::optional<std::vector<std::uint8_t>> resumeReplyFromStore(ResumeRecordStore& store)
{
    const auto stored = store.read();
    if (!stored)
        return std::nullopt;
    // The attachment restores and names the panes exactly as a client with
    // Core in its process does; its Greet is read back for the request.
    ClientAttachment attachment(measuredClientPolicy(), measuredLivenessPolicy(), { .clientInstance = stored->clientInstance, .kind = ClientKind::Human }, nullptr, &store);
    if (!attachment.restoreFromStore())
        return std::nullopt;
    std::vector<bool> isDrawn;
    for (const auto& pane : stored->panes)
        isDrawn.push_back(attachment.restoredPictureGeneration(pane.paneID).has_value());
    (void)attachment.connect(0, ResumeCarrier::Secret);
    FrameDecoder decoder;
    decoder.append(attachment.takeStreamOutput());
    std::optional<Greet> greet;
    while (const auto frame = decoder.next()) {
        if (frame->kind == FrameKind::Greet)
            greet = decodeGreet(frame->body);
    }
    if (!greet || !greet->resume)
        return std::nullopt;
    std::vector<std::uint8_t> tail;
    encodeGreetResume(tail, *greet->resume);
    if (tail.empty())
        return std::nullopt;
    std::vector<std::uint8_t> reply;
    WireWriter rest(reply);
    rest.u64(stored->clientInstance.low);
    rest.u64(stored->clientInstance.high);
    rest.u32(static_cast<std::uint32_t>(tail.size()));
    reply.insert(reply.end(), tail.begin(), tail.end());
    rest.u16(static_cast<std::uint16_t>(stored->panes.size()));
    for (std::size_t index = 0; index < stored->panes.size(); ++index) {
        const auto& pane = stored->panes[index];
        rest.u64(pane.paneID.low);
        rest.u64(pane.paneID.high);
        rest.u64(pane.lastInputSeq);
        rest.u64(pane.effects.acknowledged);
        rest.u64(pane.effects.consumed);
        const std::span<const std::uint8_t> picture = isDrawn[index] ? std::span<const std::uint8_t>(pane.checkpoint) : std::span<const std::uint8_t> {};
        rest.u32(static_cast<std::uint32_t>(picture.size()));
        reply.insert(reply.end(), picture.begin(), picture.end());
    }
    return reply;
}

} // namespace Ubiquitty::Session
