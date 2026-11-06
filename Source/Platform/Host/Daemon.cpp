// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// See Daemon.h. Single-threaded poll() reactor MULTIPLEXER: N sessions (each a
// forkpty'd shell + Core Terminal) and N clients each bound to one. The only
// platform surface is the shared PTY transport and POSIX sockets. Each session's
// Terminal is driven exactly as the macOS Stream path drives it (processBytes
// inbound, drainOutput for the back-channel), but each frame the coherence rule
// takes is published to the session's Sync screen source and carried to the
// attached clients' connections instead of painted.

// _GNU_SOURCE: SO_PEERCRED + struct ucred (the acceptClient peer-credential check)
// are glibc GNU extensions; must precede every system header. No effect on Apple.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostInstances.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Mouse/Hilite.h"
#include "Core/OSC/RichTextBlocks.h"
#include "Core/OSC/TabStatus.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Control/HostAdministration.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Control/ProcessSnapshot.h"
#include "Core/Protocol/Sync/Control.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Protocol/Sync/HostManagement.h"
#include "Core/Protocol/Sync/IDText.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Protocol/Sync/WireFormat.h"
#include "Core/Screen/ClearBuffer.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Security/CapabilityConsentText.h"
#include "Core/Session/SharedWorkspace.h"
#include "Platform/Host/CapabilityGrantFile.h"
#include "Platform/Host/ClipboardTool.h"
#include "Platform/Host/ConsentStore.h"
#include "Platform/Host/GrantFileHeader.h"
#include "Platform/Host/HandoverAdmission.h"
#include "Platform/Host/HandoverStartup.h"
#include "Platform/Host/HostPaths.h"
#include "Platform/Host/HostSocketName.h"
#include "Platform/Host/LocalHandoff.h"
#include "Platform/Host/ManagementMetadata.h"
#include "Platform/Host/OutboundCapacity.h"
#include "Platform/Host/PeerProcessInstance.h"
#include "Platform/Host/PollWake.h"
#include "Platform/Host/SessionHandoverLedger.h"
#include "Platform/Host/TrustedLocalBinaries.h"

#include "Core/Crypto/SHA256.h"
#include "Platform/Host/TokenStore.h"

#include "Core/CellGrid.h"
#include "Core/Codec/Base64.h"
#include "Core/Codec/ZlibDeflate.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Modes/DisplayIdle.h"
#include "Core/Modes/Focus.h"
#include "Core/Modes/SyncOutput.h"
#include "Core/Network/RemoteLaunch.h"
#include "Core/Panes/DividerPath.h"
#include "Core/Panes/PaneTree.h"
#include "Core/Panes/PaneTreeNode.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Protocol/Sync/QUICBootstrap.h"
#include "Core/Protocol/Tmux/PaneLock.h"
#include "Core/Rendering/RenderPlan.h"
#include "Core/RichText/Blocks.h"
#include "Core/Runtime/DrainPass.h"
#include "Core/Runtime/TerminalLaunchSeed.h"
#include "Core/Runtime/TerminalRuntime.h"
#include "Core/Screen/Addressing.h"
#include "Core/Session/AdoptTransactionWire.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionShare.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/Session/ShareTerminalLimits.h"
#include "Core/Session/ShellForeground.h"
#include "Core/Session/WalkedProcessFacts.h"
#include "Core/Session/WorkspaceCapture.h"
#include "Core/Session/WorkspaceWire.h"
#include "Core/TerminalOperations.h"
#include "Core/Themes/RethemeDefaults.h"
#include "Core/Themes/ThemePreferences.h"
#include "Core/Title/WalkPacing.h"
#include "Core/Unicode/UTF8Encoder.h"
#include "PAL/DurableFile.h"
#include "PAL/Log.h"
#include "PAL/PAL.h"
#include "PAL/Stream.h"
#include "Platform/POSIX/ReceivedDescriptor.h"
#ifdef UBIQUITTY_QUIC
#include "Platform/Host/HostQUIC.h"
#endif

#include <dirent.h>
#include <sys/file.h>
#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

#ifdef __APPLE__
#include <libproc.h>
#endif
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/ucred.h>
#endif

#include "Core/Mouse/Mouse.h"
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace Ubiquitty {
namespace Host {

namespace Identity = Ubiquitty::Session::Identity;

namespace {

/// What a tokenless same-user local agent may do when the token requirement
/// is off: the scopes an `all` token carries, never FileRead or Manage.
constexpr Protocol::Sync::ScopeSet tokenlessLocalAgentScope { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive,
    Protocol::Sync::Scope::Control, Protocol::Sync::Scope::RegionRead, Protocol::Sync::Scope::NewSession };

/// Reads the exact revision a state-derived tree intent was confirmed against.
/// - Parameter request: The schema-checked call.
/// - Returns: A nonzero canonical decimal uint64, or no valid revision.
std::optional<std::uint64_t> expectedStructureRevision(const Protocol::Control::DecodedRequest& request)
{
    const auto* value = request.params.find("expectedRevision");
    if (!value || !value->isString())
        return std::nullopt;
    const auto& text = value->asString();
    if (text.empty() || text.size() > 20 || text.front() < '1' || text.front() > '9')
        return std::nullopt;
    std::uint64_t revision = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), revision);
    return parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size() ? std::optional { revision } : std::nullopt;
}

/// The object named by an admitted tree call; identity strings are validated
/// here before either asking for consent or applying the operation.
///
/// - Parameter request: A schema-validated structure request.
/// - Returns: Its object, or nothing for a malformed or unsupported identity.
std::optional<Identity::ObjectID> structureObjectOf(const Protocol::Control::DecodedRequest& request)
{
    if (request.method == "label.set" || request.method == "tags.set") {
        const auto id = Identity::parseObjectID(request.params.stringOr("object"));
        return id && id->kind != Identity::ObjectKind::Client ? id : std::nullopt;
    }
    if (request.method == "workspace.rename")
        return Identity::ObjectID { Identity::ObjectKind::Workspace, static_cast<std::uint32_t>(request.params.numberOr("workspace")) };
    if (request.method == "window.close" || request.method == "workspace.addTab" || request.method == "window.activate")
        return Identity::ObjectID { Identity::ObjectKind::Window, static_cast<std::uint32_t>(request.params.numberOr("window")) };
    if (request.method == "tab.balance" || request.method == "pane.adjustRatio" || request.method == "divider.setRatio"
        || request.method == "tab.rename" || request.method == "tab.move" || request.method == "workspace.closeTab" || request.method == "tab.activate" || request.method == "tab.extractWindow" || request.method == "tab.moveToPane")
        return Identity::ObjectID { Identity::ObjectKind::Tab, static_cast<std::uint32_t>(request.params.numberOr("tab")) };
    if (request.method == "workspace.addWindow" || request.method == "workspace.split" || request.method == "workspace.closePane" || request.method == "pane.focus" || request.method == "pane.zoom" || request.method == "window.moveFocus"
        || request.method == "pane.move" || request.method == "pane.swap" || request.method == "pane.extractTab" || request.method == "pane.extractWindow"
        || request.method == "pane.moveToTab" || request.method == "pane.moveToNewTab")
        return Identity::ObjectID { Identity::ObjectKind::Pane, static_cast<std::uint32_t>(request.params.numberOr("pane")) };
    return std::nullopt;
}

/// Resolves an untrusted binary path in one exact tab tree.
/// - Parameters:
///   - tree: The named tab, never a focused substitute.
///   - value: The request's bounded path array.
/// - Returns: The split's internal index, or nothing for malformed or leaf paths.
std::optional<int> requestedDivider(const Ubiquitty::Panes::Tree& tree, const Codec::JSON::Value* value)
{
    if (!value || !value->isArray() || value->items().size() > Ubiquitty::Panes::maxPanes - 1)
        return std::nullopt;
    std::array<std::uint8_t, Ubiquitty::Panes::maxPanes - 1> path {};
    std::size_t length = 0;
    for (const auto& child : value->items()) {
        if (!child.isNumber() || (child.asNumber() != 0 && child.asNumber() != 1))
            return std::nullopt;
        path[length++] = static_cast<std::uint8_t>(child.asNumber());
    }
    return Ubiquitty::Panes::dividerAtPath(tree, std::span { path }.first(length));
}

/// Whether a tree call sizes an explicitly named tab.
/// - Parameter method: The registry name.
/// - Returns: Whether the operation is one of the three sizing methods.
bool isLayoutSizingMethod(std::string_view method)
{
    return method == "tab.balance" || method == "pane.adjustRatio" || method == "divider.setRatio";
}

/// Validates a complete sizing selector without changing tab selection.
/// - Parameters:
///   - request: The schema-checked call.
///   - layout: The workspace owning its tab.
/// - Returns: Whether its window, pane or split still belongs to that tab.
bool hasObservedSizingTarget(const Protocol::Control::DecodedRequest& request, const Ubiquitty::Session::Layout::Workspace& layout)
{
    using namespace Ubiquitty::Session::Layout;
    const auto* tree = layout.paneTree(SurfaceID { static_cast<std::uint32_t>(request.params.numberOr("window")) }, TabID { static_cast<std::uint32_t>(request.params.numberOr("tab")) });
    if (!tree)
        return false;
    if (request.method == "divider.setRatio")
        return requestedDivider(*tree, request.params.find("path")).has_value();
    if (request.method == "pane.adjustRatio")
        return tree->holdsPane(static_cast<int>(static_cast<std::uint32_t>(request.params.numberOr("pane")))) && request.params.numberOr("step") > 0;
    return true;
}

/// Checks the exact pair without changing focus or looking up another workspace.
/// - Parameters:
///   - request: A schema-validated pane relocation.
///   - layout: The source pane's workspace.
/// - Returns: Whether both distinct panes remain in the named active tab.
bool hasObservedRelocationPair(const Protocol::Control::DecodedRequest& request, const Ubiquitty::Session::Layout::Workspace& layout)
{
    using namespace Ubiquitty::Session::Layout;
    const SurfaceID window { static_cast<std::uint32_t>(request.params.numberOr("window")) };
    const PaneID pane { static_cast<std::uint32_t>(request.params.numberOr("pane")) };
    const PaneID target { static_cast<std::uint32_t>(request.params.numberOr("target")) };
    return pane != target && layout.surfaceHolding(pane) == window && layout.surfaceHolding(target) == window
        && layout.tabHolding(window, pane) == layout.activeTab(window)
        && layout.tabHolding(window, target) == layout.activeTab(window);
}

/// Checks a transfer's complete source and destination without changing focus.
/// - Parameters:
///   - request: A schema-checked transfer request.
///   - layout: The source object's workspace.
/// - Returns: Whether the named containers and target belong to this workspace.
bool hasObservedTransfer(const Protocol::Control::DecodedRequest& request, const Ubiquitty::Session::Layout::Workspace& layout)
{
    using namespace Ubiquitty::Session::Layout;
    const SurfaceID window { static_cast<std::uint32_t>(request.params.numberOr("window")) };
    const PaneID pane { static_cast<std::uint32_t>(request.params.numberOr("pane")) };
    const TabID tab { static_cast<std::uint32_t>(request.params.numberOr("tab")) };
    const bool isTab = request.method == "tab.moveToPane";
    if (isTab ? !layout.paneTree(window, tab) : layout.surfaceHolding(pane) != window)
        return false;
    if (request.method == "pane.moveToNewTab")
        return layout.surface(SurfaceID { static_cast<std::uint32_t>(request.params.numberOr("targetWindow")) });
    const PaneID target { static_cast<std::uint32_t>(request.params.numberOr("target")) };
    const auto targetWindow = layout.surfaceHolding(target);
    if (targetWindow == invalidSurface)
        return false;
    const auto targetTab = layout.tabHolding(targetWindow, target);
    return isTab ? targetTab != tab : pane != target && targetTab == tab;
}

/// The pixels a `Viewport` says its client draws a pane in.
///
/// - Parameter viewport: The client's frame.
/// - Returns: The pixels, or `nullopt` when the client has measured none.
std::optional<Ubiquitty::PixelGeometry> pixelsOf(const Protocol::Sync::Viewport& viewport)
{
    if (viewport.cellPixelWidth == 0 || viewport.cellPixelHeight == 0)
        return std::nullopt;
    return Ubiquitty::PixelGeometry { { viewport.cellPixelWidth, viewport.cellPixelHeight },
        { viewport.textAreaPixelWidth, viewport.textAreaPixelHeight },
        { viewport.drawablePixelWidth, viewport.drawablePixelHeight } };
}

/// Whether a terminal draws at these pixels already.
bool holdsPixels(const Ubiquitty::Terminal& term, const Ubiquitty::PixelGeometry& pixels)
{
    return term.cellPixelWidth == pixels.cell.width && term.cellPixelHeight == pixels.cell.height
        && term.textAreaPixelWidth == pixels.textArea.width && term.textAreaPixelHeight == pixels.textArea.height
        && term.drawablePixelWidth == pixels.drawable.width && term.drawablePixelHeight == pixels.drawable.height;
}

/// This machine's `uname -sm`, the same key the bootstrap prints and a bundled
/// binary is chosen by.
///
/// - Returns: "<system> <machine>", or empty when uname fails.
std::string hostPlatformName()
{
    struct utsname identity {};
    if (::uname(&identity) != 0)
        return {};
    return std::string(identity.sysname) + " " + identity.machine;
}

std::uint64_t createDaemonInstanceID()
{
    PAL::UnixFileDescriptor entropy(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    if (!entropy) {
        return 0;
    }
    std::array<std::byte, sizeof(std::uint64_t)> bytes {};
    std::size_t filled = 0;
    while (filled < bytes.size()) {
        const auto count = ::read(entropy.get(), bytes.data() + filled, bytes.size() - filled);
        if (count > 0) {
            filled += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return 0;
        }
    }
    std::uint64_t identity = 0;
    std::memcpy(&identity, bytes.data(), bytes.size());
    return identity;
}

// SIGHUP requests a credential reload. The handler only sets this flag; the
// run loop re-reads the consent list and re-verifies token clients at a safe
// point. The handler installs without SA_RESTART so a
// SIGHUP-interrupted poll() returns EINTR and the loop re-checks the flag
// promptly.
volatile sig_atomic_t gTokenStoreChanged = 0;
void onTokenStoreSignal(int) { gTokenStoreChanged = 1; }

// SIGUSR1 asks the run loop to log why each client's input is or is not being
// read, at the same safe point and by the same flag-and-EINTR route.
volatile sig_atomic_t isHoldStateRequested = 0;
void onHoldStateSignal(int) { isHoldStateRequested = 1; }

// SIGTERM and SIGINT ask the daemon to stop, by the same flag-and-EINTR route.
// The run loop stops at a point where no pane is held for a handover and
// leaves by the tail a daemon whose last session ended takes: the workspace
// records are written for the next host to take in, and the socket and PID
// files are removed, so no client keeps dialling a socket nothing listens on.
volatile sig_atomic_t isStopRequested = 0;
void onStopSignal(int) { isStopRequested = 1; }

// SIGUSR2 asks a daemon that holds nothing to stop as SIGTERM stops it, so a
// start of the same build asks the older hosts beside it again
// (`Session::upgradeLocalHost`). One that holds a session, a workspace, an
// adoption or a handover keeps serving; the request is answered once no
// client is still greeting.
volatile sig_atomic_t isIdleStopRequested = 0;
void onIdleStopSignal(int) { isIdleStopRequested = 1; }

// Read chunk off a PTY per poll wake.
constexpr int ptyReadChunk = 65536;

// Defense-in-depth resource caps for the persistent, same-uid-reachable
// daemon: bound how many clients and sessions one connector can spawn, and
// reap a connection that never HELLOs (an accept-and-idle fd-exhaustion probe).
constexpr std::size_t maxClients = 64;
static_assert(maxClients == Ubiquitty::OSC::TextSizing::maxFaceOffersPerPane);
constexpr std::size_t maxSessions = 64;
/// The input every pane holds unwritten together: a connection's bound for
/// every client the host attaches. Input accepted from a lost link is kept to
/// be written, so connections that come and go would otherwise hold more than
/// live ones can.
constexpr std::size_t maxHostHeldInputBytes = maxClients * Protocol::Sync::maxConnectionHeldInputBytes;
static_assert(maxClients <= Protocol::Sync::maxInputInstances, "every client attached to a pane may hold a share of it");
// Retained evidence has a finite admission budget, never an eviction timer.
// The global metadata budget is the owner/session capacity product; exhaustion
// refuses new operations instead of removing evidence for a live authority.
constexpr std::size_t maxLocalAdoptOutcomes = maxClients * maxSessions;
constexpr std::size_t maxLocalAdoptRecordBytes = Protocol::Local::maxAdoptBodyBytes + Protocol::Local::adoptWirePrepareHeaderBytes;
constexpr int localAdoptMaintenanceMilliseconds = 1000;
// bindScreenPanes treats a refused bind as one still to come, and queueToPane
// holds an unbound pane's frames against the client's output cap; a connection
// asked to bind past its pane cap would hold them until the client is dropped.
// Every session fits one connection.
static_assert(maxSessions <= Protocol::Sync::maxPanesPerConnection);
// A workspace's structure names every pane in it and everyone attached to
// each: no workspace holds more panes than the host runs sessions, and no
// pane more attendees than the host has clients, so a record this host
// builds is one its encoder takes.
static_assert(maxSessions <= Ubiquitty::Session::maxStructureLeaves);
static_assert(maxClients <= Ubiquitty::Session::maxTreeAttendeesPerPane);
constexpr std::uint64_t helloGraceMilliseconds = 5000;
constexpr std::uint64_t sessionExitDrainMilliseconds = 5000;
// What a client draining a session exit must read per deadline window for the
// window to extend, and the most the windows may add up to. Without the floor
// a reader that takes one byte a window holds its slot, and the daemon whose
// last session is gone, for as long as its backlog lasts in bytes times
// windows; without the ceiling the slot lasts as long as the backlog does at
// the floor's rate, and the backlog is bounded only by the high-water mark.
constexpr std::size_t sessionExitMinimumDrainBytes = 64u * 1024u;
constexpr std::uint64_t maxSessionExitDrainMilliseconds = 12u * sessionExitDrainMilliseconds;
// How long a client dropped for its output backlog keeps its slot while the
// Bye that says so drains. What it still has to read is at most one frame and
// the Bye; a reader that cannot take that in this long is not reading.
constexpr std::uint64_t backlogByeDrainMilliseconds = 2000;

// The ssh a HELLO's sshHost gets connected by. A bare name, not a path: the
// child resolves it against $PATH inside openPTY (execve does no search of its
// own), which is the same resolution the login shell in every other session
// gets, and it keeps a second install-prefix list out of this file.
constexpr const char* remoteLaunchProgram = "ssh";

// Partial inbound-frame budget. Client→host frames
// (HELLO/Sync/control/ADOPT) are usually small, but a transfer can reach the
// carrier ceiling. Local clipboard requests have their own smaller bound. The
// per-client cap must admit one complete max-size frame mid-transfer —
// otherwise a legitimate frame climbs past a tighter cap while still
// incomplete and its client is dropped mid-frame. So the per-client cap is
// maxClientFramePayloadLength + the frame header + headroom: one in-flight max frame
// fits, and the per-client cap does not double as anti-abuse. The anti-abuse
// mechanisms are the 10 s completion deadline (catches a slow-trickle peer)
// and the aggregate cap (catches many concurrent peers each holding a max frame).
// Sized off the client frame bound rather than maxPayloadLength, which grows
// with the screens this daemon sends: no client frame is larger than the
// bound, and one that claims to be is dropped here before the decoder holds
// it.
constexpr std::size_t maxClientInboundBytes = Protocol::Sync::maxClientFramePayloadLength + 64u * 1024u; // one max frame + header + headroom
// Aggregate DoS bound across all clients: 4 concurrent max-frame transfers'
// worth. Bounds total daemon partial-frame memory regardless of how
// many of the up-to-maxClients connections trickle a large frame at once;
// exceeding it drops peers (newest-first via the loop) until under budget.
// An adopt's parts are held outside the decoder until its ADOPT_SESSION
// arrives, up to maxAdoptBodyBytes for one local client, and count here: the
// cap admits one whole adopt beside the four transfers.
constexpr std::size_t maxAggregateInboundBytes = 4u * maxClientInboundBytes + Protocol::Local::maxAdoptBodyBytes;
// A stalled partial frame's TTL. Ten seconds is the shipping value; a test that
// asserts the deadline fires has to wait it out, and there are three such waits
// in one stanza. UBIQUITTY_PARTIAL_FRAME_DEADLINE_MS shortens it for that case
// only — the daemon reads it once at startup, so a client cannot move the
// deadline it is being held to.
static std::uint64_t partialFrameDeadline()
{
    static const std::uint64_t value = [] {
        const char* override = ::getenv("UBIQUITTY_PARTIAL_FRAME_DEADLINE_MS");
        if (!override || !*override)
            return std::uint64_t { 10000 };
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(override, &end, 10);
        // A junk or zero value keeps the shipping deadline rather than
        // disabling the mechanism the variable exists to exercise.
        if (end == override || *end != '\0' || parsed == 0)
            return std::uint64_t { 10000 };
        return static_cast<std::uint64_t>(parsed);
    }();
    return value;
}

// QUIC bootstrap tokens: ours are 32 bytes; the preamble length byte
// allows up to 255, so cap to reject a junk preamble early. maxQUICTokenLength
// is the cap the Sync bootstrap parser enforces (Protocol/Sync/QUICBootstrap.h);
// the daemon shares it so mint and verify agree. Bound the unconsumed set so
// mint requests can't grow it without limit.
using Protocol::Sync::maxQUICTokenLength;
static_assert(maxQUICTokenLength == Protocol::Sync::maxPreambleTokenBytes, "a bootstrap token the mint may issue is one a preamble carries");
constexpr std::size_t maxPendingQUICTokens = 64;
// A bootstrap token is a short-lived handoff over the trusted channel; an
// unused one expires so a leaked/stale-transcript token can't authorize a
// later QUIC connection. The dialer is expected to connect within this window.
constexpr std::uint64_t quicTokenTTLMilliseconds = 60000;

// How long an ends-when-detached session outlives a lost transport. A dropped
// connection is not a decision to end the session: a laptop lid, a network
// move, and a client restart each take seconds to tens of seconds, and the
// reattach that follows has to find the session still there. Two minutes covers
// those and still ends a session nobody comes back to. A clean detach (EXIT) is
// the decision, and waits for nothing.
constexpr std::uint64_t detachedSessionGraceMilliseconds = 120000;

// How long the record of the named workspaces waits after a change before it is
// written. A burst of verbs, or a person typing commands, is one write; and a
// machine shutting down kills the shells inside it, before the host, so the
// arrangement the file holds is the one from before the shutdown began.
constexpr std::uint64_t layoutRecordDebounceMilliseconds = 1000;

// How long a session whose terminal hung up waits for its child's exit status
// before it is reaped without one, and how often the loop looks in between.
constexpr std::uint64_t exitStatusWaitMilliseconds = 200;
constexpr std::uint64_t exitStatusPollMilliseconds = 10;

// The most entries of the control directory read while looking for the record
// files of hosts on sibling sockets that are no longer running.
constexpr std::size_t maxSiblingLayoutRecordScan = 256;

// How long a host question waits for a person before the daemon retires it
// unanswered and refuses for itself: a held agent attach (reason = timeout)
// and every capability question.
constexpr std::uint64_t consentTimeoutMilliseconds = 30000;
// Bound the persisted always-allow list.
constexpr std::size_t maxAlwaysAllowedAgents = maxStandingConsentGrants;

// How many capability prompts may be outstanding at once.
//
// A record is what a request id means, and it is removed when a human rules on
// it, when its session ends, or when the grant is revoked. None of those is
// guaranteed to happen: an agent reconnecting under a fresh identity each time
// yields a fresh triple, a fresh prompt and a record nothing prunes.
//
// A count rather than a deadline, though both bound it. A deadline drops a
// prompt while the tree is idle and nothing is wrong — a human who takes eleven
// minutes loses the question they were reading. A count drops one only when a
// 257th arrives, which is a flood, and in a flood something has to give.
constexpr std::size_t maxPendingCapabilityGrants = 256;
// A recipient can owe at most the questions already outstanding when its
// first retirement met backpressure. No new question is admitted until empty.
constexpr std::size_t maxPendingConsentRetirements = maxPendingCapabilityGrants + maxClients / 2 + Protocol::Sync::maxPendingStructureConsents;

std::uint64_t nowMilliseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

// The same clock at the resolution the coherence rule counts in.
std::uint64_t nowMicroseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000ull;
}

// The zero-timeout poll the coherence rule's read quiescence asks for: whether
// the program wrote more since the pass that ended at a would-block.
Runtime::ReadReadiness ptyReadiness(PAL::Stream* stream)
{
    if (!stream)
        return Runtime::ReadReadiness::Quiet;
    struct pollfd descriptor { stream->fileDescriptor(), POLLIN, 0 };
    return ::poll(&descriptor, 1, 0) > 0 && (descriptor.revents & POLLIN) ? Runtime::ReadReadiness::Pending : Runtime::ReadReadiness::Quiet;
}

// Clamp a remote HELLO or viewport dimension to the Core sanity ceiling
// (VT100Constants maxColumns/maxRows) before it reaches the PTY (TIOCSWINSZ)
// or Terminal::resize, so the child process and the Core model agree on geometry.
// Terminal::resize clamps internally too, but the PTY does not — without this an
// over-limit wire value reaches the child raw.
int clampColumns(int columns) { return columns > maxColumns ? maxColumns : columns; }
int clampRows(int rows) { return rows > maxRows ? maxRows : rows; }

// Foreground-probe budget for typed records (TerminalRuntime's
// foregroundProbeIntervalTicks twin for daemon-hosted sessions): ~20 Hz worst
// case per session. Consumers of foreground facts need ~20 Hz freshness at most.
constexpr std::uint64_t foregroundProbeIntervalMilliseconds = 50;

// How often a pane's records are read from its terminal at most: the probe's
// budget, since the two change together and a listing needs no fresher.
constexpr std::uint64_t paneRecordsIntervalMilliseconds = foregroundProbeIntervalMilliseconds;

// How often a pane's activity stamps move its facts by themselves at most. A
// pane that floods stamps every drain pass; a reader shows "output just now"
// from a stamp a second old as well as from a newer one.
constexpr std::uint64_t activityStampIntervalMilliseconds = 1000;

// How often a pane only a listing reads has its records read at most: the
// listing's cadence, the one a connection sends a listed pane's records at.
constexpr std::uint64_t listedRecordsIntervalMilliseconds = Protocol::Sync::listedRecordsIntervalMicroseconds / 1000;

// Region read.

// Bound a single region read so a hostile request can't make the daemon
// build an unbounded string.
constexpr std::size_t maxRegionBytes = 1u << 20; // 1 MiB

// Extract inclusive absolute rows [absStart, absEnd] as UTF-8 text, one
// line per row with trailing blanks trimmed. Clamps the range to the live
// buffer (writing back the actual range) and caps the output size — if the
// cap is hit, absEnd is lowered to the last row emitted.
void extractRegionText(const Ubiquitty::Terminal& t, int& absStart, int& absEnd, std::string& out)
{
    const int total = Ubiquitty::Screen::scrollbackCount(&t) + t.rows;
    if (absStart < 0)
        absStart = 0;
    if (absEnd >= total)
        absEnd = total - 1;
    out.clear();
    if (absStart > absEnd)
        return;
    const int columns = t.columns;
    for (int row = absStart; row <= absEnd; ++row) {
        // Core's row text, not a second walk of the same cells. A walk that
        // drops every cell whose code point is zero closes up a row laid out
        // by cursor addressing — a status bar or a table reads as one word,
        // while every frontend rendering those rows through Core shows the
        // reader the gaps. And a sized block's lower rows carry their glyph
        // for the renderer's sake, so a walk that does not know the rule
        // hands an agent the block once per row it covers.
        const std::string line = Ubiquitty::Screen::absoluteRowText(t, row, columns);
        out += line;
        out.push_back('\n');
        if (out.size() >= maxRegionBytes) {
            absEnd = row; // report only what we actually returned
            break;
        }
    }
}

// Derive the most recent command's OUTPUT abs-row range from OSC-133 marks:
// the last OUTPUT_START (C) at or before the cursor, up to the row before
// the next PROMPT_START (A) — or the cursor row if the command is still
// running. Returns false (→ status no-command) when no output mark exists.
bool lastCommandRange(const Ubiquitty::Terminal& t, int& outStart, int& outEnd)
{
    const int total = Ubiquitty::Screen::scrollbackCount(&t) + t.rows;
    int bottom = Ubiquitty::Screen::scrollbackCount(&t) + (t.screens.active ? t.screens.active->cursorY : 0);
    if (bottom >= total)
        bottom = total - 1;
    int cRow = -1;
    for (int row = bottom; row >= 0; --row)
        if (Ubiquitty::Screen::rowMark(&t, row) & Screen::osc133MarkOutputStart) {
            cRow = row;
            break;
        }
    if (cRow < 0)
        return false;
    int endRow = bottom;
    for (int row = cRow + 1; row <= bottom; ++row)
        if (Ubiquitty::Screen::rowMark(&t, row) & Screen::osc133MarkPromptStart) {
            endRow = row - 1;
            break;
        }
    if (endRow < cRow)
        endRow = cRow;
    outStart = cRow;
    outEnd = endRow;
    return true;
}

// File transfer (click-to-open).

// Cap on total pasted uploads landed per client connection (Client::pastePushCount):
// each landed push writes a temp file, and a hostile driver looping pushes is a
// full disk. A user pastes tens of images a session, not thousands.
constexpr uint32_t maxPastePushesPerClient = 256;

// Clipboard requests one client may have outstanding at once.
//
// A program can ask faster than a person can approve a pasteboard read, and
// every unanswered request is state the daemon holds on the client's behalf.
// Small because a legitimate caller has one question in flight: a probe, then a
// read.
constexpr std::size_t maxPendingClipboardRequestsPerClient = 8;

// Cap on total file.read calls served per client connection (see Client::fileRequestCount).
constexpr uint32_t maxFileRequestsPerClient = 256;

// Confine file.read to the session's own current-working-directory
// subtree: realpath() both the request and the tracked cwd (resolving every
// symlink) and require the former to be a proper descendant of the latter.
// No escape hatch — a path outside cwd is denied outright, no consent
// round-trip (the click-to-open use case — click a path from `ls`/compiler
// output — is always within cwd). On success `outRealPath` is the fully-resolved path
// the caller opens.
//
// The type (regular-file) and size checks belong on the fd the caller actually
// opens, never on a pre-open path stat.
// A same-uid process racing inside the confined subtree could swap the
// realpath'd regular file for a different inode between this check and the
// open — a stat here would validate the wrong inode, and worse, a swap to a
// FIFO would make the caller's blocking open() wedge the client-handling
// thread (an availability bug). So the caller opens O_NOFOLLOW | O_NONBLOCK,
// then fstat()s the fd for S_ISREG + the size. Confinement via
// realpath stays the primary control; O_NOFOLLOW is the hard guarantee against
// a symlink re-introduced on the final component after this check.
// The extension a landed paste carries, from its MIME. Exact lowercase
// matches: every sender is one of this product's own frontends, and they
// stage bare lowercase types; an unmatched flavour lands as the generic.
const char* pasteExtensionForMime(std::string_view mime)
{
    if (mime == "image/png")
        return ".png";
    if (mime == "image/jpeg")
        return ".jpg";
    if (mime == "image/gif")
        return ".gif";
    if (mime == "image/tiff")
        return ".tiff";
    if (mime == "application/pdf")
        return ".pdf";
    if (mime == "text/html")
        return ".html";
    if (mime == "text/plain")
        return ".txt";
    return ".bin";
}

// Lands one pasted payload as a host temp file the foreground program can
// open, its extension named by the MIME so what the path claims is what the
// bytes are. mkstemps creates it user-only in the standard temp directory —
// the payload is the user's own paste on their own host.
Protocol::Control::PastePushStatus landPastePush(std::string_view mime, std::span<const std::uint8_t> bytes, std::string& outLandedPath)
{
    const char* extension = pasteExtensionForMime(mime);
    const char* tmp = ::getenv("TMPDIR");
    std::string pathTemplate = (tmp != nullptr && *tmp != '\0') ? tmp : "/tmp";
    if (pathTemplate.back() != '/')
        pathTemplate += '/';
    pathTemplate += "ubiquitty-paste-XXXXXX";
    pathTemplate += extension;
    std::vector<char> buffer(pathTemplate.begin(), pathTemplate.end());
    buffer.push_back('\0');
    PAL::UnixFileDescriptor fileDescriptor(::mkstemps(buffer.data(), static_cast<int>(std::strlen(extension))));
    if (!fileDescriptor)
        return Protocol::Control::PastePushStatus::IOError;
    // Ensure the paste file is owner-only regardless of the process umask.
    ::fchmod(fileDescriptor.get(), 0600);
    std::size_t written = 0;
    while (written < bytes.size()) {
        // No EINTR retry: mkstemps hands back a regular file, and a write to
        // one is not interruptible — the arm would be unreachable rather than
        // defensive.
        const ssize_t n = ::write(fileDescriptor.get(), bytes.data() + written, bytes.size() - written);
        if (n <= 0) {
            ::unlink(buffer.data()); // a half-written paste is worse than none
            return Protocol::Control::PastePushStatus::IOError;
        }
        written += static_cast<std::size_t>(n);
    }
    outLandedPath.assign(buffer.data());
    return Protocol::Control::PastePushStatus::Landed;
}

bool resolveConfinedFile(const std::string& requestedPath, const std::string& cwd, Protocol::Sync::FileDataStatus& outStatus, std::string& outRealPath)
{
    char resolvedCwd[PATH_MAX];
    if (cwd.empty() || ::realpath(cwd.c_str(), resolvedCwd) == nullptr) {
        outStatus = Protocol::Sync::FileDataStatus::OutsideConfinement;
        return false;
    }
    char resolvedPath[PATH_MAX];
    if (requestedPath.empty() || ::realpath(requestedPath.c_str(), resolvedPath) == nullptr) {
        outStatus = Protocol::Sync::FileDataStatus::NotFound;
        return false;
    }
    const std::string cwdString(resolvedCwd);
    const std::string pathString(resolvedPath);
    // A proper descendant only — require a '/' boundary so a sibling directory
    // sharing the cwd's string prefix (e.g. "/home/user2" vs "/home/user")
    // can't pass confinement.
    const bool isInsideCwd = pathString.starts_with(cwdString) && pathString.size() > cwdString.size() && pathString[cwdString.size()] == '/';
    if (!isInsideCwd) {
        outStatus = Protocol::Sync::FileDataStatus::OutsideConfinement;
        return false;
    }
    outRealPath = pathString;
    outStatus = Protocol::Sync::FileDataStatus::OK;
    return true;
}

} // namespace

// Defined here rather than in the header: the members' destructors need the
// complete types the header only forward-declares.
Daemon::Session::Session(const Ubiquitty::CapabilityDecisionTable& capabilityDecisions)
    : capabilityPolicy(capabilityDecisions)
{
}

Daemon::Session::~Session()
{
    // The pasted bytes are the user's, and nothing else removes them: the path
    // was handed to the program rather than kept by the daemon, so teardown is
    // the last moment anyone knows the file exists. Best-effort — a path already
    // gone (the program moved it, a sweeper took it) is the expected case, not
    // an error worth reporting to a destructor's caller.
    if (ownsLandedPastePaths) {
        for (const std::string& path : landedPastePaths)
            ::unlink(path.c_str());
    }
}

Daemon::LocalAdoptCandidate::LocalAdoptCandidate(Daemon& owner, std::unique_ptr<Session> sessionOwner, std::unique_ptr<HostedWorkspace> workspaceOwner, PAL::PTYAdoptionIdentity identity)
    : daemon(owner)
    , m_target { sessionOwner->id, sessionOwner->screenSource->paneID() }
    , endpoint(identity)
    , session(std::move(sessionOwner))
    , workspace(std::move(workspaceOwner))
{
    ++daemon.m_preparedLocalAdopts;
}

Daemon::LocalAdoptCandidate::~LocalAdoptCandidate()
{
    assert(daemon.m_preparedLocalAdopts);
    --daemon.m_preparedLocalAdopts;
}

Daemon::Daemon()
    : m_trustedLocalBinaries(resolveTrustedLocalBinaries(resolveSelfExecutablePath()))
    , m_capabilityGrantFile(std::make_unique<CapabilityGrantFile>())
    , m_daemonInstanceID(createDaemonInstanceID())
    , m_hostInstanceID { .low = createDaemonInstanceID(), .high = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) }
    , m_attachments(std::make_unique<Protocol::Sync::AttachmentTable>(m_hostInstanceID, Protocol::Sync::measuredResumePolicy(), m_attachmentSecrets))
{
    wireAttachmentRetirement();
    static_assert(localAdoptAuthorityCapacity == maxClients);
    const auto policy = Ubiquitty::Session::localShareHandoffLimits();
    constexpr std::uint64_t bytesPerMebibyte = 1024 * 1024;
    constexpr std::uint64_t bodyMebibytes = (Protocol::Local::maxAdoptBodyBytes - 1) / bytesPerMebibyte + 1;
    // Two complete maximum-size answer allowances permit preparation and its
    // immediate Commit. Expiry still means inactive, never guessed ownership.
    constexpr auto halfTimeRange = std::numeric_limits<std::uint64_t>::max() / 2;
    if (policy.silenceMicroseconds && policy.silenceMicroseconds < halfTimeRange
        && policy.applyMicrosecondsPerMebibyte <= (halfTimeRange - policy.silenceMicroseconds) / bodyMebibytes) {
        const auto lease = 2 * (policy.silenceMicroseconds + bodyMebibytes * policy.applyMicrosecondsPerMebibyte);
        if (auto transactions = Ubiquitty::Session::AdoptTransaction::create(m_daemonInstanceID,
                { .authorityCapacity = m_localAdoptAuthorities.size(), .outcomeCapacity = maxLocalAdoptOutcomes, .preparedLeaseMicroseconds = lease }))
            m_localAdoptTransactions = std::move(*transactions);
    }
}

Protocol::Sync::ID128 Daemon::EntropySecrets::nextSecret()
{
    // A resume proves itself with this secret, so it is drawn from the
    // system's entropy and never derived from what a peer can see. A refused
    // draw leaves a secret of zero, which the table compares like any other:
    // no resume presents it unless a Welcome issued it.
    std::array<std::uint8_t, 16> bytes {};
    if (::getentropy(bytes.data(), bytes.size()) != 0)
        return {};
    Protocol::Sync::ID128 secret;
    for (std::size_t byte = 0; byte < 8; ++byte) {
        secret.low |= std::uint64_t { bytes[byte] } << (8 * byte);
        secret.high |= std::uint64_t { bytes[8 + byte] } << (8 * byte);
    }
    return secret;
}

std::string Daemon::principalOf(const Client& client) const
{
    if (client.hasToken && !client.tokenID.empty())
        return "token:" + client.tokenID;
    if (originOf(client) == Ubiquitty::CapabilityOrigin::Local)
        return "uid:" + std::to_string(::getuid());
    return "connection:" + std::to_string(client.clientID);
}

void Daemon::setConfiguration(const Preferences::Values& configuration)
{
    m_configuration = configuration;
    m_capabilityDecisions = Preferences::Effective::capabilityDecisions(m_configuration);
    Themes::registerThemesFromValues(m_configuration);
}

void Daemon::setShellIntegrationDirectory(std::string directory)
{
    m_shellIntegrationDirectory = std::move(directory);
}

void Daemon::setSessionFileCreationMask(std::uint32_t mask)
{
    m_sessionFileCreationMask = mask & 0777;
}

Daemon::~Daemon()
{
    teardownAll();
    cleanup();
}

bool Daemon::acquireStartupLock(const char* socketPath)
{
    // Held for the daemon's lifetime, beside the socket it guards. The probe
    // below cannot tell a stale socket from a live daemon's bind-before-listen
    // window — both refuse a connect — so two starters racing can each read
    // "stale" and the loser unlinks the winner's socket, leaving a daemon
    // listening on an inode nothing can reach. The lock is what makes the
    // probe's answer trustworthy: whoever holds it is the only process in that
    // window.
    std::string lockPath = std::string(socketPath) + ".lock";
    PAL::UnixFileDescriptor lockFileDescriptor(::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!lockFileDescriptor)
        return true; // cannot lock (read-only or exotic filesystem): proceed without it
    if (::flock(lockFileDescriptor.get(), LOCK_EX | LOCK_NB) != 0) {
        // EWOULDBLOCK is another starter mid-flight, which is the case this
        // exists for. Anything else — and flock is advisory-at-best on a
        // network home directory — is not a reason to refuse to start.
        if (errno == EWOULDBLOCK)
            return false;
        return true;
    }
    m_startupLockFileDescriptor = std::move(lockFileDescriptor);
    return true;
}

bool requiresTokenFor(const char* value)
{
    return value == nullptr || std::string_view(value) != "0";
}

std::size_t maximumSocketPathLength()
{
    struct sockaddr_un addr;
    return sizeof(addr.sun_path) - 1;
}

const char* describeBindFailure(BindFailure failure)
{
    switch (failure) {
    case BindFailure::None:
        return "no failure";
    case BindFailure::PathTooLong:
        return "socket path is longer than this platform can bind";
    case BindFailure::LockHeld:
        return "another daemon is starting on this socket path";
    case BindFailure::SocketCreateFailed:
        return "could not create the socket";
    case BindFailure::AddressInUse:
        return "another daemon is already listening on this socket";
    case BindFailure::BindFailed:
        return "could not bind the socket";
    case BindFailure::ListenFailed:
        return "could not listen on the socket";
    }
    return "unknown failure";
}

bool Daemon::listenAsOwner(int descriptor)
{
    // Backlog matches the client cap: a near-simultaneous burst up to maxClients
    // must be absorbed at accept, not refused below the cap by a short backlog.
    // Admission stays bounded by the >= maxClients checks after accept.
    return ::listen(descriptor, static_cast<int>(maxClients)) == 0;
}

BindFailure Daemon::bindSocket(const char* path)
{
    struct sockaddr_un addr;
    addr = {};
    addr.sun_family = AF_UNIX;
    // Validated before the lock is taken: acquireStartupLock creates
    // <path>.lock, so a path this platform cannot bind would otherwise leave a
    // lock file beside a socket that never existed.
    if (std::strlen(path) >= sizeof(addr.sun_path))
        return BindFailure::PathTooLong;

    if (!acquireStartupLock(path))
        return BindFailure::LockHeld;

    PAL::UnixFileDescriptor fileDescriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fileDescriptor)
        return BindFailure::SocketCreateFailed;

    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    // A stale socket from a crashed daemon blocks bind with EADDRINUSE. Probe
    // it: if nothing is listening (connect refused), unlink and retry. If a
    // live daemon answers, we lose the race and bail.
    if (::bind(fileDescriptor.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno == EADDRINUSE) {
            PAL::UnixFileDescriptor probe(::socket(AF_UNIX, SOCK_STREAM, 0));
            const bool alive = probe && ::connect(probe.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
            if (alive)
                return BindFailure::AddressInUse; // another daemon owns it
            ::unlink(path);
            if (::bind(fileDescriptor.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0)
                return BindFailure::BindFailed;
        } else {
            return BindFailure::BindFailed;
        }
    }
    ::chmod(path, 0600); // owner-only socket perms
    if (!listenAsOwner(fileDescriptor.get())) {
        ::unlink(path);
        return BindFailure::ListenFailed;
    }

    int flags = ::fcntl(fileDescriptor.get(), F_GETFL, 0);
    if (flags != -1)
        ::fcntl(fileDescriptor.get(), F_SETFL, flags | O_NONBLOCK);

    m_listenFileDescriptor = std::move(fileDescriptor);
    m_socketPath = path;
    return BindFailure::None;
}

std::string Daemon::socketBasename() const
{
    const auto slash = m_socketPath.find_last_of('/');
    return (slash == std::string::npos) ? m_socketPath : m_socketPath.substr(slash + 1);
}

void Daemon::writeProcessIDFile(const char* path)
{
    m_processIDFilePath = path;
    FILE* f = ::fopen(path, "w");
    if (!f)
        return;
    std::fprintf(f, "%ld\n", static_cast<long>(::getpid()));
    ::fclose(f);
}

void Daemon::captureLocalAdoptExecutables()
{
    if (auto executables = LocalAdoptExecutables::capture(resolveSelfExecutablePath()))
        m_trustedAdoptExecutables.emplace(std::move(*executables));
}

bool Daemon::start(const char* socketPath, const char* processIDFilePath, const char* shellPath, int columns, int rows)
{
    if (columns <= 0)
        columns = 80;
    if (rows <= 0)
        rows = 24;

    // Every signal the daemon answers is handled before its socket answers:
    // whoever reaches the socket may signal the process serving it.
    // SIGPIPE on a dead client write must not kill the daemon; we detect dead
    // fds via the write return.
    ::signal(SIGPIPE, SIG_IGN);

    // SIGHUP → re-read the consent list and re-verify token clients.
    // No SA_RESTART so the run loop's poll()
    // returns EINTR and re-checks the flag without waiting for its next
    // timeout.
    {
        struct sigaction sa {};
        sa.sa_handler = onTokenStoreSignal;
        ::sigaction(SIGHUP, &sa, nullptr);
    }
    {
        struct sigaction sa {};
        sa.sa_handler = onHoldStateSignal;
        ::sigaction(SIGUSR1, &sa, nullptr);
    }
    {
        struct sigaction sa {};
        sa.sa_handler = onStopSignal;
        ::sigaction(SIGTERM, &sa, nullptr);
        ::sigaction(SIGINT, &sa, nullptr);
    }
    {
        struct sigaction sa {};
        sa.sa_handler = onIdleStopSignal;
        ::sigaction(SIGUSR2, &sa, nullptr);
    }

    m_bindFailure = bindSocket(socketPath);
    if (m_bindFailure != BindFailure::None)
        return false;
    captureLocalAdoptExecutables();
    // Opened while descriptors are plentiful, because the moment it is needed
    // is the moment one cannot be had.
    m_reserveFileDescriptor = PAL::UnixFileDescriptor(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    writeProcessIDFile(processIDFilePath);

    m_shellPath = (shellPath != nullptr) ? shellPath : "/bin/sh";
    m_columns = columns;
    m_rows = rows;
    (void)loadAlwaysAllowedAgents(); // persisted agent-attach consent grants
    loadAlwaysAllowedCapabilityGrants(); // persisted capability Always grants
    // Capability-token store: same control directory as the consent file.
    {
        const auto slash = m_socketPath.find_last_of('/');
        const std::string directory = (slash == std::string::npos) ? std::string(".") : m_socketPath.substr(0, slash);
        m_tokenPath = directory + "/tokens";
    }
    // Token enforcement is ON by default — a tokenless NON-Local peer
    // (Remote over QUIC, or a non-bridge local Agent) fails closed at the HELLO
    // gate. The uid-gated Local attach bridge is exempt (originOf==Local), so the
    // macOS GUI is unaffected. UBIQUITTY_REQUIRE_TOKEN=0 lets a same-user local
    // agent in without one (development and tests); see tokenScopeAllows.
    m_requireToken = requiresTokenFor(::getenv("UBIQUITTY_REQUIRE_TOKEN"));
    // Read while the startup lock is held, so no other host on this socket is
    // writing the file, and before any client can ask for what it holds.
    m_layoutRecordPath = m_socketPath + ".layouts";
    loadLayoutRecords();
    m_backgroundEventPath = m_socketPath + ".background-events";
    loadBackgroundEvents();
    // Sessions are spawned on demand by the first HELLO (sessionID 0).
    return true;
}

void Daemon::cleanup()
{
    m_listenFileDescriptor.reset();
    // A daemon that handed over leaves every socket path it served bound to
    // the successor, which now accepts on them.
    if (!m_socketPath.empty()) {
        if (!m_hasRelinquishedService)
            ::unlink(m_socketPath.c_str());
        m_socketPath.clear();
    }
    for (auto& listener : m_inheritedListeners) {
        if (!m_hasRelinquishedService)
            ::unlink(listener.path.c_str());
    }
    m_inheritedListeners.clear();
    if (!m_processIDFilePath.empty()) {
        ::unlink(m_processIDFilePath.c_str());
        m_processIDFilePath.clear();
    }
}

void Daemon::teardownAll()
{
    m_pendingLocalAdopt.reset();
    m_pendingLocalAdoptReply.reset();
    m_localAdoptTransactions.reset();
    for (auto& record : m_localAdoptAuthorities)
        record = {};
    for (std::size_t i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].syncConnection) {
            m_clients[i].syncConnection->finish();
            drainScreenInteractive(i);
        }
        flushClient(i);
    }
    for (auto& c : m_clients) {
        c.fileDescriptor.reset();
        c.pendingAdoptFileDescriptor.reset();
    }
    m_clients.clear();
    m_sessions.clear(); // ~Session disposes each Terminal; the stream closes
    ++m_backgroundInventoryRevision;
}

// A session's remote input reaches its program through the daemon, which
// stamps the driver's origin and applies a pending resize first.
class Daemon::SessionInputSink final : public Runtime::ByteSink {
public:
    SessionInputSink(Daemon& daemon, Session& session)
        : m_daemon(daemon)
        , m_session(session)
    {
    }

    [[nodiscard]] Protocol::Sync::WriteStatus writeAll(Protocol::Sync::ID128 clientInstance, std::span<const std::uint8_t> bytes) override
    {
        return writeOwned({}, clientInstance, bytes);
    }

    [[nodiscard]] Protocol::Sync::WriteStatus writeOwned(Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance, std::span<const std::uint8_t> bytes) override
    {
        if (authority != Protocol::Sync::ID128 {} && !m_daemon.mayWriteRemoteInput(authority, clientInstance))
            return Protocol::Sync::WriteStatus::Withdrawn;
        // What the program is not taking stays in the pane, numbered and
        // bounded, never in the stream's queue: the pane's ledger then counts
        // written only what the stream had room to hand on, a paste's window
        // is measured against the program and not against a queue that takes
        // megabytes, and an interrupt finds the unwritten input where each
        // client can be told what became of its own. A signal is the one
        // write the stream is handed regardless: it lets it past what it
        // holds (Docs/architecture.md, "an interrupt does not queue").
        if (m_session.stream && m_session.stream->hasPendingWrite() && signalEffect(bytes) == Protocol::Sync::SignalEffect::None)
            return Protocol::Sync::WriteStatus::Refused;
        return m_daemon.writeRemoteInput(m_session, authority, clientInstance, bytes);
    }

    [[nodiscard]] std::size_t pendingBytes(Protocol::Sync::ID128 authority) const override
    {
        if (authority.low > std::numeric_limits<std::uint64_t>::max() / 2)
            return 0;
        return m_session.stream ? m_session.stream->pendingOwnedWriteBytes(authority.low * 2 + 1).value_or(0) : 0;
    }

    [[nodiscard]] Protocol::Sync::WriteStatus writePasteOwned(Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance, std::span<const std::uint8_t> bytes) override
    {
        if (authority != Protocol::Sync::ID128 {} && !m_daemon.mayWriteRemoteInput(authority, clientInstance))
            return Protocol::Sync::WriteStatus::Withdrawn;
        if (m_session.stream && m_session.stream->hasPendingWrite())
            return Protocol::Sync::WriteStatus::Refused;
        return m_daemon.writeRemoteInput(m_session, authority, clientInstance, bytes, RemoteInputLane::Paste);
    }

    void withdraw(Protocol::Sync::ID128 authority, std::span<const char> repair) override
    {
        if (!m_session.stream || authority == Protocol::Sync::ID128 {} || authority.low > std::numeric_limits<std::uint64_t>::max() / 2)
            return;
        (void)m_session.stream->discardPendingWrite(authority.low * 2);
        if (!m_session.stream->repairPendingWrite(authority.low * 2 + 1, repair)) {
            // Never let a failed repair leave revoked bytes available to a
            // later drain. The stream is no longer safe to feed.
            (void)m_session.stream->discardPendingWrite(authority.low * 2 + 1);
            m_session.exitReason = "pty refused paste delimiter repair after input withdrawal";
            m_session.shellExited = true;
        }
    }

    [[nodiscard]] Protocol::Sync::SignalEffect signalEffect(std::span<const std::uint8_t> bytes) override
    {
        // The line discipline the stream reads and the echo context reports:
        // one source for what a signal character is and does.
        const std::optional<PAL::LineDiscipline> discipline = m_session.stream ? m_session.stream->process().lineDiscipline() : std::nullopt;
        if (!discipline || !discipline->isSignalEnabled)
            return Protocol::Sync::SignalEffect::None;
        const auto isSignal = [&](std::uint8_t byte) {
            return discipline->interruptCharacter == std::optional { byte } || discipline->quitCharacter == std::optional { byte } || discipline->suspendCharacter == std::optional { byte };
        };
        if (std::ranges::none_of(bytes, isSignal))
            return Protocol::Sync::SignalEffect::None;
        return discipline->isInputDiscardedOnSignal ? Protocol::Sync::SignalEffect::SignalsAndDiscards : Protocol::Sync::SignalEffect::Signals;
    }

private:
    Daemon& m_daemon;
    Session& m_session;
};

// Session registry.

Daemon::Session* Daemon::createSession(int columns, int rows, std::span<const std::string> command, int adoptFileDescriptor, uint32_t adopterProcessID,
    std::optional<Ubiquitty::Session::Layout::RootIdentity> placement, std::string_view creatorGrantKey, std::string_view initialDirectory,
    std::unique_ptr<Ubiquitty::Terminal> authoritativeTerminal)
{
    auto session = constructSession(columns, rows, command, adoptFileDescriptor, adopterProcessID, placement, creatorGrantKey, initialDirectory, std::move(authoritativeTerminal));
    if (session) {
        // Ordinary creation cannot consume pointer capacity reserved by any
        // inactive candidate; Commit must not allocate a registry backing.
        m_sessions.reserve(m_sessions.size() + m_preparedLocalAdopts + 1);
        m_workspaces.reserve(m_workspaces.size() + m_preparedLocalAdopts + 1);
    }
    return session ? publishSession(std::move(session)) : nullptr;
}

std::unique_ptr<Daemon::Session> Daemon::constructSession(int columns, int rows, std::span<const std::string> command, int adoptFileDescriptor, uint32_t adopterProcessID,
    std::optional<Ubiquitty::Session::Layout::RootIdentity> placement, std::string_view creatorGrantKey, std::string_view initialDirectory,
    std::unique_ptr<Ubiquitty::Terminal> authoritativeTerminal)
{
    PAL::UnixFileDescriptor adoptedFileDescriptor(adoptFileDescriptor);
    if (authoritativeTerminal && (!adoptedFileDescriptor || !command.empty()))
        return nullptr;
    const bool isCompleteAdoption = static_cast<bool>(authoritativeTerminal);
    if (columns <= 0)
        columns = m_columns;
    if (rows <= 0)
        rows = m_rows;
    // One normalization for both the PTY (setWindowSize) and Core (Terminal::resize)
    // below, so an over-limit remote HELLO can't hand the child a geometry the
    // Core model can't represent.
    columns = clampColumns(columns);
    rows = clampRows(rows);

    // Bound live sessions on the persistent daemon — each pins a PTY + Terminal
    // + up to a 32 MiB input queue, so an unbounded spawn is an aggregate
    // resource-exhaustion vector. The HELLO handler drops the client on null.
    if (m_preparedLocalAdopts >= maxSessions || m_sessions.size() >= maxSessions - m_preparedLocalAdopts)
        return nullptr;

    // Structural allocations refuse the session, not the daemon: a throwing
    // new that fails under -fno-exceptions is std::terminate for every tenant
    // in the process (Docs/coding-conventions.md §6.1).
    std::unique_ptr<Session> s(new (std::nothrow) Session(m_capabilityDecisions));
    if (!s)
        return nullptr;
    // A session ID is never reused while the daemon runs: a client or script
    // holding `session:N` must never find it naming another session. Past the
    // last 32-bit number the counter stands at zero and every create is refused.
    do {
        if (m_nextSessionID == 0)
            return nullptr;
        s->id = m_nextSessionID++;
        s->createdUnix = static_cast<std::uint64_t>(::time(nullptr));
    } while (findSession(s->id) || std::ranges::any_of(m_workspaces, [&](const auto& workspace) { return workspace->workspaceID == s->id; })
        || findRestorableWorkspace(s->id));
    if (!placement)
        placement = Ubiquitty::Session::Layout::mintRootIdentity(*m_layoutIdentities);
    if (!placement)
        return nullptr;
    s->placement = *placement;

    s->configuration = m_configuration;
    if (authoritativeTerminal) {
        s->term = std::move(authoritativeTerminal);
        // The storage-tube colours are this host's configuration, which a
        // handover does not carry; a colour query answers with them.
        Ubiquitty::Runtime::seedTekColorPreference(*s->term, s->configuration);
    } else {
        s->term.reset(new (std::nothrow) Ubiquitty::Terminal());
        if (!s->term)
            return nullptr;
        // Only a new shell receives the host's default model configuration.
        // An adopted continuation already owns its authoritative configuration,
        // dictionaries, parser, screens and history.
        (void)Ubiquitty::Runtime::seedTerminalConfiguration(*s->term, s->configuration);
        s->term->drawsSizedText = true;
        s->term->drawsProportionalText = true;
        Ubiquitty::resize(*s->term, rows, columns);
    }
    // A grid whose buffers were refused keeps the geometry it had, and a
    // Terminal whose first grid was refused has none. The session is only
    // made once the grid holds the geometry the PTY is about to be sized to,
    // as Core represents it: a one-row PTY has a two-row grid.
    if (!s->term->grid || s->term->grid->rows() != clampGridRows(rows) || s->term->grid->columns() != clampGridColumns(columns))
        return nullptr;

    // The pane's identity is never reused: the daemon instance names this
    // process's lifetime and the session id is unique within it.
    s->screenSource.reset(new (std::nothrow) Protocol::Sync::PaneSource(*s->term, { .low = s->id, .high = m_daemonInstanceID }));
    if (!s->screenSource)
        return nullptr;
    wireBackgroundNotifications(*s);
    if (!wireRemoteInput(*s))
        return nullptr;
    // The first frame is owed before any program writes: a client that binds
    // the pane is sent a checkpoint of it.
    s->isFrameDue = true;

    // Enforce the capability gates server-side: wire the session policy to the
    // host-wide grant store and install it on the Terminal. The driving principal's
    // identity is stamped per Input frame, next to currentCapabilityOrigin.
    s->capabilityPolicy.setGrantStore(&m_grantStore);
    s->capabilityPolicy.setConsentQueue(&m_consentQueue); // Prompt → human ask
    s->term->capabilityPolicy = &s->capabilityPolicy;
    installTerminalClients(*s);

    if (adoptedFileDescriptor) {
        // re-home: adopt an existing PTY master fd (passed via SCM_RIGHTS).
        // The app already sized it + owns the child; don't setWindowSize (a
        // SIGWINCH would disturb the running shell) or reap.
        s->stream = isCompleteAdoption
            ? Ubiquitty::PAL::preparePTYAdoption(adoptedFileDescriptor.release(), static_cast<Ubiquitty::PAL::ProcessID>(adopterProcessID))
            : Ubiquitty::PAL::adoptPTY(adoptedFileDescriptor.release(), static_cast<Ubiquitty::PAL::ProcessID>(adopterProcessID));
        if (!s->stream)
            return nullptr; // ~Session disposes the Terminal (includes a failed ownership check)
        s->adopted = true; // the app owns the child; reclaimable via RECLAIM_SESSION
    } else {
        // The same PTY transport every frontend uses (Platform/POSIX/PTYStream.cpp):
        // open the login shell, then size it (openPTY forks without a winsize).
        const std::vector<std::string> shellArguments { m_shellPath };
        const bool isRemote = !command.empty();
        auto childEnvironment = clipboardToolEnvironment(m_socketPath, isRemote ? command.front() : m_shellPath);
        if (childEnvironment.empty()) {
            std::fprintf(stderr, "ubiquitty-host: clipboard command integration unavailable for this session\n");
        }
        // Where the child runs, so a tool inside it can address `here`.
        for (std::string& entry : Ubiquitty::Session::contextEnvironment(launchContextFor(*s)))
            childEnvironment.push_back(std::move(entry));
        if (!m_shellIntegrationDirectory.empty())
            childEnvironment.push_back("UBIQUITTY_INTEGRATION_DIR=" + m_shellIntegrationDirectory);
        // A remote session execs a named program, so its argv[0] is the path as
        // given: the login-shell '-' prefix is a convention for the shell a
        // terminal opens on its own, and a program that reads its own argv[0]
        // sees a name it does not have.
        s->stream = Ubiquitty::PAL::openPTY(isRemote ? command : std::span<const std::string>(shellArguments),
            { .initialDirectory = initialDirectory,
                .identity = Ubiquitty::Preferences::terminalIdentity(s->configuration),
                .argument0Style = isRemote ? Ubiquitty::PAL::Argument0Style::ProgramPath : Ubiquitty::PAL::Argument0Style::LoginShell,
                .environmentOverrides = childEnvironment,
                .colorEnvironmentPolicy = Ubiquitty::PAL::ColorEnvironmentPolicy::Terminal,
                .fileCreationMask = m_sessionFileCreationMask });
        if (!s->stream)
            return nullptr; // ~Session disposes the Terminal
        s->stream->setWindowSize(columns, rows, 0, 0);
    }

    // Granted before the creation is recorded, so the agent that made the
    // session is among those its first event reaches. It is one grant among
    // the session's, revocable on its own, not a slot a later approval
    // overwrites.
    if (!creatorGrantKey.empty())
        (void)s->addSessionGrant(std::string(creatorGrantKey));
    return s;
}

Daemon::Session* Daemon::publishSession(std::unique_ptr<Session> s)
{
    m_everHadSession = true;
    Session* raw = s.get();
    m_sessions.push_back(std::move(s));
    ++m_backgroundInventoryRevision;
    m_isStructureDirty = true;
    Ubiquitty::Session::Event created;
    created.kind = Ubiquitty::Session::EventKind::SessionCreated;
    created.sessionID = raw->id;
    created.paneID = std::to_underlying(raw->placement.pane);
    recordEvent(std::move(created));
    return raw;
}

Daemon::Session* Daemon::findSession(uint32_t id)
{
    if (id == 0)
        return nullptr;
    for (auto& sp : m_sessions)
        if (sp->id == id)
            return sp.get();
    return nullptr;
}

Daemon::Session* Daemon::findSessionByWorkspaceName(const std::string& workspaceName)
{
    // An unnamed session carries an empty name, so an empty query would match
    // every one of them — a client that named nothing asked for nothing.
    if (workspaceName.empty())
        return nullptr;
    for (const auto& workspace : m_workspaces) {
        if (workspace->name != workspaceName)
            continue;
        for (const auto& binding : workspace->paneSessions) {
            if (Session* session = findSession(binding.second))
                return session;
        }
    }
    for (auto& sp : m_sessions)
        if (!workspaceHolding(sp->id) && sp->workspaceName == workspaceName)
            return sp.get();
    return nullptr;
}

std::vector<Ubiquitty::Session::TreeAttendee> Daemon::attendeesOf(uint32_t sessionID) const
{
    std::vector<Ubiquitty::Session::TreeAttendee> attendees;
    if (sessionID == 0)
        return attendees;
    for (const auto& c : m_clients) {
        // A leaving client is already gone from the roster its peers see, so
        // it is gone from this one.
        if (!c.subscriptionFor(sessionID) || !c.helloSeen || c.didOverflow)
            continue;
        attendees.push_back({ .clientID = c.clientID, .kind = isAgentClient(c) ? Ubiquitty::Session::TreeAttendeeKind::Agent : Ubiquitty::Session::TreeAttendeeKind::Person });
    }
    return attendees;
}

namespace {

/// How many of a roster's attendees are people.
std::size_t peopleAmong(std::span<const Ubiquitty::Session::TreeAttendee> attendees)
{
    return static_cast<std::size_t>(std::ranges::count(attendees, Ubiquitty::Session::TreeAttendeeKind::Person, &Ubiquitty::Session::TreeAttendee::kind));
}

} // namespace

void Daemon::noteClientDeparture(uint32_t sessionID, bool wasHuman, bool cleanDetach) // bool-ok: two independent facts about one departure — whether a person left, and whether the detach was clean; an enum would have to enumerate their product
{
    Session* session = findSession(sessionID);
    if (!session || !session->endsWhenDetached || session->graceExpiresMilliseconds != 0)
        return;
    const auto attendees = attendeesOf(sessionID);
    if (peopleAmong(attendees) > 0)
        return; // someone is still watching it
    // The session ends with the last person on it. An agent leaving one that
    // never had a person ends it too, but only once nothing is attached at
    // all — one agent's departure must not close a session another is working
    // in.
    if (!wasHuman && !attendees.empty())
        return;
    const std::uint64_t now = nowMilliseconds();
    // Zero is the disarmed value, so an armed deadline is never zero.
    session->graceExpiresMilliseconds = cleanDetach ? std::max<std::uint64_t>(now, 1) : now + detachedSessionGraceMilliseconds;
    session->isGraceAwaitingHuman = wasHuman;
}

void Daemon::serviceDetachedSessions(std::uint64_t now)
{
    // Collected first: destroySession mutates m_sessions.
    std::vector<uint32_t> ended;
    for (auto& sp : m_sessions) {
        if (sp->graceExpiresMilliseconds == 0)
            continue;
        const auto attendees = attendeesOf(sp->id);
        if (sp->isGraceAwaitingHuman ? peopleAmong(attendees) > 0 : !attendees.empty()) {
            sp->graceExpiresMilliseconds = 0; // the arrival the window waited for
            continue;
        }
        if (now >= sp->graceExpiresMilliseconds)
            ended.push_back(sp->id);
    }
    for (const uint32_t id : ended)
        destroySession(id);
}

// The kernel-verified peer binaries the daemon trusts as LOCAL clients — its own
// bundled binaries doing a legitimate local connection:
//   - ubiquitty-host: the `--attach` bridge every local GUI SESSION streams
//     through (macOS localBridgeCommand).
//   - Ubiquitty: the macOS GUI app itself, which connects DIRECTLY for the one
//     operation the byte-stream --attach relay cannot carry — the SCM_RIGHTS PTY
//     handoff of "Share This Window" (ADOPT_SESSION). Must match the app's
//     CMake OUTPUT_NAME (UBIQUITTY_APP_NAME).
// An agent connects to the socket directly as ubq; any other unlabeled
// local peer is untrusted and falls to the gated Agent tier.
//
// What this tier is and is not. The Agent tier is a consent and visibility layer
// for automation that names itself, not same-uid-malware containment: at the same
// uid there is no hard privilege boundary, and the hard gate on adopt is the
// kernel proc-ancestry ownership walk in PAL::adoptPTY, which no identity here
// substitutes for. The allowlist keeps the bundled interface and its bridge from
// a consent-prompt storm, and nothing more.
//
// It is an identity, not a name, and the identity is taken once. Comparing the
// last component of the peer's path made the whole tier reachable by choosing a
// filename: one `cp /bin/sh /tmp/ubiquitty-host` and any program running as the
// user held full Allow, past the consent prompt and past the Agent scope floor.
// Device and inode is what a copy under any name does not share.
//
// Resolving that pair from the path at each classification would only move the
// forgery: a path is a name too, `::stat` follows it as it stands now, and the
// peer chooses what stands there. Connect from /tmp/x, replace /tmp/x with a
// link to this daemon's own binary, then send HELLO, and the classification that
// reads the path reads the wrong file. So capturePeerIdentity takes the identity
// at accept, from what the kernel resolved — /proc/<pid>/exe on Linux, which
// cannot be pointed elsewhere — and every later question is asked of that.
//
// What it therefore does not cover: a binary that is genuinely one of these two
// files, and a mixed installation. A bridge built in a worktree and run against
// a daemon installed elsewhere is a different file and lands in Agent, which is
// a consent prompt rather than a failure, and is the correct answer to "should
// this daemon trust a binary it was not built with".

// Drop the questions the daemon asked about this principal but nobody answered.
//
// The consent queue holds whether a triple is outstanding; this map holds what a
// request id means. Both have to go, and for different reasons: the queue entry
// suppresses the next request, and the record makes the old one answerable.
void Daemon::withdrawPendingGrantsFor(std::string_view identity)
{
    std::erase_if(m_pendingCapabilityGrants,
        [&](const auto& entry) {
            if (entry.second.identity != identity)
                return false;
            retireCapabilityQuestion(entry.first, entry.second);
            return true;
        });
}

// As above, for a session that is going away. A record names the session whose
// human may rule on it, so one outliving that session can never be ruled on and
// is only reachable by an id nobody should still be answering.
void Daemon::withdrawPendingGrantsForSession(std::uint32_t sessionID)
{
    std::erase_if(m_pendingCapabilityGrants,
        [&](const auto& entry) {
            if (entry.second.sessionID != sessionID)
                return false;
            retireCapabilityQuestion(entry.first, entry.second);
            return true;
        });
}

// Both at once, for a revoke that names a principal on one session and leaves
// what it holds elsewhere alone.
void Daemon::withdrawPendingGrantsForSessionOf(std::string_view identity, std::uint32_t sessionID)
{
    std::erase_if(m_pendingCapabilityGrants, [&](const auto& entry) {
        if (entry.second.identity != identity || entry.second.sessionID != sessionID)
            return false;
        retireCapabilityQuestion(entry.first, entry.second);
        return true;
    });
}

Ubiquitty::CapabilityOrigin Daemon::originOf(const Client& c) const
{
    // No daemon-verified local identity (QUIC / empty peer) ⇒ Remote, the
    // strictest principal — a self-claimed agentLabel is unverified over the wire.
    if (c.quicConnection || c.peerProcess.empty())
        return Ubiquitty::CapabilityOrigin::Remote;
    // A verified-local client that claims to be an agent ⇒ Agent.
    if (!c.agentLabel.empty())
        return Ubiquitty::CapabilityOrigin::Agent;
    // An unlabelled verified-local client is Local, with full Allow, only when
    // its kernel-verified peer binary is one of the daemon's own: the attach
    // bridge, or the app connecting directly for ADOPT_SESSION. On macOS a
    // release build knows them by the release team's signature, a development
    // build also by the files it resolved; elsewhere they are known by file
    // (`isOwnBinary`). A program the daemon does not recognise is Agent, the
    // gated tier, however it is labelled, so leaving the label off earns it
    // nothing. The check tells recognised binaries from unrecognised ones; it
    // is not a boundary against same-user code, which can start the bridge
    // itself (Docs/capability-origin.md).
    if (!c.hasPeerBinary)
        return Ubiquitty::CapabilityOrigin::Agent;
    const auto fileTrust = identityIsTrustedLocalBinary(c.peerBinary, m_trustedLocalBinaries) ? FileTrust::Trusted : FileTrust::Untrusted;
    if (isOwnBinary(buildSigningPolicy(), c.peerSignature, fileTrust))
        return Ubiquitty::CapabilityOrigin::Local;
    return Ubiquitty::CapabilityOrigin::Agent;
}

Ubiquitty::CapabilityOrigin Daemon::capabilityOriginOf(const Client& c) const
{
    // The bridge's mark only ever lowers: whatever originOf says, a person who
    // reached this host over SSH is on another machine, so the strictest
    // origin is theirs.
    if (c.isRelayedFromSSH)
        return Ubiquitty::CapabilityOrigin::Remote;
    return originOf(c);
}

LocalControlPeer Daemon::localControlPeer(const Client& client) const
{
    return {
        .isSameUserLocalStream = client.hasOwnerLocalCredentials && client.fileDescriptor && !client.quicConnection,
        // A person on this host: the daemon's own binary on a stream that did
        // not come over SSH. A relayed person keeps the owner's identity, but
        // every check that asks whether they are here answers as callers do.
        .isTrustedOwnerBinary = capabilityOriginOf(client) == Ubiquitty::CapabilityOrigin::Local,
        .hasRemoteGreeting = client.helloSeen,
        .hasDescriptors = static_cast<bool>(client.pendingAdoptFileDescriptor),
    };
}

bool Daemon::mayReceiveLocalHandoff(const Client& client) const
{
    if (!client.localAdoptAuthority || !m_trustedAdoptExecutables || !client.fileDescriptor || client.quicConnection || client.helloSeen)
        return false;
    const auto current = currentLocalAdoptAuthority(client.fileDescriptor.get(), *m_trustedAdoptExecutables);
    auto peer = localControlPeer(client);
    peer.isTrustedOwnerBinary = current.has_value();
    return admitsLocalAdopt({ .peer = peer, .isSelectedHandoff = client.localHandoffCorrelation != 0, .hasVerifiedProcess = current && current->processID == client.localAdoptAuthority->processID, .hasCurrentProcessInstance = current && *current == *client.localAdoptAuthority });
}

bool Daemon::bindLocalClipboardHelper(std::size_t index)
{
    if (index >= m_clients.size())
        return false;
    Client& client = m_clients[index];
    const auto peer = localControlPeer(client);
    if (!peer.isSameUserLocalStream || peer.hasRemoteGreeting || peer.hasDescriptors || client.hasPeerProcessInstanceMismatch
        || client.didSeeClipboardToolRequest || !client.peerProcessID)
        return false;
    std::uint64_t instanceBefore = 0;
    const bool hasInstance = peerProcessInstanceOf(client.peerProcessID, instanceBefore);
    if (client.peerProcessInstance && (!hasInstance || *client.peerProcessInstance != instanceBefore))
        return false;
    Session* owner = nullptr;
    std::array<PAL::Process::Record, 256> records;
    for (const auto& session : m_sessions) {
        const int count = session->stream ? session->stream->process().gatherProcesses(records) : 0;
        for (int record = 0; record < count; ++record) {
            if (records[static_cast<std::size_t>(record)].processID != client.peerProcessID)
                continue;
            if (owner)
                return false;
            owner = session.get();
        }
    }
    if (!owner || ::getpgid(static_cast<pid_t>(client.peerProcessID)) < 0)
        return false;
    if (hasInstance) {
        std::uint64_t instanceAfter = 0;
        if (!peerProcessInstanceOf(client.peerProcessID, instanceAfter) || instanceBefore != instanceAfter)
            return false;
        client.peerProcessInstance = instanceBefore;
    }
    client.clipboardToolSessionID = owner->id;
    return true;
}

void Daemon::feedLocalClipboard(std::size_t index, std::span<const std::uint8_t> bytes)
{
    if (index >= m_clients.size())
        return;
    Client& client = m_clients[index];
    const auto peer = localControlPeer(client);
    if (!peer.isSameUserLocalStream || peer.hasRemoteGreeting || peer.hasDescriptors) {
        dropClient(index);
        return;
    }
    const auto state = client.localClipboard.consume(bytes, [&](std::size_t bytes) { return admitsInboundCapacity(index, 0, bytes); });
    noteInboundDecoder(index);
    if (state == Protocol::Local::LocalClipboardRequestReader::State::Awaiting)
        return;
    const auto request = client.localClipboard.take();
    // Revalidate the kernel process tree after an arbitrarily delayed body,
    // before requesting any clipboard or allowing native routing.
    const auto sessionID = client.clipboardToolSessionID;
    if (!request || !bindLocalClipboardHelper(index) || client.clipboardToolSessionID != sessionID) {
        dropClient(index);
        return;
    }
    client.localClipboardKind = request->kind;
    client.localClipboardMIME = request->mime;
    Protocol::Sync::ClipboardRequestMessage operation {
        .requestID = 1,
        .kind = request->kind == Protocol::Local::LocalClipboardKind::Item ? Protocol::Sync::ClipboardRequestKind::Item : Protocol::Sync::ClipboardRequestKind::Types,
        .mime = std::string(request->mime),
    };
    handleClipboardToolRequest(index, operation, request->kind == Protocol::Local::LocalClipboardKind::Route ? ClipboardToolOperation::InspectSource : ClipboardToolOperation::Read);
}

void Daemon::unlistSession(uint32_t sessionID)
{
    m_isStructureDirty = true;
    for (Client& client : m_clients) {
        std::erase_if(client.listedSessions, [&](const std::pair<std::uint32_t, std::uint32_t>& listed) {
            if (listed.first != sessionID)
                return false;
            if (client.syncConnection)
                client.syncConnection->unlistPane(listed.second);
            return true;
        });
    }
}

void Daemon::destroySession(uint32_t id)
{
    // A target ending elsewhere retires its unanswered operation before the
    // target or its human subscriptions disappear. A later approval cannot
    // apply to another session, nor leave the caller waiting on a dead prompt.
    for (std::size_t index = 0; index < m_clients.size(); ++index) {
        if (m_clients[index].terminationCall && m_clients[index].terminationCall->sessionID == id)
            completeTerminationCall(index, "refused");
    }
    // A listing holds the pane's source by reference: it lets go first.
    unlistSession(id);
    // Read before the session goes: the record it leaves behind depends on it.
    SessionEnding sessionEnding = SessionEnding::Concluded;
    if (const Session* session = findSession(id); session && session->wasKilledBySignal)
        sessionEnding = SessionEnding::Lost;
    if (Session* ending = findSession(id)) {
        Ubiquitty::Session::Event ended;
        ended.kind = Ubiquitty::Session::EventKind::SessionEnded;
        ended.sessionID = id;
        ended.paneID = paneOfSession(*ending);
        ended.text = ending->exitReason.empty() ? "terminated" : ending->exitReason;
        recordEvent(std::move(ended));
    }
    // Each exact pane is unbound before its source goes. Other panes sharing
    // the physical connection remain live; only the last pane ends it.
    for (std::size_t i = m_clients.size(); i-- > 0;) {
        auto& client = m_clients[i];
        auto* subscription = client.subscriptionFor(id);
        if (!subscription)
            continue;
        if (client.sessionExitDeadlineMilliseconds || client.didOverflow) {
            unbindScreenPane(client, *subscription);
            subscription->screenHandle = 0;
            continue;
        }
        const bool isMultiplexed = client.intents.contains(Protocol::Sync::HelloIntent::AttachesWorkspace);
        unbindScreenPane(client, *subscription);
        subscription->screenHandle = 0;
        std::erase_if(client.paneSubscriptions,
            [id](const auto& pane) { return pane.sessionID == id; });
        if (!isMultiplexed || client.paneSubscriptions.empty()) {
            if (client.syncConnection)
                client.syncConnection->finish();
            armSessionExitDeadline(client, nowMilliseconds());
        }
        drainScreenInteractive(i);
        flushClient(i);
    }
    for (std::size_t k = 0; k < m_sessions.size(); ++k) {
        if (m_sessions[k]->id == id) {
            m_sessions.erase(m_sessions.begin() + static_cast<long>(k));
            ++m_backgroundInventoryRevision;
            break;
        }
    }
    // Release all Session-scoped capability grants tied to this session so the
    // next operation by the same principal re-prompts rather than riding a stale
    // per-session authorization.
    m_grantStore.dropSession(id);
    // The questions asked about this session go with it. A record names the
    // session whose human may rule on it, so one outliving the session can never
    // be ruled on and stays answerable only by an id nobody should be sending.
    withdrawPendingGrantsForSession(id);
    releaseWorkspaceSession(id, sessionEnding);
}

void Daemon::reclaimSession(std::size_t requesterIndex, uint32_t sessionID)
{
    if (requesterIndex >= m_clients.size())
        return;
    Session* s = findSession(sessionID);
    if (!s || !s->stream)
        return;
    if (s->stream->fileDescriptor() < 0)
        return;

    // The exact Result already precedes this pane's unbind. The requester
    // keeps its original local master; no descriptor or EOF grants reclaim.
    const uint32_t requesterClientID = m_clients[requesterIndex].clientID;
    for (std::size_t i = m_clients.size(); i-- > 0;) {
        auto& client = m_clients[i];
        auto* subscription = client.subscriptionFor(sessionID);
        if (!subscription)
            continue;
        unbindScreenPane(client, *subscription);
        subscription->screenHandle = 0;
        std::erase_if(client.paneSubscriptions, [sessionID](const auto& pane) {
            return pane.sessionID == sessionID;
        });
        if (client.paneSubscriptions.empty()) {
            client.sessionID = 0;
            if (client.syncConnection)
                client.syncConnection->finish();
            armSessionExitDeadline(client, nowMilliseconds());
        }
        // Never discard a partly transmitted frame to send the final outcome.
        // The ordinary service loop drains this bounded, ordered output.
        if (client.clientID != requesterClientID)
            drainScreenInteractive(i);
        flushClient(i);
    }
    {
        Ubiquitty::Session::Event ended;
        ended.kind = Ubiquitty::Session::EventKind::SessionEnded;
        ended.sessionID = sessionID;
        ended.paneID = paneOfSession(*s);
        ended.text = "reclaimed by the app";
        recordEvent(std::move(ended));
    }
    // A listing holds the pane's source by reference: it lets go first.
    unlistSession(sessionID);
    for (std::size_t k = 0; k < m_sessions.size(); ++k) {
        if (m_sessions[k]->id == sessionID) {
            m_sessions.erase(m_sessions.begin() + static_cast<long>(k));
            ++m_backgroundInventoryRevision;
            break;
        }
    }
    m_grantStore.dropSession(sessionID);
    withdrawPendingGrantsForSession(sessionID);
    // The app took its shell back: the session ended here by a person's choice.
    releaseWorkspaceSession(sessionID, SessionEnding::Concluded);
}

// Client I/O.

// How long the accept loop pauses when the descriptor table is full and there
// is no reserve to spend. Long enough that a spin becomes a wait, short enough
// that the daemon answers promptly once a descriptor frees.
constexpr useconds_t fileDescriptorExhaustionBackoffMicroseconds = 50 * 1000;

void Daemon::refuseClientWithReserveDescriptor(int listener)
{
    if (!m_reserveFileDescriptor) {
        // Nothing to spend. Sleeping is not a fix, but it turns a spin into a
        // wait, and the table emptying is the only thing that ends either.
        ::usleep(fileDescriptorExhaustionBackoffMicroseconds);
        return;
    }
    m_reserveFileDescriptor.reset();
    PAL::UnixFileDescriptor refused(::accept(listener, nullptr, nullptr));
    refused.reset(); // closing it is the refusal the client can see
    m_reserveFileDescriptor = PAL::UnixFileDescriptor(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (!m_reserveFileDescriptor)
        ::usleep(fileDescriptorExhaustionBackoffMicroseconds);
}

Daemon::PaneSubscription*
Daemon::Client::subscriptionFor(std::uint32_t targetSessionID)
{
    for (auto& subscription : paneSubscriptions) {
        if (subscription.sessionID == targetSessionID) {
            return &subscription;
        }
    }
    return nullptr;
}

const Daemon::PaneSubscription*
Daemon::Client::subscriptionFor(std::uint32_t targetSessionID) const
{
    for (const auto& subscription : paneSubscriptions) {
        if (subscription.sessionID == targetSessionID) {
            return &subscription;
        }
    }
    return nullptr;
}

void Daemon::acceptClient()
{
    acceptClient(m_listenFileDescriptor.get());
}

void Daemon::acceptClient(int listener)
{
    for (;;) {
        PAL::UnixFileDescriptor fileDescriptor(::accept(listener, nullptr, nullptr));
        if (!fileDescriptor) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                // Out of descriptors. The connection stays in the backlog, the
                // listener stays readable, and a level-triggered poll hands it
                // straight back — a full-CPU spin for as long as the table is
                // full, with every waiting client hung in connect() rather
                // than told no. Spending the reserve descriptor buys exactly
                // one accept, which is enough to refuse the connection and
                // drain it from the backlog.
                refuseClientWithReserveDescriptor(listener);
                break;
            }
            break;
        }
        // Peer authentication: the socket is already 0600 inside a 0700 directory,
        // but verify the connector's effective uid matches ours so that if the
        // socket ever moves to a shared/sandbox path it still can't be attached
        // cross-uid (a root connector to a user socket is the one case 0600 lets
        // through).
        uid_t peerEUID = 0;
        [[maybe_unused]] gid_t peerEGID = 0; // getpeereid out-parameter; gid value unused
#if defined(__APPLE__) || defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
        // getpeereid is the BSD-origin uid/gid call the accept-time check needs;
        // every BSD + Darwin has it (in <unistd.h>).
        const bool peerCredentialsOK = ::getpeereid(fileDescriptor.get(), &peerEUID, &peerEGID) == 0;
#elif defined(__linux__)
        // glibc has no getpeereid; SO_PEERCRED is the portable equivalent.
        struct ucred peerCredentials {};
        socklen_t peerCredentialsLength = sizeof(peerCredentials);
        const bool peerCredentialsOK = ::getsockopt(fileDescriptor.get(), SOL_SOCKET, SO_PEERCRED, &peerCredentials, &peerCredentialsLength) == 0
            && peerCredentialsLength == sizeof(peerCredentials);
        peerEUID = peerCredentials.uid;
        peerEGID = peerCredentials.gid;
#else
        const bool peerCredentialsOK = false; // unknown platform: fail closed
#endif
        if (!peerCredentialsOK || peerEUID != ::geteuid())
            continue; // only the owning uid may attach
        // Bound concurrent clients so a local process can't exhaust the fd
        // table / m_clients by opening connections it never HELLOs.
        // A client ID is never reused while the daemon runs; past the last one
        // the counter stands at zero and connections are refused.
        if (m_clients.size() >= maxClients || m_nextClientID == 0)
            continue;
        int flags = ::fcntl(fileDescriptor.get(), F_GETFL, 0);
        if (flags != -1)
            ::fcntl(fileDescriptor.get(), F_SETFL, flags | O_NONBLOCK);
        Client c;
        c.fileDescriptor = std::move(fileDescriptor);
        c.hasOwnerLocalCredentials = true;
        c.clientID = m_nextClientID++;
        c.connectMilliseconds = nowMilliseconds();
        c.attachedUnix = static_cast<std::uint64_t>(::time(nullptr));
        capturePeerIdentity(c); // verified processID + exe path (consent gate)
        // Once per connection, not per frame: a daemon outlives an upgrade, and
        // the bridge that connects after one is launched from the replacement
        // file. Merging here is what keeps that bridge out of the Agent tier on
        // a host where no human is attached to answer a consent prompt.
        mergeTrustedLocalBinaries(resolveSelfExecutablePath(), m_trustedLocalBinaries);
        m_clients.push_back(std::move(c));
        // No snapshot yet: the client must HELLO first (create/attach a
        // session), then we ACK + send a full snapshot (handleFrame).
    }
}

// Deferred drop: mark the client dead; the run-loop reap pass (reapClient, from
// the END of m_clients) does the actual close + erase. Never erasing inline keeps
// indices stable mid-dispatch, so a peer that RSTs while a forward loop or a saved
// index is live cannot shift it onto the wrong client. The same
// deferred-reap invariant applies to kickClient via `didOverflow`.
void Daemon::dropClient(std::size_t index)
{
    if (index >= m_clients.size())
        return;
    retireClientConsent(m_clients[index]);
    cancelPendingClipboardWrites(m_clients[index].clientID);
    // A client's label lasts as long as its connection: no other connection
    // is ever numbered as it was, and a resume is a new one.
    m_labels.forget({ Identity::ObjectKind::Client, m_clients[index].clientID });
    m_clients[index].terminationCall.reset();
    m_clients[index].reclaimCall.reset();
    m_clients[index].attachConsentApproval.reset();
    m_clients[index].attachConsentTerminal.reset();
    m_clients[index].consentPending = false;
    // One attendee fewer, and one reader.
    m_isStructureDirty = true;
    std::erase_if(m_structureConsents, [&](const auto& held) {
        if (held.second.clientID != m_clients[index].clientID)
            return false;
        retireStructureConsent(held.first, held.second);
        return true;
    });
    for (auto& source : m_clients) {
        if (source.clientID == m_clients[index].clipboardToolSourceClientID) {
            std::erase(source.pendingClipboardRequests, m_clients[index].clipboardToolRequestID);
            if (source.syncConnection)
                source.syncConnection->cancelClipboardRequest(m_clients[index].clipboardToolRequestID);
        }
    }
    // A driver that goes holds the focus of no pane. Its attachment is
    // Detached when the client left on purpose, and Lost otherwise, so a
    // resume on another connection can continue it; one a resume already moved
    // off this connection is untouched.
    // A connection a resume proof admitted that goes before its Greet resumed
    // leaves the attachment free to be resumed again.
    m_attachments->abandonProof(m_clients[index].clientID);
    if (m_clients[index].syncConnection) {
        auto& connection = *m_clients[index].syncConnection;
        const Protocol::Sync::AttachmentRecord* attachment = connection.attachment();
        const std::optional<Protocol::Sync::ID128> attachmentID = attachment ? std::optional { attachment->attachmentID } : std::nullopt;
        const bool isLeaving = attachment && m_clients[index].didDetachCleanly && attachment->servingConnection == m_clients[index].clientID;
        // The table may retire the record now or before this client's reap.
        // Release its record and pane borrows before either can be freed.
        connection.withdrawAuthority();
        for (const PaneSubscription& subscription : m_clients[index].paneSubscriptions) {
            if (Session* session = findSession(subscription.sessionID))
                notePaneFocus(*session);
        }
        if (attachmentID) {
            if (isLeaving)
                m_attachments->noteDetached(*attachmentID);
            else
                m_attachments->noteTransportLost(*attachmentID, m_clients[index].clientID, nowMicroseconds());
        }
    }
    // It shows nobody the questions it was asked any more, so each is left to
    // the others asked, and refused if none is. Left after its attachment is
    // marked, as the connection marks it itself on a suspending Bye or a
    // silent link, so the table says whether its person may resume and be
    // asked again. Only the host's own supersession says a resume continued
    // it elsewhere; a peer's Bye naming one is a link it ended.
    const bool isContinued = m_clients[index].syncConnection && m_clients[index].syncConnection->wasSuperseded();
    leaveEveryQuestion(m_clients[index].clientID, isContinued ? QuestionLeave::Continued : QuestionLeave::Departed);
    // What the client sent before its attachment was known sets no pane's
    // size once it is gone; an attachment that may resume keeps its own.
    for (const PaneSubscription& subscription : m_clients[index].paneSubscriptions) {
        if (Session* session = findSession(subscription.sessionID); session && session->screenSource)
            session->screenSource->geometry().forget({ .low = m_clients[index].clientID, .high = 0 });
    }
    // The table, not this channel, owns Prepared candidates and outcomes.
    // A decoder borrow must end before its record allocation is released.
    m_clients[index].pendingAdoptFileDescriptor.reset();
    if (!m_clients[index].isReadingFrames)
        m_clients[index].localAdopt.reset();
    if (m_clients[index].syncConnection)
        m_clients[index].syncConnection->discardInboundClipboard();
    m_clients[index].clipboardToolRequestID = 0;
    m_clients[index].clipboardToolSourceClientID = 0;
    cancelClipboardTools(m_clients[index].clientID);
    // A question this client asked and nobody ruled on. The queue dedupes, so
    // an entry outliving the connection that raised it is not a leak but a
    // permanent denial: the reconnecting agent's ask is suppressed as a repeat,
    // no request reaches a human, and the chokepoint answers Prompt forever.
    //
    // Only when nothing else on the host answers to the same identity. A grant
    // key for a tokenless agent is its binary, so two processes of one agent
    // share it, and dropping on the first departure would take the survivor's
    // question with it.
    const std::string departing = grantKeyFor(m_clients[index]);
    if (!departing.empty()) {
        bool isHeldByAnother = false;
        for (std::size_t other = 0; other < m_clients.size(); ++other) {
            if (other == index || m_clients[other].didOverflow)
                continue;
            isHeldByAnother = isHeldByAnother || grantKeyFor(m_clients[other]) == departing;
        }
        if (!isHeldByAnother)
            m_consentQueue.resolveIdentity(departing);
    }
    m_clients[index].didOverflow = true;
    // An owed-work boundary may have selected this client before its offer
    // cache was observed. Departure always revalidates, regardless of cache.
    for (PaneSubscription& subscription : m_clients[index].paneSubscriptions) {
        subscription.richTextOffer.reset();
        subscription.richTextOfferHandle = 0;
        if (Session* session = findSession(subscription.sessionID))
            refreshRichTextMeasurer(*session);
    }
}

// Close + erase a dead client. Only the run-loop reap pass calls this, and only
// from the END of m_clients, so the erase never shifts a still-iterated index.
void Daemon::reapClient(std::size_t index)
{
    if (index >= m_clients.size())
        return;
    dropClient(index);
    auto subscriptions = std::move(m_clients[index].paneSubscriptions);
    // Read before the erase, which is the last point at which this client's
    // kind and its manner of leaving are still knowable.
    const bool wasHuman = !isAgentClient(m_clients[index]);
    const bool cleanDetach = m_clients[index].didDetachCleanly;
    if (m_clients[index].fileDescriptor) {
        // Half-close the write end before close() so any bytes still queued in
        // the kernel send buffer (e.g. a final AUTH_DENIED right before a
        // revoke drop) reach the peer with an orderly EOF. A plain close()
        // discards the unread send buffer on macOS (BSD AF_UNIX semantics),
        // which would swallow that last frame; the half-close flushes it on
        // both macOS and Linux.
        ::shutdown(m_clients[index].fileDescriptor.get(), SHUT_WR);
        m_clients[index].fileDescriptor.reset();
    }
    m_clients[index].pendingAdoptFileDescriptor.reset(); // unconsumed handoff fd
#ifdef UBIQUITTY_QUIC
    // Free a dropped QUIC client's connection + its queued output now, not at the
    // 90 s idle timeout — else an overflowed/reaped peer pins its ~16 MiB until then.
    if (m_clients[index].quicConnection && m_quic) {
        std::optional<QUICApplicationClose> close;
        if (const auto reason = m_clients[index].quicCloseReason)
            close = QUICApplicationClose { .errorCode = Protocol::Sync::quicCloseCodeOf(*reason), .reasonPhrase = Protocol::Sync::byeNotice(*reason) };
        m_quic->closeConnection(m_clients[index].quicConnection, close);
    }
#endif
    m_clients.erase(m_clients.begin() + static_cast<long>(index));
    // No EXIT here on purpose: this drops a client for overflow / I/O error /
    // decode fault while its SESSION stays alive (tmux model). The bare
    // socket EOF is the right signal — the client reattaches and gets a fresh
    // snapshot. EXIT means "your shell exited" and is sent only by
    // destroySession; sending it here would wrongly tear the window down.
    // The departure does change the roster, so refresh the remaining humans.
    for (const auto& subscription : subscriptions)
        broadcastPresence(subscription.sessionID);
    // An ends-when-detached session left with nobody on it starts counting
    // down; serviceDetachedSessions ends it when the count runs out.
    for (const auto& subscription : subscriptions)
        noteClientDeparture(subscription.sessionID, wasHuman, cleanDetach);
}

void Daemon::armSessionExitDeadline(Client& client, std::uint64_t now) const
{
    client.sessionExitDrainStartMilliseconds = now;
    client.sessionExitDeadlineMilliseconds = now + sessionExitDrainMilliseconds;
    client.sessionExitPendingBytes = outboundRetainedBytes(client);
}

bool Daemon::isSessionExitDrainOver(Client& client, std::uint64_t now) const
{
    if (!client.sessionExitDeadlineMilliseconds || now < client.sessionExitDeadlineMilliseconds)
        return false;
    const std::size_t pending = outboundRetainedBytes(client);
    const bool isReading = pending > 0 && pending < client.sessionExitPendingBytes
        && client.sessionExitPendingBytes - pending >= sessionExitMinimumDrainBytes;
    const bool hasDrainTimeLeft = now - client.sessionExitDrainStartMilliseconds < maxSessionExitDrainMilliseconds;
    if (isReading && hasDrainTimeLeft) {
        client.sessionExitDeadlineMilliseconds = now + sessionExitDrainMilliseconds;
        client.sessionExitPendingBytes = pending;
        return false;
    }
    return true;
}

bool Daemon::isReapDue(Client& client, std::uint64_t now) const
{
    // A client dropped for its backlog keeps its slot until the Bye that says
    // so has left, or its bounded wait ends.
    if (client.didOverflow && client.backlogByeDeadlineMilliseconds && client.hasPendingOutput() && now < client.backlogByeDeadlineMilliseconds)
        return false;
    return client.didOverflow || (!client.helloSeen && !client.localHandoffCorrelation && now - client.connectMilliseconds > helloGraceMilliseconds)
        || isSessionExitDrainOver(client, now);
}

void Daemon::kickClient(std::size_t index)
{
    if (index >= m_clients.size())
        return;
    if (!m_clients[index].syncConnection) {
        dropClient(index);
        return;
    }
    m_clients[index].syncConnection->finish();
    withdrawClientAttachment(index);
    cancelPendingClipboardWrites(m_clients[index].clientID);
    // Its attachment is withdrawn, so it answers nothing it was asked: what
    // it allowed and no agent has taken is void, and each question is left
    // to the others now, not when it is reaped.
    voidApprovalsGivenBy(m_clients[index].clientID);
    leaveEveryQuestion(m_clients[index].clientID, QuestionLeave::Departed);
    m_clients[index].didOverflow = false;
    drainScreenInteractive(index);
    armSessionExitDeadline(m_clients[index], nowMilliseconds());
    flushClient(index);
}

void Daemon::withdrawClientAttachment(std::size_t index)
{
    // A resume's old stream may still borrow this record until its pending
    // supersession is processed. Release those borrowers before any retirement.
    closeSupersededConnections();
    Client& client = m_clients[index];
    m_attachments->abandonProof(client.clientID);
    if (!client.syncConnection)
        return;
    const Protocol::Sync::AttachmentRecord* attachment = client.syncConnection->attachment();
    const std::optional<Protocol::Sync::ID128> retiring = attachment && attachment->servingConnection == client.clientID
        ? std::optional { attachment->attachmentID }
        : std::nullopt;
    if (retiring)
        retireAttachmentGrant(*retiring);
    else
        client.syncConnection->withdrawAuthority();
}

void Daemon::retireAttachmentGrant(Protocol::Sync::ID128 attachmentID)
{
    closeSupersededConnections();
    for (auto& session : m_sessions)
        if (session->screenSource)
            session->screenSource->withdrawInputAuthority(attachmentID, nowMicroseconds());
    const auto* attachment = m_attachments->find(attachmentID);
    for (std::size_t index = 0; index < m_clients.size(); ++index) {
        Client& client = m_clients[index];
        // Lost streams can await reap while a replacement serves the record.
        if (attachment && client.syncConnection && client.syncConnection->attachment() == attachment) {
            client.syncConnection->withdrawAuthority();
            client.didOverflow = true;
        }
        // A proof-only connection must not lose its binding then greet fresh.
        if (m_attachments->provenAttachment(client.clientID) == std::optional { attachmentID })
            dropClient(index);
    }
    std::erase_if(m_attachmentGrants, [&](const AttachmentGrant& grant) { return grant.attachmentID == attachmentID; });
    m_attachments->noteDetached(attachmentID);
    m_isStructureDirty = true;
}

void Daemon::broadcastPresence(uint32_t sessionID)
{
    // This invalidates the one attachments roster; it never creates a second
    // wire list or sends before the owner-thread service/admission boundary.
    for (Client& recipient : m_clients)
        if (sessionID && recipient.subscriptionFor(sessionID) && recipient.syncConnection)
            recipient.syncConnection->noteAttachmentsChanged();
}

std::vector<Protocol::Sync::ID128> Daemon::agentAttachmentsForSession(std::uint32_t sessionID, std::string_view identity) const
{
    std::vector<Protocol::Sync::ID128> panes;
    for (const auto& session : m_sessions)
        if (session->screenSource && (!sessionID || session->id == sessionID))
            panes.push_back(session->screenSource->paneID());
    std::vector<Protocol::Sync::ID128> attachments;
    for (const auto* attachment : m_attachments->retainingPanes(panes))
        if (attachment->kind == Protocol::Sync::ClientKind::Agent
            && (identity.empty() || attachment->authority.principal == identity))
            attachments.push_back(attachment->attachmentID);
    return attachments;
}

std::optional<Protocol::Sync::AttachmentPresence> Daemon::attachmentPresenceFor(std::uint32_t recipientClientID,
    Protocol::Sync::ID128 attachmentID, std::span<const Protocol::Sync::ID128> panes, bool isSelf) const
{
    using namespace Protocol::Sync;
    const auto nowUnix = static_cast<std::uint64_t>(::time(nullptr));
    const auto isCurrent = [&](const Client& client) {
        return client.helloSeen && !client.didOverflow && !client.sessionExitDeadlineMilliseconds && hasLiveStructureAttachment(client)
            && (!client.hasToken || !client.tokenExpiryUnix || client.tokenExpiryUnix > nowUnix);
    };
    const auto recipient = std::ranges::find(m_clients, recipientClientID, &Client::clientID);
    if (!m_attachments || recipient == m_clients.end() || !isCurrent(*recipient) || isAgentClient(*recipient)
        || !tokenScopeAllows(*recipient, Protocol::Sync::Scope::Observe) || (!isSelf && panes.empty()))
        return std::nullopt;
    const auto* recipientRecord = recipient->syncConnection->attachment();
    if (!recipientRecord || recipientRecord->kind == ClientKind::Agent || recipient->syncConnection->grantedRole() == Role::Agent
        || m_attachments->find(recipientRecord->attachmentID) != recipientRecord)
        return std::nullopt;
    const auto target = std::ranges::find_if(m_clients, [&](const Client& candidate) {
        const auto* record = candidate.syncConnection ? candidate.syncConnection->attachment() : nullptr;
        return isCurrent(candidate) && record && record->attachmentID == attachmentID
            && record->admission == AttachmentAdmission::Granted && record->state == AttachmentState::Live
            && record->servingConnection == candidate.clientID && m_attachments->find(attachmentID) == record;
    });
    if (target == m_clients.end() || isSelf != (target->clientID == recipientClientID))
        return std::nullopt;
    AttachmentPresence presence {
        .clientID = target->clientID,
        .processID = target->peerProcessID,
        .process = target->peerProcessID ? target->peerProcess : std::string {},
        .claimedLabel = target->agentLabel,
        .grantIdentity = grantKeyFor(*target),
        .isQUIC = target->quicConnection != nullptr,
        .sharedPanes = {},
    };
    const bool isAgent = isAgentClient(*target);
    const std::string key = isAgent ? grantKeyFor(*target) : std::string {};
    const bool isAlways = !key.empty() && std::ranges::find(m_alwaysAllowedAgents, key) != m_alwaysAllowedAgents.end();
    presence.sharedPanes.reserve(panes.size());
    for (const ID128 pane : panes) {
        const auto session = std::ranges::find_if(m_sessions, [&](const auto& current) {
            return current->screenSource && current->screenSource->paneID() == pane
                && target->subscriptionFor(current->id) && recipient->subscriptionFor(current->id);
        });
        if (session == m_sessions.end())
            return std::nullopt;
        const bool isSession = (*session)->hasSessionGrantFor(key);
        presence.sharedPanes.push_back({ pane, isAlways ? PresenceGrant::Always : isSession ? PresenceGrant::Session
                                                                                            : PresenceGrant::None });
    }
    return presence;
}

// A session echoes what it is sent, so a client admitted to send one max-size
// frame is owed up to that many bytes back. The echo arrives as OUTPUT frames
// of at most ptyReadChunk bytes, each carrying a header, so the cap must hold
// the payload and that framing.
static constexpr std::size_t maxEchoOfOneClientFrame = Protocol::Sync::maxClientFramePayloadLength
    + ((Protocol::Sync::maxClientFramePayloadLength / static_cast<std::size_t>(ptyReadChunk)) + 1u)
        * Protocol::Sync::frameHeaderLength;
// The per-client and aggregate outbound caps derive from the catalog
// allowance (Platform/Host/OutboundCapacity.h).
static_assert(clientBufferHighWater >= maxEchoOfOneClientFrame,
    "a cap under one client frame's echo drops the sender mid-frame, at a size the inbound cap admits");

std::size_t Daemon::outboundPendingBytes(const Client& c) const
{
    std::size_t pending = c.pendingOutputBytes() + c.pendingConsentRetirements.capacity() * sizeof(Protocol::Sync::ConsentResolution);
#ifdef UBIQUITTY_QUIC
    // A QUIC client's outputBuffer is drained into the connection's own queues
    // by flushClient, which clears it on every flush — so for QUIC the buffer
    // reads near zero however far behind the reader is, and the bytes that are
    // actually waiting live in the transport. Only the ones ngtcp2 has not taken
    // are waiting: counting the ones in flight against the one-frame low water
    // held a QUIC client to one frame per round trip, and a 50 ms, 20 Mbit/s
    // link to half its rate.
    if (c.quicConnection && m_quic)
        pending += m_quic->unsentBytes(c.quicConnection);
#endif
    return pending;
}

std::size_t Daemon::outboundRetainedBytes(const Client& c) const
{
    std::size_t retained = c.pendingOutputBytes();
#ifdef UBIQUITTY_QUIC
    if (c.quicConnection && m_quic)
        retained += m_quic->pendingBytes(c.quicConnection);
#endif
    return retained;
}

std::size_t Daemon::outboundPayloadCapacity(const Client& client) const
{
    std::size_t bytes = client.outputBuffer.capacity();
    if (client.syncConnection) {
        bytes += client.syncConnection->retainedOutboundPayloadBytes();
        // Leave room for both bounded carrier frames even before service
        // allocates them. Their room survives final image-source release and
        // cannot disappear if a higher-priority path temporarily fills the cap.
        bytes += 2 * screenOutputLowWaterBytes;
    }
#ifdef UBIQUITTY_QUIC
    if (client.quicConnection && m_quic)
        bytes += m_quic->storageBytes(client.quicConnection);
#endif
    return bytes;
}

Protocol::Sync::BulkSourceAdmission Daemon::imageSourceAdmission(std::uint32_t clientID) const
{
    Protocol::Sync::BulkSourceAdmission admission;
    admission.aggregate = m_imageSourceBudget;
    for (const auto& client : m_clients) {
        const auto bytes = outboundPayloadCapacity(client);
        admission.otherAggregateBytes += bytes;
        if (client.clientID == clientID && !client.didOverflow) {
            admission.connection = client.imageSourceBudget;
            admission.otherConnectionBytes = bytes;
        }
    }
    return admission;
}

void Daemon::preemptOutboundImages()
{
    for (auto& client : m_clients) {
        if (client.syncConnection)
            client.syncConnection->preemptImageTransfers();
    }
}

std::optional<Protocol::Sync::BulkSourceReservation> Daemon::reserveImageCarrierScratch(std::uint32_t clientID, std::size_t bytes)
{
    const auto admission = imageSourceAdmission(clientID);
    if (!admission.connection || !admission.aggregate)
        return std::nullopt;
    auto reservation = Protocol::Sync::BulkSourceReservation::acquire(admission, bytes);
    if (!reservation) {
        preemptOutboundImages();
        reservation = Protocol::Sync::BulkSourceReservation::acquire(imageSourceAdmission(clientID), bytes);
    }
    // With every image owner retired, absence leaves the pre-existing carrier
    // path under its original nonimage caps; this is no new blanket refusal.
    return reservation;
}

void Daemon::releaseIdleImageCarrierCapacity()
{
    for (auto& client : m_clients) {
        if (!client.hasPendingOutput() && client.outputBuffer.capacity() > screenOutputLowWaterBytes) {
            std::vector<std::uint8_t>().swap(client.outputBuffer);
            client.outputConsumed = 0;
        }
    }
}

void Daemon::queueToClient(std::size_t index, std::span<const std::uint8_t> frames)
{
    if (index >= m_clients.size() || frames.empty())
        return;
    auto& c = m_clients[index];
    if (c.didOverflow || c.writeFailureDeadlineMilliseconds)
        return; // already flagged or unable to write
    const std::span<const std::uint8_t> bytes = frames;
    const std::size_t pending = outboundPendingBytes(c);
    // Overflow-safe high-water test: pending + length must not wrap SIZE_MAX
    // (a wrapped sum reads as "under cap" and would bypass the very defense this
    // adds). Equivalent to pending + length > clientBufferHighWater without the
    // addition.
    if (!fitsClientOutput(pending, bytes.size(), clientBufferHighWater)) {
        // The three inbound caps and the outbound aggregate name themselves; this
        // one did not, and it is the path a peer hits first. A client discarded
        // here closes mid-write with nothing in the log, so ruling the other four
        // out by their silence leaves a drop no line accounts for.
        Ubiquitty::PAL::logLine(stderr,
            "daemon: outbound buffer dropped client {} — per-client high water (pending {}, adding {}, cap {})",
            index, pending, bytes.size(), clientBufferHighWater);
        dropForOutputBacklog(index);
        return;
    }
    if (bytes.size() > c.outputBuffer.capacity() - c.outputBuffer.size()) {
        // Keep geometric growth, but choose its capacity before allocation.
        // The old allocation and its replacement coexist during reserve, so
        // the full new allocation is the additional peak charge.
        const auto doubled = c.outputBuffer.capacity() <= clientBufferHighWater / 2 ? 2 * c.outputBuffer.capacity() : clientBufferHighWater;
        const auto newCapacity = std::max(c.outputBuffer.size() + bytes.size(), doubled);
        const auto admission = imageSourceAdmission(c.clientID);
        if (admission.connection && admission.aggregate
            && (newCapacity > admission.connection->availableBytes(admission.otherConnectionBytes)
                || newCapacity > admission.aggregate->availableBytes(admission.otherAggregateBytes)))
            preemptOutboundImages();
        c.outputBuffer.reserve(newCapacity);
    }
    c.outputBuffer.insert(c.outputBuffer.end(), bytes.begin(), bytes.end());
}

std::optional<std::size_t> Daemon::Client::advanceOutputFrameStart()
{
    while (outputFrameStart < outputConsumed) {
        Protocol::Sync::FrameHeader header;
        Protocol::Sync::EnvelopeError error = Protocol::Sync::EnvelopeError::None;
        const auto rest = std::span<const std::uint8_t>(outputBuffer).subspan(outputFrameStart);
        if (Protocol::Sync::readFrameHeader(rest, header, error) != Protocol::Sync::FrameHeaderState::Complete)
            return std::nullopt;
        const std::size_t frameBytes = Protocol::Sync::frameBytes(header.channel, header.bodyBytes);
        if (frameBytes > rest.size())
            return std::nullopt;
        if (outputFrameStart + frameBytes > outputConsumed)
            return outputFrameStart + frameBytes;
        outputFrameStart += frameBytes;
    }
    return outputFrameStart;
}

void Daemon::dropForOutputBacklog(std::size_t index)
{
    dropWithFarewell(index, Protocol::Sync::ByeReason::OutputBacklog, "The client fell too far behind reading the host's output.");
}

void Daemon::dropWithFarewell(std::size_t index, Protocol::Sync::ByeReason reason, std::string_view detail)
{
    Client& client = m_clients[index];
    client.didOverflow = true; // the loop reaps the client; nothing else serves it
    // It answers nothing from now on, so its questions are left at once; its
    // attachment, lost when it is reaped, may still be resumed.
    const bool isContinued = client.syncConnection && client.syncConnection->wasSuperseded();
    leaveEveryQuestion(client.clientID, isContinued ? QuestionLeave::Continued : QuestionLeave::Departed);
    bool isFramedStream = client.syncConnection && client.fileDescriptor && client.syncConnection->state() != Protocol::Sync::ConnectionState::Closed;
#ifdef UBIQUITTY_QUIC
    // A QUIC stream already holds what was handed to it, and a Bye queued
    // behind that would never leave: the reap closes the connection with the
    // reason in its CONNECTION_CLOSE instead, which no stream data delays.
    if (client.quicConnection) {
        client.quicCloseReason = reason;
        isFramedStream = false;
    }
#endif
    const std::optional<std::size_t> inFlightEnd = isFramedStream ? client.advanceOutputFrameStart() : std::nullopt;
    if (!inFlightEnd) {
        std::vector<std::uint8_t>().swap(client.outputBuffer);
        client.clearPendingOutput();
        return;
    }
    // The rest of the frame the socket is partway through, then the Bye, in a
    // buffer of their own size: the backlog's allocation goes now.
    std::vector<std::uint8_t> farewell(client.outputBuffer.begin() + static_cast<std::ptrdiff_t>(client.outputConsumed),
        client.outputBuffer.begin() + static_cast<std::ptrdiff_t>(*inFlightEnd));
    client.syncConnection->closeDiscardingQueue(reason, detail);
    std::vector<std::uint8_t> bye;
    // Bounded: a reason, that sentence and the build metadata the Bye's
    // grammar bounds, one frame written past the mark on purpose.
    if (!client.syncConnection->popInteractiveFrame(bye) || bye.size() > Protocol::Sync::maxEnvelopeHeaderBytes + Protocol::Sync::maxFrameBodyBytes) {
        std::vector<std::uint8_t>().swap(client.outputBuffer);
        client.clearPendingOutput();
        return;
    }
    farewell.insert(farewell.end(), bye.begin(), bye.end());
    client.outputBuffer = std::move(farewell);
    client.outputConsumed = 0;
    client.outputFrameStart = 0;
    client.backlogByeDeadlineMilliseconds = nowMilliseconds() + backlogByeDrainMilliseconds;
}

LocalReplyAdmission Daemon::reserveLocalQUICTokenReply(std::size_t index)
{
    return reserveLocalReplyBytes(index, Protocol::Local::maxLocalQUICTokenReplyBytes);
}

LocalReplyAdmission Daemon::reserveLocalReplyBytes(std::size_t index, std::size_t replyBytes)
{
    if (!replyBytes || replyBytes > clientBufferHighWater)
        return LocalReplyAdmission::Refused;
    if (index >= m_clients.size() || m_clients[index].didOverflow)
        return LocalReplyAdmission::Refused;
    auto& client = m_clients[index];
    if (!fitsClientOutput(outboundPendingBytes(client), replyBytes, clientBufferHighWater)
        || client.outputBuffer.size() > clientBufferHighWater - replyBytes)
        return LocalReplyAdmission::Refused;
    const auto required = client.outputBuffer.size() + replyBytes;
    if (required <= client.outputBuffer.capacity())
        return LocalReplyAdmission::Reserved;
    const auto admission = imageSourceAdmission(client.clientID);
    const auto connectionAvailable = admission.connection
        ? admission.connection->availableBytes(admission.otherConnectionBytes)
        : clientBufferHighWater - std::min(admission.otherConnectionBytes, clientBufferHighWater);
    const auto aggregateAvailable = admission.aggregate
        ? admission.aggregate->availableBytes(admission.otherAggregateBytes)
        : maxAggregateOutboundBytes - std::min(admission.otherAggregateBytes, maxAggregateOutboundBytes);
    if (required > connectionAvailable || required > aggregateAvailable)
        return LocalReplyAdmission::Refused;
    client.outputBuffer.reserve(required);
    // reserve may retain more than requested; no mint or fallback may spend
    // that excess. Retire the allocation immediately instead of leaving an
    // over-budget owner until the next reap.
    const auto capacity = client.outputBuffer.capacity();
    if (capacity > connectionAvailable || capacity > aggregateAvailable) {
        std::vector<std::uint8_t>().swap(client.outputBuffer);
        client.outputConsumed = 0;
        client.didOverflow = true;
        return LocalReplyAdmission::Refused;
    }
    return LocalReplyAdmission::Reserved;
}

bool Daemon::queueLocalClipboardReply(std::size_t index, const Protocol::Local::LocalClipboardReply& reply)
{
    const auto measured = Protocol::Local::localClipboardReplyBytes(reply);
    if (!measured || index >= m_clients.size() || m_clients[index].didOverflow)
        return false;
    auto& client = m_clients[index];
    if (!fitsClientOutput(outboundPendingBytes(client), *measured, clientBufferHighWater)
        || client.outputBuffer.size() > clientBufferHighWater - *measured)
        return false;
    const auto required = client.outputBuffer.size() + *measured;
    if (required > client.outputBuffer.capacity()) {
        const auto admission = imageSourceAdmission(client.clientID);
        const auto connectionAvailable = admission.connection
            ? admission.connection->availableBytes(admission.otherConnectionBytes)
            : clientBufferHighWater - std::min(admission.otherConnectionBytes, clientBufferHighWater);
        const auto aggregateAvailable = admission.aggregate
            ? admission.aggregate->availableBytes(admission.otherAggregateBytes)
            : maxAggregateOutboundBytes - std::min(admission.otherAggregateBytes, maxAggregateOutboundBytes);
        if (required > connectionAvailable || required > aggregateAvailable)
            return false;
        client.outputBuffer.reserve(required);
        if (client.outputBuffer.capacity() > connectionAvailable || client.outputBuffer.capacity() > aggregateAvailable) {
            std::vector<std::uint8_t>().swap(client.outputBuffer);
            client.outputConsumed = 0;
            client.didOverflow = true;
            return false;
        }
    }
    const auto start = client.outputBuffer.size();
    client.outputBuffer.resize(required);
    if (!Protocol::Local::writeLocalClipboardReply(reply, std::span(client.outputBuffer).subspan(start))) {
        client.outputBuffer.resize(start);
        return false;
    }
    return true;
}

void Daemon::drainScreenInteractive(std::size_t index)
{
    if (index >= m_clients.size())
        return;
    Client& client = m_clients[index];
    if (!client.syncConnection)
        return;
    std::vector<std::uint8_t> frame;
    frame.reserve(Protocol::Sync::maxEnvelopeHeaderBytes + Protocol::Sync::maxFrameBodyBytes);
    while (!client.didOverflow) {
        frame.clear();
        if (!client.syncConnection->popInteractiveFrame(frame))
            return;
        queueToClient(index, frame);
    }
}

namespace {

/// Writes to a client's descriptor so that a peer that hung up is an error
/// the caller reads, never a signal.
///
/// The process-wide disposition that `start` sets is not enough on its own:
/// a daemon is also built and torn down inside processes that never call it,
/// and its teardown says goodbye to every client, including one whose end is
/// already closed. A platform with `MSG_NOSIGNAL` suppresses the signal per
/// send; one without it suppresses it per socket with `SO_NOSIGPIPE`, once.
///
/// - Parameters:
///   - fileDescriptor: The client's descriptor, a socket or a pipe.
///   - bytes: What to write.
///   - isSignalSuppressed: Whether the socket option is already set; set on
///     the first write where the platform needs it.
/// - Returns: What `write` returns.
ssize_t writeToClient(int fileDescriptor, std::span<const std::uint8_t> bytes, bool& isSignalSuppressed)
{
#if defined(MSG_NOSIGNAL)
    (void)isSignalSuppressed;
    const ssize_t sent = ::send(fileDescriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    // A pipe takes no send flags; for it the process disposition is what
    // there is.
    if (sent >= 0 || errno != ENOTSOCK)
        return sent;
#elif defined(SO_NOSIGPIPE)
    if (!isSignalSuppressed) {
        const int suppress = 1;
        (void)::setsockopt(fileDescriptor, SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress));
        isSignalSuppressed = true;
    }
#else
    (void)isSignalSuppressed;
#endif
    return ::write(fileDescriptor, bytes.data(), bytes.size());
}

} // namespace

void Daemon::flushClient(std::size_t index)
{
    if (index >= m_clients.size())
        return;
    auto& c = m_clients[index];
    if (c.writeFailureDeadlineMilliseconds) {
        c.clearPendingOutput();
        return;
    }
    if (c.localClipboardCorrelation && c.hasPendingOutput()) {
        if (!isClipboardToolSelectionCurrent(c)) {
            c.clearPendingOutput();
            dropClient(index);
            return;
        }
    }
#ifdef UBIQUITTY_QUIC
    if (c.quicConnection) {
        // QUIC client: hand the buffered Sync frames to its relay stream.
        // sendStream only QUEUES (ngtcp2 forbids writing from inside a
        // read_pkt callback, the common caller); run()'s post-service
        // m_quic->flush() does the actual UDP write.
        if (c.hasPendingOutput() && m_quic) {
            const auto additional = m_quic->additionalStreamStorageBytes(c.quicConnection, c.pendingOutputBytes());
            const auto admission = imageSourceAdmission(c.clientID);
            if (admission.connection && admission.aggregate
                && (additional > admission.connection->availableBytes(admission.otherConnectionBytes)
                    || additional > admission.aggregate->availableBytes(admission.otherAggregateBytes)))
                preemptOutboundImages();
            m_quic->sendStream(c.quicConnection, { c.outputBuffer.data() + c.outputConsumed, c.pendingOutputBytes() });
            ++c.outputWrites;
            c.clearPendingOutput();
        }
        return;
    }
#endif
    // Drain via the consumed-bytes cursor: each partial write advances the cursor
    // instead of erasing the sent prefix, so a slow reader pulling a multi-MiB
    // backlog ~8 KB per wake costs O(n) total, not O(n^2) memmove.
    while (c.hasPendingOutput()) {
        ssize_t n = writeToClient(c.fileDescriptor.get(), { c.outputBuffer.data() + c.outputConsumed, c.pendingOutputBytes() }, c.isBrokenPipeSignalSuppressed);
        if (n > 0) {
            c.outputConsumed += static_cast<std::size_t>(n);
            ++c.outputWrites;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Bound the dead prefix while backlogged; a framed client keeps the
            // frame a write stopped inside.
            const std::optional<std::size_t> frameEnd = c.syncConnection ? c.advanceOutputFrameStart() : std::nullopt;
            c.compactOutputBuffer(frameEnd ? c.outputFrameStart : c.outputConsumed);
            return; // retry on POLLOUT
        }
        if (n < 0 && errno == EINTR)
            continue;
        // A stream may contain the peer's orderly Bye after its read end has
        // closed. Writes cannot deliver anything further, but the read side
        // keeps its final frames until EOF or the fixed drain budget expires.
        c.backlogByeDeadlineMilliseconds = 0;
        c.clearPendingOutput();
        if (c.syncConnection && !c.didOverflow && c.syncConnection->state() != Protocol::Sync::ConnectionState::Closed) {
            constexpr std::uint64_t writeFailureDrainMilliseconds = 100;
            c.writeFailureDeadlineMilliseconds = nowMilliseconds() + writeFailureDrainMilliseconds;
            drainScreenInteractive(index);
        } else {
            dropClient(index);
        }
        return;
    }
    c.clearPendingOutput(); // fully drained → release the buffer + reset the cursor
}

void Daemon::serviceClient(std::size_t index)
{
    if (m_quiescingPanes)
        return;
    // A local stream's first record may be a successor's handover request,
    // which no remote decoder reads; everything else continues below.
    if (index < m_clients.size() && !HandoverStartup::route(*this, index, nowMilliseconds(), m_clients[index].connectMilliseconds + helloGraceMilliseconds))
        return;
    constexpr std::size_t clientReadPassCapacityBytes = 64 * 1024;
    std::size_t drainedBytes = 0;
    char buffer[8192];
    for (;;) {
        if (index >= m_clients.size())
            return;
        // Inbound access gate: a client marked `didOverflow` this same pass
        // (kicked/dropped by another client's frame handling earlier in the
        // pfds loop, or a queueToClient output-cap trip) is awaiting reap at
        // the next top-of-loop — it must not still drive/read until then.
        if (m_clients[index].didOverflow)
            return;
        if (m_clients[index].writeFailureDeadlineMilliseconds) {
            if (nowMilliseconds() >= m_clients[index].writeFailureDeadlineMilliseconds
                || m_clients[index].writeFailureReadBytes >= clientReadPassCapacityBytes) {
                dropClient(index);
                return;
            }
            drainScreenInteractive(index);
        }
        // Checked per read, not once per pass: a frame this pass dispatched can
        // be the one whose answer filled the screen connection.
        if (isInputHeld(m_clients[index]))
            return;
        int fileDescriptor = m_clients[index].fileDescriptor.get();
        // recvmsg (not read) so a master PTY fd passed via SCM_RIGHTS for re-home
        // session re-home is captured; otherwise it behaves exactly like read.
        alignas(cmsghdr) char control[POSIX::receivedDescriptorControlCapacity];
        struct iovec iov;
        iov.iov_base = buffer;
        const LocalHandoffDescriptorPosition handoffPosition {
            .isSelectedHandoff = m_clients[index].localHandoffCorrelation != 0,
            .hasPendingDescriptor = static_cast<bool>(m_clients[index].pendingAdoptFileDescriptor),
            .pendingRecordBytes = m_clients[index].localAdopt ? m_clients[index].localAdopt->pendingBytes() : 0,
        };
        // Isolate the ancillary-data byte. A recvmsg spanning more than one
        // sender write cannot prove that a descriptor accompanied byte zero.
        iov.iov_len = handoffPosition.isSelectedHandoff
            ? (m_clients[index].localAdopt ? m_clients[index].localAdopt->readCapacity(sizeof(buffer)) : 0)
            : sizeof(buffer);
        if (m_clients[index].writeFailureDeadlineMilliseconds)
            iov.iov_len = std::min(iov.iov_len, clientReadPassCapacityBytes - m_clients[index].writeFailureReadBytes);
        if (!iov.iov_len) {
            // A handoff whose exchange has ended takes no more records: its
            // ingress is latched once its last record was answered. Bytes its
            // peer sends after that, or its hangup, belong to no exchange, and
            // leaving them unread would keep the socket readable — waking this
            // loop on every pass — while the peer waits for an answer that never
            // comes. So the connection ends once its reply has left; a peer
            // with more to ask opens a new exchange, as `resolveAdopt` does.
            const auto& client = m_clients[index];
            if (handoffPosition.isSelectedHandoff && client.localAdopt && !client.localAdopt->isAccepting() && !client.hasPendingOutput()) {
                char unread = 0;
                const ssize_t peeked = ::recv(fileDescriptor, &unread, 1, MSG_PEEK | MSG_DONTWAIT);
                if (peeked >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    dropClient(index);
            }
            return;
        }
        struct msghdr message {};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        ssize_t n = ::recvmsg(fileDescriptor, &message, POSIX::receivedDescriptorFlags);
        if (n > 0) {
            auto descriptor = POSIX::takeReceivedDescriptor(message);
            if (!descriptor) {
                dropClient(index);
                return;
            }
            const bool admitsDescriptor = handoffPosition.isSelectedHandoff
                ? admitsLocalAdoptDescriptorPosition(handoffPosition, static_cast<std::uint8_t>(buffer[0]), *descriptor ? 1 : 0)
                : !*descriptor;
            if (!admitsDescriptor) {
                dropClient(index);
                return;
            }
            if (*descriptor) {
                const auto& client = m_clients[index];
                if (!mayReceiveLocalHandoff(client) || client.pendingAdoptFileDescriptor
                    || !client.localAdopt || client.localAdopt->pendingBytes() || client.localClipboardCorrelation || client.localControl.hasPendingRequest()) {
                    dropClient(index);
                    return;
                }
                m_clients[index].pendingAdoptFileDescriptor = std::move(*descriptor);
            }
            feedClientFrames(index, std::as_bytes(std::span { buffer, static_cast<std::size_t>(n) }));
            if (index >= m_clients.size() || m_clients[index].didOverflow)
                return;
            if (m_clients[index].writeFailureDeadlineMilliseconds) {
                m_clients[index].writeFailureReadBytes += static_cast<std::size_t>(n);
                if (m_clients[index].writeFailureReadBytes >= clientReadPassCapacityBytes) {
                    dropClient(index);
                    return;
                }
            }
            drainedBytes += static_cast<std::size_t>(n);
            // Finish decoding this batch before yielding, so buffered frames
            // cannot be stranded when the socket has no more readable bytes.
            if (drainedBytes >= clientReadPassCapacityBytes) {
                return;
            }
            continue;
        }
        if (n == 0) {
            dropClient(index);
            return;
        } // peer closed
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        dropClient(index);
        return;
    }
}

std::size_t Daemon::heldInputRoom() const
{
    std::size_t held = 0;
    for (const auto& session : m_sessions) {
        if (session->screenSource)
            held += session->screenSource->heldInputBytes();
    }
    return held < maxHostHeldInputBytes ? maxHostHeldInputBytes - held : 0;
}

bool Daemon::isInputHeld(const Client& client) const
{
    // Input is bounded by its own window and the pane's bound, and an
    // interrupt or a PasteAbort behind a paste a program is not reading must
    // be read to be acted on: a program that reads nothing holds no client.
    return !client.writeFailureDeadlineMilliseconds && client.syncConnection && !client.syncConnection->wantsInput();
}

void Daemon::logHoldState() const
{
    for (std::size_t index = 0; index < m_clients.size(); ++index) {
        const Client& client = m_clients[index];
        const auto* screen = client.syncConnection.get();
        Ubiquitty::PAL::logLine(stderr,
            "daemon: client {} (id {}): input {}; screen connection {}, wants input {}, {} interactive and {} bulk bytes queued; "
            "{} output bytes pending, overflowed {}",
            index, client.clientID, isInputHeld(client) ? "held" : "read",
            screen ? "bound" : "none", screen ? screen->wantsInput() : true,
            screen ? screen->queuedInteractiveBytes() : std::size_t { 0 }, screen ? screen->queuedBulkBytes() : std::size_t { 0 },
            client.pendingOutputBytes(), client.didOverflow);
    }
    for (const auto& session : m_sessions) {
        Ubiquitty::PAL::logLine(stderr, "daemon: session {}: {} input bytes queued for its program, frame due {}, exited {}",
            session->id, session->stream ? session->stream->pendingWriteBytes() : std::size_t { 0 }, session->isFrameDue, session->shellExited);
    }
}

bool Daemon::admitsInboundCapacity(std::size_t index, std::size_t previous, std::size_t capacity) const
{
    if (index >= m_clients.size() || m_clients[index].didOverflow || capacity < previous)
        return false;
    const Client& client = m_clients[index];
    const std::size_t increase = capacity - previous;
    // Adopt and clipboard assemblies have their own body caps. Their retained
    // bytes count in the aggregate, not the one-frame per-client allowance.
    const std::size_t frames = heldInboundBytes(client)
        - (client.localAdopt ? client.localAdopt->retainedBytes() : 0)
        - (client.syncConnection ? client.syncConnection->inboundClipboardBytes() : 0);
    if (frames > maxClientInboundBytes || increase > maxClientInboundBytes - frames)
        return false;
    return admitsAssemblyCapacity(index, increase);
}

bool Daemon::admitsAssemblyCapacity(std::size_t index, std::size_t additional) const
{
    if (index >= m_clients.size() || m_clients[index].didOverflow)
        return false;
    std::size_t remaining = maxAggregateInboundBytes;
    for (const Client& other : m_clients) {
        const std::size_t held = heldInboundBytes(other);
        if (held > remaining)
            return false;
        remaining -= held;
    }
    return additional <= remaining;
}

class Daemon::LocalAdoptDriver final : public Ubiquitty::Session::AdoptTransactionDriver {
public:
    LocalAdoptDriver(Daemon& daemon, std::uint32_t requester, std::uint64_t correlation,
        PAL::UnixFileDescriptor master = {}, std::span<const std::uint8_t> body = {},
        std::optional<Ubiquitty::Session::AdoptPreparation> preparation = std::nullopt)
        : m_daemon(daemon)
        , m_clientID(requester)
        , m_correlation(correlation)
        , m_master(std::move(master))
        , m_body(body)
        , m_preparation(preparation)
    {
    }

    std::expected<void, Ubiquitty::Session::AdoptTransactionError> verifyAuthority(const Ubiquitty::Session::AdoptAuthority& authority) override
    {
        using Error = Ubiquitty::Session::AdoptTransactionError;
        const auto requester = index();
        if (!requester)
            return std::unexpected(Error::Unauthorised);
        const auto& client = m_daemon.m_clients[*requester];
        if (client.didOverflow || !client.localAdoptAuthority || *client.localAdoptAuthority != authority
            || !m_daemon.mayReceiveLocalHandoff(client))
            return std::unexpected(Error::Unauthorised);
        return {};
    }

    std::expected<std::unique_ptr<Ubiquitty::Session::AdoptCandidate>, Ubiquitty::Session::AdoptRejection> prepareCandidate(const Ubiquitty::Session::AdoptPreparation& preparation) override
    {
        using Rejection = Ubiquitty::Session::AdoptRejection;
        const auto requester = index();
        if (!requester || !m_preparation || *m_preparation != preparation || !m_master || m_body.size() != preparation.bodyBytes)
            return std::unexpected(Rejection::InvalidPayload);
        if (m_daemon.m_preparedLocalAdopts >= maxSessions || m_daemon.m_sessions.size() >= maxSessions - m_daemon.m_preparedLocalAdopts)
            return std::unexpected(Rejection::HostFull);
        auto restored = Protocol::Sync::decodeHandoverTerminal(m_body,
            Ubiquitty::Session::localShareTerminalLimits(Protocol::Local::maxAdoptBodyBytes), m_daemon.m_decodeMemory);
        if (!restored)
            return std::unexpected(restored.error() == Protocol::Sync::HandoverBodyError::AllocationRefused
                    ? Rejection::NoMemory
                    : Rejection::InvalidPayload);
        const auto authority = m_daemon.m_clients[*requester].localAdoptAuthority;
        if (!authority || !verifyAuthority(*authority))
            return std::unexpected(Rejection::NotOwner);
        const auto endpoint = PAL::capturePTYAdoptionIdentity(m_master.get(), authority->processID);
        if (!endpoint)
            return std::unexpected(Rejection::NotOwner);
        const auto columns = restored->terminal->columns;
        const auto rows = restored->terminal->rows;
        auto session = m_daemon.constructSession(columns, rows, {}, m_master.release(),
            static_cast<std::uint32_t>(authority->processID), std::nullopt, {}, {}, std::move(restored->terminal));
        if (!session)
            return std::unexpected(Rejection::InvalidCandidate);
        std::unique_ptr<HostedWorkspace> workspace(new (std::nothrow) HostedWorkspace(m_daemon.m_layoutIdentities, session->placement));
        if (!workspace)
            return std::unexpected(Rejection::NoMemory);
        workspace->workspaceID = session->id;
        workspace->paneSessions.emplace_back(session->placement.pane, session->id);
        // Reserve every inactive candidate's registry slots. Ordinary registry
        // writers preserve this spare capacity until activation or cancellation.
        m_daemon.m_sessions.reserve(m_daemon.m_sessions.size() + m_daemon.m_preparedLocalAdopts + 1);
        m_daemon.m_workspaces.reserve(m_daemon.m_workspaces.size() + m_daemon.m_preparedLocalAdopts + 1);
        std::unique_ptr<LocalAdoptCandidate> candidate(
            new (std::nothrow) LocalAdoptCandidate(m_daemon, std::move(session), std::move(workspace), *endpoint));
        if (!candidate)
            return std::unexpected(Rejection::NoMemory);
        candidate->decodeCharge = std::move(restored->decodeCharge);
        return std::unique_ptr<Ubiquitty::Session::AdoptCandidate>(std::move(candidate));
    }

    std::expected<std::unique_ptr<Ubiquitty::Session::AdoptCommitReservation>, Ubiquitty::Session::AdoptTransactionError> reserveCommittedReply(const Ubiquitty::Session::AdoptOutcome& outcome, const Ubiquitty::Session::AdoptCandidate& candidate) override
    {
        using Error = Ubiquitty::Session::AdoptTransactionError;
        const auto requester = index();
        const auto wire = Ubiquitty::Session::adoptOutcomeToWire(outcome, m_correlation);
        if (!requester || !wire || m_daemon.m_pendingLocalAdopt || m_daemon.m_pendingLocalAdoptReply
            || m_daemon.m_sessions.capacity() - m_daemon.m_sessions.size() < m_daemon.m_preparedLocalAdopts
            || m_daemon.m_workspaces.capacity() - m_daemon.m_workspaces.size() < m_daemon.m_preparedLocalAdopts)
            return std::unexpected(Error::ReplyUnavailable);
        const auto& staged = static_cast<const LocalAdoptCandidate&>(candidate);
        const auto& authority = m_daemon.m_clients[*requester].localAdoptAuthority;
        if (!authority || staged.target() != outcome.target || !staged.session || !staged.session->stream
            || !PAL::validatePTYAdoptionIdentity(staged.session->stream->fileDescriptor(), authority->processID, staged.endpoint))
            return std::unexpected(Error::Unauthorised);
        const auto encoded = Protocol::Local::encodeAdoptWireOutcome(*wire);
        if (!encoded)
            return std::unexpected(Error::ReplyUnavailable);
        std::unique_ptr<LocalAdoptCommitReply> reply(new (std::nothrow) LocalAdoptCommitReply);
        if (!reply)
            return std::unexpected(Error::NoMemory);
        reply->bytes = *encoded;
        reply->clientID = m_clientID;
        if (m_daemon.reserveLocalReplyBytes(*requester, reply->bytes.size()) != LocalReplyAdmission::Reserved)
            return std::unexpected(Error::ReplyUnavailable);
        return std::unique_ptr<Ubiquitty::Session::AdoptCommitReservation>(std::move(reply));
    }

    void activate(std::unique_ptr<Ubiquitty::Session::AdoptCandidate> candidate,
        std::unique_ptr<Ubiquitty::Session::AdoptCommitReservation> reservation) noexcept override
    {
        assert(!m_daemon.m_pendingLocalAdopt && !m_daemon.m_pendingLocalAdoptReply);
        // This driver alone constructs both concrete owners. No publication,
        // carrier write, allocation, or callback occurs under the decoder borrow.
        m_daemon.m_pendingLocalAdopt.reset(static_cast<LocalAdoptCandidate*>(candidate.release()));
        m_daemon.m_pendingLocalAdoptReply.reset(static_cast<LocalAdoptCommitReply*>(reservation.release()));
    }

    Ubiquitty::Session::AdoptRetirementEvidence retirementEvidence(const Ubiquitty::Session::AdoptAuthority& authority) override
    {
        return m_daemon.localAdoptRetirementEvidence(authority);
    }

private:
    std::optional<std::size_t> index() const
    {
        const auto found = std::ranges::find(m_daemon.m_clients, m_clientID, &Client::clientID);
        if (found == m_daemon.m_clients.end())
            return std::nullopt;
        return static_cast<std::size_t>(found - m_daemon.m_clients.begin());
    }

    Daemon& m_daemon;
    const std::uint32_t m_clientID;
    const std::uint64_t m_correlation;
    PAL::UnixFileDescriptor m_master;
    const std::span<const std::uint8_t> m_body;
    const std::optional<Ubiquitty::Session::AdoptPreparation> m_preparation;
};

std::expected<Ubiquitty::Session::AdoptAuthorityLease, Ubiquitty::Session::AdoptTransactionError> Daemon::admitLocalAdoptAuthority(std::size_t index, LocalAdoptDriver& driver)
{
    using Error = Ubiquitty::Session::AdoptTransactionError;
    if (!m_localAdoptTransactions || !m_trustedAdoptExecutables || index >= m_clients.size() || !m_clients[index].localAdoptAuthority)
        return std::unexpected(Error::Unauthorised);
    const auto& authority = *m_clients[index].localAdoptAuthority;
    LocalAdoptAuthorityRecord* available = nullptr;
    for (auto& record : m_localAdoptAuthorities) {
        if (record.lifetime && record.lifetime->authority() == authority)
            return m_localAdoptTransactions->admitAuthority(authority, driver);
        if (!record.lifetime && !available)
            available = &record;
    }
    if (!available)
        return std::unexpected(Error::Busy);
    auto lifetime = LocalAdoptLifetime::capture(m_clients[index].fileDescriptor.get(), authority, *m_trustedAdoptExecutables);
    if (!lifetime)
        return std::unexpected(lifetime.error());
    const auto lease = m_localAdoptTransactions->admitAuthority(authority, driver);
    if (!lease)
        return std::unexpected(lease.error());
    available->lifetime.emplace(std::move(*lifetime));
    available->lease = *lease;
    return *lease;
}

Ubiquitty::Session::AdoptRetirementEvidence Daemon::localAdoptRetirementEvidence(const Ubiquitty::Session::AdoptAuthority& authority)
{
    using Evidence = Ubiquitty::Session::AdoptRetirementEvidence;
    if (m_pendingLocalAdopt || m_pendingLocalAdoptReply)
        return Evidence::Unproven;
    for (auto& record : m_localAdoptAuthorities) {
        if (!record.lifetime || record.lifetime->authority() != authority || !record.lifetime->didExit())
            continue;
        // The serial reactor calls retirement only outside decoder callbacks.
        // Even an overflowed channel must be physically reaped first.
        if (std::ranges::any_of(m_clients, [&](const auto& client) {
                return client.localAdoptAuthority && *client.localAdoptAuthority == authority;
            }))
            return Evidence::ProcessGoneOnly;
        return Evidence::ProcessGoneAndAllChannelsAndOperationsRevokedAndDrained;
    }
    return Evidence::Unproven;
}

void Daemon::serviceLocalAdoptRetirement()
{
    if (!m_localAdoptTransactions || m_pendingLocalAdopt || m_pendingLocalAdoptReply)
        return;
    LocalAdoptDriver driver(*this, 0, 0);
    for (auto& record : m_localAdoptAuthorities) {
        if (!record.lifetime || !record.lease || !record.lifetime->didExit())
            continue;
        for (std::size_t index = 0; index < m_clients.size(); ++index) {
            const auto& authority = m_clients[index].localAdoptAuthority;
            if (authority && *authority == record.lifetime->authority() && !m_clients[index].didOverflow)
                dropClient(index);
        }
        if (m_localAdoptTransactions->retireAuthority(*record.lease, driver))
            record = {};
    }
}

void Daemon::receiveLocalAdopt(std::size_t index, std::span<const std::uint8_t> bytes)
{
    if (index >= m_clients.size() || m_clients[index].didOverflow || !m_clients[index].localAdopt)
        return;
    const auto clientID = m_clients[index].clientID;
    m_clients[index].isReadingFrames = true;
    const auto result = m_clients[index].localAdopt->consume(bytes, [&](Protocol::Local::AdoptWireMessage kind, std::size_t, std::size_t requiredCapacity) {
            auto& client = m_clients[index];
            if (client.clientID != clientID || client.didOverflow || !mayReceiveLocalHandoff(client))
                return false;
            const bool needsDescriptor = kind == Protocol::Local::AdoptWireMessage::Prepare;
            if (needsDescriptor != static_cast<bool>(client.pendingAdoptFileDescriptor)
                || (kind != Protocol::Local::AdoptWireMessage::Prepare && kind != Protocol::Local::AdoptWireMessage::Commit
                    && kind != Protocol::Local::AdoptWireMessage::Abort && kind != Protocol::Local::AdoptWireMessage::Status))
                return false;
            const auto previous = client.localAdopt->retainedBytes();
            return requiredCapacity >= previous && admitsAssemblyCapacity(index, requiredCapacity - previous); }, [&](const Protocol::Local::AdoptIngress::Frame& frame) {
            dispatchLocalAdopt(index, frame);
            return m_pendingLocalAdopt || m_clients[index].didOverflow || m_clients[index].isHandoffComplete
                ? Protocol::Sync::FrameDelivery::Stop : Protocol::Sync::FrameDelivery::Continue; });
    m_clients[index].isReadingFrames = false;
    // A table-recorded Commit cannot be rolled back by a malformed suffix or
    // lost requester. Publish before considering any carrier failure.
    finishLocalAdoptPublication();
    if (result.state == Protocol::Sync::FrameIngress::State::Refused || result.consumedBytes != bytes.size()) {
        dropClient(index);
        return;
    }
    m_clients[index].localAdopt->releaseRetained();
    noteInboundDecoder(index);
    flushClient(index);
}

void Daemon::dispatchLocalAdopt(std::size_t index, const Protocol::Local::AdoptIngress::Frame& frame)
{
    using namespace Ubiquitty::Session;
    using Kind = Protocol::Local::AdoptWireMessage;
    if (!mayReceiveLocalHandoff(m_clients[index]) || !m_localAdoptTransactions) {
        dropClient(index);
        return;
    }
    if (!m_localAdoptTransactions->advanceTime(nowMicroseconds())) {
        dropClient(index);
        return;
    }
    std::uint64_t correlation = 0;
    AdoptOperation operation;
    std::optional<AdoptPreparation> preparation;
    std::span<const std::uint8_t> body;
    if (frame.kind == Kind::Prepare) {
        const auto request = Protocol::Local::decodeAdoptWirePrepare(frame.record, maxLocalAdoptRecordBytes);
        if (!request) {
            dropClient(index);
            return;
        }
        const auto converted = adoptPreparationFromWire(request->preparation);
        if (!converted) {
            dropClient(index);
            return;
        }
        correlation = request->correlation;
        preparation = *converted;
        operation = preparation->operation;
        body = request->terminalBody;
    } else {
        const auto request = Protocol::Local::decodeAdoptWireCommand(frame.record);
        if (!request || request->kind != frame.kind || m_clients[index].pendingAdoptFileDescriptor) {
            dropClient(index);
            return;
        }
        const auto converted = adoptOperationFromWire(request->operation);
        if (!converted) {
            dropClient(index);
            return;
        }
        operation = *converted;
        correlation = request->correlation;
    }
    // Even duplicate Prepare consumes its newly received duplicate master:
    // the table may return retained evidence without calling the factory.
    LocalAdoptDriver driver(*this, m_clients[index].clientID, correlation,
        std::move(m_clients[index].pendingAdoptFileDescriptor), body, preparation);
    auto outcome = [&]() -> std::expected<AdoptOutcome, AdoptTransactionError> {
        const auto lease = admitLocalAdoptAuthority(index, driver);
        if (!lease)
            return std::unexpected(lease.error());
        switch (frame.kind) {
        case Kind::Prepare:
            return m_localAdoptTransactions->prepare(*lease, *preparation, driver);
        case Kind::Commit:
            return m_localAdoptTransactions->commit(*lease, operation, driver);
        case Kind::Abort:
            return m_localAdoptTransactions->abort(*lease, operation, driver);
        case Kind::Status:
            return m_localAdoptTransactions->status(*lease, operation, driver);
        case Kind::Outcome:
        case Kind::Refused:
            return std::unexpected(AdoptTransactionError::Unauthorised);
        }
        return std::unexpected(AdoptTransactionError::InvalidIdentity);
    }();
    if (m_pendingLocalAdopt)
        return; // Its exact admitted Committed reply leaves after publication.
    const auto append = [&](std::span<const std::uint8_t> reply) {
        if (reserveLocalReplyBytes(index, reply.size()) != LocalReplyAdmission::Reserved) {
            dropClient(index);
            return;
        }
        auto& client = m_clients[index];
        client.outputBuffer.insert(client.outputBuffer.end(), reply.begin(), reply.end());
    };
    if (!outcome) {
        const auto wire = adoptRefusedToWire(operation, outcome.error(), correlation);
        const auto encoded = wire ? Protocol::Local::encodeAdoptWireRefused(*wire)
                                  : std::unexpected(Protocol::Local::AdoptWireError::InvalidIdentity);
        if (!encoded) {
            dropClient(index);
            return;
        }
        append(*encoded);
        return;
    }
    const auto wire = adoptOutcomeToWire(*outcome, correlation);
    const auto encoded = wire ? Protocol::Local::encodeAdoptWireOutcome(*wire)
                              : std::unexpected(Protocol::Local::AdoptWireError::InvalidIdentity);
    if (!encoded) {
        dropClient(index);
        return;
    }
    append(*encoded);
    if (outcome->disposition != AdoptDisposition::Prepared && outcome->disposition != AdoptDisposition::Unknown) {
        m_clients[index].isHandoffComplete = true;
        armSessionExitDeadline(m_clients[index], nowMilliseconds());
    }
}

void Daemon::finishLocalAdoptPublication()
{
    auto candidate = std::move(m_pendingLocalAdopt);
    auto reply = std::move(m_pendingLocalAdoptReply);
    if (!candidate)
        return;
    assert(reply && m_sessions.size() < m_sessions.capacity() && m_workspaces.size() < m_workspaces.capacity());
    // Registry slots and the empty-text event ring were admitted before
    // Committed. This remains infallible even after the requester disconnects.
    m_workspaces.push_back(std::move(candidate->workspace));
    (void)publishSession(std::move(candidate->session));
    const auto found = std::ranges::find(m_clients, reply->clientID, &Client::clientID);
    if (found == m_clients.end() || found->didOverflow)
        return;
    assert(reply->bytes.size() <= found->outputBuffer.capacity() - found->outputBuffer.size());
    found->outputBuffer.insert(found->outputBuffer.end(), reply->bytes.begin(), reply->bytes.end());
    found->isHandoffComplete = true;
    armSessionExitDeadline(*found, nowMilliseconds());
}

void Daemon::feedClientFrames(std::size_t index, std::span<const std::byte> bytes)
{
    if (index >= m_clients.size() || m_clients[index].didOverflow || bytes.empty())
        return;
    if (m_clients[index].isReadingFrames) {
        dropClient(index);
        return;
    }
    // The stores decide before the batch does: a revocation committed before
    // these bytes are dispatched refuses them.
    if (!refreshCredentialStoresBeforeDispatch(index))
        return;
    auto& localClient = m_clients[index];
    if (localClient.localHandoffCorrelation) {
        receiveLocalAdopt(index, { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
        return;
    }
    if (localClient.localClipboardCorrelation) {
        feedLocalClipboard(index, { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
        return;
    }
    const auto localState = localClient.localControl.consume({ reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
    if (localState != LocalControlReader::State::Ordinary) {
        const auto peer = localControlPeer(localClient);
        if (!peer.isSameUserLocalStream || peer.hasRemoteGreeting || peer.hasDescriptors || localState == LocalControlReader::State::Refused
            || localState == LocalControlReader::State::Consumed) {
            dropClient(index);
            return;
        }
        if (localState == LocalControlReader::State::Awaiting) {
            noteInboundDecoder(index);
            return;
        }
        const auto request = localClient.localControl.take();
        noteInboundDecoder(index);
        if (!request) {
            dropClient(index);
            return;
        }
        const auto clientID = localClient.clientID;
        if (request->operation == Protocol::Local::LocalControlOperation::Clipboard) {
            if (!bindLocalClipboardHelper(index)) {
                dropClient(index);
                return;
            }
            localClient.localClipboardCorrelation = request->correlation;
            feedLocalClipboard(index, { reinterpret_cast<const std::uint8_t*>(bytes.data()) + localClient.localControl.consumedBytes(), bytes.size() - localClient.localControl.consumedBytes() });
            return;
        }
        if (request->operation == Protocol::Local::LocalControlOperation::RemoteRelay) {
            // The bridge marks, before the client's first byte, that the
            // stream it relays came over SSH. Only this daemon's own bridge is
            // lowered by it. Any other same-user peer's is acknowledged as not
            // applied rather than applied: a program is already less than
            // Local, and Remote would give it a remote owner's rememberable
            // grants. The bridge attaches only on an applied mark, and the
            // rest of the connection is the client's ordinary stream.
            const bool isMarked = admitsLocalControl(localControlPeer(localClient));
            localClient.isRelayedFromSSH = isMarked;
            // The instance names this daemon to its own bridge only.
            const auto reply = Protocol::Local::encodeLocalRemoteRelayReply({ .correlation = request->correlation, .daemonInstanceID = isMarked ? m_daemonInstanceID : 0, .isMarked = isMarked });
            if (!reply) {
                dropClient(index);
                return;
            }
            queueToClient(index, *reply);
            flushClient(index);
            if (index >= m_clients.size() || m_clients[index].clientID != clientID || m_clients[index].didOverflow)
                return;
            m_clients[index].localControl.continueAsCarrier();
            const std::size_t marked = m_clients[index].localControl.consumedBytes();
            if (marked < bytes.size())
                feedClientFrames(index, bytes.subspan(marked));
            return;
        }
        if (!admitsLocalControl(localControlPeer(localClient))) {
            dropClient(index);
            return;
        }
        if (request->operation == Protocol::Local::LocalControlOperation::SessionHandoff) {
            if (!m_localAdoptTransactions || bytes.size() != localClient.localControl.consumedBytes()) {
                dropClient(index);
                return;
            }
            localClient.localAdopt.reset(new (std::nothrow) Protocol::Local::AdoptIngress(maxLocalAdoptRecordBytes));
            if (!localClient.localAdopt) {
                dropClient(index);
                return;
            }
            localClient.localHandoffCorrelation = request->correlation;
            if (m_trustedAdoptExecutables)
                localClient.localAdoptAuthority = currentLocalAdoptAuthority(localClient.fileDescriptor.get(), *m_trustedAdoptExecutables);
            if (!mayReceiveLocalHandoff(localClient)) {
                dropClient(index);
                return;
            }
            const auto ready = Protocol::Local::encodeLocalHandoffReady({ .correlation = request->correlation,
                .daemonInstanceID = m_daemonInstanceID,
                .status = Protocol::Local::HandoffReadyStatus::Ready });
            queueToClient(index, ready);
            noteInboundDecoder(index);
            flushClient(index);
            return;
        }
        if (request->operation == Protocol::Local::LocalControlOperation::QUICToken) {
            const auto admission = reserveLocalQUICTokenReply(index);
            if (admission != LocalReplyAdmission::Reserved) {
                dropClient(index);
                return;
            }
            std::string certificate;
            LocalQUICListener listener;
#ifdef UBIQUITTY_QUIC
            if (m_quic && m_quic->fileDescriptor() >= 0) {
                certificate = m_quic->certificateSHA256();
                listener = { .isEnabled = true, .port = m_quic->boundPort(), .certificateSHA256 = certificate };
            }
#endif
            const auto reply = handleLocalQUICToken(request->correlation, m_daemonInstanceID,
                localControlPeer(m_clients[index]), listener, admission, [&] {
                    const Protocol::Sync::ScopeSet scope { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive,
                        Protocol::Sync::Scope::Control, Protocol::Sync::Scope::RegionRead,
                        Protocol::Sync::Scope::NewSession, Protocol::Sync::Scope::FileRead };
                    return mintQUICToken(clientID, scope, true);
                });
            const auto encoded = Protocol::Local::encodeLocalQUICTokenReply(reply);
            if (encoded && index < m_clients.size() && m_clients[index].clientID == clientID && !m_clients[index].didOverflow) {
                queueToClient(index, encoded->view());
                flushClient(index);
            } else
                dropClient(index);
            return;
        }
        if (request->operation != Protocol::Local::LocalControlOperation::ReloadCredentials) {
            dropClient(index);
            return;
        }
        const auto reply = handleLocalReload(*request, m_daemonInstanceID, localControlPeer(m_clients[index]), [&] { reloadCredentialStores(); });
        if (index < m_clients.size() && m_clients[index].clientID == clientID && !m_clients[index].didOverflow) {
            queueToClient(index, encodeLocalControlReply(reply));
            flushClient(index);
        }
        return;
    }
    m_clients[index].isReadingFrames = true;
    const std::uint32_t clientID = m_clients[index].clientID;
    // Canonical header admission owns direction, state, size and capacity
    // checks before allocating a body. There is no second carrier grammar.
    receiveScreenBytes(index, { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
    if (index >= m_clients.size() || m_clients[index].clientID != clientID)
        return;
    m_clients[index].isReadingFrames = false;
    noteInboundDecoder(index);
    retireStandingConsentWithdrawals();
}

void Daemon::noteInboundDecoder(std::size_t index)
{
    if (index >= m_clients.size())
        return;
    auto& c = m_clients[index];
    if (!c.localControl.hasPendingRequest() && (!c.localAdopt || !c.localAdopt->pendingBytes())
        && (!c.localHandoffCorrelation || c.isHandoffComplete)
        && !(c.localClipboardCorrelation && c.localClipboard.hasPendingRequest())
        && (!c.syncConnection || c.syncConnection->pendingInboundFrameBytes() == 0)) {
        // Every selected reader has drained its partial frame.
        c.partialFrameDeadlineMilliseconds = 0;
    } else {
        // A partial frame sits buffered — restart its clock. Both callers reach
        // here only on drain progress, so the deadline measures SILENCE rather
        // than total transfer time: re-arming only when unset would bound how
        // long a frame may take to arrive, and drop a peer that is sending as
        // fast as the link allows. What bounds the memory is the per-client and
        // aggregate caps below, which a trickling peer trips on volume.
        c.partialFrameDeadlineMilliseconds = nowMilliseconds() + partialFrameDeadline();
    }
}

std::size_t Daemon::heldInboundBytes(const Client& client)
{
    return (client.localControl.hasPendingRequest() ? client.localControl.pendingBytes() : 0)
        + (client.localClipboardCorrelation ? client.localClipboard.retainedBytes() : 0)
        + (client.localAdopt ? client.localAdopt->retainedBytes() : 0)
        + client.consentRecipients.capacity() * sizeof(ConsentRecipient)
        + (client.attachConsentApproval ? client.attachConsentApproval->requesterPrincipal.capacity() + client.attachConsentApproval->approverPrincipal.capacity()
                    + client.attachConsentApproval->approverIdentity.capacity()
                                        : 0)
        + (client.reclaimCall ? client.reclaimCall->principal.capacity() + client.reclaimCall->requestID.asString().capacity() : 0)
        + (client.pasteCall ? Client::pasteCallMetadataBytes + client.pasteCall->upload.heldBytes() : 0)
        + (client.syncConnection ? client.syncConnection->inboundClipboardBytes() + client.syncConnection->inboundFrameBytes() : 0);
}

void Daemon::enforceInboundBudget()
{
    const std::uint64_t now = nowMilliseconds();
    std::size_t aggregate = 0;
    for (auto& c : m_clients)
        aggregate += heldInboundBytes(c);
    // From the end so erases keep lower indices valid.
    for (std::size_t i = m_clients.size(); i-- > 0;) {
        auto& c = m_clients[i];
        if (c.writeFailureDeadlineMilliseconds && now >= c.writeFailureDeadlineMilliseconds) {
            dropClient(i);
            continue;
        }
        const std::size_t footprint = heldInboundBytes(c);
        const std::size_t frames = footprint - (c.localAdopt ? c.localAdopt->retainedBytes() : 0)
            - (c.syncConnection ? c.syncConnection->inboundClipboardBytes() : 0);
        const bool overPerClient = frames > maxClientInboundBytes;
        // A held client is silent because this daemon stopped reading it, so
        // its partial frame's clock restarts rather than running.
        if (c.partialFrameDeadlineMilliseconds != 0 && isInputHeld(c))
            c.partialFrameDeadlineMilliseconds = now + partialFrameDeadline();
        const bool stalled = c.partialFrameDeadlineMilliseconds != 0 && now >= c.partialFrameDeadlineMilliseconds;
        const bool overAggregate = aggregate > maxAggregateInboundBytes;
        if (overPerClient || stalled || overAggregate) {
            // Name which cap fired, with its numbers. All three look identical
            // from the peer — the connection closes mid-write — so a report of
            // one is a report of any, and a test that says "the daemon dropped
            // us" cannot say which mechanism to go and read. The stall age is
            // the interesting one: the deadline measures the gap since this
            // frame last advanced, so a busy daemon and a silent peer both
            // reach it, and only the age distinguishes them.
            std::string fired;
            const auto note = [&fired](const char* cap) {
                if (!fired.empty())
                    fired += " + ";
                fired += cap;
            };
            if (overPerClient)
                note("per-client cap");
            if (stalled)
                note("completion deadline");
            if (overAggregate)
                note("aggregate cap");
            const std::uint64_t stalledForMilliseconds = stalled
                ? now - (c.partialFrameDeadlineMilliseconds - partialFrameDeadline())
                : 0;
            Ubiquitty::PAL::logLine(stderr,
                "daemon: inbound budget dropped client {} — {} (pending {}, retained {}, "
                "aggregate {} of {}, stalled for {} ms)",
                i, fired, (c.localAdopt ? c.localAdopt->pendingBytes() : 0) + (c.syncConnection ? c.syncConnection->pendingInboundFrameBytes() : 0), footprint, aggregate, maxAggregateInboundBytes,
                stalledForMilliseconds);
            if (overAggregate)
                aggregate -= footprint; // this client's bytes are about to be freed
            dropClient(i);
        }
    }
}

void Daemon::enforceOutboundBudget()
{
    // New immutable image owners may use only room left by every retained
    // payload capacity. If higher-priority output grew meanwhile, free those
    // owners before applying the existing nonimage drop policy below.
    if (m_imageSourceBudget && m_imageSourceBudget->heldBytes()) {
        std::size_t held = m_imageSourceBudget->heldBytes();
        bool isOver = false;
        for (const auto& client : m_clients) {
            const auto bytes = outboundPayloadCapacity(client);
            held += bytes;
            const auto sources = client.imageSourceBudget ? client.imageSourceBudget->heldBytes() : 0;
            isOver = isOver || bytes > clientBufferHighWater || sources > clientBufferHighWater - std::min(bytes, clientBufferHighWater);
        }
        if (isOver || held > maxAggregateOutboundBytes)
            preemptOutboundImages();
    }
    // Bound the TOTAL queued output across clients — the per-client high-water alone
    // admits N x 16 MiB. Sum each client's pending output (the AF_UNIX staging buffer
    // plus the QUIC stream/datagram backlog) and drop newest-first (like the inbound
    // aggregate) until the total is back under the cap.
    std::size_t aggregate = 0;
    for (const auto& c : m_clients) {
        aggregate += c.pendingOutputBytes();
#ifdef UBIQUITTY_QUIC
        if (c.quicConnection && m_quic)
            aggregate += m_quic->pendingBytes(c.quicConnection);
#endif
    }
    for (std::size_t i = m_clients.size(); i-- > 0 && aggregate > maxAggregateOutboundBytes;) {
        auto& c = m_clients[i];
        std::size_t footprint = c.pendingOutputBytes();
#ifdef UBIQUITTY_QUIC
        if (c.quicConnection && m_quic)
            footprint += m_quic->pendingBytes(c.quicConnection);
#endif
        // A drop here and a drop by an inbound cap read identically from
        // outside: the connection closes mid-write in both. The inbound caps
        // name themselves, and so does this one: silent, it would leave a peer
        // reporting "the daemon dropped us" to rule the inbound three out by
        // their silence and be left with a path that names nothing either.
        Ubiquitty::PAL::logLine(stderr,
            "daemon: outbound budget dropped client {} — aggregate cap (queued {}, aggregate {} of {})",
            i, footprint, aggregate, maxAggregateOutboundBytes);
        aggregate -= footprint;
        dropClient(i);
    }
}

std::expected<Protocol::Sync::HostIdentity, Protocol::Sync::Bye> Daemon::admitGreeting(
    std::uint32_t clientID, const Protocol::Sync::Greet& greet)
{
    using Protocol::Sync::AdmissionTarget;
    using Protocol::Sync::ByeReason;
    const auto refuse = [this](ByeReason reason, std::string detail) {
        return std::unexpected(Protocol::Sync::spokenBye(m_wireVersion, std::string(Protocol::Sync::appVersion()),
            hostPlatformName(), reason, std::move(detail)));
    };
    const auto found = std::ranges::find(m_clients, clientID, &Client::clientID);
    if (found == m_clients.end() || found->didOverflow)
        return refuse(ByeReason::SessionGone, "The connection is no longer available.");
    Client& client = *found;
    if (client.helloSeen)
        return refuse(ByeReason::DuplicateHandshake, "This connection has already greeted the host.");

    // The portable reader has already checked the version and complete grammar.
    // A label is only a claim; it can lower a verified local binary to an agent,
    // never promote a remote peer to a person.
    const auto& request = greet.request;
    client.helloSeen = true;
    client.agentLabel = Ubiquitty::boundedUTF8String(request.agentLabel, Protocol::Sync::maxAgentLabelBytes);
    client.intents = {};
    client.intents.set(Protocol::Sync::HelloIntent::AttachesWorkspace, request.target == AdmissionTarget::Workspace);
    client.intents.set(Protocol::Sync::HelloIntent::RequestsManagement, request.target == AdmissionTarget::Management);
    client.intents.set(Protocol::Sync::HelloIntent::ServesClipboard, request.servesClipboard);
    if (greet.credential) {
        // A bootstrap connection may replace its in-memory credential here,
        // after the batch gate exempted it from store-bound authority.
        if (!readConsentList(consentStorePath()).isWhole)
            return refuse(ByeReason::TokenInvalid, "The credential stores could not be read completely.");
        const std::string credential(greet.credential->begin(), greet.credential->end());
        const auto verdict = Host::verifyToken(m_tokenPath, credential, static_cast<std::uint64_t>(::time(nullptr)));
        if (!verdict.ok)
            return refuse(verdict.reason, "The access token was not accepted.");
        client.hasToken = true;
        client.tokenScope = verdict.scope;
        client.tokenID = verdict.tokenID;
        client.tokenExpiryUnix = verdict.expiryUnix;
        client.hasBootstrapToken = false;
        client.isInteractiveUser = false;
    } else if (client.hasToken && !client.hasBootstrapToken && !m_areCredentialStoresWhole)
        return refuse(ByeReason::TokenInvalid, "The credential stores could not be read completely.");
    else if (!client.hasToken
        && (originOf(client) == Ubiquitty::CapabilityOrigin::Remote || (m_requireToken && originOf(client) != Ubiquitty::CapabilityOrigin::Local)))
        return refuse(ByeReason::TokenMissing, "An access token is required to connect to this host.");

    const bool isUnbound = request.target == AdmissionTarget::Control || request.target == AdmissionTarget::Management;
    const auto requiredScope = isUnbound
        ? (request.target == AdmissionTarget::Management ? Protocol::Sync::Scope::Manage : Protocol::Sync::Scope::Control)
        : Protocol::Sync::Scope::Observe;
    if (!tokenScopeAllows(client, requiredScope)) {
        std::string_view asked = "attaching to a session";
        if (request.target == AdmissionTarget::Management)
            asked = "a management connection";
        else if (isUnbound)
            asked = "a control connection, which lists and ends sessions,";
        return refuse(ByeReason::OutsideScope, scopeRefusal(client, requiredScope, asked));
    }
    recordManagementTokenUse(client);

    const auto inherited = std::ranges::find(m_inheritedHostIdentities, request.expectedDaemonInstanceID, &AttachmentHostIdentity::daemonInstanceID);
    const bool isInheritedPin = request.expectedDaemonInstanceID && request.expectedDaemonInstanceID != m_daemonInstanceID;
    const bool isGoneInstance = isInheritedPin && inherited == m_inheritedHostIdentities.end();
    // A saved reference to a workspace an earlier instance held names one
    // workspace here by that instance and the id it held it under, whichever
    // id it is listed by now: a record of it, for an instance that no longer
    // runs, or the workspace live here, which a handover from an instance
    // this one succeeded brought, or which an earlier client's reference
    // recreated from its record. A reference two of them claim, and one none
    // claims, is answered by nothing else: not a blank session, not another
    // workspace (`findWorkspaceReference`).
    const WorkspaceReference referenced = [&]() -> WorkspaceReference {
        if (!isInheritedPin || request.target != AdmissionTarget::Workspace || !request.workspaceID)
            return {};
        const auto records = isGoneInstance ? ReferenceSearch::Included : ReferenceSearch::Excluded;
        const auto live = request.sshHost.empty() ? ReferenceSearch::Included : ReferenceSearch::Excluded;
        auto found = findWorkspaceReference(request.expectedDaemonInstanceID, request.workspaceID, records, live);
        // A host on a sibling socket that stopped after this one started left
        // records nothing here has read. They are looked for only when nothing
        // answers, and only for a connection that may restore what they hold,
        // so no other greeting reads the directory.
        if (!found.isFound() && isGoneInstance && !isAgentClient(client) && tokenScopeAllows(client, Protocol::Sync::Scope::NewSession)
            && takeInStoppedSiblingRecords())
            found = findWorkspaceReference(request.expectedDaemonInstanceID, request.workspaceID, records, live);
        return found;
    }();
    if (!m_daemonInstanceID || (isGoneInstance && !referenced.isFound()))
        return refuse(ByeReason::SessionGone, "The requested host instance is no longer serving this connection.");
    // A workspace a handover brought keeps the sessions it runs, and answers
    // a reference to the instance that handed it over unless that reference
    // resumes an attachment the instance issued.
    HostedWorkspace* handedOverWorkspace = isGoneInstance ? nullptr : referenced.live;
    if (isInheritedPin && !isGoneInstance) {
        const Session* target = !isUnbound && request.sessionID ? findSession(request.sessionID) : nullptr;
        const auto proven = m_attachments->provenAttachment(clientID);
        const bool isInheritedResumeHeld = target && target->screenSource && request.sshHost.empty()
            && m_attachments->holdsResumablePane(inherited->issuer, principalOf(client), target->screenSource->paneID(), proven, nowMicroseconds());
        // An attachment the predecessor issued resumes as its own. A client
        // that resumes none holds no witness of the predecessor, and accepts
        // the instance that runs now only when the answer names the one its
        // reference named: such a reference is answered from the workspace.
        const bool isResuming = greet.resume.has_value() || proven.has_value();
        if (handedOverWorkspace && isResuming && isInheritedResumeHeld)
            handedOverWorkspace = nullptr;
        else if (!handedOverWorkspace && !isInheritedResumeHeld)
            return refuse(ByeReason::SessionGone, "The requested session is not held by this attachment.");
    }

    Protocol::Sync::HostIdentity identity;
    identity.hostInstanceID = m_hostInstanceID;
    identity.wireVersion = m_wireVersion;
    identity.mayDrive = tokenScopeAllows(client, Protocol::Sync::Scope::Drive);
    identity.verifiedKind = isAgentClient(client) ? Protocol::Sync::ClientKind::Agent : Protocol::Sync::ClientKind::Human;
    identity.maxRole = maxRoleOf(client);
    identity.principal = principalOf(client);
    identity.selection = {
        .target = request.target,
        .daemonInstanceID = m_daemonInstanceID,
        .socketName = socketBasename(),
        .buildFlags = Protocol::Sync::localHostBuildFlags().toRaw(),
        .hostPlatform = hostPlatformName(),
    };
    if (isUnbound) {
        client.sessionID = 0;
        client.workspaceID = 0;
        return identity;
    }

    // A reference to an instance that no longer runs is answered by a
    // restoration: its record, recreated now, or the workspace an earlier
    // client's reference recreated, which this one joins under the same
    // answer without starting a shell. Either needs the authority to restore.
    if (isGoneInstance && referenced.isFound()) {
        if (isAgentClient(client) || !tokenScopeAllows(client, Protocol::Sync::Scope::NewSession))
            return refuse(ByeReason::OutsideScope, "This connection cannot restore a workspace.");
        // A recreated workspace's sessions are numbered afresh, so the session
        // the reference names is not one of them.
        Session* restored = nullptr;
        if (const HostedWorkspace* live = referenced.live)
            restored = answeringSession(*live, live->wasRestoredAfterHostRestart ? 0 : request.sessionID);
        else
            restored = restoreWorkspace(referenced.recordedWorkspaceID);
        if (!restored)
            return refuse(ByeReason::SessionGone, "The requested workspace could not be restored.");
        identity.selection.sessionID = restored->id;
        identity.selection.workspaceID = hostedWorkspaceFor(*restored).workspaceID;
        identity.selection.restoredFromDaemonInstanceID = request.expectedDaemonInstanceID;
        client.sessionID = restored->id;
        client.workspaceID = identity.selection.workspaceID;
        return identity;
    }

    const bool shouldNameWorkspace = !request.sessionID && !request.workspaceName.empty();
    Session* session = request.sessionID ? findSession(request.sessionID)
                                         : (shouldNameWorkspace ? findSessionByWorkspaceName(request.workspaceName) : nullptr);
    if (handedOverWorkspace) {
        // What the reference names is the workspace, under the id it holds
        // here: the referenced session while it runs there, else another.
        session = answeringSession(*handedOverWorkspace, request.sessionID);
        if (!session)
            return refuse(ByeReason::SessionGone, "The requested workspace has no session left on this host.");
        identity.selection.restoredFromDaemonInstanceID = request.expectedDaemonInstanceID;
    }
    // An exact pane that vanished is never interpreted as a workspace ID or a
    // fresh shell. The caller can explicitly ask to create another session.
    if (request.sessionID && !session)
        return refuse(ByeReason::SessionGone, "The requested session is no longer on this host.");
    if (request.target == AdmissionTarget::Workspace) {
        if (isAgentClient(client))
            return refuse(ByeReason::OutsideScope, "This connection cannot open a workspace.");
        if (request.workspaceID) {
            if (session) {
                const auto* workspace = workspaceHolding(session->id);
                const uint32_t expectedWorkspaceID = handedOverWorkspace ? handedOverWorkspace->workspaceID : request.workspaceID;
                if (!workspace || workspace->workspaceID != expectedWorkspaceID)
                    return refuse(ByeReason::OutsideScope, "The requested session does not belong to this workspace.");
            } else {
                for (const auto& workspace : m_workspaces) {
                    if (workspace->workspaceID != request.workspaceID)
                        continue;
                    for (const auto& [pane, sessionID] : workspace->paneSessions) {
                        session = findSession(sessionID);
                        if (session)
                            break;
                    }
                    break;
                }
            }
        }
    }

    bool needsConsent = false;
    if (session) {
        needsConsent = isAgentClient(client) && !agentHasStandingGrant(client, *session);
        if (needsConsent) {
            if (!sessionHasAskableHuman(session->id))
                return refuse(ByeReason::NoConsent, "No one connected can be asked to approve access to this session.");
            const auto key = grantKeyFor(client);
            std::size_t pendingCount = 0;
            std::size_t sessionPendingCount = 0;
            bool hasDuplicateRequest = false;
            for (const Client& requester : m_clients) {
                if (!requester.consentPending)
                    continue;
                ++pendingCount;
                if (requester.consentSessionID == session->id) {
                    ++sessionPendingCount;
                    hasDuplicateRequest |= !key.empty() && grantKeyFor(requester) == key;
                }
            }
            if (pendingCount >= maxClients / 2 || sessionPendingCount >= 8 || hasDuplicateRequest)
                return refuse(ByeReason::NoConsent, "Another access request must be resolved before this connection can continue.");
        }
    } else {
        const RestorableWorkspace* restorable = nullptr;
        if (request.target == AdmissionTarget::Workspace && request.workspaceID && request.expectedDaemonInstanceID == m_daemonInstanceID)
            restorable = findRestorableWorkspace(request.workspaceID);
        else if (!request.workspaceID && shouldNameWorkspace)
            restorable = findRestorableWorkspaceByName(request.workspaceName);
        if (restorable) {
            if (isAgentClient(client) || !tokenScopeAllows(client, Protocol::Sync::Scope::NewSession))
                return refuse(ByeReason::OutsideScope, "This connection cannot restore a workspace.");
            session = restoreWorkspace(restorable->workspaceID);
            if (!session)
                return refuse(ByeReason::SessionGone, "The requested workspace could not be restored.");
        } else {
            if (request.isAttachOnly || request.workspaceID)
                return refuse(ByeReason::SessionGone, "The requested session or workspace is no longer on this host.");
            if (!tokenScopeAllows(client, Protocol::Sync::Scope::NewSession))
                return refuse(ByeReason::OutsideScope, scopeRefusal(client, Protocol::Sync::Scope::NewSession, "creating a session"));
            std::vector<std::string> remoteCommand;
            if (!request.sshHost.empty()) {
                if (!Transport::validateSSHHost(request.sshHost))
                    return refuse(ByeReason::RemoteHostRefused, "The SSH destination is not valid.");
                remoteCommand = Network::buildSSHArguments(remoteLaunchProgram, request.sshHost);
                if (remoteCommand.empty())
                    return refuse(ByeReason::RemoteHostRefused, "The SSH destination could not be opened.");
            }
            const auto creatorKey = isAgentClient(client) ? grantKeyFor(client) : std::string();
            session = createSession(greet.facts.viewportColumns, greet.facts.viewportRows, remoteCommand, -1, 0, std::nullopt, creatorKey);
            if (!session)
                return refuse(ByeReason::HostFull, "The host could not start a new session.");
            if (shouldNameWorkspace)
                session->workspaceName = request.workspaceName;
            session->endsWhenDetached = request.endsWhenDetached;
            if (!remoteCommand.empty())
                session->sshHost = request.sshHost;
            scheduleLayoutRecordWrite(hostedWorkspaceFor(*session));
        }
    }

    const auto sizeStatus = session->screenSource ? session->screenSource->geometry().status() : Protocol::Sync::GeometryStatus {};
    // A pane laid out beside others takes its size from its place among them,
    // as the geometry arbiter's placement says; no window's size is its own.
    const bool mayResize = !needsConsent && identity.mayDrive && !isAgentClient(client)
        && request.target == AdmissionTarget::Session && request.sessionID && !sizeStatus.isPinned && !sizeStatus.isOverridden
        && placementOf(session->id) == Protocol::Sync::PanePlacement::WholeWindow;
    if (mayResize && session->stream && greet.facts.viewportColumns && greet.facts.viewportRows
        && (session->term->columns != greet.facts.viewportColumns || session->term->rows != greet.facts.viewportRows)) {
        const bool hasOtherClients = std::ranges::any_of(m_clients, [&](const Client& other) {
            return other.clientID != clientID && other.subscriptionFor(session->id);
        });
        if (!hasOtherClients) {
            const int columns = clampColumns(greet.facts.viewportColumns);
            const int rows = clampRows(greet.facts.viewportRows);
            const auto pixels = Ubiquitty::reportedTextAreaPixels(*session->term);
            session->stream->setWindowSize(columns, rows, pixels.width, pixels.height);
            const bool foregroundIsShell = session->stream->process().foregroundIsShell();
            Ubiquitty::resize(*session->term, rows, columns,
                Ubiquitty::shouldReflowOnResize(session->term.get(), foregroundIsShell) ? Ubiquitty::Modes::ReflowMode::Reflow : Ubiquitty::Modes::ReflowMode::Fixed);
            session->isFrameDue = true;
        }
    }
    identity.selection.sessionID = session->id;
    identity.selection.workspaceID = hostedWorkspaceFor(*session).workspaceID;
    if (needsConsent) {
        client.consentPending = true;
        client.consentAction = Client::ConsentAction::Attach;
        client.consentSessionID = session->id;
        client.consentRequestID = m_nextConsentRequestID++;
        client.consentDeadlineMilliseconds = nowMilliseconds() + consentTimeoutMilliseconds;
        client.consentPaneID = session->screenSource->paneID();
        identity.pendingConsent = Protocol::Sync::ConsentBinding { .requestID = client.consentRequestID, .sessionID = session->id };
        sendConsentRequestToHumans(client);
    } else {
        // Subscription publication waits for the table to admit the attachment
        // and queue Welcome. In particular, a failed resume cannot momentarily
        // become another pane's observer or geometry owner.
        client.sessionID = session->id;
        client.workspaceID = identity.selection.workspaceID;
    }
    return identity;
}

// QUIC data-plane.
// A QUIC connection is just another Client (fileDescriptor -1): inbound
// stream bytes feed the same HostConnection as a socket client's,
// outbound frames queue via flushClient's QUIC branch. So HELLO, the
// Sync planes and the agent frames flow over QUIC unchanged — but only
// after the connection presents a valid one-time token (the QUIC port faces
// the network; the local socket is uid-gated, so only QUIC is token-gated).

std::string Daemon::mintQUICToken(std::uint32_t mintingClientID, Protocol::Sync::ScopeSet scope, bool isInteractiveUser) // bool-ok: carries the token record's own interactive-user mark, set only by a Local-origin minter; the flag is the mark, and a second spelling of it could disagree
{
    uint8_t raw[32];
    PAL::UnixFileDescriptor fileDescriptor(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    if (!fileDescriptor)
        return {};

    std::size_t filled = 0;
    while (filled < sizeof(raw)) {
        ssize_t n = ::read(fileDescriptor.get(), raw + filled, sizeof(raw) - filled);
        if (n > 0) {
            filled += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return {};
    }

    std::string token(reinterpret_cast<const char*>(raw), sizeof(raw));
    const std::uint64_t now = nowMilliseconds();
    pruneExpiredQUICTokens(now); // GC at mint so the cap eviction drops dead ones first
    if (m_pendingQUICTokens.size() >= maxPendingQUICTokens) {
        // Self-eviction only: a same-uid mint flood must never evict
        // ANOTHER client's still-pending token — that's a local DoS against a
        // peer awaiting its own bootstrap mint. Drop this client's own oldest
        // pending token (first match = oldest: push_back keeps insertion
        // order, pruning above never reorders survivors) if it has one; a
        // client with none pending yet just fails this mint closed rather
        // than taking a stranger's slot — retryable, and far safer than
        // silently invalidating someone else's in-flight bootstrap.
        auto ownOldest = std::find_if(m_pendingQUICTokens.begin(), m_pendingQUICTokens.end(), [mintingClientID](const PendingQUICToken& p) { return p.mintingClientID == mintingClientID; });
        if (ownOldest == m_pendingQUICTokens.end())
            return {};
        m_pendingQUICTokens.erase(ownOldest);
    }
    // Bake the minter's scope and a per-token principal (digest) into the pending
    // record so consumeQUICToken can carry them onto the QUIC client verbatim —
    // never widened at the gate — and with them whether the minter asked on a
    // person's behalf.
    m_pendingQUICTokens.push_back({ token, now + quicTokenTTLMilliseconds, mintingClientID, scope, Ubiquitty::Crypto::sha256Hex(token), isInteractiveUser });
    return token;
}

void Daemon::pruneExpiredQUICTokens(std::uint64_t now)
{
    for (std::size_t i = m_pendingQUICTokens.size(); i-- > 0;)
        if (now >= m_pendingQUICTokens[i].deadlineMilliseconds)
            m_pendingQUICTokens.erase(m_pendingQUICTokens.begin() + static_cast<long>(i));
}

std::uint64_t Daemon::earliestQUICTokenDeadline() const
{
    std::uint64_t earliest = 0;
    for (const auto& pending : m_pendingQUICTokens)
        if (earliest == 0 || pending.deadlineMilliseconds < earliest)
            earliest = pending.deadlineMilliseconds;
    return earliest;
}

bool Daemon::consumeQUICToken(const std::string& token, Protocol::Sync::ScopeSet& outScope, std::string& outTokenID, bool* outInteractiveUser)
{
    const std::uint64_t now = nowMilliseconds();
    pruneExpiredQUICTokens(now); // reject any that aged out, before matching
    for (std::size_t i = 0; i < m_pendingQUICTokens.size(); ++i) {
        const auto& pending = m_pendingQUICTokens[i];
        // Constant-time compare: a one-shot bootstrap secret must not leak a
        // match position through early-out timing (digestsEqual XOR-accumulates).
        if (digestsEqual(pending.token, token)) {
            // Hand the carried identity back before erase (pending dangles after).
            outScope = pending.scope;
            outTokenID = pending.tokenID;
            if (outInteractiveUser)
                *outInteractiveUser = pending.isInteractiveUser;
            m_pendingQUICTokens.erase(m_pendingQUICTokens.begin() + static_cast<long>(i));
            return true; // one-time: a token authorizes exactly one connection
        }
    }
    return false;
}

// Consent gate.

bool Daemon::isAgentClient(const Client& c) const
{
    // Who a client is, for the consent, presence and terminate gates. What its
    // input may cause is originOf's question, and originOf is untouched: a QUIC
    // peer stays Remote there, so the output side-effect floor, driverOriginFloor
    // and the HostCapabilityPolicy keep gating a dialled-in person at the
    // strictest principal.
    //
    // Two clients are a person. The trusted local GUI — the ubiquitty-host attach
    // bridge, origin Local. And the bearer of an unlabelled interactive-user
    // bootstrap token: the token a uid-gated local client minted on the
    // ssh-authenticated user's behalf, presented over QUIC by that user's own
    // phone. That mint is unreachable from QUIC and the token is one-time, so the
    // mark is not something a peer can assert about itself.
    //
    // Everything else is an agent, and each exclusion carries its own weight. A
    // tokenless remote peer is an agent: deriving identity from origin, rather
    // than from "no token and no capability bit ⇒ human", is what closed the
    // classifier contradiction where such a peer silently joined a human session,
    // drove input, spoofed presence and ruled on other agents' consent, and a
    // mark it cannot carry leaves that closed. A token bearer naming an agentLabel is an agent by its own
    // account. A client holding a token from the tokens file is an agent: that
    // store mints for automation and stamps no mark.
    if (c.hasToken && c.isInteractiveUser && c.agentLabel.empty())
        return false;
    return originOf(c) != Ubiquitty::CapabilityOrigin::Local;
}

bool Daemon::sessionHasHumanClient(uint32_t sessionID) const
{
    for (const auto& c : m_clients)
        if (mayRuleOnConsentFor(c, sessionID))
            return true;
    return false;
}

bool Daemon::sessionHasAskableHuman(std::uint32_t sessionID) const
{
    return std::ranges::any_of(m_clients, [&](const Client& c) { return mayBeAskedFor(c, sessionID); });
}

std::string Daemon::grantKeyFor(const Client& c) const
{
    // A token digest is a precise, non-spoofable principal — prefer it so two
    // agents sharing a binary are distinguishable and a swapped binary can't
    // inherit a grant. Falls back to the verified exe path when tokenless.
    if (c.hasToken && !c.tokenID.empty())
        return "token:" + c.tokenID;
    return c.peerProcess;
}

bool Daemon::agentHasStandingGrant(const Client& agent, const Session& s) const
{
    // No stable key (remote/QUIC tokenless, or a path we rejected) → a control
    // agent can never silently ride a prior grant; it always re-prompts.
    const std::string key = grantKeyFor(agent);
    if (key.empty())
        return false;
    // allow-session is bound to the agents a person approved (and the agent
    // that created the session), one grant each.
    if (s.hasSessionGrantFor(key))
        return true;
    // allow-always-this-agent: the persisted key.
    return std::find(m_alwaysAllowedAgents.begin(), m_alwaysAllowedAgents.end(), key) != m_alwaysAllowedAgents.end();
}

bool Daemon::persistAlwaysAllowedAgent(std::size_t agentIndex)
{
    if (agentIndex >= m_clients.size())
        return false;
    const Client& agent = m_clients[agentIndex];
    // Persist only a token-bound principal. A tokenless agent's grant key is its
    // verified-but-MUTABLE exe path, so an attacker who later replaces that
    // binary would inherit a permanent grant — the very inheritance the token
    // digest exists to prevent. A tokenless "always" is instead honored
    // for the session (by the caller); nothing path-keyed ever reaches disk.
    if (!(agent.hasToken && !agent.tokenID.empty()))
        return false;
    const std::string key = grantKeyFor(agent);
    if (key.empty())
        return false;
    auto result = editStandingConsentList(consentStorePath(), key, StandingConsentOperation::Add);
    if (result.outcome != ConsentEdit::Changed && result.outcome != ConsentEdit::Unchanged)
        return false;
    queueStandingConsentWithdrawals(result.withdrawnGrants);
    adoptStandingConsentEntries(std::move(result.entries));
    return true;
}

bool Daemon::revokeAlwaysAllowedAgent(const std::string& identity)
{
    auto result = editStandingConsentList(consentStorePath(), identity, StandingConsentOperation::Remove);
    if (result.outcome != ConsentEdit::Changed && result.outcome != ConsentEdit::Unchanged)
        return false;
    queueStandingConsentWithdrawals(result.withdrawnGrants);
    adoptStandingConsentEntries(std::move(result.entries));
    return true;
}

bool Daemon::tokenScopeAllows(const Client& c, Protocol::Sync::Scope scopeBit) const
{
    // The tokenless free-pass (require_token off) belongs to the trusted LOCAL client
    // only — origin Local, the uid-gated attach bridge. A non-Local client (Remote
    // over QUIC, or a non-bridge local Agent) is transport-unverified and must be held
    // to a real token's scope ceiling. Without this origin guard a bare-HELLO remote
    // peer (hasToken==false) passed every scope gate — Drive/Control/RegionRead — the
    // same transport-blind fail-open class. Origin is the authority:
    // Local is fully allowed; everyone else needs a token that carries the scope.
    //
    // With the requirement off (UBIQUITTY_REQUIRE_TOKEN=0), a tokenless agent the
    // daemon verified as a same-user local process is held to the ceiling an
    // `all` token carries, as though it held one. FileRead and Manage are never
    // part of it, and a Remote peer is never verified local, so the switch opens
    // nothing to the network.
    const auto origin = originOf(c);
    if (origin == Ubiquitty::CapabilityOrigin::Local)
        return true;
    if (c.hasToken)
        return c.tokenScope.contains(scopeBit);
    return !m_requireToken && origin == Ubiquitty::CapabilityOrigin::Agent && tokenlessLocalAgentScope.contains(scopeBit);
}

void Daemon::refuseClient(std::size_t index, Protocol::Sync::ByeReason reason)
{
    if (index >= m_clients.size())
        return;
    auto& client = m_clients[index];
    std::vector<std::uint8_t> frame;
    if (client.syncConnection)
        client.syncConnection->refuse(reason);
    else {
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::spokenBye(m_wireVersion, Protocol::Sync::appVersion(), hostPlatformName(), reason, {}));
        (void)Protocol::Sync::appendFrame(frame, Protocol::Sync::FrameKind::Bye, 0, body);
    }
    withdrawClientAttachment(index);
    cancelPendingClipboardWrites(client.clientID);
    retireClientConsent(client);
    client.attachConsentApproval.reset();
    client.attachConsentTerminal.reset();
    client.consentPending = false;
    // Its authority is withdrawn now: what it allowed and no agent has taken
    // is void, and every question it was asked is left to the others.
    voidApprovalsGivenBy(client.clientID);
    leaveEveryQuestion(client.clientID, QuestionLeave::Departed);
    client.didOverflow = false;
    // Keep any partly transmitted body intact. The ordered refusal drains
    // through normal service, without blocking the reactor on a slow reader.
    if (client.syncConnection)
        drainScreenInteractive(index);
    else
        queueToClient(index, frame);
    armSessionExitDeadline(client, nowMilliseconds());
    flushClient(index);
}

void Daemon::recordManagementCreation(const std::string& identifier, const Client& actor)
{
    recordManagementCreation(identifier, managementIdentityOf(actor));
}

void Daemon::recordManagementCreation(const std::string& identifier, const std::string& identity)
{
    TokenStoreLock lock(m_tokenPath);
    if (!lock.isLocked()) {
        m_didFailManagementHistory = true;
        return;
    }
    ManagementMetadataStore metadata(m_tokenPath);
    if (!metadata.created(identifier, static_cast<std::uint64_t>(::time(nullptr)), identity)) {
        m_didFailManagementHistory = true;
    }
}

std::string Daemon::managementIdentityOf(const Client& actor) const
{
    return originOf(actor) == CapabilityOrigin::Local ? "Local owner (uid " + std::to_string(::geteuid()) + ")" : grantKeyFor(actor);
}

void Daemon::voidApprovalsGivenBy(std::uint32_t approverClientID)
{
    for (Client& requester : m_clients) {
        if (requester.attachConsentApproval && requester.attachConsentApproval->approverClientID == approverClientID && !requester.attachConsentTerminal)
            requester.attachConsentApproval.reset();
    }
}

void Daemon::voidApprovalsGivenUnderToken(std::string_view tokenID)
{
    const std::string principal = "token:" + std::string(tokenID);
    for (Client& requester : m_clients) {
        if (requester.attachConsentApproval && requester.attachConsentApproval->approverPrincipal == principal && !requester.attachConsentTerminal)
            requester.attachConsentApproval.reset();
    }
}

void Daemon::recordManagementTokenUse(const Client& actor)
{
    if (!actor.hasToken || actor.hasBootstrapToken || actor.tokenID.empty()) {
        return;
    }
    TokenStoreLock lock(m_tokenPath);
    if (!lock.isLocked()) {
        m_didFailManagementHistory = true;
        return;
    }
    ManagementMetadataStore metadata(m_tokenPath);
    const auto label = actor.peerProcess.empty() ? std::string("Remote client") : actor.peerProcess.substr(0, 128);
    if (!metadata.used("token:" + actor.tokenID, static_cast<std::uint64_t>(::time(nullptr)), label)) {
        m_didFailManagementHistory = true;
    }
}

void Daemon::reverifyTokenClients(CredentialStoreReading reading)
{
    // The daemon never stores the plaintext token, only its digest (tokenID),
    // so match each live token-bound client against the digests still present
    // and unexpired in the reloaded store. A vanished or expired digest →
    // Bye(TokenRevoked) + deferred reap. A store that could not be read whole
    // validates nothing.
    const std::uint64_t now = static_cast<std::uint64_t>(::time(nullptr));
    TokenStoreContents store = reading == CredentialStoreReading::AsFound && m_isConsentStoreWhole ? readTokenStore(m_tokenPath) : TokenStoreContents {};
    if (!store.isWhole) {
        Ubiquitty::PAL::logLine(stderr, "daemon: the credential stores could not be read whole; every token-bound grant is withdrawn");
        store.records.clear();
    }
    const auto& records = store.records;

    m_areCredentialStoresWhole = store.isWhole;
    if (!m_areCredentialStoresWhole) {
        queueStandingConsentWithdrawals(m_alwaysAllowedAgents);
        m_alwaysAllowedAgents.clear();
        m_credentialStoreStamp.reset();
    }
    // Refusal withdraws authority immediately; the run loop alone reaps clients.
    for (std::size_t i = m_clients.size(); i-- > 0;) {
        Client& c = m_clients[i];
        if (!c.hasToken || c.tokenID.empty())
            continue;
        // A carried bootstrap digest is not a record here — it was minted in
        // memory for one connection and never written — so its absence from the
        // store is its normal state, not a revocation. Matching it against the
        // store would take every live QUIC client down with any one revoke.
        if (c.hasBootstrapToken)
            continue;
        const TokenRecord* verified = nullptr;
        for (const auto& record : records)
            if (tokenExpiry(record, now) != TokenExpiry::Expired && digestsEqual(record.hashHex, c.tokenID)
                && (record.scope.toRaw() & c.tokenScope.toRaw()) == c.tokenScope.toRaw()) {
                verified = &record;
                break;
            }
        if (!verified) {
            voidApprovalsGivenUnderToken(c.tokenID);
            refuseClient(i, Protocol::Sync::ByeReason::TokenRevoked);
        } else if (verified->expiryUnix && (!c.tokenExpiryUnix || verified->expiryUnix < c.tokenExpiryUnix))
            c.tokenExpiryUnix = verified->expiryUnix;
    }
    // No live Client need remain for a Lost attachment. The retained digest,
    // not a label or a replacement token's scope, identifies its authority.
    for (std::size_t index = m_attachmentGrants.size(); index-- > 0;) {
        AttachmentGrant& grant = m_attachmentGrants[index];
        if (grant.hasBootstrapToken)
            continue;
        const auto record = std::ranges::find_if(records, [&](const TokenRecord& candidate) {
            return digestsEqual(candidate.hashHex, grant.tokenID);
        });
        if (record == records.end() || tokenExpiry(*record, now) == TokenExpiry::Expired
            || (record->scope.toRaw() & grant.scope.toRaw()) != grant.scope.toRaw()) {
            voidApprovalsGivenUnderToken(grant.tokenID);
            retireAttachmentGrant(grant.attachmentID);
            continue;
        }
        // A store rewrite may shorten, but never renew, retained authority.
        if (record->expiryUnix && (!grant.tokenExpiryUnix || record->expiryUnix < grant.tokenExpiryUnix))
            grant.tokenExpiryUnix = record->expiryUnix;
    }
}

void Daemon::reloadCredentialStores(CredentialStoreReading reading)
{
    if (reading == CredentialStoreReading::AsFound) {
        if (!loadAlwaysAllowedAgents())
            reading = CredentialStoreReading::Withheld;
    } else {
        queueStandingConsentWithdrawals(m_alwaysAllowedAgents);
        m_alwaysAllowedAgents.clear();
        m_isConsentStoreWhole = false;
    }
    if (!m_isConsentStoreWhole)
        m_credentialStoreStamp.reset();
    reverifyTokenClients(reading);
    retireStandingConsentWithdrawals();
    for (Client& client : m_clients)
        if (client.syncConnection)
            client.syncConnection->noteAttachmentsChanged();
}

bool Daemon::holdsStoreBoundAuthority() const
{
    // A bootstrap credential is independent of the token store. An agent's
    // standing attach consent can still depend on the consent store.
    const auto hasStandingConsent = [&](std::string_view tokenID) {
        return std::ranges::any_of(m_alwaysAllowedAgents, [&](const auto& identity) {
            return identity.starts_with("token:") && std::string_view(identity).substr(6) == tokenID;
        });
    };
    const bool hasStoreBoundClient = std::ranges::any_of(m_clients, [&](const Client& client) {
        return !client.didOverflow && ((client.hasToken && (!client.hasBootstrapToken || (isAgentClient(client) && hasStandingConsent(client.tokenID)))) || client.consentPending);
    });
    return !m_areCredentialStoresWhole || hasStoreBoundClient || std::ranges::any_of(m_attachmentGrants, [&](const AttachmentGrant& grant) {
        if (!grant.hasBootstrapToken)
            return true;
        const auto* record = m_attachments->find(grant.attachmentID);
        return record && record->kind == Protocol::Sync::ClientKind::Agent && hasStandingConsent(grant.tokenID);
    });
}

Daemon::CredentialFileStamp Daemon::credentialFileStamp(const std::string& path)
{
    struct stat information {};
    if (::stat(path.c_str(), &information) != 0)
        return { .error = errno ? errno : EIO };
    return grantFileStamp(information);
}

void Daemon::refreshCredentialStoresIfChanged()
{
    // A daemon that never opened a store has none to follow.
    if (m_tokenPath.empty())
        return;
    const std::string consentPath = consentStorePath();
    const std::array<CredentialFileStamp, 2> stamp { credentialFileStamp(m_tokenPath), credentialFileStamp(consentPath) };
    if (m_credentialStoreStamp && *m_credentialStoreStamp == stamp)
        return;
    // An absent store is a known state: it holds nothing. Any other failure
    // to examine one leaves the daemon unable to say what was revoked, so
    // every grant is withheld, and the stamp is not kept, so each later
    // check withholds again until the store can be examined.
    const bool isExaminable = std::ranges::all_of(stamp, [](const CredentialFileStamp& file) { return file.error == 0 || file.error == ENOENT; });
    if (!isExaminable) {
        m_credentialStoreStamp.reset();
        Ubiquitty::PAL::logLine(stderr, "daemon: a credential store could not be examined; every store-bound grant is withdrawn");
        reloadCredentialStores(CredentialStoreReading::Withheld);
        return;
    }
    reloadCredentialStores();
    if (m_areCredentialStoresWhole)
        m_credentialStoreStamp = stamp;
}

bool Daemon::refreshCredentialStoresBeforeDispatch(std::size_t& index)
{
    const Client& client = m_clients[index];
    // A trusted local client's own frames carry no store-bound authority; it
    // is checked only while someone else's is held, since what it sends can
    // act on that authority (an answer to a pending request, say).
    const bool mayCarryStoreAuthority = originOf(client) != Ubiquitty::CapabilityOrigin::Local && !client.hasBootstrapToken;
    if (!mayCarryStoreAuthority && !holdsStoreBoundAuthority())
        return true;
    const std::uint32_t clientID = client.clientID;
    refreshCredentialStoresIfChanged();
    const auto found = std::ranges::find(m_clients, clientID, &Client::clientID);
    if (found == m_clients.end() || found->didOverflow
        || (found->syncConnection && found->syncConnection->state() == Protocol::Sync::ConnectionState::Closed))
        return false;
    index = static_cast<std::size_t>(found - m_clients.begin());
    if (!m_areCredentialStoresWhole && mayCarryStoreAuthority) {
        // A fresh greeting has not supplied its token yet, so reverifying
        // already-authenticated clients cannot withhold this connection.
        refuseClient(index, found->hasToken ? Protocol::Sync::ByeReason::TokenRevoked : Protocol::Sync::ByeReason::TokenInvalid);
        return false;
    }
    return true;
}

void Daemon::dropExpiredTokenClients()
{
    // Expiry binds the connection, not just the handshake that presented the
    // token: the scope a lapsed token carries is no longer its bearer's, and
    // time passing changes nothing on disk, so the store's own refresh never
    // notices it.
    // Bootstrap authority has no token-store deadline; its attachment's
    // ordinary Lost grace still bounds transport resume.
    //
    // Reverse iteration for the reason reverifyTokenClients states: refuseClient
    // can drop the slot it is given, shifting the later ones down.
    const std::uint64_t nowUnix = static_cast<std::uint64_t>(::time(nullptr));
    for (std::size_t i = m_clients.size(); i-- > 0;) {
        const Client& c = m_clients[i];
        if (c.hasToken && c.tokenExpiryUnix != 0 && nowUnix >= c.tokenExpiryUnix && !c.didOverflow) {
            if (!c.hasBootstrapToken)
                voidApprovalsGivenUnderToken(c.tokenID);
            refuseClient(i, Protocol::Sync::ByeReason::TokenExpired);
        }
    }
    for (std::size_t index = m_attachmentGrants.size(); index-- > 0;) {
        const AttachmentGrant& grant = m_attachmentGrants[index];
        if (grant.tokenExpiryUnix && nowUnix >= grant.tokenExpiryUnix) {
            if (!grant.hasBootstrapToken)
                voidApprovalsGivenUnderToken(grant.tokenID);
            retireAttachmentGrant(grant.attachmentID);
        }
    }
}

std::uint64_t Daemon::earliestTokenExpiryUnix() const
{
    std::uint64_t earliest = 0;
    for (const auto& c : m_clients)
        if (c.hasToken && c.tokenExpiryUnix != 0 && (earliest == 0 || c.tokenExpiryUnix < earliest))
            earliest = c.tokenExpiryUnix;
    for (const AttachmentGrant& grant : m_attachmentGrants)
        if (grant.tokenExpiryUnix && (!earliest || grant.tokenExpiryUnix < earliest))
            earliest = grant.tokenExpiryUnix;
    return earliest;
}

void Daemon::capturePeerIdentity(Client& c)
{
    if (!c.fileDescriptor)
        return; // QUIC/remote: no local processID → cannot be "always" allowed
    // Which use of the process id this is, read before the identity is taken
    // from it and again after. The id is a number the kernel reuses, so the
    // path and the inode below describe whoever holds it at the moment of each
    // call — a process that exits mid-capture hands the rest of the work to
    // its successor.
    std::uint64_t instanceBefore = 0;
    bool sawInstanceBefore = false;
#ifndef __APPLE__
    // Code that is not signed is judged by its file alone (`isOwnBinary`).
    c.peerSignature = { .signer = Signer::Unsupported, .identifier = {} };
#endif
#ifdef __APPLE__
    pid_t processID = 0;
    socklen_t length = sizeof(processID);
    if (::getsockopt(c.fileDescriptor.get(), SOL_LOCAL, LOCAL_PEERPID, &processID, &length) == 0 && processID > 0) {
        c.peerProcessID = static_cast<uint32_t>(processID);
        sawInstanceBefore = peerProcessInstanceOf(c.peerProcessID, instanceBefore);
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(processID, path, sizeof(path)) > 0) {
            c.peerProcess = path; // the always-allow grant key (verified)
            c.hasPeerBinary = binaryIdentityOf(c.peerProcess, c.peerBinary);
        }
        // Read once for each binary peers run: a file that has not changed
        // since its verdict keeps it, so a connection costs a stat.
        if (c.hasPeerBinary) {
            const auto key = signatureKeyOf(c.peerProcess);
            if (const CodeSignature* known = key ? m_peerSignatures.find(*key) : nullptr)
                c.peerSignature = *known;
            else {
                c.peerSignature = signatureOfPeer(c.fileDescriptor.get());
                if (key)
                    m_peerSignatures.remember(*key, c.peerSignature);
            }
        }
    }
#elif defined(__linux__)
    // SO_PEERCRED gives the connecting processID; /proc/<processID>/exe resolves to its
    // verified binary path — the same stable grant key the macOS path uses,
    // so a tokenless agent's allow-session/allow-always survives a reconnect
    // (without it the key is empty and every re-attach re-prompts).
    struct ucred peerCredentials {};
    socklen_t peerCredentialsLength = sizeof(peerCredentials);
    if (::getsockopt(c.fileDescriptor.get(), SOL_SOCKET, SO_PEERCRED, &peerCredentials, &peerCredentialsLength) == 0 && peerCredentials.pid > 0) {
        // `struct ucred` is a kernel struct — its field is `pid`, not a
        // renamed `processID`.
        c.peerProcessID = static_cast<uint32_t>(peerCredentials.pid);
        sawInstanceBefore = peerProcessInstanceOf(c.peerProcessID, instanceBefore);
        char link[64];
        std::snprintf(link, sizeof(link), "/proc/%d/exe", peerCredentials.pid);
        char path[PATH_MAX];
        const ssize_t n = ::readlink(link, path, sizeof(path) - 1);
        if (n > 0) {
            path[n] = '\0';
            c.peerProcess = path; // the always-allow grant key (verified)
        }
        // The link, not what it reads as: the kernel resolves /proc/<pid>/exe to
        // the file the process is running, so this cannot be pointed elsewhere.
        // The path above is a label for the grant key and is not identity.
        c.hasPeerBinary = binaryIdentityOf(link, c.peerBinary);
    }
#elif defined(__FreeBSD__)
    // FreeBSD: LOCAL_PEERCRED (SOL_LOCAL) fills a struct xucred whose cr_pid is
    // the connecting pid (added in 13.0; cr_version guards a kernel that filled
    // it — older ones leave the id 0, uid still verified). The verified binary
    // path comes from sysctl KERN_PROC_PATHNAME (procfs is not mounted by
    // default, so /proc is unavailable). LOCAL_PEERPID does not exist here.
    struct xucred peerCredentials {};
    socklen_t peerCredentialsLength = sizeof(peerCredentials);
    if (::getsockopt(c.fileDescriptor.get(), SOL_LOCAL, LOCAL_PEERCRED, &peerCredentials, &peerCredentialsLength) == 0 && peerCredentials.cr_version == XUCRED_VERSION && peerCredentials.cr_pid > 0) {
        c.peerProcessID = static_cast<uint32_t>(peerCredentials.cr_pid);
        int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, peerCredentials.cr_pid };
        char path[PATH_MAX];
        std::size_t pathLength = sizeof(path);
        if (::sysctl(mib, 4, path, &pathLength, nullptr, 0) == 0 && pathLength > 0)
            c.peerProcess = path; // the always-allow grant key (verified)
    }
#elif defined(__OpenBSD__)
    // OpenBSD: SO_PEERCRED fills a struct sockpeercred (pid/uid/gid). There is
    // no /proc to resolve the binary path, so the always-allow grant key stays
    // empty (allow-once / allow-session still work; allow-always just won't
    // persist a path key). The uid is already verified at accept time.
    struct sockpeercred peerCredentials {};
    socklen_t peerCredentialsLength = sizeof(peerCredentials);
    if (::getsockopt(c.fileDescriptor.get(), SOL_SOCKET, SO_PEERCRED, &peerCredentials, &peerCredentialsLength) == 0 && peerCredentials.pid > 0) {
        c.peerProcessID = static_cast<uint32_t>(peerCredentials.pid);
    }
#elif defined(__NetBSD__)
    // NetBSD: LOCAL_PEEREID (level 0) fills struct unpcbid (pid/euid/egid). Like
    // OpenBSD, no /proc, so the always-allow path key stays empty; uid verified.
    struct unpcbid peerCredentials {};
    socklen_t peerCredentialsLength = sizeof(peerCredentials);
    if (::getsockopt(c.fileDescriptor.get(), 0, LOCAL_PEEREID, &peerCredentials, &peerCredentialsLength) == 0 && peerCredentials.unp_pid > 0) {
        c.peerProcessID = static_cast<uint32_t>(peerCredentials.unp_pid);
    }
#endif
    // The same use of the id, or none of it counts. A mismatch means the work
    // above ran across a reuse, so the path and the inode are about a process
    // that is not the one that connected — and this client cannot be Local.
    //
    // Only a mismatch refuses. A platform that supplies no instance leaves the
    // capture as it was rather than failing every local client on it, because
    // refusing what was never checked is a different decision from refusing
    // what was checked and disagreed.
    if (sawInstanceBefore) {
        std::uint64_t instanceAfter = 0;
        if (peerProcessInstanceOf(c.peerProcessID, instanceAfter)) {
            if (instanceAfter != instanceBefore) {
                c.hasPeerProcessInstanceMismatch = true;
                c.peerProcess.clear();
                c.hasPeerBinary = false;
                c.peerSignature = {};
            } else
                c.peerProcessInstance = instanceBefore;
        }
    }
    // The path is the persistent always-allow key, stored one-per-line, so a
    // control char in it (a newline is a legal filename byte) could inject a
    // bogus "allowed" entry — drop such a path so it can only be once/session.
    if (c.peerProcess.find_first_of("\n\r") != std::string::npos)
        c.peerProcess.clear();
}

std::size_t Daemon::clipboardClientForSession(std::uint32_t sessionID) const
{
    const Session* session = nullptr;
    for (const std::unique_ptr<Session>& candidate : m_sessions) {
        if (candidate && candidate->id == sessionID) {
            session = candidate.get();
            break;
        }
    }
    if (session == nullptr)
        return m_clients.size();

    const auto nowUnix = static_cast<std::uint64_t>(::time(nullptr));
    auto canServe = [&](const Client& client) {
        // Only a client that can drive the session serves its clipboard to a
        // helper the session's shell runs: the command stands in for a
        // driver's paste, and an observe-only viewer's pasteboard is a
        // bystander's, never the one the command means.
        return client.subscriptionFor(sessionID) && client.helloSeen && !client.didOverflow && !client.sessionExitDeadlineMilliseconds
            && !(client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= nowUnix)
            && tokenScopeAllows(client, Protocol::Sync::Scope::Drive)
            && client.intents.contains(Protocol::Sync::HelloIntent::ServesClipboard);
    };

    // Someone has typed: their clipboard is the one the command means, and no
    // other client's will do. A driver that cannot serve answers nothing rather
    // than deferring to a neighbour who never ran the command.
    if (session->lastInputClientID != 0) {
        for (std::size_t i = 0; i < m_clients.size(); ++i) {
            if (m_clients[i].clientID == session->lastInputClientID)
                return canServe(m_clients[i]) ? i : m_clients.size();
        }
        // The driver detached. Its clipboard left with it, and the remaining
        // clients did not run the command.
        return m_clients.size();
    }

    // Nobody has typed yet, so there is no "whose" to honour. One capable
    // client is unambiguous; two are a coin toss between two people's
    // pasteboards, which is the answer this whole function exists to refuse.
    std::size_t only = m_clients.size();
    for (std::size_t i = 0; i < m_clients.size(); ++i) {
        if (!canServe(m_clients[i]))
            continue;
        if (only != m_clients.size())
            return m_clients.size();
        only = i;
    }
    return only;
}

std::uint32_t Daemon::requestClipboardFromClient(
    std::uint32_t sessionID, Protocol::Sync::ClipboardRequestKind kind, const std::string& mime)
{
    const std::size_t index = clipboardClientForSession(sessionID);
    if (index >= m_clients.size())
        return 0;
    Client& client = m_clients[index];
    if (client.pendingClipboardRequests.size() >= maxPendingClipboardRequestsPerClient || m_nextClipboardRequestID == UINT32_MAX)
        return 0;

    Protocol::Sync::ClipboardRequestMessage request;
    request.requestID = ++m_nextClipboardRequestID;
    request.kind = kind;
    request.mime = mime;
    const auto* subscription = client.subscriptionFor(sessionID);
    if (!client.syncConnection || !subscription || !client.syncConnection->requestClipboard(subscription->screenHandle, { .requestID = request.requestID, .buffer = ClipboardBuffer::Clipboard, .kind = kind == Protocol::Sync::ClipboardRequestKind::Types ? Protocol::Sync::EffectClipboardRequestKind::Types : Protocol::Sync::EffectClipboardRequestKind::Item, .mime = mime }))
        return 0;
    client.pendingClipboardRequests.push_back(request.requestID);
    flushClient(index);
    return request.requestID;
}

bool Daemon::mayReceiveClipboardData(std::size_t index, std::uint32_t handle, std::uint32_t requestID) const
{
    if (index >= m_clients.size())
        return false;
    const Client& source = m_clients[index];
    const auto nowUnix = static_cast<std::uint64_t>(::time(nullptr));
    if (!source.helloSeen || source.didOverflow || source.sessionExitDeadlineMilliseconds
        || (source.hasToken && source.tokenExpiryUnix && source.tokenExpiryUnix <= nowUnix)
        || std::ranges::find(source.pendingClipboardRequests, requestID) == source.pendingClipboardRequests.end())
        return false;
    const auto subscription = std::ranges::find_if(source.paneSubscriptions, [&](const PaneSubscription& pane) { return pane.screenHandle == handle; });
    if (subscription == source.paneSubscriptions.end() || clipboardClientForSession(subscription->sessionID) != index)
        return false;
    const auto now = nowMilliseconds();
    return std::ranges::any_of(m_clients, [&](const Client& helper) {
        return !helper.didOverflow && helper.clipboardToolSourceClientID == source.clientID
            && helper.clipboardToolRequestID == requestID && helper.clipboardToolSessionID == subscription->sessionID
            && helper.clipboardToolDeadlineMilliseconds > now
            && !(helper.hasToken && helper.tokenExpiryUnix && helper.tokenExpiryUnix <= nowUnix);
    });
}

bool Daemon::isClipboardToolSelectionCurrent(const Client& helper) const
{
    if (helper.hasPeerProcessInstanceMismatch)
        return false;
    if (helper.peerProcessInstance) {
        std::uint64_t current = 0;
        if (!peerProcessInstanceOf(helper.peerProcessID, current) || current != *helper.peerProcessInstance)
            return false;
    }
    if (!helper.clipboardToolSelectionClientID)
        return !helper.isClipboardToolHostSelected;
    const auto source = clipboardClientForSession(helper.clipboardToolSessionID);
    if (!helper.isClipboardToolHostSelected)
        return source < m_clients.size() && m_clients[source].clientID == helper.clipboardToolSelectionClientID;
    if (source < m_clients.size())
        return false;
    const auto found = std::ranges::find_if(m_sessions, [&](const auto& session) { return session->id == helper.clipboardToolSessionID; });
    const auto* session = found == m_sessions.end() ? nullptr : found->get();
    if (!session)
        return false;
    std::size_t attached = 0;
    bool isSelectedPresent = false;
    for (const auto& client : m_clients) {
        if (!client.subscriptionFor(session->id) || !client.helloSeen || client.didOverflow || client.sessionExitDeadlineMilliseconds)
            continue;
        if (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr)))
            continue;
        ++attached;
        if (client.clientID == helper.clipboardToolSelectionClientID)
            isSelectedPresent = !client.intents.contains(Protocol::Sync::HelloIntent::ServesClipboard);
    }
    return isSelectedPresent && (session->lastInputClientID ? session->lastInputClientID == helper.clipboardToolSelectionClientID : attached == 1);
}

void Daemon::handleClipboardToolRequest(std::size_t index, const Protocol::Sync::ClipboardRequestMessage& request, ClipboardToolOperation operation)
{
    Client& helper = m_clients[index];
    Session* owner = findSession(helper.clipboardToolSessionID);
    assert(owner);
    helper.helloSeen = true;
    helper.didSeeClipboardToolRequest = true;
    helper.clipboardToolSessionID = owner->id;
    helper.clipboardToolOriginalRequestID = request.requestID;
    const std::size_t source = clipboardClientForSession(owner->id);
    if (source >= m_clients.size()) {
        bool isHostSelected = false;
        for (const auto& client : m_clients) {
            if (client.subscriptionFor(owner->id) && client.clientID == owner->lastInputClientID && client.helloSeen && !client.didOverflow && !client.sessionExitDeadlineMilliseconds && !client.intents.contains(Protocol::Sync::HelloIntent::ServesClipboard)) {
                isHostSelected = true;
                helper.clipboardToolSelectionClientID = client.clientID;
            }
        }
        if (!owner->lastInputClientID) {
            std::size_t attached = 0;
            for (const auto& client : m_clients) {
                if (client.subscriptionFor(owner->id) && client.helloSeen && !client.didOverflow && !client.sessionExitDeadlineMilliseconds) {
                    ++attached;
                    isHostSelected = !client.intents.contains(Protocol::Sync::HelloIntent::ServesClipboard);
                    helper.clipboardToolSelectionClientID = client.clientID;
                }
            }
            isHostSelected = isHostSelected && attached == 1;
        }
        if (isHostSelected) {
            helper.isClipboardToolHostSelected = true;
            if (!queueLocalClipboardReply(index, { .correlation = helper.localClipboardCorrelation, .daemonInstanceID = m_daemonInstanceID, .status = Protocol::Sync::EffectClipboardStatus::Empty, .isHostSelected = true })) {
                dropClient(index);
                return;
            }
            helper.sessionExitDeadlineMilliseconds = nowMilliseconds() + 1000;
            flushClient(index);
        } else {
            helper.clipboardToolSelectionClientID = 0;
            Protocol::Sync::ClipboardDataMessage refused;
            refused.status = Protocol::Sync::ClipboardServeStatus::Refused;
            finishClipboardTool(index, refused);
        }
        return;
    }
    helper.clipboardToolSelectionClientID = m_clients[source].clientID;
    if (operation == ClipboardToolOperation::InspectSource) {
        Protocol::Sync::ClipboardDataMessage empty;
        empty.status = Protocol::Sync::ClipboardServeStatus::Empty;
        finishClipboardTool(index, empty);
        return;
    }
    helper.clipboardToolSourceClientID = m_clients[source].clientID;
    helper.clipboardToolRequestID = requestClipboardFromClient(owner->id, request.kind, request.mime);
    if (!helper.clipboardToolRequestID) {
        Protocol::Sync::ClipboardDataMessage refused;
        refused.status = Protocol::Sync::ClipboardServeStatus::Refused;
        finishClipboardTool(index, refused);
        return;
    }
    helper.clipboardToolDeadlineMilliseconds = nowMilliseconds() + 10000;
}

void Daemon::finishClipboardTool(std::size_t index, const Protocol::Sync::ClipboardDataMessage& response)
{
    std::array<Protocol::Sync::ClipboardServeItemView, Protocol::Sync::maxClipboardServeItems> items {};
    if (response.items.size() > items.size()) {
        finishClipboardTool(index, Protocol::Sync::ClipboardServeStatus::TooLarge, {});
        return;
    }
    for (std::size_t item = 0; item < response.items.size(); ++item)
        items[item] = { response.items[item].mime, response.items[item].data };
    finishClipboardTool(index, response.status, std::span(items).first(response.items.size()));
}

void Daemon::finishClipboardTool(std::size_t index, Protocol::Sync::ClipboardServeStatus status,
    std::span<const Protocol::Sync::ClipboardServeItemView> items)
{
    Client& helper = m_clients[index];
    Protocol::Local::LocalClipboardReply reply { .correlation = helper.localClipboardCorrelation,
        .daemonInstanceID = m_daemonInstanceID,
        .status = static_cast<Protocol::Sync::EffectClipboardStatus>(status) };
    if (items.size() <= reply.bundle.items.size()) {
        reply.bundle.count = items.size();
        for (std::size_t item = 0; item < items.size(); ++item)
            reply.bundle.items[item] = { items[item].mime, items[item].data };
    } else
        reply.status = Protocol::Sync::EffectClipboardStatus::TooLarge;
    if (reply.status == Protocol::Sync::EffectClipboardStatus::OK) {
        const bool isWrongShape = helper.localClipboardKind == Protocol::Local::LocalClipboardKind::Route
            || (helper.localClipboardKind == Protocol::Local::LocalClipboardKind::Types
                && std::ranges::any_of(std::span(reply.bundle.items).first(reply.bundle.count), [](const auto& item) { return !item.data.empty(); }))
            || (helper.localClipboardKind == Protocol::Local::LocalClipboardKind::Item
                && (reply.bundle.count != 1 || reply.bundle.items.front().mime != helper.localClipboardMIME));
        if (isWrongShape) {
            reply.status = Protocol::Sync::EffectClipboardStatus::Refused;
            reply.bundle.count = 0;
        }
    }
    bool didQueue = queueLocalClipboardReply(index, reply);
    if (!didQueue) {
        reply.status = Protocol::Sync::EffectClipboardStatus::Refused;
        reply.bundle.count = 0;
        didQueue = queueLocalClipboardReply(index, reply);
    }
    for (auto& client : m_clients) {
        if (client.clientID == helper.clipboardToolSourceClientID) {
            std::erase(client.pendingClipboardRequests, helper.clipboardToolRequestID);
            if (client.syncConnection)
                client.syncConnection->cancelClipboardRequest(helper.clipboardToolRequestID);
        }
    }
    helper.clipboardToolReplySourceClientID = helper.clipboardToolSelectionClientID;
    helper.clipboardToolRequestID = 0;
    helper.clipboardToolSourceClientID = 0;
    helper.clipboardToolDeadlineMilliseconds = 0;
    helper.sessionExitDeadlineMilliseconds = nowMilliseconds() + 1000;
    if (!didQueue)
        dropClient(index);
}

bool Daemon::wireRemoteInput(Session& session)
{
    session.inputSink.reset(new (std::nothrow) SessionInputSink(*this, session));
    if (!session.inputSink || !session.term || !session.screenSource)
        return false;
    session.inputEncoder.reset(new (std::nothrow) Runtime::RemoteInputEncoder(*session.term, *session.inputSink));
    if (!session.inputEncoder)
        return false;
    session.screenSource->setInputWriter(*session.inputEncoder);
    return true;
}

void Daemon::stampInputOrigin(Session& session, const Client& client)
{
    setSessionInputDriver(session, client.clientID);
    std::string grantIdentity = grantKeyFor(client);
    std::string inputPrincipal = grantIdentity;
    const Ubiquitty::CapabilityOrigin origin = capabilityOriginOf(client);
    const auto* attachment = client.syncConnection ? client.syncConnection->attachment() : nullptr;
    const bool isAuthenticatedHuman = (client.hasToken && client.hasBootstrapToken && client.isInteractiveUser)
        || (client.hasOwnerLocalCredentials && client.isRelayedFromSSH);
    if (origin == Ubiquitty::CapabilityOrigin::Remote && attachment && isAuthenticatedHuman
        && !isAgentClient(client) && attachment->authority.verifiedKind == Protocol::Sync::ClientKind::Human
        && attachment->servingConnection == client.clientID) {
        // The uid-gated SSH bridge or its one-time interactive QUIC bootstrap
        // proves this host account. A fresh token or attachment does not make
        // the same account another input principal.
        inputPrincipal = "human-account:" + std::to_string(::geteuid());
    }
    const bool wasAmbiguous = session.hasMultipleInputPrincipals;
    if (!session.firstInputPrincipal)
        session.firstInputPrincipal = inputPrincipal;
    if (inputPrincipal.empty() || *session.firstInputPrincipal != inputPrincipal)
        session.hasMultipleInputPrincipals = true;
    if (!wasAmbiguous && session.hasMultipleInputPrincipals)
        Ubiquitty::PAL::logLine(stderr, "daemon: session {} cannot attribute output to one input principal; capability grants are withheld", session.id);
    if (!session.term)
        return;
    session.term->currentCapabilityOrigin = origin;
    session.capabilityPolicy.setCurrentIdentity(std::move(grantIdentity));
    // Ratchet the sticky origin floor: this injecting principal may spawn
    // background work whose output arrives after it detaches; the floor keeps
    // that output gated at (at least) this origin so a later detach can't
    // downgrade the gate below it.
    session.driverOriginFloor = std::max(session.driverOriginFloor, origin);
}

bool Daemon::mayWriteRemoteInput(Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance) const
{
    const auto* attachment = m_attachments->find(authority);
    if (!attachment || !attachment->authority.mayDrive || attachment->clientInstance != clientInstance
        || authority.low == 0 || authority.low > std::numeric_limits<std::uint64_t>::max() / 2)
        return false;
    const auto now = static_cast<std::uint64_t>(::time(nullptr));
    const auto grant = std::ranges::find(m_attachmentGrants, authority, &AttachmentGrant::attachmentID);
    if (grant != m_attachmentGrants.end()
        && (!grant->scope.contains(Protocol::Sync::Scope::Drive) || (grant->tokenExpiryUnix && now >= grant->tokenExpiryUnix)))
        return false;
    for (const Client& client : m_clients) {
        if (client.clientID == attachment->servingConnection && client.hasToken
            && (!tokenScopeAllows(client, Protocol::Sync::Scope::Drive) || (client.tokenExpiryUnix && now >= client.tokenExpiryUnix)))
            return false;
    }
    return true;
}

Protocol::Sync::WriteStatus Daemon::writeRemoteInput(Session& session, Protocol::Sync::ID128 authority, Protocol::Sync::ID128 clientInstance, std::span<const std::uint8_t> bytes, RemoteInputLane lane)
{
    if (!session.stream || session.shellExited)
        return Protocol::Sync::WriteStatus::Closed;
    // A pane a closing verb took out of its workspace ends when the closure
    // settles, and its handle stays bound until then. Input behind the close,
    // in the same read or held from before it, would reach the shell first.
    if (const HostedWorkspace* workspace = workspaceHolding(session.id);
        workspace && workspace->layout.surfaceHolding(workspace->paneFor(session.id)) == Ubiquitty::Session::Layout::invalidSurface)
        return Protocol::Sync::WriteStatus::Closed;
    if (authority != Protocol::Sync::ID128 {} && !mayWriteRemoteInput(authority, clientInstance))
        return Protocol::Sync::WriteStatus::Withdrawn;
    const auto driver = std::ranges::find_if(m_clients, [&](const Client& client) {
        const auto* attachment = client.syncConnection ? client.syncConnection->attachment() : nullptr;
        return attachment && attachment->attachmentID == authority && attachment->servingConnection == client.clientID
            && !client.didOverflow && client.subscriptionFor(session.id);
    });
    if (driver != m_clients.end())
        stampInputOrigin(session, *driver);
    // Input typed after a resize reaches a program that has been told of it.
    applyPendingGeometry(session, driver != m_clients.end() ? std::optional { geometryKey(*driver) } : std::nullopt);
    const std::span<const char> encoded { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
    const std::uint64_t streamOwner = authority.low * 2 + (lane == RemoteInputLane::Paste ? 1 : 0);
    const auto result = authority == Protocol::Sync::ID128 {} ? std::optional { session.stream->writeAll(encoded) } : session.stream->writeAllOwned(streamOwner, encoded);
    if (!result)
        return Protocol::Sync::WriteStatus::Refused;
    const int written = *result;
    if (written == std::to_underlying(PAL::StreamStatus::Again))
        return Protocol::Sync::WriteStatus::Refused;
    if (written == std::to_underlying(PAL::StreamStatus::Closed) || written == std::to_underlying(PAL::StreamStatus::Fatal)) {
        session.exitReason = written == std::to_underlying(PAL::StreamStatus::Closed) ? "pty write closed (child gone) during remote input" : "pty write fatal (allocation refused) during remote input";
        session.shellExited = true;
        return Protocol::Sync::WriteStatus::Closed;
    }
    noteLayoutInput(session, bytes);
    // A program that turned echo off, or moved a signal character, without
    // writing anything published no frame to say so. Typing is when a client
    // needs to know: a frame is due, and it carries the echo context.
    if (session.publishedEchoFacts && *session.publishedEchoFacts != echoFactsOf(session))
        session.isFrameDue = true;
    return Protocol::Sync::WriteStatus::Written;
}

void Daemon::drainRemoteInput(Session& session)
{
    // Expiry may have passed while poll slept. Do not flush its user-space
    // suffix before the regular next-loop token sweep notices.
    dropExpiredTokenClients();
    reapAttachments(nowMicroseconds());
    if (!session.stream || session.shellExited)
        return;
    (void)session.stream->drainPendingWrite();
    if (session.screenSource && session.screenSource->hasPendingInput())
        session.screenSource->drainInput(nowMicroseconds());
    drainTermToPTY(session);
}

void Daemon::setSessionInputDriver(Session& session, std::uint32_t clientID)
{
    if (session.lastInputClientID == clientID) {
        return;
    }
    for (const auto& previous : m_clients) {
        if (previous.subscriptionFor(session.id) && previous.clientID != clientID) {
            cancelClipboardTools(previous.clientID, session.id);
        }
    }
    session.lastInputClientID = clientID;
    for (const Client& client : m_clients) {
        if (client.clientID == clientID && !isAgentClient(client)) {
            session.lastHumanInputClientID = clientID;
            break;
        }
    }
}

void Daemon::cancelClipboardTools(std::uint32_t sourceClientID,
    std::uint32_t sessionID)
{
    Protocol::Sync::ClipboardDataMessage refused;
    refused.status = Protocol::Sync::ClipboardServeStatus::Refused;
    for (std::size_t index = 0; index < m_clients.size(); ++index) {
        if (sessionID && m_clients[index].clipboardToolSessionID != sessionID)
            continue;
        if (m_clients[index].clipboardToolReplySourceClientID == sourceClientID && sourceClientID) {
            m_clients[index].clearPendingOutput();
            m_clients[index].clipboardToolReplySourceClientID = 0;
            m_clients[index].didOverflow = true;
        }
        if (m_clients[index].clipboardToolSourceClientID == sourceClientID && sourceClientID) {
            finishClipboardTool(index, refused);
        }
    }
}

void Daemon::expireClipboardTools()
{
    const auto now = nowMilliseconds();
    Protocol::Sync::ClipboardDataMessage refused;
    refused.status = Protocol::Sync::ClipboardServeStatus::Refused;
    for (std::size_t index = 0; index < m_clients.size(); ++index) {
        if (m_clients[index].clipboardToolDeadlineMilliseconds && m_clients[index].clipboardToolDeadlineMilliseconds <= now) {
            finishClipboardTool(index, refused);
        }
    }
}

void Daemon::evictOldestCapabilityGrantIfFull()
{
    if (m_pendingCapabilityGrants.size() < maxPendingCapabilityGrants)
        return;
    // Request ids come from one monotonic counter, so the smallest outstanding
    // one is the oldest question and is the one to drop.
    auto oldest = m_pendingCapabilityGrants.begin();
    for (auto it = m_pendingCapabilityGrants.begin(); it != m_pendingCapabilityGrants.end(); ++it)
        if (it->first < oldest->first)
            oldest = it;
    // Resolved as well as erased, and that pairing is the whole of why this is
    // safe to do. The queue dedupes on the triple, so a record dropped while
    // the triple still counts as pending is a capability nothing will ever ask
    // about again: the next reach is suppressed as a repeat and the policy
    // answers Prompt forever. Resolving puts the question back in reach.
    retireCapabilityQuestion(oldest->first, oldest->second);
    m_pendingCapabilityGrants.erase(oldest);
}

void Daemon::retireCapabilityQuestion(std::uint32_t requestID, const PendingCapabilityGrant& pending)
{
    retireConsentQuestion(Protocol::Sync::ConsentKind::Capability, requestID, pending.recipients);
    discardPendingClipboardWrite(requestID, pending.sessionID);
    m_consentQueue.resolve(pending.capability, pending.origin, pending.identity);
}

void Daemon::discardPendingClipboardWrite(std::uint32_t requestID, std::uint32_t sessionID)
{
    if (Session* session = findSession(sessionID))
        session->capabilityPolicy.discardClipboardWrite(requestID);
}

void Daemon::cancelPendingClipboardWrites(std::uint32_t clientID)
{
    if (!clientID)
        return;
    std::erase_if(m_pendingCapabilityGrants, [&](const auto& entry) {
        const auto& pending = entry.second;
        if (!pending.clipboardRecipientClientID
            || (pending.clipboardSourceClientID != clientID && pending.clipboardRecipientClientID != clientID))
            return false;
        retireCapabilityQuestion(entry.first, pending);
        return true;
    });
}

void Daemon::expireCapabilityQuestions(std::uint64_t now)
{
    std::erase_if(m_pendingCapabilityGrants, [&](const auto& entry) {
        const auto& pending = entry.second;
        if (pending.deadlineMilliseconds > now)
            return false;
        retireCapabilityQuestion(entry.first, pending);
        return true;
    });
}

bool Daemon::mayCompleteClipboardWrite(const Session& session, std::uint32_t sourceClientID,
    std::uint32_t recipientClientID, std::string_view identity) const
{
    if (!session.term || !session.term->clipboardClient || !session.term->isOSC52Allowed
        || session.term->osc52ClipboardWritePolicy == OSC::ClipboardWritePolicy::Deny)
        return false;
    const std::size_t primary = primaryHumanForSession(session);
    if (primary >= m_clients.size() || m_clients[primary].clientID != recipientClientID)
        return false;
    const auto nowUnix = static_cast<std::uint64_t>(::time(nullptr));
    const auto isExpired = [nowUnix](const Client& client) {
        return client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= nowUnix;
    };
    if (isExpired(m_clients[primary]))
        return false;
    return std::ranges::any_of(m_clients, [&](const Client& source) {
        return source.clientID == sourceClientID && source.helloSeen && !source.didOverflow
            && !isExpired(source)
            && !source.sessionExitDeadlineMilliseconds && source.subscriptionFor(session.id)
            && tokenScopeAllows(source, Protocol::Sync::Scope::Drive) && grantKeyFor(source) == identity;
    });
}

void Daemon::sendCapabilityGrantRequests(Session& s)
{
    const bool wasClipboardWriteRefused = s.capabilityPolicy.takeClipboardWriteRefusal();
    for (const Ubiquitty::CapabilityGrantKey& pend : m_consentQueue.takeNewlyPending()) {
        if (wasClipboardWriteRefused && pend.capability == Capability::ClipboardWrite) {
            s.capabilityPolicy.discardClipboardWrite(0);
            m_consentQueue.resolve(pend.capability, pend.origin, pend.identity);
            continue;
        }
        evictOldestCapabilityGrantIfFull();
        if (!m_nextConsentRequestID) {
            s.capabilityPolicy.discardClipboardWrite(0);
            m_consentQueue.resolve(pend.capability, pend.origin, pend.identity);
            continue;
        }
        const uint32_t requestID = m_nextConsentRequestID++;
        auto* held = s.capabilityPolicy.pendingClipboardWrite();
        const bool isClipboardWrite = pend.capability == Capability::ClipboardWrite && held && !held->requestID
            && held->identity == pend.identity && held->origin == pend.origin;
        std::uint32_t clipboardRecipient = 0;
        if (isClipboardWrite) {
            const std::size_t primary = primaryHumanForSession(s);
            if (primary < m_clients.size())
                clipboardRecipient = m_clients[primary].clientID;
            if (!clipboardRecipient || !mayCompleteClipboardWrite(s, s.lastInputClientID, clipboardRecipient, pend.identity)) {
                s.capabilityPolicy.discardClipboardWrite(0);
                m_consentQueue.resolve(pend.capability, pend.origin, pend.identity);
                continue;
            }
        }
        Protocol::Sync::ConsentQuestion req {};
        req.requestID = requestID;
        req.sessionID = s.id;
        req.action = 2; // capability grant
        req.capabilityCode = static_cast<uint16_t>(std::to_underlying(pend.capability));
        // The retained grant key remains the authority. Presentation facts may
        // come only from a current admitted driver with that same server-derived
        // key; neither a claimed label nor a process spelling is a substitute.
        const auto nowUnix = static_cast<std::uint64_t>(::time(nullptr));
        const auto isSource = [&](const Client& candidate) {
            return !pend.identity.empty() && candidate.helloSeen && !candidate.didOverflow
                && !candidate.sessionExitDeadlineMilliseconds && hasLiveStructureAttachment(candidate)
                && (!candidate.hasToken || !candidate.tokenExpiryUnix || candidate.tokenExpiryUnix > nowUnix)
                && candidate.subscriptionFor(s.id) && tokenScopeAllows(candidate, Protocol::Sync::Scope::Drive)
                && grantKeyFor(candidate) == pend.identity;
        };
        auto source = std::ranges::find_if(m_clients, [&](const Client& candidate) {
            return candidate.clientID == s.lastInputClientID && isSource(candidate);
        });
        if (source == m_clients.end() && !isClipboardWrite)
            source = std::ranges::find_if(m_clients, isSource);
        if (source != m_clients.end()) {
            req.peerProcessID = source->peerProcessID;
            req.peerProcess = source->peerProcessID ? source->peerProcess : std::string {};
            req.agentLabel = source->agentLabel;
        }
        // The lifetime a grant may have depends on the origin, so the prompt
        // needs it to offer only what the daemon will keep.
        req.capabilityOrigin = std::to_underlying(pend.origin);
        std::vector<ConsentRecipient> asked;
        asked.reserve(m_clients.size());
        for (std::size_t i = 0; i < m_clients.size(); ++i) {
            if ((!clipboardRecipient || m_clients[i].clientID == clipboardRecipient) && mayBeAskedFor(m_clients[i], s.id)
                && m_clients[i].pendingConsentRetirements.empty()) {
                // Per recipient: the person whose own device drives the program
                // is asked about their device, not about an unnamed principal.
                req.isRecipientsOwnRequest = grantKeyFor(m_clients[i]) == pend.identity;
                if (const auto notification = Protocol::Sync::consentQuestionNotify(req);
                    notification && m_clients[i].syncConnection->sendNotify(*notification))
                    asked.push_back({ .clientID = m_clients[i].clientID, .sessionID = s.id, .attachmentID = m_clients[i].syncConnection->attachment()->attachmentID });
            }
        }
        if (asked.empty()) {
            // Nobody was there to ask, which on a host with no human attached is
            // the ordinary state rather than an edge. A triple is recorded as
            // pending by check(), and the queue dedupes on it, so leaving it
            // there would make the first reach for each capability consume its
            // own prompt: a human arriving later is shown nothing, and the next
            // reach emits nothing because the triple already counts as asked.
            // Asked means delivered to someone who could answer.
            m_consentQueue.resolve(pend.capability, pend.origin, pend.identity);
            continue;
        }
        // Recorded only now, because a record is what a request id means and
        // there is no request to mean anything by until one has gone out.
        m_pendingCapabilityGrants[requestID] = { pend.capability, pend.origin, pend.identity, s.id,
            isClipboardWrite ? s.lastInputClientID : 0, clipboardRecipient,
            nowMilliseconds() + consentTimeoutMilliseconds, std::move(asked) };
        if (isClipboardWrite)
            held->requestID = requestID;
    }
    // Another session may already own this principal's deduplicated question.
    // A write without its own request ID is never attached to that question.
    s.capabilityPolicy.discardClipboardWrite(0);
}

bool Daemon::asksAboutHostEffects(const Client& client) const
{
    return client.syncConnection && client.syncConnection->asksAboutHostEffects();
}

std::size_t Daemon::primaryHumanForSession(const Session& s) const
{
    // A human who can drive the session and is still being served. A viewer
    // holds no Drive scope, so a program's write never lands on a clipboard
    // whose owner is only watching; an agent is not a person and has none.
    auto isCandidate = [&](const Client& client) {
        return client.subscriptionFor(s.id) && client.helloSeen && !client.didOverflow && !client.sessionExitDeadlineMilliseconds
            && !isAgentClient(client) && tokenScopeAllows(client, Protocol::Sync::Scope::Drive);
    };
    // The human who typed last: the program was run by them, for them.
    if (s.lastHumanInputClientID != 0) {
        for (std::size_t i = 0; i < m_clients.size(); ++i)
            if (m_clients[i].clientID == s.lastHumanInputClientID && isCandidate(m_clients[i]))
                return i;
    }
    // Nobody who is here has typed. One human is unambiguous; two are a coin
    // toss between two people's machines, which delivers to neither.
    std::size_t only = m_clients.size();
    for (std::size_t i = 0; i < m_clients.size(); ++i) {
        if (!isCandidate(m_clients[i]))
            continue;
        if (only != m_clients.size())
            return m_clients.size();
        only = i;
    }
    return only;
}

void Daemon::wireBackgroundNotifications(Session& session)
{
    const auto pane = session.screenSource->paneID();
    const std::string identity = Ubiquitty::Session::backgroundPaneID(pane.high, pane.low);
    session.effects.setNotificationRecorder([this, identity, sessionID = session.id, &policy = session.capabilityPolicy](const HostNotification& notification) {
        // Other devices and background refresh post what is recorded here
        // without asking, so a banner forwarded on its recipient's own
        // question, which nobody here was asked about, is not recorded.
        if (policy.takeForwardedNotification())
            return;
        m_backgroundEvents.appendNotification(identity, sessionID,
            { notification.title, notification.body, notification.identifier, notification.occasion }, m_wallClockMilliseconds());
        scheduleBackgroundEventWrite();
    });
}

void Daemon::installTerminalClients(Session& s)
{
    // What the gate admits reaches a person's machine through these; see
    // deliverSessionEffects.
    s.term->clipboardClient = &s.clipboardClient;
    s.term->shellClient = &s.shellClient;
    s.term->dragDropClient = &s.dragDropQueue;
    s.term->effectRoute = &s.effectRoute;
    s.term->tickClock = &s.clock;
    s.term->textMeasureClient = Ubiquitty::CheckedPtr<Ubiquitty::TextMeasureClient>(&s.runFaces);
    // The host answers every program query for its sessions (Docs/sync.md §3),
    // whatever a terminal it adopted or decoded was marked as.
    s.term->replyAuthority = ReplyAuthority::ThisTerminal;
}

std::optional<Protocol::Sync::ID128> Daemon::typedEffectPrimaryForSession(const Session& s) const
{
    if (!s.screenSource || !std::ranges::any_of(m_clients, [&s](const Client& client) {
            return client.helloSeen && !client.didOverflow && !client.sessionExitDeadlineMilliseconds
                && client.subscriptionFor(s.id) && client.syncConnection
                && client.syncConnection->state() == Protocol::Sync::ConnectionState::Attached;
        }))
        return std::nullopt;
    const std::size_t primary = primaryHumanForSession(s);
    if (primary < m_clients.size() && m_clients[primary].syncConnection)
        return m_clients[primary].syncConnection->clientInstance();
    return Protocol::Sync::ID128 {};
}

void Daemon::emitPendingBell(Session& s)
{
    if (!s.term || !s.term->bellPending)
        return;
    // The route counts each bell; a pending bell it never heard (one a
    // handover carried) is still one.
    const std::uint32_t bells = std::max<std::uint32_t>(s.effectRoute.pendingBells(), 1);
    const auto primary = typedEffectPrimaryForSession(s);
    if (!primary || !s.screenSource->appendBells(bells, *primary))
        return;
    s.effectRoute.clearBells();
    s.term->bellPending = false;
    Ubiquitty::Session::Event rang;
    rang.kind = Ubiquitty::Session::EventKind::Bell;
    rang.sessionID = s.id;
    rang.paneID = paneOfSession(s);
    recordEvent(std::move(rang));
}

void Daemon::emitPendingSounds(Session& s)
{
    if (!s.term || !s.term->pendingSound)
        return;
    const auto primary = typedEffectPrimaryForSession(s);
    if (!primary)
        return;
    auto& pending = *s.term->pendingSound;
    while (const auto* sound = pending.front()) {
        if (!s.screenSource->appendSound(sound->request, sound->sampleSerial, *primary))
            return;
        pending.pop();
    }
}

void Daemon::deliverSessionEffects(Session& s)
{
    deliverSessionDragDrop(s);
    if (s.effects.isEmpty())
        return;
    std::vector<HostEffectQueue::Effect> effects = s.effects.take();
    const std::size_t primary = primaryHumanForSession(s);
    if (primary >= m_clients.size())
        return;
    if (!m_clients[primary].subscriptionFor(s.id))
        return;
    const auto* connection = m_clients[primary].syncConnection.get();
    for (HostEffectQueue::Effect& effect : effects) {
        const bool isSameRecipient = std::visit([&](auto& owner) {
            if (owner.primary)
                return true;
            if (!owner.pendingPrimaryClientID)
                owner.pendingPrimaryClientID = m_clients[primary].clientID;
            return owner.pendingPrimaryClientID == m_clients[primary].clientID;
        },
            effect);
        if (!isSameRecipient)
            continue;
        // The selected human may still be completing the inner handshake.
        // Keep the exact operation; an outer catalog is not an effect carrier.
        if (!s.screenSource || !connection || connection->state() != Protocol::Sync::ConnectionState::Attached) {
            s.effects.returnUndelivered(std::move(effect));
            continue;
        }
        if (auto* write = std::get_if<HostEffectQueue::ClipboardWrite>(&effect)) {
            if (!write->primary)
                write->primary = connection->clientInstance();
            if (s.isFrameDue) {
                s.effects.returnUndelivered(std::move(effect));
                continue;
            }
            const auto retained = s.screenSource->appendClipboardWrite(write->buffer, write->encoded, *write->primary);
            if (!retained && retained.error() != Protocol::Sync::EffectRefusal::Invalid)
                s.effects.returnUndelivered(std::move(effect));
        } else if (auto* notification = std::get_if<HostNotification>(&effect)) {
            if (!notification->primary)
                notification->primary = connection->clientInstance();
            // Its text may have arrived beside changed cells. Wait for
            // the frame rule to assign that output its picture boundary.
            if (s.isFrameDue) {
                s.effects.returnUndelivered(std::move(effect));
                continue;
            }
            // The durable journal retains longer text than the live wire.
            // Bound only this presentation, preserving the recorded owner.
            const auto title = Ubiquitty::Session::backgroundEventText(notification->title, OSC::attentionTextCapacity);
            const auto message = Ubiquitty::Session::backgroundEventText(notification->body, OSC::attentionTextCapacity);
            const Protocol::Sync::NotificationEffect body { title, message, notification->identifier, notification->occasion };
            if (!s.screenSource->appendEffect(body, *notification->primary))
                s.effects.returnUndelivered(std::move(effect));
        }
    }
}

bool Daemon::mayReceiveNativeDrag(std::uint32_t clientID, std::uint32_t handle, std::uint64_t epoch) const
{
    const auto client = std::ranges::find(m_clients, clientID, &Client::clientID);
    if (client == m_clients.end() || !client->helloSeen || client->didOverflow || client->sessionExitDeadlineMilliseconds)
        return false;
    const auto nowUnix = static_cast<std::uint64_t>(::time(nullptr));
    if (client->hasToken && client->tokenExpiryUnix && client->tokenExpiryUnix <= nowUnix)
        return false;
    const auto subscription = std::ranges::find_if(client->paneSubscriptions, [handle](const auto& pane) { return pane.screenHandle == handle; });
    if (subscription == client->paneSubscriptions.end())
        return false;
    for (const auto& session : m_sessions) {
        if (!session || session->id != subscription->sessionID)
            continue;
        const auto primary = primaryHumanForSession(*session);
        return session->dragDropOwner == clientID && session->dragDropEpoch == epoch
            && primary < m_clients.size() && m_clients[primary].clientID == clientID;
    }
    return false;
}

void Daemon::deliverSessionDragDrop(Session& session)
{
    if (!session.term)
        return;
    const auto primary = primaryHumanForSession(session);
    const auto owner = primary < m_clients.size() && m_clients[primary].syncConnection
            && m_clients[primary].syncConnection->supportsNativeDrag()
        ? m_clients[primary].clientID
        : 0;
    if (owner != session.dragDropOwner) {
        const auto previous = std::ranges::find(m_clients, session.dragDropOwner, &Client::clientID);
        if (previous != m_clients.end() && previous->syncConnection) {
            if (const auto* pane = previous->subscriptionFor(session.id)) {
                previous->syncConnection->cancelNativeDragCommands(pane->screenHandle);
                DragDrop::Metadata stop;
                stop.type = DragDrop::Type::StopAccepting;
                (void)previous->syncConnection->sendNativeDragCommand(pane->screenHandle, session.dragDropEpoch,
                    { .kind = DragDrop::CommandKind::Registration, .metadata = stop });
                stop.type = DragDrop::Type::Offer;
                stop.x = 2;
                (void)previous->syncConnection->sendNativeDragCommand(pane->screenHandle, session.dragDropEpoch,
                    { .kind = DragDrop::CommandKind::Registration, .metadata = stop });
            }
        }
        DragDrop::rebindNativeOwner(*session.term);
        session.dragDropQueue.clear();
        session.dragDropOwner = owner;
        if (session.dragDropEpoch == UINT64_MAX)
            return;
        ++session.dragDropEpoch;
        DragDrop::publishRegistration(*session.term);
    }
    if (!owner || session.dragDropEpoch == UINT64_MAX)
        return;
    auto& client = m_clients[primary];
    const auto* pane = client.subscriptionFor(session.id);
    if (!pane)
        return;
    while (const auto* command = session.dragDropQueue.front()) {
        if (!client.syncConnection->sendNativeDragCommand(pane->screenHandle, session.dragDropEpoch,
                { .kind = command->kind, .sessionID = command->sessionID, .requestID = command->requestID, .metadata = command->metadata, .payload = { command->bytes.data(), command->bytes.size() }, .isFinal = command->isFinal }))
            break;
        (void)session.dragDropQueue.take();
    }
}

bool Daemon::mayRuleOnConsentFor(const Client& c, std::uint32_t sessionID) const
{
    return c.helloSeen && !c.didOverflow && !c.sessionExitDeadlineMilliseconds
        && hasLiveStructureAttachment(c) && !isAgentClient(c)
        && (!c.hasToken || !c.tokenExpiryUnix || c.tokenExpiryUnix > static_cast<std::uint64_t>(::time(nullptr)))
        && tokenScopeAllows(c, Protocol::Sync::Scope::Observe) && c.subscriptionFor(sessionID) != nullptr;
}

bool Daemon::mayBeAskedFor(const Client& c, std::uint32_t sessionID) const
{
    return showsQuestions(c) && mayRuleOnConsentFor(c, sessionID);
}

bool Daemon::mayAttachmentResume(Protocol::Sync::ID128 attachmentID) const
{
    using Protocol::Sync::AttachmentState;
    const auto* record = m_attachments->find(attachmentID);
    if (!record || record->admission != Protocol::Sync::AttachmentAdmission::Granted || !record->isResumable)
        return false;
    if (record->state == AttachmentState::Lost)
        return true;
    // A connection being dropped, its link gone silent or its reader too far
    // behind, leaves its attachment lost when it is reaped.
    const auto serving = std::ranges::find(m_clients, record->servingConnection, &Client::clientID);
    return record->state == AttachmentState::Live && serving != m_clients.end() && serving->didOverflow && !serving->didDetachCleanly;
}

template<typename IsAskable>
bool Daemon::mayRecipientReturn(const ConsentRecipient& recipient, std::span<const ConsentRecipient> recipients, const IsAskable& isAskable) const
{
    switch (recipient.standing) {
    case ConsentRecipient::Standing::Asked:
    case ConsentRecipient::Standing::Declined:
        return false;
    case ConsentRecipient::Standing::Left:
        return mayAttachmentResume(recipient.attachmentID);
    case ConsentRecipient::Standing::Continued: {
        // The connection now serving it is the same person's, and is asked
        // on attaching: it waits on that connection only while it could be
        // asked and has not declined the question itself.
        const auto* record = m_attachments->find(recipient.attachmentID);
        if (!record || record->state == Protocol::Sync::AttachmentState::Detached)
            return false;
        const auto serving = std::ranges::find(m_clients, record->servingConnection, &Client::clientID);
        return serving != m_clients.end() && isAskable(*serving) && std::ranges::none_of(recipients, [&](const ConsentRecipient& other) {
            return other.clientID == record->servingConnection && other.standing == ConsentRecipient::Standing::Declined;
        });
    }
    }
    return false;
}

template<typename IsAskable>
bool Daemon::mayAskedPersonReturn(std::span<const ConsentRecipient> recipients, const IsAskable& isAskable) const
{
    return std::ranges::any_of(recipients, [&](const ConsentRecipient& recipient) { return mayRecipientReturn(recipient, recipients, isAskable); });
}

template<typename IsAskable>
bool Daemon::isRecipientKept(const ConsentRecipient& recipient, std::span<const ConsentRecipient> recipients, const IsAskable& isAskable) const
{
    const auto current = std::ranges::find(m_clients, recipient.clientID, &Client::clientID);
    switch (recipient.standing) {
    case ConsentRecipient::Standing::Asked:
        // One that cannot be asked now, its link silent or its reader being
        // dropped, is kept while its person may resume and be asked again.
        return (current != m_clients.end() && isAskable(*current)) || mayAttachmentResume(recipient.attachmentID);
    case ConsentRecipient::Standing::Declined:
        // A decliner stays listed while its connection lasts, however briefly
        // it cannot be asked, so it is never asked again.
        return current != m_clients.end() && !current->didOverflow;
    case ConsentRecipient::Standing::Left:
    case ConsentRecipient::Standing::Continued:
        return mayRecipientReturn(recipient, recipients, isAskable);
    }
    return false;
}

void Daemon::sendConsentRequestToHumans(Client& agent, std::optional<std::size_t> recipient)
{
    std::erase_if(agent.consentRecipients, [&](const ConsentRecipient& recipient) {
        return !isRecipientKept(recipient, agent.consentRecipients, [&](const Client& client) { return mayBeAskedFor(client, agent.consentSessionID); });
    });
    const auto isRecipient = [&](std::size_t index) {
        return (!recipient || *recipient == index) && mayBeAskedFor(m_clients[index], agent.consentSessionID)
            && m_clients[index].pendingConsentRetirements.empty()
            && std::ranges::find(agent.consentRecipients, m_clients[index].clientID, &ConsentRecipient::clientID) == agent.consentRecipients.end();
    };
    if (agent.consentRecipients.capacity() < maxClients) {
        const auto index = static_cast<std::size_t>(&agent - m_clients.data());
        const auto capacity = maxClients * sizeof(ConsentRecipient);
        if (!admitsAssemblyCapacity(index, capacity - agent.consentRecipients.capacity() * sizeof(ConsentRecipient)))
            return;
        agent.consentRecipients.reserve(maxClients);
    }
    Protocol::Sync::ConsentQuestion request;
    request.requestID = agent.consentRequestID;
    request.sessionID = agent.consentSessionID;
    request.peerProcessID = agent.peerProcessID;
    request.peerProcess = agent.peerProcessID ? agent.peerProcess : std::string {};
    request.agentLabel = agent.agentLabel;
    request.action = agent.consentAction == Client::ConsentAction::Terminate ? 1 : 0;
    const auto notification = Protocol::Sync::consentQuestionNotify(request);
    if (!notification)
        return;
    // A request re-sent to one late-joining person is the same question, so
    // only the first asking is an event.
    if (!recipient) {
        Ubiquitty::Session::Event asked;
        asked.kind = Ubiquitty::Session::EventKind::ConsentRequested;
        asked.sessionID = agent.consentSessionID;
        asked.text = agent.agentLabel;
        asked.detail = request.action ? "terminate" : "attach";
        recordEvent(std::move(asked));
    }
    for (std::size_t i = 0; i < m_clients.size(); ++i)
        if (isRecipient(i) && agent.consentRecipients.size() < maxClients && m_clients[i].syncConnection->sendNotify(*notification))
            agent.consentRecipients.push_back({ .clientID = m_clients[i].clientID, .sessionID = agent.consentSessionID, .attachmentID = m_clients[i].syncConnection->attachment()->attachmentID });
}

bool Daemon::retainAttachConsentDecision(std::size_t index, const Client& approver, Protocol::Sync::ConsentChoice decision)
{
    if (index >= m_clients.size() || decision > Protocol::Sync::ConsentChoice::AllowAlways
        || !mayRuleOnConsentFor(approver, m_clients[index].consentSessionID))
        return false;
    Client& requester = m_clients[index];
    const auto* requesterAttachment = requester.syncConnection ? requester.syncConnection->attachment() : nullptr;
    const auto* approverAttachment = approver.syncConnection->attachment();
    if (!requester.consentPending || requester.consentAction != Client::ConsentAction::Attach
        || std::ranges::find(requester.consentRecipients, approver.clientID, &ConsentRecipient::clientID) == requester.consentRecipients.end()
        || requester.attachConsentApproval || requester.attachConsentTerminal || !requesterAttachment || !approverAttachment
        || requesterAttachment->admission != Protocol::Sync::AttachmentAdmission::AwaitingConsent
        || nowMilliseconds() >= requester.consentDeadlineMilliseconds)
        return false;
    if (decision == Protocol::Sync::ConsentChoice::Deny) {
        denyAgentAttach(index, Protocol::Sync::ConsentStatusPhase::Denied);
        return true;
    }
    auto requesterPrincipal = principalOf(requester);
    auto approverPrincipal = principalOf(approver);
    auto approverIdentity = managementIdentityOf(approver);
    if (!admitsAssemblyCapacity(index, requesterPrincipal.capacity() + approverPrincipal.capacity() + approverIdentity.capacity()))
        return false;
    requester.attachConsentApproval = Client::AttachConsentApproval {
        .question = { .requestID = requester.consentRequestID, .sessionID = requester.consentSessionID },
        .decision = decision,
        .requesterAttachment = requesterAttachment->attachmentID,
        .requesterPrincipal = std::move(requesterPrincipal),
        .requesterScopes = requester.tokenScope,
        .approverClientID = approver.clientID,
        .approverAttachment = approverAttachment->attachmentID,
        .approverPrincipal = std::move(approverPrincipal),
        .approverIdentity = std::move(approverIdentity),
        .targetPane = requester.consentPaneID,
    };
    return true;
}

void Daemon::servicePendingAttachConsent(std::size_t index)
{
    using namespace Protocol::Sync;
    if (index >= m_clients.size())
        return;
    Client& client = m_clients[index];
    if (!client.consentPending || client.consentAction != Client::ConsentAction::Attach || !client.syncConnection)
        return;
    HostConnection& connection = *client.syncConnection;
    if (connection.state() != ConnectionState::AwaitingConsent && connection.state() != ConnectionState::ApprovedAwaitingDelivery)
        return;
    const ConsentBinding question { .requestID = client.consentRequestID, .sessionID = client.consentSessionID };
    (void)connection.sendConsentStatus({ .question = question, .phase = ConsentStatusPhase::Pending });
    const auto now = nowMilliseconds();
    if (!client.attachConsentTerminal && now >= client.consentDeadlineMilliseconds) {
        client.attachConsentApproval.reset();
        client.attachConsentTerminal = ConsentStatusPhase::Expired;
    }
    if (!client.attachConsentTerminal && !client.attachConsentApproval) {
        const auto isAskable = [&](const Client& candidate) { return mayBeAskedFor(candidate, question.sessionID); };
        std::erase_if(client.consentRecipients, [&](const ConsentRecipient& recipient) {
            return !isRecipientKept(recipient, client.consentRecipients, isAskable);
        });
        for (std::size_t recipient = 0; recipient < m_clients.size(); ++recipient)
            if (isAskable(m_clients[recipient])
                && std::ranges::find(client.consentRecipients, m_clients[recipient].clientID, &ConsentRecipient::clientID) == client.consentRecipients.end())
                sendConsentRequestToHumans(client, recipient);
        // Asking prunes those who can no longer answer: a question nobody is
        // left to answer or return to is refused now, not at its deadline. A
        // person present who could be asked, though asking failed this pass,
        // is asked again on the next.
        const bool hasUnaskedHuman = std::ranges::any_of(m_clients, [&](const Client& candidate) {
            return isAskable(candidate)
                && std::ranges::find(client.consentRecipients, candidate.clientID, &ConsentRecipient::clientID) == client.consentRecipients.end();
        });
        if (!isStillAsked(client.consentRecipients) && !mayAskedPersonReturn(client.consentRecipients, isAskable)
            && !hasUnaskedHuman)
            client.attachConsentTerminal = ConsentStatusPhase::Denied;
    }
    if (client.attachConsentTerminal) {
        const auto phase = *client.attachConsentTerminal;
        if (!connection.sendConsentStatus({ .question = question, .phase = phase }))
            return;
        retireClientConsent(client);
        client.consentPending = false;
        client.attachConsentApproval.reset();
        client.attachConsentTerminal.reset();
        Ubiquitty::Session::Event refused;
        refused.kind = Ubiquitty::Session::EventKind::ConsentDenied;
        refused.sessionID = question.sessionID;
        refused.text = client.agentLabel;
        refused.detail = phase == ConsentStatusPhase::Expired ? "timed out" : "denied";
        recordEvent(std::move(refused));
        return;
    }
    if (!client.attachConsentApproval)
        return;
    // The approver's answer was checked against its authority when given,
    // and stands: it may leave, or lose that authority, before the approval
    // is delivered. What is checked again is that the agent and the pane are
    // still the ones the person allowed.
    const Client::AttachConsentApproval& held = *client.attachConsentApproval;
    const auto* requesterAttachment = connection.attachment();
    const Session* target = findSession(question.sessionID);
    const auto nowUnix = static_cast<std::uint64_t>(::time(nullptr));
    const auto isExpired = [nowUnix](const Client& owner) { return owner.hasToken && owner.tokenExpiryUnix && owner.tokenExpiryUnix <= nowUnix; };
    const bool isValid = held.question == question && client.helloSeen && !client.didOverflow && !client.sessionExitDeadlineMilliseconds
        && requesterAttachment && requesterAttachment->attachmentID == held.requesterAttachment
        && requesterAttachment->admission == AttachmentAdmission::AwaitingConsent
        && principalOf(client) == held.requesterPrincipal && client.tokenScope == held.requesterScopes
        && tokenScopeAllows(client, Protocol::Sync::Scope::Observe) && !isExpired(client)
        && target && target->screenSource && target->screenSource->paneID() == held.targetPane && held.targetPane == client.consentPaneID;
    if (!isValid) {
        client.attachConsentApproval.reset();
        client.attachConsentTerminal = ConsentStatusPhase::Denied;
        return;
    }
    const ConnectionAuthority authority { .principal = principalOf(client), .mayDrive = tokenScopeAllows(client, Protocol::Sync::Scope::Drive), .verifiedKind = isAgentClient(client) ? ClientKind::Agent : ClientKind::Human };
    if (connection.deliverConsentApproval(question, authority, now, client.consentDeadlineMilliseconds))
        grantAgentAttach(index);
}

void Daemon::grantAgentAttach(std::size_t index)
{
    if (index >= m_clients.size() || !m_clients[index].attachConsentApproval || !m_clients[index].syncConnection
        || m_clients[index].syncConnection->state() != Protocol::Sync::ConnectionState::Attached)
        return;
    const auto held = std::move(*m_clients[index].attachConsentApproval);
    m_clients[index].attachConsentApproval.reset();
    const uint32_t sessionID = m_clients[index].consentSessionID;
    retireClientConsent(m_clients[index]);
    m_clients[index].consentPending = false;
    Session* s = findSession(sessionID);
    if (!s) {
        // Promotion and target validation share one owner-thread turn. A
        // vanished target must not create an unbound, resumable attachment.
        refuseClient(index, Protocol::Sync::ByeReason::SessionGone);
        return;
    }
    // Persistence follows the exact status/promotion transaction; it cannot
    // create standing authority while a revoked approval waits for output,
    // since a revocation voids the approval. The approver may have gone: the
    // history names who granted it by the identity kept with the approval.
    const bool persistedAlways = held.decision == Protocol::Sync::ConsentChoice::AllowAlways && persistAlwaysAllowedAgent(index);
    if (persistedAlways)
        recordManagementCreation("grant:attach:" + grantKeyFor(m_clients[index]), held.approverIdentity);
    if (held.decision == Protocol::Sync::ConsentChoice::AllowSession
        || (held.decision == Protocol::Sync::ConsentChoice::AllowAlways && !persistedAlways)) {
        // Adds this agent's grant beside any the session already holds; the
        // creator's and other approved agents' grants are untouched.
        const std::string key = grantKeyFor(m_clients[index]);
        if (s->addSessionGrant(key))
            recordManagementCreation("grant:session:" + std::to_string(s->id) + ":" + key, held.approverIdentity);
    }
    m_clients[index].sessionID = sessionID;
    // Approval can promote without another inbound frame. Record its token
    // ceiling before that newly attached connection can be retained or used.
    noteAttachmentGrant(m_clients[index]);
    Ubiquitty::Session::Event granted;
    granted.kind = Ubiquitty::Session::EventKind::ConsentGranted;
    granted.sessionID = sessionID;
    granted.text = m_clients[index].agentLabel;
    granted.detail = "attach";
    recordEvent(std::move(granted));
    seedInitialAttachState(index, *s);
}

void Daemon::denyAgentAttach(std::size_t index, Protocol::Sync::ConsentStatusPhase reason)
{
    if (index >= m_clients.size())
        return;
    if (m_clients[index].consentAction == Client::ConsentAction::Terminate) {
        completeTerminationCall(index, reason == Protocol::Sync::ConsentStatusPhase::Expired ? "timed-out" : "denied");
        return;
    }
    Client& client = m_clients[index];
    client.attachConsentApproval.reset();
    client.attachConsentTerminal = reason == Protocol::Sync::ConsentStatusPhase::Expired
        ? Protocol::Sync::ConsentStatusPhase::Expired
        : Protocol::Sync::ConsentStatusPhase::Denied;
    if (!client.syncConnection || client.syncConnection->state() == Protocol::Sync::ConnectionState::Greeting) {
        // No Greet means there is no welcomed identity to resolve or resume.
        // Closing also releases the bounded pending slot without replay.
        dropClient(index);
        return;
    }
    servicePendingAttachConsent(index);
}

std::string Daemon::consentStorePath() const
{
    std::string path = m_tokenPath.empty() ? m_socketPath : m_tokenPath;
    const auto slash = path.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : path.substr(0, slash)) + "/consent";
}

bool Daemon::loadAlwaysAllowedAgents()
{
    const auto path = consentStorePath();
    auto list = readConsentList(path);
    m_isConsentStoreWhole = list.isWhole;
    if (!list.isWhole) {
        queueStandingConsentWithdrawals(m_alwaysAllowedAgents);
        m_alwaysAllowedAgents.clear();
        if (list.readError)
            Ubiquitty::PAL::logLine(stderr, "daemon: refusing unreadable consent store {} (I/O error {})", path, list.readError);
        else
            Ubiquitty::PAL::logLine(stderr, "daemon: consent store {} has an unreadable layout or exceeds its bounds", path);
        return false;
    }
    adoptStandingConsentEntries(std::move(list.entries));
    return true;
}

void Daemon::adoptStandingConsentEntries(std::vector<std::string> entries)
{
    // Path-keyed entries never become authority for a replacement binary.
    std::erase_if(entries, [](const auto& entry) { return !entry.starts_with("token:"); });
    if (entries.size() > maxAlwaysAllowedAgents)
        entries.resize(maxAlwaysAllowedAgents);
    std::vector<std::string> withdrawn;
    for (const auto& identity : m_alwaysAllowedAgents)
        if (std::ranges::find(entries, identity) == entries.end())
            withdrawn.push_back(identity);
    queueStandingConsentWithdrawals(withdrawn);
    m_alwaysAllowedAgents = std::move(entries);
    m_isConsentStoreWhole = true;
}

void Daemon::queueStandingConsentWithdrawals(std::span<const std::string> identities)
{
    for (const auto& identity : identities) {
        if (!identity.starts_with("token:"))
            continue;
        if (std::ranges::find(m_withdrawnStandingConsentPrincipals, identity) == m_withdrawnStandingConsentPrincipals.end())
            m_withdrawnStandingConsentPrincipals.push_back(identity);
        for (const auto& session : m_sessions) {
            if (session->hasSessionGrantFor(identity))
                continue;
            for (const auto attachmentID : agentAttachmentsForSession(session->id, identity))
                if (std::ranges::find(m_standingConsentRetirements, attachmentID) == m_standingConsentRetirements.end())
                    m_standingConsentRetirements.push_back(attachmentID);
        }
    }
}

void Daemon::retireStandingConsentWithdrawals()
{
    const auto retiring = std::exchange(m_standingConsentRetirements, {});
    const auto principals = std::exchange(m_withdrawnStandingConsentPrincipals, {});
    for (const auto& identity : principals) {
        for (std::size_t index = m_clients.size(); index-- > 0;) {
            const auto& client = m_clients[index];
            if (client.consentPending && client.consentAction == Client::ConsentAction::Attach
                && isAgentClient(client) && grantKeyFor(client) == identity)
                refuseClient(index, Protocol::Sync::ByeReason::TokenRevoked);
        }
    }
    for (const auto attachmentID : retiring) {
        for (std::size_t index = m_clients.size(); index-- > 0;) {
            const auto* record = m_clients[index].syncConnection ? m_clients[index].syncConnection->attachment() : nullptr;
            if (record && record->attachmentID == attachmentID)
                refuseClient(index, Protocol::Sync::ByeReason::TokenRevoked);
        }
        retireAttachmentGrant(attachmentID);
    }
}

std::string Daemon::capabilityGrantsPath() const
{
    const auto slash = m_socketPath.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : m_socketPath.substr(0, slash)) + "/capability_grants";
}

void Daemon::loadAlwaysAllowedCapabilityGrants()
{
    m_capabilityGrantFile->bind(capabilityGrantsPath(), m_grantStore);
    const auto contents = m_capabilityGrantFile->reload(m_grantStore);
    if (contents.isWhole)
        return;
    if (contents.hasUnreadableLayout)
        Ubiquitty::PAL::logLine(stderr, "daemon: {} is not in this host's layout — refusing it whole", m_capabilityGrantFile->path());
    else
        Ubiquitty::PAL::logLine(stderr, "daemon: refusing capability grants {} that could not be read whole (I/O error {})", m_capabilityGrantFile->path(), contents.readError);
}

bool Daemon::editAlwaysAllowedCapabilityGrants(CapabilityGrantOperation operation, const CapabilityGrantKey& key)
{
    // A daemon that has not loaded the file binds it at its first edit, so
    // what it commits is confirmed like every other Always grant.
    if (auto path = capabilityGrantsPath(); m_capabilityGrantFile->path() != path)
        m_capabilityGrantFile->bind(std::move(path), m_grantStore);
    const auto outcome = m_capabilityGrantFile->edit(m_grantStore, operation, key);
    return outcome == ConsentEdit::Changed || outcome == ConsentEdit::Unchanged;
}

void Daemon::replaceCapabilityGrants(Ubiquitty::CapabilityGrantStore&& grants)
{
    const auto source = m_grantStore.standingGrantSource();
    m_grantStore = std::move(grants);
    m_grantStore.setStandingGrantSource(source);
    m_capabilityGrantFile->invalidate();
}

#ifdef UBIQUITTY_QUIC
bool Daemon::startQUIC(std::uint16_t port)
{
    auto listener = std::make_unique<QUICListener>();
    // A predecessor's socket keeps the port, and every client's pin, it had.
    const bool isStarted = m_inheritedQUICSocket ? listener->startOnBoundSocket(std::move(m_inheritedQUICSocket)) : listener->start(port);
    if (!isStarted) {
        std::fprintf(stderr, "ubiquitty-host: QUIC listen failed: %s\n", listener->lastError().c_str());
        return false;
    }
    listener->setStreamDataCallback([this](QUICConnection* connection, std::span<const std::uint8_t> bytes) { onQUICStreamData(connection, bytes); });
    listener->setConnectionClosedCallback([this](QUICConnection* connection) { onQUICConnectionClosed(connection); });
    listener->setDatagramCallback([this](QUICConnection* connection, std::span<const std::uint8_t> bytes) { onQUICDatagram(connection, bytes); });
    m_quic = std::move(listener);
    return true;
}

std::size_t Daemon::findOrCreateQUICClient(QUICConnection* connection)
{
    for (std::size_t i = 0; i < m_clients.size(); ++i)
        if (m_clients[i].quicConnection == connection)
            return i;
    if (m_clients.size() >= maxClients || m_nextClientID == 0)
        return m_clients.size(); // at the client cap, or out of client IDs — refuse
    Client c;
    c.quicConnection = connection;
    c.clientID = m_nextClientID++;
    c.connectMilliseconds = nowMilliseconds();
    c.attachedUnix = static_cast<std::uint64_t>(::time(nullptr));
    m_clients.push_back(std::move(c));
    return m_clients.size() - 1;
}

void Daemon::feedQUICClientFrames(QUICConnection* connection, std::size_t index, std::span<const std::uint8_t> data)
{
    if (index >= m_clients.size())
        return;
    // Inbound access gate: mirrors serviceClient's TCP-side gate — a
    // client already marked `didOverflow` this pass (kicked/dropped by another
    // connection's callback earlier in the same QUIC poll) must not still
    // have its bytes fed/dispatched before the next top-of-loop reap.
    if (m_clients[index].didOverflow)
        return;
    if (m_clients[index].quicConnection == connection)
        feedClientFrames(index, std::as_bytes(data));
}

void Daemon::onQUICStreamData(QUICConnection* connection, std::span<const std::uint8_t> data)
{
    std::size_t index = findOrCreateQUICClient(connection);
    if (index >= m_clients.size())
        return; // client cap reached — drop the bytes
    // Inbound access gate: an already-kicked/dropped client (`didOverflow`
    // this same pass) must not have its pre-auth buffer or token state touched
    // either, not just its post-auth frames.
    if (m_clients[index].didOverflow)
        return;

    // Auth gate: a QUIC connection must lead with a valid one-time token,
    // framed [u8 length][token], before any byte is treated as a Sync frame.
    if (!m_clients[index].quicAuthed) {
        // Pre-auth remote-OOM defense (no auth needed to trigger): only the
        // preamble's own bytes are ever held before it checks out, so what an
        // unauthenticated peer can make the daemon buffer is at most
        // Sync::maxPreambleBytes however much it sends. An unknown kind, or
        // a token of no length or one past the cap, is malformed and drops
        // the peer with nothing kept. Bytes past the preamble in the same
        // chunk — a client's first frames, sent with it — wait in this call
        // and reach the decoder only once the preamble has been consumed.
        Protocol::Sync::PreambleReader& reader = m_clients[index].quicPreamble;
        const std::span<const std::uint8_t> afterPreamble = data.subspan(reader.feed(data));
        if (reader.status() == Protocol::Sync::PreambleReader::Status::Malformed) {
            dropClient(index);
            return;
        }
        const std::optional<Protocol::Sync::Preamble> preamble = reader.take();
        if (!preamble)
            return; // preamble incomplete — await more bytes
        Protocol::Sync::ScopeSet carriedScope;
        std::string carriedTokenID;
        bool didCarryInteractiveUser = false;
        if (const auto* token = std::get_if<Protocol::Sync::TokenPreamble>(&*preamble)) {
            if (!consumeQUICToken(token->token, carriedScope, carriedTokenID, &didCarryInteractiveUser)) {
                dropClient(index); // bad or already-used token → close
                return;
            }
            m_clients[index].hasToken = true;
            m_clients[index].tokenScope = carriedScope;
            m_clients[index].tokenID = std::move(carriedTokenID);
            m_clients[index].hasBootstrapToken = true;
            m_clients[index].tokenExpiryUnix = 0;
            m_clients[index].isInteractiveUser = didCarryInteractiveUser;
        } else {
            // A resume proof: a MAC under the attachment's resume secret over
            // keying material only this handshake's two ends can export, so it
            // proves the secret on this connection and replays on no other.
            // It attaches nothing by itself: the connection is admitted for
            // that attachment's `Greet` alone, and is served under what the
            // client that created the attachment was granted.
            const Protocol::Sync::ResumeProof& proof = std::get<Protocol::Sync::ResumeProof>(*preamble);
            const std::optional<std::array<std::uint8_t, 32>> exporter = m_quic ? m_quic->exportKeyingMaterial(connection, Protocol::Sync::resumeExporterLabel) : std::nullopt;
            if (!exporter || !admitAttachmentProof(index, proof, *exporter)) {
                // Every refused proof gets the same close reason: the peer
                // learns no attachment or MAC validation detail.
                m_clients[index].quicCloseReason = Protocol::Sync::ByeReason::Malformed;
                dropClient(index);
                return;
            }
            // An earlier proof's connection that never greeted gives way.
            closeSupersededConnections();
            if (index >= m_clients.size() || m_clients[index].didOverflow)
                return;
        }
        m_clients[index].quicAuthed = true;
        // Off the pre-auth leash: the connection may now use the full idle window.
        if (m_quic)
            m_quic->markAuthenticated(m_clients[index].quicConnection);
        // Bytes past the preamble begin the Sync stream.
        if (!afterPreamble.empty())
            feedQUICClientFrames(connection, index, afterPreamble);
        return;
    }

    feedQUICClientFrames(connection, index, data);
}

void Daemon::onQUICDatagram(QUICConnection* connection, std::span<const std::uint8_t> datagram)
{
    // Only an authenticated client's screen plane takes datagrams: the
    // carrier is the connection's, but what a datagram may say is the screen
    // connection's, which exists only past its HELLO.
    for (std::size_t i = 0; i < m_clients.size(); ++i) {
        Client& client = m_clients[i];
        if (client.quicConnection != connection)
            continue;
        if (client.quicAuthed && client.syncConnection)
            client.syncConnection->receiveDatagram(datagram, nowMicroseconds());
        return;
    }
}

void Daemon::onQUICConnectionClosed(QUICConnection* connection)
{
    for (std::size_t i = 0; i < m_clients.size(); ++i)
        if (m_clients[i].quicConnection == connection) {
            // The listener frees `connection` after this returns; null it before
            // the deferred drop so nothing dereferences it in the reap window.
            m_clients[i].quicConnection = nullptr;
            dropClient(i);
            return;
        }
}
#else
bool Daemon::startQUIC(std::uint16_t) { return false; } // not built with -DUBIQUITTY_QUIC=ON
#endif

// PTY <-> Terminal.

void Daemon::drainTermToPTY(Session& s)
{
    if (s.shellExited || !s.stream)
        return;
    OSC::RichTextBlocks::publishMeasurements(*s.term);
    if (!s.term->outputLength && !s.term->bulkOutput.length) {
        s.isReplyWriteRefused = false;
        return;
    }
    // An ordinary terminal reply can flush the same output queue as owned
    // input. Retire expired authority before that incidental drain too. A
    // drain with nothing to write flushes nothing: expiry is then the run
    // loop's sweep, and a call already read is answered under its checks.
    dropExpiredTokenClients();
    reapAttachments(nowMicroseconds());
    if (s.shellExited || !s.stream)
        return;
    // DSR/DA and other terminal reports the Core wants to send upstream get
    // written back to the PTY master. They go through the same queue as the
    // clients' input, so a reply lands behind a paste the child has not read
    // yet rather than inside it, and a reply the master cannot take now waits
    // where the run loop's POLLOUT drains it rather than for the program's
    // next output.
    struct ReplyDelivery {
        PAL::Stream& stream;
        int status = 0;
    };
    ReplyDelivery delivery { .stream = *s.stream, .status = 0 };
    for (std::size_t round = 0; round <= RichText::maxPendingMeasurements; ++round) {
        OSC::RichTextBlocks::publishMeasurements(*s.term);
        if (!s.term->outputLength && !s.term->bulkOutput.length)
            break;
        Ubiquitty::flushOutput(*s.term, [](void* context, std::span<const char> bytes) {
            auto& target = *static_cast<ReplyDelivery*>(context);
            target.status = target.stream.writeAll(bytes);
            return target.status >= 0; }, &delivery);
        if (s.term->outputLength || s.term->bulkOutput.length)
            break;
    }
    s.isReplyWriteRefused = delivery.status < 0;
    // A refused write queues nothing for a child that is gone. A background
    // job can hold the replica open, so no hangup ends the session: the write
    // that saw the child go ends it, as the input paths do.
    if (delivery.status == std::to_underlying(PAL::StreamStatus::Closed) || delivery.status == std::to_underlying(PAL::StreamStatus::Fatal)) {
        s.exitReason = delivery.status == std::to_underlying(PAL::StreamStatus::Closed) ? "pty write closed (child gone) during a terminal reply" : "pty write fatal during a terminal reply";
        s.shellExited = true;
    }
}

void Daemon::refreshRunFaces(Session& s)
{
    if (!s.term)
        return;
    // Only a pane that draws runs needs faces. Driving viewers contribute
    // their own; a viewer or agent cannot decide the program's layout.
    if (Ubiquitty::OSC::TextSizing::mayOfferFaceFacts(*s.term)) {
        static_assert(maxClients <= Ubiquitty::OSC::TextSizing::PublishedFacesMeasurer::maxOffered, "every client's faces can be offered");
        static_assert(maxSessions == Ubiquitty::OSC::TextSizing::maxFaceOffersPerPane, "the retained envelope bound covers every session");
        std::array<std::shared_ptr<const Ubiquitty::OSC::TextSizing::ProportionalFaceFacts>, Ubiquitty::OSC::TextSizing::PublishedFacesMeasurer::maxOffered> faces {};
        std::size_t count = 0;
        bool areFacesOwed = false;
        for (const auto& c : m_clients) {
            if (!c.subscriptionFor(s.id) || c.didOverflow || isAgentClient(c) || !tokenScopeAllows(c, Protocol::Sync::Scope::Drive)
                || !c.syncConnection)
                continue;
            if (!c.syncConnection->publishesFaces())
                continue;
            if (c.syncConnection->proportionalFaces()) {
                if (count < faces.size())
                    faces[count++] = c.syncConnection->proportionalFaces();
            } else {
                areFacesOwed = true;
            }
        }
        if (s.screenSource && count < faces.size()) {
            std::array<const Protocol::Sync::AttachmentRecord*, Ubiquitty::OSC::TextSizing::PublishedFacesMeasurer::maxOffered> inherited {};
            const std::size_t offered = m_attachments->inheritedFaceOffers(s.screenSource->paneID(), std::span(inherited).first(faces.size() - count));
            for (const auto* record : std::span(inherited).first(offered))
                faces[count++] = record->inheritedProportionalFaces;
        }
        s.runFaces.setFaces(std::span(faces).first(count), s.term->ambiguousWidth);
        s.runFaces.notePublicationState(areFacesOwed ? Ubiquitty::OSC::TextSizing::PublishedFacesMeasurer::FacePublicationState::Owed
                                                     : Ubiquitty::OSC::TextSizing::PublishedFacesMeasurer::FacePublicationState::Ready,
            nowMilliseconds());
    } else {
        s.runFaces.setFaces({}, s.term->ambiguousWidth);
        s.runFaces.notePublicationState(Ubiquitty::OSC::TextSizing::PublishedFacesMeasurer::FacePublicationState::Ready, nowMilliseconds());
    }
    if (s.runFaces.releaseHeld(*s.term, nowMilliseconds()))
        drainTermToPTY(s);
}

Runtime::DrainPassResult Daemon::servicePTY(Session& s, int maxBytes)
{
    s.clock.tick(Ticks::fromMilliseconds(m_monotonicMicroseconds() / 1000));
    // Changed client offers choose again before their next service pass, and
    // a departure chooses before the client goes. A new pane and one with
    // measurements to route are revalidated here, before the bytes this pass
    // reads can ask for another.
    if (s.isRichTextMeasurerStale || !s.term->richTextMeasurements.empty() || s.term->typography.needsReport) {
        refreshRichTextMeasurer(s);
        OSC::RichTextBlocks::expireMeasurements(*s.term, s.clock.now());
    }
    if (s.term->outputLength || s.term->bulkOutput.length)
        drainTermToPTY(s);
    // Stamp the OUTPUT-side capability origin as the most-restrictive origin among
    // the session's attached DRIVERS — clients whose token scope permits Drive
    // (local/no-token clients are ungated drivers). A side effect the program emits
    // autonomously is gated at the least-trusted principal able to INJECT into the
    // session; a passive observe-only viewer (e.g. the owner's 2nd device watching
    // their own session) cannot drive, so it does not escalate the origin and never
    // suppresses the local user's own program reports. Never defaults a remote-driven
    // session to Local; a detached session keeps its last-stamped origin.
    {
        auto strictest = std::to_underlying(Ubiquitty::CapabilityOrigin::Local);
        bool anyDriver = false;
        // Every subscribed client that may drive raises the output origin, whether
        // or not its attachment is live: one between attachments can drive again
        // the moment it resumes. A connection admitted as a viewer cannot inject
        // input, so it raises nothing.
        for (const auto& c : m_clients)
            if (c.subscriptionFor(s.id) && !c.didOverflow && tokenScopeAllows(c, Protocol::Sync::Scope::Drive)
                && (!c.syncConnection || c.syncConnection->mayDrive())) {
                anyDriver = true;
                strictest = std::max(strictest, std::to_underlying(capabilityOriginOf(c)));
            }
        if (anyDriver) {
            s.outputOrigin = static_cast<Ubiquitty::CapabilityOrigin>(strictest);
            // A currently-attached higher-origin driver ratchets the sticky floor so a
            // later detach can't downgrade the gate under its backgrounded output.
            s.driverOriginFloor = std::max(s.driverOriginFloor, s.outputOrigin);
        }
        // Background work outlives its input sender and its connection.
        // An idle attachment establishes no output grant identity.
        if (s.hasMultipleInputPrincipals && !s.capabilityPolicy.currentIdentity().empty())
            s.capabilityPolicy.setCurrentIdentity({});
        // Never below the sticky floor: a detached Agent/Remote driver's backgrounded
        // output keeps gating at (at least) its origin instead of downgrading to a
        // remaining Local attendee — the detach-race TOCTOU.
        if (std::to_underlying(s.outputOrigin) < std::to_underlying(s.driverOriginFloor))
            s.outputOrigin = s.driverOriginFloor;
        if (s.term)
            s.term->currentCapabilityOrigin = s.outputOrigin;
    }
    // The clipboard writes and notifications this pass makes are bound, at the
    // delivery below, to the recipient chosen now: nothing between here and
    // there changes who typed last.
    const std::size_t recipient = primaryHumanForSession(s);
    s.capabilityPolicy.setEffectQuestion(!s.hasMultipleInputPrincipals && recipient < m_clients.size() && asksAboutHostEffects(m_clients[recipient])
            ? HostCapabilityPolicy::EffectQuestion::AskedByRecipient
            : HostCapabilityPolicy::EffectQuestion::AskedByHost);
    refreshRunFaces(s);
    // The pass is the one every frontend drains by (Runtime::runDrainPass): it
    // reads on past a short read to a would-block or its byte budget and stops
    // the parser at each 2026 close. The budget matters here more than
    // anywhere: without it a flooding program holds the single-threaded poll
    // loop and every other session and client on this daemon is served
    // nothing. Yielding costs nothing, because the fd stays readable and the
    // next poll returns it at once — the bytes are not dropped, only the turn
    // is.
    //
    // The pass ends at its first would-block; it does not ride the writer's
    // refill gaps. A ride's budget is per gap and starts over after every read,
    // so a writer whose gaps fit inside it — a shell loop printing a byte every
    // few microseconds — keeps one pass alive to the byte budget, seconds of
    // a loop that serves every other session's PTY, every client's input and
    // every timer. A redraw split across a refill gap is still one frame: the
    // coherence rule's read quiescence merges it.
    struct Owner final : Runtime::DrainPassOwner {
        Owner(Daemon& owner, Session& owned)
            : daemon(owner)
            , session(owned)
        {
        }
        // The bytes in hand are parsed at the instant they were read.
        int judgeRead(std::span<const char>) override
        {
            session.clock.tick(Ticks::fromMilliseconds(daemon.m_monotonicMicroseconds() / 1000));
            return -1;
        }
        void flushReplies() override { daemon.drainTermToPTY(session); }
        // A 2026 close is read off the terminal's own counter by the rule.
        void noteSyncClear() override { }
        // The daemon takes its frames at the pass end, from the publish loop,
        // never inside a pass: a mid-read close is classed there as a
        // boundary.
        void publishMidReadSyncFrame(Runtime::DrainHost&) override { }
        // A read that finds nothing settles the input written at least E
        // before it began, so the clock is read before the read, never after.
        void noteReadBegin() override { readBegan = daemon.m_monotonicMicroseconds(); }
        Daemon& daemon;
        Session& session;
        std::uint64_t readBegan = 0;
    };
    // The daemon's model has no other thread to lock against, and its frames
    // are taken by the publish loop, so the host hears no frame.
    struct Host final : Runtime::DrainHost {
        void lockModel() override { }
        void unlockModel() override { }
        void onSyncFrameReadyLocked(Runtime::SyncFrameReason) override { }
    };
    Owner owner(*this, s);
    Host host;
    const Runtime::DrainPassResult pass = Runtime::runDrainPass(*s.term, *s.stream, host, owner,
        { .doesRideBurst = false, .modelLockBiteBytes = Runtime::TerminalRuntime::modelLockBiteBytes, .maxBytes = maxBytes });
    s.capabilityPolicy.setEffectQuestion(HostCapabilityPolicy::EffectQuestion::AskedByHost);
    const bool gotData = pass.bytesDrained > 0;
    // A title's owner is sampled after its bytes, before a deferred foreground
    // probe could attribute a short-lived program's report to the next shell.
    if (s.term->titleChanged && s.term->titleOwnerProcessGroup == 0)
        (void)Ubiquitty::retireTitleIfStale(*s.term, s.stream->process().foregroundProcessGroup().value_or(0));
    if (pass.didCatchUp && s.screenSource)
        s.screenSource->noteReadWouldBlock(owner.readBegan);
    if (pass.didReachEnd) {
        s.exitReason = pass.isFatal                                              ? "pty read fatal"
            : pass.endStatus == std::to_underlying(PAL::StreamStatus::EndOfFile) ? "pty read end of file"
                                                                                 : "pty read closed";
        s.shellExited = true;
    }
    // The program's output just parsed above may have pushed capabilities
    // to Prompt (recorded in m_consentQueue by the session policy's check()). Emit
    // one requested consent Notify per NEW (capability, origin, identity) to this session's
    // human (presence-capable) clients so they can grant. The chokepoint already
    // denied it (fail-closed); a grant flips future checks to Allow. The grant is
    // keyed by the requestID the daemon issues here, so a response can only approve
    // what the daemon actually asked (never an attacker-chosen capability). Entries
    // stay pending until the human rules — no duplicate ask for the same triple.
    sendCapabilityGrantRequests(s);
    recordProgramStatusEvents(s);
    if (gotData) {
        // Stale-mouse guard — the same policy as TerminalRuntime::drainAvailable:
        // mouse tracking left on by a foreground job that died without DECRST
        // (ssh dropped mid-tmux) gets cleared once the shell is back in the
        // foreground under a different group. isFrameDue below ships the
        // changed inputModes bits to every attached client.
        const bool isMouseTracking = Ubiquitty::Mouse::tracksPointer(s.term.get()) || s.term->modes.mouseRelativePointerLock;
        if (!isMouseTracking)
            s.mouseTrackingProcessGroup = 0;
        else {
            PAL::ProcessFacts& process = s.stream->process();
            const std::optional<PAL::ProcessID> foregroundGroup = process.foregroundProcessGroup();
            (void)Ubiquitty::Mouse::retireTrackingIfStale(*s.term, s.mouseTrackingProcessGroup, foregroundGroup.value_or(0), process.foregroundIsShell());
        }
        // The pass boundary is the only instant the rule can act on, so it is
        // where the rule samples the terminal for a bracket and starts counting
        // quiescence.
        const std::uint64_t passEnd = m_monotonicMicroseconds();
        s.coherence.noteDrainPass(*s.term, passEnd,
            pass.didCatchUp ? Runtime::PassEnd::WouldBlock : Runtime::PassEnd::Capacity);
        s.census.noteDirty(passEnd);
        s.activity.noteOutput(m_wallClockMilliseconds());
        drainTermToPTY(s);
        s.isFrameDue = true;
    }
    deliverSessionEffects(s);
    return pass;
}

std::optional<Protocol::Sync::HandoverRefuseReason> Daemon::handoverBlocker() const
{
    using Reason = Protocol::Sync::HandoverRefuseReason;
    // Local operation evidence belongs to this daemon incarnation. It must
    // not disappear in a host transfer that has not carried this ledger.
    if (m_pendingLocalAdopt || (m_localAdoptTransactions && m_localAdoptTransactions->outcomeCount()))
        return Reason::AdoptPending;
    if (!m_structureConsents.empty())
        return Reason::ApprovalPending;
    // Every retained question belongs to the current recipients and serving
    // incarnation; none is part of the handover ledger, not just clipboard.
    if (!m_pendingCapabilityGrants.empty())
        return Reason::GrantPending;
    for (const Client& client : m_clients) {
        if (client.pendingAdoptFileDescriptor || (client.localAdopt && (client.localAdopt->pendingBytes() || client.localAdopt->retainedBytes()))
            || (client.localHandoffCorrelation && !client.isHandoffComplete))
            return Reason::AdoptPending;
#ifdef UBIQUITTY_QUIC
        // A carrier handoff clears outputBuffer before QUIC transmits or
        // acknowledges it. Those bytes can still contain a call's only reply.
        if (client.quicConnection && (!m_quic || m_quic->pendingBytes(client.quicConnection)))
            return Reason::OutputUnsent;
#endif
        // Unclassified bytes may include a completed call's only reply. They
        // must leave the physical carrier before it can be replaced; screen
        // state still owned by a Sync connection can be regenerated on resume.
        if (client.hasPendingOutput() || !client.pendingConsentRetirements.empty())
            return Reason::OutputUnsent;
        if (client.isReadingFrames || client.localControl.hasPendingRequest()
            || (client.localClipboardCorrelation && client.localClipboard.hasPendingRequest())
            || client.consentPending || client.terminationCall || client.reclaimCall || !client.pendingClipboardRequests.empty()
            || client.clipboardToolRequestID || client.clipboardToolDeadlineMilliseconds
            || (client.syncConnection && client.syncConnection->hasHandoverBlockedOperations()))
            return Reason::RequestPending;
    }
    return std::nullopt;
}

bool Daemon::beginHandoverQuiesce(std::uint64_t now)
{
    if (m_quiescingPanes || hasHandoverBlockedOperations() || std::ranges::any_of(m_sessions, [](const auto& session) {
            return !session->term || !session->stream || !session->screenSource || session->shellExited;
        }))
        return false;
    // Explicit safety bounds, not a measured handover latency promise. A
    // continuously readable flood may refuse rather than inventing quiet.
    constexpr Runtime::QuiesceTuning tuning { 100'000, 100'000, 64 * 1024 };
    m_quiescingPanes.emplace();
    m_quiescingPanes->reserve(m_sessions.size());
    m_didHandoverQuiesceFail = false;
    m_handoverQuiesceBlocker.reset();
    for (auto& session : m_sessions) {
        m_quiescingPanes->push_back({ session->id, Runtime::HandoverQuiesce(tuning, now) });
        // Even an idle pane needs a fresh classification of its exact state,
        // not the class of a frame published before this attempt.
        session->isFrameDue = true;
    }
    return true;
}

bool Daemon::isQuiesced() const
{
    return m_quiescingPanes && !hasHandoverBlockedOperations() && m_quiescingPanes->size() == m_sessions.size()
        && std::ranges::all_of(*m_quiescingPanes, [&](const auto& pane) {
               const auto found = std::ranges::find_if(m_sessions, [&](const auto& session) { return session->id == pane.sessionID; });
               return pane.controller.phase() == Runtime::QuiescePhase::Ready && found != m_sessions.end()
                   && !(*found)->isFrameDue && !(*found)->shellExited && (*found)->screenSource
                   && (*found)->screenSource->isFrameHeld() && (*found)->screenSource->isEncodable()
                   && (*found)->screenSource->generation() == pane.heldGeneration;
           });
}

void Daemon::endHandoverQuiesce()
{
    if (!m_quiescingPanes)
        return;
    m_quiescingPanes.reset();
    for (auto& session : m_sessions)
        session->isFrameDue = true;
}

void Daemon::serviceHandoverQuiesce(std::uint64_t now)
{
    if (!m_quiescingPanes)
        return;
    const auto fail = [&] {
        endHandoverQuiesce();
        m_didHandoverQuiesceFail = true;
    };
    if (const auto blocker = handoverBlocker()) {
        m_handoverQuiesceBlocker = blocker;
        fail();
        return;
    }
    for (auto& pane : *m_quiescingPanes) {
        now = std::max(now, nowMicroseconds());
        Session* session = findSession(pane.sessionID);
        if (!session || !session->term || !session->stream || session->shellExited) {
            fail();
            return;
        }
        auto& controller = pane.controller;
        if (controller.phase() == Runtime::QuiescePhase::Ready) {
            // Cross-pane authority expiry can dirty a frozen pane's focus.
            // Refuse that attempt rather than snapshotting an older boundary.
            if (session->isFrameDue) {
                fail();
                return;
            }
            continue;
        }
        controller.notePass(*session->term, std::nullopt, now);
        if (controller.phase() == Runtime::QuiescePhase::Failed) {
            fail();
            return;
        }
        drainRemoteInput(*session);
        now = std::max(now, nowMicroseconds());
        controller.notePass(*session->term, std::nullopt, now);
        if (controller.phase() == Runtime::QuiescePhase::Failed) {
            fail();
            return;
        }
        std::optional<Runtime::DrainPassResult> pass;
        if (controller.phase() == Runtime::QuiescePhase::Natural)
            pass = servicePTY(*session);
        else if (controller.takeGroundReadAllowance(*session->term, now)) {
            pass = servicePTY(*session, 1);
            now = std::max(now, nowMicroseconds());
            controller.completeGroundRead(static_cast<std::size_t>(pass->bytesDrained), now);
        }
        now = std::max(now, nowMicroseconds());
        if (pass && pass->bytesDrained)
            pane.isReadQuietRecorded = false;
        // A bounded one-byte read cannot consume a second byte merely to
        // discover EAGAIN. Only the real descriptor's quiet poll can supply
        // that observation; stopping reads by itself never counts as quiet.
        const bool didObserveQuiet = (pass && pass->didCatchUp)
            || (controller.phase() == Runtime::QuiescePhase::ReadsFrozen
                && session->stream->fileDescriptor() >= 0 && ptyReadiness(session->stream.get()) == Runtime::ReadReadiness::Quiet);
        if (didObserveQuiet && !pane.isReadQuietRecorded) {
            session->coherence.noteDrainPass(*session->term, now, Runtime::PassEnd::WouldBlock);
            pane.isReadQuietRecorded = true;
        }
        if (session->shellExited) {
            fail();
            return;
        }
        const auto frameClass = mayPublishFrame(*session, now);
        if (frameClass) {
            publishFrame(*session, *frameClass);
            if (!session->screenSource->isFrameHeld() || !session->screenSource->isEncodable()) {
                fail();
                return;
            }
        }
        now = std::max(now, nowMicroseconds());
        controller.notePass(*session->term, frameClass, now);
        if (controller.phase() == Runtime::QuiescePhase::Failed) {
            fail();
            return;
        }
        if (controller.phase() != Runtime::QuiescePhase::Ready)
            session->isFrameDue = true;
        else
            pane.heldGeneration = session->screenSource->generation();
    }
    // Servicing a later pane can withdraw shared authority and dirty an
    // earlier frozen pane. Check the whole set before exposing readiness.
    for (const auto& pane : *m_quiescingPanes) {
        const Session* session = findSession(pane.sessionID);
        if (!session || (pane.controller.phase() == Runtime::QuiescePhase::Ready && (session->isFrameDue || !session->screenSource || !session->screenSource->isFrameHeld() || !session->screenSource->isEncodable() || session->screenSource->generation() != pane.heldGeneration))) {
            fail();
            return;
        }
    }
}

int Daemon::scrollbackMaintenanceTimeout(std::uint64_t now) const
{
    for (const auto& session : m_sessions) {
        if (session->shellExited || !session->term || !Screen::foldColdScrollback(*session->term, 0)) {
            continue;
        }
        if (ptyReadiness(session->stream.get()) == Runtime::ReadReadiness::Quiet) {
            return wakeAfterMilliseconds(m_nextScrollbackFoldMilliseconds, now);
        }
    }
    return -1;
}

void Daemon::serviceScrollbackMaintenance(std::uint64_t now)
{
    if (now < m_nextScrollbackFoldMilliseconds) {
        return;
    }

    // IDs survive vector erasure and insertion between turns. Select the
    // successor, wrapping to the smallest eligible ID after the last one.
    Session* first = nullptr;
    Session* next = nullptr;
    for (const auto& session : m_sessions) {
        if (session->shellExited || !session->term || !Screen::foldColdScrollback(*session->term, 0)) {
            continue;
        }
        if (ptyReadiness(session->stream.get()) != Runtime::ReadReadiness::Quiet) {
            continue;
        }
        if (!first || session->id < first->id) {
            first = session.get();
        }
        if (session->id > m_lastScrollbackFoldSessionID && (!next || session->id < next->id)) {
            next = session.get();
        }
    }
    if (!next) {
        next = first;
    }
    if (!next) {
        return;
    }

    // The reactor is the sole model writer. Folding changes storage only;
    // it does not dirty a frame or advance the history's wire identity.
    (void)Screen::foldColdScrollback(*next->term, Runtime::scrollbackFoldBlockBudget);
    m_lastScrollbackFoldSessionID = next->id;
    m_nextScrollbackFoldMilliseconds = now + Runtime::scrollbackFoldTickMilliseconds;
}

// Frame publication.

void Daemon::seedInitialAttachState(std::size_t index, Session& session)
{
    if (index >= m_clients.size())
        return;
    auto& client = m_clients[index];
    client.sessionID = session.id;
    auto& workspace = hostedWorkspaceFor(session);
    client.workspaceID = workspace.workspaceID;
    for (const PaneSubscription& subscription : client.paneSubscriptions)
        unbindScreenPane(client, subscription);
    client.paneSubscriptions.clear();
    if (client.intents.contains(Protocol::Sync::HelloIntent::AttachesWorkspace)) {
        client.paneSubscriptions.reserve(workspace.paneSessions.size());
        for (const auto& [pane, sessionID] : workspace.paneSessions) {
            if (findSession(sessionID))
                client.paneSubscriptions.push_back({ .sessionID = sessionID });
        }
    } else
        client.paneSubscriptions.push_back({ .sessionID = session.id });
    for (auto& subscription : client.paneSubscriptions) {
        if (auto* subscribedSession = findSession(subscription.sessionID))
            seedPaneState(*subscribedSession);
    }
    bindScreenPanes(index);
    if (!isAgentClient(client)) {
        const auto now = nowMilliseconds();
        for (auto& pending : m_clients) {
            if (pending.consentPending && pending.consentDeadlineMilliseconds > now
                && client.subscriptionFor(pending.consentSessionID)) {
                sendConsentRequestToHumans(pending, index);
            }
        }
        offerStructureConsents(index);
    }
    flushClient(index);
}

void Daemon::seedPaneState(Session& session)
{
    // All records use the bound pane's paced source. An attach may precede
    // its first frame, so sample the foreground now within the shared budget.
    probeForegroundIfDue(session);
    (void)refreshPaneFacts(session);
    // The pane has a reader from here on: its records are read for it now,
    // however long nobody read them.
    noteRecordsDue(session, nowMilliseconds());
    broadcastPresence(session.id);
}

uint32_t Daemon::HostedWorkspace::sessionFor(Ubiquitty::Session::Layout::PaneID pane) const
{
    for (const auto& [boundPane, sessionID] : paneSessions)
        if (boundPane == pane)
            return sessionID;
    return 0;
}

Ubiquitty::Session::Layout::PaneID Daemon::HostedWorkspace::paneFor(uint32_t sessionID) const
{
    for (const auto& [pane, boundSession] : paneSessions)
        if (boundSession == sessionID)
            return pane;
    return Ubiquitty::Session::Layout::invalidPane;
}

Daemon::HostedWorkspace* Daemon::workspaceHolding(uint32_t sessionID)
{
    for (const auto& workspace : m_workspaces)
        if (workspace->paneFor(sessionID) != Ubiquitty::Session::Layout::invalidPane)
            return workspace.get();
    return nullptr;
}

Protocol::Sync::PanePlacement Daemon::placementOf(uint32_t sessionID) const
{
    for (const auto& workspace : m_workspaces) {
        const Ubiquitty::Session::Layout::PaneID pane = workspace->paneFor(sessionID);
        if (pane == Ubiquitty::Session::Layout::invalidPane)
            continue;
        const Ubiquitty::Session::Layout::SurfaceID surface = workspace->layout.surfaceHolding(pane);
        const Ubiquitty::Panes::Tree* tree = workspace->layout.paneTree(surface, workspace->layout.tabHolding(surface, pane));
        return tree && tree->paneCount() > 1 ? Protocol::Sync::PanePlacement::BesideOthers : Protocol::Sync::PanePlacement::WholeWindow;
    }
    return Protocol::Sync::PanePlacement::WholeWindow;
}

Daemon::HostedWorkspace& Daemon::hostedWorkspaceFor(Session& session)
{
    if (HostedWorkspace* holding = workspaceHolding(session.id))
        return *holding;
    // A fresh Layout::Workspace is the degenerate one surface / one tab / one
    // pane, which is what a session with no container is; the root session
    // draws into that one pane.
    // The root the session was created with, or one minted now for a session
    // that reached the daemon some other way.
    auto workspace = session.placement.pane != Ubiquitty::Session::Layout::invalidPane
        ? std::make_unique<HostedWorkspace>(m_layoutIdentities, session.placement)
        : std::make_unique<HostedWorkspace>(m_layoutIdentities);
    // Session allocation reserves IDs held by live workspace records; the
    // record retains that identity independently of this session's lifetime.
    workspace->workspaceID = session.id;
    workspace->name = session.workspaceName;
    workspace->shouldEndWhenDetached = session.endsWhenDetached;
    const auto pane = workspace->layout.focusedPane(workspace->layout.surfaceIDAt(0));
    workspace->paneSessions.emplace_back(pane, session.id);
    m_workspaces.reserve(m_workspaces.size() + m_preparedLocalAdopts + 1);
    m_workspaces.push_back(std::move(workspace));
    return *m_workspaces.back();
}

void Daemon::admitWorkspacePanes(HostedWorkspace& workspace)
{
    // The workspace's structure says what changed, to every client that
    // reads one, on the pass that changed it.
    m_isStructureDirty = true;
    for (std::size_t index = 0; index < m_clients.size(); ++index) {
        auto& client = m_clients[index];
        // A client on one session is told the structure and bound nothing
        // more: its panes are the one it attached to.
        if (!client.intents.contains(Protocol::Sync::HelloIntent::AttachesWorkspace))
            continue;
        if (client.workspaceID != workspace.workspaceID || client.didOverflow)
            continue;
        // Only authenticated interactive clients carry a whole workspace: an
        // agent is refused where the workspace grows past what it attached
        // to, since a carrier routes panes it would otherwise wait on.
        if (isAgentClient(client)) {
            refuseClient(index, Protocol::Sync::ByeReason::OutsideScope);
            continue;
        }
        const auto previousCount = client.paneSubscriptions.size();
        client.paneSubscriptions.reserve(workspace.paneSessions.size());
        for (const auto& [pane, sessionID] : workspace.paneSessions) {
            if (!client.subscriptionFor(sessionID) && findSession(sessionID))
                client.paneSubscriptions.push_back({ .sessionID = sessionID });
        }
        for (std::size_t paneIndex = previousCount;
            paneIndex < client.paneSubscriptions.size(); ++paneIndex) {
            auto& subscription = client.paneSubscriptions[paneIndex];
            if (auto* session = findSession(subscription.sessionID))
                seedPaneState(*session);
        }
        bindScreenPanes(index);
    }
}

void Daemon::reapVanishedPanes(HostedWorkspace& workspace)
{
    // Bindings go before their sessions end, so an ending session finds no
    // pane of its own to take out of the model.
    std::vector<uint32_t> ended;
    std::erase_if(workspace.paneSessions, [&](const auto& binding) {
        if (workspace.layout.surfaceHolding(binding.first) != Ubiquitty::Session::Layout::invalidSurface)
            return false;
        ended.push_back(binding.second);
        return true;
    });
    for (const uint32_t sessionID : ended)
        destroySession(sessionID);
}

void Daemon::releaseWorkspaceSession(uint32_t sessionID, SessionEnding ending)
{
    // Every session is a leaf. Ending one never terminates its siblings.
    for (const auto& workspace : m_workspaces) {
        const auto pane = workspace->paneFor(sessionID);
        if (pane == Ubiquitty::Session::Layout::invalidPane)
            continue;
        std::erase_if(workspace->paneSessions, [&](const auto& binding) { return binding.second == sessionID; });
        workspace->layout.removePane(pane);
        // A lost pane leaves the record as it was. A machine shutting down
        // kills every shell before the host, and each loss written as it
        // happened would leave the record emptier for every pane the host
        // outlived; what is worth restoring is the arrangement before the loss.
        if (ending == SessionEnding::Concluded)
            scheduleLayoutRecordWrite(*workspace);
        if (workspace->paneSessions.empty()) {
            const auto* ended = workspace.get();
            std::optional<RestorableWorkspace> lost;
            if (ending == SessionEnding::Lost && isRecordedWorkspace(*workspace) && workspace->recordedLayout) {
                Ubiquitty::PAL::logLine(stderr, "daemon: {} lost its last pane to a signal; it stays restorable", describeWorkspace(workspace->name, workspace->workspaceID));
                lost = RestorableWorkspace { workspace->workspaceID, std::move(*workspace->recordedLayout) };
            }
            std::erase_if(m_workspaces, [ended](const auto& candidate) { return candidate.get() == ended; });
            if (lost) {
                keepRestorableWorkspace(std::move(*lost));
                scheduleLayoutRecordWrite();
            }
        } else {
            admitWorkspacePanes(*workspace);
        }
        return;
    }
}

std::optional<std::string> Daemon::rearrangeRefusal(const Client& client, const HostedWorkspace& workspace)
{
    const std::string named = "workspace:" + std::to_string(workspace.workspaceID);
    if (!isAgentClient(client)) {
        if (capabilityOriginOf(client) == Ubiquitty::CapabilityOrigin::Local || (client.workspaceID != 0 && client.workspaceID == workspace.workspaceID))
            return std::nullopt;
        return "a person on another machine changes only the workspace their connection is attached to, and this one is attached to " + (client.workspaceID ? "workspace:" + std::to_string(client.workspaceID) : std::string("none")) + ", not " + named;
    }
    // A person attached is said first: no grant answers it.
    for (const auto& binding : workspace.paneSessions) {
        if (sessionHasHumanClient(binding.second))
            return "a person is attached to session:" + std::to_string(binding.second) + " of " + named + ", and no grant or consent lets an agent change a workspace a person is using: the person changes it from their own window";
    }
    for (const auto& binding : workspace.paneSessions) {
        const Session* session = findSession(binding.second);
        if (!session || !agentHasStandingGrant(client, *session))
            return "this agent holds no grant for session:" + std::to_string(binding.second) + " of " + named + ": it changes a workspace only when it made every session in it or a person allowed its attach to each, for the session or always";
    }
    if (workspace.paneSessions.empty())
        return named + " holds no session";
    return std::nullopt;
}

bool Daemon::performWorkspaceVerb(std::size_t clientIndex, HostedWorkspace& workspace, Session& attached, const Ubiquitty::Session::WorkspaceIntent& verb, Ubiquitty::Session::WorkspaceIntentResult& result)
{
    using namespace Ubiquitty::Session::Layout;
    using WorkspaceVerb = Ubiquitty::Session::WorkspaceIntentKind;
    using Ubiquitty::Panes::Direction;
    using Ubiquitty::Panes::InsertSide;
    using Ubiquitty::Panes::Orientation;

    Workspace& model = workspace.layout;
    const auto surface = static_cast<SurfaceID>(verb.surfaceID);
    const auto tab = static_cast<TabID>(verb.tabID);
    const auto pane = static_cast<PaneID>(verb.paneID);
    const auto targetPane = static_cast<PaneID>(verb.targetPaneID);
    const auto targetTab = static_cast<TabID>(verb.targetTabID);
    const auto orientation = static_cast<Orientation>(verb.orientation);
    const auto side = static_cast<InsertSide>(verb.side);
    const auto direction = static_cast<Direction>(verb.direction);

    const auto closeSurface = [&](SurfaceID closingSurface) {
        if (model.tabCount(closingSurface) == 0)
            return false;
        if (model.surfaceCount() > 1)
            return model.closeSurface(closingSurface);
        workspace.isClosing = true;
        return true;
    };
    const auto closeTab = [&](SurfaceID closingSurface, TabID closingTab) {
        if (!model.paneTree(closingSurface, closingTab))
            return false;
        return model.tabCount(closingSurface) > 1 ? model.closeTab(closingSurface, closingTab) : closeSurface(closingSurface);
    };
    const auto closePane = [&](PaneID closingPane) {
        const auto closingSurface = model.surfaceHolding(closingPane);
        if (closingSurface != surface)
            return false;
        const auto closingTab = model.tabHolding(closingSurface, closingPane);
        const auto* tree = model.paneTree(closingSurface, closingTab);
        if (!tree)
            return false;
        return tree->paneCount() > 1 ? model.removePane(closingPane) : closeTab(closingSurface, closingTab);
    };

    // The session a minted pane opens from: the pane the verb names, else the
    // one focused where the verb lands (the pane a split divides), else the
    // asking client's own. Read before the verb moves the focus.
    Session* openedFrom = findSession(workspace.sessionFor(pane));
    if (!openedFrom)
        openedFrom = findSession(workspace.sessionFor(model.focusedPane(surface)));
    if (!openedFrom)
        openedFrom = &attached;

    // A pane the model minted gets a session at the asking client's geometry,
    // so its first checkpoint is the size that client expects. A hosted workspace
    // never describes a pane nothing draws into: a pane that cannot be backed
    // is taken out of the model again and the verb is refused.
    const auto backNewPane = [&](PaneID minted) -> bool {
        // The scope that creates sessions, asked for where the session is created,
        // so a verb the gate at the message does not name as minting still cannot
        // mint one without it.
        if (clientIndex >= m_clients.size() || !tokenScopeAllows(m_clients[clientIndex], Protocol::Sync::Scope::NewSession))
            return false;
        const SurfaceID holder = model.surfaceHolding(minted);
        const RootIdentity placement { holder, model.tabHolding(holder, minted), minted };
        // The pane an agent mints is its own, as the session it creates by
        // HELLO is: it may attach to it and end it later without asking a
        // person who never saw it.
        const std::string creatorKey = isAgentClient(m_clients[clientIndex]) ? grantKeyFor(m_clients[clientIndex]) : std::string();
        Session* session = createSession(attached.term ? attached.term->columns : 0, attached.term ? attached.term->rows : 0, {}, -1, 0, placement, creatorKey);
        if (!session)
            return false;
        // Dressed in the theme of the session it opens from, which is what the
        // client already painted the pane with: its first checkpoint changes
        // nothing a person can see. The theme is the reset baseline; an
        // override a program set there belongs to that program.
        if (session->term && openedFrom->term) {
            session->term->palette = openedFrom->term->paletteBaseline;
            session->term->paletteBaseline = openedFrom->term->paletteBaseline;
            Ubiquitty::Themes::rethemeDefaults(session->term.get());
        }
        session->endsWhenDetached = workspace.shouldEndWhenDetached;
        workspace.paneSessions.emplace_back(minted, session->id);
        result.paneID = std::to_underlying(minted);
        result.sessionID = session->id;
        return true;
    };
    // A new tab or surface arrives with one pane the model allocated.
    const auto backNewTab = [&](SurfaceID inSurface, TabID newTab) -> bool {
        const auto* tree = model.paneTree(inSurface, newTab);
        if (tree == nullptr || !backNewPane(static_cast<PaneID>(static_cast<uint32_t>(tree->focusedPaneID()))))
            return false;
        result.tabID = std::to_underlying(newTab);
        return true;
    };

    switch (verb.verb) {
    case Ubiquitty::Session::WorkspaceIntentKind::AddSurface: {
        const SurfaceID minted = model.addSurface();
        result.wasPerformed = minted != invalidSurface && backNewTab(minted, model.activeTab(minted));
        if (result.wasPerformed)
            result.surfaceID = std::to_underlying(minted);
        else if (minted != invalidSurface)
            model.closeSurface(minted);
        break;
    }
    case Ubiquitty::Session::WorkspaceIntentKind::CloseSurface:
        // Only window.close's separately confirmed revision authorizes closure.
        result.wasPerformed = false;
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::SetActiveSurface:
        result.wasPerformed = model.setActiveSurface(surface);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::AddTab: {
        const TabID minted = model.addTab(surface);
        result.wasPerformed = minted != invalidTab && backNewTab(surface, minted);
        if (!result.wasPerformed && minted != invalidTab)
            model.closeTab(surface, minted);
        break;
    }
    case Ubiquitty::Session::WorkspaceIntentKind::CloseTab:
        result.wasPerformed = closeTab(surface, tab);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::SetActiveTab:
        result.wasPerformed = model.setActiveTab(surface, tab);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::SplitFocusedPane: {
        const PaneID minted = model.splitFocusedPane(surface, orientation, verb.ratio);
        result.wasPerformed = minted != invalidPane && backNewPane(minted);
        if (!result.wasPerformed && minted != invalidPane)
            model.closePane(surface, minted);
        break;
    }
    case Ubiquitty::Session::WorkspaceIntentKind::CloseFocusedPane:
        result.wasPerformed = closePane(model.focusedPane(surface));
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::ClosePane:
        result.wasPerformed = closePane(pane);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::FocusPane:
        result.wasPerformed = model.focusPane(surface, pane);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::MoveFocus:
        result.wasPerformed = model.moveFocus(surface, direction);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::MovePane:
        result.wasPerformed = model.movePane(surface, pane, targetPane, orientation, side);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::SwapPanes:
        result.wasPerformed = model.swapPanes(surface, pane, targetPane);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::MovePaneToTab:
        result.wasPerformed = model.movePaneToTab(surface, pane, targetTab, targetPane, orientation, side, static_cast<Ubiquitty::Panes::InsertTarget>(verb.insertTarget));
        break;
    case WorkspaceVerb::MoveTabToPane:
        result.wasPerformed = model.moveTabToPane(surface, tab, targetPane, orientation, side, static_cast<Ubiquitty::Panes::InsertTarget>(verb.insertTarget));
        break;
    case WorkspaceVerb::MoveTabToSurface:
        result.wasPerformed = model.moveTabToSurface(surface, tab, model.surfaceHolding(targetPane));
        break;
    case WorkspaceVerb::MovePaneToNewTab:
        result.wasPerformed = model.movePaneToNewTab(surface, pane, model.surfaceHolding(targetPane));
        break;
    case WorkspaceVerb::ExtractTabToSurface: {
        const auto minted = model.extractTabToSurface(surface, tab);
        result.wasPerformed = minted != invalidSurface;
        result.surfaceID = std::to_underlying(minted);
        break;
    }
    case Ubiquitty::Session::WorkspaceIntentKind::ExtractPaneToTab: {
        const TabID minted = model.extractPaneToTab(surface, pane);
        result.wasPerformed = minted != invalidTab;
        if (result.wasPerformed)
            result.tabID = std::to_underlying(minted); // an existing pane moved; nothing to back
        break;
    }
    case Ubiquitty::Session::WorkspaceIntentKind::ExtractPaneToSurface: {
        const SurfaceID minted = model.extractPaneToSurface(surface, pane);
        result.wasPerformed = minted != invalidSurface;
        if (result.wasPerformed)
            result.surfaceID = std::to_underlying(minted);
        break;
    }
    case Ubiquitty::Session::WorkspaceIntentKind::BalanceActiveTab:
        result.wasPerformed = model.balanceActiveTab(surface);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::AdjustFocusedPaneRatio:
        result.wasPerformed = model.adjustFocusedPaneRatio(surface, direction, verb.ratio);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::SetDividerRatio:
        result.wasPerformed = model.setDividerRatio(surface, verb.splitIndex, verb.ratio);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::ToggleZoom:
        result.wasPerformed = model.toggleZoom(surface, pane);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::RenameWorkspace: {
        // A name is refused where it is given, not cut where it is sent: a
        // workspace's structure carries it whole (Docs/sync.md §7.3).
        if (verb.title.size() > Protocol::Sync::maxWorkspaceNameLength) {
            Ubiquitty::PAL::logLine(stderr, "daemon: client {} asked to name workspace {} with {} bytes; a name is at most {}", clientIndex, workspace.workspaceID, verb.title.size(), Protocol::Sync::maxWorkspaceNameLength);
            break;
        }
        // This frame's answer has no room for a reason, so the log takes
        // it; `workspace.rename` answers with it.
        if (const std::optional<std::string> refusal = renameWorkspace(workspace, verb.title)) {
            Ubiquitty::PAL::logLine(stderr, "daemon: client {} was refused a name for workspace {}: {}", clientIndex, workspace.workspaceID, *refusal);
            break;
        }
        result.wasPerformed = true;
        break;
    }
    case Ubiquitty::Session::WorkspaceIntentKind::RenameTab:
        if (const std::optional<std::string> refusal = renameTab(workspace, surface, tab, verb.title)) {
            Ubiquitty::PAL::logLine(stderr, "daemon: client {} was refused a name for a tab of workspace {}: {}", clientIndex, workspace.workspaceID, *refusal);
            break;
        }
        result.wasPerformed = true;
        break;
    // A label names only what this client's own workspace holds, or the client
    // itself: an identity elsewhere on the host is not this verb's to name.
    case Ubiquitty::Session::WorkspaceIntentKind::LabelSurface:
        result.wasPerformed = model.surface(surface) && setLabel({ Identity::ObjectKind::Window, std::to_underlying(surface) }, verb.title);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::LabelPane:
        result.wasPerformed = model.surfaceHolding(pane) != invalidSurface && setLabel({ Identity::ObjectKind::Pane, std::to_underlying(pane) }, verb.title);
        break;
    case Ubiquitty::Session::WorkspaceIntentKind::LabelSession: {
        const uint32_t sessionID = workspace.sessionFor(pane);
        result.wasPerformed = sessionID != 0 && findSession(sessionID) && setLabel({ Identity::ObjectKind::Session, sessionID }, verb.title);
        break;
    }
    case Ubiquitty::Session::WorkspaceIntentKind::LabelClient:
        result.wasPerformed = clientIndex < m_clients.size() && setLabel({ Identity::ObjectKind::Client, m_clients[clientIndex].clientID }, verb.title);
        break;
    }
    return result.wasPerformed;
}

std::optional<std::string> Daemon::renameWorkspace(HostedWorkspace& workspace, std::string_view name)
{
    // A name is refused where it is given, not cut where it is sent: a
    // workspace's structure carries it whole (Docs/sync.md §7.3).
    if (name.size() > Ubiquitty::Session::maxTreeNameBytes)
        return "a workspace's name is at most " + std::to_string(Ubiquitty::Session::maxTreeNameBytes) + " bytes, and this one is " + std::to_string(name.size());
    // A restorable workspace holds its name too: taking it would leave
    // the record under the name reachable by nothing.
    const Session* named = findSessionByWorkspaceName(std::string(name));
    if ((named && workspaceHolding(named->id) != &workspace) || findRestorableWorkspaceByName(name))
        return "another workspace of this host is named \"" + std::string(name) + "\"";
    workspace.name = name;
    return std::nullopt;
}

std::optional<std::string> Daemon::renameTab(HostedWorkspace& workspace, Ubiquitty::Session::Layout::SurfaceID surface, Ubiquitty::Session::Layout::TabID tab, std::string_view name)
{
    if (name.size() > Ubiquitty::Session::maxTreeNameBytes)
        return "a tab's name is at most " + std::to_string(Ubiquitty::Session::maxTreeNameBytes) + " bytes, and this one is " + std::to_string(name.size());
    if (!workspace.layout.renameTab(surface, tab, name))
        return "the window holds no such tab";
    return std::nullopt;
}

Identity::LabelVerdict Daemon::putLabel(Identity::ObjectID id, std::string_view label)
{
    Identity::LabelVerdict verdict = m_labels.set(id, label);
    if (verdict == Identity::LabelVerdict::Full) {
        m_labels.prune([this](Identity::ObjectID labelled) { return isLiveObject(labelled); });
        verdict = m_labels.set(id, label);
    }
    return verdict;
}

bool Daemon::setLabel(Identity::ObjectID id, std::string_view label)
{
    return putLabel(id, label) == Identity::LabelVerdict::Accepted;
}

Identity::TagVerdict Daemon::putTags(Identity::ObjectID id, std::span<const std::string> names)
{
    Identity::TagVerdict verdict = m_tags.set(id, names);
    if (verdict == Identity::TagVerdict::Full) {
        m_tags.prune([this](Identity::ObjectID tagged) { return isLiveObject(tagged); });
        verdict = m_tags.set(id, names);
    }
    return verdict;
}

bool Daemon::setTags(Identity::ObjectID id, std::span<const std::string> names)
{
    return putTags(id, names) == Identity::TagVerdict::Accepted;
}

std::optional<Daemon::LocatedObject> Daemon::locateObject(Identity::ObjectID id)
{
    using namespace Ubiquitty::Session::Layout;
    if (id.number == 0 || id.number > UINT32_MAX)
        return std::nullopt;
    const auto number = static_cast<std::uint32_t>(id.number);
    PaneID pane = invalidPane;
    switch (id.kind) {
    case Identity::ObjectKind::Client:
        return std::nullopt;
    case Identity::ObjectKind::Workspace: {
        const auto found = std::ranges::find_if(m_workspaces, [&](const auto& held) { return held->workspaceID == number && !held->isClosing; });
        return found == m_workspaces.end() ? std::nullopt : std::optional { LocatedObject { .workspace = found->get(), .surface = invalidSurface, .tab = invalidTab } };
    }
    case Identity::ObjectKind::Session: {
        Session* session = findSession(number);
        if (!session)
            return std::nullopt;
        HostedWorkspace& workspace = hostedWorkspaceFor(*session);
        if (workspace.isClosing)
            return std::nullopt;
        const SurfaceID surface = workspace.layout.surfaceHolding(workspace.paneFor(number));
        return LocatedObject { .workspace = &workspace, .surface = surface, .tab = workspace.layout.tabHolding(surface, workspace.paneFor(number)) };
    }
    case Identity::ObjectKind::Pane:
        pane = PaneID { number };
        break;
    case Identity::ObjectKind::Window:
    case Identity::ObjectKind::Tab:
        break;
    }
    for (const auto& held : m_workspaces) {
        if (held->isClosing)
            continue;
        const Workspace& layout = held->layout;
        if (id.kind == Identity::ObjectKind::Pane) {
            const SurfaceID surface = layout.surfaceHolding(pane);
            if (surface != invalidSurface)
                return LocatedObject { .workspace = held.get(), .surface = surface, .tab = layout.tabHolding(surface, pane) };
            continue;
        }
        if (id.kind == Identity::ObjectKind::Window) {
            if (layout.surface(SurfaceID { number }))
                return LocatedObject { .workspace = held.get(), .surface = SurfaceID { number }, .tab = invalidTab };
            continue;
        }
        for (int index = 0; index < layout.surfaceCount(); ++index) {
            if (layout.paneTree(layout.surfaceIDAt(index), TabID { number }))
                return LocatedObject { .workspace = held.get(), .surface = layout.surfaceIDAt(index), .tab = TabID { number } };
        }
    }
    return std::nullopt;
}

Ubiquitty::Session::LaunchContext Daemon::launchContextFor(const Session& session) const
{
    using Identity::ObjectKind;
    Ubiquitty::Session::LaunchContext context;
    context.socketPath = m_socketPath;
    context.session = Identity::ObjectID { ObjectKind::Session, session.id };
    const auto& placement = session.placement;
    if (placement.pane != Ubiquitty::Session::Layout::invalidPane) {
        context.window = Identity::ObjectID { ObjectKind::Window, std::to_underlying(placement.surface) };
        context.tab = Identity::ObjectID { ObjectKind::Tab, std::to_underlying(placement.tab) };
        context.pane = Identity::ObjectID { ObjectKind::Pane, std::to_underlying(placement.pane) };
    }
    return context;
}

bool Daemon::isLiveObject(Identity::ObjectID id) const
{
    using namespace Ubiquitty::Session::Layout;
    if (id.number == 0 || id.number > UINT32_MAX)
        return false;
    const auto number = static_cast<uint32_t>(id.number);
    switch (id.kind) {
    case Identity::ObjectKind::Workspace:
        return std::ranges::any_of(m_workspaces, [&](const auto& workspace) { return workspace->workspaceID == number; });
    case Identity::ObjectKind::Session:
        return std::ranges::any_of(m_sessions, [&](const auto& session) { return session->id == number; });
    case Identity::ObjectKind::Client:
        return std::ranges::any_of(m_clients, [&](const Client& client) { return client.clientID == number; });
    case Identity::ObjectKind::Window:
        return std::ranges::any_of(m_workspaces, [&](const auto& workspace) { return workspace->layout.surface(SurfaceID { number }) != nullptr; });
    case Identity::ObjectKind::Tab:
        return std::ranges::any_of(m_workspaces, [&](const auto& workspace) {
            const Workspace& layout = workspace->layout;
            for (int index = 0; index < layout.surfaceCount(); ++index) {
                if (layout.paneTree(layout.surfaceIDAt(index), TabID { number }))
                    return true;
            }
            return false;
        });
    case Identity::ObjectKind::Pane:
        return std::ranges::any_of(m_workspaces, [&](const auto& workspace) { return workspace->layout.surfaceHolding(PaneID { number }) != invalidSurface; });
    }
    return false;
}

// Workspace records.

bool Daemon::isRecordedWorkspace(const HostedWorkspace& workspace)
{
    return !workspace.shouldEndWhenDetached;
}

std::string Daemon::describeWorkspace(std::string_view name, uint32_t workspaceID)
{
    return name.empty() ? "workspace " + std::to_string(workspaceID) : "workspace \"" + std::string(name) + "\"";
}

void Daemon::keepRestorableWorkspace(RestorableWorkspace&& restorable)
{
    m_restorableWorkspaces.push_back(std::move(restorable));
    trimRestorableWorkspaces();
}

void Daemon::trimRestorableWorkspaces()
{
    // The most recently held first, so the tail is what goes: a workspace
    // nobody has held for longest is the one least likely to be returned to.
    std::ranges::stable_sort(m_restorableWorkspaces, [](const RestorableWorkspace& a, const RestorableWorkspace& b) {
        return a.record.heldUnixSeconds > b.record.heldUnixSeconds;
    });
    const std::size_t live = static_cast<std::size_t>(std::ranges::count_if(m_workspaces, [](const auto& workspace) {
        return isRecordedWorkspace(*workspace) && !workspace->paneSessions.empty();
    }));
    const std::size_t kept = live >= Ubiquitty::Session::maxHostLayoutRecords ? 0 : Ubiquitty::Session::maxHostLayoutRecords - live;
    while (m_restorableWorkspaces.size() > kept) {
        const RestorableWorkspace& evicted = m_restorableWorkspaces.back();
        Ubiquitty::PAL::logLine(stderr, "daemon: {} is no longer kept restorable: the host keeps the {} most recently held", describeWorkspace(evicted.record.arrangement.name, evicted.workspaceID), Ubiquitty::Session::maxHostLayoutRecords);
        m_restorableWorkspaces.pop_back();
    }
}

void Daemon::scheduleLayoutRecordWrite()
{
    // A daemon that was never started has no file to write, and a deadline it
    // can never meet would only wake its loop.
    if (m_layoutRecordPath.empty() || !m_isLayoutRecordKept)
        return;
    m_layoutRecordWrite.oweForChange(nowMilliseconds(), layoutRecordDebounceMilliseconds);
}

void Daemon::scheduleLayoutRecordWrite(const HostedWorkspace& workspace)
{
    if (isRecordedWorkspace(workspace))
        scheduleLayoutRecordWrite();
}

void Daemon::noteLayoutInput(const Session& session, std::span<const std::uint8_t> bytes)
{
    if (m_layoutRecordPath.empty() || !m_isLayoutRecordKept
        || !std::ranges::any_of(bytes, [](std::uint8_t byte) { return byte == '\r' || byte == '\n'; }))
        return;
    if (const HostedWorkspace* workspace = workspaceHolding(session.id); workspace && isRecordedWorkspace(*workspace))
        m_layoutRecordWrite.oweAfterSign(nowMilliseconds(), layoutRecordDebounceMilliseconds);
}

Daemon::RestorableWorkspace* Daemon::findRestorableWorkspace(uint32_t workspaceID)
{
    if (!workspaceID)
        return nullptr;
    const auto found = std::ranges::find(m_restorableWorkspaces, workspaceID, &RestorableWorkspace::workspaceID);
    return found == m_restorableWorkspaces.end() ? nullptr : &*found;
}

Daemon::WorkspaceReference Daemon::findWorkspaceReference(std::uint64_t daemonInstanceID, uint32_t workspaceID, ReferenceSearch records, ReferenceSearch live)
{
    // The id a workspace is listed under here may be one this host gave it,
    // when the one its holders knew it by was taken; the holders say what a
    // client saved. Two workspaces that both claim the reference cannot both
    // be its workspace, so neither answers it.
    if (!daemonInstanceID || !workspaceID)
        return {};
    const Ubiquitty::Session::HostLayoutHolder reference { .daemonInstanceID = daemonInstanceID, .workspaceID = workspaceID };
    WorkspaceReference answer;
    std::string claimant;
    const auto claim = [&](WorkspaceReference found, std::string name) {
        if (!answer.isFound()) {
            answer = found;
            claimant = std::move(name);
            return true;
        }
        Ubiquitty::PAL::logLine(stderr, "daemon: {} and {} both name workspace {} of instance {:016x}; neither answers it", claimant, name, workspaceID, daemonInstanceID);
        return false;
    };
    if (records == ReferenceSearch::Included) {
        for (const RestorableWorkspace& restorable : m_restorableWorkspaces) {
            if (Ubiquitty::Session::isRecordOfReference(restorable.record, daemonInstanceID, workspaceID)
                && !claim({ .recordedWorkspaceID = restorable.workspaceID, .live = nullptr }, describeWorkspace(restorable.record.arrangement.name, restorable.workspaceID)))
                return {};
        }
    }
    if (live == ReferenceSearch::Included) {
        for (const auto& workspace : m_workspaces) {
            if (!workspace || workspace->isClosing)
                continue;
            assert(workspace->formerHolders.size() <= Ubiquitty::Session::maxHostLayoutHolders);
            if (std::ranges::find(workspace->formerHolders, reference) != workspace->formerHolders.end()
                && !claim({ .recordedWorkspaceID = 0, .live = workspace.get() }, describeWorkspace(workspace->name, workspace->workspaceID)))
                return {};
        }
    }
    return answer;
}

Daemon::Session* Daemon::answeringSession(const HostedWorkspace& workspace, uint32_t requestedSessionID)
{
    if (requestedSessionID) {
        if (Session* requested = findSession(requestedSessionID); requested && workspaceHolding(requested->id) == &workspace)
            return requested;
    }
    if (Session* focused = findSession(workspace.sessionFor(workspace.layout.focusedPane(workspace.layout.activeSurface()))))
        return focused;
    for (const auto& [pane, sessionID] : workspace.paneSessions) {
        if (Session* session = findSession(sessionID))
            return session;
    }
    return nullptr;
}

Daemon::RestorableWorkspace* Daemon::findRestorableWorkspaceByName(std::string_view workspaceName)
{
    if (workspaceName.empty())
        return nullptr;
    const auto found = std::ranges::find_if(m_restorableWorkspaces, [&](const RestorableWorkspace& restorable) { return restorable.record.arrangement.name == workspaceName; });
    return found == m_restorableWorkspaces.end() ? nullptr : &*found;
}

std::optional<Ubiquitty::Session::HostLayoutRecord> Daemon::captureLayoutRecord(HostedWorkspace& workspace)
{
    using namespace Ubiquitty::Session;
    struct Capture {
        Daemon* daemon;
        HostedWorkspace* workspace;
        std::vector<HostLayoutPane> panes;
    } capture { this, &workspace, {} };
    HostLayoutRecord record;
    record.arrangement = Layout::captureToSnapshot(
        workspace.layout,
        [](void* context, Layout::PaneID pane) {
            auto& capture = *static_cast<Capture*>(context);
            Daemon& daemon = *capture.daemon;
            Layout::PaneInfo info;
            HostLayoutPane facts;
            const auto tagsOf = [&](Identity::ObjectID tagged) {
                const std::span<const std::string> tags = daemon.m_tags.tags(tagged);
                return std::vector<std::string>(tags.begin(), tags.end());
            };
            facts.label = daemon.m_labels.label({ Identity::ObjectKind::Pane, std::to_underlying(pane) });
            facts.tags = tagsOf({ Identity::ObjectKind::Pane, std::to_underlying(pane) });
            const uint32_t sessionID = capture.workspace->sessionFor(pane);
            if (Session* session = daemon.findSession(sessionID)) {
                facts.sessionLabel = daemon.m_labels.label({ Identity::ObjectKind::Session, sessionID });
                facts.sessionTags = tagsOf({ Identity::ObjectKind::Session, sessionID });
                if (!session->sshHost.empty())
                    info.origin = { Transport::OriginKind::SSH, session->sshHost };
                if (session->term) {
                    info.title = Ubiquitty::boundedUTF8String(session->term->titleUTF8, maxHostLayoutTitleLength);
                    info.directory = session->term->currentDirectory;
                    facts.columns = static_cast<std::uint16_t>(std::clamp(session->term->columns, 1, static_cast<int>(UINT16_MAX)));
                    facts.rows = static_cast<std::uint16_t>(std::clamp(session->term->rows, 1, static_cast<int>(UINT16_MAX)));
                }
                // A shell that reports no directory by OSC 7 is asked where
                // its foreground process is, as a listing asks it.
                if (info.directory.empty() && session->stream)
                    info.directory = session->stream->process().foregroundCurrentDirectory();
            }
            // A directory cut short is a different directory; none starts the
            // shell at home, which is at least a place the person can name.
            if (info.directory.size() > maxHostLayoutDirectoryLength)
                info.directory.clear();
            if (!facts.columns || !facts.rows) {
                facts.columns = static_cast<std::uint16_t>(daemon.m_columns);
                facts.rows = static_cast<std::uint16_t>(daemon.m_rows);
            }
            capture.panes.push_back(std::move(facts));
            return info;
        },
        &capture);
    if (capture.panes.size() > maxHostLayoutPanes) {
        Ubiquitty::PAL::logLine(stderr, "daemon: {} holds {} panes, more than a record keeps; it is not recorded", describeWorkspace(workspace.name, workspace.workspaceID), capture.panes.size());
        return std::nullopt;
    }
    // Persist the same identity a handshake and listing expose, never a
    // shortened name that could collide with another workspace.
    if (workspace.name.size() > Ubiquitty::Session::maxWorkspaceNameBytes)
        return std::nullopt;
    record.arrangement.name = workspace.name;
    for (auto& window : record.arrangement.windows) {
        for (auto& tab : window.tabs)
            tab.title = Ubiquitty::boundedUTF8String(tab.title, maxHostLayoutTitleLength);
    }
    const auto tagsOf = [&](Identity::ObjectID tagged) {
        const std::span<const std::string> tags = m_tags.tags(tagged);
        return std::vector<std::string>(tags.begin(), tags.end());
    };
    record.workspaceID = workspace.workspaceID;
    record.holders = holdersAfter(workspace.formerHolders, { .daemonInstanceID = m_daemonInstanceID, .workspaceID = workspace.workspaceID });
    workspace.heldUnixSeconds = heldTimeAfter(workspace.heldUnixSeconds, static_cast<std::uint64_t>(::time(nullptr)));
    record.heldUnixSeconds = workspace.heldUnixSeconds;
    record.workspaceLabel = m_labels.label({ Identity::ObjectKind::Workspace, workspace.workspaceID });
    record.workspaceTags = tagsOf({ Identity::ObjectKind::Workspace, workspace.workspaceID });
    for (int surfaceIndex = 0; surfaceIndex < workspace.layout.surfaceCount(); ++surfaceIndex) {
        const Layout::SurfaceID surface = workspace.layout.surfaceIDAt(surfaceIndex);
        record.windowLabels.emplace_back(m_labels.label({ Identity::ObjectKind::Window, std::to_underlying(surface) }));
        record.windowTags.push_back(tagsOf({ Identity::ObjectKind::Window, std::to_underlying(surface) }));
        for (int tabIndex = 0; tabIndex < workspace.layout.tabCount(surface); ++tabIndex) {
            const Identity::ObjectID tab { Identity::ObjectKind::Tab, std::to_underlying(workspace.layout.tabAt(surface, tabIndex)) };
            record.tabLabels.emplace_back(m_labels.label(tab));
            record.tabTags.push_back(tagsOf(tab));
        }
    }
    record.panes = std::move(capture.panes);
    return record;
}

bool Daemon::writeLayoutRecords()
{
    m_layoutRecordWrite.noteWritten(nowMilliseconds(), layoutRecordDebounceMilliseconds);
    if (m_layoutRecordPath.empty() || !m_isLayoutRecordKept)
        return false;
    std::vector<Ubiquitty::Session::HostLayoutRecord> records;
    // A name reaches one workspace, so it is kept once; ids are this host's
    // and distinct already.
    const auto isNameTaken = [&](std::string_view name) {
        return !name.empty() && std::ranges::any_of(records, [&](const auto& record) { return record.arrangement.name == name; });
    };
    for (const auto& workspace : m_workspaces) {
        if (!isRecordedWorkspace(*workspace) || workspace->paneSessions.empty() || records.size() >= Ubiquitty::Session::maxHostLayoutRecords)
            continue;
        auto record = captureLayoutRecord(*workspace);
        if (!record || isNameTaken(record->arrangement.name))
            continue;
        workspace->recordedLayout = *record;
        records.push_back(std::move(*record));
    }
    for (const RestorableWorkspace& restorable : m_restorableWorkspaces) {
        if (records.size() < Ubiquitty::Session::maxHostLayoutRecords && !isNameTaken(restorable.record.arrangement.name))
            records.push_back(restorable.record);
    }
    if (records.empty()) {
        // Nothing named outlives this host now, so nothing is offered after it.
        if (::unlink(m_layoutRecordPath.c_str()) != 0 && errno != ENOENT) {
            Ubiquitty::PAL::logLine(stderr, "daemon: could not remove {}: {}", m_layoutRecordPath, std::strerror(errno));
            return false;
        }
        m_writtenLayoutRecords.clear();
        return true;
    }
    std::vector<std::uint8_t> bytes;
    Ubiquitty::Session::encodeHostLayoutRecords(records, bytes);
    if (bytes == m_writtenLayoutRecords)
        return true;
    // Written beside the file and renamed over it, so a reader — the next host,
    // after a crash in the middle of this — finds the old record or the new one
    // and never half of either.
    const std::string temporaryPath = m_layoutRecordPath + ".tmp";
    PAL::UnixFileDescriptor file(::open(temporaryPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600));
    bool isWritten = static_cast<bool>(file) && ::fchmod(file.get(), 0600) == 0;
    for (std::size_t offset = 0; isWritten && offset < bytes.size();) {
        const ssize_t written = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        isWritten = written > 0;
        offset += isWritten ? static_cast<std::size_t>(written) : 0;
    }
    isWritten = isWritten && ::fsync(file.get()) == 0;
    file.reset();
    if (!isWritten || ::rename(temporaryPath.c_str(), m_layoutRecordPath.c_str()) != 0) {
        Ubiquitty::PAL::logLine(stderr, "daemon: could not write {}: {}", m_layoutRecordPath, std::strerror(errno));
        ::unlink(temporaryPath.c_str());
        return false;
    }
    m_writtenLayoutRecords = std::move(bytes);
    return true;
}

namespace {

/// Reads a record file whole, refusing anything but a regular file of a size
/// a host writes.
///
/// - Parameter path: The file.
/// - Returns: The bytes, or nothing when there is no such file to read.
std::optional<std::vector<std::uint8_t>> readLayoutRecordFile(const std::string& path)
{
    PAL::UnixFileDescriptor file(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat status {};
    if (!file || ::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0
        || static_cast<std::uint64_t>(status.st_size) > Ubiquitty::Session::maxHostLayoutFileBytes)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(status.st_size));
    for (std::size_t offset = 0; offset < bytes.size();) {
        const ssize_t got = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return std::nullopt;
        offset += static_cast<std::size_t>(got);
    }
    return bytes;
}

} // namespace

void Daemon::admitRestorableRecords(std::vector<Ubiquitty::Session::HostLayoutRecord>&& records)
{
    for (auto& record : records) {
        if (findRestorableWorkspaceByName(record.arrangement.name))
            continue;
        // The id it was recorded with when nothing here holds that id, so it
        // comes back under the id its clients saved; otherwise one from the
        // session id space, as every workspace id is. Either way no session
        // created later takes it: a session id is never one a restorable
        // workspace holds. A saved reference is matched against the record's
        // holders, which keep the ids they held it under whichever it is
        // listed by here.
        const auto isTaken = [&](uint32_t candidate) {
            return findSession(candidate) || findRestorableWorkspace(candidate)
                || std::ranges::any_of(m_workspaces, [&](const auto& workspace) { return workspace->workspaceID == candidate; });
        };
        uint32_t workspaceID = record.workspaceID && !isTaken(record.workspaceID) ? record.workspaceID : 0;
        while (!workspaceID && m_nextSessionID) {
            const uint32_t candidate = m_nextSessionID++;
            if (!isTaken(candidate))
                workspaceID = candidate;
        }
        if (!workspaceID)
            break;
        // Listed under the id the record names from here on, so the next
        // write says what this host lists it by and no two records of the
        // file share one.
        record.workspaceID = workspaceID;
        m_restorableWorkspaces.push_back({ workspaceID, std::move(record) });
    }
    trimRestorableWorkspaces();
}

bool Daemon::adoptSiblingLayoutRecords(const std::string& path)
{
    const auto bytes = readLayoutRecordFile(path);
    if (!bytes)
        return false;
    Ubiquitty::Session::HostLayoutFile file = Ubiquitty::Session::readHostLayoutFile(*bytes);
    if (file.verdict != Ubiquitty::Session::HostLayoutFileVerdict::Read) {
        Ubiquitty::PAL::logLine(stderr, "daemon: {} is not read because {}; it is left as it is", path, Ubiquitty::Session::describeUnreadHostLayoutFile(file));
        return false;
    }
    Ubiquitty::PAL::logLine(stderr, "daemon: taking in {} workspace record(s) from {}, whose host is not running", file.records.size(), path);
    admitRestorableRecords(std::move(file.records));
    return true;
}

void Daemon::retireStartupLayoutRecords(const std::string& startupPath)
{
    if (startupPath.empty() || startupPath == m_layoutRecordPath || ::access(startupPath.c_str(), F_OK) != 0)
        return;
    if (writeLayoutRecords() && ::unlink(startupPath.c_str()) != 0 && errno != ENOENT)
        Ubiquitty::PAL::logLine(stderr, "daemon: could not remove {}, whose records {} now keeps: {}", startupPath, m_layoutRecordPath, std::strerror(errno));
}

void Daemon::setAsideUnreadLayoutRecords(std::string_view reason)
{
    // A file this host cannot read is not a file that holds nothing: the next
    // write would replace it, and whatever workspaces it recorded would be
    // forgotten. It keeps its bytes under another name, where the build that
    // wrote it, or a person, can still read them.
    //
    // Each file set aside before is kept too, a place older, so that a host
    // which meets a second unreadable file does not replace the first. The
    // oldest of `maxUnreadHostLayoutFiles` makes room, and the log says so.
    using Ubiquitty::Session::maxUnreadHostLayoutFiles;
    using Ubiquitty::Session::unreadHostLayoutFilePath;
    for (std::size_t age = maxUnreadHostLayoutFiles; age-- > 0;) {
        const std::string older = unreadHostLayoutFilePath(m_layoutRecordPath, age);
        if (::access(older.c_str(), F_OK) != 0)
            continue;
        if (age + 1 == maxUnreadHostLayoutFiles) {
            if (::unlink(older.c_str()) == 0) {
                Ubiquitty::PAL::logLine(stderr, "daemon: {} is removed: it is the oldest of the {} record files kept unread, and another is set aside", older, maxUnreadHostLayoutFiles);
                continue;
            }
        } else if (::rename(older.c_str(), unreadHostLayoutFilePath(m_layoutRecordPath, age + 1).c_str()) == 0) {
            continue;
        }
        Ubiquitty::PAL::logLine(stderr, "daemon: {} is not read because {}, and {} could not make room for it ({}); this host records no workspace, so both files stay as they are", m_layoutRecordPath, reason, older, std::strerror(errno));
        m_isLayoutRecordKept = false;
        return;
    }
    const std::string kept = unreadHostLayoutFilePath(m_layoutRecordPath, 0);
    if (::rename(m_layoutRecordPath.c_str(), kept.c_str()) == 0) {
        Ubiquitty::PAL::logLine(stderr, "daemon: {} is not read because {}; it is kept as {} and nothing is restored from it", m_layoutRecordPath, reason, kept);
        return;
    }
    Ubiquitty::PAL::logLine(stderr, "daemon: {} is not read because {}, and could not be set aside ({}); this host records no workspace, so the file stays as it is", m_layoutRecordPath, reason, std::strerror(errno));
    m_isLayoutRecordKept = false;
}

void Daemon::loadLayoutRecords()
{
    if (m_layoutRecordPath.empty())
        return;
    if (const auto bytes = readLayoutRecordFile(m_layoutRecordPath)) {
        Ubiquitty::Session::HostLayoutFile file = Ubiquitty::Session::readHostLayoutFile(*bytes);
        if (file.verdict == Ubiquitty::Session::HostLayoutFileVerdict::Read) {
            m_writtenLayoutRecords = *bytes;
            admitRestorableRecords(std::move(file.records));
        } else {
            setAsideUnreadLayoutRecords(Ubiquitty::Session::describeUnreadHostLayoutFile(file));
        }
    } else if (::access(m_layoutRecordPath.c_str(), F_OK) == 0) {
        setAsideUnreadLayoutRecords("it is larger than a record file may be, or is no regular file this host may open");
    }

    takeInStoppedSiblingRecords();
}

bool Daemon::takeInStoppedSiblingRecords()
{
    if (m_layoutRecordPath.empty())
        return false;
    // A host replaced by another binary listens on a socket of its own name, so
    // the records it left are in a file the new one would never open. They are
    // taken in only from a host that is not running: holding its startup lock
    // says no host bound to that socket is running or starting, and keeps one
    // from writing the file while it is read and removed. A chain of
    // handovers keeps its records beside the first host's socket after that
    // host has gone, and the host serving them holds no lock there, so the
    // socket is probed too: records beside a socket something listens on
    // belong to a running host, and are neither taken in nor removed. A
    // socket this host serves is its own, and is not probed: the connection
    // would only reach this host.
    const auto slash = m_layoutRecordPath.find_last_of('/');
    const std::string directory = slash == std::string::npos ? std::string(".") : m_layoutRecordPath.substr(0, slash);
    const std::string ownLeaf = slash == std::string::npos ? m_layoutRecordPath : m_layoutRecordPath.substr(slash + 1);
    constexpr std::string_view suffix = ".layouts";
    std::vector<std::string> siblings;
    if (DIR* listing = ::opendir(directory.c_str())) {
        std::size_t examined = 0;
        while (const dirent* entry = ::readdir(listing)) {
            if (++examined > maxSiblingLayoutRecordScan)
                break;
            const std::string_view leaf(entry->d_name);
            if (leaf.size() > suffix.size() && leaf.ends_with(suffix) && leaf != ownLeaf)
                siblings.emplace_back(leaf);
        }
        ::closedir(listing);
    }
    const auto isServedHere = [&](std::string_view socketPath) {
        return socketPath == m_socketPath
            || std::ranges::any_of(m_inheritedListeners, [&](const InheritedListener& listener) { return listener.path == socketPath; });
    };
    std::vector<std::pair<std::string, PAL::UnixFileDescriptor>> adopted;
    for (const std::string& leaf : siblings) {
        const std::string path = directory + "/" + leaf;
        const std::string socketPath = path.substr(0, path.size() - suffix.size());
        if (isServedHere(socketPath))
            continue;
        const std::string lockPath = socketPath + ".lock";
        PAL::UnixFileDescriptor lock(::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
        if (!lock || ::flock(lock.get(), LOCK_EX | LOCK_NB) != 0)
            continue; // its host is running, or starting
        if (isSocketPathServed(socketPath.c_str()))
            continue; // a host that took it over serves it
        if (adoptSiblingLayoutRecords(path))
            adopted.emplace_back(path, std::move(lock));
    }
    if (adopted.empty())
        return false;
    // Ours first, so the records are on disk under this socket before the file
    // they came from goes; a write that fails leaves the other file standing.
    if (!writeLayoutRecords())
        return true;
    for (const auto& [path, lock] : adopted)
        ::unlink(path.c_str());
    return true;
}

Daemon::Session* Daemon::restoreWorkspace(uint32_t workspaceID)
{
    using namespace Ubiquitty::Session;
    const auto found = std::ranges::find(m_restorableWorkspaces, workspaceID, &RestorableWorkspace::workspaceID);
    if (found == m_restorableWorkspaces.end())
        return nullptr;
    HostLayoutRecord record = std::move(found->record);
    m_restorableWorkspaces.erase(found);
    // The record leaves the restorable list either way: restored, it is the
    // live workspace's to write; refused, it is not one to offer again.
    scheduleLayoutRecordWrite();

    auto owned = std::make_unique<HostedWorkspace>(m_layoutIdentities);
    HostedWorkspace& workspace = *owned;
    workspace.workspaceID = workspaceID;
    workspace.name = record.arrangement.name;
    workspace.wasRestoredAfterHostRestart = true;
    workspace.formerHolders = record.holders;
    workspace.heldUnixSeconds = record.heldUnixSeconds;
    std::vector<std::pair<Layout::PaneID, Layout::PaneInfo>> leaves;
    const bool isArranged = Layout::restoreFromSnapshot(
        record.arrangement, workspace.layout,
        [](void* context, Layout::PaneID pane, const Layout::PaneInfo& info) { static_cast<decltype(leaves)*>(context)->emplace_back(pane, info); },
        &leaves);
    if (!isArranged || leaves.size() != record.panes.size()) {
        Ubiquitty::PAL::logLine(stderr, "daemon: the record of {} does not describe an arrangement this host can hold; it is dropped", describeWorkspace(workspace.name, workspace.workspaceID));
        return nullptr;
    }
    // Listed under its id before any session is made, so no session takes it.
    m_workspaces.reserve(m_workspaces.size() + m_preparedLocalAdopts + 1);
    m_workspaces.push_back(std::move(owned));

    std::vector<Layout::PaneID> unbacked;
    for (std::size_t index = 0; index < leaves.size(); ++index) {
        const auto& [pane, info] = leaves[index];
        const HostLayoutPane& facts = record.panes[index];
        // A remote pane runs what the record names only if it is still a host
        // string the HELLO would accept; it never falls back to a local shell
        // the person would type remote commands into.
        std::vector<std::string> command;
        if (info.origin.transport == Transport::OriginKind::SSH) {
            if (Transport::validateSSHHost(info.origin.host))
                command = Network::buildSSHArguments(remoteLaunchProgram, info.origin.host);
            if (command.empty()) {
                unbacked.push_back(pane);
                continue;
            }
        }
        const Layout::SurfaceID surface = workspace.layout.surfaceHolding(pane);
        const Layout::RootIdentity placement { surface, workspace.layout.tabHolding(surface, pane), pane };
        Session* session = createSession(facts.columns, facts.rows, command, -1, 0, placement, {}, info.directory);
        if (!session) {
            unbacked.push_back(pane);
            continue;
        }
        session->workspaceName = workspace.name;
        session->wasRestoredAfterHostRestart = true;
        if (!command.empty())
            session->sshHost = info.origin.host;
        workspace.paneSessions.emplace_back(pane, session->id);
        if (!facts.label.empty())
            setLabel({ Identity::ObjectKind::Pane, std::to_underlying(pane) }, facts.label);
        if (!facts.sessionLabel.empty())
            setLabel({ Identity::ObjectKind::Session, session->id }, facts.sessionLabel);
        if (!facts.tags.empty())
            setTags({ Identity::ObjectKind::Pane, std::to_underlying(pane) }, facts.tags);
        if (!facts.sessionTags.empty())
            setTags({ Identity::ObjectKind::Session, session->id }, facts.sessionTags);
    }
    if (workspace.paneSessions.empty()) {
        Ubiquitty::PAL::logLine(stderr, "daemon: no pane of {} could be started; it is dropped", describeWorkspace(workspace.name, workspace.workspaceID));
        std::erase_if(m_workspaces, [&](const auto& candidate) { return candidate.get() == &workspace; });
        return nullptr;
    }
    // The tabs are labelled and tagged while they are still the record's, in
    // its order: removing an unbacked pane may take its tab with it.
    if (!record.workspaceLabel.empty())
        setLabel({ Identity::ObjectKind::Workspace, workspace.workspaceID }, record.workspaceLabel);
    if (!record.workspaceTags.empty())
        setTags({ Identity::ObjectKind::Workspace, workspace.workspaceID }, record.workspaceTags);
    for (int surfaceIndex = 0, tabOrdinal = 0; surfaceIndex < workspace.layout.surfaceCount(); ++surfaceIndex) {
        const Layout::SurfaceID surface = workspace.layout.surfaceIDAt(surfaceIndex);
        for (int tabIndex = 0; tabIndex < workspace.layout.tabCount(surface); ++tabIndex, ++tabOrdinal) {
            const Identity::ObjectID tab { Identity::ObjectKind::Tab, std::to_underlying(workspace.layout.tabAt(surface, tabIndex)) };
            const auto ordinal = static_cast<std::size_t>(tabOrdinal);
            if (ordinal < record.tabLabels.size() && !record.tabLabels[ordinal].empty())
                setLabel(tab, record.tabLabels[ordinal]);
            if (ordinal < record.tabTags.size() && !record.tabTags[ordinal].empty())
                setTags(tab, record.tabTags[ordinal]);
        }
    }
    // A layout never names a pane nothing draws into.
    for (const Layout::PaneID pane : unbacked)
        workspace.layout.removePane(pane);
    for (std::size_t index = 0; index < record.windowLabels.size() && static_cast<int>(index) < workspace.layout.surfaceCount(); ++index) {
        if (!record.windowLabels[index].empty())
            setLabel({ Identity::ObjectKind::Window, std::to_underlying(workspace.layout.surfaceIDAt(static_cast<int>(index))) }, record.windowLabels[index]);
    }
    for (std::size_t index = 0; index < record.windowTags.size() && static_cast<int>(index) < workspace.layout.surfaceCount(); ++index) {
        if (!record.windowTags[index].empty())
            setTags({ Identity::ObjectKind::Window, std::to_underlying(workspace.layout.surfaceIDAt(static_cast<int>(index))) }, record.windowTags[index]);
    }
    Ubiquitty::PAL::logLine(stderr, "daemon: restored {} after the host restarted — {} of {} panes", describeWorkspace(workspace.name, workspace.workspaceID), workspace.paneSessions.size(), leaves.size());
    const uint32_t focused = workspace.sessionFor(workspace.layout.focusedPane(workspace.layout.activeSurface()));
    if (Session* session = findSession(focused))
        return session;
    return findSession(workspace.paneSessions.front().second);
}

// What holds a due frame back is the coherence rule's to say
// (Runtime::FrameCoherence): the parser mid-sequence, an open DEC 2026 block, a
// cursor-hidden redraw, a read that has not gone quiet. Each is a screen the
// program has not finished drawing, and here that is the daemon's decision
// alone.
//
// Not const, and cannot be: asking is what ages a 2026 block towards its expiry
// and a bracket towards persistence, so a caller that stops asking leaves a
// program that went quiet mid-frame holding every attached client's screen.
//
// `isFrameDue` survives a refusal, so the frame goes out when the rule admits
// it, at the latest on the rule's deadline. Holding defers a frame; it never
// drops one.
std::optional<Runtime::FrameClass> Daemon::mayPublishFrame(Session& s, std::uint64_t now)
{
    if (!s.isFrameDue)
        return std::nullopt;
    if (!s.term)
        return Runtime::FrameClass::Settled;
    // No connection attached to the pane draws its frames, so none is paced:
    // the per-frame steps run in the pass that read the output, and a frame
    // owes no wake of its own. A handover takes its frames by the rule.
    if (!m_quiescingPanes && !isAttachedTo(s))
        return Runtime::FrameClass::Settled;
    // A change the host made itself (a resize, a palette, a clear, an adopted
    // screen) dirties the pane without a drain pass.
    if (!s.coherence.isDirty()) {
        s.coherence.noteHostChange(now);
        s.census.noteDirty(now);
    }
    const Runtime::FrameAdmission admission = s.coherence.admit(*s.term, now, Ticks::fromMilliseconds(now / 1000), ptyReadiness(s.stream.get()));
    if (!admission.isAdmitted)
        return std::nullopt;
    return admission.frameClass;
}

std::optional<Protocol::Sync::CensusVerdict> Daemon::publishFrameIfAdmitted(Session& s)
{
    const std::uint64_t now = m_monotonicMicroseconds();
    const std::optional<Runtime::FrameClass> frameClass = mayPublishFrame(s, now);
    if (!frameClass)
        return std::nullopt;
    // Counted for the summary the reap logs; a daemon session has no halt
    // journal to take a per-frame record.
    Protocol::Sync::CensusVerdict verdict;
    if (s.term)
        verdict = s.census.notePublish(*s.term, now, *frameClass);
    publishFrame(s, *frameClass);
    return verdict;
}

void Daemon::notePaneFocus(Session& session)
{
    const bool isFocused = session.screenSource && session.screenSource->isAnyDriverFocused();
    if (isFocused == session.isDriverFocusRecorded)
        return;
    session.isDriverFocusRecorded = isFocused;
    // The host's terminal holds the pane's focus as its drivers report it:
    // a bell asks to be notified of only while nobody is looking.
    if (session.term)
        session.term->windowFocused = isFocused;
    if (!isFocused)
        return;
    Ubiquitty::Session::Event focused;
    focused.kind = Ubiquitty::Session::EventKind::PaneFocused;
    focused.sessionID = session.id;
    focused.paneID = paneOfSession(session);
    recordEvent(std::move(focused));
}

void Daemon::notePresented(std::size_t index, Client::Presented presented)
{
    Client& client = m_clients[index];
    const auto samePixels = [](const std::optional<Ubiquitty::PixelGeometry>& a, const std::optional<Ubiquitty::PixelGeometry>& b) {
        return a.has_value() == b.has_value() && (!a || (a->cell == b->cell && a->textArea == b->textArea && a->drawable == b->drawable));
    };
    if (samePixels(presented.pixels, client.presented.pixels) && presented.palette == client.presented.palette)
        return;
    presented.revision = ++m_presentationRevision;
    client.presented = std::move(presented);
    // Before anyone owns a pane's size, a verified human with Drive scope may
    // present for it. Facts alone never grant authority over program colours.
    const bool mayStandIn = !isAgentClient(client) && tokenScopeAllows(client, Protocol::Sync::Scope::Drive);
    for (const PaneSubscription& subscription : client.paneSubscriptions) {
        Session* session = findSession(subscription.sessionID);
        if (!session)
            continue;
        if (mayStandIn)
            session->presentedStandIn = geometryKey(client);
        applyOwnerPresentation(*session);
    }
}

void Daemon::applyOwnerPresentation(Session& s)
{
    if (!s.term || !s.screenSource)
        return;
    const std::optional<Protocol::Sync::ID128> drivingOwner = s.screenSource->geometry().drivingOwner();
    std::optional<Protocol::Sync::ID128> owner = drivingOwner ? drivingOwner : s.presentedStandIn;
    const auto mayPresent = [&](const Client& client) {
        if (!client.subscriptionFor(s.id) || isAgentClient(client)
            || client.didOverflow || client.sessionExitDeadlineMilliseconds || !tokenScopeAllows(client, Protocol::Sync::Scope::Drive)
            || (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr))))
            return false;
        // HELLO pixels may precede Greet. Once a screen connection exists,
        // only its live physical owner can apply a delayed presentation.
        if (!client.syncConnection)
            return true;
        const auto* attachment = client.syncConnection->attachment();
        return client.syncConnection->state() == Protocol::Sync::ConnectionState::Attached
            && attachment && attachment->state == Protocol::Sync::AttachmentState::Live
            && attachment->servingConnection == client.clientID;
    };
    auto found = owner ? std::ranges::find_if(m_clients, [&](const Client& client) {
        return geometryKey(client) == *owner && mayPresent(client);
    })
                       : m_clients.end();
    if (found == m_clients.end()) {
        // A lost driver keeps its size for resume. Its absence must not stop
        // a live person from presenting colours without taking that size.
        for (auto candidate = m_clients.begin(); candidate != m_clients.end(); ++candidate) {
            if (candidate->presented.palette && mayPresent(*candidate)
                && (found == m_clients.end() || candidate->presented.revision > found->presented.revision))
                found = candidate;
        }
        if (found != m_clients.end())
            owner = geometryKey(*found);
    }
    if (found == m_clients.end()) {
        // Nobody presents: what waited was for a client that is gone, and the
        // loop must not keep waking for it.
        s.presentedDueAt.reset();
        if (!drivingOwner)
            s.presentedStandIn.reset();
        return;
    }
    const Client::Presented& presented = found->presented;
    const PaneSubscription* subscription = found->subscriptionFor(s.id);
    assert(subscription);
    if (s.presentedApplied && s.presentedApplied->owner == *owner && s.presentedApplied->revision == presented.revision
        && s.presentedApplied->panePixelsRevision == subscription->pixelsRevision) {
        s.presentedDueAt.reset();
        return;
    }
    // A new owner's pixels and theme wait out the window the size waits out,
    // so typists in turn cost the program one change per window.
    const std::uint64_t now = nowMicroseconds();
    const std::uint64_t window = Protocol::Sync::measuredGeometryPolicy().coalesceMicroseconds;
    if (s.presentedApplied && s.presentedApplied->owner != *owner && now < s.presentedApplied->appliedAt + window) {
        s.presentedDueAt = s.presentedApplied->appliedAt + window;
        return;
    }
    s.presentedDueAt.reset();
    s.presentedApplied = Session::PresentedApplied { .owner = *owner, .revision = presented.revision, .panePixelsRevision = subscription->pixelsRevision, .appliedAt = now };
    const std::optional<Ubiquitty::PixelGeometry> pixels = subscription->pixels ? subscription->pixels
        : placementOf(s.id) == Protocol::Sync::PanePlacement::WholeWindow       ? presented.pixels
                                                                                : std::nullopt;
    if ((!drivingOwner || drivingOwner == owner) && pixels && !holdsPixels(*s.term, *pixels)) {
        Ubiquitty::publishPixelGeometry(*s.term, *pixels);
        s.isWindowSizeDue = true;
    }
    if (presented.palette && (s.term->palette != presented.palette->live || s.term->paletteBaseline != presented.palette->baseline)) {
        s.term->palette = presented.palette->live;
        s.term->paletteBaseline = presented.palette->baseline;
        // The same call a local frontend makes after writing the palette:
        // bump the epoch every mirror keys its colour caches on, repaint, and
        // tell a DEC 2031 subscriber the scheme changed, which is the point:
        // an application picks its colours from the terminal's.
        Ubiquitty::Themes::rethemeDefaults(s.term.get());
        // The palette rides the next update as a block of its own.
        s.isFrameDue = true;
        // The notification lands in the reply buffer, which servicePTY drains
        // only when the PTY produces output; an idle program is told now.
        drainTermToPTY(s);
    }
}

Protocol::Sync::ID128 Daemon::geometryKey(const Client& client) const
{
    if (client.syncConnection && client.syncConnection->attachment())
        return client.syncConnection->attachment()->attachmentID;
    // Before its attachment is known: a key no attachment has, whose low half
    // is the client and whose high half no attachment ID carries.
    return { .low = client.clientID, .high = 0 };
}

void Daemon::rekeyGeometry(const Client& client)
{
    if (!client.syncConnection || !client.syncConnection->attachment())
        return;
    const Protocol::Sync::ID128 provisional { .low = client.clientID, .high = 0 };
    for (const PaneSubscription& subscription : client.paneSubscriptions) {
        Session* session = findSession(subscription.sessionID);
        if (!session || !session->screenSource)
            continue;
        session->screenSource->geometry().rekey(provisional, client.syncConnection->attachment()->attachmentID);
        if (session->presentedStandIn == std::optional { provisional })
            session->presentedStandIn = client.syncConnection->attachment()->attachmentID;
    }
}

void Daemon::applyPendingGeometry(Session& s, std::optional<Protocol::Sync::ID128> typist)
{
    applyOwnerPresentation(s);
    if (s.screenSource && s.term) {
        Protocol::Sync::GeometryArbiter& arbiter = s.screenSource->geometry();
        if (!arbiter.applied())
            arbiter.seed({ .rows = static_cast<std::uint16_t>(s.term->rows), .columns = static_cast<std::uint16_t>(s.term->columns) });
        arbiter.setPlacement(placementOf(s.id));
        const std::uint64_t now = nowMicroseconds();
        if (const std::optional<Protocol::Sync::PaneGeometry> due = typist ? arbiter.dueBeforeInput(*typist, now) : arbiter.due(now))
            s.pendingGeometry = std::pair { clampColumns(due->columns), clampRows(due->rows) };
    }
    const std::optional<std::pair<int, int>> pending = std::exchange(s.pendingGeometry, std::nullopt);
    const bool isWindowSizeDue = std::exchange(s.isWindowSizeDue, false);
    if (!s.term || (!pending && !isWindowSizeDue))
        return;
    // New pixels at the same size still tell the program.
    const auto [columns, rows] = pending.value_or(std::pair { s.term->columns, s.term->rows });
    const Ubiquitty::IntSize pixels = Ubiquitty::reportedTextAreaPixels(*s.term);
    if (s.term->columns == columns && s.term->rows == rows) {
        if (isWindowSizeDue && s.stream)
            s.stream->setWindowSize(columns, rows, pixels.width, pixels.height);
        return;
    }
    if (s.stream)
        s.stream->setWindowSize(columns, rows, pixels.width, pixels.height);
    // Same reflow policy as every frontend (Core single source of truth):
    // supply the real foreground signal from the PAL stream so a remote
    // pager/editor (less -X, vim) gets a geometry-only resize instead of an
    // unwanted scrollback rewrap. A null stream counts as the shell (reflow),
    // matching the macOS frontend.
    const bool foregroundIsShell = (s.stream == nullptr) || s.stream->process().foregroundIsShell();
    const bool reflowed = Ubiquitty::shouldReflowOnResize(s.term.get(), foregroundIsShell);
    Ubiquitty::resize(*s.term, rows, columns,
        reflowed ? Ubiquitty::Modes::ReflowMode::Reflow
                 : Ubiquitty::Modes::ReflowMode::Fixed);
    ++s.geometryChangesApplied;
    // A new geometry is a checkpoint, the reflowed history with it, and a
    // leaf of the workspace's structure.
    s.isFrameDue = true;
    m_isStructureDirty = true;
}

Protocol::Sync::PaneEchoFacts Daemon::echoFactsOf(const Session& s) const
{
    Protocol::Sync::PaneEchoFacts echo { .isSecureEntry = false, .isAtPrompt = s.term && s.term->isReadingCommand, .interruptCharacters = Protocol::Sync::InterruptCharacters::terminalDefaults(), .foregroundGeneration = s.foregroundGeneration };
    if (const std::optional<PAL::LineDiscipline> discipline = s.stream ? s.stream->process().lineDiscipline() : std::nullopt) {
        echo.isSecureEntry = !discipline->isEchoEnabled;
        echo.eraseCharacter = discipline->eraseCharacter;
        // With signals off the characters are data: nothing interrupts.
        if (discipline->isSignalEnabled)
            echo.interruptCharacters = { .interrupt = discipline->interruptCharacter, .quit = discipline->quitCharacter, .suspend = discipline->suspendCharacter };
        else
            echo.interruptCharacters = { .interrupt = std::nullopt, .quit = std::nullopt, .suspend = std::nullopt };
    }
    return echo;
}

void Daemon::publishFrame(Session& s, Runtime::FrameClass frameClass)
{
    if (!s.term || !s.screenSource) {
        s.isFrameDue = false;
        return;
    }
    Ubiquitty::Terminal& t = *s.term;

    // The generation and its records share this budgeted foreground reading.
    // Probes belong to readers: a pane nobody reads is probed when a reader
    // attaches or a listing asks, never once a write.
    if (isFactsRead(s) || m_quiescingPanes)
        probeForegroundIfDue(s);

    // A directory reported by OSC 7 is one the record of a named workspace
    // should hold; one string compare a frame, and a lookup only on a change.
    if (t.currentDirectory != s.observedDirectory) {
        s.observedDirectory = t.currentDirectory;
        if (const HostedWorkspace* workspace = workspaceHolding(s.id))
            scheduleLayoutRecordWrite(*workspace);
    }

    // A pane no connection shows is captured by nobody: a detached flood, or
    // one in a tab no client has in front, would otherwise encode its rows
    // every frame for no one. Its source then holds a stale frame, counted
    // changed, which a Dormant attachment is told of once; a connection that
    // shows it waits for the next frame, which showing it asks for.
    const bool isShown = std::ranges::any_of(m_clients, [&s](const Client& c) {
        const PaneSubscription* subscription = c.subscriptionFor(s.id);
        return subscription && subscription->screenHandle != 0 && c.syncConnection
            && c.syncConnection->paneView(subscription->screenHandle) == Protocol::Sync::ViewState::Live;
    });
    // The generation's first change, which an update's lag is counted from.
    const std::uint64_t now = m_monotonicMicroseconds();
    const std::uint64_t changedAt = std::min(s.unpublishedSince.value_or(now), s.coherence.dirtySinceMicroseconds().value_or(now));
    if (isShown || m_quiescingPanes) {
        // What a client's predictor reads of the frame, sampled with it.
        s.publishedEchoFacts = echoFactsOf(s);
        s.screenSource->noteEchoFacts(*s.publishedEchoFacts);
        s.screenSource->publish(Runtime::wireFrameClass(frameClass), changedAt);
        s.unpublishedSince.reset();
    } else {
        s.screenSource->noteTerminalChanged();
        s.unpublishedSince = changedAt;
    }

    const bool isTitleChanged = s.term->titleChanged;
    if (isTitleChanged) {
        s.term->titleChanged = false;
        Ubiquitty::Session::Event titled;
        titled.kind = Ubiquitty::Session::EventKind::TitleChanged;
        titled.sessionID = s.id;
        titled.paneID = paneOfSession(s);
        titled.text = s.term->titleUTF8;
        recordEvent(std::move(titled));
    }
    if (typedEffectPrimaryForSession(s))
        emitPendingBell(s);
    else if (s.term->bellPending) {
        s.effectRoute.clearBells();
        s.term->bellPending = false;
        Ubiquitty::Session::Event rang;
        rang.kind = Ubiquitty::Session::EventKind::Bell;
        rang.sessionID = s.id;
        rang.paneID = paneOfSession(s);
        recordEvent(std::move(rang));
    }
    emitPendingSounds(s);

    // Harvest command effects and event records from this generation.
    emitAgentEvents(s);

    // What the host says about the pane beside its picture, read before the
    // connections are serviced so a record precedes the frame it describes.
    // A title the program set is never held to the records budget: it travels
    // with the frame that shows the program setting it.
    if (isTitleChanged && isFactsRead(s))
        publishRecords(s, nowMilliseconds());
    else
        noteRecordsDue(s, nowMilliseconds());
    // A structural frame gives the pane's leaf its generation.
    if (const std::uint64_t structural = s.screenSource->structuralGeneration(); structural != s.describedStructuralGeneration) {
        s.describedStructuralGeneration = structural;
        m_isStructureDirty = true;
    }

    s.isFrameDue = false;
    s.coherence.noteFrameTaken(t, m_monotonicMicroseconds());
}

void Daemon::receiveScreenBytes(std::size_t index, std::span<const std::uint8_t> bytes)
{
    if (index >= m_clients.size())
        return;
    if (!m_clients[index].syncConnection || m_clients[index].syncConnection->state() == Protocol::Sync::ConnectionState::Greeting)
        dropExpiredTokenClients();
    if (m_clients[index].didOverflow)
        return;
    Client& client = m_clients[index];
    if (!client.syncConnection) {
        Protocol::Sync::HostIdentity identity;
        identity.wireVersion = m_wireVersion;
        identity.hostInstanceID = m_hostInstanceID;
        identity.selection.hostPlatform = hostPlatformName();
        std::size_t datagramMaxBytes = 0;
#ifdef UBIQUITTY_QUIC
        if (client.quicConnection && m_quic && m_quic->datagramCapable(client.quicConnection))
            datagramMaxBytes = quicDatagramMaxBytes;
#endif
        client.syncConnection = std::make_unique<Protocol::Sync::HostConnection>(Protocol::Sync::measuredHostPolicy(datagramMaxBytes), std::move(identity));
        const std::uint32_t clientID = client.clientID;
        client.syncConnection->setGreetingAdmission([this, clientID](const Protocol::Sync::Greet& greet) {
            return admitGreeting(clientID, greet);
        });
        client.syncConnection->setAttachmentPresenceProvider([this, clientID](Protocol::Sync::ID128 attachmentID,
                                                                 std::span<const Protocol::Sync::ID128> panes, bool isSelf) {
            return attachmentPresenceFor(clientID, attachmentID, panes, isSelf);
        });
        if (!m_imageSourceBudget)
            m_imageSourceBudget = std::make_shared<Protocol::Sync::BulkSourceBudget>(maxAggregateOutboundBytes);
        client.imageSourceBudget = std::make_shared<Protocol::Sync::BulkSourceBudget>(clientBufferHighWater);
        client.syncConnection->setImageSourceAdmission([this, clientID] { return imageSourceAdmission(clientID); }, [this] { preemptOutboundImages(); });
        client.syncConnection->setHeldInputRoom([this] { return heldInputRoom(); });
        client.syncConnection->setInboundFrameAdmission([this, clientID](std::size_t capacity) {
            // Reaping another client between reads can shift this index.
            for (std::size_t current = 0; current < m_clients.size(); ++current) {
                if (m_clients[current].clientID == clientID && m_clients[current].syncConnection)
                    return admitsInboundCapacity(current, m_clients[current].syncConnection->inboundFrameBytes(), capacity);
            }
            return false;
        });
        client.syncConnection->setInboundAssemblyAdmission([this, clientID](std::size_t additional) {
            for (std::size_t current = 0; current < m_clients.size(); ++current) {
                if (m_clients[current].clientID == clientID)
                    return admitsAssemblyCapacity(current, additional);
            }
            return false;
        });
        client.syncConnection->setClipboardReplyAdmission([this, clientID](std::uint32_t handle, std::uint32_t requestID) {
            for (std::size_t current = 0; current < m_clients.size(); ++current) {
                if (m_clients[current].clientID == clientID)
                    return mayReceiveClipboardData(current, handle, requestID);
            }
            return false;
        });
        client.syncConnection->setNativeDragAdmission([this, clientID](std::uint32_t handle, std::uint64_t epoch) {
            return mayReceiveNativeDrag(clientID, handle, epoch);
        });
        client.syncConnection->setControlUploadReceiver(
            [this, clientID](const Protocol::Sync::BulkChunk& chunk) { return receivePasteUpload(clientID, chunk); },
            [this, clientID](std::uint32_t transfer) {
                const auto found = std::ranges::find(m_clients, clientID, &Client::clientID);
                return found != m_clients.end() && found->pasteCall && found->pasteCall->upload.transfer() == transfer;
            });
        // A call past the outstanding-call cap is answered busy where it is
        // read, so a client's flood of calls is never held.
        Protocol::Sync::answerBusyAsRead(*client.syncConnection);
        // The attachment outlives this connection: a resume on another one
        // continues it, and only the panes it held can be named.
        client.syncConnection->serveFromTable(*m_attachments, {}, client.clientID,
            [this](Protocol::Sync::ID128 paneID) -> Protocol::Sync::PaneSource* {
                const auto found = std::ranges::find_if(m_sessions, [&](const std::unique_ptr<Session>& session) { return session->screenSource && session->screenSource->paneID() == paneID; });
                return found == m_sessions.end() ? nullptr : (*found)->screenSource.get();
            });
    }
    const std::uint64_t resyncsBefore = client.syncConnection->counters().resyncsReceived;
    const Protocol::Sync::ConnectionState stateBefore = client.syncConnection->state();
    client.syncConnection->receiveStream(bytes, nowMicroseconds());
    if (index >= m_clients.size() || m_clients[index].didOverflow)
        return;
    // A client that has just attached, or gone, is a reader more or fewer.
    if (client.syncConnection->state() != stateBefore)
        m_isStructureDirty = true;
    if (client.syncConnection->state() == Protocol::Sync::ConnectionState::Closed
        && client.syncConnection->byeReason() == Protocol::Sync::ByeReason::Normal)
        client.didDetachCleanly = true;
    if (client.syncConnection->state() != Protocol::Sync::ConnectionState::Attached) {
        if (client.consentPending && client.consentAction == Client::ConsentAction::Attach)
            servicePendingAttachConsent(index);
        return;
    }
    // A Resync whose row hashes the host can compare names the first row the
    // client built differently (Docs/sync.md §8): what a journal reader
    // needs to find the op that diverged.
    const Protocol::Sync::HostCounters& counters = client.syncConnection->counters();
    if (counters.resyncsReceived > resyncsBefore && counters.lastResyncFirstRow >= 0)
        Ubiquitty::PAL::logLine(stderr, "daemon: client {} diverged at generation {}: the first row it built differently is {}", index,
            counters.lastResyncGeneration, counters.lastResyncFirstRow);
    adoptResumedPanes(index);
    if (stateBefore == Protocol::Sync::ConnectionState::Greeting && client.paneSubscriptions.empty() && client.sessionID) {
        if (Session* selected = findSession(client.sessionID))
            seedInitialAttachState(index, *selected);
    }
    closeSupersededConnections();
    if (index >= m_clients.size())
        return;
    noteAttachmentGrant(m_clients[index]);
    rekeyGeometry(m_clients[index]);
    for (const PaneSubscription& subscription : m_clients[index].paneSubscriptions) {
        Session* session = findSession(subscription.sessionID);
        if (!session)
            continue;
        notePaneFocus(*session);
        // A person whose window took the focus has seen what the pane
        // raised: its facts are read again, which is where that is noted.
        if (session->screenSource && session->screenSource->personFocusGains() != session->seenPersonFocusGains)
            noteRecordsDue(*session, nowMilliseconds());
    }
    // A Sync client presents its pixels and theme in its Facts; it reports
    // no padding, so it draws all of its text area.
    if (const Protocol::Sync::HostConnection* connection = m_clients[index].syncConnection.get()) {
        const Protocol::Sync::Facts& facts = connection->facts();
        Client::Presented presented = m_clients[index].presented;
        if (facts.cellPixelWidth > 0 && facts.cellPixelHeight > 0)
            presented.pixels = Ubiquitty::PixelGeometry { { facts.cellPixelWidth, facts.cellPixelHeight }, { facts.textAreaPixelWidth, facts.textAreaPixelHeight }, { facts.textAreaPixelWidth, facts.textAreaPixelHeight } };
        if (facts.themePalette)
            presented.palette = facts.themePalette;
        notePresented(index, std::move(presented));
    }
    adoptViewportPixels(index);
    bindScreenPanes(index);
    adoptScreenTypography(index);
    if (stateBefore != Protocol::Sync::ConnectionState::Attached && !isAgentClient(client)) {
        // The outer greeting is not observation authority. A late person is
        // asked only after Welcome and the panes routing the question exist.
        const auto now = nowMilliseconds();
        for (Client& pending : m_clients)
            if (pending.consentPending && pending.consentDeadlineMilliseconds > now
                && client.subscriptionFor(pending.consentSessionID))
                sendConsentRequestToHumans(pending, index);
        offerStructureConsents(index);
    }
    // A pane a client now shows whose source holds no frame of its terminal
    // (nobody showed it) is owed one, which the next publish takes.
    for (const PaneSubscription& subscription : client.paneSubscriptions) {
        if (subscription.screenHandle == 0 || client.syncConnection->paneView(subscription.screenHandle) != Protocol::Sync::ViewState::Live)
            continue;
        if (Session* session = findSession(subscription.sessionID); session && session->screenSource && !session->screenSource->isFrameHeld())
            session->isFrameDue = true;
    }
    if (index >= m_clients.size() || m_clients[index].didOverflow)
        return;
    (void)m_clients[index].syncConnection->drainClipboardReplies([&](std::uint32_t handle, const Protocol::Sync::ClipboardRead& request,
                                                                     Protocol::Sync::EffectClipboardStatus status, const Protocol::Sync::EffectClipboardBundle& bundle) {
        if (!mayReceiveClipboardData(index, handle, request.requestID))
            return false;
        // The admitted transfer remains owned and charged during this callback;
        // the local reply is measured before writing into its admitted output owner.
        std::array<Protocol::Sync::ClipboardServeItemView, Protocol::Sync::maxClipboardServeItems> items {};
        for (std::size_t item = 0; item < bundle.count; ++item)
            items[item] = { bundle.items[item].mime, bundle.items[item].data };
        for (std::size_t helper = 0; helper < m_clients.size(); ++helper) {
            if (m_clients[helper].clipboardToolRequestID == request.requestID
                && m_clients[helper].clipboardToolSourceClientID == m_clients[index].clientID) {
                finishClipboardTool(helper, static_cast<Protocol::Sync::ClipboardServeStatus>(status), std::span(items).first(bundle.count));
                break;
            }
        }
        return index < m_clients.size() && !m_clients[index].didOverflow;
    });
}

void Daemon::adoptViewportPixels(std::size_t index)
{
    if (index >= m_clients.size() || !m_clients[index].syncConnection)
        return;
    for (const Protocol::Sync::ViewportReport& report : m_clients[index].syncConnection->takeViewportReports()) {
        if (const std::optional<Ubiquitty::PixelGeometry> pixels = pixelsOf(report.viewport)) {
            Client& client = m_clients[index];
            const auto subscription = std::ranges::find(client.paneSubscriptions, report.handle, &PaneSubscription::screenHandle);
            if (subscription == client.paneSubscriptions.end())
                continue;
            if (subscription->pixels && subscription->pixels->cell == pixels->cell
                && subscription->pixels->textArea == pixels->textArea && subscription->pixels->drawable == pixels->drawable)
                continue;
            subscription->pixels = pixels;
            ++subscription->pixelsRevision;
            if (Session* session = findSession(subscription->sessionID)) {
                if (!isAgentClient(client) && tokenScopeAllows(client, Protocol::Sync::Scope::Drive))
                    session->presentedStandIn = geometryKey(client);
                applyOwnerPresentation(*session);
            }
        }
    }
}

void Daemon::adoptResumedPanes(std::size_t index)
{
    Client& client = m_clients[index];
    if (!client.syncConnection)
        return;
    for (const auto& [handle, paneID] : client.syncConnection->boundPanes()) {
        if (std::ranges::any_of(client.paneSubscriptions, [&](const PaneSubscription& subscription) { return subscription.screenHandle == handle; }))
            continue;
        const auto session = std::ranges::find_if(m_sessions, [&](const std::unique_ptr<Session>& candidate) { return candidate->screenSource && candidate->screenSource->paneID() == paneID; });
        if (session == m_sessions.end())
            continue;
        if (PaneSubscription* existing = client.subscriptionFor((*session)->id)) {
            existing->screenHandle = handle;
            continue;
        }
        client.paneSubscriptions.push_back({ .sessionID = (*session)->id, .screenHandle = handle });
        if (!(*session)->screenSource->isFrameHeld())
            (*session)->isFrameDue = true;
    }
}

void Daemon::closeSupersededConnections()
{
    for (const std::uint64_t connection : m_attachments->takeSuperseded()) {
        const auto found = std::ranges::find_if(m_clients, [&](const Client& client) { return client.clientID == connection; });
        if (found == m_clients.end())
            continue;
        // A connection a proof admitted that never greeted has no screen
        // connection to say Bye on: a newer proof took its place.
        if (!found->syncConnection) {
            dropClient(static_cast<std::size_t>(found - m_clients.begin()));
            continue;
        }
        found->syncConnection->supersede();
        // Its Bye leaves, then the reap pass drops it: the session it drove
        // stays, for the connection that superseded it.
        armSessionExitDeadline(*found, nowMilliseconds());
    }
}

void Daemon::bindScreenPanes(std::size_t index)
{
    if (index >= m_clients.size())
        return;
    Client& client = m_clients[index];
    if (!client.syncConnection || client.syncConnection->state() != Protocol::Sync::ConnectionState::Attached)
        return;
    // The structure that first names a pane leaves before any frame of the
    // pane (Docs/sync.md §7.8), its PaneBind included, so a client that
    // mirrors the workspace knows every pane it is sent frames of. The
    // structure is brought up to what the workspace holds now, and a pane
    // waits unbound, its frames with it, until the connection has sent that
    // revision whole. An agent reads no structure and waits for none.
    const bool isStructureAwaited = !isAgentClient(client);
    const auto isBindable = [this](const PaneSubscription& subscription) {
        const Session* session = subscription.screenHandle == 0 ? findSession(subscription.sessionID) : nullptr;
        return session && session->screenSource;
    };
    if (!std::ranges::any_of(client.paneSubscriptions, isBindable))
        return;
    if (isStructureAwaited) {
        m_isStructureDirty = true;
        syncStructures();
    }
    bool didBind = false; // bool-ok: whether this call bound a pane
    for (PaneSubscription& subscription : client.paneSubscriptions) {
        if (subscription.screenHandle != 0)
            continue;
        Session* session = findSession(subscription.sessionID);
        if (!session || !session->screenSource)
            continue;
        if (isStructureAwaited && !client.syncConnection->isWorkspaceStructureSent(hostedWorkspaceFor(*session).workspaceID))
            continue;
        subscription.screenHandle = client.syncConnection->bindPane(*session->screenSource);
        didBind = didBind || subscription.screenHandle != 0;
        // A source that does not hold its terminal's frame has nothing a
        // checkpoint may be cut from; the next publish gives it one.
        if (!session->screenSource->isFrameHeld())
            session->isFrameDue = true;
    }
    // What a bind queued goes to the client's output with it. Nothing else
    // is moved here: the answers a client is owed stay in the connection,
    // where the bound on them stops the daemon reading a client that reads
    // nothing.
    if (didBind)
        drainScreenInteractive(index);
}

void Daemon::adoptScreenTypography(std::size_t index)
{
    if (index >= m_clients.size())
        return;
    Client& client = m_clients[index];
    const bool isEligible = client.syncConnection && !client.didOverflow && !isAgentClient(client)
        && tokenScopeAllows(client, Protocol::Sync::Scope::Drive) && hasLiveStructureAttachment(client)
        && client.syncConnection->mayDrive();
    for (PaneSubscription& subscription : client.paneSubscriptions) {
        std::shared_ptr<const RichText::TypographyFacts> offer;
        if (isEligible && subscription.screenHandle
            && client.syncConnection->paneView(subscription.screenHandle) == Protocol::Sync::ViewState::Live) {
            offer = client.syncConnection->typography(subscription.screenHandle);
            if (offer && !offer->rendersBlocks)
                offer.reset();
        }
        const std::uint32_t handle = offer ? subscription.screenHandle : 0;
        if (subscription.richTextOffer == offer && subscription.richTextOfferHandle == handle)
            continue;
        subscription.richTextOffer = std::move(offer);
        subscription.richTextOfferHandle = handle;
        if (Session* session = findSession(subscription.sessionID))
            refreshRichTextMeasurer(*session);
    }
}

void Daemon::refreshRichTextMeasurer(Session& session)
{
    if (!session.term)
        return;
    ++session.richTextMeasurerElections;
    session.isRichTextMeasurerStale = false;
    const Client* selected = nullptr;
    std::shared_ptr<const RichText::TypographyFacts> selectedFacts;
    for (const Client& client : m_clients) {
        const auto* subscription = client.subscriptionFor(session.id);
        if (!subscription || !subscription->screenHandle || client.didOverflow || isAgentClient(client)
            || !tokenScopeAllows(client, Protocol::Sync::Scope::Drive) || !hasLiveStructureAttachment(client)
            || !client.syncConnection->mayDrive()
            || client.syncConnection->paneView(subscription->screenHandle) != Protocol::Sync::ViewState::Live)
            continue;
        const auto facts = client.syncConnection->typography(subscription->screenHandle);
        if (!facts || !facts->rendersBlocks)
            continue;
        if (!selected || client.clientID < selected->clientID) {
            selected = &client;
            selectedFacts = facts;
        }
        if (client.clientID == session.richTextMeasurerClientID) {
            selected = &client;
            selectedFacts = facts;
            break;
        }
    }
    const std::uint32_t selectedID = selected ? selected->clientID : 0;
    const auto authority = selectedID == session.richTextMeasurerClientID ? RichText::TypographyAuthority::Unchanged : RichText::TypographyAuthority::Changed;
    if (authority == RichText::TypographyAuthority::Unchanged && session.term->typography.facts == selectedFacts)
        return;
    // A native shaper is identified by its connection, not by equal advance
    // tables. Cancel old work before changing the epoch and its authority.
    for (const auto& pending : session.term->richTextMeasurements) {
        for (Client& client : m_clients) {
            if (client.clientID != pending.recipient || !client.syncConnection)
                continue;
            if (const auto* subscription = client.subscriptionFor(session.id))
                client.syncConnection->cancelRichText(subscription->screenHandle, pending.request.token);
        }
    }
    session.richTextMeasurerClientID = selectedID;
    RichText::adoptTypography(*session.term, selectedFacts, authority);
}

namespace {

/// Whether a pane holds rich-text measurements, a typography report or
/// replies that its program is still owed.
///
/// - Parameter terminal: The pane's terminal.
/// - Returns: False when a service pass over the pane would do nothing.
[[nodiscard]] bool owesProgramReplies(const Ubiquitty::Terminal& terminal)
{
    return !terminal.richTextMeasurements.empty() || terminal.typography.needsReport || terminal.outputLength || terminal.bulkOutput.length;
}

} // namespace

void Daemon::serviceRichTextMeasurements(std::uint64_t now)
{
    for (auto& session : m_sessions) {
        if (!session->term || !owesProgramReplies(*session->term))
            continue;
        session->clock.tick(Ticks::fromMilliseconds(now / 1000));
        refreshRichTextMeasurer(*session);
        OSC::RichTextBlocks::expireMeasurements(*session->term, session->clock.now());
        OSC::RichTextBlocks::publishMeasurements(*session->term);
        drainTermToPTY(*session);
    }
}

void Daemon::unbindScreenPane(Client& client, const PaneSubscription& subscription)
{
    if (client.syncConnection && subscription.screenHandle != 0)
        client.syncConnection->unbindPane(subscription.screenHandle, 0);
    // This subscription may disappear before another service pass. Withdraw
    // its authority now, including the last viewer of an idle subscribed pane.
    if (Session* session = findSession(subscription.sessionID))
        refreshRichTextMeasurer(*session);
}

std::optional<Codec::JSON::Value> Daemon::beginHostAdministrationCall(std::size_t index, std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request, AdministrationEffects& effects)
{
    using namespace Protocol::Control;
    Client& actor = m_clients[index];
    const auto refuse = [&](ErrorCode code, std::string_view reason) { return rpcError(request.idOrNull(), code, reason); };
    const auto* method = findSyncMethod(request.method);
    if (const auto refusal = admitCall(method, request.params, callerAuthorityOf(actor)))
        return refuse(refusal->code, refusal->reason);
    if (!hasLiveStructureAttachment(actor)
        || (actor.hasToken && actor.tokenExpiryUnix && actor.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr))))
        return refuse(ErrorCode::Forbidden, "host administration requires the original live authorization");
    const auto intent = readHostAdministrationRequest(request.method, request.params);
    if (!intent)
        return refuse(ErrorCode::InvalidParameters, "host administration requires bounded selectors and a nonzero canonical daemon incarnation");
    if (intent->daemonInstanceID != m_daemonInstanceID)
        return refuse(ErrorCode::Forbidden, "the host restarted; refresh before making changes");
    auto& connection = *actor.syncConnection;
    if (intent->operation != Protocol::Sync::ManagementOperation::List) {
        const auto answerBytes = hostAdministrationMutationAnswerBytes(*intent, request.idOrNull());
        // Capacity admission, not an authorization denial: nothing is changed
        // if the full correlated answer would need bulk or exceed the queue.
        if (!answerBytes || !hostAdministrationMutationHasCapacity(*answerBytes, connection.queuedBytes(), connection.limits().maxQueuedBulkBytes))
            return refuse(ErrorCode::Busy, "the correlated administration answer does not fit inline or in the current queue; use a shorter call id or retry after queued answers leave");
    }
    const auto previousDisconnects = effects.clientsToDisconnect.size();
    auto reply = performHostAdministration(actor, *intent, effects);
    auto answer = hostAdministrationResponse(*intent, reply, request.idOrNull());
    if (!answer) {
        reply = {};
        reply.daemonInstanceID = m_daemonInstanceID;
        reply.status = Protocol::Sync::ManagementStatus::Failed;
        reply.detail = "The management snapshot exceeds its size limit";
        answer = hostAdministrationResponse(*intent, reply, request.idOrNull());
    }
    if (!answer)
        return refuse(ErrorCode::InternalError, "the host could not encode the administration outcome");
    const auto answered = connection.answerCall(callIndex, Codec::JSON::serialize(*answer));
    if (answered == Protocol::Sync::CallAnswer::OverBulkLimit)
        connection.refuseCall(callIndex, Protocol::Sync::busyResponse(request.idOrNull()), Protocol::Sync::CallRefusal::Busy);
    // A requested disconnect has no durable authorization change to enforce
    // when its answer failed; a committed revocation must still take effect.
    if (answered != Protocol::Sync::CallAnswer::Sent && intent->operation == Protocol::Sync::ManagementOperation::DisconnectClient)
        effects.clientsToDisconnect.resize(previousDisconnects);
    return std::nullopt;
}

std::optional<Codec::JSON::Value> Daemon::beginReclaimCall(std::size_t index, std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request)
{
    using namespace Protocol::Control;
    auto& client = m_clients[index];
    const auto refuse = [&](ErrorCode code, std::string_view text) { return rpcError(request.idOrNull(), code, text); };
    if (const auto refusal = admitCall(findSyncMethod(request.method), request.params, callerAuthorityOf(client)))
        return refuse(refusal->code, refusal->reason);
    const auto intent = readReclaimRequest(request.params);
    if (!intent || !request.id.isString())
        return refuse(ErrorCode::InvalidParameters, "reclaim requires an exact daemon pin, bound handle and string request id");
    if (client.reclaimCall)
        return refuse(ErrorCode::Busy, "this attachment already has a reclaim awaiting release");
    const auto found = std::ranges::find(client.paneSubscriptions, intent->handle, &PaneSubscription::screenHandle);
    const auto* session = found != client.paneSubscriptions.end() ? findSession(found->sessionID) : nullptr;
    const auto* attachment = client.syncConnection ? client.syncConnection->attachment() : nullptr;
    if (!session || !session->screenSource || !attachment || intent->daemonInstanceID != m_daemonInstanceID)
        return refuse(ErrorCode::NotFound, "the exact adopted pane is no longer bound here");
    client.reclaimCall.emplace(Client::ReclaimCall { .callIndex = callIndex,
        .target = { .handle = intent->handle, .sessionID = session->id, .daemonInstanceID = m_daemonInstanceID, .hostInstanceID = m_hostInstanceID, .attachmentID = attachment->attachmentID, .paneID = session->screenSource->paneID() },
        .role = client.syncConnection->grantedRole(),
        .scopes = client.tokenScope,
        .principal = principalOf(client),
        .requestID = request.id });
    if (!mayCompleteReclaim(client)) {
        client.reclaimCall.reset();
        return refuse(ErrorCode::Forbidden, "reclaim requires the current local attached person and an adopted pane");
    }
    const auto response = Codec::JSON::serialize(reclaimReleasedResult(request.idOrNull(), client.reclaimCall->target));
    if (!hostAdministrationMutationHasCapacity(response.size(), client.syncConnection->queuedBytes(), client.syncConnection->limits().maxQueuedBulkBytes)) {
        client.reclaimCall.reset();
        return refuse(ErrorCode::Busy, "the exact reclaim result cannot be queued inline");
    }
    return std::nullopt;
}

bool Daemon::mayCompleteReclaim(const Client& client) const
{
    if (!client.reclaimCall || !client.syncConnection)
        return false;
    const auto& held = *client.reclaimCall;
    const auto* attachment = client.syncConnection->attachment();
    const auto found = std::ranges::find(client.paneSubscriptions, held.target.handle, &PaneSubscription::screenHandle);
    const auto selected = std::ranges::find_if(m_sessions, [&](const auto& candidate) { return candidate->id == held.target.sessionID; });
    const auto* session = selected == m_sessions.end() ? nullptr : selected->get();
    const bool hasExactTarget = held.target.daemonInstanceID == m_daemonInstanceID && held.target.hostInstanceID == m_hostInstanceID
        && session && session->adopted && session->stream && session->stream->fileDescriptor() >= 0
        && session->screenSource && session->screenSource->paneID() == held.target.paneID
        && found != client.paneSubscriptions.end() && found->sessionID == held.target.sessionID
        && client.syncConnection->paneView(held.target.handle).has_value();
    const bool hasExactAuthority = attachment && attachment->attachmentID == held.target.attachmentID
        && client.syncConnection->grantedRole() == held.role && principalOf(client) == held.principal
        && client.tokenScope == held.scopes && tokenScopeAllows(client, Protocol::Sync::Scope::Observe)
        && !(client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr)));
    return admitsLocalReclaim({ .peer = localControlPeer(client), .hasLiveAttachment = hasLiveStructureAttachment(client), .isGrantedHuman = attachment && attachment->admission == Protocol::Sync::AttachmentAdmission::Granted && attachment->kind == Protocol::Sync::ClientKind::Human && !isAgentClient(client), .hasCurrentAuthority = hasExactAuthority, .hasExactAdoptedTarget = hasExactTarget });
}

void Daemon::completeReclaimCall(std::size_t index)
{
    auto& client = m_clients[index];
    if (!client.reclaimCall)
        return;
    const bool isAuthorized = mayCompleteReclaim(client);
    auto held = std::move(*client.reclaimCall);
    client.reclaimCall.reset();
    if (!client.syncConnection || client.didOverflow)
        return;
    auto response = Codec::JSON::serialize(isAuthorized
            ? Protocol::Control::reclaimReleasedResult(&held.requestID, held.target)
            : Protocol::Control::rpcError(&held.requestID, Protocol::Control::ErrorCode::Forbidden, "the reclaim's original local owner or adopted target is no longer current"));
    if (!Protocol::Control::hostAdministrationMutationHasCapacity(response.size(), client.syncConnection->queuedBytes(), client.syncConnection->limits().maxQueuedBulkBytes)) {
        client.syncConnection->refuseCall(held.callIndex, Protocol::Sync::busyResponse(&held.requestID), Protocol::Sync::CallRefusal::Busy);
        return;
    }
    if (client.syncConnection->answerCall(held.callIndex, std::move(response)) != Protocol::Sync::CallAnswer::Sent || !isAuthorized)
        return;
    // No callback or event-loop hop separates the checked owner, the queued
    // Result and release. Unbinding appends behind that Result on this owner.
    reclaimSession(index, held.target.sessionID);
}

Protocol::Sync::ManagementReply Daemon::performHostAdministration(const Client& actor, const Protocol::Sync::ManagementRequest& request, AdministrationEffects& effects)
{
    using namespace Protocol::Sync;
    bool& shouldReverifyTokens = effects.shouldReverifyTokens;
    auto& clientsToDisconnect = effects.clientsToDisconnect;
    ManagementReply reply;
    reply.requestID = request.requestID;
    reply.daemonInstanceID = m_daemonInstanceID;
    reply.status = ManagementStatus::Invalid;
    if (request.daemonInstanceID != m_daemonInstanceID) {
        reply.detail = "The host restarted; refresh before making changes";
    } else if (request.operation == ManagementOperation::List) {
        reply.status = ManagementStatus::Success;
        for (const auto& client : m_clients) {
            if (!client.helloSeen || client.didOverflow || client.clientID == actor.clientID) {
                continue;
            }
            ManagedClient record;
            record.clientID = client.clientID;
            record.sessionID = client.sessionID;
            record.attachedUnix = client.attachedUnix;
            switch (capabilityOriginOf(client)) {
            case CapabilityOrigin::Local:
                record.principal = "Local";
                break;
            case CapabilityOrigin::Agent:
                record.principal = "Agent";
                break;
            case CapabilityOrigin::Remote:
                record.principal = "Remote";
                break;
            }
            record.label = client.agentLabel.empty() ? client.peerProcess : client.agentLabel;
            record.transport = client.fileDescriptor ? "AF_UNIX" : "QUIC";
            record.identity = grantKeyFor(client);
            reply.clients.push_back(std::move(record));
        }
        // The list shows the Always grants the file holds now.
        m_grantStore.confirmStandingGrants();
        for (const auto& [key, grant] : m_grantStore.records()) {
            ManagedGrant record;
            record.identifier = "capability:" + std::to_string(std::to_underlying(key.capability)) + ":" + std::to_string(std::to_underlying(key.origin)) + ":" + key.identity;
            record.capability = std::string(Security::capabilityConsentVerb(key.capability));
            record.identity = key.identity;
            record.origin = key.origin == CapabilityOrigin::Local ? "Local" : key.origin == CapabilityOrigin::Agent ? "Agent"
                                                                                                                    : "Remote";
            record.sessionID = grant.sessionID;
            record.lifetime = grant.scope == CapabilityGrantScope::Always ? "Always" : grant.scope == CapabilityGrantScope::Session ? "Session"
                                                                                                                                    : "Once";
            reply.grants.push_back(std::move(record));
        }
        for (const auto& identity : m_alwaysAllowedAgents) {
            ManagedGrant record;
            record.identifier = "attach:" + identity;
            record.identity = identity;
            record.capability = "Attach to sessions";
            record.origin = "Agent";
            record.lifetime = "Always";
            reply.grants.push_back(std::move(record));
        }
        for (const auto& session : m_sessions) {
            if (!session)
                continue;
            for (const auto& identity : session->agentSessionGrantIdentities) {
                ManagedGrant record;
                record.identifier = "session:" + std::to_string(session->id) + ":" + identity;
                record.identity = identity;
                record.sessionID = session->id;
                record.capability = "Attach to this session";
                record.origin = "Agent";
                record.lifetime = "Session";
                reply.grants.push_back(std::move(record));
            }
        }
        ManagementMetadataStore metadata(m_tokenPath);
        if (!metadata.isValid() || m_didFailManagementHistory) {
            reply.detail = "Some activity history could not be recorded";
        }
        for (auto& grant : reply.grants) {
            const auto facts = metadata.metadata("grant:" + grant.identifier);
            grant.grantedUnix = facts.createdUnix;
            grant.grantedBy = facts.createdBy;
        }
        for (const auto& token : loadTokens(m_tokenPath)) {
            ManagedToken record;
            const auto facts = metadata.metadata("token:" + token.hashHex);
            record.createdUnix = facts.createdUnix;
            record.lastUsedUnix = facts.lastUsedUnix;
            record.lastUsedBy = facts.lastUsedBy;
            record.fingerprint = token.hashHex;
            record.label = token.label;
            record.scopes = token.scope;
            record.expiryUnix = token.expiryUnix;
            reply.tokens.push_back(std::move(record));
        }
    } else if (request.operation == ManagementOperation::TerminateSession) {
        reply.status = ManagementStatus::Refused;
        reply.detail = "Session termination requires the typed host.session.terminate call";
    } else if (request.operation == ManagementOperation::DisconnectClient) {
        reply.status = ManagementStatus::NotFound;
        reply.detail = "This client is no longer attached";
        for (auto& client : m_clients) {
            if (client.clientID != request.clientID || !client.helloSeen || client.didOverflow) {
                continue;
            }
            if (client.clientID == actor.clientID) {
                reply.status = ManagementStatus::Invalid;
                reply.detail = "The management connection cannot disconnect itself";
            } else {
                clientsToDisconnect.push_back(client.clientID);
                reply.status = ManagementStatus::Success;
                reply.detail.clear();
            }
            break;
        }
    } else if (request.operation == ManagementOperation::IssueToken || request.operation == ManagementOperation::RevokeToken) {
        TokenStoreLock tokenLock(m_tokenPath);
        if (!tokenLock.isLocked()) {
            reply.status = ManagementStatus::Failed;
            reply.detail = "The token store is busy or inaccessible; retry";
        } else {
            auto store = readTokenStore(m_tokenPath);
            if (!store.isWhole) {
                reply.status = ManagementStatus::Failed;
                reply.detail = "The token store could not be read completely; no token was changed";
                return reply;
            }
            auto records = std::move(store.records);
            const auto now = static_cast<std::uint64_t>(::time(nullptr));
            bool shouldWrite = false;
            if (request.operation == ManagementOperation::IssueToken) {
                const bool isLabelValid = !request.label.empty() && request.label.size() <= 128
                    && std::ranges::none_of(request.label, [](unsigned char byte) { return byte < 32 || byte == 127; });
                if (request.scopes.isEmpty() || !isLabelValid || !request.expiryUnix || request.expiryUnix <= now) {
                    reply.detail = "Choose a label, explicit scopes, and a future expiry";
                } else if (records.size() >= 1024) {
                    reply.detail = "Revoke an unused token before issuing another";
                } else {
                    std::array<std::uint8_t, 32> entropy;
                    if (::getentropy(entropy.data(), entropy.size()) != 0) {
                        reply.status = ManagementStatus::Failed;
                        reply.detail = "Could not obtain secure randomness";
                    } else {
                        constexpr std::string_view digits = "0123456789abcdef";
                        for (auto byte : entropy) {
                            reply.issuedToken.push_back(digits[byte >> 4]);
                            reply.issuedToken.push_back(digits[byte & 15]);
                        }
                        records.push_back({ Crypto::sha256Hex(reply.issuedToken), request.scopes, request.expiryUnix, request.label });
                        shouldWrite = true;
                    }
                }
            } else {
                const auto removed = std::erase_if(records, [&](const auto& record) { return record.hashHex == request.identity; });
                if (!removed) {
                    reply.status = ManagementStatus::NotFound;
                    reply.detail = "This token no longer exists";
                } else {
                    shouldWrite = true;
                }
            }
            if (shouldWrite) {
                const auto outcome = writeTokens(m_tokenPath, records);
                if (outcome == TokenWriteOutcome::IOError) {
                    reply.issuedToken.clear();
                    reply.status = ManagementStatus::Failed;
                    reply.detail = "Could not commit the token store";
                } else {
                    reply.status = ManagementStatus::Success;
                    reply.detail = outcome == TokenWriteOutcome::UnsupportedDurability ? "Saved; the filesystem could not confirm crash durability" : "";
                    if (request.operation == ManagementOperation::IssueToken) {
                        ManagementMetadataStore metadata(m_tokenPath);
                        if (!metadata.created("token:" + Crypto::sha256Hex(reply.issuedToken), now,
                                originOf(actor) == CapabilityOrigin::Local ? "Local owner (uid " + std::to_string(::geteuid()) + ")" : grantKeyFor(actor))) {
                            reply.detail = "Token created, but its activity history could not be recorded";
                            m_didFailManagementHistory = true;
                        }
                    }
                    shouldReverifyTokens = request.operation == ManagementOperation::RevokeToken;
                }
            }
        }
    } else if (request.operation == ManagementOperation::RevokeGrant) {
        reply.status = ManagementStatus::NotFound;
        reply.detail = "This grant no longer exists";
        std::string revokedIdentity;
        std::uint32_t revokedSessionID = 0;
        bool shouldDisconnect = false;
        // An Always grant another daemon withdrew is no longer this one's to revoke.
        m_grantStore.confirmStandingGrants();
        for (const auto& [key, grant] : m_grantStore.records()) {
            const auto identifier = "capability:" + std::to_string(std::to_underlying(key.capability)) + ":" + std::to_string(std::to_underlying(key.origin)) + ":" + key.identity;
            if (identifier != request.identity) {
                continue;
            }
            // An Always grant ends with the file's record of it: the edit
            // drops it from the store only once that withdrawal commits.
            bool didRevoke = true;
            if (grant.scope == CapabilityGrantScope::Always)
                didRevoke = editAlwaysAllowedCapabilityGrants(CapabilityGrantOperation::Withdraw, key);
            else
                m_grantStore.revoke(key.capability, key.origin, key.identity);
            if (!didRevoke) {
                reply.status = ManagementStatus::Failed;
                reply.detail = "Could not commit the permission store";
            } else {
                revokedIdentity = key.identity;
                reply.status = ManagementStatus::Success;
            }
            break;
        }
        for (auto iterator = m_alwaysAllowedAgents.begin(); reply.status == ManagementStatus::NotFound && iterator != m_alwaysAllowedAgents.end(); ++iterator) {
            if ("attach:" + *iterator != request.identity) {
                continue;
            }
            const auto identity = *iterator;
            if (!revokeAlwaysAllowedAgent(identity)) {
                reply.status = ManagementStatus::Failed;
                reply.detail = "Could not commit the permission store";
            } else {
                revokedIdentity = identity;
                shouldDisconnect = true;
                reply.status = ManagementStatus::Success;
            }
            break;
        }
        for (const auto& session : m_sessions) {
            if (reply.status != ManagementStatus::NotFound || !session)
                continue;
            const std::string prefix = "session:" + std::to_string(session->id) + ":";
            const auto found = std::ranges::find_if(session->agentSessionGrantIdentities,
                [&](const std::string& identity) { return prefix + identity == request.identity; });
            if (found == session->agentSessionGrantIdentities.end())
                continue;
            revokedIdentity = *found;
            revokedSessionID = session->id;
            session->agentSessionGrantIdentities.erase(found);
            m_grantStore.revokeSessionGrantsFor(revokedIdentity, revokedSessionID);
            shouldDisconnect = true;
            reply.status = ManagementStatus::Success;
            break;
        }
        if (reply.status == ManagementStatus::Success) {
            reply.detail.clear();
            m_consentQueue.resolveIdentity(revokedIdentity);
            if (revokedSessionID) {
                withdrawPendingGrantsForSessionOf(revokedIdentity, revokedSessionID);
            } else {
                withdrawPendingGrantsFor(revokedIdentity);
            }
            if (shouldDisconnect) {
                for (auto& client : m_clients) {
                    if (isAgentClient(client) && grantKeyFor(client) == revokedIdentity
                        && (!revokedSessionID || client.sessionID == revokedSessionID || client.subscriptionFor(revokedSessionID)
                            || (client.consentPending && client.consentAction == Client::ConsentAction::Attach && client.consentSessionID == revokedSessionID))) {
                        clientsToDisconnect.push_back(client.clientID);
                    }
                }
                const auto attachments = agentAttachmentsForSession(revokedSessionID, revokedIdentity);
                effects.attachmentsToRetire.insert(effects.attachmentsToRetire.end(), attachments.begin(), attachments.end());
            }
        }
    } else {
        reply.detail = "Unknown management operation";
    }
    return reply;
}

std::optional<Codec::JSON::Value> Daemon::beginTerminationCall(std::size_t index, std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request)
{
    using Codec::JSON::Value;
    using Protocol::Control::ErrorCode;
    Client& client = m_clients[index];
    const auto refuse = [&](ErrorCode code, std::string_view message) { return Protocol::Control::rpcError(request.idOrNull(), code, message); };
    const auto* method = Protocol::Control::findSyncMethod(request.method);
    if (const auto refusal = Protocol::Control::admitCall(method, request.params, callerAuthorityOf(client)))
        return refuse(refusal->code, refusal->reason);
    const bool isManagement = request.method == "host.session.terminate";
    if (isManagement && isAgentClient(client))
        return refuse(ErrorCode::Forbidden, "host administration requires a verified person");
    if (!hasLiveStructureAttachment(client) || client.consentPending || client.terminationCall)
        return refuse(ErrorCode::Busy, "this attachment is not available for another termination");
    if (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr)))
        return refuse(ErrorCode::Forbidden, "the original authorization has expired");
    const auto sessionID = static_cast<std::uint32_t>(request.params.numberOr("session"));
    Session* target = findSession(sessionID);
    if (!isManagement && !target && findRestorableWorkspace(sessionID)) {
        if (isAgentClient(client))
            return refuse(ErrorCode::Forbidden, "only a person may forget a remembered workspace");
        std::erase_if(m_restorableWorkspaces, [&](const auto& restorable) { return restorable.workspaceID == sessionID; });
        scheduleLayoutRecordWrite();
        auto result = Value::object();
        result.set("session", Value::number(sessionID));
        result.set("outcome", Value::string("forgotten"));
        return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
    }
    if (!target || !target->screenSource)
        return refuse(ErrorCode::NotFound, "the named session is no longer held");
    const bool needsConsent = !isManagement && isAgentClient(client) && !client.subscriptionFor(sessionID) && !agentHasStandingGrant(client, *target);
    if (needsConsent && (!sessionHasAskableHuman(sessionID) || !m_nextConsentRequestID))
        return refuse(ErrorCode::Forbidden, "no attached person can approve this exact termination");
    const auto* attachment = client.syncConnection->attachment();
    client.terminationCall = Client::TerminationCall { callIndex, sessionID, target->screenSource->paneID(), attachment->attachmentID,
        client.syncConnection->grantedRole(), isManagement ? Protocol::Sync::Scope::Manage : Protocol::Sync::Scope::Control, principalOf(client), request.id };
    if (!needsConsent) {
        completeTerminationCall(index, "terminated");
        return std::nullopt;
    }
    client.consentPending = true;
    client.consentAction = Client::ConsentAction::Terminate;
    client.consentSessionID = sessionID;
    client.consentRequestID = m_nextConsentRequestID++;
    client.consentDeadlineMilliseconds = nowMilliseconds() + consentTimeoutMilliseconds;
    sendConsentRequestToHumans(client);
    return std::nullopt;
}

void Daemon::completeTerminationCall(std::size_t index, std::string_view outcome)
{
    using Codec::JSON::Value;
    Client& client = m_clients[index];
    if (!client.terminationCall)
        return;
    const auto held = std::move(*client.terminationCall);
    client.terminationCall.reset();
    retireClientConsent(client);
    const bool wasPending = std::exchange(client.consentPending, false);
    const auto* attachment = client.syncConnection ? client.syncConnection->attachment() : nullptr;
    const Session* target = findSession(held.sessionID);
    if (!hasLiveStructureAttachment(client) || !attachment || attachment->attachmentID != held.attachmentID
        || client.syncConnection->grantedRole() != held.role || principalOf(client) != held.principal
        || !tokenScopeAllows(client, held.scope)
        || (held.scope == Protocol::Sync::Scope::Manage && isAgentClient(client))
        || (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr)))
        || !target || !target->screenSource || target->screenSource->paneID() != held.paneID)
        outcome = "refused";
    else if (wasPending && nowMilliseconds() >= client.consentDeadlineMilliseconds)
        outcome = "timed-out";
    if (!client.syncConnection || client.syncConnection->state() != Protocol::Sync::ConnectionState::Attached)
        return;
    auto result = Value::object();
    result.set("session", Value::number(held.sessionID));
    result.set("outcome", Value::string(std::string(outcome)));
    const auto answer = Protocol::Control::rpcResult(&held.requestID, std::move(result));
    const auto answered = client.syncConnection->answerCall(held.callIndex, Codec::JSON::serialize(answer));
    if (answered == Protocol::Sync::CallAnswer::OverBulkLimit)
        client.syncConnection->refuseCall(held.callIndex, Protocol::Sync::busyResponse(&held.requestID), Protocol::Sync::CallRefusal::Busy);
    if (answered != Protocol::Sync::CallAnswer::Sent)
        return;
    if (outcome == "terminated" && std::ranges::none_of(m_terminationsOwed, [&](const auto& pending) { return pending.first == held.sessionID; }))
        m_terminationsOwed.emplace_back(held.sessionID, held.paneID);
    if (wasPending) {
        Ubiquitty::Session::Event event;
        event.kind = outcome == "terminated" ? Ubiquitty::Session::EventKind::ConsentGranted : Ubiquitty::Session::EventKind::ConsentDenied;
        event.sessionID = held.sessionID;
        event.text = client.agentLabel;
        event.detail = std::string(outcome);
        recordEvent(std::move(event));
    }
}

void Daemon::serviceSyncConnection(std::size_t index, std::uint64_t now)
{
    if (index >= m_clients.size())
        return;
    if (!m_clients[index].syncConnection || m_clients[index].didOverflow)
        return;
    adoptScreenTypography(index);
    const auto clientID = m_clients[index].clientID;
    std::vector<std::uint32_t> repliedSessions;
    // Font subscriptions can produce replies without any new PTY output.
    // Drain only after ending all borrows of clients and their subscriptions.
    // A pane with no rich-text work and no owed reply has nothing to move.
    for (const auto& subscription : m_clients[index].paneSubscriptions) {
        Session* session = findSession(subscription.sessionID);
        if (!session || !session->term || !owesProgramReplies(*session->term))
            continue;
        session->clock.tick(Ticks::fromMilliseconds(now / 1000));
        OSC::RichTextBlocks::expireMeasurements(*session->term, session->clock.now());
        OSC::RichTextBlocks::publishMeasurements(*session->term);
        repliedSessions.push_back(session->id);
    }
    for (const auto& message : m_clients[index].syncConnection->takeNativeDragEvents()) {
        if (!mayReceiveNativeDrag(clientID, message.handle, message.epoch))
            continue;
        const auto& client = m_clients[index];
        const auto pane = std::ranges::find_if(client.paneSubscriptions, [&](const auto& value) { return value.screenHandle == message.handle; });
        if (pane == client.paneSubscriptions.end())
            continue;
        auto* session = findSession(pane->sessionID);
        if (!session || !session->term)
            continue;
        const auto& native = message.event;
        DragDrop::Metadata metadata;
        metadata.x = native.x;
        metadata.y = native.y;
        metadata.pixelX = native.pixelX;
        metadata.pixelY = native.pixelY;
        metadata.operation = native.operation;
        DragDrop::receive(*session->term, { .kind = native.kind, .sessionID = native.sessionID, .requestID = native.requestID, .metadata = metadata, .payload = { message.bytes.data(), message.bytes.size() }, .error = native.error, .isFinal = native.isFinal, .isSameWindow = native.isSameWindow });
        deliverSessionDragDrop(*session);
        repliedSessions.push_back(session->id);
    }
    for (auto& reply : m_clients[index].syncConnection->takeRichTextReplies()) {
        const Client& client = m_clients[index];
        const auto subscription = std::ranges::find_if(client.paneSubscriptions, [&](const auto& pane) { return pane.screenHandle == reply.handle; });
        if (subscription == client.paneSubscriptions.end())
            continue;
        Session* session = findSession(subscription->sessionID);
        if (!session || !session->term || session->richTextMeasurerClientID != client.clientID)
            continue;
        auto& pending = session->term->richTextMeasurements;
        const auto found = std::ranges::find_if(pending, [&](const auto& item) { return item.request.token == reply.token && item.recipient == client.clientID; });
        if (found == pending.end() || found->typographyEpoch != session->term->typography.epoch)
            continue;
        if (found->blockSerial) {
            (void)RichText::adoptBlockMeasurement(*session->term, *found, std::move(reply.measurement));
            pending.erase(found);
        } else {
            reply.measurement.epoch = session->term->typography.epoch;
            (void)OSC::RichTextBlocks::completeMeasurement(*session->term, reply.token, std::move(reply.measurement));
        }
        repliedSessions.push_back(session->id);
    }
    // Draining can retire attachments. Finish using the client references
    // first, then resolve the same client again before serving its frames.
    for (const auto& sessionID : repliedSessions) {
        if (Session* session = findSession(sessionID)) {
            OSC::RichTextBlocks::publishMeasurements(*session->term);
            drainTermToPTY(*session);
            OSC::RichTextBlocks::publishMeasurements(*session->term);
        }
    }
    if (index >= m_clients.size() || m_clients[index].clientID != clientID || m_clients[index].didOverflow || !m_clients[index].syncConnection)
        return;
    const auto actorID = m_clients[index].clientID;
    AdministrationEffects effects;
    serviceSyncConnection(index, now, effects);
    if (!effects.isPending()) {
        retireStandingConsentWithdrawals();
        return;
    }
    // No borrowed Client or HostConnection crosses flushClient, kickClient
    // or token revalidation: each may erase a client after a failed write.
    const auto actor = std::ranges::find(m_clients, actorID, &Client::clientID);
    if (actor != m_clients.end() && actor->syncConnection && !actor->didOverflow) {
        const auto actorIndex = static_cast<std::size_t>(actor - m_clients.begin());
        std::vector<std::uint8_t> frame;
        // Mutations were admitted only with an inline answer. Move interactive
        // frames only, never force-drain or bypass pacing for queued bulk.
        while (actor->syncConnection->popInteractiveFrame(frame)) {
            queueToClient(actorIndex, frame);
            frame.clear();
            if (actor->didOverflow)
                break;
        }
        flushClient(actorIndex);
    }
    for (const auto clientID : effects.clientsToDisconnect) {
        const auto client = std::ranges::find(m_clients, clientID, &Client::clientID);
        if (client != m_clients.end())
            kickClient(static_cast<std::size_t>(client - m_clients.begin()));
    }
    for (const auto attachmentID : effects.attachmentsToRetire)
        retireAttachmentGrant(attachmentID);
    if (effects.shouldReverifyTokens)
        reverifyTokenClients();
    retireStandingConsentWithdrawals();
}

void Daemon::serviceSyncConnection(std::size_t index, std::uint64_t now, AdministrationEffects& effects)
{
    if (index >= m_clients.size())
        return;
    Client& client = m_clients[index];
    if (!client.syncConnection || client.didOverflow)
        return;
    if (client.writeFailureDeadlineMilliseconds) {
        drainScreenInteractive(index);
        return;
    }
    Protocol::Sync::HostConnection& connection = *client.syncConnection;
    for (const auto& subscription : m_clients[index].paneSubscriptions) {
        Session* session = findSession(subscription.sessionID);
        if (!session || !session->term || !subscription.screenHandle || session->richTextMeasurerClientID != client.clientID)
            continue;
        std::array<std::uint32_t, RichText::maxPendingMeasurements> tokens {};
        std::size_t tokenCount = 0;
        for (auto& pending : session->term->richTextMeasurements) {
            if (pending.completion || pending.failure != RichText::MeasurementFailure::None)
                continue;
            const auto remaining = RichText::measurementWaitMicroseconds(pending, session->clock.now());
            if (!remaining)
                continue;
            tokens[tokenCount++] = pending.request.token;
            if (!pending.recipient && connection.requestRichText(subscription.screenHandle, pending.request, now, remaining)) {
                pending.recipient = client.clientID;
                pending.expiresAt = now + remaining;
            }
        }
        connection.retainRichTextRequests(subscription.screenHandle, std::span(tokens).first(tokenCount));
    }
    releaseIdleImageCarrierCapacity();
    if (connection.state() != Protocol::Sync::ConnectionState::Attached) {
        servicePendingAttachConsent(index);
        if (client.didOverflow)
            return;
        if (connection.state() != Protocol::Sync::ConnectionState::Attached) {
            connection.service(now);
            drainSyncConnection(index, now);
            return;
        }
    }
    // The admission query always keeps room for the two bounded carrier
    // vectors below, including after the last chunk releases its source.
    std::erase_if(client.pendingConsentRetirements, [&](const Protocol::Sync::ConsentResolution& resolution) {
        const auto notification = Protocol::Sync::consentResolvedNotify(resolution);
        return notification && connection.sendNotify(*notification);
    });
    // A tree edit from an agent remains an outstanding call while a person
    // decides. Every other call is answered in this pass.
    Protocol::Sync::serveCallsDeferred(connection, [this, index, &client, &effects](std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request) -> std::optional<Codec::JSON::Value> {
        if (effects.isPending())
            return Protocol::Control::rpcError(request.idOrNull(), Protocol::Control::ErrorCode::Busy, "the preceding administration outcome is being delivered; retry after it arrives");
        if (Protocol::Control::isHostAdministrationMethod(request.method))
            return beginHostAdministrationCall(index, callIndex, request, effects);
        if (request.method == "session.terminate" || request.method == "host.session.terminate")
            return beginTerminationCall(index, callIndex, request);
        if (request.method == "paste.push")
            return beginPasteCall(index, callIndex, request);
        if (request.method == "pane.reclaim")
            return beginReclaimCall(index, callIndex, request);
        if (isAgentClient(client) && Protocol::Sync::isStructureMethod(request.method))
            return beginStructureConsent(index, callIndex, request);
        return serveScreenCall(client, request);
    });
    // The queued administration answer precedes its authority changes. Held
    // operations and disclosures wait until the outer owner applies them.
    if (effects.isPending())
        return;
    servicePasteUpload(index);
    if (!serviceEventSubscriber(index) || !serviceScreenAccessSubscriber(index)) {
        dropClient(index);
        return;
    }
    // A client whose output already holds a frame is not encoded for: what
    // would be encoded now could only leave behind what is queued, and by
    // then a newer frame may stand in for it.
    const std::uint64_t withheldBefore = connection.counters().refusedCheckpointsWithheld;
    // A pane the connection unbound for its checkpoint's size and bound again
    // once it fit keeps its subscription under the new handle; one whose
    // subscription has gone since is let go.
    const auto adoptRebinds = [&] {
        for (const auto& rebind : connection.takePaneRebinds()) {
            const auto subscription = std::ranges::find(client.paneSubscriptions, rebind.previousHandle, &PaneSubscription::screenHandle);
            if (subscription != client.paneSubscriptions.end())
                subscription->screenHandle = rebind.handle;
            else
                connection.unbindPane(rebind.handle, std::to_underlying(Protocol::Sync::PaneUnbindReason::Ended));
        }
    };
    if (outboundPendingBytes(client) < screenOutputLowWaterBytes) {
        // Who else shows the client's panes, and who owns their size.
        Protocol::Sync::serveNotifies(connection);
        connection.service(now);
        adoptRebinds();
    }
    // A pane that waited for the structure naming it is bound once that has
    // left, and served in the same pass.
    if (std::ranges::any_of(client.paneSubscriptions, [](const PaneSubscription& subscription) { return subscription.screenHandle == 0; })) {
        bindScreenPanes(index);
        if (index >= m_clients.size() || m_clients[index].didOverflow)
            return;
        if (outboundPendingBytes(client) < screenOutputLowWaterBytes) {
            connection.service(now);
            adoptRebinds();
        }
    }
    // A client that refused a pane's checkpoint is not sent the same state
    // again; the journal says so, since the pane waits for its next change.
    if (connection.counters().refusedCheckpointsWithheld > withheldBefore)
        Ubiquitty::PAL::logLine(stderr, "daemon: client {} refused a checkpoint; the same state is withheld until its pane changes ({} withheld)", index,
            connection.counters().refusedCheckpointsWithheld);
    drainSyncConnection(index, now);
}

void Daemon::drainSyncConnection(std::size_t index, std::uint64_t now)
{
    if (index >= m_clients.size() || !m_clients[index].syncConnection || m_clients[index].didOverflow)
        return;
    Client& client = m_clients[index];
    Protocol::Sync::HostConnection& connection = *client.syncConnection;
    std::vector<std::uint8_t> frame;
    frame.reserve(Protocol::Sync::maxEnvelopeHeaderBytes + Protocol::Sync::maxFrameBodyBytes);
    while (outboundPendingBytes(client) < screenOutputLowWaterBytes) {
        frame.clear();
        if (!connection.popStreamFrame(frame, now))
            break;
        queueToClient(index, frame);
        if (client.didOverflow)
            return;
    }
    // Moving the queue must not make its payload allocations disappear from
    // admission while QUIC copies them into its own retained datagrams.
    std::optional<Protocol::Sync::BulkSourceReservation> datagramScratch;
    const auto datagrams = connection.takeDatagrams();
    if (isAgentClient(client)) {
        for (auto& subscription : client.paneSubscriptions) {
            if (!subscription.hasReportedScreenAccess && connection.hasDisclosedScreen(subscription.screenHandle)) {
                if (const Session* session = findSession(subscription.sessionID)) {
                    recordScreenAccess(client, *session);
                    subscription.hasReportedScreenAccess = true;
                }
            }
        }
    }
    std::size_t datagramCapacity = 0;
    for (const auto& datagram : datagrams)
        datagramCapacity += datagram.capacity();
    if (datagramCapacity)
        datagramScratch = reserveImageCarrierScratch(client.clientID, datagramCapacity);
    for (const std::vector<std::uint8_t>& datagram : datagrams) {
#ifdef UBIQUITTY_QUIC
        if (client.quicConnection && m_quic) {
            const auto admission = imageSourceAdmission(client.clientID);
            if (admission.connection && admission.aggregate
                && (datagram.size() > admission.connection->availableBytes(admission.otherConnectionBytes)
                    || datagram.size() > admission.aggregate->availableBytes(admission.otherAggregateBytes)))
                preemptOutboundImages();
            m_quic->sendDatagram(client.quicConnection, datagram);
        }
#else
        (void)datagram;
#endif
    }
    // A connection that closed has nothing left to say but its Bye; once that
    // is in the output the client goes with it.
    if (connection.state() == Protocol::Sync::ConnectionState::Closed && !connection.hasQueuedStreamFrames()) {
        flushClient(index);
        if (client.didOverflow)
            return;
        if (outboundRetainedBytes(client)) {
            if (!client.sessionExitDeadlineMilliseconds)
                armSessionExitDeadline(client, nowMilliseconds());
            return;
        }
        dropClient(index);
        return;
    }
    // A path that is really gone delivers nothing, but one the client still
    // reads, slowed rather than cut, learns why it was dropped. Either way the
    // client goes, its sessions stay, and it reattaches on a path of its own.
    if (connection.state() == Protocol::Sync::ConnectionState::Lost)
        dropWithFarewell(index, Protocol::Sync::ByeReason::Silent, "The host heard nothing from this connection while it awaited an answer.");
}

Codec::JSON::Value Daemon::serveScreenCall(Client& client, const Protocol::Control::DecodedRequest& request)
{
    using Protocol::Control::ErrorCode;
    // The one check at the operation boundary: who may call it, from where,
    // under what scope and with what parameters, before any effect. A
    // refused caller is answered and stays attached.
    const Protocol::Control::MethodDescriptor* method = Protocol::Control::findSyncMethod(request.method);
    if (const std::optional<Protocol::Control::CallRefusal> refusal = Protocol::Control::admitCall(method, request.params, callerAuthorityOf(client)))
        return Protocol::Control::rpcError(request.idOrNull(), refusal->code, refusal->reason);
    // Routing and subscriptions belong only to the calling connection.
    if (method->target == Protocol::Control::CallTarget::Connection) {
        if (!hasLiveStructureAttachment(client)
            || (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr))))
            return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::Forbidden, "connection calls require the current live attachment");
        if (method->name == "events.subscribe") {
            const auto subscription = Protocol::Control::eventSubscription(request.params);
            if (!subscription)
                return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InvalidParameters, "after is an exact decimal cursor; fromLatest requires after zero");
            client.eventReplay.begin(*subscription, m_events.latestCursor());
            auto result = Codec::JSON::Value::object();
            result.set("cursor", Codec::JSON::Value::string(std::to_string(client.eventReplay.cursor())));
            return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
        }
        if (method->name != "client.clipboardServing")
            return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::MethodNotFound, "the connection serves no such method");
        const bool enabled = request.params.boolOr("enabled");
        client.intents.set(Protocol::Sync::HelloIntent::ServesClipboard, enabled);
        if (!enabled)
            cancelClipboardTools(client.clientID);
        auto result = Codec::JSON::Value::object();
        result.set("enabled", Codec::JSON::Value::boolean(enabled));
        return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
    }
    // A call may change what a structure says.
    if (request.method != "host.refresh") {
        m_isStructureDirty = true;
    }
    // The entry says what the call is addressed to, not the parameters.
    if (method->target == Protocol::Control::CallTarget::Host)
        return serveHostCall(client, *method, request);
    const Codec::JSON::Value* pane = request.params.find("pane");
    if (!pane || !pane->isNumber() || pane->asNumber() < 1 || pane->asNumber() > static_cast<double>(Protocol::Sync::maxChannel)
        || pane->asNumber() != static_cast<double>(static_cast<std::uint32_t>(pane->asNumber())))
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InvalidParameters, "pane is the handle of a pane bound to this connection");
    const auto handle = static_cast<std::uint32_t>(pane->asNumber());
    const auto subscription = std::ranges::find(client.paneSubscriptions, handle, &PaneSubscription::screenHandle);
    Session* session = subscription != client.paneSubscriptions.end() ? findSession(subscription->sessionID) : nullptr;
    if (!session || !session->term)
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::NotFound, "no pane is bound to that handle");
    if (method->name == "pane.processes") {
        std::array<PAL::Process::Record, 128> records;
        const int count = session->stream ? session->stream->process().gatherProcesses(records) : -1;
        const auto walked = std::span<const PAL::Process::Record>(records).first(static_cast<std::size_t>(std::clamp(count, 0, 128)));
        return Protocol::Control::rpcResult(request.idOrNull(), Protocol::Control::processSnapshot(handle, walked, count >= 0));
    }
    if (method->name == "file.read")
        return serveFileRead(client, *session, handle, request);
    if (method->name == "pane.clear") {
        const auto name = request.params.stringOr("command");
        const auto command = name == "buffer" ? Screen::ClearCommand::Buffer
            : name == "scrollback"            ? Screen::ClearCommand::Scrollback
            : name == "screen"                ? Screen::ClearCommand::Screen
                                              : Screen::ClearCommand::ToPreviousMark;
        Screen::runClearCommand(command, session->term.get(), nullptr,
            [this, session, clientID = client.clientID](std::span<const char> bytes) {
                if (!session->stream || bytes.empty())
                    return;
                const auto sender = std::ranges::find(m_clients, clientID, &Client::clientID);
                if (sender == m_clients.end())
                    return;
                stampInputOrigin(*session, *sender);
                session->stream->writeAll(bytes);
            });
        session->isFrameDue = true;
        auto result = Codec::JSON::Value::object();
        result.set("pane", Codec::JSON::Value::number(handle));
        result.set("command", Codec::JSON::Value::string(name));
        return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
    }
    if (method->name == "pane.search")
        return Protocol::Sync::searchPane(*session->term, request);
    if (method->name == "pane.pinGeometry")
        return pinPaneGeometry(client, *session, request);
    if (method->name == "pane.overrideGeometry")
        return overridePaneGeometry(client, *session, request);
    // A method the registry gives a Sync name answers here; one with no
    // branch is a registry entry that outran the host.
    if (method->name != "pane.capture")
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::MethodNotFound, "the host serves no such method");
    if (request.params.stringOr("source", "screen") == "screen"
        && !request.params.find("start") && !request.params.find("end") && !request.params.find("lines")) {
        RichText::requestBlockMeasurements(*session->term);
        recordScreenAccess(client, *session);
        return Protocol::Control::rpcResult(request.idOrNull(), Protocol::Control::captureScreen(*session->term, handle));
    }
    // Range selection and extraction share one host snapshot. A tail does not
    // race a separate extent query against output, reflow or history eviction.
    const std::string source = request.params.stringOr("source", "screen");
    int first = Ubiquitty::Screen::scrollbackCount(session->term.get());
    const int rows = session->term->rows;
    const int columns = session->term->columns;
    const int total = first + rows;
    int requestedLast = total - 1;
    const auto invalidRange = [&] {
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InvalidParameters, "source requires only its own range parameters");
    };
    if (source == "rows") {
        const auto* start = request.params.find("start");
        const auto* end = request.params.find("end");
        if (!start || !end || request.params.find("lines"))
            return invalidRange();
        const auto from = static_cast<std::int64_t>(start->asNumber());
        const auto to = static_cast<std::int64_t>(end->asNumber());
        if (from > to || to < 0 || from >= total)
            return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::NotFound, "the requested rows are outside the retained buffer");
        first = static_cast<int>(std::max<std::int64_t>(from, 0));
        requestedLast = static_cast<int>(std::min<std::int64_t>(to, total - 1));
    } else if (source == "tail") {
        if (!request.params.find("lines") || request.params.find("start") || request.params.find("end"))
            return invalidRange();
        first = std::max(0, total - static_cast<int>(request.params.numberOr("lines")));
    } else {
        if (request.params.find("start") || request.params.find("end") || request.params.find("lines"))
            return invalidRange();
        if (source == "last-command" && !lastCommandRange(*session->term, first, requestedLast))
            return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::NotFound, "the session has no command output marked with OSC 133");
    }
    int last = requestedLast;
    std::string text;
    extractRegionText(*session->term, first, last, text);
    Codec::JSON::Value result = Codec::JSON::Value::object();
    result.set("pane", Codec::JSON::Value::number(handle));
    if (source == "screen") {
        result.set("rows", Codec::JSON::Value::number(rows));
    } else {
        result.set("source", Codec::JSON::Value::string(source));
        result.set("first", Codec::JSON::Value::number(first));
        result.set("last", Codec::JSON::Value::number(last));
        result.set("totalRows", Codec::JSON::Value::number(total));
    }
    result.set("columns", Codec::JSON::Value::number(columns));
    // The extractor returns whole rows and lowers last at its byte bound.
    // Reaching the bound on the final row still returns the complete screen.
    result.set("complete", Codec::JSON::Value::boolean(last == requestedLast));
    result.set("text", Codec::JSON::Value::string(std::move(text)));
    recordScreenAccess(client, *session);
    return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
}

Codec::JSON::Value Daemon::serveFileRead(Client& client, Session& session, std::uint32_t handle, const Protocol::Control::DecodedRequest& request)
{
    using namespace Protocol::Control;
    using Codec::JSON::Value;
    const auto path = request.params.stringOr("path");
    if (path.empty() || path.size() > maxReadFilePathBytes || path.find('\0') != std::string::npos)
        return rpcError(request.idOrNull(), ErrorCode::InvalidParameters, "path must be a bounded path without NUL");
    auto status = FileReadStatus::Denied;
    std::unique_ptr<std::uint8_t[]> bytes;
    std::size_t count = 0;
    if (client.fileRequestCount < maxFileRequestsPerClient) {
        ++client.fileRequestCount;
        // OSC 7 is program output, not authority to choose the confinement root.
        // A serial stream has no kernel cwd and therefore cannot serve files.
        const std::string cwd = session.stream ? session.stream->process().foregroundCurrentDirectory() : std::string();
        std::string realPath;
        if (resolveConfinedFile(path, cwd, status, realPath)) {
            // Pin the opened inode, refuse final-component symlink swaps and
            // never block on a FIFO swapped in after realpath. Intermediate
            // directory races remain inside the existing same-uid threat model.
            PAL::UnixFileDescriptor file(::open(realPath.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            struct stat facts {};
            if (!file || ::fstat(file.get(), &facts) != 0 || facts.st_size < 0)
                status = FileReadStatus::NotFound;
            else if (!S_ISREG(facts.st_mode))
                status = FileReadStatus::NotRegular;
            else if (static_cast<std::uint64_t>(facts.st_size) > maxReadFileBytes)
                status = FileReadStatus::TooLarge;
            else {
                preemptOutboundImages();
                count = static_cast<std::size_t>(facts.st_size);
                if (count)
                    bytes.reset(new (std::nothrow) std::uint8_t[count]);
                if (count && !bytes)
                    status = FileReadStatus::Denied;
                else {
                    for (std::size_t offset = 0; offset < count;) {
                        const auto received = ::read(file.get(), bytes.get() + offset, count - offset);
                        if (received < 0 && errno == EINTR)
                            continue;
                        if (received <= 0) {
                            status = FileReadStatus::NotFound;
                            break;
                        }
                        offset += static_cast<std::size_t>(received);
                    }
                }
            }
        }
    }
    if (status != FileReadStatus::OK) {
        count = 0;
        bytes.reset();
    }
    std::string encoded(Codec::base64EncodedLength(count), '\0');
    if (!Codec::base64Encode({ reinterpret_cast<const char*>(bytes.get()), count }, encoded))
        return rpcError(request.idOrNull(), ErrorCode::InternalError, "file encoding failed");
    PAL::logLine(stderr, "ubiquitty-host: file.read client={} bytes={} status={}",
        client.peerProcess.empty() ? "remote" : client.peerProcess, count, fileReadStatusName(status));
    auto result = Value::object();
    result.set("pane", Value::number(handle));
    result.set("path", Value::string(path));
    result.set("status", Value::string(std::string(fileReadStatusName(status))));
    result.set("bytes", Value::number(static_cast<double>(count)));
    result.set("data", Value::string(std::move(encoded)));
    return rpcResult(request.idOrNull(), std::move(result));
}

Protocol::Sync::Role Daemon::maxRoleOf(const Client& client) const
{
    using Protocol::Sync::Role;
    if (isAgentClient(client))
        return Role::Agent;
    if (originOf(client) == Ubiquitty::CapabilityOrigin::Local)
        return Role::Owner;
    return tokenScopeAllows(client, Protocol::Sync::Scope::Drive) ? Role::Driver : Role::Viewer;
}

Protocol::Control::CallerAuthority Daemon::callerAuthorityOf(const Client& client) const
{
    Protocol::Control::CallerAuthority caller;
    caller.role = client.syncConnection ? client.syncConnection->grantedRole() : Protocol::Sync::Role::Viewer;
    // A caller restricted to clients on this host is judged by where the
    // person is, so a client relayed from SSH is not one; its scopes below
    // are its authority's.
    caller.origin = capabilityOriginOf(client);
    caller.scopes = scopesOf(client);
    return caller;
}

Protocol::Sync::ScopeSet Daemon::scopesOf(const Client& client) const
{
    // The tokenless free pass is a local client's alone, as tokenScopeAllows
    // has it; stated once here so no method restates the exception.
    Protocol::Sync::ScopeSet scopes;
    for (const Protocol::Sync::Scope scope : { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive, Protocol::Sync::Scope::Control, Protocol::Sync::Scope::RegionRead, Protocol::Sync::Scope::NewSession, Protocol::Sync::Scope::FileRead, Protocol::Sync::Scope::Manage }) {
        if (tokenScopeAllows(client, scope))
            scopes.add(scope);
    }
    return scopes;
}

std::string Daemon::scopeRefusal(const Client& client, Protocol::Sync::Scope needed, std::string_view asked) const
{
    return "This token carries " + Protocol::Sync::scopeMaskToString(scopesOf(client)) + "; " + std::string(asked) + " needs " + Protocol::Sync::scopeMaskToString(needed) + ".";
}

namespace {

std::string backgroundBase64(std::span<const std::uint8_t> bytes)
{
    std::string encoded(Codec::base64EncodedLength(bytes.size()), '\0');
    // Base64 consumes the byte representation, independent of character signedness.
    const std::span<const char> characters(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!Codec::base64Encode(characters, encoded)) {
        return {};
    }
    return encoded;
}

std::string backgroundIdentity(const Protocol::Sync::PaneSource& source)
{
    const auto identity = source.paneID();
    return Ubiquitty::Session::backgroundPaneID(identity.high, identity.low);
}

} // namespace

void Daemon::loadBackgroundEvents()
{
    PAL::UnixFileDescriptor file(::open(m_backgroundEventPath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (!file && errno == ENOENT) {
        return;
    }
    struct stat status {};
    m_isBackgroundEventStorageAvailable = file && ::fstat(file.get(), &status) == 0 && S_ISREG(status.st_mode)
        && status.st_uid == ::geteuid() && (status.st_mode & 077) == 0 && status.st_size >= 0
        && static_cast<std::uint64_t>(status.st_size) <= Ubiquitty::Session::BackgroundEvents::maxFileBytes;
    std::string contents;
    if (m_isBackgroundEventStorageAvailable) {
        contents.resize(static_cast<std::size_t>(status.st_size));
        for (std::size_t offset = 0; offset < contents.size();) {
            const ssize_t count = ::read(file.get(), contents.data() + offset, contents.size() - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                m_isBackgroundEventStorageAvailable = false;
                break;
            }
            offset += static_cast<std::size_t>(count);
        }
    }
    bool isParsed = false;
    const auto state = m_isBackgroundEventStorageAvailable ? Codec::JSON::parse(contents, isParsed) : Codec::JSON::Value {};
    m_isBackgroundEventStorageAvailable = isParsed && m_backgroundEvents.restore(state);
    if (!m_isBackgroundEventStorageAvailable) {
        PAL::logLine(stderr, "daemon: background inbox {} could not be restored; the file is preserved and passive refresh is unavailable", m_backgroundEventPath);
        return;
    }
    m_writtenBackgroundEventRevision = m_backgroundEvents.revision();
    // A restarted host has no surviving terminal behind an old pane identity.
    for (const auto& event : state.find("events")->items()) {
        m_backgroundEvents.removePane(event.stringOr("paneID"));
    }
    scheduleBackgroundEventWrite();
}

void Daemon::scheduleBackgroundEventWrite()
{
    if (!m_backgroundEventPath.empty() && m_isBackgroundEventStorageAvailable && !m_backgroundEventWriteDue
        && m_writtenBackgroundEventRevision != m_backgroundEvents.revision()) {
        m_backgroundEventWriteDue = nowMilliseconds() + 1000;
    }
}

bool Daemon::writeBackgroundEvents()
{
    if (!m_isBackgroundEventStorageAvailable) {
        return false;
    }
    if (m_backgroundEventPath.empty() || m_writtenBackgroundEventRevision == m_backgroundEvents.revision()) {
        m_backgroundEventWriteDue = 0;
        return true;
    }
    const std::string contents = Codec::JSON::serialize(m_backgroundEvents.persistentState());
    if (contents.size() > Ubiquitty::Session::BackgroundEvents::maxFileBytes
        || PAL::storageWriteDurable(m_backgroundEventPath.c_str(), std::as_bytes(std::span(contents))) != PAL::DurableFileResult::Durable) {
        m_backgroundEventWriteDue = nowMilliseconds() + 30'000;
        return false;
    }
    m_writtenBackgroundEventRevision = m_backgroundEvents.revision();
    m_backgroundEventWriteDue = 0;
    return true;
}

Codec::JSON::Value Daemon::serveBackgroundRefresh(const Client& client, const Protocol::Control::DecodedRequest& request)
{
    using Codec::JSON::Value;
    using Protocol::Control::ErrorCode;
    const auto after = Ubiquitty::Session::backgroundSequence(request.params.stringOr("after", "0"));
    const auto epoch = Ubiquitty::Session::backgroundSequence(request.params.stringOr("epoch", "0"));
    const auto inventory = Ubiquitty::Session::backgroundSequence(request.params.stringOr("inventory", "0"));
    if (!after || !epoch || !inventory) {
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InvalidParameters, "epoch, after and inventory must be unsigned decimal strings");
    }
    const std::string screenPane = request.params.stringOr("screenPane");
    if (!screenPane.empty() && !Ubiquitty::Session::isValidBackgroundPaneID(screenPane)) {
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InvalidParameters, "screenPane must be a canonical pane identity");
    }
    const std::string screenRevision = request.params.stringOr("screenRevision");
    if (!screenRevision.empty() && !Ubiquitty::Session::isValidBackgroundPaneID(screenRevision)) {
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InvalidParameters, "screenRevision must be a canonical checkpoint digest");
    }
    if (!screenPane.empty() && !tokenScopeAllows(client, Protocol::Sync::Scope::RegionRead)) {
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::Forbidden, "screen prefetch requires the region scope");
    }
    const std::uint64_t now = m_wallClockMilliseconds();
    for (const auto& session : m_sessions) {
        if (session->term && session->screenSource) {
            recordProgramStatusEvents(*session);
        }
    }
    // Events are acknowledged only after the journal that owns their identity is durable.
    if (!writeBackgroundEvents()) {
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InternalError, "the host could not persist its background inbox");
    }
    const bool didReset = *epoch != 0 && *epoch != m_backgroundEvents.epoch();
    const auto page = m_backgroundEvents.since(didReset ? 0 : *after, now);
    Value result = Value::object();
    result.set("epoch", Value::string(std::to_string(m_backgroundEvents.epoch())));
    result.set("inventory", Value::string(std::to_string(m_backgroundInventoryRevision)));
    result.set("cursor", Value::string(std::to_string(page.cursor)));
    result.set("hasMore", Value::boolean(page.hasMore));
    result.set("gap", Value::boolean(didReset || page.hasGap));
    result.set("hostNowUnixMilliseconds", Value::string(std::to_string(now)));
    Value events = Value::array();
    for (const auto& event : page.events) {
        events.push(Ubiquitty::Session::backgroundEventJSON(event));
    }
    result.set("events", std::move(events));
    constexpr std::size_t responseBudget = 1024 * 1024 - 1024;
    std::size_t responseBytes = Codec::JSON::serialize(result).size();
    const auto offset = didReset || *inventory != m_backgroundInventoryRevision
        ? 0
        : static_cast<std::size_t>(request.params.numberOr("paneOffset"));
    std::size_t next = offset ? offset - 1 : 0;
    Value panes = Value::array();
    bool didOmitPanes = false;
    for (; next < m_sessions.size(); ++next) {
        Session& session = *m_sessions[next];
        if (!session.term || !session.screenSource) {
            continue;
        }
        if (panes.items().size() == 32) {
            break;
        }
        Value pane = Value::object();
        pane.set("paneID", Value::string(backgroundIdentity(*session.screenSource)));
        pane.set("sessionID", Value::number(session.id));
        std::vector<std::uint8_t> encoded;
        Protocol::Sync::encodePaneFacts(encoded, Protocol::Sync::paneFactsOf(*session.term, paneHostFactsOf(session)));
        pane.set("facts", Value::string(backgroundBase64(encoded)));
        const auto records = Protocol::Sync::statusRecordsOf(*session.term);
        Value statuses = Value::array();
        std::size_t first = 0;
        do {
            encoded.clear();
            first = Protocol::Sync::encodeStatusFragment(encoded, records, first);
            statuses.push(Value::string(backgroundBase64(encoded)));
        } while (first < records.store.records.size());
        pane.set("status", std::move(statuses));
        const std::size_t paneBytes = Codec::JSON::serialize(pane).size() + 1;
        if (paneBytes > responseBudget - 1024) {
            didOmitPanes = true;
            continue;
        }
        if (responseBytes + paneBytes > responseBudget) {
            break;
        }
        responseBytes += paneBytes;
        panes.push(std::move(pane));
    }
    result.set("panes", std::move(panes));
    result.set("nextPaneOffset", Value::number(next < m_sessions.size() ? static_cast<double>(next + 1) : 0));
    result.set("panesOmitted", Value::boolean(didOmitPanes));
    if (!screenPane.empty()) {
        const auto found = std::ranges::find_if(m_sessions, [&](const auto& session) {
            return session->screenSource && session->term && backgroundIdentity(*session->screenSource) == screenPane;
        });
        if (found != m_sessions.end()) {
            const auto& terminal = *(*found)->term;
            // A passive reader never forces publication or closes the program's synchronized redraw.
            if (!Modes::SyncOutput::isActive(&terminal) && terminal.rows > 0 && terminal.columns > 0
                && terminal.runTextStore.liveCodePoints() <= 32'768
                && static_cast<std::uint64_t>(terminal.rows) * static_cast<std::uint64_t>(terminal.columns) <= 32'768) {
                Protocol::Sync::ScreenPublisher publisher;
                Protocol::Sync::AttachmentBaseline baseline;
                std::vector<std::uint8_t> checkpoint;
                if (publisher.capture(terminal) && publisher.checkpoint(terminal, 1, checkpoint, baseline) && checkpoint.size() <= 256 * 1024) {
                    const auto digest = Protocol::Sync::digestOf(checkpoint);
                    const std::string revision = Ubiquitty::Session::backgroundPaneID(digest.high, digest.low);
                    if (request.params.stringOr("screenRevision") != revision) {
                        Value screen = Value::object();
                        screen.set("paneID", Value::string(screenPane));
                        screen.set("revision", Value::string(revision));
                        screen.set("checkpoint", Value::string(backgroundBase64(checkpoint)));
                        if (responseBytes + Codec::JSON::serialize(screen).size() <= responseBudget) {
                            result.set("screen", std::move(screen));
                        }
                    }
                }
            }
        }
    }
    return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
}

Codec::JSON::Value Daemon::serveSharedWorkspace(const Client& client, const Protocol::Control::DecodedRequest& request)
{
    using namespace Protocol::Control;
    using namespace Ubiquitty::Session;
    const auto refuse = [&](ErrorCode code, std::string_view reason) { return rpcError(request.idOrNull(), code, reason); };
    const auto peer = localControlPeer(client);
    if (!hasLiveStructureAttachment(client) || !peer.isSameUserLocalStream || !peer.isTrustedOwnerBinary
        || peer.hasDescriptors || isAgentClient(client)
        || (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr))))
        return refuse(ErrorCode::Forbidden, "Only a trusted local person can share an adopted window.");
    if (request.params.stringOr("daemonInstanceID") != std::to_string(m_daemonInstanceID))
        return refuse(ErrorCode::Forbidden, "The host changed. Refresh before sharing this window.");
    const auto shared = readSharedWorkspace(request.params);
    if (!shared)
        return refuse(ErrorCode::InvalidParameters, "The arrangement must contain exactly one distinct adopted session per pane.");
    if (std::ranges::find(shared->sessions, client.sessionID) == shared->sessions.end())
        return refuse(ErrorCode::Forbidden, "The publishing connection must be attached to this shared window.");
    const auto fingerprint = Crypto::sha256Hex(Codec::JSON::serialize(request.params));
    const auto success = [&](std::uint32_t workspaceID) {
        auto result = Codec::JSON::Value::object();
        result.set("workspace", Codec::JSON::Value::number(workspaceID));
        return rpcResult(request.idOrNull(), std::move(result));
    };
    HostedWorkspace* completed = nullptr;
    bool isReplay = true;
    for (const auto id : shared->sessions) {
        const auto* session = findSession(id);
        auto* source = workspaceHolding(id);
        if (!session || !session->adopted || session->shellExited || !source || source->isClosing)
            return refuse(ErrorCode::NotFound, "An adopted terminal ended before the window could be shared.");
        if (const auto refusal = rearrangeRefusal(client, *source))
            return refuse(ErrorCode::Forbidden, *refusal);
        isReplay &= session->sharedWindowFingerprint == fingerprint && (!completed || completed == source);
        completed = source;
    }
    if (isReplay)
        return success(completed->workspaceID);
    HostedWorkspace* destination = nullptr;
    if (shared->destinationWorkspaceID) {
        const auto found = std::ranges::find(m_workspaces, shared->destinationWorkspaceID, [](const auto& workspace) { return workspace->workspaceID; });
        if (found == m_workspaces.end() || (*found)->isClosing)
            return refuse(ErrorCode::NotFound, "The selected workspace is no longer running. Choose another workspace.");
        destination = found->get();
        if (const auto refusal = rearrangeRefusal(client, *destination))
            return refuse(ErrorCode::Forbidden, *refusal);
    }
    for (const auto id : shared->sessions) {
        const auto* session = findSession(id);
        const auto* source = workspaceHolding(id);
        if (!session->sharedWindowFingerprint.empty() || source == destination || source->paneSessions.size() != 1)
            return refuse(ErrorCode::Forbidden, "A terminal already belongs to a shared workspace. Open that workspace instead.");
    }
    std::unique_ptr<HostedWorkspace> staged(new (std::nothrow) HostedWorkspace(m_layoutIdentities));
    if (!staged)
        return refuse(ErrorCode::Busy, "The host has no memory for the shared window.");
    if (destination) {
        staged->workspaceID = destination->workspaceID;
        staged->name = destination->name;
        staged->layout = destination->layout;
        staged->paneSessions = destination->paneSessions;
    } else {
        staged->workspaceID = shared->sessions.front();
        if (const auto refusal = renameWorkspace(*staged, shared->arrangement.name))
            return refuse(ErrorCode::InvalidParameters, *refusal);
    }
    struct Bindings {
        Daemon& daemon;
        HostedWorkspace& workspace;
        const SharedWorkspace& shared;
        std::size_t next = 0;
        std::vector<std::pair<Layout::PaneID, Layout::PaneID>> replacements;
    } bindings { *this, *staged, *shared, 0, {} };
    bindings.replacements.reserve(shared->sessions.size());
    staged->paneSessions.reserve(staged->paneSessions.size() + shared->sessions.size());
    if (!Layout::restoreFromSnapshot(shared->arrangement, staged->layout, [](void* context, Layout::PaneID pane, const Layout::PaneInfo&) {
            auto& bindings = *static_cast<Bindings*>(context);
            assert(bindings.next < bindings.shared.sessions.size());
            const auto id = bindings.shared.sessions[bindings.next++];
            const auto original = bindings.daemon.workspaceHolding(id)->paneFor(id);
            bindings.replacements.emplace_back(pane, original);
            bindings.workspace.paneSessions.emplace_back(original, id); }, &bindings, destination ? Layout::RestorePlacement::Append : Layout::RestorePlacement::FreshWorkspace) || !rebindSharedPanes(staged->layout, bindings.replacements))
        return refuse(ErrorCode::InvalidParameters, "The shared window exceeds the workspace's layout limits.");
    // All validation and staging precede publication. The daemon reactor owns
    // these books, so observers see either the original roots or the whole window.
    const auto workspaceID = staged->workspaceID;
    m_workspaces.reserve(m_workspaces.size() + m_preparedLocalAdopts + 1);
    std::erase_if(m_workspaces, [&](const auto& workspace) {
        return workspace.get() != destination && std::ranges::any_of(workspace->paneSessions, [&](const auto& binding) {
            return std::ranges::find(shared->sessions, binding.second) != shared->sessions.end();
        });
    });
    if (destination) {
        destination->layout = std::move(staged->layout);
        destination->paneSessions = std::move(staged->paneSessions);
    } else {
        destination = staged.get();
        m_workspaces.push_back(std::move(staged));
    }
    for (const auto id : shared->sessions) {
        auto* session = findSession(id);
        session->sharedWindowFingerprint = fingerprint;
        session->workspaceName = destination->name;
        const auto pane = destination->paneFor(id);
        const auto surface = destination->layout.surfaceHolding(pane);
        session->placement = { surface, destination->layout.tabHolding(surface, pane), pane };
    }
    for (auto& attached : m_clients) {
        if (std::ranges::find(shared->sessions, attached.sessionID) != shared->sessions.end())
            attached.workspaceID = workspaceID;
    }
    admitWorkspacePanes(*destination);
    scheduleLayoutRecordWrite(*destination);
    return success(workspaceID);
}

Codec::JSON::Value Daemon::serveHostCall(const Client& client, const Protocol::Control::MethodDescriptor& method, const Protocol::Control::DecodedRequest& request)
{
    using Protocol::Control::ErrorCode;
    if (method.name == "workspace.share")
        return serveSharedWorkspace(client, request);
    if (method.name == "client.label") {
        if (const auto refusal = Protocol::Control::admitCall(&method, request.params, callerAuthorityOf(client)))
            return Protocol::Control::rpcError(request.idOrNull(), refusal->code, refusal->reason);
        if (!hasLiveStructureAttachment(client) || !client.clientID
            || (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr))))
            return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::Forbidden, "client.label requires the original live attachment");
        const auto workspace = std::ranges::find_if(m_workspaces, [&](const auto& held) { return held->workspaceID == client.workspaceID && !held->isClosing; });
        if (workspace == m_workspaces.end() || !std::ranges::any_of(client.paneSubscriptions, [&](const PaneSubscription& subscription) {
                return findSession(subscription.sessionID) && (*workspace)->paneFor(subscription.sessionID) != Ubiquitty::Session::Layout::invalidPane;
            }))
            return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::Forbidden, "client.label requires an attached workspace");
        if (const auto refusal = rearrangeRefusal(client, **workspace))
            return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::Forbidden, *refusal);
        const Identity::ObjectID self { Identity::ObjectKind::Client, client.clientID };
        const std::string label = request.params.stringOr("label");
        if (const auto verdict = putLabel(self, label); verdict != Identity::LabelVerdict::Accepted)
            return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InvalidParameters, Identity::labelVerdictSentence(verdict));
        auto result = Codec::JSON::Value::object();
        result.set("client", Codec::JSON::Value::string(Identity::format(self)));
        result.set("label", Codec::JSON::Value::string(label));
        return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
    }
    if (method.name == "host.refresh") {
        return serveBackgroundRefresh(client, request);
    }
    if (method.name == "host.methods")
        return Protocol::Control::rpcResult(request.idOrNull(), Protocol::Control::listSyncMethods(callerAuthorityOf(client)));
    if (method.name == "consent.respond")
        return serveConsentResponse(client, request);
    if (method.name == "consent.revoke")
        return serveConsentRevocation(client, request);
    if (method.name == "host.sessions.subscribe" || method.name == "host.sessions.unsubscribe") {
        const auto index = static_cast<std::size_t>(&client - m_clients.data());
        return Protocol::Control::rpcResult(request.idOrNull(), serveListing(index, method.name == "host.sessions.subscribe" ? ListingChange::Begin : ListingChange::End));
    }
    if (Protocol::Sync::isStructureMethod(method.name))
        return serveStructureCall(static_cast<std::size_t>(&client - m_clients.data()), method, request);
    if (method.name == "host.describe") {
        Codec::JSON::Value result = Codec::JSON::Value::object();
        result.set("wireVersion", Codec::JSON::Value::number(m_wireVersion));
        result.set("appVersion", Codec::JSON::Value::string(std::string(Protocol::Sync::appVersion())));
        result.set("scopes", Protocol::Control::scopeNames(scopesOf(client)));
        return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
    }
    // A host method the registry names answers here; one with no branch is a
    // registry entry that outran the host.
    return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::MethodNotFound, "the host serves no such method");
}

bool Daemon::hasLiveStructureAttachment(const Client& client) const
{
    using namespace Protocol::Sync;
    if (!client.helloSeen || client.didOverflow || client.sessionExitDeadlineMilliseconds
        || !client.syncConnection || client.syncConnection->state() != ConnectionState::Attached)
        return false;
    const AttachmentRecord* attachment = client.syncConnection->attachment();
    return attachment && attachment->state == AttachmentState::Live && attachment->servingConnection == client.clientID;
}

bool Daemon::showsQuestions(const Client& client) const
{
    return client.syncConnection && !client.syncConnection->showsNoQuestions();
}

bool Daemon::mayApproveStructure(const Client& client, const HostedWorkspace& workspace) const
{
    using Protocol::Sync::Role;
    if (workspace.isClosing || !hasLiveStructureAttachment(client) || isAgentClient(client)
        || !tokenScopeAllows(client, Protocol::Sync::Scope::Drive))
        return false;
    const Role role = client.syncConnection->grantedRole();
    if (role != Role::Owner && role != Role::Driver)
        return false;
    return std::ranges::any_of(client.paneSubscriptions, [&](const PaneSubscription& subscription) {
        return workspace.paneFor(subscription.sessionID) != Ubiquitty::Session::Layout::invalidPane;
    });
}

bool Daemon::mayBeAskedToApproveStructure(const Client& client, const HostedWorkspace& workspace) const
{
    return showsQuestions(client) && mayApproveStructure(client, workspace);
}

std::optional<Codec::JSON::Value> Daemon::beginStructureConsent(std::size_t index, std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request)
{
    using Protocol::Control::ErrorCode;
    using namespace Protocol::Sync;
    Client& client = m_clients[index];
    const auto refuse = [&](ErrorCode code, std::string_view reason) -> std::optional<Codec::JSON::Value> {
        return Protocol::Control::rpcError(request.idOrNull(), code, std::string(request.method) + ": " + std::string(reason));
    };
    if (!hasLiveStructureAttachment(client))
        return refuse(ErrorCode::Forbidden, "the requesting attachment is no longer live");
    const auto* method = Protocol::Control::findSyncMethod(request.method);
    if (const auto refusal = Protocol::Control::admitCall(method, request.params, callerAuthorityOf(client)))
        return refuse(refusal->code, refusal->reason);
    if (requiresStructureRevision(request.method) && !expectedStructureRevision(request))
        return refuse(ErrorCode::InvalidParameters, "expectedRevision must be a nonzero decimal uint64 string");
    if (createsSessionForStructure(request.method) && !tokenScopeAllows(client, Protocol::Sync::Scope::NewSession))
        return refuse(ErrorCode::Forbidden, "the requesting token does not permit creating a session");
    const std::string parameters = Codec::JSON::serialize(request.params);
    if (parameters.size() > maxStructureConsentParametersBytes)
        return refuse(ErrorCode::InvalidParameters, "an approval question holds at most 32768 bytes of parameters");
    // These pure checks refuse invalid words before asking a person. The live
    // books are checked again by the operation when approval is exercised.
    if (request.method == "label.set") {
        const auto verdict = Identity::checkLabel(request.params.stringOr("label"));
        if (verdict != Identity::LabelVerdict::Accepted)
            return refuse(ErrorCode::InvalidParameters, Identity::labelVerdictSentence(verdict));
    } else if (request.method == "tags.set") {
        std::vector<std::string> names;
        for (const auto& tag : request.params.find("tags")->items())
            names.push_back(tag.asString());
        Identity::TagBook checked;
        const auto verdict = checked.set({ Identity::ObjectKind::Workspace, 1 }, names);
        if (verdict != Identity::TagVerdict::Accepted)
            return refuse(ErrorCode::InvalidParameters, Identity::tagVerdictSentence(verdict));
    } else if ((request.method == "workspace.rename" || request.method == "tab.rename") && request.params.stringOr("name").size() > Ubiquitty::Session::maxWorkspaceNameBytes) {
        return refuse(ErrorCode::InvalidParameters, "a name holds at most 4096 bytes");
    }
    const auto object = structureObjectOf(request);
    // locateObject's session case may create a standalone root. A question
    // must not create its target: only an already held workspace can be asked
    // about, and no approved operation relies on a lookup's side effect.
    if (!object || (object->kind == Identity::ObjectKind::Session && (object->number > UINT32_MAX || !workspaceHolding(static_cast<std::uint32_t>(object->number)))))
        return refuse(ErrorCode::NotFound, "the target has no live workspace");
    const auto located = locateObject(*object);
    if (!located)
        return refuse(ErrorCode::NotFound, "the target is no longer held by this host");
    HostedWorkspace& workspace = *located->workspace;
    if (isLayoutSizingMethod(request.method) && !hasObservedSizingTarget(request, workspace.layout))
        return refuse(ErrorCode::InvalidParameters, "the exact sizing target is not in the named tab and window");
    if ((request.method == "pane.moveToTab" || request.method == "tab.moveToPane" || request.method == "pane.moveToNewTab")
        && !hasObservedTransfer(request, workspace.layout))
        return refuse(ErrorCode::InvalidParameters, "the exact source and destination must remain in the same workspace");
    if ((request.method == "workspace.addWindow" || request.method == "pane.extractTab" || request.method == "pane.extractWindow" || request.method == "tab.extractWindow")
        && std::to_underlying(located->surface) != request.params.numberOr("window"))
        return refuse(ErrorCode::InvalidParameters, "the source no longer belongs to the named window");
    if ((request.method == "pane.move" || request.method == "pane.swap") && !hasObservedRelocationPair(request, workspace.layout))
        return refuse(ErrorCode::InvalidParameters, "both distinct panes must remain in the named window's active tab");
    if (request.method == "window.moveFocus"
        && (std::to_underlying(located->surface) != request.params.numberOr("window") || workspace.layout.focusedPane(located->surface) != Ubiquitty::Session::Layout::PaneID { static_cast<std::uint32_t>(object->number) }))
        return refuse(ErrorCode::Forbidden, "the observed pane is no longer focused in the named window; refresh before moving focus");
    if (std::ranges::none_of(m_clients, [&](const Client& candidate) { return mayBeAskedToApproveStructure(candidate, workspace); }))
        return refuse(ErrorCode::Forbidden, "an attached person with Drive permission must approve this exact change; no such person is attached");
    if (m_structureConsents.size() >= maxPendingStructureConsents || m_nextStructureConsentID == 0
        || std::ranges::any_of(m_structureConsents, [&](const auto& held) { return held.second.clientID == client.clientID; }))
        return refuse(ErrorCode::Forbidden, "an approval question is already pending or the host's approval queue is full");
    const AttachmentRecord* attachment = client.syncConnection->attachment();
    // Capture a fresh revision. Publishing the present tree changes no
    // operation parameters, labels, names, placement or lifetime.
    m_isStructureDirty = true;
    syncStructures();
    const auto structure = m_structureRecords.find(workspace.workspaceID);
    if (structure == m_structureRecords.end() || !structure->second.record)
        return refuse(ErrorCode::NotFound, "the target has no current structure");
    if (requiresStructureRevision(request.method)
        && !Protocol::Sync::isStructureRevisionCurrent(request.method, expectedStructureRevision(request).value_or(0), structure->second.revision, structure->second.arrangedAt))
        return refuse(ErrorCode::Forbidden, "the target's structure changed; refresh and confirm the current target before changing it");
    const std::uint32_t requestID = m_nextStructureConsentID++;
    StructureConsent question;
    question.requestID = requestID;
    question.workspaceID = workspace.workspaceID;
    question.revision = structure->second.revision;
    question.workspaceName = workspace.name;
    question.method = request.method;
    question.parameters = parameters;
    question.agentLabel = client.agentLabel;
    question.peerProcess = client.peerProcess;
    question.peerProcessID = client.peerProcessID;
    PendingStructureCall pending { .clientID = client.clientID, .attachmentID = attachment->attachmentID, .role = client.syncConnection->grantedRole(), .principal = principalOf(client), .callIndex = callIndex, .workspaceID = workspace.workspaceID, .revision = question.revision, .deadlineMilliseconds = nowMilliseconds() + structureConsentTimeoutMilliseconds, .recipients = {}, .request = request, .question = std::move(question) };
    for (Client& candidate : m_clients)
        askStructureConsent(pending, candidate, workspace);
    if (pending.recipients.empty())
        return refuse(ErrorCode::Forbidden, "no attached person could receive this approval question");
    m_structureConsents.emplace(requestID, std::move(pending));
    return std::nullopt;
}

void Daemon::askStructureConsent(PendingStructureCall& pending, Client& candidate, const HostedWorkspace& workspace)
{
    if (!mayBeAskedToApproveStructure(candidate, workspace) || !candidate.pendingConsentRetirements.empty()
        || std::ranges::find(pending.recipients, candidate.clientID, &ConsentRecipient::clientID) != pending.recipients.end())
        return;
    // Recipients that cannot answer or return are only the retirement list's
    // dead weight.
    std::erase_if(pending.recipients, [&](const ConsentRecipient& recipient) {
        return !isRecipientKept(recipient, pending.recipients, [&](const Client& client) { return mayBeAskedToApproveStructure(client, workspace); });
    });
    if (pending.recipients.size() >= maxClients)
        return;
    const auto subscription = std::ranges::find_if(candidate.paneSubscriptions, [&](const PaneSubscription& pane) { return workspaceHolding(pane.sessionID) == &workspace; });
    Protocol::Sync::StructureConsent question = pending.question;
    question.sessionID = subscription->sessionID;
    const auto notification = Protocol::Sync::structureConsentNotify(question);
    if (!notification || !candidate.syncConnection->sendNotify(*notification))
        return;
    pending.recipients.push_back({ .clientID = candidate.clientID, .sessionID = question.sessionID, .attachmentID = candidate.syncConnection->attachment()->attachmentID });
}

void Daemon::offerStructureConsents(std::size_t index)
{
    const auto now = nowMilliseconds();
    for (auto& [requestID, pending] : m_structureConsents) {
        const auto workspace = std::ranges::find(m_workspaces, pending.workspaceID, [](const auto& held) { return held->workspaceID; });
        if (now < pending.deadlineMilliseconds && workspace != m_workspaces.end())
            askStructureConsent(pending, m_clients[index], **workspace);
    }
}

Codec::JSON::Value Daemon::serveConsentResponse(const Client& approver, const Protocol::Control::DecodedRequest& request)
{
    using Codec::JSON::Value;
    using Protocol::Control::ErrorCode;
    using Protocol::Sync::ConsentKind;
    const auto refuse = [&](ErrorCode code, std::string_view reason) { return Protocol::Control::rpcError(request.idOrNull(), code, reason); };
    const auto response = Protocol::Control::readConsentResponse(request.params);
    if (!response)
        return refuse(ErrorCode::InvalidParameters, "consent.respond requires one exact question kind and decision");
    if (response->decision == Protocol::Sync::ConsentChoice::Unshown)
        return declineUnshownConsent(approver, request, *response);
    if (response->kind == ConsentKind::Structure)
        return answerStructureConsent(approver, request, response->decision);
    if (response->kind == ConsentKind::Capability) {
        const auto found = m_pendingCapabilityGrants.find(response->requestID);
        // A question past its deadline is the host's to retire, unanswered,
        // whatever answer reaches it late.
        if (found == m_pendingCapabilityGrants.end() || found->second.deadlineMilliseconds <= nowMilliseconds())
            return refuse(ErrorCode::NotFound, "that capability question is no longer pending");
        const PendingCapabilityGrant& pending = found->second;
        const bool isRecipient = std::ranges::find(pending.recipients, approver.clientID, &ConsentRecipient::clientID) != pending.recipients.end();
        if (!mayRuleOnConsentFor(approver, pending.sessionID) || !isRecipient
            || (pending.clipboardRecipientClientID && pending.clipboardRecipientClientID != approver.clientID)) {
            // A clipboard question has one recipient. When that recipient
            // answers after its own authority lapsed, nobody can complete the
            // operation, so the held payload is released now rather than at
            // the next expired-token sweep.
            if (isRecipient && pending.clipboardRecipientClientID == approver.clientID) {
                PendingCapabilityGrant held = std::move(found->second);
                m_pendingCapabilityGrants.erase(found);
                retireConsentQuestion(ConsentKind::Capability, response->requestID, held.recipients);
                m_consentQueue.resolve(held.capability, held.origin, held.identity);
                if (Session* session = findSession(held.sessionID))
                    (void)session->capabilityPolicy.takeClipboardWrite(response->requestID);
            }
            return refuse(ErrorCode::Forbidden, "only the exact live human recipient may decide this question");
        }
        PendingCapabilityGrant held = std::move(found->second);
        m_pendingCapabilityGrants.erase(found);
        retireConsentQuestion(ConsentKind::Capability, response->requestID, held.recipients);
        m_consentQueue.resolve(held.capability, held.origin, held.identity);
        Session* clipboardSession = held.clipboardRecipientClientID ? findSession(held.sessionID) : nullptr;
        std::optional<HostCapabilityPolicy::PendingClipboardWrite> clipboardWrite;
        if (held.clipboardRecipientClientID) {
            if (clipboardSession)
                clipboardWrite = clipboardSession->capabilityPolicy.takeClipboardWrite(response->requestID);
            if (!clipboardSession || !clipboardWrite
                || clipboardWrite->identity != held.identity || clipboardWrite->origin != held.origin
                || !mayCompleteClipboardWrite(*clipboardSession, held.clipboardSourceClientID, held.clipboardRecipientClientID, held.identity))
                return refuse(ErrorCode::NotFound, "the exact clipboard operation or its authority expired");
        }
        if (response->decision != Protocol::Sync::ConsentChoice::Deny) {
            CapabilityGrantScope scope = CapabilityGrantScope::OneShot;
            if (response->decision == Protocol::Sync::ConsentChoice::AllowSession)
                scope = CapabilityGrantScope::Session;
            else if (response->decision == Protocol::Sync::ConsentChoice::AllowAlways)
                scope = CapabilityGrantScope::Always;
            // Per-use capabilities cannot be silently widened by a lifetime
            // choice, and mutable executable paths never persist authority.
            if (Ubiquitty::capabilityGrantIsPerUse(held.capability, held.origin))
                scope = CapabilityGrantScope::OneShot;
            if (scope == CapabilityGrantScope::Always && !held.identity.starts_with("token:"))
                scope = CapabilityGrantScope::Session;
            // An Always grant is the file's record of it, adopted into the
            // store by the edit; one that cannot be recorded holds for this
            // session only.
            if (scope == CapabilityGrantScope::Always
                && !editAlwaysAllowedCapabilityGrants(CapabilityGrantOperation::Grant, { held.capability, held.origin, held.identity }))
                scope = CapabilityGrantScope::Session;
            if (scope != CapabilityGrantScope::Always)
                m_grantStore.grant(held.capability, held.origin, held.identity, scope, held.sessionID);
            recordManagementCreation("grant:capability:" + std::to_string(std::to_underlying(held.capability)) + ":" + std::to_string(std::to_underlying(held.origin)) + ":" + held.identity, approver);
            if (clipboardWrite && clipboardSession->capabilityPolicy.checkFor(Capability::ClipboardWrite, held.origin, held.identity) == CapabilityDecision::Allow) {
                // The exact retained operation is consumed before dispatch.
                for (ClipboardBuffer buffer : std::span(clipboardWrite->buffers).first(clipboardWrite->bufferCount))
                    clipboardSession->term->clipboardClient->writeClipboard(buffer, { clipboardWrite->bytes.get(), clipboardWrite->byteCount });
                deliverSessionEffects(*clipboardSession);
                for (std::size_t recipient = 0; recipient < m_clients.size(); ++recipient)
                    if (m_clients[recipient].clientID == held.clipboardRecipientClientID)
                        flushClient(recipient);
            }
        }
        return Protocol::Control::rpcResult(request.idOrNull(), Value::object());
    }
    const auto requester = std::ranges::find_if(m_clients, [&](const Client& candidate) {
        return candidate.consentPending && candidate.consentRequestID == response->requestID;
    });
    if (requester == m_clients.end())
        return refuse(ErrorCode::NotFound, "that exact question is no longer pending");
    const ConsentKind actualKind = requester->consentAction == Client::ConsentAction::Terminate ? ConsentKind::Terminate : ConsentKind::Attach;
    if (actualKind != response->kind)
        return refuse(ErrorCode::InvalidParameters, "the question's operation kind cannot be changed");
    if (!mayRuleOnConsentFor(approver, requester->consentSessionID))
        return refuse(ErrorCode::Forbidden, "only a live human subscribed to the question's session may decide it");
    if (std::ranges::find(requester->consentRecipients, approver.clientID, &ConsentRecipient::clientID) == requester->consentRecipients.end())
        return refuse(ErrorCode::Forbidden, "this attachment was not a recipient of the exact requested question");
    if (nowMilliseconds() >= requester->consentDeadlineMilliseconds)
        return refuse(ErrorCode::NotFound, "that exact question expired");
    const std::size_t index = static_cast<std::size_t>(requester - m_clients.begin());
    if (actualKind == ConsentKind::Terminate)
        completeTerminationCall(index, response->decision == Protocol::Sync::ConsentChoice::Deny ? "denied" : "terminated");
    else if (!retainAttachConsentDecision(index, approver, response->decision))
        return refuse(ErrorCode::Busy, "the exact attachment question cannot be decided now");
    return Protocol::Control::rpcResult(request.idOrNull(), Value::object());
}

Codec::JSON::Value Daemon::declineUnshownConsent(const Client& approver, const Protocol::Control::DecodedRequest& request, const Protocol::Control::ConsentResponse& response)
{
    using Codec::JSON::Value;
    using Protocol::Control::ErrorCode;
    using Protocol::Sync::ConsentKind;
    const auto refuse = [&](ErrorCode code, std::string_view reason) { return Protocol::Control::rpcError(request.idOrNull(), code, reason); };
    QuestionLeaving left = QuestionLeaving::NotAsked;
    switch (response.kind) {
    case ConsentKind::Capability:
        left = leaveCapabilityQuestion(response.requestID, approver.clientID, QuestionLeave::Declined);
        break;
    case ConsentKind::Structure:
        left = leaveStructureQuestion(response.requestID, approver.clientID, QuestionLeave::Declined);
        break;
    case ConsentKind::Attach:
    case ConsentKind::Terminate: {
        const auto requester = std::ranges::find_if(m_clients, [&](const Client& candidate) {
            return candidate.consentPending && candidate.consentRequestID == response.requestID;
        });
        if (requester == m_clients.end()) {
            left = QuestionLeaving::Gone;
            break;
        }
        const ConsentKind actualKind = requester->consentAction == Client::ConsentAction::Terminate ? ConsentKind::Terminate : ConsentKind::Attach;
        if (actualKind != response.kind)
            return refuse(ErrorCode::InvalidParameters, "the question's operation kind cannot be changed");
        left = leaveClientQuestion(static_cast<std::size_t>(requester - m_clients.begin()), approver.clientID, QuestionLeave::Declined);
        break;
    }
    }
    switch (left) {
    case QuestionLeaving::Gone:
        return refuse(ErrorCode::NotFound, "that exact question is no longer pending");
    case QuestionLeaving::NotAsked:
        return refuse(ErrorCode::Forbidden, "this attachment was not a recipient of the exact requested question");
    case QuestionLeaving::Left:
        break;
    }
    return Protocol::Control::rpcResult(request.idOrNull(), Value::object());
}

bool Daemon::takeRecipientOff(std::vector<ConsentRecipient>& recipients, std::uint32_t clientID, QuestionLeave how)
{
    const auto found = std::ranges::find(recipients, clientID, &ConsentRecipient::clientID);
    if (found == recipients.end())
        return false;
    switch (how) {
    case QuestionLeave::Declined:
        found->standing = ConsentRecipient::Standing::Declined;
        break;
    case QuestionLeave::Departed:
        found->standing = ConsentRecipient::Standing::Left;
        break;
    case QuestionLeave::Continued:
        found->standing = ConsentRecipient::Standing::Continued;
        break;
    }
    return true;
}

bool Daemon::isStillAsked(std::span<const ConsentRecipient> recipients)
{
    return std::ranges::any_of(recipients, [](const ConsentRecipient& recipient) { return recipient.standing == ConsentRecipient::Standing::Asked; });
}

Daemon::QuestionLeaving Daemon::leaveCapabilityQuestion(std::uint32_t requestID, std::uint32_t clientID, QuestionLeave how)
{
    const auto found = m_pendingCapabilityGrants.find(requestID);
    // A question past its deadline is the sweep's to retire, as an answer
    // that reaches it late is refused.
    if (found == m_pendingCapabilityGrants.end() || found->second.deadlineMilliseconds <= nowMilliseconds())
        return QuestionLeaving::Gone;
    if (!takeRecipientOff(found->second.recipients, clientID, how))
        return QuestionLeaving::NotAsked;
    if (!isStillAsked(found->second.recipients)) {
        // Nobody left can show it: refused now, as its deadline would, and
        // asked about again on the capability's next reach.
        PendingCapabilityGrant held = std::move(found->second);
        m_pendingCapabilityGrants.erase(found);
        retireCapabilityQuestion(requestID, held);
    }
    return QuestionLeaving::Left;
}

Daemon::QuestionLeaving Daemon::leaveStructureQuestion(std::uint32_t requestID, std::uint32_t clientID, QuestionLeave how)
{
    const auto found = m_structureConsents.find(requestID);
    if (found == m_structureConsents.end() || found->second.deadlineMilliseconds <= nowMilliseconds())
        return QuestionLeaving::Gone;
    if (!takeRecipientOff(found->second.recipients, clientID, how))
        return QuestionLeaving::NotAsked;
    // A person whose link was lost may resume and be asked again, as the
    // expiry sweep allows, and so may one whose attachment a resume continued.
    const auto workspace = std::ranges::find(m_workspaces, found->second.workspaceID, [](const auto& held) { return held->workspaceID; });
    const auto isAskable = [&](const Client& candidate) { return workspace != m_workspaces.end() && mayBeAskedToApproveStructure(candidate, **workspace); };
    if (isStillAsked(found->second.recipients) || mayAskedPersonReturn(found->second.recipients, isAskable))
        return QuestionLeaving::Left;
    PendingStructureCall pending = std::move(found->second);
    m_structureConsents.erase(found);
    // Those who declined it are still listed, and are told it is settled.
    retireStructureConsent(requestID, pending);
    if (const auto asking = std::ranges::find(m_clients, pending.clientID, &Client::clientID); asking != m_clients.end() && hasLiveStructureAttachment(*asking)) {
        const auto answer = Protocol::Control::rpcError(pending.request.idOrNull(), Protocol::Control::ErrorCode::Forbidden, "nobody attached could be shown this exact tree change");
        if (asking->syncConnection->answerCall(pending.callIndex, Codec::JSON::serialize(answer)) == Protocol::Sync::CallAnswer::OverBulkLimit)
            asking->syncConnection->refuseCall(pending.callIndex, Protocol::Sync::busyResponse(pending.request.idOrNull()), Protocol::Sync::CallRefusal::Busy);
    }
    return QuestionLeaving::Left;
}

Daemon::QuestionLeaving Daemon::leaveClientQuestion(std::size_t requesterIndex, std::uint32_t clientID, QuestionLeave how)
{
    Client& requester = m_clients[requesterIndex];
    if (!requester.consentPending)
        return QuestionLeaving::Gone;
    const bool isDecidedOrExpired = nowMilliseconds() >= requester.consentDeadlineMilliseconds || requester.attachConsentApproval || requester.attachConsentTerminal;
    if (how == QuestionLeave::Declined && isDecidedOrExpired)
        return QuestionLeaving::Gone;
    if (!takeRecipientOff(requester.consentRecipients, clientID, how))
        return QuestionLeaving::NotAsked;
    // A held answer still records departures, without turning them into a
    // refusal of the decision awaiting delivery.
    if (isDecidedOrExpired)
        return QuestionLeaving::Left;
    // A person who may return is asked again on resuming, before the deadline.
    const auto isAskable = [&](const Client& candidate) { return mayBeAskedFor(candidate, requester.consentSessionID); };
    if (isStillAsked(requester.consentRecipients) || mayAskedPersonReturn(requester.consentRecipients, isAskable))
        return QuestionLeaving::Left;
    if (requester.consentAction == Client::ConsentAction::Terminate)
        completeTerminationCall(requesterIndex, "denied");
    else
        denyAgentAttach(requesterIndex, Protocol::Sync::ConsentStatusPhase::Denied);
    return QuestionLeaving::Left;
}

void Daemon::leaveEveryQuestion(std::uint32_t clientID, QuestionLeave how)
{
    if (!clientID)
        return;
    std::vector<std::uint32_t> asked;
    for (const auto& [requestID, pending] : m_pendingCapabilityGrants) {
        if (std::ranges::find(pending.recipients, clientID, &ConsentRecipient::clientID) != pending.recipients.end())
            asked.push_back(requestID);
    }
    for (const std::uint32_t requestID : asked)
        (void)leaveCapabilityQuestion(requestID, clientID, how);
    asked.clear();
    for (const auto& [requestID, pending] : m_structureConsents) {
        if (std::ranges::find(pending.recipients, clientID, &ConsentRecipient::clientID) != pending.recipients.end())
            asked.push_back(requestID);
    }
    for (const std::uint32_t requestID : asked)
        (void)leaveStructureQuestion(requestID, clientID, how);
    for (std::size_t index = 0; index < m_clients.size(); ++index) {
        if (m_clients[index].clientID != clientID
            && std::ranges::find(m_clients[index].consentRecipients, clientID, &ConsentRecipient::clientID) != m_clients[index].consentRecipients.end())
            (void)leaveClientQuestion(index, clientID, how);
    }
}

Codec::JSON::Value Daemon::serveConsentRevocation(const Client& approver, const Protocol::Control::DecodedRequest& request)
{
    using Codec::JSON::Value;
    using Protocol::Control::ErrorCode;
    const auto refuse = [&](ErrorCode code, std::string_view reason) { return Protocol::Control::rpcError(request.idOrNull(), code, reason); };
    const auto revocation = Protocol::Control::readConsentRevocation(request.params);
    if (!revocation)
        return refuse(ErrorCode::InvalidParameters, "consent.revoke requires the exact fields of one action");
    const auto sessionID = revocation->sessionID;
    if (!mayRuleOnConsentFor(approver, sessionID))
        return refuse(ErrorCode::Forbidden, "only a live human subscribed to that session may revoke its authority");
    const auto disconnectAgents = [&](std::string_view identity) {
        // A resumed attachment may hold a different pane than its greeting
        // selected, and a lost attachment has no live client to disconnect.
        const auto attachments = agentAttachmentsForSession(sessionID, identity);
        for (std::size_t index = m_clients.size(); index-- > 0;)
            if ((m_clients[index].sessionID == sessionID || m_clients[index].subscriptionFor(sessionID)
                    || (m_clients[index].consentPending && m_clients[index].consentAction == Client::ConsentAction::Attach && m_clients[index].consentSessionID == sessionID))
                && isAgentClient(m_clients[index])
                && (identity.empty() || grantKeyFor(m_clients[index]) == identity))
                kickClient(index);
        for (const auto attachmentID : attachments)
            retireAttachmentGrant(attachmentID);
    };
    switch (revocation->action) {
    case Protocol::Sync::ConsentRevocationKind::DisconnectNow: {
        const auto target = std::ranges::find_if(m_clients, [&](const Client& candidate) {
            const auto* attachment = candidate.syncConnection ? candidate.syncConnection->attachment() : nullptr;
            return (candidate.sessionID == sessionID || candidate.subscriptionFor(sessionID)) && isAgentClient(candidate) && hasLiveStructureAttachment(candidate)
                && attachment && attachment->attachmentID == revocation->attachmentID;
        });
        if (target == m_clients.end())
            return refuse(ErrorCode::NotFound, "that exact agent attachment is no longer live on the session");
        kickClient(static_cast<std::size_t>(target - m_clients.begin()));
        break;
    }
    case Protocol::Sync::ConsentRevocationKind::StopAlways: {
        const auto target = std::ranges::find_if(m_clients, [&](const Client& candidate) {
            return (candidate.sessionID == sessionID || candidate.subscriptionFor(sessionID)) && isAgentClient(candidate) && hasLiveStructureAttachment(candidate)
                && grantKeyFor(candidate) == revocation->identity;
        });
        if (target == m_clients.end())
            return refuse(ErrorCode::NotFound, "that verified identity is not attached to this session");
        const std::string grantKey = grantKeyFor(*target);
        // Commit capability removal before the per-key attach edit. A failed
        // first edit changes neither file nor this daemon's grants; a later
        // failure never restores authority already withdrawn from either file.
        if (!editAlwaysAllowedCapabilityGrants(CapabilityGrantOperation::WithdrawIdentity, { Capability::None, CapabilityOrigin::Local, grantKey }))
            return refuse(ErrorCode::InternalError, "The capability grants could not be withdrawn.");
        m_grantStore.revokeIdentity(grantKey);
        m_consentQueue.resolveIdentity(grantKey);
        withdrawPendingGrantsFor(grantKey);
        if (!revokeAlwaysAllowedAgent(grantKey))
            return refuse(ErrorCode::InternalError, "The standing consent could not be withdrawn.");
        disconnectAgents(grantKey);
        break;
    }
    case Protocol::Sync::ConsentRevocationKind::SessionGrant: {
        Session* session = findSession(sessionID);
        const auto found = session ? std::ranges::find(session->agentSessionGrantIdentities, revocation->identity)
                                   : std::vector<std::string>::iterator {};
        if (!session || found == session->agentSessionGrantIdentities.end())
            return refuse(ErrorCode::NotFound, "that exact session grant no longer exists");
        const std::string grantKey = *found;
        session->agentSessionGrantIdentities.erase(found);
        m_grantStore.revokeSessionGrantsFor(grantKey, sessionID);
        withdrawPendingGrantsForSessionOf(grantKey, sessionID);
        disconnectAgents(grantKey);
        break;
    }
    case Protocol::Sync::ConsentRevocationKind::DisconnectAll:
        disconnectAgents({});
        break;
    }
    broadcastPresence(sessionID);
    return Protocol::Control::rpcResult(request.idOrNull(), Value::object());
}

Codec::JSON::Value Daemon::answerStructureConsent(const Client& approver, const Protocol::Control::DecodedRequest& request, Protocol::Sync::ConsentChoice decision)
{
    using Codec::JSON::Value;
    using Protocol::Control::ErrorCode;
    const auto refuse = [&](ErrorCode code, std::string_view reason) { return Protocol::Control::rpcError(request.idOrNull(), code, reason); };
    expireStructureConsents(nowMilliseconds());
    const auto requestID = static_cast<std::uint32_t>(request.params.numberOr("request"));
    const auto found = m_structureConsents.find(requestID);
    if (found == m_structureConsents.end())
        return refuse(ErrorCode::NotFound, "that exact-operation approval is no longer pending");
    const auto workspace = std::ranges::find(m_workspaces, found->second.workspaceID, [](const auto& held) { return held->workspaceID; });
    const auto recipient = std::ranges::find(found->second.recipients, approver.clientID, &ConsentRecipient::clientID);
    if (workspace == m_workspaces.end() || !mayApproveStructure(approver, **workspace)
        || recipient == found->second.recipients.end() || !mayRuleOnConsentFor(approver, recipient->sessionID))
        return refuse(ErrorCode::Forbidden, "only a verified recipient still attached with Drive permission to this workspace may answer");
    const HostedWorkspace* approvedWorkspace = workspace->get();
    // Consume first: denial, a stale target, an error applying the change and
    // a successful change each decide this question exactly once.
    PendingStructureCall pending = std::move(found->second);
    m_structureConsents.erase(found);
    retireStructureConsent(requestID, pending);
    const auto asking = std::ranges::find(m_clients, pending.clientID, &Client::clientID);
    if (asking == m_clients.end() || !hasLiveStructureAttachment(*asking))
        return refuse(ErrorCode::NotFound, "the requesting connection has left");
    Client& requester = *asking;
    const auto sendOriginal = [&](const Value& result) {
        if (requester.syncConnection->answerCall(pending.callIndex, Codec::JSON::serialize(result)) == Protocol::Sync::CallAnswer::OverBulkLimit)
            requester.syncConnection->refuseCall(pending.callIndex, Protocol::Sync::busyResponse(pending.request.idOrNull()), Protocol::Sync::CallRefusal::Busy);
    };
    const auto answerOriginal = [&](ErrorCode code, std::string_view reason) {
        sendOriginal(Protocol::Control::rpcError(pending.request.idOrNull(), code, reason));
    };
    if (decision == Protocol::Sync::ConsentChoice::Deny) {
        answerOriginal(ErrorCode::Forbidden, "an attached person denied this exact tree change");
        return Protocol::Control::rpcResult(request.idOrNull(), Value::object());
    }
    const auto* attachment = requester.syncConnection->attachment();
    const auto* method = Protocol::Control::findSyncMethod(pending.request.method);
    const auto authorityRefusal = Protocol::Control::admitCall(method, pending.request.params, callerAuthorityOf(requester));
    if (!attachment || attachment->attachmentID != pending.attachmentID || requester.syncConnection->grantedRole() != pending.role
        || principalOf(requester) != pending.principal || authorityRefusal
        || (Protocol::Sync::createsSessionForStructure(pending.request.method) && !tokenScopeAllows(requester, Protocol::Sync::Scope::NewSession))) {
        answerOriginal(ErrorCode::Forbidden, "the requesting attachment or its authority changed while approval was pending");
        return refuse(ErrorCode::Forbidden, "the requesting attachment or its authority changed while approval was pending");
    }
    const auto object = structureObjectOf(pending.request);
    const bool hasHeldTarget = object && (object->kind != Identity::ObjectKind::Session || (object->number <= UINT32_MAX && workspaceHolding(static_cast<std::uint32_t>(object->number))));
    const auto located = hasHeldTarget ? locateObject(*object) : std::nullopt;
    m_isStructureDirty = true;
    syncStructures();
    const auto structure = m_structureRecords.find(pending.workspaceID);
    // An approval is held to the arrangement the question was asked about: a
    // person who takes seconds to answer should not see it refused because a
    // shell printed meanwhile.
    const bool isAskedRevisionCurrent = structure != m_structureRecords.end()
        && Protocol::Sync::isStructureRevisionCurrent(pending.request.method, pending.revision, structure->second.revision, structure->second.arrangedAt);
    if (!located || located->workspace != approvedWorkspace || structure == m_structureRecords.end() || !structure->second.record || !isAskedRevisionCurrent) {
        answerOriginal(ErrorCode::Forbidden, "the target's structure changed while approval was pending; ask again for the current target");
        return refuse(ErrorCode::Forbidden, "the target's structure changed while approval was pending; ask again for the current target");
    }
    const Value result = serveStructureCall(static_cast<std::size_t>(asking - m_clients.begin()), *method, pending.request, &approver);
    sendOriginal(result);
    return Protocol::Control::rpcResult(request.idOrNull(), Value::object());
}

void Daemon::expireStructureConsents(std::uint64_t now)
{
    for (auto held = m_structureConsents.begin(); held != m_structureConsents.end();) {
        const PendingStructureCall& pending = held->second;
        const auto client = std::ranges::find(m_clients, pending.clientID, &Client::clientID);
        if (client == m_clients.end() || !hasLiveStructureAttachment(*client)) {
            retireStructureConsent(held->first, pending);
            held = m_structureConsents.erase(held);
            continue;
        }
        const auto workspace = std::ranges::find(m_workspaces, pending.workspaceID, [](const auto& owned) { return owned->workspaceID; });
        const auto* attachment = client->syncConnection->attachment();
        const auto* method = Protocol::Control::findSyncMethod(pending.request.method);
        const bool hasAuthority = attachment && attachment->attachmentID == pending.attachmentID && client->syncConnection->grantedRole() == pending.role
            && principalOf(*client) == pending.principal && !Protocol::Control::admitCall(method, pending.request.params, callerAuthorityOf(*client))
            && (!Protocol::Sync::createsSessionForStructure(pending.request.method) || tokenScopeAllows(*client, Protocol::Sync::Scope::NewSession))
            && workspace != m_workspaces.end()
            && (std::ranges::any_of(m_clients, [&](const Client& person) { return mayBeAskedToApproveStructure(person, **workspace); })
                || mayAskedPersonReturn(pending.recipients, [&](const Client& candidate) { return mayBeAskedToApproveStructure(candidate, **workspace); }));
        if (now < pending.deadlineMilliseconds && hasAuthority) {
            ++held;
            continue;
        }
        const auto answer = Protocol::Control::rpcError(pending.request.idOrNull(), Protocol::Control::ErrorCode::Forbidden,
            hasAuthority ? "approval of this exact tree change expired without permission" : "authority for this exact tree change was withdrawn before approval");
        if (client->syncConnection->answerCall(pending.callIndex, Codec::JSON::serialize(answer)) == Protocol::Sync::CallAnswer::OverBulkLimit)
            client->syncConnection->refuseCall(pending.callIndex, Protocol::Sync::busyResponse(pending.request.idOrNull()), Protocol::Sync::CallRefusal::Busy);
        retireStructureConsent(held->first, pending);
        held = m_structureConsents.erase(held);
    }
}

void Daemon::retireStructureConsent(std::uint32_t requestID, const PendingStructureCall& pending)
{
    retireConsentQuestion(Protocol::Sync::ConsentKind::Structure, requestID, pending.recipients);
}

void Daemon::retireClientConsent(Client& client)
{
    if (!client.consentRecipients.empty()) {
        const auto kind = client.consentAction == Client::ConsentAction::Attach ? Protocol::Sync::ConsentKind::Attach : Protocol::Sync::ConsentKind::Terminate;
        retireConsentQuestion(kind, client.consentRequestID, client.consentRecipients);
        client.consentRecipients.clear();
    }
}

void Daemon::retireConsentQuestion(Protocol::Sync::ConsentKind kind, std::uint32_t requestID, std::span<const ConsentRecipient> recipients)
{
    for (const auto& recipient : recipients) {
        const Protocol::Sync::ConsentResolution resolution { kind, requestID, recipient.sessionID };
        const auto notification = Protocol::Sync::consentResolvedNotify(resolution);
        if (!notification)
            continue;
        const auto client = std::ranges::find(m_clients, recipient.clientID, &Client::clientID);
        if (client == m_clients.end() || client->didOverflow || !hasLiveStructureAttachment(*client) || client->syncConnection->sendNotify(*notification))
            continue;
        if (std::ranges::find(client->pendingConsentRetirements, resolution) != client->pendingConsentRetirements.end())
            continue;
        if (client->pendingConsentRetirements.size() < maxPendingConsentRetirements)
            client->pendingConsentRetirements.push_back(resolution);
        else
            client->didOverflow = true; // Never evict a promised retirement for a newer question.
    }
}

Codec::JSON::Value Daemon::serveStructureCall(std::size_t index, const Protocol::Control::MethodDescriptor& method, const Protocol::Control::DecodedRequest& request, const Client* approver)
{
    using Codec::JSON::Value;
    using Protocol::Control::ErrorCode;
    using namespace Ubiquitty::Session::Layout;
    const auto refuse = [&](ErrorCode code, std::string_view reason) { return Protocol::Control::rpcError(request.idOrNull(), code, reason); };
    const auto numbered = [&](const char* parameter) { return static_cast<std::uint64_t>(request.params.numberOr(parameter)); };

    // What the call names: an identity as a listing writes it, or the number
    // of the one kind the method acts on.
    if (const auto refusal = Protocol::Control::admitCall(&method, request.params, callerAuthorityOf(m_clients[index])))
        return refuse(refusal->code, refusal->reason);
    if (Protocol::Sync::requiresStructureRevision(method.name) && !expectedStructureRevision(request))
        return refuse(ErrorCode::InvalidParameters, "expectedRevision must be a nonzero decimal uint64 string");
    if (Protocol::Sync::createsSessionForStructure(method.name) && !tokenScopeAllows(m_clients[index], Protocol::Sync::Scope::NewSession))
        return refuse(ErrorCode::Forbidden, "the requesting token does not permit creating a session");
    if (isAgentClient(m_clients[index]) && !approver)
        return refuse(ErrorCode::Forbidden, "an agent's tree change requires an attached person's approval of this exact operation");
    const auto named = structureObjectOf(request);
    if (!named)
        return refuse(ErrorCode::InvalidParameters, std::string(method.name) + ": object names a workspace, a window, a tab, a pane or a session as kind:number, for example tab:7");
    const Identity::ObjectID id = *named;
    const std::string object = Identity::format(id);
    const std::optional<LocatedObject> located = locateObject(id);
    if (!located)
        return refuse(ErrorCode::NotFound, "this host holds no " + object);
    HostedWorkspace& workspace = *located->workspace;
    // Who may change this workspace is decided where the workspace is found,
    // as it is for a verb that names one of its panes.
    if (approver && !mayApproveStructure(*approver, workspace))
        return refuse(ErrorCode::Forbidden, "the person is no longer attached with permission to change this workspace");
    if (const std::optional<std::string> refusal = rearrangeRefusal(approver ? *approver : m_clients[index], workspace))
        return refuse(ErrorCode::Forbidden, std::string(method.name) + ": " + *refusal);
    if (method.name == "workspace.addWindow" && std::to_underlying(located->surface) != numbered("window"))
        return refuse(ErrorCode::InvalidParameters, "the source pane does not belong to the named window");
    if (Protocol::Sync::requiresStructureRevision(method.name)) {
        // Compare immediately before mutation, including changes not yet sent
        // to the caller. Approval does not authorize a later, larger window;
        // a revision that only resized a pane leaves the arrangement asked
        // about standing.
        m_isStructureDirty = true;
        syncStructures();
        const auto structure = m_structureRecords.find(workspace.workspaceID);
        if (structure == m_structureRecords.end() || !structure->second.record
            || !Protocol::Sync::isStructureRevisionCurrent(method.name, expectedStructureRevision(request).value_or(0), structure->second.revision, structure->second.arrangedAt))
            return refuse(ErrorCode::Forbidden, "the target's structure changed; refresh and confirm the current target before changing it");
    }

    Value result = Value::object();
    if (isLayoutSizingMethod(method.name)) {
        if (!hasObservedSizingTarget(request, workspace.layout))
            return refuse(ErrorCode::InvalidParameters, "the exact sizing target is not in the named tab and window");
        bool wasPerformed = false;
        if (method.name == "tab.balance") {
            wasPerformed = workspace.layout.balanceTab(located->surface, located->tab);
        } else if (method.name == "divider.setRatio") {
            const auto* tree = workspace.layout.paneTree(located->surface, located->tab);
            const auto split = tree ? requestedDivider(*tree, request.params.find("path")) : std::nullopt;
            wasPerformed = split && workspace.layout.setTabDividerRatio(located->surface, located->tab, *split, static_cast<float>(request.params.numberOr("ratio")));
            result.set("path", *request.params.find("path"));
            result.set("ratio", *request.params.find("ratio"));
        } else {
            const auto direction = request.params.stringOr("direction");
            const auto edge = direction == "left" ? Panes::Direction::Left : direction == "right" ? Panes::Direction::Right
                : direction == "up"                                                               ? Panes::Direction::Up
                                                                                                  : Panes::Direction::Down;
            wasPerformed = workspace.layout.adjustPaneRatio(located->surface, located->tab, PaneID { static_cast<std::uint32_t>(numbered("pane")) }, edge, static_cast<float>(request.params.numberOr("step")));
            result.set("pane", Value::string(Identity::format({ Identity::ObjectKind::Pane, numbered("pane") })));
            result.set("direction", *request.params.find("direction"));
            result.set("step", *request.params.find("step"));
        }
        if (!wasPerformed)
            return refuse(ErrorCode::InvalidParameters, "the requested tab or divider cannot be resized in that direction");
        result.set("window", Value::string(Identity::format({ Identity::ObjectKind::Window, std::to_underlying(located->surface) })));
        result.set("tab", Value::string(object));
    } else if (method.name == "pane.moveToTab" || method.name == "tab.moveToPane" || method.name == "pane.moveToNewTab") {
        if (!hasObservedTransfer(request, workspace.layout))
            return refuse(ErrorCode::InvalidParameters, "the exact source and destination must remain in the same workspace");
        const auto sourceWindow = located->surface;
        const auto sourceTab = located->tab;
        const PaneID pane { static_cast<std::uint32_t>(id.number) };
        const PaneID target { static_cast<std::uint32_t>(numbered("target")) };
        const auto destinationWindow = method.name == "pane.moveToNewTab" ? SurfaceID { static_cast<std::uint32_t>(numbered("targetWindow")) }
                                                                          : workspace.layout.surfaceHolding(target);
        auto destinationTab = workspace.layout.tabHolding(destinationWindow, target);
        if (method.name == "pane.moveToNewTab") {
            if (!workspace.layout.movePaneToNewTab(sourceWindow, pane, destinationWindow))
                return refuse(ErrorCode::InvalidParameters, "the pane cannot move to a separate tab in this window");
            destinationTab = workspace.layout.tabHolding(destinationWindow, pane);
        } else {
            const auto orientation = request.params.stringOr("orientation") == "horizontal" ? Panes::Orientation::Horizontal : Panes::Orientation::Vertical;
            const auto side = request.params.stringOr("side") == "before" ? Panes::InsertSide::First : Panes::InsertSide::Second;
            const auto insertTarget = request.params.stringOr("insertTarget") == "root" ? Panes::InsertTarget::Root : Panes::InsertTarget::Pane;
            const bool wasPerformed = method.name == "tab.moveToPane"
                ? workspace.layout.moveTabToPane(sourceWindow, sourceTab, target, orientation, side, insertTarget)
                : workspace.layout.movePaneToTab(sourceWindow, pane, destinationTab, target, orientation, side, insertTarget);
            if (!wasPerformed)
                return refuse(ErrorCode::InvalidParameters, "the exact source cannot be inserted into this destination layout");
            result.set("target", Value::string(Identity::format({ Identity::ObjectKind::Pane, std::to_underlying(target) })));
            result.set("orientation", Value::string(request.params.stringOr("orientation")));
            result.set("side", Value::string(request.params.stringOr("side")));
            result.set("insertTarget", Value::string(request.params.stringOr("insertTarget")));
        }
        result.set("sourceWindow", Value::string(Identity::format({ Identity::ObjectKind::Window, std::to_underlying(sourceWindow) })));
        result.set("sourceTab", Value::string(Identity::format({ Identity::ObjectKind::Tab, std::to_underlying(sourceTab) })));
        result.set("window", Value::string(Identity::format({ Identity::ObjectKind::Window, std::to_underlying(destinationWindow) })));
        result.set("tab", Value::string(Identity::format({ Identity::ObjectKind::Tab, std::to_underlying(destinationTab) })));
        if (method.name != "tab.moveToPane")
            result.set("pane", Value::string(object));
    } else if (method.name == "pane.extractTab" || method.name == "pane.extractWindow" || method.name == "tab.extractWindow") {
        if (std::to_underlying(located->surface) != numbered("window"))
            return refuse(ErrorCode::InvalidParameters, "the source no longer belongs to the named window");
        const auto sourceWindow = located->surface;
        const auto sourceTab = located->tab;
        SurfaceID destinationWindow = sourceWindow;
        TabID destinationTab = sourceTab;
        if (method.name == "tab.extractWindow") {
            destinationWindow = workspace.layout.extractTabToSurface(sourceWindow, sourceTab);
        } else {
            const PaneID pane { static_cast<std::uint32_t>(id.number) };
            if (method.name == "pane.extractTab")
                destinationTab = workspace.layout.extractPaneToTab(sourceWindow, pane);
            else {
                destinationWindow = workspace.layout.extractPaneToSurface(sourceWindow, pane);
                destinationTab = workspace.layout.tabHolding(destinationWindow, pane);
            }
        }
        if (destinationWindow == invalidSurface || destinationTab == invalidTab)
            return refuse(ErrorCode::InvalidParameters, "the source cannot be extracted or the destination identity or container limit is exhausted");
        result.set("sourceWindow", Value::string(Identity::format({ Identity::ObjectKind::Window, std::to_underlying(sourceWindow) })));
        result.set("window", Value::string(Identity::format({ Identity::ObjectKind::Window, std::to_underlying(destinationWindow) })));
        result.set("tab", Value::string(Identity::format({ Identity::ObjectKind::Tab, std::to_underlying(destinationTab) })));
        if (method.name != "tab.extractWindow") {
            result.set("pane", Value::string(object));
            result.set("sourceTab", Value::string(Identity::format({ Identity::ObjectKind::Tab, std::to_underlying(sourceTab) })));
        }
    } else if (method.name == "pane.move" || method.name == "pane.swap") {
        if (!hasObservedRelocationPair(request, workspace.layout))
            return refuse(ErrorCode::InvalidParameters, "both distinct panes must remain in the named window's active tab");
        const PaneID pane { static_cast<std::uint32_t>(id.number) };
        const PaneID target { static_cast<std::uint32_t>(numbered("target")) };
        const auto orientation = request.params.stringOr("orientation");
        const auto side = request.params.stringOr("side");
        const bool wasPerformed = method.name == "pane.swap" ? workspace.layout.swapPanes(located->surface, pane, target)
                                                             : workspace.layout.movePane(located->surface, pane, target,
                                                                   orientation == "horizontal" ? Panes::Orientation::Horizontal : Panes::Orientation::Vertical,
                                                                   side == "before" ? Panes::InsertSide::First : Panes::InsertSide::Second);
        if (!wasPerformed)
            return refuse(ErrorCode::InvalidParameters, "the exact pane pair could not be rearranged");
        result.set("window", Value::string(Identity::format({ Identity::ObjectKind::Window, std::to_underlying(located->surface) })));
        result.set("pane", Value::string(object));
        result.set("target", Value::string(Identity::format({ Identity::ObjectKind::Pane, std::to_underlying(target) })));
        if (method.name == "pane.move") {
            result.set("orientation", Value::string(orientation));
            result.set("side", Value::string(side));
        }
    } else if (method.name == "window.activate") {
        if (!workspace.layout.setActiveSurface(located->surface))
            return refuse(ErrorCode::NotFound, "the window is no longer present");
        result.set("window", Value::string(object));
    } else if (method.name == "tab.activate") {
        if (!workspace.layout.setActiveTab(located->surface, located->tab))
            return refuse(ErrorCode::NotFound, "the tab is no longer present");
        result.set("tab", Value::string(object));
        result.set("window", Value::string(Identity::format({ Identity::ObjectKind::Window, std::to_underlying(located->surface) })));
    } else if (method.name == "pane.focus") {
        if (!workspace.layout.focusPane(located->surface, PaneID { static_cast<std::uint32_t>(id.number) }))
            return refuse(ErrorCode::InvalidParameters, "the pane is not in its window's active tab");
        result.set("pane", Value::string(object));
        result.set("window", Value::string(Identity::format({ Identity::ObjectKind::Window, std::to_underlying(located->surface) })));
    } else if (method.name == "window.moveFocus") {
        if (std::to_underlying(located->surface) != numbered("window") || workspace.layout.focusedPane(located->surface) != PaneID { static_cast<std::uint32_t>(id.number) })
            return refuse(ErrorCode::Forbidden, "the observed pane is no longer focused in the named window; refresh before moving focus");
        const auto direction = request.params.stringOr("direction");
        const auto movement = direction == "left" ? Panes::Direction::Left : direction == "right" ? Panes::Direction::Right
            : direction == "up"                                                                   ? Panes::Direction::Up
                                                                                                  : Panes::Direction::Down;
        if (!workspace.layout.moveFocus(located->surface, movement))
            return refuse(ErrorCode::InvalidParameters, "there is no pane in that direction");
        result.set("window", Value::string(Identity::format({ Identity::ObjectKind::Window, std::to_underlying(located->surface) })));
        result.set("fromPane", Value::string(object));
        result.set("pane", Value::string(Identity::format({ Identity::ObjectKind::Pane, std::to_underlying(workspace.layout.focusedPane(located->surface)) })));
        result.set("direction", Value::string(direction));
    } else if (method.name == "pane.zoom") {
        if (workspace.layout.activeTab(located->surface) != located->tab)
            return refuse(ErrorCode::InvalidParameters, "the pane is not in its window's active tab");
        const auto state = request.params.stringOr("state");
        const auto zoomed = workspace.layout.zoomedPane(located->surface);
        const PaneID pane { static_cast<std::uint32_t>(id.number) };
        if ((state == "zoomed" && zoomed != pane) || (state == "unzoomed" && zoomed == pane)) {
            if (!workspace.layout.toggleZoom(located->surface, pane))
                return refuse(ErrorCode::InvalidParameters, "the pane cannot be zoomed in this tab");
        }
        result.set("pane", Value::string(object));
        result.set("state", Value::string(state));
    } else if (method.name == "label.set") {
        const std::string label = request.params.stringOr("label");
        const Identity::LabelVerdict verdict = putLabel(id, label);
        if (verdict != Identity::LabelVerdict::Accepted)
            return refuse(ErrorCode::InvalidParameters, Identity::labelVerdictSentence(verdict));
        result.set("object", Value::string(object));
        result.set("label", Value::string(label));
    } else if (method.name == "tags.set") {
        std::vector<std::string> names;
        if (const Value* tags = request.params.find("tags")) {
            for (const Value& tag : tags->items())
                names.push_back(tag.asString());
        }
        const Identity::TagVerdict verdict = putTags(id, names);
        if (verdict != Identity::TagVerdict::Accepted)
            return refuse(ErrorCode::InvalidParameters, Identity::tagVerdictSentence(verdict));
        // As the book holds them: a name given twice is there once.
        Value held = Value::array();
        for (const std::string& tag : m_tags.tags(id))
            held.push(Value::string(tag));
        result.set("object", Value::string(object));
        result.set("tags", std::move(held));
    } else if (method.name == "workspace.rename") {
        const std::string name = request.params.stringOr("name");
        if (const std::optional<std::string> refusal = renameWorkspace(workspace, name))
            return refuse(ErrorCode::InvalidParameters, *refusal);
        result.set("workspace", Value::string(object));
        result.set("name", Value::string(name));
    } else if (method.name == "tab.rename") {
        const std::string name = request.params.stringOr("name");
        if (const std::optional<std::string> refusal = renameTab(workspace, located->surface, located->tab, name))
            return refuse(ErrorCode::InvalidParameters, *refusal);
        result.set("tab", Value::string(object));
        result.set("name", Value::string(name));
    } else if (method.name == "tab.move") {
        const Identity::ObjectID window { Identity::ObjectKind::Window, numbered("window") };
        const std::optional<LocatedObject> destination = locateObject(window);
        if (!destination)
            return refuse(ErrorCode::NotFound, "this host holds no " + Identity::format(window));
        if (destination->workspace != &workspace)
            return refuse(ErrorCode::InvalidParameters, "a tab moves among the windows of its own workspace: " + object + " is in workspace:" + std::to_string(workspace.workspaceID) + " and " + Identity::format(window) + " in workspace:" + std::to_string(destination->workspace->workspaceID));
        const int place = static_cast<int>(numbered("index"));
        const int standing = workspace.layout.tabCount(destination->surface);
        const bool isReorder = destination->surface == located->surface; // bool-ok: names the one state it reports
        if (!isReorder && standing >= maxTabsPerSurface)
            return refuse(ErrorCode::InvalidParameters, Identity::format(window) + " holds as many tabs as a window may, " + std::to_string(maxTabsPerSurface));
        if (const int furthest = isReorder ? standing - 1 : standing; place > furthest)
            return refuse(ErrorCode::InvalidParameters, Identity::format(window) + " holds " + std::to_string(standing) + (standing == 1 ? " tab" : " tabs") + ", so the furthest place " + object + " can take there is " + std::to_string(furthest));
        if (!workspace.layout.moveTab(located->tab, destination->surface, place))
            return refuse(ErrorCode::InternalError, object + " could not be moved");
        result.set("tab", Value::string(object));
        result.set("window", Value::string(Identity::format(window)));
        result.set("index", Value::number(place));
    } else if (method.name == "window.close") {
        // The window's sessions are counted before it leaves the model and
        // ended once this pass is over: ending one here could take away the
        // very connection this call is being answered on.
        const auto ending = std::ranges::count_if(workspace.paneSessions, [&](const auto& binding) { return workspace.layout.surfaceHolding(binding.first) == located->surface; });
        if (workspace.layout.surfaceCount() > 1)
            (void)workspace.layout.closeSurface(located->surface);
        else
            workspace.isClosing = true;
        m_closuresOwed.push_back(workspace.workspaceID);
        result.set("window", Value::string(object));
        result.set("sessionsEnded", Value::number(static_cast<double>(ending)));
    } else {
        const bool doesCreateSession = Protocol::Sync::createsSessionForStructure(method.name);
        if (doesCreateSession && m_sessions.size() >= maxSessions) {
            Value details = Value::object();
            details.set("reason", Value::string("hostFull"));
            details.set("limit", Value::number(maxSessions));
            return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::Busy,
                "the host holds its limit of " + std::to_string(maxSessions) + " sessions", details);
        }
        Ubiquitty::Session::WorkspaceIntent verb;
        verb.surfaceID = std::to_underlying(located->surface);
        verb.tabID = std::to_underlying(located->tab);
        Session* openedFrom = nullptr;
        if (method.name == "workspace.addTab" && request.params.find("pane")) {
            verb.paneID = static_cast<std::uint32_t>(numbered("pane"));
            const auto pane = PaneID { verb.paneID };
            if (workspace.layout.surfaceHolding(pane) != located->surface)
                return refuse(ErrorCode::InvalidParameters, "the source pane does not belong to the named window");
            openedFrom = findSession(workspace.sessionFor(pane));
        } else if (id.kind == Identity::ObjectKind::Pane) {
            verb.paneID = static_cast<std::uint32_t>(id.number);
            openedFrom = findSession(workspace.sessionFor(PaneID { verb.paneID }));
        } else {
            const auto binding = std::ranges::find_if(workspace.paneSessions, [&](const auto& held) {
                return workspace.layout.surfaceHolding(held.first) == located->surface;
            });
            if (binding != workspace.paneSessions.end())
                openedFrom = findSession(binding->second);
        }
        if (!openedFrom)
            return refuse(ErrorCode::NotFound, "the target holds no live session");
        const auto previouslyFocused = workspace.layout.focusedPane(located->surface);
        const auto previousTab = workspace.layout.activeTab(located->surface);
        const auto previousZoom = workspace.layout.zoomedPane(located->surface);
        const auto previousWindow = workspace.layout.activeSurface();
        if (method.name == "workspace.addWindow") {
            verb.verb = Ubiquitty::Session::WorkspaceIntentKind::AddSurface;
        } else if (method.name == "workspace.addTab") {
            verb.verb = Ubiquitty::Session::WorkspaceIntentKind::AddTab;
        } else if (method.name == "workspace.split") {
            // The named pane must be in the shown tab. A failed split leaves
            // the existing focus intact, including allocation refusal.
            if (workspace.layout.activeTab(located->surface) != located->tab)
                return refuse(ErrorCode::InvalidParameters, "the pane to split is not in its window's active tab");
            verb.verb = Ubiquitty::Session::WorkspaceIntentKind::SplitFocusedPane;
            verb.orientation = std::to_underlying(request.params.stringOr("orientation") == "horizontal" ? Ubiquitty::Panes::Orientation::Horizontal : Ubiquitty::Panes::Orientation::Vertical);
            verb.ratio = static_cast<float>(request.params.numberOr("ratio"));
            if (!workspace.layout.focusPane(located->surface, PaneID { verb.paneID }))
                return refuse(ErrorCode::NotFound, "the pane to split is no longer held");
        } else if (method.name == "workspace.closeTab") {
            verb.verb = Ubiquitty::Session::WorkspaceIntentKind::CloseTab;
        } else if (method.name == "workspace.closePane") {
            verb.verb = Ubiquitty::Session::WorkspaceIntentKind::ClosePane;
        } else {
            return refuse(ErrorCode::MethodNotFound, "the host serves no such tree operation");
        }
        const auto ending = doesCreateSession ? 0 : std::ranges::count_if(workspace.paneSessions, [&](const auto& binding) {
            return method.name == "workspace.closePane" ? std::to_underlying(binding.first) == verb.paneID
                                                        : workspace.layout.tabHolding(located->surface, binding.first) == located->tab;
        });
        Ubiquitty::Session::WorkspaceIntentResult applied;
        if (!performWorkspaceVerb(index, workspace, *openedFrom, verb, applied)) {
            if (doesCreateSession) {
                // The shared helper removes a pane/tab it could not back.
                // Restore the previous presentation too: addTab selected the
                // now-removed tab and cleared the old tab's zoom.
                (void)workspace.layout.setActiveTab(located->surface, previousTab);
                if (previouslyFocused != invalidPane)
                    (void)workspace.layout.focusPane(located->surface, previouslyFocused);
                if (previousZoom != invalidPane && workspace.layout.zoomedPane(located->surface) != previousZoom)
                    (void)workspace.layout.toggleZoom(located->surface, previousZoom);
                (void)workspace.layout.setActiveSurface(previousWindow);
            }
            return refuse(ErrorCode::InvalidParameters, "the host could not apply this tree change; the target is full or its new session could not be created");
        }
        if (doesCreateSession) {
            result.set("pane", Value::string(Identity::format({ Identity::ObjectKind::Pane, applied.paneID })));
            result.set("session", Value::string(Identity::format({ Identity::ObjectKind::Session, applied.sessionID })));
            if (method.name == "workspace.addWindow" || method.name == "workspace.addTab")
                result.set("tab", Value::string(Identity::format({ Identity::ObjectKind::Tab, applied.tabID })));
            if (method.name == "workspace.addWindow") {
                result.set("sourceWindow", Value::string(Identity::format({ Identity::ObjectKind::Window, std::to_underlying(located->surface) })));
                result.set("sourcePane", Value::string(object));
                result.set("window", Value::string(Identity::format({ Identity::ObjectKind::Window, applied.surfaceID })));
            }
        } else {
            m_closuresOwed.push_back(workspace.workspaceID);
            result.set(method.name == "workspace.closeTab" ? "tab" : "pane", Value::string(object));
            result.set("sessionsEnded", Value::number(static_cast<double>(ending)));
        }
    }

    m_isStructureDirty = true;
    // A rename can take a workspace out of the record as well as into it.
    if (method.name == "workspace.rename")
        scheduleLayoutRecordWrite();
    else
        scheduleLayoutRecordWrite(workspace);
    if (!workspace.isClosing)
        admitWorkspacePanes(workspace);
    return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
}

void Daemon::settleOwedClosures()
{
    std::vector<std::uint32_t> reclaims;
    for (const auto& client : m_clients) {
        if (client.reclaimCall)
            reclaims.push_back(client.clientID);
    }
    for (const auto clientID : reclaims) {
        const auto found = std::ranges::find(m_clients, clientID, &Client::clientID);
        if (found != m_clients.end() && !found->didOverflow)
            completeReclaimCall(static_cast<std::size_t>(found - m_clients.begin()));
    }
    const auto terminations = std::exchange(m_terminationsOwed, {});
    for (const auto& [sessionID, paneID] : terminations) {
        const Session* session = findSession(sessionID);
        if (session && session->screenSource && session->screenSource->paneID() == paneID)
            destroySession(sessionID);
    }
    const std::vector<std::uint32_t> owed = std::exchange(m_closuresOwed, {});
    for (const std::uint32_t workspaceID : owed) {
        const auto found = std::ranges::find_if(m_workspaces, [&](const auto& held) { return held->workspaceID == workspaceID; });
        if (found == m_workspaces.end())
            continue;
        HostedWorkspace* workspace = found->get();
        if (!workspace->isClosing) {
            reapVanishedPanes(*workspace);
            continue;
        }
        // Bindings go before their sessions end, as a closing verb's do.
        auto ended = std::move(workspace->paneSessions);
        std::erase_if(m_workspaces, [workspace](const auto& candidate) { return candidate.get() == workspace; });
        for (const auto& binding : ended)
            destroySession(binding.second);
        m_isStructureDirty = true;
    }
}

Codec::JSON::Value Daemon::pinPaneGeometry(const Client& client, Session& session, const Protocol::Control::DecodedRequest& request)
{
    using Protocol::Control::ErrorCode;
    const Codec::JSON::Value* rows = request.params.find("rows");
    const Codec::JSON::Value* columns = request.params.find("columns");
    if (!session.screenSource)
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::NotFound, "no pane is bound to that handle");
    Protocol::Sync::GeometryArbiter& arbiter = session.screenSource->geometry();
    const auto isCount = [](const Codec::JSON::Value* value) {
        return value && value->isNumber() && value->asNumber() >= 1 && value->asNumber() <= 1000 && value->asNumber() == static_cast<double>(static_cast<int>(value->asNumber()));
    };
    Codec::JSON::Value result = Codec::JSON::Value::object();
    if (!rows && !columns) {
        arbiter.unpin();
        noteRecordsDue(session, nowMilliseconds());
        result.set("pinned", Codec::JSON::Value::boolean(false));
        return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
    }
    if (!isCount(rows) || !isCount(columns))
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InvalidParameters, "rows and columns are both given, each from 1 to 1000, or neither");
    // The pin names who asked, so every other client can be told whose size
    // it is looking at.
    const Protocol::Sync::AttachmentRecord* asking = client.syncConnection ? client.syncConnection->attachment() : nullptr;
    arbiter.pin({ .rows = static_cast<std::uint16_t>(clampRows(static_cast<int>(rows->asNumber()))), .columns = static_cast<std::uint16_t>(clampColumns(static_cast<int>(columns->asNumber()))) },
        asking ? std::optional { asking->attachmentID } : std::nullopt);
    noteRecordsDue(session, nowMilliseconds());
    result.set("pinned", Codec::JSON::Value::boolean(true));
    return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
}

Codec::JSON::Value Daemon::overridePaneGeometry(const Client& client, Session& session, const Protocol::Control::DecodedRequest& request)
{
    using Protocol::Control::ErrorCode;
    // Setting a pane's size drives it, as typing does: a caller that may not
    // drive is answered forbidden, and its connection stays.
    if (!tokenScopeAllows(client, Protocol::Sync::Scope::Drive))
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::Forbidden, "setting a pane's size needs the Drive scope");
    if (!session.screenSource)
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::NotFound, "no pane is bound to that handle");
    const auto count = [&](const char* name) -> std::optional<int> {
        const Codec::JSON::Value* value = request.params.find(name);
        if (!value || !value->isNumber() || value->asNumber() < 1 || value->asNumber() > 1000 || value->asNumber() != static_cast<double>(static_cast<int>(value->asNumber())))
            return std::nullopt;
        return static_cast<int>(value->asNumber());
    };
    const std::optional<int> rows = count("rows");
    const std::optional<int> columns = count("columns");
    if (!rows || !columns)
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InvalidParameters, "rows and columns are each from 1 to 1000");
    // Clamped once, so the program and Core are given the same size.
    const Protocol::Sync::PaneGeometry geometry { .rows = static_cast<std::uint16_t>(clampRows(*rows)), .columns = static_cast<std::uint16_t>(clampColumns(*columns)) };
    session.screenSource->geometry().overrideSize(geometryKey(client), geometry, nowMicroseconds());
    Codec::JSON::Value result = Codec::JSON::Value::object();
    result.set("rows", Codec::JSON::Value::number(geometry.rows));
    result.set("columns", Codec::JSON::Value::number(geometry.columns));
    return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
}

void Daemon::noteAttachmentGrant(const Client& client)
{
    const Protocol::Sync::AttachmentRecord* attachment = client.hasToken && !client.tokenID.empty() && client.syncConnection ? client.syncConnection->attachment() : nullptr;
    if (!attachment || std::ranges::find(m_attachmentGrants, attachment->attachmentID, &AttachmentGrant::attachmentID) != m_attachmentGrants.end())
        return;
    m_attachmentGrants.push_back({ .attachmentID = attachment->attachmentID, .scope = client.tokenScope, .tokenID = client.tokenID, .tokenExpiryUnix = client.tokenExpiryUnix, .hasBootstrapToken = client.hasBootstrapToken, .allowsProof = client.quicAuthed, .isInteractiveUser = client.isInteractiveUser });
}

AttachmentHandover Daemon::exportAttachmentAuthority(std::uint64_t now) const
{
    AttachmentHandover carried { .attachments = m_attachments->exportForHandover(now), .grants = {}, .hostIdentities = m_inheritedHostIdentities };
    for (auto& record : carried.attachments.records) {
        for (const auto& client : m_clients) {
            if (!client.didOverflow && !isAgentClient(client) && tokenScopeAllows(client, Protocol::Sync::Scope::Drive)
                && client.syncConnection && client.syncConnection->attachment()
                && client.syncConnection->attachment()->attachmentID == record->attachmentID
                && client.syncConnection->proportionalFaces()) {
                record->inheritedProportionalFaces = client.syncConnection->proportionalFaces();
                break;
            }
        }
    }
    carried.hostIdentities.push_back({ .issuer = m_hostInstanceID, .daemonInstanceID = m_daemonInstanceID });
    for (const auto& grant : m_attachmentGrants) {
        if (std::ranges::any_of(carried.attachments.records, [&](const auto& record) { return record->attachmentID == grant.attachmentID; }))
            carried.grants.push_back(grant);
    }
    return carried;
}

std::expected<void, AttachmentHandoverRefusal> Daemon::inheritAttachmentAuthority(AttachmentHandover&& carried,
    Protocol::Sync::ID128 predecessor, std::uint64_t now, std::uint64_t nowUnix)
{
    auto prepared = prepareAttachmentAuthority(std::move(carried), predecessor, now, nowUnix);
    if (!prepared)
        return std::unexpected(prepared.error());
    m_attachments = std::move(prepared->attachments);
    wireAttachmentRetirement();
    m_attachmentGrants = std::move(prepared->grants);
    m_inheritedHostIdentities = std::move(prepared->hostIdentities);
    return {};
}

std::expected<PreparedAttachmentAuthority, AttachmentHandoverRefusal> Daemon::prepareAttachmentAuthority(AttachmentHandover&& carried,
    Protocol::Sync::ID128 predecessor, std::uint64_t now, std::uint64_t nowUnix)
{
    if (!m_clients.empty() || !m_attachmentGrants.empty() || !m_inheritedHostIdentities.empty() || !m_attachments->isPristine())
        return std::unexpected(AttachmentHandoverRefusal::DestinationInUse);
    if (carried.attachments.inheritedInstances.size() > Protocol::Sync::maxInheritedInstances
        || carried.hostIdentities.size() != carried.attachments.inheritedInstances.size() + 1)
        return std::unexpected(AttachmentHandoverRefusal::InvalidTable);
    for (std::size_t index = 0; index < carried.hostIdentities.size(); ++index) {
        const auto& identity = carried.hostIdentities[index];
        const auto expected = index == carried.attachments.inheritedInstances.size() ? predecessor : carried.attachments.inheritedInstances[index];
        if (identity.issuer != expected || !identity.daemonInstanceID || identity.daemonInstanceID == m_daemonInstanceID
            || std::find_if(carried.hostIdentities.begin(), carried.hostIdentities.begin() + index,
                   [&](const auto& previous) { return previous.daemonInstanceID == identity.daemonInstanceID; })
                != carried.hostIdentities.begin() + index)
            return std::unexpected(AttachmentHandoverRefusal::InvalidTable);
    }
    for (const auto& record : carried.attachments.records) {
        if (!record)
            return std::unexpected(AttachmentHandoverRefusal::InvalidTable);
        const auto grant = std::ranges::find(carried.grants, record->attachmentID, &AttachmentGrant::attachmentID);
        if (record->authority.principal.starts_with("token:")) {
            if (grant == carried.grants.end())
                return std::unexpected(AttachmentHandoverRefusal::MissingGrant);
            if (grant->tokenID.empty() || record->authority.principal != "token:" + grant->tokenID
                || record->authority.mayDrive != grant->scope.contains(Protocol::Sync::Scope::Drive)
                || (record->authority.verifiedKind == Protocol::Sync::ClientKind::Human && !grant->isInteractiveUser))
                return std::unexpected(AttachmentHandoverRefusal::InvalidGrant);
        } else if (grant != carried.grants.end())
            return std::unexpected(AttachmentHandoverRefusal::InvalidGrant);
    }
    // A carried grant is admitted only against a store read whole.
    TokenStoreContents store = readTokenStore(m_tokenPath);
    if (!store.isWhole)
        store.records.clear();
    const auto& tokens = store.records;
    auto admittedGrants = carried.grants;
    for (std::size_t index = 0; index < admittedGrants.size(); ++index) {
        auto& grant = admittedGrants[index];
        if (std::find_if(admittedGrants.begin(), admittedGrants.begin() + index,
                [&](const auto& previous) { return previous.attachmentID == grant.attachmentID; })
                != admittedGrants.begin() + index
            || !std::ranges::any_of(carried.attachments.records, [&](const auto& record) { return record->attachmentID == grant.attachmentID; })
            || (grant.isInteractiveUser && !grant.hasBootstrapToken))
            return std::unexpected(AttachmentHandoverRefusal::InvalidGrant);
        if (grant.tokenExpiryUnix && nowUnix >= grant.tokenExpiryUnix)
            return std::unexpected(AttachmentHandoverRefusal::ExpiredGrant);
        if (grant.hasBootstrapToken)
            continue;
        const auto token = std::ranges::find_if(tokens, [&](const TokenRecord& candidate) { return digestsEqual(candidate.hashHex, grant.tokenID); });
        if (token == tokens.end() || (token->scope.toRaw() & grant.scope.toRaw()) != grant.scope.toRaw())
            return std::unexpected(AttachmentHandoverRefusal::RevokedGrant);
        if (tokenExpiry(*token, nowUnix) == TokenExpiry::Expired)
            return std::unexpected(AttachmentHandoverRefusal::ExpiredGrant);
        if (token->expiryUnix && (!grant.tokenExpiryUnix || token->expiryUnix < grant.tokenExpiryUnix))
            grant.tokenExpiryUnix = token->expiryUnix;
    }
    auto attachments = std::make_unique<Protocol::Sync::AttachmentTable>(m_hostInstanceID, Protocol::Sync::measuredResumePolicy(), m_attachmentSecrets);
    if (!attachments->inherit(std::move(carried.attachments), predecessor, now))
        return std::unexpected(AttachmentHandoverRefusal::InvalidTable);
    std::erase_if(admittedGrants, [&](const auto& grant) { return !attachments->find(grant.attachmentID); });
    std::erase_if(carried.hostIdentities, [&](const auto& identity) { return !attachments->isResumableIssuer(identity.issuer); });
    carried.grants.clear();
    return PreparedAttachmentAuthority { std::move(attachments), std::move(admittedGrants), std::move(carried.hostIdentities) };
}

std::expected<void, Protocol::Sync::ResumeRefusal> Daemon::admitAttachmentProof(std::size_t index,
    const Protocol::Sync::ResumeProof& proof, std::span<const std::uint8_t, 32> exporter)
{
    dropExpiredTokenClients();
    auto grant = std::ranges::find(m_attachmentGrants, proof.attachmentID, &AttachmentGrant::attachmentID);
    if (grant == m_attachmentGrants.end() || !grant->allowsProof)
        return std::unexpected(Protocol::Sync::ResumeRefusal::UnknownAttachment);
    if (!grant->hasBootstrapToken) {
        reverifyTokenClients();
        grant = std::ranges::find(m_attachmentGrants, proof.attachmentID, &AttachmentGrant::attachmentID);
    }
    const auto* attachment = m_attachments->find(proof.attachmentID);
    if (index >= m_clients.size() || m_clients[index].didOverflow || grant == m_attachmentGrants.end() || !grant->allowsProof
        || !attachment || attachment->authority.principal != "token:" + grant->tokenID)
        return std::unexpected(Protocol::Sync::ResumeRefusal::UnknownAttachment);
    const auto admitted = m_attachments->admitProof(proof, exporter, m_clients[index].clientID, nowMicroseconds());
    if (!admitted)
        return std::unexpected(admitted.error());
    Client& client = m_clients[index];
    client.hasToken = true;
    client.tokenScope = grant->scope;
    client.tokenID = grant->tokenID;
    client.tokenExpiryUnix = grant->tokenExpiryUnix;
    client.hasBootstrapToken = grant->hasBootstrapToken;
    client.isInteractiveUser = grant->isInteractiveUser && attachment->authority.verifiedKind == Protocol::Sync::ClientKind::Human;
    return {};
}

void Daemon::wireAttachmentRetirement()
{
    m_attachments->setRetireRecord([this](Protocol::Sync::ID128 attachmentID) {
        for (const auto& session : m_sessions) {
            if (session->screenSource)
                (void)session->screenSource->effects().noteState(attachmentID, Protocol::Sync::AttachmentState::Detached, 0);
        }
    });
}

void Daemon::reapAttachments(std::uint64_t now)
{
    // An attachment freed sets no pane's size again, and a paste its client
    // began and will never finish has its bracket closed, so the program is
    // not left in paste mode.
    m_attachments->expire(now);
    for (const Protocol::Sync::AttachmentTable::Freed& freed : m_attachments->takeFreed()) {
        std::erase_if(m_attachmentGrants, [&](const AttachmentGrant& grant) { return grant.attachmentID == freed.attachmentID; });
        for (auto& session : m_sessions) {
            if (!session->screenSource)
                continue;
            // A handover may retire a record before its pane is installed.
            (void)session->screenSource->effects().noteState(freed.attachmentID, Protocol::Sync::AttachmentState::Detached, 0);
            session->screenSource->geometry().forget(freed.attachmentID);
            session->screenSource->withdrawInputAuthority(freed.attachmentID, now);
        }
    }
}

void Daemon::settleDueInput(std::uint64_t now)
{
    for (auto& session : m_sessions) {
        if (!session->screenSource || !session->stream)
            continue;
        const std::optional<std::uint64_t> due = session->screenSource->nextSettleMicroseconds();
        if (!due || now < *due)
            continue;
        // A program quiet at the check has written nothing since the last
        // read: the check is a read that found nothing, begun at `now`, which
        // was read before the check. One with output pending settles at the
        // read that drains it.
        if (ptyReadiness(session->stream.get()) == Runtime::ReadReadiness::Quiet)
            session->screenSource->noteReadWouldBlock(now);
    }
}

std::optional<std::uint64_t> Daemon::nextScreenServiceMicroseconds(std::uint64_t now) const
{
    std::optional<std::uint64_t> soonest = m_attachments->nextExpiryMicroseconds();
    // A session or workspace a call ended is ended by the next pass, where no
    // connection is being served; nothing a client sends need come first.
    if (!m_terminationsOwed.empty() || !m_closuresOwed.empty())
        return now;
    // A write due to settle is checked for at its time, and a size waiting
    // out its coalesce window is applied at the window's end.
    for (const auto& session : m_sessions) {
        if (session->term) {
            // A departure can retire the last query and queue its status after
            // this pane's service pass. Move that outcome to the PTY queue on
            // the next pass; an already blocked PTY wakes through POLLOUT. A
            // pane whose last reply write was refused is not due at once: the
            // pass would meet the same refusal.
            if (!session->shellExited && session->stream && !session->stream->hasPendingWrite() && !session->isReplyWriteRefused
                && (session->term->outputLength || session->term->bulkOutput.length))
                return now;
            const auto ticks = Ticks::fromMilliseconds(now / 1000);
            for (const auto& pending : session->term->richTextMeasurements) {
                if (pending.completion || pending.failure != RichText::MeasurementFailure::None)
                    continue;
                const auto due = now + RichText::measurementWaitMicroseconds(pending, ticks);
                if (!soonest || due < *soonest)
                    soonest = due;
            }
        }
        if (!session->screenSource)
            continue;
        if (const std::optional<std::uint64_t> settle = session->screenSource->nextSettleMicroseconds(); settle && (!soonest || *settle < *soonest))
            soonest = settle;
        if (const std::optional<std::uint64_t> resize = session->screenSource->geometry().nextDueMicroseconds(); resize && (!soonest || *resize < *soonest))
            soonest = resize;
        if (session->presentedDueAt && (!soonest || *session->presentedDueAt < *soonest))
            soonest = session->presentedDueAt;
    }
    for (const Client& client : m_clients) {
        if (!client.syncConnection)
            continue;
        if (client.pasteCall && client.syncConnection->state() == Protocol::Sync::ConnectionState::Attached) {
            const auto& held = *client.pasteCall;
            if (!held.reply && !held.refusal) {
                const auto expiry = held.deadlineMilliseconds * 1000;
                if (!soonest || expiry < *soonest)
                    soonest = expiry;
            }
            // Existing queued bytes wake their transport; do not spin while
            // a final answer or credit is waiting for that queue to drain.
            // The expiry above still releases a stalled raw owner even if
            // that transport cannot take its small final refusal yet.
            if (!client.syncConnection->hasQueuedStreamFrames() && !outboundPendingBytes(client)) {
                std::optional<std::uint64_t> due;
                if (held.reply || held.refusal || held.upload.isComplete())
                    due = now;
                else if (!held.hasReceivedChunk)
                    due = held.lastCreditMilliseconds ? (held.lastCreditMilliseconds + 1000) * 1000 : now;
                if (due && (!soonest || *due < *soonest))
                    soonest = *due;
            }
        }
        // A bounded replay with only withheld records may queue no bytes at
        // all. Continue it promptly, but never spin over a blocked carrier or
        // an already queued notification: transport/ACK progress wakes those.
        if ((client.eventReplay.isOwed(m_events.latestCursor()) || client.screenAccessReplay.isOwed(m_events.latestCursor()))
            && client.syncConnection->state() == Protocol::Sync::ConnectionState::Attached
            && !client.syncConnection->hasQueuedStreamFrames() && !outboundPendingBytes(client))
            soonest = now;
        if (const std::optional<std::uint64_t> due = client.syncConnection->nextServiceMicroseconds(now); due && (!soonest || *due < *soonest))
            soonest = due;
        // A frame another client's call queued here after this connection's
        // drain in the same pass (a consent question, a retirement) waits in
        // the connection, where no descriptor event reaches it. The lane
        // hands it over on the next pass when the output has room, so that
        // pass is due now; with the output full, its POLLOUT drives it.
        if (!client.didOverflow && client.syncConnection->state() == Protocol::Sync::ConnectionState::Attached
            && client.syncConnection->queuedInteractiveBytes() && outboundPendingBytes(client) < screenOutputLowWaterBytes)
            soonest = now;
    }
    return soonest;
}

void Daemon::emitAgentEvents(Session& s)
{
    if (!s.term)
        return;
    const auto effectPrimary = typedEffectPrimaryForSession(s);
    // One retained effect PER OSC-133 D completed since the last emit. A
    // single output batch can carry several D marks (a command's D;n then the
    // prompt's D;0); the Core's ring preserves each code so we never coalesce
    // them into the last one (which dropped the command's real exit). -1 (a D
    // with no ;n) maps to the INT32_MIN "no exit code" sentinel.
    if (s.term->commandCompletedGeneration != s.lastEmittedCommandGeneration) {
        uint32_t from = s.lastEmittedCommandGeneration;
        const uint32_t to = s.term->commandCompletedGeneration;
        if (to - from > 8) // fell >8 behind → the ring wrapped; emit the last 8
            from = to - 8;
        // The explicit command line (OSC 633;E) buffer holds only the LATEST
        // command, so attach it to the FINAL completion of this batch (the
        // current completion), and only when it advanced since the last emit —
        // a completion with no fresh E carries an empty field, never a stale one.
        const bool commandLineFresh = s.term->commandLineGeneration != s.lastEmittedCommandLineGeneration;
        for (uint32_t g = from; g < to; ++g) {
            const int32_t code = s.term->commandExitCodeRing[g % 8];
            const bool isLast = (g + 1 == to);
            const Protocol::Sync::CommandFinishedEffect effect {
                .exitStatus = (code < 0) ? INT32_MIN : code,
                .durationMicroseconds = static_cast<std::uint64_t>(s.term->commandDurationTicksRing[g % 8]) * 1'000'000 / 60,
                .currentDirectory = s.term->currentDirectory,
                .commandLine = isLast && commandLineFresh ? std::string_view(s.term->commandLine) : std::string_view {},
            };
            if (effectPrimary) {
                if (!s.screenSource->appendEffect(effect, *effectPrimary))
                    break;
            }
            // Only advance the source cursor once the effect is retained.
            // The starts still precede this end in the event subscriber log.
            recordCommandStarts(s, s.term->commandStartedAtCompletionRing[g % 8]);
            Ubiquitty::Session::Event finished;
            finished.kind = Ubiquitty::Session::EventKind::CommandFinished;
            finished.sessionID = s.id;
            finished.paneID = paneOfSession(s);
            finished.exitStatus = effect.exitStatus == INT32_MIN ? Ubiquitty::Session::noExitStatus : effect.exitStatus;
            finished.text = effect.commandLine;
            finished.detail = effect.currentDirectory;
            recordEvent(std::move(finished));
            s.lastEmittedCommandGeneration = g + 1;
            if (isLast)
                s.lastEmittedCommandLineGeneration = s.term->commandLineGeneration;
        }
    }
    if (s.lastEmittedCommandGeneration == s.term->commandCompletedGeneration)
        recordCommandStarts(s, s.term->commandStartedGeneration);
    recordProgramStatusEvents(s);
    if (isFactsRead(s))
        (void)refreshPaneFacts(s);
}

void Daemon::recordCommandStarts(Session& s, std::uint32_t through)
{
    // Wrapping arithmetic, as the completion generations use: a count that
    // fell more than the ring behind records only its last eight, and a stale
    // count from before the last recorded start records nothing.
    const std::uint32_t pending = through - s.lastEmittedCommandStartedGeneration;
    if (pending == 0 || pending > (1u << 31))
        return;
    for (std::uint32_t remaining = std::min<std::uint32_t>(pending, 8); remaining > 0; --remaining) {
        Ubiquitty::Session::Event started;
        started.kind = Ubiquitty::Session::EventKind::CommandStarted;
        started.sessionID = s.id;
        started.paneID = paneOfSession(s);
        recordEvent(std::move(started));
    }
    s.lastEmittedCommandStartedGeneration = through;
}

void Daemon::recordEvent(Ubiquitty::Session::Event event)
{
    if (event.kind == Ubiquitty::Session::EventKind::SessionEnded) {
        if (const Session* session = findSession(event.sessionID); session && session->screenSource) {
            m_backgroundEvents.removePane(backgroundIdentity(*session->screenSource));
            scheduleBackgroundEventWrite();
        }
    }
    m_events.append(std::move(event));
}

void Daemon::recordScreenAccess(const Client& client, const Session& session)
{
    if (!isAgentClient(client))
        return;
    Ubiquitty::Session::Event event;
    event.kind = Ubiquitty::Session::EventKind::ScreenRead;
    event.sessionID = session.id;
    event.count = m_wallClockMilliseconds();
    event.text = client.agentLabel;
    event.detail = client.peerProcessID ? client.peerProcess : std::string {};
    recordEvent(std::move(event));
}

Daemon::EventDisclosure Daemon::eventDisclosureFor(const Client& subscriber, const Ubiquitty::Session::Event& event)
{
    using Ubiquitty::Session::EventKind;
    if (event.kind == EventKind::ScreenRead)
        return EventDisclosure::Withheld;
    if (event.kind == EventKind::Gap || capabilityOriginOf(subscriber) == Ubiquitty::CapabilityOrigin::Local)
        return EventDisclosure::Whole;
    const Session* session = findSession(event.sessionID);
    const bool isReachable = subscriber.subscriptionFor(event.sessionID) || (session && agentHasStandingGrant(subscriber, *session));
    if (!isReachable)
        return EventDisclosure::Withheld;
    return EventDisclosure::Whole;
}

std::uint32_t Daemon::paneOfSession(const Session& session)
{
    if (const HostedWorkspace* workspace = workspaceHolding(session.id))
        return std::to_underlying(workspace->paneFor(session.id));
    return std::to_underlying(session.placement.pane);
}

void Daemon::recordProgramStatusEvents(Session& s)
{
    if (!s.term || s.term->activityGeneration == s.reportedActivityGeneration)
        return;
    s.reportedActivityGeneration = s.term->activityGeneration;
    if (s.screenSource) {
        const auto pane = s.screenSource->paneID();
        const std::string identity = Ubiquitty::Session::backgroundPaneID(pane.high, pane.low);
        m_backgroundEvents.updateQuestions(identity, s.id, s.reportedProgramStatus, s.term->programStatus.records, m_wallClockMilliseconds());
        scheduleBackgroundEventWrite();
    }
    const auto& current = s.term->programStatus.records;
    const std::uint32_t pane = paneOfSession(s);
    const auto report = [&](const OSC::ProgramStatus& record) {
        Ubiquitty::Session::Event changed;
        changed.kind = Ubiquitty::Session::EventKind::ProgramStatus;
        changed.sessionID = s.id;
        changed.paneID = pane;
        changed.state = static_cast<Ubiquitty::Session::EventProgramState>(record.state);
        changed.text = record.message;
        changed.detail = record.id;
        recordEvent(std::move(changed));
    };
    // A record that disappeared reports state none, so a waiter on blocked
    // learns that the question went away.
    for (const auto& previous : s.reportedProgramStatus) {
        if (std::ranges::none_of(current, [&](const auto& record) { return record.id == previous.id; }))
            report({ .id = previous.id, .app = {}, .title = {}, .message = {}, .state = OSC::ProgramState::None });
    }
    for (const auto& record : current) {
        if (std::ranges::none_of(s.reportedProgramStatus, [&](const auto& previous) {
                return previous.id == record.id && previous.state == record.state && previous.message == record.message;
            }))
            report(record);
    }
    s.reportedProgramStatus.clear();
    if (!current.empty())
        s.reportedProgramStatus.assign(current.begin(), current.end());
}

std::optional<Codec::JSON::Value> Daemon::beginPasteCall(std::size_t index, std::uint32_t callIndex, const Protocol::Control::DecodedRequest& request)
{
    using Protocol::Control::ErrorCode;
    Client& client = m_clients[index];
    const auto* method = Protocol::Control::findSyncMethod(request.method);
    if (const auto refusal = Protocol::Control::admitCall(method, request.params, callerAuthorityOf(client)))
        return Protocol::Control::rpcError(request.idOrNull(), refusal->code, refusal->reason);
    if (!hasLiveStructureAttachment(client)
        || (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr))))
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::Forbidden, "paste.push requires the original live Drive attachment");
    const auto manifest = Protocol::Control::pastePushParameters(request.params);
    if (!manifest || !request.id.isString() || request.id.asString().empty()
        || request.id.asString().size() > Protocol::Sync::maxFrameBodyBytes)
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::InvalidParameters, "paste.push requires an exact bounded manifest and a nonempty string request id");
    const auto subscription = std::ranges::find(client.paneSubscriptions, manifest->pane, &PaneSubscription::screenHandle);
    const Session* session = subscription != client.paneSubscriptions.end() ? findSession(subscription->sessionID) : nullptr;
    if (!session || !session->screenSource || !client.syncConnection->paneView(manifest->pane))
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::NotFound, "no live pane is bound to the paste handle");
    if (client.pasteCall)
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::Busy, "this physical connection already holds a paste upload");
    if (!session->stream || client.pastePushCount >= maxPastePushesPerClient) {
        auto result = Codec::JSON::Value::object();
        result.set("pane", Codec::JSON::Value::number(manifest->pane));
        result.set("status", Codec::JSON::Value::string(std::string(Protocol::Control::pastePushStatusName(session->stream ? Protocol::Control::PastePushStatus::TooLarge : Protocol::Control::PastePushStatus::Unsupported))));
        result.set("path", Codec::JSON::Value::string(""));
        return Protocol::Control::rpcResult(request.idOrNull(), std::move(result));
    }
    std::uint32_t transfer = 0;
    for (std::size_t attempt = 0; attempt <= Protocol::Sync::maxPendingClipboardReads && client.nextPasteTransfer; ++attempt) {
        const auto candidate = client.nextPasteTransfer++;
        if (!client.syncConnection->isClipboardTransferInUse(candidate)) {
            transfer = candidate;
            break;
        }
    }
    Protocol::Control::PasteUpload upload;
    if (!transfer || !upload.begin(*manifest, transfer, [&](std::size_t bytes) {
            return admitsAssemblyCapacity(index, Client::pasteCallMetadataBytes + bytes);
        }))
        return Protocol::Control::rpcError(request.idOrNull(), ErrorCode::Busy, "the exact paste allocation cannot be admitted");
    const auto* attachment = client.syncConnection->attachment();
    client.pasteCall.emplace(Client::PasteCall {
        .callIndex = callIndex,
        .sessionID = session->id,
        .handle = manifest->pane,
        .paneID = session->screenSource->paneID(),
        .attachmentID = attachment->attachmentID,
        .role = client.syncConnection->grantedRole(),
        .principal = principalOf(client),
        .requestID = request.id.asString(),
        .deadlineMilliseconds = nowMilliseconds() + 30'000,
        .upload = std::move(upload),
        .reply = {},
        .refusal = {},
    });
    return std::nullopt;
}

bool Daemon::mayReceivePasteUpload(std::size_t index) const
{
    if (index >= m_clients.size())
        return false;
    const Client& client = m_clients[index];
    if (!client.pasteCall || !hasLiveStructureAttachment(client) || !tokenScopeAllows(client, Protocol::Sync::Scope::Drive)
        || (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr))))
        return false;
    const auto& held = *client.pasteCall;
    const auto* attachment = client.syncConnection->attachment();
    const auto subscription = std::ranges::find(client.paneSubscriptions, held.handle, &PaneSubscription::screenHandle);
    const auto foundSession = std::ranges::find_if(m_sessions, [&](const auto& session) { return session->id == held.sessionID; });
    const Session* session = foundSession == m_sessions.end() ? nullptr : foundSession->get();
    return attachment && attachment->attachmentID == held.attachmentID && client.syncConnection->grantedRole() == held.role
        && principalOf(client) == held.principal && subscription != client.paneSubscriptions.end() && subscription->sessionID == held.sessionID
        && client.syncConnection->paneView(held.handle).has_value() && session && session->screenSource && session->screenSource->paneID() == held.paneID;
}

Protocol::Sync::ClientBulkDisposition Daemon::receivePasteUpload(std::uint32_t clientID, const Protocol::Sync::BulkChunk& chunk)
{
    using Protocol::Sync::ClientBulkDisposition;
    const auto found = std::ranges::find(m_clients, clientID, &Client::clientID);
    if (found == m_clients.end() || !found->pasteCall || found->pasteCall->upload.transfer() != chunk.transfer)
        return ClientBulkDisposition::Unhandled;
    const auto index = static_cast<std::size_t>(found - m_clients.begin());
    auto& held = *found->pasteCall;
    const auto accepted = held.upload.accept(chunk, [&] { return !held.refusal && mayReceivePasteUpload(index) && nowMilliseconds() < held.deadlineMilliseconds; });
    if (accepted == Protocol::Control::PasteUploadProgress::Refused)
        return ClientBulkDisposition::Refused;
    held.hasReceivedChunk = true;
    held.deadlineMilliseconds = nowMilliseconds() + 30'000;
    return ClientBulkDisposition::Accepted;
}

Protocol::Control::PastePushReply Daemon::performPastePush(std::size_t index, Session& session, std::string_view mime, std::span<const std::uint8_t> bytes)
{
    using Protocol::Control::PastePushStatus;
    Protocol::Control::PastePushReply reply;
    if (!session.stream) {
        reply.status = PastePushStatus::Unsupported;
    } else if (bytes.size() > Protocol::Control::maxPastePushBytes || m_clients[index].pastePushCount >= maxPastePushesPerClient) {
        reply.status = PastePushStatus::TooLarge;
    } else {
        ++m_clients[index].pastePushCount;
        reply.status = landPastePush(mime, bytes, reply.path);
        if (reply.status == PastePushStatus::Landed) {
            // Session teardown owns every successfully landed file, including
            // a path whose input queue later refuses its injection.
            session.landedPastePaths.push_back(reply.path);
            stampInputOrigin(session, m_clients[index]);
            std::string typed;
            const bool isBracketed = session.term && session.term->modes.bracketedPaste;
            if (isBracketed)
                typed = "\033[200~";
            typed += reply.path;
            if (isBracketed)
                typed += "\033[201~";
            const int written = session.stream->writeAll({ typed.data(), typed.size() });
            if (written == std::to_underlying(PAL::StreamStatus::Again)) {
                reply.status = PastePushStatus::Busy;
            } else if (written == std::to_underlying(PAL::StreamStatus::Closed) || written == std::to_underlying(PAL::StreamStatus::Fatal)) {
                session.exitReason = "pty write failed during a paste.push injection";
                session.shellExited = true;
                reply.status = PastePushStatus::Unsupported;
            }
        }
    }
    Ubiquitty::PAL::logLine(stderr, "ubiquitty-host: paste.push mime={} bytes={} client={} status={} path={}",
        mime, bytes.size(), m_clients[index].peerProcess.empty() ? "remote" : m_clients[index].peerProcess, static_cast<unsigned>(reply.status), reply.path);
    return reply;
}

void Daemon::servicePasteUpload(std::size_t index)
{
    using Codec::JSON::Value;
    using Protocol::Control::ErrorCode;
    Client& client = m_clients[index];
    if (!client.pasteCall || !client.syncConnection || client.syncConnection->state() != Protocol::Sync::ConnectionState::Attached)
        return;
    auto& held = *client.pasteCall;
    const auto id = Value::string(held.requestID);
    const auto now = nowMilliseconds();
    if (!held.refusal && !mayReceivePasteUpload(index)) {
        held.refusal = Protocol::Control::rpcError(&id, ErrorCode::Forbidden, "the original paste attachment or Drive authority was withdrawn");
        held.upload.clear();
    } else if (!held.refusal && !held.reply && now >= held.deadlineMilliseconds) {
        held.refusal = Protocol::Control::rpcError(&id, ErrorCode::Busy, "the paste upload expired before completing");
        held.upload.clear();
    }
    if (!held.refusal && !held.reply && held.upload.isComplete()) {
        // All chunk callbacks have returned. No file write or path injection
        // occurs from inside a connection decoder or an authority query.
        held.reply = performPastePush(index, *findSession(held.sessionID), held.upload.request().mime, held.upload.data());
        held.upload.clear();
    }
    if (held.reply || held.refusal) {
        auto result = Value::object();
        if (held.reply) {
            result.set("pane", Value::number(held.handle));
            result.set("status", Value::string(std::string(Protocol::Control::pastePushStatusName(held.reply->status))));
            result.set("path", Value::string(held.reply->path));
        }
        const auto answer = held.refusal ? *held.refusal : Protocol::Control::rpcResult(&id, std::move(result));
        // Output backpressure retains the exact final outcome; retrying this
        // Result never lands or types the file a second time.
        if (client.syncConnection->answerCall(held.callIndex, Codec::JSON::serialize(answer)) != Protocol::Sync::CallAnswer::OverBulkLimit)
            client.pasteCall.reset();
        return;
    }
    if (!held.hasReceivedChunk && (!held.lastCreditMilliseconds || now - held.lastCreditMilliseconds >= 1000)) {
        const auto ready = Protocol::Control::pasteReadyNotify({ held.requestID, held.handle, held.upload.transfer() });
        if (!ready) {
            held.refusal = Protocol::Control::rpcError(&id, ErrorCode::InternalError, "the upload credit could not fit its canonical control frame");
            held.upload.clear();
        } else if (client.syncConnection->sendNotify(*ready)) {
            (void)held.upload.grantCredit(held.upload.transfer());
            held.lastCreditMilliseconds = now;
        }
    }
}

bool Daemon::serviceScreenAccessSubscriber(std::size_t index)
{
    Client& client = m_clients[index];
    const auto* record = client.syncConnection ? client.syncConnection->attachment() : nullptr;
    if (!client.helloSeen || client.didOverflow || client.sessionExitDeadlineMilliseconds
        || !hasLiveStructureAttachment(client) || isAgentClient(client)
        || !tokenScopeAllows(client, Protocol::Sync::Scope::Observe)
        || (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr)))
        || !record || record->kind == Protocol::Sync::ClientKind::Agent
        || client.syncConnection->grantedRole() == Protocol::Sync::Role::Agent
        || m_attachments->find(record->attachmentID) != record) {
        client.screenAccessReplay.stop();
        client.screenAccessSessions.clear();
        return true;
    }
    if (!client.screenAccessReplay.isSubscribed()
        || !std::ranges::equal(client.screenAccessSessions, client.paneSubscriptions, {}, std::identity {}, &PaneSubscription::sessionID)) {
        client.screenAccessSessions.clear();
        for (const auto& subscription : client.paneSubscriptions)
            client.screenAccessSessions.push_back(subscription.sessionID);
        // Adding a pane replays its retained history too. Per-pane consumers
        // deduplicate positions already seen, including reviewed records.
        client.screenAccessReplay.begin({ .after = 0, .isFromLatest = false }, m_events.latestCursor());
    }
    bool hasFailed = false;
    client.screenAccessReplay.service(m_events, [&](const Ubiquitty::Session::Event& event) { return event.kind == Ubiquitty::Session::EventKind::ScreenRead && client.subscriptionFor(event.sessionID); }, [&](const Ubiquitty::Session::Event& event) {
        if (event.kind == Ubiquitty::Session::EventKind::Gap)
            return true;
        const auto notification = Protocol::Control::eventsNotify(std::span(&event, 1));
        if (!notification) {
            hasFailed = true;
            return false;
        }
        const bool admitted = client.syncConnection->sendNotify(*notification);
        if (!admitted && client.syncConnection->state() == Protocol::Sync::ConnectionState::Attached
            && client.syncConnection->wantsInput() && !client.syncConnection->hasQueuedStreamFrames())
            hasFailed = true;
        return admitted; });
    return !hasFailed;
}

bool Daemon::serviceEventSubscriber(std::size_t index)
{
    if (index >= m_clients.size())
        return true;
    Client& client = m_clients[index];
    if (!client.eventReplay.isSubscribed() || !client.syncConnection || client.didOverflow)
        return true;
    if (!tokenScopeAllows(client, Protocol::Sync::Scope::Control)
        || (client.hasToken && client.tokenExpiryUnix && client.tokenExpiryUnix <= static_cast<std::uint64_t>(::time(nullptr)))) {
        client.eventReplay.stop();
        return true;
    }
    bool hasFailed = false;
    m_eventDeliveries += client.eventReplay.service(m_events, [&](const Ubiquitty::Session::Event& event) { return eventDisclosureFor(client, event) == EventDisclosure::Whole; }, [&](const Ubiquitty::Session::Event& event) {
            const auto notification = Protocol::Control::eventsNotify(std::span(&event, 1));
            if (!notification) {
                hasFailed = true;
                return false;
            }
            const bool admitted = client.syncConnection->sendNotify(*notification);
            if (!admitted && client.syncConnection->state() == Protocol::Sync::ConnectionState::Attached
                && client.syncConnection->wantsInput() && !client.syncConnection->hasQueuedStreamFrames())
                hasFailed = true;
            return admitted; });
    if (hasFailed)
        client.eventReplay.stop();
    return !hasFailed;
}

// One budgeted foreground reading feeds the pane facts and the event log.
void Daemon::probeForegroundIfDue(Session& s)
{
    if (!s.stream)
        return;
    PAL::ProcessFacts& process = s.stream->process();
    // Probe budget: foregroundHolder() costs several tty
    // ioctls + process sysctls + string allocations per call — too heavy per
    // frame on the PTY-read loop. One cheap foregroundProcessGroup() ioctl is
    // the change-detector: a moved group re-probes immediately, otherwise the
    // cached answer stands until the budget elapses. An unsettled probe (no
    // resolvable foreground group, or an empty name — the window right after
    // forkpty before the child execs — or a foreground still moving around
    // its name) never latches the budget, so the next frame probes again
    // until a real foreground shows.
    const std::optional<PAL::ProcessID> resolvedForegroundGroup = process.foregroundProcessGroup();
    const PAL::ProcessID foregroundGroup = resolvedForegroundGroup.value_or(0);
    if (s.term && Ubiquitty::retireTitleIfStale(*s.term, foregroundGroup)) {
        s.term->titleChanged = true;
        // A quiet exit can retire the title without another PTY read. Publish
        // its empty value and the replacement pane facts through the usual path.
        s.isFrameDue = true;
    }
    const std::uint64_t now = nowMilliseconds();
    if (s.foregroundProbeSettled && foregroundGroup == s.lastForegroundProbeGroup && now - s.lastForegroundProbeMilliseconds < foregroundProbeIntervalMilliseconds) {
        // Held back, not dropped: the loop makes it when the window closes.
        s.foregroundProbeOwedAtMilliseconds = s.lastForegroundProbeMilliseconds + foregroundProbeIntervalMilliseconds;
        return;
    }
    s.foregroundProbeOwedAtMilliseconds = 0;
    // Who holds the terminal is published beside the name, and a client keys
    // the pane's program by the pair, so both come from one reading of the
    // owner and are settled against the reading after them, as a local pane's
    // sample settles them. A reading that finds the pair the last settled
    // probe published moves nothing and is not read again; one still moving
    // leaves the probe unsettled, and the next frame probes again.
    PAL::ForegroundHolder holder = process.foregroundHolder();
    Ubiquitty::Session::ForegroundReading reading { .name = std::move(holder.name), .processGroup = foregroundGroup, .isShell = holder.isShell, .isSettled = false };
    reading.isSettled = s.foregroundProbeSettled && reading.isShell == s.lastForegroundIsShell && reading.name == s.lastForegroundName;
    reading = Ubiquitty::Session::settleForegroundReading(process, std::move(reading), [&process] { return process.foregroundHolder(); });
    Ubiquitty::Session::holdUnnamedReading(reading, s.lastForegroundName);
    s.isProbedForegroundShell = reading.isShell;
    s.probedForegroundName = std::move(reading.name);
    if (s.term && Mouse::retireTrackingIfStale(*s.term, s.mouseTrackingProcessGroup, reading.processGroup, s.isProbedForegroundShell))
        s.isFrameDue = true;
    // The directory stands in only while the shell reports none (OSC 7), so
    // it is read only then, and on this budget like the name beside it.
    s.probedForegroundDirectory = s.term && s.term->currentDirectory.empty() ? process.foregroundCurrentDirectory() : std::string {};
    s.lastForegroundProbeMilliseconds = now;
    s.lastForegroundProbeGroup = reading.processGroup;
    s.foregroundProbeSettled = reading.processGroup != 0 && !s.probedForegroundName.empty() && reading.isSettled;
    if (s.isProbedForegroundShell != s.lastForegroundIsShell || s.probedForegroundName != s.lastForegroundName) {
        // The shell has the terminal back: where it marks no prompt, that is
        // where a program's working or blocked record ends.
        if (s.isProbedForegroundShell && !s.lastForegroundIsShell && s.term)
            OSC::foregroundReturnedToShell(*s.term);
        s.lastForegroundIsShell = s.isProbedForegroundShell;
        s.lastForegroundName = s.probedForegroundName;
        ++s.foregroundGeneration;
        Ubiquitty::Session::Event moved;
        moved.kind = Ubiquitty::Session::EventKind::ForegroundChanged;
        moved.sessionID = s.id;
        moved.paneID = paneOfSession(s);
        moved.isAtPrompt = s.isProbedForegroundShell;
        moved.detail = s.probedForegroundName;
        recordEvent(std::move(moved));
    }
}

std::uint64_t Daemon::systemWallClockMilliseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint64_t Daemon::systemMonotonicMicroseconds()
{
    return nowMicroseconds();
}

void Daemon::stampTypedInput(Session& s)
{
    if (!s.screenSource)
        return;
    const std::uint64_t typedWrites = s.screenSource->typedInputWrites();
    if (typedWrites == s.stampedTypedInputWrites)
        return;
    s.stampedTypedInputWrites = typedWrites;
    s.activity.noteInput(m_wallClockMilliseconds());
    if (s.term && Modes::DisplayIdle::noteInput(*s.term, Ticks::fromMilliseconds(m_monotonicMicroseconds() / 1000)))
        s.isFrameDue = true;
    // Typing moves the stamp, and can move who owns the pane's size.
    noteRecordsDue(s, nowMilliseconds());
}

std::string Daemon::homeDirectoryOfProcess()
{
    const char* home = ::getenv("HOME");
    return home ? std::string(home) : std::string();
}

bool Daemon::isFactsRead(const Session& s) const
{
    return std::ranges::any_of(m_clients, [&](const Client& client) { return client.isListing || client.subscriptionFor(s.id) != nullptr; });
}

bool Daemon::isAttachedTo(const Session& s) const
{
    return std::ranges::any_of(m_clients, [&](const Client& client) { return client.subscriptionFor(s.id) != nullptr; });
}

void Daemon::noteRecordsDue(Session& s, std::uint64_t now)
{
    // Nothing is read, and nothing owed, for a pane no connection reads.
    if (!isFactsRead(s))
        return;
    // A pane only a listing reads is read at the listing's cadence; one a
    // client is attached to, on the probe's budget.
    const std::uint64_t interval = isAttachedTo(s) ? paneRecordsIntervalMilliseconds : listedRecordsIntervalMilliseconds;
    if (s.lastRecordsMilliseconds != 0 && now - s.lastRecordsMilliseconds < interval) {
        // Held back, not dropped: the loop reads them when the window closes.
        // An earlier wake already owed stands.
        const std::uint64_t owedAt = s.lastRecordsMilliseconds + interval;
        s.recordsOwedAtMilliseconds = s.recordsOwedAtMilliseconds ? std::min(s.recordsOwedAtMilliseconds, owedAt) : owedAt;
        return;
    }
    publishRecords(s, now);
}

bool Daemon::readsStructures(const Client& client) const
{
    if (!client.syncConnection || client.syncConnection->state() != Protocol::Sync::ConnectionState::Attached)
        return false;
    // A listing reads every workspace, whoever it is: its call was admitted
    // under the Control scope. Otherwise a structure names every pane of a
    // workspace, which a person attached to it draws and an agent on one
    // session has no business with.
    return client.isListing || (!isAgentClient(client) && !client.paneSubscriptions.empty());
}

Protocol::Sync::ListedDetail Daemon::listedDetailFor(const Client& client) const
{
    // The listing itself is admitted under Control. A pane's command line is
    // what its screen is: it goes to a connection whose credential would let
    // it attach and read that screen, which is the Observe scope, and a
    // connection without it is sent the names of what runs.
    return tokenScopeAllows(client, Protocol::Sync::Scope::Observe) ? Protocol::Sync::ListedDetail::Whole : Protocol::Sync::ListedDetail::WithoutArguments;
}

void Daemon::syncListedPanes(std::size_t index)
{
    Client& client = m_clients[index];
    if (!client.syncConnection)
        return;
    // A pane the client has bound has its records on the binding.
    std::erase_if(client.listedSessions, [&](const std::pair<std::uint32_t, std::uint32_t>& listed) {
        const Session* session = findSession(listed.first);
        if (client.isListing && session && session->screenSource && !client.subscriptionFor(listed.first))
            return false;
        client.syncConnection->unlistPane(listed.second);
        return true;
    });
    if (!client.isListing)
        return;
    for (const auto& session : m_sessions) {
        if (!session->screenSource || client.subscriptionFor(session->id) || std::ranges::find(client.listedSessions, session->id, &std::pair<std::uint32_t, std::uint32_t>::first) != client.listedSessions.end())
            continue;
        if (const std::uint32_t handle = client.syncConnection->listPane(*session->screenSource, listedDetailFor(client))) {
            client.listedSessions.emplace_back(session->id, handle);
            // The pane has a reader from here on.
            noteRecordsDue(*session, nowMilliseconds());
        }
    }
}

void Daemon::syncStructures()
{
    if (!m_isStructureDirty)
        return;
    // Nothing is built while nobody reads one; what changed stays noted for
    // the reader that arrives.
    if (std::ranges::none_of(m_clients, [&](const Client& client) { return readsStructures(client); }))
        return;
    m_isStructureDirty = false;
    using namespace Ubiquitty::Session;
    // A session no workspace holds is the root of one of its own, as it is
    // for every other reader of the host's model.
    for (std::size_t index = 0; index < m_sessions.size(); ++index) {
        if (!workspaceHolding(m_sessions[index]->id))
            (void)hostedWorkspaceFor(*m_sessions[index]);
    }
    std::vector<TreeWorkspaceSource> sources;
    sources.reserve(m_workspaces.size());
    for (const auto& workspace : m_workspaces) {
        if (workspace->isClosing)
            continue;
        sources.push_back({ .id = workspace->workspaceID, .name = workspace->name, .layout = &workspace->layout, .endsWhenDetached = workspace->shouldEndWhenDetached, .wasRestoredAfterHostRestart = workspace->wasRestoredAfterHostRestart });
    }
    std::vector<TreeRestorableSource> restorable;
    restorable.reserve(m_restorableWorkspaces.size());
    for (const RestorableWorkspace& kept : m_restorableWorkspaces)
        restorable.push_back({ .id = kept.workspaceID, .record = &kept.record });
    const SessionTree tree = buildSessionTree({ .revision = 0, .nowUnixMilliseconds = 0, .workspaces = sources, .tags = &m_tags, .restorable = restorable }, m_labels,
        [&](std::uint32_t workspaceID, Layout::PaneID pane) {
            TreePaneFacts facts;
            const auto workspace = std::ranges::find(m_workspaces, workspaceID, [](const auto& held) { return held->workspaceID; });
            const Session* session = workspace != m_workspaces.end() ? findSession((*workspace)->sessionFor(pane)) : nullptr;
            if (!session)
                return facts;
            facts.sessionID = session->id;
            if (session->screenSource) {
                facts.paneID = session->screenSource->paneID();
                facts.structuralGeneration = session->screenSource->structuralGeneration();
            }
            if (session->term) {
                facts.facts.rows = static_cast<std::uint16_t>(std::clamp(session->term->rows, 0, 0xFFFF));
                facts.facts.columns = static_cast<std::uint16_t>(std::clamp(session->term->columns, 0, 0xFFFF));
            }
            // Who is attached, as a number and a kind: no principal, no
            // label, nothing the attachments list keeps from a role.
            facts.attendees = attendeesOf(session->id);
            return facts;
        });
    // A structure that reads differently takes the next revision.
    std::vector<std::uint32_t> standing;
    for (TreeWorkspace workspace : tree.workspaces) {
        standing.push_back(workspace.id);
        workspace.revision = 0;
        std::vector<std::uint8_t> unrevised;
        if (!encodeWorkspaceStructure(unrevised, workspace)) {
            Ubiquitty::PAL::logLine(stderr, "daemon: workspace {} has a structure its record cannot say; its readers keep the last one", workspace.id);
            continue;
        }
        StructureRecord& kept = m_structureRecords[workspace.id];
        if (kept.record && kept.unrevised == unrevised)
            continue;
        ++kept.revision;
        std::string arrangement = Layout::arrangementSignature(workspace);
        if (!kept.record || arrangement != kept.arrangement) {
            kept.arrangement = std::move(arrangement);
            kept.arrangedAt = kept.revision;
        }
        auto record = std::make_shared<std::vector<std::uint8_t>>(unrevised);
        // The record opens with its revision, eight bytes little-endian.
        for (std::size_t byte = 0; byte < 8; ++byte)
            (*record)[byte] = static_cast<std::uint8_t>(kept.revision >> (8 * byte));
        kept.unrevised = std::move(unrevised);
        kept.record = std::move(record);
    }
    // A workspace the host no longer holds is withdrawn from everyone told
    // of it. Its revision is kept, so the id made again starts above it.
    for (auto& [workspaceID, kept] : m_structureRecords) {
        if (kept.record && std::ranges::find(standing, workspaceID) == standing.end()) {
            kept.record.reset();
            kept.unrevised.clear();
            for (Client& client : m_clients) {
                if (client.syncConnection)
                    client.syncConnection->withdrawWorkspaceStructure(workspaceID);
            }
        }
    }
    for (std::size_t index = 0; index < m_clients.size(); ++index) {
        Client& client = m_clients[index];
        syncListedPanes(index);
        if (!readsStructures(client))
            continue;
        for (const auto& [workspaceID, kept] : m_structureRecords) {
            if (!kept.record)
                continue;
            const bool isAttachedTo = std::ranges::any_of(client.paneSubscriptions, [&](const PaneSubscription& subscription) { // bool-ok: names the one state it reports
                const HostedWorkspace* holding = workspaceHolding(subscription.sessionID);
                return holding && holding->workspaceID == workspaceID;
            });
            if (client.isListing || isAttachedTo)
                client.syncConnection->offerWorkspaceStructure(workspaceID, kept.revision, kept.record);
        }
    }
}

Codec::JSON::Value Daemon::serveListing(std::size_t index, ListingChange change)
{
    Client& client = m_clients[index];
    Codec::JSON::Value result = Codec::JSON::Value::object();
    if (change == ListingChange::End) {
        // Unlist its panes; later structure publication offers no workspace but the
        // ones it is attached to. The call that carried this has already
        // asked for one.
        client.isListing = false;
        syncListedPanes(index);
        return result;
    }
    client.isListing = true;
    // A new subscriber must be offered unchanged structures too. Capture
    // current facts before stating the minimum revisions it must receive. A
    // reader that arrives asks for the foreground itself: a program that took
    // the terminal without printing has made no frame that would have.
    for (const auto& session : m_sessions) {
        if (!session || !session->screenSource)
            continue;
        probeForegroundIfDue(*session);
        (void)refreshPaneFacts(*session);
        publishRecords(*session, nowMilliseconds());
    }
    m_isStructureDirty = true;
    syncStructures();
    // Which workspaces to expect, so the client knows when it holds the
    // whole host, and the host's wall clock, which ages are read against.
    Codec::JSON::Value workspaces = Codec::JSON::Value::array();
    Codec::JSON::Value structureRevisions = Codec::JSON::Value::array();
    for (const auto& [workspaceID, kept] : m_structureRecords) {
        if (kept.record) {
            workspaces.push(Codec::JSON::Value::number(workspaceID));
            structureRevisions.push(Codec::JSON::Value::string(std::to_string(kept.revision)));
        }
    }
    Codec::JSON::Value paneRevisions = Codec::JSON::Value::array();
    for (const auto& session : m_sessions) {
        if (!session || !session->screenSource)
            continue;
        Codec::JSON::Value pane = Codec::JSON::Value::object();
        pane.set("session", Codec::JSON::Value::number(session->id));
        pane.set("revision", Codec::JSON::Value::number(session->screenSource->facts().revision));
        paneRevisions.push(std::move(pane));
    }
    result.set("workspaces", std::move(workspaces));
    result.set("structureRevisions", std::move(structureRevisions));
    result.set("paneRevisions", std::move(paneRevisions));
    result.set("panes", Codec::JSON::Value::number(static_cast<double>(client.listedSessions.size())));
    result.set("hostNowUnixMilliseconds", Codec::JSON::Value::number(static_cast<double>(m_wallClockMilliseconds())));
    if (!isAgentClient(client)) {
        auto instances = Codec::JSON::Value::array();
        for (const auto& instance : discoverHostInstances()) {
            auto value = Codec::JSON::Value::object();
            value.set("socket", Codec::JSON::Value::string(instance.socketName));
            value.set("executable", Codec::JSON::Value::string(instance.executable));
            value.set("startedUnixSeconds", Codec::JSON::Value::number(static_cast<double>(instance.startedUnixSeconds)));
            value.set("appVersion", Codec::JSON::Value::string(instance.appVersion));
            instances.push(std::move(value));
        }
        result.set("instances", std::move(instances));
    }
    return result;
}

void Daemon::serveOwedRecords(Session& s, std::uint64_t now)
{
    if (s.recordsOwedAtMilliseconds && now >= s.recordsOwedAtMilliseconds)
        publishRecords(s, now);
}

Protocol::Sync::PaneHostFacts Daemon::paneHostFactsOf(const Session& s) const
{
    Protocol::Sync::PaneHostFacts host;
    host.homeDirectory = m_paneHomeDirectory;
    host.foregroundDirectory = s.probedForegroundDirectory;
    host.processChain = s.paneFacts.processChain;
    host.processChainWithArguments = s.paneFacts.processChainWithArguments;
    host.shellCommandName = s.paneFacts.shellCommandName;
    host.terminalName = s.paneFacts.terminalName;
    host.hostName = m_paneHostName;
    host.foregroundLeadsTo = s.end.didEnd ? PAL::Process::RemoteKind::None : s.paneFacts.foregroundLeadsTo;
    host.end = s.end;
    host.foreground = { .processID = static_cast<std::uint32_t>(std::max(s.lastForegroundProbeGroup, 0)), .name = s.paneFacts.foreground.name, .generation = s.foregroundGeneration };
    host.isAtPrompt = s.isProbedForegroundShell;
    if (s.term) {
        const Ubiquitty::IntSize textArea = Ubiquitty::reportedTextAreaPixels(*s.term);
        const auto pixels = [](int value) { return static_cast<std::uint16_t>(std::clamp(value, 0, static_cast<int>(std::numeric_limits<std::uint16_t>::max()))); };
        host.pixels = { .cellWidth = pixels(s.term->cellPixelWidth), .cellHeight = pixels(s.term->cellPixelHeight), .textAreaWidth = pixels(textArea.width), .textAreaHeight = pixels(textArea.height) };
    }
    host.activity = { .lastOutputUnixMilliseconds = s.activity.lastOutputUnixMilliseconds, .lastInputUnixMilliseconds = s.activity.lastInputUnixMilliseconds };
    host.createdUnixSeconds = s.createdUnix;
    host.isAdopted = s.adopted;
    return host;
}

void Daemon::publishRecords(Session& s, std::uint64_t now)
{
    s.recordsOwedAtMilliseconds = 0;
    s.lastRecordsMilliseconds = now;
    if (!s.term || !s.screenSource)
        return;
    // Seen before read: a bell that rings in front of a person latches
    // nothing a listing would show.
    clearAttentionAPersonSaw(s);
    using Stamps = Protocol::Sync::PaneSource::ActivityStamps;
    const bool isStampDue = now - s.lastActivityStampMilliseconds >= activityStampIntervalMilliseconds;
    const std::uint32_t before = s.screenSource->facts().revision;
    s.screenSource->setFacts(Protocol::Sync::paneFactsOf(*s.term, paneHostFactsOf(s)), isStampDue ? Stamps::Publish : Stamps::RideAlong);
    if (isStampDue && s.screenSource->facts().revision != before)
        s.lastActivityStampMilliseconds = now;
    // The status set is the terminal's store, copied only when it moved.
    if (s.recordedActivityGeneration != s.term->activityGeneration) {
        s.recordedActivityGeneration = s.term->activityGeneration;
        s.screenSource->setStatus(Protocol::Sync::statusRecordsOf(*s.term));
    }
}

void Daemon::clearAttentionAPersonSaw(Session& s)
{
    if (!s.screenSource || !s.term)
        return;
    // A person who holds the focus is looking now, and one who gained and
    // lost it within the pass looked: either way what is latched was seen.
    const std::uint64_t focusGains = s.screenSource->personFocusGains();
    if (focusGains == s.seenPersonFocusGains && !s.screenSource->isAnyPersonFocused())
        return;
    s.seenPersonFocusGains = focusGains;
    (void)Ubiquitty::markAttentionSeen(*s.term);
}

void Daemon::serveOwedForegroundProbe(Session& s, std::uint64_t now)
{
    if (!s.foregroundProbeOwedAtMilliseconds || now < s.foregroundProbeOwedAtMilliseconds)
        return;
    s.foregroundProbeOwedAtMilliseconds = 0;
    // A pane whose facts nobody reads reports to nobody; a reader that
    // arrives asks for itself.
    if (!isFactsRead(s) || !s.stream || !s.screenSource)
        return;
    probeForegroundIfDue(s);
    (void)refreshPaneFacts(s);
    publishRecords(s, now);
}

namespace {

// What a tracker's watcher thread is handed: the session's flag and the
// loop's wake, each shared so neither dies under a notify in flight.
struct ProcessTreeNotice {
    std::shared_ptr<ProcessTreeWake> wake;
    std::shared_ptr<std::atomic<bool>> edge;
};

// Runs on the watcher thread, so it only raises the flag and wakes the loop.
bool noteProcessTreeEdge(void* context)
{
    const ProcessTreeNotice& notice = *static_cast<const ProcessTreeNotice*>(context);
    notice.edge->store(true, std::memory_order_release);
    if (!notice.wake->isPending.exchange(true, std::memory_order_acq_rel)) {
        constexpr char wakeByte = 1;
        ssize_t written;
        do {
            written = ::write(notice.wake->pipe.write.get(), &wakeByte, 1);
        } while (written < 0 && errno == EINTR);
        // A full pipe already holds a byte the loop has yet to drain, so the
        // wake stands. Any other refusal leaves no byte behind, and the next
        // edge must try again rather than find the wake marked pending.
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            notice.wake->isPending.store(false, std::memory_order_release);
    }
    return true;
}

// The tracker calls this once, after its watcher thread is joined.
void releaseProcessTreeNotice(void* context)
{
    std::unique_ptr<ProcessTreeNotice>(static_cast<ProcessTreeNotice*>(context)).reset();
}

bool makeLoopDescriptor(const PAL::UnixFileDescriptor& descriptor)
{
    const int statusFlags = ::fcntl(descriptor.get(), F_GETFL, 0);
    const int descriptorFlags = ::fcntl(descriptor.get(), F_GETFD, 0);
    return statusFlags != -1 && descriptorFlags != -1
        && ::fcntl(descriptor.get(), F_SETFL, statusFlags | O_NONBLOCK) != -1
        && ::fcntl(descriptor.get(), F_SETFD, descriptorFlags | FD_CLOEXEC) != -1;
}

} // namespace

void Daemon::watchProcessTree(Session& s)
{
    if (s.processTreeEdge || !s.stream)
        return;
    s.processTreeEdge = std::make_shared<std::atomic<bool>>(false);
    PAL::ProcessFacts& process = s.stream->process();
    s.isTreeEdgeTracked = process.deliversTreeEdges();
    if (!s.isTreeEdgeTracked)
        return;
    if (!m_processTreeWake) {
        auto wake = std::make_shared<ProcessTreeWake>();
        wake->pipe = PAL::makeUnixPipe();
        // Without the wake an edge would raise a flag nobody looks at, so the
        // session falls back to the cadence that needs none.
        if (!wake->pipe || !makeLoopDescriptor(wake->pipe.read) || !makeLoopDescriptor(wake->pipe.write)) {
            s.isTreeEdgeTracked = false;
            return;
        }
        m_processTreeWake = std::move(wake);
    }
    auto notice = std::make_unique<ProcessTreeNotice>(ProcessTreeNotice { .wake = m_processTreeWake, .edge = s.processTreeEdge });
    process.setTreeChangeNotifier(&noteProcessTreeEdge, notice.release(), &releaseProcessTreeNotice);
}

void Daemon::takeProcessTreeEdges(std::uint64_t now)
{
    if (!m_processTreeWake)
        return;
    // Cleared before the flags are read: an edge raised after this writes a
    // fresh byte, and one raised before it is in a flag read below.
    m_processTreeWake->isPending.store(false, std::memory_order_release);
    std::array<char, 64> drained {};
    while (::read(m_processTreeWake->pipe.read.get(), drained.data(), drained.size()) > 0)
        continue;
    for (auto& sp : m_sessions) {
        if (!sp->processTreeEdge || !sp->processTreeEdge->exchange(false, std::memory_order_acq_rel))
            continue;
        sp->latestTreeEdgeMilliseconds = now;
        if (!sp->firstUnansweredTreeEdgeMilliseconds)
            sp->firstUnansweredTreeEdgeMilliseconds = now;
    }
}

std::uint64_t Daemon::processTreeProbeDueAt(const Session& s) const
{
    if (!s.stream || s.shellExited)
        return 0;
    if (s.isTreeEdgeTracked) {
        if (!s.firstUnansweredTreeEdgeMilliseconds)
            return 0;
        const std::uint64_t settled = std::min(s.latestTreeEdgeMilliseconds + Title::edgeSettleMilliseconds,
            s.firstUnansweredTreeEdgeMilliseconds + Title::edgeMaximumDeferMilliseconds);
        return std::max(settled, s.lastForegroundProbeMilliseconds + Title::edgeFloorMilliseconds);
    }
    // Nothing announces a change here, so it is looked for, and only for a
    // pane whose facts someone reads: a reader that arrives asks for itself.
    if (!s.processTreeEdge || !isFactsRead(s))
        return 0;
    return std::max<std::uint64_t>(s.lastForegroundProbeMilliseconds + Title::walkFallbackMilliseconds, 1);
}

void Daemon::serveProcessTreeProbe(Session& s, std::uint64_t now)
{
    const std::uint64_t due = processTreeProbeDueAt(s);
    if (!due || now < due)
        return;
    s.firstUnansweredTreeEdgeMilliseconds = 0;
    s.latestTreeEdgeMilliseconds = 0;
    if (!isFactsRead(s) || !s.screenSource)
        return;
    // The fallback is paced by the last probe, so one is made even when the
    // foreground group stands still; a frame's probe would hold it back.
    if (!s.isTreeEdgeTracked)
        s.lastForegroundProbeMilliseconds = 0;
    probeForegroundIfDue(s);
    if (!s.isTreeEdgeTracked && !s.lastForegroundProbeMilliseconds)
        s.lastForegroundProbeMilliseconds = now;
    (void)refreshPaneFacts(s);
    publishRecords(s, now);
}

// The pane facts a title is woven from, read the way a local pane's runtime
// reads them (Session::walkedProcessFacts), so a host pane titles the same as
// the shell would in a local window: argv[0] with its login dash, not the
// executable's file name.
//
// The walk is the costly half. It runs when the foreground can have moved, on
// the same gate the runtime uses; a platform whose walk only descends the
// session's own subtree also walks on the foreground probe's cadence, because
// a program that already holds the terminal can spawn a child without moving
// either the group or the name.
bool Daemon::refreshPaneFacts(Session& s)
{
    if (!s.stream || !s.term)
        return false;
    PAL::ProcessFacts& process = s.stream->process();
    Protocol::Sync::PaneHostFacts next = s.paneFacts;
    // OSC 7 is often silent (a shell that sets only its OSC title), so the
    // foreground process's real directory stands in and the folder still shows.
    // Both it and the name are the budgeted probe's reading: a probe here
    // would be one per frame per attached pane, which is what the budget is
    // for.
    next.foregroundDirectory = s.probedForegroundDirectory;
    next.foreground.name = s.probedForegroundName;
    // A forked child that has not exec'd yet still wears this process's
    // image; naming the pane after the host is worse than naming nothing.
    if (next.foreground.name == PAL::Process::hostProcessName())
        next.foreground.name.clear();

    const auto foregroundGroup = static_cast<std::uint32_t>(process.foregroundProcessGroup().value_or(0));
    const std::uint64_t now = nowMilliseconds();
    const bool isWalkDue = (process.hasCheapProcessWalk() && now - s.lastPaneFactsWalkMilliseconds >= foregroundProbeIntervalMilliseconds)
        || PAL::Process::shouldRewalkForegroundChain(foregroundGroup, s.paneFactsChainProcessGroup, !s.paneFacts.processChain.empty(),
            next.foreground.name != s.paneFacts.foreground.name, s.paneFactsChainForegroundCount, false);
    if (isWalkDue) {
        s.lastPaneFactsWalkMilliseconds = now;
        m_processWalkRecords.resize(Ubiquitty::Session::processChainWalkCapacity);
        const int found = process.gatherProcesses(m_processWalkRecords);
        // An empty walk leaves the last answer standing: a transient failure
        // to read the process table is not the shell going away, and the
        // session's end has its own signal.
        if (found > 0) {
            const std::span<const PAL::Process::Record> walked { m_processWalkRecords.data(), static_cast<std::size_t>(found) };
            s.paneFactsChainProcessGroup = foregroundGroup;
            s.paneFactsChainForegroundCount = static_cast<int>(std::ranges::count_if(walked, [](const PAL::Process::Record& record) { return record.isForeground; }));
            Ubiquitty::Session::WalkedProcessFacts walkedFacts = Ubiquitty::Session::walkedProcessFacts(walked);
            next.processChain = std::move(walkedFacts.processChain);
            next.processChainWithArguments = std::move(walkedFacts.processChainWithArguments);
            next.shellCommandName = std::move(walkedFacts.shellCommandName);
            next.terminalName = std::move(walkedFacts.leaderTTYName);
            next.foregroundLeadsTo = PAL::Process::remoteTargetForChain(walked).kind;
        }
    }
    // Before the first walk resolves, the stream's own answers stand in, as
    // they do for a local pane's launch seed and tty.
    if (next.processChain.empty() && next.processChainWithArguments.empty())
        next.processChainWithArguments = next.processChain = next.foreground.name;
    if (next.terminalName.empty())
        next.terminalName = process.ttyName();

    if (next == s.paneFacts)
        return false;
    s.paneFacts = std::move(next);
    return true;
}

// The loop.

// Whether this daemon has had a session and has none now, with nothing left to
// deliver about the one that went.
//
// A client draining a session exit still counts as work: it was sent an EXIT
// and has a deadline to read it by, and quitting under it would truncate the
// frame that says the session ended.
bool Daemon::hasOutlivedItsLastSession() const
{
    if (!m_everHadSession || !m_sessions.empty() || m_pendingLocalAdopt
        || (m_localAdoptTransactions && m_localAdoptTransactions->outcomeCount()))
        return false;
    return !std::ranges::any_of(m_clients, [](const auto& client) {
        return client.sessionExitDeadlineMilliseconds && !client.didOverflow;
    });
}

int Daemon::run()
{
    std::vector<struct pollfd> pfds;
    std::vector<uint32_t> sessionSlotIDs; // pfds[sessionBase+k] -> session id
    for (;;) {
        if (m_localAdoptTransactions) {
            const auto advanced = m_localAdoptTransactions->advanceTime(nowMicroseconds());
            if (!advanced) {
                // Refuse admission on a broken clock without discarding
                // retained outcomes or any committed session owner.
                for (std::size_t index = 0; index < m_clients.size(); ++index) {
                    if (m_clients[index].localHandoffCorrelation)
                        dropClient(index);
                }
            }
        }
        if (m_quiescingPanes) {
            // No carrier reads, QUIC callbacks, geometry changes, maintenance
            // or session reaping may mutate a pane held for transfer. The
            // private handover owner ends this state on release or refusal.
            serviceHandoverQuiesce(nowMicroseconds());
            if (m_quiescingPanes)
                (void)::poll(nullptr, 0, 1);
            continue;
        }
        if (isStopRequested)
            break;
        // A client still greeting may be about to ask for a session, so the
        // request waits for every connection to greet or be dropped for not
        // greeting in time. A successor's handover connection is the pending
        // handover's.
        const auto isGreeting = [](const Client& client) { return !client.helloSeen && !client.didOverflow && !client.localHandoffCorrelation; };
        if (isIdleStopRequested && std::ranges::none_of(m_clients, isGreeting)) {
            isIdleStopRequested = 0;
            const bool isHoldingNothing = m_sessions.empty() && m_workspaces.empty() && !m_pendingLocalAdopt && !m_pendingHandover
                && !(m_localAdoptTransactions && m_localAdoptTransactions->outcomeCount());
            if (isHoldingNothing) {
                Ubiquitty::PAL::logLine(stderr, "ubiquitty-host: asked to stop while it holds nothing; it stops");
                break;
            }
            Ubiquitty::PAL::logLine(stderr, "ubiquitty-host: asked to stop while it holds {} sessions; it keeps serving", m_sessions.size());
        }
        // A credential reload was requested: the consent list on disk is the
        // truth again (a grant issued or withdrawn from outside), and any
        // live client whose token no longer validates is dropped, before the
        // usual reap pass reaps them.
        if (gTokenStoreChanged) {
            gTokenStoreChanged = 0;
            reloadCredentialStores();
        }
        // An edit no one announced is found here too, before this pass serves
        // anything that store-bound authority admits.
        if (holdsStoreBoundAuthority())
            refreshCredentialStoresIfChanged();
        if (isHoldStateRequested) {
            isHoldStateRequested = 0;
            logHoldState();
        }

        // A token that lapsed under a live connection takes the connection with
        // it, ahead of the reap pass that carries the denial out.
        dropExpiredTokenClients();

        // Drop clients that overran the output high-water cap, and reap any
        // connection that never HELLO'd within the grace window (an
        // accept-and-idle fd-exhaustion probe). From the end so erases keep
        // the lower indices valid.
        {
            const std::uint64_t reapNow = nowMilliseconds();
            for (std::size_t i = m_clients.size(); i-- > 0;) {
                auto& c = m_clients[i];
                if (isReapDue(c, reapNow))
                    reapClient(i);
                else if (c.consentPending && !c.attachConsentTerminal && reapNow >= c.consentDeadlineMilliseconds)
                    denyAgentAttach(i, Protocol::Sync::ConsentStatusPhase::Expired); // fail closed
            }
        }

        serviceLocalAdoptRetirement();

        // Drop peers that pin per-client / aggregate inbound memory or stall a
        // large partial frame past its deadline.
        enforceInboundBudget();
        // Same for queued OUTBOUND memory — bound the total across all clients.
        expireClipboardTools();
        expireCapabilityQuestions(nowMilliseconds());
        enforceOutboundBudget();

        // GC expired QUIC bootstrap tokens even when idle — an unused,
        // possibly-leaked token must not stay valid past its TTL.
        pruneExpiredQUICTokens(nowMilliseconds());

        // Departures above may have retired the last query and left only its
        // queued outcome. Service those bytes before constructing poll events.
        serviceRichTextMeasurements(nowMicroseconds());

        pfds.clear();
        sessionSlotIDs.clear();
        pfds.push_back({ m_listenFileDescriptor.get(), POLLIN, 0 });
        // Predecessors' listeners follow the daemon's own, in order.
        for (const auto& listener : m_inheritedListeners)
            pfds.push_back({ listener.descriptor.get(), POLLIN, 0 });

        // The QUIC data-plane UDP socket, right after the listen sockets.
        [[maybe_unused]] std::size_t quicSlot = SIZE_MAX;
#ifdef UBIQUITTY_QUIC
        if (m_quic && m_quic->fileDescriptor() >= 0) {
            quicSlot = pfds.size();
            pfds.push_back({ m_quic->fileDescriptor(), POLLIN, 0 });
        }
#endif

        for (auto& sp : m_sessions)
            watchProcessTree(*sp);
        std::size_t processTreeWakeSlot = SIZE_MAX;
        if (m_processTreeWake) {
            processTreeWakeSlot = pfds.size();
            pfds.push_back({ m_processTreeWake->pipe.read.get(), POLLIN, 0 });
        }

        std::size_t sessionBase = pfds.size();
        for (auto& sp : m_sessions) {
            int fileDescriptor = sp->stream->fileDescriptor();
            // A session waiting on its child's exit status has nothing left to
            // read, and its hung-up descriptor would wake the loop every pass.
            if (fileDescriptor < 0 || sp->shellExited)
                continue;
            // A pending handover reads no pane, so no client is owed more.
            short ev = m_pendingHandover ? 0 : POLLIN;
            if (sp->stream->hasPendingWrite())
                ev |= POLLOUT; // drain queued input (writeAll) when writable
            if (!ev)
                continue;
            pfds.push_back({ fileDescriptor, ev, 0 });
            sessionSlotIDs.push_back(sp->id);
        }
        std::size_t clientBase = pfds.size();
        for (auto& c : m_clients) {
            // A dropped client is not read: polling it for input would wake the
            // loop on every byte it sends while its Bye drains.
            short ev = isInputHeld(c) || c.didOverflow ? 0 : POLLIN;
            if (c.hasPendingOutput())
                ev |= POLLOUT;
            pfds.push_back({ c.fileDescriptor.get(), ev, 0 });
        }

        // Timeout: the soonest throttled-but-due snapshot across all sessions.
        int timeout = scrollbackMaintenanceTimeout(nowMilliseconds());
        if (std::ranges::any_of(m_localAdoptAuthorities, [](const auto& record) { return record.lifetime.has_value(); }))
            timeout = timeout < 0 ? localAdoptMaintenanceMilliseconds : std::min(timeout, localAdoptMaintenanceMilliseconds);
        std::uint64_t t0 = nowMilliseconds();
        for (const auto& client : m_clients) {
            if (client.writeFailureDeadlineMilliseconds) {
                const int remaining = wakeAfterMilliseconds(client.writeFailureDeadlineMilliseconds, t0);
                timeout = timeout < 0 ? remaining : std::min(timeout, remaining);
            }
            if (client.clipboardToolDeadlineMilliseconds) {
                const int remaining = static_cast<int>(client.clipboardToolDeadlineMilliseconds > t0 ? client.clipboardToolDeadlineMilliseconds - t0 : 0);
                timeout = timeout < 0 ? remaining : std::min(timeout, remaining);
            }
        }
        for (const auto& [requestID, pending] : m_pendingCapabilityGrants) {
            const int remaining = wakeAfterMilliseconds(pending.deadlineMilliseconds, t0);
            timeout = timeout < 0 ? remaining : std::min(timeout, remaining);
        }
        // A foreground probe or a records read the budget held back, when
        // its window closes.
        for (const auto& sp : m_sessions) {
            for (const std::uint64_t owedAt : { sp->foregroundProbeOwedAtMilliseconds, sp->recordsOwedAtMilliseconds, processTreeProbeDueAt(*sp) }) {
                if (!owedAt)
                    continue;
                const int t = static_cast<int>(owedAt > t0 ? std::min<std::uint64_t>(owedAt - t0, INT_MAX) : 0);
                if (timeout < 0 || t < timeout)
                    timeout = t;
            }
        }
        for (auto& sp : m_sessions) {
            if (sp->term) {
                const Ticks idleNow = Ticks::fromMilliseconds(m_monotonicMicroseconds() / 1000);
                if (Modes::DisplayIdle::advance(*sp->term, idleNow))
                    sp->isFrameDue = true;
                if (const auto remaining = Modes::DisplayIdle::ticksUntilDeadline(*sp->term, idleNow)) {
                    const int delay = static_cast<int>((*remaining * 1000ull + 59) / 60);
                    if (timeout < 0 || delay < timeout)
                        timeout = delay;
                }
            }
            if (!sp->isFrameDue)
                continue;
            // A host change not yet noted is asked at once; a held frame when
            // the rule says it may answer differently, rounded up so the wake
            // is never early.
            int t = 0;
            if (sp->term && sp->coherence.isDirty()) {
                const std::uint64_t askedAt = m_monotonicMicroseconds();
                const std::optional<std::uint64_t> wait = sp->coherence.microsecondsUntilAsk(*sp->term, askedAt, Ticks::fromMilliseconds(askedAt / 1000));
                t = wait ? static_cast<int>(std::min<std::uint64_t>((*wait + 999) / 1000, INT_MAX)) : 0;
            }
            if (timeout < 0 || t < timeout)
                timeout = t;
        }

        // A connection's rate interval, probe or datagram loss timeout, which
        // nothing the client sends and no publish would wake the loop for.
        // Rounded up so the wake is never early.
        if (const std::uint64_t screenNow = nowMicroseconds(); const std::optional<std::uint64_t> due = nextScreenServiceMicroseconds(screenNow)) {
            const int t = static_cast<int>(std::min<std::uint64_t>((*due > screenNow ? *due - screenNow + 999 : 0) / 1000, INT_MAX));
            if (timeout < 0 || t < timeout)
                timeout = t;
        }

        // If any client hasn't HELLO'd yet, wake within the grace window so
        // the pre-HELLO idle reap runs on time even when nothing else would
        // unblock poll().
        for (const auto& c : m_clients) {
            if (!c.helloSeen) {
                if (timeout < 0 || timeout > static_cast<int>(helloGraceMilliseconds))
                    timeout = static_cast<int>(helloGraceMilliseconds);
                break;
            }
        }

        // Likewise wake when a held agent's consent deadline elapses so the
        // fail-safe timeout-deny fires on time.
        for (const auto& c : m_clients) {
            if (!c.consentPending || c.attachConsentTerminal)
                continue;
            const std::uint64_t nowForConsent = nowMilliseconds();
            const int t = wakeAfterMilliseconds(c.consentDeadlineMilliseconds, nowForConsent);
            if (timeout < 0 || t < timeout)
                timeout = t;
        }
        for (const auto& [requestID, pending] : m_structureConsents) {
            (void)requestID;
            const int t = wakeAfterMilliseconds(pending.deadlineMilliseconds, nowMilliseconds());
            if (timeout < 0 || t < timeout)
                timeout = t;
        }

        // Wake when a detached session's window closes, so it ends on schedule
        // rather than at whatever unrelated traffic arrives next.
        for (const auto& sp : m_sessions) {
            if (sp->graceExpiresMilliseconds == 0)
                continue;
            const std::uint64_t nowForGrace = nowMilliseconds();
            const int t = wakeAfterMilliseconds(sp->graceExpiresMilliseconds, nowForGrace);
            if (timeout < 0 || t < timeout)
                timeout = t;
        }

        // A query owed a driving face cannot wait for unrelated PTY traffic.
        for (const auto& sp : m_sessions) {
            const std::uint64_t deadline = sp->runFaces.heldDeadlineMilliseconds();
            if (!deadline)
                continue;
            const int t = wakeAfterMilliseconds(deadline, nowMilliseconds());
            if (timeout < 0 || t < timeout)
                timeout = t;
        }

        // A client dropped for its backlog is reaped when its Bye's wait ends,
        // whether or not anything else wakes the loop.
        for (const auto& client : m_clients) {
            if (!client.backlogByeDeadlineMilliseconds || !client.hasPendingOutput())
                continue;
            const int remaining = wakeAfterMilliseconds(client.backlogByeDeadlineMilliseconds, nowMilliseconds());
            if (timeout < 0 || remaining < timeout)
                timeout = remaining;
        }

        // A peer that stops reading after session exit cannot retain its
        // carrier indefinitely, even when there is no other host activity.
        for (const auto& client : m_clients) {
            if (!client.sessionExitDeadlineMilliseconds)
                continue;
            const auto nowForExit = nowMilliseconds();
            const int remaining = wakeAfterMilliseconds(client.sessionExitDeadlineMilliseconds, nowForExit);
            if (timeout < 0 || remaining < timeout)
                timeout = remaining;
        }

        // Wake when a client's partial-frame deadline elapses so enforceInboundBudget
        // drops a stalled peer even with no further inbound bytes.
        for (const auto& c : m_clients) {
            if (c.partialFrameDeadlineMilliseconds == 0)
                continue;
            const std::uint64_t nowForFrame = nowMilliseconds();
            const int t = wakeAfterMilliseconds(c.partialFrameDeadlineMilliseconds, nowForFrame);
            if (timeout < 0 || t < timeout)
                timeout = t;
        }

        // Wake when a session waiting on its child's exit status is due to be
        // reaped whether or not the status arrived.
        for (const auto& sp : m_sessions) {
            if (!sp->exitStatusDeadlineMilliseconds)
                continue;
            const int t = std::min(wakeAfterMilliseconds(sp->exitStatusDeadlineMilliseconds, nowMilliseconds()), static_cast<int>(exitStatusPollMilliseconds));
            if (timeout < 0 || t < timeout)
                timeout = t;
        }

        // Wake when the record of the named workspaces is owed to disk.
        if (m_layoutRecordWrite.isOwed()) {
            const int t = wakeAfterMilliseconds(m_layoutRecordWrite.dueMilliseconds, nowMilliseconds());
            if (timeout < 0 || t < timeout)
                timeout = t;
        }

        if (m_backgroundEventWriteDue) {
            const int remaining = wakeAfterMilliseconds(m_backgroundEventWriteDue, nowMilliseconds());
            if (timeout < 0 || remaining < timeout) {
                timeout = remaining;
            }
        }

        // Wake when a live token reaches its expiry, so the connection it
        // authenticated ends on time on an idle link too. This is the one
        // deadline kept in seconds rather than milliseconds.
        if (const std::uint64_t expiryUnix = earliestTokenExpiryUnix(); expiryUnix != 0) {
            const int t = wakeAfterSeconds(expiryUnix, static_cast<std::uint64_t>(::time(nullptr)));
            if (timeout < 0 || t < timeout)
                timeout = t;
        }

        // Wake at the soonest QUIC bootstrap-token deadline so an unused token
        // is GC'd on time even with no traffic. Unconditional — the
        // pending set + GC are not QUIC-build-gated, only the mint trigger is.
        if (const std::uint64_t tokenDeadline = earliestQUICTokenDeadline(); tokenDeadline != 0) {
            const std::uint64_t nowForToken = nowMilliseconds();
            const int t = wakeAfterMilliseconds(tokenDeadline, nowForToken);
            if (timeout < 0 || t < timeout)
                timeout = t;
        }

#ifdef UBIQUITTY_QUIC
        // Wake when a QUIC timer is due (loss/PTO retransmit, keepalive PING,
        // idle timeout) so recovery fires on schedule even with no inbound
        // datagram — without this the server only serviced timers opportunistically
        // on a read, stalling retransmits on a lossy/quiet reverse path.
        if (m_quic) {
            const int quicTimeout = m_quic->nextTimeoutMilliseconds();
            if (quicTimeout >= 0 && (timeout < 0 || quicTimeout < timeout))
                timeout = quicTimeout;
        }
#endif
        // A pending handover is looked at again soon after each write, and
        // once its wait ends.
        if (m_pendingHandover) {
            constexpr int pendingHandoverPollMilliseconds = 5;
            timeout = timeout < 0 ? pendingHandoverPollMilliseconds : std::min(timeout, pendingHandoverPollMilliseconds);
        }

        // The same question, before committing to a wait. It can become true
        // partway through this iteration — the reaper above drops the last
        // client that was still draining a session exit — and the check at the
        // bottom is then unreachable: with no sessions and nothing pending the
        // timeout computed above is -1, so the poll blocks until some event
        // arrives, and for a daemon whose last session is gone there may be
        // none. It waited on clients that had nothing left to send.
        if (hasOutlivedItsLastSession())
            break;
        // Every entry polls a distinct open descriptor, and descriptors are ints.
        int r = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        // Expiry also precedes readable-client dispatch: a client's signal
        // may bypass a backed-up PTY even without a POLLOUT event this pass.
        dropExpiredTokenClients();
        reapAttachments(nowMicroseconds());

        // Sessions: drain queued input on POLLOUT, then service PTY output.
        for (std::size_t k = 0; k < sessionSlotIDs.size(); ++k) {
            const short rev = pfds[sessionBase + k].revents;
            if (rev == 0)
                continue;
            Session* s = findSession(sessionSlotIDs[k]);
            if (!s)
                continue;
            if ((rev & POLLOUT) && !s->shellExited) {
                drainRemoteInput(*s);
            }
            if (!m_pendingHandover && (rev & (POLLIN | POLLHUP | POLLERR)))
                servicePTY(*s);
        }

        // Accept new clients.
        if (pfds[0].revents & POLLIN)
            acceptClient();
        for (std::size_t index = 0; index < m_inheritedListeners.size(); ++index) {
            if (pfds[1 + index].revents & POLLIN)
                acceptClient(m_inheritedListeners[index].descriptor.get());
        }

        if (processTreeWakeSlot != SIZE_MAX && (pfds[processTreeWakeSlot].revents & POLLIN))
            takeProcessTreeEdges(nowMilliseconds());

#ifdef UBIQUITTY_QUIC
        // Service the QUIC data-plane: drive handshakes + deliver inbound
        // stream bytes (onQUICStreamData → handleFrame). Frames those
        // handlers queue flush below via m_quic->flush().
        if (quicSlot != SIZE_MAX && (pfds[quicSlot].revents & (POLLIN | POLLERR | POLLHUP)))
            m_quic->processReadable();
#endif

        // Service existing clients, matched by fd (handlers can drop/shift).
        for (std::size_t k = clientBase; k < pfds.size(); ++k) {
            int fileDescriptor = pfds[k].fd;
            short rev = pfds[k].revents;
            if (rev == 0)
                continue;
            std::size_t index = m_clients.size();
            for (std::size_t i = 0; i < m_clients.size(); ++i)
                if (m_clients[i].fileDescriptor.get() == fileDescriptor) {
                    index = i;
                    break;
                }
            if (index >= m_clients.size())
                continue;
            if (rev & POLLOUT)
                flushClient(index);
            if (index >= m_clients.size() || m_clients[index].fileDescriptor.get() != fileDescriptor)
                continue;
            // poll reports a hangup whether or not POLLIN was asked for, so a
            // held client that hung up would wake the loop on every pass. What
            // it still had to send sits behind answers it does not read.
            if ((rev & (POLLHUP | POLLERR)) && (isInputHeld(m_clients[index]) || m_clients[index].didOverflow)) {
                m_clients[index].backlogByeDeadlineMilliseconds = 0;
                dropClient(index);
                continue;
            }
            serviceClient(index);
            if (m_hasRelinquishedService)
                break;
        }
        if (m_hasRelinquishedService) {
            // The successor holds every master and keeps the records this
            // host wrote; only this process's own socket and lock files go.
            teardownAll();
            cleanup();
            return 0;
        }

        // The geometry the viewports of the pass left each session due, once.
        for (auto& sp : m_sessions)
            applyPendingGeometry(*sp);

        // Whoever typed during the pass, stamped once per pane.
        for (auto& sp : m_sessions)
            stampTypedInput(*sp);

        // The bell and notification a person's focus during the pass saw.
        for (auto& sp : m_sessions)
            clearAttentionAPersonSaw(*sp);

        // The foreground probes frames asked for inside the budget, now due,
        // and the records.
        for (auto& sp : m_sessions) {
            serveOwedForegroundProbe(*sp, nowMilliseconds());
            serveProcessTreeProbe(*sp, nowMilliseconds());
            serveOwedRecords(*sp, nowMilliseconds());
        }

        // Frames, per session, when the coherence rule admits them.
        for (auto& sp : m_sessions)
            (void)publishFrameIfAdmitted(*sp);

        // What a call served last pass left to end, ended where no
        // connection is being served.
        settleOwedClosures();

        // The workspaces' structures, when something they say changed, go to
        // their readers behind the frames the publishes above took.
        syncStructures();

        // Every screen connection: what the publishes, the acknowledgements
        // read this pass and the timers made due. A pane whose terminal moved
        // on since its last frame sends nothing until the rule takes the next.
        for (auto& sp : m_sessions) {
            if (sp->isFrameDue && sp->screenSource)
                sp->screenSource->noteTerminalChanged();
        }
        settleDueInput(nowMicroseconds());
        reapAttachments(nowMicroseconds());
        expireStructureConsents(nowMilliseconds());
        // A full effect log can become writable when an acknowledgement or
        // Lost-reader expiry was processed, even without another PTY byte.
        for (auto& session : m_sessions) {
            if (!session->isFrameDue && typedEffectPrimaryForSession(*session)) {
                emitPendingBell(*session);
                emitPendingSounds(*session);
                if (session->term && session->lastEmittedCommandGeneration != session->term->commandCompletedGeneration)
                    emitAgentEvents(*session);
            }
            deliverSessionEffects(*session);
        }
        for (std::size_t i = 0; i < m_clients.size(); ++i)
            serviceSyncConnection(i, nowMicroseconds());

        // Client frames may have supplied the faces for a query even when
        // its program writes no more output. Release in that same turn.
        for (auto& session : m_sessions)
            refreshRunFaces(*session);

        // Push any client outputBuffers that flushClient couldn't. For QUIC
        // clients flushClient hands the bytes to sendStream (queue only);
        // m_quic->flush() below does the actual UDP write.
        for (std::size_t i = 0; i < m_clients.size();) {
            std::size_t before = m_clients.size();
            flushClient(i);
            if (m_clients.size() == before)
                ++i;
        }
#ifdef UBIQUITTY_QUIC
        if (m_quic) {
            m_quic->serviceTimers(); // fire due loss/PTO/keepalive/idle timers
            m_quic->flush(); // write queued outbound stream bytes + timer-driven packets
        }
#endif
        // What this pass wrote may have been the last a client was owed.
        HandoverStartup::servicePending(*this, nowMilliseconds());
        if (m_hasRelinquishedService) {
            teardownAll();
            cleanup();
            return 0;
        }

        // End the ends-when-detached sessions whose window has run out, before
        // the reap below reads what is left — a session ended here is one the
        // last-session check at the bottom of this pass can act on.
        serviceDetachedSessions(nowMilliseconds());

        // Reap exited sessions: final frame, then EXIT + dispose. Collect
        // ids first (destroySession mutates m_sessions).
        std::vector<uint32_t> exited;
        for (auto& sp : m_sessions) {
            if (!sp->shellExited)
                continue;
            // End of file on the master usually arrives a moment before the
            // child's exit status. Whether the child exited or was killed by a
            // signal decides what its workspace's record keeps, so the reap
            // waits a short, bounded moment for the status to land.
            if (sp->stream && sp->stream->process().hasRunningChild()) {
                const std::uint64_t now = nowMilliseconds();
                if (!sp->exitStatusDeadlineMilliseconds)
                    sp->exitStatusDeadlineMilliseconds = now + exitStatusWaitMilliseconds;
                if (now < sp->exitStatusDeadlineMilliseconds)
                    continue;
            }
            exited.push_back(sp->id);
            // Reaping the session ends every client on it, which from a peer is
            // the same event as a budget drop: the socket closes mid-write. The
            // four caps name themselves, so a drop none of them claims is
            // attributed here or nowhere.
            //
            // Reported here rather than where the condition fired, because the
            // child's exit status is not yet knowable there. hasRunningChild
            // re-probes waitpid, which is what turns "not reaped" into a number
            // on the common path: EOF arrives when the replica's last descriptor
            // closes, and the exit that follows lands after the reader saw it.
            const bool isChildStillRunning = sp->stream && sp->stream->process().hasRunningChild();
            const PAL::ChildExit exit = sp->stream ? sp->stream->childExit() : PAL::ChildExit {};
            sp->wasKilledBySignal = exit.didExit && exit.wasSignalled;
            sp->end = { .didEnd = true, .hasStatus = exit.didExit, .wasSignalled = exit.didExit && exit.wasSignalled, .code = exit.didExit ? exit.code : 0 };
            Ubiquitty::PAL::logLine(stderr, "daemon: session {} reaped — {}, child {}; every client on it is sent an exit",
                sp->id, sp->exitReason.empty() ? "no reason recorded" : sp->exitReason.c_str(),
                exit.didExit              ? (exit.wasSignalled ? std::format("killed by signal {}", exit.code)
                                                               : std::format("exited {}", exit.code))
                    : isChildStillRunning ? std::string("still running")
                                          : std::string("gone, status taken elsewhere"));
            const Protocol::Sync::PublishCensus& census = sp->census;
            using Runtime::FrameClass;
            Ubiquitty::PAL::logLine(stderr,
                "daemon: session {} published {} boundary, {} settled, {} flood, {} forced and {} structural frames; "
                "{} inside a cursor-hidden redraw; longest hold {} us; {} geometry changes applied",
                sp->id, census.frameCount(FrameClass::Boundary), census.frameCount(FrameClass::Settled),
                census.frameCount(FrameClass::Flood), census.frameCount(FrameClass::Forced),
                census.frameCount(FrameClass::Structural), census.redrawBracketFrameCount(), census.longestHoldMicroseconds(),
                sp->geometryChangesApplied);
        }
        for (uint32_t id : exited) {
            Session* s = findSession(id);
            if (s && s->term) {
                OSC::finishProgramActivity(*s->term);
                s->term->contexts = {};
                OSC::activityChanged(*s->term);
                // How it ended is in the pane's last facts, whatever the
                // budget says; then the last frame goes out ahead of the
                // exit, as far as each client's window lets it.
                publishRecords(*s, nowMilliseconds());
                publishFrame(*s, Runtime::FrameClass::Structural);
                for (std::size_t i = 0; i < m_clients.size(); ++i) {
                    if (m_clients[i].subscriptionFor(id))
                        serviceSyncConnection(i, nowMicroseconds());
                }
            }
            destroySession(id);
        }

        if (m_layoutRecordWrite.isDue(nowMilliseconds()))
            writeLayoutRecords();
        if (m_backgroundEventWriteDue && nowMilliseconds() >= m_backgroundEventWriteDue) {
            (void)writeBackgroundEvents();
        }

        // Input, frame delivery and reaping precede bounded cold-history work.
        serviceScrollbackMaintenance(nowMilliseconds());

        // Multiplexer lifetime: exit once the last session is gone.
        if (hasOutlivedItsLastSession())
            break;
    }

    // The last session ended, or a stop was asked for. Whatever the record
    // still owes is written either way: an ending, so no record offers a
    // workspace that is over, or what is live, for the next host to restore.
    if (m_layoutRecordWrite.isOwed())
        writeLayoutRecords();
    (void)writeBackgroundEvents();
    teardownAll();
    cleanup();
    return 0;
}

} // namespace Host
} // namespace Ubiquitty
