// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Cursor::Style — the DECSCUSR (CSI Ps SP q) cursor style carried in
// Terminal::cursorStyle. The enumerators are the DEC/xterm Ps codes, so the field
// serializes byte-identically over the sync wire (putU8 / u8) with a cast at
// the transport boundary; the DTOs that carry it stay uint8_t.
//

#include <cstdint>

namespace Ubiquitty::Cursor {

/// A DEC VT510 DECSCUSR cursor style.
///
/// The enumerators are the DEC and xterm Ps codes, so the value crosses the sync
/// wire as a byte with a cast at the transport boundary and nothing to map.
/// Odd values blink and even are steady; 1 and 2 block, 3 and 4 underline, 5 and
/// 6 bar.
///
/// `FollowUserPreference` (0) means "follow the user's configured shape and
/// blink". It is the power-on, RIS and DECSTR state, and what xterm's DECSCUSR `Ps 7` extension selects, handing the cursor back. It is an
/// internal sentinel: a program-observable reply must resolve it first, or the
/// raw 0 tells an application "blinking block" while a steady bar is on screen.
enum class Style : uint8_t {
    FollowUserPreference = 0,
    BlinkingBlock = 1,
    SteadyBlock = 2,
    BlinkingUnderline = 3,
    SteadyUnderline = 4,
    BlinkingBar = 5,
    SteadyBar = 6,
};

/// Maps an internal or Sync cursor-style code to its enum value.
///
/// Zero is the internal `FollowUserPreference` sentinel; a code above 6
/// selects a blinking block. The DECSCUSR handler separately maps the
/// protocol defaults and its preference-restoring extension.
///
/// - Parameter code: The Ps code, or the raw byte a peer sent.
/// - Returns: The style; never an out-of-range enumerator.
constexpr Style styleFromCode(int code)
{
    if (code < 0 || code > static_cast<int>(Style::SteadyBar))
        return Style::BlinkingBlock;
    return static_cast<Style>(code);
}

/// Returns whether a DECSCUSR style blinks of itself.
///
/// - Parameter style: The style to test.
/// - Returns: True for the odd enumerators. `FollowUserPreference` has no
///   intrinsic blink and answers false; `resolve` falls back to the user
///   preference and mode 12 for that case.
constexpr bool isBlinking(Style style) { return (static_cast<uint8_t>(style) & 1u) != 0; }

// Every enumerator, so a consumer spelling the blinking styles out as a list
// disagrees with this at compile time rather than in a repaint that never gets
// scheduled. DECSCUSR is clamped to 0..6 on the wire, which is what makes a list
// of 1, 3 and 5 look equivalent — it is equivalent only while the enum ends at 6.
static_assert(!isBlinking(Style::FollowUserPreference));
static_assert(isBlinking(Style::BlinkingBlock));
static_assert(!isBlinking(Style::SteadyBlock));
static_assert(isBlinking(Style::BlinkingUnderline));
static_assert(!isBlinking(Style::SteadyUnderline));
static_assert(isBlinking(Style::BlinkingBar));
static_assert(!isBlinking(Style::SteadyBar));

} // namespace Ubiquitty::Cursor
