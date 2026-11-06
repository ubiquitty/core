// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/CommandPalette/BeamState.h — what the command palette is showing,
// and the events that change it.
//
// The state is portable and the surface is not. A frontend drawing Beam as
// native chrome and one drawing it into the cell grid read the same state, so
// the two cannot drift in behaviour however differently they look — which is
// what makes `appearance.beam_surface` a rendering choice rather than a fork.
//
// `BeamEvent` is the whole vocabulary. A frontend classifies its own key events
// into these (see Core/Input/BeamKey.h) and applies them; nothing downstream
// knows about key codes, modifiers, or which platform produced them.

#include "Core/CommandPalette/PaletteScope.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::CommandPalette {

/// Everything a user can do to the palette.
///
/// Closed on purpose: a switch over it is checked exhaustively by
/// `Scripts/check-enum-switches.py`, so adding an event forces every consumer to
/// say what it does rather than falling through a default into silence.
enum class BeamEvent : std::uint8_t {
    Toggle, ///< Open when closed, close when open.
    Dismiss, ///< Close, whatever the state. Escape, and a click outside.
    Character, ///< Append the code point the caller passes alongside.
    Backspace, ///< Delete the code point before the caret.
    SelectPrevious,
    SelectNext,
    PageUp,
    PageDown,
    SelectFirst,
    SelectLast,
    Accept, ///< Run the selected row.
    AcceptAlternate, ///< Run it the other way — the modified Enter.
    Descend, ///< Enter the selected row's sub-list.
    Ascend, ///< Leave it, or clear the query when there is nothing to leave.
};

/// One row the palette is offering.
///
/// `matchedIndices` are offsets into `title`'s CODE POINTS, not its bytes, so a
/// highlight lands on the character the matcher matched rather than mid-sequence
/// on anything outside ASCII.
struct BeamRow {
    std::string title;
    std::string detail; ///< Secondary text, drawn dim. May be empty.
    std::vector<int> matchedIndices;
    bool hasChildren = false; ///< Whether Descend does anything here.
    bool isChecked = false; ///< The current value, inside an options drill.

    /// What accepting this row runs. Carried ON the row rather than recovered by
    /// re-ranking at accept time: a second ranking is a second chance to
    /// disagree with what the user is looking at, and the row they picked is the
    /// only thing that can say what they picked.
    std::string commandID;

    /// The value's own index when this row is a setting VALUE rather than a
    /// command, else -1. Not the row position — ranking reorders, so a caller
    /// applying the position sets a different value the moment a query narrows
    /// the list.
    int optionIndex = -1;
};

/// What the palette is showing right now.
///
/// A plain aggregate with no invariants of its own: `BeamSession` maintains it,
/// and `BeamLayout` reads it. Keeping it inert is what lets a test build one by
/// hand and assert the picture without driving a session to reach that shape.
struct BeamState {
    bool isOpen = false;
    /// The command being drilled into, or empty at the command list. A frontend
    /// draws it as a breadcrumb; its emptiness is also what tells one which list
    /// is on screen without asking the session a second question.
    std::string drillTitle;
    /// Whether the open drill is a value EDITOR — `query` is the value being
    /// entered rather than a filter, and there is no list.
    ///
    /// Without this an empty `rows` inside a drill is ambiguous: a number editor
    /// showing nothing and an options list filtered down to nothing are the same
    /// picture, and a frontend that guesses draws "no matching values" under a
    /// field that is working exactly as intended.
    bool isValueEditor = false;
    /// Which command set is being ranked. A prefix retargets it, and a frontend
    /// draws it as the chip naming what is being searched.
    Scope scope = Scope::Actions;
    std::string query;
    std::vector<BeamRow> rows;
    /// Index into `rows`, or -1 when there are none. Never points past the end.
    int selectedIndex = -1;
    /// First visible row, for a list taller than its viewport.
    int scrollTop = 0;
    /// Breadcrumb of the sub-lists entered so far; empty at the top level.
    std::vector<std::string> path;
    /// A line drawn under the list — a count, a hint, or an error.
    std::string status;
    /// Bumped on every change, so a frontend can skip work when nothing moved
    /// and a wire can carry a difference rather than the whole state.
    std::uint64_t generation = 0;
};

} // namespace Ubiquitty::CommandPalette
