// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Sync/HandoverGround.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/Runtime/HandoverQuiesce.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>

using namespace Ubiquitty;
using namespace Ubiquitty::Runtime;
using Protocol::Sync::terminalHandoverBlockers;

namespace {

constexpr QuiesceTuning bounds { 100, 200, 3 };

std::unique_ptr<Terminal> makeTerminal()
{
    auto terminal = std::make_unique<Terminal>();
    resize(*terminal, 6, 20);
    return terminal;
}

struct Pane {
    std::unique_ptr<Terminal> terminal = makeTerminal();
    FrameCoherence coherence { CoherenceTuning { .isPaced = false } };

    Pane()
    {
        pass("ready", 1'000);
        CHECK(publish(3'000) == FrameClass::Structural);
    }

    void pass(std::string_view bytes, std::uint64_t now, PassEnd end = PassEnd::WouldBlock)
    {
        processBytes(terminal.get(), bytes);
        coherence.noteDrainPass(*terminal, now, end);
    }

    std::optional<FrameClass> publish(std::uint64_t now, ReadReadiness readiness = ReadReadiness::Quiet)
    {
        const auto admission = coherence.admit(*terminal, now, Ticks::fromMilliseconds(now / 1'000), readiness);
        if (!admission.isAdmitted)
            return std::nullopt;
        coherence.noteFrameTaken(*terminal, now);
        return admission.frameClass;
    }
};

void increment(void* calls) { ++*static_cast<int*>(calls); }

} // namespace

TEST(aNaturalCoherentPublicationMakesThePaneReady)
{
    Pane pane;
    HandoverQuiesce quiesce({ 10'000, 10'000, 32 }, 4'000);
    pane.pass("text", 5'000);
    quiesce.notePass(*pane.terminal, pane.publish(5'000), 5'000);
    CHECK(quiesce.phase() == QuiescePhase::Natural);
    const auto published = pane.publish(7'000);
    REQUIRE(published == FrameClass::Settled);
    quiesce.notePass(*pane.terminal, published, 7'000);
    CHECK(quiesce.phase() == QuiescePhase::Ready);
    CHECK(quiesce.failure() == QuiesceFailure::None);
    CHECK_EQ(quiesce.takeGroundReadAllowance(*pane.terminal, 50'000), 0u);
    CHECK(quiesce.phase() == QuiescePhase::Ready);
}

TEST(aProgramBoundaryCanCompleteWithMoreBytesStillWaiting)
{
    Pane pane;
    HandoverQuiesce quiesce({ 10'000, 10'000, 32 }, 4'000);
    pane.pass("\x1b[?2026htext\x1b[?2026l", 5'000);
    const auto published = pane.publish(5'000, ReadReadiness::Pending);
    REQUIRE(published == FrameClass::Boundary);
    quiesce.notePass(*pane.terminal, published, 5'000);
    CHECK(quiesce.phase() == QuiescePhase::Ready);
}

TEST(groundAloneAndIncoherentPublicationsNeverQualify)
{
    auto terminal = makeTerminal();
    REQUIRE(terminalHandoverBlockers(*terminal).isEmpty());
    HandoverQuiesce quiesce(bounds, 0);
    for (const auto published : { std::optional<FrameClass> {}, std::optional { FrameClass::Flood }, std::optional { FrameClass::Forced }, std::optional { FrameClass::Structural } }) {
        quiesce.notePass(*terminal, published, 0);
        CHECK(quiesce.phase() == QuiescePhase::Natural);
    }
    quiesce.notePass(*terminal, std::nullopt, 100);
    CHECK(quiesce.phase() == QuiescePhase::ReadsFrozen);
    CHECK_EQ(quiesce.takeGroundReadAllowance(*terminal, 100), 0u);
    quiesce.notePass(*terminal, std::nullopt, 300);
    CHECK(quiesce.failure() == QuiesceFailure::Deadline);
}

TEST(aFloodMidCSIUsesOnlyTheBytesNeededToReachGround)
{
    Pane pane;
    HandoverQuiesce quiesce({ 100, 10'000, 1 }, 4'000);
    pane.pass("\x1b[31", 4'050, PassEnd::Capacity);
    const auto forced = pane.publish(4'050);
    REQUIRE(forced == FrameClass::Forced);
    quiesce.notePass(*pane.terminal, forced, 4'050);
    CHECK(quiesce.phase() == QuiescePhase::Natural);
    quiesce.notePass(*pane.terminal, std::nullopt, 4'100);
    REQUIRE(quiesce.phase() == QuiescePhase::ReadsFrozen);
    REQUIRE(quiesce.takeGroundReadAllowance(*pane.terminal, 4'101) == 1);
    pane.pass("m", 4'102);
    quiesce.completeGroundRead(1, 4'102);
    CHECK_EQ(quiesce.groundBytesRead(), 1u);
    CHECK_EQ(quiesce.takeGroundReadAllowance(*pane.terminal, 4'102), 0u);
    quiesce.notePass(*pane.terminal, pane.publish(4'102), 4'102);
    CHECK(quiesce.phase() == QuiescePhase::ReadsFrozen);
    const auto settled = pane.publish(6'102);
    REQUIRE(settled == FrameClass::Settled);
    quiesce.notePass(*pane.terminal, settled, 6'102);
    CHECK(quiesce.phase() == QuiescePhase::Ready);
    CHECK_EQ(quiesce.groundBytesRead(), 1u);
}

TEST(anUnterminatedStringSpendsExactlyItsByteBudget)
{
    auto terminal = makeTerminal();
    processBytes(terminal.get(), "\x1b]0;");
    HandoverQuiesce quiesce(bounds, 0);
    CHECK_EQ(quiesce.takeGroundReadAllowance(*terminal, 99), 0u);
    for (std::size_t index = 0; index < bounds.maxGroundBytes; ++index) {
        REQUIRE(quiesce.takeGroundReadAllowance(*terminal, 100) == 1);
        processBytes(terminal.get(), "x");
        quiesce.completeGroundRead(1, 100);
    }
    const auto parserState = terminal->parser.parseState;
    const auto length = terminal->parser.osc.length;
    CHECK_EQ(quiesce.takeGroundReadAllowance(*terminal, 100), 0u);
    CHECK(quiesce.phase() == QuiescePhase::Failed);
    CHECK(quiesce.failure() == QuiesceFailure::ByteBudget);
    CHECK_EQ(quiesce.groundBytesRead(), bounds.maxGroundBytes);
    CHECK(terminal->parser.parseState == parserState);
    CHECK_EQ(terminal->parser.osc.length, length);
    quiesce.notePass(*terminal, FrameClass::Settled, 300);
    CHECK(quiesce.failure() == QuiesceFailure::ByteBudget);
}

TEST(wouldBlockSpendsNoBytesAndDoesNotRestartTheDeadline)
{
    auto terminal = makeTerminal();
    processBytes(terminal.get(), "\x1b]0;");
    HandoverQuiesce quiesce(bounds, 0);
    for (const std::uint64_t now : { 100u, 200u, 299u }) {
        REQUIRE(quiesce.takeGroundReadAllowance(*terminal, now) == 1);
        quiesce.completeGroundRead(0, now);
    }
    CHECK_EQ(quiesce.groundBytesRead(), 0u);
    CHECK_EQ(quiesce.takeGroundReadAllowance(*terminal, 300), 0u);
    CHECK(quiesce.failure() == QuiesceFailure::Deadline);
}

TEST(aLateFirstObservationCannotExtendTheFrozenWindow)
{
    auto terminal = makeTerminal();
    HandoverQuiesce quiesce(bounds, 50);
    quiesce.notePass(*terminal, FrameClass::Settled, 350);
    CHECK(quiesce.phase() == QuiescePhase::Failed);
    CHECK(quiesce.failure() == QuiesceFailure::Deadline);
}

TEST(asynchronousWorkWaitsWithoutReadingOrRunningItsCallbacks)
{
    auto terminal = makeTerminal();
    terminal->asyncDecodes.kittyDecode = std::make_unique<TaskQueue>();
    int calls = 0;
    REQUIRE(terminal->asyncDecodes.kittyDecode->submit(increment, &calls));
    REQUIRE(calls == 1);
    terminal->isOSC52ClipboardReadInFlight = true;
    HandoverQuiesce quiesce(bounds, 0);
    quiesce.notePass(*terminal, FrameClass::Settled, 100);
    CHECK(quiesce.phase() == QuiescePhase::ReadsFrozen);
    CHECK_EQ(quiesce.takeGroundReadAllowance(*terminal, 100), 0u);
    CHECK_EQ(calls, 1);
    CHECK_EQ(terminal->asyncDecodes.kittyDecode->pendingCount(), 1);
    CHECK(terminal->isOSC52ClipboardReadInFlight);
    REQUIRE(terminal->asyncDecodes.kittyDecode->drain(increment) == 1);
    terminal->isOSC52ClipboardReadInFlight = false;
    quiesce.notePass(*terminal, std::nullopt, 101);
    CHECK(quiesce.phase() == QuiescePhase::ReadsFrozen);
    quiesce.notePass(*terminal, FrameClass::Settled, 102);
    CHECK(quiesce.phase() == QuiescePhase::Ready);
    CHECK_EQ(calls, 2);
}

TEST(observationNeverEndsSynchronizedOutputOrAnOpenGrapheme)
{
    auto terminal = makeTerminal();
    terminal->modes.graphemeClustering = true;
    processBytes(terminal.get(), "e\xcc\x81");
    REQUIRE(terminal->parser.clusterActive);
    terminal->modes.syncOutput = true;
    const auto clusterLength = terminal->parser.clusterCodepointLength;
    const auto clusterLast = terminal->parser.clusterLast;
    HandoverQuiesce quiesce(bounds, 0);
    quiesce.notePass(*terminal, FrameClass::Boundary, 100);
    CHECK(quiesce.phase() == QuiescePhase::ReadsFrozen);
    quiesce.notePass(*terminal, FrameClass::Settled, 300);
    CHECK(quiesce.failure() == QuiesceFailure::Deadline);
    CHECK(terminal->modes.syncOutput);
    CHECK(terminal->parser.clusterActive);
    CHECK_EQ(terminal->parser.clusterCodepointLength, clusterLength);
    CHECK_EQ(terminal->parser.clusterLast, clusterLast);
}

TEST(anOpaqueOwnerPreventsReadsEvenWithAnUnfinishedSequence)
{
    auto terminal = makeTerminal();
    processBytes(terminal.get(), "\x1b[31");
    const auto closeFile = [](std::FILE* file) { std::fclose(file); };
    std::unique_ptr<std::FILE, decltype(closeFile)> sink(std::tmpfile(), closeFile);
    REQUIRE(sink);
    terminal->sessionLogFile = sink.get();
    HandoverQuiesce logging(bounds, 0);
    CHECK_EQ(logging.takeGroundReadAllowance(*terminal, 100), 0u);
    CHECK(logging.phase() == QuiescePhase::ReadsFrozen);
    CHECK(terminal->sessionLogFile == sink.get());
    CHECK_EQ(std::ftell(sink.get()), 0);
    terminal->sessionLogFile = nullptr;

    terminal->integration.tmux.state = Protocol::Tmux::TmuxState::Line;
    HandoverQuiesce integration(bounds, 0);
    CHECK_EQ(integration.takeGroundReadAllowance(*terminal, 100), 0u);
    CHECK(integration.phase() == QuiescePhase::ReadsFrozen);
    CHECK(terminal->integration.tmux.state == Protocol::Tmux::TmuxState::Line);
    terminal->integration.tmux.state = Protocol::Tmux::TmuxState::Passthrough;
}

TEST(aCursorHiddenRedrawWaitsForItsActualCoherentClose)
{
    Pane pane;
    HandoverQuiesce quiesce({ 30'000, 10'000, 32 }, 4'000);
    pane.pass("\x1b[?25lrepaint", 5'000);
    quiesce.notePass(*pane.terminal, pane.publish(7'000), 7'000);
    CHECK(pane.coherence.isBracketHolding());
    CHECK(quiesce.phase() == QuiescePhase::Natural);
    pane.pass("\x1b[?25h", 8'000);
    const auto settled = pane.publish(10'000);
    REQUIRE(settled == FrameClass::Settled);
    quiesce.notePass(*pane.terminal, settled, 10'000);
    CHECK(quiesce.phase() == QuiescePhase::Ready);
}

TEST(aFrozenFloodIsNotRelabeledAsSettled)
{
    Pane pane;
    HandoverQuiesce quiesce({ 100, 40'000, 32 }, 4'000);
    pane.pass("flood", 4'050, PassEnd::Capacity);
    const auto flood = pane.publish(4'100, ReadReadiness::Pending);
    REQUIRE(flood == FrameClass::Flood);
    quiesce.notePass(*pane.terminal, flood, 4'100);
    CHECK(quiesce.phase() == QuiescePhase::ReadsFrozen);
    CHECK_EQ(quiesce.takeGroundReadAllowance(*pane.terminal, 4'100), 0u);
    quiesce.notePass(*pane.terminal, std::nullopt, 44'100);
    CHECK(quiesce.failure() == QuiesceFailure::Deadline);
}

TEST(readLeasesCannotOverlapOrAdmitAFrameBeforeCompletion)
{
    auto terminal = makeTerminal();
    processBytes(terminal.get(), "\x1b[");
    HandoverQuiesce overlap(bounds, 0);
    REQUIRE(overlap.takeGroundReadAllowance(*terminal, 100) == 1);
    CHECK_EQ(overlap.takeGroundReadAllowance(*terminal, 100), 0u);
    CHECK(overlap.failure() == QuiesceFailure::ReadContract);
    HandoverQuiesce premature(bounds, 0);
    REQUIRE(premature.takeGroundReadAllowance(*terminal, 100) == 1);
    processBytes(terminal.get(), "m");
    premature.notePass(*terminal, FrameClass::Settled, 101);
    CHECK(premature.failure() == QuiesceFailure::ReadContract);
}

TEST(unsolicitedAndOversizedReadCompletionsFailClosed)
{
    auto terminal = makeTerminal();
    processBytes(terminal.get(), "\x1b[");
    HandoverQuiesce unsolicited(bounds, 0);
    unsolicited.completeGroundRead(1, 0);
    CHECK(unsolicited.failure() == QuiesceFailure::ReadContract);
    HandoverQuiesce oversized(bounds, 0);
    REQUIRE(oversized.takeGroundReadAllowance(*terminal, 100) == 1);
    oversized.completeGroundRead(2, 101);
    CHECK(oversized.failure() == QuiesceFailure::ReadContract);
    CHECK_EQ(oversized.groundBytesRead(), 0u);
}

TEST(aReadCompletingAtTheDeadlineIsCountedButNeverAdmitted)
{
    auto terminal = makeTerminal();
    processBytes(terminal.get(), "\x1b[");
    HandoverQuiesce quiesce(bounds, 0);
    REQUIRE(quiesce.takeGroundReadAllowance(*terminal, 299) == 1);
    processBytes(terminal.get(), "m");
    quiesce.completeGroundRead(1, 300);
    CHECK_EQ(quiesce.groundBytesRead(), 1u);
    quiesce.notePass(*terminal, FrameClass::Settled, 300);
    CHECK(quiesce.failure() == QuiesceFailure::Deadline);
}

TEST(zeroBoundsAndClockRegressionAreBoundedWithoutOverflow)
{
    auto terminal = makeTerminal();
    processBytes(terminal.get(), "\x1b[");
    HandoverQuiesce noTime({ 0, 0, 0 }, 0);
    noTime.notePass(*terminal, std::nullopt, 0);
    CHECK(noTime.failure() == QuiesceFailure::Deadline);
    HandoverQuiesce noBytes({ 0, 100, 0 }, 0);
    CHECK_EQ(noBytes.takeGroundReadAllowance(*terminal, 0), 0u);
    CHECK(noBytes.failure() == QuiesceFailure::ByteBudget);
    constexpr auto last = std::numeric_limits<std::uint64_t>::max();
    HandoverQuiesce nearEnd({ 5, 10, 1 }, last - 20);
    nearEnd.notePass(*terminal, std::nullopt, last - 15);
    CHECK(nearEnd.phase() == QuiescePhase::ReadsFrozen);
    nearEnd.notePass(*terminal, std::nullopt, last - 6);
    CHECK(nearEnd.phase() == QuiescePhase::ReadsFrozen);
    nearEnd.notePass(*terminal, std::nullopt, last - 5);
    CHECK(nearEnd.failure() == QuiesceFailure::Deadline);
    HandoverQuiesce regressed(bounds, 100);
    regressed.notePass(*terminal, std::nullopt, 99);
    CHECK(regressed.failure() == QuiesceFailure::ClockRegressed);
}

UBIQUITTY_TEST_MAIN()
