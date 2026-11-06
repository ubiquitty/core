// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_login_session_command — the argument vector a window's shell is launched
// through.
//
// The PAL seam is covered elsewhere; this covers what Core builds on top of it,
// which is where a wrong flag turns into a window that opens on an error
// message instead of a shell. The flags are not decoration: -p carries the
// environment, -f skips authentication for the calling user, and -l is what
// keeps an inherited working directory — dropping it moves every ⌘T to the home
// directory.
//
// Written against the kind rather than against a platform macro, so the arm
// that does not wrap an argument vector is asserted to leave it alone rather
// than skipped.

#include "Harness.h"

#include "Core/Runtime/LaunchCommand.h"
#include "PAL/PAL.h"

#include <array>
#include <string>

namespace {

namespace Rt = Ubiquitty::Runtime;
namespace P = Ubiquitty::PAL;

std::string palString(int (*query)(char*, int))
{
    std::array<char, 1024> buffer {};
    const int n = query(buffer.data(), static_cast<int>(buffer.size()));
    return n > 0 ? std::string(buffer.data(), static_cast<std::size_t>(n)) : std::string();
}

bool wrapsAnArgumentVector() { return P::loginSessionKind() == P::LoginSessionKind::ThroughLoginProgram; }

} // namespace

// A window with nothing to inherit lands in the home directory, which the login
// program reaches by itself — so the vector stays at three words and the shell
// is exec'd by the program rather than re-exec'd through a wrapper.
TEST(testAHomeWindowLaunchesTheLoginProgramDirectly)
{
    const Rt::LaunchCommand command = Rt::LaunchCommand::loginSession();
    const std::span<const std::string> arguments = command.arguments();

    if (!wrapsAnArgumentVector()) {
        // The plain shell, unwrapped: this arm records the session elsewhere.
        CHECK(arguments.size() >= 1);
        CHECK(arguments[0] != palString(&P::loginSessionProgram));
        return;
    }

    CHECK(arguments.size() == 3);
    CHECK(arguments[0] == palString(&P::loginSessionProgram));
    CHECK(arguments[1] == "-pf");
    CHECK(arguments[2] == palString(&P::systemLoginUserName));
    CHECK(command.initialDirectory().empty());
}

// An inherited directory takes -l and a re-exec, because -l stops the login
// program from exec'ing the shell itself. The leading dash in the re-exec'd
// name is what makes the shell source its login files; without it the inherited
// directory costs the user their startup profile.
TEST(testAnInheritedDirectoryKeepsTheDirectoryAndTheLoginShell)
{
    const Rt::LaunchCommand command = Rt::LaunchCommand::loginSession("/tmp");
    const std::span<const std::string> arguments = command.arguments();

    CHECK(command.initialDirectory() == "/tmp");

    if (!wrapsAnArgumentVector()) {
        CHECK(arguments[0] != palString(&P::loginSessionProgram));
        return;
    }

    CHECK(arguments.size() == 6);
    CHECK(arguments[0] == palString(&P::loginSessionProgram));
    CHECK(arguments[1] == "-pfl");
    CHECK(arguments[2] == palString(&P::systemLoginUserName));
    CHECK(arguments[3] == "/bin/bash");
    CHECK(arguments[4] == "-c");

    // `exec -la <name> <path>`: the name is the path's last component, and the
    // two must agree — a name from one shell and a path from another launches
    // the second wearing the first's identity.
    const std::string& reexec = arguments[5];
    const std::string shell = Rt::LaunchCommand::loginShell().executablePath();
    const std::string name = shell.substr(shell.find_last_of('/') + 1);
    CHECK(reexec == "exec -la " + name + " " + shell);
}

// The two shapes differ only where the directory forces them to. Checked as a
// pair because the failure that matters is one shape drifting: a -pf that grew
// an -l would move every window to the home directory, and a -pfl that lost it
// would do the same to every inherited one.
TEST(testOnlyTheInheritedShapeCarriesTheDirectoryFlag)
{
    if (!wrapsAnArgumentVector())
        return;

    const Rt::LaunchCommand home = Rt::LaunchCommand::loginSession();
    const Rt::LaunchCommand inherited = Rt::LaunchCommand::loginSession("/tmp");

    CHECK(home.arguments()[1] == "-pf");
    CHECK(inherited.arguments()[1] == "-pfl");
    CHECK(home.arguments()[0] == inherited.arguments()[0]);
    CHECK(home.arguments()[2] == inherited.arguments()[2]);
}

// A split joins the window's session and must not open another, so the plain
// shell never carries the mark that would record one.
TEST(testOnlyALoginSessionAsksForARecord)
{
    CHECK(Rt::LaunchCommand::loginShell().sessionRecord() == P::SessionRecord::None);
    CHECK(Rt::LaunchCommand::fromArguments({ "/bin/echo", "hello" }).sessionRecord() == P::SessionRecord::None);

    // Asked for only where the record is written after the fork; the arm that
    // wraps an argument vector has the login program write it instead.
    const P::SessionRecord expected = P::loginSessionKind() == P::LoginSessionKind::ByRecordingThePTY
        ? P::SessionRecord::AsLogin
        : P::SessionRecord::None;
    CHECK(Rt::LaunchCommand::loginSession().sessionRecord() == expected);
}

UBIQUITTY_TEST_MAIN()
