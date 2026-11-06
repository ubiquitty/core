// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Session/RemoteBootstrap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace Ubiquitty::Session {

/// Separates bounded shell startup output from the host's binary stream.
class BootstrapResponse {
public:
    /// Identifies the outcome of the bootstrap response.
    enum class Status {
        Waiting,
        Ready,
        Deploy,
        /// The bootstrap was pinned to a daemon that no longer listens; see
        /// ``goneSocketName()``.
        InstanceGone,
        Invalid
    };

    /// Borrows protocol bytes remaining after startup processing.
    struct Result {
        Status status;
        std::span<const std::uint8_t> protocolBytes;
        /// A single undecided first byte retained across a carrier read.
        std::span<const std::uint8_t> protocolPrefix {};
    };

    /// Consumes startup lines and returns only bytes past the protocol boundary.
    ///
    /// Direct socket carriers start with a handshake frame. A shell carrier
    /// that printed text must announce its boundary; binary bytes are never
    /// searched for a plausible frame after discarded text.
    ///
    /// - Parameter bytes: The next carrier chunk, borrowed through the returned result.
    /// - Returns: The startup outcome and the remaining protocol bytes.
    [[nodiscard]] Result consume(std::span<const std::uint8_t> bytes)
    {
        if (m_status == Status::Ready) {
            return { m_status, bytes };
        }
        if (m_status != Status::Waiting || bytes.empty()) {
            return { m_status, {} };
        }
        if (m_hasHeldByeByte) {
            m_hasHeldByeByte = false;
            if (!bytes.front()) {
                m_status = Status::Ready;
                return { m_status, bytes, m_byePrefix };
            }
            // A leading blank startup line, not a binary channel-zero Bye.
            m_receivedBytes = 1;
        }
        if (!m_receivedBytes) {
            const auto type = static_cast<Protocol::Sync::FrameKind>(bytes.front());
            if (type == Protocol::Sync::FrameKind::Welcome
                || (type == Protocol::Sync::FrameKind::Bye && bytes.size() > 1 && !bytes[1])) {
                m_status = Status::Ready;
                return { m_status, bytes };
            }
            if (type == Protocol::Sync::FrameKind::Bye && bytes.size() == 1) {
                // Bye's kind is also LF. Keep only this byte until its exact
                // channel distinguishes a direct frame from shell startup.
                m_hasHeldByeByte = true;
                return { Status::Waiting, {} };
            }
        }
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            if (++m_receivedBytes > maxStartupBytes || !bytes[index]) {
                m_status = Status::Invalid;
                return { m_status, {} };
            }
            if (bytes[index] != '\n') {
                if (m_line.size() >= maxLineBytes) {
                    m_status = Status::Invalid;
                    return { m_status, {} };
                }
                m_line += static_cast<char>(bytes[index]);
                continue;
            }
            if (!m_line.empty() && m_line.back() == '\r') {
                m_line.pop_back();
            }
            if (m_line == syncSentinel) {
                m_status = Status::Ready;
                m_line.clear();
                return { m_status, bytes.subspan(index + 1) };
            }
            const std::string prefix = std::string(deploySentinel) + ' ';
            if (m_line.starts_with(prefix)) {
                m_platform = m_line.substr(prefix.size());
                m_status = !m_platform.empty() && m_platform.size() <= 128 ? Status::Deploy : Status::Invalid;
                m_line.clear();
                return { m_status, {} };
            }
            if (const InstanceGoneReading gone = readInstanceGoneLine(m_line); gone.kind != InstanceGoneLine::Other) {
                m_goneSocketName = gone.socketName;
                m_status = gone.kind == InstanceGoneLine::Gone ? Status::InstanceGone : Status::Invalid;
                m_line.clear();
                return { m_status, {} };
            }
            m_line.clear();
        }
        return { Status::Waiting, {} };
    }

    /// Returns the platform reported by a host requiring installation.
    [[nodiscard]] const std::string& platform() const { return m_platform; }

    /// Returns the socket of the pinned daemon a host reported gone.
    [[nodiscard]] const std::string& goneSocketName() const { return m_goneSocketName; }

    /// Reports whether the remote command wrote anything at all.
    [[nodiscard]] bool hasReceivedAnything() const { return m_receivedBytes || m_hasHeldByeByte || m_status != Status::Waiting; }

    /// Reports whether startup text arrived without a complete outcome.
    [[nodiscard]] bool isIncomplete() const { return (m_receivedBytes || m_hasHeldByeByte) && m_status == Status::Waiting; }

private:
    static constexpr std::size_t maxStartupBytes = 16 * 1024;
    static constexpr std::size_t maxLineBytes = 4096;
    Status m_status { Status::Waiting };
    std::array<std::uint8_t, 1> m_byePrefix { std::to_underlying(Protocol::Sync::FrameKind::Bye) };
    bool m_hasHeldByeByte = false;
    std::size_t m_receivedBytes { 0 };
    std::string m_line;
    std::string m_platform;
    std::string m_goneSocketName;
};

} // namespace Ubiquitty::Session
