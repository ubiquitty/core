// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

#include "Base/Types.h"
#include "Core/CaseSensitivity.h"
#include "Forward.h"
#include <optional>
#include <span>

// The Terminal-level trigger surface. The engine is Core/Triggers.{h,cpp}; these
// entry points own its lifetime, building it on the first rule and forwarding.
// A Terminal with no rules never allocates one.

namespace Ubiquitty::Triggers {

/// A matched byte range, both ends inclusive.
///
/// Returned rather than written through two out-parameters: an absent match is
/// then the empty optional instead of a `false` a caller can ignore while
/// reading the span anyway, and the pair cannot be half-written.
struct MatchSpan {
    int begin = 0;
    int end = 0;
};

/// Adds a trigger rule that fires `action` on output matching `pattern`.
///
/// - Parameters:
///   - action: An `Ubiquitty::Triggers::Action`, widened so callers holding
///     configuration text need not name the enumeration.
///   - arg: The action's payload — a title, a colour, a command.
/// - Returns: False when the pattern does not compile or the rule table is full.
bool addRule(Ubiquitty::Terminal& t, const char* pattern, uint8_t action, const char* arg, Ubiquitty::CaseSensitivity caseSensitivity);
/// actionSpec is the textual form ("highlight:red"), parsed here so callers that
/// only have configuration text do not need the Action enum.
bool addRuleSpec(Ubiquitty::Terminal* t, const char* pattern, const char* actionSpec, Ubiquitty::CaseSensitivity caseSensitivity);
/// Defines a named character class that rule patterns can reference by name.
///
/// - Parameters:
///   - t: The terminal whose trigger engine gains the class.
///   - name: What a pattern refers to the class by.
///   - pattern: The class body, in the trigger engine's pattern syntax.
///   - caseSensitivity: Whether the class matches without regard to case.
/// - Returns: False when the pattern does not compile or the name is already taken.
bool addCharacterClass(Ubiquitty::Terminal& t, const char* name, const char* pattern, Ubiquitty::CaseSensitivity caseSensitivity);
void clear(Ubiquitty::Terminal* t);

[[nodiscard]] int ruleCount(const Ubiquitty::Terminal* t);
[[nodiscard]] int characterClassCount(const Ubiquitty::Terminal* t);

/// Widens a selection to the trigger-defined word class around a column.
///
/// - Parameters:
///   - t: The terminal whose engine holds the character classes.
///   - utf8: The line's bytes.
///   - column: The column the widened span must cover.
/// - Returns: The span, or nothing when no class matched — the caller's
///   default word rule runs in that case.
std::optional<MatchSpan> expandWordSelection(const Ubiquitty::Terminal* t, std::string_view utf8, int column);

[[nodiscard]] int highlightCount(const Ubiquitty::Terminal* t);
bool highlightAt(const Ubiquitty::Terminal* t, int index, int* absoluteRow, int* column0, int* column1);

} // namespace Ubiquitty::Triggers
