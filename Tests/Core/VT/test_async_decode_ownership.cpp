// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Who may run an async decode's completion.
//
// A sixel or kitty decode submits mid-parse, and its completion writes deep into
// the terminal that submitted it — cursor position, image slots, the scroll
// region — and frees the pixel buffer an evicted image held. So it may only run
// on the thread that owns that terminal, and only that terminal's own drain may
// pick it up.
//
// While the queues were process-global, `drain` popped every terminal's jobs, so
// whichever pane parsed next ran another pane's completion holding only its own
// model lock. Both decodes now drain inside the parse of their own terminator,
// so no job waits for a later drain at all; these cases pin that the image lands
// on the terminal that parsed it, before any drain, and that nothing is left
// pending for another terminal's drain, a reset, or a teardown to find.

#include "Harness.h"
#include "VT100Fixture.h"

#include "Core/Image/TerminalImage.h"
#include "Core/VT/Constants.h"
#include "Core/VT/Parser.h"

#include <string>

namespace {

int liveImageCount(const Ubiquitty::Terminal& t)
{
    int n = 0;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i)
        if (t.images.stored[i].used && t.images.stored[i].pixels)
            ++n;
    return n;
}

// A two-by-one sixel: raster attributes, one colour register, one sixel band.
constexpr char sixelFrame[] = "\033P0;0;0q\"1;1;2;1#0;2;100;0;0#0~~\033\\";

// A payload whose synchronized-output clear lands before the end, so
// processBytesUpToSyncClear returns with consumed < length and skips the drain
// at the end of the read. A decode that waited for that drain would sit in the
// queue here; the sixel must not.
constexpr char sixelThenSyncClear[] = "\033P0;0;0q\"1;1;2;1#0;2;100;0;0#0~~\033\\"
                                      "\033[?2026h"
                                      "x"
                                      "\033[?2026l"
                                      "trailing";

} // namespace

// The image is on its terminal by the time the DCS terminator has been parsed:
// the read that stops short of its end-of-read drain still shows it, and a
// later drain has nothing to add. What a deferred install cost was the cursor:
// the completion anchors from the DCS origin, and the newline an emitter prints
// after its image had already moved the cursor by the time a later drain ran.
TEST(testASixelIsOnItsTerminalAtItsTerminator)
{
    Ubiquitty::Terminal a;
    Ubiquitty::Terminal b;
    Ubiquitty::hardReset(a);
    Ubiquitty::hardReset(b);

    const int consumed = Ubiquitty::processBytesUpToSyncClear(&a, std::string_view(sixelThenSyncClear, sizeof(sixelThenSyncClear) - 1));
    CHECK(consumed < static_cast<int>(sizeof(sixelThenSyncClear) - 1)); // the end-of-read drain did not run
    CHECK(liveImageCount(a) == 1); // and the image is there regardless
    CHECK(a.asyncDecodes.sixel == nullptr || a.asyncDecodes.sixel->pendingCount() == 0);

    // B parses and drains, and finds nothing of A's; A's own drain adds nothing.
    feed(b, "hello");
    Ubiquitty::drainAsyncDecodes(a);
    CHECK(liveImageCount(a) == 1);
    CHECK(liveImageCount(b) == 0);
}

// A drain in the ordinary path still delivers: the per-terminal queue must not
// have turned the common case into a no-op.
TEST(testSixelStillArrivesOnTheSameTerminal)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, sixelFrame);
    CHECK(liveImageCount(t) == 1); // processBytes drains at the end
}

// A terminal torn down right after its sixel takes the image with it. The
// assertion a unit test can make is that destruction completes and nothing of
// the departed terminal's surfaces on another; the leak it also guards is what
// a sanitizer build sees.
TEST(testASixelGoesWithTheTerminalThatParsedIt)
{
    Ubiquitty::Terminal survivor;
    Ubiquitty::hardReset(survivor);
    {
        Ubiquitty::Terminal doomed;
        Ubiquitty::hardReset(doomed);
        Ubiquitty::processBytesUpToSyncClear(&doomed, std::string_view(sixelThenSyncClear, sizeof(sixelThenSyncClear) - 1));
        CHECK(liveImageCount(doomed) == 1); // installed as it goes out of scope
    }
    feed(survivor, "hello");
    Ubiquitty::drainAsyncDecodes(survivor);
    CHECK(liveImageCount(survivor) == 0);
}

// A payload may legitimately declare a large canvas — the raster attribute sets
// the size decode reports — and a bite holds as many headers as fit in it.
// Installing each at its own terminator is what keeps a bite from holding a
// canvas per header until a drain at its end.
TEST(testABurstOfSixelsHoldsNoCanvasPending)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // 2048x2048 declared, one painted pixel: 8 MiB of canvas per 20 bytes of wire.
    std::string burst;
    for (int i = 0; i < 40; ++i)
        burst += "\033Pq\"1;1;2048;2048~\033\\";
    // Stop short of the end so the bite's own end-of-buffer drain never runs.
    burst += "\033[?2026h"
             "x"
             "\033[?2026l"
             "trailing";

    const int consumed = Ubiquitty::processBytesUpToSyncClear(&t, burst);
    CHECK(consumed < static_cast<int>(burst.size())); // the end-of-read drain did not run
    CHECK(t.asyncDecodes.sixel != nullptr);
    if (t.asyncDecodes.sixel) {
        // Each canvas was installed at its own terminator; a deferred install
        // held one per header in the bite, forty of them here.
        CHECK(t.asyncDecodes.sixel->pendingCount() == 0);
    }
    CHECK(liveImageCount(t) > 0);
}

// A payload whose work is set by its length rather than its canvas is refused
// before it is submitted. `!2048~$` repeated repaints one band without end: the
// canvas stays four million pixels while the emissions run to billions, and the
// decode happens inside the pane's model lock.
TEST(testSixelRefusedWhenTheWorkOutrunsItsCanvas)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // A small declared canvas keeps the case cheap: the budget scales with the
    // canvas, so a 64x6 one is exceeded by a few thousand repaints rather than
    // the hundreds of thousands a full-screen canvas would need.
    std::string attack = "\033Pq\"1;1;64;6";
    for (int i = 0; i < 2000; ++i)
        attack += "!64~$"; // 128,000 emissions against a budget of 64 * 1 * 512
    attack += "\033\\";
    feed(t, attack.c_str());
    CHECK(liveImageCount(t) == 0); // never submitted, so never decoded

    // The same grammar within budget still renders: the rule is a repaint count,
    // not a ban on `!` and `$`.
    std::string withinBudget = "\033Pq\"1;1;64;6";
    for (int i = 0; i < 16; ++i)
        withinBudget += "#1!64~$";
    withinBudget += "\033\\";
    feed(t, withinBudget.c_str());
    CHECK(liveImageCount(t) == 1);
}

// Every legacy DCS Pa aspect-ratio selector resolves through the table: 0/1/5/6
// take the 2:1 arm, 2 the 5:1, 3/4 the 3:1, 7/8/9 the 1:1, and an unknown
// selector falls to the default. Without a raster "Pan;Pad command the table
// is the only stretch authority, so each frame here must land in its arm.
// Each image still lands — the selector only changes the vertical stretch.
TEST(testSixelAspectRatioSelectors)
{
    for (int selector : { 0, 1, 5, 6, 2, 3, 4, 7, 8, 9, 99 }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        char frame[64];
        std::snprintf(frame, sizeof(frame), "\033P%d;0;0q#0;2;100;0;0#0~~\033\\", selector);
        feed(t, frame);
        CHECK(liveImageCount(t) == 1);
    }
}

// A raster Pan;Pad command overrides the Pa table; a pan:pad ratio that would
// stretch to zero is clamped to 1:1.
TEST(testSixelRasterStretchClampedToOne)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033P0;0;0q\"1;4;2;1#0;2;100;0;0#0~~\033\\"); // pan=1 pad=4
    CHECK(liveImageCount(t) == 1);
}

// A sixel DCS that never accumulates a payload (no bytes between the final and
// ST) finalizes with nothing to decode: no image, parser back at ground.
TEST(testSixelEmptyPayloadIsSkipped)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033P0;0;0q\033\\");
    CHECK(liveImageCount(t) == 0);
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
    feed(t, "x");
    CHECK(liveImageCount(t) == 0); // text after it still parses as text
}

// The accumulator starts small and doubles on demand: a payload past the
// initial capacity exercises the growth path and still decodes to an image.
// The grown allocation is read before the terminator, because finishing the
// image releases it.
TEST(testSixelAccumulatorGrowsPastInitialCapacity)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    std::string frame = "\033P0;0;0q\"1;1;2;1";
    // Repaint one band to exceed the accumulator's capacity within the image dimension limit.
    for (int pass = 0; pass < 70; ++pass) {
        frame.append(1000, '~');
        frame += '$';
    }
    feed(t, frame.c_str());
    CHECK(t.images.sixelAccumulatorCapacity > 65536);
    feed(t, "\033\\");
    CHECK(liveImageCount(t) == 1);
    CHECK(t.images.sixelAccumulatorCapacity <= 65536);
}

// The per-byte parser path appends through its own accumulator arms — the
// reserve on the first byte, the doubling at the capacity boundary, the store
// — which the batched run above never touches. The frame still decodes.
TEST(testSixelPerByteFeedCrossesTheCapacityBoundary)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    std::string frame = "\033P0;0;0q\"1;1;2;1";
    for (int pass = 0; pass < 70; ++pass) {
        frame.append(1000, '~');
        frame += '$';
    }
    for (const char byte : frame)
        Ubiquitty::processByte(&t, static_cast<uint8_t>(byte));
    CHECK(t.images.sixelAccumulatorCapacity > 65536);
    for (const char byte : std::string("\033\\"))
        Ubiquitty::processByte(&t, static_cast<uint8_t>(byte));
    // The per-byte driver defers the async-decode drain to its caller.
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(liveImageCount(t) == 1);
    CHECK(t.images.sixelAccumulatorCapacity <= 65536);
}

// Past the 16 MB ceiling the accumulator latches overflow and drops the rest
// — batched to the latch, then per byte, so both writers' overflow arms run.
// The oversize frame is refused rather than decoded truncated.
TEST(testSixelAccumulatorOverflowLatchesAcrossBothWriters)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    std::string frame = "\033P0;0;0q\"1;1;2;1";
    frame.append(17 * 1024 * 1024, '~'); // past maxSixelAccumulatorCapacity
    feed(t, frame.c_str());
    CHECK(t.images.sixelAccumulatorOverflow == true);
    // The latch short-circuits the per-byte writer too.
    Ubiquitty::processByte(&t, static_cast<uint8_t>('~'));
    Ubiquitty::processByte(&t, static_cast<uint8_t>('~'));
    CHECK(t.images.sixelAccumulatorLength <= 16 * 1024 * 1024);
    feed(t, "\033\\");
    CHECK(liveImageCount(t) == 0); // an overflowed frame never becomes an image
    // The next frame starts clean.
    feed(t, "\033P0;0;0q#0;2;100;0;0#0~~\033\\");
    CHECK(liveImageCount(t) == 1);
}

// A reset after the sixel disposes it with the rest of the session, and the
// drain that follows has no stale completion to bring it back.
TEST(testAResetAfterASixelLeavesNoImage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(sixelThenSyncClear, sizeof(sixelThenSyncClear) - 1));
    CHECK(liveImageCount(t) == 1);
    Ubiquitty::hardReset(t);
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(liveImageCount(t) == 0);
}

UBIQUITTY_TEST_MAIN()
