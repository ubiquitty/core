// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_stream_double.cpp — the shared PAL test doubles
// (Tests/Platform/StreamDouble.h).
//
// Tests across Tests/Host, Tests/Runtime and Tests/UbiquittyKit share these
// doubles in place of hand-rolled Stream fakes. This pins the union they need
// so the doubles can be relied on rather than re-derived: scripted inbound
// bytes, quantum capping, both refill policies, reply capture, EOF and error
// injection, window capture, probe-counting ProcessFacts, and destruction
// accounting.
//
// The deterministic refill policy is the one that matters most here. A bench can
// afford a wall-clock gap; a correctness test cannot, so refillAfterEagainCount
// reproduces the mid-burst EAGAIN ladder with no clock at all and therefore
// cannot flake under load.

#include "StreamDouble.h"

#include <array>
#include <cstdio>
#include <optional>
#include <span>
#include <string>

using Ubiquitty::PAL::StreamStatus;
using Ubiquitty::TestDoubles::DrainHostDouble;
using Ubiquitty::TestDoubles::StreamDouble;

namespace {

int readInto(StreamDouble& stream, std::span<char> buffer) { return stream.read(buffer); }

TEST(scriptedReadsAndQuantumCapacity)
{
    StreamDouble stream;
    stream.pushInbound("abcdefghij");
    stream.setQuantumBytes(4);
    std::array<char, 64> buffer {};

    CHECK(readInto(stream, buffer) == 4);
    CHECK(std::string(buffer.data(), 4) == "abcd");
    CHECK(readInto(stream, buffer) == 4);
    CHECK(std::string(buffer.data(), 4) == "efgh");
    CHECK(readInto(stream, buffer) == 2); // partial final quantum
    CHECK(std::string(buffer.data(), 2) == "ij");
    // Exhausted without an EOF script: Again, not EndOfFile, so a drain loop
    // treats it as a quiet gap rather than a closed peer.
    CHECK(readInto(stream, buffer) == static_cast<int>(StreamStatus::Again));
    stream.signalEndOfFile();
    CHECK(readInto(stream, buffer) == static_cast<int>(StreamStatus::EndOfFile));
    CHECK(stream.readCalls == 5);
}

TEST(deterministicRefillLadder)
{
    StreamDouble stream;
    stream.pushInbound("XXXXYYYY");
    stream.setQuantumBytes(4);
    stream.refillAfterEagainCount(2); // exactly two Agains between quanta
    std::array<char, 64> buffer {};

    // The first quantum is available immediately: no read has consumed anything,
    // so the simulated writer has nothing to refill yet.
    CHECK(readInto(stream, buffer) == 4);
    CHECK(readInto(stream, buffer) == static_cast<int>(StreamStatus::Again));
    CHECK(readInto(stream, buffer) == static_cast<int>(StreamStatus::Again));
    CHECK(readInto(stream, buffer) == 4);
    CHECK(std::string(buffer.data(), 4) == "YYYY");
    CHECK(stream.againCount == 2);
}

TEST(repliesAreCaptured)
{
    StreamDouble stream;
    const std::string reply = "\x1b[0n";
    CHECK(stream.write({ reply.data(), reply.size() }) == static_cast<int>(reply.size()));
    const std::string more = "\x1b[?1;2c";
    CHECK(stream.writeAll({ more.data(), more.size() }) == static_cast<int>(more.size()));
    CHECK(stream.written == reply + more);
    CHECK(stream.writeCalls == 2);
}

TEST(theWriteSideCanShortWriteAndRefuse)
{
    // A pty write is not all-or-nothing: it takes what fits and refuses the
    // rest, and once the child is gone it refuses outright. A caller that
    // treats a partial write as complete loses the tail, so the double has to
    // be able to produce both — and its own budget has to be exact, or every
    // test built on it asserts against the wrong split.
    StreamDouble stream;
    stream.acceptBytesThenRefuse(2, StreamStatus::Again);
    const std::string reply = "\x1b[0n";
    CHECK(stream.write({ reply.data(), reply.size() }) == 1); // one byte per write
    CHECK(stream.write({ reply.data() + 1, reply.size() - 1 }) == 1);
    CHECK(stream.write({ reply.data() + 2, reply.size() - 2 }) == static_cast<int>(StreamStatus::Again));
    CHECK(stream.written == "\x1b[");

    // Recovery takes the rest, so a test can show that a deferred payload came
    // back rather than having been dropped.
    stream.allowWrites();
    CHECK(stream.write({ reply.data() + 2, reply.size() - 2 }) == 2);
    CHECK(stream.written == reply);

    // A budget of zero refuses from the first write, with whichever status the
    // caller asked for.
    StreamDouble gone;
    gone.acceptBytesThenRefuse(0, StreamStatus::Closed);
    CHECK(gone.write({ reply.data(), reply.size() }) == static_cast<int>(StreamStatus::Closed));
    CHECK(gone.written.empty());
    CHECK(gone.writeCalls == 1);
}

TEST(errorAndWindowAndBackpressure)
{
    StreamDouble stream;
    stream.pushInbound("data");
    stream.failWith(5 /* EIO */);
    std::array<char, 16> buffer {};
    CHECK(readInto(stream, buffer) == static_cast<int>(StreamStatus::Fatal));
    CHECK(stream.lastError() == 5);
    // The injected failure is one-shot: the scripted bytes survive it.
    CHECK(readInto(stream, buffer) == 4);

    stream.setWindowSize(202, 49, 1616, 833);
    CHECK(stream.columns == 202);
    CHECK(stream.rows == 49);
    CHECK(stream.xPixels == 1616);
    CHECK(stream.yPixels == 833);

    CHECK(!stream.hasPendingWrite());
    stream.setPendingWrite(true);
    CHECK(stream.hasPendingWrite());
    CHECK(stream.drainPendingWrite());
}

// Under queue emulation the double queues owned writes through the real owned
// FIFO, as the PTY stream does: an owner's bytes wait behind a terminal reply
// already queued, and withdrawing that owner removes its suffix alone. Owner
// zero is no owner; it refuses without reaching the ordinary writer.
TEST(anOwnedWriteQueuesBehindAReplyAndIsWithdrawnAlone)
{
    StreamDouble stream;
    stream.queueWriteAll();
    stream.acceptBytesThenRefuse(0, StreamStatus::Again);
    CHECK(stream.writeAll(std::span<const char>("reply", 5)) == 0);
    const std::optional<int> admitted = stream.writeAllOwned(1, std::span<const char>("input", 5));
    CHECK(admitted.has_value() && *admitted >= 0);
    CHECK(stream.pendingOwnedWriteBytes(1) == std::optional<std::size_t> { 5 });
    CHECK_EQ(stream.pendingWriteBytes(), 10u);

    const auto calls = stream.writeCalls;
    CHECK(!stream.writeAllOwned(0, {}));
    CHECK(!stream.discardPendingWrite(0));
    CHECK_EQ(stream.writeCalls, calls);

    CHECK(stream.discardPendingWrite(1) == std::optional<std::size_t> { 5 });
    CHECK_EQ(stream.pendingWriteBytes(), 5u);
    stream.allowWrites();
    CHECK(!stream.drainPendingWrite());
    CHECK_STR_EQ(stream.written, std::string("reply"));
}

TEST(processFactsAreScriptedAndCounted)
{
    StreamDouble stream;
    stream.facts.name = "-zsh";
    stream.facts.group = 4242;
    stream.facts.currentDirectory = "/tmp";
    CHECK(stream.process().foregroundName() == "-zsh");
    CHECK(stream.process().foregroundName() == "-zsh");
    CHECK(stream.process().foregroundProcessGroup() == 4242);
    CHECK(stream.process().foregroundCurrentDirectory() == "/tmp");
    // The counters are the point: Tests/Host asserts a probe budget, not a value.
    CHECK(stream.facts.nameProbes == 2);
    CHECK(stream.facts.groupProbes == 1);
    CHECK(stream.facts.currentDirectoryProbes == 1);
}

TEST(destructionIsAccounted)
{
    int destroyed = 0;
    {
        StreamDouble stream(&destroyed);
        CHECK(destroyed == 0);
    }
    CHECK(destroyed == 1);
}

TEST(drainHostIsInertByDefault)
{
    DrainHostDouble host;
    host.lockModel();
    host.onSyncFrameReadyLocked(Ubiquitty::Runtime::SyncFrameReason::Boundary);
    host.unlockModel();
    CHECK(host.lockAcquisitions == 1);
    CHECK(host.syncFrames == 1);
    // No frame period given, so no contending thread was ever started.
    CHECK(host.framesRendered() == 0);
}

} // namespace

UBIQUITTY_TEST_MAIN()
