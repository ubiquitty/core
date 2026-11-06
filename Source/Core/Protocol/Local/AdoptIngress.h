// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Sync/FrameIngress.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>

namespace Ubiquitty::Protocol::Local {

/// Owns one header-admitted complete local adoption record at a time.
/// Framing belongs to AdoptWire; transport authority and descriptor ownership
/// belong to the caller. No terminal payload is decoded or hashed here.
class AdoptIngress {
public:
    /// Borrows the exact contiguous header and body during delivery only.
    struct Frame {
        AdoptWireMessage kind;
        std::span<const std::uint8_t> record;
    };

    /// Sets the independent allowance for a complete record, including its header.
    explicit AdoptIngress(std::size_t maxRecordBytes)
        : m_maxRecordBytes(maxRecordBytes)
    {
    }

    AdoptIngress(const AdoptIngress&) = delete;
    AdoptIngress& operator=(const AdoptIngress&) = delete;
    AdoptIngress(AdoptIngress&&) = delete;
    AdoptIngress& operator=(AdoptIngress&&) = delete;

    /// Admits framing before retaining any body bytes or allocating a record.
    ///
    /// Admit receives the kind, body length, and actual required allocation
    /// capacity, including a larger idle owner reused for a shorter record.
    /// Deliver receives one whole record and may stop before a coalesced suffix.
    /// Neither callback may re-enter, destroy, or replace this ingress. A Stop
    /// or refusal stays latched; a new exchange needs a new ingress owner.
    /// - Parameters:
    ///   - bytes: The next input span; an unconsumed suffix remains the caller's.
    ///   - admit: Returns whether this record and its capacity may be retained.
    ///   - deliver: Consumes a synchronous borrow and returns Continue or Stop.
    /// - Returns: The exact consumed prefix and acceptance state.
    template<typename Admit, typename Deliver>
    [[nodiscard]] Sync::FrameIngress::Result consume(std::span<const std::uint8_t> bytes, Admit&& admit, Deliver&& deliver)
    {
        using State = Sync::FrameIngress::State;
        Sync::FrameIngress::Result result { .state = m_state };
        if (m_state != State::Accepting)
            return result;
        while (result.consumedBytes < bytes.size()) {
            if (!m_recordLength) {
                const auto count = std::min(m_header.size() - m_headerBytes, bytes.size() - result.consumedBytes);
                std::copy_n(bytes.data() + result.consumedBytes, count, m_header.data() + m_headerBytes);
                m_headerBytes += count;
                result.consumedBytes += count;
                if (m_headerBytes != m_header.size())
                    break;
                const auto header = decodeAdoptWireHeader(m_header, m_maxRecordBytes);
                if (!header) {
                    refuse(header.error() == AdoptWireError::TooLong ? Sync::FrameIngressError::Oversize : Sync::FrameIngressError::HeaderRefused);
                    break;
                }
                // The authoritative decoder proved this whole-record sum fits
                // size_t and the independent allowance before this conversion.
                const auto recordBytes = static_cast<std::size_t>(std::uint64_t { header->bodyBytes } + adoptWireFrameHeaderBytes);
                const auto requiredCapacity = std::max(m_recordCapacity, recordBytes);
                if (!admit(header->kind, static_cast<std::size_t>(header->bodyBytes), requiredCapacity)) {
                    refuse(Sync::FrameIngressError::HeaderRefused);
                    break;
                }
                if (recordBytes > m_recordCapacity) {
                    // Growth has no old/new owner overlap, including failure.
                    releaseRecord();
                    m_record.reset(new (std::nothrow) std::uint8_t[recordBytes]);
                    if (!m_record) {
                        refuse(Sync::FrameIngressError::AllocationFailed);
                        break;
                    }
                    m_recordCapacity = recordBytes;
                }
                m_kind = header->kind;
                m_recordLength = recordBytes;
                std::ranges::copy(m_header, m_record.get());
            }
            const auto count = std::min(m_recordLength - m_headerBytes - m_bodyBytes, bytes.size() - result.consumedBytes);
            if (count) {
                std::copy_n(bytes.data() + result.consumedBytes, count, m_record.get() + m_headerBytes + m_bodyBytes);
                m_bodyBytes += count;
                result.consumedBytes += count;
            }
            if (m_headerBytes + m_bodyBytes != m_recordLength)
                continue;
            const auto decision = deliver(Frame { m_kind, { m_record.get(), m_recordLength } });
            m_headerBytes = 0;
            m_bodyBytes = 0;
            m_recordLength = 0;
            if (decision == Sync::FrameDelivery::Stop) {
                m_state = State::Stopped;
                releaseRecord();
                break;
            }
        }
        result.state = m_state;
        return result;
    }

    /// Keeps a physical recvmsg inside one record, isolating each kind byte.
    /// Coalesced memory input remains valid for consume, but a transport read
    /// must not associate a later record's ancillary descriptor with this one.
    [[nodiscard]] std::size_t readCapacity(std::size_t maxBytes) const
    {
        if (m_state != Sync::FrameIngress::State::Accepting)
            return 0;
        if (!m_headerBytes)
            return std::min(maxBytes, std::size_t { 1 });
        if (!m_recordLength)
            return std::min(maxBytes, m_header.size() - m_headerBytes);
        return std::min(maxBytes, m_recordLength - m_headerBytes - m_bodyBytes);
    }

    /// Returns the complete allocation capacity, including the frame header.
    [[nodiscard]] std::size_t retainedBytes() const { return m_recordCapacity; }
    /// Returns header and body bytes received but not yet delivered.
    [[nodiscard]] std::size_t pendingBytes() const { return m_headerBytes + m_bodyBytes; }
    /// Reports a latched framing, policy, or allocation refusal, not Stop.
    [[nodiscard]] bool faulted() const { return m_state == Sync::FrameIngress::State::Refused; }
    /// Reports whether the exchange still takes records: false once a Stop or
    /// a refusal latched.
    [[nodiscard]] bool isAccepting() const { return m_state == Sync::FrameIngress::State::Accepting; }
    /// Returns the latched failure, or None before one.
    [[nodiscard]] Sync::FrameIngressError error() const { return m_error; }

    /// Releases idle storage without discarding any partial record or state.
    void releaseRetained()
    {
        if (!pendingBytes())
            releaseRecord();
    }

private:
    void releaseRecord()
    {
        m_record.reset();
        m_recordCapacity = 0;
        m_recordLength = 0;
        m_bodyBytes = 0;
    }

    void refuse(Sync::FrameIngressError error)
    {
        releaseRecord();
        m_headerBytes = 0;
        m_state = Sync::FrameIngress::State::Refused;
        m_error = error;
    }

    const std::size_t m_maxRecordBytes;
    std::array<std::uint8_t, adoptWireFrameHeaderBytes> m_header {};
    std::size_t m_headerBytes = 0;
    std::unique_ptr<std::uint8_t[]> m_record;
    std::size_t m_recordCapacity = 0;
    std::size_t m_recordLength = 0;
    std::size_t m_bodyBytes = 0;
    AdoptWireMessage m_kind = AdoptWireMessage::Status;
    Sync::FrameIngress::State m_state = Sync::FrameIngress::State::Accepting;
    Sync::FrameIngressError m_error = Sync::FrameIngressError::None;
};

} // namespace Ubiquitty::Protocol::Local
