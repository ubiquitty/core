// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The in-terminal form of a hint, for the arms with no native chrome.
//
// The box is sized in COLUMNS, not bytes: every hint carries modifier glyphs
// (⌘ ⌥ ⇧ ⌃) that are multi-byte and single-column, so a byte count would draw a
// box several cells too wide on every one of them and leave a ragged edge.

#include "Harness.h"

#include "Core/Input/TransientHint.h"
#include "Core/Rendering/HintToastLayout.h"

#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

namespace {

ChromeStyle testStyle()
{
    ChromeStyle style;
    style.surface = 0x202020;
    style.border = 0x808080;
    style.text = 0xE0E0E0;
    style.dim = 0x909090;
    return style;
}

}

TEST(aHintIsDrawnAtTheBottomRightWhereAShellIsNotWriting)
{
    const std::string_view text = Input::hintText(Input::Hint::SelectWithShift, Input::ModifierVocabulary::Mac);
    const HintToastGeometry geometry = layoutHintToast(text, testStyle(), 24, 80);
    CHECK_MSG(geometry.canvas.rows == 3, "a border row above and below the text");
    CHECK_MSG(geometry.originRow + geometry.canvas.rows <= 24, "the box lands inside the grid");
    CHECK_MSG(geometry.originColumn + geometry.canvas.columns <= 80, "and inside its columns");
    CHECK_MSG(geometry.originRow > 12 && geometry.originColumn > 40,
        "bottom right: a shell's cursor sits bottom-left and its output runs down the left");
}

TEST(theBoxIsSizedInColumnsNotBytes)
{
    // A glyph is three bytes and one column, so a box sized by bytes is two
    // cells too wide. The lessons themselves are words now; the case keeps a
    // glyph of its own so it still measures columns against bytes.
    const std::string_view text = "⌘-drag to select text";
    CHECK_MSG(text.size() > 21, "the text really is multi-byte, so the case is not vacuous");
    const HintToastGeometry geometry = layoutHintToast(text, testStyle(), 24, 80);
    CHECK_MSG(geometry.canvas.columns == 21 + 4,
        "the text's columns plus a border and a space each side");
}

TEST(aGridTooSmallForTheWholeHintDrawsNoneOfIt)
{
    const std::string_view text = Input::hintText(Input::Hint::PointerLockRelease, Input::ModifierVocabulary::Mac);
    CHECK_MSG(layoutHintToast(text, testStyle(), 24, 10).canvas.rows == 0,
        "half a sentence teaching a gesture teaches nothing");
    CHECK_MSG(layoutHintToast(text, testStyle(), 2, 80).canvas.rows == 0, "nor does a grid too short for the box");
    CHECK_MSG(layoutHintToast("", testStyle(), 24, 80).canvas.rows == 0, "and no hint draws nothing");
    CHECK_MSG(layoutHintToast(text, testStyle(), 0, 0).canvas.rows == 0, "a grid with no cells is not a crash");
}

TEST(everyHintFitsAnOrdinaryGrid)
{
    // A hint too wide for 80 columns would silently never appear.
    const Input::Hint every[] = { Input::Hint::PointerLockRelease, Input::Hint::SelectWithShift };
    for (const Input::Hint hint : every) {
        const HintToastGeometry geometry = layoutHintToast(Input::hintText(hint, Input::ModifierVocabulary::Mac), testStyle(), 24, 80);
        CHECK_MSG(geometry.canvas.rows == 3, "every hint draws on an 80x24 grid");
    }
}

namespace {

// The canvas as text, one string per row, so a failure prints the picture that
// was drawn rather than a coordinate. Code points only: the pen is asserted
// separately, and a colour in this string would make every row unreadable.
std::vector<std::string> picture(const CellCanvas& canvas)
{
    std::vector<std::string> rows;
    for (int row = 0; row < canvas.rows; ++row) {
        std::string line;
        for (int column = 0; column < canvas.columns; ++column) {
            const Cell* cell = canvas.at(row, column);
            const std::uint32_t codePoint = cell != nullptr ? cell->codePoint : 0;
            // UTF-8 encode; the box glyphs and the modifier glyphs are all
            // three-byte, and a blank cell reads as a space.
            const std::uint32_t point = codePoint == 0 ? U' ' : codePoint;
            if (point < 0x80) {
                line.push_back(static_cast<char>(point));
            } else if (point < 0x800) {
                line.push_back(static_cast<char>(0xC0 | (point >> 6)));
                line.push_back(static_cast<char>(0x80 | (point & 0x3F)));
            } else {
                line.push_back(static_cast<char>(0xE0 | (point >> 12)));
                line.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
                line.push_back(static_cast<char>(0x80 | (point & 0x3F)));
            }
        }
        rows.push_back(line);
    }
    return rows;
}

}

// The drawn picture, asserted whole. Chrome has no golden here — every snapshot
// case is driven by a feed of escape sequences, and a composited overlay is not
// something a program can emit — so this is the picture that would otherwise be
// a golden, in a form that prints itself when it breaks.
TEST(theHintDrawsItsWholePicture)
{
    const HintToastGeometry geometry = layoutHintToast(Input::hintText(Input::Hint::SelectWithShift, Input::ModifierVocabulary::Mac),
        testStyle(), 24, 80);
    const std::vector<std::string> drawn = picture(geometry.canvas);
    const std::vector<std::string> expected = {
        "╭───────────────────────────╮",
        "│ Shift-drag to select text │",
        "╰───────────────────────────╯",
    };
    CHECK_EQ(drawn.size(), expected.size());
    for (std::size_t row = 0; row < expected.size() && row < drawn.size(); ++row)
        CHECK_MSG(drawn[row] == expected[row], (std::string("row ") + std::to_string(row) + ": drew \"" + drawn[row] + "\" wanted \"" + expected[row] + "\"").c_str());
}

TEST(theTextSitsInsideOneCellOfPaddingWithinTheBorder)
{
    const HintToastGeometry geometry = layoutHintToast("hi", testStyle(), 24, 80);
    const std::vector<std::string> drawn = picture(geometry.canvas);
    CHECK_EQ(geometry.canvas.columns, 2 + 4);
    CHECK_MSG(drawn.size() == 3 && drawn[1] == "│ hi │", "a border, a space, the text, a space, a border");
    CHECK_MSG(drawn[0] == "╭────╮" && drawn[2] == "╰────╯", "rounded corners top and bottom");
}

TEST(theBoxTakesAMarginWhereTheGridAffordsOneAndDropsItWhereItDoesNot)
{
    const std::string_view text = Input::hintText(Input::Hint::PointerLockRelease, Input::ModifierVocabulary::Mac);
    const HintToastGeometry roomy = layoutHintToast(text, testStyle(), 24, 80);
    CHECK_MSG(roomy.originRow == 24 - 3 - 1, "a row of margin above the bottom edge");
    CHECK_MSG(roomy.originColumn == 80 - roomy.canvas.columns - 1, "and a column in from the right");

    // A grid exactly the size of the box: the margin would push it off, so it goes.
    const HintToastGeometry exact = layoutHintToast(text, testStyle(), 3, roomy.canvas.columns);
    CHECK_MSG(exact.canvas.rows == 3, "an exact fit still draws");
    CHECK_MSG(exact.originRow == 0 && exact.originColumn == 0, "flush, because there is nowhere to inset to");
}

TEST(textThatIsNotValidUTF8IsSizedAsTheReplacementItDraws)
{
    // drawText draws U+FFFD for an invalid byte, which is one column. A layout
    // that measured differently would size the box wrong around it.
    const char invalid[] = { 'a', static_cast<char>(0xFF), 'b', '\0' };
    const HintToastGeometry geometry = layoutHintToast(invalid, testStyle(), 24, 80);
    CHECK_MSG(geometry.canvas.columns == 3 + 4, "three columns of text plus the chrome");
    CHECK_MSG(geometry.canvas.rows == 3, "and it still draws rather than refusing");
}

UBIQUITTY_TEST_MAIN()
