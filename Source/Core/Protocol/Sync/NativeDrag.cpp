// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/NativeDrag.h"
#include "Core/Protocol/Sync/Wire.h"

#include <bit>
#include <limits>

namespace Ubiquitty::Protocol::Sync {
namespace {
bool isInteger(std::int64_t value)
{
    return value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::uint32_t>::max();
}

void writeMetadata(WireWriter& writer, const DragDrop::Metadata& value)
{
    writer.u8(static_cast<std::uint8_t>(value.type));
    std::uint16_t flags = 0;
    unsigned shift = 0;
    for (const auto isPresent : { value.x.has_value(), value.y.has_value(), value.pixelX.has_value(), value.pixelY.has_value(),
             value.identity.has_value(), value.operation.has_value(), value.hasType, value.hasMore, value.hasMoreChunks })
        flags |= static_cast<std::uint16_t>(static_cast<unsigned>(isPresent) << shift++);
    writer.u16(flags);
    for (const auto& field : { value.x, value.y, value.pixelX, value.pixelY }) {
        if (field)
            writer.u64(std::bit_cast<std::uint64_t>(*field));
    }
    if (value.identity)
        writer.u32(*value.identity);
    if (value.operation)
        writer.u32(*value.operation);
}

DragDrop::Metadata readMetadata(WireReader& reader)
{
    DragDrop::Metadata value;
    const auto type = static_cast<char>(reader.u8());
    if (std::string_view("aAmMrRopPeEkqQ").find(type) == std::string_view::npos)
        reader.fail();
    value.type = static_cast<DragDrop::Type>(type);
    const auto flags = reader.u16();
    if (flags & ~0x1ffu)
        reader.fail();
    unsigned shift = 0;
    for (auto* field : { &value.x, &value.y, &value.pixelX, &value.pixelY }) {
        if (flags & (1u << shift)) {
            *field = std::bit_cast<std::int64_t>(reader.u64());
            if (!isInteger(**field))
                reader.fail();
        }
        ++shift;
    }
    if (flags & 16)
        value.identity = reader.u32();
    if (flags & 32)
        value.operation = reader.u32();
    value.hasType = flags & 64;
    value.hasMore = flags & 128;
    value.hasMoreChunks = flags & 256;
    return value;
}

void writeBytes(WireWriter& writer, std::span<const char> bytes)
{
    writer.bytes({ reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
}

bool readBytes(WireReader& reader, FallibleVector<char>& destination, std::size_t limit)
{
    const auto bytes = reader.bytes(limit);
    return reader.ok() && destination.tryAssign(bytes.begin(), bytes.end()).has_value();
}
}

bool encodeNativeDragCommand(std::vector<std::uint8_t>& bytes, std::uint64_t epoch, const DragDrop::Command& command)
{
    if (!epoch || command.kind > DragDrop::CommandKind::CancelDrag
        || std::string_view("aAmMrRopPeEkqQ").find(static_cast<char>(command.metadata.type)) == std::string_view::npos
        || command.payload.size() > DragDrop::maxMIMEListBytes)
        return false;
    for (const auto field : { command.metadata.x, command.metadata.y, command.metadata.pixelX, command.metadata.pixelY }) {
        if (field && !isInteger(*field))
            return false;
    }
    WireWriter writer(bytes);
    writer.u64(epoch);
    writer.u8(static_cast<std::uint8_t>(command.kind));
    writer.u64(command.sessionID);
    writer.u64(command.requestID);
    writer.u8(command.isFinal ? 1 : 0);
    writeMetadata(writer, command.metadata);
    writeBytes(writer, std::span(command.payload.data(), command.payload.size()));
    return true;
}

std::optional<NativeDragCommand> decodeNativeDragCommand(std::span<const std::uint8_t> bytes)
{
    WireReader reader(bytes);
    NativeDragCommand result;
    result.epoch = reader.u64();
    auto& command = result.command;
    command.kind = static_cast<DragDrop::CommandKind>(reader.u8());
    command.sessionID = reader.u64();
    command.requestID = reader.u64();
    const auto final = reader.u8();
    command.isFinal = final != 0;
    command.metadata = readMetadata(reader);
    if (!result.epoch || command.kind > DragDrop::CommandKind::CancelDrag || final > 1
        || !readBytes(reader, command.bytes, DragDrop::maxMIMEListBytes) || !reader.isAtEnd())
        return std::nullopt;
    command.isPresent = true;
    return result;
}

bool encodeNativeDragEvent(std::vector<std::uint8_t>& bytes, std::uint64_t epoch, const DragDrop::NativeEvent& event, std::span<const char> payload)
{
    if (!epoch || (!event.sessionID && event.kind != DragDrop::EventKind::Cancel) || event.kind > DragDrop::EventKind::Cancel
        || event.error > DragDrop::Error::Unknown
        || payload.size() > (event.kind == DragDrop::EventKind::ReadData ? 3072 : DragDrop::maxMIMEListBytes))
        return false;
    for (const auto field : { event.x, event.y, event.pixelX, event.pixelY }) {
        if (!isInteger(field))
            return false;
    }
    WireWriter writer(bytes);
    writer.u64(epoch);
    writer.u8(static_cast<std::uint8_t>(event.kind));
    writer.u64(event.sessionID);
    writer.u64(event.requestID);
    for (const auto value : { event.x, event.y, event.pixelX, event.pixelY })
        writer.u64(std::bit_cast<std::uint64_t>(value));
    writer.u32(event.operation);
    writer.u8(static_cast<std::uint8_t>(event.error));
    writer.u8(static_cast<std::uint8_t>((event.isFinal ? 1 : 0) | (event.isSameWindow ? 2 : 0)));
    writeBytes(writer, payload);
    return true;
}

std::optional<NativeDragEvent> decodeNativeDragEvent(std::span<const std::uint8_t> bytes)
{
    WireReader reader(bytes);
    NativeDragEvent result;
    result.epoch = reader.u64();
    auto& event = result.event;
    event.kind = static_cast<DragDrop::EventKind>(reader.u8());
    event.sessionID = reader.u64();
    event.requestID = reader.u64();
    for (auto* value : { &event.x, &event.y, &event.pixelX, &event.pixelY }) {
        *value = std::bit_cast<std::int64_t>(reader.u64());
        if (!isInteger(*value))
            reader.fail();
    }
    event.operation = reader.u32();
    event.error = static_cast<DragDrop::Error>(reader.u8());
    const auto flags = reader.u8();
    event.isFinal = flags & 1;
    event.isSameWindow = flags & 2;
    if (!result.epoch || (!event.sessionID && event.kind != DragDrop::EventKind::Cancel) || event.kind > DragDrop::EventKind::Cancel
        || event.error > DragDrop::Error::Unknown || flags > 3
        || !readBytes(reader, result.bytes, event.kind == DragDrop::EventKind::ReadData ? 3072 : DragDrop::maxMIMEListBytes)
        || !reader.isAtEnd())
        return std::nullopt;
    return result;
}

} // namespace Ubiquitty::Protocol::Sync
