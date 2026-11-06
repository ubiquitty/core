// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/RowMeta.h"

#include "Harness.h"
#include <initializer_list>
#include <utility>

using Ubiquitty::TextScale;
using Ubiquitty::textScaleForLineAttribute;

TEST(dec_attributes_map_onto_scales)
{
    const TextScale normal = textScaleForLineAttribute(Ubiquitty::lineAttributeNormal);
    CHECK_EQ(int(normal.widthScale), 1);
    CHECK_EQ(int(normal.heightScale), 1);
    CHECK(normal.half == TextScale::Half::Whole);
    CHECK(!normal.wide());
    CHECK(normal.xStep() == 1.0f);

    // DECDWL doubles the advance without a taller master.
    const TextScale wide = textScaleForLineAttribute(Ubiquitty::lineAttributeDoubleWidth);
    CHECK_EQ(int(wide.widthScale), 2);
    CHECK_EQ(int(wide.heightScale), 1);
    CHECK(wide.half == TextScale::Half::Whole);
    CHECK(wide.xStep() == 2.0f);

    // Both DECDHL halves are double-WIDTH as well; they differ only in half.
    const TextScale top = textScaleForLineAttribute(Ubiquitty::lineAttributeDoubleHeightTop);
    CHECK_EQ(int(top.widthScale), 2);
    CHECK_EQ(int(top.heightScale), 2);
    CHECK(top.half == TextScale::Half::Top);
    CHECK(top.wide());
    CHECK(top.xStep() == 2.0f);

    const TextScale bottom = textScaleForLineAttribute(Ubiquitty::lineAttributeDoubleHeightBottom);
    CHECK_EQ(int(bottom.widthScale), 2);
    CHECK_EQ(int(bottom.heightScale), 2);
    CHECK(bottom.half == TextScale::Half::Bottom);
    CHECK(bottom.wide());
}

TEST(every_value_outside_the_dec_set_is_unscaled)
{
    // All 256, not a sample: the field is a byte and only four values mean
    // anything, so the other 252 have to be inert rather than accidentally
    // matching a case.
    for (int value = 0; value <= 255; ++value) {
        const auto attribute = static_cast<uint8_t>(value);
        const TextScale out = textScaleForLineAttribute(attribute);
        const bool isDECValue = attribute == Ubiquitty::lineAttributeNormal || attribute == Ubiquitty::lineAttributeDoubleWidth
            || attribute == Ubiquitty::lineAttributeDoubleHeightTop || attribute == Ubiquitty::lineAttributeDoubleHeightBottom;
        if (isDECValue)
            continue;
        CHECK_EQ(int(out.widthScale), 1);
        CHECK_EQ(int(out.heightScale), 1);
        CHECK(out.half == TextScale::Half::Whole);
    }
}

TEST(a_high_bit_must_never_read_as_a_dec_attribute)
{
    // RowMeta is at its 4-byte assert, so the temptation is to pack an unrelated
    // flag into lineAttribute's six unused bits. That must not work: this field
    // is an enum, and a reader comparing against lineAttributeNormal or
    // switching on it would silently stop matching, or worse, start matching the
    // wrong case. Every DEC value with a high bit set must read as unscaled.
    for (uint8_t dec : std::initializer_list<uint8_t> { Ubiquitty::lineAttributeNormal, Ubiquitty::lineAttributeDoubleWidth,
             Ubiquitty::lineAttributeDoubleHeightTop, Ubiquitty::lineAttributeDoubleHeightBottom }) {
        for (int bit = 2; bit < 8; ++bit) {
            const auto polluted = static_cast<uint8_t>(dec | (1u << bit));
            const TextScale out = textScaleForLineAttribute(polluted);
            CHECK_EQ(int(out.widthScale), 1);
            CHECK_EQ(int(out.heightScale), 1);
            CHECK(out.half == TextScale::Half::Whole);
        }
    }
}

TEST(scales_reach_past_what_dec_can_express)
{
    // The kitty protocol carries s and w to 7 and decouples the two axes; the
    // type has to hold that even though no DEC attribute selects it.
    TextScale triple;
    triple.widthScale = 3;
    triple.heightScale = 3;
    CHECK(triple.xStep() == 3.0f);
    CHECK(triple.wide());

    TextScale tallOnly;
    tallOnly.heightScale = 2;
    CHECK(!tallOnly.wide());
    CHECK(tallOnly.xStep() == 1.0f);
}

UBIQUITTY_TEST_MAIN()
