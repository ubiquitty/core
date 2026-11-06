// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

//
// Tests/Core/Runtime/test_launchcommand.cpp — the LaunchCommand value type: the
// $SHELL→passwd→/bin/zsh resolver, the interactive flag, and the display
// derivations (joined command line + login-shell basename). Pure, links only
// LaunchCommand.cpp + libc.
//

#include "Core/Runtime/LaunchCommand.h"
#include "PAL/Stream.h"

#include <cstdio>
#include <cstdlib>

using Ubiquitty::Runtime::LaunchCommand;

TEST(launchcommand)
{

    // loginShell honors $SHELL, appends -i, derives the display name.
    ::setenv("SHELL", "/opt/homebrew/bin/fish", 1);
    {
        const LaunchCommand command = LaunchCommand::loginShell();
        CHECK(command.executablePath() == "/opt/homebrew/bin/fish");
        CHECK(command.arguments().size() == 2);
        CHECK(command.arguments()[1] == "-i");
        CHECK(command.displayShellName() == "fish");
        CHECK(command.displayName() == "/opt/homebrew/bin/fish -i");
        CHECK(!command.empty());
    }

    // Non-interactive: no -i.
    {
        const LaunchCommand command = LaunchCommand::loginShell(false);
        CHECK(command.arguments().size() == 1);
        CHECK(command.arguments()[0] == "/opt/homebrew/bin/fish");
    }

    // Empty $SHELL falls back (passwd entry, else /bin/zsh) — assert it resolves.
    ::unsetenv("SHELL");
    {
        const LaunchCommand command = LaunchCommand::loginShell();
        CHECK(!command.empty());
        CHECK(!command.executablePath().empty());
        CHECK(command.executablePath().front() == '/'); // an absolute path
    }

    // fromArguments + display (tmux -CC).
    {
        const LaunchCommand command = LaunchCommand::fromArguments({ "/usr/bin/tmux", "-CC", "new" });
        CHECK(command.executablePath() == "/usr/bin/tmux");
        CHECK(command.arguments().size() == 3);
        CHECK(command.displayShellName() == "tmux");
        CHECK(command.displayName() == "/usr/bin/tmux -CC new");
        CHECK(command.characterLocalePolicy() == Ubiquitty::PAL::CharacterLocalePolicy::Auto);
    }

    // Character-locale policy is a launch attribute, independent of argv/display.
    {
        LaunchCommand command = LaunchCommand::fromArguments({ "/usr/bin/ssh", "host" });
        command.setCharacterLocalePolicy(Ubiquitty::PAL::CharacterLocalePolicy::UTF8);
        CHECK(command.characterLocalePolicy() == Ubiquitty::PAL::CharacterLocalePolicy::UTF8);
        CHECK(command.displayName() == "/usr/bin/ssh host");
    }

    // A login-shell argv[0] ("-zsh") strips the leading '-' for the display name.
    {
        const LaunchCommand command = LaunchCommand::fromArguments({ "-zsh" });
        CHECK(command.displayShellName() == "zsh");
    }

    // A default-constructed command is empty.
    {
        const LaunchCommand command;
        CHECK(command.empty());
        CHECK(command.displayShellName().empty());
    }
}

// The start directory a ⌘T tab inherits. Home answers empty because the login
// program reaches it by itself, and asking costs a flag and a re-exec; both
// frontends asked this privately, so one of them opened the window that paid.
TEST(startDirectoryLeavesHomeToLogin)
{
    CHECK(LaunchCommand::startDirectoryFor("/home/user/Projects", "/home/user") == "/home/user/Projects");
    CHECK_MSG(LaunchCommand::startDirectoryFor("/home/user", "/home/user").empty(), "home is login's own");
    CHECK_MSG(LaunchCommand::startDirectoryFor("", "/home/user").empty(), "nothing inherited is login's own");

    // The two arguments reach here from different sources — a directory sampled
    // from a process, a home read from the platform — so the one spelling
    // difference that shows up between them is folded.
    CHECK(LaunchCommand::startDirectoryFor("/home/user/", "/home/user").empty());
    CHECK(LaunchCommand::startDirectoryFor("/home/user", "/home/user/").empty());
    CHECK_MSG(LaunchCommand::startDirectoryFor("/home/user2", "/home/user") == "/home/user2",
        "a home that prefixes a directory's name is not that directory");

    // Root is not home, and trimming must not eat it.
    CHECK(LaunchCommand::startDirectoryFor("/", "/home/user") == "/");
}

// The seed a title shows before the first process walk. It is spelled the way
// the walk will spell it, so the title does not read one name and then flip to
// another a poll interval later.
TEST(titleSeedNameMatchesWhatTheWalkWillReport)
{
    LaunchCommand shell = LaunchCommand::fromArguments({ "/bin/zsh" });
    shell.setArgument0Style(Ubiquitty::PAL::Argument0Style::LoginShell);
    CHECK_MSG(shell.titleSeedName() == "-zsh", "a login shell is exec'd with the dash the walk reports");
    CHECK_MSG(shell.displayShellName() == "zsh", "the display name is the same shell without it");

    // A program named on a command line is not exec'd login-style, so a dash
    // would be a name it does not have.
    LaunchCommand program = LaunchCommand::fromArguments({ "/usr/bin/vim", "notes.txt" });
    program.setArgument0Style(Ubiquitty::PAL::Argument0Style::ProgramPath);
    CHECK(program.titleSeedName() == "vim");

    // An argv already carrying the dash is not given a second one.
    LaunchCommand dashed = LaunchCommand::fromArguments({ "-zsh" });
    dashed.setArgument0Style(Ubiquitty::PAL::Argument0Style::LoginShell);
    CHECK(dashed.titleSeedName() == "-zsh");

    // Nothing to name is nothing to show, not a bare dash.
    LaunchCommand empty;
    CHECK(empty.titleSeedName().empty());
}

UBIQUITTY_TEST_MAIN()
