// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_launch_context.cpp — a program started in a pane is told where it runs
// (Core/Session/LaunchContext.h): the socket, and the window, tab, and pane,
// each a typed identity. A context the parent inherited from an enclosing pane
// never reaches it, and a value of the wrong kind is not read as one.

#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/LaunchCommand.h"
#include "Core/Runtime/WorkspaceSession.h"
#include "Core/Session/LaunchContext.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>

using namespace Ubiquitty::Runtime;
using Ubiquitty::Session::contextEnvironment;
using Ubiquitty::Session::contextFromEnvironment;
using Ubiquitty::Session::isContextEnvironmentEntry;
using Ubiquitty::Session::LaunchContext;
using Ubiquitty::Session::Identity::ObjectID;
using Ubiquitty::Session::Identity::ObjectKind;

namespace {

/// The environment a `contextFromEnvironment` case reads, in place of the
/// process's own.
std::map<std::string, std::string>& fakeEnvironment()
{
    static std::map<std::string, std::string> table;
    return table;
}

const char* lookupFake(const char* name)
{
    const auto found = fakeEnvironment().find(name);
    return found == fakeEnvironment().end() ? nullptr : found->second.c_str();
}

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

/// A child that writes the context it was given to `path` and exits.
LaunchCommand reportingChild(const std::string& path)
{
    const std::string script = "for name in UBIQUITTY_SOCKET UBIQUITTY_SESSION UBIQUITTY_WINDOW UBIQUITTY_TAB UBIQUITTY_PANE; do "
                               "printf '%s=%s\\n' \"$name\" \"$(printenv $name)\"; done > '"
        + path + ".partial' && mv '" + path + ".partial' '" + path + "'";
    LaunchCommand command = LaunchCommand::fromArguments({ "/bin/sh", "-c", script });
    command.setArgument0Style(Ubiquitty::PAL::Argument0Style::ProgramPath);
    return command;
}

/// The child's report, once it has written it whole.
std::map<std::string, std::string> waitForReport(const std::string& path)
{
    std::map<std::string, std::string> report;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream in(path);
        if (in) {
            for (std::string line; std::getline(in, line);) {
                const auto separator = line.find('=');
                if (separator != std::string::npos)
                    report[line.substr(0, separator)] = line.substr(separator + 1);
            }
            return report;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return report;
}

std::string scratchPath(const char* name)
{
    return "/tmp/ubiquitty-launch-context-" + std::to_string(::getpid()) + "-" + name;
}

} // namespace

TEST(aContextIsWrittenAndReadAsTypedIdentities)
{
    LaunchContext context;
    context.socketPath = "/tmp/control.sock";
    context.window = ObjectID { ObjectKind::Window, 2 };
    context.tab = ObjectID { ObjectKind::Tab, 3 };
    context.pane = ObjectID { ObjectKind::Pane, 4 };
    const auto entries = contextEnvironment(context);
    CHECK_EQ(entries.size(), 4u);
    for (const std::string& entry : entries) {
        CHECK_MSG(isContextEnvironmentEntry(entry), entry.c_str());
        const auto separator = entry.find('=');
        fakeEnvironment()[entry.substr(0, separator)] = entry.substr(separator + 1);
    }
    CHECK(fakeEnvironment()["UBIQUITTY_PANE"] == "pane:4");

    const LaunchContext read = contextFromEnvironment(lookupFake);
    CHECK(read.socketPath == "/tmp/control.sock");
    CHECK(!read.session);
    CHECK(read.pane && *read.pane == (ObjectID { ObjectKind::Pane, 4 }));
    CHECK_EQ(read.identities().size(), 3u);

    // A value naming the wrong kind, or nothing, is not a location.
    fakeEnvironment()["UBIQUITTY_PANE"] = "window:4";
    fakeEnvironment()["UBIQUITTY_TAB"] = "3";
    CHECK(!contextFromEnvironment(lookupFake).pane);
    CHECK(!contextFromEnvironment(lookupFake).tab);
    fakeEnvironment().clear();

    CHECK(!isContextEnvironmentEntry("UBIQUITTY_PANES=pane:1"));
    CHECK(!isContextEnvironmentEntry("UBIQUITTY_PANE"));
}

// The negative half: a stale context in the parent's environment — the app or
// host was itself started from inside a pane — must not reach a child launched
// without one, or that child would act on a pane it is not in.
TEST(anInheritedContextNeverReachesAChild)
{
    ::setenv("UBIQUITTY_SESSION", "session:77", 1);
    ::setenv("UBIQUITTY_PANE", "pane:999", 1);
    const std::string path = scratchPath("inherited");
    ::unlink(path.c_str());
    {
        Ubiquitty::Preferences::Values configuration;
        ModelHandle handle(configuration);
        REQUIRE(handle.model != nullptr);
        const auto surface = handle.model->addWindowSurface(reportingChild(path));
        REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
        const auto pane = handle.model->focusedPane(surface);
        auto report = waitForReport(path);
        CHECK_STR_EQ(report["UBIQUITTY_SESSION"], "");
        CHECK_STR_EQ(report["UBIQUITTY_PANE"], "pane:" + std::to_string(std::to_underlying(pane)));
    }
    ::unsetenv("UBIQUITTY_SESSION");
    ::unsetenv("UBIQUITTY_PANE");
    ::unlink(path.c_str());
}

TEST(aLocalPanesChildSeesTheSocketAndThePaneItRunsIn)
{
    const std::string rootPath = scratchPath("root");
    ::unlink(rootPath.c_str());
    {
        Ubiquitty::Preferences::Values configuration;
        ModelHandle handle(configuration);
        REQUIRE(handle.model != nullptr);
        WorkspaceSession& model = *handle.model;
        model.setContextSocketPath("/tmp/ubiquitty-control.sock");

        const auto surface = model.addWindowSurface(reportingChild(rootPath));
        REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
        const auto rootPane = model.focusedPane(surface);
        auto report = waitForReport(rootPath);
        CHECK_STR_EQ(report["UBIQUITTY_SOCKET"], "/tmp/ubiquitty-control.sock");
        // The control socket names windows by their window numbers and tabs
        // not at all, so the workspace's own window and tab IDs are not given
        // to a child that could resolve neither.
        CHECK_STR_EQ(report["UBIQUITTY_WINDOW"], "");
        CHECK_STR_EQ(report["UBIQUITTY_TAB"], "");
        CHECK_STR_EQ(report["UBIQUITTY_PANE"], "pane:" + std::to_string(std::to_underlying(rootPane)));
        CHECK_STR_EQ(report["UBIQUITTY_SESSION"], "");

        const auto context = model.launchContext(rootPane);
        CHECK(context.pane && context.pane->number == std::to_underlying(rootPane));
    }
    ::unlink(rootPath.c_str());
}

UBIQUITTY_TEST_MAIN()
