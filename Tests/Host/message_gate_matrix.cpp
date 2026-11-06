// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Canonical frame and method admission against verified peer, role, token and
// scope. The daemon seam also drives local descriptor handoff, typed file.read,
// host-incarnation pins, consent refusal and geometry without starting a PTY.
// Socketpairs and filesystem fixtures are owned by this standalone Host test.

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/FrameGates.h"
#include "Core/Protocol/Sync/HostManagement.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"
#include "TrustedLocalPeer.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace Ubiquitty::Host {

/// Seeds verified fixture facts, never a caller-claimed promotion to Local.
struct MessageGateAccess {
    /// Pins the executables a local adopt may come from, as a daemon does when
    /// it starts serving; one that never starts admits no adopt at all.
    static void trustLocalAdopt(Daemon& daemon) { daemon.captureLocalAdoptExecutables(); }

    static void addClient(Daemon& daemon, const std::string& peerProcess, const std::string& agentLabel,
        bool hasToken, Protocol::Sync::ScopeSet scope) // bool-ok: credential-presence axis.
    {
        Daemon::Client client {};
        client.clientID = daemon.m_nextClientID++;
        client.peerProcess = peerProcess;
        client.hasPeerBinary = binaryIdentityOf(peerProcess, client.peerBinary);
        client.peerProcessID = peerProcess.empty() ? 0 : static_cast<std::uint32_t>(::getpid());
        client.agentLabel = agentLabel;
        client.hasToken = hasToken;
        client.tokenScope = scope;
        client.helloSeen = true;
        daemon.m_clients.push_back(std::move(client));
    }

    static void clearOutput(Daemon& daemon, std::size_t index) { daemon.m_clients[index].clearPendingOutput(); }

    /// Keeps the peer alive while a separately tagged local readiness reply drains.
    static PAL::UnixFileDescriptor giveLocalSocket(Daemon& daemon, std::size_t index)
    {
        int descriptors[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
            std::perror("message gate local socket");
            std::abort();
        }
        daemon.m_clients[index].fileDescriptor = PAL::UnixFileDescriptor(descriptors[0]);
        daemon.m_clients[index].hasOwnerLocalCredentials = true;
        return PAL::UnixFileDescriptor(descriptors[1]);
    }

    static void giveAdoptDescriptor(Daemon& daemon, std::size_t index)
    {
        int descriptors[2];
        if (::pipe(descriptors) != 0) {
            std::perror("message gate adopt descriptor");
            std::abort();
        }
        ::close(descriptors[1]);
        daemon.m_clients[index].pendingAdoptFileDescriptor = PAL::UnixFileDescriptor(descriptors[0]);
    }

    static std::size_t heldAdoptBytes(const Daemon& daemon, std::size_t index)
    {
        const auto& reader = daemon.m_clients[index].localAdopt;
        return reader ? reader->pendingBytes() : 0;
    }
    static std::size_t retainedAdoptBytes(const Daemon& daemon, std::size_t index)
    {
        const auto& reader = daemon.m_clients[index].localAdopt;
        return reader ? reader->retainedBytes() : 0;
    }
    static bool holdsDescriptor(const Daemon& daemon, std::size_t index) { return static_cast<bool>(daemon.m_clients[index].pendingAdoptFileDescriptor); }
    static void forgetHello(Daemon& daemon, std::size_t index) { daemon.m_clients[index].helloSeen = false; }
    static void setInteractiveUser(Daemon& daemon, std::size_t index, bool enabled) // bool-ok: authenticated interactive-user fact.
    {
        daemon.m_clients[index].isInteractiveUser = enabled;
    }
    /// What start() reads from UBIQUITTY_REQUIRE_TOKEN; a Daemon built without start() requires one.
    static void setTokenRequirement(Daemon& daemon, bool isRequired) // bool-ok: the daemon's own switch, both positions iterated.
    {
        daemon.m_requireToken = isRequired;
    }
    static int sessionColumns(Daemon& daemon, std::uint32_t sessionID) { return daemon.findSession(sessionID)->term->columns; }
    static std::uint64_t daemonInstanceID(const Daemon& daemon) { return daemon.m_daemonInstanceID; }

    /// Runs the real first-byte selector and canonical reader. The basename is
    /// an explicit in-memory fixture fact, not an opened or unlinked path.
    static void feed(Daemon& daemon, std::size_t index, std::span<const std::uint8_t> bytes)
    {
        const std::string savedPath = std::exchange(daemon.m_socketPath, "message-gate-fixture.sock");
        daemon.feedClientFrames(index, std::as_bytes(bytes));
        daemon.m_socketPath = savedPath;
    }

    static std::optional<Protocol::Sync::ByeReason> refusal(const Daemon& daemon, std::size_t index)
    {
        const auto* connection = daemon.m_clients[index].syncConnection.get();
        return connection ? connection->byeReason() : std::nullopt;
    }

    static bool hasGreeted(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].helloSeen; }
    static std::size_t takeCallCount(Daemon& daemon, std::size_t index)
    {
        auto* connection = daemon.m_clients[index].syncConnection.get();
        return connection ? connection->takeCalls().size() : 0;
    }
    static std::size_t sessionCount(const Daemon& daemon) { return daemon.m_sessions.size(); }
    static CapabilityOrigin originOf(const Daemon& daemon, std::size_t index) { return daemon.originOf(daemon.m_clients[index]); }

    static std::uint32_t addSession(Daemon& d, std::uint32_t id)
    {
        auto session = std::make_unique<Daemon::Session>();
        session->id = id;
        d.m_sessions.push_back(std::move(session));
        return id;
    }

    static void attachClientToSession(Daemon& d, std::size_t index, std::uint32_t sessionID)
    {
        d.m_clients[index].sessionID = sessionID;
        d.m_clients[index].paneSubscriptions = { { .sessionID = sessionID } };
        d.m_clients[index].workspaceID = sessionID;
    }

    static bool consentPending(const Daemon& d, std::size_t index) { return d.m_clients[index].consentPending; }

    /// Whether the daemon is closing a client: marked for its reap, or
    /// draining to its close.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: Whether the loop will reap it.
    static bool isLeaving(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow || d.m_clients[index].sessionExitDeadlineMilliseconds != 0; }

    // Give a session the directory a file read is confined to. It rides the
    // session's process rather than its Terminal: the Terminal's own
    // currentDirectory is written by OSC 7, which is the session's output and
    // therefore the thing confinement must not take its root from.
    static void setSessionDirectory(Daemon& d, std::uint32_t sessionID, const std::string& directory)
    {
        for (auto& session : d.m_sessions) {
            if (!session || session->id != sessionID)
                continue;
            session->term = std::make_unique<Ubiquitty::Terminal>();
            Ubiquitty::hardReset(*session->term);
            session->term->currentDirectory = directory;
            auto stream = std::make_unique<TestDoubles::StreamDouble>();
            stream->facts.currentDirectory = directory;
            session->stream = std::move(stream);
        }
    }

    static void setFileRequestCount(Daemon& d, std::size_t index, std::uint32_t count)
    {
        d.m_clients[index].fileRequestCount = count;
    }

    /// Calls the production typed dispatcher with the exact bound pane handle.
    /// - Parameters:
    ///   - d: The fixture daemon.
    ///   - index: Its exact caller.
    ///   - path: The path to test under that caller's authority.
    /// - Returns: Its file status (Denied for an RPC refusal), or -1 for malformed data.
    static int readFile(Daemon& d, std::size_t index, const std::string& path)
    {
        auto& client = d.m_clients[index];
        if (!client.paneSubscriptions.empty())
            client.paneSubscriptions.front().screenHandle = 1;
        const auto request = Protocol::Control::decodeRequest(Protocol::Control::fileReadRequest(1, path, "file-test"), {});
        const auto answer = d.serveScreenCall(client, request);
        const auto reply = Protocol::Control::decodeFileReadReply(answer, "file-test", 1, path);
        return reply ? static_cast<int>(reply->status) : -1;
    }

    // Who the daemon says a client is to a Sync call, were its Greet to ask
    // for the most its credential allows: the role is the ceiling, since no
    // screen connection has greeted here.
    static Protocol::Sync::ListedDetail listedDetail(const Daemon& d, std::size_t index)
    {
        return d.listedDetailFor(d.m_clients[index]);
    }

    static Protocol::Control::CallerAuthority callerAuthority(const Daemon& d, std::size_t index)
    {
        Protocol::Control::CallerAuthority caller = d.callerAuthorityOf(d.m_clients[index]);
        caller.role = d.maxRoleOf(d.m_clients[index]);
        return caller;
    }

    static bool isClosed(const Daemon& daemon, std::size_t index)
    {
        return isLeaving(daemon, index) || (daemon.m_clients[index].syncConnection && daemon.m_clients[index].syncConnection->state() == Protocol::Sync::ConnectionState::Closed);
    }
};

} // namespace Ubiquitty::Host

using Ubiquitty::CapabilityOrigin;
using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::MessageGateAccess;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Local = Ubiquitty::Protocol::Local;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) // bool-ok: assertion value.
{
    if (!ok) {
        std::fprintf(stderr, "message_gate_matrix: FAIL — %s\n", what.c_str());
        ++failures;
    }
}

std::string trustedLocalPeer() { return Ubiquitty::Host::trustedLocalPeerPath(); }

struct Principal {
    const char* name;
    std::string peerProcess;
    const char* agentLabel;
    CapabilityOrigin expectedOrigin;
    bool isInteractiveUser = false;
};

const Principal principals[] = {
    { "remote (no verified local identity)", "", "", CapabilityOrigin::Remote },
    { "agent (verified local, self-labeled)", "/usr/local/bin/some-agent", "mcp-agent", CapabilityOrigin::Agent },
    { "unlabeled local stranger", "/usr/local/bin/some-agent", "", CapabilityOrigin::Agent },
    { "local bridge", trustedLocalPeer(), "", CapabilityOrigin::Local },
    { "interactive QUIC user", "", "", CapabilityOrigin::Remote, true },
};

enum class Token {
    Absent,
    Present
};
enum class Handshake {
    NotYet,
    Done
};
enum class Descriptor {
    Absent,
    Brought
};

template<typename Message>
std::vector<std::uint8_t> wireFrame(Sync::FrameKind kind, std::uint32_t channel, const Message& message)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, message);
    std::vector<std::uint8_t> wire;
    check(Sync::appendFrame(wire, kind, channel, body), "the fixture emits a bounded canonical envelope");
    return wire;
}

Sync::Greet sessionGreeting(std::uint32_t session)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.clientInstance = { 7, 8 };
    greeting.request.sessionID = session;
    greeting.request.isAttachOnly = true;
    return greeting;
}

void canonicalIngressDoesNotInventAuthority()
{
    // Every row has one live wire kind and the exact role policy, independently
    // of a message body that could otherwise fail for an unrelated reason.
    for (unsigned raw = 0; raw <= 255; ++raw) {
        const auto kind = Sync::frameKindOf(static_cast<std::uint8_t>(raw));
        for (const auto role : { Sync::Role::Owner, Sync::Role::Driver, Sync::Role::Viewer, Sync::Role::Agent }) {
            const auto count = kind ? std::ranges::count(Sync::frameGates, *kind, &Sync::FrameGate::kind) : 0;
            check(count == (kind && Sync::directionOf(*kind) != Sync::Direction::HostToClient ? 1 : 0),
                "each client kind has exactly one live role row");
            const bool drives = kind && (*kind == Sync::FrameKind::Raw || *kind == Sync::FrameKind::Keys || *kind == Sync::FrameKind::Pointer || *kind == Sync::FrameKind::Paste || *kind == Sync::FrameKind::PasteAbort || *kind == Sync::FrameKind::BulkChunk);
            const bool ownsGeometryOrClipboard = kind && (*kind == Sync::FrameKind::Viewport || *kind == Sync::FrameKind::ClipboardData);
            const bool isNativeDrag = kind && *kind == Sync::FrameKind::NativeDragEvent;
            const bool expected = kind && Sync::directionOf(*kind) != Sync::Direction::HostToClient
                && (role == Sync::Role::Owner || role == Sync::Role::Driver
                    || (role == Sync::Role::Agent ? !ownsGeometryOrClipboard && !isNativeDrag : !ownsGeometryOrClipboard && !isNativeDrag && !drives));
            check(Sync::admitsSender(static_cast<Sync::FrameKind>(raw), role) == expected,
                "role rows never promote a viewer or agent to human geometry, clipboard or native-drag standing");
        }
    }

    for (const auto& row : Sync::frameGates) {
        {
            Daemon daemon;
            MessageGateAccess::addClient(daemon, trustedLocalPeer(), "", true,
                { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::Manage });
            MessageGateAccess::forgetHello(daemon, 0);
            const std::array<std::uint8_t, 3> wrongScope { std::to_underlying(row.kind),
                static_cast<std::uint8_t>(Sync::isConnectionKind(row.kind) ? 1 : 0), 0 };
            MessageGateAccess::feed(daemon, 0, wrongScope);
            check(MessageGateAccess::refusal(daemon, 0) == Sync::ByeReason::Malformed,
                "no connection command can enter on a pane channel, or pane command on channel zero");
            check(!MessageGateAccess::hasGreeted(daemon, 0) && MessageGateAccess::takeCallCount(daemon, 0) == 0,
                "a misaddressed header establishes no authority or callback");
        }
        if (row.kind == Sync::FrameKind::Greet || row.kind == Sync::FrameKind::Bye)
            continue;
        Daemon daemon;
        MessageGateAccess::addClient(daemon, trustedLocalPeer(), "", true,
            { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::Manage });
        MessageGateAccess::forgetHello(daemon, 0);
        std::vector<std::uint8_t> wire;
        check(Sync::appendFrame(wire, row.kind, Sync::isConnectionKind(row.kind) ? 0 : 1, {}),
            "the pre-Greet fixture uses the kind's real channel scope");
        MessageGateAccess::feed(daemon, 0, wire);
        check(MessageGateAccess::refusal(daemon, 0) == Sync::ByeReason::Malformed,
            "operation-specific grants cannot substitute for Greet");
        check(!MessageGateAccess::hasGreeted(daemon, 0) && MessageGateAccess::takeCallCount(daemon, 0) == 0
                && MessageGateAccess::sessionCount(daemon) == 0,
            "a pre-Greet frame causes no callback, attachment or session");
    }

    for (const auto kind : { Sync::FrameKind::Welcome, Sync::FrameKind::Result, Sync::FrameKind::PaneFacts }) {
        Daemon daemon;
        MessageGateAccess::addClient(daemon, trustedLocalPeer(), "", true, { Sync::Scope::Observe, Sync::Scope::Drive });
        MessageGateAccess::forgetHello(daemon, 0);
        std::vector<std::uint8_t> wire;
        check(Sync::appendFrame(wire, kind, Sync::isConnectionKind(kind) ? 0 : 1, {}), "host-only test scope is well formed");
        MessageGateAccess::feed(daemon, 0, wire);
        check(MessageGateAccess::refusal(daemon, 0) == Sync::ByeReason::Malformed,
            "a host-only frame cannot enter through the client dispatcher");
    }
    for (const std::uint8_t unknown : std::array<std::uint8_t, 3> { 0, 35, 255 }) {
        Daemon daemon;
        MessageGateAccess::addClient(daemon, trustedLocalPeer(), "", true, { Sync::Scope::Observe, Sync::Scope::Drive });
        MessageGateAccess::forgetHello(daemon, 0);
        const std::array bytes { unknown };
        MessageGateAccess::feed(daemon, 0, bytes);
        check(MessageGateAccess::refusal(daemon, 0) == Sync::ByeReason::UnknownKind,
            "unknown and retired envelope kinds close before any handler");
    }

    const auto whole = wireFrame(Sync::FrameKind::Greet, 0, sessionGreeting(88));
    for (std::size_t cut = 0; cut < whole.size(); ++cut) {
        Daemon daemon;
        MessageGateAccess::addClient(daemon, trustedLocalPeer(), "", true, { Sync::Scope::Observe, Sync::Scope::Drive });
        MessageGateAccess::forgetHello(daemon, 0);
        MessageGateAccess::feed(daemon, 0, std::span(whole).first(cut));
        check(!MessageGateAccess::hasGreeted(daemon, 0) && MessageGateAccess::takeCallCount(daemon, 0) == 0
                && MessageGateAccess::sessionCount(daemon) == 0,
            "every truncated Greet prefix leaves authority and session state untouched");
    }
}

void aPartOfAnAdoptIsHeldOnlyOnTheLocalAdoptPath()
{
    constexpr std::array<std::uint8_t, 7> part { 0, 0xFF, 0x1B, 7, 0, 0x80, 1 };
    constexpr std::size_t prefixBytes = Local::adoptWireFrameHeaderBytes + 3;
    constexpr std::size_t recordBytes = Local::adoptWirePrepareHeaderBytes + part.size();
    const auto heldFor = [&](const Principal& principal, Handshake handshake, Descriptor descriptor, Token token) {
        Daemon daemon;
        MessageGateAccess::trustLocalAdopt(daemon);
        MessageGateAccess::addClient(daemon, principal.peerProcess, principal.agentLabel, token == Token::Present,
            { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
        MessageGateAccess::setInteractiveUser(daemon, 0, principal.isInteractiveUser);
        if (handshake == Handshake::NotYet)
            MessageGateAccess::forgetHello(daemon, 0);
        Ubiquitty::PAL::UnixFileDescriptor peer;
        if (!principal.peerProcess.empty())
            peer = MessageGateAccess::giveLocalSocket(daemon, 0);
        const auto selected = Local::encodeLocalHandoffRequest(1);
        check(selected.has_value(), "a local operation has an exact nonzero correlation");
        if (selected)
            MessageGateAccess::feed(daemon, 0, *selected);
        if (!MessageGateAccess::isClosed(daemon, 0) && descriptor == Descriptor::Brought)
            MessageGateAccess::giveAdoptDescriptor(daemon, 0);
        Local::AdoptWirePrepare prepare { .correlation = 2,
            .preparation = { .operation = { .operationID = { .low = 3, .high = 5 }, .daemonInstanceID = MessageGateAccess::daemonInstanceID(daemon) },
                .bodyBytes = part.size() },
            .terminalBody = part };
        Ubiquitty::Crypto::sha256(part, prepare.preparation.bodySHA256);
        const auto encoded = Local::encodeAdoptWirePrepare(prepare, recordBytes);
        check(encoded.has_value(), "the fixture encodes a canonical complete-record Prepare header");
        if (encoded)
            MessageGateAccess::feed(daemon, 0, std::span(encoded->header).first(prefixBytes));
        const auto held = MessageGateAccess::heldAdoptBytes(daemon, 0);
        check(MessageGateAccess::retainedAdoptBytes(daemon, 0) == (held ? recordBytes : 0),
            "the selected raw Prepare charges its complete record owner before retaining its partial body");
        return MessageGateAccess::heldAdoptBytes(daemon, 0);
    };
    for (const Principal& principal : principals) {
        const bool isLocal = principal.expectedOrigin == CapabilityOrigin::Local;
        for (const Token token : { Token::Absent, Token::Present }) {
            check((heldFor(principal, Handshake::NotYet, Descriptor::Brought, token) == prefixBytes) == isLocal,
                std::string("local Prepare / ") + principal.name + ": only the trusted local owner retains it");
            check(heldFor(principal, Handshake::NotYet, Descriptor::Absent, token) == 0,
                "a selected local lane without a descriptor retains no adopt");
            check(heldFor(principal, Handshake::Done, Descriptor::Brought, token) == 0,
                "a remote greeting cannot be reused as local descriptor authority");
        }
    }

    const std::array suffixes {
        wireFrame(Sync::FrameKind::Greet, 0, sessionGreeting(88)),
        wireFrame(Sync::FrameKind::Bye, 0, Sync::Bye { .reason = Sync::ByeReason::Normal }),
        wireFrame(Sync::FrameKind::Call, 0, Sync::Call { .text = R"({"jsonrpc":"2.0","id":1,"method":"host.describe","params":{}})" }),
    };
    for (const auto& suffix : suffixes) {
        Daemon daemon;
        MessageGateAccess::trustLocalAdopt(daemon);
        MessageGateAccess::addClient(daemon, trustedLocalPeer(), "", false, {});
        MessageGateAccess::forgetHello(daemon, 0);
        auto peer = MessageGateAccess::giveLocalSocket(daemon, 0);
        const auto selected = Local::encodeLocalHandoffRequest(1);
        if (selected)
            MessageGateAccess::feed(daemon, 0, *selected);
        const auto status = Local::encodeAdoptWireCommand({ .kind = Local::AdoptWireMessage::Status, .correlation = 2, .operation = { .operationID = { .low = 3, .high = 5 }, .daemonInstanceID = MessageGateAccess::daemonInstanceID(daemon) } });
        check(status.has_value(), "a fresh authenticated Status carries no descriptor");
        if (status)
            MessageGateAccess::feed(daemon, 0, *status);
        check(!MessageGateAccess::isClosed(daemon, 0) && MessageGateAccess::heldAdoptBytes(daemon, 0) == 0
                && MessageGateAccess::retainedAdoptBytes(daemon, 0) == 0,
            "Unknown keeps the selected channel open at a clean record boundary");
        MessageGateAccess::feed(daemon, 0, suffix);
        // A short canonical envelope is only a partial local header. Finish
        // those framing bytes before expecting this separate grammar to refuse.
        if (suffix.size() < Local::adoptWireFrameHeaderBytes) {
            const std::array<std::uint8_t, Local::adoptWireFrameHeaderBytes> padding {};
            MessageGateAccess::feed(daemon, 0, std::span(padding).first(Local::adoptWireFrameHeaderBytes - suffix.size()));
        }
        check(MessageGateAccess::isClosed(daemon, 0) && MessageGateAccess::heldAdoptBytes(daemon, 0) == 0
                && MessageGateAccess::retainedAdoptBytes(daemon, 0) == 0
                && !MessageGateAccess::holdsDescriptor(daemon, 0),
            "non-Adopt traffic cannot enter the selected local grammar or retain an owner");
    }
}

void canonicalGreetingKeepsGeometryAndCallbacksWithTheirOwner()
{
    {
        Daemon daemon;
        MessageGateAccess::addSession(daemon, 88);
        MessageGateAccess::setSessionDirectory(daemon, 88, "/tmp");
        MessageGateAccess::addClient(daemon, trustedLocalPeer(), "", false, {});
        MessageGateAccess::forgetHello(daemon, 0);
        auto control = sessionGreeting(0);
        control.request.target = Sync::AdmissionTarget::Control;
        control.request.isAttachOnly = false;
        auto bytes = wireFrame(Sync::FrameKind::Greet, 0, control);
        auto replacement = sessionGreeting(88);
        replacement.facts.viewportColumns = 120;
        replacement.facts.viewportRows = 40;
        const auto suffix = wireFrame(Sync::FrameKind::Greet, 0, replacement);
        bytes.insert(bytes.end(), suffix.begin(), suffix.end());
        const int originalColumns = MessageGateAccess::sessionColumns(daemon, 88);
        MessageGateAccess::feed(daemon, 0, bytes);
        check(MessageGateAccess::refusal(daemon, 0) == Sync::ByeReason::Malformed,
            "a coalesced second Greet cannot replace an unbound target's authority");
        check(MessageGateAccess::sessionColumns(daemon, 88) == originalColumns
                && MessageGateAccess::sessionCount(daemon) == 1 && MessageGateAccess::takeCallCount(daemon, 0) == 0,
            "repeated admission neither resizes another pane nor delivers a callback");
    }
    for (const bool isCurrentInstance : { false, true }) {
        Daemon previous;
        Daemon daemon;
        MessageGateAccess::addSession(daemon, 88);
        MessageGateAccess::setSessionDirectory(daemon, 88, "/tmp");
        MessageGateAccess::addClient(daemon, "", "", true, { Sync::Scope::Observe, Sync::Scope::Drive });
        MessageGateAccess::setInteractiveUser(daemon, 0, true);
        MessageGateAccess::forgetHello(daemon, 0);
        const int originalColumns = MessageGateAccess::sessionColumns(daemon, 88);
        auto greeting = sessionGreeting(88);
        greeting.facts.viewportColumns = 120;
        greeting.facts.viewportRows = 40;
        greeting.request.expectedDaemonInstanceID = MessageGateAccess::daemonInstanceID(isCurrentInstance ? daemon : previous);
        check(greeting.request.expectedDaemonInstanceID != 0, "a daemon has a nonzero serving incarnation");
        MessageGateAccess::feed(daemon, 0, wireFrame(Sync::FrameKind::Greet, 0, greeting));
        check(MessageGateAccess::sessionColumns(daemon, 88) == (isCurrentInstance ? 120 : originalColumns),
            "an old daemon pin cannot resize a reused session number");
        check(MessageGateAccess::refusal(daemon, 0) == (isCurrentInstance ? std::optional<Sync::ByeReason> {} : Sync::ByeReason::SessionGone),
            "only the matching serving incarnation admits the saved reference");
    }

    for (const bool isInteractiveUser : { false, true }) {
        for (const bool mayDrive : { false, true }) {
            Daemon daemon;
            MessageGateAccess::addSession(daemon, 88);
            MessageGateAccess::setSessionDirectory(daemon, 88, "/tmp");
            Sync::ScopeSet scopes { Sync::Scope::Observe };
            if (mayDrive)
                scopes.add(Sync::Scope::Drive);
            MessageGateAccess::addClient(daemon, "", isInteractiveUser ? "" : "test-agent", true, scopes);
            MessageGateAccess::setInteractiveUser(daemon, 0, isInteractiveUser);
            MessageGateAccess::forgetHello(daemon, 0);
            const int originalColumns = MessageGateAccess::sessionColumns(daemon, 88);
            auto greeting = sessionGreeting(88);
            greeting.request.agentLabel = isInteractiveUser ? "" : "test-agent";
            greeting.facts.viewportColumns = 120;
            greeting.facts.viewportRows = 40;
            auto wire = wireFrame(Sync::FrameKind::Greet, 0, greeting);
            const auto destructive = wireFrame(Sync::FrameKind::Call, 0,
                Sync::Call { .text = R"({"jsonrpc":"2.0","id":"must-not-run","method":"session.terminate","params":{"session":88}})" });
            if (!isInteractiveUser)
                wire.insert(wire.end(), destructive.begin(), destructive.end());
            MessageGateAccess::feed(daemon, 0, wire);
            check(MessageGateAccess::sessionColumns(daemon, 88) == (isInteractiveUser && mayDrive ? 120 : originalColumns),
                "canonical Greet resizes only for an admitted human with Drive");
            check(MessageGateAccess::refusal(daemon, 0) == (isInteractiveUser ? std::optional<Sync::ByeReason> {} : Sync::ByeReason::NoConsent),
                "an unattended agent cannot attach without an exact standing grant");
            check(!MessageGateAccess::consentPending(daemon, 0),
                "an unattended agent creates no question waiting for a person to arrive");
            check(MessageGateAccess::takeCallCount(daemon, 0) == 0 && MessageGateAccess::sessionCount(daemon) == 1,
                "a coalesced destructive Call cannot escape its refused attachment");
        }
    }
}

void canonicalTargetsKeepCredentialScopeAndHumanStandingSeparate()
{
    for (const Principal& principal : principals) {
        for (const bool requiresToken : { true, false }) {
            for (const bool hasToken : { false, true }) {
                for (const bool hasScope : { false, true }) {
                    for (const auto target : { Sync::AdmissionTarget::Session, Sync::AdmissionTarget::Workspace,
                             Sync::AdmissionTarget::Control, Sync::AdmissionTarget::Management }) {
                        Daemon daemon;
                        MessageGateAccess::setTokenRequirement(daemon, requiresToken);
                        MessageGateAccess::addSession(daemon, 88);
                        MessageGateAccess::setSessionDirectory(daemon, 88, "/tmp");
                        const auto required = target == Sync::AdmissionTarget::Management ? Sync::Scope::Manage
                            : target == Sync::AdmissionTarget::Control                    ? Sync::Scope::Control
                                                                                          : Sync::Scope::Observe;
                        Sync::ScopeSet scopes { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control,
                            Sync::Scope::RegionRead, Sync::Scope::NewSession, Sync::Scope::FileRead, Sync::Scope::Manage };
                        if (!hasScope)
                            scopes.remove(required);
                        MessageGateAccess::addClient(daemon, principal.peerProcess, principal.agentLabel, hasToken, scopes);
                        MessageGateAccess::setInteractiveUser(daemon, 0, principal.isInteractiveUser);
                        MessageGateAccess::forgetHello(daemon, 0);
                        auto greeting = sessionGreeting(Sync::isUnboundTarget(target) ? 0 : 88);
                        greeting.request.target = target;
                        greeting.request.isAttachOnly = !Sync::isUnboundTarget(target);
                        greeting.request.agentLabel = principal.agentLabel;
                        MessageGateAccess::feed(daemon, 0, wireFrame(Sync::FrameKind::Greet, 0, greeting));
                        const bool isLocal = principal.expectedOrigin == CapabilityOrigin::Local;
                        const bool isHuman = isLocal || (hasToken && principal.isInteractiveUser);
                        // A missing credential is judged before scope. With the requirement
                        // off, a tokenless verified-local agent holds the scopes an `all`
                        // token carries, never Manage; a tokenless Remote peer is refused
                        // whatever the setting.
                        const bool isTokenlessLocalAgent = !hasToken && !requiresToken && principal.expectedOrigin == CapabilityOrigin::Agent;
                        const bool hasRequiredScope = isLocal || (hasToken ? hasScope : isTokenlessLocalAgent && required != Sync::Scope::Manage);
                        std::optional<Sync::ByeReason> expected;
                        if (!isLocal && !hasToken && !isTokenlessLocalAgent)
                            expected = Sync::ByeReason::TokenMissing;
                        else if (!hasRequiredScope)
                            expected = Sync::ByeReason::OutsideScope;
                        else if (target == Sync::AdmissionTarget::Workspace && !isHuman)
                            expected = Sync::ByeReason::OutsideScope;
                        else if (target == Sync::AdmissionTarget::Session && !isHuman)
                            expected = Sync::ByeReason::NoConsent;
                        check(MessageGateAccess::refusal(daemon, 0) == expected,
                            std::string("canonical target / ") + principal.name + (requiresToken ? " / token required" : " / token not required")
                                + (hasToken ? " / token" : " / no token") + (hasScope ? " / scope" : " / no scope")
                                + ": a credential, then scope, then human standing are independently required");
                        check(MessageGateAccess::sessionCount(daemon) == 1 && MessageGateAccess::takeCallCount(daemon, 0) == 0
                                && !MessageGateAccess::consentPending(daemon, 0),
                            "a target matrix never creates a new session, queues a hidden call or unattended consent question");
                    }
                }
            }
        }
    }
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    canonicalIngressDoesNotInventAuthority();
    aPartOfAnAdoptIsHeldOnlyOnTheLocalAdoptPath();
    canonicalGreetingKeepsGeometryAndCallbacksWithTheirOwner();
    canonicalTargetsKeepCredentialScopeAndHumanStandingSeparate();

    // Every method a Sync connection may call, against every principal,
    // with and without a token and its scope: admitted exactly when the
    // caller holds a role the entry names, stands where it requires and
    // carries its scope. The principal is the daemon's reading of the client;
    // the rule is the registry's.
    for (const Ubiquitty::Protocol::Control::MethodDescriptor& entry : Ubiquitty::Protocol::Control::methodRegistry()) {
        if (entry.syncMethod.empty())
            continue;
        const Ubiquitty::Codec::JSON::Value example = Ubiquitty::Protocol::Control::parseRegistryJSON(entry.example);
        const Ubiquitty::Codec::JSON::Value* parameters = example.find("params");
        check(parameters != nullptr, std::string(entry.name) + ": the entry's example has parameters to call it with");
        if (!parameters)
            continue;
        for (const Principal& principal : principals) {
            for (bool hasToken : { false, true }) { // bool-ok: exhaustive token-presence bit axis
                for (bool hasScope : { false, true }) { // bool-ok: exhaustive required-scope bit axis
                    Daemon daemon;
                    MessageGateAccess::addClient(daemon, principal.peerProcess, principal.agentLabel, hasToken, hasScope ? Sync::ScopeSet { entry.scope } : Sync::ScopeSet {});
                    MessageGateAccess::setInteractiveUser(daemon, 0, principal.isInteractiveUser);
                    using Ubiquitty::Protocol::Sync::Role;
                    const bool isLocal = principal.expectedOrigin == CapabilityOrigin::Local;
                    const bool isAgent = !isLocal && !(hasToken && principal.isInteractiveUser);
                    const bool mayDrive = hasToken && hasScope && entry.scope == Sync::Scope::Drive;
                    const Role role = isLocal ? Role::Owner : isAgent ? Role::Agent
                        : mayDrive                                    ? Role::Driver
                                                                      : Role::Viewer;
                    const Ubiquitty::Protocol::Control::CallerAuthority caller = MessageGateAccess::callerAuthority(daemon, 0);
                    check(caller.role == role, std::string(entry.name) + " / " + principal.name + ": the daemon reads the caller's role as the principal's");
                    check(caller.origin == principal.expectedOrigin, std::string(entry.name) + " / " + principal.name + ": the daemon says where the caller stands as it verified it");
                    for (const Sync::Scope scope : { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::RegionRead, Sync::Scope::NewSession, Sync::Scope::FileRead, Sync::Scope::Manage })
                        check(caller.scopes.contains(scope) == (isLocal || (hasToken && hasScope && scope == entry.scope)), std::string(entry.name) + " / " + principal.name + ": the caller carries exactly its credential's scopes");
                    const bool isRefusalExpected = !entry.roles.contains(role) || (entry.origin == Ubiquitty::Protocol::Control::CallOrigin::Local && !isLocal) || (!isLocal && !(hasToken && hasScope));
                    const std::optional<Ubiquitty::Protocol::Control::CallRefusal> refusal = Ubiquitty::Protocol::Control::admitCall(&entry, *parameters, caller);
                    check(refusal.has_value() == isRefusalExpected,
                        std::string(entry.name) + " / " + principal.name + (hasToken ? " / token" : " / no token") + (hasScope ? " / scope" : " / no scope") + ": admission matches role, origin and scope");
                    check(!refusal || (refusal->code == Ubiquitty::Protocol::Control::ErrorCode::Forbidden && !refusal->reason.empty()), std::string(entry.name) + " / " + principal.name + ": a refusal is forbidden, with its reason");
                }
            }
        }
    }

    // Each topology method has its own canonical operation boundary. Agents
    // enter exact-operation consent or the self-label standing check, not
    // direct mutation; structure_calls owns those execution assertions.
    for (const std::string_view name : { "label.set", "tags.set", "workspace.rename", "tab.rename", "tab.move", "window.close",
             "workspace.addTab", "workspace.split", "workspace.closeTab", "workspace.closePane", "pane.move", "pane.swap",
             "window.activate", "tab.activate", "pane.moveToTab", "tab.moveToPane", "pane.moveToNewTab", "pane.extractTab",
             "pane.extractWindow", "tab.extractWindow", "pane.focus", "window.moveFocus", "pane.zoom", "client.label",
             "tab.balance", "pane.adjustRatio", "divider.setRatio", "workspace.addWindow" }) {
        const auto* entry = Ubiquitty::Protocol::Control::findSyncMethod(name);
        check(entry && entry->scope == Sync::Scope::Drive
                && entry->roles == Ubiquitty::Protocol::Sync::Roles { Ubiquitty::Protocol::Sync::Role::Owner, Ubiquitty::Protocol::Sync::Role::Driver, Ubiquitty::Protocol::Sync::Role::Agent },
            std::string(name) + ": canonical topology admission retains Drive and its exact roles");
    }
    {
        for (const std::string_view method : { "host.describe", "host.methods", "host.sessions.subscribe", "host.sessions.unsubscribe" }) {
            const auto* entry = Ubiquitty::Protocol::Control::findSyncMethod(method);
            check(entry && entry->scope == Sync::Scope::Control && entry->roles.contains(Ubiquitty::Protocol::Sync::Role::Agent),
                std::string(method) + " requires Control and admits an authorized agent");
        }
        for (const std::string_view method : { "consent.respond", "consent.revoke" }) {
            const auto* entry = Ubiquitty::Protocol::Control::findSyncMethod(method);
            check(entry && entry->scope == Sync::Scope::Observe
                    && entry->roles == Ubiquitty::Protocol::Sync::Roles { Ubiquitty::Protocol::Sync::Role::Owner, Ubiquitty::Protocol::Sync::Role::Driver, Ubiquitty::Protocol::Sync::Role::Viewer },
                std::string(method) + ": human Observe admission preserves generic consent; the exact Structure dispatcher additionally requires Drive");
        }
    }

    // The listing is least privilege. `host.sessions.subscribe` asks Control
    // and nothing more, and what it lists of a pane depends on what else the
    // caller's credential carries: a pane's command line is what its screen
    // is, so it goes only to a caller that could read the screen (Observe).
    // Control alone lists program names without arguments; Control with
    // Observe lists them whole; Observe without Control is refused the
    // listing; a local client, which needs no token, is sent everything.
    {
        using Ubiquitty::Protocol::Sync::ListedDetail;
        const Ubiquitty::Protocol::Control::MethodDescriptor* subscribe = Ubiquitty::Protocol::Control::findSyncMethod("host.sessions.subscribe");
        check(subscribe != nullptr && subscribe->scope == Sync::Scope::Control, "host.sessions.subscribe is in the registry under the Control scope");
        const Ubiquitty::Codec::JSON::Value noParameters = Ubiquitty::Codec::JSON::Value::object();
        struct Credential {
            const char* name;
            Sync::ScopeSet scope;
            bool isAdmitted; // bool-ok: the row's expected verdict
            ListedDetail detail;
        };
        const Credential credentials[] = {
            { "Control alone", Sync::ScopeSet { Sync::Scope::Control }, true, ListedDetail::WithoutArguments },
            { "Control and Observe", Sync::ScopeSet { Sync::Scope::Control, Sync::Scope::Observe }, true, ListedDetail::Whole },
            { "Control and Drive", Sync::ScopeSet { Sync::Scope::Control, Sync::Scope::Drive }, true, ListedDetail::WithoutArguments },
            { "Observe alone", Sync::ScopeSet { Sync::Scope::Observe }, false, ListedDetail::Whole },
            { "Observe and Drive", Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Drive }, false, ListedDetail::Whole },
            { "no scope", Sync::ScopeSet {}, false, ListedDetail::WithoutArguments },
        };
        for (const Principal& principal : principals) {
            const bool isLocal = principal.expectedOrigin == CapabilityOrigin::Local;
            for (const Credential& credential : credentials) {
                Daemon daemon;
                MessageGateAccess::addClient(daemon, principal.peerProcess, principal.agentLabel, true, credential.scope);
                MessageGateAccess::setInteractiveUser(daemon, 0, principal.isInteractiveUser);
                const std::string prefix = std::string("listing / ") + principal.name + " / " + credential.name + ": ";
                const Ubiquitty::Protocol::Control::CallerAuthority caller = MessageGateAccess::callerAuthority(daemon, 0);
                const bool isAdmitted = subscribe && !Ubiquitty::Protocol::Control::admitCall(subscribe, noParameters, caller).has_value();
                check(isAdmitted == (isLocal || credential.isAdmitted), prefix + "the listing is admitted exactly under Control");
                check(MessageGateAccess::listedDetail(daemon, 0) == (isLocal ? ListedDetail::Whole : credential.detail), prefix + "a command line is listed exactly to a caller that could read the screen");
            }
            // A token is what carries a scope: without one a caller that is
            // not local is sent no command line whatever it claims.
            Daemon daemon;
            MessageGateAccess::addClient(daemon, principal.peerProcess, principal.agentLabel, false, Sync::ScopeSet { Sync::Scope::Control, Sync::Scope::Observe });
            check(MessageGateAccess::listedDetail(daemon, 0) == (isLocal ? ListedDetail::Whole : ListedDetail::WithoutArguments), std::string("listing / ") + principal.name + " / no token: a scope with no token behind it reads no command line");
        }
    }

    // Origin follows the verified binary, never a missing self-claimed label.
    for (const Principal& principal : principals) {
        Daemon daemon;
        MessageGateAccess::addClient(daemon, principal.peerProcess, principal.agentLabel, false,
            Sync::ScopeSet {});
        check(MessageGateAccess::originOf(daemon, 0) == principal.expectedOrigin,
            std::string("origin of ") + principal.name + " is what the kernel verified");
    }

    // Typed file.read confines paths to the session's working directory and
    // retains its independent scope, file-type, and per-client request bounds.
    {
        char directoryTemplate[] = "/tmp/ubiquitty-filereq-XXXXXX";
        const char* directory = ::mkdtemp(directoryTemplate);
        check(directory != nullptr, "a confinement root was created");
        if (directory) {
            const std::string root(directory);
            const std::string regular = root + "/readable.txt";
            {
                std::FILE* f = std::fopen(regular.c_str(), "wb");
                if (f) {
                    std::fputs("hello", f);
                    std::fclose(f);
                }
            }
            const std::string subdirectory = root + "/adirectory";
            ::mkdir(subdirectory.c_str(), 0700);
            const std::string fifo = root + "/afifo";
            ::mkfifo(fifo.c_str(), 0600);

            const auto ask = [&](const std::string& path, std::uint32_t priorRequests) {
                Daemon daemon;
                MessageGateAccess::addSession(daemon, 88);
                MessageGateAccess::setSessionDirectory(daemon, 88, root);
                MessageGateAccess::addClient(daemon, trustedLocalPeer(), "", true,
                    Sync::ScopeSet { Sync::Scope::FileRead });
                MessageGateAccess::attachClientToSession(daemon, 0, 88);
                MessageGateAccess::setFileRequestCount(daemon, 0, priorRequests);
                MessageGateAccess::clearOutput(daemon, 0);
                return MessageGateAccess::readFile(daemon, 0, path);
            };

            check(ask(regular, 0) == static_cast<int>(Sync::FileDataStatus::OK),
                "a regular file inside the session's directory is served");
            check(ask(subdirectory, 0) == static_cast<int>(Sync::FileDataStatus::NotRegular),
                "a directory is refused as not-regular rather than read");
            check(ask(fifo, 0) == static_cast<int>(Sync::FileDataStatus::NotRegular),
                "a FIFO is refused as not-regular — opening it to read would wedge the thread");
            check(ask(root + "/no-such-file", 0) == static_cast<int>(Sync::FileDataStatus::NotFound),
                "a path that is not there is not-found");

            // Escapes: the resolved path must stay inside the session's own
            // subtree, whichever way the client spells its way out.
            for (const std::string& escape : { std::string("/etc/passwd"),
                     root + "/../../../etc/passwd", root + "/./../..", std::string("../../etc/passwd") }) {
                const int status = ask(escape, 0);
                check(status != static_cast<int>(Sync::FileDataStatus::OK),
                    "a path resolving outside the session's directory is never served");
            }

            // The per-client ceiling: past it every request is denied, however
            // ordinary the path. A client that could keep asking would turn a
            // file-read grant into an unmetered filesystem scan.
            check(ask(regular, 256) == static_cast<int>(Sync::FileDataStatus::Denied),
                "past the per-client request ceiling the read is denied");
            check(ask(regular, 255) == static_cast<int>(Sync::FileDataStatus::OK),
                "one below the ceiling still serves — the cap is not off by one");

            // The typed route retains FileRead's independent scope and local
            // principal policy; no Drive/Control role shortcut grants it.
            for (const auto& principal : principals) {
                for (const bool hasToken : { false, true }) {
                    for (const bool hasFileScope : { false, true }) {
                        Daemon daemon;
                        MessageGateAccess::addSession(daemon, 88);
                        MessageGateAccess::setSessionDirectory(daemon, 88, root);
                        MessageGateAccess::addClient(daemon, principal.peerProcess, principal.agentLabel, hasToken,
                            hasFileScope ? Sync::ScopeSet { Sync::Scope::FileRead } : Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
                        MessageGateAccess::setInteractiveUser(daemon, 0, principal.isInteractiveUser);
                        MessageGateAccess::attachClientToSession(daemon, 0, 88);
                        const bool isAllowed = principal.expectedOrigin == CapabilityOrigin::Local || (hasToken && hasFileScope);
                        check((MessageGateAccess::readFile(daemon, 0, regular) == static_cast<int>(Sync::FileDataStatus::OK)) == isAllowed,
                            std::string("typed file.read scope: ") + principal.name);
                    }
                }
            }

            ::unlink(regular.c_str());
            ::unlink(fifo.c_str());
            ::rmdir(subdirectory.c_str());
            ::rmdir(root.c_str());
        }

        // A file request from a client attached to no session at all: there is
        // no directory to confine against, so it cannot be served.
        {
            Daemon daemon;
            MessageGateAccess::addClient(daemon, trustedLocalPeer(), "", true,
                Sync::ScopeSet { Sync::Scope::FileRead });
            MessageGateAccess::clearOutput(daemon, 0);
            check(MessageGateAccess::readFile(daemon, 0, "/etc/hosts") != static_cast<int>(Sync::FileDataStatus::OK),
                "a sessionless client is served nothing");
        }
    }

    if (!failures) {
        std::printf("message_gate_matrix: %zu canonical frame policies, %zu verified principals, method/token/scope, local handoff, confinement and geometry matrices\n",
            std::size(Sync::frameGates), std::size(principals));
        return 0;
    }
    std::fprintf(stderr, "message_gate_matrix: %d failure(s)\n", failures);
    return 1;
}
