// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/Keys/KeyEvent.h"
#include "Core/Kitty/KeyboardFlags.h"
#include "Core/Modes/TerminalModes.h"
#include "Core/VT/UserDefinedKeys.h"
#include <span>

namespace Ubiquitty::Keys::Keymap {

// The standard VT/xterm key encoder both frontends share. The kitty keyboard
// protocol extension (classification predicates + CSI u emitter) layers on
// top of it from Keys/KittyKeyboard.h; its flag values live in
// Core/Kitty/KeyboardFlags.h and its per-screen flag stack in
// Core/Kitty/KeyboardState.h.

/// Where an encoder writes its sequence: the caller's storage, never grown.
///
/// Named rather than spelled `std::span<char>` at each signature because the
/// Swift importer has no spelling for the extent template parameter and drops
/// every function that names one — with no diagnostic, so the encoder is absent
/// on the Swift side and the call fails to resolve against a name that exists in
/// C++. The alias is what the importer keys on, which is why all three encoders
/// below take it.
using EncodedKeyBytes = std::span<char>;

// ASCII printable range [space, DEL). Space (0x20) is the first printable byte;
// DEL (0x7F) is the first control past the printable block.
constexpr uint32_t minPrintableASCII = 0x20;
constexpr uint32_t asciiDelete = 0x7F;

// A codePoint that is printable ASCII — usable directly as a standard key byte
// or as a kitty primary codePoint. Non-ASCII (>= DEL) and C0 controls are excluded.
[[nodiscard]] constexpr bool isAsciiPrintable(uint32_t codePoint) { return codePoint >= minPrintableASCII && codePoint < asciiDelete; }

// A C0 control code or DEL. These are never valid "associated text" — but note
// this is not the negation of isAsciiPrintable: a non-ASCII codePoint (é, CJK,
// ...) is neither a control nor ASCII-printable, and is legal associated text.
[[nodiscard]] constexpr bool isControlCodePoint(uint32_t codePoint) { return codePoint < minPrintableASCII || codePoint == asciiDelete; }

// The numeric modifier parameter shared by the xterm modified-key forms
// (CSI 1 ; mod X) and the kitty CSI u second group: the modifier bitmask
// plus one. Clamped to the eight defined bits so a stray flag can't shift
// the wire value out of bounds.
[[nodiscard]] constexpr int numericModifierParameter(uint32_t modifierBits) { return static_cast<int>((modifierBits & 0xFF) + 1u); }

/// The stored modifyCursorKeys or modifyFunctionKeys level that disables the
/// modifier: xterm's -1 (`mfkNone`), under which a modified key sends what the
/// unmodified key sends. A level is otherwise 0 through 3.
constexpr int keymapModifyLevelDisabled = 0xFF;

constexpr int keymapModifierShift = 1;
constexpr int keymapModifierAlternate = 2;
constexpr int keymapModifierCtrl = 4;
constexpr int keymapModifierSuper = 8;
constexpr int keymapModifierHyper = 16;
constexpr int keymapModifierMeta = 32;
constexpr int keymapModifierCapsLock = 64;
constexpr int keymapModifierNumLock = 128;

// Key transition values live in Keys::KeyEventType (KeyEvent.h).

// Kitty PUA functional key codePoints, per
// https://sw.kovidgoyal.net/kitty/keyboard-protocol/#functional-key-definitions
// Defined for the full PUA range even when the hardware can't surface the key;
// the vkey → codePoint mapping only covers what the host OS can deliver.
constexpr int keymapKeyEscape = 27;
constexpr int keymapKeyEnter = 13;
constexpr int keymapKeyTab = 9;
constexpr int keymapKeyBackspace = 127;
constexpr int keymapKeyInsert = 57348;
constexpr int keymapKeyDelete = 57349;
constexpr int keymapKeyLeft = 57350;
constexpr int keymapKeyRight = 57351;
constexpr int keymapKeyUp = 57352;
constexpr int keymapKeyDown = 57353;
constexpr int keymapKeyPageUp = 57354;
constexpr int keymapKeyPageDown = 57355;
constexpr int keymapKeyHome = 57356;
constexpr int keymapKeyEnd = 57357;
constexpr int keymapKeyCapsLock = 57358;
constexpr int keymapKeyScrollLock = 57359;
constexpr int keymapKeyNumLock = 57360;
constexpr int keymapKeyPrintScreen = 57361;
constexpr int keymapKeyPause = 57362;
constexpr int keymapKeyMenu = 57363;
constexpr int keymapKeyF1 = 57364;
constexpr int keymapKeyF2 = 57365;
constexpr int keymapKeyF3 = 57366;
constexpr int keymapKeyF4 = 57367;
constexpr int keymapKeyF5 = 57368;
constexpr int keymapKeyF6 = 57369;
constexpr int keymapKeyF7 = 57370;
constexpr int keymapKeyF8 = 57371;
constexpr int keymapKeyF9 = 57372;
constexpr int keymapKeyF10 = 57373;
constexpr int keymapKeyF11 = 57374;
constexpr int keymapKeyF12 = 57375;
constexpr int keymapKeyF13 = 57376;
constexpr int keymapKeyF14 = 57377;
constexpr int keymapKeyF15 = 57378;
constexpr int keymapKeyF16 = 57379;
constexpr int keymapKeyF17 = 57380;
constexpr int keymapKeyF18 = 57381;
constexpr int keymapKeyF19 = 57382;
constexpr int keymapKeyF20 = 57383;
constexpr int keymapKeyF21 = 57384;
constexpr int keymapKeyF22 = 57385;
constexpr int keymapKeyF23 = 57386;
constexpr int keymapKeyF24 = 57387;
constexpr int keymapKeyF25 = 57388;
constexpr int keymapKeyF26 = 57389;
constexpr int keymapKeyF27 = 57390;
constexpr int keymapKeyF28 = 57391;
constexpr int keymapKeyF29 = 57392;
constexpr int keymapKeyF30 = 57393;
constexpr int keymapKeyF31 = 57394;
constexpr int keymapKeyF32 = 57395;
constexpr int keymapKeyF33 = 57396;
constexpr int keymapKeyF34 = 57397;
constexpr int keymapKeyF35 = 57398;
constexpr int keymapKeyKeypad0 = 57399;
constexpr int keymapKeyKeypad1 = 57400;
constexpr int keymapKeyKeypad2 = 57401;
constexpr int keymapKeyKeypad3 = 57402;
constexpr int keymapKeyKeypad4 = 57403;
constexpr int keymapKeyKeypad5 = 57404;
constexpr int keymapKeyKeypad6 = 57405;
constexpr int keymapKeyKeypad7 = 57406;
constexpr int keymapKeyKeypad8 = 57407;
constexpr int keymapKeyKeypad9 = 57408;
constexpr int keymapKeyKeypadDecimal = 57409;
constexpr int keymapKeyKeypadDivide = 57410;
constexpr int keymapKeyKeypadMultiply = 57411;
constexpr int keymapKeyKeypadSubtract = 57412;
constexpr int keymapKeyKeypadAdd = 57413;
constexpr int keymapKeyKeypadEnter = 57414;
constexpr int keymapKeyKeypadEqual = 57415;
constexpr int keymapKeyKeypadSeparator = 57416;
constexpr int keymapKeyKeypadLeft = 57417;
constexpr int keymapKeyKeypadRight = 57418;
constexpr int keymapKeyKeypadUp = 57419;
constexpr int keymapKeyKeypadDown = 57420;
constexpr int keymapKeyKeypadPageUp = 57421;
constexpr int keymapKeyKeypadPageDown = 57422;
constexpr int keymapKeyKeypadHome = 57423;
constexpr int keymapKeyKeypadEnd = 57424;
constexpr int keymapKeyKeypadInsert = 57425;
constexpr int keymapKeyKeypadDelete = 57426;
constexpr int keymapKeyKeypadBegin = 57427;
constexpr int keymapKeyMediaPlay = 57428;
constexpr int keymapKeyMediaPause = 57429;
constexpr int keymapKeyMediaPlayPause = 57430;
constexpr int keymapKeyMediaReverse = 57431;
constexpr int keymapKeyMediaStop = 57432;
constexpr int keymapKeyMediaFastForward = 57433;
constexpr int keymapKeyMediaRewind = 57434;
constexpr int keymapKeyMediaTrackNext = 57435;
constexpr int keymapKeyMediaTrackPrevious = 57436;
constexpr int keymapKeyMediaRecord = 57437;
constexpr int keymapKeyLowerVolume = 57438;
constexpr int keymapKeyRaiseVolume = 57439;
constexpr int keymapKeyMuteVolume = 57440;
constexpr int keymapKeyLeftShift = 57441;
constexpr int keymapKeyLeftControl = 57442;
constexpr int keymapKeyLeftAlternate = 57443;
constexpr int keymapKeyLeftSuper = 57444;
constexpr int keymapKeyLeftHyper = 57445;
constexpr int keymapKeyLeftMeta = 57446;
constexpr int keymapKeyRightShift = 57447;
constexpr int keymapKeyRightControl = 57448;
constexpr int keymapKeyRightAlternate = 57449;
constexpr int keymapKeyRightSuper = 57450;
constexpr int keymapKeyRightHyper = 57451;
constexpr int keymapKeyRightMeta = 57452;
constexpr int keymapKeyIsoLevel3Shift = 57453;
constexpr int keymapKeyIsoLevel5Shift = 57454;

/// Encodes a special key with its modifiers, xterm style.
///
/// Covers the cursor, function, keypad and editing keys. Used when
/// modifyOtherKeys is on, or when the key carries non-trivial modifiers: the
/// result is the standard CSI sequence with a numeric modifier suffix, per the
/// xterm "function key modifiers" table.
///
/// - Parameters:
///   - keyKind: The key family. 1 = cursor (Up/Down/Left/Right/Home/End) →
///     `CSI 1;M{ABCDHF}`; 2 = function (F1..F12) → `CSI 1;M{PQRS}` or
///     `CSI N;M~`; 3 = editing (Insert/Delete/PgUp/PgDn) → `CSI N;M~`; 4 =
///     other, which the caller routes through xterm modifyOtherKeys instead.
///   - keySymbol: Read against `keyKind`. Kind 1 takes the ASCII final byte
///     `'A'`..`'D'` / `'H'` / `'F'`; kind 2 takes the F-number 1..12; kind 3
///     takes the ~-form parameter (2 = Insert, 3 = Delete, 5 = PgUp,
///     6 = PgDn, 1 = Home, 4 = End, the xterm convention).
///   - modifiers: A bitmask of `KEYMAP_MOD_*`. The xterm numeric modifier is
///     `(modifiers & 0x3F) + 1`.
///   - cursorKeyMode: DECCKM. Selects SS3 over CSI for an unmodified cursor key.
///   - level: The xterm modifyCursorKeys / modifyFunctionKeys resource value,
///     which places the numeric modifier. 0 and 2 emit the default form
///     (`CSI 1 ; mod letter` / `CSI parameter ; mod ~`); 1 omits the leading
///     "1"; 3 emits the private `CSI > …` form; ``keymapModifyLevelDisabled``
///     drops the modifier, as xterm's -1 does. Editing keys ignore it. Pass 0
///     for the historical default.
///   - output: Destination for the sequence.
/// - Returns: The byte count written, or 0 when the key has no encoding or
///   `output` is too short for it.
int encodeXtermSpecial(int keyKind, int keySymbol, uint32_t modifiers, Ubiquitty::Modes::CursorKeyMode cursorKeyMode, int level, EncodedKeyBytes output);

/// Per-terminal mode flags threaded into ``encodeKeyEvent``.
///
/// Grouped into one struct so adjacent booleans cannot silently transpose at a
/// call site. Defaults match a fresh VT220: `metaSendsEscape` on, the rest off.
///
/// `applicationKeypad` is DECPAM / DECNKM: when set, an UNMODIFIED keypad key (a
/// `keymapKeyKeypad*` codePoint) emits its SS3 application-keypad sequence
/// (`ESC O <final>`) instead of the plain character — the same table
/// ``applicationKeypadSS3`` exposes. Modified keypad presses still route through
/// the xterm / Kitty paths so modifyOtherKeys and control-folding apply, and
/// Kitty mode supersedes the SS3 shortcut entirely.
///
/// `metaSendsEscape` / `alternateSendsEscape` / `eightBitInput` are DEC private
/// 1036 / 1039 / 1034. They affect only how an Alternate/Meta-modified printable
/// byte is carried on the standard path — Kitty and modifyOtherKeys carry the
/// modifier explicitly: either escape mode gives ESC then the key; failing that
/// `eightBitInput` gives the key with bit 7 set; failing that, an ESC prefix.
/// macOS exposes a single Alternate/Meta modifier, so either escape mode applies
/// to it, and `metaSendsEscape` defaults on so a caller that passes no modes
/// keeps Option-as-Meta.
///
/// `deleteKeySendsDelete` is DEC private 1037: the editing-keypad Delete key
/// transmits DEL (0x7F) rather than the VT220 Remove sequence `CSI 3 ~`. It
/// applies only to an UNMODIFIED Delete on the standard path.
struct EncodeModes {
    bool applicationKeypad = false;
    bool shouldHonorNumLock = true;
    KeyboardType keyboardType = KeyboardType::Default;
    TermcapProfile termcapProfile = TermcapProfile::Ubiquitty;
    uint8_t modifyKeyboard = 0;
    uint8_t modifyModifierKeys = 0;
    uint8_t modifySpecialKeys = 0;
    uint8_t formatModifierKeys = 0;
    uint8_t formatSpecialKeys = 0;
    bool vt52Mode = false;
    bool lineFeedNewLineMode = false;
    bool metaSendsEscape = true;
    bool alternateSendsEscape = false;
    bool eightBitInput = false;
    bool deleteKeySendsDelete = false;
};

/// Encodes a key event into the byte sequence bound for the host.
///
/// The portable entry point. ``Keys::KeyEvent``
/// (Source/Core/Keys/KeyEvent.h) carries a Kitty-codePoint `keyCode` plus
/// `KEYMAP_MOD_*` modifiers, so this function has no platform dependency and
/// every frontend routes through it.
///
/// - Parameters:
///   - cursorKeyMode: DECCKM, selecting SS3 over CSI for cursor keys.
///   - kittyKeyboardFlags: The active Kitty keyboard-protocol flag set. A
///     non-empty set routes the event through the Kitty encoder.
///   - xtermModifyOtherKeys: The `CSI > 4 ; Pv` level.
///   - xtermFormatOtherKeys: The `CSI > 4 ; Pv f` format selector.
///   - xtermModifyCursorKeys: The stored `CSI > 1 ; Pv` level.
///   - xtermModifyFunctionKeys: The stored `CSI > 2 ; Pv` level.
///   - xtermModifyKeypadKeys: The stored `CSI > 3 ; Pv` level. These three
///     select modifier placement for the matching key family — see
///     ``encodeXtermSpecial``'s `level`.
///   - output: Destination for the sequence.
///   - modes: Per-terminal keyboard discipline. See ``EncodeModes``.
///   - userDefinedKeys: The DECUDK table, or null. A programmable functional key
///     (editing-keypad or F-key) pressed UNMODIFIED with Kitty mode off emits its
///     programmed byte string in place of the default sequence; a key with no
///     programmed value falls through to the default encoding.
/// - Returns: The byte count written, or 0 when the event is suppressed — a
///   release event under a terminal that reports none, or an `output` with no
///   room for the sequence. The destination is the caller's and is never grown.
int encodeKeyEvent(const Keys::KeyEvent&, Ubiquitty::Modes::CursorKeyMode cursorKeyMode, Kitty::KeyFlagSet kittyKeyboardFlags, uint8_t xtermModifyOtherKeys, uint8_t xtermFormatOtherKeys, uint8_t xtermModifyCursorKeys, uint8_t xtermModifyFunctionKeys, uint8_t xtermModifyKeypadKeys, EncodedKeyBytes output, const EncodeModes& modes = {}, const VT::UserDefinedKeys* userDefinedKeys = nullptr);

/// Writes a keypad key's application-keypad sequence.
///
/// The DECPAM / DECNKM form `ESC O <final>`, per the VT220 and xterm
/// application-keypad table. This is the single shared table the macOS frontend
/// and the shared encoder both route through, so the two cannot disagree about
/// what a keypad key sends.
///
/// - Parameters:
///   - keypadKeyCode: The Kitty keypad PUA codePoint — `keymapKeyKeypad0`..`9`,
///     KeypadAdd, KeypadSubtract, KeypadMultiply, KeypadDivide, KeypadDecimal,
///     KeypadEqual, or KeypadEnter.
///   - output: Destination for the three bytes.
/// - Returns: The byte count written, or 0 when `keypadKeyCode` is not an
///   application-keypad key or `output` is shorter than the sequence. The caller
///   then falls back to the normal encoding.
int applicationKeypadSS3(uint32_t keypadKeyCode, EncodedKeyBytes output);

} // namespace Ubiquitty::Keys::Keymap
