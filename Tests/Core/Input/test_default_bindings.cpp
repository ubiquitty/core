// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include <string>

// test_default_bindings.cpp — the built-in default bindings and the one rule
// that retires them (Core/Input/Keybindings.cpp). A default is a rebind target,
// not an addition: a user who binds its action anywhere replaces it, and its
// chord goes back to being an ordinary key. Both macOS shells read this table
// and this rule, so neither spells the chord or the yield itself.

#include "Core/Input/Keybindings.h"

#include <vector>

using namespace Ubiquitty::Input;

namespace {

constexpr KeyChord beamChord { 'p', bindModSuper | bindModShift };

} // namespace

// The stored chord is what the keystroke path compares and the spec is what a
// settings UI shows, so the two are pinned to each other.
TEST(theDefaultCatalogSpellsItsChordAndItsSpecTheSameWay)
{
    int count = 0;
    const DefaultBindingEntry* entries = defaultBindingCatalog(count);
    CHECK_MSG(count >= 1, "the catalog holds a default");
    bool hasBeam = false;
    for (int i = 0; i < count; ++i) {
        std::vector<KeyChord> parsed;
        CHECK_MSG(parseKeySpec(entries[i].spec, parsed), "every spec parses");
        CHECK_MSG(parsed.size() == 1 && parsed[0] == entries[i].chord, "and parses to the chord stored beside it");
        CHECK_MSG(entries[i].action != ActionKind::None, "and runs something");
        hasBeam = hasBeam || (entries[i].chord == beamChord && entries[i].action == ActionKind::ToggleBeam);
    }
    CHECK_MSG(hasBeam, "cmd+shift+p summons Beam");
}

// The yield: a rebind of the action retires the default wherever the user
// wrote it, and the handover is not a loss.
TEST(aRebindOfTheActionRetiresTheDefaultWhereverItWasWritten)
{
    BindingTable table;
    CHECK(table.add("ctrl+alt+b", "toggle_beam"));
    CHECK(table.hasBindingForAction(ActionKind::ToggleBeam));

    BindingAction action;
    CHECK_MSG(!resolveDefaultBinding(table, beamChord, action), "the default yields to a rebind on another chord");
    CHECK_MSG(action.kind == ActionKind::None, "and leaves the action untouched");

    const int index = table.matchSingle(KeyChord { 'b', bindModCtrl | bindModAlternate });
    CHECK_MSG(index >= 0 && table.at(index).action.kind == ActionKind::ToggleBeam, "the rebind is the one that summons");
}

// A binding on the default's own chord owns it outright; a table binding some
// other action leaves the default standing, so the yield above is not a
// resolver that refuses every non-empty table.
TEST(aBindingOnTheDefaultChordOwnsIt)
{
    BindingTable owning;
    CHECK(owning.add("cmd+shift+p", "send:x"));
    BindingAction action;
    CHECK_MSG(!resolveDefaultBinding(owning, beamChord, action), "the user's binding on the chord wins outright");

    BindingTable unrelated;
    CHECK(unrelated.add("ctrl+alt+c", "copy"));
    CHECK(!unrelated.hasBindingForAction(ActionKind::ToggleBeam));
    CHECK_MSG(resolveDefaultBinding(unrelated, beamChord, action), "an unrelated table leaves the default standing");
    CHECK(action.kind == ActionKind::ToggleBeam);

    BindingTable empty;
    action = BindingAction {};
    CHECK_MSG(!resolveDefaultBinding(empty, KeyChord { 'q', bindModSuper | bindModShift }, action), "a chord no default claims is nobody's");
}

// The bracket pair every browser and editor on this platform cycles tabs with.
// Their direction follows the brackets: `[` points back, `]` points forward.
TEST(testTheBracketChordsCycleTabs)
{
    int count = 0;
    const Ubiquitty::Input::DefaultBindingEntry* catalog = Ubiquitty::Input::defaultBindingCatalog(count);
    bool sawPrevious = false;
    bool sawNext = false;
    for (int index = 0; index < count; ++index) {
        const Ubiquitty::Input::DefaultBindingEntry& entry = catalog[index];
        if (entry.action == Ubiquitty::Input::ActionKind::PrevTab) {
            sawPrevious = true;
            CHECK(entry.chord.key == '[');
            CHECK(entry.chord.modifiers == (Ubiquitty::Input::bindModSuper | Ubiquitty::Input::bindModShift));
            CHECK_MSG(std::string(entry.spec) == "cmd+shift+[", "the spec spells the chord it carries");
        }
        if (entry.action == Ubiquitty::Input::ActionKind::NextTab) {
            sawNext = true;
            CHECK(entry.chord.key == ']');
            CHECK(entry.chord.modifiers == (Ubiquitty::Input::bindModSuper | Ubiquitty::Input::bindModShift));
            CHECK_MSG(std::string(entry.spec) == "cmd+shift+]", "the spec spells the chord it carries");
        }
    }
    CHECK(sawPrevious);
    CHECK(sawNext);
}

// A binding names the key and carries shift beside it, so a chord built from
// the character shift PRODUCED matches nothing a person would write. macOS and
// the web both report that produced character; the layout says which key it
// was, and asking the layout is what keeps this right off a US keyboard.
TEST(testAShiftedChordNamesTheKeyRatherThanWhatShiftMadeOfIt)
{
    using Ubiquitty::Input::bindModShift;
    using Ubiquitty::Input::bindModSuper;
    using Ubiquitty::Input::resolveChordKey;

    // Shift and the bracket key: reported as '{', named as '['.
    CHECK(resolveChordKey('{', '[', bindModSuper | bindModShift) == '[');
    CHECK(resolveChordKey('}', ']', bindModSuper | bindModShift) == ']');
    // The same key without shift is itself, and no layout answer is needed.
    CHECK(resolveChordKey('[', 0, bindModSuper) == '[');

    // A platform that already reports the unshifted character passes no layout
    // answer and keeps its own: this is the Linux arm, which unshifts the
    // keyval before asking.
    CHECK_MSG(resolveChordKey('3', 0, bindModShift) == '3', "an arm that already unshifted keeps its answer");

    // Letters need no layout answer: their shifted form differs only in case.
    CHECK(resolveChordKey('K', 0, bindModSuper | bindModShift) == 'k');

    // The non-Latin recovery still stands, and does not need shift to fire.
    CHECK(resolveChordKey(0x043A /* Cyrillic ka */, 'k', bindModSuper) == 'k');
}

UBIQUITTY_TEST_MAIN()
