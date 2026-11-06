// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/RemoteTarget.h"

// Process.h — portable model + pure functions over one session's PTY processes. The platform gathers
// raw records; this layer picks the foreground process, formats the child/parent chain, and assembles the
// window title. Both the Inspector table and the title consume these — no platform policy duplication.

#include "Base/Types.h"

#include <span>
#include <string>
#include <string_view>

namespace Ubiquitty::PAL::Process {

/// Whether a process leads its terminal session.
enum class SessionLeaderStatus : uint8_t {
    NonLeader,
    Leader
};
/// Whether a process runs with elevated privilege.
enum class UserPrivilege : uint8_t {
    NonRoot,
    Root
};
/// How a process's memory figures are expressed.
enum class DimensionFormat : uint8_t {
    OmitDimensions,
    IncludeDimensions
};

/// The step from a process to the one it runs in a displayed chain
/// ("mc ▸ vi"), which a window's title also takes before what its program
/// says it is doing.
inline constexpr std::string_view chainArrow = " ▸ ";

/// The step from a process to the one it was started from in a displayed
/// chain ("vi ◂ claude"): the process holding the terminal leads either way.
inline constexpr std::string_view chainParentArrow = " ◂ ";

/// What a composed title puts between two segments.
///
/// The macOS window title is the one surface that renders the separator itself:
/// its titlebar draws the em dash as its own component and spaces it with kern,
/// so the joined string carries a bare dash. Every other surface — tabs, panes,
/// and the Linux window and tab labels — is plain text and carries the spaces.
enum class TitleSeparator : uint8_t {
    Bare,
    Spaced
};

/// A process's scheduler state, as the OS reports it.
enum class State : uint8_t {
    Unknown = 0,
    Running,
    Sleeping,
    Stopped,
    Zombie
};

struct Record {
    uint32_t processID = 0;
    uint32_t parentProcessID = 0;
    uint32_t processGroupID = 0; // killpg target
    std::string name {}; // executable basename, e.g. "vim", "claude"
    std::string commandLine {}; // full command line, argv joined with spaces
    bool isForeground = false; // in the tty's foreground process group
    bool isSessionLeader = false; // the PTY's session-leader shell
    std::string user {}; // login name; may be empty
    std::string tty {}; // replica tty basename, e.g. "ttys003"
    State state = State::Unknown;

    // Field by field, so a reader keyed on the sample can tell a walk that
    // found the same processes from one that found a job reaped, a program
    // stopped, or the tree in a different order.
    bool operator==(const Record&) const = default;
};

/// One member of the terminal's foreground process group, as the rule that
/// tells a shell's own work from a job reads it.
///
/// The platform reports these facts; ``Session::foregroundOwner`` decides over
/// them.
struct GroupMember {
    uint32_t processID = 0;
    uint32_t parentProcessID = 0;
    /// Whether the process has exec'd since it was forked. A fork that has not
    /// is still running its parent's program: a shell's subshell runs shell code.
    bool hasExecuted = false;
    /// Whether SIGINT is ignored. A shell starts an asynchronous list this way
    /// when job control is off, as it is inside a subshell, and the disposition
    /// survives the exec.
    bool isInterruptIgnored = false;
    /// Whether the process has ended and waits to be reaped. A process whose
    /// first thread has ended while another runs on has not.
    bool isZombie = false;
    /// When the process was forked, in a unit compared only with another
    /// member's on the same platform: Darwin's microseconds since the epoch,
    /// Linux's clock ticks since boot.
    uint64_t startTime = 0;
};

bool isLoginShell(std::string_view name, SessionLeaderStatus leaderStatus);
bool safeToClose(std::string_view name);
std::string_view marker(std::string_view name, SessionLeaderStatus leaderStatus, UserPrivilege privilege);

uint32_t foregroundLeafProcessID(std::span<const Record> records);

// The PTY's session-leader record, or nullptr when the walk found none. Its
// .tty feeds the tty-name segment. NOTE: on macOS this is the login(1)
// wrapper, not the user's shell — shell-identity judgments use
// effectiveShellLeader below.
const Record* sessionLeader(std::span<const Record> records);

// The record that is the user's shell: the session leader, or — when the
// leader is the login(1) wrapper — its login-shell child. Feeds the
// shell-command title segment and the chain's parent-suppression.
const Record* effectiveShellLeader(std::span<const Record> records);

// The shell-command segment's spelling of a shell record: the argv0 with the
// login dash stripped ("-zsh" → "zsh"). The chain keeps the raw argv0.
std::string_view shellCommandDisplayName(const Record& record);

/// The name to show for a process, which is the script when it runs one.
///
/// `python3 /srv/deploy.py` reads as `deploy.py`: the interpreter is how the
/// script runs, not what it is, and a reader who wrote `deploy.py` means that.
/// The same holds when the shebang runs it — the kernel reports the interpreter
/// and argv[0] holds the script, so the script wins there too.
///
/// A name is not an argument, so this is independent of whether arguments are
/// shown. An interpreter with no script — `-c`, `-e`, or nothing at all — keeps
/// its own name, and so does every program that is not an interpreter.
///
/// - Parameters:
///   - name: The executable basename the kernel reports.
///   - commandLine: argv joined by spaces, empty when unknown.
/// - Returns: A view into one of the arguments, or `name` unchanged. Borrowed
///   from the caller's storage, so it lives exactly as long as they do.
[[nodiscard]] std::string_view scriptDisplayName(std::string_view name, std::string_view commandLine);

/// Whether the chain's leaf shows its full argv tail ([title]
/// *_process_arguments) or only the smart name (the default: a tail is
/// appended only when small enough to read as part of the name).
enum class ChainArguments : uint8_t {
    OmitArguments,
    IncludeArguments
};

std::string formatChain(std::span<const Record> records, ChainArguments arguments);

/// Where this session's foreground process leads, if anywhere.
///
/// Searches from the foreground leaf through its ancestors up to the shell,
/// so transport helpers retain their connection owner's destination.
/// A local foreground chain answers a target whose kind is `RemoteKind::None`.
///
/// - Parameter records: The walked process subtree.
/// - Returns: The foreground chain's destination, or a `None` target.
[[nodiscard]] RemoteTarget remoteTargetForChain(std::span<const Record> records);
inline std::string formatChain(std::span<const Record> records) { return formatChain(records, ChainArguments::OmitArguments); }

// Whether the title poll should re-walk the full process table (the costly part) rather than reuse the
// cached chain. Re-walks on a foreground-group/name change, or while more than one process shares the
// foreground group — a `$(...)` transient can run in its own job pgid with the shell backgrounded, and
// gating only on the leader being foreground would latch the title on the transient after it exits.
// `treeChanged` is the kqueue tracker's fork/exec/exit edge. Without it the gate
// watches only the foreground group and the foreground NAME, and neither moves
// when a program that already holds the terminal spawns a child of its own —
// mc parking its subshell changes nothing either of them can see, so the cached
// chain stood at "mc" for the life of the session.
bool shouldRewalkForegroundChain(uint32_t foregroundGroup, uint32_t chainProcessGroup, bool hasCachedChain, bool processNameChanged, int chainForegroundCount, bool treeChanged);

/// Whether the foreground of this walk is tmux in control mode.
///
/// A frontend that speaks the control protocol attaches on this, so the test
/// is strict: the `-CC` flag on a FOREGROUND record's command line. A plain
/// `tmux a` shares the name and not the protocol, and a frontend that
/// attached to it would type its kickstart command into the user's shell.
///
/// - Parameter records: The walk to judge.
/// - Returns: Whether control-mode tmux holds the terminal.
bool isForegroundControlModeTmux(std::span<const Record> records);

/// This process's own image name.
///
/// A child that has forked but not yet exec'd still reads as it, so a title
/// sampled in that window would name the terminal after itself. The sampler
/// compares against this and shows the launch seed instead.
///
/// - Returns: The name, or empty where the platform does not publish one.
std::string_view hostProcessName();
std::string formatTitle(std::string_view lead, std::string_view osc, std::string_view chain, int columns, int rows, DimensionFormat dimensions, TitleSeparator separator);

} // namespace Ubiquitty::PAL::Process
