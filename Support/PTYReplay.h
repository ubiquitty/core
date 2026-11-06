// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// PTYReplay.h — small benchmark-only helper for replaying bytes through a raw
// forkpty child and draining the master into Core. This measures the kernel PTY
// / read-loop axis while staying headless; production frontends use
// Platform/PTYStream instead.

#include "Core/TerminalOperations.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <span>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#else
#include <util.h>
#endif

namespace Ubiquitty::Bench {

/// What one replay measured about the kernel's side of the PTY.
///
/// `readCalls` and the read-size range are the point of the measurement: the
/// same byte total arriving in a few large reads or many small ones is a
/// different cost, and only the kernel decides which. `minReadBytes` is 0 when
/// no read returned data, which is indistinguishable from a genuine zero-byte
/// read only because there is no such thing.
struct PTYReplayResult {
    bool ok { false };
    std::size_t bytesRead { 0 };
    std::size_t readCalls { 0 };
    std::size_t minReadBytes { 0 };
    std::size_t maxReadBytes { 0 };
};

namespace Detail {

constexpr int ptyDrainTimeoutMilliseconds = 10000;

/// Writes a whole buffer, resuming after short writes and interrupts.
///
/// A PTY write is short whenever the slave's buffer fills, so a single `write`
/// is never the whole stream and treating it as one silently truncates the
/// corpus the replay is measuring.
///
/// - Parameters:
///   - fileDescriptor: The descriptor to write to.
///   - data: The first byte to write.
///   - length: How many bytes to write.
/// - Returns: `true` when every byte was written; `false` on the first error
///   other than `EINTR`.
inline bool writeAll(int fileDescriptor, const char* data, std::size_t length)
{
    while (length > 0) {
        ssize_t n = ::write(fileDescriptor, data, length);
        if (n > 0) {
            data += n;
            length -= static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

/// Puts a descriptor into non-blocking mode, preserving its other flags.
///
/// `drainPTY` reads until `EAGAIN` to learn how much the kernel had buffered;
/// on a blocking descriptor that read waits for the next byte instead of
/// reporting exhaustion, which turns the read-size histogram into a measure of
/// the writer's timing rather than the kernel's buffering.
///
/// - Parameter fileDescriptor: The descriptor to modify.
/// - Returns: `true` when `O_NONBLOCK` is set.
inline bool setNonBlocking(int fileDescriptor)
{
    int flags = ::fcntl(fileDescriptor, F_GETFL, 0);
    if (flags < 0)
        return false;
    return ::fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

/// Whether the parent still expects the child to finish on its own.
enum class ChildTermination {
    /// The drain ended normally, so the child has already written everything
    /// and only has to be collected.
    Await,
    /// The parent gave up on the drain. Nothing will read what the child still
    /// wants to write, so it would block in `write` forever and has to be
    /// signalled before waiting.
    Signal,
};

/// Waits for the replay child to exit, escalating to `SIGKILL` if it will not.
///
/// Polls for half a second before killing, so a child still flushing its last
/// write is not counted as a hang. A killed child always reports failure: it
/// did not finish writing the corpus, so whatever the drain measured covers
/// less than the caller asked for.
///
/// - Parameters:
///   - child: The process to reap. A non-positive value means no child was
///     started and succeeds.
///   - termination: Whether to signal the child before waiting.
/// - Returns: `true` only when the child exited with status 0 of its own
///   accord.
inline bool reapChild(pid_t child, ChildTermination termination)
{
    if (child <= 0)
        return true;
    if (termination == ChildTermination::Signal)
        ::kill(child, SIGTERM);
    for (int attempt = 0; attempt < 50; ++attempt) {
        int status = 0;
        pid_t rc = ::waitpid(child, &status, WNOHANG);
        if (rc == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (rc < 0 && errno != EINTR)
            return false;
        ::usleep(10000);
    }
    ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return false;
}

/// Reads the PTY master until the slave closes, feeding every byte to `term`.
///
/// Polls, then reads until `EAGAIN`, so each poll wakeup contributes as many
/// reads as the kernel had buffered rather than one — that inner loop is what
/// makes the read-size range meaningful.
///
/// `EIO` on a master whose slave has closed is how BSD and Linux both spell
/// end of stream, so it reports success; a poll that times out reports failure,
/// because a stalled child leaves the corpus partly unparsed and a partial
/// measurement is worse than none.
///
/// - Parameters:
///   - masterFileDescriptor: The PTY master, already non-blocking.
///   - term: The terminal receiving the bytes.
///   - result: Filled with the byte and read-call counts, overwriting whatever
///     it held.
/// - Returns: `true` when the stream ended; `false` on timeout or an
///   unrecoverable read error.
inline bool drainPTY(int masterFileDescriptor, Ubiquitty::Terminal* term, PTYReplayResult& result)
{
    char buffer[64 * 1024];
    result.bytesRead = 0;
    result.readCalls = 0;
    result.minReadBytes = 0;
    result.maxReadBytes = 0;
    for (;;) {
        struct pollfd pfd;
        pfd.fd = masterFileDescriptor;
        pfd.events = POLLIN | POLLHUP | POLLERR;
        pfd.revents = 0;
        int pollResult = ::poll(&pfd, 1, ptyDrainTimeoutMilliseconds);
        if (pollResult < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (pollResult == 0)
            return false;

        for (;;) {
            ssize_t n = ::read(masterFileDescriptor, buffer, sizeof(buffer));
            if (n > 0) {
                const std::size_t bytes = static_cast<std::size_t>(n);
                result.bytesRead += bytes;
                result.readCalls++;
                if (result.minReadBytes == 0 || bytes < result.minReadBytes)
                    result.minReadBytes = bytes;
                if (bytes > result.maxReadBytes)
                    result.maxReadBytes = bytes;
                Ubiquitty::processBytes(term, std::string_view(buffer, static_cast<std::size_t>(n)));
                continue;
            }
            if (n == 0)
                return true;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EIO)
                return true; // BSD/Linux PTY EOF after slave closes.
            return false;
        }

        if (pfd.revents & (POLLHUP | POLLERR))
            return true;
    }
}

} // namespace Detail

/// Replays a byte stream through a real PTY and parses it back into `term`.
///
/// Forks a child whose stdout is the PTY slave and which writes `stream`
/// `repeats` times, then drains the master in the parent. The kernel is
/// therefore in the loop — its buffering decides the read sizes, which is the
/// axis this measures and the one an in-process `processBytes` call cannot
/// reach.
///
/// The line discipline is raw: `ONLCR` would rewrite every LF in the corpus on
/// the way through and the parser would see a stream the caller never wrote.
///
/// - Parameters:
///   - term: The terminal receiving the replayed bytes.
///   - stream: The corpus. Empty means nothing to measure.
///   - repeats: How many times the child writes the corpus. Must be positive.
///   - rows: Window height reported to the slave.
///   - columns: Window width reported to the slave.
/// - Returns: The measurement, with `ok` false when any stage failed — an
///   unusable result rather than a partial one.
inline PTYReplayResult replayPTY(Ubiquitty::Terminal* term, std::span<const char> stream,
    int repeats, int rows, int columns)
{
    PTYReplayResult result;
    if (!term || stream.empty() || repeats <= 0)
        return result;

    struct termios raw;
    std::memset(&raw, 0, sizeof(raw));
    cfmakeraw(&raw);
    raw.c_cflag |= CREAD | CLOCAL;
    cfsetispeed(&raw, B38400);
    cfsetospeed(&raw, B38400);

    struct winsize windowSize;
    std::memset(&windowSize, 0, sizeof(windowSize));
    windowSize.ws_row = static_cast<unsigned short>(rows);
    windowSize.ws_col = static_cast<unsigned short>(columns);

    int masterFileDescriptor = -1;
    pid_t child = ::forkpty(&masterFileDescriptor, nullptr, &raw, &windowSize);
    if (child < 0)
        return result;
    if (child == 0) {
        for (int i = 0; i < repeats; ++i) {
            if (!Detail::writeAll(STDOUT_FILENO, stream.data(), stream.size()))
                _exit(1);
        }
        _exit(0);
    }

    const bool didDrain = Detail::setNonBlocking(masterFileDescriptor)
        && Detail::drainPTY(masterFileDescriptor, term, result);
    ::close(masterFileDescriptor);
    const bool didExitCleanly = Detail::reapChild(child,
        didDrain ? Detail::ChildTermination::Await : Detail::ChildTermination::Signal);
    result.ok = didDrain && didExitCleanly;
    return result;
}

} // namespace Ubiquitty::Bench
