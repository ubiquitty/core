// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Keys/Keymap.h"

#include "Core/Keys/KeyEvent.h"
#include "Core/Keys/KeyTranslation.h"
#include "Core/Keys/KittyKeyboard.h"
#include "Core/Modes/TerminalModes.h"
#include "Core/Unicode/UTF8Encoder.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
using Ubiquitty::Keys::KeyEventType;

// Helper: copy `s` (NUL-terminated) into `output`, truncated to what fits.
// Returns bytes copied.
static int putString(std::span<char> output, const char* s)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    int n = static_cast<int>(std::strlen(s));
    if (n > outputCapacity)
        n = outputCapacity;
    std::memcpy(outputBuffer, s, n);
    return n;
}

namespace Ubiquitty::Keys::Keymap {

int encodeXtermSpecial(int keyKind, int keySymbol, uint32_t modifiers, Ubiquitty::Modes::CursorKeyMode cursorKeyMode, int level, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    // xterm honours only shift, alt and ctrl in the numeric modifier, and none
    // under a disabled level.
    int modifierValue = level == keymapModifyLevelDisabled && keyKind != 3 ? 1 : numericModifierParameter(modifiers & 0x07);
    char tmp[32];
    int n = 0;

    if (!outputBuffer || outputCapacity <= 0)
        return 0;

    switch (keyKind) {
    case 1: { // cursor / Home / End: CSI 1;M{ABCDHF}
        char final = static_cast<char>(keySymbol);
        if (modifierValue == 1) {
            // No modifiers — emit DECCKM-aware short form.
            tmp[0] = 0x1B;
            tmp[1] = cursorKeyMode == Ubiquitty::Modes::CursorKeyMode::Application ? 'O' : '[';
            tmp[2] = final;
            n = 3;
            if (n > outputCapacity)
                return 0;
            std::memcpy(outputBuffer, tmp, static_cast<size_t>(n));
            return n;
        }
        // Modifier placement follows modifyCursorKeys:
        //   1      -> CSI mod letter          (no leading parameter "1")
        //   3      -> CSI > 1 ; mod letter     (private form)
        //   0 / 2  -> CSI 1 ; mod letter       (default)
        if (level == 1)
            n = snprintf(tmp, sizeof(tmp), "\033[%d%c", modifierValue, final);
        else if (level == 3)
            n = snprintf(tmp, sizeof(tmp), "\033[>1;%d%c", modifierValue, final);
        else
            n = snprintf(tmp, sizeof(tmp), "\033[1;%d%c", modifierValue, final);
        break;
    }
    case 2: { // function key F1..F12
        int f = keySymbol;
        if (f >= 1 && f <= 4) {
            if (modifierValue == 1)
                n = snprintf(tmp, sizeof(tmp), "\033O%c", 'O' + f);
            else if (level == 1)
                n = snprintf(tmp, sizeof(tmp), "\033[%d%c", modifierValue, 'O' + f);
            else if (level == 3)
                n = snprintf(tmp, sizeof(tmp), "\033[>1;%d%c", modifierValue, 'O' + f);
            else
                n = snprintf(tmp, sizeof(tmp), "\033[1;%d%c", modifierValue, 'O' + f);
        } else {
            // F5..F12 use the ~-form. Parameter values per xterm.
            int tildeParameter = 0;
            switch (f) {
            case 5:
                tildeParameter = 15;
                break;
            case 6:
                tildeParameter = 17;
                break;
            case 7:
                tildeParameter = 18;
                break;
            case 8:
                tildeParameter = 19;
                break;
            case 9:
                tildeParameter = 20;
                break;
            case 10:
                tildeParameter = 21;
                break;
            case 11:
                tildeParameter = 23;
                break;
            case 12:
                tildeParameter = 24;
                break;
            default:
                return 0;
            }
            if (modifierValue == 1)
                n = snprintf(tmp, sizeof(tmp), "\033[%d~", tildeParameter);
            else if (level == 3)
                n = snprintf(tmp, sizeof(tmp), "\033[>%d;%d~", tildeParameter, modifierValue);
            else
                n = snprintf(tmp, sizeof(tmp), "\033[%d;%d~", tildeParameter, modifierValue);
        }
        break;
    }
    case 3: { // editing keys: Insert/Delete/PgUp/PgDn (~-form)
        int p = keySymbol;
        if (modifierValue == 1)
            n = snprintf(tmp, sizeof(tmp), "\033[%d~", p);
        else
            n = snprintf(tmp, sizeof(tmp), "\033[%d;%d~", p, modifierValue);
        break;
    }
    default:
        return 0;
    }
    if (n <= 0 || n > outputCapacity)
        return 0;
    std::memcpy(outputBuffer, tmp, static_cast<size_t>(n));
    return n;
}

// Repeat/release shapes for the named functional keys: the event type rides
// the modifier field as a colon subparameter INSIDE the named forms
// (CSI 1;mod:event D, CSI num;mod:event ~). The kitty spec text only shows the
// event subparameter in the CSI u shape, but kitty itself emits exactly these
// named-form events and the ecosystem parsers (crossterm, libvaxis) decode
// them — reference behavior wins over spec silence here.
static int encodeXtermSpecialEvent(int keyKind, int keySymbol, uint32_t modifiers, KeyEventType eventType, int level, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    if (eventType == KeyEventType::Press)
        return encodeXtermSpecial(keyKind, keySymbol, modifiers, Ubiquitty::Modes::CursorKeyMode::Normal, level, output);

    int modifierValue = numericModifierParameter(modifiers & 0x07);
    int eventValue = (eventType == KeyEventType::Repeat) ? 2 : 3;
    char tmp[32];
    int n = 0;

    if (!outputBuffer || outputCapacity <= 0)
        return 0;

    switch (keyKind) {
    case 1:
        if (level == 1)
            n = snprintf(tmp, sizeof(tmp), "\033[%d:%d%c", modifierValue, eventValue, static_cast<char>(keySymbol));
        else if (level == 3)
            n = snprintf(tmp, sizeof(tmp), "\033[>1;%d:%d%c", modifierValue, eventValue, static_cast<char>(keySymbol));
        else
            n = snprintf(tmp, sizeof(tmp), "\033[1;%d:%d%c", modifierValue, eventValue, static_cast<char>(keySymbol));
        break;
    case 2: {
        int f = keySymbol;
        if (f >= 1 && f <= 4) {
            if (level == 1)
                n = snprintf(tmp, sizeof(tmp), "\033[%d:%d%c", modifierValue, eventValue, 'O' + f);
            else if (level == 3)
                n = snprintf(tmp, sizeof(tmp), "\033[>1;%d:%d%c", modifierValue, eventValue, 'O' + f);
            else
                n = snprintf(tmp, sizeof(tmp), "\033[1;%d:%d%c", modifierValue, eventValue, 'O' + f);
        } else {
            int tildeParameter = 0;
            switch (f) {
            case 5:
                tildeParameter = 15;
                break;
            case 6:
                tildeParameter = 17;
                break;
            case 7:
                tildeParameter = 18;
                break;
            case 8:
                tildeParameter = 19;
                break;
            case 9:
                tildeParameter = 20;
                break;
            case 10:
                tildeParameter = 21;
                break;
            case 11:
                tildeParameter = 23;
                break;
            case 12:
                tildeParameter = 24;
                break;
            default:
                return 0;
            }
            if (level == 3)
                n = snprintf(tmp, sizeof(tmp), "\033[>%d;%d:%d~", tildeParameter, modifierValue, eventValue);
            else
                n = snprintf(tmp, sizeof(tmp), "\033[%d;%d:%d~", tildeParameter, modifierValue, eventValue);
        }
        break;
    }
    case 3:
        n = snprintf(tmp, sizeof(tmp), "\033[%d;%d:%d~", keySymbol, modifierValue, eventValue);
        break;
    default:
        return 0;
    }

    if (n <= 0 || n > outputCapacity)
        return 0;
    std::memcpy(outputBuffer, tmp, static_cast<size_t>(n));
    return n;
}

// xterm modifyOtherKeys CSI ~ form for printable keys.
// Used when format=0/2 (the classic 27;mod;code~ shape).
static int putXtermOther(std::span<char> output, uint32_t code, int mod, uint8_t format)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    char tmp[32];
    int n;
    if (format == 1) {
        // CSI u form: code;mod u
        n = snprintf(tmp, sizeof(tmp), "\033[%u;%du", code, mod);
    } else {
        n = snprintf(tmp, sizeof(tmp), "\033[27;%d;%u~", mod, code);
    }
    if (n <= 0 || n > outputCapacity)
        return 0;
    std::memcpy(outputBuffer, tmp, static_cast<size_t>(n));
    return n;
}

// Apply Ctrl-modifier folding (the OS doesn't include ctrl in charCode
// reliably, so we map manually). Returns the folded byte.
static uint8_t foldControl(uint8_t ch)
{
    if (ch >= 0x80)
        return ch;
    if (ch >= 'a' && ch <= 'z')
        return static_cast<uint8_t>(ch - 'a' + 1);
    if (ch >= 'A' && ch <= 'Z')
        return static_cast<uint8_t>(ch - 'A' + 1);

    switch (ch) {
    case '@':
        return 0x00;
    case ' ':
        return 0x00;
    case '[':
        return 0x1B;
    case '\\':
        return 0x1C;
    case ']':
        return 0x1D;
    case '^':
        return 0x1E;
    case '_':
        return 0x1F;
    case '?':
        return asciiDelete;
    default:
        return ch;
    }
}

// Whether Ctrl folds this character to a distinct control byte (Ctrl+I → TAB,
// Ctrl+[ → ESC, …). When it doesn't, the modified key falls through to the
// modifyOtherKeys wrapping instead.
static bool hasControlFolding(uint8_t character) { return foldControl(character) != character; }

// xterm meta/alternate discipline (modes 1036/1039/1034): decide how an Alternate/Meta-
// modified character is carried. metaSendsEscape (1036) or alternateSendsEscape (1039)
// → ESC prefix. Otherwise eightBitInput (1034) → set bit 7. Otherwise (both
// escape modes off and eightBitInput off) → ESC prefix, matching xterm's
// eightBitInput=false fallback. macOS has a single Alternate/Meta modifier, so either
// escape mode applies to it.
static bool alternateShouldEscapePrefix(const EncodeModes& modes)
{
    if (modes.metaSendsEscape || modes.alternateSendsEscape)
        return true;
    return !modes.eightBitInput;
}

// Standard-path byte carrier for a Ctrl / Alternate-modified printable:
// control folding, then the meta-discipline ESC prefix or 8th-bit form.
// Returns -1 when the modifier combination has no byte form here (the caller
// then tries modifyOtherKeys wrapping and finally the plain byte).
static int putModifiedPrintable(std::span<char> output, uint8_t character, uint32_t modifiers, const EncodeModes& modes)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    bool ctrl = (modifiers & keymapModifierCtrl) != 0;
    bool alternate = (modifiers & keymapModifierAlternate) != 0;
    const bool shouldEscapePrefix = alternateShouldEscapePrefix(modes);
    if (ctrl && hasControlFolding(character)) {
        uint8_t folded = foldControl(character);
        if (!alternate) {
            outputBuffer[0] = static_cast<char>(folded);
            return 1;
        }
        // Ctrl+Alternate with eightBitInput (and no escape mode) sets bit 7 on the
        // folded control byte; otherwise ESC-prefix it.
        if (!shouldEscapePrefix) {
            outputBuffer[0] = static_cast<char>(folded | 0x80);
            return 1;
        }
        if (outputCapacity < 2)
            return 0;
        outputBuffer[0] = 0x1B;
        outputBuffer[1] = static_cast<char>(folded);
        return 2;
    }
    if (alternate && !ctrl) {
        if (!shouldEscapePrefix) {
            // eightBitInput: carry the character with the 8th bit set.
            outputBuffer[0] = static_cast<char>(character | 0x80);
            return 1;
        }
        if (outputCapacity < 2)
            return 0;
        outputBuffer[0] = 0x1B;
        outputBuffer[1] = static_cast<char>(character);
        return 2;
    }
    return -1;
}

// Map a Kitty PUA codePoint to its xterm-special (kind, symbol)
// pair. Mirrors vkeyToXtermSpecial above, keyed by the portable
// identifier instead of the Mac vkey. Returns 0 when the codePoint
// has no xterm-special legacy form.
static int kittyKeyToXtermSpecial(uint32_t keyCode, int* outSymbol)
{
    switch (keyCode) {
    case keymapKeyLeft:
        *outSymbol = 'D';
        return 1;
    case keymapKeyRight:
        *outSymbol = 'C';
        return 1;
    case keymapKeyUp:
        *outSymbol = 'A';
        return 1;
    case keymapKeyDown:
        *outSymbol = 'B';
        return 1;
    case keymapKeyHome:
        *outSymbol = 'H';
        return 1;
    case keymapKeyEnd:
        *outSymbol = 'F';
        return 1;
    case keymapKeyInsert:
        *outSymbol = 2;
        return 3;
    case keymapKeyDelete:
        *outSymbol = 3;
        return 3;
    case keymapKeyPageUp:
        *outSymbol = 5;
        return 3;
    case keymapKeyPageDown:
        *outSymbol = 6;
        return 3;
    case keymapKeyF1:
        *outSymbol = 1;
        return 2;
    case keymapKeyF2:
        *outSymbol = 2;
        return 2;
    case keymapKeyF3:
        *outSymbol = 3;
        return 2;
    case keymapKeyF4:
        *outSymbol = 4;
        return 2;
    case keymapKeyF5:
        *outSymbol = 5;
        return 2;
    case keymapKeyF6:
        *outSymbol = 6;
        return 2;
    case keymapKeyF7:
        *outSymbol = 7;
        return 2;
    case keymapKeyF8:
        *outSymbol = 8;
        return 2;
    case keymapKeyF9:
        *outSymbol = 9;
        return 2;
    case keymapKeyF10:
        *outSymbol = 10;
        return 2;
    case keymapKeyF11:
        *outSymbol = 11;
        return 2;
    case keymapKeyF12:
        *outSymbol = 12;
        return 2;
    default:
        return 0;
    }
}

// Map a portable key codePoint to its DEC function-key code — the numbering
// DECUDK (and the VT220 keyboard) uses to name programmable keys. Editing
// keypad: Find/Insert/Remove/Select/Prev/Next = 1..6; function keys
// F1..F5 = 11..15, F6..F10 = 17..21, F11..F14 = 23..26, F15(Help) = 28,
// F16(Do) = 29, F17..F20 = 31..34. Returns 0 when the key is not DEC-numbered.
static uint16_t keyCodeToDECFunctionKey(uint32_t keyCode)
{
    switch (keyCode) {
    case keymapKeyInsert:
        return 2; // Insert Here
    case keymapKeyDelete:
        return 3; // Remove
    case keymapKeyPageUp:
        return 5; // Prev
    case keymapKeyPageDown:
        return 6; // Next
    case keymapKeyF1:
        return 11;
    case keymapKeyF2:
        return 12;
    case keymapKeyF3:
        return 13;
    case keymapKeyF4:
        return 14;
    case keymapKeyF5:
        return 15;
    case keymapKeyF6:
        return 17;
    case keymapKeyF7:
        return 18;
    case keymapKeyF8:
        return 19;
    case keymapKeyF9:
        return 20;
    case keymapKeyF10:
        return 21;
    case keymapKeyF11:
        return 23;
    case keymapKeyF12:
        return 24;
    case keymapKeyF13:
        return 25;
    case keymapKeyF14:
        return 26;
    case keymapKeyF15:
        return 28;
    case keymapKeyF16:
        return 29;
    case keymapKeyF17:
        return 31;
    case keymapKeyF18:
        return 32;
    case keymapKeyF19:
        return 33;
    case keymapKeyF20:
        return 34;
    default:
        return 0;
    }
}

// Emit a DECUDK-programmed key's value if one is defined for this key.
// Returns bytes written, or -1 when the key has no programmed value (the
// caller then falls through to the default encoding).
static int emitUserDefinedKey(const VT::UserDefinedKeys* udk, uint32_t keyCode, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    if (!udk || udk->count <= 0)
        return -1;
    uint16_t decCode = keyCodeToDECFunctionKey(keyCode);
    if (decCode == 0)
        return -1;
    for (int i = 0; i < udk->count; ++i) {
        const Ubiquitty::VT::UDKEntry& e = udk->entries[i];
        if (e.keyCode != decCode || e.length == 0)
            continue;
        int n = e.length;
        if (n > outputCapacity)
            n = outputCapacity;
        std::memcpy(outputBuffer, e.value, static_cast<size_t>(n));
        return n;
    }
    return -1;
}

// SS3 final byte for an application-keypad key, per VT220 / xterm.
// Returns 0 when keyCode is not a keypad key.
static char keypadSS3Final(uint32_t keyCode)
{
    switch (keyCode) {
    case keymapKeyKeypad0:
        return 'p';
    case keymapKeyKeypad1:
        return 'q';
    case keymapKeyKeypad2:
        return 'r';
    case keymapKeyKeypad3:
        return 's';
    case keymapKeyKeypad4:
        return 't';
    case keymapKeyKeypad5:
        return 'u';
    case keymapKeyKeypad6:
        return 'v';
    case keymapKeyKeypad7:
        return 'w';
    case keymapKeyKeypad8:
        return 'x';
    case keymapKeyKeypad9:
        return 'y';
    case keymapKeyKeypadSubtract:
        return 'm';
    case keymapKeyKeypadAdd:
        // xterm/VT220 application keypad: keypad '+' → SS3 'k' ('l' is the comma/separator, not plus).
        return 'k';
    case keymapKeyKeypadSeparator:
        return 'l';
    case keymapKeyKeypadDecimal:
        return 'n';
    case keymapKeyKeypadDivide:
        return 'o';
    case keymapKeyKeypadMultiply:
        return 'j';
    case keymapKeyKeypadEnter:
        return 'M';
    case keymapKeyKeypadEqual:
        return 'X';
    default:
        return 0;
    }
}

int applicationKeypadSS3(uint32_t keypadKeyCode, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    if (!outputBuffer || outputCapacity < 3)
        return 0;
    char final = keypadSS3Final(keypadKeyCode);
    if (final == 0)
        return 0;
    outputBuffer[0] = '\033';
    outputBuffer[1] = 'O';
    outputBuffer[2] = final;
    return 3;
}

static std::optional<int> encodeKeyboardType(uint32_t keyCode, uint32_t modifiers, Ubiquitty::Modes::CursorKeyMode cursorKeyMode,
    uint8_t cursorLevel, uint8_t functionLevel, uint8_t otherLevel, std::span<char> output, const EncodeModes& modes, const VT::UserDefinedKeys* userDefinedKeys)
{
    if (modes.keyboardType == KeyboardType::Default)
        return std::nullopt;
    const bool isDEC = modes.keyboardType == KeyboardType::Legacy || modes.keyboardType == KeyboardType::VT220;
    int function = keyCode >= keymapKeyF1 && keyCode <= keymapKeyF35 ? static_cast<int>(keyCode - keymapKeyF1 + 1) : 0;
    if (isDEC && function && (modifiers & keymapModifierCtrl)) {
        function += 10;
        keyCode += 10;
        modifiers &= ~keymapModifierCtrl;
    }
    const uint8_t allowed = modes.modifyKeyboard == keymapModifyLevelDisabled ? 0 : modes.modifyKeyboard;
    if (modes.keyboardType == KeyboardType::VT220 && function <= 20 && (modifiers & keymapModifierShift)
        && !(allowed & 4) && !otherLevel) {
        const int written = emitUserDefinedKey(userDefinedKeys, keyCode, output);
        if (written >= 0)
            return written;
    }
    int symbol = 0;
    const int kind = kittyKeyToXtermSpecial(keyCode, &symbol);
    if (!function && !kind && keyCode != keymapKeyKeypadBegin && keyCode != keymapKeyMenu)
        return std::nullopt;
    if (isDEC && !otherLevel) {
        if (!(allowed & (function ? 4 : keyCode == keymapKeyMenu ? 8
                                                                 : 2)))
            modifiers = 0;
    }
    const int level = function || kind == 3 ? functionLevel : cursorLevel;
    const int modifier = level == keymapModifyLevelDisabled ? 1 : numericModifierParameter(modifiers & 7);
    char prefix = '[';
    char final = 0;
    int parameter = 0;
    switch (modes.keyboardType) {
    case KeyboardType::HP:
        prefix = 0;
        if (function >= 1 && function <= 8)
            final = static_cast<char>('o' + function);
        else if (!function) {
            switch (keyCode) {
            case keymapKeyUp:
                final = 'A';
                break;
            case keymapKeyDown:
                final = 'B';
                break;
            case keymapKeyRight:
                final = 'C';
                break;
            case keymapKeyLeft:
                final = 'D';
                break;
            case keymapKeyEnd:
                final = 'F';
                break;
            case keymapKeyDelete:
                final = 'P';
                break;
            case keymapKeyInsert:
                final = 'Q';
                break;
            case keymapKeyPageDown:
                final = 'S';
                break;
            case keymapKeyPageUp:
                final = 'T';
                break;
            case keymapKeyHome:
                final = 'h';
                break;
            default:
                break;
            }
        }
        break;
    case KeyboardType::SCO:
        if (function)
            final = static_cast<char>(function <= 14 ? 'L' + function : 'a' + function - 15);
        else {
            switch (keyCode) {
            case keymapKeyUp:
                final = 'A';
                break;
            case keymapKeyDown:
                final = 'B';
                break;
            case keymapKeyRight:
                final = 'C';
                break;
            case keymapKeyLeft:
                final = 'D';
                break;
            case keymapKeyKeypadBegin:
                final = 'E';
                break;
            case keymapKeyEnd:
                final = 'F';
                break;
            case keymapKeyInsert:
                final = 'L';
                break;
            case keymapKeyPageDown:
                final = 'G';
                break;
            case keymapKeyPageUp:
                final = 'I';
                break;
            case keymapKeyHome:
                final = 'H';
                break;
            default:
                break;
            }
        }
        break;
    case KeyboardType::Sun:
        if (function)
            parameter = function <= 10 ? 223 + function : function <= 20 ? 181 + function
                                                                         : 187 + function;
        else {
            switch (keyCode) {
            case keymapKeyInsert:
                parameter = 2;
                break;
            case keymapKeyDelete:
                parameter = 3;
                break;
            case keymapKeyHome:
                parameter = 214;
                break;
            case keymapKeyEnd:
                parameter = 220;
                break;
            case keymapKeyPageUp:
                parameter = 216;
                break;
            case keymapKeyPageDown:
                parameter = 222;
                break;
            case keymapKeyKeypadBegin:
                parameter = 218;
                break;
            case keymapKeyMenu:
                parameter = 197;
                break;
            default:
                break;
            }
        }
        if (parameter)
            final = 'z';
        else if (kind == 1) {
            prefix = 'O';
            final = static_cast<char>(symbol);
        }
        break;
    case KeyboardType::Termcap:
        // The built-in entry uses application-mode cursor keys and level-2
        // modifier forms. Linux's first five function keys use CSI [ A–E.
        if (modes.termcapProfile == TermcapProfile::Linux && function >= 1 && function <= 5 && modifier == 1) {
            if (output.size() < 4)
                return 0;
            output[0] = '\033';
            output[1] = '[';
            output[2] = '[';
            output[3] = static_cast<char>('A' + function - 1);
            return 4;
        }
        if (modes.termcapProfile == TermcapProfile::Linux && (keyCode == keymapKeyHome || keyCode == keymapKeyEnd))
            return encodeXtermSpecial(3, keyCode == keymapKeyHome ? 1 : 4, modifiers, cursorKeyMode, 0, output);
        if (kind)
            return encodeXtermSpecial(kind, symbol, modifiers,
                modes.termcapProfile == TermcapProfile::Linux ? Ubiquitty::Modes::CursorKeyMode::Normal : Ubiquitty::Modes::CursorKeyMode::Application,
                2, output);
        break;
    case KeyboardType::Legacy:
    case KeyboardType::VT220:
        if (function) {
            parameter = function <= 20 ? keyCodeToDECFunctionKey(keyCode) : 42 + function - 21;
            final = '~';
            if (modes.keyboardType == KeyboardType::VT220 && function <= 4) {
                prefix = 'O';
                final = static_cast<char>('O' + function);
                parameter = 0;
            }
        } else if (keyCode == keymapKeyMenu) {
            parameter = 29;
            final = '~';
        } else if (keyCode == keymapKeyDelete && modes.deleteKeySendsDelete && modifiers == 0)
            return putString(output, "\177");
        else if (modes.keyboardType == KeyboardType::VT220 && (keyCode == keymapKeyHome || keyCode == keymapKeyEnd)) {
            parameter = keyCode == keymapKeyHome ? 1 : 4;
            final = '~';
        } else if (kind)
            return encodeXtermSpecial(kind, symbol, modifiers, cursorKeyMode, level, output);
        break;
    case KeyboardType::Default:
        break;
    }
    if (!final)
        return std::nullopt;
    if (modifier != 1 && level >= 1 && level != keymapModifyLevelDisabled)
        prefix = '[';
    if (modifier != 1 && level >= 2 && !parameter)
        parameter = 1;
    std::array<char, 32> bytes {};
    std::size_t used = 0;
    bytes[used++] = '\033';
    if (prefix)
        bytes[used++] = prefix;
    if (modifier != 1 && level == 3)
        bytes[used++] = '>';
    if (parameter) {
        const int count = std::snprintf(bytes.data() + used, bytes.size() - used, "%d", parameter);
        used += static_cast<std::size_t>(count);
    }
    if (modifier != 1) {
        if (parameter)
            bytes[used++] = ';';
        const int count = std::snprintf(bytes.data() + used, bytes.size() - used, "%d", modifier);
        used += static_cast<std::size_t>(count);
    }
    bytes[used++] = final;
    if (used > output.size())
        return 0;
    std::memcpy(output.data(), bytes.data(), used);
    return static_cast<int>(used);
}

static int extendedModifierKey(uint32_t keyCode)
{
    if (keyCode == keymapKeyCapsLock)
        return 0xe2e5;
    if (keyCode == keymapKeyNumLock)
        return 0xe27f;
    // Xterm maps X11's non-character keysyms to Unicode private-use values.
    static constexpr std::array<int, 14> values {
        0xe2e1, 0xe2e3, 0xe2e9, 0xe2eb, 0xe2ed, 0xe2e7,
        0xe2e2, 0xe2e4, 0xe2ea, 0xe2ec, 0xe2ee, 0xe2e8, 0xe103, 0xe111
    };
    return keyCode >= keymapKeyLeftShift && keyCode - keymapKeyLeftShift < values.size()
        ? values[keyCode - keymapKeyLeftShift]
        : 0;
}

static int extendedSpecialKey(uint32_t keyCode)
{
    switch (keyCode) {
    case keymapKeyScrollLock:
        return 0xe214;
    case keymapKeyPrintScreen:
        return 0xe261;
    case keymapKeyPause:
        return 0xe213;
    case keymapKeyMenu:
        return 0xe267;
    default:
        return 0;
    }
}

int encodeKeyEvent(const Keys::KeyEvent& event, Ubiquitty::Modes::CursorKeyMode cursorKeyMode, Kitty::KeyFlagSet kittyKeyboardFlags, uint8_t xtermModifyOtherKeys, uint8_t xtermFormatOtherKeys, uint8_t xtermModifyCursorKeys, uint8_t xtermModifyFunctionKeys, uint8_t xtermModifyKeypadKeys, std::span<char> output, const EncodeModes& modes, const VT::UserDefinedKeys* userDefinedKeys)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    // The queryable disabled sentinel is distinct from level zero, but both
    // turn off keypad and ordinary-key wrapping.
    if (xtermModifyOtherKeys == keymapModifyLevelDisabled)
        xtermModifyOtherKeys = 0;
    if (xtermModifyKeypadKeys == keymapModifyLevelDisabled)
        xtermModifyKeypadKeys = 0;
    const bool applicationKeypad = modes.applicationKeypad && !(modes.shouldHonorNumLock && (event.modifiers & keymapModifierNumLock));
    const bool vt52Mode = modes.vt52Mode;
    const bool lineFeedNewLineMode = modes.lineFeedNewLineMode;
    const bool deleteKeySendsDelete = modes.deleteKeySendsDelete;
    uint32_t keyCode = event.keyCode;
    uint32_t modifiers = event.modifiers;
    if (kittyKeyboardFlags.isEmpty())
        modifiers &= ~(keymapModifierCapsLock | keymapModifierNumLock);
    // A zeroed POD field means "unset" — treat it as a press.
    KeyEventType eventType = (event.eventType == KeyEventType {}) ? KeyEventType::Press : event.eventType;
    int modifierValue = numericModifierParameter(modifiers & 0x07);
    bool opt = (modifiers & keymapModifierAlternate) != 0;

    if (!outputBuffer || outputCapacity <= 0)
        return 0;

    // Repeat / release exist only in the kitty extension — the standard
    // encoding has no key-up byte form, so route them straight to the
    // extension rather than letting them fall into the standard branches
    // below (which would, e.g., emit a word-motion ESC b on an Option+arrow
    // release). Kitty::reportsNonPressEvent owns the gating: REPORT_EVENTS
    // must be on, and a plain text key has no release channel at all.
    if (eventType != KeyEventType::Press) {
        if (!Kitty::reportsNonPressEvent(keyCode, modifiers, kittyKeyboardFlags)) {
            // reportsNonPressEvent can return false for two different
            // reasons, and only one of them should still produce a byte:
            //   (a) REPORT_EVENTS isn't on at all — the wire contract is
            //       silence for every non-Press event, full stop.
            //   (b) REPORT_EVENTS is on, but this text-producing key has no
            //       escape-code channel (no ReportAll, no non-shift
            //       modifier). Release genuinely has nothing to say there —
            //       there was never a legacy key-up byte. But a Repeat is
            //       not a release: OS auto-repeat of a plain text key has
            //       always meant "resend the byte" (typematic repeat), so
            //       fall through to the same encoding a Press would take.
            //       Without this distinction held-key repeat (e.g.
            //       vim/nvim's hjkl) fails for any app that enables
            //       REPORT_EVENTS without ReportAll.
            if (eventType == KeyEventType::Repeat && kittyKeyboardFlags.contains(Kitty::KeyFlag::ReportEvents))
                eventType = KeyEventType::Press;
            else
                return 0;
        } else {
            int sym = 0;
            int kind = kittyKeyToXtermSpecial(keyCode, &sym);
            if (kind != 0) {
                int level = (kind == 1) ? xtermModifyCursorKeys : (kind == 2) ? xtermModifyFunctionKeys
                                                                              : 0;
                return encodeXtermSpecialEvent(kind, sym, modifiers, eventType, level, output);
            }
            return Kitty::encodeKey(keyCode, event.shiftedKey, event.baseLayoutKey, modifiers, eventType, event.associatedText(), kittyKeyboardFlags, output);
        }
    }

    // VT52 mode (DECANM off): cursor keys transmit bare ESC A/B/C/D —
    // there is no SS3/CSI or modifier channel in VT52. Overrides the
    // xterm/kitty cursor-key paths below so an app in VT52 mode does not
    // see ESC O A ("unknown cursor key").
    if (vt52Mode) {
        switch (keyCode) {
        case keymapKeyUp:
            return putString(output, "\033A");
        case keymapKeyDown:
            return putString(output, "\033B");
        case keymapKeyRight:
            return putString(output, "\033C");
        case keymapKeyLeft:
            return putString(output, "\033D");
        default:
            break;
        }
    }

    // Shift-Tab → CSI Z. Held back from the Kitty path so apps that
    // speak both protocols still see the reverse-tab byte.
    if (keyCode == keymapKeyTab && (modifiers & keymapModifierShift) && kittyKeyboardFlags.isEmpty()) {
        return putString(output, "\033[Z");
    }

    // Option-modifier word-motion / word-erase conveniences (readline / bash).
    // Standard-path only: under any kitty flag the extension owns the key and
    // the application gets the explicit modifier form (CSI 1;3D, CSI 127;3u,
    // CSI 3;3~) below, so vim or neovim speaking the kitty protocol never
    // receives ESC b for Option+Left.
    if (opt && kittyKeyboardFlags.isEmpty() && modes.keyboardType == KeyboardType::Default) {
        switch (keyCode) {
        case keymapKeyLeft:
            return putString(output, "\033b");
        case keymapKeyRight:
            return putString(output, "\033f");
        case keymapKeyBackspace:
            return putString(output, "\033\x7F");
        case keymapKeyDelete:
            return putString(output, "\033d");
        case keymapKeyTab:
            return putString(output, "\033\t");
        case keymapKeyEnter:
            return putString(output, "\033\r");
        case keymapKeyEscape:
            return putString(output, "\033\033");
        default:
            break;
        }
    }

    // Functional keys the kitty extension claims (Kitty::functionalReportedAsEscapeCode:
    // REPORT_ALL, a modifier, or an ambiguous control key with DISAMBIGUATE).
    // Keys with a named xterm form stay on the named CSI sequence — the spec
    // keeps arrows / F-keys / editing keys legacy-shaped — and everything else
    // goes to the CSI u emitter.
    if (Kitty::functionalReportedAsEscapeCode(keyCode, modifiers, kittyKeyboardFlags)) {
        int sym = 0;
        int kind = kittyKeyToXtermSpecial(keyCode, &sym);
        if (kind != 0) {
            int level = (kind == 1) ? xtermModifyCursorKeys : (kind == 2) ? xtermModifyFunctionKeys
                                                                          : 0;
            return encodeXtermSpecial(kind, sym, modifiers, cursorKeyMode, level, output);
        }
        return Kitty::encodeKey(keyCode, event.shiftedKey, event.baseLayoutKey, modifiers, eventType, event.associatedText(), kittyKeyboardFlags, output);
    }

    if (kittyKeyboardFlags.isEmpty()) {
        if (modes.modifyModifierKeys == 4) {
            if (const int code = extendedModifierKey(keyCode))
                return putXtermOther(output, code, modifierValue, modes.formatModifierKeys);
        }
        if (modes.modifySpecialKeys == 4) {
            if (const int code = extendedSpecialKey(keyCode))
                return putXtermOther(output, code, modifierValue, modes.formatSpecialKeys);
        }
        if (modes.keyboardType == KeyboardType::Legacy || modes.keyboardType == KeyboardType::VT220) {
            uint32_t keypadCode = event.virtualKey == VirtualKey::KeypadEnter ? keymapKeyKeypadEnter : keypadKeyCodePoint(event.virtualKey);
            if (!keypadCode && keyCode >= keymapKeyKeypad0 && keyCode <= keymapKeyKeypadSeparator)
                keypadCode = keyCode;
            if (keypadCode) {
                if (modes.keyboardType == KeyboardType::VT220 && !(modifiers & keymapModifierShift)) {
                    if (keypadCode == keymapKeyKeypadAdd) {
                        keypadCode = keymapKeyKeypadSeparator;
                        keyCode = ',';
                    }
                    if (keypadCode == keymapKeyKeypadSeparator && (modifiers & keymapModifierCtrl)) {
                        keypadCode = keymapKeyKeypadSubtract;
                        keyCode = '-';
                        modifiers &= ~keymapModifierCtrl;
                    }
                }
                const uint8_t allowed = modes.modifyKeyboard == keymapModifyLevelDisabled ? 0 : modes.modifyKeyboard;
                if (!(allowed & 1) && !xtermModifyOtherKeys)
                    modifiers = 0;
                modifierValue = numericModifierParameter(modifiers & 7);
                if (applicationKeypad) {
                    const char final = keypadSS3Final(keypadCode);
                    if (final) {
                        if (modifierValue == 1)
                            return applicationKeypadSS3(keypadCode, output);
                        std::array<char, 16> bytes {};
                        const int count = std::snprintf(bytes.data(), bytes.size(), "\033O%d%c", modifierValue, final);
                        return count > 0 && static_cast<std::size_t>(count) < bytes.size()
                            ? putString(output, bytes.data())
                            : 0;
                    }
                }
            }
        }
        if (const auto encoded = encodeKeyboardType(keyCode, modifiers, cursorKeyMode, xtermModifyCursorKeys,
                xtermModifyFunctionKeys, xtermModifyOtherKeys, output, modes, userDefinedKeys))
            return *encoded;
    }

    // DECUDK user-defined keys. An UNMODIFIED programmable functional key
    // (editing keypad or F-key) with Kitty mode off emits its programmed byte
    // string in place of the default sequence. A modifier or Kitty mode routes
    // the key through the explicit-modifier paths instead (xterm parity: UDK
    // carries no modifier channel).
    if (modifiers == 0 && kittyKeyboardFlags.isEmpty()) {
        int n = emitUserDefinedKey(userDefinedKeys, keyCode, output);
        if (n >= 0)
            return n;
    }

    // deleteSendsDel (DEC private 1037): an UNMODIFIED Delete key transmits DEL
    // (0x7F) rather than the default VT220 Remove sequence CSI 3 ~. A modifier
    // or Kitty mode keeps the explicit form so the modifier survives.
    if (deleteKeySendsDelete && keyCode == keymapKeyDelete && modifiers == 0 && kittyKeyboardFlags.isEmpty()) {
        outputBuffer[0] = static_cast<char>(asciiDelete);
        return 1;
    }

    // Xterm-special legacy form for functional keys (arrows, F-keys,
    // editing block). Wins over the printable fallthrough below.
    {
        int sym = 0;
        int kind = kittyKeyToXtermSpecial(keyCode, &sym);
        if (kind != 0) {
            // Cursor keys (kind 1) take modifyCursorKeys; function
            // keys (kind 2) take modifyFunctionKeys. Editing keys (kind 3)
            // have no per-family level in xterm.
            int level = (kind == 1) ? xtermModifyCursorKeys : (kind == 2) ? xtermModifyFunctionKeys
                                                                          : 0;
            return encodeXtermSpecial(kind, sym, modifiers, cursorKeyMode, level, output);
        }
    }

    // DECPAM / DECNKM application keypad: an UNMODIFIED keypad key emits its
    // SS3 application-keypad sequence (ESC O <final>) instead of the plain
    // character. Kitty mode supersedes (handled above), and a modifier routes
    // the key through the modify-keypad / printable paths so control-folding
    // and modifyOtherKeys still apply.
    // `wasModified` as well as `modifiers`: the reported set is what the protocol
    // carries and a frontend may legitimately omit a key from it, so it cannot
    // answer whether the press was bare.
    if (applicationKeypad && kittyKeyboardFlags.isEmpty() && modifiers == 0 && !event.wasModified) {
        // From the KEY, not from keyCode. Under the legacy encoder a keypad key
        // reports the typed character — the encoder has no byte form for a bare
        // keypad code point — so keyCode holds '5' by the time this runs and the
        // key's own identity is only in `virtualKey`. Deriving it here is what
        // lets a frontend stop deciding: it supplies the key it already has and
        // Core routes. A caller that gives no key leaves keypadKeyCodePoint at
        // 0 and this gate inactive.
        const std::uint32_t keypadCode = keypadKeyCodePoint(event.virtualKey);
        int n = applicationKeypadSS3(keypadCode != 0 ? keypadCode : keyCode, output);
        if (n > 0)
            return n;
    }

    // Application-keypad keys with a modifier, when modifyKeypadKeys is
    // enabled: inject the xterm numeric modifier into the SS3 form.
    //   level 0/2 -> CSI 1 ; mod final   level 3 -> CSI > 1 ; mod final
    // Without modifiers (or with the level off) the legacy printable byte
    // path below handles the key.
    // The key's identity comes from `virtualKey` first, for the reason the
    // unmodified gate above gives: under the legacy encoder keyCode holds the
    // typed character, and a stored level that never meets a keypad code point
    // is a setting the host can read back but not feel.
    if (xtermModifyKeypadKeys != 0 && modifierValue != 1) {
        const std::uint32_t keypadCode = keypadKeyCodePoint(event.virtualKey);
        char final = keypadSS3Final(keypadCode != 0 ? keypadCode : keyCode);
        if (final != 0) {
            char tmp[32];
            int n = (xtermModifyKeypadKeys == 3) ? snprintf(tmp, sizeof(tmp), "\033[>1;%d%c", modifierValue, final) : snprintf(tmp, sizeof(tmp), "\033[1;%d%c", modifierValue, final);
            if (n <= 0 || n > outputCapacity)
                return 0;
            std::memcpy(outputBuffer, tmp, static_cast<size_t>(n));
            return n;
        }
    }

    // Backspace key with no modifier on a legacy terminal — the
    // vkey-shaped encoder emits 0x7F here.
    if (keyCode == keymapKeyBackspace) {
        outputBuffer[0] = static_cast<char>(asciiDelete);
        return 1;
    }

    // A text key beyond ASCII under a chord: Ctrl+ф on a Cyrillic layout,
    // Ctrl+α on a Greek one. The chord is the KEY, not the letter — Ctrl+A is
    // 0x01 whatever the layout paints on that position — and the position is
    // what the frontend's base-layout key names. Kitty reports the layout's
    // own letter with that key beside it as the base-layout alternate
    // (CSI 1092::97;5u for Ctrl+ф), so a program that wants the position has
    // it; the legacy encoder has one byte to send and folds the base-layout
    // key instead. A press with no base-layout key sends nothing, since no
    // byte says "Ctrl+ф". A bare or shifted non-ASCII letter is text and takes
    // the passthrough below.
    if (Kitty::isTextProducingKey(keyCode) && !isAsciiPrintable(keyCode)) {
        if (Kitty::printableReportedAsEscapeCode(modifiers, kittyKeyboardFlags))
            return Kitty::encodeKey(keyCode, event.shiftedKey, event.baseLayoutKey, modifiers, eventType, event.associatedText(), kittyKeyboardFlags, output);
        if ((modifiers & keymapModifierCtrl) != 0 && isAsciiPrintable(event.baseLayoutKey))
            keyCode = event.baseLayoutKey;
    }

    // Printable byte handling. We accept ASCII directly from the
    // keyCode in the printable range; non-ASCII codePoints currently
    // round-trip only via the kitty / xterm modifier-aware paths.
    if (isAsciiPrintable(keyCode)) {
        uint8_t ch = static_cast<uint8_t>(keyCode);
        if (Kitty::printableReportedAsEscapeCode(modifiers, kittyKeyboardFlags)) {
            // The kitty primary key is the UNSHIFTED base (keyCode); the shifted glyph
            // rides event.shiftedKey as the alternate (emitted only under ReportAlternate).
            return Kitty::encodeKey(static_cast<uint32_t>(ch), event.shiftedKey, event.baseLayoutKey, modifiers, eventType, event.associatedText(), kittyKeyboardFlags, output);
        }
        // A Super chord — ⌘ on macOS — is the shell's, never text. It reaches
        // the encoder only when no menu item and no binding claimed it, which
        // is ⌘C with nothing selected: the Copy item is disabled, AppKit hands
        // the press on, and the letter typed into the shell there was the
        // defect. The standard encoding has no form for the chord; under the
        // Kitty protocol it was reported above, since a program may bind it.
        if ((modifiers & keymapModifierSuper) != 0)
            return 0;
        // The standard encoding carries the SHIFTED glyph as the byte. keyCode holds the
        // unshifted base (so the kitty primary above is spec-correct), and event.shiftedKey
        // holds the shifted glyph when it differs — so `shiftedKey ? shiftedKey : keyCode`
        // is the byte the key actually typed. ASCII-guarded so a non-ASCII shifted
        // codePoint can't truncate into the byte; a front-end that leaves shiftedKey 0
        // (e.g. the unchanged Linux path) falls back to keyCode and is untouched.
        uint8_t typedByte = isAsciiPrintable(event.shiftedKey) ? static_cast<uint8_t>(event.shiftedKey) : ch;
        // Level 2 skips the byte carrier rather than consulting it. A carrier
        // that returns a byte is answering "this key has well-known
        // behaviour", which is the set level 1 excludes — and xterm defines
        // level 2 as enabling the feature for keys including those exceptions
        // (xterm(1), modifyOtherKeys). The exceptions are what an application
        // at level 2 is asking to undo: Ctrl+I and Tab are one byte, as are
        // Ctrl+M and Enter. Consulting the carrier first returns the fold, and
        // the wrapper below is never reached.
        //
        // The Tab/Enter/Escape KEYS are unaffected: they are not ASCII-printable
        // keyCodes and are carried by the switch below, so "special keys keep
        // their sequences" still holds. Only the modified letters change.
        const bool formatAllModifiedKeys = xtermModifyOtherKeys >= 2 && modifierValue != 1;
        if (!formatAllModifiedKeys) {
            int modifiedPrintableLength = putModifiedPrintable(output, typedByte, modifiers, modes);
            if (modifiedPrintableLength >= 0)
                return modifiedPrintableLength;
        }
        // modifyOtherKeys level 1 only wraps keys that LACK an unambiguous
        // byte form. A Shift-only printable already has one — the shifted
        // glyph in `typedByte` — so xterm (and we) emit it plainly at level 1.
        // Only level 2 (format-all) and the private level 3 wrap Shift+printable
        // as CSI 27 ; mod ; code ~. Emacs enables level 1 at startup and cannot
        // decode that wrapper for a plain letter, so wrapping it there produced
        // a beep and a stray "<code>~" in the buffer.
        const bool nonShiftModifier = (modifiers & (keymapModifierCtrl | keymapModifierAlternate | keymapModifierMeta | keymapModifierSuper)) != 0;
        const bool wrapAsOtherKey = (xtermModifyOtherKeys == 3) || (xtermModifyOtherKeys >= 2 && modifierValue != 1) || (xtermModifyOtherKeys == 1 && modifierValue != 1 && nonShiftModifier);
        if (wrapAsOtherKey) {
            return putXtermOther(output, typedByte, modifierValue, xtermFormatOtherKeys);
        }
        outputBuffer[0] = static_cast<char>(typedByte);
        return 1;
    }

    // Carrier for the keyCode value itself when it's a C0 control
    // (Enter / Tab / Escape / Backspace under the standard encoding).
    switch (keyCode) {
    case keymapKeyTab:
        outputBuffer[0] = '\t';
        return 1;
    case keymapKeyEnter:
        // LNM (mode 20) set: Return transmits CR+LF; reset: CR only.
        if (lineFeedNewLineMode && outputCapacity >= 2) {
            outputBuffer[0] = '\r';
            outputBuffer[1] = '\n';
            return 2;
        }
        outputBuffer[0] = '\r';
        return 1;
    case keymapKeyEscape:
        outputBuffer[0] = 0x1B;
        return 1;
    default:
        break;
    }

    // Key identities are Unicode scalars, including Latin-1 characters.
    // Meta prefixes the complete UTF-8 sequence; setting bit seven would
    // corrupt a multibyte character rather than encode its modifier.
    if (Kitty::isTextProducingKey(keyCode) && keyCode >= 0x80 && Unicode::isScalarValue(keyCode)) {
        if (modifiers & (keymapModifierSuper | keymapModifierCtrl)) {
            return 0;
        }
        if (xtermModifyOtherKeys >= 2 && modifierValue != 1) {
            return putXtermOther(output, keyCode, modifierValue, xtermFormatOtherKeys);
        }
        std::array<char, 5> encoded;
        std::size_t length = 0;
        if (modifiers & (keymapModifierAlternate | keymapModifierMeta)) {
            encoded[length++] = '\x1B';
        }
        Unicode::appendUTF8(encoded.data(), &length, keyCode);
        if (length > output.size()) {
            return 0;
        }
        std::memcpy(outputBuffer, encoded.data(), length);
        return static_cast<int>(length);
    }

    return 0;
}

} // namespace Ubiquitty::Keys::Keymap
