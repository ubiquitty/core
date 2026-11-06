// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_session_exit_lifetime.cpp — whether a session ends when its child does is
// a fact of the launch, carried per session (Core/Runtime/LaunchCommand.h,
// Core/Runtime/TerminalRuntime.cpp). One process holds a -e window and a login
// shell at once, and each answers for itself; a process-wide flag, which is what
// the macOS arm read, cannot.

#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/LaunchArguments.h"
#include "Core/Runtime/LaunchCommand.h"
#include "Core/Runtime/WorkspaceSession.h"
#include "Core/Session/ProcessExitPolicy.h"
#include "Core/Terminal.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace Ubiquitty::Runtime;

TEST(cleanExitPolicyWaitsForTheReapedStatus)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::Session;
    ProcessExitPolicy policy;
    policy.didLaunch(Ticks(100));
    Title::SessionEnd ended;
    CHECK(policy.action(ProcessExitBehavior::CloseOnSuccess, ended, Ticks(200)) == ProcessExitAction::KeepOutput);
    ended.didEnd = true;
    CHECK(policy.action(ProcessExitBehavior::CloseOnSuccess, ended, Ticks(200)) == ProcessExitAction::AwaitStatus);
    ended.hasStatus = true;
    CHECK(policy.action(ProcessExitBehavior::CloseOnSuccess, ended, Ticks(201)) == ProcessExitAction::Close);
    CHECK(policy.action(ProcessExitBehavior::CloseOnSuccess, ended, Ticks(202)) == ProcessExitAction::KeepOutput);

    policy.didLaunch(Ticks(300));
    ended.code = 1;
    CHECK(policy.action(ProcessExitBehavior::CloseOnSuccess, ended, Ticks(400)) == ProcessExitAction::KeepOutput);
    policy.didLaunch(Ticks(500));
    ended.code = 0;
    ended.wasSignalled = true;
    CHECK(policy.action(ProcessExitBehavior::CloseOnSuccess, ended, Ticks(600)) == ProcessExitAction::KeepOutput);
}

TEST(restartBudgetBelongsToTheSessionAndSurvivesRepeatedExitDelivery)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::Session;
    ProcessExitPolicy crashing;
    ProcessExitPolicy independent;
    Title::SessionEnd ended;
    ended.didEnd = true;
    ended.hasStatus = true;
    ended.code = 1;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        crashing.didLaunch(Ticks(attempt * 100));
        CHECK(crashing.action(ProcessExitBehavior::Restart, ended, Ticks(attempt * 100 + 1))
            == (attempt < 3 ? ProcessExitAction::Restart : ProcessExitAction::RestartLimitReached));
        CHECK(crashing.action(ProcessExitBehavior::Restart, ended, Ticks(attempt * 100 + 2)) == ProcessExitAction::KeepOutput);
    }
    independent.didLaunch(Ticks(400));
    CHECK(independent.action(ProcessExitBehavior::Restart, ended, Ticks(401)) == ProcessExitAction::Restart);

    crashing.didLaunch(Ticks(0xfffffff0));
    CHECK(crashing.action(ProcessExitBehavior::Restart, ended, Ticks(0xfffffff0) + Ticks::fromMilliseconds(60000).count)
        == ProcessExitAction::Restart);
}

namespace {

struct ModelHandle {
    WorkspaceSession* model = nullptr;
    explicit ModelHandle(const Ubiquitty::Preferences::Values& configuration)
        : model(WorkspaceSession::make(WorkspaceSession::IngestPolicy::ThreadDrained, DrawnText::SizedAndProportional, configuration))
    {
    }
    ~ModelHandle()
    {
        if (model != nullptr)
            releaseWorkspaceSession(model);
    }
};

// A child that holds its PTY open long enough to be asked, with the lifetime a
// parsed command line implies.
LaunchCommand sleepingChild(const std::vector<std::string>& commandLine)
{
    std::vector<const char*> argv;
    argv.reserve(commandLine.size());
    for (const std::string& token : commandLine)
        argv.push_back(token.c_str());
    const LaunchRequest request = parseLaunchArguments(static_cast<int>(argv.size()), argv.data());
    LaunchCommand command = LaunchCommand::fromArguments({ "/bin/sh", "-c", "sleep 30" });
    command.setArgument0Style(Ubiquitty::PAL::Argument0Style::ProgramPath);
    command.setSessionLifetime(launchEndsWithChild(request) ? SessionLifetime::EndsWithChild : SessionLifetime::OutlivesChild);
    return command;
}

} // namespace

TEST(threeSessionsInOneModelHoldDivergentExitLifetimes)
{
    Ubiquitty::Preferences::Values configuration;
    ModelHandle handle(configuration);
    REQUIRE(handle.model != nullptr);
    WorkspaceSession& model = *handle.model;

    const auto runsProgram = model.addWindowSurface(sleepingChild({ "ubiquitty", "-e", "x" }));
    const auto holdsProgram = model.addWindowSurface(sleepingChild({ "ubiquitty", "--hold", "-e", "x" }));
    const auto loginShell = model.addWindowSurface(sleepingChild({ "ubiquitty" }));
    REQUIRE(runsProgram != Ubiquitty::Session::Layout::invalidSurface);
    REQUIRE(holdsProgram != Ubiquitty::Session::Layout::invalidSurface);
    REQUIRE(loginShell != Ubiquitty::Session::Layout::invalidSurface);

    TerminalSession* runs = model.session(model.focusedPane(runsProgram));
    TerminalSession* holds = model.session(model.focusedPane(holdsProgram));
    TerminalSession* login = model.session(model.focusedPane(loginShell));
    REQUIRE(runs != nullptr);
    REQUIRE(holds != nullptr);
    REQUIRE(login != nullptr);

    // All three read while all three are alive: the point is that they hold
    // different answers at once, in one process.
    CHECK_MSG(runs->doesSessionEndWithChild(), "-e ends with its program");
    CHECK_MSG(!holds->doesSessionEndWithChild(), "--hold keeps the window");
    CHECK_MSG(!login->doesSessionEndWithChild(), "a login shell outlives its child");
}

// The route the Swift shell takes: a draft carries the lifetime through build,
// and an empty draft, which falls back to the login shell, answers OutlivesChild
// however the caller set it.
TEST(aDraftedLaunchCarriesTheLifetimeAndAnEmptyDraftDoesNot)
{
    LaunchCommandDraft named;
    named.addArgument("/bin/sh");
    named.setSessionLifetime(SessionLifetime::EndsWithChild);
    named.setTmuxControlHost("user@host");
    CHECK(named.build().sessionLifetime() == SessionLifetime::EndsWithChild);
    CHECK(named.build().argument0Style() == Ubiquitty::PAL::Argument0Style::ProgramPath);
    CHECK(named.build().tmuxControlHost() == std::optional<std::string>("user@host"));

    LaunchCommandDraft empty;
    empty.setSessionLifetime(SessionLifetime::EndsWithChild);
    empty.setTmuxControlHost("user@host");
    CHECK_MSG(empty.build().sessionLifetime() == SessionLifetime::OutlivesChild, "a login shell nobody named outlives its child whatever the caller asked");
    CHECK(empty.build().argument0Style() == Ubiquitty::PAL::Argument0Style::LoginShell);
    CHECK(!empty.build().tmuxControlHost());

    LaunchCommandDraft unset;
    unset.addArgument("/bin/sh");
    CHECK_MSG(unset.build().sessionLifetime() == SessionLifetime::OutlivesChild, "a command with no lifetime named outlives its child");
}

UBIQUITTY_TEST_MAIN()

TEST(workspaceRestartsTheOriginalCommandWithoutReplacingItsPane)
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::Session;
    Preferences::Values configuration;
    ModelHandle handle(configuration);
    REQUIRE(handle.model);
    auto command = LaunchCommand::fromArguments({ "/bin/sh", "-c", "exit 7" });
    command.setArgument0Style(PAL::Argument0Style::ProgramPath);
    const auto surface = handle.model->addWindowSurface(command);
    REQUIRE(surface != Layout::invalidSurface);
    const auto pane = handle.model->focusedPane(surface);
    CHECK(!handle.model->restartEndedPane(pane));
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        auto* session = handle.model->session(pane);
        REQUIRE(session);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!session->sessionEnd().didEnd && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(session->sessionEnd().didEnd);
        CHECK(handle.model->processExitAction(pane, ProcessExitBehavior::Restart)
            == (attempt < 3 ? ProcessExitAction::Restart : ProcessExitAction::RestartLimitReached));
        if (attempt < 3) {
            CHECK(handle.model->restartEndedPane(pane));
            CHECK(handle.model->focusedPane(surface) == pane);
            CHECK(handle.model->paneCount(surface) == 1);
        } else {
            CHECK(!handle.model->restartEndedPane(pane));
        }
    }
}
