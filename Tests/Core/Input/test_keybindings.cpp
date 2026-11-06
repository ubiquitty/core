// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

//
// test_keybindings.cpp — the portable key-spec/action parser, the
// binding table, and the leader/chord state machine. These run identically
// on every frontend; a frontend only translates a native event into a
// KeyChord and dispatches the resolved action.
//

#include "Core/Input/Keybindings.h"
#include "Core/Keys/Keymap.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace Ubiquitty::Input;
namespace KM = Ubiquitty::Keys::Keymap;

// ---- key-spec parsing -------------------------------------------------

TEST(testSingleChordModifiers)
{
    std::vector<KeyChord> seq;
    CHECK(parseKeySpec("cmd+shift+k", seq));
    {
        // toggle_beam is a real catalog action (rebindable Beam summon).
        BindingAction beam = parseAction("toggle_beam");
        CHECK(beam.kind == ActionKind::ToggleBeam);
        int catalogCount = 0;
        const NamedActionEntry* entries = namedActionCatalog(catalogCount);
        bool found = false;
        for (int i = 0; i < catalogCount; ++i)
            if (std::string(entries[i].token) == "toggle_beam")
                found = true;
        CHECK(found);
    }
    CHECK(seq.size() == 1);
    CHECK(seq[0].key == 'k');
    CHECK(seq[0].modifiers == (bindModSuper | bindModShift));
}

TEST(testModifierAliases)
{
    std::vector<KeyChord> a, b;
    CHECK(parseKeySpec("command+control+option+shift+x", a));
    CHECK(parseKeySpec("super+ctrl+alt+shift+x", b));
    CHECK(a.size() == 1 && b.size() == 1);
    CHECK(a[0] == b[0]);
    CHECK(a[0].modifiers == (bindModSuper | bindModCtrl | bindModAlternate | bindModShift));
}

TEST(testLetterCaseFolds)
{
    // Letter case never distinguishes a chord; the shift bit does.
    std::vector<KeyChord> upper, lower;
    CHECK(parseKeySpec("cmd+K", upper));
    CHECK(parseKeySpec("cmd+k", lower));
    CHECK(upper.size() == 1 && lower.size() == 1);
    CHECK(upper[0] == lower[0]);
    CHECK(lower[0].key == 'k');
}

TEST(testNamedKeys)
{
    std::vector<KeyChord> seq;
    CHECK(parseKeySpec("ctrl+up", seq));
    CHECK(seq[0].key == static_cast<uint32_t>(KM::keymapKeyUp));
    CHECK(seq[0].modifiers == bindModCtrl);

    CHECK(parseKeySpec("shift+pageup", seq));
    CHECK(seq[0].key == static_cast<uint32_t>(KM::keymapKeyPageUp));

    CHECK(parseKeySpec("escape", seq));
    CHECK(seq[0].key == static_cast<uint32_t>(KM::keymapKeyEscape));
    CHECK(seq[0].modifiers == 0);
}

TEST(testFunctionKeys)
{
    std::vector<KeyChord> seq;
    CHECK(parseKeySpec("f5", seq));
    CHECK(seq[0].key == static_cast<uint32_t>(KM::keymapKeyF5));
    CHECK(parseKeySpec("cmd+f12", seq));
    CHECK(seq[0].key == static_cast<uint32_t>(KM::keymapKeyF12));
    CHECK(seq[0].modifiers == bindModSuper);
    // F36 is out of range.
    CHECK(!parseKeySpec("f36", seq));
}

TEST(testChordSequence)
{
    std::vector<KeyChord> seq;
    CHECK(parseKeySpec("ctrl+a ctrl+x", seq));
    CHECK(seq.size() == 2);
    CHECK(seq[0].key == 'a' && seq[0].modifiers == bindModCtrl);
    CHECK(seq[1].key == 'x' && seq[1].modifiers == bindModCtrl);
}

TEST(testLiteralPlusKey)
{
    std::vector<KeyChord> seq;
    CHECK(parseKeySpec("cmd++", seq));
    CHECK(seq.size() == 1);
    CHECK(seq[0].key == static_cast<uint32_t>('+'));
    CHECK(seq[0].modifiers == bindModSuper);
}

TEST(testMalformedSpecs)
{
    std::vector<KeyChord> seq;
    CHECK(!parseKeySpec("", seq));
    CHECK(!parseKeySpec("cmd+", seq)); // no key
    CHECK(!parseKeySpec("bogusmod+k", seq)); // unknown modifier
    CHECK(!parseKeySpec("notakey", seq)); // unknown named key
    CHECK(seq.empty());
}

// ---- action parsing ---------------------------------------------------

TEST(testNamedActions)
{
    CHECK(parseAction("copy").kind == ActionKind::Copy);
    CHECK(parseAction("paste").kind == ActionKind::Paste);
    CHECK(parseAction("scroll_page_up").kind == ActionKind::ScrollPageUp);
    CHECK(parseAction("clear_scrollback").kind == ActionKind::ClearScrollback);
    CHECK(parseAction("new_tab").kind == ActionKind::NewTab);
    // Case-insensitive.
    CHECK(parseAction("COPY").kind == ActionKind::Copy);
    // Unknown -> none.
    CHECK(parseAction("frobnicate").kind == ActionKind::None);
    CHECK(!parseAction("frobnicate").valid());
}

TEST(testSendActionEscapes)
{
    BindingAction a = parseAction("send:\\x01");
    CHECK(a.kind == ActionKind::SendBytes);
    CHECK(a.bytes.size() == 1);
    CHECK(static_cast<unsigned char>(a.bytes[0]) == 0x01);

    BindingAction b = parseAction("send:hi\\r\\n");
    CHECK(b.kind == ActionKind::SendBytes);
    CHECK(b.bytes == std::string("hi\r\n"));

    BindingAction e = parseAction("send:\\e[2J");
    CHECK(e.kind == ActionKind::SendBytes);
    CHECK(e.bytes.size() == 4);
    CHECK(static_cast<unsigned char>(e.bytes[0]) == 0x1b);

    // NUL byte via \0.
    BindingAction z = parseAction("send:a\\0b");
    CHECK(z.bytes.size() == 3);
    CHECK(z.bytes[1] == '\0');

    // Malformed \x -> invalid.
    CHECK(!parseAction("send:\\xZZ").valid());
    CHECK(!parseAction("send:\\x1").valid());
}

TEST(testCommandAction)
{
    BindingAction a = parseAction("command:open_settings");
    CHECK(a.kind == ActionKind::Named);
    CHECK(a.name == "open_settings");
}

TEST(testMacroAction)
{
    BindingAction a = parseAction("macro:deploy");
    CHECK(a.kind == ActionKind::Macro);
    CHECK(a.name == "deploy");
    // A macro name with spaces / punctuation rides verbatim in .name.
    BindingAction b = parseAction("macro:clear log");
    CHECK(b.kind == ActionKind::Macro);
    CHECK(b.name == "clear log");
    // Empty name ⇒ not a macro action (the "macro:" prefix needs a name).
    CHECK(parseAction("macro:").kind == ActionKind::None);
}

// ---- binding table + chord resolver -----------------------------------

static KeyChord chord(uint32_t key, uint32_t modifiers)
{
    KeyChord c;
    c.key = key;
    c.modifiers = modifiers;
    return c;
}

TEST(testTableSingleMatch)
{
    BindingTable t;
    CHECK(t.add("cmd+c", "copy"));
    CHECK(t.add("cmd+v", "paste"));
    CHECK(t.count() == 2);

    ChordResolver r(t);
    BindingAction act;
    CHECK(r.feed(chord('c', bindModSuper), act) == ResolveResult::Matched);
    CHECK(act.kind == ActionKind::Copy);
    CHECK(!r.pending());

    // Unbound chord -> no match, default path runs.
    CHECK(r.feed(chord('q', bindModSuper), act) == ResolveResult::NoMatch);
    CHECK(act.kind == ActionKind::None);
}

TEST(testTableRejectsBadEntries)
{
    BindingTable t;
    CHECK(!t.add("cmd+", "copy")); // bad spec
    CHECK(!t.add("cmd+c", "bogus")); // bad action
    CHECK(t.empty());
}

TEST(testLaterBindingOverrides)
{
    BindingTable t;
    CHECK(t.add("cmd+k", "copy"));
    CHECK(t.add("cmd+k", "clear_scrollback")); // newest wins

    ChordResolver r(t);
    BindingAction act;
    CHECK(r.feed(chord('k', bindModSuper), act) == ResolveResult::Matched);
    CHECK(act.kind == ActionKind::ClearScrollback);
}

TEST(testChordStateMachine)
{
    BindingTable t;
    CHECK(t.add("ctrl+a ctrl+x", "send:\\x18"));
    CHECK(t.add("ctrl+a c", "copy"));

    ChordResolver r(t);
    BindingAction act;

    // Leader: ctrl+a is pending (a known prefix), no action yet.
    CHECK(r.feed(chord('a', bindModCtrl), act) == ResolveResult::Pending);
    CHECK(r.pending());
    CHECK(act.kind == ActionKind::None);

    // Completing chord fires the bound action and clears pending.
    CHECK(r.feed(chord('x', bindModCtrl), act) == ResolveResult::Matched);
    CHECK(act.kind == ActionKind::SendBytes);
    CHECK(act.bytes.size() == 1 && static_cast<unsigned char>(act.bytes[0]) == 0x18);
    CHECK(!r.pending());

    // The other branch of the same leader.
    CHECK(r.feed(chord('a', bindModCtrl), act) == ResolveResult::Pending);
    CHECK(r.feed(chord('c', 0), act) == ResolveResult::Matched);
    CHECK(act.kind == ActionKind::Copy);
}

TEST(testBrokenChordRetriesFresh)
{
    BindingTable t;
    CHECK(t.add("ctrl+a ctrl+x", "send:\\x18"));
    CHECK(t.add("cmd+c", "copy"));

    ChordResolver r(t);
    BindingAction act;

    // Start the leader, then press an unrelated *bound* shortcut. The
    // resolver abandons the prefix and matches the shortcut fresh.
    CHECK(r.feed(chord('a', bindModCtrl), act) == ResolveResult::Pending);
    CHECK(r.feed(chord('c', bindModSuper), act) == ResolveResult::Matched);
    CHECK(act.kind == ActionKind::Copy);
    CHECK(!r.pending());
}

TEST(testBrokenChordToNothing)
{
    BindingTable t;
    CHECK(t.add("ctrl+a ctrl+x", "send:\\x18"));

    ChordResolver r(t);
    BindingAction act;
    CHECK(r.feed(chord('a', bindModCtrl), act) == ResolveResult::Pending);
    // An unrelated, unbound key breaks the prefix and matches nothing.
    CHECK(r.feed(chord('z', 0), act) == ResolveResult::NoMatch);
    CHECK(!r.pending());
}

TEST(testResetClearsPrefix)
{
    BindingTable t;
    CHECK(t.add("ctrl+a ctrl+x", "send:\\x18"));
    ChordResolver r(t);
    BindingAction act;
    CHECK(r.feed(chord('a', bindModCtrl), act) == ResolveResult::Pending);
    r.reset();
    CHECK(!r.pending());
    // After reset, the completing chord no longer matches the sequence.
    CHECK(r.feed(chord('x', bindModCtrl), act) == ResolveResult::NoMatch);
}

// resolveChordKey: a non-Latin layout's base char yields the ASCII-capable
// fallback so cmd-chords still match; ASCII passes through, case folds. No
// modifiers here — the shifted-chord case these all pass zero for is
// testAShiftedChordNamesTheKeyRatherThanWhatShiftMadeOfIt's, in
// test_default_bindings.cpp beside the chords that need it.
TEST(testResolveChordKey)
{
    // Russian layout: physical '[' produces Cyrillic 'х' (U+0445); the
    // ASCII-capable layout gives '[' — the binding key must be '['.
    CHECK(resolveChordKey(0x0445, '[', 0) == '[');
    CHECK(resolveChordKey(0x044A, ']', 0) == ']'); // 'ъ' on the ']' key
    // ASCII base char is used directly; the fallback is ignored.
    CHECK(resolveChordKey('[', 0, 0) == '[');
    CHECK(resolveChordKey('k', 0, 0) == 'k');
    // ASCII letters fold to lower-case (shift carries case, not the key).
    CHECK(resolveChordKey('K', 0, 0) == 'k');
    CHECK(resolveChordKey(0x041A, 'K', 0) == 'k'); // Cyrillic 'К' → fallback 'K' → 'k'
    // Non-ASCII base with no usable fallback: leave it (no crash, no match).
    CHECK(resolveChordKey(0x0445, 0, 0) == 0x0445);
    CHECK(resolveChordKey(0x0445, 0x0410, 0) == 0x0445); // fallback also non-ASCII → unchanged
}

// ---- named-action catalog + formatKeySpec round-trip ------------------

TEST(testNamedActionCatalog)
{
    int count = 0;
    const NamedActionEntry* catalog = namedActionCatalog(count);
    CHECK(catalog != nullptr);
    // Not a fixed count: the vocabulary is meant to grow, and a literal here
    // fails on the entry that grows it rather than on anything being wrong.
    // What must hold is that the catalog is non-empty and every token in it is
    // one parseAction accepts.
    CHECK_MSG(count > 0, "the vocabulary is not empty");

    // Every catalog token must parse to a non-none, non-named action — i.e.
    // the UI list can never offer a token parseAction rejects.
    for (int i = 0; i < count; ++i) {
        CHECK(catalog[i].token != nullptr);
        CHECK(catalog[i].label != nullptr);
        const BindingAction action = parseAction(catalog[i].token);
        CHECK(action.valid());
        CHECK(action.kind != ActionKind::Named);
    }

    // Every token is distinct: two entries sharing one would make a settings UI
    // offer the same binding twice and hide whichever came second.
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j)
            CHECK_MSG(std::strcmp(catalog[i].token, catalog[j].token) != 0, "catalog tokens are unique");
    }

    // Spot-check the order matches the menu order the settings UI presents.
    CHECK(std::strcmp(catalog[0].token, "copy") == 0);
    CHECK(std::strcmp(catalog[0].label, "Copy") == 0);
    CHECK(std::strcmp(catalog[count - 1].token, "toggle_beam") == 0);
}

// formatKeySpec(parseKeySpec(spec)) == spec for already-canonical specs.
static void checkSpecRoundTrip(const char* spec)
{
    std::vector<KeyChord> seq;
    CHECK(parseKeySpec(spec, seq));
    CHECK(formatKeySpec(seq) == spec);
}

TEST(testFormatKeySpecRoundTrip)
{
    // Canonical single chords: modifiers in cmd+ctrl+alternate+shift order.
    checkSpecRoundTrip("cmd+shift+k");
    checkSpecRoundTrip("ctrl+a");
    checkSpecRoundTrip("cmd+ctrl+alt+shift+x");
    // Named keys + function keys.
    checkSpecRoundTrip("shift+up");
    checkSpecRoundTrip("ctrl+pageup");
    checkSpecRoundTrip("f3");
    checkSpecRoundTrip("cmd+enter");
    checkSpecRoundTrip("alt+space");
    // A multi-key chord sequence (leader).
    checkSpecRoundTrip("ctrl+a ctrl+x");
    // The literal '+' key.
    checkSpecRoundTrip("cmd++");
}

// The header's claim is parseKeySpec(formatKeySpec(seq)) == seq for any
// sequence parseKeySpec produced, so the population under test is every token
// parseKeySpec accepts — not a reading of it. Walked rather than sampled: the
// case above checks f3, and f3 sits inside the range where the two halves
// agreed, so no sample from there could reach the sixteen where they did not.
//
// An alias is why this compares chords rather than strings: "return" and "esc"
// parse to the same key as "enter" and "escape", and the renderer emits one
// canonical spelling per group, so the spec text is allowed to change while
// the chord it names is not.
static void checkSpecSurvivesFormatting(const std::string& spec)
{
    std::vector<KeyChord> parsed;
    CHECK_MSG(parseKeySpec(spec.c_str(), parsed), spec.c_str());
    const std::string rendered = formatKeySpec(parsed);
    const std::string complaint = spec + " parses but renders back as \"" + rendered + "\"";
    CHECK_MSG(!rendered.empty(), complaint.c_str());
    std::vector<KeyChord> reparsed;
    CHECK_MSG(parseKeySpec(rendered, reparsed), complaint.c_str());
    CHECK_MSG(reparsed == parsed, complaint.c_str());
}

TEST(everyTokenTheParserAcceptsSurvivesBeingFormatted)
{
    // f1..f35 is the range keyCodePoint parses; keyTokenForCodePoint stopped at
    // f19, so f20..f35 parsed into a chord that rendered as nothing.
    for (int n = 1; n <= 35; ++n)
        checkSpecSurvivesFormatting("f" + std::to_string(n));

    // Every named key, both spellings of each alias group.
    for (const char* name : { "enter", "return", "tab", "escape", "esc", "space",
             "backspace", "delete", "insert", "left", "right", "up", "down",
             "pageup", "pagedown", "home", "end" })
        checkSpecSurvivesFormatting(name);

    // The printable single characters, which render as themselves.
    for (char c = '!'; c <= '~'; ++c)
        checkSpecSurvivesFormatting(std::string(1, c));
}

TEST(testFormatKeySpecCanonicalizes)
{
    // A non-canonical input (alias modifiers, upper-case letter, reordered mods)
    // parses then formats to the one canonical spelling.
    std::vector<KeyChord> seq;
    CHECK(parseKeySpec("shift+control+K", seq));
    CHECK(formatKeySpec(seq) == "ctrl+shift+k");
    CHECK(parseKeySpec("option+command+return", seq));
    CHECK(formatKeySpec(seq) == "cmd+alt+enter");
    // The formatted spec must itself re-parse to the same chord sequence.
    std::vector<KeyChord> reparsed;
    CHECK(parseKeySpec(formatKeySpec(seq), reparsed));
    CHECK(reparsed == seq);
}

TEST(testFormatKeySpecEmpty)
{
    std::vector<KeyChord> empty;
    CHECK(formatKeySpec(empty).empty());
}

UBIQUITTY_TEST_MAIN()
