// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Ubiquitty::Title {

/// Who holds a pane's terminal: a program the shell started, or the shell
/// itself, back at its prompt.
enum class TerminalHolder : std::uint8_t {
    Program,
    Shell,
};

/// How a frontend paints a program's icon.
enum class ProcessIconInk : std::uint8_t {
    /// One colour, drawn in the title's own ink, so it reads on any theme.
    Template,
    /// The icon's own colours, as its owner draws it.
    Drawn,
};

/// One program a title can mark with its icon: what it is called, where its
/// icon is bundled and how it is painted.
struct ProcessIcon {
    /// The catalog's key, which names the program in a test or a log.
    std::string_view key {};
    /// The program's name as its owner writes it, for a tooltip or a spoken label.
    std::string_view title {};
    /// The icon's file in the bundle's process-icons directory.
    std::string_view file {};
    ProcessIconInk ink = ProcessIconInk::Template;
    /// A drawn icon's main colour, which its contrast with a title's ground is
    /// measured by; unused for a template icon.
    std::uint32_t inkRGB = 0;
    /// Whether a drawn icon brings its own ground, a tile, and so never needs a plate.
    bool hasOwnPlate = false;
    /// The lowercase names it answers to: an OSC 7501 app, a process name.
    std::array<std::string_view, 4> names {};
    std::size_t nameCount = 0;
};

} // namespace Ubiquitty::Title
