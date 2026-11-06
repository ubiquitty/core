// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Cell.h"
#include "Core/Codec/JSON.h"
#include "Core/GridBounds.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Unicode/UTF8Repair.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace Ubiquitty::Protocol::Control {

/// The authoritative text range selected by a capture call.
enum class CaptureSource : std::uint8_t {
    Screen,
    LastCommand,
    Rows,
    Tail,
};

/// Bounded source parameters. Rows are inclusive physical rows in the host's
/// current retained buffer; Tail chooses its range in the same snapshot.
struct CaptureRequest {
    CaptureSource source = CaptureSource::Screen;
    std::int32_t start = 0;
    std::int32_t end = 0;
    std::uint32_t lines = 100;
};

/// The registry spelling of a capture source.
///
/// - Parameter source: The requested source.
/// - Returns: Its JSON enum spelling.
[[nodiscard]] inline std::string_view captureSourceName(CaptureSource source)
{
    switch (source) {
    case CaptureSource::Screen:
        return "screen";
    case CaptureSource::LastCommand:
        return "last-command";
    case CaptureSource::Rows:
        return "rows";
    case CaptureSource::Tail:
        return "tail";
    }
    return {};
}

/// The pane, source and optional visible geometry a request is bound to.
struct CaptureTarget {
    std::uint32_t pane = 0;
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
    CaptureRequest request {};
};

/// Whether an answer supplies the complete range for the exact request.
enum class CaptureReplyStatus : std::uint8_t {
    Unrelated,
    Complete,
    Refused,
    Incomplete,
    Pending,
    Invalid,
};

/// A validated capture answer. Only Complete carries text. Screen text has no
/// final row separator; other sources retain the extractor's final newline.
struct CaptureReply {
    CaptureReplyStatus status = CaptureReplyStatus::Invalid;
    std::string text;
    std::string reason = "the host returned an invalid capture";
    std::int32_t errorCode = 0;
    Codec::JSON::Value screen {};
};

namespace CaptureReplyDetail {

/// Whether a JSON object repeats any member name.
[[nodiscard]] inline bool hasRepeatedMembers(const Codec::JSON::Value& value)
{
    const auto& members = value.members();
    for (std::size_t index = 0; index < members.size(); ++index) {
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (members[index].first == members[earlier].first)
                return true;
        }
    }
    return false;
}

/// Whether a JSON number is the exact expected integer.
[[nodiscard]] inline bool isNumber(const Codec::JSON::Value* value, double expected)
{
    return value && value->isNumber() && value->asNumber() == expected;
}

/// Whether a number is an integer inside inclusive bounds.
///
/// - Parameters:
///   - value: The optional JSON number.
///   - minimum: The smallest accepted integer.
///   - maximum: The largest accepted integer.
/// - Returns: Whether the value is finite, integral and within both bounds.
[[nodiscard]] inline bool isInteger(const Codec::JSON::Value* value, double minimum, double maximum)
{
    return value && value->isNumber() && std::isfinite(value->asNumber()) && std::trunc(value->asNumber()) == value->asNumber()
        && value->asNumber() >= minimum && value->asNumber() <= maximum;
}

} // namespace CaptureReplyDetail

/// Validates one correlated capture. Only screen replies lose their final row
/// terminator. Cell text may contain LF; no separator count proves completeness.
///
/// - Parameters:
///   - response: The parsed JSON-RPC response.
///   - requestID: The unique, unretired string id of this request.
///   - target: Its connection-local pane handle, source and snapshot geometry.
/// - Returns: Complete text, an explicit refusal/incomplete answer, an unrelated
///   id, or Invalid. Error and incomplete bodies never expose partial text.
[[nodiscard]] inline CaptureReply decodeCaptureReply(const Codec::JSON::Value& response, std::string_view requestID, const CaptureTarget& target)
{
    using namespace CaptureReplyDetail;
    const bool isScreen = target.request.source == CaptureSource::Screen;
    if (!response.isObject() || response.members().size() != 3 || hasRepeatedMembers(response)
        || requestID.empty() || !target.pane || (isScreen && (!target.rows || target.rows > maxRows || !target.columns || target.columns > maxColumns)))
        return {};
    const auto* id = response.find("id");
    if (!id || !id->isString())
        return {};
    if (id->asString() != requestID)
        return { .status = CaptureReplyStatus::Unrelated, .text = {}, .reason = {} };
    const auto* version = response.find("jsonrpc");
    if (!version || !version->isString() || version->asString() != "2.0")
        return {};
    const auto* result = response.find("result");
    const auto* error = response.find("error");
    if (error) {
        if (result || !error->isObject() || error->members().size() < 2 || error->members().size() > 3 || hasRepeatedMembers(*error))
            return {};
        const auto* code = error->find("code");
        const auto* message = error->find("message");
        const auto* data = error->find("data");
        if (!code || !code->isNumber() || !std::isfinite(code->asNumber()) || std::trunc(code->asNumber()) != code->asNumber()
            || code->asNumber() < std::numeric_limits<std::int32_t>::min() || code->asNumber() > std::numeric_limits<std::int32_t>::max()
            || !message || !message->isString() || (data && !data->isObject()) || (error->members().size() == 3 && !data))
            return {};
        return { .status = CaptureReplyStatus::Refused, .text = {}, .reason = message->asString(), .errorCode = static_cast<std::int32_t>(code->asNumber()) };
    }
    const auto* richText = result ? result->find("richText") : nullptr;
    std::uint64_t richTextBytes = 0;
    if (!result || !result->isObject() || result->members().size() != (isScreen ? (richText ? 6u : 5u) : 8u) || hasRepeatedMembers(*result)
        || !isNumber(result->find("pane"), target.pane))
        return {};
    if (richText) {
        bool isParsed = false;
        const auto schema = Codec::JSON::parse(std::string(findSyncMethod("pane.capture")->resultSchema), isParsed);
        if (!isScreen || !isParsed || validate(*result, schema))
            return {};
        if (richText->find("layoutPending")->asBool())
            return { .status = CaptureReplyStatus::Pending, .text = {}, .reason = "the frontend is measuring rich text" };
        for (const auto& block : richText->find("blocks")->items())
            richTextBytes += block.find("text")->asString().size();
    }
    std::uint64_t returnedRows = target.rows;
    std::uint64_t columns = target.columns;
    if (isScreen) {
        if (!isNumber(result->find("rows"), target.rows) || !isNumber(result->find("columns"), target.columns))
            return {};
    } else {
        const auto* source = result->find("source");
        const auto* first = result->find("first");
        const auto* last = result->find("last");
        const auto* total = result->find("totalRows");
        const auto* width = result->find("columns");
        if (!source || !source->isString() || source->asString() != captureSourceName(target.request.source)
            || !isInteger(total, 1, std::numeric_limits<std::int32_t>::max())
            || !isInteger(first, 0, total->asNumber() - 1) || !isInteger(last, first->asNumber(), total->asNumber() - 1)
            || !isInteger(width, 1, maxColumns))
            return {};
        if (target.request.source == CaptureSource::Rows
            && (target.request.start > target.request.end || target.request.end < 0 || target.request.start >= total->asNumber()
                || first->asNumber() != std::max<std::int32_t>(target.request.start, 0)
                || last->asNumber() > std::min<double>(target.request.end, total->asNumber() - 1)))
            return {};
        if (target.request.source == CaptureSource::Tail
            && (!target.request.lines || target.request.lines > 100000
                || first->asNumber() != std::max<double>(0, total->asNumber() - target.request.lines)))
            return {};
        returnedRows = static_cast<std::uint64_t>(last->asNumber() - first->asNumber()) + 1;
        columns = static_cast<std::uint64_t>(width->asNumber());
    }
    const auto* complete = result->find("complete");
    const auto* text = result->find("text");
    if (!complete || !complete->isBool() || !text || !text->isString())
        return {};
    if (!complete->asBool())
        return { .status = CaptureReplyStatus::Incomplete, .text = {}, .reason = "the host's capture exceeded its text limit" };
    if (!isScreen) {
        const double last = result->find("last")->asNumber();
        const double total = result->find("totalRows")->asNumber();
        if ((target.request.source == CaptureSource::Rows && last != std::min<double>(target.request.end, total - 1))
            || (target.request.source == CaptureSource::Tail && last != total - 1))
            return {};
    }
    const std::string& bytes = text->asString();
    const std::uint64_t maxBytes = returnedRows * (columns * 4 * (1 + cellMaxCombining) + 1) + richTextBytes;
    const std::uint64_t maxWholeRowCaptureBytes = (1u << 20) + maxColumns * 4 * (1 + cellMaxCombining) + 1 + richTextBytes;
    if (bytes.empty() || bytes.back() != '\n' || bytes.size() > maxBytes || bytes.size() > maxWholeRowCaptureBytes || Unicode::wellFormedUTF8PrefixLength(bytes) != bytes.size())
        return {};
    return { .status = CaptureReplyStatus::Complete, .text = isScreen ? bytes.substr(0, bytes.size() - 1) : bytes, .reason = {}, .screen = isScreen ? *result : Codec::JSON::Value {} };
}

} // namespace Ubiquitty::Protocol::Control
