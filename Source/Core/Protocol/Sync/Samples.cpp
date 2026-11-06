// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Samples.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Lanes.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>

namespace Ubiquitty::Protocol::Sync {

namespace {

bool isValidSampleHead(const SampleHead& head)
{
    return head.slot >= 1 && head.slot <= VT::sampleSlotCount && isValidSampleIdentity(head.sample)
        && (head.sample.frames != 0) == (head.transfer != 0);
}

} // namespace

bool isValidSampleIdentity(const SampleIdentity& identity)
{
    return identity.frames <= VT::sampleMaxFrames
        && (identity.frames || std::ranges::all_of(identity.sha256, [](std::uint8_t byte) { return byte == 0; }));
}

bool encodeSampleHead(std::vector<std::uint8_t>& output, const SampleHead& head)
{
    if (!isValidSampleHead(head))
        return false;
    WireWriter writer(output);
    writer.u8(head.slot);
    writer.u32(head.sample.serial);
    writer.u32(head.sample.frames);
    writer.raw(head.sample.sha256);
    writer.u32(head.transfer);
    return true;
}

std::optional<SampleHead> decodeSampleHead(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != sampleHeadBytes)
        return std::nullopt;
    WireReader reader(bytes);
    SampleHead head;
    head.slot = reader.u8();
    head.sample.serial = reader.u32();
    head.sample.frames = reader.u32();
    for (auto& byte : head.sample.sha256)
        byte = reader.u8();
    head.transfer = reader.u32();
    return reader.ok() && reader.remaining() == 0 && isValidSampleHead(head) ? std::optional { head } : std::nullopt;
}

std::optional<SampleBytes> SampleBytes::capture(std::uint32_t serial, std::span<const std::int16_t> pcm, std::size_t availableBytes)
{
    if (pcm.size() > VT::sampleMaxFrames || pcm.size() * 2 > availableBytes)
        return std::nullopt;
    SampleBytes captured { .sample = { .serial = serial, .frames = static_cast<std::uint32_t>(pcm.size()), .sha256 = {} }, .bytes = {} };
    if (pcm.empty())
        return captured;
    captured.bytes.reset(new (std::nothrow) std::uint8_t[pcm.size() * 2]);
    if (!captured.bytes)
        return std::nullopt;
    for (std::size_t index = 0; index < pcm.size(); ++index) {
        const auto bits = std::bit_cast<std::uint16_t>(pcm[index]);
        captured.bytes[2 * index] = static_cast<std::uint8_t>(bits);
        captured.bytes[2 * index + 1] = static_cast<std::uint8_t>(bits >> 8);
    }
    Crypto::sha256(captured.view(), captured.sample.sha256);
    return captured;
}

std::optional<SampleTransfer> SampleTransfer::begin(const SampleHead& head, std::size_t availableBytes)
{
    if (!isValidSampleHead(head) || !head.transfer || static_cast<std::size_t>(head.sample.frames) * 2 > availableBytes)
        return std::nullopt;
    std::unique_ptr<std::int16_t[]> frames(new (std::nothrow) std::int16_t[head.sample.frames]);
    if (!frames)
        return std::nullopt;
    return SampleTransfer(head.transfer, { .sample = head.sample, .frames = std::move(frames) });
}

SampleChunkResult SampleTransfer::accept(const BulkChunk& chunk)
{
    const auto refuse = [&] {
        m_pcm.frames.reset();
        return SampleChunkResult::Refused;
    };
    if (!m_pcm.frames || m_isComplete || chunk.transfer != m_transfer || chunk.index != m_nextIndex
        || chunk.bytes.empty() || chunk.bytes.size() > bulkChunkPayloadBytes
        || chunk.compression != Compression::None || chunk.inflatedBytes
        || chunk.bytes.size() > heldBytes() - m_received)
        return refuse();
    auto* bytes = reinterpret_cast<std::uint8_t*>(m_pcm.frames.get());
    std::memcpy(bytes + m_received, chunk.bytes.data(), chunk.bytes.size());
    m_received += chunk.bytes.size();
    ++m_nextIndex;
    if (!chunk.isLast)
        return m_received < heldBytes() ? SampleChunkResult::Pending : refuse();
    if (m_received != heldBytes())
        return refuse();
    std::array<std::uint8_t, 32> digest {};
    Crypto::sha256({ bytes, m_received }, digest);
    if (digest != chunk.sha256 || digest != m_pcm.sample.sha256)
        return refuse();
    if constexpr (std::endian::native == std::endian::big) {
        for (std::size_t index = 0; index < m_pcm.sample.frames; ++index)
            m_pcm.frames[index] = std::bit_cast<std::int16_t>(std::byteswap(std::bit_cast<std::uint16_t>(m_pcm.frames[index])));
    }
    m_isComplete = true;
    return SampleChunkResult::Complete;
}

std::optional<SamplePCM> SampleTransfer::take()
{
    if (!m_isComplete || !m_pcm.frames)
        return std::nullopt;
    return std::move(m_pcm);
}

} // namespace Ubiquitty::Protocol::Sync
