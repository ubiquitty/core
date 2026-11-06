// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/quic_client_migration.cpp — a QUIC client whose socket dies
// moves its connection to a fresh one and keeps it (Docs/sync.md §6.9), over
// real loopback QUIC against a spawned host.
//
// A client attaches and is sent its pane. Its UDP socket is closed under it,
// as an interface going away does: every send fails. The client opens a
// fresh socket and migrates the connection there. It is not closed, the same
// attachment is still served, with no second Welcome, and input typed after
// the move is written. Told that its interface is gone, it moves again, and
// so does a carrier told through the channel a session holds.
//
// Usage: quic_client_migration <path-to-ubiquitty-host>. Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Transport/QUICChannel.h"
#include "Core/Transport/QUICClient.h"
#include "Host/HostConnection.h"
#include "Host/QUICDial.h"
#include "Host/ScreenClient.h"
#include "Platform/Host/HostSocketName.h"
#include "quic_test_mint.h"

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Sync = Ubiquitty::Protocol::Sync;
namespace Transport = Ubiquitty::Transport;
using UbiquittyTest::Dial;

namespace {

/// The authenticated fresh selection and the first verified pane it bound.
struct PaneReceipt {
    Sync::Welcome welcome;
    std::uint32_t handle = 0;
    Sync::ID128 paneID;
    std::uint64_t generation = 0;
};

/// Selects fresh creation explicitly; the actual bootstrap supplies authority.
///
/// - Returns: A fresh Session request with no invented prior owner pin.
Sync::AdmissionRequest freshSessionRequest()
{
    Sync::AdmissionRequest request;
    request.target = Sync::AdmissionTarget::Session;
    return request;
}

/// Captures a fresh owner's authenticated routing answer and verified picture.
///
/// - Parameters:
///   - client: The actual retained protocol reader, under its owner's lock.
///   - socketName: The daemon basename selected by this fixture before dialing.
/// - Returns: The complete receipt, absent before its first checkpoint.
std::optional<PaneReceipt> freshPaneReceipt(const UbiquittyTest::ScreenClient& client, std::string_view socketName)
{
    if (!client.hasStarted())
        return std::nullopt;
    const auto& connection = client.connection();
    const auto& welcome = connection.welcome();
    if (connection.state() != Sync::ConnectionState::Attached || !welcome
        // A person on QUIC is not the host's local owner: the Owner role it
        // asks for is granted as Driver, its token's ceiling.
        || welcome->admission != Sync::AttachmentAdmission::Granted || welcome->grantedRole != Sync::Role::Driver
        || welcome->selection.target != Sync::AdmissionTarget::Session || welcome->selection.sessionID == 0
        || welcome->selection.workspaceID == 0 || welcome->selection.daemonInstanceID == 0 || welcome->selection.socketName != socketName
        || welcome->hostInstanceID == Sync::ID128 {} || welcome->attachmentID == Sync::ID128 {} || welcome->resumeSecret == Sync::ID128 {}
        || !welcome->resumed.empty() || client.count(Sync::FrameKind::Welcome) != 1)
        return std::nullopt;
    const auto panes = connection.boundPanes();
    if (panes.size() != 1 || panes.front().first == 0 || panes.front().second == Sync::ID128 {})
        return std::nullopt;
    const auto [handle, paneID] = panes.front();
    const std::uint64_t generation = connection.paneGeneration(handle);
    if (!connection.paneTerminal(handle) || generation == 0 || !connection.isPaneCurrent(handle)
        || connection.counters().paneBindings != 1 || connection.counters().checkpointsApplied == 0)
        return std::nullopt;
    return PaneReceipt { .welcome = *welcome, .handle = handle, .paneID = paneID, .generation = generation };
}

/// Checks that a physical path change did not create or re-greet an attachment.
///
/// - Parameters:
///   - client: The same retained reader, under its owner's lock.
///   - before: The authenticated selection captured before the path changed.
/// - Returns: Whether that exact attachment and pane still serve this client.
bool keepsAttachment(const UbiquittyTest::ScreenClient& client, const PaneReceipt& before)
{
    const auto after = freshPaneReceipt(client, before.welcome.selection.socketName);
    if (!after)
        return false;
    const auto& held = before.welcome;
    const auto& current = after->welcome;
    return current.hostInstanceID == held.hostInstanceID && current.attachmentID == held.attachmentID && current.resumeSecret == held.resumeSecret
        && current.selection.sessionID == held.selection.sessionID && current.selection.workspaceID == held.selection.workspaceID
        && current.selection.daemonInstanceID == held.selection.daemonInstanceID
        && after->handle == before.handle && after->paneID == before.paneID && after->generation >= before.generation;
}

/// Types a line and waits for the host to report it written.
///
/// - Parameters:
///   - dial: The existing physical connection and retained protocol reader.
///   - pane: The pane bound before either migration.
///   - line: The bytes to write.
/// - Returns: Whether it was written within five seconds.
bool typeAndAwait(Dial& dial, const PaneReceipt& pane, const std::string& line)
{
    std::optional<std::uint64_t> sent;
    {
        const std::scoped_lock lock(dial.lock);
        if (!keepsAttachment(dial.client, pane))
            return false;
        sent = dial.client.sendRaw(pane.handle, { reinterpret_cast<const std::uint8_t*>(line.data()), line.size() });
    }
    if (!sent)
        return false;
    return dial.pumpUntil(5'000, [&](const UbiquittyTest::ScreenClient& client) {
        return keepsAttachment(client, pane) && client.connection().receipt(pane.handle).inputWritten >= *sent;
    });
}

/// Retains one production protocol reader for the channel's entire lifetime.
struct Greeter final : Transport::ChannelObserver {
    Transport::QUICChannel* channel = nullptr;
    UbiquittyTest::ScreenClient client { 24, 80 };
    std::mutex lock;
    std::uint32_t connectCount = 0;
    bool hasClosed = false;

    void onConnect() override
    {
        std::vector<std::uint8_t> upstream;
        {
            const std::scoped_lock held(lock);
            ++connectCount;
            // A path change must not emit another Greet, even if the carrier
            // incorrectly repeats its connection callback.
            if (connectCount == 1 && client.start(freshSessionRequest()))
                upstream = client.takeUpstream(UbiquittyTest::nowMicroseconds());
        }
        if (!upstream.empty())
            channel->send(upstream);
    }

    void onData(std::span<const std::uint8_t> bytes) override
    {
        std::vector<std::uint8_t> upstream;
        {
            const std::scoped_lock held(lock);
            client.read(bytes, UbiquittyTest::nowMicroseconds());
            upstream = client.takeUpstream(UbiquittyTest::nowMicroseconds());
        }
        if (!upstream.empty())
            channel->send(upstream);
    }

    void onClose(const std::string&) override
    {
        const std::scoped_lock held(lock);
        hasClosed = true;
    }

    /// Advances the retained reader without holding its lock across send.
    void pump()
    {
        std::vector<std::uint8_t> upstream;
        {
            const std::scoped_lock held(lock);
            if (client.hasStarted() && !hasClosed)
                upstream = client.takeUpstream(UbiquittyTest::nowMicroseconds());
        }
        if (!upstream.empty())
            channel->send(upstream);
    }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-ubiquitty-host>\n", argv[0]);
        return 2;
    }
    const char* hostBinary = argv[1];
    char homeTemplate[] = "/tmp/ubiquitty-quic-migration.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "quic_client_migration: mkdtemp failed\n");
        return 1;
    }
    const std::string controlDirectory = std::string(home) + "/.ubiquitty";
    const std::string socketName = Ubiquitty::Host::socketLeafForBinary(hostBinary);
    const std::uint16_t port = UbiquittyTest::freeUDPPort();
    const std::string portArgument = std::to_string(port);
    const pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", controlDirectory.c_str(), 1);
        ::execl(hostBinary, hostBinary, "--daemon", "--quic-port", portArgument.c_str(), "--columns", "80", "--rows", "24", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    const auto stopDaemon = [&] {
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, 0);
    };
    if (!Ubiquitty::HostTest::waitForDaemonReady(controlDirectory + "/" + socketName)) {
        std::fprintf(stderr, "quic_client_migration: premise: the daemon never accepted a connection\n");
        stopDaemon();
        return 1;
    }
    std::string token;
    std::string pin;
    std::uint16_t mintedPort = 0;
    if (!mintQUICTokenViaBootstrap(hostBinary, controlDirectory, token, mintedPort, pin) || pin.size() != 32 || mintedPort != port) {
        std::fprintf(stderr, "quic_client_migration: premise: no QUIC token or certificate pin was minted\n");
        stopDaemon();
        return 1;
    }

    [&] {
        Sync::ReplicaSet replicas;
        Dial dial(replicas, std::nullopt);
        // No prior session or daemon identity exists for fresh creation. The
        // real, locally minted bootstrap authorizes this explicit request.
        REQUIRE_MSG(dial.start(port, Transport::BootstrapToken { .token = token }, pin, freshSessionRequest()), "premise: the dial did not start");
        const bool wasSentThePane = dial.pumpUntil(10'000, [&](const UbiquittyTest::ScreenClient& client) { return freshPaneReceipt(client, socketName).has_value(); });
        REQUIRE_MSG(wasSentThePane, "premise: no granted owner selection and verified pane arrived");
        std::optional<PaneReceipt> before;
        {
            const std::scoped_lock held(dial.lock);
            before = freshPaneReceipt(dial.client, socketName);
        }
        REQUIRE(before);
        REQUIRE_MSG(typeAndAwait(dial, *before, "true\r"), "premise: input was not written before the move");
        const std::uint16_t portBefore = Transport::LocalEndpointAccess::localPort(dial.carrier);
        CHECK_MSG(Transport::LocalEndpointAccess::migrations(dial.carrier) == 0, "premise: the carrier moved before anything happened to its path");

        // The socket goes under the connection.
        Transport::LocalEndpointAccess::closeSocketForTest(dial.carrier);
        // Something to send, so the dead socket is found.
        (void)typeAndAwait(dial, *before, "true\r");
        const bool didMove = dial.pumpUntil(5'000, [&](const UbiquittyTest::ScreenClient&) { return Transport::LocalEndpointAccess::migrations(dial.carrier) >= 1; });
        CHECK_MSG(didMove, "the carrier did not move to a fresh socket when its own died");
        CHECK_MSG(Transport::LocalEndpointAccess::migrations(dial.carrier) == 1 && !dial.hasClosed.load(), "the carrier moved more than once, or closed the connection");
        CHECK_MSG(Transport::LocalEndpointAccess::localPort(dial.carrier) != portBefore, "the carrier is on the socket it had");

        // The same connection carries on: input typed after the move is
        // written, and the host greeted nothing anew.
        CHECK_MSG(typeAndAwait(dial, *before, "true\r"), "input typed after the move was not written");
        CHECK_MSG(!dial.hasClosed.load(), "the connection closed after the move");
        {
            const std::scoped_lock held(dial.lock);
            CHECK_MSG(keepsAttachment(dial.client, *before), "the move changed the owner, pane or attachment, or sent a second Welcome");
        }

        // The system says the interface is gone: the carrier moves again,
        // without waiting to find out by silence.
        dial.carrier.repath();
        const bool didMoveAgain = dial.pumpUntil(5'000, [&](const UbiquittyTest::ScreenClient&) { return Transport::LocalEndpointAccess::migrations(dial.carrier) >= 2; });
        CHECK_MSG(didMoveAgain, "the carrier did not move when told its interface was gone");
        CHECK_MSG(typeAndAwait(dial, *before, "true\r"), "input typed after the second move was not written");
        CHECK_MSG(Transport::LocalEndpointAccess::migrations(dial.carrier) == 2 && !dial.hasClosed.load(), "the carrier moved again unasked, or closed the connection");
        {
            const std::scoped_lock held(dial.lock);
            CHECK_MSG(keepsAttachment(dial.client, *before), "the second move changed the attachment or repeated Welcome");
        }
        dial.carrier.stop();
    }();

    // The same ask through the channel a session holds: it reaches the
    // carrier, which moves.
    std::string channelToken;
    std::string channelPin;
    if (mintQUICTokenViaBootstrap(hostBinary, controlDirectory, channelToken, mintedPort, channelPin) && mintedPort == port && channelPin == pin) {
        [&] {
            // Destruction joins channel callbacks before the borrowed reader
            // dies, including an early failed premise below.
            Greeter greeter;
            Transport::QUICChannel channel("127.0.0.1", port, channelToken, channelPin, Transport::Origin { Transport::OriginKind::Daemon, "" });
            greeter.channel = &channel;
            REQUIRE_MSG(channel.start(&greeter), "premise: the channel did not start");
            const auto migrationsOf = [&]() -> std::optional<std::uint64_t> {
                Transport::TransportStats stats;
                return channel.queryStats(stats) ? std::optional { stats.migrations } : std::nullopt;
            };
            const auto waitFor = [&](std::uint64_t wanted, const std::optional<PaneReceipt>& before) {
                for (int tries = 0; tries < 500; ++tries) {
                    greeter.pump();
                    if (migrationsOf() == std::optional { wanted }) {
                        const std::scoped_lock held(greeter.lock);
                        if (!greeter.hasClosed && greeter.connectCount == 1
                            && (before ? keepsAttachment(greeter.client, *before) : freshPaneReceipt(greeter.client, socketName).has_value()))
                            return true;
                    }
                    ::usleep(10'000);
                }
                return false;
            };
            REQUIRE_MSG(waitFor(0, std::nullopt), "premise: the channel never admitted its fresh owner and verified pane");
            std::optional<PaneReceipt> before;
            {
                const std::scoped_lock held(greeter.lock);
                before = freshPaneReceipt(greeter.client, socketName);
            }
            REQUIRE(before);
            channel.repath();
            CHECK_MSG(waitFor(1, before), "a channel told its interface was gone did not keep its exact attachment on the moved carrier");
            channel.stop();
        }();
    } else {
        CHECK_MSG(false, "premise: no second QUIC token was minted");
    }

    stopDaemon();
    if (failures == 0)
        std::printf("quic_client_migration: a client whose socket died moved its connection to a fresh one and kept it, and moved again when told its interface was gone\n");
    return failures == 0 ? 0 : 1;
}
