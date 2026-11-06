// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
// workspace_layout_projection — a client that mirrors pane arrangements is told
// the one the host has, in the workspace's structure, and a layout verb it
// sends is answered.
//
// A fixture starts with one hosted pane and exercises the host-owned workspace
// as it gains windows, tabs and splits. The client's structure and Core's layout
// emitter describe that same model; neither a received pane nor a Welcome is
// used to invent the fixture's expected owner or admission target.
//
// Three cases:
//
//   (a) an exact Workspace Greet is sent the workspace's structure, describing
//       one surface / one tab / one pane whose session is the one it bound
//   (b) a person attached to one session is sent the structure too, and an
//       agent is not
//   (c) a canonical structure Call changes the workspace the session roots: a tab
//       the model mints gets a session of its own, the result names both, the
//       new topology follows whole, and a verb the model cannot carry is refused;
//       closing the tab ends the session behind it, the root pane cannot be
//       closed by a verb, and a pane whose session ends leaves the tree
//   (d) a pane that cannot be backed by a session is not minted
//   (e) every structure operation is applied by the host as the model applies
//       it: a shadow Layout::Workspace takes the same twenty verbs, and after
//       each the layout the host re-emits is the shadow's description
//   (f) a client attaching later sees the tree the verb-driving one built,
//       whether it attaches to the root session or to a pane's, and a verb
//       from either reaches every client of the workspace
//   (g) a tmux control-mode layout written into a pane is terminal output,
//       not topology: the hosted tree keeps its own ids (session-model.md
//       rule 5, the outer authority)
//   (h) a verb that mints a pane mints the session behind it, so a token
//       holding Drive without NewSession is refused it
//   (i) a workspace verb is a person's act: an agent the human approved for one
//       pane cannot close the surface that holds every other pane's session
//   (j) so is seeing the topology: a layout names every pane in the workspace,
//       including the sessions the agent was never approved for, and neither the
//       attach nor the broadcast hands one to an agent
//   (k) labels: a window, pane, session and client a client labels come back in
//       the typed directory beside placement IDs, which no two workspaces share;
//       a label that reads as an ID is refused, and so is one naming a pane in
//       another client's workspace
//   (l) a canonical Call names the pane it splits or labels, and who may name it
//       is decided by the workspace: a person on this machine anywhere; a
//       remote person only in the workspace it is bound to; an agent must use
//       structure calls with exact-operation consent, and a
//       token without Drive is refused. A refused split leaves the
//       workspace's focus where it was, and a verb other than a split or a
//       label is not taken
//   (m) a pane a verb mints opens in the theme of the pane it is opened from —
//       the pane AddTab and AddSurface name, the pane a split divides — so the
//       client, which painted the new pane in that theme before any frame,
//       sees no change when the first checkpoint lands; a program's override
//       of the source's palette stays with that program
//   (n) the structure that first names a pane leaves before the pane's
//       PaneBind and every frame of it, and the one that retires it after
//       its last frame and its PaneUnbind, for a pane made and written to in
//       one pass and for a carrier whose socket was full meanwhile
//
// Exit 0 on success.
//
#include "Core/Panes/DividerPath.h"
#include "Core/Panes/PaneTree.h"
#include "Core/Panes/PaneTreeNode.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Scope.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Protocol/Tmux/PaneLock.h"
#include "Core/Session/HostListing.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/TermPalette.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/Stream.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"
#include "TrustedLocalPeer.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty {
namespace Host {

// Friend seam: seed a session, add a client on a real socketpair fd, drive real
// frames through the production canonical ingress.
struct WorkspaceLayoutProjectionAccess {
    static std::uint32_t makeSession(Daemon& d, std::string workspaceName)
    {
        auto s = std::make_unique<Daemon::Session>();
        s->id = d.m_nextSessionID++;
        s->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*s->term, 24, 80);
        s->capabilityPolicy.setGrantStore(&d.m_grantStore);
        s->term->capabilityPolicy = &s->capabilityPolicy;
        s->stream = TestDoubles::makeClosedStream();
        s->workspaceName = std::move(workspaceName);
        // As createSession makes it: a client's theme reaches the terminal
        // through the pane's screen source, which names who presents for it.
        s->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*s->term, Protocol::Sync::ID128 { .low = s->id, .high = 1 });
        const std::uint32_t id = s->id;
        d.m_sessions.push_back(std::move(s));
        return id;
    }

    static std::size_t addClient(Daemon& d, PAL::UnixFileDescriptor fileDescriptor,
        Protocol::Sync::ScopeSet scope = Protocol::Sync::ScopeSet { Protocol::Sync::Scope::Observe,
            Protocol::Sync::Scope::Drive, Protocol::Sync::Scope::Control, Protocol::Sync::Scope::NewSession },
        const std::string& agentLabel = {}, const std::string& tokenID = "workspace-layout-projection")
    {
        Daemon::Client c {};
        c.fileDescriptor = std::move(fileDescriptor);
        // A person unless the case labels it: a labelled or non-interactive token
        // bearer is an agent, held for consent on a session it did not create, and
        // a held client never reaches the attach most of these cases are about.
        c.hasToken = true;
        c.isInteractiveUser = agentLabel.empty();
        c.tokenID = tokenID;
        c.tokenScope = scope;
        // Its own ID, as the accept path gives each client: who presents for
        // a pane is told apart by it.
        c.clientID = d.m_nextClientID++;
        d.m_clients.push_back(std::move(c));
        // agentLabel is what the Greet carries; admission copies it onto the
        // client, and the gates the label decides run before that copy.
        d.m_clients.back().agentLabel = agentLabel;
        return d.m_clients.size() - 1;
    }

    /// The standing grant a human's consent leaves on a session, by token digest.
    static void grantAgentSession(Daemon& d, std::uint32_t sessionID, const std::string& tokenID)
    {
        if (Daemon::Session* s = d.findSession(sessionID)) {
            (void)s->addSessionGrant("token:" + tokenID);
        }
    }

    static void drive(Daemon& d, std::size_t index, const std::vector<std::uint8_t>& frame)
    {
        ScreenPlaneAccess::receive(d, index, frame);
    }

    /// Selects only identities already held by this fixture, before greeting.
    static std::optional<Protocol::Sync::AdmissionRequest> requestFor(Daemon& d, Protocol::Sync::AdmissionTarget target,
        std::uint32_t sessionID, const std::string& agentLabel)
    {
        Protocol::Sync::AdmissionRequest request;
        request.target = target;
        request.expectedDaemonInstanceID = d.m_daemonInstanceID;
        request.agentLabel = agentLabel;
        if (target == Protocol::Sync::AdmissionTarget::Control)
            return sessionID ? std::nullopt : std::optional { request };
        auto* selected = d.findSession(sessionID);
        if (!selected || (target != Protocol::Sync::AdmissionTarget::Session && target != Protocol::Sync::AdmissionTarget::Workspace))
            return std::nullopt;
        request.sessionID = sessionID;
        request.isAttachOnly = true;
        if (target == Protocol::Sync::AdmissionTarget::Workspace)
            request.workspaceID = d.hostedWorkspaceFor(*selected).workspaceID;
        return request;
    }

    /// Runs the production call dispatcher before the publication part of
    /// the pass, so a newly created program may write before its first frame.
    static void dispatchCalls(Daemon& d, std::size_t index)
    {
        if (index >= d.m_clients.size())
            return;
        auto& client = d.m_clients[index];
        if (!client.syncConnection || client.didOverflow)
            return;
        Protocol::Sync::serveCallsDeferred(*client.syncConnection,
            [&](std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request) -> std::optional<Codec::JSON::Value> {
                if (d.isAgentClient(client) && Protocol::Sync::isStructureMethod(request.method))
                    return d.beginStructureConsent(index, callIndex, request);
                return d.serveScreenCall(client, request);
            });
    }

    /// Writes what the daemon has queued for `i`, as its event loop would on POLLOUT.
    ///
    /// flushClient stops at EAGAIN and leaves the rest pending for the loop this
    /// harness never runs, so a reader that only drained the socket would see
    /// the first buffer's worth and take the remainder for never sent.
    static bool flushPending(Daemon& d, std::size_t i)
    {
        if (i >= d.m_clients.size())
            return false;
        d.flushClient(i);
        return i < d.m_clients.size() && d.m_clients[i].hasPendingOutput();
    }

    static std::size_t sessionCount(const Daemon& d) { return d.m_sessions.size(); }
    static std::size_t clientCount(const Daemon& d) { return d.m_clients.size(); }
    static bool hasHumanClient(const Daemon& d, std::uint32_t sessionID) { return d.sessionHasHumanClient(sessionID); }
    /// The session the daemon made last, or zero when it holds none.
    static std::uint32_t newestSession(const Daemon& d) { return d.m_sessions.empty() ? 0 : d.m_sessions.back()->id; }
    /// Makes a seeded client the person at this machine: its peer is the
    /// daemon's own binary, as the attach bridge's is.
    static void makeTrustedLocal(Daemon& d, std::size_t index) { seedTrustedLocalPeer(d.m_clients[index]); }
    /// The pane that has focus in the tab showing the session's pane.
    static std::uint32_t focusedPaneBeside(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::HostedWorkspace* workspace = d.workspaceHolding(sessionID);
        if (!workspace)
            return 0;
        const auto surface = workspace->layout.surfaceHolding(workspace->paneFor(sessionID));
        return std::to_underlying(workspace->layout.focusedPane(surface));
    }
    static bool isGrantedTo(Daemon& d, std::uint32_t sessionID, const std::string& tokenID)
    {
        const Daemon::Session* s = d.findSession(sessionID);
        return s && s->hasSessionGrantFor("token:" + tokenID);
    }
    /// Bytes the pane's program wrote, as the daemon's PTY reader would feed them.
    static void writeOutput(Daemon& d, std::uint32_t sessionID, std::string_view bytes)
    {
        if (Daemon::Session* s = d.findSession(sessionID))
            Ubiquitty::processBytes(s->term.get(), bytes);
    }
    static void useShell(Daemon& d, std::string path) { d.m_shellPath = std::move(path); }
    /// The live palette and the reset baseline a session's model holds.
    static std::optional<std::pair<Ubiquitty::TermPalette, Ubiquitty::TermPalette>> paletteOf(Daemon& d, std::uint32_t sessionID)
    {
        const Daemon::Session* s = d.findSession(sessionID);
        if (!s || !s->term)
            return std::nullopt;
        return std::pair { s->term->palette, s->term->paletteBaseline };
    }
    /// The pane a session draws into, in the workspace that holds it.
    static std::uint32_t paneOf(Daemon& d, std::uint32_t sessionID)
    {
        const Daemon::HostedWorkspace* workspace = d.workspaceHolding(sessionID);
        return workspace ? std::to_underlying(workspace->paneFor(sessionID)) : 0;
    }
    static std::optional<Protocol::Sync::ID128> paneIdentity(Daemon& d, std::uint32_t sessionID)
    {
        const auto* session = d.findSession(sessionID);
        return session && session->screenSource ? std::optional { session->screenSource->paneID() } : std::nullopt;
    }
    static void destroySession(Daemon& d, std::uint32_t id) { d.destroySession(id); }
    static void closeClientFileDescriptor(Daemon& d, std::size_t i)
    {
        if (i < d.m_clients.size())
            d.m_clients[i].fileDescriptor.reset();
    }
    static std::size_t workspaceCount(const Daemon& d) { return d.m_workspaces.size(); }
    static std::uint32_t allocateFrom(Daemon& daemon, std::uint32_t seed)
    {
        daemon.m_nextSessionID = seed;
        auto* session = daemon.createSession(80, 24);
        if (!session)
            return 0;
        const auto identity = session->id;
        daemon.destroySession(identity);
        return identity;
    }
    static std::size_t clientForSession(const Daemon& d, std::uint32_t sessionID)
    {
        for (std::size_t index = 0; index < d.m_clients.size(); ++index)
            if (d.m_clients[index].sessionID == sessionID)
                return index;
        return d.m_clients.size();
    }
    /// Fills the client's output well past what its socket takes. The
    /// frames travel through its connection, as every frame the daemon
    /// sends does: the client's Ack counts every stream byte it received,
    /// and a count past what the connection sent retires nothing, so bytes
    /// written around the connection would hold its bulk lane shut.
    /// - Returns: Whether the output still holds more than one capped frame.
    [[nodiscard]] static bool queueBackpressuredOutput(Daemon& d, std::size_t index)
    {
        const int capacity = 1024;
        ::setsockopt(d.m_clients[index].fileDescriptor.get(), SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity));
        auto& connection = *d.m_clients[index].syncConnection;
        std::vector<std::uint8_t> frame;
        for (int count = 0; count < 32768; ++count) {
            if (!connection.sendNotify(std::vector<std::uint8_t> { 0x7F, 'p' }))
                break;
            frame.clear();
            while (connection.popStreamFrame(frame, ScreenPlaneAccess::now()))
                ;
            d.queueToClient(index, frame);
        }
        d.flushClient(index);
        return index < d.m_clients.size() && !d.m_clients[index].didOverflow
            && d.m_clients[index].pendingOutputBytes() > Protocol::Sync::maxFrameBodyBytes;
    }
    static std::uint32_t namedSession(Daemon& d, const std::string& name)
    {
        const auto* session = d.findSessionByWorkspaceName(name);
        return session ? session->id : 0;
    }
};

} // namespace Host
} // namespace Ubiquitty

namespace {

using Ubiquitty::Host::WorkspaceLayoutProjectionAccess;
using Ubiquitty::Protocol::Sync::Scope;
using Ubiquitty::Protocol::Sync::ScopeSet;
using Ubiquitty::Session::SessionRow;
using Ubiquitty::Session::WorkspaceIntent;
using Ubiquitty::Session::WorkspaceIntentKind;
using Ubiquitty::Session::WorkspaceIntentResult;
using WorkspaceVerbMessage = Ubiquitty::Session::WorkspaceIntent;
namespace Sync = Ubiquitty::Protocol::Sync;

bool failed = false;

void check(bool condition, const char* what)
{
    if (condition)
        return;
    std::fprintf(stderr, "workspace_layout_projection: %s\n", what);
    failed = true;
}

/// One pane of a workspace as a client is told it: the pane's id in the
/// host's layout and the session that fills it.
struct WorkspacePane {
    std::uint32_t paneID {};
    std::uint32_t sessionID {};
};

/// One tab as a client is told it. `layout` is the tab's split in Core's
/// layout grammar, every node at the cells the host's layout gave it.
struct WorkspaceTab {
    std::uint32_t tabID {};
    std::string title;
    std::uint32_t focusedPaneID {};
    std::string layout;
    std::vector<WorkspacePane> panes;
};

/// One window as a client is told it.
struct WorkspaceSurface {
    std::uint32_t surfaceID {};
    std::uint32_t activeTabID {};
    std::uint32_t zoomedPaneID {};
    std::vector<WorkspaceTab> tabs;
};

/// The workspace facts a comparison reads, as bits of
/// `WorkspaceLayoutMessage::flags`.
constexpr std::uint8_t workspaceEndsWhenDetached = 0x01;
constexpr std::uint8_t workspaceRestoredAfterHostRestart = 0x02;

/// A workspace as a client is told it, read off the structure the host sent
/// (`decodeWorkspaceLayout`) or off a model (`describeWorkspace`), so the two
/// are compared field for field.
struct WorkspaceLayoutMessage {
    std::uint32_t workspaceID {};
    std::string name;
    std::uint8_t flags {};
    std::uint32_t activeSurfaceID {};
    std::vector<WorkspaceSurface> surfaces;
};

/// What a model does not hold of its workspace: the id, the name and the
/// facts a host says of it.
struct WorkspaceIdentity {
    std::uint32_t workspaceID {};
    std::string_view name;
    std::uint8_t flags {};
};

/// A model as a client would be told it.
///
/// - Parameters:
///   - live: The model.
///   - identity: What a host says of the workspace beside it.
///   - sessionForPane: Answers the session behind a pane.
///   - sessionContext: Passed to `sessionForPane`.
///   - dimensionsForPane: Answers a pane's grid.
///   - dimensionsContext: Passed to `dimensionsForPane`.
/// - Returns: The description.
WorkspaceLayoutMessage describeWorkspace(const Ubiquitty::Session::Layout::Workspace& live, WorkspaceIdentity identity,
    std::uint32_t (*sessionForPane)(void*, Ubiquitty::Session::Layout::PaneID), void* sessionContext,
    Ubiquitty::Protocol::Tmux::PaneDimensionsLookup dimensionsForPane, void* dimensionsContext)
{
    namespace Layout = Ubiquitty::Session::Layout;
    WorkspaceLayoutMessage message;
    message.workspaceID = identity.workspaceID;
    message.name.assign(identity.name);
    message.flags = identity.flags;
    message.activeSurfaceID = std::to_underlying(live.activeSurface());
    for (int surfaceIndex = 0; surfaceIndex < live.surfaceCount(); ++surfaceIndex) {
        const Layout::SurfaceID surface = live.surfaceIDAt(surfaceIndex);
        WorkspaceSurface described;
        described.surfaceID = std::to_underlying(surface);
        described.activeTabID = std::to_underlying(live.activeTab(surface));
        described.zoomedPaneID = std::to_underlying(live.zoomedPane(surface));
        for (int tabIndex = 0; tabIndex < live.tabCount(surface); ++tabIndex) {
            const Layout::TabID tab = live.tabAt(surface, tabIndex);
            WorkspaceTab describedTab;
            describedTab.tabID = std::to_underlying(tab);
            describedTab.title = live.tabTitle(surface, tab);
            if (const Ubiquitty::Panes::Tree* tree = live.paneTree(surface, tab)) {
                describedTab.focusedPaneID = static_cast<std::uint32_t>(tree->focusedPaneID());
                Ubiquitty::Protocol::Tmux::emitPaneLayout(*tree, tree->root(), dimensionsForPane, dimensionsContext, describedTab.layout);
                Ubiquitty::Panes::forEachLeaf(*tree, tree->root(), [&](int paneID) {
                    describedTab.panes.push_back({ static_cast<std::uint32_t>(paneID), sessionForPane(sessionContext, static_cast<Layout::PaneID>(static_cast<std::uint32_t>(paneID))) });
                });
            }
            described.tabs.push_back(std::move(describedTab));
        }
        message.surfaces.push_back(std::move(described));
    }
    return message;
}

/// Writes a node of a structure's split in the grammar `emitPaneLayout`
/// writes a model's in: each node's cells, and a leaf's pane.
void appendLayout(std::string& out, const Ubiquitty::Protocol::Sync::TopologyNode& node, std::span<const Ubiquitty::Session::TreePane> panes, std::size_t& nextLeaf)
{
    out += std::to_string(node.columns) + "x" + std::to_string(node.rows) + ",0,0";
    if (node.isLeaf()) {
        out += "," + std::to_string(nextLeaf < panes.size() ? std::to_underlying(panes[nextLeaf].id) : 0);
        ++nextLeaf;
        return;
    }
    const bool isSideBySide = node.axis == Ubiquitty::Protocol::Sync::SplitAxis::Horizontal; // bool-ok: picks the bracket
    out += isSideBySide ? '{' : '[';
    for (std::size_t child = 0; child < node.children.size(); ++child) {
        if (child != 0)
            out += ',';
        appendLayout(out, node.children[child], panes, nextLeaf);
    }
    out += isSideBySide ? '}' : ']';
}

/// A workspace as the structure record a host sent says it.
///
/// - Parameter record: The record's bytes.
/// - Returns: The description, or nothing when the record does not decode.
std::optional<WorkspaceLayoutMessage> decodeWorkspaceLayout(std::span<const std::uint8_t> record)
{
    const std::optional<Ubiquitty::Session::TreeWorkspace> structure = Ubiquitty::Session::decodeWorkspaceStructure(record);
    if (!structure || structure->windows.empty() || structure->hostActiveWindow >= structure->windows.size())
        return std::nullopt;
    WorkspaceLayoutMessage message;
    message.workspaceID = structure->id;
    message.name = structure->name;
    message.flags = static_cast<std::uint8_t>((structure->endsWhenDetached ? workspaceEndsWhenDetached : 0)
        | (structure->state == Ubiquitty::Session::TreeWorkspaceState::RestoredAfterHostRestart ? workspaceRestoredAfterHostRestart : 0));
    message.activeSurfaceID = std::to_underlying(structure->windows[structure->hostActiveWindow].id);
    for (const Ubiquitty::Session::TreeWindow& window : structure->windows) {
        if (window.tabs.empty() || window.hostActiveTab >= window.tabs.size())
            return std::nullopt;
        WorkspaceSurface surface;
        surface.surfaceID = std::to_underlying(window.id);
        surface.activeTabID = std::to_underlying(window.tabs[window.hostActiveTab].id);
        if (window.hostZoomedPane)
            surface.zoomedPaneID = std::to_underlying(window.tabs[window.hostZoomedPane->tab].panes[window.hostZoomedPane->pane].id);
        for (const Ubiquitty::Session::TreeTab& tab : window.tabs) {
            WorkspaceTab described;
            described.tabID = std::to_underlying(tab.id);
            described.title = tab.name;
            described.focusedPaneID = std::to_underlying(tab.panes[tab.hostFocusedPane].id);
            std::size_t leaves = 0;
            appendLayout(described.layout, tab.split, tab.panes, leaves);
            for (const Ubiquitty::Session::TreePane& pane : tab.panes)
                described.panes.push_back({ std::to_underlying(pane.id), pane.sessionID });
            surface.tabs.push_back(std::move(described));
        }
        message.surfaces.push_back(std::move(surface));
    }
    return message;
}

/// Every frame the daemon wrote to this end, decoded in order.
///
/// An attach writes several screen-plane records,
/// so the frame under test is found by type rather than by position, and a
/// case that read the first frame would pass or fail on what came before.
struct Frame {
    /// Absent only for a local assembled-structure or intent-completion event.
    std::optional<Sync::FrameKind> type;
    std::vector<std::uint8_t> payload;
    std::optional<WorkspaceIntentResult> intentResult = std::nullopt;
    std::uint32_t channel = 0;
};

/// A client's end of the screen plane, kept across reads of its socket.
struct ScreenEnd {
    std::unique_ptr<UbiquittyTest::ScreenClient> client;
    /// The fixture's preselected identity, never filled from a received pane.
    std::optional<Sync::AdmissionRequest> request;
    Sync::FrameDecoder decoder;
    /// The revision of each workspace's structure the list was last told.
    std::map<std::uint32_t, std::uint64_t> told;
    /// Every revision of each workspace's structure the client assembled, by
    /// revision: the record it held when a read of its socket ended.
    std::map<std::uint32_t, std::map<std::uint64_t, std::vector<std::uint8_t>>> assembled;
    std::map<std::string, WorkspaceIntent> pendingIntents;
    std::vector<std::string> directoryResults;
    std::uint64_t nextIntent = 0;
};

/// The screen-plane ends of the clients under test, by the test's end of
/// each one's socket.
std::map<int, ScreenEnd>& screenEnds()
{
    static std::map<int, ScreenEnd> ends;
    return ends;
}

std::vector<Frame> readAll(Ubiquitty::Host::Daemon& daemon, std::size_t index, int peerFileDescriptor)
{
    using Screen = Ubiquitty::Host::ScreenPlaneAccess;
    ScreenEnd& end = screenEnds()[peerFileDescriptor];
    check(end.client && end.client->hasStarted() && end.request, "the fixture must select and greet before reading its endpoint");
    if (!end.client || !end.client->hasStarted() || !end.request)
        return {};
    std::vector<Frame> frames;
    std::array<std::uint8_t, 65536> buffer {};
    for (;;) {
        // The daemon's pass ends by bringing every structure up to what its
        // workspaces hold and servicing each client's screen plane, and what
        // the client says back is read as its socket's bytes are.
        const std::uint64_t at = Screen::now();
        Screen::service(daemon, at);
        const std::vector<std::uint8_t> upstream = end.client->takeUpstream(at);
        if (!upstream.empty() && index < WorkspaceLayoutProjectionAccess::clientCount(daemon))
            WorkspaceLayoutProjectionAccess::drive(daemon, index, upstream);
        // Alternate the daemon's flush with our read: each write frees buffer
        // the next flush fills, until nothing is pending and nothing arrives.
        const bool stillPending = WorkspaceLayoutProjectionAccess::flushPending(daemon, index);
        pollfd waiting { .fd = peerFileDescriptor, .events = POLLIN, .revents = 0 };
        // Upstream work was handed to the daemon synchronously. If it made
        // no socket bytes yet, run the next service pass instead of sleeping
        // for a response that only that pass can produce.
        if (::poll(&waiting, 1, !upstream.empty() ? 0 : stillPending ? 1000
                                                                     : 300)
                <= 0
            || !(waiting.revents & POLLIN)) {
            if (stillPending || !upstream.empty())
                continue;
            break;
        }
        const ssize_t read = ::read(peerFileDescriptor, buffer.data(), buffer.size());
        if (read <= 0)
            break;
        const std::span<const std::uint8_t> bytes { buffer.data(), static_cast<std::size_t>(read) };
        end.decoder.append(bytes);
        while (const auto frame = end.decoder.next())
            frames.push_back({ frame->kind, { frame->body.begin(), frame->body.end() }, std::nullopt, frame->channel });
        end.client->read(bytes, at);
        for (const auto& response : end.client->takeResults()) {
            bool isJSON = false; // bool-ok: parser output
            const auto answer = Ubiquitty::Codec::JSON::parse(response, isJSON);
            if (isJSON && answer.stringOr("id").starts_with("directory.")) {
                end.directoryResults.push_back(response);
                continue;
            }
            const auto pending = end.pendingIntents.find(answer.stringOr("id"));
            check(isJSON && pending != end.pendingIntents.end(), "a typed layout answer lost its exact request");
            if (!isJSON || pending == end.pendingIntents.end())
                continue;
            WorkspaceIntentResult result;
            result.requestID = pending->second.requestID;
            const auto* value = answer.find("result");
            result.wasPerformed = answer.stringOr("jsonrpc") == "2.0" && !answer.find("error") && value && value->isObject();
            if (result.wasPerformed) {
                const auto identity = [&](const char* key, Ubiquitty::Session::Identity::ObjectKind kind) -> std::uint32_t {
                    const auto id = Ubiquitty::Session::Identity::parseObjectID(value->stringOr(key));
                    return id && id->kind == kind && id->number <= UINT32_MAX ? static_cast<std::uint32_t>(id->number) : 0;
                };
                using Ubiquitty::Session::Identity::ObjectKind;
                if (pending->second.verb == WorkspaceIntentKind::AddSurface || pending->second.verb == WorkspaceIntentKind::AddTab || pending->second.verb == WorkspaceIntentKind::SplitFocusedPane) {
                    if (pending->second.verb == WorkspaceIntentKind::AddSurface)
                        result.surfaceID = identity("window", ObjectKind::Window);
                    result.tabID = identity("tab", ObjectKind::Tab);
                    result.paneID = identity("pane", ObjectKind::Pane);
                    result.sessionID = identity("session", ObjectKind::Session);
                } else if (pending->second.verb == WorkspaceIntentKind::ExtractPaneToTab) {
                    result.tabID = identity("tab", ObjectKind::Tab);
                } else if (pending->second.verb == WorkspaceIntentKind::ExtractPaneToSurface || pending->second.verb == WorkspaceIntentKind::ExtractTabToSurface) {
                    result.surfaceID = identity("window", ObjectKind::Window);
                }
            }
            frames.push_back({ std::nullopt, { response.begin(), response.end() }, result });
            end.pendingIntents.erase(pending);
        }
        for (const auto& [workspaceID, held] : end.client->connection().workspaceStructures()) {
            std::uint64_t& told = end.told[workspaceID];
            if (held.revision == told)
                continue;
            told = held.revision;
            end.assembled[workspaceID][held.revision] = held.record;
            frames.push_back({ std::nullopt, held.record });
        }
    }
    check(end.decoder.error() == Sync::EnvelopeError::None, "the host emitted an unreadable canonical frame");
    return frames;
}

/// Reads a complete directory over the endpoint's existing typed connection.
/// - Parameters:
///   - daemon: The fixture host.
///   - index: The exact host client.
///   - peerFileDescriptor: The matching fixture endpoint.
///   - requestID: The unique listing correlation.
/// - Returns: Complete rows, or no value on refusal or incomplete delivery.
std::optional<std::vector<SessionRow>> readDirectory(Ubiquitty::Host::Daemon& daemon, std::size_t index, int peerFileDescriptor, std::uint32_t requestID)
{
    auto& end = screenEnds().at(peerFileDescriptor);
    const std::string id = "directory." + std::to_string(requestID);
    Ubiquitty::Session::HostListing listing(id);
    end.directoryResults.clear();
    if (!end.client->call(Ubiquitty::Session::HostListing::subscribeRequest(id)))
        return std::nullopt;
    std::optional<std::vector<SessionRow>> rows;
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        (void)readAll(daemon, index, peerFileDescriptor);
        for (const auto& result : end.directoryResults)
            (void)listing.noteAnswer(result, 0);
        end.directoryResults.clear();
        if (listing.isRefused() || listing.refresh(end.client->connection(), attempt + 1, 0) == Ubiquitty::Session::HostListing::Change::Malformed)
            break;
        if (listing.isListing() && listing.isWhole()) {
            rows = Ubiquitty::Session::sessionRows(listing.tree());
            break;
        }
    }
    check(end.client->call(Ubiquitty::Session::HostListing::unsubscribeRequest(id + ".end")), "directory cleanup was refused");
    (void)readAll(daemon, index, peerFileDescriptor);
    end.directoryResults.clear();
    return rows;
}

/// Sends a canonical structure Call using the client's received structure for
/// focused-pane and revision selection.
void driveLayoutIntent(Ubiquitty::Host::Daemon& daemon, std::size_t index, int peerFileDescriptor, const WorkspaceIntent& verb, std::optional<Ubiquitty::Panes::DividerPath> divider = std::nullopt)
{
    using Ubiquitty::Codec::JSON::Value;
    ScreenEnd& end = screenEnds().at(peerFileDescriptor);
    std::optional<Ubiquitty::Session::TreeWorkspace> workspace;
    if (const auto found = end.assembled.find(verb.workspaceID); found != end.assembled.end() && !found->second.empty())
        workspace = Ubiquitty::Session::decodeWorkspaceStructure(found->second.rbegin()->second);
    std::uint32_t focused = 0;
    std::uint32_t zoomed = 0;
    std::uint32_t activeTab = 0;
    if (workspace) {
        for (const auto& window : workspace->windows) {
            if (std::to_underlying(window.id) != verb.surfaceID || window.hostActiveTab >= window.tabs.size())
                continue;
            const auto& tab = window.tabs[window.hostActiveTab];
            activeTab = std::to_underlying(tab.id);
            if (tab.hostFocusedPane < tab.panes.size())
                focused = std::to_underlying(tab.panes[tab.hostFocusedPane].id);
            if (window.hostZoomedPane)
                zoomed = std::to_underlying(window.tabs[window.hostZoomedPane->tab].panes[window.hostZoomedPane->pane].id);
        }
    }
    Value parameters = Value::object();
    std::string method;
    switch (verb.verb) {
    case WorkspaceIntentKind::LabelClient:
        method = "client.label";
        parameters.set("label", Value::string(verb.title));
        break;
    case WorkspaceIntentKind::BalanceActiveTab:
    case WorkspaceIntentKind::AdjustFocusedPaneRatio:
    case WorkspaceIntentKind::SetDividerRatio:
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set("tab", Value::number(verb.tabID ? verb.tabID : activeTab));
        parameters.set("expectedRevision", Value::string(std::to_string(workspace ? workspace->revision : 0)));
        if (verb.verb == WorkspaceIntentKind::BalanceActiveTab) {
            method = "tab.balance";
        } else if (verb.verb == WorkspaceIntentKind::SetDividerRatio) {
            check(divider.has_value(), "the observed divider path must be captured from the client's tree");
            if (!divider)
                return;
            method = "divider.setRatio";
            auto path = Value::array();
            for (std::size_t child = 0; child < divider->length; ++child)
                path.push(Value::number(divider->children[child]));
            parameters.set("path", std::move(path));
            parameters.set("ratio", Value::number(verb.ratio));
        } else {
            method = "pane.adjustRatio";
            parameters.set("pane", Value::number(verb.paneID ? verb.paneID : focused));
            const auto direction = static_cast<Ubiquitty::Panes::Direction>(verb.direction);
            parameters.set("direction", Value::string(direction == Ubiquitty::Panes::Direction::Left ? "left" : direction == Ubiquitty::Panes::Direction::Right ? "right"
                                                : direction == Ubiquitty::Panes::Direction::Up                                                                  ? "up"
                                                                                                                                                                : "down"));
            parameters.set("step", Value::number(verb.ratio));
        }
        break;
    case WorkspaceIntentKind::MovePaneToTab:
    case WorkspaceIntentKind::MoveTabToPane:
    case WorkspaceIntentKind::MovePaneToNewTab:
        method = verb.verb == WorkspaceIntentKind::MoveTabToPane ? "tab.moveToPane" : verb.verb == WorkspaceIntentKind::MovePaneToTab ? "pane.moveToTab"
                                                                                                                                      : "pane.moveToNewTab";
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set(verb.verb == WorkspaceIntentKind::MoveTabToPane ? "tab" : "pane", Value::number(verb.verb == WorkspaceIntentKind::MoveTabToPane ? verb.tabID : verb.paneID));
        parameters.set("expectedRevision", Value::string(std::to_string(workspace ? workspace->revision : 0)));
        if (verb.verb == WorkspaceIntentKind::MovePaneToNewTab) {
            std::uint32_t destination = 0;
            if (workspace) {
                for (const auto& window : workspace->windows) {
                    for (const auto& tab : window.tabs) {
                        if (std::ranges::any_of(tab.panes, [&](const auto& pane) { return std::to_underlying(pane.id) == verb.targetPaneID; }))
                            destination = std::to_underlying(window.id);
                    }
                }
            }
            parameters.set("targetWindow", Value::number(destination));
        } else {
            if (verb.verb == WorkspaceIntentKind::MovePaneToTab)
                parameters.set("tab", Value::number(verb.targetTabID));
            parameters.set("target", Value::number(verb.targetPaneID));
            parameters.set("orientation", Value::string(verb.orientation == std::to_underlying(Ubiquitty::Panes::Orientation::Horizontal) ? "horizontal" : "vertical"));
            parameters.set("side", Value::string(verb.side == std::to_underlying(Ubiquitty::Panes::InsertSide::First) ? "before" : "after"));
            parameters.set("insertTarget", Value::string(verb.insertTarget == std::to_underlying(Ubiquitty::Panes::InsertTarget::Root) ? "root" : "pane"));
        }
        break;
    case WorkspaceIntentKind::ExtractPaneToTab:
    case WorkspaceIntentKind::ExtractPaneToSurface:
    case WorkspaceIntentKind::ExtractTabToSurface:
        method = verb.verb == WorkspaceIntentKind::ExtractTabToSurface ? "tab.extractWindow" : verb.verb == WorkspaceIntentKind::ExtractPaneToTab ? "pane.extractTab"
                                                                                                                                                  : "pane.extractWindow";
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set(verb.verb == WorkspaceIntentKind::ExtractTabToSurface ? "tab" : "pane", Value::number(verb.verb == WorkspaceIntentKind::ExtractTabToSurface ? verb.tabID : verb.paneID));
        parameters.set("expectedRevision", Value::string(std::to_string(workspace ? workspace->revision : 0)));
        break;
    case WorkspaceIntentKind::MovePane:
    case WorkspaceIntentKind::SwapPanes:
        method = verb.verb == WorkspaceIntentKind::MovePane ? "pane.move" : "pane.swap";
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set("pane", Value::number(verb.paneID));
        parameters.set("target", Value::number(verb.targetPaneID));
        parameters.set("expectedRevision", Value::string(std::to_string(workspace ? workspace->revision : 0)));
        if (verb.verb == WorkspaceIntentKind::MovePane) {
            parameters.set("orientation", Value::string(verb.orientation == std::to_underlying(Ubiquitty::Panes::Orientation::Horizontal) ? "horizontal" : "vertical"));
            parameters.set("side", Value::string(verb.side == std::to_underlying(Ubiquitty::Panes::InsertSide::First) ? "before" : "after"));
        }
        break;
    case WorkspaceIntentKind::SetActiveSurface:
        method = "window.activate";
        parameters.set("window", Value::number(verb.surfaceID));
        break;
    case WorkspaceIntentKind::SetActiveTab:
        method = "tab.activate";
        parameters.set("tab", Value::number(verb.tabID));
        break;
    case WorkspaceIntentKind::FocusPane:
        method = "pane.focus";
        parameters.set("pane", Value::number(verb.paneID));
        break;
    case WorkspaceIntentKind::MoveFocus:
        method = "window.moveFocus";
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set("pane", Value::number(focused));
        parameters.set("direction", Value::string(verb.direction == 0 ? "left" : verb.direction == 1 ? "right"
                                            : verb.direction == 2                                    ? "up"
                                                                                                     : "down"));
        parameters.set("expectedRevision", Value::string(std::to_string(workspace ? workspace->revision : 0)));
        break;
    case WorkspaceIntentKind::ToggleZoom:
        method = "pane.zoom";
        parameters.set("pane", Value::number(verb.paneID));
        parameters.set("state", Value::string(zoomed == verb.paneID ? "unzoomed" : "zoomed"));
        parameters.set("expectedRevision", Value::string(std::to_string(workspace ? workspace->revision : 0)));
        break;
    case WorkspaceIntentKind::AddSurface:
        method = "workspace.addWindow";
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set("pane", Value::number(verb.paneID ? verb.paneID : focused));
        parameters.set("expectedRevision", Value::string(std::to_string(workspace ? workspace->revision : 0)));
        break;
    case WorkspaceIntentKind::AddTab:
        method = "workspace.addTab";
        parameters.set("window", Value::number(verb.surfaceID));
        if (verb.paneID)
            parameters.set("pane", Value::number(verb.paneID));
        break;
    case WorkspaceIntentKind::SplitFocusedPane:
        method = "workspace.split";
        parameters.set("pane", Value::number(verb.paneID ? verb.paneID : focused));
        parameters.set("orientation", Value::string(verb.orientation == std::to_underlying(Ubiquitty::Panes::Orientation::Horizontal) ? "horizontal" : "vertical"));
        parameters.set("ratio", Value::number(verb.ratio));
        break;
    case WorkspaceIntentKind::CloseTab:
        method = "workspace.closeTab";
        parameters.set("tab", Value::number(verb.tabID));
        break;
    case WorkspaceIntentKind::CloseFocusedPane:
    case WorkspaceIntentKind::ClosePane:
        method = "workspace.closePane";
        parameters.set("pane", Value::number(verb.verb == WorkspaceIntentKind::CloseFocusedPane ? focused : verb.paneID));
        break;
    case WorkspaceIntentKind::CloseSurface:
        method = "window.close";
        parameters.set("window", Value::number(verb.surfaceID));
        parameters.set("expectedRevision", Value::string(std::to_string(workspace ? workspace->revision : 0)));
        break;
    case WorkspaceIntentKind::MoveTabToSurface:
        method = "tab.move";
        parameters.set("tab", Value::number(verb.tabID));
        if (workspace) {
            for (const auto& window : workspace->windows) {
                for (const auto& tab : window.tabs) {
                    if (std::ranges::any_of(tab.panes, [&](const auto& pane) { return std::to_underlying(pane.id) == verb.targetPaneID; })) {
                        parameters.set("window", Value::number(std::to_underlying(window.id)));
                        parameters.set("index", Value::number(window.tabs.size()));
                    }
                }
            }
        }
        break;
    case WorkspaceIntentKind::RenameWorkspace:
        method = "workspace.rename";
        parameters.set("workspace", Value::number(verb.workspaceID));
        parameters.set("name", Value::string(verb.title));
        break;
    case WorkspaceIntentKind::RenameTab:
        method = "tab.rename";
        parameters.set("tab", Value::number(verb.tabID));
        parameters.set("name", Value::string(verb.title));
        break;
    case WorkspaceIntentKind::LabelSurface:
    case WorkspaceIntentKind::LabelPane:
    case WorkspaceIntentKind::LabelSession: {
        using Ubiquitty::Session::Identity::ObjectKind;
        const auto kind = verb.verb == WorkspaceIntentKind::LabelSurface ? ObjectKind::Window
            : verb.verb == WorkspaceIntentKind::LabelPane                ? ObjectKind::Pane
                                                                         : ObjectKind::Session;
        std::uint32_t target = verb.verb == WorkspaceIntentKind::LabelSurface ? verb.surfaceID : verb.paneID;
        if (verb.verb == WorkspaceIntentKind::LabelSession) {
            target = 0;
            if (workspace) {
                for (const auto& window : workspace->windows)
                    for (const auto& tab : window.tabs)
                        for (const auto& pane : tab.panes)
                            if (std::to_underlying(pane.id) == verb.paneID)
                                target = pane.sessionID;
            }
        }
        method = "label.set";
        parameters.set("object", Value::string(Ubiquitty::Session::Identity::format({ kind, target })));
        parameters.set("label", Value::string(verb.title));
        break;
    }
    default:
        check(false, "the local intent has no registered topology method");
        return;
    }
    const auto id = "layout-intent-" + std::to_string(++end.nextIntent);
    Value request = Value::object();
    request.set("jsonrpc", Value::string("2.0"));
    request.set("id", Value::string(id));
    request.set("method", Value::string(method));
    request.set("params", std::move(parameters));
    check(end.client && end.client->call(Ubiquitty::Codec::JSON::serialize(request)), "a typed layout intent was not queued");
    end.pendingIntents.emplace(id, verb);
    WorkspaceLayoutProjectionAccess::drive(daemon, index, end.client->takeUpstream(Ubiquitty::Host::ScreenPlaneAccess::now()));
    WorkspaceLayoutProjectionAccess::dispatchCalls(daemon, index);
}

/// Whether any pane of `layout` is backed by `sessionID`.
bool layoutNamesSession(const WorkspaceLayoutMessage& layout, std::uint32_t sessionID);

/// One Sync frame the daemon sent a client, in the order it was sent.
struct PlaneFrame {
    Ubiquitty::Protocol::Sync::FrameKind kind;
    std::uint64_t channel;
    std::vector<std::uint8_t> body;
};

/// The canonical frames the daemon wrote, in order, excluding local events.
///
/// - Parameter frames: What the daemon wrote.
/// - Returns: Every actual frame, with its original channel and body.
std::vector<PlaneFrame> planeFrames(const std::vector<Frame>& frames)
{
    std::vector<PlaneFrame> out;
    for (const Frame& frame : frames) {
        if (frame.type)
            out.push_back({ *frame.type, frame.channel, frame.payload });
    }
    return out;
}

/// Where, in a client's Sync frames, a pane's frames stand to the
/// structures that name it.
struct PaneOrder {
    /// The first structure of the workspace that names the session.
    std::optional<std::size_t> firstNamed;
    /// The first structure after that one that does not name it.
    std::optional<std::size_t> firstRetired;
    /// The pane's PaneBind, and its PaneUnbind.
    std::optional<std::size_t> bind;
    std::optional<std::size_t> unbind;
    /// The first and the last frame on the pane's handle.
    std::optional<std::size_t> firstFrame;
    std::optional<std::size_t> lastFrame;
};

/// Reads the order of a session's pane against its workspace's structures.
///
/// - Parameters:
///   - plane: The client's Sync frames, in order.
///   - sessionID: The session.
/// - Returns: The positions found.
PaneOrder orderOf(const std::vector<PlaneFrame>& plane, std::uint32_t sessionID, Sync::ID128 paneID)
{
    using namespace Ubiquitty::Protocol::Sync;
    PaneOrder order;
    std::optional<std::uint64_t> handle;
    for (std::size_t index = 0; index < plane.size(); ++index) {
        const PlaneFrame& frame = plane[index];
        if (frame.kind == FrameKind::WorkspaceStructure) {
            const auto head = decodeWorkspaceStructureHead(frame.body);
            const auto described = head && !head->isGone ? decodeWorkspaceLayout(head->record) : std::nullopt;
            const bool isNamed = described && layoutNamesSession(*described, sessionID); // bool-ok: one structure's answer
            if (isNamed && !order.firstNamed)
                order.firstNamed = index;
            if (!isNamed && order.firstNamed && !order.firstRetired)
                order.firstRetired = index;
        } else if (frame.kind == FrameKind::PaneBind) {
            if (const auto bind = decodePaneBind(frame.body); bind && bind->paneID == paneID) {
                order.bind = index;
                handle = bind->handle;
            }
        } else if (frame.kind == FrameKind::PaneUnbind) {
            if (const auto unbind = decodePaneUnbind(frame.body); unbind && handle && unbind->handle == *handle) {
                order.unbind = index;
                handle.reset();
            }
        } else if (handle && frame.channel == *handle) {
            if (!order.firstFrame)
                order.firstFrame = index;
            order.lastFrame = index;
        }
    }
    return order;
}

/// The newest structure among the frames, as the record the client was sent.
///
/// - Parameter frames: What the daemon wrote.
/// - Returns: The arrival, or nothing when no structure revision arrived.
std::optional<Frame> layoutArrival(const std::vector<Frame>& frames)
{
    for (auto frame = frames.rbegin(); frame != frames.rend(); ++frame) {
        if (!frame->type && !frame->intentResult)
            return *frame;
    }
    return std::nullopt;
}

/// Whether a structure revision arrived among the frames.
///
/// - Parameter frames: What the daemon wrote.
/// - Returns: True when one did.
bool carriesLayout(const std::vector<Frame>& frames) { return layoutArrival(frames).has_value(); }

bool carries(const std::vector<Frame>& frames, Sync::FrameKind type)
{
    return std::ranges::any_of(frames, [&](const Frame& frame) { return frame.type == type; });
}

std::optional<Frame> firstOfType(const std::vector<Frame>& frames, Sync::FrameKind type)
{
    for (const Frame& frame : frames)
        if (frame.type == type)
            return frame;
    // Named on the way out so a missing frame says what came instead.
    std::string saw;
    for (const Frame& frame : frames)
        saw += std::string(saw.empty() ? "" : " ") + (frame.type ? Sync::frameKindName(std::to_underlying(*frame.type)) : frame.intentResult ? "IntentResult"
                                                                                                                                             : "AssembledStructure");
    std::fprintf(stderr, "workspace_layout_projection: looking for %s, the daemon sent: [%s]\n",
        Sync::frameKindName(std::to_underlying(type)), saw.c_str());
    return std::nullopt;
}

/// Checks a receipt against the fixture's request, never the other way around.
std::optional<Sync::Welcome> admittedWelcome(const std::vector<Frame>& frames, int peerFileDescriptor)
{
    const auto found = screenEnds().find(peerFileDescriptor);
    const auto frame = firstOfType(frames, Sync::FrameKind::Welcome);
    if (found == screenEnds().end() || !found->second.request || !frame)
        return std::nullopt;
    const auto& request = *found->second.request;
    const auto welcome = Sync::decodeWelcome(frame->payload);
    if (!welcome || welcome->admission != Sync::AttachmentAdmission::Granted || welcome->selection.target != request.target
        || welcome->selection.sessionID != request.sessionID
        || (request.target == Sync::AdmissionTarget::Workspace && welcome->selection.workspaceID != request.workspaceID)
        || welcome->selection.daemonInstanceID != request.expectedDaemonInstanceID
        || welcome->selection.socketName != "screen-plane-fixture.sock")
        return std::nullopt;
    return welcome;
}

/// Finds a local completion decoded from its correlated canonical Result.
std::optional<Frame> firstIntentResult(const std::vector<Frame>& frames)
{
    for (const Frame& frame : frames)
        if (!frame.type && frame.intentResult)
            return frame;
    return std::nullopt;
}

/// Checks a canonical Result's exact error rather than inferring a refusal.
bool intentRefusedWith(const Frame& frame, Ubiquitty::Protocol::Control::ErrorCode code)
{
    bool isJSON = false; // bool-ok: the parser's output flag
    const auto answer = Ubiquitty::Codec::JSON::parse(std::string(frame.payload.begin(), frame.payload.end()), isJSON);
    const auto* error = isJSON ? answer.find("error") : nullptr;
    const auto* number = error && error->isObject() ? error->find("code") : nullptr;
    return answer.stringOr("jsonrpc") == "2.0" && !answer.find("result")
        && number && number->isNumber() && number->asNumber() == std::to_underlying(code);
}

/// Whether any pane of `layout` is backed by `sessionID`.
bool layoutNamesSession(const WorkspaceLayoutMessage& layout, std::uint32_t sessionID)
{
    for (const auto& surface : layout.surfaces)
        for (const auto& tab : surface.tabs)
            for (const auto& pane : tab.panes)
                if (pane.sessionID == sessionID)
                    return true;
    return false;
}

/// Whether the request selects a whole workspace rather than one session.
enum class WorkspaceClaim {
    Unclaimed,
    Claimed
};

enum class PeerOrigin : std::uint8_t {
    Remote,
    TrustedLocal
};

/// Two layouts that describe the same topology, field for field.
bool sameLayout(const WorkspaceLayoutMessage& a, const WorkspaceLayoutMessage& b)
{
    if (a.workspaceID != b.workspaceID || a.name != b.name || a.flags != b.flags || a.activeSurfaceID != b.activeSurfaceID
        || a.surfaces.size() != b.surfaces.size())
        return false;
    for (std::size_t s = 0; s < a.surfaces.size(); ++s) {
        const auto& x = a.surfaces[s];
        const auto& y = b.surfaces[s];
        if (x.surfaceID != y.surfaceID || x.activeTabID != y.activeTabID || x.zoomedPaneID != y.zoomedPaneID || x.tabs.size() != y.tabs.size())
            return false;
        for (std::size_t t = 0; t < x.tabs.size(); ++t) {
            const auto& c = x.tabs[t];
            const auto& d = y.tabs[t];
            if (c.tabID != d.tabID || c.focusedPaneID != d.focusedPaneID || c.layout != d.layout || c.panes.size() != d.panes.size())
                return false;
            for (std::size_t q = 0; q < c.panes.size(); ++q)
                if (c.panes[q].paneID != d.panes[q].paneID || c.panes[q].sessionID != d.panes[q].sessionID)
                    return false;
        }
    }
    return true;
}

/// One client of the daemon: its index there and the socket pair it speaks over.
struct Attached {
    std::size_t index = 0;
    std::array<int, 2> ends { -1, -1 };
};

struct Peer {
    Ubiquitty::Host::Daemon daemon;
    std::uint32_t sessionID = 0;
    std::vector<Attached> clients;
    std::size_t index = 0; // the first client's
    std::array<int, 2> ends { -1, -1 }; // the first client's

    explicit Peer(WorkspaceClaim claim, std::string workspaceName = "")
    {
        // A pane the model mints is backed by a real session, so the daemon
        // needs the shell a running one is started with.
        WorkspaceLayoutProjectionAccess::useShell(daemon, "/bin/sh");
        sessionID = WorkspaceLayoutProjectionAccess::makeSession(daemon, std::move(workspaceName));
        const Attached first = attach(sessionID, claim);
        index = first.index;
        ends = first.ends;
    }
    ~Peer()
    {
        for (const Attached& client : clients) {
            WorkspaceLayoutProjectionAccess::closeClientFileDescriptor(daemon, client.index);
            screenEnds().erase(client.ends[1]);
            ::close(client.ends[1]);
        }
    }

    /// Greets the fixture's exact existing target; its frames are left unread.
    ///
    /// `scope` is its token's ceiling and `agentLabel`, when set, makes it an
    /// agent — the two axes the gates on a workspace verb read.
    Attached attach(std::uint32_t session, WorkspaceClaim claim,
        ScopeSet scope = ScopeSet { Scope::Observe, Scope::Drive, Scope::Control, Scope::NewSession },
        const std::string& agentLabel = {}, const std::string& tokenID = "workspace-layout-projection", PeerOrigin origin = PeerOrigin::Remote)
    {
        return attachTo(claim == WorkspaceClaim::Claimed ? Sync::AdmissionTarget::Workspace : Sync::AdmissionTarget::Session,
            session, scope, agentLabel, tokenID, origin);
    }

    Attached attach(Sync::AdmissionTarget target, WorkspaceClaim claim,
        ScopeSet scope = ScopeSet { Scope::Observe, Scope::Drive, Scope::Control, Scope::NewSession },
        const std::string& agentLabel = {}, const std::string& tokenID = "workspace-layout-projection", PeerOrigin origin = PeerOrigin::Remote)
    {
        check(target == Sync::AdmissionTarget::Control && claim == WorkspaceClaim::Unclaimed, "an unbound request must explicitly select Control");
        if (target != Sync::AdmissionTarget::Control || claim != WorkspaceClaim::Unclaimed)
            return {};
        return attachTo(target, 0, scope, agentLabel, tokenID, origin);
    }

    Attached attachTo(Sync::AdmissionTarget target, std::uint32_t session, ScopeSet scope,
        const std::string& agentLabel, const std::string& tokenID, PeerOrigin origin)
    {
        Attached client;
        const auto connected = ::socketpair(AF_UNIX, SOCK_STREAM, 0, client.ends.data());
        check(connected == 0, "socketpair");
        if (connected != 0)
            return client;
        screenEnds().erase(client.ends[1]);
        // Non-blocking on the daemon's end: an attach writes SNAPSHOT, the
        // backfill and the layout before this test reads any of it, and a
        // blocking write into a full buffer waits on a reader that runs only
        // after handleFrame returns.
        ::fcntl(client.ends[0], F_SETFL, O_NONBLOCK);
        client.index = WorkspaceLayoutProjectionAccess::addClient(daemon, Ubiquitty::PAL::UnixFileDescriptor { client.ends[0] }, scope, agentLabel, tokenID);
        if (origin == PeerOrigin::TrustedLocal)
            WorkspaceLayoutProjectionAccess::makeTrustedLocal(daemon, client.index);
        auto& end = screenEnds()[client.ends[1]];
        end.request = WorkspaceLayoutProjectionAccess::requestFor(daemon, target, session, agentLabel);
        check(end.request.has_value(), "the admission request must name this fixture's actual held target");
        end.client = std::make_unique<UbiquittyTest::ScreenClient>(24, 80,
            Sync::ID128 { .low = Ubiquitty::Host::ScreenPlaneAccess::clientID(daemon, client.index), .high = 7 },
            agentLabel.empty() ? Sync::Role::Owner : Sync::Role::Agent);
        if (end.request) {
            check(end.client->start(*end.request), "the explicit canonical greeting must be valid");
            WorkspaceLayoutProjectionAccess::drive(daemon, client.index, end.client->takeUpstream(Ubiquitty::Host::ScreenPlaneAccess::now()));
        }
        clients.push_back(client);
        return client;
    }
};

} // namespace

int main()
{
    // (a) The projection. One surface, one tab, one pane, and the pane's session
    // is the one this client bound.
    {
        Peer peer(WorkspaceClaim::Claimed, "shared");
        const auto frames = readAll(peer.daemon, peer.index, peer.ends[1]);
        const auto ack = admittedWelcome(frames, peer.ends[1]);
        check(ack && ack->selection.workspaceID != 0, "the Granted Welcome must name the exact preselected workspace");

        const auto layoutFrame = layoutArrival(frames);
        check(layoutFrame.has_value(), "no structure followed a HELLO that claimed Workspace");
        if (layoutFrame) {
            const auto layout = decodeWorkspaceLayout(layoutFrame->payload);
            check(layout.has_value(), "the structure record did not decode");
            if (layout) {
                check(layout->workspaceID == peer.sessionID, "the workspace is not the session, as SESSION_LIST says it is");
                check(layout->name == "shared", "the workspace name is not the session's");
                check(layout->surfaces.size() == 1, "a session projected to other than one surface");
                if (layout->surfaces.size() == 1) {
                    const auto& surface = layout->surfaces.front();
                    check(layout->activeSurfaceID == surface.surfaceID, "the active surface is not the only one");
                    check(surface.tabs.size() == 1, "a session projected to other than one tab");
                    if (surface.tabs.size() == 1) {
                        const auto& tab = surface.tabs.front();
                        check(surface.activeTabID == tab.tabID, "the active tab is not the only one");
                        check(tab.panes.size() == 1, "a session projected to other than one pane");
                        if (tab.panes.size() == 1) {
                            check(tab.panes.front().sessionID == peer.sessionID, "the pane is bound to a session other than the one attached");
                            check(tab.focusedPaneID == tab.panes.front().paneID, "focus names a pane that is not the only one");
                            // Core's grammar: "80x24,0,0,<paneID>" for one leaf at the
                            // session's geometry. Asserted by content so a hand-written
                            // string that happened to be non-empty would not pass.
                            check(tab.layout == "80x24,0,0," + std::to_string(tab.panes.front().paneID),
                                ("the layout string is not the emitter's one-leaf form: " + tab.layout).c_str());
                        }
                    }
                }
            }
        }
    }

    // (b) A person attached to one session reads the layout it sits in, to
    // draw its pane in place; an agent attached the same way is told nothing,
    // because the layout names every pane in the workspace.
    {
        Peer peer(WorkspaceClaim::Unclaimed);
        const auto frames = readAll(peer.daemon, peer.index, peer.ends[1]);
        check(admittedWelcome(frames, peer.ends[1]).has_value(), "the exact single-session Greet was not granted");
        const auto layoutFrame = layoutArrival(frames);
        const auto layout = layoutFrame ? decodeWorkspaceLayout(layoutFrame->payload) : std::nullopt;
        check(layout && layoutNamesSession(*layout, peer.sessionID),
            "a person attached to one session was not handed the layout that session sits in");

        WorkspaceLayoutProjectionAccess::grantAgentSession(peer.daemon, peer.sessionID, "single-session-agent");
        const Attached agent = peer.attach(peer.sessionID, WorkspaceClaim::Unclaimed,
            ScopeSet { Scope::Observe, Scope::Drive }, /*agentLabel*/ "mcp", /*tokenID*/ "single-session-agent");
        const auto agentFrames = readAll(peer.daemon, agent.index, agent.ends[1]);
        const auto agentWelcome = admittedWelcome(agentFrames, agent.ends[1]);
        check(agentWelcome && agentWelcome->grantedRole == Sync::Role::Agent, "the exact granted Agent session must retain its Agent role");
        check(carries(agentFrames, Sync::FrameKind::PaneBind) && screenEnds().at(agent.ends[1]).client->pane()
                && !carries(agentFrames, Sync::FrameKind::WorkspaceStructure) && !carriesLayout(agentFrames),
            "an agent attached to one session was handed the workspace's topology");
    }

    // (c) A verb is applied to the workspace the session roots, a pane the model
    // mints gets a session behind it, and the new topology follows the result.
    {
        Peer peer(WorkspaceClaim::Claimed, "shared");
        static_cast<void>(readAll(peer.daemon, peer.index, peer.ends[1])); // the attach frames, out of the way
        const std::size_t sessionsBefore = WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon);

        WorkspaceIntent verb {};
        verb.workspaceID = peer.sessionID;
        verb.requestID = 0x5EED;
        verb.workspaceID = peer.sessionID;
        verb.verb = WorkspaceIntentKind::AddTab;
        verb.surfaceID = 1; // the fresh model's first surface
        std::vector<std::uint8_t> frame;
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], verb);

        const auto frames = readAll(peer.daemon, peer.index, peer.ends[1]);
        const auto resultFrame = firstIntentResult(frames);
        check(resultFrame.has_value(), "a canonical structure Call was dropped rather than answered");
        std::uint32_t mintedSession = 0;
        std::uint32_t mintedTab = 0;
        std::uint32_t rootPane = 0;
        if (resultFrame) {
            const auto result = resultFrame->intentResult;
            check(result.has_value(), "the canonical Result did not produce a local completion");
            if (result) {
                check(result->requestID == 0x5EED, "the result does not echo the request it answers");
                check(result->wasPerformed, "AddTab on a hosted workspace was refused");
                check(result->tabID != 0, "a performed AddTab minted no tab id");
                check(result->paneID != 0, "a performed AddTab minted no pane id");
                check(result->sessionID != 0 && result->sessionID != peer.sessionID,
                    "the new tab's pane was not backed by a session of its own");
                mintedSession = result->sessionID;
                mintedTab = result->tabID;
            }
        }
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == sessionsBefore + 1,
            "a minted pane did not mint exactly one session");

        // The topology follows the result, whole, and names the new binding.
        const auto layoutFrame = layoutArrival(frames);
        check(layoutFrame.has_value(), "no structure followed a performed verb");
        if (layoutFrame) {
            const auto layout = decodeWorkspaceLayout(layoutFrame->payload);
            check(layout && layout->surfaces.size() == 1 && layout->surfaces[0].tabs.size() == 2,
                "the re-emitted layout does not show the second tab");
            if (layout && layout->surfaces.size() == 1 && layout->surfaces[0].tabs.size() == 2) {
                const auto& added = layout->surfaces[0].tabs[1];
                check(added.panes.size() == 1 && added.panes[0].sessionID == mintedSession,
                    "the new tab's pane is not bound to the session the result named");
                check(layout->surfaces[0].tabs[0].panes[0].sessionID == peer.sessionID,
                    "the root pane lost its binding to the root session");
                rootPane = layout->surfaces[0].tabs[0].panes[0].paneID;
            }
        }

        // Renaming an unknown tab is refused without changing the layout.
        WorkspaceIntent rename {};
        rename.workspaceID = peer.sessionID;
        rename.requestID = 0x5EEE;
        rename.verb = WorkspaceIntentKind::RenameTab;
        rename.surfaceID = 1;
        rename.tabID = 0xFFFFFFFF;
        rename.title = "renamed";
        frame.clear();
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], rename);
        const auto after = readAll(peer.daemon, peer.index, peer.ends[1]);
        const auto refused = firstIntentResult(after);
        check(refused.has_value(), "RenameTab was dropped rather than answered");
        if (refused) {
            const auto result = refused->intentResult;
            check(result && result->requestID == 0x5EEE && !result->wasPerformed,
                "RenameTab was reported performed for an unknown tab");
        }
        check(!carriesLayout(after), "a refused verb was followed by a layout as though it had changed something");

        // Closing the tab ends the session behind its pane, and the layout that
        // follows names neither.
        WorkspaceIntent close {};
        close.workspaceID = peer.sessionID;
        close.requestID = 0x5EEF;
        close.verb = WorkspaceIntentKind::CloseTab;
        close.surfaceID = 1;
        close.tabID = mintedTab;
        frame.clear();
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], close);
        const auto closed = readAll(peer.daemon, peer.index, peer.ends[1]);
        if (const auto closeResult = firstIntentResult(closed)) {
            const auto result = closeResult->intentResult;
            check(result && result->wasPerformed, "CloseTab on the minted tab was refused");
        } else {
            check(false, "CloseTab was dropped rather than answered");
        }
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == sessionsBefore,
            "the closed tab's session outlived its pane");
        if (const auto closedLayout = layoutArrival(closed)) {
            const auto layout = decodeWorkspaceLayout(closedLayout->payload);
            check(layout && layout->surfaces.size() == 1 && layout->surfaces[0].tabs.size() == 1,
                "the closed tab is still described");
        } else {
            check(false, "no structure followed the close");
        }

        // A split mints a backed pane; an unknown pane close is refused.
        WorkspaceIntent split {};
        split.workspaceID = peer.sessionID;
        split.requestID = 0x5EF0;
        split.verb = WorkspaceIntentKind::SplitFocusedPane;
        split.surfaceID = 1;
        split.orientation = static_cast<decltype(split.orientation)>(Ubiquitty::Panes::Orientation::Vertical);
        split.ratio = 0.5f;
        frame.clear();
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], split);
        const auto splitFrames = readAll(peer.daemon, peer.index, peer.ends[1]);
        std::uint32_t splitSession = 0;
        if (const auto splitResult = firstIntentResult(splitFrames)) {
            const auto result = splitResult->intentResult;
            check(result && result->wasPerformed && result->paneID != 0 && result->sessionID != 0,
                "SplitFocusedPane did not mint a backed pane");
            if (result)
                splitSession = result->sessionID;
        } else {
            check(false, "SplitFocusedPane was dropped rather than answered");
        }
        WorkspaceIntent closeRoot {};
        closeRoot.workspaceID = peer.sessionID;
        closeRoot.requestID = 0x5EF1;
        closeRoot.verb = WorkspaceIntentKind::ClosePane;
        closeRoot.surfaceID = 1;
        closeRoot.paneID = 0xFFFFFFFF;
        frame.clear();
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], closeRoot);
        const auto rootFrames = readAll(peer.daemon, peer.index, peer.ends[1]);
        if (const auto rootResult = firstIntentResult(rootFrames)) {
            const auto result = rootResult->intentResult;
            check(result && !result->wasPerformed, "an unknown pane was reported closed");
        } else {
            check(false, "ClosePane on an unknown pane was dropped rather than answered");
        }
        check(!carriesLayout(rootFrames), "an unknown pane close re-emitted the layout");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == sessionsBefore + 1,
            "an unknown pane close ended a session");

        // The session behind a pane ending takes the pane out of the tree, and
        // the root's clients see it go.
        WorkspaceLayoutProjectionAccess::destroySession(peer.daemon, splitSession);
        const auto gone = readAll(peer.daemon, peer.index, peer.ends[1]);
        if (const auto goneLayout = layoutArrival(gone)) {
            const auto layout = decodeWorkspaceLayout(goneLayout->payload);
            const bool onlyRoot = layout && layout->surfaces.size() == 1 && layout->surfaces[0].tabs.size() == 1
                && layout->surfaces[0].tabs[0].panes.size() == 1 && layout->surfaces[0].tabs[0].panes[0].paneID == rootPane;
            check(onlyRoot, "the pane whose session ended is still described");
        } else {
            check(false, "no structure followed a pane's session ending");
        }
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == sessionsBefore,
            "an ended pane session was counted twice or not at all");
    }

    // Workspace identity survives closing the initial tab, and the final tab
    // closes the record only after its requester receives the accepted result.
    {
        Peer peer(WorkspaceClaim::Claimed, "durable-workspace");
        const auto initialFrames = readAll(peer.daemon, peer.index, peer.ends[1]);
        const auto initialFrame = layoutArrival(initialFrames);
        const auto initial = initialFrame ? decodeWorkspaceLayout(initialFrame->payload) : std::nullopt;
        if (!initial || initial->surfaces.empty())
            return 1;
        const auto identity = initial->workspaceID;
        const auto surfaceID = initial->surfaces[0].surfaceID;
        const auto initialTab = initial->surfaces[0].tabs[0].tabID;
        WorkspaceIntent add {};
        add.workspaceID = peer.sessionID;
        add.requestID = 901;
        add.verb = WorkspaceIntentKind::AddTab;
        add.surfaceID = surfaceID;
        std::vector<std::uint8_t> frame;
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], add);
        const auto addedFrames = readAll(peer.daemon, peer.index, peer.ends[1]);
        const auto addedFrame = firstIntentResult(addedFrames);
        const auto added = addedFrame ? addedFrame->intentResult : std::nullopt;
        if (!added || !added->wasPerformed)
            return 1;
        auto sibling = peer.attach(added->sessionID, WorkspaceClaim::Claimed);
        static_cast<void>(readAll(peer.daemon, sibling.index, sibling.ends[1]));
        for (const auto kind : { WorkspaceIntentKind::RenameWorkspace, WorkspaceIntentKind::RenameTab }) {
            WorkspaceIntent rename {};
            rename.workspaceID = identity;
            rename.surfaceID = surfaceID;
            rename.tabID = added->tabID;
            rename.requestID = 905;
            rename.verb = kind;
            rename.title = kind == WorkspaceIntentKind::RenameWorkspace ? "renamed-workspace" : "Build — 日本語";
            frame.clear();
            driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], rename);
            for (const auto& receiver : { peer.clients[0], sibling }) {
                const auto updatedFrames = readAll(peer.daemon, receiver.index, receiver.ends[1]);
                const auto updatedFrame = layoutArrival(updatedFrames);
                const auto updated = updatedFrame ? decodeWorkspaceLayout(updatedFrame->payload) : std::nullopt;
                check(updated && updated->workspaceID == identity && updated->name == "renamed-workspace",
                    "workspace rename did not reach both attached clients");
                if (kind == WorkspaceIntentKind::RenameTab) {
                    check(updated && updated->surfaces[0].tabs[1].title == "Build — 日本語",
                        "tab rename did not reach both attached clients");
                }
            }
        }
        // A workspace's name has one length, and a rename past it is refused
        // where it is given: nothing is renamed and no client is sent a name.
        // The name the rest of this case knows the workspace by is given
        // back last.
        for (const std::size_t length : { Ubiquitty::Protocol::Sync::maxWorkspaceNameLength + 1, Ubiquitty::Protocol::Sync::maxWorkspaceNameLength, std::size_t { 0 } }) {
            WorkspaceIntent rename {};
            rename.workspaceID = identity;
            rename.surfaceID = surfaceID;
            rename.requestID = 906;
            rename.verb = WorkspaceIntentKind::RenameWorkspace;
            rename.title = length == 0 ? std::string("renamed-workspace") : std::string(length, 'n');
            frame.clear();
            driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], rename);
            const auto renameFrames = readAll(peer.daemon, peer.index, peer.ends[1]);
            const auto resultFrame = firstIntentResult(renameFrames);
            const auto result = resultFrame ? resultFrame->intentResult : std::nullopt;
            const bool isWithin = length <= Ubiquitty::Protocol::Sync::maxWorkspaceNameLength;
            check(result && result->wasPerformed == isWithin, isWithin ? "a workspace name at its length was refused" : "a workspace name past its length was taken");
            const auto layoutFrame = layoutArrival(renameFrames);
            const auto layout = layoutFrame ? decodeWorkspaceLayout(layoutFrame->payload) : std::nullopt;
            check(isWithin ? (layout && layout->name == rename.title) : !layout, isWithin ? "a workspace name at its length did not reach its client whole" : "a refused rename sent a layout");
            static_cast<void>(readAll(peer.daemon, sibling.index, sibling.ends[1]));
        }
        WorkspaceIntent close {};
        close.workspaceID = peer.sessionID;
        close.requestID = 902;
        close.verb = WorkspaceIntentKind::CloseTab;
        close.surfaceID = surfaceID;
        close.tabID = initialTab;
        frame.clear();
        driveLayoutIntent(peer.daemon, sibling.index, sibling.ends[1], close);
        sibling.index = WorkspaceLayoutProjectionAccess::clientForSession(peer.daemon, added->sessionID);
        const auto closedFrames = readAll(peer.daemon, sibling.index, sibling.ends[1]);
        const auto resultFrame = firstIntentResult(closedFrames);
        const auto result = resultFrame ? resultFrame->intentResult : std::nullopt;
        check(result && result->wasPerformed, "initial tab close was refused or lost its result");
        check(!carries(closedFrames, Sync::FrameKind::Bye), "closing initial tab ended a sibling client");
        const auto layoutFrame = layoutArrival(closedFrames);
        const auto layout = layoutFrame ? decodeWorkspaceLayout(layoutFrame->payload) : std::nullopt;
        check(layout && layout->workspaceID == identity && layout->name == "renamed-workspace",
            "initial tab closure changed workspace identity or name");
        check(layout && layout->surfaces[0].tabs.size() == 1 && layout->surfaces[0].tabs[0].tabID == added->tabID,
            "initial tab closure removed the wrong tab");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == 1, "initial tab close orphaned or killed sibling sessions");
        const auto reseededSession = WorkspaceLayoutProjectionAccess::allocateFrom(peer.daemon, identity);
        check(reseededSession != 0 && reseededSession != identity && reseededSession != added->sessionID,
            "session allocation reused a retained workspace or live session identity");
        check(WorkspaceLayoutProjectionAccess::namedSession(peer.daemon, "renamed-workspace") == added->sessionID,
            "named attachment no longer resolves the surviving workspace");
        const auto listed = readDirectory(peer.daemon, sibling.index, sibling.ends[1], 903);
        check(listed && listed->size() == 1 && (*listed)[0].workspaceID == identity
                && (*listed)[0].workspaceName == "renamed-workspace",
            "directory lost the workspace's stable identity");
        const auto reattached = peer.attach(added->sessionID, WorkspaceClaim::Claimed);
        const auto reattachedFrames = readAll(peer.daemon, reattached.index, reattached.ends[1]);
        const auto reattachedFrame = layoutArrival(reattachedFrames);
        const auto reattachedLayout = reattachedFrame ? decodeWorkspaceLayout(reattachedFrame->payload) : std::nullopt;
        check(reattachedLayout && reattachedLayout->workspaceID == identity && reattachedLayout->name == "renamed-workspace",
            "reattachment fabricated a workspace after the initial tab ended");
        check(reattachedLayout && reattachedLayout->surfaces[0].tabs[0].title == "Build — 日本語",
            "reattachment lost the authoritative custom tab title");
        // The accepted result must survive a full socket and the queued
        // frames already ahead of it when its session is destroyed.
        const auto finalPane = WorkspaceLayoutProjectionAccess::paneIdentity(peer.daemon, added->sessionID);
        const auto* finalConnection = Ubiquitty::Host::ScreenPlaneAccess::connection(peer.daemon, sibling.index);
        std::optional<std::uint32_t> finalHandle;
        if (finalPane && finalConnection)
            for (const auto& [handle, paneID] : finalConnection->boundPanes())
                if (paneID == *finalPane)
                    finalHandle = handle;
        check(finalHandle.has_value(), "the final close must name an actual pane bound on this physical connection");
        if (!finalHandle)
            return 1;
        check(WorkspaceLayoutProjectionAccess::queueBackpressuredOutput(peer.daemon, sibling.index), "premise: the sibling's output is backed up");
        close.requestID = 904;
        close.tabID = added->tabID;
        frame.clear();
        driveLayoutIntent(peer.daemon, sibling.index, sibling.ends[1], close);
        const auto finalFrames = readAll(peer.daemon, sibling.index, sibling.ends[1]);
        const auto finalResultFrame = firstIntentResult(finalFrames);
        const auto finalResult = finalResultFrame ? finalResultFrame->intentResult : std::nullopt;
        check(finalResult && finalResult->wasPerformed, "final tab close discarded its acceptance");
        const auto goodbye = firstOfType(finalFrames, Sync::FrameKind::Bye);
        const auto decodedBye = goodbye ? Sync::decodeBye(goodbye->payload) : std::nullopt;
        check(decodedBye && decodedBye->reason == Sync::ByeReason::Normal, "final tab close omitted its normal last-pane Bye");
        const auto unbound = firstOfType(finalFrames, Sync::FrameKind::PaneUnbind);
        const auto decodedUnbind = unbound ? Sync::decodePaneUnbind(unbound->payload) : std::nullopt;
        check(decodedUnbind && decodedUnbind->handle == *finalHandle, "final tab close omitted the exact physical pane's unbind");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == 0, "final tab retained a session");
        check(WorkspaceLayoutProjectionAccess::workspaceCount(peer.daemon) == 0, "final tab retained its workspace");
    }

    // (d) A pane that cannot be backed is not minted: with no shell to start
    // the verb is refused, and the tab it would have added is gone from the model.
    {
        Peer peer(WorkspaceClaim::Claimed, "shared");
        static_cast<void>(readAll(peer.daemon, peer.index, peer.ends[1]));
        const std::size_t sessionsBefore = WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon);
        WorkspaceLayoutProjectionAccess::useShell(peer.daemon, "");

        WorkspaceIntent verb {};
        verb.workspaceID = peer.sessionID;
        verb.requestID = 0x5EF2;
        verb.verb = WorkspaceIntentKind::AddTab;
        verb.surfaceID = 1;
        std::vector<std::uint8_t> frame;
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], verb);
        const auto refused = readAll(peer.daemon, peer.index, peer.ends[1]);
        if (const auto resultFrame = firstIntentResult(refused)) {
            const auto result = resultFrame->intentResult;
            check(result && !result->wasPerformed && result->tabID == 0 && result->sessionID == 0,
                "AddTab under a shell that cannot start was reported performed");
        } else {
            check(false, "AddTab under a shell that cannot start was dropped rather than answered");
        }
        check(!carriesLayout(refused), "a refused AddTab re-emitted the layout");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == sessionsBefore, "a refused AddTab left a session behind");

        // With a shell again the same verb is performed, onto a model that
        // never held the refused tab.
        WorkspaceLayoutProjectionAccess::useShell(peer.daemon, "/bin/sh");
        verb.requestID = 0x5EF3;
        frame.clear();
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], verb);
        const auto performed = readAll(peer.daemon, peer.index, peer.ends[1]);
        if (const auto layoutFrame = layoutArrival(performed)) {
            const auto layout = decodeWorkspaceLayout(layoutFrame->payload);
            check(layout && layout->surfaces.size() == 1 && layout->surfaces[0].tabs.size() == 2,
                "the refused tab stayed in the model");
        } else {
            check(false, "AddTab with a shell again was not performed");
        }
    }

    // (e) Every verb the wire names, applied by the host as the model applies it.
    // A fresh Layout::Workspace mints the same ids in the same order as the
    // host's, so a shadow taking the same verbs predicts every id the host
    // answers with and every layout it re-emits.
    {
        using namespace Ubiquitty::Session::Layout;
        using Ubiquitty::Panes::Direction;
        using Ubiquitty::Panes::InsertSide;
        using Ubiquitty::Panes::Orientation;

        Peer peer(WorkspaceClaim::Claimed, "shared");
        static_cast<void>(readAll(peer.daemon, peer.index, peer.ends[1]));
        auto shadow = std::make_unique<Workspace>();
        const SurfaceID first = shadow->surfaceIDAt(0);
        const TabID firstTab = shadow->activeTab(first);
        const PaneID root = shadow->focusedPane(first);
        // The session behind each pane: the root's is this client's, a minted
        // one's is what the result named.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> bindings { { std::to_underlying(root), peer.sessionID } };
        const auto sessionForPane = [](void* context, PaneID pane) -> std::uint32_t {
            for (const auto& [bound, session] : *static_cast<const decltype(bindings)*>(context))
                if (bound == std::to_underlying(pane))
                    return session;
            return 0;
        };
        // Every host session is the root's 80x24, so every leaf's extents are.
        const auto extents = [](void*, Ubiquitty::Protocol::Tmux::PaneID) -> Ubiquitty::Protocol::Tmux::PaneDimensions { return { 80, 24 }; };
        const auto expected = [&] {
            return describeWorkspace(*shadow, { peer.sessionID, "shared", 0 }, sessionForPane, &bindings, extents, nullptr);
        };

        std::uint32_t nextRequest = 0x1000;
        // Sends `verb`, which the shadow has already taken with outcome
        // `didModelPerform`, and holds the host to the same outcome and the
        // same topology after it.
        const auto exchange = [&](const char* name, WorkspaceIntent verb, bool didModelPerform) {
            WorkspaceIntentResult answer {};
            const std::string label(name);
            check(didModelPerform, (label + ": the model refused it, so this step proves nothing about the host").c_str());
            verb.requestID = ++nextRequest;
            verb.workspaceID = peer.sessionID;
            std::vector<std::uint8_t> frame;
            const auto divider = verb.verb == WorkspaceIntentKind::SetDividerRatio
                ? Ubiquitty::Panes::dividerPath(*shadow->activePaneTree(SurfaceID { verb.surfaceID }), verb.splitIndex)
                : std::nullopt;
            driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], verb, divider);
            const auto frames = readAll(peer.daemon, peer.index, peer.ends[1]);
            const auto resultFrame = firstIntentResult(frames);
            check(resultFrame.has_value(), (label + ": dropped rather than answered").c_str());
            if (!resultFrame)
                return answer;
            const auto result = resultFrame->intentResult;
            check(result && result->requestID == verb.requestID, (label + ": the result does not answer the request").c_str());
            if (!result)
                return answer;
            answer = *result;
            check(answer.wasPerformed == didModelPerform, (label + ": the host's outcome is not the model's").c_str());
            if (answer.paneID != 0) {
                check(answer.sessionID != 0, (label + ": a minted pane has no session").c_str());
                bindings.emplace_back(answer.paneID, answer.sessionID);
            }
            const auto layoutFrame = layoutArrival(frames);
            check(layoutFrame.has_value() == didModelPerform, (label + ": a layout followed exactly when nothing changed").c_str());
            if (layoutFrame) {
                const auto layout = decodeWorkspaceLayout(layoutFrame->payload);
                check(layout && sameLayout(*layout, expected()), (label + ": the host's topology is not the model's").c_str());
            }
            return answer;
        };
        const auto verbFor = [](WorkspaceIntentKind kind, SurfaceID surface) {
            WorkspaceIntent verb {};
            verb.verb = kind;
            verb.surfaceID = std::to_underlying(surface);
            return verb;
        };
        const auto paneOf = [](PaneID pane) { return std::to_underlying(pane); };
        const auto tabOf = [](TabID tab) { return std::to_underlying(tab); };

        // Surfaces and tabs.
        const SurfaceID second = shadow->addSurface();
        auto answer = exchange("AddSurface", verbFor(WorkspaceIntentKind::AddSurface, first), second != invalidSurface);
        check(answer.surfaceID == std::to_underlying(second) && answer.tabID == tabOf(shadow->activeTab(second))
                && answer.paneID == paneOf(shadow->focusedPane(second)),
            "AddSurface: the host minted other ids than the model");
        exchange("SetActiveSurface", verbFor(WorkspaceIntentKind::SetActiveSurface, first), shadow->setActiveSurface(first));
        const TabID secondTab = shadow->addTab(first);
        answer = exchange("AddTab", verbFor(WorkspaceIntentKind::AddTab, first), secondTab != invalidTab);
        check(answer.tabID == tabOf(secondTab) && answer.paneID == paneOf(shadow->focusedPane(first)),
            "AddTab: the host minted other ids than the model");
        {
            auto verb = verbFor(WorkspaceIntentKind::SetActiveTab, first);
            verb.tabID = tabOf(firstTab);
            exchange("SetActiveTab", verb, shadow->setActiveTab(first, firstTab));
        }

        // Splits, focus and ratios in the first tab.
        const auto split = [&](const char* name, Orientation orientation, float ratio) {
            auto verb = verbFor(WorkspaceIntentKind::SplitFocusedPane, first);
            verb.orientation = static_cast<std::uint8_t>(orientation);
            verb.ratio = ratio;
            const PaneID minted = shadow->splitFocusedPane(first, orientation, ratio);
            const auto reply = exchange(name, verb, minted != invalidPane);
            check(reply.paneID == paneOf(minted), "SplitFocusedPane: the host minted another pane than the model");
            return minted;
        };
        const PaneID beside = split("SplitFocusedPane (vertical)", Orientation::Vertical, 0.5f);
        const PaneID below = split("SplitFocusedPane (horizontal)", Orientation::Horizontal, 0.4f);
        {
            auto verb = verbFor(WorkspaceIntentKind::FocusPane, first);
            verb.paneID = paneOf(root);
            exchange("FocusPane", verb, shadow->focusPane(first, root));
            verb = verbFor(WorkspaceIntentKind::MoveFocus, first);
            verb.direction = static_cast<std::uint8_t>(Direction::Right);
            exchange("MoveFocus", verb, shadow->moveFocus(first, Direction::Right));
            verb = verbFor(WorkspaceIntentKind::SetDividerRatio, first);
            verb.splitIndex = static_cast<std::uint16_t>(shadow->activePaneTree(first)->root());
            verb.ratio = 0.3f;
            exchange("SetDividerRatio", verb, shadow->setDividerRatio(first, shadow->activePaneTree(first)->root(), 0.3f));
            verb = verbFor(WorkspaceIntentKind::AdjustFocusedPaneRatio, first);
            verb.direction = static_cast<std::uint8_t>(Direction::Left);
            verb.ratio = 0.1f;
            exchange("AdjustFocusedPaneRatio", verb, shadow->adjustFocusedPaneRatio(first, Direction::Left, 0.1f));
            exchange("BalanceActiveTab", verbFor(WorkspaceIntentKind::BalanceActiveTab, first), shadow->balanceActiveTab(first));
            verb = verbFor(WorkspaceIntentKind::ToggleZoom, first);
            verb.paneID = paneOf(below);
            exchange("ToggleZoom", verb, shadow->toggleZoom(first, below));
        }

        // Panes moved within the tab, between tabs, out to their own tab and surface.
        {
            auto verb = verbFor(WorkspaceIntentKind::SwapPanes, first);
            verb.paneID = paneOf(root);
            verb.targetPaneID = paneOf(below);
            exchange("SwapPanes", verb, shadow->swapPanes(first, root, below));
            verb = verbFor(WorkspaceIntentKind::MovePane, first);
            verb.paneID = paneOf(below);
            verb.targetPaneID = paneOf(root);
            verb.orientation = static_cast<std::uint8_t>(Orientation::Vertical);
            verb.side = static_cast<std::uint8_t>(InsertSide::First);
            exchange("MovePane", verb, shadow->movePane(first, below, root, Orientation::Vertical, InsertSide::First));
        }
        const PaneID fourth = split("SplitFocusedPane (a fourth pane)", Orientation::Vertical, 0.5f);
        const std::size_t sessionsWithFour = WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon);
        {
            auto verb = verbFor(WorkspaceIntentKind::ClosePane, first);
            verb.paneID = paneOf(fourth);
            exchange("ClosePane", verb, shadow->closePane(first, fourth));
            check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == sessionsWithFour - 1,
                "ClosePane: the closed pane's session did not end");
            verb = verbFor(WorkspaceIntentKind::ExtractPaneToSurface, first);
            verb.paneID = paneOf(below);
            const SurfaceID third = shadow->extractPaneToSurface(first, below);
            const auto reply = exchange("ExtractPaneToSurface", verb, third != invalidSurface);
            check(reply.surfaceID == std::to_underlying(third) && reply.paneID == 0, "ExtractPaneToSurface: the host minted what the model did not");
            verb = verbFor(WorkspaceIntentKind::MovePaneToTab, first);
            verb.paneID = paneOf(beside);
            verb.targetTabID = tabOf(secondTab);
            const PaneID secondTabRoot = static_cast<PaneID>(shadow->paneTree(first, secondTab)->focusedPaneID());
            verb.targetPaneID = paneOf(secondTabRoot);
            verb.orientation = static_cast<std::uint8_t>(Orientation::Horizontal);
            verb.side = static_cast<std::uint8_t>(InsertSide::Second);
            exchange("MovePaneToTab", verb, shadow->movePaneToTab(first, beside, secondTab, secondTabRoot, Orientation::Horizontal, InsertSide::Second));
            verb = verbFor(WorkspaceIntentKind::SetActiveTab, first);
            verb.tabID = tabOf(secondTab);
            exchange("SetActiveTab (the second tab)", verb, shadow->setActiveTab(first, secondTab));
            verb = verbFor(WorkspaceIntentKind::ExtractPaneToTab, first);
            verb.paneID = paneOf(beside);
            const TabID thirdTab = shadow->extractPaneToTab(first, beside);
            const auto extracted = exchange("ExtractPaneToTab", verb, thirdTab != invalidTab);
            check(extracted.tabID == tabOf(thirdTab) && extracted.paneID == 0, "ExtractPaneToTab: the host minted what the model did not");
        }

        // Transfers across surfaces preserve the binding set and the authoritative topology.
        {
            const auto count = WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon);
            const auto movingTab = shadow->tabHolding(first, beside);
            auto verb = verbFor(WorkspaceIntentKind::MoveTabToSurface, first);
            verb.tabID = tabOf(movingTab);
            verb.targetPaneID = paneOf(shadow->focusedPane(second));
            exchange("MoveTabToSurface", verb, shadow->moveTabToSurface(first, movingTab, second));
            verb = verbFor(WorkspaceIntentKind::ExtractTabToSurface, second);
            verb.tabID = tabOf(movingTab);
            const auto detached = shadow->extractTabToSurface(second, movingTab);
            exchange("ExtractTabToSurface", verb, detached != invalidSurface);
            verb = verbFor(WorkspaceIntentKind::MoveTabToPane, detached);
            verb.tabID = tabOf(movingTab);
            verb.targetPaneID = paneOf(root);
            verb.orientation = static_cast<std::uint8_t>(Orientation::Horizontal);
            verb.side = static_cast<std::uint8_t>(InsertSide::First);
            exchange("MoveTabToPane", verb, shadow->moveTabToPane(detached, movingTab, root, Orientation::Horizontal, InsertSide::First));
            verb = verbFor(WorkspaceIntentKind::MovePaneToNewTab, first);
            verb.paneID = paneOf(beside);
            verb.targetPaneID = paneOf(shadow->focusedPane(second));
            exchange("MovePaneToNewTab", verb, shadow->movePaneToNewTab(first, beside, second));
            verb = verbFor(WorkspaceIntentKind::MovePaneToTab, second);
            verb.paneID = paneOf(beside);
            verb.targetPaneID = paneOf(root);
            verb.targetTabID = tabOf(firstTab);
            exchange("MovePaneToTab across surfaces", verb, shadow->movePaneToTab(second, beside, firstTab, root, Orientation::Vertical, InsertSide::First));
            check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == count, "transfer ended or minted a session");
            // Restore a separate tab for the close scenarios.
            verb = verbFor(WorkspaceIntentKind::ExtractPaneToTab, first);
            verb.paneID = paneOf(beside);
            exchange("ExtractPaneToTab after transfer", verb, shadow->extractPaneToTab(first, beside) != invalidTab);
        }

        // Closes: a focused pane, a tab, a surface; each ends the sessions behind it.
        {
            const std::size_t before = WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon);
            exchange("CloseFocusedPane", verbFor(WorkspaceIntentKind::CloseFocusedPane, first), shadow->closeFocusedPane(first));
            check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == before - 1, "CloseFocusedPane: the pane's session did not end");
            TabID other = invalidTab;
            for (int index = 0; index < shadow->tabCount(first); ++index)
                if (shadow->tabAt(first, index) != firstTab)
                    other = shadow->tabAt(first, index);
            auto verb = verbFor(WorkspaceIntentKind::CloseTab, first);
            verb.tabID = tabOf(other);
            exchange("CloseTab", verb, shadow->closeTab(first, other));
            check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == before - 2, "CloseTab: the tab's session did not end");
            SurfaceID holdingBelow = invalidSurface;
            for (int index = 0; index < shadow->surfaceCount(); ++index)
                if (shadow->surfaceIDAt(index) != first && shadow->surfaceIDAt(index) != second)
                    holdingBelow = shadow->surfaceIDAt(index);
            exchange("CloseSurface", verbFor(WorkspaceIntentKind::CloseSurface, holdingBelow), shadow->closeSurface(holdingBelow));
            check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == before - 3, "CloseSurface: the surface's session did not end");
        }
        // What is left: the root's pane and the second surface's, each with its session.
        check(shadow->surfaceCount() == 2 && shadow->tabCount(first) == 1 && shadow->activePaneTree(first)->paneCount() == 1,
            "the sequence did not end on the two panes it was built to");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == 2, "sessions outlive the panes the sequence closed");
    }

    // (f) A client attaching later sees the tree the verb-driving one built,
    // from the root session or from a pane's; a verb from either reaches all.
    {
        Peer peer(WorkspaceClaim::Claimed, "shared");
        static_cast<void>(readAll(peer.daemon, peer.index, peer.ends[1]));
        WorkspaceIntent verb {};
        verb.workspaceID = peer.sessionID;
        verb.requestID = 0x5F00;
        verb.verb = WorkspaceIntentKind::AddTab;
        verb.surfaceID = 1;
        std::vector<std::uint8_t> frame;
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], verb);
        const auto built = readAll(peer.daemon, peer.index, peer.ends[1]);
        std::uint32_t paneSession = 0;
        if (const auto resultFrame = firstIntentResult(built)) {
            const auto result = resultFrame->intentResult;
            if (result)
                paneSession = result->sessionID;
        }
        check(paneSession != 0, "the tab that seeds this case was not minted");
        const auto builtLayoutFrame = layoutArrival(built);
        const auto builtLayout = builtLayoutFrame ? decodeWorkspaceLayout(builtLayoutFrame->payload) : std::nullopt;
        check(builtLayout.has_value(), "the tree this case is about was not described");
        if (!builtLayout || paneSession == 0)
            return 1;

        // Fresh to the root: the built tree, not the one-pane projection.
        const Attached late = peer.attach(peer.sessionID, WorkspaceClaim::Claimed);
        const auto lateFrames = readAll(peer.daemon, late.index, late.ends[1]);
        const auto lateLayoutFrame = layoutArrival(lateFrames);
        const auto lateLayout = lateLayoutFrame ? decodeWorkspaceLayout(lateLayoutFrame->payload) : std::nullopt;
        check(lateLayout && sameLayout(*lateLayout, *builtLayout), "a client attaching to the root later does not see the tree that was built");

        // To a pane's session: the workspace it belongs to, rooted where it is,
        // never a second workspace rooted on the pane.
        const Attached onPane = peer.attach(paneSession, WorkspaceClaim::Claimed);
        const auto onPaneFrames = readAll(peer.daemon, onPane.index, onPane.ends[1]);
        const auto onPaneLayoutFrame = layoutArrival(onPaneFrames);
        const auto onPaneLayout = onPaneLayoutFrame ? decodeWorkspaceLayout(onPaneLayoutFrame->payload) : std::nullopt;
        check(onPaneLayout && onPaneLayout->workspaceID == peer.sessionID && sameLayout(*onPaneLayout, *builtLayout),
            "a client attaching to a pane's session is not told the workspace that pane is in");

        // A verb from the pane-attached client applies to that same workspace
        // and its outcome reaches all three clients.
        WorkspaceIntent split {};
        split.workspaceID = peer.sessionID;
        split.requestID = 0x5F01;
        split.verb = WorkspaceIntentKind::SplitFocusedPane;
        split.surfaceID = 1;
        split.orientation = static_cast<std::uint8_t>(Ubiquitty::Panes::Orientation::Vertical);
        split.ratio = 0.5f;
        frame.clear();
        driveLayoutIntent(peer.daemon, onPane.index, onPane.ends[1], split);
        std::optional<WorkspaceLayoutMessage> seenByAsker;
        for (const Attached& client : peer.clients) {
            const auto frames = readAll(peer.daemon, client.index, client.ends[1]);
            const auto layoutFrame = layoutArrival(frames);
            const auto layout = layoutFrame ? decodeWorkspaceLayout(layoutFrame->payload) : std::nullopt;
            check(layout.has_value(), "a client of the workspace was not told the topology a verb changed");
            if (!layout)
                continue;
            if (client.index == onPane.index)
                seenByAsker = layout;
            check(layout->surfaces.size() == 1 && layout->surfaces[0].tabs.size() == 2 && layout->surfaces[0].tabs[1].panes.size() == 2,
                "the topology a client was told is not the one the verb made");
        }
        check(seenByAsker.has_value(), "the asker itself was not told the topology it changed");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == 3, "the split's pane did not get a session of its own");
    }

    // (g) An inner mux's layout is opaque output. A tmux running in a pane
    // emits its own `%layout-change` with its own pane ids; the hosted tree
    // is written by verbs and session lifetimes only, so a client attaching
    // afterwards is told the same tree with the host's ids, none of tmux's.
    {
        Peer peer(WorkspaceClaim::Claimed, "shared");
        const auto before = readAll(peer.daemon, peer.index, peer.ends[1]);
        const auto beforeFrame = layoutArrival(before);
        const auto beforeLayout = beforeFrame ? decodeWorkspaceLayout(beforeFrame->payload) : std::nullopt;
        check(beforeLayout.has_value(), "the tree this case is about was not described");
        if (!beforeLayout)
            return 1;
        // Control mode's DCS preamble, a layout for two tmux panes %7 and %9
        // side by side, and the exit; what tmux -CC writes to its terminal.
        WorkspaceLayoutProjectionAccess::writeOutput(peer.daemon, peer.sessionID,
            "\x1bP1000p%begin 1 1 0\r\n%end 1 1 0\r\n%layout-change @1 b1d2,80x24,0,0{40x24,0,0,7,39x24,41,0,9} b1d2,80x24,0,0{40x24,0,0,7,39x24,41,0,9} *\r\n\x1b\\");
        const Attached later = peer.attach(peer.sessionID, WorkspaceClaim::Claimed);
        const auto after = readAll(peer.daemon, later.index, later.ends[1]);
        const auto afterFrame = layoutArrival(after);
        const auto afterLayout = afterFrame ? decodeWorkspaceLayout(afterFrame->payload) : std::nullopt;
        check(afterLayout && sameLayout(*afterLayout, *beforeLayout), "a tmux layout inside a pane changed the hosted tree");
        if (afterLayout) {
            const auto& panes = afterLayout->surfaces[0].tabs[0].panes;
            check(panes.size() == 1 && panes[0].paneID != 7 && panes[0].paneID != 9, "the hosted tree carries a tmux pane id");
        }
    }

    // (h) A pane the model mints is a session the daemon spawns, so the verb that
    // mints it asks for the scope that creates sessions. A token holding Drive and
    // not NewSession may rearrange the workspace and may not grow it.
    {
        Peer peer(WorkspaceClaim::Claimed, "shared");
        static_cast<void>(readAll(peer.daemon, peer.index, peer.ends[1]));
        const Attached driver = peer.attach(peer.sessionID, WorkspaceClaim::Claimed,
            ScopeSet { Scope::Observe, Scope::Drive, Scope::Control });
        static_cast<void>(readAll(peer.daemon, driver.index, driver.ends[1]));
        const std::size_t sessionsBefore = WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon);

        WorkspaceIntent split {};
        split.workspaceID = peer.sessionID;
        split.requestID = 0x5F10;
        split.verb = WorkspaceIntentKind::SplitFocusedPane;
        split.surfaceID = 1;
        split.orientation = static_cast<std::uint8_t>(Ubiquitty::Panes::Orientation::Vertical);
        split.ratio = 0.5f;
        driveLayoutIntent(peer.daemon, driver.index, driver.ends[1], split);
        const auto frames = readAll(peer.daemon, driver.index, driver.ends[1]);
        const auto denied = firstIntentResult(frames);
        check(denied && denied->intentResult && !denied->intentResult->wasPerformed
                && intentRefusedWith(*denied, Ubiquitty::Protocol::Control::ErrorCode::Forbidden),
            "a canonical split without the new-session scope was not refused");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == sessionsBefore,
            "a token without the new-session scope got a session out of a split");
    }

    // (i) Reshaping the workspace is a person's act. An agent the human approved
    // for one pane may drive that pane's session; it may not close the surface
    // and take every other pane's session down with it.
    {
        Peer peer(WorkspaceClaim::Claimed, "shared");
        static_cast<void>(readAll(peer.daemon, peer.index, peer.ends[1]));
        WorkspaceIntent addTab {};
        addTab.workspaceID = peer.sessionID;
        addTab.requestID = 0x5F20;
        addTab.verb = WorkspaceIntentKind::AddTab;
        addTab.surfaceID = 1;
        std::vector<std::uint8_t> frame;
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], addTab);
        const auto built = readAll(peer.daemon, peer.index, peer.ends[1]);
        std::uint32_t paneSession = 0;
        if (const auto resultFrame = firstIntentResult(built)) {
            const auto result = resultFrame->intentResult;
            if (result)
                paneSession = result->sessionID;
        }
        check(paneSession != 0, "the second pane this case is about was not minted");
        if (paneSession == 0)
            return 1;

        // The grant a human's consent leaves behind, on that pane's session only.
        WorkspaceLayoutProjectionAccess::grantAgentSession(peer.daemon, paneSession, "workspace-agent");
        const Attached agent = peer.attach(paneSession, WorkspaceClaim::Unclaimed,
            ScopeSet { Scope::Observe, Scope::Drive, Scope::Control, Scope::NewSession },
            /*agentLabel*/ "mcp", /*tokenID*/ "workspace-agent");
        const auto attached = readAll(peer.daemon, agent.index, agent.ends[1]);
        const auto attachedSession = admittedWelcome(attached, agent.ends[1]);
        check(attachedSession && attachedSession->selection.sessionID == paneSession && attachedSession->grantedRole == Sync::Role::Agent,
            "the granted agent did not attach to its one session as Agent");
        if (!attachedSession || attachedSession->selection.sessionID != paneSession)
            return 1;
        const std::size_t sessionsBefore = WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon);
        check(sessionsBefore == 2, "the workspace this case closes does not hold two panes");

        WorkspaceIntent closeSurface {};
        closeSurface.workspaceID = peer.sessionID;
        closeSurface.requestID = 0x5F21;
        closeSurface.verb = WorkspaceIntentKind::CloseSurface;
        closeSurface.surfaceID = 1;
        driveLayoutIntent(peer.daemon, agent.index, agent.ends[1], closeSurface);
        const auto frames = readAll(peer.daemon, agent.index, agent.ends[1]);
        const auto denied = firstIntentResult(frames);
        check(!denied || (denied->intentResult && !denied->intentResult->wasPerformed),
            "an agent closed a window without its observed revision and exact approval");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == sessionsBefore,
            "an agent attached to one pane ended the workspace's sessions");
    }

    // (j) Topology is a person's to see. The broadcast path already refuses an
    // agent that claims Mux, and the rule it states there — only authenticated
    // interactive clients receive unfiltered topology — does not depend on which
    // capability the agent claimed. A workspace's layout names every pane in it,
    // including the sessions an agent was never approved for, so an agent
    // approved for one pane learns the shape of the whole workspace from the
    // frame it is handed on attach.
    {
        Peer peer(WorkspaceClaim::Claimed, "shared");
        static_cast<void>(readAll(peer.daemon, peer.index, peer.ends[1]));
        WorkspaceIntent addTab {};
        addTab.workspaceID = peer.sessionID;
        addTab.requestID = 0x5F30;
        addTab.verb = WorkspaceIntentKind::AddTab;
        addTab.surfaceID = 1;
        std::vector<std::uint8_t> frame;
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], addTab);
        const auto built = readAll(peer.daemon, peer.index, peer.ends[1]);
        std::uint32_t paneSession = 0;
        if (const auto resultFrame = firstIntentResult(built)) {
            const auto result = resultFrame->intentResult;
            if (result)
                paneSession = result->sessionID;
        }
        check(paneSession != 0, "the second pane this case is about was not minted");
        if (paneSession == 0)
            return 1;

        WorkspaceLayoutProjectionAccess::grantAgentSession(peer.daemon, paneSession, "topology-agent");
        const Attached agent = peer.attach(paneSession, WorkspaceClaim::Claimed,
            ScopeSet { Scope::Observe, Scope::Drive, Scope::Control, Scope::NewSession },
            /*agentLabel*/ "mcp", /*tokenID*/ "topology-agent");
        const auto agentFrames = readAll(peer.daemon, agent.index, agent.ends[1]);
        const auto refusal = firstOfType(agentFrames, Sync::FrameKind::Bye);
        const auto reason = refusal ? Sync::decodeBye(refusal->payload) : std::nullopt;
        check(reason && reason->reason == Sync::ByeReason::OutsideScope
                && !carries(agentFrames, Sync::FrameKind::Welcome) && !carries(agentFrames, Sync::FrameKind::PaneBind)
                && !screenEnds().at(agent.ends[1]).client->welcome() && !screenEnds().at(agent.ends[1]).client->pane()
                && !carries(agentFrames, Sync::FrameKind::WorkspaceStructure) && !carriesLayout(agentFrames),
            "an Agent Workspace request must be refused before Welcome, pane binding or topology");

        const Attached singlePaneAgent = peer.attach(paneSession, WorkspaceClaim::Unclaimed,
            ScopeSet { Scope::Observe, Scope::Drive, Scope::Control, Scope::NewSession }, "mcp", "topology-agent");
        const auto singlePaneFrames = readAll(peer.daemon, singlePaneAgent.index, singlePaneAgent.ends[1]);
        const auto singlePaneWelcome = admittedWelcome(singlePaneFrames, singlePaneAgent.ends[1]);
        check(singlePaneWelcome && singlePaneWelcome->grantedRole == Sync::Role::Agent
                && carries(singlePaneFrames, Sync::FrameKind::PaneBind) && screenEnds().at(singlePaneAgent.ends[1]).client->pane()
                && !carries(singlePaneFrames, Sync::FrameKind::WorkspaceStructure) && !carriesLayout(singlePaneFrames),
            "the separately authorized Agent Session attaches only its pane, never the whole topology");

        // The same act reaches the agent a second way: a layout that changes is
        // broadcast to every client of the workspace, and an agent holding the
        // claim without Mux takes the branch the refusal above sits past.
        WorkspaceIntent secondTab {};
        secondTab.workspaceID = peer.sessionID;
        secondTab.requestID = 0x5F31;
        secondTab.verb = WorkspaceIntentKind::AddTab;
        secondTab.surfaceID = 1;
        frame.clear();
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], secondTab);
        static_cast<void>(readAll(peer.daemon, peer.index, peer.ends[1]));
        const auto afterBroadcast = readAll(peer.daemon, agent.index, agent.ends[1]);
        check(!carries(afterBroadcast, Sync::FrameKind::WorkspaceStructure) && !carriesLayout(afterBroadcast),
            "a workspace layout broadcast reached an agent claiming Workspace without Mux");
        const auto singlePaneBroadcast = readAll(peer.daemon, singlePaneAgent.index, singlePaneAgent.ends[1]);
        check(!carries(singlePaneBroadcast, Sync::FrameKind::WorkspaceStructure) && !carriesLayout(singlePaneBroadcast)
                && screenEnds().at(singlePaneAgent.ends[1]).client->connection().state() == Sync::ConnectionState::Attached,
            "a workspace broadcast leaked topology to the admitted single-pane Agent or silently closed it");

        // And the gate is about who asked, not about the frame: the human on the
        // same workspace still receives it, so a refusal that silenced everyone
        // would not read as a pass here.
        const Attached human = peer.attach(peer.sessionID, WorkspaceClaim::Claimed);
        const auto humanFrames = readAll(peer.daemon, human.index, human.ends[1]);
        check(layoutArrival(humanFrames).has_value(),
            "a person claiming Workspace was not told the workspace's topology");
    }

    // (k) Labels travel where SESSION_LIST already does, with the placement a
    // tool needs to address the pane: window, tab and pane IDs unique across
    // the host's workspaces.
    {
        Peer peer(WorkspaceClaim::Claimed, "labelled");
        const auto attachFrames = readAll(peer.daemon, peer.index, peer.ends[1]);
        const auto layoutFrame = layoutArrival(attachFrames);
        const auto layout = layoutFrame ? decodeWorkspaceLayout(layoutFrame->payload) : std::nullopt;
        check(layout && layout->surfaces.size() == 1 && layout->surfaces[0].tabs.size() == 1, "the labelled workspace did not describe its root");
        if (!layout || layout->surfaces.empty() || layout->surfaces[0].tabs.empty() || layout->surfaces[0].tabs[0].panes.empty())
            return 1;
        const std::uint32_t surfaceID = layout->surfaces[0].surfaceID;
        const std::uint32_t paneID = layout->surfaces[0].tabs[0].panes[0].paneID;

        // A second workspace on the same host, to prove its IDs are not the
        // first one's and that its pane is not the first client's to label.
        const std::uint32_t otherSession = WorkspaceLayoutProjectionAccess::makeSession(peer.daemon, "elsewhere");
        const Attached other = peer.attach(otherSession, WorkspaceClaim::Claimed);
        const auto otherFrames = readAll(peer.daemon, other.index, other.ends[1]);
        const auto otherLayoutFrame = layoutArrival(otherFrames);
        const auto otherLayout = otherLayoutFrame ? decodeWorkspaceLayout(otherLayoutFrame->payload) : std::nullopt;
        std::uint32_t otherPane = 0;
        if (otherLayout && !otherLayout->surfaces.empty() && !otherLayout->surfaces[0].tabs.empty() && !otherLayout->surfaces[0].tabs[0].panes.empty()) {
            otherPane = otherLayout->surfaces[0].tabs[0].panes[0].paneID;
            check(otherLayout->surfaces[0].surfaceID != surfaceID, "two workspaces on one host hold the same window ID");
        }
        check(otherPane != 0 && otherPane != paneID, "two workspaces on one host hold the same pane ID");

        const auto label = [&](WorkspaceIntentKind kind, std::uint32_t surface, std::uint32_t pane, const std::string& title) {
            WorkspaceIntent verb {};
            verb.workspaceID = peer.sessionID;
            verb.requestID = 0x1AB;
            verb.verb = kind;
            verb.surfaceID = surface;
            verb.paneID = pane;
            verb.title = title;
            std::vector<std::uint8_t> frame;
            driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], verb);
            const auto frames = readAll(peer.daemon, peer.index, peer.ends[1]);
            const auto resultFrame = firstIntentResult(frames);
            const auto result = resultFrame ? resultFrame->intentResult : std::nullopt;
            return result && result->wasPerformed;
        };
        check(label(WorkspaceIntentKind::LabelSurface, surfaceID, 0, "main"), "labelling the window was refused");
        check(label(WorkspaceIntentKind::LabelPane, surfaceID, paneID, "logs"), "labelling the pane was refused");
        check(label(WorkspaceIntentKind::LabelSession, surfaceID, paneID, "build"), "labelling the session was refused");
        check(label(WorkspaceIntentKind::LabelClient, 0, 0, "laptop"), "labelling the client was refused");
        check(!label(WorkspaceIntentKind::LabelPane, surfaceID, paneID, "pane:9"), "a label that reads as an ID was accepted");
        check(!label(WorkspaceIntentKind::LabelPane, surfaceID, otherPane, "stolen"), "a client labelled a pane in another workspace");

        const auto listed = readDirectory(peer.daemon, peer.index, peer.ends[1], 905);
        check(listed && listed->size() == 2, "the host did not list both sessions");
        if (listed) {
            for (const SessionRow& row : *listed) {
                if (row.id == peer.sessionID) {
                    check(row.label == "build", "the session's label did not travel in SESSION_LIST");
                    check(row.windowID == surfaceID && row.windowLabel == "main", "the window's ID or label did not travel");
                    check(row.paneID == paneID && row.paneLabel == "logs", "the pane's ID or label did not travel, or a refused label replaced it");
                    check(row.tabID != 0, "the tab's ID did not travel");
                } else {
                    check(row.paneID == otherPane && row.paneLabel.empty(), "the other workspace's pane took a label its client never gave it");
                }
            }
        }
    }

    // (l) A pane verb may come from a connection bound to no workspace:
    // the pane it names picks the workspace. Legacy agent split/label verbs
    // have no exact-operation consent exchange and cannot use standing grants
    // to bypass workspace.split/label.set questions.
    {
        Peer peer(WorkspaceClaim::Claimed, "watched");
        const auto initialSession = admittedWelcome(readAll(peer.daemon, peer.index, peer.ends[1]), peer.ends[1]);
        check(initialSession && initialSession->selection.sessionID == peer.sessionID, "the bound person did not attach to the watched workspace");
        if (!initialSession || initialSession->selection.sessionID != peer.sessionID)
            return 1;
        const std::uint32_t lonely = WorkspaceLayoutProjectionAccess::makeSession(peer.daemon, "unattended");
        const Attached agent = peer.attach(Sync::AdmissionTarget::Control, WorkspaceClaim::Unclaimed,
            ScopeSet { Scope::Observe, Scope::Drive, Scope::Control, Scope::NewSession }, "ubq", "pane-verb-agent");
        const Attached labelWatcher = peer.attach(Sync::AdmissionTarget::Control, WorkspaceClaim::Unclaimed,
            ScopeSet { Scope::Observe, Scope::Control, Scope::NewSession }, "ubq", "pane-verb-agent");
        const Attached splitWatcher = peer.attach(Sync::AdmissionTarget::Control, WorkspaceClaim::Unclaimed,
            ScopeSet { Scope::Observe, Scope::Control, Scope::NewSession }, "ubq", "pane-verb-agent");
        const Attached remotePerson = peer.attach(Sync::AdmissionTarget::Control, WorkspaceClaim::Unclaimed);
        const Attached person = peer.attach(Sync::AdmissionTarget::Control, WorkspaceClaim::Unclaimed,
            ScopeSet { Scope::Observe, Scope::Drive, Scope::Control, Scope::NewSession }, {}, "workspace-layout-projection", PeerOrigin::TrustedLocal);
        const Attached boundPerson { peer.index, peer.ends };
        for (const Attached& client : { agent, labelWatcher, splitWatcher, remotePerson, person }) {
            const auto attached = admittedWelcome(readAll(peer.daemon, client.index, client.ends[1]), client.ends[1]);
            check(attached && attached->selection.target == Sync::AdmissionTarget::Control && !attached->selection.sessionID && !attached->selection.workspaceID,
                "a control-only pane-verb client was not admitted without a session binding");
            if (!attached || attached->selection.sessionID)
                return 1;
        }

        std::vector<std::uint8_t> request;
        const auto listed = readDirectory(peer.daemon, person.index, person.ends[1], 906);
        std::uint32_t watchedPane = 0;
        std::uint32_t lonelyPane = 0;
        if (listed) {
            for (const SessionRow& row : *listed) {
                if (row.id == peer.sessionID)
                    watchedPane = row.paneID;
                if (row.id == lonely)
                    lonelyPane = row.paneID;
            }
        }
        check(watchedPane && lonelyPane, "the list did not place both sessions in panes");

        const auto sendPaneVerb = [&](const Attached& client, WorkspaceIntentKind kind, std::uint32_t pane, const std::string& title) {
            using Ubiquitty::Codec::JSON::Value;
            ScreenEnd& end = screenEnds().at(client.ends[1]);
            WorkspaceIntent verb { .requestID = 0x1AC, .verb = kind, .paneID = pane, .title = title };
            auto parameters = Value::object();
            std::string method;
            if (kind == WorkspaceIntentKind::SplitFocusedPane) {
                method = "workspace.split";
                parameters.set("pane", Value::number(pane));
                parameters.set("orientation", Value::string("vertical"));
                parameters.set("ratio", Value::number(0.5));
            } else if (kind == WorkspaceIntentKind::LabelPane || kind == WorkspaceIntentKind::LabelSession) {
                method = "label.set";
                std::uint32_t target = pane;
                if (kind == WorkspaceIntentKind::LabelSession) {
                    target = 0;
                    if (listed)
                        for (const SessionRow& row : *listed)
                            if (row.paneID == pane)
                                target = row.id;
                }
                parameters.set("object", Value::string((kind == WorkspaceIntentKind::LabelPane ? "pane:" : "session:") + std::to_string(target)));
                parameters.set("label", Value::string(title));
            } else {
                check(false, "the pane intent has no canonical method");
                return std::vector<Frame> {};
            }
            const auto id = "pane-intent-" + std::to_string(++end.nextIntent);
            auto call = Value::object();
            call.set("jsonrpc", Value::string("2.0"));
            call.set("id", Value::string(id));
            call.set("method", Value::string(method));
            call.set("params", std::move(parameters));
            check(end.client && end.client->call(Ubiquitty::Codec::JSON::serialize(call)), "a pane Call was not queued");
            end.pendingIntents.emplace(id, std::move(verb));
            WorkspaceLayoutProjectionAccess::drive(peer.daemon, client.index, end.client->takeUpstream(Ubiquitty::Host::ScreenPlaneAccess::now()));
            WorkspaceLayoutProjectionAccess::dispatchCalls(peer.daemon, client.index);
            return readAll(peer.daemon, client.index, client.ends[1]);
        };
        const auto paneVerb = [&](const Attached& client, WorkspaceIntentKind kind, std::uint32_t pane, const std::string& title) {
            const auto resultFrame = firstIntentResult(sendPaneVerb(client, kind, pane, title));
            return resultFrame ? resultFrame->intentResult : std::nullopt;
        };
        const auto isPerformed = [](const auto& result) { return result && result->wasPerformed; };
        const auto typedPaneSplit = [&](const Attached& client, std::uint32_t pane) {
            WorkspaceIntent verb;
            verb.requestID = 0x1AD;
            verb.verb = WorkspaceIntentKind::SplitFocusedPane;
            verb.paneID = pane;
            verb.orientation = std::to_underlying(Ubiquitty::Panes::Orientation::Vertical);
            verb.ratio = 0.5f;
            driveLayoutIntent(peer.daemon, client.index, client.ends[1], verb);
            const auto resultFrame = firstIntentResult(readAll(peer.daemon, client.index, client.ends[1]));
            return resultFrame ? resultFrame->intentResult : std::nullopt;
        };
        const auto typedPaneLabel = [&](const Attached& client, std::uint32_t pane, const std::string& label) {
            WorkspaceIntent verb;
            verb.requestID = 0x1AE;
            verb.verb = WorkspaceIntentKind::LabelPane;
            verb.paneID = pane;
            verb.title = label;
            driveLayoutIntent(peer.daemon, client.index, client.ends[1], verb);
            const auto resultFrame = firstIntentResult(readAll(peer.daemon, client.index, client.ends[1]));
            return resultFrame ? resultFrame->intentResult : std::nullopt;
        };
        const auto isScopeRefused = [&](const Attached& client, WorkspaceIntentKind kind, std::uint32_t pane, const std::string& title) {
            const auto frames = sendPaneVerb(client, kind, pane, title);
            const auto refused = firstIntentResult(frames);
            return refused && refused->intentResult && !refused->intentResult->wasPerformed
                && intentRefusedWith(*refused, Ubiquitty::Protocol::Control::ErrorCode::Forbidden);
        };

        check(!isPerformed(paneVerb(agent, WorkspaceIntentKind::LabelPane, watchedPane, "taken")), "an agent labelled a pane in a workspace a person is using");
        check(!isPerformed(paneVerb(agent, WorkspaceIntentKind::LabelSession, lonelyPane, "batch")), "an agent labelled a session it holds no grant for");
        check(!isPerformed(paneVerb(agent, WorkspaceIntentKind::SplitFocusedPane, lonelyPane, "")), "an agent split a workspace it holds no grant for");
        check(!isPerformed(paneVerb(remotePerson, WorkspaceIntentKind::LabelPane, watchedPane, "remote")), "a remote person labelled a pane in a workspace it is not bound to");
        check(!isPerformed(paneVerb(remotePerson, WorkspaceIntentKind::SplitFocusedPane, lonelyPane, "")), "a remote person split a workspace it is not bound to");
        check(isPerformed(typedPaneLabel(boundPerson, watchedPane, "mine")), "a remote person could not label a pane in its own workspace");

        WorkspaceLayoutProjectionAccess::grantAgentSession(peer.daemon, lonely, "pane-verb-agent");
        check(!WorkspaceLayoutProjectionAccess::hasHumanClient(peer.daemon, lonely), "the label consent-bypass case is not unattended");
        check(!isPerformed(paneVerb(agent, WorkspaceIntentKind::LabelSession, lonelyPane, "batch")), "an agent's standing grant bypassed exact-operation consent for label.set");
        check(isScopeRefused(labelWatcher, WorkspaceIntentKind::LabelSession, lonelyPane, "peek"), "a label from a token without Drive was not refused for insufficient scope");
        check(isScopeRefused(splitWatcher, WorkspaceIntentKind::SplitFocusedPane, lonelyPane, ""), "a split from a token without Drive was not refused for insufficient scope");
        check(isPerformed(typedPaneLabel(person, watchedPane, "logs")), "a person could not label a pane by naming it");
        const std::size_t before = WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon);
        const auto split = typedPaneSplit(person, watchedPane);
        check(split && split->wasPerformed && split->paneID && split->sessionID, "a person could not split a pane by naming it");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == before + 1, "the split pane has no session behind it");
        check(!WorkspaceLayoutProjectionAccess::hasHumanClient(peer.daemon, lonely), "the split consent-bypass case is not unattended");
        const auto agentSplit = paneVerb(agent, WorkspaceIntentKind::SplitFocusedPane, lonelyPane, "");
        check(!isPerformed(agentSplit), "an agent's standing grant bypassed exact-operation consent");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == before + 1, "an unapproved split created a session");
        const auto personSplit = typedPaneSplit(person, lonelyPane);
        check(personSplit && personSplit->wasPerformed, "a person could not split the second workspace");
        check(split && !WorkspaceLayoutProjectionAccess::isGrantedTo(peer.daemon, split->sessionID, "workspace-layout-projection"),
            "a person's split was granted to an agent identity");

        // A split the token may not back is refused before the named pane is
        // focused, so the workspace a person returns to is the one they left.
        const Attached builder = peer.attach(lonely, WorkspaceClaim::Unclaimed,
            ScopeSet { Scope::Observe, Scope::Drive, Scope::Control });
        const auto builderSession = admittedWelcome(readAll(peer.daemon, builder.index, builder.ends[1]), builder.ends[1]);
        check(builderSession && builderSession->selection.sessionID == lonely, "the restricted builder did not attach to the target workspace");
        if (!builderSession || builderSession->selection.sessionID != lonely)
            return 1;
        const std::uint32_t focusedBefore = WorkspaceLayoutProjectionAccess::focusedPaneBeside(peer.daemon, lonely);
        const std::uint32_t unfocused = focusedBefore == lonelyPane && personSplit ? personSplit->paneID : lonelyPane;
        check(focusedBefore != 0 && unfocused != focusedBefore, "the granted workspace does not hold two panes to choose between");
        const auto sessionsBeforeRefusedSplit = WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon);
        check(!isPerformed(typedPaneSplit(builder, unfocused)), "a token that cannot create sessions split a pane");
        check(WorkspaceLayoutProjectionAccess::sessionCount(peer.daemon) == sessionsBeforeRefusedSplit, "a refused split created a session");
        check(WorkspaceLayoutProjectionAccess::focusedPaneBeside(peer.daemon, lonely) == focusedBefore, "a refused split moved the workspace's focus");
    }

    // (m) A minted pane's session takes the theme of the pane it opens from.
    {
        Peer peer(WorkspaceClaim::Claimed, "themed");
        readAll(peer.daemon, peer.index, peer.ends[1]);
        // The theme switch arrives as a client sends it, through the handler,
        // from a person attached to the root session alone.
        const Attached switcher = peer.attach(peer.sessionID, WorkspaceClaim::Unclaimed);
        readAll(peer.daemon, switcher.index, switcher.ends[1]);
        Ubiquitty::TermPalette theme;
        theme.seedDefaults();
        theme.defaultBackground = { 0xFBFB, 0xF1F1, 0xC7C7 };
        theme.defaultForeground = { 0x3C3C, 0x3838, 0x3636 };
        theme.cursor = { 0x4242, 0x7B7B, 0x5858 };
        theme.indexed[1] = { 0xCCCC, 0x2424, 0x1D1D };
        theme.fillExtended();
        screenEnds().at(switcher.ends[1]).client->setThemePalette(Ubiquitty::Protocol::Sync::ThemePalette { .live = theme, .baseline = theme });
        readAll(peer.daemon, switcher.index, switcher.ends[1]);
        const auto source = WorkspaceLayoutProjectionAccess::paletteOf(peer.daemon, peer.sessionID);
        check(source && source->second.defaultBackground == theme.defaultBackground, "the theme switch did not reach the root session");
        // A program in the root pane then paints its own ground, which a
        // fresh shell beside it must not inherit.
        WorkspaceLayoutProjectionAccess::writeOutput(peer.daemon, peer.sessionID, "\x1b]11;#ff0000\x07");
        const auto overridden = WorkspaceLayoutProjectionAccess::paletteOf(peer.daemon, peer.sessionID);
        check(overridden && !(overridden->first.defaultBackground == theme.defaultBackground),
            "the program's background override did not land on the root session");

        const std::uint32_t rootPane = WorkspaceLayoutProjectionAccess::paneOf(peer.daemon, peer.sessionID);
        check(rootPane != 0, "the root session has no pane to open from");
        struct Opening {
            WorkspaceIntentKind verb;
            std::uint32_t paneID;
            const char* name;
        };
        // The split names no pane: it opens from the one it divides, the
        // focused root.
        for (const Opening& opening : { Opening { WorkspaceIntentKind::AddTab, rootPane, "AddTab" },
                 Opening { WorkspaceIntentKind::AddSurface, rootPane, "AddSurface" },
                 Opening { WorkspaceIntentKind::SplitFocusedPane, 0, "SplitFocusedPane" } }) {
            WorkspaceVerbMessage verb {};
            verb.workspaceID = peer.sessionID;
            verb.requestID = 0x7E3E;
            verb.verb = opening.verb;
            verb.surfaceID = 1;
            verb.paneID = opening.paneID;
            verb.orientation = static_cast<decltype(verb.orientation)>(Ubiquitty::Panes::Orientation::Vertical);
            verb.ratio = 0.5f;
            driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], verb);
            const auto resultFrame = firstIntentResult(readAll(peer.daemon, peer.index, peer.ends[1]));
            const auto result = resultFrame ? resultFrame->intentResult : std::nullopt;
            const std::string what = std::string(opening.name) + ": ";
            check(result && result->wasPerformed && result->sessionID != 0, (what + "the verb minted no session").c_str());
            if (!result || !result->sessionID)
                continue;
            const auto minted = WorkspaceLayoutProjectionAccess::paletteOf(peer.daemon, result->sessionID);
            check(minted && minted->second == source->second, (what + "the new session's theme is not the source's").c_str());
            check(minted && minted->first == source->second,
                (what + "the new session's live palette is not the source's theme, override excluded").c_str());
        }
    }

    // (n) A structure that first names a pane leaves before any frame of the
    // pane, and the one that retires it after the pane's last: a carrier is
    // sent them in that order when the pane is made and written to in one
    // pass, and when its socket was full for the whole of it.
    for (const bool isBackpressured : { false, true }) { // bool-ok: the two carriers the case is run for
        const std::string what = isBackpressured ? "(n) a carrier that read nothing meanwhile: " : "(n) ";
        Peer peer(WorkspaceClaim::Claimed, "ordered");
        static_cast<void>(readAll(peer.daemon, peer.index, peer.ends[1]));
        if (isBackpressured)
            check(WorkspaceLayoutProjectionAccess::queueBackpressuredOutput(peer.daemon, peer.index), (what + "premise: the carrier's output is backed up").c_str());
        WorkspaceIntent add {};
        add.workspaceID = peer.sessionID;
        add.requestID = 950;
        add.verb = WorkspaceIntentKind::AddTab;
        add.surfaceID = 1; // the fresh model's first surface
        std::vector<std::uint8_t> frame;
        const std::uint32_t sessionBefore = WorkspaceLayoutProjectionAccess::newestSession(peer.daemon);
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], add);
        const std::uint32_t minted = WorkspaceLayoutProjectionAccess::newestSession(peer.daemon);
        check(minted != 0 && minted != sessionBefore, (what + "premise: AddTab minted a session").c_str());
        const auto mintedPane = WorkspaceLayoutProjectionAccess::paneIdentity(peer.daemon, minted);
        check(mintedPane.has_value(), (what + "premise: the fixture holds the exact new pane identity").c_str());
        if (!mintedPane)
            continue;
        // The pane's program writes before the daemon's pass ends.
        WorkspaceLayoutProjectionAccess::writeOutput(peer.daemon, minted, "written in the pass that made the pane\r\n");
        std::vector<Frame> frames = readAll(peer.daemon, peer.index, peer.ends[1]);
        PaneOrder made = orderOf(planeFrames(frames), minted, *mintedPane);
        check(made.firstNamed.has_value() && made.bind.has_value() && made.firstFrame.has_value(),
            (what + "premise: the carrier was sent the structure naming the new pane, its PaneBind and a frame of it").c_str());
        if (made.firstNamed && made.bind && made.firstFrame) {
            check(*made.firstNamed < *made.bind, (what + "the new pane's PaneBind left before the structure that names it").c_str());
            check(*made.bind < *made.firstFrame, (what + "a frame of the new pane left before its PaneBind").c_str());
        }

        // Closing the tab: the pane's last frame and its PaneUnbind, then
        // the structure that no longer names it.
        const auto result = firstIntentResult(frames);
        const auto added = result ? result->intentResult : std::nullopt;
        if (!added || !added->wasPerformed) {
            check(false, (what + "premise: the AddTab was answered").c_str());
            continue;
        }
        WorkspaceIntent close {};
        close.workspaceID = peer.sessionID;
        close.surfaceID = 1;
        close.tabID = added->tabID;
        close.requestID = 951;
        close.verb = WorkspaceIntentKind::CloseTab;
        frame.clear();
        if (isBackpressured)
            check(WorkspaceLayoutProjectionAccess::queueBackpressuredOutput(peer.daemon, peer.index), (what + "premise: the carrier's output is backed up").c_str());
        WorkspaceLayoutProjectionAccess::writeOutput(peer.daemon, minted, "written in the pass that closed it\r\n");
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], close);
        const std::vector<Frame> closing = readAll(peer.daemon, peer.index, peer.ends[1]);
        frames.insert(frames.end(), closing.begin(), closing.end());
        const PaneOrder closed = orderOf(planeFrames(frames), minted, *mintedPane);
        check(closed.unbind.has_value() && closed.firstRetired.has_value() && closed.lastFrame.has_value(),
            (what + "premise: the carrier was sent the closed pane's PaneUnbind and a structure without it").c_str());
        if (closed.unbind && closed.firstRetired && closed.lastFrame) {
            check(*closed.lastFrame < *closed.firstRetired, (what + "the structure that retires a pane left before the pane's last frame").c_str());
            check(*closed.unbind < *closed.firstRetired, (what + "the structure that retires a pane left before its PaneUnbind").c_str());
        }
    }

    // (o) A structure longer than a frame leaves as a bulk transfer, and a
    // pane it first names is bound once the whole transfer has left: a
    // workspace of as many panes as a host runs, every tab named at a name's
    // full length, gains its last pane while that pane's program writes.
    for (const bool isBackpressured : { false, true }) { // bool-ok: the two carriers the case is run for
        namespace Sync = Ubiquitty::Protocol::Sync;
        const std::string what = isBackpressured ? "(o) a carrier that read nothing meanwhile: " : "(o) ";
        Peer peer(WorkspaceClaim::Claimed, "long");
        static_cast<void>(readAll(peer.daemon, peer.index, peer.ends[1]));
        std::uint32_t nextRequest = 0x2000;
        std::size_t performed = 0;
        std::vector<WorkspaceVerbMessage> batch;
        std::size_t batched = 0;
        const auto flush = [&] {
            if (batched == 0)
                return;
            // A typed split names a pane, so each creation's returned
            // structure supplies the next operation's source identity.
            for (const auto& intent : batch) {
                driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], intent);
                for (const Frame& frame : readAll(peer.daemon, peer.index, peer.ends[1])) {
                    if (frame.type || !frame.intentResult)
                        continue;
                    if (const auto result = frame.intentResult; result && result->wasPerformed)
                        ++performed;
                }
            }
            batch.clear();
            batched = 0;
        };
        const auto queue = [&](WorkspaceIntent verb) {
            verb.workspaceID = peer.sessionID;
            verb.surfaceID = 1; // the fresh model's first surface
            verb.requestID = ++nextRequest;
            batch.push_back(std::move(verb));
            if (++batched == 8)
                flush();
        };
        WorkspaceIntent split {};
        split.verb = WorkspaceIntentKind::SplitFocusedPane;
        split.orientation = static_cast<std::uint8_t>(Ubiquitty::Panes::Orientation::Vertical);
        split.ratio = 0.5f;
        WorkspaceIntent addTab {};
        addTab.verb = WorkspaceIntentKind::AddTab;
        // Every tab of the window split in two but the newest, which the
        // case's own verb splits: one pane short of what a host runs.
        const std::size_t tabs = static_cast<std::size_t>(Ubiquitty::Session::Layout::maxTabsPerSurface);
        std::size_t asked = 0;
        queue(split);
        ++asked;
        for (std::size_t tab = 1; tab < tabs; ++tab) {
            queue(addTab);
            ++asked;
            if (tab + 1 < tabs) {
                queue(split);
                ++asked;
            }
        }
        flush();
        const auto& assembled = screenEnds()[peer.ends[1]].assembled[peer.sessionID];
        const auto built = assembled.empty() ? std::nullopt : Ubiquitty::Session::decodeWorkspaceStructure(assembled.rbegin()->second);
        const bool isBuilt = built && built->windows.size() == 1 && built->windows.front().tabs.size() == tabs; // bool-ok: one premise's answer
        check(performed == asked && isBuilt, (what + "premise: the window holds every tab the model allows it").c_str());
        if (!isBuilt)
            continue;
        // Letters no deflate folds to a frame's length.
        std::uint32_t state = 0x9E3779B9u;
        performed = 0;
        asked = 0;
        for (const Ubiquitty::Session::TreeTab& tab : built->windows.front().tabs) {
            WorkspaceVerbMessage rename {};
            rename.verb = WorkspaceIntentKind::RenameTab;
            rename.tabID = std::to_underlying(tab.id);
            for (std::size_t letter = 0; letter < Ubiquitty::Session::maxTreeNameBytes; ++letter) {
                state = state * 1664525u + 1013904223u;
                rename.title.push_back(static_cast<char>('a' + (state >> 24) % 26));
            }
            queue(rename);
            ++asked;
        }
        flush();
        check(performed == asked, (what + "premise: every tab took a name of a name's full length").c_str());

        if (isBackpressured)
            check(WorkspaceLayoutProjectionAccess::queueBackpressuredOutput(peer.daemon, peer.index), (what + "premise: the carrier's output is backed up").c_str());
        const std::uint32_t sessionBefore = WorkspaceLayoutProjectionAccess::newestSession(peer.daemon);
        split.workspaceID = peer.sessionID;
        split.surfaceID = 1;
        split.requestID = ++nextRequest;
        std::vector<std::uint8_t> frame;
        driveLayoutIntent(peer.daemon, peer.index, peer.ends[1], split);
        const std::uint32_t minted = WorkspaceLayoutProjectionAccess::newestSession(peer.daemon);
        check(minted != 0 && minted != sessionBefore, (what + "premise: the split minted a session").c_str());
        const auto mintedPane = WorkspaceLayoutProjectionAccess::paneIdentity(peer.daemon, minted);
        check(mintedPane.has_value(), (what + "premise: the fixture holds the exact split pane identity").c_str());
        if (!mintedPane)
            continue;
        WorkspaceLayoutProjectionAccess::writeOutput(peer.daemon, minted, "written in the pass that made the pane\r\n");
        const std::vector<PlaneFrame> plane = planeFrames(readAll(peer.daemon, peer.index, peer.ends[1]));
        const PaneOrder made = orderOf(plane, minted, *mintedPane);
        check(made.bind.has_value() && made.firstFrame.has_value(), (what + "premise: the carrier was sent the new pane's PaneBind and a frame of it").c_str());
        if (!made.bind || !made.firstFrame)
            continue;
        check(*made.bind < *made.firstFrame, (what + "a frame of the new pane left before its PaneBind").c_str());
        // The newest structure sent before the PaneBind, and its transfer.
        std::optional<Sync::WorkspaceStructureHead> newest;
        for (std::size_t index = 0; index < *made.bind; ++index) {
            if (plane[index].kind != Sync::FrameKind::WorkspaceStructure)
                continue;
            if (auto head = Sync::decodeWorkspaceStructureHead(plane[index].body); head && head->workspaceID == peer.sessionID)
                newest = std::move(head);
        }
        check(newest.has_value(), (what + "the new pane's PaneBind left before any structure").c_str());
        if (!newest)
            continue;
        check(newest->transfer != 0 && newest->record.empty(), (what + "a structure longer than a frame was not sent as a transfer").c_str());
        std::size_t chunksBefore = 0;
        std::size_t chunksAfter = 0;
        bool didEndBefore = false; // bool-ok: whether the transfer's last chunk preceded the bind
        for (std::size_t index = 0; index < plane.size(); ++index) {
            if (plane[index].kind != Sync::FrameKind::BulkChunk)
                continue;
            const auto chunk = Sync::decodeBulkChunk(plane[index].body);
            if (!chunk || chunk->transfer != newest->transfer)
                continue;
            ++(index < *made.bind ? chunksBefore : chunksAfter);
            didEndBefore = didEndBefore || (chunk->isLast && index < *made.bind);
        }
        check(chunksBefore > 1, (what + "premise: the structure took more than one chunk").c_str());
        check(didEndBefore && chunksAfter == 0, (what + "the new pane was bound before the transfer of the structure naming it had left whole").c_str());
        // What the client put together from the chunks is the workspace.
        const auto record = assembled.find(newest->revision);
        const auto whole = record == assembled.end() ? std::nullopt : Ubiquitty::Session::decodeWorkspaceStructure(record->second);
        check(record != assembled.end() && record->second.size() > Sync::maxFrameBodyBytes && whole.has_value(), (what + "the client did not assemble the transferred structure into a record that reads").c_str());
        if (!whole)
            continue;
        std::size_t panes = 0;
        bool isNamed = false; // bool-ok: whether the record names the new pane
        bool areNamesWhole = true; // bool-ok: whether every tab's name crossed at its full length
        for (const Ubiquitty::Session::TreeWindow& window : whole->windows) {
            for (const Ubiquitty::Session::TreeTab& tab : window.tabs) {
                areNamesWhole = areNamesWhole && tab.name.size() == Ubiquitty::Session::maxTreeNameBytes;
                panes += tab.panes.size();
                for (const Ubiquitty::Session::TreePane& pane : tab.panes)
                    isNamed = isNamed || pane.sessionID == minted;
            }
        }
        check(isNamed, (what + "the structure sent whole before the PaneBind does not name the new pane").c_str());
        check(panes == Ubiquitty::Session::maxStructureLeaves && areNamesWhole, (what + "the assembled structure is not the workspace: every pane a host runs, every tab's name whole").c_str());
    }

    if (failed) {
        std::fprintf(stderr, "workspace_layout_projection: FAILED\n");
        return 1;
    }
    std::printf("workspace_layout_projection: PASSED\n");
    return 0;
}
