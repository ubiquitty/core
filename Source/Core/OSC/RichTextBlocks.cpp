// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/OSC/RichTextBlocks.h"

#include "Core/EffectGate.h"
#include "Core/RichText/Layout.h"
#include "Core/RichText/RTFReader.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/TerminalShared.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <limits>
#include <span>
#include <utility>

namespace Ubiquitty::OSC::RichTextBlocks {

namespace {

/// The keys a code may carry, one bit each, to refuse a repeat.
enum class Key : std::uint32_t {
    Action = 1 << 0,
    Width = 1 << 1,
    Height = 1 << 2,
    ID = 1 << 3,
    Overflow = 1 << 4,
    Alignment = 1 << 5,
    Cursor = 1 << 6,
    Line = 1 << 7,
    Pointer = 1 << 8,
    Lifetime = 1 << 9,
    Caret = 1 << 10,
    Viewport = 1 << 11,
    VisibleRects = 1 << 12,
    Geometry = 1 << 13,
    Subscription = 1 << 14,
    Status = 1 << 15,
    Padding = 1 << 16,
    VerticalPlacement = 1 << 17,
    Epoch = 1 << 18,
    DocumentRevision = 1 << 19,
    CatalogOffset = 1 << 20,
    CatalogCount = 1 << 21,
};

[[nodiscard]] std::optional<unsigned> decimal(std::string_view value, unsigned largest)
{
    if (value.empty() || value.size() > (largest > 65535 ? 10 : 5))
        return std::nullopt;
    unsigned parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc {} || end != value.data() + value.size() || parsed > largest)
        return std::nullopt;
    return parsed;
}

[[nodiscard]] std::optional<char> letter(std::string_view value, std::string_view allowed)
{
    if (value.size() != 1 || allowed.find(value[0]) == std::string_view::npos)
        return std::nullopt;
    return value[0];
}

/// Reads `p`: one inset for every side, or four in the order top, right,
/// bottom, left, each 0–255 points.
[[nodiscard]] std::optional<RichText::Insets> insets(std::string_view value)
{
    std::array<std::uint8_t, 4> sides {};
    std::size_t count = 0;
    while (true) {
        const auto comma = value.find(',');
        if (count == sides.size())
            return std::nullopt;
        const auto parsed = decimal(value.substr(0, comma), 255);
        if (!parsed)
            return std::nullopt;
        sides[count++] = static_cast<std::uint8_t>(*parsed);
        if (comma == std::string_view::npos)
            break;
        value = value.substr(comma + 1);
    }
    if (count == 1)
        return RichText::Insets { sides[0], sides[0], sides[0], sides[0] };
    if (count != 4)
        return std::nullopt;
    return RichText::Insets { sides[0], sides[1], sides[2], sides[3] };
}

bool answerStatus(Terminal& t, std::uint16_t id, std::string_view status)
{
    const std::string reply = "\x1b]7404;a=e:i=" + std::to_string(id) + ":v=3:s=" + std::string(status) + std::string(oscReplyTerminator(t));
    return respondBulk(t, Capability::TextMeasureRead, std::span(reply.data(), reply.size()));
}

std::string_view readStatus(RichText::ReadError error)
{
    switch (error) {
    case RichText::ReadError::NotRTF:
    case RichText::ReadError::Malformed:
    case RichText::ReadError::InvalidUTF8:
    case RichText::ReadError::ParagraphOutOfRange:
        return "invalid";
    case RichText::ReadError::UnsupportedControl:
        return "unsupported";
    default:
        return "limit";
    }
}

bool answerFontCatalog(Terminal& t, std::uint16_t id, const RichText::Measurement& measured)
{
    const auto& page = *measured.fontCatalog;
    std::string_view status;
    switch (page.status) {
    case RichText::FontCatalogStatus::Ready:
        status = "ok";
        break;
    case RichText::FontCatalogStatus::Unavailable:
        status = "unavailable";
        break;
    case RichText::FontCatalogStatus::Denied:
        status = "denied";
        break;
    case RichText::FontCatalogStatus::Stale:
        status = "stale";
        break;
    case RichText::FontCatalogStatus::Invalid:
        status = "invalid";
        break;
    }
    std::string reply = "\x1b]7404;a=f:i=" + std::to_string(id) + ":v=3:E=" + std::to_string(measured.epoch)
        + ":F=" + std::to_string(std::to_underlying(page.scope)) + ":b=" + std::to_string(page.offset)
        + ":n=" + std::to_string(page.families.size()) + ":t=" + std::to_string(page.total) + ":s=" + std::string(status) + ';';
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t index = 0; index < page.families.size(); ++index) {
        if (index)
            reply.push_back(',');
        for (const unsigned char byte : page.families[index].familyName()) {
            reply.push_back(digits[byte >> 4]);
            reply.push_back(digits[byte & 15]);
        }
    }
    reply += oscReplyTerminator(t);
    if (reply.size() >= RichText::fontCatalogPacketBytes)
        return false;
    return respondBulk(t, Capability::TextMeasureRead, std::span(reply.data(), reply.size()));
}

void answerFontQuery(Terminal& t, const Request& request)
{
    if (replyDisposition(t, Capability::TextMeasureRead) != ReplyDisposition::Answer)
        return;
    if (t.richTextMeasurements.size() >= RichText::maxPendingMeasurements || !t.richTextMeasurements.tryReserve(t.richTextMeasurements.size() + 1)) {
        if (request.wantsStatus)
            (void)answerStatus(t, request.placement.id, "busy");
        return;
    }
    auto query = request.fontCatalog;
    query.rendererRevision = t.typography.facts ? t.typography.facts->rendererRevision : 1;
    RichText::PendingMeasurement pending;
    pending.request.id = request.placement.id;
    pending.request.rendererRevision = query.rendererRevision;
    pending.request.fontCatalog = query;
    pending.typographyEpoch = t.typography.epoch;
    pending.wantsStatus = request.wantsStatus;
    pending.admittedAt = terminalNow(t).value_or(Ticks {});
    if (t.typography.facts && t.typography.facts->fontCatalogScope != RichText::FontCatalogScope::Unavailable && !t.typography.fontCatalog.function) {
        if (!++t.richTextMeasurementToken)
            ++t.richTextMeasurementToken;
        pending.request.token = t.richTextMeasurementToken;
    } else {
        RichText::Measurement measured;
        measured.epoch = t.typography.epoch;
        measured.rendererRevision = query.rendererRevision;
        measured.fontCatalog = RichText::queryFontCatalog(query, t.typography);
        const auto now = terminalNow(t);
        if (now && !RichText::measurementWaitMicroseconds(pending, *now))
            pending.failure = RichText::MeasurementFailure::Timeout;
        else
            pending.completion = std::move(measured);
    }
    (void)t.richTextMeasurements.tryPushBack(std::move(pending));
    publishMeasurements(t);
}

void failMeasurement(RichText::PendingMeasurement& pending, RichText::MeasurementFailure failure)
{
    pending.failure = failure;
    pending.completion.reset();
    pending.request.document = {};
}

bool retireDuplicateQuery(Terminal& t, const Request& request)
{
    if (replyDisposition(t, Capability::TextMeasureRead) != ReplyDisposition::Answer)
        return false;
    const auto pending = std::ranges::find_if(t.richTextMeasurements, [&](const auto& measurement) {
        return !measurement.blockSerial && measurement.request.id == request.placement.id;
    });
    if (pending != t.richTextMeasurements.end()) {
        pending->wantsStatus |= request.wantsStatus;
        failMeasurement(*pending, RichText::MeasurementFailure::Duplicate);
        publishMeasurements(t);
        return true;
    }
    return false;
}

void answerQuery(Terminal& t, const Request& request, std::string_view body)
{
    if (replyDisposition(t, Capability::TextMeasureRead) != ReplyDisposition::Answer)
        return;
    if (t.richTextMeasurements.size() >= RichText::maxPendingMeasurements || !t.richTextMeasurements.tryReserve(t.richTextMeasurements.size() + 1)) {
        if (request.wantsStatus)
            (void)answerStatus(t, request.placement.id, "busy");
        return;
    }
    const auto admittedAt = terminalNow(t).value_or(Ticks {});
    RichText::Document document;
    if (!body.empty()) {
        auto read = RichText::readRTF(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
        if (!read) {
            if (request.wantsStatus)
                answerStatus(t, request.placement.id, readStatus(read.error()));
            return;
        }
        document = std::move(*read);
    }
    const auto& placement = request.placement;
    const RichText::ShapeRequest shape { placement.width, placement.height, placement.overflow, placement.alignment, placement.lineMode, placement.padding };
    if (!document.text.empty() && !t.typography.shaper.function && t.typography.facts && t.typography.facts->shaping) {
        if (!++t.richTextMeasurementToken)
            ++t.richTextMeasurementToken;
        if (!t.richTextMeasurements.tryPushBack({ { t.richTextMeasurementToken, placement.id, shape, std::move(document), request.needsGeometry },
                0, 0, 0, t.typography.epoch, request.wantsStatus, admittedAt })
            && request.wantsStatus)
            (void)answerStatus(t, placement.id, "busy");
        return;
    }
    auto measured = RichText::measure(document, shape, t.typography);
    if (document.text.empty() && t.typography.facts)
        measured.isShaped = t.typography.facts->shaping != 0;
    RichText::PendingMeasurement completed;
    completed.request.id = placement.id;
    completed.request.needsGeometry = request.needsGeometry;
    completed.typographyEpoch = t.typography.epoch;
    completed.wantsStatus = request.wantsStatus;
    completed.admittedAt = admittedAt;
    const auto now = terminalNow(t);
    if (now && !RichText::measurementWaitMicroseconds(completed, *now))
        completed.failure = RichText::MeasurementFailure::Timeout;
    else
        completed.completion = std::move(measured);
    (void)t.richTextMeasurements.tryPushBack(std::move(completed));
    publishMeasurements(t);
}

// Even a malformed request may opt into a correlated error. Ambiguous IDs
// and repeated opt-ins stay silent; none of the other keys are trusted here.
std::optional<std::uint16_t> statusID(std::string_view keys)
{
    std::optional<unsigned> id;
    bool wantsStatus = false;
    bool hasStatus = false;
    while (!keys.empty()) {
        const auto colon = keys.find(':');
        const auto pair = keys.substr(0, colon);
        keys = colon == std::string_view::npos ? std::string_view {} : keys.substr(colon + 1);
        if (pair.starts_with("i=")) {
            if (id)
                return std::nullopt;
            id = decimal(pair.substr(2), 65535);
            if (!id)
                return std::nullopt;
        }
        if (pair.starts_with("e=")) {
            if (hasStatus)
                return std::nullopt;
            hasStatus = true;
            wantsStatus = pair == "e=1";
        }
    }
    return wantsStatus && id ? std::optional(static_cast<std::uint16_t>(*id)) : std::nullopt;
}

bool reportTypography(Terminal& t)
{
    if (!t.typography.subscriptionID)
        return true;
    const auto* facts = t.typography.facts.get();
    std::array<char, 192> buffer {};
    const auto length = std::snprintf(buffer.data(), buffer.size(), "\x1b]7404;a=n:i=%u:v=3:E=%u:W=%u:H=%u:B=%u:S=%u:Q=%d:K=%d:F=%u:T=%u\x1b\\",
        t.typography.subscriptionID, t.typography.epoch, facts ? facts->cellWidth : 0, facts ? facts->cellHeight : 0,
        facts ? facts->baseline : 0, facts ? facts->proportionalSize : 0, facts && facts->shaping ? 1 : 0, facts && facts->rendersBlocks ? 1 : 0,
        facts ? std::to_underlying(facts->fontCatalogScope) : 0, RichText::maxMeasurementWaitMilliseconds);
    if (length > 0 && static_cast<std::size_t>(length) < buffer.size())
        return respondBulk(t, Capability::TextMeasureRead, std::span(buffer.data(), static_cast<std::size_t>(length)));
    return false;
}

} // namespace

void typographyChanged(Terminal& t)
{
    for (auto& pending : t.richTextMeasurements) {
        if (!pending.blockSerial && pending.failure == RichText::MeasurementFailure::None)
            failMeasurement(pending, RichText::MeasurementFailure::Stale);
    }
    const auto removed = std::ranges::remove_if(t.richTextMeasurements, [](const auto& pending) { return pending.blockSerial != 0; });
    t.richTextMeasurements.erase(removed.begin(), removed.end());
    t.typography.needsReport = t.typography.subscriptionID != 0;
    publishMeasurements(t);
}

void expireMeasurements(Terminal& t, Ticks now)
{
    const auto expiredBlocks = std::ranges::remove_if(t.richTextMeasurements, [&](const auto& pending) {
        return pending.blockSerial && !RichText::measurementWaitMicroseconds(pending, now);
    });
    t.richTextMeasurements.erase(expiredBlocks.begin(), expiredBlocks.end());
    for (auto& pending : t.richTextMeasurements) {
        if (!pending.blockSerial && !pending.completion && pending.failure == RichText::MeasurementFailure::None
            && !RichText::measurementWaitMicroseconds(pending, now))
            failMeasurement(pending, RichText::MeasurementFailure::Timeout);
    }
    publishMeasurements(t);
}

bool completeMeasurement(Terminal& t, std::uint32_t token, RichText::Measurement measurement)
{
    if (const auto now = terminalNow(t))
        expireMeasurements(t, *now);
    const auto pending = std::ranges::find_if(t.richTextMeasurements, [&](const auto& candidate) {
        return candidate.request.token == token && !candidate.blockSerial && !candidate.completion
            && candidate.failure == RichText::MeasurementFailure::None && candidate.typographyEpoch == t.typography.epoch;
    });
    if (pending == t.richTextMeasurements.end())
        return false;
    if (pending->request.fontCatalog.has_value() != measurement.fontCatalog.has_value()
        || (pending->request.fontCatalog && (!RichText::isFontCatalogPage(*pending->request.fontCatalog, *measurement.fontCatalog) || !t.typography.facts || measurement.fontCatalog->scope != t.typography.facts->fontCatalogScope)))
        return false;
    measurement.epoch = pending->typographyEpoch;
    pending->completion = std::move(measurement);
    pending->request.document = {};
    return true;
}

void publishMeasurements(Terminal& t)
{
    for (auto pending = t.richTextMeasurements.begin(); pending != t.richTextMeasurements.end();) {
        if (pending->blockSerial) {
            ++pending;
            continue;
        }
        bool didPublish = false;
        if (pending->completion)
            didPublish = pending->completion->fontCatalog ? answerFontCatalog(t, pending->request.id, *pending->completion)
                                                          : answerMeasurement(t, pending->request.id, *pending->completion, pending->request.needsGeometry ? GeometryReport::Include : GeometryReport::Omit);
        else if (pending->failure != RichText::MeasurementFailure::None) {
            std::string_view status;
            switch (pending->failure) {
            case RichText::MeasurementFailure::Timeout:
                status = "timeout";
                break;
            case RichText::MeasurementFailure::Cancelled:
                status = "cancelled";
                break;
            case RichText::MeasurementFailure::Duplicate:
                status = "duplicate";
                break;
            case RichText::MeasurementFailure::Stale:
                status = "stale";
                break;
            case RichText::MeasurementFailure::None:
                break;
            }
            didPublish = !pending->wantsStatus || answerStatus(t, pending->request.id, status);
        }
        if (didPublish)
            pending = t.richTextMeasurements.erase(pending);
        else if (pending->completion || pending->failure != RichText::MeasurementFailure::None)
            break;
        else
            ++pending;
    }
    if (t.typography.needsReport && reportTypography(t))
        t.typography.needsReport = false;
}

bool answerMeasurement(Terminal& t, std::uint16_t id, const RichText::Measurement& measured, GeometryReport geometry)
{
    const auto spans = geometry == GeometryReport::Include && measured.isShaped ? std::span(measured.geometry.spans) : std::span<const RichText::ShapedSpan> {};
    std::string reply;
    constexpr std::size_t spansPerChunk = 128;
    for (std::size_t start = 0; start < spans.size(); start += spansPerChunk) {
        reply += "\x1b]7404;a=g:i=" + std::to_string(id) + ":v=3:E=" + std::to_string(measured.epoch) + ":s=" + std::to_string(start) + ":G=";
        const auto end = std::min(spans.size(), start + spansPerChunk);
        for (auto index = start; index < end; ++index) {
            const auto& span = spans[index];
            if (index != start)
                reply.push_back(',');
            reply += std::to_string(span.first) + ',' + std::to_string(span.end) + ',' + std::to_string(span.line) + ','
                + std::to_string((static_cast<std::int64_t>(span.leading) + 32) / 64) + ',' + std::to_string((static_cast<std::int64_t>(span.trailing) + 32) / 64);
        }
        reply += oscReplyTerminator(t);
    }
    constexpr std::string_view replyFormat = "\x1b]7404;a=r:i=%u:f=%d:w=%u:h=%u:n=%u:c=%u:l=%u:m=%u:x=%d:v=3:W=%u:H=%u:B=%u:S=%u:E=%u:L=%u:A=%u:Q=%d:K=%d:F=%u:V=1:N=%zu:T=%u:Z=body=%zu,scalars=%d,runs=%d,blocks=%d,pending=%zu,rectangles=%zu,carets=%zu,fonts=%d,colors=%d,depth=%d,family=%d,width=%d,height=%d,open-width=%u,open-height=%u,retained-bytes=%zu,retained-used=%zu,catalog-count=%u,catalog-bytes=%zu:R=";
    constexpr auto numberCount = std::count(replyFormat.begin(), replyFormat.end(), '%');
    constexpr auto maxNumberBytes = std::numeric_limits<std::uint64_t>::digits10 + 1;
    std::array<char, replyFormat.size() + static_cast<std::size_t>(numberCount * maxNumberBytes) + 1> header {};
    const int length = std::snprintf(header.data(), header.size(), replyFormat.data(),
        static_cast<unsigned>(id), measured.isFitting ? 1 : 0, measured.width, measured.height, measured.shown,
        measured.capacity, measured.lineCapacity, measured.minWidth, measured.isExact ? 1 : 0, measured.cellWidth, measured.cellHeight,
        measured.baseline, measured.fontSize, measured.epoch, measured.lineHeight, measured.lineBaseline, measured.isShaped ? 1 : 0, t.typography.facts && t.typography.facts->rendersBlocks ? 1 : 0,
        t.typography.facts ? std::to_underlying(t.typography.facts->fontCatalogScope) : 0,
        spans.size(), RichText::maxMeasurementWaitMilliseconds, RichText::maxBodyBytes, RichText::maxCodePoints, RichText::maxRuns, RichText::maxBlocksPerScreen,
        RichText::maxPendingMeasurements, RichText::maxVisibleRects, spansPerChunk,
        RichText::maxFonts, RichText::maxColors, RichText::maxGroupDepth, RichText::maxFamilyNameBytes,
        maxColumns, maxColumns, RichText::openWidthCells, RichText::openHeightCells,
        t.richTextBlocks.storageLimit(), t.richTextBlocks.storageBytes(), RichText::maxFontCatalogPageFamilies, RichText::maxFontCatalogPageBytes);
    if (length <= 0 || static_cast<std::size_t>(length) >= header.size())
        return false;
    reply.append(header.data(), static_cast<std::size_t>(length));
    for (std::size_t index = 0; index < measured.lineEnds.size(); ++index) {
        if (index)
            reply.push_back(',');
        reply += std::to_string(measured.lineEnds[index]);
    }
    reply += oscReplyTerminator(t);
    // One bulk append keeps geometry chunks and their final result atomic.
    return respondBulk(t, Capability::TextMeasureRead, std::span(reply.data(), reply.size()));
}

std::optional<Request> parseKeys(std::string_view keys)
{
    if (keys.ends_with(':'))
        return std::nullopt;
    Request request;
    std::uint32_t seen = 0;
    while (!keys.empty()) {
        const std::size_t colon = keys.find(':');
        const std::string_view pair = keys.substr(0, colon);
        keys = colon == std::string_view::npos ? std::string_view {} : keys.substr(colon + 1);
        const std::size_t equals = pair.find('=');
        if (equals != 1)
            return std::nullopt;
        const char name = pair[0];
        const std::string_view value = pair.substr(2);
        Key key = Key::Action;
        switch (name) {
        case 'D': {
            const auto revision = decimal(value, std::numeric_limits<std::uint32_t>::max());
            if (!revision)
                return std::nullopt;
            request.placement.documentRevision = *revision;
            key = Key::DocumentRevision;
            break;
        }
        case 'E': {
            const auto epoch = decimal(value, std::numeric_limits<std::uint32_t>::max());
            if (!epoch)
                return std::nullopt;
            request.expectedTypographyEpoch = *epoch;
            key = Key::Epoch;
            break;
        }
        case 'a': {
            const std::optional<char> action = letter(value, "pqdscf");
            if (!action)
                return std::nullopt;
            request.action = *action == 'p' ? Action::Place : *action == 'q' ? Action::Query
                : *action == 'd'                                             ? Action::Delete
                : *action == 's'                                             ? Action::Subscribe
                : *action == 'f'                                             ? Action::FontCatalog
                                                                             : Action::Cancel;
            key = Key::Action;
            break;
        }
        case 'G':
        case 'N':
        case 'e': {
            const auto mode = decimal(value, 1);
            if (!mode)
                return std::nullopt;
            if (name == 'G')
                request.needsGeometry = *mode;
            else if (name == 'N')
                request.shouldSubscribe = *mode;
            else
                request.wantsStatus = *mode;
            key = name == 'G' ? Key::Geometry : name == 'N' ? Key::Subscription
                                                            : Key::Status;
            break;
        }
        case 'w':
        case 'h': {
            // Zero opens the dimension, which only a query may ask for.
            const std::optional<unsigned> cells = decimal(value, Ubiquitty::maxColumns);
            if (!cells)
                return std::nullopt;
            (name == 'w' ? request.placement.width : request.placement.height) = static_cast<std::uint16_t>(*cells);
            key = name == 'w' ? Key::Width : Key::Height;
            break;
        }
        case 'i': {
            const std::optional<unsigned> id = decimal(value, 65535);
            if (!id)
                return std::nullopt;
            request.placement.id = static_cast<std::uint16_t>(*id);
            request.hasID = true;
            key = Key::ID;
            break;
        }
        case 'b':
        case 'n': {
            const auto number = decimal(value, name == 'b' ? std::numeric_limits<std::uint32_t>::max() : RichText::maxFontCatalogPageFamilies);
            if (!number || (name == 'n' && !*number))
                return std::nullopt;
            if (name == 'b')
                request.fontCatalog.offset = *number;
            else
                request.fontCatalog.count = static_cast<std::uint16_t>(*number);
            key = name == 'b' ? Key::CatalogOffset : Key::CatalogCount;
            break;
        }
        case 'o': {
            const std::optional<char> overflow = letter(value, "wce");
            if (!overflow)
                return std::nullopt;
            request.placement.overflow = *overflow == 'w' ? RichText::Overflow::Wrap : *overflow == 'c' ? RichText::Overflow::Clip
                                                                                                        : RichText::Overflow::Ellipsis;
            key = Key::Overflow;
            break;
        }
        case 'j': {
            const std::optional<char> alignment = letter(value, "lcr");
            if (!alignment)
                return std::nullopt;
            request.placement.alignment = *alignment == 'l' ? RichText::Alignment::Left : *alignment == 'c' ? RichText::Alignment::Center
                                                                                                            : RichText::Alignment::Right;
            key = Key::Alignment;
            break;
        }
        case 'v': {
            const auto top = decimal(value, RichText::maxCodePoints);
            if (!top)
                return std::nullopt;
            request.placement.viewportTop = static_cast<std::uint16_t>(*top);
            key = Key::Viewport;
            break;
        }
        case 'r': {
            auto remaining = value;
            while (!remaining.empty()) {
                if (request.placement.visibleRects.size() == RichText::maxVisibleRects)
                    return std::nullopt;
                std::array<std::uint16_t, 4> fields {};
                for (std::size_t index = 0; index < fields.size(); ++index) {
                    const auto separator = remaining.find(',');
                    const auto number = decimal(remaining.substr(0, separator), Ubiquitty::maxColumns);
                    if (!number || (separator == std::string_view::npos && index != fields.size() - 1))
                        return std::nullopt;
                    fields[index] = static_cast<std::uint16_t>(*number);
                    if (separator != std::string_view::npos && separator + 1 == remaining.size())
                        return std::nullopt;
                    remaining = separator == std::string_view::npos ? std::string_view {} : remaining.substr(separator + 1);
                }
                request.placement.visibleRects.push_back({ fields[0], fields[1], fields[2], fields[3] });
            }
            if (request.placement.visibleRects.empty())
                return std::nullopt;
            key = Key::VisibleRects;
            break;
        }
        case 'k': {
            const std::optional<unsigned> caret = decimal(value, RichText::maxCodePoints);
            if (!caret)
                return std::nullopt;
            request.placement.caret = static_cast<std::uint16_t>(*caret);
            key = Key::Caret;
            break;
        }
        case 'P': {
            const std::optional<unsigned> mode = decimal(value, 1);
            if (!mode)
                return std::nullopt;
            request.placement.lifetime = *mode ? RichText::Lifetime::Overlay : RichText::Lifetime::Cells;
            key = Key::Lifetime;
            break;
        }
        case 'g':
        case 'M': {
            const std::optional<unsigned> mode = decimal(value, 1);
            if (!mode)
                return std::nullopt;
            if (name == 'g')
                request.placement.lineMode = *mode ? RichText::LineMode::Terminal : RichText::LineMode::Natural;
            else
                request.placement.pointerMode = *mode ? RichText::PointerMode::Characters : RichText::PointerMode::Cells;
            key = name == 'g' ? Key::Line : Key::Pointer;
            break;
        }
        case 'p': {
            const std::optional<RichText::Insets> padding = insets(value);
            if (!padding)
                return std::nullopt;
            request.placement.padding = *padding;
            key = Key::Padding;
            break;
        }
        case 'y': {
            const std::optional<char> placement = letter(value, "tcb");
            if (!placement)
                return std::nullopt;
            request.placement.verticalPlacement = *placement == 't' ? RichText::VerticalPlacement::Top : *placement == 'c' ? RichText::VerticalPlacement::Center
                                                                                                                           : RichText::VerticalPlacement::Bottom;
            key = Key::VerticalPlacement;
            break;
        }
        case 'C': {
            const std::optional<unsigned> stays = decimal(value, 1);
            if (!stays)
                return std::nullopt;
            request.placement.cursor = *stays ? RichText::CursorPolicy::Stay : RichText::CursorPolicy::Move;
            key = Key::Cursor;
            break;
        }
        default:
            return std::nullopt;
        }
        const auto bit = std::to_underlying(key);
        if (seen & bit)
            return std::nullopt;
        seen |= bit;
    }
    constexpr auto geometry = static_cast<std::uint32_t>(static_cast<std::uint32_t>(Key::Width) | static_cast<std::uint32_t>(Key::Height));
    constexpr auto layout = static_cast<std::uint32_t>(static_cast<std::uint32_t>(Key::Overflow) | static_cast<std::uint32_t>(Key::Alignment) | static_cast<std::uint32_t>(Key::Line) | static_cast<std::uint32_t>(Key::Pointer) | static_cast<std::uint32_t>(Key::Lifetime) | static_cast<std::uint32_t>(Key::Caret));
    if ((request.wantsStatus && !request.hasID)
        || (request.action != Action::Place && (seen & std::to_underlying(Key::DocumentRevision)))
        || (request.expectedTypographyEpoch && request.action != Action::Place && request.action != Action::Query && request.action != Action::FontCatalog)
        || (request.action != Action::FontCatalog && (seen & (std::to_underlying(Key::CatalogOffset) | std::to_underlying(Key::CatalogCount))))
        || (request.action != Action::Query && (seen & std::to_underlying(Key::Geometry)))
        || (request.action != Action::Subscribe && (seen & std::to_underlying(Key::Subscription))))
        return std::nullopt;
    switch (request.action) {
    case Action::FontCatalog:
        if (!request.hasID || (request.fontCatalog.offset && !request.expectedTypographyEpoch)
            || (seen & ~(std::to_underlying(Key::Action) | std::to_underlying(Key::ID) | std::to_underlying(Key::Epoch) | std::to_underlying(Key::CatalogOffset) | std::to_underlying(Key::CatalogCount) | std::to_underlying(Key::Status))))
            return std::nullopt;
        break;
    case Action::Place:
        if (!request.placement.width || !request.placement.height)
            return std::nullopt;
        if (!RichText::validVisibleRects(request.placement.visibleRects, request.placement.width, request.placement.height))
            return std::nullopt;
        if (request.placement.viewportTop && request.placement.lineMode != RichText::LineMode::Terminal)
            return std::nullopt;
        if ((seen & geometry) != geometry || (request.placement.pointerMode == RichText::PointerMode::Characters && (!request.placement.id || !request.placement.documentRevision)))
            return std::nullopt;
        break;
    case Action::Query:
        if (seen & (static_cast<std::uint16_t>(Key::Viewport) | static_cast<std::uint16_t>(Key::VisibleRects)))
            return std::nullopt;
        if ((seen & geometry) != geometry || (seen & (static_cast<std::uint16_t>(Key::Cursor) | static_cast<std::uint16_t>(Key::Pointer) | static_cast<std::uint16_t>(Key::Lifetime) | static_cast<std::uint16_t>(Key::Caret))))
            return std::nullopt;
        break;
    case Action::Subscribe:
        if (!request.placement.id || !(seen & std::to_underlying(Key::Subscription))
            || (seen & ~(std::to_underlying(Key::Action) | std::to_underlying(Key::ID) | std::to_underlying(Key::Subscription) | std::to_underlying(Key::Status))))
            return std::nullopt;
        break;
    case Action::Cancel:
        if (!request.hasID || (seen & ~(std::to_underlying(Key::Action) | std::to_underlying(Key::ID) | std::to_underlying(Key::Status))))
            return std::nullopt;
        break;
    case Action::Delete:
        if (seen & (static_cast<std::uint16_t>(Key::Viewport) | static_cast<std::uint16_t>(Key::VisibleRects)))
            return std::nullopt;
        if (seen & (std::to_underlying(Key::Padding) | std::to_underlying(Key::VerticalPlacement)))
            return std::nullopt;
        if (seen & (geometry | layout | static_cast<std::uint16_t>(Key::Cursor)))
            return std::nullopt;
        break;
    }
    return request;
}

void handle(Ubiquitty::Terminal& t, std::string_view payload)
{
    if (const auto now = terminalNow(t))
        expireMeasurements(t, *now);
    const std::size_t semicolon = payload.find(';');
    const std::string_view keys = payload.substr(0, semicolon);
    const std::string_view body = semicolon == std::string_view::npos ? std::string_view {} : payload.substr(semicolon + 1);
    const std::optional<Request> request = parseKeys(keys);
    if (!request) {
        if (const auto id = statusID(keys)) {
            bool hasUnknownKey = false;
            auto remaining = keys;
            while (!remaining.empty()) {
                const auto colon = remaining.find(':');
                const auto pair = remaining.substr(0, colon);
                hasUnknownKey |= pair.size() >= 2 && pair[1] == '=' && std::string_view("awhiojCMPgkvrGNepyEDbn").find(pair[0]) == std::string_view::npos;
                remaining = colon == std::string_view::npos ? std::string_view {} : remaining.substr(colon + 1);
            }
            answerStatus(t, *id, hasUnknownKey ? "unsupported" : "invalid");
        }
        return;
    }
    if ((request->action == Action::Query || request->action == Action::FontCatalog) && retireDuplicateQuery(t, *request))
        return;
    if (request->expectedTypographyEpoch && *request->expectedTypographyEpoch != t.typography.epoch) {
        if (request->wantsStatus)
            answerStatus(t, request->placement.id, "stale");
        return;
    }
    switch (request->action) {
    case Action::FontCatalog:
        if (!body.empty()) {
            if (request->wantsStatus)
                (void)answerStatus(t, request->placement.id, "invalid");
            return;
        }
        answerFontQuery(t, *request);
        return;
    case Action::Cancel: {
        if (replyDisposition(t, Capability::TextMeasureRead) != ReplyDisposition::Answer)
            return;
        const auto pending = std::ranges::find_if(t.richTextMeasurements, [&](const auto& measurement) {
            return !measurement.blockSerial && measurement.request.id == request->placement.id;
        });
        if (pending == t.richTextMeasurements.end()) {
            if (request->wantsStatus)
                answerStatus(t, request->placement.id, "absent");
            return;
        }
        pending->wantsStatus |= request->wantsStatus;
        failMeasurement(*pending, RichText::MeasurementFailure::Cancelled);
        publishMeasurements(t);
        return;
    }
    case Action::Subscribe:
        if (replyDisposition(t, Capability::TextMeasureRead) != ReplyDisposition::Answer)
            return;
        t.typography.subscriptionID = request->shouldSubscribe ? request->placement.id : 0;
        t.typography.needsReport = request->shouldSubscribe;
        publishMeasurements(t);
        if (request->wantsStatus)
            answerStatus(t, request->placement.id, "ok");
        return;
    case Action::Delete:
        if (!effectAllowed(t, Capability::RichTextRender)) {
            if (request->wantsStatus)
                answerStatus(t, request->placement.id, "denied");
            return;
        }
        if (request->hasID)
            RichText::removeBlocks(t, request->placement.id);
        else
            RichText::removeBlocks(t, std::nullopt);
        if (request->wantsStatus)
            answerStatus(t, request->placement.id, "ok");
        return;
    case Action::Query:
        answerQuery(t, *request, body);
        return;
    case Action::Place: {
        // The payload's chars are the RTF body's octets.
        auto document = RichText::readRTF(std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
        if (!document) {
            if (request->wantsStatus)
                answerStatus(t, request->placement.id, readStatus(document.error()));
            return;
        }
        const auto placed = RichText::placeBlock(t, request->placement, std::move(*document));
        if (request->wantsStatus)
            answerStatus(t, request->placement.id, placed ? "ok" : placed.error() == RichText::PlacementError::Denied ? "denied"
                    : placed.error() == RichText::PlacementError::Limit                                               ? "limit"
                    : placed.error() == RichText::PlacementError::Revision                                            ? "conflict"
                                                                                                                      : "geometry");
        return;
    }
    }
}

} // namespace Ubiquitty::OSC::RichTextBlocks
