// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/PaneFactsCapture.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"

TEST(unchangedPaneFactsKeepTheirOwnedStorageAndRevision)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 24, 80);
    Ubiquitty::Protocol::Sync::PaneHostFacts host;
    host.processChain = "a long enough process chain to live outside small-string storage";
    Ubiquitty::processBytes(&terminal, "\x1b]1337;SetUserVar=project=dWJpcXVpdHR5\x1b\\");
    Ubiquitty::Runtime::PaneFactsCapture capture;
    capture.refresh(terminal, host);
    const auto* facts = capture.factsFor(&terminal);
    REQUIRE(facts);
    REQUIRE(facts->userVariables.size() == 1);
    const auto revision = facts->revision;
    const auto generation = capture.generation();
    const auto* chainStorage = facts->processChain.data();
    const auto* variablesStorage = facts->userVariables.data();
    for (unsigned index = 0; index < 10; ++index)
        capture.refresh(terminal, host);
    CHECK_EQ(capture.generation(), generation);
    CHECK_EQ(facts->revision, revision);
    CHECK(facts->processChain.data() == chainStorage);
    CHECK(facts->userVariables.data() == variablesStorage);
}

TEST(eachPaneReportRefreshesTheCanonicalFactsWithoutAnyCellsChanging)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 24, 80);
    Ubiquitty::Protocol::Sync::PaneHostFacts host;
    Ubiquitty::Runtime::PaneFactsCapture capture;
    capture.refresh(terminal, host);
    const auto feed = [&](std::string_view bytes) {
        const auto before = capture.generation();
        Ubiquitty::processBytes(&terminal, bytes);
        capture.refresh(terminal, host);
        CHECK_EQ(capture.generation(), before + 1);
        const auto* facts = capture.factsFor(&terminal);
        REQUIRE(facts);
        auto canonical = Ubiquitty::Protocol::Sync::paneFactsOf(terminal, host);
        canonical.revision = facts->revision;
        CHECK(*facts == canonical);
    };
    feed("\x1b]2;project tests\x1b\\");
    feed("\x1b]7;file://devbox/Projects/ubiquitty\x1b\\");
    feed("\x1b]6;file:///Projects/ubiquitty/README.md\x1b\\");
    feed("\x1b]9;4;1;42\x1b\\");
    feed("\x1b]1337;SetBadgeFormat=dGVzdHM=\x1b\\");
    feed("\x1b]1337;SetUserVar=project=dWJpcXVpdHR5\x1b\\");
    feed("\x1b]1337;SetUserVar=project=\x1b\\");
    feed("\x1b]133;D;7\x1b\\");
    CHECK_EQ(capture.factsFor(&terminal)->exitStatus.value_or(-1), 7);
}

TEST(aPaneFactsCaptureNeverBorrowsThePreviousModelsRecord)
{
    Ubiquitty::Terminal first;
    Ubiquitty::Terminal second;
    Ubiquitty::resize(first, 24, 80);
    Ubiquitty::resize(second, 24, 80);
    Ubiquitty::Runtime::PaneFactsCapture capture;
    capture.refresh(first, {});
    CHECK(capture.factsFor(&first));
    CHECK(!capture.factsFor(&second));
    CHECK(!capture.factsFor(nullptr));
    const auto before = capture.generation();
    Ubiquitty::Protocol::Sync::PaneHostFacts adopted;
    adopted.isAdopted = true;
    capture.refresh(second, adopted);
    CHECK_EQ(capture.generation(), before + 1);
    CHECK(!capture.factsFor(&first));
    REQUIRE(capture.factsFor(&second));
    CHECK(capture.factsFor(&second)->isAdopted);
}

TEST(activityAloneDoesNotAdvanceTheFactsRevision)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 24, 80);
    Ubiquitty::Runtime::PaneFactsCapture capture;
    Ubiquitty::Protocol::Sync::PaneHostFacts host;
    capture.refresh(terminal, host);
    const auto revision = capture.factsFor(&terminal)->revision;
    host.activity.lastOutputUnixMilliseconds = 42;
    capture.refresh(terminal, host);
    CHECK_EQ(capture.factsFor(&terminal)->revision, revision);
    CHECK_EQ(capture.factsFor(&terminal)->activity.lastOutputUnixMilliseconds, 42u);
}

UBIQUITTY_TEST_MAIN()
