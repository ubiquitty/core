// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_vt_parameter_bounds.cpp — every cursor and region control, driven with
// parameters on and past the edges of the grid.
//
// Most of the guards in VT.cpp are one-line clamps and early returns, and
// nearly every one of them answers the same question: did a parameter arrive
// outside the grid? A case per guard would be a long list of single
// assertions that nobody reads. These cases instead drive each control that
// takes a position across a fixed set of boundary values, and assert what the
// guards exist to keep: the cursor stays on the grid, and the grid stays
// addressable.
//
// Checked as a property, one run reaches many clamps at once, and goes on
// reaching them as controls are added, which a case per guard cannot.

#include "Harness.h"

#include "Core/CellGrid.h"
#include "Core/Screen/Addressing.h"
#include "Core/TerminalOperations.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

namespace {

constexpr int gridRows = 10;
constexpr int gridColumns = 20;

// Each value sits on, or just across, a boundary some clamp guards: the
// omitted default, zero, the first cell and the one after it, 9 10 11 around
// the row count, 19 20 21 around the column count, then magnitudes that
// overflow careless int arithmetic. The shared-terminal case feeds them in
// this order.
constexpr std::string_view boundaryParameters[] = { "", "0", "1", "2", "9", "10", "11", "19", "20", "21", "0", "255", "999", "65535", "2147483647" };

// The final bytes of the controls that take one positional parameter: CUU CUD
// CUF CUB, CNL CPL CHA, CHT CBT, IL DL, DCH ECH, SU SD, ICH, VPA, HPR VPR.
constexpr std::string_view singleParameterFinals = "ABCDEFGIZLMPXST@dae";

/// Spells a CSI control: ESC [, the parameters joined by `;`, then `finalByte`.
///
/// An empty parameter stays in its position, so `{ "", "5" }` spells `ESC [ ; 5`.
std::string controlSequence(std::initializer_list<std::string_view> parameters, char finalByte)
{
    std::string sequence = "\033[";
    std::string_view separator;
    for (const std::string_view parameter : parameters) {
        sequence.append(separator);
        sequence.append(parameter);
        separator = ";";
    }
    sequence.push_back(finalByte);
    return sequence;
}

void feed(Ubiquitty::Terminal& terminal, std::string_view bytes) { Ubiquitty::processBytes(&terminal, bytes); }

/// Checks what every bounds guard here exists to keep: the active cursor sits
/// on a cell of the grid.
void checkCursorInsideGrid(const Ubiquitty::Terminal& terminal, const char* context)
{
    const Ubiquitty::Screen::State& screen = *terminal.screens.active;
    CHECK_MSG(screen.cursorX >= 0, context);
    CHECK_MSG(screen.cursorX < terminal.columns, context);
    CHECK_MSG(screen.cursorY >= 0, context);
    CHECK_MSG(screen.cursorY < terminal.rows, context);
}

/// Returns a terminal of the size the boundary values are chosen around.
std::unique_ptr<Ubiquitty::Terminal> makeTerminal()
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, gridRows, gridColumns);
    return terminal;
}

TEST(parametersPastTheLastSlotAreDroppedRatherThanMergedIntoIt)
{
    // The separator before one parameter past the last slot is refused, which
    // leaves its digits with nowhere of their own to go — and they went into
    // the last slot, whose value the control does read. So a long SGR run
    // rewrote one of its own attributes with the digits of an attribute this
    // terminal never keeps.
    auto t = makeTerminal();
    std::string manyParameters = "\033[";
    for (int i = 1; i <= Ubiquitty::VT::maxParameters; ++i)
        manyParameters += std::to_string(i) + ";";
    manyParameters += "99;98m"; // two parameters past the last slot
    feed(*t, manyParameters);

    CHECK_EQ(t->parser.parameterCount, Ubiquitty::VT::maxParameters);
    // The last slot still holds the last parameter that fit, not that value
    // with the overflow's digits appended.
    CHECK_EQ(static_cast<int>(t->parser.parameters[Ubiquitty::VT::maxParameters - 1]), Ubiquitty::VT::maxParameters);
    // And every earlier slot is untouched.
    for (int i = 0; i < Ubiquitty::VT::maxParameters; ++i)
        CHECK_EQ(static_cast<int>(t->parser.parameters[i]), i + 1);
}

// The same through SGR: the thirtieth parameter is italic, and the thirty-first,
// bold, is past the limit and ignored rather than joined to it as 31, red.
TEST(anAttributePastTheLastSlotIsIgnored)
{
    auto t = makeTerminal();
    std::string sequence = "\033[";
    for (int i = 1; i < Ubiquitty::VT::maxParameters; ++i)
        sequence += "0;";
    sequence += "3;1mA";
    feed(*t, sequence);
    const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(t.get(), 0, 0, 0);
    CHECK(cell != nullptr && cell->codePoint == 'A');
    CHECK(cell->pen.attributes.contains(Ubiquitty::VT::Attribute::Italic));
    CHECK(!cell->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell->pen.foregroundIndex != 1);
}

TEST(everyParameterSlotRecordsWhetherItCarriedDigits)
{
    // parameterWasEmpty is one bit per slot, and the batched scanner carries a
    // shifting bit that walks the whole cap. A mask narrower than the cap does
    // not truncate — it shifts to zero, so every slot past the mask's width
    // reads as digit-less and the control takes its default in place of the
    // value it was sent. The bit is read through `parameterOmitted`, by
    // controls that each read their own slots; the invariant is stated for
    // every slot, so it is asserted for every slot.
    auto t = makeTerminal();
    std::string alternating = "\033[";
    for (int i = 0; i < Ubiquitty::VT::maxParameters; ++i) {
        if (i > 0)
            alternating += ";";
        if (i % 2 == 0) // odd slots carry no digits
            alternating += std::to_string(i + 1);
    }
    alternating += "m";
    feed(*t, alternating);

    CHECK_EQ(t->parser.parameterCount, Ubiquitty::VT::maxParameters);
    for (int i = 0; i < Ubiquitty::VT::maxParameters; ++i) {
        const bool isSlotEmpty = (t->parser.parameterWasEmpty & (1u << i)) != 0;
        CHECK(isSlotEmpty == (i % 2 != 0));
    }
}

TEST(testSingleParameterControls)
{
    for (const char finalByte : singleParameterFinals) {
        for (const std::string_view parameter : boundaryParameters) {
            auto terminal = makeTerminal();
            // From the middle of the grid a control can overshoot either way.
            feed(*terminal, "\033[5;10H");
            feed(*terminal, controlSequence({ parameter }, finalByte));
            checkCursorInsideGrid(*terminal, "single-parameter control keeps the cursor in the grid");
        }
    }
}

// CUP and HVP clamp the row and the column independently, so every pairing of
// boundary values gets its own run.
TEST(testCursorPositionPairs)
{
    for (const std::string_view row : boundaryParameters) {
        for (const std::string_view column : boundaryParameters) {
            for (const char finalByte : { 'H', 'f' }) {
                auto terminal = makeTerminal();
                feed(*terminal, controlSequence({ row, column }, finalByte));
                checkCursorInsideGrid(*terminal, "CUP/HVP clamps both parameters");
            }
        }
    }
}

// DECSTBM and DECSLRM set the margins the cursor is then confined to. A
// region that is inverted, or that lies off the grid, corrupts the bounds the
// clamps themselves use, which makes it the usual way out of the grid.
TEST(testScrollRegionsThenMovement)
{
    // Every direction out of the region, as far as a parameter reaches, and home.
    constexpr std::string_view escapes[] = {
        "\033[999A", "\033[999B", "\033[999C", "\033[999D", "\033[999;999H", "\033[H"
    };
    for (const std::string_view top : boundaryParameters) {
        for (const std::string_view bottom : boundaryParameters) {
            auto terminal = makeTerminal();
            feed(*terminal, controlSequence({ top, bottom }, 'r')); // DECSTBM
            feed(*terminal, "\033[?69h"); // DECLRMM, which DECSLRM needs
            feed(*terminal, controlSequence({ top, bottom }, 's')); // DECSLRM
            for (const std::string_view escape : escapes) {
                feed(*terminal, escape);
                checkCursorInsideGrid(*terminal, "movement inside a hostile region stays in the grid");
            }
        }
    }
}

// Insertion, deletion and scrolling derive their bounds from the region rather
// than from the grid, which puts them behind clamps of their own.
TEST(testRegionsThenEditing)
{
    // IL, DL, SU, SD, DCH, ECH and ICH, each with a count past any grid.
    constexpr std::string_view edits[] = {
        "\033[999L", "\033[999M", "\033[999S", "\033[999T", "\033[999P", "\033[999X", "\033[999@"
    };
    for (const std::string_view top : boundaryParameters) {
        for (const std::string_view bottom : boundaryParameters) {
            auto terminal = makeTerminal();
            feed(*terminal, controlSequence({ top, bottom }, 'r'));
            feed(*terminal, "\033[5;10H");
            for (const std::string_view edit : edits) {
                feed(*terminal, edit);
                checkCursorInsideGrid(*terminal, "editing inside a hostile region stays in the grid");
            }
        }
    }
}

// An index that slipped past a clamp shows up as a missing cell even when the
// cursor came back inside, so the whole grid is read after one terminal has
// taken every control at every boundary value.
TEST(testGridRemainsAddressableAfterAbuse)
{
    auto terminal = makeTerminal();
    // Collapsed margins on both axes before the sweep.
    feed(*terminal, "\033[0;0r\033[?69h\033[0;0s");
    for (const std::string_view parameter : boundaryParameters) {
        for (const char finalByte : singleParameterFinals)
            feed(*terminal, controlSequence({ parameter }, finalByte));
    }

    for (int row = 0; row < gridRows; ++row) {
        for (int column = 0; column < gridColumns; ++column)
            CHECK_MSG(terminal->grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column)) != nullptr,
                "every cell is addressable after parameter abuse");
    }
}

} // namespace

UBIQUITTY_TEST_MAIN()
