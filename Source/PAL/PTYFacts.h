// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/Stream.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>

// OS-specific process-introspection seam behind the portable PTYStream. The PTY
// transport is portable POSIX; only "what is this pid" introspection differs per
// OS. macOS implements it with libproc / KERN_PROCARGS2, other POSIX hosts with
// /proc. All take a target process id and return empty / 0 on a miss.

namespace Ubiquitty::PAL {

/// The name of the program a process runs (e.g. "vim", "ssh").
///
/// Darwin's is the basename of the executable's path. Linux's is the name the
/// program was run by, argv[0]'s basename, which a program that renames
/// itself (`prctl(PR_SET_NAME)`) keeps and a symbolic link it was run
/// through gives: `vi` through a link to `vim.basic` reads `vi`.
///
/// - Parameter processID: The process to read.
/// - Returns: The name, or empty when it does not read: on Darwin for a
///   process whose file was deleted or that is exiting, on Linux for one that
///   is exiting or is in exec.
[[nodiscard]] std::string processExecutableName(ProcessID processID);

/// The name the kernel keeps for a process. It reads for as long as the
/// process exists, where the executable's path need not: a process that is
/// exiting has let go of its file, and one whose file was deleted has none to
/// read.
///
/// Darwin's is p_comm, which exec copies from the name of the file it ran,
/// past any symbolic link, keeping 16 bytes. Linux's is comm, which exec takes
/// from the last part of the path the program was run by, a symbolic link's
/// own name among them, keeping 15 bytes, and which the process can rename
/// (`prctl(PR_SET_NAME)`); it need not be the executable's file name.
///
/// - Parameter processID: The process to read.
/// - Returns: The name, or empty when the process is gone.
[[nodiscard]] std::string processCommandName(ProcessID processID);

/// The name a reading of the terminal's foreground gives a process: its
/// executable's (`processExecutableName`), or once that no longer reads, as
/// for one that is exiting or whose file was deleted, the kernel's
/// (`processCommandName`).
///
/// On Linux a process in exec names nothing: it reads as exec'd a moment
/// before the kernel renames it, and comm still names the program it
/// replaces then.
///
/// - Parameter processID: The process to read.
/// - Returns: The name, or empty for a process that has gone or is in exec.
[[nodiscard]] std::string processForegroundName(ProcessID processID);

// The process's current directory; empty on miss.
std::string processCurrentDirectory(ProcessID);

/// Kernel birth identity; opaque values compared only on the same platform.
struct ProcessBirthIdentity {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    friend bool operator==(const ProcessBirthIdentity&, const ProcessBirthIdentity&) = default;
};

/// Process identity and ownership facts from one kernel snapshot.
struct PTYProcessSnapshot {
    ProcessID parentProcessID = noProcess;
    ProcessID processGroupID = noProcess;
    ProcessBirthIdentity birth;
};

/// Pins the inactive terminal endpoint and its exact session-leader birth.
/// No descriptor or process ownership transfers through this value.
struct PTYAdoptionIdentity {
    ProcessID sessionLeader = noProcess;
    ProcessBirthIdentity birth;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    friend bool operator==(const PTYAdoptionIdentity&, const PTYAdoptionIdentity&) = default;
};

/// Captures a nonblocking PTY's device and exact live session-leader ancestry.
/// Performs no flag changes, reads, writes, resize, signaling, or reaping.
/// - Parameters:
///   - master: The borrowed master, retained by the caller throughout the check.
///   - owner: The freshly authenticated physical owner process.
/// - Returns: Exact current identity, or refusal without changing the endpoint.
[[nodiscard]] std::optional<PTYAdoptionIdentity> capturePTYAdoptionIdentity(int master, ProcessID owner);

/// Rechecks both the original endpoint identity and current owner ancestry.
/// - Parameters:
///   - master: The same privately retained inactive master.
///   - owner: The freshly authenticated physical owner process.
///   - expected: The identity captured during Prepare.
/// - Returns: Whether all ownership and nonblocking facts still match.
[[nodiscard]] bool validatePTYAdoptionIdentity(int master, ProcessID owner, const PTYAdoptionIdentity& expected);

/// Reads a live process's identity without waiting for or reaping it.
///
/// - Parameter processID: The process to inspect.
/// - Returns: Its snapshot, or empty for exited, unreadable or unsupported input.
[[nodiscard]] std::optional<PTYProcessSnapshot> ptyProcessSnapshot(ProcessID processID);

/// The facts one process contributes to the foreground-owner rule.
///
/// - Parameter processID: The process to read.
/// - Returns: Its facts, or empty when it is gone or unreadable.
std::optional<PAL::Process::GroupMember> processGroupMember(ProcessID processID);

/// The members of a process group, for the foreground-owner rule.
///
/// - Parameters:
///   - group: The process group to list.
///   - outputBuffer: Receives the group's members, as many as it holds; a
///     group larger than it fills it.
/// - Returns: How many members the group has, or for a group the buffer could
///   not hold, the buffer's size plus one; empty when the process table is
///   unreadable. The read stops there, so its cost is bounded by the buffer.
std::optional<std::size_t> processGroupMembers(ProcessID group, std::span<PAL::Process::GroupMember> outputBuffer);

// The first child process of `parent`, or 0 on miss / no children. Used to see
// through a `login` wrapper to the real shell it forks (login has one child).
ProcessID firstChildProcess(ProcessID parent);

// Opaque per-session process-tree tracker. macOS maintains `leader`'s live
// descendants off-thread via kqueue NOTE_TRACK, so gatherSubtree inspects
// a handful of pids instead of scanning every process (the title-poll stall).
// start returns nullptr where unsupported (Host build) — gatherSubtree
// then falls back to the full scan. stop joins the watcher thread; both are safe
// on nullptr.
/// Identifies the process whose exit describes the terminal session.
enum class ProcessExitSource {
    DirectChild,
    LoginShell
};

class ProcessTracker;
ProcessTracker* startProcessTracker(ProcessID leader, ProcessExitSource exitSource = ProcessExitSource::DirectChild);
void stopProcessTracker(ProcessTracker*);

/// Returns the observed exit of the shell beneath a login wrapper.
///
/// - Returns: An unknown exit until the kernel reports the shell status.
ChildExit processTrackerLoginShellExit(ProcessTracker*);

// Tree-change notifier: `notify(context)` fires on the watcher's OWN thread
// before processing a kqueue batch carrying any fork/exec/exit edge, so a frontend can
// refresh process-derived title state on the edge instead of a poll tick.
// The notifier must only wake another queue (merge into a dispatch source);
// it runs off the frontend's threads and never touches terminal state.
// Returns whether descendant discovery is observed. A false result retains
// existing kernel watches but suppresses fallback subtree enumeration; the
// observer refreshes and offers current descendants through observeProcesses
// when observation resumes.
// `releaseContext` is the context's ownership hand-off: the tracker calls it
// exactly once — after the watcher thread is joined (destruction), or
// immediately when `tracker` is null (Host build) — so an in-flight notify
// can never outlive the context. Registration is once per tracker lifetime.
void processTrackerSetChangeNotifier(ProcessTracker* tracker, bool (*notify)(void*), void* context, void (*releaseContext)(void*));

// Whether this tracker can actually deliver fork/exec/exit edges.
//
// A non-null tracker does not guarantee working watches: registering EVFILT_PROC
// on a process this one does not own is refused, and the session's own leader
// is `login`, which is root. A caller that treats "I got a tracker" as "I will
// be told when the tree changes" waits forever.
[[nodiscard]] bool processTrackerDeliversEdges(ProcessTracker* tracker);

// Offer the tracker processes discovered by other means, so it can register
// interest in the ones it is permitted to watch.
//
// The session's own leader is `login`, which is root: its forks are not
// observable by us, so a tracker rooted there learns nothing and the shell
// below it is never discovered from an edge. The subtree walk finds that shell
// anyway, and it belongs to us, so what the walk knows is handed back here and
// the watch set grows to cover it. Idempotent; pids already watched are skipped.
void processTrackerObserveProcesses(ProcessTracker* tracker, std::span<const ProcessID> processIDs);

// Snapshot the process subtree rooted at `leader` into `out` (<= out.size()
// records); marks processes in `foregroundGroup` as foreground and stamps each
// record's tty. Returns the count written. Admission is strictly by ancestry to
// `leader`, never a global tty/pgid, so two live sessions never bleed. A non-null
// `tracker` supplies the candidate pids; otherwise every process is scanned.
int gatherSubtree(ProcessID leader, ProcessID foregroundGroup, const std::string& tty, std::span<PAL::Process::Record> outputBuffer, ProcessTracker* tracker = nullptr);

// True when `claimedOwner` is `sessionLeader`, or `sessionLeader` is one of its
// process-tree descendants — i.e. `claimedOwner` could plausibly have forked the
// session it claims to own. Verifies an ADOPT_SESSION's PTY master fd
// (crossed via SCM_RIGHTS from a same-uid peer) belongs to the peer's own
// subtree, not an unrelated session it merely had permission to open. FAILS
// CLOSED (false) when `sessionLeader` isn't a live pid, or where the walk isn't
// implemented yet.
bool isProcessAncestorOfSession(ProcessID claimedOwner, ProcessID sessionLeader);

} // namespace Ubiquitty::PAL
