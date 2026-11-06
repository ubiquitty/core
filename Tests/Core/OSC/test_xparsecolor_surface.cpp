// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_xparsecolor_surface — what the colour parser accepts, pinned as values.
//
// The parser walks a `std::string_view`, consumed from the front, and decides a
// specification has ended by looking one past its last consumed character. That
// cursor arrived as a `const char*` reading its way to a NUL, and the move to a
// view was a change the compiler could not check: every helper takes a string
// either way, so a cursor scoped one byte wrong compiles and quietly accepts or
// rejects a different set of inputs. These cases are what the move answered to,
// and what the next one will.
//
// So the terminator is the axis they are built on. Every form appears three
// times — ending at the text's end, ending at `;`, and truncated mid-token —
// because those are the three the cursor has to keep telling apart, and a suite
// that only feeds whole well-formed specs agrees with a broken cursor on every
// one of them.
//
// The values are what the parser produces today, not what a colour-space
// reference says it should. The file's contract is to reproduce Xlib's answers,
// so a difference from Xlib is a finding about this file and a difference from
// these goldens is a finding about the change that caused it.

#include "Core/OSC/XParseColor.h"

#include <cstdint>
#include <optional>
#include <string_view>

using namespace std::string_view_literals;

namespace {

using Ubiquitty::PaletteRGB;
using Ubiquitty::OSC::XParseColor::parseColorSpecification;

// One case: the text a client sends, and what the parser makes of it.
struct Case {
    const char* spec;
    bool isAccepted;
    uint16_t red;
    uint16_t green;
    uint16_t blue;
};

void check(const Case& c)
{
    const std::optional<PaletteRGB> parsed = parseColorSpecification(c.spec);
    CHECK_MSG(parsed.has_value() == c.isAccepted, c.spec);
    if (!parsed.has_value() || !c.isAccepted) {
        if (parsed.has_value() != c.isAccepted)
            std::printf("  %s: want %s, got %s\n", c.spec,
                c.isAccepted ? "accepted" : "rejected",
                parsed.has_value() ? "accepted" : "rejected");
        return;
    }
    const bool matches = parsed->red == c.red && parsed->green == c.green && parsed->blue == c.blue;
    CHECK_MSG(matches, c.spec);
    if (!matches) {
        std::printf("  %s: want %u/%u/%u, got %u/%u/%u\n", c.spec,
            c.red, c.green, c.blue, parsed->red, parsed->green, parsed->blue);
    }
}

} // namespace

// The hash form at each digit width Xlib admits. A width it does not admit is
// rejected rather than rounded, which is the behaviour a rewrite is most likely
// to lose: the length check runs before the terminator check.
TEST(testHashFormWidths)
{
    const Case cases[] = {
        // A short form replicates the high byte rather than left-justifying, so #f00
        // is 0xF0F0 and not 0xFFFF. That differs from Xlib, which left-justifies;
        // it is pinned here as this parser's behaviour, not endorsed as correct.
        { "#f00", true, 0xF0F0, 0x0000, 0x0000 },
        { "#ff0000", true, 0xFFFF, 0x0000, 0x0000 },
        { "#fff000000", true, 0xFFFF, 0x0000, 0x0000 },
        { "#ffff00000000", true, 0xFFFF, 0x0000, 0x0000 },
        { "#0f0", true, 0x0000, 0xF0F0, 0x0000 },
        { "#00f", true, 0x0000, 0x0000, 0xF0F0 },
        { "#888", true, 0x8080, 0x8080, 0x8080 },
        // Widths between the admitted ones, and one past the longest.
        { "#ff", false, 0, 0, 0 },
        { "#ffff", false, 0, 0, 0 },
        { "#fffff", false, 0, 0, 0 },
        { "#fffffffffffff", false, 0, 0, 0 },
        { "#", false, 0, 0, 0 },
        { "#gg0", false, 0, 0, 0 },
    };
    for (const Case& c : cases)
        check(c);
}

// The device-dependent forms. `rgb:` is slash-separated hex per channel and
// `rgbi:` is floating intensity, and they share a prefix — so a prefix match that
// consumes one byte too many turns every `rgbi:` into a malformed `rgb:`.
TEST(testDeviceDependentForms)
{
    const Case cases[] = {
        { "rgb:ff/00/00", true, 0xFFFF, 0x0000, 0x0000 },
        { "rgb:f/0/0", true, 0xFFFF, 0x0000, 0x0000 },
        { "rgb:ffff/0000/0000", true, 0xFFFF, 0x0000, 0x0000 },
        { "rgb:00/ff/00", true, 0x0000, 0xFFFF, 0x0000 },
        { "rgbi:1.0/0.0/0.0", true, 0xFFFF, 0x0000, 0x0000 },
        { "rgbi:0.0/0.0/0.0", true, 0x0000, 0x0000, 0x0000 },
        // Rejections: a missing channel, a missing separator, an empty payload.
        { "rgb:ff/00", false, 0, 0, 0 },
        { "rgb:ff0000", false, 0, 0, 0 },
        { "rgb:", false, 0, 0, 0 },
        { "rgbi:", false, 0, 0, 0 },
        { "rgbi:1.0/0.0", false, 0, 0, 0 },
    };
    for (const Case& c : cases)
        check(c);
}

// The terminator, which is the whole reason this file exists. A specification
// ends where the text does or at `;`, and the parser looks one past its last
// digit to find out. A cursor that stops one byte short accepts the first column and a cursor
// that runs one byte long rejects the second.
TEST(testSpecificationTerminator)
{
    const Case cases[] = {
        // Ends where the text does.
        { "#f00", true, 0xF0F0, 0x0000, 0x0000 },
        { "rgb:ff/00/00", true, 0xFFFF, 0x0000, 0x0000 },
        // Ends at ';' — accepted, and the remainder is not the parser's.
        { "#f00;", true, 0xF0F0, 0x0000, 0x0000 },
        { "#f00;anything at all", true, 0xF0F0, 0x0000, 0x0000 },
        { "rgb:ff/00/00;more", true, 0xFFFF, 0x0000, 0x0000 },
        { "rgbi:1.0/0.0/0.0;", true, 0xFFFF, 0x0000, 0x0000 },
        // Whitespace ahead of the terminator is skipped, so these are accepted.
        // atSpecificationEnd runs a skip loop first, and a cursor rewrite that
        // drops it turns every one of these into a rejection.
        { "#f00 ", true, 0xF0F0, 0x0000, 0x0000 },
        { "#f00\t", true, 0xF0F0, 0x0000, 0x0000 },
        { "#f00  \t ;rest", true, 0xF0F0, 0x0000, 0x0000 },
        { "rgb:ff/00/00 ", true, 0xFFFF, 0x0000, 0x0000 },
        // Trailing text that is not whitespace or a terminator: rejected.
        { "#f00x", false, 0, 0, 0 },
        { "rgb:ff/00/00x", false, 0, 0, 0 },
        { "#f00/", false, 0, 0, 0 },
    };
    for (const Case& c : cases)
        check(c);
}

// The device-independent spaces. Each carries Xcms' range check, and a spec
// outside it is rejected rather than clamped — so a rewrite that loses a check
// shows up here as an acceptance rather than as a wrong colour.
TEST(testDeviceIndependentSpaces)
{
    const Case cases[] = {
        { "CIEXYZ:0.0/0.0/0.0", true, 0x0000, 0x0000, 0x0000 },
        { "CIEuvY:0.2/0.35/0.5", true, 52171, 43690, 65535 },
        { "CIExyY:0.3/0.3/0.5", true, 53713, 46774, 47031 },
        // The three neutral-lightness forms agree, which is the cheapest check
        // that the shared Lab pipeline still runs for each of them.
        { "CIELab:50.0/0.0/0.0", true, 33667, 32125, 32382 },
        { "CIELuv:50.0/0.0/0.0", true, 33667, 32125, 32382 },
        { "TekHVC:0.0/50.0/0.0", true, 33667, 32125, 32382 },
        // Out of the range each space checks.
        { "CIExyY:2.0/0.3/0.5", false, 0, 0, 0 },
        { "CIExyY:0.3/0.3/2.0", false, 0, 0, 0 },
        { "CIEuvY:0.2/0.35/2.0", false, 0, 0, 0 },
        // Malformed rather than out of range.
        { "CIEXYZ:0.0/0.0", false, 0, 0, 0 },
        { "CIEXYZ:", false, 0, 0, 0 },
        { "CIEXYZ:a/b/c", false, 0, 0, 0 },
    };
    for (const Case& c : cases)
        check(c);
}

// Inputs with no colour in them at all. A rewrite that reads before the start or
// past the end shows up here first, because there is nothing valid to mask it.
TEST(testDegenerateInputs)
{
    const Case cases[] = {
        { "", false, 0, 0, 0 },
        { ";", false, 0, 0, 0 },
        { "rgb", false, 0, 0, 0 },
        { "CIE", false, 0, 0, 0 },
        { "nosuchform:1/2/3", false, 0, 0, 0 },
        { "/", false, 0, 0, 0 },
        { "//", false, 0, 0, 0 },
    };
    for (const Case& c : cases)
        check(c);
    // The table above cannot hold this one: a NUL inside the text ends a C string
    // and so ends the literal. The view carries its own length, which makes the
    // NUL an ordinary trailing byte and the colour a rejection, on the same terms
    // as `#f00x`. The `const char*` this parameter replaced could not be handed
    // such an input, so this is a new answer rather than a preserved one.
    CHECK_MSG(!parseColorSpecification("#f00\0junk"sv).has_value(),
        "a NUL inside the view is payload, not a terminator");
}

// Colours inside each space's own range check and outside the display's gamut,
// which is the only way into the chroma-compression branch. Every case above is
// either rejected by a range check or already representable, so the compression
// path — queryMaxChroma, its surface search, and the zero-intensity end it picks
// — ran in none of them.
//
// The tell is a channel of 2313 rather than 0: a compressed colour reads its
// zero off the HIGHEST device value whose measured intensity is still zero,
// where an uncompressed one reads the lowest. Two controls fail here and pass
// against the rest of this file: dropping that choice, and leaving the hue the
// HVC round trip perturbs instead of restoring the one asked for.
TEST(testGamutCompression)
{
    const Case cases[] = {
        { "CIELab:50.0/100.0/-100.0", true, 2313, 31868, 50372 },
        { "CIELab:50.0/-100.0/100.0", true, 2313, 31868, 49858 },
        { "CIELab:30.0/70.0/-80.0", true, 2313, 21331, 31611 },
        { "CIELuv:70.0/-120.0/130.0", true, 2313, 50886, 29041 },
        { "TekHVC:120.0/50.0/150.0", true, 2313, 37265, 13878 },
        { "TekHVC:240.0/70.0/180.0", true, 2313, 45489, 63993 },
        { "CIEXYZ:0.9/0.1/0.9", true, 44718, 4883, 32125 },
        { "CIExyY:0.05/0.9/0.4", true, 2313, 50115, 33667 },
    };
    for (const Case& c : cases)
        check(c);
}

// Failing every colour space, a specification is a name from X.Org's colour
// database, matched without regard to case or spaces, its 8-bit entry widened
// by replication as Xlib widens it.
TEST(testColorNames)
{
    const Case cases[] = {
        { "red", true, 0xFFFF, 0x0000, 0x0000 },
        { "light sky blue", true, 0x8787, 0xCECE, 0xFAFA },
        { "LightSkyBlue", true, 0x8787, 0xCECE, 0xFAFA },
        { "Gray50", true, 0x7F7F, 0x7F7F, 0x7F7F },
        { "nosuch", false, 0, 0, 0 },
        { "", false, 0, 0, 0 },
        { "red\x01", false, 0, 0, 0 },
        { "averyveryveryveryveryverylongcolourname", false, 0, 0, 0 },
    };
    for (const Case& c : cases)
        check(c);
}

UBIQUITTY_TEST_MAIN()
