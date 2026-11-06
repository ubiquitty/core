// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_bytebuffer.cpp — unit tests for ByteBuffer, the bounded growable buffer
// the Kitty APC parser and the chunked-image accumulator fill from the wire.
//
// The refusal is the reason this file exists. Growth caps capacity at the
// limit, so an append allowed past the limit would memcpy beyond the
// allocation — the limit test is the only thing between an attacker-chosen
// payload length and a heap overflow. Deleting it passed every Kitty, image and
// VT case in the suite before these tests existed.
//
// One branch needs a seam rather than an input. `append` returns false when the
// allocator declines, and no payload provokes that: the growth loop asks only
// for what the append needs, so a 16-byte append into a fresh buffer takes the
// initial 1024 whatever limit it is given. `s_shouldFailNextGrowForTest` is how the
// case reaches it, and a case asserting the append was either refused or stored
// would cover the line while passing whichever happened.

#include "Base/ByteBuffer.h"

#include <cstring>
#include <string>

using Ubiquitty::ByteBuffer;

TEST(anAppendWithinTheLimitKeepsTheBytesAndTerminates)
{
    ByteBuffer buffer;
    CHECK(buffer.append("abc", 1024));
    CHECK_EQ(buffer.length, 3);
    CHECK(std::memcmp(buffer.data, "abc", 3) == 0);
    // Readers take this buffer as a C string, so the terminator is contract.
    CHECK_EQ(int(buffer.data[3]), 0);
    CHECK(buffer.capacity >= 4);
    buffer.reset();
}

TEST(appendsAccumulateRatherThanReplace)
{
    ByteBuffer buffer;
    CHECK(buffer.append("ab", 1024));
    CHECK(buffer.append("cd", 1024));
    CHECK_EQ(buffer.length, 4);
    CHECK(std::memcmp(buffer.data, "abcd", 4) == 0);
    CHECK_EQ(int(buffer.data[4]), 0);
    buffer.reset();
}

TEST(anAppendPastTheLimitIsRefusedAndChangesNothing)
{
    ByteBuffer buffer;
    CHECK(buffer.append(std::string(60, 'x'), 100));
    const int lengthBefore = buffer.length;
    const int capacityBefore = buffer.capacity;

    // 60 + 50 exceeds 100. The refusal must leave the buffer exactly as it was:
    // a partial append would hand the caller a payload it never sent.
    CHECK(!buffer.append(std::string(50, 'y'), 100));
    CHECK_EQ(buffer.length, lengthBefore);
    CHECK_EQ(buffer.capacity, capacityBefore);
    for (int i = 0; i < buffer.length; ++i)
        CHECK_EQ(int(buffer.data[i]), int('x'));
    buffer.reset();
}

TEST(theLimitIsInclusiveAndTheByteAfterItIsNot)
{
    ByteBuffer atTheLimit;
    CHECK(atTheLimit.append(std::string(100, 'x'), 100));
    CHECK_EQ(atTheLimit.length, 100);
    // One more byte on a buffer already at the limit.
    CHECK(!atTheLimit.append("y", 100));
    CHECK_EQ(atTheLimit.length, 100);
    atTheLimit.reset();

    ByteBuffer pastTheLimit;
    CHECK(!pastTheLimit.append(std::string(101, 'x'), 100));
    CHECK_EQ(pastTheLimit.length, 0);
    pastTheLimit.reset();
}

TEST(manySmallAppendsAreRefusedAtTheSamePointAsOneLargeOne)
{
    // The single-byte path is how the APC parser fills this buffer, and it is
    // the one that reaches the limit gradually rather than in one request.
    ByteBuffer buffer;
    int accepted = 0;
    for (int i = 0; i < 200; ++i) {
        if (!buffer.append("z", 100))
            break;
        ++accepted;
    }
    CHECK_EQ(accepted, 100);
    CHECK_EQ(buffer.length, 100);
    buffer.reset();
}

TEST(anEmptyAppendIsAllowedAndAddsNothing)
{
    ByteBuffer buffer;
    CHECK(buffer.append("", 100));
    CHECK_EQ(buffer.length, 0);
    CHECK(buffer.append("ab", 100));
    CHECK(buffer.append("", 100));
    CHECK_EQ(buffer.length, 2);
    buffer.reset();
}

TEST(resetClearsAllThreeFieldsTogether)
{
    ByteBuffer buffer;
    CHECK(buffer.append("payload", 1024));
    CHECK(buffer.capacity > 0);
    buffer.reset();
    // A length cleared without its capacity leaves an allocation the buffer no
    // longer describes, which is what two hand-written teardowns of this buffer
    // disagreed about before it was a type.
    CHECK(buffer.data == nullptr);
    CHECK_EQ(buffer.length, 0);
    CHECK_EQ(buffer.capacity, 0);
    // Reset is idempotent, and a reset buffer still appends.
    buffer.reset();
    CHECK(buffer.append("again", 1024));
    CHECK_EQ(buffer.length, 5);
    buffer.reset();
}

TEST(anAppendIsRefusedWhenTheAllocatorDeclinesToGrow)
{
    // The refusal is what stops `append` copying into an allocation that did not
    // grow. Without it the memcpy below would run against the old capacity.
    ByteBuffer buffer;
    ByteBuffer::s_shouldFailNextGrowForTest = true;
    const bool isAccepted = buffer.append(std::string(16, 'x'), 1024);
    ByteBuffer::s_shouldFailNextGrowForTest = false;

    CHECK(!isAccepted);
    CHECK(buffer.length == 0);
    CHECK(buffer.data == nullptr);

    // The buffer is still usable once the allocator obliges, so the refusal
    // leaves no wreckage — a caller that retries gets a working buffer.
    CHECK(buffer.append(std::string(16, 'y'), 1024));
    CHECK(buffer.length == 16);
    CHECK(buffer.data[16] == 0);
    buffer.reset();
}

TEST(growthDoublesUntilTheAppendFits)
{
    // A first append inside the initial 1024 never enters the growth loop, so
    // every case that stays small leaves the doubling untested. This one needs
    // more than the initial capacity with a limit far above it, which is the
    // ordinary shape: double until the append fits and stop there.
    ByteBuffer buffer;
    const int generousLimit = 1 << 20;
    CHECK(buffer.append(std::string(5000, 'x'), generousLimit));
    CHECK(buffer.length == 5000);
    CHECK(buffer.data[5000] == 0);

    // Doubling from 1024 reaches 8192 for 5001 bytes, and stops rather than
    // running to the limit — a loop that kept going would allocate a megabyte
    // for a five-kilobyte payload.
    CHECK(buffer.capacity == 8192);

    // A second growth starts from the capacity already held rather than from the
    // initial 1024, which is the arm a buffer that grows once never reaches: a
    // reset to 1024 here would shrink the allocation under the bytes already in
    // it.
    CHECK(buffer.append(std::string(5000, 'y'), generousLimit));
    CHECK(buffer.length == 10000);
    CHECK(buffer.capacity == 16384); // doubled from 8192, not restarted at 1024
    CHECK(buffer.data[0] == 'x');
    CHECK(buffer.data[9999] == 'y');
    CHECK(buffer.data[10000] == 0);
    buffer.reset();
}

TEST(growthClampsToTheLimitRatherThanOvershootingIt)
{
    // Doubling past half the limit jumps straight to the limit instead of
    // doubling again, so the buffer never allocates more than the limit admits.
    // Without that arm the next double would be 4096 against a limit of 3000.
    ByteBuffer buffer;
    const int limit = 3000;
    CHECK(buffer.append(std::string(2500, 'x'), limit));
    CHECK(buffer.length == 2500);
    CHECK(buffer.capacity == limit + 1); // the clamp, not a power of two
    CHECK(buffer.data[2500] == 0);

    // And the limit still refuses what it should: the buffer is at 2500 of 3000,
    // so 501 more is one too many and 500 is exact.
    CHECK(!buffer.append(std::string(501, 'y'), limit));
    CHECK(buffer.length == 2500);
    CHECK(buffer.append(std::string(500, 'y'), limit));
    CHECK(buffer.length == limit);
    buffer.reset();
}

// A view too large to measure in the signed domain is refused, rather than
// arriving as a negative length that every later test reads as room.
//
// `bytes.size()` is unsigned. Narrowed to int, two gibibytes or more is
// negative, and each of the three checks downstream then reports the opposite of
// what it exists to report: `limit - byteCount` grows, so the overflow test says
// there is room; `need` comes out below `length`, so the growth test says the
// capacity already covers it; and the copy is skipped by its own `> 0` guard,
// which is what makes it look handled. The write that lands is the terminator,
// at a negative offset from the allocation.
//
// The view is never read, only measured, so its size can be one no allocation
// could back: the refusal has to happen before anything dereferences the
// pointer, and this case takes a fault rather than a failure if that stops
// being true.
TEST(anOversizedViewIsRefusedRatherThanNarrowed)
{
    Ubiquitty::ByteBuffer buffer;
    const int limit = 3000;
    CHECK(buffer.append("seed", limit));
    CHECK(buffer.length == 4);

    char anchor[8] = {};
    const std::string_view oversized(anchor, static_cast<std::size_t>(3) * 1024 * 1024 * 1024);
    CHECK_MSG(!buffer.append(oversized, limit), "a view past the signed range is refused");
    CHECK_MSG(buffer.length == 4, "and the buffer is left exactly as it was");
    CHECK_MSG(buffer.data[buffer.length] == 0, "with its terminator still at the length it reports");

    // The control on the other side: a negative limit is refused too, since
    // widening one into the comparison is the same defect pointed the other way.
    CHECK_MSG(!buffer.append("x", -1), "a negative limit refuses rather than casting");
    CHECK(buffer.length == 4);
    buffer.reset();
}

UBIQUITTY_TEST_MAIN()
