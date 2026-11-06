// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/control_surface.cpp — the daemon's control surface over a live
// socket: a typed directory enumerates what exists, obsolete text-read and
// termination layouts are refused, Normal Bye detaches without
// destroying, and session.terminate from the session's own attached client
// takes the attached-requester exemption through the consent gate.
//
// The named-session half of the same surface follows: a Greet naming a
// workspace attaches to the session bearing that name or creates it, the list
// carries each session's name, workspace and attendee summary, and a session
// created ends-when-detached goes away with the client that made it.
//
// Drives the REAL ubiquitty-host daemon (UBIQUITTY_REQUIRE_TOKEN=1) over the
// unix socket, with one full-scope token. Exit 0 on success. argv[1] = the
// built ubiquitty-host binary.
//

#include "Core/Crypto/SHA256.h"
#include "Core/Protocol/Control/HostAdministration.h"
#include "Core/Protocol/Control/SessionTermination.h"
#include "HostConnection.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/HostSocketName.h"
#include "TerminationClient.h"

#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace Sync = Ubiquitty::Protocol::Sync;
namespace Control = Ubiquitty::Protocol::Control;

constexpr std::uint8_t terminateSessionTag = 0x06;
constexpr std::uint8_t readRegionTag = 0x07;
constexpr std::uint8_t readLastCommandTag = 0x08;
constexpr std::uint8_t fullScope = 0x1F; // observe|drive|control|regionRead|newSession

using namespace Ubiquitty::HostTest;

std::uint32_t managementClientFor(const Sync::ManagementReply& reply, const std::string& identity)
{
    for (const auto& client : reply.clients)
        if (client.identity == identity)
            return client.clientID;
    return 0;
}

bool fail(const char* msg)
{
    std::fprintf(stderr, "control_surface: FAIL — %s\n", msg);
    return false;
}

bool replaceStoreBytes(const std::string& path, const std::string& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return !file.fail();
}

std::optional<std::string> storeBytes(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::string bytes { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    return file.bad() ? std::nullopt : std::optional { std::move(bytes) };
}

void putU64(std::string& b, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// Refusal fixtures only. These old five-byte headers are not current APIs;
// their low kind bytes may name unrelated canonical kinds with other scopes.
bool sendObsoleteLayout(Connection& connection, std::uint8_t tag, const std::string& body)
{
    std::string bytes(1, static_cast<char>(tag));
    putU32(bytes, static_cast<std::uint32_t>(body.size()));
    bytes += body;
    return writeToDaemon(connection.fileDescriptor, bytes);
}

bool sendReadRegion(Connection& c, std::uint8_t tag, std::uint32_t requestID, std::int64_t start, std::int64_t end)
{
    std::string body;
    putU32(body, requestID);
    body.push_back('\0'); // styled = 0
    putU64(body, static_cast<std::uint64_t>(start));
    putU64(body, static_cast<std::uint64_t>(end));
    return sendObsoleteLayout(c, tag, body);
}

bool sendAdmission(Connection& connection, Sync::AdmissionRequest request, Sync::ID128 instance, const std::string& token)
{
    Sync::Greet greeting;
    greeting.wireVersion = Sync::wireVersion;
    greeting.appVersion = Sync::appVersion();
    greeting.clientInstance = instance;
    greeting.clientKind = token.empty() ? Sync::ClientKind::Human : Sync::ClientKind::Agent;
    greeting.requestedRole = token.empty() ? Sync::Role::Owner : Sync::Role::Agent;
    greeting.request = std::move(request);
    greeting.facts.viewportRows = 24;
    greeting.facts.viewportColumns = 80;
    greeting.historyCacheLines = Sync::defaultHistoryCacheLines;
    greeting.backfillDepthLines = Sync::defaultBackfillDepthLines;
    if (!token.empty())
        greeting.credential.emplace(token.begin(), token.end());
    return sendGreeting(connection, std::move(greeting));
}

bool awaitAdmission(Connection& connection, const std::string& socketLeaf)
{
    std::string body;
    return waitFor(connection, Sync::FrameKind::Welcome, body, 3000) && connection.welcome && !connection.hasFailed
        && connection.admission == FixtureAdmission::Granted
        && connection.welcome->admission == Sync::AttachmentAdmission::Granted
        && connection.welcome->selection.socketName == socketLeaf && connection.welcome->selection.daemonInstanceID;
}

bool leave(Connection& connection)
{
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::Normal, .detail = {}, .peer = std::nullopt });
    return sendFrame(connection, Sync::FrameKind::Bye, { reinterpret_cast<const char*>(body.data()), body.size() });
}

bool waitForBye(Connection& connection, Sync::ByeReason reason)
{
    std::string body;
    if (!waitFor(connection, Sync::FrameKind::Bye, body, 3000))
        return false;
    const auto bye = Sync::decodeBye({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    return !connection.hasFailed && bye && bye->reason == reason;
}

bool waitForEndedPane(Connection& connection, std::uint32_t handle)
{
    std::string body;
    if (!handle || !waitFor(connection, Sync::FrameKind::PaneUnbind, body, 3000))
        return false;
    const auto unbind = Sync::decodePaneUnbind({ reinterpret_cast<const std::uint8_t*>(body.data()), body.size() });
    return !connection.hasFailed && unbind && unbind->handle == handle && !unbind->reason;
}

std::optional<Sync::ManagementReply> administration(Connection& connection, const Sync::ManagementRequest& request, std::string* responseText = nullptr)
{
    const auto id = "surface.management." + std::to_string(request.requestID);
    const auto requestText = Control::hostAdministrationRequest(request, id);
    if (requestText.empty() || connection.admission != FixtureAdmission::Granted)
        return std::nullopt;
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Call { .text = requestText });
    if (!sendFrame(connection, Sync::FrameKind::Call, { reinterpret_cast<const char*>(body.data()), body.size() }))
        return std::nullopt;
    std::string received;
    if (!waitFor(connection, Sync::FrameKind::Result, received, 3000))
        return std::nullopt;
    const auto result = Sync::decodeResult({ reinterpret_cast<const std::uint8_t*>(received.data()), received.size() });
    // This fixture's bounded initial inventory and mutations fit inline.
    // A partial bulk head or another correlation cannot satisfy the call.
    if (!result || result->transfer || connection.hasFailed)
        return std::nullopt;
    const auto reply = Control::readHostAdministrationResult(result->text, id, request);
    if (reply && responseText)
        *responseText = result->text;
    return reply;
}

// The local row adapter is projected from typed structures and pane facts.
using ListedSession = Ubiquitty::Session::SessionRow;

// Ask one lister for a complete, revision-correlated typed snapshot.
bool listSessions(Connection& lister, std::uint32_t requestID, std::vector<ListedSession>& out)
{
    out.clear();
    const auto rows = readSessionDirectory(lister, requestID);
    if (!rows)
        return false;
    out = *rows;
    return true;
}

// An unbound pane is not a substitute for its operation's exact result.
bool terminateSession(Connection& connection, std::uint32_t sessionID, Ubiquitty::Protocol::Control::TerminationAuthority authority = Ubiquitty::Protocol::Control::TerminationAuthority::Control)
{
    TerminationClient client(connection);
    return client.send(sessionID, "fixture.terminate." + std::to_string(sessionID), authority)
        && client.wait() == Ubiquitty::Protocol::Control::TerminationOutcome::Terminated;
}

const ListedSession* entryFor(const std::vector<ListedSession>& sessions, std::uint32_t id)
{
    for (const ListedSession& entry : sessions)
        if (entry.id == id)
            return &entry;
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <ubiquitty-host>\n", argv[0]);
        return 2;
    }
    // A peer closed after a typed refusal can race a later write. That failed
    // write is the error under test, not a reason to end the fixture process.
    ::signal(SIGPIPE, SIG_IGN);
    char homeTemplate[] = "/tmp/ubiquitty-ctrlsurf.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return 1;
    std::string macHome = std::string(home) + "/.ubiquitty";
    const std::string socketLeaf = Ubiquitty::Host::socketLeafForBinary(argv[1]);
    std::string sockPath = macHome + "/" + socketLeaf;
    ::mkdir(macHome.c_str(), 0700);

    const std::string token = "surfacetokensurfacetokensurfacetoken";
    {
        std::string tokensPath = macHome + "/tokens";
        FILE* file = std::fopen(tokensPath.c_str(), "w");
        if (!file)
            return 1;
        std::fprintf(file, "%s %u 0 surface\n", Ubiquitty::Crypto::sha256Hex(token).c_str(), static_cast<unsigned>(fullScope));
        std::fclose(file);
    }

    pid_t daemon = ::fork();
    if (daemon < 0)
        return 1;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        ::setenv("UBIQUITTY_REQUIRE_TOKEN", "1", 1);
        // A shell that reads no rc file of the person running the test: its
        // start does not depend on what that person's profile does, and a
        // line typed before its first read waits in the terminal for it.
        ::setenv("SHELL", "/bin/sh", 1);
        ::setenv("ENV", "/dev/null", 1);
        char* args[] = { argv[1], const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"), const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(argv[1], args);
        _exit(127);
    }

    int rc = 1;
    std::string body;
    // Every role below that has to count as a person connects through the real
    // `ubiquitty-host --attach` relay: the human/agent split is an origin
    // question the daemon answers from the kernel-verified peer binary, so a
    // raw connection from this test binary is an agent whatever token it holds.
    // Each spawned relay is reaped at teardown.
    std::vector<pid_t> bridgeProcessIDs;
    auto connectAsHuman = [&]() -> int {
        LocalBridgeConnection bridge = connectDaemonAsLocalBridge(argv[1], macHome);
        if (bridge.bridgeProcessID > 0)
            bridgeProcessIDs.push_back(bridge.bridgeProcessID);
        return bridge.fileDescriptor;
    };
    std::uint64_t nextClientInstance = 1;
    const auto sendRequest = [&](Connection& connection, Sync::AdmissionRequest request, const std::string& credential = std::string()) {
        return sendAdmission(connection, std::move(request), { .low = nextClientInstance++, .high = 0x73757266616365 }, credential);
    };
    const auto admit = [&](Connection& connection, Sync::AdmissionRequest request, const std::string& credential = std::string()) {
        return sendRequest(connection, std::move(request), credential) && awaitAdmission(connection, socketLeaf);
    };
    do {
        if (!waitForDaemonReady(sockPath)) {
            fail("daemon never accepted a connection");
            break;
        }

        // A native, unbound receipt identifies this daemon before a request
        // names an existing session. No targeted Welcome supplies its own pin.
        Connection control;
        control.fileDescriptor = connectAsHuman();
        if (control.fileDescriptor < 0 || !admit(control, { .target = Sync::AdmissionTarget::Control })
            || control.welcome->selection.target != Sync::AdmissionTarget::Control
            || control.welcome->selection.sessionID || control.welcome->selection.workspaceID) {
            fail("native Control receipt did not identify the fixture daemon");
            break;
        }
        const auto expectedDaemon = control.welcome->selection.daemonInstanceID;
        if (!leave(control)) {
            fail("native Control detach");
            break;
        }
        ::close(control.fileDescriptor);

        // --- Create a real session, holding region-read and control. ---
        Connection owner;
        owner.fileDescriptor = connectDaemon(sockPath);
        if (owner.fileDescriptor < 0) {
            fail("owner connect");
            break;
        }
        if (!admit(owner, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = expectedDaemon }, token)) {
            fail("session create (canonical Welcome)");
            break;
        }
        const std::uint32_t realSession = owner.welcome->selection.sessionID;

        Connection manager;
        manager.fileDescriptor = connectAsHuman();
        if (!admit(manager, { .target = Sync::AdmissionTarget::Management, .expectedDaemonInstanceID = expectedDaemon })) {
            fail("a native Management Greet is admitted");
            break;
        }
        const auto managementRequest = [&](std::uint32_t requestID) {
            return Sync::ManagementRequest { .requestID = requestID, .daemonInstanceID = expectedDaemon, .operation = Sync::ManagementOperation::List };
        };
        // What the host lists, read on a Control connection of its own and
        // closed after; nothing on failure.
        const auto listedNow = [&](std::uint32_t requestID) -> std::optional<std::vector<ListedSession>> {
            Connection lister;
            lister.fileDescriptor = connectDaemon(sockPath);
            std::vector<ListedSession> rows;
            const bool didList = lister.fileDescriptor >= 0
                && admit(lister, { .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = expectedDaemon }, token)
                && listSessions(lister, requestID, rows);
            if (lister.fileDescriptor >= 0)
                ::close(lister.fileDescriptor);
            return didList ? std::optional(std::move(rows)) : std::nullopt;
        };
        auto snapshot = administration(manager, managementRequest(71), &body);
        if (!snapshot || snapshot->status != Sync::ManagementStatus::Success
            || std::ranges::none_of(snapshot->clients, [&](const auto& client) { return client.sessionID == realSession; })
            || body.find(token) != std::string::npos) {
            fail("management listing must name the attachment on the live session without its bearer credential");
            break;
        }
        auto staleRequest = managementRequest(72);
        staleRequest.daemonInstanceID = expectedDaemon == 1 ? 2 : expectedDaemon ^ 1;
        const auto staleReply = administration(manager, staleRequest, &body);
        bool isJSON = false;
        const auto staleAnswer = Ubiquitty::Codec::JSON::parse(body, isJSON);
        const auto* staleError = staleAnswer.find("error");
        if (!staleReply || staleReply->status != Sync::ManagementStatus::Refused || !isJSON || !staleError
            || staleError->numberOr("code") != static_cast<int>(Control::ErrorCode::Forbidden)) {
            fail("management must reject a different daemon incarnation");
            break;
        }
        const auto issued = administration(manager, { .requestID = 73, .daemonInstanceID = expectedDaemon, .operation = Sync::ManagementOperation::IssueToken, .label = "management test", .scopes = { Sync::Scope::Control }, .expiryUnix = static_cast<std::uint64_t>(std::time(nullptr)) + 3600 });
        if (!issued || issued->status != Sync::ManagementStatus::Success || issued->issuedToken.size() != 64) {
            fail("explicit token issue must return its secret once");
            break;
        }
        const std::string issuedToken = issued->issuedToken;
        const auto issuedFingerprint = Ubiquitty::Crypto::sha256Hex(issuedToken);
        snapshot = administration(manager, managementRequest(74), &body);
        if (!snapshot || snapshot->status != Sync::ManagementStatus::Success || body.find(issuedToken) != std::string::npos
            || std::ranges::none_of(snapshot->tokens, [&](const auto& record) { return record.fingerprint == issuedFingerprint && record.scopes == Sync::ScopeSet { Sync::Scope::Control }; })) {
            fail("token listing must contain the fingerprint and exclude its secret");
            break;
        }
        Connection issuedClient;
        issuedClient.fileDescriptor = connectDaemon(sockPath);
        if (!admit(issuedClient, { .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = expectedDaemon }, issuedToken)) {
            fail("issued token must authenticate with its chosen control scope");
            break;
        }
        snapshot = administration(manager, managementRequest(77));
        if (!snapshot || snapshot->status != Sync::ManagementStatus::Success) {
            fail("management lists the authenticated client");
            break;
        }
        const auto issuedClientID = managementClientFor(*snapshot, "token:" + issuedFingerprint);
        if (!issuedClientID) {
            fail("management records the client's verified token identity");
            break;
        }
        const auto disconnected = administration(manager, { .requestID = 78, .daemonInstanceID = expectedDaemon, .operation = Sync::ManagementOperation::DisconnectClient, .clientID = issuedClientID });
        if (!disconnected || disconnected->status != Sync::ManagementStatus::Success || !waitForBye(issuedClient, Sync::ByeReason::Normal)) {
            fail("disconnect sends Normal Bye so a client does not automatically reconnect");
            break;
        }
        Connection reconnectedClient;
        reconnectedClient.fileDescriptor = connectDaemon(sockPath);
        if (!admit(reconnectedClient, { .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = expectedDaemon }, issuedToken)) {
            fail("disconnect alone preserves the credential for an explicit reconnect");
            break;
        }
        const auto revoked = administration(manager, { .requestID = 75, .daemonInstanceID = expectedDaemon, .operation = Sync::ManagementOperation::RevokeToken, .identity = issuedFingerprint });
        if (!revoked || revoked->status != Sync::ManagementStatus::Success || !waitForBye(reconnectedClient, Sync::ByeReason::TokenRevoked)) {
            fail("revocation must end a live token client's authorization");
            break;
        }
        const auto revokedAgain = administration(manager, { .requestID = 76, .daemonInstanceID = expectedDaemon, .operation = Sync::ManagementOperation::RevokeToken, .identity = issuedFingerprint });
        if (!revokedAgain || revokedAgain->status != Sync::ManagementStatus::NotFound) {
            fail("a stale token revocation must report not found");
            break;
        }
        Connection terminatedClient;
        terminatedClient.fileDescriptor = connectAsHuman();
        if (!admit(terminatedClient, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = expectedDaemon }) || !waitForPane(terminatedClient)) {
            fail("management termination fixture creates a session");
            break;
        }
        const auto terminationSessionID = terminatedClient.welcome->selection.sessionID;
        const auto terminationHandle = terminatedClient.paneHandle;
        // The removed layout led with tag 0x1A, a kind the canonical envelope
        // never assigned, so the reader stops at its first byte with
        // UnknownKind (Docs/sync.md §7.2) and reads none of the termination.
        Connection obsoleteManager;
        obsoleteManager.fileDescriptor = connectAsHuman();
        std::string obsoleteRequest;
        putU32(obsoleteRequest, 79);
        putU64(obsoleteRequest, expectedDaemon);
        obsoleteRequest.push_back(5); // the removed management termination layout
        putU32(obsoleteRequest, 0);
        putU32(obsoleteRequest, terminationSessionID);
        putU16(obsoleteRequest, 0);
        putU16(obsoleteRequest, 0);
        obsoleteRequest.push_back(0);
        putU64(obsoleteRequest, 0);
        if (!admit(obsoleteManager, { .target = Sync::AdmissionTarget::Management, .expectedDaemonInstanceID = expectedDaemon })
            || !sendObsoleteLayout(obsoleteManager, 0x1A, obsoleteRequest) || !waitForBye(obsoleteManager, Sync::ByeReason::UnknownKind)) {
            fail("the obsolete management layout's kind byte names no Sync kind, so it is refused rather than read as a termination");
            break;
        }
        ::close(obsoleteManager.fileDescriptor);
        const auto afterObsolete = listedNow(80);
        if (!afterObsolete || !entryFor(*afterObsolete, terminationSessionID)) {
            fail("malformed obsolete management bytes changed the target session");
            break;
        }
        if (!terminateSession(manager, terminationSessionID, Control::TerminationAuthority::Management) || !waitForEndedPane(terminatedClient, terminationHandle)) {
            fail("the management connection's typed termination ends its named session");
            break;
        }
        Connection unprivilegedManager;
        unprivilegedManager.fileDescriptor = connectDaemon(sockPath);
        if (!sendRequest(unprivilegedManager, { .target = Sync::AdmissionTarget::Management, .expectedDaemonInstanceID = expectedDaemon }, token)
            || !waitForBye(unprivilegedManager, Sync::ByeReason::OutsideScope)) {
            fail("ordinary control authority must not grant management access");
            break;
        }

        // Full authority does not revive either obsolete text layout. Their
        // low tag values belong to current kinds; only these bytes are invalid.
        bool areRetiredReadsRefused = true;
        for (const auto tag : { readRegionTag, readLastCommandTag }) {
            Connection retired;
            retired.fileDescriptor = connectDaemon(sockPath);
            if (!admit(retired, { .target = Sync::AdmissionTarget::Session, .sessionID = realSession, .expectedDaemonInstanceID = expectedDaemon, .isAttachOnly = true }, token)) {
                areRetiredReadsRefused = false;
            } else {
                areRetiredReadsRefused = sendReadRegion(retired, tag, 21, 0, 5)
                    && waitForBye(retired, Sync::ByeReason::Malformed)
                    && areRetiredReadsRefused;
            }
            ::close(retired.fileDescriptor);
        }
        if (!areRetiredReadsRefused) {
            fail("obsolete region layouts must refuse as malformed canonical bytes");
            break;
        }

        Connection retiredTermination;
        retiredTermination.fileDescriptor = connectDaemon(sockPath);
        if (!admit(retiredTermination, { .target = Sync::AdmissionTarget::Session, .sessionID = realSession, .expectedDaemonInstanceID = expectedDaemon, .isAttachOnly = true }, token)) {
            fail("retired termination fixture attaches with full authority");
            break;
        }
        std::string retiredBody;
        putU32(retiredBody, realSession);
        const bool wasRetiredTerminationRefused = sendObsoleteLayout(retiredTermination, terminateSessionTag, retiredBody)
            && waitForBye(retiredTermination, Sync::ByeReason::Malformed);
        ::close(retiredTermination.fileDescriptor);
        if (!wasRetiredTerminationRefused) {
            fail("the obsolete termination layout must refuse as malformed canonical bytes");
            break;
        }
        const auto afterRetired = listedNow(81);
        if (!afterRetired || afterRetired->size() != 1 || afterRetired->front().id != realSession) {
            fail("malformed alternate layouts must neither terminate nor create a session");
            break;
        }

        // --- A control-only peer lists the session with its geometry. ---
        Connection lister;
        lister.fileDescriptor = connectDaemon(sockPath);
        if (lister.fileDescriptor < 0) {
            fail("lister connect");
            break;
        }
        if (!admit(lister, { .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = expectedDaemon }, token)) {
            fail("Control-only Greet");
            break;
        }
        std::vector<ListedSession> listedRows;
        if (!listSessions(lister, 7, listedRows) || !entryFor(listedRows, realSession)) {
            fail("typed directory should name the created session");
            break;
        }

        // --- A second session; Normal Bye detaches without destroying it. ---
        Connection second;
        second.fileDescriptor = connectDaemon(sockPath);
        if (second.fileDescriptor < 0) {
            fail("second connect");
            break;
        }
        if (!admit(second, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = expectedDaemon }, token)) {
            fail("second session create");
            break;
        }
        const std::uint32_t secondSession = second.welcome->selection.sessionID;
        if (!leave(second)) {
            fail("the second session's client did not send a clean detach");
            break;
        }
        ::usleep(200 * 1000); // the drop lands before the next list
        if (!listSessions(lister, 8, listedRows) || !entryFor(listedRows, secondSession) || listedRows.size() != 2) {
            fail("Normal Bye should detach the client and leave its session listed");
            break;
        }

        // A second lister, connected the way the named stanzas below connect,
        // so their reads do not interleave with `lister`'s request IDs.
        Connection namedLister;
        namedLister.fileDescriptor = connectDaemon(sockPath);
        if (namedLister.fileDescriptor < 0) {
            fail("named lister connect");
            break;
        }
        if (!admit(namedLister, { .target = Sync::AdmissionTarget::Control, .expectedDaemonInstanceID = expectedDaemon }, token)) {
            fail("named Control-only Greet");
            break;
        }
        std::uint32_t nextRequestID = 100; // clear of the ids the stanzas above use
        std::vector<ListedSession> listed;
        // Ask the named lister until the answer satisfies `holds`, ~100 ms
        // apart. Each pass is a fresh request, so nothing here depends on how
        // long the daemon's loop takes to notice a departure.
        auto namedListUntil = [&](int attempts, auto&& holds) -> bool {
            for (int attempt = 0; attempt < attempts; ++attempt) {
                if (listSessions(namedLister, nextRequestID++, listed) && holds(listed))
                    return true;
                ::usleep(100 * 1000);
            }
            return false;
        };
        // End a session the way one of its own clients does — reattach, then
        // session.terminate under the requester-is-attached exemption. How the
        // named stanzas below clean up after themselves.
        auto terminateAsAttachedHuman = [&](std::uint32_t sessionID) -> bool {
            Connection closer;
            closer.fileDescriptor = connectAsHuman();
            if (closer.fileDescriptor < 0)
                return false;
            if (!admit(closer, { .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = expectedDaemon, .isAttachOnly = true })
                || closer.welcome->selection.sessionID != sessionID || !waitForPane(closer))
                return false;
            const auto handle = closer.paneHandle;
            const bool terminated = terminateSession(closer, sessionID) && waitForEndedPane(closer, handle);
            ::close(closer.fileDescriptor);
            return terminated;
        };

        // --- A Session Greet naming a workspace creates it; another naming the
        // same one attaches to what the first made. ---
        Connection alphaOwner;
        alphaOwner.fileDescriptor = connectAsHuman();
        if (alphaOwner.fileDescriptor < 0) {
            fail("alpha owner connect");
            break;
        }
        if (!admit(alphaOwner, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = expectedDaemon, .workspaceName = "alpha" })) {
            fail("naming a fresh workspace should create a session");
            break;
        }
        const std::uint32_t alphaSession = alphaOwner.welcome->selection.sessionID;
        const std::uint32_t alphaWorkspace = alphaOwner.welcome->selection.workspaceID;
        if (!alphaSession || !alphaWorkspace) {
            fail("the named session must select both of its actual identities");
            break;
        }
        Connection alphaGuest;
        alphaGuest.fileDescriptor = connectAsHuman();
        if (alphaGuest.fileDescriptor < 0) {
            fail("alpha guest connect");
            break;
        }
        if (!admit(alphaGuest, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = expectedDaemon, .workspaceName = "alpha" })
            || alphaGuest.welcome->selection.sessionID != alphaSession || alphaGuest.welcome->selection.workspaceID != alphaWorkspace) {
            fail("naming a live workspace should attach to it, not mint a second");
            break;
        }
        Connection betaOwner;
        betaOwner.fileDescriptor = connectAsHuman();
        if (betaOwner.fileDescriptor < 0) {
            fail("beta owner connect");
            break;
        }
        if (!admit(betaOwner, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = expectedDaemon, .workspaceName = "beta" })) {
            fail("naming a second workspace should create a session");
            break;
        }
        const std::uint32_t betaSession = betaOwner.welcome->selection.sessionID;
        if (betaSession == 0 || betaSession == alphaSession) {
            fail("a different name should reach a different session");
            break;
        }

        // --- The listing names each workspace, once, against the session that
        // holds it. ---
        if (!namedListUntil(20, [&](const std::vector<ListedSession>& sessions) {
                const ListedSession* alpha = entryFor(sessions, alphaSession);
                return alpha && alpha->workspaceName == "alpha" && alpha->workspaceID == alphaWorkspace;
            })) {
            fail("the listing should join alpha's actual selected workspace and session");
            break;
        }
        int alphaNamedEntries = 0;
        for (const ListedSession& entry : listed)
            if (entry.workspaceName == "alpha")
                ++alphaNamedEntries;
        const ListedSession* betaEntry = entryFor(listed, betaSession);
        if (alphaNamedEntries != 1 || !betaEntry || betaEntry->workspaceName != "beta") {
            fail("each name should list exactly once, against its own session");
            break;
        }

        // --- The listed strings are the session's own: the directory its shell
        // reports with OSC 7 reaches the entry, beside a live foreground
        // process. Typed once: the line waits in the terminal until the shell
        // reads it. ---
        const std::string reportDirectory = "cd /tmp\nprintf '\\033]7;file://localhost/tmp\\007'\n";
        bool hasAlphaValues = false;
        if (!sendInput(alphaOwner, reportDirectory)) {
            fail("the owner types the line that reports its directory");
            break;
        }
        for (int attempt = 0; attempt < 40 && !hasAlphaValues; ++attempt) {
            ::usleep(100 * 1000);
            if (!listSessions(namedLister, nextRequestID++, listed))
                break;
            const ListedSession* alpha = entryFor(listed, alphaSession);
            hasAlphaValues = alpha && alpha->currentDirectory.find("tmp") != std::string::npos && !alpha->foregroundProcess.empty();
        }
        if (!hasAlphaValues) {
            fail("the listed session should carry the directory its shell reported and a foreground process");
            break;
        }

        // --- The entry says who is on the session and how many of them are
        // people: two attached, both human, then one after a clean detach. ---
        if (!namedListUntil(20, [&](const std::vector<ListedSession>& sessions) {
                const ListedSession* alpha = entryFor(sessions, alphaSession);
                return alpha && alpha->attendeeCount == 2 && alpha->humanAttendeeCount == 2;
            })) {
            fail("two attached people should list as two attendees, both of them human");
            break;
        }
        if (!leave(alphaGuest)) {
            fail("the alpha guest did not send a clean detach");
            break;
        }
        if (!namedListUntil(30, [&](const std::vector<ListedSession>& sessions) {
                const ListedSession* alpha = entryFor(sessions, alphaSession);
                return alpha && alpha->attendeeCount == 1 && alpha->humanAttendeeCount == 1;
            })) {
            fail("a detaching attendee should leave the summary at one");
            break;
        }

        // --- A session created ends-when-detached goes with the client that
        // made it; one created without the flag survives the same Bye. ---
        Connection endsOwner;
        endsOwner.fileDescriptor = connectAsHuman();
        if (endsOwner.fileDescriptor < 0) {
            fail("ends-when-detached owner connect");
            break;
        }
        if (!admit(endsOwner, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = expectedDaemon, .workspaceName = "ends", .endsWhenDetached = true })) {
            fail("ends-when-detached session create");
            break;
        }
        const std::uint32_t endsSession = endsOwner.welcome->selection.sessionID;
        Connection keepsOwner;
        keepsOwner.fileDescriptor = connectAsHuman();
        if (keepsOwner.fileDescriptor < 0) {
            fail("outliving owner connect");
            break;
        }
        if (!admit(keepsOwner, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = expectedDaemon, .workspaceName = "keeps" })) {
            fail("outliving session create");
            break;
        }
        const std::uint32_t keepsSession = keepsOwner.welcome->selection.sessionID;
        if (!namedListUntil(20, [&](const std::vector<ListedSession>& sessions) {
                const ListedSession* ends = entryFor(sessions, endsSession);
                const ListedSession* keeps = entryFor(sessions, keepsSession);
                return ends && keeps && ends->endsWhenDetached() && !keeps->endsWhenDetached();
            })) {
            fail("the listing should say which session ends when its last client detaches");
            break;
        }
        if (!leave(endsOwner) || !leave(keepsOwner)) {
            fail("the lifetime comparison did not send both clean detaches");
            break;
        }
        if (!namedListUntil(30, [&](const std::vector<ListedSession>& sessions) {
                return entryFor(sessions, endsSession) == nullptr;
            })) {
            fail("an ends-when-detached session should be gone once its only client detaches");
            break;
        }
        if (!entryFor(listed, keepsSession)) {
            fail("a session without the flag should outlive the same Bye");
            break;
        }

        // --- An unnamed session outlives the window that opened it exactly as
        // a named one does: no name and no flag is a session the next connect
        // can return to, listed under an empty workspace name. ---
        Connection unnamedOwner;
        unnamedOwner.fileDescriptor = connectAsHuman();
        if (unnamedOwner.fileDescriptor < 0) {
            fail("unnamed owner connect");
            break;
        }
        if (!admit(unnamedOwner, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = expectedDaemon })) {
            fail("unnamed session create");
            break;
        }
        const std::uint32_t unnamedSession = unnamedOwner.welcome->selection.sessionID;
        if (!leave(unnamedOwner)) {
            fail("the unnamed session's client did not send a clean detach");
            break;
        }
        if (!namedListUntil(30, [&](const std::vector<ListedSession>& sessions) {
                const ListedSession* unnamed = entryFor(sessions, unnamedSession);
                return unnamed && unnamed->attendeeCount == 0;
            })) {
            fail("an unnamed session without the flag should outlive its only client's Bye");
            break;
        }
        const ListedSession* unnamedRow = entryFor(listed, unnamedSession);
        if (!unnamedRow || !unnamedRow->workspaceName.empty() || unnamedRow->endsWhenDetached()) {
            fail("the surviving unnamed session should list under no name and not as ending on detach");
            break;
        }

        // --- A lost transport is not a detach: the session waits out a
        // reconnect window instead of ending. Read at the moment the daemon has
        // seen the client go — its attendee count reaches zero — so "still
        // listed" cannot pass merely because the drop had yet to land. ---
        Connection dropsOwner;
        dropsOwner.fileDescriptor = connectAsHuman();
        if (dropsOwner.fileDescriptor < 0) {
            fail("dropping owner connect");
            break;
        }
        if (!admit(dropsOwner, { .target = Sync::AdmissionTarget::Session, .expectedDaemonInstanceID = expectedDaemon, .workspaceName = "drops", .endsWhenDetached = true })) {
            fail("dropping session create");
            break;
        }
        const std::uint32_t dropsSession = dropsOwner.welcome->selection.sessionID;
        ::close(dropsOwner.fileDescriptor); // no Bye: the relay exits and its socket closes under the daemon
        dropsOwner.fileDescriptor = -1;
        if (!namedListUntil(60, [&](const std::vector<ListedSession>& sessions) {
                const ListedSession* drops = entryFor(sessions, dropsSession);
                return drops && drops->attendeeCount == 0;
            })) {
            fail("a dropped transport should leave its ends-when-detached session listed with nobody on it");
            break;
        }

        // --- Attaching the id a listing offered: Welcome names the session the
        // client picked out of it. ---
        if (!listSessions(namedLister, nextRequestID++, listed)) {
            fail("typed directory before an attach by id");
            break;
        }
        const ListedSession* chosenRow = entryFor(listed, betaSession);
        if (!chosenRow) {
            fail("the listing should still offer the beta session");
            break;
        }
        Connection chosen;
        chosen.fileDescriptor = connectAsHuman();
        if (chosen.fileDescriptor < 0) {
            fail("chosen-id connect");
            break;
        }
        if (!admit(chosen, { .target = Sync::AdmissionTarget::Session, .sessionID = chosenRow->id, .expectedDaemonInstanceID = expectedDaemon, .isAttachOnly = true })
            || chosen.welcome->selection.sessionID != betaSession) {
            fail("a Greet carrying a listed id should select that exact session");
            break;
        }

        // --- Every lister reads the same sessions. ---
        std::vector<ListedSession> otherListed;
        if (!listSessions(lister, nextRequestID++, otherListed)) {
            fail("the first lister should parse the whole body");
            break;
        }
        if (!listSessions(namedLister, nextRequestID++, listed) || otherListed.empty() || otherListed.size() != listed.size()) {
            fail("both listers should see the same sessions");
            break;
        }

        // --- Clear the named sessions, so the teardown below is about the two
        // sessions this file's earlier stanzas made. ---
        if (!terminateAsAttachedHuman(alphaSession) || !terminateAsAttachedHuman(betaSession)
            || !terminateAsAttachedHuman(keepsSession) || !terminateAsAttachedHuman(unnamedSession)
            || !terminateAsAttachedHuman(dropsSession)) {
            fail("reattaching a detached session should let its own client end it");
            break;
        }
        if (!namedListUntil(40, [&](const std::vector<ListedSession>& sessions) {
                return sessions.size() == 2 && entryFor(sessions, realSession) && entryFor(sessions, secondSession);
            })) {
            fail("the named sessions should all be gone, leaving the two this file started with");
            break;
        }

        // --- The creator's principal rides the create-time session grant:
        // owner presented the same token as the detached session's creator, so
        // its identity matches agentSessionGrantIdentity and the terminate is
        // allowed without a consent prompt. (The never-attached DIFFERENT
        // principal's denial is consent_owner_gate's subject.) ---
        if (!terminateSession(owner, secondSession)) {
            fail("the creator's detached-session termination must return the exact typed result");
            break;
        }
        bool secondGone = false;
        for (int attempt = 0; attempt < 40 && !secondGone; ++attempt) {
            ::usleep(100 * 1000);
            if (!listSessions(lister, static_cast<std::uint32_t>(50 + attempt), listedRows))
                break;
            secondGone = !entryFor(listedRows, secondSession);
        }
        if (!secondGone) {
            fail("the creating principal's standing grant should allow terminating its detached session");
            break;
        }

        // A local manager retains repair access when a token store becomes
        // unreadable, but neither mutation may rewrite its readable prefix.
        const std::string tokensPath = macHome + "/tokens";
        const auto originalTokens = storeBytes(tokensPath);
        if (!originalTokens) {
            fail("the management mutation fixture could not read the token store");
            break;
        }
        bool wereMalformedStoresPreserved = true;
        for (const std::string& suffix : { std::string("\0hidden\n", 8), std::string("malformed token record\n") }) {
            const std::string malformed = *originalTokens + suffix;
            if (!replaceStoreBytes(tokensPath, malformed)) {
                wereMalformedStoresPreserved = false;
                break;
            }
            for (const auto operation : { Sync::ManagementOperation::IssueToken, Sync::ManagementOperation::RevokeToken }) {
                Sync::ManagementRequest mutation;
                mutation.requestID = nextRequestID++;
                mutation.daemonInstanceID = expectedDaemon;
                mutation.operation = operation;
                if (operation == Sync::ManagementOperation::IssueToken) {
                    mutation.label = "refused malformed store";
                    mutation.scopes = { Sync::Scope::Control };
                    mutation.expiryUnix = static_cast<std::uint64_t>(std::time(nullptr)) + 3600;
                } else
                    mutation.identity = Ubiquitty::Crypto::sha256Hex(token);
                body.clear();
                const auto refused = administration(manager, mutation, &body);
                const auto unchanged = storeBytes(tokensPath);
                if (!refused || refused->status != Sync::ManagementStatus::Failed || !refused->issuedToken.empty()
                    || body.find(token) != std::string::npos || !unchanged || *unchanged != malformed) {
                    wereMalformedStoresPreserved = false;
                    break;
                }
            }
            if (!wereMalformedStoresPreserved)
                break;
        }
        if (!replaceStoreBytes(tokensPath, *originalTokens) || !wereMalformedStoresPreserved) {
            fail("administrative token mutations must refuse incomplete stores without changing bytes or exposing a secret");
            break;
        }
        // The malformed store withdrew the original token-bound connection.
        // A repaired store admits its principal on a fresh connection.
        Connection finalOwner;
        Ubiquitty::PAL::UnixFileDescriptor finalOwnerSocket(connectDaemon(sockPath));
        finalOwner.fileDescriptor = finalOwnerSocket.get();
        if (!admit(finalOwner, { .target = Sync::AdmissionTarget::Session, .sessionID = realSession, .expectedDaemonInstanceID = expectedDaemon, .isAttachOnly = true }, token)) {
            fail("the repaired store must admit the session's original principal");
            break;
        }

        // --- The attached owner terminates its own session: the exemption
        // through the consent gate. It was the last session, so the destroy is
        // observable as the daemon's own clean exit — the ever-had-a-session,
        // now-empty condition that ends its loop. ---
        if (!waitForPane(finalOwner)) {
            fail("the attached owner never received its actual pane binding");
            break;
        }
        const auto ownerHandle = finalOwner.paneHandle;
        if (!terminateSession(finalOwner, realSession) || !waitForEndedPane(finalOwner, ownerHandle)) {
            fail("the attached owner's termination must return the exact typed result");
            break;
        }
        // Patience has to EXCEED the daemon's session-exit drain, not match it.
        //
        // Other fixture clients may still have queued records to drain. The
        // bounded wait exceeds the daemon's session-exit drain deadline.
        //
        // Twenty seconds is four times the drain. It costs nothing when the
        // daemon exits on time, because this polls rather than sleeps.
        constexpr int millisecondsBetweenChecks = 20;
        constexpr int patienceMilliseconds = 20000; // > sessionExitDrainMilliseconds
        bool daemonExited = false;
        for (int attempt = 0; attempt < patienceMilliseconds / millisecondsBetweenChecks && !daemonExited; ++attempt) {
            if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
                daemonExited = true;
            else
                ::usleep(millisecondsBetweenChecks * 1000);
        }
        if (!daemonExited) {
            fail("terminating the last session should end the daemon's loop");
            break;
        }
        daemon = -1; // reaped; teardown below must not signal another process

        rc = 0;
    } while (false);

    if (rc == 0)
        std::printf("control_surface: typed list/obsolete-layout refusal/detach/terminate and the named-session "
                    "attach-or-create, workspace listing, attendee summary and ends-when-detached lifetime "
                    "answer over a live daemon socket\n");

    // The spawned `--attach` relays outlive their sessions when a stanza broke
    // early; each is a child of this process, so each is reaped here.
    for (const pid_t bridgeProcessID : bridgeProcessIDs) {
        ::kill(bridgeProcessID, SIGKILL);
        ::waitpid(bridgeProcessID, nullptr, 0);
    }

    if (daemon > 0) {
        ::kill(daemon, SIGTERM);
        for (int i = 0; i < 50; ++i) {
            if (::waitpid(daemon, nullptr, WNOHANG) == daemon)
                break;
            ::usleep(20 * 1000);
        }
        ::kill(daemon, SIGKILL);
        ::waitpid(daemon, nullptr, WNOHANG);
    }
    return rc;
}
