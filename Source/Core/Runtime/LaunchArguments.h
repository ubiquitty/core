// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <vector>

// The `-e` / `--` command-line grammar every frontend shares.
//
// `-e <program> [args…]` runs that program in place of the login shell. The
// grammar is small and the whole of it is here rather than in each frontend,
// because the part that is easy to get wrong is not the parse — it is that
// everything after the marker belongs to the CHILD. A frontend that scans its
// own argv afterwards reads the child's tokens as its own, so `-e env --snapshot
// out.png` writes a PNG instead of running env. `terminalArgumentCount` is what
// prevents that, and it only works if every scanner honours it.

namespace Ubiquitty::Runtime {

/// A command line split into the terminal's own arguments and a child command.
struct LaunchRequest {
    /// What the command line asked for.
    enum class Status {
        /// No `-e` and no `--`: the terminal launches its usual login shell.
        None,
        /// A child command was given in `command`.
        Command,
        /// The command line is unusable; `error` says why.
        Error,
    };

    /// What the command line asked for.
    Status status { Status::None };
    /// The child's argv, `command[0]` being the program. Empty unless `status`
    /// is `Command`.
    std::vector<std::string> command;
    /// Whether `--hold` asked the window to outlive the child.
    bool shouldHold {};
    /// Why the command line is unusable. Set only when `status` is `Error`.
    std::string error;
    /// How many leading argv entries the terminal still owns, marker included.
    /// Every other scanner must treat this as its `argc`, or it parses the
    /// child's arguments as the terminal's.
    int terminalArgumentCount {};
};

/// Splits a command line at `-e` or `--`.
///
/// Everything after the marker is the child's, including tokens that look like
/// options: `-e env --title=x` runs `env` with `--title=x`, and the terminal's
/// own `--title` is not set. The marker is not required to be last — it takes
/// the rest of the line, so anything after it was never the terminal's to read.
///
/// A marker with nothing after it is an error rather than a silent fallback to
/// the login shell, because a script whose `$@` expanded to nothing is asking
/// for a program and getting a shell is not what it asked for.
///
/// A bare trailing word is not a command. Two frontends already give argv[1] a
/// positional meaning — a mode name on Linux, a device path on Wscons — so
/// taking bare words would silently redefine their command lines.
///
/// - Parameters:
///   - argc: The count of entries in `argv`.
///   - argv: The command line, `argv[0]` being the terminal itself.
/// - Returns: The split. `terminalArgumentCount` is `argc` when no marker is
///   present.
LaunchRequest parseLaunchArguments(int argc, const char* const* argv);

/// The process's own command line, read by the grammar above.
///
/// The entry point's spelling of `argv`, for a caller that holds it as the
/// process received it — a Swift entry point has no `const` view of it.
///
/// - Parameters:
///   - argc: The count of entries in `argv`.
///   - argv: The process's arguments, borrowed for the call.
/// - Returns: The split; see `parseLaunchArguments`.
[[nodiscard]] inline LaunchRequest parseProcessLaunchArguments(int argc, char** argv) { return parseLaunchArguments(argc, argv); }

/// Whether the session ends when the program it was given does.
///
/// True for a command without `--hold`: a terminal asked to run one program has
/// that program's lifetime, so the window closes and the process exits when it
/// returns. That is what every terminal offering `-e` does, and a caller waiting
/// on the terminal depends on it — a script, a `make` rule, a conformance suite.
///
/// A frontend decides how to end (close an NSWindow, destroy a GTK widget, restore
/// the console); it does not decide WHETHER, or three frontends answer one
/// question three times and drift on the second reading.
///
/// - Parameters:
///   - request: A parsed command line.
/// - Returns: Whether the child's exit ends the session.
[[nodiscard]] bool launchEndsWithChild(const LaunchRequest& request);

/// The window title a child command implies.
///
/// The program's basename, which is what a user reads as the name of what is
/// running — never the joined command line.
///
/// - Parameters:
///   - request: A request whose `status` is `Command`.
/// - Returns: The basename of `request.command[0]`, or an empty string when
///   there is no command.
std::string launchDefaultTitle(const LaunchRequest& request);

} // namespace Ubiquitty::Runtime
