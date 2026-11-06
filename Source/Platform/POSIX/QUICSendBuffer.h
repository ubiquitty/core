// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Platform/POSIX/QUICSendBuffer.h — one QUIC stream's outbound bytes,
// held at a fixed address until the peer acknowledges them.
//
// ngtcp2 does not copy stream data. ngtcp2_conn_writev_stream keeps pointers to
// the bytes it took, and its contract is that the caller keeps them "in tact
// until acked_stream_data_offset indicates that they are acknowledged". A lost
// packet is rebuilt from those pointers. A buffer that can move its bytes — a
// vector that reallocates on append, or erases its acknowledged prefix by
// shifting the rest down — leaves ngtcp2 reading whichever bytes occupy those addresses, and
// the peer receives a retransmission whose payload is not what that offset
// carried.
//
// So the bytes live in fixed-size blocks that never move. Appending fills the
// last block and starts another; an acknowledgement frees whole blocks behind
// it. Both are constant time per block, so draining a backlog costs the same per
// byte whether eight kibibytes or eight mebibytes are queued.
//
// Three stream offsets partition the buffer, in order:
//
//   acknowledged <= written <= end
//
// [acknowledged, written) is in flight: ngtcp2 holds pointers into it.
// [written, end) is queued and not yet handed to ngtcp2. Only the second is a
// backpressure signal; the first is the congestion controller's to govern.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <span>

namespace Ubiquitty::POSIX {

/// One QUIC stream's outbound bytes, at fixed addresses until acknowledged.
///
/// Not thread-safe: one owner appends, writes and acknowledges, the same thread
/// that drives the connection.
class QUICSendBuffer {
public:
    /// The size of one block.
    ///
    /// A keystroke echo occupies one block, so this is the floor of what an idle
    /// connection holds; a keyframe spans a few dozen. A block boundary costs one
    /// extra vector in a packet's gather, never a short packet.
    static constexpr std::size_t blockBytes = 16 * 1024;

    /// Appends bytes at the stream's end.
    ///
    /// Bytes already held stay where they are; only the last block is written.
    ///
    /// - Parameter bytes: The bytes to queue, copied.
    void append(std::span<const std::uint8_t> bytes)
    {
        while (!bytes.empty()) {
            const std::size_t used = m_blocks.empty() ? blockBytes : static_cast<std::size_t>(m_endOffset - m_frontOffset) - (m_blocks.size() - 1) * blockBytes;
            if (used == blockBytes) {
                m_blocks.push_back(takeBlock());
                continue;
            }
            const std::size_t take = std::min(bytes.size(), blockBytes - used);
            std::memcpy(m_blocks.back().get() + used, bytes.data(), take);
            m_endOffset += take;
            bytes = bytes.subspan(take);
        }
    }

    /// Fills `out` with the queued bytes from the write cursor, in stream order.
    ///
    /// Each span is contiguous within one block; a gather of two covers any
    /// packet, since a packet is smaller than a block.
    ///
    /// - Parameter out: Where the spans go; as many are filled as fit.
    /// - Returns: How many spans were filled.
    std::size_t unsentSpans(std::span<std::span<const std::uint8_t>> out) const
    {
        std::size_t count = 0;
        std::uint64_t offset = m_writtenOffset;
        while (count < out.size() && offset < m_endOffset) {
            const std::size_t relative = static_cast<std::size_t>(offset - m_frontOffset);
            const std::size_t block = relative / blockBytes;
            const std::size_t within = relative % blockBytes;
            assert(block < m_blocks.size());
            const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(blockBytes - within, m_endOffset - offset));
            out[count++] = { m_blocks[block].get() + within, length };
            offset += length;
        }
        return count;
    }

    /// Records that ngtcp2 took `bytes` from the write cursor.
    ///
    /// Moves the cursor only: the bytes stay until they are acknowledged.
    ///
    /// - Parameter bytes: How many bytes ngtcp2 took; clamped to what is queued.
    void markWritten(std::size_t bytes) { m_writtenOffset = std::min<std::uint64_t>(m_writtenOffset + bytes, m_endOffset); }

    /// Releases everything below a stream offset the peer has acknowledged.
    ///
    /// ngtcp2 reports acknowledged ranges in increasing order without overlap,
    /// so the acknowledged bytes are always a prefix. Only whole blocks are freed:
    /// a partly acknowledged block still holds bytes ngtcp2 may resend.
    ///
    /// - Parameter acknowledgedThrough: The stream offset below which every byte
    ///   is acknowledged; clamped to what was handed over.
    void acknowledge(std::uint64_t acknowledgedThrough)
    {
        // Never past what was handed over: a peer cannot acknowledge bytes it was
        // never sent, and the clamp keeps the cursors ordered whatever it claims.
        acknowledgedThrough = std::min(acknowledgedThrough, m_writtenOffset);
        if (acknowledgedThrough <= m_acknowledgedOffset)
            return;
        m_acknowledgedOffset = acknowledgedThrough;
        while (!m_blocks.empty() && m_frontOffset + blockBytes <= m_acknowledgedOffset) {
            if (!m_spare)
                m_spare = std::move(m_blocks.front());
            m_blocks.pop_front();
            m_frontOffset += blockBytes;
        }
        // Everything acknowledged and nothing queued: the partly filled last block
        // has no byte anyone needs, so the next append can start a fresh one at
        // the stream's end rather than keep filling it.
        if (m_acknowledgedOffset == m_endOffset && m_blocks.size() == 1) {
            if (!m_spare)
                m_spare = std::move(m_blocks.front());
            m_blocks.clear();
        }
        if (m_blocks.empty())
            m_frontOffset = m_endOffset;
    }

    /// Bytes queued and not yet handed to ngtcp2.
    [[nodiscard]] std::size_t unsentBytes() const { return static_cast<std::size_t>(m_endOffset - m_writtenOffset); }
    /// Bytes handed to ngtcp2 and not yet acknowledged.
    [[nodiscard]] std::size_t unacknowledgedBytes() const { return static_cast<std::size_t>(m_writtenOffset - m_acknowledgedOffset); }
    /// Every byte the buffer is obliged to hold: queued or in flight.
    [[nodiscard]] std::size_t retainedBytes() const { return static_cast<std::size_t>(m_endOffset - m_acknowledgedOffset); }
    /// The memory the blocks occupy, the spare included.
    [[nodiscard]] std::size_t storageBytes() const { return (m_blocks.size() + (m_spare ? 1 : 0)) * blockBytes; }
    /// Additional fixed-block capacity append would allocate. Acknowledged
    /// prefixes in a live block are not reusable; the spare and last block's
    /// suffix are. Saturates for a request whose rounded capacity cannot fit.
    [[nodiscard]] std::size_t additionalStorageBytes(std::size_t bytes) const
    {
        const auto available = storageBytes() - static_cast<std::size_t>(m_endOffset - m_frontOffset);
        if (bytes <= available)
            return 0;
        const auto missing = bytes - available;
        const auto blocks = missing / blockBytes + (missing % blockBytes != 0 ? 1 : 0);
        return blocks > std::numeric_limits<std::size_t>::max() / blockBytes ? std::numeric_limits<std::size_t>::max() : blocks * blockBytes;
    }
    /// Whether any queued byte has not been handed to ngtcp2.
    [[nodiscard]] bool hasUnsent() const { return m_writtenOffset < m_endOffset; }

private:
    std::unique_ptr<std::uint8_t[]> takeBlock()
    {
        if (m_spare)
            return std::move(m_spare);
        return std::make_unique_for_overwrite<std::uint8_t[]>(blockBytes);
    }

    std::deque<std::unique_ptr<std::uint8_t[]>> m_blocks;
    // One freed block kept back, so a connection streaming steadily recycles
    // its blocks rather than returning one to the allocator per sixteen
    // kibibytes and asking for it again.
    std::unique_ptr<std::uint8_t[]> m_spare;
    // The stream offset of the first byte of m_blocks.front().
    std::uint64_t m_frontOffset = 0;
    std::uint64_t m_acknowledgedOffset = 0;
    std::uint64_t m_writtenOffset = 0;
    std::uint64_t m_endOffset = 0;
};

} // namespace Ubiquitty::POSIX
