// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_frame_coherence.cpp — when may a pane's frame be taken, and in which class?
//
// FrameCoherence is the rule the daemon, the loopback oracle and the local
// runtime publish by. Each case drives a real Terminal through drain passes on a
// hand-held clock, the way a host does, and asks the rule at chosen instants.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Modes/SyncOutput.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using Ubiquitty::Runtime::coherenceDeadlineMicroseconds;
using Ubiquitty::Runtime::CoherenceTuning;
using Ubiquitty::Runtime::FrameAdmission;
using Ubiquitty::Runtime::FrameClass;
using Ubiquitty::Runtime::FrameCoherence;
using Ubiquitty::Runtime::PassEnd;
using Ubiquitty::Runtime::readQuiescenceMicroseconds;
using Ubiquitty::Runtime::ReadReadiness;
using Ubiquitty::Runtime::refreshIntervalMicroseconds;
using UbiquittyTest::feed;

namespace {

// Every case but the pacing ones asks closer together than a refresh, so the
// rule runs unpaced unless a case asks for the shipped tuning.
constexpr CoherenceTuning unpaced { .isPaced = false };

struct Pane {
    std::unique_ptr<Ubiquitty::Terminal> terminal = std::make_unique<Ubiquitty::Terminal>();
    FrameCoherence coherence;

    explicit Pane(const CoherenceTuning& tuning = unpaced)
        : coherence(tuning)
    {
        Ubiquitty::resize(*terminal, 6, 20);
        // The first frame is structural; take it so each case starts settled.
        pass("ready", 1000);
        const FrameAdmission first = ask(1000 + readQuiescenceMicroseconds);
        CHECK(first.isAdmitted && first.frameClass == FrameClass::Structural);
        take(1000 + readQuiescenceMicroseconds);
    }

    void pass(std::string_view bytes, std::uint64_t at, PassEnd end = PassEnd::WouldBlock)
    {
        feed(*terminal, bytes);
        coherence.noteDrainPass(*terminal, at, end);
    }

    FrameAdmission ask(std::uint64_t at, ReadReadiness readiness = ReadReadiness::Quiet)
    {
        return coherence.admit(*terminal, at, Ubiquitty::Ticks::fromMilliseconds(at / 1000), readiness);
    }

    void take(std::uint64_t at) { coherence.noteFrameTaken(*terminal, at); }

    // Asks, and takes the frame when admitted.
    std::optional<FrameClass> publish(std::uint64_t at)
    {
        const FrameAdmission admission = ask(at);
        if (!admission.isAdmitted)
            return std::nullopt;
        take(at);
        return admission.frameClass;
    }
};

constexpr std::uint64_t quiet = readQuiescenceMicroseconds;

} // namespace

TEST(aResizeIsStructural)
{
    Pane pane;
    Ubiquitty::resize(*pane.terminal, 8, 20);
    pane.pass("x", 10'000);
    CHECK(pane.publish(10'000 + quiet) == FrameClass::Structural);
}

TEST(ordinaryOutputIsSettledOnlyOnceTheReadHasGoneQuiet)
{
    Pane pane;
    pane.pass("a", 10'000);
    CHECK(!pane.ask(10'000).isAdmitted);
    CHECK(!pane.ask(10'000 + quiet - 1).isAdmitted);
    // The zero-timeout poll found the next write already waiting.
    CHECK(!pane.ask(10'000 + quiet, ReadReadiness::Pending).isAdmitted);
    CHECK(pane.publish(10'000 + quiet) == FrameClass::Settled);
    // Nothing changed since: nothing to take.
    CHECK(!pane.ask(50'000).isAdmitted);
}

TEST(theWaitForQuietIsAskedAgainWhenItEnds)
{
    Pane pane;
    pane.pass("a", 10'000);
    CHECK(pane.coherence.microsecondsUntilAsk(*pane.terminal, 10'500, Ubiquitty::Ticks()) == quiet - 500);
    // Quiet now and nothing else holding: ask at once, not at the deadline.
    CHECK(pane.coherence.microsecondsUntilAsk(*pane.terminal, 10'000 + quiet, Ubiquitty::Ticks()) == 0);
    // A bracket or a parser mid-sequence holds it to the deadline of the
    // frame, dirty since the first pass.
    pane.pass("\x1b[?25l", 11'000);
    CHECK(pane.coherence.microsecondsUntilAsk(*pane.terminal, 11'000 + quiet, Ubiquitty::Ticks()) == 10'000 + coherenceDeadlineMicroseconds - 11'000 - quiet);
    pane.pass("\x1b[?25h\x1b[3", 12'000);
    CHECK(pane.coherence.microsecondsUntilAsk(*pane.terminal, 12'000 + quiet, Ubiquitty::Ticks()) == 10'000 + coherenceDeadlineMicroseconds - 12'000 - quiet);
    pane.take(20'000);
    CHECK(!pane.coherence.microsecondsUntilAsk(*pane.terminal, 20'000, Ubiquitty::Ticks()).has_value());
}

TEST(aPassThatSpentItsBudgetIsAFloodPacedAtTheFrameInterval)
{
    Pane pane { CoherenceTuning {} };
    pane.pass("flood", 30'000, PassEnd::Capacity);
    CHECK(pane.publish(30'000) == FrameClass::Flood);
    pane.pass("more", 31'000, PassEnd::Capacity);
    CHECK(!pane.ask(31'000).isAdmitted);
    CHECK(pane.publish(30'000 + refreshIntervalMicroseconds) == FrameClass::Flood);
}

// A flood cut inside an escape sequence is torn like any other frame taken
// mid-sequence; counted as a flood, it would drop out of the forced share.
TEST(aFloodPassCutMidSequenceIsForced)
{
    Pane pane;
    pane.pass("coloured log line \x1b[3", 30'000, PassEnd::Capacity);
    CHECK(pane.publish(30'000) == FrameClass::Forced);
}

// A 2026 block opened and closed inside one read never shows at a pass
// boundary as an open block; the close counter is what makes it a boundary.
TEST(aSynchronizedBlockClosedInsideOneReadMakesABoundary)
{
    Pane pane;
    pane.pass("\x1b[?2026hframe\x1b[?2026l", 10'000);
    CHECK(pane.publish(10'000 + quiet) == FrameClass::Boundary);
}

TEST(anOpenSynchronizedBlockHoldsPastTheDeadlineUntilItsOwnBudget)
{
    Pane pane;
    pane.pass("\x1b[?2026hhalf", 10'000);
    CHECK(!pane.ask(10'000 + coherenceDeadlineMicroseconds * 2).isAdmitted);
    // The block's budget, not the program, ends it.
    Ubiquitty::Modes::SyncOutput::expire(pane.terminal.get());
    CHECK(pane.publish(2'000'000) == FrameClass::Forced);
}

TEST(aParserMidSequenceHoldsUntilGroundOrTheDeadline)
{
    Pane pane;
    pane.pass("text\x1b[3", 10'000);
    CHECK(!pane.ask(10'000 + quiet).isAdmitted);
    pane.pass("1mred", 12'000);
    CHECK(pane.publish(12'000 + quiet) == FrameClass::Settled);

    pane.pass("text\x1b[3", 100'000);
    CHECK(!pane.ask(100'000 + coherenceDeadlineMicroseconds - 1).isAdmitted);
    CHECK(pane.publish(100'000 + coherenceDeadlineMicroseconds) == FrameClass::Forced);
}

TEST(aCursorHiddenRedrawHoldsUntilTheCursorIsShown)
{
    Pane pane;
    pane.pass("\x1b[?25lrow one", 10'000);
    CHECK(pane.coherence.isBracketHolding());
    CHECK(!pane.ask(10'000 + quiet).isAdmitted);
    pane.pass("row two", 14'000);
    CHECK(!pane.ask(14'000 + quiet).isAdmitted);
    pane.pass("row three\x1b[?25h", 18'000);
    CHECK(!pane.coherence.isBracketHolding());
    CHECK(pane.publish(18'000 + quiet) == FrameClass::Settled);
}

// A bracket outliving the deadline releases its frame as forced and then holds
// nothing until the cursor is shown, so a program that hides the cursor for an
// input burst pays the deadline once.
TEST(aBracketOutlivingTheDeadlineIsPersistentUntilTheCursorIsShown)
{
    Pane pane;
    pane.pass("\x1b[?25lkey 1", 10'000);
    CHECK(!pane.ask(10'000 + coherenceDeadlineMicroseconds - 1).isAdmitted);
    CHECK(pane.publish(10'000 + coherenceDeadlineMicroseconds) == FrameClass::Forced);
    CHECK(!pane.coherence.isBracketHolding());
    pane.pass("key 2", 100'000);
    CHECK(pane.publish(100'000 + quiet) == FrameClass::Settled);
    // Shown, then hidden again: a new bracket holds again.
    pane.pass("\x1b[?25h", 200'000);
    CHECK(pane.publish(200'000 + quiet) == FrameClass::Settled);
    pane.pass("\x1b[?25lnext burst", 300'000);
    CHECK(!pane.ask(300'000 + quiet).isAdmitted);
}

TEST(withoutThePersistentRuleEveryKeyOfABurstWaitsTheDeadline)
{
    Pane pane(CoherenceTuning { .isPaced = false, .isPersistentBracketEnabled = false });
    pane.pass("\x1b[?25lkey 1", 10'000);
    CHECK(pane.publish(10'000 + coherenceDeadlineMicroseconds) == FrameClass::Forced);
    pane.pass("key 2", 100'000);
    CHECK(!pane.ask(100'000 + quiet).isAdmitted);
}

// A program that keeps the cursor hidden (a pager, htop, an Ink-style UI) opens
// no bracket: the cursor was already hidden when the last frame was taken.
TEST(aCursorAlreadyHiddenAtTheLastFrameOpensNoBracket)
{
    Pane pane;
    pane.pass("\x1b[?25l", 10'000);
    (void)pane.publish(10'000 + coherenceDeadlineMicroseconds);
    pane.coherence = FrameCoherence();
    pane.take(50'000); // a frame taken with the cursor already hidden
    pane.pass("spinner", 90'000);
    CHECK(!pane.coherence.isBracketHolding());
    CHECK(pane.publish(90'000 + quiet) == FrameClass::Settled);
}

// Shown and hidden again between two frames is a new bracket, even though the
// cursor was hidden at both frames.
TEST(aCursorHiddenAgainBetweenFramesOpensABracket)
{
    Pane pane;
    pane.pass("\x1b[?25l", 10'000);
    (void)pane.publish(10'000 + coherenceDeadlineMicroseconds);
    pane.coherence = FrameCoherence();
    pane.take(50'000);
    pane.pass("done\x1b[?25h typed \x1b[?25lnext", 90'000);
    CHECK(pane.coherence.isBracketHolding());
    CHECK(!pane.ask(90'000 + quiet).isAdmitted);
}

// RIS wipes the program's modes but not the counters the rule compares: a reset
// after a closed block is not a second boundary.
TEST(aHardResetAfterABoundaryIsNotAnotherBoundary)
{
    Pane pane;
    pane.pass("\x1b[?2026hframe\x1b[?2026l", 10'000);
    CHECK(pane.publish(10'000 + quiet) == FrameClass::Boundary);
    pane.pass("\033c", 40'000);
    CHECK(pane.publish(40'000 + quiet) == FrameClass::Settled);
}

// A resize or a palette dirties the pane without a pass; the read went quiet
// long ago, so nothing but the rule's other conditions holds it.
TEST(aHostChangeIsTakenWithoutWaitingForQuiet)
{
    Pane pane;
    pane.coherence.noteHostChange(500'000);
    CHECK(pane.publish(500'000) == FrameClass::Settled);
}

// A bracket that turns persistent on an ask that is refused for another reason
// still released its frame on the deadline: whatever is taken next, while the
// redraw it opened is under way, is not a coherent frame.
TEST(aBracketThatTurnedPersistentOnARefusedAskStillForcesTheFrameItReleases)
{
    Pane pane;
    constexpr std::uint64_t start = 100'000;
    pane.pass("plain output", start);
    pane.pass("\x1b[?25l", start + 20'000);
    // The deadline, counted from the plain output, takes a frame while the
    // bracket is still young; the bracket stays open.
    CHECK(pane.publish(start + coherenceDeadlineMicroseconds) == FrameClass::Forced);
    CHECK(pane.coherence.isBracketHolding());
    // The program keeps writing a byte a millisecond; the ask that ages the
    // bracket to persistent finds the read busy and refuses.
    for (std::uint64_t at = start + 40'000; at <= start + 53'000; at += 1'000)
        pane.pass("x", at);
    CHECK(!pane.ask(start + 53'400).isAdmitted);
    CHECK(!pane.coherence.isBracketHolding());
    for (std::uint64_t at = start + 54'000; at <= start + 55'000; at += 1'000)
        pane.pass("x", at);
    CHECK(pane.publish(start + 55'000 + quiet) == FrameClass::Forced);
    // Taken; the next quiet frame is ordinary again.
    pane.pass("y", start + 70'000);
    CHECK(pane.publish(start + 70'000 + quiet) == FrameClass::Settled);
}

// A DEC 2026 block expired on its own budget left the program's screen half
// composed, whichever ask expired it and whatever pass follows before a frame
// is taken.
TEST(aSynchronizedBlockExpiredBeforeAnotherPassStillForcesTheFrame)
{
    {
        Pane pane;
        pane.pass("\x1b[?2026hhalf", 10'000);
        // Expired by another reader of the terminal, before the next pass.
        Ubiquitty::Modes::SyncOutput::expire(pane.terminal.get());
        pane.pass(" and more", 11'000);
        CHECK(pane.publish(11'000 + quiet) == FrameClass::Forced);
    }
    {
        Pane pane { CoherenceTuning {} };
        pane.pass("\x1b[?2026hhalf", 10'000);
        CHECK(!pane.ask(10'000).isAdmitted); // stamps the block's age
        // A frame taken just before the block's budget runs out, and more
        // written into the still open block.
        constexpr std::uint64_t expired = 10'000 + 1'100'000;
        pane.take(expired - 1'000);
        pane.pass(" and more", expired - 500);
        // The ask that expires the block is refused for the pacing.
        CHECK(!pane.ask(expired).isAdmitted);
        pane.pass(" and the rest", expired + 1'000);
        CHECK(pane.publish(expired + 1'000 + refreshIntervalMicroseconds) == FrameClass::Forced);
    }
}

// The program closing a 2026 block has said the frame is complete; waiting for
// the read to go quiet only delays it, and a host that is not asked again
// would not show it until the program next writes.
TEST(aClosedSynchronizedBlockIsTakenWithoutWaitingForQuiet)
{
    Pane pane;
    pane.pass("\x1b[?2026hframe\x1b[?2026l", 10'000);
    CHECK(pane.coherence.microsecondsUntilAsk(*pane.terminal, 10'000, Ubiquitty::Ticks()) == 0);
    CHECK(pane.publish(10'000) == FrameClass::Boundary);
    // A block closed inside a cursor-hidden redraw is still the bracket's.
    pane.pass("\x1b[?25l\x1b[?2026hpart\x1b[?2026l", 20'000);
    CHECK(!pane.ask(20'000).isAdmitted);
}

namespace {

// Feeds a pass every `gap` microseconds for `duration`, asking at each pass end
// and whenever the rule names, as a host does. Returns the classes taken.
std::vector<FrameClass> steadyWriter(Pane& pane, std::uint64_t start, std::uint64_t gap, std::uint64_t duration)
{
    std::vector<FrameClass> taken;
    std::uint64_t nextPass = start;
    std::uint64_t now = start;
    const std::uint64_t end = start + duration;
    while (now < end) {
        if (now == nextPass) {
            pane.pass("a line of build output\r\n", now);
            nextPass += gap;
        }
        if (const std::optional<FrameClass> frameClass = pane.publish(now))
            taken.push_back(*frameClass);
        const std::optional<std::uint64_t> wait = pane.coherence.microsecondsUntilAsk(*pane.terminal, now, Ubiquitty::Ticks());
        const std::uint64_t nextAsk = wait ? now + std::max<std::uint64_t>(*wait, 1) : nextPass;
        now = std::min(nextPass, nextAsk);
    }
    return taken;
}

} // namespace

// A writer whose gaps never reach the quiet window is still shown at the frame
// interval, as a writer still writing rather than on the deadline: half the
// rate, and every frame counted as forced, is what the deadline would give.
TEST(aWriterThatNeverGoesQuietIsTakenAtTheFrameInterval)
{
    Pane pane { CoherenceTuning {} };
    const std::vector<FrameClass> taken = steadyWriter(pane, 100'000, 1'000, 1'000'000);
    CHECK_MSG(taken.size() >= 50 && taken.size() <= 61, ("frames in a second: " + std::to_string(taken.size())).c_str());
    CHECK(std::ranges::count(taken, FrameClass::Forced) == 0);
    CHECK(std::ranges::count(taken, FrameClass::Flood) == static_cast<std::ptrdiff_t>(taken.size()));
}

// A writer whose gaps just clear the quiet window would be taken after every
// write; no frame is taken closer than the frame interval to the last.
TEST(aWriterJustSlowerThanTheQuietWindowIsPacedAtTheFrameInterval)
{
    Pane pane { CoherenceTuning {} };
    const std::vector<FrameClass> taken = steadyWriter(pane, 100'000, 2'500, 1'000'000);
    CHECK_MSG(taken.size() >= 50 && taken.size() <= 61, ("frames in a second: " + std::to_string(taken.size())).c_str());
    CHECK(std::ranges::count(taken, FrameClass::Forced) == 0);
    // Paced, the next ask waits for the interval as well as for quiet.
    pane.take(2'000'000);
    pane.pass("x", 2'000'100);
    CHECK(pane.coherence.microsecondsUntilAsk(*pane.terminal, 2'000'100 + quiet, Ubiquitty::Ticks()) == refreshIntervalMicroseconds - 100 - quiet);
}

TEST(aTimedPassKeepsTheFirstDirtyEpochAndNeedsNoSecondFrameInterval)
{
    Pane pane;
    pane.coherence.noteHostChange(100'000);
    pane.coherence.noteHostChange(100'100);
    pane.pass("continuous output", 100'000 + refreshIntervalMicroseconds, PassEnd::TimeBudget);
    CHECK(pane.coherence.dirtySinceMicroseconds() == 100'000);
    const auto admission = pane.ask(100'000 + refreshIntervalMicroseconds, ReadReadiness::Pending);
    CHECK(admission.isAdmitted && admission.frameClass == FrameClass::Flood);
}

TEST(aTimedPassPreservesCursorBracketsAndSynchronizedOutput)
{
    for (const std::string_view bytes : { "\x1b[?25lredraw", "\x1b[?2026hcomposing" }) {
        Pane pane;
        pane.coherence.noteHostChange(100'000);
        pane.pass(bytes, 100'000 + refreshIntervalMicroseconds, PassEnd::TimeBudget);
        CHECK(!pane.ask(100'000 + refreshIntervalMicroseconds, ReadReadiness::Pending).isAdmitted);
    }
}

TEST(aTimedPassTakesTheAppliedFrameInsideAnIncompleteCommand)
{
    Pane pane;
    constexpr std::uint64_t startedAt = 100'000;
    constexpr std::uint64_t endedAt = startedAt + refreshIntervalMicroseconds;
    pane.coherence.noteHostChange(startedAt);
    pane.pass("painted\x1b[48;5;", endedAt, PassEnd::TimeBudget);
    CHECK(pane.coherence.dirtySinceMicroseconds() == startedAt);
    CHECK(pane.coherence.microsecondsUntilAsk(*pane.terminal, endedAt, Ubiquitty::Ticks()) == 0);
    const auto admission = pane.ask(endedAt, ReadReadiness::Pending);
    CHECK(admission.isAdmitted && admission.frameClass == FrameClass::Forced);
}

TEST(aTimedPartialCommandStillObservesPresentationPacing)
{
    Pane pane { CoherenceTuning {} };
    constexpr std::uint64_t previousFrameAt = 100'000;
    pane.take(previousFrameAt);
    pane.coherence.noteHostChange(previousFrameAt + 1);
    pane.pass("painted\x1b[48;5;", previousFrameAt + refreshIntervalMicroseconds - 1, PassEnd::TimeBudget);
    CHECK(!pane.ask(previousFrameAt + refreshIntervalMicroseconds - 1, ReadReadiness::Pending).isAdmitted);
    CHECK(pane.coherence.microsecondsUntilAsk(*pane.terminal, previousFrameAt + refreshIntervalMicroseconds - 1, Ubiquitty::Ticks()) == 1);
    const auto admission = pane.ask(previousFrameAt + refreshIntervalMicroseconds, ReadReadiness::Pending);
    CHECK(admission.isAdmitted && admission.frameClass == FrameClass::Forced);
}

TEST(aPendingPassDoesNotReportReadQuiescence)
{
    Pane pane;
    pane.pass("pending", 100'000, PassEnd::Pending);
    CHECK(!pane.ask(100'000 + quiet, ReadReadiness::Pending).isAdmitted);
    const auto observedQuiet = pane.ask(100'000 + quiet, ReadReadiness::Quiet);
    CHECK(observedQuiet.isAdmitted && observedQuiet.frameClass == FrameClass::Settled);
}

TEST(aPendingPassKeepsTheParserGuardUntilTheCommandCompletes)
{
    Pane pane;
    pane.coherence.noteHostChange(100'000);
    pane.pass("painted\x1b[48;5;", 100'000 + refreshIntervalMicroseconds, PassEnd::Pending);
    CHECK(!pane.ask(100'000 + refreshIntervalMicroseconds, ReadReadiness::Pending).isAdmitted);
    CHECK(pane.coherence.microsecondsUntilAsk(*pane.terminal, 100'000 + refreshIntervalMicroseconds, Ubiquitty::Ticks()) == refreshIntervalMicroseconds);
    pane.pass("196m", 100'001 + refreshIntervalMicroseconds, PassEnd::Pending);
    const auto admission = pane.ask(100'001 + refreshIntervalMicroseconds, ReadReadiness::Pending);
    CHECK(admission.isAdmitted && admission.frameClass == FrameClass::Flood);
}

TEST(aTakenTimedBudgetDoesNotReleaseTheNextDirtyCommand)
{
    Pane pane;
    pane.pass("painted\x1b[48;5;", 100'000, PassEnd::TimeBudget);
    REQUIRE(pane.ask(100'000, ReadReadiness::Pending).isAdmitted);
    pane.take(100'000);
    pane.coherence.noteHostChange(200'000);
    feed(*pane.terminal, "196mnext\x1b[38;5;");
    CHECK(!pane.ask(200'000, ReadReadiness::Pending).isAdmitted);
    CHECK(pane.coherence.microsecondsUntilAsk(*pane.terminal, 200'000, Ubiquitty::Ticks()) == coherenceDeadlineMicroseconds);
}

TEST(aTakenTimedBudgetPreservesTheNextMidReadBoundaryClass)
{
    Pane pane;
    pane.pass("painted", 100'000, PassEnd::TimeBudget);
    REQUIRE(pane.ask(100'000, ReadReadiness::Pending).isAdmitted);
    pane.take(100'000);
    pane.coherence.noteHostChange(200'000);
    feed(*pane.terminal, "\x1b[?2026hnext\x1b[?2026l");
    pane.coherence.noteMidReadClose(*pane.terminal, 200'000);
    const auto admission = pane.ask(200'000, ReadReadiness::Pending);
    CHECK(admission.isAdmitted && admission.frameClass == FrameClass::Boundary);
}

UBIQUITTY_TEST_MAIN()
