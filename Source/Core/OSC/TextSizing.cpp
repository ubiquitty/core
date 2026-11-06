// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// TextSizing.cpp — the kitty text-sizing protocol (OSC 66): the metadata
// parser, and the emit that turns a parsed size into blocks on the grid.
//
// A refused specification drops the whole code, which is the parser's own
// strictness. Everything past that reaches the screen: a glyph the terminal
// can size becomes a block, and a glyph it cannot — every spec id still named
// by a cell, the block wider than the row, the block would straddle the scroll
// region — emits at its ordinary size, which is what a terminal that never
// learned the code does with the same text. The alternative is dropping
// characters the application believes it printed.

#include "Core/OSC/TextSizing.h"

#include "Core/Rendering/GlyphKey.h"

#include "Core/Diagnostics/DiagLog.h"
#include "Core/OSC/Internal.h"
#include "Core/Rendering/ProportionalRun.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Terminal.h"
#include "Core/TerminalShared.h"
#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/UTF8Decoder.h"
#include "Core/Unicode/Width.h"
#include "Core/VT/Internal.h"
#include "PAL/TerminalClients.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <span>

namespace Ubiquitty::OSC {

namespace TextSizing {

namespace {

/// Parses an ASCII decimal into [0, 255], refusing empty, oversized
/// and non-digit input.
///
/// - Parameters:
///   - digits: The value bytes of one `key=value` pair.
///   - out: The parsed value, written only on success.
/// - Returns: Whether `digits` was a plain decimal in range.
/// Parses up to five decimal digits, at most 65535.
bool parseDecimal(std::string_view digits, unsigned& out)
{
    if (digits.empty() || digits.size() > 5)
        return false;
    unsigned value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > 65535)
        return false;
    out = value;
    return true;
}

bool parseSmallDecimal(std::string_view digits, uint8_t& out)
{
    if (digits.empty() || digits.size() > 3)
        return false;
    unsigned value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > 255)
        return false;
    out = static_cast<uint8_t>(value);
    return true;
}

} // namespace

std::optional<Request> parseRequest(std::string_view metadata)
{
    Request request;
    Ubiquitty::TextSizeSpec& spec = request.spec;
    unsigned seen = 0;
    std::string_view rest = metadata;
    while (!rest.empty()) {
        const std::size_t colon = rest.find(':');
        const std::string_view pair = colon == std::string_view::npos ? rest : rest.substr(0, colon);
        rest = colon == std::string_view::npos ? std::string_view {} : rest.substr(colon + 1);
        // A separator with nothing after it is an empty final pair.
        if (colon != std::string_view::npos && rest.empty())
            return std::nullopt;
        // An empty pair is a doubled, leading or trailing colon.
        if (pair.size() < 3 || pair[1] != '=')
            return std::nullopt;
        const char key = pair[0];
        const std::string_view digits = pair.substr(2);
        // `c` alone reaches past a byte: a claim runs to a row's width.
        uint16_t* wideField = nullptr;
        if (key == 'c') {
            unsigned cells = 0;
            if (!parseDecimal(digits, cells) || cells < 1 || cells > static_cast<unsigned>(maxProportionalRunCells))
                return std::nullopt;
            const unsigned keyBit = 1u << static_cast<unsigned>(key - 'a');
            if (seen & keyBit)
                return std::nullopt;
            seen |= keyBit;
            wideField = &spec.runWidthCells;
            *wideField = static_cast<uint16_t>(cells);
            continue;
        }
        uint8_t value = 0;
        if (!parseSmallDecimal(digits, value))
            return std::nullopt;
        uint8_t* field = nullptr;
        bool* flag = nullptr;
        uint8_t low = 0;
        uint8_t high = 0;
        switch (key) {
        case 's':
            field = &spec.scale;
            low = 1;
            high = 7;
            break;
        case 'w':
            field = &spec.widthCells;
            high = 7;
            break;
        case 'n':
            field = &spec.fractionNumerator;
            high = 15;
            break;
        case 'd':
            field = &spec.fractionDenominator;
            high = 15;
            break;
        case 'v':
            field = &spec.verticalAlign;
            high = 2;
            break;
        case 'h':
            field = &spec.horizontalAlign;
            // 3, justified, is the proportional extension's: a sized block
            // has no word gaps to distribute slack into, and the check
            // after the loop refuses it without `p`.
            high = 3;
            break;
        // The proportional keys (Docs/proportional-text.md).
        case 'p':
            field = &spec.isProportional;
            low = 1;
            high = 1;
            break;
        case 'q':
            flag = &request.isQuery;
            low = 1;
            high = 1;
            break;
        case 'i':
            field = &request.requestID;
            high = 255;
            break;
        default:
            return std::nullopt;
        }
        const unsigned keyBit = 1u << static_cast<unsigned>(key - 'a');
        if (seen & keyBit)
            return std::nullopt;
        seen |= keyBit;
        if (value < low || value > high)
            return std::nullopt;
        if (flag)
            *flag = true;
        else
            *field = value;
    }
    // A numerator with no denominator is a fraction with no denominator. It
    // reads as harmless and is not: accepted, it interns a specification whose
    // fraction says nothing, and that specification travels the wire and
    // reaches every renderer. The protocol's answer to a client that has
    // misunderstood is refusal, not a reading nobody meant.
    if (spec.fractionDenominator == 0 && spec.fractionNumerator != 0)
        return std::nullopt;
    if (spec.fractionDenominator != 0 && spec.fractionDenominator <= spec.fractionNumerator)
        return std::nullopt;
    // Beside `p`, `s` scales the run as it scales a block: `s` rows tall, each
    // of its `c` cells `s` columns wide. The other sizing keys have no defined
    // meaning there, and an undefined combination accepted now is one a later
    // revision has to keep: `w` would be a second width beside `c`, and `n`,
    // `d` and `v` set a glyph smaller inside its block, where a run's cells
    // are what its text is measured and laid out in. The proportional keys
    // without `p` are likewise nothing the sizing protocol defines. A print
    // needs its width; a query has nothing to print, and its answer is in the
    // run's own cells, which `s` scales whole.
    const bool hasUnscalingSizingKey = (seen & ((1u << ('w' - 'a')) | (1u << ('n' - 'a')) | (1u << ('d' - 'a')) | (1u << ('v' - 'a')))) != 0;
    const bool hasProportionalKey = (seen & ((1u << ('c' - 'a')) | (1u << ('q' - 'a')) | (1u << ('i' - 'a')))) != 0;
    if (spec.horizontalAlign == 3 && !spec.isProportional)
        return std::nullopt;
    if (spec.isProportional) {
        if (hasUnscalingSizingKey)
            return std::nullopt;
        if (!request.isQuery && spec.runWidthCells == 0)
            return std::nullopt;
        if (request.isQuery)
            spec.runWidthCells = 0;
    } else if (hasProportionalKey) {
        return std::nullopt;
    }
    return request;
}

std::optional<Ubiquitty::TextSizeSpec> parseMetadata(std::string_view metadata)
{
    const std::optional<Request> request = parseRequest(metadata);
    if (!request)
        return std::nullopt;
    return request->spec;
}

} // namespace TextSizing

namespace {

/// The width a run takes in the grid's own face: the code points' cell
/// widths, which is what a terminal without a proportional face draws and
/// what Core answers when no frontend measures.
int gridFaceCells(std::span<const std::uint32_t> codePoints, Unicode::Width::AmbiguousWidth ambiguous)
{
    int cells = 0;
    for (const std::uint32_t codePoint : codePoints) {
        const int width = Unicode::Width::forCodePoint(codePoint, ambiguous);
        cells += width > 0 ? width : 0;
    }
    return cells;
}

/// A proportional request (Docs/proportional-text.md): measure and reply,
/// or place the run at the width it carries.
void handleProportionalRun(Ubiquitty::Terminal& t, const TextSizing::Request& request, std::string_view text)
{
    // Sized to the payload bound on the heap: a code point takes at least one
    // byte, so the payload never decodes to more, and a run rarely fills it.
    std::vector<std::uint32_t> codePoints(TextSizing::maxTextBytes);
    static_assert(static_cast<std::size_t>(Ubiquitty::runTextMaximumCodePoints) == TextSizing::maxTextBytes,
        "the run-text bound is the OSC payload bound");
    std::size_t count = 0;
    Unicode::UTF8Decoder decoder;
    Unicode::utf8Reset(&decoder);
    for (const char raw : text) {
        std::uint32_t codePoint = 0;
        const Unicode::UTF8Status status = Unicode::utf8Feed(&decoder, static_cast<std::uint8_t>(raw), &codePoint);
        if (status == Unicode::UTF8Status::Need)
            continue;
        if (status == Unicode::UTF8Status::Invalid)
            codePoint = 0xFFFD;
        // Past the payload bound the code refuses whole: a run truncated to it
        // would print text the application never asked for at that width.
        if (count == codePoints.size())
            return;
        codePoints[count++] = codePoint;
    }
    if (count == 0)
        return;
    const std::span<const std::uint32_t> run { codePoints.data(), count };

    if (request.isQuery) {
        // A measurement reply advertises proportional-run support.
        if (!t.drawsProportionalText)
            return;
        if (Ubiquitty::replyDisposition(t, Ubiquitty::Capability::TextMeasureRead) != Ubiquitty::ReplyDisposition::Answer)
            return;
        const Ubiquitty::ProportionalRunStyle style = TextSizing::runStyleOf(t.screens.active->pen);
        const std::string_view terminator = Ubiquitty::oscReplyTerminator(t);
        Ubiquitty::ProportionalQueryDisposition disposition = Ubiquitty::ProportionalQueryDisposition::Answer;
        if (Ubiquitty::TextMeasureClient* measurer = t.textMeasureClient.get())
            disposition = measurer->proportionalQuery(t, run, style, request.requestID, terminator);
        if (disposition == Ubiquitty::ProportionalQueryDisposition::Defer)
            return;
        // The frontend's face when it has one, the grid's face when it does
        // not; either way at least one cell and never wider than a run can be.
        const int cells = disposition == Ubiquitty::ProportionalQueryDisposition::Grid
            ? std::clamp(gridFaceCells(run, t.ambiguousWidth), 1, TextSizing::maxProportionalRunCells)
            : TextSizing::proportionalRunCells(t, run, style);
        std::array<char, 48> reply {};
        const int length = std::snprintf(reply.data(), reply.size(), "\x1b]66;i=%u:c=%d%.*s", static_cast<unsigned>(request.requestID), cells, static_cast<int>(terminator.size()), terminator.data());
        if (length > 0)
            Ubiquitty::respond(t, Ubiquitty::Capability::TextMeasureRead, std::span<const char>(reply.data(), static_cast<std::size_t>(length)));
        return;
    }

    // Unsupported runs advance by their text's grid width.
    if (!t.drawsProportionalText) {
        for (const std::uint32_t codePoint : run)
            VT::emitGraphicCodePoint(t, codePoint);
        return;
    }
    // A scaled run is drawn in bands, which a renderer that crops no sized
    // bands cannot draw: there it is the run at its own size, as a terminal
    // that never learned `s` reads the code.
    Ubiquitty::TextSizeSpec spec = request.spec;
    if (!t.drawsSizedText)
        spec.scale = 1;
    // A claim no row can hold is ordinary text, which wraps where the row ends.
    const std::optional<Screen::ProportionalRunPlacement> placement = Screen::fitProportionalRun(t, spec);
    if (!placement) {
        for (const std::uint32_t codePoint : run)
            VT::emitGraphicCodePoint(t, codePoint);
        return;
    }
    const unsigned specID = Screen::internTextSizeSpec(t, spec);
    if (specID == 0)
        Diagnostics::diagLog(t, "text-size-specs-exhausted", Ubiquitty::textSizeTableCapacity);
    if (specID != 0 && Screen::placeProportionalRun(t, run, spec, specID))
        return;
    // A run the spec table or the run-text store has no room for still takes
    // the cells the program laid out: its text in the grid face, inside them.
    Screen::setProportionalRunInGridFace(t, run, spec, *placement);
}

/// Shapes and lays out a run through the frontend's measurer.
///
/// - Parameters:
///   - t: The terminal whose measurer answers.
///   - codePoints: The run's text.
///   - style: The face and weight the run is set in.
///   - widthCells: The cells the run claims, or zero to lay out unaligned.
///   - horizontalAlign: The run's alignment.
///   - shaped: Scratch for the shaper's answer.
///   - placements: Receives the layout's glyphs.
///   - cellAdvanceOut: Receives one cell's advance in the layout's pixels.
/// - Returns: The layout; `glyphCount` is zero when nothing shaped.
Rendering::ProportionalRunLayout layoutThroughMeasurer(const Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints,
    Ubiquitty::ProportionalRunStyle style, int widthCells, std::uint8_t horizontalAlign, std::span<Ubiquitty::ProportionalShapedGlyph> shaped,
    std::span<Rendering::ProportionalGlyphPlacement> placements, double* cellAdvanceOut)
{
    Rendering::ProportionalRunLayout layout;
    Ubiquitty::TextMeasureClient* measurer = t.textMeasureClient.get();
    if (!measurer)
        return layout;
    const int shapedCount = measurer->shapeProportionalRun(codePoints, style, shaped);
    const double cellAdvance = static_cast<double>(measurer->proportionalCellAdvanceInPixels());
    if (shapedCount <= 0 || cellAdvance <= 0.0)
        return layout;
    if (cellAdvanceOut)
        *cellAdvanceOut = cellAdvance;
    // An unaligned layout answers the width: one cell claimed, left-set.
    return Rendering::layoutProportionalRun(shaped.first(static_cast<std::size_t>(shapedCount)), codePoints,
        widthCells > 0 ? widthCells : 1, widthCells > 0 ? horizontalAlign : static_cast<std::uint8_t>(0), cellAdvance, placements);
}

} // namespace

Ubiquitty::ProportionalRunStyle TextSizing::runStyleOf(const Ubiquitty::Pen& pen)
{
    const bool isBold = pen.attributes.contains(Ubiquitty::VT::Attribute::Bold);
    const bool isItalic = pen.attributes.contains(Ubiquitty::VT::Attribute::Italic) || penFontStyle(&pen) == Ubiquitty::VT::fontFraktur;
    return Ubiquitty::ProportionalRunStyle {
        .faceBits = static_cast<std::uint8_t>(Rendering::fontFace(isBold, isItalic)),
        .weight = static_cast<std::uint32_t>(pen.extendedAttributes.weight()),
    };
}

int TextSizing::proportionalRunCells(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle style)
{
    if (Ubiquitty::TextMeasureClient* measurer = t.textMeasureClient.get()) {
        if (const int answered = measurer->proportionalRunCells(codePoints, style); answered > 0)
            return std::clamp(answered, 1, TextSizing::maxProportionalRunCells);
    }
    int cells = 0;
    std::vector<Ubiquitty::ProportionalShapedGlyph> shaped(2 * codePoints.size() + 1);
    std::vector<Rendering::ProportionalGlyphPlacement> placements(2 * codePoints.size() + 1);
    double cellAdvance = 0.0;
    const Rendering::ProportionalRunLayout layout = layoutThroughMeasurer(t, codePoints, style, 0, 0, shaped, placements, &cellAdvance);
    if (layout.glyphCount > 0 && cellAdvance > 0.0)
        cells = static_cast<int>(std::ceil(layout.widthInPixels / cellAdvance - 1e-6));
    if (cells <= 0)
        cells = gridFaceCells(codePoints, t.ambiguousWidth);
    return std::clamp(cells, 1, TextSizing::maxProportionalRunCells);
}

bool TextSizing::mayOfferFaceFacts(const Ubiquitty::Terminal& terminal)
{
    return terminal.drawsProportionalText;
}

bool TextSizing::isAdoptable(const ProportionalFaceFacts& facts)
{
    if (facts.cellAdvance < minFacedCellAdvance || facts.cellAdvance > maxFacedCellAdvance)
        return false;
    const std::uint64_t widest = static_cast<std::uint64_t>(facts.cellAdvance) * maxFacedAdvanceCells;
    const auto isBounded = [widest](std::uint16_t advance) { return static_cast<std::uint64_t>(advance) <= widest; };
    if (!std::ranges::all_of(facts.untabledAdvances, [&](std::uint16_t advance) { return advance > 0 && isBounded(advance); }))
        return false;
    return std::ranges::all_of(facts.advances, [&](const auto& face) { return std::ranges::all_of(face, isBounded); })
        && std::ranges::all_of(facts.weightedAdvances, [&](const auto& face) {
               return std::ranges::all_of(face, [](std::uint8_t advance) { return advance <= maxFacedAdvanceCells * weightedAdvanceFractionsPerCell; });
           });
}

bool TextSizing::measureProportionalFaces(Ubiquitty::TextMeasureClient& measurer, ProportionalFaceFacts& facts)
{
    facts = {};
    // One code point shapes to a glyph, or to a few where the face
    // decomposes it; eight hold what any face gives one.
    std::array<Ubiquitty::ProportionalShapedGlyph, 8> glyphs {};
    // An advance in 1/64 pixel, rounded up. A glyph over 1,024 pixels wide
    // does not fit a u16, and its face publishes nothing: the host keeps the
    // faces it had.
    const auto inSixtyFourths = [](double pixels, std::uint32_t& out) {
        const double units = std::ceil(std::max(pixels, 0.0) * 64.0 - 1e-9);
        if (!(units <= 65535.0))
            return false;
        out = static_cast<std::uint32_t>(units);
        return true;
    };
    // A measurer with nothing to shape in answers no letter.
    const std::array<std::uint32_t, 1> probe { 'a' };
    if (measurer.shapeProportionalRun(probe, {}, glyphs) <= 0)
        return false;
    const double cellUnits = std::floor(static_cast<double>(measurer.proportionalCellAdvanceInPixels()) * 64.0 + 1e-9);
    if (!(cellUnits >= static_cast<double>(minFacedCellAdvance)) || cellUnits > static_cast<double>(maxFacedCellAdvance))
        return false;
    facts.cellAdvance = static_cast<std::uint32_t>(cellUnits);
    for (std::size_t face = 0; face < runFaceCount; ++face) {
        std::uint64_t letterUnits = 0;
        unsigned letters = 0;
        for (std::uint32_t codePoint = firstFacedCodePoint; codePoint <= lastFacedCodePoint; ++codePoint) {
            const auto tableIndex = facedCodePointIndex(codePoint);
            if (!tableIndex)
                continue;
            const std::array<std::uint32_t, 1> text { codePoint };
            for (std::uint32_t weight = 0; weight <= 7; ++weight) {
                const Ubiquitty::ProportionalRunStyle style { .faceBits = static_cast<std::uint8_t>(face), .weight = weight };
                const int count = measurer.shapeProportionalRun(text, style, glyphs);
                double advance = 0.0;
                for (int index = 0; index < std::min(count, static_cast<int>(glyphs.size())); ++index)
                    advance += static_cast<double>(glyphs[static_cast<std::size_t>(index)].advanceInPixels);
                std::uint32_t units = 0;
                if (!inSixtyFourths(advance, units))
                    return false;
                if (weight == 0) {
                    facts.advances[face][*tableIndex] = static_cast<std::uint16_t>(units);
                    if ((codePoint >= 'A' && codePoint <= 'Z') || (codePoint >= 'a' && codePoint <= 'z')) {
                        letterUnits += units;
                        ++letters;
                    }
                } else {
                    const std::uint64_t fractions = (static_cast<std::uint64_t>(units) * weightedAdvanceFractionsPerCell + facts.cellAdvance - 1) / facts.cellAdvance;
                    if (fractions > maxFacedAdvanceCells * weightedAdvanceFractionsPerCell)
                        return false;
                    auto& tabled = facts.weightedAdvances[face][*tableIndex];
                    tabled = std::max(tabled, static_cast<std::uint8_t>(fractions));
                }
            }
        }
        facts.untabledAdvances[face] = static_cast<std::uint16_t>(std::max<std::uint64_t>((letterUnits + letters - 1) / letters, 1));
    }
    return isAdoptable(facts);
}

int TextSizing::tabledRunCells(const ProportionalFaceFacts& facts, std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle style,
    Unicode::Width::AmbiguousWidth ambiguous)
{
    if (facts.cellAdvance == 0)
        return 1;
    const std::size_t face = style.faceBits & (runFaceCount - 1);
    if (style.weight > 7)
        return maxProportionalRunCells;
    std::uint64_t units = 0;
    std::uint64_t weightedFractions = 0;
    std::uint64_t widestTabledAdvance = 0;
    bool didFindWidestTabledAdvance = false;
    for (const std::uint32_t codePoint : codePoints) {
        if (const auto tableIndex = facedCodePointIndex(codePoint)) {
            if (style.weight == 0)
                units += facts.advances[face][*tableIndex];
            else
                weightedFractions += facts.weightedAdvances[face][*tableIndex];
            continue;
        }
        if (!didFindWidestTabledAdvance) {
            widestTabledAdvance = style.weight == 0 ? std::ranges::max(facts.advances[face]) : std::ranges::max(facts.weightedAdvances[face]);
            didFindWidestTabledAdvance = true;
        }
        const bool isWide = Unicode::Width::forCodePoint(codePoint, ambiguous) == 2;
        if (style.weight == 0)
            units += std::max(widestTabledAdvance, isWide ? static_cast<std::uint64_t>(facts.cellAdvance) * 2 : 0);
        else
            weightedFractions += std::max(widestTabledAdvance, isWide ? static_cast<std::uint64_t>(weightedAdvanceFractionsPerCell) * 2 : 0);
    }
    const std::uint64_t cells = style.weight == 0
        ? (units + facts.cellAdvance - 1) / facts.cellAdvance
        : (weightedFractions + weightedAdvanceFractionsPerCell - 1) / weightedAdvanceFractionsPerCell;
    return static_cast<int>(std::clamp<std::uint64_t>(cells, 1, static_cast<std::uint64_t>(maxProportionalRunCells)));
}

bool TextSizing::proportionalRunCellOwners(const Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints, int widthCells,
    std::uint8_t horizontalAlign, Ubiquitty::ProportionalRunStyle style, std::span<std::uint16_t> cellOfCodePoint, int* textCellsOut)
{
    if (widthCells < 1 || cellOfCodePoint.size() < codePoints.size())
        return false;
    std::vector<Ubiquitty::ProportionalShapedGlyph> shaped(2 * codePoints.size() + 1);
    std::vector<Rendering::ProportionalGlyphPlacement> placements(2 * codePoints.size() + 1);
    double cellAdvance = 0.0;
    const Rendering::ProportionalRunLayout layout = layoutThroughMeasurer(t, codePoints, style, widthCells, horizontalAlign, shaped, placements, &cellAdvance);
    if (layout.glyphCount <= 0 || cellAdvance <= 0.0)
        return false;
    Rendering::proportionalRunCellOwners(std::span<const Rendering::ProportionalGlyphPlacement>(placements.data(), static_cast<std::size_t>(layout.glyphCount)),
        layout, static_cast<int>(codePoints.size()), widthCells, cellAdvance, cellOfCodePoint);
    if (textCellsOut)
        *textCellsOut = Rendering::proportionalRunTextCells(layout, widthCells, cellAdvance);
    return true;
}

int TextSizing::proportionalRunCaretCodePoint(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints, int widthCells,
    std::uint8_t horizontalAlign, Ubiquitty::ProportionalRunStyle style, double xInCells)
{
    const int count = static_cast<int>(codePoints.size());
    if (widthCells < 1 || count <= 0)
        return -1;
    std::vector<Ubiquitty::ProportionalShapedGlyph> shaped(2 * codePoints.size() + 1);
    std::vector<Rendering::ProportionalGlyphPlacement> placements(2 * codePoints.size() + 1);
    double cellAdvance = 0.0;
    const Rendering::ProportionalRunLayout layout
        = layoutThroughMeasurer(t, codePoints, style, widthCells, horizontalAlign, shaped, placements, &cellAdvance);
    if (layout.glyphCount <= 0 || cellAdvance <= 0.0)
        return -1;
    const Rendering::ProportionalRunCaret caret = Rendering::proportionalRunCaretAt(
        std::span<const Rendering::ProportionalGlyphPlacement>(placements.data(), static_cast<std::size_t>(layout.glyphCount)),
        layout, count, xInCells * cellAdvance);
    int index = caret.codePointIndex;
    if (index <= 0 || index >= count)
        return std::clamp(index, 0, count);
    // The run's grapheme boundaries, walked once. A caret between a base and
    // its mark, or inside an emoji sequence, moves off it in the direction the
    // pixel rounded, so the letter a drag takes is a letter a reader sees.
    Unicode::GraphemeState state {};
    Unicode::graphemeStateReset(&state, codePoints[0]);
    std::vector<bool> isBoundary(codePoints.size() + 1, false);
    isBoundary.front() = true;
    isBoundary.back() = true;
    for (std::size_t at = 1; at < codePoints.size(); ++at) {
        if (!Unicode::graphemeExtends(&state, codePoints[at - 1], codePoints[at])) {
            isBoundary[at] = true;
            Unicode::graphemeStateReset(&state, codePoints[at]);
        }
    }
    if (caret.didRoundForward) {
        while (index < count && !isBoundary[static_cast<std::size_t>(index)])
            ++index;
    } else {
        while (index > 0 && !isBoundary[static_cast<std::size_t>(index)])
            --index;
    }
    return index;
}

bool TextSizing::proportionalRunLetterUnder(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints, int widthCells,
    std::uint8_t horizontalAlign, Ubiquitty::ProportionalRunStyle style, double xInCells, int& first, int& end)
{
    const int count = static_cast<int>(codePoints.size());
    const int under = proportionalRunCodePointUnder(t, codePoints, widthCells, horizontalAlign, style, xInCells);
    if (under < 0 || under >= count)
        return false;
    // The run's grapheme boundaries, walked once; the letter is the cluster
    // the code point sits in.
    Unicode::GraphemeState state {};
    Unicode::graphemeStateReset(&state, codePoints[0]);
    first = 0;
    end = count;
    for (int at = 1; at < count; ++at) {
        if (Unicode::graphemeExtends(&state, codePoints[static_cast<std::size_t>(at) - 1], codePoints[static_cast<std::size_t>(at)]))
            continue;
        Unicode::graphemeStateReset(&state, codePoints[static_cast<std::size_t>(at)]);
        if (at <= under) {
            first = at;
        } else {
            end = at;
            break;
        }
    }
    return true;
}

int TextSizing::proportionalRunCodePointUnder(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints, int widthCells,
    std::uint8_t horizontalAlign, Ubiquitty::ProportionalRunStyle style, double xInCells)
{
    const int count = static_cast<int>(codePoints.size());
    if (widthCells < 1 || count <= 0)
        return -1;
    std::vector<Ubiquitty::ProportionalShapedGlyph> shaped(2 * codePoints.size() + 1);
    std::vector<Rendering::ProportionalGlyphPlacement> placements(2 * codePoints.size() + 1);
    double cellAdvance = 0.0;
    const Rendering::ProportionalRunLayout layout
        = layoutThroughMeasurer(t, codePoints, style, widthCells, horizontalAlign, shaped, placements, &cellAdvance);
    if (layout.glyphCount <= 0 || cellAdvance <= 0.0)
        return -1;
    return Rendering::proportionalRunCodePointAt(
        std::span<const Rendering::ProportionalGlyphPlacement>(placements.data(), static_cast<std::size_t>(layout.glyphCount)),
        layout, count, xInCells * cellAdvance);
}

void handleOSC66(Ubiquitty::Terminal& t, std::string_view payload)
{
    // `metadata ; text` — the first semicolon splits; later ones are text.
    const std::size_t split = payload.find(';');
    if (split == std::string_view::npos)
        return;
    const std::string_view text = payload.substr(split + 1);
    if (text.size() > TextSizing::maxTextBytes)
        return;
    const std::optional<TextSizing::Request> request = TextSizing::parseRequest(payload.substr(0, split));
    if (!request)
        return;
    if (request->spec.isProportional) {
        handleProportionalRun(t, *request, text);
        return;
    }

    std::vector<std::uint32_t> codePoints;
    codePoints.reserve(text.size());
    Unicode::UTF8Decoder decoder;
    Unicode::utf8Reset(&decoder);
    for (const char raw : text) {
        std::uint32_t codePoint = 0;
        const auto status = Unicode::utf8Feed(&decoder, static_cast<std::uint8_t>(raw), &codePoint);
        if (status != Unicode::UTF8Status::Need)
            codePoints.push_back(status == Unicode::UTF8Status::Invalid ? 0xFFFD : codePoint);
    }
    if (codePoints.empty())
        return;
    const auto plain = [&](std::span<const std::uint32_t> cluster) {
        for (const auto codePoint : cluster)
            VT::emitGraphicCodePoint(t, codePoint);
    };
    const auto& requested = request->spec;
    if (!t.drawsSizedText || (requested.scale == 1 && requested.widthCells == 0 && requested.fractionDenominator == 0)) {
        plain(codePoints);
        return;
    }
    const auto place = [&](std::span<const std::uint32_t> cluster) {
        auto spec = requested;
        if (spec.widthCells == 0) {
            int width = Unicode::Width::forCodePoint(cluster.front(), t.ambiguousWidth);
            if (width <= 0) {
                plain(cluster);
                return;
            }
            for (const auto codePoint : cluster) {
                if ((Unicode::isExtendedPictographic(cluster.front()) && codePoint == Unicode::Width::variationSelector16)
                    || (Unicode::isKeycapBase(cluster.front()) && codePoint == 0x20E3))
                    width = 2;
            }
            spec.widthCells = static_cast<std::uint8_t>(width);
        }
        if (spec.scale > t.rows || spec.scale * spec.widthCells > t.columns)
            return;
        const unsigned specID = Screen::internTextSizeSpec(t, spec);
        if (specID == 0) {
            Diagnostics::diagLog(t, "text-size-specs-exhausted", textSizeTableCapacity);
            plain(cluster);
        } else if (t.statusLineActive) {
            const Pen savedPen = t.screens.active->pen;
            t.screens.active->pen.extendedAttributes.setTextSizeSpecID(specID);
            penSetTextBand(&t.screens.active->pen, 0);
            plain(cluster);
            t.screens.active->pen = savedPen;
        } else if (!Screen::placeSizedCluster(t, cluster, spec, specID))
            plain(cluster);
    };
    Unicode::GraphemeState state {};
    Unicode::graphemeStateReset(&state, codePoints.front());
    std::size_t first = 0;
    for (std::size_t at = 1; at < codePoints.size(); ++at) {
        if (Unicode::graphemeExtends(&state, codePoints[at - 1], codePoints[at]))
            continue;
        if (requested.widthCells != 0) {
            auto block = *request;
            block.spec.isTextRun = 1;
            if (block.spec.scale > t.rows || block.spec.scale * block.spec.widthCells > t.columns)
                return;
            if (t.statusLineActive) {
                plain(codePoints);
                return;
            }
            const unsigned specID = Screen::internTextSizeSpec(t, block.spec);
            if (!specID || !Screen::placeSizedTextRun(t, codePoints, block.spec, specID))
                plain(codePoints);
            return;
        }
        place(std::span(codePoints).subspan(first, at - first));
        first = at;
        Unicode::graphemeStateReset(&state, codePoints[at]);
    }
    place(std::span(codePoints).subspan(first));
}

} // namespace Ubiquitty::OSC
