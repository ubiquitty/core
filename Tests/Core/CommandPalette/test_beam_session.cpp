// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_beam_session — the palette's state machine.
//
// Ranking is `Engine`'s and has tests of its own. This covers the session
// around it, and every case here is a state a user reaches by accident rather
// than on purpose: typing into a list that just emptied, paging past the end,
// backspacing a multi-byte character, accepting nothing.
//
// The through-line is that the selection must always name a row that exists or
// no row at all. Every event that can move it is followed by an assertion on
// what it points at, because an index one past the end is the failure that
// reads as fine until something dereferences it.

#include "Harness.h"

#include "Core/CommandPalette/BeamSession.h"

#include <string>

using Ubiquitty::CommandPalette::BeamAction;
using Ubiquitty::CommandPalette::BeamEvent;
using Ubiquitty::CommandPalette::BeamSession;
using Ubiquitty::CommandPalette::PaletteCommand;
using Ubiquitty::CommandPalette::PaletteCommandList;
using Ubiquitty::CommandPalette::PaletteNumber;
using Ubiquitty::CommandPalette::Scope;
using Ubiquitty::CommandPalette::TerminalAvailability;

namespace {

PaletteCommandList threeCommands()
{
    PaletteCommandList commands;
    commands.push_back(PaletteCommand { "split.right", "Split Right", "Window", "", "", {}, {} });
    commands.push_back(PaletteCommand { "split.down", "Split Down", "Window", "", "", {}, {} });
    commands.push_back(PaletteCommand { "theme.pick", "Choose Theme", "Appearance", "", "", {}, {} });
    return commands;
}

BeamSession openedSession()
{
    BeamSession session;
    session.setCommands(Scope::Actions, threeCommands());
    session.apply(BeamEvent::Toggle);
    return session;
}

void type(BeamSession& session, const std::string& text)
{
    for (const char character : text)
        session.apply(BeamEvent::Character, static_cast<std::uint32_t>(static_cast<unsigned char>(character)));
}

} // namespace

TEST(beamSessionOpensClosedAndIgnoresEverythingElseWhileClosed)
{
    BeamSession session;
    session.setCommands(Scope::Actions, threeCommands());
    CHECK_MSG(!session.isOpen(), "a session starts closed");

    // A closed palette must not act on anything but Toggle. A caller that has
    // not checked isOpen() is a bug, and consuming the event would hide it while
    // stealing the user's keystroke.
    for (const BeamEvent event : { BeamEvent::Character, BeamEvent::Accept, BeamEvent::SelectNext, BeamEvent::Dismiss }) {
        const BeamAction action = session.apply(event, 'x');
        CHECK_MSG(action.kind == BeamAction::Kind::None, "a closed palette does nothing");
        CHECK_MSG(!session.isOpen(), "and stays closed");
    }
    CHECK_MSG(session.state().query.empty(), "in particular it did not accumulate the typing");

    session.apply(BeamEvent::Toggle);
    CHECK(session.isOpen());
    CHECK_EQ(static_cast<int>(session.state().rows.size()), 3);
    CHECK_EQ(session.state().selectedIndex, 0);

    const BeamAction closing = session.apply(BeamEvent::Toggle);
    CHECK_MSG(closing.kind == BeamAction::Kind::Closed, "toggling again closes and says so");
    CHECK(!session.isOpen());
}

TEST(beamSessionFiltersAndKeepsTheSelectionOnARowThatExists)
{
    BeamSession session = openedSession();

    type(session, "split");
    CHECK_EQ(static_cast<int>(session.state().rows.size()), 2);
    CHECK_MSG(session.state().selectedIndex == 0,
        "typing re-ranks, so the selection returns to the top rather than to whatever landed at its index");

    // The negative: a query matching nothing leaves no selection, not index 0
    // into an empty list.
    type(session, "zzz");
    CHECK_EQ(static_cast<int>(session.state().rows.size()), 0);
    CHECK_MSG(session.state().selectedIndex == -1, "an empty list selects nothing");
    CHECK_MSG(!session.state().status.empty(), "and says so rather than looking broken");

    // Accepting nothing does nothing — the case that would otherwise index -1.
    const BeamAction nothing = session.apply(BeamEvent::Accept);
    CHECK_MSG(nothing.kind == BeamAction::Kind::None, "accepting an empty list runs nothing");
    CHECK_MSG(session.isOpen(), "and does not close, because nothing happened");

    // Backspacing back into a match restores rows and a selection.
    for (int i = 0; i < 3; ++i)
        session.apply(BeamEvent::Backspace);
    CHECK_EQ(static_cast<int>(session.state().rows.size()), 2);
    CHECK_EQ(session.state().selectedIndex, 0);
}

TEST(beamSessionBackspacesWholeCharacters)
{
    BeamSession session = openedSession();

    // A two-byte character. Backspacing a BYTE would leave a truncated sequence
    // in the query — which matches nothing and draws as a replacement character,
    // so the delete appears to do nothing on exactly the input that needs it.
    session.apply(BeamEvent::Character, 0xE9); // é
    CHECK_EQ(static_cast<int>(session.state().query.size()), 2);
    session.apply(BeamEvent::Backspace);
    CHECK_MSG(session.state().query.empty(), "one backspace removes the whole character, not one byte");

    // And backspacing an empty query is a no-op rather than an underflow.
    session.apply(BeamEvent::Backspace);
    CHECK(session.state().query.empty());
}

TEST(beamSessionClampsEveryMotion)
{
    BeamSession session = openedSession();
    session.setViewportRows(2);
    const int rowCount = static_cast<int>(session.state().rows.size());
    CHECK_EQ(rowCount, 3);

    // Up from the top stays at the top; down from the bottom stays at the
    // bottom. Both are held rather than wrapped, so a long press does not cycle
    // the list under the user.
    session.apply(BeamEvent::SelectPrevious);
    CHECK_EQ(session.state().selectedIndex, 0);
    for (int i = 0; i < 10; ++i)
        session.apply(BeamEvent::SelectNext);
    CHECK_EQ(session.state().selectedIndex, rowCount - 1);

    // Paging past either end lands on the end, not past it.
    session.apply(BeamEvent::PageDown);
    CHECK_EQ(session.state().selectedIndex, rowCount - 1);
    session.apply(BeamEvent::PageUp);
    CHECK_MSG(session.state().selectedIndex >= 0, "paging up never goes negative");

    session.apply(BeamEvent::SelectFirst);
    CHECK_EQ(session.state().selectedIndex, 0);
    CHECK_MSG(session.state().scrollTop == 0, "and the window follows it back to the top");

    session.apply(BeamEvent::SelectLast);
    CHECK_EQ(session.state().selectedIndex, rowCount - 1);
    CHECK_MSG(session.state().scrollTop == rowCount - 2,
        "a viewport of 2 scrolls just far enough to show the last row");
    CHECK_MSG(session.state().scrollTop + 2 <= rowCount, "and never past the end of the list");
}

TEST(beamSessionAcceptsTheSelectedRowAndCloses)
{
    BeamSession session = openedSession();
    session.apply(BeamEvent::SelectNext);

    const BeamAction ran = session.apply(BeamEvent::Accept);
    CHECK_MSG(ran.kind == BeamAction::Kind::Run, "accepting runs something");
    CHECK_MSG(!ran.commandID.empty(), "and names which");
    CHECK_MSG(!ran.isAlternate, "the plain Enter is not the alternate");
    CHECK_MSG(!session.isOpen(),
        "and it closes — a palette left open puts the user typing into a filter over the command's own output");

    BeamSession alternate = openedSession();
    const BeamAction other = alternate.apply(BeamEvent::AcceptAlternate);
    CHECK(other.kind == BeamAction::Kind::Run);
    CHECK_MSG(other.isAlternate, "the modified Enter is recorded as the other way, not as the same way");
}

TEST(beamSessionAscendsThroughTheQueryBeforeClosing)
{
    BeamSession session = openedSession();
    type(session, "split");

    const BeamAction clearing = session.apply(BeamEvent::Ascend);
    CHECK_MSG(clearing.kind == BeamAction::Kind::None, "the first Ascend clears the query rather than closing");
    CHECK(session.state().query.empty());
    CHECK(session.isOpen());

    const BeamAction closing = session.apply(BeamEvent::Ascend);
    CHECK_MSG(closing.kind == BeamAction::Kind::Closed, "with nothing left to undo it closes");
    CHECK(!session.isOpen());
}

// --- The options drill ------------------------------------------------------
//
// Choosing a theme or a font is the reason the palette exists, and it is the
// half that was Swift-only: macOS drills into a value list, Core did not. These
// pin the semantics against macOS's, which is the authority.

namespace {

// A session already inside the theme drill, with four values and the second in
// effect. Not alphabetical: a drill that returned the row position
// instead of the value's own index passes every assertion on a sorted list.
BeamSession drilledSession()
{
    BeamSession session;
    session.setCommands(Scope::Actions, threeCommands());
    session.setOptions("theme.pick",
        { { "Solarized Dark" }, { "Nord" }, { "Gruvbox" }, { "Tokyo Night" } }, /*currentIndex=*/1);
    session.apply(BeamEvent::Toggle);
    for (const char ch : std::string("theme"))
        session.apply(BeamEvent::Character, static_cast<std::uint32_t>(ch));
    session.apply(BeamEvent::Descend);
    return session;
}

int rowTitled(const BeamSession& session, const std::string& title)
{
    for (std::size_t i = 0; i < session.state().rows.size(); ++i)
        if (session.state().rows[i].title == title)
            return static_cast<int>(i);
    return -1;
}

} // namespace

TEST(beamSessionDrillsIntoAValueList)
{
    BeamSession session = drilledSession();

    CHECK_MSG(session.isDrilled(), "Descend entered the drill");
    CHECK_STR_EQ(session.state().drillTitle.c_str(), "Choose Theme");
    CHECK_MSG(session.state().rows.size() == 4, "every value is listed");
    CHECK_MSG(session.state().query.empty(), "and the command filter was cleared for the value filter");

    // It lands on the CURRENT value, so Enter straight after Descend changes
    // nothing. Landing on the top row would silently set whatever sorted first.
    const int nord = rowTitled(session, "Nord");
    CHECK_MSG(nord >= 0, "the current value is listed");
    CHECK_MSG(session.state().selectedIndex == nord, "and is what the drill opens on");
    CHECK_MSG(session.state().rows[static_cast<std::size_t>(nord)].isChecked, "marked as current");

    // A row with no registered values has nothing to enter, and Descend must
    // not invent a drill for it.
    BeamSession plain;
    plain.setCommands(Scope::Actions, threeCommands());
    plain.apply(BeamEvent::Toggle);
    for (const char ch : std::string("split"))
        plain.apply(BeamEvent::Character, static_cast<std::uint32_t>(ch));
    plain.apply(BeamEvent::Descend);
    CHECK_MSG(!plain.isDrilled(), "a command with no values does not drill");
}

// The assertion that separates a correct drill from one that happens to work on
// a list nobody filtered: accepting must name the value's OWN index, not where
// it landed after ranking.
TEST(beamSessionAcceptsTheValuesOwnIndexNotItsRowPosition)
{
    BeamSession session = drilledSession();

    // "gru" ranks Gruvbox — original index 2 — into row 0.
    for (const char ch : std::string("gru"))
        session.apply(BeamEvent::Character, static_cast<std::uint32_t>(ch));
    CHECK_MSG(session.state().rows.size() == 1, "the filter narrowed to one value");
    CHECK_STR_EQ(session.state().rows[0].title.c_str(), "Gruvbox");
    CHECK_MSG(session.state().selectedIndex == 0, "which is the row selected");

    const BeamAction action = session.apply(BeamEvent::Accept);
    CHECK_MSG(action.kind == BeamAction::Kind::Run, "accepting runs it");
    CHECK_STR_EQ(action.commandID.c_str(), "theme.pick");
    CHECK_MSG(action.optionIndex == 2, "the value's own index, not the row it occupied");
    CHECK_MSG(!session.isOpen(), "and the palette closed");
    CHECK_MSG(!session.isDrilled(), "leaving no drill behind for the next open");
}

// A plain command still reports no option, so a caller can tell "run this" from
// "set this to that" without consulting its own tables.
TEST(beamSessionReportsNoOptionForAPlainCommand)
{
    BeamSession session;
    session.setCommands(Scope::Actions, threeCommands());
    session.apply(BeamEvent::Toggle);
    for (const char ch : std::string("down"))
        session.apply(BeamEvent::Character, static_cast<std::uint32_t>(ch));
    const BeamAction action = session.apply(BeamEvent::Accept);
    CHECK_MSG(action.kind == BeamAction::Kind::Run, "it ran");
    CHECK_STR_EQ(action.commandID.c_str(), "split.down");
    CHECK_MSG(action.optionIndex == -1, "and named no option");
}

// Escape inside a drill goes BACK and never dismisses — macOS's rule, so it is
// the rule. Closing from a value list would throw away the command the user had
// already found, and the key that means "back" everywhere else in the palette
// would be the one key that means "give up".
TEST(beamSessionEscapeBacksOutOfADrillWithoutClosing)
{
    for (const BeamEvent back : { BeamEvent::Dismiss, BeamEvent::Ascend }) {
        BeamSession session = drilledSession();
        const BeamAction action = session.apply(back);

        CHECK_MSG(action.kind == BeamAction::Kind::None, "backing out is not a close");
        CHECK_MSG(session.isOpen(), "the palette is still open");
        CHECK_MSG(!session.isDrilled(), "and out of the drill");

        // The command filter comes back. Dropping it would strand the user at an
        // unfiltered list they have to retype their way into.
        CHECK_STR_EQ(session.state().query.c_str(), "theme");
        CHECK_MSG(rowTitled(session, "Choose Theme") >= 0, "showing the command list again");
    }
}

// The toggle key is not a back gesture and never was: it closes outright from
// anywhere, and must not leave a drill set for the next open to inherit.
TEST(beamSessionToggleClosesFromInsideADrill)
{
    BeamSession session = drilledSession();
    const BeamAction action = session.apply(BeamEvent::Toggle);

    CHECK_MSG(action.kind == BeamAction::Kind::Closed, "the toggle closed it");
    CHECK_MSG(!session.isOpen() && !session.isDrilled(), "with no drill left behind");

    // Reopening starts at the command list rather than back inside the values.
    session.apply(BeamEvent::Toggle);
    CHECK_MSG(session.isOpen() && !session.isDrilled(), "reopening lands on the commands");
    CHECK_MSG(session.state().drillTitle.empty(), "and draws no breadcrumb");
}

TEST(beamSessionGenerationMovesOnlyWhenSomethingDid)
{
    BeamSession session = openedSession();
    const std::uint64_t opened = session.state().generation;

    session.apply(BeamEvent::Character, 's');
    CHECK_MSG(session.state().generation > opened, "typing is a change");

    const std::uint64_t typed = session.state().generation;
    session.apply(BeamEvent::Descend); // nothing to descend into
    CHECK_MSG(session.state().generation == typed,
        "an event that changed nothing does not bump the generation — a frontend skipping "
        "unchanged frames would otherwise redraw on every ignored key");
}

// MARK: - the number drill
//
// The field is the value here, which makes every failure quiet: a misparse does
// not throw, it substitutes a different number and applies it. So the cases
// below lead with the entries that must not be read as numbers, and each one
// asserts the value that arrives at the setter rather than what the field shows.

namespace {

// `padding` as Core's own schema states it — Preferences/Schema.h's
// [appearance] padding row, 0–64 pt in whole points. Six is the built-in
// default and sits away from both bounds: a step that clamped when it should
// not have still passes every assertion that starts from an edge.
PaletteNumber paddingSchema()
{
    return PaletteNumber { 6.0, 0.0, 64.0, 1.0, "pt" };
}

PaletteCommandList commandsWithPadding()
{
    PaletteCommandList commands = threeCommands();
    commands.push_back(PaletteCommand { "beam.padding", "Window Padding", "Appearance", "", "", {},
        Ubiquitty::CommandPalette::CommandKind::DrillNumber });
    return commands;
}

// A session already inside the padding editor.
BeamSession numberSession()
{
    BeamSession session;
    session.setCommands(Scope::Actions, commandsWithPadding());
    session.setNumber("beam.padding", paddingSchema());
    session.apply(BeamEvent::Toggle);
    type(session, "padding");
    session.apply(BeamEvent::Descend);
    return session;
}

// Enter, and the value the caller is told to apply.
double accepted(BeamSession& session)
{
    const BeamAction action = session.apply(BeamEvent::Accept);
    CHECK_MSG(action.kind == BeamAction::Kind::Run, "accepting a value editor runs its command");
    CHECK_STR_EQ(action.commandID.c_str(), "beam.padding");
    return action.numberValue;
}

} // namespace

TEST(beamSessionNumberDrillOpensOnTheLiveValue)
{
    BeamSession session = numberSession();

    CHECK_MSG(session.isDrilled(), "Descend entered the editor");
    CHECK_MSG(session.state().isValueEditor,
        "and said so — an empty row list inside a drill is otherwise indistinguishable "
        "from an options list filtered down to nothing");
    CHECK_STR_EQ(session.state().drillTitle.c_str(), "Window Padding");
    CHECK_MSG(session.state().rows.empty(), "a value editor has no list");
    CHECK_MSG(session.state().selectedIndex == -1, "and so selects no row");

    // The live value, not an empty box: an empty field reads as "unset", and the
    // first Enter would apply a fallback the user never saw.
    CHECK_STR_EQ(session.state().query.c_str(), "6");
    CHECK_STR_EQ(session.state().status.c_str(), "0–64 pt");
}

// The registration is what makes the drill, not the declared kind. A command
// whose schema was never registered must stay an ordinary action rather than
// advertise an editor that opens empty.
TEST(beamSessionNumberDrillNeedsItsSchemaRegistered)
{
    BeamSession session;
    session.setCommands(Scope::Actions, commandsWithPadding());
    session.apply(BeamEvent::Toggle);
    type(session, "padding");

    session.apply(BeamEvent::Descend);
    CHECK_MSG(!session.isDrilled(), "an unregistered number does not drill");
    CHECK_MSG(!session.state().isValueEditor, "and claims no editor");

    const BeamAction action = session.apply(BeamEvent::Accept);
    CHECK_MSG(action.kind == BeamAction::Kind::Run, "it runs as a plain command instead");
    CHECK_STR_EQ(action.commandID.c_str(), "beam.padding");
}

TEST(beamSessionNumberDrillRefusesAnEntryThatIsNotWhollyANumber)
{
    // Each of these is a field a user reaches mid-typing or mid-paste. None of
    // them is a number, and the value that must arrive at the setter is the one
    // the setting already had — 6 — rather than a leading digit run.
    for (const char* partial : { "12px", "1.2.3", "", "   ", "-", "px", "1 2" }) {
        BeamSession session = numberSession();
        for (int i = 0; i < 8; ++i)
            session.apply(BeamEvent::Backspace); // clear the prefilled value
        type(session, partial);
        CHECK_MSG(accepted(session) == 6.0, partial);
    }

    // The same refusal on the STEP path, which is the one that overwrites the
    // field: stepping off a misread would discard the half-typed entry and put a
    // number the user never asked for in its place.
    BeamSession session = numberSession();
    for (int i = 0; i < 8; ++i)
        session.apply(BeamEvent::Backspace);
    type(session, "12px");
    session.apply(BeamEvent::SelectPrevious);
    CHECK_STR_EQ(session.state().query.c_str(), "7"); // 6 + 1, off the registered value
}

// strtod spells "inf" and "nan", and gridFloor either is not a value: under IEEE
// comparison max(NaN, low) is NaN, so a NaN that reached the clamp would survive
// it and land in the setter.
TEST(beamSessionNumberDrillRefusesInfinityAndNaN)
{
    for (const char* spelled : { "inf", "-inf", "nan", "infinity" }) {
        BeamSession session = numberSession();
        for (int i = 0; i < 8; ++i)
            session.apply(BeamEvent::Backspace);
        type(session, spelled);
        const double value = accepted(session);
        CHECK_MSG(value == 6.0, spelled);
    }
}

TEST(beamSessionNumberDrillStepsAndClampsAtBothBounds)
{
    BeamSession session = numberSession();

    // Up increases, matching the top half of a ± stepper.
    session.apply(BeamEvent::SelectPrevious);
    CHECK_STR_EQ(session.state().query.c_str(), "7");
    session.apply(BeamEvent::SelectNext);
    session.apply(BeamEvent::SelectNext);
    CHECK_STR_EQ(session.state().query.c_str(), "5");

    // Down past the floor stops at it rather than going negative.
    for (int i = 0; i < 20; ++i)
        session.apply(BeamEvent::SelectNext);
    CHECK_STR_EQ(session.state().query.c_str(), "0");

    // And up past the ceiling stops at 64. Both bounds, because a clamp written
    // once and applied to one end is the shape that ships half-working.
    for (int i = 0; i < 200; ++i)
        session.apply(BeamEvent::SelectPrevious);
    CHECK_STR_EQ(session.state().query.c_str(), "64");
    CHECK_MSG(accepted(session) == 64.0, "and the ceiling is what the setter is handed");
}

TEST(beamSessionNumberDrillStepsOffWhatIsInTheFieldNotWhatWasRegistered)
{
    BeamSession session = numberSession();
    for (int i = 0; i < 8; ++i)
        session.apply(BeamEvent::Backspace);
    type(session, "20");

    session.apply(BeamEvent::SelectPrevious);
    CHECK_STR_EQ(session.state().query.c_str(), "21");
}

TEST(beamSessionNumberDrillClampsATypedEntryOnAccept)
{
    // Typed past the ceiling. The parse succeeds, so nothing falls back — the
    // bounds are the only thing standing between the schema and a value it says
    // cannot exist, and they are applied here so no frontend has to remember to.
    BeamSession session = numberSession();
    for (int i = 0; i < 8; ++i)
        session.apply(BeamEvent::Backspace);
    type(session, "999");
    CHECK_MSG(accepted(session) == 64.0, "a typed entry is clamped too");
    CHECK_MSG(!session.isOpen(), "and applying a value closes, exactly like running a command");
    CHECK_MSG(!session.isDrilled(), "with no editor left behind");
}

TEST(beamSessionNumberDrillPagingAndHomeEndLeaveTheFieldAlone)
{
    // There is no list for these to move through, and the field is the value —
    // so the failure they must not have is editing it.
    for (const BeamEvent event : { BeamEvent::PageUp, BeamEvent::PageDown, BeamEvent::SelectFirst, BeamEvent::SelectLast }) {
        BeamSession session = numberSession();
        session.apply(event);
        CHECK_STR_EQ(session.state().query.c_str(), "6");
        CHECK_MSG(session.state().isValueEditor, "and the editor is still open");
    }
}

TEST(beamSessionNumberDrillEscapeRestoresTheCommandFilter)
{
    BeamSession session = numberSession();
    session.apply(BeamEvent::SelectPrevious); // move the value, then abandon it

    session.apply(BeamEvent::Dismiss);
    CHECK_MSG(session.isOpen(), "Escape backs out of the editor without closing");
    CHECK_MSG(!session.isDrilled() && !session.state().isValueEditor, "and leaves no editor state");
    CHECK_STR_EQ(session.state().query.c_str(), "padding");

    // Re-entering shows the REGISTERED value again: the step was abandoned, and
    // an editor that reopened on it would report a setting that never changed.
    session.apply(BeamEvent::Descend);
    CHECK_STR_EQ(session.state().query.c_str(), "6");
}

// MARK: - scope prefixes
//
// `PaletteScope.h` declares five scopes and `Engine::interpretInput` resolves a
// leading prefix into one, strips it exactly once, and refuses a scope needing a
// terminal when none is attached. All of it portable, and the session every
// non-macOS frontend runs never consulted it — so '@' filtered the Actions list
// for a literal '@' while macOS, which switches in its own Swift, moved scope.
//
// A frontend cannot fix that for itself without re-deriving the prefix table,
// which is how two frontends come to disagree about one keystroke.

namespace {

PaletteCommandList oneBlock()
{
    PaletteCommandList commands;
    commands.push_back(PaletteCommand { "block.42", "npm test", "Blocks", "", "", {}, {} });
    return commands;
}

} // namespace

TEST(beamSessionEntersAScopeByItsPrefix)
{
    BeamSession session;
    session.setCommands(Scope::Actions, threeCommands());
    session.setCommands(Scope::Blocks, oneBlock());
    session.setTerminalAvailability(TerminalAvailability::Available);
    session.apply(BeamEvent::Toggle);

    type(session, "@");
    CHECK_MSG(session.state().scope == Scope::Blocks, "the prefix retargeted the palette");
    CHECK_MSG(session.state().query.empty(),
        "and was stripped rather than left to filter the new scope for a character nobody typed");

    // The scope's own rows, not the actions'.
    CHECK_MSG(session.state().rows.size() == 1, "the Blocks list is what is ranked");
    CHECK_STR_EQ(session.state().rows[0].title.c_str(), "npm test");
}

// The half that decides whether the gate is real. A scope that needs a terminal
// must not be enterable without one — and the '@' stays in the query, because a
// refused prefix is ordinary text rather than a silent no-op.
TEST(beamSessionRefusesATerminalScopeWithNoTerminal)
{
    BeamSession session;
    session.setCommands(Scope::Actions, threeCommands());
    session.setCommands(Scope::Blocks, oneBlock());
    session.setTerminalAvailability(TerminalAvailability::Unavailable);
    session.apply(BeamEvent::Toggle);

    type(session, "@");
    CHECK_MSG(session.state().scope == Scope::Actions, "no terminal, no terminal-scoped list");
    CHECK_STR_EQ(session.state().query.c_str(), "@");
}

// The prefix is consumed once. A second '@' is a character in the query of a
// scope already entered, not a re-entry — feeding the stripped query back
// through the parse would eat a character the user typed.
TEST(beamSessionStripsThePrefixExactlyOnce)
{
    BeamSession session;
    session.setCommands(Scope::Actions, threeCommands());
    session.setCommands(Scope::Blocks, oneBlock());
    session.setTerminalAvailability(TerminalAvailability::Available);
    session.apply(BeamEvent::Toggle);

    type(session, "@@");
    CHECK_MSG(session.state().scope == Scope::Blocks, "still the scope the first prefix chose");
    CHECK_STR_EQ(session.state().query.c_str(), "@");
}

// Escape leaves a scope before it closes the palette, so a user who mistyped a
// prefix is not thrown out of the palette to correct it.
TEST(beamSessionEscapeLeavesTheScopeBeforeItCloses)
{
    BeamSession session;
    session.setCommands(Scope::Actions, threeCommands());
    session.setCommands(Scope::Blocks, oneBlock());
    session.setTerminalAvailability(TerminalAvailability::Available);
    session.apply(BeamEvent::Toggle);
    type(session, "@");

    const BeamAction back = session.apply(BeamEvent::Dismiss);
    CHECK_MSG(back.kind == BeamAction::Kind::None, "the first Escape does not close");
    CHECK_MSG(session.isOpen(), "the palette is still open");
    CHECK_MSG(session.state().scope == Scope::Actions, "and back at the actions");

    const BeamAction closing = session.apply(BeamEvent::Dismiss);
    CHECK_MSG(closing.kind == BeamAction::Kind::Closed, "the second Escape closes");
}

// A scope with no commands registered ranks EMPTY, not the previous scope's.
//
// activateScope hands the engine an empty list when the map has no entry, and
// no other case reaches that branch: every prefix case registers its scope
// first, which is the arrangement where the omission cannot show. The failure it
// guards is not a crash — it is the palette drawing the Actions commands under a
// Blocks chip, so the header names one thing and the rows are another, and the
// user's next Enter runs a command from a scope they did not think they were in.
TEST(beamSessionRanksAnUnregisteredScopeEmptyRatherThanTheOneItLeft)
{
    BeamSession session;
    session.setCommands(Scope::Actions, threeCommands());
    // Blocks is not registered: the scope exists in the prefix table whether or
    // not a caller filled it, and this is the arm where it did not.
    session.setTerminalAvailability(TerminalAvailability::Available);
    session.apply(BeamEvent::Toggle);
    CHECK_MSG(session.state().rows.size() == 3, "the actions are what is ranked to begin with");

    type(session, "@");
    CHECK_MSG(session.state().scope == Scope::Blocks, "the prefix still retargets — an empty scope is enterable");
    CHECK_MSG(session.state().rows.empty(),
        "and ranks nothing: the actions must not survive under a Blocks chip");
    CHECK_MSG(session.state().selectedIndex == -1,
        "with no row selected, so Enter cannot apply a command from the scope just left");
}

UBIQUITTY_TEST_MAIN()
