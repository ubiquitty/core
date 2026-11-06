// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/ssh_relay_origin.cpp — a client that reached the host over SSH is
// gated as a remote one.
//
// A client on another machine attaches by having sshd run this daemon's own
// bridge, so the daemon sees its own executable on the socket and, by binary
// identity alone, a Local peer. SSH authenticated that person, so the owner's
// authority is right; but what the sessions they drive may do beyond the grid —
// read a file, write the clipboard, post a notification — is a remote client's
// question. The bridge marks the connection before the client's first byte and
// the daemon gates the client's sessions at the Remote origin from then on,
// while its authority stays the owner's, and every check asking whether the
// person is on this host answers no.
//
// The daemon acknowledges every mark from a same-user stream and never ends a
// connection over one, naming whether it applied it. The bridge relays only on
// a mark the daemon applied: a daemon that closes, answers anything else or
// says it did not apply the mark is refused, so a person on another machine is
// never attached as one here or as a program here.
//
// Each case drives the production ingress: the mark's exact bytes through
// feedClientFrames, then a real Greet and real typing through a ScreenClient.
// The bridge's own half runs over a socket pair against a scripted daemon.
//
// Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Protocol/Local/Control.h"
#include "Core/Terminal.h"
#include "PAL/Capability.h"
#include "Platform/Host/AttachRelay.h"
#include "Platform/Host/Daemon.h"
#include "ScreenClient.h"
#include "ScreenPlaneAccess.h"
#include "TrustedLocalPeer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty::Host {

struct SSHRelayOriginAccess {
    // A client on a trusted local stream: the uid the accept verified and the
    // daemon's own binary, which is what a bridge is.
    static std::size_t addBridgeClient(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client c;
        c.clientID = d.m_nextClientID++;
        c.connectMilliseconds = ScreenPlaneAccess::acceptedAt();
        c.hasOwnerLocalCredentials = true;
        c.sessionID = sessionID;
        seedTrustedLocalPeer(c);
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    // A same-user client the daemon does not recognise as its own binary.
    static std::size_t addUnrecognisedClient(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client c;
        c.clientID = d.m_nextClientID++;
        c.connectMilliseconds = ScreenPlaneAccess::acceptedAt();
        c.hasOwnerLocalCredentials = true;
        c.sessionID = sessionID;
        c.peerProcess = "/usr/bin/true";
        c.hasPeerBinary = binaryIdentityOf(c.peerProcess, c.peerBinary);
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    // Takes the acknowledgement the daemon queued ahead of the carrier's own
    // output, as the bridge reads it before relaying.
    static std::optional<Protocol::Local::LocalRemoteRelayReply> takeRelayReply(Daemon& d, std::size_t index)
    {
        Daemon::Client& client = d.m_clients[index];
        if (client.outputBuffer.size() - client.outputConsumed < Protocol::Local::localControlReplyBytes)
            return std::nullopt;
        const auto reply = Protocol::Local::decodeLocalRemoteRelayReply(std::span(client.outputBuffer).subspan(client.outputConsumed, Protocol::Local::localControlReplyBytes));
        if (reply)
            client.outputConsumed += Protocol::Local::localControlReplyBytes;
        return reply;
    }

    static std::unique_ptr<UbiquittyTest::ScreenClient> attach(Daemon& d, std::size_t index, bool isMarked, std::optional<Protocol::Local::LocalRemoteRelayReply>* acknowledgement = nullptr) // bool-ok: the case's one variable
    {
        auto peer = std::make_unique<UbiquittyTest::ScreenClient>(24, 80, Protocol::Sync::ID128 { .low = d.m_clients[index].clientID, .high = 0x7373 });
        const auto request = ScreenPlaneAccess::selectedRequest(d, index);
        if (!request || !peer->start(*request))
            return nullptr;
        d.m_clients[index].fileDescriptor = peer->takeCarrier();
        std::vector<std::uint8_t> bytes;
        if (isMarked) {
            const auto mark = Protocol::Local::encodeLocalRemoteRelayRequest(1);
            if (!mark)
                return nullptr;
            bytes.assign(mark->begin(), mark->end());
        }
        const auto stream = peer->takeStream(ScreenPlaneAccess::now());
        bytes.insert(bytes.end(), stream.begin(), stream.end());
        ScreenPlaneAccess::receive(d, index, bytes);
        if (isMarked && !isDropped(d, index)) {
            const auto reply = takeRelayReply(d, index);
            if (acknowledgement)
                *acknowledgement = reply;
        }
        if (!isDropped(d, index))
            ScreenPlaneAccess::deliver(d, index, *peer);
        return peer;
    }

    // Sends the mark alone on a carrier, as the bridge does before it relays,
    // and takes the acknowledgement; the carrier stays with the client.
    static std::optional<Protocol::Local::LocalRemoteRelayReply> markAlone(Daemon& d, std::size_t index, UbiquittyTest::ScreenClient& peer)
    {
        d.m_clients[index].fileDescriptor = peer.takeCarrier();
        const auto mark = Protocol::Local::encodeLocalRemoteRelayRequest(1);
        if (!mark)
            return std::nullopt;
        ScreenPlaneAccess::receive(d, index, *mark);
        return isDropped(d, index) ? std::nullopt : takeRelayReply(d, index);
    }

    static void type(Daemon& d, std::size_t index, UbiquittyTest::ScreenClient& peer)
    {
        constexpr std::string_view text = "x";
        (void)peer.sendRaw({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
        ScreenPlaneAccess::deliver(d, index, peer);
    }

    static bool isRelayed(const Daemon& d, std::size_t index) { return d.m_clients[index].isRelayedFromSSH; }
    static bool isDropped(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }
    static bool isAttached(const Daemon& d, std::size_t index, std::uint32_t sessionID) { return d.m_clients[index].subscriptionFor(sessionID) != nullptr; }
    static CapabilityOrigin authority(const Daemon& d, std::size_t index) { return d.originOf(d.m_clients[index]); }
    static bool isPerson(const Daemon& d, std::size_t index) { return !d.isAgentClient(d.m_clients[index]); }
    static bool mayManage(const Daemon& d, std::size_t index) { return d.tokenScopeAllows(d.m_clients[index], Protocol::Sync::Scope::Manage); }
    static bool isTrustedLocalPerson(const Daemon& d, std::size_t index) { return d.localControlPeer(d.m_clients[index]).isTrustedOwnerBinary; }
    static bool continuesAsCarrier(Daemon& d, std::size_t index)
    {
        std::array<std::uint8_t, 1> ordinary { 0x00 };
        return d.m_clients[index].localControl.consume(ordinary) == Protocol::Local::LocalControlReader::State::Ordinary;
    }
    static std::uint64_t daemonInstance(const Daemon& d) { return d.m_daemonInstanceID; }
    static CapabilityOrigin sessionOrigin(Daemon& d, std::uint32_t sessionID) { return d.findSession(sessionID)->term->currentCapabilityOrigin; }
    static CapabilityOrigin sessionFloor(Daemon& d, std::uint32_t sessionID) { return d.findSession(sessionID)->driverOriginFloor; }
    static bool mayReadFiles(Daemon& d, std::uint32_t sessionID) { return effectAllowed(*d.findSession(sessionID)->term, Capability::FileReadAccess, "/tmp/x"); }
};

} // namespace Ubiquitty::Host

using Ubiquitty::CapabilityOrigin;
using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::ScreenPlaneAccess;
using Ubiquitty::Host::SSHRelayOriginAccess;

namespace {

// The bridge decides from its own environment, which sshd fills for every
// session it starts and no application on this machine sets.
void theBridgeMarksOnlyAnSSHStartedRelay()
{
    CHECK(Ubiquitty::Host::isStartedBySSH("192.0.2.1 52000 192.0.2.2 22", nullptr));
    CHECK(Ubiquitty::Host::isStartedBySSH(nullptr, "192.0.2.1 52000 22"));
    CHECK(!Ubiquitty::Host::isStartedBySSH(nullptr, nullptr));
    CHECK(!Ubiquitty::Host::isStartedBySSH("", ""));
}

// The SSH-relayed client keeps the owner's authority and drives its session at
// the Remote origin, so a file read Local allows is refused; an unmarked bridge
// client on the same session drives it at Local, where the read is allowed.
void aClientRelayedFromSSHIsDeniedWhatLocalAllows()
{
    {
        Daemon d;
        const std::uint32_t sessionID = ScreenPlaneAccess::makeSession(d, 24, 80).id;
        const std::size_t relayed = SSHRelayOriginAccess::addBridgeClient(d, sessionID);
        std::optional<Ubiquitty::Protocol::Local::LocalRemoteRelayReply> acknowledgement;
        auto peer = SSHRelayOriginAccess::attach(d, relayed, true, &acknowledgement);
        CHECK(peer != nullptr);
        if (!peer)
            return;
        CHECK_MSG(acknowledgement && acknowledgement->isMarked && acknowledgement->correlation == 1
                && acknowledgement->daemonInstanceID == SSHRelayOriginAccess::daemonInstance(d),
            "the daemon did not acknowledge its own bridge's mark ahead of the carrier");
        CHECK_MSG(SSHRelayOriginAccess::isRelayed(d, relayed), "the bridge's mark was not taken");
        CHECK_MSG(SSHRelayOriginAccess::isAttached(d, relayed, sessionID), "the client was not attached after its mark");
        // Authority is the owner's: SSH authenticated this person.
        CHECK(SSHRelayOriginAccess::authority(d, relayed) == CapabilityOrigin::Local);
        CHECK(SSHRelayOriginAccess::isPerson(d, relayed));
        CHECK(SSHRelayOriginAccess::mayManage(d, relayed));
        // Whether the person is here is the Remote answer at every check.
        CHECK_MSG(!SSHRelayOriginAccess::isTrustedLocalPerson(d, relayed), "a person relayed from SSH passed as a person on this host");
        SSHRelayOriginAccess::type(d, relayed, *peer);
        CHECK_MSG(SSHRelayOriginAccess::sessionOrigin(d, sessionID) == CapabilityOrigin::Remote,
            "a session driven over SSH was not gated as a remote client's");
        CHECK(SSHRelayOriginAccess::sessionFloor(d, sessionID) == CapabilityOrigin::Remote);
        CHECK_MSG(!SSHRelayOriginAccess::mayReadFiles(d, sessionID), "a session driven over SSH read a file Local may read");
    }
    {
        Daemon d;
        const std::uint32_t sessionID = ScreenPlaneAccess::makeSession(d, 24, 80).id;
        const std::size_t local = SSHRelayOriginAccess::addBridgeClient(d, sessionID);
        auto peer = SSHRelayOriginAccess::attach(d, local, false);
        CHECK(peer != nullptr);
        if (!peer)
            return;
        CHECK(!SSHRelayOriginAccess::isRelayed(d, local));
        CHECK(SSHRelayOriginAccess::isTrustedLocalPerson(d, local));
        SSHRelayOriginAccess::type(d, local, *peer);
        CHECK(SSHRelayOriginAccess::sessionOrigin(d, sessionID) == CapabilityOrigin::Local);
        CHECK_MSG(SSHRelayOriginAccess::mayReadFiles(d, sessionID), "the control: a local driver may read the file");
    }
}

// A mark only lowers, so it never ends a connection: another same-user
// program's is acknowledged and ignored, and the rest of its connection is its
// ordinary carrier, unlowered and already less than Local.
void anotherProgramsMarkIsAcknowledgedAndIgnored()
{
    Daemon d;
    const std::uint32_t sessionID = ScreenPlaneAccess::makeSession(d, 24, 80).id;
    const std::size_t stranger = SSHRelayOriginAccess::addUnrecognisedClient(d, sessionID);
    UbiquittyTest::ScreenClient peer(24, 80, Ubiquitty::Protocol::Sync::ID128 { .low = 1, .high = 0x7373 });
    const auto acknowledgement = SSHRelayOriginAccess::markAlone(d, stranger, peer);
    CHECK_MSG(!SSHRelayOriginAccess::isDropped(d, stranger), "a lowering-only mark ended the connection");
    CHECK(acknowledgement && !acknowledgement->isMarked && acknowledgement->correlation == 1);
    CHECK_MSG(acknowledgement && acknowledgement->daemonInstanceID == 0, "the daemon named its instance to a program it did not recognise");
    CHECK(!SSHRelayOriginAccess::isRelayed(d, stranger));
    CHECK(SSHRelayOriginAccess::authority(d, stranger) != CapabilityOrigin::Local);
    CHECK_MSG(SSHRelayOriginAccess::continuesAsCarrier(d, stranger), "the connection after an ignored mark is not the client's carrier");
}

// Reads one RemoteRelay mark from a scripted daemon's end.
bool readRelayMark(int fileDescriptor)
{
    namespace Local = Ubiquitty::Protocol::Local;
    std::array<std::uint8_t, Local::localControlRequestBytes> mark {};
    std::size_t received = 0;
    while (received < mark.size()) {
        const ssize_t count = ::read(fileDescriptor, mark.data() + received, mark.size() - received);
        if (count <= 0)
            return false;
        received += static_cast<std::size_t>(count);
    }
    Local::LocalControlReader reader;
    if (reader.consume(mark) != Local::LocalControlReader::State::Ready)
        return false;
    const auto request = reader.take();
    return request && request->operation == Local::LocalControlOperation::RemoteRelay;
}

// Runs the bridge's half against a scripted daemon on a socket pair. The
// script writes what the daemon would have sent, or shuts the daemon's writing
// side as a daemon that closes does, before the bridge runs; the socket holds
// it until the bridge reads.
template<typename Script>
Ubiquitty::Host::RelayMarkOutcome markAgainst(Script daemon, std::chrono::milliseconds budget, std::vector<std::uint8_t>* after = nullptr)
{
    std::array<int, 2> ends { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) != 0)
        return Ubiquitty::Host::RelayMarkOutcome::Unconfirmed;
    daemon(ends[1]);
    const auto outcome = Ubiquitty::Host::markRelayedFromSSH(ends[0], budget);
    CHECK_MSG(readRelayMark(ends[1]), "the bridge did not send its mark");
    ::close(ends[1]);
    if (after) {
        std::array<std::uint8_t, 64> buffer {};
        for (ssize_t count; (count = ::read(ends[0], buffer.data(), buffer.size())) > 0;)
            after->insert(after->end(), buffer.begin(), buffer.begin() + count);
    }
    ::close(ends[0]);
    return outcome;
}

// The bridge's half, against a scripted daemon: it relays only on an applied
// mark and reads nothing past its acknowledgement. A daemon that closes before
// acknowledging, whether it is older than the mark or a current one that
// failed, one that answers something else, one that acknowledges without
// applying the mark, and one that never answers all refuse the attach.
void theBridgeAttachesOnlyOnAnAppliedMark()
{
    using Ubiquitty::Host::RelayMarkOutcome;
    namespace Local = Ubiquitty::Protocol::Local;
    constexpr std::chrono::milliseconds budget { 2000 };
    const std::vector<std::uint8_t> carrier { 0x01, 0x02, 0x03, 0x04 };
    const auto acknowledges = [&](bool isMarked) { // bool-ok: the script's one variable
        return [&carrier, isMarked](int fileDescriptor) {
            const auto reply = Local::encodeLocalRemoteRelayReply({ .correlation = 1, .daemonInstanceID = isMarked ? 9u : 0u, .isMarked = isMarked });
            std::vector<std::uint8_t> bytes(reply->begin(), reply->end());
            bytes.insert(bytes.end(), carrier.begin(), carrier.end());
            CHECK_EQ(static_cast<std::size_t>(::write(fileDescriptor, bytes.data(), bytes.size())), bytes.size());
            ::shutdown(fileDescriptor, SHUT_WR);
        };
    };
    const auto closesOnTheMark = [](int fileDescriptor) { ::shutdown(fileDescriptor, SHUT_WR); };
    const auto answersSomethingElse = [](int fileDescriptor) {
        const auto reload = Local::encodeLocalControlReply({ .correlation = 1, .daemonInstanceID = 9, .status = Local::LocalControlStatus::Reloaded });
        CHECK_EQ(static_cast<std::size_t>(::write(fileDescriptor, reload.data(), reload.size())), reload.size());
        ::shutdown(fileDescriptor, SHUT_WR);
    };
    std::vector<std::uint8_t> after;
    CHECK(markAgainst(acknowledges(true), budget, &after) == RelayMarkOutcome::Marked);
    CHECK_MSG(after == carrier, "the bridge read past the acknowledgement into the carrier");
    CHECK_MSG(markAgainst(closesOnTheMark, budget) == RelayMarkOutcome::Unconfirmed, "a daemon that closed before acknowledging let the bridge attach unmarked");
    CHECK_MSG(markAgainst(acknowledges(false), budget) == RelayMarkOutcome::Unrecognised, "a mark the daemon did not apply let the bridge attach");
    CHECK(markAgainst(answersSomethingElse, budget) == RelayMarkOutcome::Unconfirmed);
    CHECK(markAgainst([](int) { }, std::chrono::milliseconds(100)) == RelayMarkOutcome::Unconfirmed);
    // The refusal says only what changes the outcome. This binary's own daemon
    // is this version, so connecting again is the remedy; a daemon another
    // socket named may be older, and starting this binary's daemon, quoted for
    // the shell, takes over its sessions.
    const std::string own = Ubiquitty::Host::relayMarkRefusal(RelayMarkOutcome::Unconfirmed, "/opt/ubiquitty/ubiquitty-host", Ubiquitty::Host::AttachedDaemon::OwnBinary);
    CHECK(own.starts_with("The host didn’t confirm") && own.ends_with("Try again."));
    CHECK_MSG(own.find("--ensure-daemon") == std::string::npos, "the refusal from this version's own daemon offered a restart that does nothing");
    const std::string named = Ubiquitty::Host::relayMarkRefusal(RelayMarkOutcome::Unconfirmed, "/opt/ubi quitty/ubiquitty-host", Ubiquitty::Host::AttachedDaemon::Named);
    CHECK_MSG(named.find("run '/opt/ubi quitty/ubiquitty-host' --ensure-daemon on the host") != std::string::npos, "the printed command was not shell-quoted");
    CHECK(named.find("without ending them") != std::string::npos);
    // A pinned daemon that does not recognise this program may be an older
    // one, which this binary's own daemon takes over; its own daemon not
    // recognising it is not something a restart changes.
    const std::string stranger = Ubiquitty::Host::relayMarkRefusal(RelayMarkOutcome::Unrecognised, "/opt/b", Ubiquitty::Host::AttachedDaemon::Named);
    CHECK(stranger.starts_with("The host didn’t recognize"));
    CHECK_MSG(stranger.find("run '/opt/b' --ensure-daemon on the host") != std::string::npos, "an unrecognised bridge at a pinned daemon was offered no remedy");
    const std::string ownStranger = Ubiquitty::Host::relayMarkRefusal(RelayMarkOutcome::Unrecognised, "/opt/b", Ubiquitty::Host::AttachedDaemon::OwnBinary);
    // A pin that names this binary's own leaf is this version's daemon, and
    // gets its advice rather than an older version's.
    CHECK_MSG(Ubiquitty::Host::attachedDaemonOf("host-0123456789abcdef.sock", "host-0123456789abcdef.sock") == Ubiquitty::Host::AttachedDaemon::OwnBinary,
        "a pin naming this binary's own leaf was taken for another version's daemon");
    CHECK(Ubiquitty::Host::attachedDaemonOf("host-fedcba9876543210.sock", "host-0123456789abcdef.sock") == Ubiquitty::Host::AttachedDaemon::Named);
    CHECK(Ubiquitty::Host::attachedDaemonOf("host-0123456789abcdef.sock", "") == Ubiquitty::Host::AttachedDaemon::Named);
    CHECK(ownStranger.starts_with("The host didn’t recognize") && ownStranger.ends_with("wasn’t attached.") && ownStranger.find("--ensure-daemon") == std::string::npos);
}

// The write is bounded too: a daemon that never reads, on a blocking
// descriptor whose buffer is already full, refuses the attach within the
// budget instead of holding the bridge.
void theBridgesMarkNeverBlocksPastItsBudget()
{
    std::array<int, 2> ends { -1, -1 };
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0);
    const int flags = ::fcntl(ends[0], F_GETFL, 0);
    REQUIRE(::fcntl(ends[0], F_SETFL, flags | O_NONBLOCK) == 0);
    const std::array<char, 4096> filler {};
    while (::write(ends[0], filler.data(), filler.size()) > 0) {
    }
    while (::write(ends[0], filler.data(), 1) > 0) {
    }
    REQUIRE(::fcntl(ends[0], F_SETFL, flags) == 0);
    const auto started = std::chrono::steady_clock::now();
    const auto outcome = Ubiquitty::Host::markRelayedFromSSH(ends[0], std::chrono::milliseconds(200));
    const auto waited = std::chrono::steady_clock::now() - started;
    CHECK(outcome == Ubiquitty::Host::RelayMarkOutcome::Unconfirmed);
    CHECK_MSG(waited < std::chrono::seconds(2), "the bridge's mark blocked past its budget on a full blocking descriptor");
    ::close(ends[0]);
    ::close(ends[1]);
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    theBridgeMarksOnlyAnSSHStartedRelay();
    aClientRelayedFromSSHIsDeniedWhatLocalAllows();
    anotherProgramsMarkIsAcknowledgedAndIgnored();
    theBridgeAttachesOnlyOnAnAppliedMark();
    theBridgesMarkNeverBlocksPastItsBudget();
    if (failures == 0)
        std::printf("ssh_relay_origin: a client the bridge relays from SSH keeps the owner's authority, drives its session at the Remote origin, is refused a file read Local allows and is no person on this host; only the daemon's own bridge is lowered, every mark is acknowledged, and the bridge attaches only on a mark the daemon applied\n");
    return UBIQUITTY_TEST_SUMMARY();
}
