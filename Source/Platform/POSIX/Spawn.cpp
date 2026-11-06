// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "PAL/Stream.h"

#include "Core/Session/LaunchContext.h"

#include "PAL/Socket.h"
#include "PAL/UnixFileDescriptor.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" char** environ;

namespace Ubiquitty::PAL {

namespace {

// Grace between asking a child to exit and forcing it. A child that has not
// gone in this long is not going to, and the caller has already stopped reading
// it — holding the descriptor longer only delays the reap.
constexpr int terminateGraceMilliseconds = 2000;

class PosixChildProcess final : public ChildProcess {
public:
    explicit PosixChildProcess(pid_t processID)
        : m_processID(processID)
    {
    }

    ~PosixChildProcess() override { terminate(); }

    void terminate() override
    {
        const pid_t processID = m_processID;
        if (processID <= 0)
            return;
        m_processID = -1;

        ::kill(processID, SIGTERM);
        if (::waitpid(processID, nullptr, WNOHANG) == processID)
            return; // exited promptly, which is the common case

        // Still alive: escalate and reap on a detached thread, so a caller
        // tearing down its transport never waits on a child that is wedged.
        std::thread([processID] {
            for (int elapsed = 0; elapsed < terminateGraceMilliseconds; ++elapsed) {
                const pid_t reaped = ::waitpid(processID, nullptr, WNOHANG);
                if (reaped == processID || reaped < 0)
                    return;
                ::usleep(1000);
            }
            ::kill(processID, SIGKILL);
            ::waitpid(processID, nullptr, 0);
        }).detach();
    }

private:
    pid_t m_processID = -1;
};

// Sets the mode of the terminal behind `descriptor` and discards nothing queued
// in either direction.
//
// The terminal belongs to a live session: what its user typed ahead and what
// the far side wrote past the agent's last read are the session's bytes, and
// the caller reads on once the agent is done. So the mode changes with TCSANOW.
// TCSAFLUSH discards the unread input, and TCSADRAIN waits for a reader the
// caller has stopped.
//
// On Darwin that is not enough for a pseudo-terminal master: the controller side
// discards its unread output on every mode change, whatever the action. The
// replica takes the same mode without that flush, so a master is reached
// through its replica, opened for the one call. O_NONBLOCK, because a close that
// turns out to be the replica's last flushes instead of waiting for a master
// nobody is reading. A replica that cannot be opened leaves the master itself,
// which still sets the mode: a terminal left raw costs the session more than
// the far side's unread output does.
bool setTerminalMode(int descriptor, const struct termios& mode)
{
#if defined(__APPLE__)
    // TIOCPTYGNAME's argument size, terminator included.
    std::array<char, 128> replicaPath {};
    if (::ioctl(descriptor, TIOCPTYGNAME, replicaPath.data()) == 0) {
        UnixFileDescriptor replica(::open(replicaPath.data(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
        if (replica.isValid())
            return ::tcsetattr(replica.get(), TCSANOW, &mode) == 0;
    }
#endif
    return ::tcsetattr(descriptor, TCSANOW, &mode) == 0;
}

// A terminal lent to an agent: its mode and file-status flags, handed back on
// every return once either has changed.
//
// More than one step can fail after the terminal has changed, and the caller
// reads on whichever it was. A terminal left raw garbles the session, and a
// master left blocking stalls the endpoint that polls it.
class TerminalLoan final {
public:
    explicit TerminalLoan(int descriptor)
        : m_descriptor(descriptor)
    {
    }

    ~TerminalLoan()
    {
        if (m_didClearNonBlocking)
            (void)::fcntl(m_descriptor, F_SETFL, m_originalFlags);
        if (m_didChangeMode)
            (void)setTerminalMode(m_descriptor, m_originalMode);
    }

    TerminalLoan(const TerminalLoan&) = delete;
    TerminalLoan& operator=(const TerminalLoan&) = delete;

    /// Puts the terminal in raw mode when the descriptor is a terminal.
    ///
    /// - Returns: False only when the descriptor is a terminal that did not
    ///   take the raw mode.
    [[nodiscard]] bool enterRawMode()
    {
        struct termios mode;
        if (::tcgetattr(m_descriptor, &mode) != 0)
            return true;
        m_originalMode = mode;
        ::cfmakeraw(&mode);
        // Marked before the attempt, so a mode applied part of the way is
        // handed back too.
        m_didChangeMode = true;
        return setTerminalMode(m_descriptor, mode);
    }

    /// Clears O_NONBLOCK on the descriptor's open-file description.
    ///
    /// - Returns: False only when the flag is set and could not be cleared.
    [[nodiscard]] bool clearNonBlocking()
    {
        const int flags = ::fcntl(m_descriptor, F_GETFL);
        if (flags < 0 || (flags & O_NONBLOCK) == 0)
            return true;
        if (::fcntl(m_descriptor, F_SETFL, flags & ~O_NONBLOCK) != 0)
            return false;
        m_originalFlags = flags;
        m_didClearNonBlocking = true;
        return true;
    }

private:
    int m_descriptor = -1;
    struct termios m_originalMode {};
    int m_originalFlags = 0;
    bool m_didChangeMode = false;
    bool m_didClearNonBlocking = false;
};

} // namespace

PipedCommand spawnPipedCommand(std::string_view command, CommandStandardError standardError)
{
    PipedCommand result;

    UnixPipe inPipe = makeUnixPipe(); // read: child reads, write: we write
    UnixPipe outPipe = makeUnixPipe(); // read: we read, write: child writes
    UnixPipe errorPipe;
    if (standardError == CommandStandardError::Capture)
        errorPipe = makeUnixPipe();
    if (!inPipe || !outPipe || (standardError == CommandStandardError::Capture && !errorPipe))
        return result;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, inPipe.read.get(), STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outPipe.write.get(), STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errorPipe ? errorPipe.write.get() : STDERR_FILENO, STDERR_FILENO);
    if (errorPipe) {
        posix_spawn_file_actions_addclose(&actions, errorPipe.read.get());
        posix_spawn_file_actions_addclose(&actions, errorPipe.write.get());
    }
    posix_spawn_file_actions_addclose(&actions, inPipe.read.get());
    posix_spawn_file_actions_addclose(&actions, inPipe.write.get());
    posix_spawn_file_actions_addclose(&actions, outPipe.read.get());
    posix_spawn_file_actions_addclose(&actions, outPipe.write.get());

    // Every descriptor beyond the three standard ones stays with this process.
    // The parent here can be the app or the daemon, and what it holds open —
    // PTY masters, the control socket, attached clients — is exactly what a
    // shell command must not inherit: a child holding another session's master
    // can read and write that session. The pipe ends above are re-closed by
    // the default harmlessly; the dup2 targets survive it.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
#if defined(__APPLE__)
    short spawnFlags = 0;
    posix_spawnattr_getflags(&attributes, &spawnFlags);
    posix_spawnattr_setflags(&attributes, static_cast<short>(spawnFlags | POSIX_SPAWN_CLOEXEC_DEFAULT));
#elif defined(__GLIBC__) && ((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
#endif

    const std::string commandLine(command);
    char shellPath[] = "/bin/sh";
    char shellFlag[] = "-c";
    char* arguments[] = { shellPath, shellFlag, const_cast<char*>(commandLine.c_str()), nullptr };

    // The command inherits this process's environment except its credentials:
    // a token handed to this process is not the command's, and ssh, a common
    // command here, forwards whatever its configuration's SendEnv names.
    std::vector<char*> environment;
    for (char** entry = environ; *entry; ++entry) {
        if (!Ubiquitty::Session::isCredentialEnvironmentEntry(*entry))
            environment.push_back(*entry);
    }
    environment.push_back(nullptr);

    pid_t processID = -1;
    const int spawnResult = ::posix_spawn(&processID, shellPath, &actions, &attributes, arguments, environment.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);

    inPipe.read.reset(); // the child's ends; this process never uses them
    outPipe.write.reset();

    if (spawnResult != 0)
        return result;

    result.input = adoptByteStream(inPipe.write.release(), ReadinessWaitStrategy::ProbeBeforeWait);
    result.output = adoptByteStream(outPipe.read.release(), ReadinessWaitStrategy::ProbeBeforeWait);
    if (errorPipe)
        result.standardError = adoptByteStream(errorPipe.read.release(), ReadinessWaitStrategy::ProbeBeforeWait);
    if (!result.input || !result.output || (standardError == CommandStandardError::Capture && !result.standardError)) {
        // The child is spawned but unreachable, so reap it rather than leaving
        // an orphan whose output goes to a descriptor nobody holds.
        PosixChildProcess(processID).terminate();
        result.input.reset();
        result.output.reset();
        return result;
    }
    result.process = std::make_unique<PosixChildProcess>(processID);
    return result;
}

int processSpawnWithTerminalAndWait(const char* path, const char* const argv[], const char* workingDirectory, int stdioDescriptor, double limitSeconds)
{
    if (!path || !argv || !workingDirectory || stdioDescriptor < 0)
        return -1;

    // posix_spawn's working-directory action is not portable across the
    // support matrix (_np on some, absent on others), so the chdir happens in
    // a fork — the child runs only async-signal-safe calls up to execve.
    // A CLOEXEC pipe reports an execve failure exactly: the child's write
    // lands before its _exit, and a successful exec leaves no writer. The pipe
    // exists before the terminal changes, so a process out of descriptors fails
    // here with nothing to hand back.
    UnixPipe execReportPipe = makeUnixPipe();
    if (!execReportPipe)
        return -1;
    (void)::fcntl(execReportPipe.read.get(), F_SETFD, FD_CLOEXEC);
    (void)::fcntl(execReportPipe.write.get(), F_SETFD, FD_CLOEXEC);

    // Read before the fork: the child closes everything above its report
    // descriptor, and only close() is guaranteed async-signal-safe there.
    // getdtablesize is the soft RLIMIT_NOFILE, the whole range a descriptor of
    // this process can occupy.
    int descriptorCeiling = ::getdtablesize();
    if (descriptorCeiling < 0)
        descriptorCeiling = 0;

    TerminalLoan terminal(stdioDescriptor);

    // The child speaks a binary file-transfer protocol into the terminal's
    // data stream. The line discipline must not get between the two: cooked
    // mode canonical-buffers the child's handshake until a newline, echo
    // bounces it back up the stream, and both garble the transfer. The
    // terminal is raw for the child's duration and handed back after; the
    // caller's shell reinitializes its own mode on the way back regardless.
    if (!terminal.enterRawMode())
        return -1;

    // The caller's pollable endpoint opened the descriptor O_NONBLOCK. The
    // agent treats an EAGAIN on its first read as fatal, and O_NONBLOCK is a
    // property of the open-file-description — shared by every dup — so the
    // mode is cleared for the child and restored before the endpoint resumes.
    if (!terminal.clearNonBlocking())
        return -1;

    const int reportDescriptor = execReportPipe.write.get();
    const pid_t childPID = ::fork();
    if (childPID < 0)
        return -1;
    if (childPID == 0) {
        // A working directory that cannot be entered fails the spawn rather
        // than running the command somewhere else: the caller named the
        // directory because it is part of what it asked for, and a command
        // that runs in the wrong one reports success for different work.
        if (workingDirectory[0] != '\0' && ::chdir(workingDirectory) != 0) {
            const char failed = 1;
            const ssize_t reported = ::write(reportDescriptor, &failed, 1);
            (void)reported;
            ::_exit(127);
        }
        (void)::dup2(stdioDescriptor, STDIN_FILENO);
        (void)::dup2(stdioDescriptor, STDOUT_FILENO);
        const int nullOutput = ::open("/dev/null", O_WRONLY);
        if (nullOutput >= 0) {
            (void)::dup2(nullOutput, STDERR_FILENO);
            (void)::close(nullOutput);
        }

        // The agent holds the terminal and nothing else of this process's. The
        // caller is the app or the daemon, and a descriptor it opened without
        // close-on-exec — another session's master, an attached client's socket
        // — would otherwise cross into a third-party binary that can read and
        // write it. The report pipe moves to the first descriptor above the
        // standard three, marked close-on-exec again because dup2 clears the
        // mark, so a single sweep past it closes the rest.
        const int childReportDescriptor = STDERR_FILENO + 1;
        if (reportDescriptor != childReportDescriptor
            && (::dup2(reportDescriptor, childReportDescriptor) != childReportDescriptor || ::fcntl(childReportDescriptor, F_SETFD, FD_CLOEXEC) != 0))
            ::_exit(127);
#if defined(__linux__) && defined(SYS_close_range)
        if (::syscall(SYS_close_range, static_cast<unsigned>(childReportDescriptor + 1), ~0u, 0u) != 0) {
            for (int descriptor = childReportDescriptor + 1; descriptor < descriptorCeiling; ++descriptor)
                ::close(descriptor);
        }
#else
        for (int descriptor = childReportDescriptor + 1; descriptor < descriptorCeiling; ++descriptor)
            ::close(descriptor);
#endif

        ::execve(path, const_cast<char* const*>(argv), environ);
        const char failed = 1;
        // Best effort: the parent treats a silent pipe as a failed exec too,
        // and there is nothing a child past execve can do about a short write.
        const ssize_t reported = ::write(childReportDescriptor, &failed, 1);
        (void)reported;
        ::_exit(127);
    }
    execReportPipe.write.reset();

    // Polled rather than blocking, because the thing being waited for is a
    // third-party binary this process cannot fix.
    //
    // Measured: `lsz` stops inside its own terminal-mode setup — `io_mode`, an
    // ioctl that does not return — and a `waitpid` with no options then waits
    // for a process that has no way to finish. The caller stopped ingest for
    // the duration and runs this synchronously, so the whole session stops with
    // it, and on the frontend that is the window's own thread. About one launch
    // in twenty-five here.
    //
    // No signal available to this side separates a wedged agent from a slow
    // one: TIOCOUTQ reads zero on a Darwin pty in every state, tcdrain on the
    // master returns in all of them, and a healthy agent waiting on the far end
    // burns no more processor time than a stuck one. So this does not try to
    // tell them apart. It bounds the wait and says so, which turns a session
    // that never comes back into a transfer that reports it did not finish.
    int status = 0;
    // Three outcomes, kept apart because the caller's answer differs: the child
    // exited and its status means something, the limit ran out, or the wait
    // itself failed and there is no status to read.
    bool didChildExit = false;
    bool didOverrunLimit = false;
    const auto waitStarted = std::chrono::steady_clock::now();
    for (;;) {
        const pid_t reaped = ::waitpid(childPID, &status, WNOHANG);
        if (reaped == childPID) {
            didChildExit = true;
            break;
        }
        // The child is unreachable — already reaped, or never ours. Signalling
        // now would aim at whatever holds that number next, so it does not.
        if (reaped < 0 && errno != EINTR)
            break;
        if (limitSeconds > 0.0) {
            const std::chrono::duration<double> waited = std::chrono::steady_clock::now() - waitStarted;
            if (waited.count() >= limitSeconds) {
                didOverrunLimit = true;
                break;
            }
        }
        // Long enough that a transfer of any length costs a rounding error of
        // processor time, short enough that a quick agent is not held back.
        ::usleep(5000);
    }
    if (didOverrunLimit) {
        // Asked first, then insisted: an agent that can still act on a signal
        // gets to put the terminal down on its way out, and one that cannot is
        // past caring. Reaped either way, so a caller that spawns again does
        // not accumulate zombies.
        (void)::kill(childPID, SIGTERM);
        for (int attempt = 0; attempt < 40 && !didChildExit; ++attempt) {
            ::usleep(5000);
            didChildExit = ::waitpid(childPID, &status, WNOHANG) == childPID;
        }
        if (!didChildExit) {
            (void)::kill(childPID, SIGKILL);
            while (::waitpid(childPID, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    char execFailed = 0;
    const ssize_t reportRead = ::read(execReportPipe.read.get(), &execFailed, 1);
    // A child the limit took is a failure however it was reaped, so the overrun
    // is answered before the status rather than through it: a terminated agent
    // can still be waited into a status, and reading that would report the
    // signal that ended it as a verdict on the transfer.
    const bool didExitNormally = !didOverrunLimit && didChildExit && WIFEXITED(status);
    const bool didSpawnFail = reportRead == 1 && execFailed != 0;
    return didExitNormally && !didSpawnFail ? WEXITSTATUS(status) : -1;
}

} // namespace Ubiquitty::PAL
