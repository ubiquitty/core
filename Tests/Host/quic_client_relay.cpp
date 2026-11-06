// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_client_relay.cpp — drive the PRODUCTION portable QUICClient
// against a real ubiquitty-host daemon, end to end.
//
// Where quic_session_relay hand-rolls an inline ngtcp2 client, this exercises
// Source/Core/Transport/QUICClient (the class the macOS + Linux frontends will wrap): it
// spawns `ubiquitty-host --daemon --quic-port N`, QUICClient.start()s to it, sends
// one canonical Greet on the onConnect callback, and asserts the
// daemon's granted Welcome, actual PaneBind and checkpoint arrive via onData — the portable
// client carries the Sync session over QUIC exactly like the AF_UNIX transports.
//
// Built only under -DUBIQUITTY_QUIC=ON. Exit 0 on success. argv[1] = the built
// (QUIC-enabled) ubiquitty-host binary.
//

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Transport/QUICClient.h"
#include "Platform/Host/HostSocketName.h"

#include "Host/HostConnection.h"
#include "quic_test_mint.h"

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;

std::vector<std::uint8_t> encodeHelloFrame(std::uint16_t columns, std::uint16_t rows)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { 1, 0x71756963636c };
    greeting.clientKind = Sync::ClientKind::Human;
    greeting.requestedRole = Sync::Role::Owner;
    // This is fresh creation over a certificate-pinned, authenticated carrier;
    // there is no previously held daemon or session identity to resume.
    greeting.request = { .target = Sync::AdmissionTarget::Session };
    greeting.facts.viewportColumns = columns;
    greeting.facts.viewportRows = rows;
    const auto encoded = Ubiquitty::HostTest::greetFrame(greeting);
    return { encoded.begin(), encoded.end() };
}

bool scanFrames(const std::string& s, bool& sawHelloAck, bool& sawScreen)
{
    Sync::FrameDecoder decoder;
    decoder.append({ reinterpret_cast<const std::uint8_t*>(s.data()), s.size() });
    std::optional<Sync::PaneBind> pane;
    while (const auto frame = decoder.next()) {
        if (frame->kind == Sync::FrameKind::Welcome) {
            const auto welcome = Sync::decodeWelcome(frame->body);
            if (sawHelloAck || frame->channel || !welcome
                || welcome->admission != Sync::AttachmentAdmission::Granted
                // A person on QUIC is not the host's local owner: the Owner
                // role it asks for is granted as Driver, its token's ceiling.
                || welcome->grantedRole != Sync::Role::Driver
                || welcome->selection.target != Sync::AdmissionTarget::Session
                || !welcome->selection.sessionID || !welcome->selection.daemonInstanceID)
                return false;
            sawHelloAck = true;
        } else if (frame->kind == Sync::FrameKind::PaneBind) {
            if (!sawHelloAck || pane || frame->channel)
                return false;
            pane = Sync::decodePaneBind(frame->body);
            if (!pane)
                return false;
        } else if (frame->kind == Sync::FrameKind::CheckpointHead) {
            if (!pane || frame->channel != pane->handle || !Sync::decodeCheckpointHead(frame->body))
                return false;
            sawScreen = true;
        } else if (frame->kind == Sync::FrameKind::Bye)
            return false;
    }
    return decoder.error() == Sync::EnvelopeError::None;
}

std::uint16_t freeUDPPort()
{
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t length = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &length);
    std::uint16_t port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

} // namespace

// Milliseconds on a monotonic clock, for timing a call that must not wait.
static std::uint64_t nowMilliseconds()
{
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const char* hostBin = argv[1];

    char homeTemplate[] = "/tmp/ubiquitty-quic-client.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    std::string macHome = std::string(home) + "/.ubiquitty";

    std::uint16_t port = freeUDPPort();
    char portArgument[16];
    std::snprintf(portArgument, sizeof(portArgument), "%u", port);

    pid_t daemon = ::fork();
    if (daemon < 0) {
        std::fprintf(stderr, "fork failed\n");
        return 1;
    }
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        char* args[] = { const_cast<char*>(hostBin), const_cast<char*>("--daemon"), const_cast<char*>("--quic-port"), portArgument, const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(hostBin, args);
        _exit(127);
    }

    std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(hostBin);
    bool isDaemonReady = Ubiquitty::HostTest::waitForDaemonReady(sockPath);
    if (!isDaemonReady) {
        std::fprintf(stderr, "daemon never accepted a connection\n");
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
        return 1;
    }
    ::usleep(100 * 1000);

    // --- drive the production QUICClient -------------------------------------
    int rcResolution = 0;
    std::mutex mutex;
    std::string received;
    std::atomic<bool> done { false };
    std::atomic<bool> dropped { false };

    Ubiquitty::Transport::QUICClient client;
    Ubiquitty::Transport::QUICClient::Callbacks callbacks;
    callbacks.onConnect = [&client] {
        std::vector<std::uint8_t> frame = encodeHelloFrame(80, 24);
        client.send(frame);
    };
    std::size_t viewsScanned = 0;
    callbacks.onData = [&](std::span<const std::uint8_t> bytes) {
        bool sawHelloAck = false, sawScreen = false;
        std::string views;
        {
            std::lock_guard<std::mutex> lock(mutex);
            received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!scanFrames(received, sawHelloAck, sawScreen)) {
                dropped.store(true);
                return;
            }
            // The host sends a pane's screen once the client shows it.
            views = Ubiquitty::HostTest::viewsForBoundPanes(received, viewsScanned);
        }
        if (!views.empty())
            client.send(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(views.data()), views.size()));
        if (sawHelloAck && sawScreen)
            done.store(true);
    };
    callbacks.onClose = [&](Ubiquitty::Transport::Health, const std::string&) { dropped.store(true); };

    // Mint a one-time token through the bootstrap the product ships (the ssh
    // bootstrap runs it over the ssh channel); the client presents it to
    // authenticate.
    std::string token, certSHA256;
    std::uint16_t mintedPort = 0;
    if (!mintQUICTokenViaBootstrap(hostBin, macHome, token, mintedPort, certSHA256)) {
        std::fprintf(stderr, "could not mint a QUIC token\n");
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
        return 1;
    }
    if (certSHA256.size() != 32) {
        std::fprintf(stderr, "bootstrap returned no cert fingerprint\n");
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
        return 1;
    }

    // start() must not wait on the resolver. The thread it is called on is the
    // one a frontend draws its window on, and a resolver holds its caller for
    // however long it takes — which for a name that cannot resolve is the whole
    // timeout. That case is timed here because it is the slowest one and the
    // one a user notices.
    {
        Ubiquitty::Transport::QUICClient slowName;
        Ubiquitty::Transport::QUICClient::Callbacks quietCallbacks;
        std::atomic<bool> nameClosed { false };
        quietCallbacks.onClose = [&](Ubiquitty::Transport::Health, const std::string&) { nameClosed.store(true); };
        const std::uint64_t before = nowMilliseconds();
        const auto startedSlow = slowName.start("no-such-host.invalid", 4433, Ubiquitty::Transport::QUICCredential {}, "", quietCallbacks);
        const std::uint64_t elapsed = nowMilliseconds() - before;
        if (!startedSlow.has_value()) {
            std::fprintf(stderr, "quic_client_relay: start() refused a resolvable-later name\n");
            rcResolution = 1;
        }
        if (elapsed > 250) {
            std::fprintf(stderr, "quic_client_relay: start() waited %llums on the resolver\n",
                static_cast<unsigned long long>(elapsed));
            rcResolution = 1;
        }
        // And the failure still arrives, as a close rather than a return value.
        for (int i = 0; i < 500 && !nameClosed.load(); ++i)
            ::usleep(10 * 1000);
        if (!nameClosed.load()) {
            std::fprintf(stderr, "quic_client_relay: a name that cannot resolve never reported a close\n");
            rcResolution = 1;
        }
        slowName.stop();
    }

    {
        const int sink = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t addressLength = sizeof(address);
        if (sink < 0 || ::bind(sink, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
            || ::getsockname(sink, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0) {
            rcResolution = 1;
        } else {
            Ubiquitty::Transport::QUICClient filtered;
            Ubiquitty::Transport::QUICClient::Callbacks filteredCallbacks;
            std::atomic<bool> didClose { false };
            filteredCallbacks.onClose = [&](Ubiquitty::Transport::Health, const std::string&) { didClose.store(true); };
            const auto before = nowMilliseconds();
            const auto started = filtered.start("127.0.0.1", ntohs(address.sin_port), Ubiquitty::Transport::BootstrapToken { .token = token }, certSHA256, filteredCallbacks);
            while (started && !didClose.load() && nowMilliseconds() - before < 8000) {
                ::usleep(10 * 1000);
            }
            if (!started || !didClose.load() || nowMilliseconds() - before < 4000) {
                std::fprintf(stderr, "quic_client_relay: filtered UDP did not expire at the handshake deadline\n");
                rcResolution = 1;
            }
            filtered.stop();
        }
        if (sink >= 0) {
            ::close(sink);
        }
    }

    if (auto started = client.start("127.0.0.1", port, Ubiquitty::Transport::BootstrapToken { .token = token }, certSHA256, callbacks); !started) {
        std::fprintf(stderr, "QUICClient.start failed: StartError %d\n", static_cast<int>(started.error()));
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
        return 1;
    }

    // A stranger's datagram, aimed at the client's own port from a source that
    // is not the daemon. The client's UDP port is reachable by any local
    // process and findable by scanning, and this session lives for hours; a
    // packet from anywhere else must be dropped before it reaches QUIC header
    // parsing, where it would be work done on a stranger's behalf and, worse,
    // indistinguishable from the daemon's.
    //
    // Garbage rather than a crafted packet on purpose: what is being asserted
    // is that the source decides, not the content — and unfiltered garbage is
    // exactly what ends this connection, which is what makes the case fail
    // without the filter.
    bool didInject = false;
    for (int i = 0; i < 500 && !didInject; ++i) {
        const std::uint16_t clientPort = Ubiquitty::Transport::LocalEndpointAccess::localPort(client);
        if (clientPort == 0) {
            ::usleep(10 * 1000);
            continue;
        }
        const int stranger = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (stranger < 0)
            break;
        sockaddr_in target {};
        target.sin_family = AF_INET;
        target.sin_port = htons(clientPort);
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        std::uint8_t noise[64];
        for (std::size_t b = 0; b < sizeof(noise); ++b)
            noise[b] = static_cast<std::uint8_t>(b * 37u + 11u);
        for (int repeat = 0; repeat < 8; ++repeat)
            (void)::sendto(stranger, noise, sizeof(noise), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));
        ::close(stranger);
        didInject = true;
    }

    int rc = 1;
    for (int i = 0; i < 500; ++i) { // isDaemonReady to ~5s
        if (done.load()) {
            rc = 0;
            break;
        }
        if (dropped.load())
            break;
        ::usleep(10 * 1000);
    }
    // The filter is only proven by it firing: garbage from a stranger does not
    // end a QUIC connection, since an unrecognized packet is ignored by design.
    // What the injection costs an unfiltered client is the parse and the
    // decryption attempt, and what shows that is the refusal count.
    const std::uint64_t refused = Ubiquitty::Transport::LocalEndpointAccess::foreignDatagramsDropped(client);
    if (didInject && refused == 0) {
        std::fprintf(stderr, "quic_client_relay: the stranger's datagrams reached the parser (0 refused)\n");
        rc = 1;
    }
    if (rcResolution != 0)
        rc = 1;
    if (!didInject)
        std::fprintf(stderr, "quic_client_relay: the stranger's datagram was never sent, so the source filter is untested\n");
    if (rc != 0 && dropped.load())
        std::fprintf(stderr, "quic_client_relay: the session ended — a datagram from a foreign source reached the parser\n");
    client.stop();

    // Negative: a never-minted token must be rejected — the daemon closes the
    // connection before admission, so no Welcome or checkpoint ever arrives.
    if (rc == 0) {
        std::mutex mutex2;
        std::string received2;
        std::atomic<bool> accepted2 { false };
        Ubiquitty::Transport::QUICClient bogus;
        Ubiquitty::Transport::QUICClient::Callbacks cb2;
        cb2.onConnect = [&bogus] {
            std::vector<std::uint8_t> frame = encodeHelloFrame(80, 24);
            bogus.send(frame);
        };
        cb2.onData = [&](std::span<const std::uint8_t> bytes) {
            bool a = false, s = false;
            {
                std::lock_guard<std::mutex> lock(mutex2);
                received2.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                if (!scanFrames(received2, a, s))
                    accepted2.store(true);
            }
            if (a || s)
                accepted2.store(true);
        };
        cb2.onClose = [](Ubiquitty::Transport::Health, const std::string&) { };
        std::string bogusToken(32, '\x00'); // 32 zero bytes — never minted
        (void)bogus.start("127.0.0.1", port, Ubiquitty::Transport::BootstrapToken { .token = bogusToken }, certSHA256, cb2);
        for (int i = 0; i < 200 && !accepted2.load(); ++i) // ~2s
            ::usleep(10 * 1000);
        bogus.stop();
        if (accepted2.load()) {
            std::fprintf(stderr, "quic_client_relay: FAIL — a bogus token was accepted\n");
            rc = 1;
        }
    }

    // Negative 2 (MITM defense): a WRONG cert pin is rejected before the token is
    // ever sent, even with a freshly-minted valid token.
    if (rc == 0) {
        std::string token2, cert2;
        std::uint16_t mintedPort2 = 0;
        std::atomic<bool> accepted3 { false };
        if (mintQUICTokenViaBootstrap(hostBin, macHome, token2, mintedPort2, cert2)) {
            std::mutex mutex3;
            std::string received3;
            Ubiquitty::Transport::QUICClient pinned;
            Ubiquitty::Transport::QUICClient::Callbacks cb3;
            cb3.onConnect = [&pinned] {
                std::vector<std::uint8_t> frame = encodeHelloFrame(80, 24);
                pinned.send(frame);
            };
            cb3.onData = [&](std::span<const std::uint8_t> bytes) {
                bool a = false, s = false;
                {
                    std::lock_guard<std::mutex> lock(mutex3);
                    received3.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                    if (!scanFrames(received3, a, s))
                        accepted3.store(true);
                }
                if (a || s)
                    accepted3.store(true);
            };
            cb3.onClose = [](Ubiquitty::Transport::Health, const std::string&) { };
            std::string wrongPin(32, '\xFF'); // not the daemon's cert
            (void)pinned.start("127.0.0.1", port, Ubiquitty::Transport::BootstrapToken { .token = token2 }, wrongPin, cb3);
            for (int i = 0; i < 200 && !accepted3.load(); ++i)
                ::usleep(10 * 1000);
            pinned.stop();
            if (accepted3.load()) {
                std::fprintf(stderr, "quic_client_relay: FAIL — a wrong cert pin was accepted\n");
                rc = 1;
            }
        }
    }

    // Negative 3 (defense in depth): start() itself must REFUSE a non-empty token
    // over an unpinned carrier (pin size != 32) and never spawn the I/O thread — a
    // bearer token without a cert pin is MITM-stealable. Pure local check, no daemon.
    if (rc == 0) {
        Ubiquitty::Transport::QUICClient unpinned;
        Ubiquitty::Transport::QUICClient::Callbacks cb4; // must not fire — start() returns false first
        if (unpinned.start("127.0.0.1", port, Ubiquitty::Transport::BootstrapToken { .token = token }, std::string(), cb4)) {
            std::fprintf(stderr, "quic_client_relay: FAIL — token accepted with no cert pin\n");
            unpinned.stop();
            rc = 1;
        }
    }

    if (rc == 0)
        std::printf("quic_client_relay: token-authenticated Greet over QUIC receives Welcome, PaneBind and a checkpoint; "
                    "bogus token, wrong cert pin, AND unpinned token all rejected\n");
    else
        std::fprintf(stderr, "quic_client_relay: FAIL (done=%d dropped=%d)\n", done.load(), dropped.load());

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
