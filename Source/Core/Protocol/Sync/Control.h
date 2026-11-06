// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/Control.h — serving the requests `Call` carries on channel 0
// (Docs/sync.md §6.5, §7.3).
//
// A `Call` is a JSON-RPC 2.0 request with an id, so every one draws exactly
// one `Result`: a call that cannot be read is answered with the error that
// says why, one the connection took past a per-attachment cap with `busy`, and
// the rest by the method the owner runs. The connection counts and frames;
// this reads the JSON, so the connection's own link set carries no parser.
//

#include "Core/Codec/JSON.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/Connection.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Protocol::Sync {

/// Explains why a serialized call cannot fit in a control frame.
///
/// - Parameter bytes: The serialized request's byte count.
/// - Returns: The refusal, or nothing when the request fits.
[[nodiscard]] inline std::optional<std::string> callSizeRefusal(std::size_t bytes)
{
    if (bytes <= maxFrameBodyBytes)
        return std::nullopt;
    return "A host call holds at most " + std::to_string(maxFrameBodyBytes) + " bytes, and this one holds " + std::to_string(bytes) + ".";
}

/// Decodes a `Call`'s text as a JSON-RPC 2.0 request.
///
/// A request with no id is answered as an invalid request, since a call
/// awaits its `Result` and a notification travels as `Notify`.
///
/// - Parameter text: The `Call`'s text.
/// - Returns: The request: `Dispatch` with an id to answer, or `Respond` with
///   the error response to send at once.
[[nodiscard]] Control::DecodedRequest decodeCallRequest(std::string_view text);

/// The response to a call over the outstanding-call or queued-bulk cap.
///
/// - Parameter id: The call's id, or null.
/// - Returns: A JSON-RPC 2.0 error response with `ErrorCode::Busy`.
[[nodiscard]] std::string busyResponse(const Codec::JSON::Value* id);

/// Runs a method for one admitted request.
///
/// - Parameter request: A request with an id and `Dispatch` as its outcome.
/// - Returns: The JSON-RPC 2.0 response object, a result or an error.
using CallServer = std::function<Codec::JSON::Value(const Control::DecodedRequest& request)>;

/// Runs or holds one admitted call. A held call remains counted against the
/// attachment's bound until its owner answers or refuses it.
///
/// - Parameters:
///   - callIndex: The connection's stable correlation number for the answer.
///   - request: The decoded request.
/// - Returns: An immediate response, or nothing while a decision is pending.
using DeferredCallServer = std::function<std::optional<Codec::JSON::Value>(std::uint32_t callIndex, const Control::DecodedRequest& request)>;

/// The most matches one `pane.search` answers; more set `truncated`.
inline constexpr std::size_t maxPaneSearchMatches = 256;

/// Runs `pane.search` over a pane's terminal (Docs/sync.md §6.6): the
/// pattern over the current epoch's history by logical line, whatever of it
/// the caller has fetched. The caller names the pane; this reads its params'
/// `pattern`, and `from`, `to` and `caseSensitive` when present.
///
/// - Parameters:
///   - terminal: The pane's terminal.
///   - request: The request.
/// - Returns: `{epoch, matches: [{line, startCell, endCell}], truncated}`, or
///   an invalid-parameters error.
[[nodiscard]] Codec::JSON::Value searchPane(const Ubiquitty::Terminal& terminal, const Control::DecodedRequest& request);

/// Has a connection answer the calls that arrive past its outstanding-call
/// cap as it reads them, keeping nothing of them: each is answered `busy`
/// under its own id, or, when it cannot be read, with the error that says
/// why. The calls `serveCalls` then takes are the admitted ones.
///
/// - Parameter connection: The host's end.
void answerBusyAsRead(HostConnection& connection);

/// Answers every call a connection has received: an unreadable one with the
/// error that says why, one over a cap with `busy`, and the rest with what
/// `serve` returns — or `busy`, when that answer would pass the queued-bulk
/// cap.
///
/// - Parameters:
///   - connection: The host's end; its calls are taken.
///   - serve: Runs the method of each admitted request.
void serveCalls(HostConnection& connection, const CallServer& serve);

/// Serves calls while allowing an admitted request to await an external
/// decision. Malformed and over-limit calls are refused immediately, as in
/// `serveCalls`; an absent response leaves the call outstanding.
///
/// - Parameters:
///   - connection: The host's end; its newly received calls are taken.
///   - serve: Runs or holds each admitted request.
void serveCallsDeferred(HostConnection& connection, const DeferredCallServer& serve);

} // namespace Ubiquitty::Protocol::Sync
