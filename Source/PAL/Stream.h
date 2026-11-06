// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"

#include "PAL/FileDescriptor.h"
#include "PAL/OutputQueue.h"
#include "PAL/Process.h"
#include "PAL/TerminalIdentity.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Byte-stream transport object. Destructor closes the transport (RAII); one
// concrete subclass per build; openPTY() is the factory. Foreground-process
// introspection is vended by reference via Stream::process() (see ProcessFacts).

namespace Ubiquitty::PAL {

/// How a flood reader approaches an empty non-blocking descriptor.
///
/// A PTY writer usually refills its shared kernel queue within microseconds, so
/// a short probe band avoids a readiness syscall at every refill gap. A socket
/// peer is paced by scheduling and framing instead; it must wait as soon as a
/// read would block.
enum class ReadinessWaitStrategy : uint8_t {
    ProbeBeforeWait,
    WaitFirst,
};

// read / write / writeAll return a non-negative byte count, or one of these
// (negative) sentinels cast to int — the < 0 contract the Stream methods document.
// lastError() carries the backend errno when the status is Fatal.
enum class StreamStatus : int {
    Again = -2, // would block; retry on the next idle tick
    EndOfFile = -3, // peer closed; further reads return 0
    Closed = -4, // local side already closed; the handle is dead
    Fatal = -1, // backend error — see lastError()
};

// POSIX pid_t.
using ProcessID = int;
// No live process in a slot that can hold one: a forkpty child once reaped, or a
// master adopted without a child.
inline constexpr ProcessID noProcess = -1;

/// Chooses whether closing a process-owning stream ends its child.
enum class ProcessClosePolicy {
    Terminate,
    KeepRunning,
};

// forkpty child's character-classification locale. Auto supplies a UTF-8 locale
// only when the inherited environment has no characterSet locale; Preserve copies the
// caller's verbatim; UTF8 forces UTF-8 LC_CTYPE even over C/POSIX.
enum class CharacterLocalePolicy {
    Auto UBIQUITTY_SWIFT_NAME(auto),
    Preserve UBIQUITTY_SWIFT_NAME(preserve),
    UTF8 UBIQUITTY_SWIFT_NAME(utf8),
};

/// Whether command-output color controls belong to the child terminal.
enum class ColorEnvironmentPolicy {
    Preserve,
    /// A daemon's noninteractive launcher does not choose its panes' colors.
    Terminal,
};

/// The line discipline a program set on its terminal, as the PTY's master
/// reads it: whether typed characters echo, whether input is read by line,
/// whether a signal character signals and discards the input waiting unread,
/// and the characters that signal the foreground group. A character the
/// program disabled is absent.
struct LineDiscipline {
    bool isEchoEnabled = true;
    bool isCanonical = true;
    /// Whether the signal characters signal (ISIG). Off, they are data.
    bool isSignalEnabled = true;
    /// Whether taking a signal character discards the input the terminal
    /// holds unread (NOFLSH off).
    bool isInputDiscardedOnSignal = true;
    std::optional<std::uint8_t> interruptCharacter;
    std::optional<std::uint8_t> quitCharacter;
    std::optional<std::uint8_t> suspendCharacter;
    std::optional<std::uint8_t> eraseCharacter = 0x7f;

    friend bool operator==(const LineDiscipline&, const LineDiscipline&) = default;
};

/// Who holds a terminal and what it is named, from one reading of its
/// foreground group.
struct ForegroundHolder {
    /// Whether the shell, rather than a job, holds the terminal.
    bool isShell = true;
    /// The name of what holds it: the program that makes a foreground job
    /// one, or the shell for its own work. Empty when it could not be read.
    std::string name;
};

// Foreground-process introspection over a session PTY. Lifetime == the Stream's.
// Non-const: foregroundIsShell()/hasRunningChild() reap a dead child (mutating
// the owning Stream's processID state), so this is a live handle, not a snapshot.
class ProcessFacts {
public:
    virtual ~ProcessFacts() = default;
    ProcessFacts(const ProcessFacts&) = delete;
    ProcessFacts& operator=(const ProcessFacts&) = delete;

    // The name of what holds the terminal, as foregroundIsShell answers who
    // does: the program that makes a foreground job one, or the shell for its
    // own work.
    virtual std::string foregroundName() = 0;
    // Whether the shell, rather than a job, holds the terminal. A foreground
    // group that is the shell's own work, such as a prompt hook's subshell,
    // reads as the shell; Session::foregroundOwner draws that line.
    virtual bool foregroundIsShell() = 0;
    /// Who holds the terminal and its name, as `foregroundIsShell` and
    /// `foregroundName` answer them, from one reading of the foreground group,
    /// so the name is always the holder's.
    ///
    /// - Returns: The holder.
    virtual ForegroundHolder foregroundHolder()
    {
        const bool isShell = foregroundIsShell();
        return { .isShell = isShell, .name = foregroundName() };
    }
    // Cheap change-detector. Empty when no foreground group resolves; a held
    // value is always a live group, so callers test only for presence. Optional
    // rather than a sentinel because every consumer stores the group as an
    // unsigned 0-means-none field, and an implicit cast of a negative sentinel
    // would survive as a large positive and read as a live group.
    virtual std::optional<ProcessID> foregroundProcessGroup() = 0;
    // Fork/exec/exit edge notifier for the session's process tree (see
    // processTrackerSetChangeNotifier in PTYFacts.h for the threading and
    // context-ownership contract). Backends without a tracker release the
    // context immediately.
    virtual void setTreeChangeNotifier(bool (*notify)(void*), void* context, void (*releaseContext)(void*))
    {
        (void)notify;
        if (releaseContext)
            releaseContext(context);
    }
    // Whether gatherProcesses descends a handful of pids rather than scanning
    // every process on the machine. A cheap walk needs no gate in front of it,
    // and the gate is what kept a title standing wrong: it guesses from the
    // foreground group and name, neither of which moves when a program that
    // already holds the terminal spawns a child.
    virtual bool hasCheapProcessWalk() { return false; }

    /// Whether this session's process tree announces its own changes.
    ///
    /// A fork, exec or exit that arrives as an event rather than as a
    /// difference noticed by walking. Darwin's kqueue raises them; a platform
    /// with no such facility raises none, and there the walk has to be paced by
    /// something, because a title whose only other signal is output stands
    /// still under a program that prints nothing.
    ///
    /// - Returns: `true` where the kernel raises tree edges for this session.
    virtual bool deliversTreeEdges() { return false; }

    /// The line discipline the program set on its terminal.
    ///
    /// - Returns: It, or `nullopt` where there is no terminal to read: a
    ///   pipe, wasm, an in-process session.
    virtual std::optional<LineDiscipline> lineDiscipline() { return std::nullopt; }

    // Offer process ids found by a walk to whatever watches this session's
    // tree, so the watch set can cover what the walk discovered.
    virtual void observeProcesses(std::span<const ProcessID>) { }
    virtual std::string foregroundCurrentDirectory() = 0;
    virtual std::string ttyName() = 0; // replica tty basename, e.g. "ttys003"
    virtual bool hasRunningChild() = 0;
    // Snapshot the PTY session subtree into `out`; returns the count written
    // (<= out.size()).
    virtual int gatherProcesses(std::span<PAL::Process::Record> outputBuffer) = 0;

    /// Sends a signal to a process group owned by this terminal session.
    ///
    /// - Parameters:
    ///   - processGroup: The group to signal.
    ///   - signal: The native signal number.
    /// - Returns: Whether the backend accepted the signal.
    virtual bool sendSignalToProcessGroup(ProcessID processGroup, int signal)
    {
        (void)processGroup;
        (void)signal;
        return false;
    }

protected:
    ProcessFacts() = default;
};

/// An open byte transport.
///
/// The destructor closes it; an owner holds a `unique_ptr`, and borrows are a
/// non-owning reference or pointer. `read`, `write` and `writeAll` are
/// non-blocking: a value >= 0 is a byte count, and a negative value is a
/// ``StreamStatus`` cast to int. No method throws (`-fno-exceptions`), and none
/// relies on `dynamic_cast` (`-fno-rtti`).
///
/// A socket, a pipe and a serial line are all this and nothing more. ``Stream``
/// adds what only a terminal transport can answer.
class ByteStream {
public:
    virtual ~ByteStream() = default;
    ByteStream(const ByteStream&) = delete;
    ByteStream& operator=(const ByteStream&) = delete;

    virtual int read(std::span<char> buffer) = 0;
    virtual int write(std::span<const char> bytes) = 0;

    /// Writes what it can and queues the remainder.
    ///
    /// There is no caller-visible output queue; drive the queued bytes from the
    /// poll loop with ``drainPendingWrite`` and ``hasPendingWrite``.
    virtual int writeAll(std::span<const char> bytes) = 0;

    /// - Returns: True while queued bytes remain.
    virtual bool drainPendingWrite() = 0;
    [[nodiscard]] virtual bool hasPendingWrite() const = 0;

    /// How many bytes ``writeAll`` queued that the transport has not taken.
    ///
    /// A stream that does not count answers with the largest value while
    /// anything is queued, so a caller bounding the queue reads it as full.
    [[nodiscard]] virtual std::size_t pendingWriteBytes() const { return hasPendingWrite() ? std::numeric_limits<std::size_t>::max() : 0; }

    /// The backend `errno`, or 0 when the last operation succeeded.
    [[nodiscard]] virtual int lastError() const = 0;

protected:
    ByteStream() = default;
};

/// A byte transport a loop can wait on.
///
/// Pollability is orthogonal to what a transport carries, so it branches here
/// rather than sitting on ``ByteStream``: an in-memory transport has no handle to
/// wait on, and a browser one is woken by events instead of by readiness.
///
/// ``handle`` is pure rather than a defaulted "no handle": a transport that
/// cannot answer must fail to compile as this type, instead of returning an empty
/// owner that a poll loop would spin on.
class PollableByteStream : public ByteStream {
public:
    /// Duplicates this endpoint as an independently owned stream.
    ///
    /// A carrier that polls each direction separately needs one owner per
    /// direction over a single endpoint; each closes exactly once and this stream
    /// keeps its own. Declared here as well as on ``Stream`` because the two are
    /// separate hierarchies and both kinds of endpoint are polled the same way.
    ///
    /// - Returns: Nothing when the backend cannot duplicate, which is the default.
    [[nodiscard]] virtual std::unique_ptr<PollableByteStream> duplicateAsPollableStream() { return nullptr; }

    /// The handle to register in a ``PAL::Watch``, owned by this stream.
    [[nodiscard]] virtual FileDescriptor& handle() = 0;

    /// Returns how a flood reader should approach a temporary empty read.
    [[nodiscard]] virtual ReadinessWaitStrategy readinessWaitStrategy() const { return ReadinessWaitStrategy::ProbeBeforeWait; }

protected:
    PollableByteStream() = default;
};

/// How a transport's child process ended.
///
/// A reaped child is the same event to a peer whichever way it went — the
/// session closes — so a caller reporting the reap can say nothing useful
/// without this. A clean exit, a SIGHUP and a SIGTTOU are three different
/// faults, and the first is the program's own decision while the others are
/// something done to it.
struct ChildExit {
    /// Whether a child was reaped at all. False on a transport with none, and
    /// on one whose child is still running.
    bool didExit = false;
    /// Whether a signal ended it, rather than its own return or exit call.
    bool wasSignalled = false;
    /// The exit status, or the signal number when ``wasSignalled``.
    int code = 0;
};

/// A byte transport that carries a terminal session.
///
/// Adds the two questions only a terminal transport can answer: the window size
/// the far side should believe, and the foreground process running on it. A
/// serial line already answers the first with an empty body — a socket could
/// answer neither, which is why they are here rather than on ``ByteStream``.
///
/// Not a ``PollableByteStream``: the in-memory implementations report no
/// descriptor, so carrying a session promises a loop nothing to wait on.
///
/// What ``read`` returns from a PTY is the kernel's bytes with one addition.
/// When a write carried the terminal's interrupt, quit or suspend character
/// and the terminal's modes make it discard unread output and echo the
/// character (ISIG on, NOFLSH off, ECHO and ECHOCTL on), the read that begins
/// with that echo is returned with one cancel byte (0x18) ahead of it. The
/// output was cut at that point, and the cancel keeps a parser from joining
/// the sequence before the cut to the echo after it. Every other read is the
/// kernel's bytes unchanged. Docs/architecture.md, section 4, has the
/// reasoning.
///
/// What ``writeAll`` hands a PTY's program is the caller's bytes in order, with
/// one exception. An interrupt, quit or suspend character does not wait behind
/// input the terminal is not taking: the input queued ahead of it is discarded,
/// as the line discipline discards unread input on taking the character, and
/// under NOFLSH the signal is raised directly and nothing is discarded. The
/// same section has the reasoning.
class Stream : public ByteStream {
public:
    /// Copies accepted bytes that have not reached the PTY, with their owners.
    /// The caller holds writes and drains until handover commits or aborts;
    /// this snapshot leaves the predecessor able to resume on refusal.
    /// - Returns: A bounded exact suffix, or nothing if unsupported, closed,
    ///   or allocation refused. An empty supported queue returns an empty owner.
    [[nodiscard]] virtual std::optional<OutputQueue::Unwritten> copyPendingWrite() const { return std::nullopt; }

    /// Installs a validated suffix ahead of all future input, without writing.
    /// Already accepted events must retain their ledger receipts, not be
    /// submitted again. Carried owner tokens must be reserved before new ones.
    /// - Parameter unwritten: Consumed only on success, unchanged on refusal.
    /// - Returns: False if unsupported, closed, occupied, or allocation refuses.
    [[nodiscard]] virtual bool restorePendingWrite(OutputQueue::Unwritten& unwritten)
    {
        (void)unwritten;
        return false;
    }

    /// Submits bytes with a queue-local owner that can withdraw their pending
    /// suffix. This is storage ownership, not permission to write. Ordinary
    /// writeAll bytes have no cancellable owner. The default refuses rather
    /// than accepting bytes a backend cannot later cancel selectively.
    ///
    /// - Parameters:
    ///   - owner: A nonzero token issued by this stream's trusted owner.
    ///   - bytes: The bytes to submit. Compound-event refusal has no signal
    ///     side effect. A standalone terminal signal retains its existing
    ///     bypass at capacity without admitting ordinary queued payload.
    /// - Returns: The writeAll status, or nullopt for unsupported ownership or
    ///   owner zero. A nonnegative status means queued, not fully delivered.
    [[nodiscard]] virtual std::optional<int> writeAllOwned(std::uint64_t owner, std::span<const char> bytes)
    {
        (void)owner;
        (void)bytes;
        return std::nullopt;
    }

    /// Drops only one owner's bytes still buffered in user space. Already
    /// delivered bytes and other owners' input or terminal replies are not
    /// touched. This does not revoke the token or stop future submissions.
    ///
    /// - Parameter owner: A nonzero stream-owner-issued token.
    /// - Returns: Bytes removed, or nullopt if unsupported or owner zero.
    ///   The count describes neither an original event nor delivery history.
    [[nodiscard]] virtual std::optional<std::size_t> discardPendingWrite(std::uint64_t owner)
    {
        (void)owner;
        return std::nullopt;
    }

    /// Counts one owner's undelivered bytes under the stream's output lock.
    /// - Parameter owner: A nonzero server-issued queue token.
    /// - Returns: The queued suffix length, or nullopt when unsupported.
    [[nodiscard]] virtual std::optional<std::size_t> pendingOwnedWriteBytes(std::uint64_t owner) const
    {
        (void)owner;
        return std::nullopt;
    }

    /// Withdraws owned bytes, retaining only trusted paste delimiter repair.
    /// - Parameters:
    ///   - owner: Exact server-issued output owner.
    ///   - repair: At most twelve delimiter bytes, placed before later replies.
    /// - Returns: Whether cancellation and repair were admitted atomically.
    [[nodiscard]] virtual bool repairPendingWrite(std::uint64_t owner, std::span<const char> repair)
    {
        return repair.empty() && discardPendingWrite(owner).has_value();
    }

    virtual void setWindowSize(int columns, int rows, int xPixels, int yPixels) = 0;

    /// Sends the window size and tells the foreground program to read it again.
    ///
    /// A size equal to the one the terminal already holds changes nothing, so
    /// setting it raises no SIGWINCH, and a program that missed the signal for
    /// the last real change is never told again. A pseudo-terminal sends the
    /// signal itself after the size. A transport with no foreground program to
    /// signal sends the size alone.
    ///
    /// The caller decided this re-send for one process group, and the group
    /// holding the terminal at delivery can be another: the signal goes out
    /// only while the two are the same, so a program that has since given the
    /// terminal back is not woken in the new holder's place. The size is sent
    /// either way.
    ///
    /// - Parameters:
    ///   - columns: The grid's columns.
    ///   - rows: The grid's rows.
    ///   - xPixels: The text area's width in device pixels.
    ///   - yPixels: The text area's height in device pixels.
    ///   - expectedForegroundGroup: The foreground group the re-send was
    ///     decided for.
    virtual void reassertWindowSize(int columns, int rows, int xPixels, int yPixels, ProcessID expectedForegroundGroup)
    {
        (void)expectedForegroundGroup;
        setWindowSize(columns, rows, xPixels, yPixels);
    }

    /// How this transport's child ended, once it has.
    ///
    /// Defaulted to "no child" rather than pure, because a transport carrying a
    /// session need not own a process: a socket carrying one from elsewhere has
    /// no exit status to report and is not wrong for that.
    [[nodiscard]] virtual ChildExit childExit() const { return {}; }

    /// Sets child ownership after a confirmed transfer or reclaim.
    ///
    /// Streams without an owned process have nothing to transfer. Keeping a
    /// process running requires another owner to retain its terminal endpoint.
    ///
    /// - Parameter policy: Whether closing this stream terminates its child.
    virtual void setProcessClosePolicy(ProcessClosePolicy policy) { (void)policy; }

    /// The pollable descriptor, or -1 when the transport exposes none.
    ///
    /// Raw rather than a ``FileDescriptor``, and so the one place this contract
    /// still names a POSIX type. A caller that only waits on the transport
    /// belongs on ``PollableByteStream``; the rest hand the descriptor to a
    /// platform event source that takes the integer.
    [[nodiscard]] virtual int fileDescriptor() const = 0;

    /// Duplicates this transport's endpoint as an independently owned stream, for
    /// a carrier that polls one direction.
    ///
    /// A stream rather than a handle: a caller that wanted two raw endpoints had
    /// to name a platform handle type, and doing so in a Core interface is what
    /// made a concrete POSIX handle reachable from fourteen Core files. Each
    /// returned stream closes its own copy exactly once, and this stream keeps
    /// its own.
    ///
    /// - Returns: Nothing when the backend offers no poll endpoint, which is the
    ///   default.
    [[nodiscard]] virtual std::unique_ptr<PollableByteStream> duplicateAsPollableStream() { return nullptr; }

    /// The foreground-process facet, with the same lifetime as this stream.
    ///
    /// Non-const because it reaps.
    virtual ProcessFacts& process() = 0;

protected:
    Stream() = default;
};

// Everything about a PTY launch that is not the command itself. A struct rather
// than trailing defaults: the list runs past three, and a fourth positional
// argument is where call sites stop being readable.
// Every member carries a default initializer, including the ones whose default
// construction would already be right. A designated initializer that names a
// later member skips the earlier ones, and GCC's -Wmissing-field-initializers
// reads that as an oversight — which is -Werror on the Linux build, so a call
// site setting only `identity` fails to compile there and builds on macOS.
/// What the child process sees as its `argv[0]`.
enum class Argument0Style {
    // "-basename": the leading '-' is what tells an sh-like shell to source the
    // login profiles, and it is right for the shell a terminal opens by itself.
    LoginShell,
    // The path as given. A program named on a command line is not a login shell,
    // and one that inspects its own argv[0] — busybox, a multi-call binary, or
    // anything printing usage — reads "-vim" as a name it does not have.
    ProgramPath,
};

/// Whether this launch is the one a login session is recorded for.
///
/// At most one per window: a split, a restored pane and a named command join a
/// session already recorded, and a record each lists one user once per pane in
/// `who`. Ignored where the platform records sessions some other way.
enum class SessionRecord {
    /// Record this pseudo-terminal as a login session, and clear it on close.
    AsLogin,
    /// Leave the session table alone.
    None,
};

struct PTYLaunchOptions {
    // Start directory when given and reachable, else $HOME.
    std::string_view initialDirectory {};
    CharacterLocalePolicy characterLocalePolicy = CharacterLocalePolicy::Auto;
    TerminalIdentity identity {};
    Argument0Style argument0Style = Argument0Style::LoginShell;
    SessionRecord sessionRecord = SessionRecord::None;
    /// Unique NAME=value replacements, validated before the child is forked.
    std::span<const std::string> environmentOverrides {};
    ColorEnvironmentPolicy colorEnvironmentPolicy = ColorEnvironmentPolicy::Preserve;
    /// The file-creation mask the child starts with; unset leaves this
    /// process's own. A daemon that runs owner-only hands its shells the
    /// mask of the person who started it.
    std::optional<std::uint32_t> fileCreationMask {};
};

// Launch `arguments` (arguments[0] = executable path, exec'd as a login shell)
// on a fresh PTY. Empty pointer on any failure; never throws. Defined per-backend.
std::unique_ptr<Stream> openPTY(std::span<const std::string> arguments, const PTYLaunchOptions& options = {});

/// A child process whose standard input and output were redirected onto pipes.
///
/// Held for as long as the child matters; destroying it terminates and reaps.
/// Neither destruction nor ``terminate`` blocks the calling thread — a caller
/// tearing a transport down must not wait on a wedged child, and the escalation
/// from a polite request to a forced kill is the backend's to schedule.
class ChildProcess {
public:
    virtual ~ChildProcess() = default;

    /// Asks the child to exit, escalating if it does not, and reaps it.
    ///
    /// Idempotent, and safe to call from a teardown path: it returns before the
    /// child necessarily has.
    virtual void terminate() = 0;

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

protected:
    ChildProcess() = default;
};

/// Chooses whether the caller owns a separate diagnostic pipe.
enum class CommandStandardError {
    Inherit,
    Capture
};

/// A spawned command's pipe endpoints and the process behind them.
struct PipedCommand {
    /// Bytes written here reach the child's standard input.
    std::unique_ptr<PollableByteStream> input;
    /// The child's standard output.
    std::unique_ptr<PollableByteStream> output;
    /// Standard error when capture was requested; the caller must drain it.
    std::unique_ptr<PollableByteStream> standardError;
    /// The child. Null when the spawn failed.
    std::unique_ptr<ChildProcess> process;

    /// - Returns: True when the command is running and both endpoints are open.
    [[nodiscard]] explicit operator bool() const { return process && input && output; }
};

/// Runs `command` through the system shell with its input and output on pipes.
///
/// Captured standard error must be drained independently of standard output.
///
/// The two endpoints are separate kernel objects — one pipe each way — so a
/// caller polling them independently closes each exactly once.
///
/// - Parameters:
///   - command: A shell command line, interpreted by the platform's shell.
///   - standardError: Whether to inherit stderr or return a pipe that must be drained.
/// - Returns: The endpoints and the child; a failed spawn converts to false.
[[nodiscard]] PipedCommand spawnPipedCommand(std::string_view command, CommandStandardError standardError = CommandStandardError::Inherit);

// Adopt an EXISTING PTY master fd (e.g. passed to the daemon via SCM_RIGHTS for a
// session re-home). Wraps it without a child process — the adopter never
// signals/reaps the shell; the session ends when the master EOFs. Empty pointer
// on failure.
//
// `claimedOwnerProcessID` is the kernel-verified peer (e.g. SO_PEERCRED at accept
// time). Being a valid PTY master proves it is one, not who owns it: a same-uid
// peer could pass an fd to an unrelated tty replica and have the daemon broadcast
// that session's foreground facts to every client. Verified by walking the
// master's session leader (TIOCGSID) against the claimed owner's process subtree
// (PAL::isProcessAncestorOfSession) before trusting the fd.
std::unique_ptr<Stream> adoptPTY(int masterFileDescriptor, ProcessID claimedOwnerProcessID);

/// Constructs an inactive adopted endpoint without changing the shared master.
/// The descriptor must already be nonblocking. Admission verifies the same
/// master/session ancestry as adoptPTY, but never sets flags, resizes, reads,
/// writes, signals or takes ownership of a child. Destruction closes only this
/// duplicate. The caller keeps it private until adoption commits.
/// - Parameters:
///   - masterFileDescriptor: Consumed on success and failure.
///   - claimedOwnerProcessID: The freshly verified physical peer process.
/// - Returns: The private endpoint, or refusal without changing shared flags.
[[nodiscard]] std::unique_ptr<Stream> preparePTYAdoption(int masterFileDescriptor, ProcessID claimedOwnerProcessID);

/// Takes a transferred PTY while retaining its non-child process identity.
///
/// The authenticated caller supplies the expected leader and acceptable direct
/// parents. Parent membership is checked at admission; later reparenting does
/// not change the process's birth identity. Inherited processes are never reaped
/// here. Terminate closes signal the verified session group; KeepRunning closes
/// only this endpoint and require another master owner to keep the PTY alive.
///
/// - Parameters:
///   - masterFileDescriptor: Consumed on success and failure.
///   - expectedSessionLeader: The transferred session's exact leader PID.
///   - acceptableParents: Authenticated holder/adopter PIDs, not untrusted claims.
/// - Returns: The owned stream, or null when identity cannot be corroborated.
[[nodiscard]] std::unique_ptr<Stream> inheritPTY(int masterFileDescriptor, ProcessID expectedSessionLeader, std::span<const ProcessID> acceptableParents);

// Open a serial / RS-232 device as a Stream (a tty line, not a forkpty'd shell).
// `baudRate` is a standard rate; 0 leaves the port's current speed. Line is set
// raw 8N1, non-blocking. No child process (process() reports an empty session);
// the session ends when the device goes away. Empty pointer on open/config
// failure. Defined in SerialStream.cpp (POSIX termios).
std::unique_ptr<Stream> openSerial(std::string_view devicePath, int baudRate = 0);

} // namespace Ubiquitty::PAL
