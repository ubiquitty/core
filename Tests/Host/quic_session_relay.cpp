// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_session_relay.cpp — drive a REAL ubiquitty-host daemon over
// QUIC and prove the Sync session binding end to end.
//
// Unlike quic_daemon_accept (which echoes raw stream bytes through a bare
// QUICListener), this spawns the actual `ubiquitty-host --daemon --quic-port N`
// binary, dials its QUIC data-plane with a real ngtcp2 client, opens a bidi
// stream, and speaks the Sync protocol: it sends one canonical Greet and
// verifies a Granted Welcome, the actual pane binding and an applied checkpoint.
// The QUIC connection binds a freshly created daemon Session exactly like
// an AF_UNIX client.
//
// The production client reads fragmented canonical envelopes, applies the
// checkpoint and updates, and sends its own View, credit and acknowledgements.
//
// Built only under -DUBIQUITTY_QUIC=ON. Exit 0 on success. argv[1] = path to the
// built ubiquitty-host binary (the QUIC-enabled one).
//

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Platform/Host/HostSocketName.h"

#include "HostConnection.h"
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
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

const char aLPN[] = "\x03mtq"; // must match HostQUIC's qUICAlpn

namespace Sync = Ubiquitty::Protocol::Sync;

// The production reader owns envelope ordering, pane identity, input numbering
// and complete-picture verification. This fixture supplies only its terminal.
struct RelayScreen final : private Sync::ClientPaneDelegate {
    RelayScreen()
        : connection({ .maxCheckpointBytes = 1 << 20, .maxBufferedUpdateBytes = 1 << 20, .minAckIntervalMicroseconds = 0 }, identity(), this)
    {
        connection.start();
    }

    Ubiquitty::Terminal terminal;
    Sync::ClientConnection connection;

private:
    static Sync::ClientIdentity identity()
    {
        Sync::ClientIdentity identity;
        identity.clientInstance = { .low = 1, .high = 0x72656c6179 };
        identity.request = { .target = Sync::AdmissionTarget::Session };
        identity.facts.viewportColumns = 80;
        identity.facts.viewportRows = 24;
        return identity;
    }

    Ubiquitty::Terminal* terminalForPane(Sync::ID128) override { return &terminal; }
};

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
    ngtcp2_conn* connection = nullptr;
    ngtcp2_crypto_conn_ref connectionRef {};
    SSL_CTX* ctx = nullptr;
    SSL* tls = nullptr;
    int fileDescriptor = -1;
    sockaddr_in localAddr {};
    sockaddr_in remoteAddr {};
    bool handshakeDone = false;
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
    static_cast<Client*>(user)->handshakeDone = true;
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

// Honor ngtcp2's loss/PTO timer so a dropped Initial gets retransmitted (the
// daemon may still be coming up when we send the first packet).
void clientExpiry(Client& c)
{
    std::uint64_t now = nowNanoseconds();
    std::uint64_t exp = ngtcp2_conn_get_expiry(c.connection);
    if (exp != UINT64_MAX && exp <= now)
        ngtcp2_conn_handle_expiry(c.connection, now);
}

// Pick a free UDP port by binding ephemeral and reading it back.
std::uint16_t freeUDPPort()
{
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t length = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &length);
    std::uint16_t port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const char* hostBin = argv[1];

    // Isolated control directory so we never touch a real ~/.ubiquitty or a live daemon.
    char homeTemplate[] = "/tmp/ubiquitty-quic-relay.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    std::string macHome = std::string(home) + "/.ubiquitty";

    std::uint16_t port = freeUDPPort();
    char portArgument[16];
    std::snprintf(portArgument, sizeof(portArgument), "%u", port);

    // --- spawn the real daemon: ubiquitty-host --daemon --quic-port PORT --------
    pid_t daemon = ::fork();
    if (daemon < 0) {
        std::fprintf(stderr, "fork failed\n");
        return 1;
    }
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        char* args[] = { const_cast<char*>(hostBin), const_cast<char*>("--daemon"), const_cast<char*>("--quic-port"), portArgument, const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(hostBin, args);
        _exit(127); // exec failed
    }

    // Wait for the daemon to bind its unix socket (startQUIC runs right after,
    // so the QUIC port is up by then); then a small margin before the first
    // Initial. The PTO retransmit below covers any residual race.
    std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(hostBin);
    // The socket file exists from bind(), before listen(); only an accepted
    // connection says the daemon is up.
    const bool up = Ubiquitty::HostTest::waitForDaemonReady(sockPath);
    if (!up) {
        std::fprintf(stderr, "daemon did not accept a connection on host.sock\n");
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
        return 1;
    }
    ::usleep(100 * 1000);

    // Mint a one-time token through the bootstrap the product ships; the QUIC
    // stream must lead with it (the daemon rejects a token-less connection).
    std::string quicToken, quicCert; // the inline ngtcp2 client doesn't pin
    std::uint16_t mintedPort = 0;
    if (!mintQUICTokenViaBootstrap(hostBin, macHome, quicToken, mintedPort, quicCert)) {
        std::fprintf(stderr, "could not mint a QUIC token\n");
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
        return 1;
    }

    // --- QUIC client → daemon -------------------------------------------------
    Client client;
    client.fileDescriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
    client.localAddr.sin_family = AF_INET;
    client.localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(client.fileDescriptor, reinterpret_cast<sockaddr*>(&client.localAddr), sizeof(client.localAddr));
    socklen_t localLength = sizeof(client.localAddr);
    ::getsockname(client.fileDescriptor, reinterpret_cast<sockaddr*>(&client.localAddr), &localLength);
    int flags = ::fcntl(client.fileDescriptor, F_GETFL, 0);
    ::fcntl(client.fileDescriptor, F_SETFL, flags | O_NONBLOCK);
    client.remoteAddr.sin_family = AF_INET;
    client.remoteAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    client.remoteAddr.sin_port = htons(port);

    client.ctx = SSL_CTX_new(TLS_method());
    SSL_CTX_set_min_proto_version(client.ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(client.ctx, TLS1_3_VERSION);
    if (ngtcp2_crypto_boringssl_configure_client_context(client.ctx) != 0) {
        std::fprintf(stderr, "configure_client_context failed\n");
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
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
    parameters.initial_max_data = 4 * 1024 * 1024;
    parameters.initial_max_stream_data_bidi_local = 1024 * 1024;

    ngtcp2_path path = clientPath(client);
    if (ngtcp2_conn_client_new(&client.connection, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &cb, &settings, &parameters, nullptr, &client) != 0) {
        std::fprintf(stderr, "conn_client_new failed\n");
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
        return 1;
    }
    ngtcp2_conn_set_tls_native_handle(client.connection, client.tls);

    int rc = 1;
    bool sawWelcome = false, sawScreen = false;
    bool didSendGreet = false, inputSent = false, sawLiveUpdate = false;
    std::size_t viewsScanned = 0;
    std::uint64_t baselineUpdates = 0;
    RelayScreen screen;
    int lastInputIter = -1000; // bounded-retry cadence for the live-update probe
    clientWrite(client); // client Initial
    for (int iter = 0; iter < 5000; ++iter) {
        clientExpiry(client);
        if (!clientWrite(client))
            break;
        if (!clientRead(client))
            break;
        // The real bootstrap token authenticates this carrier. This is explicit
        // fresh creation, so no earlier session or daemon pin exists yet.
        if (client.handshakeDone && !didSendGreet) {
            if (ngtcp2_conn_open_bidi_stream(client.connection, &client.streamID, &client) == 0) {
                client.toSend.push_back(static_cast<char>(Ubiquitty::Protocol::Sync::PreambleKind::Token));
                client.toSend.push_back(static_cast<char>(quicToken.size()));
                client.toSend.append(quicToken);
                const auto greeting = screen.connection.takeStreamOutput();
                client.toSend.append(greeting.begin(), greeting.end());
                didSendGreet = true;
            }
        }
        if (!didSendGreet) {
            ::usleep(1000);
            continue;
        }
        if (viewsScanned < client.received.size()) {
            screen.connection.receiveStream({ reinterpret_cast<const std::uint8_t*>(client.received.data() + viewsScanned), client.received.size() - viewsScanned }, nowNanoseconds() / 1000);
            viewsScanned = client.received.size();
        }
        if (screen.connection.state() == Sync::ConnectionState::Closed)
            break;
        if (const auto& welcome = screen.connection.welcome()) {
            const auto& selected = welcome->selection;
            // A person on QUIC is not the host's local owner: the Owner role it
            // asks for is granted as Driver, its token's ceiling.
            if (welcome->admission != Sync::AttachmentAdmission::Granted || welcome->grantedRole != Sync::Role::Driver
                || selected.target != Sync::AdmissionTarget::Session || !selected.sessionID || !selected.daemonInstanceID
                || selected.socketName != Ubiquitty::Host::socketLeafForBinary(hostBin))
                break;
            sawWelcome = true;
        }
        const auto panes = screen.connection.boundPanes();
        if (panes.size() > 1)
            break;
        if (!panes.empty() && (!screen.connection.welcome() || panes.front().second.low != screen.connection.welcome()->selection.sessionID || panes.front().second.high != screen.connection.welcome()->selection.daemonInstanceID))
            break;
        sawScreen = panes.size() == 1 && screen.connection.isPaneCurrent(panes.front().first)
            && screen.connection.counters().checkpointsApplied != 0;
        // After an applied checkpoint, drive an applied live update. This proves the daemon sends
        // ongoing updates to a QUIC client (fileDescriptor -1, live
        // quicConnection), not just the attach checkpoint. The input is re-driven
        // on a bounded cadence until a second screen frame lands, so a loaded
        // loopback (CI) that is slow to echo does not fail the probe. Re-running
        // echo only adds more updates; a header without a valid picture is not success.
        if (sawWelcome && sawScreen && (iter - lastInputIter) >= 500) {
            if (!inputSent)
                baselineUpdates = screen.connection.counters().updatesApplied;
            constexpr std::string_view input = "echo quic-live\r";
            if (!screen.connection.sendRaw(panes.front().first, { reinterpret_cast<const std::uint8_t*>(input.data()), input.size() }))
                break;
            inputSent = true;
            lastInputIter = iter;
        }
        screen.connection.tick(nowNanoseconds() / 1000);
        const auto upstream = screen.connection.takeStreamOutput();
        client.toSend.append(upstream.begin(), upstream.end());
        if (inputSent && screen.connection.counters().updatesApplied > baselineUpdates) {
            sawLiveUpdate = true;
            rc = 0;
            break;
        }
        ::usleep(1000);
    }

    if (rc != 0)
        std::fprintf(stderr, "quic_session_relay: FAIL (handshake=%d welcome=%d checkpoint=%d liveUpdate=%d)\n", client.handshakeDone, sawWelcome, sawScreen, sawLiveUpdate);
    else
        std::printf("quic_session_relay: canonical Greet over QUIC → Granted Welcome + an applied checkpoint, then a "
                    "keystroke drove a LIVE screen update to the QUIC client "
                    "(daemon sends ongoing frames over the QUIC relay)\n");

    // --- teardown -------------------------------------------------------------
    ::kill(daemon, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
            break;
        ::usleep(10 * 1000);
    }
    ::kill(daemon, SIGKILL);
    ::waitpid(daemon, nullptr, WNOHANG);
    return rc;
}
