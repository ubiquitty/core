// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include "PAL/Stream.h"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Structured intent for what to run in a terminal session. arguments[0] is the
// executable path; Argument0Style decides whether the child sees a login name.

namespace Ubiquitty::Runtime {

class LaunchCommand;

/// Whether the session opened for a launch ends when that launch's child does.
///
/// A fact of the launch, not of the process. `Runtime::launchEndsWithChild`
/// decides it once from the parsed command line; everything below only
/// carries it, so a window opened later, by ⌘N, a split or a restored pane,
/// runs a different command and is not judged by this one's answer.
///
/// Open rather than closed: it crosses an `@objc` seam as a raw value, and a
/// closed enum traps in Swift on a value it does not know.
enum class SessionLifetime {
    /// The session outlives its child: the window stays up with its scrollback,
    /// because closing it would discard history nobody asked to lose.
    OutlivesChild UBIQUITTY_SWIFT_NAME(outlivesChild),
    /// The session ends with its child: a terminal asked to run one program has
    /// that program's lifetime, and a caller waiting on the process, a script or
    /// a make rule, waits forever otherwise.
    EndsWithChild UBIQUITTY_SWIFT_NAME(endsWithChild),
};

/// Whether a launch's first output waits for a screen the frontend writes.
///
/// Open rather than closed, for the same reason as ``SessionLifetime``.
enum class OpeningScreen {
    /// The child's output parses as it arrives.
    None UBIQUITTY_SWIFT_NAME(none),
    /// The session holds the child's output unread until the frontend hands
    /// over the screen it opens with, so that screen precedes the first
    /// prompt. The child starts as any other launch's does.
    Held UBIQUITTY_SWIFT_NAME(held),
};

/// A launch spelled one argument at a time, for a frontend that cannot hand
/// over a `std::vector<std::string>`.
///
/// The argument vector is the launch's substance, and it arrives from places
/// whose native form is not a C++ container: an `NSArray` from an open-with
/// event, a Swift `[String]` from the window shell. Handing those over one
/// pointer-and-length at a time keeps the assembly — and the ownership of
/// every byte — in Core, where the command is resolved.
class LaunchCommandDraft {
public:
    LaunchCommandDraft() = default;

    /// Appends one argument. The first is the executable path.
    ///
    /// - Parameter argument: The argument, copied.
    void addArgument(const std::string& argument)
    {
        if (!argument.empty())
            m_arguments.push_back(argument);
    }

    /// Sets the directory the child starts in. Empty leaves it to $HOME.
    ///
    /// - Parameter directory: The path, copied.
    void setInitialDirectory(const std::string& directory) { m_initialDirectory = directory; }

    /// Sets how the child's character locale is decided.
    ///
    /// - Parameter policy: The policy the caller resolved.
    void setCharacterLocalePolicy(PAL::CharacterLocalePolicy policy) { m_characterLocalePolicy = policy; }

    /// Sets whether the session ends when this launch's child does.
    ///
    /// - Parameter lifetime: What the caller's command line asked for.
    void setSessionLifetime(SessionLifetime lifetime) { m_sessionLifetime = lifetime; }

    /// Declares a tmux control client; an empty host selects the local machine.
    ///
    /// - Parameter host: The explicit SSH destination, or empty for a local client.
    void setTmuxControlHost(const std::string& host) { m_tmuxControlHost = host; }

    /// Whether any argument has been added.
    ///
    /// - Returns: True while the draft names no executable.
    [[nodiscard]] bool isEmpty() const { return m_arguments.empty(); }

    /// The command this draft describes.
    ///
    /// - Returns: The assembled command; an empty draft answers the login
    ///   shell, so a caller that collected nothing still opens a terminal
    ///   rather than a window with no child.
    [[nodiscard]] LaunchCommand build() const;

private:
    std::vector<std::string> m_arguments;
    std::string m_initialDirectory;
    PAL::CharacterLocalePolicy m_characterLocalePolicy = PAL::CharacterLocalePolicy::Auto;
    SessionLifetime m_sessionLifetime = SessionLifetime::OutlivesChild;
    std::optional<std::string> m_tmuxControlHost;
};

class LaunchCommand {
public:
    LaunchCommand() = default;

    // The user's login shell: $SHELL → the passwd entry → /bin/zsh, plus "-i" when interactive.
    static LaunchCommand loginShell(bool interactive = true);

    /// The command a window's own shell opens under, as a login session.
    ///
    /// Registers the session, so `who` lists it and a PAM session runs.
    /// Degrades to ``loginShell`` where the platform names no login program or
    /// the user's name does not resolve: an unregistered shell beats no window.
    ///
    /// One per window. A split or a restored pane takes ``loginShell`` — they
    /// join a session already registered, and a second would list one user
    /// twice for one login.
    ///
    /// - Parameter initialDirectory: Where the shell starts. Empty means the
    ///   home directory, which the login program reaches by itself; anything
    ///   else costs an extra flag and a re-exec of the shell.
    /// - Returns: The command, with its initial directory already set.
    static LaunchCommand loginSession(const std::string& initialDirectory = {});

    /// The start directory a login session should be opened with, given the
    /// directory it inherits.
    ///
    /// Empty for the home directory as well as for nothing at all: the login
    /// program reaches home by itself, and asking for it costs an extra flag
    /// and a re-exec of the shell to honour. Stated here rather than at each
    /// caller because a frontend that spells it itself opens the one window
    /// that pays for the re-exec.
    ///
    /// - Parameters:
    ///   - inheritedDirectory: Where the pane being inherited from is.
    ///   - home: The user's home directory, which only a platform can resolve.
    /// - Returns: The directory to launch in, or empty to leave it to login.
    ///
    /// Strings rather than views on both sides: the Swift caller hands over
    /// `std.string` values, which the importer will not convert to a view, and
    /// the answer is returned by value because both arguments are temporaries
    /// at the call site.
    [[nodiscard]] static std::string startDirectoryFor(const std::string& inheritedDirectory, const std::string& home);

    // An explicit command (e.g. tmux -CC). arguments[0] is the executable path.
    static LaunchCommand fromArguments(std::vector<std::string> arguments);

    [[nodiscard]] std::span<const std::string> arguments() const { return m_arguments; }
    [[nodiscard]] const std::string& executablePath() const { return m_arguments.front(); }
    [[nodiscard]] bool empty() const { return m_arguments.empty(); }

    // Empty → the PTY backend falls back to $HOME.
    void setInitialDirectory(std::string directory) { m_initialDirectory = std::move(directory); }
    [[nodiscard]] const std::string& initialDirectory() const { return m_initialDirectory; }

    void setCharacterLocalePolicy(PAL::CharacterLocalePolicy policy) { m_characterLocalePolicy = policy; }
    [[nodiscard]] PAL::CharacterLocalePolicy characterLocalePolicy() const { return m_characterLocalePolicy; }

    /// Sets what the child reads as its `argv[0]`.
    ///
    /// `LoginShell` for the shell a terminal opens by itself, whose leading
    /// `-` is what makes an sh-like shell source the login profiles.
    /// `ProgramPath` for a program a caller named, which is not a login shell
    /// and may inspect its own `argv[0]`.
    ///
    /// The default is `LoginShell` for every command rather than being derived
    /// from which factory built it: `fromArguments` already carries
    /// `/usr/bin/login -pf`, so a factory-keyed default would change how macOS
    /// opens its ordinary window.
    ///
    /// - Parameters:
    ///   - style: The spelling to hand the child.
    void setArgument0Style(PAL::Argument0Style style) { m_argument0Style = style; }

    /// The spelling the child will read as its `argv[0]`.
    ///
    /// - Returns: The style set by `setArgument0Style`, or `LoginShell`.
    [[nodiscard]] PAL::Argument0Style argument0Style() const { return m_argument0Style; }

    /// Sets whether the session ends when this command's child does.
    ///
    /// - Parameter lifetime: What the command line asked for.
    void setSessionLifetime(SessionLifetime lifetime) { m_sessionLifetime = lifetime; }

    /// Whether the session ends when this command's child does.
    ///
    /// - Returns: The lifetime set by ``setSessionLifetime``, or
    ///   `OutlivesChild`, the answer for every launch nobody named a program
    ///   for on a command line.
    [[nodiscard]] SessionLifetime sessionLifetime() const { return m_sessionLifetime; }

    /// Sets whether the session holds the child's output for an opening
    /// screen; see ``OpeningScreen``.
    ///
    /// - Parameter openingScreen: Whether to hold.
    void setOpeningScreen(OpeningScreen openingScreen) { m_openingScreen = openingScreen; }

    /// Whether the session holds the child's output for an opening screen.
    ///
    /// - Returns: The value ``setOpeningScreen`` set, or `None`.
    [[nodiscard]] OpeningScreen openingScreen() const { return m_openingScreen; }

    /// Declares the authority of an explicitly launched tmux control client.
    ///
    /// - Parameter host: The explicit SSH destination, or empty for a local client.
    void setTmuxControlHost(std::string host) { m_tmuxControlHost = std::move(host); }

    /// Returns no value for ordinary launches, or the control client's SSH host.
    ///
    /// - Returns: The declared destination; an empty string names a local client.
    [[nodiscard]] const std::optional<std::string>& tmuxControlHost() const { return m_tmuxControlHost; }

    /// Sets the `NAME=value` entries the child's environment takes in place of
    /// any inherited value of the same name.
    ///
    /// - Parameter entries: Unique names; replaces the previous set.
    void setEnvironmentOverrides(std::vector<std::string> entries) { m_environmentOverrides = std::move(entries); }

    /// The entries the child's environment takes over its inherited one.
    ///
    /// - Returns: The set ``setEnvironmentOverrides`` stored, empty by default.
    [[nodiscard]] std::span<const std::string> environmentOverrides() const { return m_environmentOverrides; }

    /// Whether a login-session record is written for this launch's terminal.
    ///
    /// Only ``loginSession`` sets it, and only where the platform records
    /// sessions after the fork rather than inside the argument vector.
    ///
    /// - Returns: What the spawn should do about the session table.
    [[nodiscard]] PAL::SessionRecord sessionRecord() const { return m_sessionRecord; }

    // The whole command line joined with spaces.
    [[nodiscard]] std::string displayName() const;
    // The executable's basename, a login-shell leading '-' stripped (e.g. "zsh").
    [[nodiscard]] std::string displayShellName() const;

    /// The name a title shows before the first process walk resolves.
    ///
    /// Spelled the way the walk will spell it, which is why this is not
    /// ``displayShellName`` on its own: a login shell is exec'd with a leading
    /// dash and the walk reports that dash, so a seed without one makes the
    /// title read `zsh` and then flip to `-zsh` when the walk lands. A program
    /// named on a command line is not exec'd that way and keeps its plain name.
    ///
    /// - Returns: The name, or empty for a command with no arguments.
    [[nodiscard]] std::string titleSeedName() const;

private:
    explicit LaunchCommand(std::vector<std::string> arguments)
        : m_arguments(std::move(arguments))
    {
    }
    std::vector<std::string> m_arguments;
    std::string m_initialDirectory;
    PAL::CharacterLocalePolicy m_characterLocalePolicy = PAL::CharacterLocalePolicy::Auto;
    PAL::Argument0Style m_argument0Style = PAL::Argument0Style::LoginShell;
    PAL::SessionRecord m_sessionRecord = PAL::SessionRecord::None;
    SessionLifetime m_sessionLifetime = SessionLifetime::OutlivesChild;
    OpeningScreen m_openingScreen = OpeningScreen::None;
    std::optional<std::string> m_tmuxControlHost;
    std::vector<std::string> m_environmentOverrides;
};

} // namespace Ubiquitty::Runtime
