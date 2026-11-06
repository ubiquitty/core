// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// fuzz_session_tree.cpp — libFuzzer harness for the decoder of a workspace's
// structure (Source/Core/Session/SessionTreeWire.{h,cpp}).
//
// A client reads a structure as it arrives from a host, so every byte is the
// peer's choice. The input either is a body, or flips bytes of a valid body
// built here, so a fixed corpus reaches past the decoder's first length check
// into its splits, its indexes and its leaves. The first byte picks:
//
//   even  the input as a structure's body
//   odd   a valid body with the input's (offset, mask) pairs applied
//
// Beyond memory safety, a workspace the decoder accepts is one a reader can
// walk without a check of its own: every window has a tab and every tab a
// pane, every index and zoom names something, a tab's split has as many
// leaves as the tab has panes, and the whole names no more panes than a host
// runs. It assembles into a tree with no record held, it encodes, and the
// body it encodes decodes to the same bytes again.
//
// A structure's names are bytes, and so are the records a client holds for
// its panes. Whatever they are, the outline of the tree they assemble into is
// text: its lines are well-formed UTF-8 and its JSON is a document that
// parses. The tree is assembled twice for that, with no record and with
// records whose every text is the input's own bytes.
//
// Build via Scripts/run-tests.sh --with-fuzz; ctest drives it over a fixed
// corpus through Tests/FuzzDriver.cpp.

#include "Core/App/SessionOutline.h"
#include "Core/App/SessionOutlineText.h"
#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/Topology.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/Unicode/UTF8Repair.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::Session;
using Ubiquitty::Protocol::Sync::TopologyNode;

namespace {

std::size_t leavesOf(const TopologyNode& node)
{
    if (node.isLeaf())
        return 1;
    std::size_t leaves = 0;
    for (const TopologyNode& child : node.children)
        leaves += leavesOf(child);
    return leaves;
}

/// A workspace with a split tab, a second window, a zoom, labels and tags:
/// every part of the body a flipped byte can land in.
std::vector<std::uint8_t> seedBody()
{
    TreeWorkspace workspace;
    workspace.id = 3;
    workspace.revision = 7;
    workspace.name = "work";
    workspace.label = "w";
    workspace.tags = { "prod" };
    for (int windowIndex = 0; windowIndex < 2; ++windowIndex) {
        TreeWindow window;
        window.id = Layout::SurfaceID { static_cast<std::uint32_t>(windowIndex + 1) };
        window.label = "ops";
        TreeTab tab;
        tab.id = Layout::TabID { static_cast<std::uint32_t>(windowIndex + 10) };
        tab.name = "build";
        for (std::uint32_t paneIndex = 0; paneIndex < 3; ++paneIndex) {
            TreePane pane;
            pane.id = Layout::PaneID { paneIndex + 1 };
            pane.sessionID = paneIndex + 20;
            pane.label = "p";
            pane.tags = { "mine" };
            pane.attendees = { { 1, TreeAttendeeKind::Person }, { 2, TreeAttendeeKind::Agent } };
            pane.facts.programTitle = "make all";
            pane.facts.currentDirectory = "/srv/app";
            pane.facts.userVariables = { { .name = "branch", .value = "main" } };
            pane.facts.exitStatus = 2;
            Ubiquitty::OSC::ProgramStatus status;
            status.id = "job";
            status.state = Ubiquitty::OSC::ProgramState::Working;
            status.message = "compiling";
            pane.records.store.records.push_back(std::move(status));
            pane.facts.attention.hasUnseenBell = true;
            tab.panes.push_back(std::move(pane));
        }
        TopologyNode inner;
        inner.children.resize(2);
        tab.split.children.push_back(TopologyNode {});
        tab.split.children.push_back(std::move(inner));
        window.tabs.push_back(std::move(tab));
        window.hostZoomedPane = TreeZoom { .tab = 0, .pane = 1 };
        workspace.windows.push_back(std::move(window));
    }
    std::vector<std::uint8_t> body;
    const bool isWritten = encodeWorkspaceStructure(body, workspace);
    assert(isWritten && "the seed workspace encodes");
    (void)isWritten;
    return body;
}

/// Asserts that both spellings of a tree's outline are text.
///
/// - Parameter tree: The tree, assembled from whatever bytes.
void checkOutline(const SessionTree& tree)
{
    using namespace Ubiquitty::App;
    OutlineConfiguration configuration;
    for (Ubiquitty::Preferences::Effective::TitleComposition* composition : { &configuration.tabTitle, &configuration.paneTitle }) {
        composition->showsAppTitle = true;
        composition->showsDirectory = true;
        composition->showsDirectoryFullPath = true;
        composition->showsProcess = true;
        composition->showsProcessArguments = true;
        composition->showsShellCommand = true;
        composition->showsTTYName = true;
        composition->showsProgramStatus = true;
    }
    const SessionOutline outline = buildSessionOutline(tree, configuration);
    const std::string printed = outlineText(outline);
    assert(Ubiquitty::Unicode::wellFormedUTF8PrefixLength(printed) == printed.size() && "the lines of an outline are UTF-8");
    const std::string spelled = Ubiquitty::Codec::JSON::serialize(outlineJSON(outline));
    assert(Ubiquitty::Unicode::wellFormedUTF8PrefixLength(spelled) == spelled.size() && "the JSON of an outline is UTF-8");
    bool isJSON = false; // bool-ok: the parser's out flag
    const Ubiquitty::Codec::JSON::Value read = Ubiquitty::Codec::JSON::parse(spelled, isJSON);
    assert(isJSON && read.find("rows") && read.find("rows")->items().size() == outline.rows.size() && "the JSON of an outline parses to its rows");
    (void)read;
}

void check(std::span<const std::uint8_t> body)
{
    const std::optional<TreeWorkspace> workspace = decodeWorkspaceStructure(body);
    if (!workspace)
        return;
    assert(workspace->windows.empty() ? workspace->hostActiveWindow == 0 : workspace->hostActiveWindow < workspace->windows.size());
    std::size_t leaves = 0;
    for (const TreeWindow& window : workspace->windows) {
        assert(!window.tabs.empty() && window.hostActiveTab < window.tabs.size());
        assert(!window.hostZoomedPane || (window.hostZoomedPane->tab < window.tabs.size() && window.hostZoomedPane->pane < window.tabs[window.hostZoomedPane->tab].panes.size()));
        for (const TreeTab& tab : window.tabs) {
            assert(!tab.panes.empty() && tab.hostFocusedPane < tab.panes.size());
            assert(leavesOf(tab.split) == tab.panes.size());
            leaves += tab.panes.size();
            // What a reader calls without a check of its own.
            (void)tab.titlePane();
        }
    }
    assert(leaves <= maxStructureLeaves);
    // A structure a client was sent assembles with no record held for it.
    const SessionTree tree = assembleSessionTree(std::span { &*workspace, 1 }, 1, 2, [](Ubiquitty::Protocol::Sync::ID128) { return HeldPaneRecords {}; });
    assert(tree.workspaces.size() == 1);
    checkOutline(tree);
    // The same structure with records a program filled with these bytes.
    const std::string bytes(reinterpret_cast<const char*>(body.data()), std::min<std::size_t>(body.size(), 96));
    Ubiquitty::Protocol::Sync::PaneFacts facts;
    facts.revision = 1;
    for (std::string* text : { &facts.programTitle, &facts.currentDirectory, &facts.homeDirectory, &facts.processChain, &facts.processChainWithArguments, &facts.shellCommandName, &facts.terminalName, &facts.hostName,
             &facts.foreground.name, &facts.badgeText, &facts.attention.notificationTitle, &facts.attention.notificationBody })
        *text = bytes;
    Ubiquitty::Protocol::Sync::StatusRecords status;
    status.store.records.push_back({ .id = bytes, .app = bytes, .title = bytes, .message = bytes, .state = Ubiquitty::OSC::ProgramState::Working });
    checkOutline(assembleSessionTree(std::span { &*workspace, 1 }, 1, 2, [&](Ubiquitty::Protocol::Sync::ID128) { return HeldPaneRecords { .facts = &facts, .status = &status }; }));
    std::vector<std::uint8_t> encoded;
    const bool isWritten = encodeWorkspaceStructure(encoded, *workspace);
    assert(isWritten && "a structure the decoder accepted encodes");
    (void)isWritten;
    const std::optional<TreeWorkspace> again = decodeWorkspaceStructure(encoded);
    assert(again && "a structure's own body decodes");
    std::vector<std::uint8_t> reencoded;
    const bool isRewritten = encodeWorkspaceStructure(reencoded, *again);
    assert(isRewritten && reencoded == encoded && "a body is a fixed point of decode and encode");
    (void)isRewritten;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
        return 0;
    const std::span<const std::uint8_t> input(data + 1, size - 1);
    if (data[0] % 2 == 0) {
        check(input);
        return 0;
    }
    static const std::vector<std::uint8_t> seed = seedBody();
    std::vector<std::uint8_t> body = seed;
    for (std::size_t index = 0; index + 2 < input.size(); index += 3) {
        const std::size_t offset = (std::size_t { input[index] } << 8 | input[index + 1]) % body.size();
        body[offset] ^= input[index + 2];
    }
    check(body);
    return 0;
}
