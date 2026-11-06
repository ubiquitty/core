// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_loopback.cpp — a self-contained ngtcp2 + AWS-LC loopback.
//
// Brings up a QUIC server and client (both ngtcp2 connections) over two
// localhost UDP sockets, drives the handshake to completion, then the client
// opens a bidi stream and sends "ping"; the server echoes it back and the
// client verifies it. This is the "QUIC actually works" gate for the daemon's
// QUIC transport: the crypto-default callbacks do the TLS, and we drive
// each ngtcp2_conn from a single thread exactly as the daemon's poll() loop will.
//
// Built only under -DUBIQUITTY_QUIC=ON. Exit 0 on success.
//

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
#include <string>

namespace {

const char aLPN[] = "\x03mtq"; // length-prefixed ALPN protocol id ("mtq")

// Server ALPN selection: accept our single protocol (the client only offers it).
int serverAlpnSelectHandler(SSL*, const unsigned char** outputBuffer, unsigned char* outlen, const unsigned char*, unsigned, void*)
{
    *outputBuffer = reinterpret_cast<const unsigned char*>(aLPN) + 1; // skip the length byte
    *outlen = static_cast<unsigned char>(aLPN[0]);
    return 0; // SSL_TLSEXT_ERR_OK
}

const char certPath[] = "/tmp/ubiquitty-quic-loopback-cert.pem";
const char keyPath[] = "/tmp/ubiquitty-quic-loopback-key.pem";

// Self-contained: generate the server's self-signed cert into /tmp if absent so
// the test needs no pre-existing daemon state. False when openssl could not, as
// every handshake after it would then fail for want of a certificate.
[[nodiscard]] bool ensureLoopbackCert()
{
    if (::access(certPath, R_OK) == 0 && ::access(keyPath, R_OK) == 0)
        return true;
    char command[1024];
    std::snprintf(command, sizeof(command),
        "openssl req -x509 -newkey rsa:2048 -keyout '%s' -out '%s' -days 2 -nodes "
        "-subj '/CN=loopback' >/dev/null 2>&1",
        keyPath, certPath);
    return ::system(command) == 0;
}

std::uint64_t timestampNanoseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

void fillRandom(std::uint8_t* dest, std::size_t length)
{
    // Test-grade randomness; the real listener uses AWS-LC's RNG.
    static int urandom = ::open("/dev/urandom", O_RDONLY);
    if (urandom < 0 || ::read(urandom, dest, length) != static_cast<ssize_t>(length)) {
        for (std::size_t i = 0; i < length; ++i)
            dest[i] = static_cast<std::uint8_t>(i * 2654435761u);
    }
}

void randHandler(std::uint8_t* dest, std::size_t destlen, const ngtcp2_rand_ctx*) { fillRandom(dest, destlen); }

int getNewConnectionIdHandler(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t cidlen, void*)
{
    fillRandom(cid->data, cidlen);
    cid->datalen = cidlen;
    fillRandom(token->data, sizeof(token->data));
    return 0;
}

// One end of the loopback (client or server).
struct Endpoint {
    ngtcp2_conn* connection = nullptr;
    ngtcp2_crypto_conn_ref connectionRef {}; // TLS app-data → ngtcp2_conn back-ref
    SSL_CTX* tlsContext = nullptr;
    SSL* tls = nullptr;
    int fileDescriptor = -1;
    sockaddr_in localAddr {};
    sockaddr_in remoteAddr {};
    bool isServer = false;
    std::int64_t streamID = -1;
    std::string toSend; // pending outbound stream payload
    std::string received; // inbound stream payload
    std::string toSendDatagram; // pending outbound DATAGRAM payload (one frame)
    std::string receivedDatagram; // inbound DATAGRAM payload
    bool handshakeDone = false;
};

ngtcp2_conn* getConnectionFromRef(ngtcp2_crypto_conn_ref* ref) { return static_cast<Endpoint*>(ref->user_data)->connection; }

// Link a TLS session back to its ngtcp2_conn so the crypto callbacks
// (add_handshake_data etc.) can find the connection.
void linkTLS(Endpoint& ep)
{
    ep.connectionRef.get_conn = getConnectionFromRef;
    ep.connectionRef.user_data = &ep;
    SSL_set_app_data(ep.tls, &ep.connectionRef);
}

int receiveStreamDataHandler(ngtcp2_conn*, std::uint32_t, std::int64_t streamID, std::uint64_t, const std::uint8_t* data, std::size_t datalen, void* user, void*)
{
    Endpoint* ep = static_cast<Endpoint*>(user);
    if (ep->isServer) {
        // Echo what the client sent back on the same stream.
        if (ep->streamID < 0)
            ep->streamID = streamID;
        ep->toSend.append(reinterpret_cast<const char*>(data), datalen);
    } else {
        ep->received.append(reinterpret_cast<const char*>(data), datalen);
    }
    return 0;
}

int handshakeCompletedHandler(ngtcp2_conn*, void* user)
{
    static_cast<Endpoint*>(user)->handshakeDone = true;
    return 0;
}

int receiveDatagramHandler(ngtcp2_conn*, std::uint32_t, const std::uint8_t* data, std::size_t datalen, void* user)
{
    static_cast<Endpoint*>(user)->receivedDatagram.append(reinterpret_cast<const char*>(data), datalen);
    return 0;
}

ngtcp2_path makePath(Endpoint& ep)
{
    ngtcp2_path path;
    path.local.addr = reinterpret_cast<sockaddr*>(&ep.localAddr);
    path.local.addrlen = sizeof(ep.localAddr);
    path.remote.addr = reinterpret_cast<sockaddr*>(&ep.remoteAddr);
    path.remote.addrlen = sizeof(ep.remoteAddr);
    return path;
}

bool openSocket(Endpoint& ep)
{
    ep.fileDescriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (ep.fileDescriptor < 0)
        return false;
    ep.localAddr.sin_family = AF_INET;
    ep.localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ep.localAddr.sin_port = 0;
    if (::bind(ep.fileDescriptor, reinterpret_cast<sockaddr*>(&ep.localAddr), sizeof(ep.localAddr)) != 0)
        return false;
    socklen_t length = sizeof(ep.localAddr);
    ::getsockname(ep.fileDescriptor, reinterpret_cast<sockaddr*>(&ep.localAddr), &length);
    int flags = ::fcntl(ep.fileDescriptor, F_GETFL, 0);
    ::fcntl(ep.fileDescriptor, F_SETFL, flags | O_NONBLOCK);
    return true;
}

void commonCallbacks(ngtcp2_callbacks& cb)
{
    cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    cb.encrypt = ngtcp2_crypto_encrypt_cb;
    cb.decrypt = ngtcp2_crypto_decrypt_cb;
    cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
    cb.update_key = ngtcp2_crypto_update_key_cb;
    cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    cb.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    cb.rand = randHandler;
    cb.get_new_connection_id2 = getNewConnectionIdHandler;
    cb.handshake_completed = handshakeCompletedHandler;
    cb.recv_stream_data = receiveStreamDataHandler;
    cb.recv_datagram = receiveDatagramHandler;
}

void defaultParameters(ngtcp2_transport_params& parameters)
{
    ngtcp2_transport_params_default(&parameters);
    parameters.initial_max_streams_bidi = 8;
    parameters.initial_max_streams_uni = 8;
    parameters.active_connection_id_limit = 8; // spare CIDs for migration
    parameters.initial_max_stream_data_bidi_local = 256 * 1024;
    parameters.initial_max_stream_data_bidi_remote = 256 * 1024;
    parameters.initial_max_data = 1024 * 1024;
    parameters.max_datagram_frame_size = 65535; // accept RFC 9221 DATAGRAM frames
}

// Drain a connection's UDP socket into ngtcp2.
bool readPackets(Endpoint& ep)
{
    std::uint8_t buffer[65536];
    for (;;) {
        sockaddr_in from;
        socklen_t fromLength = sizeof(from);
        ssize_t n = ::recvfrom(ep.fileDescriptor, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (n < 0)
            return true; // drained
        // Feed the ACTUAL packet source (not a cached remote) so ngtcp2 sees a
        // roamed peer's new address and runs path validation — mirrors HostQUIC.
        ngtcp2_path path;
        path.local.addr = reinterpret_cast<sockaddr*>(&ep.localAddr);
        path.local.addrlen = sizeof(ep.localAddr);
        path.remote.addr = reinterpret_cast<sockaddr*>(&from);
        path.remote.addrlen = fromLength;
        ngtcp2_pkt_info pi {};
        int rv = ngtcp2_conn_read_pkt(ep.connection, &path, &pi, buffer, static_cast<std::size_t>(n), timestampNanoseconds());
        if (rv != 0) {
            std::fprintf(stderr, "read_pkt(%s): %s\n", ep.isServer ? "server" : "client", ngtcp2_strerror(rv));
            return false;
        }
    }
}

// Pump a connection's pending packets out to its UDP socket.
bool writePackets(Endpoint& ep)
{
    std::uint8_t buffer[1452];

    // Drain a pending DATAGRAM first (mirrors HostQUIC's quicWritePump): one
    // writev_datagram, its own packet, sent to the path ngtcp2 chose.
    while (!ep.toSendDatagram.empty()) {
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi {};
        int accepted = 0;
        ngtcp2_vec datav { reinterpret_cast<std::uint8_t*>(ep.toSendDatagram.data()), ep.toSendDatagram.size() };
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_datagram(ep.connection, &ps.path, &pi, buffer, sizeof(buffer), &accepted, NGTCP2_WRITE_DATAGRAM_FLAG_NONE, 0, &datav, 1, timestampNanoseconds());
        if (nwrite < 0) {
            std::fprintf(stderr, "writev_datagram(%s): %s\n", ep.isServer ? "server" : "client", ngtcp2_strerror(static_cast<int>(nwrite)));
            return false;
        }
        if (nwrite == 0)
            break; // congestion — retry next pump
        ::sendto(ep.fileDescriptor, buffer, static_cast<std::size_t>(nwrite), 0, ps.path.remote.addr, ps.path.remote.addrlen);
        if (accepted)
            ep.toSendDatagram.clear();
        else
            break;
    }

    for (;;) {
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi {};
        ngtcp2_ssize wdatalen = 0;
        std::int64_t streamID = -1;
        std::uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_NONE;
        ngtcp2_vec datav { nullptr, 0 };
        int datavcnt = 0;

        // Carry any pending stream payload (client's "ping" or the server's echo)
        // with FIN — a one-shot message for this test.
        if (ep.streamID >= 0 && !ep.toSend.empty()) {
            datav.base = reinterpret_cast<std::uint8_t*>(ep.toSend.data());
            datav.len = ep.toSend.size();
            streamID = ep.streamID;
            datavcnt = 1;
            flags = NGTCP2_WRITE_STREAM_FLAG_FIN;
        }

        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(ep.connection, &ps.path, &pi, buffer, sizeof(buffer), &wdatalen, flags, streamID, datavcnt ? &datav : nullptr, datavcnt, timestampNanoseconds());
        if (nwrite < 0) {
            std::fprintf(stderr, "writev_stream(%s): %s\n", ep.isServer ? "server" : "client", ngtcp2_strerror(static_cast<int>(nwrite)));
            return false;
        }
        if (wdatalen > 0)
            ep.toSend.erase(0, static_cast<std::size_t>(wdatalen));
        if (nwrite == 0)
            return true;
        // Send to the path ngtcp2 chose (ps.path is out) — honors a migration
        // probe to a new path, mirroring HostQUIC's quicWritePump.
        ::sendto(ep.fileDescriptor, buffer, static_cast<std::size_t>(nwrite), 0, ps.path.remote.addr, ps.path.remote.addrlen);
    }
}

} // namespace

int main()
{
    if (!ensureLoopbackCert()) {
        std::fprintf(stderr, "loopback certificate generation failed\n");
        return 1;
    }

    Endpoint server, client;
    server.isServer = true;
    if (!openSocket(server) || !openSocket(client)) {
        std::fprintf(stderr, "socket setup failed\n");
        return 1;
    }
    // Each end's remote is the other's bound loopback address.
    server.remoteAddr = client.localAddr;
    client.remoteAddr = server.localAddr;

    // ---- client TLS + connection ----
    client.tlsContext = SSL_CTX_new(TLS_method());
    SSL_CTX_set_min_proto_version(client.tlsContext, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(client.tlsContext, TLS1_3_VERSION);
    if (ngtcp2_crypto_boringssl_configure_client_context(client.tlsContext) != 0) {
        std::fprintf(stderr, "configure_client_context failed\n");
        return 1;
    }
    // Self-signed server cert + SSH-token auth → don't CA-verify the peer.
    SSL_CTX_set_verify(client.tlsContext, SSL_VERIFY_NONE, nullptr);
    client.tls = SSL_new(client.tlsContext);
    SSL_set_connect_state(client.tls); // AWS-LC TLS_method() is role-agnostic
    SSL_set_alpn_protos(client.tls, reinterpret_cast<const unsigned char*>(aLPN), sizeof(aLPN) - 1);
    linkTLS(client);

    ngtcp2_cid dcid, scid;
    dcid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
    fillRandom(dcid.data, dcid.datalen);
    scid.datalen = 8;
    fillRandom(scid.data, scid.datalen);

    ngtcp2_callbacks ccb {};
    commonCallbacks(ccb);
    ccb.client_initial = ngtcp2_crypto_client_initial_cb;
    ccb.recv_retry = ngtcp2_crypto_recv_retry_cb;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = timestampNanoseconds();
    ngtcp2_transport_params clientParameters;
    defaultParameters(clientParameters);

    ngtcp2_path cpath = makePath(client);
    if (ngtcp2_conn_client_new(&client.connection, &dcid, &scid, &cpath, NGTCP2_PROTO_VER_V1, &ccb, &settings, &clientParameters, nullptr, &client) != 0) {
        std::fprintf(stderr, "conn_client_new failed\n");
        return 1;
    }
    ngtcp2_conn_set_tls_native_handle(client.connection, client.tls);

    // ---- kick the handshake: client writes its initial ----
    if (!writePackets(client))
        return 1;

    // ---- drive the loopback until the client handshake completes ----
    bool serverConnectionCreated = false;
    for (int iter = 0; iter < 400 && !(client.handshakeDone && server.handshakeDone); ++iter) {
        // The server's connection is created lazily from the client's first initial.
        if (!serverConnectionCreated) {
            std::uint8_t peek[65536];
            sockaddr_in from;
            socklen_t fromLength = sizeof(from);
            ssize_t n = ::recvfrom(server.fileDescriptor, peek, sizeof(peek), MSG_PEEK, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (n > 0) {
                ngtcp2_version_cid vc;
                if (ngtcp2_pkt_decode_version_cid(&vc, peek, static_cast<std::size_t>(n), NGTCP2_MAX_CIDLEN) == 0) {
                    server.tlsContext = SSL_CTX_new(TLS_method());
                    SSL_CTX_set_min_proto_version(server.tlsContext, TLS1_3_VERSION);
                    SSL_CTX_set_max_proto_version(server.tlsContext, TLS1_3_VERSION);
                    ngtcp2_crypto_boringssl_configure_server_context(server.tlsContext);
                    SSL_CTX_set_alpn_select_cb(server.tlsContext, serverAlpnSelectHandler, nullptr);
                    SSL_CTX_use_certificate_chain_file(server.tlsContext, certPath);
                    SSL_CTX_use_PrivateKey_file(server.tlsContext, keyPath, SSL_FILETYPE_PEM);
                    server.tls = SSL_new(server.tlsContext);
                    SSL_set_accept_state(server.tls); // AWS-LC TLS_method() is role-agnostic
                    linkTLS(server);

                    ngtcp2_cid sdcid, sscid, ocid;
                    std::memcpy(sdcid.data, vc.dcid, vc.dcidlen);
                    sdcid.datalen = vc.dcidlen;
                    std::memcpy(sscid.data, vc.scid, vc.scidlen);
                    sscid.datalen = vc.scidlen;
                    std::memcpy(ocid.data, vc.dcid, vc.dcidlen);
                    ocid.datalen = vc.dcidlen;

                    ngtcp2_callbacks scb {};
                    commonCallbacks(scb);
                    scb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;

                    ngtcp2_settings sset;
                    ngtcp2_settings_default(&sset);
                    sset.initial_ts = timestampNanoseconds();
                    ngtcp2_transport_params serverParameters;
                    defaultParameters(serverParameters);
                    serverParameters.original_dcid = ocid;
                    serverParameters.original_dcid_present = 1;

                    ngtcp2_cid newscid;
                    newscid.datalen = 8;
                    fillRandom(newscid.data, newscid.datalen);

                    ngtcp2_path spath = makePath(server);
                    if (ngtcp2_conn_server_new(&server.connection, &sscid, &newscid, &spath, vc.version, &scb, &sset, &serverParameters, nullptr, &server) != 0) {
                        std::fprintf(stderr, "conn_server_new failed\n");
                        return 1;
                    }
                    ngtcp2_conn_set_tls_native_handle(server.connection, server.tls);
                    serverConnectionCreated = true;
                }
            }
        }

        if (server.connection) {
            if (!readPackets(server) || !writePackets(server))
                return 1;
        }
        if (!readPackets(client) || !writePackets(client))
            return 1;
        ::usleep(1000);
    }

    if (!client.handshakeDone || !server.handshakeDone) {
        std::fprintf(stderr, "handshake did not complete (client=%d server=%d)\n", client.handshakeDone, server.handshakeDone);
        return 1;
    }

    // ---- data path: client opens a stream, sends "ping"; server echoes it ----
    const std::string message = "ping";
    if (ngtcp2_conn_open_bidi_stream(client.connection, &client.streamID, &client) != 0) {
        std::fprintf(stderr, "open_bidi_stream failed\n");
        return 1;
    }
    client.toSend = message;
    for (int iter = 0; iter < 400 && client.received != message; ++iter) {
        if (!writePackets(client) || !readPackets(server) || !writePackets(server) || !readPackets(client))
            return 1;
        ::usleep(1000);
    }
    if (client.received != message) {
        std::fprintf(stderr, "stream echo failed (received='%s')\n", client.received.c_str());
        return 1;
    }

    // ---- ROAM: the client's source address changes (wifi<->cellular / NAT
    // rebind). Re-bind the client UDP socket to a NEW ephemeral port while
    // keeping the connection's stable local addr; the server (which routes by DCID)
    // must validate + adopt the new path and keep echoing — no re-handshake.
    const sockaddr_in originalLocal = client.localAddr;
    ::close(client.fileDescriptor);
    if (!openSocket(client)) {
        std::fprintf(stderr, "client re-bind failed\n");
        return 1;
    }
    const std::uint16_t newSourcePort = client.localAddr.sin_port;
    client.localAddr = originalLocal; // keep ngtcp2's view of our local stable
    if (newSourcePort == originalLocal.sin_port) {
        std::fprintf(stderr, "re-bind got the same port; test inconclusive\n");
        return 1;
    }

    const std::string message2 = "roam";
    client.received.clear();
    client.toSend.clear();
    client.streamID = -1;
    // The first stream was FIN'd; let the server adopt the fresh post-roam stream
    // (receiveStreamDataHandler only latches streamID when < 0).
    server.streamID = -1;
    server.toSend.clear();
    if (ngtcp2_conn_open_bidi_stream(client.connection, &client.streamID, &client) != 0) {
        std::fprintf(stderr, "open_bidi_stream (post-roam) failed\n");
        return 1;
    }
    client.toSend = message2;
    bool serverAdoptedNewPath = false;
    for (int iter = 0; iter < 4000 && client.received != message2; ++iter) {
        if (!writePackets(client) || !readPackets(server) || !writePackets(server) || !readPackets(client))
            return 1;
        const ngtcp2_path* sp = ngtcp2_conn_get_path2(server.connection);
        if (sp && sp->remote.addr && reinterpret_cast<const sockaddr_in*>(sp->remote.addr)->sin_port == newSourcePort)
            serverAdoptedNewPath = true;
        ::usleep(1000);
    }
    if (client.received != message2) {
        std::fprintf(stderr, "post-roam echo failed (received='%s')\n", client.received.c_str());
        return 1;
    }
    if (!serverAdoptedNewPath) {
        std::fprintf(stderr, "server did not adopt the roamed client path\n");
        return 1;
    }

    // ---- DATAGRAM path: the server ships one unreliable DATAGRAM (an Update
    // stand-in) to the client; it must arrive intact over the post-roam path. ----
    const std::string datagram = "ROWDELTA-over-datagram";
    server.toSendDatagram = datagram;
    for (int iter = 0; iter < 400 && client.receivedDatagram != datagram; ++iter) {
        if (!writePackets(server) || !readPackets(client) || !writePackets(client) || !readPackets(server))
            return 1;
        ::usleep(1000);
    }
    if (client.receivedDatagram != datagram) {
        std::fprintf(stderr, "datagram delivery failed (received='%s')\n", client.receivedDatagram.c_str());
        return 1;
    }

    std::printf("quic_loopback: handshake + stream echo complete; "
                "migration (client roam) survived — echo continued, server adopted the new path; "
                "DATAGRAM (RFC 9221) delivered host->client\n");
    return 0;
}
