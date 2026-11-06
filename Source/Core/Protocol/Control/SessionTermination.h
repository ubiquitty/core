// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Protocol/ControlRPC.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::Protocol::Control {

/// A correlated termination result; transport silence never establishes success.
enum class TerminationOutcome : std::uint8_t {
    Terminated,
    Forgotten,
    Denied,
    TimedOut,
    Refused,
    Failed,
};

/// The two existing authorities have distinct methods, never an OR-scope gate.
enum class TerminationAuthority : std::uint8_t {
    Control,
    Management,
};

/// Encodes an exact session termination request.
/// - Parameters:
///   - sessionID: The nonzero session or remembered workspace identity.
///   - requestID: A connection-unique correlation string.
///   - authority: The existing control or human-administration operation.
/// - Returns: The JSON-RPC request, or empty for an invalid identity.
[[nodiscard]] inline std::string terminationRequest(std::uint32_t sessionID, std::string_view requestID, TerminationAuthority authority = TerminationAuthority::Control)
{
    using Codec::JSON::Value;
    if (!sessionID || requestID.empty())
        return {};
    auto request = Value::object();
    request.set("jsonrpc", Value::string("2.0"));
    request.set("id", Value::string(std::string(requestID)));
    request.set("method", Value::string(authority == TerminationAuthority::Management ? "host.session.terminate" : "session.terminate"));
    auto parameters = Value::object();
    parameters.set("session", Value::number(sessionID));
    request.set("params", std::move(parameters));
    return Codec::JSON::serialize(request);
}

/// Reads only the answer to the named termination.
/// - Parameters:
///   - answer: A decoded JSON-RPC answer.
///   - requestID: The exact correlation string.
///   - sessionID: The requested identity, never inferred from a current pane.
/// - Returns: No value for another request; failed for malformed matching data.
[[nodiscard]] inline std::optional<TerminationOutcome> terminationOutcome(const Codec::JSON::Value& answer, std::string_view requestID, std::uint32_t sessionID)
{
    const auto* identity = answer.find("id");
    if (!identity || !identity->isString() || identity->asString() != requestID)
        return std::nullopt;
    if (answer.stringOr("jsonrpc") != "2.0")
        return TerminationOutcome::Failed;
    const auto* result = answer.find("result");
    if (const auto* error = answer.find("error"))
        return !result && error->isObject() ? TerminationOutcome::Refused : TerminationOutcome::Failed;
    if (!result || !result->isObject())
        return TerminationOutcome::Failed;
    const auto* target = result->find("session");
    if (!target || !target->isNumber() || !sessionID || target->asNumber() != sessionID)
        return TerminationOutcome::Failed;
    const auto outcome = result->stringOr("outcome");
    if (outcome == "terminated")
        return TerminationOutcome::Terminated;
    if (outcome == "forgotten")
        return TerminationOutcome::Forgotten;
    if (outcome == "denied")
        return TerminationOutcome::Denied;
    if (outcome == "timed-out")
        return TerminationOutcome::TimedOut;
    if (outcome == "refused")
        return TerminationOutcome::Refused;
    return TerminationOutcome::Failed;
}

} // namespace Ubiquitty::Protocol::Control
