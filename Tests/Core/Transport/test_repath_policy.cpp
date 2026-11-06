// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_repath_policy — a carrier moves to a fresh path before its connection
// is declared lost (Docs/sync.md §6.9): on a socket error, on the interface
// going, or after the silence bound with data outstanding; once; and the
// connection is lost only when the fresh path fails too.

#include "Harness.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Transport/RepathPolicy.h"

#include <cstdint>
#include <optional>

using namespace Ubiquitty::Transport;

namespace {

constexpr std::uint64_t silence = repathSilenceMicroseconds;

} // namespace

// Ten seconds with data outstanding and no round trip: the carrier moves.
TEST(silenceWithDataOutstandingMovesTheCarrier)
{
    RepathPolicy policy;
    const std::uint64_t began = 1'000'000;
    policy.noteOutstanding(began);
    const std::uint64_t now = began + silence;
    CHECK(policy.due(now) == std::optional { RepathReason::Silence });
    CHECK(!policy.due(now - 1).has_value());
    CHECK(policy.nextDueMicroseconds() == std::optional { began + silence });
    // Outstanding is counted from when it began, not from each report.
    policy.noteOutstanding(began + silence / 2);
    CHECK(policy.due(now) == std::optional { RepathReason::Silence });
    CHECK(!policy.hasFailed(now));
    // The bound is the one the session layer's measured table holds.
    CHECK_EQ(repathSilenceMicroseconds, Ubiquitty::Protocol::Sync::measuredLivenessPolicy().repathMicroseconds);
}

// A connection with nothing outstanding never moves, however long it idles,
// and neither does one whose peer keeps answering.
TEST(anIdleConnectionNeverRepaths)
{
    RepathPolicy policy;
    CHECK(!policy.due(100 * silence).has_value());
    CHECK(!policy.nextDueMicroseconds().has_value());
    policy.noteOutstanding(1'000);
    policy.noteNothingOutstanding();
    CHECK(!policy.due(100 * silence).has_value());
    CHECK(!policy.nextDueMicroseconds().has_value());
    // Answered every second for a minute: outstanding never grows old.
    for (std::uint64_t second = 1; second <= 60; ++second) {
        policy.noteOutstanding(second * 1'000'000);
        CHECK(!policy.due(second * 1'000'000 + 900'000).has_value());
        policy.noteRoundTrip();
    }
    CHECK(!policy.hasFailed(100 * silence));
}

// A socket error, or the interface going, moves the carrier at once, with
// nothing outstanding; the first sign is the reason given.
TEST(aSocketErrorOrALostInterfaceMovesTheCarrierAtOnce)
{
    RepathPolicy failing;
    failing.noteSocketError();
    CHECK(failing.due(0) == std::optional { RepathReason::SocketError });
    failing.notePathRemoved();
    CHECK(failing.due(0) == std::optional { RepathReason::SocketError });
    CHECK(!failing.nextDueMicroseconds().has_value());

    RepathPolicy removed;
    removed.notePathRemoved();
    CHECK(removed.due(0) == std::optional { RepathReason::PathRemoved });
    removed.noteSocketError();
    CHECK(removed.due(0) == std::optional { RepathReason::PathRemoved });
    // A sign outranks the silence that would follow it.
    removed.noteOutstanding(0);
    CHECK(removed.due(silence) == std::optional { RepathReason::PathRemoved });
    // A round trip on the path says the sign was not the path's end.
    removed.noteRoundTrip();
    CHECK(!removed.due(silence).has_value());
}

// The move is tried once, and judged: a round trip on the fresh path is its
// success, after which the carrier may move again another day.
TEST(aMoveThatIsAnsweredSucceeds)
{
    RepathPolicy policy;
    policy.noteSocketError();
    REQUIRE(policy.due(1'000).has_value());
    policy.noteRepathed(1'000);
    CHECK(!policy.due(1'000).has_value());
    CHECK(!policy.hasFailed(1'000));
    CHECK(policy.nextDueMicroseconds() == std::optional { 1'000 + silence });
    // While it is judged nothing moves it again, whatever is reported.
    policy.notePathRemoved();
    policy.noteOutstanding(2'000);
    CHECK(!policy.due(2'000 + silence - 2'000).has_value());
    policy.noteRoundTrip();
    CHECK(!policy.hasFailed(1'000 + 2 * silence));
    CHECK(!policy.due(1'000 + 2 * silence).has_value());
    policy.notePathRemoved();
    CHECK(policy.due(1'000 + 2 * silence) == std::optional { RepathReason::PathRemoved });
}

// The connection is lost only after the move fails: the fresh path stays
// silent for the bound, or fails as a socket, or could not be opened.
TEST(theConnectionIsLostOnlyAfterTheMoveFails)
{
    RepathPolicy silent;
    silent.noteOutstanding(0);
    REQUIRE(silent.due(silence) == std::optional { RepathReason::Silence });
    CHECK(!silent.hasFailed(silence));
    silent.noteRepathed(silence);
    // What goes out on the fresh path and is not answered does not move the
    // carrier a second time: the move is tried once.
    silent.noteOutstanding(silence);
    CHECK(!silent.hasFailed(2 * silence - 1));
    CHECK(silent.hasFailed(2 * silence));
    CHECK(!silent.due(2 * silence).has_value());
    CHECK(!silent.due(3 * silence).has_value());

    RepathPolicy broken;
    broken.notePathRemoved();
    broken.noteRepathed(5);
    CHECK(!broken.hasFailed(6));
    broken.noteSocketError();
    CHECK(broken.hasFailed(6));
    CHECK(!broken.due(6).has_value());
    CHECK(!broken.nextDueMicroseconds().has_value());
    // Nothing brings it back.
    broken.noteRoundTrip();
    CHECK(broken.hasFailed(7));

    RepathPolicy stuck;
    stuck.noteSocketError();
    stuck.noteRepathImpossible();
    CHECK(stuck.hasFailed(0));
    CHECK(!stuck.due(0).has_value());
}

// A shorter bound is the caller's to give.
TEST(theSilenceBoundIsThePolicys)
{
    RepathPolicy quick(250'000);
    quick.noteOutstanding(0);
    CHECK(!quick.due(249'999).has_value());
    CHECK(quick.due(250'000) == std::optional { RepathReason::Silence });
}

UBIQUITTY_TEST_MAIN()
