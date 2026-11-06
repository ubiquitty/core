// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ConsentTranscript.h"
#include "HostConnection.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::HostTest {

/// A typed control reader beside the raw socket fixture's screen reader. Its
/// independent transcript cursor never consumes a checkpoint or a pane record.
class ConsentClient {
public:
    explicit ConsentClient(Connection& connection)
        : m_connection(connection)
    {
    }

    std::optional<Protocol::Sync::ConsentQuestion> question(Protocol::Sync::ConsentKind kind, int milliseconds = 3000)
    {
        std::optional<Protocol::Sync::ConsentQuestion> result;
        wait(milliseconds, [&] {
            result = m_transcript.takeQuestion(kind);
            return result.has_value();
        });
        return result;
    }

    std::optional<Protocol::Sync::ConsentStatus> status(Protocol::Sync::ConsentStatusPhase phase, int milliseconds = 3000)
    {
        std::optional<Protocol::Sync::ConsentStatus> result;
        wait(milliseconds, [&] {
            result = m_transcript.takeStatus(phase);
            return result.has_value();
        });
        return result;
    }

    std::optional<Protocol::Sync::AttachmentsList> roster(int milliseconds = 3000)
    {
        std::optional<Protocol::Sync::AttachmentsList> result;
        wait(milliseconds, [&] {
            result = m_transcript.takeRoster();
            return result.has_value();
        });
        return result;
    }

    bool isGranted(int milliseconds = 3000)
    {
        wait(milliseconds, [&] { return m_transcript.isGranted(); });
        return !m_failed && m_transcript.isGranted();
    }

    bool respond(const Protocol::Sync::ConsentQuestion& question, Protocol::Sync::ConsentChoice choice)
    {
        return send(Protocol::Control::consentResponseRequest({ .requestID = question.requestID, .kind = static_cast<Protocol::Sync::ConsentKind>(question.action), .decision = choice }, "consent:answer:" + std::to_string(m_nextCall++)));
    }

    std::optional<std::string> revoke(const Protocol::Control::ConsentRevocation& revocation)
    {
        const auto id = "consent:revoke:" + std::to_string(m_nextCall++);
        return send(Protocol::Control::consentRevocationRequest(revocation, id)) ? std::optional { id } : std::nullopt;
    }

    std::optional<Codec::JSON::Value> result(std::string_view id, int milliseconds = 3000)
    {
        std::optional<Codec::JSON::Value> result;
        wait(milliseconds, [&] {
            result = m_transcript.takeResult(id);
            return result.has_value();
        });
        return result;
    }

private:
    bool send(const std::string& text)
    {
        if (text.empty() || !isGranted() || !m_connection.greeting || !m_connection.welcome || m_connection.hasFailed)
            return false;
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Call { .text = text });
        std::vector<std::uint8_t> stream;
        if (!Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Call, 0, body))
            return false;
        return writeToDaemon(m_connection.fileDescriptor, { reinterpret_cast<const char*>(stream.data()), stream.size() });
    }

    void readReceived()
    {
        if (!m_transcript.feed(std::as_bytes(std::span(m_connection.rx.data() + m_fed, m_connection.rx.size() - m_fed))))
            m_failed = true;
        m_fed = m_connection.rx.size();
        m_failed |= m_connection.hasFailed;
    }

    template<typename Accept>
    void wait(int milliseconds, Accept&& accept)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        readReceived();
        while (!m_failed && !accept() && std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            pollfd descriptor { m_connection.fileDescriptor, POLLIN, 0 };
            if (::poll(&descriptor, 1, static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 50))) <= 0)
                continue;
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
    }

    Connection& m_connection;
    ConsentTranscript m_transcript;
    std::size_t m_fed = 0;
    std::uint64_t m_nextCall = 1;
    bool m_failed = false;
};

} // namespace Ubiquitty::HostTest
