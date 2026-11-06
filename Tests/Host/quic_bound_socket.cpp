// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Helpers/TempDir.h"
#include "Platform/Host/HostQUIC.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

using Ubiquitty::Host::QUICListener;
using Ubiquitty::PAL::UnixFileDescriptor;

namespace {

struct PrivateHome {
    Ubiquitty::Test::TempDir directory { "/tmp/ubiquitty-quic-bound.XXXXXX" };
    std::optional<std::string> previous;

    PrivateHome()
    {
        if (const auto* value = std::getenv("UBIQUITTY_HOME"))
            previous = value;
        if (directory.valid())
            ::setenv("UBIQUITTY_HOME", directory.path().c_str(), 1);
    }

    ~PrivateHome()
    {
        if (previous)
            ::setenv("UBIQUITTY_HOME", previous->c_str(), 1);
        else
            ::unsetenv("UBIQUITTY_HOME");
    }
};

UnixFileDescriptor boundSocket(int family = AF_INET, int type = SOCK_DGRAM)
{
    UnixFileDescriptor socket(::socket(family, type, 0));
    if (!socket)
        return {};
    if (family == AF_INET6) {
        sockaddr_in6 address {};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_loopback;
        if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)))
            return {};
    } else {
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)))
            return {};
    }
    return socket;
}

std::uint16_t portOf(int socket)
{
    sockaddr_storage address {};
    socklen_t length = sizeof(address);
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length))
        return 0;
    if (address.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
}

bool isClosed(int socket)
{
    return ::fcntl(socket, F_GETFD) == -1 && errno == EBADF;
}

} // namespace

TEST(certificateIsMadeWithoutRunningAProgramFromPath)
{
    // An `openssl` first on PATH that leaves a mark: the daemon's QUIC key is
    // made by the linked TLS library, so nothing on PATH runs, and the key is
    // owner-only.
    PrivateHome home;
    REQUIRE(home.directory.valid());
    const std::string directory = home.directory.path();
    const std::string bin = directory + "/bin";
    const std::string mark = directory + "/stand-in-ran";
    REQUIRE(::mkdir(bin.c_str(), 0700) == 0);
    {
        std::ofstream standIn(bin + "/openssl");
        standIn << "#!/bin/sh\n: > '" << mark << "'\nexit 1\n";
    }
    REQUIRE(::chmod((bin + "/openssl").c_str(), 0700) == 0);
    std::optional<std::string> previousPath;
    if (const auto* value = std::getenv("PATH"))
        previousPath = value;
    ::setenv("PATH", (bin + ":" + previousPath.value_or("/usr/bin:/bin")).c_str(), 1);
    QUICListener listener;
    const bool didStart = listener.start(0);
    if (previousPath)
        ::setenv("PATH", previousPath->c_str(), 1);
    else
        ::unsetenv("PATH");
    CHECK(didStart);
    CHECK(::access(mark.c_str(), F_OK) != 0);
    CHECK_EQ(listener.certificateSHA256().size(), 32u);
    struct stat key {};
    REQUIRE(::stat((directory + "/quic-key.pem").c_str(), &key) == 0);
    CHECK_EQ(static_cast<unsigned>(key.st_mode & 0777), 0600u);
    listener.stop();
}

TEST(boundSocketHandoffKeepsDescriptorPortCertificateAndQueuedDatagram)
{
    PrivateHome home;
    REQUIRE(home.directory.valid());
    QUICListener successor;
    std::uint16_t port = 0;
    int descriptor = -1;
    {
        QUICListener predecessor;
        auto socket = boundSocket();
        REQUIRE(socket);
        descriptor = socket.get();
        port = portOf(descriptor);
        REQUIRE(port);
        REQUIRE(predecessor.startOnBoundSocket(std::move(socket)));
        CHECK(!socket);
        CHECK_EQ(predecessor.fileDescriptor(), descriptor);
        CHECK_EQ(predecessor.boundPort(), port);
        const auto certificate = predecessor.certificateSHA256();
        REQUIRE(certificate.size() == 32);

        UnixFileDescriptor sender(::socket(AF_INET, SOCK_DGRAM, 0));
        REQUIRE(sender);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        const std::array<std::uint8_t, 3> bytes { 1, 2, 3 };
        REQUIRE(::sendto(sender.get(), bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 3);

        auto transferred = predecessor.releaseBoundSocket();
        REQUIRE(transferred);
        CHECK_EQ(transferred.get(), descriptor);
        CHECK_EQ(predecessor.fileDescriptor(), -1);
        CHECK_EQ(predecessor.boundPort(), 0);
        predecessor.stop();
        CHECK(!isClosed(descriptor));
        REQUIRE(successor.startOnBoundSocket(std::move(transferred)));
        CHECK(successor.certificateSHA256() == certificate);
    }
    CHECK_EQ(successor.fileDescriptor(), descriptor);
    CHECK_EQ(successor.boundPort(), port);
    CHECK(!isClosed(descriptor));
    std::array<std::uint8_t, 3> received {};
    REQUIRE(::recv(descriptor, received.data(), received.size(), 0) == 3);
    CHECK((received == std::array<std::uint8_t, 3> { 1, 2, 3 }));
    successor.stop();
    CHECK(isClosed(descriptor));
}

TEST(boundSocketAdoptionSetsNonblockingAndCloseOnExecForBothFamilies)
{
    PrivateHome home;
    REQUIRE(home.directory.valid());
    for (const int family : { AF_INET, AF_INET6 }) {
        auto socket = boundSocket(family);
        REQUIRE(socket);
        REQUIRE(::fcntl(socket.get(), F_SETFL, 0) == 0);
        REQUIRE(::fcntl(socket.get(), F_SETFD, 0) == 0);
        const auto port = portOf(socket.get());
        QUICListener listener;
        REQUIRE(listener.startOnBoundSocket(std::move(socket)));
        CHECK_EQ(listener.boundPort(), port);
        CHECK(::fcntl(listener.fileDescriptor(), F_GETFL) & O_NONBLOCK);
        CHECK(::fcntl(listener.fileDescriptor(), F_GETFD) & FD_CLOEXEC);
    }
}

TEST(readableTurnYieldsWithEmptyAndMalformedDatagramsStillQueued)
{
    PrivateHome home;
    REQUIRE(home.directory.valid());
    for (const std::size_t datagramBytes : { 0u, 1u }) {
        auto socket = boundSocket();
        REQUIRE(socket);
        const int receiveBufferBytes = 1024 * 1024;
        REQUIRE(::setsockopt(socket.get(), SOL_SOCKET, SO_RCVBUF, &receiveBufferBytes, sizeof(receiveBufferBytes)) == 0);
        QUICListener listener;
        REQUIRE(listener.startOnBoundSocket(std::move(socket)));
        UnixFileDescriptor sender(::socket(AF_INET, SOCK_DGRAM, 0));
        REQUIRE(sender);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(listener.boundPort());
        const std::array<std::uint8_t, 1> bytes { 0 };
        // Preloaded on loopback: no sender thread or timing window is needed
        // to keep input queued across two complete receive budgets.
        for (std::size_t index = 0; index < 2 * Ubiquitty::Host::quicReceivePacketBudget + 1; ++index)
            REQUIRE(::sendto(sender.get(), bytes.data(), datagramBytes, 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == static_cast<ssize_t>(datagramBytes));

        std::array<std::uint8_t, 1> pending {};
        for (int turn = 0; turn < 2; ++turn) {
            listener.processReadable();
            CHECK_EQ(::recv(listener.fileDescriptor(), pending.data(), pending.size(), MSG_PEEK | MSG_DONTWAIT), static_cast<ssize_t>(datagramBytes));
            CHECK_EQ(Ubiquitty::Host::testQUICConnectionCount(listener), 0u);
        }
        listener.processReadable();
        CHECK_EQ(::recv(listener.fileDescriptor(), pending.data(), pending.size(), MSG_PEEK | MSG_DONTWAIT), -1);
        CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

TEST(boundSocketReleaseRefusesUntilConnectionsAreExplicitlyRetired)
{
    PrivateHome home;
    REQUIRE(home.directory.valid());
    QUICListener listener;
    REQUIRE(listener.start(0));
    const auto descriptor = listener.fileDescriptor();
    const auto port = listener.boundPort();
    auto* connection = Ubiquitty::Host::adoptTestQUICConnection(listener, Ubiquitty::Host::makeTestQUICConnection());
    REQUIRE(connection);
    CHECK(!listener.releaseBoundSocket());
    CHECK_EQ(listener.fileDescriptor(), descriptor);
    CHECK_EQ(listener.boundPort(), port);
    CHECK_EQ(Ubiquitty::Host::testQUICConnectionCount(listener), 1u);
    listener.closeConnection(connection);
    auto socket = listener.releaseBoundSocket();
    REQUIRE(socket);
    CHECK_EQ(socket.get(), descriptor);
    listener.flush();
    listener.serviceTimers();
    CHECK_EQ(listener.nextTimeoutMilliseconds(), -1);
    CHECK(!isClosed(descriptor));
    CHECK(!listener.releaseBoundSocket());
}

TEST(boundSocketAdoptionRefusesInvalidUnboundStreamAndUnixSockets)
{
    PrivateHome home;
    REQUIRE(home.directory.valid());
    QUICListener listener;
    CHECK(!listener.startOnBoundSocket({}));
    for (auto& socket : std::array<UnixFileDescriptor, 4> {
             UnixFileDescriptor(::socket(AF_INET, SOCK_DGRAM, 0)),
             boundSocket(AF_INET, SOCK_STREAM),
             UnixFileDescriptor(::socket(AF_UNIX, SOCK_DGRAM, 0)),
             UnixFileDescriptor(::open("/dev/null", O_RDONLY)) }) {
        REQUIRE(socket);
        const int descriptor = socket.get();
        CHECK(!listener.startOnBoundSocket(std::move(socket)));
        CHECK(isClosed(descriptor));
        CHECK_EQ(listener.fileDescriptor(), -1);
        CHECK_EQ(listener.boundPort(), 0);
    }
}

TEST(boundSocketAdoptionRefusesConnectedUDP)
{
    PrivateHome home;
    REQUIRE(home.directory.valid());
    auto peer = boundSocket();
    auto socket = boundSocket();
    REQUIRE(peer && socket);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(portOf(peer.get()));
    REQUIRE(::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    const int descriptor = socket.get();
    QUICListener listener;
    CHECK(!listener.startOnBoundSocket(std::move(socket)));
    CHECK(isClosed(descriptor));
    CHECK_EQ(listener.boundPort(), 0);
}

TEST(boundSocketAdoptionNeverReplacesARunningListener)
{
    PrivateHome home;
    REQUIRE(home.directory.valid());
    QUICListener listener;
    REQUIRE(listener.start(0));
    const int descriptor = listener.fileDescriptor();
    const auto port = listener.boundPort();
    auto socket = boundSocket();
    REQUIRE(socket);
    const int rejected = socket.get();
    CHECK(!listener.startOnBoundSocket(std::move(socket)));
    CHECK(isClosed(rejected));
    CHECK(!listener.start(0));
    CHECK_EQ(listener.fileDescriptor(), descriptor);
    CHECK_EQ(listener.boundPort(), port);
    CHECK(!isClosed(descriptor));
}

TEST(boundSocketTLSRefusalConsumesOnlyTheCandidateAndAllowsRetry)
{
    PrivateHome home;
    REQUIRE(home.directory.valid());
    const auto invalidHome = home.directory.path() + "/missing/control";
    REQUIRE(::setenv("UBIQUITTY_HOME", invalidHome.c_str(), 1) == 0);
    auto socket = boundSocket();
    REQUIRE(socket);
    const auto descriptor = socket.get();
    QUICListener listener;
    CHECK(!listener.startOnBoundSocket(std::move(socket)));
    CHECK(isClosed(descriptor));
    CHECK_EQ(listener.fileDescriptor(), -1);
    CHECK_EQ(listener.boundPort(), 0);
    REQUIRE(::setenv("UBIQUITTY_HOME", home.directory.path().c_str(), 1) == 0);
    REQUIRE(listener.startOnBoundSocket(boundSocket()));
}

TEST(boundSocketStopRetiresConnectionsBeforeRestart)
{
    PrivateHome home;
    REQUIRE(home.directory.valid());
    QUICListener listener;
    REQUIRE(listener.start(0));
    REQUIRE(Ubiquitty::Host::adoptTestQUICConnection(listener, Ubiquitty::Host::makeTestQUICConnection()));
    listener.stop();
    CHECK_EQ(Ubiquitty::Host::testQUICConnectionCount(listener), 0u);
    REQUIRE(listener.start(0));
    auto socket = listener.releaseBoundSocket();
    REQUIRE(socket);
}

TEST(boundSocketTLSKeyRefusalClearsPartialContextBeforeRetry)
{
    PrivateHome home;
    REQUIRE(home.directory.valid());
    QUICListener listener;
    REQUIRE(listener.start(0));
    listener.stop();
    const auto key = home.directory.path() + "/quic-key.pem";
    const auto savedKey = key + ".saved";
    REQUIRE(::rename(key.c_str(), savedKey.c_str()) == 0);
    {
        UnixFileDescriptor invalidKey(::open(key.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600));
        REQUIRE(invalidKey);
    }
    auto socket = boundSocket();
    REQUIRE(socket);
    const auto descriptor = socket.get();
    CHECK(!listener.startOnBoundSocket(std::move(socket)));
    CHECK(isClosed(descriptor));
    CHECK_EQ(listener.fileDescriptor(), -1);
    CHECK_EQ(listener.boundPort(), 0);
    CHECK(listener.certificateSHA256().empty());
    REQUIRE(::rename(savedKey.c_str(), key.c_str()) == 0);
    REQUIRE(listener.startOnBoundSocket(boundSocket()));
    CHECK(listener.certificateSHA256().size() == 32);
}

UBIQUITTY_TEST_MAIN()
