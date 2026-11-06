// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// ShowcaseSessionTree.h — one host with something of everything a listing
// draws, for a frontend's scenarios and pictures.
//
// Every frontend that draws a host's outline is checked against the same
// host, so a row that reads well in one window and badly in another is the
// drawing's doing and not the data's. The host is built the way a real one
// is: layouts, a label book, a tag book, a record of a workspace nothing runs
// in, and each pane's facts as the two records a host sends, through
// `Session::buildSessionTree`. Nothing here states a roll-up, a title or a
// status; the tree derives them.
//
// What it holds:
//
//   api        labelled "backend", tagged production and eu
//     services   a window of three tabs
//       server     a split: a build at 62 percent, and a log with a bell
//       tests      one pane, labelled; its last command failed; a tab colour
//                  and a badge
//       agent      one pane blocked on a permission, with a notification
//                  that has a title and a body, an indicator colour, a
//                  person and an agent attached
//     docs       a window of one tab of one pane, tagged
//   deploy     tagged staging; one window of two tabs, two people attached
//     rollout    three panes: done, progress held, progress of unknown length
//     migrate    one pane whose progress reports an error
//   scratch    one pane at its prompt; ends when detached
//   notes      one pane; restored after the host restarted; a session label
//   release    nothing runs in it: two recorded windows, a label and a tag
//
// Every name, path and program is invented.
//

#include "Core/App/StatusRollup.h"
#include "Core/Color.h"
#include "Core/OSC/Activity.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/Panes/PaneTreeNode.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/StatedPane.h"
#include "Core/Session/Workspace.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace UbiquittyTest {

/// The showcase host: what it holds, and the tree it states of itself.
struct ShowcaseHost {
    static constexpr std::uint32_t apiWorkspaceID = 3;
    static constexpr std::uint32_t releaseWorkspaceID = 5;
    static constexpr std::uint32_t deployWorkspaceID = 7;
    static constexpr std::uint32_t scratchWorkspaceID = 9;
    static constexpr std::uint32_t notesWorkspaceID = 11;

    /// A pane as the host states it, and how long before the host's clock
    /// it last wrote and was last typed into; zero for never.
    struct Pane {
        StatedPane stated;
        std::uint64_t secondsSinceOutput = 0;
        std::uint64_t secondsSinceInput = 0;
    };

    std::shared_ptr<Ubiquitty::Session::Identity::Allocator> identities = std::make_shared<Ubiquitty::Session::Identity::Allocator>();
    std::unique_ptr<Ubiquitty::Session::Layout::Workspace> api = std::make_unique<Ubiquitty::Session::Layout::Workspace>(identities);
    std::unique_ptr<Ubiquitty::Session::Layout::Workspace> deploy = std::make_unique<Ubiquitty::Session::Layout::Workspace>(identities);
    std::unique_ptr<Ubiquitty::Session::Layout::Workspace> scratch = std::make_unique<Ubiquitty::Session::Layout::Workspace>(identities);
    std::unique_ptr<Ubiquitty::Session::Layout::Workspace> notes = std::make_unique<Ubiquitty::Session::Layout::Workspace>(identities);
    Ubiquitty::Session::Identity::LabelBook labels;
    Ubiquitty::Session::Identity::TagBook tags;
    Ubiquitty::Session::HostLayoutRecord release;
    std::map<std::uint32_t, Pane> panes;
    std::uint32_t nextSessionID = 40;

    /// Lays out the workspaces the file's header lists and states each
    /// pane.
    ShowcaseHost()
    {
        namespace Layout = Ubiquitty::Session::Layout;
        namespace OSC = Ubiquitty::OSC;
        using Ubiquitty::ColorRGBA;
        using Ubiquitty::Panes::Orientation;
        using Ubiquitty::Session::TreeAttendeeKind;
        using Ubiquitty::Session::Identity::ObjectID;
        using Ubiquitty::Session::Identity::ObjectKind;
        const auto tag = [this](ObjectID id, std::vector<std::string> names) { (void)tags.set(id, names); };

        // api: a window of three tabs, and a window of one.
        const Layout::SurfaceID services = api->surfaceIDAt(0);
        const Layout::TabID server = api->tabAt(services, 0);
        const Layout::PaneID build = api->focusedPane(services);
        const Layout::PaneID log = api->splitFocusedPane(services, Orientation::Vertical);
        const Layout::TabID tests = api->addTab(services);
        const Layout::PaneID testRun = api->focusedPane(services);
        const Layout::TabID agent = api->addTab(services);
        const Layout::PaneID assistant = api->focusedPane(services);
        const Layout::SurfaceID docs = api->addSurface();
        const Layout::PaneID editor = api->focusedPane(docs);
        (void)api->renameTab(services, server, "server");
        (void)api->renameTab(services, agent, "agent");
        (void)api->setActiveTab(services, server);
        (void)api->setActiveSurface(services);
        (void)tests;
        (void)labels.set({ ObjectKind::Workspace, apiWorkspaceID }, "backend");
        (void)labels.set({ ObjectKind::Window, std::to_underlying(services) }, "services");
        (void)labels.set({ ObjectKind::Window, std::to_underlying(docs) }, "docs");
        (void)labels.set({ ObjectKind::Pane, std::to_underlying(testRun) }, "tests");
        tag({ ObjectKind::Workspace, apiWorkspaceID }, { "production", "eu" });
        tag({ ObjectKind::Window, std::to_underlying(docs) }, { "writing" });

        Pane& building = run(build, "cargo run --release", "cargo", "/srv/api");
        building.stated.title.hostName = "build-1";
        building.stated.status.programState = OSC::ProgramState::Working;
        building.stated.status.progress = { OSC::ProgressNormal, 62 };
        building.stated.attendees = { { 1, TreeAttendeeKind::Person } };
        building.secondsSinceOutput = 3;
        building.secondsSinceInput = 40;

        Pane& logging = run(log, "tail -f api.log", "tail", "/var/log/api");
        logging.stated.title.hostName = "build-1";
        logging.stated.status.hasUnseenBell = true;
        logging.secondsSinceOutput = 1;

        Pane& testing = run(testRun, "cargo test", "zsh", "/srv/api");
        testing.stated.status.isAtPrompt = true;
        testing.stated.status.exitStatus = 101;
        testing.stated.status.tabColor = ColorRGBA { 204, 68, 68, 255 };
        testing.stated.badgeText = "CI";
        testing.secondsSinceOutput = 4 * 60;
        testing.secondsSinceInput = 5 * 60;

        Pane& assisting = run(assistant, "Refactor the session listing", "assistant", "/srv/api");
        assisting.stated.status.programState = OSC::ProgramState::Blocked;
        assisting.stated.status.blockKind = OSC::ProgramBlockKind::Permission;
        assisting.stated.status.hasUnseenNotification = true;
        assisting.stated.notificationTitle = "Approve the migration?";
        assisting.stated.notificationBody = "The assistant wants to run the schema change against staging.";
        assisting.stated.status.indicatorColor = ColorRGBA { 150, 90, 220, 255 };
        assisting.stated.attendees = { { 1, TreeAttendeeKind::Person }, { 7, TreeAttendeeKind::Agent } };
        assisting.secondsSinceOutput = 12;
        assisting.secondsSinceInput = 3 * 60;

        Pane& editing = run(editor, "nvim README.md", "nvim", "/home/user/api/docs");
        editing.secondsSinceOutput = 2 * 3600;
        editing.secondsSinceInput = 2 * 3600;

        // deploy: one window of two tabs, the first split in three.
        const Layout::SurfaceID pipeline = deploy->surfaceIDAt(0);
        const Layout::TabID rollout = deploy->tabAt(pipeline, 0);
        const Layout::PaneID playbook = deploy->focusedPane(pipeline);
        const Layout::PaneID sync = deploy->splitFocusedPane(pipeline, Orientation::Vertical);
        const Layout::PaneID image = deploy->splitFocusedPane(pipeline, Orientation::Horizontal);
        const Layout::TabID migrate = deploy->addTab(pipeline);
        const Layout::PaneID migration = deploy->focusedPane(pipeline);
        (void)deploy->renameTab(pipeline, rollout, "rollout");
        (void)deploy->renameTab(pipeline, migrate, "migrate");
        (void)deploy->setActiveTab(pipeline, rollout);
        tag({ ObjectKind::Workspace, deployWorkspaceID }, { "staging" });

        Pane& rolledOut = run(playbook, "ansible-playbook site.yml", "ansible-playbook", "/srv/deploy");
        rolledOut.stated.status.programState = OSC::ProgramState::Done;
        rolledOut.stated.attendees = { { 1, TreeAttendeeKind::Person }, { 2, TreeAttendeeKind::Person } };
        rolledOut.secondsSinceOutput = 9 * 60;
        rolledOut.secondsSinceInput = 25 * 60;

        Pane& syncing = run(sync, "rsync -a assets/ cdn:/assets", "rsync", "/srv/deploy/assets");
        syncing.stated.status.progress = { OSC::ProgressPaused, 40 };
        syncing.secondsSinceOutput = 90;

        Pane& imaging = run(image, "docker build .", "docker", "/srv/deploy");
        imaging.stated.status.progress = { OSC::ProgressIndeterminate, 0 };
        imaging.secondsSinceOutput = 2;

        Pane& migrating = run(migration, "psql -f migrate.sql", "psql", "/srv/deploy/sql");
        migrating.stated.status.progress = { OSC::ProgressError, 0 };
        migrating.secondsSinceOutput = 6 * 60;
        migrating.secondsSinceInput = 7 * 60;

        // scratch and notes: one pane each, folded into one row.
        Pane& shell = run(scratch->focusedPane(scratch->surfaceIDAt(0)), "zsh", "zsh", "/home/user");
        shell.stated.status.isAtPrompt = true;
        shell.secondsSinceOutput = 26 * 3600;
        shell.secondsSinceInput = 26 * 3600;

        Pane& journal = run(notes->focusedPane(notes->surfaceIDAt(0)), "vim todo.md", "vim", "/home/user/notes");
        journal.secondsSinceOutput = 3 * 86'400;
        journal.secondsSinceInput = 3 * 86'400;
        (void)labels.set({ ObjectKind::Session, journal.stated.sessionID }, "journal");

        // release: recorded, with nothing running in it.
        using Ubiquitty::Session::PaneNode;
        const auto leaf = [](std::string directory, std::string title) {
            PaneNode node = PaneNode::makeLeaf({}, std::move(title));
            node.directory = std::move(directory);
            return node;
        };
        release.arrangement.name = "release";
        release.workspaceLabel = "r1";
        release.workspaceTags = { "production" };
        Ubiquitty::Session::WorkspaceWindow first;
        Ubiquitty::Session::WorkspaceTab split;
        split.root = PaneNode::makeSplit(false, 0.5, leaf("/var/log/release", "tail -f release.log"), leaf("/srv/release", "vim CHANGELOG.md"));
        first.tabs.push_back(split);
        release.arrangement.windows.push_back(first);
        Ubiquitty::Session::WorkspaceWindow second;
        Ubiquitty::Session::WorkspaceTab single;
        single.root = leaf("/home/user", "zsh");
        second.tabs.push_back(single);
        release.arrangement.windows.push_back(second);
        release.windowLabels = { "main", "" };
        release.tabLabels = { "tailing", "" };
        release.panes = { { 120, 36, "", "", {}, {} }, { 120, 36, "", "", {}, {} }, { 80, 24, "", "", {}, {} } };
    }

    /// States a pane a session draws into.
    ///
    /// - Parameters:
    ///   - id: The layout's pane.
    ///   - title: The title its program set.
    ///   - program: The program holding its terminal.
    ///   - directory: Where that program runs.
    /// - Returns: The pane, to say more about.
    Pane& run(Ubiquitty::Session::Layout::PaneID id, std::string title, std::string program, std::string directory)
    {
        Pane& pane = panes[std::to_underlying(id)];
        pane.stated.sessionID = nextSessionID++;
        pane.stated.paneID = { .low = pane.stated.sessionID, .high = 77 };
        pane.stated.structuralGeneration = 1;
        pane.stated.title.programTitle = std::move(title);
        pane.stated.title.currentDirectory = std::move(directory);
        pane.stated.title.columns = 120;
        pane.stated.title.rows = 36;
        pane.stated.program = std::move(program);
        return pane;
    }

    /// The tree the host states of itself.
    ///
    /// - Parameters:
    ///   - nowUnixMilliseconds: The host's clock; each pane's activity is
    ///     that long before it.
    ///   - revision: The host's count of the state described.
    /// - Returns: The tree.
    [[nodiscard]] Ubiquitty::Session::SessionTree tree(std::uint64_t nowUnixMilliseconds, std::uint64_t revision = 1) const
    {
        using Ubiquitty::Session::TreeRestorableSource;
        using Ubiquitty::Session::TreeWorkspaceSource;
        const std::array sources {
            TreeWorkspaceSource { .id = apiWorkspaceID, .name = "api", .layout = api.get() },
            TreeWorkspaceSource { .id = deployWorkspaceID, .name = "deploy", .layout = deploy.get() },
            TreeWorkspaceSource { .id = scratchWorkspaceID, .name = "scratch", .layout = scratch.get(), .endsWhenDetached = true },
            TreeWorkspaceSource { .id = notesWorkspaceID, .name = "notes", .layout = notes.get(), .wasRestoredAfterHostRestart = true },
        };
        const std::array restorable { TreeRestorableSource { releaseWorkspaceID, &release } };
        const auto before = [nowUnixMilliseconds](std::uint64_t seconds) { return seconds == 0 ? std::uint64_t { 0 } : nowUnixMilliseconds - seconds * 1000; };
        return Ubiquitty::Session::buildSessionTree(
            { .revision = revision, .nowUnixMilliseconds = nowUnixMilliseconds, .workspaces = sources, .tags = &tags, .restorable = restorable }, labels,
            [&](std::uint32_t, Ubiquitty::Session::Layout::PaneID id) {
                const auto found = panes.find(std::to_underlying(id));
                if (found == panes.end())
                    return Ubiquitty::Session::TreePaneFacts {};
                StatedPane stated = found->second.stated;
                stated.activity = { before(found->second.secondsSinceOutput), before(found->second.secondsSinceInput) };
                return stated.treeFacts();
            });
    }
};

} // namespace UbiquittyTest
