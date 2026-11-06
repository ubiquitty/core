// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_beam_layout — the palette's picture.
//
// `layoutBeam` is pure, which is the whole basis for saying Beam looks the same
// on every frontend: the claim is about this function's output, not about four
// backends agreeing to try. So the cases assert the cells, and the negatives are
// the grid sizes and states a user reaches by resizing rather than by intent.
//
// The one that matters most is degradation. A palette asked to lay out on a grid
// smaller than itself must shrink or draw nothing, and must never produce a
// negative extent — a frontend compositing one writes outside its own buffer,
// which no assertion inside the canvas can see.

#include "Harness.h"

#include "Core/CommandPalette/BeamState.h"
#include "Core/Rendering/BeamLayout.h"
#include "Core/TermPalette.h"
#include "Core/Themes/ChromeStyle.h"

#include <string>

using Ubiquitty::CommandPalette::BeamRow;
using Ubiquitty::CommandPalette::BeamState;
using Ubiquitty::Rendering::BeamGeometry;
using Ubiquitty::Rendering::ChromeStyle;
using Ubiquitty::Rendering::layoutBeam;

namespace {

ChromeStyle darkStyle()
{
    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    return Ubiquitty::Themes::resolveChromeStyle(palette);
}

BeamState openStateWith(int rowCount)
{
    BeamState state;
    state.isOpen = true;
    state.query = "spl";
    for (int i = 0; i < rowCount; ++i) {
        BeamRow row;
        row.title = "Split Right " + std::to_string(i);
        row.detail = "Window";
        row.matchedIndices = { 0, 1, 2 };
        state.rows.push_back(row);
    }
    state.selectedIndex = rowCount > 0 ? 0 : -1;
    state.status = std::to_string(rowCount) + " results";
    return state;
}

bool anyGlyphOn(const BeamGeometry& geometry, int row)
{
    for (int column = 0; column < geometry.canvas.columns; ++column) {
        const Ubiquitty::Cell* cell = geometry.canvas.at(row, column);
        if (cell && cell->codePoint != 0)
            return true;
    }
    return false;
}

} // namespace

TEST(beamLayoutDrawsNothingWhenItShould)
{
    const ChromeStyle style = darkStyle();

    // Closed is the common case, and it must cost nothing — a caller composites
    // unconditionally rather than remembering to check.
    BeamState closed = openStateWith(3);
    closed.isOpen = false;
    CHECK_EQ(layoutBeam(closed, style, 24, 80).canvas.rows, 0);

    // A grid too narrow for a palette draws nothing rather than a stub. A
    // 20-column box would be a worse answer than none.
    CHECK_EQ(layoutBeam(openStateWith(3), style, 24, 20).canvas.rows, 0);

    // Too short, likewise.
    CHECK_EQ(layoutBeam(openStateWith(3), style, 4, 80).canvas.rows, 0);

    // Degenerate sizes never produce a negative extent — the failure a caller
    // could not detect from inside the canvas.
    for (const int dimension : { 0, -1 }) {
        const BeamGeometry geometry = layoutBeam(openStateWith(3), style, dimension, dimension);
        CHECK_MSG(geometry.canvas.rows >= 0 && geometry.canvas.columns >= 0, "no negative extent");
        CHECK_MSG(geometry.originRow >= 0 && geometry.originColumn >= 0, "and no negative origin");
    }
}

TEST(beamLayoutFitsTheGridItIsGiven)
{
    const ChromeStyle style = darkStyle();

    const BeamGeometry roomy = layoutBeam(openStateWith(4), style, 30, 100);
    CHECK_MSG(roomy.canvas.rows > 0, "a roomy grid draws a palette");
    CHECK_MSG(roomy.canvas.columns <= 100, "never wider than the grid");
    CHECK_MSG(roomy.originColumn + roomy.canvas.columns <= 100, "and it fits where it is placed");
    CHECK_MSG(roomy.originRow + roomy.canvas.rows <= 30, "on both axes");

    // The list does not reserve space it will not fill: one row is a shorter box
    // than four, rather than four rows of nothing.
    const BeamGeometry few = layoutBeam(openStateWith(1), style, 30, 100);
    CHECK_MSG(few.canvas.rows < roomy.canvas.rows, "fewer results draw a shorter palette");

    // A grid barely tall enough shrinks the list rather than overflowing. The
    // discriminating case: it still fits, and it still draws something.
    const BeamGeometry tight = layoutBeam(openStateWith(40), style, 8, 100);
    CHECK_MSG(tight.canvas.rows > 0, "a short grid still gets a palette");
    CHECK_MSG(tight.originRow + tight.canvas.rows <= 8, "clamped to the grid it was given");
    CHECK_MSG(tight.canvas.rows < 40, "with the list shrunk rather than the box overflowing");
}

TEST(beamLayoutDrawsTheBorderQueryAndSelection)
{
    const ChromeStyle style = darkStyle();
    const BeamGeometry geometry = layoutBeam(openStateWith(3), style, 30, 100);
    const int lastColumn = geometry.canvas.columns - 1;
    const int lastRow = geometry.canvas.rows - 1;

    // Rounded corners, and the rule under the query joined to the border on both
    // sides — the seam a border routine usually leaves open.
    CHECK_EQ(static_cast<int>(geometry.canvas.at(0, 0)->codePoint), 0x256D);
    CHECK_EQ(static_cast<int>(geometry.canvas.at(0, lastColumn)->codePoint), 0x256E);
    CHECK_EQ(static_cast<int>(geometry.canvas.at(lastRow, 0)->codePoint), 0x2570);
    CHECK_EQ(static_cast<int>(geometry.canvas.at(2, 0)->codePoint), 0x251C);
    CHECK_EQ(static_cast<int>(geometry.canvas.at(2, lastColumn)->codePoint), 0x2524);

    // The query is drawn where it was typed.
    CHECK_EQ(static_cast<int>(geometry.canvas.at(1, 2)->codePoint), static_cast<int>('s'));
    CHECK_EQ(static_cast<int>(geometry.canvas.at(1, 4)->codePoint), static_cast<int>('l'));

    // The selected row carries the selection background and the others do not —
    // asserted as a DIFFERENCE rather than against a literal colour, so it holds
    // on any theme.
    const Ubiquitty::Cell* selected = geometry.canvas.at(3, 5);
    const Ubiquitty::Cell* unselected = geometry.canvas.at(4, 5);
    CHECK(selected && unselected);
    CHECK_MSG(selected->pen.backgroundRed != unselected->pen.backgroundRed
            || selected->pen.backgroundGreen != unselected->pen.backgroundGreen
            || selected->pen.backgroundBlue != unselected->pen.backgroundBlue,
        "the selected row reads differently from its neighbours");

    // Matched characters take the accent, and unmatched ones do not.
    const Ubiquitty::Cell* matched = geometry.canvas.at(4, 2);
    const Ubiquitty::Cell* plain = geometry.canvas.at(4, 8);
    CHECK(matched && plain);
    CHECK_MSG(matched->pen.foregroundRed != plain->pen.foregroundRed
            || matched->pen.foregroundGreen != plain->pen.foregroundGreen
            || matched->pen.foregroundBlue != plain->pen.foregroundBlue,
        "a matched character is coloured differently from an unmatched one");

    CHECK_MSG(anyGlyphOn(geometry, geometry.canvas.rows - 2), "the status line is drawn");
}

// Match indices arrive over the wire in the host-scope case, so they are
// attacker-controlled. An index past the title must be ignored rather than
// written — the check is that laying out with absurd indices produces the same
// picture as laying out with none.
TEST(beamLayoutIgnoresOutOfRangeMatchIndices)
{
    const ChromeStyle style = darkStyle();

    BeamState hostile = openStateWith(2);
    for (BeamRow& row : hostile.rows)
        row.matchedIndices = { -5, 9999, 100000 };
    const BeamGeometry drawn = layoutBeam(hostile, style, 30, 100);

    BeamState none = openStateWith(2);
    for (BeamRow& row : none.rows)
        row.matchedIndices.clear();
    const BeamGeometry reference = layoutBeam(none, style, 30, 100);

    CHECK_EQ(drawn.canvas.rows, reference.canvas.rows);
    CHECK_EQ(drawn.canvas.columns, reference.canvas.columns);
    bool identical = true;
    for (std::size_t i = 0; i < drawn.canvas.cells.size() && i < reference.canvas.cells.size(); ++i)
        if (drawn.canvas.cells[i].codePoint != reference.canvas.cells[i].codePoint)
            identical = false;
    CHECK_MSG(identical, "out-of-range indices changed nothing that was drawn");
}

// A drill draws what is being changed and which value is current. Both are
// things the user cannot recover from the list itself: four theme names with no
// subject, and no way to see which one is already in effect.
TEST(beamLayoutDrawsTheDrillBreadcrumbAndCheck)
{
    const ChromeStyle style = darkStyle();

    BeamState drill = openStateWith(3);
    drill.drillTitle = "Choose Theme";
    drill.query = "no";
    drill.rows[1].isChecked = true;
    const BeamGeometry drawn = layoutBeam(drill, style, 30, 100);
    CHECK_MSG(drawn.canvas.rows > 0, "the drill draws");

    // The breadcrumb leads the query line.
    CHECK_EQ(static_cast<int>(drawn.canvas.at(1, 2)->codePoint), static_cast<int>('C'));

    // U+2713 marks the current value and only it.
    CHECK_EQ(static_cast<int>(drawn.canvas.at(4, 2)->codePoint), 0x2713);
    CHECK_MSG(drawn.canvas.at(3, 2)->codePoint != 0x2713, "and the others carry no tick");

    // Every row is indented by the check column whether ticked or not, so the
    // titles do not jitter as the selection moves. Asserted as EQUALITY between
    // a ticked row and an unticked one, which is the property that matters.
    const int tickedTitle = 4;
    CHECK_MSG(drawn.canvas.at(3, tickedTitle)->codePoint == static_cast<std::uint32_t>('S')
            && drawn.canvas.at(4, tickedTitle)->codePoint == static_cast<std::uint32_t>('S'),
        "ticked and unticked rows start their titles in the same column");

    // Outside a drill nothing is indented and no tick is drawn, so the ordinary
    // palette is unchanged by any of this.
    const BeamGeometry plain = layoutBeam(openStateWith(3), style, 30, 100);
    CHECK_EQ(static_cast<int>(plain.canvas.at(3, 2)->codePoint), static_cast<int>('S'));
}

// A popover leaves the terminal visible. The theme drill reaches this the moment
// it opens — Core ships more themes than a short grid has rows — so a list that
// took every row it could would cover the screen it is drawn over.
TEST(beamLayoutLeavesTheTerminalVisibleBehindALongList)
{
    const ChromeStyle style = darkStyle();

    const BeamGeometry many = layoutBeam(openStateWith(200), style, 40, 100);
    CHECK_MSG(many.canvas.rows > 0, "a long list still draws");
    CHECK_MSG(many.canvas.rows <= 40 / 2 + 6, "and takes about half the grid, not all of it");
    CHECK_MSG(many.originRow + many.canvas.rows < 40, "leaving rows below it");

    // The cap yields on a short grid, where half of very little is worse than
    // what fits: the palette must still draw rather than shrink to nothing.
    const BeamGeometry tight = layoutBeam(openStateWith(200), style, 8, 100);
    CHECK_MSG(tight.canvas.rows > 0, "a short grid still gets a palette");
    CHECK_MSG(tight.originRow + tight.canvas.rows <= 8, "that fits");

    // A short list is unaffected — the cap is a ceiling, not a size.
    const BeamGeometry few = layoutBeam(openStateWith(2), style, 40, 100);
    CHECK_MSG(few.canvas.rows < many.canvas.rows, "two results draw a shorter box than two hundred");
}

// The layout draws as many rows as fit; the session scrolls against a window it
// is told about separately. Nothing connects them by construction, and when they
// disagree the selection walks off the bottom of a box that never scrolls —
// arrow-down appears to do nothing and the palette reads as frozen.
//
// This pins the CONTRACT rather than any frontend: whatever the layout drew is
// what a caller must hand the session. A frontend that forgets has no way to
// find out from inside its own frame.
TEST(beamLayoutReportsHowManyResultRowsItDrew)
{
    const ChromeStyle style = darkStyle();

    // A short grid where the height cap bites: far more results than rows.
    const BeamGeometry tight = layoutBeam(openStateWith(50), style, 14, 100);
    CHECK_MSG(tight.canvas.rows > 0, "it drew");
    CHECK_MSG(tight.bodyRows > 0, "and says how many result rows");
    CHECK_MSG(tight.bodyRows < 50, "fewer than the results, because the grid is short");

    // Body plus chrome is the whole box — asserted as an identity, so a change
    // to either cannot drift from the other.
    const int chromeRows = tight.canvas.rows - tight.bodyRows;
    CHECK_MSG(chromeRows >= 4 && chromeRows <= 5, "chrome is the border, query, rule and any status");

    // A roomy grid draws every result and says so.
    CHECK_EQ(layoutBeam(openStateWith(3), style, 40, 100).bodyRows, 3);

    // Closed reports nothing, so a caller applying it unconditionally does not
    // set a window of zero on a session that is about to open.
    BeamState closed = openStateWith(3);
    closed.isOpen = false;
    CHECK_EQ(layoutBeam(closed, style, 40, 100).bodyRows, 0);
}

UBIQUITTY_TEST_MAIN()
