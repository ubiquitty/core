// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Tests/Host/HostConnection.h — shared AF_UNIX daemon-connection harness for the
// Host integration tests (cf. quic_test_mint.h for the QUIC-flavored precedent).
//
// Remote bytes use the canonical envelope and production payload codecs.
// Descriptor-bearing adoption uses its separately selected local operation;
// no remote frame, numeric tag translation, or Catalog adapter carries an FD.
//

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/FrameIngress.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Session/HostListing.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::HostTest {

// ---- little-endian byte helpers ---------------------------------------------

inline void putU16(std::string& b, std::uint16_t v)
{
    b.push_back(static_cast<char>(v & 0xFF));
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
}

inline void putU32(std::string& b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
}

inline std::uint16_t getU16(const std::string& b, std::size_t off)
{
    if (off + 2 > b.size())
        return 0;
    return static_cast<std::uint8_t>(b[off]) | (static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[off + 1])) << 8);
}

inline std::uint32_t getU32(const std::string& b, std::size_t off)
{
    if (off + 4 > b.size())
        return 0;
    return static_cast<std::uint8_t>(b[off]) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off + 1])) << 8) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off + 2])) << 16) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off + 3])) << 24);
}

// ---- the connection + frame queue -------------------------------------------

/// An owned canonical frame; its channel remains part of its identity.
struct ReceivedFrame {
    Protocol::Sync::FrameKind kind;
    std::uint32_t channel = 0;
    std::string body;
};

/// The exact stream-order admission boundary, independent of queued queries.
enum class FixtureAdmission : std::uint8_t {
    Greeting,
    AwaitingPending,
    Pending,
    Granted,
    Denied,
    Expired,
    Closed,
};

struct Connection {
    int fileDescriptor = -1;
    std::string rx;
    std::size_t off = 0;
    std::vector<ReceivedFrame> frames;
    Protocol::Sync::FrameIngress ingress { Protocol::Sync::maxFrameBodyBytes };
    std::optional<Protocol::Sync::Greet> greeting;
    std::optional<Protocol::Sync::Welcome> welcome;
    std::vector<Protocol::Sync::PaneBind> panes;
    std::vector<Protocol::Sync::PaneBind> listedPanes;
    FixtureAdmission admission = FixtureAdmission::Greeting;
    bool hasFailed = false;
    /// The handle the host bound the connection's first pane to, 0 before it
    /// has.
    std::uint32_t paneHandle = 0;
    /// The number of the next input event the client sends that pane.
    std::uint64_t nextInputSeq = 1;
};

/// Writes a client's bytes to the daemon, whose end may already be closed.
///
/// The daemon closes a connection it has denied, revoked or superseded, and a
/// client that answers a frame it read late writes after that close. That
/// write is an error to drop; raised as SIGPIPE it ends the test with no
/// message. Every descriptor a `Connection` holds is a socket.
///
/// - Parameters:
///   - fileDescriptor: The client's socket.
///   - bytes: What to write.
/// - Returns: Whether every byte was written without treating closure as success.
inline bool writeToDaemon(int fileDescriptor, const std::string& bytes)
{
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    (void)::setsockopt(fileDescriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
    const int flags = 0;
#else
    const int flags = MSG_NOSIGNAL;
#endif
    for (std::size_t sent = 0; sent < bytes.size();) {
        const auto count = ::send(fileDescriptor, bytes.data() + sent, bytes.size() - sent, flags);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

// Connect to the daemon's AF_UNIX socket at `path`. Returns the fd, or -1.
inline int connectDaemon(const std::string& path)
{
    int fileDescriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fileDescriptor < 0)
        return -1;
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fileDescriptor, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fileDescriptor);
        return -1;
    }
    return fileDescriptor;
}

/// Connects to the daemon through its own `--attach` bridge, as a person's
/// client reaches it over SSH: the daemon sees its own binary at the other
/// end and verifies the client as a person, which a test binary on the
/// socket it is not.
///
/// - Parameters:
///   - hostBinary: The `ubiquitty-host` the daemon runs.
///   - home: The `UBIQUITTY_HOME` the daemon runs under.
///   - bridge: The bridge's process id, for the caller to end.
/// - Returns: The test's end of the bridge, or -1.
inline int connectThroughAttachBridge(const std::string& hostBinary, const std::string& home, pid_t& bridge)
{
    int ends[2] = { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0)
        return -1;
    bridge = ::fork();
    if (bridge < 0) {
        ::close(ends[0]);
        ::close(ends[1]);
        return -1;
    }
    if (bridge == 0) {
        ::dup2(ends[1], STDIN_FILENO);
        ::dup2(ends[1], STDOUT_FILENO);
        ::close(ends[0]);
        ::close(ends[1]);
        ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
        // A local attach: a bridge that inherited an SSH session's variables
        // would mark its client as relayed from SSH.
        ::unsetenv("SSH_CONNECTION");
        ::unsetenv("SSH_CLIENT");
        char* arguments[] = { const_cast<char*>(hostBinary.c_str()), const_cast<char*>("--attach"), nullptr };
        ::execv(hostBinary.c_str(), arguments);
        _exit(127);
    }
    ::close(ends[1]);
    return ends[0];
}

/// Waits until the daemon accepts a connection, and reports whether it did.
///
/// Readiness is measured by connecting, not by the socket file existing. bind()
/// creates the file and listen() is what makes it answer, so a test that stats
/// the path proceeds during the window between them — and the failure surfaces
/// later, out of a spawned `--attach` reporting no daemon to attach to, which
/// names neither this wait nor the test that skipped it.
///
/// The bound is wall-clock rather than a number of iterations. A loop counted in
/// passes gets shorter as the machine gets busier, which is the opposite of what
/// a timeout is for, and it reads as a fixed number of seconds while being one
/// only on an idle machine.
///
/// - Parameters:
///   - path: The daemon's AF_UNIX socket path.
///   - seconds: How long to keep trying before giving up.
/// - Returns: `true` once a connection succeeded, `false` at the deadline.
inline bool waitForDaemonReady(const std::string& path, int seconds = 30)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    for (;;) {
        const int fileDescriptor = connectDaemon(path);
        if (fileDescriptor >= 0) {
            ::close(fileDescriptor);
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        ::usleep(10 * 1000);
    }
}

// A local-bridge connection: `fileDescriptor` is the test's end of the
// socketpair; `bridgeProcessID` is the spawned `--attach` process to reap on
// teardown (kill + waitpid), alongside the daemon.
struct LocalBridgeConnection {
    int fileDescriptor = -1;
    pid_t bridgeProcessID = -1;
};

// Spawn `<hostBinaryPath> --attach` wired to a socketpair instead of
// connecting this process's own socket directly. `Daemon::originOf` trusts
// only a kernel-verified peer binary literally named "ubiquitty-host" as
// Local; a raw test binary calling connectDaemon() is classified
// Agent like any other unlabeled local tool, which fails any origin-gated
// check (isAgentClient — ADOPT_SESSION, the consent gate's "is a human
// attached" test) no token can satisfy (those aren't scope checks). This is
// the same relay the real macOS app's local attach uses ("ubiquitty-host
// --ensure-daemon && exec ubiquitty-host --attach") — spawning it makes the
// actual daemon connection genuinely come from the bundled binary, so
// origin resolves to Local exactly like production. `--attach` derives the
// same content-hashed socket leaf as the daemon automatically (both
// processes exec the identical `hostBinaryPath`), so no --socket flag is
// needed. `macHome` must match the daemon's UBIQUITTY_HOME.
inline LocalBridgeConnection connectDaemonAsLocalBridge(const std::string& hostBinaryPath, const std::string& macHome)
{
    int socketPair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, socketPair) != 0)
        return {};
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(socketPair[0]);
        ::close(socketPair[1]);
        return {};
    }
    if (pid == 0) {
        ::close(socketPair[0]);
        ::dup2(socketPair[1], 0); // stdin
        ::dup2(socketPair[1], 1); // stdout
        ::close(socketPair[1]);
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::execl(hostBinaryPath.c_str(), hostBinaryPath.c_str(), "--attach", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(socketPair[1]);
    return { socketPair[0], pid };
}

/// Writes one canonical frame, preserving its actual channel and payload.
/// - Parameters:
///   - connection: The physical socket.
///   - kind: The canonical frame kind.
///   - body: Its encoded payload.
///   - channel: Zero for connection kinds, an actual pane handle otherwise.
/// - Returns: Whether encoding and the complete write succeeded.
inline bool sendFrame(Connection& connection, Protocol::Sync::FrameKind kind, const std::string& body, std::uint32_t channel = 0)
{
    std::vector<std::uint8_t> frame;
    if (!Protocol::Sync::appendFrame(frame, kind, channel, { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() }))
        return false;
    return writeToDaemon(connection.fileDescriptor, { reinterpret_cast<const char*>(frame.data()), frame.size() });
}

/// Copies the exact locally selected greeting for a reader replaying this socket.
/// - Parameter greeting: The same request already sent on the physical carrier.
/// - Returns: A production reader identity, never inferred from a Welcome.
inline Protocol::Sync::ClientIdentity clientIdentity(const Protocol::Sync::Greet& greeting)
{
    Protocol::Sync::ClientIdentity identity;
    identity.clientInstance = greeting.clientInstance;
    identity.kind = greeting.clientKind;
    identity.requestedRole = greeting.requestedRole;
    identity.engineFingerprint = greeting.engineFingerprint;
    identity.facts = greeting.facts;
    identity.resume = greeting.resume;
    identity.request = greeting.request;
    identity.credential = greeting.credential;
    identity.appVersion = greeting.appVersion;
    return identity;
}

/// Encodes one schema-valid canonical greeting, with no outer handshake.
/// - Parameter greeting: The complete caller-selected identity and request.
/// - Returns: Canonical bytes, or an empty string when the schema refuses it.
inline std::string greetFrame(const Protocol::Sync::Greet& greeting)
{
    std::vector<std::uint8_t> body;
    Protocol::Sync::encode(body, greeting);
    std::vector<std::uint8_t> frame;
    if (body.empty() || !Protocol::Sync::decodeGreet(body)
        || !Protocol::Sync::appendFrame(frame, Protocol::Sync::FrameKind::Greet, 0, body))
        return {};
    return { reinterpret_cast<const char*>(frame.data()), frame.size() };
}

/// Sends exactly the caller's target, identity, credentials, and geometry once.
/// - Parameters:
///   - connection: The not-yet-greeted physical socket.
///   - greeting: The explicit fixture-owned target and identity.
/// - Returns: Whether the one complete greeting was sent and retained.
inline bool sendGreeting(Connection& connection, Protocol::Sync::Greet greeting)
{
    if (connection.greeting || connection.hasFailed)
        return false;
    const auto frame = greetFrame(greeting);
    if (frame.empty() || !writeToDaemon(connection.fileDescriptor, frame))
        return false;
    connection.greeting = std::move(greeting);
    return true;
}

/// Reads a varint of a Sync envelope.
///
/// - Parameters:
///   - b: The bytes.
///   - off: Where it starts; moved past it.
/// - Returns: The value, or `nullopt` when it runs off the end.
inline std::optional<std::uint64_t> getVarint(const std::string& b, std::size_t& off)
{
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64 && off < b.size(); shift += 7) {
        const std::uint8_t byte = static_cast<std::uint8_t>(b[off++]);
        value |= std::uint64_t { byte & 0x7Fu } << shift;
        if (!(byte & 0x80))
            return value;
    }
    return std::nullopt;
}

/// A canonical View setting an actually bound pane live.
inline std::string viewLiveFrame(std::uint32_t handle)
{
    std::vector<std::uint8_t> body;
    Protocol::Sync::encode(body, Protocol::Sync::View { .state = Protocol::Sync::ViewState::Live });
    std::vector<std::uint8_t> frame;
    if (!handle || !Protocol::Sync::appendFrame(frame, Protocol::Sync::FrameKind::View, handle, body))
        return {};
    return { reinterpret_cast<const char*>(frame.data()), frame.size() };
}

/// The View frames owed the panes bound in complete canonical envelopes read
/// since the last call, for a client that keeps its own read buffer.
///
/// - Parameters:
///   - received: Everything read, in order.
///   - scanned: How far earlier calls read; moved past the whole frames.
/// - Returns: The frames to send, back to back.
inline std::string viewsForBoundPanes(const std::string& received, std::size_t& scanned)
{
    using namespace Protocol::Sync;
    std::string views;
    while (scanned < received.size()) {
        const std::span<const std::uint8_t> unread { reinterpret_cast<const std::uint8_t*>(received.data() + scanned), received.size() - scanned };
        FrameHeader header;
        EnvelopeError error = EnvelopeError::None;
        std::size_t prefix = 1;
        for (; prefix <= std::min(unread.size(), maxEnvelopeHeaderBytes); ++prefix) {
            const auto state = readFrameHeader(unread.first(prefix), header, error);
            if (state == FrameHeaderState::Failed)
                return {};
            if (state == FrameHeaderState::Complete)
                break;
        }
        if (prefix > std::min(unread.size(), maxEnvelopeHeaderBytes) || header.bodyBytes > unread.size() - prefix)
            break;
        if (header.kind == FrameKind::PaneBind) {
            const auto bind = decodePaneBind(unread.subspan(prefix, header.bodyBytes));
            if (!bind)
                return {};
            views += viewLiveFrame(bind->handle);
        }
        scanned += prefix + header.bodyBytes;
    }
    return views;
}

/// Retains direct canonical frames and their channels; malformed input is sticky.
/// The complete received transcript remains available to independent typed readers.
inline void parseFrames(Connection& c)
{
    using namespace Protocol::Sync;
    if (c.hasFailed || c.off > c.rx.size())
        return;
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(c.rx.data() + c.off), c.rx.size() - c.off);
    std::string owedViews;
    const auto result = c.ingress.consume(bytes, [&](std::span<const std::uint8_t> prefix) {
        FrameHeader header;
        EnvelopeError error = EnvelopeError::None;
        const auto state = readFrameHeader(prefix, header, error);
        if (state == FrameHeaderState::NeedsMore)
            return FrameHeaderDecision {};
        bool isAdmitted = state == FrameHeaderState::Complete && directionOf(header.kind) != Direction::ClientToHost;
        if (c.admission == FixtureAdmission::Greeting)
            isAdmitted &= header.kind == FrameKind::Welcome || header.kind == FrameKind::Bye;
        else
            isAdmitted &= header.kind != FrameKind::Welcome;
        if (c.admission == FixtureAdmission::AwaitingPending || c.admission == FixtureAdmission::Pending)
            isAdmitted &= header.kind == FrameKind::Bye || (header.kind == FrameKind::Probe && header.bodyBytes == 8)
                || (header.kind == FrameKind::Echo && header.bodyBytes == 12)
                || (header.kind == FrameKind::Notify && header.bodyBytes <= maxConsentStatusBytes);
        if (c.admission == FixtureAdmission::Denied || c.admission == FixtureAdmission::Expired)
            isAdmitted &= header.kind == FrameKind::Bye;
        isAdmitted &= c.admission != FixtureAdmission::Closed;
        return FrameHeaderDecision { .state = isAdmitted ? FrameHeaderDecision::State::Admitted : FrameHeaderDecision::State::Refused,
            .bodyBytes = header.bodyBytes }; }, [&](std::span<const std::uint8_t> prefix, std::span<const std::uint8_t> body) {
        FrameHeader header;
        EnvelopeError error = EnvelopeError::None;
        if (readFrameHeader(prefix, header, error) != FrameHeaderState::Complete)
            c.hasFailed = true;
        else if (header.kind == FrameKind::Welcome) {
            auto welcome = decodeWelcome(body);
            // A saved reference to an earlier instance is answered by the one
            // that holds its workspace now, naming the instance asked for; a
            // resume is answered by the successor, naming its predecessor.
            const bool isSuccessorAnswer = welcome && c.greeting
                && (isRestoredReferenceAnswer(c.greeting->request, welcome->selection) || (c.greeting->resume && welcome->predecessorHostInstanceID));
            if (!welcome || c.welcome || !c.greeting || welcome->selection.target != c.greeting->request.target
                || (c.greeting->request.sessionID && !isSuccessorAnswer && welcome->selection.sessionID != c.greeting->request.sessionID)
                || (c.greeting->request.workspaceID && welcome->selection.workspaceID != c.greeting->request.workspaceID)
                || (c.greeting->request.expectedDaemonInstanceID && !isSuccessorAnswer
                    && welcome->selection.daemonInstanceID != c.greeting->request.expectedDaemonInstanceID))
                c.hasFailed = true;
            else {
                c.admission = welcome->admission == AttachmentAdmission::Granted ? FixtureAdmission::Granted : FixtureAdmission::AwaitingPending;
                c.welcome = std::move(welcome);
            }
        } else if (!c.welcome && header.kind != FrameKind::Bye)
            c.hasFailed = true;
        else if (header.kind == FrameKind::Bye) {
            const auto bye = decodeBye(body);
            if (!bye || ((c.admission == FixtureAdmission::Denied || c.admission == FixtureAdmission::Expired) && bye->reason != ByeReason::NoConsent))
                c.hasFailed = true;
            else
                c.admission = FixtureAdmission::Closed;
        } else if (header.kind == FrameKind::Probe)
            c.hasFailed = !decodeProbe(body);
        else if (header.kind == FrameKind::Echo)
            c.hasFailed = !decodeEcho(body);
        else if (header.kind == FrameKind::Notify && (c.admission == FixtureAdmission::AwaitingPending || c.admission == FixtureAdmission::Pending)) {
            const auto notify = decodeNotifyHead(body);
            const auto status = notify && notify->kind == std::to_underlying(NotifyKind::ConsentStatus) ? decodeConsentStatus(notify->record) : std::nullopt;
            if (!status || status->question != c.welcome->consent)
                c.hasFailed = true;
            else if (c.admission == FixtureAdmission::AwaitingPending) {
                c.hasFailed = status->phase != ConsentStatusPhase::Pending;
                if (!c.hasFailed)
                    c.admission = FixtureAdmission::Pending;
            } else {
                switch (status->phase) {
                case ConsentStatusPhase::Pending:
                    c.hasFailed = true;
                    break;
                case ConsentStatusPhase::Approved:
                    c.admission = FixtureAdmission::Granted;
                    break;
                case ConsentStatusPhase::Denied:
                    c.admission = FixtureAdmission::Denied;
                    break;
                case ConsentStatusPhase::Expired:
                    c.admission = FixtureAdmission::Expired;
                    break;
                }
            }
        } else if (header.kind == FrameKind::Notify) {
            const auto notify = decodeNotifyHead(body);
            c.hasFailed = !notify || notify->kind == std::to_underlying(NotifyKind::ConsentStatus);
        } else if (header.kind == FrameKind::PaneBind) {
            const auto bind = decodePaneBind(body);
            if (!bind || isUnboundTarget(c.welcome->selection.target) || bind->handle >= firstListedHandle || c.panes.size() >= maxPanesPerConnection
                || std::ranges::any_of(c.panes, [&](const auto& pane) { return pane.handle == bind->handle || pane.paneID == bind->paneID; }))
                c.hasFailed = true;
            else {
                c.panes.push_back(*bind);
                if (!c.paneHandle)
                    c.paneHandle = bind->handle;
                owedViews += viewLiveFrame(bind->handle);
            }
        } else if (header.kind == FrameKind::PaneListed) {
            const auto listed = decodePaneBind(body);
            if (!listed || listed->handle < firstListedHandle || c.listedPanes.size() >= maxPanesPerConnection
                || std::ranges::any_of(c.listedPanes, [&](const auto& pane) { return pane.handle == listed->handle; }))
                c.hasFailed = true;
            else
                c.listedPanes.push_back(*listed);
        } else if (header.kind == FrameKind::PaneUnbind) {
            const auto unbind = decodePaneUnbind(body);
            if (!unbind || (std::ranges::none_of(c.panes, [&](const auto& pane) { return pane.handle == unbind->handle; })
                               && std::ranges::none_of(c.listedPanes, [&](const auto& pane) { return pane.handle == unbind->handle; })))
                c.hasFailed = true;
            else {
                std::erase_if(c.panes, [&](const auto& pane) { return pane.handle == unbind->handle; });
                std::erase_if(c.listedPanes, [&](const auto& pane) { return pane.handle == unbind->handle; });
                if (c.paneHandle == unbind->handle)
                    c.paneHandle = c.panes.empty() ? 0 : c.panes.front().handle;
            }
        }
        if (c.hasFailed)
            return FrameDelivery::Stop;
        c.frames.push_back({ .kind = header.kind, .channel = header.channel,
            .body = std::string(body.begin(), body.end()) });
        return FrameDelivery::Continue; });
    c.off += result.consumedBytes;
    c.hasFailed |= result.state != FrameIngress::State::Accepting;
    // No native write while the ingress lends a frame body to a callback.
    if (!c.hasFailed && !owedViews.empty())
        c.hasFailed = !writeToDaemon(c.fileDescriptor, owedViews);
}

/// Waits for a frame of `type` and takes it out of the queue.
///
/// The bound is wall clock. Counting poll calls instead spends the budget early
/// on a connection carrying other frames, because poll returns as soon as any
/// byte arrives rather than after its full timeout.
///
/// - Parameters:
///   - c: The connection to read from; its frame queue is consumed.
///   - type: The frame type to wait for.
///   - body: Receives the frame body when one arrives.
///   - totalMilliseconds: How long to wait before giving up.
///   - channel: The exact connection or pane channel to match.
/// - Returns: `true` when the frame arrived, `false` on timeout or on the peer
///   closing without sending it.
inline bool waitFor(Connection& c, Protocol::Sync::FrameKind type, std::string& body, int totalMilliseconds, std::uint32_t channel = 0)
{
    auto take = [&]() -> bool {
        for (std::size_t i = 0; i < c.frames.size(); ++i)
            if (c.frames[i].kind == type && c.frames[i].channel == channel) {
                body = c.frames[i].body;
                c.frames.erase(c.frames.begin() + static_cast<long>(i));
                return true;
            }
        return false;
    };
    if (take())
        return true;
    // A deadline, not a tally of poll calls. `waited += 50` assumes each poll
    // costs its full timeout, and poll returns EARLY whenever a byte arrives —
    // so on a connection carrying other frames the budget is spent in a
    // fraction of totalMilliseconds, and the parameter's name says the opposite of what
    // it does at all 67 call sites.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(totalMilliseconds);
    while (!c.hasFailed && std::chrono::steady_clock::now() < deadline) {
        pollfd p { c.fileDescriptor, POLLIN, 0 };
        if (::poll(&p, 1, 50) > 0) {
            char buffer[8192];
            ssize_t n = ::read(c.fileDescriptor, buffer, sizeof(buffer));
            if (n > 0) {
                c.rx.append(buffer, static_cast<std::size_t>(n));
                parseFrames(c);
            } else if (n == 0) {
                return take(); // peer closed
            }
        }
        if (take())
            return true;
    }
    return false;
}

/// Reads a complete typed directory through a raw fixture connection. Its
/// retained transcript supplies the already performed greeting; only new
/// Calls and acknowledgements are written, never a second Greet.
/// - Parameters:
///   - connection: The fixture's established carrier and transcript.
///   - requestID: A unique id for this listing on the carrier.
///   - timeoutMilliseconds: The whole read deadline.
/// - Returns: Complete rows, or nothing after refusal, malformed data or timeout.
[[nodiscard]] inline std::optional<std::vector<Ubiquitty::Session::SessionRow>> readSessionDirectory(Connection& connection, std::uint32_t requestID, int timeoutMilliseconds = 3000)
{
    namespace Sync = Protocol::Sync;
    if (!connection.greeting || connection.hasFailed)
        return std::nullopt;
    Sync::ClientConnection client(Sync::ClientPolicy {}, clientIdentity(*connection.greeting));
    client.start();
    (void)client.takeStreamOutput();
    const std::string id = "fixture.list." + std::to_string(requestID);
    Session::HostListing listing(id);
    std::size_t fed = 0;
    std::uint64_t revision = 0;
    bool didSubscribe = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    const auto flush = [&] {
        client.tick(++revision);
        const auto stream = client.takeStreamOutput();
        if (!stream.empty())
            writeToDaemon(connection.fileDescriptor, { reinterpret_cast<const char*>(stream.data()), stream.size() });
    };
    while (std::chrono::steady_clock::now() < deadline) {
        client.receiveStream({ reinterpret_cast<const std::uint8_t*>(connection.rx.data() + fed), connection.rx.size() - fed }, ++revision);
        fed = connection.rx.size();
        if (client.state() == Sync::ConnectionState::Closed || connection.hasFailed)
            break;
        if (!didSubscribe && client.welcome()) {
            if (!client.call(Session::HostListing::subscribeRequest(id)))
                break;
            didSubscribe = true;
        }
        for (const auto& response : client.takeResults())
            (void)listing.noteAnswer(response, 0);
        if (listing.isRefused() || listing.refresh(client, ++revision, 0) == Session::HostListing::Change::Malformed)
            break;
        if (listing.isListing() && listing.isWhole()) {
            const auto rows = Session::sessionRows(listing.tree());
            if (!client.call(Session::HostListing::unsubscribeRequest(id + ".end")))
                break;
            flush();
            return rows;
        }
        flush();
        pollfd descriptor { connection.fileDescriptor, POLLIN, 0 };
        if (::poll(&descriptor, 1, 20) > 0) {
            char bytes[8192];
            const auto count = ::read(connection.fileDescriptor, bytes, sizeof(bytes));
            if (count <= 0)
                break;
            connection.rx.append(bytes, static_cast<std::size_t>(count));
            parseFrames(connection);
        }
    }
    if (didSubscribe) {
        (void)client.call(Session::HostListing::unsubscribeRequest(id + ".end"));
        flush();
    }
    return std::nullopt;
}

/// Reads the connection until the host has bound its pane, leaving every
/// frame read in the queue for a later `waitFor`.
///
/// - Parameters:
///   - c: The connection.
///   - totalMilliseconds: How long to wait before giving up.
/// - Returns: Whether the pane is bound.
inline bool waitForPane(Connection& c, int totalMilliseconds = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(totalMilliseconds);
    while (!c.hasFailed && c.paneHandle == 0 && std::chrono::steady_clock::now() < deadline) {
        pollfd p { c.fileDescriptor, POLLIN, 0 };
        if (::poll(&p, 1, 50) <= 0)
            continue;
        char buffer[8192];
        const ssize_t n = ::read(c.fileDescriptor, buffer, sizeof(buffer));
        if (n == 0)
            break; // peer closed
        if (n > 0) {
            c.rx.append(buffer, static_cast<std::size_t>(n));
            parseFrames(c);
        }
    }
    return !c.hasFailed && c.paneHandle != 0;
}

/// Encodes one canonical envelope with its actual connection or pane channel.
///
/// - Parameters:
///   - kind: The envelope's `FrameKind`.
///   - handle: Zero for connection kinds, the actual bound handle otherwise.
///   - body: The envelope's body, within the canonical frame cap.
/// - Returns: The envelope, or an empty string for invalid scope or size.
inline std::string paneEnvelopeFrame(Ubiquitty::Protocol::Sync::FrameKind kind, std::uint32_t handle, const std::string& body)
{
    std::vector<std::uint8_t> frame;
    if (!Protocol::Sync::appendFrame(frame, kind, handle, { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() }))
        return {};
    return { reinterpret_cast<const char*>(frame.data()), frame.size() };
}

/// Types bytes at the connection's pane, as a client's keystrokes and pastes
/// travel: `Raw` input events on the pane's channel (Docs/sync.md §7.6),
/// numbered in the order sent, once the host has bound the pane.
///
/// - Parameters:
///   - c: The connection, greeted.
///   - bytes: What to type.
/// - Returns: Whether the pane was bound and the events were written.
inline bool sendInput(Connection& c, const std::string& bytes)
{
    if (!waitForPane(c))
        return false;
    constexpr std::size_t maxRawEventBytes = 16 * 1024;
    for (std::size_t at = 0; at < bytes.size() || at == 0; at += maxRawEventBytes) {
        const std::string piece = bytes.substr(at, maxRawEventBytes);
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Raw { .inputSeq = c.nextInputSeq, .bytes = { reinterpret_cast<const std::uint8_t*>(piece.data()), piece.size() } });
        if (!sendFrame(c, Protocol::Sync::FrameKind::Raw, { reinterpret_cast<const char*>(body.data()), body.size() }, c.paneHandle))
            return false;
        ++c.nextInputSeq;
        if (bytes.empty())
            break;
    }
    return true;
}

/// The pixels a client draws a pane in, for its `Viewport`; all zero when it
/// has measured none.
struct ViewportPixels {
    std::uint16_t cellWidth = 0;
    std::uint16_t cellHeight = 0;
    std::uint16_t textAreaWidth = 0;
    std::uint16_t textAreaHeight = 0;
    std::uint16_t drawableWidth = 0;
    std::uint16_t drawableHeight = 0;
};

/// Tells the host the size the client shows its pane at: a `Viewport` on the
/// pane's channel (Docs/sync.md §7.6), once the host has bound the pane.
/// Only a client the host verifies as a person may send one.
///
/// - Parameters:
///   - c: The connection, greeted.
///   - columns: The grid's columns.
///   - rows: Its rows.
///   - pixels: The pixels the client draws it in.
/// - Returns: Whether the pane was bound and the frame was written.
inline bool sendViewport(Connection& c, std::uint16_t columns, std::uint16_t rows, ViewportPixels pixels = {})
{
    if (!waitForPane(c))
        return false;
    std::vector<std::uint8_t> body;
    Protocol::Sync::encode(body, Protocol::Sync::Viewport { .rows = rows, .columns = columns, .cellPixelWidth = pixels.cellWidth, .cellPixelHeight = pixels.cellHeight, .textAreaPixelWidth = pixels.textAreaWidth, .textAreaPixelHeight = pixels.textAreaHeight, .drawablePixelWidth = pixels.drawableWidth, .drawablePixelHeight = pixels.drawableHeight });
    return sendFrame(c, Protocol::Sync::FrameKind::Viewport, { reinterpret_cast<const char*>(body.data()), body.size() }, c.paneHandle);
}

/// Waits for a checkpoint head on the first actually bound pane's channel.
///
/// - Parameters:
///   - c: The connection to read from.
///   - totalMilliseconds: How long to wait before giving up.
/// - Returns: Whether a checkpoint began within the time.
inline bool waitForScreen(Connection& c, int totalMilliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(totalMilliseconds);
    if (!waitForPane(c, totalMilliseconds))
        return false;
    std::string body;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return waitFor(c, Protocol::Sync::FrameKind::CheckpointHead, body, static_cast<int>(std::max<std::int64_t>(left, 0)), c.paneHandle)
        && Protocol::Sync::decodeCheckpointHead({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() }).has_value();
}

} // namespace Ubiquitty::HostTest
