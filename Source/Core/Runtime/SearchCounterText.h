// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Runtime/SearchCounterText.h — what a find bar's counter says.
//
// Find bars share the wording for scan progress, the active match's place,
// an empty answer, and an invalid pattern.
//

#include <cstddef>
#include <string>

namespace Ubiquitty::Runtime {

/// The counter a find bar shows after a search.
///
/// - Parameters:
///   - totalMatches: The matches found; zero for none.
///   - activeIndex: The active match, from zero; negative when none is.
///   - isPatternInvalid: Whether the pattern failed to compile.
///   - isComplete: Whether the scan's count is final.
/// - Returns: The count and active place, progress, or the pattern error.
[[nodiscard]] inline std::string searchCounterText(std::ptrdiff_t totalMatches, std::ptrdiff_t activeIndex, bool isPatternInvalid, bool isComplete = true) // bool-ok: search status data
{
    if (isPatternInvalid)
        return "Invalid pattern";
    if (totalMatches <= 0)
        return isComplete ? "No matches" : "Searching…";
    const std::string count = std::to_string(totalMatches) + (isComplete ? "" : "+");
    if (activeIndex < 0 || activeIndex >= totalMatches)
        return count + (totalMatches == 1 && isComplete ? " match" : " matches");
    return std::to_string(activeIndex + 1) + " of " + count;
}

} // namespace Ubiquitty::Runtime
