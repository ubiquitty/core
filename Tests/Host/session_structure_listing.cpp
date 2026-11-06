// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/session_structure_listing.cpp — the daemon describes each
// workspace in one record and sends it to whoever reads the workspace, and a
// client that lists the host is sent every workspace and the records of every
// pane it has not bound (Docs/sync.md §7.3, §7.7).
//
//   1. With nobody to read one, no structure is built.
//   2. A person attached to a pane is sent the structure of that pane's
//      workspace and no other: its split, each leaf's three names and its
//      cells, and who is attached. An agent attached to a pane is sent none.
//   3. A connection bound to no session subscribes: it is told which
//      workspaces to expect, is sent each one's structure, the workspace it
//      could restore among them, and each pane's facts and status with no
//      screen. The tree Core assembles from them is the host's.
//   4. A structure is sent again when what it says changes and not
//      otherwise: a label, a size, someone attaching, a session made and one
//      ended. A workspace that ends is withdrawn and its pane unlisted.
//   5. A person who lists is listed only the panes they have not bound.
//   6. Unsubscribed, a connection is unlisted every pane and owed nothing.
//
// Friend seam, the QUIC-off host build, as geometry_follows_hands. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/TerminalOperations.h"
#include "Host/FullCarrier.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Tree = Ubiquitty::Session;
namespace OSC = Ubiquitty::OSC;

namespace {

std::uint64_t wallNow = 1'700'000'000'000;
std::uint64_t testWallClock() { return wallNow; }

/// The structures a client holds, decoded, by workspace id.
std::map<std::uint32_t, Tree::TreeWorkspace> structuresOf(const UbiquittyTest::ScreenClient& client)
{
    std::map<std::uint32_t, Tree::TreeWorkspace> structures;
    for (const auto& [workspaceID, held] : client.connection().workspaceStructures()) {
        std::optional<Tree::TreeWorkspace> workspace = Tree::decodeWorkspaceStructure(held.record);
        CHECK_MSG(workspace.has_value(), "a structure the daemon sent does not decode");
        if (workspace) {
            CHECK_MSG(workspace->id == workspaceID && workspace->revision == held.revision, "a structure's head and its record disagree");
            structures[workspaceID] = std::move(*workspace);
        }
    }
    return structures;
}

/// The tree a listing client draws: its structures and the records of the
/// panes it was listed.
Tree::SessionTree treeOf(const UbiquittyTest::ScreenClient& client)
{
    std::vector<Tree::TreeWorkspace> structures;
    for (auto& [workspaceID, workspace] : structuresOf(client))
        structures.push_back(std::move(workspace));
    return Tree::assembleSessionTree(structures, 1, wallNow, [&](Sync::ID128 paneID) {
        return Tree::HeldPaneRecords { .facts = client.connection().listedPaneFacts(paneID), .status = client.connection().listedPaneStatus(paneID) };
    });
}

} // namespace

int main()
{
    // The agent's carrier closes before the daemon's teardown writes to it,
    // and the daemon's own process ignores SIGPIPE too.
    ::signal(SIGPIPE, SIG_IGN);
    Daemon d;
    Screen::setWallClock(d, &testWallClock);
    const Sync::ScopeSet controlling { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control };

    // A workspace of two panes side by side, and a session on its own.
    auto& left = Screen::makeSession(d, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    auto& right = Screen::makeSession(d, 30, 100, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    auto& alone = Screen::makeSession(d, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    const std::uint32_t work = Screen::nameWorkspace(d, left, "work");
    Screen::splitInto(d, left, right, Ubiquitty::Panes::Orientation::Vertical);
    for (auto* session : { &left, &right, &alone })
        Screen::publish(d, *session, Ubiquitty::Runtime::FrameClass::Structural);
    Ubiquitty::processBytes(left.term.get(), "\x1b]2;editor\x07");
    Ubiquitty::processBytes(right.term.get(), "\x1b]2;build\x07\x1b]7501;id=job:state=blocked:kind=question\x1b\\\x1b]6;1;bg;red;brightness;200\x07");
    Ubiquitty::processBytes(alone.term.get(), "\x1b]2;logs\x07\x1b]9;4;1;40\x07");

    // 1. Nobody reads a structure: none is built.
    Screen::service(d);
    CHECK_MSG(Screen::structuresBuilt(d) == 0, "a structure was built with nobody to read it");

    // 2. A person attached to the left pane.
    const std::size_t personIndex = Screen::attach(d, left.id);
    Screen::grantScope(d, personIndex, controlling);
    Screen::setTokenID(d, personIndex, "person");
    Screen::markInteractiveUser(d, personIndex);
    UbiquittyTest::ScreenClient person { 24, 80 };
    Screen::greet(d, personIndex, person);
    Screen::deliver(d, personIndex, person);
    std::map<std::uint32_t, Tree::TreeWorkspace> seen = structuresOf(person);
    CHECK_MSG(seen.size() == 1 && seen.contains(work), "a person was not sent the structure of the workspace they are attached to, and that one alone");
    if (seen.contains(work)) {
        const Tree::TreeWorkspace& structure = seen[work];
        CHECK_MSG(structure.name == "work" && structure.revision >= 1 && structure.windows.size() == 1 && structure.windows[0].tabs.size() == 1, "the structure does not name the workspace or hold its one window and tab");
        const Tree::TreeTab& tab = structure.windows[0].tabs[0];
        CHECK_MSG(tab.panes.size() == 2 && tab.split.children.size() == 2 && tab.split.axis == Sync::SplitAxis::Horizontal, "the structure does not hold the split, side by side");
        if (tab.panes.size() == 2 && tab.split.children.size() == 2) {
            const Sync::TopologyNode& first = tab.split.children[0];
            const Sync::TopologyNode& second = tab.split.children[1];
            CHECK_MSG(first.paneID == left.screenSource->paneID() && second.paneID == right.screenSource->paneID(), "a leaf does not name its pane as the screen plane does");
            CHECK_MSG(tab.panes[0].sessionID == left.id && tab.panes[1].sessionID == right.id, "a leaf does not name the session behind it");
            CHECK_MSG(std::to_underlying(tab.panes[0].id) == Screen::layoutPane(d, left) && std::to_underlying(tab.panes[1].id) == Screen::layoutPane(d, right), "a leaf does not name the pane as a layout verb does");
            CHECK_MSG(first.columns == 80 && first.rows == 24 && second.columns == 100 && second.rows == 30, "a leaf does not carry its pane's grid");
            CHECK_MSG(second.offsetColumns == 80 && second.offsetRows == 0 && tab.split.columns == 180 && tab.split.rows == 30, "the split's cells are not the host layout's");
            CHECK_MSG(first.structuralGeneration == left.screenSource->structuralGeneration(), "a leaf does not carry the generation that gave the pane its grid");
            // A structural frame of the pane moves that generation, and the
            // structure that names it follows.
            const std::uint64_t before = left.screenSource->structuralGeneration();
            Screen::publish(d, left, Ubiquitty::Runtime::FrameClass::Structural);
            Screen::deliver(d, personIndex, person);
            const std::uint64_t after = left.screenSource->structuralGeneration();
            CHECK_MSG(after > before && structuresOf(person)[work].windows[0].tabs[0].split.children[0].structuralGeneration == after, "a structural frame's generation did not reach the pane's leaf");
            CHECK_MSG(tab.panes[0].attendees.size() == 1 && tab.panes[0].attendees[0].kind == Tree::TreeAttendeeKind::Person && tab.panes[1].attendees.empty(), "the leaves do not say who is attached");
        }
    }
    CHECK_MSG(person.count(Sync::FrameKind::PaneListed) == 0, "a client that did not subscribe was listed a pane");
    // An agent attached to the lone session is sent no structure at all. No
    // person is attached there to approve it, so it holds the standing grant a
    // person's allow-for-this-session answer gives; without one the host
    // refuses the attach with NoConsent. Its transport is a full socket, as
    // every caller fixture's is: an agent with a standing grant is welcomed
    // inside the admission, whose flush would otherwise write to no descriptor
    // and drop the client, and a full one leaves the bytes for this test to
    // carry.
    Ubiquitty::HostTest::FullCarrier agentCarrier;
    CHECK_MSG(agentCarrier.isReady(), "premise: no carrier for the agent");
    const std::size_t agentIndex = Screen::attach(d, alone.id, agentCarrier.takeDaemonEnd());
    Screen::grantScope(d, agentIndex, controlling);
    Screen::setTokenID(d, agentIndex, "agent");
    Screen::grantAgentSession(d, alone, agentIndex);
    UbiquittyTest::ScreenClient agent { 24, 80 };
    Screen::greet(d, agentIndex, agent);
    Screen::deliver(d, agentIndex, agent);
    CHECK_MSG(agent.connection().state() == Sync::ConnectionState::Attached, "premise: the agent was not admitted to the lone session");
    CHECK_MSG(agent.connection().workspaceStructures().empty() && agent.count(Sync::FrameKind::WorkspaceStructure) == 0, "an agent attached to one session was sent a workspace's structure");

    // 3. A connection bound to no session lists the host.
    Tree::HostLayoutRecord record;
    record.arrangement.name = "kept";
    Tree::WorkspaceWindow window;
    Tree::WorkspaceTab recordedTab;
    recordedTab.root = Tree::PaneNode::makeLeaf({}, "shell");
    recordedTab.root.directory = "/home/user";
    window.tabs.push_back(recordedTab);
    record.arrangement.windows.push_back(window);
    record.panes = { { 80, 24, "", "", {}, {} } };
    Screen::addRestorable(d, 900, record);
    const std::size_t listingIndex = Screen::attachControlOnly(d);
    Screen::grantScope(d, listingIndex, controlling);
    Screen::setTokenID(d, listingIndex, "listing");
    Screen::markInteractiveUser(d, listingIndex);
    UbiquittyTest::ScreenClient listing { 24, 80 };
    Screen::greet(d, listingIndex, listing);
    CHECK_MSG(listing.connection().workspaceStructures().empty(), "a connection that reads nothing was sent a structure");
    CHECK(listing.call(R"({"jsonrpc":"2.0","id":1,"method":"host.sessions.subscribe","params":{}})"));
    // Listing a pane is what reads its records: nothing else asks.
    Screen::deliver(d, listingIndex, listing);
    Screen::deliver(d, listingIndex, listing);
    const std::vector<std::string> answers = listing.takeResults();
    CHECK_MSG(answers.size() == 1, "the subscribe call was not answered once");
    const std::uint32_t aloneWorkspace = Screen::workspaceID(d, alone);
    if (answers.size() == 1) {
        const std::string& answer = answers[0];
        CHECK_MSG(answer.find("\"result\"") != std::string::npos && answer.find("\"panes\":3") != std::string::npos && answer.find("\"hostNowUnixMilliseconds\":1700000000000") != std::string::npos, answer.c_str());
        for (const std::uint32_t id : { work, aloneWorkspace, std::uint32_t { 900 } })
            CHECK_MSG(answer.find(std::to_string(id)) != std::string::npos, ("the answer does not name workspace " + std::to_string(id) + ": " + answer).c_str());
    }
    seen = structuresOf(listing);
    CHECK_MSG(seen.size() == 3 && seen.contains(work) && seen.contains(aloneWorkspace) && seen.contains(900), "a listing was not sent every workspace's structure");
    CHECK_MSG(listing.connection().listedPanes().size() == 3 && listing.count(Sync::FrameKind::PaneListed) == 3, "a listing was not listed every pane");
    CHECK_MSG(listing.connection().boundPanes().empty() && listing.count(Sync::FrameKind::PaneBind) == 0 && listing.count(Sync::FrameKind::CheckpointHead) == 0 && listing.count(Sync::FrameKind::Update) == 0, "a listing was bound a pane or sent a screen");
    const Tree::SessionTree tree = treeOf(listing);
    CHECK_MSG(tree.workspaces.size() == 3, "the assembled tree does not hold the host's three workspaces");
    if (const std::optional<Tree::TreePanePath> path = tree.locate(Tree::Layout::PaneID { Screen::layoutPane(d, right) })) {
        const Tree::TreePane& pane = tree.pane(*path);
        CHECK_MSG(pane.title.programTitle == "build" && pane.status.kind == Ubiquitty::App::StatusKind::Blocked && pane.status.tabColor.has_value(), "the listed pane's facts and status did not reach the tree");
        CHECK_MSG(tree.workspaces[path->workspace].status.kind == Ubiquitty::App::StatusKind::Blocked && tree.workspaces[path->workspace].status.paneCount == 2, "the workspace's status is not its panes' rolled up");
        CHECK_MSG(tree.workspaces[path->workspace].attendance.people == 1, "the tree does not count the person attached");
    } else {
        CHECK_MSG(false, "the tree does not hold the right-hand pane");
    }
    if (const std::optional<Tree::TreePanePath> path = tree.locate(Tree::Layout::PaneID { Screen::layoutPane(d, alone) })) {
        CHECK_MSG(tree.pane(*path).title.programTitle == "logs" && tree.pane(*path).status.progress.value == 40, "the lone pane's facts did not reach the tree");
        CHECK_MSG(tree.workspaces[path->workspace].attendance.agents == 1, "the tree does not count the agent attached");
    } else {
        CHECK_MSG(false, "the tree does not hold the lone pane");
    }
    const auto kept = std::ranges::find(tree.workspaces, std::uint32_t { 900 }, &Tree::TreeWorkspace::id);
    CHECK_MSG(kept != tree.workspaces.end() && kept->isRestorable() && kept->name == "kept" && kept->windows.size() == 1 && kept->windows[0].tabs[0].panes[0].title.programTitle == "shell" && kept->windows[0].tabs[0].panes[0].title.currentDirectory == "/home/user",
        "the workspace the host could restore is not listed from its record");

    // 4. Nothing changed: nothing is sent again, however many passes.
    const std::size_t sentToListing = listing.count(Sync::FrameKind::WorkspaceStructure);
    const std::size_t sentToPerson = person.count(Sync::FrameKind::WorkspaceStructure);
    for (int pass = 0; pass < 5; ++pass) {
        Screen::deliver(d, listingIndex, listing);
        Screen::deliver(d, personIndex, person);
    }
    CHECK_MSG(listing.count(Sync::FrameKind::WorkspaceStructure) == sentToListing && person.count(Sync::FrameKind::WorkspaceStructure) == sentToPerson, "a structure that did not change was sent again");
    // A label: a call on any connection asks for the structures to be read
    // again, and the one that reads differently goes to both readers.
    CHECK(Screen::setLabel(d, { Tree::Identity::ObjectKind::Pane, Screen::layoutPane(d, right) }, "builder"));
    CHECK(listing.call(R"({"jsonrpc":"2.0","id":2,"method":"host.describe","params":{}})"));
    Screen::deliver(d, listingIndex, listing);
    Screen::deliver(d, personIndex, person);
    CHECK_MSG(listing.count(Sync::FrameKind::WorkspaceStructure) == sentToListing + 1 && person.count(Sync::FrameKind::WorkspaceStructure) == sentToPerson + 1, "a label did not send its workspace's structure, once, to each reader");
    CHECK_MSG(structuresOf(listing)[work].windows[0].tabs[0].panes[1].label == "builder" && structuresOf(person)[work].windows[0].tabs[0].panes[1].label == "builder", "the label is not in the structure");
    CHECK_MSG(structuresOf(listing)[work].revision == structuresOf(person)[work].revision, "two readers of one workspace hold different revisions");
    // A size: the leaf's cells and the split's follow.
    right.pendingGeometry = std::pair { 120, 40 };
    Screen::endReadPass(d);
    Screen::deliver(d, listingIndex, listing);
    const Tree::TreeTab resized = structuresOf(listing)[work].windows[0].tabs[0];
    CHECK_MSG(resized.split.children[1].columns == 120 && resized.split.children[1].rows == 40 && resized.split.columns == 200 && resized.split.rows == 40, "a pane's new size is not in its workspace's structure");
    // Someone attaching to the right-hand pane.
    const std::uint64_t beforeAttach = structuresOf(listing)[work].revision;
    const std::size_t secondIndex = Screen::attach(d, right.id);
    Screen::grantScope(d, secondIndex, controlling);
    Screen::setTokenID(d, secondIndex, "second");
    Screen::markInteractiveUser(d, secondIndex);
    UbiquittyTest::ScreenClient second { 24, 80, Sync::ID128 { .low = 21, .high = 22 } };
    Screen::greet(d, secondIndex, second);
    Screen::deliver(d, secondIndex, second);
    Screen::deliver(d, listingIndex, listing);
    CHECK_MSG(structuresOf(listing)[work].revision > beforeAttach && structuresOf(listing)[work].windows[0].tabs[0].panes[1].attendees.size() == 1, "someone attaching is not in the structure");
    CHECK_MSG(structuresOf(second).contains(work), "the person who attached was not sent the workspace's structure");
    // 5. That person lists too: the pane they have bound is not listed.
    CHECK(second.call(R"({"jsonrpc":"2.0","id":1,"method":"host.sessions.subscribe","params":{}})"));
    Screen::deliver(d, secondIndex, second);
    const std::vector<Sync::ID128> listedToSecond = second.connection().listedPanes();
    CHECK_MSG(listedToSecond.size() == 2 && !std::ranges::contains(listedToSecond, right.screenSource->paneID()), "a pane a client has bound was listed to it as well");
    CHECK_MSG(structuresOf(second).size() == 3, "a person who lists was not sent every workspace");
    // A session made: its workspace and its pane reach the listing.
    const std::array<std::string, 2> command { "/bin/cat", "-u" };
    auto* made = Screen::spawnSession(d, 80, 24, command);
    CHECK_MSG(made != nullptr, "premise: a session could not be spawned");
    if (made) {
        Screen::deliver(d, listingIndex, listing);
        const std::uint32_t madeWorkspace = Screen::workspaceID(d, *made);
        CHECK_MSG(structuresOf(listing).contains(madeWorkspace) && listing.connection().listedPanes().size() == 4, "a session made after the listing began did not reach it");
        // And one ended: its pane is unlisted and its workspace withdrawn.
        const std::uint32_t madeID = made->id;
        Screen::destroySession(d, madeID);
        Screen::deliver(d, listingIndex, listing);
        CHECK_MSG(!structuresOf(listing).contains(madeWorkspace) && listing.connection().listedPanes().size() == 3, "a session that ended is still listed");
        CHECK_MSG(structuresOf(listing).size() == 3, "ending one workspace withdrew another");
    }

    // Someone leaving: one attendee fewer in the workspace's structure.
    const std::uint64_t beforeLeaving = structuresOf(listing)[work].revision;
    Screen::reap(d, secondIndex);
    Screen::deliver(d, listingIndex, listing);
    CHECK_MSG(structuresOf(listing)[work].revision > beforeLeaving && structuresOf(listing)[work].windows[0].tabs[0].panes[1].attendees.empty(), "someone leaving is still in the structure");
    // A pane only a listing reads is read at the listing's cadence, a second,
    // and one a client is attached to on the probe's budget.
    const std::uint64_t read = 50'000'000;
    Screen::publishRecords(d, right, read);
    Screen::publishRecords(d, left, read);
    Ubiquitty::processBytes(right.term.get(), "\x1b]2;linking\x07");
    Ubiquitty::processBytes(left.term.get(), "\x1b]2;saving\x07");
    Screen::noteRecordsDue(d, right, read + 100);
    Screen::noteRecordsDue(d, left, read + 100);
    CHECK_MSG(right.screenSource->facts().programTitle == "build" && right.recordsOwedAtMilliseconds == read + 1'000, "a pane only a listing reads was read inside the listing's cadence, or is not owed at it");
    CHECK_MSG(left.screenSource->facts().programTitle == "saving", "a pane a person is attached to waited for the listing's cadence");
    Screen::serveOwedRecords(d, right, read + 999);
    CHECK_MSG(right.screenSource->facts().programTitle == "build", "a listed pane's owed records were read before the cadence allowed");
    Screen::serveOwedRecords(d, right, read + 1'000);
    CHECK_MSG(right.screenSource->facts().programTitle == "linking", "a listed pane's owed records were not read when the cadence allowed");

    // 6. Unsubscribed: every pane unlisted, and a change is sent no more.
    const std::size_t beforeEnd = listing.count(Sync::FrameKind::WorkspaceStructure);
    CHECK(listing.call(R"({"jsonrpc":"2.0","id":3,"method":"host.sessions.unsubscribe","params":{}})"));
    Screen::deliver(d, listingIndex, listing);
    CHECK_MSG(listing.connection().listedPanes().empty(), "an unsubscribed connection is still listed a pane");
    CHECK(Screen::setLabel(d, { Tree::Identity::ObjectKind::Pane, Screen::layoutPane(d, left) }, "ed"));
    CHECK(listing.call(R"({"jsonrpc":"2.0","id":4,"method":"host.describe","params":{}})"));
    Screen::deliver(d, listingIndex, listing);
    Screen::deliver(d, personIndex, person);
    CHECK_MSG(listing.count(Sync::FrameKind::WorkspaceStructure) == beforeEnd, "an unsubscribed connection was sent a structure");
    CHECK_MSG(structuresOf(person)[work].windows[0].tabs[0].panes[0].label == "ed", "the person attached stopped being sent their workspace's structure");
    for (const UbiquittyTest::ScreenClient* client : { &person, &agent, &listing, &second })
        CHECK_MSG(client->connection().state() == Sync::ConnectionState::Attached, "a client was closed along the way");

    // 7. Least privilege. A pane's command line is what its screen is, so a
    //    listing carries it only to a connection that could read the screen.
    //    Control alone lists the names of what runs; Control with Observe
    //    lists the arguments; Observe without Control is refused the listing.
    right.paneFacts.processChain = "sh > make";
    right.paneFacts.processChainWithArguments = "sh > make -j8 TOKEN=secret";
    Screen::publishRecords(d, right, Screen::now());
    struct Lister {
        const char* name;
        Sync::ScopeSet scope;
        bool isAdmitted; // bool-ok: the expected verdict
        const char* arguments;
    };
    const Lister listers[] = {
        { "control", Sync::ScopeSet { Sync::Scope::Control }, true, "sh > make" },
        { "control+observe", Sync::ScopeSet { Sync::Scope::Control, Sync::Scope::Observe }, true, "sh > make -j8 TOKEN=secret" },
        { "observe", Sync::ScopeSet { Sync::Scope::Observe }, false, "" },
    };
    std::uint64_t instance = 40;
    for (const Lister& lister : listers) {
        const std::size_t index = Screen::attachControlOnly(d);
        Screen::grantScope(d, index, lister.scope);
        Screen::setTokenID(d, index, lister.name);
        Screen::markInteractiveUser(d, index);
        UbiquittyTest::ScreenClient client { 24, 80, Sync::ID128 { .low = ++instance, .high = 23 } };
        Screen::greet(d, index, client);
        if (client.connection().state() != Sync::ConnectionState::Attached) {
            // A token without Control does not reach a control-only attach.
            CHECK_MSG(!lister.isAdmitted, (std::string(lister.name) + ": a connection that should list was not welcomed").c_str());
            continue;
        }
        // Neither token drives, so each is a viewer: the listing asks no
        // role, and a viewer is listed as anyone is.
        CHECK_MSG(client.connection().welcome() && client.connection().welcome()->grantedRole == Sync::Role::Viewer, (std::string(lister.name) + ": a token that does not drive was granted more than a viewer's role").c_str());
        CHECK(client.call(R"({"jsonrpc":"2.0","id":7,"method":"host.sessions.subscribe","params":{}})"));
        Screen::deliver(d, index, client);
        Screen::deliver(d, index, client);
        const std::vector<std::string> results = client.takeResults();
        CHECK_MSG(results.size() == 1, (std::string(lister.name) + ": the subscribe call was not answered once").c_str());
        const bool isListed = results.size() == 1 && results[0].find("\"result\"") != std::string::npos; // bool-ok: a local reading
        CHECK_MSG(isListed == lister.isAdmitted, (std::string(lister.name) + ": the listing is admitted exactly under Control").c_str());
        const Sync::PaneFacts* facts = client.connection().listedPaneFacts(right.screenSource->paneID());
        if (!lister.isAdmitted) {
            CHECK_MSG(facts == nullptr && client.connection().listedPanes().empty() && client.connection().workspaceStructures().empty(), (std::string(lister.name) + ": a connection refused the listing was sent some of it").c_str());
            continue;
        }
        CHECK_MSG(facts != nullptr, (std::string(lister.name) + ": the pane's facts did not reach the listing").c_str());
        if (!facts)
            continue;
        CHECK_MSG(facts->processChainWithArguments == lister.arguments, (std::string(lister.name) + ": the command line reached, or was withheld from, the wrong connection: " + facts->processChainWithArguments).c_str());
        // What a listing is for is served under Control either way.
        CHECK_MSG(facts->processChain == "sh > make" && facts->programTitle == "linking", (std::string(lister.name) + ": the names or the title did not reach the listing").c_str());
    }

    // 8. A host at its cap, all of it in one workspace: sixty-four sessions
    //    in one tab. The structure is one record every reader takes, every
    //    pane is listed, and the host makes no sixty-fifth.
    {
        Daemon full;
        Screen::setWallClock(full, &testWallClock);
        constexpr std::size_t cap = 64;
        const auto show = [&](auto& made, std::size_t index) {
            Screen::publish(full, made, Ubiquitty::Runtime::FrameClass::Structural);
            const std::string title = "\x1b]2;pane " + std::to_string(index) + "\x07";
            Ubiquitty::processBytes(made.term.get(), title.c_str());
        };
        auto& root = Screen::makeSession(full, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
        (void)Screen::nameWorkspace(full, root, "full");
        show(root, 0);
        for (std::size_t index = 1; index < cap; ++index) {
            auto& made = Screen::makeSession(full, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
            Screen::splitInto(full, root, made, index % 2 ? Ubiquitty::Panes::Orientation::Vertical : Ubiquitty::Panes::Orientation::Horizontal);
            show(made, index);
        }
        const std::array<std::string, 2> command { "/bin/cat", "-u" };
        CHECK_MSG(Screen::spawnSession(full, 80, 24, command) == nullptr, "a host at its cap made another session");
        const std::size_t index = Screen::attachControlOnly(full);
        Screen::grantScope(full, index, controlling);
        Screen::setTokenID(full, index, "full");
        Screen::markInteractiveUser(full, index);
        UbiquittyTest::ScreenClient client { 24, 80, Sync::ID128 { .low = 61, .high = 62 } };
        Screen::greet(full, index, client);
        CHECK(client.call(R"({"jsonrpc":"2.0","id":8,"method":"host.sessions.subscribe","params":{}})"));
        for (int round = 0; round < 4; ++round)
            Screen::deliver(full, index, client);
        CHECK_MSG(client.connection().state() == Sync::ConnectionState::Attached, "listing a full host closed the connection");
        CHECK_MSG(client.connection().listedPanes().size() == cap, ("a full host listed " + std::to_string(client.connection().listedPanes().size()) + " of its 64 panes").c_str());
        const std::map<std::uint32_t, Tree::TreeWorkspace> structures = structuresOf(client);
        CHECK_MSG(structures.size() == 1, "a full host's one workspace is not one structure");
        const auto& held = client.connection().workspaceStructures();
        if (held.size() == 1)
            CHECK_MSG(held.begin()->second.record.size() <= Tree::maxWorkspaceStructureBytes, "a full workspace's record is past the bound proved for it");
        const Tree::SessionTree tree = treeOf(client);
        if (tree.workspaces.size() == 1 && tree.workspaces[0].windows.size() == 1 && tree.workspaces[0].windows[0].tabs.size() == 1) {
            const Tree::TreeTab& tab = tree.workspaces[0].windows[0].tabs[0];
            CHECK_MSG(tab.panes.size() == cap, "the structure does not hold every pane of a full tab");
            std::size_t described = 0;
            for (const Tree::TreePane& pane : tab.panes)
                described += pane.facts.revision != 0 && pane.title.programTitle.starts_with("pane ") ? 1 : 0;
            CHECK_MSG(described == cap, ("the facts of " + std::to_string(described) + " of 64 listed panes reached the tree").c_str());
            CHECK_MSG(tree.workspaces[0].status.paneCount == cap, "the workspace does not count its sixty-four panes");
        } else {
            CHECK_MSG(false, "a full host's tree is not one workspace of one window of one tab");
        }
    }

    if (failures == 0)
        std::printf("session_structure_listing: one record a workspace, sent to who reads it and when it changes; a listing is sent every workspace and each unbound pane's records with no screen\n");
    return failures == 0 ? 0 : 1;
}
