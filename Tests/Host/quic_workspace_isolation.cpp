// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/quic_workspace_isolation.cpp — a person on another machine
// changes the layout of the workspace their connection is attached to and no
// other (Docs/sync.md §7.3, the caller table), over real loopback QUIC
// against a spawned host.
//
// Two people dial the host with the token its bootstrap mints, each asking
// for a fresh workspace. The first names the second's workspace in
// `workspace.split`, `workspace.addTab`, `workspace.addWindow`,
// `workspace.closePane` and `window.close`, each call otherwise well formed
// and at the workspace's current revision: every one is refused as forbidden,
// naming the workspace the connection is attached to, the connection stays
// up, and the second person's structure keeps its revision and its tree. The
// same five calls on the first person's own workspace are served, and the
// structure it is sent says what each did.
//
// Usage: quic_workspace_isolation <path-to-ubiquitty-host>. Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"
#include "Host/HostConnection.h"
#include "Host/QUICDial.h"
#include "Host/ScreenClient.h"
#include "Platform/Host/HostSocketName.h"
#include "quic_test_mint.h"

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Sync = Ubiquitty::Protocol::Sync;
namespace Transport = Ubiquitty::Transport;
namespace Tree = Ubiquitty::Session;

namespace {

using UbiquittyTest::Dial;

/// One pane of a structure as a call names it: its window, tab, pane and
/// session numbers.
struct Leaf {
    std::uint32_t window = 0;
    std::uint32_t tab = 0;
    std::uint32_t pane = 0;
    std::uint32_t session = 0;

    friend bool operator==(const Leaf&, const Leaf&) = default;
};

/// A workspace's structure as one dial holds it.
struct Shape {
    std::uint64_t revision = 0;
    std::vector<Leaf> leaves;

    [[nodiscard]] std::vector<std::uint32_t> windows() const
    {
        std::vector<std::uint32_t> ids;
        for (const Leaf& leaf : leaves) {
            if (std::ranges::find(ids, leaf.window) == ids.end())
                ids.push_back(leaf.window);
        }
        return ids;
    }

    [[nodiscard]] std::size_t tabCount() const
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ids;
        for (const Leaf& leaf : leaves) {
            if (std::ranges::find(ids, std::pair { leaf.window, leaf.tab }) == ids.end())
                ids.emplace_back(leaf.window, leaf.tab);
        }
        return ids.size();
    }
};

/// The structure of `workspaceID` the client holds, decoded; nothing before
/// one has arrived.
std::optional<Shape> shapeOf(const UbiquittyTest::ScreenClient& client, std::uint32_t workspaceID)
{
    const auto& held = client.connection().workspaceStructures();
    const auto found = held.find(workspaceID);
    if (found == held.end())
        return std::nullopt;
    const std::optional<Tree::TreeWorkspace> workspace = Tree::decodeWorkspaceStructure(found->second.record);
    if (!workspace)
        return std::nullopt;
    Shape shape { .revision = found->second.revision, .leaves = {} };
    for (const Tree::TreeWindow& window : workspace->windows) {
        for (const Tree::TreeTab& tab : window.tabs) {
            for (const Tree::TreePane& pane : tab.panes)
                shape.leaves.push_back({ std::to_underlying(window.id), std::to_underlying(tab.id), std::to_underlying(pane.id), pane.sessionID });
        }
    }
    return shape;
}

/// The workspace a dial's Welcome attached it to; 0 before admission.
std::uint32_t attachedWorkspace(const UbiquittyTest::ScreenClient& client)
{
    const std::optional<Sync::Welcome> welcome = client.welcome();
    return welcome ? welcome->selection.workspaceID : 0;
}

/// Reads a dial's structure of `workspaceID` under its lock.
std::optional<Shape> heldShape(Dial& dial, std::uint32_t workspaceID)
{
    const std::scoped_lock held(dial.lock);
    return shapeOf(dial.client, workspaceID);
}

bool contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

/// Sends one JSON-RPC call on a dial and waits for its Result.
///
/// - Parameters:
///   - dial: The connection that calls.
///   - identifier: The call's id, unique on this dial.
///   - method: The method.
///   - parameters: The parameters, as JSON.
/// - Returns: The Result's text, or empty when none arrived in time.
std::string callOn(Dial& dial, int identifier, const std::string& method, const std::string& parameters)
{
    const std::string request = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(identifier) + ",\"method\":\"" + method + "\",\"params\":" + parameters + "}";
    {
        const std::scoped_lock held(dial.lock);
        if (!dial.client.call(request))
            return {};
    }
    const std::string named = "\"id\":" + std::to_string(identifier) + ",";
    const std::string namedLast = "\"id\":" + std::to_string(identifier) + "}";
    std::string answer;
    (void)dial.pumpUntil(10'000, [&](const UbiquittyTest::ScreenClient&) {
        // pumpUntil hands a const client; the results are taken under the
        // same lock it holds.
        for (std::string& result : dial.client.takeResults()) {
            if (contains(result, named) || contains(result, namedLast))
                answer = std::move(result);
        }
        return !answer.empty() || dial.hasClosed.load();
    });
    return answer;
}

bool isServed(const std::string& answer) { return contains(answer, "\"result\"") && !contains(answer, "\"error\""); }

std::string number(std::uint64_t value) { return std::to_string(value); }

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const char* hostBinary = argv[1];
    char homeTemplate[] = "/tmp/ubiquitty-quic-isolation.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "quic_workspace_isolation: mkdtemp failed\n");
        return 1;
    }
    const std::string controlDirectory = std::string(home) + "/.ubiquitty";
    const std::uint16_t port = UbiquittyTest::freeUDPPort();
    const std::string portArgument = std::to_string(port);

    const pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", controlDirectory.c_str(), 1);
        ::setenv("SHELL", "/bin/sh", 1);
        ::execl(hostBinary, hostBinary, "--daemon", "--quic-port", portArgument.c_str(), "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    const auto stopDaemon = [&] {
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
    };
    if (!Ubiquitty::HostTest::waitForDaemonReady(controlDirectory + "/" + Ubiquitty::Host::socketLeafForBinary(hostBinary))) {
        std::fprintf(stderr, "quic_workspace_isolation: premise: the daemon never accepted a connection\n");
        stopDaemon();
        return 1;
    }
    // Each person's token is the one the product's bootstrap mints for the
    // ssh-authenticated user: an interactive person's, which may drive and
    // create sessions. A token is good for one dial, so each person has one.
    std::string pin;
    std::string tokens[2];
    for (std::string& token : tokens) {
        std::uint16_t mintedPort = 0;
        if (!mintQUICTokenViaBootstrap(hostBinary, controlDirectory, token, mintedPort, pin) || pin.size() != 32 || mintedPort != port) {
            std::fprintf(stderr, "quic_workspace_isolation: premise: no QUIC token or certificate pin was minted\n");
            stopDaemon();
            return 1;
        }
    }

    // Two people, each dialling over QUIC for a workspace of their own.
    Sync::ReplicaSet mineReplicas;
    Sync::ReplicaSet theirsReplicas;
    Dial mine(mineReplicas, std::nullopt);
    Dial theirs(theirsReplicas, std::nullopt);
    const auto admitted = [](Dial& dial) {
        return dial.pumpUntil(10'000, [](const UbiquittyTest::ScreenClient& client) {
            const std::uint32_t workspace = attachedWorkspace(client);
            const std::optional<Shape> shape = workspace ? shapeOf(client, workspace) : std::nullopt;
            return shape && shape->leaves.size() == 1;
        });
    };
    CHECK_MSG(mine.start(port, Transport::BootstrapToken { .token = tokens[0] }, pin, { .target = Sync::AdmissionTarget::Workspace }), "premise: the first person's dial did not start");
    const bool wasMineAdmitted = admitted(mine);
    CHECK_MSG(theirs.start(port, Transport::BootstrapToken { .token = tokens[1] }, pin, { .target = Sync::AdmissionTarget::Workspace }), "premise: the second person's dial did not start");
    const bool wasTheirsAdmitted = admitted(theirs);
    std::uint32_t mineWorkspace = 0;
    std::uint32_t theirsWorkspace = 0;
    Sync::Role mineRole = Sync::Role::Viewer;
    {
        const std::scoped_lock held(mine.lock);
        mineWorkspace = attachedWorkspace(mine.client);
        if (const std::optional<Sync::Welcome> welcome = mine.client.welcome())
            mineRole = welcome->grantedRole;
    }
    {
        const std::scoped_lock held(theirs.lock);
        theirsWorkspace = attachedWorkspace(theirs.client);
    }
    if (!wasMineAdmitted || !wasTheirsAdmitted || mineWorkspace == 0 || theirsWorkspace == 0 || mineWorkspace == theirsWorkspace || mineRole != Sync::Role::Driver) {
        std::fprintf(stderr, "quic_workspace_isolation: premise: two people over QUIC were not each attached as a driver to a one-pane workspace of their own (workspaces %u and %u)\n", mineWorkspace, theirsWorkspace);
        stopDaemon();
        return 1;
    }
    const std::optional<Shape> theirsBefore = heldShape(theirs, theirsWorkspace);
    const std::optional<Shape> mineBefore = heldShape(mine, mineWorkspace);
    if (!theirsBefore || !mineBefore || theirsBefore->leaves.size() != 1 || mineBefore->leaves.size() != 1) {
        std::fprintf(stderr, "quic_workspace_isolation: premise: a person's structure did not arrive\n");
        stopDaemon();
        return 1;
    }
    const Leaf foreign = theirsBefore->leaves.front();
    const Leaf own = mineBefore->leaves.front();
    CHECK_MSG(foreign.pane != own.pane && foreign.window != own.window && foreign.session != own.session, "premise: the two workspaces share a window, pane or session number, so a call cannot name one apart from the other");

    // The first person names the second's workspace in every layout verb. Each
    // call is well formed and at that workspace's current revision, so where
    // it is turned away it is for whose workspace it is.
    int identifier = 0;
    const std::string foreignRevision = number(theirsBefore->revision);
    const std::vector<std::pair<std::string, std::string>> foreignCalls {
        { "workspace.split", "{\"pane\":" + number(foreign.pane) + ",\"orientation\":\"vertical\",\"ratio\":0.5}" },
        { "workspace.addTab", "{\"window\":" + number(foreign.window) + "}" },
        { "workspace.addWindow", "{\"window\":" + number(foreign.window) + ",\"pane\":" + number(foreign.pane) + ",\"expectedRevision\":\"" + foreignRevision + "\"}" },
        { "workspace.closePane", "{\"pane\":" + number(foreign.pane) + "}" },
        { "window.close", "{\"window\":" + number(foreign.window) + ",\"expectedRevision\":\"" + foreignRevision + "\"}" },
    };
    // The refusal Docs/sync.md §7.3 tabulates: forbidden, naming the
    // workspace the connection is attached to, and the one it named.
    const std::string attachedTo = "attached to workspace:" + number(mineWorkspace) + ", not workspace:" + number(theirsWorkspace);
    int refusals = 0;
    for (const auto& [method, parameters] : foreignCalls) {
        const std::string answer = callOn(mine, ++identifier, method, parameters);
        const bool wasRefused = contains(answer, "-32002") && contains(answer, method + ": a person on another machine changes only the workspace their connection is attached to") && contains(answer, attachedTo);
        CHECK_MSG(wasRefused, (method + " naming another person's workspace over QUIC was not refused for its workspace: " + (answer.empty() ? std::string("no answer") : answer)).c_str());
        CHECK_MSG(!mine.hasClosed.load(), (method + ": a refused remote person's connection was closed").c_str());
        if (wasRefused) {
            ++refusals;
            std::printf("quic_workspace_isolation: refused %s on workspace:%u from a QUIC person attached to workspace:%u\n", method.c_str(), theirsWorkspace, mineWorkspace);
        }
    }

    // Nothing the refused calls named was changed: the second person, who is
    // sent every revision of their workspace, still holds the one they had.
    (void)theirs.pumpUntil(500, [](const UbiquittyTest::ScreenClient&) { return false; });
    const std::optional<Shape> theirsAfterRefusals = heldShape(theirs, theirsWorkspace);
    CHECK_MSG(theirsAfterRefusals && theirsAfterRefusals->revision == theirsBefore->revision && theirsAfterRefusals->leaves == theirsBefore->leaves, "a refused call changed the other person's workspace");
    CHECK_MSG(!theirs.hasClosed.load(), "a refused call against the other person's workspace closed their connection");

    // The same verbs on the first person's own workspace are served.
    const auto ownShapeBecomes = [&](const std::function<bool(const Shape&)>& isExpected) {
        return mine.pumpUntil(10'000, [&](const UbiquittyTest::ScreenClient& client) {
            const std::optional<Shape> shape = shapeOf(client, mineWorkspace);
            return shape && isExpected(*shape);
        });
    };
    int served = 0;
    const auto serve = [&](const std::string& method, const std::string& parameters, const std::function<bool(const Shape&)>& isExpected) {
        const std::string answer = callOn(mine, ++identifier, method, parameters);
        const bool wasServed = isServed(answer);
        CHECK_MSG(wasServed, (method + " on the remote person's own workspace was not served: " + (answer.empty() ? std::string("no answer") : answer)).c_str());
        const bool didChange = wasServed && ownShapeBecomes(isExpected);
        CHECK_MSG(didChange, (method + " on the remote person's own workspace was served but its structure does not say so").c_str());
        if (wasServed && didChange) {
            ++served;
            std::printf("quic_workspace_isolation: served %s on workspace:%u from the QUIC person attached to it\n", method.c_str(), mineWorkspace);
        }
        return heldShape(mine, mineWorkspace).value_or(Shape {});
    };

    Shape shape = serve("workspace.split", "{\"pane\":" + number(own.pane) + ",\"orientation\":\"vertical\",\"ratio\":0.5}", [](const Shape& now) { return now.leaves.size() == 2; });
    std::uint32_t splitPane = 0;
    for (const Leaf& leaf : shape.leaves) {
        if (leaf.pane != own.pane)
            splitPane = leaf.pane;
    }
    shape = serve("workspace.addTab", "{\"window\":" + number(own.window) + "}", [](const Shape& now) { return now.leaves.size() == 3 && now.tabCount() == 2; });
    shape = serve("workspace.addWindow", "{\"window\":" + number(own.window) + ",\"pane\":" + number(own.pane) + ",\"expectedRevision\":\"" + number(shape.revision) + "\"}", [](const Shape& now) { return now.windows().size() == 2; });
    std::uint32_t addedWindow = 0;
    for (const std::uint32_t window : shape.windows()) {
        if (window != own.window)
            addedWindow = window;
    }
    shape = serve("workspace.closePane", "{\"pane\":" + number(splitPane) + "}", [&](const Shape& now) {
        return splitPane && std::ranges::none_of(now.leaves, [&](const Leaf& leaf) { return leaf.pane == splitPane; });
    });
    shape = serve("window.close", "{\"window\":" + number(addedWindow) + ",\"expectedRevision\":\"" + number(shape.revision) + "\"}", [&](const Shape& now) {
        return addedWindow && now.windows().size() == 1 && now.windows().front() == own.window;
    });
    CHECK_MSG(!mine.hasClosed.load(), "the remote person's connection closed while changing their own workspace");

    // And the other person's workspace is still as it was.
    (void)theirs.pumpUntil(500, [](const UbiquittyTest::ScreenClient&) { return false; });
    const std::optional<Shape> theirsAtEnd = heldShape(theirs, theirsWorkspace);
    CHECK_MSG(theirsAtEnd && theirsAtEnd->revision == theirsBefore->revision && theirsAtEnd->leaves == theirsBefore->leaves, "the remote person's own changes reached the other person's workspace");

    mine.carrier.stop();
    theirs.carrier.stop();
    stopDaemon();
    CHECK_MSG(refusals == static_cast<int>(foreignCalls.size()) && served == 5, "not every verb was refused abroad and served at home");
    if (failures == 0)
        std::printf("quic_workspace_isolation: a person over QUIC was refused all %d layout verbs on another person's workspace, which kept its revision, and was served all %d on their own\n", refusals, served);
    return failures == 0 ? 0 : 1;
}
