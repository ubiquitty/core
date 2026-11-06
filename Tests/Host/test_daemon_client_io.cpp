// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Local/AdoptWire.h"
#include "Core/Protocol/Local/Clipboard.h"
#include "Core/Protocol/Local/Handoff.h"
#include "Core/Session/SharedWorkspace.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/Socket.h"
#include "PAL/Stream.h"
#include "Platform/Host/Daemon.h"
#include "Platform/POSIX/ReceivedDescriptor.h"
#include "Platform/StreamDouble.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <span>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <vector>

namespace Ubiquitty::Host {

struct DaemonClientIOAccess {
    enum class Destination {
        NewWorkspace,
        ExistingWorkspace,
    };

    static void checkSharedWindow(Daemon& daemon, Destination destination)
    {
        using namespace Ubiquitty::Session;
        auto& first = ScreenPlaneAccess::makeSession(daemon, 24, 80);
        auto& second = ScreenPlaneAccess::makeSession(daemon, 24, 80);
        first.adopted = true;
        second.adopted = true;
        const auto firstPane = daemon.hostedWorkspaceFor(first).paneFor(first.id);
        const auto secondPane = daemon.hostedWorkspaceFor(second).paneFor(second.id);
        const auto firstScreen = first.screenSource->paneID();
        const auto secondScreen = second.screenSource->paneID();
        SharedWorkspace shared;
        WorkspaceWindow window;
        window.tabs.push_back({ "split", PaneNode::makeSplit(false, 0.3, PaneNode::makeLeaf({}), PaneNode::makeLeaf({})), 1 });
        shared.arrangement.name = "shared window";
        shared.arrangement.windows.push_back(std::move(window));
        shared.sessions = { first.id, second.id };
        if (destination == Destination::ExistingWorkspace) {
            auto& existing = ScreenPlaneAccess::makeSession(daemon, 24, 80);
            shared.destinationWorkspaceID = daemon.hostedWorkspaceFor(existing).workspaceID;
        }
        daemon.m_clients[0].sessionID = first.id;
        UbiquittyTest::ScreenClient screen(24, 80);
        ScreenPlaneAccess::greet(daemon, 0, screen);
        REQUIRE(daemon.hasLiveStructureAttachment(daemon.m_clients[0]));
        const auto request = Protocol::Sync::decodeCallRequest(shared.request(daemon.m_daemonInstanceID, "share-test"));
        const auto sourceCount = daemon.m_workspaces.size();
        // A remote or unverified local peer cannot rearrange even adopted terminals.
        daemon.m_clients[0].hasOwnerLocalCredentials = false;
        CHECK(daemon.serveScreenCall(daemon.m_clients[0], request).find("error"));
        CHECK_EQ(daemon.m_workspaces.size(), sourceCount);
        daemon.m_clients[0].hasOwnerLocalCredentials = true;
        CHECK(daemon.serveScreenCall(daemon.m_clients[0], Protocol::Sync::decodeCallRequest(shared.request(daemon.m_daemonInstanceID + 1, "share-test"))).find("error"));
        CHECK_EQ(daemon.m_workspaces.size(), sourceCount);
        second.adopted = false;
        CHECK(daemon.serveScreenCall(daemon.m_clients[0], request).find("error"));
        CHECK_EQ(daemon.m_workspaces.size(), sourceCount);
        second.adopted = true;
        const auto answer = daemon.serveScreenCall(daemon.m_clients[0], request);
        REQUIRE(answer.find("result"));
        CHECK_EQ(daemon.m_workspaces.size(), 1u);
        auto* workspace = daemon.workspaceHolding(first.id);
        REQUIRE(workspace);
        CHECK(daemon.workspaceHolding(second.id) == workspace);
        CHECK_EQ(workspace->layout.surfaceCount(), destination == Destination::ExistingWorkspace ? 2 : 1);
        CHECK(workspace->paneFor(first.id) == firstPane);
        CHECK(workspace->paneFor(second.id) == secondPane);
        CHECK(first.screenSource->paneID() == firstScreen);
        CHECK(second.screenSource->paneID() == secondScreen);
        CHECK(daemon.serveScreenCall(daemon.m_clients[0], request).find("result"));
        CHECK_EQ(daemon.m_workspaces.size(), 1u);
        CHECK_EQ(workspace->layout.surfaceCount(), destination == Destination::ExistingWorkspace ? 2 : 1);
        auto changed = shared;
        changed.arrangement.name = "different retry";
        CHECK(daemon.serveScreenCall(daemon.m_clients[0], Protocol::Sync::decodeCallRequest(changed.request(daemon.m_daemonInstanceID, "share-test"))).find("error"));
        CHECK_EQ(workspace->layout.surfaceCount(), destination == Destination::ExistingWorkspace ? 2 : 1);
    }

    enum class ReplyState {
        Pending,
        Queued
    };
    enum class SourceFailure {
        Overflow,
        Draining
    };

    /// Pins the executables a local adopt may come from, as a started daemon
    /// does before it serves; the handoff cases need them, the others do not.
    static void trustLocalAdopt(Daemon& daemon) { daemon.captureLocalAdoptExecutables(); }

    static bool addClient(Daemon& daemon, PAL::UnixFileDescriptor descriptor)
    {
        Daemon::Client client;
        client.fileDescriptor = std::move(descriptor);
        client.clientID = daemon.m_nextClientID++;
        client.hasOwnerLocalCredentials = PAL::peerIsSameUser(client.fileDescriptor);
        if (!client.hasOwnerLocalCredentials)
            return false;
        daemon.capturePeerIdentity(client);
        if (!client.peerProcessID || daemon.originOf(client) != CapabilityOrigin::Local)
            return false;
        daemon.m_clients.push_back(std::move(client));
        return true;
    }

    static void seedClipboardReply(Daemon& daemon, ReplyState replyState = ReplyState::Queued)
    {
        auto stream = std::make_unique<TestDoubles::StreamDouble>();
        // Membership is modeled by this session's process-facts seam. The
        // helper's PID, uid and incarnation still come from its real socket.
        stream->facts.processes.push_back({ .processID = daemon.m_clients[1].peerProcessID });
        daemon.m_nextSessionID = 7;
        auto& session = ScreenPlaneAccess::makeSession(daemon, 2, 8, std::move(stream));
        REQUIRE(session.id == 7);
        session.lastInputClientID = daemon.m_clients[0].clientID;
        auto& source = daemon.m_clients[0];
        source.sessionID = 7;
        source.intents.add(Protocol::Sync::HelloIntent::ServesClipboard);
        UbiquittyTest::ScreenClient client(2, 8);
        ScreenPlaneAccess::greet(daemon, 0, client);
        REQUIRE(client.connection().state() == Protocol::Sync::ConnectionState::Attached);
        REQUIRE(client.welcome() && client.welcome()->selection.sessionID == 7);
        REQUIRE(daemon.hasLiveStructureAttachment(daemon.m_clients[0]));

        daemon.m_nextClipboardRequestID = 7;
        const auto request = Protocol::Local::encodeLocalClipboardRequest(17,
            { .kind = Protocol::Local::LocalClipboardKind::Item, .mime = "image/png" });
        REQUIRE(request);
        ScreenPlaneAccess::receive(daemon, 1, request->view());
        REQUIRE(!daemon.m_clients[1].didOverflow);
        REQUIRE(daemon.m_clients[1].localClipboardCorrelation == 17);
        REQUIRE(daemon.m_clients[1].clipboardToolSelectionClientID == daemon.m_clients[0].clientID);
        REQUIRE(daemon.isClipboardToolSelectionCurrent(daemon.m_clients[1]));
        REQUIRE(hasPendingClipboardRequest(daemon));
        Protocol::Sync::ClipboardDataMessage response;
        response.status = Protocol::Sync::ClipboardServeStatus::OK;
        response.items.push_back({ "image/png", { 1, 2, 3, 4 } });
        if (replyState == ReplyState::Queued) {
            daemon.finishClipboardTool(1, response);
            const auto& helper = daemon.m_clients[1];
            Protocol::Local::LocalClipboardReplyReader reader(17);
            REQUIRE(reader.consume(std::span(helper.outputBuffer).subspan(helper.outputConsumed)) == Protocol::Local::LocalClipboardRequestReader::State::Ready);
            const auto reply = reader.take();
            REQUIRE(reply && reply->daemonInstanceID == daemon.m_daemonInstanceID && !reply->isHostSelected
                && reply->status == Protocol::Sync::EffectClipboardStatus::OK && reply->bundle.count == 1
                && reply->bundle.items[0].mime == "image/png" && reply->bundle.items[0].data.size() == 4);
        }
    }

    static void setDriver(Daemon& daemon, std::uint32_t clientID)
    {
        daemon.setSessionInputDriver(*daemon.m_sessions.front(), clientID);
    }

    static bool hasPendingClipboardRequest(const Daemon& daemon)
    {
        return daemon.m_clients[1].clipboardToolRequestID == 8 && daemon.m_clients[0].pendingClipboardRequests.size() == 1;
    }

    static void failSource(Daemon& daemon, SourceFailure failure)
    {
        if (failure == SourceFailure::Draining) {
            daemon.m_clients[0].sessionExitDeadlineMilliseconds = 1;
        } else {
            daemon.m_clients[0].didOverflow = true;
        }
    }

    static void flushReply(Daemon& daemon) { daemon.flushClient(1); }
    static void reapSource(Daemon& daemon) { daemon.reapClient(0); }
    static bool replyWasDiscarded(const Daemon& daemon, std::size_t index)
    {
        return daemon.m_clients[index].didOverflow && !daemon.m_clients[index].hasPendingOutput();
    }

    static void service(Daemon& daemon)
    {
        const ScreenPlaneAccess::FixtureServingMetadata metadata(daemon);
        daemon.serviceClient(0);
        if (!daemon.m_clients[0].didOverflow)
            daemon.serviceSyncConnection(0, ScreenPlaneAccess::now());
    }
    static std::uint64_t daemonInstanceID(const Daemon& daemon) { return daemon.m_daemonInstanceID; }
    static std::size_t pendingFrameBytes(const Daemon& daemon)
    {
        return daemon.m_clients[0].syncConnection ? daemon.m_clients[0].syncConnection->pendingInboundFrameBytes() : 0;
    }
    static bool wasDropped(const Daemon& daemon) { return daemon.m_clients[0].didOverflow; }
    static int pendingDescriptor(const Daemon& daemon) { return daemon.m_clients[0].pendingAdoptFileDescriptor.get(); }
    static void clearPendingDescriptor(Daemon& daemon) { daemon.m_clients[0].pendingAdoptFileDescriptor.reset(); }
};

}

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::DaemonClientIOAccess;
using Ubiquitty::PAL::UnixFileDescriptor;

namespace {

namespace Local = Ubiquitty::Protocol::Local;
namespace Sync = Ubiquitty::Protocol::Sync;

struct SocketPair {
    UnixFileDescriptor sender;
    UnixFileDescriptor receiver;
};

SocketPair makeSocketPair()
{
    std::array<int, 2> descriptors {};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) < 0) {
        CHECK(false);
        return {};
    }
    SocketPair sockets { UnixFileDescriptor(descriptors[0]), UnixFileDescriptor(descriptors[1]) };
    for (int descriptor : descriptors) {
        if (::fcntl(descriptor, F_SETFL, O_NONBLOCK) < 0) {
            CHECK(false);
            return {};
        }
    }
    return sockets;
}

/// Sends what the local adoption client sends first on a selected exchange: the
/// kind byte of a Prepare record pinned to `daemonInstanceID`, carrying the
/// descriptors, then the rest of its frame header but one byte. The record
/// stays incomplete, so the descriptor stays with its admitted transport owner
/// and no candidate is prepared, committed, or answered here.
void sendDescriptors(int socket, std::uint64_t daemonInstanceID, std::span<const int> descriptors)
{
    static constexpr std::array<std::uint8_t, 4> terminalBody { 0x1b, 'c', 'o', 'k' };
    Local::AdoptWirePrepare prepare { .correlation = 2,
        .preparation = { .operation = { .operationID = { .low = 0x71, .high = 0x72 }, .daemonInstanceID = daemonInstanceID },
            .bodyBytes = terminalBody.size() },
        .terminalBody = terminalBody };
    Ubiquitty::Crypto::sha256(terminalBody, prepare.preparation.bodySHA256);
    const auto encoded = Local::encodeAdoptWirePrepare(prepare, Local::adoptWirePrepareHeaderBytes + terminalBody.size());
    REQUIRE(encoded);
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 3)> control {};
    iovec part { const_cast<std::uint8_t*>(encoded->header.data()), 1 };
    msghdr message {};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = CMSG_SPACE(descriptors.size_bytes());
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(descriptors.size_bytes());
    std::memcpy(CMSG_DATA(header), descriptors.data(), descriptors.size_bytes());
    REQUIRE(::sendmsg(socket, &message, 0) == 1);
    const auto rest = std::span(encoded->header).subspan(1, Local::adoptWireFrameHeaderBytes - 2);
    REQUIRE(::write(socket, rest.data(), rest.size()) == static_cast<ssize_t>(rest.size()));
}

bool selectHandoff(Daemon& daemon, int peer)
{
    constexpr std::uint64_t correlation = 31;
    const auto expectedDaemon = DaemonClientIOAccess::daemonInstanceID(daemon);
    const auto request = Local::encodeLocalHandoffRequest(correlation);
    if (!request || ::write(peer, request->data(), request->size()) != static_cast<ssize_t>(request->size()))
        return false;
    DaemonClientIOAccess::service(daemon);
    std::array<std::uint8_t, Local::localControlReplyBytes> bytes;
    alignas(cmsghdr) std::array<char, Ubiquitty::POSIX::receivedDescriptorControlCapacity> control;
    iovec part { bytes.data(), bytes.size() };
    msghdr message {};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    if (::recvmsg(peer, &message, Ubiquitty::POSIX::receivedDescriptorFlags) != static_cast<ssize_t>(bytes.size()))
        return false;
    const auto descriptor = Ubiquitty::POSIX::takeReceivedDescriptor(message);
    if (!descriptor || *descriptor)
        return false;
    const auto ready = Local::decodeLocalHandoffReady(bytes);
    return ready && ready->correlation == correlation && ready->daemonInstanceID == expectedDaemon
        && ready->status == Local::HandoffReadyStatus::Ready && !DaemonClientIOAccess::wasDropped(daemon);
}

bool greetControl(Daemon& daemon, int peer)
{
    const auto expectedDaemon = DaemonClientIOAccess::daemonInstanceID(daemon);
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = { .low = 1, .high = 0x696f };
    greeting.request = { .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = expectedDaemon };
    std::vector<std::uint8_t> body, wire;
    Sync::encode(body, greeting);
    if (body.empty() || !Sync::decodeGreet(body) || !Sync::appendFrame(wire, Sync::FrameKind::Greet, 0, body)
        || ::write(peer, wire.data(), wire.size()) != static_cast<ssize_t>(wire.size()))
        return false;
    DaemonClientIOAccess::service(daemon);
    const auto* connection = Ubiquitty::Host::ScreenPlaneAccess::connection(daemon, 0);
    if (DaemonClientIOAccess::wasDropped(daemon) || !connection || connection->state() != Sync::ConnectionState::Attached)
        return false;
    const auto output = Ubiquitty::Host::ScreenPlaneAccess::takeOutput(daemon, 0);
    Sync::FrameDecoder decoder;
    decoder.append(output);
    unsigned welcomes = 0;
    while (const auto frame = decoder.next()) {
        if (frame->kind != Sync::FrameKind::Welcome)
            continue;
        const auto welcome = Sync::decodeWelcome(frame->body);
        if (!welcome || welcome->selection.target != Sync::AdmissionTarget::Control
            || welcome->selection.sessionID || welcome->selection.workspaceID
            || welcome->selection.daemonInstanceID != expectedDaemon)
            return false;
        ++welcomes;
    }
    return welcomes == 1 && decoder.error() == Sync::EnvelopeError::None && !decoder.bufferedBytes();
}

void checkExcessDescriptors(std::size_t count)
{
    auto sockets = makeSocketPair();
    REQUIRE(sockets.sender && sockets.receiver);
    auto pipe = Ubiquitty::PAL::makeUnixPipe();
    REQUIRE(pipe.isValid());
    REQUIRE(::fcntl(pipe.read.get(), F_SETFL, O_NONBLOCK) == 0);
    Daemon daemon;
    DaemonClientIOAccess::trustLocalAdopt(daemon);
    REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(sockets.receiver)));
    REQUIRE(selectHandoff(daemon, sockets.sender.get()));
    const std::array<int, 3> descriptors { pipe.write.get(), pipe.write.get(), pipe.write.get() };
    sendDescriptors(sockets.sender.get(), DaemonClientIOAccess::daemonInstanceID(daemon), std::span(descriptors).first(count));
    pipe.write.reset();
    DaemonClientIOAccess::service(daemon);
    CHECK(DaemonClientIOAccess::wasDropped(daemon));
    CHECK_EQ(DaemonClientIOAccess::pendingDescriptor(daemon), -1);
    DaemonClientIOAccess::clearPendingDescriptor(daemon);
    char byte = 0;
    // EOF proves every received write end was closed, including padding slots.
    CHECK_EQ(::read(pipe.read.get(), &byte, 1), 0);
}

}

TEST(sharedWindowPublishesExistingPTYsOnceWithLocalAuthority)
{
    for (const bool usesExistingWorkspace : { false, true }) {
        auto sockets = makeSocketPair();
        REQUIRE(sockets.sender && sockets.receiver);
        Daemon daemon;
        DaemonClientIOAccess::trustLocalAdopt(daemon);
        REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(sockets.receiver)));
        DaemonClientIOAccess::checkSharedWindow(daemon, usesExistingWorkspace ? DaemonClientIOAccess::Destination::ExistingWorkspace : DaemonClientIOAccess::Destination::NewWorkspace);
    }
}

TEST(testOneHandoffDescriptorIsOwnedAndCloseOnExec)
{
    auto sockets = makeSocketPair();
    REQUIRE(sockets.sender && sockets.receiver);
    auto pipe = Ubiquitty::PAL::makeUnixPipe();
    REQUIRE(pipe.isValid());
    REQUIRE(::fcntl(pipe.read.get(), F_SETFL, O_NONBLOCK) == 0);
    Daemon daemon;
    DaemonClientIOAccess::trustLocalAdopt(daemon);
    REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(sockets.receiver)));
    REQUIRE(selectHandoff(daemon, sockets.sender.get()));
    const int descriptor = pipe.write.get();
    sendDescriptors(sockets.sender.get(), DaemonClientIOAccess::daemonInstanceID(daemon), std::span(&descriptor, 1));
    pipe.write.reset();
    DaemonClientIOAccess::service(daemon);
    CHECK(!DaemonClientIOAccess::wasDropped(daemon));
    const int received = DaemonClientIOAccess::pendingDescriptor(daemon);
    REQUIRE(received >= 0);
    CHECK((::fcntl(received, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK_EQ(::write(received, "x", 1), 1);
    char byte = 0;
    CHECK_EQ(::read(pipe.read.get(), &byte, 1), 1);
    CHECK_EQ(byte, 'x');
    DaemonClientIOAccess::clearPendingDescriptor(daemon);
    CHECK_EQ(::read(pipe.read.get(), &byte, 1), 0);
}

TEST(testHandoffDescriptorBeforeReadyIsRejectedAndClosed)
{
    auto sockets = makeSocketPair();
    REQUIRE(sockets.sender && sockets.receiver);
    auto pipe = Ubiquitty::PAL::makeUnixPipe();
    REQUIRE(pipe.isValid());
    REQUIRE(::fcntl(pipe.read.get(), F_SETFL, O_NONBLOCK) == 0);
    Daemon daemon;
    REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(sockets.receiver)));
    const int descriptor = pipe.write.get();
    sendDescriptors(sockets.sender.get(), DaemonClientIOAccess::daemonInstanceID(daemon), std::span(&descriptor, 1));
    pipe.write.reset();
    DaemonClientIOAccess::service(daemon);
    CHECK(DaemonClientIOAccess::wasDropped(daemon));
    CHECK_EQ(DaemonClientIOAccess::pendingDescriptor(daemon), -1);
    char byte = 0;
    CHECK_EQ(::read(pipe.read.get(), &byte, 1), 0);
}

TEST(testUnknownCanonicalFrameIsRefused)
{
    auto sockets = makeSocketPair();
    REQUIRE(sockets.sender && sockets.receiver);
    Daemon daemon;
    REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(sockets.receiver)));
    constexpr std::uint8_t unknown = 0xFF;
    REQUIRE(::write(sockets.sender.get(), &unknown, sizeof(unknown)) == static_cast<ssize_t>(sizeof(unknown)));
    DaemonClientIOAccess::service(daemon);
    CHECK(DaemonClientIOAccess::wasDropped(daemon));
    std::array<std::uint8_t, 4096> bytes;
    const auto count = ::read(sockets.sender.get(), bytes.data(), bytes.size());
    REQUIRE(count > 0);
    Sync::EnvelopeError error {};
    const auto frame = Sync::decodeDatagram(std::span(bytes).first(static_cast<std::size_t>(count)), error);
    REQUIRE(frame && frame->kind == Sync::FrameKind::Bye && !frame->channel);
    const auto bye = Sync::decodeBye(frame->body);
    // A kind byte the catalog does not define is its own refusal, apart from
    // a frame whose grammar the catalog knows and the bytes break.
    CHECK(bye && bye->reason == Sync::ByeReason::UnknownKind);
}

TEST(testTwoHandoffDescriptorsAreRejectedAndClosed)
{
    checkExcessDescriptors(2);
}

TEST(testThreeHandoffDescriptorsAreRejectedAndClosed)
{
    checkExcessDescriptors(3);
}

TEST(testTruncatedAncillaryMessageClosesDeliveredDescriptor)
{
    auto pipe = Ubiquitty::PAL::makeUnixPipe();
    REQUIRE(pipe.isValid());
    REQUIRE(::fcntl(pipe.read.get(), F_SETFL, O_NONBLOCK) == 0);
    const int delivered = pipe.write.release();
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control {};
    msghdr message {};
    message.msg_flags = MSG_CTRUNC;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &delivered, sizeof(delivered));
    CHECK(!Ubiquitty::POSIX::takeReceivedDescriptor(message));
    char byte = 0;
    CHECK_EQ(::read(pipe.read.get(), &byte, 1), 0);
}

TEST(testClientReadPassYieldsWithoutDroppingQueuedFrames)
{
    auto sockets = makeSocketPair();
    REQUIRE(sockets.sender && sockets.receiver);
    const int capacity = 1024 * 1024;
    REQUIRE(::setsockopt(sockets.sender.get(), SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0);
    REQUIRE(::setsockopt(sockets.receiver.get(), SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity)) == 0);
    const int receiver = sockets.receiver.get();
    Daemon daemon;
    REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(sockets.receiver)));
    REQUIRE(greetControl(daemon, sockets.sender.get()));
    std::vector<std::uint8_t> body, frame, frames;
    Sync::encode(body, Sync::Facts { .viewportRows = 24, .viewportColumns = 80 });
    REQUIRE(Sync::appendFrame(frame, Sync::FrameKind::Facts, 0, body));
    while (frames.size() < 5 * 32768)
        frames.insert(frames.end(), frame.begin(), frame.end());
    REQUIRE(::write(sockets.sender.get(), frames.data(), frames.size()) == static_cast<ssize_t>(frames.size()));
    DaemonClientIOAccess::service(daemon);
    int pending = 0;
    REQUIRE(::ioctl(receiver, FIONREAD, &pending) == 0);
    CHECK(pending > 0);
    for (int pass = 0; pending > 0 && pass < 32; ++pass) {
        DaemonClientIOAccess::service(daemon);
        REQUIRE(::ioctl(receiver, FIONREAD, &pending) == 0);
    }
    CHECK_EQ(pending, 0);
    CHECK_EQ(DaemonClientIOAccess::pendingFrameBytes(daemon), 0u);
    CHECK(!DaemonClientIOAccess::wasDropped(daemon));
    std::vector<std::uint8_t> goodbyeBody, exitFrame;
    Sync::encode(goodbyeBody, Sync::Bye { .reason = Sync::ByeReason::Normal, .detail = {}, .peer = std::nullopt });
    REQUIRE(Sync::appendFrame(exitFrame, Sync::FrameKind::Bye, 0, goodbyeBody));
    REQUIRE(::write(sockets.sender.get(), exitFrame.data(), exitFrame.size()) == static_cast<ssize_t>(exitFrame.size()));
    DaemonClientIOAccess::service(daemon);
    CHECK(DaemonClientIOAccess::wasDropped(daemon));
}

TEST(clipboardReplyStopsWhenSourceOverflowsOrDrains)
{
    for (const auto failure : { DaemonClientIOAccess::SourceFailure::Overflow, DaemonClientIOAccess::SourceFailure::Draining }) {
        auto source = makeSocketPair();
        auto helper = makeSocketPair();
        Daemon daemon;
        REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(source.receiver)));
        REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(helper.receiver)));
        DaemonClientIOAccess::seedClipboardReply(daemon);
        DaemonClientIOAccess::failSource(daemon, failure);
        DaemonClientIOAccess::flushReply(daemon);
        CHECK(DaemonClientIOAccess::replyWasDiscarded(daemon, 1));
        char byte;
        CHECK(::read(helper.sender.get(), &byte, 1) == -1);
        CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

TEST(reapingClipboardSourceDiscardsQueuedReply)
{
    auto source = makeSocketPair();
    auto helper = makeSocketPair();
    Daemon daemon;
    REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(source.receiver)));
    REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(helper.receiver)));
    DaemonClientIOAccess::seedClipboardReply(daemon);
    DaemonClientIOAccess::reapSource(daemon);
    CHECK(DaemonClientIOAccess::replyWasDiscarded(daemon, 0));
}

TEST(unchangedClipboardDriverRetainsPendingRequest)
{
    auto source = makeSocketPair();
    auto helper = makeSocketPair();
    Daemon daemon;
    REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(source.receiver)));
    REQUIRE(DaemonClientIOAccess::addClient(daemon, std::move(helper.receiver)));
    DaemonClientIOAccess::seedClipboardReply(daemon, DaemonClientIOAccess::ReplyState::Pending);
    DaemonClientIOAccess::setDriver(daemon, 1);
    CHECK(DaemonClientIOAccess::hasPendingClipboardRequest(daemon));
    DaemonClientIOAccess::setDriver(daemon, 3);
    CHECK(!DaemonClientIOAccess::hasPendingClipboardRequest(daemon));
}

UBIQUITTY_TEST_MAIN()
