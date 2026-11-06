// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_publish_census.cpp — does the census see each frame for what it was?
//
// PublishCensus watches the coherence rule from the terminal's side: it counts
// each published frame in the class the rule took it in, says on its own
// whether the frame landed inside a cursor-hidden redraw, and times how long
// the frame waited. Each case drives a real Terminal the way the daemon does
// and asks what one frame was seen as. Which class a frame is taken in is the
// rule's, tested in Tests/Core/Runtime/test_frame_coherence.cpp.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/PublishCensus.h"
#include "Core/TerminalOperations.h"

#include <memory>

using Ubiquitty::Protocol::Sync::CensusVerdict;
using Ubiquitty::Protocol::Sync::PublishCensus;
using Ubiquitty::Runtime::FrameClass;
using UbiquittyTest::feed;

namespace {

struct Pane {
    std::unique_ptr<Ubiquitty::Terminal> terminal = std::make_unique<Ubiquitty::Terminal>();
    PublishCensus census;

    Pane()
    {
        Ubiquitty::resize(*terminal, 6, 20);
        pass("ready", 1000);
        census.notePublish(*terminal, 1000, FrameClass::Structural);
    }

    void pass(std::string_view bytes, std::uint64_t at)
    {
        feed(*terminal, bytes);
        census.noteDirty(at);
    }

    CensusVerdict publish(std::uint64_t at, FrameClass frameClass = FrameClass::Settled) { return census.notePublish(*terminal, at, frameClass); }
};

} // namespace

TEST(eachFrameIsCountedInTheClassTheRuleTookItIn)
{
    Pane pane;
    pane.pass("a", 10'000);
    CHECK(pane.publish(12'000, FrameClass::Forced).frameClass == FrameClass::Forced);
    pane.pass("b", 20'000);
    (void)pane.publish(22'000, FrameClass::Flood);
    CHECK(pane.census.frameCount(FrameClass::Structural) == 1);
    CHECK(pane.census.frameCount(FrameClass::Forced) == 1);
    CHECK(pane.census.frameCount(FrameClass::Flood) == 1);
    CHECK(pane.census.frameCount(FrameClass::Settled) == 0);
}

TEST(aFrameIsTimedFromTheFirstPassThatDirtiedIt)
{
    Pane pane;
    pane.pass("a", 10'000);
    pane.pass("b", 14'000);
    const CensusVerdict verdict = pane.publish(26'000);
    CHECK(!verdict.isInsideRedrawBracket);
    CHECK(verdict.holdMicroseconds == 16'000);
    CHECK(pane.census.holdHistogram()[PublishCensus::holdBucket(16'000)] == 1);
    CHECK(pane.census.longestHoldMicroseconds() == 16'000);
}

TEST(aFrameInsideACursorHiddenRepaintIsCounted)
{
    Pane pane;
    pane.pass("\x1b[?25lrow one", 10'000);
    const CensusVerdict torn = pane.publish(12'000);
    CHECK(torn.isInsideRedrawBracket);
    pane.pass("row two\x1b[?25h", 13'000);
    CHECK(!pane.publish(30'000).isInsideRedrawBracket);
    CHECK(pane.census.redrawBracketFrameCount() == 1);
}

// A program that keeps the cursor hidden (a pager, htop, an Ink-style UI) opens
// no bracket: the cursor was already hidden when the last frame was taken.
TEST(aCursorAlreadyHiddenAtTheLastFrameOpensNoBracket)
{
    Pane pane;
    pane.pass("\x1b[?25l", 10'000);
    (void)pane.publish(10'000);
    pane.pass("spinner", 90'000);
    CHECK(!pane.publish(90'000).isInsideRedrawBracket);
}

// Shown and hidden again between two frames is a new bracket, even though the
// cursor was hidden at both frames.
TEST(aCursorHiddenAgainBetweenFramesOpensABracket)
{
    Pane pane;
    pane.pass("\x1b[?25l", 10'000);
    (void)pane.publish(10'000);
    pane.pass("done\x1b[?25h typed \x1b[?25lnext", 40'000);
    CHECK(pane.publish(40'000).isInsideRedrawBracket);
}

// The census sees a frame taken mid-sequence or inside an open 2026 block for
// itself, whatever class the rule took it in: a rule that calls such a frame
// coherent is caught here rather than agreeing with itself.
TEST(aFrameTakenMidSequenceOrInsideASynchronizedBlockIsSeenIncoherent)
{
    Pane pane;
    pane.pass("text\x1b[3", 10'000);
    CHECK(pane.publish(12'000, FrameClass::Settled).isIncoherent);
    pane.pass("1m\x1b[?2026hhalf", 20'000);
    CHECK(pane.publish(22'000, FrameClass::Settled).isIncoherent);
    pane.pass("\x1b[?2026l", 30'000);
    CHECK(!pane.publish(32'000).isIncoherent);
    CHECK(pane.census.incoherentFrameCount() == 2);
}

TEST(theHoldHistogramBucketsByPowersOfTwo)
{
    CHECK(PublishCensus::holdBucket(0) == 0);
    CHECK(PublishCensus::holdBucket(1023) == 0);
    CHECK(PublishCensus::holdBucket(1024) == 1);
    CHECK(PublishCensus::holdBucket(2047) == 1);
    CHECK(PublishCensus::holdBucket(2048) == 2);
    CHECK(PublishCensus::holdBucket(std::uint64_t { 1 } << 40) == PublishCensus::holdBucketCount - 1);
}

UBIQUITTY_TEST_MAIN()
