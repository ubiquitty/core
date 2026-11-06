// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Local/Clipboard.h"

#include "Core/Protocol/Sync/Envelope.h"

#include <cstring>
#include <new>

namespace Ubiquitty::Protocol::Local {

namespace {

void write32(std::span<std::uint8_t, 4> bytes, std::uint32_t value)
{
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(value >> (8 * index));
}

std::uint32_t read32(std::span<const std::uint8_t, 4> bytes)
{
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index)
        value |= std::uint32_t { bytes[index] } << (8 * index);
    return value;
}

bool validRequest(const LocalClipboardRequest& request)
{
    return request.kind <= LocalClipboardKind::Route
        && (request.kind == LocalClipboardKind::Item) == !request.mime.empty()
        && request.mime.size() <= Sync::maxEffectClipboardMIMEBytes
        && request.mime.find('\0') == std::string_view::npos
        && Sync::isValidUTF8({ reinterpret_cast<const std::uint8_t*>(request.mime.data()), request.mime.size() });
}

} // namespace

std::expected<Sync::EffectCodecBytes, LocalControlError> encodeLocalClipboardRequest(std::uint64_t correlation, const LocalClipboardRequest& request)
{
    if (!correlation || !validRequest(request))
        return std::unexpected(LocalControlError::Malformed);
    const auto size = localControlRequestBytes + 7 + request.mime.size();
    Sync::EffectCodecBytes encoded { std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[size]), size };
    if (!encoded.bytes)
        return std::unexpected(LocalControlError::NoMemory);
    const auto prefix = LocalControlDetail::encodeRequest(LocalControlOperation::Clipboard, correlation);
    std::ranges::copy(prefix, encoded.bytes.get());
    auto header = std::span(encoded.bytes.get() + prefix.size(), 7);
    write32(header.first<4>(), static_cast<std::uint32_t>(3 + request.mime.size()));
    header[4] = std::to_underlying(request.kind);
    header[5] = static_cast<std::uint8_t>(request.mime.size());
    header[6] = static_cast<std::uint8_t>(request.mime.size() >> 8);
    if (!request.mime.empty())
        std::memcpy(encoded.bytes.get() + prefix.size() + header.size(), request.mime.data(), request.mime.size());
    return encoded;
}

std::expected<std::size_t, LocalControlError> localClipboardReplyBytes(const LocalClipboardReply& reply)
{
    if (!reply.correlation || !reply.daemonInstanceID || reply.status > Sync::EffectClipboardStatus::TooLarge
        || (reply.isHostSelected && reply.status != Sync::EffectClipboardStatus::Empty)
        || (reply.status != Sync::EffectClipboardStatus::OK && reply.bundle.count))
        return std::unexpected(LocalControlError::Malformed);
    if (reply.status != Sync::EffectClipboardStatus::OK)
        return localClipboardReplyHeaderBytes;
    const auto payload = Sync::effectClipboardEncodedBytes(reply.bundle, Sync::maxEffectClipboardReadBytes);
    if (!payload)
        return std::unexpected(LocalControlError::Malformed);
    return localClipboardReplyHeaderBytes + *payload;
}

std::expected<void, LocalControlError> writeLocalClipboardReply(const LocalClipboardReply& reply, std::span<std::uint8_t> output)
{
    const auto size = localClipboardReplyBytes(reply);
    if (!size || *size != output.size())
        return std::unexpected(LocalControlError::Malformed);
    std::ranges::copy(localControlPreamble, output.begin());
    output[localControlPreamble.size()] = localClipboardReplyOperation;
    output[localControlPreamble.size() + 1] = std::to_underlying(reply.status);
    LocalControlDetail::write64(std::span<std::uint8_t, 8>(output.data() + localControlPreamble.size() + 2, 8), reply.correlation);
    LocalControlDetail::write64(std::span<std::uint8_t, 8>(output.data() + localControlReplyBytes - 8, 8), reply.daemonInstanceID);
    output[localControlReplyBytes] = static_cast<std::uint8_t>(reply.isHostSelected);
    write32(std::span<std::uint8_t, 4>(output.data() + localControlReplyBytes + 1, 4), static_cast<std::uint32_t>(output.size() - localClipboardReplyHeaderBytes));
    if (reply.status == Sync::EffectClipboardStatus::OK
        && !Sync::writeEffectClipboard(reply.bundle, Sync::maxEffectClipboardReadBytes, output.subspan(localClipboardReplyHeaderBytes)))
        return std::unexpected(LocalControlError::Malformed);
    return {};
}

LocalClipboardRequestReader::State LocalClipboardRequestReader::consume(std::span<const std::uint8_t> bytes, const std::function<bool(std::size_t)>& admitsAllocation)
{
    if (bytes.empty())
        return m_state;
    if (m_state != State::Awaiting)
        return m_state = State::Refused;
    const auto header = bytes.first(std::min(bytes.size(), m_header.size() - m_headerReceived));
    std::ranges::copy(header, m_header.begin() + m_headerReceived);
    m_headerReceived += header.size();
    bytes = bytes.subspan(header.size());
    if (m_headerReceived < m_header.size())
        return m_state;
    if (!m_mime && !m_mimeReceived) {
        m_mimeLength = m_header[5] | (std::size_t { m_header[6] } << 8);
        if (read32(std::span { m_header }.first<4>()) != 3 + m_mimeLength
            || m_header[4] > std::to_underlying(LocalClipboardKind::Route)
            || (m_header[4] == std::to_underlying(LocalClipboardKind::Item)) != (m_mimeLength != 0))
            return m_state = State::Refused;
        if (m_mimeLength) {
            if (admitsAllocation && !admitsAllocation(m_mimeLength))
                return m_state = State::Refused;
            m_mime.reset(new (std::nothrow) char[m_mimeLength]);
            if (!m_mime)
                return m_state = State::Refused;
        }
    }
    if (bytes.size() > m_mimeLength - m_mimeReceived)
        return m_state = State::Refused;
    if (!bytes.empty())
        std::memcpy(m_mime.get() + m_mimeReceived, bytes.data(), bytes.size());
    m_mimeReceived += bytes.size();
    if (m_mimeReceived != m_mimeLength)
        return m_state;
    const LocalClipboardRequest request { static_cast<LocalClipboardKind>(m_header[4]), { m_mime.get(), m_mimeLength } };
    return m_state = validRequest(request) ? State::Ready : State::Refused;
}

std::optional<LocalClipboardRequest> LocalClipboardRequestReader::take()
{
    if (m_state != State::Ready)
        return std::nullopt;
    m_state = State::Consumed;
    return LocalClipboardRequest { static_cast<LocalClipboardKind>(m_header[4]), { m_mime.get(), m_mimeLength } };
}

LocalClipboardRequestReader::State LocalClipboardReplyReader::consume(std::span<const std::uint8_t> bytes)
{
    using State = LocalClipboardRequestReader::State;
    if (bytes.empty())
        return m_state;
    if (m_state != State::Awaiting)
        return m_state = State::Refused;
    const auto header = bytes.first(std::min(bytes.size(), m_header.size() - m_headerReceived));
    std::ranges::copy(header, m_header.begin() + m_headerReceived);
    m_headerReceived += header.size();
    bytes = bytes.subspan(header.size());
    if (m_headerReceived < m_header.size())
        return m_state;
    if (!m_body && !m_bodyReceived) {
        if (!std::ranges::equal(std::span { m_header }.first(localControlPreamble.size()), localControlPreamble)
            || m_header[localControlPreamble.size()] != localClipboardReplyOperation
            || m_header[localControlPreamble.size() + 1] > std::to_underlying(Sync::EffectClipboardStatus::TooLarge)
            || m_header[localControlReplyBytes] > 1)
            return m_state = State::Refused;
        m_reply.correlation = LocalControlDetail::read64(std::span<const std::uint8_t, 8>(m_header.data() + localControlPreamble.size() + 2, 8));
        m_reply.daemonInstanceID = LocalControlDetail::read64(std::span<const std::uint8_t, 8>(m_header.data() + localControlReplyBytes - 8, 8));
        m_reply.status = static_cast<Sync::EffectClipboardStatus>(m_header[localControlPreamble.size() + 1]);
        m_reply.isHostSelected = m_header[localControlReplyBytes] != 0;
        m_bodyLength = read32(std::span<const std::uint8_t, 4>(m_header.data() + localControlReplyBytes + 1, 4));
        if (m_reply.correlation != m_correlation || !m_reply.daemonInstanceID
            || m_bodyLength > Sync::maxEffectClipboardEncodedReadBytes
            || (m_reply.isHostSelected && m_reply.status != Sync::EffectClipboardStatus::Empty)
            || (m_reply.status == Sync::EffectClipboardStatus::OK) != (m_bodyLength != 0))
            return m_state = State::Refused;
        if (m_bodyLength) {
            m_body.reset(new (std::nothrow) std::uint8_t[m_bodyLength]);
            if (!m_body)
                return m_state = State::Refused;
        }
    }
    if (bytes.size() > m_bodyLength - m_bodyReceived)
        return m_state = State::Refused;
    if (!bytes.empty())
        std::memcpy(m_body.get() + m_bodyReceived, bytes.data(), bytes.size());
    m_bodyReceived += bytes.size();
    if (m_bodyReceived != m_bodyLength)
        return m_state;
    if (m_bodyLength) {
        const auto bundle = Sync::decodeEffectClipboard({ m_body.get(), m_bodyLength }, Sync::maxEffectClipboardReadBytes);
        if (!bundle)
            return m_state = State::Refused;
        m_reply.bundle = *bundle;
    }
    return m_state = State::Ready;
}

std::optional<LocalClipboardReply> LocalClipboardReplyReader::take()
{
    using State = LocalClipboardRequestReader::State;
    if (m_state != State::Ready)
        return std::nullopt;
    m_state = State::Consumed;
    return m_reply;
}

} // namespace Ubiquitty::Protocol::Local
