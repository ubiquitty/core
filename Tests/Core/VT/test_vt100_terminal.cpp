// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The terminal object itself: construction, destruction, and the state that
// spans everything else.
//
// A freshly constructed terminal's power-on state, repeated construction and
// destruction, the generation counter a hard reset bumps, the cell size, and the
// modified flag that tells a frontend anything happened at all.
//
// These are the cases with no sequence to name them after. They pin the
// properties every other file assumes: that a new terminal starts where the
// spec says, and that tearing one down releases what it took.

#include "Harness.h"
#include "Platform/StreamDouble.h"
#include "VT100Fixture.h"

TEST(testTermConstructorPowerOnState)
{
    Ubiquitty::Terminal t;
    CHECK(t.grid != nullptr);
    CHECK(t.parser.osc.data != nullptr);
    CHECK(t.rows == Ubiquitty::defaultRows);
    CHECK(t.columns == Ubiquitty::defaultColumns);
    CHECK(t.screens.active == &t.screens.primary);
    CHECK(t.modes.allowAlternateScreen);
    CHECK(t.modes.isCursorVisible);
    CHECK(t.cursorBlinkPref);
    CHECK(t.allowTerminalCapabilityOperations);
}

TEST(testTermRepeatedConstructionDestruction)
{
    for (int i = 0; i < 16; ++i) {
        Ubiquitty::Terminal t;
        Ubiquitty::resize(t, 10 + i, 40 + i);
        feed(t, "raii");
        CHECK(t.rows == 10 + i);
        CHECK(t.columns == 40 + i);
        CHECK(cell(t, 0, 0)->codePoint == 'r');
    }
}

TEST(testHardResetKeepsTheHostsScrollLaneSwitches)
{
    // The runtime enables the deferred scroll lanes once, at creation. RIS
    // rebuilds the terminal, and a switch it dropped would leave every later
    // flood on the per-line path for the rest of the session.
    for (const bool isEnabled : { true, false }) {
        Ubiquitty::Terminal t;
        t.isScrollLaneEnabled = isEnabled;
        t.isBandScrollLaneEnabled = isEnabled;
        feed(t, "\033c");
        CHECK(t.isScrollLaneEnabled == isEnabled);
        CHECK(t.isBandScrollLaneEnabled == isEnabled);
    }
}

TEST(test_bracketed_paste_gated_by_allowPasteControls)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    /// Denied: the ENABLE is a no-op.
    Ubiquitty::Modes::HostPolicy::setPasteControlsPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    feed(t, "\033[?2004h");
    CHECK(!t.modes.bracketedPaste);

    /// Allowed (the init default): the ENABLE applies.
    Ubiquitty::Modes::HostPolicy::setPasteControlsPolicy(&t, Ubiquitty::OperationPolicy::Allowed);
    feed(t, "\033[?2004h");
    CHECK(t.modes.bracketedPaste);

    /// A DISABLE always applies, even when re-denied.
    Ubiquitty::Modes::HostPolicy::setPasteControlsPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    feed(t, "\033[?2004l");
    CHECK(!t.modes.bracketedPaste);
}

TEST(testTypingSetsCellsModifiedFlag)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Sync shadow after init so hasUnpaintedWork starts false.
    drainPaintCycle(t, nullptr);
    CHECK(!t.grid->hasUnpaintedWork());
    // Pure cursor move (CSI CUP) must not raise the flag — preserves
    // the perf win for genuine cursor-only frames.
    feed(t, "\033[5;10H");
    CHECK(!t.grid->hasUnpaintedWork());
    // But a character write must set it.
    feed(t, "X");
    CHECK(t.grid->hasUnpaintedWork());
}

TEST(testGraphicAttrIsExplicitNotPrivateUseRange)
{
    Ubiquitty::Terminal t;
    unsigned char private_use[] = { 0xEE, 0x82, 0xA0 }; // U+E0A0
    unsigned char box_hline[] = { 0xE2, 0x94, 0x80 }; // U+2500

    Ubiquitty::hardReset(t);
    feed(t, private_use);
    CHECK(cell(t, 0, 0)->codePoint == 0xE0A0); // canonical Unicode pass-through
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::ForceEmojiPresentation));

    Ubiquitty::hardReset(t);
    feed(t, box_hline);
    CHECK(cell(t, 0, 0)->codePoint == 0x2500);
}

TEST(testTermHardResetBumpsGeneration)
{
    Ubiquitty::Terminal t;
    uint32_t startGen = t.generation;
    Ubiquitty::hardReset(t);
    CHECK(t.generation == startGen + 1);
}

TEST(testCellSizeUnchanged)
{
    CHECK(sizeof(Ubiquitty::Cell) == 32);

    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    unsigned char family4[] = { 0xF0, 0x9F, 0x91, 0xA8, 0xE2, 0x80, 0x8D, 0xF0, 0x9F, 0x91, 0xA9, 0xE2, 0x80, 0x8D, 0xF0, 0x9F, 0x91, 0xA7, 0xE2, 0x80, 0x8D, 0xF0, 0x9F, 0x91, 0xA6 };
    feed(t, family4);
    const Ubiquitty::Cell* lead = cell(t, 0, 0);
    Ubiquitty::Cell copy = *lead; // whole-cell copy
    CHECK(memcmp(lead, &copy, sizeof(Ubiquitty::Cell)) == 0); // cluster id rides along
    CHECK(copy.clusterID() == lead->clusterID());
    Ubiquitty::Cell bare = *lead;
    bare.setClusterID(0);
    bare.setCombiningLength(0);
    CHECK(memcmp(lead, &bare, sizeof(Ubiquitty::Cell)) != 0); // distinct from bare base
}

TEST(testMoreFixNoDoubleLinefeed)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    /// Resize to a known narrow width so the last column is reachable.
    Ubiquitty::resize(t, 24, 5);
    feed(t, "ABCDE"); // fills the row; arms wrapPending at col 4
    // Cursor is parked at the right margin with a pending wrap, still row 0.
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\r\n"); // CR+LF: advance exactly one row, no extra blank line
    CHECK(t.screens.active->cursorY == 1);
    feed(t, "Z");
    CHECK(cell(t, 1, 0)->codePoint == 'Z'); // lands on row 1, not row 2
}

// Password-prompt heuristic (config-gated on passwordDetect): a settled line
// ending in ':' that contains "password"/"passphrase"/"passcode"
// (case-insensitive) flips passwordPromptActive; anything else clears it.
// The heuristic reads the cursor's row at the end of each input batch, so
// every feed below ends with the cursor parked on the row under test.
TEST(testPasswordPromptHeuristic)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.passwordDetect = true;

    feed(t, "password: "); // colon-terminated keyword line → active
    CHECK(t.passwordPromptActive == true);

    feed(t, "password: "); // same prompt state again: no transition, no re-signal
    CHECK(t.passwordPromptActive == true);

    feed(t, "\r\nls -la"); // an ordinary line (no colon) clears the flag
    CHECK(t.passwordPromptActive == false);

    feed(t, "\r\nbuild dir: "); // colon without a keyword never triggers
    CHECK(t.passwordPromptActive == false);

    feed(t, "\r\nenter passcode: "); // "passcode" triggers like "password"
    CHECK(t.passwordPromptActive == true);

    feed(t, "\r\n\xc3\xa9"); // a blank-only row (é substituted to a space) is not a prompt
    CHECK(t.passwordPromptActive == false);
}

// The PTY read loop feeds through processBytesUpToSyncClear, which decides
// nothing about the prompt itself: a batch on that entry is one kernel quantum
// of a flood, and the owner's facts cost an ioctl. The runtime settles once
// the writer pauses, after any tail it re-enters past a synchronized-update
// clear.
TEST(testPasswordPromptSettlesAfterThePTYEntryNotInsideIt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.passwordDetect = true;
    Ubiquitty::TestDoubles::ProcessFactsDouble process;

    const std::string_view prompt = "Password: ";
    CHECK(Ubiquitty::processBytesUpToSyncClear(&t, prompt, nullptr) == static_cast<int>(prompt.size()));
    CHECK(t.passwordPromptActive == false); // the feed alone decides nothing
    CHECK_EQ(process.disciplineProbes, 0);
    Ubiquitty::settlePasswordPrompt(t, &process);
    CHECK(t.passwordPromptActive == true);
    CHECK_EQ(process.disciplineProbes, 1);

    // A batch that ends inside a synchronized update stops at the clear; the
    // loop feeds the remainder as its own batch before the pause settles.
    const std::string_view framed = "\r\n\033[?2026hls -la\033[?2026l\r\nPassphrase for key: ";
    bool stoppedAtSyncClear = false;
    const int consumed = Ubiquitty::processBytesUpToSyncClear(&t, framed, &stoppedAtSyncClear);
    CHECK(stoppedAtSyncClear == true);
    CHECK(consumed > 0 && consumed < static_cast<int>(framed.size()));
    const std::string_view tail = framed.substr(static_cast<std::size_t>(consumed));
    CHECK(Ubiquitty::processBytesUpToSyncClear(&t, tail, nullptr) == static_cast<int>(tail.size()));
    CHECK_EQ(process.disciplineProbes, 1); // neither the cut batch nor its tail sampled
    Ubiquitty::settlePasswordPrompt(t, &process);
    CHECK(t.passwordPromptActive == true);

    const std::string_view plain = "\r\n$ ";
    CHECK(Ubiquitty::processBytesUpToSyncClear(&t, plain, nullptr) == static_cast<int>(plain.size()));
    CHECK(t.passwordPromptActive == true); // the last settled verdict stands until the pause
    Ubiquitty::settlePasswordPrompt(t, &process);
    CHECK(t.passwordPromptActive == false);
}

TEST(passwordDetectionUsesTheOwningPTYDiscipline)
{
    using namespace Ubiquitty;
    const std::array<std::string_view, 4> prompts {
        "That password is the one in /Users/user/keychain-password. Put it on the clipboard until the export ends? (y/n) [y]:",
        "Password:",
        "user@host's password:",
        "PIN> ",
    };
    PAL::LineDiscipline canonical;
    PAL::LineDiscipline secret = canonical;
    secret.isEchoEnabled = false;
    PAL::LineDiscipline raw = secret;
    raw.isCanonical = false;
    PAL::LineDiscipline rawEcho = raw;
    rawEcho.isEchoEnabled = true;
    const std::array<std::optional<PAL::LineDiscipline>, 5> disciplines { canonical, secret, raw, rawEcho, std::nullopt };
    for (const auto& discipline : disciplines) {
        for (const auto prompt : prompts) {
            for (const bool usesSyncEntry : { false, true }) {
                Terminal terminal;
                resize(terminal, 4, 200);
                terminal.passwordDetect = true;
                TestDoubles::ProcessFactsDouble process;
                process.discipline = discipline;
                if (usesSyncEntry) {
                    CHECK_EQ(processBytesUpToSyncClear(&terminal, prompt, nullptr), static_cast<int>(prompt.size()));
                    CHECK_EQ(process.disciplineProbes, 0); // the PTY entry leaves the facts to the pause
                    settlePasswordPrompt(terminal, &process);
                } else {
                    processBytes(&terminal, prompt, &process);
                    CHECK_EQ(process.disciplineProbes, 1); // the plain entry settles itself
                }
                const bool isExpectedActive = discipline && discipline->isCanonical ? !discipline->isEchoEnabled : prompt != "PIN> ";
                CHECK_EQ(terminal.passwordPromptActive, isExpectedActive);
                CHECK_EQ(process.disciplineProbes, 1);

                process.discipline = canonical;
                processBytes(&terminal, "", &process);
                CHECK(!terminal.passwordPromptActive);
                CHECK_EQ(process.disciplineProbes, 2);
            }
        }
    }
}

TEST(passwordDetectionSamplesOnlyWhenAnUnassertedBatchSettles)
{
    using namespace Ubiquitty;
    Terminal terminal;
    TestDoubles::ProcessFactsDouble process;
    process.discipline = PAL::LineDiscipline {};
    processBytes(&terminal, "Password:", &process);
    CHECK_EQ(process.disciplineProbes, 0);
    CHECK(!terminal.passwordPromptActive);

    terminal.passwordDetect = true;
    processBytes(&terminal, "\033[?7401h", &process);
    CHECK(terminal.passwordPromptActive);
    CHECK_EQ(process.disciplineProbes, 0);
    processBytes(&terminal, "\033[?7401l", &process);
    CHECK(!terminal.passwordPromptActive);
    CHECK_EQ(process.disciplineProbes, 1);

    const std::string_view batch = "\033[?2026h\r\nPassword:\033[?2026l tail";
    bool stopped = false;
    const int consumed = processBytesUpToSyncClear(&terminal, batch, &stopped);
    REQUIRE(stopped);
    REQUIRE(consumed > 0 && consumed < static_cast<int>(batch.size()));
    const auto tail = batch.substr(static_cast<std::size_t>(consumed));
    CHECK_EQ(processBytesUpToSyncClear(&terminal, tail, &stopped), static_cast<int>(tail.size()));
    CHECK_EQ(process.disciplineProbes, 1); // the PTY entry never samples; the pause does
    settlePasswordPrompt(terminal, &process);
    CHECK_EQ(process.disciplineProbes, 2);
    CHECK(!terminal.passwordPromptActive);
}

TEST(aClientWithoutPTYFactsPreservesTheHostsPasswordVerdict)
{
    using namespace Ubiquitty;
    for (const bool isHostPromptActive : { false, true }) {
        Terminal terminal;
        terminal.replyAuthority = ReplyAuthority::Elsewhere;
        terminal.passwordDetect = true;
        terminal.passwordPromptActive = isHostPromptActive;
        processBytes(&terminal, "Password:");
        CHECK_EQ(terminal.passwordPromptActive, isHostPromptActive);
        const std::string_view plain = "\r\n$ ";
        CHECK_EQ(processBytesUpToSyncClear(&terminal, plain), static_cast<int>(plain.size()));
        CHECK_EQ(terminal.passwordPromptActive, isHostPromptActive);
        TestDoubles::ProcessFactsDouble process;
        process.discipline = PAL::LineDiscipline {};
        process.discipline->isEchoEnabled = false;
        processBytes(&terminal, "Password:", &process);
        CHECK_EQ(terminal.passwordPromptActive, isHostPromptActive);
        CHECK_EQ(process.disciplineProbes, 0);
        setPasswordDetect(&terminal, false);
        CHECK_EQ(terminal.passwordPromptActive, isHostPromptActive);
    }
}

TEST(aParserOwnerWithoutPTYFactsDetectsPasswordPrompts)
{
    using namespace Ubiquitty;
    Terminal terminal;
    terminal.passwordDetect = true;
    processBytes(&terminal, "Password:");
    CHECK(terminal.passwordPromptActive);
    processBytes(&terminal, "\r\n$ ");
    CHECK(!terminal.passwordPromptActive);
    // The PTY entry leaves the verdict to the pause; an owner with no facts
    // still reads the text when that pause settles.
    const std::string_view prompt = "\r\nuser@host's password:";
    CHECK_EQ(processBytesUpToSyncClear(&terminal, prompt), static_cast<int>(prompt.size()));
    settlePasswordPrompt(terminal, nullptr);
    CHECK(terminal.passwordPromptActive);
    const std::string_view plain = "\r\n$ ";
    CHECK_EQ(processBytesUpToSyncClear(&terminal, plain), static_cast<int>(plain.size()));
    settlePasswordPrompt(terminal, nullptr);
    CHECK(!terminal.passwordPromptActive);
}

UBIQUITTY_TEST_MAIN()
