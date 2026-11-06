// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tmuxlaunch.cpp — what a tmux Session field means (Network/TmuxLaunch.h
// mapSessionArgument) and the launch plans built over it. The mapping is the
// one a person meets in three places — the Connect window, Beam and a saved
// tmux host — so it lives in Core and each arm of it is pinned by the argv it
// produces, not by the string alone.

#include "Core/Network/TmuxLaunch.h"

#include <algorithm>
#include <string>
#include <vector>

using Ubiquitty::Network::defaultControlArguments;
using Ubiquitty::Network::mapSessionArgument;
using Ubiquitty::Network::planLocalTmuxLaunch;
using Ubiquitty::Network::planRemoteTmuxLaunch;
using Ubiquitty::Network::TmuxLaunchFailure;

// Blank is the attach-or-create default, a typed command passes through
// behind -CC, and anything else names a session to attach to or create.
TEST(aSessionFieldMapsToTheControlArgumentsItMeans)
{
    CHECK_STR_EQ(mapSessionArgument(""), std::string(defaultControlArguments));
    CHECK_STR_EQ(mapSessionArgument("   "), std::string(defaultControlArguments));
    CHECK_STR_EQ(mapSessionArgument("attach"), "-CC attach");
    CHECK_STR_EQ(mapSessionArgument("attach -t work"), "-CC attach -t work");
    CHECK_STR_EQ(mapSessionArgument("new-session -s scratch"), "-CC new-session -s scratch");
    CHECK_STR_EQ(mapSessionArgument("new-session"), "-CC new-session");
    CHECK_STR_EQ(mapSessionArgument(" work "), "-CC new-session -A -s work");
    // A name that merely starts like a command is still a name.
    CHECK_STR_EQ(mapSessionArgument("attachments"), "-CC new-session -A -s attachments");
}

// The local plan spawns the found tmux with the mapped arguments; with none
// found it is the typed TmuxUnavailable fact carrying where it looked, never an
// empty argv a caller has to read as a failure.
TEST(aLocalPlanIsTheArgvOrTheTypedAbsenceOfTmux)
{
    const auto plan = planLocalTmuxLaunch("/opt/homebrew/bin/tmux", "/usr/bin:/bin", "work");
    CHECK(plan.failure == TmuxLaunchFailure::None);
    const std::vector<std::string> expected { "/opt/homebrew/bin/tmux", "-CC", "new-session", "-A", "-s", "work" };
    CHECK(plan.arguments == expected);
    CHECK(plan.searchedDirectories.empty());

    const auto missing = planLocalTmuxLaunch("", "/usr/bin:/home/user/bin", "work");
    CHECK(missing.failure == TmuxLaunchFailure::TmuxUnavailable);
    CHECK(missing.arguments.empty());
    REQUIRE(!missing.searchedDirectories.empty());
    CHECK_STR_EQ(missing.searchedDirectories.front(), "/opt/homebrew/bin");
    CHECK_STR_EQ(missing.searchedDirectories.back(), "/home/user/bin");
    // A PATH entry already in the fixed prefixes is searched once.
    CHECK_EQ(std::count(missing.searchedDirectories.begin(), missing.searchedDirectories.end(), std::string("/usr/bin")), 1);
}

// The remote plan wraps the mapped tmux in ssh, and a destination ssh would
// read as an option is the typed InvalidHost fact rather than an empty argv.
TEST(aRemotePlanWrapsTheMappedTmuxInSSH)
{
    const auto plan = planRemoteTmuxLaunch("/usr/bin/ssh", "user@build.example", "main");
    CHECK(plan.failure == TmuxLaunchFailure::None);
    REQUIRE(plan.arguments.size() == 6);
    CHECK_STR_EQ(plan.arguments[4], "user@build.example");
    CHECK_STR_EQ(plan.arguments[5], Ubiquitty::Transport::remoteShellCommand("'tmux' '-CC' 'new-session' '-A' '-s' 'main'"));
    CHECK(plan.arguments[5].starts_with("exec /bin/sh -c '"));

    const auto refused = planRemoteTmuxLaunch("/usr/bin/ssh", "-oProxyCommand=evil", "main");
    CHECK(refused.failure == TmuxLaunchFailure::InvalidHost);
    CHECK(refused.arguments.empty());
}

UBIQUITTY_TEST_MAIN()
