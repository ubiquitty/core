// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The `-e` / `--` command-line grammar.
//
// The cases are written against what real terminals do rather than against what
// this parser happens to do: xterm registers `-e` as XrmoptionSkipLine and takes
// the rest of the line, konsole's own help calls it "this option will catch all
// following arguments", and urxvt's manual says it must be last. Every case
// below is one of those behaviours, so a rewrite that keeps the tests green
// keeps the compatibility they stand for.

#include "Harness.h"

#include "Core/Runtime/LaunchArguments.h"

using Ubiquitty::Runtime::launchDefaultTitle;
using Ubiquitty::Runtime::launchEndsWithChild;
using Ubiquitty::Runtime::LaunchRequest;
using Ubiquitty::Runtime::parseLaunchArguments;

namespace {

// argv as the C runtime hands it over, terminal name included.
LaunchRequest parse(std::initializer_list<const char*> arguments)
{
    std::vector<const char*> argv(arguments);
    return parseLaunchArguments(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST(testNoMarkerLeavesTheWholeCommandLineToTheTerminal)
{
    const LaunchRequest request = parse({ "ubiquitty", "--columns", "80" });
    CHECK(request.status == LaunchRequest::Status::None);
    CHECK(request.command.empty());
    // The terminal keeps every argument, so its own scanners see what they
    // always saw when nobody asked for a child command.
    CHECK_EQ(request.terminalArgumentCount, 3);
}

TEST(testTheMarkerTakesTheWholeTailNotOneArgument)
{
    const LaunchRequest request = parse({ "ubiquitty", "-e", "printf", "%s|", "a", "b" });
    CHECK(request.status == LaunchRequest::Status::Command);
    CHECK_EQ((int)request.command.size(), 4);
    CHECK(request.command[0] == "printf");
    CHECK(request.command[3] == "b");
}

TEST(testOptionsAfterTheMarkerBelongToTheChild)
{
    // The case that decides whether hyphen transparency is real: these tokens
    // are spelled exactly like the terminal's own flags, and a scanner reading
    // past terminalArgumentCount would act on them.
    const LaunchRequest request = parse({ "ubiquitty", "-e", "env", "--snapshot", "out.png", "--install-terminfo" });
    CHECK(request.status == LaunchRequest::Status::Command);
    CHECK_EQ((int)request.command.size(), 4);
    CHECK(request.command[1] == "--snapshot");
    CHECK(request.command[3] == "--install-terminfo");
    // Only "ubiquitty" is the terminal's.
    CHECK_EQ(request.terminalArgumentCount, 1);
}

TEST(testTheMarkerNeedNotBeLastBecauseItTakesTheRest)
{
    // urxvt's manual asks for the marker last; xterm implements that by taking
    // the tail rather than by diagnosing anything. A terminal flag written
    // after the marker is the child's, and that is not an error.
    const LaunchRequest request = parse({ "ubiquitty", "-e", "echo", "--hold" });
    CHECK(request.status == LaunchRequest::Status::Command);
    CHECK_EQ((int)request.command.size(), 2);
    CHECK(request.command[1] == "--hold");
    CHECK(!request.shouldHold); // the child's argument, not the terminal's
}

TEST(testHoldBeforeTheMarkerIsTheTerminals)
{
    const LaunchRequest request = parse({ "ubiquitty", "--hold", "-e", "false" });
    CHECK(request.status == LaunchRequest::Status::Command);
    CHECK(request.shouldHold);
    CHECK_EQ((int)request.command.size(), 1);
    CHECK(request.command[0] == "false");
}

TEST(testDoubleDashSpellsTheSameThing)
{
    const LaunchRequest dashE = parse({ "ubiquitty", "-e", "printf", "-b" });
    const LaunchRequest doubleDash = parse({ "ubiquitty", "--", "printf", "-b" });
    CHECK(dashE.status == doubleDash.status);
    CHECK(dashE.command == doubleDash.command);
    CHECK_EQ(dashE.terminalArgumentCount, doubleDash.terminalArgumentCount);
}

TEST(testAMarkerWithNothingAfterItIsAnError)
{
    // Not a fallback to the login shell: a script whose "$@" expanded to
    // nothing asked for a program, and a shell is not one.
    for (const char* marker : { "-e", "--" }) {
        const char* argv[] = { "ubiquitty", marker };
        const LaunchRequest request = parseLaunchArguments(2, argv);
        CHECK(request.status == LaunchRequest::Status::Error);
        CHECK(request.command.empty());
        CHECK(!request.error.empty());
    }
}

TEST(testAnEmptyProgramNameIsAnError)
{
    const LaunchRequest request = parse({ "ubiquitty", "-e", "" });
    CHECK(request.status == LaunchRequest::Status::Error);
    CHECK(request.command.empty());
}

TEST(testABareWordIsNotACommand)
{
    // Linux reads argv[1] as a mode name and Wscons as a device path. Taking
    // bare words would redefine both command lines, so the marker is required.
    const LaunchRequest request = parse({ "ubiquitty", "snapshot", "out.png" });
    CHECK(request.status == LaunchRequest::Status::None);
    CHECK(request.command.empty());
    CHECK_EQ(request.terminalArgumentCount, 3);
}

TEST(testTheFirstMarkerWinsSoASecondOneIsTheChilds)
{
    const LaunchRequest request = parse({ "ubiquitty", "-e", "sh", "-c", "--", "echo" });
    CHECK(request.status == LaunchRequest::Status::Command);
    CHECK_EQ((int)request.command.size(), 4);
    CHECK(request.command[2] == "--");
}

TEST(testTheTitleIsTheProgramsBasename)
{
    CHECK(launchDefaultTitle(parse({ "ubiquitty", "-e", "/bin/ls", "-la" })) == "ls");
    CHECK(launchDefaultTitle(parse({ "ubiquitty", "-e", "ls" })) == "ls");
    // Not the joined command line, which is what a user would read as the name
    // of a program they did not run.
    CHECK(launchDefaultTitle(parse({ "ubiquitty", "-e", "/usr/bin/env", "VAR=1", "cmd" })) == "env");
    // No command, no title.
    CHECK(launchDefaultTitle(parse({ "ubiquitty" })).empty());
    // A trailing slash names a directory; the whole spelling beats an empty
    // title, which would leave the window unlabelled.
    CHECK(launchDefaultTitle(parse({ "ubiquitty", "-e", "/usr/bin/" })) == "/usr/bin/");
}

TEST(testAnEmptyCommandLineIsSurvivable)
{
    CHECK(parseLaunchArguments(0, nullptr).status == LaunchRequest::Status::None);
    CHECK_EQ(parseLaunchArguments(0, nullptr).terminalArgumentCount, 0);
    const char* argv[] = { "ubiquitty" };
    CHECK(parseLaunchArguments(1, argv).status == LaunchRequest::Status::None);
}

TEST(testASessionEndsWithTheProgramUnlessHoldIsAsked)
{
    // The lifetime rule, asked of Core rather than re-derived per frontend:
    // three frontends answering one question is three chances to answer it
    // differently, and the one that drifts is the one nobody runs a suite in.
    CHECK(launchEndsWithChild(parse({ "ubiquitty", "-e", "true" })));
    CHECK(launchEndsWithChild(parse({ "ubiquitty", "--", "true" })));
    CHECK(!launchEndsWithChild(parse({ "ubiquitty", "--hold", "-e", "true" })));

    // No command means a shell session, which outlives nothing in particular —
    // the ordinary window a user closes when they are done with it.
    CHECK(!launchEndsWithChild(parse({ "ubiquitty" })));
    CHECK(!launchEndsWithChild(parse({ "ubiquitty", "--columns", "80" })));

    // An unusable command line ends nothing, because it starts nothing.
    CHECK(!launchEndsWithChild(parse({ "ubiquitty", "-e" })));
}

UBIQUITTY_TEST_MAIN()
