// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Control/SessionTermination.h"
#include "HostConnection.h"

namespace Ubiquitty::HostTest {

/// Reads the connection-level result independently of an attached pane's
/// picture. Only a complete, correlated typed answer confirms termination.
class TerminationClient final : private Protocol::Sync::ClientPaneDelegate {
public:
    /// Borrows a socket fixture, including its already received greeting.
    /// - Parameter connection: The physical connection, which outlives this reader.
    explicit TerminationClient(Connection& connection)
        : m_connection(connection)
    {
        if (!connection.greeting) {
            m_failed = true;
            return;
        }
        m_client = std::make_unique<Protocol::Sync::ClientConnection>(Protocol::Sync::measuredClientPolicy(), clientIdentity(*connection.greeting), static_cast<Protocol::Sync::ClientPaneDelegate*>(this));
        m_client->start();
        // The physical connection already sent this exact Greet. Only replay
        // its retained transcript; never send a second greeting to the daemon.
        (void)m_client->takeStreamOutput();
    }

    TerminationClient(const TerminationClient&) = delete;
    TerminationClient& operator=(const TerminationClient&) = delete;
    TerminationClient(TerminationClient&&) = delete;
    TerminationClient& operator=(TerminationClient&&) = delete;

    /// Sends one operation after the authenticated canonical Welcome.
    /// - Parameters:
    ///   - sessionID: The exact target.
    ///   - requestID: A fresh correlation string on this connection.
    ///   - authority: The operation's distinct existing authority.
    /// - Returns: Whether the request was encoded and sent.
    bool send(std::uint32_t sessionID, std::string requestID,
        Protocol::Control::TerminationAuthority authority = Protocol::Control::TerminationAuthority::Control)
    {
        if (m_pending)
            return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        readReceived();
        while (!isAdmitted() && !m_failed && std::chrono::steady_clock::now() < deadline)
            readSocket();
        if (!isAdmitted() || m_failed || !m_connection.welcome)
            return false;
        const auto text = Protocol::Control::terminationRequest(sessionID, requestID, authority);
        if (text.empty())
            return false;
        m_sessionID = sessionID;
        m_requestID = std::move(requestID);
        m_outcome.reset();
        m_pending = true;
        if (!m_client->call(text)) {
            m_pending = false;
            return false;
        }
        if (!flush()) {
            m_failed = true;
            return false;
        }
        return true;
    }

    /// Waits for the exact answer, never treating PaneUnbind or Bye as success.
    /// - Returns: The answer, or Failed after closure, malformed data, or timeout.
    Protocol::Control::TerminationOutcome wait()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        readReceived();
        while (!m_outcome && !m_failed && std::chrono::steady_clock::now() < deadline)
            readSocket();
        m_pending = false;
        if (m_outcome)
            releaseAnswerFrame();
        return m_failed ? Protocol::Control::TerminationOutcome::Failed
                        : m_outcome.value_or(Protocol::Control::TerminationOutcome::Failed);
    }

private:
    /// Owns only this operation's exact answer, before a later unbind or Bye.
    /// No callback writes to the carrier or changes the production reader.
    bool consumeControlResult(std::string_view text) override
    {
        if (!m_pending)
            return false;
        bool isJSON = false;
        const auto answer = Codec::JSON::parse(std::string(text), isJSON);
        if (!isJSON)
            return false;
        const auto outcome = Protocol::Control::terminationOutcome(answer, m_requestID, m_sessionID);
        if (!outcome)
            return false;
        if (m_outcome)
            m_failed = true;
        else
            m_outcome = outcome;
        return true;
    }

    /// Removes this operation's answer from the borrowed connection's parsed
    /// frames. Both readers parse the same bytes, so an answer left there
    /// would satisfy the fixture's next wait for an unrelated Result.
    void releaseAnswerFrame()
    {
        auto& frames = m_connection.frames;
        const auto answer = std::ranges::find_if(frames, [&](const ReceivedFrame& frame) {
            if (frame.kind != Protocol::Sync::FrameKind::Result || frame.channel != 0)
                return false;
            const auto result = Protocol::Sync::decodeResult({ reinterpret_cast<const std::uint8_t*>(frame.body.data()), frame.body.size() });
            if (!result || result->transfer)
                return false;
            bool isJSON = false;
            const auto parsed = Codec::JSON::parse(result->text, isJSON);
            return isJSON && Protocol::Control::terminationOutcome(parsed, m_requestID, m_sessionID).has_value();
        });
        if (answer != frames.end())
            frames.erase(answer);
    }

    /// Uses the actual pending-consent state, not merely a queued Welcome.
    [[nodiscard]] bool isAdmitted() const
    {
        return m_client && m_client->state() == Protocol::Sync::ConnectionState::Attached;
    }

    /// Publishes ACKs, probes, and the typed Call only outside ingress callbacks.
    bool flush()
    {
        if (!m_client)
            return false;
        m_client->tick(now());
        const auto bytes = m_client->takeStreamOutput();
        return bytes.empty() || writeToDaemon(m_connection.fileDescriptor, { reinterpret_cast<const char*>(bytes.data()), bytes.size() });
    }

    [[nodiscard]] static std::uint64_t now()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    /// Decodes the unread socket-fixture suffix and retains only the exact answer.
    void readReceived()
    {
        if (m_failed || !m_client)
            return;
        m_client->receiveStream({ reinterpret_cast<const std::uint8_t*>(m_connection.rx.data() + m_fed), m_connection.rx.size() - m_fed }, now());
        m_fed = m_connection.rx.size();
        // Unrelated inline or bulk Results remain owned by the real reader;
        // their arrival cannot satisfy or invalidate this operation's ID.
        (void)m_client->takeResults();
        if (m_connection.hasFailed)
            m_failed = true;
        if (m_client->state() == Protocol::Sync::ConnectionState::Closed && !m_outcome)
            m_failed = true;
    }

    /// Makes one bounded socket read without altering another connection.
    void readSocket()
    {
        // Once the Call was sent, a failed Ack or Echo cannot invalidate a
        // Result already waiting on the readable half of a closing carrier.
        // Stop writing, but keep the same bounded wait and exact-result check.
        if (!m_isOutputClosed && !flush())
            m_isOutputClosed = true;
        if (m_isOutputClosed && !m_pending) {
            m_failed = true;
            return;
        }
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
    std::size_t m_fed = 0;
    std::uint32_t m_sessionID = 0;
    std::string m_requestID;
    std::optional<Protocol::Control::TerminationOutcome> m_outcome;
    bool m_pending = false;
    bool m_failed = false;
    bool m_isOutputClosed = false;
    // Destroy first: the decoder's delegate remains alive through teardown.
    std::unique_ptr<Protocol::Sync::ClientConnection> m_client;
};

} // namespace Ubiquitty::HostTest
