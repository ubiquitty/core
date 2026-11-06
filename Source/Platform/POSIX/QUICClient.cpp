// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// QUICClient.cpp — see QUICClient.h. One ngtcp2 client connection driven by an
// internal I/O thread that owns the UDP socket + ngtcp2_conn, polls for datagrams
// and the loss/idle timer, and pumps writes.

#include "Core/Transport/QUICClient.h"
#include "Platform/POSIX/DatagramBatch.h"
#include "Platform/POSIX/QUICSendBuffer.h"
#include "Platform/POSIX/QUICTimestamp.h"

#include "PAL/UnixFileDescriptor.h"

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_boringssl.h>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <span>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace Ubiquitty::Transport {

namespace {

constexpr char quicALPN[] = "\x03mtq"; // matches HostQUIC's quicALPN
constexpr std::size_t clientConnectionIDLength = 8;

// Entropy from the TLS library this transport already links, which draws from
// the kernel through a cached generator and needs no descriptor of its own.
// Opening /dev/urandom per call would spend a descriptor each time, and the
// calls are not rare: ngtcp2 asks for randomness for every spare connection id
// at setup and again on every rotation. A descriptor per call would also turn
// descriptor exhaustion — an ordinary transient for a terminal juggling PTYs
// and sockets — into a failure to make a connection id.
bool fillRandom(std::span<std::uint8_t> dest)
{
    if (dest.empty())
        return true;
    return RAND_bytes(dest.data(), dest.size()) == 1;
}

void setNonBlocking(int fileDescriptor)
{
    int flags = ::fcntl(fileDescriptor, F_GETFL, 0);
    if (flags != -1)
        ::fcntl(fileDescriptor, F_SETFL, flags | O_NONBLOCK);
}

// The peer cert's DER SHA-256 equals `pin` (32 bytes). The bootstrap
// delivered `pin` over the trusted channel, so a MITM presenting any
// other cert fails here — before we send the bearer token.
bool peerCertMatchesPin(SSL* tls, const std::string& pin)
{
    if (!tls || pin.size() != SHA256_DIGEST_LENGTH)
        return false;
    X509* x509 = SSL_get_peer_certificate(tls); // ref'd — X509_free below
    if (!x509)
        return false;
    bool match = false;
    unsigned char* der = nullptr;
    const int derLength = i2d_X509(x509, &der); // allocates der; OPENSSL_free below
    if (der && derLength > 0) {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(der, static_cast<std::size_t>(derLength), digest);
        match = std::memcmp(digest, pin.data(), SHA256_DIGEST_LENGTH) == 0;
    }
    OPENSSL_free(der);
    X509_free(x509);
    return match;
}

} // namespace

// ---- the connection state, hidden from the header --------------------

struct QUICClient::Impl {
    Callbacks callbacks;
    std::string host;
    std::uint16_t port = 0;
    QUICCredential credential; // what the stream's preamble presents
    std::string pinnedCertSHA256; // expected peer cert DER hash (MITM defense)
    std::string description;

    // ngtcp2 + TLS handles as RAII (stateless-lambda deleters — zero-size, no
    // bespoke wrapper). Declared SSL_CTX → SSL → ngtcp2_conn so member
    // destruction (reverse order) frees the ngtcp2_conn first, then SSL, then
    // SSL_CTX — the same order stop()'s explicit teardown uses — and every
    // early-return in start() is leak-free by construction.
    std::unique_ptr<SSL_CTX, decltype([](SSL_CTX* context) { SSL_CTX_free(context); })> tlsContext;
    std::unique_ptr<SSL, decltype([](SSL* session) { SSL_free(session); })> tls;
    std::unique_ptr<ngtcp2_conn, decltype([](ngtcp2_conn* connection) { ngtcp2_conn_del(connection); })> connection;
    ngtcp2_crypto_conn_ref connectionRef {}; // TLS app-data → connection back-ref (non-owning)
    // The ngtcp2/AWS-LC C callbacks carry this Impl as a raw `void* user`
    // (connectionRef.user_data + ngtcp2_conn_client_new's user_data = &Impl) —
    // the one sanctioned raw seam a C API forces. It is lifetime-safe, not
    // checked-pointer-able: the callbacks fire only on the I/O thread while the
    // connection lives, and stop() joins that thread before any owner is
    // released, so `user` is never dangling when a callback dereferences it.

    PAL::UnixFileDescriptor udp; // the client's UDP socket
    // send()/stop() write a byte to `wakePipe.write` to break the I/O thread out
    // of poll() (see the wake() method below).
    struct SelfPipe {
        PAL::UnixFileDescriptor read;
        PAL::UnixFileDescriptor write;
    };
    SelfPipe wakePipe;
    sockaddr_storage localAddr {};
    socklen_t localLength = 0;
    sockaddr_storage remoteAddr {};
    socklen_t remoteLength = 0;

    std::jthread thread;
    // The frontend's single owner thread — the one that constructs the client and
    // drives start/stop/send. Captured once at construction (before the I/O thread
    // exists), then read-only, so the I/O thread reads it race-free. The debug
    // thread-affinity asserts key on it; see QUICClient::start.
    std::thread::id ownerThreadID;
    std::atomic<bool> stopRequested { false };

    std::mutex outboundMutex;
    std::vector<std::uint8_t> outbound; // queued by send(), drained by the thread
    // Datagrams queued by sendDatagram(), under outboundMutex, and the ones the
    // I/O thread has taken and not yet written. Each is sent once or dropped.
    std::deque<std::vector<std::uint8_t>> outboundDatagrams;
    std::deque<std::vector<std::uint8_t>> toSendDatagrams;
    // The largest DATAGRAM frame the daemon accepts, from its transport
    // parameters once the handshake completes; 0 until then, and for a peer
    // that takes none.
    std::atomic<std::size_t> peerDatagramMaxBytes { 0 };
    // The relay stream's outbound bytes, staged on the I/O thread. ngtcp2 does not
    // copy stream data: writev_stream requires the caller to keep the written bytes
    // intact until acked_stream_data_offset reports them acknowledged, because a
    // lost packet is rebuilt from the same pointers. QUICSendBuffer never moves a
    // byte it holds, and releases whole blocks behind the acknowledgement.
    POSIX::QUICSendBuffer toSend;
    bool hasOutboundOverflowed = false; // send() hit the limit; drain turns it into onClose(Failed)

    std::int64_t streamID = -1;
    // Every timestamp the connection's ngtcp2 calls receive: ngtcp2 aborts the
    // process on one lower than the last. Read only on the I/O thread.
    POSIX::QUICTimestamp clock;
    bool isHandshakeDone = false; // set in the handshake_completed callback
    // Whether the peer cert has been checked against pinnedCertSHA256
    // (or no pin was configured). Set synchronously inside
    // handshakeCompletedHandler — before any 1-RTT receive handler can fire in
    // the same read_pkt call, since 1-RTT keys don't exist until the handshake
    // completes (a QUIC protocol guarantee, not incidental ordering). Gates
    // onData/onDatagram delivery so an active MITM (SSL_VERIFY_NONE completes
    // TLS with any cert) can't get a spoofed Sync frame decoded before the pin
    // — the only daemon authentication — is actually checked.
    bool isPinVerified = false;
    std::atomic<bool> hasPeerAuthenticationFailed { false };
    // The application error code of the peer's CONNECTION_CLOSE, written on
    // the I/O thread before onClose; read once that callback has fired.
    std::atomic<bool> hasPeerApplicationClose { false };
    std::atomic<std::uint64_t> peerApplicationCloseCode { 0 };
    bool streamOpen = false;
    // A packet the socket refused. ngtcp2 commits a packet's number, its
    // bytes-in-flight and its loss timer before handing the datagram back, so
    // dropping one is a guaranteed loss the peer must recover from — and under
    // a burst that is every packet until the congestion window is spent. Held
    // here and sent when the socket next reports writable.
    std::vector<std::uint8_t> blockedPacket;
    // Datagrams refused for arriving from somewhere other than the daemon.
    // Counted rather than logged: what is worth knowing is that the filter
    // fires at all, and a log line per packet is what an attacker would aim for.
    std::atomic<std::uint64_t> foreignDatagrams { 0 };
    // When the connection moves to a fresh path, and whether that move
    // failed. Read and written on the I/O thread only.
    RepathPolicy repathPolicy;
    // repath() asks from the owner thread; the I/O thread takes the ask.
    std::atomic<bool> isRepathRequested { false };
    // How many times the connection moved to a fresh socket.
    std::atomic<std::uint64_t> migrations { 0 };
#if UBIQUITTY_ENABLE_TEST_HOOKS
    // A test asks for the socket to be closed under the connection.
    std::atomic<bool> isSocketCloseRequested { false };
#endif
    bool connectFired = false;
    bool closeReported = false;
    bool areInitialKeysInstalled = false; // gate client_initial to one install (single retry-AEAD)

    // Connection statistics, refreshed by the I/O thread, read by stats() from
    // the owner thread. Guarded by statsMutex (a struct copy, so cheap).
    mutable std::mutex statsMutex;
    TransportStats stats;

    ngtcp2_path path()
    {
        ngtcp2_path p;
        p.local.addr = reinterpret_cast<sockaddr*>(&localAddr);
        p.local.addrlen = localLength;
        p.remote.addr = reinterpret_cast<sockaddr*>(&remoteAddr);
        p.remote.addrlen = remoteLength;
        return p;
    }

    /// Reads what a send the socket refused leaves the pump to do, and
    /// tells the repath policy when the refusal says the path is gone.
    ///
    /// - Parameters:
    ///   - errorCode: The send's `errno`.
    ///   - packet: The packet refused, kept when the socket will take it
    ///     later.
    /// - Returns: False when the connection ends here: a socket error
    ///   before the handshake, when there is no connection yet to move.
    bool afterRefusedSend(int errorCode, std::span<const std::uint8_t> packet)
    {
        switch (datagramSendErrorAction(errorCode)) {
        case DatagramSendErrorAction::Hold:
            blockedPacket.assign(packet.begin(), packet.end());
            return true;
        case DatagramSendErrorAction::Drop:
            // The route is gone for this packet and the next: loss recovery
            // resends, on the fresh path the policy now asks for.
            repathPolicy.noteSocketError();
            return true;
        case DatagramSendErrorAction::Close:
            // The socket itself failed. An established connection is moved
            // to a fresh one before it is given up.
            if (!isHandshakeDone)
                return false;
            repathPolicy.noteSocketError();
            return true;
        }
        return false;
    }

    /// Opens a UDP socket for the daemon's address family, bound to a port
    /// of its own.
    ///
    /// - Parameters:
    ///   - family: The address family.
    ///   - local: Receives the address the socket bound.
    ///   - localAddressLength: Receives its length.
    /// - Returns: The socket, or an empty descriptor.
    static PAL::UnixFileDescriptor openSocket(int family, sockaddr_storage& local, socklen_t& localAddressLength)
    {
        PAL::UnixFileDescriptor socket(::socket(family, SOCK_DGRAM, 0));
        if (!socket)
            return socket;
        // The client is the bulk RECEIVER (snapshots/keyframes stream down to it):
        // a large receive buffer absorbs a keyframe burst without kernel drops;
        // a moderate send buffer is plenty for keystrokes/paste. OS clamps to its max.
        const int receiveBufferBytes = 4 * 1024 * 1024;
        const int sendBufferBytes = 1 * 1024 * 1024;
        ::setsockopt(socket.get(), SOL_SOCKET, SO_RCVBUF, &receiveBufferBytes, sizeof(receiveBufferBytes));
        ::setsockopt(socket.get(), SOL_SOCKET, SO_SNDBUF, &sendBufferBytes, sizeof(sendBufferBytes));
        // Bind an ephemeral local port so getsockname gives the path's local addr.
        sockaddr_storage any {};
        any.ss_family = static_cast<sa_family_t>(family);
        const socklen_t anyLength = (family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        ::bind(socket.get(), reinterpret_cast<sockaddr*>(&any), anyLength);
        localAddressLength = sizeof(local);
        ::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&local), &localAddressLength);
        setNonBlocking(socket.get());
        return socket;
    }

    /// Moves the connection to a fresh socket: a new local address, a new
    /// connection ID, the same daemon. Packets not yet answered are resent
    /// there by loss recovery.
    ///
    /// - Returns: False when no socket could be opened or the connection has
    ///   no spare connection ID to move under.
    bool moveToFreshPath()
    {
        sockaddr_storage freshLocal {};
        socklen_t freshLength = 0;
        PAL::UnixFileDescriptor fresh = openSocket(remoteAddr.ss_family, freshLocal, freshLength);
        if (!fresh)
            return false;
        ngtcp2_path fresher;
        fresher.local.addr = reinterpret_cast<sockaddr*>(&freshLocal);
        fresher.local.addrlen = freshLength;
        fresher.remote.addr = reinterpret_cast<sockaddr*>(&remoteAddr);
        fresher.remote.addrlen = remoteLength;
        fresher.user_data = nullptr;
        if (ngtcp2_conn_initiate_immediate_migration(connection.get(), &fresher, clock.now()) != 0)
            return false;
        udp = std::move(fresh);
        localAddr = freshLocal;
        localLength = freshLength;
        // What the old socket refused is the old path's; loss recovery
        // resends its contents on this one.
        blockedPacket.clear();
        migrations.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void wake()
    {
        if (wakePipe.write) {
            std::uint8_t b = 1;
            ssize_t r = ::write(wakePipe.write.get(), &b, 1);
            (void)r; // a full pipe already means "wake pending"
        }
    }

    void reportClose(const std::string& reason, Health health)
    {
        if (closeReported || stopRequested.load())
            return;
        closeReported = true;
        if (callbacks.onClose)
            callbacks.onClose(health, reason);
    }

    // The backlog bound: send()'s outbound guard alone doesn't close the OOM,
    // because ioLoop moves all of outbound onto toSend every tick, so a stalled
    // daemon grows toSend without re-tripping that guard. Call this right after the
    // outbound→toSend move so the retained bytes are the total backlog.
    // Returns true (having already reported the close) when over budget.
    bool checkOutboundBudgetOverflowed()
    {
        if (toSend.retainedBytes() > QUICClient::maxOutboundBytes) {
            reportClose("the connection stalled with too much unsent data", Health::Failed);
            return true;
        }
        return false;
    }
};

#if UBIQUITTY_ENABLE_TEST_HOOKS
// OutboundCapacityAccess (declared in QUICClient.h): reads the .cpp-hidden
// outbound queue so quic_outbound_capacity can assert the real send() limit.
std::size_t OutboundCapacityAccess::queuedBytes(const QUICClient& client) { return client.m_impl->outbound.size(); }
bool OutboundCapacityAccess::didOverflow(const QUICClient& client) { return client.m_impl->hasOutboundOverflowed; }
void OutboundCapacityAccess::stageBacklogBytes(QUICClient& client, std::size_t backlogBytes)
{
    client.m_impl->toSend = {};
    const std::vector<std::uint8_t> staged(backlogBytes, 0xAB);
    client.m_impl->toSend.append(staged);
}
bool OutboundCapacityAccess::checkBudget(QUICClient& client) { return client.m_impl->checkOutboundBudgetOverflowed(); }
void OutboundCapacityAccess::installOnCloseForTest(QUICClient& client, QUICClient::Callbacks::CloseHandler onClose) { client.m_impl->callbacks.onClose = std::move(onClose); }
void OutboundCapacityAccess::seedToSend(QUICClient& client, std::span<const std::uint8_t> bytes) { client.m_impl->toSend.append(bytes); }
void OutboundCapacityAccess::stageConsumedBacklog(QUICClient& client, std::size_t size, std::size_t consumed)
{
    stageBacklogBytes(client, size);
    client.m_impl->toSend.markWritten(consumed);
}
std::vector<std::uint8_t> OutboundCapacityAccess::toSendWindow(const QUICClient& client, std::size_t maxBytes)
{
    std::vector<std::uint8_t> window;
    std::array<std::span<const std::uint8_t>, 2> spans;
    const std::size_t count = client.m_impl->toSend.unsentSpans(spans);
    for (std::size_t index = 0; index < count && window.size() < maxBytes; ++index) {
        const std::size_t take = std::min(spans[index].size(), maxBytes - window.size());
        window.insert(window.end(), spans[index].begin(), spans[index].begin() + static_cast<std::ptrdiff_t>(take));
    }
    return window;
}
const std::uint8_t* OutboundCapacityAccess::unsentFrontAddress(const QUICClient& client)
{
    std::array<std::span<const std::uint8_t>, 1> spans;
    return client.m_impl->toSend.unsentSpans(spans) ? spans[0].data() : nullptr;
}
std::size_t OutboundCapacityAccess::pendingBacklogBytes(const QUICClient& client) { return client.m_impl->toSend.unsentBytes(); }
void OutboundCapacityAccess::advanceToSend(QUICClient& client, std::size_t written) { client.m_impl->toSend.markWritten(written); }
void OutboundCapacityAccess::ackThrough(QUICClient& client, std::uint64_t ackedThrough) { client.m_impl->toSend.acknowledge(ackedThrough); }
std::size_t OutboundCapacityAccess::retainedBacklogBytes(const QUICClient& client) { return client.m_impl->toSend.retainedBytes(); }
std::size_t OutboundCapacityAccess::storageBytes(const QUICClient& client) { return client.m_impl->toSend.storageBytes(); }
std::size_t OutboundCapacityAccess::blockBytes() { return POSIX::QUICSendBuffer::blockBytes; }
void QUICClockAccess::setSource(QUICClient& client, std::function<std::uint64_t()> source) { client.m_impl->clock.setSourceForTesting(std::move(source)); }
#endif // UBIQUITTY_ENABLE_TEST_HOOKS

// ---- ngtcp2 callbacks (recover Impl from connection user_data) --------------

namespace {

ngtcp2_conn* getConnectionFromRef(ngtcp2_crypto_conn_ref* ref) { return static_cast<QUICClient::Impl*>(ref->user_data)->connection.get(); }

// ngtcp2_crypto_client_initial_cb re-derives the Initial keys and re-installs the
// fixed v1 retry AEAD (RFC 9001 §5.8 — a version-derived constant, not per-conn)
// on every call. A PTO retransmit of the client Initial re-enters this handler
// before the server replies; the AWS-LC/boringssl crypto backend hands back a
// non-null aead native_handle each time, so the second ngtcp2_conn_set_retry_aead
// asserts and leaks the first ctx. The keys + retry AEAD installed on the first
// call stay valid for the retransmit, so install exactly once.
int clientInitialOnceHandler(ngtcp2_conn* connection, void* userData)
{
    auto* d = static_cast<QUICClient::Impl*>(userData);
    if (d->areInitialKeysInstalled) {
        // Keys + the fixed v1 retry AEAD are already installed; re-deriving (the full
        // callback's first act) would double-install the retry AEAD — the boringssl
        // assert + a leaked aead_ctx. Skip the re-derive, but still drive the Initial
        // crypto so a PTO retransmit of the ClientHello proceeds; suppressing it would
        // stall the handshake under Initial packet loss.
        return ngtcp2_crypto_read_write_crypto_data(connection, NGTCP2_ENCRYPTION_LEVEL_INITIAL, nullptr, 0);
    }
    // First call: the full setup (derive + install keys/retry-AEAD + transport
    // params + drive the ClientHello). Flag before, so any re-entrant call takes the
    // narrow path above.
    d->areInitialKeysInstalled = true;
    return ngtcp2_crypto_client_initial_cb(connection, userData);
}
// Set when a draw fails inside a callback that cannot report it. The I/O loop
// reads it and closes the connection rather than continuing on bytes that are
// not random.
std::atomic<bool> s_secureRandomFailed { false };

void randHandler(std::uint8_t* dest, std::size_t length, const ngtcp2_rand_ctx*)
{
    if (fillRandom({ dest, length }))
        return;
    // ngtcp2's rand callback returns void, so there is no way to tell it the
    // draw failed. Zeroing is not a substitute for entropy and the connection
    // must not proceed on it: the bytes are cleared so nothing downstream reads
    // a stale stack, and the connection is failed at the next opportunity by
    // the caller that can — a transport that cannot make a connection id is one
    // connection ending, not a reason to end the process, which is what an
    // abort here did.
    std::fill_n(dest, length, static_cast<std::uint8_t>(0));
    s_secureRandomFailed.store(true, std::memory_order_release);
}
int newConnectionIdHandler(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t cidlen, void*)
{
    if (!fillRandom({ cid->data, cidlen }))
        return -1;
    cid->datalen = cidlen;
    if (!fillRandom(token->data))
        return -1;
    return 0;
}
int handshakeCompletedHandler(ngtcp2_conn* connection, void* user)
{
    auto* impl = static_cast<QUICClient::Impl*>(user);
    impl->isHandshakeDone = true;
    if (const ngtcp2_transport_params* parameters = connection ? ngtcp2_conn_get_remote_transport_params(connection) : nullptr)
        impl->peerDatagramMaxBytes.store(static_cast<std::size_t>(parameters->max_datagram_frame_size), std::memory_order_release);
    // Verify the pin the instant the handshake completes, synchronously
    // inside this callback — before readDatagrams()/ioLoop ever gets a chance
    // to check it, and before any 1-RTT receive handler in this same read_pkt
    // call can fire (1-RTT decrypt keys don't exist until this point). Empty
    // pin (no pin configured) reads as verified, matching the existing
    // "empty pin skips the check" semantics elsewhere.
    impl->isPinVerified = impl->pinnedCertSHA256.empty() || peerCertMatchesPin(impl->tls.get(), impl->pinnedCertSHA256);
    impl->hasPeerAuthenticationFailed.store(!impl->isPinVerified, std::memory_order_release);
    return 0;
}
// ngtcp2 acknowledges stream bytes here, and only here may they be released:
// writev_stream requires them kept intact until this reports them acknowledged,
// because a lost packet is rebuilt from the same buffer.
int ackedStreamDataHandler(ngtcp2_conn*, std::int64_t, std::uint64_t offset, std::uint64_t datalen, void* user, void*)
{
    auto* impl = static_cast<QUICClient::Impl*>(user);
    if (impl)
        impl->toSend.acknowledge(offset + datalen);
    return 0;
}

// The delivery gate on its own, apart from the connection plumbing, so the
// pin seam drives it without a live ngtcp2_conn: what the seam guards is which
// bytes reach the Sync layer, and that decision needs only the impl.
void deliverGatedStreamData(QUICClient::Impl* impl, std::span<const std::uint8_t> bytes)
{
    // Never decode a Sync frame from a peer whose cert hasn't been
    // pin-verified. A mismatch is torn down shortly after by ioLoop's existing
    // post-readDatagrams check — this just ensures nothing leaks to the Sync
    // layer in the meantime.
    if (impl->isPinVerified && impl->callbacks.onData && !bytes.empty())
        impl->callbacks.onData(bytes);
}

int receiveStreamDataHandler(ngtcp2_conn* connection, std::uint32_t, std::int64_t streamID, std::uint64_t, const std::uint8_t* data, std::size_t datalen, void* user, void*)
{
    // On the I/O thread, inside read_pkt — safe to deliver (the callback
    // must not re-enter ngtcp2 writes; send() only enqueues + wakes).
    deliverGatedStreamData(static_cast<QUICClient::Impl*>(user), { data, datalen });
    // The bytes are consumed on return — delivered above, or dropped on the way
    // to a teardown the pin check has already arranged — so the flow-control
    // credit comes back here. Initial windows are all the peer ever holds
    // without this: a transfer stalls for good at the first window edge, and
    // the sender's queue wedges behind it.
    // The pin-gate seam drives this handler with no connection behind it, and
    // credit on a connection that does not exist is owed to nobody.
    if (connection && datalen > 0) {
        ngtcp2_conn_extend_max_stream_offset(connection, streamID, datalen);
        ngtcp2_conn_extend_max_offset(connection, datalen);
    }
    return 0;
}
int receiveDatagramHandler(ngtcp2_conn*, std::uint32_t, const std::uint8_t* data, std::size_t datalen, void* user)
{
    // An unreliable DATAGRAM carrying one Sync envelope (a small Update, a Probe or an Echo).
    // Deliver it separately from stream bytes so it never disturbs the stream's
    // partial-frame reassembly. Same I/O-thread reentrancy contract as recvStreamData.
    auto* impl = static_cast<QUICClient::Impl*>(user);
    if (!impl->isPinVerified) // same pin gate as receiveStreamDataHandler
        return 0;
    if (impl->callbacks.onDatagram && datalen > 0)
        impl->callbacks.onDatagram({ data, datalen });
    return 0;
}

} // namespace

#if UBIQUITTY_ENABLE_TEST_HOOKS
// PinGateAccess (declared in QUICClient.h): drives the real ngtcp2
// handshake/receive callbacks directly (visible here, same TU), reproducing
// the coalesced-packet MITM race deterministically without a live
// connection.
void PinGateAccess::installDataCallbacksForTest(QUICClient& client, QUICClient::Callbacks::DataCallback onData, QUICClient::Callbacks::DatagramCallback onDatagram)
{
    client.m_impl->callbacks.onData = std::move(onData);
    client.m_impl->callbacks.onDatagram = std::move(onDatagram);
}

std::uint64_t LocalEndpointAccess::foreignDatagramsDropped(const QUICClient& client)
{
    return client.m_impl->foreignDatagrams.load(std::memory_order_relaxed);
}

std::uint64_t LocalEndpointAccess::migrations(const QUICClient& client)
{
    return client.m_impl->migrations.load(std::memory_order_relaxed);
}

void LocalEndpointAccess::closeSocketForTest(QUICClient& client)
{
    client.m_impl->isSocketCloseRequested.store(true);
    client.m_impl->wake();
}

std::uint16_t LocalEndpointAccess::localPort(const QUICClient& client)
{
    const QUICClient::Impl& d = *client.m_impl;
    if (d.localAddr.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in&>(d.localAddr).sin_port);
    if (d.localAddr.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6&>(d.localAddr).sin6_port);
    return 0;
}

void PinGateAccess::setPinnedCertSHA256ForTest(QUICClient& client, const std::string& pin) { client.m_impl->pinnedCertSHA256 = pin; }
bool PinGateAccess::isPinVerified(const QUICClient& client) { return client.m_impl->isPinVerified; }
void PinGateAccess::simulateHandshakeCompleted(QUICClient& client) { handshakeCompletedHandler(nullptr, client.m_impl.get()); }
// Through the gate itself, not the ngtcp2-facing wrapper: the wrapper's
// remainder is flow-control credit against a live connection, and this
// seam runs with none.
void PinGateAccess::simulateReceiveStreamData(QUICClient& client, std::span<const std::uint8_t> bytes) { deliverGatedStreamData(client.m_impl.get(), bytes); }
void PinGateAccess::simulateReceiveDatagram(QUICClient& client, std::span<const std::uint8_t> bytes) { receiveDatagramHandler(nullptr, 0, bytes.data(), bytes.size(), client.m_impl.get()); }
#endif // UBIQUITTY_ENABLE_TEST_HOOKS

// ---- QUICClient ------------------------------------------------------

QUICClient::QUICClient()
    : m_impl(std::make_unique<Impl>())
{
    // Pin the owner thread now, while this is the only thread touching the client.
    m_impl->ownerThreadID = std::this_thread::get_id();
}

QUICClient::~QUICClient() { stop(); }

bool QUICClient::peerAuthenticationFailed() const { return m_impl->hasPeerAuthenticationFailed.load(std::memory_order_acquire); }

std::optional<std::uint64_t> QUICClient::peerApplicationCloseCode() const
{
    if (!m_impl->hasPeerApplicationClose.load(std::memory_order_acquire))
        return std::nullopt;
    return m_impl->peerApplicationCloseCode.load(std::memory_order_relaxed);
}

std::string QUICClient::description() const { return m_impl->description; }

std::expected<void, QUICClient::StartError> QUICClient::start(const std::string& host, std::uint16_t port, QUICCredential credential, const std::string& pinnedCertSHA256, Callbacks callbacks)
{
    Impl& d = *m_impl;
    // Thread-affinity guard: start/stop (the lifecycle pair) must run on the
    // frontend's single owner thread (pinned at construction). A violation means a
    // caller drove connection setup/teardown off that thread — a data race the
    // send() lock does not cover, since it guards only the outbound queue. stop()
    // additionally must not be the I/O thread or the jthread join deadlocks. send()
    // is exempt by design (see its body). Debug-only; asserts compile out in release.
    assert(std::this_thread::get_id() == d.ownerThreadID);
    if (d.thread.joinable())
        return std::unexpected(StartError::AlreadyStarted);
    d.hasPeerAuthenticationFailed.store(false, std::memory_order_release);
    // A bearer token over an unpinned carrier is MITM-stealable: without the
    // cert pin we can't authenticate the daemon before handing it the token.
    // The macOS path already refuses this upstream; enforce it here too so no
    // future caller can reintroduce the hole. (Pin is the raw 32-byte SHA-256
    // of the peer cert DER — see peerCertMatchesPin.)
    const BootstrapToken* token = std::get_if<BootstrapToken>(&credential);
    const bool isPresentingNothing = token && token->token.empty();
    if (!isPresentingNothing && pinnedCertSHA256.size() != SHA256_DIGEST_LENGTH) {
        d.hasPeerAuthenticationFailed.store(true, std::memory_order_release);
        return std::unexpected(StartError::MissingCertificatePin);
    }
    // The relay preamble is a single length byte followed by the token, so a
    // longer one cannot be expressed. Sending the stream without it leaves the
    // daemon reading the first byte of HELLO as a length and failing
    // authentication for a reason that names neither this token nor its size.
    if (token && token->token.size() > maxBootstrapTokenBytes)
        return std::unexpected(StartError::TokenTooLong);

    d.callbacks = std::move(callbacks);
    d.host = host;
    d.port = port;
    d.credential = std::move(credential);
    d.pinnedCertSHA256 = pinnedCertSHA256;
    d.description = "quic " + host + ":" + std::to_string(port);

    // The wake pipe stays here, ahead of the thread: send() runs on the owner
    // thread and writes it, so a pipe built on the I/O thread would be a
    // descriptor the two threads race over — and a send arriving before it
    // existed would sit in the queue until the next poll timeout.
    PAL::UnixPipe wakePipe = PAL::makeUnixPipe();
    if (!wakePipe)
        return std::unexpected(StartError::WakePipe);
    d.wakePipe.read = std::move(wakePipe.read);
    d.wakePipe.write = std::move(wakePipe.write);
    setNonBlocking(d.wakePipe.read.get());
    setNonBlocking(d.wakePipe.write.get());

    // Resolution and everything that depends on it now run on the I/O thread.
    // getaddrinfo blocks for as long as the resolver takes, and this call is on
    // the frontend's owner thread — the thread drawing the window — so a slow
    // or unreachable resolver froze the UI for its whole timeout on every
    // session open. The thread this spawns exists precisely to keep that
    // thread free.
    d.thread = std::jthread([this] { ioLoop(); });
    return {};
}

void QUICClient::send(std::span<const std::uint8_t> bytes)
{
    Impl& d = *m_impl;
    // No owner-thread assert: send() is thread-safe (outboundMutex + wake pipe) so
    // an I/O-thread callback (e.g. onConnect enqueuing HELLO) and the UI thread can
    // both call it — the one method the contract lets cross threads.
    if (bytes.empty() || d.stopRequested.load())
        return;
    {
        std::scoped_lock lock(d.outboundMutex);
        // Bound the queue so a stalled daemon can't OOM the frontend. Dropping bytes
        // would corrupt the framed stream, so flag an overflow the I/O thread turns into onClose(Failed).
        if (outboundWouldOverflow(d.outbound.size(), bytes.size()))
            d.hasOutboundOverflowed = true;
        else
            d.outbound.insert(d.outbound.end(), bytes.begin(), bytes.end());
    }
    d.wake();
}

bool QUICClient::sendDatagram(std::span<const std::uint8_t> datagram)
{
    Impl& d = *m_impl;
    // A DATAGRAM frame spends a type byte and a length varint of the peer's
    // limit on itself.
    constexpr std::size_t frameOverheadBytes = 1 + 8;
    const std::size_t maxPeerBytes = d.peerDatagramMaxBytes.load(std::memory_order_acquire);
    if (datagram.empty() || d.stopRequested.load() || maxPeerBytes <= frameOverheadBytes || datagram.size() > maxPeerBytes - frameOverheadBytes)
        return false;
    {
        std::scoped_lock lock(d.outboundMutex);
        if (d.outboundDatagrams.size() >= maxQueuedDatagrams)
            return false;
        d.outboundDatagrams.emplace_back(datagram.begin(), datagram.end());
    }
    d.wake();
    return true;
}

void QUICClient::repath()
{
    m_impl->isRepathRequested.store(true);
    m_impl->wake();
}

void QUICClient::stop() noexcept
{
    Impl& d = *m_impl;
    // Owner-thread guard, but only while a live I/O thread exists to join: the
    // real teardown (join) must run on the owner thread and never from an I/O
    // callback (self-join deadlock). Once the thread is joined a re-entrant stop()
    // is a no-op and may run on any thread — QUICChannel holds the client by
    // shared_ptr, so the last ref (hence ~QUICClient -> stop()) can drop on a
    // send() thread rather than the owner. See start().
    assert(!d.thread.joinable() || std::this_thread::get_id() == d.ownerThreadID);
    d.stopRequested.store(true);
    d.wake();
    if (d.thread.joinable())
        d.thread.join();
    // The I/O thread is joined, so nothing touches these anymore. Release in
    // dependency order — the ngtcp2_conn before its SSL before the SSL_CTX, then
    // the socket. stop() drops the socket without a CONNECTION_CLOSE; the daemon
    // reaps the connection on its idle timeout. (A later ~Impl re-runs these dtors
    // on already-empty owners — harmless.) The wakePipe is not reset
    // here: a send() racing this stop() may still call wake() through it after the
    // stopRequested check, so the pipe fds must stay valid — the shared_ptr keeps
    // this instance (and its Impl) alive until every send() ref drops, and ~Impl
    // then frees the pipe. Resetting it here would let a concurrent wake() write a
    // closed-and-recycled descriptor.
    d.connection.reset();
    d.tls.reset();
    d.tlsContext.reset();
    d.udp.reset();
}

// ---- the I/O thread --------------------------------------------------

// Read every queued datagram into the connection. Returns false on a fatal
// connection error (caller closes), classifying it via closeHealth.
namespace {

/// Answers whether two socket addresses name the same endpoint.
///
/// Compared field by field rather than with a memcmp: `sockaddr_storage` has
/// padding that carries no meaning, and two addresses that are equal can differ
/// in it.
///
/// - Parameters:
///   - left: One address.
///   - right: The other.
/// - Returns: true when family, address and port all match.
[[nodiscard]] bool sameEndpoint(const sockaddr_storage& left, const sockaddr_storage& right)
{
    if (left.ss_family != right.ss_family)
        return false;
    if (left.ss_family == AF_INET) {
        const auto& leftInet = reinterpret_cast<const sockaddr_in&>(left);
        const auto& rightInet = reinterpret_cast<const sockaddr_in&>(right);
        return leftInet.sin_port == rightInet.sin_port
            && leftInet.sin_addr.s_addr == rightInet.sin_addr.s_addr;
    }
    if (left.ss_family == AF_INET6) {
        const auto& leftInet6 = reinterpret_cast<const sockaddr_in6&>(left);
        const auto& rightInet6 = reinterpret_cast<const sockaddr_in6&>(right);
        return leftInet6.sin6_port == rightInet6.sin6_port
            && std::memcmp(&leftInet6.sin6_addr, &rightInet6.sin6_addr, sizeof(leftInet6.sin6_addr)) == 0;
    }
    return false; // a family this transport never uses is not this peer
}

} // namespace

bool QUICClient::readDatagrams(Health& closeHealth)
{
    Impl& d = *m_impl;
    // Several datagrams per syscall where the platform offers it. What comes
    // back is the same either way, so the loop below is one loop.
    POSIX::DatagramBatch batch;
    for (;;) {
        const int received = POSIX::fillDatagramBatch(d.udp.get(), batch);
        if (received < 0)
            return true; // EAGAIN / drained
        for (int slot = 0; slot < received; ++slot) {
            const sockaddr_storage& from = batch.from[slot];
            const std::uint8_t* buffer = batch.storage[slot].data();
            const std::size_t n = batch.length[slot];
            // Anything not from the daemon is dropped before it reaches header
            // parsing or a decryption attempt. This port is reachable by any local
            // process and discoverable by scanning, and it carries a long-lived
            // session: without this, a stranger's datagrams are work this thread
            // does on their behalf, and a spoofed one is indistinguishable from the
            // daemon's. A QUIC v1 server does not migrate, so the address resolved
            // at start is the only one that can be speaking.
            if (!sameEndpoint(from, d.remoteAddr)) {
                d.foreignDatagrams.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            ngtcp2_path path = d.path();
            ngtcp2_pkt_info pi {};
            int rc = ngtcp2_conn_read_pkt(d.connection.get(), &path, &pi, buffer, n, d.clock.now());
            if (rc != 0) {
                // NGTCP2_ERR_DRAINING is the peer's CONNECTION_CLOSE — a clean,
                // peer-initiated close; every other code (TLS alert, protocol
                // violation, our-side closing) is a failure teardown.
                closeHealth = rc == NGTCP2_ERR_DRAINING ? Health::Closed : Health::Failed;
                // An application close says why the peer ended the connection,
                // ahead of anything its stream still held.
                if (rc == NGTCP2_ERR_DRAINING) {
                    const ngtcp2_ccerr* error = ngtcp2_conn_get_ccerr(d.connection.get());
                    if (error && error->type == NGTCP2_CCERR_TYPE_APPLICATION) {
                        d.peerApplicationCloseCode.store(error->error_code, std::memory_order_relaxed);
                        d.hasPeerApplicationClose.store(true, std::memory_order_release);
                    }
                }
                return false;
            }
            // The daemon answered on the path in use.
            d.repathPolicy.noteRoundTrip();
        }
    }
}

// Pump pending protocol packets + relay-stream payload to the socket.
// Returns false on a fatal write error.
bool QUICClient::writePump()
{
    Impl& d = *m_impl;
    if (!d.blockedPacket.empty()) {
        const ssize_t sent = ::sendto(d.udp.get(), d.blockedPacket.data(), d.blockedPacket.size(), 0, reinterpret_cast<sockaddr*>(&d.remoteAddr), d.remoteLength);
        if (sent < 0) {
            if (datagramSendErrorAction(errno) == DatagramSendErrorAction::Hold)
                return true; // still blocked; nothing new may go ahead of it
            const std::vector<std::uint8_t> refused = std::exchange(d.blockedPacket, {});
            if (!d.afterRefusedSend(errno, refused))
                return false;
        }
        d.blockedPacket.clear();
    }
    std::uint8_t buffer[1452];
    // Datagrams first, each in its own packet, as the daemon writes them: one
    // the connection cannot carry is dropped, one the congestion window holds
    // waits for the next pass. None goes before the pin is verified.
    while (d.isPinVerified && !d.toSendDatagrams.empty()) {
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi {};
        int accepted = 0;
        std::vector<std::uint8_t>& datagram = d.toSendDatagrams.front();
        ngtcp2_vec datav { datagram.data(), datagram.size() };
        const ngtcp2_ssize nwrite = ngtcp2_conn_writev_datagram(d.connection.get(), &ps.path, &pi, buffer, sizeof(buffer), &accepted, NGTCP2_WRITE_DATAGRAM_FLAG_NONE, 0, &datav, 1, d.clock.now());
        if (nwrite < 0) {
            d.toSendDatagrams.pop_front();
            continue;
        }
        if (nwrite == 0)
            break;
        if (accepted)
            d.toSendDatagrams.pop_front();
        const ssize_t sent = ::sendto(d.udp.get(), buffer, static_cast<std::size_t>(nwrite), 0, reinterpret_cast<sockaddr*>(&d.remoteAddr), d.remoteLength);
        if (sent < 0)
            return d.afterRefusedSend(errno, { buffer, static_cast<std::size_t>(nwrite) });
        if (!accepted)
            break;
    }
    // Flow control blocked the stream for the rest of this pump. The peer's
    // MAX_STREAM_DATA lifts it; the next pump after that read offers the stream
    // again.
    bool isStreamBlocked = false;
    for (;;) {
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi {};
        ngtcp2_ssize wdatalen = 0;
        std::int64_t streamID = -1;
        std::array<ngtcp2_vec, 2> datav {};
        std::size_t datavcnt = 0;
        if (d.streamOpen && !isStreamBlocked && d.toSend.hasUnsent()) {
            std::array<std::span<const std::uint8_t>, datav.size()> spans;
            datavcnt = d.toSend.unsentSpans(spans);
            for (std::size_t index = 0; index < datavcnt; ++index)
                datav[index] = { const_cast<std::uint8_t*>(spans[index].data()), spans[index].size() };
            streamID = d.streamID;
        }
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(d.connection.get(), &ps.path, &pi, buffer, sizeof(buffer), &wdatalen, NGTCP2_WRITE_STREAM_FLAG_NONE, streamID, datavcnt ? datav.data() : nullptr, datavcnt, d.clock.now());
        if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
            // The peer's stream window is full: ordinary flow control, not a
            // failure. The pump carries on without the stream, so ACKs and
            // timer-driven packets still go out while the window is closed.
            isStreamBlocked = true;
            continue;
        }
        if (nwrite < 0)
            return false;
        // The cursor only: the bytes stay until the acknowledgement releases them.
        if (wdatalen > 0)
            d.toSend.markWritten(static_cast<std::size_t>(wdatalen));
        if (nwrite == 0)
            return true; // nothing more to send
        const ssize_t sent = ::sendto(d.udp.get(), buffer, static_cast<std::size_t>(nwrite), 0, reinterpret_cast<sockaddr*>(&d.remoteAddr), d.remoteLength);
        if (sent >= 0)
            continue;
        // A packet the socket will take later is held, and nothing is built
        // ahead of it. One whose route or socket is gone is dropped: loss
        // recovery resends it, on a fresh path once the policy moves there.
        // Either way the pump stops building until the next pass.
        return d.afterRefusedSend(errno, { buffer, static_cast<std::size_t>(nwrite) });
    }
}

// Resolve, open the socket and build the connection — all of it on the I/O
// thread, because the first step blocks for as long as the resolver takes and
// the owner thread is the one drawing the window.
//
// Every failure here is reported as a close rather than returned: by the time
// this runs, start() has already told its caller the attempt is under way.
//
// - Returns: false when the connection could not be established.
bool QUICClient::establishConnection()
{
    Impl& d = *m_impl;
    // Resolve the daemon address (numeric or hostname; IPv4/IPv6).
    char portText[16];
    std::snprintf(portText, sizeof(portText), "%u", d.port);
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (::getaddrinfo(d.host.c_str(), portText, &hints, &result) != 0 || !result) {
        d.reportClose("could not resolve the host", Health::Failed);
        return false;
    }
    // Every candidate, not just the first. A dual-stack name whose AAAA sorts
    // ahead of its A fails outright on a network where IPv6 does not work, and
    // the fallback the resolver's ordering is meant to enable only happens if
    // something tries the next entry.
    for (const addrinfo* candidate = result; candidate; candidate = candidate->ai_next) {
        PAL::UnixFileDescriptor attempt = Impl::openSocket(candidate->ai_family, d.localAddr, d.localLength);
        if (!attempt)
            continue;
        std::memcpy(&d.remoteAddr, candidate->ai_addr, candidate->ai_addrlen);
        d.remoteLength = candidate->ai_addrlen;
        d.udp = std::move(attempt);
        break;
    }
    ::freeaddrinfo(result);
    if (!d.udp) {
        d.reportClose("could not create the UDP socket", Health::Failed);
        return false;
    }

    // AWS-LC client context (self-signed daemon cert → no CA verification; the
    // auth is the bootstrap token in the Sync HELLO above us, and the cert is
    // pinned by SHA-256 in peerCertMatchesPin).
    d.tlsContext.reset(SSL_CTX_new(TLS_method()));
    if (!d.tlsContext || ngtcp2_crypto_boringssl_configure_client_context(d.tlsContext.get()) != 0) {
        d.reportClose("could not set up TLS", Health::Failed);
        return false;
    }
    SSL_CTX_set_min_proto_version(d.tlsContext.get(), TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(d.tlsContext.get(), TLS1_3_VERSION);
    SSL_CTX_set_verify(d.tlsContext.get(), SSL_VERIFY_NONE, nullptr);
    d.tls.reset(SSL_new(d.tlsContext.get()));
    if (!d.tls) {
        d.reportClose("could not set up TLS", Health::Failed);
        return false;
    }
    // TLS_method() is role-agnostic, so the client role must be set explicitly —
    // without it the TLS handshake never starts (client_initial fails). Set it
    // before any crypto is driven.
    SSL_set_connect_state(d.tls.get());
    SSL_set_alpn_protos(d.tls.get(), reinterpret_cast<const unsigned char*>(quicALPN), sizeof(quicALPN) - 1);
    d.connectionRef.get_conn = getConnectionFromRef;
    d.connectionRef.user_data = &d;
    SSL_set_app_data(d.tls.get(), &d.connectionRef);

    ngtcp2_cid dcid, scid;
    dcid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
    if (!fillRandom({ dcid.data, dcid.datalen })) {
        d.reportClose("could not draw a connection id", Health::Failed);
        return false;
    }
    scid.datalen = clientConnectionIDLength;
    if (!fillRandom({ scid.data, scid.datalen })) {
        d.reportClose("could not draw a connection id", Health::Failed);
        return false;
    }

    ngtcp2_callbacks cb {};
    cb.client_initial = clientInitialOnceHandler;
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
    cb.handshake_completed = handshakeCompletedHandler;
    cb.recv_stream_data = receiveStreamDataHandler;
    cb.acked_stream_data_offset = ackedStreamDataHandler;
    cb.recv_datagram = receiveDatagramHandler;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = d.clock.now();
    // A filtered UDP path must release the caller to its SSH fallback promptly.
    settings.handshake_timeout = 5 * NGTCP2_SECONDS;
    ngtcp2_transport_params parameters;
    ngtcp2_transport_params_default(&parameters);
    parameters.initial_max_streams_bidi = 4;
    parameters.initial_max_streams_uni = 4;
    // Receive windows govern host->client delivery (the relay stream the host
    // writes the screen plane on). The host writes a screen in frames of at
    // most 64 KiB, a checkpoint as a run of them, so a window holds many
    // frames and a checkpoint of a few mebibytes lands in one flow-control
    // window instead of stalling for a MAX_STREAM_DATA round-trip.
    parameters.initial_max_data = 8 * 1024 * 1024;
    parameters.initial_max_stream_data_bidi_local = 4 * 1024 * 1024;
    parameters.initial_max_stream_data_bidi_remote = 4 * 1024 * 1024;
    // Spare connection IDs for roaming (one per path) + a mosh-tolerant idle
    // window, matching the host; the keepalive below holds the path open inside it.
    parameters.active_connection_id_limit = 8;
    parameters.max_idle_timeout = 90 * NGTCP2_SECONDS;
    // Accept unreliable DATAGRAM frames (RFC 9221): lets the host ship small
    // Updates, Probes and Echoes as datagrams. On-wire size is bounded by path MTU; 65535 = accept any.
    parameters.max_datagram_frame_size = 65535;

    ngtcp2_path path = d.path();
    ngtcp2_conn* rawConnection = nullptr;
    if (ngtcp2_conn_client_new(&rawConnection, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &cb, &settings, &parameters, nullptr, &d) != 0) {
        d.reportClose("could not create the QUIC connection", Health::Failed);
        return false;
    }
    d.connection.reset(rawConnection);
    ngtcp2_conn_set_tls_native_handle(d.connection.get(), d.tls.get());
    // Keepalive PING after 15 s idle: holds NAT/firewall UDP bindings open through
    // quiet periods so a roam/rebind doesn't drop inside the 90 s idle window.
    ngtcp2_conn_set_keep_alive_timeout(d.connection.get(), 15 * NGTCP2_SECONDS);

    return true;
}

void QUICClient::ioLoop()
{
    Impl& d = *m_impl;
    // The I/O thread and its callbacks run OFF the owner thread by construction;
    // assert it so a refactor that runs the loop inline trips here. See start().
    assert(std::this_thread::get_id() != d.ownerThreadID);
    if (!establishConnection())
        return; // establishConnection reported why
    writePump(); // client Initial

    while (!d.stopRequested.load()) {
        // Wake on a readable socket, the self-pipe (send/stop), or the
        // connection's next loss/idle timer expiry.
        int timeout = -1;
        std::uint64_t expiry = ngtcp2_conn_get_expiry(d.connection.get());
        if (expiry != UINT64_MAX) {
            std::uint64_t now = d.clock.now();
            timeout = (expiry <= now) ? 0 : static_cast<int>((expiry - now) / 1000000ull);
            if (timeout > 1000)
                timeout = 1000; // cap so idle timers still fire promptly
        }
        struct pollfd pfds[2];
        pfds[0] = { d.udp.get(), static_cast<short>(POLLIN | (d.blockedPacket.empty() ? 0 : POLLOUT)), 0 };
        pfds[1] = { d.wakePipe.read.get(), POLLIN, 0 };
        ::poll(pfds, 2, timeout);
        if (d.stopRequested.load())
            break;

        if (pfds[1].revents & POLLIN) {
            std::uint8_t drain[256];
            while (::read(d.wakePipe.read.get(), drain, sizeof(drain)) > 0) {
            }
        }

        // Drive timers (retransmit a lost Initial, idle close, …).
        std::uint64_t now = d.clock.now();
        if (ngtcp2_conn_get_expiry(d.connection.get()) <= now) {
            if (ngtcp2_conn_handle_expiry(d.connection.get(), now) != 0) {
                d.reportClose("connection timed out", Health::Failed);
                return;
            }
        }

        if (pfds[0].revents & (POLLIN | POLLERR)) {
            Health closeHealth = Health::Failed;
            if (!readDatagrams(closeHealth)) {
                d.reportClose(closeHealth == Health::Closed ? "connection closed by peer" : "connection read error", closeHealth);
                return;
            }
        }

        // Once the handshake completes, open the single relay stream and
        // announce the carrier is ready.
        if (d.isHandshakeDone && !d.streamOpen) {
            // Pin the server cert before opening the relay stream / sending
            // the token — a mismatch means this isn't the real daemon.
            if (!d.pinnedCertSHA256.empty() && !peerCertMatchesPin(d.tls.get(), d.pinnedCertSHA256)) {
                d.reportClose("server certificate pin mismatch", Health::Failed);
                return;
            }
            if (ngtcp2_conn_open_bidi_stream(d.connection.get(), &d.streamID, &d) == 0)
                d.streamOpen = true;
            if (d.streamOpen) {
                // Lead the stream with the one-time-token preamble
                // [u8 length][token] so it precedes the frontend's HELLO (which
                // only arrives via send() after onConnect fires below).
                // start() refuses a longer token, so this is a bound the
                // preamble states rather than a condition that can fail here.
                if (const BootstrapToken* token = std::get_if<BootstrapToken>(&d.credential)) {
                    if (!token->token.empty() && token->token.size() <= maxBootstrapTokenBytes) {
                        const std::array<std::uint8_t, 2> head { bootstrapTokenPreambleKind, static_cast<std::uint8_t>(token->token.size()) };
                        d.toSend.append(head);
                        d.toSend.append({ reinterpret_cast<const std::uint8_t*>(token->token.data()), token->token.size() });
                    }
                } else {
                    // Built from keying material only the two ends of this
                    // handshake can export, so what it proves it proves here
                    // and on no other connection.
                    const ExportedPreamble& exported = std::get<ExportedPreamble>(d.credential);
                    std::array<std::uint8_t, 32> exporter {};
                    const bool didExport = SSL_export_keying_material(d.tls.get(), exporter.data(), exporter.size(), exported.exporterLabel.data(), exported.exporterLabel.size(), nullptr, 0, 0) == 1;
                    const std::vector<std::uint8_t> preamble = didExport && exported.build ? exported.build(exporter) : std::vector<std::uint8_t> {};
                    if (preamble.empty()) {
                        d.reportClose("the handshake exported no keying material, or none a preamble could be built from", Health::Failed);
                        return;
                    }
                    d.toSend.append(preamble);
                }
                if (!d.connectFired) {
                    d.connectFired = true;
                    if (d.callbacks.onConnect)
                        d.callbacks.onConnect();
                }
            }
        }

        // Move queued outbound bytes onto the relay stream.
        bool overflowed = false;
        {
            std::scoped_lock lock(d.outboundMutex);
            overflowed = d.hasOutboundOverflowed;
            if (!d.outbound.empty()) {
                d.toSend.append(d.outbound);
                d.outbound.clear();
            }
            while (!d.outboundDatagrams.empty() && d.toSendDatagrams.size() < maxQueuedDatagrams) {
                d.toSendDatagrams.push_back(std::move(d.outboundDatagrams.front()));
                d.outboundDatagrams.pop_front();
            }
        }
        if (overflowed) {
            d.reportClose("the connection stalled with too much unsent data", Health::Failed);
            return;
        }
        if (d.checkOutboundBudgetOverflowed())
            return;

        if (!writePump()) {
            d.reportClose("connection write error", Health::Failed);
            return;
        }

        // Refresh the connection-stats snapshot for the Sync Debug window once
        // the handshake has produced RTT samples. Cheap (a struct copy under a
        // short-held lock); the owner thread reads it via stats().
        if (d.isHandshakeDone && d.connection) {
            ngtcp2_conn_info info;
            ngtcp2_conn_get_conn_info(d.connection.get(), &info);
            std::scoped_lock lock(d.statsMutex);
            d.stats.valid = true;
            d.stats.smoothedRttMicros = static_cast<std::int64_t>(info.smoothed_rtt / 1000);
            d.stats.latestRttMicros = static_cast<std::int64_t>(info.latest_rtt / 1000);
            d.stats.minRttMicros = static_cast<std::int64_t>(info.min_rtt / 1000);
            d.stats.rttVarMicros = static_cast<std::int64_t>(info.rttvar / 1000);
            d.stats.congestionWindow = info.cwnd;
            d.stats.bytesInFlight = info.bytes_in_flight;
            d.stats.slowStartThreshold = info.ssthresh;
            d.stats.packetsSent = info.pkt_sent;
            d.stats.packetsReceived = info.pkt_recv;
            d.stats.packetsLost = info.pkt_lost;
            d.stats.bytesSent = info.bytes_sent;
            d.stats.bytesReceived = info.bytes_recv;
            d.stats.bytesLost = info.bytes_lost;
            d.stats.migrations = d.migrations.load(std::memory_order_relaxed);
        }

        // The path. A connection whose packets leave and are not answered is
        // moved to a fresh socket before it is called lost, and lost only
        // when that path fails too (Core/Transport/RepathPolicy.h).
        if (d.isHandshakeDone && d.connection) {
#if UBIQUITTY_ENABLE_TEST_HOOKS
            if (d.isSocketCloseRequested.exchange(false))
                d.udp.reset();
#endif
            if (d.isRepathRequested.exchange(false))
                d.repathPolicy.notePathRemoved();
            ngtcp2_conn_info info;
            ngtcp2_conn_get_conn_info(d.connection.get(), &info);
            const std::uint64_t nowMicroseconds = d.clock.now() / 1000;
            if (info.bytes_in_flight > 0)
                d.repathPolicy.noteOutstanding(nowMicroseconds);
            else
                d.repathPolicy.noteNothingOutstanding();
            if (d.repathPolicy.due(nowMicroseconds)) {
                if (d.moveToFreshPath())
                    d.repathPolicy.noteRepathed(nowMicroseconds);
                else
                    d.repathPolicy.noteRepathImpossible();
            }
            if (d.repathPolicy.hasFailed(nowMicroseconds)) {
                d.reportClose("the network path was lost and a fresh one did not reach the host", Health::Failed);
                return;
            }
        }
    }
}

bool QUICClient::stats(TransportStats& out) const
{
    std::scoped_lock lock(m_impl->statsMutex);
    if (!m_impl->stats.valid)
        return false;
    out = m_impl->stats;
    return true;
}

} // namespace Ubiquitty::Transport
