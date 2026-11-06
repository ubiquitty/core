// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_overlaygeometry.cpp — portable overlay pixel math (Rendering/
// OverlayGeometry): the cursor shape/snap/fade quad, the cell-span highlight
// quad, and the scrollback thumb. Pins the geometry every frontend renders
// identically. Pure C++, links standalone.

#include "Core/Rendering/ColorResolve.h"
#include "Core/Rendering/OverlayGeometry.h"

#include <cmath>
#include <cstdio>

using Ubiquitty::Cursor::Shape;
using Ubiquitty::Rendering::buildCursorQuad;
using Ubiquitty::Rendering::buildPasswordLockQuads;
using Ubiquitty::Rendering::buildPromptMarkBrackets;
using Ubiquitty::Rendering::buildScrollIndicatorQuad;
using Ubiquitty::Rendering::cellSpanQuad;
using Ubiquitty::Rendering::CursorQuadParameters;
using Ubiquitty::Rendering::maxPasswordLockQuads;
using Ubiquitty::Rendering::OverlayQuad;
using Ubiquitty::Rendering::PasswordLockGeometry;
using Ubiquitty::Rendering::PasswordLockParameters;
using Ubiquitty::Rendering::PromptMarkGeometry;
using Ubiquitty::Rendering::PromptMarkParameters;
using Ubiquitty::Rendering::resolvePasswordLockGeometry;
using Ubiquitty::Rendering::resolvePromptMarkGeometry;
using Ubiquitty::Rendering::ScrollIndicatorParameters;
using Ubiquitty::Rendering::searchMatchQuad;
using Ubiquitty::Rendering::selectionHighlightColor;
using Ubiquitty::Rendering::triggerHighlightQuad;

static bool approx(float a, float b) { return std::fabs(a - b) < 1e-3f; }

// A baseline cursor: 8x16 px cell, 1px/pt (cellHeightPoints == cellHeightInPixels),
// no line spacing, opaque white, on-screen.
static CursorQuadParameters baseCursor()
{
    CursorQuadParameters p;
    p.column = 4;
    p.vrow = 2;
    p.rows = 24;
    p.columns = 80;
    p.shape = Shape::Block;
    p.blinkAlpha = 1.0f;
    p.focused = false;
    p.cellWidthInPixels = 8.0f;
    p.cellHeightInPixels = 16.0f;
    p.origin.x = 0.0f;
    p.origin.y = 0.0f;
    p.cursorRed = p.cursorGreen = p.cursorBlue = p.cursorAlpha = 1.0f;
    p.cellHeightPoints = 16.0;
    p.lineSpacingPoints = 0.0;
    return p;
}

TEST(overlaygeometryCases)
{

    // ---- cellSpanQuad: [c0,c1] inclusive → pixel rect (c1+1 / vrow+1) ----
    {
        OverlayQuad q = cellSpanQuad(2, 5, 3, { 8.0f, 16.0f }, { 10.0f, 20.0f }, { 0.4f, 0.55f, 0.85f, 0.35f });
        CHECK(approx(q.minX, 2 * 8 + 10)); // 26
        CHECK(approx(q.minY, 3 * 16 + 20)); // 68
        CHECK(approx(q.maxX, 6 * 8 + 10)); // 58
        CHECK(approx(q.maxY, 4 * 16 + 20)); // 84
        CHECK(approx(q.red, 0.4f) && approx(q.alpha, 0.35f));
        CHECK(q.cornerRadius == 0.0f && q.strokeWidth == 0.0f);
    }

    // ---- selectionHighlightColor: the single baked selection tint both the
    //      macOS + Linux frontends (and the agent rasterizer) share ----
    {
        const auto c = selectionHighlightColor();
        CHECK(approx(c.red, 0.40f));
        CHECK(approx(c.green, 0.55f));
        CHECK(approx(c.blue, 0.85f));
        CHECK(approx(c.alpha, 0.35f));
        // It feeds a cell-span quad unchanged (the macOS selection path), so the
        // quad carries exactly the shared tint.
        OverlayQuad sel = cellSpanQuad(0, 0, 0, { 8.0f, 16.0f }, {}, c);
        CHECK(approx(sel.red, 0.40f) && approx(sel.alpha, 0.35f));
    }

    // ---- searchMatchQuad: abs row → visible row, [col,col+len) span ----
    {
        // history 100, viewOffset 10 ⇒ vrow = absoluteRow - 100 + 10. absoluteRow 95 ⇒ vrow 5.
        OverlayQuad q;
        CHECK(searchMatchQuad(/*absoluteRow*/ 95, /*column*/ 3, /*length*/ 4,
            /*history*/ 100, /*viewOffset*/ 10, /*rows*/ 24, { 8.0f, 16.0f }, { 0.0f, 0.0f }, Ubiquitty::Rendering::SearchMatchEmphasis::Other, q));
        // A quarter of a cell wider than the span on each side. A highlight
        // ending on the advance box clips the side bearing of the first and last
        // glyph, so the ink touches the edge and the span reads as cut off.
        // Asserted as the inset rather than as the number it produces: a bearing
        // changed here should move this line, and a bearing dropped should fail
        // it.
        constexpr float sideBearing = 8.0f * 0.25f;
        CHECK(approx(q.minX, 3 * 8 - sideBearing)); // col 3, less the bearing
        CHECK(approx(q.maxX, 7 * 8 + sideBearing)); // col+len = 7 (exclusive), plus it
        CHECK(approx(q.minY, 5 * 16)); // vrow 5
        CHECK(approx(q.maxY, 6 * 16)); // the rows are untouched — the padding is horizontal
        // normal (inactive) match: Xcode/native find grammar's subtle neutral gray.
        CHECK(approx(q.red, 0.60f) && approx(q.green, 0.60f) && approx(q.blue, 0.62f) && approx(q.alpha, 0.30f));
        // Every match gets a rounded corner (cellHeightInPixels * 0.22); no stroke.
        CHECK(approx(q.cornerRadius, 16.0f * 0.22f) && q.strokeWidth == 0.0f);
        // Continuous, not a circular arc: a superellipse curves into the edges
        // rather than meeting them at a tangent, which is the corner macOS draws.
        // Both emphases carry it — they are one affordance at two weights.
        CHECK(q.cornerStyle == Ubiquitty::Rendering::CornerStyle::Continuous);
    }
    // ---- both quads sit at a non-zero, NON-SQUARE origin ----
    //
    // Every case above places the grid at {0, 0}, where exchanging the origin's
    // axes changes nothing — and the cell extent is the only pair those cases
    // can tell apart. This one offsets x and y differently, so a quad that adds
    // the wrong one lands somewhere these assertions do not accept.
    {
        OverlayQuad q;
        CHECK(searchMatchQuad(/*absoluteRow*/ 95, /*column*/ 3, /*length*/ 4,
            /*history*/ 100, /*viewOffset*/ 10, /*rows*/ 24, { 8.0f, 16.0f }, { 20.0f, 7.0f }, Ubiquitty::Rendering::SearchMatchEmphasis::Other, q));
        CHECK(approx(q.minX, 20.0f + 3 * 8 - 8.0f * 0.25f)); // origin.x + col * cellWidth, less the bearing
        CHECK(approx(q.maxX, 20.0f + 7 * 8 + 8.0f * 0.25f));
        CHECK(approx(q.minY, 7.0f + 5 * 16)); // origin.y + vrow * cellHeight
        CHECK(approx(q.maxY, 7.0f + 6 * 16));

        OverlayQuad h;
        CHECK(triggerHighlightQuad(/*absoluteRow*/ 95, /*column0*/ 3, /*column1*/ 6,
            /*history*/ 100, /*viewOffset*/ 10, /*rows*/ 24, { 8.0f, 16.0f }, { 20.0f, 7.0f }, h));
        CHECK(approx(h.minX, 20.0f + 3 * 8));
        CHECK(approx(h.minY, 7.0f + 5 * 16));
    }
    // ---- searchMatchQuad: the focused match carries no quad at all ----
    {
        OverlayQuad q;
        // The focused match is its baked cell background and the ink on it,
        // and nothing else. Anything this function drew would sit over the
        // glyph the bake exists to keep readable, and a border around the span
        // reads as a second highlight rather than as emphasis. So the function
        // declines, and every frontend that asks gets the same answer.
        CHECK(!searchMatchQuad(95, 3, 4, 100, 10, 24, { 8.0f, 16.0f }, { 0.0f, 0.0f }, Ubiquitty::Rendering::SearchMatchEmphasis::Focused, q));
        // Declining for the FOCUSED emphasis specifically, not because these
        // arguments are out of view — the same span with the other emphasis
        // still paints, so a future bounds change cannot make this pass for the
        // wrong reason.
        CHECK(searchMatchQuad(95, 3, 4, 100, 10, 24, { 8.0f, 16.0f }, { 0.0f, 0.0f }, Ubiquitty::Rendering::SearchMatchEmphasis::Other, q));
        CHECK(q.strokeWidth == 0.0f); // filled, not a ring
        CHECK(q.cornerStyle == Ubiquitty::Rendering::CornerStyle::Continuous);
        // And the fill the builder bakes is opaque — the channel the bake forces
        // anyway, so the function does not carry a value only one consumer reads.
        CHECK(approx(Ubiquitty::Rendering::focusedSearchMatchColor().alpha, 1.00f));
    }
    // ---- the two entry points to the non-focused quad are one geometry ----
    {
        // A frontend driving the search engine holds absolute rows; the builder
        // holds spans already mapped into the viewport. Both reach the same
        // quad, which is the property that lets the builder draw this tier
        // without a second copy of the inset, radius and corner style.
        OverlayQuad viaAbsolute;
        CHECK(searchMatchQuad(/*absoluteRow*/ 95, /*column*/ 3, /*length*/ 4,
            /*history*/ 100, /*viewOffset*/ 10, /*rows*/ 24, { 8.0f, 16.0f }, { 20.0f, 7.0f },
            Ubiquitty::Rendering::SearchMatchEmphasis::Other, viaAbsolute));
        // 95 - 100 + 10 = visible row 5, columns [3, 6] inclusive.
        OverlayQuad viaVisible;
        CHECK(Ubiquitty::Rendering::otherSearchMatchQuadAtVisibleRow(5, 3, 6, { 8.0f, 16.0f }, { 20.0f, 7.0f }, viaVisible));
        CHECK(approx(viaAbsolute.minX, viaVisible.minX) && approx(viaAbsolute.maxX, viaVisible.maxX));
        CHECK(approx(viaAbsolute.minY, viaVisible.minY) && approx(viaAbsolute.maxY, viaVisible.maxY));
        CHECK(approx(viaAbsolute.cornerRadius, viaVisible.cornerRadius));
        CHECK(viaAbsolute.cornerStyle == viaVisible.cornerStyle);
        CHECK(approx(viaAbsolute.red, viaVisible.red) && approx(viaAbsolute.green, viaVisible.green)
            && approx(viaAbsolute.blue, viaVisible.blue) && approx(viaAbsolute.alpha, viaVisible.alpha));
        // Not a comparison of two defaulted structs: the shared quad carries a
        // radius and a widened box, so an implementation returning an untouched
        // OverlayQuad on both sides would fail these rather than pass them.
        CHECK(viaVisible.cornerRadius > 0.0f);
        CHECK(viaVisible.minX < 20.0f + 3 * 8);
        // An inverted span draws nothing, so a caller clamping columns into an
        // empty range emits no quad rather than a degenerate one.
        OverlayQuad empty;
        CHECK(!Ubiquitty::Rendering::otherSearchMatchQuadAtVisibleRow(5, 6, 3, { 8.0f, 16.0f }, { 20.0f, 7.0f }, empty));
    }
    // ---- the search pills centre on the cap-height band, not the cell ----
    {
        using Ubiquitty::Rendering::searchPillInsets;
        using Ubiquitty::Rendering::SearchPillInsets;
        const auto is = [](SearchPillInsets insets, float top, float bottom) { return approx(insets.top, top) && approx(insets.bottom, bottom); };
        // A 32px row of 16pt, atlas at 2x: the baseline sits at 20.4 + 2.8 =
        // 23.2, and a point is two pixels, the breathing room each side.
        // No cap height: the x-height band's midpoint is 23.2 - 5.5 = 17.7,
        // 14.3 from the row's bottom, so the pill reaches 12.3 each way — a
        // 5px top inset and a 2px bottom one.
        CHECK(is(searchPillInsets(32.0f, 16.0, 2.0, 20.4f, 2.8f, 11.0f, 0.0f), 5.0f, 2.0f));
        // A cap height answers instead: JetBrains Mono's 14.6 centres at 15.9,
        // the row's own midpoint give or take, so the pill stands two pixels in
        // at both ends and the x-height beside it no longer decides.
        CHECK(is(searchPillInsets(32.0f, 16.0, 2.0, 20.4f, 2.8f, 11.0f, 14.6f), 2.0f, 2.0f));
        // A shorter cap band sits lower and the pill follows it: centred at
        // 19.2, reaching 10.8 each way, top 8 and bottom 2.
        CHECK(is(searchPillInsets(32.0f, 16.0, 2.0, 20.4f, 2.8f, 11.0f, 8.0f), 8.0f, 2.0f));
        // The room above the band equals the room below it: an inset pill is
        // centred on the band, not merely inside the row.
        {
            const SearchPillInsets insets = searchPillInsets(32.0f, 16.0, 2.0, 20.4f, 2.8f, 11.0f, 8.0f);
            const float middle = (insets.top + (32.0f - insets.bottom)) * 0.5f;
            CHECK(std::fabs(middle - 19.2f) <= 0.5f);
        }
        // Whole device pixels, never a fraction: a straight edge at a half
        // pixel blurs.
        const SearchPillInsets whole = searchPillInsets(32.0f, 16.0, 2.0, 20.4f, 2.8f, 11.0f, 14.6f);
        CHECK(std::floor(whole.top) == whole.top && std::floor(whole.bottom) == whole.bottom);
        // Unmeasured heights or ascent leave the pill on the cell's box: zero
        // rather than a guess, on every arm that has no metric to give.
        CHECK(is(searchPillInsets(32.0f, 16.0, 2.0, 20.4f, 2.8f, 0.0f, 0.0f), 0.0f, 0.0f));
        CHECK(is(searchPillInsets(32.0f, 16.0, 2.0, 0.0f, 2.8f, 11.0f, 14.6f), 0.0f, 0.0f));
        CHECK(is(searchPillInsets(0.0f, 16.0, 2.0, 20.4f, 2.8f, 11.0f, 14.6f), 0.0f, 0.0f));
        // An eccentric face cannot squeeze the pill past two thirds of the row,
        // at either end: a band near the top clamps the bottom inset, one near
        // the bottom the top inset, both at a whole third.
        CHECK(is(searchPillInsets(32.0f, 16.0, 2.0, 12.0f, 0.0f, 20.0f, 0.0f), 0.0f, 10.0f));
        CHECK(is(searchPillInsets(32.0f, 16.0, 2.0, 30.0f, 0.0f, 1.0f, 0.0f), 10.0f, 0.0f));
        // Zero points reads as one pixel per point, so a caller with only
        // device pixels still gets the same fraction arithmetic, with one
        // pixel of breathing room.
        CHECK(is(searchPillInsets(32.0f, 0.0, 1.0, 20.4f, 2.8f, 11.0f, 0.0f), 4.0f, 1.0f));
        // The inset quad keeps the plain one's width, stands in by exactly the
        // insets at each end, and rounds its corners by its own height.
        OverlayQuad plain, inset;
        CHECK(Ubiquitty::Rendering::otherSearchMatchQuadAtVisibleRow(5, 3, 6, { 8.0f, 16.0f }, { 20.0f, 7.0f }, plain));
        CHECK(Ubiquitty::Rendering::otherSearchMatchQuadAtVisibleRow(5, 3, 6, { 8.0f, 16.0f }, { 20.0f, 7.0f }, { 2.0f, 3.0f }, inset));
        CHECK(approx(inset.minY, plain.minY + 2.0f) && approx(inset.maxY, plain.maxY - 3.0f));
        CHECK(approx(inset.minX, plain.minX) && approx(inset.maxX, plain.maxX));
        CHECK(approx(inset.cornerRadius, (16.0f - 5.0f) * 0.22f) && inset.cornerStyle == plain.cornerStyle);
        OverlayQuad empty;
        CHECK(!Ubiquitty::Rendering::otherSearchMatchQuadAtVisibleRow(5, 6, 3, { 8.0f, 16.0f }, { 20.0f, 7.0f }, { 2.0f, 3.0f }, empty));
    }
    // ---- the trigger highlight's two entry points are one geometry ----
    {
        // Same split as the search tier: a frontend reading Triggers::highlightAt
        // holds absolute rows, the builder holds spans already mapped into the
        // viewport, and both reach one quad.
        OverlayQuad viaAbsolute;
        CHECK(triggerHighlightQuad(/*absoluteRow*/ 95, /*column0*/ 3, /*column1*/ 6,
            /*history*/ 100, /*viewOffset*/ 10, /*rows*/ 24, { 8.0f, 16.0f }, { 20.0f, 7.0f }, viaAbsolute));
        OverlayQuad viaVisible;
        CHECK(Ubiquitty::Rendering::triggerHighlightQuadAtVisibleRow(5, 3, 6, { 8.0f, 16.0f }, { 20.0f, 7.0f }, viaVisible));
        CHECK(approx(viaAbsolute.minX, viaVisible.minX) && approx(viaAbsolute.maxX, viaVisible.maxX));
        CHECK(approx(viaAbsolute.minY, viaVisible.minY) && approx(viaAbsolute.maxY, viaVisible.maxY));
        CHECK(approx(viaAbsolute.red, viaVisible.red) && approx(viaAbsolute.green, viaVisible.green)
            && approx(viaAbsolute.blue, viaVisible.blue) && approx(viaAbsolute.alpha, viaVisible.alpha));
        // Not two defaulted structs: the quad covers four columns of an 8px cell
        // at row 5, so an implementation returning an untouched OverlayQuad on
        // both sides fails here rather than passing.
        CHECK(approx(viaVisible.minX, 20.0f + 3 * 8) && approx(viaVisible.maxX, 20.0f + 7 * 8));
        CHECK(approx(viaVisible.minY, 7.0f + 5 * 16));
        // Distinct from the search tier's neutral grey, which is the property
        // that keeps a fired rule from reading as a search hit.
        OverlayQuad search;
        CHECK(Ubiquitty::Rendering::otherSearchMatchQuadAtVisibleRow(5, 3, 6, { 8.0f, 16.0f }, { 20.0f, 7.0f }, search));
        CHECK(!(approx(viaVisible.red, search.red) && approx(viaVisible.green, search.green)
            && approx(viaVisible.blue, search.blue)));
        OverlayQuad empty;
        CHECK(!Ubiquitty::Rendering::triggerHighlightQuadAtVisibleRow(5, 6, 3, { 8.0f, 16.0f }, { 20.0f, 7.0f }, empty));
    }
    // ---- searchMatchQuad: scrolled off-screen / empty span → draws nothing ----
    {
        OverlayQuad q;
        // vrow = 200 - 100 + 0 = 100 ⇒ >= rows (24) ⇒ off-screen below.
        CHECK(!searchMatchQuad(200, 0, 4, 100, 0, 24, { 8.0f, 16.0f }, { 0.0f, 0.0f }, Ubiquitty::Rendering::SearchMatchEmphasis::Other, q));
        // vrow = 50 - 100 + 0 = -50 ⇒ < 0 ⇒ scrolled off above.
        CHECK(!searchMatchQuad(50, 0, 4, 100, 0, 24, { 8.0f, 16.0f }, { 0.0f, 0.0f }, Ubiquitty::Rendering::SearchMatchEmphasis::Other, q));
        // zero-length span ⇒ nothing.
        CHECK(!searchMatchQuad(95, 3, 0, 100, 10, 24, { 8.0f, 16.0f }, { 0.0f, 0.0f }, Ubiquitty::Rendering::SearchMatchEmphasis::Other, q));
    }

    // ---- triggerHighlightQuad: inclusive span, distinct violet, same row map ----
    {
        OverlayQuad q;
        // history 100, viewOffset 10 ⇒ vrow = absoluteRow - 100 + 10. absoluteRow 95 ⇒ vrow 5.
        // Inclusive [col0=3 .. col1=6] ⇒ 4 cells, maxX = (6+1)*cw = 7*cw.
        CHECK(Ubiquitty::Rendering::triggerHighlightQuad(/*absoluteRow*/ 95,
            /*column0*/ 3, /*column1*/ 6, /*history*/ 100, /*viewOffset*/ 10,
            /*rows*/ 24, { 8.0f, 16.0f }, { 0.0f, 0.0f }, q));
        CHECK(approx(q.minX, 3 * 8)); // col0 3
        CHECK(approx(q.maxX, 7 * 8)); // inclusive col1 6 → maxX = (6+1)*cw
        CHECK(approx(q.minY, 5 * 16)); // vrow 5
        CHECK(approx(q.maxY, 6 * 16));
        // Violet tint — not find's yellow/orange, so a trigger hit on screen is
        // never read as a search match.
        CHECK(approx(q.red, 0.62f) && approx(q.green, 0.40f) && approx(q.blue, 0.95f) && approx(q.alpha, 0.34f));
        CHECK(q.cornerRadius == 0.0f && q.strokeWidth == 0.0f);
    }
    // ---- triggerHighlightQuad: off-screen / inverted span → draws nothing ----
    {
        OverlayQuad q;
        // vrow = 200 - 100 + 0 = 100 ⇒ >= rows (24) ⇒ off-screen below.
        CHECK(!Ubiquitty::Rendering::triggerHighlightQuad(200, 0, 4, 100, 0, 24, { 8.0f, 16.0f }, { 0.0f, 0.0f }, q));
        // vrow = 50 - 100 + 0 = -50 ⇒ < 0 ⇒ scrolled off above.
        CHECK(!Ubiquitty::Rendering::triggerHighlightQuad(50, 0, 4, 100, 0, 24, { 8.0f, 16.0f }, { 0.0f, 0.0f }, q));
        // inverted span (col1 < col0) ⇒ nothing.
        CHECK(!Ubiquitty::Rendering::triggerHighlightQuad(95, 6, 3, 100, 10, 24, { 8.0f, 16.0f }, { 0.0f, 0.0f }, q));
    }

    // ---- cursor: unfocused block → hollow outline, full cell, 1px stroke ----
    {
        CursorQuadParameters p = baseCursor(); // focused = false
        OverlayQuad q;
        CHECK(buildCursorQuad(p, q));
        CHECK(approx(q.minX, 32) && approx(q.minY, 32)); // 4*8, 2*16
        CHECK(approx(q.maxX, 40) && approx(q.maxY, 48));
        CHECK(approx(q.alpha, 1.0f)); // unfocused stays solid (no blink scale)
        CHECK(approx(q.strokeWidth, 1.0f)); // hollow outline
        CHECK(q.cornerRadius == 0.0f);
    }

    // ---- cursor: focused block is the builder's tier → no quad here ----
    {
        CursorQuadParameters p = baseCursor();
        p.focused = true;
        OverlayQuad q;
        CHECK(!buildCursorQuad(p, q));
    }

    // ---- cursor: SLIDING focused block → filled moving quad at the fractional
    // cell (overrides the "baked, draw nothing" rule) ----
    {
        CursorQuadParameters p = baseCursor();
        p.focused = true;
        p.sliding = true;
        p.columnFractional = 4.5f; // mid-slide between cols 4 and 5
        p.vrowFractional = 2.0f;
        OverlayQuad q;
        CHECK(buildCursorQuad(p, q)); // now does draw
        CHECK(approx(q.minX, 36) && approx(q.minY, 32)); // 4.5*8, 2*16
        CHECK(approx(q.maxX, 44) && approx(q.maxY, 48)); // +cell (8x16)
        CHECK(approx(q.strokeWidth, 0.0f)); // FILLED, not the unfocused outline
        CHECK(approx(q.alpha, 1.0f)); // baseAlpha * blinkAlpha(1)
    }

    // ---- cursor: a SLIDING focused caret stays fully lit — the blink fade /
    // post-clear dim is suppressed mid-slide so the motion reads ----
    {
        CursorQuadParameters p = baseCursor();
        p.focused = true;
        p.sliding = true;
        p.columnFractional = 4.5f;
        p.vrowFractional = 2.0f;
        p.blinkAlpha = 0.25f; // mid-blink / dimmed — would scale a SETTLED caret
        OverlayQuad q;
        CHECK(buildCursorQuad(p, q));
        CHECK(approx(q.alpha, 1.0f)); // full while sliding, not baseAlpha*blinkAlpha
    }

    // ---- cursor: a SLIDING bar rides the fractional grid (no device-pixel snap),
    // landing true — the snap's rightward ceil bias would shift it a few px ----
    {
        CursorQuadParameters p = baseCursor();
        p.shape = Shape::Bar;
        p.focused = true;
        p.sliding = true;
        p.columnFractional = 4.3f; // 4.3*8 = 34.4 px, off the pixel grid
        p.vrowFractional = 2.0f;
        OverlayQuad q;
        CHECK(buildCursorQuad(p, q));
        // 2px-wide bar centred at 34.4 → [33.4, 35.4], not ceil-snapped to [34, 36].
        CHECK(approx(q.minX, 33.4f));
        CHECK(approx(q.maxX, 35.4f));
    }

    // ---- cursor: sliding off (default) ignores the fractional fields ----
    {
        CursorQuadParameters p = baseCursor();
        p.focused = false; // unfocused so it draws
        p.columnFractional = 99.0f; // would be off-screen if used
        p.vrowFractional = 99.0f;
        // sliding stays false → integer (column,vrow) used, fractional ignored
        OverlayQuad q;
        CHECK(buildCursorQuad(p, q));
        CHECK(approx(q.minX, 32) && approx(q.minY, 32)); // integer cell 4,2
    }

    // ---- cursor: faded-out / off-screen → draws nothing ----
    {
        CursorQuadParameters p = baseCursor();
        p.blinkAlpha = 0.0005f;
        OverlayQuad q;
        CHECK(!buildCursorQuad(p, q));
        p = baseCursor();
        p.vrow = -1;
        CHECK(!buildCursorQuad(p, q));
        p = baseCursor();
        p.column = p.columns; // == columns ⇒ out of range
        CHECK(!buildCursorQuad(p, q));
    }

    // ---- cursor: focused bar → 2px capsule on the cell's left edge, snapped ----
    {
        CursorQuadParameters p = baseCursor();
        p.focused = true;
        p.shape = Shape::Bar;
        p.column = 0;
        p.vrow = 0;
        p.cellWidthInPixels = 10.0f;
        p.cellHeightInPixels = 20.0f;
        p.cellHeightPoints = 20.0; // 1 px/pt ⇒ barWidth = 2
        OverlayQuad q;
        CHECK(buildCursorQuad(p, q));
        CHECK(approx(q.minX, -1) && approx(q.maxX, 1)); // centred on left edge, snapped
        CHECK(approx(q.minY, 0) && approx(q.maxY, 20)); // full cell height
        CHECK(approx(q.cornerRadius, 2.0f * 0.52f)); // capsule ends
        CHECK(q.strokeWidth == 0.0f);
    }

    // ---- cursor: focused underline → 2px capsule on the cell bottom ----
    {
        CursorQuadParameters p = baseCursor();
        p.focused = true;
        p.shape = Shape::Underline;
        p.column = 1;
        p.vrow = 1;
        p.cellWidthInPixels = 10.0f;
        p.cellHeightInPixels = 20.0f;
        p.cellHeightPoints = 20.0;
        OverlayQuad q;
        CHECK(buildCursorQuad(p, q));
        CHECK(approx(q.minX, 10) && approx(q.maxX, 20)); // spans the cell width
        CHECK(approx(q.maxY, 40) && approx(q.minY, 38)); // 2px thick, sits on bottom
        CHECK(approx(q.cornerRadius, 2.0f * 0.52f));
    }

    // ---- cursor on a DECDWL row: 2× column + 2× cell width (matches glyphs) ----
    // Unfocused block: outline lands at the doubled column and spans the doubled
    // cell (col 4, 8px cell ⇒ left 4*8*2=64, width 2*8=16 ⇒ [64,80]).
    {
        CursorQuadParameters p = baseCursor(); // focused = false, col 4, vrow 2
        p.lineAttribute = Ubiquitty::lineAttributeDoubleWidth;
        OverlayQuad q;
        CHECK(buildCursorQuad(p, q));
        CHECK(approx(q.minX, 64) && approx(q.maxX, 80)); // 4*8*2 .. +2*8
        CHECK(approx(q.minY, 32) && approx(q.maxY, 48)); // vertical unchanged
        CHECK(approx(q.strokeWidth, 1.0f));
    }
    // DECDHL top + bottom halves are double-width too ⇒ identical horizontal span.
    {
        for (uint8_t la : { Ubiquitty::lineAttributeDoubleHeightTop, Ubiquitty::lineAttributeDoubleHeightBottom }) {
            CursorQuadParameters p = baseCursor();
            p.lineAttribute = la;
            OverlayQuad q;
            CHECK(buildCursorQuad(p, q));
            CHECK(approx(q.minX, 64) && approx(q.maxX, 80));
        }
    }
    // Bar on a DWL row: insertion point rides the doubled left edge; width stays
    // thin (col 1, 10px cell ⇒ left 1*10*2=20, 2px bar ⇒ [19,21], snapped/ceil).
    {
        CursorQuadParameters p = baseCursor();
        p.focused = true;
        p.shape = Shape::Bar;
        p.column = 1;
        p.vrow = 0;
        p.cellWidthInPixels = 10.0f;
        p.cellHeightInPixels = 20.0f;
        p.cellHeightPoints = 20.0;
        p.lineAttribute = Ubiquitty::lineAttributeDoubleWidth;
        OverlayQuad q;
        CHECK(buildCursorQuad(p, q));
        CHECK(approx(q.minX, 19) && approx(q.maxX, 21)); // left edge at 1*10*2=20
    }
    // Underline on a DWL row: spans the full doubled cell (col 1 ⇒ [20,40]).
    {
        CursorQuadParameters p = baseCursor();
        p.focused = true;
        p.shape = Shape::Underline;
        p.column = 1;
        p.vrow = 1;
        p.cellWidthInPixels = 10.0f;
        p.cellHeightInPixels = 20.0f;
        p.cellHeightPoints = 20.0;
        p.lineAttribute = Ubiquitty::lineAttributeDoubleWidth;
        OverlayQuad q;
        CHECK(buildCursorQuad(p, q));
        CHECK(approx(q.minX, 20) && approx(q.maxX, 40)); // 1*10*2 .. +2*10
    }

    // ---- prompt-mark brackets: both gutters, ok color, exact quad geometry ----
    {
        PromptMarkParameters p;
        p.grid = { 80, 24 };
        p.cellSizeInPixels = { 8.0f, 16.0f };
        p.origin.x = 20.0f; // 20px left gutter
        p.origin.y = 0.0f;
        p.viewportWidthInPixels = 20.0f + 80.0f * 8.0f + 20.0f; // 20px right gutter
        p.cellHeightPoints = 16.0; // 1 px/pt
        p.atlasScale = 1.0;
        p.fontSize = 12;
        p.fontAscent = 12.0f;
        p.fontDescent = 4.0f;
        p.lineGapTopInPixels = 2.0f;
        p.okRed = 0.1f, p.okGreen = 0.2f, p.okBlue = 0.3f;
        p.failRed = 0.9f, p.failGreen = 0.0f, p.failBlue = 0.0f;

        PromptMarkGeometry g = resolvePromptMarkGeometry(p);
        CHECK(g.drawLeft && g.drawRight);
        CHECK(g.perRow == 6);
        CHECK(approx(g.thick, 1.0f) && approx(g.barHeight, 10.0f));
        CHECK(approx(g.minLeftX, 16.0f) && approx(g.maxLeftX, 18.0f));
        CHECK(approx(g.minRightX, 662.0f) && approx(g.maxRightX, 664.0f));
        CHECK(approx(g.baselineFrac, 0.875f) && approx(g.ascentFrac, 0.75f) && approx(g.descentFrac, 0.25f));

        OverlayQuad q[6];
        int n = buildPromptMarkBrackets(p, g, /*rowIndex*/ 0, /*failed*/ false, q, 6);
        CHECK(n == 6);
        // Row 0: glyphMid 10.5 ⇒ yTop 5.0 (0.5pt taller top, yBot held), yBot 15.5.
        // Left spine.
        CHECK(approx(q[0].minX, 16.0f) && approx(q[0].maxX, 17.0f));
        CHECK(approx(q[0].minY, 5.0f) && approx(q[0].maxY, 15.5f));
        // Left top stub.
        CHECK(approx(q[1].minX, 17.0f) && approx(q[1].maxX, 18.0f));
        CHECK(approx(q[1].minY, 5.0f) && approx(q[1].maxY, 6.0f));
        // Left bottom stub.
        CHECK(approx(q[2].minX, 17.0f) && approx(q[2].maxX, 18.0f));
        CHECK(approx(q[2].minY, 14.5f) && approx(q[2].maxY, 15.5f));
        // Right spine.
        CHECK(approx(q[3].minX, 663.0f) && approx(q[3].maxX, 664.0f));
        CHECK(approx(q[3].minY, 5.0f) && approx(q[3].maxY, 15.5f));
        // Right top stub.
        CHECK(approx(q[4].minX, 662.0f) && approx(q[4].maxX, 663.0f));
        // Right bottom stub.
        CHECK(approx(q[5].minX, 662.0f) && approx(q[5].maxX, 663.0f));
        CHECK(approx(q[5].minY, 14.5f) && approx(q[5].maxY, 15.5f));
        // The mark is SOLID ink in a colour mixed with the ground, not the ink
        // at an alpha: over an opaque window the two are the same pixel, and
        // over a translucent one a half-transparent quad adds its coverage to
        // the surface's and stands as a denser patch where the bracket is. The
        // expected value is the same mix Core states rather than a number
        // restated here, so a change to the weights moves both together.
        const auto mixOverGround = [](float ink, float ground, double weight) {
            using Ubiquitty::Rendering::SRGBGamma;
            return static_cast<float>(SRGBGamma::encode(
                SRGBGamma::linearize(ink) * weight + SRGBGamma::linearize(ground) * (1.0 - weight)));
        };
        CHECK(approx(q[0].red, mixOverGround(0.1f, p.groundRed, Ubiquitty::Rendering::promptMarkOkWeight))
            && approx(q[0].green, mixOverGround(0.2f, p.groundGreen, Ubiquitty::Rendering::promptMarkOkWeight))
            && approx(q[0].blue, mixOverGround(0.3f, p.groundBlue, Ubiquitty::Rendering::promptMarkOkWeight)));
        CHECK_MSG(approx(q[0].alpha, 1.0f), "the softening is in the colour, not in an alpha");
        CHECK(q[0].cornerRadius == 0.0f && q[0].strokeWidth == 0.0f);

        // failed row → fail ink at its own weight, mixed over the same ground.
        OverlayQuad qf[6];
        int nf = buildPromptMarkBrackets(p, g, 1, /*failed*/ true, qf, 6);
        CHECK(nf == 6);
        CHECK(approx(qf[0].red, mixOverGround(0.9f, p.groundRed, Ubiquitty::Rendering::promptMarkFailWeight))
            && approx(qf[0].alpha, 1.0f));
        // A failed mark stands harder than an ordinary one, which is the whole
        // point of the two weights: more of its ink survives the mix.
        CHECK(qf[0].red > q[0].red);
        // Row 1 sits one cell lower.
        CHECK(approx(qf[0].minY, q[0].minY + 16.0f));

        // Not enough room in the output buffer → writes nothing.
        CHECK(buildPromptMarkBrackets(p, g, 0, false, q, 5) == 0);
        // Out-of-range row → nothing.
        CHECK(buildPromptMarkBrackets(p, g, 24, false, q, 6) == 0);
    }
    // ---- prompt-mark brackets: only one gutter wide enough ----
    {
        PromptMarkParameters p;
        p.grid = { 80, 10 };
        p.cellSizeInPixels = { 8.0f, 16.0f };
        p.origin.x = 20.0f; // wide left gutter
        p.viewportWidthInPixels = 20.0f + 80.0f * 8.0f + 1.0f; // 1px right gutter (too narrow)
        p.cellHeightPoints = 16.0;
        p.atlasScale = 1.0;
        p.fontSize = 12;
        p.fontAscent = 12.0f;
        p.fontDescent = 4.0f;
        p.lineGapTopInPixels = 2.0f;
        PromptMarkGeometry g = resolvePromptMarkGeometry(p);
        CHECK(g.drawLeft && !g.drawRight);
        CHECK(g.perRow == 3);
        OverlayQuad q[6];
        CHECK(buildPromptMarkBrackets(p, g, 0, false, q, 6) == 3);
    }
    // ---- prompt-mark brackets: both gutters too narrow ⇒ perRow 0, nothing ----
    {
        PromptMarkParameters p;
        p.grid = { 80, 10 };
        p.cellSizeInPixels = { 8.0f, 16.0f };
        p.origin.x = 1.0f; // 1px gutters both sides
        p.viewportWidthInPixels = 1.0f + 80.0f * 8.0f + 1.0f;
        p.cellHeightPoints = 16.0;
        p.atlasScale = 1.0;
        p.fontSize = 12;
        PromptMarkGeometry g = resolvePromptMarkGeometry(p);
        CHECK(!g.drawLeft && !g.drawRight && g.perRow == 0);
        OverlayQuad q[6];
        CHECK(buildPromptMarkBrackets(p, g, 0, false, q, 6) == 0);
    }

    // ---- scroll thumb: live tail ⇒ thumb pinned to the track bottom ----
    {
        ScrollIndicatorParameters p;
        p.alpha = 1.0f;
        p.scrollbackCount = 100;
        p.rows = 24;
        p.viewOffset = 0; // live tail
        p.viewportSizeInPixels = { 800.0f, 600.0f };
        p.isFullScreen = false;
        p.scale = 2.0f;
        OverlayQuad q;
        CHECK(buildScrollIndicatorQuad(p, q));
        // 6pt thumb at 2 px/pt, 3pt right inset: x in [782, 794].
        CHECK(approx(q.minX, 782) && approx(q.maxX, 794));
        CHECK(approx(q.cornerRadius, 6.0f)); // barWidth/2
        // track = [6, 600-16] ⇒ trackTop 6, trackHeight 578; thumbHeight = 24/124*578.
        const float trackBottom = 6.0f + 578.0f; // 584
        CHECK(approx(q.maxY, trackBottom)); // bottom-pinned at live tail
        const float thumbHeight = (24.0f / 124.0f) * 578.0f;
        CHECK(approx(q.minY, trackBottom - thumbHeight));
        CHECK(approx(q.alpha, 0.45f)); // 0.45 * alpha
    }

    // ---- scroll thumb: scrolled to oldest ⇒ thumb pinned to the track top ----
    {
        ScrollIndicatorParameters p;
        p.alpha = 1.0f;
        p.scrollbackCount = 100;
        p.rows = 24;
        p.viewOffset = 100; // oldest (== scrollbackCount)
        p.viewportSizeInPixels = { 800.0f, 600.0f };
        p.scale = 1.0f;
        OverlayQuad q;
        CHECK(buildScrollIndicatorQuad(p, q));
        CHECK(approx(q.minY, 6.0f)); // trackTop
    }

    // ---- scroll thumb: nothing to show ----
    {
        ScrollIndicatorParameters p;
        p.alpha = 0.0f; // fully faded
        p.scrollbackCount = 100;
        p.rows = 24;
        p.viewportSizeInPixels = { 800.0f, 600.0f };
        OverlayQuad q;
        CHECK(!buildScrollIndicatorQuad(p, q));
        p.alpha = 1.0f;
        p.scrollbackCount = 0; // no history
        CHECK(!buildScrollIndicatorQuad(p, q));
        p.scrollbackCount = 100;
        p.viewportSizeInPixels.height = 0.0f; // degenerate viewport
        CHECK(!buildScrollIndicatorQuad(p, q));
    }
}

/// A cursor standing inside a glyph's claim spans the claim.
///
/// The builder hands the overlay the anchor column and the claimed width
/// when the cursor falls anywhere in a wide pair, a sized block or a
/// proportional run. The underline and the hollow block then cover the
/// glyph; the bar keeps its own width and seats at the claim's left edge,
/// which is where a caret belongs beside a word.
TEST(cursorQuadSpansTheClaim)
{
    CursorQuadParameters p;
    p.column = 4;
    p.vrow = 1;
    p.rows = 10;
    p.columns = 40;
    p.widthCells = 3;
    p.cellWidthInPixels = 8.0f;
    p.cellHeightInPixels = 16.0f;
    p.focused = false;
    p.shape = Ubiquitty::Cursor::Shape::Block;

    OverlayQuad hollow;
    CHECK_MSG(buildCursorQuad(p, hollow), "an unfocused block draws its hollow outline");
    CHECK_MSG(approx(hollow.minX, 4 * 8.0f) && approx(hollow.maxX, 7 * 8.0f),
        "the hollow block spans the three claimed cells");

    p.shape = Ubiquitty::Cursor::Shape::Underline;
    p.focused = true;
    OverlayQuad underline;
    CHECK_MSG(buildCursorQuad(p, underline), "the underline draws");
    CHECK_MSG(approx(underline.minX, 4 * 8.0f) && approx(underline.maxX, 7 * 8.0f),
        "the underline spans the claim too");

    p.shape = Ubiquitty::Cursor::Shape::Bar;
    OverlayQuad bar;
    CHECK_MSG(buildCursorQuad(p, bar), "the bar draws");
    CHECK_MSG(bar.maxX - bar.minX < 8.0f, "the bar keeps its own width inside the claim");
    CHECK_MSG(bar.minX < 4 * 8.0f + 4.0f, "and seats at the claim's left edge");

    p.widthCells = 1;
    p.shape = Ubiquitty::Cursor::Shape::Underline;
    OverlayQuad plain;
    CHECK_MSG(buildCursorQuad(p, plain), "an unclaimed cell still draws");
    CHECK_MSG(approx(plain.maxX - plain.minX, 8.0f), "at one cell, as before");
}

/// The thin shapes are defined in points, so an arm at backing scale two
/// draws them twice as thick as one at scale one, and the line spacing insets
/// them from the padded cell edge. An arm that passes no point space gets the
/// one-pixel-per-point geometry: a two-pixel bar and a two-pixel underline on
/// the cell foot, which is what the software reference drew until it learned
/// to pass its own scale.
TEST(cursorThinShapesScaleWithThePointSpace)
{
    CursorQuadParameters p;
    p.column = 2;
    p.vrow = 1;
    p.rows = 10;
    p.columns = 40;
    p.focused = true;
    p.cellWidthInPixels = 16.0f;
    p.cellHeightInPixels = 38.0f;
    p.cellHeightPoints = 19.0; // 2 px/pt
    p.lineSpacingPoints = 3.0; // 6 px of padding, 3 above and 3 below

    p.shape = Ubiquitty::Cursor::Shape::Bar;
    OverlayQuad bar;
    CHECK_MSG(buildCursorQuad(p, bar), "the bar draws");
    CHECK_MSG(approx(bar.minX, 30) && approx(bar.maxX, 34), "two points wide is four pixels, centred on the cell edge");
    CHECK_MSG(approx(bar.minY, 41) && approx(bar.maxY, 73), "inset by half the line spacing at each end");

    p.shape = Ubiquitty::Cursor::Shape::Underline;
    OverlayQuad underline;
    CHECK_MSG(buildCursorQuad(p, underline), "the underline draws");
    CHECK_MSG(approx(underline.minY, 69) && approx(underline.maxY, 73), "four pixels thick, seated on the font box bottom");
    CHECK_MSG(approx(underline.minX, 32) && approx(underline.maxX, 48), "across the cell");

    p.shape = Ubiquitty::Cursor::Shape::Block;
    p.focused = false;
    OverlayQuad hollow;
    CHECK_MSG(buildCursorQuad(p, hollow), "the hollow block draws");
    CHECK_MSG(approx(hollow.strokeWidth, 2.0f), "one point of stroke is two pixels");
    CHECK_MSG(approx(hollow.minY, 38) && approx(hollow.maxY, 76), "the block keeps the full cell");

    // No point space, as an arm that never passed one: one pixel per point
    // and no spacing to inset by.
    p.cellHeightPoints = 0.0;
    p.lineSpacingPoints = 0.0;
    p.focused = true;
    p.shape = Ubiquitty::Cursor::Shape::Bar;
    OverlayQuad flatBar;
    CHECK_MSG(buildCursorQuad(p, flatBar), "the bar draws without a point space");
    CHECK_MSG(approx(flatBar.minX, 31) && approx(flatBar.maxX, 33), "two pixels wide");
    CHECK_MSG(approx(flatBar.minY, 38) && approx(flatBar.maxY, 76), "the full cell");

    p.shape = Ubiquitty::Cursor::Shape::Underline;
    OverlayQuad flatUnderline;
    CHECK_MSG(buildCursorQuad(p, flatUnderline), "the underline draws without a point space");
    CHECK_MSG(approx(flatUnderline.minY, 74) && approx(flatUnderline.maxY, 76), "two pixels on the cell foot");

    p.shape = Ubiquitty::Cursor::Shape::Block;
    p.focused = false;
    OverlayQuad flatHollow;
    CHECK_MSG(buildCursorQuad(p, flatHollow), "the hollow block draws without a point space");
    CHECK_MSG(approx(flatHollow.strokeWidth, 1.0f), "a one-pixel stroke");
}

// The bar as GTK's insertion caret (gtk/gtkrenderlayout.c,
// gtk_css_style_snapshot_caret): the line box mapped to whole logical pixels,
// the origin floored and the far edge ceiled; a stem truncating
// height * aspect + 1 logical pixels, stem / 2 left of the insertion point;
// scaled to device pixels whole; square, faded by the blink. Unfocused, the
// bar is the unfocused block's hollow outline, as every unfocused cursor is.
TEST(cursorBarIsTheToolkitCaret)
{
    CursorQuadParameters p = baseCursor();
    p.shape = Shape::Bar;
    p.focused = true;
    p.cellHeightInPixels = 17.0f;
    p.cellHeightPoints = 17.0;
    p.toolkitCaret = { .aspectRatio = 0.04, .logicalScale = 1 };
    p.blinkAlpha = 0.5f;
    OverlayQuad caret;
    CHECK_MSG(buildCursorQuad(p, caret), "the caret draws");
    CHECK_MSG(approx(caret.minX, 32) && approx(caret.maxX, 33), "seventeen pixels of line make a one-pixel stem at the cell edge");
    CHECK_MSG(approx(caret.minY, 34) && approx(caret.maxY, 51), "the line's height");
    CHECK_MSG(caret.cornerRadius == 0.0f && caret.strokeWidth == 0.0f, "a square fill");
    CHECK_MSG(approx(caret.alpha, 0.5f), "faded by the blink");

    // Scale two: the same logical caret, every edge doubled.
    p.cellWidthInPixels = 16.0f;
    p.cellHeightInPixels = 34.0f;
    p.toolkitCaret.logicalScale = 2;
    CHECK_MSG(buildCursorQuad(p, caret), "the scaled caret draws");
    CHECK_MSG(approx(caret.minX, 64) && approx(caret.maxX, 66) && approx(caret.minY, 68) && approx(caret.maxY, 102), "two device pixels per logical one");

    // An insertion point half a logical pixel in floors onto the logical grid.
    p.cellWidthInPixels = 19.0f;
    p.column = 1;
    CHECK_MSG(buildCursorQuad(p, caret), "the odd-edge caret draws");
    CHECK_MSG(approx(caret.minX, 18) && approx(caret.maxX, 20), "nine and a half logical pixels floor to nine");

    // Fifty logical pixels of line: a three-pixel stem, one of them left of
    // the insertion point.
    p.column = 1;
    p.vrow = 0;
    p.cellWidthInPixels = 30.0f;
    p.cellHeightInPixels = 100.0f;
    p.cellHeightPoints = 50.0;
    CHECK_MSG(buildCursorQuad(p, caret), "the tall caret draws");
    CHECK_MSG(approx(caret.minX, 28) && approx(caret.maxX, 34) && approx(caret.minY, 0) && approx(caret.maxY, 100), "stem three, offset one, doubled");
    // Zero is a setting GTK takes: one logical pixel at any height.
    p.toolkitCaret.aspectRatio = 0.0;
    CHECK_MSG(buildCursorQuad(p, caret), "the zero-aspect caret draws");
    CHECK_MSG(approx(caret.minX, 30) && approx(caret.maxX, 32), "one logical pixel");

    // Line spacing insets the line box; its half-pixel edges round outward.
    p = baseCursor();
    p.shape = Shape::Bar;
    p.focused = true;
    p.vrow = 1;
    p.cellHeightInPixels = 20.0f;
    p.cellHeightPoints = 20.0;
    p.lineSpacingPoints = 3.0;
    p.toolkitCaret = { .aspectRatio = 0.04, .logicalScale = 1 };
    CHECK_MSG(buildCursorQuad(p, caret), "the spaced caret draws");
    CHECK_MSG(approx(caret.minY, 21) && approx(caret.maxY, 39), "21.5 floors and 38.5 ceils");

    // Unfocused: the steady hollow outline of the unfocused block, not GTK's
    // absent caret; the blink does not fade it.
    p.focused = false;
    p.blinkAlpha = 0.25f;
    OverlayQuad outline;
    CHECK_MSG(buildCursorQuad(p, caret), "the unfocused bar draws");
    CursorQuadParameters block = p;
    block.shape = Shape::Block;
    CHECK(buildCursorQuad(block, outline));
    CHECK_MSG(approx(caret.minX, outline.minX) && approx(caret.maxX, outline.maxX) && approx(caret.minY, outline.minY)
            && approx(caret.maxY, outline.maxY) && caret.strokeWidth == outline.strokeWidth && caret.strokeWidth > 0.0f,
        "the unfocused bar is the unfocused block's outline");
    CHECK_MSG(approx(caret.minX, 32) && approx(caret.maxX, 40) && approx(caret.minY, 20) && approx(caret.maxY, 40), "across the whole cell");
    CHECK_MSG(approx(caret.alpha, 1.0f), "steady");
    p.toolkitCaret = {};
    CHECK_MSG(buildCursorQuad(p, caret) && caret.strokeWidth > 0.0f && approx(caret.minX, 32) && approx(caret.maxX, 40),
        "the capsule's arms outline it too");
    // The unfocused underline is the same outline: one contract for every shape.
    p.shape = Shape::Underline;
    OverlayQuad underline;
    CHECK_MSG(buildCursorQuad(p, underline) && underline.strokeWidth == outline.strokeWidth && approx(underline.minX, outline.minX)
            && approx(underline.maxX, outline.maxX) && approx(underline.minY, outline.minY) && approx(underline.maxY, outline.maxY)
            && underline.cornerRadius == 0.0f && approx(underline.alpha, 1.0f),
        "the unfocused underline is the unfocused block's outline, steady");
    p.shape = Shape::Bar;
    p.blinkAlpha = 1.0f;

    // No toolkit: Core's capsule.
    p.focused = true;
    p.toolkitCaret = {};
    CHECK_MSG(buildCursorQuad(p, caret) && caret.cornerRadius > 0.0f, "the capsule without a toolkit");
}

TEST(hiliteRegionRowSpansCases)
{
    using Ubiquitty::Rendering::hiliteRegionRowSpans;
    using Ubiquitty::Rendering::SelectionRowSpan;
    SelectionRowSpan out[32];

    // ---- the band is a text flow, not a rectangle ----
    {
        // Start at column 5 of row 2, band rows 1..4, on an 80x24 screen.
        const int n = hiliteRegionRowSpans(5, 2, 1, 4, 24, 80, 0, out, 32);
        CHECK(n == 3); // rows 2, 3, 4
        CHECK(out[0].visibleRow == 1 && out[0].column0 == 4 && out[0].column1 == 79);
        CHECK(out[1].visibleRow == 2 && out[1].column0 == 0 && out[1].column1 == 79);
        CHECK(out[2].visibleRow == 3 && out[2].column0 == 0 && out[2].column1 == 79);
    }

    // ---- a start above the band snaps down to its first row ----
    {
        const int n = hiliteRegionRowSpans(5, 1, 3, 5, 24, 80, 0, out, 32);
        CHECK(n == 3);
        CHECK(out[0].visibleRow == 2 && out[0].column0 == 4); // row 3 keeps the column
    }

    // ---- a start below the band collapses onto its last row ----
    {
        const int n = hiliteRegionRowSpans(5, 9, 2, 4, 24, 80, 0, out, 32);
        CHECK(n == 1);
        CHECK(out[0].visibleRow == 3 && out[0].column0 == 4 && out[0].column1 == 79);
    }

    // ---- the band is clamped into the live screen ----
    {
        const int n = hiliteRegionRowSpans(200, 1, 1, 999, 5, 80, 0, out, 32);
        CHECK(n == 5); // capped at rows, not at the host's 999
        CHECK(out[0].column0 == 79); // start column capped at the last column
        CHECK(out[4].visibleRow == 4);
    }

    // ---- an inverted band emits nothing ----
    {
        CHECK(hiliteRegionRowSpans(1, 4, 6, 2, 24, 80, 0, out, 32) == 0);
    }

    // ---- viewOffset moves screen rows into the viewport, dropping those past it ----
    {
        const int n = hiliteRegionRowSpans(1, 1, 1, 3, 4, 80, 2, out, 32);
        CHECK(n == 2); // rows 0,1,2 -> visible 2,3,4; the last falls outside 4 rows
        CHECK(out[0].visibleRow == 2);
        CHECK(out[1].visibleRow == 3);
    }

    // ---- degenerate arguments are refused rather than written ----
    {
        CHECK(hiliteRegionRowSpans(1, 1, 1, 3, 0, 80, 0, out, 32) == 0);
        CHECK(hiliteRegionRowSpans(1, 1, 1, 3, 24, 0, 0, out, 32) == 0);
        CHECK(hiliteRegionRowSpans(1, 1, 1, 3, 24, 80, 0, out, 0) == 0);
        CHECK(hiliteRegionRowSpans(1, 1, 1, 3, 24, 80, 0, nullptr, 32) == 0);
    }

    // ---- maxOut bounds the write ----
    {
        const int n = hiliteRegionRowSpans(1, 1, 1, 10, 24, 80, 0, out, 4);
        CHECK(n == 4);
    }
}

// A cell big enough to hold a badge, with the caret parked mid-row.
static PasswordLockParameters lockParameters()
{
    PasswordLockParameters p;
    p.grid = { 40, 5 };
    p.cellSizeInPixels = { 9.0f, 18.0f };
    p.origin = { 6.0f, 0.0f };
    p.cursorRow = 1;
    p.cursorColumn = 10;
    p.inkRed = 0.1f;
    p.inkGreen = 0.1f;
    p.inkBlue = 0.1f;
    p.groundRed = 0.99f;
    p.groundGreen = 0.96f;
    p.groundBlue = 0.89f;
    return p;
}

TEST(passwordLockCases)
{
    // ---- the badge stands in the cell following the caret, on the caret's row ----
    //
    // Asserted against the cell rectangle rather than against a recorded pixel:
    // the contract is which cell it occupies, and a number copied out of a
    // passing run agrees with whatever that run did.
    {
        const PasswordLockParameters p = lockParameters();
        const PasswordLockGeometry g = resolvePasswordLockGeometry(p);
        CHECK(g.doesFit);
        const float nextCellLeft = p.origin.x + static_cast<float>(p.cursorColumn + 1) * p.cellSizeInPixels.width;
        const float nextCellRight = nextCellLeft + p.cellSizeInPixels.width;
        CHECK(g.minX >= nextCellLeft);
        CHECK(g.maxX <= nextCellRight);
        const float rowTop = p.origin.y + static_cast<float>(p.cursorRow) * p.cellSizeInPixels.height;
        CHECK(g.minY >= rowTop);
        CHECK(g.maxY <= rowTop + p.cellSizeInPixels.height);
        // A pixel of air each side, so the body does not abut the caret block.
        CHECK(g.minX > nextCellLeft);
        CHECK(g.maxX < nextCellRight);
    }

    // ---- the body covers the shackle's bottom, which is what makes it a lock ----
    {
        const PasswordLockParameters p = lockParameters();
        const PasswordLockGeometry g = resolvePasswordLockGeometry(p);
        CHECK(g.doesFit);
        OverlayQuad quads[maxPasswordLockQuads];
        CHECK(buildPasswordLockQuads(p, g, quads, maxPasswordLockQuads) == maxPasswordLockQuads);
        const OverlayQuad& shackle = quads[0];
        const OverlayQuad& body = quads[1];
        // Order: the shackle is written first so the body's fill lands over it.
        CHECK(shackle.strokeWidth > 0.0f);
        CHECK(body.strokeWidth == 0.0f);
        // The shackle's bottom edge is inside the body, not above it — above it
        // and the hollow rect closes with a visible bar across the lock.
        CHECK(shackle.maxY > body.minY);
        CHECK(shackle.maxY <= body.maxY);
        // The shackle is narrower than the body and centred within it.
        CHECK(shackle.minX > body.minX);
        CHECK(shackle.maxX < body.maxX);
        CHECK(approx((shackle.minX + shackle.maxX) * 0.5f, (body.minX + body.maxX) * 0.5f));
        // Square corners: the software compositor implements no radius, so a
        // non-zero one here is a shape that differs by which arm drew it.
        CHECK(shackle.cornerRadius == 0.0f);
        CHECK(body.cornerRadius == 0.0f);
        // Drawn solid. A translucent badge thickens where a translucent surface
        // shows the desktop through it.
        CHECK(approx(shackle.alpha, 1.0f));
        CHECK(approx(body.alpha, 1.0f));
    }

    // ---- the ink is mixed toward the ground, not taken raw ----
    //
    // Identified by the relation to its two inputs rather than by a literal:
    // the badge is secondary to the text beside it, and that is a statement
    // about where the colour sits between ink and ground.
    {
        PasswordLockParameters p = lockParameters();
        const PasswordLockGeometry g = resolvePasswordLockGeometry(p);
        OverlayQuad quads[maxPasswordLockQuads];
        CHECK(buildPasswordLockQuads(p, g, quads, maxPasswordLockQuads) == maxPasswordLockQuads);
        CHECK(quads[1].red > p.inkRed);
        CHECK(quads[1].red < p.groundRed);
    }

    // ---- a caret with no cell after it gets no badge ----
    {
        PasswordLockParameters p = lockParameters();
        p.cursorColumn = p.grid.columns - 1;
        CHECK(!resolvePasswordLockGeometry(p).doesFit);
    }

    // ---- no caret, no badge: it is placed relative to one ----
    {
        PasswordLockParameters p = lockParameters();
        p.cursorRow = -1;
        CHECK(!resolvePasswordLockGeometry(p).doesFit);
        p = lockParameters();
        p.cursorColumn = -1;
        CHECK(!resolvePasswordLockGeometry(p).doesFit);
    }

    // ---- a cell too small for a recognisable lock draws none ----
    //
    // A shape nobody recognises is not an indicator, so the badge is dropped
    // rather than shrunk into a smudge.
    {
        PasswordLockParameters p = lockParameters();
        p.cellSizeInPixels = { 9.0f, 10.0f };
        CHECK(!resolvePasswordLockGeometry(p).doesFit);
        p = lockParameters();
        p.cellSizeInPixels = { 2.0f, 18.0f };
        CHECK(!resolvePasswordLockGeometry(p).doesFit);
    }

    // ---- a geometry that does not fit writes nothing, whatever the room ----
    {
        PasswordLockParameters p = lockParameters();
        p.cursorRow = -1;
        const PasswordLockGeometry g = resolvePasswordLockGeometry(p);
        OverlayQuad quads[maxPasswordLockQuads];
        CHECK(buildPasswordLockQuads(p, g, quads, maxPasswordLockQuads) == 0);
    }

    // ---- a partial slot writes nothing rather than half a lock ----
    //
    // Half a padlock is a filled rectangle, which reads as a cursor.
    {
        const PasswordLockParameters p = lockParameters();
        const PasswordLockGeometry g = resolvePasswordLockGeometry(p);
        OverlayQuad quads[maxPasswordLockQuads];
        CHECK(buildPasswordLockQuads(p, g, quads, maxPasswordLockQuads - 1) == 0);
        CHECK(buildPasswordLockQuads(p, g, nullptr, maxPasswordLockQuads) == 0);
    }
}

UBIQUITTY_TEST_MAIN()
