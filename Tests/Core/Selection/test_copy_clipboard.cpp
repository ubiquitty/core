// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_copy_clipboard.cpp — the copy decision as one Core fold
// (Core/Selection/CopyClipboard.cpp): which flavour a menu item carries, which
// colours the styled walk bakes, and that a frontend gets every flavour from one
// call. The macOS window controller held all three itself, so the Swift shell
// had none of them and the two spellings of one setting disagreed.

#include "Core/CellColor.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Selection/CopyClipboard.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "TerminalFixture.h"

#include <string>

using namespace Ubiquitty;
using UbiquittyTest::feed;

namespace {

Preferences::Values valuesNaming(const char* copyStyle)
{
    Preferences::Values values;
    values.copyStyle = std::string(copyStyle);
    return values;
}

} // namespace

// The file's name is consumed as an enum, and a Copy Special item's own flavour
// is not re-decided by the file: the two arms spelling that apart is how an
// item written to override a preference came to obey it.
TEST(theCopyStyleAnswersTheFileAndAnItemThatNamesItsOwn)
{
    using Selection::CopyStyle;
    Preferences::Values unset;
    CHECK_MSG(Selection::copyStyle(unset) == CopyStyle::Everything, "an unset copy_style carries everything");
    CHECK_MSG(Selection::copyStyle(valuesNaming("")) == CopyStyle::Everything, "an empty name carries everything");
    CHECK_MSG(Selection::copyStyle(valuesNaming("mauve")) == CopyStyle::Everything, "an unrecognised name carries everything");
    CHECK(Selection::copyStyle(valuesNaming("plain")) == CopyStyle::PlainText);
    // Both spellings: the file template documents the hyphenated names and
    // the window controller compared the underscored ones.
    CHECK(Selection::copyStyle(valuesNaming("no_background")) == CopyStyle::NoBackground);
    CHECK(Selection::copyStyle(valuesNaming("no-background")) == CopyStyle::NoBackground);
    CHECK(Selection::copyStyle(valuesNaming("styles_only")) == CopyStyle::StylesOnly);
    CHECK(Selection::copyStyle(valuesNaming("styles-only")) == CopyStyle::StylesOnly);

    const Preferences::Values plain = valuesNaming("plain");
    CHECK_MSG(Selection::copyStyle(plain, CopyStyle::Configured) == CopyStyle::PlainText, "the ⌘C item takes the file's answer");
    CHECK_MSG(Selection::copyStyle(plain, CopyStyle::NoBackground) == CopyStyle::NoBackground, "a Copy Special item keeps its own flavour");
    CHECK_MSG(Selection::copyStyle(plain, CopyStyle::StylesOnly) == CopyStyle::StylesOnly, "whatever the file says");
    CHECK_MSG(Selection::copyStyle(plain, CopyStyle::Everything) == CopyStyle::Everything, "including the everything flavour under a plain file");

    CHECK(Selection::styleColorMode(CopyStyle::NoBackground) == Selection::StyleColorMode::NoBackground);
    CHECK(Selection::styleColorMode(CopyStyle::StylesOnly) == Selection::StyleColorMode::StylesOnly);
    CHECK(Selection::styleColorMode(CopyStyle::Everything) == Selection::StyleColorMode::Full);
}

// The styled walk bakes the run's colours the way the screen drew them, and the
// colour mode is what drops them; the plain flavour is the plain copy byte for
// byte.
TEST(aStyledCopyCarriesTheRunsColoursAndTheModeDropsThem)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    feed(t, "\033[1;31mBOLDRED\033[0m");

    Selection::Session session;
    Selection::selectAllText(session, t);
    CellPalette palette = CellPalette::fromTerminalPalette(t.palette);
    const Selection::StyledFont font { "Menlo", 13.0 };

    const Selection::StyledClipboard full = Selection::styledClipboard(session, t, Selection::StyleColorMode::Full, palette, font);
    CHECK_MSG(full.plain == Selection::selectedText(session, t), "the plain flavour is the plain copy");
    CHECK_MSG(full.rtf.find("\\colortbl;") != std::string::npos, "the RTF carries a colour table");
    CHECK_MSG(full.rtf.find("\\b") != std::string::npos, "and the bold run");
    CHECK_MSG(full.html.find("font-weight:bold") != std::string::npos, "the HTML carries the weight");
    CHECK_MSG(full.html.find("color:#") != std::string::npos, "and the colour");

    const Selection::StyledClipboard stylesOnly = Selection::styledClipboard(session, t, Selection::StyleColorMode::StylesOnly, palette, font);
    CHECK_MSG(stylesOnly.html.find("font-weight:bold") != std::string::npos, "styles-only keeps the weight");
    CHECK_MSG(stylesOnly.html.find("color:#") == std::string::npos, "and drops every colour");

    // The control: a cell the walk cannot find answers a blank cell, so a
    // resolver that answered blanks for every cell would carry no colour.
    Selection::Session empty;
    const Selection::StyledClipboard nothing = Selection::styledClipboard(empty, t, Selection::StyleColorMode::Full, palette, font);
    CHECK_MSG(nothing.plain.empty() && nothing.rtf.empty() && nothing.html.empty(), "no selection answers three empty flavours");
}

// A view owns Selection::Session by value and may ask whether it has anything
// to copy before its first gesture. Default construction is therefore the idle
// state, including under a sanitizer that diagnoses an indeterminate bool read.
TEST(aDefaultConstructedSessionHasNothingToCopy)
{
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 4, 20);
    feed(terminal, "text");

    Selection::Session session;
    CHECK(!Selection::hasSelection(session));
    CHECK(Selection::selectedText(session, terminal).empty());
}

// The whole decision from one call: a frontend hands over the picked item and
// the face, and the flavour, the palette and the walk are decided here.
TEST(theCopyDecisionIsOneFold)
{
    Terminal t;
    hardReset(t);
    resize(t, 4, 20);
    feed(t, "\033[1;31mBOLDRED\033[0m");
    Selection::Session session;
    Selection::selectAllText(session, t);
    const Selection::StyledFont font { "Menlo", 13.0 };

    const Selection::StyledClipboard plain = Selection::copyClipboard(session, t, valuesNaming("plain"), Selection::CopyStyle::Configured, font);
    CHECK_MSG(plain.plain.find("BOLDRED") != std::string::npos && plain.rtf.empty() && plain.html.empty(), "a plain file gives ⌘C the plain flavour alone");

    const Selection::StyledClipboard everything = Selection::copyClipboard(session, t, Preferences::Values {}, Selection::CopyStyle::Configured, font);
    CHECK_MSG(everything.plain.find("BOLDRED") != std::string::npos && !everything.rtf.empty() && !everything.html.empty(), "an unset file gives ⌘C every flavour");
    CHECK_MSG(everything.plain == plain.plain, "and the same plain text as the plain flavour");

    const Selection::StyledClipboard special = Selection::copyClipboard(session, t, valuesNaming("plain"), Selection::CopyStyle::StylesOnly, font);
    CHECK_MSG(!special.html.empty() && special.html.find("color:#") == std::string::npos, "a Copy Special item overrides a plain file");
}

TEST(rightToLeftCopyPreservesClustersAndSearchOrder)
{
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 2, 40);
    const std::string input = "Ae\xCC\x81👩‍💻🇬🇧Z";
    feed(terminal, input + "\033[?96h");
    Selection::Session session;
    Selection::beginGesture(session, terminal, 0, 0, 1, 0, {});
    Selection::dragGesture(session, terminal, 0, 6, 0);
    CHECK(Selection::selectedText(session, terminal) == input);
    const std::string expected = "Z🇬🇧👩‍💻e\xCC\x81"
                                 "A";
    CHECK(Selection::copiedText(session, terminal) == expected);
    const auto plain = Selection::copyClipboard(session, terminal, {}, Selection::CopyStyle::PlainText, { nullptr, 0 });
    CHECK(plain.plain == expected);
    CHECK(plain.html.empty());
    CHECK(plain.rtf.empty());
    feed(terminal, "\033[?96l");
    CHECK(Selection::copiedText(session, terminal) == input);
}

TEST(rightToLeftCopyKeepsStylesWithTheirText)
{
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 2, 8);
    feed(terminal, "\033[1;31mAB\033[0mCD\033[?96h");
    Selection::Session session;
    Selection::beginGesture(session, terminal, 0, 0, 1, 0, {});
    Selection::dragGesture(session, terminal, 0, 3, 0);
    const auto copy = Selection::copyClipboard(session, terminal, {}, Selection::CopyStyle::Everything, { nullptr, 0 });
    CHECK(copy.plain == "DCBA");
    const auto first = copy.html.find(">DC</span>");
    const auto second = copy.html.find(">BA</span>");
    CHECK(first != std::string::npos);
    CHECK(second != std::string::npos);
    CHECK(first < second);
    CHECK(copy.html.find("font-weight:bold", first) < second);
    CHECK(copy.rtf.find("DC") < copy.rtf.find("BA"));
    CHECK(copy.rtf.find("{\\b\\cf", copy.rtf.find("DC")) < copy.rtf.find("BA"));
}

TEST(rightToLeftCopyJoinsWrappedLinesAndKeepsRectangleRows)
{
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 3, 4);
    feed(terminal, "ABCDEFGH\033[?96h");
    Selection::Session session;
    Selection::beginGesture(session, terminal, 0, 0, 1, 0, {});
    Selection::dragGesture(session, terminal, 1, 3, 0);
    CHECK(Selection::copiedText(session, terminal) == "HGFEDCBA");
    (void)Selection::clearSelection(session);
    Selection::beginGesture(session, terminal, 0, 0, 1, 0, { .isRectangular = true });
    Selection::dragGesture(session, terminal, 1, 2, 0);
    CHECK(Selection::copiedText(session, terminal) == "CBA\nGFE");
    feed(terminal, "\r\nIJ");
    Selection::selectAllText(session, terminal);
    CHECK(Selection::copiedText(session, terminal) == "HGFEDCBA\nJI");
}

TEST(rightToLeftCopyPreservesEmptyLinesAndEmptySelections)
{
    Terminal terminal;
    hardReset(terminal);
    resize(terminal, 4, 8);
    feed(terminal, "AB\r\n\r\nCD\033[?96h");
    Selection::Session session;
    CHECK(Selection::copiedText(session, terminal).empty());
    Selection::beginGesture(session, terminal, 0, 0, 1, 0, {});
    Selection::dragGesture(session, terminal, 2, 1, 0);
    CHECK(Selection::copiedText(session, terminal) == "BA\n\nDC");
}

UBIQUITTY_TEST_MAIN()
