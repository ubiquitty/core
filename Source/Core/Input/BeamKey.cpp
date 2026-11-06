// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Input/BeamKey.cpp — see BeamKey.h.

#include "Core/Input/BeamKey.h"

#include "Core/Keys/Keymap.h"

namespace Ubiquitty::Input {

namespace {

using CommandPalette::BeamEvent;

BeamKeyOutcome consumed(BeamEvent event, std::uint32_t codePoint = 0)
{
    return BeamKeyOutcome { true, event, codePoint };
}

BeamKeyOutcome passedThrough()
{
    return BeamKeyOutcome {};
}

} // namespace

BeamKeyOutcome classifyBeamKey(std::uint32_t key, std::uint32_t modifiers, std::uint32_t text)
{
    const bool isShiftHeld = (modifiers & Keys::Keymap::keymapModifierShift) != 0;
    const bool isControlHeld = (modifiers & Keys::Keymap::keymapModifierCtrl) != 0;
    const bool isAlternateHeld = (modifiers & Keys::Keymap::keymapModifierAlternate) != 0;
    const bool isSuperHeld = (modifiers & Keys::Keymap::keymapModifierSuper) != 0;

    switch (static_cast<int>(key)) {
    case Keys::Keymap::keymapKeyEscape:
        return consumed(BeamEvent::Dismiss);
    case Keys::Keymap::keymapKeyEnter:
        // The modified Enter is the "other way" — open in a split, reveal rather
        // than run. Which modifier means that is the frontend's convention
        // elsewhere; here any of the three reads as alternate, so a user's habit
        // from another tool works rather than silently doing the ordinary thing.
        return consumed((isShiftHeld || isControlHeld || isAlternateHeld) ? BeamEvent::AcceptAlternate : BeamEvent::Accept);
    case Keys::Keymap::keymapKeyBackspace:
        return consumed(BeamEvent::Backspace);
    case Keys::Keymap::keymapKeyUp:
        return consumed(BeamEvent::SelectPrevious);
    case Keys::Keymap::keymapKeyDown:
        return consumed(BeamEvent::SelectNext);
    case Keys::Keymap::keymapKeyPageUp:
        return consumed(BeamEvent::PageUp);
    case Keys::Keymap::keymapKeyPageDown:
        return consumed(BeamEvent::PageDown);
    case Keys::Keymap::keymapKeyHome:
        return consumed(BeamEvent::SelectFirst);
    case Keys::Keymap::keymapKeyEnd:
        return consumed(BeamEvent::SelectLast);
    case Keys::Keymap::keymapKeyRight:
        return consumed(BeamEvent::Descend);
    case Keys::Keymap::keymapKeyLeft:
        return consumed(BeamEvent::Ascend);
    case Keys::Keymap::keymapKeyTab:
        // Tab moves through the list rather than inserting one. A palette is a
        // list; a tab character in a filter query has no meaning any scope reads.
        return consumed(isShiftHeld ? BeamEvent::SelectPrevious : BeamEvent::SelectNext);
    default:
        break;
    }

    // Emacs-style motion, because a terminal's users have it in their fingers and
    // a palette that ignores it feels foreign inside a terminal specifically.
    if (isControlHeld && !isAlternateHeld && !isSuperHeld) {
        switch (static_cast<int>(key)) {
        case 'p':
            return consumed(BeamEvent::SelectPrevious);
        case 'n':
            return consumed(BeamEvent::SelectNext);
        case 'a':
            return consumed(BeamEvent::SelectFirst);
        case 'e':
            return consumed(BeamEvent::SelectLast);
        default:
            break;
        }
    }

    // Ordinary typing. Control- and Super-modified keys are not text: they are
    // the frontend's own bindings, and swallowing them would make an open
    // palette eat the window shortcuts. Alternate is left to the frontend for
    // the same reason — a composed character arrives here as `text` without the
    // modifier set.
    if (text >= 0x20 && text != 0x7F && !isControlHeld && !isSuperHeld)
        return consumed(BeamEvent::Character, text);

    // Anything else reaches the shell. An open palette that consumed every key
    // would make a mistyped shortcut look like a hung terminal.
    return passedThrough();
}

} // namespace Ubiquitty::Input
