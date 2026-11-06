// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/HostPolicy.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

#include <cstdio>
#include <cstring>

// The host-capability gates: what a hostile host is allowed to do to the
// terminal. Denied must stop the sequence, not merely record a preference.

// The gates are what a hostile host is allowed to do. Denied must actually stop
// the sequence, not merely record a preference.
TEST(test_hostPolicy_denies_title_operations)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    Ubiquitty::Modes::HostPolicy::setTitlePolicy(&t, Ubiquitty::OperationPolicy::Allowed);
    feed(t, "\033]0;allowed\007");
    CHECK(t.titleUTF8 == "allowed");

    Ubiquitty::Modes::HostPolicy::setTitlePolicy(&t, Ubiquitty::OperationPolicy::Denied);
    feed(t, "\033]0;denied\007");
    CHECK(t.titleUTF8 == "allowed"); // unchanged
}

UBIQUITTY_TEST_MAIN()
