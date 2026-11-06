// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include <algorithm>
#include <span>

// test_cell_decisions — pins the shared per-cell render styling helpers that
// the macOS Metal renderer and the portable buildCellInstances now route
// through. Closing renderer forks made these the single source of truth, so
// this test locks their behavior independently of either caller: link-hover
// color membership and ligature foreground-boundary classes.

#include "Core/Rendering/CellDecisions.h"
#include "Core/Rendering/GlyphSlotPolicy.h"
#include "Core/Rendering/ShapedGlyphGroups.h"

#include <string>

#include "Core/TerminalOperations.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace Ubiquitty::Rendering;

TEST(shapedGlyphGroupsPreserveCarrierInk)
{
    std::array<ShapedGlyphExtent, 5> glyphs { {
        { 0, 1, 0.1, 0.9 },
        { 1, 1, 1.1, 3.9 },
        { 2, 1, 0, 0 },
        { 3, 1, 0, 0 },
        { 4, 1, 4.1, 4.9 },
    } };
    std::array<unsigned char, 5> boundaries;
    std::array<ShapedGlyphGroup, 5> groups;
    const int count = partitionShapedGlyphGroups(glyphs, 5, boundaries, groups);
    CHECK(count == 3);
    CHECK(groups[0].firstCell == 0 && groups[0].cellCount == 1);
    CHECK(groups[1].firstCell == 1 && groups[1].cellCount == 3);
    CHECK(groups[2].firstCell == 4 && groups[2].cellCount == 1);
}

TEST(shapedGlyphGroupsMergeTransitiveOverhangs)
{
    std::array<ShapedGlyphExtent, 3> glyphs { {
        { 0, 1, -0.1, 1.2 },
        { 1, 1, 1.1, 2.2 },
        { 2, 1, 2.1, 3.2 },
    } };
    std::array<unsigned char, 3> boundaries;
    std::array<ShapedGlyphGroup, 3> groups;
    CHECK(partitionShapedGlyphGroups(glyphs, 3, boundaries, groups) == 1);
    CHECK(groups[0].firstCell == 0 && groups[0].cellCount == 3);
    CHECK(partitionShapedGlyphGroups(glyphs, 3, boundaries, std::span(groups).first(2)) == 0);
}

TEST(shapedGlyphGroupsKeepMultiCellSourceClusters)
{
    std::array<ShapedGlyphExtent, 1> glyphs { { { 1, 3, 1.2, 1.8 } } };
    std::array<unsigned char, 5> boundaries;
    std::array<ShapedGlyphGroup, 5> groups;
    CHECK(partitionShapedGlyphGroups(glyphs, 5, boundaries, groups) == 3);
    CHECK(groups[1].firstCell == 1 && groups[1].cellCount == 3);
}

TEST(testLinkHoverContainsCell)
{
    CHECK(!linkHoverContainsCell(false, 0, 4, 2, 6, 0));

    // Regex/path hover: no id, so the captured column band is the membership.
    CHECK(linkHoverContainsCell(true, 0, 4, 2, 6, 0));
    CHECK(!linkHoverContainsCell(true, 0, 1, 2, 6, 0));
    CHECK(!linkHoverContainsCell(true, 0, 7, 2, 6, 0));

    // OSC 8 hover: must be both inside the captured band and on the hovered id.
    CHECK(linkHoverContainsCell(true, 42, 4, 2, 6, 42));
    CHECK(!linkHoverContainsCell(true, 42, 4, 2, 6, 99));
    CHECK(!linkHoverContainsCell(true, 42, 9, 2, 6, 42));
}

// The per-cell strip slice is what keeps per-cell color decisions (link-hover,
// selection recolor) alive through ligature shaping: every covered cell emits
// its OWN quad over its own x-slice of the shaped slot, colored with that
// cell's resolved colors. These invariants are exactly what a regression to
// "one quad from the run lead" (the per-cell color collapse this extraction
// pins against) would break first.
TEST(testLigatureStripCellSlice)
{
    // A 4-cell run: advance 10 atlas units/cell, overhang margin 3, inked
    // extent 46. Cell c's atlas segment = [3 + (c-runStart)*10, +10).
    const int runStart = 2, runEnd = 6;
    const float margin = 3.0f, advance = 10.0f, usedWidth = 46.0f;
    auto segmentLeft = [&](int c) { return margin + (c - runStart) * advance; };
    // Shaped alone, so both margins hold this run's own overhang and the outer
    // cells reach the slot edges.
    const LigatureStripContextInsets insets = ligatureStripContextInsets(0, 0, runEnd - runStart, advance, margin, usedWidth);
    CHECK(insets.leftInAtlasPixels == 0.0f && insets.rightInAtlasPixels == 0.0f);

    LigatureStripSlice previous { 0.0f, 0.0f };
    float covered = 0.0f;
    for (int c = runStart; c < runEnd; ++c) {
        const LigatureStripSlice s = ligatureStripCellSlice(c, runStart, runEnd, segmentLeft(c), segmentLeft(c) + advance, usedWidth, insets.leftInAtlasPixels, insets.rightInAtlasPixels);
        // Ordered, in-slot.
        CHECK(s.atlasLeft <= s.atlasRight);
        CHECK(s.atlasLeft >= 0.0f && s.atlasRight <= usedWidth);
        // Every covered cell gets a NON-EMPTY slice of its own — the property
        // a lead-cell-only emit (per-cell color collapse) breaks first.
        CHECK(s.atlasRight > s.atlasLeft);
        // First/last absorb the overhang margins; interior boundaries land on
        // the cell segments, so adjacent slices tile with no gap or overlap.
        if (c == runStart)
            CHECK(s.atlasLeft == 0.0f);
        else
            CHECK(s.atlasLeft == previous.atlasRight);
        if (c + 1 == runEnd)
            CHECK(s.atlasRight == usedWidth);
        else
            CHECK(s.atlasRight == segmentLeft(c) + advance);
        covered += s.atlasRight - s.atlasLeft;
        previous = s;
    }
    // The slices exactly cover the slot's inked extent.
    CHECK(covered == usedWidth);

    // A single-cell run spans the whole slot.
    const LigatureStripSlice single = ligatureStripCellSlice(0, 0, 1, margin, margin + advance, 16.0f, 0.0f, 0.0f);
    CHECK(single.atlasLeft == 0.0f && single.atlasRight == 16.0f);

    // Clamps: a segment past the inked extent clips to it; an inverted pair
    // collapses to empty instead of a negative-width quad.
    const LigatureStripSlice clipped = ligatureStripCellSlice(3, 0, 8, 40.0f, 50.0f, 46.0f, 0.0f, 0.0f);
    CHECK(clipped.atlasLeft == 40.0f && clipped.atlasRight == 46.0f);
    const LigatureStripSlice inverted = ligatureStripCellSlice(3, 0, 8, 50.0f, 40.0f, 46.0f, 0.0f, 0.0f);
    CHECK(inverted.atlasLeft == inverted.atlasRight);
}

// A run longer than `ligatureMaxCells` is shaped in chunks, and each chunk is
// shaped with its neighbours as context so the font sees across the split.
// Those context glyphs are rasterised into the margins, and the margin is also
// where an outer cell reaching the slot edge looks for its own overhang — so a
// chunk seam is the one place where reaching the edge paints a glyph the
// neighbouring chunk already drew, and the seam column draws bolder than the
// same character elsewhere on the row.
TEST(testLigatureStripContextInsetsHoldBackAChunkSeam)
{
    const int runLength = 32;
    const float margin = 3.0f, advance = 10.0f;
    // The slot holds the run plus a margin either side, and the context glyphs
    // shaped beyond the run fill those margins with ink.
    const float usedWidth = margin + runLength * advance + margin;

    // Leading context only: the left edge is held back to the run's first
    // cell, the right edge keeps the slot.
    const LigatureStripContextInsets afterSplit = ligatureStripContextInsets(4, 0, runLength, advance, margin, usedWidth);
    CHECK(afterSplit.leftInAtlasPixels == margin);
    CHECK(afterSplit.rightInAtlasPixels == 0.0f);

    // Trailing context only: the mirror.
    const LigatureStripContextInsets beforeSplit = ligatureStripContextInsets(0, 4, runLength, advance, margin, usedWidth);
    CHECK(beforeSplit.leftInAtlasPixels == 0.0f);
    CHECK(beforeSplit.rightInAtlasPixels == margin);

    // A chunk in the middle of a long stretch has both, so it draws its cells
    // and nothing else.
    const LigatureStripContextInsets interior = ligatureStripContextInsets(4, 4, runLength, advance, margin, usedWidth);
    CHECK(interior.leftInAtlasPixels == margin);
    CHECK(interior.rightInAtlasPixels == margin);

    // The slot's own clamp still wins: a strip whose ink was already cut to
    // the slot has no margin left to hold back, and an inset past that cut
    // would take the slice off the run's own cells.
    const float cut = margin + runLength * advance - 4.0f;
    const LigatureStripContextInsets clamped = ligatureStripContextInsets(4, 4, runLength, advance, margin, cut);
    CHECK(clamped.rightInAtlasPixels == 0.0f);

    // The seam cells take the insets above, and the tiling the slice test pins
    // holds against them: no gap, no overlap, and no reach past the run's ink.
    const int runStart = 0, runEnd = runLength;
    auto segmentLeft = [&](int c) { return margin + c * advance; };
    float previousRight = interior.leftInAtlasPixels;
    for (int c = runStart; c < runEnd; ++c) {
        const LigatureStripSlice s = ligatureStripCellSlice(c, runStart, runEnd, segmentLeft(c), segmentLeft(c) + advance,
            usedWidth, interior.leftInAtlasPixels, interior.rightInAtlasPixels);
        CHECK(s.atlasRight > s.atlasLeft);
        CHECK(s.atlasLeft == previousRight);
        previousRight = s.atlasRight;
    }
    CHECK(previousRight == usedWidth - interior.rightInAtlasPixels);
    CHECK(previousRight == margin + runLength * advance);
}

// Ligature strips must stay inside one glyph-shape class. Paint-only changes
// should not split the run, or later color edits would make otherwise stable
// characters pick a different phase.
TEST(testLigatureCellsHaveSameShape)
{
    Ubiquitty::Cell a {};
    Ubiquitty::Cell b {};
    a.pen.foregroundIndex = Ubiquitty::colorDefault;
    a.pen.backgroundIndex = Ubiquitty::colorDefault;
    a.pen.underlineIndex = Ubiquitty::colorDefault;
    a.pen.attributes = Ubiquitty::VT::Attribute::Bold;
    a.pen.extendedAttributes.setWeight(4);
    Ubiquitty::penSetFontStyle(&a.pen, Ubiquitty::VT::fontPrimary);
    Ubiquitty::penSetScript(&a.pen, Ubiquitty::VT::scriptNone);
    Ubiquitty::penSetProportional(&a.pen, false);
    b = a;
    CHECK(ligatureCellsHaveSameShape(&a, &b));

    b.pen.foregroundIndex = 1;
    b.pen.backgroundIndex = 2;
    b.pen.attributes = { Ubiquitty::VT::Attribute::Bold, Ubiquitty::VT::Attribute::Reverse, Ubiquitty::VT::Attribute::Dim };
    CHECK(ligatureCellsHaveSameShape(&a, &b));

    b = a;
    Ubiquitty::penSetScript(&b.pen, Ubiquitty::VT::scriptSuper);
    CHECK(!ligatureCellsHaveSameShape(&a, &b));

    b = a;
    Ubiquitty::penSetFontStyle(&b.pen, Ubiquitty::VT::fontFraktur);
    CHECK(!ligatureCellsHaveSameShape(&a, &b));

    b = a;
    Ubiquitty::penSetProportional(&b.pen, true);
    CHECK(!ligatureCellsHaveSameShape(&a, &b));

    b = a;
    b.pen.extendedAttributes.setWeight(5);
    CHECK(!ligatureCellsHaveSameShape(&a, &b));
}

// Eligibility gates which cells can join a shaped run (and its context):
// printable base-font ASCII only, none of the attributes that force the
// per-glyph path.
TEST(testLigatureCellEligible)
{
    Ubiquitty::Cell c {};
    c.codePoint = '+';
    CHECK(ligatureCellEligible(&c));
    CHECK(!ligatureCellEligible(nullptr));

    c.codePoint = 0x1F; // below printable ASCII
    CHECK(!ligatureCellEligible(&c));
    c.codePoint = 0x7F; // above printable ASCII
    CHECK(!ligatureCellEligible(&c));
    c.codePoint = 0x2192; // → non-ASCII
    CHECK(!ligatureCellEligible(&c));
    // A space joins a run. Excluding it gives each word its own run and its
    // own subpixel phase, which is the identity the strip cache wants, and
    // puts every word through the per-cell owned-ink slice path that carries
    // only operator runs at this eligibility. The two populations differ by
    // two orders of magnitude on a screen of prose.
    c.codePoint = ' ';
    CHECK(ligatureCellEligible(&c));

    // What blocks is what changes the GLYPH: a wide cell and its continuation,
    // an emoji, hidden text, and the italic face the ligature fonts do not
    // cover.
    for (auto attribute : { Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::WideContinuation, Ubiquitty::VT::Attribute::Emoji, Ubiquitty::VT::Attribute::ForceEmojiPresentation, Ubiquitty::VT::Attribute::Hidden, Ubiquitty::VT::Attribute::Italic }) {
        Ubiquitty::Cell blocked {};
        blocked.codePoint = '+';
        blocked.pen.attributes = attribute;
        CHECK(!ligatureCellEligible(&blocked));
    }
    // A decoration is paint over the cell, not a different glyph, and its pass
    // reads the pen rather than the glyph route — so a decorated cell shapes
    // with its word and is decorated on top. Ejecting it stranded the
    // fragments either side: an underlined `m` in `tmux` left `t` alone, below
    // the two-cell floor, and `ux` shaping as a run of its own, so the healed
    // form of every letter changed.
    for (auto attribute : { Ubiquitty::VT::Attribute::Underline, Ubiquitty::VT::Attribute::Strike }) {
        Ubiquitty::Cell decorated {};
        decorated.codePoint = '+';
        decorated.pen.attributes = attribute;
        CHECK(ligatureCellEligible(&decorated));
    }
    // Bold does not block eligibility (it selects the bold ligature face).
    Ubiquitty::Cell bold {};
    bold.codePoint = '+';
    bold.pen.attributes = Ubiquitty::VT::Attribute::Bold;
    CHECK(ligatureCellEligible(&bold));
}

// The shaping-context walk: how many same-shape eligible cells extend from a
// point. This is what lets a run split by the shaped-run length cap hand
// CoreText the neighbouring cells, so the font's contextual rules see one
// long operator run instead of ligating the rump at each split edge (the
// git-diffstat "+++…" first-three-ligated bug).
TEST(testLigatureContextLength)
{
    std::vector<Ubiquitty::Cell> cells(40);
    for (auto& cell : cells)
        cell.codePoint = '+';
    // The walk reads the distilled row, so the case distils after every edit —
    // asserting against a stale distillation would pass while the arms shaped
    // from something else.
    std::vector<Ubiquitty::Rendering::LigatureCell> row(40);
    const auto distil = [&] { distilLigatureRow(std::span<const Ubiquitty::Cell>(cells), std::span<LigatureCell>(row)); };
    distil();
    const Ubiquitty::Rendering::LigatureCell* r = row.data();
    const Ubiquitty::Rendering::LigatureCell ref = row[0];

    // The chunk-split scenario: a run capped at [0, 32) asks for trailing
    // context from column 32 — the stretch continues, so the walk fills the cap.
    CHECK(ligatureContextLength(r, 40, 32, +1, 8, ref) == 8);
    // And the next chunk's leading context, walking backwards from column 31.
    CHECK(ligatureContextLength(r, 40, 31, -1, 8, ref) == 8);

    // Row edges clamp the walk.
    CHECK(ligatureContextLength(r, 40, 38, +1, 8, ref) == 2);
    CHECK(ligatureContextLength(r, 40, 1, -1, 8, ref) == 2);
    CHECK(ligatureContextLength(r, 40, -1, -1, 8, ref) == 0);
    CHECK(ligatureContextLength(r, 40, 40, +1, 8, ref) == 0);

    // An ineligible cell ends the context (mirrors the run's own boundary).
    cells[34].codePoint = 0x2192;
    distil();
    CHECK(ligatureContextLength(r, 40, 32, +1, 8, ref) == 2);
    cells[34].codePoint = '+';
    distil();

    // A shape boundary (bold flip) ends the context too.
    cells[35].pen.attributes = Ubiquitty::VT::Attribute::Bold;
    distil();
    CHECK(ligatureContextLength(r, 40, 32, +1, 8, ref) == 3);
    cells[35].pen.attributes = {};
    distil();

    // Cap of zero → no context, regardless of the stretch.
    CHECK(ligatureContextLength(r, 40, 32, +1, 0, ref) == 0);
}

// The run walk is seam contract: where a run starts, where a long stretch
// chunks, and how much context rides along must be one answer for every
// backend, or two arms split the same row differently and disagree on exactly
// the rows a comparison is least likely to hold. This pins the walk against a
// hand-derived candidate list on the shapes that decide it.
TEST(testIdentifyLigatureRuns)
{
    std::vector<Ubiquitty::Cell> cells(80);
    std::vector<Ubiquitty::Rendering::LigatureCell> row(80);
    const auto distil = [&] { distilLigatureRow(std::span<const Ubiquitty::Cell>(cells), std::span<LigatureCell>(row)); };
    std::vector<LigatureRunCandidate> found;
    const auto walk = [&] {
        found.clear();
        identifyLigatureRuns(row.data(), static_cast<int>(row.size()), LigatureParticipation::OperatorRuns, LigatureFaceMixing::SplitByFace,
            [&](const LigatureRunCandidate& candidate) { found.push_back(candidate); });
    };

    // A same-shape operator stretch one full context beyond the cap chunks
    // into two: [0,cap) with trailing context into the next chunk, then the
    // remainder with leading context reaching back across the split. Both
    // lengths come from the constants rather than from numbers, because a
    // fixture that fixes them tests the cap's current value: at a shorter cap
    // the remainder stops covering a whole context and the seam reads as
    // truncated when only the fixture is.
    const int stretch = ligatureMaxCells + ligatureContextCells;
    for (int i = 0; i < stretch; ++i)
        cells[static_cast<std::size_t>(i)].codePoint = '+';
    distil();
    walk();
    // The row's blanks distil as the spaces they look like, so the stretch
    // reaches the row edge and the second chunk fills to the cap with space
    // cells; only chunks holding an operator report under this participation.
    CHECK(found.size() == 2);
    CHECK(found[0].startColumn == 0);
    CHECK(found[0].length == ligatureMaxCells);
    CHECK(found[0].leftContext == 0);
    CHECK(found[0].rightContext == ligatureContextCells);
    CHECK(found[1].startColumn == ligatureMaxCells);
    CHECK(found[1].length == ligatureMaxCells);
    CHECK(found[1].leftContext == ligatureContextCells);
    CHECK(found[1].rightContext == ligatureContextCells);

    // Letters alone are a run without a candidate: shaped to itself, so not
    // worth a strip and not reported.
    for (auto& cell : cells)
        cell.codePoint = 0;
    cells[0].codePoint = 'a';
    cells[1].codePoint = 'b';
    cells[2].codePoint = 'c';
    distil();
    walk();
    CHECK(found.empty());

    // One operator inside the letters makes the stretch a candidate, and the
    // stretch runs through the trailing blanks to the chunk cap: the erased
    // cells beside the text are whitespace to the reader, so they are
    // whitespace to the shaper.
    cells[1].codePoint = '=';
    distil();
    walk();
    CHECK(found.size() == 1);
    CHECK(found[0].startColumn == 0);
    CHECK(found[0].length == ligatureMaxCells);
    CHECK(found[0].rightContext == ligatureContextCells);

    // A lone operator among blanks still reports — the blanks beside it are
    // its shaping context now, exactly as typed spaces would be. The
    // two-cell floor guards a run of cells, and a space-joined stretch is
    // one.
    cells[0].codePoint = 0;
    cells[2].codePoint = 0;
    distil();
    walk();
    CHECK(found.size() == 1);

    // A shape boundary splits the stretch, and only the half holding the
    // operator reports; context stops at the boundary from both sides.
    for (int i = 0; i < 8; ++i)
        cells[i].codePoint = (i == 2) ? '=' : 'x';
    for (int i = 8; i < 80; ++i)
        cells[i].codePoint = 0;
    cells[4].pen.attributes = Ubiquitty::VT::Attribute::Bold;
    cells[5].pen.attributes = Ubiquitty::VT::Attribute::Bold;
    cells[6].pen.attributes = Ubiquitty::VT::Attribute::Bold;
    cells[7].pen.attributes = Ubiquitty::VT::Attribute::Bold;
    distil();
    walk();
    CHECK(found.size() == 1);
    CHECK(found[0].startColumn == 0);
    CHECK(found[0].length == 4);
    CHECK(found[0].leftContext == 0);
    CHECK(found[0].rightContext == 0);
}

TEST(testLigatureOperatorCharacterBitmap)
{
    // The predicate is a hand-written bitmap, which is the kind of thing that is
    // wrong in one bit and looks right. Checked against the character list it
    // encodes, over the whole domain a cell's codePoint can hold — a spot check
    // of the members alone would pass a mask with a spurious extra bit set,
    // which is the way this goes wrong.
    const std::string members = "=<>-!&|+*/~^%:;.?@#\\_";
    for (std::uint32_t codePoint = 0; codePoint <= 0x10FFFF; ++codePoint) {
        const bool isListed = codePoint < 128 && members.find(static_cast<char>(codePoint)) != std::string::npos;
        CHECK(ligatureOperatorCharacter(codePoint) == isListed);
    }
    // Named individually as well, so a change to the character list has to edit
    // this line and say what it is doing rather than silently agreeing with a
    // mask that moved with it.
    CHECK(members.size() == 21);
}

// The participation criterion, pinned on both sides of every clause: equal
// mappings keep the operator economy, any glyph disagreement or a count
// change (a substitution inside plain letters) demands every eligible run,
// and an unanswerable probe stays on the economy — a backend that cannot
// shape must not be promoted to a mode built for backends that can.
TEST(testDecideLigatureParticipation)
{
    using Ubiquitty::Rendering::decideLigatureParticipation;
    using Ubiquitty::Rendering::LigatureParticipation;
    const std::uint32_t same[] = { 5, 9, 12, 40 };
    const std::uint32_t healedOne[] = { 5, 9, 13, 40 };
    const std::uint32_t fewer[] = { 5, 9, 12 };
    CHECK(decideLigatureParticipation(same, same) == LigatureParticipation::OperatorRuns);
    CHECK(decideLigatureParticipation(healedOne, same) == LigatureParticipation::AllEligibleRuns);
    CHECK(decideLigatureParticipation(fewer, same) == LigatureParticipation::AllEligibleRuns);
    CHECK(decideLigatureParticipation({}, same) == LigatureParticipation::OperatorRuns);
    CHECK(decideLigatureParticipation(same, {}) == LigatureParticipation::OperatorRuns);
}

// The walk's two modes over one row. A letters-only stretch is the text the
// operator economy skips and a healing font must shape; the mode is the only
// input that changes, so any drift between the two walks is the gate itself.
/// One word is one run wherever it sits, so it takes its own phase.
///
/// A strip is rasterised once, at the subpixel phase its run begins on, and
/// every glyph inside is laid out from there. While a run began at the line's
/// first space, an indented word sat at a fractional offset the phase never
/// accounted for: `tmux` at indent two met a different pixel grid than `tmux`
/// at indent zero, and rendered as a visibly different word. Two indents that
/// land on one phase differed by 98 and 134 pixels; with the run starting at
/// the word they are the same image.
///
/// The run's content is what has to be indent-invariant, since the key is
/// built from it: same length, same code points, no context borrowed from the
/// spaces before it.
// A run spans the whole same-shape stretch, spaces included, so an indented
// word and a bare one begin at different columns and take different subpixel
// phases. The same word therefore meets a different pixel grid at different
// indents and renders as visibly different letterforms on a healing face.
//
// The assertions pin that behaviour. Making a space ineligible gives each
// word its own phase and its own letterforms; it also routes every word
// through the per-cell owned-ink slice path, where cells draw blank in
// proportion to how many runs a row holds. Both faults are real and they
// trade against each other at this one predicate, which is why the test
// names which side the code sits on.
TEST(aRunSpansTheStretchSoAnIndentMovesItsPhase)
{
    auto runAt = [](int indent) {
        std::vector<Ubiquitty::Cell> cells(24);
        std::vector<Ubiquitty::Rendering::LigatureCell> row(24);
        for (int i = 0; i < indent; ++i)
            cells[static_cast<std::size_t>(i)].codePoint = U' ';
        const char* word = "tmux";
        for (int i = 0; i < 4; ++i)
            cells[static_cast<std::size_t>(indent + i)].codePoint = static_cast<std::uint32_t>(word[i]);
        distilLigatureRow(std::span<const Ubiquitty::Cell>(cells), std::span<LigatureCell>(row));
        std::vector<LigatureRunCandidate> found;
        identifyLigatureRuns(row.data(), 24, LigatureParticipation::AllEligibleRuns, LigatureFaceMixing::SplitByFace,
            [&](const LigatureRunCandidate& candidate) { found.push_back(candidate); });
        return found;
    };

    const auto bare = runAt(0);
    CHECK_MSG(!bare.empty() && bare[0].startColumn == 0,
        "a word at the margin begins its run at the margin");
    const auto indented = runAt(3);
    CHECK_MSG(!indented.empty() && indented[0].startColumn == 0,
        "and an indented word's run STILL begins at the margin, because the "
        "blanks before it are spaces to the shaper — the run's start no "
        "longer moves with the indent, so neither does the word's phase");
    CHECK_MSG(indented[0].length == bare[0].length,
        "the chunking is the row's, not the indent's");
}

TEST(aBoldBoundaryEndsARunOnlyForABackendThatCannotMixFaces)
{
    std::vector<Ubiquitty::Cell> cells(12);
    std::vector<Ubiquitty::Rendering::LigatureCell> row(12);
    const char* word = "tmux";
    for (int i = 0; i < 4; ++i)
        cells[static_cast<std::size_t>(i)].codePoint = static_cast<std::uint32_t>(word[i]);
    cells[0].pen.attributes = Ubiquitty::VT::Attribute::Bold;
    distilLigatureRow(std::span<const Ubiquitty::Cell>(cells), std::span<LigatureCell>(row));

    std::vector<LigatureRunCandidate> split;
    identifyLigatureRuns(row.data(), 12, LigatureParticipation::AllEligibleRuns,
        LigatureFaceMixing::SplitByFace, [&](const LigatureRunCandidate& c) { split.push_back(c); });
    CHECK_MSG(split.size() == 1, "the bold t is one cell, below the floor, so only mux survives as a run");
    if (split.size() == 1) {
        // The run reaches the row edge: the blanks after the word are spaces
        // to the shaper now, which is what heals a word against the margin
        // the way it heals against typed spaces.
        CHECK(split[0].startColumn == 1 && split[0].length == 11);
    }

    std::vector<LigatureRunCandidate> mixed;
    identifyLigatureRuns(row.data(), 12, LigatureParticipation::AllEligibleRuns,
        LigatureFaceMixing::ShapesMixedFaces, [&](const LigatureRunCandidate& c) { mixed.push_back(c); });
    CHECK_MSG(mixed.size() == 1, "a mixing backend keeps the word whole");
    if (mixed.size() == 1) {
        CHECK_MSG(mixed[0].startColumn == 0 && mixed[0].length == 12,
            "the run covers the bold cell, the regular ones and the trailing blanks together");
    }

    // The per-cell face survives the merge, because the backend draws each cell
    // in its own face: a run that carried no face information could only be
    // drawn in one.
    CHECK_MSG((row[0].shape & ligatureShapeBold) != 0, "the bold cell still says it is bold");
    CHECK_MSG((row[1].shape & ligatureShapeBold) == 0, "its neighbour still says it is not");

    // A change that is not a face still ends the run under both rules, since no
    // shaped image can carry two sizes or two bands.
    std::vector<Ubiquitty::Cell> sized(12);
    std::vector<Ubiquitty::Rendering::LigatureCell> sizedRow(12);
    for (int i = 0; i < 4; ++i)
        sized[static_cast<std::size_t>(i)].codePoint = static_cast<std::uint32_t>(word[i]);
    penSetScript(&sized[0].pen, Ubiquitty::VT::scriptSuper);
    distilLigatureRow(std::span<const Ubiquitty::Cell>(sized), std::span<LigatureCell>(sizedRow));
    std::vector<LigatureRunCandidate> stillSplit;
    identifyLigatureRuns(sizedRow.data(), 12, LigatureParticipation::AllEligibleRuns,
        LigatureFaceMixing::ShapesMixedFaces, [&](const LigatureRunCandidate& c) { stillSplit.push_back(c); });
    CHECK_MSG(!stillSplit.empty() && stillSplit[0].startColumn == 1,
        "a superscript cell leaves the run even where faces may mix");
}

TEST(testIdentifyLigatureRunsParticipation)
{
    std::vector<Ubiquitty::Cell> cells(20);
    std::vector<Ubiquitty::Rendering::LigatureCell> row(20);
    for (int i = 0; i < 11; ++i)
        cells[static_cast<std::size_t>(i)].codePoint = static_cast<std::uint32_t>("mmm mode on"[i]);
    distilLigatureRow(std::span<const Ubiquitty::Cell>(cells), std::span<LigatureCell>(row));

    std::vector<LigatureRunCandidate> found;
    identifyLigatureRuns(row.data(), 20, LigatureParticipation::OperatorRuns, LigatureFaceMixing::SplitByFace,
        [&](const LigatureRunCandidate& candidate) { found.push_back(candidate); });
    CHECK_MSG(found.empty(), "no operator, so the economy shapes nothing");

    identifyLigatureRuns(row.data(), 20, LigatureParticipation::AllEligibleRuns, LigatureFaceMixing::SplitByFace,
        [&](const LigatureRunCandidate& candidate) { found.push_back(candidate); });
    // One run for the stretch. A space joins it, so `mmm mode on` is a single
    // eleven-cell run rather than three words — which is what puts the whole
    // line on the phase its first column decided. Dividing it at the spaces
    // would move every word onto the per-cell owned-ink slice path, where
    // cells draw blank.
    CHECK_MSG(found.size() == 2, "a healing font shapes the stretch, spaces and trailing blanks included");
    if (found.size() == 2) {
        CHECK(found[0].startColumn == 0 && found[0].length == ligatureMaxCells);
        CHECK(found[1].startColumn == ligatureMaxCells && found[1].length == 20 - ligatureMaxCells);
    }

    // A single eligible cell among blanks still shapes: the blanks are its
    // whitespace context, and the healing face draws a lone letter between
    // spaces as its own variant — the reader typing one `m` sees the same
    // letterform the sent line shows.
    std::ranges::fill(cells, Ubiquitty::Cell {});
    cells[0].codePoint = 'm';
    distilLigatureRow(std::span<const Ubiquitty::Cell>(cells), std::span<LigatureCell>(row));
    found.clear();
    identifyLigatureRuns(row.data(), 20, LigatureParticipation::AllEligibleRuns, LigatureFaceMixing::SplitByFace,
        [&](const LigatureRunCandidate& candidate) { found.push_back(candidate); });
    CHECK(!found.empty());
}

// A seam through a multi-cell cluster loses ink: the font may shape the
// cluster as zero-advance glyphs whose one carrier draws the whole image,
// and the carrier's back-reach exceeds the strip slot's one-cell margin.
// Measured live: `///` astride a cap seam draws `//`. The walk therefore
// retreats a cap cut to the rightmost boundary in the seam window that no
// cluster spans — a space flank or an operator-class change.
TEST(aChunkSeamRetreatsPastAClusterItWouldSplit)
{
    std::vector<Ubiquitty::Cell> cells(40);
    std::vector<Ubiquitty::Rendering::LigatureCell> row(40);
    const char* line = "let payload = snapshot.mime;  /// Adds o";
    for (int i = 0; i < 40; ++i)
        cells[static_cast<std::size_t>(i)].codePoint = static_cast<std::uint32_t>(line[i]);
    distilLigatureRow(std::span<const Ubiquitty::Cell>(cells), std::span<LigatureCell>(row));
    std::vector<LigatureRunCandidate> found;
    identifyLigatureRuns(row.data(), 40, LigatureParticipation::AllEligibleRuns, LigatureFaceMixing::SplitByFace,
        [&](const LigatureRunCandidate& candidate) { found.push_back(candidate); });
    bool doesAChunkBeginAtTheSlashes = false;
    for (const LigatureRunCandidate& candidate : found) {
        CHECK_MSG(candidate.startColumn != 31 && candidate.startColumn != 32,
            "no seam falls inside the three-cell cluster at columns 30..32");
        if (candidate.startColumn == 30)
            doesAChunkBeginAtTheSlashes = true;
    }
    CHECK_MSG(doesAChunkBeginAtTheSlashes,
        "the cut lands on the space flank before the cluster, so the cluster "
        "opens its chunk and shapes whole");
}

// The retreat is bounded by the seam window: a token wider than the window
// offers no safe boundary, and the cap still cuts it hard. The seam goldens'
// unbroken-token row pins what a cluster astride that seam loses.
TEST(aTokenWiderThanTheWindowStillCutsHard)
{
    std::vector<Ubiquitty::Cell> cells(40);
    std::vector<Ubiquitty::Rendering::LigatureCell> row(40);
    for (int i = 0; i < 40; ++i)
        cells[static_cast<std::size_t>(i)].codePoint = U'x';
    distilLigatureRow(std::span<const Ubiquitty::Cell>(cells), std::span<LigatureCell>(row));
    std::vector<LigatureRunCandidate> found;
    identifyLigatureRuns(row.data(), 40, LigatureParticipation::AllEligibleRuns, LigatureFaceMixing::SplitByFace,
        [&](const LigatureRunCandidate& candidate) { found.push_back(candidate); });
    CHECK_MSG(found.size() == 3 && found[0].length == ligatureMaxCells,
        "an unbroken token takes the cap where the window holds no cut");
}

// A retreat moves cells into the next chunk, and the operator economy must
// answer for the cells the chunk keeps, not the ones it gave away: a chunk
// whose only operators retreated across the seam holds none.
TEST(aRetreatedChunkRecountsItsOperators)
{
    std::vector<Ubiquitty::Cell> cells(20);
    std::vector<Ubiquitty::Rendering::LigatureCell> row(20);
    const char* line = "abcdefghij k+=======";
    for (int i = 0; i < 20; ++i)
        cells[static_cast<std::size_t>(i)].codePoint = static_cast<std::uint32_t>(line[i]);
    distilLigatureRow(std::span<const Ubiquitty::Cell>(cells), std::span<LigatureCell>(row));
    std::vector<LigatureRunCandidate> found;
    identifyLigatureRuns(row.data(), 20, LigatureParticipation::OperatorRuns, LigatureFaceMixing::SplitByFace,
        [&](const LigatureRunCandidate& candidate) { found.push_back(candidate); });
    CHECK_MSG(found.size() == 1, "the letters-only chunk left of the retreat is not worth a strip");
    if (found.size() == 1) {
        CHECK_MSG(found[0].startColumn == 12 && found[0].length == 8,
            "the operator stretch opens its own chunk at the class change and "
            "shapes whole rather than losing its head to the cap");
    }
}

// The strip plan is Core's for every arm: the grid follows the mode, the
// device clamp binds it, and a slot no device fits refuses the sheet whole.
TEST(testPlanLigatureStripSheet)
{
    using Ubiquitty::Rendering::LigatureParticipation;
    using Ubiquitty::Rendering::planLigatureStripSheet;
    const auto economy = planLigatureStripSheet(10, 20, 1, 1, LigatureParticipation::OperatorRuns, 16384);
    const auto healing = planLigatureStripSheet(10, 20, 1, 1, LigatureParticipation::AllEligibleRuns, 16384);
    CHECK(economy.valid && healing.valid);
    CHECK(economy.slotWidthInPixels == ligatureStripSlotWidthInPixels(10, 1));
    CHECK_MSG(healing.capacity > economy.capacity,
        "a healing font's prose working set gets the larger grid");

    // The clamp binds: a device whose edge holds one slot row yields a
    // single-column plan rather than an overflowing sheet.
    const auto clamped = planLigatureStripSheet(10, 20, 1, 1, LigatureParticipation::AllEligibleRuns,
        ligatureStripSlotWidthInPixels(10, 1));
    CHECK(clamped.valid);
    CHECK(clamped.columns == 1);

    // A slot alone past the device is a refusal, not a truncation.
    const auto refused = planLigatureStripSheet(10, 20, 1, 1, LigatureParticipation::OperatorRuns, 8);
    CHECK(!refused.valid);
}

// The strip sheet is sized once, for both arms. The Metal sheet measures its
// screen and the portable sheet cannot, and a rule that answered the floor to
// the one that could not measure gave the two arms capacities four times
// apart — the same screen then thrashed one cache and rattled inside the
// other, with no comparison able to name capacity as the cause.
TEST(bothArmsSizeOneStripSheet)
{
    using Ubiquitty::Rendering::planLigatureStripSheet;
    using Ubiquitty::Rendering::worstCaseGlyphSlotsForScreen;
    using Ubiquitty::Rendering::worstCaseLigatureStripsForCells;

    // A 1512x982pt screen of 8x14pt cells, which is a 16-inch display at the
    // size this font is read at.
    const int cells = worstCaseGlyphSlotsForScreen({ 1512, 982 }, { 8, 14 });
    const int strips = worstCaseLigatureStripsForCells(2 * cells);
    const auto measured = planLigatureStripSheet(15, 28, 2, 2, LigatureParticipation::AllEligibleRuns, 16384, strips);
    const auto unmeasured = planLigatureStripSheet(15, 28, 2, 2, LigatureParticipation::AllEligibleRuns, 16384);
    CHECK(measured.valid && unmeasured.valid);
    CHECK_MSG(measured.capacity == unmeasured.capacity,
        "an arm that cannot measure its screen must still land on the other arm's capacity");

    // And the sheet holds more than the screen it serves. A screen of this
    // repo's own prose distils to ~1,170 distinct strips under a healing
    // face and ~380 under an operator face; the pins of one frame are held
    // until the next re-baseline, so a sheet that holds only one screen has
    // nothing evictable left the moment that screen is a frame old.
    CHECK_MSG(measured.capacity > 2 * 1170, "the healing sheet must outlast a screen of prose plus its pins");
    const auto economy = planLigatureStripSheet(15, 28, 2, 2, LigatureParticipation::OperatorRuns, 16384, strips);
    CHECK_MSG(economy.capacity > 2 * 380, "the operator sheet must outlast a screen of code plus its pins");
}

// A proportional run's strip holds the text's ink, not the run's cells, and
// the shaper owns the alignment: the pen moves by the slack and floors to the
// pixel the way CoreText floors every pen of a line, the whole pixels place
// the quad, the residual is drawn and keyed. On the macOS arm a sheet placed
// by this arithmetic draws what one CTLineDraw of each label at the
// protocol's pen draws.
TEST(testProportionalStripLayoutAlignsInsideTheCellsAndKeepsTheInk)
{
    const double cell = 12.0, margin = 12.0, slot = 217.0;
    const double text = 156.25;

    // Left, whole pen: the text starts at the margin; the clip keeps a margin
    // each side because the text fits; the strip runs to the ink plus a margin.
    const ProportionalStripLayout left = proportionalStripLayout(text, 18, 0, 0.0, cell, margin, slot);
    CHECK(left.penXInSlotPixels - left.rasterInsetInPixels == margin);
    CHECK(left.penResidualInPixels == 0.0);
    CHECK(left.quadLeftInPixels == static_cast<float>(-margin));
    CHECK(left.clipLeftInSlotPixels - left.rasterInsetInPixels == 0.0);
    CHECK(left.clipRightInSlotPixels - left.rasterInsetInPixels == margin + 18 * cell + margin);
    CHECK(left.usedWidthInSlotPixels == static_cast<float>(std::ceil(margin + text) + margin));

    // Right in 18 cells: slack 59.75 → the pen floors to 59 whole pixels and a
    // 0.75 residual, which is drawn and keyed; the quad moves by the whole
    // pixels; the cells start 59.75 left of the aligned pen.
    const ProportionalStripLayout right = proportionalStripLayout(text, 18, 1, 0.0, cell, margin, slot);
    CHECK(std::fabs(right.penResidualInPixels - 0.75) < 1e-9);
    CHECK(std::fabs(right.penXInSlotPixels - right.rasterInsetInPixels - (margin + 0.75)) < 1e-9);
    CHECK(right.quadLeftInPixels == static_cast<float>(59.0 - margin));
    CHECK(std::fabs(right.clipLeftInSlotPixels - right.rasterInsetInPixels - (margin + 0.75 - 59.75 - margin)) < 1e-9);
    CHECK(std::fabs(right.clipRightInSlotPixels - right.rasterInsetInPixels - (margin + 0.75 - 59.75 + 18 * cell + margin)) < 1e-9);

    // Centred with a fractional run pen: the run's own residual and half the
    // slack add before the floor, so the quad and the residual together place
    // the text exactly where a single pen at residual + slack/2 would sit.
    const ProportionalStripLayout centred = proportionalStripLayout(text, 18, 2, 0.4, cell, margin, slot);
    const double alignedPen = 0.4 + 59.75 / 2.0;
    CHECK(centred.quadLeftInPixels == static_cast<float>(std::floor(alignedPen) - margin));
    CHECK(std::fabs(centred.penResidualInPixels - (alignedPen - std::floor(alignedPen))) < 1e-9);

    // Overflow: four cells cannot hold the text, so no slack moves the pen and
    // the clip ends at the last cell with no margin — the neighbour's cell is
    // the protocol's promise — and the strip ends there too.
    const ProportionalStripLayout overflow = proportionalStripLayout(text, 4, 1, 0.0, cell, margin, slot);
    CHECK(overflow.penXInSlotPixels - overflow.rasterInsetInPixels == margin);
    CHECK(overflow.clipRightInSlotPixels - overflow.rasterInsetInPixels == margin + 4 * cell);
    CHECK(overflow.usedWidthInSlotPixels == static_cast<float>(std::ceil(margin + 4 * cell) + margin));

    // A run wider than the slot holds: 32 cells right-aligned is more cells
    // than any strip could carry, and the strip carries the ink instead — its
    // width is the text's and a margin, the quad moves by the whole slack.
    const ProportionalStripLayout wide = proportionalStripLayout(text, 32, 1, 0.0, cell, margin, slot);
    const double wideResidual = (32 * cell - text) - std::floor(32 * cell - text);
    CHECK(wide.usedWidthInSlotPixels == static_cast<float>(std::ceil(margin + wideResidual + text) + margin));
    CHECK(wide.usedWidthInSlotPixels < static_cast<float>(slot));
    CHECK(wide.quadLeftInPixels == static_cast<float>(std::floor(32 * cell - text) - margin));

    // Text wider than one slot is covered by disjoint guarded tiles.
    const ProportionalStripLayout huge = proportionalStripLayout(300.0, 32, 0, 0.0, cell, margin, slot);
    CHECK(huge.usedWidthInSlotPixels == static_cast<float>(slot - 2));
    CHECK(huge.tileCount == 2);
    const auto tail = proportionalStripTile(huge, static_cast<int>(slot), 1);
    CHECK(tail.quadLeftInPixels == huge.quadLeftInPixels + huge.usedWidthInSlotPixels);
    CHECK(huge.usedWidthInSlotPixels + tail.usedWidthInSlotPixels == huge.fullWidthInPixels);
}

TEST(theAutomaticProportionalSizeIsOneOpticalStepOverTheGrid)
{
    // The stated target: a 12pt grid sets its runs at 13pt, whatever the two
    // families are. 12 * 1.08 = 12.96, and the quarter-point grid takes it up.
    CHECK(automaticProportionalPointSize(12.0) == 13.0);

    // The step scales with the grid and lands on a quarter point either way it
    // rounds: 10 * 1.08 = 10.8 down to 10.75, 11 * 1.08 = 11.88 up to 12.0,
    // 13 * 1.08 = 14.04 down to 14.0, 14 * 1.08 = 15.12 down to 15.0,
    // 16 * 1.08 = 17.28 down to 17.25.
    CHECK(automaticProportionalPointSize(10.0) == 10.75);
    CHECK(automaticProportionalPointSize(11.0) == 12.0);
    CHECK(automaticProportionalPointSize(13.0) == 14.0);
    CHECK(automaticProportionalPointSize(14.0) == 15.0);
    CHECK(automaticProportionalPointSize(16.0) == 17.25);
    for (double grid = 6.0; grid <= 96.0; grid += 0.5) {
        const double size = automaticProportionalPointSize(grid);
        CHECK_MSG(std::abs(size * 4.0 - std::round(size * 4.0)) < 1e-9,
            "every automatic size is a whole quarter point");
        CHECK_MSG(size >= grid, "and never sits below the grid's own size");
        CHECK_MSG(size <= grid * 1.08 + 0.13, "and never overshoots the step by more than the rounding");
    }

    // The floor is what a size too small for the step to clear a quarter point
    // needs: 0.1 * 1.08 rounds to zero, and a face at zero draws nothing.
    CHECK(automaticProportionalPointSize(0.1) == 0.1);

    // A configured size wins outright; zero is the only value that asks for
    // the automatic one.
    CHECK(proportionalFacePointSize(12.0, 11.0) == 11.0);
    CHECK(proportionalFacePointSize(12.0, 20.5) == 20.5);
    CHECK(proportionalFacePointSize(12.0, 0.0) == 13.0);
    CHECK(proportionalFacePointSize(12.0, -1.0) == 13.0);
}

TEST(theColourGridIsOneDerivationBothArmsReach)
{
    // The software sheet and the Vulkan sheet are compared frame for frame, so
    // a cluster one evicted and the other did not is a difference in the picture
    // that is about neither renderer. They agree because they ask the same
    // function, and the function is screen-independent: the software renderer
    // builds from a cell size alone and the Vulkan arm from a measured screen,
    // so anything derived from the screen would put them on different grids.
    const AtlasGridFit small = colourGlyphAtlasGrid(16, sharedColourAtlasTextureLimit);
    CHECK(small.valid);
    CHECK_MSG(small.columns * small.rows >= 1614,
        "the grid holds zwj_all_emoji's 1614 distinct clusters, which is the "
        "screenful the 256-slot sheet could not");

    // Square rather than the sheet's sixteen columns: at 128 rows a full-size
    // slot puts the sheet past the 4096 texels every target guarantees.
    CHECK_MSG(small.columns * 16 <= sharedColourAtlasTextureLimit
            && small.rows * 16 <= sharedColourAtlasTextureLimit,
        "and fits the shared limit on both axes");

    // A slot large enough that the wanted count cannot fit is cut, not refused:
    // the clamp is what keeps a big cell from asking for a texture no device
    // allocates.
    const AtlasGridFit large = colourGlyphAtlasGrid(200, sharedColourAtlasTextureLimit);
    CHECK(large.valid);
    CHECK_MSG(large.columns * 200 <= sharedColourAtlasTextureLimit
            && large.rows * 200 <= sharedColourAtlasTextureLimit,
        "a large slot is cut to the limit rather than overflowing it");
    CHECK_MSG(large.columns * large.rows < small.columns * small.rows,
        "and holds fewer slots than a small one, which is what being cut means");

    // One slot past the limit is no grid at all — the caller refuses the sheet
    // rather than allocating a texture the device will not take.
    CHECK_MSG(!colourGlyphAtlasGrid(sharedColourAtlasTextureLimit + 1, sharedColourAtlasTextureLimit).valid,
        "a slot alone past the limit is refused");
}

UBIQUITTY_TEST_MAIN()
