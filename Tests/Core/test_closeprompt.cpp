// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_closeprompt.cpp — the words a close asks before it kills something.
//
// Close confirmations identify both the affected surface and its live processes.

#include "Core/Session/ClosePrompt.h"

#include <string>
#include <vector>

using Ubiquitty::Session::closeConfirmationBody;
using Ubiquitty::Session::closeConfirmationTitle;
using Ubiquitty::Session::closeConfirmationVerb;
using Ubiquitty::Session::CloseScope;

namespace {
std::string body(CloseScope scope, const std::vector<std::string>& names)
{
    return closeConfirmationBody(scope, std::span<const std::string> { names });
}
} // namespace

TEST(testTheTitleNamesTheSurfaceBeingClosed)
{
    CHECK_STR_EQ(closeConfirmationTitle(CloseScope::Window), "Do you want to terminate running processes in this window?");
    CHECK_STR_EQ(closeConfirmationTitle(CloseScope::Tab), "Do you want to terminate running processes in this tab?");
    CHECK_STR_EQ(closeConfirmationTitle(CloseScope::Pane), "Do you want to terminate running processes in this pane?");
}

TEST(testTheTitleNeverChangesWithTheProcessCount)
{
    // Only the BODY counts. The title stays plural even for a single process —
    // "in this tab?" whether one thing dies or five — because it is the
    // question being asked, not the report. Terminal carries exactly two title
    // strings, one per surface, and no singular form of either.
    const std::string one = closeConfirmationTitle(CloseScope::Tab);
    CHECK_STR_EQ(one, "Do you want to terminate running processes in this tab?");
    CHECK_MSG(one.find("processes") != std::string::npos, "the title is plural regardless of the count");
    CHECK_MSG(one.find("process ") == std::string::npos, "no singular form of the title exists");
}

TEST(testOneProcessReadsAsOneAndSeveralReadAsAList)
{
    // Singular is its own sentence, with no colon and no list punctuation: a
    // prompt is read in a hurry and "the running process psql" lands at a
    // glance where "the running process(es): psql" has to be parsed.
    CHECK_STR_EQ(body(CloseScope::Window, { "psql" }), "Closing this window will terminate the running process psql.");
    CHECK_STR_EQ(body(CloseScope::Tab, { "vi" }), "Closing this tab will terminate the running process vi.");

    // Several: a colon, then commas throughout with no conjunction. A reader is
    // counting what they lose, and an "and" makes the last one look different
    // from the rest.
    CHECK_STR_EQ(body(CloseScope::Window, { "zsh", "mc" }), "Closing this window will terminate the running processes: zsh, mc.");
    CHECK_STR_EQ(body(CloseScope::Pane, { "zsh", "mc", "vi" }), "Closing this pane will terminate the running processes: zsh, mc, vi.");
}

TEST(testNothingRunningProducesNoSentence)
{
    // The decision to ask at all is needsCloseConfirmation's. A body with
    // nothing to name would be a prompt with no reason behind it, so this
    // answers empty rather than inventing a sentence for the caller.
    CHECK(body(CloseScope::Window, {}).empty());
}

TEST(testTheButtonIsNamedForTheConsequence)
{
    // Not "Close": that is what the reader clicked a moment ago. What they are
    // being warned about is what happens to the processes.
    CHECK_STR_EQ(std::string { closeConfirmationVerb() }, "Terminate");
}

TEST(testCloseNamesExcludeLauncherZombiesAndEmptyNames)
{
    using namespace Ubiquitty::PAL::Process;
    std::vector<Record> processes(5);
    processes[0].name = "login";
    processes[0].isSessionLeader = true;
    processes[1].name = "ssh";
    processes[2].name = "expired";
    processes[2].state = State::Zombie;
    processes[4].name = "rg";
    const auto names = Ubiquitty::Session::runningProcessNamesForClose(processes);
    CHECK(names.size() == 2);
    CHECK_STR_EQ(body(CloseScope::Tab, names), "Closing this tab will terminate the running processes: ssh, rg.");
}

TEST(testCloseNamesExcludeLoginShellButKeepNestedShellJobs)
{
    using namespace Ubiquitty::PAL::Process;
    std::vector<Record> processes(4);
    processes[0].processID = 100;
    processes[0].name = "login";
    processes[0].isSessionLeader = true;
    processes[1].processID = 101;
    processes[1].parentProcessID = 100;
    processes[1].name = "-zsh";
    processes[2].processID = 102;
    processes[2].parentProcessID = 101;
    processes[2].name = "ssh";
    auto names = Ubiquitty::Session::runningProcessNamesForClose(processes);
    CHECK_STR_EQ(body(CloseScope::Tab, names), "Closing this tab will terminate the running process ssh.");
    processes[3].processID = 103;
    processes[3].parentProcessID = 101;
    processes[3].name = "bash";
    processes[3].commandLine = "bash deploy.sh";
    names = Ubiquitty::Session::runningProcessNamesForClose(processes);
    CHECK(names.size() == 2);
    CHECK_STR_EQ(names[1], "bash");
    processes[0].isSessionLeader = false;
    processes[1].isSessionLeader = true;
    processes.erase(processes.begin());
    names = Ubiquitty::Session::runningProcessNamesForClose(processes);
    CHECK(names.size() == 2);
    CHECK_STR_EQ(names[0], "ssh");
}

UBIQUITTY_TEST_MAIN()
