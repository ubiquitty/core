// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Input/ModifierPolicy.h"
#include "Core/Keys/KeyTranslation.h"
#include "Core/Keys/Keymap.h"
#include "Core/Keys/KittyKeyboard.h"
#include "Core/Runtime/KeyPress.h"
#include "Core/Runtime/ModeSnapshot.h"
#include "Core/Runtime/RuntimeLink.h"

namespace Ubiquitty::Runtime {

/// Encodes one key press for a terminal in these modes./// Whether bytes the user sends are also shown locally: SRM reset (mode 12)
/// makes this a half-duplex terminal that echoes its own input, and a
/// password prompt keeps the secret off the screen whatever the mode says.
/// The session re-reads the prompt off the model beside this: one stale
/// snapshot here is one character of a password on screen.
[[nodiscard]] inline bool shouldEchoInputLocally(const ModeSnapshot& modes)
{
    return !modes.sendReceiveMode && !modes.isPasswordPromptActive;
}

///
/// The whole key path, so no frontend holds any of it: the Kitty flag set for
/// the screen in front of the user, DECARM repeat suppression and the
/// repeat/press decision, the application-keypad SS3 form, which table reports
/// the key, the xterm modifier ladder, VT52, the meta/alternate escape
/// discipline, DECUDK, and whether SRM leaves the echo to the caller.
///
/// A free function over the snapshot rather than a method on the session,
/// because a frontend reaches its modes differently — the Swift arm through a
/// drive, a headless one directly — and all of them have a snapshot.
///
/// The bytes are returned rather than sent: sending is the part that genuinely
/// differs, since a local session writes to its PTY, a remote one frames a
/// Sync input event and a carrier-backed one hands them to its channel.
///
/// - Parameters:
///   - modes: The terminal's modes, as sampled.
///   - press: What the frontend knows; see ``KeyPress``.
///   - output: Where the bytes land.
///   - userDefinedKeys: The DECUDK table the modes count, or nullptr. Read only
///     while `modes.userDefinedKeyCount` is nonzero, so a caller whose modes
///     count no key passes nullptr.
/// - Returns: How many bytes, whether to echo, and the resolved transition.
[[nodiscard]] inline KeyEncoding encodeKey(const ModeSnapshot& modes, const KeyPress& press, Keys::Keymap::EncodedKeyBytes output, const VT::UserDefinedKeys* userDefinedKeys)
{

    if (modes.shouldReportKeyPositions) {
        if (output.size() < (modes.shouldTransmit8BitC1 ? 8u : 10u))
            return {};
        if (press.isRepeat && !modes.autoRepeat)
            return {};
        const std::string_view position = Keys::isoKeyPosition(press.virtualKey);
        const std::uint8_t modifier = Keys::isoModifierKey(press.virtualKey);
        if (position.empty() || (modifier && !(modes.keyPositionReportedModifiers & modifier))
            || (!modifier && press.eventType == Keys::KeyEventType::Release))
            return {};
        const std::uint8_t held = Keys::isoKeyModifiers(press.modifiers) & ~modes.keyPositionDisabledModifiers;
        constexpr std::string_view hex = "0123456789ABCDEF";
        int length = 0;
        if (modes.shouldTransmit8BitC1)
            output[length++] = static_cast<char>(0x9f);
        else {
            output[length++] = '\033';
            output[length++] = '_';
        }
        output[length++] = ':';
        for (const char byte : position)
            output[length++] = byte;
        output[length++] = hex[held >> 4];
        output[length++] = hex[held & 15];
        if (modes.shouldTransmit8BitC1)
            output[length++] = static_cast<char>(0x9c);
        else {
            output[length++] = '\033';
            output[length++] = '\\';
        }
        return { .length = length, .eventType = press.eventType };
    }

    // The flag set belongs to the screen the user is looking at: a program
    // that enabled the protocol on the alternate screen has not enabled it
    // for the shell behind it.
    const Kitty::KeyFlagSet kittyFlags = Kitty::KeyFlagSet::fromRaw(
        modes.kittyKeyboardFlags[modes.alternateScreenActive ? 1 : 0]);
    const bool reportsEvents = kittyFlags.contains(Kitty::KeyFlag::ReportEvents);

    // DECARM reset: the host opted out of auto-repeat, so an OS repeat
    // emits nothing — unless the host consumes Kitty repeat events, where
    // the repeat is a real event rather than a duplicate press.
    if (press.isRepeat && !modes.autoRepeat && !reportsEvents)
        return {};

    KeyEncoding encoding;
    encoding.eventType = press.eventType;
    if (press.eventType == Keys::KeyEventType::Press && press.isRepeat && reportsEvents)
        encoding.eventType = Keys::KeyEventType::Repeat;
    // A release sends no character to echo.
    encoding.shouldEchoLocally = shouldEchoInputLocally(modes)
        && encoding.eventType != Keys::KeyEventType::Release;

    // DECNKM/DECPAM: an unmodified keypad key emits its SS3 sequence
    // instead of the plain character. A modified press falls through so
    // modifyOtherKeys and control folding still apply, and Kitty takes
    // precedence when the host enabled any flag, because its CSI u form
    // already carries the keypad identity.
    if (modes.applicationKeypad && kittyFlags.isEmpty() && !press.wasModified
        && modes.keyboardType != Keys::KeyboardType::Legacy && modes.keyboardType != Keys::KeyboardType::VT220
        && !(modes.shouldHonorNumLock && (press.modifiers & Keys::Keymap::keymapModifierNumLock))) {
        // KeypadEnter is Functional in the shared table — so an ordinary
        // Enter emits a newline — but under DECPAM xterm sends the
        // application-keypad final ESC O M. The classification stays and the
        // gate names the key, so only this path sends it: Enter off the
        // keypad is a newline everywhere else.
        const std::uint32_t keypadCodePoint = press.virtualKey == Keys::VirtualKey::KeypadEnter
            ? static_cast<std::uint32_t>(Keys::Keymap::keymapKeyKeypadEnter)
            : Keys::keypadKeyCodePoint(press.virtualKey);
        const int written = Keys::Keymap::applicationKeypadSS3(keypadCodePoint, output);
        if (written > 0) {
            encoding.length = written;
            return encoding;
        }
    }

    // A bare modifier is a key only to a host that asked for one —
    // ``Kitty::bareModifierKeyEvent``'s gate, applied here because a frontend
    // that forwards modifiers reaches the encoder through this path and not
    // through that function. It cannot be left to the dead-key check below:
    // ``reportedKeyCodePoint`` answers a modifier with its Kitty code point
    // under both encoders, so the press carries a private-use code point and
    // encodes as an ordinary key — which a program that never asked for
    // modifiers receives as a character.
    if (Keys::keyFamily(press.virtualKey) == Keys::KeyFamily::Modifier
        && !kittyFlags.contains(Kitty::KeyFlag::ReportAll)
        && !(kittyFlags.isEmpty() && modes.xtermKeyModifiers[6] == 4))
        return {};

    Keys::KeyEvent event;
    event.eventType = encoding.eventType;
    event.modifiers = press.modifiers & Keys::characterModifierMask;
    event.virtualKey = press.virtualKey;
    event.wasModified = press.wasModified;
    // Which table answers is the terminal's decision: a keypad key reports
    // its own code point only under Kitty, because the legacy encoder has
    // no byte form for one. The functional table alone answers zero for
    // every keypad key, and KP_5 would then report as the digit 5.
    const Keys::KeyEncoder encoder
        = kittyFlags.isEmpty() ? Keys::KeyEncoder::Legacy : Keys::KeyEncoder::Kitty;
    const Keys::ReportedKey resolved = Keys::resolveReportedKey(
        Keys::reportedKeyCodePoint(press.virtualKey, encoder),
        press.unshiftedCodePoint, press.shiftedCodePoint);
    if (resolved.keyCode == 0)
        return {}; // a dead key: no character and no functional identity
    event.keyCode = resolved.keyCode;
    if (resolved.shiftedKey != 0)
        event.shiftedKey = resolved.shiftedKey;
    // An alternate that repeats the key it is an alternate for is not one, and
    // Kitty's REPORT_ALT_KEYS form says so: the sub-param is omitted when the
    // base layout agrees. Suppressed here rather than by each frontend, so no
    // frontend reports `97:97` for an unmodified `a`.
    if (press.baseLayoutCodePoint != 0 && press.baseLayoutCodePoint != resolved.keyCode)
        event.baseLayoutKey = press.baseLayoutCodePoint;
    const int textLength = press.textLength < 0
        ? 0
        : (press.textLength > Keys::maxKeyEventTextLength ? Keys::maxKeyEventTextLength : press.textLength);
    for (int index = 0; index < textLength; ++index)
        event.textCodePoints[index] = press.text[index];
    event.textLength = textLength;

    Keys::Keymap::EncodeModes encodeModes;
    // DEC keyboard layouts apply their keypad remapping in Keymap.
    encodeModes.applicationKeypad = modes.applicationKeypad
        && (modes.keyboardType == Keys::KeyboardType::Legacy || modes.keyboardType == Keys::KeyboardType::VT220);
    encodeModes.shouldHonorNumLock = modes.shouldHonorNumLock;
    encodeModes.keyboardType = modes.keyboardType;
    encodeModes.termcapProfile = modes.termcapProfile;
    encodeModes.modifyKeyboard = modes.xtermKeyModifiers[0];
    encodeModes.modifyModifierKeys = modes.xtermKeyModifiers[6];
    encodeModes.modifySpecialKeys = modes.xtermKeyModifiers[7];
    encodeModes.formatModifierKeys = modes.xtermKeyFormats[6];
    encodeModes.formatSpecialKeys = modes.xtermKeyFormats[7];
    encodeModes.vt52Mode = modes.vt52Mode;
    encodeModes.lineFeedNewLineMode = modes.lineFeedNewLineMode;
    encodeModes.metaSendsEscape = modes.metaSendsEscape;
    encodeModes.alternateSendsEscape = modes.alternateSendsEscape;
    encodeModes.eightBitInput = modes.eightBitInput;
    encodeModes.deleteKeySendsDelete = modes.deleteKeySendsDelete;

    // Category 4 is xterm's "other (printable) keys"; 1, 2 and 3 are
    // cursor, function and keypad. The host sets each through
    // CSI > Ps ; Pv m, and the encoder places the modifier per category.
    encoding.length = Keys::Keymap::encodeKeyEvent(event,
        modes.applicationCursorKeys ? Ubiquitty::Modes::CursorKeyMode::Application
                                    : Ubiquitty::Modes::CursorKeyMode::Normal,
        kittyFlags, modes.xtermKeyModifiers[4], modes.xtermKeyFormats[4],
        modes.xtermKeyModifiers[1], modes.xtermKeyModifiers[2], modes.xtermKeyModifiers[3],
        output, encodeModes, modes.userDefinedKeyCount > 0 ? userDefinedKeys : nullptr);
    if (encoding.length <= 0)
        return {};
    return encoding;
}

/// Encodes one key press against modes the caller already holds, taking the
/// DECUDK table from the link only when those modes say there is one.
///
/// For a frontend that caches its own ModeSnapshot per pane. The runtime
/// publishes the table before it hands out any snapshot that counts it, on
/// the link or in a DrainOutcome, so the table read here is at least as new
/// as the count that asked for it.
///
/// - Parameters:
///   - modes: The terminal's modes, as the caller holds them.
///   - link: The same session's link.
///   - press: What the frontend knows; see ``KeyPress``.
///   - output: Where the bytes land.
/// - Returns: How many bytes, whether to echo, and the resolved transition.
[[nodiscard]] inline KeyEncoding encodeKey(const ModeSnapshot& modes, const RuntimeLink& link, const KeyPress& press, Keys::Keymap::EncodedKeyBytes output)
{
    if (modes.userDefinedKeyCount == 0)
        return encodeKey(modes, press, output, nullptr);
    const VT::UserDefinedKeys keys = link.userDefinedKeys.read();
    return encodeKey(modes, press, output, &keys);
}

/// Encodes one key press from a published snapshot, taking the DECUDK table
/// from its side channel only when the snapshot says there is one.
///
/// The overload a frontend that reads the link should use: the common path is
/// one snapshot read, and the table is copied only for a session that
/// programmed keys.
///
/// - Parameters:
///   - link: The session's link.
///   - press: What the frontend knows; see ``KeyPress``.
///   - output: Where the bytes land.
/// - Returns: How many bytes, whether to echo, and the resolved transition.
[[nodiscard]] inline KeyEncoding encodeKey(const RuntimeLink& link, const KeyPress& press, Keys::Keymap::EncodedKeyBytes output)
{
    return encodeKey(link.reads.read().modes, link, press, output);
}

/// Encodes one key press for a live terminal.
///
/// The overload for a frontend that holds the terminal rather than a snapshot
/// of it — the web page, the wscons console, the GTK shell. Samples the modes
/// and answers exactly as the snapshot form does.
///
/// - Parameters:
///   - term: The terminal the key was pressed on.
///   - press: What the frontend knows; see ``KeyPress``.
///   - output: Where the bytes land.
/// - Returns: How many bytes, whether to echo, and the resolved transition.
[[nodiscard]] inline KeyEncoding encodeKey(const Ubiquitty::Terminal& term, const KeyPress& press, Keys::Keymap::EncodedKeyBytes output)
{
    return encodeKey(sampleModes(term), press, output, sampleUserDefinedKeys(term));
}

/// Returns a press as `[keys] option_as_alt` says to read its Option key.
///
/// An Option key the setting does not make Meta is the layout's third level:
/// German Option-L types `@` and Option-5 types `[`. When it shifted the key to
/// another printable ASCII character, that character is what the reader typed
/// and Option is not a modifier of it, so the press loses the Alternate bit and
/// carries the character as the glyph the key produced. The legacy encoder
/// then sends the character, and modifyOtherKeys and the Kitty flags see a
/// typed character rather than an Alt chord. The key's unshifted identity stays, which is how
/// the GTK shell reports a character a Linux layout puts on AltGr. With the
/// bit left in, German Option-L reaches the shell as ESC l.
///
/// The press comes back unchanged when a held Option key is Meta, when Command
/// is held, or when Option did not shift the key to another printable ASCII
/// character. Option-Left and Option-Backspace keep their word motions, a
/// control chord keeps its control byte, and a dead key has no character to
/// carry. A non-ASCII character is the input method's to write as composed
/// text; one that reaches this path had its key swallowed on the way, and the
/// legacy encoder writes one byte per character, so it stays an Option chord.
///
/// - Parameters:
///   - press: The press as the frontend read it off its event.
///   - policy: The reader's `[keys] option_as_alt`.
///   - alternate: Which Option keys the event holds.
///   - super: Whether Command is held.
/// - Returns: The press to encode.
[[nodiscard]] inline KeyPress pressUnderOptionAsAlternate(KeyPress press, Input::OptionAsAlternate policy, Input::AlternateModifier alternate, Input::SuperModifier super)
{
    if (alternate == Input::AlternateModifier::Up || super == Input::SuperModifier::Held)
        return press;
    if (Input::alternateActsAsMeta(policy, alternate))
        return press;
    // More than one unit is not one character the glyph field can carry; the
    // input method writes such a commit through as composed text.
    if (press.textLength != 1)
        return press;
    const std::uint32_t typed = press.text[0];
    if (Keys::isComposedCommit(1, typed) || typed == press.shiftedCodePoint)
        return press;
    press.modifiers &= ~static_cast<std::uint32_t>(Keys::Keymap::keymapModifierAlternate);
    press.shiftedCodePoint = typed;
    return press;
}

} // namespace Ubiquitty::Runtime
