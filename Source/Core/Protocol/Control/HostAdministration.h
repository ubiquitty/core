// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/HostManagement.h"

#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty::Protocol::Control {

/// Bounds the decoded administration result before JSON parsing or record copying.
inline constexpr std::size_t maxAdministrationResultBytes = Sync::maxManagementPayload;
/// Bounds each administration collection independently.
inline constexpr std::size_t maxAdministrationRecords = Sync::maxManagementEntries;
/// Bounds one host-owned administration string.
inline constexpr std::size_t maxAdministrationStringBytes = Sync::maxManagementString;

/// Names an active administration method, excluding the separately owned termination call.
/// - Parameter operation: The frontend's administration intent.
/// - Returns: Its typed method, or empty for an operation this family does not serve.
[[nodiscard]] std::string_view hostAdministrationMethod(Sync::ManagementOperation operation);

/// Whether a method belongs to this administration family.
/// - Parameter method: The exact typed method name.
/// - Returns: Whether it is one of the five active operations.
[[nodiscard]] bool isHostAdministrationMethod(std::string_view method);

/// Checks an intent before a control connection learns its daemon incarnation.
/// - Parameter request: The frontend-owned operation and selectors.
/// - Returns: Whether this family serves the bounded intent; a pin is checked separately.
[[nodiscard]] bool isValidHostAdministrationIntent(const Sync::ManagementRequest& request);

/// Bounds a mutation's complete answer before any operation changes host-owned state.
/// - Parameters:
///   - request: The admitted, pinned mutation and exact selectors.
///   - callID: The JSON-RPC identity to echo.
/// - Returns: The worst bounded answer size, or no value for a snapshot or invalid intent.
[[nodiscard]] std::optional<std::size_t> hostAdministrationMutationAnswerBytes(const Sync::ManagementRequest& request, const Codec::JSON::Value* callID);

/// Checks response capacity, not permission, before a mutation can take effect.
/// - Parameters:
///   - answerBytes: The complete worst-case JSON-RPC response size.
///   - queuedBytes: Answers already held by the connection.
///   - queueLimit: Its existing queued-bulk bound.
/// - Returns: Whether the answer fits inline and in the existing queue.
[[nodiscard]] bool hostAdministrationMutationHasCapacity(std::size_t answerBytes, std::size_t queuedBytes, std::size_t queueLimit);

/// Builds a pinned, correlated typed administration request.
/// - Parameters:
///   - request: The exact intent, with a nonzero daemon incarnation.
///   - callID: The connection-unique JSON-RPC identity.
/// - Returns: The request text, or empty for invalid input.
[[nodiscard]] std::string hostAdministrationRequest(const Sync::ManagementRequest& request, std::string_view callID);

/// Reads one typed operation after the shared method-admission gate.
/// - Parameters:
///   - method: The exact registry method.
///   - parameters: Its decoded parameters.
/// - Returns: The bounded intent, or no value for malformed input.
[[nodiscard]] std::optional<Sync::ManagementRequest> readHostAdministrationRequest(std::string_view method, const Codec::JSON::Value& parameters);

/// Builds a bounded response without putting a bearer secret in a snapshot or failed issue.
/// - Parameters:
///   - request: The admitted operation and exact selectors to echo.
///   - reply: The host-owned snapshot or outcome.
///   - callID: The decoded JSON-RPC identity to echo.
/// - Returns: The response object, or no value when it violates the family's bounds.
[[nodiscard]] std::optional<Codec::JSON::Value> hostAdministrationResponse(const Sync::ManagementRequest& request, const Sync::ManagementReply& reply, const Codec::JSON::Value* callID);

/// Reads only the answer to one exact administration operation.
/// - Parameters:
///   - response: The complete JSON-RPC response, bounded before parsing.
///   - callID: The outstanding call's exact identity.
///   - request: Its retained intent and daemon incarnation.
/// - Returns: No value for another call; a failed outcome for malformed matching data.
[[nodiscard]] std::optional<Sync::ManagementReply> readHostAdministrationResult(std::string_view response, std::string_view callID, const Sync::ManagementRequest& request);

} // namespace Ubiquitty::Protocol::Control
