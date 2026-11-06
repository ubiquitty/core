// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Tests/Core/Input/test_keymap_exhaustive.cpp — the exhaustive keyboard matrix.
//
// Keyboard encoding is the input path everything else depends on, so this
// test does not sample: it sweeps the FULL combination space —
//
//   every key class (printable ASCII, shifted/base variants, non-ASCII,
//   the C0-ambiguous four, the entire kitty PUA functional range)
//   x every modifier set (all 16 shift/alt/ctrl/super combinations, plus
//     capslock / numlock samples)
//   x every event type (press / repeat / release)
//   x all 32 kitty progressive-enhancement flag sets
//   x DECCKM on/off
//   x ~22 terminal-mode configurations (modifyOtherKeys levels+formats,
//     modifyCursor/Function/Keypad levels, DECPAM, VT52, LNM, the
//     meta/alternate/eight-bit disciplines, DECSET 1037, DECUDK)
//
// — ~38 million encodes — and asserts protocol INVARIANTS on every output
// rather than comparing golden bytes, so it stays valid as shapes evolve:
//
//   I1  flag-empty purity: without kitty flags no CSI u ever carries a
//       PUA keyCode, and no event subparameter is emitted.
//   I2  text stays text: a printable key with no non-shift modifier and no
//       ReportAll is exactly its 1-byte typed glyph (baseline config) —
//       the vim/claude-code/atuin regression class.
//   I3  escape direction: a printable with ctrl/alt under any kitty flag is
//       a CSI u whose primary is the unshifted base keyCode.
//   I4  CSI u round-trip: every CSI u output parses back to the input's
//       keyCode, modifier parameter, event type, alternate-key gating, and
//       control-free associated text.
//   I5  event-type gating: no repeat/release output without ReportEvents;
//       no release channel for plain text keys without ReportAll — but a
//       repeat of the same key still re-emits its byte (typematic repeat,
//       not a release; the nvim-repeat regression class).
//   I6  structural well-formedness: every output is a raw byte form, an
//       ESC-prefixed byte, one complete UTF-8 character with or without
//       Meta's ESC before it, a valid CSI, or a valid SS3 — never a torn
//       sequence.
//
// A failure prints the full input tuple, so any regression is immediately
// reproducible as a one-line unit test.

#include "Core/Keys/KeyEvent.h"
#include "Core/Keys/Keymap.h"
#include "Core/Modes/TerminalModes.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Ubiquitty::Keys::KeyEvent;
using Ubiquitty::Keys::KeyEventType;
using namespace Ubiquitty::Keys::Keymap;
namespace Kitty = Ubiquitty::Kitty;

static long g_failures = 0;
static long g_checked = 0;

struct ModifyKeysConfig {
    const char* name;
    uint8_t modifyOtherKeys = 0;
    uint8_t formatOtherKeys = 0;
    uint8_t modifyCursorKeys = 0;
    uint8_t modifyFunctionKeys = 0;
    uint8_t modifyKeypadKeys = 0;
    EncodeModes modes {};
    bool withUDK = false;
};

struct Case {
    const ModifyKeysConfig* config;
    KeyEvent event;
    Kitty::KeyFlagSet flags;
    bool appCursor;
    const char* outputBuffer;
    int n;
};

// One failure line carries the whole reproducing tuple.
static void fail(const Case& c, const char* invariant, const char* detail)
{
    ++g_failures;
    if (g_failures > 40) // don't flood — the first failures localize the bug
        return;
    std::fprintf(stderr, "FAIL [%s] %s: cfg=%s key=%u shifted=%u base=%u mods=%u event=%u flags=%u appCursor=%d text0=%u -> %d bytes:", invariant, detail, c.config->name, c.event.keyCode, c.event.shiftedKey, c.event.baseLayoutKey, c.event.modifiers,
        static_cast<unsigned>(c.event.eventType), c.flags.toRaw(), c.appCursor ? 1 : 0, c.event.textLength > 0 ? c.event.textCodePoints[0] : 0, c.n);
    for (int i = 0; i < c.n; ++i)
        std::fprintf(stderr, " %02X", static_cast<uint8_t>(c.outputBuffer[i]));
    std::fprintf(stderr, "\n");
}

// ---- tiny escape-sequence reader for the invariant checks -----------------

// Parsed CSI: parameter groups split on ';', subparameters on ':'.
struct ParsedCSI {
    uint32_t parameters[4][8];
    int subCount[4];
    int groupCount;
    char final;
};

static bool parseCSI(const char* s, int n, ParsedCSI* outputBuffer)
{
    if (n < 3 || s[0] != 0x1B || s[1] != '[')
        return false;
    std::memset(outputBuffer, 0, sizeof(*outputBuffer));
    int group = 0, sub = 0;
    bool sawDigit = false;
    for (int i = 2; i < n; ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            if (group >= 4 || sub >= 8)
                return false;
            outputBuffer->parameters[group][sub] = outputBuffer->parameters[group][sub] * 10 + static_cast<uint32_t>(c - '0');
            sawDigit = true;
        } else if (c == ':') {
            ++sub;
            if (group < 4)
                outputBuffer->subCount[group] = sub + 1;
        } else if (c == ';') {
            ++group;
            sub = 0;
        } else if (c == '>' || c == '<' || c == '=' || c == '?') {
            if (i != 2)
                return false; // private prefix only right after CSI
        } else if (c >= 0x40 && c <= 0x7E) {
            if (i != n - 1)
                return false; // final byte must end the sequence
            outputBuffer->final = c;
            outputBuffer->groupCount = group + 1;
            if (outputBuffer->subCount[group] == 0)
                outputBuffer->subCount[group] = sub + 1;
            return sawDigit || n == 3;
        } else {
            return false;
        }
    }
    return false; // no final byte
}

// Whether `s` is exactly one well-formed UTF-8 encoding of a non-ASCII
// scalar: a lead byte, its continuation bytes, nothing after them, and no
// overlong form, surrogate or value past U+10FFFF.
static bool isOneNonASCIIScalar(const char* s, int n)
{
    if (n < 2 || n > 4)
        return false;
    const auto byte = [s](int i) { return static_cast<uint8_t>(s[i]); };
    const int expected = byte(0) >= 0xF0 && byte(0) <= 0xF4 ? 4 : byte(0) >= 0xE0 ? 3
        : byte(0) >= 0xC2                                                         ? 2
                                                                                  : 0;
    if (expected != n || byte(0) > 0xF4)
        return false;
    uint32_t scalar = byte(0) & (n == 2 ? 0x1Fu : n == 3 ? 0x0Fu
                                                         : 0x07u);
    for (int i = 1; i < n; ++i) {
        if ((byte(i) & 0xC0u) != 0x80u)
            return false;
        scalar = (scalar << 6) | (byte(i) & 0x3Fu);
    }
    const uint32_t floor = n == 2 ? 0x80u : n == 3 ? 0x800u
                                                   : 0x10000u;
    return scalar >= floor && scalar <= 0x10FFFFu && !(scalar >= 0xD800u && scalar <= 0xDFFFu);
}

static bool isCSIu(const char* s, int n, ParsedCSI* p) { return parseCSI(s, n, p) && p->final == 'u' && n > 3 && s[2] != '?'; }

// ---- the invariants --------------------------------------------------------

static void checkInvariants(const Case& c)
{
    const KeyEvent& e = c.event;
    const ModifyKeysConfig& cfg = *c.config;
    const bool press = e.eventType == KeyEventType::Press;
    const bool reportAll = c.flags.contains(Kitty::KeyFlag::ReportAll);
    const bool reportEvents = c.flags.contains(Kitty::KeyFlag::ReportEvents);
    const bool reportAlternate = c.flags.contains(Kitty::KeyFlag::ReportAlternateKeys);
    const bool reportText = c.flags.contains(Kitty::KeyFlag::ReportText);
    const uint32_t nonShift = keymapModifierCtrl | keymapModifierAlternate | keymapModifierSuper | keymapModifierMeta | keymapModifierHyper;
    // Independent re-derivation of the key classes (not the production
    // predicates, so a bug in those can't blind the invariants).
    const bool functionalKey = e.keyCode >= 57344 || e.keyCode == 9 || e.keyCode == 13 || e.keyCode == 27 || e.keyCode == 127;
    const bool textProducing = e.keyCode >= 0x20 && !functionalKey;
    ParsedCSI csi;
    const bool csiU = c.n > 0 && isCSIu(c.outputBuffer, c.n, &csi);

    ++g_checked;

    // I6 — structural well-formedness of every non-empty output.
    if (c.n > 0 && !cfg.withUDK) {
        const uint8_t first = static_cast<uint8_t>(c.outputBuffer[0]);
        bool ok;
        if (first == 0x1B && c.n >= 3 && c.outputBuffer[1] == '[') {
            ParsedCSI any;
            ok = parseCSI(c.outputBuffer, c.n, &any);
        } else if (first == 0x1B && c.n == 3 && c.outputBuffer[1] == 'O') {
            ok = c.outputBuffer[2] >= 0x40 && c.outputBuffer[2] <= 0x7E; // SS3 final
        } else if (first == 0x1B) {
            // ESC-prefixed byte or bare ESC (vt52 forms are ESC+letter), or
            // Meta over a non-ASCII key: ESC and the character's whole UTF-8.
            // A byte past ASCII after the ESC opens a character, and a lead
            // byte on its own is a torn one.
            ok = c.n >= 2 && static_cast<uint8_t>(c.outputBuffer[1]) >= 0x80
                ? isOneNonASCIIScalar(c.outputBuffer + 1, c.n - 1)
                : c.n <= 2;
        } else if (first >= 0x80) {
            // Eight-bit input sets bit seven on the one byte an ASCII key
            // encodes, control-folded or not; an ASCII key never produces
            // UTF-8, so its single high byte is that form. Any other byte past
            // ASCII opens a non-ASCII key's UTF-8, which must be complete.
            const bool isEightBitMeta = c.n == 1 && e.keyCode < 0x80;
            ok = isEightBitMeta || isOneNonASCIIScalar(c.outputBuffer, c.n);
        } else {
            ok = c.n <= 2; // raw byte forms (CR LF is 2)
        }
        if (!ok)
            fail(c, "I6", "torn or unrecognizable sequence");
    }

    // I5 — event-type gating. The text-key rule covers text-PRODUCING keys as
    // a class (é and ю included), not just the ASCII range: without ReportAll,
    // a key whose press is text has no release channel, and only a non-shift
    // modifier moves it to the escape-code side. Repeat is not release,
    // though: a repeat of the same un-escape-coded text key is typematic
    // repeat and must still re-emit the byte, or held-key repeat breaks for
    // any app (e.g. nvim) that enables REPORT_EVENTS without ReportAll. The
    // "must re-emit" half is asserted for ASCII printable keys, the ones every
    // encoding path emits as a plain byte.
    if (!press) {
        if (!reportEvents && c.n != 0)
            fail(c, "I5", "repeat/release emitted without ReportEvents");
        const bool escapeCoded = reportAll || (e.modifiers & nonShift) != 0;
        const bool isRelease = e.eventType == KeyEventType::Release;
        const bool asciiPrintable = e.keyCode >= 0x20 && e.keyCode < 0x7F;
        if (reportEvents && textProducing && !escapeCoded) {
            if (isRelease && c.n != 0)
                fail(c, "I5", "text key leaked a release event");
            if (!isRelease && asciiPrintable && c.n == 0)
                fail(c, "I5", "ascii text key repeat produced no byte");
        }
        return; // press-shaped invariants below don't apply
    }

    // I7 — the extension owns every functional key it can see: with kitty
    // flags active (and outside VT52 mode) a functional-key press never emits
    // a bare ESC-prefixed convenience byte (the Option word-motion family
    // ESC b / ESC f / ESC DEL / ESC d / ESC TAB / ESC CR / ESC ESC). Those are
    // standard-path-only; kitty-mode apps must get the explicit CSI forms.
    if (!c.flags.isEmpty() && functionalKey && !cfg.modes.vt52Mode) {
        if (c.n == 2 && static_cast<uint8_t>(c.outputBuffer[0]) == 0x1B)
            fail(c, "I7", "functional key emitted a 2-byte ESC convenience under kitty flags");
    }

    // I1 — flag-empty purity: the extension inactive means no CSI u with a PUA
    // primary and no event-type subparameter anywhere.
    if (c.flags.isEmpty()) {
        if (csiU && csi.parameters[0][0] >= 57344)
            fail(c, "I1", "PUA CSI u under empty flags");
        ParsedCSI any;
        if (c.n > 0 && parseCSI(c.outputBuffer, c.n, &any) && any.groupCount >= 2 && any.subCount[1] >= 2)
            fail(c, "I1", "event subparameter under empty flags");
    }

    // I2 — text stays text (baseline config: all xterm modify levels 0, default
    // disciplines): a printable key with no non-shift modifier and no ReportAll
    // is exactly its one typed byte.
    const bool baseline = cfg.modifyOtherKeys == 0 && cfg.modifyCursorKeys == 0 && cfg.modifyFunctionKeys == 0 && cfg.modifyKeypadKeys == 0 && !cfg.withUDK && !cfg.modes.vt52Mode && !cfg.modes.lineFeedNewLineMode && !cfg.modes.applicationKeypad && cfg.modes.metaSendsEscape
        && !cfg.modes.eightBitInput && !cfg.modes.deleteKeySendsDelete;
    if (baseline && isAsciiPrintable(e.keyCode) && (e.modifiers & nonShift) == 0 && !reportAll) {
        const uint8_t typed = isAsciiPrintable(e.shiftedKey) ? static_cast<uint8_t>(e.shiftedKey) : static_cast<uint8_t>(e.keyCode);
        if (!(c.n == 1 && static_cast<uint8_t>(c.outputBuffer[0]) == typed))
            fail(c, "I2", "printable did not stay a plain typed byte");
    }

    // I3 — escape direction: printable + ctrl/alt + any kitty flag is a CSI u
    // whose primary is the unshifted base.
    if (isAsciiPrintable(e.keyCode) && !c.flags.isEmpty() && (e.modifiers & (keymapModifierCtrl | keymapModifierAlternate)) != 0) {
        if (!csiU)
            fail(c, "I3", "modified printable under kitty flags is not CSI u");
        else if (csi.parameters[0][0] != e.keyCode)
            fail(c, "I3", "CSI u primary is not the unshifted base");
    }

    // I4 — CSI u round-trip. Two protocols share the CSI u SHAPE and differ in
    // modifier semantics: the kitty extension (flags non-empty) carries the
    // full 8-bit modifier mask, while xterm modifyOtherKeys formatOtherKeys=1
    // (reachable with flags empty) is a 3-bit shift/alt/ctrl protocol whose
    // code parameter is the TYPED byte, not the unshifted base. When both are
    // active a shift-only printable legitimately reaches the xterm wrapper
    // (the kitty extension leaves it as text), so the kitty-semantics
    // round-trip is asserted only with modifyOtherKeys off.
    if (csiU && !c.flags.isEmpty() && cfg.modifyOtherKeys == 0) {
        if (csi.parameters[0][0] != e.keyCode)
            fail(c, "I4", "primary != keyCode");
        // A text-producing key drops the Lock modifiers unless ReportAll is
        // active ("the Lock modifiers are not reported for text producing
        // keys"); functional keys report the full mask.
        const uint32_t reportedModifiers = (textProducing && !reportAll) ? (e.modifiers & ~0xC0u) : e.modifiers;
        if (csi.groupCount >= 2) {
            const uint32_t modifierParameter = csi.parameters[1][0];
            if (modifierParameter != static_cast<uint32_t>(numericModifierParameter(reportedModifiers)))
                fail(c, "I4", "modifier parameter mismatch");
            if (csi.subCount[1] >= 2 && csi.parameters[1][1] != static_cast<uint32_t>(e.eventType))
                fail(c, "I4", "event subparameter mismatch");
            if (csi.subCount[1] >= 2 && !reportEvents)
                fail(c, "I4", "event subparameter without ReportEvents");
        }
        if (csi.subCount[0] >= 2 && !reportAlternate)
            fail(c, "I4", "alternate subparameters without ReportAlternateKeys");
        // Spec: "the shifted key must be present only if shift is also present
        // in the modifiers" — a non-empty shifted subparameter with Shift
        // absent is a protocol violation.
        if (csi.subCount[0] >= 2 && csi.parameters[0][1] != 0 && (e.modifiers & keymapModifierShift) == 0)
            fail(c, "I4", "shifted subparameter without Shift held");
        if (csi.groupCount >= 3) {
            if (!reportText)
                fail(c, "I4", "text group without ReportText");
            for (int i = 0; i < csi.subCount[2]; ++i)
                if (isControlCodePoint(csi.parameters[2][i]))
                    fail(c, "I4", "control codePoint in associated text");
        }
    } else if (csiU && c.flags.isEmpty() && cfg.modifyKeypadKeys != 0 && e.keyCode >= 57344) {
        // Wire-format collision: keypad-5's SS3 final byte is 'u', so the xterm
        // modifyKeypadKeys form CSI 1 ; mod u is CSI-u-SHAPED. Its parameters
        // are the fixed "1" and the 3-bit modifier, not a key report.
        if (csi.parameters[0][0] != 1)
            fail(c, "I4", "modified-keypad form parameter is not 1");
        if (csi.groupCount >= 2 && csi.parameters[1][0] != static_cast<uint32_t>(numericModifierParameter(e.modifiers & 0x07)))
            fail(c, "I4", "modified-keypad form modifier is not the 3-bit value");
    } else if (csiU && c.flags.isEmpty()) {
        // xterm modifyOtherKeys CSI u form: 3-bit modifier, exactly two groups,
        // no subparameters (xterm has no alternate/event/text channels).
        const uint32_t xtermTypedByte = isAsciiPrintable(e.shiftedKey) ? e.shiftedKey : e.keyCode;
        if (csi.parameters[0][0] != xtermTypedByte)
            fail(c, "I4", "xterm CSI u code is not the typed byte");
        if (csi.groupCount >= 2 && csi.parameters[1][0] != static_cast<uint32_t>(numericModifierParameter(e.modifiers & 0x07)))
            fail(c, "I4", "xterm CSI u modifier is not the 3-bit value");
        if (csi.subCount[0] >= 2 || (csi.groupCount >= 2 && csi.subCount[1] >= 2) || csi.groupCount >= 3)
            fail(c, "I4", "xterm CSI u carries kitty-only subparameters");
    }
}

/// A partition of the config sweep, selected by `--shard I/N`.
///
/// The swept space is identical for any `count`: each config falls in exactly
/// one shard, and nothing below the config loop is partitioned. A shard that
/// draws no config passes with zero encodes.
struct Shard {
    int index = 0;
    int count = 1;
};

/// Parses `--shard I/N`, defaulting to the whole sweep when no argument is given.
///
/// - Returns: `false` when an argument is present but malformed.
static bool parseShard(int argc, char** argv, Shard& shard)
{
    if (argc == 1)
        return true;
    if (argc != 3 || std::strcmp(argv[1], "--shard") != 0)
        return false;
    if (std::sscanf(argv[2], "%d/%d", &shard.index, &shard.count) != 2)
        return false;
    return shard.count >= 1 && shard.index >= 0 && shard.index < shard.count;
}

int main(int argc, char** argv)
{
    Shard shard;
    if (!parseShard(argc, argv, shard)) {
        std::fprintf(stderr, "usage: %s [--shard I/N]  (0 <= I < N)\n", argv[0]);
        return 2;
    }

    // ---- key set (mirrors the full protocol surface) ------------------------
    struct KeyVariant {
        uint32_t keyCode, shiftedKey, baseLayoutKey;
    };
    std::vector<KeyVariant> keys;
    const uint32_t printables[] = { ' ', '!', ',', '.', '/', '0', '1', '5', '9', ';', '<', '=', '?', '@', 'A', 'Z', '[', '\\', ']', '^', '_', '`', 'a', 'e', 'k', 'p', 'q', 'v', 'z', '{', '|', '}', '~' };
    for (uint32_t codePoint : printables)
        keys.push_back({ codePoint, 0, 0 });
    keys.push_back({ ';', ':', 0 });
    keys.push_back({ 'a', 'A', 0 });
    keys.push_back({ 'a', 'A', 'q' });
    keys.push_back({ '1', '!', 0 });
    keys.push_back({ '`', '~', 0 });
    keys.push_back({ ',', '<', 0 });
    keys.push_back({ 'e', 0xE9, 0 });
    keys.push_back({ 0xE9, 0, 0 });
    keys.push_back({ 0xA7, 0, 0 });
    keys.push_back({ 0x44E, 0, 0 });
    keys.push_back({ 9, 0, 0 });
    keys.push_back({ 13, 0, 0 });
    keys.push_back({ 27, 0, 0 });
    keys.push_back({ 127, 0, 0 });
    for (uint32_t k = 57344; k <= 57454; ++k)
        keys.push_back({ k, 0, 0 });

    struct TextVariant {
        int length;
        uint32_t first;
    };
    const TextVariant textVariants[] = { { 0, 0 }, { 1, 'A' }, { 1, 0x01 } };
    // All 16 shift/alt/ctrl/super combinations, then Lock-modifier compounds:
    // CapsLock (64) and NumLock (128) alone, with Shift, with Ctrl, with
    // Ctrl+Shift, and both Locks together — the text-key Lock-strip rule needs
    // escape-coded chords that carry a Lock bit.
    const uint32_t modifierSets[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 64, 65, 68, 69, 128, 129, 132, 192, 196 };
    const KeyEventType events[] = { KeyEventType::Press, KeyEventType::Repeat, KeyEventType::Release };

    // ---- configs (one knob at a time off the defaults) ----------------------
    std::vector<ModifyKeysConfig> configs;
    configs.push_back({ "default" });
    for (uint8_t level : { 1, 2, 3 })
        for (uint8_t format : { 0, 1 })
            configs.push_back({ "modifyOtherKeys", level, format });
    for (uint8_t level : { 1, 2, 3 })
        configs.push_back({ "modifyCursorKeys", 0, 0, level });
    for (uint8_t level : { 1, 3 })
        configs.push_back({ "modifyFunctionKeys", 0, 0, 0, level });
    for (uint8_t level : { 2, 3 })
        configs.push_back({ "modifyKeypadKeys", 0, 0, 0, 0, level });
    configs.push_back({ "applicationKeypad", 0, 0, 0, 0, 0, { .applicationKeypad = true } });
    configs.push_back({ "vt52", 0, 0, 0, 0, 0, { .vt52Mode = true } });
    configs.push_back({ "lnm", 0, 0, 0, 0, 0, { .lineFeedNewLineMode = true } });
    configs.push_back({ "meta-off-8bit-off", 0, 0, 0, 0, 0, { .metaSendsEscape = false } });
    configs.push_back({ "meta-off-8bit-on", 0, 0, 0, 0, 0, { .metaSendsEscape = false, .eightBitInput = true } });
    configs.push_back({ "altEsc-8bit-on", 0, 0, 0, 0, 0, { .metaSendsEscape = false, .alternateSendsEscape = true, .eightBitInput = true } });
    configs.push_back({ "deleteSendsDel", 0, 0, 0, 0, 0, { .deleteKeySendsDelete = true } });
    ModifyKeysConfig udkConfig { "udk" };
    udkConfig.withUDK = true;
    configs.push_back(udkConfig);

    Ubiquitty::VT::UserDefinedKeys udkTable {};
    udkTable.count = 2;
    udkTable.entries[0].keyCode = 17; // F6
    udkTable.entries[0].length = 3;
    std::memcpy(udkTable.entries[0].value, "xyz", 3);
    udkTable.entries[1].keyCode = 3; // Delete/Remove
    udkTable.entries[1].length = 2;
    std::memcpy(udkTable.entries[1].value, "OK", 2);

    for (std::size_t configIndex = 0; configIndex < configs.size(); ++configIndex) {
        if (static_cast<int>(configIndex % static_cast<std::size_t>(shard.count)) != shard.index)
            continue;
        const ModifyKeysConfig& cfg = configs[configIndex];
        for (const KeyVariant& kv : keys) {
            for (const TextVariant& tv : textVariants) {
                for (uint32_t mods : modifierSets) {
                    for (KeyEventType ev : events) {
                        for (uint16_t rawFlags = 0; rawFlags < 32; ++rawFlags) {
                            for (int appCursor = 0; appCursor < 2; ++appCursor) {
                                KeyEvent e {};
                                e.keyCode = kv.keyCode;
                                e.shiftedKey = kv.shiftedKey;
                                e.baseLayoutKey = kv.baseLayoutKey;
                                e.modifiers = mods;
                                e.eventType = ev;
                                e.textLength = tv.length;
                                if (tv.length > 0)
                                    e.textCodePoints[0] = tv.first;
                                char outputBuffer[64];
                                int n = encodeKeyEvent(e, appCursor != 0 ? Ubiquitty::Modes::CursorKeyMode::Application : Ubiquitty::Modes::CursorKeyMode::Normal, Kitty::KeyFlagSet::fromRaw(rawFlags), cfg.modifyOtherKeys, cfg.formatOtherKeys, cfg.modifyCursorKeys, cfg.modifyFunctionKeys, cfg.modifyKeypadKeys, outputBuffer, cfg.modes,
                                    cfg.withUDK ? &udkTable : nullptr);
                                checkInvariants({ &cfg, e, Kitty::KeyFlagSet::fromRaw(rawFlags), appCursor != 0, outputBuffer, n });
                            }
                        }
                    }
                }
            }
        }
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%ld invariant failure(s) across %ld encodes\n", g_failures, g_checked);
        return 1;
    }
    std::printf("keymap exhaustive matrix passed (shard %d/%d, %ld encodes)\n", shard.index, shard.count, g_checked);
    return 0;
}
