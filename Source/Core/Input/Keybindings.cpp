// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Keybindings.cpp — key-spec / action parsing, the binding table, and the
// leader/chord state machine. See Keybindings.h for the model.
//

#include "Core/Input/Keybindings.h"

#include <array>
#include <iterator>

#include "Core/Keys/Keymap.h"
#include "Core/Unicode/ASCII.h"

#include <cctype>

namespace Ubiquitty::Input {

namespace {

bool ieq(const std::string& a, const char* b) { return Unicode::equalIgnoringASCIICase(a, b); }

// Map a modifier token to its bit, or 0 if it isn't a modifier.
uint32_t modifierBit(const std::string& tok)
{
    if (ieq(tok, "cmd") || ieq(tok, "command") || ieq(tok, "super") || ieq(tok, "win"))
        return bindModSuper;
    if (ieq(tok, "ctrl") || ieq(tok, "control"))
        return bindModCtrl;
    if (ieq(tok, "alt") || ieq(tok, "opt") || ieq(tok, "option") || ieq(tok, "meta"))
        return bindModAlternate;
    if (ieq(tok, "shift"))
        return bindModShift;
    return 0;
}

// Map a key token to its codePoint. Single printable chars lower-case;
// named keys resolve to KEYMAP_KK_* PUA codePoints; fN to function keys.
// Returns 0 on an unrecognized token.
uint32_t keyCodePoint(const std::string& tok)
{
    if (tok.empty())
        return 0;

    // Single printable ASCII character: the unshifted codePoint. Letters
    // fold to lower-case so "cmd+K" and "cmd+k" are the same chord (the
    // shift bit, not letter case, distinguishes them).
    if (tok.size() == 1) {
        unsigned char c = static_cast<unsigned char>(tok[0]);
        if (c > 0x20 && c < 0x7f)
            return static_cast<uint32_t>(Unicode::toASCIILower(static_cast<char>(c)));
        return 0;
    }

    // fN function keys.
    if ((tok[0] == 'f' || tok[0] == 'F') && tok.size() >= 2) {
        bool allDigits = true;
        for (size_t i = 1; i < tok.size(); ++i) {
            if (tok[i] < '0' || tok[i] > '9') {
                allDigits = false;
                break;
            }
        }
        if (allDigits) {
            int n = 0;
            for (size_t i = 1; i < tok.size(); ++i) {
                n = n * 10 + (tok[i] - '0');
                if (n > 35) // out of fN range; stop before overflow
                    break;
            }
            if (n >= 1 && n <= 35)
                return static_cast<uint32_t>(Keys::Keymap::keymapKeyF1 + (n - 1));
            return 0;
        }
    }

    struct Named {
        const char* name;
        uint32_t codePoint;
    };
    static const Named named[] = {
        { "enter", Keys::Keymap::keymapKeyEnter },
        { "return", Keys::Keymap::keymapKeyEnter },
        { "tab", Keys::Keymap::keymapKeyTab },
        { "escape", Keys::Keymap::keymapKeyEscape },
        { "esc", Keys::Keymap::keymapKeyEscape },
        { "space", 0x20 },
        { "backspace", Keys::Keymap::keymapKeyBackspace },
        { "delete", Keys::Keymap::keymapKeyDelete },
        { "insert", Keys::Keymap::keymapKeyInsert },
        { "left", Keys::Keymap::keymapKeyLeft },
        { "right", Keys::Keymap::keymapKeyRight },
        { "up", Keys::Keymap::keymapKeyUp },
        { "down", Keys::Keymap::keymapKeyDown },
        { "pageup", Keys::Keymap::keymapKeyPageUp },
        { "pagedown", Keys::Keymap::keymapKeyPageDown },
        { "menu", Keys::Keymap::keymapKeyMenu },
        { "home", Keys::Keymap::keymapKeyHome },
        { "end", Keys::Keymap::keymapKeyEnd },
    };
    for (const Named& n : named) {
        if (ieq(tok, n.name))
            return n.codePoint;
    }
    return 0;
}

// Split a string on runs of ASCII whitespace.
std::vector<std::string> splitWhitespace(const std::string& s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i > start)
            out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> splitPlus(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '+') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

bool parseChord(const std::string& chordText, KeyChord& out)
{
    std::vector<std::string> toks = splitPlus(chordText);
    if (toks.empty())
        return false;

    KeyChord chord;
    bool haveKey = false;

    // A literal '+' key written as the final element ("cmd++") splits to a
    // trailing pair of empty tokens; fold it back into a single '+' key
    // token so the modifier/key walk below stays simple.
    if (toks.size() >= 2 && toks.back().empty() && toks[toks.size() - 2].empty()) {
        toks.pop_back();
        toks.back() = "+";
    }

    // All but the last token are modifiers; the last is the key.
    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string& tok = toks[i];
        bool isLast = (i + 1 == toks.size());
        if (tok.empty())
            return false; // stray '+' (e.g. "cmd+" with no key, or "a++b")
        if (!isLast) {
            uint32_t bit = modifierBit(tok);
            if (bit == 0)
                return false;
            chord.modifiers |= bit;
            continue;
        }
        uint32_t codePoint = keyCodePoint(tok);
        if (codePoint == 0)
            return false;
        chord.key = codePoint;
        haveKey = true;
    }
    if (!haveKey || chord.key == 0)
        return false;
    out = chord;
    return true;
}

} // namespace

bool decodeSendPayload(const std::string& text, std::string& out)
{
    out.clear();
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c != '\\' || i + 1 >= text.size()) {
            out += c;
            continue;
        }
        char n = text[++i];
        switch (n) {
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'e':
            out += '\x1b';
            break;
        case '0':
            out += '\0';
            break;
        case '\\':
            out += '\\';
            break;
        case 'x': {
            if (i + 2 >= text.size())
                return false;
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9')
                    return h - '0';
                if (h >= 'a' && h <= 'f')
                    return h - 'a' + 10;
                if (h >= 'A' && h <= 'F')
                    return h - 'A' + 10;
                return -1;
            };
            int hi = hex(text[i + 1]);
            int lo = hex(text[i + 2]);
            if (hi < 0 || lo < 0)
                return false;
            out += static_cast<char>((hi << 4) | lo);
            i += 2;
            break;
        }
        default:
            out += n; // unknown escape: pass the char through
            break;
        }
    }
    return true;
}

bool parseKeySpec(const std::string& spec, std::vector<KeyChord>& out)
{
    out.clear();
    std::vector<std::string> chords = splitWhitespace(spec);
    if (chords.empty())
        return false;
    for (const std::string& ct : chords) {
        KeyChord chord;
        if (!parseChord(ct, chord)) {
            out.clear();
            return false;
        }
        out.push_back(chord);
    }
    return true;
}

namespace {

// The built-in named-action vocabulary: token (the config string), label (the
// settings-UI name), and the typed ActionKind. parseAction maps token -> kind;
// namedActionCatalog exposes the token/label pairs. One table so the parser and
// the UI list can't drift. The order is the settings-menu order (KeysPane).
struct CatalogEntry {
    const char* token;
    const char* label;
    ActionKind kind;
};
constexpr CatalogEntry actionCatalog[] = {
    { "copy", "Copy", ActionKind::Copy },
    { "paste", "Paste", ActionKind::Paste },
    { "scroll_line_up", "Scroll Line Up", ActionKind::ScrollLineUp },
    { "scroll_line_down", "Scroll Line Down", ActionKind::ScrollLineDown },
    { "scroll_page_up", "Scroll Page Up", ActionKind::ScrollPageUp },
    { "scroll_page_down", "Scroll Page Down", ActionKind::ScrollPageDown },
    { "scroll_to_top", "Scroll to Top", ActionKind::ScrollToTop },
    { "scroll_to_bottom", "Scroll to Bottom", ActionKind::ScrollToBottom },
    { "new_tab", "New Tab", ActionKind::NewTab },
    { "new_window", "New Window", ActionKind::NewWindow },
    { "close_tab", "Close Tab", ActionKind::CloseTab },
    { "next_tab", "Next Tab", ActionKind::NextTab },
    { "prev_tab", "Previous Tab", ActionKind::PrevTab },
    { "increase_font_size", "Increase Font Size", ActionKind::IncreaseFontSize },
    { "decrease_font_size", "Decrease Font Size", ActionKind::DecreaseFontSize },
    { "reset_font_size", "Reset Font Size", ActionKind::ResetFontSize },
    { "toggle_fullscreen", "Toggle Fullscreen", ActionKind::ToggleFullscreen },
    { "clear_scrollback", "Clear Scrollback", ActionKind::ClearScrollback },
    { "clear_screen", "Clear Screen", ActionKind::ClearScreen },
    { "clear", "Clear", ActionKind::ClearBuffer },
    { "select_all", "Select All", ActionKind::SelectAll },
    { "toggle_beam", "Toggle Beam", ActionKind::ToggleBeam },
};

// The built-in defaults. Beam's summon is the only one with a chord of its
// own; a new default is an entry here and nothing else, because both macOS
// shells read this table rather than each carrying the constant.
constexpr DefaultBindingEntry defaultBindings[] = {
    { { 'p', bindModSuper | bindModShift }, "cmd+shift+p", ActionKind::ToggleBeam },
    // The bracket pair every browser and editor on this platform cycles
    // tabs with, so a reader arrives already knowing it. The direction
    // follows the brackets themselves rather than the alphabet: `[` points
    // back and `]` points forward.
    { { '[', bindModSuper | bindModShift }, "cmd+shift+[", ActionKind::PrevTab },
    { { ']', bindModSuper | bindModShift }, "cmd+shift+]", ActionKind::NextTab },
};

// Render a single chord's key codePoint back to its spec token (the inverse of
// keyCodePoint). Empty string when the codePoint maps to no token.
std::string keyTokenForCodePoint(uint32_t codePoint)
{
    // fN function keys span a contiguous PUA range, and the bound is the one
    // keyCodePoint parses to — f35. Stopping at f19 left the sixteen above it
    // parsing into a chord this function then rendered as nothing, which
    // formatKeySpec reports as an unrenderable key.
    if (codePoint >= static_cast<uint32_t>(Keys::Keymap::keymapKeyF1) && codePoint <= static_cast<uint32_t>(Keys::Keymap::keymapKeyF35)) {
        const int n = static_cast<int>(codePoint - Keys::Keymap::keymapKeyF1) + 1;
        return "f" + std::to_string(n);
    }

    struct Named {
        uint32_t codePoint;
        const char* name;
    };
    // The canonical spelling parseKeySpec re-parses to the same codePoint (the
    // first of each alias group: "enter" not "return", "escape" not "esc").
    static const Named named[] = {
        { Keys::Keymap::keymapKeyEnter, "enter" },
        { Keys::Keymap::keymapKeyTab, "tab" },
        { Keys::Keymap::keymapKeyEscape, "escape" },
        { 0x20, "space" },
        { Keys::Keymap::keymapKeyBackspace, "backspace" },
        { Keys::Keymap::keymapKeyDelete, "delete" },
        { Keys::Keymap::keymapKeyInsert, "insert" },
        { Keys::Keymap::keymapKeyLeft, "left" },
        { Keys::Keymap::keymapKeyRight, "right" },
        { Keys::Keymap::keymapKeyUp, "up" },
        { Keys::Keymap::keymapKeyDown, "down" },
        { Keys::Keymap::keymapKeyPageUp, "pageup" },
        { Keys::Keymap::keymapKeyPageDown, "pagedown" },
        { Keys::Keymap::keymapKeyMenu, "menu" },
        { Keys::Keymap::keymapKeyHome, "home" },
        { Keys::Keymap::keymapKeyEnd, "end" },
    };
    for (const Named& n : named) {
        if (codePoint == n.codePoint)
            return n.name;
    }

    // A printable ASCII codePoint (parseKeySpec already folded letter case into
    // the shift modifier, so this is the lower-cased character).
    //
    // Parenthesised on purpose: std::string{1, c} is an initializer_list<char>
    // and builds the two characters 0x01 and c, while std::string(1, c) is the
    // count-and-character constructor this wants. The list overload wins
    // whenever braces are used, so the two spellings are not interchangeable.
    if (codePoint > 0x20 && codePoint < 0x7f)
        return std::string(1, static_cast<char>(codePoint));

    return {};
}

} // namespace

const NamedActionEntry* namedActionCatalog(int& count)
{
    constexpr int actionCatalogCount = static_cast<int>(std::size(actionCatalog));
    // Filled from actionCatalog on first use — a fixed-index initializer list
    // here once left a null tail when the catalog grew (index-count drift).
    static const auto entries = [] {
        std::array<NamedActionEntry, actionCatalogCount> out {};
        for (int i = 0; i < actionCatalogCount; ++i)
            out[static_cast<std::size_t>(i)] = { actionCatalog[i].token, actionCatalog[i].label };
        return out;
    }();
    count = actionCatalogCount;
    return entries.data();
}

const DefaultBindingEntry* defaultBindingCatalog(int& count)
{
    count = static_cast<int>(std::size(defaultBindings));
    return defaultBindings;
}

bool resolveDefaultBinding(const BindingTable& table, const KeyChord& chord, BindingAction& action)
{
    // The user's own table is asked for the chord first: a binding on the
    // default's chord owns the key whatever it runs, and a leader starting
    // there is mid-capture rather than free.
    if (table.matchSingle(chord) >= 0 || table.anyStartsWith(chord))
        return false;
    for (const DefaultBindingEntry& entry : defaultBindings) {
        if (!(entry.chord == chord))
            continue;
        // A rebind replaces the default: the user who bound the action meant
        // that key and not this one, so the default's chord goes back to being
        // an ordinary key.
        if (table.hasBindingForAction(entry.action))
            return false;
        action.kind = entry.action;
        return true;
    }
    return false;
}

std::string formatKeySpec(const std::vector<KeyChord>& sequence)
{
    if (sequence.empty())
        return {};
    std::string out;
    for (const KeyChord& chord : sequence) {
        const std::string key = keyTokenForCodePoint(chord.key);
        if (key.empty())
            return {}; // unrenderable key -> no spec
        if (!out.empty())
            out += ' ';
        // Fixed modifier order so the spec is canonical (cmd+ctrl+alternate+shift).
        if (chord.modifiers & bindModSuper)
            out += "cmd+";
        if (chord.modifiers & bindModCtrl)
            out += "ctrl+";
        if (chord.modifiers & bindModAlternate)
            out += "alt+";
        if (chord.modifiers & bindModShift)
            out += "shift+";
        out += key;
    }
    return out;
}

BindingAction parseAction(const std::string& action)
{
    BindingAction a;
    if (action.empty())
        return a;

    // send:<payload>
    if (action.size() > 5 && action.starts_with("send:")) {
        std::string payload;
        if (!decodeSendPayload(action.substr(5), payload))
            return a; // malformed escape -> invalid (ActionKind::None)
        a.kind = ActionKind::SendBytes;
        a.bytes = std::move(payload);
        return a;
    }

    // command:<id>
    if (action.size() > 8 && action.starts_with("command:")) {
        a.kind = ActionKind::Named;
        a.name = action.substr(8);
        return a;
    }

    // macro:<name>
    if (action.size() > 6 && action.starts_with("macro:")) {
        a.kind = ActionKind::Macro;
        a.name = action.substr(6);
        return a;
    }

    // The named vocabulary lives in the shared actionCatalog so the parser and
    // the settings-UI list (namedActionCatalog) stay in lockstep.
    std::string lo = Unicode::lowercasedASCII(action);
    for (const CatalogEntry& entry : actionCatalog) {
        if (lo == entry.token) {
            a.kind = entry.kind;
            return a;
        }
    }
    return a; // unknown -> ActionKind::None
}

bool BindingTable::add(const std::string& spec, const std::string& action)
{
    Binding b;
    if (!parseKeySpec(spec, b.sequence))
        return false;
    b.action = parseAction(action);
    if (!b.action.valid())
        return false;
    m_bindings.push_back(std::move(b));
    return true;
}

bool BindingTable::anyStartsWith(const KeyChord& c) const
{
    for (const Binding& b : m_bindings) {
        if (!b.sequence.empty() && b.sequence[0] == c)
            return true;
    }
    return false;
}

bool BindingTable::hasBindingForAction(ActionKind kind) const
{
    for (const Binding& b : m_bindings) {
        if (b.action.kind == kind)
            return true;
    }
    return false;
}

int BindingTable::matchSingle(const KeyChord& c) const
{
    for (int i = static_cast<int>(m_bindings.size()) - 1; i >= 0; --i) {
        const Binding& b = m_bindings[static_cast<size_t>(i)];
        if (b.sequence.size() == 1 && b.sequence[0] == c)
            return i;
    }
    return -1;
}

namespace {

bool prefixMatches(const std::vector<KeyChord>& seq, const std::vector<KeyChord>& prefix, const KeyChord& c)
{
    if (seq.size() != prefix.size() + 1)
        return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (!(seq[i] == prefix[i]))
            return false;
    }
    return seq[prefix.size()] == c;
}

} // namespace

int BindingTable::matchSequence(const std::vector<KeyChord>& prefix, const KeyChord& c) const
{
    for (int i = static_cast<int>(m_bindings.size()) - 1; i >= 0; --i) {
        if (prefixMatches(m_bindings[static_cast<size_t>(i)].sequence, prefix, c))
            return i;
    }
    return -1;
}

bool BindingTable::isPrefix(const std::vector<KeyChord>& prefix, const KeyChord& c) const
{
    const size_t want = prefix.size() + 1;
    for (const Binding& b : m_bindings) {
        if (b.sequence.size() <= want)
            continue;
        bool ok = true;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (!(b.sequence[i] == prefix[i])) {
                ok = false;
                break;
            }
        }
        if (ok && b.sequence[prefix.size()] == c)
            return true;
    }
    return false;
}

ResolveResult ChordResolver::feed(const KeyChord& chord, BindingAction& action)
{
    action = BindingAction();

    // Continuation of a pending multi-key sequence.
    if (!m_prefix.empty()) {
        int hit = m_table.matchSequence(m_prefix, chord);
        if (hit >= 0) {
            action = m_table.at(hit).action;
            m_prefix.clear();
            return ResolveResult::Matched;
        }
        if (m_table.isPrefix(m_prefix, chord)) {
            m_prefix.push_back(chord);
            return ResolveResult::Pending;
        }
        // The pending prefix is broken. Fall through and retry this chord
        // fresh as a standalone binding, so a stray leader followed by an
        // ordinary shortcut still fires.
        m_prefix.clear();
    }

    // Fresh chord. A complete single-chord binding fires immediately.
    int single = m_table.matchSingle(chord);
    if (single >= 0) {
        action = m_table.at(single).action;
        return ResolveResult::Matched;
    }

    // Otherwise, if the chord opens a longer sequence, start capturing.
    std::vector<KeyChord> empty;
    if (m_table.isPrefix(empty, chord)) {
        m_prefix.push_back(chord);
        return ResolveResult::Pending;
    }

    return ResolveResult::NoMatch;
}

uint32_t resolveChordKey(uint32_t baseCharacter, uint32_t unshiftedLayoutCharacter, uint32_t modifiers)
{
    uint32_t key = baseCharacter;
    const bool hasLayoutCharacter = unshiftedLayoutCharacter != 0 && unshiftedLayoutCharacter < 0x80;
    // Shift held: the reported character is what shift made of the key, and a
    // binding names the key. The layout answers which key that is.
    if ((modifiers & bindModShift) != 0 && hasLayoutCharacter)
        key = unshiftedLayoutCharacter;
    // A non-Latin layout (Russian, …) yields a non-ASCII letter for an ASCII
    // key position; fall back to the ASCII-capable layout's character so the
    // binding still matches (the menu bar does the same for ⌘ equivalents).
    else if (baseCharacter >= 0x80 && hasLayoutCharacter)
        key = unshiftedLayoutCharacter;
    if (key >= 'A' && key <= 'Z')
        key = key - 'A' + 'a';
    return key;
}

} // namespace Ubiquitty::Input
