// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_skip_ink — where glyph ink cuts an underline (SkipInk.h).
//
// The rule every renderer draws is decided here, so these cases are the whole
// specification: ink cuts an underline only where it touches it, in its rows
// or the row adjacent; the cut widens across by the padding; each row is
// judged on its own; a texel is ink at half coverage; the padding is measured
// from where a stroke's coverage puts its edge, so a glyph drawn at a subpixel
// phase is cleared alike on both sides; and a pixel is cut or kept whole. Planes are small synthetic sheets mapped one texel to a device
// pixel, so every expected column and row can be read off the picture.

#include "Harness.h"
#include <algorithm>

#include "Core/Rendering/DottedRuleRun.h"
#include "Core/Rendering/SkipInk.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace Ubiquitty::Rendering;

// A row of the cut against the row expected, both shown when they differ.
#define CHECK_ROW(actual, expected) CHECK_MSG((actual) == (expected), ("got " + std::string(actual) + ", want " + std::string(expected)).c_str())

namespace {

/// A sheet of coverage, one byte a texel, with ink painted in rectangles.
struct Sheet {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> coverage;

    Sheet(int sheetWidth, int sheetHeight)
        : width(sheetWidth)
        , height(sheetHeight)
        , coverage(static_cast<std::size_t>(sheetWidth) * static_cast<std::size_t>(sheetHeight), 0)
    {
    }
    void paint(int left, int top, int right, int bottom, std::uint8_t value = 255)
    {
        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x)
                coverage[static_cast<std::size_t>(y * width + x)] = value;
    }
    [[nodiscard]] SkipInkPlanes planes() const
    {
        SkipInkPlanes planes;
        planes.mono.coverage = coverage.data();
        planes.mono.width = width;
        planes.mono.height = height;
        planes.mono.stride = width;
        return planes;
    }
};

/// A source reading the sheet one texel to a pixel, as a rule laid over the
/// sheet itself sees it, with the whole sheet as its slot.
SkipInkSource identity(const Sheet& sheet, const SkipInkRule& rule, float boxesBefore = 0.0f)
{
    SkipInkSource source;
    source.sheet = SkipInkSheet::Mono;
    source.inkLeft = rule.left;
    source.inkRight = rule.right;
    source.bandTop = rule.top;
    source.bandBottom = rule.bottom;
    source.boxesBefore = boxesBefore;
    source.slotRight = static_cast<float>(sheet.width);
    source.slotBottom = static_cast<float>(sheet.height);
    source.isPresent = true;
    return source;
}

SkipInkQuery query(const Sheet& sheet, const SkipInkRule& rule, float padding)
{
    SkipInkQuery query;
    query.own = identity(sheet, rule);
    query.reachAcross = padding;
    query.reachUpDown = skipInkContactInDevicePixels;
    return query;
}

/// The cut as a picture, one string a rule row and a character a pixel: '#'
/// where all of the pixel is cut, '+' where part of it is, '-' where none is.
std::vector<std::string> picture(const SkipInkRule& rule, const SkipInkCut& cut)
{
    std::vector<std::string> rows;
    for (int row = 0; row < rule.rows(); ++row) {
        std::string line;
        for (int column = 0; column < rule.columns(); ++column) {
            const float left = static_cast<float>(rule.firstColumn() + column);
            const float covered = skipInkCutCoverage(cut.row(row), left, left + 1.0f);
            line += covered >= 0.999f ? '#' : (covered <= 0.001f ? '-' : '+');
        }
        rows.push_back(line);
    }
    return rows;
}

std::vector<std::string> cutOf(const Sheet& sheet, const SkipInkRule& rule, const SkipInkQuery& query)
{
    SkipInkScratch scratch;
    markSkipInkCuts(sheet.planes(), query, rule, scratch.cut, scratch);
    return picture(rule, scratch.cut);
}

/// The one span a row is cut across, or [0, 0) when it is not cut once.
std::pair<float, float> onlySpanOf(const Sheet& sheet, const SkipInkRule& rule, const SkipInkQuery& query, int row = 0)
{
    SkipInkScratch scratch;
    markSkipInkCuts(sheet.planes(), query, rule, scratch.cut, scratch);
    const std::span<const float> spans = scratch.cut.row(row);
    if (spans.size() != 2)
        return { 0.0f, 0.0f };
    return { spans[0], spans[1] };
}

} // namespace

TEST(theBowlOfALetterWithAGapAboveTheRuleLeavesItWhole)
{
    // A bowl resting on the baseline at row 10, one empty row (11), then a
    // two-row rule at rows 12 and 13. However wide the padding, the bowl does
    // not touch the rule.
    Sheet sheet(24, 20);
    sheet.paint(6, 4, 14, 11);
    const SkipInkRule rule { .left = 0, .top = 12, .right = 24, .bottom = 14 };
    for (float padding : { 0.0f, 1.0f, 3.5f, 8.0f }) {
        const auto cut = cutOf(sheet, rule, query(sheet, rule, padding));
        CHECK_ROW(cut[0], std::string(24, '-'));
        CHECK_ROW(cut[1], std::string(24, '-'));
    }
}

TEST(aStrokeRestingOnTheRuleBitesIntoItAsDeepAsThePadding)
{
    // A stroke ending in row 11, directly above the rule: it touches it, and
    // bites into it as far down as the padding reaches. With a padding of two
    // the top row is cut the whole padding either side, and the bottom row, a
    // row of white below the stroke, as far as a circle of radius two reaches:
    // the square root of three either side. With a padding of one the bite
    // stops at the top row.
    Sheet sheet(24, 20);
    sheet.paint(10, 4, 12, 12);
    const SkipInkRule rule { .left = 0, .top = 12, .right = 24, .bottom = 14 };
    const auto deep = cutOf(sheet, rule, query(sheet, rule, 2.0f));
    CHECK_ROW(deep[0], std::string("--------######----------"));
    CHECK_ROW(deep[1], std::string("--------+####+----------"));
    SkipInkScratch scratch;
    markSkipInkCuts(sheet.planes(), query(sheet, rule, 2.0f), rule, scratch.cut, scratch);
    const std::span<const float> bottom = scratch.cut.row(1);
    REQUIRE(bottom.size() == 2u);
    CHECK(std::fabs(bottom[0] - (10.0f - std::sqrt(3.0f))) < 0.01f);
    CHECK(std::fabs(bottom[1] - (12.0f + std::sqrt(3.0f))) < 0.01f);
    const auto shallow = cutOf(sheet, rule, query(sheet, rule, 1.0f));
    CHECK_ROW(shallow[0], std::string("---------####-----------"));
    CHECK_ROW(shallow[1], std::string(24, '-'));
}

TEST(aDescenderEndingInsideTheRuleCutsEveryRowItTouches)
{
    // The case a short descender meets: its tip ends in the rule's top row.
    // The row below the tip touches it, so the cut runs through the whole
    // rule, the padding wide each side, clean in both rows.
    Sheet sheet(24, 20);
    sheet.paint(10, 4, 12, 13);
    const SkipInkRule rule { .left = 0, .top = 12, .right = 24, .bottom = 14 };
    const auto cut = cutOf(sheet, rule, query(sheet, rule, 3.0f));
    CHECK_ROW(cut[0], std::string("-------########---------"));
    CHECK_ROW(cut[1], std::string("-------########---------"));
}

TEST(aSlantedStrokeIsClearedByThePaddingAsADistance)
{
    // A tail two pixels wide slanting a pixel left each row down, through a
    // two-row rule. Each row is cleared the padding from the stroke measured
    // as a distance, edge to edge: the whole padding beside the ink in the
    // row and the rows resting against it, and from ink a row farther off as
    // much as a circle of the padding's radius reaches. So the cut steps with
    // the slant, and the side the tail leans toward keeps as much white
    // between line and stroke as a straight stem's sides do.
    Sheet sheet(32, 20);
    for (int row = 4; row < 18; ++row)
        sheet.paint(24 - row, row, 26 - row, row + 1);
    const SkipInkRule rule { .left = 0, .top = 12, .right = 32, .bottom = 14 };
    const float circle = std::sqrt(3.0f); // a padding of two, a row of white apart
    const struct {
        float padding;
        float left[2];
        float right[2];
    } expected[] = {
        { 1.0f, { 10.0f, 9.0f }, { 16.0f, 15.0f } },
        { 2.0f, { 10.0f - circle, 9.0f - circle }, { 16.0f + circle, 15.0f + circle } },
    };
    for (const auto& want : expected) {
        SkipInkScratch scratch;
        REQUIRE(markSkipInkCuts(sheet.planes(), query(sheet, rule, want.padding), rule, scratch.cut, scratch));
        for (int row = 0; row < 2; ++row) {
            const std::span<const float> spans = scratch.cut.row(row);
            REQUIRE(spans.size() == 2u);
            CHECK_MSG(std::fabs(spans[0] - want.left[row]) < 0.01f && std::fabs(spans[1] - want.right[row]) < 0.01f,
                ("padding " + std::to_string(want.padding) + " row " + std::to_string(row) + " cut " + std::to_string(spans[0]) + ".." + std::to_string(spans[1])).c_str());
        }
    }
}

TEST(aBowlJoiningAStemDoesNotWidenTheStemsCut)
{
    // A p: a stem through a two-row rule, and a bowl joining it whose bottom
    // is two rows above the rule. Followed up from the rule, the stem is
    // taken no more than a pixel wider a row, so the bowl it joins adds
    // nothing while the padding stops short of it and a pixel at most once it
    // reaches it.
    Sheet sheet(32, 20);
    sheet.paint(10, 2, 12, 17);
    sheet.paint(12, 4, 18, 10);
    const SkipInkRule rule { .left = 0, .top = 12, .right = 32, .bottom = 14 };
    CHECK_ROW(cutOf(sheet, rule, query(sheet, rule, 2.0f))[0], std::string("--------######------------------"));
    const auto [left, right] = onlySpanOf(sheet, rule, query(sheet, rule, 3.0f));
    CHECK(std::fabs(left - 7.0f) < 0.01f);
    CHECK(right <= 16.01f);
}

TEST(aStretchOfLineTooShortToReadAsLineIsCutWithTheLetters)
{
    // Two stems close enough that their cuts leave three pixels of a two-pixel
    // line between them: shorter than twice the line's weight, a speck rather
    // than line, so it goes with them. A stub as short between a cut and the
    // line's end goes too, unless the line runs on past that end.
    const SkipInkRule rule { .left = 0, .top = 12, .right = 24, .bottom = 14 };
    Sheet close(24, 20);
    close.paint(10, 4, 12, 16);
    close.paint(17, 4, 19, 16);
    SkipInkQuery between = query(close, rule, 1.0f);
    between.shortestPiece = 4.0f;
    CHECK_ROW(cutOf(close, rule, between)[0], std::string("---------###########----"));
    Sheet nearTheEnd(24, 20);
    nearTheEnd.paint(3, 4, 5, 16);
    SkipInkQuery stub = query(nearTheEnd, rule, 1.0f);
    stub.shortestPiece = 4.0f;
    CHECK_ROW(cutOf(nearTheEnd, rule, stub)[0], std::string("######------------------"));
    stub.continuesLeft = true;
    CHECK_ROW(cutOf(nearTheEnd, rule, stub)[0], std::string("--####------------------"));
}

TEST(aRuleCutAcrossItsFullHeightTakesEveryRowsCut)
{
    // A stem through both rows cuts [7, 11) with a pixel's clearance, and a
    // blot touching only the upper row cuts [15, 19) there: cut across its
    // full height, the rule takes both cuts in both rows, so a dot laid on it
    // is whole or absent.
    const SkipInkRule rule { .left = 0, .top = 12, .right = 24, .bottom = 14 };
    Sheet sheet(24, 20);
    sheet.paint(8, 4, 10, 16);
    sheet.paint(16, 4, 18, 13);
    SkipInkQuery dots = query(sheet, rule, 1.0f);
    dots.reachUpDown = 0.0f;
    dots.isCutAcrossFullHeight = true;
    SkipInkScratch scratch;
    REQUIRE(markSkipInkCuts(sheet.planes(), dots, rule, scratch.cut, scratch));
    REQUIRE(std::ranges::equal(scratch.cut.row(0), scratch.cut.row(1)));
    REQUIRE(scratch.cut.row(0).size() == 4u);
    const auto near = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
    CHECK(near(scratch.cut.row(1)[0], 7.0f) && near(scratch.cut.row(1)[1], 11.0f));
    CHECK(near(scratch.cut.row(1)[2], 15.0f) && near(scratch.cut.row(1)[3], 19.0f));
}

TEST(theMemoCutsAGlyphOnceWhereverItsRuleStands)
{
    // One glyph read under two rules of one shape, the second far along the
    // line at a fractional edge: memoised, the second is served from the
    // first's entry, and both land where a cut worked out afresh does.
    const SkipInkRule here { .left = 0, .top = 12, .right = 24, .bottom = 14 };
    const SkipInkRule there { .left = 240.25f, .top = 12, .right = 264.25f, .bottom = 14 };
    Sheet sheet(24, 20);
    sheet.paint(8, 4, 10, 16);
    const SkipInkQuery stem = query(sheet, here, 1.0f);
    const auto spansOf = [&](const SkipInkRule& rule, SkipInkScratch& scratch) {
        CHECK(markSkipInkCuts(sheet.planes(), stem, rule, scratch.cut, scratch));
        return scratch.cut.spans;
    };
    SkipInkScratch fresh;
    const std::vector<float> freshHere = spansOf(here, fresh);
    const std::vector<float> freshThere = spansOf(there, fresh);
    SkipInkScratch memoised;
    memoised.memo.begin();
    const std::vector<float> memoHere = spansOf(here, memoised);
    const std::size_t pooled = memoised.memo.pool().size();
    const std::vector<float> memoThere = spansOf(there, memoised);
    CHECK(pooled > 0u);
    CHECK(memoised.memo.pool().size() == pooled);
    REQUIRE(memoHere.size() == freshHere.size());
    REQUIRE(memoThere.size() == freshThere.size());
    for (std::size_t index = 0; index < freshHere.size(); ++index)
        CHECK(std::fabs(memoHere[index] - freshHere[index]) < 1.0e-4f);
    for (std::size_t index = 0; index < freshThere.size(); ++index)
        CHECK(std::fabs(memoThere[index] - freshThere[index]) < 1.0e-4f);
    REQUIRE(!freshHere.empty() && !freshThere.empty());
    CHECK(std::fabs(freshThere[0] - (freshHere[0] + 240.25f)) < 1.0e-3f);
    // The next build forgets it: the slot may hold another glyph then.
    memoised.memo.begin();
    CHECK(memoised.memo.pool().empty());
}

TEST(theCutMemoServesARuleBesideTheSameGlyphsWhereverItStands)
{
    // A whole rule's cut, memoised: the same rule beside the same glyph at
    // another place is served from the memo and lands where a cut worked out
    // afresh does; a rule whose line goes on past its edge is another cut.
    const SkipInkRule here { .left = 0, .top = 12, .right = 24, .bottom = 14 };
    const SkipInkRule there { .left = 512.5f, .top = 12, .right = 536.5f, .bottom = 14 };
    Sheet sheet(24, 20);
    sheet.paint(8, 4, 10, 16);
    sheet.paint(15, 4, 17, 13);
    SkipInkQuery stems = query(sheet, here, 1.0f);
    stems.shortestPiece = 2.0f;
    SkipInkScratch fresh;
    SkipInkScratch memoised;
    memoised.beginBuild();
    REQUIRE(markSkipInkCuts(sheet.planes(), stems, here, memoised.cut, memoised));
    const std::size_t filed = memoised.cutMemo.pool().size();
    REQUIRE(markSkipInkCuts(sheet.planes(), stems, there, memoised.cut, memoised));
    CHECK(memoised.cutMemo.pool().size() == filed);
    REQUIRE(markSkipInkCuts(sheet.planes(), stems, there, fresh.cut, fresh));
    REQUIRE(memoised.cut.rowEnds == fresh.cut.rowEnds);
    REQUIRE(memoised.cut.spans.size() == fresh.cut.spans.size());
    for (std::size_t index = 0; index < fresh.cut.spans.size(); ++index)
        CHECK(std::fabs(memoised.cut.spans[index] - fresh.cut.spans[index]) < 1.0e-3f);
    stems.continuesRight = true;
    REQUIRE(markSkipInkCuts(sheet.planes(), stems, there, memoised.cut, memoised));
    CHECK(memoised.cutMemo.pool().size() > filed);
}

TEST(aGlyphWhoseInkStopsAboveTheTouchingRowsIsPassedOver)
{
    // A round letter resting two rows above the rule: its ink never reaches
    // the rows where ink touches the rule, so a build that knows its slot's
    // inked rows passes it over without reading it, and nothing is cut. A
    // stem through the rule in another slot is cut as ever.
    const SkipInkRule rule { .left = 0, .top = 12, .right = 24, .bottom = 14 };
    Sheet round(24, 20);
    round.paint(6, 4, 18, 9);
    SkipInkQuery bowl = query(round, rule, 1.0f);
    bowl.own.slot = 0;
    SkipInkScratch scratch;
    scratch.beginBuild();
    CHECK(!markSkipInkCuts(round.planes(), bowl, rule, scratch.cut, scratch));
    REQUIRE(scratch.slotInk.size() == 1u);
    CHECK(scratch.slotInk[0].top == 4 && scratch.slotInk[0].bottom == 8);
    Sheet stem(24, 20);
    stem.paint(8, 4, 10, 16);
    SkipInkQuery through = query(stem, rule, 1.0f);
    through.own.slot = 1;
    CHECK(markSkipInkCuts(stem.planes(), through, rule, scratch.cut, scratch));
}

namespace {

/// The share a soft cut keeps at a pixel; one where no stretch drawn covers it.
float keptAt(const std::vector<SkipInkKeep>& keeps, std::int32_t row, float x)
{
    for (const SkipInkKeep& keep : keeps) {
        if (keep.row <= row && row < keep.row + keep.rows && keep.left <= x && x < keep.right)
            return keep.keep;
    }
    return 0.0f;
}

/// A curl over a 24-pixel rule six rows tall, three pixels thick, a crest
/// every twelve.
constexpr SkipInkRule curlRule { .left = 0, .top = 10, .right = 24, .bottom = 16 };
constexpr SkipInkCurl curl { .origin = 0.0f, .wavelength = 12.0f, .thickness = 3.0f };

} // namespace

TEST(aCurlBesideNoInkIsNotCut)
{
    Sheet blank(24, 20);
    std::vector<SkipInkKeep> keeps;
    SkipInkScratch scratch;
    CHECK(!markSkipInkMask(blank.planes(), query(blank, curlRule, 2.0f), curlRule, curl, keeps, scratch));
    CHECK(keeps.empty());
}

TEST(aStrokeThroughACurlFadesItAroundTheStroke)
{
    // A stem through the band, drawn half a texel off the pixel grid: gone
    // under the stem and the reach either side of it, kept whole far off, and
    // in part at the reach's edge, where a pixel's centre reads between texels.
    Sheet stem(24, 20);
    stem.paint(11, 0, 13, 20);
    SkipInkQuery shifted = query(stem, curlRule, 2.0f);
    shifted.own.inkLeft += 0.5f;
    shifted.own.inkRight += 0.5f;
    std::vector<SkipInkKeep> keeps;
    SkipInkScratch scratch;
    REQUIRE(markSkipInkMask(stem.planes(), shifted, curlRule, curl, keeps, scratch));
    for (std::int32_t row = 0; row < curlRule.rows(); ++row) {
        CHECK(keptAt(keeps, row, 11.5f) == 0.0f);
        CHECK(keptAt(keeps, row, 2.5f) == 1.0f);
        CHECK(keptAt(keeps, row, 21.5f) == 1.0f);
    }
    bool isAnyPartial = false;
    for (const SkipInkKeep& keep : keeps)
        isAnyPartial = isAnyPartial || (keep.keep > 0.0f && keep.keep < 1.0f);
    CHECK(isAnyPartial);
}

TEST(inkRestingAboveACurlCutsItWithinTheReachDownward)
{
    // A blot ending a row above the band, under the reach: grown as far down
    // as across, it cuts the band's top row, and leaves its bottom row whole.
    Sheet blot(24, 20);
    blot.paint(8, 4, 16, 9);
    std::vector<SkipInkKeep> keeps;
    SkipInkScratch scratch;
    REQUIRE(markSkipInkMask(blot.planes(), query(blot, curlRule, 2.0f), curlRule, curl, keeps, scratch));
    CHECK(keptAt(keeps, 0, 12.5f) < 1.0f);
    CHECK(keptAt(keeps, curlRule.rows() - 1, 12.5f) == 1.0f);
}

TEST(aSpeckOfCurlBetweenTheLineEndAndACutGoes)
{
    // A stem a few pixels in from where the line starts leaves less than one
    // and a half stroke widths of curl before it: that speck goes, every row.
    Sheet stem(24, 20);
    stem.paint(5, 0, 7, 20);
    std::vector<SkipInkKeep> keeps;
    SkipInkScratch scratch;
    REQUIRE(markSkipInkMask(stem.planes(), query(stem, curlRule, 1.0f), curlRule, curl, keeps, scratch));
    for (std::int32_t row = 0; row < curlRule.rows(); ++row) {
        CHECK(keptAt(keeps, row, 0.5f) == 0.0f);
        CHECK(keptAt(keeps, row, 1.5f) == 0.0f);
        CHECK(keptAt(keeps, row, 20.5f) == 1.0f);
    }
}

TEST(aSoftCutReadsTheSameMemoisedAsWorkedOutAfresh)
{
    Sheet stem(24, 20);
    stem.paint(11, 0, 13, 20);
    stem.paint(17, 2, 21, 9);
    const SkipInkQuery stems = query(stem, curlRule, 2.0f);
    std::vector<SkipInkKeep> fresh;
    SkipInkScratch freshScratch;
    REQUIRE(markSkipInkMask(stem.planes(), stems, curlRule, curl, fresh, freshScratch));
    std::vector<SkipInkKeep> memoised;
    SkipInkScratch memoScratch;
    memoScratch.beginBuild();
    REQUIRE(markSkipInkMask(stem.planes(), stems, curlRule, curl, memoised, memoScratch));
    const std::size_t grown = memoScratch.grownMemo.pool().size();
    REQUIRE(markSkipInkMask(stem.planes(), stems, curlRule, curl, memoised, memoScratch));
    CHECK(memoScratch.grownMemo.pool().size() == grown);
    REQUIRE(memoised.size() == fresh.size());
    for (std::size_t index = 0; index < fresh.size(); ++index) {
        CHECK(memoised[index].row == fresh[index].row);
        CHECK(memoised[index].rows == fresh[index].rows);
        CHECK(std::fabs(memoised[index].left - fresh[index].left) < 1.0e-4f);
        CHECK(std::fabs(memoised[index].right - fresh[index].right) < 1.0e-4f);
        CHECK(std::fabs(memoised[index].keep - fresh[index].keep) < 1.0e-4f);
    }
}

TEST(aDashedRuleIsCutSquareBesideASlantingStroke)
{
    // A comma's tail slanting down to the left across the rule's two rows:
    // it cuts the upper row further right than the lower. Cut square, both
    // rows take the cut of both, so no dash beside it is left a row tall, and
    // the dash the cut ends in keeps what is left of it only where that is two
    // pixels or more.
    const SkipInkRule rule { .left = 0, .top = 12, .right = 24, .bottom = 14 };
    Sheet sheet(24, 20);
    sheet.paint(10, 4, 12, 13);
    sheet.paint(8, 13, 10, 16);
    SkipInkQuery dashes = query(sheet, rule, 1.0f);
    dashes.reachUpDown = 0.0f;
    dashes.isCutAcrossFullHeight = true;
    dashes.pattern = { .pitch = 6.0f, .origin = 0.0f, .offset = 0.0f, .length = 4.0f, .shortest = 2.0f };
    SkipInkScratch scratch;
    REQUIRE(markSkipInkCuts(sheet.planes(), dashes, rule, scratch.cut, scratch));
    CHECK(std::ranges::equal(scratch.cut.row(0), scratch.cut.row(1)));
    REQUIRE(scratch.cut.row(0).size() == 2u);
    CHECK(scratch.cut.row(0)[0] <= 7.0f);
    CHECK(scratch.cut.row(0)[1] >= 13.0f);
}

namespace {

/// A dotted run from 0 to 40 with two-pixel dots every four pixels.
DottedRunFrame dottedRun(DottedRunEdge rightEdge)
{
    return { .left = 0.0f, .right = 40.0f, .origin = 0.0f, .pitch = 4.0f, .dot = 2.0f, .isEnding = rightEdge == DottedRunEdge::Ends };
}

bool isStretch(const DottedStretch& stretch, float left, float right, float origin)
{
    return std::fabs(stretch.left - left) < 0.01f && std::fabs(stretch.right - right) < 0.01f && std::fabs(stretch.origin - origin) < 0.01f;
}

} // namespace

TEST(theStretchBeforeTheFirstCutKeepsTheRunsRhythmInWholeDots)
{
    // Dots at 0 and 4 end before the cut at 9; the one at 8 would end inside
    // it and is not drawn, so the stretch stops past the dot at 4.
    std::vector<DottedStretch> stretches;
    const float cuts[] = { 9.0f, 13.0f };
    fitDotsBetweenCuts(dottedRun(DottedRunEdge::Ends), cuts, stretches);
    REQUIRE(stretches.size() == 2u);
    CHECK(isStretch(stretches[0], 0.0f, 6.5f, 0.0f));
}

TEST(aStretchBetweenCutsHoldsEveryDotThatFitsCentredOnThePixelGrid)
{
    // Nineteen pixels between cuts at 9 and 28 hold five dots, eighteen
    // pixels end to end: the rhythm would lay four (12 to 24), the dot at 8
    // and the one at 28 falling in the cuts. Centred they start at 9.5, put on
    // the rhythm's pixel phase at 10, and the last ends at 28.
    std::vector<DottedStretch> stretches;
    const float cuts[] = { 5.0f, 9.0f, 28.0f, 31.0f };
    fitDotsBetweenCuts(dottedRun(DottedRunEdge::Ends), cuts, stretches);
    REQUIRE(stretches.size() == 3u);
    CHECK(isStretch(stretches[1], 9.5f, 28.5f, 10.0f));
}

TEST(theStretchToTheRunsEndIsCentredWhereTheLineEnds)
{
    // From the cut at 13 to the run's end at 40: twenty-seven pixels hold
    // seven dots, twenty-six end to end, centred at 13.5 and put on the pixel
    // grid at 14.
    std::vector<DottedStretch> stretches;
    const float cuts[] = { 9.0f, 13.0f };
    fitDotsBetweenCuts(dottedRun(DottedRunEdge::Ends), cuts, stretches);
    REQUIRE(stretches.size() == 2u);
    CHECK(isStretch(stretches[1], 13.5f, 40.0f, 14.0f));
}

TEST(aStretchRunningOnPastTheRunKeepsTheRhythm)
{
    // Where the line goes on past the run's right edge the stretch's far end
    // is not known: it keeps the rhythm, from its first whole dot at 16.
    std::vector<DottedStretch> stretches;
    const float cuts[] = { 9.0f, 13.0f };
    fitDotsBetweenCuts(dottedRun(DottedRunEdge::Continues), cuts, stretches);
    REQUIRE(stretches.size() == 2u);
    CHECK(isStretch(stretches[1], 15.5f, 40.0f, 0.0f));
}

TEST(anUncutRunEndsWithItsLastWholeDot)
{
    std::vector<DottedStretch> stretches;
    DottedRunFrame frame = dottedRun(DottedRunEdge::Ends);
    frame.right = 39.0f;
    fitDotsBetweenCuts(frame, {}, stretches);
    REQUIRE(stretches.size() == 1u);
    CHECK(isStretch(stretches[0], 0.0f, 38.5f, 0.0f));
}

TEST(aHeldRunDrawsEachCellsShareOfItsStretches)
{
    // Three cells of ten pixels, cut at [8, 12) and [23, 27): the stretch
    // between the cuts crosses the cell edge at 20, and both cells draw their
    // part of it from the same origin. Eleven pixels hold three dots, ten end
    // to end, centred at 12.5 and on the grid at 13.
    DottedRuleRun run;
    const auto cell = [](float left) {
        DecorationInstance rule {};
        rule.topLeftInPixels = { left, 10.0f };
        rule.bottomRightInPixels = { left + 10.0f, 12.0f };
        rule.dashPeriodInPixels = 4.0f;
        rule.bandTopLeftInPixels = { 0.0f, 10.0f };
        rule.bandBottomRightInPixels = { left + 10.0f, 12.0f };
        return rule;
    };
    const float firstCuts[] = { 8.0f, 10.0f };
    const float secondCuts[] = { 10.0f, 12.0f };
    const float thirdCuts[] = { 23.0f, 27.0f };
    run.hold(cell(0.0f), firstCuts, DottedRunEdge::Continues, DottedRuleRun::untraced);
    CHECK(run.doesContinueWith(cell(10.0f)));
    run.hold(cell(10.0f), secondCuts, DottedRunEdge::Continues, DottedRuleRun::untraced);
    run.hold(cell(20.0f), thirdCuts, DottedRunEdge::Ends, DottedRuleRun::untraced);
    std::vector<DecorationInstance> pieces;
    std::vector<std::uint32_t> counts;
    run.release([&](const DecorationInstance& piece) { pieces.push_back(piece); },
        [&](std::size_t, std::uint32_t count) { counts.push_back(count); });
    CHECK(run.isEmpty());
    REQUIRE(counts.size() == 3u);
    CHECK(counts[0] == 1u && counts[1] == 1u && counts[2] == 2u);
    REQUIRE(pieces.size() == 4u);
    // The run's own dots before the first cut, at 0 and 4.
    CHECK(std::fabs(pieces[0].bottomRightInPixels.x - 6.5f) < 0.01f && std::fabs(pieces[0].bandTopLeftInPixels.x) < 0.01f);
    // The centred stretch, split at the cell edge, from its own origin: the
    // fringe before its first dot lies outside its band.
    CHECK(std::fabs(pieces[1].topLeftInPixels.x - 13.0f) < 0.01f && std::fabs(pieces[1].bottomRightInPixels.x - 20.0f) < 0.01f);
    CHECK(std::fabs(pieces[2].topLeftInPixels.x - 20.0f) < 0.01f && std::fabs(pieces[2].bottomRightInPixels.x - 23.5f) < 0.01f);
    CHECK(std::fabs(pieces[1].bandTopLeftInPixels.x - 13.0f) < 0.01f && std::fabs(pieces[2].bandTopLeftInPixels.x - 13.0f) < 0.01f);
    // Every piece lies inside its band, where every renderer draws the same.
    for (const DecorationInstance& piece : pieces) {
        CHECK(piece.topLeftInPixels.x >= piece.bandTopLeftInPixels.x);
        CHECK(piece.bottomRightInPixels.x <= piece.bandBottomRightInPixels.x);
    }
}

TEST(aPieceOfLineIsKeptOnlyWhereItIsTheLinesFullHeight)
{
    // Two stems through a two-row line, the left one hooking right along the
    // bottom row: between their cuts four pixels stand in the top row and one
    // in the bottom. The bottom row's pixel is a speck, and the top row's four
    // are a sliver half the line's height with no full-height line under them,
    // so the piece goes from both rows rather than leaving a half-height stub.
    const SkipInkRule rule { .left = 0, .top = 12, .right = 24, .bottom = 14 };
    Sheet hooked(24, 20);
    hooked.paint(4, 4, 6, 16);
    hooked.paint(6, 13, 9, 14);
    hooked.paint(12, 4, 14, 16);
    SkipInkQuery between = query(hooked, rule, 1.0f);
    between.shortestPiece = 2.0f;
    const auto cut = cutOf(hooked, rule, between);
    CHECK_ROW(cut[0], std::string("---############---------"));
    CHECK_ROW(cut[1], std::string("---############---------"));
    // A stroke resting on a four-row line notches its top rows; the line runs
    // on at full height either side, so the notch stays a notch.
    const SkipInkRule thick { .left = 0, .top = 12, .right = 24, .bottom = 16 };
    Sheet resting(24, 24);
    resting.paint(10, 6, 14, 13);
    SkipInkQuery notch = query(resting, thick, 1.0f);
    notch.shortestPiece = 4.0f;
    const auto notched = cutOf(resting, thick, notch);
    CHECK_ROW(notched[0], std::string("---------######---------"));
    CHECK_ROW(notched[1], std::string("---------######---------"));
    CHECK_ROW(notched[2], std::string(24, '-'));
}

TEST(aStretchRunningIntoTheNextCellIsJudgedByItsWholeLength)
{
    // A stem near this cell's right edge, and the next cell's stem near its
    // left: the stretch between their cuts crosses the edge, and only the two
    // cells' ink together says it is a speck. Without the neighbour's stem
    // the same stretch runs on as line.
    const SkipInkRule rule { .left = 0, .top = 12, .right = 16, .bottom = 14 };
    Sheet sheet(16, 20);
    sheet.paint(11, 4, 12, 16);
    SkipInkQuery alone = query(sheet, rule, 1.0f);
    alone.shortestPiece = 4.0f;
    alone.continuesRight = true;
    CHECK_ROW(cutOf(sheet, rule, alone)[0], std::string("----------###---"));
    // The two cells' glyphs a box apart in one sheet, each source reading its
    // own box: the neighbour's a box to the right of the rule.
    Sheet shared(32, 20);
    shared.paint(11, 4, 12, 16);
    shared.paint(17, 4, 18, 16);
    SkipInkQuery joined = query(shared, rule, 1.0f);
    joined.shortestPiece = 4.0f;
    joined.continuesRight = true;
    joined.right = identity(shared, rule, -1.0f);
    joined.right.inkLeft = 16.0f;
    joined.right.inkRight = 32.0f;
    joined.right.slotLeft = 16.0f;
    joined.right.slotRight = 32.0f;
    joined.own.slotRight = 16.0f;
    CHECK_ROW(cutOf(shared, rule, joined)[0], std::string("----------######"));
}

TEST(aStrokeTouchingOnlyTheTopOfAThickRuleNotchesIt)
{
    // A CJK stroke bottoming out on a four-row rule's top row cuts that row
    // and the one beneath it, the reach of touching; the rows past that stand.
    Sheet sheet(24, 24);
    sheet.paint(4, 6, 20, 13);
    const SkipInkRule rule { .left = 0, .top = 12, .right = 24, .bottom = 16 };
    const auto cut = cutOf(sheet, rule, query(sheet, rule, 1.0f));
    CHECK_ROW(cut[0], std::string("---##################---"));
    CHECK_ROW(cut[1], std::string("---##################---"));
    CHECK_ROW(cut[2], std::string(24, '-'));
    CHECK_ROW(cut[3], std::string(24, '-'));
}

TEST(halfCoverageIsInkAndAFainterStrokeLeavesTheRuleWhole)
{
    // A stroke below half coverage throughout touches nothing: the rule under
    // it stands. One at half is ink, half a pixel wide, and with no padding
    // the cut is that half of the pixel under it.
    Sheet sheet(16, 16);
    sheet.paint(4, 4, 5, 13, 127);
    sheet.paint(10, 4, 11, 13, 128);
    const SkipInkRule rule { .left = 0, .top = 12, .right = 16, .bottom = 13 };
    const auto cut = cutOf(sheet, rule, query(sheet, rule, 0.0f));
    CHECK_ROW(cut[0], std::string("----------+-----"));
    const auto [left, right] = onlySpanOf(sheet, rule, query(sheet, rule, 0.0f));
    CHECK(std::fabs(left - 10.249f) < 0.01f);
    CHECK(std::fabs(right - 10.751f) < 0.01f);
}

TEST(inkPastTheSlotsEdgeBelongsToAnotherGlyph)
{
    // The sheet packs glyphs side by side: ink one texel past the slot is the
    // neighbour in the sheet, not anything over this cell.
    Sheet sheet(32, 16);
    sheet.paint(16, 0, 17, 16);
    const SkipInkRule rule { .left = 0, .top = 12, .right = 16, .bottom = 13 };
    SkipInkQuery inSlot = query(sheet, rule, 4.0f);
    inSlot.own.slotRight = 16.0f;
    CHECK_ROW(cutOf(sheet, rule, inSlot)[0], std::string(16, '-'));
    inSlot.own.slotRight = 17.0f;
    CHECK_ROW(cutOf(sheet, rule, inSlot)[0], std::string("------------####"));
}

TEST(aNeighboursDescenderCutsAcrossTheCellEdge)
{
    // A descender at the left edge of the right neighbour's box: its padding
    // reaches back into this cell's rule.
    Sheet sheet(16, 16);
    sheet.paint(0, 4, 2, 14);
    const SkipInkRule rule { .left = 0, .top = 12, .right = 16, .bottom = 13 };
    SkipInkQuery neighbour;
    neighbour.right = identity(sheet, rule, -1.0f);
    neighbour.reachAcross = 3.0f;
    neighbour.reachUpDown = skipInkContactInDevicePixels;
    CHECK_ROW(cutOf(sheet, rule, neighbour)[0], std::string("-------------###"));
}

TEST(aRuleThatIsNotTouchedFromAboveIgnoresInkAboveIt)
{
    // A rich-text rule hanging a single row under the baseline: ink above its
    // top row does not touch it, ink in its rows does.
    Sheet sheet(16, 16);
    sheet.paint(2, 4, 4, 12);
    sheet.paint(10, 4, 11, 13);
    const SkipInkRule rule { .left = 0, .top = 12, .right = 16, .bottom = 13 };
    SkipInkQuery richText = query(sheet, rule, 1.0f);
    richText.isTouchedFromAbove = false;
    CHECK_ROW(cutOf(sheet, rule, richText)[0], std::string("---------###----"));
}

TEST(aRecordReadsLikeTheCoverageItWasRecordedFrom)
{
    // A GPU backend's record of a sheet: written over the region an upload
    // writes, so a reused slot's previous ink is gone, and clipped to the sheet.
    const int width = 70;
    const int height = 4;
    SkipInkRecord record(width, height);
    // Every texel ink to begin with, so what an upload leaves alone shows.
    const std::vector<std::uint8_t> solid(static_cast<std::size_t>(width * height), 255);
    record.record(0, 0, width, height, solid.data(), static_cast<std::size_t>(width), 1, 0);
    // Two bytes a texel, coverage in the second: the first is ignored.
    std::vector<std::uint8_t> upload(static_cast<std::size_t>(8 * 2 * 2), 255);
    for (int x = 0; x < 8; ++x) {
        upload[static_cast<std::size_t>(x * 2 + 1)] = x == 2 ? 200 : 0;
        upload[static_cast<std::size_t>(16 + x * 2 + 1)] = x == 5 ? 128 : 0;
    }
    record.record(62, 1, 8, 2, upload.data(), 16, 2, 1);
    SkipInkPlanes planes;
    planes.mono = skipInkPlaneOf(&record);
    CHECK_EQ(planes.mono.stride, width);
    // Read back through a rule over each row of the region, padding none.
    const auto row = [&](int y) {
        const SkipInkRule rule { .left = 62, .top = static_cast<float>(y), .right = 70, .bottom = static_cast<float>(y + 1) };
        SkipInkQuery exact;
        exact.own.inkLeft = 62;
        exact.own.inkRight = 70;
        exact.own.bandTop = static_cast<float>(y);
        exact.own.bandBottom = static_cast<float>(y + 1);
        exact.own.slotLeft = 62;
        exact.own.slotTop = static_cast<float>(y);
        exact.own.slotRight = 70;
        exact.own.slotBottom = static_cast<float>(y + 1);
        exact.own.isPresent = true;
        SkipInkScratch scratch;
        markSkipInkCuts(planes, exact, rule, scratch.cut, scratch);
        return picture(rule, scratch.cut)[0];
    };
    // The coverage the upload carried, not a verdict on it: the texel at 200
    // is ink 0.78 of a pixel wide, and the one at 128 half a pixel.
    CHECK_ROW(row(1), std::string("--+-----"));
    CHECK_ROW(row(2), std::string("-----+--"));
    // Rows the upload did not write keep what was there.
    CHECK_ROW(row(0), std::string("########"));
    record.clear(60, 0, 20, 1);
    CHECK_ROW(row(0), std::string("--------"));
    // A region reaching past the sheet's right edge writes what is on the
    // sheet and drops the rest: texels 66 to 69 of the eight from 66, ink only
    // at 68, while 62 to 65 keep what was there.
    record.record(66, 3, 8, 1, upload.data(), 16, 2, 1);
    CHECK_ROW(row(3), std::string("####--+-"));
}

TEST(aStrokeAtASubpixelPhaseIsClearedByExactlyThePaddingOnBothSides)
{
    // A two-pixel stem drawn at every tenth of a pixel's phase: its coverage
    // shares its first and last texels with the background. The cut is the
    // stem from where that coverage puts its edges, and the padding either
    // side of them exactly, whatever the phase and the padding.
    const SkipInkRule rule { .left = 0, .top = 12, .right = 32, .bottom = 13 };
    for (int tenth = 0; tenth < 10; ++tenth) {
        const float phase = static_cast<float>(tenth) / 10.0f;
        Sheet sheet(32, 16);
        const auto byte = [](float fraction) { return static_cast<std::uint8_t>(std::lround(fraction * 255.0f)); };
        sheet.paint(14, 4, 15, 13, byte(1.0f - phase));
        sheet.paint(15, 4, 16, 13, 255);
        if (phase > 0.0f)
            sheet.paint(16, 4, 17, 13, byte(phase));
        for (float padding : { 0.0f, 1.0f, 1.5f, 2.0f, 2.5f, 3.5f }) {
            const auto [left, right] = onlySpanOf(sheet, rule, query(sheet, rule, padding));
            const std::string where = "phase " + std::to_string(phase) + " padding " + std::to_string(padding);
            CHECK_MSG(std::fabs(left - (14.0f + phase - padding)) < 0.01f, (where + " left " + std::to_string(left)).c_str());
            CHECK_MSG(std::fabs(right - (16.0f + phase + padding)) < 0.01f, (where + " right " + std::to_string(right)).c_str());
        }
    }
}

TEST(aCutIsTheSameWhereverItsCellFallsOnThePixelGrid)
{
    // Two hundred cells at a terminal's fractional pitch from a fractional
    // origin, each with a stem two texels wide, mapped onto the sheet as the
    // builder maps a cell's glyph box: exact in arithmetic, a hair off in
    // floats. The cut is the stem and the padding either side in every cell,
    // centred on the stem, never a pixel more on one side than the other.
    Sheet sheet(128, 16);
    sheet.paint(106, 4, 108, 13);
    const float pitch = 16.8f;
    for (int cell = 0; cell < 200; ++cell) {
        const float cellLeft = 12.4f + static_cast<float>(cell) * pitch;
        const SkipInkRule rule { .left = cellLeft, .top = 12, .right = cellLeft + pitch, .bottom = 13 };
        SkipInkQuery mapped = query(sheet, rule, 2.5f);
        // Pixel x shows texel x - floor(cellLeft) + 100.
        mapped.own.inkLeft = 100.0f + (cellLeft - std::floor(cellLeft));
        mapped.own.inkRight = mapped.own.inkLeft + pitch;
        const auto [left, right] = onlySpanOf(sheet, rule, mapped);
        const float stemLeft = std::floor(cellLeft) + 6.0f;
        CHECK_MSG(std::fabs(left - (stemLeft - 2.5f)) < 0.001f && std::fabs(right - (stemLeft + 2.0f + 2.5f)) < 0.001f,
            ("cell " + std::to_string(cell) + " cut " + std::to_string(left) + ".." + std::to_string(right)).c_str());
    }
}

TEST(theKeptPiecesAreExactlyTheRuleLessItsCut)
{
    // A rule with fractional ends, cut through both rows by a descender and
    // notched in its top row by a stroke beside it at a subpixel phase: each
    // row's pieces and cut spans tile the rule exactly, the rows that keep the
    // same share pieces, and a piece's end is a cut end exactly where the cut
    // put it.
    Sheet sheet(32, 20);
    sheet.paint(10, 4, 12, 14);
    sheet.paint(20, 6, 22, 12, 255);
    sheet.paint(22, 6, 23, 12, 102);
    const SkipInkRule rule { .left = 0.25f, .top = 12, .right = 30.5f, .bottom = 14 };
    SkipInkScratch scratch;
    REQUIRE(markSkipInkCuts(sheet.planes(), query(sheet, rule, 1.0f), rule, scratch.cut, scratch));
    struct Piece {
        float left, top, right, bottom;
        std::uint32_t cutEnds;
    };
    std::vector<Piece> pieces;
    forEachKeptPiece(rule, scratch.cut, scratch, [&](float left, float top, float right, float bottom, std::uint32_t cutEnds) {
        pieces.push_back({ left, top, right, bottom, cutEnds });
    });
    for (int row = 0; row < rule.rows(); ++row) {
        const float y = static_cast<float>(rule.firstRow() + row) + 0.5f;
        float keptLength = 0.0f;
        for (const Piece& piece : pieces) {
            if (y < piece.top || y >= piece.bottom)
                continue;
            keptLength += piece.right - piece.left;
            CHECK(skipInkCutCoverage(scratch.cut.row(row), piece.left, piece.right) < 0.0001f);
            CHECK(((piece.cutEnds & skipInkCutLeftEnd) != 0) == (piece.left > rule.left));
            CHECK(((piece.cutEnds & skipInkCutRightEnd) != 0) == (piece.right < rule.right));
        }
        const float cutLength = skipInkCutCoverage(scratch.cut.row(row), rule.left, rule.right) * (rule.right - rule.left);
        CHECK(std::fabs(keptLength + cutLength - (rule.right - rule.left)) < 0.001f);
    }
    // Two pieces across the bottom row, three across the top.
    REQUIRE(pieces.size() == 5);
    CHECK(pieces.front().left == 0.25f);
    CHECK(pieces.back().right == 30.5f);
}

UBIQUITTY_TEST_MAIN()
