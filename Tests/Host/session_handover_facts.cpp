// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Host/HandoverAccess.h"
#include "PAL/Stream.h"
#include "Platform/Host/SessionHandoverFacts.h"

#include <cstring>
#include <unistd.h>

using namespace Ubiquitty;
using namespace Ubiquitty::Host;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

Sync::SessionFacts fixture()
{
    Sync::SessionFacts facts;
    facts.sessionID = 11;
    facts.createdUnix = 12;
    facts.placement = { 13, 14, 15 };
    facts.lastInputClientID = 16;
    facts.lastHumanInputClientID = 17;
    facts.activity = { 18, 19 };
    facts.stampedTypedInputWrites = 20;
    facts.seenPersonFocusGains = 21;
    facts.lastRecordsMilliseconds = 22;
    facts.recordsOwedAtMilliseconds = 23;
    facts.lastActivityStampMilliseconds = 24;
    facts.describedStructuralGeneration = 25;
    facts.recordedActivityGeneration = 26;
    facts.end = { true, true, true, 9 };
    facts.outputOrigin = CapabilityOrigin::Remote;
    facts.driverOriginFloor = CapabilityOrigin::Agent;
    facts.firstInputPrincipal = "token:first-driver";
    facts.hasMultipleInputPrincipals = true;
    facts.isFrameDue = true;
    facts.isDriverFocusRecorded = true;
    facts.pendingGeometry = Sync::PaneGeometry { 30, 90 };
    facts.isWindowSizeDue = true;
    facts.geometryChangesApplied = 27;
    facts.presentedApplied = Sync::SessionFacts::Presented { { 28, 29 }, 30, 31 };
    facts.presentedDueAt = 32;
    facts.presentedStandIn = { 33, 34 };
    facts.unpublishedSince = 35;
    facts.lastEmittedCommandGeneration = 36;
    facts.lastEmittedCommandLineGeneration = 37;
    facts.lastEmittedCommandStartedGeneration = 38;
    facts.wasForegroundShell = false;
    facts.lastForegroundName = "program";
    facts.foregroundGeneration = 39;
    facts.publishedEchoFacts.emplace();
    facts.publishedEchoFacts->isSecureEntry = true;
    facts.publishedEchoFacts->isAtPrompt = true;
    facts.publishedEchoFacts->foregroundGeneration = 49;
    facts.reportedProgramStatus = { { "task", "tool", "waiting", "continue?", OSC::ProgramState::Blocked, OSC::ProgramBlockKind::Question, 42 } };
    facts.reportedActivityGeneration = 40;
    // The frozen cache's old revision is unused; PaneSourceContinuation owns
    // the typed record revision independently of these sampled strings.
    facts.paneFacts = { 0, "directory", "chain", "chain-arguments", "foreground", "command", "tty" };
    facts.didShellExit = true;
    facts.exitReason = std::string("owned reason\0suffix", 19);
    facts.mouseTrackingProcessGroup = 42;
    facts.isAdopted = true;
    facts.historyLinesNotCarried = 43;
    facts.workspaceName = "workspace";
    facts.shouldEndWhenDetached = true;
    facts.sshHost = "example.invalid";
    facts.wasRestoredAfterHostRestart = true;
    facts.wasKilledBySignal = true;
    facts.exitStatusDeadlineMilliseconds = 44;
    facts.observedDirectory = "observed";
    facts.graceExpiresMilliseconds = 45;
    facts.isGraceAwaitingHuman = true;
    facts.landedPastePaths = { "/tmp/ubq-fixture-first-held-path", "/tmp/ubq-fixture-second-held-path" };
    return facts;
}

Sync::HandoverPaneLimits limits()
{
    Sync::HandoverPaneLimits bounds;
    bounds.maxSessionBytes = 64 * 1024;
    bounds.maxLandedPastePaths = 8;
    return bounds;
}

bool equalFacts(const Sync::SessionFacts& first, const Sync::SessionFacts& second)
{
    const auto a = Sync::encodeHandoverSessionFacts(first, limits());
    const auto b = Sync::encodeHandoverSessionFacts(second, limits());
    CHECK(a && b);
    return a && b && std::ranges::equal(a->view(), b->view());
}

} // namespace

TEST(allPortableFactsReachTheActualHostOwner)
{
    auto expected = fixture();
    auto carried = fixture();
    HandoverAccess::Session restored;
    REQUIRE(SessionHandoverFacts::restore(restored, std::move(carried)));
    CHECK(equalFacts(expected, SessionHandoverFacts::capture(restored)));
    CHECK(!restored.ownsLandedPastePaths);
    CHECK(restored.agentSessionGrantIdentities.empty());
    CHECK(!restored.term && !restored.stream && !restored.screenSource && !restored.inputSink && !restored.inputEncoder);
    CHECK(restored.pendingGeometry == std::optional(std::pair { 90, 30 }));
    CHECK(restored.firstInputPrincipal == expected.firstInputPrincipal);
    CHECK(restored.hasMultipleInputPrincipals);
    carried.exitReason.assign(400, 'x');
    CHECK(restored.exitReason == expected.exitReason);
    auto recaptured = SessionHandoverFacts::capture(restored);
    recaptured.landedPastePaths.clear();
    CHECK_EQ(restored.landedPastePaths.size(), 2u);
}

TEST(absentFactsStayAbsentAndNoGrantOrProbeIsRecreated)
{
    Sync::SessionFacts expected;
    expected.sessionID = 1;
    expected.placement = { 1, 1, 1 };
    auto carried = expected;
    HandoverAccess::Session restored;
    REQUIRE(SessionHandoverFacts::restore(restored, std::move(carried)));
    CHECK(equalFacts(expected, SessionHandoverFacts::capture(restored)));
    CHECK(!restored.pendingGeometry && !restored.presentedApplied && !restored.presentedDueAt && !restored.publishedEchoFacts);
    CHECK_EQ(restored.lastForegroundProbeMilliseconds, 0u);
    CHECK(restored.probedForegroundName.empty());
    CHECK(restored.capabilityPolicy.currentIdentity().empty());
    CHECK(restored.effects.isEmpty());
    CHECK(!restored.firstInputPrincipal);
    CHECK(!restored.hasMultipleInputPrincipals);
}

TEST(inputPrincipalHistoryReachesTheSuccessorThroughThePortableCodec)
{
    HandoverAccess::Session predecessor;
    auto facts = fixture();
    const auto firstPrincipal = facts.firstInputPrincipal;
    REQUIRE(SessionHandoverFacts::restore(predecessor, std::move(facts)));
    const auto captured = SessionHandoverFacts::capture(predecessor);
    const auto encoded = Sync::encodeHandoverSessionFacts(captured, limits());
    REQUIRE(encoded);
    auto decoded = Sync::decodeHandoverSessionFacts(encoded->view(), limits());
    REQUIRE(decoded);
    HandoverAccess::Session successor;
    REQUIRE(SessionHandoverFacts::restore(successor, std::move(*decoded)));
    CHECK(successor.firstInputPrincipal == firstPrincipal);
    CHECK(successor.hasMultipleInputPrincipals);
    CHECK(equalFacts(captured, SessionHandoverFacts::capture(successor)));
}

TEST(occupiedReceiverRefusesWithoutConsumingEitherOwner)
{
    HandoverAccess::Session occupied;
    auto first = fixture();
    REQUIRE(SessionHandoverFacts::restore(occupied, std::move(first)));
    const auto before = SessionHandoverFacts::capture(occupied);
    auto second = fixture();
    second.workspaceName = "replacement";
    const auto expected = second;
    CHECK(!SessionHandoverFacts::restore(occupied, std::move(second)));
    CHECK(equalFacts(before, SessionHandoverFacts::capture(occupied)));
    CHECK(equalFacts(expected, second));
}

TEST(abandoningAPrivateSuccessorCannotUnlinkTheHoldersPastedFiles)
{
    char path[] = "/tmp/ubq-handover-cleanup-XXXXXX";
    const int descriptor = ::mkstemp(path);
    REQUIRE(descriptor >= 0);
    ::close(descriptor);
    {
        HandoverAccess::Session holder;
        holder.id = 1;
        holder.placement = { static_cast<Ubiquitty::Session::Layout::SurfaceID>(1),
            static_cast<Ubiquitty::Session::Layout::TabID>(1), static_cast<Ubiquitty::Session::Layout::PaneID>(1) };
        holder.landedPastePaths.emplace_back(path);
        auto facts = SessionHandoverFacts::capture(holder);
        {
            HandoverAccess::Session successor;
            REQUIRE(SessionHandoverFacts::restore(successor, std::move(facts)));
            CHECK(::access(path, F_OK) == 0);
        }
        CHECK(::access(path, F_OK) == 0);
    }
    CHECK(::access(path, F_OK) != 0);
}

UBIQUITTY_TEST_MAIN()
