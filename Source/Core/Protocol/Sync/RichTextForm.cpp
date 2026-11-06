// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/RichTextForm.h"

#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/VT/Constants.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

/// The deepest a block's top may sit in the scrollback: the deepest a row can.
constexpr std::int64_t deepestRow = -static_cast<std::int64_t>(Ubiquitty::maxRows) * 1024;

/// One placement as the state block carries it.
struct PlacementRecord {
    ScreenMode screen = ScreenMode::Primary;
    RichText::Block block;
};

[[nodiscard]] const RichText::Document* heldDocument(std::uint32_t serial, std::span<const RichTextDefinition> definitions, const RichText::BlockStore& held)
{
    for (const RichTextDefinition& definition : definitions) {
        if (definition.serial == serial)
            return &definition.document;
    }
    for (const ScreenMode screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const RichText::Block& block : held.blocks(screen)) {
            if (block.serial == serial)
                return &block.document;
        }
    }
    return nullptr;
}

} // namespace

void writeMeasureRequest(WireWriter& writer, const RichText::MeasureRequest& request, std::uint64_t rendererRevision)
{
    writer.u32(request.token);
    writer.u64(rendererRevision ? rendererRevision : request.rendererRevision);
    writer.u16(request.id);
    writer.u8(request.fontCatalog ? 1 : 0);
    if (request.fontCatalog) {
        writer.u32(request.fontCatalog->offset);
        writer.u8(static_cast<std::uint8_t>(request.fontCatalog->count));
        return;
    }
    writer.u16(request.shape.width);
    writer.u16(request.shape.height);
    writer.u8(std::to_underlying(request.shape.overflow));
    writer.u8(std::to_underlying(request.shape.alignment));
    writer.u8(std::to_underlying(request.shape.lineMode));
    writer.u8(request.shape.padding.top);
    writer.u8(request.shape.padding.right);
    writer.u8(request.shape.padding.bottom);
    writer.u8(request.shape.padding.left);
    writer.u16(request.shape.viewportAnchor);
    writer.u8(request.needsGeometry ? 1 : 0);
    writeDocument(writer, request.document);
}

bool readMeasureRequest(WireReader& reader, RichText::MeasureRequest& request)
{
    request.token = reader.u32();
    request.rendererRevision = reader.u64();
    request.id = reader.u16();
    const auto mode = reader.u8();
    if (!request.token || !request.rendererRevision || mode > 1) {
        reader.fail();
        return false;
    }
    if (mode) {
        request.fontCatalog = RichText::FontCatalogQuery { reader.u32(), reader.u8(), request.rendererRevision };
        if (!request.fontCatalog->count)
            reader.fail();
        return reader.ok();
    }
    request.fontCatalog.reset();
    request.shape.width = reader.u16();
    request.shape.height = reader.u16();
    const auto overflow = reader.u8();
    const auto alignment = reader.u8();
    const auto lineMode = reader.u8();
    request.shape.padding = { reader.u8(), reader.u8(), reader.u8(), reader.u8() };
    request.shape.viewportAnchor = reader.u16();
    const auto geometry = reader.u8();
    request.needsGeometry = geometry == 1;
    // A zero width or height is a query's open dimension.
    if (geometry > 1 || !request.token || !request.rendererRevision || request.shape.width > maxColumns || request.shape.height > maxColumns + RichText::maxCodePoints
        || (request.shape.viewportAnchor && (request.shape.height > maxColumns || lineMode != std::to_underlying(RichText::LineMode::Terminal)))
        || overflow > std::to_underlying(RichText::Overflow::Ellipsis) || alignment > std::to_underlying(RichText::Alignment::Right)
        || lineMode > std::to_underlying(RichText::LineMode::Terminal)) {
        reader.fail();
        return false;
    }
    request.shape.overflow = static_cast<RichText::Overflow>(overflow);
    request.shape.alignment = static_cast<RichText::Alignment>(alignment);
    request.shape.lineMode = static_cast<RichText::LineMode>(lineMode);
    if (!readDocument(reader, request.document) || request.shape.viewportAnchor > request.document.text.size()) {
        reader.fail();
        return false;
    }
    return true;
}

namespace {

void writeGeometry(WireWriter& writer, const RichText::ShapedText& geometry)
{
    writer.varint(geometry.lines.size());
    for (const auto& line : geometry.lines) {
        writer.u32(line.first);
        writer.u32(line.end);
        writer.u32(line.top);
        writer.u32(line.height);
        writer.u32(line.baseline);
        writer.u32(line.origin);
        writer.u32(line.width);
    }
    writer.varint(geometry.spans.size());
    for (const auto& span : geometry.spans) {
        writer.u32(span.first);
        writer.u32(span.end);
        writer.u32(span.line);
        writer.u32(static_cast<std::uint32_t>(span.leading));
        writer.u32(static_cast<std::uint32_t>(span.trailing));
    }
}

RichText::ShapedText readGeometry(WireReader& reader)
{
    RichText::ShapedText geometry;
    geometry.lines.resize(reader.count(RichText::maxCodePoints, 28));
    for (auto& line : geometry.lines)
        line = { reader.u32(), reader.u32(), reader.u32(), reader.u32(), reader.u32(), reader.u32(), reader.u32() };
    geometry.spans.resize(reader.count(RichText::maxCodePoints, 20));
    for (auto& span : geometry.spans)
        span = { reader.u32(), reader.u32(), reader.u32(), static_cast<std::int32_t>(reader.u32()), static_cast<std::int32_t>(reader.u32()) };
    std::uint32_t end = 0;
    std::uint32_t top = 0;
    constexpr auto largestPosition = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    for (const auto& line : geometry.lines) {
        if (line.first != end || line.end <= end || line.end > RichText::maxCodePoints
            || line.top != top || !line.height || line.height >= largestPosition - std::min(top, largestPosition)
            || line.baseline > line.height || line.origin >= largestPosition || line.width >= largestPosition)
            reader.fail();
        end = line.end;
        top += line.height;
    }
    end = 0;
    for (const auto& span : geometry.spans) {
        if (span.first != end || span.end <= end || span.line >= geometry.lines.size()
            || span.leading < 0 || span.trailing < 0 || span.leading >= static_cast<std::int32_t>(largestPosition)
            || span.trailing >= static_cast<std::int32_t>(largestPosition))
            reader.fail();
        if (span.line < geometry.lines.size()) {
            const auto& line = geometry.lines[span.line];
            if (span.first < line.first || span.end > line.end)
                reader.fail();
        }
        end = span.end;
    }
    if (end != (geometry.lines.empty() ? 0 : geometry.lines.back().end))
        reader.fail();
    return geometry;
}

} // namespace

void writeMeasurement(WireWriter& writer, const RichText::Measurement& value)
{
    writer.u64(value.rendererRevision);
    writer.u8(value.fontCatalog ? 1 : 0);
    if (value.fontCatalog) {
        const auto& page = *value.fontCatalog;
        writer.u8(std::to_underlying(page.status));
        writer.u8(std::to_underlying(page.scope));
        writer.u32(page.offset);
        writer.u32(page.total);
        writer.u8(static_cast<std::uint8_t>(page.families.size()));
        for (const auto& family : page.families)
            writer.utf8(family.familyName());
        return;
    }
    writer.varint(value.width);
    writer.varint(value.height);
    writer.varint(value.shown);
    writer.varint(value.capacity);
    writer.varint(value.lineCapacity);
    writer.varint(value.epoch);
    writer.varint(value.cellWidth);
    writer.varint(value.cellHeight);
    writer.varint(value.baseline);
    writer.varint(value.fontSize);
    writer.varint(value.lineHeight);
    writer.varint(value.lineBaseline);
    writer.varint(value.minWidth);
    writer.u8((value.isFitting ? 1 : 0) | (value.isExact ? 2 : 0) | (value.isShaped ? 4 : 0));
    writer.varint(value.lineEnds.size());
    for (const auto end : value.lineEnds)
        writer.varint(end);
    writeGeometry(writer, value.geometry);
    writer.u8(value.geometry.elision ? 1 : 0);
    if (value.geometry.elision) {
        writer.varint(value.geometry.elisionEnd);
        writeGeometry(writer, *value.geometry.elision);
    }
}

bool readMeasurement(WireReader& reader, RichText::Measurement& value)
{
    value.rendererRevision = reader.u64();
    const auto mode = reader.u8();
    if (!value.rendererRevision || mode > 1) {
        reader.fail();
        return false;
    }
    if (mode) {
        RichText::FontCatalogPage page;
        page.rendererRevision = value.rendererRevision;
        page.status = static_cast<RichText::FontCatalogStatus>(reader.u8());
        page.scope = static_cast<RichText::FontCatalogScope>(reader.u8());
        page.offset = reader.u32();
        page.total = reader.u32();
        const auto count = reader.u8();
        for (unsigned index = 0; index < count && reader.ok(); ++index) {
            if (!page.appendFamilyView(reader.utf8(RichText::maxFamilyNameBytes)))
                reader.fail();
        }
        if (!RichText::isFontCatalogPage({ page.offset, RichText::maxFontCatalogPageFamilies, page.rendererRevision }, page))
            reader.fail();
        value.fontCatalog = std::move(page);
        return reader.ok();
    }
    value.fontCatalog.reset();
    value.width = static_cast<std::uint32_t>(reader.varint((1u << 24)));
    value.height = static_cast<std::uint32_t>(reader.varint((1u << 24)));
    value.shown = static_cast<std::uint32_t>(reader.varint((1u << 24)));
    value.capacity = static_cast<std::uint32_t>(reader.varint((1u << 24)));
    value.lineCapacity = static_cast<std::uint32_t>(reader.varint((1u << 24)));
    value.epoch = static_cast<std::uint32_t>(reader.varint(std::numeric_limits<std::uint32_t>::max()));
    value.cellWidth = static_cast<std::uint16_t>(reader.varint(RichText::maxCellSize));
    value.cellHeight = static_cast<std::uint16_t>(reader.varint(RichText::maxCellSize));
    value.baseline = static_cast<std::uint16_t>(reader.varint(RichText::maxCellSize));
    value.fontSize = static_cast<std::uint16_t>(reader.varint(RichText::maxCellSize));
    value.lineHeight = static_cast<std::uint32_t>(reader.varint((1u << 24)));
    value.lineBaseline = static_cast<std::uint32_t>(reader.varint((1u << 24)));
    value.minWidth = static_cast<std::uint32_t>(reader.varint(maxColumns));
    const auto flags = reader.u8();
    if (!value.rendererRevision || flags > 7 || !value.cellWidth || !value.cellHeight || value.shown > RichText::maxCodePoints)
        reader.fail();
    value.isFitting = flags & 1;
    value.isExact = flags & 2;
    value.isShaped = flags & 4;
    value.lineEnds.resize(reader.count(RichText::maxCodePoints, 1));
    std::uint32_t previous = 0;
    for (auto& end : value.lineEnds) {
        end = static_cast<std::uint32_t>(reader.varint(RichText::maxCodePoints));
        if (end <= previous)
            reader.fail();
        previous = end;
    }
    value.geometry = readGeometry(reader);
    if (!value.geometry.lines.empty() || !value.geometry.spans.empty()) {
        if (!value.isShaped || value.geometry.lines.size() != value.lineEnds.size())
            reader.fail();
        for (std::size_t index = 0; index < value.geometry.lines.size(); ++index) {
            if (index >= value.lineEnds.size() || value.geometry.lines[index].end != value.lineEnds[index])
                reader.fail();
        }
    }
    const auto hasElision = reader.u8();
    if (hasElision > 1)
        reader.fail();
    if (hasElision == 1) {
        value.geometry.elisionEnd = static_cast<std::uint32_t>(reader.varint(RichText::maxCodePoints - 1));
        value.geometry.elision = std::make_shared<RichText::ShapedText>(readGeometry(reader));
        if (value.geometry.lines.empty() || value.geometry.elision->lines.empty()
            || value.geometry.elisionEnd >= value.geometry.lines.back().end
            || value.geometry.elision->lines.back().end != value.geometry.elisionEnd + 1)
            reader.fail();
    }
    return reader.ok();
}

void writeDocument(WireWriter& writer, const RichText::Document& document)
{
    writer.varint(document.text.size());
    for (const std::uint32_t codePoint : document.text)
        writer.varint(codePoint);
    writer.varint(document.runs.size());
    for (const RichText::Run& run : document.runs) {
        writer.varint(run.length);
        writer.u16(run.halfPoints);
        writer.u8(run.font);
        writer.u8(run.color);
        writer.u8(run.background);
        writer.u8(run.styles);
        writer.u8(std::to_underlying(run.alignment));
        writer.u8(std::to_underlying(run.paragraph.lineSpacing));
        writer.u16(run.paragraph.lineSpacingValue);
        writer.u16(run.paragraph.spaceBefore);
        writer.u16(run.paragraph.spaceAfter);
        writer.u16(run.paragraph.leftIndent);
        writer.u16(run.paragraph.rightIndent);
        writer.u16(static_cast<std::uint16_t>(run.paragraph.firstIndent));
    }
    writer.varint(document.fontCount());
    for (const RichText::FontEntry& font : document.fontTable()) {
        writer.u8(std::to_underlying(font.familyClass));
        writer.utf8(font.familyName());
    }
    writer.varint(document.colorCount);
    for (const RichText::ColorEntry& color : document.colorTable()) {
        writer.u8(color.red);
        writer.u8(color.green);
        writer.u8(color.blue);
        writer.u8(color.isAutomatic ? 1 : 0);
    }
}

namespace {

template<typename TextStorage, typename RunStorage, typename FontStorage>
bool readDocumentFields(WireReader& reader, RichText::Document& document, TextStorage&& textStorage, RunStorage&& runStorage, FontStorage&& fontStorage)
{
    document = {};
    const auto text = textStorage(reader.count(RichText::maxCodePoints, 1));
    for (std::uint32_t& codePoint : text)
        codePoint = static_cast<std::uint32_t>(reader.varint(0x10FFFF));
    const auto runs = runStorage(reader.count(RichText::maxRuns, 21));
    std::uint32_t start = 0;
    for (RichText::Run& run : runs) {
        run.start = start;
        run.length = static_cast<std::uint32_t>(reader.varint(RichText::maxCodePoints));
        run.halfPoints = reader.u16();
        run.font = reader.u8();
        run.color = reader.u8();
        run.background = reader.u8();
        run.styles = reader.u8();
        const std::uint8_t alignment = reader.u8();
        if (alignment > std::to_underlying(RichText::Alignment::Right))
            reader.fail();
        run.alignment = static_cast<RichText::Alignment>(alignment);
        const std::uint8_t lineSpacing = reader.u8();
        if (lineSpacing > std::to_underlying(RichText::LineSpacing::Multiple))
            reader.fail();
        run.paragraph.lineSpacing = static_cast<RichText::LineSpacing>(lineSpacing);
        run.paragraph.lineSpacingValue = reader.u16();
        run.paragraph.spaceBefore = reader.u16();
        run.paragraph.spaceAfter = reader.u16();
        run.paragraph.leftIndent = reader.u16();
        run.paragraph.rightIndent = reader.u16();
        run.paragraph.firstIndent = static_cast<std::int16_t>(reader.u16());
        if (!RichText::isWellFormed(run.paragraph))
            reader.fail();
        start += run.length;
    }
    const auto fonts = fontStorage(reader.count(RichText::maxFonts, 2));
    for (RichText::FontEntry& font : fonts) {
        const std::uint8_t familyClass = reader.u8();
        if (familyClass > std::to_underlying(RichText::FamilyClass::Monospace))
            reader.fail();
        font.familyClass = static_cast<RichText::FamilyClass>(familyClass);
        const std::string_view name = reader.utf8(RichText::maxFamilyNameBytes);
        std::ranges::copy(name, font.name.begin());
        font.nameLength = static_cast<std::uint8_t>(name.size());
    }
    document.colorCount = static_cast<std::uint8_t>(reader.count(RichText::maxColors, 4));
    for (std::size_t index = 0; index < document.colorCount; ++index) {
        RichText::ColorEntry& color = document.colors[index];
        color.red = reader.u8();
        color.green = reader.u8();
        color.blue = reader.u8();
        const std::uint8_t isAutomatic = reader.u8();
        if (isAutomatic > 1)
            reader.fail();
        color.isAutomatic = isAutomatic == 1;
    }
    if (reader.ok() && !RichText::isWellFormed(RichText::DocumentView { text, runs, fonts, document.colorTable() }))
        reader.fail();
    return reader.ok();
}

} // namespace

bool readDocument(WireReader& reader, RichText::Document& document)
{
    return tryReadDocument(reader, document).has_value();
}

std::expected<void, ApplyOutcome> tryReadDocument(WireReader& reader, RichText::Document& document)
{
    bool didRefuseAllocation = false;
    const auto decoded = readDocumentFields(reader, document, [&](std::size_t count) -> std::span<std::uint32_t> {
        if (!document.text.tryResize(count)) {
            didRefuseAllocation = true;
            reader.fail();
            return {};
        }
        return document.text; }, [&](std::size_t count) -> std::span<RichText::Run> {
        if (!document.runs.tryResize(count)) {
            didRefuseAllocation = true;
            reader.fail();
            return {};
        }
        return document.runs; }, [&](std::size_t count) -> std::span<RichText::FontEntry> {
        if (!document.fonts.tryResize(count)) {
            didRefuseAllocation = true;
            reader.fail();
            return {};
        }
        return document.fonts; });
    if (didRefuseAllocation)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    if (!decoded)
        return std::unexpected(ApplyOutcome::Malformed);
    return {};
}

std::optional<DocumentWireView> readDocumentView(WireReader& reader)
{
    const auto begin = reader.offset();
    std::array<std::uint32_t, RichText::maxCodePoints> text;
    std::array<RichText::Run, RichText::maxRuns> runs;
    std::array<RichText::FontEntry, RichText::maxFonts> fonts;
    RichText::Document fields;
    DocumentWireView result;
    if (!readDocumentFields(reader, fields, [&](std::size_t count) {
                result.codePoints = count;
                return std::span(text).first(count); }, [&](std::size_t count) {
                result.runs = count;
                return std::span(runs).first(count); }, [&](std::size_t count) {
                result.fonts = count;
                return std::span(fonts).first(count); }))
        return std::nullopt;
    result.bytes = reader.consumed(begin, reader.offset());
    return result;
}

void writeRichTextBlock(WireWriter& writer, const RichText::BlockStore& blocks)
{
    const std::span<const RichText::Block> primary = blocks.blocks(ScreenMode::Primary);
    const std::span<const RichText::Block> alternate = blocks.blocks(ScreenMode::Alternate);
    writer.varint(primary.size() + alternate.size());
    for (const ScreenMode screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const RichText::Block& block : blocks.blocks(screen)) {
            writer.u8(std::to_underlying(screen));
            writer.varint(block.serial);
            writer.u16(block.id);
            writer.u32(block.documentRevision);
            writer.svarint(block.row);
            writer.u16(block.column);
            writer.u16(block.width);
            writer.u16(block.height);
            writer.u8(std::to_underlying(block.overflow));
            writer.u8(std::to_underlying(block.alignment));
            writer.u8(std::to_underlying(block.lineMode));
            writer.u8(std::to_underlying(block.pointerMode));
            writer.u8(std::to_underlying(block.lifetime));
            writer.u16(block.caret);
            writer.u16(block.viewportTop);
            writer.u8(block.padding.top);
            writer.u8(block.padding.right);
            writer.u8(block.padding.bottom);
            writer.u8(block.padding.left);
            writer.u8(std::to_underlying(block.verticalPlacement));
            writer.varint(block.visibleRects.size());
            for (const auto& rectangle : block.visibleRects) {
                writer.u16(rectangle.x);
                writer.u16(rectangle.y);
                writer.u16(rectangle.width);
                writer.u16(rectangle.height);
            }
        }
    }
}

namespace {

template<typename Resolve, typename Place>
std::optional<RichTextPlacementStorage> readPlacements(std::span<const std::uint8_t> bytes, int rows, int columns, Resolve&& resolve, Place&& place)
{
    WireReader reader(bytes);
    const std::size_t count = reader.count(2 * RichText::maxBlocksPerScreen, 17);
    std::array<std::uint32_t, 2 * RichText::maxBlocksPerScreen> serials {};
    std::array<std::array<std::uint16_t, RichText::maxBlocksPerScreen>, 2> ids {};
    RichTextPlacementStorage storage;
    for (std::size_t index = 0; index < count && reader.ok(); ++index) {
        const std::uint8_t screen = reader.u8();
        const auto serial = static_cast<std::uint32_t>(reader.varint(std::numeric_limits<std::uint32_t>::max()));
        RichText::Block block;
        block.serial = serial;
        block.id = reader.u16();
        block.documentRevision = reader.u32();
        const std::int64_t row = reader.svarint();
        block.column = reader.u16();
        block.width = reader.u16();
        block.height = reader.u16();
        const std::uint8_t overflow = reader.u8();
        const std::uint8_t alignment = reader.u8();
        const std::uint8_t lineMode = reader.u8();
        const std::uint8_t pointerMode = reader.u8();
        const std::uint8_t lifetime = reader.u8();
        block.caret = reader.u16();
        block.viewportTop = reader.u16();
        block.padding = { reader.u8(), reader.u8(), reader.u8(), reader.u8() };
        const std::uint8_t verticalPlacement = reader.u8();
        const auto rectangleCount = reader.count(RichText::maxVisibleRects, 8);
        std::array<RichText::VisibleRect, RichText::maxVisibleRects> fixedRectangles;
        for (std::size_t rectangle = 0; rectangle < rectangleCount && reader.ok(); ++rectangle)
            fixedRectangles[rectangle] = { reader.u16(), reader.u16(), reader.u16(), reader.u16() };
        const auto rectangles = std::span(fixedRectangles).first(rectangleCount);
        if (!reader.ok() || !RichText::validVisibleRects(rectangles, block.width, block.height))
            return std::nullopt;
        const bool isPlacementValid = screen <= 1 && serial != 0 && block.width >= 1 && block.height >= 1 && block.column + block.width <= columns
            && row >= (screen ? 0 : deepestRow) && row <= static_cast<std::int64_t>(rows) - static_cast<std::int64_t>(block.height)
            && overflow <= std::to_underlying(RichText::Overflow::Ellipsis)
            && lineMode <= 1 && pointerMode <= 1 && lifetime <= 1 && block.viewportTop <= RichText::maxCodePoints
            && (!pointerMode || (block.id && block.documentRevision))
            && (!block.viewportTop || lineMode == std::to_underlying(RichText::LineMode::Terminal))
            && verticalPlacement <= std::to_underlying(RichText::VerticalPlacement::Bottom)
            && alignment >= std::to_underlying(RichText::Alignment::Left) && alignment <= std::to_underlying(RichText::Alignment::Right);
        if (!reader.ok() || !isPlacementValid || std::ranges::find(std::span(serials).first(index), serial) != std::span(serials).first(index).end()
            || storage.blocks[screen] >= RichText::maxBlocksPerScreen)
            return std::nullopt;
        const auto heldIDs = std::span(ids[screen]).first(storage.blocks[screen]);
        if (block.id && std::ranges::find(heldIDs, block.id) != heldIDs.end())
            return std::nullopt;
        ids[screen][storage.blocks[screen]++] = block.id;
        const auto document = resolve(serial);
        if (!document || (block.caret != 65535 && block.caret > document->codePoints) || block.viewportTop > document->codePoints)
            return std::nullopt;
        if (document->codePoints > static_cast<std::size_t>(RichText::maxTerminalCodePoints) - storage.codePoints
            || document->runs > std::numeric_limits<std::size_t>::max() - storage.runs
            || document->fonts > std::numeric_limits<std::size_t>::max() - storage.fonts)
            return std::nullopt;
        storage.codePoints += document->codePoints;
        storage.runs += document->runs;
        storage.fonts += document->fonts;
        if (rectangleCount > std::numeric_limits<std::size_t>::max() - storage.visibleRectangles)
            return std::nullopt;
        storage.visibleRectangles += rectangleCount;
        const auto retainedBytes = (storage.blocks[0] + storage.blocks[1]) * sizeof(RichText::Block)
            + storage.codePoints * sizeof(std::uint32_t) + storage.runs * sizeof(RichText::Run)
            + storage.fonts * sizeof(RichText::FontEntry)
            + storage.visibleRectangles * sizeof(RichText::VisibleRect);
        if (retainedBytes > RichText::maxRetainedBytes)
            return std::nullopt;
        serials[index] = serial;
        block.row = static_cast<std::int32_t>(row);
        block.overflow = static_cast<RichText::Overflow>(overflow);
        block.alignment = static_cast<RichText::Alignment>(alignment);
        block.lineMode = static_cast<RichText::LineMode>(lineMode);
        block.pointerMode = static_cast<RichText::PointerMode>(pointerMode);
        block.lifetime = static_cast<RichText::Lifetime>(lifetime);
        block.verticalPlacement = static_cast<RichText::VerticalPlacement>(verticalPlacement);
        if (!place(screen ? ScreenMode::Alternate : ScreenMode::Primary, std::move(block), rectangles))
            return std::nullopt;
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::nullopt;
    return storage;
}

} // namespace

bool readRichTextBlock(std::span<const std::uint8_t> bytes, int rows, int columns, std::span<const RichTextDefinition> definitions,
    const RichText::BlockStore& held, RichText::BlockStore& blocks)
{
    return tryReadRichTextBlock(bytes, rows, columns, definitions, held, blocks).has_value();
}

std::expected<void, ApplyOutcome> tryReadRichTextBlock(std::span<const std::uint8_t> bytes, int rows, int columns,
    std::span<const RichTextDefinition> definitions, const RichText::BlockStore& held, RichText::BlockStore& blocks)
{
    const auto resolve = [&](std::uint32_t serial) -> std::optional<DocumentWireView> {
        const auto* document = heldDocument(serial, definitions, held);
        if (!document)
            return std::nullopt;
        return DocumentWireView { {}, document->text.size(), document->runs.size(), document->fonts.size() };
    };
    const auto planned = readPlacements(bytes, rows, columns, resolve,
        [](ScreenMode, const RichText::Block&, std::span<const RichText::VisibleRect>) { return true; });
    if (!planned)
        return std::unexpected(ApplyOutcome::Malformed);
    RichText::BlockStore staged;
    if (!staged.tryReserve(ScreenMode::Primary, planned->blocks[0]) || !staged.tryReserve(ScreenMode::Alternate, planned->blocks[1]))
        return std::unexpected(ApplyOutcome::AllocationRefused);
    bool didRefuseAllocation = false;
    const auto decoded = readPlacements(bytes, rows, columns, resolve,
        [&](ScreenMode screen, RichText::Block block, std::span<const RichText::VisibleRect> rectangles) {
            const auto* document = heldDocument(block.serial, definitions, held);
            assert(document);
            if (!block.visibleRects.tryAssign(rectangles.begin(), rectangles.end()) || !block.document.tryAssign(*document)
                || !staged.tryRestore(screen, std::move(block))) {
                didRefuseAllocation = true;
                return false;
            }
            return true;
        });
    if (didRefuseAllocation)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    if (!decoded)
        return std::unexpected(ApplyOutcome::Malformed);
    blocks = std::move(staged);
    return {};
}

std::optional<RichTextPlacementStorage> richTextPlacementStorage(std::span<const std::uint8_t> bytes,
    int rows, int columns, std::span<const RichTextDefinitionView> definitions)
{
    return readPlacements(bytes, rows, columns, [&](std::uint32_t serial) -> std::optional<DocumentWireView> {
            const auto found = std::ranges::find(definitions, serial, &RichTextDefinitionView::serial);
            if (found == definitions.end())
                return std::nullopt;
            return found->document; }, [](ScreenMode, const RichText::Block&, std::span<const RichText::VisibleRect>) { return true; });
}

std::vector<std::uint32_t> richTextSerials(const RichText::BlockStore& blocks)
{
    std::vector<std::uint32_t> serials;
    for (const ScreenMode screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const RichText::Block& block : blocks.blocks(screen))
            serials.push_back(block.serial);
    }
    std::ranges::sort(serials);
    return serials;
}

} // namespace Ubiquitty::Protocol::Sync
