// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_characterset_96set.cpp — every GR byte through the 96-character set
// translation, and every GL byte through the four 94-character set slots.
//
// Two facts shape the sweep:
//
//   - The input encoding has to be single-byte. Under UTF-8 each byte in
//     0xA0..0xFF is a lead or continuation byte, GR never engages, and no
//     96-character set is consulted.
//   - SOFT HYPHEN (0xAD) is zero-width and writes no cell, so a column index
//     drifts away from the byte that wrote it. Each byte is fed on its own and
//     credited with the column the cursor held just before it.
//
// What the cases assert is the shape of the table, not its contents. Checking
// values needs DEC STD 070 as an outside reference, and expectations copied
// from the switch under test would prove nothing. The shape is still enough to
// catch the failures that matter:
//
//   - each byte that writes a cell writes a code point;
//   - two bytes share a code point only where an undefined position falls
//     through to identity, so a repeated case arm cannot hide;
//   - a set translates the same whichever slot it was designated into;
//   - Latin-1 is the identity, which needs no outside reference;
//   - DEC Supplemental differs from Latin-1, so its checks are not running
//     against a fallthrough.

#include "Harness.h"

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>

namespace {

using Ubiquitty::Unicode::Encoding;

/// The code point each fed byte left in the cell it wrote, keyed by that byte.
///
/// A byte that wrote no cell has no entry.
using ByteTranslation = std::map<int, std::uint32_t>;

/// A contiguous run of bytes to feed, and the input encoding they arrive under.
struct ByteSweep {
    int firstByte;
    int lastByte;
    std::optional<Encoding> inputEncoding;
};

constexpr ByteSweep graphicRightSweep { 0xA0, 0xFF, Encoding::Latin1 };
constexpr ByteSweep graphicLeftSweep { 0x20, 0x7E, std::nullopt };

// ESC - <F> designates a 96-character set into G1, and LS1R (ESC ~) locks G1
// into GR.
constexpr std::string_view decSupplementalInGR = "\033-<\033~";
constexpr std::string_view latin1InGR = "\033-A\033~";

// ESC ( 0 designates DEC Special Graphics into G0, which GL already shows.
constexpr std::string_view specialGraphicsInG0 = "\033(0";

/// A designation sequence, and what its agreeing with the reference route means.
struct DesignationRoute {
    std::string_view sequence;
    const char* label;
};

/// Feeds `sweep` one byte at a time after `designation` and records what each
/// byte wrote.
///
/// The cursor column is read before every byte, so a byte is credited with the
/// cell it actually wrote, and one that leaves the cursor in place is recorded
/// as having written nothing.
ByteTranslation translate(const ByteSweep& sweep, std::string_view designation)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, 4, 200);
    if (sweep.inputEncoding)
        Ubiquitty::Modes::setInputEncoding(terminal.get(), *sweep.inputEncoding);
    Ubiquitty::processBytes(terminal.get(), designation);

    ByteTranslation translation;
    for (int value = sweep.firstByte; value <= sweep.lastByte; ++value) {
        const int columnBefore = terminal->screens.active->cursorX;
        const char byte = static_cast<char>(value);
        Ubiquitty::processBytes(terminal.get(), std::string_view(&byte, 1));
        if (terminal->screens.active->cursorX != columnBefore) {
            const Ubiquitty::Cell* written = terminal->grid->cellAt(Ubiquitty::Row(0), Ubiquitty::Column(columnBefore));
            translation[value] = written ? written->codePoint : 0u;
        }
    }
    return translation;
}

/// Counts the bytes both translations cover and map to different code points.
int countDisagreements(const ByteTranslation& subject, const ByteTranslation& reference)
{
    return static_cast<int>(std::ranges::count_if(subject, [&](const auto& entry) {
        const auto match = reference.find(entry.first);
        return match != reference.end() && match->second != entry.second;
    }));
}

TEST(testEveryOccupyingByteProducesACodePoint)
{
    const ByteTranslation translation = translate(graphicRightSweep, decSupplementalInGR);

    // SOFT HYPHEN is zero-width by design; each of the other 95 bytes writes a cell.
    CHECK_MSG(translation.size() == 95, "95 of the 96 bytes occupy a cell");
    CHECK_MSG(!translation.contains(0xAD), "0xAD SOFT HYPHEN occupies no cell");

    // A cell written with nothing in it is a blank where a graphic character belongs.
    for (const std::uint32_t codePoint : std::views::values(translation))
        CHECK_MSG(codePoint != 0u, "every byte that writes a cell writes a code point");
}

// Two bytes landing on one code point is the trace a duplicated case arm leaves
// among the 96 written by hand. Exactly one shape of sharing is legitimate:
// DEC-MCS leaves some positions undefined, an undefined position passes through
// to its Latin-1 identity, and that identity can equal a defined position
// elsewhere. 0xA4, undefined, shares the currency sign with 0xA8; 0xFF,
// undefined, shares its code point with 0xF7. Sharing where neither byte maps
// to itself is a duplicated arm.
TEST(testCollisionsAreOnlyIdentityPassthrough)
{
    const ByteTranslation translation = translate(graphicRightSweep, decSupplementalInGR);
    // The walk below asserts only when two bytes share a code point, so an empty
    // translation would pass it untested; the count rules that out.
    CHECK_MSG(translation.size() == 95, "95 of the 96 bytes occupy a cell");

    std::map<std::uint32_t, int> firstByteWriting;
    for (const auto& [byte, codePoint] : translation) {
        const auto [earlier, isFirstWriter] = firstByteWriting.try_emplace(codePoint, byte);
        if (!isFirstWriter) {
            const bool isIdentityPassthrough = codePoint == static_cast<std::uint32_t>(byte)
                || codePoint == static_cast<std::uint32_t>(earlier->second);
            CHECK_MSG(isIdentityPassthrough,
                "two defined positions sharing a code point is a duplicated case arm");
        }
    }
}

// Latin-1 is documented as the identity map, which needs no outside reference.
TEST(testLatin1IsIdentity)
{
    for (const auto& [byte, codePoint] : translate(graphicRightSweep, latin1InGR))
        CHECK_MSG(codePoint == static_cast<std::uint32_t>(byte), "Latin-1 maps every byte to itself");
}

TEST(testTranslationIsDeterministicAcrossDesignations)
{
    // One 96-character set, designated into G1, G2 and G3 by ESC -, ESC . and
    // ESC /, and locked into GR by LS1R, LS2R and LS3R. The set's table does not
    // depend on the slot it sits in, so a route that disagrees with the G1 route
    // has resolved its designator somewhere the others do not.
    const ByteTranslation viaG1 = translate(graphicRightSweep, "\033-<\033~");

    // Two empty translations compare equal, so the agreement below means
    // something only once the reference route has filled its range.
    CHECK_MSG(!viaG1.empty(), "G1 via ESC - and LS1R produced a mapping");

    const DesignationRoute laterSlots[] = {
        { "\033.<\033}", "G2 via ESC . and LS2R agrees with G1" },
        { "\033/<\033|", "G3 via ESC / and LS3R agrees with G1" },
    };
    for (const DesignationRoute& route : laterSlots)
        CHECK_MSG(translate(graphicRightSweep, route.sequence) == viaG1, route.label);
}

// A silent fallthrough to Latin-1 would satisfy every DEC case above while
// exercising none of the table under test.
TEST(testDecSupplementalDiffersFromLatin1)
{
    const ByteTranslation decSupplemental = translate(graphicRightSweep, decSupplementalInGR);
    const ByteTranslation latin1 = translate(graphicRightSweep, latin1InGR);
    CHECK_MSG(countDisagreements(decSupplemental, latin1) > 0, "DEC Supplemental is not merely Latin-1");
}

// ---- The 94-character sets, G0 through G3 ------------------------------
//
// ESC ( ) * + designate into G0, G1, G2 and G3, and each slot reaches GL by
// its own route: G0 directly, G1 through SO, G2 and G3 through the locking
// shifts LS2 (ESC n) and LS3 (ESC o). A fault that belongs to one route
// shows only when all four are driven.

TEST(testASCIIIsIdentity)
{
    for (const auto& [byte, codePoint] : translate(graphicLeftSweep, ""))
        CHECK_MSG(codePoint == static_cast<std::uint32_t>(byte), "plain ASCII is the identity map");
}

// One set designated into each of the four slots translates the same. Reading
// the wrong G-set, or misparsing an intermediate byte, breaks exactly one route.
TEST(testAllFourSlotsAgree)
{
    const ByteTranslation viaG0 = translate(graphicLeftSweep, specialGraphicsInG0);
    CHECK_MSG(!viaG0.empty(), "G0 designation produced a mapping");

    const DesignationRoute shiftedSlots[] = {
        { "\033)0\016", "G1 through SO agrees with G0" },
        { "\033*0\033n", "G2 through LS2 agrees with G0" },
        { "\033+0\033o", "G3 through LS3 agrees with G0" },
    };
    for (const DesignationRoute& route : shiftedSlots)
        CHECK_MSG(translate(graphicLeftSweep, route.sequence) == viaG0, route.label);
}

// The agreement above would hold for four routes that each did nothing, so
// DEC Special Graphics has to be shown to remap a real share of GL.
TEST(testSpecialGraphicsDiffersFromASCII)
{
    const ByteTranslation specialGraphics = translate(graphicLeftSweep, specialGraphicsInG0);
    const ByteTranslation ascii = translate(graphicLeftSweep, "");
    CHECK_MSG(countDisagreements(specialGraphics, ascii) > 20, "DEC Special Graphics remaps a substantial range");
}

} // namespace

UBIQUITTY_TEST_MAIN()
