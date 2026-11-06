// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Core/Input/test_keytranslation.cpp — shared input-policy tests.
//
// Three groups, pinning the input policy that Source/Core/Input/ holds for
// every frontend:
//
//   1. KeyTranslation parity — every VirtualKey resolves to the expected
//      Kitty codePoint, including the keys a per-frontend table gets wrong
//      (Insert, ISO_Left_Tab → Tab, and the keypad PUA),
//      and the KeyFamily classifier agrees with the codePoint tables.
//
//   2. Bare-modifier CSI-u forwarding for Doom-style binds, through the
//      encoder every frontend reports a key with: the REPORT_ALL gate,
//      press / release, and the modifier PUA codePoints.
//
//   3. ModifierPolicy — the Option-as-Meta bypass truth table over
//      ([keys] option_as_alt, which Option keys are held, super).
//
// No platform dependency: the assertions describe what every frontend
// (macOS, Linux) must produce for a given portable input.

#include "Core/Input/ModifierPolicy.h"
#include "Core/Keys/KeyEvent.h"
#include "Core/Keys/KeyTranslation.h"
#include "Core/Keys/Keymap.h"
#include "Core/Keys/KittyKeyboard.h"
#include "Core/Modes/TerminalModes.h"
#include "Core/Runtime/KeyEncode.h"

#include <cstdio>
#include <cstring>
#include <span>

using Ubiquitty::Input::AlternateModifier;
using Ubiquitty::Input::SuperModifier;
using Ubiquitty::Keys::KeyEventType;
using Ubiquitty::Keys::KeyFamily;
using Ubiquitty::Keys::VirtualKey;
using namespace Ubiquitty::Keys::Keymap;
namespace Kitty = Ubiquitty::Kitty;

#define EXPECT_CP(key, expected)                                                                                           \
    do {                                                                                                                   \
        uint32_t got = codePointFor(key);                                                                                  \
        if (got != static_cast<uint32_t>(expected)) {                                                                      \
            char ubqMessage[192];                                                                                          \
            std::snprintf(ubqMessage, sizeof(ubqMessage), "%s → %u, want %u", #key, got, static_cast<uint32_t>(expected)); \
            CHECK_MSG(false, ubqMessage);                                                                                  \
        }                                                                                                                  \
    } while (0)

#define EXPECT_BYTES(buffer, length, expected, explen)                                                                                                                                        \
    do {                                                                                                                                                                                      \
        if ((length) != (explen) || std::memcmp((buffer), (expected), (explen)) != 0) {                                                                                                       \
            char ubqMessage[192];                                                                                                                                                             \
            std::snprintf(ubqMessage, sizeof(ubqMessage), "bytes mismatch (got %d '%.*s', want %d '%.*s')", (length), (length) > 0 ? (length) : 0, (buffer), (explen), (explen), (expected)); \
            CHECK_MSG(false, ubqMessage);                                                                                                                                                     \
        }                                                                                                                                                                                     \
    } while (0)

// Resolve a VirtualKey to whichever table claims it (functional / keypad /
// modifier). Exactly one table is non-zero for any non-None key, which the
// classifier-agreement test pins; this helper just surfaces it.
static uint32_t codePointFor(VirtualKey key)
{
    uint32_t functional = Ubiquitty::Keys::functionalKeyCodePoint(key);
    if (functional != 0)
        return functional;
    uint32_t keypad = Ubiquitty::Keys::keypadKeyCodePoint(key);
    if (keypad != 0)
        return keypad;
    return Ubiquitty::Keys::modifierKittyCodePoint(key);
}

// ---- Group 1: KeyTranslation parity ----------------------------------------

// The classifier and the three codePoint tables must agree for every
// enumerator: a key is Functional iff functionalKeyCodePoint != 0, Keypad
// iff keypadKeyCodePoint != 0, Modifier iff modifierKittyCodePoint != 0,
// and at most one table is non-zero. This walks the whole enum so a new
// key added to one table but not the classifier fails here.
// The keypad PUA the shared table hands out must be the same codePoint the
// shared application-keypad SS3 table keys on — i.e. every keypad VirtualKey
// drives a real ESC O <final>. This is the Linux-parity fix: once GTK maps
// GDK_KEY_KP_* → VirtualKey::Keypad*, its application-keypad path lights up
// against the same table macOS already uses.
// ---- Group 2: bare-modifier CSI-u forwarding -------------------------------

// One bare modifier through the encoder every frontend reports a key with. A
// snapshot carries the four xterm levels at 0, which is what these cases hold
// fixed: the Kitty flags are the only thing they move.
static int modEvent(VirtualKey key, uint32_t modifiers, KeyEventType type, Ubiquitty::Kitty::KeyFlagSet kittyFlags, std::span<char> output)
{
    Ubiquitty::Runtime::ModeSnapshot modes;
    modes.kittyKeyboardFlags[0] = kittyFlags.toRaw();
    Ubiquitty::Runtime::KeyPress press;
    press.virtualKey = key;
    press.modifiers = modifiers;
    press.eventType = type;
    // A modifier's own bit is live in the mask while it is held.
    press.wasModified = modifiers != 0;
    return Ubiquitty::Runtime::encodeKey(modes, press, output, nullptr).length;
}

// The alternate-key subparameters (keyCode:shifted:base in the first group)
// gate only on ReportAlternateKeys (flag 4). ReportAll (flag 8) forces the
// modifier group but must not leak the alt-key subparams — the kitty spec keeps
// the two independent.
// ---- Group 2b: modifyOtherKeys printable wrapping --------------------------

// Drive a printable key through the full encoder with a chosen modifyOtherKeys
// level (CSI > 4 ; Pm m), Kitty off.
static int encodePrintable(uint32_t keyCode, uint32_t modifiers, uint8_t modifyOther, std::span<char> output)
{
    Ubiquitty::Keys::KeyEvent ev {};
    ev.keyCode = keyCode;
    ev.modifiers = modifiers;
    ev.eventType = KeyEventType::Press;
    return encodeKeyEvent(ev, Ubiquitty::Modes::CursorKeyMode::Normal, /*kittyFlags*/ {},
        /*modifyOther*/ modifyOther, /*formatOther*/ 0,
        /*modifyCursor*/ 0, /*modifyFunction*/ 0, /*modifyKeypad*/ 0, output);
}

// ---- Group 3: ModifierPolicy truth table -----------------------------------

TEST(testFunctionalKeysParity)
{
    // Control / editing keys carried by their Kitty identity. Backspace
    // and ForwardDelete are the two halves of the Mac "delete" split.
    EXPECT_CP(VirtualKey::Return, keymapKeyEnter);
    EXPECT_CP(VirtualKey::KeypadEnter, keymapKeyKeypadEnter);
    EXPECT_CP(VirtualKey::Tab, keymapKeyTab);
    EXPECT_CP(VirtualKey::Backspace, keymapKeyBackspace);
    EXPECT_CP(VirtualKey::ForwardDelete, keymapKeyDelete);
    EXPECT_CP(VirtualKey::Escape, keymapKeyEscape);
    EXPECT_CP(VirtualKey::LeftArrow, keymapKeyLeft);
    EXPECT_CP(VirtualKey::RightArrow, keymapKeyRight);
    EXPECT_CP(VirtualKey::UpArrow, keymapKeyUp);
    EXPECT_CP(VirtualKey::DownArrow, keymapKeyDown);
    EXPECT_CP(VirtualKey::Home, keymapKeyHome);
    EXPECT_CP(VirtualKey::End, keymapKeyEnd);
    EXPECT_CP(VirtualKey::PageUp, keymapKeyPageUp);
    EXPECT_CP(VirtualKey::PageDown, keymapKeyPageDown);
}

TEST(testPreviouslyDivergentKeys)
{
    // Insert: the shared table maps it to keymapKeyInsert for every frontend,
    // so a frontend that translates its native Insert to VirtualKey::Insert
    // reports the same code point as every other.
    EXPECT_CP(VirtualKey::Insert, keymapKeyInsert);
    CHECK(keymapKeyInsert == 57348);

    // ISO_Left_Tab (the keyval GTK delivers for Shift+Tab) folds to the
    // same VirtualKey::Tab as a plain Tab, so the platform's native
    // ISO_Left_Tab → VirtualKey::Tab translation lands on keymapKeyTab and
    // the encoder's Shift-Tab → CSI Z rule fires from the shared keyCode.
    EXPECT_CP(VirtualKey::Tab, keymapKeyTab);

    // Keypad PUA: the shared table carries the keypad PUA, so a frontend
    // drives the application-keypad SS3 path from the keypad key rather than
    // from the character its toolkit maps the key to.
    EXPECT_CP(VirtualKey::Keypad0, keymapKeyKeypad0);
    EXPECT_CP(VirtualKey::Keypad9, keymapKeyKeypad9);
    EXPECT_CP(VirtualKey::KeypadDecimal, keymapKeyKeypadDecimal);
    EXPECT_CP(VirtualKey::KeypadDivide, keymapKeyKeypadDivide);
    EXPECT_CP(VirtualKey::KeypadMultiply, keymapKeyKeypadMultiply);
    EXPECT_CP(VirtualKey::KeypadSubtract, keymapKeyKeypadSubtract);
    EXPECT_CP(VirtualKey::KeypadAdd, keymapKeyKeypadAdd);
    EXPECT_CP(VirtualKey::KeypadEqual, keymapKeyKeypadEqual);
}

TEST(testFunctionKeyBlockParity)
{
    EXPECT_CP(VirtualKey::F1, keymapKeyF1);
    EXPECT_CP(VirtualKey::F2, keymapKeyF2);
    EXPECT_CP(VirtualKey::F3, keymapKeyF3);
    EXPECT_CP(VirtualKey::F4, keymapKeyF4);
    EXPECT_CP(VirtualKey::F5, keymapKeyF5);
    EXPECT_CP(VirtualKey::F6, keymapKeyF6);
    EXPECT_CP(VirtualKey::F7, keymapKeyF7);
    EXPECT_CP(VirtualKey::F8, keymapKeyF8);
    EXPECT_CP(VirtualKey::F9, keymapKeyF9);
    EXPECT_CP(VirtualKey::F10, keymapKeyF10);
    EXPECT_CP(VirtualKey::F11, keymapKeyF11);
    EXPECT_CP(VirtualKey::F12, keymapKeyF12);
    // The F-block is a contiguous PUA run; a frontend can add F-number to
    // the F1 base (as the GTK mapKey does), so pin the contiguity too.
    CHECK(keymapKeyF1 + 11 == keymapKeyF12);
}

TEST(testModifierCodepointsParity)
{
    EXPECT_CP(VirtualKey::LeftShift, keymapKeyLeftShift);
    EXPECT_CP(VirtualKey::RightShift, keymapKeyRightShift);
    EXPECT_CP(VirtualKey::LeftControl, keymapKeyLeftControl);
    EXPECT_CP(VirtualKey::RightControl, keymapKeyRightControl);
    EXPECT_CP(VirtualKey::LeftAlternate, keymapKeyLeftAlternate);
    EXPECT_CP(VirtualKey::RightAlternate, keymapKeyRightAlternate);
    // Apple maps Command → Super, so the Command keys resolve to Super.
    EXPECT_CP(VirtualKey::LeftSuper, keymapKeyLeftSuper);
    EXPECT_CP(VirtualKey::RightSuper, keymapKeyRightSuper);
}

TEST(testNoneAndPrintable)
{
    // VirtualKey::None is the no-functional-identity sentinel: all three
    // tables return 0, and the family is None (a printable key, whose
    // codePoint the frontend derives from the typed character).
    CHECK(Ubiquitty::Keys::functionalKeyCodePoint(VirtualKey::None) == 0);
    CHECK(Ubiquitty::Keys::keypadKeyCodePoint(VirtualKey::None) == 0);
    CHECK(Ubiquitty::Keys::modifierKittyCodePoint(VirtualKey::None) == 0);
    CHECK(Ubiquitty::Keys::keyFamily(VirtualKey::None) == KeyFamily::None);
}

TEST(aSoftwareCommandChordMatchesTheMacClientOnTheHost)
{
    Ubiquitty::Runtime::KeyPress macPress;
    macPress.virtualKey = Ubiquitty::Keys::printableKeyForHID(6); // C on the physical keyboard.
    macPress.modifiers = keymapModifierSuper;
    macPress.unshiftedCodePoint = 'c';
    macPress.shiftedCodePoint = 'c';
    macPress.text[0] = 'c';
    macPress.textLength = 1;
    macPress.wasModified = true;

    auto accessoryPress = macPress;
    accessoryPress.virtualKey = VirtualKey::None; // A software cap has no physical position.
    for (const auto flags : { Kitty::KeyFlagSet {}, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate } }) {
        Ubiquitty::Runtime::ModeSnapshot modes;
        modes.kittyKeyboardFlags[0] = flags.toRaw();
        char macOutput[32] {};
        char accessoryOutput[32] {};
        const int macLength = Ubiquitty::Runtime::encodeKey(modes, macPress, macOutput, nullptr).length;
        const int accessoryLength = Ubiquitty::Runtime::encodeKey(modes, accessoryPress, accessoryOutput, nullptr).length;
        CHECK(macLength == accessoryLength);
        CHECK(std::string_view(macOutput, static_cast<std::size_t>(macLength))
            == std::string_view(accessoryOutput, static_cast<std::size_t>(accessoryLength)));
        CHECK(macLength == (flags.isEmpty() ? 0 : 7));
    }
}

TEST(testClassifierAgreesWithTables)
{
    // The enum is a contiguous range [None, RightSuper]; iterate it.
    for (uint16_t v = static_cast<uint16_t>(VirtualKey::None); v <= static_cast<uint16_t>(VirtualKey::RightSuper); ++v) {
        const VirtualKey key = static_cast<VirtualKey>(v);
        const uint32_t functional = Ubiquitty::Keys::functionalKeyCodePoint(key);
        const uint32_t keypad = Ubiquitty::Keys::keypadKeyCodePoint(key);
        const uint32_t modifier = Ubiquitty::Keys::modifierKittyCodePoint(key);
        const int nonZero = (functional != 0) + (keypad != 0) + (modifier != 0);
        // At most one table claims any key.
        CHECK(nonZero <= 1);
        const KeyFamily family = Ubiquitty::Keys::keyFamily(key);
        if (modifier != 0)
            CHECK(family == KeyFamily::Modifier);
        else if (keypad != 0)
            CHECK(family == KeyFamily::Keypad);
        else if (functional != 0)
            CHECK(family == KeyFamily::Functional);
        else
            CHECK(family == KeyFamily::None);
    }
    // Spot-check the four representatives directly.
    CHECK(Ubiquitty::Keys::keyFamily(VirtualKey::LeftArrow) == KeyFamily::Functional);
    CHECK(Ubiquitty::Keys::keyFamily(VirtualKey::Keypad5) == KeyFamily::Keypad);
    CHECK(Ubiquitty::Keys::keyFamily(VirtualKey::LeftControl) == KeyFamily::Modifier);
    // KeypadEnter is Functional (emits a newline), not Keypad — it must not
    // route through the application-keypad SS3 table.
    CHECK(Ubiquitty::Keys::keyFamily(VirtualKey::KeypadEnter) == KeyFamily::Functional);
    CHECK(Ubiquitty::Keys::keypadKeyCodePoint(VirtualKey::KeypadEnter) == 0);
}

// `reportedKeyCodePoint` is the one routing every frontend reports a key
// through. Reading the functional table alone answers 0 for a keypad key, so a
// keypad press reports as the digit on its cap and the distinction the keypad
// code points carry is lost under Kitty.
//
// The encoder is a parameter rather than a detail. A keypad key answers only
// under Kitty: the legacy encoder has no byte form for a bare keypad code
// point, so reporting one there leaves an ordinary keypad press emitting
// nothing at all. A switch on the key family alone breaks that case.
TEST(testReportedKeyCodePointRoutesEveryFamily)
{
    using Ubiquitty::Keys::KeyEncoder;
    using Ubiquitty::Keys::reportedKeyCodePoint;

    // The enum is a contiguous range [None, RightSuper]; iterate it.
    for (uint16_t v = static_cast<uint16_t>(VirtualKey::None); v <= static_cast<uint16_t>(VirtualKey::RightSuper); ++v) {
        const VirtualKey key = static_cast<VirtualKey>(v);
        const uint32_t functional = Ubiquitty::Keys::functionalKeyCodePoint(key);
        const uint32_t keypad = Ubiquitty::Keys::keypadKeyCodePoint(key);
        const uint32_t modifier = Ubiquitty::Keys::modifierKittyCodePoint(key);

        // Each family answers with its own table, and with the same value the
        // table itself gives — a resolver that invented a code point would pass
        // a non-zero check and fail this one.
        switch (Ubiquitty::Keys::keyFamily(key)) {
        case KeyFamily::Modifier:
            CHECK(reportedKeyCodePoint(key, KeyEncoder::Kitty) == modifier);
            CHECK(reportedKeyCodePoint(key, KeyEncoder::Legacy) == modifier);
            break;
        case KeyFamily::Keypad:
            CHECK(reportedKeyCodePoint(key, KeyEncoder::Kitty) == keypad);
            CHECK(reportedKeyCodePoint(key, KeyEncoder::Legacy) == 0);
            break;
        case KeyFamily::Functional:
            CHECK(reportedKeyCodePoint(key, KeyEncoder::Kitty) == functional);
            CHECK(reportedKeyCodePoint(key, KeyEncoder::Legacy) == functional);
            break;
        case KeyFamily::None:
            CHECK(reportedKeyCodePoint(key, KeyEncoder::Kitty) == 0);
            CHECK(reportedKeyCodePoint(key, KeyEncoder::Legacy) == 0);
            break;
        }
    }

    // The discrimination a functional-table read loses. A keypad key reports
    // its own identity under Kitty and defers to the typed character under
    // legacy; a functional key answers the same under both. Reading the
    // functional table alone collapses the first pair.
    CHECK(reportedKeyCodePoint(VirtualKey::Keypad5, KeyEncoder::Kitty) == Ubiquitty::Keys::Keymap::keymapKeyKeypad5);
    CHECK(reportedKeyCodePoint(VirtualKey::Keypad5, KeyEncoder::Legacy) == 0);
    CHECK(reportedKeyCodePoint(VirtualKey::LeftArrow, KeyEncoder::Kitty)
        == reportedKeyCodePoint(VirtualKey::LeftArrow, KeyEncoder::Legacy));

    // KeypadEnter sits in the Functional family, so it answers under both
    // encoders — the one keypad-named key the encoder argument must not gate.
    CHECK(reportedKeyCodePoint(VirtualKey::KeypadEnter, KeyEncoder::Legacy)
        == Ubiquitty::Keys::Keymap::keymapKeyKeypadEnter);
}

TEST(testKeypadDrivesApplicationKeypadSs3)
{
    // Each keypad VirtualKey → its EXACT ESC O <final> per VT220 / xterm. The
    // Linux frontend's keypadVirtualKeyFor table (GDK_KEY_KP_* → these) now feeds
    // keypadKeyCodePoint → applicationKeypadSS3, so pinning the final byte here
    // pins the wire output of every Linux application-keypad press against the
    // same table macOS already drives.
    struct {
        VirtualKey key;
        char final;
    } cases[] = {
        { VirtualKey::Keypad0, 'p' },
        { VirtualKey::Keypad1, 'q' },
        { VirtualKey::Keypad2, 'r' },
        { VirtualKey::Keypad3, 's' },
        { VirtualKey::Keypad4, 't' },
        { VirtualKey::Keypad5, 'u' },
        { VirtualKey::Keypad6, 'v' },
        { VirtualKey::Keypad7, 'w' },
        { VirtualKey::Keypad8, 'x' },
        { VirtualKey::Keypad9, 'y' },
        { VirtualKey::KeypadDecimal, 'n' },
        { VirtualKey::KeypadDivide, 'o' },
        { VirtualKey::KeypadMultiply, 'j' },
        { VirtualKey::KeypadSubtract, 'm' },
        { VirtualKey::KeypadAdd, 'k' },
        { VirtualKey::KeypadEqual, 'X' },
    };
    for (auto& c : cases) {
        char ss3[8] = { 0 };
        int n = applicationKeypadSS3(Ubiquitty::Keys::keypadKeyCodePoint(c.key), ss3);
        const char expect[3] = { '\033', 'O', c.final };
        EXPECT_BYTES(ss3, n, expect, 3);
    }
}

TEST(testModifierSuppressedWithoutReportAll)
{
    char outputBuffer[64] = { 0 };
    // No Kitty flags → legacy terminal, no modifier-key channel.
    int n = modEvent(VirtualKey::LeftControl, 0, KeyEventType::Press, {}, outputBuffer);
    CHECK(n == 0);
    // Disambiguate / report-events alone (without REPORT_ALL) still suppress
    // a standalone modifier — only REPORT_ALL opens the channel.
    n = modEvent(VirtualKey::LeftControl, 0, KeyEventType::Press, Kitty::KeyFlagSet { Kitty::KeyFlag::Disambiguate, Kitty::KeyFlag::ReportEvents }, outputBuffer);
    CHECK(n == 0);
}

TEST(testModifierZeroCodepointEmitsNothing)
{
    char outputBuffer[64] = { 0 };
    // A key with no identity at all — no modifier code point and no typed
    // character — forwards nothing even under REPORT_ALL.
    int n = modEvent(VirtualKey::None, 0, KeyEventType::Press, Kitty::KeyFlag::ReportAll, outputBuffer);
    CHECK(n == 0);
}

TEST(testModifierPressReportAll)
{
    char outputBuffer[64] = { 0 };
    // Left Control press under REPORT_ALL, with the Ctrl bit already live in
    // the modifier mask (the modifier is held). REPORT_ALL forces the full
    // CSI u shape with an explicit modifier parameter. Ctrl mask bit (4) →
    // kitty modifier value 5.
    int n = modEvent(VirtualKey::LeftControl, keymapModifierCtrl, KeyEventType::Press, Kitty::KeyFlag::ReportAll, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[57442;5u", 10);
}

TEST(testKittyAlternateSubparamsGateOnFlag4Only)
{
    char outputBuffer[64] = { 0 };
    // keyCode 'a' (97), shifted 'A' (65), base 'a' (97), no active modifier.
    // ReportAll ALONE: the forced modifier group only — no :shifted:base.
    int n = Ubiquitty::Kitty::encodeKey(97, 65, 97, 0, KeyEventType::Press, {}, Kitty::KeyFlag::ReportAll, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[97;1u", 7);
    // ReportAlternateKeys with Shift held: both subparams are emitted
    // (modifier value 2 = shift mask 1 + 1).
    n = Ubiquitty::Kitty::encodeKey(97, 65, 97, keymapModifierShift, KeyEventType::Press, {}, Kitty::KeyFlag::ReportAlternateKeys, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[97:65:97;2u", 13);
    // Without Shift in the modifiers the shifted subparam is suppressed (spec:
    // "the shifted key must be present only if shift is also present") — the
    // base-layout alternate keeps its slot with the shifted one left empty.
    n = Ubiquitty::Kitty::encodeKey(97, 65, 97, 0, KeyEventType::Press, {}, Kitty::KeyFlag::ReportAlternateKeys, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[97::97;1u", 11);
}

TEST(testModifierPressReleaseWithEvents)
{
    char outputBuffer[64] = { 0 };
    // With REPORT_ALL + REPORT_EVENTS the event-type subparameter is emitted.
    // Left Shift press: modifier value 1 (no live modifier bits in the mask
    // at the instant we model here) and event-type 1.
    int n = modEvent(VirtualKey::LeftShift, 0, KeyEventType::Press, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportEvents }, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[57441;1:1u", 12);

    // Release of the same key reports event-type 3.
    n = modEvent(VirtualKey::LeftShift, 0, KeyEventType::Release, Kitty::KeyFlagSet { Kitty::KeyFlag::ReportAll, Kitty::KeyFlag::ReportEvents }, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[57441;1:3u", 12);
}

TEST(testModifierReleaseWithoutEventsSuppressed)
{
    char outputBuffer[64] = { 0 };
    // REPORT_ALL but not REPORT_EVENTS: a release has no byte form, so
    // nothing is emitted (the encoder drops non-press events). This is the
    // gate that keeps a held-key release from leaking onto a stream that
    // only asked for press reporting.
    int n = modEvent(VirtualKey::LeftAlternate, 0, KeyEventType::Release, Kitty::KeyFlag::ReportAll, outputBuffer);
    CHECK(n == 0);
    // The matching press is emitted.
    n = modEvent(VirtualKey::LeftAlternate, keymapModifierAlternate, KeyEventType::Press, Kitty::KeyFlag::ReportAll, outputBuffer);
    CHECK(n > 0);
}

TEST(testModifierCodepointsRoute)
{
    // Each bare modifier forwards its own PUA codePoint as the CSI u key.
    struct {
        VirtualKey key;
        const char* expect;
    } cases[] = {
        { VirtualKey::LeftShift, "\033[57441;1u" },
        { VirtualKey::RightShift, "\033[57447;1u" },
        { VirtualKey::LeftControl, "\033[57442;1u" },
        { VirtualKey::RightControl, "\033[57448;1u" },
        { VirtualKey::LeftAlternate, "\033[57443;1u" },
        { VirtualKey::RightAlternate, "\033[57449;1u" },
        { VirtualKey::LeftSuper, "\033[57444;1u" },
        { VirtualKey::RightSuper, "\033[57450;1u" },
    };
    for (auto& c : cases) {
        char outputBuffer[64] = { 0 };
        int n = modEvent(c.key, 0, KeyEventType::Press, Kitty::KeyFlag::ReportAll, outputBuffer);
        EXPECT_BYTES(outputBuffer, n, c.expect, static_cast<int>(std::strlen(c.expect)));
    }
}

TEST(testModifyOtherKeysShiftPrintable)
{
    char outputBuffer[64] = { 0 };
    // Shift+a arrives as keyCode 'A' (the shifted glyph) plus the Shift bit.
    const uint32_t shiftedA = static_cast<uint32_t>('A');

    // Level 0 (off): the plain shifted byte.
    int n = encodePrintable(shiftedA, keymapModifierShift, 0, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "A", 1);

    // Level 1: a Shift-only printable keeps its ordinary byte — it must not be
    // wrapped in CSI 27;mod;code~. Emacs enables level 1 and can't decode that
    // wrapper for a letter, which beeped and dropped a stray "65~".
    n = encodePrintable(shiftedA, keymapModifierShift, 1, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "A", 1);

    // Level 2 (format-all): Shift+printable is wrapped (65 == 'A').
    n = encodePrintable(shiftedA, keymapModifierShift, 2, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[27;2;65~", 10);

    // Level 1 still wraps a genuine "other" key — a non-Shift modifier with no
    // legacy control byte (here Ctrl+'2', which has no C0 encoding).
    n = encodePrintable(static_cast<uint32_t>('2'), keymapModifierCtrl, 1, outputBuffer);
    EXPECT_BYTES(outputBuffer, n, "\033[27;5;50~", 10);
}

// Every setting against every held state, with Command up and down. The rows
// are written out rather than derived, so a predicate that drifts from the
// table fails on the row that names the case.
TEST(testModifierPolicyTruthTable)
{
    using Ubiquitty::Input::eventShouldBypassTextInputForTerminalEncoding;
    using Ubiquitty::Input::OptionAsAlternate;

    struct Case {
        OptionAsAlternate policy;
        AlternateModifier alternate;
        bool shouldBypassWithCommandUp;
    };
    constexpr Case cases[] = {
        // none: no Option key is Meta, so the input method types the layout's
        // character (German Option-L is @) and composes its dead keys.
        { OptionAsAlternate::None, AlternateModifier::Up, false },
        { OptionAsAlternate::None, AlternateModifier::Held, false },
        { OptionAsAlternate::None, AlternateModifier::Left, false },
        { OptionAsAlternate::None, AlternateModifier::Right, false },
        { OptionAsAlternate::None, AlternateModifier::Both, false },
        // left: the left key is Meta and the right one still types.
        { OptionAsAlternate::Left, AlternateModifier::Up, false },
        { OptionAsAlternate::Left, AlternateModifier::Held, true },
        { OptionAsAlternate::Left, AlternateModifier::Left, true },
        { OptionAsAlternate::Left, AlternateModifier::Right, false },
        { OptionAsAlternate::Left, AlternateModifier::Both, true },
        // right: the mirror image.
        { OptionAsAlternate::Right, AlternateModifier::Up, false },
        { OptionAsAlternate::Right, AlternateModifier::Held, true },
        { OptionAsAlternate::Right, AlternateModifier::Left, false },
        { OptionAsAlternate::Right, AlternateModifier::Right, true },
        { OptionAsAlternate::Right, AlternateModifier::Both, true },
        // both: every held Option key is Meta.
        { OptionAsAlternate::Both, AlternateModifier::Up, false },
        { OptionAsAlternate::Both, AlternateModifier::Held, true },
        { OptionAsAlternate::Both, AlternateModifier::Left, true },
        { OptionAsAlternate::Both, AlternateModifier::Right, true },
        { OptionAsAlternate::Both, AlternateModifier::Both, true },
    };
    int row = 0;
    for (const Case& c : cases) {
        char label[64];
        std::snprintf(label, sizeof(label), "policy %d, alternate %d", static_cast<int>(c.policy), static_cast<int>(c.alternate));
        CHECK_MSG(eventShouldBypassTextInputForTerminalEncoding(c.policy, c.alternate, SuperModifier::Up) == c.shouldBypassWithCommandUp, label);
        // A Command chord is an app shortcut, never Meta, whatever the setting.
        CHECK_MSG(!eventShouldBypassTextInputForTerminalEncoding(c.policy, c.alternate, SuperModifier::Held), label);
        ++row;
    }
    CHECK_EQ(row, 20);
}

// The per-side bits say which key, the general flag says whether any is held.
TEST(testAlternateModifierForSides)
{
    using Ubiquitty::Input::alternateModifierForSides;
    CHECK(alternateModifierForSides(false, false, false) == AlternateModifier::Up);
    // A side bit without the general flag is not a held key.
    CHECK(alternateModifierForSides(false, true, false) == AlternateModifier::Up);
    CHECK(alternateModifierForSides(false, false, true) == AlternateModifier::Up);
    CHECK(alternateModifierForSides(false, true, true) == AlternateModifier::Up);
    CHECK(alternateModifierForSides(true, true, false) == AlternateModifier::Left);
    CHECK(alternateModifierForSides(true, false, true) == AlternateModifier::Right);
    CHECK(alternateModifierForSides(true, true, true) == AlternateModifier::Both);
    // A synthesised event with no side bit reports an unsided hold, which a
    // one-sided setting honours rather than dropping.
    CHECK(alternateModifierForSides(true, false, false) == AlternateModifier::Held);
}

TEST(testComposedCommitIsEverythingButOnePrintableASCIICharacter)
{
    using Ubiquitty::Keys::isComposedCommit;
    // Ordinary typing, left for the encoder: the printable ASCII span, both
    // ends included.
    CHECK(!isComposedCommit(1, ' '));
    CHECK(!isComposedCommit(1, 'a'));
    CHECK(!isComposedCommit(1, '~'));
    // Control characters are composed: an input method that delivers one is not
    // describing a key press, and the encoder has its own forms for these.
    CHECK(isComposedCommit(1, 0x00));
    CHECK(isComposedCommit(1, 0x1F));
    CHECK(isComposedCommit(1, 0x7F));
    // Outside ASCII: a dead-key accent, a CJK commit, an emoji-picker insert.
    CHECK(isComposedCommit(1, 0x00E9)); // e-acute
    CHECK(isComposedCommit(1, 0x3042)); // hiragana A
    // More than one unit, whether that is a surrogate pair or two letters.
    CHECK(isComposedCommit(2, 0xD83D)); // the lead of an astral pair
    CHECK(isComposedCommit(2, 'a'));
    CHECK(isComposedCommit(9, 'h'));
    // Nothing committed is neither: there is no text to write through and no
    // key to encode, and both callers drop it before asking.
    CHECK(!isComposedCommit(0, 0));
    printf("testComposedCommitIsEverythingButOnePrintableASCIICharacter done\n");
}

UBIQUITTY_TEST_MAIN()
