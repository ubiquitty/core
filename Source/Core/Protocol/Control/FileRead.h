// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/Base64.h"
#include "Core/Codec/JSON.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>

namespace Ubiquitty::Protocol::Control {

/// File bytes before base64 encoding; the complete result fits the call bulk budget.
inline constexpr std::size_t maxReadFileBytes = 4u * 1024u * 1024u;
/// Paths retain the file reader's existing length bound.
inline constexpr std::size_t maxReadFilePathBytes = 65535;

/// The existing file-confinement outcomes, also used by native presentation.
enum class FileReadStatus : std::uint8_t {
    OK,
    NotFound,
    TooLarge,
    OutsideConfinement,
    NotRegular,
    Denied,
};

/// The stable JSON spelling of a file outcome.
/// - Parameter status: A validated outcome.
/// - Returns: Its name, or empty for an invalid enum value.
[[nodiscard]] inline std::string_view fileReadStatusName(FileReadStatus status)
{
    constexpr std::array<std::string_view, 6> names { "ok", "not-found", "too-large", "outside-confinement", "not-regular", "denied" };
    const auto index = static_cast<std::size_t>(status);
    return index < names.size() ? names[index] : std::string_view {};
}

/// One exact, move-only result. The receiver transfers its byte owner to the
/// native consumer instead of copying a second input-sized vector.
struct FileReadReply {
    std::uint32_t requestID = 0;
    FileReadStatus status = FileReadStatus::Denied;
    std::string path;
    std::unique_ptr<std::uint8_t[]> storage;
    std::size_t length = 0;

    /// Borrows the complete file until this owner is moved or destroyed.
    /// - Returns: The bytes; empty for every refused result.
    [[nodiscard]] std::span<const std::uint8_t> data() const { return { storage.get(), length }; }
};

/// A reply that cannot be delivered as the requested file.
enum class FileReadFailure : std::uint8_t {
    Unrelated,
    Invalid,
    AllocationRefused,
};

/// Encodes a bound-pane read without widening its confinement policy.
/// - Parameters:
///   - pane: The exact connection-local bound handle.
///   - path: The requested path, without embedded NUL.
///   - requestID: A unique string correlation on this physical connection.
/// - Returns: The request, or empty for invalid parameters.
[[nodiscard]] inline std::string fileReadRequest(std::uint32_t pane, std::string_view path, std::string_view requestID)
{
    using Codec::JSON::Value;
    if (!pane || requestID.empty() || path.empty() || path.size() > maxReadFilePathBytes || path.find('\0') != std::string_view::npos)
        return {};
    auto request = Value::object();
    request.set("jsonrpc", Value::string("2.0"));
    request.set("id", Value::string(std::string(requestID)));
    request.set("method", Value::string("file.read"));
    auto parameters = Value::object();
    parameters.set("pane", Value::number(pane));
    parameters.set("path", Value::string(std::string(path)));
    request.set("params", std::move(parameters));
    return Codec::JSON::serialize(request);
}

/// Validates the complete reply before allocating its exact decoded owner.
/// - Parameters:
///   - answer: The decoded response, including its JSON-RPC envelope.
///   - requestID: The pending call's string identity.
///   - pane: Its original connection-local handle.
///   - path: Its original path, never inferred from the current pane.
/// - Returns: A complete file/refusal, or an explicit decode failure. Unrelated
///   responses consume no pending request and allocate no file bytes.
[[nodiscard]] inline std::expected<FileReadReply, FileReadFailure> decodeFileReadReply(const Codec::JSON::Value& answer,
    std::string_view requestID, std::uint32_t pane, std::string_view path)
{
    const auto* id = answer.find("id");
    if (!id || !id->isString() || id->asString() != requestID)
        return std::unexpected(FileReadFailure::Unrelated);
    const auto repeated = [](const Codec::JSON::Value& value) {
        const auto& members = value.members();
        for (std::size_t index = 0; index < members.size(); ++index) {
            for (std::size_t before = 0; before < index; ++before) {
                if (members[before].first == members[index].first)
                    return true;
            }
        }
        return false;
    };
    if (!answer.isObject() || answer.members().size() != 3 || repeated(answer) || answer.stringOr("jsonrpc") != "2.0")
        return std::unexpected(FileReadFailure::Invalid);
    FileReadReply reply;
    reply.path = path;
    const auto* result = answer.find("result");
    if (const auto* error = answer.find("error")) {
        const auto* code = error->find("code");
        const auto* message = error->find("message");
        if (result || !error->isObject() || error->members().size() < 2 || error->members().size() > 3
            || (error->members().size() == 3 && !error->find("data")) || repeated(*error) || !code || !code->isNumber()
            || !std::isfinite(code->asNumber()) || std::trunc(code->asNumber()) != code->asNumber()
            || code->asNumber() < std::numeric_limits<std::int32_t>::min() || code->asNumber() > std::numeric_limits<std::int32_t>::max()
            || !message || !message->isString())
            return std::unexpected(FileReadFailure::Invalid);
        return reply;
    }
    if (!result || !result->isObject() || result->members().size() != 5 || repeated(*result))
        return std::unexpected(FileReadFailure::Invalid);
    const auto* handle = result->find("pane");
    const auto* echoedPath = result->find("path");
    const auto* status = result->find("status");
    const auto* bytes = result->find("bytes");
    const auto* data = result->find("data");
    if (!pane || !handle || !handle->isNumber() || handle->asNumber() != pane
        || !echoedPath || !echoedPath->isString() || echoedPath->asString() != path
        || !status || !status->isString() || !bytes || !bytes->isNumber()
        || !std::isfinite(bytes->asNumber()) || bytes->asNumber() < 0 || bytes->asNumber() > maxReadFileBytes
        || std::trunc(bytes->asNumber()) != bytes->asNumber() || !data || !data->isString())
        return std::unexpected(FileReadFailure::Invalid);
    bool found = false;
    for (unsigned candidate = 0; candidate <= static_cast<unsigned>(FileReadStatus::Denied); ++candidate) {
        if (status->asString() == fileReadStatusName(static_cast<FileReadStatus>(candidate))) {
            reply.status = static_cast<FileReadStatus>(candidate);
            found = true;
            break;
        }
    }
    const auto length = static_cast<std::size_t>(bytes->asNumber());
    if (!found || data->asString().size() != Codec::base64EncodedLength(length)
        || (reply.status != FileReadStatus::OK && length))
        return std::unexpected(FileReadFailure::Invalid);
    if (length) {
        reply.storage.reset(new (std::nothrow) std::uint8_t[length]);
        if (!reply.storage)
            return std::unexpected(FileReadFailure::AllocationRefused);
    }
    const auto decoded = Codec::base64Decode(data->asString(), { reinterpret_cast<char*>(reply.storage.get()), length });
    if (!decoded || decoded->size() != length)
        return std::unexpected(FileReadFailure::Invalid);
    reply.length = length;
    return reply;
}

} // namespace Ubiquitty::Protocol::Control
