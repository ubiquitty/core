// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Keys/KittyKeyboard.h"
#include "Core/Keys/Keymap.h"
#include "Core/Modes/TerminalModes.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>

using Ubiquitty::Keys::KeyEventType;

namespace Ubiquitty::Kitty {

// Append printf-style text to `output`, never past its end.
// Returns the new total written length, or -1 on overflow.
static int appendFormat(std::span<char> output, int written, const char* fmt, ...)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    va_list ap;
    int n;
    if (written < 0 || written >= outputCapacity)
        return -1;
    va_start(ap, fmt);
    n = std::vsnprintf(outputBuffer + written, static_cast<size_t>(outputCapacity - written), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= outputCapacity - written)
        return -1;
    return written + n;
}

// A keyCode the protocol treats as functional: the PUA functional range
// (57344–63743), or one of the four keys it carries by their C0 codePoints
// because the standard byte is ambiguous (Tab collides with Ctrl-I, Enter
// with Ctrl-M, Escape opens every escape sequence, Backspace with Ctrl-H/?).
static bool isFunctionalKey(uint32_t keyCode) { return (keyCode >= 57344 && keyCode <= 63743) || keyCode == Keys::Keymap::keymapKeyTab || keyCode == Keys::Keymap::keymapKeyEnter || keyCode == Keys::Keymap::keymapKeyBackspace || keyCode == Keys::Keymap::keymapKeyEscape; }

static bool isAmbiguousControlKey(uint32_t keyCode) { return keyCode == Keys::Keymap::keymapKeyTab || keyCode == Keys::Keymap::keymapKeyEnter || keyCode == Keys::Keymap::keymapKeyBackspace || keyCode == Keys::Keymap::keymapKeyEscape; }

bool printableReportedAsEscapeCode(uint32_t modifiers, KeyFlagSet flags)
{
    if (flags.isEmpty())
        return false;
    if (flags.contains(KeyFlag::ReportAll))
        return true;
    return (modifiers & (Keys::Keymap::keymapModifierCtrl | Keys::Keymap::keymapModifierAlternate | Keys::Keymap::keymapModifierSuper | Keys::Keymap::keymapModifierMeta | Keys::Keymap::keymapModifierHyper)) != 0;
}

bool functionalReportedAsEscapeCode(uint32_t keyCode, uint32_t modifiers, KeyFlagSet flags)
{
    if (flags.isEmpty() || !isFunctionalKey(keyCode))
        return false;
    if (flags.contains(KeyFlag::ReportAll))
        return true;
    if (modifiers != 0)
        return true;
    return flags.contains(KeyFlag::Disambiguate) && isAmbiguousControlKey(keyCode);
}

// A text-producing key: any non-functional codePoint from space up — printable
// ASCII, and every non-PUA codePoint beyond it (é, CJK, …). The C0 range and
// DEL are carried by functional identities (Tab/Enter/Escape/Backspace).
bool isTextProducingKey(uint32_t keyCode) { return keyCode >= Keys::Keymap::minPrintableASCII && !isFunctionalKey(keyCode); }

bool reportsNonPressEvent(uint32_t keyCode, uint32_t modifiers, KeyFlagSet flags)
{
    if (!flags.contains(KeyFlag::ReportEvents))
        return false;
    // Event types are only reported for keys reported as escape codes. A text
    // key (no report-all, shift-only / unmodified) carries no release/repeat
    // channel — its key-up must not leak a CSI u release. This is about
    // text-producing keys as a class, not the ASCII range: é has no release
    // channel either.
    if (isTextProducingKey(keyCode) && !printableReportedAsEscapeCode(modifiers, flags))
        return false;
    return true;
}

// Spec: "the Lock modifiers are not reported for text producing keys, to keep
// them usable in legacy programs. To get lock modifiers for all keys use the
// Report all keys as escape codes enhancement."
static uint32_t textKeyReportedModifiers(uint32_t modifiers, KeyFlagSet flags)
{
    if (flags.contains(KeyFlag::ReportAll))
        return modifiers;
    return modifiers & ~static_cast<uint32_t>(Keys::Keymap::keymapModifierCapsLock | Keys::Keymap::keymapModifierNumLock);
}

int encodeKey(uint32_t keyCode, uint32_t shiftedKey, uint32_t baseLayoutKey, uint32_t modifiers, KeyEventType eventType, std::span<const uint32_t> associatedText, KeyFlagSet flags, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    int modifierValue;
    bool wantAlternate;
    bool wantEvents;
    bool wantText;
    bool hasSecondGroup;
    bool hasThirdGroup;
    int written;

    if (!outputBuffer || outputCapacity <= 0)
        return 0;

    // Text-producing keys drop the Lock modifiers unless ReportAll is active
    // (functional keys — including the ambiguous four — report them always).
    if (isTextProducingKey(keyCode))
        modifiers = textKeyReportedModifiers(modifiers, flags);

    wantAlternate = flags.contains(KeyFlag::ReportAlternateKeys);
    wantEvents = flags.contains(KeyFlag::ReportEvents);
    wantText = flags.contains(KeyFlag::ReportText) && !associatedText.empty();
    // REPORT_ALL (flag 8) forces the full CSI u shape — an explicit modifier
    // group even when no modifier is active — but it does not enable the
    // alternate-key subparameters: the kitty spec gates keycode:shifted:base on
    // ReportAlternateKeys (flag 4) alone. So it feeds hasSecondGroup below, not
    // wantAlternate (which stays flag 4 and gates the first-group subparams).
    bool wantReportAll = flags.contains(KeyFlag::ReportAll);

    // Release events are reported only when REPORT_EVENTS is on. Repeat
    // events likewise piggy-back on REPORT_EVENTS (xterm always sends
    // press; the kitty protocol elevates repeat/release to first-class
    // events the application must opt into).
    if (!wantEvents && eventType != KeyEventType::Press)
        return 0;

    modifierValue = Keys::Keymap::numericModifierParameter(modifiers);
    // Always emit the modifier group when alternate-key reporting is on
    // (kitty requires the modifier parameter to anchor the subparameters) or
    // when event reporting is on (we always emit the event subparameter,
    // even for press, so apps that opt into REPORT_EVENTS get a
    // consistent shape).
    hasSecondGroup = (modifierValue != 1) || wantEvents || wantAlternate || wantReportAll;
    hasThirdGroup = wantText;

    // Spec: "the shifted key must be present only if shift is also present in
    // the modifiers" — a populated shiftedKey field on a Ctrl-only chord must
    // not leak (an application would misread CSI 97:65;5u as Ctrl+Shift+a).
    // The base-layout alternate carries no such requirement.
    if ((modifiers & Keys::Keymap::keymapModifierShift) == 0)
        shiftedKey = 0;

    // First parameter group: keyCode[:shifted[:base]]
    written = appendFormat(output, 0, "\033[%lu", static_cast<unsigned long>(keyCode));
    if (written < 0)
        return 0;
    if (wantAlternate && (shiftedKey != 0 || baseLayoutKey != 0)) {
        if (shiftedKey != 0)
            written = appendFormat(output, written, ":%lu", static_cast<unsigned long>(shiftedKey));
        else
            written = appendFormat(output, written, ":");
        if (written < 0)
            return 0;
        if (baseLayoutKey != 0) {
            written = appendFormat(output, written, ":%lu", static_cast<unsigned long>(baseLayoutKey));
            if (written < 0)
                return 0;
        }
    }

    // Second parameter group: modifiers[:eventType]
    if (hasSecondGroup || hasThirdGroup) {
        written = appendFormat(output, written, ";%d", modifierValue);
        if (written < 0)
            return 0;
        if (wantEvents) {
            written = appendFormat(output, written, ":%d", std::to_underlying(eventType));
            if (written < 0)
                return 0;
        }
    }

    // Third parameter group: associated text codePoints (colon-separated).
    // Per the kitty spec associated text never contains control codes — a
    // ctrl-combo's OS-produced C0 byte (Ctrl+A -> 0x01) must not leak into the
    // text group, where the application would read it as a spurious inserted
    // character. Skip C0 / DEL; if nothing printable remains, emit no third
    // group at all (the key is still fully described by the first two groups).
    if (hasThirdGroup) {
        bool emittedAny = false;
        for (uint32_t codePoint : associatedText) {
            if (Keys::Keymap::isControlCodePoint(codePoint))
                continue;
            written = appendFormat(output, written, "%s%lu", emittedAny ? ":" : ";", static_cast<unsigned long>(codePoint));
            if (written < 0)
                return 0;
            emittedAny = true;
        }
    }

    // Final byte — `u` is the kitty CSI u terminator.
    if (written >= outputCapacity)
        return 0;
    outputBuffer[written++] = 'u';
    return written;
}

} // namespace Ubiquitty::Kitty
