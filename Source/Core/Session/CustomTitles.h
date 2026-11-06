// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/CustomTitles.h — the titles a person puts on a window
// and on a tab, and the one spelling of what a typed title becomes.
//
// A window and a tab each carry at most one custom title, and each is either
// set or automatic; empty is how automatic is spelled, so clearing a field and
// never having typed in it are the same fact. Where a title is held is the
// owner's: the workspace model holds a local tab's, the host holds a hosted
// tab's and sends it over Sync, and a frontend holds what no model carries.
// What a held title does to the composed title is `resolveTitleNames`'s.

#pragma once

#include "Core/Session/NameLimits.h"
#include "Core/Unicode/UTF8Repair.h"
#include <cstddef>
#include <string>
#include <string_view>

namespace Ubiquitty::Session {

/// The most UTF-8 bytes a custom title keeps: the bound a workspace name has,
/// since a tab's title travels in the same records.
inline constexpr std::size_t maxCustomTitleBytes = maxWorkspaceNameBytes;

/// The custom titles one window and its tab carry.
///
/// Each is a whole name or empty for automatic. Read by every surface that
/// shows or edits a title, so the popover, the rename sheet, the inspector, a
/// script and a restore cannot each answer with a different copy.
struct CustomTitles {
    /// The window's name: fills the window's title in place of what the
    /// running program would name it, and the tab's where the tab has none.
    std::string window;
    /// The tab's name: fills the tab's label in place of what the running
    /// program, or the window's name, would put there.
    std::string tab;
};

/// What a typed title is kept as.
///
/// Well-formed UTF-8 with no control characters: a line break or a tab typed
/// or pasted into the field reads as a space, any other control is dropped,
/// and spaces at either end are trimmed, so a name of spaces is no name.
/// Bounded at ``maxCustomTitleBytes`` on a character boundary.
///
/// - Parameter typed: The text as entered, from any surface or a peer.
/// - Returns: The title, empty for automatic.
[[nodiscard]] inline std::string normalizedCustomTitle(std::string_view typed)
{
    std::string repaired(typed.substr(0, maxCustomTitleBytes + 4));
    Unicode::repairUTF8(repaired);
    std::string kept;
    kept.reserve(repaired.size());
    for (std::size_t index = 0; index < repaired.size(); ++index) {
        const auto byte = static_cast<unsigned char>(repaired[index]);
        if (byte == '\t' || byte == '\n' || byte == '\r') {
            kept.push_back(' ');
            continue;
        }
        if (byte < 0x20 || byte == 0x7F)
            continue;
        // C1 controls, U+0080 to U+009F, are C2 80 to C2 9F in UTF-8.
        if (byte == 0xC2 && index + 1 < repaired.size()) {
            const auto next = static_cast<unsigned char>(repaired[index + 1]);
            if (next >= 0x80 && next <= 0x9F) {
                ++index;
                continue;
            }
        }
        kept.push_back(static_cast<char>(byte));
    }
    if (kept.size() > maxCustomTitleBytes) {
        std::size_t cut = maxCustomTitleBytes;
        while (cut > 0 && (static_cast<unsigned char>(kept[cut]) & 0xC0) == 0x80)
            --cut;
        kept.resize(cut);
    }
    const std::size_t first = kept.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    const std::size_t last = kept.find_last_not_of(' ');
    return kept.substr(first, last - first + 1);
}

/// ``normalizedCustomTitle`` for a caller that holds an owned string, which is
/// how Swift hands one over: it converts a `String` to `std::string` and will
/// not convert either to a view.
///
/// - Parameter typed: The text as entered.
/// - Returns: The title, empty for automatic.
[[nodiscard]] inline std::string customTitleFromTyped(const std::string& typed)
{
    return normalizedCustomTitle(typed);
}

} // namespace Ubiquitty::Session
