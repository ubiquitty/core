// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A complete greeting bounds each retained field independently: labels are
// presented at a UTF-8 boundary, while oversized credentials and SSH targets
// fail the schema before token admission or session creation.

#include "Assertions.h"
#include "HostConnection.h"
#include "ScreenPlaneAccess.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Control/HostAdministration.h"
#include "Core/Protocol/Sync/HostManagement.h"
#include "Core/Protocol/Sync/TokenRecord.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Transport/SSHHost.h"
#include "Platform/Host/TokenStore.h"

#include <fcntl.h>
#include <sys/socket.h>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::Host {

struct HelloFieldBoundsAccess {
    static const std::string& agentLabel(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].agentLabel; }
    static bool hasToken(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].hasToken; }
    static bool helloSeen(const Daemon& daemon, std::size_t index) { return daemon.m_clients[index].helloSeen; }
    static std::size_t sessionCount(const Daemon& daemon) { return daemon.m_sessions.size(); }
};

}

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
using Access = Ubiquitty::Host::HelloFieldBoundsAccess;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using Ubiquitty::Host::Daemon;

struct Peer {
    std::size_t index;
    Ubiquitty::PAL::UnixFileDescriptor socket;
    Ubiquitty::HostTest::Connection reader;
};

std::optional<Peer> connectPeer(Daemon& daemon)
{
    std::array<int, 2> ends { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()))
        return std::nullopt;
    if (::fcntl(ends[0], F_SETFL, O_NONBLOCK) < 0) {
        ::close(ends[0]);
        ::close(ends[1]);
        return std::nullopt;
    }
    const auto index = Screen::connect(daemon, Ubiquitty::PAL::UnixFileDescriptor { ends[0] });
    Screen::markTrustedLocal(daemon, index);
    Peer peer { .index = index, .socket = Ubiquitty::PAL::UnixFileDescriptor { ends[1] }, .reader = {} };
    peer.reader.fileDescriptor = peer.socket.get();
    return peer;
}

Sync::Greet controlGreeting(Daemon& daemon, std::uint64_t identity)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = identity, .high = 0x626f756e6473 };
    greeting.request.target = Sync::AdmissionTarget::Control;
    greeting.request.expectedDaemonInstanceID = Screen::daemonInstanceID(daemon);
    greeting.facts.viewportRows = 2;
    greeting.facts.viewportColumns = 8;
    return greeting;
}

enum class GreetingField : std::uint8_t {
    AgentLabel,
    SSHHost,
    Credential
};

// Only locates a field of an already validated production encoding. The
// replacement can violate its bound without adding an alternate encoder.
std::optional<std::vector<std::uint8_t>> replacingField(std::span<const std::uint8_t> body, GreetingField field, std::string_view replacement)
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
    auto begin = reader.offset();
    (void)reader.utf8(Sync::maxFrameBodyBytes);
    auto end = reader.offset();
    if (field != GreetingField::AgentLabel) {
        begin = reader.offset();
        (void)reader.utf8(Sync::maxFrameBodyBytes);
        end = reader.offset();
        if (field == GreetingField::Credential) {
            (void)reader.u8();
            (void)reader.u8();
            (void)reader.u8();
            if (reader.u8() != 1)
                return std::nullopt;
            begin = reader.offset();
            (void)reader.bytes(Sync::maxCredentialBytes);
            end = reader.offset();
        }
    }
    if (!reader.ok() || begin > end || end > body.size())
        return std::nullopt;
    std::vector<std::uint8_t> changed(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(begin));
    Sync::WireWriter writer(changed);
    if (field == GreetingField::Credential)
        writer.bytes({ reinterpret_cast<const std::uint8_t*>(replacement.data()), replacement.size() });
    else
        writer.utf8(replacement);
    changed.insert(changed.end(), body.begin() + static_cast<std::ptrdiff_t>(end), body.end());
    return changed;
}

void deliver(Daemon& daemon, Peer& peer, const Sync::Greet& greeting, std::span<const std::uint8_t> body)
{
    std::vector<std::uint8_t> frame;
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Greet, 0, body));
    peer.reader.greeting = greeting;
    Screen::receive(daemon, peer.index, frame);
    Screen::service(daemon);
    Screen::flush(daemon, peer.index);
}

std::span<const std::uint8_t> bytes(const std::string& body)
{
    return { reinterpret_cast<const std::uint8_t*>(body.data()), body.size() };
}

void checkGranted(Peer& peer)
{
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer.reader, Sync::FrameKind::Welcome, answer, 2000));
    const auto welcome = Sync::decodeWelcome(bytes(answer));
    REQUIRE(welcome);
    CHECK(welcome->admission == Sync::AttachmentAdmission::Granted);
    CHECK(welcome->selection.target == Sync::AdmissionTarget::Control);
    CHECK_EQ(welcome->selection.daemonInstanceID, peer.reader.greeting->request.expectedDaemonInstanceID);
    CHECK(!peer.reader.hasFailed);
}

void checkMalformed(Daemon& daemon, Peer& peer)
{
    std::string answer;
    REQUIRE(Ubiquitty::HostTest::waitFor(peer.reader, Sync::FrameKind::Bye, answer, 2000));
    const auto bye = Sync::decodeBye(bytes(answer));
    REQUIRE(bye);
    CHECK(bye->reason == Sync::ByeReason::Malformed);
    CHECK(!peer.reader.welcome);
    CHECK(!Access::hasToken(daemon, peer.index));
    CHECK(!Access::helloSeen(daemon, peer.index));
    CHECK_EQ(Access::sessionCount(daemon), 0u);
}

void fieldBounds(Daemon& daemon, const std::string& longestToken, const std::string& overlongToken)
{
    {
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        auto greeting = controlGreeting(daemon, 1);
        greeting.credential = std::vector<std::uint8_t>(longestToken.begin(), longestToken.end());
        std::string label = "a";
        while (label.size() < 5000)
            label += "\xC3\xA9";
        greeting.request.agentLabel = label;
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        const auto encoded = Sync::decodeGreet(body);
        REQUIRE(encoded);
        CHECK_EQ(encoded->request.agentLabel.size(), Sync::maxAgentLabelBytes - 1);
        const auto hostile = replacingField(body, GreetingField::AgentLabel, label);
        REQUIRE(hostile && Sync::decodeGreet(*hostile));
        deliver(daemon, *peer, greeting, *hostile);
        checkGranted(*peer);
        const auto kept = Access::agentLabel(daemon, peer->index);
        CHECK_EQ(kept.size(), Sync::maxAgentLabelBytes - 1);
        CHECK(!kept.empty() && label.starts_with(kept));
        CHECK(Access::hasToken(daemon, peer->index));

        Sync::ManagementReply reply;
        reply.requestID = 1;
        reply.daemonInstanceID = Screen::daemonInstanceID(daemon);
        Sync::ManagedClient listed;
        listed.label = kept;
        reply.clients.push_back(listed);
        Sync::ManagementRequest request;
        request.daemonInstanceID = reply.daemonInstanceID;
        const auto callID = Ubiquitty::Codec::JSON::Value::string("bounds");
        CHECK(Ubiquitty::Protocol::Control::hostAdministrationResponse(request, reply, &callID));
        reply.clients.front().label = label;
        CHECK(!Ubiquitty::Protocol::Control::hostAdministrationResponse(request, reply, &callID));
        Screen::drop(daemon, peer->index);
    }
    {
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        auto greeting = controlGreeting(daemon, 2);
        greeting.credential = std::vector<std::uint8_t>(longestToken.begin(), longestToken.end());
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        REQUIRE(Sync::decodeGreet(body));
        deliver(daemon, *peer, greeting, body);
        checkGranted(*peer);
        CHECK(Access::hasToken(daemon, peer->index));
        Screen::drop(daemon, peer->index);
    }
    {
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        auto greeting = controlGreeting(daemon, 3);
        greeting.credential = std::vector<std::uint8_t>(longestToken.begin(), longestToken.end());
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        const auto hostile = replacingField(body, GreetingField::Credential, overlongToken);
        REQUIRE(hostile);
        CHECK(!Sync::decodeGreet(*hostile));
        greeting.credential = std::vector<std::uint8_t>(overlongToken.begin(), overlongToken.end());
        std::vector<std::uint8_t> refused;
        Sync::encode(refused, greeting);
        CHECK(refused.empty());
        deliver(daemon, *peer, greeting, *hostile);
        checkMalformed(daemon, *peer);
    }
    {
        auto peer = connectPeer(daemon);
        REQUIRE(peer);
        auto greeting = controlGreeting(daemon, 4);
        greeting.request.target = Sync::AdmissionTarget::Session;
        const std::string longestHost(Ubiquitty::Transport::maxSSHHostLength, 'h');
        greeting.request.sshHost = longestHost;
        CHECK(Ubiquitty::Transport::validateSSHHost(longestHost));
        std::vector<std::uint8_t> body;
        Sync::encode(body, greeting);
        REQUIRE(Sync::decodeGreet(body));
        const std::string overlongHost(Ubiquitty::Transport::maxSSHHostLength + 1, 'h');
        const auto hostile = replacingField(body, GreetingField::SSHHost, overlongHost);
        REQUIRE(hostile);
        CHECK(!Sync::decodeGreet(*hostile));
        greeting.request.sshHost = overlongHost;
        std::vector<std::uint8_t> refused;
        Sync::encode(refused, greeting);
        CHECK(refused.empty());
        deliver(daemon, *peer, greeting, *hostile);
        checkMalformed(daemon, *peer);
    }
}

}

int main()
{
    char storeTemplate[] = "/tmp/hello_field_bounds.XXXXXX";
    const auto* directory = ::mkdtemp(storeTemplate);
    if (!directory)
        return 1;
    const std::string path = std::string(directory) + "/tokens";
    const std::string longestToken(Sync::maxCredentialBytes, 't');
    const std::string overlongToken(Sync::maxCredentialBytes + 1, 't');
    std::vector<Sync::TokenRecord> records;
    for (const auto* token : { &longestToken, &overlongToken }) {
        Sync::TokenRecord record;
        record.hashHex = Ubiquitty::Crypto::sha256Hex(*token);
        record.scope = Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Control };
        record.label = "bounds";
        records.push_back(std::move(record));
    }
    const auto written = Ubiquitty::Host::writeTokens(path, records);
    CHECK(written == Ubiquitty::Host::TokenWriteOutcome::Durable);
    if (written == Ubiquitty::Host::TokenWriteOutcome::Durable) {
        Daemon daemon;
        Screen::useTokenStore(daemon, path);
        fieldBounds(daemon, longestToken, overlongToken);
    }
    ::unlink(path.c_str());
    ::rmdir(directory);
    return UBIQUITTY_TEST_SUMMARY();
}
