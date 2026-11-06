// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// paste_push_landing.cpp — credited paste.push end to end inside the daemon: the
// pasted payload lands as a temp file whose extension tells the truth, the
// path reaches the session's PTY as a paste — bracketed exactly when the
// program asked for bracketed pastes — and every refusal answers with the
// ack naming why. Driven through a friend seam like the gate matrix, because
// a black-box driver cannot hold the temp directory still or read the PTY
// side of the injection.

#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "NotifyRead.h"
#include "Platform/Host/Daemon.h"
#include "TrustedLocalPeer.h"

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Local/Clipboard.h"
#include "Core/Protocol/Sync/IDText.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Socket.h"
#include "Platform/StreamDouble.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty {
namespace Host {

namespace Sync = Protocol::Sync;
namespace Control = Protocol::Control;

/// Fixture input owns the local bytes; it is not a wire DTO or codec.
struct PastePayload {
    std::uint32_t pushID = 0;
    std::string mime;
    std::vector<std::uint8_t> data;
};

struct PastePushAccess {
    static void addClient(Daemon& d, Sync::HelloIntents intents)
    {
        Daemon::Client c {};
        c.intents = intents;
        c.clientID = d.m_nextClientID++;
        // The trusted local bridge: a tokenless Local principal passes the
        // scope gate, which the gate matrix owns the proofs for — this test
        // is about what happens past it.
        // Local is an identity taken at accept rather than a name: the seed is
        // the running binary, and setting only the path would build a client
        // the daemon never accepts.
        Ubiquitty::Host::seedTrustedLocalPeer(c);
        d.m_clients.push_back(std::move(c));
    }

    static void addRemoteClient(Daemon& d, std::string tokenID)
    {
        Daemon::Client client {};
        client.clientID = d.m_nextClientID++;
        client.hasToken = true;
        client.hasBootstrapToken = true;
        client.isInteractiveUser = true;
        client.tokenID = std::move(tokenID);
        client.tokenScope = { Sync::Scope::Observe, Sync::Scope::Drive };
        d.m_clients.push_back(std::move(client));
    }

    static std::string grantKey(const Daemon& d, std::size_t index) { return d.grantKeyFor(d.m_clients[index]); }
    static std::string inputPrincipal(const Daemon& d, std::size_t index)
    {
        const auto* attachment = d.m_clients[index].syncConnection->attachment();
        if (!attachment)
            std::abort();
        return "human-account:" + std::to_string(::geteuid());
    }

    static std::optional<std::string> firstInputPrincipal(const Daemon& d, std::uint32_t sessionID)
    {
        for (const auto& session : d.m_sessions)
            if (session->id == sessionID)
                return session->firstInputPrincipal;
        return std::nullopt;
    }

    static bool hasMultipleInputPrincipals(const Daemon& d, std::uint32_t sessionID)
    {
        for (const auto& session : d.m_sessions)
            if (session->id == sessionID)
                return session->hasMultipleInputPrincipals;
        return false;
    }

    // A clipboard-serving client subscribed to a session, with a token scope
    // but no Local identity, so its clipboard is offered only if the source
    // selection admits a client that cannot drive. Returns its index.
    static std::size_t addServerWithScope(Daemon& d, std::uint32_t sessionID, Protocol::Sync::ScopeSet scope)
    {
        Daemon::Client c {};
        c.intents = Sync::HelloIntents { Sync::HelloIntent::ServesClipboard };
        c.clientID = static_cast<std::uint32_t>(d.m_clients.size() + 1);
        c.helloSeen = true;
        c.hasToken = true;
        c.tokenScope = scope;
        c.tokenID = "tok-server-" + std::to_string(c.clientID);
        c.agentLabel = "viewer"; // a verified-local agent label, so originOf is Agent, not Local
        c.peerProcess = "/usr/bin/viewer";
        c.paneSubscriptions.push_back({ .sessionID = sessionID });
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    static void addSession(Daemon& d, std::uint32_t id, TestDoubles::StreamDouble** streamOut)
    {
        auto session = std::make_unique<Daemon::Session>();
        session->id = id;
        auto stream = std::make_unique<TestDoubles::StreamDouble>();
        if (streamOut)
            *streamOut = stream.get();
        session->stream = std::move(stream);
        session->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*session->term, 24, 80);
        session->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*session->term, Protocol::Sync::ID128 { .low = id, .high = 1 });
        d.m_sessions.push_back(std::move(session));
    }

    // Session is the daemon's own type, so the bracketed-paste seed lives
    // here with the rest of the friend surface.
    static void enableBracketedPaste(Daemon& d, std::uint32_t sessionID)
    {
        for (auto& session : d.m_sessions) {
            if (session->id != sessionID)
                continue;
            if (!session->term)
                session->term = std::make_unique<Ubiquitty::Terminal>();
            Ubiquitty::hardReset(*session->term);
            session->term->modes.bracketedPaste = true;
        }
    }

    /// Keeps the actual client owner of one canonical admission, including its
    /// bound handles and numbered input. Setup selects, never seeds, subscriptions.
    static std::unique_ptr<UbiquittyTest::ScreenClient> attach(Daemon& d, std::size_t index, std::uint32_t sessionID, std::uint32_t workspaceID = 0)
    {
        auto* selected = d.findSession(sessionID);
        if (!selected)
            std::abort();
        const auto expectedWorkspace = d.hostedWorkspaceFor(*selected).workspaceID;
        d.m_clients[index].sessionID = sessionID;
        d.m_clients[index].workspaceID = workspaceID;
        auto peer = std::make_unique<UbiquittyTest::ScreenClient>(24, 80,
            Protocol::Sync::ID128 { .low = d.m_clients[index].clientID, .high = 9 });
        ScreenPlaneAccess::greet(d, index, *peer);
        const auto welcome = peer->welcome();
        if (!welcome || peer->connection().state() != Protocol::Sync::ConnectionState::Attached
            || welcome->admission != Protocol::Sync::AttachmentAdmission::Granted
            || welcome->selection.target != (workspaceID ? Protocol::Sync::AdmissionTarget::Workspace : Protocol::Sync::AdmissionTarget::Session)
            || welcome->selection.sessionID != sessionID || welcome->selection.workspaceID != expectedWorkspace
            || welcome->selection.daemonInstanceID != d.m_daemonInstanceID || welcome->selection.socketName != "screen-plane-fixture.sock"
            || !d.m_clients[index].subscriptionFor(sessionID))
            std::abort();
        return peer;
    }

    static std::uint32_t paneHandle(const Daemon& d, std::size_t index, std::uint32_t sessionID)
    {
        const auto* subscription = index < d.m_clients.size() ? d.m_clients[index].subscriptionFor(sessionID) : nullptr;
        return subscription ? subscription->screenHandle : 0;
    }

    /// A real held two-pane tree keeps the carrier alive when one target ends.
    static std::uint32_t twoPaneWorkspace(Daemon& d, std::uint32_t first, std::uint32_t second)
    {
        auto* root = d.findSession(first);
        if (!root || !d.findSession(second))
            std::abort();
        auto& workspace = d.hostedWorkspaceFor(*root);
        const auto pane = workspace.layout.splitFocusedPane(workspace.layout.surfaceIDAt(0), Panes::Orientation::Horizontal, 0.5f);
        if (pane == Session::Layout::invalidPane)
            std::abort();
        workspace.paneSessions.emplace_back(pane, second);
        d.m_isStructureDirty = true;
        return workspace.workspaceID;
    }

    static bool sessionExited(const Daemon& d, std::uint32_t sessionID)
    {
        for (const auto& session : d.m_sessions) {
            if (session->id == sessionID)
                return session->shellExited;
        }
        return false;
    }

    static void spendPushBudget(Daemon& d, std::size_t index) { d.m_clients[index].pastePushCount = 0xFFFFFF; }

    /// Tears the session down the way the daemon does, rather than by dropping
    /// the record: the unlink hangs off ~Session, and a test that erased the
    /// vector itself would pass with destroySession's call removed.
    static void destroy(Daemon& d, std::uint32_t sessionID) { d.destroySession(sessionID); }

    /// Asks for a clipboard through the daemon's own broker.
    ///
    /// Through the friend seam rather than by widening the API: the broker is
    /// the daemon's business and nothing outside it calls this, so making it
    /// public to test it would publish a method with no caller.
    static std::uint32_t requestClipboard(
        Daemon& d, std::uint32_t sessionID, Sync::ClipboardRequestKind kind, const std::string& mime)
    {
        return d.requestClipboardFromClient(sessionID, kind, mime);
    }

    /// Adds a session a client can type into: one with the terminal, the
    /// screen source and the input encoder a session the daemon creates
    /// carries.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - id: The session's id.
    ///   - streamOut: Receives the session's PTY double, when asked for.
    static void addTypedSession(Daemon& d, std::uint32_t id, TestDoubles::StreamDouble** streamOut)
    {
        auto session = std::make_unique<Daemon::Session>();
        session->id = id;
        session->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*session->term, 24, 80);
        auto stream = std::make_unique<TestDoubles::StreamDouble>();
        if (streamOut)
            *streamOut = stream.get();
        session->stream = std::move(stream);
        session->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*session->term, Protocol::Sync::ID128 { .low = id, .high = 1 });
        (void)d.wireRemoteInput(*session);
        session->isFrameDue = true;
        d.m_sessions.push_back(std::move(session));
    }

    /// Types as a client, which is what stamps the session's driver.
    ///
    /// Through the client's own screen connection and the daemon's input
    /// path rather than by setting the field: the stamp is the behaviour
    /// under test, and a test that writes it itself would pass with the
    /// stamp removed.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client, attached to a session `addTypedSession` made.
    ///   - client: The client's end of its screen connection, greeted here
    ///     the first time it types.
    ///   - text: What it types.
    static void typeAs(Daemon& d, std::size_t index, UbiquittyTest::ScreenClient& client, std::string_view text)
    {
        assert(client.hasStarted() && ScreenPlaneAccess::connection(d, index));
        const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        if (!client.sendRaw(bytes).has_value())
            std::fprintf(stderr, "paste_push_landing: the client's typing was not taken\n");
        ScreenPlaneAccess::deliver(d, index, client);
    }

    static std::size_t clipboardClientFor(const Daemon& d, std::uint32_t sessionID)
    {
        return d.clipboardClientForSession(sessionID);
    }

    /// Injects an expired-token predicate after real Local admission; this
    /// never supplies authority to the Greet or to a local helper.
    static void setClipboardProviderExpiry(Daemon& d, std::size_t index, std::uint64_t expiry)
    {
        d.m_clients[index].hasToken = true;
        d.m_clients[index].tokenExpiryUnix = expiry;
    }

    /// Pins selection state directly; the typing cases below prove its producer.
    static void setClipboardDriver(Daemon& d, std::uint32_t sessionID, std::size_t index)
    {
        if (auto* session = d.findSession(sessionID))
            session->lastInputClientID = d.m_clients[index].clientID;
    }

    static std::size_t clientCount(const Daemon& d) { return d.m_clients.size(); }

    /// Disconnects a client the way a dropped socket does.
    ///
    /// Both halves, because they are two steps in the daemon: dropClient marks
    /// the client dead and the run loop's reap pass is what erases it. Through
    /// the daemon's own removal rather than by erasing the vector — the
    /// question under test is what the broker does once a client is gone, and a
    /// test that took the client out by hand would answer it about a state the
    /// daemon never reaches.
    static void disconnect(Daemon& d, std::size_t index)
    {
        d.dropClient(index);
        d.reapClient(index);
    }

    static std::size_t pendingClipboardRequests(const Daemon& d, std::size_t index)
    {
        return d.m_clients[index].pendingClipboardRequests.size();
    }

    static void feedClipboardData(Daemon& d, std::size_t index, const Sync::ClipboardDataMessage& message, std::uint32_t selectedHandle = 0)
    {
        using namespace Protocol::Sync;
        EffectClipboardBundle bundle;
        bundle.count = message.items.size();
        for (std::size_t item = 0; item < bundle.count; ++item)
            bundle.items[item] = { message.items[item].mime, message.items[item].data };
        const auto payload = encodeEffectClipboard(bundle, maxEffectClipboardReadBytes);
        assert(payload);
        const auto body = encodeClipboardData({ .requestID = message.requestID,
            .status = static_cast<EffectClipboardStatus>(message.status),
            .payload = { .transfer = 0, .encodedBytes = message.status == Sync::ClipboardServeStatus::OK ? static_cast<std::uint32_t>(payload->size) : 0, .inlineBytes = message.status == Sync::ClipboardServeStatus::OK ? payload->view() : std::span<const std::uint8_t> {} } });
        assert(body);
        std::vector<std::uint8_t> wire;
        const auto handle = selectedHandle ? selectedHandle : (index < d.m_clients.size() ? paneHandle(d, index, d.m_clients[index].sessionID) : 0);
        if (!handle || !appendFrame(wire, FrameKind::ClipboardData, handle, body->view()))
            std::abort();
        ScreenPlaneAccess::receive(d, index, wire);
    }

    struct ClipboardOperation {
        PAL::UnixFileDescriptor peer;
        std::uint32_t requestID = 0;
        std::uint32_t correlation = 0;
        std::size_t helper = 0;
        std::vector<std::uint8_t> received;
    };

    /// Admits a fresh operation-4 helper. UID/PID/incarnation come from its
    /// actual socket; only membership in this fixture's process list is modeled.
    static ClipboardOperation prepareClipboardHelper(Daemon& d, std::size_t source, std::uint32_t sessionID, std::uint32_t correlation,
        Protocol::Local::LocalClipboardKind kind, std::string_view mime)
    {
        auto* session = d.findSession(sessionID);
        if (!session || !session->stream || source >= d.m_clients.size())
            std::abort();
        std::array<int, 2> descriptors { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0)
            std::abort();
        Daemon::Client helper;
        helper.clientID = d.m_nextClientID++;
        helper.fileDescriptor.reset(descriptors[0]);
        ClipboardOperation operation;
        operation.peer.reset(descriptors[1]);
        operation.correlation = correlation;
        operation.helper = d.m_clients.size();
        for (const auto descriptor : descriptors)
            if (::fcntl(descriptor, F_SETFL, O_NONBLOCK) != 0)
                std::abort();
        helper.hasOwnerLocalCredentials = PAL::peerIsSameUser(helper.fileDescriptor);
        d.capturePeerIdentity(helper);
        if (!helper.hasOwnerLocalCredentials || helper.peerProcessID != static_cast<std::uint32_t>(::getpid()))
            std::abort();
        // Every session this TU constructs owns a StreamDouble, not an OS PTY.
        static_cast<TestDoubles::StreamDouble&>(*session->stream).facts.processes.push_back({ .processID = helper.peerProcessID });
        d.m_clients.push_back(std::move(helper));
        const auto request = Protocol::Local::encodeLocalClipboardRequest(correlation, { .kind = kind, .mime = mime });
        if (!request)
            std::abort();
        ScreenPlaneAccess::receive(d, operation.helper, request->view());
        const auto& admitted = d.m_clients[operation.helper];
        if (admitted.didOverflow || admitted.localClipboardCorrelation != correlation || admitted.clipboardToolSessionID != sessionID
            || admitted.clipboardToolSelectionClientID != d.m_clients[source].clientID)
            std::abort();
        operation.requestID = admitted.clipboardToolRequestID;
        return operation;
    }

    /// Reads the already-queued local result while its borrowed payload lives.
    static bool hasLocalClipboardReply(const Daemon& d, ClipboardOperation& operation, Protocol::Sync::EffectClipboardStatus status, std::string_view mime)
    {
        using namespace Protocol::Local;
        std::array<std::uint8_t, 4096> buffer;
        for (;;) {
            const auto count = ::read(operation.peer.get(), buffer.data(), buffer.size());
            if (count > 0) {
                constexpr auto cap = localClipboardReplyHeaderBytes + Protocol::Sync::maxEffectClipboardEncodedReadBytes;
                if (operation.received.size() > cap || static_cast<std::size_t>(count) > cap - operation.received.size())
                    return false;
                operation.received.insert(operation.received.end(), buffer.begin(), buffer.begin() + count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                return false;
            break;
        }
        const auto& helper = d.m_clients[operation.helper];
        auto received = operation.received;
        const auto queued = std::span(helper.outputBuffer).subspan(helper.outputConsumed);
        received.insert(received.end(), queued.begin(), queued.end());
        LocalClipboardReplyReader reader(operation.correlation);
        if (reader.consume(received) != LocalClipboardRequestReader::State::Ready)
            return false;
        const auto reply = reader.take();
        return reply && reply->daemonInstanceID == d.m_daemonInstanceID && !reply->isHostSelected
            && reply->status == status && reply->bundle.count == 1 && reply->bundle.items[0].mime == mime
            && reply->bundle.items[0].data.empty();
    }

    /// Queues canonical inventory through the real budget and output writer.
    /// A positive deficit makes its existing budget too small by that many bytes.
    static bool queueClipboardInventory(Daemon& d, std::size_t helper, std::size_t deficit)
    {
        Protocol::Local::LocalClipboardReply reply { .correlation = d.m_clients[helper].localClipboardCorrelation,
            .daemonInstanceID = d.m_daemonInstanceID,
            .status = Protocol::Sync::EffectClipboardStatus::OK };
        reply.bundle.count = 1;
        reply.bundle.items[0] = { "image/png", {} };
        const auto measured = Protocol::Local::localClipboardReplyBytes(reply);
        if (!measured || deficit > *measured)
            return false;
        d.m_clients[helper].imageSourceBudget = std::make_shared<Protocol::Sync::BulkSourceBudget>(*measured - deficit);
        return d.queueLocalClipboardReply(helper, reply);
    }

    /// Inspects the exact predicate flushClient uses, without writing a descriptor.
    static bool isClipboardSelectionCurrent(const Daemon& d, std::size_t helper)
    {
        return d.isClipboardToolSelectionCurrent(d.m_clients[helper]);
    }

    /// Seeds a positively detected mismatch, not a missing platform witness.
    static void latchClipboardPeerMismatch(Daemon& d, std::size_t helper)
    {
        d.m_clients[helper].hasPeerProcessInstanceMismatch = true;
    }

    static std::size_t clipboardOutputCapacity(const Daemon& d, std::size_t helper)
    {
        return d.m_clients[helper].outputBuffer.capacity();
    }

    /// Whether an invalid response closed its authenticated reader.
    static bool isAttached(const Daemon& d, std::size_t index)
    {
        return d.m_clients[index].syncConnection && d.m_clients[index].syncConnection->state() == Protocol::Sync::ConnectionState::Attached;
    }

    /// Sends an undeclared canonical kind with an outstanding correlation ID.
    static void feedRetiredReply(Daemon& d, std::size_t index, std::uint32_t requestID)
    {
        const std::array<std::uint8_t, 7> payload { static_cast<std::uint8_t>(requestID), static_cast<std::uint8_t>(requestID >> 8),
            static_cast<std::uint8_t>(requestID >> 16), static_cast<std::uint8_t>(requestID >> 24), 1, 0, 0 };
        constexpr std::uint8_t unassignedKind = 0xFF;
        if (Protocol::Sync::frameKindOf(unassignedKind))
            std::abort();
        std::vector<std::uint8_t> wire { unassignedKind, 0, static_cast<std::uint8_t>(payload.size()) };
        wire.insert(wire.end(), payload.begin(), payload.end());
        ScreenPlaneAccess::receive(d, index, wire);
    }

    /// The most recent typed ClipboardRequest in a client's pending stream.
    static bool lastClipboardRequest(Daemon& d, std::size_t index, Sync::ClipboardRequestMessage& out)
    {
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
        const auto& client = d.m_clients[index];
        Protocol::Sync::FrameDecoder decoder;
        decoder.append(std::span(client.outputBuffer).subspan(client.outputConsumed));
        bool found = false;
        while (const auto frame = decoder.next()) {
            if (frame->kind != Protocol::Sync::FrameKind::ClipboardRequest)
                continue;
            if (const auto decoded = Protocol::Sync::decodeClipboardRead(frame->body)) {
                if (frame->channel != paneHandle(d, index, client.sessionID))
                    std::abort();
                out = { .requestID = decoded->requestID, .kind = static_cast<Sync::ClipboardRequestKind>(decoded->kind), .mime = std::string(decoded->mime) };
                found = true;
            }
        }
        assert(decoder.error() == Protocol::Sync::EnvelopeError::None && !decoder.bufferedBytes());
        return found;
    }

    static std::vector<std::string> controlMessages(const Daemon& d, std::size_t index, Protocol::Sync::FrameKind kind)
    {
        std::vector<std::string> messages;
        const auto& client = d.m_clients[index];
        Protocol::Sync::FrameDecoder decoder;
        decoder.append(std::span(client.outputBuffer).subspan(client.outputConsumed));
        while (const auto frame = decoder.next()) {
            assert(Protocol::Sync::directionOf(frame->kind) != Protocol::Sync::Direction::ClientToHost);
            if (frame->kind != kind)
                continue;
            if (frame->channel)
                std::abort();
            if (kind == Protocol::Sync::FrameKind::Notify) {
                messages.emplace_back(frame->body.begin(), frame->body.end());
            } else if (const auto result = Protocol::Sync::decodeResult(frame->body); result && !result->transfer) {
                messages.push_back(result->text);
            }
        }
        assert(decoder.error() == Protocol::Sync::EnvelopeError::None && !decoder.bufferedBytes());
        return messages;
    }

    static void feedPush(Daemon& d, std::size_t index, const PastePayload& message, std::uint32_t staleHandle = 0)
    {
        using Codec::JSON::Value;
        using namespace Protocol::Sync;
        if (!d.m_clients[index].syncConnection || d.m_clients[index].syncConnection->state() != ConnectionState::Attached)
            std::abort();
        const auto handle = staleHandle ? staleHandle : paneHandle(d, index, d.m_clients[index].sessionID);
        if (!handle)
            std::abort();
        const auto requestID = "paste.fixture:" + std::to_string(message.pushID);
        std::array<std::uint8_t, 32> digest;
        Crypto::sha256(message.data, digest);
        auto parameters = Value::object();
        parameters.set("pane", Value::number(handle));
        parameters.set("mime", Value::string(message.mime));
        parameters.set("bytes", Value::number(message.data.size()));
        parameters.set("sha256", Value::string(Control::PastePushDetail::digestText(digest)));
        auto call = Value::object();
        call.set("jsonrpc", Value::string("2.0"));
        call.set("id", Value::string(requestID));
        call.set("method", Value::string("paste.push"));
        call.set("params", std::move(parameters));
        const auto text = Codec::JSON::serialize(call);
        std::vector<std::uint8_t> wire;
        (void)appendFrame(wire, FrameKind::Call, 0, { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
        ScreenPlaneAccess::receive(d, index, wire);
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
        std::optional<Control::PasteReady> credit;
        for (const auto& notify : controlMessages(d, index, FrameKind::Notify)) {
            const auto read = UbiquittyTest::notifyOf({ reinterpret_cast<const std::uint8_t*>(notify.data()), notify.size() });
            if (const auto ready = read ? UbiquittyTest::pasteReadyOf(*read) : std::nullopt; ready && ready->requestID == requestID && ready->pane == handle)
                credit = ready;
        }
        if (!credit)
            return; // A normal correlated refusal granted no upload capacity.
        std::size_t offset = 0;
        std::uint32_t nextIndex = 0;
        while (offset < message.data.size()) {
            const auto length = std::min(bulkChunkPayloadBytes, message.data.size() - offset);
            const BulkChunk chunk { .transfer = credit->transfer, .index = nextIndex++, .isLast = offset + length == message.data.size(), .bytes = std::span(message.data).subspan(offset, length), .sha256 = digest };
            std::vector<std::uint8_t> body;
            encode(body, chunk);
            wire.clear();
            (void)appendFrame(wire, FrameKind::BulkChunk, 0, body);
            ScreenPlaneAccess::receive(d, index, wire);
            d.serviceSyncConnection(index, ScreenPlaneAccess::now());
            offset += length;
        }
    }

    // The most recent exact paste Result, never upload credit or transport silence.
    static bool lastAck(const Daemon& d, std::size_t index, std::uint32_t pushID, Control::PastePushReply& out)
    {
        bool found = false;
        for (const auto& text : controlMessages(d, index, Protocol::Sync::FrameKind::Result)) {
            bool isJSON = false;
            const auto answer = Codec::JSON::parse(text, isJSON);
            const auto* id = answer.find("id");
            const auto* result = answer.find("result");
            const auto expectedID = "paste.fixture:" + std::to_string(pushID);
            if (!isJSON || !id || !id->isString() || id->asString() != expectedID || !result)
                continue;
            const auto* pane = result->find("pane");
            if (!pane || !pane->isNumber())
                continue;
            if (const auto decoded = Control::decodePastePushReply(answer, expectedID, paneHandle(d, index, d.m_clients[index].sessionID))) {
                out = *decoded;
                out.pushID = pushID;
                found = true;
            }
        }
        return found;
    }

    static bool lastPasteRefusal(const Daemon& d, std::size_t index, std::uint32_t pushID, Control::ErrorCode expected)
    {
        for (const auto& text : controlMessages(d, index, Protocol::Sync::FrameKind::Result)) {
            bool isJSON = false;
            const auto answer = Codec::JSON::parse(text, isJSON);
            const auto* id = answer.find("id");
            const auto* error = answer.find("error");
            if (isJSON && answer.isObject() && answer.members().size() == 3 && answer.stringOr("jsonrpc") == "2.0"
                && id && id->isString() && id->asString() == "paste.fixture:" + std::to_string(pushID) && error && !answer.find("result")
                && error->numberOr("code") == static_cast<int>(expected))
                return true;
        }
        return false;
    }

    static bool hasPasteCredit(const Daemon& d, std::size_t index, std::uint32_t pushID)
    {
        for (const auto& text : controlMessages(d, index, Protocol::Sync::FrameKind::Notify)) {
            const auto read = UbiquittyTest::notifyOf({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
            if (const auto credit = read ? UbiquittyTest::pasteReadyOf(*read) : std::nullopt; credit && credit->requestID == "paste.fixture:" + std::to_string(pushID))
                return true;
        }
        return false;
    }

    /// Gives a client the transport every accepted client has. A pass that
    /// flushes, such as a session ending, writes to it, and a write to no
    /// descriptor is the fatal error that drops the client.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: The test's end, which receives what the daemon flushed.
    static PAL::UnixFileDescriptor giveTransport(Daemon& d, std::size_t index)
    {
        std::array<int, 2> descriptors { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0)
            std::abort();
        for (const auto descriptor : descriptors)
            if (::fcntl(descriptor, F_SETFL, O_NONBLOCK) != 0)
                std::abort();
        d.m_clients[index].fileDescriptor.reset(descriptors[0]);
        return PAL::UnixFileDescriptor(descriptors[1]);
    }

    /// Reads everything the daemon has flushed to a transport so far.
    ///
    /// - Parameter peer: The test's end of the transport.
    /// - Returns: The bytes, in the order the daemon wrote them.
    static std::vector<std::uint8_t> takeFlushed(const PAL::UnixFileDescriptor& peer)
    {
        std::vector<std::uint8_t> bytes;
        std::array<std::uint8_t, 4096> buffer;
        for (;;) {
            const auto count = ::read(peer.get(), buffer.data(), buffer.size());
            if (count > 0) {
                bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            return bytes;
        }
    }

    /// Whether the client was sent a PaneUnbind for exactly this handle.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - handle: The handle the unbind must name.
    ///   - flushed: What the daemon already wrote to the client's transport,
    ///     which precedes what is still queued.
    /// - Returns: Whether such an unbind was sent, in well-formed frames.
    static bool hasExactUnbind(Daemon& d, std::size_t index, std::uint32_t handle, std::span<const std::uint8_t> flushed)
    {
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
        const auto& client = d.m_clients[index];
        Protocol::Sync::FrameDecoder decoder;
        decoder.append(flushed);
        decoder.append(std::span(client.outputBuffer).subspan(client.outputConsumed));
        bool found = false;
        while (const auto frame = decoder.next())
            if (frame->kind == Protocol::Sync::FrameKind::PaneUnbind && !frame->channel)
                if (const auto unbind = Protocol::Sync::decodePaneUnbind(frame->body))
                    found |= unbind->handle == handle;
        return found && decoder.error() == Protocol::Sync::EnvelopeError::None && !decoder.bufferedBytes();
    }

    /// The neighbor's first input on its actual captured handle, not handle 1.
    static bool typeHandle(Daemon& d, std::size_t index, std::uint32_t handle, std::string_view text)
    {
        std::vector<std::uint8_t> body, wire;
        Protocol::Sync::encode(body, Protocol::Sync::Raw { .inputSeq = 1, .bytes = { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() } });
        if (!handle || !Protocol::Sync::appendFrame(wire, Protocol::Sync::FrameKind::Raw, handle, body))
            return false;
        ScreenPlaneAccess::receive(d, index, wire);
        return isAttached(d, index);
    }
};

namespace {

int failures = 0;
void check(bool ok, const char* what) // bool-ok: matches every Tests/Host/*.cpp check() harness
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

std::string readWholeFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

PastePayload pngPush(std::uint32_t id)
{
    PastePayload message;
    message.pushID = id;
    message.mime = "image/png";
    message.data = { 0x89, 'P', 'N', 'G', 0x00, 0x1A };
    return message;
}

// How many entries a directory holds, so a landing that had to be undone can
// be told from one that merely answered with an error.
std::size_t countEntries(const std::string& directory)
{
    DIR* handle = ::opendir(directory.c_str());
    if (handle == nullptr)
        return 0;
    std::size_t count = 0;
    while (const dirent* entry = ::readdir(handle)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
            ++count;
    }
    ::closedir(handle);
    return count;
}

// One scratch temp directory per run, so landed files are this test's own.
std::string makeScratchDirectory()
{
    char pathTemplate[] = "/tmp/ubq-paste-test-XXXXXX";
    const char* made = ::mkdtemp(pathTemplate);
    return made != nullptr ? made : "";
}

} // namespace
} // namespace Host
} // namespace Ubiquitty

int main()
{
    using namespace Ubiquitty;
    using namespace Ubiquitty::Host;
    using A = PastePushAccess;

    // The daemon ignores SIGPIPE (main.cpp does the same): a write to a client
    // whose end has closed is an error the daemon handles, not a signal.
    ::signal(SIGPIPE, SIG_IGN);

    const std::string scratch = makeScratchDirectory();
    if (scratch.empty())
        return 1;
    ::setenv("TMPDIR", scratch.c_str(), 1);

    // (a) landed, unbracketed: the file carries the bytes and the truthful
    // extension, and the bare path is what the PTY receives.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        TestDoubles::StreamDouble* stream = nullptr;
        A::addSession(daemon, 7, &stream);
        auto firstClient = A::attach(daemon, 0, 7);
        A::feedPush(daemon, 0, pngPush(1));

        Control::PastePushReply ack {};
        check(A::lastAck(daemon, 0, 1, ack), "a push answers with an ack");
        check(ack.pushID == 1, "the ack echoes the push id");
        check(ack.status == Control::PastePushStatus::Landed, "the push landed");
        check(ack.path.size() > 4 && ack.path.compare(ack.path.size() - 4, 4, ".png") == 0,
            "the landed file's extension names the payload's type");
        check(ack.path.compare(0, scratch.size(), scratch) == 0, "the file landed in the temp directory");
        const std::string contents = Host::readWholeFile(ack.path);
        check(contents.size() == 6 && std::memcmp(contents.data(), pngPush(1).data.data(), 6) == 0,
            "the landed bytes are the pasted bytes");
        check(stream->written == ack.path, "the bare path reached the PTY: the program did not ask for brackets");
    }

    // (b) landed, bracketed: the session's terminal has bracketed paste on,
    // which the host knows and the client cannot.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        TestDoubles::StreamDouble* stream = nullptr;
        A::addSession(daemon, 7, &stream);
        A::enableBracketedPaste(daemon, 7);
        auto firstClient = A::attach(daemon, 0, 7);
        A::feedPush(daemon, 0, pngPush(2));

        Control::PastePushReply ack {};
        check(A::lastAck(daemon, 0, 2, ack) && ack.status == Control::PastePushStatus::Landed, "the bracketed push landed");
        check(stream->written == "\033[200~" + ack.path + "\033[201~",
            "the path arrived inside bracketed-paste markers");
    }

    // (c) refusals answer, in their own words.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        TestDoubles::StreamDouble* neighbor = nullptr;
        A::addTypedSession(daemon, 7, nullptr);
        A::addTypedSession(daemon, 8, &neighbor);
        const auto workspace = A::twoPaneWorkspace(daemon, 7, 8);
        auto firstClient = A::attach(daemon, 0, 7, workspace);
        const auto endedHandle = A::paneHandle(daemon, 0, 7);
        const auto liveHandle = A::paneHandle(daemon, 0, 8);
        check(endedHandle && liveHandle && endedHandle != liveHandle, "premise: the actual workspace bound two distinct panes");
        const auto transport = A::giveTransport(daemon, 0);
        const auto before = countEntries(scratch);
        A::destroy(daemon, 7);
        const auto flushed = A::takeFlushed(transport);
        check(A::hasExactUnbind(daemon, 0, endedHandle, flushed), "destroying the target unbinds its exact old handle");
        check(A::isAttached(daemon, 0) && A::paneHandle(daemon, 0, 8) == liveHandle, "the independent neighbor keeps this carrier live");
        A::feedPush(daemon, 0, pngPush(3), endedHandle);
        check(A::lastPasteRefusal(daemon, 0, 3, Protocol::Control::ErrorCode::NotFound),
            "a gone pane refuses the exact call before granting upload credit");
        check(!A::hasPasteCredit(daemon, 0, 3) && countEntries(scratch) == before, "the stale handle grants no credit and lands no file");
        check(A::typeHandle(daemon, 0, liveHandle, "neighbor"),
            "the live neighbor's input is still accepted");
        firstClient->read(flushed, ScreenPlaneAccess::now());
        firstClient->read(A::takeFlushed(transport), ScreenPlaneAccess::now());
        ScreenPlaneAccess::deliver(daemon, 0, *firstClient);
        check(firstClient->connection().state() == Protocol::Sync::ConnectionState::Attached, "the real workspace reader remains admitted");
        check(neighbor && neighbor->written.find("neighbor") != std::string::npos, "the unaffected neighbor still receives its own input");
    }
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        A::addSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        PastePayload big = pngPush(4);
        big.data.assign(Control::maxPastePushBytes + 1, 0x55);
        A::feedPush(daemon, 0, big);
        check(A::lastPasteRefusal(daemon, 0, 4, Protocol::Control::ErrorCode::InvalidParameters),
            "an oversized manifest refuses before upload allocation or landing");
    }
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        A::addSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        A::spendPushBudget(daemon, 0);
        A::feedPush(daemon, 0, pngPush(5));
        Control::PastePushReply ack {};
        check(A::lastAck(daemon, 0, 5, ack) && ack.status == Control::PastePushStatus::TooLarge,
            "a client past its push budget is refused");
    }
    {
        // A temp directory nothing can write: the landing fails and says so.
        ::setenv("TMPDIR", "/nonexistent-ubq-paste-dir", 1);
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        TestDoubles::StreamDouble* stream = nullptr;
        A::addSession(daemon, 7, &stream);
        auto firstClient = A::attach(daemon, 0, 7);
        A::feedPush(daemon, 0, pngPush(6));
        Control::PastePushReply ack {};
        check(A::lastAck(daemon, 0, 6, ack) && ack.status == Control::PastePushStatus::IOError,
            "an unwritable temp directory answers IOError");
        check(stream->written.empty(), "and no path is typed for a file that does not exist");
        ::setenv("TMPDIR", scratch.c_str(), 1);
    }

    {
        // A session whose input queue is full: the file lands but its path is
        // not typed, and the ack must not say it was.
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        TestDoubles::StreamDouble* stream = nullptr;
        A::addSession(daemon, 7, &stream);
        auto firstClient = A::attach(daemon, 0, 7);
        stream->acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);
        A::feedPush(daemon, 0, pngPush(7));
        Control::PastePushReply ack {};
        check(A::lastAck(daemon, 0, 7, ack) && ack.status == Control::PastePushStatus::Busy,
            "a path the full input queue refused answers Busy, not Landed");
        check(stream->written.empty(), "nothing reached the PTY");
        check(!A::sessionExited(daemon, 7), "a full input queue does not end the session");
    }

    // (e) the extension table, every row of it: what the path claims is what
    // the bytes are, and an unmatched flavour says only that it is bytes.
    {
        struct Row {
            const char* mime;
            const char* extension;
        };
        static constexpr Row rows[] = {
            { "image/png", ".png" },
            { "image/jpeg", ".jpg" },
            { "image/gif", ".gif" },
            { "image/tiff", ".tiff" },
            { "application/pdf", ".pdf" },
            { "text/html", ".html" },
            { "text/plain", ".txt" },
            { "application/x-private", ".bin" },
            { "", ".bin" },
        };
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        A::addSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        std::uint32_t id = 8;
        for (const Row& row : rows) {
            PastePayload push = pngPush(id++);
            push.mime = row.mime;
            A::feedPush(daemon, 0, push);
            Control::PastePushReply ack {};
            const std::size_t length = std::strlen(row.extension);
            const bool isNamed = A::lastAck(daemon, 0, push.pushID, ack) && ack.path.size() > length
                && ack.path.compare(ack.path.size() - length, length, row.extension) == 0;
            std::string what = std::string("a ") + (row.mime[0] != '\0' ? row.mime : "typeless")
                + " paste lands as " + row.extension;
            check(isNamed, what.c_str());
        }
    }

    // (f) a write that cannot finish takes its file with it: a half-written
    // paste is worse than none, and the ack must not name a path.
    //
    // The file-size limit is the one way to make a regular-file write fail on
    // a healthy host. SIGXFSZ is ignored for the duration, or exceeding the
    // limit kills the run rather than returning the error being tested.
    {
        rlimit original {};
        const bool didAsk = ::getrlimit(RLIMIT_FSIZE, &original) == 0;
        rlimit capped = original;
        capped.rlim_cur = 512;
        const bool isLimited = didAsk && ::setrlimit(RLIMIT_FSIZE, &capped) == 0;
        void (*previousHandler)(int) = ::signal(SIGXFSZ, SIG_IGN);
        const std::size_t before = countEntries(scratch);

        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        A::addSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        PastePayload big = pngPush(20);
        big.data.assign(4096, 0x7F);
        A::feedPush(daemon, 0, big);

        Control::PastePushReply ack {};
        const bool didAnswer = A::lastAck(daemon, 0, 20, ack);
        const std::size_t after = countEntries(scratch);

        ::signal(SIGXFSZ, previousHandler);
        if (isLimited)
            ::setrlimit(RLIMIT_FSIZE, &original);

        check(isLimited, "the file-size limit could be set: the arm below is reachable");
        check(didAnswer && ack.status == Control::PastePushStatus::IOError,
            "a write that cannot finish answers IOError");
        check(ack.path.empty(), "a failed landing names no path");
        check(after == before, "the half-written file is gone");
    }

    // (g) a PTY that refuses the injection: the bytes landed, but the program
    // never received the path, so the ack must not claim it did.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        TestDoubles::StreamDouble* stream = nullptr;
        A::addSession(daemon, 7, &stream);
        auto firstClient = A::attach(daemon, 0, 7);
        stream->acceptBytesThenRefuse(0, PAL::StreamStatus::Closed);
        A::feedPush(daemon, 0, pngPush(30));

        Control::PastePushReply ack {};
        check(A::lastAck(daemon, 0, 30, ack) && ack.status != Control::PastePushStatus::Landed,
            "a refused injection does not answer Landed");
        check(stream->written.empty(), "nothing reached the program");
        check(A::sessionExited(daemon, 7), "the session is marked exited, as any failed PTY write marks it");
    }

    // (h) the clipboard a host does not have: the daemon asks a client that
    // offered to serve, and files the answer against the request that asked.
    {
        Daemon daemon;
        A::addClient(daemon,
            Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        A::addTypedSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);

        auto operation = A::prepareClipboardHelper(daemon, 0, 7, 71, Protocol::Local::LocalClipboardKind::Types, {});
        const std::uint32_t requestID = operation.requestID;
        check(requestID != 0, "a client that offered to serve is asked");
        check(A::pendingClipboardRequests(daemon, 0) == 1, "and the ask is outstanding until answered");

        Sync::ClipboardRequestMessage asked {};
        check(A::lastClipboardRequest(daemon, 0, asked), "the client received a CLIPBOARD_REQUEST");
        check(asked.requestID == requestID, "carrying the id the daemon will match on");
        check(asked.kind == Sync::ClipboardRequestKind::Types, "and the kind the caller asked for");

        Sync::ClipboardDataMessage answer;
        answer.requestID = requestID;
        answer.status = Sync::ClipboardServeStatus::OK;
        answer.items = { { "image/png", {} } };
        A::feedClipboardData(daemon, 0, answer);
        check(A::pendingClipboardRequests(daemon, 0) == 0, "the answer clears the request it answers");
        check(A::hasLocalClipboardReply(daemon, operation, Ubiquitty::Protocol::Sync::EffectClipboardStatus::OK, "image/png"),
            "the local helper receives the exact correlated F9 canonical inventory");
    }

    // (i) unknown correlations and an undeclared canonical kind cannot satisfy a
    // typed request. Both close the reader instead of selecting another parser.
    for (const bool isRetiredOuter : { false, true }) {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        A::addTypedSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);

        auto operation = A::prepareClipboardHelper(daemon, 0, 7, 72, Protocol::Local::LocalClipboardKind::Item, "image/png");
        const std::uint32_t requestID = operation.requestID;
        check(requestID != 0, "the client was asked");

        // A frame from a client index past the list is dropped at the dispatch,
        // before any handler sees it.
        Sync::ClipboardDataMessage fromNobody;
        fromNobody.requestID = requestID;
        fromNobody.status = Sync::ClipboardServeStatus::Empty;
        A::feedClipboardData(daemon, A::clientCount(daemon) + 5, fromNobody, A::paneHandle(daemon, 0, 7));
        check(A::pendingClipboardRequests(daemon, 0) == 1,
            "an answer from no client leaves the real request open");

        if (isRetiredOuter)
            A::feedRetiredReply(daemon, 0, requestID);
        else {
            Sync::ClipboardDataMessage stray;
            stray.requestID = requestID + 999;
            stray.status = Sync::ClipboardServeStatus::Empty;
            A::feedClipboardData(daemon, 0, stray);
        }
        check(!A::isAttached(daemon, 0), "an invalid answer closes the reader without filing clipboard data");
    }

    // (j) a client that never offered to serve is not asked. The bit is the
    // client saying it will answer; asking anyway would hang a program on a
    // reply that is not coming.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {}); // no clipboard offered
        A::addSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        check(A::requestClipboard(daemon, 7, Sync::ClipboardRequestKind::Types, "") == 0,
            "a client that offered no clipboard is not asked");
        check(A::pendingClipboardRequests(daemon, 0) == 0, "and nothing is left outstanding for it");

        // Nor is a client on a different session: the pasteboard that matters
        // belongs to the user whose session the program runs in.
        check(A::requestClipboard(daemon, 99, Sync::ClipboardRequestKind::Types, "") == 0,
            "no client on that session, no request");
    }

    // (k2) the driver left, and nobody inherits their clipboard. A neighbour
    // still attached did not run the command, so answering from their
    // pasteboard would hand one person's clipboard to another person's shell —
    // the exact substitution the whole broker exists to refuse.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        A::addTypedSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        auto secondClient = A::attach(daemon, 1, 7);

        A::typeAs(daemon, 1, *secondClient, "who is driving");
        check(A::clipboardClientFor(daemon, 7) == 1, "the one who typed owns the clipboard");

        A::disconnect(daemon, 1);
        check(A::clientCount(daemon) == 1, "and then they leave");
        check(A::clipboardClientFor(daemon, 7) == A::clientCount(daemon),
            "the remaining client is capable and still is not asked");
        check(A::requestClipboard(daemon, 7, Sync::ClipboardRequestKind::Types, "") == 0,
            "so no request goes out at all");
        check(A::pendingClipboardRequests(daemon, 0) == 0,
            "and the survivor carries nothing outstanding");
    }

    // (k) a program can ask faster than a person can approve a pasteboard read,
    // so the outstanding requests per client are bounded.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        A::addTypedSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);

        std::size_t granted = 0;
        for (int i = 0; i < 64; ++i) {
            if (A::requestClipboard(daemon, 7, Sync::ClipboardRequestKind::Types, "") != 0)
                ++granted;
        }
        check(granted > 0, "some requests are served");
        check(granted < 64, "and an unanswered client stops being asked");
        check(A::pendingClipboardRequests(daemon, 0) == granted, "every granted ask is outstanding");
    }

    // (l) two people share a session, and the clipboard follows whoever typed.
    //
    // Both hold a pasteboard; only one ran the command. Answering from the
    // other's would hand one person's clipboard to a command the other typed,
    // which is a disclosure rather than a convenience.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard }); // 0
        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard }); // 1
        TestDoubles::StreamDouble* stream = nullptr;
        A::addTypedSession(daemon, 7, &stream);
        auto firstClient = A::attach(daemon, 0, 7);
        auto secondClient = A::attach(daemon, 1, 7);

        // Before anyone types there is no "whose", and two capable clients are
        // a coin toss between two people's pasteboards.
        check(A::clipboardClientFor(daemon, 7) == A::clientCount(daemon),
            "two clients and nobody typing is ambiguous, so nobody answers");

        A::typeAs(daemon, 1, *secondClient, "ls\n");
        check(A::clipboardClientFor(daemon, 7) == 1, "the clipboard follows the client that typed");
        check(stream && stream->written.find("ls\n") != std::string::npos, "premise: what the client typed reached the program");

        A::typeAs(daemon, 0, *firstClient, "pwd\n");
        check(A::clipboardClientFor(daemon, 7) == 0, "and moves when the other one types");

        const std::uint32_t requestID
            = A::requestClipboard(daemon, 7, Sync::ClipboardRequestKind::Types, "");
        check(requestID != 0, "the driver is asked");
        check(A::pendingClipboardRequests(daemon, 0) == 1, "the request went to the driver");
        check(A::pendingClipboardRequests(daemon, 1) == 0, "and not to the other client");
    }

    // (m) the driver cannot serve, and a neighbour who can is left alone: the
    // neighbour did not run the command.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {}); // 0: driver, offers no clipboard
        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard }); // 1
        A::addTypedSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        auto secondClient = A::attach(daemon, 1, 7);

        A::typeAs(daemon, 0, *firstClient, "ls\n");
        check(A::clipboardClientFor(daemon, 7) == A::clientCount(daemon),
            "a driver that cannot serve answers nothing rather than deferring");
        check(A::requestClipboard(daemon, 7, Sync::ClipboardRequestKind::Types, "") == 0,
            "so no request is made");
        check(A::pendingClipboardRequests(daemon, 1) == 0,
            "and the capable neighbour is left alone");
    }

    // (n) one capable client and nobody typing is unambiguous: there is only
    // one pasteboard it could mean.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        A::addSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        check(A::clipboardClientFor(daemon, 7) == 0, "a lone capable client answers without being asked to type");
    }

    // Expiry is selection authority, even before the periodic token sweep runs.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        A::addSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        A::setClipboardProviderExpiry(daemon, 0, UINT64_MAX);
        check(A::clipboardClientFor(daemon, 7) == 0, "an unexpired token provider remains selectable");

        A::setClipboardProviderExpiry(daemon, 0, 1);
        check(A::clipboardClientFor(daemon, 7) == A::clientCount(daemon),
            "an expired provider is not selectable before a sweep");
        check(A::requestClipboard(daemon, 7, Sync::ClipboardRequestKind::Types, "") == 0,
            "an expired provider is never asked for clipboard data");
        check(A::pendingClipboardRequests(daemon, 0) == 0, "expiry creates no outstanding clipboard read");

        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        auto secondClient = A::attach(daemon, 1, 7);
        check(A::clipboardClientFor(daemon, 7) == 1, "before typing the sole unexpired provider remains unambiguous");
        A::setClipboardDriver(daemon, 7, 0);
        check(A::clipboardClientFor(daemon, 7) == A::clientCount(daemon),
            "an expired driver never delegates its clipboard to the live neighbor");
        check(A::requestClipboard(daemon, 7, Sync::ClipboardRequestKind::Types, "") == 0,
            "an expired driver's command cannot read the neighbor's clipboard");
        check(A::pendingClipboardRequests(daemon, 1) == 0, "the live neighbor remains unasked");
    }

    // A queued result's original selection is rechecked without reading a PID
    // again when accept already proved that its process incarnation changed.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        A::addSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        constexpr std::uint32_t correlation = 91;
        auto operation = A::prepareClipboardHelper(daemon, 0, 7, correlation, Protocol::Local::LocalClipboardKind::Types, {});
        const auto helper = operation.helper;
        check(A::isClipboardSelectionCurrent(daemon, helper), "the admitted helper's exact live selection is current");
        check(!A::queueClipboardInventory(daemon, helper, 1), "reply capacity one byte below the measured whole result refuses");
        check(A::clipboardOutputCapacity(daemon, helper) == 0, "a capacity refusal allocates no output owner");
        check(A::isClipboardSelectionCurrent(daemon, helper), "a budget refusal does not change the selected provider");
        check(A::queueClipboardInventory(daemon, helper, 0), "the exact measured reply budget admits the same inventory");
        check(A::hasLocalClipboardReply(daemon, operation, Protocol::Sync::EffectClipboardStatus::OK, "image/png"),
            "the positive control queued the complete correlated inventory");
        check(A::isClipboardSelectionCurrent(daemon, helper), "a queued result still belongs to the original live provider");
        A::latchClipboardPeerMismatch(daemon, helper);
        check(!A::isClipboardSelectionCurrent(daemon, helper), "a positively latched incarnation mismatch refuses delayed selection");
        check(A::hasLocalClipboardReply(daemon, operation, Protocol::Sync::EffectClipboardStatus::OK, "image/png"),
            "the negative exercised final selection with a real queued result, not an absent reply");
    }

    // (o) a paste drives the session as much as a keystroke, so the clipboard
    // follows it. Two paths reach the PTY, and a driver left behind by either
    // would answer a later read with the wrong person's pasteboard.
    {
        Daemon daemon;
        A::addClient(daemon,
            Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        A::addClient(daemon,
            Sync::HelloIntents { Sync::HelloIntent::ServesClipboard });
        A::addTypedSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        auto secondClient = A::attach(daemon, 1, 7);

        A::typeAs(daemon, 0, *firstClient, "ls\n");
        check(A::clipboardClientFor(daemon, 7) == 0, "typing set the driver");

        A::feedPush(daemon, 1, pngPush(77));
        check(A::clipboardClientFor(daemon, 7) == 1, "and a paste from the other client moved it");
    }

    // Successful paste uploads participate in the same sticky principal
    // history as typed input; attaching alone records no sender.
    {
        Daemon daemon;
        A::addRemoteClient(daemon, Crypto::sha256Hex("paste-principal-first"));
        A::addRemoteClient(daemon, Crypto::sha256Hex("paste-principal-second"));
        TestDoubles::StreamDouble* stream = nullptr;
        A::addTypedSession(daemon, 7, &stream);
        auto firstClient = A::attach(daemon, 0, 7);
        auto secondClient = A::attach(daemon, 1, 7);
        const auto firstPrincipal = A::grantKey(daemon, 0);
        const auto secondPrincipal = A::grantKey(daemon, 1);
        check(!firstPrincipal.empty() && !secondPrincipal.empty() && firstPrincipal != secondPrincipal,
            "the admitted paste clients hold different token grant identities");
        const auto firstInputPrincipal = A::inputPrincipal(daemon, 0);
        check(!A::firstInputPrincipal(daemon, 7) && !A::hasMultipleInputPrincipals(daemon, 7),
            "attachment does not record input provenance");

        A::feedPush(daemon, 0, pngPush(78));
        Control::PastePushReply first {};
        check(A::lastAck(daemon, 0, 78, first) && first.status == Control::PastePushStatus::Landed,
            "the first principal's upload lands");
        check(!first.path.empty() && stream->written == first.path, "the first paste path reaches the actual PTY");
        check(A::firstInputPrincipal(daemon, 7) == firstInputPrincipal && !A::hasMultipleInputPrincipals(daemon, 7),
            "the first successful paste records its principal without ambiguity");

        A::feedPush(daemon, 0, pngPush(79));
        Control::PastePushReply repeated {};
        check(A::lastAck(daemon, 0, 79, repeated) && repeated.status == Control::PastePushStatus::Landed,
            "the same principal's repeated upload lands");
        check(!repeated.path.empty() && stream->written == first.path + repeated.path,
            "both same-principal paste paths reach the PTY");
        check(A::firstInputPrincipal(daemon, 7) == firstInputPrincipal && !A::hasMultipleInputPrincipals(daemon, 7),
            "repeated pastes by the same principal remain unambiguous");

        A::feedPush(daemon, 1, pngPush(80));
        Control::PastePushReply different {};
        check(A::lastAck(daemon, 1, 80, different) && different.status == Control::PastePushStatus::Landed,
            "the other principal's upload lands");
        check(!different.path.empty() && stream->written == first.path + repeated.path + different.path,
            "the different principal's path reaches the same PTY");
        check(A::firstInputPrincipal(daemon, 7) == firstInputPrincipal && !A::hasMultipleInputPrincipals(daemon, 7),
            "a paste from the same authenticated account keeps input history unambiguous");

        A::feedPush(daemon, 0, pngPush(81));
        Control::PastePushReply returning {};
        check(A::lastAck(daemon, 0, 81, returning) && returning.status == Control::PastePushStatus::Landed,
            "the first principal can paste again");
        check(!returning.path.empty() && stream->written == first.path + repeated.path + different.path + returning.path,
            "the returning principal's paste also reaches the PTY");
        check(A::firstInputPrincipal(daemon, 7) == firstInputPrincipal && !A::hasMultipleInputPrincipals(daemon, 7),
            "later input from the same account keeps its input principal");
    }

    // (h) the landed file does not outlive the session. The path is typed to
    // the program, so the daemon keeps no other reference to it and nothing
    // else ever removes it: before this, every successful paste left its
    // contents on the host for the life of the daemon.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        A::addSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);
        A::feedPush(daemon, 0, pngPush(9));

        Control::PastePushReply ack {};
        check(A::lastAck(daemon, 0, 9, ack) && ack.status == Control::PastePushStatus::Landed, "the push landed");
        struct ::stat probe {};
        check(::stat(ack.path.c_str(), &probe) == 0, "the file exists while the session does");

        A::destroy(daemon, 7);
        check(::stat(ack.path.c_str(), &probe) != 0, "and is gone once the session is destroyed");
    }

    // (i) two pastes in one session both go, so the teardown walks the whole
    // list rather than remembering only the most recent path.
    {
        Daemon daemon;
        A::addClient(daemon, Sync::HelloIntents {});
        A::addSession(daemon, 7, nullptr);
        auto firstClient = A::attach(daemon, 0, 7);

        A::feedPush(daemon, 0, pngPush(10));
        Control::PastePushReply first {};
        check(A::lastAck(daemon, 0, 10, first) && first.status == Control::PastePushStatus::Landed, "the first push landed");
        A::feedPush(daemon, 0, pngPush(11));
        Control::PastePushReply second {};
        check(A::lastAck(daemon, 0, 11, second) && second.status == Control::PastePushStatus::Landed, "the second push landed");
        check(first.path != second.path, "the two pushes landed as different files");

        A::destroy(daemon, 7);
        struct ::stat probe {};
        check(::stat(first.path.c_str(), &probe) != 0, "the first file is gone");
        check(::stat(second.path.c_str(), &probe) != 0, "the second file is gone too");
    }

    // (m) a clipboard read is answered only by a client that can drive the
    // session. An observe-only viewer sharing a session has a pasteboard of
    // its own that the session's helper never ran a command for; selecting it
    // would read a bystander's clipboard. The command's clipboard is a
    // driver's, so a sole observe-only server is no server at all, and a
    // driver beside it is the one chosen.
    {
        Daemon daemon;
        A::addSession(daemon, 7, nullptr);
        const std::size_t viewer = A::addServerWithScope(daemon, 7, Sync::ScopeSet { Sync::Scope::Observe });
        check(A::clipboardClientFor(daemon, 7) == A::clientCount(daemon),
            "an observe-only viewer's clipboard is never the session's");
        check(A::requestClipboard(daemon, 7, Sync::ClipboardRequestKind::Types, "") == 0,
            "so no read goes out to it");
        (void)viewer;

        const std::size_t driver = A::addServerWithScope(daemon, 7, Sync::ScopeSet { Sync::Scope::Observe, Sync::Scope::Drive });
        check(A::clipboardClientFor(daemon, 7) == driver,
            "a client that can drive is the one asked, past the observer");
    }

    if (failures) {
        std::printf("FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("OK: paste_push_landing\n");
    return 0;
}
