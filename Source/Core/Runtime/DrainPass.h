// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// DrainPass.h — one drain pass: read a PTY to a would-block or its byte budget
// and parse what it gave.
//
// The read loop every host of a Terminal drains by: the local runtime's
// `TerminalRuntime::drainAvailable` and the host daemon's `servicePTY`. A pass
// ends at a would-block or at `TerminalRuntime::drainPassCapacityBytes`, never at
// a short read; where the host's policy asks, it rides a writer's refill gaps
// for a few spins and yields before calling a would-block the end of a burst;
// it parses under the host's model lock a bite at a time; and it stops the
// parser at each DEC 2026 close so the owner can take that frame. One loop, so
// every host reads and parses the same way; a host whose thread is its own
// rides the gaps, and one whose loop is shared (the daemon) does not, because
// a ride holds that loop for as long as the writer keeps refilling.
//
// What a pass is followed by is the owner's: a frontend consumes the
// terminal's events, the daemon relays them to its clients.

#include "Base/Types.h"

#include <cstdint>
#include <limits>
#include <span>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::PAL {
class Stream;
}

namespace Ubiquitty::Runtime {

class DrainHost;

/// The bytes one read syscall asks for.
inline constexpr int drainReadBufferBytes = 64 * 1024;

/// What the owner of a pass adds to the shared read loop.
class DrainPassOwner {
public:
    virtual ~DrainPassOwner() = default;

    /// Sees each read before any of it is parsed.
    ///
    /// - Parameter bytes: The read.
    /// - Returns: A negative value to parse the whole read and keep reading;
    ///   otherwise how many leading bytes to parse before the pass ends, the
    ///   rest left for whoever takes the stream from here.
    virtual int judgeRead(std::span<const char> bytes) = 0;

    /// Notes the first parse bite of a paced read, under the model lock.
    ///
    /// The owner can start its pending frame's age without adding a lock
    /// acquisition. Disabled time budgeting does not call this hook.
    virtual void noteParseBegin() { }

    /// Drains the replies the parser queued, after it stopped at reply
    /// pressure. Called with the model lock held.
    virtual void flushReplies() = 0;

    /// Notes a DEC 2026 block closing inside the pass. Called with the model
    /// lock held.
    virtual void noteSyncClear() = 0;

    /// Takes, or declines, the frame a DEC 2026 block closed with more of the
    /// read still to parse: the close is that frame's only instant. Called
    /// with the model lock held, after `noteSyncClear`.
    ///
    /// - Parameter host: The pass's drain host.
    virtual void publishMidReadSyncFrame(DrainHost& host) = 0;

    /// Notes that a read is about to begin. An owner that settles input by
    /// the time a read that found nothing began reads its clock here; the
    /// pass's last read is the one that ended it.
    virtual void noteReadBegin() { }
};

/// How a pass reads.
struct DrainPassPolicy {
    /// Whether a would-block after bytes is waited out for a few spins and
    /// yields before the pass ends; false ends it at the first one.
    bool doesRideBurst = true;
    /// How many bytes are parsed per acquisition of the model lock.
    int modelLockBiteBytes = 4 * 1024;
    /// An owner may lease a smaller read budget, for example one byte to
    /// finish a handover parser sequence. The ordinary pass cap still applies.
    int maxBytes = std::numeric_limits<int>::max();
    /// Presentation deadline checked after a whole read is parsed. Zero
    /// leaves an unpaced host on the byte budget alone.
    std::uint32_t maxMicroseconds = 0;
};

/// Why the reader stopped. A budget yield makes no claim that input is quiet.
enum class DrainPassEnd : std::uint8_t {
    None,
    WouldBlock,
    ByteBudget,
    TimeBudget,
    OwnerFence,
    StreamEnd,
};

/// How one pass ended.
struct DrainPassResult {
    /// Bytes read, including any the owner kept from the parser.
    int bytesDrained = 0;
    DrainPassEnd end = DrainPassEnd::None;
    /// The pass ended at a would-block: the writer has paused.
    bool didCatchUp = false;
    /// The stream ended, cleanly or not.
    bool didReachEnd = false;
    /// The end was a fatal read error rather than an end of file.
    bool isFatal = false;
    /// The stream's last error when fatal.
    int lastError = 0;
    /// The status the last read returned when the pass ended without bytes.
    int endStatus = 0;
};

/// Runs one drain pass.
///
/// - Parameters:
///   - terminal: The terminal the bytes are parsed into.
///   - stream: The PTY to read.
///   - host: Takes the model lock around each bite; handed to the owner with
///     each DEC 2026 frame completed inside a read.
///   - owner: Judges each read, flushes replies and counts 2026 closes.
///   - policy: How the pass reads.
/// - Returns: How the pass ended.
[[nodiscard]] DrainPassResult runDrainPass(Terminal& terminal, PAL::Stream& stream, DrainHost& host, DrainPassOwner& owner, const DrainPassPolicy& policy);

} // namespace Ubiquitty::Runtime
