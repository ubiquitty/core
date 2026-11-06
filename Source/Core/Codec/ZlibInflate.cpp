// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Codec::Zlib::Inflater: thin adapter over the vendored Wuffs zlib decoder. Maps wuffs
// status ok → Done, short_read → NeedMoreInput, short_write → NeedMoreOutput,
// else Error. Each call's spans are positioned via meta.pos so deflate
// back-reference distance validates against the total history even when the
// caller switches output spans. src is never marked closed (no end-of-input
// signal), so a truncated stream keeps answering NeedMoreInput. Checksum
// verification stays ENABLED, so Done means the adler32 trailer matched. Done
// and Error are absorbing; reset() returns to the fresh-stream state.

#include "Core/Codec/ZlibInflate.h"

#include "Core/Codec/WuffsCodec.h"

namespace Ubiquitty::Codec::Zlib {

static_assert(Inflater::codecStorageCapacity == Ubiquitty::wuffsCodecStorageBytes, "Inflater storage must match the shared wuffs codec storage capacity");

void Inflater::reset()
{
    m_totalConsumed = 0;
    m_totalProduced = 0;
    m_streamComplete = false;
    m_usable = false;
    // Runtime sizeof__ is the real size (the compile-time sizeof is poisoned
    // outside the wuffs impl TU). Fails closed (decode() reports Error) if the
    // storage does not fit.
    if (sizeof__wuffs_zlib__decoder() > codecStorageCapacity)
        return;
    wuffs_zlib__decoder* decoder = reinterpret_cast<wuffs_zlib__decoder*>(m_storage);
    const wuffs_base__status status = wuffs_zlib__decoder__initialize(decoder, sizeof__wuffs_zlib__decoder(), WUFFS_VERSION, WUFFS_INITIALIZE__LEAVE_INTERNAL_BUFFERS_UNINITIALIZED);
    m_usable = wuffs_base__status__is_ok(&status);
}

Inflater::DecodeResult Inflater::decode(std::span<const uint8_t> input, std::span<uint8_t> output)
{
    if (m_streamComplete)
        return { InflateStatus::Done, 0, 0 };
    if (!m_usable)
        return { InflateStatus::Error, 0, 0 };

    wuffs_zlib__decoder* decoder = reinterpret_cast<wuffs_zlib__decoder*>(m_storage);

    // Spans positioned at their cumulative stream offsets. `false` = src not
    // closed, so exhausting input is always NeedMoreInput, never a truncation error.
    wuffs_base__io_buffer source = wuffs_base__ptr_u8__reader(const_cast<uint8_t*>(input.data()), input.size(), false);
    source.meta.pos = m_totalConsumed;
    wuffs_base__io_buffer destination = wuffs_base__ptr_u8__writer(output.data(), output.size());
    destination.meta.pos = m_totalProduced;

    // wuffs_zlib's workbuf_len is a constant 1 byte — an interface formality.
    uint8_t workbuf[1];
    const wuffs_base__status status = wuffs_zlib__decoder__transform_io(decoder, &destination, &source, wuffs_base__make_slice_u8(workbuf, sizeof(workbuf)));

    const std::size_t consumed = source.meta.ri;
    const std::size_t produced = destination.meta.wi;
    m_totalConsumed += consumed;
    m_totalProduced += produced;

    if (wuffs_base__status__is_ok(&status)) {
        // Completing means the deflate stream ended and the adler32 trailer matched.
        m_streamComplete = true;
        return { InflateStatus::Done, consumed, produced };
    }
    if (status.repr == wuffs_base__suspension__short_read)
        return { InflateStatus::NeedMoreInput, consumed, produced };
    if (status.repr == wuffs_base__suspension__short_write)
        return { InflateStatus::NeedMoreOutput, consumed, produced };
    m_usable = false; // errors are absorbing until reset()
    return { InflateStatus::Error, consumed, produced };
}

std::span<uint8_t> Inflater::claimCodecStorage()
{
    // The claimant overwrites the zlib decoder in place; latch this object
    // unusable so a stale decode() cannot run over the foreign struct (both
    // decoders start with a valid wuffs magic, so wuffs' own guard wouldn't catch it).
    m_streamComplete = false;
    m_usable = false;
    return { m_storage, codecStorageCapacity };
}

bool inflateExact(std::span<const uint8_t> input, std::span<uint8_t> output)
{
    Inflater inflater;
    std::size_t inputOffset = 0;
    std::size_t outputOffset = 0;
    for (;;) {
        const Inflater::DecodeResult result = inflater.decode(input.subspan(inputOffset), output.subspan(outputOffset));
        inputOffset += result.bytesConsumed;
        outputOffset += result.bytesProduced;
        if (result.status == InflateStatus::Done)
            return outputOffset == output.size();
        if (result.status == InflateStatus::Error)
            return false;
        // No forward progress means truncated input or an output longer than
        // the caller's expected length — either way, reject.
        if (result.bytesConsumed == 0 && result.bytesProduced == 0)
            return false;
    }
}

} // namespace Ubiquitty::Codec::Zlib
