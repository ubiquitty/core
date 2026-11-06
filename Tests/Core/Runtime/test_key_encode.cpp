// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// test_key_encode.cpp — the frontend's contract for a key press
// (Core/Runtime/KeyEncode.h).
//
// A frontend reports which key, which modifiers and what text; everything the
// bytes depend on beyond that is the terminal's. These cases hold that line by
// changing only a mode and asserting the bytes move — which is the property a
// frontend cannot supply for itself: a frontend that spells the cursor keys as
// CSI forms keeps them CSI under DECCKM, where an application expects SS3.
//
// The seam takes none of the terminal's own state as an argument, because a
// caller that reads that state off the model and threads it back in can also
// skip it. So the assertions here are about what the caller withholds rather
// than what it supplies.
//

#include "Harness.h"
#include "TerminalFixture.h"
#include <cstring>

#include "Core/Runtime/KeyEncode.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <string>
#include <utility>

namespace {

namespace Rt = Ubiquitty::Runtime;
namespace K = Ubiquitty::Keys;

// One press of a key, with nothing held — what a frontend reports.
Rt::KeyPress bare(K::VirtualKey key)
{
    Rt::KeyPress press;
    press.virtualKey = key;
    press.eventType = K::KeyEventType::Press;
    return press;
}

std::string encoded(const Rt::ModeSnapshot& modes, const Rt::KeyPress& press)
{
    char buffer[64];
    const Rt::KeyEncoding encoding
        = Rt::encodeKey(modes, press, K::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) }, nullptr);
    return std::string(buffer, static_cast<std::size_t>(encoding.length < 0 ? 0 : encoding.length));
}

} // namespace

/// DECCKM is the terminal's, and the caller never names it.
///
/// The same press either side of the mode: CSI while the cursor keys are
/// normal, SS3 once an application asked for the application forms. A frontend
/// holding its own table gets the first of these and keeps sending it, which
/// is right until the first program that sets the mode — vim, less, readline —
/// and wrong silently after.
TEST(theCursorKeysFollowDECCKMWithoutTheCallerNamingIt)
{
    Rt::ModeSnapshot modes;
    modes.applicationCursorKeys = false;
    CHECK(encoded(modes, bare(K::VirtualKey::UpArrow)) == "\033[A");
    CHECK(encoded(modes, bare(K::VirtualKey::DownArrow)) == "\033[B");
    CHECK(encoded(modes, bare(K::VirtualKey::RightArrow)) == "\033[C");
    CHECK(encoded(modes, bare(K::VirtualKey::LeftArrow)) == "\033[D");

    modes.applicationCursorKeys = true;
    CHECK(encoded(modes, bare(K::VirtualKey::UpArrow)) == "\033OA");
    CHECK(encoded(modes, bare(K::VirtualKey::DownArrow)) == "\033OB");
    CHECK(encoded(modes, bare(K::VirtualKey::RightArrow)) == "\033OC");
    CHECK(encoded(modes, bare(K::VirtualKey::LeftArrow)) == "\033OD");
}

/// Keypad Enter under DECPAM is the application-keypad final, not a newline.
///
/// The shared table classifies KeypadEnter as Functional, so Enter off the
/// keypad emits a newline — which is right outside application keypad mode and
/// wrong inside it, where DEC and xterm send SS3 M. The gate names the key so
/// only this path changes.
TEST(keypadEnterSendsItsApplicationFinalUnderDECPAM)
{
    Rt::ModeSnapshot modes;
    modes.applicationKeypad = true;
    CHECK(encoded(modes, bare(K::VirtualKey::KeypadEnter)) == "\033OM");

    // Outside the mode it is Enter, and stays a newline.
    modes.applicationKeypad = false;
    CHECK(encoded(modes, bare(K::VirtualKey::KeypadEnter)) != "\033OM");
}

/// DECARM reset means an OS repeat emits nothing.
///
/// The platform reports that a key repeated; whether a repeat is an event at
/// all is the terminal's. A frontend that decided this itself would send a byte
/// per repeat to a host that asked for none.
TEST(anAutoRepeatIsSuppressedWhenTheHostTurnedAutoRepeatOff)
{
    Rt::ModeSnapshot modes;
    modes.autoRepeat = true;
    Rt::KeyPress press = bare(K::VirtualKey::UpArrow);
    press.isRepeat = true;
    CHECK(!encoded(modes, press).empty());

    modes.autoRepeat = false;
    CHECK(encoded(modes, press).empty());
}

/// Whether the caller echoes is the terminal's answer too.
///
/// SRM reset means the host will not echo, so the bytes have to go back through
/// the model to appear at all — and never while a password prompt is up,
/// whatever the echo mode says.
TEST(theTerminalDecidesWhetherTheCallerEchoes)
{
    Rt::ModeSnapshot modes;
    char buffer[64];

    modes.sendReceiveMode = true; // the host echoes
    CHECK(!Rt::encodeKey(modes, bare(K::VirtualKey::UpArrow),
        K::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) }, nullptr)
            .shouldEchoLocally);

    modes.sendReceiveMode = false; // it does not
    CHECK(Rt::encodeKey(modes, bare(K::VirtualKey::UpArrow),
        K::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) }, nullptr)
            .shouldEchoLocally);

    modes.isPasswordPromptActive = true;
    CHECK(!Rt::encodeKey(modes, bare(K::VirtualKey::UpArrow),
        K::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) }, nullptr)
            .shouldEchoLocally);
}

/// The unshifted key is the identity; the shifted glyph is what gets typed.
///
/// Both ride the same press because they answer different questions, and a
/// frontend that supplies one where the other belongs is not detectable from
/// the byte count. The web page crossed them — its `keyCode` is what the
/// browser says was TYPED and its `baseLayoutKey` is the layout position — and
/// Shift+`;` put `;` on the wire.
/// A bare modifier reports to the host that asked for one, and to no other.
///
/// The gate is `Kitty::bareModifierKeyEvent`'s, and a caller can reach the
/// encoder without passing it: a modifier forwarded through that function is
/// gated, and one forwarded through this encoder must be gated here. The
/// press cannot fall through harmlessly either, because
/// `reportedKeyCodePoint` answers a modifier with its private-use code point
/// under both encoders — so an ungated modifier reached a program as a
/// character, which a reader in nvim saw as a mark appearing under the text
/// and as Escape and `:q` no longer working.
///
/// Disambiguate is the flag a program sets without wanting modifiers, which
/// is why it sits between the two ends of this case rather than outside them.
TEST(aBareModifierReportsOnlyToAHostThatAskedForEveryKey)
{
    // What a frontend reports when the physical Shift goes down: the key, its
    // own bit already in the modifier set, and no text — an AppKit
    // flags-changed event carries no characters. The bit and `wasModified` are
    // what separate this from a synthetic press of the same key, and they are
    // what a modifier ladder reads.
    const auto held = [](K::VirtualKey key, std::uint32_t bit) {
        Rt::KeyPress press = bare(key);
        press.modifiers = bit;
        press.wasModified = true;
        return press;
    };
    const std::uint32_t shift = static_cast<std::uint32_t>(K::Keymap::keymapModifierShift);

    Rt::ModeSnapshot modes;
    const std::string legacyForm = encoded(modes, held(K::VirtualKey::LeftShift, shift));
    CHECK_MSG(legacyForm.empty(), legacyForm.c_str());

    // The resource nvim sets. A modifier reported through the xterm ladder
    // reaches a program as a key it never asked about.
    modes.xtermKeyModifiers[4] = 2;
    const std::string underModifyOtherKeys = encoded(modes, held(K::VirtualKey::LeftShift, shift));
    CHECK_MSG(underModifyOtherKeys.empty(), underModifyOtherKeys.c_str());
    modes.xtermKeyModifiers[4] = 0;

    // Disambiguate is the flag a program sets without wanting modifiers, so it
    // sits between the two ends of this case rather than outside them.
    modes.kittyKeyboardFlags[0] = static_cast<std::uint16_t>(Ubiquitty::Kitty::KeyFlag::Disambiguate);
    const std::string underDisambiguate = encoded(modes, held(K::VirtualKey::LeftShift, shift));
    CHECK_MSG(underDisambiguate.empty(), underDisambiguate.c_str());

    modes.kittyKeyboardFlags[0] = static_cast<std::uint16_t>(Ubiquitty::Kitty::KeyFlag::Disambiguate)
        | static_cast<std::uint16_t>(Ubiquitty::Kitty::KeyFlag::ReportAll);
    CHECK(!encoded(modes, held(K::VirtualKey::LeftShift, shift)).empty());

    // The control: the gate names the modifier family and nothing else, so an
    // ordinary key still reports under the same flags.
    CHECK(!encoded(modes, bare(K::VirtualKey::UpArrow)).empty());
}

TEST(theShiftedGlyphIsWhatReachesTheWire)
{
    Rt::ModeSnapshot modes;

    Rt::KeyPress press;
    press.eventType = K::KeyEventType::Press;
    press.unshiftedCodePoint = ';';
    press.shiftedCodePoint = ':';
    press.modifiers = static_cast<std::uint32_t>(K::Keymap::keymapModifierShift);
    press.wasModified = true;
    press.text[0] = ':';
    press.textLength = 1;
    CHECK(encoded(modes, press) == ":");

    // Crossed, which is what the web page did, the byte is the key that was
    // not pressed. The two fields are not interchangeable and the length does
    // not say so — only the byte does.
    std::swap(press.unshiftedCodePoint, press.shiftedCodePoint);
    CHECK(encoded(modes, press) == ";");
}

/// Kitty reports the unshifted key, with the alternates as sub-parameters —
/// and only the alternates that differ from it.
///
/// A frontend reports what its platform knows and Core decides what survives:
/// an alternate equal to the key it qualifies carries nothing, which is the
/// `97:97` the web page reported for an unmodified `a`.
TEST(anAlternateThatRepeatsTheKeyIsOmitted)
{
    Rt::ModeSnapshot modes;
    // On the primary screen: report alternates, and report every key as an
    // escape code so an unmodified letter takes the CSI u form that carries
    // them.
    modes.kittyKeyboardFlags[0] = static_cast<std::uint16_t>(
        std::to_underlying(Ubiquitty::Kitty::KeyFlag::Disambiguate)
        | std::to_underlying(Ubiquitty::Kitty::KeyFlag::ReportAlternateKeys)
        | std::to_underlying(Ubiquitty::Kitty::KeyFlag::ReportAll));

    Rt::KeyPress press;
    press.eventType = K::KeyEventType::Press;
    press.unshiftedCodePoint = 'a';
    press.shiftedCodePoint = 'a';
    press.baseLayoutCodePoint = 'a';
    press.text[0] = 'a';
    press.textLength = 1;
    const std::string plain = encoded(modes, press);
    CHECK(plain.find(':') == std::string::npos);

    // A Dvorak `o` sits where the standard layout carries `s`, so that one is
    // an alternate and does reach the host.
    press.baseLayoutCodePoint = 's';
    CHECK(encoded(modes, press) != plain);
}

/// A control chord on a non-Latin layout is the KEY, not the letter.
///
/// Apple's Cyrillic, Greek and Hebrew layouts paint a letter on every ASCII
/// position, and Ctrl+ф is what a Russian user presses for Ctrl+A. The layout's
/// letter is what the frontend reports as the key; the Latin letter on the same
/// position is its base-layout key, from the ASCII-capable layout the menu bar
/// consults for ⌘ equivalents. Which byte the two become is decided here, so a
/// frontend that reports both never spells 0x01 itself.
TEST(aControlChordOnANonLatinLayoutFoldsTheKeyNotTheLetter)
{
    Rt::ModeSnapshot modes;

    Rt::KeyPress press;
    press.eventType = K::KeyEventType::Press;
    press.modifiers = static_cast<std::uint32_t>(K::Keymap::keymapModifierCtrl);
    press.wasModified = true;
    press.unshiftedCodePoint = 0x0444; // ф, on the A position
    press.shiftedCodePoint = 0x0444;
    press.text[0] = 0x01; // what the layout itself types for the chord
    press.textLength = 1;
    // No position reported: nothing says Ctrl+ф, and the typed control byte
    // is not read in its place — the text is Kitty's, not the encoder's.
    CHECK(encoded(modes, press).empty());

    press.baseLayoutCodePoint = 'a';
    CHECK(encoded(modes, press) == "\x01");

    press.unshiftedCodePoint = 0x043B; // л, on the K position
    press.shiftedCodePoint = 0x043B;
    press.baseLayoutCodePoint = 'k';
    CHECK(encoded(modes, press) == "\x0B");

    // A Latin layout reports the letter it types and the same letter as its
    // position; nothing changes for it.
    press.unshiftedCodePoint = 'k';
    press.shiftedCodePoint = 'k';
    press.baseLayoutCodePoint = 'k';
    CHECK(encoded(modes, press) == "\x0B");
}

/// A frontend holding the terminal itself gets the same answer.
///
/// The web page, the wscons console and the GTK shell hold a `Terminal` rather
/// than a snapshot of one. The overload samples it once, so no frontend spells
/// out which field or which array index answers for a mode — the spelling is
/// where they disagree, and `xtermKeyModifiers[0]` for a level that lives at
/// `[4]` is a mode that does nothing rather than a build failure.
TEST(aLiveTerminalAnswersAsItsSnapshotDoes)
{
    Ubiquitty::Terminal term;
    Ubiquitty::resize(term, 24, 80);

    char buffer[64];
    const auto encode = [&](const Rt::KeyPress& press) {
        const Rt::KeyEncoding encoding
            = Rt::encodeKey(term, press, K::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) });
        return std::string(buffer, static_cast<std::size_t>(encoding.length < 0 ? 0 : encoding.length));
    };

    CHECK(encode(bare(K::VirtualKey::UpArrow)) == "\033[A");
    term.modes.appCursorKeys = true;
    CHECK(encode(bare(K::VirtualKey::UpArrow)) == "\033OA");

    // Sampled, not cached: the mode moves and the next press follows it.
    term.modes.appCursorKeys = false;
    CHECK(encode(bare(K::VirtualKey::UpArrow)) == "\033[A");

    // modifyOtherKeys is category 4. Read from any other index it stays zero
    // however the host sets it, and the mode silently does nothing — which is
    // what the web page did, so `CSI > 4 ; 2 m` changed nothing there.
    Rt::KeyPress press;
    press.eventType = K::KeyEventType::Press;
    press.unshiftedCodePoint = 'a';
    press.shiftedCodePoint = 'a';
    press.modifiers = static_cast<std::uint32_t>(K::Keymap::keymapModifierCtrl);
    press.wasModified = true;
    CHECK(encode(press) == "\001");

    term.xtermKeyModifiers[4] = 2;
    CHECK(encode(press) == "\033[27;5;97~");
}

/// Every key resource a host can set changes the key it governs.
///
/// The parser accepts only the resources the encoder reads, so each is set here
/// the way a host sets it, as a control on the wire, and read back through a
/// press on the live terminal. A resource that stored and reported a value
/// without reaching this path would be a mode the host can query and no key
/// obeys — the shape a keypad key takes when it reaches the encoder as the digit
/// it types rather than as a keypad key.
TEST(eachKeyResourceAHostSetsChangesTheKeyItGoverns)
{
    Ubiquitty::Terminal term;
    Ubiquitty::resize(term, 24, 80);

    char buffer[64];
    const auto encode = [&](const Rt::KeyPress& press) {
        const Rt::KeyEncoding encoding
            = Rt::encodeKey(term, press, K::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) });
        return std::string(buffer, static_cast<std::size_t>(encoding.length < 0 ? 0 : encoding.length));
    };
    const auto modified = [](K::VirtualKey key, std::uint32_t modifiers, std::uint32_t text) {
        Rt::KeyPress press = bare(key);
        press.modifiers = modifiers;
        press.wasModified = true;
        press.unshiftedCodePoint = text;
        press.shiftedCodePoint = text;
        return press;
    };
    const std::uint32_t control = static_cast<std::uint32_t>(K::Keymap::keymapModifierCtrl);
    const std::uint32_t shift = static_cast<std::uint32_t>(K::Keymap::keymapModifierShift);

    // modifyCursorKeys, resource 1.
    const Rt::KeyPress controlUp = modified(K::VirtualKey::UpArrow, control, 0);
    CHECK(encode(controlUp) == "\033[1;5A");
    UbiquittyTest::feed(term, "\033[>1;3m");
    CHECK(encode(controlUp) == "\033[>1;5A");

    // modifyFunctionKeys, resource 2.
    const Rt::KeyPress controlF5 = modified(K::VirtualKey::F5, control, 0);
    CHECK(encode(controlF5) == "\033[15;5~");
    UbiquittyTest::feed(term, "\033[>2;3m");
    CHECK(encode(controlF5) == "\033[>15;5~");

    // modifyKeypadKeys, resource 3. The press carries the digit the key types.
    const Rt::KeyPress shiftKeypad1 = modified(K::VirtualKey::Keypad1, shift, '1');
    CHECK(encode(shiftKeypad1) == "1");
    UbiquittyTest::feed(term, "\033[>3;2m");
    CHECK(encode(shiftKeypad1) == "\033[1;2q");

    // modifyOtherKeys and formatOtherKeys, resource 4 of each table.
    Rt::KeyPress controlA;
    controlA.eventType = K::KeyEventType::Press;
    controlA.unshiftedCodePoint = 'a';
    controlA.shiftedCodePoint = 'a';
    controlA.modifiers = control;
    controlA.wasModified = true;
    CHECK(encode(controlA) == "\001");
    UbiquittyTest::feed(term, "\033[>4;2m");
    CHECK(encode(controlA) == "\033[27;5;97~");
    UbiquittyTest::feed(term, "\033[>4;1f");
    CHECK(encode(controlA) == "\033[97;5u");
}

// The DECUDK table rides beside the snapshot, not in it. The count in the
// snapshot is what decides whether the encoder consults the side channel, so a
// programmed key must still emit its string through the link overload, and a
// zero count must not reach for a table at all.
TEST(aProgrammedKeyResolvesThroughTheLinkSideChannel)
{
    Rt::RuntimeLink link;

    Ubiquitty::VT::UserDefinedKeys keys {};
    keys.count = 1;
    keys.entries[0].keyCode = 17; // F6
    keys.entries[0].length = 3;
    keys.entries[0].value[0] = 'x';
    keys.entries[0].value[1] = 'y';
    keys.entries[0].value[2] = 'z';
    link.userDefinedKeys.publish(keys);

    Rt::KeyPress press;
    press.virtualKey = K::VirtualKey::F6;

    char buffer[32] = { 0 };
    Rt::ModeSnapshot modes;

    // Zero count: the table is published but the snapshot does not claim it, so
    // F6 encodes as its default sequence rather than the programmed string.
    modes.userDefinedKeyCount = 0;
    const int unclaimed = Rt::encodeKey(modes, link, press, K::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) }).length;
    CHECK(unclaimed > 0);
    CHECK_MSG(std::memcmp(buffer, "xyz", 3) != 0, "a zero count must not reach the side channel");

    // Nonzero: the encoder reads the table and F6 emits what DECUDK programmed.
    modes.userDefinedKeyCount = keys.count;
    const int claimed = Rt::encodeKey(modes, link, press, K::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) }).length;
    CHECK(claimed == 3);
    CHECK(std::memcmp(buffer, "xyz", 3) == 0);

    // A table in hand that the modes do not count is not read either. The count
    // is the one switch: a caller holding a table that outlived its snapshot
    // cannot make the encoder consult it.
    modes.userDefinedKeyCount = 0;
    std::memset(buffer, 0, sizeof(buffer));
    const int uncounted = Rt::encodeKey(modes, press, K::Keymap::EncodedKeyBytes { buffer, sizeof(buffer) }, &keys).length;
    CHECK(uncounted > 0);
    CHECK_MSG(std::memcmp(buffer, "xyz", 3) != 0, "a table the modes do not count is not read");
}

namespace {

// A key press as both macOS arms read it off AppKit: the layout's letter with
// nothing applied, the letter with Shift applied, what the chord typed, and the
// Option bit in the modifier mask.
Rt::KeyPress optionChord(char32_t unshifted, char32_t shifted, char32_t typed, std::uint32_t extraModifiers = 0)
{
    Rt::KeyPress press;
    press.eventType = K::KeyEventType::Press;
    press.modifiers = static_cast<std::uint32_t>(K::Keymap::keymapModifierAlternate) | extraModifiers;
    press.wasModified = true;
    press.unshiftedCodePoint = unshifted;
    press.shiftedCodePoint = shifted;
    press.text[0] = typed;
    press.textLength = 1;
    return press;
}

// What reaches the host for a press once the frontend has applied the
// reader's Option setting, the order both macOS arms run.
std::string typedUnder(const Rt::ModeSnapshot& modes, const Rt::KeyPress& press, Ubiquitty::Input::OptionAsAlternate policy, Ubiquitty::Input::AlternateModifier alternate)
{
    return encoded(modes, Rt::pressUnderOptionAsAlternate(press, policy, alternate, Ubiquitty::Input::SuperModifier::Up));
}

} // namespace

/// An Option key that is not Meta types the character the layout puts on it.
///
/// German Option-L is `@`, Option-5 is `[` and Option-Shift-7 is a backslash.
/// The input method commits a one-character ASCII glyph back to the key path,
/// so these reach the encoder with Option still in the mask, and an encoder
/// that reads Option as Meta sends ESC and the base letter: ESC l for German
/// Option-L, which is readline's downcase-word.
TEST(anOptionKeyThatIsNotMetaTypesTheLayoutsCharacter)
{
    using Ubiquitty::Input::AlternateModifier;
    using Ubiquitty::Input::OptionAsAlternate;
    const std::uint32_t shift = static_cast<std::uint32_t>(K::Keymap::keymapModifierShift);
    const Rt::KeyPress optionL = optionChord('l', 'l', '@');
    Rt::ModeSnapshot modes;

    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::None, AlternateModifier::Left), "@");
    CHECK_STR_EQ(typedUnder(modes, optionChord('5', '5', '['), OptionAsAlternate::None, AlternateModifier::Left), "[");
    CHECK_STR_EQ(typedUnder(modes, optionChord('6', '6', ']'), OptionAsAlternate::None, AlternateModifier::Right), "]");
    CHECK_STR_EQ(typedUnder(modes, optionChord('7', '/', '\\', shift), OptionAsAlternate::None, AlternateModifier::Left), "\\");
    // An event that cannot say which side is held is still not Meta under none.
    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::None, AlternateModifier::Held), "@");

    // A reader who made that Option key Meta gets Meta.
    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::Both, AlternateModifier::Left), "\033l");
    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::Left, AlternateModifier::Left), "\033l");
    // One side Meta, the other still types.
    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::Left, AlternateModifier::Right), "@");
    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::Right, AlternateModifier::Left), "@");
}

/// The typed character is a character to every encoding a program can ask
/// for, not an Option chord.
///
/// modifyOtherKeys at level 2 and the Kitty disambiguate flag both report an
/// Alt chord in escape form, so a leftover Option bit turns `@` into a
/// sequence the program reads as Alt+L. Under Kitty's report-all flag the key
/// is its unshifted self with no modifier and `@` as the text, which is how
/// the same character reports when a Linux layout puts it on AltGr: the GTK
/// shell reports AltGr+Q with the layout's `q` as the key, `@` as the glyph and
/// no Alternate bit.
TEST(theTypedCharacterReportsAsTextUnderEveryKeyEncoding)
{
    using Ubiquitty::Input::AlternateModifier;
    using Ubiquitty::Input::OptionAsAlternate;
    const Rt::KeyPress optionL = optionChord('l', 'l', '@');

    Rt::ModeSnapshot modes;
    modes.xtermKeyModifiers[4] = 2;
    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::None, AlternateModifier::Left), "@");
    modes.xtermKeyModifiers[4] = 0;

    modes.kittyKeyboardFlags[0] = static_cast<std::uint16_t>(Ubiquitty::Kitty::KeyFlag::Disambiguate);
    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::None, AlternateModifier::Left), "@");

    modes.kittyKeyboardFlags[0] = static_cast<std::uint16_t>(Ubiquitty::Kitty::KeyFlag::Disambiguate)
        | static_cast<std::uint16_t>(Ubiquitty::Kitty::KeyFlag::ReportAll)
        | static_cast<std::uint16_t>(Ubiquitty::Kitty::KeyFlag::ReportText);
    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::None, AlternateModifier::Left), "\033[108;1;64u");

    Rt::KeyPress thirdLevelPress;
    thirdLevelPress.eventType = K::KeyEventType::Press;
    thirdLevelPress.unshiftedCodePoint = 'l';
    thirdLevelPress.shiftedCodePoint = '@';
    thirdLevelPress.text[0] = '@';
    thirdLevelPress.textLength = 1;
    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::None, AlternateModifier::Left), encoded(modes, thirdLevelPress));

    // Meta stays an Alt chord under the same flags.
    CHECK_STR_EQ(typedUnder(modes, optionL, OptionAsAlternate::Both, AlternateModifier::Left), "\033[108;3;64u");
}

/// Option on a key it does not shift to another character keeps its meaning.
///
/// Option-Left and Option-Backspace are readline's word motions whatever the
/// setting, a control chord keeps its control byte, a Command chord belongs to
/// the app, and a commit of more than one unit is not one character.
TEST(anOptionKeyThatShiftsNothingKeepsItsModifier)
{
    using Ubiquitty::Input::AlternateModifier;
    using Ubiquitty::Input::OptionAsAlternate;
    using Ubiquitty::Input::SuperModifier;
    const std::uint32_t ctrl = static_cast<std::uint32_t>(K::Keymap::keymapModifierCtrl);
    Rt::ModeSnapshot modes;

    // AppKit reports the arrow's private-use character with and without Option.
    Rt::KeyPress left = optionChord(0xF702, 0xF702, 0xF702);
    left.virtualKey = K::VirtualKey::LeftArrow;
    CHECK_STR_EQ(typedUnder(modes, left, OptionAsAlternate::None, AlternateModifier::Left), "\033b");

    Rt::KeyPress backspace = optionChord(0x7F, 0x7F, 0x7F);
    backspace.virtualKey = K::VirtualKey::Backspace;
    CHECK_STR_EQ(typedUnder(modes, backspace, OptionAsAlternate::None, AlternateModifier::Left), "\033\x7F");

    // Ctrl+Option+L as the layout types it: the control byte, not a glyph.
    CHECK_STR_EQ(typedUnder(modes, optionChord('l', 'l', 0x0C, ctrl), OptionAsAlternate::None, AlternateModifier::Left), "\033\x0C");
    // A press whose text is the key's own letter carries no third level, as
    // a synthesised Ctrl+Option+K reports it.
    CHECK_STR_EQ(typedUnder(modes, optionChord('k', 'k', 'k', ctrl), OptionAsAlternate::None, AlternateModifier::Left), "\033\x0B");

    const Rt::KeyPress optionL = optionChord('l', 'l', '@');
    const Rt::KeyPress withCommand = Rt::pressUnderOptionAsAlternate(optionL, OptionAsAlternate::None, AlternateModifier::Left, SuperModifier::Held);
    CHECK_EQ(withCommand.modifiers, optionL.modifiers);
    CHECK_EQ(withCommand.shiftedCodePoint, optionL.shiftedCodePoint);

    // Two units, the halves of an astral character: the glyph field would
    // carry half of it.
    Rt::KeyPress pair = optionChord('l', 'l', 0xD83D);
    pair.text[1] = 0xDE00;
    pair.textLength = 2;
    const Rt::KeyPress unsplit = Rt::pressUnderOptionAsAlternate(pair, OptionAsAlternate::None, AlternateModifier::Left, SuperModifier::Up);
    CHECK_EQ(unsplit.shiftedCodePoint, static_cast<std::uint32_t>('l'));

    // Without Option held there is nothing to reinterpret, even in a press
    // whose text and glyph differ for a reason of their own.
    Rt::KeyPress unmodified = optionChord('a', 'a', 'A');
    unmodified.modifiers = 0;
    const Rt::KeyPress untouched = Rt::pressUnderOptionAsAlternate(unmodified, OptionAsAlternate::None, AlternateModifier::Up, SuperModifier::Up);
    CHECK_EQ(untouched.shiftedCodePoint, static_cast<std::uint32_t>('a'));
}

/// A non-ASCII Option character that reaches the key path keeps its modifier.
///
/// The input method writes a non-ASCII commit through as composed text, so
/// one arrives here only when the key was swallowed on the way: a
/// `DefaultKeyBinding.dict` Option binding, or a frontend that bypasses the
/// input method. The legacy encoder writes one byte per character, so a
/// press that loses its Option bit sends U+00A0 as a lone 0xA0, which is not
/// UTF-8, and U+0192 as nothing. Such a press stays an Option chord.
TEST(aNonASCIIOptionCharacterKeepsItsModifier)
{
    using Ubiquitty::Input::AlternateModifier;
    using Ubiquitty::Input::OptionAsAlternate;
    Rt::ModeSnapshot modes;

    // US Option-F types U+0192, Option-Space U+00A0 and Option-S U+00DF.
    CHECK_STR_EQ(typedUnder(modes, optionChord('f', 'f', 0x192), OptionAsAlternate::None, AlternateModifier::Left), "\033f");
    CHECK_STR_EQ(typedUnder(modes, optionChord(' ', ' ', 0xA0), OptionAsAlternate::None, AlternateModifier::Left), "\033 ");
    CHECK_STR_EQ(typedUnder(modes, optionChord('s', 's', 0xDF), OptionAsAlternate::None, AlternateModifier::Left), "\033s");
    // DEL is not a typed character either.
    const Rt::KeyPress del = Rt::pressUnderOptionAsAlternate(optionChord('l', 'l', 0x7F), OptionAsAlternate::None, AlternateModifier::Left, Ubiquitty::Input::SuperModifier::Up);
    CHECK_EQ(del.shiftedCodePoint, static_cast<std::uint32_t>('l'));
}

TEST(keyboardTypesChangeFunctionAndEditingKeyBytes)
{
    Ubiquitty::Terminal term;
    const auto select = [&](std::string_view command) {
        UbiquittyTest::feed(term, command);
        return Rt::sampleModes(term);
    };
    auto modes = select("\033[?1051h");
    CHECK(encoded(modes, bare(K::VirtualKey::F1)) == "\033[224z");
    CHECK(encoded(modes, bare(K::VirtualKey::F12)) == "\033[193z");
    CHECK(encoded(modes, bare(K::VirtualKey::Home)) == "\033[214z");
    CHECK(encoded(modes, bare(K::VirtualKey::UpArrow)) == "\033OA");
    modes = select("\033[?1052h");
    CHECK(encoded(modes, bare(K::VirtualKey::F1)) == "\033p");
    CHECK(encoded(modes, bare(K::VirtualKey::F8)) == "\033w");
    CHECK(encoded(modes, bare(K::VirtualKey::Home)) == "\033h");
    modes = select("\033[?1053h");
    CHECK(encoded(modes, bare(K::VirtualKey::F1)) == "\033[M");
    CHECK(encoded(modes, bare(K::VirtualKey::F12)) == "\033[X");
    CHECK(encoded(modes, bare(K::VirtualKey::Insert)) == "\033[L");
    modes = select("\033[?1060h");
    CHECK(encoded(modes, bare(K::VirtualKey::F1)) == "\033[11~");
    auto press = bare(K::VirtualKey::F1);
    press.modifiers = K::Keymap::keymapModifierCtrl;
    press.wasModified = true;
    CHECK(encoded(modes, press) == "\033[23~");
    modes = select("\033[?1061h");
    CHECK(encoded(modes, bare(K::VirtualKey::F1)) == "\033OP");
    CHECK(encoded(modes, bare(K::VirtualKey::Home)) == "\033[1~");
    CHECK(encoded(modes, bare(K::VirtualKey::End)) == "\033[4~");
    modes = select("\033[?1051l");
    CHECK(encoded(modes, bare(K::VirtualKey::Home)) == "\033[H");
}

TEST(numLockSelectsNumericKeysOnlyWhenEnabled)
{
    Ubiquitty::Terminal term;
    UbiquittyTest::feed(term, "\033=");
    auto press = bare(K::VirtualKey::Keypad5);
    press.unshiftedCodePoint = '5';
    press.shiftedCodePoint = '5';
    press.modifiers = K::Keymap::keymapModifierNumLock;
    CHECK(encoded(Rt::sampleModes(term), press) == "5");
    UbiquittyTest::feed(term, "\033[?1035l");
    CHECK(encoded(Rt::sampleModes(term), press) == "\033Ou");
    UbiquittyTest::feed(term, "\033[?1035h");
    CHECK(encoded(Rt::sampleModes(term), press) == "5");
}

TEST(modifyKeyboardEnablesModifierParametersForVT220Keys)
{
    Ubiquitty::Terminal term;
    UbiquittyTest::feed(term, "\033[?1061h");
    auto press = bare(K::VirtualKey::UpArrow);
    press.modifiers = K::Keymap::keymapModifierCtrl;
    press.wasModified = true;
    CHECK(encoded(Rt::sampleModes(term), press) == "\033[A");
    UbiquittyTest::feed(term, "\033[>0;2m");
    CHECK(encoded(Rt::sampleModes(term), press) == "\033[1;5A");
    UbiquittyTest::feed(term, "\033[>0n");
    CHECK(encoded(Rt::sampleModes(term), press) == "\033[A");
}

TEST(termcapQueriesMatchTheSelectedKeyboardEntry)
{
    Ubiquitty::Terminal term;
    UbiquittyTest::feed(term, "\033[?1050h\033P+p6c696e7578\033\\");
    CHECK(encoded(Rt::sampleModes(term), bare(K::VirtualKey::F1)) == "\033[[A");
    CHECK(encoded(Rt::sampleModes(term), bare(K::VirtualKey::F5)) == "\033[[E");
    CHECK(encoded(Rt::sampleModes(term), bare(K::VirtualKey::F6)) == "\033[17~");
    UbiquittyTest::feed(term, "\033P+q6b31\033\\");
    std::array<char, 64> reply {};
    const int replyLength = UbiquittyTest::drain(term, reply);
    CHECK(std::string_view(reply.data(), static_cast<std::size_t>(replyLength)) == "\033P1+r6b31=1B5B5B41\033\\");
    UbiquittyTest::feed(term, "\033P+p626f677573\033\\");
    CHECK(encoded(Rt::sampleModes(term), bare(K::VirtualKey::F1)) == "\033[[A");
    UbiquittyTest::feed(term, "\033P+p756269717569747479\033\\");
    CHECK(encoded(Rt::sampleModes(term), bare(K::VirtualKey::F1)) == "\033OP");
}

TEST(extendedModifierKeysUseTheConfiguredXtermFormat)
{
    Ubiquitty::Terminal term;
    const auto press = bare(K::VirtualKey::LeftShift);
    CHECK(encoded(Rt::sampleModes(term), press).empty());
    UbiquittyTest::feed(term, "\033[>6;1m");
    CHECK(encoded(Rt::sampleModes(term), press).empty());
    UbiquittyTest::feed(term, "\033[>6;4m");
    CHECK(encoded(Rt::sampleModes(term), press) == "\033[27;1;58081~");
    UbiquittyTest::feed(term, "\033[>6;1f");
    CHECK(encoded(Rt::sampleModes(term), press) == "\033[58081;1u");
    UbiquittyTest::feed(term, "\033[>6n");
    CHECK(encoded(Rt::sampleModes(term), press).empty());
}

TEST(vt220KeypadRemappingAndModifierMask)
{
    Ubiquitty::Terminal term;
    UbiquittyTest::feed(term, "\033[?1061h\033=");
    auto press = bare(K::VirtualKey::KeypadAdd);
    press.unshiftedCodePoint = '+';
    press.shiftedCodePoint = '+';
    CHECK(encoded(Rt::sampleModes(term), press) == "\033Ol");
    press.modifiers = K::Keymap::keymapModifierCtrl;
    press.wasModified = true;
    CHECK(encoded(Rt::sampleModes(term), press) == "\033Om");
    press = bare(K::VirtualKey::Keypad5);
    press.unshiftedCodePoint = '5';
    press.shiftedCodePoint = '5';
    press.modifiers = K::Keymap::keymapModifierCtrl;
    press.wasModified = true;
    CHECK(encoded(Rt::sampleModes(term), press) == "\033Ou");
    UbiquittyTest::feed(term, "\033[>0;1m");
    CHECK(encoded(Rt::sampleModes(term), press) == "\033O5u");
    UbiquittyTest::feed(term, "\033[>0n");
    CHECK(encoded(Rt::sampleModes(term), press) == "\033Ou");
}

TEST(extendedSpecialKeysRespectFormatAndKittyPrecedence)
{
    Ubiquitty::Terminal term;
    auto press = bare(K::VirtualKey::None);
    press.unshiftedCodePoint = K::Keymap::keymapKeyScrollLock;
    press.shiftedCodePoint = K::Keymap::keymapKeyScrollLock;
    UbiquittyTest::feed(term, "\033[>7;4m");
    CHECK(encoded(Rt::sampleModes(term), press) == "\033[27;1;57876~");
    UbiquittyTest::feed(term, "\033[>7;1f");
    CHECK(encoded(Rt::sampleModes(term), press) == "\033[57876;1u");
    UbiquittyTest::feed(term, "\033[>8u");
    CHECK(encoded(Rt::sampleModes(term), press) == "\033[57359;1u");
}

TEST(keyPositionsAreIndependentOfTheTypedLayout)
{
    Rt::ModeSnapshot modes;
    modes.shouldReportKeyPositions = true;
    Rt::KeyPress press = bare(K::VirtualKey::KeyA);
    press.unshiftedCodePoint = 0x0444;
    press.text[0] = 0x0444;
    press.textLength = 1;
    CHECK(encoded(modes, press) == "\033_:C0100\033\\");
    press.modifiers = (2u << K::positionModifierShift) | K::Keymap::keymapModifierShift;
    CHECK(encoded(modes, press) == "\033_:C0102\033\\");
    CHECK(encoded(modes, bare(K::VirtualKey::Space)) == "\033_:A0200\033\\");
    CHECK(encoded(modes, bare(K::VirtualKey::Return)) == "\033_:C1300\033\\");
    CHECK(encoded(modes, bare(K::VirtualKey::None)).empty());
    press.eventType = K::KeyEventType::Release;
    CHECK(encoded(modes, press).empty());
    press.eventType = K::KeyEventType::Press;
    modes.shouldTransmit8BitC1 = true;
    CHECK(encoded(modes, press) == "\237:C0102\234");
    modes.shouldReportKeyPositions = false;
    press.unshiftedCodePoint = 'a';
    press.shiftedCodePoint = 'A';
    CHECK(encoded(modes, press) == "A");
}

TEST(keyPositionModifierActionsControlPressAndReleaseReports)
{
    Rt::ModeSnapshot modes;
    modes.shouldReportKeyPositions = true;
    auto shift = bare(K::VirtualKey::LeftShift);
    shift.modifiers = 1u << K::positionModifierShift;
    CHECK(encoded(modes, shift).empty());
    modes.keyPositionReportedModifiers |= 1;
    CHECK(encoded(modes, shift) == "\033_:B9901\033\\");
    shift.eventType = K::KeyEventType::Release;
    shift.modifiers = 0;
    CHECK(encoded(modes, shift) == "\033_:B9900\033\\");
    modes.keyPositionDisabledModifiers = 1;
    auto key = bare(K::VirtualKey::KeyB);
    key.modifiers = 1u << K::positionModifierShift;
    CHECK(encoded(modes, key) == "\033_:B0500\033\\");
    modes.autoRepeat = false;
    key.isRepeat = true;
    CHECK(encoded(modes, key).empty());
}

TEST(keyPositionModesResetAndModifierSelectionUsesPairs)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 4, 20);
    UbiquittyTest::feed(terminal, "\033[0;3+r");
    CHECK(terminal.keyPositionReportedModifiers == 0x30);
    CHECK(terminal.keyPositionDisabledModifiers == 0);
    UbiquittyTest::feed(terminal, "\033[?81h\033[1;2;6;3+r");
    auto modes = Rt::sampleModes(terminal);
    CHECK(modes.shouldReportKeyPositions);
    CHECK(modes.keyPositionReportedModifiers == 0x11);
    CHECK(modes.keyPositionDisabledModifiers == 0x20);
    UbiquittyTest::feed(terminal, "\033[0;0+r");
    CHECK(terminal.keyPositionReportedModifiers == 0x30);
    CHECK(terminal.keyPositionDisabledModifiers == 0);
    UbiquittyTest::feed(terminal, "\033[!p");
    CHECK(!terminal.modes.shouldReportKeyPositions);
}

TEST(keyPositionReportsRefuseAShortOutputBuffer)
{
    Rt::ModeSnapshot modes;
    modes.shouldReportKeyPositions = true;
    char bytes[12];
    std::fill(std::begin(bytes), std::end(bytes), '!');
    const auto result = Rt::encodeKey(modes, bare(K::VirtualKey::KeyA), std::span(bytes).first(9), nullptr);
    CHECK(result.length == 0);
    CHECK(std::ranges::all_of(bytes, [](char byte) { return byte == '!'; }));
}

UBIQUITTY_TEST_MAIN()
