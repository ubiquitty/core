// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Fresh admission accepts one validated SSH destination, never argv or shell
// text. Existing-session admission cannot carry an SSH destination or create a
// replacement. The fake executable proves the real launch argv without network.

#include "Assertions.h"
#include "HostConnection.h"
#include "ScreenPlaneAccess.h"

#include "Core/Protocol/Sync/Wire.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Host {

struct HelloSSHHostAccess {
    static void setShellPath(Daemon& daemon, const std::string& path) { daemon.m_shellPath = path; }
    static std::size_t sessionCount(const Daemon& daemon) { return daemon.m_sessions.size(); }
    static std::uint32_t clientSessionID(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].sessionID; }
    static int newestSessionPTY(const Daemon& daemon) { return daemon.m_sessions.back()->stream->fileDescriptor(); }
};

}

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
using Access = Ubiquitty::Host::HelloSSHHostAccess;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using Ubiquitty::Host::Daemon;
constexpr std::string_view validHost = "deploy@build-07.internal";

std::string readPTY(int descriptor)
{
    std::string collected;
    for (unsigned wake = 0; wake < 20; ++wake) {
        pollfd waiting { .fd = descriptor, .events = POLLIN, .revents = 0 };
        if (::poll(&waiting, 1, 100) <= 0)
            continue;
        std::array<char, 4096> buffer {};
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count <= 0)
            break;
        collected.append(buffer.data(), static_cast<std::size_t>(count));
        if (collected.contains('\n'))
            break;
    }
    return collected;
}

Sync::Greet freshGreeting(Daemon& daemon)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = 1, .high = 0x737368 };
    greeting.request.target = Sync::AdmissionTarget::Session;
    greeting.request.expectedDaemonInstanceID = Screen::daemonInstanceID(daemon);
    greeting.facts.viewportColumns = 8;
    greeting.facts.viewportRows = 2;
    return greeting;
}

std::span<const std::uint8_t> bytes(const std::string& body)
{
    return { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() };
}

struct Outcome {
    std::optional<Sync::Welcome> welcome;
    std::optional<Sync::Bye> bye;
    std::string output;
    std::size_t sessions = 0;
    std::uint32_t sessionID = 0;
};

std::optional<Outcome> driveOneGreeting(Daemon& daemon, const Sync::Greet& greeting, std::span<const std::uint8_t> body)
{
    std::array<int, 2> ends { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()))
        return std::nullopt;
    Ubiquitty::PAL::UnixFileDescriptor peer(ends[1]);
    if (::fcntl(ends[0], F_SETFL, O_NONBLOCK) < 0) {
        ::close(ends[0]);
        return std::nullopt;
    }
    const auto index = Screen::connect(daemon, Ubiquitty::PAL::UnixFileDescriptor { ends[0] });
    Screen::markTrustedLocal(daemon, index);
    Ubiquitty::HostTest::Connection reader;
    reader.fileDescriptor = peer.get();
    reader.greeting = greeting;
    std::vector<std::uint8_t> frame;
    if (!Sync::appendFrame(frame, Sync::FrameKind::Greet, 0, body))
        return std::nullopt;
    const auto before = Access::sessionCount(daemon);
    Screen::receive(daemon, index, frame);
    Screen::service(daemon);
    Screen::flush(daemon, index);
    Outcome outcome;
    outcome.sessions = Access::sessionCount(daemon);
    outcome.sessionID = Access::clientSessionID(daemon, index);
    if (outcome.sessions > before)
        outcome.output = readPTY(Access::newestSessionPTY(daemon));
    const auto* connection = Screen::connection(daemon, index);
    std::string answer;
    if (connection && connection->state() == Sync::ConnectionState::Attached) {
        if (Ubiquitty::HostTest::waitFor(reader, Sync::FrameKind::Welcome, answer, 2000))
            outcome.welcome = Sync::decodeWelcome(bytes(answer));
    } else if (Ubiquitty::HostTest::waitFor(reader, Sync::FrameKind::Bye, answer, 2000))
        outcome.bye = Sync::decodeBye(bytes(answer));
    CHECK(!reader.hasFailed);
    Screen::drop(daemon, index);
    return outcome;
}

// Inserts only the prohibited field into an otherwise valid existing request.
std::optional<std::vector<std::uint8_t>> withSSHHost(std::span<const std::uint8_t> body)
{
    const auto greeting = Sync::decodeGreet(body);
    if (!greeting)
        return std::nullopt;
    Sync::WireReader reader(body);
    (void)reader.u16();
    (void)reader.utf8(Sync::maxAppVersionBytes);
    (void)reader.take(greeting->engineFingerprint.size() + 16 + 2);
    (void)reader.u8();
    (void)reader.u32();
    (void)reader.u32();
    (void)reader.u64();
    (void)reader.utf8(Sync::maxFrameBodyBytes);
    (void)reader.utf8(Sync::maxFrameBodyBytes);
    const auto begin = reader.offset();
    (void)reader.utf8(Sync::maxFrameBodyBytes);
    const auto end = reader.offset();
    if (!reader.ok() || end > body.size())
        return std::nullopt;
    std::vector<std::uint8_t> changed(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(begin));
    Sync::WireWriter writer(changed);
    writer.utf8(validHost);
    changed.insert(changed.end(), body.begin() + static_cast<std::ptrdiff_t>(end), body.end());
    return changed;
}

void run(Daemon& daemon)
{
    {
        auto greeting = freshGreeting(daemon);
        greeting.request.sshHost = "-oProxyCommand=x";
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        REQUIRE(Sync::decodeGreet(body));
        const auto outcome = driveOneGreeting(daemon, greeting, body);
        REQUIRE(outcome && outcome->bye);
        CHECK(outcome->bye->reason == Sync::ByeReason::RemoteHostRefused);
        CHECK(!outcome->welcome);
        CHECK_EQ(outcome->sessions, 0u);
        CHECK(outcome->output.empty());
    }
    std::uint32_t createdID = 0;
    {
        auto greeting = freshGreeting(daemon);
        greeting.request.sshHost = validHost;
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        REQUIRE(Sync::decodeGreet(body));
        const auto outcome = driveOneGreeting(daemon, greeting, body);
        REQUIRE(outcome && outcome->welcome);
        CHECK(outcome->welcome->admission == Sync::AttachmentAdmission::Granted);
        CHECK_EQ(outcome->welcome->selection.daemonInstanceID, greeting.request.expectedDaemonInstanceID);
        CHECK_EQ(outcome->sessions, 1u);
        CHECK_CONTAINS(outcome->output, validHost);
        CHECK_CONTAINS(outcome->output, "-t ");
        createdID = outcome->sessionID;
        REQUIRE(createdID);
        CHECK_EQ(outcome->welcome->selection.sessionID, createdID);
    }
    for (const auto sessionID : { createdID, std::uint32_t { 0xDEAD } }) {
        auto greeting = freshGreeting(daemon);
        greeting.request.sessionID = sessionID;
        greeting.request.isAttachOnly = true;
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        REQUIRE(Sync::decodeGreet(body));
        const auto hostile = withSSHHost(body);
        REQUIRE(hostile);
        CHECK(!Sync::decodeGreet(*hostile));
        greeting.request.sshHost = validHost;
        std::vector<std::uint8_t> refused;
        Sync::encode(refused, greeting);
        CHECK(refused.empty());
        const auto outcome = driveOneGreeting(daemon, greeting, *hostile);
        REQUIRE(outcome && outcome->bye);
        CHECK(outcome->bye->reason == Sync::ByeReason::Malformed);
        CHECK(!outcome->welcome);
        CHECK_EQ(outcome->sessions, 1u);
        CHECK(outcome->output.empty());
    }
    {
        auto greeting = freshGreeting(daemon);
        greeting.request.sessionID = createdID;
        greeting.request.isAttachOnly = true;
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        REQUIRE(Sync::decodeGreet(body));
        const auto outcome = driveOneGreeting(daemon, greeting, body);
        REQUIRE(outcome && outcome->welcome);
        CHECK(outcome->welcome->admission == Sync::AttachmentAdmission::Granted);
        CHECK_EQ(outcome->welcome->selection.sessionID, createdID);
        CHECK_EQ(outcome->welcome->selection.daemonInstanceID, greeting.request.expectedDaemonInstanceID);
        CHECK_EQ(outcome->sessions, 1u);
        CHECK(outcome->output.empty());
    }
}

}

int main(int argc, char** argv)
{
    const char* invoked = argc && argv[0] ? argv[0] : "";
    const auto* afterSlash = std::strrchr(invoked, '/');
    std::string self = afterSlash ? afterSlash + 1 : invoked;
    if (!self.empty() && self.front() == '-')
        self.erase(0, 1);
    if (self == "ssh") {
        for (int index = 1; index < argc; ++index)
            std::printf(index > 1 ? " %s" : "%s", argv[index]);
        std::printf("\n");
        return 0;
    }
    if (self == "localshell") {
        std::printf("/localshell\n");
        return 0;
    }
    char directoryTemplate[] = "/tmp/hello_ssh_host.XXXXXX";
    const auto* directory = ::mkdtemp(directoryTemplate);
    if (!directory)
        return 1;
    const auto fakeSSH = std::string(directory) + "/ssh";
    const auto fakeShell = std::string(directory) + "/localshell";
    std::array<char, PATH_MAX> selfPath {};
    if (!::realpath(argv[0], selfPath.data()))
        return 1;
    CHECK_EQ(::symlink(selfPath.data(), fakeSSH.c_str()), 0);
    CHECK_EQ(::symlink(selfPath.data(), fakeShell.c_str()), 0);
    CHECK_EQ(::setenv("PATH", directory, 1), 0);
    {
        Daemon daemon;
        Access::setShellPath(daemon, fakeShell);
        run(daemon);
    }
    ::unlink(fakeSSH.c_str());
    ::unlink(fakeShell.c_str());
    ::rmdir(directory);
    return UBIQUITTY_TEST_SUMMARY();
}
