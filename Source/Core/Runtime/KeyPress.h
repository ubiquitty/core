// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Keys/KeyEvent.h"
#include <cstdint>

namespace Ubiquitty::Runtime {

/// What a frontend knows about a key press, and nothing more.
///
/// The contract between a frontend and the terminal for keyboard input. A
/// frontend fills this in from its own event and calls
/// ``TerminalSession::encodeKey``; it decides nothing about what the bytes are.
///
/// Everything the encoding depends on beyond these fields is the terminal's and
/// is resolved behind that call: DECCKM, the Kitty flag set for the screen in
/// front of the user, DECARM repeat suppression, the application-keypad SS3
/// form, the xterm modifier ladder and its formats, VT52, the meta/alternate
/// escape discipline, eight-bit input, the delete-key rule, DECUDK, and whether
/// SRM leaves the echo to us.
///
/// Stated as a struct rather than a parameter list: the encoder underneath
/// takes eleven arguments, ten of which are the terminal's own state, and a
/// frontend that reads them off the model and threads them back in can get any
/// of them wrong, or skip the gathering and spell the bytes by hand. A frontend
/// cannot get DECCKM wrong here because it never names it.
struct KeyPress {
    /// The key, from the frontend's own platform table.
    Keys::VirtualKey virtualKey {};

    /// Low byte: `KEYMAP_MOD_*` flags. High byte: DECEKBD sided modifier state.
    /// Synthetic events can leave the high byte zero; Core uses left-side defaults.
    std::uint32_t modifiers = 0;

    /// The layout's code point with every modifier removed — Shift included.
    ///
    /// The key's reported identity: what a Kitty host receives as the primary
    /// code point, and what the control and meta ladders fold.
    std::uint32_t unshiftedCodePoint = 0;

    /// The glyph the key produces with Shift applied, when it differs.
    ///
    /// Reported alongside the unshifted key rather than in place of it: the
    /// legacy encoder puts this on the wire — Shift+`;` types `:` — while the
    /// Kitty encoder carries it as the shifted alternate. A frontend that
    /// supplies only one of the two supplies this one everywhere.
    std::uint32_t shiftedCodePoint = 0;

    /// The code point the standard layout carries at this physical position,
    /// or 0 when the platform does not report position.
    ///
    /// Kitty's base-layout alternate, for a program that wants the key rather
    /// than the letter. Distinct from ``unshiftedCodePoint``, which is the
    /// USER's layout: on a Dvorak keyboard the physical `S` position reports
    /// `o` unshifted and `s` here.
    ///
    /// Also what a control chord folds when the layout's letter has no byte
    /// form: Ctrl+ф on a Cyrillic layout is Ctrl+A, and this says so. A
    /// platform whose one layout-independent answer is its ASCII-capable
    /// layout — macOS — reports that: under a non-Latin layout it is the
    /// Latin layout the user types on; under a Latin layout it is the layout
    /// in use and repeats the key, which is discarded.
    std::uint32_t baseLayoutCodePoint = 0;

    /// The code points the key actually produced, for a host that asked for
    /// REPORT_TEXT and wants the composed character.
    std::uint32_t text[Keys::maxKeyEventTextLength] {};

    /// How many of ``text`` are live.
    int textLength = 0;

    /// Press or release. A repeat arrives as a press with ``isRepeat`` set,
    /// because whether a repeat is its own event is a terminal mode rather
    /// than a fact the platform can settle.
    Keys::KeyEventType eventType = Keys::KeyEventType::Press;

    /// Whether the platform reported this as an auto-repeat.
    bool isRepeat = false;

    /// Whether any modifier was held, whether or not it reached ``modifiers``.
    /// A path asking whether the press was bare must not depend on the two
    /// agreeing; they do today and are still different questions.
    bool wasModified = false;
};

/// What the terminal wants done with a key press.
struct KeyEncoding {
    /// Bytes written to the caller's buffer. Zero means no local bytes: a
    /// dead key, a release this model does not report, or a repeat DECARM
    /// suppressed. A remote host still decides under its own live modes
    /// whether the semantic event produces bytes there.
    int length = 0;

    /// Whether the host will not echo (SRM reset), so the bytes have to go
    /// back through the model to appear. False during a password prompt
    /// whatever the echo mode says.
    bool shouldEchoLocally = false;

    /// The transition as the terminal resolved it: a press the host wants
    /// reported as a repeat comes back as one.
    Keys::KeyEventType eventType = Keys::KeyEventType::Press;

    /// Whether `TerminalSession::sendKey` handed the input to its endpoint.
    /// This is admission, not delivery: a remote event may wait for reconnect.
    /// Encoding alone leaves this false; local byte length does not determine
    /// whether a remote event was accepted.
    bool wasAccepted = false;
};

} // namespace Ubiquitty::Runtime
