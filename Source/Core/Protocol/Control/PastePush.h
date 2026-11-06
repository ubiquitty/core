// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Wire.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Protocol::Control {

/// Raw pasted bytes retain the existing carrier-independent payload ceiling.
inline constexpr std::size_t maxPastePushBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t maxPasteMimeBytes = 65535;
inline constexpr std::size_t maxPastePathBytes = 65535;

/// Native presentation values are stable while the wire uses these names.
enum class PastePushStatus : std::uint8_t {
    Landed = 0,
    TooLarge = 1,
    IOError = 2,
    Unsupported = 3,
    Busy = 4,
};

/// The exact pane and immutable content declared before upload admission.
struct PastePushRequest {
    std::uint32_t pane = 0;
    std::string mime;
    std::uint32_t bytes = 0;
    std::array<std::uint8_t, 32> sha256 {};
    friend bool operator==(const PastePushRequest&, const PastePushRequest&) = default;
};

/// Correlated upload credit, never a successful landing or a final Result.
struct PasteReady {
    std::string requestID;
    std::uint32_t pane = 0;
    std::uint32_t transfer = 0;
    friend bool operator==(const PasteReady&, const PasteReady&) = default;
};

/// One final presentation result; pushID is the native caller's own token.
struct PastePushReply {
    std::uint32_t pushID = 0;
    PastePushStatus status = PastePushStatus::IOError;
    std::string path {};
};

/// Why a response cannot settle the exact upload as a normal final result.
enum class PasteReplyFailure : std::uint8_t {
    Unrelated,
    Refused,
    Invalid,
};

namespace PastePushDetail {

/// Tests a finite JSON integer against inclusive bounds without narrowing it.
/// - Parameters:
///   - value: The optional incoming field.
///   - low: Its inclusive minimum.
///   - high: Its inclusive maximum.
/// - Returns: Whether the complete value lies within the integer range.
inline bool integer(const Codec::JSON::Value* value, double low, double high)
{
    return value && value->isNumber() && std::isfinite(value->asNumber())
        && value->asNumber() >= low && value->asNumber() <= high
        && std::trunc(value->asNumber()) == value->asNumber();
}

/// Checks bounded UTF-8 before a metadata copy.
/// - Parameters:
///   - value: Borrowed incoming text.
///   - maximum: The byte cap, not a character count.
/// - Returns: Whether both the byte bound and UTF-8 grammar hold.
inline bool text(std::string_view value, std::size_t maximum)
{
    return value.size() <= maximum && Sync::isValidUTF8({ reinterpret_cast<const std::uint8_t*>(value.data()), value.size() });
}

/// Encodes one SHA-256 in its fixed lowercase hexadecimal form.
/// - Parameter digest: The complete immutable digest.
/// - Returns: Exactly 64 ASCII bytes.
inline std::string digestText(const std::array<std::uint8_t, 32>& digest)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[2 * index] = hex[digest[index] >> 4];
        result[2 * index + 1] = hex[digest[index] & 15];
    }
    return result;
}

/// Reads only a complete canonical lowercase hexadecimal SHA-256.
/// - Parameter value: The optional incoming digest field.
/// - Returns: Its exact 32 bytes, or nothing for another representation.
inline std::optional<std::array<std::uint8_t, 32>> digest(const Codec::JSON::Value* value)
{
    if (!value || !value->isString() || value->asString().size() != 64)
        return std::nullopt;
    const auto nibble = [](char digit) -> std::optional<std::uint8_t> {
        if (digit >= '0' && digit <= '9')
            return static_cast<std::uint8_t>(digit - '0');
        if (digit >= 'a' && digit <= 'f')
            return static_cast<std::uint8_t>(digit - 'a' + 10);
        return std::nullopt;
    };
    std::array<std::uint8_t, 32> result;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto high = nibble(value->asString()[2 * index]);
        const auto low = nibble(value->asString()[2 * index + 1]);
        if (!high || !low)
            return std::nullopt;
        result[index] = static_cast<std::uint8_t>((*high << 4) | *low);
    }
    return result;
}

/// Checks the shared manifest invariants without allocating payload storage.
/// - Parameter request: The parsed or outgoing immutable manifest.
/// - Returns: Whether the pane, payload size and MIME satisfy their bounds.
inline bool valid(const PastePushRequest& request)
{
    return request.pane && request.bytes && request.bytes <= maxPastePushBytes && text(request.mime, maxPasteMimeBytes);
}

} // namespace PastePushDetail

/// Reads the small manifest before any payload-sized reservation or allocation.
/// - Parameter parameters: The complete paste.push params object.
/// - Returns: Exact immutable metadata, or nothing for malformed input.
[[nodiscard]] inline std::optional<PastePushRequest> pastePushParameters(const Codec::JSON::Value& parameters)
{
    const auto* mime = parameters.find("mime");
    const auto digest = PastePushDetail::digest(parameters.find("sha256"));
    if (!parameters.isObject() || parameters.members().size() != 4 || !mime || !mime->isString()
        || !PastePushDetail::text(mime->asString(), maxPasteMimeBytes) || !digest
        || !PastePushDetail::integer(parameters.find("pane"), 1, std::numeric_limits<std::uint32_t>::max())
        || !PastePushDetail::integer(parameters.find("bytes"), 1, maxPastePushBytes))
        return std::nullopt;
    return PastePushRequest { static_cast<std::uint32_t>(parameters.find("pane")->asNumber()), mime->asString(),
        static_cast<std::uint32_t>(parameters.find("bytes")->asNumber()), *digest };
}

/// Builds a bounded text-only Call; the body bytes wait for upload credit.
/// - Parameters:
///   - manifest: Exact target, MIME, byte count and digest.
///   - requestID: A nonempty connection-unique string correlation.
/// - Returns: The complete request, or empty without copying an oversized ID.
[[nodiscard]] inline std::string pastePushRequest(const PastePushRequest& manifest, std::string_view requestID)
{
    if (!PastePushDetail::valid(manifest) || requestID.empty() || !PastePushDetail::text(requestID, Sync::maxFrameBodyBytes))
        return {};
    using Codec::JSON::Value;
    auto parameters = Value::object();
    parameters.set("pane", Value::number(manifest.pane));
    parameters.set("mime", Value::string(manifest.mime));
    parameters.set("bytes", Value::number(manifest.bytes));
    parameters.set("sha256", Value::string(PastePushDetail::digestText(manifest.sha256)));
    auto request = Value::object();
    request.set("jsonrpc", Value::string("2.0"));
    request.set("id", Value::string(std::string(requestID)));
    request.set("method", Value::string("paste.push"));
    request.set("params", std::move(parameters));
    auto encoded = Codec::JSON::serialize(request);
    return encoded.size() <= Sync::maxFrameBodyBytes ? std::move(encoded) : std::string {};
}

/// Encodes credit only after the host admitted this exact upload owner, as a
/// `PasteReady` `Notify` body: `request:utf8 pane:u32 transfer:u32`.
/// - Parameter ready: The original call identity, pane and fresh transfer ID.
/// - Returns: The whole body, or nothing for unusable correlation.
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>> pasteReadyNotify(const PasteReady& ready)
{
    if (!ready.pane || !ready.transfer || ready.requestID.empty() || ready.requestID.find('\0') != std::string::npos
        || !PastePushDetail::text(ready.requestID, Sync::maxFrameBodyBytes)
        || Sync::varintBytes(std::to_underlying(Sync::NotifyKind::PasteReady)) + Sync::utf8Bytes(ready.requestID) + 4 + 4 > Sync::maxFrameBodyBytes)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    Sync::WireWriter writer(body);
    writer.varint(std::to_underlying(Sync::NotifyKind::PasteReady));
    writer.utf8(ready.requestID);
    writer.u32(ready.pane);
    writer.u32(ready.transfer);
    return body;
}

/// Reads a `PasteReady` record without admitting an unrelated upload.
/// - Parameter record: A `PasteReady` `Notify`'s record.
/// - Returns: Exact owned correlation and credit, or nothing when malformed.
[[nodiscard]] inline std::optional<PasteReady> decodePasteReady(std::span<const std::uint8_t> record)
{
    Sync::WireReader reader(record);
    const std::string_view request = reader.utf8(Sync::maxFrameBodyBytes);
    const std::uint32_t pane = reader.u32();
    const std::uint32_t transfer = reader.u32();
    if (!reader.ok() || !reader.isAtEnd() || request.empty() || request.find('\0') != std::string_view::npos || !pane || !transfer)
        return std::nullopt;
    return PasteReady { std::string(request), pane, transfer };
}

/// The canonical final status name; native numeric presentation stays stable.
/// - Parameter status: One of the existing five outcomes.
/// - Returns: Its name, or empty for an invalid enum value.
[[nodiscard]] inline std::string_view pastePushStatusName(PastePushStatus status)
{
    constexpr std::array<std::string_view, 5> names { "landed", "too-large", "io-error", "unsupported", "busy" };
    const auto index = static_cast<std::size_t>(status);
    return index < names.size() ? names[index] : std::string_view {};
}

/// Reads only the exact final answer; credit can never masquerade as success.
/// - Parameters:
///   - answer: The decoded JSON-RPC response object.
///   - requestID: Its original string correlation.
///   - pane: Its original physical connection's bound handle.
/// - Returns: A result, an unrelated identity, a call refusal or malformed data.
[[nodiscard]] inline std::expected<PastePushReply, PasteReplyFailure> decodePastePushReply(const Codec::JSON::Value& answer,
    std::string_view requestID, std::uint32_t pane)
{
    const auto* id = answer.find("id");
    if (!id || !id->isString() || id->asString() != requestID)
        return std::unexpected(PasteReplyFailure::Unrelated);
    if (!answer.isObject() || answer.members().size() != 3 || answer.stringOr("jsonrpc") != "2.0")
        return std::unexpected(PasteReplyFailure::Invalid);
    const auto* result = answer.find("result");
    if (const auto* error = answer.find("error")) {
        const auto* message = error->find("message");
        if (result || !error->isObject() || error->members().size() < 2 || error->members().size() > 3
            || (error->members().size() == 3 && !error->find("data")) || !message || !message->isString()
            || !PastePushDetail::integer(error->find("code"), std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()))
            return std::unexpected(PasteReplyFailure::Invalid);
        return std::unexpected(PasteReplyFailure::Refused);
    }
    const auto* path = result ? result->find("path") : nullptr;
    const auto* status = result ? result->find("status") : nullptr;
    if (!pane || !result || !result->isObject() || result->members().size() != 3
        || !PastePushDetail::integer(result->find("pane"), pane, pane) || !path || !path->isString()
        || !PastePushDetail::text(path->asString(), maxPastePathBytes) || !status || !status->isString())
        return std::unexpected(PasteReplyFailure::Invalid);
    for (unsigned candidate = 0; candidate <= static_cast<unsigned>(PastePushStatus::Busy); ++candidate) {
        const auto outcome = static_cast<PastePushStatus>(candidate);
        if (status->asString() == pastePushStatusName(outcome))
            return PastePushReply { .status = outcome, .path = path->asString() };
    }
    return std::unexpected(PasteReplyFailure::Invalid);
}

} // namespace Ubiquitty::Protocol::Control
