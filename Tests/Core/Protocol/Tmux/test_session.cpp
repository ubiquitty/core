// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Tmux/Tmux.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Harness.h"

// test_session.cpp — portable Session model: layout-tree parsing,
// applyLayout pane reconcile, and %output delivery.

#include "Core/OSC/Dispatch.h"
#include "Core/Protocol/Tmux/Session.h"
#include "Core/Selection/TerminalSelection.h"
#include "Core/Themes/TermPaletteFromTheme.h"

#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Tmux;

// In-order leaf walk of a binary Panes::Tree → left-to-right pane ids, the same
// order tmux lists the layout's leaves.
static void collectTreeLeaves(const Ubiquitty::Panes::Tree& tree, int nodeIndex, std::vector<int>& out)
{
    if (tree.isLeaf(nodeIndex)) {
        out.push_back(tree.node(nodeIndex).paneID);
        return;
    }
    const Ubiquitty::Panes::Node& n = tree.node(nodeIndex);
    collectTreeLeaves(tree, n.first, out);
    collectTreeLeaves(tree, n.second, out);
}

// A tmux pane's setGeometry grow blanks the newly-revealed band (unlike a
// bare Terminal::resize(..., Fixed), which keeps it because no resync follows
// to repaint the band; CellGrid keeps the revealed cells for that reason).
// setGeometry's growth always gets a content-resync scheduled right after
// (TmuxControlHost.cpp), so showing stale backing-store content there (possibly
// wrong, from a since-changed server state) is worse than a brief blank
// until the resync lands with the real capture.
// A height grow blanks the newly-revealed bottom rows the same way.
/// A hostile server minting unlimited distinct @N window IDs via %layout-change
/// must not grow the Session without bound: addWindowLocked refuses past
/// tmuxMaxSessionWindows, so the portable model is capped by construction
/// (verify-by-revert: drop the guard and the size check below fails).
TEST(testParseSinglePane)
{
    LayoutNode root;
    const char* s = "bf3a,80x24,0,0,1";
    CHECK(parseLayoutTree(s, root));
    CHECK(root.isLeaf());
    CHECK(root.paneID == 1);
    CHECK(root.width == 80 && root.height == 24);
    CHECK(root.x == 0 && root.y == 0);
    CHECK(root.children.empty());
}

TEST(testParseColumnsSplit)
{
    LayoutNode root;
    // 80 wide = 40 + 1 divider + 39, side by side ({} = columns).
    const char* s = "abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}";
    CHECK(parseLayoutTree(s, root));
    CHECK(root.split == Split::Columns);
    CHECK(root.children.size() == 2);
    CHECK(root.children[0].isLeaf() && root.children[0].paneID == 1);
    CHECK(root.children[0].width == 40 && root.children[0].x == 0);
    CHECK(root.children[1].isLeaf() && root.children[1].paneID == 2);
    CHECK(root.children[1].width == 39 && root.children[1].x == 41);
}

TEST(testParseRowsSplit)
{
    LayoutNode root;
    // 24 tall = 12 + 1 divider + 11, stacked ([] = rows).
    const char* s = "abcd,80x24,0,0[80x12,0,0,1,80x11,0,13,2]";
    CHECK(parseLayoutTree(s, root));
    CHECK(root.split == Split::Rows);
    CHECK(root.children.size() == 2);
    CHECK(root.children[0].paneID == 1 && root.children[0].height == 12 && root.children[0].y == 0);
    CHECK(root.children[1].paneID == 2 && root.children[1].height == 11 && root.children[1].y == 13);
}

TEST(testParseNested)
{
    LayoutNode root;
    // Columns of [left leaf | (rows of two)].
    const char* s = "abcd,80x24,0,0{40x24,0,0,1,39x24,41,0[39x12,41,0,2,39x11,41,13,3]}";
    CHECK(parseLayoutTree(s, root));
    CHECK(root.split == Split::Columns);
    CHECK(root.children.size() == 2);
    CHECK(root.children[0].isLeaf() && root.children[0].paneID == 1);
    CHECK(root.children[1].split == Split::Rows);
    CHECK(root.children[1].children.size() == 2);
    CHECK(root.children[1].children[0].paneID == 2);
    CHECK(root.children[1].children[1].paneID == 3);
}

TEST(testParseRejectsMalformed)
{
    LayoutNode root;
    CHECK(!parseLayoutTree("", root));
    CHECK(!parseLayoutTree(std::string_view {}, root));
    CHECK(!parseLayoutTree("garbage", root));
    CHECK(!parseLayoutTree("bf3a,80x24,0,0{", root)); // unterminated split

    // A >5-digit dimension can't integer-overflow the parser — rejected.
    const std::string overflow = "bf3a,999999x24,0,0,0";
    CHECK(!parseLayoutTree(overflow, root));

    // Nesting past tmuxMaxLayoutDepth is rejected before the recursion can
    // exhaust the ingest-thread stack.
    std::string deep = "bf3a,";
    for (int i = 0; i < 64; ++i)
        deep += "1x1,0,0{";
    deep += "1x1,0,0,0";
    for (int i = 0; i < 64; ++i)
        deep += "}";
    CHECK(!parseLayoutTree(deep, root));
}

TEST(testApplyLayoutCreatesAndRemovesPanes)
{
    Session session;
    const char* two = "abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}";
    session.applyLayout(0, two);
    Window* w = session.window(0);
    CHECK(w != nullptr);
    CHECK(w->panes().size() == 2);
    CHECK(w->pane(1) != nullptr && w->pane(2) != nullptr);
    CHECK(w->pane(1)->columns() == 40 && w->pane(2)->columns() == 39);
    CHECK(session.pane(2) != nullptr); // session-wide lookup

    // Collapse to a single pane: pane 2 should be dropped, pane 1 resized.
    const char* one = "ef01,80x24,0,0,1";
    session.applyLayout(0, one);
    CHECK(w->panes().size() == 1);
    CHECK(w->pane(1) != nullptr && w->pane(2) == nullptr);
    CHECK(w->pane(1)->columns() == 80);
}

TEST(aPaneIsBornWithTheSessionsSequenceGate)
{
    // The Session mints the pane's Terminal itself and reads no config, so the
    // gate it was handed is the only route to a pane that refuses a denied OSC.
    Session session;
    Ubiquitty::OSC::SequenceGate gate;
    gate.oscDeny[0] = 2;
    gate.oscDenyCount = 1;
    session.setPaneSequenceGate(gate);
    session.applyLayout(0, "ef01,80x24,0,0,7");
    Pane* p = session.pane(7);
    CHECK(p != nullptr && p->term() != nullptr);
    if (!p || !p->term())
        return;
    CHECK(!Ubiquitty::OSC::oscAllowed(*p->term(), 2));

    const char* deniedTitle = "\033]2;blocked\033\\";
    session.deliverOutput(7, { reinterpret_cast<const unsigned char*>(deniedTitle), std::strlen(deniedTitle) });
    CHECK(p->term()->titleUTF8.empty());
    // OSC 0 is not in the deny list, so the same pane still takes that title.
    const char* allowedTitle = "\033]0;shown\033\\";
    session.deliverOutput(7, { reinterpret_cast<const unsigned char*>(allowedTitle), std::strlen(allowedTitle) });
    CHECK(p->term()->titleUTF8 == "shown");

    // A session handed no gate mints a pane that passes everything.
    Session open;
    open.applyLayout(0, "ef01,80x24,0,0,3");
    Pane* openPane = open.pane(3);
    CHECK(openPane != nullptr && openPane->term() != nullptr);
    if (!openPane || !openPane->term())
        return;
    open.deliverOutput(3, { reinterpret_cast<const unsigned char*>(deniedTitle), std::strlen(deniedTitle) });
    CHECK(openPane->term()->titleUTF8 == "blocked");
}

TEST(aPaneIsBornWithTheSessionsOSC52Policy)
{
    // The same route as the sequence gate: the policy the session was handed
    // is the only way a pane refuses the clipboard write its config denies.
    Session session;
    Ubiquitty::OSC::OSC52Policy policy;
    policy.write = Ubiquitty::OSC::ClipboardWritePolicy::Deny;
    policy.read = Ubiquitty::OSC::ClipboardReadPolicy::Deny;
    session.setPaneOSC52Policy(policy);
    session.applyLayout(0, "ef01,80x24,0,0,7");
    Pane* p = session.pane(7);
    CHECK(p != nullptr && p->term() != nullptr);
    if (!p || !p->term())
        return;
    CHECK(!Ubiquitty::OSC::osc52WriteAllowed(*p->term()));
    CHECK(p->term()->osc52ClipboardReadPolicy == Ubiquitty::OSC::ClipboardReadPolicy::Deny);

    // A session handed no policy mints a pane under the unset-key defaults.
    Session open;
    open.applyLayout(0, "ef01,80x24,0,0,3");
    Pane* openPane = open.pane(3);
    CHECK(openPane != nullptr && openPane->term() != nullptr);
    if (!openPane || !openPane->term())
        return;
    CHECK(Ubiquitty::OSC::osc52WriteAllowed(*openPane->term()));
    CHECK(openPane->term()->osc52ClipboardReadPolicy == Ubiquitty::OSC::ClipboardReadPolicy::Prompt);
}

TEST(panesInheritControllingPaletteWithoutOverwritingReboundPanes)
{
    Ubiquitty::Terminal controlling;
    CHECK(Ubiquitty::Themes::applyThemeToTerminal(controlling, Ubiquitty::Themes::indexForName("ANSI Light")));
    const auto baseline = controlling.paletteBaseline;
    controlling.palette.indexed[3] = { 123, 456, 789 };
    controlling.palette.indexedAlpha[3] = 91;
    Session session;
    session.setPanePalette(controlling.palette, controlling.paletteBaseline);
    session.applyLayout(0, "ef01,80x24,0,0,7");
    Pane* pane = session.pane(7);
    CHECK(pane != nullptr);
    if (!pane) {
        return;
    }
    CHECK(pane->term()->palette.defaultBackground == baseline.defaultBackground);
    CHECK(pane->term()->palette.defaultForeground == baseline.defaultForeground);
    CHECK(pane->term()->palette.cursor == baseline.cursor);
    CHECK(pane->term()->palette.indexed[3] == controlling.palette.indexed[3]);
    CHECK(pane->term()->palette.indexedAlpha[3] == 91);
    CHECK(pane->term()->paletteBaseline.indexed[3] == baseline.indexed[3]);

    constexpr std::string_view programPalette = "\033]11;#123456\033\\";
    session.deliverOutput(7, { reinterpret_cast<const uint8_t*>(programPalette.data()), programPalette.size() });
    const auto programBackground = pane->term()->palette.defaultBackground;
    CHECK(programBackground != baseline.defaultBackground);
    CHECK(Ubiquitty::Themes::applyThemeToTerminal(controlling, Ubiquitty::Themes::indexForName("ANSI Dark")));
    session.setPanePalette(controlling.palette, controlling.paletteBaseline);
    session.applyLayout(0, "ef01,80x24,0,0,7");
    CHECK(session.pane(7) == pane);
    CHECK(pane->term()->palette.defaultBackground == programBackground);
    CHECK(pane->term()->paletteBaseline.defaultBackground == baseline.defaultBackground);
    session.applyLayout(1, "ef01,80x24,0,0,8");
    CHECK(session.pane(8)->term()->palette.defaultBackground == controlling.palette.defaultBackground);

    constexpr std::string_view reset = "\033]111\033\\";
    session.deliverOutput(7, { reinterpret_cast<const uint8_t*>(reset.data()), reset.size() });
    CHECK(pane->term()->palette.defaultBackground == baseline.defaultBackground);
}

TEST(testDeliverOutputRoutesToPane)
{
    Session session;
    const char* one = "ef01,80x24,0,0,7";
    session.applyLayout(0, one);
    Pane* p = session.pane(7);
    CHECK(p != nullptr && p->term() != nullptr);
    CHECK(!p->consumeSnapshotDue());
    const unsigned char bytes[] = { 'h', 'i' };
    session.deliverOutput(7, { bytes, 2 });
    CHECK(p->consumeSnapshotDue()); // deliver ran → flagged a republish
    CHECK(!p->consumeSnapshotDue()); // and cleared it
    // Unknown pane: no-op, no crash.
    session.deliverOutput(999, { bytes, 2 });
}

namespace {

// A recovered screen whose rows are `rows`, each wrapped where `wrapped` says.
RecoveredScreen recoveredScreen(CaptureScope scope, const std::vector<std::string>& rows, int cursorX, int cursorY, const std::vector<bool>& wrapped = {})
{
    RecoveredScreen screen;
    screen.scope = scope;
    screen.cursorX = cursorX;
    screen.cursorY = cursorY;
    for (std::size_t index = 0; index < rows.size(); ++index)
        screen.rows.push_back({ rows[index], index < wrapped.size() && wrapped[index] });
    return screen;
}

// What a selection from one visible cell to another copies.
std::string copiedText(Ubiquitty::Terminal& term, int fromRow, int fromColumn, int toRow, int toColumn)
{
    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, term, fromRow, fromColumn, /*clickCount*/ 1, 0, {});
    Ubiquitty::Selection::dragGesture(selection, term, toRow, toColumn, 0);
    Ubiquitty::Selection::endGesture(selection);
    return Ubiquitty::Selection::selectedText(selection, term);
}

} // namespace

TEST(aFullHistoryRecoveryReplacesThePanesAbsoluteRows)
{
    Session session;
    session.applyLayout(0, "ef01,80x24,0,0,7");
    Pane* pane = session.pane(7);
    CHECK(pane != nullptr && pane->term() != nullptr);
    if (!pane || !pane->term()) {
        return;
    }

    const std::uint32_t beforeCurrentScreen = pane->term()->absoluteRowEpoch;
    CHECK(session.deliverRecovery(7, recoveredScreen(CaptureScope::CurrentScreen, { "fresh" }, 5, 0), 80, 24));
    CHECK(pane->term()->absoluteRowEpoch == beforeCurrentScreen);

    CHECK(session.deliverRecovery(7, recoveredScreen(CaptureScope::FullHistory, { "fresh" }, 5, 0), 80, 24));
    CHECK(pane->term()->absoluteRowEpoch != beforeCurrentScreen);
}

TEST(anOrdinaryContentResyncPreservesALiveSelectionAndFullHistoryCancelsIt)
{
    Session session;
    session.applyLayout(0, "ef01,80x24,0,0,7");
    Pane* pane = session.pane(7);
    CHECK(pane != nullptr && pane->term() != nullptr);
    if (!pane || !pane->term()) {
        return;
    }
    Ubiquitty::Terminal& term = *pane->term();
    Ubiquitty::processBytes(&term, "anchor\r\nmiddle\r\npointer");

    Ubiquitty::Selection::Session selection;
    Ubiquitty::Selection::init(selection);
    Ubiquitty::Selection::beginGesture(selection, term, /*visibleRow*/ 0, /*column*/ 0, /*clickCount*/ 1, 0, {});
    Ubiquitty::Selection::dragGesture(selection, term, /*visibleRow*/ 2, /*column*/ 6, 0);
    Ubiquitty::Selection::endGesture(selection);
    REQUIRE(Ubiquitty::Selection::hasSelection(selection));

    // The common case, per setGeometry's own comment above: a resync scoped to
    // the current screen replaces the pane's TEXT but keeps its absolute-row
    // identity, so a standing selection has to survive it exactly as it would
    // survive ordinary local output -- this is the pane-owning Session path a
    // Terminal-level epoch check alone cannot prove, since the epoch not
    // moving says nothing about whether the Selection::Session on top of it
    // was ever asked and answered correctly.
    const std::vector<std::string> rows { "anchor", "middle", "pointer" };
    CHECK(session.deliverRecovery(7, recoveredScreen(CaptureScope::CurrentScreen, rows, 7, 2), 80, 24));
    CHECK(Ubiquitty::Selection::hasSelection(selection, term));
    const std::string afterCurrentScreen = Ubiquitty::Selection::selectedText(selection, term);
    CHECK_MSG(afterCurrentScreen.find("anchor") != std::string::npos,
        "an ordinary content resync does not cancel a standing selection");

    // A full-history recovery replaces the coordinate space outright (proven
    // at the Terminal level above), so the same live selection is cancelled
    // rather than silently reattached to whatever the resync's own content
    // now occupies those absolute rows.
    CHECK(session.deliverRecovery(7, recoveredScreen(CaptureScope::FullHistory, rows, 7, 2), 80, 24));
    CHECK(!Ubiquitty::Selection::hasSelection(selection, term));
}

// tmux reports a row filled to the right edge, with the wrap still deferred,
// as a cursor one column past the last. Recovered in either scope, the pane's
// next character goes on the next row and the last column keeps its own; a
// cursor on the last column overwrites it, as it does in tmux.
TEST(aRecoveredCursorPastTheRightEdgeKeepsTheDeferredWrap)
{
    const auto characterAt = [](const Ubiquitty::Terminal& term, int row, int column) {
        const Ubiquitty::Cell* cell = term.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
        return cell ? cell->codePoint : 0;
    };
    for (const CaptureScope scope : { CaptureScope::CurrentScreen, CaptureScope::FullHistory }) {
        for (const int cursorColumn : { 10, 9 }) {
            Session session;
            session.applyLayout(0, "ef01,10x4,0,0,7");
            Pane* pane = session.pane(7);
            REQUIRE(pane != nullptr && pane->term() != nullptr);
            Ubiquitty::Terminal& term = *pane->term();
            REQUIRE(term.columns == 10);
            REQUIRE(session.deliverRecovery(7, recoveredScreen(scope, { "0123456789" }, cursorColumn, 0), 10, 4));
            Ubiquitty::processBytes(&term, "X");
            const bool isWrapDeferred = cursorColumn == 10;
            CHECK_EQ(characterAt(term, 0, 9), isWrapDeferred ? std::uint32_t { '9' } : std::uint32_t { 'X' });
            CHECK_EQ(characterAt(term, 1, 0) == 'X', isWrapDeferred);
        }
    }
}

namespace {

// A screen's rows at full width, a blank cell read as a space.
std::vector<std::string> fullRows(const Ubiquitty::Terminal& term)
{
    std::vector<std::string> rows;
    for (int row = 0; row < term.rows; ++row) {
        std::string text;
        for (int column = 0; column < term.columns; ++column) {
            const Ubiquitty::Cell* cell = term.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
            const std::uint32_t codePoint = cell ? cell->codePoint : 0;
            text.push_back(codePoint > 0x20 && codePoint < 0x7F ? static_cast<char>(codePoint) : ' ');
        }
        rows.push_back(std::move(text));
    }
    return rows;
}

// Whether each row of a terminal, history first, wraps into the next.
std::vector<bool> wrapMarks(const Ubiquitty::Terminal& term)
{
    std::vector<bool> marks;
    const int rows = Ubiquitty::Screen::scrollbackCount(&term) + term.rows;
    for (int row = 0; row < rows; ++row)
        marks.push_back(Ubiquitty::Screen::absoluteRowWrapped(&term, row));
    return marks;
}

} // namespace

// A recovered screen is the screen the output left. The rows tmux wrapped are
// marked wrapped, whether the recovery leaves them on the screen or scrolls
// them into history, and no other row is: not a row filled to the right edge
// with no wrap, nor one a stale wrap marked before. The cells hold the spaces
// the output wrote. So a copy across a wrap, or across a hard break, gives
// what it gives on the screen the output wrote directly.
TEST(aRecoveryLeavesTheWrapsTheOutputLeft)
{
    struct Case {
        CaptureScope scope;
        std::string output;
        // The rows as tmux 3.8 prints them with -N -T -F, flags taken off.
        std::vector<std::string> rows;
        std::vector<bool> wrapped;
    };
    const Case cases[] = {
        { CaptureScope::CurrentScreen, "abcdef    ghij\r\n0123456789\r\nleft", { "abcdef    ", "ghij", "0123456789", "left" }, { true, false, false, false } },
        { CaptureScope::FullHistory, "aaaaaaaaaabbbbbbbbbbcc\r\n0123456789\r\nabcdef    ghij\r\nleft\r\nright",
            { "aaaaaaaaaa", "bbbbbbbbbb", "cc", "0123456789", "abcdef    ", "ghij", "left", "right" }, { true, true, false, false, true, false, false, false } },
    };
    for (const Case& recovery : cases) {
        const std::string scope = recovery.scope == CaptureScope::FullHistory ? "full history" : "current screen";
        Session session;
        session.applyLayout(0, "ef01,10x4,0,0,7");
        Pane* pane = session.pane(7);
        REQUIRE(pane != nullptr && pane->term() != nullptr);
        Ubiquitty::Terminal& term = *pane->term();
        // A stale wrap on the pane's last row, for the repaint to drop.
        Ubiquitty::processBytes(&term, "\x1b[4;1H0123456789xyz");

        auto live = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*live, 4, 10);
        Ubiquitty::processBytes(live.get(), recovery.output);
        const Ubiquitty::Screen::State& cursor = *live->screens.active;
        const RecoveredScreen screen = recoveredScreen(recovery.scope, recovery.rows, cursor.cursorX + (cursor.wrapPending ? 1 : 0), cursor.cursorY, recovery.wrapped);
        REQUIRE(session.deliverRecovery(7, screen, 10, 4));

        if (recovery.scope == CaptureScope::FullHistory) {
            CHECK_MSG(wrapMarks(term) == wrapMarks(*live), (scope + ": the wrap marks differ from the live screen's").c_str());
            CHECK_MSG(wrapMarks(term) == recovery.wrapped, (scope + ": the wrap marks differ from tmux's").c_str());
        } else {
            for (int row = 0; row < 4; ++row)
                CHECK_MSG(Ubiquitty::Screen::visibleRowWrapped(&term, 0, row) == recovery.wrapped[static_cast<std::size_t>(row)],
                    (scope + ": row " + std::to_string(row) + " wrap").c_str());
        }
        CHECK_MSG(fullRows(term) == fullRows(*live), (scope + ": the rows differ from the live screen's").c_str());
        CHECK_MSG(copiedText(term, 0, 0, 1, 3) == copiedText(*live, 0, 0, 1, 3), (scope + ": a copy across the wrap differs").c_str());
        CHECK_MSG(copiedText(term, 0, 0, 1, 3).find('\n') == std::string::npos, (scope + ": a copy across the wrap broke the line").c_str());
        CHECK_MSG(copiedText(term, 2, 0, 3, 9) == copiedText(*live, 2, 0, 3, 9), (scope + ": a copy across the hard break differs").c_str());
        CHECK_MSG(copiedText(term, 2, 0, 3, 9).find('\n') != std::string::npos, (scope + ": a copy across the hard break joined the lines").c_str());
    }
}

TEST(testWindowLifecycle)
{
    Session session;
    Window* w = session.addWindow(3);
    CHECK(w != nullptr && session.window(3) == w);
    session.renameWindow(3, "build");
    CHECK(w->name() == "build");
    session.setActivePane(3, 42);
    CHECK(w->activePaneID() == 42);
    session.closeWindow(3);
    CHECK(session.window(3) == nullptr);
}

TEST(testPaneSetGeometryBlanksRevealedBandOnGrow)
{
    Pane p(1, 10, 4, Ubiquitty::OSC::SequenceGate {}, Ubiquitty::OSC::OSC52Policy {}, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow);
    Ubiquitty::processBytes(p.term(), "0123456789"); // fills row 0 fully at width 10

    p.setGeometry(0, 0, 5, 4); // shrink: columns 5..9 of row 0 become hidden, not erased
    p.setGeometry(0, 0, 10, 4); // grow back to the original width

    // A bare Core resize would reveal the original '5'..'9' here (that's
    // what the Core resize fix does); the tmux-specific path blanks it instead.
    for (int c = 5; c < 10; c++) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(p.term(), 0, 0, c);
        CHECK(cell && (cell->codePoint == 0 || cell->codePoint == ' '));
    }
    // Columns that were never hidden are untouched.
    for (int c = 0; c < 5; c++) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(p.term(), 0, 0, c);
        CHECK(cell && cell->codePoint == (unsigned char)('0' + c));
    }
}

// A pane that shrinks keeps its cells past the new width, and an erase to the
// edge at that width lowers the row's content bound past them without touching
// them. The grow's blank must not trust that bound, or the band shows a line
// the program cleared: a pane in a resize storm did, until its resync landed.
TEST(testPaneSetGeometryBlanksWhatANarrowEraseLeftPastTheWidth)
{
    const auto isBlank = [](Pane& p, int row, int fromColumn, int toColumn) {
        for (int c = fromColumn; c < toColumn; c++) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(p.term(), 0, row, c);
            if (!cell || (cell->codePoint != 0 && cell->codePoint != ' '))
                return false;
        }
        return true;
    };

    // Erased at the narrow width, then grown back.
    {
        Pane p(3, 10, 4, Ubiquitty::OSC::SequenceGate {}, Ubiquitty::OSC::OSC52Policy {}, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow);
        Ubiquitty::processBytes(p.term(), "0123456789");
        p.setGeometry(0, 0, 5, 4);
        Ubiquitty::processBytes(p.term(), "\r\033[K");
        p.setGeometry(0, 0, 10, 4);
        CHECK_MSG(isBlank(p, 0, 0, 10), "the band kept the line the narrow erase cleared");
    }

    // Erased at the narrow width, hidden by a shorter pane, and revealed again
    // by the height that comes back after the width.
    {
        Pane p(4, 10, 8, Ubiquitty::OSC::SequenceGate {}, Ubiquitty::OSC::OSC52Policy {}, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow);
        for (int r = 0; r < 8; r++) {
            Ubiquitty::processBytes(p.term(), "0123456789");
            if (r + 1 < 8)
                Ubiquitty::processBytes(p.term(), "\r\n");
        }
        p.setGeometry(0, 0, 5, 8);
        Ubiquitty::processBytes(p.term(), "\033[7;1H\033[K");
        p.setGeometry(0, 0, 5, 4);
        p.setGeometry(0, 0, 10, 4);
        p.setGeometry(0, 0, 10, 8);
        CHECK_MSG(isBlank(p, 6, 0, 10), "a revealed row kept the line the narrow erase cleared");
    }
}

TEST(testPaneSetGeometryBlanksRevealedRowsOnGrow)
{
    Pane p(2, 10, 8, Ubiquitty::OSC::SequenceGate {}, Ubiquitty::OSC::OSC52Policy {}, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow);
    for (int r = 0; r < 8; r++) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "R%dxxxxxxx", r);
        Ubiquitty::processBytes(p.term(), std::string_view(buf, static_cast<std::size_t>(10)));
        if (r + 1 < 8)
            Ubiquitty::processBytes(p.term(), "\r\n");
    }

    p.setGeometry(0, 0, 10, 4); // shrink rows 4..7 out of view
    p.setGeometry(0, 0, 10, 8); // grow back to the original height

    for (int r = 4; r < 8; r++) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(p.term(), 0, r, 0);
        CHECK(cell && (cell->codePoint == 0 || cell->codePoint == ' '));
    }
}

TEST(testBuildTreeSingle)
{
    LayoutNode root;
    const char* s = "bf3a,80x24,0,0,7";
    CHECK(parseLayoutTree(s, root));
    Ubiquitty::Panes::Tree tree(leftmostPaneID(root));
    buildPaneTreeInto(tree, root);
    CHECK(tree.paneCount() == 1);
    CHECK(tree.isLeaf(tree.root()));
    CHECK(tree.node(tree.root()).paneID == 7);
}

TEST(testBuildTreeColumns)
{
    LayoutNode root;
    const char* s = "abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}";
    CHECK(parseLayoutTree(s, root));
    Ubiquitty::Panes::Tree tree(leftmostPaneID(root));
    buildPaneTreeInto(tree, root);
    CHECK(tree.paneCount() == 2);
    CHECK(!tree.isLeaf(tree.root()));
    CHECK(tree.node(tree.root()).orientation == Ubiquitty::Panes::Orientation::Vertical);
    std::vector<int> leaves;
    collectTreeLeaves(tree, tree.root(), leaves);
    CHECK(leaves.size() == 2 && leaves[0] == 1 && leaves[1] == 2);
}

TEST(testBuildTreeRows)
{
    LayoutNode root;
    const char* s = "abcd,80x24,0,0[80x12,0,0,1,80x11,0,13,2]";
    CHECK(parseLayoutTree(s, root));
    Ubiquitty::Panes::Tree tree(leftmostPaneID(root));
    buildPaneTreeInto(tree, root);
    CHECK(tree.paneCount() == 2);
    CHECK(tree.node(tree.root()).orientation == Ubiquitty::Panes::Orientation::Horizontal);
}

TEST(testBuildTreeNaryThree)
{
    LayoutNode root;
    // Three columns side by side (tmux flattens same-axis splits into one node).
    const char* s = "abcd,120x24,0,0{40x24,0,0,1,39x24,41,0,2,39x24,81,0,3}";
    CHECK(parseLayoutTree(s, root));
    Ubiquitty::Panes::Tree tree(leftmostPaneID(root));
    buildPaneTreeInto(tree, root);
    CHECK(tree.paneCount() == 3);
    std::vector<int> leaves;
    collectTreeLeaves(tree, tree.root(), leaves);
    CHECK(leaves.size() == 3 && leaves[0] == 1 && leaves[1] == 2 && leaves[2] == 3);
    // Right-nested: root is leaf-1 | (split of 2,3).
    const Ubiquitty::Panes::Node& r = tree.node(tree.root());
    CHECK(tree.isLeaf(r.first) && tree.node(r.first).paneID == 1);
    CHECK(!tree.isLeaf(r.second));
}

TEST(testBuildTreeNested)
{
    LayoutNode root;
    // Columns of [ leaf 1 | rows(2,3) ].
    const char* s = "abcd,80x24,0,0{40x24,0,0,1,39x24,41,0[39x12,41,0,2,39x11,41,13,3]}";
    CHECK(parseLayoutTree(s, root));
    Ubiquitty::Panes::Tree tree(leftmostPaneID(root));
    buildPaneTreeInto(tree, root);
    CHECK(tree.paneCount() == 3);
    std::vector<int> leaves;
    collectTreeLeaves(tree, tree.root(), leaves);
    CHECK(leaves.size() == 3 && leaves[0] == 1 && leaves[1] == 2 && leaves[2] == 3);
    const Ubiquitty::Panes::Node& r = tree.node(tree.root());
    CHECK(r.orientation == Ubiquitty::Panes::Orientation::Vertical);
    CHECK(tree.isLeaf(r.first) && tree.node(r.first).paneID == 1);
    CHECK(!tree.isLeaf(r.second) && tree.node(r.second).orientation == Ubiquitty::Panes::Orientation::Horizontal);
}

TEST(testSessionWindowCapacity)
{
    Session session;
    char layout[64];
    for (int i = 0; i < Ubiquitty::Protocol::Tmux::tmuxMaxSessionWindows + 8; ++i) {
        std::snprintf(layout, sizeof(layout), "ef01,80x24,0,0,%d", i + 1);
        session.applyLayout(i, layout);
    }
    CHECK(static_cast<int>(session.windows().size()) == Ubiquitty::Protocol::Tmux::tmuxMaxSessionWindows);
    // Windows under the cap exist; ones past it were refused.
    CHECK(session.window(0) != nullptr);
    CHECK(session.window(Ubiquitty::Protocol::Tmux::tmuxMaxSessionWindows + 5) == nullptr);
    // An EXISTING window's layout still applies at the cap (refresh, not add).
    session.applyLayout(0, "ef01,90x30,0,0,1");
    CHECK(session.window(0)->pane(1) != nullptr);
    CHECK(session.window(0)->pane(1)->columns() == 90);
}

TEST(testMovingPaneBetweenWindowsRetainsTerminalInEitherEventOrder)
{
    for (bool isSourceFirst : { false, true }) { // bool-ok: Iterates both event orders; this is not a boolean parameter.
        Session session;
        session.applyLayout(0, "abcd,80x24,0,0{40x24,0,0,7,39x24,41,0,8}");
        session.applyLayout(1, "abcd,80x24,0,0,9");
        auto* pane = session.window(0)->pane(7);
        CHECK(pane != nullptr);
        const auto* terminal = pane->term();
        if (isSourceFirst) {
            session.applyLayout(0, "abcd,80x24,0,0,8");
        }
        session.applyLayout(1, "abcd,80x24,0,0{40x24,0,0,9,39x24,41,0,7}");
        if (!isSourceFirst) {
            session.applyLayout(0, "abcd,80x24,0,0,8");
        }
        CHECK(session.window(1)->pane(7) == pane);
        CHECK(session.window(1)->pane(7)->term() == terminal);
        session.closeWindow(0);
        CHECK(session.window(1)->pane(7) == pane);
        CHECK(!session.disposeRetiredPane(7));
    }
}

UBIQUITTY_TEST_MAIN()
