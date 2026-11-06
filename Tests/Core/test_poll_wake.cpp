// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Core/test_poll_wake.cpp — a deadline converts to a poll timeout without
// wrapping.
//
// Daemon::run turns six deadlines into the single `int` that poll takes. Five
// were millisecond differences cast straight across and one clamped its seconds
// before multiplying, so the block had one rule and five exceptions. A
// difference past INT_MAX does not saturate on conversion; it truncates to
// whatever the low bits say, which is usually a SHORT timeout where a long one
// was due. The loop then spins at that cadence until the deadline arrives on
// its own — a busy-wait produced by an arithmetic accident rather than by any
// decision.
//
// Clamping makes the error late rather than wrong. A poll that returns before
// its deadline recomputes and waits again; one that wrapped cannot recover,
// because nothing about it looks wrong from the inside.
//
// Exit 0 on success.
//

#include "Platform/Host/PollWake.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <string>

using Ubiquitty::Host::wakeAfterMilliseconds;
using Ubiquitty::Host::wakeAfterSeconds;

namespace {

int pollWakeFailures = 0;

void check(bool isSo, const std::string& what) // bool-ok: every line below is one condition, and the message names it
{
    if (!isSo) {
        std::fprintf(stderr, "poll_wake: FAIL — %s\n", what.c_str());
        ++pollWakeFailures;
    }
}

constexpr std::uint64_t maxInt = static_cast<std::uint64_t>(INT_MAX);

} // namespace

int main()
{
    // The ordinary case: what is left until the deadline.
    check(wakeAfterMilliseconds(1500, 1000) == 500, "a deadline half a second out waits half a second");

    // Due, and past due. Both are zero rather than negative — poll reads a
    // negative timeout as "block forever", which is the opposite of a deadline
    // that has already arrived.
    check(wakeAfterMilliseconds(1000, 1000) == 0, "a deadline exactly now waits not at all");
    check(wakeAfterMilliseconds(1000, 9000) == 0, "a deadline long past waits not at all, rather than forever");

    // The boundary the clamp exists for. One millisecond either side of what an
    // int holds: the first still fits, the second would wrap.
    check(wakeAfterMilliseconds(maxInt, 0) == INT_MAX, "a deadline at exactly INT_MAX fits");
    check(wakeAfterMilliseconds(maxInt + 1, 0) == INT_MAX, "and one past it clamps rather than wrapping");

    // A deadline far enough out that the truncation would land on a small
    // positive number — the shape that spins the loop. 2^32 milliseconds is
    // about 49 days, and its low 32 bits are zero.
    check(wakeAfterMilliseconds(std::uint64_t { 1 } << 32, 0) == INT_MAX,
        "a deadline whose low bits read as an instant does not wake instantly");
    check(wakeAfterMilliseconds(UINT64_MAX, 0) == INT_MAX, "and neither does the largest one representable");

    // Seconds. The clamp sits on the seconds because the product is where the
    // overflow is: an expiry is time(nullptr) + ttl with an operator-named ttl.
    check(wakeAfterSeconds(1000, 999) == 1001, "a second out waits a second, plus the millisecond past the boundary");
    check(wakeAfterSeconds(1000, 1000) == 0, "an expiry exactly now waits not at all");
    check(wakeAfterSeconds(1000, 5000) == 0, "an expiry long past waits not at all");

    // The seconds boundary: INT_MAX/1000 seconds is the most that survives the
    // multiply, so at and past it the answer is the clamp.
    constexpr std::uint64_t secondsThatFit = maxInt / 1000;
    check(wakeAfterSeconds(secondsThatFit, 0) == INT_MAX, "an expiry at the seconds boundary clamps");
    check(wakeAfterSeconds(secondsThatFit - 1, 0) == static_cast<int>((secondsThatFit - 1) * 1000 + 1),
        "and one below it converts exactly");
    // The seconds analogue of the case above, and the reason the clamp sits on
    // the seconds rather than on the product: 2^61 x 1000 is 2^64 x 125, so the
    // product is congruent to zero and the timeout wraps to one millisecond. A
    // clamp applied after the multiply cannot see that — it reads 1 and agrees.
    check(wakeAfterSeconds(std::uint64_t { 1 } << 61, 0) == INT_MAX,
        "an expiry whose product wraps to an instant does not poll every millisecond for 73 billion years");
    check(wakeAfterSeconds(UINT64_MAX, 0) == INT_MAX,
        "an expiry whose product with 1000 does not fit a uint64 clamps rather than wrapping");

    // Both are constexpr, so the boundaries above are also checked at compile
    // time — a wrap that only happened in a constant expression would be a
    // hard error rather than a wrong number.
    static_assert(wakeAfterMilliseconds(maxInt + 1, 0) == INT_MAX);
    static_assert(wakeAfterSeconds(std::uint64_t { 1 } << 61, 0) == INT_MAX);
    static_assert(wakeAfterMilliseconds(1000, 9000) == 0);

    // A file that trails what it records. A change owes one write an
    // interval later, and a second change before it owes nothing more.
    {
        Ubiquitty::Host::TrailingWrite write;
        check(!write.isOwed() && !write.isDue(5000), "nothing is owed before anything changed");
        write.oweForChange(1000, 1000);
        check(write.isOwed() && write.dueMilliseconds == 2000, "a change owes a write one interval later");
        write.oweForChange(1500, 1000);
        check(write.dueMilliseconds == 2000, "a second change rides the write the first owes");
        check(!write.isDue(1999) && write.isDue(2000), "a write is due at its time and not before");
        write.noteWritten(2000, 1000);
        check(!write.isOwed(), "a write for changes alone leaves nothing owed");
    }
    // A line typed while a write is owed: that write may come before the
    // shell has read the line, so the line is owed the next, an interval
    // after the latest line the write came too soon after.
    {
        Ubiquitty::Host::TrailingWrite write;
        write.oweForChange(1000, 1000);
        write.oweAfterSign(1990, 1000);
        check(write.dueMilliseconds == 2000, "a sign beside an owed write does not move it");
        write.oweAfterSign(1995, 1000);
        write.noteWritten(2000, 1000);
        check(write.isOwed() && write.dueMilliseconds == 2995, "a write too soon after a sign leaves one owed an interval after the latest sign");
        write.oweForChange(2500, 1000);
        check(write.dueMilliseconds == 2995, "a change beside an owed write rides it");
        write.noteWritten(2995, 1000);
        check(!write.isOwed(), "a write an interval after the latest sign serves it, and nothing more is owed");
    }
    // A sign typed after the write it was owed was scheduled, and one the
    // write comes late enough for.
    {
        Ubiquitty::Host::TrailingWrite write;
        write.oweAfterSign(1000, 1000);
        check(write.dueMilliseconds == 2000, "a sign with nothing owed owes one write an interval later");
        write.oweAfterSign(1500, 1000);
        check(write.dueMilliseconds == 2000, "a second sign does not move the write owed");
        write.noteWritten(2000, 1000);
        check(write.dueMilliseconds == 2500, "the second sign is owed a write of its own");
        write.oweAfterSign(2100, 1000);
        write.noteWritten(3200, 1000);
        check(!write.isOwed(), "a write late enough for every sign before it leaves nothing owed");
    }
    // Steady typing: each write is owed by a sign no earlier write served, so
    // writes are never closer together than the signs that owe them, and
    // nothing is owed once the typing stops.
    {
        Ubiquitty::Host::TrailingWrite write;
        int writes = 0;
        std::uint64_t lastWrite = 0;
        std::uint64_t closest = UINT64_MAX;
        for (std::uint64_t now = 1000; now <= 10'000; ++now) {
            if (now % 400 == 0 && now <= 6000)
                write.oweAfterSign(now, 1000);
            if (write.isDue(now)) {
                write.noteWritten(now, 1000);
                if (lastWrite)
                    closest = std::min(closest, now - lastWrite);
                lastWrite = now;
                ++writes;
            }
        }
        check(writes == 7 && closest >= 400 && lastWrite == 7000, "writes under steady typing are no closer together than the lines that owe them");
        check(!write.isOwed(), "and nothing is owed once the typing stops");
    }

    if (pollWakeFailures == 0) {
        std::printf("poll_wake: a deadline past what an int holds clamps to the longest wait poll takes, "
                    "in milliseconds and in seconds, and one already past waits not at all rather than forever\n");
        return 0;
    }
    std::fprintf(stderr, "poll_wake: %d failure(s)\n", pollWakeFailures);
    return 1;
}
