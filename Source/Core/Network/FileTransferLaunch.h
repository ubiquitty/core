// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Portable assembly of the argv for an in-band ZMODEM transfer agent (rz / sz).
// The frontend resolves the binary's absolute path and spawns it; which flags a
// transfer runs under is decided here, next to the detection that starts one.
//
// SECURITY: a receive is driven by the REMOTE end — the sender names the file.
// lrzsz's default already refuses an absolute path, a parent reference, and an
// overwrite. What it still allows is a LEADING DOT, and that is what -R adds
// (with subdirectories). Measured, lrzsz 0.12.20, one file sent as `.hidden`
// between a real sz and rz:
//
//     rz -R    rz: Security Violation, nothing written
//     rz       .hidden written into the download directory
//
// So without the flag a remote sender drops a dotfile wherever the user pointed
// the transfer — .zshrc, .bash_profile — and it runs at their next shell. The
// download directory being user-confirmed does not help: the file the user
// agreed to receive is not the file that arrives.
//
// Passed rather than relied upon for a second reason: the frontend resolves
// whichever of `rz` / `lrz` it finds first, and only lrzsz guarantees the
// restricted default. The flag costs nothing where that default already holds.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Preferences {
struct Values;
}

namespace Ubiquitty::Network {

/// What `[transfer] auto_receive` asks of a detected handshake.
///
/// Detection and consent are separate questions, and this is the first one:
/// whether a handshake the terminal noticed may act at all. Disabled, the
/// transfer menu item still starts a transfer the user asked for — what stops is
/// the stream's ability to raise the subject.
enum class AutoReceive : std::uint8_t {
    Disabled,
    Enabled
};

/// Tracks how many more times a detected handshake may interrupt the user.
///
/// Detection is driven by PTY output, so anything that can print chooses when a
/// prompt appears — and the prompt is app-modal. A stream that repeats the
/// preamble drives one dialog per repeat, each blocking the whole app until it
/// is dismissed, and each dismissal arms the detector for the next. Declining is
/// the signal that bounds it: a user who says no to a transfer they did not ask
/// for should not be asked again by the same stream.
///
/// Kept here rather than in a frontend because it is the same question on every
/// one of them, and because a limit that lives next to the detection cannot be
/// forgotten by the next surface that adds a prompt. A frontend holds one per
/// pane, consults ``mayPrompt()`` before raising a dialog, and reports what the
/// user answered.
class PromptBudget {
public:
    /// Consecutive declines before auto-prompting stops for this session.
    ///
    /// Two, so a mis-click does not disable a feature the user wanted, and a
    /// stream that keeps asking gets exactly one more chance than an accident
    /// needs.
    static constexpr int maxConsecutiveDeclines = 2;

    /// Reports whether a detected handshake may raise a prompt.
    ///
    /// Two reasons say no and both belong here: the user turned automatic
    /// receiving off, or this stream has spent its budget interrupting them. The
    /// setting is a parameter rather than state because a config reload can
    /// change it mid-session, and because a frontend that reads it at
    /// construction would answer with what the user wanted when the pane opened.
    ///
    /// - Parameter autoReceive: what `[transfer] auto_receive` says right now.
    /// - Returns: `true` while a detection may still reach the user.
    [[nodiscard]] bool mayPrompt(AutoReceive autoReceive) const
    {
        return autoReceive == AutoReceive::Enabled && m_consecutiveDeclines < maxConsecutiveDeclines;
    }

    /// Records that the user declined a prompt.
    ///
    /// A cancel counts, and so does dismissing a notice the user could not act
    /// on: both cost the same interruption. Enough of these and the stream stops
    /// being able to ask.
    void recordDecline() { ++m_consecutiveDeclines; }

    /// Records that a transfer was accepted, clearing the count.
    ///
    /// Accepting is what wanting the feature looks like, so earlier declines
    /// stop counting against the next detection.
    void recordAccept() { m_consecutiveDeclines = 0; }

    /// Restores the full budget.
    ///
    /// For a new session, or for a frontend that offers the user an explicit way
    /// back to automatic prompting.
    void reset() { m_consecutiveDeclines = 0; }

    /// Returns the number of declines since the last accept or reset.
    [[nodiscard]] int consecutiveDeclines() const { return m_consecutiveDeclines; }

private:
    int m_consecutiveDeclines = 0;
};

// Which side of the transfer the agent runs.
enum class TransferDirection {
    Receive, // rz — the remote sends, we write
    Send // sz — we send the named file
};

/// Assembles the argv for a ZMODEM transfer agent.
///
/// Returns an empty vector when the launch must not happen: no binary, or a send
/// with no file. The caller treats empty as "do not spawn" rather than spawning
/// a bare binary.
///
/// Layout: `<binary> -b -e -R [file]`
///   `-b` binary mode, no CR/LF translation.
///   `-e` escape all control characters, for an 8-bit-clean pseudoterminal.
///   `-R` restricted: refuse absolute paths and parent references, and refuse to
///        overwrite. Applied to both directions, so the argv carries the policy
///        rather than the direction deciding whether the policy is present. On a
///        send it refuses to read a file named that way, so `runTransferAgent`
///        names the file relative to its own directory.
///
/// - Parameters:
///   - binaryPath: Absolute path the frontend resolved for rz or sz.
///   - direction: Which side runs.
///   - sendFilePath: The file to send; ignored for a receive, required for a send.
/// - Returns: The argv, or empty when the launch must not happen.
[[nodiscard]] inline std::vector<std::string> buildZModemArguments(std::string_view binaryPath, TransferDirection direction, std::string_view sendFilePath = {})
{
    std::vector<std::string> arguments;
    if (binaryPath.empty())
        return arguments;
    if (direction == TransferDirection::Send && sendFilePath.empty())
        return arguments;
    arguments.emplace_back(binaryPath);
    arguments.emplace_back("-b");
    arguments.emplace_back("-e");
    arguments.emplace_back("-R");
    if (direction == TransferDirection::Send)
        arguments.emplace_back(sendFilePath);
    return arguments;
}

/// Resolves the transfer agent binary for `direction`.
///
/// Candidate spellings (`rz`/`lrz` to receive, `sz`/`lsz` to send) are probed
/// across the prefixes a desktop-launched application's minimal PATH misses.
/// The agent is the frontend's business only insofar as the frontend chooses
/// to prompt about it; where it lives is a question every frontend would
/// answer the same way.
///
/// - Parameter direction: Which side runs.
/// - Returns: An absolute path, or an empty string when no agent is installed.
std::string resolveTransferAgentPath(TransferDirection direction);

/// The save directory a receive runs under when the user accepts the default.
///
/// The `[transfer] download_dir` of `configuration` wins; otherwise the
/// platform's default downloads directory applies. Created (with parents) if
/// missing, so the agent's working directory exists before it runs.
///
/// - Parameter configuration: The preferences the receiving session runs under.
/// - Returns: The confirmed directory, or an empty string when neither source
///   yields one or the directory cannot be created.
std::string resolvedTransferDownloadDirectory(const Preferences::Values& configuration);

/// How long a transfer agent may run before it is taken to be wedged.
///
/// An agent can stop inside its own terminal setup and never return, and the
/// run is synchronous with the session's ingest stopped — so without a limit
/// the session never comes back, and on the frontend that is the window's own
/// thread. Measured at roughly one launch in twenty-five (`lsz` stopped in
/// `io_mode`), so this is a case a reader meets rather than a theoretical one.
///
/// An hour, because the limit has to be a wall clock: nothing this side can see
/// separates a wedged agent from a slow one — the terminal reports no pending
/// output queue on Darwin, and an agent waiting on the far end burns no more
/// processor time than a stuck one. So the number is chosen against the
/// transfer instead: ZMODEM over a shell moves tens of kilobytes a second, and
/// an hour of that is far past any file a reader sends down a terminal. A
/// transfer that reaches it has already stopped being one.
///
/// The bound is containment, not a fix for the agent. What it buys is a
/// transfer that reports it did not finish, in place of a window that is gone.
inline constexpr double transferAgentLimitSeconds = 3600.0;

/// Runs a transfer agent against an open terminal stream.
///
/// Receives run `rz` with the agent's working directory set to
/// `downloadDirectoryOrSendFile`; sends run `sz` in the directory of
/// `downloadDirectoryOrSendFile` on the file's name, the form a restricted
/// agent accepts. The descriptor is bound
/// to the agent's standard input and output; the caller keeps ingest stopped
/// and no other reader on the descriptor for the whole call (the agent owns
/// the data stream — see ``PAL::processSpawnWithTerminalAndWait`` for the
/// mode and blocking contract).
///
/// - Parameters:
///   - terminalDescriptor: The open terminal descriptor the agent speaks on.
///   - direction: Which side runs.
///   - downloadDirectoryOrSendFile: The receive directory or the send file.
///   - limitSeconds: How long the agent may run before it is terminated;
///     defaults to ``transferAgentLimitSeconds``. Zero or less waits without
///     limit, which only a caller that can abort some other way should ask for.
/// - Returns: True when the agent ran to a normal, successful exit.
[[nodiscard]] bool runTransferAgent(int terminalDescriptor, TransferDirection direction, std::string_view downloadDirectoryOrSendFile,
    double limitSeconds = transferAgentLimitSeconds);

} // namespace Ubiquitty::Network
