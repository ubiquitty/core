// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/EventLog.h"
#include "Core/Unicode/UTF8Repair.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Protocol::Control {

/// Starts one connection's event stream after an exact cursor, or from now.
struct EventSubscription {
    std::uint64_t after = 0;
    bool isFromLatest = false;
    friend bool operator==(const EventSubscription&, const EventSubscription&) = default;
};

/// Bounds one notification independently of the retained host log.
inline constexpr std::size_t maxEventsPerNotify = 16;

namespace EventStreamDetail {

/// Reads one exact canonical decimal cursor without floating-point rounding.
/// - Parameter value: The optional incoming JSON string.
/// - Returns: Its complete uint64 value, or nothing for another representation.
inline std::optional<std::uint64_t> cursor(const Codec::JSON::Value* value)
{
    if (!value || !value->isString())
        return std::nullopt;
    const std::string& text = value->asString();
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0'))
        return std::nullopt;
    std::uint64_t number = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (error != std::errc {} || end != text.data() + text.size())
        return std::nullopt;
    return number;
}

/// Repairs event text while retaining the log's aggregate field bound.
/// - Parameter source: Borrowed bytes from the bounded event record.
/// - Returns: Owned well-formed UTF-8 within maxEventTextLength bytes.
inline std::string text(std::string_view source)
{
    // The record carries UTF-8 only. Repair the log's bytes, then retain its
    // byte bound even when replacement characters expanded them.
    std::string result(source.substr(0, Session::maxEventTextLength));
    (void)Unicode::repairUTF8(result);
    if (result.size() > Session::maxEventTextLength)
        result.resize(Unicode::wellFormedUTF8PrefixLength(std::string_view(result).substr(0, Session::maxEventTextLength)));
    return result;
}

} // namespace EventStreamDetail

/// Reads exact event subscription parameters, never a floating-point cursor.
/// - Parameter parameters: The call's params object.
/// - Returns: The subscription, or nothing for ambiguous or malformed fields.
[[nodiscard]] inline std::optional<EventSubscription> eventSubscription(const Codec::JSON::Value& parameters)
{
    const auto after = EventStreamDetail::cursor(parameters.find("after"));
    const auto* latest = parameters.find("fromLatest");
    if (!parameters.isObject() || parameters.members().size() != 2 || !after || !latest || !latest->isBool()
        || (latest->asBool() && *after))
        return std::nullopt;
    return EventSubscription { *after, latest->asBool() };
}

/// Builds a correlated subscription call without rounding its saved cursor.
/// - Parameters:
///   - subscription: Where the caller wishes to begin.
///   - requestID: A nonempty connection-unique correlation string.
/// - Returns: The complete JSON-RPC request, or empty for contradictory input.
[[nodiscard]] inline std::string eventSubscriptionRequest(EventSubscription subscription, std::string_view requestID)
{
    if (requestID.empty() || requestID.size() > Sync::maxFrameBodyBytes || (subscription.isFromLatest && subscription.after))
        return {};
    using Codec::JSON::Value;
    auto parameters = Value::object();
    parameters.set("after", Value::string(std::to_string(subscription.after)));
    parameters.set("fromLatest", Value::boolean(subscription.isFromLatest));
    auto request = Value::object();
    request.set("jsonrpc", Value::string("2.0"));
    request.set("id", Value::string(std::string(requestID)));
    request.set("method", Value::string("events.subscribe"));
    request.set("params", std::move(parameters));
    auto encoded = Codec::JSON::serialize(request);
    return encoded.size() <= Sync::maxFrameBodyBytes ? std::move(encoded) : std::string {};
}

/// Reads the accepted stream position from a correlated call's result object.
/// - Parameter result: The result already matched to the caller's request ID.
/// - Returns: Its exact cursor, or nothing for a malformed result.
[[nodiscard]] inline std::optional<std::uint64_t> eventSubscriptionCursor(const Codec::JSON::Value& result)
{
    return result.isObject() && result.members().size() == 1 ? EventStreamDetail::cursor(result.find("cursor")) : std::nullopt;
}

/// Encodes an admitted ordered batch as an `Events` `Notify` body; callers
/// apply disclosure before this call.
///
///     records:[Record] (1 to maxEventsPerNotify)
///     Record := cursor:u64 kind:u8 session:u32 pane:u32 exitStatus:svarint
///               state:u8 atPrompt:u8 count:u64 text:utf8 detail:utf8
///
/// Cursors strictly increase; only a gap record may carry cursor 0. The batch's
/// cursor is its last record's.
///
/// - Parameter events: At most maxEventsPerNotify records, including explicit gaps.
/// - Returns: The whole body, or nothing for invalid metadata or size.
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>> eventsNotify(std::span<const Session::Event> events)
{
    if (events.empty() || events.size() > maxEventsPerNotify)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    Sync::WireWriter writer(body);
    writer.varint(std::to_underlying(Sync::NotifyKind::Events));
    writer.varint(events.size());
    std::optional<std::uint64_t> previous;
    for (const auto& event : events) {
        if (!Session::eventKindFromRaw(std::to_underlying(event.kind))
            || std::to_underlying(event.state) > std::to_underlying(Session::EventProgramState::Error)
            || (!event.cursor && event.kind != Session::EventKind::Gap) || (previous && event.cursor <= *previous))
            return std::nullopt;
        previous = event.cursor;
        writer.u64(event.cursor);
        writer.u8(std::to_underlying(event.kind));
        writer.u32(event.sessionID);
        writer.u32(event.paneID);
        writer.svarint(event.exitStatus);
        writer.u8(std::to_underlying(event.state));
        writer.u8(event.isAtPrompt ? 1 : 0);
        writer.u64(event.count);
        writer.utf8(EventStreamDetail::text(event.text));
        writer.utf8(EventStreamDetail::text(event.detail));
    }
    if (body.size() > Sync::maxFrameBodyBytes)
        return std::nullopt;
    return body;
}

/// Reads an `Events` record whole before exposing any event to a consumer.
/// - Parameter record: An `Events` `Notify`'s record.
/// - Returns: Exact owned records, or nothing for a malformed batch.
[[nodiscard]] inline std::optional<std::vector<Session::Event>> decodeEvents(std::span<const std::uint8_t> record)
{
    Sync::WireReader reader(record);
    const std::size_t count = reader.count(maxEventsPerNotify);
    if (!reader.ok() || !count)
        return std::nullopt;
    std::vector<Session::Event> events;
    events.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        Session::Event event;
        event.cursor = reader.u64();
        const std::uint8_t kind = reader.u8();
        event.sessionID = reader.u32();
        event.paneID = reader.u32();
        const std::int64_t exitStatus = reader.svarint();
        const std::uint8_t state = reader.u8();
        const std::uint8_t atPrompt = reader.u8();
        event.count = reader.u64();
        const std::string_view text = reader.utf8(Session::maxEventTextLength);
        const std::string_view detail = reader.utf8(Session::maxEventTextLength);
        if (!reader.ok() || kind > Session::lastEventKind || state > std::to_underlying(Session::EventProgramState::Error)
            || atPrompt > 1 || exitStatus < std::numeric_limits<std::int32_t>::min() || exitStatus > std::numeric_limits<std::int32_t>::max())
            return std::nullopt;
        event.kind = static_cast<Session::EventKind>(kind);
        event.state = static_cast<Session::EventProgramState>(state);
        event.exitStatus = static_cast<std::int32_t>(exitStatus);
        event.isAtPrompt = atPrompt == 1;
        event.text = text;
        event.detail = detail;
        if ((!event.cursor && event.kind != Session::EventKind::Gap) || (!events.empty() && event.cursor <= events.back().cursor))
            return std::nullopt;
        events.push_back(std::move(event));
    }
    if (!reader.isAtEnd())
        return std::nullopt;
    return events;
}

} // namespace Ubiquitty::Protocol::Control
