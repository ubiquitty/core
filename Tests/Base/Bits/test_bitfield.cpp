// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_bitfield.cpp — unit tests for Bits::BitField<Storage, Shift, Width>, the
// constexpr subfield arithmetic the packed Pen bytes share (ExtendedAttributes'
// SGR 1:n weight, underlineStyle's shape/script/ideogram, the font byte's style
// and proportional flag). Pins: get/set round-trip, neighbour preservation,
// width truncation, and the real Pen field layouts against their SGR appliers.

#include "Base/Bits/BitField.h"
#include "Core/Pen.h"
#include "Core/VT/Constants.h"

#include <cstdint>
#include <cstdio>

using Ubiquitty::Bits::BitField;

TEST(testGetSetRoundTripAndNeighbours)
{
    using Middle = BitField<uint8_t, 3, 2>; // bits 3-4
    CHECK(Middle::mask == 0x18);
    CHECK(Middle::maxValue == 3);

    // A byte with every other bit set: writes to the field must not disturb them.
    const uint8_t neighbours = static_cast<uint8_t>(~Middle::mask);
    uint8_t b = neighbours;
    b = Middle::set(b, 2);
    CHECK(Middle::get(b) == 2);
    CHECK((b & ~Middle::mask) == neighbours); // neighbours untouched
    b = Middle::set(b, 0);
    CHECK(Middle::get(b) == 0);
    CHECK(b == neighbours); // clearing the field restores the original byte

    // Width truncation: a value wider than the field is masked, never smeared
    // into the neighbouring bits.
    b = Middle::set(0, 0xFF);
    CHECK(Middle::get(b) == Middle::maxValue);
    CHECK((b & ~Middle::mask) == 0);
}

TEST(testFullWidthAndEdges)
{
    using Low = BitField<uint8_t, 0, 3>;
    using High = BitField<uint8_t, 7, 1>;
    using Whole = BitField<uint16_t, 0, 16>;
    CHECK(Low::mask == 0x07);
    CHECK(High::mask == 0x80);
    CHECK(Whole::maxValue == 0xFFFF);
    CHECK(Whole::get(Whole::set(0, 0xABCD)) == 0xABCD);
    CHECK(High::get(High::set(0x7F, 1)) == 1);
    CHECK(High::set(0x7F, 1) == 0xFF);

    // A field spanning the whole 32-bit storage: maxValue must be all-ones
    // without shifting by the type width (which would be UB — the reason
    // maxValue is spelt ~0 at full width). Under UBSan this pins that path.
    using Whole32 = BitField<uint32_t, 0, 32>;
    CHECK(Whole32::maxValue == 0xFFFFFFFFu);
    CHECK(Whole32::mask == 0xFFFFFFFFu);
    CHECK(Whole32::get(Whole32::set(0, 0xDEADBEEFu)) == 0xDEADBEEFu);
    // 64-bit full width: the same all-ones path on the widest storage.
    using Whole64 = BitField<uint64_t, 0, 64>;
    CHECK(Whole64::maxValue == 0xFFFFFFFFFFFFFFFFull);
    CHECK(Whole64::get(Whole64::set(0, 0x0123456789ABCDEFull)) == 0x0123456789ABCDEFull);
}

// The three packed Pen bytes coexist correctly: writing one subfield through
// its accessor leaves the byte's other subfields intact.
TEST(testPenPackedBytes)
{
    Ubiquitty::Pen p {};

    // underlineStyle: shape + script + ideogram share the byte.
    Ubiquitty::penSetUnderlineShape(&p, Ubiquitty::VT::underlineCurly);
    Ubiquitty::penSetScript(&p, Ubiquitty::VT::scriptSub);
    Ubiquitty::penSetIdeogram(&p, Ubiquitty::VT::ideogramStress);
    CHECK(Ubiquitty::penUnderlineShape(&p) == Ubiquitty::VT::underlineCurly);
    CHECK(Ubiquitty::penScript(&p) == Ubiquitty::VT::scriptSub);
    CHECK(Ubiquitty::penIdeogram(&p) == Ubiquitty::VT::ideogramStress);
    Ubiquitty::penSetScript(&p, Ubiquitty::VT::scriptNone);
    CHECK(Ubiquitty::penUnderlineShape(&p) == Ubiquitty::VT::underlineCurly); // neighbours survive the clear
    CHECK(Ubiquitty::penIdeogram(&p) == Ubiquitty::VT::ideogramStress);

    // The font byte: font style + proportional share it, and each accessor
    // writes its own subfield only.
    Ubiquitty::penSetFontStyle(&p, Ubiquitty::VT::fontFraktur);
    Ubiquitty::penSetProportional(&p, true);
    CHECK(Ubiquitty::penFontStyle(&p) == Ubiquitty::VT::fontFraktur);
    CHECK(Ubiquitty::penProportional(&p));
    Ubiquitty::penSetProportional(&p, false);
    CHECK(Ubiquitty::penFontStyle(&p) == Ubiquitty::VT::fontFraktur); // neighbour survives the clear
    CHECK(!Ubiquitty::penProportional(&p));
    Ubiquitty::penSetProportional(&p, true);
    Ubiquitty::penSetFontStyle(&p, Ubiquitty::VT::fontPrimary);
    CHECK(Ubiquitty::penProportional(&p)); // and the clear in the other direction
    CHECK(Ubiquitty::penFontStyle(&p) == Ubiquitty::VT::fontPrimary);

    // ExtendedAttributes: the weight subfield rides above the flag bits.
    Ubiquitty::VT::ExtendedAttributes e;
    e.flags.add(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
    e.setWeight(7);
    CHECK(e.weight() == 7);
    CHECK(e.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));
    e.setWeight(0);
    CHECK(e.weight() == 0);
    CHECK(e.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast));
}

UBIQUITTY_TEST_MAIN()
