// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The shared Native-VK → Kitty codePoint table and key-family classifier. Each
// frontend gets a NATIVE key code (kVK_* on macOS, GDK keyval on Linux) and turns
// it into the Kitty-protocol key identity the portable encoder consumes. The seam
// splits in two: (1) the platform maps its native code to the portable VirtualKey
// enum below; (2) this header maps VirtualKey → the Kitty codePoint via the shared
// tables. No platform type appears here.

#include "Base/SwiftInterop.h"
#include "Core/Keys/Keymap.h"

#include <string_view>

namespace Ubiquitty::Keys {

/// A key's identity, independent of the window system and active layout.
/// Printable identities describe physical positions; their character values
/// come from the layout. None means that no physical identity is available.
/// Numeric values are stable because Sync carries them between host and client.
enum class VirtualKey : uint16_t {
    None = 0,

    // Control / editing keys carried by their Kitty identity.
    Return = 1,
    KeypadEnter = 2,
    Tab = 3,
    Backspace = 4, // the "delete"/backspace key (Mac label "delete")
    ForwardDelete = 5, // the forward-delete key (Mac label "fn+delete")
    Escape = 6,
    Insert = 7,
    LeftArrow = 8,
    RightArrow = 9,
    UpArrow = 10,
    DownArrow = 11,
    Home = 12,
    End = 13,
    PageUp = 14,
    PageDown = 15,

    // Function keys F1..F12 (the block every frontend can deliver).
    F1 = 16,
    F2 = 17,
    F3 = 18,
    F4 = 19,
    F5 = 20,
    F6 = 21,
    F7 = 22,
    F8 = 23,
    F9 = 24,
    F10 = 25,
    F11 = 26,
    F12 = 27,

    // Keypad keys (the numeric keypad / application-keypad block).
    Keypad0 = 28,
    Keypad1 = 29,
    Keypad2 = 30,
    Keypad3 = 31,
    Keypad4 = 32,
    Keypad5 = 33,
    Keypad6 = 34,
    Keypad7 = 35,
    Keypad8 = 36,
    Keypad9 = 37,
    KeypadDecimal = 38,
    KeypadDivide = 39,
    KeypadMultiply = 40,
    KeypadSubtract = 41,
    KeypadAdd = 42,
    KeypadEqual = 43,

    // Bare modifier keys, left/right distinguished (Kitty REPORT_ALL).
    LeftShift = 44,
    RightShift = 45,
    LeftControl = 46,
    RightControl = 47,
    LeftAlternate = 48,
    RightAlternate = 49,
    LeftSuper = 50,
    RightSuper = 51,

    // USB keyboard-page positions; character values still come from the layout.
    KeyA = 0x104,
    KeyB = 0x105,
    KeyC = 0x106,
    KeyD = 0x107,
    KeyE = 0x108,
    KeyF = 0x109,
    KeyG = 0x10a,
    KeyH = 0x10b,
    KeyI = 0x10c,
    KeyJ = 0x10d,
    KeyK = 0x10e,
    KeyL = 0x10f,
    KeyM = 0x110,
    KeyN = 0x111,
    KeyO = 0x112,
    KeyP = 0x113,
    KeyQ = 0x114,
    KeyR = 0x115,
    KeyS = 0x116,
    KeyT = 0x117,
    KeyU = 0x118,
    KeyV = 0x119,
    KeyW = 0x11a,
    KeyX = 0x11b,
    KeyY = 0x11c,
    KeyZ = 0x11d,
    Digit1 = 0x11e,
    Digit2 = 0x11f,
    Digit3 = 0x120,
    Digit4 = 0x121,
    Digit5 = 0x122,
    Digit6 = 0x123,
    Digit7 = 0x124,
    Digit8 = 0x125,
    Digit9 = 0x126,
    Digit0 = 0x127,
    Space = 0x12c,
    Minus = 0x12d,
    Equal = 0x12e,
    LeftBracket = 0x12f,
    RightBracket = 0x130,
    Backslash = 0x131,
    InternationalHash = 0x132,
    Semicolon = 0x133,
    Quote = 0x134,
    Grave = 0x135,
    Comma = 0x136,
    Period = 0x137,
    Slash = 0x138,
    CapsLock = 0x139,
    InternationalBackslash = 0x164,
};

/// Resolves a printable USB keyboard-page usage to its physical key identity.
/// - Parameter usage: A HID keyboard usage, independent of the active layout.
/// - Returns: Its identity, or None when the usage is not a printable position.
[[nodiscard]] inline VirtualKey printableKeyForHID(std::uint16_t usage)
{
    if ((usage >= 0x04 && usage <= 0x27) || (usage >= 0x2c && usage <= 0x39) || usage == 0x64)
        return static_cast<VirtualKey>(0x100 + usage);
    return VirtualKey::None;
}

/// Resolves a key to the VT keyboard's ISO position.
/// - Parameter key: The physical or functional key identity.
/// - Returns: Three ASCII position characters, or an empty view for an unknown key.
[[nodiscard]] inline std::string_view isoKeyPosition(VirtualKey key)
{
    switch (key) {
    case VirtualKey::KeyA:
        return "C01";
    case VirtualKey::KeyS:
        return "C02";
    case VirtualKey::KeyD:
        return "C03";
    case VirtualKey::KeyF:
        return "C04";
    case VirtualKey::KeyG:
        return "C05";
    case VirtualKey::KeyH:
        return "C06";
    case VirtualKey::KeyJ:
        return "C07";
    case VirtualKey::KeyK:
        return "C08";
    case VirtualKey::KeyL:
        return "C09";
    case VirtualKey::KeyZ:
        return "B01";
    case VirtualKey::KeyX:
        return "B02";
    case VirtualKey::KeyC:
        return "B03";
    case VirtualKey::KeyV:
        return "B04";
    case VirtualKey::KeyB:
        return "B05";
    case VirtualKey::KeyN:
        return "B06";
    case VirtualKey::KeyM:
        return "B07";
    case VirtualKey::KeyQ:
        return "D01";
    case VirtualKey::KeyW:
        return "D02";
    case VirtualKey::KeyE:
        return "D03";
    case VirtualKey::KeyR:
        return "D04";
    case VirtualKey::KeyT:
        return "D05";
    case VirtualKey::KeyY:
        return "D06";
    case VirtualKey::KeyU:
        return "D07";
    case VirtualKey::KeyI:
        return "D08";
    case VirtualKey::KeyO:
        return "D09";
    case VirtualKey::KeyP:
        return "D10";
    case VirtualKey::Digit1:
        return "E01";
    case VirtualKey::Digit2:
        return "E02";
    case VirtualKey::Digit3:
        return "E03";
    case VirtualKey::Digit4:
        return "E04";
    case VirtualKey::Digit5:
        return "E05";
    case VirtualKey::Digit6:
        return "E06";
    case VirtualKey::Digit7:
        return "E07";
    case VirtualKey::Digit8:
        return "E08";
    case VirtualKey::Digit9:
        return "E09";
    case VirtualKey::Digit0:
        return "E10";
    case VirtualKey::Space:
        return "A02";
    case VirtualKey::Minus:
        return "E11";
    case VirtualKey::Equal:
        return "E12";
    case VirtualKey::LeftBracket:
        return "D11";
    case VirtualKey::RightBracket:
        return "D12";
    case VirtualKey::Backslash:
        return "C12";
    case VirtualKey::InternationalHash:
        return "C12";
    case VirtualKey::Semicolon:
        return "C10";
    case VirtualKey::Quote:
        return "C11";
    case VirtualKey::Grave:
        return "E00";
    case VirtualKey::Comma:
        return "B08";
    case VirtualKey::Period:
        return "B09";
    case VirtualKey::Slash:
        return "B10";
    case VirtualKey::CapsLock:
        return "C00";
    case VirtualKey::InternationalBackslash:
        return "B00";
    case VirtualKey::Return:
        return "C13";
    case VirtualKey::Tab:
        return "D00";
    case VirtualKey::Backspace:
        return "E13";
    case VirtualKey::LeftShift:
        return "B99";
    case VirtualKey::RightShift:
        return "B11";
    case VirtualKey::LeftControl:
        return "C99";
    case VirtualKey::RightControl:
        return "C99";
    case VirtualKey::LeftAlternate:
        return "A01";
    case VirtualKey::RightAlternate:
        return "A09";
    case VirtualKey::LeftSuper:
        return "A00";
    case VirtualKey::RightSuper:
        return "A10";
    case VirtualKey::LeftArrow:
        return "B16";
    case VirtualKey::DownArrow:
        return "B17";
    case VirtualKey::UpArrow:
        return "C17";
    case VirtualKey::RightArrow:
        return "B18";
    case VirtualKey::Home:
        return "E16";
    case VirtualKey::Insert:
        return "E17";
    case VirtualKey::ForwardDelete:
        return "E18";
    case VirtualKey::End:
        return "D16";
    case VirtualKey::PageUp:
        return "D17";
    case VirtualKey::PageDown:
        return "D18";
    case VirtualKey::KeypadDivide:
        return "E21";
    case VirtualKey::KeypadMultiply:
        return "E22";
    case VirtualKey::KeypadSubtract:
        return "E23";
    case VirtualKey::KeypadAdd:
        return "C23";
    case VirtualKey::KeypadEnter:
        return "A23";
    case VirtualKey::KeypadDecimal:
        return "A22";
    case VirtualKey::Keypad0:
        return "A20";
    case VirtualKey::KeypadEqual:
        return "E20";
    case VirtualKey::Escape:
        return "E00";
    case VirtualKey::Keypad1:
        return "B20";
    case VirtualKey::Keypad2:
        return "B21";
    case VirtualKey::Keypad3:
        return "B22";
    case VirtualKey::Keypad4:
        return "C20";
    case VirtualKey::Keypad5:
        return "C21";
    case VirtualKey::Keypad6:
        return "C22";
    case VirtualKey::Keypad7:
        return "D20";
    case VirtualKey::Keypad8:
        return "D21";
    case VirtualKey::Keypad9:
        return "D22";
    case VirtualKey::F1:
        return "G00";
    case VirtualKey::F2:
        return "G01";
    case VirtualKey::F3:
        return "G02";
    case VirtualKey::F4:
        return "G03";
    case VirtualKey::F5:
        return "G05";
    case VirtualKey::F6:
        return "G06";
    case VirtualKey::F7:
        return "G07";
    case VirtualKey::F8:
        return "G08";
    case VirtualKey::F9:
        return "G09";
    case VirtualKey::F10:
        return "G11";
    case VirtualKey::F11:
        return "G12";
    case VirtualKey::F12:
        return "G13";
    default:
        return {};
    }
}

/// The upper byte of a key event's modifier word carries DEC's sided state.
/// The low byte retains the ordinary key-encoder modifier flags.
inline constexpr std::uint32_t positionModifierShift = 8;
inline constexpr std::uint32_t characterModifierMask = 0xff;

/// Resolves sided modifiers, with left-side defaults for synthetic events.
/// - Parameter modifiers: The packed key-event modifier word.
/// - Returns: DECEKBD's eight modifier-state bits.
[[nodiscard]] inline std::uint8_t isoKeyModifiers(std::uint32_t modifiers)
{
    if (modifiers >> positionModifierShift)
        return static_cast<std::uint8_t>(modifiers >> positionModifierShift);
    using namespace Keymap;
    return ((modifiers & keymapModifierShift) ? 1 : 0)
        | ((modifiers & keymapModifierCapsLock) ? 4 : 0)
        | ((modifiers & keymapModifierCtrl) ? 8 : 0)
        | ((modifiers & keymapModifierAlternate) ? 16 : 0);
}

/// Identifies the DECSMKR modifier bit for a physical key.
/// - Parameter key: The key identity.
/// - Returns: One modifier bit, or zero for a non-modifier key.
[[nodiscard]] inline std::uint8_t isoModifierKey(VirtualKey key)
{
    switch (key) {
    case VirtualKey::LeftShift:
        return 1;
    case VirtualKey::RightShift:
        return 2;
    case VirtualKey::CapsLock:
        return 4;
    case VirtualKey::LeftControl:
    case VirtualKey::RightControl:
        return 8;
    case VirtualKey::LeftAlternate:
        return 16;
    case VirtualKey::RightAlternate:
        return 32;
    case VirtualKey::LeftSuper:
        return 64;
    case VirtualKey::RightSuper:
        return 128;
    default:
        return 0;
    }
}

/// Names the family a `VirtualKey` belongs to.
///
/// A family selects the encoder path a key takes, and each one names the
/// codePoint function that answers for it.
enum class KeyFamily : uint8_t {
    None = 0, // not a functional/keypad/modifier key (printable)
    Functional, // Return/Tab/arrows/editing/F-keys → functionalKeyCodePoint
    Keypad, // numeric-keypad keys → keypadKeyCodePoint
    Modifier, // bare Shift/Control/Alternate/Super → modifierKittyCodePoint
};

/// Returns a key's Kitty functional-key codePoint.
///
/// The answer is the `keymapKey*` private-use constant that names the key on
/// the wire. A key event carries its identity in this form, so a frontend that
/// knows only a `VirtualKey` passes it through here first.
///
/// - Parameter key: the key's portable identity.
/// - Returns: the private-use codePoint, or 0 for keypad keys, bare modifiers
///   and `VirtualKey::None` — the caller then routes through
///   `keypadKeyCodePoint` or `modifierKittyCodePoint`, or derives the
///   codePoint from the typed character.
inline uint32_t functionalKeyCodePoint(VirtualKey key)
{
    using namespace Ubiquitty::Keys::Keymap;
    switch (key) {
    case VirtualKey::Return:
        return keymapKeyEnter;
    case VirtualKey::KeypadEnter:
        return keymapKeyKeypadEnter;
    case VirtualKey::Tab:
        return keymapKeyTab;
    case VirtualKey::Backspace:
        return keymapKeyBackspace;
    case VirtualKey::ForwardDelete:
        return keymapKeyDelete;
    case VirtualKey::Escape:
        return keymapKeyEscape;
    case VirtualKey::Insert:
        return keymapKeyInsert;
    case VirtualKey::LeftArrow:
        return keymapKeyLeft;
    case VirtualKey::RightArrow:
        return keymapKeyRight;
    case VirtualKey::UpArrow:
        return keymapKeyUp;
    case VirtualKey::DownArrow:
        return keymapKeyDown;
    case VirtualKey::Home:
        return keymapKeyHome;
    case VirtualKey::End:
        return keymapKeyEnd;
    case VirtualKey::PageUp:
        return keymapKeyPageUp;
    case VirtualKey::PageDown:
        return keymapKeyPageDown;
    case VirtualKey::F1:
        return keymapKeyF1;
    case VirtualKey::F2:
        return keymapKeyF2;
    case VirtualKey::F3:
        return keymapKeyF3;
    case VirtualKey::F4:
        return keymapKeyF4;
    case VirtualKey::F5:
        return keymapKeyF5;
    case VirtualKey::F6:
        return keymapKeyF6;
    case VirtualKey::F7:
        return keymapKeyF7;
    case VirtualKey::F8:
        return keymapKeyF8;
    case VirtualKey::F9:
        return keymapKeyF9;
    case VirtualKey::F10:
        return keymapKeyF10;
    case VirtualKey::F11:
        return keymapKeyF11;
    case VirtualKey::F12:
        return keymapKeyF12;
    default:
        return 0;
    }
}

/// Returns a numeric-keypad key's Kitty keypad codePoint.
///
/// The answer feeds `Keymap::applicationKeypadSS3`, the single source of truth
/// for the `ESC O <final>` application-keypad mapping. KeypadEnter is absent
/// from this table on purpose: it is a functional key and answers through
/// `functionalKeyCodePoint`, so Enter on the keypad emits a newline rather
/// than an application-keypad final.
///
/// - Parameter key: the key's portable identity.
/// - Returns: the private-use codePoint, or 0 for any non-keypad key.
inline uint32_t keypadKeyCodePoint(VirtualKey key)
{
    using namespace Ubiquitty::Keys::Keymap;
    switch (key) {
    case VirtualKey::Keypad0:
        return keymapKeyKeypad0;
    case VirtualKey::Keypad1:
        return keymapKeyKeypad1;
    case VirtualKey::Keypad2:
        return keymapKeyKeypad2;
    case VirtualKey::Keypad3:
        return keymapKeyKeypad3;
    case VirtualKey::Keypad4:
        return keymapKeyKeypad4;
    case VirtualKey::Keypad5:
        return keymapKeyKeypad5;
    case VirtualKey::Keypad6:
        return keymapKeyKeypad6;
    case VirtualKey::Keypad7:
        return keymapKeyKeypad7;
    case VirtualKey::Keypad8:
        return keymapKeyKeypad8;
    case VirtualKey::Keypad9:
        return keymapKeyKeypad9;
    case VirtualKey::KeypadDecimal:
        return keymapKeyKeypadDecimal;
    case VirtualKey::KeypadDivide:
        return keymapKeyKeypadDivide;
    case VirtualKey::KeypadMultiply:
        return keymapKeyKeypadMultiply;
    case VirtualKey::KeypadSubtract:
        return keymapKeyKeypadSubtract;
    case VirtualKey::KeypadAdd:
        return keymapKeyKeypadAdd;
    case VirtualKey::KeypadEqual:
        return keymapKeyKeypadEqual;
    default:
        return 0;
    }
}

/// Returns a bare modifier key's Kitty modifier codePoint.
///
/// Bare modifiers reach the stream only under Kitty's report-all mode, which
/// forwards their presses and releases as events in their own right. The
/// platform supplies the press/release discrimination separately — only the
/// codePoint is portable. Apple maps Command onto Super, which is why the
/// Command keys resolve to the Super codePoints.
///
/// - Parameter key: the key's portable identity.
/// - Returns: the private-use codePoint (`keymapKeyLeftShift`, …), or 0 for
///   any non-modifier key.
inline uint32_t modifierKittyCodePoint(VirtualKey key)
{
    using namespace Ubiquitty::Keys::Keymap;
    switch (key) {
    case VirtualKey::CapsLock:
        return keymapKeyCapsLock;
    case VirtualKey::LeftShift:
        return keymapKeyLeftShift;
    case VirtualKey::RightShift:
        return keymapKeyRightShift;
    case VirtualKey::LeftControl:
        return keymapKeyLeftControl;
    case VirtualKey::RightControl:
        return keymapKeyRightControl;
    case VirtualKey::LeftAlternate:
        return keymapKeyLeftAlternate;
    case VirtualKey::RightAlternate:
        return keymapKeyRightAlternate;
    case VirtualKey::LeftSuper:
        return keymapKeyLeftSuper;
    case VirtualKey::RightSuper:
        return keymapKeyRightSuper;
    default:
        return 0;
    }
}

/// Classifies a key into the family whose encoder path it takes.
///
/// The classifier and the three codePoint tables are one statement made twice,
/// and the tests hold them so: a key is `Functional` exactly when
/// `functionalKeyCodePoint` answers non-zero, `Keypad` exactly when
/// `keypadKeyCodePoint` does, `Modifier` exactly when `modifierKittyCodePoint`
/// does, and `None` otherwise.
///
/// - Parameter key: the key's portable identity.
/// - Returns: the key's family.
inline KeyFamily keyFamily(VirtualKey key)
{
    if (modifierKittyCodePoint(key) != 0)
        return KeyFamily::Modifier;
    if (keypadKeyCodePoint(key) != 0)
        return KeyFamily::Keypad;
    if (functionalKeyCodePoint(key) != 0)
        return KeyFamily::Functional;
    return KeyFamily::None;
}

/// Which encoder will carry the key, which decides whether a keypad key can
/// report its own identity.
enum class KeyEncoder : std::uint8_t {
    /// The legacy byte encoder — CSI/SS3 sequences and control folding.
    Legacy = 0,
    /// Kitty progressive enhancement — CSI-u, which carries any code point.
    Kitty = 1,
};

/// Returns the code point `key` reports as under `encoder`, or 0 when it has
/// none and the caller supplies the typed character instead.
///
/// The three tables above each answer for one family and return 0 for the other
/// two, so a caller reading one alone gets 0 for every key outside it — which
/// reads as "not a special key" and falls through to the typed character. That
/// is how a keypad key comes to report as the digit printed on its cap, losing
/// the distinction the keypad code points exist to carry.
///
/// A keypad key answers only under ``KeyEncoder::Kitty``. The legacy encoder has
/// no byte form for a bare keypad code point, so reporting one there would leave
/// an ordinary keypad press emitting nothing at all; the typed character is what
/// it needs, and the application-keypad SS3 form is a separate path keyed on the
/// same identity.
///
/// - Parameters:
///   - key: The key pressed.
///   - encoder: The encoder that will carry it.
/// - Returns: The reported code point, or 0 to use the typed character.
inline std::uint32_t reportedKeyCodePoint(VirtualKey key, KeyEncoder encoder)
{
    switch (keyFamily(key)) {
    case KeyFamily::Modifier:
        return modifierKittyCodePoint(key);
    case KeyFamily::Keypad:
        return encoder == KeyEncoder::Kitty ? keypadKeyCodePoint(key) : 0;
    case KeyFamily::Functional:
        return functionalKeyCodePoint(key);
    case KeyFamily::None:
        return 0;
    }
    return 0;
}

/// A key's resolved report identity: the primary code point and the shifted
/// glyph that rides the alternate field.
struct ReportedKey {
    /// The primary code point; zero means a dead key with no identity.
    std::uint32_t keyCode = 0;
    /// The shifted glyph for the alternate field, or zero.
    std::uint32_t shiftedKey = 0;
};

/// Resolves a key's report identity from the three facts a frontend reads
/// off its event: the functional (protocol) code point, the unshifted layout
/// base, and the base with Shift folded in.
///
/// Kitty wants the UNSHIFTED base key as the primary code point, with the
/// shifted glyph in the alternate field. The remap is confined to ASCII,
/// which is the scope the protocol describes: the primary does double duty
/// as the Kitty code point and the legacy byte source, and those reconcile
/// only when base and shifted are both ASCII. On other layouts remapping
/// breaks the legacy encoder — an ASCII base strands a non-ASCII shifted
/// glyph past its printable guard (German Shift+3 → §), and a non-ASCII base
/// skips that branch entirely, stranding the ASCII shifted glyph (AZERTY
/// base é, shift 2 — which breaks digit entry).
///
/// - Parameters:
///   - functional: The protocol code point, or 0 for a plain character key.
///   - unshifted: The layout's base with no modifiers applied.
///   - shiftedBase: The base with Shift folded in, the OS's
///     ignoring-modifiers spelling.
/// - Returns: The resolved identity; a zero `keyCode` is a dead key.
[[nodiscard]] constexpr ReportedKey resolveReportedKey(std::uint32_t functional, std::uint32_t unshifted, std::uint32_t shiftedBase)
{
    ReportedKey key;
    if (functional != 0) {
        key.keyCode = functional;
        return key;
    }
    const bool shouldRemapToASCIIBase = unshifted > 0 && unshifted < 0x80 && shiftedBase > 0 && shiftedBase < 0x80;
    if (shouldRemapToASCIIBase) {
        key.keyCode = unshifted;
        if (shiftedBase != unshifted)
            key.shiftedKey = shiftedBase;
        return key;
    }
    key.keyCode = shiftedBase;
    return key;
}

/// What a navigation chord does to the scrollback view.
///
/// The Swift names carry the language's own case convention, so a Swift
/// caller writes `.pageUp` rather than the C++ spelling.
enum class ScrollNavigationAction : std::uint8_t {
    /// Not a navigation chord; the key belongs to the input method.
    None UBIQUITTY_SWIFT_NAME(none),
    /// One screenful toward history.
    PageUp UBIQUITTY_SWIFT_NAME(pageUp),
    /// One screenful toward the tail.
    PageDown UBIQUITTY_SWIFT_NAME(pageDown),
    /// The oldest retained line.
    Top UBIQUITTY_SWIFT_NAME(top),
    /// The live tail.
    Bottom UBIQUITTY_SWIFT_NAME(bottom),
};

/// One screenful of scrollback paging, in lines.
///
/// A row of overlap: the last visible row before the jump is the first after
/// it, so a reader paging through history never loses the line that anchors
/// their place. One rule for every frontend, so which terminal you are on
/// never decides whether a page loses a row.
///
/// - Parameter rows: The viewport's row count.
/// - Returns: The lines one page moves, at least one.
[[nodiscard]] constexpr std::ptrdiff_t pageLinesForRows(std::ptrdiff_t rows)
{
    return rows > 1 ? rows - 1 : 1;
}

/// Resolves the scrollback navigation chords, one set for every frontend.
///
/// Command-PageUp and Command-PageDown page; Command-Home and
/// Shift-Command-Up jump to the oldest retained line; Command-End and
/// Shift-Command-Down return to the live tail. Defined here rather than in
/// each frontend's key handler because a per-frontend copy drifts — a
/// frontend that forgets the Shift-Command pair reads as a terminal whose
/// scrollback shortcut set depends on which window has focus.
///
/// - Parameters:
///   - key: The pressed key, in the portable numbering.
///   - hasCommand: Whether the command modifier is held.
///   - hasShift: Whether the shift modifier is held.
/// - Returns: The action, `None` when the chord is not navigation.
[[nodiscard]] constexpr ScrollNavigationAction scrollNavigationAction(VirtualKey key, bool hasCommand, bool hasShift)
{
    if (!hasCommand)
        return ScrollNavigationAction::None;
    switch (key) {
    case VirtualKey::PageUp:
        return ScrollNavigationAction::PageUp;
    case VirtualKey::PageDown:
        return ScrollNavigationAction::PageDown;
    case VirtualKey::Home:
        return ScrollNavigationAction::Top;
    case VirtualKey::End:
        return ScrollNavigationAction::Bottom;
    case VirtualKey::UpArrow:
        return hasShift ? ScrollNavigationAction::Top : ScrollNavigationAction::None;
    case VirtualKey::DownArrow:
        return hasShift ? ScrollNavigationAction::Bottom : ScrollNavigationAction::None;
    default:
        return ScrollNavigationAction::None;
    }
}

/// Whether an input method's commit is composed text the terminal writes
/// through, rather than ordinary typing the key-down path should encode.
///
/// A commit of exactly one printable ASCII character is ordinary typing, and
/// the frontend leaves it alone: the key event is still coming, and the encoder
/// it reaches is what applies the control forms, modifyOtherKeys and the Kitty
/// protocol. A program that negotiated any of those is owed the escape form for
/// its ordinary keys, and a frontend that writes the bare character instead has
/// silently opted it out.
///
/// Everything else is what an input method composed — a multi-unit commit, a
/// control character, anything outside ASCII — and the terminal writes it as it
/// stands, because no key event describes it.
///
/// UTF-16 units are the unit because that is what both toolkits' input methods
/// deliver and count in, and the question is about one character rather than one
/// grapheme: a commit of two units is composed whether it is a surrogate pair or
/// a pair of letters.
///
/// A commit that ENDS a composition is composed whatever its text — the keys
/// that built it were swallowed as they arrived — so the caller folds its own
/// composition state in; this answers about the text alone.
///
/// - Parameters:
///   - utf16Length: How many UTF-16 units the commit carries.
///   - firstUnit: Its first UTF-16 unit; read only when the length is one.
/// - Returns: Whether the commit is composed text. An empty commit is not: there
///   is nothing to write through, and nothing to encode either.
[[nodiscard]] constexpr bool isComposedCommit(std::uint32_t utf16Length, std::uint32_t firstUnit)
{
    if (utf16Length == 0)
        return false;
    if (utf16Length != 1)
        return true;
    return firstUnit < 0x20 || firstUnit > 0x7E;
}

} // namespace Ubiquitty::Keys
