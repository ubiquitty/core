// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_rule_weight_tracks_thickness — every underline shape must answer the
// thickness setting, and in step with the single rule.
//
// `underline_thickness` scales `metrics.underlineThickness`. The single rule
// takes it in whole device rows, as AppKit paints a face's underline, and the
// patterned shapes take the single rule's weight: a dash on its rows, a dot its
// weight across, a curl's stroke its weight measured across the curve. So no
// shape reads lighter or heavier than the straight rule beside it, and the
// setting moves them all together.
//
// This measures the composited surface rather than the emitted instance: what
// a user sees is how the rasterizer turns a band into alpha, and a shape whose
// rasterizer ignored part of its band would pass on the instance alone.

#include "Harness.h"

#include "Core/Rendering/CellInstanceBuilder.h"

#include "Core/Pen.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/TermPalette.h"
#include "Core/VT/Constants.h"

#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

constexpr int rows = 1;
constexpr int columns = 4;
constexpr int cellWidth = 8;
constexpr int cellHeight = 34;

// Total alpha the rule paints over the whole surface, as a float so a coverage
// change smaller than one pixel row is still visible in the number. Summed over
// every channel-A byte: the cell is blank apart from its rule, so this is the
// rule's weight and nothing else.
double ruleWeight(VT::UnderlineShape underlineShape, float underlineThickness)
{
    TermPalette palette;
    palette.seedDefaults();

    std::vector<Cell> cells { static_cast<std::size_t>(rows) * columns, defaultBlankCell };
    for (Cell& cell : cells) {
        // A blank cell still carries its pen, and a rule is drawn for the cell
        // rather than for the glyph, so no ink is needed to place one. Blank
        // also keeps the sum free of glyph coverage.
        Pen pen = cellPen(cell);
        pen.attributes.add(VT::Attribute::Underline);
        penSetUnderlineShape(&pen, underlineShape);
        cellSetPen(cell, pen);
    }

    SoftwareRenderer renderer { cellWidth, cellHeight };
    BuildInputs in;
    in.cells = cells.data();
    in.rows = rows;
    in.columns = columns;
    in.palette = &palette;
    in.metrics.cellWidth = static_cast<float>(cellWidth);
    in.metrics.cellHeight = static_cast<float>(cellHeight);
    in.metrics.baselineFromTop = 25.0f;
    in.metrics.underlineThickness = underlineThickness;
    in.metrics.underlineThicknessBold = underlineThickness;
    in.metrics.underlineCurlyThickness = underlineThickness;
    in.metrics.strikeThickness = underlineThickness;

    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    if (!renderer.render(in, rgba, width, height))
        return -1.0;

    double weight = 0.0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        // Against a black ground the rule's own luminance is its coverage. Read
        // one channel: the rule is drawn in the default foreground, which is
        // neutral, so any channel carries the same coverage.
        weight += static_cast<double>(rgba[i]);
    }
    return weight;
}

std::string shapeName(VT::UnderlineShape shape)
{
    switch (shape) {
    case VT::UnderlineShape::Single:
        return "single";
    case VT::UnderlineShape::Double:
        return "double";
    case VT::UnderlineShape::Curly:
        return "curly";
    case VT::UnderlineShape::Dotted:
        return "dotted";
    case VT::UnderlineShape::Dashed:
        return "dashed";
    default:
        return "unknown";
    }
}

} // namespace

// The setting's whole job. A user who doubles the thickness must see a heavier
// rule in every shape, not only in the shapes whose rasterizer happens to
// anti-alias vertically.
TEST(everyUnderlineShapeGrowsHeavierWithTheThicknessSetting)
{
    const VT::UnderlineShape shapes[] = { VT::UnderlineShape::Single, VT::UnderlineShape::Double,
        VT::UnderlineShape::Curly, VT::UnderlineShape::Dotted, VT::UnderlineShape::Dashed };

    for (const VT::UnderlineShape shape : shapes) {
        const double thin = ruleWeight(shape, 1.0f);
        const double thick = ruleWeight(shape, 2.0f);
        CHECK_MSG(thin > 0.0, (shapeName(shape) + " draws a rule at all").c_str());
        CHECK_MSG(thick > thin,
            (shapeName(shape) + " is heavier at 2.0 than at 1.0 (thin=" + std::to_string(thin)
                + " thick=" + std::to_string(thick) + ")")
                .c_str());
    }
}

// The setting moves every shape together. Each patterned shape takes the
// single rule's weight, drawn exactly, so a whole step of the setting makes
// each of them heavier with the single rule. The steps are whole because the
// marks keep a whole-pixel pitch: between two neighbouring sub-pixel settings
// the pitch can step up a pixel and leave a dotted line a hair lighter, which
// is the price of dots that sit on the pixel grid.
TEST(everyPatternedShapeGrowsWithTheSingleRule)
{
    const float settings[] = { 1.0f, 2.0f, 3.0f, 4.0f };
    const VT::UnderlineShape shapes[] = { VT::UnderlineShape::Curly, VT::UnderlineShape::Dotted, VT::UnderlineShape::Dashed };
    for (std::size_t i = 1; i < std::size(settings); ++i) {
        const float from = settings[i - 1];
        const float to = settings[i];
        CHECK_MSG(ruleWeight(VT::UnderlineShape::Single, to) > ruleWeight(VT::UnderlineShape::Single, from), "the single rule grows with the setting");
        for (const VT::UnderlineShape shape : shapes) {
            const double before = ruleWeight(shape, from);
            const double after = ruleWeight(shape, to);
            CHECK_MSG(after > before,
                (shapeName(shape) + " from " + std::to_string(from) + " to " + std::to_string(to) + " grows with the single rule (before="
                    + std::to_string(before) + " after=" + std::to_string(after) + ")")
                    .c_str());
        }
    }
}

UBIQUITTY_TEST_MAIN()
