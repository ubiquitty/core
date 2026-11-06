// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/LinkIndicator.h"
#include "Harness.h"

using namespace Ubiquitty::Session;

TEST(aHealthyLocalLinkHasNoIndicator)
{
    const LinkState state { .phase = LinkPhase::Live, .transport = { .lastHeardMicroseconds = 5'000'000 } };
    CHECK(linkIndicatorFor(state, 0, 5'000'000, {}).kind == LinkIndicatorKind::None);
    CHECK(linkIndicatorText({}).empty());
}

TEST(aSilentLinkShowsItsContactAge)
{
    LinkState state { .phase = LinkPhase::Live, .transport = { .lastHeardMicroseconds = 2'000'000, .isSilent = true } };
    const auto indicator = linkIndicatorFor(state, 0, 5'000'000, {});
    CHECK(indicator.kind == LinkIndicatorKind::NoContact);
    CHECK_EQ(indicator.seconds, 3u);
    CHECK_STR_EQ(linkIndicatorText(indicator), "No contact for 3 s");
    CHECK_EQ(linkIndicatorFor(state, 0, 1'000'000, {}).seconds, 0u);
}

TEST(anInputStallOutranksPictureLag)
{
    LinkState state { .phase = LinkPhase::Live, .transport = { .oldestUnreceiptedAgeMicroseconds = 2'000'000, .isInputUndelivered = true } };
    const auto indicator = linkIndicatorFor(state, 10'000'000, 5'000'000, { .behindFloorMicroseconds = 100'000 });
    CHECK(indicator.kind == LinkIndicatorKind::InputNotDelivered);
    CHECK_EQ(indicator.seconds, 2u);
}

TEST(anEndedPaneOutranksEveryStall)
{
    LinkState state { .phase = LinkPhase::Ended, .transport = { .isSilent = true, .isInputUndelivered = true } };
    CHECK(linkIndicatorFor(state, 10'000'000, 5'000'000, { .behindFloorMicroseconds = 100'000 }).kind == LinkIndicatorKind::Ended);
    state.phase = LinkPhase::Lost;
    CHECK(linkIndicatorFor(state, 10'000'000, 5'000'000, {}).kind == LinkIndicatorKind::Reconnecting);
}

TEST(pictureLagNeedsAnExplicitMeasuredPolicy)
{
    const LinkState state { .phase = LinkPhase::Live };
    CHECK(linkIndicatorFor(state, 10'000'000, 5'000'000, {}).kind == LinkIndicatorKind::None);
    CHECK(linkIndicatorFor(state, 100'000, 5'000'000, { .behindFloorMicroseconds = 100'000 }).kind == LinkIndicatorKind::None);
    CHECK(linkIndicatorFor(state, 100'001, 5'000'000, { .behindFloorMicroseconds = 100'000 }).kind == LinkIndicatorKind::Behind);
}

UBIQUITTY_TEST_MAIN()
