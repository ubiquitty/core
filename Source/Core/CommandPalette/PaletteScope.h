// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// PaletteScope.h — the portable command-palette scope model: the retargeting
// prefixes and the per-scope behavioral facts. No platform / UI types — a
// frontend maps each scope to its own presentation strings and command source.
//

#include "Base/Compiler.h"

#include <cstdint>
#include <iterator>
#include <optional>

namespace Ubiquitty::CommandPalette {

// The palette's five sources. Actions is the default (no prefix); the rest are
// entered by typing their leading prefix as the first character.
//
// UBIQUITTY_CLOSED_ENUM makes Swift/C++ interop import this as a FROZEN Swift
// enum: a scoped C++ enum is otherwise imported NON-frozen, so an exhaustive
// Swift `switch` over it demands `@unknown default` (a hard error in the Swift 6
// language mode). Scope is a fixed, closed set, so mark it closed — see
// Base/Compiler.h. (The macro guards the clang-only attribute so it's
// a no-op on GCC rather than -Werror=attributes.)
enum class UBIQUITTY_CLOSED_ENUM Scope : std::uint8_t {
    Actions, // no prefix — the command registry (menu + every setting)
    Settings, // '#' — the settings subset, still adjustable inline
    GoTo, // ':' — open tabs and split panes
    Blocks, // '@' — OSC 133 command blocks of the focused terminal
    Find, // '/' — scrollback search of the focused terminal
};

// The single leading character that retargets the palette at a scope, or '\0'
// for Actions (no prefix). The prefix table lives here alone.
constexpr char prefixFor(Scope scope)
{
    switch (scope) {
    case Scope::Settings:
        return '#';
    case Scope::GoTo:
        return ':';
    case Scope::Blocks:
        return '@';
    case Scope::Find:
        return '/';
    case Scope::Actions:
        return '\0';
    }
    return '\0';
}

// The scope a leading prefix character selects, or nullopt when the character is
// not a scope prefix (the text is an ordinary query).
constexpr std::optional<Scope> scopeForPrefix(char character)
{
    switch (character) {
    case '#':
        return Scope::Settings;
    case ':':
        return Scope::GoTo;
    case '@':
        return Scope::Blocks;
    case '/':
        return Scope::Find;
    default:
        return std::nullopt;
    }
}

// The two tables above are inverses, and nothing but this says so. A scope whose
// prefix is edited in one and not the other is reachable by a character that
// maps back to a different scope — the palette then retargets on '@' and ranks
// Find, which reads as a ranking bug rather than as a table that disagrees with
// itself.
//
// Checked at COMPILE time rather than by a test, because both functions are
// constexpr and a table this small has no runtime state to observe: a test would
// have to re-list the pairs, which is a third copy of the thing being verified.
static_assert([] {
    // Every scope with a prefix maps back to itself.
    for (const Scope scope : { Scope::Settings, Scope::GoTo, Scope::Blocks, Scope::Find }) {
        const char prefix = prefixFor(scope);
        if (prefix == '\0')
            return false; // a prefixed scope must name a character
        const std::optional<Scope> back = scopeForPrefix(prefix);
        if (!back || *back != scope)
            return false;
    }
    // Actions is the unprefixed default, and its NUL must not resolve to a scope.
    if (prefixFor(Scope::Actions) != '\0')
        return false;
    return !scopeForPrefix('\0').has_value();
}(),
    "prefixFor and scopeForPrefix disagree: a scope's prefix maps back to a different scope");

// No two scopes share a prefix. The round trip above passes when two scopes both
// map to '@' and scopeForPrefix happens to answer the one being tested first, so
// the collision needs its own statement.
static_assert([] {
    constexpr Scope prefixed[] = { Scope::Settings, Scope::GoTo, Scope::Blocks, Scope::Find };
    for (std::size_t i = 0; i < std::size(prefixed); ++i)
        for (std::size_t j = i + 1; j < std::size(prefixed); ++j)
            if (prefixFor(prefixed[i]) == prefixFor(prefixed[j]))
                return false;
    return true;
}(),
    "two scopes claim the same prefix, so one of them is unreachable");

// Blocks / go-to carry an intrinsic order (recency, strip order) a blank query
// must preserve; actions / settings sort alphabetically.
constexpr bool preservesOrder(Scope scope) { return scope == Scope::Blocks || scope == Scope::GoTo; }

// Find is the one live scope: each keystroke drives a fresh search rather than
// filtering a fixed list.
constexpr bool isLive(Scope scope) { return scope == Scope::Find; }

// Blocks / find / go-to need a focused terminal; actions / settings are always
// reachable.
constexpr bool requiresTerminal(Scope scope)
{
    switch (scope) {
    case Scope::Actions:
    case Scope::Settings:
        return false;
    case Scope::GoTo:
    case Scope::Blocks:
    case Scope::Find:
        return true;
    }
    return false;
}

} // namespace Ubiquitty::CommandPalette
