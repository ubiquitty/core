// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/consent_answer_attach.cpp — what a human's answer buys, and for
// whom.
//
// When an agent asks to attach to a terminal a person is using, the daemon
// holds it and asks that person. The answer has four shapes — once, for this
// session, always, or no — and each one has to land on its own durability, and
// on exactly one principal. The gate is not the prompt; the gate is what the
// answer is recorded against.
//
// Questions have current pending attachments, immutable pane targets and live
// recipients. Answers enter as canonical Calls; only exact approval promotion
// may attach or create a standing grant. A tokenless agent is refused for its
// missing token, and a mutable executable path is never a persistent grant key.
//
// Exit 0 on success.
//

#include "NotifyRead.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/ManagementMetadata.h"
#include "TrustedLocalPeer.h"

#include "Assertions.h"

#include "Core/Codec/JSON.h"
#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Sync/ConsentStatus.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/Stream.h"
#include "Platform/Host/TokenStore.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct ConsentAnswerAccess {
    static std::uint32_t addSession(Daemon& d, std::uint32_t id)
    {
        CHECK(id >= d.m_nextSessionID);
        if (id < d.m_nextSessionID)
            return 0;
        d.m_nextSessionID = id;
        const auto& session = ScreenPlaneAccess::makeSession(d, 2, 8);
        CHECK(session.id == id);
        return session.id;
    }
    static void destroySession(Daemon& d, std::uint32_t id)
    {
        ScreenPlaneAccess::destroySession(d, id);
    }

    // Every client owns a pipe's write end. With fd -1 the daemon's write fails
    // EBADF and routes into dropClient, which sets `didOverflow` — the same flag
    // the deny path sets to reap an attaching agent. A test reading that flag
    // off a client with no descriptor is measuring its own harness.
    static int openPipe(Daemon::Client& c)
    {
        int fds[2] = { -1, -1 };
        if (::pipe(fds) != 0)
            return -1;
        ::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
        ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
        c.fileDescriptor = PAL::UnixFileDescriptor(fds[1]);
        return fds[0];
    }

    // A human on `sessionID`: verified-local, no agent label, presence-capable.
    static std::size_t addHuman(Daemon& d, std::uint32_t sessionID, int* readEnd = nullptr)
    {
        Daemon::Client c {};
        const int end = openPipe(c);
        if (readEnd)
            *readEnd = end;
        c.clientID = d.m_nextClientID++;
        Ubiquitty::Host::seedTrustedLocalPeer(c);
        c.sessionID = sessionID;
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Human, Protocol::Sync::Role::Owner);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        return index;
    }

    // A real greeting creates the hold, question and unapproved attachment.
    // Only the request-number allocator is seeded for deterministic assertions.
    static std::size_t addHeldAgent(Daemon& d, std::uint32_t sessionID, std::uint32_t requestID,
        const std::string& tokenID, int* readEnd = nullptr)
    {
        Daemon::Client c {};
        const int end = openPipe(c);
        if (readEnd)
            *readEnd = end;
        c.clientID = d.m_nextClientID++;
        c.peerProcess = "/usr/bin/some-agent";
        c.agentLabel = "agent";
        c.sessionID = sessionID;
        CHECK(!tokenID.empty());
        if (!tokenID.empty()) {
            c.hasToken = true;
            c.tokenID = tokenID;
            c.tokenScope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive, Protocol::Sync::Scope::Control };
        }
        CHECK(requestID >= d.m_nextConsentRequestID);
        d.m_nextConsentRequestID = requestID;
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Agent, Protocol::Sync::Role::Agent);
        CHECK(d.m_clients[index].syncConnection);
        if (!d.m_clients[index].syncConnection)
            return index;
        const auto& connection = *d.m_clients[index].syncConnection;
        CHECK(connection.state() == Protocol::Sync::ConnectionState::AwaitingConsent);
        CHECK(connection.attachment() && connection.attachment()->admission == Protocol::Sync::AttachmentAdmission::AwaitingConsent);
        CHECK(connection.boundPanes().empty());
        CHECK(d.m_clients[index].consentRequestID == requestID && d.m_clients[index].consentSessionID == sessionID);
        CHECK(pendingWelcome(d, index, end, { .requestID = requestID, .sessionID = sessionID }));
        return index;
    }

    static std::size_t addUnidentifiedAgent(Daemon& d)
    {
        Daemon::Client client;
        (void)openPipe(client);
        client.clientID = d.m_nextClientID++;
        client.agentLabel = "agent";
        d.m_clients.push_back(std::move(client));
        return d.m_clients.size() - 1;
    }

    static std::size_t addUnscopedAgent(Daemon& d, std::uint32_t sessionID, int* readEnd)
    {
        Daemon::Client client;
        const auto end = openPipe(client);
        if (readEnd)
            *readEnd = end;
        client.clientID = d.m_nextClientID++;
        client.agentLabel = "agent";
        client.peerProcess = "/usr/bin/some-agent";
        client.sessionID = sessionID;
        d.m_clients.push_back(std::move(client));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Agent, Protocol::Sync::Role::Agent);
        return index;
    }

    static std::size_t addAttachedAgent(Daemon& d, std::uint32_t sessionID, const std::string& tokenID)
    {
        const auto index = addUnidentifiedAgent(d);
        auto& client = d.m_clients[index];
        client.peerProcess = "/usr/bin/some-agent";
        client.hasToken = true;
        client.tokenID = tokenID;
        client.tokenScope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive };
        client.sessionID = sessionID;
        auto* session = d.findSession(sessionID);
        CHECK(session);
        if (!session)
            return index;
        ScreenPlaneAccess::grantAgentSession(d, *session, index);
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Agent, Protocol::Sync::Role::Agent);
        CHECK(admitted(d, index));
        // Prior owner consent admits only this insider. The unrelated pending
        // question below cannot borrow that setup grant or a human identity.
        session->agentSessionGrantIdentities.clear();
        return index;
    }

    // The production Call ingress and scheduler, never a direct adapter call.
    static void feedCall(Daemon& d, std::size_t index, const std::string& text)
    {
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Call { text });
        std::vector<std::uint8_t> stream;
        CHECK(Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Call, 0, body));
        ScreenPlaneAccess::receive(d, index, stream);
    }

    static void queueAnswer(Daemon& d, std::size_t humanIndex, std::uint32_t requestID, Protocol::Sync::ConsentChoice decision,
        Protocol::Sync::ConsentKind kind = Protocol::Sync::ConsentKind::Attach)
    {
        CHECK(d.m_clients[humanIndex].syncConnection);
        if (!d.m_clients[humanIndex].syncConnection)
            return;
        const auto& connection = *d.m_clients[humanIndex].syncConnection;
        const auto correlation = "fixture.answer." + std::to_string(connection.counters().callsAdmitted + 1);
        const auto text = Protocol::Control::consentResponseRequest({ requestID, kind, decision }, correlation);
        CHECK(!text.empty());
        if (text.empty())
            return;
        feedCall(d, humanIndex, text);
    }

    static void settlePending(Daemon& d)
    {
        for (unsigned round = 0; round < 2; ++round)
            for (std::size_t index = 0; index < d.m_clients.size(); ++index)
                d.servicePendingAttachConsent(index);
    }

    static void serviceQueuedAnswer(Daemon& d, std::size_t humanIndex)
    {
        d.serviceSyncConnection(humanIndex, ScreenPlaneAccess::now());
    }

    static bool hasValidatedApproval(Daemon& d, std::size_t requesterIndex, std::size_t approverIndex,
        Protocol::Sync::ConsentBinding question, Protocol::Sync::ConsentChoice decision)
    {
        const auto& requester = d.m_clients[requesterIndex];
        const auto& approver = d.m_clients[approverIndex];
        const auto& approval = requester.attachConsentApproval;
        const auto* target = d.findSession(question.sessionID);
        return approval && target && target->screenSource && requester.syncConnection && approver.syncConnection
            && requester.syncConnection->attachment() && approver.syncConnection->attachment()
            && approval->question == question && approval->decision == decision
            && approval->requesterAttachment == requester.syncConnection->attachment()->attachmentID
            && approval->requesterPrincipal == d.principalOf(requester) && approval->requesterScopes == requester.tokenScope
            && approval->approverClientID == approver.clientID
            && approval->approverAttachment == approver.syncConnection->attachment()->attachmentID
            && approval->approverPrincipal == d.principalOf(approver)
            && approval->targetPane == target->screenSource->paneID() && approval->targetPane == requester.consentPaneID;
    }

    static void answer(Daemon& d, std::size_t humanIndex, std::uint32_t requestID, Protocol::Sync::ConsentChoice decision,
        Protocol::Sync::ConsentKind kind = Protocol::Sync::ConsentKind::Attach)
    {
        queueAnswer(d, humanIndex, requestID, decision, kind);
        serviceQueuedAnswer(d, humanIndex);
        settlePending(d);
    }

    static void answerMalformed(Daemon& d, std::size_t humanIndex, std::string_view parameters)
    {
        feedCall(d, humanIndex, R"({"jsonrpc":"2.0","id":"fixture.invalid","method":"consent.respond","params":)" + std::string(parameters) + "}");
        serviceQueuedAnswer(d, humanIndex);
    }

    static void deny(Daemon& d, std::size_t index, Protocol::Sync::ConsentStatusPhase reason) { d.denyAgentAttach(index, reason); }
    static void kick(Daemon& d, std::size_t index) { d.kickClient(index); }
    static void refuse(Daemon& d, std::size_t index, Protocol::Sync::ByeReason reason) { d.refuseClient(index, reason); }
    // A link gone silent, as the host drops one it heard nothing from.
    static void dropSilent(Daemon& d, std::size_t index) { d.dropWithFarewell(index, Protocol::Sync::ByeReason::Silent, "silent"); }
    // A client still owed a retirement is not asked a new question until it
    // takes it, which makes one asking fail.
    static void owesRetirement(Daemon& d, std::size_t index) { d.m_clients[index].pendingConsentRetirements.push_back({ Protocol::Sync::ConsentKind::Attach, 1, 1 }); }
    static void settleRetirements(Daemon& d, std::size_t index) { d.m_clients[index].pendingConsentRetirements.clear(); }
    static void useTokenStore(Daemon& d, const std::string& directory) { d.m_tokenPath = directory + "/tokens"; }
    static void writeTokens(Daemon& d, std::string_view requester, std::string_view approver)
    {
        auto* file = openOwnerOnly(d.m_tokenPath, "w");
        REQUIRE(file);
        const Protocol::Sync::ScopeSet scope { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive, Protocol::Sync::Scope::Control };
        std::fprintf(file, "%.*s %u 0 requester\n", static_cast<int>(requester.size()), requester.data(), static_cast<unsigned>(scope.toRaw()));
        if (!approver.empty())
            std::fprintf(file, "%.*s %u 0 approver\n", static_cast<int>(approver.size()), approver.data(), static_cast<unsigned>(scope.toRaw()));
        CHECK(std::fclose(file) == 0);
    }
    static std::size_t addTokenHuman(Daemon& d, std::uint32_t sessionID, const std::string& tokenID)
    {
        Daemon::Client client {};
        (void)openPipe(client);
        client.clientID = d.m_nextClientID++;
        seedTrustedLocalPeer(client);
        client.sessionID = sessionID;
        client.hasToken = true;
        client.tokenID = tokenID;
        client.tokenScope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive, Protocol::Sync::Scope::Control };
        client.tokenExpiryUnix = static_cast<std::uint64_t>(::time(nullptr)) + 3600;
        client.isInteractiveUser = true;
        d.m_clients.push_back(std::move(client));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Human, Protocol::Sync::Role::Owner);
        CHECK(d.hasLiveStructureAttachment(d.m_clients[index]));
        d.noteAttachmentGrant(d.m_clients[index]);
        return index;
    }
    static void expireRetainedToken(Daemon& d, std::string_view tokenID)
    {
        bool didExpire = false;
        for (auto& grant : d.m_attachmentGrants) {
            if (grant.tokenID == tokenID) {
                grant.tokenExpiryUnix = 1;
                didExpire = true;
            }
        }
        CHECK(didExpire);
        d.dropExpiredTokenClients();
    }
    static bool hasApproval(const Daemon& d, std::size_t index) { return d.m_clients[index].attachConsentApproval.has_value(); }
    static void holdTerminal(Daemon& d, std::size_t index) { d.m_clients[index].attachConsentTerminal = Protocol::Sync::ConsentStatusPhase::Denied; }
    static bool hasRecipientLeft(const Daemon& d, std::size_t requester, std::uint32_t clientID)
    {
        const auto& recipients = d.m_clients[requester].consentRecipients;
        const auto found = std::ranges::find(recipients, clientID, &Daemon::ConsentRecipient::clientID);
        return found != recipients.end() && found->standing == Daemon::ConsentRecipient::Standing::Left;
    }
    static bool hasRecipientContinued(const Daemon& d, std::size_t requester, std::uint32_t clientID)
    {
        const auto& recipients = d.m_clients[requester].consentRecipients;
        const auto found = std::ranges::find(recipients, clientID, &Daemon::ConsentRecipient::clientID);
        return found != recipients.end() && found->standing == Daemon::ConsentRecipient::Standing::Continued;
    }
    static std::uint32_t clientID(const Daemon& d, std::size_t index) { return d.m_clients[index].clientID; }
    static void supersedeAndDropBehind(Daemon& d, std::size_t index)
    {
        ScreenPlaneAccess::supersedeScreen(d, index);
        d.dropForOutputBacklog(index);
    }
    static std::string createdBy(const std::string& directory, const std::string& identifier) { return ManagementMetadataStore(directory + "/tokens").metadata(identifier).createdBy; }
    static std::string managementIdentity(const Daemon& d, std::size_t index) { return d.managementIdentityOf(d.m_clients[index]); }
    static std::size_t clientCount(const Daemon& d) { return d.m_clients.size(); }
    // An agent that needs a person's consent to attach, greeting with nobody
    // asked yet.
    static std::size_t addAgentNeedingConsent(Daemon& d, std::uint32_t sessionID, const std::string& tokenID, int* readEnd)
    {
        Daemon::Client c {};
        const int end = openPipe(c);
        if (readEnd)
            *readEnd = end;
        c.clientID = d.m_nextClientID++;
        c.peerProcess = "/usr/bin/some-agent";
        c.agentLabel = "agent";
        c.sessionID = sessionID;
        c.hasToken = true;
        c.tokenID = tokenID;
        c.tokenScope = { Protocol::Sync::Scope::Observe, Protocol::Sync::Scope::Drive, Protocol::Sync::Scope::Control };
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Agent, Protocol::Sync::Role::Agent);
        return index;
    }
    // A client's own goodbye, as an iOS app sends one going to the background.
    static void sayBye(Daemon& d, std::size_t index, Protocol::Sync::ByeReason reason)
    {
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Bye { .reason = reason });
        std::vector<std::uint8_t> stream;
        CHECK(Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Bye, 0, body));
        ScreenPlaneAccess::receive(d, index, stream);
    }
    // A person on a client that says it shows no questions, as the editors do.
    static std::size_t addEditorHuman(Daemon& d, std::uint32_t sessionID)
    {
        Daemon::Client c {};
        (void)openPipe(c);
        c.clientID = d.m_nextClientID++;
        Ubiquitty::Host::seedTrustedLocalPeer(c);
        c.sessionID = sessionID;
        d.m_clients.push_back(std::move(c));
        const auto index = d.m_clients.size() - 1;
        ScreenPlaneAccess::greetExisting(d, index, Protocol::Sync::ClientKind::Human, Protocol::Sync::Role::Owner,
            Protocol::Sync::ClientCapabilities { Protocol::Sync::ClientCapability::ShowsNoQuestions });
        return index;
    }
    static void expire(Daemon& d, std::size_t index) { d.m_clients[index].consentDeadlineMilliseconds = 1; }
    static void forgetRecipient(Daemon& d, std::size_t index) { d.m_clients[index].consentRecipients.clear(); }
    static void moveHuman(Daemon& d, std::size_t index, std::uint32_t sessionID)
    {
        d.m_clients[index].paneSubscriptions = { { .sessionID = sessionID } };
        d.m_clients[index].sessionID = sessionID;
    }
    static bool persistPathPolicy(Daemon& d, std::size_t index) { return d.persistAlwaysAllowedAgent(index); }
    // What UBIQUITTY_REQUIRE_TOKEN=0 sets at start(); a Daemon built without start() requires a token.
    static void permitMissingToken(Daemon& d) { d.m_requireToken = false; }
    static void seedRequest(Daemon& d, std::uint32_t requestID) { d.m_nextConsentRequestID = requestID; }
    static void askHumans(Daemon& d, std::size_t agentIndex) { d.sendConsentRequestToHumans(d.m_clients[agentIndex]); }
    static void capturePeerIdentity(Daemon& d, std::size_t index) { d.capturePeerIdentity(d.m_clients[index]); }

    static void setPeerProcess(Daemon& d, std::size_t index, const std::string& path) { d.m_clients[index].peerProcess = path; }
    static void clearFileDescriptor(Daemon& d, std::size_t index) { d.m_clients[index].fileDescriptor = PAL::UnixFileDescriptor(); }
    // A real socket, so the OS can answer who is on the other end of it.
    static void useSocketPair(Daemon& d, std::size_t index, int* peerEnd)
    {
        int fds[2] = { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
            return;
        d.m_clients[index].fileDescriptor = PAL::UnixFileDescriptor(fds[0]);
        if (peerEnd)
            *peerEnd = fds[1];
    }
    static std::uint32_t peerProcessID(const Daemon& d, std::size_t index) { return d.m_clients[index].peerProcessID; }
    static const std::string& peerProcess(const Daemon& d, std::size_t index) { return d.m_clients[index].peerProcess; }
    /// The session whose pane the daemon subscribed the client to, or zero.
    /// `sessionID` alone is the target the client selected before admission,
    /// which a refused or still-held client keeps.
    static std::uint32_t attachedTo(const Daemon& d, std::size_t index)
    {
        const auto& subscriptions = d.m_clients[index].paneSubscriptions;
        return subscriptions.empty() ? 0 : subscriptions.front().sessionID;
    }
    static bool pending(const Daemon& d, std::size_t index) { return d.m_clients[index].consentPending; }
    static bool reaped(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }
    static std::string grantKey(const Daemon& d, std::size_t index) { return d.grantKeyFor(d.m_clients[index]); }
    static std::vector<std::string>& persistedGrants(Daemon& d) { return d.m_alwaysAllowedAgents; }
    static void useScratchStore(Daemon& d, const std::string& directory) { d.m_socketPath = directory + "/daemon.sock"; }

    static bool admitted(const Daemon& d, std::size_t index)
    {
        const auto* connection = d.m_clients[index].syncConnection.get();
        if (!connection || connection->state() != Protocol::Sync::ConnectionState::Attached
            || !connection->attachment() || connection->attachment()->admission != Protocol::Sync::AttachmentAdmission::Granted)
            return false;
        const auto session = std::ranges::find_if(d.m_sessions, [&](const auto& held) { return held && held->id == d.m_clients[index].sessionID; });
        if (session == d.m_sessions.end() || !(*session)->screenSource)
            return false;
        const auto panes = connection->boundPanes();
        return std::ranges::any_of(panes, [&](const auto& pane) { return pane.second == (*session)->screenSource->paneID(); });
    }

    static Protocol::Sync::ID128 attachmentID(const Daemon& d, std::size_t index)
    {
        const auto* connection = d.m_clients[index].syncConnection.get();
        return connection && connection->attachment() ? connection->attachment()->attachmentID : Protocol::Sync::ID128 {};
    }

    static bool sessionGrant(const Daemon& d, std::uint32_t id, std::string* identity = nullptr)
    {
        for (const auto& session : d.m_sessions)
            if (session && session->id == id) {
                if (identity)
                    *identity = session->agentSessionGrantIdentities.empty() ? std::string {} : session->agentSessionGrantIdentities.front();
                return !session->agentSessionGrantIdentities.empty();
            }
        return false;
    }

    // Everything the daemon wrote to this client: what reached the pipe, then
    // whatever a short write left queued behind outputConsumed.
    static std::vector<std::uint8_t> written(Daemon& d, std::size_t index, int readEnd)
    {
        d.serviceSyncConnection(index, ScreenPlaneAccess::now());
        std::vector<std::uint8_t> bytes;
        if (readEnd >= 0) {
            std::uint8_t chunk[8192];
            for (;;) {
                const ssize_t n = ::read(readEnd, chunk, sizeof chunk);
                if (n <= 0)
                    break;
                bytes.insert(bytes.end(), chunk, chunk + n);
            }
        }
        Daemon::Client& c = d.m_clients[index];
        if (c.outputConsumed < c.outputBuffer.size())
            bytes.insert(bytes.end(), c.outputBuffer.begin() + static_cast<std::ptrdiff_t>(c.outputConsumed), c.outputBuffer.end());
        c.clearPendingOutput();
        return bytes;
    }

    // Current typed notifications inside the ordinary screen carrier.
    static std::vector<Protocol::Sync::Notify> notifiesWritten(Daemon& d, std::size_t index, int readEnd)
    {
        const std::vector<std::uint8_t> out = written(d, index, readEnd);
        std::vector<Protocol::Sync::Notify> notifications;
        Protocol::Sync::FrameDecoder decoder;
        decoder.append(out);
        while (const auto frame = decoder.next()) {
            if (frame->kind != Protocol::Sync::FrameKind::Notify)
                continue;
            if (auto notification = UbiquittyTest::notifyOf(frame->body))
                notifications.push_back(std::move(*notification));
        }
        CHECK(decoder.error() == Protocol::Sync::EnvelopeError::None && !decoder.bufferedBytes());
        return notifications;
    }

    static int deniedReason(Daemon& d, std::size_t index, int readEnd)
    {
        for (const auto& notification : notifiesWritten(d, index, readEnd)) {
            const auto status = UbiquittyTest::consentStatusOf(notification);
            if (status && status->question == Protocol::Sync::ConsentBinding { d.m_clients[index].consentRequestID, d.m_clients[index].consentSessionID }
                && (status->phase == Protocol::Sync::ConsentStatusPhase::Denied || status->phase == Protocol::Sync::ConsentStatusPhase::Expired))
                return static_cast<int>(std::to_underlying(status->phase));
        }
        return -1;
    }

    static int askedAction(Daemon& d, std::size_t index, int readEnd)
    {
        for (const auto& notification : notifiesWritten(d, index, readEnd)) {
            if (const auto question = UbiquittyTest::consentQuestionOf(notification))
                return question->action;
        }
        return -1;
    }

    static bool hasError(Daemon& d, std::size_t index, Protocol::Control::ErrorCode expected, std::string_view callID = {})
    {
        const auto bytes = written(d, index, -1);
        const auto generatedID = "fixture.answer." + std::to_string(d.m_clients[index].syncConnection->counters().callsAdmitted);
        if (callID.empty())
            callID = generatedID;
        Protocol::Sync::FrameDecoder decoder;
        decoder.append(bytes);
        bool found = false;
        while (const auto frame = decoder.next()) {
            if (frame->kind != Protocol::Sync::FrameKind::Result)
                continue;
            const auto result = Protocol::Sync::decodeResult(frame->body);
            if (!result || result->transfer)
                continue;
            bool valid = false;
            const auto value = Codec::JSON::parse(result->text, valid);
            const auto* error = valid ? value.find("error") : nullptr;
            if (value.stringOr("id") == callID && error && error->isObject() && error->numberOr("code") == std::to_underlying(expected))
                found = true;
        }
        CHECK(decoder.error() == Protocol::Sync::EnvelopeError::None && !decoder.bufferedBytes());
        return found;
    }

    static bool pendingWelcome(Daemon& d, std::size_t index, int readEnd, Protocol::Sync::ConsentBinding expected)
    {
        const auto bytes = written(d, index, readEnd);
        Protocol::Sync::FrameDecoder decoder;
        decoder.append(bytes);
        bool sawWelcome = false;
        bool sawPending = false;
        while (const auto frame = decoder.next()) {
            if (frame->kind == Protocol::Sync::FrameKind::Welcome) {
                const auto welcome = Protocol::Sync::decodeWelcome(frame->body);
                if (sawWelcome || !welcome || welcome->admission != Protocol::Sync::AttachmentAdmission::AwaitingConsent
                    || welcome->consent != expected || !welcome->resumed.empty()
                    || welcome->selection.target != Protocol::Sync::AdmissionTarget::Session || welcome->selection.sessionID != expected.sessionID
                    || welcome->selection.daemonInstanceID != d.m_daemonInstanceID || welcome->attachmentID != attachmentID(d, index))
                    return false;
                sawWelcome = true;
            } else if (frame->kind == Protocol::Sync::FrameKind::Notify) {
                const auto notification = UbiquittyTest::notifyOf(frame->body);
                const auto status = notification ? UbiquittyTest::consentStatusOf(*notification) : std::nullopt;
                if (!sawWelcome || sawPending || !status || status->question != expected || status->phase != Protocol::Sync::ConsentStatusPhase::Pending)
                    return false;
                sawPending = true;
            } else if (frame->kind != Protocol::Sync::FrameKind::Probe && frame->kind != Protocol::Sync::FrameKind::Echo)
                return false;
        }
        return sawWelcome && sawPending && decoder.error() == Protocol::Sync::EnvelopeError::None && !decoder.bufferedBytes();
    }

    static std::optional<Protocol::Sync::ByeReason> refusal(Daemon& d, std::size_t index, int readEnd)
    {
        Protocol::Sync::FrameDecoder decoder;
        decoder.append(written(d, index, readEnd));
        std::optional<Protocol::Sync::ByeReason> reason;
        while (const auto frame = decoder.next()) {
            CHECK(frame->kind == Protocol::Sync::FrameKind::Bye);
            if (const auto bye = Protocol::Sync::decodeBye(frame->body))
                reason = bye->reason;
        }
        CHECK(decoder.error() == Protocol::Sync::EnvelopeError::None && !decoder.bufferedBytes());
        return reason;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::ConsentAnswerAccess;
using Ubiquitty::Host::Daemon;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

using A = ConsentAnswerAccess;
constexpr std::uint32_t theSession = 7;
constexpr std::uint32_t theRequest = 4242;

std::string scratch;

// With only a person who is shown no questions attached, a termination
// that needs consent is refused at once: nobody can be asked about it.
void checkTerminationNobodyCanBeAskedAbout()
{
    using Screen = Ubiquitty::Host::ScreenPlaneAccess;
    Daemon daemon;
    A::useScratchStore(daemon, scratch);
    const auto sessionID = Screen::makeSession(daemon, 2, 8).id;
    (void)A::addEditorHuman(daemon, sessionID);
    const auto caller = Screen::attachControlOnly(daemon);
    Screen::grantScope(daemon, caller, { Sync::Scope::Observe, Sync::Scope::Control });
    Screen::setTokenID(daemon, caller, "editor-only-termination");
    UbiquittyTest::ScreenClient client(2, 8);
    Screen::greet(daemon, caller, client);
    CHECK(client.call("{\"jsonrpc\":\"2.0\",\"id\":\"end\",\"method\":\"session.terminate\",\"params\":{\"session\":" + std::to_string(sessionID) + "}}"));
    Screen::deliver(daemon, caller, client);
    const auto results = client.takeResults();
    CHECK_MSG(results.size() == 1 && results.front().find("no attached person can approve this exact termination") != std::string::npos,
        "a termination nobody could be asked about was held");
    CHECK(!Screen::terminationQuestion(daemon, caller));
    CHECK(Screen::hasSession(daemon, sessionID));
}

// A termination question owns a real admitted Call, not only presentation
// fields. The exact result is queued before the deferred session destruction.
void checkTerminationAnswer(Sync::ConsentChoice decision)
{
    using Screen = Ubiquitty::Host::ScreenPlaneAccess;
    Daemon daemon;
    A::useScratchStore(daemon, scratch);
    const auto sessionID = Screen::makeSession(daemon, 2, 8).id;
    int humanRead = -1;
    const auto human = A::addHuman(daemon, sessionID, &humanRead);
    const auto caller = Screen::attachControlOnly(daemon);
    Screen::grantScope(daemon, caller, { Sync::Scope::Observe, Sync::Scope::Control });
    Screen::setTokenID(daemon, caller, "exact-termination-owner");
    UbiquittyTest::ScreenClient client(2, 8);
    Screen::greet(daemon, caller, client);
    CHECK(client.call("{\"jsonrpc\":\"2.0\",\"id\":\"end\",\"method\":\"session.terminate\",\"params\":{\"session\":" + std::to_string(sessionID) + "}}"));
    Screen::deliver(daemon, caller, client);
    const auto question = Screen::terminationQuestion(daemon, caller);
    CHECK(question);
    CHECK(client.takeResults().empty());
    CHECK(A::askedAction(daemon, human, humanRead) == 1);
    A::answer(daemon, human, question, decision, Sync::ConsentKind::Terminate);
    CHECK(!A::pending(daemon, caller));
    CHECK(!A::reaped(daemon, caller));
    CHECK(Screen::hasSession(daemon, sessionID));
    Screen::deliver(daemon, caller, client);
    const auto results = client.takeResults();
    CHECK(results.size() == 1);
    const bool allowed = decision != Sync::ConsentChoice::Deny;
    if (!results.empty()) {
        CHECK(results.front().find("\"id\":\"end\"") != std::string::npos);
        CHECK(results.front().find(allowed ? "\"outcome\":\"terminated\"" : "\"outcome\":\"denied\"") != std::string::npos);
    }
    CHECK(Screen::hasSession(daemon, sessionID) != allowed);
    CHECK(A::persistedGrants(daemon).empty());
    A::answer(daemon, human, question, decision, Sync::ConsentKind::Terminate);
    Screen::deliver(daemon, caller, client);
    CHECK(client.takeResults().empty());
}

} // namespace

int main()
{
    // start() is what installs this for a real daemon, and these cases build
    // daemons without it. Case 12 leaves a socket whose far end is closed, so
    // teardown writes into it — the process would die of SIGPIPE rather than
    // report anything.
    ::signal(SIGPIPE, SIG_IGN);

    char directoryTemplate[] = "/tmp/ubiquitty-consent-XXXXXX";
    const char* made = ::mkdtemp(directoryTemplate);
    if (!made) {
        std::fprintf(stderr, "consent_answer_attach: could not create a scratch directory\n");
        return 1;
    }
    scratch = made;

    // ---- 1. "Allow for this session" attaches the agent and records the grant
    // against that agent's identity. Recorded against the session alone, the
    // next agent to ask would inherit a person's answer about a different
    // program.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-a");
        const std::string expected = A::grantKey(d, agent);
        const auto heldAttachment = A::attachmentID(d, agent);

        A::answer(d, human, theRequest, Sync::ConsentChoice::AllowSession);

        CHECK(!A::pending(d, agent));
        CHECK(A::admitted(d, agent));
        CHECK(A::attachmentID(d, agent) == heldAttachment);
        CHECK(A::attachedTo(d, agent) == theSession);
        std::string identity;
        CHECK(A::sessionGrant(d, theSession, &identity));
        CHECK(!identity.empty());
        CHECK(identity == expected); // this agent, not "an agent"
        CHECK(A::persistedGrants(d).empty()); // and nothing was written to disk
    }

    // ---- 2. "Always" for a token-bound agent is persisted, because the token
    // digest names the principal rather than a path something else could be
    // moved into.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-b");
        const std::string expected = A::grantKey(d, agent);
        const auto heldAttachment = A::attachmentID(d, agent);

        A::answer(d, human, theRequest, Sync::ConsentChoice::AllowAlways);

        CHECK(A::attachedTo(d, agent) == theSession);
        CHECK(A::admitted(d, agent));
        CHECK(A::attachmentID(d, agent) == heldAttachment);
        bool persisted = false;
        for (const std::string& entry : A::persistedGrants(d))
            persisted = persisted || entry == expected;
        CHECK(persisted);
    }

    // A tokenless agent is refused for its missing token before consent is
    // asked. The direct persistence-policy guard is checked independently: a
    // mutable path must never reach disk, even before the real promotion path
    // rejects that principal's authority.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        int agentRead = -1;
        const std::size_t agent = A::addUnscopedAgent(d, theSession, &agentRead);
        CHECK(!A::grantKey(d, agent).empty());
        CHECK(!A::persistPathPolicy(d, agent));
        CHECK(A::persistedGrants(d).empty());

        CHECK(A::refusal(d, agent, agentRead) == Sync::ByeReason::TokenMissing);
        CHECK(!A::pending(d, agent));
        CHECK(A::attachmentID(d, agent) == Sync::ID128 {});
        A::answer(d, human, theRequest, Sync::ConsentChoice::AllowAlways);
        CHECK(A::hasError(d, human, Ubiquitty::Protocol::Control::ErrorCode::NotFound));
        CHECK(!A::admitted(d, agent));
        CHECK(A::attachedTo(d, agent) == 0);
        CHECK(!A::sessionGrant(d, theSession));
        CHECK(A::persistedGrants(d).empty());
    }

    // With the requirement off, the same tokenless agent is held and asked like
    // any other, and a person's Always still cannot make its mutable path a
    // standing grant: the answer admits it for this session alone and nothing
    // reaches disk.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::permitMissingToken(d);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        A::seedRequest(d, theRequest);
        int agentRead = -1;
        const std::size_t agent = A::addUnscopedAgent(d, theSession, &agentRead);
        CHECK(A::pending(d, agent));
        CHECK(A::pendingWelcome(d, agent, agentRead, { .requestID = theRequest, .sessionID = theSession }));
        CHECK(!A::persistPathPolicy(d, agent));

        A::answer(d, human, theRequest, Sync::ConsentChoice::AllowAlways);

        CHECK(!A::pending(d, agent));
        CHECK(A::admitted(d, agent));
        CHECK(A::attachedTo(d, agent) == theSession);
        std::string identity;
        CHECK(A::sessionGrant(d, theSession, &identity));
        CHECK(identity == A::grantKey(d, agent));
        CHECK(A::persistedGrants(d).empty());
    }

    // A valid observed token's Always answer falls back to Session only when
    // the durable write actually fails. The missing child is fixture-owned;
    // no permissions or pre-existing paths are changed to force the failure.
    {
        Daemon d;
        A::useScratchStore(d, scratch + "/missing-child");
        A::addSession(d, theSession);
        const auto human = A::addHuman(d, theSession);
        const auto agent = A::addHeldAgent(d, theSession, theRequest, "token-write-refused");
        const auto expected = A::grantKey(d, agent);
        const auto heldAttachment = A::attachmentID(d, agent);

        A::answer(d, human, theRequest, Sync::ConsentChoice::AllowAlways);

        CHECK(A::admitted(d, agent));
        CHECK(A::attachmentID(d, agent) == heldAttachment);
        CHECK(A::attachedTo(d, agent) == theSession);
        CHECK(A::persistedGrants(d).empty());
        std::string identity;
        CHECK(A::sessionGrant(d, theSession, &identity));
        CHECK(identity == expected);
    }

    // ---- 4. "Once" attaches and buys nothing beyond this attach: no session
    // grant, nothing on disk. It is the floor the two answers above are
    // measured against.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-c");
        const auto heldAttachment = A::attachmentID(d, agent);

        A::answer(d, human, theRequest, Sync::ConsentChoice::AllowOnce);

        CHECK(A::attachedTo(d, agent) == theSession);
        CHECK(A::admitted(d, agent));
        CHECK(A::attachmentID(d, agent) == heldAttachment);
        CHECK(!A::sessionGrant(d, theSession));
        CHECK(A::persistedGrants(d).empty());

        A::answer(d, human, theRequest, Sync::ConsentChoice::AllowAlways);
        CHECK(A::hasError(d, human, Ubiquitty::Protocol::Control::ErrorCode::NotFound));
        CHECK(A::admitted(d, agent));
        CHECK(!A::sessionGrant(d, theSession));
        CHECK(A::persistedGrants(d).empty());
    }

    // Denial resolves the exact hold. A malformed decision refuses only the
    // Call, leaving the immutable question available for a real answer.
    {
        for (int variant = 0; variant < 2; ++variant) {
            Daemon d;
            A::useScratchStore(d, scratch);
            A::addSession(d, theSession);
            const std::size_t human = A::addHuman(d, theSession);
            int agentRead = -1;
            const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-d", &agentRead);

            if (variant == 0)
                A::answer(d, human, theRequest, Sync::ConsentChoice::Deny);
            else
                A::answerMalformed(d, human, R"({"request":4242,"kind":"attach","decision":"not-a-decision"})");

            if (variant == 0) {
                CHECK(A::deniedReason(d, agent, agentRead) == std::to_underlying(Sync::ConsentStatusPhase::Denied));
                CHECK(!A::pending(d, agent));
            } else {
                CHECK(A::hasError(d, human, Ubiquitty::Protocol::Control::ErrorCode::InvalidParameters, "fixture.invalid"));
                CHECK(A::deniedReason(d, agent, agentRead) == -1);
                CHECK(A::pending(d, agent));
            }
            CHECK(A::attachedTo(d, agent) == 0); // never bound to the session
            CHECK(!A::admitted(d, agent));
            CHECK(!A::sessionGrant(d, theSession));
            CHECK(A::persistedGrants(d).empty());
        }
    }

    // Only a live human on that session may decide. A pending agent's Call is
    // a protocol violation; an admitted human on another session gets Forbidden.
    {
        for (const unsigned variant : { 0u, 1u }) {
            Daemon d;
            A::useScratchStore(d, scratch);
            A::addSession(d, theSession);
            A::addSession(d, theSession + 1);
            A::addHuman(d, theSession);
            const auto elsewhere = A::addHuman(d, theSession + 1);
            const auto agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-e");

            A::answer(d, variant ? elsewhere : agent, theRequest, Sync::ConsentChoice::AllowAlways);

            if (variant) {
                CHECK(A::hasError(d, elsewhere, Ubiquitty::Protocol::Control::ErrorCode::Forbidden));
                CHECK(A::pending(d, agent));
            } else {
                CHECK(A::reaped(d, agent));
                CHECK(!A::pending(d, agent));
            }
            CHECK(!A::admitted(d, agent));
            CHECK(A::attachedTo(d, agent) == 0);
            CHECK(!A::sessionGrant(d, theSession));
            CHECK(A::persistedGrants(d).empty());
        }
    }

    // ---- 6b. And the case the session check cannot catch: an agent already
    // attached to that session, answering for the one being held. Its session
    // matches, so the only thing standing between it and approving another
    // program's access to a person's terminal is that it is not a person.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        A::addHuman(d, theSession);
        const std::size_t insider = A::addAttachedAgent(d, theSession, "token-digest-insider");
        CHECK(!A::sessionGrant(d, theSession));
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-k");

        A::answer(d, insider, theRequest, Sync::ConsentChoice::AllowAlways);

        CHECK(A::pending(d, agent));
        CHECK(A::attachedTo(d, agent) == 0);
        CHECK(!A::sessionGrant(d, theSession));
        CHECK(A::persistedGrants(d).empty());
    }

    // The immutable target can disappear while a person decides. Approval is
    // denied rather than binding the pending identity to a nonexistent pane.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        int agentRead = -1;
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-f", &agentRead);
        A::queueAnswer(d, human, theRequest, Sync::ConsentChoice::AllowSession);
        CHECK(A::pending(d, agent));
        CHECK(!A::admitted(d, agent));
        CHECK(!A::hasValidatedApproval(d, agent, human, { theRequest, theSession }, Sync::ConsentChoice::AllowSession));
        A::serviceQueuedAnswer(d, human);
        CHECK(A::pending(d, agent));
        CHECK(!A::admitted(d, agent));
        CHECK(A::hasValidatedApproval(d, agent, human, { theRequest, theSession }, Sync::ConsentChoice::AllowSession));
        A::destroySession(d, theSession);
        A::settlePending(d);

        CHECK(A::deniedReason(d, agent, agentRead) == std::to_underlying(Sync::ConsentStatusPhase::Denied));
        CHECK(A::attachedTo(d, agent) == 0);
    }

    // A refused attach ends the connection. Typed termination refusal below
    // leaves its already admitted control attachment alive.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        A::addHuman(d, theSession);
        const std::size_t attaching = A::addHeldAgent(d, theSession, theRequest, "token-digest-g");

        A::deny(d, attaching, Sync::ConsentStatusPhase::Denied);
        (void)A::written(d, attaching, -1);

        CHECK(!A::pending(d, attaching));
        CHECK(A::reaped(d, attaching));
    }

    // ---- 9. The prompt goes to the humans on that session, and it says which
    // question is being asked. Approving a kill as though it were an attach
    // would defeat the gate entirely, so the action rides in the request.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        A::addSession(d, theSession + 1);
        int humanRead = -1, elsewhereRead = -1, attachingRead = -1;
        const std::size_t human = A::addHuman(d, theSession, &humanRead);
        const std::size_t elsewhere = A::addHuman(d, theSession + 1, &elsewhereRead);
        const std::size_t attaching = A::addHeldAgent(d, theSession, theRequest, "token-digest-i", &attachingRead);

        A::askHumans(d, attaching);
        CHECK(A::askedAction(d, human, humanRead) == 0); // attach
        CHECK(A::askedAction(d, elsewhere, elsewhereRead) == -1); // another session's human is not asked
        CHECK(A::askedAction(d, attaching, attachingRead) == -1); // nor the agent itself

        // Termination's distinct kind is checked by checkTerminationAnswer,
        // whose pending question owns an actual destructive Call.
    }

    // ---- 9b. An answer naming a request nobody holds changes nothing. A prompt
    // answered after its agent disconnected, or a replayed response, must not
    // land on whichever client happens to sit at that index now.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-l");

        A::answer(d, human, theRequest + 999, Sync::ConsentChoice::AllowAlways);

        CHECK(A::hasError(d, human, Ubiquitty::Protocol::Control::ErrorCode::NotFound));
        CHECK(A::pending(d, agent));
        CHECK(A::attachedTo(d, agent) == 0);
        CHECK(A::persistedGrants(d).empty());
    }

    // Current question identity, delivered recipient, live session and deadline
    // are independent proofs. A matching request number substitutes for none.
    for (const unsigned variant : { 0u, 1u, 2u, 3u, 4u }) {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        A::addSession(d, theSession + 1);
        const auto human = A::addHuman(d, theSession);
        int agentRead = -1;
        const auto agent = A::addHeldAgent(d, theSession, theRequest, "exact-gated-agent", &agentRead);
        if (variant == 1)
            A::forgetRecipient(d, agent);
        if (variant == 2)
            A::moveHuman(d, human, theSession + 1);
        if (variant == 3)
            Ubiquitty::Host::ScreenPlaneAccess::supersedeScreen(d, human);
        if (variant == 4)
            A::expire(d, agent);

        A::answer(d, human, theRequest, Sync::ConsentChoice::AllowAlways,
            variant ? Sync::ConsentKind::Attach : Sync::ConsentKind::Terminate);

        if (variant != 3) {
            const auto code = variant == 0 ? Ubiquitty::Protocol::Control::ErrorCode::InvalidParameters
                : variant == 4             ? Ubiquitty::Protocol::Control::ErrorCode::NotFound
                                           : Ubiquitty::Protocol::Control::ErrorCode::Forbidden;
            CHECK(A::hasError(d, human, code));
        }
        CHECK(!A::admitted(d, agent));
        CHECK(A::attachedTo(d, agent) == 0);
        CHECK(!A::sessionGrant(d, theSession));
        CHECK(A::persistedGrants(d).empty());
        if (variant == 4) {
            CHECK(A::deniedReason(d, agent, agentRead) == std::to_underlying(Sync::ConsentStatusPhase::Expired));
            CHECK(!A::pending(d, agent));
        } else if (variant == 3) {
            // The superseded screen leaves no connection carrying its
            // attachment that could be asked again, so with nobody left the
            // question is refused rather than decided by the stale answer.
            CHECK_MSG(!A::pending(d, agent), "a question waited on a continuation no connection could be asked through");
            CHECK(A::deniedReason(d, agent, agentRead) == std::to_underlying(Sync::ConsentStatusPhase::Denied));
        } else if (variant == 2) {
            // The answer is forbidden and the only recipient has left this
            // session, so servicing refuses the question with nobody to ask.
            CHECK_MSG(!A::pending(d, agent), "a question waited after its only recipient left the session");
            CHECK(A::deniedReason(d, agent, agentRead) == std::to_underlying(Sync::ConsentStatusPhase::Denied));
        } else
            CHECK(A::pending(d, agent));
    }

    // Invalid bounds and mixed selectors refuse the Call without consuming or
    // rewriting the host's retained question.
    for (const std::string_view parameters : {
             R"({"request":0,"kind":"attach","decision":"always"})",
             R"({"request":4294967296,"kind":"attach","decision":"always"})",
             R"({"request":4242.5,"kind":"attach","decision":"always"})",
             R"({"request":"4242","kind":"attach","decision":"always"})",
             R"({"request":4242,"kind":"attach","kind":"terminate","decision":"always"})",
             R"({"request":4242,"kind":"attach","decision":"always","session":"session:8"})",
         }) {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const auto human = A::addHuman(d, theSession);
        const auto agent = A::addHeldAgent(d, theSession, theRequest, "bounded-answer-agent");

        A::answerMalformed(d, human, parameters);

        CHECK(A::hasError(d, human, Ubiquitty::Protocol::Control::ErrorCode::InvalidParameters, "fixture.invalid"));
        CHECK(A::pending(d, agent));
        CHECK(!A::admitted(d, agent));
        CHECK(A::attachedTo(d, agent) == 0);
        CHECK(!A::sessionGrant(d, theSession));
        CHECK(A::persistedGrants(d).empty());
    }

    checkTerminationAnswer(Sync::ConsentChoice::AllowOnce);
    checkTerminationAnswer(Sync::ConsentChoice::AllowAlways);
    checkTerminationAnswer(Sync::ConsentChoice::Deny);
    checkTerminationNobodyCanBeAskedAbout();

    // ---- 10. A peer path carrying a newline is dropped rather than kept. The
    // always-allow store is one entry per line, so a path with a newline in it
    // — a legal filename byte — writes a second line that reads as a separate
    // allowed principal on the next load.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        const std::size_t agent = A::addUnidentifiedAgent(d);
        A::setPeerProcess(d, agent, "/usr/bin/agent\n/usr/bin/anything-else");

        // The client has a real descriptor, so the function runs past its
        // no-local-descriptor bail; the peer lookup fails on a pipe, leaving the
        // path this test planted to reach the strip.
        A::capturePeerIdentity(d, agent);

        CHECK(A::peerProcess(d, agent).empty()); // no key at all beats a forgeable one
        CHECK(A::grantKey(d, agent).empty());
    }

    // ---- 11. And an ordinary path survives it, so the case above is about the
    // newline and not about the function clearing everything it sees.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        const std::size_t agent = A::addUnidentifiedAgent(d);
        A::setPeerProcess(d, agent, "/usr/bin/ordinary-agent");

        A::capturePeerIdentity(d, agent);

        CHECK(A::peerProcess(d, agent) == "/usr/bin/ordinary-agent");
    }

    // ---- 12. Over a real socket the identity comes from the operating system,
    // not from anything the peer said about itself. The path a client claims is
    // overwritten by the one the kernel attributes to the process on the other
    // end — which is the whole reason a persisted grant can be keyed on it.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        const std::size_t agent = A::addUnidentifiedAgent(d);
        int peerEnd = -1;
        A::useSocketPair(d, agent, &peerEnd);
        A::setPeerProcess(d, agent, "/usr/bin/definitely-not-this");

        A::capturePeerIdentity(d, agent);

        CHECK(A::peerProcessID(d, agent) == static_cast<std::uint32_t>(::getpid()));
        CHECK(A::peerProcess(d, agent) != "/usr/bin/definitely-not-this");
        CHECK(!A::peerProcess(d, agent).empty());
        ::close(peerEnd);
    }

    // ---- 13. A client with no local descriptor gets no verified identity at
    // all. That is the remote case: the daemon cannot ask the kernel who is on
    // the far end of a network connection, so such a peer can never hold a
    // persisted "always" grant.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        const std::size_t agent = A::addUnidentifiedAgent(d);
        A::clearFileDescriptor(d, agent);
        A::setPeerProcess(d, agent, std::string());

        A::capturePeerIdentity(d, agent);

        CHECK(A::peerProcessID(d, agent) == 0);
        CHECK(A::peerProcess(d, agent).empty());
        CHECK(A::grantKey(d, agent).empty()); // nothing to persist a grant against
    }

    // A person's allow stands when the same person then declines the
    // question as unshown before the allow is served: leaving a question is
    // not refusing it, so an answer given is never overturned into a denial.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-unshown");
        A::queueAnswer(d, human, theRequest, Sync::ConsentChoice::AllowOnce);
        A::serviceQueuedAnswer(d, human);
        CHECK_MSG(A::pending(d, agent) && !A::admitted(d, agent), "the allow was served before a second answer could follow it");
        A::queueAnswer(d, human, theRequest, Sync::ConsentChoice::Unshown);
        A::serviceQueuedAnswer(d, human);
        A::settlePending(d);
        CHECK_MSG(A::admitted(d, agent), "a person's unshown after their own allow denied the agent");
        CHECK(!A::pending(d, agent));
    }

    // A person who declines a question as unshown is not asked it again on
    // the host's next pass, so a client that cannot show it is never asked
    // it in a loop, and the question waits on the person still asked.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        int firstRead = -1;
        int secondRead = -1;
        const std::size_t first = A::addHuman(d, theSession, &firstRead);
        const std::size_t second = A::addHuman(d, theSession, &secondRead);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-declined");
        const auto questionsTo = [&](std::size_t human, int readEnd) {
            std::size_t count = 0;
            for (const auto& notification : A::notifiesWritten(d, human, readEnd))
                count += UbiquittyTest::consentQuestionOf(notification) ? 1 : 0;
            return count;
        };
        CHECK_EQ(questionsTo(first, firstRead), 1u);
        CHECK_EQ(questionsTo(second, secondRead), 1u);
        A::answer(d, second, theRequest, Sync::ConsentChoice::Unshown);
        A::settlePending(d);
        CHECK(A::pending(d, agent));
        CHECK_MSG(questionsTo(second, secondRead) == 0, "a person who declined to show a question was asked it again");
        A::answer(d, first, theRequest, Sync::ConsentChoice::AllowOnce);
        CHECK(A::admitted(d, agent));
    }

    // A person whose link was lost is asked again on resuming, so the
    // question waits for them rather than being denied as they drop.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-lost-link");
        Ubiquitty::Host::ScreenPlaneAccess::drop(d, human);
        CHECK(A::pending(d, agent));
        Ubiquitty::Host::ScreenPlaneAccess::reap(d, human);
        const auto survivingAgent = agent - 1;
        A::settlePending(d);
        CHECK_MSG(A::pending(d, survivingAgent), "an attach question was denied as its only approver's link was lost");
        CHECK(!A::admitted(d, survivingAgent));
    }

    // Declining a question already decided is answered as one no longer
    // pending, the same as an answer that reaches it late.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t first = A::addHuman(d, theSession);
        const std::size_t second = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-decided");
        A::answer(d, first, theRequest, Sync::ConsentChoice::AllowOnce);
        CHECK(A::admitted(d, agent));
        A::answer(d, second, theRequest, Sync::ConsentChoice::Unshown);
        CHECK_MSG(A::hasError(d, second, Ubiquitty::Protocol::Control::ErrorCode::NotFound), "declining a decided question was not answered as no longer pending");
    }

    // A person whose client says it is suspending, as an iOS app does going
    // to the background, may resume: the connection leaves the attachment
    // lost itself, so the question waits rather than being denied.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-suspending");
        A::sayBye(d, human, Sync::ByeReason::Suspending);
        Ubiquitty::Host::ScreenPlaneAccess::drop(d, human);
        CHECK(A::pending(d, agent));
        Ubiquitty::Host::ScreenPlaneAccess::reap(d, human);
        const auto survivingAgent = agent - 1;
        A::settlePending(d);
        CHECK_MSG(A::pending(d, survivingAgent), "an attach question was denied as its only approver suspended");
    }

    // A person who may return keeps the question open whoever leaves after
    // them: the next to decline it does not refuse it.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t lost = A::addHuman(d, theSession);
        const std::size_t other = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-returning");
        Ubiquitty::Host::ScreenPlaneAccess::drop(d, lost);
        CHECK(A::pending(d, agent));
        Ubiquitty::Host::ScreenPlaneAccess::reap(d, lost);
        const auto survivingAgent = agent - 1;
        A::answer(d, other - 1, theRequest, Sync::ConsentChoice::Unshown);
        A::settlePending(d);
        CHECK_MSG(A::pending(d, survivingAgent), "a decline after a person who may return left refused the question");
    }

    // An allow stands though its approver leaves before it is delivered: the
    // answer was checked against their authority when they gave it.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-delivered");
        A::queueAnswer(d, human, theRequest, Sync::ConsentChoice::AllowOnce);
        A::serviceQueuedAnswer(d, human);
        CHECK(A::hasValidatedApproval(d, agent, human, { theRequest, theSession }, Sync::ConsentChoice::AllowOnce));
        Ubiquitty::Host::ScreenPlaneAccess::drop(d, human);
        CHECK(A::hasApproval(d, agent));
        Ubiquitty::Host::ScreenPlaneAccess::reap(d, human);
        const auto survivingAgent = agent - 1;
        A::settlePending(d);
        CHECK_MSG(A::admitted(d, survivingAgent), "an allow was overturned as its approver left before it was delivered");
    }

    // A recipient the host kicks answers nothing it was asked, so its
    // questions are left to the others at once, not when it is reaped.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        int agentRead = -1;
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-kicked", &agentRead);
        A::kick(d, human);
        A::settlePending(d);
        CHECK_MSG(!A::pending(d, agent), "a question its only recipient could no longer answer waited for its reaping");
        CHECK(A::deniedReason(d, agent, agentRead) == std::to_underlying(Sync::ConsentStatusPhase::Denied));
    }

    // A decliner who briefly cannot be asked stays declined, so the host does
    // not ask the same connection again once it can.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        A::addSession(d, theSession + 1);
        (void)A::addHuman(d, theSession);
        int secondRead = -1;
        const std::size_t second = A::addHuman(d, theSession, &secondRead);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-briefly");
        A::answer(d, second, theRequest, Sync::ConsentChoice::Unshown);
        (void)A::notifiesWritten(d, second, secondRead);
        A::moveHuman(d, second, theSession + 1);
        (void)A::addHuman(d, theSession);
        A::settlePending(d);
        A::moveHuman(d, second, theSession);
        A::settlePending(d);
        std::size_t asked = 0;
        for (const auto& notification : A::notifiesWritten(d, second, secondRead))
            asked += UbiquittyTest::consentQuestionOf(notification) ? 1 : 0;
        CHECK_MSG(asked == 0, "a connection that declined was asked again after briefly not being askable");
        CHECK(A::pending(d, agent));
    }

    // A question the asking prunes down to nobody, with a person attached who
    // is shown no questions and so is never asked, is refused at once.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        A::addSession(d, theSession + 1);
        int agentRead = -1;
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-pruned", &agentRead);
        A::moveHuman(d, human, theSession + 1);
        (void)A::addEditorHuman(d, theSession);
        A::settlePending(d);
        CHECK_MSG(!A::pending(d, agent), "a question nobody was left to answer waited for its deadline");
        CHECK(A::deniedReason(d, agent, agentRead) == std::to_underlying(Sync::ConsentStatusPhase::Denied));
    }

    // A person whose client said it was suspending is still listed as asked
    // until the host drops it, and may resume: a late person attaching who
    // cannot be asked does not prune them away and leave the question to be
    // refused.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-suspending-asked");
        A::sayBye(d, human, Sync::ByeReason::Suspending);
        (void)A::addEditorHuman(d, theSession);
        A::settlePending(d);
        CHECK_MSG(A::pending(d, agent), "a suspending person still asked was pruned from a question they may resume to");
    }

    // A person whose link goes silent stays a recipient that may resume while
    // the host drops it: a late person attaching then, who cannot be asked,
    // does not prune them away and leave the question to be refused.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-silent");
        A::dropSilent(d, human);
        (void)A::addEditorHuman(d, theSession);
        A::settlePending(d);
        CHECK_MSG(A::pending(d, agent), "a person whose link went silent was pruned from a question they may resume to");
    }

    // An asking that fails for a person who could be asked keeps the question
    // pending; they are asked on a later pass.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        int humanRead = -1;
        const std::size_t human = A::addHuman(d, theSession, &humanRead);
        A::owesRetirement(d, human);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-retried");
        A::settlePending(d);
        CHECK_MSG(A::pending(d, agent), "a question one failed asking reached nobody was refused at once");
        A::settleRetirements(d, human);
        (void)A::notifiesWritten(d, human, humanRead);
        A::settlePending(d);
        std::size_t asked = 0;
        for (const auto& notification : A::notifiesWritten(d, human, humanRead))
            asked += UbiquittyTest::consentQuestionOf(notification) ? 1 : 0;
        CHECK_MSG(asked == 1, "the person a failed asking missed was not asked again");
        CHECK(A::pending(d, agent));
    }

    // A retained decision still records the recipient leaving, whether an
    // approval or a terminal status is waiting for output capacity.
    for (const bool hasTerminalStatus : { false, true }) {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-held-decision");
        const auto humanID = A::clientID(d, human);
        if (hasTerminalStatus)
            A::holdTerminal(d, agent);
        else {
            A::queueAnswer(d, human, theRequest, Sync::ConsentChoice::AllowOnce);
            A::serviceQueuedAnswer(d, human);
            CHECK(A::hasApproval(d, agent));
        }
        A::sayBye(d, human, Sync::ByeReason::Suspending);
        Ubiquitty::Host::ScreenPlaneAccess::drop(d, human);
        CHECK(A::hasRecipientLeft(d, agent, humanID));
        if (!hasTerminalStatus)
            CHECK(A::hasApproval(d, agent));
        Ubiquitty::Host::ScreenPlaneAccess::reap(d, human);
        const auto survivingAgent = agent - 1;
        CHECK_MSG(A::hasRecipientLeft(d, survivingAgent, humanID), "a held decision kept its departed recipient listed as asked");
        if (!hasTerminalStatus)
            CHECK_MSG(A::hasApproval(d, survivingAgent), "recording a departure refused an approval awaiting delivery");
    }

    // A listed decliner is not a fresh person who could still be asked. Once
    // the other recipient leaves the pane, no answer can arrive.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        A::addSession(d, theSession + 1);
        const std::size_t departing = A::addHuman(d, theSession);
        const std::size_t declined = A::addHuman(d, theSession);
        int agentRead = -1;
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-only-decliner", &agentRead);
        A::answer(d, declined, theRequest, Sync::ConsentChoice::Unshown);
        CHECK(A::pending(d, agent));
        A::moveHuman(d, departing, theSession + 1);
        A::settlePending(d);
        CHECK_MSG(!A::pending(d, agent), "an askable person who already declined kept a question alive");
        CHECK(A::deniedReason(d, agent, agentRead) == std::to_underlying(Sync::ConsentStatusPhase::Denied));
    }

    // A host-superseded carrier may still overflow before its reap. Its
    // recipient continues on the replacement, rather than departing afresh.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t superseded = A::addHuman(d, theSession);
        (void)A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-host-superseded");
        const auto humanID = A::clientID(d, superseded);
        A::supersedeAndDropBehind(d, superseded);
        CHECK_MSG(A::hasRecipientContinued(d, agent, humanID), "a farewell changed a host-superseded recipient into a departure");
        CHECK(A::pending(d, agent));
    }

    // Store-bound approval authority survives only while its digest remains
    // valid, even when the approving connection has already been reaped.
    enum class CredentialChange {
        Unchanged,
        Revoked,
        Expired
    };
    for (const auto change : { CredentialChange::Unchanged, CredentialChange::Revoked, CredentialChange::Expired }) {
        for (const bool isReaped : { false, true }) {
            Daemon d;
            A::useScratchStore(d, scratch);
            A::useTokenStore(d, scratch);
            A::addSession(d, theSession);
            const auto requesterDigest = Ubiquitty::Crypto::sha256Hex("consent-requester");
            const auto approverDigest = Ubiquitty::Crypto::sha256Hex("consent-approver");
            A::writeTokens(d, requesterDigest, approverDigest);
            const auto human = A::addTokenHuman(d, theSession, approverDigest);
            (void)A::addHuman(d, theSession);
            const auto originalAgent = A::addHeldAgent(d, theSession, theRequest, requesterDigest);
            A::queueAnswer(d, human, theRequest, Sync::ConsentChoice::AllowOnce);
            A::serviceQueuedAnswer(d, human);
            CHECK(A::hasApproval(d, originalAgent));
            if (isReaped) {
                Ubiquitty::Host::ScreenPlaneAccess::drop(d, human);
                CHECK(A::hasApproval(d, originalAgent));
                Ubiquitty::Host::ScreenPlaneAccess::reap(d, human);
            }
            const auto agent = originalAgent - (isReaped ? 1 : 0);
            CHECK_MSG(A::hasApproval(d, agent), "reaping alone voided a retained decision");
            if (change == CredentialChange::Expired)
                A::expireRetainedToken(d, approverDigest);
            else {
                A::writeTokens(d, requesterDigest, change == CredentialChange::Revoked ? std::string_view {} : approverDigest);
                Ubiquitty::Host::ScreenPlaneAccess::reverifyTokens(d);
            }
            CHECK_MSG(A::hasApproval(d, agent) == (change == CredentialChange::Unchanged),
                "retained approval did not follow its approver's exact token validity");
            CHECK_MSG(!A::admitted(d, agent), "credential verification promoted a held approval before delivery");
            if (change != CredentialChange::Unchanged) {
                A::settlePending(d);
                CHECK_MSG(!A::admitted(d, agent), "an invalid approver credential still authorized attachment");
                CHECK_MSG(A::pending(d, agent), "invalidating one approval refused the question still shown to another person");
            }
            ::unlink((scratch + "/tokens").c_str());
        }
    }

    // An approval the owner withdraws, disconnecting the approver, or that a
    // revoked token withdraws, before the agent takes it, is void.
    for (const bool isRevoked : { false, true }) {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, isRevoked ? "token-digest-revoked" : "token-digest-disconnected");
        A::queueAnswer(d, human, theRequest, Sync::ConsentChoice::AllowOnce);
        A::serviceQueuedAnswer(d, human);
        CHECK(A::hasValidatedApproval(d, agent, human, { theRequest, theSession }, Sync::ConsentChoice::AllowOnce));
        if (isRevoked)
            A::refuse(d, human, Sync::ByeReason::TokenRevoked);
        else
            A::kick(d, human);
        A::settlePending(d);
        CHECK_MSG(!A::admitted(d, agent), isRevoked ? "an approval outlived its approver's revoked token" : "an approval outlived the owner disconnecting its approver");
        CHECK(!A::pending(d, agent));
    }

    // A recipient the host refuses, its token revoked, answers nothing more,
    // so its questions are left at once rather than when it is reaped.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        int agentRead = -1;
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-refused", &agentRead);
        A::refuse(d, human, Sync::ByeReason::TokenRevoked);
        A::settlePending(d);
        CHECK_MSG(!A::pending(d, agent), "a question its refused recipient could no longer answer waited for its reaping");
        CHECK(A::deniedReason(d, agent, agentRead) == std::to_underlying(Sync::ConsentStatusPhase::Denied));
    }

    // A grant an approval creates names the approver in the management
    // history though they left before the agent took it.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        (void)A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-history");
        const std::size_t approver = A::addHuman(d, theSession);
        CHECK(approver + 1 == A::clientCount(d));
        const std::string identity = A::managementIdentity(d, approver);
        A::queueAnswer(d, approver, theRequest, Sync::ConsentChoice::AllowSession);
        A::serviceQueuedAnswer(d, approver);
        CHECK(A::hasValidatedApproval(d, agent, approver, { theRequest, theSession }, Sync::ConsentChoice::AllowSession));
        // The history is kept beside a token store, which the fixture's
        // tokens are not in: it is named only once the agent is approved.
        A::useTokenStore(d, scratch);
        Ubiquitty::Host::ScreenPlaneAccess::reap(d, approver);
        A::settlePending(d);
        CHECK(A::admitted(d, agent));
        std::string key;
        CHECK(A::sessionGrant(d, theSession, &key));
        CHECK_MSG(!identity.empty() && A::createdBy(scratch, "grant:session:" + std::to_string(theSession) + ":" + key) == identity,
            "a grant made by an approver who left names no one in the management history");
        ::unlink((scratch + "/tokens.metadata").c_str());
        ::unlink((scratch + "/tokens.lock").c_str());
    }

    // Declining a question a person has allowed, which no agent has taken
    // yet, is answered as one no longer pending: it is decided.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t first = A::addHuman(d, theSession);
        const std::size_t second = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-allowed");
        A::queueAnswer(d, first, theRequest, Sync::ConsentChoice::AllowOnce);
        A::serviceQueuedAnswer(d, first);
        CHECK(A::hasValidatedApproval(d, agent, first, { theRequest, theSession }, Sync::ConsentChoice::AllowOnce));
        A::queueAnswer(d, second, theRequest, Sync::ConsentChoice::Unshown);
        A::serviceQueuedAnswer(d, second);
        CHECK_MSG(A::hasError(d, second, Ubiquitty::Protocol::Control::ErrorCode::NotFound), "declining an allowed question was answered as if it still waited");
        A::settlePending(d);
        CHECK(A::admitted(d, agent));
    }

    // A peer's own Bye naming Superseded is a link it ended: its person may
    // resume, as one whose link was lost may, and the question waits. Only the
    // host superseding a connection says a resume continued it elsewhere.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        const std::size_t human = A::addHuman(d, theSession);
        const std::size_t agent = A::addHeldAgent(d, theSession, theRequest, "token-digest-peer-superseded");
        A::sayBye(d, human, Sync::ByeReason::Superseded);
        Ubiquitty::Host::ScreenPlaneAccess::drop(d, human);
        CHECK(A::pending(d, agent));
        Ubiquitty::Host::ScreenPlaneAccess::reap(d, human);
        const auto survivingAgent = agent - 1;
        A::settlePending(d);
        CHECK_MSG(A::pending(d, survivingAgent), "a peer's Bye naming Superseded was taken as the host's own supersession");
    }

    // With only a person who is shown no questions attached, an agent that
    // needs consent to attach is refused at once: nobody can be asked.
    {
        Daemon d;
        A::useScratchStore(d, scratch);
        A::addSession(d, theSession);
        (void)A::addEditorHuman(d, theSession);
        int agentRead = -1;
        const std::size_t agent = A::addAgentNeedingConsent(d, theSession, "token-digest-editor-only", &agentRead);
        CHECK_MSG(A::refusal(d, agent, agentRead) == Sync::ByeReason::NoConsent, "an attach nobody could be asked about was held");
        CHECK(!A::pending(d, agent));
    }

    ::unlink((scratch + "/consent").c_str());
    ::unlink((scratch + "/capability_grants").c_str());
    ::unlink((scratch + "/capability_grants.lock").c_str());
    ::rmdir(scratch.c_str());

    if (failures == 0)
        std::printf("consent_answer_attach: a human's answer lands on one principal and one durability — "
                    "session scope bound to the approved agent's identity, always persisted only for a "
                    "token-bound one and degraded to session scope when its durable write fails, "
                    "once buying nothing further, and anything else failing closed\n");
    return UBIQUITTY_TEST_SUMMARY() == 0 ? 0 : 1;
}
