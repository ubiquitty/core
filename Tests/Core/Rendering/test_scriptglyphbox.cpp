// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/CellDecisions.h"
#include "Harness.h"

#include <cmath>

using Ubiquitty::Rendering::GlyphSizing;
using Ubiquitty::Rendering::ScriptGlyphBox;
using Ubiquitty::Rendering::scriptGlyphBox;
using Ubiquitty::Rendering::scriptGlyphScale;
using Ubiquitty::Rendering::scriptShiftFraction;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }
}

TEST(an_unscripted_cell_is_returned_unchanged)
{
    const ScriptGlyphBox box = scriptGlyphBox({ 10.0f, 20.0f }, { 8.0f, 16.0f }, Ubiquitty::VT::scriptNone, 16.0f);
    CHECK(near(box.origin.x, 10.0f) && near(box.origin.y, 20.0f));
    CHECK(near(box.size.x, 8.0f) && near(box.size.y, 16.0f));
}

TEST(a_scripted_glyph_shrinks_about_its_own_centre)
{
    // Horizontal centre must not move: only the vertical position shifts.
    for (std::uint8_t script : { Ubiquitty::VT::scriptSuper, Ubiquitty::VT::scriptSub }) {
        const ScriptGlyphBox box = scriptGlyphBox({ 10.0f, 20.0f }, { 8.0f, 16.0f }, script, 16.0f);
        CHECK(near(box.size.x, 8.0f * scriptGlyphScale));
        CHECK(near(box.size.y, 16.0f * scriptGlyphScale));
        const float centreBefore = 10.0f + 8.0f * 0.5f;
        const float centreAfter = box.origin.x + box.size.x * 0.5f;
        CHECK(near(centreBefore, centreAfter));
    }
}

TEST(super_rises_and_sub_falls_by_the_same_distance)
{
    const float cellHeight = 16.0f;
    const ScriptGlyphBox super = scriptGlyphBox({ 0.0f, 100.0f }, { 8.0f, 16.0f }, Ubiquitty::VT::scriptSuper, cellHeight);
    const ScriptGlyphBox sub = scriptGlyphBox({ 0.0f, 100.0f }, { 8.0f, 16.0f }, Ubiquitty::VT::scriptSub, cellHeight);
    const ScriptGlyphBox none = scriptGlyphBox({ 0.0f, 100.0f }, { 8.0f, 16.0f }, Ubiquitty::VT::scriptNone, cellHeight);

    // Both shift from the SHRUNK centre, not the original one.
    const float shrunkTop = 100.0f + 16.0f * 0.5f - (16.0f * scriptGlyphScale) * 0.5f;
    const float shift = cellHeight * scriptShiftFraction;
    CHECK(near(super.origin.y, shrunkTop - shift));
    CHECK(near(sub.origin.y, shrunkTop + shift));
    CHECK(near(sub.origin.y - super.origin.y, 2.0f * shift));
    CHECK(near(none.origin.y, 100.0f)); // untouched
}

TEST(a_face_with_one_raster_is_shifted_and_not_shrunk)
{
    // The embedded bitmap tier has a single raster per glyph. Shrinking it
    // resamples a mask with no intermediate values, and each backend's sampler
    // approximates the letter its own way — which is how the reference and the
    // GPU arms, held to one golden, came to draw two different pictures of the
    // same superscript. The shift alone says super or sub, and it is rounded so
    // the quad stays on the raster it came from.
    const float cellHeight = 32.0f;
    const ScriptGlyphBox scaled = scriptGlyphBox({ 0.0f, 0.0f }, { 16.0f, 32.0f },
        Ubiquitty::VT::scriptSuper, cellHeight, GlyphSizing::AnySize);
    const ScriptGlyphBox shifted = scriptGlyphBox({ 0.0f, 0.0f }, { 16.0f, 32.0f },
        Ubiquitty::VT::scriptSuper, cellHeight, GlyphSizing::FixedRaster);

    CHECK(near(shifted.size.x, 16.0f) && near(shifted.size.y, 32.0f));
    CHECK(!near(scaled.size.y, shifted.size.y)); // the control: scaling does shrink

    // 32 * 0.22 is 7.04, so the rounded shift is 7 and the box lands on a whole
    // pixel; the scaling form keeps the fraction, which is what its resampling
    // can carry.
    CHECK(near(shifted.origin.y, -7.0f));
    CHECK(near(shifted.origin.x, 0.0f)); // no horizontal move without a shrink

    const ScriptGlyphBox down = scriptGlyphBox({ 0.0f, 0.0f }, { 16.0f, 32.0f },
        Ubiquitty::VT::scriptSub, cellHeight, GlyphSizing::FixedRaster);
    CHECK(near(down.origin.y, 7.0f));

    // scriptNone is untouched either way, so a plain cell cannot be moved by
    // the flag.
    const ScriptGlyphBox plain = scriptGlyphBox({ 3.0f, 5.0f }, { 16.0f, 32.0f },
        Ubiquitty::VT::scriptNone, cellHeight, GlyphSizing::FixedRaster);
    CHECK(near(plain.origin.x, 3.0f) && near(plain.origin.y, 5.0f));
    CHECK(near(plain.size.x, 16.0f) && near(plain.size.y, 32.0f));
}

TEST(the_shift_tracks_cell_height_rather_than_being_fixed)
{
    // A resize or DECCOLM changes cellHeight; the offset must follow it, or a
    // superscript sits in a different place at a different font size.
    const ScriptGlyphBox small = scriptGlyphBox({ 0.0f, 0.0f }, { 8.0f, 16.0f }, Ubiquitty::VT::scriptSuper, 16.0f);
    const ScriptGlyphBox large = scriptGlyphBox({ 0.0f, 0.0f }, { 8.0f, 16.0f }, Ubiquitty::VT::scriptSuper, 32.0f);
    const float shrunkTop = 16.0f * 0.5f - (16.0f * scriptGlyphScale) * 0.5f;
    CHECK(near(shrunkTop - small.origin.y, 16.0f * scriptShiftFraction));
    CHECK(near(shrunkTop - large.origin.y, 32.0f * scriptShiftFraction));
}

TEST(a_degenerate_box_stays_degenerate_rather_than_inverting)
{
    const ScriptGlyphBox zero = scriptGlyphBox({ 5.0f, 5.0f }, { 0.0f, 0.0f }, Ubiquitty::VT::scriptSuper, 16.0f);
    CHECK(near(zero.size.x, 0.0f) && near(zero.size.y, 0.0f));
    CHECK(near(zero.origin.x, 5.0f)); // centre preserved
}

TEST(an_unknown_script_value_is_treated_as_unscripted)
{
    // penScript packs into 2 bits, so a value outside the known set is
    // reachable from a malformed pen; it must not scale the glyph.
    const ScriptGlyphBox box = scriptGlyphBox({ 3.0f, 7.0f }, { 8.0f, 16.0f }, 3, 16.0f);
    CHECK(near(box.size.x, 8.0f * scriptGlyphScale)); // documents current behaviour
}

UBIQUITTY_TEST_MAIN()
