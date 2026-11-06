// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Control/FileRead.h"
#include "HostConnection.h"

namespace Ubiquitty::HostTest {

/// Reads real inline and bulk call results on an already greeted fixture socket.
class FileReadClient {
public:
    /// Borrows one socket and replays its received greeting, never sending another.
    /// - Parameter connection: The physical fixture, which outlives this reader.
    explicit FileReadClient(Connection& connection)
        : m_connection(connection)
        , m_client(Protocol::Sync::measuredClientPolicy(), connection.greeting ? clientIdentity(*connection.greeting) : Protocol::Sync::ClientIdentity {})
    {
        if (!connection.greeting) {
            m_failed = true;
            return;
        }
        m_client.start();
        (void)m_client.takeStreamOutput();
    }

    /// Sends one exact file read after the pane is bound.
    /// - Parameters:
    ///   - requestID: A fresh fixture identity.
    ///   - path: The requested path.
    /// - Returns: Whether the call was queued and sent.
    bool send(std::uint32_t requestID, const std::string& path)
    {
        readReceived();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!m_failed && (!m_client.welcome() || !m_connection.paneHandle) && std::chrono::steady_clock::now() < deadline)
            readSocket();
        if (m_failed || !m_client.welcome() || !m_connection.paneHandle)
            return false;
        m_requestID = requestID;
        m_path = path;
        m_correlation = "fixture.file." + std::to_string(requestID);
        m_pane = m_connection.paneHandle;
        if (!m_client.call(Protocol::Control::fileReadRequest(m_pane, path, m_correlation)))
            return false;
        flush();
        return true;
    }

    /// Waits for the complete correlated bytes; partial bulk data never succeeds.
    /// - Parameter milliseconds: Whole-operation deadline.
    /// - Returns: The validated result, or nothing after refusal/malformed transport.
    std::optional<Protocol::Control::FileReadReply> wait(int milliseconds)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (!m_failed && std::chrono::steady_clock::now() < deadline) {
            readReceived();
            for (const auto& text : m_client.takeResults()) {
                bool valid = false;
                const auto answer = Codec::JSON::parse(text, valid);
                if (!valid)
                    return std::nullopt;
                auto reply = Protocol::Control::decodeFileReadReply(answer, m_correlation, m_pane, m_path);
                if (!reply && reply.error() == Protocol::Control::FileReadFailure::Unrelated)
                    continue;
                if (!reply)
                    return std::nullopt;
                reply->requestID = m_requestID;
                return std::move(*reply);
            }
            if (m_client.state() == Protocol::Sync::ConnectionState::Closed)
                break;
            flush();
            readSocket();
        }
        return std::nullopt;
    }

    /// The result heads that announced a real bulk transfer on this socket.
    /// - Returns: The number seen, excluding checkpoint and history transfers.
    [[nodiscard]] unsigned bulkResults() const { return m_bulkResults; }

private:
    /// Reads the clock used by production ACK and probe deadlines.
    /// - Returns: Monotonic microseconds.
    [[nodiscard]] static std::uint64_t now()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    /// Feeds only the unread received suffix into the production client.
    void readReceived()
    {
        if (m_failed)
            return;
        const std::span<const std::uint8_t> bytes { reinterpret_cast<const std::uint8_t*>(m_connection.rx.data() + m_fed), m_connection.rx.size() - m_fed };
        m_fed = m_connection.rx.size();
        using namespace Protocol::Sync;
        const auto observed = m_ingress.consume(bytes, [](std::span<const std::uint8_t> prefix) {
            FrameHeader header;
            EnvelopeError error = EnvelopeError::None;
            const auto state = readFrameHeader(prefix, header, error);
            if (state == FrameHeaderState::NeedsMore)
                return FrameHeaderDecision {};
            return FrameHeaderDecision { .state = state == FrameHeaderState::Complete ? FrameHeaderDecision::State::Admitted : FrameHeaderDecision::State::Refused,
                .bodyBytes = header.bodyBytes }; }, [&](std::span<const std::uint8_t> prefix, std::span<const std::uint8_t> body) {
            FrameHeader header;
            EnvelopeError error = EnvelopeError::None;
            if (readFrameHeader(prefix, header, error) != FrameHeaderState::Complete)
                return FrameDelivery::Stop;
            if (header.kind == FrameKind::Result) {
                const auto result = decodeResult(body);
                if (!result || header.channel)
                    return FrameDelivery::Stop;
                m_bulkResults += result->transfer ? 1 : 0;
            }
            return FrameDelivery::Continue; });
        m_failed = observed.state != FrameIngress::State::Accepting || m_connection.hasFailed;
        if (!m_failed)
            m_client.receiveStream(bytes, now());
    }

    /// Publishes the reader's acknowledgements and calls on this socket.
    void flush()
    {
        m_client.tick(now());
        const auto bytes = m_client.takeStreamOutput();
        if (!bytes.empty() && !writeToDaemon(m_connection.fileDescriptor, { reinterpret_cast<const char*>(bytes.data()), bytes.size() }))
            m_failed = true;
    }

    /// Makes one bounded socket read without touching another fixture.
    void readSocket()
    {
        pollfd descriptor { m_connection.fileDescriptor, POLLIN, 0 };
        if (::poll(&descriptor, 1, 20) <= 0)
            return;
        char bytes[8192];
        const auto count = ::read(m_connection.fileDescriptor, bytes, sizeof(bytes));
        if (count <= 0) {
            m_failed = true;
            return;
        }
        m_connection.rx.append(bytes, static_cast<std::size_t>(count));
        parseFrames(m_connection);
        readReceived();
    }

    Connection& m_connection;
    Protocol::Sync::ClientConnection m_client;
    Protocol::Sync::FrameIngress m_ingress { Protocol::Sync::maxFrameBodyBytes };
    bool m_failed = false;
    std::size_t m_fed = 0;
    std::uint32_t m_requestID = 0;
    std::uint32_t m_pane = 0;
    unsigned m_bulkResults = 0;
    std::string m_correlation;
    std::string m_path;
};

} // namespace Ubiquitty::HostTest
