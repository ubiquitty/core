// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tokenless remote claims never confer human identity or token scopes. The
// same canonical Human/Owner claim is refused remotely and admitted only for
// the fixture's independently verified local bridge identity. Unbound Control
// admission exercises that boundary without creating a PTY or child process.
//

#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "TrustedLocalPeer.h"

#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace Ubiquitty {
namespace Host {

// Friend seam: construct a minimal Daemon, inject a Client with a given
// peerProcess (empty ⇒ Remote/QUIC-shaped; the trusted bridge path ⇒ Local) and a
// real socketpair fd, then drive classification and canonical admission.
struct AgentClassifierOriginUnifyAccess {
    static void setRequireToken(Daemon& d, bool value) { d.m_requireToken = value; }

    // Close a client's owned daemon-side fd through the RAII owner — restores
    // the pre-RAII lifecycle (daemon end closed before the peer end), so the
    // Daemon destructor's EXIT-frame flush hits a cleanly-closed fd instead of
    // a broken pipe (SIGPIPE).
    static void closeClientFileDescriptor(Daemon& d, std::size_t index) { (void)d.m_clients[index].fileDescriptor.close(); }

    static std::size_t addClient(Daemon& d, const std::string& peerProcess, PAL::UnixFileDescriptor fileDescriptor)
    {
        Daemon::Client c {};
        c.clientID = ++d.m_nextClientID;
        c.peerProcess = peerProcess; // empty ⇒ Remote; the bridge ⇒ Local
        // Identity, which is what Local is: the daemon takes it at accept.
        c.hasPeerBinary = Ubiquitty::Host::binaryIdentityOf(peerProcess, c.peerBinary);
        c.fileDescriptor = std::move(fileDescriptor);
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    static bool isAgentClient(const Daemon& d, std::size_t index) { return d.isAgentClient(d.m_clients[index]); }

    static bool tokenScopeAllows(const Daemon& d, std::size_t index, Protocol::Sync::Scope scope) { return d.tokenScopeAllows(d.m_clients[index], scope); }

    static bool greetControl(Daemon& d, std::size_t index)
    {
        namespace Sync = Protocol::Sync;
        Sync::Greet greeting;
        greeting.wireVersion = Sync::wireVersion;
        greeting.appVersion = Sync::appVersion();
        greeting.clientInstance = { index + 1, 0x6f726967696e };
        greeting.clientKind = Sync::ClientKind::Human;
        greeting.requestedRole = Sync::Role::Owner;
        greeting.request = { .target = Sync::AdmissionTarget::Control,
            .expectedDaemonInstanceID = ScreenPlaneAccess::daemonInstanceID(d) };
        greeting.facts.viewportRows = 24;
        greeting.facts.viewportColumns = 80;
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        std::vector<std::uint8_t> frame;
        if (!Sync::decodeGreet(body) || !Sync::appendFrame(frame, Sync::FrameKind::Greet, 0, body))
            return false;
        // The connection queues its reply; the daemon's service pass moves it
        // into the client's output, as the run loop does after a read.
        ScreenPlaneAccess::receive(d, index, frame);
        ScreenPlaneAccess::service(d);
        ScreenPlaneAccess::flush(d, index);
        return true;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::AgentClassifierOriginUnifyAccess;
using Ubiquitty::Host::Daemon;
using Ubiquitty::PAL::UnixFileDescriptor;
namespace Sync = Ubiquitty::Protocol::Sync;

static int failures = 0;
static void check(bool ok, const char* msg)
{
    if (!ok) {
        std::fprintf(stderr, "agent_classifier_origin_unify: FAIL — %s\n", msg);
        ++failures;
    }
}

struct Reply {
    Sync::FrameKind kind;
    std::string body;
};

// Copies every complete control-channel reply flushed so far, in order; no
// decoder-borrowed body escapes. Empty when anything flushed is not a whole
// canonical control frame.
static std::vector<Reply> readFrames(int peerFileDescriptor)
{
    std::uint8_t buffer[4096];
    ssize_t n = ::read(peerFileDescriptor, buffer, sizeof(buffer));
    if (n <= 0)
        return {};
    Sync::FrameDecoder decoder;
    decoder.append({ buffer, static_cast<std::size_t>(n) });
    std::vector<Reply> replies;
    while (const auto frame = decoder.next()) {
        if (frame->channel)
            return {};
        replies.push_back({ frame->kind, std::string(frame->body.begin(), frame->body.end()) });
    }
    if (decoder.error() != Sync::EnvelopeError::None || decoder.bufferedBytes())
        return {};
    return replies;
}

int main()
{
    // The daemon ignores SIGPIPE (main.cpp does the same): a write to a client
    // whose peer end is gone must surface as EPIPE, not kill the process.
    ::signal(SIGPIPE, SIG_IGN);

    // A real socketpair per client: flushClient's real ::write() succeeds (no
    // EBADF → dropClient), and the test reads the daemon's reply off the peer end.
    int remoteFileDescriptors[2] = { -1, -1 };
    int localFileDescriptors[2] = { -1, -1 };
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, remoteFileDescriptors) == 0, "socketpair(remote) succeeds");
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, localFileDescriptors) == 0, "socketpair(local) succeeds");
    // Nonblocking read ends: the greeting's flush writes synchronously before
    // readFrames runs, so data is already queued — this just guarantees a
    // regression (no reply sent) fails the check instead of hanging CI.
    if (remoteFileDescriptors[1] >= 0)
        ::fcntl(remoteFileDescriptors[1], F_SETFL, O_NONBLOCK);
    if (localFileDescriptors[1] >= 0)
        ::fcntl(localFileDescriptors[1], F_SETFL, O_NONBLOCK);

    Daemon d;
    // Remote: no verified local identity (empty peerProcess) — a QUIC/remote
    // attach shape, exactly as origin_staleness.cpp's addRemoteClient mirrors it
    // (originOf's Remote guard is `quicConnection || peerProcess.empty()`).
    std::size_t remoteIndex = AgentClassifierOriginUnifyAccess::addClient(d, "", UnixFileDescriptor(remoteFileDescriptors[0]));
    // Local: the sole trusted-Local peer binary (the fail-safe marker).
    std::size_t localIndex = AgentClassifierOriginUnifyAccess::addClient(d, Ubiquitty::Host::trustedLocalPeerPath(), UnixFileDescriptor(localFileDescriptors[0]));

    // (a) isAgentClient — the tokenless, bare Remote peer is an agent.
    check(AgentClassifierOriginUnifyAccess::isAgentClient(d, remoteIndex), "a tokenless, bare REMOTE peer must be isAgentClient (not human)");

    // (d0) Local regression guard — the trusted bridge stays non-agent (human).
    check(!AgentClassifierOriginUnifyAccess::isAgentClient(d, localIndex), "the ubiquitty-host attach bridge must stay non-agent (human) — must not over-gate the GUI");

    // (b) tokenScopeAllows — the Remote peer is denied every scope bit.
    const Sync::Scope allScopes[] = { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::RegionRead, Sync::Scope::NewSession };
    for (Sync::Scope scope : allScopes) {
        check(!AgentClassifierOriginUnifyAccess::tokenScopeAllows(d, remoteIndex, scope), "a tokenless REMOTE peer must be DENIED every token scope");
    }

    // (d1) Local regression guard — the tokenless exemption still holds for Local.
    for (Sync::Scope scope : allScopes) {
        check(AgentClassifierOriginUnifyAccess::tokenScopeAllows(d, localIndex, scope), "the tokenless-Local exemption must still ALLOW every scope for the GUI bridge");
    }

    // A self-declared Human/Owner is still a tokenless remote peer.
    AgentClassifierOriginUnifyAccess::setRequireToken(d, true);
    check(AgentClassifierOriginUnifyAccess::greetControl(d, remoteIndex), "the remote canonical greeting encodes");
    {
        const auto replies = readFrames(remoteFileDescriptors[1]);
        const Reply* frame = replies.size() == 1 ? &replies.front() : nullptr;
        check(frame && frame->kind == Sync::FrameKind::Bye, "a tokenless remote peer receives only Bye, never Welcome");
        if (frame && frame->kind == Sync::FrameKind::Bye) {
            const auto denied = Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(frame->body.data()), frame->body.size() });
            check(denied && denied->reason == Sync::ByeReason::TokenMissing, "the exact refusal is TokenMissing");
        }
        const auto* connection = Ubiquitty::Host::ScreenPlaneAccess::connection(d, remoteIndex);
        check(connection && connection->state() == Sync::ConnectionState::Closed, "refused claims leave no admitted connection");
    }

    // The same claim succeeds for the independently verified bridge identity.
    check(AgentClassifierOriginUnifyAccess::greetControl(d, localIndex), "the local canonical greeting encodes");
    {
        // Welcome leads; the admitted connection's attachments roster follows it.
        const auto replies = readFrames(localFileDescriptors[1]);
        const Reply* frame = replies.empty() ? nullptr : &replies.front();
        check(frame && frame->kind == Sync::FrameKind::Welcome, "the verified local bridge receives Welcome");
        check(std::ranges::none_of(replies, [](const Reply& reply) { return reply.kind == Sync::FrameKind::Bye; }), "the verified local bridge is never refused");
        if (frame && frame->kind == Sync::FrameKind::Welcome) {
            const auto welcome = Sync::decodeWelcome({ reinterpret_cast<const std::uint8_t*>(frame->body.data()), frame->body.size() });
            check(welcome && welcome->admission == Sync::AttachmentAdmission::Granted
                    && welcome->grantedRole == Sync::Role::Owner
                    && welcome->selection.target == Sync::AdmissionTarget::Control
                    && welcome->selection.daemonInstanceID == Ubiquitty::Host::ScreenPlaneAccess::daemonInstanceID(d)
                    && !welcome->selection.sessionID && !welcome->selection.workspaceID,
                "the actual local receipt grants only unbound Control on the selected daemon");
        }
        const auto* connection = Ubiquitty::Host::ScreenPlaneAccess::connection(d, localIndex);
        check(connection && connection->state() == Sync::ConnectionState::Attached, "the verified local bridge really completes admission");
    }

    // Close the daemon-side [0] ends through their Client owners before the
    // peers, as the pre-RAII test did: ~Daemon flushes an EXIT frame to every
    // client, and a live fd whose peer is already closed would take the write
    // to a broken pipe.
    AgentClassifierOriginUnifyAccess::closeClientFileDescriptor(d, remoteIndex);
    AgentClassifierOriginUnifyAccess::closeClientFileDescriptor(d, localIndex);
    if (remoteFileDescriptors[1] >= 0)
        ::close(remoteFileDescriptors[1]);
    if (localFileDescriptors[1] >= 0)
        ::close(localFileDescriptors[1]);

    if (failures == 0)
        std::printf("agent_classifier_origin_unify: PASSED — a tokenless REMOTE peer is isAgentClient, "
                    "denied every token scope, and rejected under require_token; "
                    "the Local GUI bridge is unaffected\n");
    return failures == 0 ? 0 : 1;
}
