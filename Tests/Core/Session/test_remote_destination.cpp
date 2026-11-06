// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_remote_destination.cpp — the name a frontend shows for the far side of
// a remote session (Core/Session/RemoteDestination.h). Every progress line
// that says where a paste is going reads this, so a command shape it gets
// wrong names the wrong machine to the user.

#include "Core/Session/RemoteDestination.h"

using Ubiquitty::Session::remoteDestinationLabel;

TEST(testTheDestinationIsTheFirstOperandAfterSSH)
{
    CHECK(remoteDestinationLabel("ssh build-box") == "build-box");
    CHECK(remoteDestinationLabel("ssh build-box uname -a") == "build-box");
    // A path spelling, and a shell that was handed the whole line.
    CHECK(remoteDestinationLabel("/usr/bin/ssh build-box") == "build-box");
    CHECK(remoteDestinationLabel("  ssh   build-box  ") == "build-box");
}

TEST(testTheAccountIsNotTheMachine)
{
    CHECK(remoteDestinationLabel("ssh user@build-box") == "build-box");
    CHECK(remoteDestinationLabel("ssh user@10.0.0.4 -- tmux") == "10.0.0.4");
}

TEST(testFlagsAreNotDestinations)
{
    CHECK(remoteDestinationLabel("ssh -t build-box") == "build-box");
    // A flag that takes a value takes the token after it: the port is not a
    // host, and neither is the identity file.
    CHECK(remoteDestinationLabel("ssh -p 2222 build-box") == "build-box");
    CHECK(remoteDestinationLabel("ssh -i ~/.ssh/id_ed25519 build-box") == "build-box");
    CHECK(remoteDestinationLabel("ssh -o StrictHostKeyChecking=no build-box") == "build-box");
    // Bundled shorts: the trailing letter decides whether the next token is
    // an argument, so -tp eats the port and -tv does not.
    CHECK(remoteDestinationLabel("ssh -tp 2222 build-box") == "build-box");
    CHECK(remoteDestinationLabel("ssh -tv build-box") == "build-box");
}

TEST(testACommandThatNamesNoHostAnswersEmpty)
{
    // Not an ssh command at all: a local login shell names no destination.
    CHECK(remoteDestinationLabel("/bin/zsh -l").empty());
    CHECK(remoteDestinationLabel("").empty());
    // ssh with nothing after it, and ssh whose last token is a flag's value.
    CHECK(remoteDestinationLabel("ssh").empty());
    CHECK(remoteDestinationLabel("ssh -p 2222").empty());
    // A binary whose name merely contains ssh is not ssh.
    CHECK(remoteDestinationLabel("sshpass -p secret build-box").empty());
}

UBIQUITTY_TEST_MAIN()
