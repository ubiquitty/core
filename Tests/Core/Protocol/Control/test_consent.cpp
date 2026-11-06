// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/ControlRPC.h"

#include <array>
#include <limits>

namespace {

using namespace Ubiquitty;
using namespace Protocol::Control;
using Codec::JSON::Value;
namespace Sync = Protocol::Sync;

Value json(const char* text)
{
    bool isJSON = false;
    auto value = Codec::JSON::parse(text, isJSON);
    CHECK(isJSON);
    return value;
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

ConsentRevocation revocation(Sync::ConsentRevocationKind action)
{
    ConsentRevocation value { .sessionID = UINT32_MAX, .action = action, .attachmentID = {}, .identity = {} };
    if (action == Sync::ConsentRevocationKind::DisconnectNow)
        value.attachmentID = { 0xfedcba9876543210, 0x123456789abcdef0 };
    if (action == Sync::ConsentRevocationKind::StopAlways || action == Sync::ConsentRevocationKind::SessionGrant)
        value.identity = "token:test-only-verified-principal";
    return value;
}

constexpr std::array kinds { Sync::ConsentKind::Attach, Sync::ConsentKind::Terminate, Sync::ConsentKind::Capability, Sync::ConsentKind::Structure };
constexpr std::array decisions { Sync::ConsentChoice::Deny, Sync::ConsentChoice::AllowOnce, Sync::ConsentChoice::AllowSession, Sync::ConsentChoice::AllowAlways };
constexpr std::array actions { Sync::ConsentRevocationKind::DisconnectNow, Sync::ConsentRevocationKind::StopAlways, Sync::ConsentRevocationKind::SessionGrant, Sync::ConsentRevocationKind::DisconnectAll };

} // namespace

TEST(consentResponseCallsKeepExactKindAndQuestion)
{
    for (const auto kind : kinds) {
        for (const auto decision : decisions) {
            const ConsentResponse response { UINT32_MAX, kind, decision };
            const auto text = consentResponseRequest(response, "consent.response.9");
            if (kind == Sync::ConsentKind::Structure && (decision == Sync::ConsentChoice::AllowSession || decision == Sync::ConsentChoice::AllowAlways)) {
                CHECK(text.empty());
                continue;
            }
            const auto call = decodeRequest(text, "");
            REQUIRE(call.outcome == RequestOutcome::Dispatch);
            CHECK(call.method == "consent.respond");
            CHECK(call.id.isString() && call.id.asString() == "consent.response.9");
            CHECK(readConsentResponse(call.params) == response);
            CHECK(call.params.members().size() == 3);
            const auto result = rpcResult(call.idOrNull(), Value::object());
            CHECK(result.stringOr("id") == "consent.response.9");
            REQUIRE(result.find("result"));
            CHECK(result.find("result")->members().empty());
        }
    }
}

TEST(consentResponseRefusesAmbiguousOrStandingStructureDecisions)
{
    const auto valid = json(R"({"request":7,"kind":"attach","decision":"once"})");
    for (const auto* text : {
             R"({"kind":"attach","decision":"once"})", R"({"request":7,"decision":"once"})", R"({"request":7,"kind":"attach"})",
             R"({"request":7,"kind":"other","decision":"once"})", R"({"request":7,"kind":"attach","decision":"allow"})",
             R"({"request":7,"request":7,"kind":"attach","decision":"once"})",
             R"({"request":7,"kind":"attach","kind":"attach","decision":"once"})",
             R"({"request":7,"kind":"attach","decision":"once","decision":"once"})",
             R"({"request":7,"kind":"structure","decision":"session"})", R"({"request":7,"kind":"structure","decision":"always"})" })
        CHECK(!readConsentResponse(json(text)));
    for (const auto number : { 0.0, -1.0, 1.5, 4294967296.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
        CHECK(!readConsentResponse(replacing(valid, "request", Value::number(number))));
    for (const auto* field : { "allow", "session", "capability", "origin", "identity", "agentLabel", "params" })
        CHECK(!readConsentResponse(replacing(valid, field, Value::string("not authority"))));
    CHECK(!readConsentResponse(replacing(valid, "request", Value::string("7"))));
    CHECK(!readConsentResponse(replacing(valid, "kind", Value::number(0))));
    CHECK(!readConsentResponse(replacing(valid, "decision", Value::boolean(true))));
    CHECK(consentResponseRequest({ 0, Sync::ConsentKind::Attach, Sync::ConsentChoice::Deny }, "c").empty());
    CHECK(consentResponseRequest({ 7, static_cast<Sync::ConsentKind>(4), Sync::ConsentChoice::Deny }, "c").empty());
    CHECK(consentResponseRequest({ 7, Sync::ConsentKind::Attach, static_cast<Sync::ConsentChoice>(5) }, "c").empty());
    // A recipient that cannot show a question declines it as unshown, for any
    // kind, structure included.
    for (const auto kind : { "attach", "terminate", "capability", "structure" }) {
        const auto unshown = readConsentResponse(replacing(replacing(valid, "kind", Value::string(kind)), "decision", Value::string("unshown")));
        CHECK(unshown && unshown->decision == Sync::ConsentChoice::Unshown);
    }
    CHECK(!consentResponseRequest({ 7, Sync::ConsentKind::Structure, Sync::ConsentChoice::Unshown }, "c").empty());
}

TEST(consentRevocationsUseOnlyTheirStableActionSelectors)
{
    constexpr std::array expected { "disconnect", "forgetAlways", "forgetSession", "disconnectAgents" };
    for (std::size_t index = 0; index < actions.size(); ++index) {
        const auto intent = revocation(actions[index]);
        const auto call = decodeRequest(consentRevocationRequest(intent, "consent.revoke.4"), "");
        REQUIRE(call.outcome == RequestOutcome::Dispatch);
        CHECK(call.method == "consent.revoke");
        CHECK(call.id.isString() && call.id.asString() == "consent.revoke.4");
        CHECK(call.params.stringOr("session") == "session:4294967295");
        CHECK(call.params.stringOr("action") == expected[index]);
        CHECK(readConsentRevocation(call.params) == intent);
        CHECK(!call.params.find("client"));
        CHECK(!call.params.find("daemonInstance"));
        if (intent.action == Sync::ConsentRevocationKind::DisconnectNow) {
            CHECK(call.params.stringOr("attachment") == "123456789abcdef0fedcba9876543210");
            CHECK(!call.params.find("identity"));
        } else if (intent.action == Sync::ConsentRevocationKind::DisconnectAll)
            CHECK(call.params.members().size() == 2);
        else {
            CHECK(call.params.stringOr("identity") == intent.identity);
            CHECK(!call.params.find("attachment"));
        }
    }
}

TEST(consentRevocationsRefuseMixedEmptyAndUnstableTargets)
{
    for (const auto action : actions) {
        const auto call = decodeRequest(consentRevocationRequest(revocation(action), "c"), "");
        for (const auto* invalid : { "9", "here", "work", "pane:9", "session:0", "session:09", "session:+9", "session:9 ", "session:4294967296" })
            CHECK(!readConsentRevocation(replacing(call.params, "session", Value::string(invalid))));
        CHECK(!readConsentRevocation(replacing(call.params, "session", Value::number(9))));
        CHECK(!readConsentRevocation(replacing(call.params, "action", Value::string("other"))));
        for (const auto* field : { "client", "label", "daemonInstance" })
            CHECK(!readConsentRevocation(replacing(call.params, field, Value::number(7))));
        auto duplicate = call.params;
        duplicate.set("session", Value::string("session:4294967295"));
        CHECK(!readConsentRevocation(duplicate));
        auto contradictory = revocation(action);
        if (action == Sync::ConsentRevocationKind::DisconnectNow || action == Sync::ConsentRevocationKind::DisconnectAll)
            contradictory.identity = "not a selector for this action";
        else
            contradictory.attachmentID = { 1, 2 };
        CHECK(consentRevocationRequest(contradictory, "c").empty());
    }
    for (const auto* text : {
             R"({"session":"session:9","action":"disconnect"})",
             R"({"session":"session:9","action":"disconnect","attachment":"00000000000000000000000000000000"})",
             R"({"session":"session:9","action":"disconnect","attachment":"0000000000000000000000000000000A"})",
             R"({"session":"session:9","action":"disconnect","client":7})",
             R"({"session":"session:9","action":"forgetAlways"})", R"({"session":"session:9","action":"forgetSession","identity":""})",
             R"({"session":"session:9","action":"forgetAlways","identity":"x","attachment":"00000000000000000000000000000001"})",
             R"({"session":"session:9","action":"disconnectAgents","identity":"x"})",
             R"({"session":"session:9","action":"disconnectAgents","attachment":"00000000000000000000000000000001"})" })
        CHECK(!readConsentRevocation(json(text)));
    auto identity = revocation(Sync::ConsentRevocationKind::StopAlways);
    for (const auto& invalid : { std::string("nul\0key", 7), std::string("\xff", 1), std::string(Sync::maxConsentIdentityBytes + 1, 'x') }) {
        identity.identity = invalid;
        CHECK(consentRevocationRequest(identity, "c").empty());
        CHECK(!readConsentRevocation(replacing(json(R"({"session":"session:9","action":"forgetAlways","identity":"x"})"), "identity", Value::string(invalid))));
    }
    identity = revocation(Sync::ConsentRevocationKind::DisconnectNow);
    identity.attachmentID = {};
    CHECK(consentRevocationRequest(identity, "c").empty());
    identity = revocation(Sync::ConsentRevocationKind::DisconnectAll);
    identity.sessionID = 0;
    CHECK(consentRevocationRequest(identity, "c").empty());
    identity = revocation(static_cast<Sync::ConsentRevocationKind>(4));
    CHECK(consentRevocationRequest(identity, "c").empty());
}

TEST(consentCallsBoundCorrelationUTF8AndSerializedBytes)
{
    const ConsentResponse response { 7, Sync::ConsentKind::Attach, Sync::ConsentChoice::Deny };
    const auto revoked = revocation(Sync::ConsentRevocationKind::DisconnectAll);
    const auto responseCall = [&](std::string_view id) { return consentResponseRequest(response, id); };
    const auto revokeCall = [&](std::string_view id) { return consentRevocationRequest(revoked, id); };
    const auto checkBoundary = [&](auto build) {
        CHECK(build("").empty());
        CHECK(build(std::string("\xff", 1)).empty());
        CHECK(build(std::string(Sync::maxFrameBodyBytes + 1, 'i')).empty());
        const auto overhead = build("i").size() - 1;
        REQUIRE(overhead < Sync::maxFrameBodyBytes);
        const auto bytes = Sync::maxFrameBodyBytes - overhead;
        CHECK(build(std::string(bytes, 'i')).size() == Sync::maxFrameBodyBytes);
        CHECK(build(std::string(bytes + 1, 'i')).empty());
        const auto escaped = bytes / 6;
        CHECK(!build(std::string(escaped, '\1')).empty());
        CHECK(build(std::string(escaped + 1, '\1')).empty());
    };
    checkBoundary(responseCall);
    checkBoundary(revokeCall);
    auto identity = revocation(Sync::ConsentRevocationKind::StopAlways);
    identity.identity = "x";
    const auto overhead = consentRevocationRequest(identity, "c").size() - 1;
    identity.identity = std::string(Sync::maxFrameBodyBytes - overhead, 'x');
    CHECK(identity.identity.size() <= Sync::maxConsentIdentityBytes);
    CHECK(consentRevocationRequest(identity, "c").size() == Sync::maxFrameBodyBytes);
    identity.identity.push_back('x');
    CHECK(consentRevocationRequest(identity, "c").empty());
}

TEST(consentStableAttachmentIDsHaveOneExactPublicSpelling)
{
    const Sync::ID128 identity { 0xfedcba9876543210, 0x123456789abcdef0 };
    const auto text = Sync::formatID128(identity);
    CHECK(text == "123456789abcdef0fedcba9876543210");
    CHECK(Sync::parseID128(text) == identity);
    for (std::size_t length = 0; length < text.size(); ++length)
        CHECK(!Sync::parseID128(std::string_view(text).substr(0, length)));
    CHECK(!Sync::parseID128(text + "0"));
    CHECK(!Sync::parseID128("123456789ABCDEF0fedcba9876543210"));
    CHECK(!Sync::parseID128("123456789abcdef0fedcba987654321g"));
    CHECK(!Sync::parseID128(std::string(32, '0')));
    CHECK(Sync::formatID128({}).empty());
    const Sync::ID128 last { UINT64_MAX, UINT64_MAX };
    CHECK(Sync::parseID128(Sync::formatID128(last)) == last);
}

TEST(consentRegistryPreservesHumanObserveAndRefusesOldAliases)
{
    CHECK(!findMethod("consent.answer"));
    CHECK(!findSyncMethod("consent.answer"));
    CHECK(!findSyncMethod("CONSENT_RESPONSE"));
    CHECK(!findSyncMethod("REVOKE_CONSENT"));
    const auto response = decodeRequest(consentResponseRequest({ 7, Sync::ConsentKind::Attach, Sync::ConsentChoice::Deny }, "c"), "");
    const auto revoke = decodeRequest(consentRevocationRequest(revocation(Sync::ConsentRevocationKind::DisconnectAll), "c"), "");
    for (const auto& call : { response, revoke }) {
        const auto* method = findSyncMethod(call.method);
        REQUIRE(method);
        CHECK(method->target == CallTarget::Host);
        CHECK(method->scope == Sync::Scope::Observe);
        CHECK(method->origin == CallOrigin::Any);
        CHECK(method->controlMethod.empty() && method->commandLineVerb.empty() && method->mcpTool.empty());
        CHECK(method->principals.contains(Principal::Person));
        CHECK(!method->principals.contains(Principal::Agent));
        CHECK(!checkSchema(parseRegistryJSON(method->parametersSchema)));
        CHECK(!checkSchema(parseRegistryJSON(method->resultSchema)));
        const auto example = parseRegistryJSON(method->example);
        REQUIRE(example.find("params"));
        REQUIRE(example.find("result"));
        CHECK(!validate(*example.find("params"), parseRegistryJSON(method->parametersSchema)));
        CHECK(!validate(*example.find("result"), parseRegistryJSON(method->resultSchema)));
        for (const auto role : { Sync::Role::Owner, Sync::Role::Driver, Sync::Role::Viewer })
            CHECK(!admitCall(method, call.params, { role, { Sync::Scope::Observe }, CapabilityOrigin::Remote }));
        CHECK(admitCall(method, call.params, { Sync::Role::Agent, { Sync::Scope::Observe }, CapabilityOrigin::Local }));
        CHECK(admitCall(method, call.params, { Sync::Role::Owner, {}, CapabilityOrigin::Local }));
        auto duplicate = call.params;
        duplicate.set("session", Value::string("session:9"));
        CHECK(admitCall(method, duplicate, { Sync::Role::Owner, { Sync::Scope::Observe }, CapabilityOrigin::Local }));
    }
}

UBIQUITTY_TEST_MAIN()
