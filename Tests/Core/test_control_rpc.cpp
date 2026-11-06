// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_control_rpc — the control socket's JSON-RPC 2.0 envelope and auth gate.

#include "Harness.h"

#include "Core/Protocol/ControlRPC.h"

#include <string>

namespace {

using Ubiquitty::Codec::JSON::Value;
using Ubiquitty::Protocol::Control::constantTimeEquals;
using Ubiquitty::Protocol::Control::DecodedRequest;
using Ubiquitty::Protocol::Control::decodeRequest;
using Ubiquitty::Protocol::Control::ErrorCode;
using Ubiquitty::Protocol::Control::isBlankRequestLine;
using Ubiquitty::Protocol::Control::RequestOutcome;
using Ubiquitty::Protocol::Control::rpcError;
using Ubiquitty::Protocol::Control::rpcResult;

// The `error.code` of a response, or 0 when it carries none.
int errorCodeOf(const Value& response)
{
    const Value* error = response.find("error");
    if (!error || !error->isObject())
        return 0;
    const Value* code = error->find("code");
    return (code && code->isNumber()) ? static_cast<int>(code->asNumber()) : 0;
}

bool hasNullID(const Value& response)
{
    const Value* id = response.find("id");
    return id && id->isNull();
}

// ---- blank lines --------------------------------------------------------

TEST(testBlankLines)
{
    CHECK_MSG(isBlankRequestLine(""), "empty is blank");
    CHECK_MSG(isBlankRequestLine("  \t\r"), "spaces, tabs and CRs are blank");
    CHECK_MSG(!isBlankRequestLine("{}"), "an object is not blank");
    CHECK_MSG(!isBlankRequestLine("\n"), "a newline is not blank");

    CHECK_MSG(decodeRequest("   ", "").outcome == RequestOutcome::Ignore,
        "a blank line is ignored, not an error");
}

// ---- envelopes ----------------------------------------------------------

TEST(testResultEnvelope)
{
    const Value id = Value::number(7);
    const Value response = rpcResult(&id, Value::string("ok"));

    const Value* version = response.find("jsonrpc");
    CHECK_MSG(version && version->isString() && version->asString() == "2.0", "jsonrpc is \"2.0\"");
    const Value* echoed = response.find("id");
    CHECK_MSG(echoed && echoed->isNumber() && echoed->asNumber() == 7, "the id is echoed");
    CHECK_MSG(response.find("result") != nullptr, "a result member is present");
    CHECK_MSG(response.find("error") == nullptr, "and no error member");
}

TEST(testErrorEnvelope)
{
    const Value response = rpcError(nullptr, ErrorCode::MethodNotFound, "method not found: nope");
    CHECK_MSG(hasNullID(response), "an absent id serializes as null");
    CHECK_MSG(errorCodeOf(response) == -32601, "the code is carried through");
    CHECK_MSG(response.find("result") == nullptr, "an error carries no result");

    const Value* error = response.find("error");
    const Value* message = error ? error->find("message") : nullptr;
    CHECK_MSG(message && message->isString() && message->asString() == "method not found: nope",
        "the message is carried through");
}

// The four standard codes must keep their spec values; Unauthorized must stay
// inside the -32099..-32000 implementation-defined band.
TEST(testErrorCodeValues)
{
    CHECK_MSG(static_cast<int>(ErrorCode::ParseError) == -32700, "ParseError is -32700");
    CHECK_MSG(static_cast<int>(ErrorCode::InvalidRequest) == -32600, "InvalidRequest is -32600");
    CHECK_MSG(static_cast<int>(ErrorCode::MethodNotFound) == -32601, "MethodNotFound is -32601");
    CHECK_MSG(static_cast<int>(ErrorCode::InvalidParameters) == -32602, "InvalidParameters is -32602");

    const int unauthorized = static_cast<int>(ErrorCode::Unauthorized);
    CHECK_MSG(unauthorized >= -32099 && unauthorized <= -32000,
        "Unauthorized is inside the server-error band");
    const int forbidden = static_cast<int>(ErrorCode::Forbidden);
    CHECK_MSG(forbidden >= -32099 && forbidden <= -32000,
        "Forbidden is inside the server-error band");
    CHECK_MSG(forbidden != unauthorized, "a policy refusal is not an auth failure");
}

// ---- decoding -----------------------------------------------------------

TEST(testWellFormedRequestDispatches)
{
    const DecodedRequest request = decodeRequest(R"({"jsonrpc":"2.0","id":1,"method":"ls"})", "");
    CHECK_MSG(request.outcome == RequestOutcome::Dispatch, "a well-formed request dispatches");
    CHECK_MSG(request.method == "ls", "the method is decoded");
    CHECK_MSG(request.hasID, "the id is present");
    CHECK_MSG(request.idOrNull() != nullptr, "and reachable as a pointer");
}

TEST(testMalformedJSONIsAParseError)
{
    for (const char* line : { "{", "not json", "[1,2,3", "\"bare string" }) {
        const DecodedRequest request = decodeRequest(line, "");
        CHECK_MSG(request.outcome == RequestOutcome::Respond, "malformed JSON produces a response");
        CHECK_MSG(errorCodeOf(request.response) == -32700, "and it is a parse error");
        CHECK_MSG(hasNullID(request.response), "with a null id, since none is recoverable");
    }
}

// Valid JSON that is not an object cannot carry a method, so it is a parse
// error rather than an invalid request.
TEST(testNonObjectJSONIsAParseError)
{
    for (const char* line : { "[1,2,3]", "42", "\"a string\"", "null", "true" }) {
        const DecodedRequest request = decodeRequest(line, "");
        CHECK_MSG(request.outcome == RequestOutcome::Respond, "a non-object produces a response");
        CHECK_MSG(errorCodeOf(request.response) == -32700, "and it is a parse error");
    }
}

TEST(testMissingMethodIsAnInvalidRequest)
{
    for (const char* line : { R"({"id":1})", R"({"id":1,"method":42})", R"({"id":1,"method":null})" }) {
        const DecodedRequest request = decodeRequest(line, "");
        CHECK_MSG(request.outcome == RequestOutcome::Respond, "an absent or non-string method responds");
        CHECK_MSG(errorCodeOf(request.response) == -32600, "and it is an invalid request");
    }
}

// A malformed notification has no id to correlate a response against, so
// answering it would desynchronise a client counting replies.
TEST(testMalformedNotificationIsSilent)
{
    CHECK_MSG(decodeRequest(R"({"jsonrpc":"2.0"})", "").outcome == RequestOutcome::Ignore,
        "a notification with no method is ignored");
    CHECK_MSG(decodeRequest(R"({"method":99})", "").outcome == RequestOutcome::Ignore,
        "a notification with a non-string method is ignored");
}

// A well-formed notification still runs; only its response is the caller's
// business to suppress.
TEST(testWellFormedNotificationDispatches)
{
    const DecodedRequest request = decodeRequest(R"({"jsonrpc":"2.0","method":"ls"})", "");
    CHECK_MSG(request.outcome == RequestOutcome::Dispatch, "a notification with a method dispatches");
    CHECK_MSG(!request.hasID, "and is marked as having no id");
    CHECK_MSG(request.idOrNull() == nullptr, "so no id is echoed");
}

// A handler reads its arguments without first checking the shape, so an absent
// params member is an empty object; a present one of another shape is refused
// with the parameters error rather than read as empty, which would run the
// method with none of what the caller sent.
TEST(testParamsDecodeToAnObjectOrAreRefused)
{
    const DecodedRequest absent = decodeRequest(R"({"id":1,"method":"m"})", "");
    CHECK_MSG(absent.outcome == RequestOutcome::Dispatch, "a request without params dispatches");
    CHECK_MSG(absent.params.isObject() && absent.params.members().empty(), "with empty params");

    for (const char* line : {
             R"({"id":1,"method":"m","params":null})",
             R"({"id":1,"method":"m","params":[1,2]})",
             R"({"id":1,"method":"m","params":"text"})",
         }) {
        const DecodedRequest request = decodeRequest(line, "");
        CHECK_MSG(request.outcome == RequestOutcome::Respond, line);
        const Value* error = request.response.find("error");
        CHECK_MSG(error && error->numberOr("code") == -32602, "answered with the parameters error");
        const Value* data = error ? error->find("data") : nullptr;
        CHECK_MSG(data && data->stringOr("kind") == "bad_request", "under the bad_request kind");
    }
    CHECK_MSG(decodeRequest(R"({"method":"m","params":[1]})", "").outcome == RequestOutcome::Ignore,
        "a malformed notification is still not answered");

    const DecodedRequest request = decodeRequest(R"({"id":1,"method":"m","params":{"pane":3}})", "");
    CHECK_MSG(request.params.numberOr("pane", 0) == 3, "a real params object is carried through");
}

// Every error carries the same shape: a code, a sentence, and a kind a
// program can branch on without parsing the sentence.
TEST(testEveryErrorCarriesItsKind)
{
    const Value response = rpcError(nullptr, ErrorCode::Forbidden, "no", [] {
        Value details = Value::object();
        details.set("parameter", Value::string("/text"));
        details.set("kind", Value::string("spoofed"));
        return details;
    }());
    const Value* error = response.find("error");
    const Value* data = error ? error->find("data") : nullptr;
    CHECK(data && data->stringOr("kind") == "forbidden");
    CHECK(data && data->stringOr("parameter") == "/text");
}

// A JSON-null id is a present id, not an absent one.
TEST(testExplicitNullIDIsPresent)
{
    const DecodedRequest request = decodeRequest(R"({"id":null,"method":"ls"})", "");
    CHECK_MSG(request.hasID, "an explicit null id counts as present");
    CHECK_MSG(request.outcome == RequestOutcome::Dispatch, "and the request dispatches");
}

// ---- auth gate ----------------------------------------------------------

TEST(testNoTokenConfiguredSkipsTheGate)
{
    const DecodedRequest request = decodeRequest(R"({"id":1,"method":"ls"})", "");
    CHECK_MSG(request.outcome == RequestOutcome::Dispatch, "no configured token means no gate");
}

TEST(testConfiguredTokenIsRequired)
{
    const DecodedRequest request = decodeRequest(R"({"id":1,"method":"ls"})", "secret");
    CHECK_MSG(request.outcome == RequestOutcome::Respond, "a missing token is refused");
    CHECK_MSG(errorCodeOf(request.response) == -32001, "with Unauthorized");
}

TEST(testWrongTokenIsRefused)
{
    for (const char* line : {
             R"({"id":1,"method":"ls","params":{"token":"wrong"}})",
             R"({"id":1,"method":"ls","token":"wrong"})",
             R"({"id":1,"method":"ls","params":{"token":""}})",
             R"({"id":1,"method":"ls","params":{"token":"secre"}})",
             R"({"id":1,"method":"ls","params":{"token":"secrett"}})",
         }) {
        const DecodedRequest request = decodeRequest(line, "secret");
        CHECK_MSG(request.outcome == RequestOutcome::Respond, "a wrong token is refused");
        CHECK_MSG(errorCodeOf(request.response) == -32001, "with Unauthorized");
    }
}

// Both spellings were accepted before the decode moved; clients depend on each.
TEST(testTokenAcceptedInParamsAndAtTopLevel)
{
    CHECK_MSG(decodeRequest(R"({"id":1,"method":"ls","params":{"token":"secret"}})", "secret").outcome
            == RequestOutcome::Dispatch,
        "a token in params authorises");
    CHECK_MSG(decodeRequest(R"({"id":1,"method":"ls","token":"secret"})", "secret").outcome
            == RequestOutcome::Dispatch,
        "a token at the top level authorises");
}

// params wins, so a stale top-level token cannot override an explicit one.
TEST(testParamsTokenTakesPrecedence)
{
    CHECK_MSG(decodeRequest(R"({"id":1,"method":"ls","params":{"token":"secret"},"token":"wrong"})", "secret").outcome
            == RequestOutcome::Dispatch,
        "a correct params token wins over a wrong top-level one");
    CHECK_MSG(decodeRequest(R"({"id":1,"method":"ls","params":{"token":"wrong"},"token":"secret"})", "secret").outcome
            == RequestOutcome::Respond,
        "a wrong params token is not rescued by a correct top-level one");
}

// An unauthorised caller must not be able to tell which methods exist.
TEST(testUnauthorisedCannotEnumerateMethods)
{
    const DecodedRequest known = decodeRequest(R"({"id":1,"method":"ls"})", "secret");
    const DecodedRequest unknown = decodeRequest(R"({"id":1,"method":"no-such-method"})", "secret");

    CHECK_MSG(errorCodeOf(known.response) == errorCodeOf(unknown.response),
        "a known and an unknown method are indistinguishable without a token");
    CHECK_MSG(known.outcome == RequestOutcome::Respond && unknown.outcome == RequestOutcome::Respond,
        "neither reaches dispatch");
}

// A notification is still gated: no id does not mean no auth.
TEST(testNotificationIsAlsoGated)
{
    const DecodedRequest request = decodeRequest(R"({"method":"ls"})", "secret");
    CHECK_MSG(request.outcome == RequestOutcome::Respond, "an unauthorised notification is refused");
    CHECK_MSG(errorCodeOf(request.response) == -32001, "with Unauthorized");
}

// ---- constant-time compare ----------------------------------------------

TEST(testConstantTimeEqualsMatchesEquality)
{
    CHECK_MSG(constantTimeEquals("", ""), "empty equals empty");
    CHECK_MSG(constantTimeEquals("abc", "abc"), "equal strings compare equal");
    CHECK_MSG(!constantTimeEquals("abc", "abd"), "a differing last byte is unequal");
    CHECK_MSG(!constantTimeEquals("abc", "bbc"), "a differing first byte is unequal");
    CHECK_MSG(!constantTimeEquals("abc", "ab"), "a prefix is unequal");
    CHECK_MSG(!constantTimeEquals("ab", "abc"), "a shorter string is unequal");
    CHECK_MSG(!constantTimeEquals("", "a"), "empty is unequal to non-empty");

    // Embedded NULs are compared, not treated as terminators.
    CHECK_MSG(constantTimeEquals(std::string_view("a\0b", 3), std::string_view("a\0b", 3)),
        "embedded NULs compare equal");
    CHECK_MSG(!constantTimeEquals(std::string_view("a\0b", 3), std::string_view("a\0c", 3)),
        "and are not truncated at the NUL");
}

} // namespace

UBIQUITTY_TEST_MAIN()
