// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_dcs_passthrough_and_max — three DCS sites nothing reached.
//
// ubiquitty-layers probed each with a print and counted crossings across six
// suites. The tmux passthrough's detach-and-swap ran zero times in either branch;
// the 64 KiB ceiling was crossed 4097 times at the batched writer and zero at
// both per-byte writers. The uncovered ones are the reachable shapes — a payload
// arrives byte-at-a-time whenever it is not delivered in one buffer — and the
// covered one is the optimisation.
//
// The passthrough went unreached for a plain reason: `dcsDispatch` matches the
// prefix `tmux;`, and the DCS cases in the tree feed `\033P1000p`, which is not
// it. So every case below enters through `\033Ptmux;`.
//
// The detach is the one that matters. It hands the accumulator to a nested
// `processBytes` and then frees one of two buffers depending on what the inner
// feed left behind, so an error there is a use-after-free or a double free on a
// path an application drives.
//
// Each case below was checked with a print in the branch it names before being
// trusted, because three of the four passed on the first run and one of those
// was passing without arriving.

#include "VT100Fixture.h"

#include "Core/VT/Constants.h"

#include <string>

namespace {

/// A terminal with a grid, ready to take sequences.
void reset(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
}

/// Feeds `bytes` one byte per call, as a reader draining a slow pipe does.
///
/// The batching writer takes a run out of one buffer; a payload split across
/// calls has no run to take, which is the only way to reach the per-byte writer.
void feedByteAtATime(Ubiquitty::Terminal& t, const std::string& bytes)
{
    for (char byte : bytes)
        feed(t, std::string_view(&byte, 1));
}

} // namespace

TEST(dcsTmuxPassthroughRestoresTheOuterAccumulatorWhenTheInnerFeedCompletes)
{
    Ubiquitty::Terminal t;
    reset(t);

    // A complete inner sequence: the payload doubles every ESC, so `\033\033[31m`
    // un-doubles to SGR 31. The inner feed consumes it whole and leaves no
    // accumulator behind, which is the branch that puts the outer buffer back.
    feed(t, "\033Ptmux;\033\033[31mX\033\\");

    // The inner sequence took effect, so control reached the nested feed rather
    // than the payload being dropped at the prefix check.
    const Ubiquitty::Cell* first = UbiquittyTest::cell(t, 0, 0);
    CHECK(first != nullptr);
    CHECK_EQ(static_cast<int>(first->codePoint), static_cast<int>('X'));
    CHECK_EQ(static_cast<int>(first->pen.foregroundIndex), 1); // SGR 31

    // And the terminal keeps taking sequences afterwards, which a freed
    // accumulator still installed as the live one would not survive.
    feed(t, "\033[0m\033]0;title\007Y");
    const Ubiquitty::Cell* next = UbiquittyTest::cell(t, 0, 1);
    CHECK(next != nullptr);
    CHECK_EQ(static_cast<int>(next->codePoint), static_cast<int>('Y'));
}

TEST(dcsTmuxPassthroughKeepsTheInnerAccumulatorWhenThePayloadLeavesOneOpen)
{
    Ubiquitty::Terminal t;
    reset(t);

    // An unterminated OSC inside the payload: the inner feed parks mid-string
    // with bytes accumulated, so the buffer it allocated is the live one and the
    // outer is the one released. The other branch of the same swap.
    feed(t, "\033Ptmux;\033\033]0;partial\033\\");

    // The parser is still inside that OSC, so the terminator arriving later
    // finishes it rather than being taken as text.
    feed(t, "-more\007Z");
    const Ubiquitty::Cell* first = UbiquittyTest::cell(t, 0, 0);
    CHECK(first != nullptr);
    CHECK_EQ(static_cast<int>(first->codePoint), static_cast<int>('Z'));
    CHECK_STR_EQ(t.titleUTF8, "partial-more");
}

TEST(aDCSPayloadStopsAtTheNonImageMaximumAndSaysSoOnce)
{
    Ubiquitty::Terminal t;
    reset(t);

    // Under the ceiling nothing is refused, so the flag below is the ceiling and
    // not the shape of the feed.
    feedByteAtATime(t, "\033Ptmux;" + std::string(1024, 'a'));
    CHECK(!t.dcsTruncated);
    feed(t, "\033\\");

    // Past it, byte at a time — which is how a payload arrives when it is not
    // delivered in one buffer, and the shape a runaway title takes.
    //
    // The clip happens in the batching writer, not the per-byte one, and that is
    // worth stating because it looks the other way round. `appendDCSPayloadRun`
    // is offered every byte first and returns 0 only when the byte at hand ends
    // the run — `BEL`, `ESC`, `CAN`, `SUB`, `ST` — or when the allocator fails. A
    // byte is never one of those, so it batches even in a run of one, clamps its
    // store and consumes the rest. The per-byte writer's own
    // admission branch is therefore reachable only through allocation failure, and
    // no input crosses it; a case aimed there passes on this flag without ever
    // arriving, which is what a print in the branch says and an assertion does
    // not.
    reset(t);
    feedByteAtATime(t, "\033Ptmux;" + std::string(Ubiquitty::VT::maxOSCNonImageSize + 16, 'a'));
    CHECK(t.dcsTruncated);
    CHECK_EQ(t.parser.osc.length, Ubiquitty::VT::maxOSCNonImageSize);

    // Delivered in one buffer instead: the same ceiling, the same flag, so the
    // two delivery shapes agree about where the payload stops.
    Ubiquitty::Terminal bulk;
    reset(bulk);
    feed(bulk, "\033Ptmux;" + std::string(Ubiquitty::VT::maxOSCNonImageSize + 16, 'a'));
    CHECK(bulk.dcsTruncated);
    CHECK_EQ(bulk.parser.osc.length, t.parser.osc.length);
}

TEST(anEscapePairInsideDCSNeedsRoomForBothOfItsBytes)
{
    Ubiquitty::Terminal t;

    // The pair — a doubled ESC, the one escape a payload keeps, since any other
    // byte after an ESC abandons the string — writes two bytes and is admitted
    // only while both fit. Filling to
    // one short of the ceiling and offering a pair settles which of the two the
    // check is about: a writer comparing against its own width refuses here, and
    // one comparing against a single byte would admit and overrun.
    reset(t);
    const int onlyOneByteFree = Ubiquitty::VT::maxOSCNonImageSize - 1;
    feed(t, "\033Ptmux;" + std::string(static_cast<std::size_t>(onlyOneByteFree) - 5, 'a'));
    CHECK_EQ(t.parser.osc.length, onlyOneByteFree);
    feed(t, "\033\033");
    CHECK_EQ(t.parser.osc.length, onlyOneByteFree); // refused whole, not half

    // Two short: both bytes fit, and the pair goes in.
    reset(t);
    const int exactlyTwoBytesFree = Ubiquitty::VT::maxOSCNonImageSize - 2;
    feed(t, "\033Ptmux;" + std::string(static_cast<std::size_t>(exactlyTwoBytesFree) - 5, 'a'));
    CHECK_EQ(t.parser.osc.length, exactlyTwoBytesFree);
    feed(t, "\033\033");
    CHECK_EQ(t.parser.osc.length, Ubiquitty::VT::maxOSCNonImageSize);
}

UBIQUITTY_TEST_MAIN()
