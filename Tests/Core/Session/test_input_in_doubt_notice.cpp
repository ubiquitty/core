// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_input_in_doubt_notice.cpp — what a pane says while some of its input
// may not have reached the host (Session/InputInDoubtNotice.h). A notice
// stands exactly while a decision is awaited: input that only waits for a
// connection, or that was discarded, asks the person nothing.

#include "Core/Session/InputInDoubtNotice.h"

#include <string>

using Ubiquitty::Protocol::Sync::PaneInputStatus;
using Ubiquitty::Session::inputInDoubtNotice;

TEST(aPaneWithNothingInDoubtShowsNoNotice)
{
    CHECK(!inputInDoubtNotice({}).has_value());
    CHECK(!inputInDoubtNotice({ .inDoubt = 0, .waiting = 3, .discarded = 0 }).has_value());
    CHECK(!inputInDoubtNotice({ .inDoubt = 0, .waiting = 0, .discarded = 2 }).has_value());
}

TEST(aPaneWithInputInDoubtSaysWhyTheChoiceIsThePersons)
{
    const auto notice = inputInDoubtNotice({ .inDoubt = 1, .waiting = 2, .discarded = 0 });
    REQUIRE(notice.has_value());
    CHECK_STR_EQ(notice->message, "Some of what you typed may not have reached the host");
    CHECK(notice->detail.find("twice") != std::string::npos);
    CHECK_STR_EQ(notice->resendButton, "Send Again");
    CHECK_STR_EQ(notice->discardButton, "Discard");
}

UBIQUITTY_TEST_MAIN()
