// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Splitting a configured command string into an explicit argv.
//
// There is no shell anywhere on this path: execve receives the elements
// verbatim, so a value that lands in one element stays one argument whatever it
// contains. Callers that substitute into the result must preserve that — see
// Triggers::buildCommandArgv, where the matched text is never re-split.

#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty {

// Split on whitespace (space, tab, and the vertical family), collapsing runs.
// Empty when `command` holds no words. Quoting is not honoured: a config value
// needing an argument with a space belongs in a list, not a quoted string.
[[nodiscard]] std::vector<std::string> splitCommandWords(std::string_view command);

// The program a command string names: its first word, empty when it has none.
[[nodiscard]] std::string commandProgramName(std::string_view command);

} // namespace Ubiquitty
