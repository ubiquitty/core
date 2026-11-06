// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/QUICBootstrapReader.h"

#include "Core/Session/RemoteBootstrap.h"

#include <string_view>

namespace Ubiquitty::Protocol::Sync {

QUICBootstrapReader::State QUICBootstrapReader::feed(QUICBootstrapBytes data)
{
    if (m_state != State::NeedMoreData)
        return m_state;

    for (const std::uint8_t byte : data) {
        if (byte == '\n') {
            consumeLine({ m_buffer.data(), m_bufferSize });
            m_bufferSize = 0;
            if (m_state != State::NeedMoreData)
                return m_state;
            continue;
        }
        // The same line bound applies regardless of carrier fragmentation,
        // before storing a byte or parsing a completed line.
        if (m_bufferSize == m_buffer.size()) {
            m_state = State::Refused;
            m_refusalReason = "bootstrap output exceeded the line size limit";
            m_bufferSize = 0;
            return m_state;
        }
        m_buffer[m_bufferSize++] = static_cast<char>(byte);
    }
    return m_state;
}

QUICBootstrapReader::State QUICBootstrapReader::finish()
{
    if (m_state != State::NeedMoreData)
        return m_state;

    // A command that exits without a trailing newline still answered.
    if (m_bufferSize) {
        consumeLine({ m_buffer.data(), m_bufferSize });
        m_bufferSize = 0;
    }
    if (m_state == State::NeedMoreData) {
        m_state = State::Refused;
        m_refusalReason = "bootstrap produced no quic_port line";
    }
    return m_state;
}

void QUICBootstrapReader::consumeLine(std::string_view line)
{
    // Tolerate a CRLF carrier.
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    if (line.empty())
        return;

    if (const Session::InstanceGoneReading gone = Session::readInstanceGoneLine(line); gone.kind == Session::InstanceGoneLine::Gone) {
        m_goneSocketName = gone.socketName;
        m_state = State::InstanceGone;
        return;
    }

    QUICBootstrapInfo parsed;
    if (parseQUICBootstrapLine(line, parsed)) {
        m_info = parsed;
        m_state = State::Ready;
        return;
    }

    // A line that names a bootstrap field but did not parse is a MALFORMED
    // answer, not banner noise: the difference matters because refusing here is
    // what stops a half-supplied line — a token with no cert pin — from being
    // skipped over in the hope that a better line follows.
    if (line.find("quic_port=") != std::string_view::npos
        || line.find("quic_token=") != std::string_view::npos
        || line.find("quic_cert_sha256=") != std::string_view::npos) {
        m_state = State::Refused;
        m_refusalReason = "bootstrap line is missing a field (an unpinned token is stealable)";
    }
    // Anything else is a banner or a warning the login printed: not an answer,
    // not a refusal.
}

} // namespace Ubiquitty::Protocol::Sync
