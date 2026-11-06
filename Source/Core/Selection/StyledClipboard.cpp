// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// See StyledClipboard.h. The walk mirrors SelectionText::appendSelectionText
// (same rowSpan clip, trailing-blank trim, soft-wrap join) but groups consecutive
// same-style cells into runs, serialized into an RTF document and HTML fragment.
// RTF: colors collected into a \colortbl, runs emit \cfN / \cbN\highlightN plus
// \b / \i / \ul. HTML: each run a <span style="…">. Rows join with \line (RTF) /
// newline (HTML). Runs hold code points so the framers escape per scalar.

#include "Core/Selection/StyledClipboard.h"

#include "Core/RichText/Document.h"

#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <array>
#include <cstdio>
#include <span>
#include <vector>

namespace Ubiquitty::Selection {

namespace {

bool sameRGB(std::span<const std::uint8_t, 3> a, std::span<const std::uint8_t, 3> b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

// Map a StyledCell + color mode + theme paper to the surviving style. Full mode
// drops a background equal to the theme paper; NoBackground drops all
// backgrounds; StylesOnly drops both colors.
SurvivingStyle surviving(const StyledCell& cell, StyleColorMode mode, std::span<const std::uint8_t, 3> defaultBackground)
{
    SurvivingStyle s;
    s.isBold = cell.bold;
    s.isItalic = cell.italic;
    s.isUnderlined = cell.underline;
    if (mode == StyleColorMode::StylesOnly)
        return s;
    s.hasForeground = true;
    s.foreground[0] = cell.foreground[0];
    s.foreground[1] = cell.foreground[1];
    s.foreground[2] = cell.foreground[2];
    if (mode == StyleColorMode::Full && !sameRGB(cell.background, defaultBackground)) {
        s.hasBackground = true;
        s.background[0] = cell.background[0];
        s.background[1] = cell.background[1];
        s.background[2] = cell.background[2];
    }
    return s;
}

// One emitted run: a style plus its accumulated code points. A bare line break
// is a run whose isBreak is set (its text is empty).
// A run's code points live in one buffer shared by every run, named here by
// offset and length. A string per run allocates once per style change — three
// times a line on coloured output — and the run vector then moves every one of
// those strings each time it grows.
// --- RTF helpers ---------------------------------------------------------

// RTF color table: index 0 is the implicit "auto" color; user colors start at 1.
struct ColorTable {
    std::vector<std::array<std::uint8_t, 3>> colors;

    int indexFor(const std::array<std::uint8_t, 3>& rgb)
    {
        for (std::size_t i = 0; i < colors.size(); ++i) {
            if (colors[i][0] == rgb[0] && colors[i][1] == rgb[1] && colors[i][2] == rgb[2])
                return static_cast<int>(i) + 1; // 1-based; 0 is \cf0 auto
        }
        colors.push_back({ rgb[0], rgb[1], rgb[2] });
        return static_cast<int>(colors.size());
    }
};

// Appends a decimal integer without a formatted print.
//
// These run once per colour span and once per non-ASCII code point of an RTF
// copy, and a printf costs an order of magnitude more than the digits it
// produces — it parses a format string and walks a varargs list to emit at most
// six characters.
static void appendDecimal(std::string& out, int value)
{
    if (value < 0) {
        out.push_back('-');
        value = -value;
    }
    // Filled from the back so the digits land in reading order and reach the
    // string in one append. A push_back a digit re-checks the capacity and
    // re-writes the size for each one, and this runs twice per run of the RTF
    // body — the colour indices — where a run is a few cells of a copy.
    std::array<char, 12> digits;
    std::size_t index = digits.size();
    do {
        digits[--index] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    out.append(digits.data() + index, digits.size() - index);
}

// Append a code point as RTF body bytes: ASCII printable passes through (\ { }
// escaped); everything else becomes a \uN escape with an ASCII '?' fallback.
void appendRTFCharacter(std::string& out, char32_t codePoint)
{
    if (codePoint == U'\\' || codePoint == U'{' || codePoint == U'}') {
        out.push_back('\\');
        out.push_back(static_cast<char>(codePoint));
        return;
    }
    if (codePoint >= 0x20 && codePoint < 0x7f) {
        out.push_back(static_cast<char>(codePoint));
        return;
    }
    // RTF \uN takes a SIGNED 16-bit value; emit a surrogate pair for astral cps.
    auto appendUnit = [&out](int unit) {
        out += "\\u";
        appendDecimal(out, unit);
        out.push_back('?');
    };
    if (codePoint <= 0xffff) {
        appendUnit(static_cast<int>(static_cast<std::int16_t>(codePoint)));
    } else {
        std::uint32_t v = static_cast<std::uint32_t>(codePoint) - 0x10000;
        appendUnit(static_cast<int>(static_cast<std::int16_t>(0xd800 + (v >> 10))));
        appendUnit(static_cast<int>(static_cast<std::int16_t>(0xdc00 + (v & 0x3ff))));
    }
}

// --- HTML helpers --------------------------------------------------------

void appendHTMLCharacter(std::string& out, char32_t codePoint)
{
    switch (codePoint) {
    case U'&':
        out += "&amp;";
        return;
    case U'<':
        out += "&lt;";
        return;
    case U'>':
        out += "&gt;";
        return;
    case U'"':
        out += "&quot;";
        return;
    default:
        break;
    }
    Unicode::appendUTF8(out, static_cast<std::uint32_t>(codePoint));
}

void appendHTMLColor(std::string& out, const std::array<std::uint8_t, 3>& rgb)
{
    // Seven bytes in one append rather than seven push_backs: the HTML body
    // writes two of these per run that carries colour, and each push_back is a
    // capacity test and a size update for one character.
    static constexpr char hexDigits[] = "0123456789abcdef";
    const std::array<char, 7> text { '#',
        hexDigits[rgb[0] >> 4], hexDigits[rgb[0] & 0xf],
        hexDigits[rgb[1] >> 4], hexDigits[rgb[1] & 0xf],
        hexDigits[rgb[2] >> 4], hexDigits[rgb[2] & 0xf] };
    out.append(text.data(), text.size());
}

// A generic-family fallback so the rich flavors stay valid when no face is given.
const char* faceOrFallback(const StyledFont& font) { return (font.faceName && font.faceName[0]) ? font.faceName : "Menlo"; }

double sizeOrFallback(const StyledFont& font) { return font.pointSize > 0 ? font.pointSize : 12.0; }

} // namespace

StyledRuns buildStyledRuns(const Ubiquitty::SelectionBounds* b, Ubiquitty::SelectionMode mode, int columns, Ubiquitty::SelectionCellAt cellAt, void* cellContext, TextCellAt textAt, void* textContext, StyledCellAt styledAt, void* styledContext, SoftWrapNewline newline, Ubiquitty::SelectionRowWrappedCallback rowWrapped, void* wrappedContext,
    StyleColorMode colorMode, const StyledFont& font, const std::array<std::uint8_t, 3>& defaultForeground, const std::array<std::uint8_t, 3>& defaultBackground, Ubiquitty::SelectionRowExtentAt rowExtent, void* extentContext)
{
    StyledRuns walked;
    walked.fontFaceName = faceOrFallback(font);
    walked.fontPointSize = sizeOrFallback(font);
    walked.colorMode = colorMode;
    walked.defaultForeground = defaultForeground;
    walked.defaultBackground = defaultBackground;
    if (!b || columns <= 0)
        return walked;

    // Pass 1: build the run list + the plain text in one walk, mirroring
    // SelectionText (rowSpan clip + trailing-blank trim + soft-wrap join).
    std::vector<StyledRun> runs;
    std::u32string runText; // every run's code points, back to back
    std::string plain;

    auto pushCharacter = [&](const SurvivingStyle& style, char32_t codePoint) {
        if (runs.empty() || runs.back().isBreak || !(runs.back().style == style))
            runs.push_back(StyledRun { style, runText.size(), 0, false });
        runText.push_back(codePoint);
        ++runs.back().textLength;
    };

    // The newline after a row is written once the next row proves to be a
    // line of its own, by the plain walk's rule: a row of continuations
    // repeats the row above and adds no line.
    bool isNewlineOwed = false;
    const auto writeOwedNewline = [&] {
        if (!isNewlineOwed)
            return;
        plain.push_back('\n');
        runs.push_back(StyledRun { SurvivingStyle {}, runText.size(), 0, true });
        isNewlineOwed = false;
    };
    for (int row = b->row0; row <= b->row1; ++row) {
        const RowSpan span = rowSpan(b, mode, row, columns, cellAt, cellContext,
            /*attributesAt*/ nullptr, /*attributeContext*/ nullptr);
        if (span.selected) {
            int scanFrom = span.column1;
            if (rowExtent) {
                const int extent = rowExtent(extentContext, row);
                if (extent - 1 < scanFrom)
                    scanFrom = extent - 1;
            }
            const int lastNonBlank = lastNonBlankSelectionColumn(row, span.column0, scanFrom,
                cellAt, cellContext, textAt, textContext, runSpacesClosingRow(b, row, newline, rowWrapped, wrappedContext));
            if (lastNonBlank < span.column0)
                writeOwedNewline();
            for (int column = span.column0; column <= lastNonBlank; ++column) {
                const StyledCell cell = styledAt(styledContext, row, column);
                if (cell.isContinuation)
                    continue;
                writeOwedNewline();
                const SurvivingStyle style = surviving(cell, colorMode, defaultBackground);
                // The same cut the plain walk makes, so the three flavours of
                // one copy carry the same letters.
                if (!cell.runCodePoints.empty()) {
                    const int runCount = static_cast<int>(cell.runCodePoints.size());
                    const PackedCellCut cut = cell.isUncut ? PackedCellCut { 0, runCount } : packedCellCut(b, mode, span, row, column, runCount);
                    for (int i = cut.first; i < cut.end; ++i) {
                        const std::uint32_t codePoint = RichText::copiedCodePoint(cell.runCodePoints[static_cast<std::size_t>(i)]);
                        Unicode::appendUTF8(plain, codePoint);
                        pushCharacter(style, static_cast<char32_t>(codePoint));
                    }
                } else if (cell.codePointCount > 0) {
                    const PackedCellCut cut = packedCellCut(b, mode, span, row, column, cell.codePointCount);
                    for (int i = cut.first; i < cut.end && i < static_cast<int>(cell.codePoints.size()); ++i) {
                        const std::uint32_t codePoint = cell.codePoints[static_cast<std::size_t>(i)];
                        Unicode::appendUTF8(plain, codePoint);
                        pushCharacter(style, static_cast<char32_t>(codePoint));
                    }
                } else {
                    const std::uint32_t codePoint = cell.codePoint != 0 ? cell.codePoint : static_cast<std::uint32_t>(' ');
                    Unicode::appendUTF8(plain, codePoint);
                    pushCharacter(style, static_cast<char32_t>(codePoint));
                }
            }
        } else {
            writeOwedNewline();
        }
        if (row < b->row1) {
            const bool wrapped = newline == SoftWrapNewline::Join && rowWrapped != nullptr && rowWrapped(wrappedContext, row) != 0;
            if (!wrapped)
                isNewlineOwed = true;
        }
    }

    walked.runs = std::move(runs);
    walked.text = std::move(runText);
    walked.plain = std::move(plain);
    return walked;
}

void reverseCopyLines(StyledRuns& walked)
{
    std::vector<StyledRun> runs;
    std::u32string text;
    std::string plain;
    text.reserve(walked.text.size());
    plain.reserve(walked.plain.size());
    std::vector<std::size_t> boundaries;
    std::size_t firstRun = 0;
    while (firstRun < walked.runs.size()) {
        if (walked.runs[firstRun].isBreak) {
            runs.push_back({ {}, text.size(), 0, true });
            plain.push_back('\n');
            ++firstRun;
            continue;
        }
        std::size_t endRun = firstRun;
        while (endRun < walked.runs.size() && !walked.runs[endRun].isBreak)
            ++endRun;
        const std::size_t first = walked.runs[firstRun].textOffset;
        const StyledRun& lastRun = walked.runs[endRun - 1];
        const std::size_t end = lastRun.textOffset + lastRun.textLength;
        boundaries.clear();
        boundaries.push_back(first);
        if (first < end) {
            Unicode::GraphemeState state;
            Unicode::graphemeStateReset(&state, walked.text[first]);
            for (std::size_t index = first + 1; index < end; ++index) {
                if (!Unicode::graphemeExtends(&state, walked.text[index - 1], walked.text[index]))
                    boundaries.push_back(index);
            }
        }
        boundaries.push_back(end);
        for (std::size_t cluster = boundaries.size() - 1; cluster > 0; --cluster) {
            std::size_t offset = boundaries[cluster - 1];
            const std::size_t limit = boundaries[cluster];
            // A cluster can cross a style boundary; retain the style of each fragment.
            const auto lineRuns = std::span(walked.runs).subspan(firstRun, endRun - firstRun);
            auto source = std::upper_bound(lineRuns.begin(), lineRuns.end(), offset,
                [](std::size_t position, const StyledRun& run) { return position < run.textOffset; });
            if (source != lineRuns.begin())
                --source;
            while (offset < limit && source != lineRuns.end()) {
                const std::size_t fragmentEnd = std::min(limit, source->textOffset + source->textLength);
                if (runs.empty() || runs.back().isBreak || !(runs.back().style == source->style))
                    runs.push_back({ source->style, text.size(), 0, false });
                text.append(walked.text, offset, fragmentEnd - offset);
                runs.back().textLength += fragmentEnd - offset;
                for (; offset < fragmentEnd; ++offset)
                    Unicode::appendUTF8(plain, walked.text[offset]);
                ++source;
            }
        }
        firstRun = endRun;
    }
    walked.runs = std::move(runs);
    walked.text = std::move(text);
    walked.plain = std::move(plain);
}

StyledClipboard emitStyledFlavors(const StyledRuns& walked, StyledFlavors which)
{
    StyledClipboard result;
    result.plain = walked.plain;
    const std::u32string_view runText { walked.text };
    const char* const faceName = walked.fontFaceName.c_str();
    const double facePointSize = walked.fontPointSize;
    const StyleColorMode colorMode = walked.colorMode;
    const std::array<std::uint8_t, 3>& defaultForeground = walked.defaultForeground;
    const std::array<std::uint8_t, 3>& defaultBackground = walked.defaultBackground;
    // The code points of one run, as a view into the buffer every run addresses.
    const auto runCharacters = [runText](const StyledRun& run) {
        return runText.substr(run.textOffset, run.textLength);
    };

    // Pass 2a: RTF. Collect the color table first (it precedes the body), then
    // emit the body referencing color indices.
    if (wants(which, StyledFlavors::RTF)) {
        ColorTable table;
        const int defaultFGIndex = table.indexFor(defaultForeground);
        table.indexFor(defaultBackground); // paper present even for all-default styles

        std::string body;
        for (const StyledRun& run : walked.runs) {
            if (run.isBreak) {
                body += "\\line\n";
                continue;
            }
            const SurvivingStyle& s = run.style;
            int cf = s.hasForeground ? table.indexFor(s.foreground) : defaultFGIndex;
            body += "{";
            if (s.isBold)
                body += "\\b";
            if (s.isItalic)
                body += "\\i";
            if (s.isUnderlined)
                body += "\\ul";
            body += "\\cf";
            appendDecimal(body, cf);
            if (s.hasBackground || colorMode == StyleColorMode::Full) {
                const int cb = table.indexFor(s.hasBackground ? s.background : defaultBackground);
                body += "\\cb";
                appendDecimal(body, cb);
                body += "\\highlight";
                appendDecimal(body, cb);
            }
            body += " ";
            for (char32_t codePoint : runCharacters(run))
                appendRTFCharacter(body, codePoint);
            body += "}";
        }

        std::string rtf = R"({\rtf1\ansi\ansicpg65001\deff0)";
        rtf += R"({\fonttbl{\f0\fmodern\fcharset0 )";
        for (const char* p = faceName; *p; ++p) {
            if (*p == '\\' || *p == '{' || *p == '}')
                rtf.push_back('\\');
            rtf.push_back(*p);
        }
        rtf += ";}}";
        rtf += "{\\colortbl;"; // index 0 is auto (empty entry)
        char ent[64];
        for (const auto& c : table.colors) {
            std::snprintf(ent, sizeof(ent), R"(\red%d\green%d\blue%d;)", static_cast<int>(c[0]), static_cast<int>(c[1]), static_cast<int>(c[2]));
            rtf += ent;
        }
        rtf += "}";
        // AppKit interprets the ordinary color table as calibrated RGB unless
        // an expanded table identifies the terminal palette's sRGB values.
        rtf += R"({\*\expandedcolortbl;)";
        for (const auto& color : table.colors) {
            rtf += R"(\cssrgb)";
            for (const auto component : color) {
                rtf += R"(\c)";
                appendDecimal(rtf, (static_cast<int>(component) * 100000 + 127) / 255);
            }
            rtf += ";";
        }
        rtf += "}";
        int halfPoints = static_cast<int>(facePointSize * 2.0 + 0.5);
        std::snprintf(ent, sizeof(ent), "\\f0\\fs%d ", halfPoints);
        rtf += "\\pard";
        rtf += ent;
        rtf += body;
        rtf += "}";
        result.rtf = std::move(rtf);
    }

    // Pass 2b: HTML. A <pre> with a body background (paper) + default text color,
    // each run a <span> overriding what survives.
    if (wants(which, StyledFlavors::HTML)) {
        std::string html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"></head><body>\n";
        html += "<pre style=\"";
        html += "font-family:'";
        for (const char* p = faceName; *p; ++p)
            appendHTMLCharacter(html, static_cast<char32_t>(static_cast<unsigned char>(*p)));
        html += "',monospace;";
        char fs[48];
        std::snprintf(fs, sizeof(fs), "font-size:%gpx;", facePointSize);
        html += fs;
        if (colorMode != StyleColorMode::StylesOnly) {
            html += "color:";
            appendHTMLColor(html, defaultForeground);
            html += ";";
        }
        if (colorMode == StyleColorMode::Full) {
            html += "background-color:";
            appendHTMLColor(html, defaultBackground);
            html += ";";
        }
        html += "\">";

        for (const StyledRun& run : walked.runs) {
            if (run.isBreak) {
                html.push_back('\n');
                continue;
            }
            const SurvivingStyle& s = run.style;
            const bool hasAnyStyle = s.hasForeground || s.hasBackground || s.isBold || s.isItalic || s.isUnderlined;
            if (hasAnyStyle) {
                html += "<span style=\"";
                if (s.hasForeground) {
                    html += "color:";
                    appendHTMLColor(html, s.foreground);
                    html += ";";
                }
                if (s.hasBackground) {
                    html += "background-color:";
                    appendHTMLColor(html, s.background);
                    html += ";";
                }
                if (s.isBold)
                    html += "font-weight:bold;";
                if (s.isItalic)
                    html += "font-style:italic;";
                if (s.isUnderlined)
                    html += "text-decoration:underline;";
                html += "\">";
            }
            for (char32_t codePoint : runCharacters(run))
                appendHTMLCharacter(html, codePoint);
            if (hasAnyStyle)
                html += "</span>";
        }
        html += "</pre>\n</body></html>\n";
        result.html = std::move(html);
    }

    return result;
}

StyledClipboard buildStyledClipboard(const Ubiquitty::SelectionBounds* b, Ubiquitty::SelectionMode mode, int columns, Ubiquitty::SelectionCellAt cellAt, void* cellContext, TextCellAt textAt, void* textContext, StyledCellAt styledAt, void* styledContext, SoftWrapNewline newline, Ubiquitty::SelectionRowWrappedCallback rowWrapped, void* wrappedContext,
    StyleColorMode colorMode, const StyledFont& font, const std::array<std::uint8_t, 3>& defaultForeground, const std::array<std::uint8_t, 3>& defaultBackground, Ubiquitty::SelectionRowExtentAt rowExtent, void* extentContext)
{
    return emitStyledFlavors(buildStyledRuns(b, mode, columns, cellAt, cellContext, textAt, textContext, styledAt, styledContext, newline, rowWrapped, wrappedContext, colorMode, font, defaultForeground, defaultBackground, rowExtent, extentContext), StyledFlavors::All);
}

} // namespace Ubiquitty::Selection
