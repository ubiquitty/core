// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Core/Preferences/Preferences.h"
#include "Forward.h"

// Applying a config's [triggers] and [characterClass] blocks to a Terminal.
//
// Here rather than beside the engine because it needs Values: Core/Triggers is a
// model module and must not reach up into behaviour. Here rather than in each
// frontend so that reading config.toml is sufficient to get the sections applied
// — a frontend cannot opt out of them by omission.

namespace Ubiquitty::Runtime {

/// Why the engine would not take an entry.
enum class TriggerRejection : std::uint8_t {
    /// The pattern did not compile, or the action spec did not parse.
    Malformed,
    /// The engine's table was already full. Nothing is wrong with this entry;
    /// it arrived once the table held maxRuleCount, and a different order would
    /// have admitted it in place of another.
    TableFull,
};

/// One entry the engine refused, named so a caller can say which.
struct RejectedTriggerEntry {
    std::string name;
    TriggerRejection reason {};
};

/// What applying the trigger sections to a terminal actually installed.
///
/// The engine refuses a malformed entry and every entry past its fixed table the
/// same way, by returning false. A refused entry never fires while the config
/// naming it still loads clean, so this is the only evidence that it is missing.
struct TriggerConfigResult {
    int rulesInstalled {};
    int classesInstalled {};

    std::vector<RejectedTriggerEntry> rejectedRules;
    std::vector<RejectedTriggerEntry> rejectedClasses;

    [[nodiscard]] bool complete() const { return rejectedRules.empty() && rejectedClasses.empty(); }
};

/// Replaces the terminal's trigger rules and character classes with the config's.
///
/// Clears first, so a reload installs the fresh set rather than appending to it.
/// An entry with an empty pattern or name is skipped and not reported: it names
/// no instruction, and the parser admits it.
///
/// - Returns: What was installed, and what was refused. Marked `nodiscard`
///   because a dropped result hides the refusal — a refused rule is
///   indistinguishable from one the user never wrote.
[[nodiscard]] TriggerConfigResult applyTriggerConfig(Ubiquitty::Terminal& t, const Preferences::Values& c);

/// A human-readable account of what was refused, or empty when nothing was.
///
/// Shared so every frontend reports the same thing and the only per-frontend
/// decision left is which sink to write it to.
[[nodiscard]] std::string describeTriggerConfig(const TriggerConfigResult&);

} // namespace Ubiquitty::Runtime
