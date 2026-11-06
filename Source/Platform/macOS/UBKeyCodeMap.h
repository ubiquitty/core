// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// ⚠️  No new code in this file. Write Swift, in the Swift arm.
//
// Objective-C++ on the macOS frontend is being retired: the frontend's Swift
// is where logic lives, and it reaches Core
// directly through cxx-interop. Add nothing here. Do not grow an @objc seam to
// reach Swift from here either — existing seams shrink toward thin accessors
// and never grow. Migrate the surrounding code to Swift instead.

// UBKeyCodeMap.h — the macOS HIToolbox key code to portable identity map.
//
// The one place in this frontend that names a native hardware code. Everything
// downstream of it is Core's: `Keys::VirtualKey` feeds the functional and
// keypad code-point tables in `Core/Keys/KeyTranslation.h`, which feed the
// encoders in `Core/Keys/Keymap.h`, which both frontends share.
//
// A header rather than a translation unit because the Swift terminal surface
// reads the same table through the module map. Held in one file so the two
// languages cannot answer differently for a key: a Swift copy of this switch
// would be a second table that only diverges once someone edits one of them,
// and no test compares a frontend's key map against another frontend's.

#pragma once
#include "Core/Keys/KeyTranslation.h"

namespace Ubiquitty::macOS {

/// The native key codes this frontend recognises.
///
/// HIToolbox virtual key codes, the values AppKit reports in `NSEvent.keyCode`.
/// Only the keys with a portable identity appear: a printable key's identity is
/// its typed character, which the caller derives from the event instead.
///
/// Spelled out rather than included from Carbon's `Events.h`: that umbrella
/// pulls `Lists.h`, whose `Cell` typedef collides with Core's `Cell` struct in
/// every translation unit that reaches both.
enum class NativeKey : unsigned short {
    Return = 0x24,
    Tab = 0x30,
    Delete = 0x33, // the key labeled "delete" (backspace)
    Escape = 0x35,
    KeypadEnter = 0x4C,
    F5 = 0x60,
    F6 = 0x61,
    F7 = 0x62,
    F3 = 0x63,
    F8 = 0x64,
    F9 = 0x65,
    F11 = 0x67,
    F10 = 0x6D,
    F12 = 0x6F,
    F4 = 0x76,
    F2 = 0x78,
    F1 = 0x7A,
    LeftArrow = 0x7B,
    RightArrow = 0x7C,
    DownArrow = 0x7D,
    UpArrow = 0x7E,
    ForwardDelete = 0x75,
    Home = 0x73,
    End = 0x77,
    PageUp = 0x74,
    PageDown = 0x79,
    KP_0 = 0x52,
    KP_1 = 0x53,
    KP_2 = 0x54,
    KP_3 = 0x55,
    KP_4 = 0x56,
    KP_5 = 0x57,
    KP_6 = 0x58,
    KP_7 = 0x59,
    KP_8 = 0x5B,
    KP_9 = 0x5C,
    KP_Decimal = 0x41,
    KP_Multiply = 0x43,
    KP_Plus = 0x45,
    KP_Clear = 0x47,
    KP_Divide = 0x4B,
    KP_Minus = 0x4E,
    KP_Equals = 0x51,
    RightCommand = 0x36,
    Command = 0x37,
    Shift = 0x38,
    Option = 0x3A,
    Control = 0x3B,
    RightShift = 0x3C,
    RightOption = 0x3D,
    RightControl = 0x3E,
};

/// Maps a native key code to its portable identity.
///
/// - Parameter key: The code AppKit reported.
/// - Returns: The portable identity, or `None` for a printable key — whose code
///   point the caller derives from the typed characters — and for a key with no
///   portable identity. KP_Clear is the second case: it has no Kitty code point,
///   so it falls through to the typed character like any printable key.
[[nodiscard]] inline Keys::VirtualKey portableKeyFor(NativeKey key)
{
    using PortableKey = Keys::VirtualKey;
    switch (key) {
    case NativeKey::Return:
        return PortableKey::Return;
    case NativeKey::KeypadEnter:
        return PortableKey::KeypadEnter;
    case NativeKey::Tab:
        return PortableKey::Tab;
    case NativeKey::Delete:
        return PortableKey::Backspace; // Mac "delete" = BS
    case NativeKey::ForwardDelete:
        return PortableKey::ForwardDelete;
    case NativeKey::Escape:
        return PortableKey::Escape;
    case NativeKey::LeftArrow:
        return PortableKey::LeftArrow;
    case NativeKey::RightArrow:
        return PortableKey::RightArrow;
    case NativeKey::UpArrow:
        return PortableKey::UpArrow;
    case NativeKey::DownArrow:
        return PortableKey::DownArrow;
    case NativeKey::Home:
        return PortableKey::Home;
    case NativeKey::End:
        return PortableKey::End;
    case NativeKey::PageUp:
        return PortableKey::PageUp;
    case NativeKey::PageDown:
        return PortableKey::PageDown;
    case NativeKey::F1:
        return PortableKey::F1;
    case NativeKey::F2:
        return PortableKey::F2;
    case NativeKey::F3:
        return PortableKey::F3;
    case NativeKey::F4:
        return PortableKey::F4;
    case NativeKey::F5:
        return PortableKey::F5;
    case NativeKey::F6:
        return PortableKey::F6;
    case NativeKey::F7:
        return PortableKey::F7;
    case NativeKey::F8:
        return PortableKey::F8;
    case NativeKey::F9:
        return PortableKey::F9;
    case NativeKey::F10:
        return PortableKey::F10;
    case NativeKey::F11:
        return PortableKey::F11;
    case NativeKey::F12:
        return PortableKey::F12;
    case NativeKey::KP_0:
        return PortableKey::Keypad0;
    case NativeKey::KP_1:
        return PortableKey::Keypad1;
    case NativeKey::KP_2:
        return PortableKey::Keypad2;
    case NativeKey::KP_3:
        return PortableKey::Keypad3;
    case NativeKey::KP_4:
        return PortableKey::Keypad4;
    case NativeKey::KP_5:
        return PortableKey::Keypad5;
    case NativeKey::KP_6:
        return PortableKey::Keypad6;
    case NativeKey::KP_7:
        return PortableKey::Keypad7;
    case NativeKey::KP_8:
        return PortableKey::Keypad8;
    case NativeKey::KP_9:
        return PortableKey::Keypad9;
    case NativeKey::KP_Decimal:
        return PortableKey::KeypadDecimal;
    case NativeKey::KP_Multiply:
        return PortableKey::KeypadMultiply;
    case NativeKey::KP_Plus:
        return PortableKey::KeypadAdd;
    case NativeKey::KP_Minus:
        return PortableKey::KeypadSubtract;
    case NativeKey::KP_Divide:
        return PortableKey::KeypadDivide;
    case NativeKey::KP_Equals:
        return PortableKey::KeypadEqual;
    case NativeKey::Shift:
        return PortableKey::LeftShift;
    case NativeKey::RightShift:
        return PortableKey::RightShift;
    case NativeKey::Control:
        return PortableKey::LeftControl;
    case NativeKey::RightControl:
        return PortableKey::RightControl;
    case NativeKey::Option:
        return PortableKey::LeftAlternate;
    case NativeKey::RightOption:
        return PortableKey::RightAlternate;
    case NativeKey::Command:
        return PortableKey::LeftSuper; // Apple Command → Super
    case NativeKey::RightCommand:
        return PortableKey::RightSuper;
    default:
        return PortableKey::None;
    }
}

/// Maps a raw `NSEvent.keyCode` to its portable identity.
///
/// The spelling a Swift caller uses: AppKit hands it a `UInt16` and the enum
/// above is what this frontend calls the value, so the cast belongs here rather
/// than at each call.
///
/// - Parameter keyCode: The value from `NSEvent.keyCode`.
/// - Returns: The portable identity, or `None`.
[[nodiscard]] inline Keys::VirtualKey portableKeyForCode(unsigned short keyCode)
{
    return portableKeyFor(static_cast<NativeKey>(keyCode));
}

/// The device-dependent modifier bit AppKit flips when a physical modifier key
/// transitions.
///
/// `flagsChanged:` names the toggled key by its key code, but the general
/// `NSEventModifierFlag*` cannot tell the two sides apart or say which way the
/// transition went: releasing the left Shift while the right one is held leaves
/// the general shift bit set, and a reader of that bit calls the release a
/// press. The per-side bits AppKit reports in `modifierFlags`' low byte carry
/// both (IOKit's `NX_DEVICE*KEYMASK`), so the bit for the key that changed is
/// set on the way down and clear on the way up whatever the other side is
/// doing.
///
/// Native rather than portable because the bits are AppKit's own and have no
/// analogue elsewhere; shared between the shells because a second copy is a
/// second answer about which side moved.
///
/// - Parameter key: The portable identity of the key that transitioned.
/// - Returns: The bit to test, or zero for a key with no side (caps lock, fn).
[[nodiscard]] inline unsigned long modifierDeviceMask(Keys::VirtualKey key)
{
    switch (key) {
    case Keys::VirtualKey::LeftShift:
        return 0x00000002;
    case Keys::VirtualKey::RightShift:
        return 0x00000004;
    case Keys::VirtualKey::LeftControl:
        return 0x00000001;
    case Keys::VirtualKey::RightControl:
        return 0x00002000;
    case Keys::VirtualKey::LeftAlternate:
        return 0x00000020;
    case Keys::VirtualKey::RightAlternate:
        return 0x00000040;
    case Keys::VirtualKey::LeftSuper:
        return 0x00000008;
    case Keys::VirtualKey::RightSuper:
        return 0x00000010;
    default:
        return 0;
    }
}

} // namespace Ubiquitty::macOS
