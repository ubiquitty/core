// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/RichText/Layout.h"

#include "Core/CellGrid.h"
#include "Core/GridBounds.h"
#include "Core/RichText/ParagraphMetrics.h"
#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/Width.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <optional>

namespace Ubiquitty::RichText {

FontCatalogPage queryFontCatalog(const FontCatalogQuery& query, const PublishedTypography& typography)
{
    FontCatalogPage page;
    page.offset = query.offset;
    page.rendererRevision = query.rendererRevision;
    if (!typography.facts)
        return page;
    const auto& facts = *typography.facts;
    page.scope = facts.fontCatalogScope;
    if (facts.fontCatalogScope == FontCatalogScope::Unavailable || !typography.fontCatalog.function)
        return page;
    if (query.rendererRevision != facts.rendererRevision) {
        page.status = FontCatalogStatus::Stale;
        return page;
    }
    FontCatalogPage supplied;
    if (!typography.fontCatalog.function(typography.fontCatalog.context, &query, &facts, &supplied))
        return page;
    if (supplied.status == FontCatalogStatus::Stale) {
        page.status = FontCatalogStatus::Stale;
        return page;
    }
    if (!isFontCatalogPage(query, supplied) || supplied.scope != facts.fontCatalogScope)
        return page;
    return supplied;
}

namespace {

/// A tab stops every this many cells from the line's start.
constexpr std::uint32_t tabStopCells = 8;
/// One point in layout units, which a cell is in the cell model.
constexpr std::uint32_t unitsPerPoint = 4096;
/// A cell in 1/64 point, to layout units.
constexpr std::uint32_t unitsPerCellUnit = unitsPerPoint / 64;

[[nodiscard]] char asciiLower(char character) { return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character; }

[[nodiscard]] bool isSameFamily(std::string_view one, std::string_view other)
{
    return one.size() == other.size() && std::ranges::equal(one, other, [](char a, char b) { return asciiLower(a) == asciiLower(b); });
}

[[nodiscard]] bool isBreakSpace(std::uint32_t codePoint) { return codePoint == ' '; }

/// A face's ascent, descent and nonnegative leading, in table units.
[[nodiscard]] std::uint32_t naturalLineHeight(const FaceMetrics& face)
{
    return std::max<std::uint32_t>(face.lineHeight, static_cast<std::uint32_t>(face.ascent) + face.descent);
}

Document elidedDocument(const Document& source, std::uint32_t end)
{
    Document document = source;
    document.text.resize(end);
    document.text.push_back(0x2026);
    document.runs.erase(std::remove_if(document.runs.begin(), document.runs.end(), [end](const Run& run) { return run.start >= std::max(end, 1u); }), document.runs.end());
    auto& last = document.runs.back();
    last.length = end + 1 - last.start;
    return document;
}

ShapedText geometryOf(const Layout& layout)
{
    ShapedText geometry;
    for (std::size_t index = 0; index < layout.lineCount(); ++index) {
        const auto line = layout.line(index);
        geometry.lines.push_back({ line.first, line.end, line.top, line.height, line.baseline, line.origin, line.width });
    }
    geometry.spans.assign(layout.shapedSpans().begin(), layout.shapedSpans().end());
    return geometry;
}

} // namespace

bool isAdoptable(const TypographyFacts& facts)
{
    if (!facts.rendererRevision || !facts.cellWidth || !facts.cellHeight || facts.cellWidth > maxCellSize || facts.cellHeight > maxCellSize || facts.faceCount > maxFaces
        || facts.baseline > maxCellSize || facts.proportionalSize > maxCellSize || facts.monospaceSize > maxCellSize || facts.shaping > 1 || facts.rendersBlocks > 1
        || facts.fontCatalogScope > FontCatalogScope::System)
        return false;
    constexpr auto styleBits = static_cast<std::uint8_t>(static_cast<std::uint8_t>(StyleBit::Bold) | static_cast<std::uint8_t>(StyleBit::Italic));
    for (const FaceMetrics& face : facts.faceTable()) {
        if (!face.lineHeight || (!face.familyName().empty() && !isFontCatalogFamily(face.familyName())) || (face.styles & ~styleBits)
            || static_cast<std::uint8_t>(face.familyClass) > static_cast<std::uint8_t>(FamilyClass::Monospace))
            return false;
    }
    return true;
}

std::size_t resolveFace(const TypographyFacts& facts, const Document& document, const Run& run)
{
    assert(facts.faceCount > 0);
    constexpr auto styleBits = static_cast<std::uint8_t>(static_cast<std::uint8_t>(StyleBit::Bold) | static_cast<std::uint8_t>(StyleBit::Italic));
    const std::uint8_t styles = run.styles & styleBits;
    const FontEntry* entry = run.font == noFontEntry ? nullptr : &document.fonts[run.font];
    const FamilyClass familyClass = entry ? entry->familyClass : FamilyClass::SansSerif;
    const std::span<const FaceMetrics> faces = facts.faceTable();
    auto find = [&](auto&& matches) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < faces.size(); ++index) {
            if (matches(faces[index]))
                return index;
        }
        return std::nullopt;
    };
    if (entry && entry->nameLength) {
        if (const auto index = find([&](const FaceMetrics& face) { return face.nameLength && face.styles == styles && isSameFamily(face.familyName(), entry->familyName()); }))
            return *index;
    }
    if (const auto index = find([&](const FaceMetrics& face) { return !face.nameLength && face.familyClass == familyClass && face.styles == styles; }))
        return *index;
    if (const auto index = find([&](const FaceMetrics& face) { return !face.nameLength && face.familyClass == FamilyClass::SansSerif && face.styles == styles; }))
        return *index;
    if (const auto index = find([&](const FaceMetrics& face) { return !face.nameLength && face.familyClass == FamilyClass::SansSerif && !face.styles; }))
        return *index;
    return 0;
}

/// Lays one document out: two passes over the text with the same breaking
/// rule, the first counting lines so the second writes into one allocation.
class LayoutBuilder {
public:
    LayoutBuilder(const Document& document, std::uint16_t widthCells, std::uint16_t heightCells, Overflow overflow, Alignment alignment, const TypographyFacts* facts, LineMode lineMode, Insets padding = {})
        : m_document(document)
        , m_overflow(overflow)
        , m_alignment(alignment)
        , m_lineMode(lineMode)
        , m_facts(facts && facts->faceCount ? facts : nullptr)
    {
        m_cellWidth = m_facts ? m_facts->cellWidth * unitsPerCellUnit : unitsPerPoint;
        m_cellHeight = m_facts ? m_facts->cellHeight * unitsPerCellUnit : unitsPerPoint;
        // The content box: the rectangle less its padding, which may leave
        // it no width, where every grapheme takes a line of its own.
        m_blockWidth = static_cast<std::uint32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(widthCells) * m_cellWidth - (padding.left + padding.right) * std::int64_t { unitsPerPoint }));
        m_blockHeight = static_cast<std::uint32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(heightCells) * m_cellHeight - (padding.top + padding.bottom) * std::int64_t { unitsPerPoint }));
        m_tabStop = tabStopCells * m_cellWidth;
        for (std::size_t index = 0; index < document.runs.size(); ++index)
            m_faces[index] = m_facts ? static_cast<std::uint8_t>(resolveFace(*m_facts, document, document.runs[index])) : 0;
    }

    Layout build()
    {
        Layout layout;
        const std::size_t count = m_document.text.size();
        std::size_t lines = 0;
        for (std::uint32_t start = 0; start < count; ++lines)
            start = breakLine(start, nullptr).end;
        layout.m_storage.resize(count + lines * Layout::wordsPerLine);
        layout.m_characterCount = count;
        layout.m_lineCount = lines;
        layout.m_cellWidth = m_cellWidth;
        layout.m_cellHeight = m_cellHeight;
        layout.m_blockWidth = m_blockWidth;
        layout.m_blockHeight = m_blockHeight;

        std::uint32_t top = 0;
        std::uint32_t start = 0;
        bool isShowing = true;
        for (std::size_t index = 0; index < lines; ++index) {
            const Break lineBreak = breakLine(start, &layout.m_storage);
            const LinePlace place = linePlace(m_document.text, start, lineBreak.end);
            const Paragraph paragraph = paragraphOf(start);
            LineLayout line {
                .first = start,
                .end = lineBreak.end,
                .top = top,
                .height = 0,
                .baseline = 0,
                .origin = 0,
                .width = lineBreak.width,
                .ellipsisAdvance = 0,
                .endsWithLineFeed = lineBreak.end > start && isLineBreak(m_document.text[lineBreak.end - 1]),
            };
            std::uint32_t belowBaseline = 0;
            for (std::uint32_t character = start; character < lineBreak.end; ++character) {
                const auto [height, ascent] = lineMetrics(character);
                line.height = std::max(line.height, height);
                line.baseline = std::max(line.baseline, ascent);
                belowBaseline = std::max(belowBaseline, height - std::min(height, ascent));
            }
            // Different faces can supply the tallest ascent and deepest
            // descent. Terminal lines still occupy exactly one grid row.
            if (m_lineMode != LineMode::Terminal)
                line.height = line.baseline + belowBaseline;
            const LineBox box = lineBox(paragraph, line.height, line.baseline, place, m_lineMode);
            line.height = box.height;
            line.baseline = box.baseline;
            // The first line shows even when it is taller than the block.
            if (isShowing && index && top + line.height > m_blockHeight)
                isShowing = false;
            if (isShowing)
                layout.m_visibleLineCount = index + 1;
            layout.m_widestLine = std::max(layout.m_widestLine, line.width);
            if (m_overflow == Overflow::Clip && line.width > lineWidth(paragraph, place, m_blockWidth))
                layout.m_isFitting = false;
            align(line, paragraph, place);
            layout.setLine(index, line);
            top = saturated(top + std::uint64_t { line.height });
            start = lineBreak.end;
        }
        layout.m_totalHeight = top;
        if (layout.m_visibleLineCount < lines)
            layout.m_isFitting = false;
        if (m_overflow == Overflow::Ellipsis && layout.m_visibleLineCount < lines)
            cutForEllipsis(layout);
        layout.m_copyableEnd = layout.m_visibleLineCount ? layout.line(layout.m_visibleLineCount - 1).end : 0;
        for (std::size_t index = 0; index < layout.m_visibleLineCount; ++index) {
            const LineLayout line = layout.line(index);
            const std::uint32_t width = lineWidth(paragraphOf(line.first), linePlace(m_document.text, line.first, line.end), m_blockWidth);
            for (std::uint32_t character = line.first; character < line.end; ++character)
                layout.m_shownCount += layout.rightEdge(character) <= width ? 1 : 0;
        }
        if (layout.m_shownCount < count)
            layout.m_isFitting = false;
        layout.m_isExact = m_isExact;
        styleCapacity(layout);
        return layout;
    }

private:
    struct Break {
        std::uint32_t end = 0;
        std::uint32_t width = 0;
    };

    struct Metrics {
        std::uint32_t height = 0;
        std::uint32_t ascent = 0;
    };

    /// A length held at the largest a layout records, so a line or a stack
    /// of lines longer than that never wraps back into the block.
    [[nodiscard]] static std::uint32_t saturated(std::uint64_t units) { return static_cast<std::uint32_t>(std::min<std::uint64_t>(units, Layout::edgeMask)); }

    /// The properties of the paragraph holding a character: those of its
    /// first character's run.
    [[nodiscard]] Paragraph paragraphOf(std::uint32_t character)
    {
        if (m_document.runs.empty() || m_document.text.empty())
            return {};
        return m_document.runs[runIndex(std::min<std::uint32_t>(paragraphStart(m_document.text, character), static_cast<std::uint32_t>(m_document.text.size() - 1)))].paragraph;
    }

    [[nodiscard]] std::size_t runIndex(std::uint32_t character)
    {
        const auto& runs = m_document.runs;
        if (m_run < runs.size() && runs[m_run].start <= character && character < runs[m_run].start + runs[m_run].length)
            return m_run;
        const auto found = std::ranges::upper_bound(runs, character, {}, &Run::start);
        m_run = static_cast<std::size_t>(found - runs.begin()) - 1;
        return m_run;
    }

    /// The pen's step over a character at `x` from its line's start. A tab
    /// stops on the box's grid, from `indent`, or at `hangingStop` for the
    /// first tab of a hanging first line.
    [[nodiscard]] std::uint32_t advance(std::uint32_t character, std::uint32_t x, bool isClusterContinuation, std::uint32_t indent = 0, std::uint32_t hangingStop = 0) // bool-ok: the grapheme segmenter answers a bool, threaded through
    {
        const std::uint32_t codePoint = m_document.text[character];
        if (isClusterContinuation || isLineBreak(codePoint))
            return 0;
        if (codePoint == '\t') {
            if (hangingStop > indent + x)
                return hangingStop - indent - x;
            return m_tabStop - (indent + x) % m_tabStop;
        }
        const int cells = Unicode::Width::forCodePoint(codePoint, Unicode::Width::AmbiguousWidth::Narrow);
        if (!m_facts) {
            m_isExact = false;
            return static_cast<std::uint32_t>(std::max(cells, 0)) * m_cellWidth;
        }
        const std::size_t run = runIndex(character);
        const FaceMetrics& face = m_facts->faces[m_faces[run]];
        const std::uint32_t halfPoints = m_document.runs[run].halfPoints;
        if (codePoint >= firstTabledCodePoint && codePoint <= lastTabledCodePoint)
            return scaled(face.advances[codePoint - firstTabledCodePoint] * halfPoints, face.familyClass);
        m_isExact = false;
        if (cells <= 0)
            return 0;
        return scaled((cells == 2 ? unitsPerEm : face.averageAdvance) * halfPoints, face.familyClass);
    }

    [[nodiscard]] std::uint32_t scaled(std::uint32_t units, FamilyClass familyClass) const
    {
        if (m_lineMode != LineMode::Terminal || !m_facts || !m_facts->terminalSize(familyClass))
            return units;
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(units) * m_facts->terminalSize(familyClass) + 384) / 768);
    }

    [[nodiscard]] Metrics lineMetrics(std::uint32_t character)
    {
        if (!m_facts)
            return { m_cellHeight, m_cellHeight * 4 / 5 };
        if (m_lineMode == LineMode::Terminal)
            return { m_cellHeight, m_facts->baseline ? m_facts->baseline * unitsPerCellUnit : m_cellHeight * 4 / 5 };
        const std::size_t run = runIndex(character);
        const FaceMetrics& face = m_facts->faces[m_faces[run]];
        const std::uint32_t halfPoints = m_document.runs[run].halfPoints;
        return { naturalLineHeight(face) * halfPoints, face.ascent * halfPoints };
    }

    /// Breaks one line from `start`: at a line feed, or — wrapping — after the
    /// last space that keeps it within the block, or before the grapheme that
    /// would pass the edge when a word is wider than the block. Spaces at a
    /// wrapped line's end hang past the edge.
    ///
    /// Writes each character's edge word when `edges` is not null.
    Break breakLine(std::uint32_t start, std::vector<std::uint32_t>* edges)
    {
        const std::size_t count = m_document.text.size();
        const bool isWrapping = m_overflow != Overflow::Clip;
        const LinePlace place { .startsParagraph = start == 0 || m_document.text[start - 1] == '\n', .endsParagraph = false };
        const Paragraph paragraph = paragraphOf(start);
        const std::uint32_t wrapWidth = lineWidth(paragraph, place, m_blockWidth);
        const std::uint32_t indent = lineIndent(paragraph, place);
        std::uint32_t hangingStop = hangingTabStop(paragraph, place);
        std::uint32_t x = 0;
        std::uint32_t width = 0;
        std::optional<std::uint32_t> afterSpace;
        std::uint32_t widthBeforeSpaces = 0;
        std::uint32_t clusterStart = start;
        Unicode::GraphemeState grapheme {};
        for (std::uint32_t character = start; character < count; ++character) {
            const std::uint32_t codePoint = m_document.text[character];
            bool isContinuation = false;
            if (character == start)
                Unicode::graphemeStateReset(&grapheme, codePoint);
            else
                isContinuation = Unicode::graphemeExtends(&grapheme, m_document.text[character - 1], codePoint);
            if (!isContinuation)
                clusterStart = character;
            if (isLineBreak(codePoint)) {
                if (edges)
                    (*edges)[character] = x;
                return { character + 1, width };
            }
            const std::uint32_t step = advance(character, x, isContinuation, indent, hangingStop);
            if (codePoint == '\t')
                hangingStop = 0;
            if (isWrapping && !isContinuation && !isBreakSpace(codePoint) && character > start && x + step > wrapWidth) {
                if (afterSpace && *afterSpace > start)
                    return { *afterSpace, widthBeforeSpaces };
                return { clusterStart > start ? clusterStart : character, width };
            }
            x = saturated(x + std::uint64_t { step });
            if (edges)
                (*edges)[character] = x | (isContinuation ? Layout::continuationBit : 0);
            if (isBreakSpace(codePoint)) {
                if (!afterSpace || *afterSpace != character)
                    widthBeforeSpaces = width;
                afterSpace = character + 1;
            } else {
                width = x;
            }
        }
        return { static_cast<std::uint32_t>(count), width };
    }

    /// Places a line at its indent, and aligns it in the width its indents
    /// leave.
    void align(LineLayout& line, const Paragraph& paragraph, LinePlace place)
    {
        line.origin = lineIndent(paragraph, place);
        const std::uint32_t width = lineWidth(paragraph, place, m_blockWidth);
        if (line.width >= width || line.first == line.end)
            return;
        Alignment alignment = m_document.runs[runIndex(line.first)].alignment;
        if (alignment == Alignment::Unspecified)
            alignment = m_alignment;
        const std::uint32_t free = width - line.width;
        line.origin += alignment == Alignment::Center ? free / 2 : alignment == Alignment::Right ? free
                                                                                                 : 0;
    }

    /// Cuts the last shown line so it and an ellipsis fit, at a grapheme.
    void cutForEllipsis(Layout& layout)
    {
        const std::size_t index = layout.m_visibleLineCount - 1;
        LineLayout line = layout.line(index);
        std::uint32_t before = line.end;
        while (before > line.first && (isLineBreak(m_document.text[before - 1]) || isBreakSpace(m_document.text[before - 1])))
            --before;
        const Paragraph paragraph = paragraphOf(line.first);
        const LinePlace place = linePlace(m_document.text, line.first, line.end);
        const std::uint32_t width = lineWidth(paragraph, place, m_blockWidth);
        const std::uint32_t styleOf = before > line.first ? before - 1 : line.first;
        // The ellipsis takes the style of the character it follows, measured
        // as one more character of that run.
        std::uint32_t ellipsisAdvance = 0;
        if (m_facts) {
            const std::size_t run = runIndex(styleOf);
            const FaceMetrics& face = m_facts->faces[m_faces[run]];
            ellipsisAdvance = scaled(face.averageAdvance * m_document.runs[run].halfPoints, face.familyClass);
        } else {
            ellipsisAdvance = m_cellWidth;
        }
        std::uint32_t cut = before;
        while (cut > line.first && (layout.rightEdge(cut - 1) + ellipsisAdvance > width || layout.continuesGrapheme(cut)))
            --cut;
        while (cut > line.first && layout.continuesGrapheme(cut))
            --cut;
        line.end = cut;
        line.endsWithLineFeed = false;
        line.width = (cut > line.first ? layout.rightEdge(cut - 1) : 0) + ellipsisAdvance;
        line.ellipsisAdvance = ellipsisAdvance;
        align(line, paragraph, place);
        layout.setLine(index, line);
    }

    /// The characters per line and the lines that fit in the first
    /// character's style; the default style for an empty document.
    void styleCapacity(Layout& layout)
    {
        std::uint32_t advanceUnits = m_cellWidth;
        std::uint32_t lineUnits = m_cellHeight;
        if (m_facts) {
            const Run style = m_document.runs.empty() ? Run {} : m_document.runs.front();
            const FaceMetrics& face = m_facts->faces[m_document.runs.empty() ? resolveFace(*m_facts, m_document, style) : m_faces[0]];
            advanceUnits = scaled(face.averageAdvance * style.halfPoints, face.familyClass);
            lineUnits = m_lineMode == LineMode::Terminal ? m_cellHeight : naturalLineHeight(face) * style.halfPoints;
        }
        // In the first paragraph's spacing, and the width its first line's
        // indents leave.
        const Paragraph paragraph = paragraphOf(0);
        const std::uint32_t width = lineWidth(paragraph, LinePlace {}, m_blockWidth);
        lineUnits = lineBox(paragraph, lineUnits, 0, LinePlace { .startsParagraph = false, .endsParagraph = false }, m_lineMode).height;
        layout.m_charactersPerLine = advanceUnits ? width / advanceUnits : 0;
        layout.m_linesThatFit = lineUnits ? m_blockHeight / lineUnits : 0;
    }

    const Document& m_document;
    Overflow m_overflow;
    Alignment m_alignment;
    LineMode m_lineMode;
    const TypographyFacts* m_facts;
    std::array<std::uint8_t, maxRuns> m_faces {};
    std::size_t m_run = 0;
    std::uint32_t m_cellWidth = 0;
    std::uint32_t m_cellHeight = 0;
    std::uint32_t m_blockWidth = 0;
    std::uint32_t m_blockHeight = 0;
    std::uint32_t m_tabStop = 0;
    bool m_isExact = true;
};

Layout Layout::lay(const Document& document, std::uint16_t widthCells, std::uint16_t heightCells, Overflow overflow, Alignment alignment, const TypographyFacts* facts, LineMode lineMode, const Shaper* shaper, Insets padding, std::uint16_t sourceAnchor)
{
    // Without a table the layout counts in cells, and points have no place in it.
    const Insets effective = facts && facts->faceCount ? padding : Insets {};
    Layout layout = layContent(document, widthCells, heightCells, overflow, alignment, facts, lineMode, shaper, effective, sourceAnchor);
    layout.m_padding = effective;
    layout.m_contentTop = effective.top * unitsPerPoint;
    const std::uint32_t left = effective.left * unitsPerPoint;
    layout.shift(left, layout.m_contentTop);
    // The content box's right and bottom edges, which drawing clips at.
    layout.m_blockWidth += left;
    layout.m_blockHeight += layout.m_contentTop;
    return layout;
}

void Layout::shift(std::uint32_t deltaX, std::uint32_t deltaY)
{
    if (!deltaX && !deltaY)
        return;
    for (std::size_t index = 0; index < m_lineCount; ++index) {
        LineLayout line = this->line(index);
        line.origin += deltaX;
        line.top += deltaY;
        setLine(index, line);
    }
    if (m_isShaped) {
        // A shaped layout's edges are positions in the box, not offsets from
        // a line's origin, so they move with it.
        for (std::size_t index = 0; index < m_characterCount; ++index)
            m_storage[index] = (((m_storage[index] & edgeMask) + deltaX) & edgeMask) | (m_storage[index] & continuationBit);
        for (ShapedSpan& span : m_shapedSpans) {
            span.leading += static_cast<std::int32_t>(deltaX);
            span.trailing += static_cast<std::int32_t>(deltaX);
        }
    }
    if (m_sourceGeometry) {
        const auto moved = [&](const ShapedText& geometry) {
            ShapedText copy = geometry;
            for (ShapedLine& line : copy.lines) {
                line.origin += deltaX;
                line.top += deltaY;
            }
            for (ShapedSpan& span : copy.spans) {
                span.leading += static_cast<std::int32_t>(deltaX);
                span.trailing += static_cast<std::int32_t>(deltaX);
            }
            return copy;
        };
        auto source = std::make_shared<ShapedText>(moved(*m_sourceGeometry));
        if (m_sourceGeometry->elision) {
            source->elision = std::make_shared<ShapedText>(moved(*m_sourceGeometry->elision));
            source->elision->elision.reset();
        }
        m_sourceGeometry = std::move(source);
    }
}

void Layout::placeVertically(VerticalPlacement placement)
{
    const std::uint32_t contentHeight = m_blockHeight - m_contentTop;
    if (placement == VerticalPlacement::Top || m_totalHeight >= contentHeight)
        return;
    const std::uint32_t slack = contentHeight - m_totalHeight;
    shift(0, placement == VerticalPlacement::Center ? slack / 2 : slack);
}

Layout Layout::layContent(const Document& document, std::uint16_t widthCells, std::uint16_t heightCells, Overflow overflow, Alignment alignment, const TypographyFacts* facts, LineMode lineMode, const Shaper* shaper, Insets padding, std::uint16_t sourceAnchor)
{
    // The source anchor names text, never another renderer's wrapped row.
    // Shape the complete source before deriving the local viewport.
    const auto firstVisible = [&](std::size_t count, const auto& lineAt) {
        std::size_t first = 0;
        if (sourceAnchor && lineMode == LineMode::Terminal) {
            while (first + 1 < count && lineAt(first).end <= sourceAnchor)
                ++first;
        }
        return first;
    };
    const auto fallback = [&] {
        std::size_t first = 0;
        if (sourceAnchor) {
            const auto full = LayoutBuilder(document, widthCells, openHeightCells,
                overflow == Overflow::Clip ? Overflow::Clip : Overflow::Wrap, alignment, facts, lineMode, padding)
                                  .build();
            first = firstVisible(full.lineCount(), [&](std::size_t index) { return full.line(index); });
        }
        auto layout = LayoutBuilder(document, widthCells, static_cast<std::uint16_t>(heightCells + first), overflow, alignment, facts, lineMode, padding).build();
        layout.m_firstVisibleLine = std::min(first, layout.m_visibleLineCount);
        layout.m_viewportTop = static_cast<std::uint32_t>(first) * layout.cellHeight();
        return layout;
    };
    ShapedText shaped;
    const ShapeRequest request { widthCells, heightCells, overflow, alignment, lineMode, padding, sourceAnchor };
    if (facts && shaper && shaper->function && shaper->function(shaper->context, &document, &request, facts, &shaped)) {
        // A frontend is a boundary too. Never install incomplete geometry or
        // let a bad range reach pointer arithmetic in the capture path.
        bool valid = shaped.lines.size() <= document.text.size() && shaped.spans.size() <= document.text.size();
        std::uint32_t end = 0;
        std::uint32_t top = 0;
        constexpr std::uint32_t largestPosition = Layout::edgeMask;
        const auto largestX = largestPosition - padding.left * unitsPerPoint;
        const auto largestY = largestPosition - padding.top * unitsPerPoint;
        for (const ShapedLine& line : shaped.lines) {
            valid = valid && line.first == end && line.end > line.first && line.end <= document.text.size()
                && line.top == top && line.height && top < largestY && line.height < largestY - top
                && line.baseline <= line.height && line.origin < largestX && line.width < largestX - line.origin
                && (lineMode != LineMode::Terminal || (line.height == static_cast<std::uint32_t>(facts->cellHeight) * unitsPerCellUnit && line.baseline == static_cast<std::uint32_t>(facts->baseline) * unitsPerCellUnit));
            end = line.end;
            top += line.height;
        }
        valid = valid && end == document.text.size();
        valid = valid && (!shaped.elision || overflow == Overflow::Ellipsis);
        end = 0;
        for (const ShapedSpan& span : shaped.spans) {
            valid = valid && span.first == end && span.end > span.first && span.end <= document.text.size()
                && span.line < shaped.lines.size() && span.leading >= 0 && span.trailing >= 0
                && span.leading < static_cast<std::int32_t>(largestX) && span.trailing < static_cast<std::int32_t>(largestX);
            if (span.line < shaped.lines.size())
                valid = valid && span.first >= shaped.lines[span.line].first && span.end <= shaped.lines[span.line].end;
            end = span.end;
        }
        valid = valid && end == document.text.size();
        if (valid) {
            const auto first = firstVisible(shaped.lines.size(), [&](std::size_t index) { return shaped.lines[index]; });
            const auto contentHeightCells = static_cast<std::uint16_t>(heightCells + first);
            Layout layout;
            layout.m_isShaped = true;
            layout.m_characterCount = document.text.size();
            layout.m_lineCount = shaped.lines.size();
            layout.m_cellWidth = facts->cellWidth * unitsPerCellUnit;
            layout.m_cellHeight = facts->cellHeight * unitsPerCellUnit;
            layout.m_blockWidth = static_cast<std::uint32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(widthCells) * layout.m_cellWidth - (padding.left + padding.right) * std::int64_t { unitsPerPoint }));
            layout.m_blockHeight = static_cast<std::uint32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(contentHeightCells) * layout.m_cellHeight - (padding.top + padding.bottom) * std::int64_t { unitsPerPoint }));
            layout.m_totalHeight = top;
            layout.m_storage.resize(document.text.size() + shaped.lines.size() * wordsPerLine);
            layout.m_shapedSpans = std::move(shaped.spans);
            for (const ShapedSpan& span : layout.m_shapedSpans) {
                for (std::uint32_t index = span.first; index < span.end; ++index)
                    layout.m_storage[index] = static_cast<std::uint32_t>(span.trailing) | (index > span.first ? continuationBit : 0);
            }
            for (std::size_t index = 0; index < shaped.lines.size(); ++index) {
                const ShapedLine& line = shaped.lines[index];
                layout.setLine(index, { line.first, line.end, line.top, line.height, line.baseline, line.origin, line.width, 0, isLineBreak(document.text[line.end - 1]) });
                layout.m_widestLine = std::max(layout.m_widestLine, line.width);
                if (line.top + line.height <= layout.m_blockHeight) {
                    layout.m_visibleLineCount = index + 1;
                    layout.m_copyableEnd = line.end;
                }
            }
            for (const ShapedSpan& span : layout.m_shapedSpans) {
                if (span.line < layout.m_visibleLineCount && static_cast<std::uint32_t>(std::max(span.leading, span.trailing)) <= layout.m_blockWidth)
                    layout.m_shownCount += span.end - span.first;
            }
            layout.m_firstVisibleLine = std::min(first, layout.m_visibleLineCount);
            layout.m_viewportTop = static_cast<std::uint32_t>(first) * layout.cellHeight();
            layout.m_isFitting = layout.m_shownCount == document.text.size();
            layout.m_linesThatFit = shaped.lines.empty() ? 0 : layout.m_blockHeight / shaped.lines.front().height;
            if (overflow == Overflow::Ellipsis && layout.m_visibleLineCount < layout.m_lineCount) {
                // Native measurement, never summed character advances, chooses
                // a fitting prefix. At most log2(maxCodePoints) + 2 shapes;
                // every candidate includes the ellipsis in its shaping context.
                auto source = std::make_shared<ShapedText>(geometryOf(layout));
                source->elision = std::move(shaped.elision);
                source->elisionEnd = shaped.elisionEnd;
                if (!source->elision) {
                    std::vector<std::uint32_t> boundaries { 0 };
                    for (const auto& span : layout.m_shapedSpans) {
                        if (span.end < document.text.size())
                            boundaries.push_back(span.end);
                    }
                    const auto firstBoundary = std::ranges::lower_bound(boundaries, sourceAnchor);
                    // An anchor at the end, or inside the final grapheme, has
                    // no removable suffix. Appending a token to the complete
                    // source would exceed its scalar and geometry envelope.
                    if (firstBoundary == boundaries.end())
                        return fallback();
                    const auto minCut = *firstBoundary;
                    std::size_t low = static_cast<std::size_t>(firstBoundary - boundaries.begin());
                    std::size_t high = boundaries.size();
                    while (low < high) {
                        const auto middle = low + (high - low) / 2;
                        const auto prefix = elidedDocument(document, boundaries[middle]);
                        const auto candidate = layContent(prefix, widthCells, heightCells, Overflow::Wrap, alignment, facts, lineMode, shaper, padding, sourceAnchor);
                        if (candidate.isShaped() && candidate.isFitting()) {
                            source->elisionEnd = boundaries[middle];
                            source->elision = std::make_shared<ShapedText>(geometryOf(candidate));
                            low = middle + 1;
                        } else
                            high = middle;
                    }
                    if (!source->elision) {
                        const auto token = layContent(elidedDocument(document, minCut), widthCells, heightCells, Overflow::Wrap, alignment, facts, lineMode, shaper, padding, sourceAnchor);
                        if (token.isShaped()) {
                            source->elisionEnd = minCut;
                            source->elision = std::make_shared<ShapedText>(geometryOf(token));
                        }
                    }
                }
                if (source->elision && !source->elision->elision && source->elisionEnd < document.text.size()
                    && (!source->elisionEnd || !layout.continuesGrapheme(source->elisionEnd))) {
                    const auto prefix = elidedDocument(document, source->elisionEnd);
                    const Shaper presentation { [](void* context, const Document*, const ShapeRequest*, const TypographyFacts*, ShapedText* output) {
                                                   *output = *static_cast<const ShapedText*>(context);
                                                   return true;
                                               },
                        source->elision.get() };
                    auto visible = layContent(prefix, widthCells, heightCells, Overflow::Wrap, alignment, facts, lineMode, &presentation, padding, sourceAnchor);
                    const auto firstSource = visible.firstVisibleLine() < visible.lineCount() ? visible.line(visible.firstVisibleLine()).first : source->elisionEnd;
                    if (visible.isShaped() && (visible.isFitting() || source->elisionEnd == firstSource)) {
                        visible.m_totalHeight = layout.m_totalHeight;
                        visible.m_widestLine = layout.m_widestLine;
                        visible.m_isFitting = false;
                        visible.m_copyableEnd = source->elisionEnd;
                        visible.m_shownCount = source->elisionEnd;
                        std::erase_if(visible.m_shapedSpans, [&](const ShapedSpan& span) { return span.end > source->elisionEnd; });
                        auto last = visible.line(visible.m_lineCount - 1);
                        last.end = source->elisionEnd;
                        last.endsWithLineFeed = false;
                        visible.setLine(visible.m_lineCount - 1, last);
                        visible.m_sourceGeometry = std::move(source);
                        return visible;
                    }
                }
                // Invalid remote presentation geometry cannot claim an exact
                // native ellipsis. A valid token wider than the box clips.
                return fallback();
            }
            if (shaped.elision)
                return fallback();
            return layout;
        }
    }
    return fallback();
}

LineLayout Layout::line(std::size_t index) const
{
    assert(index < m_lineCount);
    const std::uint32_t* words = m_storage.data() + lineWord(index);
    return { words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7], words[8] != 0 };
}

void Layout::setLine(std::size_t index, const LineLayout& line)
{
    std::uint32_t* words = m_storage.data() + lineWord(index);
    words[0] = line.first;
    words[1] = line.end;
    words[2] = line.top;
    words[3] = line.height;
    words[4] = line.baseline;
    words[5] = line.origin;
    words[6] = line.width;
    words[7] = line.ellipsisAdvance;
    words[8] = line.endsWithLineFeed ? 1 : 0;
}

std::uint32_t Layout::characterAt(std::int64_t x, std::int64_t y) const
{
    if (m_visibleLineCount <= m_firstVisibleLine)
        return 0;
    y += m_viewportTop;
    // The last shown line whose top is at or above the point: a binary search
    // over the lines' tops, which ascend.
    std::size_t low = m_firstVisibleLine;
    std::size_t high = m_visibleLineCount;
    while (high - low > 1) {
        const std::size_t middle = (low + high) / 2;
        if (static_cast<std::int64_t>(line(middle).top) <= y)
            low = middle;
        else
            high = middle;
    }
    const LineLayout found = line(low);
    if (m_isShaped) {
        const ShapedSpan* nearest = nullptr;
        std::int64_t distance = std::numeric_limits<std::int64_t>::max();
        for (const ShapedSpan& span : m_shapedSpans) {
            if (span.line != low)
                continue;
            const auto left = std::min(span.leading, span.trailing);
            const auto right = std::max(span.leading, span.trailing);
            if (x >= left && x < right)
                return span.first;
            const auto away = std::min(std::abs(x - left), std::abs(x - right));
            if (away < distance) {
                nearest = &span;
                distance = away;
            }
        }
        return nearest ? nearest->first : found.first;
    }
    // A point past a line that ends at a line feed stands before it.
    const std::uint32_t end = found.endsWithLineFeed ? found.end - 1 : found.end;
    const std::int64_t along = x - static_cast<std::int64_t>(found.origin);
    if (along < 0)
        return found.first;
    // The first character whose right edge is past the point: an upper bound
    // over the line's prefix-summed edges.
    std::uint32_t first = found.first;
    std::uint32_t last = end;
    while (first < last) {
        const std::uint32_t middle = first + (last - first) / 2;
        if (static_cast<std::int64_t>(rightEdge(middle)) <= along)
            first = middle + 1;
        else
            last = middle;
    }
    std::uint32_t character = first;
    while (character > found.first && character < m_characterCount && continuesGrapheme(character))
        --character;
    return character;
}

std::uint32_t Layout::caretAt(std::int64_t x, std::int64_t y) const
{
    const auto character = characterAt(x, y);
    if (m_isShaped) {
        for (const ShapedSpan& span : m_shapedSpans) {
            if (span.first == character)
                return std::abs(x - span.leading) <= std::abs(x - span.trailing) ? span.first : span.end;
        }
        return character;
    }
    for (std::size_t index = 0; index < visibleLineCount(); ++index) {
        const LineLayout found = line(index);
        if (character >= found.first && character < found.end) {
            const auto middle = (leftEdge(found, character) + rightEdge(character)) / 2 + found.origin;
            return x < middle ? character : graphemeEnd(character);
        }
    }
    return character;
}

std::int32_t Layout::caretX(const LineLayout& line, std::uint32_t character) const
{
    if (m_isShaped) {
        for (const ShapedSpan& span : m_shapedSpans) {
            if (span.first >= line.first && span.end <= line.end) {
                if (span.first == character)
                    return span.leading;
                if (span.end == character && character == line.end)
                    return span.trailing;
            }
        }
    }
    return static_cast<std::int32_t>(line.origin + leftEdge(line, character));
}

namespace {

// A break opportunity follows a space, a tab or a line feed, and falls on
// either side of a wide character, which a line may break around.
[[nodiscard]] bool isWordSeparator(std::uint32_t codePoint) { return codePoint == ' ' || codePoint == '\t' || isLineBreak(codePoint); }
[[nodiscard]] bool isWide(std::uint32_t codePoint) { return Unicode::Width::forCodePoint(codePoint, Unicode::Width::AmbiguousWidth::Narrow) == 2; }

// The widest run of text between two break opportunities, in layout units.
// The layout must not wrap, so no run is split by the block's edge.
std::uint32_t widestWord(const Layout& layout, const Document& document)
{
    std::uint32_t widest = 0;
    const auto measureRuns = [&](auto&& extentOf) {
        std::optional<std::pair<std::int64_t, std::int64_t>> word;
        const auto close = [&] {
            if (word)
                widest = std::max(widest, static_cast<std::uint32_t>(word->second - word->first));
            word.reset();
        };
        std::size_t line = 0;
        for (std::uint32_t character = 0; character < document.text.size(); ++character) {
            const std::uint32_t codePoint = document.text[character];
            while (line + 1 < layout.lineCount() && character >= layout.line(line).end)
                ++line;
            if (layout.continuesGrapheme(character))
                continue;
            if (isWordSeparator(codePoint)) {
                close();
                continue;
            }
            const auto extent = extentOf(line, character);
            if (!extent)
                continue;
            if (isWide(codePoint))
                close();
            word = word ? std::pair { std::min(word->first, extent->first), std::max(word->second, extent->second) } : *extent;
            if (isWide(codePoint))
                close();
        }
        close();
    };
    if (layout.isShaped()) {
        const auto spans = layout.shapedSpans();
        std::size_t index = 0;
        measureRuns([&](std::size_t, std::uint32_t character) -> std::optional<std::pair<std::int64_t, std::int64_t>> {
            while (index < spans.size() && spans[index].end <= character)
                ++index;
            if (index == spans.size() || spans[index].first != character)
                return std::nullopt;
            const ShapedSpan& span = spans[index];
            return std::pair<std::int64_t, std::int64_t> { std::min(span.leading, span.trailing), std::max(span.leading, span.trailing) };
        });
    } else {
        measureRuns([&](std::size_t lineIndex, std::uint32_t character) -> std::optional<std::pair<std::int64_t, std::int64_t>> {
            if (lineIndex >= layout.lineCount())
                return std::nullopt;
            const LineLayout line = layout.line(lineIndex);
            if (character < line.first || character >= line.end)
                return std::nullopt;
            std::uint32_t last = character;
            while (last + 1 < line.end && layout.continuesGrapheme(last + 1))
                ++last;
            return std::pair<std::int64_t, std::int64_t> { layout.leftEdge(line, character), layout.rightEdge(last) };
        });
    }
    return widest;
}

// Whether every line of a layout is a whole paragraph, so no word of it was
// split or cut: its geometry then measures the widest word as it stands.
bool isUnwrapped(const Layout& layout, const Document& document)
{
    if (layout.isElided() || layout.visibleLineCount() < layout.lineCount())
        return false;
    for (std::size_t index = 0; index + 1 < layout.lineCount(); ++index) {
        const LineLayout line = layout.line(index);
        if (line.end == 0 || !isLineBreak(document.text[line.end - 1]))
            return false;
    }
    return true;
}

// Measures the body in the request's rectangle. Answers the widest word too
// when the layout already shows every paragraph whole, which spares the
// caller a second shaping pass.
Measurement measureRectangle(const Document& document, const ShapeRequest& request, const PublishedTypography& typography, std::optional<std::uint32_t>& widestWordCells)
{
    const Layout layout = Layout::lay(document, request.width, request.height, request.overflow, request.alignment,
        typography.facts.get(), request.lineMode, &typography.shaper, request.padding, request.viewportAnchor);
    const std::uint32_t horizontalPadding = (layout.padding().left + layout.padding().right) * unitsPerPoint;
    if (!document.text.empty() && isUnwrapped(layout, document))
        widestWordCells = (widestWord(layout, document) + horizontalPadding + layout.cellWidth() - 1) / layout.cellWidth();
    Measurement result;
    // Every size a reply names is a rectangle's, its padding included.
    const Insets padding = layout.padding();
    result.width = (layout.widestLine() + (padding.left + padding.right) * unitsPerPoint + layout.cellWidth() - 1) / layout.cellWidth();
    result.height = (layout.totalHeight() + (padding.top + padding.bottom) * unitsPerPoint + layout.cellHeight() - 1) / layout.cellHeight();
    result.shown = layout.shownCount();
    result.capacity = layout.charactersPerLine();
    result.lineCapacity = layout.linesThatFit();
    result.epoch = typography.epoch;
    result.rendererRevision = typography.facts ? typography.facts->rendererRevision : 0;
    result.isFitting = layout.isFitting();
    result.isExact = typography.facts && typography.facts->faceCount && layout.isExact();
    result.isShaped = layout.isShaped();
    if (typography.facts) {
        result.cellWidth = typography.facts->cellWidth;
        result.cellHeight = typography.facts->cellHeight;
        result.baseline = typography.facts->baseline;
        result.fontSize = typography.facts->proportionalSize;
    }
    for (std::size_t index = 0; index < layout.lineCount(); ++index) {
        result.lineEnds.push_back(layout.line(index).end);
        if (layout.isShaped()) {
            const auto line = layout.line(index);
            result.geometry.lines.push_back({ line.first, line.end, line.top, line.height, line.baseline, line.origin, line.width });
        }
    }
    if (layout.isShaped())
        result.geometry.spans.assign(layout.shapedSpans().begin(), layout.shapedSpans().end());
    if (const auto* source = layout.sourceGeometry()) {
        result.geometry = *source;
        result.lineEnds.clear();
        for (const auto& line : source->lines)
            result.lineEnds.push_back(line.end);
    }
    if (layout.lineCount()) {
        result.lineHeight = (layout.line(0).height + 32) / 64;
        result.lineBaseline = (layout.line(0).baseline + 32) / 64;
    }
    return result;
}

} // namespace

Measurement measure(const Document& document, const ShapeRequest& request, const PublishedTypography& typography)
{
    // A zero dimension is open: the body is measured unbounded on that axis,
    // and the dimension then resolves to the reply's, so every count in the
    // reply describes the rectangle the program would place.
    ShapeRequest resolved = request;
    std::optional<std::uint32_t> widestWordCells;
    if (!request.width || !request.height) {
        ShapeRequest probe = request;
        probe.width = request.width ? request.width : openWidthCells;
        probe.height = request.height ? request.height : openHeightCells;
        std::optional<std::uint32_t> probedWord;
        const Measurement probed = measureRectangle(document, probe, typography, probedWord);
        if (probedWord)
            widestWordCells = probedWord;
        if (!request.width)
            resolved.width = static_cast<std::uint16_t>(std::clamp<std::uint32_t>(probed.width, 1, openWidthCells));
        if (!request.height)
            resolved.height = static_cast<std::uint16_t>(std::clamp<std::uint32_t>(probed.height, 1, openHeightCells));
    }
    std::optional<std::uint32_t> resolvedWord;
    Measurement result = measureRectangle(document, resolved, typography, resolvedWord);
    if (!widestWordCells)
        widestWordCells = resolvedWord;
    if (!widestWordCells && !document.text.empty()) {
        const Layout unwrapped = Layout::lay(document, openWidthCells, openHeightCells, Overflow::Clip, request.alignment,
            typography.facts.get(), request.lineMode, &typography.shaper, request.padding);
        const std::uint32_t horizontalPadding = (unwrapped.padding().left + unwrapped.padding().right) * unitsPerPoint;
        widestWordCells = (widestWord(unwrapped, document) + horizontalPadding + unwrapped.cellWidth() - 1) / unwrapped.cellWidth();
    }
    result.minWidth = widestWordCells.value_or(0);
    return result;
}

std::uint32_t Layout::graphemeEnd(std::uint32_t index) const
{
    std::uint32_t end = index + 1;
    while (end < m_characterCount && continuesGrapheme(end))
        ++end;
    return std::min<std::uint32_t>(end, static_cast<std::uint32_t>(m_characterCount));
}

bool LayoutCache::adopt(const Block& block, const PublishedTypography& typography, ShapedText geometry)
{
    // Measurement coordinates include placement padding. Native callbacks use
    // content coordinates; remove the translation before validation and layout
    // apply it once again. Do not modify a shared ellipsis presentation.
    const auto left = static_cast<std::int32_t>(block.padding.left * unitsPerPoint);
    const auto top = block.padding.top * unitsPerPoint;
    const auto removePadding = [&](ShapedText& shaped) {
        for (ShapedLine& line : shaped.lines) {
            if (line.origin < static_cast<std::uint32_t>(left) || line.top < top)
                return false;
            line.origin -= static_cast<std::uint32_t>(left);
            line.top -= top;
        }
        for (ShapedSpan& span : shaped.spans) {
            if (span.leading < left || span.trailing < left)
                return false;
            span.leading -= left;
            span.trailing -= left;
        }
        return true;
    };
    if (!removePadding(geometry))
        return false;
    if (geometry.elision) {
        auto presentation = std::make_shared<ShapedText>(*geometry.elision);
        if (!removePadding(*presentation))
            return false;
        geometry.elision = std::move(presentation);
    }
    const Shaper shaper { [](void* context, const Document*, const ShapeRequest*, const TypographyFacts*, ShapedText* output) {
                             *output = std::move(*static_cast<ShapedText*>(context));
                             return true;
                         },
        &geometry };
    Layout layout = Layout::lay(block, typography.facts.get(), &shaper);
    if (!layout.isShaped())
        return false;
    Entry& entry = entryOf(block, typography);
    entry.layout = std::move(layout);
    entry.drawing.reset();
    ++m_buildCount;
    return true;
}

LayoutCache::Entry& LayoutCache::entryOf(const Block& block, const PublishedTypography& typography)
{
    for (Entry& entry : m_entries) {
        if (entry.serial != block.serial)
            continue;
        if (entry.epoch != typography.epoch) {
            entry.layout = Layout::lay(block, typography.facts.get(), &typography.shaper);
            entry.epoch = typography.epoch;
            entry.drawing.reset();
            ++m_buildCount;
        }
        return entry;
    }
    // Room for every block both screens may hold, so a reference one call
    // hands out survives the entries later calls add.
    if (m_entries.capacity() < static_cast<std::size_t>(2 * maxBlocksPerScreen))
        m_entries.reserve(static_cast<std::size_t>(2 * maxBlocksPerScreen));
    ++m_buildCount;
    m_entries.push_back({ .serial = block.serial, .epoch = typography.epoch, .layout = Layout::lay(block, typography.facts.get(), &typography.shaper), .drawing = {} });
    return m_entries.back();
}

const Layout& LayoutCache::layoutOf(const Block& block, const PublishedTypography& typography)
{
    return entryOf(block, typography).layout;
}

void LayoutCache::retainOnly(const BlockStore& blocks)
{
    std::erase_if(m_entries, [&](const Entry& entry) {
        for (const ScreenMode screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
            for (const Block& block : blocks.blocks(screen)) {
                if (block.serial == entry.serial)
                    return false;
            }
        }
        return true;
    });
}

} // namespace Ubiquitty::RichText
