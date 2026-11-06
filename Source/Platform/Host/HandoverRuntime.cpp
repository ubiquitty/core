// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/HandoverRuntime.h"

#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/Wire.h"

#include <bit>

namespace Ubiquitty::Host {
namespace {

namespace Sync = Protocol::Sync;
constexpr std::uint16_t version = 2;
constexpr std::size_t coherenceBytes = 3 * 8 + 2 + 4 * 8 + 4 * 4 + 2 * 4 + 2 + 6;
constexpr std::size_t pasteBytes = 2 * 16 + 1 + 1 + 5 + 8 + 4;
constexpr std::size_t fixedBytes = 2 + coherenceBytes + 1 + 16;

bool readBoolean(Sync::WireReader& reader)
{
    const auto value = reader.u8();
    if (value > 1)
        reader.fail();
    return value != 0;
}

void writeCoherence(Sync::WireWriter& writer, const Runtime::FrameCoherence::Continuation& state)
{
    writer.u64(state.tuning.deadlineMicroseconds);
    writer.u64(state.tuning.quiescenceMicroseconds);
    writer.u64(state.tuning.frameIntervalMicroseconds);
    writer.u8(state.tuning.isPaced);
    writer.u8(state.tuning.isPersistentBracketEnabled);
    writer.u64(state.dirtySinceMicroseconds);
    writer.u64(state.lastPassEndMicroseconds);
    writer.u64(state.lastFrameMicroseconds);
    writer.u64(state.bracketOpenedMicroseconds);
    writer.u32(state.bracketHideCount);
    writer.u32(state.cursorHideCountAtLastFrame);
    writer.u32(state.syncCloseCountAtLastFrame);
    writer.u32(state.syncCloseCountAtLastPass);
    writer.u32(std::bit_cast<std::uint32_t>(state.frameRows));
    writer.u32(std::bit_cast<std::uint32_t>(state.frameColumns));
    writer.u8(std::to_underlying(state.bracket));
    writer.u8(std::to_underlying(state.lastPassEnd));
    writer.u8(state.isDirty);
    writer.u8(state.hasTakenFrame);
    writer.u8(state.hasPassEnded);
    writer.u8(state.wasCursorVisibleAtLastFrame);
    writer.u8(state.wasComposingAtLastPass);
    writer.u8(state.didExpireSinceLastFrame);
}

void readCoherence(Sync::WireReader& reader, Runtime::FrameCoherence::Continuation& state)
{
    state.tuning.deadlineMicroseconds = reader.u64();
    state.tuning.quiescenceMicroseconds = reader.u64();
    state.tuning.frameIntervalMicroseconds = reader.u64();
    state.tuning.isPaced = readBoolean(reader);
    state.tuning.isPersistentBracketEnabled = readBoolean(reader);
    state.dirtySinceMicroseconds = reader.u64();
    state.lastPassEndMicroseconds = reader.u64();
    state.lastFrameMicroseconds = reader.u64();
    state.bracketOpenedMicroseconds = reader.u64();
    state.bracketHideCount = reader.u32();
    state.cursorHideCountAtLastFrame = reader.u32();
    state.syncCloseCountAtLastFrame = reader.u32();
    state.syncCloseCountAtLastPass = reader.u32();
    state.frameRows = std::bit_cast<std::int32_t>(reader.u32());
    state.frameColumns = std::bit_cast<std::int32_t>(reader.u32());
    state.bracket = static_cast<Runtime::FrameCoherence::Bracket>(reader.u8());
    state.lastPassEnd = static_cast<Runtime::PassEnd>(reader.u8());
    state.isDirty = readBoolean(reader);
    state.hasTakenFrame = readBoolean(reader);
    state.hasPassEnded = readBoolean(reader);
    state.wasCursorVisibleAtLastFrame = readBoolean(reader);
    state.wasComposingAtLastPass = readBoolean(reader);
    state.didExpireSinceLastFrame = readBoolean(reader);
}

void writePaste(Sync::WireWriter& writer, const Runtime::RemoteInputEncoder::Continuation::Paste& paste)
{
    writer.u64(paste.clientInstance.low);
    writer.u64(paste.clientInstance.high);
    writer.u64(paste.authority.low);
    writer.u64(paste.authority.high);
    writer.u8(static_cast<std::uint8_t>(paste.normalizer.bracketedPaste)
        | (paste.normalizer.options.shouldQuoteCharacters ? 2 : 0)
        | (paste.normalizer.options.shouldPreserveNewlines ? 4 : 0));
    writer.u8(paste.normalizer.heldBytes);
    for (const auto byte : paste.normalizer.held)
        writer.u8(std::bit_cast<std::uint8_t>(byte));
    writer.u64(paste.lastWriteBytes);
    writer.u8(paste.isBracketOpen);
    writer.u8(paste.didOpen);
    writer.u8(paste.didClose);
    writer.u8(paste.hasEnded);
}

void readPaste(Sync::WireReader& reader, Runtime::RemoteInputEncoder::Continuation::Paste& paste)
{
    paste.clientInstance = { reader.u64(), reader.u64() };
    paste.authority = { reader.u64(), reader.u64() };
    const auto mode = reader.u8();
    if (mode > 7)
        reader.fail();
    paste.normalizer.bracketedPaste = static_cast<Input::BracketedPaste>(mode & 1);
    paste.normalizer.options.shouldQuoteCharacters = mode & 2;
    paste.normalizer.options.shouldPreserveNewlines = mode & 4;
    paste.normalizer.heldBytes = reader.u8();
    for (auto& byte : paste.normalizer.held)
        byte = std::bit_cast<char>(reader.u8());
    paste.lastWriteBytes = reader.u64();
    paste.isBracketOpen = readBoolean(reader);
    paste.didOpen = readBoolean(reader);
    paste.didClose = readBoolean(reader);
    paste.hasEnded = readBoolean(reader);
}

bool isValid(const HandoverRuntime& state)
{
    return Runtime::FrameCoherence::isValidContinuation(state.coherence)
        && Runtime::RemoteInputEncoder::isValidContinuation(state.input);
}

} // namespace

std::expected<std::vector<std::uint8_t>, HandoverRuntimeError> encodeHandoverRuntime(
    const HandoverRuntime& state, const HandoverRuntimeLimits& limits)
{
    if (state.input.pasteCount > state.input.pastes.size() || state.input.pasteCount > limits.maxPastes)
        return std::unexpected(HandoverRuntimeError::OverCapacity);
    const auto size = fixedBytes + pasteBytes * state.input.pasteCount;
    if (size > limits.maxSectionBytes)
        return std::unexpected(HandoverRuntimeError::OverCapacity);
    if (!isValid(state))
        return std::unexpected(HandoverRuntimeError::Malformed);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(size);
    Sync::WireWriter writer(bytes);
    writer.u16(version);
    writeCoherence(writer, state.coherence);
    writer.u8(state.input.pasteCount);
    for (std::size_t index = 0; index < state.input.pasteCount; ++index)
        writePaste(writer, state.input.pastes[index]);
    const auto digest = Sync::digestOf(bytes);
    writer.u64(digest.low);
    writer.u64(digest.high);
    return bytes;
}

std::expected<HandoverRuntime, HandoverRuntimeError> decodeHandoverRuntime(
    std::span<const std::uint8_t> bytes, const HandoverRuntimeLimits& limits)
{
    if (bytes.size() > limits.maxSectionBytes)
        return std::unexpected(HandoverRuntimeError::OverCapacity);
    Sync::WireReader reader(bytes);
    const auto carriedVersion = reader.u16();
    if (reader.ok() && carriedVersion > version)
        return std::unexpected(HandoverRuntimeError::NewerVersion);
    if (!reader.ok() || carriedVersion != version || bytes.size() < fixedBytes)
        return std::unexpected(HandoverRuntimeError::Malformed);
    // Admit the active prefix's exact fixed extent before constructing it.
    const auto count = bytes[2 + coherenceBytes];
    if (count > Sync::maxLastWrittenEntries || count > limits.maxPastes)
        return std::unexpected(HandoverRuntimeError::OverCapacity);
    if (bytes.size() != fixedBytes + pasteBytes * count)
        return std::unexpected(HandoverRuntimeError::Malformed);
    Sync::WireReader digestReader(bytes.last(16));
    const Sync::Digest128 digest { digestReader.u64(), digestReader.u64() };
    if (digest != Sync::digestOf(bytes.first(bytes.size() - 16)))
        return std::unexpected(HandoverRuntimeError::DigestMismatch);
    reader = Sync::WireReader(bytes.subspan(2, bytes.size() - 18));
    HandoverRuntime state;
    readCoherence(reader, state.coherence);
    state.input.pasteCount = reader.u8();
    for (std::size_t index = 0; index < state.input.pasteCount; ++index)
        readPaste(reader, state.input.pastes[index]);
    if (!reader.ok() || !reader.isAtEnd() || !isValid(state))
        return std::unexpected(HandoverRuntimeError::Malformed);
    return state;
}

} // namespace Ubiquitty::Host
