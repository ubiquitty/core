// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// ControlRPC.cpp — see Core/Protocol/ControlRPC.h.
//

#include "Core/Protocol/ControlRPC.h"
#include "Core/Accessibility/TextProjection.h"
#include "Core/Terminal.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <algorithm>
#include <string>
#include <utility>

namespace Ubiquitty::Protocol::Control {

namespace {

/// The text bound every capture source shares (Docs/sync.md): rows are
/// returned whole, and the host stops after the row that reaches it.
constexpr std::size_t maxCaptureTextBytes = 1u << 20;

/// The projection's leading whole rows, each ending in LF, up to the bound.
/// Row ends come from the cell boundaries, never from the LFs in the text,
/// because a cell's own scalar can be LF.
/// - Parameter projection: One snapshot of the visible grid.
/// - Returns: The text and whether every row is in it.
std::pair<std::string, bool> boundedScreenText(const Accessibility::TextProjection& projection)
{
    const std::size_t rowCount = projection.lineRanges.size();
    const std::size_t stride = static_cast<std::size_t>(projection.columns) + 1;
    if (!rowCount || projection.cellBoundaries.size() != rowCount * stride)
        return { projection.text + '\n', true };
    for (std::size_t row = 0; row < rowCount; ++row) {
        // The boundary after the row's last cell, clamped to its trimmed end.
        const std::size_t end = projection.cellBoundaries[row * stride + stride - 1].byteOffset;
        if (end + 1 < maxCaptureTextBytes)
            continue;
        std::string text = projection.text.substr(0, end);
        text.push_back('\n');
        return { std::move(text), row + 1 == rowCount };
    }
    return { projection.text + '\n', true };
}

} // namespace

Value captureScreen(const Terminal& terminal, std::uint32_t pane)
{
    const auto projection = Accessibility::projectVisibleText(terminal, 0);
    Value result = Value::object();
    result.set("pane", Value::number(pane));
    result.set("rows", Value::number(terminal.rows));
    result.set("columns", Value::number(terminal.columns));
    const bool isPending = std::ranges::any_of(projection.richText, [](const auto& block) { return block.isLayoutPending; });
    auto [text, hasEveryRow] = boundedScreenText(projection);
    result.set("complete", Value::boolean(!isPending && hasEveryRow));
    result.set("text", Value::string(std::move(text)));
    Value richText = Value::object();
    richText.set("offsetUnit", Value::string("unicode-scalar"));
    richText.set("coordinateUnit", Value::string("point"));
    richText.set("layoutPending", Value::boolean(isPending));
    richText.set("cellWidth", Value::number(terminal.typography.facts ? terminal.typography.facts->cellWidth / 64.0 : 0));
    richText.set("cellHeight", Value::number(terminal.typography.facts ? terminal.typography.facts->cellHeight / 64.0 : 0));
    Value blocks = Value::array();
    for (const auto& capture : projection.richText) {
        const auto& block = capture.block;
        Value entry = Value::object();
        entry.set("id", Value::number(block.id));
        entry.set("documentRevision", Value::number(block.documentRevision));
        entry.set("serial", Value::number(block.serial));
        entry.set("typographyEpoch", Value::number(capture.typographyEpoch));
        entry.set("row", Value::number(block.row));
        entry.set("column", Value::number(block.column));
        entry.set("width", Value::number(block.width));
        entry.set("height", Value::number(block.height));
        entry.set("sourceAnchor", Value::number(block.viewportTop));
        entry.set("viewportTop", Value::number(capture.viewportTop));
        Value rectangles = Value::array();
        for (const auto& rectangle : block.visibleRects) {
            Value item = Value::object();
            item.set("x", Value::number(rectangle.x));
            item.set("y", Value::number(rectangle.y));
            item.set("width", Value::number(rectangle.width));
            item.set("height", Value::number(rectangle.height));
            rectangles.push(std::move(item));
        }
        entry.set("visibleRects", std::move(rectangles));
        entry.set("shaped", Value::boolean(capture.isShaped));
        std::string text;
        for (const auto codePoint : block.document.text)
            Unicode::appendUTF8(text, RichText::copiedCodePoint(codePoint));
        entry.set("text", Value::string(std::move(text)));
        Value visible = Value::array();
        for (const auto& range : capture.visibleRanges) {
            Value item = Value::object();
            item.set("first", Value::number(range.first));
            item.set("end", Value::number(range.end));
            item.set("row", Value::number(range.row));
            item.set("column", Value::number(range.column));
            visible.push(std::move(item));
        }
        entry.set("visible", std::move(visible));
        Value lines = Value::array();
        Value graphemes = Value::array();
        if (capture.isShaped) {
            for (const auto& line : capture.lines) {
                Value item = Value::object();
                item.set("first", Value::number(line.first));
                item.set("end", Value::number(line.end));
                item.set("top", Value::number(line.top / 4096.0));
                item.set("height", Value::number(line.height / 4096.0));
                item.set("baseline", Value::number(line.baseline / 4096.0));
                item.set("x", Value::number(line.origin / 4096.0));
                item.set("width", Value::number(line.width / 4096.0));
                lines.push(std::move(item));
            }
            for (const auto& span : capture.spans) {
                Value item = Value::object();
                item.set("first", Value::number(span.first));
                item.set("end", Value::number(span.end));
                item.set("line", Value::number(span.line));
                item.set("leading", Value::number(span.leading / 4096.0));
                item.set("trailing", Value::number(span.trailing / 4096.0));
                graphemes.push(std::move(item));
            }
        }
        entry.set("lines", std::move(lines));
        entry.set("graphemes", std::move(graphemes));
        Value colors = Value::array();
        for (const auto& color : block.document.colorTable()) {
            Value item = Value::object();
            item.set("red", Value::number(color.red));
            item.set("green", Value::number(color.green));
            item.set("blue", Value::number(color.blue));
            colors.push(std::move(item));
        }
        entry.set("colors", std::move(colors));
        Value runs = Value::array();
        for (const auto& run : block.document.runs) {
            Value item = Value::object();
            item.set("first", Value::number(run.start));
            item.set("end", Value::number(run.start + run.length));
            item.set("halfPoints", Value::number(run.halfPoints));
            item.set("color", Value::number(run.color));
            item.set("background", Value::number(run.background));
            item.set("bold", Value::boolean(run.has(RichText::StyleBit::Bold)));
            item.set("italic", Value::boolean(run.has(RichText::StyleBit::Italic)));
            item.set("underline", Value::boolean(run.has(RichText::StyleBit::Underline)));
            item.set("strikethrough", Value::boolean(run.has(RichText::StyleBit::Strikethrough)));
            item.set("family", Value::string(run.font == RichText::noFontEntry ? "" : std::string(block.document.fonts[run.font].familyName())));
            const auto familyClass = run.font == RichText::noFontEntry ? RichText::FamilyClass::SansSerif : block.document.fonts[run.font].familyClass;
            item.set("familyClass", Value::string(familyClass == RichText::FamilyClass::Monospace ? "monospace" : familyClass == RichText::FamilyClass::Serif ? "serif"
                                                                                                                                                              : "sans-serif"));
            runs.push(std::move(item));
        }
        entry.set("runs", std::move(runs));
        blocks.push(std::move(entry));
    }
    richText.set("blocks", std::move(blocks));
    result.set("richText", std::move(richText));
    return result;
}

bool isBlankRequestLine(std::string_view line)
{
    for (char ch : line) {
        if (ch != ' ' && ch != '\t' && ch != '\r')
            return false;
    }
    return true;
}

bool constantTimeEquals(std::string_view a, std::string_view b)
{
    unsigned difference = static_cast<unsigned>(a.size() ^ b.size());
    const std::size_t span = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < span; ++i)
        difference |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    return difference == 0;
}

Value rpcResult(const Value* id, Value result)
{
    Value response = Value::object();
    response.set("jsonrpc", Value::string("2.0"));
    response.set("id", id ? *id : Value::null());
    response.set("result", std::move(result));
    return response;
}

std::string_view errorKind(ErrorCode code)
{
    switch (code) {
    case ErrorCode::ParseError:
        return "parse_error";
    case ErrorCode::InvalidRequest:
        return "invalid_request";
    case ErrorCode::MethodNotFound:
        return "method_not_found";
    case ErrorCode::InvalidParameters:
        return "bad_request";
    case ErrorCode::InternalError:
        return "internal";
    case ErrorCode::Unauthorized:
        return "unauthorized";
    case ErrorCode::Forbidden:
        return "forbidden";
    case ErrorCode::NotFound:
        return "not_found";
    case ErrorCode::Ambiguous:
        return "ambiguous";
    case ErrorCode::Busy:
        return "busy";
    }
    return "internal";
}

Value rpcError(const Value* id, ErrorCode code, std::string_view message, const Value& details)
{
    Value data = Value::object();
    data.set("kind", Value::string(std::string(errorKind(code))));
    for (const auto& [key, value] : details.members()) {
        if (key != "kind")
            data.set(key, value);
    }
    Value error = Value::object();
    error.set("code", Value::number(static_cast<int>(code)));
    error.set("message", Value::string(std::string(message)));
    error.set("data", std::move(data));
    Value response = Value::object();
    response.set("jsonrpc", Value::string("2.0"));
    response.set("id", id ? *id : Value::null());
    response.set("error", std::move(error));
    return response;
}

DecodedRequest decodeRequest(std::string_view line, std::string_view token)
{
    DecodedRequest decoded;

    if (isBlankRequestLine(line))
        return decoded;

    bool didParse = false;
    const Value message = Ubiquitty::Codec::JSON::parse(std::string(line), didParse);
    if (!didParse || !message.isObject()) {
        // No id is recoverable from a line that did not parse.
        decoded.outcome = RequestOutcome::Respond;
        decoded.response = rpcError(nullptr, ErrorCode::ParseError, "parse error");
        return decoded;
    }

    if (const Value* id = message.find("id")) {
        decoded.id = *id;
        decoded.hasID = true;
    }

    const Value* methodValue = message.find("method");
    if (!methodValue || !methodValue->isString()) {
        if (!decoded.hasID)
            return decoded;
        decoded.outcome = RequestOutcome::Respond;
        decoded.response = rpcError(decoded.idOrNull(), ErrorCode::InvalidRequest,
            "invalid request: no method");
        return decoded;
    }
    decoded.method = methodValue->asString();

    const Value* params = message.find("params");
    if (params && params->isObject())
        decoded.params = *params;

    if (!token.empty()) {
        const std::string supplied = decoded.params.stringOr("token", message.stringOr("token"));
        if (!constantTimeEquals(supplied, token)) {
            decoded.outcome = RequestOutcome::Respond;
            decoded.response = rpcError(decoded.idOrNull(), ErrorCode::Unauthorized,
                "unauthorized: bad or missing token");
            return decoded;
        }
    }

    // Checked after the token, so a caller the gate refuses learns nothing
    // about how its request would have been read.
    if (params && !params->isObject()) {
        if (!decoded.hasID)
            return decoded;
        decoded.outcome = RequestOutcome::Respond;
        decoded.response = rpcError(decoded.idOrNull(), ErrorCode::InvalidParameters, "params must be an object");
        return decoded;
    }

    decoded.outcome = RequestOutcome::Dispatch;
    return decoded;
}

} // namespace Ubiquitty::Protocol::Control
