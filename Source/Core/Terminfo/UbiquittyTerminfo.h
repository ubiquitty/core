// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The installer for the terminfo entry: install() writes Core/Terminfo.h's
// sourceText() to a temp file, runs `tic -x -o <directory> <file>`, and
// reports the outcome. The app installs at launch and sets
// TERM=xterm-ubiquitty (falling back to xterm-256color when unresolvable).

namespace Ubiquitty::Terminfo {

// Outcome of an install attempt.
enum class InstallResult {
    Success, // tic compiled and wrote the entry
    TicMissing, // `tic` not found on PATH
    WriteFailed, // temp source file could not be written
    TicFailed, // tic ran but returned nonzero
    SpawnFailed, // the tic process could not be launched
};

// Install the entry into `destinationDirectory` (a terminfo database root such as
// ~/.terminfo). When destinationDirectory is null, the default ~/.terminfo is used.
// Idempotent: re-running overwrites the compiled entry in place. Progress
// and errors are written to stderr.
InstallResult install(const char* destinationDirectory);

// Map an InstallResult to a process exit code: 0 for Success, nonzero
// otherwise.
int exitCodeFor(InstallResult r);

} // namespace Ubiquitty::Terminfo
