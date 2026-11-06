// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// JSON-RPC 2.0 request envelope for the control socket: one request object per
// line, one response object per line.
//
// Decoding, validation and the auth gate are here; method dispatch is the
// frontend's. Transport is the caller's.

#include "Core/Codec/JSON.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Protocol::Control {

using Ubiquitty::Codec::JSON::Value;

/// Captures grid text and rich-text blocks from one terminal snapshot.
///
/// - Parameters:
///   - terminal: The locked authoritative terminal.
///   - pane: The caller's bound pane handle.
/// - Returns: A screen capture result with Unicode ranges and native geometry.
[[nodiscard]] Value captureScreen(const Ubiquitty::Terminal& terminal, std::uint32_t pane);

// -32700..-32600 are the JSON-RPC 2.0 standard codes. The rest sit in the
// -32099..-32000 implementation-defined server-error band: Unauthorized is a
// caller the token gate does not know, Forbidden a caller it knows asking for
// what the agent policy refuses, and NotFound and Ambiguous a well-formed
// request whose target resolves to nothing or to more than one object.
enum class ErrorCode : int {
    ParseError = -32700, // not valid JSON, or not a JSON object
    InvalidRequest = -32600, // valid JSON, not a well-formed request
    MethodNotFound = -32601,
    InvalidParameters = -32602,
    InternalError = -32603, // the frontend gave no usable answer
    Unauthorized = -32001,
    Forbidden = -32002,
    NotFound = -32003, // a parameter names an object that does not exist
    Ambiguous = -32004, // a parameter names more than one object
    Busy = -32005, // the caller holds as many calls as its attachment allows
};

// `id` null yields a null id member.
[[nodiscard]] Value rpcResult(const Value* id, Value result);

/// Returns the machine-readable word an error code is reported under.
///
/// - Parameter code: The JSON-RPC error code.
/// - Returns: For example `bad_request`, `method_not_found` or `forbidden`.
[[nodiscard]] std::string_view errorKind(ErrorCode code);

/// Builds an error response in the one shape every control error takes:
/// `{"code": <int>, "message": <sentence>, "data": {"kind": <word>, ...}}`.
///
/// - Parameters:
///   - id: The request's id, or null.
///   - code: The JSON-RPC error code; `data.kind` is derived from it.
///   - message: A sentence for the person reading it.
///   - details: Further members of `data`, such as the offending `parameter`.
/// - Returns: The response object.
[[nodiscard]] Value rpcError(const Value* id, ErrorCode code, std::string_view message, const Value& details = Value::object());

enum class RequestOutcome : uint8_t {
    Ignore, // send nothing
    Respond, // send `response` verbatim
    Dispatch, // run `method` with `params`
};

// `params` is always an object: an absent params member decodes to an empty
// one, and a present one that is not an object is answered with
// InvalidParameters rather than read as empty. `id` is held by value: the source line does not outlive it.
struct DecodedRequest {
    RequestOutcome outcome = RequestOutcome::Ignore;
    Value response;
    std::string method;
    Value params = Value::object();
    Value id;
    bool hasID = false;

    // Absent and JSON-null stay distinguishable.
    [[nodiscard]] const Value* idOrNull() const { return hasID ? &id : nullptr; }
};

// Empty `token` disables the auth gate. Otherwise the request must carry a
// matching `token`, in `params` or at the message's top level; the comparison
// is constant-time, and it runs before the method is reported back so that
// -32601 cannot enumerate methods for an unauthorised caller.
//
// A malformed notification yields Ignore: with no id, a response cannot be
// correlated. A well-formed notification still dispatches.
[[nodiscard]] DecodedRequest decodeRequest(std::string_view line, std::string_view token);

// No JSON at all: empty, or only spaces, tabs and CRs.
[[nodiscard]] bool isBlankRequestLine(std::string_view line);

// Never short-circuits on content. Length is treated as public.
[[nodiscard]] bool constantTimeEquals(std::string_view a, std::string_view b);

} // namespace Ubiquitty::Protocol::Control
