// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/IDText.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::Protocol::Control {

/// Pins a reclaim to the pane and physical attachment that actually requested it.
struct ReclaimTarget {
    std::uint32_t handle = 0;
    std::uint32_t sessionID = 0;
    std::uint64_t daemonInstanceID = 0;
    Sync::ID128 hostInstanceID;
    Sync::ID128 attachmentID;
    Sync::ID128 paneID;
    bool operator==(const ReclaimTarget&) const = default;
};

/// The bounded selectors a caller may supply; none establishes authority.
struct ReclaimRequest {
    std::uint32_t handle = 0;
    std::uint64_t daemonInstanceID = 0;
};

/// Reads the exact numeric handle and canonical decimal daemon pin.
[[nodiscard]] inline std::optional<ReclaimRequest> readReclaimRequest(const Codec::JSON::Value& parameters)
{
    const auto* pane = parameters.find("pane");
    const auto* daemon = parameters.find("daemonInstance");
    if (!parameters.isObject() || parameters.members().size() != 2 || !pane || !pane->isNumber()
        || !std::isfinite(pane->asNumber()) || pane->asNumber() < 1 || pane->asNumber() > UINT32_MAX
        || std::trunc(pane->asNumber()) != pane->asNumber() || !daemon || !daemon->isString())
        return std::nullopt;
    const auto& text = daemon->asString();
    std::uint64_t incarnation = 0;
    if (text.empty() || text.size() > 20 || text.front() == '0')
        return std::nullopt;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), incarnation);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() || !incarnation)
        return std::nullopt;
    return ReclaimRequest { .handle = static_cast<std::uint32_t>(pane->asNumber()), .daemonInstanceID = incarnation };
}

/// Builds a pane.reclaim request on an already authenticated physical connection.
[[nodiscard]] inline std::string reclaimRequest(const ReclaimRequest& intent, std::string_view callID)
{
    if (!intent.handle || !intent.daemonInstanceID || callID.empty())
        return {};
    using Codec::JSON::Value;
    auto request = Value::object();
    request.set("jsonrpc", Value::string("2.0"));
    request.set("id", Value::string(std::string(callID)));
    request.set("method", Value::string("pane.reclaim"));
    auto parameters = Value::object();
    parameters.set("pane", Value::number(intent.handle));
    parameters.set("daemonInstance", Value::string(std::to_string(intent.daemonInstanceID)));
    request.set("params", std::move(parameters));
    const auto text = Codec::JSON::serialize(request);
    return text.size() <= Sync::maxFrameBodyBytes ? text : std::string {};
}

/// Builds the exact released identity after the host resolved every selector.
[[nodiscard]] inline Codec::JSON::Value reclaimReleasedResult(const Codec::JSON::Value* callID, const ReclaimTarget& target)
{
    using Codec::JSON::Value;
    auto result = Value::object();
    result.set("pane", Value::number(target.handle));
    result.set("session", Value::number(target.sessionID));
    result.set("daemonInstance", Value::string(std::to_string(target.daemonInstanceID)));
    result.set("host", Value::string(Sync::formatID128(target.hostInstanceID)));
    result.set("attachment", Value::string(Sync::formatID128(target.attachmentID)));
    result.set("paneIdentity", Value::string(Sync::formatID128(target.paneID)));
    result.set("outcome", Value::string("released"));
    return rpcResult(callID, std::move(result));
}

/// Accepts success only for this full pending identity and exact correlated Result.
[[nodiscard]] inline bool isReclaimReleasedResult(const Codec::JSON::Value& answer, std::string_view callID, const ReclaimTarget& target)
{
    if (!target.handle || !target.sessionID || !target.daemonInstanceID || target.hostInstanceID == Sync::ID128 {}
        || target.attachmentID == Sync::ID128 {} || target.paneID == Sync::ID128 {}
        || !answer.isObject() || answer.members().size() != 3 || answer.stringOr("jsonrpc") != "2.0"
        || answer.stringOr("id") != callID || answer.find("error"))
        return false;
    const auto* result = answer.find("result");
    if (!result || !result->isObject() || result->members().size() != 7 || result->stringOr("outcome") != "released")
        return false;
    const auto* pane = result->find("pane");
    const auto* session = result->find("session");
    return pane && pane->isNumber() && pane->asNumber() == target.handle
        && session && session->isNumber() && session->asNumber() == target.sessionID
        && result->stringOr("daemonInstance") == std::to_string(target.daemonInstanceID)
        && Sync::parseID128(result->stringOr("host")) == target.hostInstanceID
        && Sync::parseID128(result->stringOr("attachment")) == target.attachmentID
        && Sync::parseID128(result->stringOr("paneIdentity")) == target.paneID;
}

} // namespace Ubiquitty::Protocol::Control
