// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The mode flags, how they are set, and what a reset does to them.
//
// DEC private modes and ANSI modes, the DECRQM reports that read them back,
// the alternate screen and the save-restore pair around it, and the three
// resets — RIS, DECSTR and the DECSCL level change — that each restore a
// different subset.
//
// The resets are the reason these sit together: a mode's definition is
// incomplete without saying which reset clears it, and that is a property of the
// mode rather than of the reset. Every case that sets a mode also asks what
// survives.

#include "Harness.h"
#include "VT100Fixture.h"

#include "Core/Mouse/Hilite.h"
#include "Core/Mouse/Mouse.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/Runtime/ModeSnapshot.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/Width.h"

/// ---- Linux-console power / display management (CSI 9/13/14 ]) ----

namespace {
struct DisplayPowerClient : Ubiquitty::WindowClient {
    int calls = 0;
    Ubiquitty::DisplayPowerState lastState = Ubiquitty::DisplayPowerState::Active;
    void setDisplayPower(Ubiquitty::DisplayPowerState s) override
    {
        calls++;
        lastState = s;
    }
};
}

/// ---- Password input mode (heuristic + private mode 7401) ----

namespace {
struct PasswordClient : Ubiquitty::WindowClient {
    int calls = 0;
    bool isLastActive = false;
    void setPasswordPromptActive(bool isActive) override // bool-ok: the frontend seam's own signature
    {
        calls++;
        isLastActive = isActive;
    }
};
}

static int risTmuxEnterCalls = 0;

static Ubiquitty::Terminal* risPaneLookup(void*, int) { return nullptr; }

static void risPaneFlush(void*) { }

TEST(testRISPreservesLiveGridSize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 40, 100);
    feed(t, "hello\033[6;12r\033[11;20H");

    feed(t, "\033c");

    CHECK(t.columns == 100);
    CHECK(t.rows == 40);
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == 39);
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    const Ubiquitty::Cell* c = cell(t, 0, 0);
    CHECK(c && (c->codePoint == ' ' || c->codePoint == 0));
}

TEST(testDECCOLMSwitchesTo132)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?40hABC");
    int origColumns = t.columns;
    feed(t, "\033[?3h");
    CHECK(t.columns == 132);
    CHECK(t.modes.columns132 == true);
    // Screen cleared.
    CHECK(cell(t, 0, 0)->codePoint == 0);
    // Cursor home.
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    // Scroll region reset.
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == t.rows - 1);

    feed(t, "\033[?3l");
    CHECK(t.columns == 80);
    CHECK(t.modes.columns132 == false);
    (void)origColumns;
}

// Mode 40 starts reset, as xterm's c132 resource does, and DECCOLM is ignored
// until a program sets it: an xterm terminfo entry's rs2 sends CSI ? 3 l, so
// `tput reset` would otherwise narrow and clear the window.
TEST(testDECCOLMIsIgnoredUntilMode40AllowsIt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];
    feed(t, "ABC\033[2;3H\033[?3h\033[?3$p\033[?40$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?3;2$y\033[?40;2$y") == 0);
    CHECK(t.columns == 80);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 2);
}

TEST(testDECNCSMKeepsScreenOnDECCOLM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Default (DECNCSM reset): DECCOLM clears.
    feed(t, "\033[?40hAB");
    feed(t, "\033[?3h");
    CHECK(cell(t, 0, 0)->codePoint == 0);

    // DECNCSM set: DECCOLM keeps the contents.
    feed(t, "\033[?95h");
    feed(t, "\033[1;1HCD");
    feed(t, "\033[?3l"); // 132 -> 80, no clear
    CHECK(t.columns == 80);
    CHECK(cell(t, 0, 0)->codePoint == 'C');
    CHECK(cell(t, 0, 1)->codePoint == 'D');
}

TEST(testDECCOLMResetsHorizontalMargins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?40h\033[?69h\033[10;60s"); // allow 80<->132, DECLRMM, margins
    CHECK(t.screens.active->scrollLeft == 9);
    CHECK(t.modes.leftRightMarginMode);
    feed(t, "\033[?3h"); // DECCOLM → 132 columns
    CHECK(t.screens.active->scrollLeft == 0);
    CHECK(t.screens.active->scrollRight == t.columns - 1);
    CHECK(!t.modes.leftRightMarginMode);
}

TEST(testAlternateScreenLineattrIndependent)
{
    /// Each Screen has its own lineAttribute — primary's DWL must not
    /// leak into the alternate screen.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;1H\033#6");
    CHECK(t.screens.primary.lineAttribute[0] == Ubiquitty::lineAttributeDoubleWidth);
    feed(t, "\033[?47h"); // switch to alternate screen
    CHECK(t.screens.alternate.lineAttribute[0] == Ubiquitty::lineAttributeNormal);
    feed(t, "\033[?47l"); // back to primary
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeDoubleWidth);
}

TEST(testAlternateScreenFeatureGate1046)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);

    // Default: feature enabled; DECRQM ?1046 reports set (1).
    CHECK(t.modes.allowAlternateScreen == true);
    feed(t, "\033[?1046$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1046;1$y") == 0);

    // DECRST 1046 disables the feature; DECRQM now reports reset (2).
    feed(t, "\033[?1046l");
    CHECK(t.modes.allowAlternateScreen == false);
    feed(t, "\033[?1046$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1046;2$y") == 0);

    // With the feature off, ?1049 h does not switch to the alternate screen.
    feed(t, "\033[?1049h");
    CHECK(t.screens.isUsingAlternateScreen == false);
    // Likewise the legacy 47 / 1047 modes are gated off.
    feed(t, "\033[?47h");
    CHECK(t.screens.isUsingAlternateScreen == false);
    feed(t, "\033[?1047h");
    CHECK(t.screens.isUsingAlternateScreen == false);

    // Re-enable the feature (DECSET 1046) → round-trips back to set.
    feed(t, "\033[?1046h");
    CHECK(t.modes.allowAlternateScreen == true);
    feed(t, "\033[?1046$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1046;1$y") == 0);

    // Now ?1049 h switches to the alternate screen as usual.
    feed(t, "\033[?1049h");
    CHECK(t.screens.isUsingAlternateScreen == true);

    // DECRST 1046 while ON the alternate screen forces back to primary.
    feed(t, "\033[?1046l");
    CHECK(t.modes.allowAlternateScreen == false);
    CHECK(t.screens.isUsingAlternateScreen == false);
}

TEST(testMetaAlternateKeyboardModes)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);

    // Power-on defaults: metaSendsEscape (1036) ON; alternateSendsEscape (1039) and
    // eightBitInput (1034) OFF. zero-initialization would make 1036 off, so construction
    // path must set it explicitly — verify both the field and DECRQM.
    CHECK(t.modes.metaSendsEscape == true);
    CHECK(t.modes.alternateSendsEscape == false);
    CHECK(t.modes.eightBitInput == false);
    feed(t, "\033[?1036$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1036;1$y") == 0);
    feed(t, "\033[?1039$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1039;2$y") == 0);
    feed(t, "\033[?1034$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1034;2$y") == 0);

    // DECRST 1036 → field clears, DECRQM reports reset (2).
    feed(t, "\033[?1036l");
    CHECK(t.modes.metaSendsEscape == false);
    feed(t, "\033[?1036$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1036;2$y") == 0);

    // DECSET 1036 → back to set (1).
    feed(t, "\033[?1036h");
    CHECK(t.modes.metaSendsEscape == true);
    feed(t, "\033[?1036$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1036;1$y") == 0);

    // DECSET 1034 (eightBitInput) → set; DECRQM reports set (1).
    feed(t, "\033[?1034h");
    CHECK(t.modes.eightBitInput == true);
    feed(t, "\033[?1034$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1034;1$y") == 0);
    feed(t, "\033[?1034l");
    CHECK(t.modes.eightBitInput == false);

    // DECSET 1039 (alternateSendsEscape) → set; DECRQM reports set (1).
    feed(t, "\033[?1039h");
    CHECK(t.modes.alternateSendsEscape == true);
    feed(t, "\033[?1039$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1039;1$y") == 0);
    feed(t, "\033[?1039l");
    CHECK(t.modes.alternateSendsEscape == false);

    // Soft reset (DECSTR) leaves these keyboard-discipline modes untouched —
    // they are not in the DECSTR list (xterm parity).
    feed(t, "\033[?1036l"); // turn 1036 off
    feed(t, "\033[?1034h"); // turn 1034 on
    feed(t, "\033[!p"); // DECSTR
    CHECK(t.modes.metaSendsEscape == false);
    CHECK(t.modes.eightBitInput == true);
}

TEST(testFontShiftingMode35)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);

    // Power-on default: reset (2).
    CHECK(t.modes.fontShifting == false);
    feed(t, "\033[?35$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?35;2$y") == 0);

    // DECSET 35 → set; DECRQM reports set (1).
    feed(t, "\033[?35h");
    CHECK(t.modes.fontShifting == true);
    feed(t, "\033[?35$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?35;1$y") == 0);

    // DECRST 35 → back to reset (2).
    feed(t, "\033[?35l");
    CHECK(t.modes.fontShifting == false);
    feed(t, "\033[?35$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?35;2$y") == 0);
}

TEST(testKeepClipboardMode1044)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);

    // Power-on default: reset (2).
    CHECK(t.modes.keepClipboard == false);
    feed(t, "\033[?1044$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1044;2$y") == 0);

    // DECSET 1044 → set; DECRQM reports set (1).
    feed(t, "\033[?1044h");
    CHECK(t.modes.keepClipboard == true);
    feed(t, "\033[?1044$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1044;1$y") == 0);

    // DECRST 1044 → back to reset (2).
    feed(t, "\033[?1044l");
    CHECK(t.modes.keepClipboard == false);
    feed(t, "\033[?1044$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1044;2$y") == 0);
}

TEST(testCursorBlinkMode12)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    // A sane, focused, no-activity baseline so resolveCursor's blink phase is
    // the only variable.
    t.windowFocused = true;
    t.lastActivityTick = 0; // freeze policy is inert when this is 0
    t.cursorStyle = Ubiquitty::Cursor::Style::FollowUserPreference; // no DECSCUSR — the default-shape path

    // Power-on default: reset (2) — no program has asked for a blink.
    CHECK(t.cursorBlinkAtt610 == false);
    feed(t, "\033[?12$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?12;2$y") == 0);

    // With the reader's blink on, the cursor blinks: the blink alpha dips below
    // 1 somewhere across a cycle.
    t.cursorBlinkPref = true;
    feed(t, "\033[?12h");
    bool sawDip = false;
    for (uint32_t tick = 0; tick < 240; ++tick)
        if (Ubiquitty::Cursor::resolve(&t, tick).blinkAlpha < 1.0f) {
            sawDip = true;
            break;
        }
    CHECK(sawDip);

    // DECRST 12 (what `cnorm` emits) is recorded — DECRQM reports reset (2) —
    // and does not steady the reader's blink: an unset `[cursor] blink` is its
    // default, so no reader has a vacancy a program's cnorm may fill.
    feed(t, "\033[?12l");
    CHECK(t.cursorBlinkAtt610 == false);
    feed(t, "\033[?12$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?12;2$y") == 0);
    sawDip = false;
    for (uint32_t tick = 0; tick < 240; ++tick)
        if (Ubiquitty::Cursor::resolve(&t, tick).blinkAlpha < 1.0f) {
            sawDip = true;
            break;
        }
    CHECK(sawDip); // still blinking — the reader's blink governs, not mode 12
    bool isEverySampleSolid = true;

    // And an explicit pref of "no blink" stays steady regardless of mode 12.
    t.cursorBlinkPref = false;
    feed(t, "\033[?12h"); // app asks for blink
    isEverySampleSolid = true;
    for (uint32_t tick = 0; tick < 240; ++tick)
        if (Ubiquitty::Cursor::resolve(&t, tick).blinkAlpha < 1.0f) {
            isEverySampleSolid = false;
            break;
        }
    CHECK(isEverySampleSolid);
}

// Mode 12 and DECSCUSR set one flag: a blinking style sets it, a steady one
// resets it, and DECSTR and RIS reset it, so DECRQM 12 tells a program such as
// vim which blink it chose last.
TEST(testCursorBlinkMode12FollowsDECSCUSRAndResets)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[1 q\033[?12$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?12;1$y") == 0);
    feed(t, "\033[2 q\033[?12$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?12;2$y") == 0);
    feed(t, "\033[?12h\033[!p\033[?12$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?12;2$y") == 0);
    feed(t, "\033[?12h\033c\033[?12$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?12;2$y") == 0);
}

// Keyboard-type modes are mutually exclusive and reset to the default table.
TEST(testFunctionKeyFlavorModes10501061)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);

    const int modes[] = { 1050, 1051, 1052, 1053, 1060, 1061 };
    for (int mode : modes) {
        char query[16], expect[24];
        snprintf(query, sizeof(query), "\033[?%d$p", mode);
        snprintf(expect, sizeof(expect), "\033[?%d;2$y", mode);

        feed(t, query);
        drain(t, outputBuffer);
        CHECK(strcmp(outputBuffer, expect) == 0);

        char setCommand[16];
        snprintf(setCommand, sizeof(setCommand), "\033[?%dh", mode);
        feed(t, setCommand);
        snprintf(expect, sizeof(expect), "\033[?%d;1$y", mode);
        feed(t, query);
        drain(t, outputBuffer);
        CHECK(strcmp(outputBuffer, expect) == 0);

        char resetCommand[16];
        snprintf(resetCommand, sizeof(resetCommand), "\033[?%dl", mode);
        feed(t, resetCommand);
        snprintf(expect, sizeof(expect), "\033[?%d;2$y", mode);
        feed(t, query);
        drain(t, outputBuffer);
        CHECK(strcmp(outputBuffer, expect) == 0);
    }
}

TEST(testNewAllowGatesDefaultToAllowed)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.allowTerminalCapabilityOperations);
    CHECK(t.allowMouseOperations);
    CHECK(t.allowPasteControls);
}

TEST(testDECSTRPreservesTabStops)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Wipe the default grid, set a single custom stop at column 5 via HTS.
    feed(t, "\033[3g");
    feed(t, "\033[6G\033H"); // column 6 (CHA, 1-based) -> stop at index 5
    CHECK(t.tabStops[5]);
    CHECK(!t.tabStops[8]); // the default grid is gone
    // Soft reset must leave the custom map intact.
    feed(t, "\033[!p");
    CHECK(t.tabStops[5]); // custom stop survives DECSTR
    CHECK(!t.tabStops[8]); // the every-8 default was not re-established
    CHECK(!t.tabStops[16]);
    // A tab from column 0 lands on the app's stop, not a default one.
    feed(t, "\033[1G\t");
    CHECK(t.screens.active->cursorX == 5);
}

TEST(testLNMLinefeedSemantics)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Default: bare LF leaves the column unchanged.
    feed(t, "ABC\nD");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(cell(t, 1, 3)->codePoint == 'D'); // dropped on column 3, no CR effect
    CHECK(!t.modes.lnm);

    Ubiquitty::hardReset(t);
    // Enable LNM. LF now also resets the column.
    feed(t, "\033[20h");
    CHECK(t.modes.lnm);
    feed(t, "ABC\nE");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(cell(t, 1, 0)->codePoint == 'E');
    CHECK(cell(t, 1, 3)->codePoint != 'E');

    // Disable LNM — back to plain LF.
    feed(t, "\033[20l");
    CHECK(!t.modes.lnm);
}

TEST(testGroundLfFastPathLNMCursorReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033[20h");
    CHECK(t.modes.lnm);
    feed(t, "ABC\n");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 0);

    feed(t, "Z");
    CHECK(cell(t, 1, 0)->codePoint == 'Z');
    CHECK(cell(t, 1, 3)->codePoint != 'Z');
}

TEST(testDECSCNMReverseVideo)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(!Ubiquitty::isReverseVideo(&t));

    feed(t, "hello");
    drainPaintCycle(t, nullptr);
    feed(t, "\033[?5h");
    CHECK(t.modes.reverseVideo);
    CHECK(Ubiquitty::isReverseVideo(&t));
    // Toggle requests a full redraw so every cell repaints with
    // swapped foreground/background.
    CHECK(t.grid->isFullRedrawRequested());

    drainPaintCycle(t, nullptr);
    feed(t, "\033[?5l");
    CHECK(!t.modes.reverseVideo);
    CHECK(!Ubiquitty::isReverseVideo(&t));
    CHECK(t.grid->isFullRedrawRequested());
}

TEST(testDecSynchronizedOutputMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Enabling the mode does not swallow subsequent host bytes — the
    // cell at the cursor still receives 'X'.
    feed(t, "\033[?2026h");
    CHECK(t.modes.syncOutput == true);
    CHECK(Ubiquitty::Modes::SyncOutput::isActive(&t) == true);
    feed(t, "X");
    CHECK(cell(t, 0, 0)->codePoint == 'X');

    // Watchdog accounting: zero before the PAL touches, advances after.
    CHECK(Ubiquitty::Modes::SyncOutput::elapsedTicks(&t, Ubiquitty::Ticks(100)) == 0);
    Ubiquitty::Modes::SyncOutput::touch(&t, Ubiquitty::Ticks(100));
    CHECK(Ubiquitty::Modes::SyncOutput::elapsedTicks(&t, Ubiquitty::Ticks(100)) == 0);
    CHECK(Ubiquitty::Modes::SyncOutput::elapsedTicks(&t, Ubiquitty::Ticks(165)) == 65);

    // Disabling clears the flag (full-redraw signalling lives on CellGrid, not
    // on an Ubiquitty::Terminal field).
    feed(t, "\033[?2026l");
    CHECK(t.modes.syncOutput == false);
    CHECK(Ubiquitty::Modes::SyncOutput::elapsedTicks(&t, Ubiquitty::Ticks(200)) == 0);

    // Expire path — force-clear after a watchdog timeout.
    feed(t, "\033[?2026h");
    Ubiquitty::Modes::SyncOutput::touch(&t, Ubiquitty::Ticks(1000));
    CHECK(Ubiquitty::Modes::SyncOutput::isActive(&t) == true);
    Ubiquitty::Modes::SyncOutput::expire(&t);
    CHECK(Ubiquitty::Modes::SyncOutput::isActive(&t) == false);
}

TEST(testTheVacatedNumbersAreNobodysModes)
{
    // 2500 and 2501 are VTE's (mirrored box drawing, BiDi autodetection) and
    // 2502 sits beside them; none is a mode this terminal has, so a program
    // setting them changes nothing here and DECRQM answers 0 for each.
    Ubiquitty::Terminal t;
    char reply[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2500h\033[?2501h\033[?2502h");
    CHECK(!t.passwordPromptActive);
    CHECK(!t.modes.fullscreenContent);
    CHECK(!t.modes.mouseRelativePointerLock);
    for (const char* probe : { "\033[?2500$p", "\033[?2501$p", "\033[?2502$p" }) {
        feed(t, probe);
        drain(t, reply);
        CHECK(strstr(reply, ";0$y") != nullptr);
    }
}

TEST(testDecFullscreenContentMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.fullscreenContent == false);

    // Enable: the modes bit flips on (the null test client makes the seam a no-op).
    feed(t, "\033[?7402h");
    CHECK(t.modes.fullscreenContent == true);

    // DECRQM (CSI ? 7402 $ p) reports "set" (1).
    char reply[32];
    feed(t, "\033[?7402$p");
    drain(t, reply);
    CHECK(strcmp(reply, "\033[?7402;1$y") == 0);

    // A soft reset (DECSTR) PRESERVES it — 2048/7401 parity.
    feed(t, "\033[!p");
    CHECK(t.modes.fullscreenContent == true);

    // Disable: the bit flips off; DECRQM reports "reset" (2).
    feed(t, "\033[?7402l");
    CHECK(t.modes.fullscreenContent == false);
    feed(t, "\033[?7402$p");
    drain(t, reply);
    CHECK(strcmp(reply, "\033[?7402;2$y") == 0);
}

TEST(testDECSTBMOnAlternateScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);

    // Primary: full-screen region by default.
    CHECK(t.screens.primary.scrollTop == 0);
    CHECK(t.screens.primary.scrollBottom == 23);

    // Enter alternate screen (mode 47 — no cursor save).
    feed(t, "\033[?47h");
    CHECK(t.screens.isUsingAlternateScreen);
    CHECK(t.screens.active == &t.screens.alternate);
    // Set a region on the alternate. DECSTBM is 1-based.
    feed(t, "\033[3;10r");
    CHECK(t.screens.alternate.scrollTop == 2);
    CHECK(t.screens.alternate.scrollBottom == 9);

    // Primary's region must not have been mutated by the alternate-screen
    // DECSTBM.
    CHECK(t.screens.primary.scrollTop == 0);
    CHECK(t.screens.primary.scrollBottom == 23);

    // Drive the cursor to the alternate bottom row and write past it — scroll
    // must stay within [3,10] (rows 2..9 zero-indexed).
    feed(t, "\033[10;1H"); // CUP to row 10 (1-based)
    CHECK(t.screens.active->cursorY == 9);
    feed(t, "BOTTOM\n");
    // After LF at the bottom of the region, cursor stays on row 9.
    CHECK(t.screens.active->cursorY == 9);
    // Rows above the region (0..1) must be untouched.
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(cell(t, 1, 0)->codePoint == 0);

    // Exit alternate screen.
    feed(t, "\033[?47l");
    CHECK(!t.screens.isUsingAlternateScreen);
    CHECK(t.screens.active == &t.screens.primary);
    // Primary's region is unchanged.
    CHECK(t.screens.primary.scrollTop == 0);
    CHECK(t.screens.primary.scrollBottom == 23);
    // Alternate's region survives across switch (it's stored on the
    // Ubiquitty::Screen::State struct, not torn down).
    CHECK(t.screens.alternate.scrollTop == 2);
    CHECK(t.screens.alternate.scrollBottom == 9);
}

TEST(testDECOMRelativeToDECSTBM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);

    // DECSTBM 5..15 (1-based) → scrollTop=4, scrollBottom=14.
    feed(t, "\033[5;15r");
    CHECK(t.screens.active->scrollTop == 4);
    CHECK(t.screens.active->scrollBottom == 14);

    // DECOM on.
    feed(t, "\033[?6h");
    CHECK(t.modes.originMode);

    // CUP 1;1 with origin mode → row 4, column 0 in absolute terms.
    feed(t, "\033[1;1HA");
    CHECK(cell(t, 4, 0)->codePoint == 'A');

    // CUP past the region's bottom must clamp to scrollBottom.
    feed(t, "\033[20;1HB");
    // Origin-relative row 20 maps to absolute 23; the cursor clamps
    // inside the active region [4,14], so the cell lands on row 14.
    CHECK(cell(t, 14, 0)->codePoint == 'B');

    // Write past the bottom — the [4,14] region must scroll, not the
    // full screen. The text above (row 0..3) stays put.
    feed(t, "\033[15;1H"); // origin-relative row 15 → clamped to row 14
    feed(t, "LINE\n");
    // Rows 0..3 (above region) untouched.
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(cell(t, 3, 0)->codePoint == 0);

    // Disable DECOM — CUP returns to absolute coordinates.
    feed(t, "\033[?6l");
    CHECK(!t.modes.originMode);
    feed(t, "\033[1;1HZ");
    CHECK(cell(t, 0, 0)->codePoint == 'Z');
}

TEST(testNELRespectsDECSTBMAndLNM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[4;8r");
    // LNM on (would only matter for LF).
    feed(t, "\033[20h");
    CHECK(t.modes.lnm);

    // CUP to (row 8, column 6) 1-based → (7, 5).
    feed(t, "\033[8;6H");
    CHECK(t.screens.active->cursorX == 5);
    CHECK(t.screens.active->cursorY == 7);

    // NEL: scroll region, cursor to column 0 on same row (region bottom).
    feed(t, "\033E");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 7);
}

TEST(testAlternateScreenDiffDirtiesOnlyDifferingCells)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // Write "ABC" at row 12 column 5; sync shadow.
    feed(t, "\033[12;5HABC");
    drainPaintCycle(t, nullptr);
    // Toggle to alternate screen (1049h). Alternate is blank, so the cells
    // holding "ABC" differ from the BB shadow, which still carries the
    // primary's last-painted state.
    feed(t, "\033[?1049h");
    peekPaintTrace(t, &tr);
    CHECK(tr.isRowYielded[11]);
    CHECK(tr.isCellYielded[11][4]);
    CHECK(tr.isCellYielded[11][5]);
    CHECK(tr.isCellYielded[11][6]);
    // Unaffected rows stay clean.
    CHECK(!tr.isRowYielded[0]);
    CHECK(!tr.isRowYielded[5]);
    CHECK(!tr.isRowYielded[23]);
}

TEST(testAlternateScreenDiffNoDirtyForIdenticalScreens)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    drainPaintCycle(t, nullptr);
    // Both screens blank, shadow already matches. 1049h toggles
    // active to alternate (also blank). No cells differ from shadow.
    feed(t, "\033[?1049h");
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
}

TEST(testAlternateScreenDiffSparseDoesNotDirtyInterior)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // Write sparse content via setCell-equivalent operations on
    // row 7 (columns 3 and 60), sync shadow.
    feed(t, "\033[8;4HX"); // row 7 column 3 (1-indexed)
    feed(t, "\033[8;61HY"); // row 7 column 60
    drainPaintCycle(t, nullptr);
    // Toggle alternate; cells 3 and 60 of row 7 now differ from shadow
    // (alternate is blank there), while the blank interior stays in
    // sync.
    feed(t, "\033[?1049h");
    peekPaintTrace(t, &tr);
    CHECK(tr.isRowYielded[7]);
    CHECK(tr.isCellYielded[7][3]);
    CHECK(tr.isCellYielded[7][60]);
    for (c = 4; c <= 59; c++)
        CHECK(!tr.isCellYielded[7][c]);
}

TEST(testModeLineRewriteNoLeak)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[23;1H(Lisp Interaction ElDoc)");
    drainPaintCycle(t, nullptr);
    // Mode-line update: CUP, shorter text, EL to clear the rest.
    feed(t, "\033[23;1H(Fundamental ElDoc)\033[K");
    drainPaintCycle(t, nullptr);
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    // Cell content checks: new text in place, old tail cleared.
    CHECK(cell(t, 22, 0)->codePoint == '(');
    CHECK(cell(t, 22, 1)->codePoint == 'F');
    CHECK(cell(t, 22, 11)->codePoint == 'l');
    CHECK(cell(t, 22, 12)->codePoint == ' ');
    CHECK(cell(t, 22, 13)->codePoint == 'E');
    CHECK(cell(t, 22, 18)->codePoint == ')');
    for (c = 19; c < 80; c++)
        CHECK(cell(t, 22, c)->codePoint == 0);
}

TEST(testModeLineRewriteWithTrailingSpaces)
{
    Ubiquitty::Terminal t;
    PaintTrace tr;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[23;1H(Lisp Interaction ElDoc)");
    drainPaintCycle(t, nullptr);
    // Overwrite the original 24 columns with new content padded with
    // trailing spaces.
    feed(t, "\033[23;1H(Fundamental ElDoc)     ");
    drainPaintCycle(t, nullptr);
    peekPaintTrace(t, &tr);
    CHECK(tr.runs == 0);
    CHECK(cell(t, 22, 1)->codePoint == 'F');
    CHECK(cell(t, 22, 11)->codePoint == 'l');
    CHECK(cell(t, 22, 12)->codePoint == ' ');
    CHECK(cell(t, 22, 18)->codePoint == ')');
    for (c = 19; c < 24; c++)
        CHECK(cell(t, 22, c)->codePoint == ' ');
}

TEST(testEmacsIncrementalModeLineDiff)
{
    Ubiquitty::Terminal t;
    int c;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[23;1H(Lisp Interaction ElDoc)");
    drainPaintCycle(t, nullptr);
    feed(t, "\033[23;1H(Fundamental ElDoc)\033[K");
    drainPaintCycle(t, nullptr);
    // No "Interaction" letters survive past the new text.
    for (c = 19; c < 80; c++) {
        uint16_t ch = cell(t, 22, c)->codePoint;
        if (ch == 't' || ch == 'i' || ch == 'o' || ch == 'n' || ch == 'r' || ch == 'a' || ch == 'c')
            CHECK(0 /* stale letter leaked past mode-line EL */);
    }
}

TEST(testDECAWMDefaultOn)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Fresh Terminal: autowrap is on by default per the VT510 reset list.
    CHECK(t.modes.autowrap == true);

    /// Default geometry is 80×24. Feed 81 'A's: 80 fill row 0 columns 0..79,
    /// the 81st triggers the deferred-wrap path and lands on row 1 column 0.
    int total = t.columns + 1;
    for (int i = 0; i < total; i++)
        feed(t, "A");

    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, t.columns - 1)->codePoint == 'A');
    CHECK(cell(t, 1, 0)->codePoint == 'A');
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 1);
}

TEST(testDECAWMOffOverwritesLastColumn)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Clear autowrap; the cursor parks at columns-1 and successive chars
    // overwrite that cell rather than wrapping.
    feed(t, "\033[?7l");
    CHECK(t.modes.autowrap == false);

    // Fill row 0 with 'A's, then overrun by writing 'B's.
    for (int i = 0; i < t.columns; i++)
        feed(t, "A");
    // Cursor should be at the last column (wrap-pending was suppressed).
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == t.columns - 1);

    feed(t, "B");
    feed(t, "C");
    // Row 1 must remain blank; the last cell of row 0 is overwritten.
    CHECK(cell(t, 1, 0)->codePoint == 0 || cell(t, 1, 0)->codePoint == ' ');
    CHECK(cell(t, 0, t.columns - 1)->codePoint == 'C');
    // Earlier columns still hold 'A'.
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, t.columns - 2)->codePoint == 'A');
}

TEST(testDECAWMRoundTrip)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?7h");
    CHECK(t.modes.autowrap == true);
    feed(t, "\033[?7l");
    CHECK(t.modes.autowrap == false);
    feed(t, "\033[?7h");
    CHECK(t.modes.autowrap == true);
}

TEST(testDECCKMDefaultOff)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.appCursorKeys == false);
}

TEST(testDECPAMSetsKeypadApp)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.appKeypad == false);
    feed(t, "\033=");
    CHECK(t.modes.appKeypad == true);
}

TEST(testDECSCUSRDefaultBlock)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Fresh Terminal: cursorStyle is zero-initialised, so rendering follows the
    // user's normal cursor shape/blink preferences.
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::FollowUserPreference);
}

TEST(testDECSCUSRStyles1Through6)
{
    // Ps 1..6 land verbatim in cursorStyle (1/2 block, 3/4 underline, 5/6 bar).
    for (int ps = 1; ps <= 6; ps++) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "\033[%d q", ps);
        feed(t, buffer);
        CHECK(static_cast<uint8_t>(t.cursorStyle) == ps);
    }
}

TEST(testDECSCUSRZeroRestoresUserCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.cursorShapePref = 2; // user prefers a bar
    t.cursorBlinkPref = false;
    feed(t, "\033[0 q");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::BlinkingBlock);
    feed(t, "\033[7 q");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::FollowUserPreference);
    CHECK(!t.cursorBlinkAtt610);
    const auto state = Ubiquitty::Cursor::resolve(&t, 100);
    CHECK(state.shape == Ubiquitty::Cursor::Shape::Bar);
}

TEST(testDECSCUSROutOfRange)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4 q\033[99 q");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::SteadyUnderline);
}

TEST(testDECSCUSRNoParameterRestoresUserCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[5 q");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::BlinkingBar);
    // The omitted parameter selects the standard blinking block.
    feed(t, "\033[ q");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::BlinkingBlock);
}

TEST(testDECSCUSRResetRestoresUserCursor)
{
    /// The reset triggers (RIS / DECSTR) restore the user's configured cursor too,
    /// alongside DECSCUSR-7: all three clear cursorStyle to the sentinel so the
    /// cursor follows the user's shape/blink preference again.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.cursorShapePref = 2; // bar
    t.cursorBlinkPref = true;
    feed(t, "\033[4 q"); // steady underline
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::SteadyUnderline);
    // DECSTR (CSI ! p) → user cursor.
    feed(t, "\033[!p");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::FollowUserPreference);
    Ubiquitty::Cursor::RenderState state = Ubiquitty::Cursor::resolve(&t, 100);
    CHECK(state.shape == Ubiquitty::Cursor::Shape::Bar);

    // RIS (ESC c) does the same via a full Ubiquitty::Terminal reconstruct.
    feed(t, "\033[6 q");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::SteadyBar);
    feed(t, "\033c");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::FollowUserPreference);
}

TEST(testBracketedPasteDefaultOff)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.bracketedPaste == false);
}

TEST(testBracketedPasteSetAndReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2004h");
    CHECK(t.modes.bracketedPaste == true);
    feed(t, "\033[?2004l");
    CHECK(t.modes.bracketedPaste == false);
    // Round-trip a second time.
    feed(t, "\033[?2004h");
    CHECK(t.modes.bracketedPaste == true);
}

TEST(testMode1048SaveRestoreCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[5;7H"); // move to row 5 column 7 (1-based)
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 6);
    feed(t, "\033[?1048h"); // save
    feed(t, "\033[1;1H"); // move to home
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\033[?1048l"); // restore
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 6);
}

TEST(testDECSTRSoftReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "hello");
    // Make a bunch of state non-default.
    feed(t, "\033[?6h"); // DECOM (origin mode)
    feed(t, "\033[?7l"); // DECAWM off
    feed(t, "\033[1;31m"); // bold red SGR
    feed(t, "\033[5;15r"); // DECSTBM scroll region
    feed(t, "\033[?2026h"); // synchronized output
    CHECK(t.modes.originMode == true);
    CHECK(t.modes.autowrap == false);
    CHECK(t.modes.syncOutput == true);
    CHECK(t.screens.active->scrollTop == 4);
    CHECK(t.screens.active->scrollBottom == 14);

    // Soft reset.
    feed(t, "\033[!p");

    CHECK(t.modes.originMode == false);
    CHECK(t.modes.autowrap == true);
    CHECK(t.modes.isCursorVisible == true);
    CHECK(t.modes.syncOutput == false);
    // Scroll region back to full screen.
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == t.rows - 1);
    // SGR pen reset.
    CHECK(!t.screens.active->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(t.screens.active->pen.foregroundIndex == Ubiquitty::colorDefault);
    // Screen content preserved.
    CHECK(cell(t, 0, 0)->codePoint == 'h');
    CHECK(cell(t, 0, 4)->codePoint == 'o');
}

TEST(testDECSTRFullResetContract)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2$~"); // configure the host-writable status display
    // At the default VT500 level (DECSLRM is VT400+ gated).
    feed(t, "\033[?69h"); // DECLRMM on
    feed(t, "\033[10;60s"); // DECSLRM: narrow left/right margins
    CHECK(t.modes.leftRightMarginMode == true);
    CHECK(t.screens.active->scrollLeft == 9);
    CHECK(t.screens.active->scrollRight == 59);
    feed(t, "\033[2$~\033[1$}"); // DECSASD: status line receives writes
    CHECK(t.statusLineActive == 1);
    feed(t, "\033[?1001h"); // hilite mouse tracking
    CHECK(t.modes.mouseHilite == true);
    t.modes.hiliteAwaiting = true; // a pending XTHIMOUSE handshake
    t.modes.hiliteRegionValid = true;

    feed(t, "\033[!p"); // DECSTR

    CHECK(t.modes.leftRightMarginMode == false);
    CHECK(t.screens.active->scrollLeft == 0);
    CHECK(t.screens.active->scrollRight == t.columns - 1);
    CHECK(t.statusLineActive == 0);
    CHECK(t.modes.mouseHilite == true); // tracking is the program's, as in xterm
    CHECK(t.modes.hiliteAwaiting == false);
    CHECK(t.modes.hiliteRegionValid == false);

    // The operating level survives a DECSTR; only RIS restores the default.
    feed(t, "\033[63\"p"); // DECSCL: VT300
    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT300);
    feed(t, "\033[!p");
    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT300);
    feed(t, "\033c");
    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT500);
}

// DECSTR leaves the xterm input modes as the program set them, as xterm's
// soft reset does; RIS and the reader's local reset turn them off.
TEST(testDECSTRLeavesMousePasteAndFocusModes)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?1002h\033[?1006h\033[?2004h\033[?1004h\033[!p");
    feed(t, "\033[?1002$p\033[?1006$p\033[?2004$p\033[?1004$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1002;1$y\033[?1006;1$y\033[?2004;1$y\033[?1004;1$y") == 0);
    Ubiquitty::softReset(&t);
    CHECK(!t.modes.mouseCellMotion);
    CHECK(!t.modes.mouseSGR);
    CHECK(!t.modes.bracketedPaste);
    CHECK(!t.modes.focusReporting);
    feed(t, "\033[?1002h\033[?2004h\033c\033[?1002$p\033[?2004$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1002;2$y\033[?2004;2$y") == 0);
}

TEST(testDECSCLPerformsSoftReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2$~"); // configure the host-writable status display
    feed(t, "text");
    feed(t, "\033[1;31m"); // bold red pen
    feed(t, "\033[?6h"); // DECOM
    feed(t, "\033[5;15r"); // DECSTBM
    feed(t, "\033[?69h\033[10;60s"); // DECLRMM + narrow margins
    feed(t, "\033[2$~\033[1$}"); // DECSASD status input
    CHECK(t.modes.originMode == true);
    CHECK(t.statusLineActive == 1);

    feed(t, "\033[63;1\"p"); // DECSCL: VT300, 7-bit

    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT300);
    CHECK(t.transmit8bitC1 == false);
    CHECK(t.modes.originMode == false);
    CHECK(!t.screens.active->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == t.rows - 1);
    CHECK(t.modes.leftRightMarginMode == false);
    CHECK(t.screens.active->scrollLeft == 0);
    CHECK(t.screens.active->scrollRight == t.columns - 1);
    CHECK(t.statusLineActive == 0);
    // Cell contents preserved — a soft reset, not a hard one.
    CHECK(cell(t, 0, 0)->codePoint == 't');
    CHECK(cell(t, 0, 3)->codePoint == 't');
    // Pc=0 selects 8-bit C1 on the same reset path.
    feed(t, "\033[62;0\"p");
    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT200);
    CHECK(t.transmit8bitC1 == true);
}

TEST(testDECSTRKeeps7bitC1Transmission)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    // Flip to 8-bit C1 transmission, then soft-reset.
    feed(t, "\033 G"); // S8C1T
    CHECK(t.transmit8bitC1 == true);
    feed(t, "\033[!p"); // DECSTR
    CHECK(t.transmit8bitC1 == false);

    // DA1: ESC [ ?...c, never the single CSI byte 0x9B.
    feed(t, "\033[c");
    int n = drain(t, outputBuffer);
    CHECK(n >= 2);
    CHECK((unsigned char)outputBuffer[0] == 0x1B && outputBuffer[1] == '[');
    for (int i = 0; i < n; ++i)
        CHECK((unsigned char)outputBuffer[i] != 0x9B);

    // DSR cursor position: ESC [ row;col R, never 0x9B.
    feed(t, "\033[6n");
    n = drain(t, outputBuffer);
    CHECK(n >= 2);
    CHECK((unsigned char)outputBuffer[0] == 0x1B && outputBuffer[1] == '[');
    for (int i = 0; i < n; ++i)
        CHECK((unsigned char)outputBuffer[i] != 0x9B);

    // OSC 11 background query: ESC ] 11;... ESC \, never 0x9D / 0x9C.
    feed(t, "\033]11;?\033\\");
    n = drain(t, outputBuffer);
    CHECK(n >= 2);
    CHECK((unsigned char)outputBuffer[0] == 0x1B && outputBuffer[1] == ']');
    for (int i = 0; i < n; ++i) {
        CHECK((unsigned char)outputBuffer[i] != 0x9D);
        CHECK((unsigned char)outputBuffer[i] != 0x9C);
    }
}

TEST(testSyncOutputWatchdogTouchExpires)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Enter 2026, touch the watchdog at tick=500, then assert elapsed
    // advances monotonically until syncOutputExpire force-clears.
    feed(t, "\033[?2026h");
    CHECK(Ubiquitty::Modes::SyncOutput::isActive(&t) == true);
    Ubiquitty::Modes::SyncOutput::touch(&t, Ubiquitty::Ticks(500));
    CHECK(Ubiquitty::Modes::SyncOutput::elapsedTicks(&t, Ubiquitty::Ticks(500)) == 0);
    CHECK(Ubiquitty::Modes::SyncOutput::elapsedTicks(&t, Ubiquitty::Ticks(800)) == 300);
    /// Force expiry — the PAL's watchdog calls this when elapsed exceeds
    /// its budget. Subsequent isSyncOutputActive must be false.
    Ubiquitty::Modes::SyncOutput::expire(&t);
    CHECK(Ubiquitty::Modes::SyncOutput::isActive(&t) == false);
    // Elapsed reads as 0 once the mode is cleared.
    CHECK(Ubiquitty::Modes::SyncOutput::elapsedTicks(&t, Ubiquitty::Ticks(1000)) == 0);
}

TEST(testDECARMAutorepeatMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.autoRepeat == true); // DECARM defaults set (auto-repeat on)
    testStoredModeRoundtrip(t, 8, &t.modes.autoRepeat, "\033[?8h", "\033[?8l", "\033[?8;1$y", "\033[?8;2$y");
}

TEST(testDECNRCMMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.nrcMode == false);
    testStoredModeRoundtrip(t, 42, &t.modes.nrcMode, "\033[?42h", "\033[?42l", "\033[?42;1$y", "\033[?42;2$y");
}

// DECGEPM (?43) and DECKBUM (?68) are recorded: reset at power-up, set and
// reset by DECSET and DECRST, and reported from that state.
TEST(testDECGEPMAndDECKBUMModes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.shouldExpandGraphicsPrint == false);
    CHECK(t.modes.shouldUseDataProcessingKeys == false);
    testStoredModeRoundtrip(t, 43, &t.modes.shouldExpandGraphicsPrint, "\033[?43h", "\033[?43l", "\033[?43;1$y", "\033[?43;2$y");
    testStoredModeRoundtrip(t, 68, &t.modes.shouldUseDataProcessingKeys, "\033[?68h", "\033[?68l", "\033[?68;1$y", "\033[?68;2$y");
}

// DECSTR returns character set mode to multinational and unlocks the keyboard
// (VT510 DECSTR: DECNRCM, KAM; DEC STD 070 DECSTR).
TEST(testDECSTRResetsDECNRCMAndKeyboardActionMode)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?42h\033[2h");
    CHECK(t.modes.nrcMode);
    CHECK(t.modes.keyboardLocked);
    feed(t, "\033[!p");
    CHECK(!t.modes.nrcMode);
    CHECK(!t.modes.keyboardLocked);
    feed(t, "\033[2$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[2;2$y") == 0);
    // The reset mode does not hold a designation back: German still designates.
    feed(t, "\033(K[");
    CHECK(cell(t, 0, 0)->codePoint == 0x00C4);
}

TEST(testDECSTRResetsTheUserPreferredSupplementalSet)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033P0!u%5\033\\\033[&u");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033P0!u%5\033\\") == 0);
    feed(t, "\033[!p\033[&u");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033P1!uA\033\\") == 0);
}

TEST(testDECBKMBackarrowMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.backspaceKeySendsBackspace == false);
    testStoredModeRoundtrip(t, 67, &t.modes.backspaceKeySendsBackspace, "\033[?67h", "\033[?67l", "\033[?67;1$y", "\033[?67;2$y");
}

TEST(testLinuxConsolePowerSequences)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    DisplayPowerClient client;
    t.windowClient = &client;

    // Defaults are zero-initialized by construction.
    CHECK(t.screenBlankTimeout == 0);
    CHECK(t.vesaPowerdownTimeout == 0);
    CHECK(t.screenBlanked == false);

    // CSI 9 ; n ] — set screen-blank timeout to n minutes. Stored, no blank
    // (a finite timeout needs a kernel idle timer we don't have).
    feed(t, "\033[9;15]");
    CHECK(t.screenBlankTimeout == 15);
    CHECK(t.screenBlanked == false);
    CHECK(client.calls == 0);

    // Clamp to 60.
    feed(t, "\033[9;9999]");
    CHECK(t.screenBlankTimeout == 60);

    // CSI 14 ; n ] — VESA powerdown interval.
    feed(t, "\033[14;30]");
    CHECK(t.vesaPowerdownTimeout == 30);

    // CSI 13 ] on a lit screen is a no-op (nothing to unblank).
    feed(t, "\033[13]");
    CHECK(client.calls == 0);

    // Simulate the frontend-driven DPMS-off, then CSI 13 ] restores. A shell
    // with no WindowClient reads the same state off the mode snapshot, so the
    // sampler carries it in both directions.
    CHECK(Ubiquitty::Runtime::sampleModes(t).isScreenBlanked == false);
    t.screenBlanked = true;
    CHECK(Ubiquitty::Runtime::sampleModes(t).isScreenBlanked == true);
    feed(t, "\033[13]");
    CHECK(t.screenBlanked == false);
    CHECK(Ubiquitty::Runtime::sampleModes(t).isScreenBlanked == false);
    CHECK(client.calls == 1);
    CHECK(client.lastState == Ubiquitty::DisplayPowerState::Active);

    // Out-of-family Linux CSI ] codes (e.g. 11 = bell duration) are swallowed
    // as documented no-ops — no power-state change, no crash.
    feed(t, "\033[11;100]");
    CHECK(client.calls == 1);
    CHECK(t.screenBlanked == false);

    // RIS clears the stored timeouts (POD re-init).
    feed(t, "\033[9;42]");
    CHECK(t.screenBlankTimeout == 42);
    feed(t, "\033c"); // RIS
    CHECK(t.screenBlankTimeout == 0);
    CHECK(t.vesaPowerdownTimeout == 0);

    t.windowClient = nullptr;
}

TEST(testPasswordModeExplicit7401)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    PasswordClient client;
    t.windowClient = &client;

    CHECK(t.passwordPromptActive == false);
    // Private mode 7401: explicit assertion.
    feed(t, "\033[?7401h");
    CHECK(t.passwordPromptActive == true);
    CHECK(client.calls == 1);
    CHECK(client.isLastActive == true);
    // DECRQM reports it set.
    {
        char outputBuffer[64];
        feed(t, "\033[?7401$p");
        int n = drain(t, outputBuffer);
        CHECK(n > 0);
        CHECK(strcmp(outputBuffer, "\033[?7401;1$y") == 0);
    }
    feed(t, "\033[?7401l");
    CHECK(t.passwordPromptActive == false);
    CHECK(client.calls == 2);
    CHECK(client.isLastActive == false);

    t.windowClient = nullptr;
}

TEST(testPasswordModeExplicit7401OutlastsTheHeuristic)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    PasswordClient client;
    t.windowClient = &client;
    Ubiquitty::setPasswordDetect(&t, true);

    // The application asserts the prompt with nothing on the line that looks
    // like one; the heuristic's next settle must not clear the assertion.
    feed(t, "\033[?7401h");
    CHECK(t.passwordPromptActive == true);
    feed(t, "enter> ");
    CHECK(t.passwordPromptActive == true);
    // Turning the heuristic off leaves an asserted prompt standing.
    Ubiquitty::setPasswordDetect(&t, false);
    CHECK(t.passwordPromptActive == true);
    Ubiquitty::setPasswordDetect(&t, true);
    // The reset is the application's, and the heuristic decides again after it.
    feed(t, "\033[?7401l");
    CHECK(t.passwordPromptActive == false);
    feed(t, "\r\nPassword: ");
    CHECK(t.passwordPromptActive == true);

    t.windowClient = nullptr;
}

TEST(testPasswordModeHeuristicDetectsPrompt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    PasswordClient client;
    t.windowClient = &client;

    // Heuristic OFF by default: a password prompt does not flip the flag.
    feed(t, "Password: ");
    CHECK(t.passwordPromptActive == false);
    CHECK(client.calls == 0);

    /// Enable the heuristic.
    Ubiquitty::setPasswordDetect(&t, true);

    // A sudo-style prompt that ends at a colon flips it on.
    feed(t, "\r\n[sudo] password for alice: ");
    CHECK(t.passwordPromptActive == true);
    CHECK(Ubiquitty::passwordPromptActive(&t));
    CHECK(client.calls == 1);
    CHECK(client.isLastActive == true);

    // Typing the secret (cursor advances past the colon) — still a password
    // prompt context until a non-prompt line settles. The cursor row no longer
    // ends at a colon prompt once chars follow, so it clears.
    feed(t, "\r\nLast login: Tue\r\n$ ");
    CHECK(t.passwordPromptActive == false);
    CHECK(client.isLastActive == false);

    t.windowClient = nullptr;
}

TEST(testPasswordModeHeuristicIgnoresNonPrompt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::setPasswordDetect(&t, true);

    // "password" mentioned mid-line but not a colon prompt → no flip.
    feed(t, "the password was changed successfully\r\n");
    CHECK(t.passwordPromptActive == false);

    // "Enter passphrase:" → flip.
    feed(t, "Enter passphrase for key '/id': ");
    CHECK(t.passwordPromptActive == true);

    /// Disabling the heuristic clears the latched prompt.
    Ubiquitty::setPasswordDetect(&t, false);
    CHECK(t.passwordPromptActive == false);
}

TEST(testReverseWrapModeRoundtrip)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.reverseWrap == false);
    testStoredModeRoundtrip(t, 45, &t.modes.reverseWrap, "\033[?45h", "\033[?45l", "\033[?45;1$y", "\033[?45;2$y");
}

TEST(testGroundLfFastPathHonorsLNM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033[20h");
    CHECK(t.modes.lnm);
    feed(t, "ABC\nZ");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 1);
    CHECK(cell(t, 1, 0)->codePoint == 'Z');
    CHECK(cell(t, 1, 3)->codePoint != 'Z');
}

/// DECAWM off (CSI ? 7 l): with autowrap disabled, writes past the
/// right margin overwrite the cell at columns-1 instead of wrapping.
TEST(testDECAWMOffNoWrap)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?7l");
    CHECK(!t.modes.autowrap);
    feed(t, "\033[1;1H");
    // Fill the row up to the right margin.
    for (i = 0; i < Ubiquitty::defaultColumns; i++)
        feed(t, "A");
    // Three extra writes - each must overwrite at columns-1.
    feed(t, "B");
    feed(t, "C");
    feed(t, "D");
    // Cursor must not have advanced to a new row.
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == Ubiquitty::defaultColumns - 1);
    // Last write wins at columns-1.
    CHECK(cell(t, 0, Ubiquitty::defaultColumns - 1)->codePoint == 'D');
    // Row 1 should still be blank.
    CHECK(cell(t, 1, 0)->codePoint == 0);
}

/// RIS must home the cursor.
TEST(testRISResetsCursorToHome)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[10;15H");
    CHECK(t.screens.active->cursorY == 9);
    CHECK(t.screens.active->cursorX == 14);
    feed(t, "\033c");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
}

/// RIS must reset DEC private modes (DECAWM, mouse tracking,
/// bracketed paste) and cursor style back to defaults.
TEST(testRISResetsModes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Flip a representative set of modes to non-default.
    feed(t, "\033[?7l"); // DECAWM off
    feed(t, "\033[?1000h"); // normal mouse tracking
    feed(t, "\033[?2004h"); // bracketed paste
    feed(t, "\033[4 q"); // DECSCUSR underline steady
    CHECK(!t.modes.autowrap);
    CHECK(t.modes.mouseButtonTracking);
    CHECK(t.modes.bracketedPaste);
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::SteadyUnderline);
    feed(t, "\033c");
    // All back to construction defaults.
    CHECK(t.modes.autowrap);
    CHECK(!t.modes.mouseButtonTracking);
    CHECK(!t.modes.bracketedPaste);
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::FollowUserPreference);
}

TEST(testRISPreservesFrontendState)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::setAnswerback(&t, "configured-answerback");
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1);
    Ubiquitty::Modes::setInvalidUTF8Policy(&t, Ubiquitty::Unicode::InvalidUTF8Policy::Drop);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Linux);
    Ubiquitty::setPasswordDetect(&t, true);
    Ubiquitty::setScrollbackLimit(&t, 4321);
    Ubiquitty::Modes::HostPolicy::setTitlePolicy(&t, Ubiquitty::OperationPolicy::Denied);
    Ubiquitty::Modes::HostPolicy::setFontPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    Ubiquitty::Modes::HostPolicy::setColorPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    Ubiquitty::Modes::HostPolicy::setWindowPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    Ubiquitty::Modes::HostPolicy::setCapabilityQueryPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    Ubiquitty::Modes::HostPolicy::setMousePolicy(&t, Ubiquitty::OperationPolicy::Denied);
    Ubiquitty::Modes::HostPolicy::setPasteControlsPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    CHECK(Ubiquitty::Triggers::addRuleSpec(&t, "ERROR", "highlight", Ubiquitty::CaseSensitivity::Sensitive));

    t.windowFocused = false;
    t.bellMode = Ubiquitty::BellMode::Both;
    t.lastBellTick = 89;
    t.cursorShapePref = 2;
    t.cursorBlinkPref = false;
    t.cursorHoldFrames = 73;
    t.cursorBlinkPeriodMilliseconds = 850;
    t.lastActivityTick = 21;
    t.images.kittyImageBytesBudget = 123456;
    t.capabilityPolicy = &fakeCapabilityPolicy;
    t.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;
    t.generation = 41;
    Ubiquitty::Terminal* termAddress = &t;
    Ubiquitty::Protocol::Tmux::configure(&t.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    risTmuxEnterCalls = 0;
    Ubiquitty::Protocol::Tmux::setEnterControlCallback(&t.integration.tmux, [](void*) { ++risTmuxEnterCalls; }, nullptr);
    t.integration.paneRouter.lookup = risPaneLookup;
    t.integration.paneRouter.context = &t;
    t.integration.paneRouter.flush = risPaneFlush;

    t.sessionLogFile = std::tmpfile();
    t.printSinkFile = std::tmpfile();
    CHECK(t.sessionLogFile != nullptr);
    CHECK(t.printSinkFile != nullptr);
    t.sessionLogCaptureEnabled = true;
    t.loggingEnabled = true;
    t.passwordPromptActive = true;
    feed(t, "\033[2 q"); // application override: steady block

    Ubiquitty::hardReset(t);

    CHECK(&t == termAddress);
    CHECK(t.answerback == "configured-answerback");
    CHECK(Ubiquitty::Modes::inputEncoding(&t) == Ubiquitty::Unicode::Encoding::Latin1);
    CHECK(Ubiquitty::Modes::invalidUTF8Policy(&t) == Ubiquitty::Unicode::InvalidUTF8Policy::Drop);
    CHECK(Ubiquitty::Modes::emulation(&t) == Ubiquitty::VT::Emulation::Linux);
    CHECK(t.passwordDetect == true);
    CHECK(t.passwordPromptActive == false);
    CHECK(t.scrollbackLimit == 4321);
    CHECK(t.allowTitleOperations == false);
    CHECK(t.allowFontOperations == false);
    CHECK(t.allowColorOperations == false);
    CHECK(t.allowWindowOperations == false);
    CHECK(t.allowTerminalCapabilityOperations == false);
    CHECK(t.allowMouseOperations == false);
    CHECK(t.allowPasteControls == false);
    CHECK(Ubiquitty::Triggers::ruleCount(&t) == 1);
    CHECK(t.windowFocused == false);
    CHECK(t.bellMode == Ubiquitty::BellMode::Both);
    CHECK(t.lastBellTick == 89);
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::FollowUserPreference);
    CHECK(t.cursorShapePref == 2);
    CHECK(t.cursorBlinkPref == false);
    CHECK(t.cursorHoldFrames == 73);
    CHECK(t.cursorBlinkPeriodMilliseconds == 850);
    CHECK(t.lastActivityTick == 21);
    CHECK(Ubiquitty::Cursor::resolve(&t, 100).shape == Ubiquitty::Cursor::Shape::Bar);
    CHECK(t.images.kittyImageBytesBudget == 123456);
    CHECK(t.capabilityPolicy.isInstalled(&fakeCapabilityPolicy));
    CHECK(t.currentCapabilityOrigin == Ubiquitty::CapabilityOrigin::Remote);
    CHECK(t.generation == 42);
    CHECK(t.integration.tmux.enabled == true);
    CHECK(t.integration.tmux.autoDetect == true);
    CHECK(static_cast<bool>(t.integration.tmux.enterControl));
    t.integration.tmux.enterControl(t.integration.tmux.enterControlContext);
    CHECK(risTmuxEnterCalls == 1);
    CHECK(t.integration.paneRouter.lookup == risPaneLookup);
    CHECK(t.integration.paneRouter.context == &t);
    CHECK(t.integration.paneRouter.flush == risPaneFlush);
    CHECK(t.sessionLogFile != nullptr);
    CHECK(t.printSinkFile != nullptr);
    CHECK(t.sessionLogCaptureEnabled == true);
    CHECK(t.loggingEnabled == false);

    if (t.sessionLogFile)
        Ubiquitty::setSessionLogPath(&t, nullptr);
    if (t.printSinkFile)
        Ubiquitty::setPrintSinkPath(&t, nullptr);
    t.capabilityPolicy = nullptr;
}

TEST(testRISResetsTabStopsToDefault)
{
    Ubiquitty::Terminal t;
    int i;
    Ubiquitty::hardReset(t);
    feed(t, "\033[3g");
    for (i = 0; i < t.columns; i++)
        CHECK(!t.tabStops[i]);
    feed(t, "\033c");
    // Every-8 grid is back: column 8, 16, 24 are stops; column 0 and any
    // non-multiple are not.
    CHECK(!t.tabStops[0]);
    CHECK(t.tabStops[8]);
    CHECK(t.tabStops[16]);
    CHECK(t.tabStops[24]);
    CHECK(!t.tabStops[7]);
    CHECK(!t.tabStops[9]);
}

TEST(testRISResetsDECSTBM)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[6;16r");
    CHECK(t.screens.active->scrollTop == 5);
    CHECK(t.screens.active->scrollBottom == 15);
    feed(t, "\033c");
    // Full-screen scroll: top = 0, bottom = rows-1.
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == t.rows - 1);
}

/// DECSC / DECRC — origin mode (DECOM, mode 6).
/// Per VT220 spec, DECSC snapshots DECOM, and Screen::savedOriginMode carries
/// it. The cursor half is covered by testDECSCDECRCSavesCursorPosition, so this
/// checks the mode bit only.
TEST(testDECSCDECRCSavesOriginMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[5;15r"); // DECSTBM rows 5..15
    feed(t, "\033[?6h"); // DECSET 6 — origin mode on
    CHECK(t.modes.originMode == true);
    feed(t, "\033[1;1H"); // CUP home (relative → row 5)
    feed(t, "\0337"); // DECSC
    feed(t, "\033[?6l"); // DECRST 6 — origin mode off
    CHECK(t.modes.originMode == false);
    feed(t, "\0338"); // DECRC
    // DECSC snapshots DECOM alongside the cursor, so DECRC restores both.
    CHECK(t.modes.originMode == true);
}

TEST(testDECSCLVT100GatesVT400Op)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[61\"p"); // VT100 level
    // DECFRA: fill rows 1..2, columns 1..3 with '*' (CSI Pch ; t;l;b;r $ x).
    feed(t, "\033[42;1;1;2;3$x");
    CHECK(cell(t, 0, 0)->codePoint != '*');
    CHECK(cell(t, 1, 2)->codePoint != '*');

    // Raise to VT400 — the same op now paints.
    feed(t, "\033[64\"p");
    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT400);
    feed(t, "\033[42;1;1;2;3$x");
    CHECK(cell(t, 0, 0)->codePoint == '*');
    CHECK(cell(t, 1, 2)->codePoint == '*');
}

TEST(testDCSAllowlistGate)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    seqGateClear(t);

    // Default: DECRQSS "r" (DECSTBM) replies.
    feed(t, "\033P$qr\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 0);

    /// Deny DECRQSS → the same query is consumed silently (no reply).
    Ubiquitty::OSC::DCSKind deny[] = { Ubiquitty::OSC::DCSKind::DECRQSS };
    CHECK(Ubiquitty::OSC::setDCSDenyList(t, deny, 1));
    feed(t, "\033P$qr\033\\");
    n = drain(t, outputBuffer);
    CHECK(n == 0);

    /// Allow-list {Sixel}: DECRQSS is excluded → still no reply.
    CHECK(Ubiquitty::OSC::setDCSDenyList(t, nullptr, 0));
    Ubiquitty::OSC::DCSKind allow[] = { Ubiquitty::OSC::DCSKind::Sixel };
    CHECK(Ubiquitty::OSC::setDCSAllowList(t, allow, 1));
    feed(t, "\033P$qr\033\\");
    n = drain(t, outputBuffer);
    CHECK(n == 0);

    /// Allow DECRQSS explicitly → reply returns.
    Ubiquitty::OSC::DCSKind allowrqss[] = { Ubiquitty::OSC::DCSKind::DECRQSS };
    CHECK(Ubiquitty::OSC::setDCSAllowList(t, allowrqss, 1));
    feed(t, "\033P$qr\033\\");
    n = drain(t, outputBuffer);
    CHECK(n > 0);

    seqGateClear(t);
}

// No frontend has a terminal toolbar, so mode 10 is permanently reset, before
// and after a set, as a default xterm answers.
TEST(testShowToolbarMode10IsPermanentlyReset)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?10$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?10;4$y") == 0);
    feed(t, "\033[?10h\033[?10$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?10;4$y") == 0);
}

TEST(testMoreFixMode41)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    CHECK(t.moreFix == false);
    feed(t, "\033[?41$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?41;2$y") == 0);
    feed(t, "\033[?41h");
    CHECK(t.moreFix == true);
    feed(t, "\033[?41$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?41;1$y") == 0);
}

TEST(testPrintFormFeedExtentModes1819)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    // DECPFF defaults off.
    CHECK(t.printFormFeed == false);
    feed(t, "\033[?18$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?18;2$y") == 0);
    feed(t, "\033[?18h");
    CHECK(t.printFormFeed == true);
    feed(t, "\033[?18$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?18;1$y") == 0);
    // DECPEX defaults reset, the scrolling region (VT510 DECPEX).
    CHECK(t.printExtentFullScreen == false);
    feed(t, "\033[?19$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?19;2$y") == 0);
    feed(t, "\033[?19h");
    CHECK(t.printExtentFullScreen == true);
    feed(t, "\033[?19$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?19;1$y") == 0);
    feed(t, "\033[?19l");
    CHECK(t.printExtentFullScreen == false);
    feed(t, "\033[?19$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?19;2$y") == 0);
}

TEST(testLoggingMode46SecurityGated)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    // No path installed: ?46 h is recorded and reported (1) but opens no file.
    CHECK(t.loggingEnabled == false);
    feed(t, "\033[?46h");
    CHECK(t.loggingEnabled == true);
    CHECK(t.sessionLogFile == nullptr);
    feed(t, "\033[?46$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?46;1$y") == 0);
    feed(t, "\033[?46l");
    feed(t, "\033[?46$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?46;2$y") == 0);
    CHECK(t.sessionLogFile == nullptr);
    // Install a log path (the gated permission point) — logging starts.
    char path[] = "/tmp/ubq_log_test_XXXXXX";
    int fileDescriptor = mkstemp(path);
    CHECK(fileDescriptor >= 0);
    if (fileDescriptor >= 0)
        close(fileDescriptor);
    CHECK(Ubiquitty::setSessionLogPath(&t, path) == true);
    CHECK(t.sessionLogCaptureEnabled == true);
    CHECK(t.loggingEnabled == false);
    feed(t, "\033[?46$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?46;2$y") == 0);
    // ?46 l resets only the DEC mode bit; frontend capture continues.
    feed(t, "\033[?46l");
    CHECK(t.loggingEnabled == false);
    // ?46 h sets the host-visible DEC mode bit when a path is installed.
    feed(t, "\033[?46h");
    CHECK(t.loggingEnabled == true);
    feed(t, "\033c"); // RIS resets the mode bit, not the configured sink/capture intent.
    CHECK(t.sessionLogFile != nullptr);
    CHECK(t.sessionLogCaptureEnabled == true);
    CHECK(t.loggingEnabled == false);
    feed(t, "\033[?46$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?46;2$y") == 0);
    // Feed bytes after RIS; frontend-owned logging must keep capturing them.
    feed(t, "hello");
    Ubiquitty::setSessionLogPath(&t, nullptr); // close + stop
    CHECK(t.sessionLogCaptureEnabled == false);
    CHECK(t.loggingEnabled == false);
    FILE* f = fopen(path, "rb");
    CHECK(f != nullptr);
    if (f) {
        char buf[64] = { 0 };
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(n >= 5);
        CHECK(strstr(buf, "hello") != nullptr);
    }
    remove(path);
}

/// ---- "is the pointer tracked" is one question with one answer ----
//
// Every DECSET tracking mode counts, DECSET 1001 included: it reports like 1000
// with a highlight handshake on top, so a predicate that reads it as the
// highlight feature rather than as tracking drops it. The runtime arms its
// stranded-tracking recovery off this answer, so a mode missing here leaves the
// pointer captured with nothing able to notice when the application that
// enabled it dies.
TEST(mouseTracksPointerCoversEveryMode)
{
    for (const char* set : { "\033[?9h", "\033[?1000h", "\033[?1001h", "\033[?1002h", "\033[?1003h" }) {
        Ubiquitty::Terminal t;
        CHECK(!Ubiquitty::Mouse::tracksPointer(&t));
        feed(t, set);
        CHECK_MSG(Ubiquitty::Mouse::tracksPointer(&t), set);
    }

    // 1001 alone must clear, which is the case the omission broke.
    Ubiquitty::Terminal t;
    feed(t, "\033[?1001h");
    t.modes.hiliteAwaiting = true;
    CHECK(Ubiquitty::Mouse::clearTracking(&t));
    CHECK(!Ubiquitty::Mouse::tracksPointer(&t));
    CHECK(t.modes.hiliteAwaiting == false);

    // A terminal that never tracked reports nothing to clear.
    Ubiquitty::Terminal fresh;
    CHECK(!Ubiquitty::Mouse::clearTracking(&fresh));
}

// With DECSASD 1, a CR inside a CSI body routes through the slow driver and
// lands the status cursor at column 0 instead of the page cursor.
TEST(testCarriageReturnOnStatusLineRedirects)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2$~"); // configure the host-writable status display
    feed(t, "\033[1$}"); // DECSASD: status line receives writes
    CHECK(t.statusLineActive == 1);
    feed(t, "\033[1;2\r"); // CR lands mid-sequence: still a redirect
    CHECK(t.statusCursorX == 0);
}

/// ---- DECECM (117), rewrap (7723), ambiguous-width reporting (7700), IME (77096) ----

TEST(testEraseColourModeBlanksToTheDefaultColours)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?117$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?117;2$y") == 0);

    // Reset: background colour erase, the blank keeps the red pen's colour.
    feed(t, "\033[41mA\033[2J");
    const Ubiquitty::Cell* blank = cell(t, 0, 0);
    CHECK(blank && blank->codePoint == 0);
    CHECK(blank && Ubiquitty::cellPen(*blank).backgroundIndex != Ubiquitty::colorDefault);
    const uint16_t red = Ubiquitty::cellPen(*blank).backgroundIndex;

    // Set: ED, EL and ECH all blank to the screen's default, whatever the pen says.
    feed(t, "\033[?117h");
    feed(t, "\033[?117$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?117;1$y") == 0);
    feed(t, "A\033[2J");
    blank = cell(t, 0, 0);
    CHECK(blank && Ubiquitty::cellPen(*blank).backgroundIndex == Ubiquitty::colorDefault);
    feed(t, "\033[1;1HAB\033[1;1H\033[K");
    blank = cell(t, 0, 1);
    CHECK(blank && blank->codePoint == 0 && Ubiquitty::cellPen(*blank).backgroundIndex == Ubiquitty::colorDefault);
    feed(t, "AB\r\033[2X");
    blank = cell(t, 0, 0);
    CHECK(blank && blank->codePoint == 0 && Ubiquitty::cellPen(*blank).backgroundIndex == Ubiquitty::colorDefault);

    // Reset again: the pen's colour is back in the blank.
    feed(t, "\033[?117l");
    feed(t, "A\033[2J");
    blank = cell(t, 0, 0);
    CHECK(blank && Ubiquitty::cellPen(*blank).backgroundIndex == red);
}

TEST(testRewrapOnResizeModeGatesTheReflow)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::shouldReflowOnResize(&t, true));
    feed(t, "\033[?7723$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?7723;1$y") == 0);
    feed(t, "\033[?7723l");
    CHECK(!Ubiquitty::shouldReflowOnResize(&t, true));
    feed(t, "\033[?7723$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?7723;2$y") == 0);
    feed(t, "\033[?7723h");
    CHECK(Ubiquitty::shouldReflowOnResize(&t, true));
}

TEST(testAmbiguousWidthReportingAnnouncesTheChoiceAtTheNextOutput)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?7700h");
    // The first output records the choice; a program joining hears nothing
    // until the choice moves.
    feed(t, "a");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "") == 0);
    t.ambiguousWidth = Ubiquitty::Unicode::Width::AmbiguousWidth::Wide;
    feed(t, "b");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[2W") == 0);
    t.ambiguousWidth = Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow;
    feed(t, "c");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[1W") == 0);
    // Reset: the choice may move, nobody asked.
    feed(t, "\033[?7700l");
    t.ambiguousWidth = Ubiquitty::Unicode::Width::AmbiguousWidth::Wide;
    feed(t, "d");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "") == 0);
}

TEST(testAParameterSaturatesAtTheCeilingNotAtSixteenBits)
{
    // foot's 77096 is the largest mode number any terminal defines, and it is
    // above 65535: a parameter that saturated there, or wrapped at sixteen
    // bits, would turn the DECSET into some other mode (11560, here). The
    // ceiling holds a runaway parameter, and a mode past it is nothing.
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?123456$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?99999;0$y") == 0);
    feed(t, "\033[?11560$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?11560;0$y") == 0);
}

TEST(testInputMethodEditorModeReachesTheKeyPath)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    CHECK(t.modes.allowsInputMethodEditor);
    CHECK(Ubiquitty::Runtime::sampleModes(t).allowsInputMethodEditor);
    feed(t, "\033[?77096$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?77096;1$y") == 0);
    feed(t, "\033[?77096l");
    CHECK(!t.modes.allowsInputMethodEditor);
    CHECK(!Ubiquitty::Runtime::sampleModes(t).allowsInputMethodEditor);
    feed(t, "\033[?77096$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?77096;2$y") == 0);
    // Power-on default, and RIS restores it.
    feed(t, "\033c");
    CHECK(t.modes.allowsInputMethodEditor);
}

// ?30 is chrome, and chrome is read off the published mode snapshot rather than
// pushed to a client only one frontend installs. If the bar's answer is not in
// sampleModes, neither macOS shell can see it.
TEST(testScrollbarModeRidesTheModeSnapshot)
{
    // Shown before any reset: a terminal sampled ahead of its first hard
    // reset must not take a bar away.
    Ubiquitty::Terminal fresh;
    CHECK(Ubiquitty::Runtime::sampleModes(fresh).isScrollbarVisible);

    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::Runtime::sampleModes(t).isScrollbarVisible);
    feed(t, "\033[?30l");
    CHECK(!t.scrollbarVisible);
    CHECK(!Ubiquitty::Runtime::sampleModes(t).isScrollbarVisible);
    feed(t, "\033[?30h");
    CHECK(Ubiquitty::Runtime::sampleModes(t).isScrollbarVisible);
    // A hard reset gives the bar back and the snapshot says so: the frontends
    // keep no memory of the mode to restore it from.
    feed(t, "\033[?30l");
    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::Runtime::sampleModes(t).isScrollbarVisible);
}

// DECSCUSR 7 restores the configured shape, as Ps 0 does; a style past 7 is
// undefined and leaves the cursor as it was.
TEST(testDECSCUSRSevenRestoresAndUndefinedStylesAreIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4 q\033[8 q");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::SteadyUnderline);
    feed(t, "\033[7 q");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::FollowUserPreference);
}

// VT510 DECLRMM: setting it makes every line single width, so a line made
// double width before wraps at the right margin as a single-width one.
TEST(testDECLRMMMakesEveryLineSingleWidth)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 8);
    feed(t, "\033#6\033[?69h\033[1;6sABCDEFGH");
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeNormal);
    CHECK(cell(t, 0, 5)->codePoint == 'F');
    CHECK(cell(t, 1, 1)->codePoint == 'H');
}

UBIQUITTY_TEST_MAIN()

TEST(testReadlinePasteModesFollowSetResetSaveAndPermissions)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    CHECK(!terminal.modes.shouldQuotePasteCharacters && !terminal.modes.shouldPreservePasteNewlines);
    feed(terminal, "\033[?2005;2006h");
    CHECK(terminal.modes.shouldQuotePasteCharacters && terminal.modes.shouldPreservePasteNewlines);
    feed(terminal, "\033[?2005;2006s\033[?2005;2006l\033[?2005;2006r");
    CHECK(terminal.modes.shouldQuotePasteCharacters && terminal.modes.shouldPreservePasteNewlines);
    feed(terminal, "\033[!p");
    CHECK(terminal.modes.shouldQuotePasteCharacters && terminal.modes.shouldPreservePasteNewlines);
    terminal.allowPasteControls = false;
    feed(terminal, "\033[?2005;2006l\033[?2005;2006h");
    CHECK(!terminal.modes.shouldQuotePasteCharacters && !terminal.modes.shouldPreservePasteNewlines);
    terminal.allowPasteControls = true;
    feed(terminal, "\033[?2005;2006h\033c");
    CHECK(!terminal.modes.shouldQuotePasteCharacters && !terminal.modes.shouldPreservePasteNewlines);
}

TEST(testReadlineMouseModesFollowSetResetSaveAndReports)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    CHECK(!terminal.modes.shouldMoveReadlinePointOnClick);
    CHECK(!terminal.modes.shouldMoveReadlinePointBeforePaste);
    CHECK(!terminal.modes.shouldDeleteReadlineSelection);
    feed(terminal, "\033[?2001;2002;2003h\033[?2001;2002;2003s\033[?2001;2002;2003l\033[?2001;2002;2003r");
    CHECK(terminal.modes.shouldMoveReadlinePointOnClick);
    CHECK(terminal.modes.shouldMoveReadlinePointBeforePaste);
    CHECK(terminal.modes.shouldDeleteReadlineSelection);
    feed(terminal, "\033[?2001$p\033[?2002$p\033[?2003$p");
    char output[128];
    drain(terminal, output);
    CHECK(std::string_view(output) == "\033[?2001;1$y\033[?2002;1$y\033[?2003;1$y");
    feed(terminal, "\033[!p");
    CHECK(terminal.modes.shouldMoveReadlinePointOnClick);
    CHECK(terminal.modes.shouldMoveReadlinePointBeforePaste);
    CHECK(terminal.modes.shouldDeleteReadlineSelection);
    feed(terminal, "\033c\033[?2001$p\033[?2002$p\033[?2003$p");
    drain(terminal, output);
    CHECK(std::string_view(output) == "\033[?2001;2$y\033[?2002;2$y\033[?2003;2$y");
}

TEST(rightToLeftCopyModeReportsSavesAndResets)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    feed(terminal, "\033[?96$p\033[?96h\033[?96s\033[?96l\033[?96$p\033[?96r\033[?96$p");
    std::array<char, 128> output {};
    drain(terminal, output);
    CHECK(std::string_view(output.data()) == "\033[?96;2$y\033[?96;2$y\033[?96;1$y");
    CHECK(terminal.shouldCopyRightToLeft);
    Ubiquitty::hardReset(terminal);
    CHECK(!terminal.shouldCopyRightToLeft);
}

TEST(rightToLeftWritingWrapsAndScrolls)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 5);
    feed(terminal, "\033[?34h\rABCDE");
    for (int column = 0; column < 5; ++column)
        CHECK(cell(terminal, 0, column)->codePoint == static_cast<unsigned>('E' - column));
    CHECK(terminal.screens.active->wrapPending);
    CHECK(terminal.screens.active->cursorX == 0);
    feed(terminal, "FGHIJK");
    for (int column = 0; column < 5; ++column)
        CHECK(cell(terminal, 0, column)->codePoint == static_cast<unsigned>('J' - column));
    CHECK(cell(terminal, 1, 4)->codePoint == 'K');
    CHECK(terminal.screens.active->cursorX == 3);
    CHECK(terminal.screens.active->cursorY == 1);
}

TEST(rightToLeftWritingInsertsTowardLeftWithoutChangingICHOrDCH)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 5);
    feed(terminal, "ABCDE\033[1;4H\033[?34h\033[4hX");
    const std::string expected = "BCDXE";
    for (int column = 0; column < 5; ++column)
        CHECK(cell(terminal, 0, column)->codePoint == static_cast<unsigned>(expected[column]));
    CHECK(terminal.screens.active->cursorX == 2);
    feed(terminal, "\033[4l\033[@");
    CHECK(cell(terminal, 0, 3)->codePoint == 'D');
    CHECK(cell(terminal, 0, 4)->codePoint == 'X');
    feed(terminal, "\033[P");
    CHECK(cell(terminal, 0, 2)->codePoint == 'D');
    CHECK(cell(terminal, 0, 3)->codePoint == 'X');
}

TEST(rightToLeftWritingControlMotionAndAddressing)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 5, 8);
    feed(terminal, "\033[?34h\rAB\bX");
    CHECK(cell(terminal, 0, 7)->codePoint == 'A');
    CHECK(cell(terminal, 0, 6)->codePoint == 'X');
    CHECK(terminal.screens.active->cursorX == 5);
    feed(terminal, "\033[20h\nY\vZ\fW\033"
                   "EV");
    CHECK(cell(terminal, 1, 7)->codePoint == 'Y');
    CHECK(cell(terminal, 2, 7)->codePoint == 'Z');
    CHECK(cell(terminal, 3, 7)->codePoint == 'W');
    CHECK(cell(terminal, 4, 7)->codePoint == 'V');
    feed(terminal, "\033[1;3H\033[CQ\033[DR");
    CHECK(cell(terminal, 0, 3)->codePoint == 'Q');
    CHECK(cell(terminal, 0, 1)->codePoint == 'R');
}

TEST(rightToLeftWritingRespectsMarginsAndDisabledWrap)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 3, 8);
    feed(terminal, "\033[?69h\033[3;6s\033[?34h\rABCDE");
    CHECK(cell(terminal, 0, 2)->codePoint == 'D');
    CHECK(cell(terminal, 0, 5)->codePoint == 'A');
    CHECK(cell(terminal, 1, 5)->codePoint == 'E');
    feed(terminal, "\033[?7lFGHI");
    CHECK(cell(terminal, 1, 2)->codePoint == 'I');
    CHECK(terminal.screens.active->cursorY == 1);
    CHECK(terminal.screens.active->cursorX == 2);
}

TEST(rightToLeftWritingSharesModeWithStatusDisplay)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 8);
    feed(terminal, "\033[?34h\033[2$~\033[1$}\rAB\bX");
    auto status = Ubiquitty::Rendering::statusRow(terminal);
    REQUIRE(!status.empty());
    CHECK(status[7].codePoint == 'A');
    CHECK(status[6].codePoint == 'X');
    feed(terminal, "\033[?34l\033[0$}CD");
    CHECK(cell(terminal, 0, 0)->codePoint == 'C');
    CHECK(cell(terminal, 0, 1)->codePoint == 'D');
}

TEST(rightToLeftWritingReportsSavesAndResets)
{
    Ubiquitty::Terminal terminal;
    feed(terminal, "\033[?34$p\033[?34h\033[?34s\033[?34l\033[?34$p\033[?34r\033[?34$p");
    std::array<char, 128> output {};
    drain(terminal, output);
    CHECK(std::string_view(output.data()) == "\033[?34;2$y\033[?34;2$y\033[?34;1$y");
    CHECK(terminal.shouldWriteRightToLeft);
    feed(terminal, "\033[!p");
    CHECK(!terminal.shouldWriteRightToLeft);
    feed(terminal, "\033[?34h\033c");
    CHECK(!terminal.shouldWriteRightToLeft);
}

TEST(rightToLeftWritingPreservesUnicodeClustersAcrossEverySplit)
{
    const std::string text = "\033[?34h\rAe\xCC\x81中❤\xEF\xB8\x8F🇬🇧👩‍💻Z";
    Ubiquitty::Terminal whole;
    Ubiquitty::resize(whole, 3, 16);
    feed(whole, text);
    CHECK(cell(whole, 0, 15)->codePoint == 'A');
    CHECK(cell(whole, 0, 14)->codePoint == 'e');
    CHECK(cell(whole, 0, 14)->combiningLength() == 1);
    CHECK(cell(whole, 0, 12)->codePoint == U'中');
    CHECK(cell(whole, 0, 10)->codePoint == U'❤');
    CHECK(cell(whole, 0, 10)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(whole, 0, 5)->codePoint == 'Z');
    for (std::size_t split = 0; split <= text.size(); ++split) {
        Ubiquitty::Terminal chunked;
        Ubiquitty::resize(chunked, 3, 16);
        feed(chunked, std::string_view(text).substr(0, split));
        feed(chunked, std::string_view(text).substr(split));
        CHECK(chunked.screens.active->cursorX == whole.screens.active->cursorX);
        CHECK(chunked.screens.active->cursorY == whole.screens.active->cursorY);
        for (int column = 0; column < 16; ++column) {
            CHECK(cell(chunked, 0, column)->codePoint == cell(whole, 0, column)->codePoint);
            CHECK(cell(chunked, 0, column)->pen.attributes == cell(whole, 0, column)->pen.attributes);
            CHECK(cell(chunked, 0, column)->combining == cell(whole, 0, column)->combining);
        }
    }
}

TEST(rightToLeftWritingUsesTheDestinationLineWidth)
{
    for (const std::string_view separator : { "\n", "\v", "\f", "\033E" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 3, 8);
        feed(terminal, "\033#6\033[?34h\033[20h\rA");
        CHECK(cell(terminal, 0, 3)->codePoint == 'A');
        feed(terminal, separator);
        feed(terminal, "B");
        CHECK(cell(terminal, 1, 7)->codePoint == 'B');
        CHECK(terminal.screens.active->cursorX == 6);
    }
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 3, 8);
    feed(terminal, "\033#6\033[?34h\rABCDE");
    CHECK(cell(terminal, 1, 7)->codePoint == 'E');
}

TEST(rightToLeftWritingOutsideLeftMarginUsesAddressedColumn)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 3, 8);
    feed(terminal, "\033[?69h\033[3;6s\033[?34h\033[1;2HAB");
    CHECK(cell(terminal, 0, 1)->codePoint == 'A');
    CHECK(cell(terminal, 0, 0)->codePoint == 'B');
}

TEST(answerbackConcealmentSurvivesResetsAndStillAnswersENQ)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 2, 8);
    Ubiquitty::setAnswerbackString(terminal, "configured");
    CHECK(Ubiquitty::answerbackForDisplay(terminal) == "configured");
    feed(terminal, "\033[?101h\033[?101l\033[?101$p\005");
    char reply[128];
    drain(terminal, reply);
    CHECK(std::strcmp(reply, "\033[?101;1$yconfigured") == 0);
    CHECK(Ubiquitty::answerbackForDisplay(terminal) == "concealed");
    for (const auto reset : { "\033[!p", "\033c", "\033[61\"p" }) {
        feed(terminal, reset);
        CHECK(terminal.isAnswerbackConcealed);
        CHECK(terminal.answerback == "configured");
        CHECK(Ubiquitty::answerbackForDisplay(terminal) == "concealed");
    }
    Ubiquitty::setAnswerbackString(terminal, "configured");
    CHECK(!terminal.isAnswerbackConcealed);
    feed(terminal, "\033[65;1\"p\033[?101$p");
    drain(terminal, reply);
    CHECK(std::strcmp(reply, "\033[?101;2$y") == 0);
    feed(terminal, "\033[?101h");
    Ubiquitty::setAnswerbackString(terminal, "");
    CHECK(!terminal.isAnswerbackConcealed);
    CHECK(Ubiquitty::answerbackForDisplay(terminal).empty());
}
