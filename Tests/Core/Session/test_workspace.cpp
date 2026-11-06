// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_workspace.cpp — the serializable Workspace model:
// capture a window/tab/pane-split arrangement (with each pane's Ubiquitty::Transport::Origin) to bytes
// and restore it byte-faithfully, plus bounds-safety over a corrupt buffer.

#include "Core/Session/Workspace.h"

#include <cstdio>
#include <limits>
#include <vector>

using namespace Ubiquitty::Session;

namespace {
void check(bool ok, const char* what)
{
    CHECK_MSG(ok, what);
}

Ubiquitty::Transport::Origin local() { return { Ubiquitty::Transport::OriginKind::Local, "" }; }
Ubiquitty::Transport::Origin ssh(const char* host) { return { Ubiquitty::Transport::OriginKind::SSH, host }; }
Ubiquitty::Transport::Origin daemon(const char* host) { return { Ubiquitty::Transport::OriginKind::Daemon, host }; }

// A representative workspace: two windows, a split tab + a single-pane tab,
// mixed origins.
Workspace sample()
{
    Workspace ws;
    ws.name = "work";

    WorkspaceWindow window0;
    WorkspaceTab tab0;
    tab0.title = "editor";
    // A vertical split: local shell | (ssh-tmux stacked over a daemon pane).
    tab0.root = PaneNode::makeSplit(/*horizontal*/ false, 0.4, PaneNode::makeLeaf(local(), "shell"), PaneNode::makeSplit(/*horizontal*/ true, 0.7, PaneNode::makeLeaf(ssh("devbox"), "vim"), PaneNode::makeLeaf(daemon("box"), "logs")));
    WorkspaceTab tab1;
    tab1.title = "scratch";
    tab1.root = PaneNode::makeLeaf(local(), "sh");
    window0.tabs = { tab0, tab1 };
    window0.activeTab = 1;

    WorkspaceWindow window1;
    WorkspaceTab tab2;
    tab2.title = "remote";
    tab2.root = PaneNode::makeLeaf(daemon("cloud"), "top");
    window1.tabs = { tab2 };

    ws.windows = { window0, window1 };
    return ws;
}

TEST(test_round_trip_is_faithful)
{
    const Workspace original = sample();
    std::vector<std::uint8_t> bytes;
    serializeWorkspace(original, bytes);
    check(!bytes.empty(), "serialize produced bytes");

    Workspace restored;
    check(deserializeWorkspace(bytes, restored), "deserialize succeeds");
    check(equalWorkspaces(original, restored), "restored workspace equals the original");
    // Spot-check a deep field survived (the nested ssh pane title + ratio).
    check(restored.windows.size() == 2, "window count");
    check(restored.windows[0].activeTab == 1, "activeTab survived");
    const PaneNode& root = restored.windows[0].tabs[0].root;
    check(!root.leaf && root.children.size() == 2, "split tree shape survived");
    check(root.children[1].children[0].origin.host == "devbox", "nested ssh host survived");
    check(root.children[1].children[0].title == "vim", "nested pane title survived");
}

TEST(test_empty_round_trip)
{
    Workspace empty;
    empty.name = "";
    std::vector<std::uint8_t> bytes;
    serializeWorkspace(empty, bytes);
    Workspace restored;
    check(deserializeWorkspace(bytes, restored), "empty workspace deserializes");
    check(equalWorkspaces(empty, restored), "empty workspace round-trips");
}

TEST(test_truncation_is_rejected)
{
    std::vector<std::uint8_t> bytes;
    serializeWorkspace(sample(), bytes);
    // Every proper prefix must be rejected (no read past the buffer).
    bool anyAccepted = false;
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        Workspace out;
        if (deserializeWorkspace({ bytes.data(), cut }, out))
            anyAccepted = true;
    }
    check(!anyAccepted, "no truncated prefix is accepted");
}

TEST(test_wrong_version_is_rejected)
{
    std::vector<std::uint8_t> bytes;
    serializeWorkspace(sample(), bytes);
    bytes[0] = 0xFE; // corrupt the version byte
    Workspace out;
    check(!deserializeWorkspace(bytes, out), "wrong version rejected");
}

// A non-finite ratio (NaN) — a corrupt file can carry one — must round-trip
// bit-faithfully and compare equal to itself. `!=` reports NaN unequal to
// itself, so equality compares the ratio bit-wise.
TEST(test_nan_ratio_round_trips_equal)
{
    Workspace ws;
    ws.name = "nan";
    WorkspaceWindow window;
    WorkspaceTab tab;
    tab.root = PaneNode::makeSplit(false, std::numeric_limits<double>::quiet_NaN(), PaneNode::makeLeaf(local(), "a"), PaneNode::makeLeaf(local(), "b"));
    window.tabs = { tab };
    ws.windows = { window };

    std::vector<std::uint8_t> bytes;
    serializeWorkspace(ws, bytes);
    Workspace restored;
    check(deserializeWorkspace(bytes, restored), "NaN-ratio deserializes");
    check(equalWorkspaces(ws, restored), "NaN-ratio workspace equals itself (bit-wise ratio compare)");
}

} // namespace

UBIQUITTY_TEST_MAIN()
