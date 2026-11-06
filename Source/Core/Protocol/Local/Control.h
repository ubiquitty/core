// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>

namespace Ubiquitty::Protocol::Local {

/// Selects a daemon-local exchange before the remote carrier decoder.
inline constexpr std::array<std::uint8_t, 16> localControlPreamble { 0xF9, 'U', 'B', 'Q', 'L', 'O', 'C', 'A', 'L', 0, 0, 0, 0, 0, 0, 1 };
inline constexpr std::uint8_t localReloadOperation = 1;
inline constexpr std::uint8_t localReloadReplyOperation = 0x81;
inline constexpr std::uint8_t localQUICTokenOperation = 2;
inline constexpr std::uint8_t localQUICTokenReplyOperation = 0x82;
inline constexpr std::uint8_t localHandoffOperation = 3;
inline constexpr std::uint8_t localHandoffReplyOperation = 0x83;
inline constexpr std::uint8_t localClipboardOperation = 4;
inline constexpr std::uint8_t localClipboardReplyOperation = 0x84;
inline constexpr std::uint8_t localReclaimOperation = 5;
inline constexpr std::uint8_t localReclaimReplyOperation = 0x85;
/// Marks a bridged connection as one that reached this host over SSH. The
/// daemon acknowledges it, and the connection continues as an ordinary carrier.
inline constexpr std::uint8_t localRemoteRelayOperation = 6;
inline constexpr std::uint8_t localRemoteRelayReplyOperation = 0x86;
inline constexpr std::size_t localControlRequestBytes = localControlPreamble.size() + 1 + sizeof(std::uint64_t);
inline constexpr std::size_t localControlReplyBytes = localControlPreamble.size() + 2 + 2 * sizeof(std::uint64_t);
inline constexpr std::size_t maxQUICTokenLength = 64;
inline constexpr std::size_t certificateSHA256Length = 32;
inline constexpr std::size_t maxLocalQUICTokenReplyBytes = localControlReplyBytes + sizeof(std::uint16_t) + 1 + maxQUICTokenLength + certificateSHA256Length;
static_assert(maxLocalQUICTokenReplyBytes == 133);

/// A first-packet operation, carrying no caller-supplied authority.
enum class LocalControlOperation : std::uint8_t {
    ReloadCredentials = localReloadOperation,
    QUICToken = localQUICTokenOperation,
    SessionHandoff = localHandoffOperation,
    Clipboard = localClipboardOperation,
    SessionReclaim = localReclaimOperation,
    RemoteRelay = localRemoteRelayOperation,
};

/// Correlates one operation on a fresh local connection.
struct LocalControlRequest {
    LocalControlOperation operation = LocalControlOperation::ReloadCredentials;
    std::uint64_t correlation = 0;
    bool operator==(const LocalControlRequest&) const = default;
};

/// Reports whether the authenticated owner completed its credential reload.
enum class LocalControlStatus : std::uint8_t {
    Reloaded,
    Refused,
};

/// Correlates a reload reply with one request and its serving daemon.
struct LocalControlReply {
    std::uint64_t correlation = 0;
    std::uint64_t daemonInstanceID = 0;
    LocalControlStatus status = LocalControlStatus::Refused;
    bool operator==(const LocalControlReply&) const = default;
};

/// Distinguishes a disabled listener from an operation that could not be granted.
enum class LocalQUICTokenStatus : std::uint8_t {
    Granted = 0,
    Disabled = 1,
    Refused = 2,
};

/// Holds a complete grant without allocating from a received length.
struct LocalQUICTokenReply {
    std::uint64_t correlation = 0;
    std::uint64_t daemonInstanceID = 0;
    LocalQUICTokenStatus status = LocalQUICTokenStatus::Refused;
    std::uint16_t port = 0;
    std::uint8_t tokenLength = 0;
    std::array<std::uint8_t, maxQUICTokenLength> token {};
    std::array<std::uint8_t, certificateSHA256Length> certificateSHA256 {};
    bool operator==(const LocalQUICTokenReply&) const = default;
};

/// Why a local packet could not represent one complete operation.
enum class LocalControlError : std::uint8_t {
    Malformed,
    InvalidIdentity,
    InvalidGrant,
    NoMemory,
};

/// An encoded reply's bounded storage and exact transmitted prefix.
struct EncodedLocalQUICTokenReply {
    std::array<std::uint8_t, maxLocalQUICTokenReplyBytes> bytes {};
    std::size_t length = 0;

    [[nodiscard]] std::span<const std::uint8_t> view() const { return std::span { bytes }.first(length); }
};

namespace LocalControlDetail {

inline void write64(std::span<std::uint8_t, sizeof(std::uint64_t)> bytes, std::uint64_t value)
{
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::uint8_t>(value >> (8 * index));
}

[[nodiscard]] inline std::uint64_t read64(std::span<const std::uint8_t, sizeof(std::uint64_t)> bytes)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index)
        value |= std::uint64_t { bytes[index] } << (8 * index);
    return value;
}

[[nodiscard]] inline std::array<std::uint8_t, localControlRequestBytes> encodeRequest(LocalControlOperation operation, std::uint64_t correlation)
{
    std::array<std::uint8_t, localControlRequestBytes> bytes {};
    std::ranges::copy(localControlPreamble, bytes.begin());
    bytes[localControlPreamble.size()] = std::to_underlying(operation);
    write64(std::span { bytes }.last<sizeof(std::uint64_t)>(), correlation);
    return bytes;
}

} // namespace LocalControlDetail

/// Encodes the frozen reload request; a reader refuses a zero correlation.
[[nodiscard]] inline std::array<std::uint8_t, localControlRequestBytes> encodeLocalReloadRequest(std::uint64_t correlation)
{
    return LocalControlDetail::encodeRequest(LocalControlOperation::ReloadCredentials, correlation);
}

/// Encodes a QUIC bootstrap request only with a nonzero correlation.
[[nodiscard]] inline std::expected<std::array<std::uint8_t, localControlRequestBytes>, LocalControlError> encodeLocalQUICTokenRequest(std::uint64_t correlation)
{
    if (!correlation)
        return std::unexpected(LocalControlError::InvalidIdentity);
    return LocalControlDetail::encodeRequest(LocalControlOperation::QUICToken, correlation);
}

/// Encodes the mark a bridge relaying an SSH channel sends before the
/// client's own bytes, only with a nonzero correlation.
///
/// It lowers what the connection's sessions may do beyond the grid to a remote
/// client's and grants nothing; the daemon applies it only from its own bridge
/// and acknowledges it from any same-user stream.
///
/// - Parameter correlation: A nonzero value the acknowledgement repeats.
/// - Returns: The request, or `InvalidIdentity` for a zero correlation.
[[nodiscard]] inline std::expected<std::array<std::uint8_t, localControlRequestBytes>, LocalControlError> encodeLocalRemoteRelayRequest(std::uint64_t correlation)
{
    if (!correlation)
        return std::unexpected(LocalControlError::InvalidIdentity);
    return LocalControlDetail::encodeRequest(LocalControlOperation::RemoteRelay, correlation);
}

/// The daemon's acknowledgement of a RemoteRelay mark, sent before any of the
/// carrier's own bytes.
///
/// A daemon that understood the mark always sends it, whether it lowered the
/// connection or not. A bridge relays only on one that says the mark applied.
struct LocalRemoteRelayReply {
    std::uint64_t correlation = 0;
    /// The daemon's instance, named only to its own bridge; zero otherwise.
    std::uint64_t daemonInstanceID = 0;
    /// Whether the daemon lowered the connection: only its own bridge's mark
    /// is applied, and any other peer's is acknowledged as not applied.
    bool isMarked = false;
    bool operator==(const LocalRemoteRelayReply&) const = default;
};

/// Whether an acknowledgement is one a daemon sends: a nonzero correlation,
/// and the instance named exactly when the mark was applied.
///
/// - Parameter reply: The acknowledgement.
/// - Returns: Whether it is well formed.
[[nodiscard]] constexpr bool isValidLocalRemoteRelayReply(const LocalRemoteRelayReply& reply)
{
    return reply.correlation && (reply.daemonInstanceID != 0) == reply.isMarked;
}

/// Encodes the acknowledgement, the same size as every other fixed reply.
///
/// - Parameter reply: A well-formed acknowledgement.
/// - Returns: The bytes, or `InvalidIdentity` for any other.
[[nodiscard]] inline std::expected<std::array<std::uint8_t, localControlReplyBytes>, LocalControlError> encodeLocalRemoteRelayReply(const LocalRemoteRelayReply& reply)
{
    if (!isValidLocalRemoteRelayReply(reply))
        return std::unexpected(LocalControlError::InvalidIdentity);
    std::array<std::uint8_t, localControlReplyBytes> bytes {};
    std::ranges::copy(localControlPreamble, bytes.begin());
    bytes[localControlPreamble.size()] = localRemoteRelayReplyOperation;
    bytes[localControlPreamble.size() + 1] = reply.isMarked ? 1 : 0;
    LocalControlDetail::write64(std::span { bytes }.subspan<localControlPreamble.size() + 2, sizeof(std::uint64_t)>(), reply.correlation);
    LocalControlDetail::write64(std::span { bytes }.last<sizeof(std::uint64_t)>(), reply.daemonInstanceID);
    return bytes;
}

/// Decodes exactly one acknowledgement, refusing anything else.
///
/// - Parameter bytes: The whole reply.
/// - Returns: The acknowledgement, or nothing for a malformed or zero one.
[[nodiscard]] inline std::optional<LocalRemoteRelayReply> decodeLocalRemoteRelayReply(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != localControlReplyBytes || !std::ranges::equal(bytes.first(localControlPreamble.size()), localControlPreamble)
        || bytes[localControlPreamble.size()] != localRemoteRelayReplyOperation || bytes[localControlPreamble.size() + 1] > 1)
        return std::nullopt;
    LocalRemoteRelayReply reply {
        .correlation = LocalControlDetail::read64(bytes.subspan<localControlPreamble.size() + 2, sizeof(std::uint64_t)>()),
        .daemonInstanceID = LocalControlDetail::read64(bytes.last<sizeof(std::uint64_t)>()),
        .isMarked = bytes[localControlPreamble.size() + 1] == 1,
    };
    return isValidLocalRemoteRelayReply(reply) ? std::optional(reply) : std::nullopt;
}

/// Selects a descriptor-free local reclaim exchange without granting authority.
[[nodiscard]] inline std::expected<std::array<std::uint8_t, localControlRequestBytes>, LocalControlError> encodeLocalReclaimRequest(std::uint64_t correlation)
{
    if (!correlation)
        return std::unexpected(LocalControlError::InvalidIdentity);
    return LocalControlDetail::encodeRequest(LocalControlOperation::SessionReclaim, correlation);
}

/// Encodes the frozen single reply to a credential reload.
[[nodiscard]] inline std::array<std::uint8_t, localControlReplyBytes> encodeLocalControlReply(const LocalControlReply& reply)
{
    std::array<std::uint8_t, localControlReplyBytes> bytes {};
    std::ranges::copy(localControlPreamble, bytes.begin());
    bytes[localControlPreamble.size()] = localReloadReplyOperation;
    bytes[localControlPreamble.size() + 1] = std::to_underlying(reply.status);
    LocalControlDetail::write64(std::span { bytes }.subspan<localControlPreamble.size() + 2, sizeof(std::uint64_t)>(), reply.correlation);
    LocalControlDetail::write64(std::span { bytes }.last<sizeof(std::uint64_t)>(), reply.daemonInstanceID);
    return bytes;
}

/// Decodes exactly one reload reply, refusing unknown or incomplete packets.
[[nodiscard]] inline std::optional<LocalControlReply> decodeLocalControlReply(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != localControlReplyBytes || !std::ranges::equal(bytes.first(localControlPreamble.size()), localControlPreamble)
        || bytes[localControlPreamble.size()] != localReloadReplyOperation
        || bytes[localControlPreamble.size() + 1] > std::to_underlying(LocalControlStatus::Refused))
        return std::nullopt;
    LocalControlReply reply {
        .correlation = LocalControlDetail::read64(bytes.subspan<localControlPreamble.size() + 2, sizeof(std::uint64_t)>()),
        .daemonInstanceID = LocalControlDetail::read64(bytes.last<sizeof(std::uint64_t)>()),
        .status = static_cast<LocalControlStatus>(bytes[localControlPreamble.size() + 1]),
    };
    return reply.correlation && reply.daemonInstanceID ? std::optional(reply) : std::nullopt;
}

/// Encodes a complete grant or a secret-free refusal within the fixed reply bound.
[[nodiscard]] inline std::expected<EncodedLocalQUICTokenReply, LocalControlError> encodeLocalQUICTokenReply(const LocalQUICTokenReply& reply)
{
    if (!reply.correlation || !reply.daemonInstanceID)
        return std::unexpected(LocalControlError::InvalidIdentity);
    if (reply.status > LocalQUICTokenStatus::Refused)
        return std::unexpected(LocalControlError::Malformed);
    const bool isGranted = reply.status == LocalQUICTokenStatus::Granted;
    if (isGranted) {
        if (!reply.port || !reply.tokenLength || reply.tokenLength > maxQUICTokenLength)
            return std::unexpected(LocalControlError::InvalidGrant);
    } else if (reply.port || reply.tokenLength || std::ranges::any_of(reply.token, [](auto byte) { return byte != 0; })
        || std::ranges::any_of(reply.certificateSHA256, [](auto byte) { return byte != 0; }))
        return std::unexpected(LocalControlError::InvalidGrant);
    EncodedLocalQUICTokenReply encoded;
    encoded.length = localControlReplyBytes;
    std::ranges::copy(localControlPreamble, encoded.bytes.begin());
    encoded.bytes[localControlPreamble.size()] = localQUICTokenReplyOperation;
    encoded.bytes[localControlPreamble.size() + 1] = std::to_underlying(reply.status);
    LocalControlDetail::write64(std::span { encoded.bytes }.subspan<localControlPreamble.size() + 2, sizeof(std::uint64_t)>(), reply.correlation);
    LocalControlDetail::write64(std::span { encoded.bytes }.subspan<localControlReplyBytes - sizeof(std::uint64_t), sizeof(std::uint64_t)>(), reply.daemonInstanceID);
    if (isGranted) {
        encoded.bytes[encoded.length++] = static_cast<std::uint8_t>(reply.port);
        encoded.bytes[encoded.length++] = static_cast<std::uint8_t>(reply.port >> 8);
        encoded.bytes[encoded.length++] = reply.tokenLength;
        std::ranges::copy(std::span { reply.token }.first(reply.tokenLength), encoded.bytes.begin() + encoded.length);
        encoded.length += reply.tokenLength;
        std::ranges::copy(reply.certificateSHA256, encoded.bytes.begin() + encoded.length);
        encoded.length += reply.certificateSHA256.size();
    }
    return encoded;
}

/// Decodes exactly one reply, allocating no memory from a received length.
[[nodiscard]] inline std::expected<LocalQUICTokenReply, LocalControlError> decodeLocalQUICTokenReply(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < localControlReplyBytes || bytes.size() > maxLocalQUICTokenReplyBytes
        || !std::ranges::equal(bytes.first(localControlPreamble.size()), localControlPreamble)
        || bytes[localControlPreamble.size()] != localQUICTokenReplyOperation
        || bytes[localControlPreamble.size() + 1] > std::to_underlying(LocalQUICTokenStatus::Refused))
        return std::unexpected(LocalControlError::Malformed);
    LocalQUICTokenReply reply {
        .correlation = LocalControlDetail::read64(bytes.subspan<localControlPreamble.size() + 2, sizeof(std::uint64_t)>()),
        .daemonInstanceID = LocalControlDetail::read64(bytes.subspan<localControlReplyBytes - sizeof(std::uint64_t), sizeof(std::uint64_t)>()),
        .status = static_cast<LocalQUICTokenStatus>(bytes[localControlPreamble.size() + 1]),
    };
    if (!reply.correlation || !reply.daemonInstanceID)
        return std::unexpected(LocalControlError::InvalidIdentity);
    if (reply.status != LocalQUICTokenStatus::Granted) {
        if (bytes.size() != localControlReplyBytes)
            return std::unexpected(LocalControlError::InvalidGrant);
        return reply;
    }
    constexpr auto grantHeaderBytes = localControlReplyBytes + sizeof(std::uint16_t) + 1;
    if (bytes.size() < grantHeaderBytes)
        return std::unexpected(LocalControlError::Malformed);
    reply.port = static_cast<std::uint16_t>(bytes[localControlReplyBytes]) | static_cast<std::uint16_t>(bytes[localControlReplyBytes + 1] << 8);
    reply.tokenLength = bytes[grantHeaderBytes - 1];
    if (!reply.port || !reply.tokenLength || reply.tokenLength > maxQUICTokenLength
        || bytes.size() != grantHeaderBytes + reply.tokenLength + certificateSHA256Length)
        return std::unexpected(LocalControlError::InvalidGrant);
    std::ranges::copy(bytes.subspan(grantHeaderBytes, reply.tokenLength), reply.token.begin());
    std::ranges::copy(bytes.last(certificateSHA256Length), reply.certificateSHA256.begin());
    return reply;
}

/// Holds one fixed first request without consuming ordinary carrier bytes.
class LocalControlReader {
public:
    /// Names the first packet's route and one-request decoding lifecycle.
    enum class State : std::uint8_t {
        Awaiting = 0,
        Ordinary = 1,
        Ready = 2,
        Refused = 3,
        Consumed = 4
    };

    /// Selects a route once and refuses malformed, oversized, or repeated requests.
    [[nodiscard]] State consume(std::span<const std::uint8_t> bytes)
    {
        m_consumedBytes = 0;
        if (m_state != State::Awaiting || bytes.empty())
            return m_state;
        if (!m_received && bytes.front() != localControlPreamble.front())
            return m_state = State::Ordinary;
        if (bytes.size() > m_bytes.size() - m_received) {
            const auto operation = m_received > localControlPreamble.size()
                ? m_bytes[localControlPreamble.size()]
                : bytes[localControlPreamble.size() - m_received];
            if (operation != localClipboardOperation && operation != localRemoteRelayOperation)
                return m_state = State::Refused;
        }
        const auto prefix = bytes.first(std::min(bytes.size(), m_bytes.size() - m_received));
        for (const auto byte : prefix) {
            if ((m_received < localControlPreamble.size() && byte != localControlPreamble[m_received])
                || (m_received == localControlPreamble.size() && byte != localReloadOperation && byte != localQUICTokenOperation
                    && byte != localHandoffOperation && byte != localClipboardOperation && byte != localReclaimOperation
                    && byte != localRemoteRelayOperation))
                return m_state = State::Refused;
            m_bytes[m_received++] = byte;
            ++m_consumedBytes;
        }
        if (m_received != m_bytes.size())
            return m_state;
        m_request = { .operation = static_cast<LocalControlOperation>(m_bytes[localControlPreamble.size()]),
            .correlation = LocalControlDetail::read64(std::span { m_bytes }.last<sizeof(std::uint64_t)>()) };
        if (bytes.size() != m_consumedBytes && m_request.operation != LocalControlOperation::Clipboard
            && m_request.operation != LocalControlOperation::RemoteRelay)
            return m_state = State::Refused;
        return m_state = m_request.correlation ? State::Ready : State::Refused;
    }

    /// Transfers the complete operation once, never authorizing a second packet.
    [[nodiscard]] std::optional<LocalControlRequest> take()
    {
        if (m_state != State::Ready)
            return std::nullopt;
        m_state = State::Consumed;
        return m_request;
    }

    /// Hands the connection to the ordinary carrier after a consumed
    /// RemoteRelay mark, so every later byte is the client's own stream.
    ///
    /// Any other operation keeps the connection to its one request; calling
    /// this for one leaves it refused.
    void continueAsCarrier()
    {
        m_state = m_state == State::Consumed && m_request.operation == LocalControlOperation::RemoteRelay ? State::Ordinary : State::Refused;
    }

    [[nodiscard]] std::size_t pendingBytes() const { return m_received; }
    /// The prefix consumed on the last call; only a body-bearing operation may
    /// leave a suffix for its permanently selected operation reader.
    [[nodiscard]] std::size_t consumedBytes() const { return m_consumedBytes; }
    [[nodiscard]] bool hasPendingRequest() const { return m_state == State::Awaiting && m_received; }

private:
    std::array<std::uint8_t, localControlRequestBytes> m_bytes {};
    std::size_t m_received = 0;
    std::size_t m_consumedBytes = 0;
    LocalControlRequest m_request;
    State m_state = State::Awaiting;
};

/// Accumulates one exactly correlated reply within fixed storage.
class LocalQUICTokenReplyReader {
public:
    /// Names the bounded reply's decoding and one-consumer lifecycle.
    enum class State : std::uint8_t {
        Awaiting = 0,
        Ready = 1,
        Refused = 2,
        Consumed = 3
    };

    explicit LocalQUICTokenReplyReader(std::uint64_t correlation)
        : m_correlation(correlation)
        , m_state(correlation ? State::Awaiting : State::Refused)
    {
    }

    /// Refuses all trailing bytes, including a later chunk after a complete reply.
    [[nodiscard]] State consume(std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty())
            return m_state;
        if (m_state != State::Awaiting || bytes.size() > m_bytes.size() - m_received)
            return m_state = State::Refused;
        for (const auto byte : bytes) {
            if ((m_received < localControlPreamble.size() && byte != localControlPreamble[m_received])
                || (m_received == localControlPreamble.size() && byte != localQUICTokenReplyOperation)
                || (m_received == localControlPreamble.size() + 1 && byte > std::to_underlying(LocalQUICTokenStatus::Refused)))
                return m_state = State::Refused;
            m_bytes[m_received++] = byte;
            if (m_received >= localControlReplyBytes) {
                const bool isGranted = m_bytes[localControlPreamble.size() + 1] == std::to_underlying(LocalQUICTokenStatus::Granted);
                std::size_t expected = localControlReplyBytes;
                if (isGranted) {
                    constexpr auto grantHeaderBytes = localControlReplyBytes + sizeof(std::uint16_t) + 1;
                    if (m_received < grantHeaderBytes)
                        continue;
                    const auto tokenLength = m_bytes[grantHeaderBytes - 1];
                    if (!tokenLength || tokenLength > maxQUICTokenLength)
                        return m_state = State::Refused;
                    expected = grantHeaderBytes + tokenLength + certificateSHA256Length;
                }
                if (m_received == expected) {
                    const auto reply = decodeLocalQUICTokenReply(std::span { m_bytes }.first(m_received));
                    if (!reply || reply->correlation != m_correlation)
                        return m_state = State::Refused;
                    m_reply = *reply;
                    m_state = State::Ready;
                } else if (m_received > expected)
                    return m_state = State::Refused;
            }
        }
        return m_state;
    }

    /// Transfers one complete answer after every received byte was validated.
    [[nodiscard]] std::optional<LocalQUICTokenReply> take()
    {
        if (m_state != State::Ready)
            return std::nullopt;
        m_state = State::Consumed;
        return m_reply;
    }

    [[nodiscard]] std::size_t pendingBytes() const { return m_received; }

private:
    std::array<std::uint8_t, maxLocalQUICTokenReplyBytes> m_bytes {};
    std::uint64_t m_correlation;
    std::size_t m_received = 0;
    LocalQUICTokenReply m_reply;
    State m_state;
};

} // namespace Ubiquitty::Protocol::Local
