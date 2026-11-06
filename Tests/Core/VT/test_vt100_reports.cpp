// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Everything the terminal says back when asked.
//
// Device attributes, device status, the DECRQSS setting queries, DECRQM, the
// XTGETTCAP terminfo surface, and the version and capability reports a program
// uses to decide what it may send.
//
// A report is a contract in the direction a test usually does not look: a
// program that mis-parses a reply degrades quietly rather than failing, so the
// cases assert the exact bytes including the introducer and terminator, not
// just the payload. The capability gates sit here too, because a denied query
// still owes a well-formed answer.

#include "Core/BuildVersion.h"
#include "Core/Keys/Keymap.h"
#include "Harness.h"
#include "VT100Fixture.h"

#include <string>
#include <string_view>
#include <utility>

// The UDK table holds udkMaxEntries bindings; a DECUDK that lists more key
// pairs stops accepting once the table is full — no wrap-around, no growth.
TEST(testDECUDKTableFullDropsExtraEntries)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    std::string dcs = "\033P1;1|";
    for (int k = 1; k <= Ubiquitty::VT::udkMaxEntries + 2; ++k)
        dcs += std::to_string(k) + "/41;"; // each pair binds key k to "A"
    dcs += "\033\\";
    feed(t, dcs.c_str());
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).count == Ubiquitty::VT::udkMaxEntries);
    CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[Ubiquitty::VT::udkMaxEntries - 1].keyCode == Ubiquitty::VT::udkMaxEntries);
}

TEST(testS7c1tS8c1tTogglesFlag)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.transmit8bitC1 == false); // default: 7-bit
    feed(t, "\033 G");
    CHECK(t.transmit8bitC1 == true);
    feed(t, "\033 F");
    CHECK(t.transmit8bitC1 == false);
    // Unknown final byte: silently dropped, flag unchanged.
    feed(t, "\033 X");
    CHECK(t.transmit8bitC1 == false);
}

TEST(testDECRQSSSGRDefault)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    feed(t, "\033P$qm\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    // Expected: ESC P 1 $ r 0m ESC `\\`
    CHECK(strstr(outputBuffer, "\033P1$r0m\033\\") != nullptr);
}

TEST(testDECRQSSSGRBoldRed)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;31m"); // bold + red foreground
    feed(t, "\033P$qm\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, ";1") != nullptr); // bold
    CHECK(strstr(outputBuffer, ";31") != nullptr); // foreground 31 (red)
}

TEST(testDECRQSSDECSTBM)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    feed(t, "\033[5;15r");
    feed(t, "\033P$qr\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r5;15r\033\\") != nullptr);
}

TEST(testDECRQSSDECSCUSR)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    feed(t, "\033[3 q"); // underline blink
    feed(t, "\033P$q q\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r3 q\033\\") != nullptr);
}

TEST(testDECRQSSDECSCL)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    feed(t, "\033[64;1\"p"); // DECSCL: VT400, 7-bit C1 transmission
    feed(t, "\033P$q\"p\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r64;1\"p\033\\") != nullptr);
}

TEST(testDECRQSSDECSLRM)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?69h"); // DECLRMM: enable left/right margins
    feed(t, "\033[3;40s"); // DECSLRM: left=3, right=40
    feed(t, "\033P$qs\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r3;40s\033\\") != nullptr);
}

TEST(testDECRQSSUnknownSelector)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    feed(t, "\033P$qZZ\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P0$r\033\\") != nullptr);
}

TEST(testDECRQSSSGRFullAttributeSet)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    // Dim, italic, underline, blink, reverse, hidden, strike + truecolor
    // foreground + indexed background: every SGR re-emit arm.
    feed(t, "\033[2;3;4;5;7;8;9m");
    feed(t, "\033[38;2;10;20;30m");
    feed(t, "\033[48;5;200m");
    feed(t, "\033P$qm\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r0;4;5;7;8;2;3;9;38:2::10:20:30;48:5:200m\033\\") != nullptr);
}

TEST(testDECRQSSWritesAttributesInXtermOrder)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    feed(t, "\033[1;2;3;5;7;8;9;21m");
    feed(t, "\033P$qm\033\\");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1$r0;1;5;7;8;2;3;9;21m\033\\");
}

TEST(testDECRQSSSGRFastBlink)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    // SGR 6 (rapid blink) re-emits as 6, not the plain 5.
    feed(t, "\033[6m");
    feed(t, "\033P$qm\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r0;6m\033\\") != nullptr);
}

TEST(testDECRQSSSGRBrightColors)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    // Bright palette slots re-emit as 90-97 / 100-107.
    feed(t, "\033[91;107m");
    feed(t, "\033P$qm\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r0;91;107m\033\\") != nullptr);
}

TEST(testDECRQSSWritesAnIndexedColorInColonForm)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    // An extended index is written 38:5:N; a dark palette slot as 40-47.
    feed(t, "\033[38;5;200;44m");
    feed(t, "\033P$qm\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r0;38:5:200;44m\033\\") != nullptr);
}

TEST(testDECRQSSWritesADirectColorInColonForm)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    feed(t, "\033[48;2;1;2;3m");
    feed(t, "\033P$qm\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r0;48:2::1:2:3m\033\\") != nullptr);
}

TEST(testDECRQSSDECSCA)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    feed(t, "\033[1\"q"); // DECSCA: protect
    feed(t, "\033P$q\"q\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r1\"q\033\\") != nullptr);
    feed(t, "\033[0\"q"); // DECSCA: unprotect
    feed(t, "\033P$q\"q\033\\");
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "\033P1$r0\"q\033\\") != nullptr);
}

TEST(testDECRQSSSettingsReport)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    // DECSSDT, DECSASD, DECSLPP: reported from live Terminal state.
    feed(t, "\033P$q$~\033\\");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033P1$r0$~\033\\") != nullptr);
    feed(t, "\033P$q$}\033\\");
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "\033P1$r0$}\033\\") != nullptr);
    feed(t, "\033P$qt\033\\");
    drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "\033P1$r24t\033\\") != nullptr);
}

TEST(testNicheDecReports)
{
    Ubiquitty::Terminal t;
    char outputBuffer[96];
    char want[64];
    Ubiquitty::hardReset(t);

    // DEC locator status / type DSR: 50 = available. Ubiquitty implements the
    // locator (DECELR/DECSLE/DECEFR/DECLRP), so status is always "ready".
    feed(t, "\033[?55n");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?50n") == 0);
    feed(t, "\033[?56n");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?57;1n") == 0);

    // DECRQDE — whole page displayed, anchored at 1;1 page 1.
    feed(t, "\033[\"v");
    drain(t, outputBuffer);
    std::snprintf(want, sizeof(want), "\033[%d;%d;1;1;1\"w", t.rows, t.columns);
    CHECK(strcmp(outputBuffer, want) == 0);

    // DECRQUPSS — ISO Latin-1 (DCS 1 ! u A ST).
    feed(t, "\033[&u");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033P1!uA\033\\") == 0);

    // DECAUPSS assigns DEC Supplemental (Ps 0, % 5) and ISO Latin-1 (Ps 1, A);
    // any other assignment is ignored.
    feed(t, "\033P0!u%5\033\\\033[&u");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033P0!u%5\033\\") == 0);
    feed(t, "\033P1!uZ\033\\\033P0!uA\033\\\033P1!u%5\033\\\033[&u");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033P0!u%5\033\\") == 0);
    feed(t, "\033P1!uA\033\\\033[&u");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033P1!uA\033\\") == 0);

    // DECCIR at home, default rendition: cursor 1;1, page 1; framed by
    // DCS 1 $ u … ST.
    feed(t, "\033[1$w");
    int n = drain(t, outputBuffer);
    CHECK(n > 7);
    CHECK(strncmp(outputBuffer, "\033P1$u1;1;1;", 11) == 0);
    CHECK(outputBuffer[n - 2] == 0x1B && outputBuffer[n - 1] == '\\');
}

TEST(test_modifyFunctionKeys_set_and_query)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[>2;3m");
    CHECK(t.xtermKeyModifiers[2] == 3);
    feed(t, "\033[?2m");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033[>2;3m") != nullptr);
}

TEST(test_modifyKeypadKeys_set_and_query)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[>3;1m");
    CHECK(t.xtermKeyModifiers[3] == 1);
    feed(t, "\033[?3m");
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "\033[>3;1m") != nullptr);
}

// CSI > Pp n disables a resource: a disabled cursor or function key sends what
// the unmodified key sends, and the query reports xterm's -1 as 65535.
TEST(testXTMODKEYSDisablesAResource)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    Ubiquitty::hardReset(t);
    feed(t, "\033[>1;3m");
    CHECK(t.xtermKeyModifiers[1] == 3);
    feed(t, "\033[>1n\033[>n\033[>4;2m\033[>4n\033[?1;2;4m");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033[>1;65535m\033[>2;65535m\033[>4;65535m");
    t.allowTerminalCapabilityOperations = true;
    feed(t, "\033P+q6b524954;6b663134\033\\"); // kRIT;kf14
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1+r6b524954=1B5B43;6b663134=1B4F51\033\\");
    feed(t, "\033P$q>1m\033\\");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1$r>1;65535m\033\\");
    feed(t, "\033[>1m");
    CHECK(t.xtermKeyModifiers[1] == 2);
}

// Format resources without an encoder binding remain zero.
TEST(testKeyFormatsTheEncoderDoesNotReadAreRefused)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[>2;1f");
    CHECK(t.xtermKeyFormats[2] == 0);
    feed(t, "\033[?2g");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strcmp(outputBuffer, "\033[>2;0f") == 0);
    feed(t, "\033[>2g");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strcmp(outputBuffer, "\033[>2;0f") == 0);
    feed(t, "\033[>9g");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strcmp(outputBuffer, "\033[>9;65535f") == 0);
    feed(t, "\033[>4;1f\033[>f\033[>4g");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strcmp(outputBuffer, "\033[>4;0f") == 0);
}

TEST(test_modifyKeys_out_of_range_category_ignored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[>15;1m");
    // No assertion on internal state — the array is 8 slots and
    // category 15 wouldn't fit. The test passes if processing
    // didn't crash.
    CHECK(true);
}

// The key-modifier resources start at xterm's defaults, which RIS and DECSTR
// restore: modifyCursorKeys and modifyFunctionKeys at 2, the rest at 0.
TEST(testXTQMODKEYSReportsXtermsDefaults)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?0;1;2;3;4m");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033[>0;0m\033[>1;2m\033[>2;2m\033[>3;0m\033[>4;0m");
    feed(t, "\033[>1;3m\033[>2;1m\033[!p\033[?1;2m");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033[>1;2m\033[>2;2m");
}

// DECRQSS reports a key-modifier or key-format resource as the control that
// restores it, and refuses a resource the CSI query does not answer.
TEST(testDECRQSSReportsKeyModifierResources)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    Ubiquitty::hardReset(t);
    feed(t, "\033P$q>1m\033\\");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1$r>1;2m\033\\");
    feed(t, "\033[>4;2m\033[>4;1f\033P$q>4m\033\\\033P$q>4f\033\\");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1$r>4;2m\033\\\033P1$r>4;1f\033\\");
    feed(t, "\033P$q>5m\033\\\033P$q>m\033\\\033P$q>2f\033\\");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1$r>5;0m\033\\\033P0$r\033\\\033P1$r>2;0f\033\\");
}

// XTMODKEYS with no parameters returns every resource to its initial value.
TEST(testBareXTMODKEYSResetsEveryResource)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[>1;3m\033[>2;1m\033[>3;3m\033[>4;2m");
    feed(t, "\033[>m");
    CHECK(t.xtermKeyModifiers[1] == 2);
    CHECK(t.xtermKeyModifiers[2] == 2);
    CHECK(t.xtermKeyModifiers[3] == 0);
    CHECK(t.xtermKeyModifiers[4] == 0);
}

// A resource named with its value omitted, or written empty, returns only that
// resource to its initial value.
TEST(testXTMODKEYSWithoutAValueResetsOnlyThatResource)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[>1;3m\033[>4;2m");
    feed(t, "\033[>1m");
    CHECK(t.xtermKeyModifiers[1] == 2);
    CHECK(t.xtermKeyModifiers[4] == 2);
    feed(t, "\033[>2;3m\033[>2;m");
    CHECK(t.xtermKeyModifiers[2] == 2);
    CHECK(t.xtermKeyModifiers[4] == 2);
}

TEST(testBareXTFMTKEYSResetsFormatOtherKeys)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[>4;1f\033[>4f");
    CHECK(t.xtermKeyFormats[4] == 0);
    feed(t, "\033[>4;1f\033[>6;1f\033[>7;1f");
    CHECK(t.xtermKeyFormats[4] == 1);
    CHECK(t.xtermKeyFormats[6] == 1);
    CHECK(t.xtermKeyFormats[7] == 1);
    feed(t, "\033[>f");
    CHECK(t.xtermKeyFormats[4] == 0);
    CHECK(t.xtermKeyFormats[6] == 0);
    CHECK(t.xtermKeyFormats[7] == 0);
}

// Resource 0 names modifyKeyboard; an omitted disable names modifyFunctionKeys.
TEST(testXTMODKEYSResourceZeroIsNotAnOmittedResource)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[>0;1m");
    CHECK(t.xtermKeyModifiers[0] == 1);
    CHECK(t.xtermKeyModifiers[4] == 0);
    feed(t, "\033[>4;2m\033[>2;1m");
    feed(t, "\033[?0m");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033[>0;1m");
    feed(t, "\033[>0n");
    CHECK(t.xtermKeyModifiers[2] == 1);
    feed(t, "\033[>n");
    CHECK(t.xtermKeyModifiers[2] == Ubiquitty::Keys::Keymap::keymapModifyLevelDisabled);
    CHECK(t.xtermKeyModifiers[4] == 2);
}

// A resource the encoder does not read, or a level past the ones it encodes,
// is refused rather than stored for a query to report.
TEST(testXTMODKEYSRefusesWhatTheEncoderDoesNotRead)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[>5;1m\033[>6;1m\033[>7;1m");
    CHECK(t.xtermKeyModifiers[5] == 0);
    CHECK(t.xtermKeyModifiers[6] == 1);
    CHECK(t.xtermKeyModifiers[7] == 1);
    feed(t, "\033[?5;6;7m");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer) == "\033[>5;0m\033[>6;1m\033[>7;1m");
    feed(t, "\033[>4;2m\033[>4;9m");
    CHECK(t.xtermKeyModifiers[4] == 2);
    feed(t, "\033[>4;1f\033[>4;2f");
    CHECK(t.xtermKeyFormats[4] == 1);
}

// XTQMODKEYS answers once per resource it names, each reply shaped as the set
// control that restores it; an omitted or unknown resource reports DEFAULT.
TEST(testXTQMODKEYSAnswersEachResourceNamed)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[>1;1m\033[>4;2m");
    feed(t, "\033[?1;4m");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strcmp(outputBuffer, "\033[>1;1m\033[>4;2m") == 0);
    feed(t, "\033[?m\033[?8m");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::string_view(outputBuffer) == "\033[>65535;65535m\033[>8;65535m");
}

TEST(testDECIDRepliesLikeDa1)
{
    Ubiquitty::Terminal t;
    char da1[64], decid[64];
    int n1, n2;
    Ubiquitty::hardReset(t);
    feed(t, "\033[c");
    n1 = drain(t, da1);
    feed(t, "\033Z");
    n2 = drain(t, decid);
    CHECK(n1 > 0);
    CHECK_EQ(n1, n2);
    CHECK(memcmp(da1, decid, (size_t)n1) == 0);
}

TEST(testDECRQMAnsiReports)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[20h"); // LNM on
    (void)drain(t, outputBuffer);
    feed(t, "\033[20$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[20;1$y") == 0);
    feed(t, "\033[20l"); // LNM off
    (void)drain(t, outputBuffer);
    feed(t, "\033[20$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[20;2$y") == 0);
}

TEST(testDECRQMDecReports)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?7h"); // DECAWM on
    (void)drain(t, outputBuffer);
    feed(t, "\033[?7$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[?7;1$y") == 0);
    feed(t, "\033[?7l");
    (void)drain(t, outputBuffer);
    feed(t, "\033[?7$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[?7;2$y") == 0);
}

TEST(testDECRQMDecReportsAddedModes)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);

    // ?2031 — color-scheme notification.
    feed(t, "\033[?2031h");
    (void)drain(t, outputBuffer);
    feed(t, "\033[?2031$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[?2031;1$y") == 0);
    feed(t, "\033[?2031l");
    (void)drain(t, outputBuffer);
    feed(t, "\033[?2031$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[?2031;2$y") == 0);

    // ?12 — AT&T 610 cursor blink (defaults reset).
    feed(t, "\033[?12$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[?12;2$y") == 0);
    feed(t, "\033[?12h");
    (void)drain(t, outputBuffer);
    feed(t, "\033[?12$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[?12;1$y") == 0);
    feed(t, "\033[?12l");
    (void)drain(t, outputBuffer);
    feed(t, "\033[?12$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[?12;2$y") == 0);

    // ?40 — allow 80<->132 (defaults reset).
    feed(t, "\033[?40$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[?40;2$y") == 0);
    feed(t, "\033[?40h");
    (void)drain(t, outputBuffer);
    feed(t, "\033[?40$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[?40;1$y") == 0);
    feed(t, "\033[?40l");
    (void)drain(t, outputBuffer);
    feed(t, "\033[?40$p");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033[?40;2$y") == 0);
}

TEST(testDECRQCRAChecksum)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H"); // home
    feed(t, "AB");
    (void)drain(t, outputBuffer);
    // DECRQCRA: Pid=1, Ppage=1, Ptop=1, Pleft=1, Pbottom=1, Pright=2.
    feed(t, "\033[1;1;1;1;1;2*y");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    CHECK(strcmp(outputBuffer, "\033P1!~FF7D\033\\") == 0);
}

TEST(testDECRQCRAAllMemoryIgnoresRectangleAndOrigin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 5);
    feed(t, "ABC\r\nDEF");
    char reference[64] = {};
    feed(t, "\033[7;1;1;1;3;5*y");
    const int referenceLength = drain(t, reference);
    CHECK(referenceLength > 0);
    feed(t, "\033[2;3r\033[?69h\033[2;4s\033[?6h");
    for (const char* query : { "\033[7;0;3;5;1;1*y", "\033[7;;1;1;1;1*y", "\033[7*y" }) {
        char reply[64] = {};
        feed(t, query);
        CHECK(drain(t, reply) == referenceLength);
        CHECK(strcmp(reply, reference) == 0);
    }
}

TEST(testDECRQCRAInvertedRectanglesAreIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (const char* query : { "\033[1;1;2;1;1;3*y", "\033[1;1;1;3;2;1*y" }) {
        char reply[64] = {};
        feed(t, query);
        CHECK(drain(t, reply) == 0);
    }
    // An invalid request does not leave the reply channel or parser stuck.
    char reply[64] = {};
    feed(t, "\033[6n");
    CHECK(drain(t, reply) > 0);
    CHECK(strcmp(reply, "\033[1;1R") == 0);
}

TEST(testXTCHECKSUMControlsCalculationAndResets)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 6);
    const auto expect = [&](const char* sequence, const char* response) {
        char output[128] {};
        feed(t, sequence);
        (void)drain(t, output);
        CHECK(std::string_view(output) == response);
    };
    expect("ABC\033[1#y\033[1;1;1;1;1;3*y", "\033P1!~00C6\033\\");
    expect("\033[0#y\033[2;1;1;1;1;3*y", "\033P2!~FF3A\033\\");
    expect("\033[1#y\033[#y\033[3;1;1;1;1;3*y", "\033P3!~FF3A\033\\");
    // VT100 attributes contribute only when bit 1 is clear.
    expect("\033[H\033[1mA\033[0m\033[1#y\033[4;1;1;1;1;1*y", "\033P4!~00C1\033\\");
    expect("\033[3#y\033[5;1;1;1;1;1*y", "\033P5!~0041\033\\");
    // An explicit space and untouched cells differ when bit 3 is selected.
    expect("\033[2J\033[HA \033[7#y\033[6;1;1;1;1;3*y", "\033P6!~0081\033\\");
    expect("\033[15#y\033[7;1;1;1;1;3*y", "\033P7!~0061\033\\");
    expect("\033[3#y\033[8;1;1;1;1;3*y", "\033P8!~0041\033\\");
    // Bit 4 preserves both the full scalar and its combining characters.
    expect("\033[2J\033[H\xCE\xA9\xCC\x81\033[19#y\033[9;1;1;1;1;1*y", "\033P9!~06AA\033\\");
    expect("\033[3#y\033[10;1;1;1;1;1*y", "\033P10!~00A9\033\\");
    expect("\033[!p\033[11;1;1;1;1;1*y", "\033P11!~F956\033\\");
    expect("\033[1#y\033cABC\033[12;1;1;1;1;3*y", "\033P12!~FF3A\033\\");
}

TEST(testDecreqtparmReplies)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[x");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(outputBuffer[0] == 0x1B && outputBuffer[1] == '[');
    CHECK(outputBuffer[n - 1] == 'x');
}

TEST(testDECXCPRReportsWithPage)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[5;7H");
    feed(t, "\033[?6n");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    // DECXCPR carries the page as a third parameter, which is what separates it
    // from the DSR 6n reply asserted above.
    CHECK_CSTR_EQ(outputBuffer, "\033[?5;7;1R");
}

TEST(testDsr6nUnderDECOMReportsRelative)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?6h"); // DECOM
    feed(t, "\033[5;20r"); // DECSTBM 5..20
    feed(t, "\033[1;1H"); // CUP region origin
    feed(t, "\033[6n");
    n = drain(t, outputBuffer);
    outputBuffer[n] = '\0';
    // Under DECOM the report is relative to the region, so a cursor at the
    // region origin answers 1;1 rather than its absolute row.
    CHECK_CSTR_EQ(outputBuffer, "\033[1;1R");
}

TEST(testDa1NonzeroParameterSilent)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1c");
    n = drain(t, outputBuffer);
    CHECK(n == 0);
}

TEST(testXTWINOPSQueryLongTitleReportsFull)
{
    Ubiquitty::Terminal t;
    char outputBuffer[512];
    Ubiquitty::hardReset(t);
    std::string longTitle(200, 'Q'); // past 160 bytes, within the 256-byte response queue
    std::string setSeq = "\033]2;" + longTitle + "\007";
    feed(t, setSeq.c_str());
    CHECK(t.titleUTF8 == longTitle); // under the 4096 cap, set in full
    feed(t, "\033[21t");
    int n = drain(t, outputBuffer);
    std::string expected = "\033]l" + longTitle + "\033\\";
    CHECK(n == (int)expected.size()); // full reply, not suppressed
    CHECK(std::string(outputBuffer, n) == expected);
}

TEST(testDECUDKGatedByCapabilityPolicy)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        fakeCapabilityPolicy.wasCalled = false;
        fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
        t.capabilityPolicy = &fakeCapabilityPolicy;
        feed(t, "\033P1;1|17/4142\033\\"); // try to program F6 -> "AB"
        CHECK(fakeCapabilityPolicy.wasCalled);
        CHECK(fakeCapabilityPolicy.lastCapability == Ubiquitty::Capability::ProgramKeys);
        CHECK(Ubiquitty::ensureUserDefinedKeys(t).count == 0); // denied -> no key programmed
        t.capabilityPolicy = nullptr;
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Allow;
        t.capabilityPolicy = &fakeCapabilityPolicy;
        feed(t, "\033P1;1|17/4142\033\\");
        CHECK(Ubiquitty::ensureUserDefinedKeys(t).count == 1); // allowed -> key programmed
        CHECK(Ubiquitty::ensureUserDefinedKeys(t).entries[0].keyCode == 17);
        t.capabilityPolicy = nullptr;
    }
}

TEST(testXtwinops18ReportsCellGridSize)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 30, 100);
    feed(t, "\033[18t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[8;30;100t") == 0);
}

TEST(testXTWINOPS19ReportsScreenCharacters)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 30, 100);
    feed(t, "\033[19t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[9;30;100t") == 0);
}

TEST(testXtwinops11ReportsWindowState)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[11t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[1t") == 0);
}

TEST(testXTWINOPSPixelReportsUseCellSize)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.cellPixelWidth = 9;
    t.cellPixelHeight = 18;

    feed(t, "\033[16t"); // cell size: CSI 6 ; H ; W t
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[6;18;9t") == 0);

    feed(t, "\033[14t"); // text area: CSI 4 ; H ; W t
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;432;720t") == 0); // 24*18 ; 80*9

    // Screen size reports the published display dims, not the text area.
    t.screenPixelWidth = 1600;
    t.screenPixelHeight = 900;
    feed(t, "\033[15t"); // screen size: CSI 5 ; H ; W t
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[5;900;1600t") == 0);
}

TEST(testXTWINOPSPixelReportsNominalFallback)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t); // cellPixelWidth/H zeroed → 8x16 nominal
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[16t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[6;16;8t") == 0);
}

TEST(testPublishedPixelGeometrySaturatesEveryDimension)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::publishPixelGeometry(terminal, { { -1, -2 }, { -3, -4 }, { -5, -6 } });
    CHECK(terminal.cellPixelWidth == 0);
    CHECK(terminal.cellPixelHeight == 0);
    CHECK(terminal.textAreaPixelWidth == 0);
    CHECK(terminal.textAreaPixelHeight == 0);
    CHECK(terminal.drawablePixelWidth == 0);
    CHECK(terminal.drawablePixelHeight == 0);

    Ubiquitty::publishPixelGeometry(terminal, { { 65536, 65537 }, { 65538, 65539 }, { 65540, 65541 } });
    CHECK(terminal.cellPixelWidth == 65535);
    CHECK(terminal.cellPixelHeight == 65535);
    CHECK(terminal.textAreaPixelWidth == 65535);
    CHECK(terminal.textAreaPixelHeight == 65535);
    CHECK(terminal.drawablePixelWidth == 65535);
    CHECK(terminal.drawablePixelHeight == 65535);
    char outputBuffer[128];
    feed(terminal, "\033[16t\033[14t\033[?7402h\033[14t");
    CHECK(drain(terminal, outputBuffer) > 0);
    CHECK(strcmp(outputBuffer, "\033[6;65535;65535t\033[4;65535;65535t\033[4;65535;65535t") == 0);

    Ubiquitty::publishPixelGeometry(terminal, { { 9, 18 }, { 711, 429 }, { 727, 445 } });
    feed(terminal, "\033[16t\033[14t\033[?7402l\033[14t");
    CHECK(drain(terminal, outputBuffer) > 0);
    CHECK(strcmp(outputBuffer, "\033[6;18;9t\033[4;445;727t\033[4;429;711t") == 0);
}

TEST(testXTWINOPSTextAreaReportsTrueDrawnExtent)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.cellPixelWidth = 9; // per-cell product 80*9 = 720 (the drift-prone basis)
    t.cellPixelHeight = 18; // 24*18 = 432
    t.textAreaPixelWidth = 711; // published true extent, distinct from 720
    t.textAreaPixelHeight = 429; // distinct from 432

    feed(t, "\033[14t"); // text area: reports the published extent, not 432;720
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;429;711t") == 0);

    feed(t, "\033[16t"); // cell size still uses cellPixelWidth — unchanged
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[6;18;9t") == 0);

    // No published extent → 14t falls back to the per-cell product (other frontends).
    t.textAreaPixelWidth = 0;
    t.textAreaPixelHeight = 0;
    feed(t, "\033[14t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;432;720t") == 0);
}

TEST(testXTWINOPSTextAreaUnderFullscreenContentIsTheDrawable)
{
    // Under DECSET 7402 a fitted image fills the whole drawable from its
    // corner, whatever the letterbox, so the extent a program is told is the
    // drawable while the mode holds: told the grid, it reasons about a space
    // about a percent smaller than the picture. Decided by Core at the
    // report, from both published facts, so a mode that flips between two
    // publishes is answered at once.
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.cellPixelWidth = 9;
    t.cellPixelHeight = 18;
    t.textAreaPixelWidth = 711;
    t.textAreaPixelHeight = 429;
    t.drawablePixelWidth = 727; // the grid plus the padding the mode paints over
    t.drawablePixelHeight = 445;

    feed(t, "\033[14t"); // out of the mode: the grid, as before
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;429;711t") == 0);

    feed(t, "\033[?7402h\033[14t"); // the mode: the drawable
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;445;727t") == 0);

    feed(t, "\033]7402;fill=pass\033\\\033[14t"); // under either letterbox
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;445;727t") == 0);

    // A frontend that never measured a drawable keeps reporting what it has.
    t.drawablePixelWidth = 0;
    t.drawablePixelHeight = 0;
    feed(t, "\033[14t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;429;711t") == 0);

    t.drawablePixelWidth = 727;
    t.drawablePixelHeight = 445;
    feed(t, "\033[?7402l\033[14t"); // the reset restores the grid
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;429;711t") == 0);
}

TEST(testXTWINOPSPixelReportsSurviveRIS)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.cellPixelWidth = 9;
    t.cellPixelHeight = 18;
    t.textAreaPixelWidth = 711;
    t.textAreaPixelHeight = 429;
    t.drawablePixelWidth = 731;
    t.drawablePixelHeight = 449;
    t.screenPixelWidth = 1600;
    t.screenPixelHeight = 900;
    t.winPosX = 120;
    t.winPosY = 64;

    feed(t, "\033c"); // RIS

    // Geometry preserved on the Terminal.
    CHECK(t.cellPixelWidth == 9);
    CHECK(t.cellPixelHeight == 18);
    CHECK(t.textAreaPixelWidth == 711);
    CHECK(t.textAreaPixelHeight == 429);
    CHECK(t.drawablePixelWidth == 731);
    CHECK(t.drawablePixelHeight == 449);
    CHECK(t.screenPixelWidth == 1600);
    CHECK(t.screenPixelHeight == 900);
    CHECK(t.winPosX == 120);
    CHECK(t.winPosY == 64);

    // And the reports use the live cell, not the nominal fallback.
    feed(t, "\033[16t"); // cell size: CSI 6 ; H ; W t
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[6;18;9t") == 0);

    feed(t, "\033[14t");
    n = drain(t, outputBuffer);
    REQUIRE(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;429;711t") == 0);

    feed(t, "\033[?7402h\033[14t");
    n = drain(t, outputBuffer);
    REQUIRE(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;449;731t") == 0);

    feed(t, "\033[13t"); // position: CSI 3 ; x ; y t
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[3;120;64t") == 0);
}

TEST(test_xtwinops_geometry_reports_gated_by_allowWindowOps)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.cellPixelWidth = 9;
    t.cellPixelHeight = 18;
    t.screenPixelWidth = 1600;
    t.screenPixelHeight = 900;
    t.winPosX = 120;
    t.winPosY = 64;

    /// Gate off: every geometry query stays silent (no respond).
    Ubiquitty::Modes::HostPolicy::setWindowPolicy(&t, Ubiquitty::OperationPolicy::Denied);
    feed(t, "\033[13t"); // position
    CHECK(drain(t, outputBuffer) == 0);
    feed(t, "\033[14t"); // text-area pixels
    CHECK(drain(t, outputBuffer) == 0);
    feed(t, "\033[15t"); // screen pixels
    CHECK(drain(t, outputBuffer) == 0);
    feed(t, "\033[16t"); // cell pixels
    CHECK(drain(t, outputBuffer) == 0);

    /// Gate on: each query reports the live geometry.
    Ubiquitty::Modes::HostPolicy::setWindowPolicy(&t, Ubiquitty::OperationPolicy::Allowed);
    feed(t, "\033[13t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[3;120;64t") == 0);

    feed(t, "\033[14t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[4;432;720t") == 0); // 24*18 ; 80*9

    feed(t, "\033[15t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[5;900;1600t") == 0);

    feed(t, "\033[16t");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[6;18;9t") == 0);
}

TEST(testDECRPKTKeyTypeReport)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);

    // Station 42 (main typewriter block) → alphanumeric (type 0).
    feed(t, "\033[42,u");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strcmp(outputBuffer, "\033[42;0,v") == 0);

    // Station 100 (function/keypad area) → function (type 1).
    feed(t, "\033[100,u");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[100;1,v") == 0);

    // Station 0 / omitted → alphanumeric default.
    feed(t, "\033[,u");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[0;0,v") == 0);
}

TEST(testXTVERSIONQueryResponds)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[>q");
    const int n = drain(t, outputBuffer);
    REQUIRE(n > 0);
    // The name and the version this binary was built as, which the build
    // derives from its commit; a fixed number would name no build at all.
    const std::string expected = "\033P>|Ubiquitty(" + std::string(Ubiquitty::buildVersion()) + ")\033\\";
    CHECK_STR_EQ(std::string(outputBuffer, static_cast<std::size_t>(n)), expected);
    feed(t, "\033[>0q");
    CHECK_STR_EQ(std::string(outputBuffer, static_cast<std::size_t>(drain(t, outputBuffer))), expected);
}

TEST(testRISPreservesCapabilityGateForRemainingBytes)
{
    CaptureShell shell;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.shellClient = &shell;
    fakeCapabilityPolicy.wasCalled = false;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    t.capabilityPolicy = &fakeCapabilityPolicy;
    t.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Remote;

    feed(t, "\033c\033]9;must stay denied\007");

    CHECK(fakeCapabilityPolicy.wasCalled);
    CHECK(fakeCapabilityPolicy.lastCapability == Ubiquitty::Capability::DesktopNotification);
    CHECK(fakeCapabilityPolicy.lastOrigin == Ubiquitty::CapabilityOrigin::Remote);
    CHECK(shell.notifyCalls == 0);
    t.capabilityPolicy = nullptr;
    t.shellClient = nullptr;
}

TEST(testDa1Primary)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[c");
    n = drain(t, outputBuffer);
    // Ubiquitty defaults to the VT500 conformance level (65) and advertises only
    // what it implements: 132 columns (1), printer port (2), Sixel (4),
    // selective erase (6), soft character set (7), UDK (8), NRCS (9), DEC
    // technical set (15), locator (16/29, Core/Mouse/Locator.cpp), terminal
    // state interrogation (17, csiH_decrqtsr), user windows (18), horizontal
    // scrolling (21), ANSI color (22), rectangular editing (28) — 44 bytes.
    // A capability advertised here that no handler answers blocks a host on a
    // report that never arrives, so this list and the handlers move together.
    // The leading device class tracks the DECSCL level (test_decscl_sets_level_and_da1).
    CHECK(n == 44);
    CHECK(outputBuffer[0] == 0x1B);
    CHECK(outputBuffer[1] == '[');
    CHECK(outputBuffer[2] == '?');
    CHECK(outputBuffer[n - 1] == 'c');
    CHECK(strcmp(outputBuffer, "\033[?65;1;2;4;6;7;8;9;15;16;17;18;21;22;28;29c") == 0);

    // ESC [ 0 c is canonical and must produce the same response.
    feed(t, "\033[0c");
    n = drain(t, outputBuffer);
    CHECK(n == 44);
    CHECK(strcmp(outputBuffer, "\033[?65;1;2;4;6;7;8;9;15;16;17;18;21;22;28;29c") == 0);
}

TEST(testDa2Secondary)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[>c");
    n = drain(t, outputBuffer);
    // DA2's model tracks the conformance level DA1 reports — at the default
    // VT500 that is 64 (VT520). A fixed model would contradict DA1 and gate a
    // host's feature detection to the wrong terminal class. 387 is a modern
    // firmware/patch level so "xterm >= N" cap gates pass; ROM cartridge 0.
    CHECK(n == 12);
    CHECK(strcmp(outputBuffer, "\033[>64;387;0c") == 0);

    // `> 0 c` is the explicit variant and must produce the same reply.
    feed(t, "\033[>0c");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[>64;387;0c") == 0);

    // DECSCL down to VT400 moves the model with it.
    feed(t, "\033[64;1\"p");
    feed(t, "\033[>c");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[>41;387;0c") == 0);

    // Per xterm: a non-zero Ps is reserved for DA2 — no reply (matches DA1's
    // Ps==0 guard, csiH_da).
    feed(t, "\033[>5c");
    n = drain(t, outputBuffer);
    CHECK(n == 0);
}

TEST(testDa3Tertiary)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[=c");
    n = drain(t, outputBuffer);
    // DA3's reply is a DCS string, so it opens with ESC P and closes with ST.
    // Those four bytes are the frame; a reply carrying no identifier between
    // them satisfies every byte check below and says nothing.
    CHECK(n > 4);
    CHECK(outputBuffer[0] == 0x1B);
    CHECK(outputBuffer[1] == 'P');
    CHECK(outputBuffer[n - 2] == 0x1B);
    CHECK(outputBuffer[n - 1] == '\\');

    // Per xterm a non-zero Ps is reserved, so it draws no reply — the same
    // guard DA1 applies in csiH_da.
    feed(t, "\033[=5c");
    n = drain(t, outputBuffer);
    CHECK(n == 0);
}

TEST(testDSR5nOK)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[5n");
    n = drain(t, outputBuffer);
    // CSI 0 n = "terminal is in good operating condition".
    CHECK(n == 4);
    CHECK(strcmp(outputBuffer, "\033[0n") == 0);
}

TEST(testDsr6nCursorPosition)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    // CUP 5;7 — 1-based input puts the cursor at row 5 column 7. DSR 6 n
    // must report those same 1-based coordinates back.
    feed(t, "\033[5;7H");
    feed(t, "\033[6n");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strcmp(outputBuffer, "\033[5;7R") == 0);

    // Move to (6,8) and re-check.
    feed(t, "\033[6;8H");
    feed(t, "\033[6n");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[6;8R") == 0);
}

TEST(testDsrUnknownPsSilent)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[99n");
    n = drain(t, outputBuffer);
    CHECK(n == 0);

    // Mix with a known DSR right after to confirm the parser state
    // isn't stuck.
    feed(t, "\033[5n");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[0n") == 0);
}

TEST(testFocusReportingDefaultOff)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.focusReporting == false);
}

TEST(testFocusReportingSetReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?1004h");
    CHECK(t.modes.focusReporting == true);
    feed(t, "\033[?1004l");
    CHECK(t.modes.focusReporting == false);
}

TEST(testFocusReportingEmitsCSIIO)
{
    Ubiquitty::Terminal t;
    char outputBuffer[16];
    int n;
    Ubiquitty::hardReset(t);
    /// Without DECSET 1004, focusChanged is silent.
    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Focused);
    n = drain(t, outputBuffer);
    CHECK(n == 0);
    // Enable focus reporting; focus-in → CSI I, focus-out → CSI O.
    feed(t, "\033[?1004h");
    CHECK(t.modes.focusReporting == true);
    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Focused);
    n = drain(t, outputBuffer);
    CHECK(n == 3);
    CHECK(strcmp(outputBuffer, "\033[I") == 0);
    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Unfocused);
    n = drain(t, outputBuffer);
    CHECK(n == 3);
    CHECK(strcmp(outputBuffer, "\033[O") == 0);
    // Disable and verify silence.
    feed(t, "\033[?1004l");
    Ubiquitty::Modes::focusChanged(&t, Ubiquitty::Modes::FocusState::Focused);
    n = drain(t, outputBuffer);
    CHECK(n == 0);
}

TEST(testDecrqm1048ReportsSaveCursorState)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];
    int n;

    // Fresh term: no save held → reset (2), not unrecognized (0).
    feed(t, "\033[?1048$p");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strcmp(outputBuffer, "\033[?1048;2$y") == 0);

    // DECSET 1048 saves the cursor → report set (1).
    feed(t, "\033[?1048h");
    CHECK(t.screens.active->hasSavedCursor == true);
    feed(t, "\033[?1048$p");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strcmp(outputBuffer, "\033[?1048;1$y") == 0);
}

TEST(testDecrqm38ReportsTekModeState)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];
    int n;

    // VT mode by default → reset (2), never unrecognized (0).
    feed(t, "\033[?38$p");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strcmp(outputBuffer, "\033[?38;2$y") == 0);

    // DECSET 38 enters Tek mode and sets the flag the report reads.
    feed(t, "\033[?38h");
    CHECK(t.isTekModeActive == true);
}

TEST(testDECSCLSetsLevelAndDa1Reflects)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    // Default level is VT500 → DA1 reports ?65;...
    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT500);
    feed(t, "\033[c");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strncmp(outputBuffer, "\033[?65;", 6) == 0);

    // Set VT300 (63), 7-bit (Pc=1) → transmit8bitC1 false. With 7-bit
    // transmission the DA1 reply keeps the literal ESC [ CSI prefix, so
    // the device-class digits are directly observable.
    feed(t, "\033[63;1\"p");
    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT300);
    CHECK(t.transmit8bitC1 == false);
    feed(t, "\033[c");
    n = drain(t, outputBuffer);
    CHECK(strncmp(outputBuffer, "\033[?63;", 6) == 0);

    // Set VT200 (62), 8-bit (Pc=0) → transmit8bitC1 true. The DA1
    // reply now collapses ESC [ into the single 8-bit CSI byte 0x9B,
    // followed by the device class.
    feed(t, "\033[62;0\"p");
    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT200);
    CHECK(t.transmit8bitC1 == true);
    feed(t, "\033[c");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK((unsigned char)outputBuffer[0] == 0x9B);
    CHECK(strncmp(outputBuffer + 1, "?62;", 4) == 0);

    // VT100 (61) forces 7-bit and the classic device class.
    feed(t, "\033[61\"p");
    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT100);
    CHECK(t.transmit8bitC1 == false);
    feed(t, "\033[c");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1;2c") == 0);

    // RIS restores the default level.
    feed(t, "\033c");
    CHECK(t.conformanceLevel == Ubiquitty::VT::ConformanceLevel::VT500);
}

TEST(testXTSMGRAPHICSReplies)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);

    // Pi=1, Pa=1 (read current registers) → 256.
    feed(t, "\033[?1;1;0S");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strcmp(outputBuffer, "\033[?1;0;256S") == 0);

    // Pi=1, Pa=4 (read maximum registers) → 256.
    feed(t, "\033[?1;4;0S");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1;0;256S") == 0);

    // Pi=2, Pa=1 (read current geometry) → 1000×1000.
    feed(t, "\033[?2;1;0S");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?2;0;1000;1000S") == 0);

    // Pi=2, Pa=4 (read maximum geometry) → 1000×1000.
    feed(t, "\033[?2;4;0S");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?2;0;1000;1000S") == 0);

    // Pi=3 (ReGIS) is an unknown item in a build without ReGIS (Ps=1).
    feed(t, "\033[?3;1;0S");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?3;1S") == 0);

    // Unknown Pi → bad-Pi error (Ps=1).
    feed(t, "\033[?9;1;0S");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?9;1S") == 0);

    // Bad Pa for a valid Pi → bad-Pa error (Ps=2), no value.
    feed(t, "\033[?1;9;0S");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1;2S") == 0);

    // The no-prefix CSI Ps S (SU / scroll-up) must stay distinct: it
    // scrolls and emits no reply. Place text on row 0, scroll up 1, and
    // confirm nothing is written to the host.
    feed(t, "\033[H");
    feed(t, "Z");
    feed(t, "\033[1S");
    n = drain(t, outputBuffer);
    CHECK(n == 0);
}

TEST(testDECRQSSReportsEachConformanceLevel)
{
    Ubiquitty::Terminal t;
    char outputBuffer[160];
    Ubiquitty::hardReset(t);
    /// Match the level-bearing DCS payload `1$r<level>;<pc>"p`, which is
    /// prefix-agnostic: the DCS / ST framing collapses to 8-bit bytes when
    /// transmit8bitC1 is on (Pc=0), so the payload substring is the stable
    /// part to assert on. Pc echoes the C1-transmission select (1=7-bit,
    /// 2=8-bit); VT100 forces 7-bit.
    const struct {
        const char* set;
        const char* expect;
    } cases[] = {
        { "\033[61\"p", nullptr }, // VT100 does not implement DECRQSS
        { "\033[62;0\"p", "1$r62;2\"p" }, // VT200, 8-bit
        { "\033[63;1\"p", "1$r63;1\"p" }, // VT300, 7-bit
        { "\033[64;0\"p", "1$r64;2\"p" }, // VT400, 8-bit
        { "\033[65;1\"p", "1$r65;1\"p" }, // VT500, 7-bit
    };
    for (const auto& c : cases) {
        feed(t, c.set);
        feed(t, "\033P$q\"p\033\\"); // DECRQSS "p
        int n = drain(t, outputBuffer);
        if (c.expect) {
            CHECK(n > 0);
            CHECK(strstr(outputBuffer, c.expect) != nullptr);
        } else
            CHECK(n == 0);
    }
}

TEST(testDECRQSSDECSCUSRReportsResolvedStyle)
{
    Ubiquitty::Terminal t;
    char outputBuffer[128];
    /// No DECSCUSR issued; the user configured a blinking bar.
    Ubiquitty::hardReset(t);
    t.cursorShapePref = 2; // bar
    t.cursorBlinkPref = true;
    feed(t, "\033P$q q\033\\"); // DECRQSS SP q
    int n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strstr(outputBuffer, "1$r5 q") != nullptr); // blinking bar, not the 0 sentinel
    CHECK(strstr(outputBuffer, "1$r0 q") == nullptr);

    /// Steady-underline user pref (no blink) → code 4.
    Ubiquitty::hardReset(t);
    t.cursorShapePref = 1; // underline
    t.cursorBlinkPref = false;
    feed(t, "\033P$q q\033\\");
    n = drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "1$r4 q") != nullptr);

    /// An explicit DECSCUSR style reports verbatim (blinking underline = 3).
    Ubiquitty::hardReset(t);
    feed(t, "\033[3 q");
    feed(t, "\033P$q q\033\\");
    n = drain(t, outputBuffer);
    CHECK(strstr(outputBuffer, "1$r3 q") != nullptr);
}

TEST(testDECTABSRReportsCustomStops)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    int n;
    Ubiquitty::hardReset(t);
    // Clear every tab stop (TBC 3), then set stops at columns 3, 6, 10
    // (1-based) by moving the cursor there and issuing HTS (ESC H).
    feed(t, "\033[3g");
    int i;
    for (i = 0; i < t.columns; i++)
        CHECK(!t.tabStops[i]);
    feed(t, "\033[3G\033H"); // column 3 (CHA), HTS
    feed(t, "\033[6G\033H"); // column 6
    feed(t, "\033[10G\033H"); // column 10
    feed(t, "\033[2$w");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    // Expect: DCS 2 $ u 3/6/10 ST
    CHECK(strcmp(outputBuffer, "\033P2$u3/6/10\033\\") == 0);
}

// The report answers in full on a grid whose stops outgrow the small reply
// buffer, because its length is the grid's to decide and not the buffer's.
//
// Two channels carry host-facing replies. The small one is a 256-byte buffer
// and `respond` drops anything longer WHOLE rather than truncating — the right
// choice for a fixed-size reply, since a DCS cut in half has an introducer and
// no terminator and leaves a host waiting inside a string it can never end. But
// a dropped reply is a query that never answers, and this report's length is a
// function of how many tab stops the grid holds: a stop in every column of a
// maximum-width grid is four thousand bytes. So it goes through the growable
// channel, as XTGETTCAP does for the same reason.
//
// The case drives the widest reply the grid can produce, which is also the one
// the handler's static assertion sizes its formatting buffer for. It fails if
// the reply is dropped, and it fails if the reply arrives cut short.
TEST(testTheTabStopReportAnswersInFullAtEveryColumn)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 24, Ubiquitty::maxColumns, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    CHECK_MSG(t.columns == Ubiquitty::maxColumns, "the grid took the width this case is about");

    // Clear the eight-column defaults, then set a stop in every column.
    feed(t, "\033[3g");
    for (int column = 1; column <= t.columns; column++) {
        char move[32];
        std::snprintf(move, sizeof(move), "\033[%dG\033H", column);
        feed(t, move);
    }

    feed(t, "\033[2$w");
    std::vector<char> outputBuffer(64 * 1024, '\0');
    const int count = drain(t, outputBuffer);
    CHECK_MSG(count > 256, "a reply past the small buffer is carried rather than dropped");

    const std::string reply(outputBuffer.data(), static_cast<std::size_t>(count > 0 ? count : 0));
    CHECK_MSG(reply.starts_with("\033P2$u"), "it opens with the DECTABSR introducer");
    CHECK_MSG(reply.ends_with("\033\\"), "and closes with the string terminator, so nothing was cut short");
    const std::string lastStop = "/" + std::to_string(Ubiquitty::maxColumns) + "\033\\";
    CHECK_MSG(reply.ends_with(lastStop), "and the final column is the last one named, so every stop fitted");
}

TEST(testDECSDMDECRQMReportsState)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);
    // Default is reset → DECRQM reports ?80;2$y.
    CHECK(t.modes.sixelDisplayMode == false);
    feed(t, "\033[?80$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?80;2$y") == 0);

    // Set ?80 → flag true, DECRQM reports ?80;1$y.
    feed(t, "\033[?80h");
    CHECK(t.modes.sixelDisplayMode == true);
    feed(t, "\033[?80$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?80;1$y") == 0);

    // Reset again.
    feed(t, "\033[?80l");
    CHECK(t.modes.sixelDisplayMode == false);
}

// DECRQSS reports an underline's shape as the parameter that sets it, so the
// reply restores the pen rather than flattening every shape to a single line.
TEST(testSGRReportKeepsTheUnderlineShape)
{
    Ubiquitty::Terminal t;
    char outputBuffer[256];
    for (const auto& [set, want] : { std::pair { "\033[21m", "\033P1$r0;21m\033\\" }, std::pair { "\033[4:3m", "\033P1$r0;4:3m\033\\" },
             std::pair { "\033[4m", "\033P1$r0;4m\033\\" } }) {
        Ubiquitty::hardReset(t);
        feed(t, set);
        feed(t, "\033P$qm\033\\");
        drain(t, outputBuffer);
        CHECK_MSG(std::string_view(outputBuffer) == want, set);
    }
}

// XTVERSION answers `CSI > q` and `CSI > 0 q` only.
TEST(testXTVERSIONAnswersOnlyParameterZero)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[128];
    feed(t, "\033[>1q");
    CHECK(drain(t, outputBuffer) == 0);
    feed(t, "\033[>0q");
    CHECK(drain(t, outputBuffer) > 0);
}

// DECCIR's Sflag carries origin mode in its lowest bit and the pending
// autowrap in its fourth, as the VT510 lays them out.
TEST(testDECCIRFlagsPutOriginModeFirst)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);
    char outputBuffer[128];
    feed(t, "\033[?6h\033[1$w");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer).starts_with("\033P1$u1;1;1;@;@;A;"));
    feed(t, "\033[?6lABCD\033[1$w");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer).starts_with("\033P1$u1;4;1;@;@;H;"));
}

// DECCIR reports G2 in GR until a locking shift invokes another set, and the
// set a locking shift invoked after it.
TEST(testDECCIRReportsG2InGRByDefault)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[128];
    feed(t, "\033[1$w");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1$u1;1;1;@;@;@;0;2;@;BBBB\033\\");
    feed(t, "\033|\033[1$w"); // LS3R
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1$u1;1;1;@;@;@;0;3;@;BBBB\033\\");
}

// DECRSPS puts back the tab stops a DECTABSR reported.
TEST(testDECRSPSRestoresTabStops)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 10);
    char outputBuffer[128];
    feed(t, "\033[3g\033P2$t3/7\033\\\r\tX\tY");
    CHECK(cell(t, 0, 2)->codePoint == 'X');
    CHECK(cell(t, 0, 6)->codePoint == 'Y');
    feed(t, "\033[2$w");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P2$u3/7\033\\");
}

// DECRSPS puts back the cursor state a DECCIR reported, and a malformed report
// changes nothing.
TEST(testDECRSPSRestoresCursorInformation)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    char reported[128];
    feed(t, "\033)0\033[1;4m\033[1\"q\033[3;5H\016\033[1$w");
    drain(t, reported);
    std::string restore(reported);
    REQUIRE(restore.starts_with("\033P1$u"));
    restore[3] = '$';
    restore[4] = 't';
    Ubiquitty::hardReset(t);
    feed(t, restore.c_str());
    char again[128];
    feed(t, "\033[1$w");
    drain(t, again);
    CHECK(std::string_view(again) == std::string_view(reported));
    feed(t, "\033P1$t9;9;1;@;@;@;0;2;@;BBB\033\\\033[1$w"); // one designation short
    drain(t, again);
    CHECK(std::string_view(again) == std::string_view(reported));
}

// DECCIR marks a set designated as a 96-character set in Scss, so the report
// puts back the set it names.
TEST(testDECCIRMarksNinetySixCharacterSets)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char reported[128];
    feed(t, "\033.A\033[1$w"); // ISO Latin-1 into G2
    drain(t, reported);
    CHECK(std::string_view(reported) == "\033P1$u1;1;1;@;@;@;0;2;D;BBAB\033\\");
    std::string restore(reported);
    restore[3] = '$';
    restore[4] = 't';
    Ubiquitty::hardReset(t);
    feed(t, restore.c_str());
    char again[128];
    feed(t, "\033[1$w");
    drain(t, again);
    CHECK(std::string_view(again) == std::string_view(reported));
}

// CRM is a mode DECRQM knows: reported reset, and still reset after a
// program sets it, since controls are always performed.
TEST(testControlRepresentationModeReportsReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];
    feed(t, "\033[3$p");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033[3;2$y");
    feed(t, "\033[3h\033[3$p");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033[3;2$y");
}

// The private operating-status request answers "no malfunction", as the
// ANSI form does.
TEST(testPrivateOperatingStatusReportsNoMalfunction)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[64];
    feed(t, "\033[?5n");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033[?0n");
}

// DECRQSS reports the page width DECSCPP sets.
TEST(testDECRQSSReportsColumnsPerPage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 12);
    char outputBuffer[64];
    feed(t, "\033P$q$|\033\\");
    drain(t, outputBuffer);
    CHECK(std::string_view(outputBuffer) == "\033P1$r12$|\033\\");
}

// DECSCL with Pl left out names no level: refused, and nothing is reset.
// DECRQDE is a VT300 control and is not answered at level 1.
TEST(testDECSCLWithoutALevelAndDECRQDEAtLevelOne)
{
    Ubiquitty::Terminal t;
    char outputBuffer[96];
    Ubiquitty::hardReset(t);
    const auto level = t.conformanceLevel;
    feed(t, "\033[4h\033[\"p");
    CHECK(t.conformanceLevel == level);
    CHECK(t.modes.insertMode);
    feed(t, "\033[61\"p\033[\"v");
    CHECK(drain(t, outputBuffer) == 0);
}

UBIQUITTY_TEST_MAIN()
// ---- DECRQM: the state digit is a contract, and it is self-checkable -------
//
// A program asks DECRQM what a mode is doing and decides what it may send from
// the answer. The digit is the whole message: 0 unrecognised, 1 set, 2 reset,
// 3 permanently set, 4 permanently reset. A mode answering 0 where it means 2
// tells the program the terminal has never heard of it, and the program stops
// using a feature that works.
//
// Asserted against the terminal's own behaviour rather than against a table of
// expected digits: a settable mode must answer 1 after SM and 2 after RM, and a
// permanent one must answer the same digit whatever SM/RM did — which is what
// "permanent" means. A table would have to be edited alongside every new mode
// and would then agree with whatever the code did.

namespace {

// The reply's state digit for one DECRQM query, or -1 when nothing was sent.
// `prefix` is "" for ANSI modes and "?" for DEC private ones.
int decrqmState(Ubiquitty::Terminal& t, const char* prefix, int ps)
{
    char outputBuffer[256];
    (void)drain(t, outputBuffer); // clear anything queued before the query
    char query[32];
    std::snprintf(query, sizeof query, "\033[%s%d$p", prefix, ps);
    feed(t, query);
    const int written = drain(t, outputBuffer);
    if (written <= 0)
        return -1;
    // CSI [?] Ps ; Pv $ y — take the digit after the semicolon.
    const char* semicolon = std::strchr(outputBuffer, ';');
    if (!semicolon || !semicolon[1])
        return -1;
    return semicolon[1] - '0';
}

} // namespace

// Modes Core implements and reports by their own state, where xterm reports
// them unrecognised, permanently reset or inverted: autorepeat, the VT510's
// no-clear-on-column-change, synchronised output, logging, the alternate
// screen's enable and sixel display mode.
TEST(testModesCoreImplementsReportTheirState)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(decrqmState(t, "?", 8) == 1);
    feed(t, "\033[?8l");
    CHECK(decrqmState(t, "?", 8) == 2);
    CHECK(decrqmState(t, "?", 95) == 2);
    feed(t, "\033[?95h");
    CHECK(decrqmState(t, "?", 95) == 1);
    CHECK(decrqmState(t, "?", 2026) == 2);
    feed(t, "\033[?2026h");
    CHECK(decrqmState(t, "?", 2026) == 1);
    feed(t, "\033[?2026l");
    CHECK(decrqmState(t, "?", 46) == 2);
    feed(t, "\033[?1046h");
    CHECK(decrqmState(t, "?", 1046) == 1);
    feed(t, "\033[?80h");
    CHECK(decrqmState(t, "?", 80) == 1);
}

// xterm's resource modes: 1014, 1040 and 1041 are settable and start as
// xterm's resources do; 13, 1020, 1021 and 1022 report a setting a program
// cannot change; 14 is permanently reset, and 1035 selects NumLock handling.
TEST(testXtermResourceModesReport)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(decrqmState(t, "?", 1014) == 1);
    CHECK(decrqmState(t, "?", 1040) == 1);
    CHECK(decrqmState(t, "?", 1041) == 2);
    feed(t, "\033[?1014l\033[?1040l\033[?1041h");
    CHECK(decrqmState(t, "?", 1014) == 2);
    CHECK(decrqmState(t, "?", 1040) == 2);
    CHECK(decrqmState(t, "?", 1041) == 1);
    t.cursorBlinkPref = false;
    feed(t, "\033[?13h\033[?1020l\033[?1021h");
    CHECK(decrqmState(t, "?", 13) == 2);
    CHECK(decrqmState(t, "?", 1020) == 1);
    CHECK(decrqmState(t, "?", 1021) == 2);
    CHECK(decrqmState(t, "?", 1022) == 1);
    feed(t, "\033[?14h\033[?1035h");
    CHECK(decrqmState(t, "?", 14) == 4);
    CHECK(decrqmState(t, "?", 1035) == 1);
}

TEST(testDECRQMAnswersEveryModeItRecognises)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // ANSI modes the terminal implements: settable, so the digit has to track
    // SM and RM.
    const int settableANSI[] = { 2, 4, 12, 20 };
    for (const int ps : settableANSI) {
        char set[16];
        char reset[16];
        std::snprintf(set, sizeof set, "\033[%dh", ps);
        std::snprintf(reset, sizeof reset, "\033[%dl", ps);
        feed(t, set);
        CHECK_MSG(decrqmState(t, "", ps) == 1, "a set mode reports set");
        feed(t, reset);
        CHECK_MSG(decrqmState(t, "", ps) == 2, "a reset mode reports reset");
    }

    // ANSI modes recognised but permanently reset — the printer, transfer and
    // area-qualification modes a character-imaging terminal has no component
    // for. "Permanently" is the claim under test: SM must not move them.
    const int permanentlyResetANSI[] = { 1, 5, 7, 10, 11, 13, 14, 15, 16, 17, 18, 19 };
    for (const int ps : permanentlyResetANSI) {
        CHECK_MSG(decrqmState(t, "", ps) == 4, "a permanently reset mode reports 4");
        char set[16];
        std::snprintf(set, sizeof set, "\033[%dh", ps);
        feed(t, set);
        CHECK_MSG(decrqmState(t, "", ps) == 4, "setting a permanently reset mode does not move it");
    }

    // An ANSI mode number nobody defines: unrecognised, not "reset".
    CHECK(decrqmState(t, "", 9999) == 0);
    CHECK(decrqmState(t, "", 6) == 0); // ERM: not implemented here

    // DEC private modes, the same three ways.
    const int settableDEC[] = { 1, 3, 4, 5, 6, 7, 8, 12, 25, 35, 40, 42, 44, 45, 66, 67, 81,
        1004, 1005, 1006, 1007, 1010, 1011, 1015, 1016, 1034, 1036, 1037, 1039,
        1042, 1043, 1044, 1046, 2004, 2005, 2006, 2026, 2031, 2048, 7402, 7403 };
    feed(t, "\033[?40h"); // DECCOLM (3) is settable only while mode 40 allows it
    for (const int ps : settableDEC) {
        char set[24];
        char reset[24];
        std::snprintf(set, sizeof set, "\033[?%dh", ps);
        std::snprintf(reset, sizeof reset, "\033[?%dl", ps);
        feed(t, set);
        const int whenSet = decrqmState(t, "?", ps);
        feed(t, reset);
        const int whenReset = decrqmState(t, "?", ps);
        // Every one of these is a real, distinct mode: it must answer both ways
        // rather than the same digit regardless.
        CHECK_MSG(whenSet == 1 || whenSet == 2, "a recognised DEC mode reports a real state");
        CHECK_MSG(whenReset == 1 || whenReset == 2, "a recognised DEC mode reports a real state");
        CHECK_MSG(whenSet != whenReset, "a settable DEC mode's report follows the mode");
    }

    // Modes that answer from state a test must not casually enter (VT52, Tek,
    // the mouse tracking modes that change every subsequent byte): queried
    // rather than toggled, because the digit for "reset" is the half a program
    // reads before deciding to enable them at all.
    const int queryOnlyDEC[] = { 2, 38, 95, 1000, 1001, 1002, 1003, 7400, 46, 41, 10, 30, 1045, 1050, 7401 };
    for (const int ps : queryOnlyDEC) {
        const int state = decrqmState(t, "?", ps);
        CHECK_MSG(state >= 0 && state <= 4, "a queried DEC mode answers a defined digit");
    }

    // Permanently set: grapheme clustering is the only path attaching combining
    // marks to a cell, so it cannot be turned off and must say so.
    CHECK(decrqmState(t, "?", 2027) == 3);
    feed(t, "\033[?2027l");
    CHECK(decrqmState(t, "?", 2027) == 3);

    // Permanently reset: the transmit rate is the host's.
    CHECK(decrqmState(t, "?", 73) == 4);
    feed(t, "\033[?73h");
    CHECK(decrqmState(t, "?", 73) == 4);

    // The cursor coupling modes are held: a reset reads back as reset and a
    // set as set.
    for (const int ps : { 60, 61, 64 }) {
        feed(t, "\033[?" + std::to_string(ps) + "l");
        CHECK(decrqmState(t, "?", ps) == 2);
        feed(t, "\033[?" + std::to_string(ps) + "h");
        CHECK(decrqmState(t, "?", ps) == 1);
    }

    // The sweep above left every settable mode RESET, including 1046 — the
    // alternate-screen feature enable. With it reset, 1049 cannot switch, which
    // is itself the contract: the feature gate outranks the switch.
    feed(t, "\033[?1049h");
    CHECK_MSG(decrqmState(t, "?", 47) == 2, "with the feature disabled, 1049 does not switch");
    feed(t, "\033[?1046h");

    // Modes whose report reads a state the alternate-screen machinery owns.
    feed(t, "\033[?1049h");
    CHECK(decrqmState(t, "?", 47) == 1);
    CHECK(decrqmState(t, "?", 1047) == 1);
    CHECK(decrqmState(t, "?", 1049) == 1);
    feed(t, "\033[?1049l");
    CHECK(decrqmState(t, "?", 47) == 2);
    feed(t, "\0337");
    CHECK(decrqmState(t, "?", 1048) == 1); // a saved cursor exists

    // Tek 4014 answers through the same table, and is the one mode whose report
    // has to survive the mode switching the parser out from under it.
    CHECK(decrqmState(t, "?", 38) == 2);

    // An undefined DEC mode: unrecognised.
    CHECK(decrqmState(t, "?", 31337) == 0);
}

// DECRQM arrived with the VT300. Below that operating level the request is not
// recognised at all, so the terminal stays silent rather than answering 0 —
// a program probing an old level must see no reply, which is how it learns the
// request itself is unavailable.
TEST(testDECRQMIsSilentBelowVT300)
{
    for (const char* level : { "\033[61\"p", "\033[62\"p" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, level);
        char outputBuffer[128];
        (void)drain(t, outputBuffer);
        feed(t, "\033[4$p"); // ANSI form
        CHECK(drain(t, outputBuffer) == 0);
        feed(t, "\033[?25$p"); // DEC form
        CHECK(drain(t, outputBuffer) == 0);
    }

    // At VT300 and above both forms answer again.
    for (const char* level : { "\033[63\"p", "\033[64\"p", "\033[65\"p" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, level);
        CHECK(decrqmState(t, "", 4) >= 0);
        CHECK(decrqmState(t, "?", 25) >= 0);
    }
}

// DA2's model number must track the operating level DA1 reports. Answering a
// VT420 model under a VT500 level contradicts our own DA1, and a host keying
// feature detection off DA2 would gate away everything above VT420.
//
// The reply's INTRODUCER is part of the same answer: DECSCL selects the C1
// transmission policy alongside the level, and at VT200 and above the default
// is 8-bit, so the reply opens with 0x9B rather than ESC [. A host parser that
// only knows the two-byte form loses the whole reply, so both halves are
// pinned here together.
TEST(testDA2ModelAndIntroducerTrackTheConformanceLevel)
{
    struct Case {
        const char* decscl;
        int model;
        bool eightBitC1;
    };
    const Case cases[] = {
        { "\033[61\"p", 0, false }, // VT100 — no 8-bit transmission at all
        { "\033[62\"p", 1, true }, // VT220, Pc defaulted: 8-bit
        { "\033[63\"p", 24, true }, // VT320
        { "\033[64\"p", 41, true }, // VT420
        { "\033[65\"p", 64, true }, // VT520
        { "\033[65;1\"p", 64, false }, // VT520, Pc=1: 7-bit requested
        { "\033[65;2\"p", 64, true }, // VT520, Pc=2: 8-bit, like the default
    };
    for (const Case& c : cases) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, c.decscl);
        char outputBuffer[128];
        (void)drain(t, outputBuffer);
        feed(t, "\033[>c");
        const int written = drain(t, outputBuffer);
        CHECK(written > 0);
        // The introducer: ESC [ when 7-bit, the single 0x9B when 8-bit.
        const char* body = nullptr;
        if (c.eightBitC1) {
            CHECK_MSG(static_cast<unsigned char>(outputBuffer[0]) == 0x9B,
                "an 8-bit level answers with the single-byte CSI");
            body = outputBuffer + 1;
        } else {
            CHECK_MSG(outputBuffer[0] == '\033' && outputBuffer[1] == '[',
                "a 7-bit level answers with ESC [");
            body = outputBuffer + 2;
        }
        char expected[24];
        std::snprintf(expected, sizeof expected, ">%d;", c.model);
        CHECK_MSG(std::strncmp(body, expected, std::strlen(expected)) == 0,
            "the DA2 model tracks the operating level");
    }

    // A non-zero Ps is reserved for both DA forms, so neither replies.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[128];
    (void)drain(t, outputBuffer);
    feed(t, "\033[1c");
    CHECK(drain(t, outputBuffer) == 0);
    feed(t, "\033[>1c");
    CHECK(drain(t, outputBuffer) == 0);

    // An out-of-range level leaves the terminal where it was, rather than
    // half-applying a soft reset for a level it then refuses.
    Ubiquitty::Terminal keeps;
    Ubiquitty::hardReset(keeps);
    feed(keeps, "\033[62\"p");
    feed(keeps, "\033[99\"p"); // past VT500
    (void)drain(keeps, outputBuffer);
    feed(keeps, "\033[>c");
    CHECK(drain(keeps, outputBuffer) > 0);
    CHECK(std::strncmp(outputBuffer + 1, ">1;", 3) == 0); // still VT220
}

// The DEC-private DSR statuses: a terminal with no printer, unlocked keys and a
// North American keyboard answers each with its own fixed statement.
TEST(testDECDSRFixedStatuses)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[128];

    const struct {
        const char* query;
        const char* reply;
    } cases[] = {
        { "\033[?15n", "\033[?13n" }, // no printer
        { "\033[?25n", "\033[?20n" }, // UDKs unlocked
        { "\033[?26n", "\033[?27;1;0;0n" }, // keyboard: North American, ready
    };
    for (const auto& c : cases) {
        (void)drain(t, outputBuffer);
        feed(t, c.query);
        CHECK(drain(t, outputBuffer) > 0);
        CHECK_MSG(std::strcmp(outputBuffer, c.reply) == 0, "the fixed status answers exactly");
    }

    // The capability statements: each answers a fixed sentence about a feature
    // this terminal does or does not carry, and ?63 carries the page id the
    // request named back in its reply.
    const struct {
        const char* query;
        const char* reply;
    } fixed[] = {
        { "\033[?62n", "\033[0*{" }, // DECMSR: macro space
        { "\033[?75n", "\033[?70n" }, // data integrity: no errors
        { "\033[?85n", "\033[?83n" }, // multiple sessions
    };
    for (const auto& c : fixed) {
        (void)drain(t, outputBuffer);
        feed(t, c.query);
        const int written = drain(t, outputBuffer);
        CHECK_MSG(written > 0, "a capability query is answered");
        if (written > 0)
            CHECK_MSG(std::strncmp(outputBuffer, c.reply, std::strlen(c.reply)) == 0,
                "the capability statement is the documented one");
    }

    // DECCKSR (?63) echoes the page id it was given. (A negative id cannot
    // arrive: the parser refuses `-` in a parameter, so the guard normalising
    // one is unreachable from the wire — see the ledger.)
    for (const char* query : { "\033[?63n", "\033[?63;2n", "\033[?63;7n" }) {
        (void)drain(t, outputBuffer);
        feed(t, query);
        const int written = drain(t, outputBuffer);
        CHECK(written > 0);
        if (written > 0)
            CHECK_MSG(std::strstr(outputBuffer, "!~") != nullptr, "the checksum report keeps its DCS shape");
    }

    // The colour-scheme report (?996n) answers from the resolved default
    // background, so it says light or dark rather than reading a mode.
    (void)drain(t, outputBuffer);
    feed(t, "\033[?996n");
    const int schemeWritten = drain(t, outputBuffer);
    CHECK(schemeWritten > 0);
    if (schemeWritten > 0)
        CHECK_MSG(std::strncmp(outputBuffer, "\033[?997;", 7) == 0, "the scheme report keeps its documented shape");
}

// Cursor position is reported relative to the scroll region under origin mode,
// and relative to the left margin as well once DECLRMM is on. Both reports
// floor at 1: a cursor that sits outside the region (the region moved under it)
// must not be reported at row or column zero, which no program expects.
TEST(testCursorReportsAreRegionRelativeAndFloorAtOne)
{
    const struct {
        const char* query;
        char terminator;
    } forms[] = { { "\033[6n", 'R' }, { "\033[?6n", 'R' } };

    for (const auto& form : forms) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 24, 80);
        char outputBuffer[128];

        // Absolute: row 10, column 20.
        feed(t, "\033[10;20H");
        (void)drain(t, outputBuffer);
        feed(t, form.query);
        CHECK(drain(t, outputBuffer) > 0);
        CHECK(std::strstr(outputBuffer, "10;20") != nullptr);

        // Region-relative: a top margin at row 5 and origin mode make the same
        // cell report as row 6.
        feed(t, "\033[5;20r\033[?6h\033[6;10H");
        (void)drain(t, outputBuffer);
        feed(t, form.query);
        CHECK(drain(t, outputBuffer) > 0);
        CHECK_MSG(std::strstr(outputBuffer, ";10") != nullptr, "the column is unchanged without DECLRMM");

        // With DECLRMM the column is left-margin-relative too.
        feed(t, "\033[?69h\033[10;40s\033[?6h\033[3;5H");
        (void)drain(t, outputBuffer);
        feed(t, form.query);
        const int written = drain(t, outputBuffer);
        CHECK(written > 0);
        // Whatever the arithmetic, the report never falls below 1;1 — the
        // floor is what a program's cursor-save/restore depends on.
        // Margins moved out from under the cursor: the region-relative
        // arithmetic goes negative and the report floors at 1;1 rather than
        // naming a row or column no program can address.
        feed(t, "\033[?69h\033[?6h\033[1;10s\033[1;40r\033[2;3H\033[30;70s\033[20;24r");
        (void)drain(t, outputBuffer);
        feed(t, form.query);
        const int floored = drain(t, outputBuffer);
        CHECK(floored > 0);
        int flooredRow = 0;
        int flooredColumn = 0;
        {
            const char* scan = outputBuffer;
            while (*scan && (*scan < '0' || *scan > '9'))
                ++scan;
            std::sscanf(scan, "%d;%d", &flooredRow, &flooredColumn);
        }
        CHECK_MSG(flooredRow >= 1 && flooredColumn >= 1, "the report never falls below 1;1");

        int row = 0;
        int column = 0;
        const char* digits = std::strchr(outputBuffer, '[');
        CHECK(digits != nullptr);
        if (digits) {
            const char* scan = digits + 1;
            while (*scan && (*scan < '0' || *scan > '9'))
                ++scan;
            std::sscanf(scan, "%d;%d", &row, &column);
        }
        CHECK(row >= 1 && column >= 1);
        CHECK(outputBuffer[written - 1] == form.terminator);
    }
}

// ---- residue ledger: VT/Reports.cpp ----------------------------------
// What stays dark in Reports.cpp after the cases above, and why each arm
// cannot execute rather than merely does not:
//
// - The cursor-report floors (`row = 1` / `column = 1` after the
//   region-relative subtraction): the modes that make the report
//   region-relative are the same ones that confine the cursor to the region.
//   DECOM's handler homes the cursor to `scrollTop` and then calls
//   cursorClampOrigin (`setPrivateMode` in Modes.cpp), and DECSTBM homes it too, so
//   `cursorY + 1 - scrollTop` cannot fall below 1 while origin mode is on.
// - DECCKSR's negative-page-id normalisation: a CSI parameter is a run of
//   digits, so the parser never produces a negative one — `\033[?63;-4n` is
//   refused as malformed before the handler sees it, rather than arriving as
//   -4.

// ---- DECCIR reports the designations that were made ------------------
//
// The cursor information report's last field lists the SCS designation final
// for G0..G3. Every existing case leaves all four at USASCII, so the whole
// mapping answered 'B' by falling through its default and no wrong answer
// could show. Asserted as a ROUND TRIP — designate with a final byte, ask,
// expect that byte back — so the case cannot agree with a mapping that is
// merely self-consistent.
TEST(testDECCIRReportsEveryDesignationItAccepts)
{
    // Each 94-set designation final, in the ESC ( form, and the byte DECCIR
    // owes back for it. The pairs where DEC allows two designators for one set
    // (the Nordic ones) are listed under both, because a terminal accepting a
    // designator it cannot name back is the failure this catches.
    const struct {
        const char* designate;
        char reported;
    } sets[] = {
        { "\033(0", '0' }, // DEC Special Graphics
        { "\033(A", 'A' }, // UK
        { "\033(R", 'R' }, // French
        { "\033(f", 'R' }, // French, the other designator
        { "\033(K", 'K' }, // German
        { "\033(Y", 'Y' }, // Italian
        { "\033(E", 'E' }, // Norwegian/Danish
        { "\033(6", 'E' },
        { "\033(H", 'H' }, // Swedish
        { "\033(7", 'H' },
        { "\033(C", 'C' }, // Finnish
        { "\033(5", 'C' },
        { "\033(4", '4' }, // Dutch
        { "\033(=", '=' }, // Swiss
        { "\033(Z", 'Z' }, // Spanish
        { "\033(<", '<' }, // DEC Supplemental
        { "\033(>", '>' }, // DEC Technical
        { "\033(B", 'B' }, // USASCII
    };

    for (const auto& set : sets) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        char outputBuffer[160];
        feed(t, set.designate);
        (void)drain(t, outputBuffer);
        feed(t, "\033[1$w"); // DECCIR
        const int written = drain(t, outputBuffer);
        CHECK(written > 0);
        // The designation list is the last field before ST: four finals.
        const char* body = std::strstr(outputBuffer, "$u");
        CHECK(body != nullptr);
        if (!body || written < 6)
            continue;
        // ...G0 G1 G2 G3 ESC backslash
        CHECK_MSG(outputBuffer[written - 6] == set.reported, "DECCIR names the set that was designated");
        // The slots nobody designated stay USASCII.
        CHECK(outputBuffer[written - 5] == 'B');
        CHECK(outputBuffer[written - 4] == 'B');
        CHECK(outputBuffer[written - 3] == 'B');
    }

    // The 96-set designations land in their own slots and report their own
    // finals. Latin-1 shares 'A' with UK, which is correct: the 96-set
    // intermediate is what distinguishes them on the wire, not the final.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        char outputBuffer[160];
        feed(t, "\033-A"); // Latin-1 into G1, 96-set
        feed(t, "\033.<"); // DEC Supplemental into G2, 96-set
        feed(t, "\033+>"); // DEC Technical into G3, 94-set (it has no 96-set form)
        (void)drain(t, outputBuffer);
        feed(t, "\033[1$w");
        const int written = drain(t, outputBuffer);
        CHECK(written > 0);
        if (written >= 6) {
            CHECK(outputBuffer[written - 5] == 'A');
            CHECK(outputBuffer[written - 4] == '<');
            CHECK(outputBuffer[written - 3] == '>');
        }
    }

    // A 96-set designator naming a set that has no 96-set form is ignored
    // rather than designated: the slot keeps what it had, and DECCIR says so.
    // Accepting it would put a 94-set in a slot whose GR mapping assumes 96
    // positions, so the wrong glyph would draw for every byte above 0xA0.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        char outputBuffer[160];
        feed(t, "\033/>"); // DEC Technical has no 96-set designation
        (void)drain(t, outputBuffer);
        feed(t, "\033[1$w");
        const int written = drain(t, outputBuffer);
        CHECK(written > 0);
        if (written >= 6)
            CHECK_MSG(outputBuffer[written - 3] == 'B', "a 96-set designator for a 94-set-only set is refused");
    }

    // An unknown selector draws no reply at all — DECRQPSR defines 1 and 2,
    // and a program asking for a third must not be handed a DECCIR it will
    // parse as an answer to its own question.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        char outputBuffer[160];
        for (const char* query : { "\033[0$w", "\033[3$w", "\033[99$w" }) {
            (void)drain(t, outputBuffer);
            feed(t, query);
            CHECK_MSG(drain(t, outputBuffer) == 0, "an unknown presentation-state selector is silent");
        }
        // And the two defined ones still answer.
        feed(t, "\033[1$w");
        CHECK(drain(t, outputBuffer) > 0);
        feed(t, "\033[2$w");
        CHECK(drain(t, outputBuffer) > 0);
    }
}

// DECRQSS `,~` is DECPS's: not a setting but the extension's bounds, the
// request's grammar with every parameter at its top and then the sample bank,
// so a program can tell the tracker from a speaker before it sends a score the
// speaker would play on one voice. Exact bytes, since a program compares them
// literally; a build without the extension answers `DCS 0 $ r ST`.
TEST(testDECRQSSDECPSReportsTheExtensionsBounds)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[160];
    feed(t, "\033P$q,~\033\\");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "\033P1$r7:8:6:48:5000:5000:7:5000:20000:20000:7:64;255:255:255:255;49:7:15:255;25:10,~\033\\") != nullptr);
}

// A program that wants a chime has no way to learn that nobody will hear it: a
// terminal with no synth and one whose user turned sound off both consume the
// request in silence. `CSI ? 7410 n` is that answer, and it carries the bank's
// occupancy so the follow-up question costs no second round trip.
TEST(testSoundStatusReportsWhetherAnyoneWillHearIt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[160];
    // No frontend installed a synth in a Core-only build, which is the truthful
    // answer rather than a hopeful one: state 0, no volume, an empty bank.
    feed(t, "\033[?7410n");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "\033[?7410;0;0;0n") != nullptr);

    // A filled slot shows in the mask, bit 0 standing for slot 1.
    Ubiquitty::VT::SampleBank& bank = Ubiquitty::ensureSampleBank(t);
    bank.slots[0] = std::vector<int16_t>(8, 1);
    bank.slots[4] = std::vector<int16_t>(8, 1);
    feed(t, "\033[?7410n");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "\033[?7410;0;0;17n") != nullptr); // slots 1 and 5
}

// The reattach question. A session outlives the program that filled the bank, so
// a full slot is not evidence that it holds the caller's own sample; the tag it
// downloaded with is.
TEST(testSoundSlotForTagAnswersWhoseSampleASlotHolds)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[160];
    feed(t, "\033[?7411;4660n");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "\033[?7411;4660;0n") != nullptr); // nothing holds it yet

    Ubiquitty::VT::SampleBank& bank = Ubiquitty::ensureSampleBank(t);
    bank.slots[2] = std::vector<int16_t>(8, 1);
    bank.tags[2] = 4660;
    feed(t, "\033[?7411;4660n");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "\033[?7411;4660;3n") != nullptr);

    // Another program's tag is not this one's sample, however full the slot is.
    feed(t, "\033[?7411;1n");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(strstr(outputBuffer, "\033[?7411;1;0n") != nullptr);
}

TEST(testWidthAndEncodingResourcesDescribeTheActualDecoderAndCells)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.ambiguousWidth = Ubiquitty::Unicode::Width::AmbiguousWidth::Wide;
    feed(t, "\033[?1023h\033[?1023l");
    CHECK(decrqmState(t, "?", 1023) == 1);
    feed(t, "\356\200\200"); // U+E000, a private-use glyph
    CHECK(t.screens.active->cursorX == 1);
    feed(t, "\033%@");
    CHECK(decrqmState(t, "?", 1020) == 2);
    feed(t, "\033%G");
    CHECK(decrqmState(t, "?", 1020) == 1);
}
