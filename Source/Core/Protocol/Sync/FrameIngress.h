// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Envelope.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>

namespace Ubiquitty::Protocol::Sync {

/// What a framing reader knows from the header bytes received so far.
struct FrameHeaderDecision {
    /// Whether the header is complete and its body may be allocated.
    enum class State : std::uint8_t {
        NeedsMore,
        Admitted,
        Refused,
    };

    State state = State::NeedsMore;
    std::size_t bodyBytes = 0;
};

/// Whether to read another frame after delivering the current one.
enum class FrameDelivery : std::uint8_t {
    Continue,
    Stop,
};

/// Why ingress stopped before it could deliver a frame.
enum class FrameIngressError : std::uint8_t {
    None,
    HeaderRefused,
    HeaderTooLong,
    Oversize,
    AllocationFailed,
};

/// A header-first byte consumer that holds at most one admitted frame body.
///
/// It knows no wire grammar or authority. Its owner reads a header and admits
/// its length before any body is allocated, then handles each complete frame
/// before the next header is read. A handshake may therefore change what the
/// next frame is allowed, even when both arrived in the same carrier read.
class FrameIngress {
public:
    /// Whether ingress accepts bytes, awaits reset, or has latched a failure.
    enum class State : std::uint8_t {
        Accepting,
        Stopped,
        Refused,
    };

    /// Exactly how much of a caller's span was consumed.
    struct Result {
        std::size_t consumedBytes = 0;
        State state = State::Accepting;
    };

    /// Makes an ingress with an absolute maximum body size.
    ///
    /// - Parameter maxBodyBytes: The largest body the owner can ever admit.
    ///   Its header reader may apply a smaller limit before a handshake.
    explicit FrameIngress(std::size_t maxBodyBytes)
        : m_maxBodyBytes(maxBodyBytes)
    {
    }

    /// Reads headers and delivers complete frames, in order.
    ///
    /// - Parameters:
    ///   - bytes: The next carrier bytes. The unconsumed suffix remains the
    ///     caller's when this returns Stopped or Refused.
    ///   - readHeader: Called after each header byte with the whole header so
    ///     far; returns FrameHeaderDecision. It must admit or refuse by
    ///     maxEnvelopeHeaderBytes. No body bytes have been copied at that point.
    ///   - deliver: Called with header and body spans, borrowed only until the
    ///     callback returns. Returns FrameDelivery; Stop stays stopped until
    ///     reset. Neither callback may re-enter or reset this ingress.
    /// - Returns: The exact consumed prefix and the resulting state. Accepting
    ///   consumes the entire span, including a trailing partial frame.
    template<typename ReadHeader, typename Deliver>
    [[nodiscard]] Result consume(std::span<const std::uint8_t> bytes, ReadHeader&& readHeader, Deliver&& deliver)
    {
        Result result { .state = m_state };
        if (m_state != State::Accepting)
            return result;
        while (result.consumedBytes < bytes.size()) {
            if (!m_bodyLength) {
                m_header[m_headerBytes++] = bytes[result.consumedBytes++];
                const FrameHeaderDecision decision = readHeader(std::span<const std::uint8_t> { m_header }.first(m_headerBytes));
                if (decision.state == FrameHeaderDecision::State::Refused) {
                    refuse(FrameIngressError::HeaderRefused);
                    break;
                }
                if (decision.state == FrameHeaderDecision::State::NeedsMore) {
                    if (m_headerBytes == m_header.size()) {
                        refuse(FrameIngressError::HeaderTooLong);
                        break;
                    }
                    continue;
                }
                if (decision.bodyBytes > m_maxBodyBytes) {
                    refuse(FrameIngressError::Oversize);
                    break;
                }
                m_bodyLength = decision.bodyBytes;
                if (decision.bodyBytes > m_bodyCapacity) {
                    // Release first: growing never retains the old allocation
                    // beside the new one, even transiently.
                    m_body.reset();
                    m_bodyCapacity = 0;
                    m_body.reset(new (std::nothrow) std::uint8_t[decision.bodyBytes]);
                    if (!m_body) {
                        refuse(FrameIngressError::AllocationFailed);
                        break;
                    }
                    m_bodyCapacity = decision.bodyBytes;
                }
            }
            const std::size_t count = std::min(*m_bodyLength - m_bodyBytes, bytes.size() - result.consumedBytes);
            if (count) {
                std::copy_n(bytes.data() + result.consumedBytes, count, m_body.get() + m_bodyBytes);
                m_bodyBytes += count;
                result.consumedBytes += count;
            }
            if (m_bodyBytes != *m_bodyLength)
                continue;
            const FrameDelivery decision = deliver(std::span<const std::uint8_t> { m_header }.first(m_headerBytes), std::span<const std::uint8_t> { m_body.get(), m_bodyBytes });
            m_headerBytes = 0;
            m_bodyBytes = 0;
            m_bodyLength.reset();
            if (decision == FrameDelivery::Stop) {
                m_state = State::Stopped;
                releaseBody();
                break;
            }
        }
        result.state = m_state;
        return result;
    }

    /// The body allocation retained, including bytes not yet filled.
    /// The fixed header uses no heap storage.
    [[nodiscard]] std::size_t retainedBytes() const { return m_bodyCapacity; }
    /// Header and body bytes held but not delivered.
    [[nodiscard]] std::size_t pendingBytes() const { return m_headerBytes + m_bodyBytes; }
    /// The latched ingress failure, or None before one.
    [[nodiscard]] FrameIngressError error() const { return m_error; }

    /// Returns idle storage after a read has finished, never during delivery.
    void releaseRetained()
    {
        if (pendingBytes() == 0)
            releaseBody();
    }

    /// Releases partial and retained storage and starts reading anew.
    void reset()
    {
        releaseBody();
        m_headerBytes = 0;
        m_state = State::Accepting;
        m_error = FrameIngressError::None;
    }

private:
    /// Releases body storage without changing header or acceptance state.
    void releaseBody()
    {
        m_body.reset();
        m_bodyCapacity = 0;
        m_bodyLength.reset();
        m_bodyBytes = 0;
    }

    /// Discards the incomplete frame and latches a failure until reset.
    ///
    /// - Parameter error: The reason further input must be refused.
    void refuse(FrameIngressError error)
    {
        releaseBody();
        m_headerBytes = 0;
        m_state = State::Refused;
        m_error = error;
    }

    const std::size_t m_maxBodyBytes;
    std::array<std::uint8_t, maxEnvelopeHeaderBytes> m_header {};
    std::size_t m_headerBytes = 0;
    std::unique_ptr<std::uint8_t[]> m_body;
    std::size_t m_bodyCapacity = 0;
    std::size_t m_bodyBytes = 0;
    std::optional<std::size_t> m_bodyLength;
    State m_state = State::Accepting;
    FrameIngressError m_error = FrameIngressError::None;
};

} // namespace Ubiquitty::Protocol::Sync
