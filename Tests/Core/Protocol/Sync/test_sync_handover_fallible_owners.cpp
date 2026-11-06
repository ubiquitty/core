// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "CheckpointContentFixture.h"
#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Session/SessionShare.h"
#include "Core/Session/ShareTerminalLimits.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <string_view>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::ScopedNothrowAllocationRefusal;

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    if (ScopedNothrowAllocationRefusal::refuses(bytes))
        return nullptr;
    return ::operator new(bytes);
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
    if (ScopedNothrowAllocationRefusal::refuses(bytes))
        return nullptr;
    return ::operator new[](bytes);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { ::operator delete[](pointer); }

namespace {

constexpr std::size_t hostStateLimit = 64 * 1024;

HandoverPaneLimits sessionLimits()
{
    HandoverPaneLimits limits;
    limits.maxSessionBytes = 64 * 1024;
    limits.maxLandedPastePaths = 4;
    return limits;
}

SessionFacts sessionWithStatus()
{
    SessionFacts facts;
    facts.sessionID = 17;
    facts.reportedProgramStatus.push_back({ .id = "program",
        .app = "build-tool",
        .title = "Compiling source",
        .message = "The source files are being compiled.",
        .state = Ubiquitty::OSC::ProgramState::Working });
    return facts;
}

} // namespace

TEST(handoverHostTextAllocationRefusalPreservesEveryDestinationOwner)
{
    Ubiquitty::Terminal source;
    source.answerback = "replacement answerback";
    source.currentDirectory = "/replacement/directory/longer";
    source.commandStartedGeneration = 99;
    const auto bytes = encodeHandoverHostState(source, hostStateLimit);
    REQUIRE(bytes);
    Ubiquitty::Terminal destination;
    destination.answerback = "retained answerback";
    destination.currentDirectory = "/retained";
    destination.commandStartedGeneration = 7;
    const auto before = encodeHandoverHostState(destination, hostStateLimit);
    REQUIRE(before);
    const auto* answerbackOwner = destination.answerback.data();
    const auto* directoryOwner = destination.currentDirectory.data();
    const std::array failedSizes { source.answerback.size() + 1, source.currentDirectory.size() + 1 };
    REQUIRE(failedSizes[0] != failedSizes[1]);
    for (const auto failedSize : failedSizes) {
        const auto priorRefusals = ScopedNothrowAllocationRefusal::refusalCount();
        {
            ScopedNothrowAllocationRefusal refusal(failedSize);
            const auto result = restoreHandoverHostState(bytes->view(), destination, hostStateLimit);
            CHECK(!result && result.error() == HandoverBodyError::AllocationRefused);
        }
        CHECK_EQ(ScopedNothrowAllocationRefusal::refusalCount(), priorRefusals + 1);
        CHECK(destination.answerback.data() == answerbackOwner);
        CHECK(destination.currentDirectory.data() == directoryOwner);
        const auto after = encodeHandoverHostState(destination, hostStateLimit);
        REQUIRE(after);
        CHECK(std::ranges::equal(before->view(), after->view()));
    }
}

TEST(handoverHostTextRestorationOwnsCanonicalBytesAfterSourceDestruction)
{
    Ubiquitty::Terminal destination;
    {
        Ubiquitty::Terminal source;
        source.answerback = "replacement answerback";
        source.currentDirectory = "/replacement/directory";
        const auto bytes = encodeHandoverHostState(source, hostStateLimit);
        REQUIRE(bytes);
        REQUIRE(restoreHandoverHostState(bytes->view(), destination, hostStateLimit));
        const auto encodedAgain = encodeHandoverHostState(destination, hostStateLimit);
        REQUIRE(encodedAgain);
        CHECK(std::ranges::equal(bytes->view(), encodedAgain->view()));
    }
    CHECK(destination.answerback == "replacement answerback");
    CHECK(destination.currentDirectory == "/replacement/directory");
}

TEST(handoverSessionStatusTextAllocationFailureIsAnExplicitRefusal)
{
    const auto facts = sessionWithStatus();
    const auto limits = sessionLimits();
    const auto bytes = encodeHandoverSessionFacts(facts, limits);
    REQUIRE(bytes);
    const auto& status = facts.reportedProgramStatus.front();
    const std::array<std::string_view, 4> fields { status.id, status.app, status.title, status.message };
    for (const auto field : fields) {
        REQUIRE(!field.empty());
        const auto priorRefusals = ScopedNothrowAllocationRefusal::refusalCount();
        {
            ScopedNothrowAllocationRefusal refusal(field.size() + 1);
            const auto result = decodeHandoverSessionFacts(bytes->view(), limits);
            CHECK(!result && result.error() == HandoverBodyError::AllocationRefused);
        }
        CHECK_EQ(ScopedNothrowAllocationRefusal::refusalCount(), priorRefusals + 1);
    }
    const auto after = encodeHandoverSessionFacts(facts, limits);
    REQUIRE(after);
    CHECK(std::ranges::equal(bytes->view(), after->view()));
}

TEST(handoverSessionStatusTextRoundTripsWithoutChangingItsWireGrammar)
{
    const auto facts = sessionWithStatus();
    const auto limits = sessionLimits();
    const auto bytes = encodeHandoverSessionFacts(facts, limits);
    REQUIRE(bytes);
    const auto restored = decodeHandoverSessionFacts(bytes->view(), limits);
    REQUIRE(restored);
    CHECK(restored->reportedProgramStatus == facts.reportedProgramStatus);
    const auto encodedAgain = encodeHandoverSessionFacts(*restored, limits);
    REQUIRE(encodedAgain);
    CHECK(std::ranges::equal(bytes->view(), encodedAgain->view()));
}

TEST(handoverCheckpointUsesItsIndependentHostStorageAllowance)
{
    auto limits = Ubiquitty::Session::localShareTerminalLimits(std::size_t { 8 } << 20);
    CHECK_EQ(limits.maxCheckpointStorageCost, std::size_t { 256 } << 20);
    CHECK_EQ(limits.maxScreenCellBytes, std::size_t { 2 } * Ubiquitty::maxRows * Ubiquitty::maxColumns * sizeof(Ubiquitty::Cell));
    CHECK(limits.maxCheckpointStorageCost > measuredClientPolicy().maxCheckpointStorageCost);
    CHECK(limits.maxScreenCellBytes > measuredClientPolicy().maxDecodedScreenCellBytes);
    Ubiquitty::Terminal source;
    Ubiquitty::resize(source, 7, 23);
    REQUIRE(UbiquittyTest::seedCheckpointOwners(source));
    const auto bytes = encodeHandoverTerminal(source, 23, limits);
    REQUIRE(bytes);
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> checkpoint;
    REQUIRE(publisher.capture(source) && publisher.checkpoint(source, 23, checkpoint, baseline));
    const auto plan = checkpointStoragePlan(checkpoint);
    REQUIRE(plan);
    Ubiquitty::Terminal candidate;
    const auto cost = checkpointStorageCost(*plan, candidate);
    REQUIRE(cost);
    for (const auto allowance : { std::size_t { 0 }, *cost - 1 }) {
        limits.maxCheckpointStorageCost = allowance;
        const auto restored = decodeHandoverTerminal(bytes->view(), limits);
        CHECK(!restored && restored.error() == HandoverBodyError::AllocationRefused);
    }
    limits.maxCheckpointStorageCost = *cost;
    auto restored = decodeHandoverTerminal(bytes->view(), limits);
    REQUIRE(restored);
    CHECK(restored->generation == 23);
    CHECK(restored->terminal->rows == source.rows && restored->terminal->columns == source.columns);
    const auto roundTrip = encodeHandoverTerminal(*restored->terminal, 23, limits);
    REQUIRE(roundTrip);
    CHECK(std::ranges::equal(bytes->view(), roundTrip->view()));
}

UBIQUITTY_TEST_MAIN()
