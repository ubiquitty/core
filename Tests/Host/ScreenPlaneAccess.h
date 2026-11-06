// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/Host/ScreenPlaneAccess.h — a friend seam onto the daemon's screen
// plane, shared by the Host tests that drive it without its poll loop.
//
// A test makes sessions and subscribed clients, greets each client's screen
// connection with a ScreenClient, publishes frames, and runs the loop's screen
// step: a session whose terminal changed since its last frame holds no frame
// a connection may encode from, and every client's connection is serviced. A
// client's output is the daemon's own buffer, read and emptied by the test as a
// transport would, so whether the bytes reach the client is the test's choice.
//

#include "Core/Protocol/Sync/Control.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/TrustedLocalPeer.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Ubiquitty::Host {

/// The friend seam the screen-plane tests reach the daemon through.
struct ScreenPlaneAccess {
    /// Reads the daemon's monotonic clock.
    ///
    /// - Returns: Microseconds.
    static std::uint64_t now()
    {
        struct timespec ts;
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000ull;
    }

    /// The daemon's accept clock, in its milliseconds: a client accepted now
    /// has the hello grace from here, where one stamped zero is past it at the
    /// loop's first reap.
    ///
    /// - Returns: The monotonic time in milliseconds.
    static std::uint64_t acceptedAt() { return now() / 1000; }

    /// Makes a session over a stream double, with the screen source a session
    /// the daemon creates carries.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - rows: The session's rows.
    ///   - columns: The session's columns.
    ///   - stream: Its PTY; a closed one when the test drives the terminal itself.
    /// - Returns: The session.
    static Daemon::Session& makeSession(Daemon& d, int rows, int columns, std::unique_ptr<PAL::Stream> stream = TestDoubles::makeClosedStream())
    {
        auto s = std::make_unique<Daemon::Session>();
        s->id = d.m_nextSessionID++;
        s->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*s->term, rows, columns);
        s->stream = std::move(stream);
        s->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*s->term, Protocol::Sync::ID128 { .low = s->id, .high = 1 });
        d.wireBackgroundNotifications(*s);
        (void)d.wireRemoteInput(*s);
        s->isFrameDue = true;
        Daemon::Session& made = *s;
        d.m_sessions.push_back(std::move(s));
        ++d.m_backgroundInventoryRevision;
        return made;
    }

    /// Selects an existing fixture session for a not-yet-admitted client.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - sessionID: The session.
    ///   - fileDescriptor: Its transport; an invalid one when the test reads
    ///     the output buffer itself.
    /// - Returns: The client's index.
    static std::size_t attach(Daemon& d, std::uint32_t sessionID, PAL::UnixFileDescriptor fileDescriptor = {})
    {
        Daemon::Client c;
        c.clientID = d.m_nextClientID++;
        c.connectMilliseconds = acceptedAt();
        c.fileDescriptor = std::move(fileDescriptor);
        c.sessionID = sessionID;
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    /// Adds a client that has connected and said nothing, for a test that
    /// sends its canonical Greet through `handleFrames`.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - fileDescriptor: Its transport, which the daemon answers on.
    /// - Returns: The client's index.
    static std::size_t connect(Daemon& d, PAL::UnixFileDescriptor fileDescriptor)
    {
        Daemon::Client c;
        c.clientID = d.m_nextClientID++;
        c.connectMilliseconds = acceptedAt();
        c.fileDescriptor = std::move(fileDescriptor);
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    /// Moves a client to another transport, as the test's next phase needs:
    /// one that holds what the greeting queued for `deliver`, then one that
    /// takes everything the daemon writes.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - fileDescriptor: The transport the daemon writes to from now on.
    static void useTransport(Daemon& d, std::size_t index, PAL::UnixFileDescriptor fileDescriptor)
    {
        d.m_clients[index].fileDescriptor = std::move(fileDescriptor);
    }

    /// Selects unbound Control admission for a not-yet-admitted client.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The client's index.
    static std::size_t attachControlOnly(Daemon& d)
    {
        Daemon::Client c;
        c.clientID = d.m_nextClientID++;
        c.connectMilliseconds = acceptedAt();
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }

    /// Lets an agent attach to a session without asking, as a person's
    /// allow-for-this-session answer does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - session: The session.
    ///   - index: The agent's client.
    static void grantAgentSession(Daemon& d, Daemon::Session& session, std::size_t index)
    {
        (void)session.addSessionGrant(d.grantKeyFor(d.m_clients[index]));
    }

    /// Greets an agent a person has allowed to attach once: the daemon admits
    /// it as it admits a holder of a standing grant, asking nobody, and the
    /// session keeps no standing grant afterwards, as an allow-once answer
    /// leaves none. The agent must already have the scope its target needs.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - session: The session the agent selected.
    ///   - index: The agent's client, not yet greeted.
    static void greetAllowedAgent(Daemon& d, Daemon::Session& session, std::size_t index)
    {
        auto heldGrants = session.agentSessionGrantIdentities;
        grantAgentSession(d, session, index);
        greetExisting(d, index, Protocol::Sync::ClientKind::Agent, Protocol::Sync::Role::Agent);
        session.agentSessionGrantIdentities = std::move(heldGrants);
    }

    /// Grants a client a token's scope, as a valid token in its HELLO does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - scope: What the token allows.
    static void grantScope(Daemon& d, std::size_t index, Protocol::Sync::ScopeSet scope)
    {
        d.m_clients[index].hasToken = true;
        d.m_clients[index].tokenScope = scope;
    }

    /// Marks a client's token as minted for the person at the minting client,
    /// as a real human client's is: the daemon then verifies it as a person
    /// rather than automation.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    static void markInteractiveUser(Daemon& d, std::size_t index) { d.m_clients[index].isInteractiveUser = true; }

    /// Sets the automation label that an authenticated HELLO carries.
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The authenticated client.
    ///   - label: Its declared automation label.
    static void setAgentLabel(Daemon& d, std::size_t index, std::string label) { d.m_clients[index].agentLabel = std::move(label); }

    /// Checks the production human/agent classifier after proof admission.
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The admitted client.
    /// - Returns: Whether the client is subject to agent consent.
    static bool isAgent(const Daemon& d, std::size_t index) { return d.isAgentClient(d.m_clients[index]); }

    /// Names a client's token, which its principal then is.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - tokenID: The token's digest.
    static void setTokenID(Daemon& d, std::size_t index, std::string tokenID) { d.m_clients[index].tokenID = std::move(tokenID); }

    /// Carries the verified token verdict's expiry onto its connection.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - expiry: The Unix deadline, or zero for no deadline.
    static void setTokenExpiry(Daemon& d, std::size_t index, std::uint64_t expiry) { d.m_clients[index].tokenExpiryUnix = expiry; }

    /// Marks the verified in-memory bootstrap origin, not a token-store record.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    static void markBootstrapToken(Daemon& d, std::size_t index) { d.m_clients[index].hasBootstrapToken = true; }

    /// Selects the test's owned token store.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - path: The token-store path inside the fixture's temporary directory.
    static void useTokenStore(Daemon& d, const std::string& path) { d.m_tokenPath = path; }

    /// Runs the production token-store withdrawal sweep.
    ///
    /// - Parameter d: The daemon.
    static void reverifyTokens(Daemon& d) { d.reverifyTokenClients(); }

    /// Reads the token deadline used by the reactor's poll timeout.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The earliest Unix deadline, or zero if no deadline remains.
    static std::uint64_t nextTokenExpiry(const Daemon& d) { return d.earliestTokenExpiryUnix(); }

    /// Moves retained authority past its deadline without sleeping.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - attachmentID: The retained attachment to expire.
    static void expireRetainedToken(Daemon& d, Protocol::Sync::ID128 attachmentID)
    {
        const auto found = std::ranges::find(d.m_attachmentGrants, attachmentID, &Daemon::AttachmentGrant::attachmentID);
        if (found != d.m_attachmentGrants.end())
            found->tokenExpiryUnix = 1;
    }

    /// Uses exactly the production proof admission and authority installation.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The replacement client.
    ///   - proof: The attachment's resume proof.
    ///   - exporter: The replacement transport's exported key material.
    /// - Returns: Success or the production proof refusal.
    static auto admitProof(Daemon& d, std::size_t index, const Protocol::Sync::ResumeProof& proof, std::span<const std::uint8_t, 32> exporter)
    {
        return d.admitAttachmentProof(index, proof, exporter);
    }

    /// Checks the verified authority restored by a proof, including its origin.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The replacement client.
    ///   - digest: The expected token digest.
    ///   - scope: The expected operation scope.
    ///   - expiry: The expected Unix deadline.
    /// - Returns: Whether the client retains exactly this non-interactive stored authority.
    static bool hasStoredAuthority(const Daemon& d, std::size_t index, const std::string& digest, Protocol::Sync::ScopeSet scope, std::uint64_t expiry)
    {
        const auto& client = d.m_clients[index];
        return client.hasToken && !client.hasBootstrapToken && !client.isInteractiveUser
            && client.tokenID == digest && client.tokenScope == scope && client.tokenExpiryUnix == expiry;
    }

    /// Checks that a bootstrap proof preserves the verified human identity.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The replacement client.
    /// - Returns: Whether the client retains interactive bootstrap authority without a store deadline.
    static bool hasBootstrapAuthority(const Daemon& d, std::size_t index)
    {
        const auto& client = d.m_clients[index];
        return client.hasToken && client.hasBootstrapToken && client.isInteractiveUser && !client.tokenExpiryUnix;
    }

    /// The daemon's attachments.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The table.
    static Protocol::Sync::AttachmentTable& attachments(Daemon& d) { return *d.m_attachments; }

    /// Reads the outer carrier identity, which is independent of the full issuer.
    /// - Parameter d: The daemon.
    /// - Returns: The exact identity used by HELLO and stable pane origins.
    static std::uint64_t daemonInstanceID(const Daemon& d) { return d.m_daemonInstanceID; }

    /// Captures the production attachment and grant continuation.
    /// - Parameters:
    ///   - d: The frozen predecessor.
    ///   - at: The shared monotonic freeze time.
    /// - Returns: Owned authority without physical clients.
    static AttachmentHandover exportAttachmentAuthority(const Daemon& d, std::uint64_t at) { return d.exportAttachmentAuthority(at); }

    /// Uses the production transactional authority admission before any client.
    /// - Parameters:
    ///   - d: The fresh successor.
    ///   - carried: Owned predecessor state.
    ///   - predecessor: The authenticated predecessor identity.
    ///   - at: The shared monotonic time.
    ///   - nowUnix: The current token-expiry clock.
    /// - Returns: The production result without changing a refused candidate.
    static auto inheritAttachmentAuthority(Daemon& d, AttachmentHandover&& carried, Protocol::Sync::ID128 predecessor,
        std::uint64_t at, std::uint64_t nowUnix)
    {
        return d.inheritAttachmentAuthority(std::move(carried), predecessor, at, nowUnix);
    }

    /// Prepares an unpublished candidate through the production admission path.
    static auto prepareAttachmentAuthority(Daemon& d, AttachmentHandover&& carried, Protocol::Sync::ID128 predecessor,
        std::uint64_t at, std::uint64_t nowUnix)
    {
        return d.prepareAttachmentAuthority(std::move(carried), predecessor, at, nowUnix);
    }

    /// Marks a client as one a QUIC preamble authenticated, as a bootstrap
    /// token's does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    static void markQUICAuthenticated(Daemon& d, std::size_t index) { d.m_clients[index].quicAuthed = true; }

    /// Counts retained token-authority grants, including socket attachments.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The grants.
    static std::size_t grantCount(const Daemon& d) { return d.m_attachmentGrants.size(); }

    /// Reads the scope kept for an attachment, which a connection a resume
    /// proof admits is served under.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - attachmentID: The attachment.
    /// - Returns: The scope, or `nullopt` when no grant is kept for it.
    static std::optional<Protocol::Sync::ScopeSet> grantedScope(const Daemon& d, Protocol::Sync::ID128 attachmentID)
    {
        const auto found = std::ranges::find(d.m_attachmentGrants, attachmentID, &Daemon::AttachmentGrant::attachmentID);
        return found == d.m_attachmentGrants.end() ? std::nullopt : std::optional { found->scope };
    }

    /// Reads the daemon's name for a client's connection.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: The connection's number.
    static std::uint64_t clientID(const Daemon& d, std::size_t index) { return d.m_clients[index].clientID; }

    /// Closes the connections the attachment table listed as superseded, as
    /// the daemon does after a resume or a proof.
    ///
    /// - Parameter d: The daemon.
    static void closeSuperseded(Daemon& d) { d.closeSupersededConnections(); }

    /// Whether a client is draining to its close: one a resume superseded,
    /// among them.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: Whether the loop will reap it.
    static bool isLeaving(const Daemon& d, std::size_t index) { return d.m_clients[index].sessionExitDeadlineMilliseconds != 0; }

    /// Runs the loop's reap of attachments at a time.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - now: The daemon clock.
    static void reapAttachments(Daemon& d, std::uint64_t now) { d.reapAttachments(now); }

    /// Drops a client as the loop does when its transport fails.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    static void drop(Daemon& d, std::size_t index) { d.dropClient(index); }

    /// Withdraws a client's standing through the common consent-revoke path.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client whose authority ends.
    static void kick(Daemon& d, std::size_t index) { d.kickClient(index); }

    /// Runs the actual writable-PTY branch, including its post-wait expiry check.
    /// - Parameters:
    ///   - d: The daemon.
    ///   - session: The session whose stream became writable.
    static void drainReadyInput(Daemon& d, Daemon::Session& session) { d.drainRemoteInput(session); }

    /// Sends an authorization refusal through the daemon's ordinary handler.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client whose authority ends.
    ///   - reason: The refusal carried to the client.
    static void denyAuthorization(Daemon& d, std::size_t index, Protocol::Sync::ByeReason reason) { d.refuseClient(index, reason); }

    /// Expires one token through the reactor's expiry sweep, without waiting.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client whose token is made past due.
    static void expireToken(Daemon& d, std::size_t index)
    {
        d.m_clients[index].tokenExpiryUnix = 1;
        d.dropExpiredTokenClients();
    }

    /// Reads the single-use question retaining a typed termination.
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The requester.
    /// - Returns: Its request ID, or zero if no termination is held.
    static std::uint32_t terminationQuestion(const Daemon& d, std::size_t index)
    {
        const auto& client = d.m_clients[index];
        return client.consentPending && client.terminationCall ? client.consentRequestID : 0;
    }

    /// Expires a retained termination through the production denial path.
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The requester.
    static void expireTermination(Daemon& d, std::size_t index)
    {
        d.m_clients[index].consentDeadlineMilliseconds = 0;
        d.denyAgentAttach(index, Protocol::Sync::ConsentStatusPhase::Expired);
    }

    /// Counts persistent attach approvals, which termination must not create.
    /// - Parameter d: The daemon.
    /// - Returns: The number of persisted principal grants.
    static std::size_t alwaysAttachGrants(const Daemon& d) { return d.m_alwaysAllowedAgents.size(); }

    /// Fills a real outstanding call's bulk answer behind a held termination.
    /// - Parameters:
    ///   - d: The fixture daemon.
    ///   - index: The exact caller.
    /// - Returns: The termination's Core call index, or zero if admission failed.
    static std::uint32_t fillTerminationReplyCapacity(Daemon& d, std::size_t index)
    {
        auto& client = d.m_clients[index];
        if (!client.terminationCall || !client.syncConnection)
            return 0;
        auto& connection = *client.syncConnection;
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, Protocol::Sync::Call { .text = R"({"jsonrpc":"2.0","id":"pressure","method":"host.describe","params":{}})" });
        std::vector<std::uint8_t> stream;
        if (!Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Call, 0, body))
            return 0;
        connection.receiveStream(stream, now());
        const auto calls = connection.takeCalls();
        if (calls.size() != 1)
            return 0;
        const auto queued = connection.queuedBytes();
        if (queued > Protocol::Sync::provisionalLimits.maxQueuedBulkBytes - 2)
            return 0;
        std::string answer(Protocol::Sync::provisionalLimits.maxQueuedBulkBytes - queued, 'x');
        answer.front() = '"';
        answer.back() = '"';
        if (connection.answerCall(calls.front().callIndex, std::move(answer)) != Protocol::Sync::CallAnswer::Sent)
            return 0;
        return client.terminationCall->callIndex;
    }

    /// Probes whether a consumed termination can still be answered.
    /// - Parameters:
    ///   - d: The fixture daemon.
    ///   - index: The exact caller.
    ///   - callIndex: The held Core call identity.
    /// - Returns: True only when Core no longer holds that operation.
    static bool isTerminationCallRetired(Daemon& d, std::size_t index, std::uint32_t callIndex)
    {
        return d.m_clients[index].syncConnection->answerCall(callIndex, "{}") == Protocol::Sync::CallAnswer::NotOutstanding;
    }

    /// Advances only the exact-operation consent clock, without sleeping.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - at: Monotonic milliseconds.
    static void expireStructureConsents(Daemon& d, std::uint64_t at) { d.expireStructureConsents(at); }

    /// Reads the bounded queue of exact operations awaiting a person.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The number of held calls.
    static std::size_t pendingStructureConsents(const Daemon& d) { return d.m_structureConsents.size(); }

    /// Counts retirements waiting for one recipient's output budget.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The recipient.
    /// - Returns: The number of retirement identifiers held.
    static std::size_t pendingStructureRetirements(const Daemon& d, std::size_t index) { return d.m_clients[index].pendingConsentRetirements.size(); }

    /// Fills a connection's ordinary Notify queue without draining transport.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The recipient.
    /// - Returns: Whether the bounded queue refused another notification.
    static bool fillNotifyQueue(Daemon& d, std::size_t index)
    {
        // A kind no build knows, which a client skips: only its size matters.
        std::vector<std::uint8_t> body(60 * 1024, 'x');
        body.front() = 0x7F;
        for (std::size_t count = 0; count < 64; ++count) {
            if (!d.m_clients[index].syncConnection->sendNotify(body))
                return true;
        }
        return false;
    }

    /// Resumes an attachment onto another stream without first reaping the
    /// superseded stream, exposing the ownership transition to consent checks.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The superseded client.
    /// - Returns: Whether the attachment table admitted the resume.
    static bool resumeAttachmentElsewhere(Daemon& d, std::size_t index)
    {
        const auto* attachment = d.m_clients[index].syncConnection->attachment();
        if (!attachment)
            return false;
        const Protocol::Sync::ResumeRequest request {
            .attachmentID = attachment->attachmentID,
            .resumeSecret = attachment->resumeSecret,
            .panes = {},
        };
        return d.m_attachments->resume(request, attachment->authority, UINT64_MAX, now()).has_value();
    }

    /// Calls admission without the normal publication pass, to prove a refusal
    /// does not create a target merely by looking it up.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The caller.
    ///   - text: The JSON-RPC call; used only for immediate-refusal cases.
    /// - Returns: The refusal, or nothing if admission held the call.
    static std::optional<Codec::JSON::Value> askStructureWithoutPublishing(Daemon& d, std::size_t index, std::string_view text)
    {
        return d.beginStructureConsent(index, 0, Protocol::Sync::decodeCallRequest(text));
    }

    /// Answers consent between an attachment-table transition and the loop's
    /// close of superseded streams.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The answering client.
    ///   - text: A schema-valid consent answer.
    /// - Returns: The answer's response.
    static Codec::JSON::Value answerStructureWithoutServicing(Daemon& d, std::size_t index, std::string_view text)
    {
        return d.serveConsentResponse(d.m_clients[index], Protocol::Sync::decodeCallRequest(text));
    }

    /// Services one connection's real Call/Result path before the loop's
    /// deferred session-close pass, so tests can inspect answer lifetimes.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The connection being answered.
    static void serviceConnectionBeforeClosures(Daemon& d, std::size_t index) { d.serviceSyncConnection(index, now()); }

    /// Applies only the closures owed by completed calls, without servicing
    /// another connection or implicitly advancing the rest of the reactor.
    ///
    /// - Parameter d: The daemon.
    static void settleClosures(Daemon& d) { d.settleOwedClosures(); }

    /// Counts actual model roots rather than their published records.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The number of hosted workspaces.
    static std::size_t heldWorkspaces(const Daemon& d) { return d.m_workspaces.size(); }

    /// Drops a client and removes it, as the loop's reap pass does: every
    /// client after it moves down one index.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    static void reap(Daemon& d, std::size_t index) { d.reapClient(index); }

    /// Gives an in-memory daemon explicit serving metadata only during ingress.
    /// A started daemon keeps its real socket path; the temporary path is
    /// restored before teardown can treat it as an owned filesystem target.
    /// It lies in a scratch directory removed with whatever a session created
    /// beside the socket, such as its clipboard helpers. The directory is under
    /// /tmp rather than TMPDIR, which some cases point at a missing directory.
    struct FixtureServingMetadata {
        explicit FixtureServingMetadata(Daemon& daemon)
            : daemon(daemon)
            , wasEmpty(daemon.m_socketPath.empty())
        {
            if (!wasEmpty)
                return;
            std::string pattern = "/tmp/ubiquitty-screen-plane.XXXXXX";
            if (::mkdtemp(pattern.data()) == nullptr)
                return;
            scratchDirectory = std::move(pattern);
            daemon.m_socketPath = scratchDirectory + "/screen-plane-fixture.sock";
        }
        ~FixtureServingMetadata()
        {
            if (wasEmpty)
                daemon.m_socketPath.clear();
            std::error_code error;
            if (!scratchDirectory.empty())
                std::filesystem::remove_all(scratchDirectory, error);
        }
        FixtureServingMetadata(const FixtureServingMetadata&) = delete;
        FixtureServingMetadata& operator=(const FixtureServingMetadata&) = delete;
        Daemon& daemon;
        bool wasEmpty;
        std::string scratchDirectory;
    };

    /// Hands the daemon direct canonical bytes through its real selector.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - bytes: The client's Sync stream bytes.
    static void receive(Daemon& d, std::size_t index, std::span<const std::uint8_t> bytes)
    {
        const FixtureServingMetadata metadata(d);
        d.feedClientFrames(index, std::as_bytes(bytes));
    }

    /// Notes a client's read progress, as a read pass does after its frames.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    static void noteInbound(Daemon& d, std::size_t index) { d.noteInboundDecoder(index); }

    /// Whether unfinished inbound state currently owns a silence deadline.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client whose inbound state is inspected.
    /// - Returns: Whether that client has an armed inbound deadline.
    static bool hasInboundDeadline(const Daemon& d, std::size_t index) { return d.m_clients[index].partialFrameDeadlineMilliseconds != 0; }

    /// Advances an already-armed inbound deadline without waiting on wall time.
    ///
    /// - Parameters:
    ///   - d: The daemon whose budget enforcement runs.
    ///   - index: The client whose armed deadline is expired.
    static void expireInboundDeadline(Daemon& d, std::size_t index)
    {
        if (hasInboundDeadline(d, index))
            d.m_clients[index].partialFrameDeadlineMilliseconds = 1;
        d.enforceInboundBudget();
    }

    /// Runs the daemon's inbound budget pass.
    ///
    /// - Parameter d: The daemon.
    static void enforceInboundBudget(Daemon& d) { d.enforceInboundBudget(); }

    /// Whether the daemon dropped a client.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: True once it is dropped.
    static bool isDropped(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }

    /// The bytes the daemon holds for what a client has not finished sending.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: The bytes, as the inbound budget counts them.
    static std::size_t heldInbound(const Daemon& d, std::size_t index) { return Daemon::heldInboundBytes(d.m_clients[index]); }

    /// Asks the coherence rule whether a session's frame may be taken.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    ///   - at: The daemon's clock in microseconds.
    /// - Returns: The frame's class when it may be taken, or nothing.
    static std::optional<Runtime::FrameClass> mayPublish(Daemon& d, Daemon::Session& s, std::uint64_t at) { return d.mayPublishFrame(s, at); }

    /// Publishes a session's frame, whatever the rule would say.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    ///   - frameClass: The class the frame carries.
    static void publish(Daemon& d, Daemon::Session& s, Runtime::FrameClass frameClass = Runtime::FrameClass::Settled) { d.publishFrame(s, frameClass); }

    /// Runs the loop's screen step: a session owing a frame holds none, and
    /// every client's connection is serviced.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - at: The daemon's clock in microseconds.
    static void service(Daemon& d, std::uint64_t at = now())
    {
        d.settleOwedClosures();
        d.syncStructures();
        d.serviceRichTextMeasurements(at);
        for (auto& session : d.m_sessions) {
            if (session->isFrameDue && session->screenSource)
                session->screenSource->noteTerminalChanged();
        }
        for (std::size_t index = 0; index < d.m_clients.size(); ++index)
            d.serviceSyncConnection(index, at);
    }

    /// Reads the next screen or measurement timer without advancing the loop.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - at: The daemon's monotonic clock, in microseconds.
    /// - Returns: The earliest outstanding service deadline, if any.
    static std::optional<std::uint64_t> nextService(const Daemon& d, std::uint64_t at) { return d.nextScreenServiceMicroseconds(at); }

    /// Hands the daemon a run of canonical frames, as one read pass of
    /// its socket dispatches them.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - frames: Whole or partial canonical frames.
    static void handleFrames(Daemon& d, std::size_t index, std::span<const std::uint8_t> frames)
    {
        receive(d, index, frames);
    }

    /// Spawns a real session through the daemon's own path.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - columns: Its columns.
    ///   - rows: Its rows.
    ///   - command: What the PTY runs.
    /// - Returns: The session, or null when it could not be made.
    static Daemon::Session* spawnSession(Daemon& d, int columns, int rows, std::span<const std::string> command) { return d.createSession(columns, rows, command); }

    /// Runs one drain pass over a session's PTY, as the loop does when it is
    /// readable.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    static void servicePTY(Daemon& d, Daemon::Session& s) { d.servicePTY(s); }

    /// Gives a session's terminal the clients every daemon path gives one,
    /// its proportional measurer among them.
    ///
    /// - Parameter s: The session.
    static void installTerminalClients(Daemon::Session& s) { Daemon::installTerminalClients(s); }

    /// Runs the loop's settle check: input written `E` ago is settled if the
    /// program's output is quiet.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - at: The daemon's clock in microseconds, read before the check.
    static void settleDueInput(Daemon& d, std::uint64_t at = now()) { d.settleDueInput(at); }

    /// Sends a client the state a pane starts from, as an attach does once
    /// the client is subscribed to the pane's session.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client, subscribed to the session, with a transport.
    ///   - s: The session.
    static void seedPane(Daemon& d, std::size_t index, Daemon::Session& s)
    {
        if (d.m_clients[index].subscriptionFor(s.id))
            d.seedPaneState(s);
    }

    /// Unbinds and removes one subscription through the daemon's withdrawal path.
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client whose subscription leaves.
    ///   - s: The pane that remains alive.
    /// - Returns: Whether that subscription existed.
    static bool unbindPane(Daemon& d, std::size_t index, Daemon::Session& s)
    {
        auto& client = d.m_clients[index];
        const auto* subscription = client.subscriptionFor(s.id);
        if (!subscription)
            return false;
        d.unbindScreenPane(client, *subscription);
        std::erase_if(client.paneSubscriptions, [&](const auto& pane) { return pane.sessionID == s.id; });
        return true;
    }

    /// Unbinds through Sync without the daemon's explicit withdrawal callback.
    ///
    /// Checkpoint admission unbinds inside connection service; the daemon
    /// observes the lost binding when it next examines that client's offers.
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client whose connection unbinds the pane.
    ///   - s: The pane whose checkpoint cannot be sent.
    /// - Returns: Whether the subscription and connection exist.
    static bool unbindCheckpointPane(Daemon& d, std::size_t index, Daemon::Session& s)
    {
        auto& client = d.m_clients[index];
        const auto* subscription = client.subscriptionFor(s.id);
        if (!subscription || !client.syncConnection)
            return false;
        client.syncConnection->unbindPane(subscription->screenHandle, std::to_underlying(Protocol::Sync::PaneUnbindReason::CheckpointTooLarge));
        return true;
    }

    /// Subscribes a client again through the daemon's normal binding path.
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client whose pane subscription is absent.
    ///   - s: The pane to bind again.
    /// - Returns: The new handle, or zero while binding is deferred.
    static std::uint32_t rebindPane(Daemon& d, std::size_t index, Daemon::Session& s)
    {
        auto& client = d.m_clients[index];
        if (!client.subscriptionFor(s.id))
            client.paneSubscriptions.push_back({ .sessionID = s.id });
        d.bindScreenPanes(index);
        return client.subscriptionFor(s.id)->screenHandle;
    }

    /// Asks for a session's records as a frame does, at a time of the
    /// test's: read now inside the budget, owed otherwise.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    ///   - at: The loop's clock, in milliseconds.
    static void noteRecordsDue(Daemon& d, Daemon::Session& s, std::uint64_t at) { d.noteRecordsDue(s, at); }

    /// Runs the loop's step that reads the records a session is owed.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    ///   - at: The loop's clock, in milliseconds.
    static void serveOwedRecords(Daemon& d, Daemon::Session& s, std::uint64_t at) { d.serveOwedRecords(s, at); }

    /// Reads a session's records from its terminal, whatever the budget says.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    ///   - at: The loop's clock, in milliseconds.
    static void publishRecords(Daemon& d, Daemon::Session& s, std::uint64_t at) { d.publishRecords(s, at); }

    /// Runs the loop's step that makes a foreground probe a session is owed.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    ///   - at: The loop's clock, in milliseconds.
    static void serveOwedForegroundProbe(Daemon& d, Daemon::Session& s, std::uint64_t at) { d.serveOwedForegroundProbe(s, at); }

    /// Replaces the wall clock the daemon's activity stamps read.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - clock: The clock, in milliseconds since the Unix epoch.
    static void setWallClock(Daemon& d, std::uint64_t (*clock)()) { d.m_wallClockMilliseconds = clock; }

    /// Runs the loop's step at the end of a read pass that stamps whoever
    /// typed during it.
    ///
    /// - Parameter d: The daemon.
    static void stampTypedInput(Daemon& d)
    {
        for (auto& session : d.m_sessions)
            d.stampTypedInput(*session);
    }

    /// Runs the loop's step at the end of a read pass that clears the
    /// attention a person's focus during it saw.
    ///
    /// - Parameter d: The daemon.
    static void clearAttentionAPersonSaw(Daemon& d)
    {
        for (auto& session : d.m_sessions)
            d.clearAttentionAPersonSaw(*session);
    }

    /// Gives a daemon that was never started the shell and grid a restored
    /// pane is created with.
    ///
    /// - Parameter d: The daemon.
    static void setRestoreDefaults(Daemon& d)
    {
        d.m_shellPath = "/bin/sh";
        d.m_columns = 80;
        d.m_rows = 24;
    }

    /// Names the workspace a session is the root of, as a HELLO under a name
    /// does, and makes it one the host records.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The root session.
    ///   - name: The workspace's name.
    /// - Returns: The workspace's id.
    static std::uint32_t nameWorkspace(Daemon& d, Daemon::Session& s, std::string name)
    {
        s.workspaceName = std::move(name);
        return d.hostedWorkspaceFor(s).workspaceID;
    }

    /// The layout of the workspace holding a session.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: A session of the workspace.
    /// - Returns: The layout, the daemon's own.
    static Ubiquitty::Session::Layout::Workspace& layout(Daemon& d, Daemon::Session& s) { return d.hostedWorkspaceFor(s).layout; }

    /// Labels an object the way a label verb does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - id: The object.
    ///   - label: The label; empty clears it.
    /// - Returns: Whether the label was taken.
    static bool setLabel(Daemon& d, Ubiquitty::Session::Identity::ObjectID id, std::string_view label) { return d.setLabel(id, label); }

    /// Tags an object the way a tag verb would.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - id: The object.
    ///   - names: Its tags; none clears them.
    /// - Returns: Whether the tags were taken.
    static bool setTags(Daemon& d, Ubiquitty::Session::Identity::ObjectID id, std::span<const std::string> names) { return d.setTags(id, names); }

    /// An object's tags in the daemon's book.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - id: The object.
    /// - Returns: The tags in order, none when it has none.
    static std::vector<std::string> tags(const Daemon& d, Ubiquitty::Session::Identity::ObjectID id)
    {
        const auto held = d.m_tags.tags(id);
        return { held.begin(), held.end() };
    }

    /// An object's label in the daemon's book.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - id: The object.
    /// - Returns: The label, empty when it has none.
    static std::string label(const Daemon& d, Ubiquitty::Session::Identity::ObjectID id) { return std::string(d.m_labels.label(id)); }

    /// Reports whether the daemon counts an identity as a live object, which
    /// is what keeps its label when a full book is pruned.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - id: The identity.
    /// - Returns: Whether it names something the daemon holds.
    static bool isLiveObject(const Daemon& d, Ubiquitty::Session::Identity::ObjectID id) { return d.isLiveObject(id); }

    /// Describes the workspace holding a session as the record the host
    /// writes.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: A session of the workspace.
    /// - Returns: The record, or nothing when it holds too many panes.
    static std::optional<Ubiquitty::Session::HostLayoutRecord> captureLayoutRecord(Daemon& d, Daemon::Session& s) { return d.captureLayoutRecord(d.hostedWorkspaceFor(s)); }

    /// Takes in decoded records as a starting host does and restores the
    /// first, as attaching to it by name does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - records: The records a file held.
    /// - Returns: A session of the restored workspace, or null.
    static Daemon::Session* restoreFirstRecord(Daemon& d, std::vector<Ubiquitty::Session::HostLayoutRecord> records)
    {
        d.admitRestorableRecords(std::move(records));
        if (d.m_restorableWorkspaces.empty())
            return nullptr;
        return d.restoreWorkspace(d.m_restorableWorkspaces.front().workspaceID);
    }

    /// The id of the workspace holding a session.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: A session of the workspace.
    /// - Returns: The id.
    static std::uint32_t workspaceID(Daemon& d, Daemon::Session& s) { return d.hostedWorkspaceFor(s).workspaceID; }

    /// Splits the focused pane of a session's workspace and puts another
    /// session in the new pane, as a split verb does once it has minted one.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - holder: A session of the workspace.
    ///   - added: The session the new pane holds.
    ///   - orientation: The split's divider.
    static void splitInto(Daemon& d, Daemon::Session& holder, Daemon::Session& added, Ubiquitty::Panes::Orientation orientation)
    {
        Daemon::HostedWorkspace& workspace = d.hostedWorkspaceFor(holder);
        const auto surface = workspace.layout.activeSurface();
        const auto pane = workspace.layout.splitFocusedPane(surface, orientation, 0.5f);
        workspace.paneSessions.emplace_back(pane, added.id);
    }

    /// Opens a tab in the active window of a session's workspace and binds
    /// its one pane to another session, as a verb that mints a tab does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - holder: A session of the workspace.
    ///   - added: The session that draws into the new tab's pane.
    /// - Returns: The new tab.
    static Ubiquitty::Session::Layout::TabID tabInto(Daemon& d, Daemon::Session& holder, Daemon::Session& added)
    {
        Daemon::HostedWorkspace& workspace = d.hostedWorkspaceFor(holder);
        const auto surface = workspace.layout.activeSurface();
        const auto tab = workspace.layout.addTab(surface);
        workspace.paneSessions.emplace_back(workspace.layout.focusedPane(surface), added.id);
        return tab;
    }

    /// Opens a window in a session's workspace and binds its one pane to
    /// another session, as a verb that mints a window does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - holder: A session of the workspace.
    ///   - added: The session that draws into the new window's pane.
    /// - Returns: The new window.
    static Ubiquitty::Session::Layout::SurfaceID windowInto(Daemon& d, Daemon::Session& holder, Daemon::Session& added)
    {
        Daemon::HostedWorkspace& workspace = d.hostedWorkspaceFor(holder);
        const auto surface = workspace.layout.addSurface();
        workspace.paneSessions.emplace_back(workspace.layout.focusedPane(surface), added.id);
        return surface;
    }

    /// Makes a client the one the daemon verifies as its own user's, at this
    /// machine: the peer binary it trusts, with no agent's label.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    static void markTrustedLocal(Daemon& d, std::size_t index) { seedTrustedLocalPeer(d.m_clients[index]); }

    /// Binds a client to the workspace holding a session, as attaching to
    /// the workspace does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - s: A session of the workspace.
    static void bindWorkspace(Daemon& d, std::size_t index, Daemon::Session& s) { d.m_clients[index].workspaceID = d.hostedWorkspaceFor(s).workspaceID; }

    /// Gives the daemon a file to record its named workspaces in, as starting
    /// on a socket does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - path: The file.
    static void setLayoutRecordPath(Daemon& d, std::string path) { d.m_layoutRecordPath = std::move(path); }

    /// Whether the daemon owes its record file a write.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: Whether a write is scheduled.
    static bool isLayoutRecordWriteOwed(const Daemon& d) { return d.m_layoutRecordWrite.isOwed(); }

    /// When the daemon's next write of its record file is due.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The daemon's monotonic clock in milliseconds; zero while
    ///   no write is owed.
    static std::uint64_t layoutRecordWriteDueMilliseconds(const Daemon& d) { return d.m_layoutRecordWrite.dueMilliseconds; }

    /// Owes the record file a write for a change to the workspace holding a
    /// session, as a rename or a split does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: A session of the workspace.
    static void oweLayoutRecordWrite(Daemon& d, Daemon::Session& s) { d.scheduleLayoutRecordWrite(d.hostedWorkspaceFor(s)); }

    /// Writes a client's input to a session's program, through the daemon's
    /// own write of input a client typed.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    ///   - bytes: Keystrokes.
    static void typeInput(Daemon& d, Daemon::Session& s, std::string_view bytes)
    {
        (void)d.writeRemoteInput(s, {}, {}, { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
    }

    /// Writes the record file now, as the loop does when a scheduled write
    /// falls due.
    ///
    /// - Parameter d: The daemon.
    static void writeLayoutRecords(Daemon& d) { (void)d.writeLayoutRecords(); }

    /// Selects and reads a fixture-owned passive event journal through the real startup path.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - path: An exact path under the fixture's temporary directory.
    static void loadBackgroundEventFile(Daemon& d, std::string path)
    {
        d.m_backgroundEventPath = std::move(path);
        d.loadBackgroundEvents();
    }

    /// Ends a session as the loop's reap does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - sessionID: The session.
    static void destroySession(Daemon& d, std::uint32_t sessionID) { d.destroySession(sessionID); }

    /// Adds a workspace the host could restore, as a record read at start is.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - workspaceID: The id it is listed by.
    ///   - record: The record.
    static void addRestorable(Daemon& d, std::uint32_t workspaceID, Ubiquitty::Session::HostLayoutRecord record)
    {
        d.m_restorableWorkspaces.push_back({ workspaceID, std::move(record) });
        d.m_isStructureDirty = true;
    }

    /// How many workspaces the daemon has built a structure for.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The count, withdrawn ones not among them.
    static std::size_t structuresBuilt(const Daemon& d)
    {
        return static_cast<std::size_t>(std::ranges::count_if(d.m_structureRecords, [](const auto& entry) { return entry.second.record != nullptr; }));
    }

    /// Publishes the actual tree and reads a named window's workspace revision.
    /// - Parameters:
    ///   - d: The fixture daemon.
    ///   - window: The host-owned window number.
    /// - Returns: Its current revision, or zero for an absent window.
    static std::uint64_t windowStructureRevision(Daemon& d, std::uint32_t window)
    {
        const auto located = d.locateObject({ Ubiquitty::Session::Identity::ObjectKind::Window, window });
        if (!located)
            return 0;
        d.m_isStructureDirty = true;
        d.syncStructures();
        const auto found = d.m_structureRecords.find(located->workspace->workspaceID);
        return found == d.m_structureRecords.end() ? 0 : found->second.revision;
    }

    /// Publishes the actual tree containing a host-owned pane.
    /// - Parameters:
    ///   - d: The fixture daemon.
    ///   - pane: The host-owned pane number.
    /// - Returns: Its workspace revision, or zero for an absent pane.
    static std::uint64_t paneStructureRevision(Daemon& d, std::uint32_t pane)
    {
        const auto located = d.locateObject({ Ubiquitty::Session::Identity::ObjectKind::Pane, pane });
        return located ? windowStructureRevision(d, std::to_underlying(located->surface)) : 0;
    }

    /// The pane a session draws into, as its workspace's layout names it.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    /// - Returns: The layout's handle for the pane.
    static std::uint32_t layoutPane(Daemon& d, Daemon::Session& s) { return std::to_underlying(d.hostedWorkspaceFor(s).paneFor(s.id)); }

    /// The session one pane of a session's workspace shows.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: A session of the workspace.
    ///   - pane: The pane.
    /// - Returns: The session's id, or 0 when the pane shows none.
    static std::uint32_t workspaceSessionFor(Daemon& d, Daemon::Session& s, Ubiquitty::Session::Layout::PaneID pane) { return d.hostedWorkspaceFor(s).sessionFor(pane); }

    /// Runs the loop's step at the end of a read pass: each session's newest
    /// RESIZE is applied.
    ///
    /// - Parameter d: The daemon.
    static void endReadPass(Daemon& d)
    {
        for (auto& session : d.m_sessions)
            d.applyPendingGeometry(*session);
    }

    /// Takes the bytes the daemon queued for a client, as its transport would.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: The canonical stream bytes, whole or partial.
    static std::vector<std::uint8_t> takeOutput(Daemon& d, std::size_t index)
    {
        Daemon::Client& client = d.m_clients[index];
        std::vector<std::uint8_t> bytes(client.outputBuffer.begin() + static_cast<std::ptrdiff_t>(client.outputConsumed), client.outputBuffer.end());
        client.clearPendingOutput();
        return bytes;
    }

    /// Carries a client's screen plane both ways, servicing the daemon, until
    /// neither end has more to say.
    ///
    /// A client holds an `Ack` back for a moment, and the host sends no more
    /// of a bulk answer until it has it. A round in which nothing moved is
    /// therefore not the end while the client owes one: the wait is for its
    /// timer, and stopping there leaves the rest of the answer with the host
    /// on any machine quick enough to come round before the timer does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - client: The client's end.
    static void deliver(Daemon& d, std::size_t index, UbiquittyTest::ScreenClient& client)
    {
        // A client that has not greeted has no connection to carry: the
        // daemon's pass still runs, as it does for every other client.
        if (!client.hasStarted()) {
            service(d);
            return;
        }
        for (int round = 0; round < 1024; ++round) {
            const std::uint64_t at = now();
            service(d, at);
            const std::vector<std::uint8_t> down = takeOutput(d, index);
            client.read(down, at);
            const std::vector<std::uint8_t> up = client.takeStream(at);
            if (!up.empty())
                receive(d, index, up);
            if (!down.empty() || !up.empty())
                continue;
            const std::optional<std::uint64_t> ackDue = client.connection().nextTickMicroseconds();
            if (!ackDue)
                return;
            if (const std::uint64_t clock = now(); *ackDue > clock)
                std::this_thread::sleep_for(std::chrono::microseconds(*ackDue - clock));
        }
    }

    /// How many events of a kind the daemon's log holds.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - kind: The kind.
    /// - Returns: The count among the events still in the log.
    static std::size_t eventCount(const Daemon& d, Ubiquitty::Session::EventKind kind)
    {
        const Ubiquitty::Session::EventLog::Backlog backlog = d.m_events.since(0);
        return static_cast<std::size_t>(std::ranges::count(backlog.events, kind, &Ubiquitty::Session::Event::kind));
    }

    /// Greets a client's screen connection and carries the attach. A client
    /// attached with no transport is given the client's carrier first.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    ///   - client: The client's end.
    static void greet(Daemon& d, std::size_t index, UbiquittyTest::ScreenClient& client)
    {
        const auto request = selectedRequest(d, index);
        if (!request || !client.start(*request))
            return;
        if (auto& transport = d.m_clients[index].fileDescriptor; !transport)
            transport = client.takeCarrier();
        receive(d, index, client.takeStream(now()));
        deliver(d, index, client);
    }

    /// Names only the target explicitly selected by fixture setup.
    /// Zero selects Control, never permission to create a replacement session.
    /// - Parameters:
    ///   - d: The daemon owning the selected session, workspace, and instance.
    ///   - index: The physical client whose setup selected that target.
    /// - Returns: The exact request, or nothing for an absent physical client.
    static std::optional<Protocol::Sync::AdmissionRequest> selectedRequest(Daemon& d, std::size_t index)
    {
        if (index >= d.m_clients.size())
            return std::nullopt;
        const auto& client = d.m_clients[index];
        Protocol::Sync::AdmissionRequest request;
        request.expectedDaemonInstanceID = d.m_daemonInstanceID;
        request.agentLabel = client.agentLabel;
        request.servesClipboard = client.intents.contains(Protocol::Sync::HelloIntent::ServesClipboard);
        if (!client.sessionID) {
            request.target = client.intents.contains(Protocol::Sync::HelloIntent::RequestsManagement)
                ? Protocol::Sync::AdmissionTarget::Management
                : Protocol::Sync::AdmissionTarget::Control;
            return request;
        }
        request.target = client.workspaceID ? Protocol::Sync::AdmissionTarget::Workspace : Protocol::Sync::AdmissionTarget::Session;
        request.sessionID = client.sessionID;
        request.workspaceID = client.workspaceID;
        request.isAttachOnly = true;
        return request;
    }

    /// Completes the actual host admission for an existing fixture transport.
    /// No compatibility frame or alternate consent state is manufactured.
    /// - Parameters:
    ///   - d: The fixture daemon.
    ///   - index: Its authenticated client.
    ///   - kind: The kind declared by this fixture client.
    ///   - role: The requested role, still capped by production authority.
    static void greetExisting(Daemon& d, std::size_t index, Protocol::Sync::ClientKind kind, Protocol::Sync::Role role,
        Protocol::Sync::ClientCapabilities capabilities = {})
    {
        const auto request = selectedRequest(d, index);
        if (!request)
            return;
        Protocol::Sync::Greet greeting;
        greeting.wireVersion = Protocol::Sync::wireVersion;
        greeting.appVersion = Protocol::Sync::appVersion();
        greeting.clientInstance = { .low = d.m_clients[index].clientID, .high = 0x636f6e73656e74 };
        greeting.clientKind = kind;
        greeting.requestedRole = role;
        greeting.request = *request;
        greeting.capabilities = capabilities;
        std::vector<std::uint8_t> body;
        Protocol::Sync::encode(body, greeting);
        std::vector<std::uint8_t> stream;
        if (body.empty() || !Protocol::Sync::decodeGreet(body)
            || !Protocol::Sync::appendFrame(stream, Protocol::Sync::FrameKind::Greet, 0, body))
            return;
        receive(d, index, stream);
    }

    /// Finds a client's screen connection.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: The connection, or null before the client greets.
    static const Protocol::Sync::HostConnection* connection(const Daemon& d, std::size_t index) { return d.m_clients[index].syncConnection.get(); }

    /// Whether the calling physical connection offered its own clipboard.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The physical connection whose offer is inspected.
    /// - Returns: Its current clipboard serving intent.
    static bool servesClipboard(const Daemon& d, std::size_t index) { return d.m_clients[index].intents.contains(Protocol::Sync::HelloIntent::ServesClipboard); }

    /// Supersedes a connection while its physical carrier awaits reap.
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client whose pending Bye may still be draining.
    static void supersedeScreen(Daemon& d, std::size_t index)
    {
        d.m_clients[index].syncConnection->supersede();
        d.armSessionExitDeadline(d.m_clients[index], 1);
    }

    /// Writes what is queued for a client to its socket, as the loop's flush
    /// at the end of a pass does.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    static void flush(Daemon& d, std::size_t index) { d.flushClient(index); }

    /// Counts the writes a client's output has taken.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: The writes.
    static std::uint64_t outputWrites(const Daemon& d, std::size_t index) { return d.m_clients[index].outputWrites; }

    /// Counts the bytes queued for a client and not yet sent.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: The bytes.
    static std::size_t pending(const Daemon& d, std::size_t index) { return d.outboundPendingBytes(d.m_clients[index]); }

    /// Reports whether a client was marked for the drop.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: True when the daemon will drop it.
    static bool didOverflow(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }

    /// Whether the daemon still holds a session.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - sessionID: The session.
    /// - Returns: Whether it is there.
    static bool hasSession(Daemon& d, std::uint32_t sessionID) { return d.findSession(sessionID) != nullptr; }

    /// Finds a live session by its id.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - sessionID: The session's id.
    /// - Returns: The session, or null when none has that id.
    static Daemon::Session* findSession(Daemon& d, std::uint32_t sessionID) { return d.findSession(sessionID); }

    /// Borrows a created session's terminal to inspect its inherited theme.
    /// - Parameters:
    ///   - d: The daemon.
    ///   - sessionID: The identity returned by a successful tree call.
    /// - Returns: Its terminal, or null after that session ends.
    static const Terminal* sessionTerminal(Daemon& d, std::uint32_t sessionID)
    {
        const auto* session = d.findSession(sessionID);
        return session ? session->term.get() : nullptr;
    }

    /// Counts the host's live sessions, including newly created panes.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: The number of sessions it holds.
    static std::size_t sessionCount(const Daemon& d) { return d.m_sessions.size(); }

    /// Exhausts the session-id allocator to exercise creation rollback without
    /// allocating a PTY or changing the workspace the request names.
    ///
    /// - Parameter d: The daemon.
    static void exhaustSessionIDs(Daemon& d) { d.m_nextSessionID = 0; }

    /// Reads a caller's standing grant without lending it another identity.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The requester connection.
    ///   - sessionID: The created session.
    /// - Returns: Whether this requester may attach under a standing grant.
    static bool hasStandingGrant(Daemon& d, std::size_t index, std::uint32_t sessionID)
    {
        const auto* session = d.findSession(sessionID);
        return index < d.m_clients.size() && session && d.agentHasStandingGrant(d.m_clients[index], *session);
    }

    /// Whether the loop leaves a client's socket unread.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - index: The client.
    /// - Returns: Whether its input is held.
    static bool isInputHeld(const Daemon& d, std::size_t index) { return d.isInputHeld(d.m_clients[index]); }

    /// How much more input the host's panes take unwritten, all together.
    ///
    /// - Parameter d: The daemon.
    /// - Returns: What is left of the host's bound.
    static std::size_t heldInputRoom(const Daemon& d) { return d.heldInputRoom(); }

    /// Reports the output below which the daemon encodes for a client.
    ///
    /// - Returns: One capped canonical frame, including its envelope header.
    static std::size_t lowWaterBytes() { return Protocol::Sync::maxEnvelopeHeaderBytes + Protocol::Sync::maxFrameBodyBytes; }
};

} // namespace Ubiquitty::Host
