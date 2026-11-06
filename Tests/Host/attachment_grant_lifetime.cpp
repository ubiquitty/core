// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/attachment_grant_lifetime.cpp — what a resume proof is served
// under lives exactly as long as its attachment (Docs/sync.md §6.9).
//
// A connection a resume proof admits presents no token: the daemon serves it
// under what the bootstrap token of the client that created the attachment
// was granted. That grant is the permission boundary, so it is the token's
// scope and nothing wider. Socket attachments retain token withdrawal identity
// without acquiring QUIC-proof eligibility. Authority
// stays while the attachment is Lost inside its grace, and goes the moment
// the attachment is freed, by its grace running out or by its client leaving
// on purpose; a proof for a freed attachment is refused by the table. A
// connection a proof admitted that never greeted is dropped when a newer
// proof takes its place.
//
// The grants are held in memory beside the attachment table and are written
// nowhere. Explicit authorization withdrawal retires the exact serving
// attachment before the client is reaped, while ordinary transport loss keeps
// it resumable. A stale stream cannot retire its replacement's standing.
//
// Friend seam, the QUIC-off host build, as screen_lost_silence. Exit 0 on
// success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Attachment.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Protocol/Sync/ResumeProof.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/TerminalOperations.h"
#include "Helpers/TempDir.h"
#include "Host/FullCarrier.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/TokenStore.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// Whether a client's connection is one a QUIC preamble authenticated.
enum class Carrier : std::uint8_t {
    QUIC,
    Socket,
};

/// Attaches a client under a token's scope and greets.
///
/// - Returns: The client's index.
std::size_t attachAndGreet(Daemon& d, auto& session, const char* token, Sync::ScopeSet scope, Carrier carrier, UbiquittyTest::ScreenClient& client, Ubiquitty::PAL::UnixFileDescriptor transport = {})
{
    const std::size_t index = Screen::attach(d, session.id, std::move(transport));
    Screen::grantScope(d, index, scope);
    Screen::setTokenID(d, index, token);
    if (carrier == Carrier::QUIC)
        Screen::markQUICAuthenticated(d, index);
    // Explicit prior owner consent belongs to this exact session/principal.
    // It neither changes the token's scope nor promotes an Agent to Human.
    Screen::grantAgentSession(d, session, index);
    Screen::greet(d, index, client);
    return index;
}

/// Keying material a test picks for a connection, as a handshake would
/// export.
std::array<std::uint8_t, 32> exporterOf(std::uint8_t first)
{
    std::array<std::uint8_t, 32> exporter {};
    for (std::size_t index = 0; index < exporter.size(); ++index)
        exporter[index] = static_cast<std::uint8_t>(first + index);
    return exporter;
}

/// Gives refusal output a successful sink without involving a live host.
Ubiquitty::PAL::UnixFileDescriptor writableTransport()
{
    return Ubiquitty::PAL::UnixFileDescriptor(::open("/dev/null", O_WRONLY));
}

/// Greets on a full carrier, so the Welcome the greeting flushes stays for
/// `deliver`, then gives the client a sink that takes whatever follows.
std::size_t attachGreetAndSink(Daemon& d, auto& session, const char* token, Sync::ScopeSet scope, UbiquittyTest::ScreenClient& client)
{
    Ubiquitty::HostTest::FullCarrier carrier;
    CHECK(carrier.isReady());
    const std::size_t index = attachAndGreet(d, session, token, scope, Carrier::QUIC, client, carrier.takeDaemonEnd());
    auto sink = writableTransport();
    CHECK(static_cast<bool>(sink));
    Screen::useTransport(d, index, std::move(sink));
    return index;
}

enum class Withdrawal : std::uint8_t {
    Consent,
    Denied,
    Expired,
};

void withdrawalRetiresOnlyItsServingAttachment()
{
    constexpr std::array withdrawals { Withdrawal::Consent, Withdrawal::Denied, Withdrawal::Expired };
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    for (const Withdrawal withdrawal : withdrawals) {
        Daemon d;
        auto& session = Screen::makeSession(d, 24, 80);
        Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
        UbiquittyTest::ScreenClient target(24, 80, Sync::ID128 { .low = 101, .high = 1 });
        UbiquittyTest::ScreenClient neighbor(24, 80, Sync::ID128 { .low = 102, .high = 1 });
        const std::size_t targetIndex = attachGreetAndSink(d, session, "target", driving, target);
        const std::size_t neighborIndex = attachGreetAndSink(d, session, "neighbor", driving, neighbor);
        REQUIRE(target.connection().welcome());
        REQUIRE(neighbor.connection().welcome());
        const Sync::Welcome welcome = *target.connection().welcome();
        const Sync::ID128 neighborID = neighbor.connection().welcome()->attachmentID;
        const Sync::AttachmentRecord* record = Screen::attachments(d).find(welcome.attachmentID);
        REQUIRE(record);
        const Sync::ConnectionAuthority authority = record->authority;
        REQUIRE(Screen::grantCount(d) == 2);
        REQUIRE(Screen::connection(d, targetIndex)->state() == Sync::ConnectionState::Attached);

        switch (withdrawal) {
        case Withdrawal::Consent:
            Screen::kick(d, targetIndex);
            break;
        case Withdrawal::Denied:
            Screen::denyAuthorization(d, targetIndex, Sync::ByeReason::TokenRevoked);
            break;
        case Withdrawal::Expired:
            Screen::expireToken(d, targetIndex);
            break;
        }
        // No deferred client/attachment reap is needed to end the authority.
        // The carrier drains its refusal and the loop reaps it at its exit
        // deadline; the checks below are what end at once.
        CHECK(Screen::isLeaving(d, targetIndex));
        CHECK(Screen::connection(d, targetIndex)->state() == Sync::ConnectionState::Closed);
        CHECK(Screen::connection(d, targetIndex)->attachment() == nullptr);
        CHECK(Screen::attachments(d).find(welcome.attachmentID) == nullptr);
        CHECK(!Screen::grantedScope(d, welcome.attachmentID));
        CHECK(Screen::grantCount(d) == 1);
        CHECK(Screen::attachments(d).find(neighborID) != nullptr);
        CHECK(Screen::grantedScope(d, neighborID) == std::optional { driving });
        CHECK(!Screen::didOverflow(d, neighborIndex));

        const auto exporter = exporterOf(30);
        const Sync::ResumeProof proof { .attachmentID = welcome.attachmentID, .mac = Sync::resumeProofMAC(welcome.resumeSecret, exporter, welcome.attachmentID, welcome.hostInstanceID) };
        const auto proofResult = Screen::attachments(d).admitProof(proof, exporter, 9'100, Screen::now());
        CHECK(!proofResult && proofResult.error() == Sync::ResumeRefusal::UnknownAttachment);
        const Sync::ResumeRequest request { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
        const auto secretResult = Screen::attachments(d).resume(request, authority, 9'101, Screen::now());
        CHECK(!secretResult && secretResult.error() == Sync::ResumeRefusal::UnknownAttachment);
        Screen::kick(d, targetIndex);
        Screen::drop(d, targetIndex);
        Screen::reap(d, targetIndex);
        Screen::reapAttachments(d, Screen::now());
        CHECK(Screen::attachments(d).find(neighborID) != nullptr);
        CHECK(Screen::grantedScope(d, neighborID) == std::optional { driving });
    }
}

void staleWithdrawalLeavesTheResumedAttachment()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    UbiquittyTest::ScreenClient oldClient(24, 80, Sync::ID128 { .low = 103, .high = 1 });
    const auto index = attachGreetAndSink(d, session, "resumer", driving, oldClient);
    REQUIRE(oldClient.connection().welcome());
    const auto attachmentID = oldClient.connection().welcome()->attachmentID;
    REQUIRE(Screen::resumeAttachmentElsewhere(d, index));
    const Sync::AttachmentRecord* resumed = Screen::attachments(d).find(attachmentID);
    REQUIRE(resumed);
    const auto servingConnection = resumed->servingConnection;
    const auto secret = resumed->resumeSecret;
    Screen::kick(d, index);
    resumed = Screen::attachments(d).find(attachmentID);
    REQUIRE(resumed);
    CHECK(resumed->state == Sync::AttachmentState::Live);
    CHECK(resumed->servingConnection == servingConnection);
    CHECK(resumed->resumeSecret == secret);
    CHECK(Screen::grantedScope(d, attachmentID) == std::optional { driving });
    CHECK(Screen::connection(d, index)->attachment() == nullptr);
}

void withdrawalReleasesADeferredLostBorrower()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    Sync::ReplicaSet replicas;
    UbiquittyTest::ScreenClient first(24, 80, replicas, std::nullopt);
    Ubiquitty::HostTest::FullCarrier firstCarrier;
    const auto firstIndex = attachAndGreet(d, session, "returner", driving, Carrier::QUIC, first, firstCarrier.takeDaemonEnd());
    REQUIRE(first.connection().welcome());
    const Sync::Welcome welcome = *first.connection().welcome();
    Screen::drop(d, firstIndex);
    REQUIRE(Screen::connection(d, firstIndex)->attachment() == nullptr);
    const Sync::AttachmentRecord* lost = Screen::attachments(d).find(welcome.attachmentID);
    REQUIRE(lost);
    REQUIRE(lost->state == Sync::AttachmentState::Lost);

    const Sync::ResumeRequest request { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
    UbiquittyTest::ScreenClient second(24, 80, replicas, request);
    const auto secondIndex = attachGreetAndSink(d, session, "returner", driving, second);
    REQUIRE(second.connection().welcome());
    REQUIRE(second.connection().welcome()->attachmentID == welcome.attachmentID);
    Screen::kick(d, secondIndex);
    CHECK(Screen::connection(d, firstIndex)->attachment() == nullptr);
    CHECK(Screen::connection(d, secondIndex)->attachment() == nullptr);
    CHECK(Screen::attachments(d).find(welcome.attachmentID) == nullptr);
    CHECK(!Screen::grantedScope(d, welcome.attachmentID));
    Screen::reap(d, secondIndex);
    Screen::reap(d, firstIndex);
}

/// Attaches a client under a stored token's verdict and greets, on a full
/// carrier the caller keeps for as long as the client must stay attached.
std::optional<std::size_t> attachStoredToken(Daemon& d, auto& session, const std::string& path,
    const std::string& token, Carrier carrier, UbiquittyTest::ScreenClient& client, Ubiquitty::HostTest::FullCarrier& transport)
{
    const auto verdict = Ubiquitty::Host::verifyToken(path, token, static_cast<std::uint64_t>(::time(nullptr)));
    if (!verdict.ok)
        return std::nullopt;
    if (!transport.isReady())
        return std::nullopt;
    const auto index = Screen::attach(d, session.id, transport.takeDaemonEnd());
    Screen::grantScope(d, index, verdict.scope);
    Screen::setTokenID(d, index, verdict.tokenID);
    Screen::setTokenExpiry(d, index, verdict.expiryUnix);
    if (carrier == Carrier::QUIC)
        Screen::markQUICAuthenticated(d, index);
    Screen::grantAgentSession(d, session, index);
    Screen::greet(d, index, client);
    return index;
}

Sync::ResumeProof proofFor(const Sync::Welcome& welcome, const std::array<std::uint8_t, 32>& exporter)
{
    return { .attachmentID = welcome.attachmentID,
        .mac = Sync::resumeProofMAC(welcome.resumeSecret, exporter, welcome.attachmentID, welcome.hostInstanceID) };
}

enum class StoreChange : std::uint8_t {
    Revoke,
    Replace,
    Narrow,
    Expire,
};

void aReapedAttachmentFollowsItsExactStoredToken()
{
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    for (const Carrier carrier : { Carrier::Socket, Carrier::QUIC }) {
        for (const StoreChange change : { StoreChange::Revoke, StoreChange::Replace, StoreChange::Narrow, StoreChange::Expire }) {
            Ubiquitty::Test::TempDir directory;
            REQUIRE(directory.valid());
            const std::string path = directory.path() + "/tokens";
            const Sync::TokenRecord targetRecord { .hashHex = Ubiquitty::Crypto::sha256Hex("target-token"), .scope = driving, .expiryUnix = 0, .label = "same label" };
            const Sync::TokenRecord neighborRecord { .hashHex = Ubiquitty::Crypto::sha256Hex("neighbor-token"), .scope = driving, .expiryUnix = 0, .label = "same label" };
            REQUIRE(Ubiquitty::Host::writeTokens(path, { targetRecord, neighborRecord }) != Ubiquitty::Host::TokenWriteOutcome::IOError);
            Daemon d;
            Screen::useTokenStore(d, path);
            auto& session = Screen::makeSession(d, 24, 80);
            Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
            UbiquittyTest::ScreenClient target(24, 80, Sync::ID128 { .low = 201, .high = 1 });
            UbiquittyTest::ScreenClient neighbor(24, 80, Sync::ID128 { .low = 202, .high = 1 });
            Ubiquitty::HostTest::FullCarrier targetCarrier;
            Ubiquitty::HostTest::FullCarrier neighborCarrier;
            const auto targetIndex = attachStoredToken(d, session, path, "target-token", carrier, target, targetCarrier);
            const auto neighborIndex = attachStoredToken(d, session, path, "neighbor-token", carrier, neighbor, neighborCarrier);
            REQUIRE(targetIndex && neighborIndex);
            REQUIRE(target.connection().welcome() && neighbor.connection().welcome());
            const Sync::Welcome welcome = *target.connection().welcome();
            const auto neighborID = neighbor.connection().welcome()->attachmentID;
            const auto authority = Screen::attachments(d).find(welcome.attachmentID)->authority;
            Screen::drop(d, *neighborIndex);
            Screen::reap(d, *neighborIndex);
            Screen::drop(d, *targetIndex);
            Screen::reap(d, *targetIndex);
            REQUIRE(Screen::grantCount(d) == 2);
            Screen::reverifyTokens(d);
            REQUIRE(Screen::attachments(d).find(welcome.attachmentID));

            std::vector<Sync::TokenRecord> records { neighborRecord };
            if (change != StoreChange::Revoke) {
                auto replacement = targetRecord;
                if (change == StoreChange::Replace)
                    replacement.hashHex = Ubiquitty::Crypto::sha256Hex("replacement-token");
                else if (change == StoreChange::Narrow)
                    replacement.scope = { Sync::Scope::Observe };
                else
                    replacement.expiryUnix = 1;
                records.push_back(std::move(replacement));
            }
            REQUIRE(Ubiquitty::Host::writeTokens(path, records) != Ubiquitty::Host::TokenWriteOutcome::IOError);
            Screen::reverifyTokens(d);
            CHECK(!Screen::attachments(d).find(welcome.attachmentID));
            CHECK(!Screen::grantedScope(d, welcome.attachmentID));
            CHECK(Screen::attachments(d).find(neighborID));
            CHECK(Screen::grantedScope(d, neighborID) == std::optional { driving });
            CHECK(Screen::grantCount(d) == 1);
            const auto exporter = exporterOf(50);
            const auto candidate = Screen::attach(d, session.id);
            const auto proof = Screen::admitProof(d, candidate, proofFor(welcome, exporter), exporter);
            CHECK(!proof && proof.error() == Sync::ResumeRefusal::UnknownAttachment);
            const Sync::ResumeRequest request { .attachmentID = welcome.attachmentID, .resumeSecret = welcome.resumeSecret, .panes = {} };
            const auto secret = Screen::attachments(d).resume(request, authority, 9'200, Screen::now());
            CHECK(!secret && secret.error() == Sync::ResumeRefusal::UnknownAttachment);
        }
    }
}

void proofAdmissionKeepsTheStoredTokenDeadlineAndOrigin()
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const std::string path = directory.path() + "/tokens";
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    const auto expiry = static_cast<std::uint64_t>(::time(nullptr)) + 3'600;
    Sync::TokenRecord record { .hashHex = Ubiquitty::Crypto::sha256Hex("persistent-token"), .scope = driving, .expiryUnix = expiry, .label = "persistent" };
    REQUIRE(Ubiquitty::Host::writeTokens(path, { record }) != Ubiquitty::Host::TokenWriteOutcome::IOError);
    Daemon d;
    Screen::useTokenStore(d, path);
    auto& session = Screen::makeSession(d, 24, 80);
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    UbiquittyTest::ScreenClient original(24, 80, Sync::ID128 { .low = 203, .high = 1 });
    Ubiquitty::HostTest::FullCarrier originalCarrier;
    const auto originalIndex = attachStoredToken(d, session, path, "persistent-token", Carrier::QUIC, original, originalCarrier);
    REQUIRE(originalIndex && original.connection().welcome());
    const Sync::Welcome welcome = *original.connection().welcome();
    Screen::drop(d, *originalIndex);
    Screen::reap(d, *originalIndex);
    REQUIRE(Screen::nextTokenExpiry(d) == expiry);
    record.expiryUnix += 3'600;
    REQUIRE(Ubiquitty::Host::writeTokens(path, { record }) != Ubiquitty::Host::TokenWriteOutcome::IOError);
    const auto candidate = Screen::attach(d, session.id);
    const auto exporter = exporterOf(51);
    REQUIRE(Screen::admitProof(d, candidate, proofFor(welcome, exporter), exporter));
    CHECK(Screen::hasStoredAuthority(d, candidate, record.hashHex, driving, expiry));
    CHECK(Screen::nextTokenExpiry(d) == expiry);
    REQUIRE(Screen::attachments(d).provenAttachment(Screen::clientID(d, candidate)) == std::optional { welcome.attachmentID });

    // Revocation while the admitted proof awaits Greet cannot turn it into a
    // fresh bootstrap-authorized connection.
    REQUIRE(Ubiquitty::Host::writeTokens(path, {}) != Ubiquitty::Host::TokenWriteOutcome::IOError);
    Screen::reverifyTokens(d);
    CHECK(Screen::didOverflow(d, candidate));
    CHECK(!Screen::attachments(d).provenAttachment(Screen::clientID(d, candidate)));
    CHECK(!Screen::attachments(d).find(welcome.attachmentID));
    CHECK(Screen::grantCount(d) == 0);
}

void anExpiredRetainedGrantIsRefusedBeforeProofConsumption()
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const std::string path = directory.path() + "/tokens";
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    const auto expiry = static_cast<std::uint64_t>(::time(nullptr)) + 3'600;
    const Sync::TokenRecord record { .hashHex = Ubiquitty::Crypto::sha256Hex("expiring-token"), .scope = driving, .expiryUnix = expiry, .label = "expiring" };
    REQUIRE(Ubiquitty::Host::writeTokens(path, { record }) != Ubiquitty::Host::TokenWriteOutcome::IOError);
    Daemon d;
    Screen::useTokenStore(d, path);
    auto& session = Screen::makeSession(d, 24, 80);
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    UbiquittyTest::ScreenClient original(24, 80, Sync::ID128 { .low = 204, .high = 1 });
    Ubiquitty::HostTest::FullCarrier originalCarrier;
    const auto originalIndex = attachStoredToken(d, session, path, "expiring-token", Carrier::QUIC, original, originalCarrier);
    REQUIRE(originalIndex && original.connection().welcome());
    const Sync::Welcome welcome = *original.connection().welcome();
    Screen::drop(d, *originalIndex);
    Screen::reap(d, *originalIndex);
    Screen::expireRetainedToken(d, welcome.attachmentID);
    CHECK(Screen::nextTokenExpiry(d) == 1);
    const auto candidate = Screen::attach(d, session.id);
    const auto exporter = exporterOf(52);
    const auto admitted = Screen::admitProof(d, candidate, proofFor(welcome, exporter), exporter);
    CHECK(!admitted && admitted.error() == Sync::ResumeRefusal::UnknownAttachment);
    CHECK(!Screen::attachments(d).find(welcome.attachmentID));
    CHECK(Screen::grantCount(d) == 0);
    CHECK(Screen::nextTokenExpiry(d) == 0);
}

void bootstrapResumeDoesNotBecomeAStoredToken()
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    Daemon d;
    Screen::useTokenStore(d, directory.path() + "/tokens");
    auto& session = Screen::makeSession(d, 24, 80);
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    UbiquittyTest::ScreenClient original(24, 80, Sync::ID128 { .low = 205, .high = 1 });
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    Ubiquitty::HostTest::FullCarrier originalCarrier;
    const auto index = Screen::attach(d, session.id, originalCarrier.takeDaemonEnd());
    Screen::grantScope(d, index, driving);
    Screen::setTokenID(d, index, Ubiquitty::Crypto::sha256Hex("bootstrap-token"));
    Screen::markBootstrapToken(d, index);
    Screen::markInteractiveUser(d, index);
    Screen::markQUICAuthenticated(d, index);
    Screen::greet(d, index, original);
    REQUIRE(original.connection().welcome());
    const Sync::Welcome welcome = *original.connection().welcome();
    Screen::drop(d, index);
    Screen::reap(d, index);
    Screen::reverifyTokens(d);
    REQUIRE(Screen::attachments(d).find(welcome.attachmentID));
    const auto candidate = Screen::attach(d, session.id);
    const auto exporter = exporterOf(53);
    REQUIRE(Screen::admitProof(d, candidate, proofFor(welcome, exporter), exporter));
    CHECK(Screen::hasBootstrapAuthority(d, candidate));
    CHECK(Screen::nextTokenExpiry(d) == 0);
}

void inheritedAuthorityChecksExactGrantsBeforeCommit()
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/tokens";
    const auto nowUnix = static_cast<std::uint64_t>(::time(nullptr));
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    const Sync::TokenRecord token { .hashHex = Ubiquitty::Crypto::sha256Hex("inherited-token"), .scope = driving, .expiryUnix = nowUnix + 3'600, .label = "inherited" };
    REQUIRE(Ubiquitty::Host::writeTokens(path, { token }) != Ubiquitty::Host::TokenWriteOutcome::IOError);
    Daemon predecessor;
    Screen::useTokenStore(predecessor, path);
    auto& source = Screen::makeSession(predecessor, 24, 80);
    Screen::publish(predecessor, source, Ubiquitty::Runtime::FrameClass::Structural);
    UbiquittyTest::ScreenClient original(24, 80);
    Ubiquitty::HostTest::FullCarrier originalCarrier;
    const auto sourceIndex = attachStoredToken(predecessor, source, path, "inherited-token", Carrier::QUIC, original, originalCarrier);
    REQUIRE(sourceIndex && original.connection().welcome());
    const auto welcome = *original.connection().welcome();
    const auto at = Screen::now();

    for (unsigned fault = 0; fault < 8; ++fault) {
        REQUIRE(Ubiquitty::Host::writeTokens(path, { token }) != Ubiquitty::Host::TokenWriteOutcome::IOError);
        auto carried = Screen::exportAttachmentAuthority(predecessor, at);
        REQUIRE(carried.attachments.records.size() == 1 && carried.grants.size() == 1);
        switch (fault) {
        case 0:
            carried.grants.clear();
            break;
        case 1:
            carried.grants[0].tokenID = Ubiquitty::Crypto::sha256Hex("other-token");
            break;
        case 2:
            carried.grants.push_back(carried.grants.front());
            break;
        case 3:
            carried.grants[0].tokenExpiryUnix = nowUnix;
            break;
        case 4:
            REQUIRE(Ubiquitty::Host::writeTokens(path, {}) != Ubiquitty::Host::TokenWriteOutcome::IOError);
            break;
        case 5: {
            auto narrowed = token;
            narrowed.scope = { Sync::Scope::Observe };
            REQUIRE(Ubiquitty::Host::writeTokens(path, { narrowed }) != Ubiquitty::Host::TokenWriteOutcome::IOError);
            break;
        }
        case 6: {
            auto expired = token;
            expired.expiryUnix = nowUnix;
            REQUIRE(Ubiquitty::Host::writeTokens(path, { expired }) != Ubiquitty::Host::TokenWriteOutcome::IOError);
            break;
        }
        case 7:
            carried.grants[0].isInteractiveUser = true;
            break;
        }
        const auto* retained = carried.attachments.records[0].get();
        const auto grantCount = carried.grants.size();
        Daemon successor;
        Screen::useTokenStore(successor, path);
        const auto admitted = Screen::inheritAttachmentAuthority(successor, std::move(carried), welcome.hostInstanceID, at, nowUnix);
        CHECK(!admitted);
        if (admitted)
            continue;
        REQUIRE(carried.attachments.records.size() == 1);
        CHECK(carried.attachments.records[0].get() == retained);
        CHECK(carried.grants.size() == grantCount);
        CHECK(!Screen::attachments(successor).find(welcome.attachmentID));
        CHECK(!Screen::attachments(successor).isResumableIssuer(welcome.hostInstanceID));
        CHECK(Screen::grantCount(successor) == 0);
    }

    auto shortened = token;
    shortened.expiryUnix -= 1'800;
    REQUIRE(Ubiquitty::Host::writeTokens(path, { shortened }) != Ubiquitty::Host::TokenWriteOutcome::IOError);
    {
        Daemon unpublished;
        Screen::useTokenStore(unpublished, path);
        auto prepared = Screen::prepareAttachmentAuthority(unpublished, Screen::exportAttachmentAuthority(predecessor, at), welcome.hostInstanceID, at, nowUnix);
        REQUIRE(prepared);
        REQUIRE(prepared->attachments->find(welcome.attachmentID));
        CHECK(prepared->attachments->isResumableIssuer(welcome.hostInstanceID));
        CHECK(prepared->grants.size() == 1);
        CHECK(prepared->grants.front().tokenExpiryUnix == shortened.expiryUnix);
        CHECK(Screen::attachments(unpublished).isPristine());
        CHECK(!Screen::attachments(unpublished).find(welcome.attachmentID));
        CHECK(!Screen::attachments(unpublished).isResumableIssuer(welcome.hostInstanceID));
        CHECK(Screen::grantCount(unpublished) == 0);
        CHECK(Screen::attachments(predecessor).find(welcome.attachmentID)->state == Sync::AttachmentState::Live);
        CHECK(Screen::attachments(predecessor).find(welcome.attachmentID)->resumeSecret == welcome.resumeSecret);
        prepared->attachments.reset();
        CHECK(Screen::attachments(unpublished).isPristine());
        REQUIRE(Screen::inheritAttachmentAuthority(unpublished, Screen::exportAttachmentAuthority(predecessor, at), welcome.hostInstanceID, at, nowUnix));
        CHECK(Screen::attachments(unpublished).find(welcome.attachmentID));
    }
    {
        Daemon alreadyUsed;
        Screen::useTokenStore(alreadyUsed, path);
        auto& table = Screen::attachments(alreadyUsed);
        const auto id = table.create({ .low = 73, .high = 91 }, Sync::ClientKind::Agent, {}, 1, at).attachmentID;
        const auto prepared = Screen::prepareAttachmentAuthority(alreadyUsed, Screen::exportAttachmentAuthority(predecessor, at), welcome.hostInstanceID, at, nowUnix);
        CHECK(!prepared && prepared.error() == Ubiquitty::Host::AttachmentHandoverRefusal::DestinationInUse);
        CHECK(table.find(id));
        CHECK(!table.find(welcome.attachmentID));
    }
    auto carried = Screen::exportAttachmentAuthority(predecessor, at);
    Daemon successor;
    Screen::useTokenStore(successor, path);
    REQUIRE(Screen::inheritAttachmentAuthority(successor, std::move(carried), welcome.hostInstanceID, at, nowUnix));
    CHECK(Screen::nextTokenExpiry(successor) == shortened.expiryUnix);
    REQUIRE(Screen::attachments(predecessor).find(welcome.attachmentID));
    CHECK(Screen::attachments(predecessor).find(welcome.attachmentID)->state == Sync::AttachmentState::Live);
    auto& destination = Screen::makeSession(successor, 24, 80);
    // The authority-only fixture transfers the session's owner-consent fields
    // explicitly, rather than manufacturing a grant for the reconnecting peer.
    destination.agentSessionGrantIdentities = source.agentSessionGrantIdentities;
    Screen::publish(successor, destination, Ubiquitty::Runtime::FrameClass::Structural);
    Ubiquitty::HostTest::FullCarrier resumedCarrier;
    const auto candidate = Screen::attach(successor, destination.id, resumedCarrier.takeDaemonEnd());
    Screen::markQUICAuthenticated(successor, candidate);
    const auto exporter = exporterOf(60);
    REQUIRE(Screen::admitProof(successor, candidate, proofFor(welcome, exporter), exporter));
    CHECK(Screen::hasStoredAuthority(successor, candidate, token.hashHex, driving, shortened.expiryUnix));
    Sync::ReplicaSet replicas;
    UbiquittyTest::ScreenClient resumed(24, 80, replicas, Sync::ResumeRequest { .attachmentID = welcome.attachmentID, .resumeSecret = std::nullopt, .panes = {} });
    Screen::greet(successor, candidate, resumed);
    REQUIRE(resumed.connection().welcome());
    CHECK(resumed.connection().welcome()->attachmentID == welcome.attachmentID);
    CHECK(resumed.connection().welcome()->predecessorHostInstanceID == std::optional { welcome.hostInstanceID });
    CHECK(resumed.connection().welcome()->hostInstanceID != welcome.hostInstanceID);
    CHECK(resumed.connection().welcome()->resumeSecret != welcome.resumeSecret);
}

/// A token attachment carried without the grant it was admitted under is
/// refused as `MissingGrant`, never defaulted to another authority, and the
/// successor's tables stay untouched.
void aTokenAttachmentArrivingWithoutItsGrantIsRefusedAsMissingGrant()
{
    Ubiquitty::Test::TempDir directory;
    REQUIRE(directory.valid());
    const auto path = directory.path() + "/tokens";
    const auto nowUnix = static_cast<std::uint64_t>(::time(nullptr));
    const Sync::TokenRecord token { .hashHex = Ubiquitty::Crypto::sha256Hex("missing-grant"), .scope = { Sync::Scope::Observe, Sync::Scope::Drive }, .expiryUnix = nowUnix + 3'600, .label = "missing" };
    REQUIRE(Ubiquitty::Host::writeTokens(path, { token }) != Ubiquitty::Host::TokenWriteOutcome::IOError);
    Daemon predecessor;
    Screen::useTokenStore(predecessor, path);
    auto& source = Screen::makeSession(predecessor, 24, 80);
    Screen::publish(predecessor, source, Ubiquitty::Runtime::FrameClass::Structural);
    UbiquittyTest::ScreenClient original(24, 80);
    Ubiquitty::HostTest::FullCarrier carrier;
    REQUIRE(attachStoredToken(predecessor, source, path, "missing-grant", Carrier::QUIC, original, carrier));
    REQUIRE(original.connection().welcome());
    const auto welcome = *original.connection().welcome();
    const auto at = Screen::now();
    auto carried = Screen::exportAttachmentAuthority(predecessor, at);
    REQUIRE(carried.attachments.records.size() == 1 && carried.grants.size() == 1);
    carried.grants.clear();
    Daemon successor;
    Screen::useTokenStore(successor, path);
    const auto admitted = Screen::inheritAttachmentAuthority(successor, std::move(carried), welcome.hostInstanceID, at, nowUnix);
    CHECK_MSG(!admitted && admitted.error() == Ubiquitty::Host::AttachmentHandoverRefusal::MissingGrant,
        "an attachment carried without its grant was not refused as MissingGrant");
    CHECK(Screen::attachments(successor).isPristine());
    CHECK(!Screen::attachments(successor).isResumableIssuer(welcome.hostInstanceID));
    CHECK(Screen::grantCount(successor) == 0);
}

void inheritedProofCannotPromoteALabeledBootstrapAgent()
{
    Daemon predecessor;
    auto& session = Screen::makeSession(predecessor, 24, 80);
    Screen::publish(predecessor, session, Ubiquitty::Runtime::FrameClass::Structural);
    UbiquittyTest::ScreenClient original(24, 80);
    Ubiquitty::HostTest::FullCarrier originalCarrier;
    const auto index = Screen::attach(predecessor, session.id, originalCarrier.takeDaemonEnd());
    Screen::grantScope(predecessor, index, { Sync::Scope::Observe, Sync::Scope::Drive });
    Screen::setTokenID(predecessor, index, Ubiquitty::Crypto::sha256Hex("labeled-bootstrap"));
    Screen::markBootstrapToken(predecessor, index);
    Screen::markInteractiveUser(predecessor, index);
    Screen::markQUICAuthenticated(predecessor, index);
    Screen::setAgentLabel(predecessor, index, "automation");
    Screen::grantAgentSession(predecessor, session, index);
    Screen::greet(predecessor, index, original);
    REQUIRE(original.connection().welcome());
    const auto welcome = *original.connection().welcome();
    REQUIRE(Screen::attachments(predecessor).find(welcome.attachmentID)->authority.verifiedKind == Sync::ClientKind::Agent);
    const auto now = Screen::now();
    Daemon successor;
    REQUIRE(Screen::inheritAttachmentAuthority(successor, Screen::exportAttachmentAuthority(predecessor, now), welcome.hostInstanceID, now, static_cast<std::uint64_t>(::time(nullptr))));
    auto& destination = Screen::makeSession(successor, 24, 80);
    const auto candidate = Screen::attach(successor, destination.id);
    Screen::markQUICAuthenticated(successor, candidate);
    const auto exporter = exporterOf(61);
    REQUIRE(Screen::admitProof(successor, candidate, proofFor(welcome, exporter), exporter));
    CHECK(Screen::isAgent(successor, candidate));
    CHECK(!Screen::hasBootstrapAuthority(successor, candidate));
}

void inheritedOuterPinReachesOnlyItsExistingSession()
{
    Daemon predecessor;
    auto& source = Screen::makeSession(predecessor, 24, 80);
    const Sync::ID128 paneID { .low = source.id, .high = Screen::daemonInstanceID(predecessor) };
    source.screenSource = std::make_unique<Sync::PaneSource>(*source.term, paneID);
    Screen::publish(predecessor, source, Ubiquitty::Runtime::FrameClass::Structural);
    UbiquittyTest::ScreenClient original(24, 80);
    Ubiquitty::HostTest::FullCarrier originalCarrier;
    const auto index = Screen::attach(predecessor, source.id, originalCarrier.takeDaemonEnd());
    Screen::grantScope(predecessor, index, { Sync::Scope::Observe, Sync::Scope::Drive });
    Screen::setTokenID(predecessor, index, Ubiquitty::Crypto::sha256Hex("inherited-outer-pin"));
    Screen::markBootstrapToken(predecessor, index);
    Screen::markInteractiveUser(predecessor, index);
    Screen::markQUICAuthenticated(predecessor, index);
    Screen::greet(predecessor, index, original);
    REQUIRE(original.connection().welcome());
    const auto welcome = *original.connection().welcome();
    REQUIRE(welcome.hostInstanceID != (Sync::ID128 { .low = Screen::daemonInstanceID(predecessor), .high = 0 }));
    REQUIRE(std::ranges::any_of(Screen::attachments(predecessor).find(welcome.attachmentID)->panes, [&](const auto& pane) { return pane->paneID == paneID && pane->isHeld; }));

    for (unsigned depth = 0; depth < 2; ++depth) {
        Daemon intermediate;
        Daemon* authoritySource = &predecessor;
        auto expectedWelcome = welcome;
        if (depth) {
            const auto now = Screen::now();
            REQUIRE(Screen::inheritAttachmentAuthority(intermediate, Screen::exportAttachmentAuthority(predecessor, now), welcome.hostInstanceID, now, static_cast<std::uint64_t>(::time(nullptr))));
            auto& middleSession = Screen::makeSession(intermediate, 24, 80);
            middleSession.screenSource = std::make_unique<Sync::PaneSource>(*middleSession.term, paneID);
            Screen::publish(intermediate, middleSession, Ubiquitty::Runtime::FrameClass::Structural);
            Ubiquitty::HostTest::FullCarrier middleCarrier;
            const auto middleClient = Screen::attach(intermediate, middleSession.id, middleCarrier.takeDaemonEnd());
            Screen::markQUICAuthenticated(intermediate, middleClient);
            const auto exporter = exporterOf(63);
            REQUIRE(Screen::admitProof(intermediate, middleClient, proofFor(welcome, exporter), exporter));
            Sync::ReplicaSet replicas;
            UbiquittyTest::ScreenClient resumed(24, 80, replicas, Sync::ResumeRequest { .attachmentID = welcome.attachmentID, .resumeSecret = std::nullopt, .panes = {} });
            Screen::greet(intermediate, middleClient, resumed);
            REQUIRE(resumed.connection().welcome());
            expectedWelcome = *resumed.connection().welcome();
            REQUIRE(expectedWelcome.hostInstanceID != welcome.hostInstanceID);
            REQUIRE(paneID.high != Screen::daemonInstanceID(intermediate));
            REQUIRE(std::ranges::any_of(Screen::attachments(intermediate).find(welcome.attachmentID)->panes, [&](const auto& pane) { return pane->paneID == paneID && pane->isHeld; }));
            authoritySource = &intermediate;
        }
        for (unsigned fault = 0; fault < (depth ? 1u : 10u); ++fault) {
            std::array<int, 2> descriptors { -1, -1 };
            REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0);
            Ubiquitty::PAL::UnixFileDescriptor peer(descriptors[1]);
            REQUIRE(::fcntl(peer.get(), F_SETFL, O_NONBLOCK) == 0);
            Daemon successor;
            const auto now = Screen::now();
            REQUIRE(Screen::inheritAttachmentAuthority(successor, Screen::exportAttachmentAuthority(*authoritySource, now), expectedWelcome.hostInstanceID, now, static_cast<std::uint64_t>(::time(nullptr))));
            auto& destination = Screen::makeSession(successor, 24, 80);
            REQUIRE(destination.id == source.id);
            destination.screenSource = std::make_unique<Sync::PaneSource>(*destination.term, paneID);
            Screen::publish(successor, destination, Ubiquitty::Runtime::FrameClass::Structural);
            const auto candidate = Screen::connect(successor, Ubiquitty::PAL::UnixFileDescriptor(descriptors[0]));
            Screen::markQUICAuthenticated(successor, candidate);
            const auto exporter = exporterOf(62);
            REQUIRE(Screen::admitProof(successor, candidate, proofFor(expectedWelcome, exporter), exporter));
            Sync::Greet greeting;
            greeting.wireVersion = Sync::wireVersion;
            greeting.appVersion = Sync::appVersion();
            greeting.clientInstance = { .low = 77, .high = 0x696e6865726974 };
            greeting.facts.viewportColumns = 80;
            greeting.facts.viewportRows = 24;
            greeting.request = { .target = Sync::AdmissionTarget::Session, .sessionID = source.id, .expectedDaemonInstanceID = Screen::daemonInstanceID(*authoritySource), .isAttachOnly = true };
            // The proof permits only continuation of its exact attachment.
            greeting.resume = Sync::ResumeRequest { .attachmentID = expectedWelcome.attachmentID, .resumeSecret = std::nullopt, .panes = {} };
            switch (fault) {
            case 0:
                break;
            case 1:
                greeting.request.expectedDaemonInstanceID ^= 1;
                break;
            case 2:
                REQUIRE(welcome.hostInstanceID.low != greeting.request.expectedDaemonInstanceID);
                greeting.request.expectedDaemonInstanceID = welcome.hostInstanceID.low;
                break;
            case 3:
                greeting.request.sessionID += 100;
                break;
            case 4:
                greeting.request.sessionID = 0;
                break;
            case 5:
                greeting.request.target = Sync::AdmissionTarget::Control;
                greeting.request.sessionID = 0;
                greeting.request.isAttachOnly = false;
                greeting.resume.reset();
                break;
            case 6:
                greeting.request.sessionID = Screen::makeSession(successor, 24, 80).id;
                break;
            case 7:
                Screen::setTokenID(successor, candidate, Ubiquitty::Crypto::sha256Hex("another-principal"));
                break;
            case 8:
                // Encoded below by mutating a valid existing-session request;
                // the canonical producer itself refuses this combination.
                break;
            case 9:
                // A named-workspace client retains this field on reconnect;
                // the explicit existing session remains the only selector.
                greeting.request.workspaceName = "ignored-on-explicit-reattach";
                break;
            }
            const bool shouldAttach = fault == 0 || fault == 9;
            if (!shouldAttach) {
                greeting.facts.viewportColumns = 100;
                greeting.facts.viewportRows = 40;
            }
            const auto sessionCount = Screen::sessionCount(successor);
            std::vector<std::uint8_t> body;
            Sync::encode(body, greeting);
            REQUIRE(Sync::decodeGreet(body));
            if (fault == 8) {
                Sync::WireReader reader(body);
                (void)reader.u16();
                (void)reader.utf8(Sync::maxAppVersionBytes);
                (void)reader.take(greeting.engineFingerprint.size() + 16 + 2);
                (void)reader.u8();
                (void)reader.u32();
                (void)reader.u32();
                (void)reader.u64();
                (void)reader.utf8(Sync::maxFrameBodyBytes);
                (void)reader.utf8(Sync::maxFrameBodyBytes);
                const auto begin = reader.offset();
                (void)reader.utf8(Sync::maxFrameBodyBytes);
                const auto end = reader.offset();
                REQUIRE(reader.ok() && end <= body.size());
                std::vector<std::uint8_t> changed(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(begin));
                Sync::WireWriter writer(changed);
                writer.utf8("example.invalid");
                changed.insert(changed.end(), body.begin() + static_cast<std::ptrdiff_t>(end), body.end());
                body = std::move(changed);
                CHECK(!Sync::decodeGreet(body));
                greeting.request.sshHost = "example.invalid";
                std::vector<std::uint8_t> refused;
                Sync::encode(refused, greeting);
                CHECK(refused.empty());
            }
            std::vector<std::uint8_t> encoded;
            REQUIRE(Sync::appendFrame(encoded, Sync::FrameKind::Greet, 0, body));
            Screen::handleFrames(successor, candidate, encoded);
            Screen::service(successor);
            Screen::flush(successor, candidate);
            CHECK(Screen::didOverflow(successor, candidate) != shouldAttach);
            CHECK(Screen::sessionCount(successor) == sessionCount);
            CHECK(destination.term->rows == 24 && destination.term->columns == 80);
            Sync::FrameDecoder decoder;
            bool sawAcknowledgement = false;
            bool sawRefusal = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!sawAcknowledgement && !sawRefusal && std::chrono::steady_clock::now() < deadline) {
                pollfd waiting { .fd = peer.get(), .events = POLLIN, .revents = 0 };
                if (::poll(&waiting, 1, 50) <= 0)
                    continue;
                std::array<std::uint8_t, 8192> reply {};
                const auto count = ::read(peer.get(), reply.data(), reply.size());
                if (count <= 0)
                    break;
                decoder.append(std::span(reply).first(static_cast<std::size_t>(count)));
                while (const auto frame = decoder.next()) {
                    if (frame->kind == Sync::FrameKind::Welcome) {
                        const auto acknowledgement = Sync::decodeWelcome(frame->body);
                        REQUIRE(acknowledgement);
                        CHECK(!sawAcknowledgement && !sawRefusal);
                        CHECK(acknowledgement->admission == Sync::AttachmentAdmission::Granted);
                        CHECK(acknowledgement->selection.target == Sync::AdmissionTarget::Session);
                        CHECK(acknowledgement->selection.sessionID == source.id);
                        CHECK(acknowledgement->selection.daemonInstanceID == Screen::daemonInstanceID(successor));
                        CHECK(acknowledgement->attachmentID == expectedWelcome.attachmentID);
                        CHECK(acknowledgement->predecessorHostInstanceID == std::optional { expectedWelcome.hostInstanceID });
                        sawAcknowledgement = true;
                    } else if (frame->kind == Sync::FrameKind::Bye) {
                        const auto refusal = Sync::decodeBye(frame->body);
                        REQUIRE(refusal);
                        CHECK(!sawAcknowledgement);
                        // An unbound Control target is a scope the token, which
                        // carries only Observe and Drive, does not grant.
                        CHECK(refusal->reason == (fault == 8 ? Sync::ByeReason::Malformed : fault == 5 ? Sync::ByeReason::OutsideScope
                                                                                                       : Sync::ByeReason::SessionGone));
                        sawRefusal = true;
                    } else
                        CHECK(sawAcknowledgement);
                }
            }
            CHECK(decoder.error() == Sync::EnvelopeError::None);
            CHECK(sawAcknowledgement == shouldAttach);
            CHECK(sawRefusal != shouldAttach);
        }
    }

    for (unsigned fault = 0; fault < 4; ++fault) {
        const auto now = Screen::now();
        auto carried = Screen::exportAttachmentAuthority(predecessor, now);
        REQUIRE(carried.hostIdentities.size() == 1);
        switch (fault) {
        case 0:
            carried.hostIdentities.clear();
            break;
        case 1:
            carried.hostIdentities[0].issuer.low ^= 1;
            break;
        case 2:
            carried.hostIdentities[0].daemonInstanceID = 0;
            break;
        case 3:
            carried.hostIdentities.push_back(carried.hostIdentities.front());
            break;
        }
        const auto* record = carried.attachments.records.front().get();
        Daemon successor;
        const auto result = Screen::inheritAttachmentAuthority(successor, std::move(carried), welcome.hostInstanceID, now, static_cast<std::uint64_t>(::time(nullptr)));
        CHECK(!result && result.error() == Ubiquitty::Host::AttachmentHandoverRefusal::InvalidTable);
        REQUIRE(!carried.attachments.records.empty());
        CHECK(carried.attachments.records.front().get() == record);
        CHECK(Screen::attachments(successor).find(welcome.attachmentID) == nullptr);
    }

    {
        Daemon successor;
        const auto now = Screen::now();
        REQUIRE(Screen::inheritAttachmentAuthority(successor, Screen::exportAttachmentAuthority(predecessor, now), welcome.hostInstanceID, now, static_cast<std::uint64_t>(::time(nullptr))));
        const auto& table = Screen::attachments(successor);
        const auto* record = table.find(welcome.attachmentID);
        REQUIRE(record);
        CHECK(table.holdsResumablePane(welcome.hostInstanceID, record->authority.principal, paneID, std::nullopt, now));
        CHECK(!table.holdsResumablePane(welcome.hostInstanceID, record->authority.principal, paneID, std::nullopt, now - 1));
        CHECK(!table.holdsResumablePane(welcome.hostInstanceID, record->authority.principal, paneID, std::nullopt, now + Sync::measuredResumePolicy().lostGraceMicroseconds));
        CHECK(!table.holdsResumablePane(welcome.hostInstanceID, record->authority.principal, paneID, Sync::ID128 { .low = 99, .high = 99 }, now));
    }

    // Identity pairs have exactly the same transitive lifetime as the table's
    // issuer list; an abandoned old secret cannot keep an outer pin admitted.
    auto carried = Screen::exportAttachmentAuthority(predecessor, Screen::now());
    for (unsigned hop = 0; hop < 9; ++hop) {
        Daemon successor;
        const auto parent = carried.attachments.hostInstanceID;
        REQUIRE(Screen::inheritAttachmentAuthority(successor, std::move(carried), parent, Screen::now(), static_cast<std::uint64_t>(::time(nullptr))));
        CHECK((Screen::attachments(successor).find(welcome.attachmentID) != nullptr) == (hop < 8));
        carried = Screen::exportAttachmentAuthority(successor, Screen::now());
        CHECK(carried.hostIdentities.size() == carried.attachments.inheritedInstances.size() + 1);
        CHECK(carried.hostIdentities.size() <= 9);
        if (hop == 8)
            CHECK(std::ranges::none_of(carried.hostIdentities, [&](const auto& identity) { return identity.daemonInstanceID == Screen::daemonInstanceID(predecessor); }));
        if (hop == 0) {
            auto malformed = Screen::exportAttachmentAuthority(successor, Screen::now());
            REQUIRE(malformed.hostIdentities.size() == 2);
            malformed.hostIdentities.back().daemonInstanceID = malformed.hostIdentities.front().daemonInstanceID;
            const auto* owner = malformed.attachments.records.front().get();
            Daemon refused;
            const auto result = Screen::inheritAttachmentAuthority(refused, std::move(malformed), carried.attachments.hostInstanceID, Screen::now(), static_cast<std::uint64_t>(::time(nullptr)));
            CHECK(!result && result.error() == Ubiquitty::Host::AttachmentHandoverRefusal::InvalidTable);
            REQUIRE(!malformed.attachments.records.empty());
            CHECK(malformed.attachments.records.front().get() == owner);
        }
    }
}

void droppingAnUnboundClientReleasesItsRecordBeforeReaping()
{
    Daemon daemon;
    for (unsigned index = 0; index < 3; ++index)
        (void)Screen::makeSession(daemon, 24, 80);
    const auto index = Screen::attachControlOnly(daemon);
    Screen::grantScope(daemon, index, { Sync::Scope::Observe, Sync::Scope::Control });
    Screen::setTokenID(daemon, index, "directory-reader");
    Screen::markInteractiveUser(daemon, index);
    UbiquittyTest::ScreenClient client { 24, 80 };
    Screen::greet(daemon, index, client);
    REQUIRE(client.connection().welcome());
    const auto attachmentID = client.connection().welcome()->attachmentID;
    const auto* connection = Screen::connection(daemon, index);
    REQUIRE(connection && connection->attachment());
    REQUIRE(!connection->attachment()->isResumable);
    REQUIRE(connection->boundPanes().empty());

    Screen::drop(daemon, index);
    CHECK_MSG(connection->attachment() == nullptr, "a dropped directory client still borrows its retired record");
    CHECK(connection->state() == Sync::ConnectionState::Closed);
    CHECK(Screen::attachments(daemon).find(attachmentID) == nullptr);
    Screen::reap(daemon, index);
    CHECK(Screen::sessionCount(daemon) == 3);
}

void droppingAScreenClientReleasesItsRecordBeforeExpiryAndReaping()
{
    for (Carrier carrierKind : { Carrier::Socket, Carrier::QUIC }) {
        Daemon daemon;
        auto& session = Screen::makeSession(daemon, 24, 80);
        Sync::ReplicaSet replicas;
        UbiquittyTest::ScreenClient client { 24, 80, replicas, std::nullopt };
        Ubiquitty::HostTest::FullCarrier carrier;
        REQUIRE(carrier.isReady());
        const auto index = attachAndGreet(daemon, session, "screen-reader", { Sync::Scope::Observe, Sync::Scope::Drive },
            carrierKind, client, carrier.takeDaemonEnd());
        REQUIRE(client.connection().welcome());
        const auto attachmentID = client.connection().welcome()->attachmentID;
        const auto* connection = Screen::connection(daemon, index);
        REQUIRE(connection && connection->attachment());
        REQUIRE(connection->attachment()->isResumable);

        Screen::drop(daemon, index);
        CHECK_MSG(connection->attachment() == nullptr, "a dropped screen client still borrows the table's record");
        CHECK(connection->state() == Sync::ConnectionState::Closed);
        CHECK(connection->boundPanes().empty());
        const auto* retained = Screen::attachments(daemon).find(attachmentID);
        REQUIRE(retained);
        CHECK(retained->state == Sync::AttachmentState::Lost);
        Screen::reapAttachments(daemon, Screen::now() + Sync::measuredResumePolicy().lostGraceMicroseconds + 1);
        CHECK(Screen::attachments(daemon).find(attachmentID) == nullptr);
        Screen::reap(daemon, index);
        CHECK(Screen::sessionCount(daemon) == 1);
    }
}

void run()
{
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);
    Ubiquitty::processBytes(session.term.get(), "$ ");
    Screen::publish(d, session, Ubiquitty::Runtime::FrameClass::Structural);
    const Sync::ScopeSet observing { Sync::Scope::Observe };
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive };
    const std::uint64_t grace = Sync::measuredResumePolicy().lostGraceMicroseconds;

    // A socket token retains withdrawal identity but cannot authorize a
    // QUIC proof, even with the attachment's correct secret.
    Sync::ReplicaSet socketReplicas;
    UbiquittyTest::ScreenClient onSocket(24, 80, socketReplicas, std::nullopt);
    Ubiquitty::HostTest::FullCarrier socketCarrier;
    const auto socketIndex = attachAndGreet(d, session, "socket", driving, Carrier::Socket, onSocket, socketCarrier.takeDaemonEnd());
    REQUIRE(onSocket.connection().welcome().has_value());
    CHECK(Screen::grantCount(d) == 1);
    const auto proofCandidate = Screen::attach(d, session.id);
    const auto socketExporter = exporterOf(54);
    CHECK(!Screen::admitProof(d, proofCandidate, proofFor(*onSocket.connection().welcome(), socketExporter), socketExporter));
    CHECK(Screen::attachments(d).find(onSocket.connection().welcome()->attachmentID));
    CHECK(Screen::grantCount(d) == 1);
    Screen::drop(d, proofCandidate);
    Screen::reap(d, proofCandidate);
    Screen::kick(d, socketIndex);
    Screen::reap(d, socketIndex);
    CHECK(Screen::grantCount(d) == 0);

    // A QUIC client's attachment keeps its token's scope, and nothing wider.
    UbiquittyTest::ScreenClient watcher(24, 80, Sync::ID128 { .low = 5, .high = 6 });
    Ubiquitty::HostTest::FullCarrier watcherCarrier;
    const std::size_t watcherIndex = attachAndGreet(d, session, "watcher", observing, Carrier::QUIC, watcher, watcherCarrier.takeDaemonEnd());
    REQUIRE(watcher.connection().welcome().has_value());
    const Sync::Welcome watcherWelcome = *watcher.connection().welcome();
    CHECK_MSG(Screen::grantCount(d) == 1, "no grant was kept for an attachment a QUIC client made");
    CHECK_MSG(Screen::grantedScope(d, watcherWelcome.attachmentID) == std::optional { observing }, "the grant kept is not the scope the client's token carried");
    // Greeted again on later passes, the grant is the one kept, once.
    Screen::deliver(d, watcherIndex, watcher);
    CHECK_MSG(Screen::grantCount(d) == 1, "a second grant was kept for one attachment");

    // Its transport goes: the attachment is Lost and its grant stays for the
    // grace, so a proof inside it is served; then both go together.
    const std::uint64_t droppedAt = Screen::now();
    Screen::drop(d, watcherIndex);
    Screen::reapAttachments(d, droppedAt + grace / 2);
    CHECK_MSG(Screen::attachments(d).find(watcherWelcome.attachmentID) != nullptr, "premise: the attachment was freed inside its grace");
    CHECK_MSG(Screen::grantedScope(d, watcherWelcome.attachmentID).has_value(), "a Lost attachment's grant went before the attachment did");
    Screen::reapAttachments(d, Screen::now() + grace + 1);
    CHECK_MSG(Screen::attachments(d).find(watcherWelcome.attachmentID) == nullptr, "premise: the attachment outlived its grace");
    CHECK_MSG(!Screen::grantedScope(d, watcherWelcome.attachmentID).has_value() && Screen::grantCount(d) == 0, "a grant outlived its attachment");
    // A proof for it, the right secret and all, is refused: there is nothing
    // to continue and nothing to be served under.
    {
        const std::array<std::uint8_t, 32> exporter = exporterOf(1);
        const Sync::ResumeProof proof { .attachmentID = watcherWelcome.attachmentID, .mac = Sync::resumeProofMAC(watcherWelcome.resumeSecret, exporter, watcherWelcome.attachmentID, watcherWelcome.hostInstanceID) };
        const auto admitted = Screen::attachments(d).admitProof(proof, exporter, 9'001, Screen::now() + grace + 2);
        CHECK_MSG(!admitted && admitted.error() == Sync::ResumeRefusal::UnknownAttachment, "a proof for a freed attachment was admitted");
    }

    // A client that leaves on purpose frees its attachment at once, and its
    // grant with it.
    UbiquittyTest::ScreenClient leaver(24, 80, Sync::ID128 { .low = 7, .high = 8 });
    // Its carrier holds what the greeting flushes, so the client is still
    // attached to send its Bye.
    Ubiquitty::HostTest::FullCarrier leaverCarrier;
    REQUIRE(leaverCarrier.isReady());
    const std::size_t leaverIndex = attachAndGreet(d, session, "leaver", driving, Carrier::QUIC, leaver, leaverCarrier.takeDaemonEnd());
    REQUIRE(leaver.connection().welcome().has_value());
    const Sync::Welcome leaverWelcome = *leaver.connection().welcome();
    REQUIRE(Screen::grantedScope(d, leaverWelcome.attachmentID) == std::optional { driving });
    std::vector<std::uint8_t> body;
    Sync::encode(body, Sync::Bye { .reason = Sync::ByeReason::Normal });
    std::vector<std::uint8_t> leaving;
    REQUIRE(Sync::appendFrame(leaving, Sync::FrameKind::Bye, 0, body));
    Screen::handleFrames(d, leaverIndex, leaving);
    Screen::drop(d, leaverIndex);
    Screen::reapAttachments(d, Screen::now());
    CHECK_MSG(Screen::attachments(d).find(leaverWelcome.attachmentID) == nullptr, "premise: a client that left on purpose kept its attachment");
    CHECK_MSG(Screen::grantCount(d) == 0, "the grant of a client that left on purpose outlived its attachment");

    // A client whose proof only ran out of time comes back through the
    // login and attaches fresh, as the same client instance under the same
    // token. The attachment it left Lost is freed as the fresh one is made,
    // with its grant, so the host holds one attachment for the client and a
    // proof for the old one is refused.
    {
        const Sync::ID128 instance { .low = 11, .high = 12 };
        UbiquittyTest::ScreenClient before(24, 80, instance);
        Ubiquitty::HostTest::FullCarrier beforeCarrier;
        const std::size_t beforeIndex = attachAndGreet(d, session, "returner", driving, Carrier::QUIC, before, beforeCarrier.takeDaemonEnd());
        REQUIRE(before.connection().welcome().has_value());
        const Sync::Welcome beforeWelcome = *before.connection().welcome();
        Screen::drop(d, beforeIndex);
        Screen::reapAttachments(d, Screen::now());
        REQUIRE(Screen::attachments(d).find(beforeWelcome.attachmentID) != nullptr);
        REQUIRE(Screen::grantedScope(d, beforeWelcome.attachmentID).has_value());
        const std::size_t grantsBefore = Screen::grantCount(d);

        UbiquittyTest::ScreenClient after(24, 80, instance);
        Ubiquitty::HostTest::FullCarrier afterCarrier;
        (void)attachAndGreet(d, session, "returner", driving, Carrier::QUIC, after, afterCarrier.takeDaemonEnd());
        REQUIRE(after.connection().welcome().has_value());
        const Sync::Welcome afterWelcome = *after.connection().welcome();
        CHECK_MSG(afterWelcome.attachmentID != beforeWelcome.attachmentID, "premise: the login resumed the old attachment");
        Screen::reapAttachments(d, Screen::now());
        CHECK_MSG(Screen::attachments(d).find(beforeWelcome.attachmentID) == nullptr, "the attachment a client left Lost stayed beside the one it made on coming back");
        CHECK_MSG(!Screen::grantedScope(d, beforeWelcome.attachmentID).has_value(), "the replaced attachment's grant outlived it");
        CHECK_MSG(Screen::grantCount(d) == grantsBefore && Screen::grantedScope(d, afterWelcome.attachmentID) == std::optional { driving }, "the fresh attachment is not the client's one grant");
        const std::array<std::uint8_t, 32> exporter = exporterOf(4);
        const Sync::ResumeProof proof { .attachmentID = beforeWelcome.attachmentID, .mac = Sync::resumeProofMAC(beforeWelcome.resumeSecret, exporter, beforeWelcome.attachmentID, beforeWelcome.hostInstanceID) };
        const auto admitted = Screen::attachments(d).admitProof(proof, exporter, 9'002, Screen::now());
        CHECK_MSG(!admitted && admitted.error() == Sync::ResumeRefusal::UnknownAttachment, "a proof for the replaced attachment was admitted");
    }

    // Two connections show a proof for one attachment and neither has
    // greeted: the newer takes the place, and the daemon drops the older.
    UbiquittyTest::ScreenClient roamer(24, 80, Sync::ID128 { .low = 9, .high = 10 });
    Ubiquitty::HostTest::FullCarrier roamerCarrier;
    const std::size_t roamerIndex = attachAndGreet(d, session, "roamer", driving, Carrier::QUIC, roamer, roamerCarrier.takeDaemonEnd());
    REQUIRE(roamer.connection().welcome().has_value());
    const Sync::Welcome roamerWelcome = *roamer.connection().welcome();
    Screen::drop(d, roamerIndex);
    const std::size_t olderIndex = Screen::attach(d, session.id);
    const std::size_t newerIndex = Screen::attach(d, session.id);
    const auto proofUnder = [&](const std::array<std::uint8_t, 32>& exporter) {
        return Sync::ResumeProof { .attachmentID = roamerWelcome.attachmentID, .mac = Sync::resumeProofMAC(roamerWelcome.resumeSecret, exporter, roamerWelcome.attachmentID, roamerWelcome.hostInstanceID) };
    };
    REQUIRE(Screen::attachments(d).admitProof(proofUnder(exporterOf(2)), exporterOf(2), Screen::clientID(d, olderIndex), Screen::now()).has_value());
    Screen::closeSuperseded(d);
    CHECK_MSG(!Screen::didOverflow(d, olderIndex), "premise: the only proof connection was dropped");
    REQUIRE(Screen::attachments(d).admitProof(proofUnder(exporterOf(3)), exporterOf(3), Screen::clientID(d, newerIndex), Screen::now()).has_value());
    Screen::closeSuperseded(d);
    CHECK_MSG(Screen::didOverflow(d, olderIndex), "the connection an earlier proof admitted, which never greeted, was left open when a newer proof took its place");
    CHECK_MSG(!Screen::didOverflow(d, newerIndex), "the connection the newer proof admitted was dropped");
    CHECK_MSG(Screen::attachments(d).provenAttachment(Screen::clientID(d, newerIndex)) == std::optional { roamerWelcome.attachmentID }, "dropping the older connection gave up the newer one's proof");
}

} // namespace

int main()
{
    // As the daemon's own process does: a write to a carrier whose reader has
    // gone is an error to handle, not a signal that ends the fixture.
    ::signal(SIGPIPE, SIG_IGN);
    droppingAnUnboundClientReleasesItsRecordBeforeReaping();
    droppingAScreenClientReleasesItsRecordBeforeExpiryAndReaping();
    run();
    withdrawalRetiresOnlyItsServingAttachment();
    staleWithdrawalLeavesTheResumedAttachment();
    withdrawalReleasesADeferredLostBorrower();
    aReapedAttachmentFollowsItsExactStoredToken();
    proofAdmissionKeepsTheStoredTokenDeadlineAndOrigin();
    anExpiredRetainedGrantIsRefusedBeforeProofConsumption();
    bootstrapResumeDoesNotBecomeAStoredToken();
    inheritedAuthorityChecksExactGrantsBeforeCommit();
    aTokenAttachmentArrivingWithoutItsGrantIsRefusedAsMissingGrant();
    inheritedProofCannotPromoteALabeledBootstrapAgent();
    inheritedOuterPinReachesOnlyItsExistingSession();
    if (failures == 0)
        std::printf("attachment_grant_lifetime: token authority survives transport loss, ends on withdrawal or expiry, and admits proofs only for eligible attachments\n");
    return failures == 0 ? 0 : 1;
}
