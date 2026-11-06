// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Envelope.h"

#include "Core/Protocol/Sync/Wire.h"

namespace Ubiquitty::Protocol::Sync {

namespace {

/// How far a header parse got.
enum class HeaderStatus : std::uint8_t {
    Complete,
    NeedsMore,
    Failed,
};

/// A parsed envelope header.
struct Header {
    FrameKind kind = FrameKind::Bye;
    std::uint32_t channel = 0;
    std::size_t length = 0;
    std::size_t headerBytes = 0;
};

/// Reads one LEB128 varint of at most `maxBytes` bytes, telling a value cut
/// short by the end of the bytes from one that can never be valid.
HeaderStatus readVarint(std::span<const std::uint8_t> bytes, std::size_t& offset, std::size_t maxBytes, std::uint64_t& value)
{
    value = 0;
    for (std::size_t index = 0; index < maxBytes; ++index) {
        if (offset >= bytes.size())
            return HeaderStatus::NeedsMore;
        const std::uint8_t byte = bytes[offset++];
        value |= static_cast<std::uint64_t>(byte & 0x7F) << (7 * index);
        if ((byte & 0x80) == 0) {
            // A trailing zero group is a longer spelling of a shorter value.
            if (index > 0 && byte == 0)
                return HeaderStatus::Failed;
            return HeaderStatus::Complete;
        }
    }
    return HeaderStatus::Failed;
}

HeaderStatus parseHeader(std::span<const std::uint8_t> bytes, Header& header, EnvelopeError& error)
{
    if (bytes.empty())
        return HeaderStatus::NeedsMore;
    const std::optional<FrameKind> kind = frameKindOf(bytes[0]);
    if (!kind) {
        error = EnvelopeError::UnknownKind;
        return HeaderStatus::Failed;
    }
    std::size_t offset = 1;
    std::uint64_t channel = 0;
    HeaderStatus status = readVarint(bytes, offset, 5, channel);
    if (status == HeaderStatus::Complete && (channel > maxChannel || (channel == 0) != isConnectionKind(*kind)))
        status = HeaderStatus::Failed;
    if (status != HeaderStatus::Complete) {
        if (status == HeaderStatus::Failed)
            error = EnvelopeError::Malformed;
        return status;
    }
    std::uint64_t length = 0;
    status = readVarint(bytes, offset, 3, length);
    if (status == HeaderStatus::Failed) {
        // Three groups hold 21 bits, so an unterminated third group is a
        // length past the cap however it continues.
        error = EnvelopeError::Oversize;
        return status;
    }
    if (status == HeaderStatus::NeedsMore)
        return status;
    if (length > maxFrameBodyBytes) {
        error = EnvelopeError::Oversize;
        return HeaderStatus::Failed;
    }
    header = { .kind = *kind, .channel = static_cast<std::uint32_t>(channel), .length = static_cast<std::size_t>(length), .headerBytes = offset };
    return HeaderStatus::Complete;
}

} // namespace

FrameHeaderState readFrameHeader(std::span<const std::uint8_t> bytes, FrameHeader& header, EnvelopeError& error)
{
    Header parsed;
    switch (parseHeader(bytes, parsed, error)) {
    case HeaderStatus::NeedsMore:
        return FrameHeaderState::NeedsMore;
    case HeaderStatus::Failed:
        return FrameHeaderState::Failed;
    case HeaderStatus::Complete:
        header = { .kind = parsed.kind, .channel = parsed.channel, .bodyBytes = parsed.length };
        return FrameHeaderState::Complete;
    }
    return FrameHeaderState::Failed;
}

const char* frameKindName(std::uint8_t value)
{
    switch (static_cast<FrameKind>(value)) {
    case FrameKind::Greet:
        return "Greet";
    case FrameKind::Welcome:
        return "Welcome";
    case FrameKind::Facts:
        return "Facts";
    case FrameKind::Ack:
        return "Ack";
    case FrameKind::Probe:
        return "Probe";
    case FrameKind::Echo:
        return "Echo";
    case FrameKind::PaneBind:
        return "PaneBind";
    case FrameKind::PaneUnbind:
        return "PaneUnbind";
    case FrameKind::BulkChunk:
        return "BulkChunk";
    case FrameKind::Bye:
        return "Bye";
    case FrameKind::TypographyFacts:
        return "TypographyFacts";
    case FrameKind::BulkAbort:
        return "BulkAbort";
    case FrameKind::Call:
        return "Call";
    case FrameKind::Result:
        return "Result";
    case FrameKind::Notify:
        return "Notify";
    case FrameKind::WorkspaceStructure:
        return "WorkspaceStructure";
    case FrameKind::PaneListed:
        return "PaneListed";
    case FrameKind::ProportionalFaces:
        return "ProportionalFaces";
    case FrameKind::CheckpointHead:
        return "CheckpointHead";
    case FrameKind::Update:
        return "Update";
    case FrameKind::Resync:
        return "Resync";
    case FrameKind::View:
        return "View";
    case FrameKind::PaneDirty:
        return "PaneDirty";
    case FrameKind::PanePalette:
        return "PanePalette";
    case FrameKind::HistoryInline:
        return "HistoryInline";
    case FrameKind::HistoryHead:
        return "HistoryHead";
    case FrameKind::HistoryReset:
        return "HistoryReset";
    case FrameKind::HistoryWant:
        return "HistoryWant";
    case FrameKind::PaneFacts:
        return "PaneFacts";
    case FrameKind::StatusRecords:
        return "StatusRecords";
    case FrameKind::Effect:
        return "Effect";
    case FrameKind::ClipboardRequest:
        return "ClipboardRequest";
    case FrameKind::ImageHead:
        return "ImageHead";
    case FrameKind::ImagePlacements:
        return "ImagePlacements";
    case FrameKind::SampleHead:
        return "SampleHead";
    case FrameKind::RichTextMeasure:
        return "RichTextMeasure";
    case FrameKind::NativeDragCommand:
        return "NativeDragCommand";
    case FrameKind::NativeDragEvent:
        return "NativeDragEvent";
    case FrameKind::Receipt:
        return "Receipt";
    case FrameKind::Keys:
        return "Keys";
    case FrameKind::Pointer:
        return "Pointer";
    case FrameKind::Paste:
        return "Paste";
    case FrameKind::PasteAbort:
        return "PasteAbort";
    case FrameKind::Raw:
        return "Raw";
    case FrameKind::Focus:
        return "Focus";
    case FrameKind::PasteReceipt:
        return "PasteReceipt";
    case FrameKind::InputDiscarded:
        return "InputDiscarded";
    case FrameKind::Viewport:
        return "Viewport";
    case FrameKind::ClipboardData:
        return "ClipboardData";
    case FrameKind::ImageWant:
        return "ImageWant";
    case FrameKind::RichTextMeasured:
        return "RichTextMeasured";
    }
    static constexpr auto unknownNames = [] {
        constexpr char hex[] = "0123456789ABCDEF";
        std::array<std::array<char, 5>, 256> names {};
        for (std::size_t index = 0; index < names.size(); ++index)
            names[index] = { '0', 'x', hex[index >> 4], hex[index & 15], '\0' };
        return names;
    }();
    return unknownNames[value].data();
}

std::optional<FrameKind> frameKindOf(std::uint8_t value)
{
    switch (static_cast<FrameKind>(value)) {
    case FrameKind::Greet:
    case FrameKind::Welcome:
    case FrameKind::Facts:
    case FrameKind::Ack:
    case FrameKind::Probe:
    case FrameKind::Echo:
    case FrameKind::PaneBind:
    case FrameKind::PaneUnbind:
    case FrameKind::BulkChunk:
    case FrameKind::Bye:
    case FrameKind::TypographyFacts:
    case FrameKind::BulkAbort:
    case FrameKind::Call:
    case FrameKind::Result:
    case FrameKind::Notify:
    case FrameKind::WorkspaceStructure:
    case FrameKind::PaneListed:
    case FrameKind::ProportionalFaces:
    case FrameKind::CheckpointHead:
    case FrameKind::Update:
    case FrameKind::Resync:
    case FrameKind::View:
    case FrameKind::PaneDirty:
    case FrameKind::PanePalette:
    case FrameKind::HistoryInline:
    case FrameKind::HistoryHead:
    case FrameKind::HistoryReset:
    case FrameKind::HistoryWant:
    case FrameKind::PaneFacts:
    case FrameKind::StatusRecords:
    case FrameKind::Effect:
    case FrameKind::RichTextMeasure:
    case FrameKind::ClipboardRequest:
    case FrameKind::NativeDragCommand:
    case FrameKind::RichTextMeasured:
    case FrameKind::ClipboardData:
    case FrameKind::NativeDragEvent:
    case FrameKind::ImageHead:
    case FrameKind::ImageWant:
    case FrameKind::ImagePlacements:
    case FrameKind::SampleHead:
    case FrameKind::Receipt:
    case FrameKind::Keys:
    case FrameKind::Pointer:
    case FrameKind::Paste:
    case FrameKind::PasteAbort:
    case FrameKind::Raw:
    case FrameKind::Focus:
    case FrameKind::PasteReceipt:
    case FrameKind::InputDiscarded:
    case FrameKind::Viewport:
        return static_cast<FrameKind>(value);
    }
    return std::nullopt;
}

std::optional<NotifyKind> notifyKindOf(std::uint64_t value)
{
    if (value > UINT8_MAX)
        return std::nullopt;
    switch (static_cast<NotifyKind>(value)) {
    case NotifyKind::Attachments:
    case NotifyKind::Events:
    case NotifyKind::ConsentStatus:
    case NotifyKind::ConsentQuestion:
    case NotifyKind::StructureConsent:
    case NotifyKind::ConsentResolved:
    case NotifyKind::PasteReady:
        return static_cast<NotifyKind>(value);
    }
    return std::nullopt;
}

Lane laneOf(FrameKind kind)
{
    // A paste's pieces share the bulk lane with transfers, so a keystroke
    // never waits behind them.
    return kind == FrameKind::BulkChunk || kind == FrameKind::Paste || kind == FrameKind::NativeDragCommand || kind == FrameKind::NativeDragEvent ? Lane::Bulk : Lane::Interactive;
}

Direction directionOf(FrameKind kind)
{
    switch (kind) {
    case FrameKind::Greet:
    case FrameKind::Facts:
    case FrameKind::TypographyFacts:
    case FrameKind::ProportionalFaces:
    case FrameKind::Ack:
    case FrameKind::Resync:
    case FrameKind::View:
    case FrameKind::HistoryWant:
    case FrameKind::ImageWant:
    case FrameKind::Keys:
    case FrameKind::RichTextMeasured:
    case FrameKind::ClipboardData:
    case FrameKind::NativeDragEvent:
    case FrameKind::Pointer:
    case FrameKind::Paste:
    case FrameKind::PasteAbort:
    case FrameKind::Raw:
    case FrameKind::Focus:
    case FrameKind::Viewport:
    // A client calls and a host answers. No client serves a method, so a
    // Call from a host and a Result from a client are each refused by the
    // end that reads them.
    case FrameKind::Call:
        return Direction::ClientToHost;
    case FrameKind::Welcome:
    case FrameKind::PaneBind:
    case FrameKind::PaneUnbind:
    case FrameKind::WorkspaceStructure:
    case FrameKind::PaneListed:
    case FrameKind::BulkAbort:
    case FrameKind::CheckpointHead:
    case FrameKind::Update:
    case FrameKind::PaneDirty:
    case FrameKind::PanePalette:
    case FrameKind::HistoryInline:
    case FrameKind::HistoryHead:
    case FrameKind::HistoryReset:
    case FrameKind::PaneFacts:
    case FrameKind::StatusRecords:
    case FrameKind::Effect:
    case FrameKind::RichTextMeasure:
    case FrameKind::ClipboardRequest:
    case FrameKind::NativeDragCommand:
    case FrameKind::ImageHead:
    case FrameKind::ImagePlacements:
    case FrameKind::SampleHead:
    case FrameKind::Receipt:
    case FrameKind::PasteReceipt:
    case FrameKind::InputDiscarded:
    case FrameKind::Result:
    case FrameKind::Notify:
        return Direction::HostToClient;
    case FrameKind::Bye:
    case FrameKind::BulkChunk:
    case FrameKind::Probe:
    case FrameKind::Echo:
        return Direction::Both;
    }
    return Direction::Both;
}

bool isConnectionKind(FrameKind kind)
{
    switch (kind) {
    case FrameKind::Greet:
    case FrameKind::Welcome:
    case FrameKind::Facts:
    case FrameKind::Ack:
    case FrameKind::Probe:
    case FrameKind::Echo:
    case FrameKind::PaneBind:
    case FrameKind::PaneUnbind:
    case FrameKind::BulkChunk:
    case FrameKind::Bye:
    case FrameKind::BulkAbort:
    case FrameKind::Call:
    case FrameKind::Result:
    case FrameKind::Notify:
    case FrameKind::WorkspaceStructure:
    case FrameKind::PaneListed:
    case FrameKind::ProportionalFaces:
        return true;
    case FrameKind::CheckpointHead:
    case FrameKind::TypographyFacts:
    case FrameKind::Update:
    case FrameKind::Resync:
    case FrameKind::View:
    case FrameKind::PaneDirty:
    case FrameKind::PanePalette:
    case FrameKind::HistoryInline:
    case FrameKind::HistoryHead:
    case FrameKind::HistoryReset:
    case FrameKind::HistoryWant:
    case FrameKind::PaneFacts:
    case FrameKind::StatusRecords:
    case FrameKind::Effect:
    case FrameKind::RichTextMeasure:
    case FrameKind::ClipboardRequest:
    case FrameKind::NativeDragCommand:
    case FrameKind::RichTextMeasured:
    case FrameKind::ClipboardData:
    case FrameKind::NativeDragEvent:
    case FrameKind::ImageHead:
    case FrameKind::ImageWant:
    case FrameKind::ImagePlacements:
    case FrameKind::SampleHead:
    case FrameKind::Receipt:
    case FrameKind::Keys:
    case FrameKind::Pointer:
    case FrameKind::Paste:
    case FrameKind::PasteAbort:
    case FrameKind::Raw:
    case FrameKind::Focus:
    case FrameKind::PasteReceipt:
    case FrameKind::InputDiscarded:
    case FrameKind::Viewport:
        return false;
    }
    return false;
}

bool appendFrame(std::vector<std::uint8_t>& output, FrameKind kind, std::uint32_t channel, std::span<const std::uint8_t> body)
{
    if (body.size() > maxFrameBodyBytes || (channel == 0) != isConnectionKind(kind))
        return false;
    WireWriter writer(output);
    writer.u8(static_cast<std::uint8_t>(kind));
    writer.varint(channel);
    writer.bytes(body);
    return true;
}

std::size_t frameBytes(std::uint32_t channel, std::size_t bodyBytes)
{
    const auto varintBytes = [](std::uint64_t value) {
        std::size_t count = 1;
        while (value >= 0x80) {
            value >>= 7;
            ++count;
        }
        return count;
    };
    return 1 + varintBytes(channel) + varintBytes(bodyBytes) + bodyBytes;
}

void FrameDecoder::append(std::span<const std::uint8_t> bytes)
{
    if (m_error != EnvelopeError::None)
        return;
    // Compacting only once the consumed prefix outweighs what remains keeps
    // the move amortised to one copy per byte.
    if (m_offset > 0 && m_offset >= m_buffer.size() - m_offset) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_offset));
        m_offset = 0;
    }
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
}

std::optional<Frame> FrameDecoder::next()
{
    if (m_error != EnvelopeError::None)
        return std::nullopt;
    const std::span<const std::uint8_t> pending = std::span<const std::uint8_t>(m_buffer).subspan(m_offset);
    Header header;
    const HeaderStatus status = parseHeader(pending, header, m_error);
    if (status != HeaderStatus::Complete || pending.size() - header.headerBytes < header.length)
        return std::nullopt;
    m_offset += header.headerBytes + header.length;
    return Frame { .kind = header.kind, .channel = header.channel, .body = pending.subspan(header.headerBytes, header.length) };
}

std::optional<Frame> decodeDatagram(std::span<const std::uint8_t> datagram, EnvelopeError& error)
{
    error = EnvelopeError::None;
    Header header;
    const HeaderStatus status = parseHeader(datagram, header, error);
    if (status == HeaderStatus::Failed)
        return std::nullopt;
    if (status == HeaderStatus::NeedsMore || datagram.size() - header.headerBytes != header.length) {
        error = EnvelopeError::Malformed;
        return std::nullopt;
    }
    return Frame { .kind = header.kind, .channel = header.channel, .body = datagram.subspan(header.headerBytes, header.length) };
}

} // namespace Ubiquitty::Protocol::Sync
