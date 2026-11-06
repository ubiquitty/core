// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/HandoverBody.h"
#include "Harness.h"
#include "Helpers/TempDir.h"
#include "Platform/Host/HandoverAdmission.h"
#include "Platform/Host/HostSocketName.h"
#include "Platform/POSIX/HandoverTransport.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

namespace Host = Ubiquitty::Host;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace POSIX = Ubiquitty::POSIX;
using Ubiquitty::PAL::UnixFileDescriptor;

namespace {

enum class ListenerState {
    Bound,
    Listening
};
enum class PeerState {
    Live,
    Exited
};

std::array<UnixFileDescriptor, 2> channelPair()
{
    int descriptors[2] { -1, -1 };
    CHECK_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
    return { UnixFileDescriptor(descriptors[0]), UnixFileDescriptor(descriptors[1]) };
}

UnixFileDescriptor boundListener(const std::string& path, ListenerState state = ListenerState::Listening)
{
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        return {};
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    UnixFileDescriptor descriptor(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!descriptor || ::bind(descriptor.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
        || (state == ListenerState::Listening && ::listen(descriptor.get(), 4) != 0))
        return {};
    return descriptor;
}

struct Fixture {
    std::string executable = Host::resolveSelfExecutablePath();
    Ubiquitty::Test::TempDir installed { std::filesystem::path(executable).parent_path().string() + "/admit.XXXXXX" };
    Ubiquitty::Test::TempDir control { "/tmp/ubq-admit.XXXXXX" };
    std::string installPath = installed.path() + "/successor";
    std::string socketPath = control.path() + "/" + Host::socketLeafForBinary(executable.c_str());
    std::array<UnixFileDescriptor, 2> channel = channelPair();
    UnixFileDescriptor listener = boundListener(socketPath);
    Sync::HandoverRequest request { Sync::handoverBodyVersion, { 17, 29 } };

    Fixture() { CHECK_EQ(::link(executable.c_str(), installPath.c_str()), 0); }

    Host::HandoverAdmissionContext context() const { return { .isFirstRecord = true, .installPath = installPath, .controlDirectory = control.path(), .holderSignature = {} }; }
};

bool isClosed(int descriptor)
{
    return ::fcntl(descriptor, F_GETFD) < 0 && errno == EBADF;
}

struct Child {
    pid_t process = -1;
    int status = 0;

    void join()
    {
        if (process <= 0)
            return;
        pid_t result;
        do {
            result = ::waitpid(process, &status, 0);
        } while (result < 0 && errno == EINTR);
        CHECK_EQ(result, process);
        process = -1;
    }

    ~Child() { join(); }
};

int runExecutablePeer(const char* channelPath, const char* listenerPath)
{
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (std::strlen(channelPath) >= sizeof(address.sun_path))
        return 1;
    std::strcpy(address.sun_path, channelPath);
    UnixFileDescriptor channel(::socket(AF_UNIX, SOCK_STREAM, 0));
    auto listener = boundListener(listenerPath);
    if (!channel || !listener || ::connect(channel.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        return 2;
    const auto bytes = Sync::encodeHandoverMessage(Sync::HandoverRequest { Sync::handoverBodyVersion, { 17, 29 } });
    const std::array descriptors { listener.get() };
    if (!POSIX::sendHandoverRecord(channel.get(), bytes, descriptors, 1000))
        return 3;
    const auto reply = POSIX::receiveHandoverRecord(channel.get(), 1, 0, 2000);
    return reply && reply->bytes().size() == 1 ? 0 : 4;
}

void checkChildRequest(PeerState state)
{
    Fixture fixture;
    fixture.listener.reset();
    REQUIRE(::unlink(fixture.socketPath.c_str()) == 0);
    const auto channelPath = fixture.control.path() + "/holder.sock";
    auto holder = boundListener(channelPath);
    REQUIRE(holder);
    Child child { ::fork() };
    REQUIRE(child.process >= 0);
    if (child.process == 0) {
        holder.reset();
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, channelPath.c_str(), channelPath.size() + 1);
        UnixFileDescriptor channel(::socket(AF_UNIX, SOCK_STREAM, 0));
        auto listener = boundListener(fixture.socketPath);
        if (!channel || !listener || ::connect(channel.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            ::_exit(1);
        const auto bytes = Sync::encodeHandoverMessage(fixture.request);
        const std::array descriptors { listener.get() };
        if (!POSIX::sendHandoverRecord(channel.get(), bytes, descriptors, 1000))
            ::_exit(2);
        if (state == PeerState::Exited)
            ::_exit(0);
        const auto reply = POSIX::receiveHandoverRecord(channel.get(), 1, 0, 2000);
        ::_exit(reply && reply->bytes().size() == 1 ? 0 : 3);
    }
    const auto peerProcessID = child.process;
    pollfd ready { holder.get(), POLLIN, 0 };
    REQUIRE(::poll(&ready, 1, 1000) == 1);
    UnixFileDescriptor channel(::accept(holder.get(), nullptr, nullptr));
    REQUIRE(channel);
    auto record = POSIX::receiveHandoverRecord(channel.get(), 64, 1, 1000);
    REQUIRE(record);
    const auto decoded = Sync::decodeHandoverMessage(record->bytes(), {});
    REQUIRE(decoded && std::holds_alternative<Sync::HandoverRequest>(*decoded));
    const auto descriptor = record->descriptors[0].get();
    if (state == PeerState::Exited)
        child.join();
    auto admitted = Host::admitHandoverRequest(channel.get(), std::move(record->descriptors[0]),
        std::get<Sync::HandoverRequest>(*decoded), fixture.context());
    if (state == PeerState::Exited) {
        CHECK(!admitted);
        CHECK(isClosed(descriptor));
    } else {
        REQUIRE(admitted);
        CHECK_EQ(admitted->peerProcessID, static_cast<std::uint32_t>(peerProcessID));
        CHECK(admitted->peerProcessID != static_cast<std::uint32_t>(::getpid()));
        const std::array<std::uint8_t, 1> done { 1 };
        REQUIRE(POSIX::sendHandoverRecord(channel.get(), done, {}, 1000));
        child.join();
    }
    CHECK(WIFEXITED(child.status) && WEXITSTATUS(child.status) == 0);
}

} // namespace

TEST(aRealLocalRequestCarriesOnlyTheSuccessorsExactListeningOwner)
{
    Fixture fixture;
    REQUIRE(fixture.listener && fixture.channel[0] && fixture.channel[1]);
    REQUIRE(Host::handoverPeerHasSameUser(fixture.channel[0].get()));
    const auto bytes = Sync::encodeHandoverMessage(fixture.request);
    const std::array descriptors { fixture.listener.get() };
    REQUIRE(POSIX::sendHandoverRecord(fixture.channel[0].get(), bytes, descriptors, 1000));
    auto record = POSIX::receiveHandoverRecord(fixture.channel[1].get(), 64, 1, 1000);
    REQUIRE(record);
    const auto decoded = Sync::decodeHandoverMessage(record->bytes(), {});
    REQUIRE(decoded && std::holds_alternative<Sync::HandoverRequest>(*decoded));
    const auto listener = record->descriptors[0].get();
    auto admitted = Host::admitHandoverRequest(fixture.channel[1].get(), std::move(record->descriptors[0]),
        std::get<Sync::HandoverRequest>(*decoded), fixture.context());
    REQUIRE(admitted);
    CHECK(admitted->request == fixture.request);
    CHECK_EQ(admitted->peerProcessID, static_cast<std::uint32_t>(::getpid()));
    CHECK_EQ(admitted->listener.get(), listener);
    CHECK(!record->descriptors[0]);
    CHECK(!isClosed(fixture.listener.get()));
}

TEST(theBodyVersionIsRefusedBeforeAnyOwnerCanBeOffered)
{
    Fixture fixture;
    fixture.request.maxBodyVersion = 0;
    const auto descriptor = fixture.listener.get();
    auto admitted = Host::admitHandoverRequest(fixture.channel[1].get(), std::move(fixture.listener), fixture.request, fixture.context());
    REQUIRE(!admitted);
    CHECK(admitted.error() == Sync::HandoverRefuseReason::NewerBody);
    CHECK(isClosed(descriptor));
    CHECK(!isClosed(fixture.channel[1].get()));
}

TEST(aPreambleAfterOrdinaryTrafficNeverStartsAHandover)
{
    Fixture fixture;
    auto context = fixture.context();
    context.isFirstRecord = false;
    const auto descriptor = fixture.listener.get();
    auto admitted = Host::admitHandoverRequest(fixture.channel[1].get(), std::move(fixture.listener), fixture.request, context);
    REQUIRE(!admitted);
    CHECK(admitted.error() == Sync::HandoverRefuseReason::NotSuccessor);
    CHECK(isClosed(descriptor));
}

TEST(aRelayWithoutAListeningDescriptorHasNoSuccessorAuthority)
{
    Fixture fixture;
    auto admitted = Host::admitHandoverRequest(fixture.channel[1].get(), {}, fixture.request, fixture.context());
    REQUIRE(!admitted);
    CHECK(admitted.error() == Sync::HandoverRefuseReason::NotSuccessor);
    CHECK(!isClosed(fixture.listener.get()));
}

TEST(aDifferentBoundPathOrNonListeningSocketIsRefused)
{
    for (const auto state : { ListenerState::Bound, ListenerState::Listening }) {
        Fixture fixture;
        auto candidate = boundListener(fixture.control.path() + "/wrong.sock", state);
        REQUIRE(candidate);
        const auto descriptor = candidate.get();
        auto admitted = Host::admitHandoverRequest(fixture.channel[1].get(), std::move(candidate), fixture.request, fixture.context());
        REQUIRE(!admitted);
        CHECK(admitted.error() == Sync::HandoverRefuseReason::NotSuccessor);
        CHECK(isClosed(descriptor));
    }
}

TEST(aCopiedBinaryAtTheInstallPathDoesNotMatchTheRunningPeer)
{
    Fixture fixture;
    REQUIRE(::unlink(fixture.installPath.c_str()) == 0);
    std::error_code error;
    REQUIRE(std::filesystem::copy_file(fixture.executable, fixture.installPath, error));
    const auto descriptor = fixture.listener.get();
    auto admitted = Host::admitHandoverRequest(fixture.channel[1].get(), std::move(fixture.listener), fixture.request, fixture.context());
    REQUIRE(!admitted);
    CHECK(admitted.error() == Sync::HandoverRefuseReason::NotSuccessor);
    CHECK(isClosed(descriptor));
}

TEST(aSeparateProcessMustOwnTheExactExecutableAndListener)
{
    checkChildRequest(PeerState::Live);
}

TEST(anExitedPeerCannotAuthorizeAQueuedRequest)
{
    checkChildRequest(PeerState::Exited);
}

TEST(aCorrectPathWithoutListenIsStillRefused)
{
    Fixture fixture;
    fixture.listener.reset();
    REQUIRE(::unlink(fixture.socketPath.c_str()) == 0);
    auto candidate = boundListener(fixture.socketPath, ListenerState::Bound);
    REQUIRE(candidate);
    const auto descriptor = candidate.get();
    const auto admitted = Host::admitHandoverRequest(fixture.channel[1].get(), std::move(candidate), fixture.request, fixture.context());
    CHECK(!admitted);
    CHECK(isClosed(descriptor));
}

TEST(ordinaryFilesDatagramsAndConnectedEndpointsAreNotListeningOwners)
{
    Fixture fixture;
    const std::array candidates {
        ::open(fixture.installPath.c_str(), O_RDONLY | O_CLOEXEC),
        ::socket(AF_UNIX, SOCK_DGRAM, 0),
        ::dup(fixture.channel[0].get())
    };
    for (const auto descriptor : candidates) {
        REQUIRE(descriptor >= 0);
        const auto admitted = Host::admitHandoverRequest(fixture.channel[1].get(), UnixFileDescriptor(descriptor), fixture.request, fixture.context());
        CHECK(!admitted);
        CHECK(isClosed(descriptor));
    }
    CHECK(!isClosed(fixture.channel[0].get()));
}

TEST(nonLocalOrNonStreamChannelsCannotEnterAdmission)
{
    Fixture fixture;
    UnixFileDescriptor network(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(network);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(network.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(network.get(), 1) == 0);
    socklen_t size = sizeof(address);
    REQUIRE(::getsockname(network.get(), reinterpret_cast<sockaddr*>(&address), &size) == 0);
    UnixFileDescriptor client(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(client && ::connect(client.get(), reinterpret_cast<sockaddr*>(&address), size) == 0);
    UnixFileDescriptor server(::accept(network.get(), nullptr, nullptr));
    REQUIRE(server);
    CHECK(!Host::handoverPeerHasSameUser(server.get()));
    const auto descriptor = fixture.listener.get();
    CHECK(!Host::admitHandoverRequest(server.get(), std::move(fixture.listener), fixture.request, fixture.context()));
    CHECK(isClosed(descriptor));
    int pair[2] { -1, -1 };
    REQUIRE(::socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    const std::array datagrams { UnixFileDescriptor(pair[0]), UnixFileDescriptor(pair[1]) };
    CHECK(!Host::handoverPeerHasSameUser(datagrams[0].get()));
}

TEST(absentIdentityAndUntrustedPathFactsRefuseWithoutTouchingTheChannel)
{
    Fixture fixture;
    const std::array<std::string_view, 4> invalidPaths { "", "relative", std::string_view("/bad\0path", 9), "/does-not-exist/ubiquitty" };
    for (const auto path : invalidPaths) {
        auto context = fixture.context();
        context.installPath = path;
        auto admitted = Host::admitHandoverRequest(fixture.channel[1].get(), UnixFileDescriptor(::dup(fixture.listener.get())), fixture.request, context);
        CHECK(!admitted);
        context = fixture.context();
        context.controlDirectory = path;
        CHECK(!Host::admitHandoverRequest(fixture.channel[1].get(), UnixFileDescriptor(::dup(fixture.listener.get())), fixture.request, context));
    }
    fixture.request.requesterInstance = {};
    const auto admitted = Host::admitHandoverRequest(fixture.channel[1].get(), std::move(fixture.listener), fixture.request, fixture.context());
    REQUIRE(!admitted);
    CHECK(admitted.error() == Sync::HandoverRefuseReason::Malformed);
    CHECK(!isClosed(fixture.channel[1].get()));
}

TEST(replacingThePeersExecutablePathCannotReplaceItsRunningIdentity)
{
    Fixture fixture;
    REQUIRE(::unlink(fixture.installPath.c_str()) == 0);
    std::error_code error;
    REQUIRE(std::filesystem::copy_file(fixture.executable, fixture.installPath, error));
    const auto replacementPath = fixture.installed.path() + "/replacement";
    REQUIRE(std::filesystem::copy_file("/bin/ls", replacementPath, error));
    const auto replacementLeaf = Host::socketLeafForBinary(replacementPath.c_str());
    REQUIRE(!replacementLeaf.empty());
    const auto replacementListener = fixture.control.path() + "/" + replacementLeaf;
    const auto channelPath = fixture.control.path() + "/holder.sock";
    auto holder = boundListener(channelPath);
    REQUIRE(holder);
    Child child { ::fork() };
    REQUIRE(child.process >= 0);
    if (child.process == 0) {
        ::execl(fixture.installPath.c_str(), fixture.installPath.c_str(), "--admission-peer", channelPath.c_str(), replacementListener.c_str(), nullptr);
        ::_exit(5);
    }
    pollfd ready { holder.get(), POLLIN, 0 };
    REQUIRE(::poll(&ready, 1, 1000) == 1);
    UnixFileDescriptor channel(::accept(holder.get(), nullptr, nullptr));
    REQUIRE(channel);
    auto record = POSIX::receiveHandoverRecord(channel.get(), 64, 1, 1000);
    REQUIRE(record);
    // Both pathname checks and the supplied listener now describe the new file,
    // but the requesting process still executes the old, unlinked image.
    REQUIRE(::rename(replacementPath.c_str(), fixture.installPath.c_str()) == 0);
    const auto descriptor = record->descriptors[0].get();
    const auto admitted = Host::admitHandoverRequest(channel.get(), std::move(record->descriptors[0]), fixture.request, fixture.context());
    CHECK(!admitted);
    CHECK(isClosed(descriptor));
    const std::array<std::uint8_t, 1> done { 1 };
    REQUIRE(POSIX::sendHandoverRecord(channel.get(), done, {}, 1000));
    child.join();
    CHECK(WIFEXITED(child.status) && WEXITSTATUS(child.status) == 0);
}

#if defined(__APPLE__)
TEST(aBundledAdopterMustMatchItsHeldExecutableAndIntactResourceSeal)
{
    for (const bool shouldChangePropertyList : { false, true }) {
        Fixture fixture;
        const auto bundle = fixture.installed.path() + "/Adopter.app";
        const auto executable = bundle + "/Contents/MacOS/Ubiquitty";
        const auto helper = bundle + "/Contents/Helpers/ubiquitty-host";
        const auto propertyList = bundle + "/Contents/Info.plist";
        const auto resource = bundle + "/Contents/Resources/marker";
        std::error_code error;
        REQUIRE(std::filesystem::create_directories(bundle + "/Contents/MacOS", error));
        REQUIRE(std::filesystem::create_directories(bundle + "/Contents/Helpers", error));
        REQUIRE(std::filesystem::create_directories(bundle + "/Contents/Resources", error));
        REQUIRE(std::filesystem::copy_file(fixture.executable, executable, error));
        REQUIRE(std::filesystem::copy_file(fixture.executable, helper, error));
        const auto write = [](const std::string& path, const char* text) {
            auto* file = std::fopen(path.c_str(), "w");
            REQUIRE(file);
            CHECK(std::fputs(text, file) >= 0);
            CHECK_EQ(std::fclose(file), 0);
        };
        write(propertyList, R"(<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict><key>CFBundleIdentifier</key><string>org.ubiquitty.adopter-fixture</string><key>CFBundleExecutable</key><string>Ubiquitty</string><key>CFBundlePackageType</key><string>APPL</string></dict></plist>)");
        write(resource, "sealed");
        Child signer { ::fork() };
        REQUIRE(signer.process >= 0);
        if (!signer.process) {
            ::execl("/usr/bin/codesign", "codesign", "--force", "--sign", "-", bundle.c_str(), nullptr);
            ::_exit(5);
        }
        signer.join();
        REQUIRE(WIFEXITED(signer.status) && WEXITSTATUS(signer.status) == 0);
        const auto trusted = Host::LocalAdoptExecutables::capture(helper);
        REQUIRE(trusted);
        const auto channelPath = fixture.control.path() + "/adopter.sock";
        const auto listenerPath = fixture.control.path() + "/offered.sock";
        auto holder = boundListener(channelPath);
        REQUIRE(holder);
        Child child { ::fork() };
        REQUIRE(child.process >= 0);
        if (!child.process) {
            ::execl(executable.c_str(), executable.c_str(), "--admission-peer", channelPath.c_str(), listenerPath.c_str(), nullptr);
            ::_exit(5);
        }
        pollfd ready { holder.get(), POLLIN, 0 };
        REQUIRE(::poll(&ready, 1, 1000) == 1);
        UnixFileDescriptor channel(::accept(holder.get(), nullptr, nullptr));
        REQUIRE(channel);
        auto record = POSIX::receiveHandoverRecord(channel.get(), 64, 1, 1000);
        REQUIRE(record);
        CHECK(Host::currentLocalAdoptAuthority(channel.get(), *trusted));
        write(shouldChangePropertyList ? propertyList : resource, "tampered");
        CHECK(!Host::currentLocalAdoptAuthority(channel.get(), *trusted));
        const std::array<std::uint8_t, 1> done { 1 };
        REQUIRE(POSIX::sendHandoverRecord(channel.get(), done, {}, 1000));
        child.join();
        CHECK(WIFEXITED(child.status) && WEXITSTATUS(child.status) == 0);
    }
}
#endif

int main(int argc, char** argv)
{
    if (argc == 4 && std::strcmp(argv[1], "--admission-peer") == 0)
        return runExecutablePeer(argv[2], argv[3]);
    UBIQUITTY_TEST_PIN_PREFERENCES();
    return UbiquittyTest::runRegisteredCases(argc, argv);
}
