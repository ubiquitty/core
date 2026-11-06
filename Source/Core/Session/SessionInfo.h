// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Portable session facts and process presentation shared by frontend inspectors.
// The snapshot consumes the process sample already used by session titles.

#include "Core/TermPalette.h"
#include "PAL/Process.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty {
struct Terminal;
namespace PAL {
class Stream;
}

namespace Session {

// Upper bound on processes captured into SessionInfo::processes. Kept in one
// place so gather scratch capacity and frontend expectations agree.
constexpr int maxProcesses = 256;

/// The portable snapshot of one session, as the Inspector displays it.
struct SessionInfo {
    int columns = 0;
    int rows = 0;
    uint8_t backgroundRed = 0;
    uint8_t backgroundGreen = 0;
    uint8_t backgroundBlue = 0;
    uint64_t paletteGeneration = 0;
    std::array<PaletteRGB, 19> colors {};
    std::string directory; // foreground current working directory (portable)
    std::string tty; // replica tty basename, e.g. "ttys016" (frontend prefixes /dev/)
    std::string command; // launch command line, if portably available (else empty)
    /// Concealment is applied before this value leaves the model lock.
    std::string answerbackDisplay;
    bool canConfigureAnswerback = false;
    std::string shell; // argv0 basename of command (else empty)
    std::vector<PAL::Process::Record> processes; // gathered records, in parent-before-child order
    int foregroundIndex = -1; // index into processes of the foreground process, or -1

    /// Returns an ANSI color, followed by the default foreground, background, and cursor.
    ///
    /// - Parameter index: A palette position from zero through eighteen.
    /// - Returns: The live terminal color at that position.
    [[nodiscard]] PaletteRGB colorAt(unsigned index) const
    {
        assert(index < colors.size());
        return colors[index];
    }
};

/// Captures geometry, palette, and cached process facts for a session inspector.
///
/// The caller holds the terminal's model lock. No process walk is started.
/// A null stream supports terminals owned by another backend.
///
/// - Parameters:
///   - terminal: The terminal whose state is being inspected.
///   - stream: The local stream, when the caller owns one.
///   - records: The process sample shared with session titles.
/// - Returns: An independent snapshot of the inspected state.
SessionInfo captureSessionInfo(const Terminal* terminal, PAL::Stream* stream,
    const std::vector<PAL::Process::Record>& records);

/// Signals a process group selected from the session's cached process sample.
///
/// - Parameters:
///   - stream: The stream that owns the selected process group.
///   - records: The session's current process sample.
///   - processID: The selected process identity.
///   - signal: The requested native signal number.
/// - Returns: Whether the selected group received the signal.
bool signalSessionProcess(PAL::Stream* stream, const std::vector<PAL::Process::Record>& records,
    uint32_t processID, int signal);

/// Fills `out` from the live session.
///
/// Columns and rows plus the default background RGB come from the terminal's
/// palette; directory and tty from the stream's process facts. Processes are
/// ordered by process ancestry, with `foregroundIndex` set to the first
/// foreground row, or -1 when none.
///
/// `command` and `shell` are left empty: the launch command line is not
/// portably reachable through `PAL::Stream` because it lives in the frontend,
/// so the frontend supplies those two. A caller reading them as authoritative
/// gets an empty string rather than a wrong one.
///
/// - Parameters:
///   - term: The terminal whose geometry and palette to read.
///   - stream: The pty stream the process facts come from.
///   - out: Destination, overwritten.
void gatherSessionInfo(const struct Ubiquitty::Terminal& term, Ubiquitty::PAL::Stream& stream, SessionInfo& out);

/// Fills `out` from the live session, using a subtree already walked.
///
/// Same as the three-argument form except that the process table is not read
/// again: the caller passes the walk it already has. A second walk answers from
/// a second instant, so a panel listing the subtree and a title naming its leaf
/// can disagree about what is running.
///
/// - Parameters:
///   - term: The terminal whose geometry and palette to read.
///   - stream: The pty stream the directory and tty come from.
///   - out: The info to fill.
///   - walked: The process sample, in any enumeration order.
void gatherSessionInfo(const struct Ubiquitty::Terminal& term, Ubiquitty::PAL::Stream& stream, SessionInfo& out,
    std::span<const PAL::Process::Record> walked);

/// Puts an already-gathered subtree into `out`, ordered and indexed.
///
/// The ordering rule and the foreground summary are the same ones
/// `gatherSessionInfo` applies, kept here so a caller holding the walk does not
/// repeat it. A second walk answers from a second instant: the list and
/// the title can then disagree about what is running, and a reader with a walk
/// of its own has no edge to wake it and falls back to a timer.
///
/// - Parameters:
///   - out: The info to fill; its `processes` and `foregroundIndex` are replaced.
///   - walked: The process sample, in any enumeration order.
void adoptProcesses(SessionInfo& out, std::span<const PAL::Process::Record> walked);

/// The display and copy values for one process row.
struct ProcessPresentation {
    uint32_t processID = 0;
    uint32_t parentProcessID = 0;
    uint32_t processGroupID = 0;
    std::string marker;
    std::string rawName;
    std::string name;
    std::string arguments;
    std::string smartName;
    std::string smartArguments;
    std::string user;
    bool isForeground = false;
    bool isSessionLeader = false;
    bool isZombie = false;
    bool isSecondary = false;
};

/// Formats a process for display without changing its identity or copy text.
///
/// - Parameter record: The process from the session's shared sample.
/// - Returns: Names, full command text, owner, and row status.
[[nodiscard]] ProcessPresentation processPresentation(const PAL::Process::Record& record);

} // namespace Session
} // namespace Ubiquitty
