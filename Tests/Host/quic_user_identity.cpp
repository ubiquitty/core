// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_user_identity.cpp — interactive bootstrap authority reaches
// the person's unlabelled QUIC client, never automation that claims a label.
//
// The physical lane uses the real trusted --quic-bootstrap producer and a real
// ngtcp2 carrier. A tokenless Local attach bridge first creates a session and
// retains its Granted serving receipt. The QUIC client then sends one Greet
// pinned to that session and daemon, applies its checkpoint and receives its
// own Human attachments row. A labelled bearer of the same bootstrap authority
// cannot attach an unattended session; an untrusted local minter gets no token.
//
// The friend lane separately checks the classifier truth table, TokenMissing,
// actual mint/consume authority versus Greet-supplied stored credentials, and
// token withdrawal. Its admitted cases do not manufacture hasToken authority.
// With an actual PTY it also checks a Human's unattended attach, a labelled
// peer's exact pending status/no pane, typed directory human counts and cwd,
// and whole UTF-8 workspace names at 4096 bytes versus malformed 4097 bytes.
// Those seams do not claim to exercise TLS or the QUIC preamble.
//
// Built only under -DUBIQUITTY_QUIC=ON, linked against the QUIC-enabled daemon
// objects. Exit 0 on success. argv[1] = the built ubiquitty-host binary.
//

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/Notify.h"
#include "Core/Protocol/Sync/QUICBootstrap.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Core/Session/NameLimits.h"
#include "Core/Terminal.h"
#include "NotifyRead.h"
#include "PAL/Socket.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostSocketName.h"
#include "Platform/Host/TokenStore.h"
#include "TrustedLocalPeer.h"

#include "ConsentTranscript.h"
#include "HostConnection.h"
#include "ScreenClient.h"
#include "ScreenPlaneAccess.h"
#include "quic_test_mint.h"

#include <openssl/ssl.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_boringssl.h>

#include <arpa/inet.h>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty {
namespace Host {

// Friend seam: the classifier, the require_token Greet gate and the directory
// fill are all private, and Daemon::Client cannot be named outside it.
struct QUICUserIdentityAccess {
    static void setShellPath(Daemon& d, const std::string& path) { d.m_shellPath = path; }
    static void setRequireToken(Daemon& d, bool isRequired) { d.m_requireToken = isRequired; } // bool-ok: test scaffolding sets one daemon fact per call; the name at each call site says which
    static void useTokenStore(Daemon& d, const std::string& path) { d.m_tokenPath = path; }

    // One client, shaped by what it presents rather than by where it came from:
    // an empty peerProcess is the remote/QUIC shape (originOf's Remote guard is
    // `quicConnection || peerProcess.empty()`), and the token fields are the ones
    // the QUIC preamble stamps. A real socketpair fd so flushClient's write()
    // succeeds instead of routing into the EBADF drop; non-blocking so a
    // checkpoint larger than the pair's buffer returns EAGAIN rather than wedging
    // this single-threaded test.
    static std::size_t addClient(Daemon& d, const std::string& peerProcess, bool hasToken, bool isInteractiveUser, int& readEndOut) // bool-ok: test scaffolding sets one daemon fact per call; the name at each call site says which
    {
        int fileDescriptors[2] = { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fileDescriptors) != 0)
            return static_cast<std::size_t>(-1);
        ::fcntl(fileDescriptors[0], F_SETFL, O_NONBLOCK);
        ::fcntl(fileDescriptors[1], F_SETFL, O_NONBLOCK);
        readEndOut = fileDescriptors[1];
        Daemon::Client c {};
        c.fileDescriptor = PAL::UnixFileDescriptor(fileDescriptors[0]);
        c.hasOwnerLocalCredentials = PAL::peerIsSameUser(c.fileDescriptor);
        c.clientID = d.m_nextClientID++;
        c.peerProcess = peerProcess;
        // The daemon takes this at accept; a seam that sets only the path
        // models a client it never builds, and Local silently becomes Agent.
        c.hasPeerBinary = Ubiquitty::Host::binaryIdentityOf(peerProcess, c.peerBinary);
        c.hasToken = hasToken;
        c.isInteractiveUser = isInteractiveUser;
        c.tokenID = hasToken ? "carried-digest" : std::string();
        c.tokenScope = Protocol::Sync::ScopeSet { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive, Protocol::Sync::Scope::Control, Protocol::Sync::Scope::NewSession };
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    // This is an explicit minter/consume seam, not a TLS or preamble test. The
    // fixture's trusted local binary must pass the same transport-origin gate;
    // only the actual consumed one-time verdict is installed on its remote peer.
    static std::size_t addBootstrapClient(Daemon& d, int& readEndOut)
    {
        int minterPeer = -1;
        const std::size_t minter = addClient(d, trustedLocalPeerPath(), false, false, minterPeer);
        if (minter == static_cast<std::size_t>(-1))
            return minter;
        const bool isTrustedMinter = admitsLocalControl(d.localControlPeer(d.m_clients[minter]));
        const Protocol::Sync::ScopeSet scope { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive,
            Protocol::Sync::Scope::Control, Protocol::Sync::Scope::RegionRead, Protocol::Sync::Scope::NewSession,
            Protocol::Sync::Scope::FileRead };
        const auto token = isTrustedMinter ? d.mintQUICToken(d.m_clients[minter].clientID, scope, true) : std::string();
        d.m_clients[minter].fileDescriptor.reset();
        ::close(minterPeer);
        Protocol::Sync::ScopeSet carriedScope;
        std::string carriedTokenID;
        bool isInteractiveUser = false;
        if (token.empty() || !d.consumeQUICToken(token, carriedScope, carriedTokenID, &isInteractiveUser))
            return static_cast<std::size_t>(-1);
        const std::size_t index = addClient(d, "", false, false, readEndOut);
        if (index == static_cast<std::size_t>(-1))
            return index;
        auto& client = d.m_clients[index];
        client.hasToken = true;
        client.tokenScope = carriedScope;
        client.tokenID = std::move(carriedTokenID);
        client.hasBootstrapToken = true;
        client.isInteractiveUser = isInteractiveUser;
        client.quicAuthed = true;
        return index;
    }

    // agentLabel is read from Greet for the gates, and set on the client only
    // once that Greet is handled — a shape the classifier table needs before any
    // handshake runs.
    static void setAgentLabel(Daemon& d, std::size_t index, const std::string& label) { d.m_clients[index].agentLabel = label; }

    static bool isAgentClient(const Daemon& d, std::size_t index) { return d.isAgentClient(d.m_clients[index]); }
    static std::uint32_t clientSessionID(const Daemon& d, std::size_t index) { return d.m_clients[index].sessionID; }

    // A real session with a live PTY and no client on it — the detached-but-alive
    // state a person's phone comes back to.
    static std::uint32_t createSession(Daemon& d, const std::vector<std::string>& command)
    {
        Daemon::Session* s = d.createSession(80, 24, command);
        return s ? s->id : 0;
    }

    static void driveFrame(Daemon& d, std::size_t index, const std::vector<std::uint8_t>& wire)
    {
        ScreenPlaneAccess::receive(d, index, wire);
    }

    static void flushClient(Daemon& d, std::size_t index) { d.flushClient(index); }
    static void serviceScreen(Daemon& d, std::size_t index)
    {
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
        d.flushClient(index);
    }

    static void closeClientFileDescriptor(Daemon& d, std::size_t index) { d.m_clients[index].fileDescriptor.reset(); }

    // The pass an authenticated local credential reload runs.
    static void reverifyTokenClients(Daemon& d) { d.reverifyTokenClients(); }

    // Ordered refusal closes the protocol owner, not a transient overflow flag.
    static bool wasDenied(const Daemon& d, std::size_t index)
    {
        const auto& connection = d.m_clients[index].syncConnection;
        return connection && connection->state() == Protocol::Sync::ConnectionState::Closed
            && connection->byeReason() == Protocol::Sync::ByeReason::TokenRevoked;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::QUICUserIdentityAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

const char aLPN[] = "\x03mtq"; // must match HostQUIC's qUICAlpn

int failures = 0;
void check(bool ok, const char* what) // bool-ok: the assertion's verdict, which is the argument this helper exists to take
{
    if (!ok) {
        std::fprintf(stderr, "quic_user_identity: FAIL — %s\n", what);
        ++failures;
    }
}

// ---- frames, either transport ----------------------------------------------

struct DecodedFrame {
    Sync::FrameKind kind = Sync::FrameKind::Bye;
    std::uint32_t channel = 0;
    std::vector<std::uint8_t> payload;
};

void takeFrames(Sync::FrameDecoder& decoder, std::vector<DecodedFrame>& out)
{
    while (const auto frame = decoder.next())
        out.push_back({ frame->kind, frame->channel, { frame->body.begin(), frame->body.end() } });
}

const DecodedFrame* frameOfKind(const std::vector<DecodedFrame>& frames, Sync::FrameKind kind)
{
    for (const DecodedFrame& f : frames)
        if (f.kind == kind)
            return &f;
    return nullptr;
}

// Whether a frame opens a pane's checkpoint; pending clients receive none.
bool isScreen(const DecodedFrame& frame)
{
    return frame.kind == Sync::FrameKind::CheckpointHead && frame.channel && Sync::decodeCheckpointHead(frame.payload).has_value();
}

Sync::Greet selectedGreeting(Sync::AdmissionRequest request, std::uint64_t clientID,
    std::optional<std::string> credential = std::nullopt)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = clientID, .high = 0x717569636964 };
    greeting.request = std::move(request);
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    if (credential)
        greeting.credential = std::vector<std::uint8_t>(credential->begin(), credential->end());
    return greeting;
}

std::vector<std::uint8_t> greetingWire(const Sync::Greet& greeting)
{
    const auto wire = Ubiquitty::HostTest::greetFrame(greeting);
    check(!wire.empty(), "the fixture's selected canonical Greet encodes completely");
    return { wire.begin(), wire.end() };
}

Sync::AdmissionRequest selectedSession(std::uint32_t sessionID, std::uint64_t daemonInstanceID,
    std::string agentLabel = {}, std::string workspaceName = {})
{
    return { .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = daemonInstanceID, .workspaceName = std::move(workspaceName), .agentLabel = std::move(agentLabel), .isAttachOnly = sessionID != 0 };
}

// Corrupt just the latest canonical workspace-name field beyond its bound.
// The production encoder must refuse this value, so the negative keeps the
// valid envelope and all other fields rather than using an obsolete grammar.
std::vector<std::uint8_t> oversizedWorkspaceGreeting(const Sync::Greet& greeting)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, greeting);
    const auto& name = greeting.request.workspaceName;
    const auto start = std::search(body.begin(), body.end(), name.begin(), name.end());
    if (name.size() != Ubiquitty::Session::maxWorkspaceNameBytes || start == body.end()
        || start - body.begin() < 2 || *(start - 2) != 0x80 || *(start - 1) != 0x20)
        return {};
    *(start - 2) = 0x81; // canonical 4097-byte length, not an overlong integer
    body.insert(start + static_cast<std::ptrdiff_t>(name.size()), 'x');
    check(!Sync::decodeGreet(body), "the oversized latest Greet is actually malformed");
    std::vector<std::uint8_t> wire;
    if (!Sync::appendFrame(wire, Sync::FrameKind::Greet, 0, body))
        return {};
    return wire;
}

// Everything the daemon has flushed to `peerFileDescriptor` so far, bounded.
// The peer end is non-blocking, so a quiet connection costs the poll timeout
// once rather than hanging a regression out as a stalled suite.
// A seam with pending output supplies the flush the real run loop retries on
// POLLOUT; reading the peer alone cannot complete a short daemon write.
template<typename Flush>
std::vector<DecodedFrame> readFlushedFrames(int peerFileDescriptor, int milliseconds, Flush flush)
{
    Sync::FrameDecoder decoder;
    std::vector<DecodedFrame> frames;
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 20) {
        flush();
        pollfd p { peerFileDescriptor, POLLIN, 0 };
        if (::poll(&p, 1, 20) <= 0)
            continue;
        std::uint8_t buffer[65536];
        const ssize_t n = ::read(peerFileDescriptor, buffer, sizeof(buffer));
        if (n <= 0)
            break;
        decoder.append({ buffer, static_cast<std::size_t>(n) });
        takeFrames(decoder, frames);
    }
    check(decoder.error() == Sync::EnvelopeError::None, "the flushed output contains only canonical envelopes");
    return frames;
}

std::vector<DecodedFrame> readFlushedFrames(int peerFileDescriptor, int milliseconds)
{
    return readFlushedFrames(peerFileDescriptor, milliseconds, [] { });
}

// ---- the ngtcp2 client (the scaffolding quic_session_relay establishes) ------

std::uint64_t nowNanoseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

void fillRandom(std::uint8_t* dest, std::size_t length)
{
    static int urandom = ::open("/dev/urandom", O_RDONLY);
    if (urandom < 0 || ::read(urandom, dest, length) != static_cast<ssize_t>(length))
        for (std::size_t i = 0; i < length; ++i)
            dest[i] = static_cast<std::uint8_t>(i * 2654435761u);
}

struct QUICClient {
    ngtcp2_conn* connection = nullptr;
    ngtcp2_crypto_conn_ref connectionRef {};
    SSL_CTX* ctx = nullptr;
    SSL* tls = nullptr;
    int fileDescriptor = -1;
    sockaddr_in localAddr {};
    sockaddr_in remoteAddr {};
    bool isHandshakeDone = false;
    std::int64_t streamID = -1;
    std::string toSend;
    Sync::FrameDecoder decoder;
    std::vector<DecodedFrame> frames;
};

int clientReceiveStreamHandler(ngtcp2_conn*, std::uint32_t, std::int64_t, std::uint64_t, const std::uint8_t* data, std::size_t datalen, void* user, void*)
{
    QUICClient& c = *static_cast<QUICClient*>(user);
    c.decoder.append({ data, datalen });
    takeFrames(c.decoder, c.frames);
    return 0;
}

ngtcp2_conn* getConnectionFromRef(ngtcp2_crypto_conn_ref* ref) { return static_cast<QUICClient*>(ref->user_data)->connection; }
void randHandler(std::uint8_t* d, std::size_t n, const ngtcp2_rand_ctx*) { fillRandom(d, n); }
int newConnectionIdHandler(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* tok, std::size_t cidlen, void*)
{
    fillRandom(cid->data, cidlen);
    cid->datalen = cidlen;
    fillRandom(tok->data, sizeof(tok->data));
    return 0;
}
int handshakeDoneHandler(ngtcp2_conn*, void* user)
{
    static_cast<QUICClient*>(user)->isHandshakeDone = true;
    return 0;
}

ngtcp2_path clientPath(QUICClient& c)
{
    ngtcp2_path p;
    p.local.addr = reinterpret_cast<sockaddr*>(&c.localAddr);
    p.local.addrlen = sizeof(c.localAddr);
    p.remote.addr = reinterpret_cast<sockaddr*>(&c.remoteAddr);
    p.remote.addrlen = sizeof(c.remoteAddr);
    return p;
}

bool clientRead(QUICClient& c)
{
    std::uint8_t buffer[65536];
    for (;;) {
        sockaddr_in from;
        socklen_t fromLength = sizeof(from);
        const ssize_t n = ::recvfrom(c.fileDescriptor, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (n < 0)
            return true;
        ngtcp2_path path = clientPath(c);
        ngtcp2_pkt_info pi {};
        if (ngtcp2_conn_read_pkt(c.connection, &path, &pi, buffer, static_cast<std::size_t>(n), nowNanoseconds()) != 0)
            return false;
    }
}

bool clientWrite(QUICClient& c)
{
    std::uint8_t buffer[1452];
    for (;;) {
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi {};
        ngtcp2_ssize wdatalen = 0;
        std::int64_t streamID = -1;
        ngtcp2_vec datav { nullptr, 0 };
        int datavcnt = 0;
        if (c.streamID >= 0 && !c.toSend.empty()) {
            datav.base = reinterpret_cast<std::uint8_t*>(c.toSend.data());
            datav.len = c.toSend.size();
            streamID = c.streamID;
            datavcnt = 1;
        }
        const ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(c.connection, &ps.path, &pi, buffer, sizeof(buffer), &wdatalen, NGTCP2_WRITE_STREAM_FLAG_NONE, streamID, datavcnt ? &datav : nullptr, datavcnt, nowNanoseconds());
        if (nwrite < 0)
            return false;
        if (wdatalen > 0)
            c.toSend.erase(0, static_cast<std::size_t>(wdatalen));
        if (nwrite == 0)
            return true;
        ::sendto(c.fileDescriptor, buffer, static_cast<std::size_t>(nwrite), 0, reinterpret_cast<sockaddr*>(&c.remoteAddr), sizeof(c.remoteAddr));
    }
}

// Honor ngtcp2's loss/PTO timer so a dropped Initial gets retransmitted.
void clientExpiry(QUICClient& c)
{
    const std::uint64_t now = nowNanoseconds();
    const std::uint64_t expiry = ngtcp2_conn_get_expiry(c.connection);
    if (expiry != UINT64_MAX && expiry <= now)
        ngtcp2_conn_handle_expiry(c.connection, now);
}

std::uint16_t freeUDPPort()
{
    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t length = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &length);
    const std::uint16_t port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

// What one QUIC handshake answered. isSelfHuman is meaningful only
// alongside sawPresenceSelf.
struct QUICOutcome {
    bool isHandshakeDone = false;
    bool sawWelcome = false;
    bool sawScreen = false;
    bool sawPresenceSelf = false;
    bool isSelfHuman = false;
    bool sawConsentPending = false;
    std::optional<Sync::ByeReason> refusal;
    bool hasInvalidSelection = false;
    bool hasMalformedFrames = false;
};

void readOutcome(QUICClient& client, const Sync::AdmissionRequest& request, std::string_view socketName, QUICOutcome& out)
{
    out.isHandshakeDone = client.isHandshakeDone;
    out.hasMalformedFrames = client.decoder.error() != Sync::EnvelopeError::None;
    std::optional<Sync::Welcome> welcomed;
    for (const DecodedFrame& f : client.frames) {
        switch (f.kind) {
        case Sync::FrameKind::Welcome: {
            const auto welcome = Sync::decodeWelcome(f.payload);
            if (!welcome || f.channel || welcome->selection.target != request.target
                || welcome->selection.sessionID != request.sessionID
                || welcome->selection.daemonInstanceID != request.expectedDaemonInstanceID
                || welcome->selection.socketName != socketName) {
                out.hasInvalidSelection = true;
                break;
            }
            out.sawWelcome = true;
            welcomed = welcome;
            break;
        }
        case Sync::FrameKind::Notify: {
            const auto notify = UbiquittyTest::notifyOf(f.payload);
            if (!notify)
                break;
            if (const auto status = UbiquittyTest::consentStatusOf(*notify))
                out.sawConsentPending = out.sawConsentPending || status->phase == Ubiquitty::Protocol::Sync::ConsentStatusPhase::Pending;
            if (const auto attachments = UbiquittyTest::attachmentsOf(*notify)) {
                for (const auto& attachment : attachments->attachments) {
                    if (!attachment.isSelf || attachment.state != Ubiquitty::Protocol::Sync::AttachmentState::Live)
                        continue;
                    if (!welcomed || attachment.attachmentID != welcomed->attachmentID) {
                        out.hasInvalidSelection = true;
                        continue;
                    }
                    out.sawPresenceSelf = true;
                    out.isSelfHuman = attachment.clientKind == Ubiquitty::Protocol::Sync::ClientKind::Human;
                }
            }
            break;
        }
        case Sync::FrameKind::Bye:
            if (const auto bye = Sync::decodeBye(f.payload))
                out.refusal = bye->reason;
            break;
        default:
            break;
        }
    }
}

// Dial the daemon's QUIC port, lead the stream with the one-time `token`
// preamble, send one explicitly selected Greet, and pump until admission answers
// or the budget runs out. `wantPresence` keeps pumping past the checkpoint for
// the roster the daemon pushes on attach.
void runQUICAdmission(std::uint16_t port, const std::string& token, const Sync::AdmissionRequest& request,
    std::string_view socketName, bool wantPresence, QUICOutcome& out) // bool-ok: test scaffolding sets one daemon fact per call; the name at each call site says which
{
    QUICClient client;
    client.fileDescriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
    client.localAddr.sin_family = AF_INET;
    client.localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(client.fileDescriptor, reinterpret_cast<sockaddr*>(&client.localAddr), sizeof(client.localAddr));
    socklen_t localLength = sizeof(client.localAddr);
    ::getsockname(client.fileDescriptor, reinterpret_cast<sockaddr*>(&client.localAddr), &localLength);
    const int flags = ::fcntl(client.fileDescriptor, F_GETFL, 0);
    ::fcntl(client.fileDescriptor, F_SETFL, flags | O_NONBLOCK);
    client.remoteAddr.sin_family = AF_INET;
    client.remoteAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    client.remoteAddr.sin_port = htons(port);

    client.ctx = SSL_CTX_new(TLS_method());
    SSL_CTX_set_min_proto_version(client.ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(client.ctx, TLS1_3_VERSION);
    if (ngtcp2_crypto_boringssl_configure_client_context(client.ctx) != 0) {
        ::close(client.fileDescriptor);
        return;
    }
    SSL_CTX_set_verify(client.ctx, SSL_VERIFY_NONE, nullptr); // the pin is not this test's subject
    client.tls = SSL_new(client.ctx);
    SSL_set_connect_state(client.tls); // AWS-LC TLS_method() is role-agnostic
    SSL_set_alpn_protos(client.tls, reinterpret_cast<const unsigned char*>(aLPN), sizeof(aLPN) - 1);
    client.connectionRef.get_conn = getConnectionFromRef;
    client.connectionRef.user_data = &client;
    SSL_set_app_data(client.tls, &client.connectionRef);

    ngtcp2_cid dcid, scid;
    dcid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
    fillRandom(dcid.data, dcid.datalen);
    scid.datalen = 8;
    fillRandom(scid.data, scid.datalen);

    ngtcp2_callbacks cb {};
    cb.client_initial = ngtcp2_crypto_client_initial_cb;
    cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    cb.encrypt = ngtcp2_crypto_encrypt_cb;
    cb.decrypt = ngtcp2_crypto_decrypt_cb;
    cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
    cb.recv_retry = ngtcp2_crypto_recv_retry_cb;
    cb.update_key = ngtcp2_crypto_update_key_cb;
    cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    cb.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    cb.rand = randHandler;
    cb.get_new_connection_id2 = newConnectionIdHandler;
    cb.handshake_completed = handshakeDoneHandler;
    cb.recv_stream_data = clientReceiveStreamHandler;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = nowNanoseconds();
    ngtcp2_transport_params parameters;
    ngtcp2_transport_params_default(&parameters);
    parameters.initial_max_streams_bidi = 4;
    parameters.initial_max_data = 4 * 1024 * 1024;
    parameters.initial_max_stream_data_bidi_local = 1024 * 1024;

    ngtcp2_path path = clientPath(client);
    if (ngtcp2_conn_client_new(&client.connection, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &cb, &settings, &parameters, nullptr, &client) != 0) {
        ::close(client.fileDescriptor);
        return;
    }
    ngtcp2_conn_set_tls_native_handle(client.connection, client.tls);

    UbiquittyTest::ScreenClient screen(24, 80);
    if (!screen.start(request)) {
        check(false, "the QUIC client starts with its independent target and pin");
        ::close(client.fileDescriptor);
        return;
    }
    const auto wire = screen.takeUpstream(UbiquittyTest::screenClientNow());
    bool didSendGreet = false;
    std::size_t viewsScanned = 0;
    // The preamble leaves on its own and the Sync frames after it, as the
    // product's dialer sends them: the daemon holds no more than a preamble's
    // bytes before the token checks out.
    int preambleSentAt = -1;
    clientWrite(client); // client Initial
    for (int iter = 0; iter < 5000; ++iter) {
        clientExpiry(client);
        if (!clientWrite(client))
            break;
        if (!clientRead(client))
            break;
        if (client.isHandshakeDone && preambleSentAt < 0) {
            if (ngtcp2_conn_open_bidi_stream(client.connection, &client.streamID, &client) == 0) {
                client.toSend.push_back(static_cast<char>(Ubiquitty::Protocol::Sync::PreambleKind::Token));
                client.toSend.push_back(static_cast<char>(token.size()));
                client.toSend.append(token);
                preambleSentAt = iter;
            }
        }
        if (preambleSentAt >= 0 && !didSendGreet && iter - preambleSentAt >= 50) {
            client.toSend.append(reinterpret_cast<const char*>(wire.data()), wire.size());
            didSendGreet = true;
        }
        // The real reader preserves fragmented envelopes and sends its view,
        // credit and ACKs rather than assuming one envelope per carrier.
        for (; viewsScanned < client.frames.size(); ++viewsScanned) {
            const DecodedFrame& frame = client.frames[viewsScanned];
            std::vector<std::uint8_t> received;
            check(Sync::appendFrame(received, frame.kind, frame.channel, frame.payload), "the decoded canonical QUIC frame retains its actual channel");
            screen.read(received, UbiquittyTest::screenClientNow());
        }
        const auto upstream = screen.takeUpstream(UbiquittyTest::screenClientNow());
        client.toSend.append(upstream.begin(), upstream.end());
        readOutcome(client, request, socketName, out);
        const auto panes = screen.connection().boundPanes();
        out.sawScreen = panes.size() == 1 && panes.front().second.low == request.sessionID
            && panes.front().second.high == request.expectedDaemonInstanceID
            && screen.connection().isPaneCurrent(panes.front().first)
            && screen.connection().counters().checkpointsApplied != 0;
        if (out.refusal || out.sawConsentPending || out.hasInvalidSelection || out.hasMalformedFrames)
            break;
        if (out.sawScreen && (!wantPresence || out.sawPresenceSelf))
            break;
        ::usleep(1000);
    }
    readOutcome(client, request, socketName, out);
    ::close(client.fileDescriptor);
}

// ---- the local half: create a session through the real attach bridge ---------

// A session created by an origin-Local client and left with nobody on it. The
// bridge is the real `ubiquitty-host --attach` relay, so the daemon's
// kernel-verified peer binary resolves Local exactly as it does for the GUI; a
// raw connection from this test binary would be an agent whatever it presented.
std::optional<Sync::ServingSelection> createAndAbandonSession(const std::string& hostBinaryPath, const std::string& macHome)
{
    using namespace Ubiquitty::HostTest;
    LocalBridgeConnection bridge = connectDaemonAsLocalBridge(hostBinaryPath, macHome);
    if (bridge.fileDescriptor < 0)
        return std::nullopt;
    Connection owner;
    owner.fileDescriptor = bridge.fileDescriptor;
    const auto greeting = selectedGreeting({ .target = Sync::AdmissionTarget::Session }, 1);
    const bool didGreet = sendGreeting(owner, greeting);
    std::string body;
    std::optional<Sync::ServingSelection> selected;
    if (didGreet && waitFor(owner, Sync::FrameKind::Welcome, body, 5000)) {
        const auto welcome = Sync::decodeWelcome({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
        if (welcome && welcome->admission == Sync::AttachmentAdmission::Granted
            && welcome->selection.target == Sync::AdmissionTarget::Session && welcome->selection.sessionID
            && welcome->selection.daemonInstanceID && welcome->selection.socketName == Ubiquitty::Host::socketLeafForBinary(hostBinaryPath.c_str()))
            selected = welcome->selection;
    }
    ::close(bridge.fileDescriptor);
    if (bridge.bridgeProcessID > 0) {
        ::kill(bridge.bridgeProcessID, SIGTERM);
        ::waitpid(bridge.bridgeProcessID, nullptr, 0);
    }
    // The daemon reaps the departed client on its next run-loop pass; the session
    // it made outlives it, which is the state this test needs it in.
    ::usleep(400 * 1000);
    return selected;
}

// ---- half one: the classifier, the gate and the list fill at the seam --------

void runSeamChecks()
{
    // Classifier-only inputs: these assertions do not admit an attachment or
    // claim to prove token verification, TLS, or preamble processing.
    {
        Daemon d;
        int peer = -1;
        const std::size_t interactive = QUICUserIdentityAccess::addClient(d, "", /*hasToken*/ true, /*isInteractiveUser*/ true, peer);
        int labelledPeer = -1;
        const std::size_t labelled = QUICUserIdentityAccess::addClient(d, "", true, true, labelledPeer);
        QUICUserIdentityAccess::setAgentLabel(d, labelled, "mcp");
        int persistedPeer = -1;
        const std::size_t persisted = QUICUserIdentityAccess::addClient(d, "", /*hasToken*/ true, /*isInteractiveUser*/ false, persistedPeer);
        int tokenlessPeer = -1;
        const std::size_t tokenless = QUICUserIdentityAccess::addClient(d, "", /*hasToken*/ false, /*isInteractiveUser*/ false, tokenlessPeer);
        int localPeer = -1;
        const std::size_t localBridge = QUICUserIdentityAccess::addClient(d, Ubiquitty::Host::trustedLocalPeerPath(), false, false, localPeer);
        // A client the uid gate admitted but the Local allowlist does not name.
        int localAgentPeer = -1;
        const std::size_t localAgent = QUICUserIdentityAccess::addClient(d, "/bin/zsh", false, false, localAgentPeer);

        check(!QUICUserIdentityAccess::isAgentClient(d, interactive), "an unlabelled interactive-user token bearer is a person, not an agent");
        check(QUICUserIdentityAccess::isAgentClient(d, labelled), "an interactive-user token bearer that names an agentLabel is still an agent");
        check(QUICUserIdentityAccess::isAgentClient(d, persisted), "a persisted-token client carries no interactive-user mark and stays an agent");
        check(QUICUserIdentityAccess::isAgentClient(d, tokenless), "a tokenless remote peer is still an agent");
        check(!QUICUserIdentityAccess::isAgentClient(d, localBridge), "the ubiquitty-host attach bridge is still a person");
        check(QUICUserIdentityAccess::isAgentClient(d, localAgent), "an unlabelled non-bridge local binary is still an agent");

        for (std::size_t i : { interactive, labelled, persisted, tokenless, localBridge, localAgent })
            QUICUserIdentityAccess::closeClientFileDescriptor(d, i);
        for (int fileDescriptor : { peer, labelledPeer, persistedPeer, tokenlessPeer, localPeer, localAgentPeer })
            ::close(fileDescriptor);
    }

    // require_token: a tokenless remote peer's Greet is refused for the
    // token it did not present. The mark cannot be reached without a token,
    // so this guarantee cannot be talked into by an attacker.
    {
        Daemon d;
        QUICUserIdentityAccess::setRequireToken(d, true);
        int peer = -1;
        const std::size_t remote = QUICUserIdentityAccess::addClient(d, "", /*hasToken*/ false, /*isInteractiveUser*/ false, peer);
        QUICUserIdentityAccess::driveFrame(d, remote, greetingWire(selectedGreeting({ .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = Ubiquitty::Host::ScreenPlaneAccess::daemonInstanceID(d) }, remote + 1)));
        const std::vector<DecodedFrame> replies = readFlushedFrames(peer, 500, [&] { QUICUserIdentityAccess::serviceScreen(d, remote); });
        const DecodedFrame* denied = frameOfKind(replies, Sync::FrameKind::Bye);
        check(denied != nullptr, "a tokenless remote Greet under require_token receives Bye");
        check(frameOfKind(replies, Sync::FrameKind::Welcome) == nullptr, "a tokenless remote Greet under require_token is never welcomed");
        if (denied) {
            const auto reason = Sync::decodeBye(denied->payload);
            check(reason && reason->reason == Sync::ByeReason::TokenMissing, "the refusal names the missing token, not consent or scope");
        }
        QUICUserIdentityAccess::closeClientFileDescriptor(d, remote);
        ::close(peer);
    }

    // The mark belongs to the token that carried it. A Greet presenting a token
    // of its own replaces the preamble's, and the tokens file marks none, so a
    // client that swaps identities mid-handshake swaps back to an agent.
    {
        char storeTemplate[] = "/tmp/ubiquitty-quic-identity-store.XXXXXX";
        char* storeDirectory = ::mkdtemp(storeTemplate);
        check(storeDirectory != nullptr, "a scratch token store directory was made");
        if (storeDirectory) {
            const std::string tokenPath = std::string(storeDirectory) + "/tokens";
            const std::string plaintext = "a-store-token-a-store-token-a-store-token";
            Sync::TokenRecord record;
            record.hashHex = Ubiquitty::Crypto::sha256Hex(plaintext);
            record.scope = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Control };
            record.expiryUnix = 0; // no expiry
            record.label = "test";
            check(Ubiquitty::Host::writeTokens(tokenPath, { record }) == Ubiquitty::Host::TokenWriteOutcome::Durable,
                "the identity token store rewrote durably");

            Daemon d;
            QUICUserIdentityAccess::setRequireToken(d, true);
            QUICUserIdentityAccess::useTokenStore(d, tokenPath);
            int peer = -1;
            const std::size_t swapper = QUICUserIdentityAccess::addBootstrapClient(d, peer);
            check(swapper != static_cast<std::size_t>(-1), "the actual trusted mint and consume established bootstrap authority");
            if (swapper == static_cast<std::size_t>(-1))
                return;
            check(!QUICUserIdentityAccess::isAgentClient(d, swapper), "the preamble's mark makes this client a person before the handshake");
            QUICUserIdentityAccess::driveFrame(d, swapper, greetingWire(selectedGreeting({ .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = Ubiquitty::Host::ScreenPlaneAccess::daemonInstanceID(d) }, swapper + 1, plaintext)));
            check(QUICUserIdentityAccess::isAgentClient(d, swapper), "presenting a tokens-file token drops the interactive-user mark with the token it came from");
            QUICUserIdentityAccess::closeClientFileDescriptor(d, swapper);
            ::close(peer);
            ::unlink(tokenPath.c_str());
            ::rmdir(storeDirectory);
        }
    }

    // (9) A revoke reaches the tokens the store issued and nothing else. The
    // bootstrap digest a QUIC preamble carried was minted in memory for one
    // connection and written nowhere, so the store's silence about it is not a
    // revocation — matching it against the file would take every dialled-in
    // client down with any one revoke.
    {
        char storeTemplate[] = "/tmp/ubiquitty-quic-revoke-store.XXXXXX";
        char* storeDirectory = ::mkdtemp(storeTemplate);
        check(storeDirectory != nullptr, "a scratch revoke store directory was made");
        if (storeDirectory) {
            const std::string tokenPath = std::string(storeDirectory) + "/tokens";
            const std::string keeper = "a-kept-token-a-kept-token-a-kept-token";
            const std::string revokedToken = "a-revoked-token-a-revoked-token-a-revoked-token";
            Sync::TokenRecord record;
            record.hashHex = Ubiquitty::Crypto::sha256Hex(keeper);
            record.scope = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Control };
            record.expiryUnix = 0; // no expiry
            record.label = "kept";
            Sync::TokenRecord revokedRecord = record;
            revokedRecord.hashHex = Ubiquitty::Crypto::sha256Hex(revokedToken);
            check(Ubiquitty::Host::writeTokens(tokenPath, { record, revokedRecord }) == Ubiquitty::Host::TokenWriteOutcome::Durable,
                "the revoke store rewrote durably");

            Daemon d;
            QUICUserIdentityAccess::useTokenStore(d, tokenPath);
            int dialledPeer = -1;
            const std::size_t dialled = QUICUserIdentityAccess::addBootstrapClient(d, dialledPeer);
            check(dialled != static_cast<std::size_t>(-1), "the kept bootstrap authority came from actual mint and consume");
            if (dialled == static_cast<std::size_t>(-1))
                return;
            int revokedPeer = -1;
            const std::size_t revoked = QUICUserIdentityAccess::addClient(d, "", false, false, revokedPeer);
            int keptPeer = -1;
            const std::size_t kept = QUICUserIdentityAccess::addClient(d, "", false, false, keptPeer);
            const Sync::AdmissionRequest control { .target = Sync::AdmissionTarget::Control,
                .expectedDaemonInstanceID = Ubiquitty::Host::ScreenPlaneAccess::daemonInstanceID(d) };
            QUICUserIdentityAccess::driveFrame(d, dialled, greetingWire(selectedGreeting(control, dialled + 1)));
            QUICUserIdentityAccess::driveFrame(d, revoked, greetingWire(selectedGreeting(control, revoked + 1, revokedToken)));
            QUICUserIdentityAccess::driveFrame(d, kept, greetingWire(selectedGreeting(control, kept + 1, keeper)));
            for (const auto& [index, peer] : { std::pair { dialled, dialledPeer }, std::pair { revoked, revokedPeer }, std::pair { kept, keptPeer } })
                check(frameOfKind(readFlushedFrames(peer, 200, [&] { QUICUserIdentityAccess::serviceScreen(d, index); }), Sync::FrameKind::Welcome) != nullptr,
                    "the reload fixture first admits each real credential");
            check(Ubiquitty::Host::writeTokens(tokenPath, { record }) == Ubiquitty::Host::TokenWriteOutcome::Durable,
                "the fixture withdraws only the admitted revoked token");

            QUICUserIdentityAccess::reverifyTokenClients(d);

            check(!QUICUserIdentityAccess::wasDenied(d, dialled), "a revoke leaves the QUIC client whose token the store never issued");
            check(QUICUserIdentityAccess::wasDenied(d, revoked), "a revoke drops the client whose store token is gone");
            check(!QUICUserIdentityAccess::wasDenied(d, kept), "a revoke leaves the client whose store token is still there");
            {
                const std::vector<DecodedFrame> replies = readFlushedFrames(revokedPeer, 500);
                const DecodedFrame* denied = frameOfKind(replies, Sync::FrameKind::Bye);
                check(denied != nullptr, "the dropped client is told why");
                if (denied) {
                    const auto reason = Sync::decodeBye(denied->payload);
                    check(reason && reason->reason == Sync::ByeReason::TokenRevoked, "the refusal names the revoke");
                }
            }
            check(frameOfKind(readFlushedFrames(dialledPeer, 200), Sync::FrameKind::Bye) == nullptr,
                "the QUIC client is told nothing by a revoke that is not about it");

            for (std::size_t i : { dialled, revoked, kept })
                QUICUserIdentityAccess::closeClientFileDescriptor(d, i);
            for (int fileDescriptor : { dialledPeer, revokedPeer, keptPeer })
                ::close(fileDescriptor);
            ::unlink(tokenPath.c_str());
            ::rmdir(storeDirectory);
        }
    }

    // (6) + (7) the attach gate and the list fill, against real sessions. The
    // shell is `cat`: it writes nothing, so no session here can emit an OSC 7 and
    // the listed directory can only have come from the process cwd.
    {
        Daemon d;
        QUICUserIdentityAccess::setRequireToken(d, true);
        QUICUserIdentityAccess::setShellPath(d, "/bin/cat");
        const std::uint32_t abandoned = QUICUserIdentityAccess::createSession(d, { "/bin/cat" });
        check(abandoned != 0, "the detached-but-alive session was created");
        const std::uint64_t daemonInstanceID = Ubiquitty::Host::ScreenPlaneAccess::daemonInstanceID(d);

        // The phone: an unlabelled interactive-user token bearer attaching a
        // session it did not create, with nobody on it to consent.
        int personPeer = -1;
        const std::size_t person = QUICUserIdentityAccess::addBootstrapClient(d, personPeer);
        check(person != static_cast<std::size_t>(-1), "a trusted minter supplied the person's actual bootstrap verdict");
        if (person == static_cast<std::size_t>(-1))
            return;
        QUICUserIdentityAccess::driveFrame(d, person, greetingWire(selectedGreeting(selectedSession(abandoned, daemonInstanceID), person + 1)));
        QUICUserIdentityAccess::serviceScreen(d, person);
        check(QUICUserIdentityAccess::clientSessionID(d, person) == abandoned, "an unlabelled interactive-user token bearer binds the unattended session it names");
        {
            const std::vector<DecodedFrame> replies = readFlushedFrames(personPeer, 500);
            const auto* answer = frameOfKind(replies, Sync::FrameKind::Welcome);
            const auto welcome = answer ? Sync::decodeWelcome(answer->payload) : std::nullopt;
            check(welcome && welcome->admission == Sync::AttachmentAdmission::Granted
                    && welcome->selection.target == Sync::AdmissionTarget::Session
                    && welcome->selection.sessionID == abandoned && welcome->selection.daemonInstanceID == daemonInstanceID,
                "that attach is granted for the exact independently selected session and daemon");
            check(frameOfKind(replies, Sync::FrameKind::Bye) == nullptr, "that attach is not refused for consent — the case a phone hits");
        }

        // The same token shape naming an agentLabel: an agent by its own account,
        // and the gate that was always there still holds it out. The person is
        // on the session by this point, so the gate's answer is a hold for that
        // person's consent rather than the outright refusal an empty session
        // earns — the handshake is acknowledged, canonical pending status arrives in
        // place of the checkpoint, and nothing is bound until the person says.
        int agentPeer = -1;
        const std::size_t agent = QUICUserIdentityAccess::addBootstrapClient(d, agentPeer);
        check(agent != static_cast<std::size_t>(-1), "the labelled peer also received an actual bootstrap verdict");
        if (agent == static_cast<std::size_t>(-1))
            return;
        QUICUserIdentityAccess::driveFrame(d, agent, greetingWire(selectedGreeting(selectedSession(abandoned, daemonInstanceID, "mcp"), agent + 1)));
        QUICUserIdentityAccess::serviceScreen(d, agent);
        check(QUICUserIdentityAccess::clientSessionID(d, agent) != abandoned, "a labelled token bearer does not bind a session it did not create");
        {
            const std::vector<DecodedFrame> replies = readFlushedFrames(agentPeer, 500);
            Ubiquitty::HostTest::ConsentTranscript transcript;
            for (const auto& reply : replies) {
                std::vector<std::uint8_t> frame;
                check(Sync::appendFrame(frame, reply.kind, reply.channel, reply.payload), "the pending transcript retains each canonical channel");
                check(transcript.feed(std::as_bytes(std::span(frame))), "pending consent preserves Welcome-before-status order and exact question identity");
            }
            const auto pending = transcript.takeStatus(Sync::ConsentStatusPhase::Pending);
            check(pending && pending->question.sessionID == abandoned, "a labelled token bearer's attach is held for the person's exact consent question");
            const auto* answer = frameOfKind(replies, Sync::FrameKind::Welcome);
            const auto welcome = answer ? Sync::decodeWelcome(answer->payload) : std::nullopt;
            check(welcome && welcome->admission == Sync::AttachmentAdmission::AwaitingConsent
                    && welcome->selection.sessionID == abandoned && welcome->selection.daemonInstanceID == daemonInstanceID,
                "the held Welcome identifies the exact selected target without granting it");
            check(frameOfKind(replies, Sync::FrameKind::PaneBind) == nullptr, "a held attach receives no pane binding");
            check(std::ranges::none_of(replies, isScreen), "a held attach receives no screen");
        }

        // Names at the shared bound survive whole, including a final
        // multi-byte character. One more byte is refused, never cut.
        const std::string longName(Ubiquitty::Session::maxWorkspaceNameBytes, 'a');
        const std::string splittingName = std::string(Ubiquitty::Session::maxWorkspaceNameBytes - 2, 'b') + "\xC3\xA9";
        int longPeer = -1;
        const std::size_t longNamer = QUICUserIdentityAccess::addBootstrapClient(d, longPeer);
        check(longNamer != static_cast<std::size_t>(-1), "the long-name creator has consumed bootstrap authority");
        if (longNamer == static_cast<std::size_t>(-1))
            return;
        QUICUserIdentityAccess::driveFrame(d, longNamer, greetingWire(selectedGreeting(selectedSession(0, daemonInstanceID, "", longName), longNamer + 1)));
        const std::uint32_t longSession = QUICUserIdentityAccess::clientSessionID(d, longNamer);
        check(longSession != 0, "a Greet naming a workspace at the bound creates a session");
        int splitPeer = -1;
        const std::size_t splitNamer = QUICUserIdentityAccess::addBootstrapClient(d, splitPeer);
        check(splitNamer != static_cast<std::size_t>(-1), "the UTF-8-name creator has consumed bootstrap authority");
        if (splitNamer == static_cast<std::size_t>(-1))
            return;
        QUICUserIdentityAccess::driveFrame(d, splitNamer, greetingWire(selectedGreeting(selectedSession(0, daemonInstanceID, "", splittingName), splitNamer + 1)));
        const std::uint32_t splitSession = QUICUserIdentityAccess::clientSessionID(d, splitNamer);
        check(splitSession != 0, "a Greet whose final character reaches the bound creates a session");
        int oversizedPeer = -1;
        const std::size_t oversizedNamer = QUICUserIdentityAccess::addBootstrapClient(d, oversizedPeer);
        check(oversizedNamer != static_cast<std::size_t>(-1), "the malformed-name peer has consumed bootstrap authority");
        if (oversizedNamer == static_cast<std::size_t>(-1))
            return;
        const auto malformed = oversizedWorkspaceGreeting(selectedGreeting(selectedSession(0, daemonInstanceID, "", longName), oversizedNamer + 1));
        check(!malformed.empty(), "the oversized-name negative carries a concrete latest Greet");
        QUICUserIdentityAccess::driveFrame(d, oversizedNamer, malformed);
        check(QUICUserIdentityAccess::clientSessionID(d, oversizedNamer) == 0, "an oversized workspace name creates and attaches nothing");
        const std::vector<DecodedFrame> oversizedReplies = readFlushedFrames(oversizedPeer, 200, [&] { QUICUserIdentityAccess::serviceScreen(d, oversizedNamer); });
        const auto* malformedAnswer = frameOfKind(oversizedReplies, Sync::FrameKind::Bye);
        const auto malformedBye = malformedAnswer ? Sync::decodeBye(malformedAnswer->payload) : std::nullopt;
        check(malformedBye && malformedBye->reason == Sync::ByeReason::Malformed, "an oversized workspace name is refused as malformed");
        check(frameOfKind(oversizedReplies, Sync::FrameKind::Welcome) == nullptr, "an oversized workspace name is never welcomed");

        // The list, read by a control-only client so no snapshot competes for the
        // socketpair's buffer. The two full names can still exceed that buffer;
        // service pending writes while reading, just as the daemon's loop does.
        int listerPeer = -1;
        const std::size_t lister = QUICUserIdentityAccess::addBootstrapClient(d, listerPeer);
        check(lister != static_cast<std::size_t>(-1), "the lister has consumed bootstrap authority");
        if (lister == static_cast<std::size_t>(-1))
            return;
        UbiquittyTest::ScreenClient screen(24, 80);
        check(screen.start({ .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = daemonInstanceID }),
            "the lister's one Greet explicitly selects unbound Control");
        Ubiquitty::Session::HostListing listing("identity.list");
        bool didSubscribe = false;
        for (unsigned attempt = 0; attempt < 50 && !listing.isWhole(); ++attempt) {
            const auto now = UbiquittyTest::screenClientNow();
            const auto upstream = screen.takeUpstream(now);
            if (!upstream.empty())
                QUICUserIdentityAccess::driveFrame(d, lister, upstream);
            Ubiquitty::Host::ScreenPlaneAccess::service(d, now);
            QUICUserIdentityAccess::flushClient(d, lister);
            pollfd pending { listerPeer, POLLIN, 0 };
            if (::poll(&pending, 1, 20) > 0) {
                std::uint8_t buffer[65536];
                const ssize_t count = ::read(listerPeer, buffer, sizeof(buffer));
                if (count > 0)
                    screen.read(std::span(buffer, static_cast<std::size_t>(count)), now);
            }
            if (!didSubscribe && screen.connection().welcome()) {
                const auto& welcome = *screen.connection().welcome();
                check(welcome.admission == Sync::AttachmentAdmission::Granted && welcome.selection.target == Sync::AdmissionTarget::Control
                        && !welcome.selection.sessionID && !welcome.selection.workspaceID && welcome.selection.daemonInstanceID == daemonInstanceID,
                    "the exact unbound Control receipt precedes the directory subscription");
                check(screen.call(Ubiquitty::Session::HostListing::subscribeRequest("identity.list")), "the verified user can request its typed directory");
                didSubscribe = true;
            }
            for (const auto& result : screen.takeResults())
                (void)listing.noteAnswer(result, 0);
            (void)listing.refresh(screen.connection(), attempt + 1, 0);
        }
        check(listing.isWhole(), "the control client's typed directory is complete");
        if (listing.isWhole()) {
            const std::optional<std::vector<Ubiquitty::Session::SessionRow>> listed = Ubiquitty::Session::sessionRows(listing.tree());
            if (listed) {
                const Ubiquitty::Session::SessionRow* abandonedRow = nullptr;
                const Ubiquitty::Session::SessionRow* longRow = nullptr;
                const Ubiquitty::Session::SessionRow* splitRow = nullptr;
                for (const Ubiquitty::Session::SessionRow& row : *listed) {
                    if (row.id == abandoned)
                        abandonedRow = &row;
                    else if (row.id == longSession)
                        longRow = &row;
                    else if (row.id == splitSession)
                        splitRow = &row;
                }
                check(abandonedRow != nullptr && longRow != nullptr && splitRow != nullptr, "every session created here is listed");
                if (abandonedRow) {
                    // `cat` emits nothing, so Terminal::currentDirectory is empty
                    // and an absolute path in this field is the process cwd the
                    // fill fell back to.
                    check(!abandonedRow->currentDirectory.empty(), "a session whose shell emitted no OSC 7 still lists a directory");
                    check(abandonedRow->currentDirectory.front() == '/', "the listed directory is the foreground process's real path");
                    check(abandonedRow->humanAttendeeCount == 1, "the attached interactive-user bearer counts as a human attendee");
                }
                if (longRow) {
                    check(longRow->workspaceName.size() == Ubiquitty::Session::maxWorkspaceNameBytes, "a workspace name at the bound is kept whole");
                    check(longRow->workspaceName == longName, "the name is unchanged");
                }
                if (splitRow) {
                    check(splitRow->workspaceName == splittingName, "a name ending in a multi-byte character is kept whole");
                }
            }
        }

        for (std::size_t i : { person, agent, longNamer, splitNamer, oversizedNamer, lister })
            QUICUserIdentityAccess::closeClientFileDescriptor(d, i);
        for (int fileDescriptor : { personPeer, agentPeer, longPeer, splitPeer, oversizedPeer, listerPeer })
            ::close(fileDescriptor);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-ubiquitty-host>\n", argv[0]);
        return 2;
    }
    // A write to a client the daemon has just dropped must surface as EPIPE, the
    // way main.cpp arranges it, rather than end this process.
    ::signal(SIGPIPE, SIG_IGN);
    const char* hostBinary = argv[1];

    runSeamChecks();

    // Isolated control directory: never a real ~/.ubiquitty, never a live daemon.
    char homeTemplate[] = "/tmp/ubiquitty-quic-identity.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "quic_user_identity: mkdtemp failed\n");
        return 1;
    }
    const std::string macHome = std::string(home) + "/.ubiquitty";
    const std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(hostBinary);

    const std::uint16_t port = freeUDPPort();
    char portArgument[16];
    std::snprintf(portArgument, sizeof(portArgument), "%u", port);

    const pid_t daemon = ::fork();
    if (daemon < 0) {
        std::fprintf(stderr, "quic_user_identity: fork failed\n");
        return 1;
    }
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        char* args[] = { const_cast<char*>(hostBinary), const_cast<char*>("--daemon"), const_cast<char*>("--quic-port"), portArgument, const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(hostBinary, args);
        _exit(127);
    }

    if (!Ubiquitty::HostTest::waitForDaemonReady(sockPath)) {
        std::fprintf(stderr, "quic_user_identity: FAIL — the daemon never accepted a connection\n");
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
        return 1;
    }

    // (1) + (2) the phone: a session someone left running, reached from a QUIC
    // client holding the token their own local client didMint.
    const auto abandoned = createAndAbandonSession(hostBinary, macHome);
    check(abandoned.has_value(), "the Local bridge created a session and retained its exact serving receipt");
    Sync::QUICBootstrapInfo personBootstrap;
    const bool didMint = mintQUICBootstrap(hostBinary, macHome, personBootstrap);
    check(didMint, "the trusted bridge didMint a QUIC bootstrap token through --quic-bootstrap");
    check(!didMint || personBootstrap.port == port, "the bootstrap line names the port this test's daemon bound");
    if (didMint && abandoned) {
        QUICOutcome outcome;
        runQUICAdmission(port, personBootstrap.token, selectedSession(abandoned->sessionID, abandoned->daemonInstanceID),
            Ubiquitty::Host::socketLeafForBinary(hostBinary), /*wantPresence*/ true, outcome);
        check(outcome.isHandshakeDone, "the QUIC handshake completed");
        check(outcome.sawWelcome && !outcome.hasInvalidSelection && !outcome.hasMalformedFrames, "the unlabelled user-token QUIC client receives the exact pinned Welcome");
        check(outcome.sawScreen, "the unlabelled user-token QUIC client receives the session's checkpoint");
        check(!outcome.refusal, "the unlabelled user-token QUIC client is not refused for consent");
        check(outcome.sawPresenceSelf, "the attached QUIC client receives its own canonical attachments row");
        check(outcome.isSelfHuman, "that row is flagged a human");
    }

    // (3) a bootstrap token naming an agentLabel, on a second unattended session:
    // held for a consent or refused outright, never handed a screen. The token is
    // the bridge's own, the one case (1) rides in on — the label alone makes its
    // bearer an agent, whatever the grant's mark says.
    const auto secondAbandoned = createAndAbandonSession(hostBinary, macHome);
    check(secondAbandoned.has_value(), "the Local bridge created a second unattended session and retained its serving receipt");
    Sync::QUICBootstrapInfo labelledBootstrap;
    const bool didMintAgentToken = mintQUICBootstrap(hostBinary, macHome, labelledBootstrap);
    check(didMintAgentToken, "the trusted bridge didMint a second QUIC bootstrap token");
    if (didMintAgentToken && secondAbandoned) {
        QUICOutcome outcome;
        runQUICAdmission(port, labelledBootstrap.token, selectedSession(secondAbandoned->sessionID, secondAbandoned->daemonInstanceID, "mcp"),
            Ubiquitty::Host::socketLeafForBinary(hostBinary), /*wantPresence*/ false, outcome);
        check(!outcome.sawScreen, "a labelled QUIC client is never handed the session's screen");
        check(outcome.refusal == Sync::ByeReason::NoConsent && !outcome.sawWelcome,
            "an unattended session refuses the labelled bearer before Welcome with exact NoConsent");
        check(!outcome.hasInvalidSelection && !outcome.hasMalformedFrames, "the labelled rejection is canonical and never supplied a mismatched serving receipt");
    }

    // (8) the raw-socket mint. mintQUICTokenForTest connects as this test binary —
    // an unlabelled non-bridge local peer, which originOf answers Agent for, the
    // shape any same-uid automation has. The token that mint would hand back
    // carries the full interactive scope and the person mark, so the daemon mints
    // nothing at all for it: identity and scope are the minter's to confer and
    // this minter holds neither. There is no such token for a bearer to present.
    std::string agentOriginToken, agentOriginCert;
    std::uint16_t agentOriginPort = 0;
    const bool didMintAgentOriginToken = mintQUICTokenForTest(sockPath, agentOriginToken, agentOriginPort, agentOriginCert);
    check(!didMintAgentOriginToken, "a same-uid local agent is refused the bootstrap mint");
    check(agentOriginToken.empty(), "the refused mint handed back no token");

    ::kill(daemon, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
            break;
        ::usleep(10 * 1000);
    }
    ::kill(daemon, SIGKILL);
    ::waitpid(daemon, nullptr, WNOHANG);

    if (failures == 0)
        std::printf("quic_user_identity: PASSED — a bootstrap token didMint through the trusted bridge makes "
                    "its unlabelled QUIC bearer a person (attach, snapshot, presence), while a labelled bearer, "
                    "a persisted-token client and a tokenless remote peer stay agents and a non-bridge local "
                    "client is refused the mint outright; the list names the "
                    "process cwd of an OSC 7-silent shell and cuts a workspace name on a character "
                    "boundary\n");
    return failures == 0 ? 0 : 1;
}
