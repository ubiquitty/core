// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Helpers/TempDir.h"

#include "Core/Protocol/Sync/HandoverBody.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HandoverStartup.h"
#include "Platform/Host/HostHandover.h"
#include "Platform/Host/HostSocketName.h"
#include "Platform/POSIX/HandoverTransport.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Host = Ubiquitty::Host;
namespace PAL = Ubiquitty::PAL;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace Ubiquitty::Host {

struct HandoverAccess {
    static void addClient(Daemon& daemon, PAL::UnixFileDescriptor channel)
    {
        daemon.m_clients.emplace_back().fileDescriptor = std::move(channel);
    }
    static void greet(Daemon& daemon) { daemon.m_clients[0].helloSeen = true; }
    static int channel(const Daemon& daemon) { return daemon.m_clients[0].fileDescriptor.get(); }
    static bool hasChannel(const Daemon& daemon) { return !!daemon.m_clients[0].fileDescriptor; }
    static bool isDropped(const Daemon& daemon) { return daemon.m_clients[0].didOverflow; }
    static bool hasDecoderBytes(const Daemon& daemon)
    {
        const auto& connection = daemon.m_clients[0].syncConnection;
        return connection && connection->pendingInboundFrameBytes();
    }
    static bool isQuiescing(const Daemon& daemon) { return daemon.m_quiescingPanes.has_value(); }
    static const std::string& installPath(const Daemon& daemon) { return daemon.m_handoverInstallPath; }
    static void setSocketPath(Daemon& daemon, std::string path) { daemon.m_socketPath = std::move(path); }
    static void addOwedClient(Daemon& daemon, PAL::UnixFileDescriptor channel, std::size_t owedBytes)
    {
        auto& client = daemon.m_clients.emplace_back();
        client.fileDescriptor = std::move(channel);
        client.helloSeen = true;
        client.outputBuffer.assign(owedBytes, 0x55);
    }
    static void payOwed(Daemon& daemon, std::size_t index) { daemon.m_clients[index].clearPendingOutput(); }
    static bool isHandoverPending(const Daemon& daemon) { return daemon.m_pendingHandover.has_value(); }
};

/// The transactions the routing cases start. Only a case that admits a real
/// successor expects one; every other case must stop before it.
struct OfferLedger {
    std::size_t calls = 0;
    bool isExpected = false;
};

OfferLedger& offerLedger()
{
    static OfferLedger ledger;
    return ledger;
}

bool HostHandover::offer(Daemon&, PAL::UnixFileDescriptor channel, AdmittedHandoverRequest)
{
    auto& ledger = offerLedger();
    CHECK(ledger.isExpected);
    ++ledger.calls;
    // Lets the successor process the case started end.
    const std::array<std::uint8_t, 1> done { 1 };
    (void)Ubiquitty::POSIX::sendHandoverRecord(channel.get(), done, {}, 1000);
    return false;
}

std::expected<std::size_t, Sync::HandoverRefuseReason> HostHandover::inherit(
    Daemon&, PAL::UnixFileDescriptor, std::string_view, std::uint32_t, std::chrono::steady_clock::time_point)
{
    CHECK(false);
    return std::unexpected(Sync::HandoverRefuseReason::Malformed);
}

} // namespace Ubiquitty::Host

namespace {

struct Fixture {
    PAL::UnixFileDescriptor peer;
    Host::Daemon daemon;

    Fixture()
    {
        int channels[2] { -1, -1 };
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, channels) == 0);
        Host::HandoverAccess::addClient(daemon, PAL::UnixFileDescriptor(channels[0]));
        peer.reset(channels[1]);
    }

    bool route(std::uint64_t now = 1) { return Host::HandoverStartup::route(daemon, 0, now, 100); }

    void send(std::span<const std::uint8_t> bytes)
    {
        REQUIRE(::send(peer.get(), bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()));
    }

    void expectRefusal(Sync::HandoverRefuseReason reason)
    {
        auto record = Ubiquitty::POSIX::receiveHandoverRecord(peer.get(), 2, 0, 1000);
        REQUIRE(record);
        auto decoded = Sync::decodeHandoverMessage(record->bytes(), {});
        REQUIRE(decoded);
        const auto* refusal = std::get_if<Sync::HandoverRefuse>(&*decoded);
        REQUIRE(refusal);
        CHECK(refusal->reason == reason);
        CHECK(Host::HandoverAccess::isDropped(daemon));
        CHECK(!Host::HandoverAccess::hasChannel(daemon));
        CHECK(!Host::HandoverAccess::isQuiescing(daemon));
        CHECK(!Host::HandoverAccess::hasDecoderBytes(daemon));
    }
};

} // namespace

TEST(theOriginalInstallPathCannotBeRetargeted)
{
    Host::Daemon daemon;
    Host::HandoverStartup::initialize(daemon, "");
    Host::HandoverStartup::initialize(daemon, "relative/host");
    Host::HandoverStartup::initialize(daemon, std::string_view("/tmp/host\0other", 15));
    CHECK(Host::HandoverAccess::installPath(daemon).empty());
    Host::HandoverStartup::initialize(daemon, "/tmp/ubiquitty-host (deleted)");
    CHECK_STR_EQ(Host::HandoverAccess::installPath(daemon), "/tmp/ubiquitty-host");
    Host::HandoverStartup::initialize(daemon, "/tmp/another-host");
    CHECK_STR_EQ(Host::HandoverAccess::installPath(daemon), "/tmp/ubiquitty-host");
}

TEST(anOrdinaryNewClientKeepsItsBytesForTheOrdinaryDecoder)
{
    Fixture fixture;
    const std::array<std::uint8_t, 3> bytes { 1, 2, 3 };
    fixture.send(bytes);
    CHECK(fixture.route());
    std::array<std::uint8_t, 3> received {};
    REQUIRE(::recv(Host::HandoverAccess::channel(fixture.daemon), received.data(), received.size(), MSG_DONTWAIT) == 3);
    CHECK(received == bytes);
    CHECK(Host::HandoverAccess::hasChannel(fixture.daemon));
    CHECK(!Host::HandoverAccess::isDropped(fixture.daemon));
    CHECK(!Host::HandoverAccess::hasDecoderBytes(fixture.daemon));
    CHECK(!Host::HandoverAccess::isQuiescing(fixture.daemon));
}

TEST(aGreetedClientCannotStartAHandover)
{
    Fixture fixture;
    Host::HandoverAccess::greet(fixture.daemon);
    fixture.send(Sync::handoverPreamble);
    CHECK(fixture.route());
    CHECK(Host::HandoverAccess::hasChannel(fixture.daemon));
    CHECK(!Host::HandoverAccess::isDropped(fixture.daemon));
    CHECK(!Host::HandoverAccess::isQuiescing(fixture.daemon));
}

TEST(aMalformedPrefixIsRefusedBeforeTheOrdinaryDecoderOrQuiesce)
{
    Fixture fixture;
    auto prefix = Sync::handoverPreamble;
    prefix[3] ^= 1;
    fixture.send(prefix);
    CHECK(!fixture.route());
    fixture.expectRefusal(Sync::HandoverRefuseReason::Malformed);
}

TEST(aStalledPrefixUsesTheOriginalAdmissionDeadline)
{
    Fixture fixture;
    fixture.send(std::span { Sync::handoverPreamble }.first(1));
    CHECK(!fixture.route());
    CHECK(Host::HandoverAccess::hasChannel(fixture.daemon));
    CHECK(!Host::HandoverAccess::isDropped(fixture.daemon));
    CHECK(!fixture.route(100));
    fixture.expectRefusal(Sync::HandoverRefuseReason::Malformed);
}

TEST(aRequestFromThisProcessNeverReachesTheOwnershipTransaction)
{
    Fixture fixture;
    Host::HandoverStartup::initialize(fixture.daemon, "/tmp/ubiquitty-host");
    fixture.send(Sync::handoverPreamble);
    auto descriptor = PAL::makeUnixPipe();
    REQUIRE(descriptor);
    const std::array descriptors { descriptor.read.get() };
    const auto request = Sync::encodeHandoverMessage(Sync::HandoverRequest { Sync::handoverBodyVersion, { 1, 2 } });
    REQUIRE(Ubiquitty::POSIX::sendHandoverRecord(fixture.peer.get(), request, descriptors, 1000));
    CHECK(!fixture.route());
    fixture.expectRefusal(Sync::HandoverRefuseReason::NotSuccessor);
}

namespace {

PAL::UnixFileDescriptor listenAt(const std::string& path)
{
    sockaddr_un address {};
    if (path.size() >= sizeof(address.sun_path))
        return {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    PAL::UnixFileDescriptor descriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!descriptor || ::bind(descriptor.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || ::listen(descriptor.get(), 4) != 0)
        return {};
    return descriptor;
}

/// Asks the holder at `holderPath` to hand over, as a starting host does, with
/// a listener bound at `listenerPath`, and waits for whatever answers it.
/// - Returns: The child's exit status.
int askToHandOver(const std::string& holderPath, const std::string& listenerPath)
{
    auto listener = listenAt(listenerPath);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, holderPath.c_str(), holderPath.size() + 1);
    PAL::UnixFileDescriptor channel(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!listener || !channel || ::connect(channel.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        return 1;
    if (::send(channel.get(), Sync::handoverPreamble.data(), Sync::handoverPreamble.size(), 0) != static_cast<ssize_t>(Sync::handoverPreamble.size()))
        return 2;
    const auto request = Sync::encodeHandoverMessage(Sync::HandoverRequest { Sync::handoverBodyVersion, { 17, 29 } });
    const std::array descriptors { listener.get() };
    if (!Ubiquitty::POSIX::sendHandoverRecord(channel.get(), request, descriptors, 1000))
        return 3;
    return Ubiquitty::POSIX::receiveHandoverRecord(channel.get(), 8, 0, 10'000) ? 0 : 4;
}

/// A real successor: this test's own image linked at an install path, in a
/// child process that binds the socket its image names and asks over its own
/// connection, so admission takes it as the running successor.
struct SuccessorPeer {
    std::string executable = Host::resolveSelfExecutablePath();
    Ubiquitty::Test::TempDir installed { std::filesystem::path(executable).parent_path().string() + "/startup.XXXXXX" };
    Ubiquitty::Test::TempDir control { "/tmp/ubq-startup.XXXXXX" };
    std::string installPath = installed.path() + "/successor";
    PAL::UnixFileDescriptor channel;
    pid_t process = -1;

    SuccessorPeer()
    {
        CHECK_EQ(::link(executable.c_str(), installPath.c_str()), 0);
        const std::string holderPath = control.path() + "/holder.sock";
        auto holder = listenAt(holderPath);
        CHECK(holder);
        process = holder ? ::fork() : -1;
        if (process == 0) {
            holder.reset();
            ::_exit(askToHandOver(holderPath, control.path() + "/" + Host::socketLeafForBinary(executable.c_str())));
        }
        pollfd ready { holder.get(), POLLIN, 0 };
        if (process > 0 && ::poll(&ready, 1, 5000) == 1)
            channel.reset(::accept(holder.get(), nullptr, nullptr));
        CHECK(channel);
    }
    SuccessorPeer(const SuccessorPeer&) = delete;
    SuccessorPeer& operator=(const SuccessorPeer&) = delete;
    ~SuccessorPeer() { (void)join(); }

    /// - Returns: The child's exit status, or -1 once joined.
    int join()
    {
        if (process <= 0)
            return -1;
        int status = 0;
        pid_t result;
        do {
            result = ::waitpid(process, &status, 0);
        } while (result < 0 && errno == EINTR);
        process = -1;
        return result > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
};

/// A holder of the peer's install with its request on client 0 and a second
/// client owed bytes it has not yet been sent.
struct OwingHolder {
    Host::Daemon daemon;

    explicit OwingHolder(SuccessorPeer& peer)
    {
        Host::HandoverAccess::setSocketPath(daemon, peer.control.path() + "/host-0123456789abcdef.sock");
        Host::HandoverStartup::initialize(daemon, peer.installPath);
        Host::HandoverAccess::addClient(daemon, std::move(peer.channel));
        int channels[2] { -1, -1 };
        CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, channels), 0);
        reader.reset(channels[1]);
        Host::HandoverAccess::addOwedClient(daemon, PAL::UnixFileDescriptor(channels[0]), 4096);
    }

    /// Routes the request until it has been read and admitted or refused.
    void routeRequest()
    {
        for (int attempt = 0; attempt < 5000 && Host::HandoverAccess::hasChannel(daemon); ++attempt) {
            (void)Host::HandoverStartup::route(daemon, 0, 1, 10'000);
            ::usleep(1000);
        }
        CHECK(!Host::HandoverAccess::hasChannel(daemon));
    }

    PAL::UnixFileDescriptor reader;
};

} // namespace

TEST(anAdmittedRequestWaitsUntilTheHostOwesItsClientsNothing)
{
    SuccessorPeer peer;
    REQUIRE(peer.channel);
    OwingHolder holder(peer);
    const std::size_t before = Host::offerLedger().calls;
    holder.routeRequest();
    // A client owed bytes does not refuse the request; it waits.
    CHECK_EQ(Host::offerLedger().calls, before);
    CHECK(Host::HandoverAccess::isHandoverPending(holder.daemon));
    Host::HandoverStartup::servicePending(holder.daemon, 2);
    CHECK_EQ(Host::offerLedger().calls, before);
    // Once the client has been sent what it was owed, the transaction starts.
    Host::HandoverAccess::payOwed(holder.daemon, 1);
    Host::offerLedger().isExpected = true;
    Host::HandoverStartup::servicePending(holder.daemon, 3);
    Host::offerLedger().isExpected = false;
    CHECK_EQ(Host::offerLedger().calls, before + 1);
    CHECK(!Host::HandoverAccess::isHandoverPending(holder.daemon));
    CHECK_EQ(peer.join(), 0);
}

TEST(aClientThatTakesNothingHoldsTheRequestOnlyForItsWait)
{
    SuccessorPeer peer;
    REQUIRE(peer.channel);
    OwingHolder holder(peer);
    const std::size_t before = Host::offerLedger().calls;
    holder.routeRequest();
    Host::HandoverStartup::servicePending(holder.daemon, Host::HostHandover::maxOwedOutputWaitMilliseconds);
    CHECK_EQ(Host::offerLedger().calls, before);
    CHECK(Host::HandoverAccess::isHandoverPending(holder.daemon));
    // At the end of the wait the transaction starts, and refuses with what is
    // still owed.
    Host::offerLedger().isExpected = true;
    Host::HandoverStartup::servicePending(holder.daemon, 1 + Host::HostHandover::maxOwedOutputWaitMilliseconds);
    Host::offerLedger().isExpected = false;
    CHECK_EQ(Host::offerLedger().calls, before + 1);
    CHECK(!Host::HandoverAccess::isHandoverPending(holder.daemon));
    CHECK_EQ(peer.join(), 0);
}

TEST(anUnstartedDaemonNeverWalksTheReadersHostDirectory)
{
    Host::Daemon daemon;
    Host::HandoverStartup::initialize(daemon, "/tmp/ubiquitty-host");
    const auto report = Host::HandoverStartup::takeOverSiblings(daemon, 1000);
    CHECK_EQ(report.movedSessions, 0u);
    CHECK_EQ(report.refusedPredecessors, 0u);
    CHECK(!Host::HandoverStartup::route(daemon, 0, 1, 100));
}

UBIQUITTY_TEST_MAIN()
