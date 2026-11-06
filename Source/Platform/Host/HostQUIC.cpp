// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// See HostQUIC.h. Compiled only under -DUBIQUITTY_QUIC=ON. Stands up the
// listener's connection setup: a self-signed cert (generated once under
// ~/.ubiquitty), the shared AWS-LC server TLS context, and a non-blocking UDP
// socket the daemon's poll() loop watches. Incoming datagrams are received and
// their QUIC long-header version/connection-id decoded.

#include "Platform/Host/HostQUIC.h"

#include "Platform/POSIX/DatagramBatch.h"
#include "Platform/POSIX/QUICSendBuffer.h"
#include "Platform/POSIX/QUICTimestamp.h"

#include "Platform/Host/HostPaths.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_boringssl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <string_view>
namespace Ubiquitty {
namespace Host {

namespace {

// Writes `contents` to `path` through an owner-only temporary renamed into
// place, so a reader never meets a half-written key and the key is never
// readable by anyone else, not even for the moment before a chmod.
bool writePrivateFile(const std::string& path, std::string_view contents)
{
    std::string temporary = path + ".XXXXXX";
    PAL::UnixFileDescriptor file(::mkstemp(temporary.data()));
    if (!file)
        return false;
    bool didWrite = ::fchmod(file.get(), 0600) == 0;
    while (didWrite && !contents.empty()) {
        const auto written = ::write(file.get(), contents.data(), contents.size());
        if (written < 0 && errno == EINTR)
            continue;
        didWrite = written > 0;
        if (didWrite)
            contents.remove_prefix(static_cast<std::size_t>(written));
    }
    didWrite = didWrite && ::fsync(file.get()) == 0;
    file.reset();
    if (didWrite && ::rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    ::unlink(temporary.c_str());
    return false;
}

// The PEM text `write` puts into a memory BIO, or empty when it fails.
template<typename Writer>
std::string pemText(const Writer& write)
{
    bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
    if (!bio || !write(bio.get()))
        return {};
    const std::uint8_t* contents = nullptr;
    std::size_t length = 0;
    if (!BIO_mem_contents(bio.get(), &contents, &length) || !length)
        return {};
    return std::string(reinterpret_cast<const char*>(contents), length);
}

// Makes the daemon's self-signed QUIC certificate and its RSA-2048 key with
// the TLS library this binary links, never a program found on PATH: a
// directory early on PATH would otherwise choose the key the daemon serves.
// RSA (not EC) so the AWS-LC server context loads it without extra ECC
// options. The SSH→QUIC model authenticates with the ssh-established one-time
// token, and the dialer pins this certificate's digest, so self-signed is
// enough. The key is written before the certificate, whose presence is what
// marks the pair complete.
bool generateSelfSignedCertificate(const std::string& certificatePath, const std::string& keyPath)
{
    constexpr long validitySeconds = 3650L * 24 * 60 * 60;
    bssl::UniquePtr<EVP_PKEY_CTX> context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
    EVP_PKEY* generated = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 || EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) != 1
        || EVP_PKEY_keygen(context.get(), &generated) != 1)
        return false;
    bssl::UniquePtr<EVP_PKEY> key(generated);

    // A positive 127-bit random serial, as RFC 5280 asks of a CA's serials.
    std::array<std::uint8_t, 16> serialBytes {};
    if (RAND_bytes(serialBytes.data(), serialBytes.size()) != 1)
        return false;
    serialBytes[0] &= 0x7f;
    bssl::UniquePtr<BIGNUM> serial(BN_bin2bn(serialBytes.data(), serialBytes.size(), nullptr));
    bssl::UniquePtr<X509> certificate(X509_new());
    if (!serial || !certificate || X509_set_version(certificate.get(), X509_VERSION_3) != 1
        || !BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate.get()))
        || !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0)
        || !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), validitySeconds)
        || X509_set_pubkey(certificate.get(), key.get()) != 1)
        return false;
    X509_NAME* name = X509_get_subject_name(certificate.get());
    constexpr std::string_view commonName = "ubiquitty-host";
    if (!name
        || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const std::uint8_t*>(commonName.data()), static_cast<ossl_ssize_t>(commonName.size()), -1, 0) != 1
        || X509_set_issuer_name(certificate.get(), name) != 1 || X509_sign(certificate.get(), key.get(), EVP_sha256()) == 0)
        return false;

    const std::string keyText = pemText([&](BIO* bio) { return PEM_write_bio_PrivateKey(bio, key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1; });
    const std::string certificateText = pemText([&](BIO* bio) { return PEM_write_bio_X509(bio, certificate.get()) == 1; });
    return !keyText.empty() && !certificateText.empty() && writePrivateFile(keyPath, keyText) && writePrivateFile(certificatePath, certificateText);
}

std::string secureControlDirectory()
{
    char directory[1024];
    if (!ensureControlDirectory() || controlDirectory(directory) == 0)
        return {};
    return directory;
}

// ALPN protocol id shared with the client transport ("mtq"), length-prefixed.
constexpr char quicALPN[] = "\x03mtq";

// Length of the source connection ids we mint. Must be passed to
// ngtcp2_pkt_decode_version_cid as short_dcidlen so post-handshake short-
// header (1-RTT) packets route by their dcid (which is our scid).
constexpr std::size_t serverConnectionIDLength = 8;
// Hard cap on concurrent (incl. pre-auth) QUIC connections, so a remote
// peer can't force unbounded handshake/TLS/ngtcp2 state allocation. Reachable
// only via a validated Retry token, so this bounds legitimate
// concurrent handshakes, not a spoofed flood.
constexpr std::size_t maxQUICConnections = 64;
// How long a handshake-complete peer may stay unauthenticated. The authed idle
// window is 90 s so a phone in a pocket keeps its session; a peer that has not
// yet said who it is has no session to keep. Generous against a slow link — the
// preamble is one small write the client sends immediately after the handshake.
constexpr std::uint64_t preAuthGraceMilliseconds = 10000;

// How long a Retry token stays valid after we mint it — the client's real
// round-trip to receive the Retry and resend its Initial with the token, never
// more than a few RTTs even on a slow path. Bounds a captured/replayed token's
// window without being tight enough to false-reject a real, if sluggish, client.
constexpr ngtcp2_duration retryTokenTimeoutNanoseconds = 10 * NGTCP2_SECONDS;

std::uint64_t monotonicNanoseconds() { return POSIX::quicClockNanoseconds(); }

bool fillRandom(std::span<std::uint8_t> dest)
{
    PAL::UnixFileDescriptor fileDescriptor(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    if (!fileDescriptor)
        return false;

    std::size_t filled = 0;
    while (filled < dest.size()) {
        ssize_t n = ::read(fileDescriptor.get(), dest.data() + filled, dest.size() - filled);
        if (n > 0) {
            filled += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

void quicRandHandler(std::uint8_t* dest, std::size_t destlen, const ngtcp2_rand_ctx*)
{
    if (!fillRandom({ dest, destlen }))
        std::abort();
}

int quicGetNewConnectionIdHandler(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t cidlen, void*)
{
    if (!fillRandom({ cid->data, cidlen }))
        return -1;
    cid->datalen = cidlen;
    if (!fillRandom(token->data))
        return -1;
    return 0;
}

int quicServerAlpnSelectHandler(SSL*, const unsigned char** outputBuffer, unsigned char* outlen, const unsigned char*, unsigned, void*)
{
    *outputBuffer = reinterpret_cast<const unsigned char*>(quicALPN) + 1; // skip length byte
    *outlen = static_cast<unsigned char>(quicALPN[0]);
    return 0; // SSL_TLSEXT_ERR_OK
}

} // namespace

std::string quicStackVersion()
{
    const ngtcp2_info* info = ngtcp2_version(0);
    const char* version = (info != nullptr && info->version_str != nullptr) ? info->version_str : "unknown";
    return std::string("ngtcp2 ") + version;
}

// One accepted QUIC connection: its ngtcp2 state machine + AWS-LC session +
// the peer it speaks to. Owned by the listener, found by our source cid.
struct QUICConnection {
    ngtcp2_conn* connection = nullptr;
    SSL* tls = nullptr;
    ngtcp2_crypto_conn_ref connectionRef {}; // TLS app-data → ngtcp2_conn back-ref
    ngtcp2_cid scid {}; // our source cid (connection-table key)
    // The dcid the client uses until it learns `scid` from our first Initial:
    // the SCID we minted for its Retry (every accepted connection came through
    // Retry). A retransmitted or coalesced tokened Initial still
    // carries it, and must route here — treating it as unknown re-verifies the
    // stateless token and mints a duplicate server connection that fights this
    // one over the same client (RFC 9000 §7.2 routing; ngtcp2's server example
    // associates this cid for the same reason).
    ngtcp2_cid retryPhaseDestinationConnectionID {};
    // The spare connection IDs ngtcp2 issued the client for this connection
    // and has not retired. A client that moves to a fresh path addresses the
    // daemon by one of these (RFC 9000 §9.5), so a packet under any of them
    // routes here; under none of them it is no connection's. ngtcp2 issues at
    // most the peer's active_connection_id_limit and retires as it replaces,
    // so the list stays that short.
    std::vector<ngtcp2_cid> spareConnectionIDs;
    sockaddr_storage peer {};
    socklen_t peerLength = 0;
    bool handshakeDone = false;
    // Every timestamp this connection's ngtcp2 calls receive, from every call
    // site: ngtcp2 aborts the process on one lower than the last.
    POSIX::QUICTimestamp clock;
    // When the handshake finished, and whether the peer has since presented a
    // valid token. Two sweeps already bound a QUIC peer and neither reaches this
    // one: the daemon reaps a client slot that never HELLOs, but a peer holding
    // ngtcp2's reassembly buffer has delivered nothing, so it owns no slot; and
    // ngtcp2's own expiry fires at the idle timeout, which is 90 s because an
    // AUTHENTICATED client is allowed to survive a tunnel outage. An
    // unauthenticated one has no such claim.
    std::uint64_t handshakeDoneMilliseconds = 0;
    bool authenticated = false;

    QUICListener* owner = nullptr; // back-ref for the stream-data callback
    std::int64_t streamID = -1; // the client's relay bidi stream (captured on first recv)
    // Outbound Sync bytes that ngtcp2 may still need. ngtcp2 does not copy stream
    // data: it rebuilds a lost packet from the pointers it was given, so every
    // byte stays at its address until the acknowledgement releases it. See
    // QUICSendBuffer.
    POSIX::QUICSendBuffer toSend;
    // Pending outbound DATAGRAM frames — each one Sync envelope (a small Update,
    // a Probe or an Echo), sent unreliably so a lost/reordered one never
    // head-of-line-blocks the others. A datagram that can't be sent now (congestion)
    // stays queued, bounded by quicDatagramQueueHighWaterBytes.
    std::deque<std::vector<std::uint8_t>> toSendDatagrams;
    // What toSendDatagrams holds. Carried rather than summed, because the bound
    // is asked on every push and a deque cannot answer it in constant time.
    std::size_t queuedDatagramBytes = 0;

    /// Drops the datagram at the head of the queue.
    ///
    /// The one place the queue shrinks, so the byte count cannot be left behind
    /// by a drop site that forgot it.
    void popFrontDatagram()
    {
        if (toSendDatagrams.empty())
            return;
        queuedDatagramBytes -= toSendDatagrams.front().size();
        toSendDatagrams.pop_front();
    }

    ~QUICConnection()
    {
        if (connection)
            ngtcp2_conn_del(connection);
        if (tls)
            SSL_free(tls);
    }
};
// One rule, so the sweep and the test that pins it cannot drift apart.
[[nodiscard]] bool preAuthDeadlinePassed(const QUICConnection& connection, std::uint64_t nowMilliseconds)
{
    return connection.handshakeDone && !connection.authenticated
        && nowMilliseconds - connection.handshakeDoneMilliseconds > preAuthGraceMilliseconds;
}

namespace {

ngtcp2_conn* quicGetConnectionFromRef(ngtcp2_crypto_conn_ref* ref) { return static_cast<QUICConnection*>(ref->user_data)->connection; }

int quicHandshakeCompletedHandler(ngtcp2_conn*, void* user)
{
    auto* handshaken = static_cast<QUICConnection*>(user);
    handshaken->handshakeDone = true;
    handshaken->handshakeDoneMilliseconds = monotonicNanoseconds() / 1000000u;
    return 0;
}

// The most spare connection IDs a connection is routed by. ngtcp2 keeps to
// the peer's active_connection_id_limit; this bounds the list whatever a
// peer makes it issue.
constexpr std::size_t maxSpareConnectionIDs = 32;

// Issues a spare connection ID and routes it to its connection.
int quicIssueConnectionIDHandler(ngtcp2_conn* connection, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t cidlen, void* user)
{
    if (const int result = quicGetNewConnectionIdHandler(connection, cid, token, cidlen, user); result != 0)
        return result;
    auto* c = static_cast<QUICConnection*>(user);
    if (c->spareConnectionIDs.size() >= maxSpareConnectionIDs)
        c->spareConnectionIDs.erase(c->spareConnectionIDs.begin());
    c->spareConnectionIDs.push_back(*cid);
    return 0;
}

// Stops routing a connection ID the client retired.
int quicRetireConnectionIDHandler(ngtcp2_conn*, const ngtcp2_cid* cid, void* user)
{
    auto* c = static_cast<QUICConnection*>(user);
    std::erase_if(c->spareConnectionIDs, [&](const ngtcp2_cid& spare) { return ngtcp2_cid_eq(&spare, cid) != 0; });
    return 0;
}

int quicReceiveStreamDataHandler(ngtcp2_conn* connection, std::uint32_t, std::int64_t streamID, std::uint64_t, const std::uint8_t* data, std::size_t datalen, void* user, void*)
{
    auto* c = static_cast<QUICConnection*>(user);
    if (c->streamID < 0)
        c->streamID = streamID; // the client's relay stream
    if (c->owner && datalen > 0)
        c->owner->deliverStreamData(c, { data, datalen });
    // Delivery is synchronous — the daemon decodes and handles the frames
    // inside the callback above, and what it drops it drops for keeps — so the
    // window credit comes back before this returns. Without it the initial
    // window is all a client ever gets, and an upload stalls for good at its
    // edge while the pre-auth reaper sees a connection that owes no bytes.
    if (datalen > 0) {
        ngtcp2_conn_extend_max_stream_offset(connection, streamID, datalen);
        ngtcp2_conn_extend_max_offset(connection, datalen);
    }
    return 0;
}

int quicReceiveDatagramHandler(ngtcp2_conn*, std::uint32_t, const std::uint8_t* data, std::size_t datalen, void* user)
{
    auto* c = static_cast<QUICConnection*>(user);
    if (c->owner && datalen > 0)
        c->owner->deliverDatagram(c, { data, datalen });
    return 0;
}

// ngtcp2 acknowledges stream bytes here, and only here may they be released.
// Ranges arrive in increasing order without overlap, per the callback's contract,
// so the acked prefix is always at the buffer's front.
int quicAckedStreamDataHandler(ngtcp2_conn*, std::int64_t streamID, std::uint64_t offset, std::uint64_t datalen, void* user, void*)
{
    auto* c = static_cast<QUICConnection*>(user);
    if (!c || streamID != c->streamID)
        return 0;
    c->toSend.acknowledge(offset + datalen);
    return 0;
}

// Stateless address validation (RFC 9000 §8.1): a fresh Initial with no Retry
// token gets a Retry packet carrying a token bound to the packet's CLAIMED
// source address — sent to that same address, never to wherever the datagram
// actually originated. A spoofed source address never receives it and so can
// never resend an Initial that echoes the token, so it can never pass the
// verify step in processReadable() that gates connection allocation
// Allocates nothing: no ngtcp2_conn, no TLS context, no
// QUICConnection. A real client's genuine round-trip is the only path to a
// valid token.
void sendRetry(int fileDescriptor, const ngtcp2_pkt_hd& clientHeader, std::uint32_t version, const sockaddr* peer, socklen_t peerLength, std::span<const std::uint8_t> secret)
{
    ngtcp2_cid retryServerConnectionID;
    retryServerConnectionID.datalen = serverConnectionIDLength;
    if (!fillRandom({ retryServerConnectionID.data, retryServerConnectionID.datalen }))
        return;
    std::uint8_t token[NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN];
    const ngtcp2_ssize tokenLength = ngtcp2_crypto_generate_retry_token(token, secret.data(), secret.size(), version, peer, peerLength, &retryServerConnectionID, &clientHeader.dcid, monotonicNanoseconds());
    if (tokenLength < 0)
        return;
    std::uint8_t packet[256];
    const ngtcp2_ssize packetLength = ngtcp2_crypto_write_retry(packet, sizeof(packet), version, &clientHeader.scid, &retryServerConnectionID, &clientHeader.dcid, token, static_cast<std::size_t>(tokenLength));
    if (packetLength < 0)
        return;
    // Best-effort: a dropped Retry just costs the client one more RTT to retry
    // its Initial, no state was ever allocated regardless.
    (void)::sendto(fileDescriptor, packet, static_cast<std::size_t>(packetLength), 0, peer, peerLength);
}

// Create + initialize a server connection from a fresh client initial.
std::unique_ptr<QUICConnection> acceptServerConnection(
    QUICListener* listener, SSL_CTX* ctx, const ngtcp2_version_cid& vc, const sockaddr* peer, socklen_t peerLength, const sockaddr* local, socklen_t localLength, const ngtcp2_cid& originalDestinationConnectionID, const ngtcp2_cid& retryServerConnectionID, const POSIX::QUICTimestamp::Source& clockSource)
{
    auto c = std::make_unique<QUICConnection>();
    c->owner = listener;
    c->clock.setSourceForTesting(clockSource);
    std::memcpy(&c->peer, peer, peerLength);
    c->peerLength = peerLength;

    c->tls = SSL_new(ctx);
    if (!c->tls)
        return nullptr;
    // TLS_method() is role-agnostic, so mark this the server side before the
    // handshake is driven.
    SSL_set_accept_state(c->tls);
    c->connectionRef.get_conn = quicGetConnectionFromRef;
    c->connectionRef.user_data = c.get();
    SSL_set_app_data(c->tls, &c->connectionRef);

    // Defense in depth: ngtcp2_pkt_decode_version_cid only yields a cid longer than
    // NGTCP2_MAX_CIDLEN via version negotiation, never on this accept path — but these
    // memcpy pre-auth network-controlled lengths into the fixed-size ngtcp2_cid.data,
    // so clamp explicitly against a patched/older/misbehaving ngtcp2.
    if (vc.scidlen > NGTCP2_MAX_CIDLEN || vc.dcidlen > NGTCP2_MAX_CIDLEN)
        return nullptr;
    ngtcp2_cid dcid; // client's scid (our send target on this, the post-Retry Initial)
    std::memcpy(dcid.data, vc.scid, vc.scidlen);
    dcid.datalen = vc.scidlen;
    c->scid.datalen = serverConnectionIDLength;
    if (!fillRandom({ c->scid.data, c->scid.datalen }))
        return nullptr;
    c->retryPhaseDestinationConnectionID = retryServerConnectionID;

    ngtcp2_callbacks cb {};
    cb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    cb.encrypt = ngtcp2_crypto_encrypt_cb;
    cb.decrypt = ngtcp2_crypto_decrypt_cb;
    cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
    cb.update_key = ngtcp2_crypto_update_key_cb;
    cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    cb.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    cb.rand = quicRandHandler;
    cb.get_new_connection_id2 = quicIssueConnectionIDHandler;
    cb.remove_connection_id = quicRetireConnectionIDHandler;
    cb.handshake_completed = quicHandshakeCompletedHandler;
    cb.recv_stream_data = quicReceiveStreamDataHandler;
    cb.acked_stream_data_offset = quicAckedStreamDataHandler;
    cb.recv_datagram = quicReceiveDatagramHandler;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = c->clock.now();
    // Bound pre-auth resource: an unauthenticated peer must complete the
    // handshake quickly, and an idle connection self-closes — so a remote
    // peer can't pin handshake/TLS/ngtcp2 state without authenticating.
    settings.handshake_timeout = 5 * NGTCP2_SECONDS;
    ngtcp2_transport_params parameters;
    ngtcp2_transport_params_default(&parameters);
    parameters.initial_max_streams_bidi = 4;
    parameters.initial_max_streams_uni = 4;
    // Receive windows for client->host (keystrokes/paste — small) plus headroom
    // for a large bracketed paste; kept symmetric with the client's windows.
    parameters.initial_max_stream_data_bidi_local = 4 * 1024 * 1024;
    parameters.initial_max_stream_data_bidi_remote = 4 * 1024 * 1024;
    parameters.initial_max_data = 8 * 1024 * 1024;
    // Migration needs spare connection IDs (one per path); the default (2) is
    // thin for a client that roams wifi<->cellular repeatedly.
    parameters.active_connection_id_limit = 8;
    // Mosh-tolerant idle window: survive a phone-in-pocket / tunnel outage long
    // enough that the client's 15 s keepalive (QUICClient) holds the path open;
    // a genuinely longer outage falls back to the reconnect+reattach path.
    parameters.max_idle_timeout = 90 * NGTCP2_SECONDS;
    // The client answers a datagram probe with a datagram, so the host takes
    // the size it sends.
    parameters.max_datagram_frame_size = quicDatagramMaxBytes;
    // original_dcid is the dcid from the client's FIRST (pre-Retry) Initial —
    // extracted from the verified Retry token, never re-read off this (second)
    // packet's header. retry_scid is our scid from the Retry packet we sent; the
    // client's transport-parameter validation (RFC 9000 §7.3) uses both to prove
    // this connection actually completed the Retry round-trip and wasn't spliced
    // in by an on-path attacker after the fact.
    parameters.original_dcid = originalDestinationConnectionID;
    parameters.original_dcid_present = 1;
    parameters.retry_scid = retryServerConnectionID;
    parameters.retry_scid_present = 1;

    ngtcp2_path path;
    path.local.addr = const_cast<sockaddr*>(local);
    path.local.addrlen = localLength;
    path.remote.addr = reinterpret_cast<sockaddr*>(&c->peer);
    path.remote.addrlen = c->peerLength;

    if (ngtcp2_conn_server_new(&c->connection, &dcid, &c->scid, &path, vc.version, &cb, &settings, &parameters, nullptr, c.get()) != 0)
        return nullptr;
    ngtcp2_conn_set_tls_native_handle(c->connection, c->tls);
    return c;
}

// Flush a connection's pending packets (handshake responses, ACKs) to its
// peer, its queued datagrams and its relay stream's bytes. Returns the packets
// it handed the socket.
std::size_t quicWritePump(QUICConnection& c, int fileDescriptor, const sockaddr* local, socklen_t localLength)
{
    std::uint8_t buffer[1452];
    std::size_t packets = 0;

    // The host is the bulk SENDER: snapshots and keyframes stream down to a
    // client, and at ~1350 bytes a packet a mebibyte is about 775 of them. One
    // syscall each is 775 crossings on the thread that also encrypts them.
    //
    // Each packet carries its own destination rather than a cached peer,
    // because ngtcp2 chooses the path per packet: during migration a
    // PATH_CHALLENGE goes to the new path while data still flows on the old
    // one, and a batch that assumed one peer would send the challenge to the
    // wrong address. Every exit from this function flushes -- a queued packet
    // nobody sent is a packet the peer waits for until loss recovery notices.
    POSIX::DatagramSendBatch sendBatch;
    const auto send = [&](std::size_t packetLength, const sockaddr* destination, socklen_t destinationLength) {
        // A refusal is unreachable from here: the batch is flushed the moment it
        // fills, and `buffer` is smaller than a slot. Returning rather than
        // asserting keeps a packet that somehow did not fit out of the stream
        // instead of sending the previous one twice.
        if (!POSIX::queueDatagram(sendBatch, std::span<const std::uint8_t>(buffer, packetLength), destination, destinationLength))
            return;
        ++packets;
        if (sendBatch.isFull())
            (void)POSIX::flushDatagramBatch(fileDescriptor, sendBatch);
    };

    // ngtcp2 fills `path` (out) with the network path each packet must go to.
    // During migration/path-validation a PATH_CHALLENGE targets the NEW path
    // while data may still flow on the old one, so we send to the path ngtcp2
    // chose — never a single cached peer. A packet left open by a datagram
    // written with FLAG_MORE is finished by the next call with the same path,
    // packet info and timestamp.
    sockaddr_storage remoteStore {};
    ngtcp2_path path;
    ngtcp2_pkt_info pi {};
    ngtcp2_tstamp timestamp = c.clock.now();
    const auto beginPacket = [&] {
        remoteStore = {};
        path.local.addr = const_cast<sockaddr*>(local);
        path.local.addrlen = localLength;
        path.remote.addr = reinterpret_cast<sockaddr*>(&remoteStore);
        path.remote.addrlen = sizeof(remoteStore);
        pi = {};
        timestamp = c.clock.now();
    };
    const auto hasStreamData = [&c] { return c.streamID >= 0 && c.toSend.hasUnsent(); };

    // Pending DATAGRAM frames go first. While stream data waits, each is written
    // with FLAG_MORE, so a publish's superset update and its stream frames share
    // a packet: a keystroke echo is one packet, not two (Docs/sync.md §6.5). A
    // datagram that can't be sent now (congestion, or no room in this pass's
    // packet) stays queued for the next pump; an unsendable one (peer never
    // enabled datagrams, or it's too large) is dropped — the next superset
    // update covers it. Datagrams are unreliable, so a lost one never blocks
    // others.
    bool isPacketOpen = false;
    beginPacket();
    while (!c.toSendDatagrams.empty()) {
        int accepted = 0;
        const std::vector<std::uint8_t>& dgram = c.toSendDatagrams.front();
        ngtcp2_vec datav { const_cast<std::uint8_t*>(dgram.data()), dgram.size() };
        const std::uint32_t flags = hasStreamData() || c.toSendDatagrams.size() > 1 ? NGTCP2_WRITE_DATAGRAM_FLAG_MORE : NGTCP2_WRITE_DATAGRAM_FLAG_NONE;
        const ngtcp2_ssize nwrite = ngtcp2_conn_writev_datagram(c.connection, &path, &pi, buffer, sizeof(buffer), &accepted, flags, /*dgram_id*/ 0, &datav, 1, timestamp);
        if (nwrite == NGTCP2_ERR_WRITE_MORE) {
            // Accepted into a packet still open for more.
            c.popFrontDatagram();
            isPacketOpen = true;
            continue;
        }
        if (nwrite < 0) {
            c.popFrontDatagram(); // unsendable (no peer support / too large) — drop
            break;
        }
        if (nwrite == 0)
            break; // congestion / amplification limit — retry on the next pump
        send(static_cast<std::size_t>(nwrite), path.remote.addr, path.remote.addrlen);
        isPacketOpen = false;
        if (accepted)
            c.popFrontDatagram();
        else
            break; // the datagram didn't fit this packet — retry on the next pump
        beginPacket();
    }

    // Flow control blocked the stream for the rest of this pump; see below.
    bool isStreamBlocked = false;
    for (;;) {
        if (!isPacketOpen)
            beginPacket();
        isPacketOpen = false;
        ngtcp2_ssize wdatalen = 0;
        std::int64_t streamID = -1;
        std::array<ngtcp2_vec, 2> datav {};
        std::size_t datavcnt = 0;
        if (c.streamID >= 0 && !isStreamBlocked && c.toSend.hasUnsent()) {
            std::array<std::span<const std::uint8_t>, datav.size()> spans;
            datavcnt = c.toSend.unsentSpans(spans);
            for (std::size_t index = 0; index < datavcnt; ++index)
                datav[index] = { const_cast<std::uint8_t*>(spans[index].data()), spans[index].size() };
            streamID = c.streamID;
        }
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(c.connection, &path, &pi, buffer, sizeof(buffer), &wdatalen, NGTCP2_WRITE_STREAM_FLAG_NONE, streamID, datavcnt ? datav.data() : nullptr, datavcnt, timestamp);
        if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
            // The client's stream window is full: ordinary flow control. Carry on
            // without the stream, so ACKs and timer-driven packets still go out;
            // the client's MAX_STREAM_DATA lifts it for a later pump. A packet a
            // FLAG_MORE datagram left open is still open, so the next call
            // finishes it with the same path, packet info and timestamp.
            isStreamBlocked = true;
            isPacketOpen = true;
            continue;
        }
        if (nwrite < 0) {
            (void)POSIX::flushDatagramBatch(fileDescriptor, sendBatch);
            return packets; // error
        }
        if (wdatalen > 0)
            c.toSend.markWritten(static_cast<std::size_t>(wdatalen)); // released on the ack, not here
        if (nwrite == 0) {
            (void)POSIX::flushDatagramBatch(fileDescriptor, sendBatch);
            return packets; // nothing more to send
        }
        send(static_cast<std::size_t>(nwrite), path.remote.addr, path.remote.addrlen);
    }
}

} // namespace

QUICListener::QUICListener() = default;

QUICListener::~QUICListener() { stop(); }

bool QUICListener::ensureCertificate(std::string& certPath, std::string& keyPath)
{
    const std::string directory = secureControlDirectory();
    if (directory.empty()) {
        m_lastError = "insecure or unavailable control directory";
        return false;
    }
    certPath = directory + "/quic-cert.pem";
    keyPath = directory + "/quic-key.pem";

    struct stat st;
    if (::stat(certPath.c_str(), &st) == 0 && ::stat(keyPath.c_str(), &st) == 0)
        return true; // already generated

    if (!generateSelfSignedCertificate(certPath, keyPath)) {
        m_lastError = "failed to generate the self-signed QUIC certificate";
        return false;
    }
    return true;
}

std::string QUICListener::certificateSHA256() const
{
    if (m_certPath.empty())
        return "";
    BIO* bio = BIO_new_file(m_certPath.c_str(), "r");
    if (!bio)
        return "";
    X509* x509 = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!x509)
        return "";
    std::string hash;
    unsigned char* der = nullptr;
    const int derLength = i2d_X509(x509, &der); // allocates der; OPENSSL_free below
    if (der && derLength > 0) {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(der, static_cast<std::size_t>(derLength), digest);
        hash.assign(reinterpret_cast<const char*>(digest), SHA256_DIGEST_LENGTH);
    }
    OPENSSL_free(der);
    X509_free(x509);
    return hash;
}

bool QUICListener::buildTLSContext(const std::string& certPath, const std::string& keyPath)
{
    m_tlsContext = SSL_CTX_new(TLS_method());
    if (!m_tlsContext) {
        m_lastError = "SSL_CTX_new failed";
        return false;
    }
    SSL_CTX_set_min_proto_version(m_tlsContext, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(m_tlsContext, TLS1_3_VERSION);
    if (ngtcp2_crypto_boringssl_configure_server_context(m_tlsContext) != 0) {
        m_lastError = "ngtcp2_crypto_boringssl_configure_server_context failed";
        return false;
    }
    SSL_CTX_set_alpn_select_cb(m_tlsContext, quicServerAlpnSelectHandler, nullptr);
    if (SSL_CTX_use_certificate_chain_file(m_tlsContext, certPath.c_str()) != 1) {
        m_lastError = "SSL_CTX_use_certificate_chain_file failed";
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(m_tlsContext, keyPath.c_str(), SSL_FILETYPE_PEM) != 1) {
        m_lastError = "SSL_CTX_use_PrivateKey_file failed";
        return false;
    }
    return true;
}

bool QUICListener::start(std::uint16_t port)
{
    if (m_fileDescriptor || m_tlsContext || !m_connections.empty()) {
        m_lastError = "QUIC listener is not stopped";
        return false;
    }

    // Prefer a dual-stack AF_INET6 socket: one fd accepts IPv6 and IPv4-mapped
    // traffic, so the daemon is reachable over either family no matter which
    // one the client resolves first (MagicDNS typically returns IPv6 first).
    // Fall back to AF_INET on hosts where the kernel has IPv6 disabled, so an
    // IPv4-only client still gets QUIC there.
    PAL::UnixFileDescriptor fileDescriptor(::socket(AF_INET6, SOCK_DGRAM, 0));
    const bool isDualStack = fileDescriptor.isValid();
    if (!isDualStack)
        fileDescriptor.reset(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!fileDescriptor) {
        m_lastError = "UDP socket() failed";
        return false;
    }
    if (isDualStack) {
        // Explicit rather than platform-defaulted: a dual-stack socket must
        // accept IPv4-mapped peers even where the OS defaults V6ONLY to 1.
        int v6Only = 0;
        if (::setsockopt(fileDescriptor.get(), IPPROTO_IPV6, IPV6_V6ONLY, &v6Only, sizeof(v6Only)) != 0) {
            m_lastError = "UDP setsockopt(IPV6_V6ONLY) failed";
            return false;
        }
    }
    struct sockaddr_storage addr = {};
    socklen_t addrLength = 0;
    if (isDualStack) {
        struct sockaddr_in6* addr6 = reinterpret_cast<struct sockaddr_in6*>(&addr);
        addr6->sin6_family = AF_INET6;
        addr6->sin6_addr = in6addr_any;
        addr6->sin6_port = htons(port);
        addrLength = sizeof(sockaddr_in6);
    } else {
        struct sockaddr_in* addr4 = reinterpret_cast<struct sockaddr_in*>(&addr);
        addr4->sin_family = AF_INET;
        addr4->sin_addr.s_addr = htonl(INADDR_ANY);
        addr4->sin_port = htons(port);
        addrLength = sizeof(sockaddr_in);
    }
    if (::bind(fileDescriptor.get(), reinterpret_cast<struct sockaddr*>(&addr), addrLength) != 0) {
        m_lastError = "UDP bind() failed";
        return false;
    }
    // Bulk sender (snapshots/keyframes) + aggregate receiver across all QUIC
    // connections on this one socket: size the kernel UDP buffers so a keyframe
    // burst isn't dropped to ENOBUFS and a larger cwnd can actually be filled.
    // The OS clamps to its max; that's fine.
    const int udpBufferBytes = 4 * 1024 * 1024;
    ::setsockopt(fileDescriptor.get(), SOL_SOCKET, SO_SNDBUF, &udpBufferBytes, sizeof(udpBufferBytes));
    ::setsockopt(fileDescriptor.get(), SOL_SOCKET, SO_RCVBUF, &udpBufferBytes, sizeof(udpBufferBytes));

    return startOnBoundSocket(std::move(fileDescriptor));
}

bool QUICListener::startOnBoundSocket(PAL::UnixFileDescriptor socket)
{
    if (m_fileDescriptor || m_tlsContext || !m_connections.empty()) {
        m_lastError = "QUIC listener is not stopped";
        return false;
    }
    int type = 0;
    socklen_t typeLength = sizeof(type);
    sockaddr_storage address {};
    socklen_t addressLength = sizeof(address);
    if (!socket || ::getsockopt(socket.get(), SOL_SOCKET, SO_TYPE, &type, &typeLength) != 0 || type != SOCK_DGRAM
        || ::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
        m_lastError = "QUIC socket must be a bound UDP socket";
        return false;
    }
    std::uint16_t port = 0;
    if (address.ss_family == AF_INET && addressLength == sizeof(sockaddr_in))
        port = ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
    else if (address.ss_family == AF_INET6 && addressLength == sizeof(sockaddr_in6))
        port = ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    if (!port) {
        m_lastError = "QUIC socket must have a bound IPv4 or IPv6 port";
        return false;
    }
#ifdef SO_PROTOCOL
    int protocol = 0;
    socklen_t protocolLength = sizeof(protocol);
    if (::getsockopt(socket.get(), SOL_SOCKET, SO_PROTOCOL, &protocol, &protocolLength) != 0 || protocol != IPPROTO_UDP) {
        m_lastError = "QUIC socket must use UDP";
        return false;
    }
#endif
    sockaddr_storage peer {};
    socklen_t peerLength = sizeof(peer);
    if (::getpeername(socket.get(), reinterpret_cast<sockaddr*>(&peer), &peerLength) == 0 || errno != ENOTCONN) {
        m_lastError = "QUIC listener socket must not be connected";
        return false;
    }
    const int flags = ::fcntl(socket.get(), F_GETFL, 0);
    const int descriptorFlags = ::fcntl(socket.get(), F_GETFD, 0);
    if (flags == -1 || descriptorFlags == -1
        || ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) == -1
        || ::fcntl(socket.get(), F_SETFD, descriptorFlags | FD_CLOEXEC) == -1) {
        m_lastError = "QUIC socket flags could not be set";
        return false;
    }
    if (!fillRandom(m_retryTokenSecret)) {
        m_lastError = "secure random source unavailable";
        return false;
    }
    std::string certPath, keyPath;
    if (!ensureCertificate(certPath, keyPath) || !buildTLSContext(certPath, keyPath)) {
        stop();
        return false;
    }
    m_certPath = std::move(certPath);
    m_boundPort = port;
    m_localAddr = address;
    m_localAddrLength = addressLength;
    m_fileDescriptor = std::move(socket);
    m_lastError.clear();
    return true;
}

PAL::UnixFileDescriptor QUICListener::releaseBoundSocket()
{
    if (!m_fileDescriptor || !m_connections.empty()) {
        m_lastError = "QUIC socket release requires a listener with no connections";
        return {};
    }
    auto socket = std::move(m_fileDescriptor);
    stop();
    m_lastError.clear();
    return socket;
}

void QUICListener::stop()
{
    m_connections.clear();
    m_fileDescriptor.reset();
    if (m_tlsContext) {
        SSL_CTX_free(m_tlsContext);
        m_tlsContext = nullptr;
    }
    m_boundPort = 0;
    m_localAddr = {};
    m_localAddrLength = 0;
    m_certPath.clear();
    std::ranges::fill(m_retryTokenSecret, 0);
}

void QUICListener::processReadable()
{
    if (!m_fileDescriptor)
        return;
    const sockaddr* local = reinterpret_cast<const sockaddr*>(&m_localAddr);
    std::uint8_t buffer[65536];
    struct sockaddr_storage peer;
    for (std::size_t packetsRead = 0; packetsRead < quicReceivePacketBudget; ++packetsRead) {
        socklen_t peerLength = sizeof(peer);
        ssize_t n = ::recvfrom(m_fileDescriptor.get(), buffer, sizeof(buffer), 0, reinterpret_cast<struct sockaddr*>(&peer), &peerLength);
        if (n < 0)
            break; // EAGAIN / drained
        if (n == 0)
            continue;
        // Decode the QUIC long-header version + connection ids. Route by the
        // packet's dcid to an existing connection (our scid); a fresh client
        // initial that matches none spawns a new server connection.
        ngtcp2_version_cid versionConnectionID;
        if (ngtcp2_pkt_decode_version_cid(&versionConnectionID, buffer, static_cast<std::size_t>(n), serverConnectionIDLength) != 0)
            continue; // unsupported version / not a packet we route here

        QUICConnection* connection = nullptr;
        for (auto& candidate : m_connections) {
            const bool matchesSCID = candidate->scid.datalen == versionConnectionID.dcidlen && std::memcmp(candidate->scid.data, versionConnectionID.dcid, versionConnectionID.dcidlen) == 0;
            // Retry-phase packets (the tokened Initial and any retransmission
            // of it) address us by the Retry SCID until the client sees our
            // first Initial — route them to the accepted connection instead of
            // re-accepting a duplicate.
            const bool matchesRetryPhaseCID = candidate->retryPhaseDestinationConnectionID.datalen == versionConnectionID.dcidlen && std::memcmp(candidate->retryPhaseDestinationConnectionID.data, versionConnectionID.dcid, versionConnectionID.dcidlen) == 0;
            // A client on a fresh path addresses us by a spare ID we issued it.
            const bool matchesSpareCID = std::ranges::any_of(candidate->spareConnectionIDs, [&](const ngtcp2_cid& spare) {
                return spare.datalen == versionConnectionID.dcidlen && std::memcmp(spare.data, versionConnectionID.dcid, versionConnectionID.dcidlen) == 0;
            });
            if (matchesSCID || matchesRetryPhaseCID || matchesSpareCID) {
                connection = candidate.get();
                break;
            }
        }
        if (!connection) {
            // Stateless address validation before allocating anything:
            // a spoofed-source Initial flood can never complete the Retry round-trip
            // (the Retry goes to the claimed, not actual, source), so it never reaches
            // acceptServerConnection at all — the flood costs us one ngtcp2_accept +
            // one HMAC-token generate per spoofed packet, no ngtcp2/TLS state.
            ngtcp2_pkt_hd clientHeader;
            if (ngtcp2_accept(&clientHeader, buffer, static_cast<std::size_t>(n)) != 0)
                continue; // not a valid first packet for a new connection

            if (clientHeader.tokenlen == 0) {
                sendRetry(m_fileDescriptor.get(), clientHeader, versionConnectionID.version, reinterpret_cast<sockaddr*>(&peer), peerLength, m_retryTokenSecret);
                continue; // no state allocated — wait for the client's real round-trip
            }

            // A token is presented: verify it before allocating anything. Any
            // failure (garbage, expired, wrong client address, wrong dcid) refuses
            // silently — exactly as if no packet had arrived, no state either way.
            ngtcp2_cid originalDestinationConnectionID;
            if (ngtcp2_crypto_verify_retry_token(
                    &originalDestinationConnectionID, clientHeader.token, clientHeader.tokenlen, m_retryTokenSecret, sizeof(m_retryTokenSecret), versionConnectionID.version, reinterpret_cast<sockaddr*>(&peer), peerLength, &clientHeader.dcid, retryTokenTimeoutNanoseconds, monotonicNanoseconds())
                != 0)
                continue; // invalid/expired/address-mismatched token — refuse

            if (m_connections.size() >= maxQUICConnections)
                continue; // at the connection cap — refuse new handshakes
            auto fresh = acceptServerConnection(this, m_tlsContext, versionConnectionID, reinterpret_cast<sockaddr*>(&peer), peerLength, local, m_localAddrLength, originalDestinationConnectionID, clientHeader.dcid, m_clockSourceForTesting);
            if (!fresh)
                continue; // not an initial / accept failed
            connection = fresh.get();
            m_connections.push_back(std::move(fresh));
        }

        // Feed read_pkt the ACTUAL source of this packet (not the cached peer),
        // so ngtcp2 sees a roamed client's new address and runs path validation.
        // Connections route by DCID (above), so a roam keeps reaching us.
        ngtcp2_path path;
        path.local.addr = const_cast<sockaddr*>(local);
        path.local.addrlen = m_localAddrLength;
        path.remote.addr = reinterpret_cast<sockaddr*>(&peer);
        path.remote.addrlen = peerLength;
        ngtcp2_pkt_info pi {};
        int rv = ngtcp2_conn_read_pkt(connection->connection, &path, &pi, buffer, static_cast<std::size_t>(n), connection->clock.now());
        if (rv != 0) {
            // Fatal for this connection (incl. a completed close) — drop it,
            // notifying the daemon so it reaps the bound Client.
            if (m_connectionClosedCallback)
                m_connectionClosedCallback(connection);
            for (std::size_t i = 0; i < m_connections.size(); ++i) {
                if (m_connections[i].get() == connection) {
                    m_connections.erase(m_connections.begin() + i);
                    break;
                }
            }
            continue;
        }
        // Adopt ngtcp2's authoritative (validated) remote path, so any code
        // reading connection->peer follows a roamed client. ngtcp2 only promotes
        // `remote` after path validation, so an off-path spoof can't move it.
        if (const ngtcp2_path* validated = ngtcp2_conn_get_path2(connection->connection)) {
            if (validated->remote.addrlen > 0 && validated->remote.addrlen <= sizeof(connection->peer)) {
                std::memcpy(&connection->peer, validated->remote.addr, validated->remote.addrlen);
                connection->peerLength = validated->remote.addrlen;
            }
        }
        m_packetsSent += quicWritePump(*connection, m_fileDescriptor.get(), local, m_localAddrLength);
    }
    // Drain any remaining pending writes (handshake responses, ACKs, timers).
    for (auto& connection : m_connections)
        m_packetsSent += quicWritePump(*connection, m_fileDescriptor.get(), local, m_localAddrLength);

    // Reap connections whose handshake/idle timer fired — an unauthenticated
    // peer that never completes the handshake within the timeout is dropped,
    // bounding pre-auth state under a flood (the cap is the hard ceiling).
    const std::uint64_t reapNowMilliseconds = monotonicNanoseconds() / 1000000u;
    for (std::size_t i = m_connections.size(); i-- > 0;) {
        // A peer that finished the handshake and never authenticated. It can hold
        // ngtcp2's per-connection reassembly buffer — send stream data at an
        // offset with a gap beneath it and the bytes are buffered against the
        // flow-control window without ever reaching recv_stream_data, so the
        // daemon sees no byte, creates no client, and its no-HELLO reaper never
        // considers it. Only the 90 s idle timeout would, and that window exists
        // for a client that has proved who it is.
        if (preAuthDeadlinePassed(*m_connections[i], reapNowMilliseconds)) {
            if (m_connectionClosedCallback)
                m_connectionClosedCallback(m_connections[i].get());
            m_connections.erase(m_connections.begin() + static_cast<long>(i));
            continue;
        }
        ngtcp2_conn* connection = m_connections[i]->connection;
        const std::uint64_t reapNow = m_connections[i]->clock.now();
        if (ngtcp2_conn_get_expiry(connection) <= reapNow && ngtcp2_conn_handle_expiry(connection, reapNow) != 0) {
            if (m_connectionClosedCallback)
                m_connectionClosedCallback(m_connections[i].get());
            m_connections.erase(m_connections.begin() + static_cast<long>(i));
        }
    }
}

void QUICListener::setStreamDataCallback(std::function<void(QUICConnection*, std::span<const std::uint8_t>)> callback) { m_streamDataCallback = std::move(callback); }

void QUICListener::setConnectionClosedCallback(std::function<void(QUICConnection*)> callback) { m_connectionClosedCallback = std::move(callback); }

void QUICListener::deliverStreamData(QUICConnection* connection, std::span<const std::uint8_t> bytes)
{
    if (m_streamDataCallback)
        m_streamDataCallback(connection, bytes);
}

void QUICListener::setDatagramCallback(std::function<void(QUICConnection*, std::span<const std::uint8_t>)> callback) { m_datagramCallback = std::move(callback); }

void QUICListener::deliverDatagram(QUICConnection* connection, std::span<const std::uint8_t> bytes)
{
    if (m_datagramCallback)
        m_datagramCallback(connection, bytes);
}

void QUICListener::sendStream(QUICConnection* connection, std::span<const std::uint8_t> bytes)
{
    if (!connection || bytes.empty())
        return;
    // Queue only — ngtcp2 forbids writing from inside a read_pkt callback (the
    // common caller: the stream-data handler). The pending bytes flush in
    // processReadable's post-read pump, or via an explicit flush().
    connection->toSend.append(bytes);
}

void QUICListener::closeConnection(QUICConnection* connection, std::optional<QUICApplicationClose> close)
{
    if (!connection)
        return;
    if (close && connection->connection && connection->handshakeDone)
        sendApplicationClose(*connection, *close);
    // Daemon-initiated drop: erase the owning unique_ptr so the QUICConnection — and
    // its toSend/toSendDatagrams — frees immediately rather than at the idle timeout.
    // No m_connectionClosedCallback: the daemon already owns this drop, and firing it
    // would re-enter the Client-reap path.
    for (std::size_t i = 0; i < m_connections.size(); ++i) {
        if (m_connections[i].get() == connection) {
            m_connections.erase(m_connections.begin() + static_cast<long>(i));
            return;
        }
    }
}

void QUICListener::sendApplicationClose(QUICConnection& connection, const QUICApplicationClose& close)
{
    ngtcp2_ccerr error;
    ngtcp2_ccerr_default(&error);
    const std::string_view phrase = close.reasonPhrase.substr(0, QUICApplicationClose::maxReasonPhraseBytes);
    ngtcp2_ccerr_set_application_error(&error, close.errorCode, reinterpret_cast<const std::uint8_t*>(phrase.data()), phrase.size());
    // ngtcp2 writes the packet's path into these, so they are storage of
    // their own, never the listener's address.
    sockaddr_storage local {};
    sockaddr_storage remote {};
    ngtcp2_path path;
    path.local.addr = reinterpret_cast<sockaddr*>(&local);
    path.local.addrlen = sizeof(local);
    path.remote.addr = reinterpret_cast<sockaddr*>(&remote);
    path.remote.addrlen = sizeof(remote);
    ngtcp2_pkt_info packetInfo {};
    std::array<std::uint8_t, NGTCP2_MAX_UDP_PAYLOAD_SIZE> packet {};
    const ngtcp2_ssize length = ngtcp2_conn_write_connection_close(connection.connection, &path, &packetInfo, packet.data(), packet.size(), &error, connection.clock.now());
    // Best-effort, like every terminal packet: a lost close leaves the peer its
    // idle timeout, which is what it had without one.
    if (length > 0)
        (void)::sendto(m_fileDescriptor.get(), packet.data(), static_cast<std::size_t>(length), 0, path.remote.addr, path.remote.addrlen);
}

void QUICListener::markAuthenticated(QUICConnection* connection)
{
    if (connection)
        connection->authenticated = true;
}

std::optional<std::array<std::uint8_t, 32>> QUICListener::exportKeyingMaterial(QUICConnection* connection, std::string_view label) const
{
    if (!connection || !connection->tls)
        return std::nullopt;
    std::array<std::uint8_t, 32> material {};
    if (SSL_export_keying_material(connection->tls, material.data(), material.size(), label.data(), label.size(), nullptr, 0, 0) != 1)
        return std::nullopt;
    return material;
}

std::size_t QUICListener::pendingBytes(QUICConnection* connection) const
{
    if (!connection)
        return 0;
    // The whole retained stream buffer plus every pending datagram — the memory a
    // stalled/flow-controlled reader accumulates while the daemon keeps producing
    // output. The buffer holds sent-but-unacknowledged bytes as well, because
    // ngtcp2 retransmits from it, and those occupy memory exactly like unsent
    // ones: measuring only the unsent part would report a fraction of what is
    // held and let the high-water cap pass a connection that is not draining.
    // The datagram half is the count the queue carries rather than a walk of it:
    // one number, kept by the one place the queue grows and the one place it
    // shrinks, so the cap and this answer cannot disagree.
    return connection->toSend.retainedBytes() + connection->queuedDatagramBytes;
}

std::size_t QUICListener::unsentBytes(QUICConnection* connection) const
{
    if (!connection)
        return 0;
    return connection->toSend.unsentBytes() + connection->queuedDatagramBytes;
}

std::size_t QUICListener::storageBytes(QUICConnection* connection) const
{
    if (!connection)
        return 0;
    std::size_t bytes = connection->toSend.storageBytes();
    for (const auto& datagram : connection->toSendDatagrams)
        bytes += datagram.capacity();
    return bytes;
}

std::size_t QUICListener::additionalStreamStorageBytes(QUICConnection* connection, std::size_t bytes) const
{
    return connection ? connection->toSend.additionalStorageBytes(bytes) : 0;
}

bool QUICListener::datagramCapable(QUICConnection* connection) const
{
    // The peer (client) advertised it accepts DATAGRAM frames iff its transport
    // params carry a non-zero max_datagram_frame_size (RFC 9221). Available only
    // after the handshake exchanged params; null before then.
    if (!connection || !connection->connection || !connection->handshakeDone)
        return false;
    const ngtcp2_transport_params* params = ngtcp2_conn_get_remote_transport_params(connection->connection);
    return params && params->max_datagram_frame_size > 0;
}

void QUICListener::sendDatagram(QUICConnection* connection, std::span<const std::uint8_t> bytes)
{
    if (!connection || bytes.empty())
        return;
    // Queue one Sync envelope as an unreliable DATAGRAM; the write pump
    // drains it (ngtcp2 forbids writing from inside a read_pkt callback). Caller
    // guarantees `bytes` fits a datagram (quicDatagramMaxBytes) so it rides one
    // packet; the pump drops it only if the peer turns out not to support them.
    //
    // Room is made from the head, so a peer that stops reading loses its oldest
    // deltas rather than growing this without end. The newest frames are the ones
    // worth keeping: they describe the screen as it is, and the seq gap the drop
    // opens is what asks for the keyframe that replaces the rest.
    while (connection->queuedDatagramBytes + bytes.size() > quicDatagramQueueHighWaterBytes
        && !connection->toSendDatagrams.empty())
        connection->popFrontDatagram();
    connection->queuedDatagramBytes += bytes.size();
    connection->toSendDatagrams.emplace_back(bytes.begin(), bytes.end());
}

void QUICListener::flush()
{
    const sockaddr* local = reinterpret_cast<const sockaddr*>(&m_localAddr);
    for (auto& connection : m_connections)
        m_packetsSent += quicWritePump(*connection, m_fileDescriptor.get(), local, m_localAddrLength);
}

int QUICListener::nextTimeoutMilliseconds() const
{
    std::uint64_t soonest = UINT64_MAX;
    for (const auto& connection : m_connections) {
        const std::uint64_t expiry = ngtcp2_conn_get_expiry(connection->connection);
        if (expiry < soonest)
            soonest = expiry;
    }
    if (soonest == UINT64_MAX)
        return -1; // no connections / no armed timer
    const std::uint64_t now = monotonicNanoseconds();
    if (soonest <= now)
        return 0; // already due
    const std::uint64_t deltaMilliseconds = (soonest - now) / 1000000ull;
    return deltaMilliseconds > 1000 ? 1000 : static_cast<int>(deltaMilliseconds);
}

void QUICListener::serviceTimers()
{
    // Reverse iteration so a reap (erase) doesn't skip the next connection.
    for (std::size_t i = m_connections.size(); i-- > 0;) {
        ngtcp2_conn* connection = m_connections[i]->connection;
        const std::uint64_t now = m_connections[i]->clock.now();
        if (ngtcp2_conn_get_expiry(connection) > now)
            continue;
        if (ngtcp2_conn_handle_expiry(connection, now) != 0) {
            // Fatal (e.g. idle timeout) — drop the connection + its bound client.
            if (m_connectionClosedCallback)
                m_connectionClosedCallback(m_connections[i].get());
            m_connections.erase(m_connections.begin() + static_cast<long>(i));
        }
    }
    // flush() (called by the daemon right after) sends any timer-driven packets:
    // PTO/loss retransmits, the keepalive PING, PATH_CHALLENGE.
}

// Test-support (see HostQUIC.h): a bare QUICConnection with null ngtcp2/TLS
// handles and empty buffers — a valid routing key, safe to sendStream into and
// safe to destroy. Not on any production path.
QUICConnection* makeTestQUICConnection() { return new QUICConnection(); }
void destroyTestQUICConnection(QUICConnection* connection) { delete connection; }
QUICConnection* adoptTestQUICConnection(QUICListener& listener, QUICConnection* connection)
{
    if (!connection)
        return nullptr;
    listener.m_connections.emplace_back(connection); // the table now owns it (unique_ptr)
    return connection; // borrowed routing pointer for the Client to bind
}
std::size_t testQUICConnectionCount(const QUICListener& listener) { return listener.m_connections.size(); }
void setTestQUICClockSource(QUICListener& listener, std::function<std::uint64_t()> source) { listener.m_clockSourceForTesting = std::move(source); }
std::vector<std::uint8_t> testQUICConnectionPendingBytes(const QUICConnection* connection)
{
    if (!connection)
        return {};
    std::vector<std::uint8_t> pending;
    std::array<std::span<const std::uint8_t>, 64> spans;
    const std::size_t count = connection->toSend.unsentSpans(spans);
    for (std::size_t index = 0; index < count; ++index)
        pending.insert(pending.end(), spans[index].begin(), spans[index].end());
    return pending;
}
void setTestQUICConnectionPending(QUICConnection* connection, std::size_t toSendSize, std::size_t toSendConsumed)
{
    if (!connection)
        return;
    const std::vector<std::uint8_t> staged(toSendSize, 0);
    connection->toSend.append(staged);
    connection->toSend.markWritten(toSendConsumed); // a desync (> size) clamps, never underflows pendingBytes
}

std::size_t testQUICQueuedDatagramCount(const QUICConnection* connection)
{
    return connection ? connection->toSendDatagrams.size() : 0;
}

int testQUICFrontDatagramTag(const QUICConnection* connection)
{
    if (!connection || connection->toSendDatagrams.empty() || connection->toSendDatagrams.front().empty())
        return -1;
    return connection->toSendDatagrams.front()[0];
}

void testAckStreamData(QUICConnection* connection, std::uint64_t offset, std::uint64_t datalen)
{
    if (!connection)
        return;
    quicAckedStreamDataHandler(nullptr, connection->streamID, offset, datalen, connection, nullptr);
}

std::size_t testRetainedSendBytes(const QUICConnection* connection) { return connection ? connection->toSend.retainedBytes() : 0; }

bool testPreAuthReapSurvives(std::uint64_t ageMilliseconds, bool authenticated)
{
    // A bare connection is enough: the deadline reads handshakeDone, authenticated
    // and the timestamp, never the ngtcp2 handle, and it is tested before the
    // expiry branch that would.
    auto staged = std::make_unique<QUICConnection>();
    staged->handshakeDone = true;
    staged->authenticated = authenticated;
    const std::uint64_t nowMilliseconds = monotonicNanoseconds() / 1000000u;
    staged->handshakeDoneMilliseconds = nowMilliseconds > ageMilliseconds ? nowMilliseconds - ageMilliseconds : 0;
    return !preAuthDeadlinePassed(*staged, nowMilliseconds);
}

bool testAcceptServerConnectionRejectsCID(std::size_t scidlen, std::size_t dcidlen)
{
    // Drive acceptServerConnection's CID clamp + memcpy directly with a synthetic
    // ngtcp2_version_cid whose scidlen/dcidlen exceed NGTCP2_MAX_CIDLEN. ngtcp2's
    // real ngtcp2_pkt_decode_version_cid filters CIDs longer than the max, so the
    // overlong lengths are injected here, not parsed off the wire. With the clamp
    // present the function returns nullptr (rejects) before the memcpy; with the
    // clamp removed the memcpy writes scidlen/dcidlen (> 20) bytes into the
    // fixed-size 20-byte ngtcp2_cid.data — an overflow ASan reports.
    //
    // A minimal TLS context so the leading SSL_new() (before the clamp) succeeds;
    // the reject path returns before any handshake is driven, so no cert is needed.
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    if (!ctx)
        return false; // can't exercise the path — fail the test conservatively
    ngtcp2_crypto_boringssl_configure_server_context(ctx);

    // Source buffers sized to the (oversized) declared lengths, so the unclamped
    // memcpy has real bytes to read past the destination's 20.
    std::vector<std::uint8_t> scidBytes(scidlen, 0xAB);
    std::vector<std::uint8_t> dcidBytes(dcidlen, 0xCD);

    ngtcp2_version_cid vc {};
    vc.version = NGTCP2_PROTO_VER_V1;
    vc.scid = scidBytes.data();
    vc.scidlen = scidlen;
    vc.dcid = dcidBytes.data();
    vc.dcidlen = dcidlen;

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    QUICListener listener; // only stored as c->owner on the reject path; never dereferenced
    ngtcp2_cid unused {}; // the CID clamp rejects before either is read
    auto result = acceptServerConnection(&listener, ctx, vc, reinterpret_cast<sockaddr*>(&addr), sizeof(addr), reinterpret_cast<sockaddr*>(&addr), sizeof(addr), unused, unused, {});
    const bool rejected = (result == nullptr);

    SSL_CTX_free(ctx);
    return rejected;
}

} // namespace Host
} // namespace Ubiquitty
