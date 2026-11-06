// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Portable user-keybinding model — the decision layer for custom keymaps:
//   1. parseKeySpec  — "cmd+shift+k" / chord "ctrl+a ctrl+x" → a KeyChord sequence.
//   2. parseAction   — "send:\\x01" / "copy" / ... → a typed BindingAction.
//   3. BindingTable  — the parsed [keys] table (chord sequence → action).
//   4. ChordResolver — the leader/chord state machine, answering Pending / Matched / NoMatch.
// The frontend only translates a native event into a KeyChord and acts on the
// resolved BindingAction. Uses STL (portable layer, not the Core hot path).

#include "Base/Types.h"

#include <string>
#include <vector>

namespace Ubiquitty::Input {

// Modifier bitmask shared with the keymap encoder (KEYMAP_MOD_* in
// Keymap.h). A KeyChord stores only the four user-facing modifiers a
// binding cares about; lock keys (caps/num) never participate.
constexpr uint32_t bindModShift = 1; // keymapModifierShift
constexpr uint32_t bindModAlternate = 2; // keymapModifierAlternate
constexpr uint32_t bindModCtrl = 4; // keymapModifierCtrl
constexpr uint32_t bindModSuper = 8; // keymapModifierSuper (cmd)
constexpr uint32_t bindModMask = bindModShift | bindModAlternate | bindModCtrl | bindModSuper;

// One key press: a base-layout codePoint (lower-cased ASCII letter, the
// printable character, or a KEYMAP_KK_* functional-key PUA codePoint)
// plus the active modifier mask. Equality is exact on both fields.
struct KeyChord {
    uint32_t key = 0;
    uint32_t modifiers = 0;

    bool operator==(const KeyChord& o) const { return key == o.key && modifiers == o.modifiers; }
};

// The action a binding fires. kind selects the behavior; bytes carries
// the decoded payload for SendBytes (raw bytes, escapes already
// resolved). name carries the identifier for Named.
enum class ActionKind {
    None,
    SendBytes, // write bytes to the PTY
    Copy,
    Paste,
    ScrollLineUp,
    ScrollLineDown,
    ScrollPageUp,
    ScrollPageDown,
    ScrollToTop,
    ScrollToBottom,
    NewTab,
    NewWindow,
    CloseTab,
    NextTab,
    PrevTab,
    IncreaseFontSize,
    DecreaseFontSize,
    ResetFontSize,
    ToggleFullscreen,
    ClearScrollback,
    ClearScreen, // scroll the current prompt to the top; nothing is destroyed
    ClearBuffer, // clear screen and scrollback, keeping the input line
    SelectAll, // select the whole buffer, oldest scrollback through the last screen row
    ToggleBeam, // summon/dismiss the command palette
    Macro, // invoke a [macros] entry by name (carried in .name)
    Named, // a named command the frontend resolves by string
};

struct BindingAction {
    ActionKind kind = ActionKind::None;
    std::string bytes; // SendBytes payload (raw, post-escape)
    std::string name; // Named command identifier / Macro macro name

    [[nodiscard]] bool valid() const { return kind != ActionKind::None; }
};

// Parse a key-spec string into a chord sequence. A spec is one or more
// whitespace-separated chords; each chord is '+'-joined tokens, the last
// of which is the key and the rest modifiers (cmd/super/ctrl/control/
// alternate/opt/option/shift, case-insensitive). Key tokens accept a single
// printable character, a named key (enter/tab/escape/space/up/down/...),
// or fN. Returns true and fills `out` on success; false (out cleared)
// on any malformed token.
bool parseKeySpec(const std::string& spec, std::vector<KeyChord>& out);

// Fold a native key event's layout-dependent base character into the chord
// `key` codePoint a binding is matched against. `baseCharacter` is the unshifted
// character the active layout produces (charactersIgnoringModifiers); under a
// non-Latin layout (Russian, Greek, …) that is a non-ASCII letter, but bindings
// are written in ASCII, so when `baseCharacter` is non-ASCII we substitute
// `asciiFallback` — the character the same physical key yields under an
/// The key a chord binding matches, given what the layout produced.
///
/// A non-Latin layout produces a character no binding names, so the platform
/// supplies the ASCII-capable alternative alongside it — on macOS that is
/// `charactersIgnoringModifiers`, in a browser `KeyboardEvent.code`. Deciding
/// here rather than per frontend keeps the fallback rule portable and testable
/// without a window.
///
/// - Parameters:
///   - baseCharacter: What the active layout produced.
///   - asciiFallback: The ASCII-capable alternative, or 0 when the platform has
///     none. ASCII letters fold to lower case, because the shift modifier rather
///     than letter case is what separates cmd+k from cmd+shift+k.
/// - Returns: The character a binding is matched against.
/// The key a chord names, from what the platform reported about a keystroke.
///
/// A binding names the key itself and carries shift as a bit beside it, which
/// is what lets `cmd+shift+[` and `cmd+[` be two chords on one key. But a
/// platform reports the character shift PRODUCED — `{` for shift and the
/// bracket key — so a chord built from that reported character matches no
/// binding written the way a person writes one. The layout's own unshifted
/// character for the physical key is the name, and asking the layout is what
/// keeps this right where a table of shifted pairs would only be right on a US
/// keyboard. Letters need no such lookup: their shifted form differs only in
/// case, which folds below.
///
/// - Parameters:
///   - baseCharacter: The character the platform reported.
///   - unshiftedLayoutCharacter: What the same physical key produces with no
///     modifiers, or 0 when the platform cannot say. A platform that already
///     reports the unshifted character passes 0 and keeps its own answer.
///   - modifiers: The chord's modifier bits, for the shift one.
/// - Returns: The key to match a binding on.
uint32_t resolveChordKey(uint32_t baseCharacter, uint32_t unshiftedLayoutCharacter, uint32_t modifiers);

// One entry of the built-in named-action vocabulary: the raw token a config
// [keys] line carries (and parseAction recognizes) paired with a human-facing
// label for a settings UI. The send:/command: free-form forms are not listed
// here — they are entered verbatim, not picked from a menu.
struct NamedActionEntry {
    const char* token; // the raw action token (what lands in config.toml)
    const char* label; // the human-facing name
};

// The built-in named actions, in a sensible menu order. This is the single
// source of truth the settings UIs (macOS KeysPane, the Linux preferences) and
// any label lookup share, so the catalog can't drift from what parseAction
// actually recognizes. Every token here parses to a non-None, non-Named
// BindingAction. Returns a pointer to a static array of `count` entries.
const NamedActionEntry* namedActionCatalog(int& count);

/// One built-in default binding: the chord a frontend offers for an action the
/// user's [keys] table does not name.
///
/// The chord is stored parsed as well as spelled. `resolveDefaultBinding` runs
/// on the key-equivalent pass of every keystroke, and parsing a spec there
/// would allocate once per key press.
struct DefaultBindingEntry {
    KeyChord chord; // the chord it claims
    const char* spec; // the same chord as a key-spec, for a settings UI
    ActionKind action; // what it runs
};

/// The built-in default bindings.
///
/// One table rather than a constant per frontend: two shells spelling the same
/// default themselves are two places it can be spelled differently, and the day
/// one of them moves the chord the other keeps the old one.
///
/// - Parameter count: Filled with the number of entries.
/// - Returns: A pointer to a static array of `count` entries.
const DefaultBindingEntry* defaultBindingCatalog(int& count);

// Format a chord sequence back into its canonical key-spec string — the inverse
// of parseKeySpec. Each chord renders its modifiers in a fixed order
// (cmd+ctrl+alternate+shift) followed by the key token (a named key like "enter"/"up"/
// "f3", or the single printable character); chords are joined with a space.
// Round-trips: parseKeySpec(formatKeySpec(seq)) == seq for any sequence
// parseKeySpec produced. Returns an empty string for an empty sequence or a
// chord whose key codePoint maps to no token.
std::string formatKeySpec(const std::vector<KeyChord>& sequence);

// Parse an action string into a BindingAction. Recognized forms:
//   "send:<text>"   — write <text> to the PTY; \\xNN \\n \\r \\t \\e \\0 and \\\\ are
//                     decoded, so "\\x01" is a literal Ctrl-A.
//   "<named>"       — a built-in: copy, paste, scroll_line_up/down,
//                     scroll_page_up/down, scroll_to_top/bottom, new_tab,
//                     new_window, close_tab, next_tab, prev_tab,
//                     increase_font_size, decrease_font_size,
//                     reset_font_size, toggle_fullscreen, clear_scrollback,
//                     toggle_beam.
//   "command:<id>"  — a named frontend command carried in BindingAction.name.
//   "macro:<name>"  — invoke a [macros] entry; the name rides in
//                     BindingAction.name (kind Macro). The frontend looks the
//                     body up in the live Values and sends it to the session.
// Returns a BindingAction whose kind is None when the string is empty or
// a "send:" payload fails to decode.
BindingAction parseAction(const std::string& action);

// Decode the escape sequences a "send:" payload supports into raw bytes.
// Returns false on a malformed \\xNN (needs two hex digits). Other escapes
// always succeed; an unknown \\c passes the char through.
bool decodeSendPayload(const std::string& text, std::string& out);

// One row of the parsed table: a chord sequence (length >= 1) -> action.
struct Binding {
    std::vector<KeyChord> sequence;
    BindingAction action;
};

// The user's [keys] table. add() parses a spec+action pair and appends a
// binding on success. Later entries win over earlier ones on an exact
// sequence match (the resolver scans newest-first), so a config can
// override an earlier line.
class BindingTable {
public:
    // Parse and append. Returns true when both the spec and action parsed
    // and a binding was added.
    bool add(const std::string& spec, const std::string& action);

    void clear() { m_bindings.clear(); }
    [[nodiscard]] bool empty() const { return m_bindings.empty(); }
    [[nodiscard]] int count() const { return static_cast<int>(m_bindings.size()); }
    [[nodiscard]] const Binding& at(int i) const { return m_bindings[static_cast<size_t>(i)]; }

    // True when some binding's first chord equals `c`.
    [[nodiscard]] bool anyStartsWith(const KeyChord& c) const;

    // Find a single-chord binding exactly matching `c`. Returns its index
    // or -1. Newest-first so later config lines override earlier ones.
    [[nodiscard]] int matchSingle(const KeyChord& c) const;

    /// Whether some binding in the table runs `kind`.
    ///
    /// The question a built-in default asks before it claims its chord: a
    /// default is a rebind target, not an addition, so a table naming the
    /// action anywhere retires the default, wherever the user wrote it.
    ///
    /// - Parameter kind: The action to look for.
    /// - Returns: Whether any binding runs it.
    [[nodiscard]] bool hasBindingForAction(ActionKind kind) const;

    // Find a binding whose full sequence equals prefix + `c`. Returns its
    // index or -1, newest-first.
    [[nodiscard]] int matchSequence(const std::vector<KeyChord>& prefix, const KeyChord& c) const;

    // True when prefix + `c` is a strict prefix of some longer binding
    // (i.e. more keys are still expected).
    [[nodiscard]] bool isPrefix(const std::vector<KeyChord>& prefix, const KeyChord& c) const;

private:
    std::vector<Binding> m_bindings;
};

/// Whether a built-in default still claims `chord`, and what it runs.
///
/// A default stands down two ways, both decided here so no frontend decides
/// them again: the user bound its action somewhere, since a default is replaced
/// by a rebind rather than supplemented and its chord then reaches the terminal
/// like any other key; or the user's table already names this chord, which owns
/// it outright.
///
/// - Parameters:
///   - table: The user's parsed [keys] table.
///   - chord: The chord that was pressed.
///   - action: Written with the default's action when one stands; left as it
///     was otherwise.
/// - Returns: Whether a default claims the chord.
bool resolveDefaultBinding(const BindingTable& table, const KeyChord& chord, BindingAction& action);

// Resolution per ChordResolver::feed:
//   NoMatch  — the chord (given the current pending prefix) matches
//               nothing; the resolver resets and the caller falls through
//               to the default key path. *action is left None.
//   Pending  — the chord extends a known multi-key binding but isn't a
//               complete match yet; the caller should swallow the key and
//               wait for the next one. *action is None.
//   Matched  — a binding fired; *action carries it and the resolver
//               resets. The caller should swallow the key and run action.
//
// A pending prefix that then receives a non-extending chord first tries to
// match that chord *fresh* as a single binding (so a stray leader followed
// by an ordinary shortcut still works), and only otherwise reports
// NoMatch.
enum class ResolveResult {
    NoMatch,
    Pending,
    Matched,
};

class ChordResolver {
public:
    explicit ChordResolver(const BindingTable& table)
        : m_table(table)
    {
    }

    ResolveResult feed(const KeyChord&, BindingAction&);

    // True while a multi-key prefix is captured (the next key is part of a
    // chord). Frontends can surface this as a leader-mode indicator.
    [[nodiscard]] bool pending() const { return !m_prefix.empty(); }

    void reset() { m_prefix.clear(); }

private:
    const BindingTable& m_table;
    std::vector<KeyChord> m_prefix;
};

} // namespace Ubiquitty::Input
