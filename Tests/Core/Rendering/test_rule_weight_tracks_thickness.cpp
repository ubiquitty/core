// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_rule_weight_tracks_thickness — every underline shape must answer the
// thickness setting.
//
// `underline_thickness` scales `metrics.underlineThickness`, and each shape
// derives its band from that number, so on the geometry side all five shapes
// already track it. What reaches the surface is decided one step later, by how
// the rasterizer turns a band into alpha.
//
// A shape that modulates alpha by the pixel's distance from the band edge
// answers a band change immediately: a band 30% taller is 30% more coverage,
// spread across the same rows. A shape that only asks whether a pixel center
// falls inside answers in whole pixel rows, so a band change smaller than a
// row is not rendered at all and the setting appears inert until it crosses
// the next row boundary.
//
// This file pins that every shape is in the first group. It is a rasterizer
// obligation, not a builder one, which is why it measures the composited
// surface rather than the emitted instance: the instance was always correct.

#include "Harness.h"

#include "Core/Rendering/CellInstanceBuilder.h"

#include "Core/Pen.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/TermPalette.h"
#include "Core/VT/Constants.h"

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
double ruleWeight(std::uint8_t underlineShape, float underlineThickness)
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

std::string shapeName(std::uint8_t shape)
{
    switch (shape) {
    case VT::underlineSingle:
        return "single";
    case VT::underlineDouble:
        return "double";
    case VT::underlineCurly:
        return "curly";
    case VT::underlineDotted:
        return "dotted";
    case VT::underlineDashed:
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
    const std::uint8_t shapes[] = { VT::underlineSingle, VT::underlineDouble,
        VT::underlineCurly, VT::underlineDotted, VT::underlineDashed };

    for (const std::uint8_t shape : shapes) {
        const double thin = ruleWeight(shape, 1.0f);
        const double thick = ruleWeight(shape, 2.0f);
        CHECK_MSG(thin > 0.0, (shapeName(shape) + " draws a rule at all").c_str());
        CHECK_MSG(thick > thin,
            (shapeName(shape) + " is heavier at 2.0 than at 1.0 (thin=" + std::to_string(thin)
                + " thick=" + std::to_string(thick) + ")")
                .c_str());
    }
}

// The step between neighbouring settings is what a user actually turns. A shape
// that only answers in whole pixel rows clears the coarse check above while
// staying visibly inert across the range the preference offers, so the fine
// step is the one that separates the two rasterizer families.
TEST(everyUnderlineShapeAnswersASubPixelThicknessStep)
{
    const std::uint8_t shapes[] = { VT::underlineCurly, VT::underlineDotted, VT::underlineDashed };

    for (const std::uint8_t shape : shapes) {
        const double base = ruleWeight(shape, 1.0f);
        const double stepped = ruleWeight(shape, 1.25f);
        CHECK_MSG(stepped > base,
            (shapeName(shape) + " answers a quarter-step (base=" + std::to_string(base)
                + " stepped=" + std::to_string(stepped) + ")")
                .c_str());
    }
}

UBIQUITTY_TEST_MAIN()
