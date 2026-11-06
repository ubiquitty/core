// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/Consent.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/IDText.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/Identity.h"

#include <array>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::Protocol::Control {

/// A decision about one immutable host-owned question, not new authority fields.
struct ConsentResponse {
    std::uint32_t requestID = 0;
    Sync::ConsentKind kind = Sync::ConsentKind::Attach;
    Sync::ConsentChoice decision = Sync::ConsentChoice::Deny;
    friend bool operator==(const ConsentResponse&, const ConsentResponse&) = default;
};

/// An exact subscribed session and only the selector this revocation needs.
struct ConsentRevocation {
    std::uint32_t sessionID = 0;
    Sync::ConsentRevocationKind action = Sync::ConsentRevocationKind::DisconnectNow;
    Sync::ID128 attachmentID;
    /// A verified grant key from presence, never the claimed display label.
    std::string identity;
    friend bool operator==(const ConsentRevocation&, const ConsentRevocation&) = default;
};

namespace ConsentDetail {

inline constexpr std::array<std::string_view, 4> kinds { "attach", "terminate", "capability", "structure" };
inline constexpr std::array<std::string_view, 5> decisions { "deny", "once", "session", "always", "unshown" };
inline constexpr std::array<std::string_view, 4> revocations { "disconnect", "forgetAlways", "forgetSession", "disconnectAgents" };

inline bool exactMembers(const Codec::JSON::Value& value, std::initializer_list<std::string_view> names)
{
    if (!value.isObject() || value.members().size() != names.size())
        return false;
    for (auto name : names) {
        std::size_t count = 0;
        for (const auto& member : value.members())
            count += member.first == name;
        if (count != 1)
            return false;
    }
    return true;
}

template<typename Enum>
inline std::optional<Enum> readWord(const Codec::JSON::Value* value, std::span<const std::string_view> words)
{
    if (!value || !value->isString())
        return std::nullopt;
    for (std::size_t index = 0; index < words.size(); ++index) {
        if (value->asString() == words[index])
            return static_cast<Enum>(index);
    }
    return std::nullopt;
}

inline bool text(std::string_view value, std::size_t maximum)
{
    return value.size() <= maximum && Sync::isValidUTF8({ reinterpret_cast<const std::uint8_t*>(value.data()), value.size() });
}

/// Counts the JSON payload bytes of already validated UTF-8, without copying it.
inline std::size_t escapedBytes(std::string_view value)
{
    std::size_t size = 0;
    for (unsigned char byte : value) {
        if (byte == '"' || byte == '\\' || byte == '\b' || byte == '\f' || byte == '\n' || byte == '\r' || byte == '\t')
            size += 2;
        else
            size += byte < 0x20 ? 6 : 1;
    }
    return size;
}

/// Reads the shared required identity of requested and resolved human notifies.
/// Each caller also enforces its complete kind-specific exact member set.
inline std::optional<Sync::ConsentResolution> noticeIdentity(const Codec::JSON::Value& parameters, std::string_view phase)
{
    const auto* suppliedPhase = parameters.find("phase");
    const auto* request = parameters.find("request");
    const auto kind = readWord<Sync::ConsentKind>(parameters.find("kind"), kinds);
    const auto* session = parameters.find("session");
    const auto target = session && session->isString() && session->asString().size() <= 18
        ? Session::Identity::parseObjectID(session->asString())
        : std::nullopt;
    if (!suppliedPhase || !suppliedPhase->isString() || suppliedPhase->asString() != phase || !kind
        || !request || !request->isNumber() || !std::isfinite(request->asNumber())
        || request->asNumber() < 1 || request->asNumber() > UINT32_MAX || std::floor(request->asNumber()) != request->asNumber()
        || !target || target->kind != Session::Identity::ObjectKind::Session || !target->number || target->number > UINT32_MAX)
        return std::nullopt;
    return Sync::ConsentResolution { *kind, static_cast<std::uint32_t>(request->asNumber()), static_cast<std::uint32_t>(target->number) };
}

inline std::string request(std::string_view method, Codec::JSON::Value parameters, std::string_view requestID)
{
    if (requestID.empty() || !text(requestID, Sync::maxFrameBodyBytes))
        return {};
    using Codec::JSON::Value;
    auto call = Value::object();
    call.set("jsonrpc", Value::string("2.0"));
    call.set("id", Value::string(std::string(requestID)));
    call.set("method", Value::string(std::string(method)));
    call.set("params", std::move(parameters));
    auto encoded = Codec::JSON::serialize(call);
    return encoded.size() <= Sync::maxFrameBodyBytes ? std::move(encoded) : std::string {};
}

} // namespace ConsentDetail

/// Reads an exact kind-bound decision before the host looks up its own record.
/// The adapter must match both kind and requestID, recheck the live recipient,
/// and retain Structure's Owner/Driver plus Drive gate immediately before effect.
/// - Parameter parameters: The canonical Call's params object.
/// - Returns: A decision, or nothing for malformed fields or a standing tree grant.
[[nodiscard]] inline std::optional<ConsentResponse> readConsentResponse(const Codec::JSON::Value& parameters)
{
    if (!ConsentDetail::exactMembers(parameters, { "request", "kind", "decision" }))
        return std::nullopt;
    const auto* request = parameters.find("request");
    const auto kind = ConsentDetail::readWord<Sync::ConsentKind>(parameters.find("kind"), ConsentDetail::kinds);
    const auto decision = ConsentDetail::readWord<Sync::ConsentChoice>(parameters.find("decision"), ConsentDetail::decisions);
    if (!request || !request->isNumber() || !std::isfinite(request->asNumber())
        || request->asNumber() < 1 || request->asNumber() > UINT32_MAX || std::trunc(request->asNumber()) != request->asNumber()
        || !kind || !decision
        || (*kind == Sync::ConsentKind::Structure && *decision != Sync::ConsentChoice::Deny && *decision != Sync::ConsentChoice::AllowOnce && *decision != Sync::ConsentChoice::Unshown))
        return std::nullopt;
    return ConsentResponse { static_cast<std::uint32_t>(request->asNumber()), *kind, *decision };
}

/// Reads only the selector required by one exact revocation action. The adapter
/// still proves its live human subscription and resolves the verified key or
/// stable attachment on this serving daemon; syntax is never that authority.
/// - Parameter parameters: The canonical Call's params object.
/// - Returns: Its bounded intent, or nothing for extra, absent or mixed selectors.
[[nodiscard]] inline std::optional<ConsentRevocation> readConsentRevocation(const Codec::JSON::Value& parameters)
{
    if (!parameters.isObject() || parameters.members().size() < 2 || parameters.members().size() > 3)
        return std::nullopt;
    const auto action = ConsentDetail::readWord<Sync::ConsentRevocationKind>(parameters.find("action"), ConsentDetail::revocations);
    const auto* session = parameters.find("session");
    if (!action || !session || !session->isString() || session->asString().size() > 18)
        return std::nullopt;
    const auto target = Session::Identity::parseObjectID(session->asString());
    if (!target || target->kind != Session::Identity::ObjectKind::Session || target->number > UINT32_MAX)
        return std::nullopt;
    ConsentRevocation result { .sessionID = static_cast<std::uint32_t>(target->number), .action = *action, .attachmentID = {}, .identity = {} };
    switch (*action) {
    case Sync::ConsentRevocationKind::DisconnectNow: {
        if (!ConsentDetail::exactMembers(parameters, { "session", "action", "attachment" }))
            return std::nullopt;
        const auto* attachment = parameters.find("attachment");
        const auto identity = attachment && attachment->isString() ? Sync::parseID128(attachment->asString()) : std::nullopt;
        if (!identity)
            return std::nullopt;
        result.attachmentID = *identity;
        break;
    }
    case Sync::ConsentRevocationKind::StopAlways:
    case Sync::ConsentRevocationKind::SessionGrant: {
        if (!ConsentDetail::exactMembers(parameters, { "session", "action", "identity" }))
            return std::nullopt;
        const auto* identity = parameters.find("identity");
        if (!identity || !identity->isString() || identity->asString().empty()
            || !ConsentDetail::text(identity->asString(), Sync::maxConsentIdentityBytes) || identity->asString().find('\0') != std::string::npos)
            return std::nullopt;
        result.identity = identity->asString();
        break;
    }
    case Sync::ConsentRevocationKind::DisconnectAll:
        if (!ConsentDetail::exactMembers(parameters, { "session", "action" }))
            return std::nullopt;
        break;
    }
    return result;
}

/// Builds a bounded, correlated answer about the exact retained question.
/// - Parameters:
///   - response: Its nonzero identity, kind and explicit decision.
///   - requestID: The connection-unique nonempty UTF-8 Call correlation.
/// - Returns: The complete current request, or empty for invalid or oversize input.
[[nodiscard]] inline std::string consentResponseRequest(const ConsentResponse& response, std::string_view requestID)
{
    if (requestID.empty() || !ConsentDetail::text(requestID, Sync::maxFrameBodyBytes))
        return {};
    const auto kind = static_cast<std::size_t>(response.kind);
    const auto decision = static_cast<std::size_t>(response.decision);
    if (kind >= ConsentDetail::kinds.size() || decision >= ConsentDetail::decisions.size())
        return {};
    using Codec::JSON::Value;
    auto parameters = Value::object();
    parameters.set("request", Value::number(response.requestID));
    parameters.set("kind", Value::string(std::string(ConsentDetail::kinds[kind])));
    parameters.set("decision", Value::string(std::string(ConsentDetail::decisions[decision])));
    if (!readConsentResponse(parameters))
        return {};
    return ConsentDetail::request("consent.respond", std::move(parameters), requestID);
}

/// Builds only the exact stable selector this revocation action uses.
/// - Parameters:
///   - revocation: Its exact subscribed session, action and selector.
///   - requestID: The connection-unique nonempty UTF-8 Call correlation.
/// - Returns: The complete current request, or empty for contradictory input.
[[nodiscard]] inline std::string consentRevocationRequest(const ConsentRevocation& revocation, std::string_view requestID)
{
    if (requestID.empty() || !ConsentDetail::text(requestID, Sync::maxFrameBodyBytes))
        return {};
    const auto action = static_cast<std::size_t>(revocation.action);
    if (!revocation.sessionID || action >= ConsentDetail::revocations.size())
        return {};
    using Codec::JSON::Value;
    auto parameters = Value::object();
    parameters.set("session", Value::string("session:" + std::to_string(revocation.sessionID)));
    parameters.set("action", Value::string(std::string(ConsentDetail::revocations[action])));
    switch (revocation.action) {
    case Sync::ConsentRevocationKind::DisconnectNow:
        if (!revocation.identity.empty() || revocation.attachmentID == Sync::ID128 {})
            return {};
        parameters.set("attachment", Value::string(Sync::formatID128(revocation.attachmentID)));
        break;
    case Sync::ConsentRevocationKind::StopAlways:
    case Sync::ConsentRevocationKind::SessionGrant:
        if (revocation.attachmentID != Sync::ID128 {} || revocation.identity.empty()
            || !ConsentDetail::text(revocation.identity, Sync::maxConsentIdentityBytes) || revocation.identity.find('\0') != std::string::npos)
            return {};
        parameters.set("identity", Value::string(revocation.identity));
        break;
    case Sync::ConsentRevocationKind::DisconnectAll:
        if (!revocation.identity.empty() || revocation.attachmentID != Sync::ID128 {})
            return {};
        break;
    }
    return ConsentDetail::request("consent.revoke", std::move(parameters), requestID);
}

} // namespace Ubiquitty::Protocol::Control
