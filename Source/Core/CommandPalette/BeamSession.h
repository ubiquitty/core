// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/CommandPalette/BeamSession.h — the palette's state machine.
//
// `Engine` already ranks a query against a scope's commands. This is the part
// that was only ever written in Swift: what is open, what has been typed, which
// row is selected, and what each event does to those.
//
// It lives here so a frontend drawing Beam as native chrome and one drawing it
// into the cell grid share it. The two then cannot drift in BEHAVIOUR however
// differently they look, which is what makes the surface a rendering choice
// rather than a fork — and it is why `appearance.beam_surface` can be a setting
// at all.
//
// It runs wherever the surface does. Every frontend links Core, including the
// wasm client, so an attached browser runs this locally and filtering costs no
// round trip; only the host's own scope needs the wire. A session that ran
// host-side would spend an RTT per keystroke in the one surface whose whole
// value is feeling instant.

#include "Core/CommandPalette/BeamState.h"
#include "Core/CommandPalette/PaletteCommand.h"
#include "Core/CommandPalette/PaletteEngine.h"
#include "Core/CommandPalette/PaletteScope.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Ubiquitty::CommandPalette {

/// What the caller must do after an event, beyond redrawing.
///
/// The session never executes anything: it has no idea what a command id means,
/// and a Core type that ran frontend actions would be the extraction working
/// backwards. It says which row was chosen and how; the caller decides.
struct BeamAction {
    enum class Kind : std::uint8_t {
        None, ///< Redraw and nothing else.
        Run, ///< Execute `commandID`.
        Closed, ///< The palette just closed; return focus to the terminal.
    };
    Kind kind = Kind::None;
    std::string commandID;
    /// Which value was chosen, when `commandID` names an options drill; -1 for
    /// a plain command. An index into the option list the caller registered,
    /// not into the ranked rows — ranking reorders them, and a caller applying
    /// the row position would set a different value the moment a query narrowed
    /// the list.
    int optionIndex = -1;
    /// The value entered, when `commandID` names a NUMBER drill; already parsed
    /// and clamped to the registered bounds. Meaningless for anything else, so a
    /// caller reads it only under the id it registered a number for.
    double numberValue = 0.0;
    /// Whether the alternate accept was used — the modified Enter. What that
    /// means is the caller's; the session only records which one happened.
    bool isAlternate = false;
};

/// The palette's live state, and the events that move it.
class BeamSession {
public:
    /// Replaces the command set for a scope and re-ranks against the current
    /// query. A frontend calls this when it opens the palette and whenever its
    /// own command list changes.
    void setCommands(Scope scope, PaletteCommandList commands);

    /// Declares whether a terminal is attached, gating the terminal-only scopes.
    ///
    /// `Scope::Blocks`, `GoTo` and `Find` read a terminal; an agent client that
    /// has none must not be able to enter them by typing their prefix. Default
    /// `Unavailable`, so a caller that never says answers no rather than
    /// offering a scope it cannot fill.
    ///
    /// - Parameter availability: Whether the caller has a terminal to read.
    void setTerminalAvailability(TerminalAvailability availability) { m_terminalAvailability = availability; }

    /// Frecency scores, so recently used commands rank above equally-matching
    /// ones. Optional: an empty map ranks on the match alone.
    void setUsageScores(UsageScoreMap scores);

    /// Registers the values behind a `CommandKind::DrillOptions` command.
    ///
    /// Held rather than fetched through a callback: a callback would let the
    /// list change under a drill that is already open, and the session would
    /// have no way to tell that from the user typing.
    ///
    /// - Parameters:
    ///   - commandID: The command these values belong to.
    ///   - options: The values, in the order the caller wants them listed.
    ///   - currentIndex: Which one is in effect, or -1 for none. Marked with
    ///     `isChecked` so the drill shows what it is about to change.
    void setOptions(std::string commandID, std::vector<PaletteOption> options, int currentIndex);

    /// Registers the schema behind a `CommandKind::DrillNumber` command.
    ///
    /// Entering it puts the LIVE value in the field rather than an empty box:
    /// the field is the value, so an empty one reads as "unset" and the first
    /// Enter would apply a fallback the user never saw.
    ///
    /// - Parameters:
    ///   - commandID: The command this value belongs to.
    ///   - number: Its value, bounds, step, and display unit.
    void setNumber(std::string commandID, PaletteNumber number);

    /// Whether a drill is open — a value list or a number editor.
    [[nodiscard]] bool isDrilled() const { return !m_state.drillTitle.empty(); }

    /// Applies one event.
    ///
    /// - Parameters:
    ///   - event: What happened.
    ///   - codePoint: The character to insert, read only for `Character`.
    /// - Returns: What the caller must do beyond redrawing.
    BeamAction apply(BeamEvent event, std::uint32_t codePoint = 0);

    /// How many rows a caller can show at once. Paging and the scroll window
    /// follow it; a viewport of zero disables both rather than dividing by it.
    void setViewportRows(int rows);

    /// The state to draw. Rebuilt on demand from the engine's current ranking,
    /// so there is no second copy to keep in step.
    [[nodiscard]] const BeamState& state() const { return m_state; }

    [[nodiscard]] bool isOpen() const { return m_state.isOpen; }

private:
    /// Points the engine at a scope's registered command list.
    ///
    /// An unregistered scope ranks nothing rather than whatever the previous one
    /// held, so a palette retargeted to a scope the caller never filled shows an
    /// empty list instead of the last scope's answers under a new name.
    ///
    /// - Parameter scope: The command set to rank against.
    void activateScope(Scope scope);
    void reRank();
    void reRankOptions();
    /// Rebuilds the value editor's picture: no rows, and the range hint in the
    /// status line.
    void refreshNumberEditor();

    /// Steps the edited value by one stride and writes it back to the field.
    ///
    /// Steps off what is in the field rather than off the registered value, so
    /// an arrow after typing continues from the typed number.
    ///
    /// - Parameter direction: +1 to increase by one step, -1 to decrease.
    void stepNumber(int direction);
    void appendMatchingValues();
    void clampSelection();
    void bump() { ++m_state.generation; }

    /// The values behind one command, with which is current.
    struct OptionSet {
        std::vector<PaletteOption> options;
        int currentIndex = -1;
    };

    void enterDrill(const std::string& commandID, const std::string& title);
    void leaveDrill();

    Engine m_engine;
    BeamState m_state;
    Scope m_scope = Scope::Actions;
    TerminalAvailability m_terminalAvailability = TerminalAvailability::Unavailable;
    /// One command list per scope, so a prefix can retarget without the caller
    /// re-supplying what it already registered.
    std::map<Scope, PaletteCommandList> m_scopeCommands;
    int m_viewportRows = 10;
    std::map<std::string, OptionSet> m_options;
    std::map<std::string, PaletteNumber> m_numbers;
    /// The drilled command's id, empty at the command list. Paired with
    /// `BeamState::drillTitle`, which is the same fact in the shape a renderer
    /// wants; this is the one a lookup wants.
    std::string m_drillID;
    /// The command-list filter, saved on drill-in and restored on drill-out.
    /// Without it, backing out of a drill drops the user at an unfiltered list
    /// they then have to retype their way back into.
    std::string m_savedQuery;
};

} // namespace Ubiquitty::CommandPalette
