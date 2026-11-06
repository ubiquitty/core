// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Canonical partial frames own silence deadlines. Typed clipboard assemblies
// own bounded capacity, including during delivery, under the daemon ledger.

#include "Assertions.h"

#include "Core/Protocol/Local/Clipboard.h"
#include "Core/Protocol/Sync/EffectCodec.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/StreamDouble.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace Ubiquitty::Host {

/// Reaches existing authority and accounting, never replacing their policies.
struct InboundHoldsAccess {
    static bool authenticateLocalHelper(Daemon& daemon, std::size_t index)
    {
        auto& client = daemon.m_clients.at(index);
        uid_t uid = 0;
#if defined(__APPLE__) || defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
        gid_t group = 0;
        const bool isVerified = ::getpeereid(client.fileDescriptor.get(), &uid, &group) == 0;
#elif defined(__linux__)
        struct ucred credentials {};
        socklen_t length = sizeof(credentials);
        const bool isVerified = ::getsockopt(client.fileDescriptor.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0
            && length == sizeof(credentials);
        uid = credentials.uid;
#else
        const bool isVerified = false;
#endif
        if (!isVerified || uid != ::geteuid())
            return false;
        client.hasOwnerLocalCredentials = true;
        daemon.capturePeerIdentity(client);
        return client.peerProcessID == static_cast<std::uint32_t>(::getpid()) && !client.hasPeerProcessInstanceMismatch;
    }

    static std::uint32_t requestID(const Daemon& daemon, std::size_t helper, std::uint32_t session)
    {
        const auto& client = daemon.m_clients.at(helper);
        return client.clipboardToolSessionID == session ? client.clipboardToolRequestID : 0;
    }

    static std::uint32_t paneHandle(const Daemon& daemon, std::size_t source, std::uint32_t session)
    {
        const auto* subscription = daemon.m_clients.at(source).subscriptionFor(session);
        return subscription ? subscription->screenHandle : 0;
    }

    static Protocol::Sync::HostConnection& connection(Daemon& daemon, std::size_t index)
    {
        return *daemon.m_clients.at(index).syncConnection;
    }

    /// Defers completed-reply delivery, retaining real authority/budget hooks.
    static void queue(Daemon& daemon, std::size_t index, std::span<const std::uint8_t> bytes)
    {
        connection(daemon, index).receiveStream(bytes, ScreenPlaneAccess::now());
        daemon.noteInboundDecoder(index);
    }

    static std::size_t aggregate(const Daemon& daemon)
    {
        std::size_t bytes = 0;
        for (const auto& client : daemon.m_clients)
            bytes += Daemon::heldInboundBytes(client);
        return bytes;
    }

    /// Queries the actual ledger allowance without allocating or changing it.
    static std::size_t remaining(const Daemon& daemon, std::size_t index)
    {
        std::size_t low = 0;
        std::size_t high = std::numeric_limits<std::size_t>::max();
        while (low < high) {
            const auto difference = high - low;
            const auto middle = low + difference / 2 + difference % 2;
            if (daemon.admitsAssemblyCapacity(index, middle))
                low = middle;
            else
                high = middle - 1;
        }
        return low;
    }
};

} // namespace Ubiquitty::Host

using Ubiquitty::Host::Daemon;
using Ubiquitty::TestDoubles::StreamDouble;
using Access = Ubiquitty::Host::InboundHoldsAccess;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Local = Ubiquitty::Protocol::Local;
namespace PAL = Ubiquitty::PAL;

namespace {

struct Attached {
    std::size_t index = 0;
    std::unique_ptr<UbiquittyTest::ScreenClient> client;
};

Attached attach(Daemon& daemon, std::uint32_t session, std::uint64_t instance)
{
    Attached attached;
    attached.index = Screen::attach(daemon, session);
    Screen::grantScope(daemon, attached.index, { Sync::Scope::Observe, Sync::Scope::Drive });
    Screen::setTokenID(daemon, attached.index, "person-" + std::to_string(instance));
    Screen::markInteractiveUser(daemon, attached.index);
    attached.client = std::make_unique<UbiquittyTest::ScreenClient>(24, 80, Sync::ID128 { instance, instance });
    auto request = Screen::selectedRequest(daemon, attached.index);
    CHECK(request.has_value());
    if (!request)
        return attached;
    request->servesClipboard = true;
    CHECK(attached.client->start(*request));
    Screen::receive(daemon, attached.index, attached.client->takeStream(Screen::now()));
    Screen::endReadPass(daemon);
    Screen::deliver(daemon, attached.index, *attached.client);
    CHECK(Screen::connection(daemon, attached.index)->state() == Sync::ConnectionState::Attached);
    CHECK(attached.client->pane() != nullptr);
    return attached;
}

std::vector<std::uint8_t> callFrame(std::size_t padding)
{
    const auto text = R"({"jsonrpc":"2.0","id":"partial","method":"host.describe","params":{"padding":")" + std::string(padding, 'x') + "\"}}";
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Call { .text = text });
    std::vector<std::uint8_t> frame;
    CHECK(!body.empty());
    CHECK(Sync::decodeCall(body).has_value());
    CHECK(Sync::appendFrame(frame, Sync::FrameKind::Call, 0, body));
    return frame;
}

struct ClipboardRequest {
    std::size_t helper = 0;
    std::uint32_t requestID = 0;
    std::uint32_t handle = 0;
    PAL::UnixFileDescriptor peer;
};

/// Only process-tree facts are modeled. The actual socket UID, executable,
/// PID lifetime, local operation, source choice and request ID use production.
ClipboardRequest askClipboard(Daemon& daemon, const Attached& source, std::uint32_t session, StreamDouble& stream, std::uint64_t correlation)
{
    ClipboardRequest request;
    int descriptors[2] { -1, -1 };
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    if (descriptors[0] < 0 || descriptors[1] < 0)
        return request;
    request.peer = PAL::UnixFileDescriptor(descriptors[1]);
    (void)::fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    request.helper = Screen::connect(daemon, PAL::UnixFileDescriptor(descriptors[0]));
    CHECK(Access::authenticateLocalHelper(daemon, request.helper));
    auto previous = std::exchange(stream.facts.processes, {});
    PAL::Process::Record process;
    process.processID = static_cast<std::uint32_t>(::getpid());
    stream.facts.processes.push_back(std::move(process));
    const auto bytes = Local::encodeLocalClipboardRequest(correlation, { .kind = Local::LocalClipboardKind::Item, .mime = "image/png" });
    CHECK(bytes.has_value());
    if (bytes)
        Screen::receive(daemon, request.helper, bytes->view());
    stream.facts.processes = std::move(previous);
    request.requestID = Access::requestID(daemon, request.helper, session);
    request.handle = Access::paneHandle(daemon, source.index, session);
    CHECK(request.requestID != 0 && request.handle != 0);
    CHECK(!Screen::isDropped(daemon, request.helper));
    return request;
}

Sync::EffectCodecBytes clipboardBundle(std::size_t dataBytes)
{
    const std::vector<std::uint8_t> data(dataBytes, 0x5a);
    Sync::EffectClipboardBundle bundle;
    bundle.count = 1;
    bundle.items[0] = { .mime = "image/png", .data = data };
    auto bytes = Sync::encodeEffectClipboard(bundle, Sync::maxEffectClipboardReadBytes);
    CHECK(bytes.has_value());
    return bytes ? std::move(*bytes) : Sync::EffectCodecBytes {};
}

std::vector<std::uint8_t> clipboardHead(const ClipboardRequest& request, std::uint32_t transfer, std::span<const std::uint8_t> bundle)
{
    const auto body = Sync::encodeClipboardData({ .requestID = request.requestID, .status = Sync::EffectClipboardStatus::OK, .payload = { .transfer = transfer, .encodedBytes = static_cast<std::uint32_t>(bundle.size()), .inlineBytes = transfer ? std::span<const std::uint8_t> {} : bundle } });
    CHECK(body.has_value());
    std::vector<std::uint8_t> frame;
    if (body) {
        CHECK(Sync::decodeClipboardData(body->view()).has_value());
        CHECK(Sync::appendFrame(frame, Sync::FrameKind::ClipboardData, request.handle, body->view()));
    }
    return frame;
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    Daemon daemon;
    auto owned = std::make_unique<StreamDouble>();
    auto* stream = owned.get();
    auto& session = Screen::makeSession(daemon, 24, 80, std::move(owned));
    Screen::publish(daemon, session, Ubiquitty::Runtime::FrameClass::Structural);

    // A Notify is the host's to send: one from a client is malformed, so the
    // host holds none of it.
    {
        const auto person = attach(daemon, session.id, 1);
        std::vector<std::uint8_t> body;
        Sync::encode(body, Sync::Notify { .kind = Sync::NotifyKind::PasteReady, .record = {} });
        std::vector<std::uint8_t> notifies;
        CHECK(Sync::appendFrame(notifies, Sync::FrameKind::Notify, 0, body));
        Screen::receive(daemon, person.index, notifies);
        const auto* connection = Screen::connection(daemon, person.index);
        CHECK(!connection || connection->state() == Sync::ConnectionState::Closed);
        if (connection)
            Screen::drop(daemon, person.index);
    }

    // A complete canonical header with one missing body byte is silence-bound.
    const auto wholeCall = callFrame(Sync::maxFrameBodyBytes - 128);
    {
        const auto person = attach(daemon, session.id, 2);
        Screen::receive(daemon, person.index, std::span(wholeCall).first(wholeCall.size() - 1));
        const auto* connection = Screen::connection(daemon, person.index);
        CHECK(connection && connection->pendingInboundFrameBytes() > Sync::maxFrameBodyBytes - 128);
        CHECK(Screen::heldInbound(daemon, person.index) >= Sync::maxFrameBodyBytes - 128);
        CHECK(Screen::hasInboundDeadline(daemon, person.index));
        Screen::enforceInboundBudget(daemon);
        CHECK(!Screen::isDropped(daemon, person.index));
        Screen::expireInboundDeadline(daemon, person.index);
        CHECK(Screen::isDropped(daemon, person.index));
    }

    // Incomplete nonempty headers own deadlines without new body allocations.
    Sync::FrameHeader header;
    Sync::EnvelopeError error = Sync::EnvelopeError::None;
    std::size_t headerBytes = 1;
    while (headerBytes <= Sync::maxEnvelopeHeaderBytes
        && Sync::readFrameHeader(std::span(wholeCall).first(headerBytes), header, error) == Sync::FrameHeaderState::NeedsMore)
        ++headerBytes;
    CHECK(error == Sync::EnvelopeError::None && headerBytes <= Sync::maxEnvelopeHeaderBytes);
    for (std::size_t prefix = 1; prefix < headerBytes; ++prefix) {
        const auto person = attach(daemon, session.id, 40 + prefix);
        const auto* connection = Screen::connection(daemon, person.index);
        const auto before = connection->inboundFrameBytes();
        Screen::receive(daemon, person.index, std::span(wholeCall).first(prefix));
        CHECK(connection->pendingInboundFrameBytes() == prefix);
        CHECK(connection->inboundFrameBytes() == before);
        CHECK(Screen::hasInboundDeadline(daemon, person.index));
        Screen::expireInboundDeadline(daemon, person.index);
        CHECK(Screen::isDropped(daemon, person.index));
    }

    // Completion disarms silence; deferred teardown releases the frame owner.
    {
        const auto person = attach(daemon, session.id, 45);
        Screen::receive(daemon, person.index, std::span(wholeCall).first(wholeCall.size() - 1));
        CHECK(Screen::hasInboundDeadline(daemon, person.index));
        Screen::receive(daemon, person.index, std::span(wholeCall).last(1));
        CHECK(!Screen::hasInboundDeadline(daemon, person.index));
        CHECK(Screen::connection(daemon, person.index)->pendingInboundFrameBytes() == 0);
        const auto held = Screen::heldInbound(daemon, person.index);
        const auto aggregate = Access::aggregate(daemon);
        Screen::reap(daemon, person.index);
        CHECK(Access::aggregate(daemon) == aggregate - held);
    }

    // An unasked response is refused before allocating a logical bundle.
    {
        const auto person = attach(daemon, session.id, 50);
        const auto bundle = clipboardBundle(1024);
        const ClipboardRequest unasked { .helper = 0, .requestID = 999, .handle = Access::paneHandle(daemon, person.index, session.id), .peer = {} };
        Screen::receive(daemon, person.index, clipboardHead(unasked, 1, bundle.view()));
        const auto* connection = Screen::connection(daemon, person.index);
        CHECK(connection->state() == Sync::ConnectionState::Closed);
        CHECK(connection->byeReason() == Sync::ByeReason::Malformed);
        CHECK(connection->inboundClipboardBytes() == 0);
        Screen::drop(daemon, person.index);
    }

    // Actual typed assemblies are charged before allocation under the live cap.
    {
        std::vector<Attached> people;
        std::vector<ClipboardRequest> requests;
        const auto bundle = clipboardBundle(Sync::maxEffectClipboardReadBytes);
        bool wasRefused = false;
        std::size_t cap = 0;
        for (std::uint64_t instance = 10; instance < 14 && !wasRefused; ++instance) {
            auto next = std::make_unique<StreamDouble>();
            auto* process = next.get();
            auto& target = Screen::makeSession(daemon, 24, 80, std::move(next));
            Screen::publish(daemon, target, Ubiquitty::Runtime::FrameClass::Structural);
            people.push_back(attach(daemon, target.id, instance));
            const auto source = people.back().index;
            if (!cap)
                cap = Access::aggregate(daemon) + Access::remaining(daemon, source);
            for (std::size_t item = 0; item < Sync::maxPendingClipboardReads && !wasRefused; ++item) {
                requests.push_back(askClipboard(daemon, people.back(), target.id, *process, 100 + requests.size()));
                const auto before = Screen::heldInbound(daemon, people.front().index);
                Screen::receive(daemon, source, clipboardHead(requests.back(), static_cast<std::uint32_t>(requests.size()), bundle.view()));
                CHECK(Access::aggregate(daemon) <= cap);
                const auto* connection = Screen::connection(daemon, source);
                if (connection->state() == Sync::ConnectionState::Closed) {
                    wasRefused = true;
                    CHECK(connection->byeReason() == Sync::ByeReason::TransferRefused);
                    CHECK(connection->inboundClipboardBytes() == 0);
                    CHECK(source != people.front().index);
                    CHECK(Screen::heldInbound(daemon, people.front().index) == before);
                } else
                    CHECK(connection->inboundClipboardBytes() >= (item + 1) * bundle.size);
            }
        }
        CHECK(wasRefused);
        CHECK(!Screen::isDropped(daemon, people.front().index));
        Screen::service(daemon);
        Screen::enforceInboundBudget(daemon);
        CHECK(Screen::isDropped(daemon, people.back().index));
        CHECK(!Screen::isDropped(daemon, people.front().index));
        for (const auto& person : people)
            Screen::drop(daemon, person.index);
        for (const auto& request : requests)
            Screen::drop(daemon, request.helper);
    }

    // During typed delivery, drop releases queued answers but not borrowed data.
    {
        const auto person = attach(daemon, session.id, 30);
        auto neighborStream = std::make_unique<StreamDouble>();
        auto* neighborProcess = neighborStream.get();
        auto& neighborSession = Screen::makeSession(daemon, 24, 80, std::move(neighborStream));
        Screen::publish(daemon, neighborSession, Ubiquitty::Runtime::FrameClass::Structural);
        const auto neighbor = attach(daemon, neighborSession.id, 31);
        const auto bundle = clipboardBundle(1024);
        const auto neighborRequest = askClipboard(daemon, neighbor, neighborSession.id, *neighborProcess, 300);
        Screen::receive(daemon, neighbor.index, clipboardHead(neighborRequest, 1, bundle.view()));
        const auto neighborHeld = Screen::heldInbound(daemon, neighbor.index);
        const auto first = askClipboard(daemon, person, session.id, *stream, 301);
        const auto second = askClipboard(daemon, person, session.id, *stream, 302);
        auto& connection = Access::connection(daemon, person.index);
        Access::queue(daemon, person.index, clipboardHead(first, 0, bundle.view()));
        const auto oneHeld = Screen::heldInbound(daemon, person.index);
        Access::queue(daemon, person.index, clipboardHead(second, 0, bundle.view()));
        CHECK(Screen::heldInbound(daemon, person.index) > oneHeld);
        const auto retainedFrame = connection.inboundFrameBytes();
        const auto encodedRequest = Sync::encodeClipboardRead({ .requestID = first.requestID, .kind = Sync::EffectClipboardRequestKind::Item, .mime = "image/png" });
        CHECK(encodedRequest.has_value());
        const auto borrowedCharge = encodedRequest ? encodedRequest->size + bundle.size : 0;
        std::size_t delivered = 0;
        CHECK(!connection.drainClipboardReplies([&](std::uint32_t handle, const Sync::ClipboardRead& request,
                                                    Sync::EffectClipboardStatus status, const Sync::EffectClipboardBundle& current) {
            ++delivered;
            CHECK(handle == first.handle && request.requestID == first.requestID);
            CHECK(status == Sync::EffectClipboardStatus::OK);
            Screen::drop(daemon, person.index);
            CHECK(Screen::isDropped(daemon, person.index));
            CHECK(connection.inboundClipboardBytes() == borrowedCharge);
            CHECK(Screen::heldInbound(daemon, person.index) == retainedFrame + borrowedCharge);
            CHECK(current.count == 1 && current.items[0].mime == "image/png");
            CHECK(current.count == 1 && current.items[0].data.size() == 1024);
            CHECK(current.count == 1 && std::ranges::all_of(current.items[0].data, [](auto byte) { return byte == 0x5a; }));
            CHECK(Screen::heldInbound(daemon, neighbor.index) == neighborHeld);
            return false;
        }));
        CHECK(delivered == 1);
        CHECK(connection.inboundClipboardBytes() == 0);
        CHECK(Screen::heldInbound(daemon, person.index) == retainedFrame);
        CHECK(!Screen::isDropped(daemon, neighbor.index));
        Screen::drop(daemon, neighbor.index);
        Screen::drop(daemon, first.helper);
        Screen::drop(daemon, second.helper);
        Screen::drop(daemon, neighborRequest.helper);
    }

    if (failures == 0)
        std::printf("inbound_holds: canonical partial frames own silence deadlines; authenticated typed clipboard assemblies remain charged through delivery and respect aggregate ownership\n");
    return failures == 0 ? 0 : 1;
}
