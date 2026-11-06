// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_outputqueue.cpp — host tests for the portable lossless write FIFO
// (Source/PAL/OutputQueue.{h,cpp}).
//
// Pins: full accept = no copy/queue; partial accept queues the tail; ordered
// lossless delivery across submit/drain cycles and across interleaved submits;
// "full now" (0) holds bytes for a later drain; fatal (<0) propagates and leaves
// the queue intact. The fake sink has a refillable byte budget modelling a
// non-blocking fd whose buffer drains between calls (0 = full right now).

#include "PAL/OutputQueue.h"
#include "ScopedNothrowAllocationRefusal.h"
#include <cstdio>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <vector>

using Ubiquitty::PAL::OutputQueue;

struct Sink {
    std::string out;
    long budget = (1L << 60); // bytes it can accept before reporting "full"
    bool fatal = false;
    std::size_t calls = 0;
};

static int sinkWrite(void* ctx, std::span<const char> bytes)
{
    Sink* s = static_cast<Sink*>(ctx);
    ++s->calls;
    if (s->fatal)
        return -7;
    long n = static_cast<long>(bytes.size());
    if (n > s->budget)
        n = s->budget;
    if (n <= 0)
        return 0; // full right now
    s->out.append(bytes.data(), static_cast<size_t>(n));
    s->budget -= n;
    return static_cast<int>(n);
}

TEST(testFullAcceptNoQueue)
{
    OutputQueue q;
    Sink s;
    int rc = q.submit(std::span<const char>("hello", 5), sinkWrite, &s);
    CHECK_EQ(rc, 0);
    CHECK_STR_EQ(s.out, "hello");
    // A sink that takes every byte leaves the queue nothing to hold.
    CHECK(!q.hasPending());
}

TEST(pasteRepairIsTransactionalAndKeepsOtherOwnersInOrder)
{
    OutputQueue queue;
    Sink sink;
    sink.budget = 0;
    REQUIRE(queue.submitOwned(2, std::span<const char>("before", 6), sinkWrite, &sink) == 0);
    REQUIRE(queue.submitOwned(1, std::span<const char>("revoked", 7), sinkWrite, &sink) == 0);
    REQUIRE(queue.submit(std::span<const char>("reply", 5), sinkWrite, &sink) == 0);
    REQUIRE(queue.submitOwned(1, std::span<const char>("also revoked", 12), sinkWrite, &sink) == 0);
    REQUIRE(queue.submitOwned(3, std::span<const char>("after", 5), sinkWrite, &sink) == 0);
    CHECK(!queue.repairPending(1, std::span<const char>("too much text", 13)));
    CHECK_EQ(queue.pendingBytes(1), 19u);
    REQUIRE(queue.repairPending(1, std::span<const char>("\x1b[201~", 6)));
    CHECK_EQ(queue.pendingBytes(1), 0u);
    CHECK_EQ(queue.pendingBytes(2), 6u);
    CHECK_EQ(queue.pendingBytes(3), 5u);
    sink.budget = 1000;
    CHECK(queue.drain(sinkWrite, &sink) == 0);
    CHECK_STR_EQ(sink.out, "before\x1b[201~replyafter");
}

TEST(pasteRepairAllocationRefusalLeavesTheQueueUntouched)
{
    OutputQueue queue;
    UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
    CHECK(!queue.repairPending(1, std::span<const char>("\x1b[201~", 6)));
    CHECK(!queue.hasPending());
    CHECK_EQ(queue.pendingBytes(), 0u);
}

TEST(testPartialThenDrain)
{
    OutputQueue q;
    Sink s;
    s.budget = 3; // 3 bytes of room, then full
    int rc = q.submit(std::span<const char>("abcdefghij", 10), sinkWrite, &s);
    CHECK_EQ(rc, 0);
    // The sink took what fit; the queue holds the tail.
    CHECK_STR_EQ(s.out, "abc");
    CHECK(q.hasPending());
    CHECK_EQ(q.pendingBytes(), 7u);

    // Each drain finds room for three more bytes, and the tail leaves in order.
    for (const char* deliveredSoFar : { "abcdef", "abcdefghi", "abcdefghij" }) {
        s.budget = 3;
        q.drain(sinkWrite, &s);
        CHECK_STR_EQ(s.out, deliveredSoFar);
    }
    CHECK(!q.hasPending());

    // With nothing queued, a drain that has room writes nothing.
    s.budget = 3;
    q.drain(sinkWrite, &s);
    CHECK_STR_EQ(s.out, "abcdefghij");
}

TEST(testOrderAcrossSubmits)
{
    OutputQueue q;
    Sink s;
    s.budget = 2; // "AA" out, rest queued
    q.submit(std::span<const char>("AAAA", 4), sinkWrite, &s);
    q.submit(std::span<const char>("BBBB", 4), sinkWrite, &s); // must queue BEHIND the "AA" tail
    s.budget = (1L << 60);
    q.drain(sinkWrite, &s);
    // The second submit waited behind the first one's tail, and nothing was lost.
    CHECK_STR_EQ(s.out, "AAAABBBB");
    CHECK(!q.hasPending());
}

TEST(testFullNowHoldsThenOpens)
{
    OutputQueue q;
    Sink s;
    s.budget = 0; // sink momentarily full
    q.submit(std::span<const char>("payload", 7), sinkWrite, &s);
    // A sink with no room takes nothing, so every byte waits in the queue.
    CHECK(s.out.empty());
    CHECK_EQ(q.pendingBytes(), 7u);
    s.budget = (1L << 60);
    q.drain(sinkWrite, &s);
    CHECK_STR_EQ(s.out, "payload");
}

TEST(discardingDropsWhatIsQueuedAndKeepsWhatFollowsInOrder)
{
    OutputQueue q;
    Sink s;
    s.budget = 3; // takes "typ" and then no more
    q.submit(std::span<const char>("typed ahead", 11), sinkWrite, &s);
    CHECK_MSG(s.out == "typ" && q.pendingBytes() == 8, "the rest is queued");
    q.discardPending();
    CHECK_MSG(!q.hasPending() && q.pendingBytes() == 0, "nothing is queued after a discard");
    s.budget = (1L << 60);
    q.submit(std::span<const char>("after", 5), sinkWrite, &s);
    q.drain(sinkWrite, &s);
    CHECK_MSG(s.out == "typafter", "what was discarded is never delivered, and what follows is");
}

TEST(testFatalPropagatesAndKeepsQueue)
{
    OutputQueue q;
    Sink s;
    s.budget = 4;
    q.submit(std::span<const char>("0123456789", 10), sinkWrite, &s); // 4 out, "456789" queued
    CHECK_STR_EQ(s.out, "0123");
    CHECK_EQ(q.pendingBytes(), 6u);
    s.fatal = true;
    int rc = q.drain(sinkWrite, &s);
    // The drain reports the sink's own error, and drops none of the backlog.
    CHECK_EQ(rc, -7);
    CHECK_EQ(q.pendingBytes(), 6u);
}

TEST(testLargeLossless)
{
    OutputQueue q;
    Sink s;
    s.budget = 0;
    std::string in;
    for (int i = 0; i < 5000; ++i)
        in.push_back(static_cast<char>('a' + (i % 26)));
    q.submit(std::span<const char>(in.data(), in.size()), sinkWrite, &s);
    // Open the sink 7 bytes at a time until the queue is empty.
    for (int i = 0; i < 5000 && q.hasPending(); ++i) {
        s.budget += 7;
        q.drain(sinkWrite, &s);
    }
    CHECK(!q.hasPending());
    CHECK_MSG(s.out == in, "every one of the 5000 bytes arrives, in the order submitted");
}

// Only queue storage uses this allocation form in these cases. The injector
// scopes its process-wide policy and retains the ordinary delete[] pairing.
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

TEST(ownedCancellationKeepsDeliveredPrefixesNeighborsAndOrdinaryReplies)
{
    const std::string first = "abcdef";
    for (std::size_t prefix = 0; prefix <= first.size(); ++prefix) {
        OutputQueue queue;
        Sink sink;
        sink.budget = static_cast<long>(prefix);
        CHECK_EQ(queue.submitOwned(11, first, sinkWrite, &sink), 0);
        CHECK_STR_EQ(sink.out, first.substr(0, prefix));
        CHECK_EQ(queue.submitOwned(22, std::span<const char>("neighbor", 8), sinkWrite, &sink), 0);
        CHECK_EQ(queue.submit(std::span<const char>("reply", 5), sinkWrite, &sink), 0);
        CHECK_EQ(queue.submitOwned(11, std::span<const char>("gh", 2), sinkWrite, &sink), 0);
        CHECK_EQ(queue.pendingBytes(11), first.size() - prefix + 2);
        CHECK_EQ(queue.pendingBytes(22), 8u);
        CHECK_EQ(queue.pendingBytes(0), 5u);
        CHECK_EQ(queue.discardPending(0), 0u);
        CHECK_EQ(queue.discardPending(999), 0u);
        CHECK_EQ(queue.discardPending(11), first.size() - prefix + 2);
        CHECK_EQ(queue.discardPending(11), 0u);
        CHECK_EQ(queue.pendingBytes(), 13u);
        sink.budget = std::numeric_limits<long>::max();
        CHECK_EQ(queue.drain(sinkWrite, &sink), 0);
        CHECK_EQ(queue.drain(sinkWrite, &sink), 0);
        CHECK_STR_EQ(sink.out, first.substr(0, prefix) + "neighborreply");
        CHECK_EQ(queue.pendingBytes(11), 0u);
        // Cancellation is storage management, not revocation of a token.
        CHECK_EQ(queue.submitOwned(11, std::span<const char>("new", 3), sinkWrite, &sink), 0);
        CHECK_STR_EQ(sink.out, first.substr(0, prefix) + "neighborreplynew");
    }
}

TEST(ownershipRunsAreConsumedAcrossPartialDrainsAndCoalescedAfterCancellation)
{
    OutputQueue queue;
    Sink sink;
    sink.budget = 0;
    CHECK_EQ(queue.submitOwned(1, std::span<const char>("AA", 2), sinkWrite, &sink), 0);
    CHECK_EQ(queue.submitOwned(2, std::span<const char>("BB", 2), sinkWrite, &sink), 0);
    CHECK_EQ(queue.submitOwned(1, std::span<const char>("CC", 2), sinkWrite, &sink), 0);
    sink.budget = 3;
    CHECK_EQ(queue.drain(sinkWrite, &sink), 0);
    CHECK_STR_EQ(sink.out, std::string("AAB"));
    CHECK_EQ(queue.pendingBytes(1), 2u);
    CHECK_EQ(queue.pendingBytes(2), 1u);
    CHECK_EQ(queue.discardPending(2), 1u);
    CHECK_EQ(queue.submitOwned(1, std::span<const char>("DD", 2), sinkWrite, &sink), 0);
    sink.budget = 20;
    CHECK_EQ(queue.drain(sinkWrite, &sink), 0);
    CHECK_STR_EQ(sink.out, std::string("AABCCDD"));
}

TEST(ownedMetadataRefusalPrecedesTheSinkAndCancellationRecoversCapacity)
{
    OutputQueue queue;
    Sink sink;
    sink.budget = 0;
    for (std::size_t index = 0; index < OutputQueue::maxPendingRuns; ++index)
        CHECK_EQ(queue.submitOwned(index % 2 + 1, std::span<const char>("x", 1), sinkWrite, &sink), 0);
    const std::size_t calls = sink.calls;
    CHECK_EQ(queue.submitOwned(3, std::span<const char>("z", 1), sinkWrite, &sink), OutputQueue::errQueueFull);
    CHECK_EQ(sink.calls, calls);
    CHECK_EQ(queue.pendingBytes(), OutputQueue::maxPendingRuns);
    CHECK_EQ(queue.submitOwned(2, std::span<const char>("y", 1), sinkWrite, &sink), 0);
    CHECK_EQ(queue.discardPending(1), OutputQueue::maxPendingRuns / 2);
    // Removing alternating runs coalesces the surviving owner, not merely its
    // bytes; another full set of distinct runs can now be admitted.
    for (std::size_t index = 1; index < OutputQueue::maxPendingRuns; ++index)
        CHECK_EQ(queue.submitOwned(index + 2, std::span<const char>("z", 1), sinkWrite, &sink), 0);
    queue.discardPending();
    CHECK_EQ(queue.pendingBytes(2), 0u);
    CHECK_EQ(queue.submitOwned(3, std::span<const char>("after", 5), sinkWrite, &sink), 0);
    CHECK_EQ(queue.pendingBytes(3), 5u);
}

TEST(ownedByteAdmissionAndAllocationFailureHaveNoDeliveredPrefix)
{
    OutputQueue queue;
    Sink sink;
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        CHECK_EQ(queue.submitOwned(1, std::span<const char>("payload", 7), sinkWrite, &sink), OutputQueue::errQueueFull);
    }
    CHECK_EQ(sink.calls, 0u);
    CHECK(sink.out.empty());
    CHECK(!queue.hasPending());
    CHECK_EQ(queue.reserveOwned(1, OutputQueue::maxPending + 1), OutputQueue::errQueueFull);
    CHECK_EQ(queue.reserveOwned(1, std::numeric_limits<std::size_t>::max()), OutputQueue::errQueueFull);
    CHECK_EQ(queue.reserveOwned(1, OutputQueue::maxPending), 0);
    CHECK_EQ(queue.pendingBytes(), 0u);
    sink.budget = 0;
    CHECK_EQ(queue.submitOwned(1, std::span<const char>("a", 1), sinkWrite, &sink), 0);
    CHECK_EQ(queue.reserveOwned(2, OutputQueue::maxPending - 1), 0);
    CHECK_EQ(queue.reserveOwned(2, OutputQueue::maxPending), OutputQueue::errQueueFull);
    CHECK_EQ(queue.pendingBytes(1), 1u);
}

TEST(ownedGrowthFailurePreservesExistingOwnershipAndBytes)
{
    OutputQueue queue;
    Sink sink;
    sink.budget = 0;
    CHECK_EQ(queue.submitOwned(1, std::span<const char>("first", 5), sinkWrite, &sink), 0);
    const std::string next(300, 'x');
    const std::size_t calls = sink.calls;
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        CHECK_EQ(queue.submitOwned(2, next, sinkWrite, &sink), OutputQueue::errQueueFull);
    }
    CHECK_EQ(sink.calls, calls);
    CHECK_EQ(queue.pendingBytes(1), 5u);
    CHECK_EQ(queue.pendingBytes(2), 0u);
    sink.budget = 5;
    CHECK_EQ(queue.drain(sinkWrite, &sink), 0);
    CHECK_STR_EQ(sink.out, std::string("first"));
}

TEST(ownedSinkFailureKeepsOnlyTheUndeliveredOwnership)
{
    OutputQueue queue;
    Sink sink;
    sink.budget = 2;
    CHECK_EQ(queue.submitOwned(1, std::span<const char>("first", 5), sinkWrite, &sink), 0);
    CHECK_EQ(queue.submit(std::span<const char>("reply", 5), sinkWrite, &sink), 0);
    sink.fatal = true;
    CHECK_EQ(queue.submitOwned(2, std::span<const char>("last", 4), sinkWrite, &sink), -7);
    CHECK_STR_EQ(sink.out, std::string("fi"));
    CHECK_EQ(queue.pendingBytes(1), 3u);
    CHECK_EQ(queue.pendingBytes(0), 5u);
    CHECK_EQ(queue.pendingBytes(2), 4u);
    CHECK_EQ(queue.discardPending(1), 3u);
    CHECK_EQ(queue.discardPending(2), 4u);
    sink.fatal = false;
    sink.budget = 5;
    CHECK_EQ(queue.drain(sinkWrite, &sink), 0);
    CHECK_STR_EQ(sink.out, std::string("fireply"));
    CHECK(!queue.hasPending());
}

TEST(zeroOwnerAndEmptyOwnedWritesCannotAffectOrdinaryBytes)
{
    OutputQueue queue;
    Sink sink;
    sink.budget = 0;
    CHECK_EQ(queue.submit(std::span<const char>("reply", 5), sinkWrite, &sink), 0);
    const std::size_t calls = sink.calls;
    CHECK_EQ(queue.submitOwned(0, std::span<const char>("bad", 3), sinkWrite, &sink), OutputQueue::errInvalidOwner);
    CHECK_EQ(queue.reserveOwned(0, 0), OutputQueue::errInvalidOwner);
    CHECK_EQ(queue.submitOwned(1, {}, sinkWrite, &sink), 0);
    CHECK_EQ(sink.calls, calls);
    CHECK_EQ(queue.discardPending(0), 0u);
    CHECK_EQ(queue.pendingBytes(0), 5u);
}

TEST(unwrittenHandoverPreservesEveryPartialPrefixAndOwnerInOrder)
{
    const std::string payload = "reply\x1b[200~paste\x1b[201~neighborend";
    for (std::size_t prefix = 0; prefix <= payload.size(); ++prefix) {
        OutputQueue predecessor;
        Sink sink;
        sink.budget = static_cast<long>(prefix);
        REQUIRE(predecessor.submit(std::span("reply", 5), sinkWrite, &sink) == 0);
        REQUIRE(predecessor.submitOwned(7, std::span("\x1b[200~paste\x1b[201~", 17), sinkWrite, &sink) == 0);
        REQUIRE(predecessor.submitOwned(8, std::span("neighbor", 8), sinkWrite, &sink) == 0);
        REQUIRE(predecessor.submit(std::span("end", 3), sinkWrite, &sink) == 0);
        const auto before = predecessor.pendingBytes();
        auto copied = predecessor.copyUnwritten();
        REQUIRE(copied);
        CHECK_EQ(predecessor.pendingBytes(), before);
        CHECK_STR_EQ(std::string(copied->bytes().begin(), copied->bytes().end()), payload.substr(prefix));
        const auto* allocation = copied->bytes().data();
        OutputQueue successor;
        const auto calls = sink.calls;
        REQUIRE(successor.restoreUnwritten(*copied));
        CHECK_EQ(sink.calls, calls);
        CHECK(copied->bytes().empty() && copied->runs().empty());
        CHECK_EQ(successor.pendingBytes(), before);
        for (const auto owner : { 0u, 7u, 8u })
            CHECK_EQ(successor.pendingBytes(owner), predecessor.pendingBytes(owner));
        // A committed predecessor must never drain its independent old copy.
        predecessor.discardPending();
        sink.budget = 1000;
        REQUIRE(successor.submitOwned(9, std::span("next", 4), sinkWrite, &sink) == 0);
        CHECK_STR_EQ(sink.out, payload + "next");
        CHECK(!successor.hasPending());
        CHECK(allocation || before == 0);
    }
}

TEST(restoredOwnersStillCancelAndRepairAheadOfOrdinaryReplies)
{
    OutputQueue source;
    Sink sink;
    sink.budget = 0;
    REQUIRE(source.submit(std::span("initial", 7), sinkWrite, &sink) == 0);
    REQUIRE(source.submitOwned(5, std::span("old", 3), sinkWrite, &sink) == 0);
    REQUIRE(source.submit(std::span("reply", 5), sinkWrite, &sink) == 0);
    REQUIRE(source.submitOwned(6, std::span("neighbor", 8), sinkWrite, &sink) == 0);
    REQUIRE(source.submitOwned(5, std::span("tail", 4), sinkWrite, &sink) == 0);
    auto copied = source.copyUnwritten();
    REQUIRE(copied);
    OutputQueue target;
    REQUIRE(target.restoreUnwritten(*copied));
    CHECK_EQ(target.discardPending(0), 0u);
    REQUIRE(target.repairPending(5, std::span("\x1b[201~", 6)));
    CHECK_EQ(target.pendingBytes(5), 0u);
    CHECK_EQ(target.pendingBytes(6), 8u);
    CHECK_EQ(source.pendingBytes(5), 7u);
    sink.budget = 1000;
    REQUIRE(target.drain(sinkWrite, &sink) == 0);
    CHECK_STR_EQ(sink.out, "initial\x1b[201~replyneighbor");
}

TEST(unwrittenImportRefusesOccupiedQueuesWithoutConsumingEitherOwner)
{
    const std::array runs { OutputQueue::Run { 11, 3 } };
    auto copied = OutputQueue::Unwritten::copyOf(std::span("new", 3), runs);
    REQUIRE(copied);
    const auto* allocation = copied->bytes().data();
    OutputQueue target;
    Sink sink;
    sink.budget = 0;
    REQUIRE(target.submit(std::span("old", 3), sinkWrite, &sink) == 0);
    REQUIRE(!target.restoreUnwritten(*copied));
    CHECK(copied->bytes().data() == allocation);
    CHECK_EQ(copied->bytes().size(), 3u);
    CHECK_EQ(target.pendingBytes(0), 3u);
    sink.budget = 3;
    REQUIRE(target.drain(sinkWrite, &sink) == 0);
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        REQUIRE(target.restoreUnwritten(*copied));
    }
    CHECK(copied->bytes().empty() && copied->runs().empty());
    sink.budget = 3;
    REQUIRE(target.drain(sinkWrite, &sink) == 0);
    CHECK_STR_EQ(sink.out, "oldnew");
}

TEST(unwrittenPartitionIsExactCanonicalAndBoundedBeforeAllocation)
{
    using Run = OutputQueue::Run;
    const std::span<const char> bytes("abc", 3);
    for (const auto& runs : { std::vector<Run> {}, std::vector<Run> { { 0, 0 }, { 1, 3 } },
             std::vector<Run> { { 1, 2 } }, std::vector<Run> { { 1, 4 } },
             std::vector<Run> { { 1, 1 }, { 1, 2 } },
             std::vector<Run> { { 1, 1 }, { 2, std::numeric_limits<std::size_t>::max() } } })
        CHECK(!OutputQueue::Unwritten::copyOf(bytes, runs));
    const std::array nonempty { Run { 1, 1 } };
    CHECK(!OutputQueue::Unwritten::copyOf({}, nonempty));
    std::array<Run, OutputQueue::maxPendingRuns + 1> excessive {};
    std::string many(excessive.size(), 'x');
    for (std::size_t index = 0; index < excessive.size(); ++index)
        excessive[index] = { index % 2, 1 };
    CHECK(!OutputQueue::Unwritten::copyOf(many, excessive));
    REQUIRE(OutputQueue::Unwritten::copyOf(std::span(many).first(OutputQueue::maxPendingRuns), std::span(excessive).first(OutputQueue::maxPendingRuns)));
    std::string full(OutputQueue::maxPending + 1, 'x');
    const std::array oversized { Run { 1, full.size() } };
    CHECK(!OutputQueue::Unwritten::copyOf(full, oversized));
    const std::array atLimit { Run { 1, OutputQueue::maxPending } };
    REQUIRE(OutputQueue::Unwritten::copyOf(std::span(full).first(OutputQueue::maxPending), atLimit));
}

TEST(unwrittenCopyAllocationRefusalLeavesSourceDrainableAndEmptyCopiesAllocateNothing)
{
    OutputQueue source;
    Sink sink;
    sink.budget = 2;
    REQUIRE(source.submitOwned(44, std::span("prefix-tail", 11), sinkWrite, &sink) == 0);
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refuse;
        CHECK(!source.copyUnwritten());
        REQUIRE(OutputQueue::Unwritten::copyOf({}, {}));
        OutputQueue empty;
        REQUIRE(empty.copyUnwritten());
    }
    CHECK_EQ(source.pendingBytes(44), 9u);
    sink.budget = 100;
    REQUIRE(source.drain(sinkWrite, &sink) == 0);
    CHECK_STR_EQ(sink.out, "prefix-tail");
}

TEST(movingUnwrittenOwnersLeavesAReusableEmptySource)
{
    const std::array runs { OutputQueue::Run { 22, 3 } };
    auto source = OutputQueue::Unwritten::copyOf(std::span("abc", 3), runs);
    REQUIRE(source);
    const auto* allocation = source->bytes().data();
    OutputQueue::Unwritten moved(std::move(*source));
    CHECK(moved.bytes().data() == allocation);
    CHECK(source->bytes().empty() && source->runs().empty());
    OutputQueue target;
    REQUIRE(target.restoreUnwritten(*source));
    CHECK(!target.hasPending());
    *source = std::move(moved);
    CHECK(moved.bytes().empty() && moved.runs().empty());
    REQUIRE(target.restoreUnwritten(*source));
    CHECK_EQ(target.pendingBytes(22), 3u);
}

UBIQUITTY_TEST_MAIN()
