// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_shell_foreground.cpp — Session/ShellForeground.h, the rule that tells a
// shell's own work in the foreground from a job, and the member that names it.
// Each case is the group a real shell builds for the shape it names;
// Tests/Platform/test_pty.cpp drives a real zsh through the same shapes.

#include "Core/Session/ShellForeground.h"

#include <array>

using Ubiquitty::PAL::Process::GroupMember;
using Ubiquitty::Session::foregroundOwner;
using Ubiquitty::Session::isJobLeader;
using Ubiquitty::Session::isJobProcess;
using Ubiquitty::Session::isShellOwnedForegroundGroup;

namespace {

constexpr std::uint32_t shell = 500;
// The group each case's members make, led by the member with this ID.
constexpr std::uint32_t group = 600;

constexpr GroupMember forkOf(std::uint32_t processID, std::uint32_t parent, std::uint64_t startTime = 0)
{
    return { .processID = processID, .parentProcessID = parent, .hasExecuted = false, .isInterruptIgnored = false, .isZombie = false, .startTime = startTime };
}

constexpr GroupMember program(std::uint32_t processID, std::uint32_t parent, std::uint64_t startTime = 0)
{
    return { .processID = processID, .parentProcessID = parent, .hasExecuted = true, .isInterruptIgnored = false, .isZombie = false, .startTime = startTime };
}

constexpr GroupMember backgroundedProgram(std::uint32_t processID, std::uint32_t parent, std::uint64_t startTime = 0)
{
    return { .processID = processID, .parentProcessID = parent, .hasExecuted = true, .isInterruptIgnored = true, .isZombie = false, .startTime = startTime };
}

constexpr GroupMember zombie(std::uint32_t processID, std::uint32_t parent)
{
    return { .processID = processID, .parentProcessID = parent, .hasExecuted = true, .isInterruptIgnored = false, .isZombie = true };
}

} // namespace

TEST(aPromptHookSubshellIsTheShells)
{
    // `( function & )`: the subshell, and the fork it leaves running shell code.
    const std::array members { forkOf(600, shell), forkOf(601, 600) };
    CHECK(isShellOwnedForegroundGroup(members, group, shell));
    // Its fork outlives it and is reparented, still in the group.
    const std::array orphaned { forkOf(601, 1) };
    CHECK(isShellOwnedForegroundGroup(orphaned, group, shell));
}

TEST(aProgramAPromptHookBackgroundsIsTheShells)
{
    // `( program & )`: the subshell started the program with SIGINT ignored,
    // as a shell without job control starts every asynchronous list.
    const std::array members { forkOf(600, shell), backgroundedProgram(601, 600) };
    CHECK(isShellOwnedForegroundGroup(members, group, shell));
    const std::array orphaned { backgroundedProgram(601, 1) };
    CHECK(isShellOwnedForegroundGroup(orphaned, group, shell));
}

TEST(aGroupWhoseJobHasEndedIsTheShells)
{
    CHECK(isShellOwnedForegroundGroup({}, group, shell));
    const std::array members { zombie(600, shell) };
    CHECK(isShellOwnedForegroundGroup(members, group, shell));
}

TEST(aProgramTheShellRanIsAJob)
{
    // `sleep 30`, `vim`, `ssh`, a nested shell: exec'd children of the shell,
    // whatever their signal dispositions.
    const std::array members { program(600, shell) };
    CHECK(!isShellOwnedForegroundGroup(members, group, shell));
    const std::array ignoring { backgroundedProgram(600, shell) };
    CHECK(!isShellOwnedForegroundGroup(ignoring, group, shell));
    CHECK(isJobProcess(program(600, shell), shell));
}

TEST(aPipelineIsAJobWhateverLeadsIt)
{
    // `cat file | less` once cat has exited and not been reaped.
    const std::array afterTheFirst { zombie(600, shell), program(601, shell) };
    CHECK(!isShellOwnedForegroundGroup(afterTheFirst, group, shell));
    // `{ … } | less`: the group's leader is a fork running shell code.
    const std::array ledByAFork { forkOf(600, shell), program(601, shell) };
    CHECK(!isShellOwnedForegroundGroup(ledByAFork, group, shell));
}

TEST(aProgramASubshellWaitsForIsAJob)
{
    // `( sleep 30; true )`: Ctrl-C reaches the program, so it is the user's.
    const std::array members { forkOf(600, shell), program(601, 600) };
    CHECK(!isShellOwnedForegroundGroup(members, group, shell));
}

TEST(aZombieIsNeverAJob)
{
    CHECK(!isJobProcess(zombie(601, 600), shell));
    CHECK(!isJobProcess(forkOf(601, shell), shell));
}

TEST(aJobIsNamedByTheMemberThatMakesItOne)
{
    // `( sleep 30; true )`: the program, not the fork of the shell leading it.
    const std::array subshell { forkOf(600, shell), program(601, 600) };
    CHECK(foregroundOwner(subshell, group, true, shell).isJob);
    CHECK_EQ(foregroundOwner(subshell, group, true, shell).namingProcessID, 601U);
    // `cat file | less` once cat has exited and not been reaped.
    const std::array afterTheFirst { zombie(600, shell), program(601, shell) };
    CHECK_EQ(foregroundOwner(afterTheFirst, group, true, shell).namingProcessID, 601U);
    // A hook's subshell and the program it backgrounds name nothing.
    const std::array hook { forkOf(600, shell), backgroundedProgram(601, 600) };
    CHECK(!foregroundOwner(hook, group, true, shell).isJob);
    CHECK_EQ(foregroundOwner(hook, group, true, shell).namingProcessID, 0U);
}

TEST(aGroupReadInPartIsAJobNamedByAProgramInIt)
{
    // The members read of a larger group: a job among them names it.
    const std::array withAJob { forkOf(600, shell), backgroundedProgram(601, 600), program(602, 600) };
    CHECK(foregroundOwner(withAJob, group, false, shell).isJob);
    CHECK_EQ(foregroundOwner(withAJob, group, false, shell).namingProcessID, 602U);
    // None of them is a job, and the group is one all the same: the first
    // that runs a program names it.
    const std::array withoutAJob { forkOf(600, shell), zombie(601, 600), backgroundedProgram(602, 600) };
    CHECK(foregroundOwner(withoutAJob, group, false, shell).isJob);
    CHECK_EQ(foregroundOwner(withoutAJob, group, false, shell).namingProcessID, 602U);
    // Shell code alone has no program to name the job by.
    const std::array shellCode { forkOf(600, shell), forkOf(601, 600) };
    CHECK(foregroundOwner(shellCode, group, false, shell).isJob);
    CHECK_EQ(foregroundOwner(shellCode, group, false, shell).namingProcessID, 0U);
}

TEST(aNestedShellsSubshellIsAJobNamedByItsLeader)
{
    // A nested shell the session's shell ran, and the group its subshell or
    // prompt hook leads with a fork of it: that shell's code, not the
    // session shell's.
    constexpr std::uint32_t nestedShell = 550;
    CHECK(isJobLeader(forkOf(600, nestedShell), shell));
    CHECK(!isJobLeader(forkOf(600, shell), shell));
    CHECK(!isJobLeader(zombie(600, nestedShell), shell));
    const std::array subshell { forkOf(600, nestedShell) };
    CHECK(foregroundOwner(subshell, group, true, shell).isJob);
    CHECK_EQ(foregroundOwner(subshell, group, true, shell).namingProcessID, 600U);
    // Its hook backgrounding a program, which the session shell's own hook
    // does without making a job.
    const std::array hook { forkOf(600, nestedShell), backgroundedProgram(601, 600) };
    CHECK(!isShellOwnedForegroundGroup(hook, group, shell));
    CHECK_EQ(foregroundOwner(hook, group, true, shell).namingProcessID, 600U);
}

TEST(aJobIsNamedByItsOldestProgramWhateverOrderItIsListedIn)
{
    // `( while :; do sleep 1; done ) | cat`: the fork leading the group, cat
    // beside it, and the sleep of the moment. Darwin lists the group newest
    // first, Linux by process ID; both name cat, which outlives every sleep.
    const std::array byProcessID { forkOf(600, shell, 10), program(601, shell, 11), program(605, 600, 50) };
    const std::array newestFirst { program(605, 600, 50), program(601, shell, 11), forkOf(600, shell, 10) };
    CHECK_EQ(foregroundOwner(byProcessID, group, true, shell).namingProcessID, 601U);
    CHECK_EQ(foregroundOwner(newestFirst, group, true, shell).namingProcessID, 601U);
    // Members started in one tick are told apart by their process IDs.
    const std::array sameTick { program(603, shell, 20), program(602, shell, 20) };
    CHECK_EQ(foregroundOwner(sameTick, group, true, shell).namingProcessID, 602U);
    // A group read in part with no job among it is named by its oldest program.
    const std::array readInPart { backgroundedProgram(604, 600, 40), backgroundedProgram(603, 600, 30) };
    CHECK_EQ(foregroundOwner(readInPart, group, false, shell).namingProcessID, 603U);
}

UBIQUITTY_TEST_MAIN()
