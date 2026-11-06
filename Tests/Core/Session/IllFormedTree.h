// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// IllFormedTree.h — a host's tree with a byte no UTF-8 decoder accepts in
// every text it holds.
//
// A title is bytes a program chose and a label bytes a person typed; both
// travel as bytes. A tree is what every listing reads, so every text in one
// has to be text. This builds the worst case, a workspace whose every name,
// label, tag and record field carries `\xFF`, and walks every text a tree
// holds, for the cases that say what a reader is then handed.
//

#include "Core/OSC/Activity.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/Topology.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionTree.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace UbiquittyTest {

/// A text with a byte that begins no UTF-8 sequence in the middle of it.
///
/// - Parameter what: What the text is, so a failure names the field.
/// - Returns: The text.
inline std::string illFormed(std::string what)
{
    return "bad\xFF" + std::move(what);
}

/// The facts of a pane whose program wrote ill-formed bytes everywhere a
/// program can.
///
/// - Returns: The facts.
inline Ubiquitty::Protocol::Sync::PaneFacts illFormedFacts()
{
    Ubiquitty::Protocol::Sync::PaneFacts facts;
    facts.revision = 1;
    facts.programTitle = illFormed("title");
    facts.currentDirectory = illFormed("directory");
    facts.homeDirectory = illFormed("home");
    facts.currentFileURL = illFormed("file");
    facts.processChain = illFormed("chain");
    facts.processChainWithArguments = illFormed("arguments");
    facts.shellCommandName = illFormed("shell");
    facts.terminalName = illFormed("tty");
    facts.hostName = illFormed("host");
    facts.rows = 24;
    facts.columns = 80;
    facts.foreground.name = illFormed("program");
    facts.indicator = Ubiquitty::Protocol::Sync::IndicatorFacts { .red = 1, .green = 2, .blue = 3, .label = illFormed("indicator") };
    facts.badgeText = illFormed("badge");
    facts.userVariables = { { .name = illFormed("variable"), .value = illFormed("value") } };
    facts.attention = { .hasUnseenBell = false, .hasUnseenNotification = true, .notificationTitle = illFormed("notification"), .notificationBody = illFormed("body") };
    return facts;
}

/// The status set of a pane whose program reported ill-formed bytes.
///
/// - Returns: The set, of one record.
inline Ubiquitty::Protocol::Sync::StatusRecords illFormedStatus()
{
    Ubiquitty::Protocol::Sync::StatusRecords records;
    Ubiquitty::OSC::ProgramStatus status;
    status.id = illFormed("id");
    status.app = illFormed("app");
    status.title = illFormed("status title");
    status.message = illFormed("message");
    status.state = Ubiquitty::OSC::ProgramState::Working;
    records.store.records.push_back(std::move(status));
    return records;
}

/// One workspace of one window and two tabs, each of one pane, with an
/// ill-formed byte in every name, label and tag, as a peer may send it.
///
/// - Returns: The workspace's structure.
inline Ubiquitty::Session::TreeWorkspace illFormedStructure()
{
    using namespace Ubiquitty::Session;
    TreeWorkspace workspace;
    workspace.id = 9;
    workspace.name = illFormed("workspace");
    workspace.label = illFormed("workspace label");
    workspace.tags = { illFormed("workspace tag") };
    TreeWindow window;
    window.id = Layout::SurfaceID { 1 };
    window.label = illFormed("window label");
    window.tags = { illFormed("window tag") };
    for (std::uint32_t index = 0; index < 2; ++index) {
        TreeTab tab;
        tab.id = Layout::TabID { index + 10 };
        tab.name = illFormed("tab");
        tab.label = illFormed("tab label");
        tab.tags = { illFormed("tab tag") };
        TreePane pane;
        pane.id = Layout::PaneID { index + 20 };
        pane.sessionID = index + 30;
        pane.label = illFormed("pane label");
        pane.tags = { illFormed("pane tag") };
        pane.sessionLabel = illFormed("session label");
        pane.sessionTags = { illFormed("session tag") };
        tab.panes.push_back(std::move(pane));
        tab.split.paneID = { .low = index + 30, .high = 7 };
        tab.split.rows = 24;
        tab.split.columns = 80;
        window.tabs.push_back(std::move(tab));
    }
    workspace.windows.push_back(std::move(window));
    return workspace;
}

/// The tree a client assembles of that workspace and those records.
///
/// - Returns: The tree.
inline Ubiquitty::Session::SessionTree illFormedTree()
{
    const Ubiquitty::Session::TreeWorkspace structure = illFormedStructure();
    const Ubiquitty::Protocol::Sync::PaneFacts facts = illFormedFacts();
    const Ubiquitty::Protocol::Sync::StatusRecords status = illFormedStatus();
    return Ubiquitty::Session::assembleSessionTree(std::span { &structure, 1 }, 1, 100'000, [&](Ubiquitty::Protocol::Sync::ID128) {
        return Ubiquitty::Session::HeldPaneRecords { .facts = &facts, .status = &status };
    });
}

/// Calls `visit` with every text a tree holds: each level's names, labels
/// and tags, and each pane's records and what is derived from them.
///
/// - Parameters:
///   - tree: The tree.
///   - visit: Called with one text at a time.
template<typename Visit>
void forEachText(const Ubiquitty::Session::SessionTree& tree, Visit&& visit)
{
    const auto each = [&](const std::vector<std::string>& texts) {
        for (const std::string& text : texts)
            visit(text);
    };
    for (const auto& workspace : tree.workspaces) {
        visit(workspace.name);
        visit(workspace.label);
        each(workspace.tags);
        for (const auto& window : workspace.windows) {
            visit(window.label);
            each(window.tags);
            for (const auto& tab : window.tabs) {
                visit(tab.name);
                visit(tab.label);
                each(tab.tags);
                for (const auto& pane : tab.panes) {
                    visit(pane.label);
                    each(pane.tags);
                    visit(pane.sessionLabel);
                    each(pane.sessionTags);
                    visit(pane.program);
                    visit(pane.badge);
                    const auto& facts = pane.facts;
                    for (const std::string* text : { &facts.programTitle, &facts.currentDirectory, &facts.homeDirectory, &facts.currentFileURL, &facts.processChain, &facts.processChainWithArguments,
                             &facts.shellCommandName, &facts.terminalName, &facts.hostName, &facts.foreground.name, &facts.badgeText, &facts.attention.notificationTitle, &facts.attention.notificationBody })
                        visit(*text);
                    if (facts.indicator)
                        visit(facts.indicator->label);
                    for (const auto& variable : facts.userVariables) {
                        visit(variable.name);
                        visit(variable.value);
                    }
                    for (const auto& status : pane.records.store.records) {
                        visit(status.id);
                        visit(status.app);
                        visit(status.title);
                        visit(status.message);
                    }
                    const auto& title = pane.title;
                    for (const std::string* text : { &title.programTitle, &title.currentDirectory, &title.homeDirectory, &title.processChain, &title.processChainWithArguments, &title.shellCommandName,
                             &title.ttyName, &title.programStatus, &title.paneLabel, &title.tabTitle, &title.windowTitle, &title.tmuxWindowName, &title.hostName })
                        visit(*text);
                }
            }
        }
    }
}

} // namespace UbiquittyTest
