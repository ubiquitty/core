// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Control/HostAdministration.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/ControlRPC.h"

#include <array>
#include <limits>

namespace {

using namespace Ubiquitty;
using namespace Protocol::Control;
namespace Sync = Protocol::Sync;
using Codec::JSON::Value;

constexpr std::array operations {
    Sync::ManagementOperation::List, Sync::ManagementOperation::DisconnectClient,
    Sync::ManagementOperation::RevokeGrant, Sync::ManagementOperation::IssueToken,
    Sync::ManagementOperation::RevokeToken
};

Sync::ManagementRequest intent(Sync::ManagementOperation operation)
{
    Sync::ManagementRequest request;
    request.requestID = 7;
    request.daemonInstanceID = std::numeric_limits<std::uint64_t>::max();
    request.operation = operation;
    request.clientID = 13;
    request.identity = "test-only-grant-or-fingerprint";
    request.label = "test-only credential";
    request.scopes = { Sync::Scope::Observe, Sync::Scope::Manage };
    request.expiryUnix = std::numeric_limits<std::uint64_t>::max();
    return request;
}

Sync::ManagementReply outcome(const Sync::ManagementRequest& request)
{
    Sync::ManagementReply reply;
    reply.requestID = request.requestID;
    reply.daemonInstanceID = request.daemonInstanceID;
    reply.status = Sync::ManagementStatus::Success;
    if (request.operation == Sync::ManagementOperation::IssueToken)
        reply.issuedToken = "test-only-not-a-real-token";
    return reply;
}

Value json(const std::string& text)
{
    bool isJSON = false;
    auto value = Codec::JSON::parse(text, isJSON);
    CHECK(isJSON);
    return value;
}

std::string response(const Sync::ManagementRequest& request, const Sync::ManagementReply& reply)
{
    const auto id = Value::string("management.7");
    auto value = hostAdministrationResponse(request, reply, &id);
    CHECK(value.has_value());
    if (value) {
        const auto* method = findSyncMethod(hostAdministrationMethod(request.operation));
        CHECK(method);
        const auto* result = value->find("result");
        CHECK(result);
        if (method && result)
            CHECK(!validate(*result, parseRegistryJSON(method->resultSchema)));
    }
    return value ? Codec::JSON::serialize(*value) : std::string();
}

Value replacing(const Value& source, const std::string& key, Value replacement)
{
    auto result = Value::object();
    bool didReplace = false;
    for (const auto& [name, value] : source.members()) {
        if (name == key) {
            result.set(name, replacement);
            didReplace = true;
        } else
            result.set(name, value);
    }
    if (!didReplace)
        result.set(key, std::move(replacement));
    return result;
}

Value replacingResult(const Value& source, const std::string& key, Value replacement)
{
    const auto* result = source.find("result");
    CHECK(result);
    return result ? replacing(source, "result", replacing(*result, key, std::move(replacement))) : Value();
}

} // namespace

TEST(administrationRequestsAreTypedPinnedAndKeepExactSelectors)
{
    constexpr std::array expectedMethods {
        "host.management.list", "host.client.disconnect", "host.grant.revoke", "host.token.issue", "host.token.revoke"
    };
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto request = intent(operations[index]);
        const auto call = decodeRequest(hostAdministrationRequest(request, "management.7"), "");
        CHECK(call.outcome == RequestOutcome::Dispatch);
        CHECK(call.method == expectedMethods[index]);
        CHECK(call.id.asString() == "management.7");
        CHECK(call.params.stringOr("daemonInstance") == "18446744073709551615");
        const auto decoded = readHostAdministrationRequest(call.method, call.params);
        REQUIRE(decoded);
        CHECK(decoded->operation == request.operation);
        CHECK(decoded->daemonInstanceID == request.daemonInstanceID);
        if (request.operation == Sync::ManagementOperation::DisconnectClient)
            CHECK(decoded->clientID == request.clientID);
        if (request.operation == Sync::ManagementOperation::RevokeGrant || request.operation == Sync::ManagementOperation::RevokeToken)
            CHECK(decoded->identity == request.identity);
        if (request.operation == Sync::ManagementOperation::IssueToken) {
            CHECK(decoded->label == request.label);
            CHECK(decoded->scopes == request.scopes);
            CHECK(decoded->expiryUnix == request.expiryUnix);
        }
    }
    CHECK(hostAdministrationMethod(Sync::ManagementOperation::TerminateSession).empty());
    CHECK(hostAdministrationRequest(intent(Sync::ManagementOperation::TerminateSession), "management.7").empty());
}

TEST(administrationRequestsRefuseMissingNoncanonicalOrInvalidAuthorityInputs)
{
    for (const auto operation : operations) {
        auto request = intent(operation);
        request.daemonInstanceID = 0;
        CHECK(hostAdministrationRequest(request, "management.7").empty());
        request = intent(operation);
        const auto call = decodeRequest(hostAdministrationRequest(request, "management.7"), "");
        for (const auto* invalid : { "0", "01", "-1", "+1", "18446744073709551616" })
            CHECK(!readHostAdministrationRequest(call.method, replacing(call.params, "daemonInstance", Value::string(invalid))));
        CHECK(!readHostAdministrationRequest(call.method, replacing(call.params, "daemonInstance", Value::number(1))));
    }
    auto issue = intent(Sync::ManagementOperation::IssueToken);
    issue.scopes = {};
    CHECK(hostAdministrationRequest(issue, "management.7").empty());
    issue = intent(Sync::ManagementOperation::IssueToken);
    issue.label = "bad\nlabel";
    CHECK(hostAdministrationRequest(issue, "management.7").empty());
    issue = intent(Sync::ManagementOperation::IssueToken);
    issue.expiryUnix = 0;
    CHECK(hostAdministrationRequest(issue, "management.7").empty());
    auto disconnect = intent(Sync::ManagementOperation::DisconnectClient);
    disconnect.clientID = 0;
    CHECK(hostAdministrationRequest(disconnect, "management.7").empty());
    const auto call = decodeRequest(hostAdministrationRequest(intent(Sync::ManagementOperation::IssueToken), "management.7"), "");
    CHECK(!readHostAdministrationRequest(call.method, replacing(call.params, "scopes", Value::number(128))));
}

TEST(administrationSnapshotsPreserveEveryOwnedMetadataFieldWithoutBearerSecrets)
{
    const auto request = intent(Sync::ManagementOperation::List);
    auto reply = outcome(request);
    const auto last = std::numeric_limits<std::uint64_t>::max();
    reply.clients.push_back({ 6, 1, last, "Agent", "client", "QUIC", "test-only principal" });
    reply.grants.push_back({ "grant", "capability", "Agent", "principal", "Session", 1, last, "approver" });
    reply.tokens.push_back({ "fingerprint-only", "label", { Sync::Scope::Manage }, last, last - 1, last - 2, "last user" });
    const auto text = response(request, reply);
    CHECK(text.find("issuedToken") == std::string::npos);
    const auto decoded = readHostAdministrationResult(text, "management.7", request);
    REQUIRE(decoded);
    CHECK(decoded->requestID == request.requestID);
    CHECK(decoded->daemonInstanceID == request.daemonInstanceID);
    CHECK(decoded->status == Sync::ManagementStatus::Success);
    REQUIRE(decoded->clients.size() == 1);
    const auto& client = decoded->clients.front();
    CHECK(client.clientID == 6 && client.sessionID == 1 && client.attachedUnix == last);
    CHECK(client.principal == "Agent" && client.label == "client" && client.transport == "QUIC" && client.identity == "test-only principal");
    REQUIRE(decoded->grants.size() == 1);
    const auto& grant = decoded->grants.front();
    CHECK(grant.identifier == "grant" && grant.capability == "capability" && grant.origin == "Agent");
    CHECK(grant.identity == "principal" && grant.lifetime == "Session" && grant.sessionID == 1);
    CHECK(grant.grantedUnix == last && grant.grantedBy == "approver");
    REQUIRE(decoded->tokens.size() == 1);
    const auto& token = decoded->tokens.front();
    CHECK(token.fingerprint == "fingerprint-only" && token.label == "label" && token.scopes == Sync::ScopeSet { Sync::Scope::Manage });
    CHECK(token.expiryUnix == last && token.createdUnix == last - 1 && token.lastUsedUnix == last - 2 && token.lastUsedBy == "last user");
    CHECK(decoded->issuedToken.empty());
}

TEST(administrationResultsMatchTheCallMethodIncarnationAndOriginalSelector)
{
    for (const auto operation : operations) {
        const auto request = intent(operation);
        const auto original = json(response(request, outcome(request)));
        CHECK(!readHostAdministrationResult(Codec::JSON::serialize(replacing(original, "id", Value::string("another call"))), "management.7", request));
        for (const auto& changed : {
                 replacingResult(original, "method", Value::string("host.unrelated")),
                 replacingResult(original, "daemonInstance", Value::string("1")),
             }) {
            const auto decoded = readHostAdministrationResult(Codec::JSON::serialize(changed), "management.7", request);
            REQUIRE(decoded);
            CHECK(decoded->status == Sync::ManagementStatus::Failed);
            CHECK(decoded->issuedToken.empty());
        }
        const char* selector = operation == Sync::ManagementOperation::DisconnectClient ? "client"
            : operation == Sync::ManagementOperation::IssueToken                        ? "label"
                                                                                        : "identity";
        if (operation != Sync::ManagementOperation::List) {
            const auto changed = replacingResult(original, selector, operation == Sync::ManagementOperation::DisconnectClient ? Value::number(14) : Value::string("different"));
            const auto decoded = readHostAdministrationResult(Codec::JSON::serialize(changed), "management.7", request);
            REQUIRE(decoded);
            CHECK(decoded->status == Sync::ManagementStatus::Failed);
            CHECK(decoded->issuedToken.empty());
        }
    }
}

TEST(onlyAnExplicitSuccessfulTokenIssueCarriesABearerSecret)
{
    const auto id = Value::string("management.7");
    for (const auto operation : operations) {
        const auto request = intent(operation);
        for (const auto status : { Sync::ManagementStatus::Success, Sync::ManagementStatus::Refused, Sync::ManagementStatus::Failed }) {
            auto reply = outcome(request);
            reply.status = status;
            reply.issuedToken = "test-only-not-a-real-token";
            const auto encoded = hostAdministrationResponse(request, reply, &id);
            CHECK(encoded.has_value() == (operation == Sync::ManagementOperation::IssueToken && status == Sync::ManagementStatus::Success));
        }
    }
    const auto request = intent(Sync::ManagementOperation::List);
    const auto original = json(response(request, outcome(request)));
    const auto contaminated = replacingResult(original, "issuedToken", Value::string("test-only-not-a-real-token"));
    const auto decoded = readHostAdministrationResult(Codec::JSON::serialize(contaminated), "management.7", request);
    REQUIRE(decoded);
    CHECK(decoded->status == Sync::ManagementStatus::Failed);
    CHECK(decoded->issuedToken.empty());
    CHECK(decoded->clients.empty() && decoded->grants.empty() && decoded->tokens.empty());
}

TEST(administrationCollectionsStringsAndCompleteResponsesAreBounded)
{
    const auto request = intent(Sync::ManagementOperation::List);
    const auto id = Value::string("management.7");
    CHECK(hostAdministrationRequest(request, "").empty());
    CHECK(hostAdministrationRequest(request, std::string(Protocol::Sync::maxFrameBodyBytes + 1, 'x')).empty());
    CHECK(hostAdministrationRequest(request, std::string(Protocol::Sync::maxFrameBodyBytes / 2, '"')).empty());
    auto reply = outcome(request);
    reply.clients.resize(maxAdministrationRecords + 1);
    CHECK(!hostAdministrationResponse(request, reply, &id));
    reply = outcome(request);
    reply.detail.assign(maxAdministrationStringBytes + 1, 'x');
    CHECK(!hostAdministrationResponse(request, reply, &id));
    reply = outcome(request);
    reply.clients.resize(maxAdministrationRecords);
    for (auto& client : reply.clients)
        client.label.assign(maxAdministrationStringBytes, 'x');
    CHECK(!hostAdministrationResponse(request, reply, &id));
    CHECK(!readHostAdministrationResult(std::string(maxAdministrationResultBytes + 1, ' '), "management.7", request));
    const auto original = json(response(request, outcome(request)));
    auto tooMany = Value::array();
    for (std::size_t index = 0; index <= maxAdministrationRecords; ++index)
        tooMany.push(Value::object());
    const auto decoded = readHostAdministrationResult(Codec::JSON::serialize(replacingResult(original, "clients", std::move(tooMany))), "management.7", request);
    REQUIRE(decoded);
    CHECK(decoded->status == Sync::ManagementStatus::Failed);
    CHECK(decoded->clients.empty());
}

TEST(administrationRegistryPreservesManageScopeForEveryExistingCallerRoleAndOrigin)
{
    using Protocol::Sync::Role;
    for (const auto operation : operations) {
        const auto* method = findSyncMethod(hostAdministrationMethod(operation));
        REQUIRE(method);
        CHECK(method->target == CallTarget::Host);
        CHECK(method->scope == Sync::Scope::Manage);
        CHECK(method->origin == CallOrigin::Any);
        const auto call = decodeRequest(hostAdministrationRequest(intent(operation), "management.7"), "");
        for (const auto role : { Role::Owner, Role::Driver, Role::Viewer, Role::Agent }) {
            for (const auto origin : { CapabilityOrigin::Local, CapabilityOrigin::Remote, CapabilityOrigin::Agent }) {
                CHECK(!admitCall(method, call.params, CallerAuthority { role, { Sync::Scope::Manage }, origin }));
                const auto refused = admitCall(method, call.params, CallerAuthority { role, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control }, origin });
                REQUIRE(refused);
                CHECK(refused->code == ErrorCode::Forbidden);
            }
        }
    }
}

TEST(administrationMutationAdmissionBoundsEveryOutcomeBeforeEffects)
{
    const auto id = Value::string(std::string(300, '\x01'));
    for (const auto operation : operations) {
        auto request = intent(operation);
        request.daemonInstanceID = 0;
        CHECK(isValidHostAdministrationIntent(request));
        CHECK(!hostAdministrationMutationAnswerBytes(request, &id));
        request = intent(operation);
        const auto bound = hostAdministrationMutationAnswerBytes(request, &id);
        CHECK(bound.has_value() == (operation != Sync::ManagementOperation::List));
        if (!bound)
            continue;
        for (const auto status : { Sync::ManagementStatus::Success, Sync::ManagementStatus::Refused, Sync::ManagementStatus::NotFound,
                 Sync::ManagementStatus::Invalid, Sync::ManagementStatus::Failed }) {
            auto reply = outcome(request);
            reply.status = status;
            reply.detail.assign(maxAdministrationStringBytes, '\x01');
            reply.issuedToken.clear();
            if (operation == Sync::ManagementOperation::IssueToken && status == Sync::ManagementStatus::Success)
                reply.issuedToken.assign(maxAdministrationStringBytes, '\x01');
            const auto answer = hostAdministrationResponse(request, reply, &id);
            REQUIRE(answer);
            CHECK(Codec::JSON::serialize(*answer).size() <= *bound);
        }
    }
    CHECK(!isValidHostAdministrationIntent(intent(Sync::ManagementOperation::TerminateSession)));
    auto invalid = intent(Sync::ManagementOperation::DisconnectClient);
    invalid.clientID = 0;
    CHECK(!isValidHostAdministrationIntent(invalid));
    CHECK(!hostAdministrationMutationAnswerBytes(invalid, &id));
}

TEST(administrationMutationCapacityHasExactInlineAndQueueBoundaries)
{
    constexpr std::size_t inlineTextLimit = Protocol::Sync::maxFrameBodyBytes - 1;
    constexpr std::size_t queueLimit = 16 * 1024 * 1024;
    const auto request = intent(Sync::ManagementOperation::RevokeToken);
    const auto shortID = Value::string("x");
    const auto shortBound = hostAdministrationMutationAnswerBytes(request, &shortID);
    REQUIRE(shortBound);
    REQUIRE(*shortBound < inlineTextLimit);
    const auto boundaryID = Value::string(std::string(1 + inlineTextLimit - *shortBound, 'x'));
    const auto bound = hostAdministrationMutationAnswerBytes(request, &boundaryID);
    REQUIRE(bound);
    CHECK(*bound == inlineTextLimit);
    CHECK(!hostAdministrationRequest(request, boundaryID.asString()).empty());
    CHECK(hostAdministrationMutationHasCapacity(*bound, 0, queueLimit));
    CHECK(hostAdministrationMutationHasCapacity(*bound, queueLimit - *bound, queueLimit));
    CHECK(!hostAdministrationMutationHasCapacity(*bound, queueLimit - *bound + 1, queueLimit));
    const auto largerID = Value::string(boundaryID.asString() + "x");
    const auto largerBound = hostAdministrationMutationAnswerBytes(request, &largerID);
    REQUIRE(largerBound);
    CHECK(*largerBound == inlineTextLimit + 1);
    CHECK(!hostAdministrationRequest(request, largerID.asString()).empty());
    CHECK(!hostAdministrationMutationHasCapacity(*largerBound, 0, queueLimit));
    CHECK(!hostAdministrationMutationHasCapacity(1, std::numeric_limits<std::size_t>::max(), queueLimit));
    CHECK(!hostAdministrationMutationHasCapacity(1, 0, 0));
}

int main(int argc, char** argv) { return UbiquittyTest::runRegisteredCases(argc, argv); }
