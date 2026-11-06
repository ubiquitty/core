// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Control.h"

#include "Core/CaseSensitivity.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Unicode/Normalize.h"

#include <optional>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

Control::DecodedRequest decodeCallRequest(std::string_view text)
{
    Control::DecodedRequest request = Control::decodeRequest(text, {});
    if (request.outcome == Control::RequestOutcome::Respond)
        return request;
    if (request.outcome == Control::RequestOutcome::Ignore || !request.hasID) {
        Control::DecodedRequest refused;
        refused.outcome = Control::RequestOutcome::Respond;
        refused.response = Control::rpcError(nullptr, Control::ErrorCode::InvalidRequest, "a Call carries an id; a notification travels as Notify");
        return refused;
    }
    return request;
}

std::string busyResponse(const Codec::JSON::Value* id)
{
    return Codec::JSON::serialize(Control::rpcError(id, Control::ErrorCode::Busy, "the attachment holds as many calls as the host allows; call again once one is answered"));
}

Codec::JSON::Value searchPane(const Ubiquitty::Terminal& terminal, const Control::DecodedRequest& request)
{
    using Codec::JSON::Value;
    const Value* pattern = request.params.find("pattern");
    if (!pattern || !pattern->isString() || pattern->asString().empty() || pattern->asString().size() >= static_cast<std::size_t>(Ubiquitty::Screen::scrollSearchMaxPattern))
        return Control::rpcError(request.idOrNull(), Control::ErrorCode::InvalidParameters, "pattern is a regular expression of 1 to 127 bytes");
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(terminal);
    const auto lineParameter = [&](const char* name, std::uint64_t fallback) -> std::optional<std::uint64_t> {
        const Value* value = request.params.find(name);
        if (!value)
            return fallback;
        if (!value->isNumber() || value->asNumber() < 0 || value->asNumber() > 9.0e15 || value->asNumber() != static_cast<double>(static_cast<std::uint64_t>(value->asNumber())))
            return std::nullopt;
        return static_cast<std::uint64_t>(value->asNumber());
    };
    const std::optional<std::uint64_t> from = lineParameter("from", address.oldest);
    const std::optional<std::uint64_t> to = lineParameter("to", address.head);
    if (!from || !to || *from > *to)
        return Control::rpcError(request.idOrNull(), Control::ErrorCode::InvalidParameters, "from and to are line numbers, from at most to");
    const Ubiquitty::CaseSensitivity caseSensitivity = request.params.boolOr("caseSensitive", false) ? Ubiquitty::CaseSensitivity::Sensitive : Ubiquitty::CaseSensitivity::Insensitive;
    // The pattern is matched as written: the form is the search's parameter,
    // never the process's setting, so every caller is answered alike.
    const std::optional<Ubiquitty::Screen::ScrollSearch::HistorySearchResult> found = Ubiquitty::Screen::ScrollSearch::searchHistory(
        terminal, pattern->asString(), { .from = *from, .to = *to }, caseSensitivity, Ubiquitty::Unicode::NormalizationForm::None, maxPaneSearchMatches);
    if (!found)
        return Control::rpcError(request.idOrNull(), Control::ErrorCode::InvalidParameters, "pattern does not compile");
    Value matches = Value::array();
    for (const Ubiquitty::Screen::ScrollSearch::HistoryMatch& match : found->matches) {
        Value entry = Value::object();
        entry.set("line", Value::number(static_cast<double>(match.line)));
        entry.set("startCell", Value::number(match.startCell));
        entry.set("endCell", Value::number(match.endCell));
        matches.push(std::move(entry));
    }
    Value result = Value::object();
    result.set("epoch", Value::number(found->epoch));
    result.set("matches", std::move(matches));
    result.set("truncated", Value::boolean(found->isTruncated));
    return Control::rpcResult(request.idOrNull(), std::move(result));
}

void answerBusyAsRead(HostConnection& connection)
{
    connection.setBusyAnswerer([](std::string_view call) {
        const Control::DecodedRequest request = decodeCallRequest(call);
        return request.outcome == Control::RequestOutcome::Respond ? Codec::JSON::serialize(request.response) : busyResponse(request.idOrNull());
    });
}

void serveCalls(HostConnection& connection, const CallServer& serve)
{
    serveCallsDeferred(connection, [&](std::uint32_t, const Control::DecodedRequest& request) {
        return std::optional { serve(request) };
    });
}

void serveCallsDeferred(HostConnection& connection, const DeferredCallServer& serve)
{
    for (ReceivedCall& call : connection.takeCalls()) {
        const Control::DecodedRequest request = decodeCallRequest(call.text);
        if (request.outcome == Control::RequestOutcome::Respond) {
            connection.refuseCall(call.callIndex, Codec::JSON::serialize(request.response), CallRefusal::Unreadable);
            continue;
        }
        if (call.admission == CallAdmission::OverLimit) {
            connection.refuseCall(call.callIndex, busyResponse(request.idOrNull()), CallRefusal::Busy);
            continue;
        }
        const std::optional<Codec::JSON::Value> response = serve(call.callIndex, request);
        if (response && connection.answerCall(call.callIndex, Codec::JSON::serialize(*response)) == CallAnswer::OverBulkLimit)
            connection.refuseCall(call.callIndex, busyResponse(request.idOrNull()), CallRefusal::Busy);
    }
    connection.releaseInputBehindCalls();
}

} // namespace Ubiquitty::Protocol::Sync
