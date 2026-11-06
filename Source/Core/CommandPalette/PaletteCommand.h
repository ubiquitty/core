// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// PaletteCommand.h — the portable command + result data model. A command
// carries no run closure: `id` is the stable key a frontend maps to the real
// action (a selector, a setting setter, a tab switch). The engine ranks these
// and hands back results the renderer draws.
//

#include "Base/Compiler.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::CommandPalette {

// Selecting a command either runs an effect (Action) or drills into a
// second-level value editor that stays inside the palette. The kind is data —
// the frontend resolves the id to the actual effect / setter.
// A fixed closed set: marked closed so Swift/C++ interop imports it @frozen (an
// exhaustive Swift switch needs no @unknown default, and stays valid under the
// Swift 6 language mode).
enum class UBIQUITTY_CLOSED_ENUM CommandKind : std::uint8_t {
    Action, // run the effect immediately
    DrillOptions, // a value list (segmented / pull-down setting)
    DrillNumber, // a numeric editor (stepper / slider setting)
    DrillText, // a free-text editor
};

constexpr bool isDrill(CommandKind kind) { return kind != CommandKind::Action; }

// One actionable command. `keywords` are extra fuzzy-match terms beyond the
// title (never displayed); `group` is the dim right-side label; `value` is the
// live value preview a drill command shows; `shortcut` is the glyph form of a
// keyboard equivalent, or empty. Nothing here is platform- or UI-specific.
// Scope membership and confirmation gating live with the frontend's
// closure-bearing command: the engine receives one scope's set via setActive
// and never runs anything itself.
struct PaletteCommand {
    std::string id;
    std::string title;
    std::string group;
    std::string shortcut;
    std::string value;
    std::vector<std::string> keywords;
    CommandKind kind = CommandKind::Action;
};

// One value in an options drill (a segmented / pull-down setting). Public
// descriptor: a frontend registers the value list, the engine ranks it.
struct PaletteOption {
    std::string label;
};

/// The schema behind a `CommandKind::DrillNumber` command.
///
/// Carries the live value, the bounds an entry clamps to, the stride the arrows
/// step by, and the display unit. A frontend resolves these from
/// `Preferences/Schema.h` and registers them; the palette formats, edits and
/// clamps, and hands back a number.
///
/// Registered like an option list rather than fetched through a callback, for
/// the same reason: a getter would let the value move under an editor that is
/// already open, and the session could not tell that from the user typing.
struct PaletteNumber {
    double value = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double step = 1.0;
    std::string unit;
};

// One ranked row the engine hands back — only what the frontend can't derive
// from the command it already holds by id: `matchedIndices` are ascending
// code-point indices into the command's title to highlight; `checked` marks
// the current value in an options drill (where `id` is the option's original
// index). CellGrid fields ride the frontend's own command object, so the hot
// per-keystroke path never copies them back across.
struct PaletteResult {
    std::string id;
    bool checked = false;
    std::vector<int> matchedIndices;
};

// Named specializations of the palette's container types. C++ code can keep
// using std::vector<…> directly; these give the Swift/C++ interop importer a
// spellable name for each (Swift can't name a bare std::vector<T> without its
// allocator parameter).
using PaletteCommandList = std::vector<PaletteCommand>;
using PaletteOptionList = std::vector<PaletteOption>;
using PaletteResultList = std::vector<PaletteResult>;

} // namespace Ubiquitty::CommandPalette
