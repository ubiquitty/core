// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/Preferences.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Unicode/Normalize.h"

namespace Ubiquitty::Runtime {

/// The form search normalizes both the needle and the haystack to.
///
/// `[unicode] normalization`, resolved: an unset key and a name the parser does
/// not know both mean None, the search over text as stored.
///
/// - Parameter config: The config fold.
/// - Returns: The form to normalize to.
[[nodiscard]] inline Unicode::NormalizationForm searchNormalizationForm(const Preferences::Values& config)
{
    if (!config.normalization)
        return Unicode::NormalizationForm::None;
    return Unicode::normalizationFormFromName(*config.normalization).value_or(Unicode::NormalizationForm::None);
}

/// Applies the preferences that hold for the whole process rather than for
/// one terminal.
///
/// A terminal's own preferences are seeded when it is built and re-applied by
/// ``TerminalSession/applyTerminalPreferences``; these have no terminal to ride,
/// so a frontend calls this where it loads or reloads its configuration. Today
/// that is the search normalizer, which every find in every window reads.
///
/// Header-only: the daemon's link set trims the search and the normalizer,
/// and an inline function nobody there calls asks it for neither.
///
/// - Parameter config: The config fold as it stands.
inline void applyProcessPreferences(const Preferences::Values& config)
{
    Screen::ScrollSearch::setNormalizationForm(searchNormalizationForm(config));
}

} // namespace Ubiquitty::Runtime
