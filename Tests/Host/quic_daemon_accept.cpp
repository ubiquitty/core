// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_daemon_accept.cpp — drive a real QUICListener through a
// handshake.
//
// Unlike quic_loopback (which uses an inline server), this exercises the
// daemon's actual Source/Platform/Host/HostQUIC.cpp QUICListener: it stands up
// the listener, points a ngtcp2 client at its bound port over localhost UDP, and
// pumps both (the client write/read loop + listener.processReadable()) until the
// client's handshake completes — proving the per-connection accept + server
// state machine in the daemon code path works, not just compiles.
//
// Built only under -DUBIQUITTY_QUIC=ON. Exit 0 on success.
//

#include "Helpers/TempDir.h"
#include "Platform/Host/HostQUIC.h"

#include <openssl/ssl.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_boringssl.h>

#include <arpa/inet.h>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

namespace {

const char aLPN[] = "\x03mtq"; // must match HostQUIC's qUICAlpn

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

struct Client {
    Client() = default;
    ~Client()
    {
        if (connection)
            ngtcp2_conn_del(connection);
        SSL_free(tls);
        SSL_CTX_free(ctx);
        if (fileDescriptor >= 0)
            ::close(fileDescriptor);
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

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
    std::string received;
};

int clientReceiveStreamHandler(ngtcp2_conn*, std::uint32_t, std::int64_t, std::uint64_t, const std::uint8_t* data, std::size_t datalen, void* user, void*)
{
    static_cast<Client*>(user)->received.append(reinterpret_cast<const char*>(data), datalen);
    return 0;
}

ngtcp2_conn* getConnectionFromRef(ngtcp2_crypto_conn_ref* ref) { return static_cast<Client*>(ref->user_data)->connection; }
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
    static_cast<Client*>(user)->isHandshakeDone = true;
    return 0;
}

ngtcp2_path clientPath(Client& c)
{
    ngtcp2_path p;
    p.local.addr = reinterpret_cast<sockaddr*>(&c.localAddr);
    p.local.addrlen = sizeof(c.localAddr);
    p.remote.addr = reinterpret_cast<sockaddr*>(&c.remoteAddr);
    p.remote.addrlen = sizeof(c.remoteAddr);
    return p;
}

bool clientRead(Client& c)
{
    std::uint8_t buffer[65536];
    for (;;) {
        sockaddr_in from;
        socklen_t fromLength = sizeof(from);
        ssize_t n = ::recvfrom(c.fileDescriptor, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (n < 0)
            return true;
        ngtcp2_path path = clientPath(c);
        ngtcp2_pkt_info pi {};
        if (ngtcp2_conn_read_pkt(c.connection, &path, &pi, buffer, static_cast<std::size_t>(n), nowNanoseconds()) != 0)
            return false;
    }
}

bool clientWrite(Client& c)
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
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(c.connection, &ps.path, &pi, buffer, sizeof(buffer), &wdatalen, NGTCP2_WRITE_STREAM_FLAG_NONE, streamID, datavcnt ? &datav : nullptr, datavcnt, nowNanoseconds());
        if (nwrite < 0)
            return false;
        if (wdatalen > 0)
            c.toSend.erase(0, static_cast<std::size_t>(wdatalen));
        if (nwrite == 0)
            return true;
        ::sendto(c.fileDescriptor, buffer, static_cast<std::size_t>(nwrite), 0, reinterpret_cast<sockaddr*>(&c.remoteAddr), sizeof(c.remoteAddr));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Ubiquitty::Test::TempDir home { "/tmp/ubiquitty-quic-accept.XXXXXX" };
    if (!home.valid() || ::setenv("UBIQUITTY_HOME", home.path().c_str(), 1) != 0)
        return 1;
    Ubiquitty::Host::QUICListener listener;
    const bool usesTransferredSocket = argc == 2 && std::string_view(argv[1]) == "--transferred-socket";
    if (usesTransferredSocket) {
        Ubiquitty::Host::QUICListener predecessor;
        if (!predecessor.start(0))
            return 1;
        const auto port = predecessor.boundPort();
        const auto certificate = predecessor.certificateSHA256();
        auto socket = predecessor.releaseBoundSocket();
        if (!socket || !listener.startOnBoundSocket(std::move(socket)) || listener.boundPort() != port
            || certificate.size() != 32 || listener.certificateSHA256() != certificate) {
            std::fprintf(stderr, "listener socket/certificate transfer failed\n");
            return 1;
        }
    } else if (!listener.start(0)) {
        std::fprintf(stderr, "listener.start failed: %s\n", listener.lastError().c_str());
        return 1;
    }
    // Echo any stream bytes back to the client — exercises the relay's inbound
    // callback + outbound sendStream through the real QUICListener.
    listener.setStreamDataCallback([&listener](Ubiquitty::Host::QUICConnection* c, std::span<const std::uint8_t> bytes) { listener.sendStream(c, bytes); });

    Client client;
    client.fileDescriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
    client.localAddr.sin_family = AF_INET;
    client.localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(client.fileDescriptor, reinterpret_cast<sockaddr*>(&client.localAddr), sizeof(client.localAddr)) != 0) {
        std::fprintf(stderr, "client bind failed\n");
        return 1;
    }
    socklen_t localLength = sizeof(client.localAddr);
    ::getsockname(client.fileDescriptor, reinterpret_cast<sockaddr*>(&client.localAddr), &localLength);
    int flags = ::fcntl(client.fileDescriptor, F_GETFL, 0);
    ::fcntl(client.fileDescriptor, F_SETFL, flags | O_NONBLOCK);
    client.remoteAddr.sin_family = AF_INET;
    client.remoteAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    client.remoteAddr.sin_port = htons(listener.boundPort());

    client.ctx = SSL_CTX_new(TLS_method());
    SSL_CTX_set_min_proto_version(client.ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(client.ctx, TLS1_3_VERSION);
    if (ngtcp2_crypto_boringssl_configure_client_context(client.ctx) != 0) {
        std::fprintf(stderr, "configure_client_context failed\n");
        return 1;
    }
    SSL_CTX_set_verify(client.ctx, SSL_VERIFY_NONE, nullptr);
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
    parameters.initial_max_data = 1024 * 1024;
    parameters.initial_max_stream_data_bidi_local = 256 * 1024;

    ngtcp2_path path = clientPath(client);
    if (ngtcp2_conn_client_new(&client.connection, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &cb, &settings, &parameters, nullptr, &client) != 0) {
        std::fprintf(stderr, "conn_client_new failed\n");
        return 1;
    }
    ngtcp2_conn_set_tls_native_handle(client.connection, client.tls);

    // Drive: client initial out, then pump the real listener + the client until
    // the client handshake completes.
    if (!clientWrite(client))
        return 1;
    for (int iter = 0; iter < 400 && !client.isHandshakeDone; ++iter) {
        listener.processReadable(); // the daemon's accept + server state machine
        if (!clientRead(client) || !clientWrite(client))
            return 1;
        ::usleep(1000);
    }

    if (!client.isHandshakeDone) {
        std::fprintf(stderr, "client handshake did not complete against the daemon listener\n");
        return 1;
    }

    // ---- relay: client opens a stream, sends "ping"; the listener echoes it ----
    const std::string message = "ping";
    if (ngtcp2_conn_open_bidi_stream(client.connection, &client.streamID, &client) != 0) {
        std::fprintf(stderr, "open_bidi_stream failed\n");
        return 1;
    }
    client.toSend = message;
    for (int iter = 0; iter < 400 && client.received != message; ++iter) {
        if (!clientWrite(client))
            return 1;
        listener.processReadable(); // accepts + receives the stream, echoes via the callback
        if (!clientRead(client))
            return 1;
        ::usleep(1000);
    }
    if (client.received != message) {
        std::fprintf(stderr, "stream relay failed (received='%s')\n", client.received.c_str());
        return 1;
    }

    // Exactly one server connection: a retransmitted/duplicate tokened Initial
    // must route to the accepted connection (by its retry-phase dcid), never
    // mint a second one — a duplicate wedges this handshake.
    if (Ubiquitty::Host::testQUICConnectionCount(listener) != 1) {
        std::fprintf(stderr, "duplicate server connection minted for one client (%zu live)\n", Ubiquitty::Host::testQUICConnectionCount(listener));
        return 1;
    }

    std::printf("quic_daemon_accept: %s QUICListener handshake + stream relay complete\n", usesTransferredSocket ? "transferred" : "fresh");
    return 0;
}
