// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_beam_key — who gets the keystroke while the palette is open.
//
// The failure is not a wrong glyph, it is a key going to both places or to
// neither. Two shapes, and the second is the one a test usually misses:
//
//   the palette does not consume what it has no use for — otherwise an open
//   palette eats the window shortcuts and reads as a hung terminal
//   the palette does consume what it needs — otherwise a typed character
//   reaches the shell as well as the filter
//
// So every case asserts `handled` explicitly, in both directions. A
// classification that returned handled=true for everything would pass any test
// that only checked which event came back.

#include "Harness.h"

#include "Core/Input/BeamKey.h"
#include "Core/Keys/Keymap.h"

using Ubiquitty::CommandPalette::BeamEvent;
using Ubiquitty::Input::BeamKeyOutcome;
using Ubiquitty::Input::classifyBeamKey;

namespace {

BeamKeyOutcome press(int key, int modifiers = 0, std::uint32_t text = 0)
{
    return classifyBeamKey(static_cast<std::uint32_t>(key), static_cast<std::uint32_t>(modifiers), text);
}

constexpr int shift = Ubiquitty::Keys::Keymap::keymapModifierShift;
constexpr int control = Ubiquitty::Keys::Keymap::keymapModifierCtrl;
constexpr int alternate = Ubiquitty::Keys::Keymap::keymapModifierAlternate;
constexpr int super = Ubiquitty::Keys::Keymap::keymapModifierSuper;

} // namespace

TEST(beamKeyConsumesNavigationAndEditing)
{
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyEscape).handled);
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyEscape).event == BeamEvent::Dismiss);

    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyEnter).event == BeamEvent::Accept);
    CHECK_MSG(press(Ubiquitty::Keys::Keymap::keymapKeyEnter, shift).event == BeamEvent::AcceptAlternate,
        "a modified Enter is the other way of running a row, not the same way");

    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyUp).event == BeamEvent::SelectPrevious);
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyDown).event == BeamEvent::SelectNext);
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyPageUp).event == BeamEvent::PageUp);
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyHome).event == BeamEvent::SelectFirst);
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyEnd).event == BeamEvent::SelectLast);
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyRight).event == BeamEvent::Descend);
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyLeft).event == BeamEvent::Ascend);
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyBackspace).event == BeamEvent::Backspace);

    // Tab moves through the list. A palette is a list, and a tab character in a
    // filter query means nothing to any scope.
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyTab).event == BeamEvent::SelectNext);
    CHECK(press(Ubiquitty::Keys::Keymap::keymapKeyTab, shift).event == BeamEvent::SelectPrevious);

    // Emacs motion, because a terminal's users have it in their fingers.
    CHECK(press('n', control).event == BeamEvent::SelectNext);
    CHECK(press('p', control).event == BeamEvent::SelectPrevious);
}

TEST(beamKeyTypesTextAndOnlyText)
{
    const BeamKeyOutcome letter = press('a', 0, 'a');
    CHECK(letter.handled);
    CHECK(letter.event == BeamEvent::Character);
    CHECK_EQ(static_cast<int>(letter.codePoint), static_cast<int>('a'));

    // A space is text. Easy to lose to a "printable" test that stops at 0x21.
    const BeamKeyOutcome space = press(' ', 0, ' ');
    CHECK(space.handled);
    CHECK(space.event == BeamEvent::Character);

    // Non-ASCII is text too — the query is UTF-8 and a filter that dropped it
    // would be unusable in most of the world.
    const BeamKeyOutcome accented = press(0xE9, 0, 0xE9);
    CHECK(accented.handled);
    CHECK_EQ(static_cast<int>(accented.codePoint), 0xE9);
}

// The half a test usually skips, and the one that decides whether an open
// palette is usable: what it must not take.
TEST(beamKeyPassesThroughWhatItDoesNotOwn)
{
    // Window and application shortcuts. An open palette that swallowed these
    // would make every Cmd- and Ctrl- binding dead while it was up.
    CHECK_MSG(!press('t', super, 't').handled, "Super-modified keys stay the frontend's");
    CHECK_MSG(!press('c', control, 'c').handled, "and so do Control-modified ones");
    CHECK_MSG(!press('w', super | shift, 'w').handled, "including with other modifiers alongside");

    // A control key the palette has no use for. F5 is not navigation and not
    // text; consuming it would silently break a binding the user set.
    CHECK_MSG(!press(Ubiquitty::Keys::Keymap::keymapKeyF5).handled, "an unclaimed functional key reaches the shell");
    CHECK_MSG(!press(Ubiquitty::Keys::Keymap::keymapKeyInsert).handled, "and so does Insert");

    // A key that produced no text and is not a known chord: nothing to type, no
    // action to take.
    CHECK_MSG(!press(Ubiquitty::Keys::Keymap::keymapKeyCapsLock).handled, "a modifier key alone does nothing");

    // A C0 control is not text. A palette taking 0x03 would eat the interrupt.
    CHECK_MSG(!press(3, 0, 3).handled, "a C0 control is not a character");

    // 0x7F is not a passthrough case: `keymapKeyBackspace` is 0x7F, so DEL
    // and Backspace are one key in this vocabulary rather than two. It is
    // claimed, as editing.
    CHECK_MSG(press(0x7F, 0, 0x7F).event == BeamEvent::Backspace,
        "DEL is Backspace here — the key vocabulary gives them one code");

    // Control-p is claimed, so the Control passthrough above is not a blanket
    // rule — asserted here so the two cannot both pass by the same mistake.
    CHECK_MSG(press('p', control).handled, "the Emacs motions are the one Control range the key vocabulary claims");
    CHECK_MSG(!press('p', control | alternate).handled,
        "but only bare Control — with another modifier it is the frontend's binding again");
}

UBIQUITTY_TEST_MAIN()
