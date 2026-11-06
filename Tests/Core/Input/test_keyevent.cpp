// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Core/Input/test_keyevent.cpp — portable key-event encoder tests.
//
// Exercises Ubiquitty::Keys::Keymap::encodeKeyEvent directly with synthesized
// KeyEvent records. No platform dependency; the assertions describe
// the wire output every frontend (macOS, Linux) must emit
// for a given portable input.

#include "Core/Keys/KeyEvent.h"
#include "Core/Keys/Keymap.h"
#include "Core/Modes/TerminalModes.h"

#include <cstdio>
#include <cstring>
#include <span>
using Ubiquitty::Keys::KeyEvent;
using Ubiquitty::Keys::KeyEventType;
using namespace Ubiquitty::Keys::Keymap;
namespace Kitty = Ubiquitty::Kitty;

#define EXPECT_BYTES(buffer, length, expected, expectedLength)                                                             \
    do {                                                                                                                   \
        if ((length) != (expectedLength) || std::memcmp((buffer), (expected), (expectedLength)) != 0) {                    \
            char ubqMessage[128];                                                                                          \
            std::snprintf(ubqMessage, sizeof(ubqMessage), "bytes mismatch (got %d, want %d)", (length), (expectedLength)); \
            CHECK_MSG(false, ubqMessage);                                                                                  \
        }                                                                                                                  \
    } while (0)

// Forwarder that supplies the three sibling modify-key levels (cursor /
// function / keypad) as 0 -- the historical default. Tests that exercise
// those levels call encodeKeyEvent directly.
static int encode(const KeyEvent& ev, Ubiquitty::Modes::CursorKeyMode cursorKeyMode, Ubiquitty::Kitty::KeyFlagSet kittyFlags, uint8_t modifyOther, uint8_t formatOther, std::span<char> output) { return Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, cursorKeyMode, kittyFlags, modifyOther, formatOther, 0, 0, 0, output); } // bool-ok: mirrors Keymap::encodeKeyEvent's own parameter — a helper taking an enum would stop exercising the signature under test.

static KeyEvent makeEvent(uint32_t keyCode, uint32_t modifiers)
{
    KeyEvent ev;
    ev.keyCode = keyCode;
    ev.shiftedKey = 0;
    ev.baseLayoutKey = 0;
    ev.modifiers = modifiers;
    ev.eventType = KeyEventType::Press;
    ev.textLength = 0;
    for (int i = 0; i < Ubiquitty::Keys::maxKeyEventTextLength; ++i)
        ev.textCodePoints[i] = 0;
    return ev;
}

// VT52 mode: cursor keys transmit bare ESC A/B/C/D (not SS3/CSI).
// LNM (mode 20): Return transmits CR+LF when set, CR only when reset.
// --- Kitty shifted-base-codepoint (kitty-keyboard-shifted-base-codepoint):
// the Kitty PRIMARY key is the unshifted base; the shifted glyph rides the
// alternate (only under ReportAlternate). The LEGACY byte stays the shifted
// glyph, so non-Kitty apps get the same bytes either way (switching keyCode alone
// to the base would change them).
static KeyEvent makeShiftedEvent(uint32_t base, uint32_t shifted)
{
    KeyEvent ev = makeEvent(base, keymapModifierShift);
    ev.shiftedKey = shifted;
    return ev;
}

// --- The kitty PRIMARY-key contract still holds where CSI u is actually used:
// under ReportAll (flag 8, "report all keys as escape codes") a Shift+key is an
// escape code, and its primary key is the UNSHIFTED base — not the shifted glyph.
// ============================================================================
// REGRESSION (kitty spec): a text-producing key is reported as a CSI u escape
// code only under ReportAll (flag 8) or with a NON-shift modifier. Shift alone,
// and no modifier, always send UTF-8 TEXT — even under Disambiguate /
// ReportAlternate / ReportEvents / ReportText. Encoding Shift+key as CSI u for
// apps that request only the lower flags breaks typing ':' in vim, capitals in
// TUIs, atuin editing, etc., while zsh (a legacy, no-flags app) is unaffected.
// ============================================================================

// Associated text (ReportText) must never carry a control code — a ctrl-combo's
// OS-produced C0 byte (Ctrl+A -> 0x01) is not "text" and must not leak into the
// third parameter group, where an app reads it as a spurious inserted char.
// Event types (ReportEvents) are only reported for keys reported as escape
// codes. A text key (no ReportAll, shift-only/unmodified) has no release
// channel — its key-up must not leak a CSI u release.
// A text key has no release channel, but OS auto-repeat of the same key is
// not a release — it is typematic repeat: the byte resent while the key is
// held. Under a lower kitty flag tier (Disambiguate |
// ReportEvents, no ReportAll — exactly what neovim's builtin kitty-keyboard
// support requests) a held 'a' must keep producing 'a', or nvim sees repeat
// as if it stopped entirely.
// ============================================================================
// Kitty-mode conveniences and spec details.
// ============================================================================

// A modifier key (left control = Doom fire) reports press and release as
// CSI-u under REPORT_ALL | REPORT_EVENTS, the path flagsChanged: drives.
// ---- modifyCursorKeys / modifyFunctionKeys / modifyKeypadKeys ----
//
// Levels stored on Ubiquitty::Terminal via CSI > 1|2|3 ; Pv m and applied by the
// encoder. Level 0/2 emit the default CSI 1 ; mod form; level 1 drops
// the leading "1"; level 3 emits the private CSI > 1 ; mod form.

static int encodeLevel(const KeyEvent& ev, Ubiquitty::Modes::CursorKeyMode cursorKeyMode, uint8_t cursorLevel, uint8_t functionLevel, uint8_t keypadLevel, std::span<char> output) { return Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, cursorKeyMode, {}, 0, 0, cursorLevel, functionLevel, keypadLevel, output); } // bool-ok: mirrors Keymap::encodeKeyEvent's own parameter — a helper taking an enum would stop exercising the signature under test.

// ---- DECPAM / DECNKM unmodified application-keypad SS3 ----
//
// With applicationKeypad set, an UNMODIFIED keypad key emits ESC O <final>;
// this is the macOS-promoted table, now shared, and the encoder's arg path.

static int encodeKeypad(const KeyEvent& ev, std::span<char> output) { return Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, 0, 0, 0, output, Ubiquitty::Keys::Keymap::EncodeModes { .applicationKeypad = true }); }

// The drift fix: keypad '+' (Add) is SS3 'k', not 'l' (which is the keypad
// comma/separator). Pin it so the macOS and shared tables never re-diverge.
// A modifier suppresses the unmodified-SS3 shortcut: with no modifyKeypadKeys
// level, a Shift+keypad key falls through to suppression (== the legacy path).
// Kitty mode supersedes the SS3 shortcut even when applicationKeypad is set.
// Pin the full shared table directly via applicationKeypadSS3().
// ---- Meta/Alternate keyboard discipline (xterm modes 1036 / 1039 / 1034) ----
//
// metaSendsEscape (1036) / alternateSendsEscape (1039) / eightBitInput (1034) are the
// last three optional args of encodeKeyEvent. They affect only how an Alternate/Meta-
// modified printable byte is carried: ESC prefix vs. the 8th-bit form.

static int encodeMeta(const KeyEvent& ev, const Ubiquitty::Keys::Keymap::EncodeModes& modes, std::span<char> output)
{
    return Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Normal, /*kittyFlags=*/ {}, 0, 0, 0, 0, 0, output, modes);
}

// 1036 on (the power-on default): Alternate+'a' sends ESC then 'a'.
// 1036 off + 1034 on: Alternate+'a' sets the 8th bit (0x61 | 0x80 == 0xE1).
// 1036 off + 1034 off: falls back to the ESC prefix (xterm eightBitInput=false).
// 1039 (alternateSendsEscape) alone forces the ESC prefix even with 1034 on.
// Ctrl+Alternate with eightBitInput (no escape mode): the folded control byte gets
// bit 7 set (Ctrl+P folds to 0x10; | 0x80 == 0x90).
// The eight-bit modes do not touch an unmodified key.
// ---- Delete key discipline (DEC private mode 1037, xterm deleteSendsDel) ----
//
// RemoveSequence (default): the Delete key sends the VT220 Remove sequence CSI 3 ~.
// SendsDelete: it sends DEL (0x7F).

enum class DeleteKeyMode {
    RemoveSequence,
    SendsDelete
};

static int encodeDelete(const KeyEvent& ev, DeleteKeyMode mode, std::span<char> output)
{
    return Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Normal, /*kittyFlags=*/ {}, 0, 0, 0, 0, 0, output, Ubiquitty::Keys::Keymap::EncodeModes { .deleteKeySendsDelete = (mode == DeleteKeyMode::SendsDelete) });
}

// 1037 reset (default): Delete → CSI 3 ~.
// 1037 set: Delete → DEL (0x7F).
// 1037 set but Delete carries a modifier: keep the explicit CSI 3 ; mod ~
// so the modifier survives (the DEL shortcut is unmodified-only).
// ---- DECUDK user-defined keys ----

static int encodeUDK(const KeyEvent& ev, const Ubiquitty::VT::UserDefinedKeys* udk, std::span<char> output) { return Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Normal, /*kittyFlags=*/ {}, 0, 0, 0, 0, 0, output, Ubiquitty::Keys::Keymap::EncodeModes {}, udk); }

// A programmed F6 (DEC code 17) emits its byte string in place of the default
// CSI 17 ~ sequence.
// An unprogrammed key falls through to the default sequence (F7 → CSI 18 ~).
// A modified press skips the UDK table (UDK has no modifier channel).
// A programmed Delete (DEC code 3) emits its string in place of CSI 3 ~.
TEST(testPlainLetter)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "a", 1);
}

TEST(testCtrlLetterFoldsToC0)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('k', keymapModifierCtrl);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    CHECK(n == 1);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x0B);
}

TEST(testOptLetterEmitsMetaPrefix)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('z', keymapModifierAlternate);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    CHECK(n == 2);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x1B);
    CHECK(outputBuffer[1] == 'z');
}

// --- A control chord on a non-Latin layout folds the base-layout key.
//
// Apple's Cyrillic, Greek and Hebrew layouts paint a letter on every ASCII
// position, and Ctrl+ф is what a Russian user presses for Ctrl+A. The letter's
// own code point has no byte form, so the frontend's base-layout key — the
// Latin letter on the same position — is what the control fold reads.
TEST(testCtrlOnANonLatinLetterFoldsTheBaseLayoutKey)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(0x0444, keymapModifierCtrl); // ф, on the A position
    ev.baseLayoutKey = 'a';
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    CHECK(n == 1);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x01);

    // Ctrl+Shift+ф folds the same: shift capitalises the letter, and the
    // legacy fold has one byte for both cases.
    KeyEvent shifted = makeEvent(0x0444, keymapModifierCtrl | keymapModifierShift);
    shifted.shiftedKey = 0x0424; // Ф
    shifted.baseLayoutKey = 'a';
    n = encode(shifted, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    CHECK(n == 1);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x01);

    // Ctrl+Alt+ф: the fold, behind the meta ESC prefix.
    KeyEvent meta = makeEvent(0x0444, keymapModifierCtrl | keymapModifierAlternate);
    meta.baseLayoutKey = 'a';
    n = encode(meta, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    CHECK(n == 2);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x1B);
    CHECK(static_cast<uint8_t>(outputBuffer[1]) == 0x01);
}

// --- A Super chord on a letter is the shell's, never text.
//
// ⌘C reaches the encoder only when no menu item and no binding claimed it —
// with nothing selected the Copy item is disabled and AppKit hands the press
// on — and the standard encoding must not type the letter there. Under the
// Kitty protocol the chord is reported, since a program may bind it.
TEST(testSuperOnALetterSendsNothingUnlessKittyReportsIt)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('c', keymapModifierSuper);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    CHECK(n == 0);

    KeyEvent shifted = makeEvent('c', keymapModifierSuper | keymapModifierShift);
    shifted.shiftedKey = 'C';
    n = encode(shifted, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    CHECK(n == 0);

    // Super is bit 8 of the protocol's modifier mask, so the parameter is 9.
    n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[99;9u", 7);
}

TEST(testCtrlOnANonLatinLetterWithoutABaseLayoutKeySendsNothing)
{
    // A platform that cannot say which position the letter sits on leaves the
    // base-layout key 0, and there is no byte that means Ctrl+ф: silence, not
    // the letter's raw code point truncated into one.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(0x0444, keymapModifierCtrl);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    CHECK(n == 0);
}

TEST(testKittyReportsANonLatinLetterWithItsBaseLayoutAlternate)
{
    // Under the protocol the letter stays the primary key and the position
    // rides beside it as the base-layout alternate — CSI 1092::97;5u — so a
    // program that binds Ctrl+A finds it in the alternate, as the spec says.
    char outputBuffer[24] = { 0 };
    KeyEvent ev = makeEvent(0x0444, keymapModifierCtrl);
    ev.baseLayoutKey = 'a';
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportAlternateKeys }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[1092::97;5u", 13);

    // Without the alternates flag the position is not reported and the letter
    // still is: the legacy fold is not a substitute for the protocol's form.
    n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[1092;5u", 9);

    // A bare letter under report-all is the letter, as an escape code, with
    // the position beside it and the modifier group anchoring the alternates.
    KeyEvent bare = makeEvent(0x0444, 0);
    bare.baseLayoutKey = 'a';
    n = encode(bare, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportAlternateKeys }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[1092::97;1u", 13);
}

TEST(testArrowLegacyNoDECCKM)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyLeft, 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[D", 3);
}

TEST(testVT52CursorKeys)
{
    struct {
        uint32_t key;
        const char* want;
    } cases[] = {
        { keymapKeyUp, "\033A" },
        { keymapKeyDown, "\033B" },
        { keymapKeyRight, "\033C" },
        { keymapKeyLeft, "\033D" },
    };
    for (auto& c : cases) {
        char outputBuffer[16] = { 0 };
        KeyEvent ev = makeEvent(c.key, 0);
        // cursorKeyMode true would otherwise force SS3 — vt52Mode must win.
        int n = Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Application, {}, 0, 0, 0, 0, 0, outputBuffer, Ubiquitty::Keys::Keymap::EncodeModes { .vt52Mode = true });
        EXPECT_BYTES(outputBuffer, n, c.want, 2);
    }
}

TEST(testLNMReturnCRLF)
{
    KeyEvent ev = makeEvent(keymapKeyEnter, 0);
    char outputBuffer[16] = { 0 };
    int n = Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, 0, 0, 0, outputBuffer, Ubiquitty::Keys::Keymap::EncodeModes { .lineFeedNewLineMode = true });
    EXPECT_BYTES(outputBuffer, n, "\r\n", 2);
    char out2[16] = { 0 };
    int n2 = Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, 0, 0, 0, out2, Ubiquitty::Keys::Keymap::EncodeModes {});
    EXPECT_BYTES(out2, n2, "\r", 1);
}

TEST(testArrowDECCKMUsesSs3)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyUp, 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Application, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1BOA", 3);
}

TEST(testArrowWithModifierUsesXtermForm)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyRight, keymapModifierShift);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[1;2C", 6);
}

TEST(testShiftTabEmitsCBT)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyTab, keymapModifierShift);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[Z", 3);
}

TEST(testFunctionKeyNoMod)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyF1, 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1BOP", 3);
}

TEST(testPageupLegacy)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyPageUp, 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[5~", 4);
}

TEST(testBackspaceLegacyEmitsDel)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyBackspace, 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    CHECK(n == 1);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x7F);
}

TEST(testOptLeftEmitsMetaB)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyLeft, keymapModifierAlternate);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n,
        "\x1B"
        "b",
        2);
}

TEST(testKittyDisambigPassesArrowLegacy)
{
    // DISAMBIGUATE alone must not promote arrow keys to CSI u — the
    // legacy CSI[A form is unambiguous and apps that only speak
    // legacy path recognises it too.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyDown, 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[B", 3);
}

TEST(testKittyDisambigPromotesTabToCSIU)
{
    // Tab is legacy-ambiguous (collides with Ctrl-I), so
    // DISAMBIGUATE moves it to the CSI u form.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyTab, 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[9u", 4);
}

TEST(testKittyReportAllPromotesPlainLetter)
{
    char outputBuffer[32] = { 0 };
    KeyEvent ev = makeEvent('q', 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::ReportAll, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[113;1u", 8);
}

TEST(testKittyReportAllKeepsArrowsNamed)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyDown, 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportAlternateKeys }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[B", 3);
}

TEST(testKittyReportAllKeepsModifiedArrowsNamed)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyUp, keymapModifierShift);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportAlternateKeys }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[1;2A", 6);
}

TEST(testKittyShiftedPunctPrimaryIsBase)
{
    // ReportAll + Shift+; → CSI u; primary is base ';' (59), not shifted ':' (58).
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent(';', ':'); // 59 base, 58 shifted
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::ReportAll, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[59;2u", 7);
}

TEST(testKittyShiftedBacktickPrimaryIsBase)
{
    // ReportAll + Shift+` → base '`' (96), not shifted '~' (126).
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('`', '~'); // 96 base, 126 shifted
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::ReportAll, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[96;2u", 7);
}

TEST(testKittyShiftedDigitPrimaryIsBase)
{
    // ReportAll + Shift+1 → base '1' (49), not shifted '!' (33).
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('1', '!'); // 49 base, 33 shifted
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::ReportAll, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[49;2u", 7);
}

TEST(testKittyShiftedCommaPrimaryIsBase)
{
    // ReportAll + Shift+, → base ',' (44), not shifted '<' (60).
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent(',', '<'); // 44 base, 60 shifted
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::ReportAll, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[44;2u", 7);
}

TEST(testKittyShiftedPunctAlternateUnderReportAlternate)
{
    // ReportAll + ReportAlternate: the shifted glyph is the alternate sub-parameter.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent(';', ':');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportAlternateKeys }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[59:58;2u", 10);
}

TEST(testKittyShiftedLetterBaseAndAlternate)
{
    // ReportAll + ReportAlternate: Shift+a → base 'a' (97) primary, 'A' (65) alternate.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('a', 'A');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportAlternateKeys }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97:65;2u", 10);
}

TEST(testKittyDisambigShiftLetterIsText)
{
    // Disambiguate + Shift+a → 'A' (text), not a CSI u escape code.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('a', 'A');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "A", 1);
}

TEST(testKittyDisambigShiftPunctIsText)
{
    // Disambiguate + Shift+; → ':' (text). This is the vim ":" command regression.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent(';', ':');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, ":", 1);
}

TEST(testKittyDisambigShiftDigitIsText)
{
    // Disambiguate + Shift+1 → '!' (text).
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('1', '!');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "!", 1);
}

TEST(testKittyReportAlternateShiftLetterIsText)
{
    // ReportAlternate is not ReportAll: Shift+a still sends text 'A'.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('a', 'A');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportAlternateKeys }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "A", 1);
}

TEST(testKittyReportEventsShiftLetterIsText)
{
    // ReportEvents is not ReportAll: Shift+a still sends text 'A'.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('a', 'A');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "A", 1);
}

TEST(testKittyReportTextPlainLetterIsText)
{
    // ReportText is not ReportAll: an unmodified 'a' still sends text 'a'.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportText }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "a", 1);
}

TEST(testKittyDisambigCtrlLetterIsCSIU)
{
    // Guard the other direction: Ctrl (a non-shift modifier) does escape, even
    // under Disambiguate alone. Ctrl+a → CSI 97 ; 5 u (mask 4, +1 = 5).
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierCtrl);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97;5u", 7);
}

TEST(testKittyDisambigAlternateLetterIsCSIU)
{
    // Alternate (non-shift) escapes too: Alternate+a → CSI 97 ; 3 u (mask 2, +1 = 3).
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierAlternate);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97;3u", 7);
}

TEST(testKittyReportAllShiftLetterIsCSIU)
{
    // ReportAll forces the escape form even for a shift-only text key.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('a', 'A');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::ReportAll, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97;2u", 7);
}

TEST(testKittyAssocTextDropsControlCode)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierCtrl);
    ev.textCodePoints[0] = 0x01; // the SOH byte macOS reports for Ctrl+A
    ev.textLength = 1;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportText }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97;5u", 7); // no ";1" third group
}

TEST(testKittyAssocTextKeepsPrintable)
{
    // A genuine printable associated codePoint is emitted (third group ";65").
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('a', 'A');
    ev.textCodePoints[0] = 'A';
    ev.textLength = 1;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportText }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97;2;65u", 10);
}

TEST(testKittyReportEventsTextKeyReleaseSuppressed)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', 0);
    ev.eventType = KeyEventType::Release;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    CHECK(n == 0);
}

TEST(testKittyReportEventsTextKeyRepeatStillEmitsByte)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', 0);
    ev.eventType = KeyEventType::Repeat;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    CHECK(n == 1);
    CHECK(outputBuffer[0] == 'a');
}

TEST(testKittyReportEventsCtrlKeyReleaseReported)
{
    // Ctrl+a is an escape-code key, so under ReportEvents its release reports.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierCtrl);
    ev.eventType = KeyEventType::Release;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97;5:3u", 9);
}

TEST(testKittyReportAllTextKeyReleaseReported)
{
    // Under ReportAll every key is an escape code, so a plain letter's release
    // is reported (this is what Doom-style key-up tracking relies on).
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', 0);
    ev.eventType = KeyEventType::Release;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97;1:3u", 9);
}

TEST(testKittyOptionArrowUsesProtocolForm)
{
    // The Option word-motion conveniences (ESC b / ESC f) are a standard-path
    // mac convention. Under any kitty flag the extension owns the key: the app
    // must see the explicit modifier form CSI 1;3D required by the kitty
    // keyboard protocol (https://sw.kovidgoyal.net/kitty/keyboard-protocol/).
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyLeft, keymapModifierAlternate);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[1;3D", 6);
}

TEST(testKittyOptionBackspaceUsesProtocolForm)
{
    // Option+Backspace: standard path word-erases (ESC DEL); kitty mode
    // reports the ambiguous control key as CSI u with the Alternate modifier.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyBackspace, keymapModifierAlternate);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[127;3u", 8);
}

TEST(testKittyOptionForwardDeleteUsesProtocolForm)
{
    // Option+ForwardDelete: standard path kill-words (ESC d); kitty mode keeps
    // the named editing-key form with the modifier.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyDelete, keymapModifierAlternate);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[3;3~", 6);
}

TEST(testStandardOptionArrowKeepsWordMotion)
{
    // Flags empty: the mac word-motion conveniences are unchanged.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyLeft, keymapModifierAlternate);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n,
        "\x1B"
        "b",
        2);
}

TEST(testKittyNonASCIITextKeyReleaseSuppressed)
{
    // A non-ASCII text key (é) has no release channel either — the text-key
    // suppression is about text-producing keys, not the ASCII range.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(0x00E9, 0);
    ev.eventType = KeyEventType::Release;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    CHECK(n == 0);
}

TEST(testKittyNonASCIIReleaseReportedUnderReportAll)
{
    // ReportAll restores the release channel for non-ASCII text keys.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(0x00E9, 0);
    ev.eventType = KeyEventType::Release;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[233;1:3u", 10);
}

TEST(testKittyShiftedSubparamRequiresShift)
{
    // Spec: "the shifted key must be present only if shift is also present in
    // the modifiers." Ctrl+a with a populated shiftedKey field must not leak
    // the :65 subparameter (an app would misread it as Ctrl+Shift+a).
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierCtrl);
    ev.shiftedKey = 'A';
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportAlternateKeys }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97;5u", 7);
}

TEST(testKittyBaseLayoutSubparamWithoutShift)
{
    // The base-layout alternate has no shift requirement: Ctrl+a on a Cyrillic
    // layout still reports the base-layout key as the SECOND subparameter
    // (with the shifted slot left empty: 97::113).
    char outputBuffer[20] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierCtrl);
    ev.shiftedKey = 'A'; // must be suppressed (no shift)
    ev.baseLayoutKey = 'q';
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportAlternateKeys }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97::113;5u", 12);
}

TEST(testKittyLockModifiersStrippedFromTextKeys)
{
    // Spec: "the Lock modifiers are not reported for text producing keys, to
    // keep them usable in legacy programs." Ctrl+a with CapsLock active is
    // CSI 97;5u — not 97;69u.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierCtrl | keymapModifierCapsLock);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::Disambiguate, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97;5u", 7);
}

TEST(testKittyLockModifiersReportedUnderReportAll)
{
    // "To get lock modifiers for all keys use the Report all keys as escape
    // codes enhancement." Ctrl(4)+CapsLock(64) + 1 = 69.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierCtrl | keymapModifierCapsLock);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::ReportAll, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[97;69u", 8);
}

TEST(testLegacyNonUsBaseASCIIShiftDigit)
{
    // French AZERTY: base 'é' (non-ASCII), Shift → '2'. The front-end's both-ASCII
    // gate leaves keyCode = the produced glyph '2' (charactersIgnoringModifiers),
    // shiftedKey 0, so digit entry emits '2', not the accented base. Guards
    // AZERTY digit entry.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('2', keymapModifierShift); // shiftedKey 0 (non-ASCII base not remapped)
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "2", 1);
}

TEST(testKittyCtrlShiftPunctBaseAndAlternate)
{
    // Ctrl+Shift+; → base ';' (59) primary, ':' (58) alternate, modifier value
    // 6 (Ctrl|Shift = mask 5, +1). Locks the base-remap under a compound modifier.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent(';', ':');
    ev.modifiers = keymapModifierShift | keymapModifierCtrl;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportAlternateKeys }, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[59:58;6u", 10);
}

TEST(testLegacyShiftedPunctKeepsShiftedByte)
{
    // Shift+; on a legacy (non-Kitty) app → ':' (58), not the base ';'. This is
    // the exact regression a base-only keyCode change would otherwise introduce.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent(';', ':');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, ":", 1);
}

TEST(testLegacyShiftedDigitKeepsSymbol)
{
    // Shift+1 → '!' (33) on a legacy app, not '1'.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('1', '!');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "!", 1);
}

TEST(testLegacyShiftedLetterUppercaseByte)
{
    // Shift+a → 'A' on a legacy app.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('a', 'A');
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "A", 1);
}

TEST(testPlainPunctBaseBothPaths)
{
    // No Shift: keyCode is the base, shiftedKey 0 — both the legacy byte and the
    // Kitty primary are the base.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(';', 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, ";", 1);
    int k = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::ReportAll, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, k, "\x1B[59;1u", 7);
}

TEST(testLegacyNonASCIIShiftedFallsBackToBase)
{
    // ASCII guard: a non-ASCII shifted codePoint must not truncate into the uint8
    // byte — the legacy path falls back to the base key.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeShiftedEvent('e', 0x00E9 /* é */);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "e", 1);
}

TEST(testLegacyNonUsShiftedGlyphPassthrough)
{
    // A layout-produced character reaches the PTY as UTF-8.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(0x00A7 /* § */, keymapModifierShift);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\xC2\xA7", 2);
}

TEST(testReleaseEventDroppedUnderLegacy)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', 0);
    ev.eventType = KeyEventType::Release;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, outputBuffer);
    CHECK(n == 0);
}

TEST(testReleaseEventDroppedWithoutReportEvents)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyUp, 0);
    ev.eventType = KeyEventType::Release;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlag::ReportAll, 0, 0, outputBuffer);
    CHECK(n == 0);
}

TEST(testReleaseEventEmittedWithReportEvents)
{
    char outputBuffer[32] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyUp, 0);
    ev.eventType = KeyEventType::Release;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    CHECK(n > 0);
    // Arrow release sequence: CSI 1;<mod>:<event><final> with event=3.
    EXPECT_BYTES(outputBuffer, n, "\x1B[1;1:3A", 8);
}

TEST(testRepeatEventEmittedWithReportEvents)
{
    char outputBuffer[32] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyUp, 0);
    ev.eventType = KeyEventType::Repeat;
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    CHECK(n > 0);
    // Arrow repeat sequence: CSI 1;<mod>:<event><final> with event=2.
    EXPECT_BYTES(outputBuffer, n, "\x1B[1;1:2A", 8);
}

TEST(testRepeatEventHonoursModifyCursorKeysLevel3)
{
    char outputBuffer[32] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyUp, keymapModifierCtrl);
    ev.eventType = KeyEventType::Repeat;
    int n = Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportEvents }, 0, 0, 3, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[>1;5:2A", 9);
}

TEST(testReleaseEventHonoursModifyFunctionKeysLevel1)
{
    char outputBuffer[32] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyF1, keymapModifierShift);
    ev.eventType = KeyEventType::Release;
    int n = Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportEvents }, 0, 0, 0, 1, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[2:3P", 6);
}

TEST(testModifierKeyPressAndRelease)
{
    char outputBuffer[32] = { 0 };
    KeyEvent press = makeEvent(keymapKeyLeftControl, keymapModifierCtrl);
    int n = encode(press, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    CHECK(n > 0);
    EXPECT_BYTES(outputBuffer, n, "\x1B[57442;5:1u", 12);

    KeyEvent release = makeEvent(keymapKeyLeftControl, 0);
    release.eventType = KeyEventType::Release;
    n = encode(release, Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportEvents }, 0, 0, outputBuffer);
    CHECK(n > 0);
    EXPECT_BYTES(outputBuffer, n, "\x1B[57442;1:3u", 12);
}

TEST(testXtermModifyOtherKeysFormatCSIU)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('.', keymapModifierCtrl);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 2, 1, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[46;5u", 7);
}

// Level 2 wraps the modified letters that levels 0 and 1 fold.
//
// Level 2 does not preserve Ctrl+letter, Meta+letter or Ctrl+Meta+letter as
// their legacy bytes. xterm(1) modifyOtherKeys: level 1 "enables this
// feature for keys except for those with well-known behavior, e.g., Tab,
// Backarrow and some special control character cases", and level 2 "enables this
// feature for keys INCLUDING the exceptions listed". The exceptions are the
// point of the level: Ctrl+I and Tab are one byte, as are Ctrl+M and Enter, and
// an application turning on level 2 is trying to tell them apart.
//
// Level 1 keeps the fold, pinned below; that is what keeps Emacs (which
// enables level 1) working.
TEST(testXtermModifyOtherKeysLevel2WrapsCtrlLetter)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('p', keymapModifierCtrl);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 2, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[27;5;112~", 11);

    // Level 1 keeps the legacy fold: Ctrl+P is 0x10 there.
    n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 1, 0, outputBuffer);
    CHECK(n == 1);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x10);
}

TEST(testXtermModifyOtherKeysLevel2WrapsMetaLetter)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('v', keymapModifierAlternate);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 2, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[27;3;118~", 11);

    // Level 1 keeps the meta-discipline ESC prefix.
    n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 1, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n,
        "\x1B"
        "v",
        2);
}

TEST(testXtermModifyOtherKeysLevel2WrapsCtrlMetaLetter)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('p', keymapModifierCtrl | keymapModifierAlternate);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 2, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[27;7;112~", 11);

    // Level 1 keeps ESC + the folded control byte.
    const char legacy[] = { 0x1B, 0x10 };
    n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 1, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, legacy, 2);
}

// The special KEYS are not "other" keys and keep their sequences at every level:
// the Tab key is still a tab, which is what stops level 2 breaking every editor.
TEST(testXtermModifyOtherKeysLevel2LeavesTheTabKeyAlone)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyTab, 0);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 2, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\t", 1);
}

TEST(testXtermModifyOtherKeysLevel1ExtendsCtrlPunctuation)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('.', keymapModifierCtrl);
    int n = encode(ev, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 1, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[27;5;46~", 10);
}

TEST(testModifyCursorKeysDefaultIsUnchanged)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyRight, keymapModifierShift);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Normal, 0, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[1;2C", 6);
}

TEST(testModifyCursorKeysLevel1DropsLeadingOne)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyRight, keymapModifierShift);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Normal, 1, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[2C", 4);
}

TEST(testModifyCursorKeysLevel3UsesPrivateForm)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyUp, keymapModifierCtrl);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Normal, 3, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[>1;5A", 7);
}

// A disabled level drops the modifier: the key sends what the unmodified key
// sends, DECCKM included.
TEST(testDisabledModifyLevelSendsTheUnmodifiedKey)
{
    char outputBuffer[16] = { 0 };
    const int disabled = Ubiquitty::Keys::Keymap::keymapModifyLevelDisabled;
    KeyEvent ev = makeEvent(keymapKeyRight, keymapModifierShift);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Application, disabled, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1BOC", 3);
    KeyEvent f5 = makeEvent(keymapKeyF5, keymapModifierCtrl);
    n = encodeLevel(f5, Ubiquitty::Modes::CursorKeyMode::Normal, 0, disabled, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[15~", 5);
}

TEST(testModifyCursorKeysNoModifierIgnoresLevel)
{
    // Without a modifier the cursor key keeps its short DECCKM-aware form
    // regardless of level.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyLeft, 0);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Normal, 3, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[D", 3);
}

TEST(testModifyFunctionKeysLevel3PqForm)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyF1, keymapModifierShift);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Normal, 0, 3, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[>1;2P", 7);
}

TEST(testModifyFunctionKeysDefaultPqForm)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyF1, keymapModifierShift);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Normal, 0, 0, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[1;2P", 6);
}

TEST(testModifyFunctionKeysLevel3TildeForm)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyF5, keymapModifierShift);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Normal, 0, 3, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[>15;2~", 8);
}

TEST(testModifyKeypadKeysDefaultOffPassesThrough)
{
    // With modifyKeypadKeys off, a modified keypad digit has no
    // printable/legacy mapping in the encoder and is suppressed.
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyKeypad1, keymapModifierShift);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Normal, 0, 0, 0, outputBuffer);
    CHECK(n == 0);
}

TEST(testModifyKeypadKeysLevel2InjectsModifier)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyKeypad1, keymapModifierShift);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Normal, 0, 0, 2, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[1;2q", 6);
}

TEST(testModifyKeypadKeysLevel3PrivateForm)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyKeypadEnter, keymapModifierCtrl);
    int n = encodeLevel(ev, Ubiquitty::Modes::CursorKeyMode::Normal, 0, 0, 3, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1B[>1;5M", 7);
}

TEST(testApplicationKeypadDigitEmitsSs3)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyKeypad5, 0);
    int n = encodeKeypad(ev, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1BOu", 3);
}

TEST(test_application_keypad_enter_emits_ss3_M)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyKeypadEnter, 0);
    int n = encodeKeypad(ev, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1BOM", 3);
}

TEST(testApplicationKeypadPlusIsSs3K)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyKeypadAdd, 0);
    int n = encodeKeypad(ev, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\x1BOk", 3);
}

TEST(testApplicationKeypadModifiedFallsThrough)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyKeypad1, keymapModifierShift);
    int n = encodeKeypad(ev, outputBuffer);
    CHECK(n == 0);
}

TEST(testApplicationKeypadKittySupersedes)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyKeypad5, 0);
    int n = Ubiquitty::Keys::Keymap::encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Normal, Ubiquitty::Kitty::KeyFlag::ReportAll, 0, 0, 0, 0, 0, outputBuffer, Ubiquitty::Keys::Keymap::EncodeModes { .applicationKeypad = true });
    // Routed through the Kitty CSI-u encoder, not ESC O u.
    CHECK(n > 0 && !(n == 3 && outputBuffer[0] == '\x1B' && outputBuffer[1] == 'O'));
}

TEST(testApplicationKeypadTable)
{
    struct Case {
        uint32_t code;
        char final;
    };
    const Case cases[] = {
        { keymapKeyKeypad0, 'p' },
        { keymapKeyKeypad1, 'q' },
        { keymapKeyKeypad2, 'r' },
        { keymapKeyKeypad3, 's' },
        { keymapKeyKeypad4, 't' },
        { keymapKeyKeypad5, 'u' },
        { keymapKeyKeypad6, 'v' },
        { keymapKeyKeypad7, 'w' },
        { keymapKeyKeypad8, 'x' },
        { keymapKeyKeypad9, 'y' },
        { keymapKeyKeypadDecimal, 'n' },
        { keymapKeyKeypadMultiply, 'j' },
        { keymapKeyKeypadAdd, 'k' },
        { keymapKeyKeypadSubtract, 'm' },
        { keymapKeyKeypadDivide, 'o' },
        { keymapKeyKeypadEqual, 'X' },
        { keymapKeyKeypadEnter, 'M' },
    };
    for (const Case& c : cases) {
        char outputBuffer[8] = { 0 };
        int n = Ubiquitty::Keys::Keymap::applicationKeypadSS3(c.code, outputBuffer);
        const char want[3] = { '\x1B', 'O', c.final };
        EXPECT_BYTES(outputBuffer, n, want, 3);
    }
    // A non-keypad key returns 0 (no SS3).
    char outputBuffer[8] = { 0 };
    CHECK(Ubiquitty::Keys::Keymap::applicationKeypadSS3(keymapKeyLeft, outputBuffer) == 0);
}

TEST(testMetaSendEscapeAlternateLetterPrefix)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierAlternate);
    int n = encodeMeta(ev, { .metaSendsEscape = true, .alternateSendsEscape = false, .eightBitInput = false }, outputBuffer);
    CHECK(n == 2);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x1B);
    CHECK(outputBuffer[1] == 'a');
}

TEST(testMetaEightBitInputAlternateLetterHighBit)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierAlternate);
    int n = encodeMeta(ev, { .metaSendsEscape = false, .alternateSendsEscape = false, .eightBitInput = true }, outputBuffer);
    CHECK(n == 1);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0xE1);
}

TEST(testMetaAllOffFallsBackToEscape)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierAlternate);
    int n = encodeMeta(ev, { .metaSendsEscape = false, .alternateSendsEscape = false, .eightBitInput = false }, outputBuffer);
    CHECK(n == 2);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x1B);
    CHECK(outputBuffer[1] == 'a');
}

TEST(testAlternateSendEscapeOverridesEightBit)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', keymapModifierAlternate);
    int n = encodeMeta(ev, { .metaSendsEscape = false, .alternateSendsEscape = true, .eightBitInput = true }, outputBuffer);
    CHECK(n == 2);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x1B);
    CHECK(outputBuffer[1] == 'a');
}

TEST(testCtrlAlternateEightBitInputHighBit)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('p', keymapModifierCtrl | keymapModifierAlternate);
    int n = encodeMeta(ev, { .metaSendsEscape = false, .alternateSendsEscape = false, .eightBitInput = true }, outputBuffer);
    CHECK(n == 1);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x90);
}

TEST(testEightBitInputIgnoresUnmodifiedKey)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent('a', 0);
    int n = encodeMeta(ev, { .metaSendsEscape = false, .alternateSendsEscape = false, .eightBitInput = true }, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "a", 1);
}

TEST(testDeleteResetSendsRemoveSequence)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyDelete, 0);
    int n = encodeDelete(ev, DeleteKeyMode::RemoveSequence, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[3~", 4);
}

TEST(testDeleteSetSendsDel)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyDelete, 0);
    int n = encodeDelete(ev, DeleteKeyMode::SendsDelete, outputBuffer);
    CHECK(n == 1);
    CHECK(static_cast<uint8_t>(outputBuffer[0]) == 0x7F);
}

TEST(testDeleteSetWithModifierKeepsExplicitForm)
{
    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyDelete, keymapModifierCtrl);
    int n = encodeDelete(ev, DeleteKeyMode::SendsDelete, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[3;5~", 6);
}

TEST(testUDKProgrammedKeyEmitsString)
{
    Ubiquitty::VT::UserDefinedKeys udk {};
    udk.count = 1;
    udk.entries[0].keyCode = 17; // F6
    udk.entries[0].length = 3;
    udk.entries[0].value[0] = 'x';
    udk.entries[0].value[1] = 'y';
    udk.entries[0].value[2] = 'z';

    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyF6, 0);
    int n = encodeUDK(ev, &udk, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "xyz", 3);
}

TEST(testUDKUnprogrammedKeyUsesDefault)
{
    Ubiquitty::VT::UserDefinedKeys udk {};
    udk.count = 1;
    udk.entries[0].keyCode = 17; // F6 programmed, F7 is not
    udk.entries[0].length = 1;
    udk.entries[0].value[0] = '!';

    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyF7, 0);
    int n = encodeUDK(ev, &udk, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[18~", 5);
}

TEST(testUDKModifiedKeySkipsTable)
{
    Ubiquitty::VT::UserDefinedKeys udk {};
    udk.count = 1;
    udk.entries[0].keyCode = 17; // F6
    udk.entries[0].length = 1;
    udk.entries[0].value[0] = '!';

    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyF6, keymapModifierShift);
    int n = encodeUDK(ev, &udk, outputBuffer);
    // Shift+F6 → the xterm modified function-key form, not the programmed byte.
    CHECK(n > 1);
    CHECK(outputBuffer[0] == 0x1B);
    CHECK(outputBuffer[1] == '[');
}

TEST(testUDKDeleteKey)
{
    Ubiquitty::VT::UserDefinedKeys udk {};
    udk.count = 1;
    udk.entries[0].keyCode = 3; // Remove (Delete)
    udk.entries[0].length = 2;
    udk.entries[0].value[0] = 'O';
    udk.entries[0].value[1] = 'K';

    char outputBuffer[16] = { 0 };
    KeyEvent ev = makeEvent(keymapKeyDelete, 0);
    int n = encodeUDK(ev, &udk, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "OK", 2);
}

TEST(testMetaPreservesUnicodeText)
{
    struct Case {
        std::uint32_t codePoint;
        const char* expected;
        int length;
    };
    const Case cases[] = {
        { 0xE4, "\x1B\xC3\xA4", 3 },
        { 0xF6, "\x1B\xC3\xB6", 3 },
        { 0xFC, "\x1B\xC3\xBC", 3 },
        { 0x3B1, "\x1B\xCE\xB1", 3 },
        { 0x1F600, "\x1B\xF0\x9F\x98\x80", 5 },
    };
    for (const auto& item : cases) {
        char output[16] = {};
        const auto event = makeEvent(item.codePoint, keymapModifierAlternate);
        const int length = encode(event, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, output);
        EXPECT_BYTES(output, length, item.expected, item.length);
        for (int capacity = 1; capacity < item.length; ++capacity) {
            output[0] = 'X';
            const int truncated = encodeKeyEvent(event, Ubiquitty::Modes::CursorKeyMode::Normal, {}, 0, 0, 0, 0, 0,
                std::span<char>(output, static_cast<std::size_t>(capacity)));
            CHECK(truncated == 0);
            CHECK(output[0] == 'X');
        }
    }
}

UBIQUITTY_TEST_MAIN()

TEST(testDisabledOtherAndKeypadModifiersKeepTheirPlainEncoding)
{
    std::array<char, 32> output {};
    const auto ordinary = makeEvent('.', keymapModifierCtrl);
    const int disabled = keymapModifyLevelDisabled;
    int length = encode(ordinary, Ubiquitty::Modes::CursorKeyMode::Normal, {}, disabled, 0, output);
    EXPECT_BYTES(output.data(), length, ".", 1);
    const auto keypad = makeEvent(keymapKeyKeypad1, keymapModifierShift);
    length = encodeLevel(keypad, Ubiquitty::Modes::CursorKeyMode::Normal, 0, 0, disabled, output);
    CHECK(length == 0);
    length = encodeLevel(keypad, Ubiquitty::Modes::CursorKeyMode::Normal, 0, 0, 2, output);
    EXPECT_BYTES(output.data(), length, "\033[1;2q", 6);
}
