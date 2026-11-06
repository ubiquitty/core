// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Portable POSIX byte-stream transport for the Stream object. PTYStream is one
// forkpty'd login shell on a non-blocking master fd, shared by every frontend via
// openPTY(). OS-specific process introspection is the only platform surface and
// lives behind PTYFacts.h.
//
// Concurrency: single-threaded by construction. The master fd is non-blocking so
// read/write never park the caller (EAGAIN -> StreamStatus::Again). The child is
// reaped lazily — every read probes waitpid(WNOHANG) so a child exit promotes the
// stream to Closed. The output-queue lock is os_unfair_lock on Apple (priority
// donation on the QoS hop between input writer and poll-loop drain), std::mutex
// elsewhere.

#include "Core/Session/EnclosingTerminalEnvironment.h"
#include "Core/Session/LaunchContext.h"
#include "Core/Session/ShellForeground.h"
#include "PAL/OutputQueue.h"
#include "PAL/PAL.h"
#include "PAL/PTYFacts.h"
#include "PAL/Socket.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/POSIX/InheritedPTY.h"

#include <new>
#if defined(__APPLE__)
#include <libproc.h>
#include <os/lock.h>
#else
#include <mutex>
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <util.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// POSIX global environment, declared at GLOBAL scope so the symbol is libc's
// `environ` (not a namespace-mangled Ubiquitty::PAL::environ). <unistd.h> declares
// it under POSIX, but Apple guards that behind SDK macros, so spell it here.
extern "C" char** environ;

namespace Ubiquitty::PAL {

namespace {

// Hard ceiling on argv tokens. Two fixed arrays of this many POINTERS live on
// the stack — one in the parent, one in the post-fork child where malloc is not
// available — so the cost of the ceiling is 8 bytes a slot and the reason for
// having one is the fork, not the memory. 32 is under what a real command line
// reaches: `-e sh -c '...'` with a long script is fine, but an invocation
// forwarding a wrapped argv is not.
constexpr int ptyMaxArguments = 256;

constexpr char defaultCharacterLocaleEnvironment[] = "LC_CTYPE=C.UTF-8";

// `assignment` carries its own '=' — "LC_ALL=" and not "LC_ALL" — so the name
// and the length that has to agree with it are the same token.
bool environmentEntryHasNonEmptyValue(const char* entry, std::string_view assignment)
{
    const std::string_view field(entry);
    return field.starts_with(assignment) && field.size() > assignment.size();
}

bool hasInheritedCharacterLocale()
{
    if (!environ)
        return false;
    for (char** entry = environ; *entry; ++entry) {
        if (environmentEntryHasNonEmptyValue(*entry, "LC_ALL=") || environmentEntryHasNonEmptyValue(*entry, "LC_CTYPE=") || environmentEntryHasNonEmptyValue(*entry, "LANG="))
            return true;
    }
    return false;
}

bool shouldSetDefaultCharacterLocale(CharacterLocalePolicy policy)
{
    switch (policy) {
    case CharacterLocalePolicy::Auto:
        return !hasInheritedCharacterLocale();
    case CharacterLocalePolicy::UTF8:
        return true;
    case CharacterLocalePolicy::Preserve:
        return false;
    }
    // Outside the enumeration: preserve, which is the choice that changes
    // nothing about the child's environment.
    return false;
}

#if defined(__APPLE__)
// os_unfair_lock: 4 bytes, and it donates the waiter's priority to the lock
// owner under QoS contention (input writer on main vs. the ingest queue drain),
// so this short critical section can't invert priority.
struct OutputLock {
    os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    void acquire() { os_unfair_lock_lock(&lock); }
    void release() { os_unfair_lock_unlock(&lock); }
};
#else
struct OutputLock {
    std::mutex lock;
    void acquire() { lock.lock(); }
    void release() { lock.unlock(); }
};
#endif

// Move `fileDescriptor` into non-blocking mode. Returns 0 on success, else the
// errno from fcntl.
int setNonBlocking(int fileDescriptor)
{
    int flags = ::fcntl(fileDescriptor, F_GETFL, 0);
    if (flags == -1)
        return errno;
    if (::fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK) == -1)
        return errno;
    return 0;
}

} // anonymous namespace

// The portable concrete byte-stream: a forkpty'd login shell on a PTY master.
// Private ProcessFacts base — process() returns *this, so the facet shares the live
// child processID / master fd directly (reapIfExited mutates through the same `this`).
// The forked child's fate, shared by the PTYStream and every pollable duplicate
// it hands out.
//
// Shared rather than copied, because reaping is destructive: `waitpid` yields the
// status exactly once, and a second caller gets ECHILD and cannot tell "still
// running" from "somebody else already reaped it". One record, one reap, and
// every holder reads the same answer.
//
// It exists at all because the duplicate is the endpoint that gets written to.
// `LocalPTYChannel` writes through `duplicateAsPollableStream()`, so a guard that
// lived only on PTYStream::write would sit on the path nothing takes.
enum class ProcessOwnership {
    Child,
    Inherited
};

class ChildProcessRecord {
public:
    /// Starts tracking a forked child.
    ///
    /// - Parameter childProcessID: The forkpty child, or `noProcess` for a
    ///   transport that owns none.
    explicit ChildProcessRecord(ProcessID childProcessID, ProcessOwnership ownership = ProcessOwnership::Child)
        : m_processID(childProcessID)
        , m_ownership(ownership)
    {
    }

    /// Probes `waitpid(WNOHANG)` once, and records the result if the child went.
    ///
    /// Best-effort and cheap: it returns immediately once the child is known to
    /// have exited, so a caller may ask on every write without paying a syscall
    /// per write after the first positive answer.
    void reapIfExited()
    {
        if (m_ownership == ProcessOwnership::Inherited || m_processID <= 0 || m_hasExited)
            return;
        int status = 0;
        const ProcessID reaped = ::waitpid(m_processID, &status, WNOHANG);
        if (reaped != m_processID)
            return;
        m_hasExited = true;
        m_processID = noProcess;
        // Kept rather than discarded: the read that follows reports end-of-file
        // whichever way the child went, so this is the only place the manner
        // survives. A caller reaping the session prints it.
        m_exit.didExit = true;
        m_exit.wasSignalled = WIFSIGNALED(status);
        m_exit.code = WIFSIGNALED(status) ? WTERMSIG(status)
                                          : (WIFEXITED(status) ? WEXITSTATUS(status) : 0);
    }

    /// Records an exit this side caused, where the status was already collected.
    void markExited() { m_hasExited = true; }
    /// Drops the process identity without claiming an exit was observed.
    void forgetProcess() { m_processID = noProcess; }

    /// The child, or `noProcess` once reaped.
    [[nodiscard]] ProcessID processID() const { return m_processID; }
    /// Whether the child is known to have gone.
    [[nodiscard]] bool hasExited() const { return m_hasExited; }
    /// How it went, default-valued until something reaped it.
    [[nodiscard]] ChildExit exit() const { return m_exit; }
    /// - Returns: Whether another process owns reaping this session leader.
    [[nodiscard]] bool isInherited() const { return m_ownership == ProcessOwnership::Inherited; }
    /// Records terminal closure without inventing a non-child's exit status.
    void noteTerminalClosed()
    {
        if (isInherited())
            m_hasExited = true;
    }

private:
    ProcessID m_processID = noProcess;
    ProcessOwnership m_ownership = ProcessOwnership::Child;
    bool m_hasExited = false;
    ChildExit m_exit;
};

// Real children and inherited leaders share their mutable exit record. An
// adopted childless master has no reap or shared exit state; keeping that
// sentinel inline avoids allocating a shared_ptr control block during Prepare.
// Each duplicate copies the sentinel, so none borrows its parent's lifetime.
class ChildProcessHandle {
public:
    ChildProcessHandle() = default;
    explicit ChildProcessHandle(std::shared_ptr<ChildProcessRecord> child)
        : m_shared(std::move(child))
    {
    }

    [[nodiscard]] ChildProcessRecord* operator->() { return m_shared ? m_shared.get() : &m_childless; }
    [[nodiscard]] const ChildProcessRecord* operator->() const { return m_shared ? m_shared.get() : &m_childless; }

private:
    std::shared_ptr<ChildProcessRecord> m_shared;
    ChildProcessRecord m_childless { noProcess };
};

// A pollable duplicate of the PTY master that answers about the child.
//
// `adoptByteStream` over a dup'd master is a descriptor and nothing else, so its
// write reports the bytes the kernel took. On Linux the master accepts a write
// after the replica hung up — the bytes land in a buffer nobody will ever read —
// so the count is a report of a delivery that did not happen. Darwin errors on
// that write, which is why the gap stayed invisible on the arm most people run.
//
// EOF and PTY EIO retire the shared inherited leader without reaping it. Closing
// one duplicate leaves sibling endpoints live; terminal hangup blocks writes
// through every endpoint that shares the record.
class PTYDuplicateStream final : public PollableByteStream {
public:
    /// Wraps a duplicated endpoint so its writes consult the child.
    ///
    /// - Parameters:
    ///   - endpoint: The dup'd master, already adopted. Owned from here.
    ///   - child: The record this duplicate shares with the stream it came from.
    PTYDuplicateStream(std::unique_ptr<PollableByteStream> endpoint, ChildProcessHandle child)
        : m_endpoint(std::move(endpoint))
        , m_child(std::move(child))
    {
    }

    int read(std::span<char> buffer) override
    {
        if (buffer.empty())
            return std::to_underlying(StreamStatus::Fatal);
        if (m_hasReachedEndOfFile)
            return std::to_underlying(StreamStatus::EndOfFile);
        const int result = m_endpoint->read(buffer);
        // A PTY master reports slave hangup as EOF or EIO, unlike a socket.
        // Closed means only this duplicate was closed, not that its sibling's
        // live session ended.
        if (result == 0 || result == std::to_underlying(StreamStatus::EndOfFile)
            || (result == std::to_underlying(StreamStatus::Fatal) && m_endpoint->lastError() == EIO)) {
            m_hasReachedEndOfFile = true;
            m_child->noteTerminalClosed();
            return std::to_underlying(StreamStatus::EndOfFile);
        }
        return result;
    }

    int write(std::span<const char> bytes) override { return m_hasReachedEndOfFile || m_child->hasExited() ? std::to_underlying(StreamStatus::Closed) : reportedAsClosedIfChildWent(m_endpoint->write(bytes)); }

    int writeAll(std::span<const char> bytes) override { return m_hasReachedEndOfFile || m_child->hasExited() ? std::to_underlying(StreamStatus::Closed) : reportedAsClosedIfChildWent(m_endpoint->writeAll(bytes)); }

    bool drainPendingWrite() override
    {
        // Pending owners remain charged until the endpoint is disposed; a
        // hangup is not permission to report its queued bytes as delivered.
        return m_hasReachedEndOfFile || m_child->hasExited() ? m_endpoint->hasPendingWrite() : m_endpoint->drainPendingWrite();
    }
    [[nodiscard]] bool hasPendingWrite() const override { return m_endpoint->hasPendingWrite(); }
    [[nodiscard]] std::size_t pendingWriteBytes() const override { return m_endpoint->pendingWriteBytes(); }
    [[nodiscard]] int lastError() const override { return m_hasReachedEndOfFile ? 0 : m_endpoint->lastError(); }
    [[nodiscard]] FileDescriptor& handle() override { return m_endpoint->handle(); }
    [[nodiscard]] ReadinessWaitStrategy readinessWaitStrategy() const override { return m_endpoint->readinessWaitStrategy(); }

    [[nodiscard]] std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override
    {
        // The duplicate of a duplicate keeps the guard: a caller that got here
        // through one wrapper has no way to know the next one dropped it.
        std::unique_ptr<PollableByteStream> endpoint = m_endpoint->duplicateAsPollableStream();
        if (!endpoint)
            return nullptr;
        return std::make_unique<PTYDuplicateStream>(std::move(endpoint), m_child);
    }

private:
    // A write that the descriptor accepted still has to answer for the child.
    // Only a positive count is reconsidered: a status is already an answer, and
    // turning `Again` into `Closed` would tell a caller to stop retrying a write
    // that would have gone through.
    [[nodiscard]] int reportedAsClosedIfChildWent(int written)
    {
        if (written <= 0)
            return written;
        m_child->reapIfExited();
        if (m_child->hasExited())
            return std::to_underlying(StreamStatus::Closed);
        return written;
    }

    std::unique_ptr<PollableByteStream> m_endpoint;
    ChildProcessHandle m_child;
    bool m_hasReachedEndOfFile = false;
};

class PTYStream final : public Stream, private ProcessFacts {
public:
    /// An already verified, nonblocking childless master, held inactive.
    explicit PTYStream(UnixFileDescriptor masterFileDescriptor)
        : m_masterFileDescriptor(std::move(masterFileDescriptor))
    {
    }

    PTYStream(UnixFileDescriptor masterFileDescriptor, ProcessID childProcessID, std::string recordedPTY = {}, ProcessExitSource exitSource = ProcessExitSource::DirectChild, std::optional<ProcessBirthIdentity> inheritedBirth = std::nullopt)
        : m_masterFileDescriptor(std::move(masterFileDescriptor))
        , m_child(std::make_shared<ChildProcessRecord>(childProcessID, inheritedBirth ? ProcessOwnership::Inherited : ProcessOwnership::Child))
        , m_inheritedBirth(inheritedBirth)
        , m_isLoginProgram(exitSource == ProcessExitSource::LoginShell)
        , m_recordedPTY(std::move(recordedPTY))
    {
        // kqueue subtree watcher (see PTYFacts.h); nullptr where unsupported, then
        // gatherProcesses falls back to the full scan.
        m_processTracker = startProcessTracker(childProcessID, exitSource);
    }
    ~PTYStream() override;

    int read(std::span<char> buffer) override;
    int write(std::span<const char> bytes) override;
    int writeAll(std::span<const char> bytes) override;
    std::optional<OutputQueue::Unwritten> copyPendingWrite() const override;
    bool restorePendingWrite(OutputQueue::Unwritten&) override;
    std::optional<int> writeAllOwned(std::uint64_t owner, std::span<const char> bytes) override;
    std::optional<std::size_t> discardPendingWrite(std::uint64_t owner) override;
    std::optional<std::size_t> pendingOwnedWriteBytes(std::uint64_t owner) const override;
    bool repairPendingWrite(std::uint64_t owner, std::span<const char> repair) override;
    bool drainPendingWrite() override;
    bool hasPendingWrite() const override;
    std::size_t pendingWriteBytes() const override;
    void setWindowSize(int columns, int rows, int xPixels, int yPixels) override;
    void reassertWindowSize(int columns, int rows, int xPixels, int yPixels, ProcessID expectedForegroundGroup) override;
    int lastError() const override { return m_lastError.load(std::memory_order_relaxed); }
    int fileDescriptor() const override { return m_masterFileDescriptor.get(); }

    // The parameters are the portable PAL::FileDescriptor; on a POSIX build the
    // only implementation is UnixFileDescriptor (same "one concrete subclass per
    // build" rule Stream itself documents), so the cast is total. Unchecked
    // because -fno-rtti rules out dynamic_cast.
    std::unique_ptr<PollableByteStream> duplicateAsPollableStream() override
    {
        const int masterFileDescriptor = m_masterFileDescriptor.get();
        if (masterFileDescriptor < 0)
            return nullptr;
        // One duplex master, so a caller polling one direction gets a duplicate
        // that closes exactly once; this Stream keeps the original for teardown.
        const int duplicate = dup(masterFileDescriptor);
        if (duplicate < 0)
            return nullptr;
        // Wrapped rather than returned bare: the channel writes through this
        // endpoint, so an unguarded duplicate is a write path with no child
        // check on it at all.
        std::unique_ptr<PollableByteStream> endpoint = adoptByteStream(duplicate, ReadinessWaitStrategy::ProbeBeforeWait);
        if (!endpoint)
            return nullptr;
        return std::make_unique<PTYDuplicateStream>(std::move(endpoint), m_child);
    }
    ProcessFacts& process() override { return *this; }

    // ProcessFacts (private base). Portable facts (tty/group/child/isShell) here;
    // name/currentDirectory/gather delegate to the per-OS seam.
    std::string foregroundName() override;
    bool foregroundIsShell() override;
    ForegroundHolder foregroundHolder() override;
    std::optional<ProcessID> foregroundProcessGroup() override;
    void setTreeChangeNotifier(bool (*notify)(void*), void* context, void (*releaseContext)(void*)) override
    {
        processTrackerSetChangeNotifier(m_processTracker, notify, context, releaseContext);
    }
    bool deliversTreeEdges() override { return processTrackerDeliversEdges(m_processTracker); }

    std::optional<LineDiscipline> lineDiscipline() override
    {
        // The master reads the discipline the program set on its side.
        struct termios mode {};
        if (!m_masterFileDescriptor || ::tcgetattr(m_masterFileDescriptor.get(), &mode) != 0)
            return std::nullopt;
        const auto character = [&](int index) -> std::optional<std::uint8_t> {
            const cc_t value = mode.c_cc[index];
            if (value == _POSIX_VDISABLE)
                return std::nullopt;
            return static_cast<std::uint8_t>(value);
        };
        return LineDiscipline { .isEchoEnabled = (mode.c_lflag & ECHO) != 0, .isCanonical = (mode.c_lflag & ICANON) != 0, .isSignalEnabled = (mode.c_lflag & ISIG) != 0, .isInputDiscardedOnSignal = (mode.c_lflag & NOFLSH) == 0, .interruptCharacter = character(VINTR), .quitCharacter = character(VQUIT), .suspendCharacter = character(VSUSP), .eraseCharacter = character(VERASE) };
    }
#if defined(__APPLE__)
    // Darwin descends with proc_listpids(PROC_PPID_ONLY) from the leader.
    bool hasCheapProcessWalk() override { return true; }
#endif
    void observeProcesses(std::span<const ProcessID> processIDs) override
    {
        processTrackerObserveProcesses(m_processTracker, processIDs);
    }
    std::string foregroundCurrentDirectory() override;
    std::string ttyName() override;
    bool hasRunningChild() override;
    int gatherProcesses(std::span<PAL::Process::Record> outputBuffer) override;
    bool sendSignalToProcessGroup(ProcessID processGroup, int signal) override
    {
        if (processGroup <= 1 || signal <= 0 || (m_child->isInherited() && !inheritedProcessIsCurrent())) {
            return false;
        }
        std::array<PAL::Process::Record, 256> records;
        const int count = gatherProcesses(records);
        for (int index = 0; index < count; ++index) {
            if (records[static_cast<std::size_t>(index)].processGroupID == static_cast<uint32_t>(processGroup)) {
                return (!m_child->isInherited() || inheritedProcessIsCurrent()) && ::killpg(processGroup, signal) == 0;
            }
        }
        return false;
    }

private:
    /// - Returns: Whether the master and live birth identity still name the
    ///   admitted session. Reparenting alone does not retire that identity.
    [[nodiscard]] bool inheritedProcessIsCurrent() const;
    /// The leader of the session the terminal runs.
    ///
    /// This stream's forkpty child, or, for an adopted master, the leader the
    /// master names: the adoption verified it descends from the peer that
    /// handed the master over, and that peer stays its parent and reaper.
    ///
    /// - Returns: The leader, or `noProcess` when the master names none.
    [[nodiscard]] ProcessID sessionLeader() const;
    // The pid whose foreground process group means "the shell is at its prompt".
    // Normally the session's leader, but when that leader is a `login` wrapper the
    // real shell is login's child — see through it. Caches once resolved.
    ProcessID effectiveShellLeader();
    /// Who owns the terminal's foreground group, by `Session::foregroundOwner`.
    ///
    /// - Returns: The shell's own work for a session that has ended or whose
    ///   group is the shell; a job without a naming member for a group the
    ///   process table cannot list.
    [[nodiscard]] Session::ForegroundOwner foregroundOwner();
    /// The name of a foreground's owner: the member that names a job, or the
    /// shell, past any `login` wrapper, for its own work and for a job no member
    /// read can name, as `processForegroundName` names it.
    ///
    /// - Parameter owner: The owner, as `foregroundOwner` read it.
    /// - Returns: The name, empty when the process has gone or is in exec.
    [[nodiscard]] std::string ownerName(const Session::ForegroundOwner& owner);
    // Best-effort waitpid(WNOHANG) on the shared record.
    void reapIfExited() { m_child->reapIfExited(); }

public:
    /// How the forked child ended.
    ///
    /// Filled by ``reapIfExited``, so it is answerable only once a read, write
    /// or poll has probed waitpid — before that the child is alive as far as
    /// this knows, and the reply says so.
    ///
    /// - Returns: The manner of exit, or a default when nothing was reaped.
    [[nodiscard]] ChildExit childExit() const override
    {
        // login's successful cleanup does not report the shell's exit status.
        return m_isLoginProgram ? processTrackerLoginShellExit(m_processTracker) : m_child->exit();
    }

    void setProcessClosePolicy(ProcessClosePolicy policy) override { m_processClosePolicy = policy; }

private:
    // One non-blocking write to the master: bytes accepted (0 = full now), <0 closed.
    int writeOnceToMaster(std::string_view bytes);
    static int writeSink(void* context, std::span<const char> bytes) { return static_cast<PTYStream*>(context)->writeOnceToMaster({ bytes.data(), bytes.size() }); }

    UnixFileDescriptor m_masterFileDescriptor; // PTY master in the parent
    ChildProcessHandle m_child; // real child state is shared with every duplicate
    std::optional<ProcessBirthIdentity> m_inheritedBirth;
    bool m_isLoginProgram = false;
    ProcessClosePolicy m_processClosePolicy = ProcessClosePolicy::Terminate;
    std::atomic<ProcessID> m_effectiveShellProcessID = 0; // resolved shell-leader cache (see effectiveShellLeader); read by every thread that asks for facts
    /* owned */ ProcessTracker* m_processTracker = nullptr; // kqueue subtree watcher; stopProcessTracker on close
    // The replica this stream wrote a login-session record for, empty when it
    // wrote none. Held so close clears exactly the record open wrote, rather
    // than re-deriving a name from a descriptor that is already gone.
    std::string m_recordedPTY;
    // errno of the most recent failing call; 0 if ok. Atomic because two
    // threads write it: the reader on the ingest thread, and the window-size
    // ioctl from whichever thread owns the view; neither reads the other's
    // value, so relaxed order is enough.
    std::atomic<int> m_lastError { 0 };
    bool m_endOfFile = false; // read() returned 0 — replica hung up
    unsigned m_againStatusCount = 0; // EAGAIN reads seen; gates the reap sampling in read()
    // The letter of an interrupt echo the next reads are watched for ('C' for
    // ^C), or zero. Set by the thread that writes input, taken by the one that
    // reads; see markOutputCutByInterrupt.
    std::atomic<std::uint8_t> m_expectedInterruptEcho { 0 };
    // Reads that brought data since the echo was expected, the reader's alone.
    std::uint8_t m_readsAwaitingInterruptEcho = 0;

    /// The terminal's modes as they stand when one of `bytes` could be a
    /// character the line discipline answers with a signal; nothing when none
    /// could, or signals are off.
    [[nodiscard]] std::optional<struct termios> signalModes(std::span<const char> bytes) const;
    /// Lets a signal character past input the terminal is not taking.
    ///
    /// The line discipline discards every unread character when it takes one,
    /// so what waits ahead of it is discarded here, from the queue and from the
    /// terminal, and the character goes in as it would have. Where the modes
    /// keep the input (NOFLSH), or the terminal cannot be emptied, the signal
    /// is raised on the foreground group directly and the character is spent.
    /// Under NOFLSH on Linux a stall the master does not report is not seen:
    /// the character is written in turn until the master refuses input.
    ///
    /// - Parameters:
    ///   - character: The signal character.
    ///   - modes: The terminal's modes.
    /// - Returns: Whether the character is still to be written.
    [[nodiscard]] bool passSignalCharacter(char character, const struct termios& modes);
    /// Discards the input the terminal holds unread.
    ///
    /// - Returns: Whether it was discarded.
    [[nodiscard]] bool discardUnreadInput();
    /// Marks, in the bytes just read, the place the line discipline cut the
    /// output for an interrupt.
    ///
    /// - Parameters:
    ///   - buffer: The caller's buffer, holding `count` bytes just read.
    ///   - count: How many bytes the read brought.
    /// - Returns: The count to report, one more when the mark was written.
    [[nodiscard]] int markOutputCutByInterrupt(std::span<char> buffer, int count);
    mutable OutputLock m_outputLock; // guards m_outputQueue (producer vs poll drain)
    /// Writes with the existing terminal signal semantics; zero owns ordinary
    /// writes, while a nonzero owner admits storage before handing bytes on.
    int writeAllForOwner(std::span<const char> bytes, std::uint64_t owner);

    /* owned */ OutputQueue* m_outputQueue = nullptr; // lazily allocated on first writeAll
};

ProcessID PTYStream::sessionLeader() const
{
    if (m_child->processID() > 0)
        return m_child->processID();
    if (!m_masterFileDescriptor)
        return noProcess;
    const pid_t leader = ::tcgetsid(m_masterFileDescriptor.get());
    return leader > 0 ? static_cast<ProcessID>(leader) : noProcess;
}

ProcessID PTYStream::effectiveShellLeader()
{
    if (const ProcessID cached = m_effectiveShellProcessID.load(std::memory_order_relaxed); cached > 0)
        return cached; // resolved once — stable
    // A `login` wrapper makes the real shell login's child. Cache only once
    // resolved: a pre-exec read must never pin the session's leader as the
    // shell (`login` cached before its shell appeared reads a pristine prompt
    // as busy). A leader that has exec'd a program other than `login` is the
    // shell for as long as it runs, which saves every later read its name.
    const ProcessID leader = sessionLeader();
    if (leader <= 0)
        return leader;
    const std::string leaderName = processExecutableName(leader);
    if (leaderName == "login") {
        const ProcessID shell = firstChildProcess(leader);
        if (shell <= 0)
            return leader; // login's shell not up yet — don't cache
        m_effectiveShellProcessID.store(shell, std::memory_order_relaxed);
        return shell;
    }
    if (leaderName.empty())
        return leader;
    if (const std::optional<PAL::Process::GroupMember> member = processGroupMember(leader); member && member->hasExecuted && !member->isZombie)
        m_effectiveShellProcessID.store(leader, std::memory_order_relaxed);
    return leader;
}

int PTYStream::read(std::span<char> buffer)
{
    if (buffer.empty())
        return std::to_underlying(StreamStatus::Fatal);
    if (!m_masterFileDescriptor)
        return std::to_underlying(StreamStatus::Closed);
    if (m_endOfFile)
        return std::to_underlying(StreamStatus::EndOfFile);

    // One byte is held back while an interrupt's echo is expected, for the
    // mark that goes ahead of it.
    const bool isAwaitingEcho = m_expectedInterruptEcho.load(std::memory_order_relaxed) != 0 && buffer.size() > 2;
    ssize_t n = ::read(m_masterFileDescriptor.get(), buffer.data(), isAwaitingEcho ? buffer.size() - 1 : buffer.size());
    if (n > 0) {
        m_lastError = 0;
        return isAwaitingEcho ? markOutputCutByInterrupt(buffer, static_cast<int>(n)) : static_cast<int>(n);
    }
    if (n == 0) {
        // Slave end closed the PTY. Drain is done; promote to EOF.
        m_endOfFile = true;
        m_child->noteTerminalClosed();
        m_lastError = 0;
        return std::to_underlying(StreamStatus::EndOfFile);
    }

    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK) {
        // No bytes ready. Sample the reap (a wait4 syscall) only every 64th burst,
        // so a flood does not pay it per burst. A normal exit still surfaces via
        // EOF/POLLHUP; only the exited-without-EOF edge waits a few bursts longer.
        if ((m_againStatusCount++ & 63u) == 0)
            reapIfExited();
        if (m_child->hasExited())
            return std::to_underlying(StreamStatus::Closed);
        m_lastError = 0;
        return std::to_underlying(StreamStatus::Again);
    }
    if (err == EINTR) {
        m_lastError = 0;
        return std::to_underlying(StreamStatus::Again);
    }
    if (err == EIO) {
        // Darwin signals "replica hung up" with EIO on the master read.
        m_endOfFile = true;
        m_child->noteTerminalClosed();
        m_lastError = 0;
        return std::to_underlying(StreamStatus::EndOfFile);
    }
    m_lastError = err;
    return std::to_underlying(StreamStatus::Fatal);
}

std::optional<struct termios> PTYStream::signalModes(std::span<const char> bytes) const
{
    // Only a control character can be one, so ordinary typing and a paste of
    // text cost a scan and no ioctl.
    const bool hasControl = std::ranges::any_of(bytes, [](char byte) { return static_cast<unsigned char>(byte) < 0x20 || byte == 0x7F; });
    if (!hasControl || !m_masterFileDescriptor)
        return std::nullopt;
    // The master answers with the replica's modes, which are the program's to
    // change: a full-screen program turns ISIG off and takes ^C as a key.
    struct termios modes {};
    if (::tcgetattr(m_masterFileDescriptor.get(), &modes) != 0 || !(modes.c_lflag & ISIG))
        return std::nullopt;
    return modes;
}

// The signal the line discipline raises for `byte` under `modes`, or zero.
static int signalForCharacter(const struct termios& modes, char byte)
{
    const auto character = static_cast<cc_t>(byte);
    if (character == _POSIX_VDISABLE)
        return 0;
    if (character == modes.c_cc[VINTR])
        return SIGINT;
    if (character == modes.c_cc[VQUIT])
        return SIGQUIT;
    if (character == modes.c_cc[VSUSP])
        return SIGTSTP;
    return 0;
}

// The letter the terminal will echo after '^' for an interrupt among `bytes`
// under `modes`; zero when none of them is one, the output is not discarded
// for it, or nothing is echoed.
static char interruptEchoLetter(std::span<const char> bytes, const struct termios& modes)
{
    const bool doesEchoControls = (modes.c_lflag & ECHO) && (modes.c_lflag & ECHOCTL);
    if ((modes.c_lflag & NOFLSH) || !doesEchoControls)
        return 0;
    char letter = 0;
    for (const char byte : bytes) {
        if (signalForCharacter(modes, byte) != 0)
            letter = byte == 0x7F ? '?' : static_cast<char>(byte ^ 0x40);
    }
    return letter;
}

// Whether the master takes input the line discipline has stopped consuming and
// reports nothing. Linux buffers a master's writes ahead of the discipline,
// which stops taking them once its own buffer is full and holds a whole line:
// the write of an interrupt succeeds and the interrupt is not acted on. Darwin
// hands each character to the discipline as it is written and refuses the
// write instead, which the stream's queue shows.
#if defined(__linux__)
constexpr bool doesMasterHideStalledInput = true;
#else
constexpr bool doesMasterHideStalledInput = false;
#endif

bool PTYStream::discardUnreadInput()
{
#if defined(__linux__)
    // What the master wrote and the line discipline has not taken: the
    // master's output is the replica's input on its way in.
    (void)::tcflush(m_masterFileDescriptor.get(), TCOFLUSH);
#endif
    // What the line discipline holds unread is asked of the replica: on Linux
    // the same request on the master discards what the program wrote instead.
    // Opened without becoming anyone's controlling terminal, and closed again
    // at once.
    std::array<char, 128> path {};
    if (::ptsname_r(m_masterFileDescriptor.get(), path.data(), path.size()) != 0)
        return false;
    const UnixFileDescriptor replica(::open(path.data(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
    return replica && ::tcflush(replica.get(), TCIFLUSH) == 0;
}

bool PTYStream::passSignalCharacter(char character, const struct termios& modes)
{
    const bool doesDiscardInput = !(modes.c_lflag & NOFLSH);
    if (doesDiscardInput)
        m_outputQueue->discardPending();
    if (doesDiscardInput && discardUnreadInput())
        return true;
    (void)::ioctl(m_masterFileDescriptor.get(), TIOCSIG, signalForCharacter(modes, character));
    return false;
}

// A terminal that turns ^C, ^\ or ^Z into a signal discards the output it has
// not yet handed over when it takes one, then echoes the character as '^' and
// a letter. The program's output is cut wherever the reader had got to — in
// the middle of an escape sequence as readily as anywhere — and the echo
// arrives as the head of the next read, where a parser joins it to the half
// sequence before it. A lone ESC and the '^' open a privacy message (ESC ^),
// which runs to a terminator that never comes: the prompt and everything after
// it are consumed unseen until some later output carries an ESC.
//
// The cut is a fact only this side knows, so it is marked here, in the stream:
// the cancel control (0x18) goes ahead of the echo. It abandons a sequence in
// progress and does nothing otherwise, so every consumer of the stream reads
// the two halves apart without being told why.
int PTYStream::markOutputCutByInterrupt(std::span<char> buffer, int count)
{
    // An echo that has not headed a read by now is not coming: the modes moved
    // between the question and the write, or the write is still queued behind
    // input the program has not read. Left standing, the expectation would
    // mark a place in some later output that merely begins with these bytes.
    constexpr std::uint8_t readsToAwaitEcho = 8;
    const std::uint8_t letter = m_expectedInterruptEcho.load(std::memory_order_relaxed);
    const bool isEchoAtHead = count >= 2 && buffer[0] == '^' && static_cast<std::uint8_t>(buffer[1]) == letter;
    if (!isEchoAtHead) {
        if (++m_readsAwaitingInterruptEcho >= readsToAwaitEcho) {
            m_readsAwaitingInterruptEcho = 0;
            m_expectedInterruptEcho.store(0, std::memory_order_relaxed);
        }
        return count;
    }
    m_readsAwaitingInterruptEcho = 0;
    m_expectedInterruptEcho.store(0, std::memory_order_relaxed);
    constexpr char cancel = 0x18;
    std::memmove(buffer.data() + 1, buffer.data(), static_cast<std::size_t>(count));
    buffer[0] = cancel;
    return count + 1;
}

int PTYStream::write(std::span<const char> bytes)
{
    if (!m_masterFileDescriptor)
        return std::to_underlying(StreamStatus::Closed);
    if (bytes.empty()) {
        m_lastError = 0;
        return 0;
    }

    ssize_t n = ::write(m_masterFileDescriptor.get(), bytes.data(), bytes.size());
    if (n > 0) {
        // A successful write is not evidence of a live child. Linux accepts the
        // first master write after the replica hung up — the bytes land in the
        // kernel buffer and nothing will ever read them — so returning the count
        // reports delivery that did not happen. Darwin errors with EIO on that
        // same write and reaches the branch below, which is why the asymmetry
        // stayed invisible on the arm most people run.
        //
        // The read path asks the same question on EAGAIN, but samples it one
        // burst in 64. That is right there and wrong here: sampling weakens the
        // answer to "reports Closed eventually", and a caller that queues into a
        // buffer nobody drains needs the answer on the write it just made. The
        // read path can defer because a normal exit still reaches it via EOF.
        //
        // The price is bounded: a waitpid(WNOHANG) costs about what the small
        // write beside it costs. Bulk input does not pay it per byte — writeAll reaps
        // once and then queues, and the queue drains through writeOnceToMaster,
        // which does not come through here. The one caller that retries this
        // call in a loop, Daemon::drainTermToPTY, carries terminal reports and
        // iterates only when a write goes short.
        reapIfExited();
        if (m_child->hasExited())
            return std::to_underlying(StreamStatus::Closed);
        m_lastError = 0;
        return static_cast<int>(n);
    }
    if (n == 0) {
        // write returning 0 with length>0 is a spec curiosity; treat as Again.
        m_lastError = 0;
        return std::to_underlying(StreamStatus::Again);
    }

    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
        m_lastError = 0;
        return std::to_underlying(StreamStatus::Again);
    }
    if (err == EPIPE || err == EIO) {
        reapIfExited();
        return std::to_underlying(StreamStatus::Closed);
    }
    m_lastError = err;
    return std::to_underlying(StreamStatus::Fatal);
}

int PTYStream::writeOnceToMaster(std::string_view bytes)
{
    if (!m_masterFileDescriptor)
        return -1;
    ssize_t n = ::write(m_masterFileDescriptor.get(), bytes.data(), bytes.size());
    if (n >= 0)
        return static_cast<int>(n);
    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
        return 0; // buffer full right now — retry later
    if (err == EPIPE || err == EIO) {
        reapIfExited();
        return -1; // peer gone
    }
    m_lastError = err;
    return -1;
}

int PTYStream::writeAll(std::span<const char> bytes)
{
    return writeAllForOwner(bytes, 0);
}

std::optional<OutputQueue::Unwritten> PTYStream::copyPendingWrite() const
{
    if (!m_masterFileDescriptor || m_child->hasExited())
        return std::nullopt;
    m_outputLock.acquire();
    auto copied = m_outputQueue ? m_outputQueue->copyUnwritten() : std::optional { OutputQueue::Unwritten {} };
    m_outputLock.release();
    return copied;
}

bool PTYStream::restorePendingWrite(OutputQueue::Unwritten& unwritten)
{
    if (!m_masterFileDescriptor)
        return false;
    reapIfExited();
    if (m_child->hasExited())
        return false;
    m_outputLock.acquire();
    if (!m_outputQueue && !unwritten.bytes().empty())
        m_outputQueue = new (std::nothrow) OutputQueue();
    const bool didRestore = m_outputQueue ? m_outputQueue->restoreUnwritten(unwritten) : unwritten.bytes().empty();
    m_outputLock.release();
    return didRestore;
}

std::optional<int> PTYStream::writeAllOwned(std::uint64_t owner, std::span<const char> bytes)
{
    if (!owner)
        return std::nullopt;
    return writeAllForOwner(bytes, owner);
}

std::optional<std::size_t> PTYStream::discardPendingWrite(std::uint64_t owner)
{
    if (!owner)
        return std::nullopt;
    m_outputLock.acquire();
    const std::size_t discarded = m_outputQueue ? m_outputQueue->discardPending(owner) : 0;
    m_outputLock.release();
    return discarded;
}

std::optional<std::size_t> PTYStream::pendingOwnedWriteBytes(std::uint64_t owner) const
{
    if (!owner)
        return std::nullopt;
    m_outputLock.acquire();
    const std::size_t pending = m_outputQueue ? m_outputQueue->pendingBytes(owner) : 0;
    m_outputLock.release();
    return pending;
}

bool PTYStream::repairPendingWrite(std::uint64_t owner, std::span<const char> repair)
{
    if (!owner)
        return false;
    m_outputLock.acquire();
    if (!m_outputQueue && !repair.empty())
        m_outputQueue = new (std::nothrow) OutputQueue();
    const bool didRepair = m_outputQueue ? m_outputQueue->repairPending(owner, repair) : repair.empty();
    m_outputLock.release();
    return didRepair;
}

int PTYStream::writeAllForOwner(std::span<const char> bytes, std::uint64_t owner)
{
    if (!m_masterFileDescriptor)
        return std::to_underlying(StreamStatus::Closed);
    if (bytes.empty())
        return 0;
    // Asked before queueing rather than after writing, because the queue would
    // otherwise accept bytes for a child that is gone and report success for
    // them. The descriptor outlives the child on Linux, so its presence answers
    // a different question than "is anyone reading".
    reapIfExited();
    if (m_child->hasExited())
        return std::to_underlying(StreamStatus::Closed);
    const std::optional<struct termios> modes = signalModes(bytes);
    m_outputLock.acquire();
    if (!m_outputQueue)
        m_outputQueue = new (std::nothrow) OutputQueue();
    if (owner && (!m_outputQueue || m_outputQueue->reserveOwned(owner, bytes.size()) < 0)) {
        // A standalone signal has no ordinary payload to partially admit.
        // Preserve the terminal's interrupt escape even at the ownership
        // bound; compound events still refuse transactionally below.
        if (m_outputQueue && modes && bytes.size() == 1 && signalForCharacter(*modes, bytes.front())) {
            if (const char letter = interruptEchoLetter(bytes, *modes))
                m_expectedInterruptEcho.store(static_cast<std::uint8_t>(letter), std::memory_order_relaxed);
            const bool isStillToWrite = passSignalCharacter(bytes.front(), *modes);
            const int result = isStillToWrite ? m_outputQueue->submitOwned(owner, bytes, writeSink, this) : 0;
            m_outputLock.release();
            return result == OutputQueue::errQueueFull ? std::to_underlying(StreamStatus::Again)
                : result < 0                           ? std::to_underlying(StreamStatus::Fatal)
                                                       : 0;
        }
        const StreamStatus status = m_outputQueue ? StreamStatus::Again : StreamStatus::Fatal;
        m_outputLock.release();
        return std::to_underlying(status);
    }
    // Expected before the write: the echo can be read by another thread the
    // moment the character lands. An owned write refused above has no effect.
    if (const char letter = modes ? interruptEchoLetter(bytes, *modes) : 0)
        m_expectedInterruptEcho.store(static_cast<std::uint8_t>(letter), std::memory_order_relaxed);
    int rc = 0;
    if (m_outputQueue) {
        // Written in runs that end at a signal character. A terminal whose
        // input queue is full and holds a whole line takes no character at
        // all, the interrupt included, so one written in turn behind input the
        // program is not reading waits for a read that only the interrupt
        // would bring about. It does not wait: see passSignalCharacter.
        std::span<const char> rest = bytes;
        while (!rest.empty()) {
            const auto signalCharacter = modes
                ? std::ranges::find_if(rest, [&modes](char byte) { return signalForCharacter(*modes, byte) != 0; })
                : rest.end();
            const std::span<const char> ahead = rest.first(static_cast<std::size_t>(signalCharacter - rest.begin()));
            rc = owner ? m_outputQueue->submitOwned(owner, ahead, writeSink, this) : m_outputQueue->submit(ahead, writeSink, this);
            if (signalCharacter == rest.end() || (rc < 0 && rc != OutputQueue::errQueueFull))
                break;
            const bool wasRefused = rc == OutputQueue::errQueueFull;
            bool isStillToWrite = true;
            // Where a stall does not show, input ahead of the character is
            // discarded whenever the line discipline would discard it on
            // taking the character, which comes to the same input read.
            const bool mayInputBeStalledUnseen = doesMasterHideStalledInput && !(modes->c_lflag & NOFLSH);
            if (wasRefused || m_outputQueue->hasPending() || mayInputBeStalledUnseen)
                isStillToWrite = passSignalCharacter(*signalCharacter, *modes);
            // What was refused and not discarded is a hole the caller is told of.
            if (wasRefused && !isStillToWrite)
                break;
            rc = isStillToWrite
                ? (owner ? m_outputQueue->submitOwned(owner, rest.subspan(ahead.size(), 1), writeSink, this) : m_outputQueue->submit(rest.subspan(ahead.size(), 1), writeSink, this))
                : 0;
            if (rc < 0)
                break;
            rest = rest.subspan(ahead.size() + 1);
        }
    } else {
        // Allocation failure is pathological; fall back to a best-effort single
        // write so input still mostly flows (may truncate, but never crashes).
        ssize_t n = ::write(m_masterFileDescriptor.get(), bytes.data(), bytes.size());
        (void)n;
    }
    m_outputLock.release();
    // A full queue means the program is not reading stdin yet, not that it is
    // gone; the child is alive, so the caller can wait rather than end it.
    if (rc == OutputQueue::errQueueFull)
        return std::to_underlying(StreamStatus::Again);
    if (rc < 0)
        return static_cast<int>(m_masterFileDescriptor ? StreamStatus::Fatal : StreamStatus::Closed);
    return 0;
}

bool PTYStream::drainPendingWrite()
{
    m_outputLock.acquire();
    bool pending = false;
    if (m_outputQueue) {
        m_outputQueue->drain(writeSink, this);
        pending = m_outputQueue->hasPending();
    }
    m_outputLock.release();
    return pending;
}

std::size_t PTYStream::pendingWriteBytes() const
{
    m_outputLock.acquire();
    const std::size_t pending = m_outputQueue ? m_outputQueue->pendingBytes() : 0;
    m_outputLock.release();
    return pending;
}

bool PTYStream::hasPendingWrite() const
{
    // m_outputLock is mutable so this query can take it: the OutputQueue's pending
    // check needs the same serialization as the producer.
    m_outputLock.acquire();
    bool pending = (m_outputQueue != nullptr) && m_outputQueue->hasPending();
    m_outputLock.release();
    return pending;
}

void PTYStream::setWindowSize(int columns, int rows, int xPixels, int yPixels)
{
    if (!m_masterFileDescriptor || columns <= 0 || rows <= 0)
        return;
    if (columns > 0xFFFF)
        columns = 0xFFFF;
    if (rows > 0xFFFF)
        rows = 0xFFFF;
    if (xPixels < 0)
        xPixels = 0;
    if (yPixels < 0)
        yPixels = 0;
    if (xPixels > 0xFFFF)
        xPixels = 0xFFFF;
    if (yPixels > 0xFFFF)
        yPixels = 0xFFFF;

    struct winsize windowSize;
    windowSize = {};
    windowSize.ws_col = static_cast<unsigned short>(columns);
    windowSize.ws_row = static_cast<unsigned short>(rows);
    windowSize.ws_xpixel = static_cast<unsigned short>(xPixels);
    windowSize.ws_ypixel = static_cast<unsigned short>(yPixels);
    m_lastError = (::ioctl(m_masterFileDescriptor.get(), TIOCSWINSZ, &windowSize) == -1) ? errno : 0;
}

// The size first, so the program the signal wakes reads the size it is being
// told about. The group is the terminal's own foreground, read from the master,
// so the signal cannot reach a process outside this session, and it goes out
// only while that foreground is still the group the re-send was decided for:
// the rule that allowed a signal at all read that group's facts, and a shell
// that has taken the terminal back since reprints its prompt on every SIGWINCH
// it is sent.
void PTYStream::reassertWindowSize(int columns, int rows, int xPixels, int yPixels, ProcessID expectedForegroundGroup)
{
    setWindowSize(columns, rows, xPixels, yPixels);
    if (!m_masterFileDescriptor)
        return;
    const pid_t foreground = ::tcgetpgrp(m_masterFileDescriptor.get());
    if (foreground > 1 && static_cast<ProcessID>(foreground) == expectedForegroundGroup
        && (!m_child->isInherited() || inheritedProcessIsCurrent()))
        (void)::killpg(foreground, SIGWINCH);
}

std::string PTYStream::foregroundName()
{
    return foregroundHolder().name;
}

bool PTYStream::foregroundIsShell()
{
    if (m_masterFileDescriptor && !m_child->hasExited())
        reapIfExited();
    return !foregroundOwner().isJob;
}

ForegroundHolder PTYStream::foregroundHolder()
{
    if (!m_masterFileDescriptor)
        return {};
    if (!m_child->hasExited())
        reapIfExited();
    // The name pairs with the holder because both come from this one reading
    // of the owner.
    const Session::ForegroundOwner owner = foregroundOwner();
    return { .isShell = !owner.isJob, .name = ownerName(owner) };
}

std::string PTYStream::ownerName(const Session::ForegroundOwner& owner)
{
    return processForegroundName(owner.namingProcessID ? static_cast<ProcessID>(owner.namingProcessID) : effectiveShellLeader());
}

Session::ForegroundOwner PTYStream::foregroundOwner()
{
    if (!m_masterFileDescriptor || m_child->hasExited())
        return {};
    // At a pristine prompt the foreground group is the shell itself (pgid == its
    // processID). Treat an unresolvable group as the shell. The shell is the
    // session's leader, or login's child.
    const ProcessID foreground = ::tcgetpgrp(m_masterFileDescriptor.get());
    if (foreground <= 0 || foreground == sessionLeader())
        return {};
    const ProcessID shell = effectiveShellLeader();
    if (foreground == shell)
        return {};
    // Another group may still be the shell's own work: a prompt hook's
    // subshell holds the terminal for as long as it lives. Session decides
    // from the group's members. The usual job is its group's leader, an exec'd
    // child of the shell or a nested shell's fork, which one read settles
    // without listing it.
    const auto shellProcessID = static_cast<std::uint32_t>(shell);
    const auto group = static_cast<std::uint32_t>(foreground);
    if (const auto leader = processGroupMember(foreground); leader && Session::isJobLeader(*leader, shellProcessID))
        return { .isJob = true, .namingProcessID = leader->processID };
    std::array<PAL::Process::GroupMember, 32> members;
    const std::optional<std::size_t> count = processGroupMembers(foreground, members);
    // A group that cannot be read at all is a job, as one read in part is.
    if (!count)
        return { .isJob = true };
    return Session::foregroundOwner(std::span(members).first(std::min(*count, members.size())), group, *count <= members.size(), shellProcessID);
}

std::optional<ProcessID> PTYStream::foregroundProcessGroup()
{
    // Cheap change-detector: one tcgetpgrp ioctl, so the expensive gatherProcesses
    // walk runs only when the foreground group moves.
    if (!m_masterFileDescriptor)
        return std::nullopt;
    ProcessID foreground = ::tcgetpgrp(m_masterFileDescriptor.get());
    if (foreground <= 0)
        return std::nullopt;
    return foreground;
}

std::string PTYStream::foregroundCurrentDirectory()
{
    if (!m_masterFileDescriptor)
        return {};
    // The foreground group leader's currentDirectory is what the user sees; fall
    // back to the session's leader when the group can't be resolved.
    ProcessID foreground = ::tcgetpgrp(m_masterFileDescriptor.get());
    if (foreground > 0) {
        std::string currentDirectory = processCurrentDirectory(foreground);
        if (!currentDirectory.empty())
            return currentDirectory;
    }
    return processCurrentDirectory(sessionLeader());
}

std::string PTYStream::ttyName()
{
    if (!m_masterFileDescriptor)
        return {};
    // ptsname gives the replica path; show the basename, matching `tty`/`ps`.
    const char* path = ::ptsname(m_masterFileDescriptor.get());
    if (!path)
        return {};
    const char* base = std::strrchr(path, '/');
    return base ? base + 1 : path;
}

bool PTYStream::hasRunningChild()
{
    if (!m_masterFileDescriptor || m_child->hasExited())
        return false;
    reapIfExited();
    return !m_child->hasExited();
}

int PTYStream::gatherProcesses(std::span<PAL::Process::Record> outputBuffer)
{
    // The session's leader roots the subtree; foreground group + tty come from
    // the live master, so two sessions never bleed.
    if (outputBuffer.empty() || !m_masterFileDescriptor)
        return 0;
    const ProcessID leader = sessionLeader();
    if (leader <= 0)
        return 0;
    ProcessID foregroundGroup = ::tcgetpgrp(m_masterFileDescriptor.get());
    return gatherSubtree(leader, foregroundGroup, ttyName(), outputBuffer, m_processTracker);
}

namespace {

// Grace between the hangup and forcing the issue. A child that has not gone in
// this long is not going to: the shells that answer SIGHUP answer it at once.
// The wait runs off the main thread, and the grace is short, so a pane's child
// stops being a process almost as soon as its pane closes.
constexpr int hangupGraceMilliseconds = 50;

// The children still inside their grace. The escalation runs on a detached
// thread, and a process that exits first — the UI test runner's std::exit, a
// quit — takes that thread with it before its SIGKILL fires. A child that
// ignores the hangup is then nobody's: macOS `login` ignores SIGHUP until it
// has committed to the session, so a pane closed in that window leaves a root
// `login` plus a shell blocked forever on a pty whose master is long gone.
// atexit drains what is still pending, with the signal that cannot be ignored.
//
// Leaked on purpose: the detached threads may still be touching it while the
// statics are being destroyed.
class PendingHangups {
public:
    static PendingHangups& shared()
    {
        static PendingHangups* instance = new PendingHangups();
        return *instance;
    }

    void add(ProcessID childProcess)
    {
        const std::scoped_lock hold(m_lock);
        m_pending.insert(childProcess);
    }

    void remove(ProcessID childProcess)
    {
        const std::scoped_lock hold(m_lock);
        m_pending.erase(childProcess);
    }

private:
    PendingHangups() { std::atexit(flushAtExit); }

    static void flushAtExit()
    {
        PendingHangups& self = shared();
        const std::scoped_lock hold(self.m_lock);
        for (const ProcessID childProcess : self.m_pending) {
            ::kill(childProcess, SIGKILL);
            ::kill(-childProcess, SIGKILL);
        }
        self.m_pending.clear();
    }

    std::mutex m_lock;
    std::set<ProcessID> m_pending;
};

// Escalates and reaps a child that ignored its hangup.
//
// Takes the process id by value and touches nothing else: the stream that
// owned the child is destroyed by the time this runs, and the record it kept
// is shared with duplicates that may outlive both.
void reapAfterHangup(ProcessID childProcess)
{
    PendingHangups::shared().add(childProcess);
    std::thread([childProcess] {
        for (int elapsed = 0; elapsed < hangupGraceMilliseconds; ++elapsed) {
            const ProcessID reaped = ::waitpid(childProcess, nullptr, WNOHANG);
            if (reaped == childProcess || reaped < 0) {
                PendingHangups::shared().remove(childProcess);
                return;
            }
            ::usleep(1000);
        }
        ::kill(childProcess, SIGKILL);
        ::kill(-childProcess, SIGKILL); // ESRCH (ignored) if not a leader
        // Blocking, which is the point of being on a thread of our own: the
        // child stays a zombie holding its process id until someone reaps it,
        // so this is what keeps the id from being recycled under a later
        // signal.
        ::waitpid(childProcess, nullptr, 0);
        PendingHangups::shared().remove(childProcess);
    }).detach();
}

// The daemon owns the surviving terminal endpoint, but only this process can
// reap its child. Waiting must not block the frontend that detached from it.
void reapTransferredChild(ProcessID childProcess, std::string recordedPTY)
{
    std::thread([childProcess, recordedPTY = std::move(recordedPTY)] {
        while (::waitpid(childProcess, nullptr, 0) < 0 && errno == EINTR) {
        }
        if (!recordedPTY.empty()) {
            unregisterLoginSession(recordedPTY.c_str());
        }
    }).detach();
}

/// The descriptor numbers a forked child sweeps before it execs: below the
/// soft RLIMIT_NOFILE, the whole range a descriptor of this process can
/// occupy. Apple has no close_range, and a limit raised toward a million made
/// that walk a shell's startup; there the walk stops above the highest
/// descriptor open now, plus room for those other threads open before the
/// fork. Each of those takes the lowest free number, so it lands at most one
/// past the highest per open; only a dup2 to a chosen number could land
/// higher, and nothing here dups to anything but the standard three.
int maxChildDescriptorCount()
{
    int count = std::max(::getdtablesize(), 0);
#if defined(__APPLE__)
    constexpr int concurrentOpenAllowance = 1024;
    const int listBytes = ::proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
    if (listBytes <= 0)
        return count;
    // The headroom keeps a list that grew since the size was asked whole;
    // a list that filled it is not known to be whole, and keeps the full walk.
    std::vector<proc_fdinfo> open(static_cast<std::size_t>(listBytes) / sizeof(proc_fdinfo) + 64);
    const int capacityBytes = static_cast<int>(open.size() * sizeof(proc_fdinfo));
    const int filledBytes = ::proc_pidinfo(::getpid(), PROC_PIDLISTFDS, 0, open.data(), capacityBytes);
    if (filledBytes <= 0 || filledBytes >= capacityBytes)
        return count;
    int highest = STDERR_FILENO;
    for (std::size_t index = 0; index < static_cast<std::size_t>(filledBytes) / sizeof(proc_fdinfo); ++index)
        highest = std::max(highest, open[index].proc_fd);
    count = std::min(count, highest + 1 + concurrentOpenAllowance);
#endif
    return count;
}

} // namespace

PTYStream::~PTYStream()
{
    // Join the watcher before deciding who reaps the child or clears its
    // login record; it can publish the child's exit until the join completes.
    stopProcessTracker(m_processTracker);
    m_processTracker = nullptr;
    if (m_child->isInherited()) {
        // The predecessor (or its replacement reaper) owns waitpid. Never put
        // this non-child in a delayed signal/reap task keyed only by a PID.
        if (m_processClosePolicy == ProcessClosePolicy::Terminate && inheritedProcessIsCurrent())
            (void)::killpg(m_child->processID(), SIGHUP);
        m_masterFileDescriptor.reset();
        m_child->forgetProcess();
        delete m_outputQueue;
        return;
    }
    const bool shouldKeepChild = m_processClosePolicy == ProcessClosePolicy::KeepRunning
        && m_child->processID() > 0 && !m_child->hasExited();
    // A transferred child keeps its login record until the reaper sees its
    // exit; an ordinary close ends the recorded session here.
    if (!shouldKeepChild && !m_recordedPTY.empty())
        unregisterLoginSession(m_recordedPTY.c_str());
    m_masterFileDescriptor.reset();
    if (m_child->processID() > 0 && !m_child->hasExited()) {
        const ProcessID childProcess = m_child->processID();
        if (shouldKeepChild) {
            reapTransferredChild(childProcess, std::move(m_recordedPTY));
        } else {
            // A `login` wrapper that has not forked its shell yet is still in
            // the stretch where it ignores SIGHUP; a hangup there is dropped,
            // and the master's close that follows leaves it to start a shell
            // on a dead pty. With no shell there is nothing to hang up
            // gracefully, so that case gets the signal login cannot ignore.
            const bool isLoginWithoutShell = m_isLoginProgram && m_effectiveShellProcessID.load(std::memory_order_relaxed) <= 0
                && firstChildProcess(childProcess) <= 0;
            ::kill(childProcess, isLoginWithoutShell ? SIGKILL : SIGHUP);
            // One non-blocking probe covers what almost always happens: a shell
            // exits on the hangup before this returns. Anything else escalates off
            // this thread, because this destructor runs on the main thread and a
            // child wedged in an uninterruptible syscall does not answer SIGKILL
            // either — waiting for it here freezes the window that asked to close.
            if (::waitpid(childProcess, nullptr, WNOHANG) == childProcess) {
                m_child->markExited();
            } else {
                reapAfterHangup(childProcess);
            }
        }
        m_child->forgetProcess();
    }
    delete m_outputQueue; // flush-on-close is moot: the fd is gone; just free
    m_outputQueue = nullptr;
}

namespace {

// Ends a child whose stream could not be built, without waiting on it here.
//
// The master closes first. A program exiting closes its controlling terminal,
// and that close can wait for output nobody has read to drain; with the master
// still open and unread, a blocking wait could hold the caller for as long as
// the child stays stuck in its exit. Closed, the terminal has no reader to
// wait for. The reap then runs where a stream's hangup reap runs, bounded and
// escalating, off the caller's thread.
void abandonSpawnedChild(ProcessID childProcessID, UnixFileDescriptor master)
{
    master = {};
    ::kill(childProcessID, SIGHUP);
    if (::waitpid(childProcessID, nullptr, WNOHANG) != childProcessID)
        reapAfterHangup(childProcessID);
}

} // namespace

std::unique_ptr<Stream> openPTY(std::span<const std::string> arguments, const PTYLaunchOptions& options)
{
    if (arguments.empty() || arguments[0].empty())
        return {};
    if (options.environmentOverrides.size() > 128) {
        return {};
    }
    for (std::size_t index = 0; index < options.environmentOverrides.size(); ++index) {
        const auto& entry = options.environmentOverrides[index];
        const auto separator = entry.find('=');
        if (separator == std::string::npos || !separator || entry.find('\0') != std::string::npos) {
            return {};
        }
        const auto name = std::string_view(entry).substr(0, separator);
        if (name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string_view::npos
            || (name.front() >= '0' && name.front() <= '9') || name == "TERM" || name == "COLORTERM"
            || name == "TERM_PROGRAM" || name == "TERM_PROGRAM_VERSION" || name == "LC_CTYPE" || name == "LC_ALL") {
            return {};
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (std::string_view(options.environmentOverrides[previous]).starts_with(std::string(name) + "=")) {
                return {};
            }
        }
    }
    const std::string_view initialDirectory = options.initialDirectory;
    const CharacterLocalePolicy characterLocalePolicy = options.characterLocalePolicy;
    const Argument0Style argument0Style = options.argument0Style;
    const int argumentCount = static_cast<int>(arguments.size());
    // Refusing beats gridFloor. A clamp drops the tail of a command line and
    // launches the head, so `-e cmd <33 args>` runs cmd against a truncated
    // argument list and reports success — the caller sees a window, the program
    // sees different work. The ceiling is a fixed child-stack array (no malloc
    // after fork), so it stays a ceiling; what changes is that reaching it is
    // an answer rather than a silent edit.
    if (argumentCount > ptyMaxArguments)
        return {};
    // Materialise the C argv in the PARENT before the fork — std::string access is
    // not async-signal-safe in the post-fork child.
    const char* commandArguments[ptyMaxArguments + 1];
    for (int i = 0; i < argumentCount; ++i)
        commandArguments[i] = arguments[i].c_str();
    commandArguments[argumentCount] = nullptr;

    // Snapshot the advertised identity into fixed buffers before the fork.
    char termProgram[64];
    char termProgramVersion[32];
    char termIdentifier[64];
    {
        // Fixed buffers before the fork: reading a std::string_view's storage is
        // not async-signal-safe in the child.
        // The span carries the destination's extent, so a buffer and a size
        // cannot be paired wrongly at any of the three calls below.
        const auto copy = [](std::span<char> outputBuffer, std::string_view in) {
            const std::size_t length = std::min(in.size(), outputBuffer.size() - 1);
            // An empty identity has no storage, and memcpy's source must be
            // non-null even for zero bytes.
            if (length > 0)
                std::memcpy(outputBuffer.data(), in.data(), length);
            outputBuffer[length] = '\0';
        };
        copy(termProgram, options.identity.program);
        copy(termProgramVersion, options.identity.programVersion);
        copy(termIdentifier, options.identity.term);
    }

    // Snapshot the start directory before the fork — string_view access is not
    // async-signal-safe in the child. Empty stays empty; the child falls to $HOME.
    char initialDirectoryBuffer[1024];
    initialDirectoryBuffer[0] = '\0';
    if (!initialDirectory.empty()) {
        std::size_t length = initialDirectory.size();
        if (length >= sizeof(initialDirectoryBuffer))
            length = sizeof(initialDirectoryBuffer) - 1;
        std::memcpy(initialDirectoryBuffer, initialDirectory.data(), length);
        initialDirectoryBuffer[length] = '\0';
    }

    // Build the child's full environment here, in the parent. setenv() post-fork
    // would malloc + rewrite environ — not async-signal-safe; that malloc, under a
    // lock another thread holds at the fork instant, is the first-window
    // dead-session deadlock. Copy environ, drop stale copies of vars we override,
    // append ours — all into fixed parent-stack scratch; no heap, no setenv.
    const char* term = (termIdentifier[0] != '\0') ? termIdentifier : ::getenv("UBIQUITTY_TERM");
    if (!term || !term[0])
        term = "xterm-256color";
    char environmentTerm[128], environmentProgram[96], environmentProgramVersion[64];
    std::snprintf(environmentTerm, sizeof(environmentTerm), "TERM=%s", term);
    std::snprintf(environmentProgram, sizeof(environmentProgram), "TERM_PROGRAM=%s", termProgram);
    std::snprintf(environmentProgramVersion, sizeof(environmentProgramVersion), "TERM_PROGRAM_VERSION=%s", termProgramVersion);
    constexpr int ptyMaxEnvironmentEntries = 1024;
    const char* childEnvironment[ptyMaxEnvironmentEntries + 1];
    const bool addDefaultCharacterLocale = shouldSetDefaultCharacterLocale(characterLocalePolicy);
    int environmentCount = 0;
    childEnvironment[environmentCount++] = environmentTerm;
    childEnvironment[environmentCount++] = "COLORTERM=truecolor";
    if (addDefaultCharacterLocale)
        childEnvironment[environmentCount++] = defaultCharacterLocaleEnvironment;
    childEnvironment[environmentCount++] = environmentProgram;
    childEnvironment[environmentCount++] = environmentProgramVersion;
    for (const auto& entry : options.environmentOverrides) {
        childEnvironment[environmentCount++] = entry.c_str();
    }
    if (environ) {
        for (char** e = environ; *e && environmentCount < ptyMaxEnvironmentEntries; ++e) {
            const char* entry = *e;
            // Drop inherited copies of values we set, so the child sees one
            // authoritative value per name (our overrides lead the array).
            const std::string_view field(entry);
            // The daemon may have been launched by a monochrome command
            // runner. Explicit per-launch overrides and shell startup files
            // can still choose their own color policy.
            if (options.colorEnvironmentPolicy == ColorEnvironmentPolicy::Terminal
                && (field.starts_with("NO_COLOR=") || field.starts_with("CLICOLOR=")
                    || field.starts_with("CLICOLOR_FORCE=") || field.starts_with("FORCE_COLOR=")))
                continue;
            bool isOverridden = false;
            for (const auto& replacement : options.environmentOverrides) {
                const auto separator = replacement.find('=');
                if (field.starts_with(std::string_view(replacement).substr(0, separator + 1))) {
                    isOverridden = true;
                    break;
                }
            }
            // Context names where an enclosing pane runs; a child given none of
            // its own must not read that as its own location. A credential was
            // handed to the process that launched this one, not to its shells.
            // The enclosing terminal's identity describes the terminal this
            // process was started in, and the pane is not that terminal; an
            // override of the same name has already been taken above.
            if (isOverridden || Ubiquitty::Session::isContextEnvironmentEntry(field) || Ubiquitty::Session::isCredentialEnvironmentEntry(field)
                || Ubiquitty::Session::isEnclosingTerminalEnvironmentEntry(field)) {
                continue;
            }
            if (field.starts_with("TERM=") || field.starts_with("COLORTERM=") || (addDefaultCharacterLocale && field.starts_with("LC_CTYPE="))
                || (characterLocalePolicy == CharacterLocalePolicy::UTF8 && field.starts_with("LC_ALL=")) || field.starts_with("TERM_PROGRAM=") || field.starts_with("TERM_PROGRAM_VERSION="))
                continue;
            childEnvironment[environmentCount++] = entry;
        }
    }
    childEnvironment[environmentCount] = nullptr;
    // $PATH for the child's own resolution of a bare-name argv[0] (execve does no
    // PATH search; the child walks this below with stack buffers).
    const char* childPath = ::getenv("PATH");
    for (const auto& entry : options.environmentOverrides) {
        if (entry.starts_with("PATH=")) {
            childPath = entry.c_str() + 5;
        }
    }
    if (!childPath || !childPath[0])
        childPath = "/usr/bin:/bin:/usr/sbin:/sbin";

    // Read the descriptor ceiling before the fork: the child closes everything
    // above its report descriptor, and only close() is guaranteed
    // async-signal-safe there.
    const int maxDescriptorCount = maxChildDescriptorCount();
    // Read here for the same reason; umask itself is async-signal-safe.
    const bool hasFileCreationMask = options.fileCreationMask.has_value();
    const mode_t fileCreationMask = static_cast<mode_t>(options.fileCreationMask.value_or(0) & 0777);

    // Closed by the child's exec, or by its exit when every exec fails. The
    // parent reads to that end before it returns, so nobody sees the child
    // between the fork and the exec: a probe of it reads the shell, in its
    // directory, and never this process's name and working directory; and
    // the child's copies of this process's descriptors (a daemon's listening
    // socket and startup lock among them) are gone before the caller goes on.
    UnixPipe execReportPipe = makeUnixPipe();
    if (!execReportPipe)
        return {};
    (void)::fcntl(execReportPipe.read.get(), F_SETFD, FD_CLOEXEC);
    (void)::fcntl(execReportPipe.write.get(), F_SETFD, FD_CLOEXEC);
    const int reportDescriptor = execReportPipe.write.get();

    int rawMasterFileDescriptor = -1;
    ProcessID childProcessID = ::forkpty(&rawMasterFileDescriptor, nullptr, nullptr, nullptr);
    if (childProcessID < 0)
        return {};
    UnixFileDescriptor masterFileDescriptor(rawMasterFileDescriptor);
    if (childProcessID == 0) {
        // Child: start in the requested directory when reachable, else $HOME (the
        // inherited cwd is "/" when launched from Finder / `open`). First, before
        // the sweep below, so the directory is the shell's from the earliest
        // moment it could be read.
        bool changedDirectory = false;
        if (initialDirectoryBuffer[0])
            changedDirectory = (::chdir(initialDirectoryBuffer) == 0);
        if (!changedDirectory) {
            const char* home = ::getenv("HOME");
            // chdir is warn_unused_result under glibc and a (void) cast does not
            // suppress it under GCC — consume the result in the condition. A failed
            // chdir is non-fatal.
            if (home && home[0] && ::chdir(home) != 0) {
                // non-fatal: the child keeps its inherited working directory
            }
        }

        // The report pipe moves to the first descriptor above the standard
        // three, marked close-on-exec again because dup2 clears the mark, so a
        // single sweep past it closes the rest.
        const int childReportDescriptor = STDERR_FILENO + 1;
        if (reportDescriptor != childReportDescriptor
            && (::dup2(reportDescriptor, childReportDescriptor) != childReportDescriptor || ::fcntl(childReportDescriptor, F_SETFD, FD_CLOEXEC) != 0))
            _exit(127);

        // Everything this process had open, except the terminal forkpty just gave
        // the child. A shell inherits every descriptor its parent did not mark
        // close-on-exec, and the parent here may be the session daemon: its
        // listening socket, each attached client's socket, every other session's
        // PTY master. A shell holding another session's master can read and write
        // that session, which is the isolation the daemon exists to provide.
        //
        // Closed here rather than marked at each open() because the set is open-
        // ended — the daemon's descriptors, the QUIC socket, whatever a future
        // caller holds — and a mark that has to be remembered per site is one that
        // is missed at one of them. forkpty has already dup2'd the replica onto 0/1/2,
        // and dup2 clears close-on-exec, so the child's terminal survives this.
        //
        // close_range hands the whole span to the kernel in one call where the
        // kernel has it. The loop it replaces walks to the descriptor ceiling,
        // and on hosts that raise RLIMIT_NOFILE toward a billion that walk is
        // the shell taking seconds to appear; both forms are async-signal-safe.
#if defined(__linux__) && defined(SYS_close_range)
        if (::syscall(SYS_close_range, static_cast<unsigned>(childReportDescriptor + 1), ~0u, 0u) != 0) {
            for (int descriptor = childReportDescriptor + 1; descriptor < maxDescriptorCount; ++descriptor)
                ::close(descriptor);
        }
#else
        for (int descriptor = childReportDescriptor + 1; descriptor < maxDescriptorCount; ++descriptor)
            ::close(descriptor);
#endif

        // Every signal back to its default and none blocked. Dispositions set to
        // ignore survive exec, and so does the mask, so the shell would start
        // with whatever this process had: the daemon ignores SIGPIPE, and a
        // daemon started in the background of a script inherits SIGINT ignored.
        // A shell passes an ignored signal on to everything it runs, so ^C
        // reached a flooding program and did nothing. sigaction and
        // sigprocmask are async-signal-safe.
        {
            struct sigaction defaultAction {};
            defaultAction.sa_handler = SIG_DFL;
            sigemptyset(&defaultAction.sa_mask);
            for (int signalNumber = 1; signalNumber < NSIG; ++signalNumber) {
                if (signalNumber != SIGKILL && signalNumber != SIGSTOP)
                    (void)::sigaction(signalNumber, &defaultAction, nullptr);
            }
            sigset_t unblocked;
            sigemptyset(&unblocked);
            (void)::sigprocmask(SIG_SETMASK, &unblocked, nullptr);
        }

        if (hasFileCreationMask)
            (void)::umask(fileCreationMask);

        // Exec as a login shell: a leading '-' in argv[0] tells an sh-like shell to
        // source the login profiles. Exec the real path but hand it "-name". Exec
        // failure exits 127 (seen via waitpid).
        const char* path = commandArguments[0];
        const char* slash = ::strrchr(path, '/');
        const char* base = slash ? slash + 1 : path;
        char loginArgument0[256];
        loginArgument0[0] = '-';
        // Bounded copy of `base` after the '-', by hand (strlcpy is BSD-only) in
        // this post-fork, async-signal-safe context.
        char* loginName = loginArgument0 + 1;
        const size_t loginRoom = sizeof(loginArgument0) - 1; // bytes after the '-'
        size_t loginLength = 0;
        for (; base[loginLength] != '\0' && loginLength + 1 < loginRoom; ++loginLength)
            loginName[loginLength] = base[loginLength];
        loginName[loginLength] = '\0';
        // Mutable, NUL-terminated execArguments (commandArguments is const; a fixed
        // child-stack array is async-signal-safe — no malloc post-fork).
        char* execArguments[ptyMaxArguments + 1];
        for (int i = 0; i < argumentCount; ++i)
            execArguments[i] = const_cast<char*>(commandArguments[i]);
        execArguments[argumentCount] = nullptr;
        if (argument0Style == Argument0Style::LoginShell)
            execArguments[0] = loginArgument0;
        char* const* childEnvironmentPointers = const_cast<char* const*>(childEnvironment);
        // execve does no $PATH search. A path with '/' exec's directly; a bare name
        // gets the PATH walk done here with stack buffers (access + execve are both
        // async-signal-safe).
        if (slash) {
            ::execve(path, execArguments, childEnvironmentPointers);
        } else {
            const char* segment = childPath;
            char candidate[1024];
            while (*segment) {
                const char* segmentEnd = segment;
                while (*segmentEnd && *segmentEnd != ':')
                    ++segmentEnd;
                size_t segmentLength = static_cast<size_t>(segmentEnd - segment);
                if (segmentLength == 0) { // empty segment == current directory
                    candidate[0] = '.';
                    segmentLength = 1;
                } else if (segmentLength < sizeof(candidate)) {
                    for (size_t i = 0; i < segmentLength; ++i)
                        candidate[i] = segment[i];
                } else {
                    segmentLength = 0; // too long — skip this segment
                }
                if (segmentLength) {
                    size_t pos = segmentLength;
                    if (pos + 1 < sizeof(candidate))
                        candidate[pos++] = '/';
                    for (size_t bi = 0; path[bi] && pos + 1 < sizeof(candidate); ++bi)
                        candidate[pos++] = path[bi];
                    candidate[pos] = '\0';
                    if (::access(candidate, X_OK) == 0)
                        ::execve(candidate, execArguments, childEnvironmentPointers);
                }
                segment = (*segmentEnd == ':') ? segmentEnd + 1 : segmentEnd;
            }
        }
        _exit(127); // every exec attempt failed
    }

    // Parent: wait out the child's exec (or its exit after every exec failed).
    execReportPipe.write = {};
    for (char discarded = 0;;) {
        const ssize_t count = ::read(execReportPipe.read.get(), &discarded, 1);
        if (count == 0 || (count < 0 && errno != EINTR))
            break;
    }
    if (setNonBlocking(masterFileDescriptor.get()) != 0) {
        abandonSpawnedChild(childProcessID, std::move(masterFileDescriptor));
        return {};
    }
    // Before the Stream exists, while the descriptor is open: ptsname needs it.
    // A refusal leaves recordedPTY empty, so close clears nothing.
    const bool shouldRecordSession = options.sessionRecord == SessionRecord::AsLogin
        && loginSessionKind() == LoginSessionKind::ByRecordingThePTY;
    std::string recordedPTY;
    if (shouldRecordSession) {
        if (const char* replica = ::ptsname(masterFileDescriptor.get())) {
            if (registerLoginSession(replica))
                recordedPTY = replica;
        }
    }

    // The concrete Stream has a vtable, so it must be `new`'d, never calloc'd.
    ProcessExitSource exitSource = ProcessExitSource::DirectChild;
#if defined(__APPLE__)
    if (arguments.front() == "/usr/bin/login") {
        exitSource = ProcessExitSource::LoginShell;
    }
#endif
    Stream* stream = new (std::nothrow) PTYStream(std::move(masterFileDescriptor), childProcessID, std::move(recordedPTY), exitSource);
    if (!stream) {
        abandonSpawnedChild(childProcessID, std::move(masterFileDescriptor));
        return {};
    }
    return std::unique_ptr<Stream>(stream);
}

namespace {

enum class PTYAdoptionFlags {
    SetNonBlocking,
    RequireNonBlocking,
};

std::unique_ptr<Stream> adoptPTYWithPolicy(int masterFileDescriptor, ProcessID claimedOwnerProcessID, PTYAdoptionFlags flagsPolicy)
{
    UnixFileDescriptor adoptedMasterFileDescriptor(masterFileDescriptor);
    if (!adoptedMasterFileDescriptor)
        return {};
    // The fd crosses a trust boundary (SCM_RIGHTS from a peer). Require a PTY
    // master (char device answering TIOCGWINSZ) so a regular file, socket, or pipe
    // can't be driven as terminal I/O.
    struct stat fileStatus;
    struct winsize windowSize;
    if (::fstat(adoptedMasterFileDescriptor.get(), &fileStatus) != 0 || !S_ISCHR(fileStatus.st_mode) || ::ioctl(adoptedMasterFileDescriptor.get(), TIOCGWINSZ, &windowSize) != 0) {
        return {};
    }
    // Ownership check: a valid PTY master proves nothing about who owns it
    // — a same-uid peer could pass an fd to an UNRELATED session's replica.
    // Require the master's session leader (TIOCGSID) to be the claimed owner or
    // one of its descendants before trusting any fact from this fd. Fails closed
    // on any lookup error (pid_t <= 0) or unverifiable ancestry.
    const pid_t sessionLeader = ::tcgetsid(adoptedMasterFileDescriptor.get());
    if (sessionLeader <= 0 || !PAL::isProcessAncestorOfSession(claimedOwnerProcessID, static_cast<ProcessID>(sessionLeader))) {
        return {};
    }
    const int flags = ::fcntl(adoptedMasterFileDescriptor.get(), F_GETFL, 0);
    if (flags < 0 || (flagsPolicy == PTYAdoptionFlags::RequireNonBlocking && !(flags & O_NONBLOCK)))
        return {};
    if (!(flags & O_NONBLOCK) && setNonBlocking(adoptedMasterFileDescriptor.get()) != 0) {
        return {};
    }
    // childProcessID = -1: the daemon owns only the master fd; the app stays the
    // shell's parent + reaper, so ~PTYStream must not SIGHUP/waitpid the child (its
    // kill block is gated on childProcessID > 0). Master EOF signals shell exit.
    // What the shell is doing is read off the session the master names
    // (`sessionLeader`), which the check above verified.
    Stream* stream = flagsPolicy == PTYAdoptionFlags::RequireNonBlocking
        ? new (std::nothrow) PTYStream(std::move(adoptedMasterFileDescriptor))
        : new (std::nothrow) PTYStream(std::move(adoptedMasterFileDescriptor), -1);
    if (!stream)
        return {};
    return std::unique_ptr<Stream>(stream);
}

} // namespace

std::unique_ptr<Stream> adoptPTY(int masterFileDescriptor, ProcessID claimedOwnerProcessID)
{
    return adoptPTYWithPolicy(masterFileDescriptor, claimedOwnerProcessID, PTYAdoptionFlags::SetNonBlocking);
}

std::unique_ptr<Stream> preparePTYAdoption(int masterFileDescriptor, ProcessID claimedOwnerProcessID)
{
    return adoptPTYWithPolicy(masterFileDescriptor, claimedOwnerProcessID, PTYAdoptionFlags::RequireNonBlocking);
}

std::optional<PTYAdoptionIdentity> capturePTYAdoptionIdentity(int master, ProcessID owner)
{
    if (master < 0 || owner <= 1)
        return std::nullopt;
    struct stat status {};
    struct winsize size {};
    const auto leader = ::tcgetsid(master);
    if (leader <= 1 || ::fstat(master, &status) != 0 || !S_ISCHR(status.st_mode)
        || ::ioctl(master, TIOCGWINSZ, &size) != 0)
        return std::nullopt;
    const auto before = ptyProcessSnapshot(static_cast<ProcessID>(leader));
    if (!before || before->birth == ProcessBirthIdentity {} || before->processGroupID != leader
        || !isProcessAncestorOfSession(owner, static_cast<ProcessID>(leader)))
        return std::nullopt;
    const auto after = ptyProcessSnapshot(static_cast<ProcessID>(leader));
    const auto flags = ::fcntl(master, F_GETFL, 0);
    if (!after || after->birth != before->birth || after->processGroupID != leader
        || ::tcgetsid(master) != leader || flags < 0 || !(flags & O_NONBLOCK))
        return std::nullopt;
    return PTYAdoptionIdentity { .sessionLeader = static_cast<ProcessID>(leader), .birth = before->birth, .device = static_cast<std::uint64_t>(status.st_dev), .inode = static_cast<std::uint64_t>(status.st_ino) };
}

bool validatePTYAdoptionIdentity(int master, ProcessID owner, const PTYAdoptionIdentity& expected)
{
    const auto current = capturePTYAdoptionIdentity(master, owner);
    return current && *current == expected;
}

std::unique_ptr<Stream> inheritPTY(int masterFileDescriptor, ProcessID expectedSessionLeader, std::span<const ProcessID> acceptableParents)
{
    UnixFileDescriptor master(masterFileDescriptor);
    if (!master || expectedSessionLeader <= 1 || acceptableParents.empty())
        return {};
    struct stat fileStatus {};
    struct winsize windowSize {};
    if (::fstat(master.get(), &fileStatus) != 0 || !S_ISCHR(fileStatus.st_mode)
        || ::ioctl(master.get(), TIOCGWINSZ, &windowSize) != 0)
        return {};
    const auto snapshot = ptyProcessSnapshot(expectedSessionLeader);
    if (!snapshot || snapshot->processGroupID != expectedSessionLeader
        || std::ranges::find(acceptableParents, snapshot->parentProcessID) == acceptableParents.end()
        || ::tcgetsid(master.get()) != expectedSessionLeader)
        return {};
    const auto current = ptyProcessSnapshot(expectedSessionLeader);
    if (!current || current->birth != snapshot->birth || current->processGroupID != expectedSessionLeader
        || std::ranges::find(acceptableParents, current->parentProcessID) == acceptableParents.end()
        || setNonBlocking(master.get()) != 0)
        return {};
    return std::unique_ptr<Stream>(new (std::nothrow) PTYStream(std::move(master), expectedSessionLeader, {}, ProcessExitSource::DirectChild, snapshot->birth));
}

bool PTYStream::inheritedProcessIsCurrent() const
{
    if (!m_masterFileDescriptor || !m_inheritedBirth || m_child->hasExited() || m_child->processID() <= 1)
        return false;
    return POSIX::matchesInheritedPTY(m_masterFileDescriptor.get(), m_child->processID(), *m_inheritedBirth);
}

} // namespace Ubiquitty::PAL

bool Ubiquitty::POSIX::matchesInheritedPTY(int master, PAL::ProcessID leader, const PAL::ProcessBirthIdentity& birth)
{
    if (master < 0 || leader <= 1)
        return false;
    const auto current = PAL::ptyProcessSnapshot(leader);
    return current && current->birth == birth && current->processGroupID == leader && ::tcgetsid(master) == leader;
}
