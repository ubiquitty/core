// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Installing shell integration: what an install does to each of a home
// directory's shell startup files.
//
// The integration itself is the scripts under Resources/shell-integration, which
// every shell Ubiquitty starts can find through $UBIQUITTY_INTEGRATION_DIR. An
// install adds one managed block to a startup file, and the block loads the
// script for that file's shell when the variable is set, so a shell some other
// terminal starts is left as it was. Everything the integration does lives in
// the scripts; a later release changes what they do without touching the
// reader's files again.
//
// A marker line opens and closes the managed block, which is what lets a later
// install rewrite the body in place rather than append a second copy, and what
// lets a reader remove it by hand. A file that already loads the script without
// the markers is left alone, as is one whose block has lost its closing marker.
//
// The edit is decided here over the file's bytes; reading and writing the file
// is the frontend's, because only the frontend can tell a file that is absent
// from one it may not read.
//
// Platform-free (check-boundaries.py: "Source/Core/App names no native
// framework").

#include "Base/Compiler.h"
#include "Base/SwiftInterop.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::App::ShellIntegrationInstall {

/// The line that opens the managed block.
inline constexpr std::string_view startMarker = "# >>> Ubiquitty shell integration >>>";

/// The line that closes the managed block.
inline constexpr std::string_view endMarker = "# <<< Ubiquitty shell integration <<<";

/// A shell with an integration script.
enum class UBIQUITTY_CLOSED_ENUM Shell : std::uint8_t {
    Zsh UBIQUITTY_SWIFT_NAME(zsh),
    Bash UBIQUITTY_SWIFT_NAME(bash),
    Fish UBIQUITTY_SWIFT_NAME(fish),
};

/// The integration script's file name for a shell, as it sits in
/// $UBIQUITTY_INTEGRATION_DIR.
///
/// - Parameter shell: The shell.
/// - Returns: `zsh.sh`, `bash.sh` or `fish.fish`.
[[nodiscard]] std::string scriptName(Shell shell);

/// The managed block for a shell's startup file, both marker lines included,
/// with no trailing newline.
///
/// - Parameter shell: The shell whose syntax the block is written in.
/// - Returns: The block this build installs.
[[nodiscard]] std::string managedBlock(Shell shell);

/// One startup file an install visits.
struct StartupFile {
    /// The file's path, relative to the home directory.
    std::string relativePath;
    /// The shell that reads it.
    Shell shell = Shell::Zsh;
    /// Whether an install creates the file when it does not exist. Only a file
    /// whose absence no other file stands in for is created: a new
    /// `.bash_profile` would stop a login bash from reading `.profile`, and a
    /// new fish configuration would clutter a home where fish is not used.
    bool isCreatedWhenAbsent = false;
};

/// Startup files, in the order an install visits them.
using StartupFiles = std::vector<StartupFile>;

/// The startup files an install visits, in the order it visits them.
///
/// - Returns: `.zshrc`, `.bashrc`, `.bash_profile` and
///   `.config/fish/config.fish`; only `.zshrc` and `.bashrc` are created when
///   absent.
[[nodiscard]] StartupFiles startupFiles();

/// What installing does to one startup file.
enum class UBIQUITTY_CLOSED_ENUM Edit : std::uint8_t {
    /// The file does not exist and is one an install creates; it is created
    /// holding the block.
    Create UBIQUITTY_SWIFT_NAME(create),
    /// The file carries no block; the block is appended after a blank line.
    Append UBIQUITTY_SWIFT_NAME(append),
    /// The file carries a managed block with an older body, which is replaced
    /// in place with everything around it kept byte for byte.
    Upgrade UBIQUITTY_SWIFT_NAME(upgrade),
    /// The file already carries this build's block; nothing is written.
    AlreadyCurrent UBIQUITTY_SWIFT_NAME(alreadyCurrent),
    /// The file loads the integration script without the markers, as the
    /// guide shows; it is left alone.
    ReaderWritten UBIQUITTY_SWIFT_NAME(readerWritten),
    /// The file carries an opening marker with no closing one after it; it is
    /// left alone rather than guessed at.
    Malformed UBIQUITTY_SWIFT_NAME(malformed),
    /// The file does not exist and is not one an install creates.
    LeftAbsent UBIQUITTY_SWIFT_NAME(leftAbsent),
};

/// An edit, and the contents the file holds after it.
struct PlannedEdit {
    Edit edit = Edit::AlreadyCurrent;
    /// The whole file after the edit. Equal to the input for every edit that
    /// writes nothing, and empty for a file left absent.
    std::string contents;
};

/// Decides the edit for a startup file that exists.
///
/// - Parameters:
///   - file: The startup file.
///   - contents: The file's bytes.
/// - Returns: The edit, and the file as it stands after it.
[[nodiscard]] PlannedEdit planEdit(const StartupFile& file, std::string_view contents);

/// Decides the edit for a startup file that does not exist.
///
/// - Parameter file: The startup file.
/// - Returns: A creation holding the block, or the file left absent.
[[nodiscard]] PlannedEdit planAbsentFile(const StartupFile& file);

/// One file an install writes, as the question before it names the file.
struct PlannedChange {
    /// The file's path, relative to the home directory.
    std::string relativePath;
    /// Whether the install creates it.
    bool isNew = false;
};

/// The files an install writes, in the order it writes them.
using PlannedChanges = std::vector<PlannedChange>;

/// What the question before an install says will happen, naming every file it
/// writes and which of them it creates.
///
/// The question stands in for the reader's consent to edit their startup
/// files, so it names the files the install would actually write rather than a
/// fixed list.
///
/// - Parameter changes: The files the install writes.
/// - Returns: The question's informative text.
[[nodiscard]] std::string consentDetail(const PlannedChanges& changes);

} // namespace Ubiquitty::App::ShellIntegrationInstall
