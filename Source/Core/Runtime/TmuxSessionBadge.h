// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source/Core/Runtime/TmuxSessionBadge.h — the titlebar pill a tmux -CC window
// wears: which colour a session's pill takes and which label reads on it.
//
// Every window of one session is one unit, so the colour is keyed by the
// session's name, never the window's, and a name nobody has picked a colour
// for still gets one that holds across launches: a hash of the name that no
// process seeds. Both shells paint what these answer, so a session cannot wear
// two colours at once. Pure functions over packed 24-bit sRGB (0xRRGGBB); no
// platform headers.

#include "Core/Themes/Contrast.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Ubiquitty::Runtime {

/// How many fills a session pill chooses among.
inline constexpr int tmuxSessionBadgePaletteCount = 8;

/// The fill at a palette index, the first fill for an index out of range.
///
/// The curated order is red, orange, yellow, green, teal, blue, purple, pink,
/// the order a picker shows them in. A stored pick is an index here, so the
/// order is part of what a saved preference means.
///
/// - Parameter index: A palette index.
/// - Returns: The fill as packed 24-bit sRGB.
[[nodiscard]] inline std::uint32_t tmuxSessionBadgeFill(int index)
{
    static constexpr std::array<std::uint32_t, tmuxSessionBadgePaletteCount> palette = {
        0xDB4D57,
        0xF08F38,
        0xE6C247,
        0x66B866,
        0x45B5A8,
        0x5291E6,
        0x9475DB,
        0xDE75B3,
    };
    if (index < 0 || index >= tmuxSessionBadgePaletteCount)
        index = 0;
    return palette[static_cast<std::size_t>(index)];
}

/// The index a session's pill takes before anyone picks one.
///
/// FNV-1a over the name's bytes, reduced into the palette. The same name
/// answers the same index in every process, which a language's seeded hash
/// does not, so a session keeps its colour across launches.
///
/// - Parameter sessionName: The tmux session's name.
/// - Returns: A palette index.
[[nodiscard]] inline int defaultTmuxSessionBadgeColorIndex(std::string_view sessionName)
{
    std::uint32_t hash = 2166136261u;
    for (const char byte : sessionName) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 16777619u;
    }
    return static_cast<int>(hash % static_cast<std::uint32_t>(tmuxSessionBadgePaletteCount));
}

/// The index a session's pill shows.
///
/// The stored pick when it names a fill, the name's default otherwise —
/// whether nothing was stored (a negative index) or the pick names a fill this
/// palette does not have, so a preference written against a longer palette
/// cannot index past this one.
///
/// - Parameters:
///   - sessionName: The tmux session's name.
///   - storedIndex: The pick read from preferences, negative for none.
/// - Returns: A palette index.
[[nodiscard]] inline int resolveTmuxSessionBadgeColorIndex(std::string_view sessionName, int storedIndex)
{
    if (storedIndex >= 0 && storedIndex < tmuxSessionBadgePaletteCount)
        return storedIndex;
    return defaultTmuxSessionBadgeColorIndex(sessionName);
}

/// The label ink that reads on the fill at an index.
///
/// Near-black or white, whichever carries the greater APCA contrast — the one
/// answer every coloured chip shares (`Themes::Contrast::labelColorOnFill`).
///
/// - Parameter index: A palette index.
/// - Returns: The ink as packed 24-bit sRGB.
[[nodiscard]] inline std::uint32_t tmuxSessionBadgeLabel(int index) { return Themes::Contrast::labelColorOnFill(tmuxSessionBadgeFill(index)); }

} // namespace Ubiquitty::Runtime
