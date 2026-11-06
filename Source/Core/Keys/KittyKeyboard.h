// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The kitty keyboard protocol extension (sw.kovidgoyal.net/kitty/keyboard-protocol),
// encoder side: a progressive enhancement over the standard VT/xterm key encoding.
// An app opts in per screen via CSI = / > / < u flags; the terminal then reports
// some key events as CSI u escape codes. This header owns the encoder-side policy
// and wire format (the flag set itself lives in Core/Kitty/KeyboardFlags.h).
//
// Emission rules encodeKey() implements:
// - REPORT_ALT_KEYS: append shiftedKey / baseLayoutKey as colon sub-params.
// - REPORT_EVENTS: emit the eventType subparameter; drop release events when unset.
// - REPORT_TEXT: append the associated-text codePoints group (C0/DEL filtered out).

#include "Core/Keys/KeyEvent.h"
#include "Core/Kitty/KeyboardFlags.h"
#include "Core/Modes/TerminalModes.h"

#include <cstdint>
#include <span>

namespace Ubiquitty::Kitty {

/// Answers whether a key produces text: any non-functional code point from
/// space up — printable ASCII and every letter beyond it, é, ф, CJK. The C0
/// range and DEL are carried by functional identities.
///
/// - Parameter keyCode: the key's code point.
/// - Returns: true for a text-producing key.
bool isTextProducingKey(uint32_t keyCode);

/// Answers whether a printable key is reported as `CSI u` rather than text.
///
/// Text is suppressed only under report-all, or while a modifier other than
/// Shift is held: Shift alone is how capital letters are typed, so it still
/// produces text.
///
/// - Parameters:
///   - modifiers: the modifier mask held with the key.
///   - flags: the mode's active Kitty keyboard flags; an empty set is the
///     protocol inactive, and reports nothing as an escape code.
/// - Returns: true when the key belongs on the escape-code path.
bool printableReportedAsEscapeCode(uint32_t modifiers, KeyFlagSet flags);

/// Answers whether the Kitty extension claims a functional key press.
///
/// It claims one under report-all, under any modifier, and — for the four keys
/// whose legacy encodings collide with C0 controls, Tab, Enter, Backspace and
/// Escape — under disambiguate. A key with a named xterm form (the arrows, the
/// function keys, the editing keys) stays on the standard encoder, because
/// applications recognise those spellings.
///
/// - Parameters:
///   - keyCode: the key's Kitty codePoint.
///   - modifiers: the modifier mask held with the key.
///   - flags: the mode's active Kitty keyboard flags; an empty set claims
///     nothing.
/// - Returns: true when the key belongs on the escape-code path.
bool functionalReportedAsEscapeCode(uint32_t keyCode, uint32_t modifiers, KeyFlagSet flags);

/// Answers whether a repeat or release transition is reported at all.
///
/// Both conditions must hold: the mode reports events, and the key is one that
/// reports as an escape code. A printable key held unmodified or with Shift
/// alone travels as text, and text has no release channel, so its key-up
/// emits nothing.
///
/// - Parameters:
///   - keyCode: the key's Kitty codePoint.
///   - modifiers: the modifier mask held with the key.
///   - flags: the mode's active Kitty keyboard flags.
/// - Returns: true when the transition produces bytes.
bool reportsNonPressEvent(uint32_t keyCode, uint32_t modifiers, KeyFlagSet flags);

/// Encodes one key event as a Kitty CSI-u sequence.
///
/// Operates on logical key codePoints: a printable key is its unshifted Unicode
/// codePoint, a functional key its PUA codePoint (the
/// `Keys::Keymap::keymapKey*` constants).
///
/// - Parameters:
///   - keyCode: The primary codePoint.
///   - shiftedKey: The shifted alternate, or 0 when it equals `keyCode`.
///   - baseLayoutKey: The base-layout alternate, or 0 when it equals `keyCode`.
///   - modifiers: An `Keys::Keymap::keymapModifier*` bitmask.
///   - eventType: Press, repeat, or release.
///   - associatedText: The codePoints the key produces, empty when none.
///   - flags: The active Kitty keyboard-protocol flag set, which decides what
///     the sequence carries and whether a release reports at all.
///   - output: Destination for the sequence.
/// - Returns: The byte count written, or 0 when suppressed — a release with
///   REPORT_EVENTS off, or an `output` too short for the sequence.
int encodeKey(uint32_t keyCode, uint32_t shiftedKey, uint32_t baseLayoutKey, uint32_t modifiers, Keys::KeyEventType eventType, std::span<const uint32_t> associatedText, KeyFlagSet flags, std::span<char> output);

// Bare-modifier CSI-u forwarding for Doom-style binds is Runtime::encodeKey's,
// like every other key: a frontend reports the modifier as a KeyPress and the
// REPORT_ALL gate is applied there, so one encoder answers for every frontend.

} // namespace Ubiquitty::Kitty
