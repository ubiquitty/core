// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_workspace_session_start.cpp — a session the app builds starts its model
// under the model lock. The app builds every pane through WorkspaceSession: a
// window from a launch command, a split, a restored layout. Each pane's start
// builds the model and publishes its first snapshot through the pane publish
// hook, which checks, in a build with assertions, that the publishing thread
// holds the lock; these cases build each pane that way and require the
// published snapshot.

#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/LaunchCommand.h"
#include "Core/Runtime/WorkspaceSession.h"

#include <string>

using namespace Ubiquitty::Runtime;

namespace {

// The model borrows its configuration, so the handle holds it for the
// model's life.
struct ModelHandle {
    Ubiquitty::Preferences::Values configuration;
    WorkspaceSession* model = nullptr;
    ModelHandle()
        : model(WorkspaceSession::make(WorkspaceSession::IngestPolicy::ThreadDrained, DrawnText::SizedAndProportional, configuration))
    {
    }
    ~ModelHandle()
    {
        if (model != nullptr)
            releaseWorkspaceSession(model);
    }
};

// A child that keeps its PTY open while the case looks at its pane.
LaunchCommand sleepingChild()
{
    LaunchCommand command = LaunchCommand::fromArguments({ "/bin/sh", "-c", "sleep 30" });
    command.setArgument0Style(Ubiquitty::PAL::Argument0Style::ProgramPath);
    return command;
}

// Whether the pane's session has published the model it started.
bool hasPublishedItsModel(WorkspaceSession& model, Ubiquitty::Session::Layout::PaneID pane)
{
    TerminalSession* session = model.session(pane);
    if (session == nullptr)
        return false;
    const RuntimeSnapshot snapshot = session->readSnapshot();
    return snapshot.rows > 0 && snapshot.columns > 0;
}

} // namespace

TEST(aWindowFromALaunchCommandStartsUnderTheModelLock)
{
    ModelHandle handle;
    REQUIRE(handle.model != nullptr);
    const auto surface = handle.model->addWindowSurface(sleepingChild());
    REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
    CHECK(hasPublishedItsModel(*handle.model, handle.model->focusedPane(surface)));
}

TEST(aSplitPaneStartsUnderTheModelLock)
{
    ModelHandle handle;
    REQUIRE(handle.model != nullptr);
    const auto surface = handle.model->addWindowSurface(sleepingChild());
    REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
    const auto split = handle.model->splitFocusedPane(surface, Ubiquitty::Panes::Orientation::Vertical);
    REQUIRE(split.createdPane != Ubiquitty::Session::Layout::invalidPane);
    CHECK(hasPublishedItsModel(*handle.model, split.createdPane));
}

TEST(aRestoredLayoutStartsItsPanesUnderTheModelLock)
{
    ModelHandle handle;
    REQUIRE(handle.model != nullptr);
    const auto surface = handle.model->addWindowSurfaceRestoring("abcd,80x24,0,0{40x24,0,0,1,39x24,41,0,2}");
    REQUIRE(surface != Ubiquitty::Session::Layout::invalidSurface);
    REQUIRE(handle.model->paneCount(surface) == 2);
    CHECK(hasPublishedItsModel(*handle.model, handle.model->focusedPane(surface)));
}

UBIQUITTY_TEST_MAIN()
