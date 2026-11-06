// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_method_registry — the one table of operations is internally sound: every
// schema parses and uses only keywords the validator enforces, every example
// satisfies both of its schemas, every name is unique on its surface, and the
// validator refuses what a schema forbids.

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Codec/JSON.h"
#include "Core/GridBounds.h"
#include "Core/Protocol/Control/FileRead.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Control/ProcessSnapshot.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Session/NameLimits.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new[](size);
}

namespace {

using Ubiquitty::Codec::JSON::Value;
using Ubiquitty::Protocol::Control::checkSchema;
using Ubiquitty::Protocol::Control::findCommandLineVerb;
using Ubiquitty::Protocol::Control::findControlMethod;
using Ubiquitty::Protocol::Control::findMCPTool;
using Ubiquitty::Protocol::Control::MethodDescriptor;
using Ubiquitty::Protocol::Control::methodRegistry;
using Ubiquitty::Protocol::Control::validate;

/// Parses registry text, reporting a failure rather than falling back.
bool parses(std::string_view text, Value& out)
{
    bool isParsed = false;
    out = Ubiquitty::Codec::JSON::parse(std::string(text), isParsed);
    return isParsed;
}

Value json(const char* text)
{
    bool isParsed = false;
    return Ubiquitty::Codec::JSON::parse(text, isParsed);
}

} // namespace

TEST(everyEntryIsSoundAndItsExampleSatisfiesItsSchemas)
{
    std::set<std::string_view> names;
    std::set<std::string_view> controlMethods;
    std::set<std::string_view> tools;
    std::set<std::string_view> verbs;
    for (const MethodDescriptor& method : methodRegistry()) {
        const std::string name(method.name);
        CHECK_MSG(names.insert(method.name).second, ("registry name repeated: " + name).c_str());
        CHECK_MSG(method.controlMethod.empty() || controlMethods.insert(method.controlMethod).second, name.c_str());
        CHECK_MSG(method.mcpTool.empty() || tools.insert(method.mcpTool).second, name.c_str());
        CHECK_MSG(method.commandLineVerb.empty() || verbs.insert(method.commandLineVerb).second, name.c_str());
        CHECK_MSG(!method.controlMethod.empty() || !method.mcpTool.empty() || !method.commandLineVerb.empty() || !method.syncMethod.empty(), ("reachable from no surface: " + name).c_str());
        CHECK_MSG(!method.principals.isEmpty(), ("callable by nobody: " + name).c_str());
        CHECK_MSG(!method.description.empty() && method.description.back() == '.', ("description is not a sentence: " + name).c_str());

        Value parameters;
        Value result;
        Value example;
        CHECK_MSG(parses(method.parametersSchema, parameters), ("parameter schema does not parse: " + name).c_str());
        CHECK_MSG(parses(method.resultSchema, result), ("result schema does not parse: " + name).c_str());
        CHECK_MSG(parses(method.example, example), ("example does not parse: " + name).c_str());
        if (const auto problem = checkSchema(parameters))
            CHECK_MSG(false, (name + ": " + *problem).c_str());
        if (const auto problem = checkSchema(result))
            CHECK_MSG(false, (name + ": " + *problem).c_str());
        CHECK_MSG(parameters.stringOr("type") == "object", ("parameters are not an object: " + name).c_str());

        const Value* exampleParameters = example.find("params");
        const Value* exampleResult = example.find("result");
        CHECK_MSG(exampleParameters && exampleResult, ("example lacks params or result: " + name).c_str());
        if (exampleParameters) {
            if (const auto violation = validate(*exampleParameters, parameters))
                CHECK_MSG(false, (name + " example params " + violation->path + ": " + violation->message).c_str());
        }
        if (exampleResult) {
            if (const auto violation = validate(*exampleResult, result))
                CHECK_MSG(false, (name + " example result " + violation->path + ": " + violation->message).c_str());
        }
    }
    CHECK(findControlMethod("send-text") && findControlMethod("send-text")->name == "send-text");
    CHECK(findMCPTool("terminal_run") && findMCPTool("terminal_run")->name == "terminal.run");
    // A verb with a space is the command line's subcommand, reached as one name.
    CHECK(findCommandLineVerb("run") && findCommandLineVerb("run")->name == "ubq.run");
    CHECK(findCommandLineVerb("api call") && findCommandLineVerb("api call")->name == "ubq.api.call");
}

TEST(paneClearRequiresDriveAndAnExactCommand)
{
    using namespace Ubiquitty::Protocol;
    const auto* method = Control::findSyncMethod("pane.clear");
    REQUIRE(method);
    CHECK(method->target == Control::CallTarget::Pane);
    for (const auto role : Sync::everyRole) {
        Control::CallerAuthority caller { .role = role, .scopes = { Sync::Scope::Drive }, .origin = Ubiquitty::CapabilityOrigin::Remote };
        for (const char* command : { "buffer", "scrollback", "screen", "previous-mark" }) {
            const auto parameters = json((std::string(R"({"pane":1,"command":")") + command + R"("})").c_str());
            CHECK(Control::admitCall(method, parameters, caller).has_value() == (role == Sync::Role::Viewer));
        }
        caller.scopes = { Sync::Scope::Observe, Sync::Scope::Control, Sync::Scope::RegionRead };
        CHECK(Control::admitCall(method, json(R"({"pane":1,"command":"buffer"})"), caller).has_value());
    }
    const Control::CallerAuthority caller { .role = Sync::Role::Owner, .scopes = { Sync::Scope::Drive }, .origin = Ubiquitty::CapabilityOrigin::Remote };
    for (const char* invalid : { "{}", R"({"pane":0,"command":"buffer"})", R"({"pane":1.5,"command":"buffer"})", R"({"pane":1,"command":0})", R"({"pane":1,"command":"all"})", R"({"pane":1,"command":"buffer","all":true})" })
        CHECK(Control::admitCall(method, json(invalid), caller).has_value());
}

TEST(paneReclaimIsLocalHumanObserveOnlyAndExposesNoFreshSocketSurface)
{
    using namespace Ubiquitty::Protocol;
    const auto* method = Control::findSyncMethod("pane.reclaim");
    REQUIRE(method);
    CHECK(method->target == Control::CallTarget::Pane);
    CHECK(method->origin == Control::CallOrigin::Local);
    CHECK(method->scope == Sync::Scope::Observe);
    CHECK(method->controlMethod.empty() && method->commandLineVerb.empty() && method->mcpTool.empty());
    const auto parameters = json(R"({"pane":1,"daemonInstance":"17"})");
    for (const auto origin : { Ubiquitty::CapabilityOrigin::Local, Ubiquitty::CapabilityOrigin::Agent, Ubiquitty::CapabilityOrigin::Remote }) {
        for (const auto role : Sync::everyRole) {
            Control::CallerAuthority caller { .role = role, .scopes = { Sync::Scope::Observe }, .origin = origin };
            const bool isAllowed = origin == Ubiquitty::CapabilityOrigin::Local && role != Sync::Role::Agent;
            CHECK(Control::admitCall(method, parameters, caller).has_value() != isAllowed);
            caller.scopes = {};
            CHECK(Control::admitCall(method, parameters, caller).has_value());
        }
    }
}

TEST(clipboardServingIsAnExactSelfOnlyOptInForEveryAttachmentRole)
{
    using namespace Ubiquitty::Protocol;
    const auto* method = Control::findSyncMethod("client.clipboardServing");
    REQUIRE(method);
    CHECK(method->target == Control::CallTarget::Connection && method->scope == Sync::Scope::Observe);
    for (const auto role : Sync::everyRole) {
        Control::CallerAuthority caller { .role = role, .scopes = { Sync::Scope::Observe }, .origin = Ubiquitty::CapabilityOrigin::Remote };
        for (const auto* parameters : { R"({"enabled":true})", R"({"enabled":false})" })
            CHECK(!Control::admitCall(method, json(parameters), caller));
        for (const auto* parameters : { R"({})", R"({"enabled":1})", R"({"enabled":"true"})", R"({"enabled":null})", R"({"enabled":true,"client":7})", R"({"enabled":true,"pane":1})" })
            CHECK(Control::admitCall(method, json(parameters), caller).has_value());
        caller.scopes = {};
        CHECK(Control::admitCall(method, json(R"({"enabled":true})"), caller).has_value());
    }
}

TEST(sessionTerminationRequiresControlAndOneExactTarget)
{
    using namespace Ubiquitty::Protocol;
    const auto* method = Control::findSyncMethod("session.terminate");
    REQUIRE(method);
    CHECK(method->target == Control::CallTarget::Host && method->scope == Sync::Scope::Control);
    for (const auto role : Sync::everyRole) {
        Control::CallerAuthority caller { .role = role, .scopes = { Sync::Scope::Control }, .origin = Ubiquitty::CapabilityOrigin::Remote };
        CHECK(!Control::admitCall(method, json(R"({"session":7})"), caller));
        caller.scopes = { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::NewSession };
        CHECK(Control::admitCall(method, json(R"({"session":7})"), caller).has_value());
    }
    const Control::CallerAuthority caller { .role = Sync::Role::Owner, .scopes = { Sync::Scope::Control }, .origin = Ubiquitty::CapabilityOrigin::Remote };
    for (const char* invalid : { "{}", R"({"session":0})", R"({"session":1.5})", R"({"session":4294967296})", R"({"session":"7"})", R"({"session":7,"all":true})" })
        CHECK(Control::admitCall(method, json(invalid), caller).has_value());
}

TEST(fileReadPreservesItsIndependentScopeForEveryAttachmentRole)
{
    using namespace Ubiquitty::Protocol;
    const auto* method = Control::findSyncMethod("file.read");
    REQUIRE(method);
    CHECK(method->target == Control::CallTarget::Pane && method->scope == Sync::Scope::FileRead);
    for (const auto role : Sync::everyRole) {
        Control::CallerAuthority caller { .role = role, .scopes = { Sync::Scope::FileRead }, .origin = Ubiquitty::CapabilityOrigin::Remote };
        CHECK(!Control::admitCall(method, json(R"({"pane":1,"path":"/srv/report.txt"})"), caller));
        caller.scopes = { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control };
        CHECK(Control::admitCall(method, json(R"({"pane":1,"path":"/srv/report.txt"})"), caller).has_value());
    }
    const Control::CallerAuthority caller { .role = Sync::Role::Owner, .scopes = { Sync::Scope::FileRead }, .origin = Ubiquitty::CapabilityOrigin::Remote };
    for (const char* invalid : { "{}", R"({"pane":0,"path":"/srv/a"})", R"({"pane":1.5,"path":"/srv/a"})", R"({"pane":1,"path":""})", R"({"pane":1,"path":"/srv/a","unconfined":true})" })
        CHECK(Control::admitCall(method, json(invalid), caller).has_value());
}

TEST(fileRepliesRequireExactCorrelationAndBoundDecodedAllocation)
{
    using namespace Ubiquitty::Protocol::Control;
    const auto answer = json(R"({"jsonrpc":"2.0","id":"file:1","result":{"pane":7,"path":"/srv/file","status":"ok","bytes":3,"data":"AP9B"}})");
    auto reply = decodeFileReadReply(answer, "file:1", 7, "/srv/file");
    REQUIRE(reply);
    CHECK(std::ranges::equal(reply->data(), std::array<std::uint8_t, 3> { 0, 255, 'A' }));
    CHECK(!decodeFileReadReply(answer, "file:1", 8, "/srv/file"));
    CHECK(!decodeFileReadReply(answer, "file:1", 7, "/srv/other"));
    {
        UbiquittyTest::ScopedNothrowAllocationRefusal refusal(3);
        const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
        const auto unrelated = decodeFileReadReply(answer, "file:2", 7, "/srv/file");
        CHECK(!unrelated && unrelated.error() == FileReadFailure::Unrelated);
        CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() == before);
        const auto refused = decodeFileReadReply(answer, "file:1", 7, "/srv/file");
        CHECK(!refused && refused.error() == FileReadFailure::AllocationRefused);
        CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount() == before + 1);
    }
    for (const auto* body : {
             R"({"pane":7,"path":"/srv/file","status":"denied","bytes":3,"data":"YWJj"})",
             R"({"pane":7,"path":"/srv/file","status":"ok","bytes":4,"data":"YWJj"})",
             R"({"pane":7,"path":"/srv/file","status":"ok","bytes":3,"data":"A?9B"})",
             R"({"pane":7,"path":"/srv/file","status":"ok","bytes":4194305,"data":""})",
             R"({"pane":7,"path":"/srv/file","status":"ok","bytes":0,"data":"","data":""})" }) {
        const auto bad = json((std::string(R"({"jsonrpc":"2.0","id":"file:1","result":)") + body + "}").c_str());
        const auto before = UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount();
        CHECK(!decodeFileReadReply(bad, "file:1", 7, "/srv/file"));
        if (bad.find("result")->stringOr("data") != "A?9B")
            CHECK(UbiquittyTest::ScopedNothrowAllocationRefusal::attemptCount() == before);
    }
    CHECK(fileReadRequest(7, std::string_view("/srv/a\0b", 8), "file:1").empty());
    CHECK(fileReadRequest(7, std::string(maxReadFilePathBytes + 1, 'x'), "file:1").empty());
    const auto denied = decodeFileReadReply(json(R"({"jsonrpc":"2.0","id":"file:1","error":{"code":-32003,"message":"forbidden"}})"), "file:1", 7, "/srv/file");
    REQUIRE(denied);
    CHECK(denied->status == FileReadStatus::Denied && denied->data().empty());
    for (const auto* error : {
             R"({"code":4294967296,"message":"wrong integer domain"})",
             R"({"code":-32003,"message":"forbidden","extra":0})",
             R"({"code":-32003,"message":"forbidden","data":0,"extra":0})" }) {
        const auto invalid = json((std::string(R"({"jsonrpc":"2.0","id":"file:1","error":)") + error + "}").c_str());
        CHECK(!decodeFileReadReply(invalid, "file:1", 7, "/srv/file"));
    }
}

TEST(fileRepliesAdmitTheExactFileLimitAndAnEmptyRegularFile)
{
    using namespace Ubiquitty::Protocol::Control;
    for (const std::size_t size : { std::size_t { 0 }, maxReadFileBytes }) {
        const std::string bytes(size, '\x81');
        std::string encoded(Ubiquitty::Codec::base64EncodedLength(size), '\0');
        const auto characters = Ubiquitty::Codec::base64Encode(bytes, encoded);
        CHECK(characters && characters->size() == encoded.size());
        auto result = Value::object();
        result.set("pane", Value::number(7));
        result.set("path", Value::string("/srv/file"));
        result.set("status", Value::string("ok"));
        result.set("bytes", Value::number(static_cast<double>(size)));
        result.set("data", Value::string(std::move(encoded)));
        auto answer = Value::object();
        answer.set("jsonrpc", Value::string("2.0"));
        answer.set("id", Value::string("file:1"));
        answer.set("result", std::move(result));
        const auto reply = decodeFileReadReply(answer, "file:1", 7, "/srv/file");
        REQUIRE(reply);
        CHECK(reply->data().size() == size);
        CHECK(std::ranges::all_of(reply->data(), [](auto byte) { return byte == 0x81; }));
    }
}

TEST(windowCloseRequiresAnExactRevisionString)
{
    const auto* method = Ubiquitty::Protocol::Control::findSyncMethod("window.close");
    REQUIRE(method);
    const auto schema = Ubiquitty::Protocol::Control::parseRegistryJSON(method->parametersSchema);
    CHECK(validate(json(R"({"window":1})"), schema).has_value());
    CHECK(validate(json(R"({"window":1,"expectedRevision":7})"), schema).has_value());
    CHECK(validate(json(R"({"window":1,"expectedRevision":""})"), schema).has_value());
    CHECK(validate(json(R"({"window":1,"expectedRevision":"123456789012345678901"})"), schema).has_value());
    CHECK(!validate(json(R"({"window":1,"expectedRevision":"18446744073709551615"})"), schema).has_value());
}

// Whether every integer a schema accepts is bounded above, so no value the
// validator passes overflows the type the code narrows it into.
void checkIntegersAreBounded(const Value& schema, const std::string& where)
{
    if (schema.stringOr("type") == "integer")
        CHECK_MSG(schema.find("maximum") != nullptr, ("an integer with no maximum: " + where).c_str());
    if (const Value* properties = schema.find("properties")) {
        for (const auto& [name, property] : properties->members())
            checkIntegersAreBounded(property, where + "/" + name);
    }
    if (const Value* items = schema.find("items"))
        checkIntegersAreBounded(*items, where + "/items");
}

TEST(everyIntegerParameterHasAMaximum)
{
    for (const MethodDescriptor& method : methodRegistry()) {
        Value parameters;
        if (parses(method.parametersSchema, parameters))
            checkIntegersAreBounded(parameters, std::string(method.name));
    }
}

TEST(eachSurfaceResolvesItsOwnNamesFirst)
{
    using Ubiquitty::Protocol::Control::findMethodFromSurface;
    using Ubiquitty::Protocol::Control::Surface;
    // The command line's `ls` and `split` are not the control socket's.
    CHECK(findMethodFromSurface("ls", Surface::CommandLine) && findMethodFromSurface("ls", Surface::CommandLine)->name == "ubq.ls");
    CHECK(findMethodFromSurface("split", Surface::CommandLine) && findMethodFromSurface("split", Surface::CommandLine)->name == "ubq.split");
    CHECK(findMethodFromSurface("split", Surface::ControlSocket) && findMethodFromSurface("split", Surface::ControlSocket)->name == "split");
    for (const MethodDescriptor& method : methodRegistry()) {
        const std::string name(method.name);
        if (!method.commandLineVerb.empty())
            CHECK_MSG(findMethodFromSurface(method.commandLineVerb, Surface::CommandLine) == &method, ("the command line reaches another operation by " + name + "'s verb").c_str());
        if (!method.controlMethod.empty())
            CHECK_MSG(findMethodFromSurface(method.controlMethod, Surface::ControlSocket) == &method, ("the control socket reaches another operation by " + name + "'s method").c_str());
        if (!method.mcpTool.empty())
            CHECK_MSG(findMethodFromSurface(method.mcpTool, Surface::MCP) == &method, ("the MCP server reaches another operation by " + name + "'s tool").c_str());
        CHECK_MSG(findMethodFromSurface(method.name, Surface::CommandLine) != nullptr, ("a registry name reaches nothing: " + name).c_str());
    }
}

TEST(aSyncCallReachesWhatTheRegistryNamesForIt)
{
    using Ubiquitty::Protocol::Control::findMethodFromSurface;
    using Ubiquitty::Protocol::Control::findSyncMethod;
    using Ubiquitty::Protocol::Control::Surface;
    CHECK(findSyncMethod("pane.search") && findSyncMethod("pane.search")->name == "pane.search");
    CHECK(findSyncMethod("pane.capture") && findSyncMethod("pane.capture")->name == "pane.capture");
    // A control method is not a Sync call: the host serves only the column.
    CHECK(!findSyncMethod("ls"));
    CHECK(!findSyncMethod("send-text"));
    for (const MethodDescriptor& method : methodRegistry()) {
        if (!method.syncMethod.empty())
            CHECK_MSG(findMethodFromSurface(method.syncMethod, Surface::SyncCall) == &method, ("a Sync call reaches another operation by " + std::string(method.name) + "'s name").c_str());
    }
}

TEST(everyVerbThatReachesADaemonNamesTheScopesItNeeds)
{
    using Ubiquitty::Protocol::Control::describeMethod;
    using Ubiquitty::Protocol::Control::findMethod;
    using Ubiquitty::Protocol::Control::parseRegistryJSON;
    namespace Sync = Ubiquitty::Protocol::Sync;
    // These run on this machine or speak only for the operations they reach;
    // every other verb connects to a daemon, which refuses a token without
    // the scopes it needs, and a refusal names them from here.
    const std::set<std::string_view> local { "api list", "api describe", "api call", "api validate", "mcp", "completions" };
    for (const MethodDescriptor& method : methodRegistry()) {
        if (method.commandLineVerb.empty()) {
            CHECK_MSG(method.commandLineScopes.isEmpty(), ("an operation with no verb names verb scopes: " + std::string(method.name)).c_str());
            continue;
        }
        const bool isLocal = local.contains(method.commandLineVerb);
        CHECK_MSG(method.commandLineScopes.isEmpty() == isLocal, ("verb scopes do not say whether it reaches a daemon: " + std::string(method.commandLineVerb)).c_str());
        // Every verb that names a session or pane resolves it over a control
        // connection, which a token without `control` cannot open.
        const Value parameters = parseRegistryJSON(method.parametersSchema);
        const Value* properties = parameters.find("properties");
        if (properties && properties->find("session"))
            CHECK_MSG(method.commandLineScopes.contains(Sync::Scope::Control), ("a verb that resolves a session does not name control: " + std::string(method.commandLineVerb)).c_str());
    }
    const MethodDescriptor* run = findMethod("ubq.run");
    CHECK(run && run->commandLineScopes.contains(Sync::Scope::Drive) && run->commandLineScopes.contains(Sync::Scope::RegionRead));
    const MethodDescriptor* list = findMethod("ubq.ls");
    CHECK(list);
    if (!list)
        return;
    const Value described = describeMethod(*list);
    const Value* scopes = described.find("commandLineScopes");
    CHECK(scopes && scopes->isArray() && scopes->items().size() == 1 && scopes->items().front().asString() == "control");
    for (const char* describer : { "describe", "ubq.api.describe" }) {
        const MethodDescriptor* method = findMethod(describer);
        CHECK(method);
        if (!method)
            continue;
        const auto violation = validate(described, parseRegistryJSON(method->resultSchema));
        CHECK_MSG(!violation, (std::string(describer) + "'s result schema refuses a verb's scopes").c_str());
    }
}

TEST(describeAndListNameTheSyncMethodWithinTheirSchemas)
{
    using Ubiquitty::Protocol::Control::describeMethod;
    using Ubiquitty::Protocol::Control::findMethod;
    using Ubiquitty::Protocol::Control::parseRegistryJSON;
    const MethodDescriptor* search = findMethod("pane.search");
    CHECK(search);
    if (!search)
        return;
    const Value described = describeMethod(*search);
    const Value* named = described.find("syncMethod");
    CHECK(named && named->isString() && named->asString() == "pane.search");
    for (const char* describer : { "describe", "ubq.api.describe" }) {
        const MethodDescriptor* method = findMethod(describer);
        CHECK(method);
        if (!method)
            continue;
        const auto violation = validate(described, parseRegistryJSON(method->resultSchema));
        CHECK_MSG(!violation, (std::string(describer) + "'s result schema refuses a Sync method's description").c_str());
    }
    // The control socket's `list` names an operation; `ubq api list` adds
    // its schemas.
    for (const char* lister : { "list", "ubq.api.list" }) {
        const MethodDescriptor* method = findMethod(lister);
        CHECK(method);
        if (!method)
            continue;
        Value entry = Value::object();
        entry.set("name", Value::string("pane.search"));
        entry.set("description", Value::string("Searches."));
        if (method->name == "ubq.api.list") {
            entry.set("params", Value::object());
            entry.set("result", Value::object());
        }
        entry.set("syncMethod", Value::string("pane.search"));
        Value methods = Value::array();
        methods.push(std::move(entry));
        Value listed = Value::object();
        listed.set("methods", std::move(methods));
        const auto violation = validate(listed, parseRegistryJSON(method->resultSchema));
        CHECK_MSG(!violation, (std::string(lister) + "'s result schema refuses a Sync method's entry").c_str());
    }
}

TEST(everySyncMethodNamesTheRolesThatMayCallIt)
{
    using Ubiquitty::Protocol::Sync::Role;
    for (const MethodDescriptor& method : methodRegistry()) {
        const std::string name(method.name);
        // A method no connection may call names nobody, and one a connection
        // may call names somebody: the column is the whole of who is let in.
        CHECK_MSG(method.syncMethod.empty() == method.roles.isEmpty(), ("a Sync method and its roles disagree: " + name).c_str());
        if (method.syncMethod.empty())
            CHECK_MSG(method.scope == Ubiquitty::Protocol::Sync::Scope::Observe, ("an operation no connection calls names a scope: " + name).c_str());
        // What needs the Drive scope drives a pane, and a viewer drives none.
        if (method.scope == Ubiquitty::Protocol::Sync::Scope::Drive)
            CHECK_MSG(!method.roles.contains(Role::Viewer), ("a viewer may call what drives a pane: " + name).c_str());
    }
}

TEST(aSyncMethodNamesTheScopeItsHostEnforces)
{
    using Ubiquitty::Protocol::Control::findSyncMethod;
    using Ubiquitty::Protocol::Sync::Role;
    using Ubiquitty::Protocol::Sync::Scope;
    // Searching history observes it; extracting screen text requires the
    // credential's distinct region-read authority, whatever the caller's role.
    for (const auto& [reader, scope] : { std::pair { "pane.search", Scope::Observe }, std::pair { "pane.capture", Scope::RegionRead } }) {
        const MethodDescriptor* method = findSyncMethod(reader);
        REQUIRE(method);
        CHECK_MSG(method->scope == scope, reader);
        for (const Role role : Ubiquitty::Protocol::Sync::everyRole)
            CHECK_MSG(method->roles.contains(role), reader);
    }
    // Freezing a pane's size drives it, as typing does.
    const MethodDescriptor* pin = findSyncMethod("pane.pinGeometry");
    REQUIRE(pin);
    CHECK(pin->scope == Scope::Drive);
    CHECK(pin->roles.contains(Role::Owner) && pin->roles.contains(Role::Driver) && pin->roles.contains(Role::Agent));
    CHECK(!pin->roles.contains(Role::Viewer));
}

TEST(aDescriptionSaysWhoMayCallASyncMethodAndUnderWhatScope)
{
    using Ubiquitty::Protocol::Control::describeMethod;
    using Ubiquitty::Protocol::Control::findMethod;
    using Ubiquitty::Protocol::Control::parseRegistryJSON;
    const MethodDescriptor* pin = findMethod("pane.pinGeometry");
    REQUIRE(pin);
    const Value described = describeMethod(*pin);
    const Value* roles = described.find("roles");
    REQUIRE(roles && roles->isArray());
    std::string listed;
    for (const Value& role : roles->items())
        listed += role.asString() + " ";
    CHECK(listed == "owner driver agent ");
    const Value* scope = described.find("scope");
    CHECK(scope && scope->isString() && scope->asString() == "drive");
    for (const char* describer : { "describe", "ubq.api.describe" }) {
        const MethodDescriptor* method = findMethod(describer);
        REQUIRE(method);
        const auto violation = validate(described, parseRegistryJSON(method->resultSchema));
        CHECK_MSG(!violation, (std::string(describer) + "'s result schema refuses a Sync method's roles and scope").c_str());
    }
    // An operation no connection calls says neither.
    const MethodDescriptor* local = findMethod("ls");
    REQUIRE(local);
    const Value localDescription = describeMethod(*local);
    CHECK(!localDescription.find("roles") && !localDescription.find("scope"));
}

namespace {

using Ubiquitty::CapabilityOrigin;
using Ubiquitty::Protocol::Control::admitCall;
using Ubiquitty::Protocol::Control::CallerAuthority;
using Ubiquitty::Protocol::Control::CallOrigin;
using Ubiquitty::Protocol::Control::CallRefusal;
using Ubiquitty::Protocol::Control::CallTarget;
using Ubiquitty::Protocol::Control::ErrorCode;
using Ubiquitty::Protocol::Sync::Role;
using Ubiquitty::Protocol::Sync::Scope;
using Ubiquitty::Protocol::Sync::ScopeSet;

/// A method of a test's own: open to an owner and a driver, under Manage,
/// served only to a local client, taking one bounded integer.
constexpr MethodDescriptor guarded {
    .name = "host.guarded",
    .description = "Does a thing only the host's own user may ask for.",
    .parametersSchema = R"json({"type":"object","properties":{"count":{"type":"integer","minimum":1,"maximum":9}},"required":["count"],"additionalProperties":false})json",
    .resultSchema = R"json({"type":"object"})json",
    .example = R"json({"params":{"count":1},"result":{}})json",
    .principals = { Ubiquitty::Protocol::Control::Principal::Person },
    .controlMethod = "",
    .controlRoute = Ubiquitty::Protocol::Control::ControlRoute::NotServed,
    .commandLineVerb = "",
    .mcpTool = "",
    .syncMethod = "host.guarded",
    .roles = { Role::Owner, Role::Driver },
    .scope = Scope::Manage,
    .target = CallTarget::Host,
    .origin = CallOrigin::Local,
};

constexpr CallerAuthority localOwner { .role = Role::Owner, .scopes = ScopeSet::fromRaw(0x7F), .origin = CapabilityOrigin::Local };

} // namespace

TEST(aCallIsAdmittedOnlyToWhomItsEntryNames)
{
    const Value good = json(R"({"count":3})");
    CHECK(!admitCall(&guarded, good, localOwner).has_value());

    // No such method, and an operation no Sync connection calls.
    const std::optional<CallRefusal> missing = admitCall(nullptr, good, localOwner);
    REQUIRE(missing.has_value());
    CHECK(missing->code == ErrorCode::MethodNotFound && !missing->reason.empty());
    const MethodDescriptor* local = Ubiquitty::Protocol::Control::findMethod("ls");
    REQUIRE(local);
    CHECK(admitCall(local, json("{}"), localOwner).has_value() && admitCall(local, json("{}"), localOwner)->code == ErrorCode::MethodNotFound);

    // A role the entry does not name, and the refusal says which.
    CallerAuthority viewer = localOwner;
    viewer.role = Role::Viewer;
    const std::optional<CallRefusal> byRole = admitCall(&guarded, good, viewer);
    REQUIRE(byRole.has_value());
    CHECK(byRole->code == ErrorCode::Forbidden);
    CHECK_MSG(byRole->reason.find("host.guarded") != std::string::npos && byRole->reason.find("viewer") != std::string::npos, byRole->reason.c_str());
    CallerAuthority agent = localOwner;
    agent.role = Role::Agent;
    CHECK(admitCall(&guarded, good, agent).has_value());
    CallerAuthority driver = localOwner;
    driver.role = Role::Driver;
    CHECK(!admitCall(&guarded, good, driver).has_value());

    // Where the caller stands: a token carrying every scope is still not local.
    CallerAuthority remote = localOwner;
    remote.origin = CapabilityOrigin::Remote;
    const std::optional<CallRefusal> byOrigin = admitCall(&guarded, good, remote);
    REQUIRE(byOrigin.has_value());
    CHECK(byOrigin->code == ErrorCode::Forbidden);
    CHECK_MSG(byOrigin->reason.find("on the host") != std::string::npos, byOrigin->reason.c_str());
    CallerAuthority agentOrigin = localOwner;
    agentOrigin.origin = CapabilityOrigin::Agent;
    CHECK(admitCall(&guarded, good, agentOrigin).has_value());

    // The scope, named in the refusal.
    CallerAuthority unscoped = localOwner;
    unscoped.scopes = { Scope::Observe, Scope::Drive, Scope::Control };
    const std::optional<CallRefusal> byScope = admitCall(&guarded, good, unscoped);
    REQUIRE(byScope.has_value());
    CHECK(byScope->code == ErrorCode::Forbidden);
    CHECK_MSG(byScope->reason.find("manage") != std::string::npos, byScope->reason.c_str());

    // The parameters, last: a caller who may not call is not told what the
    // method takes.
    const std::optional<CallRefusal> byShape = admitCall(&guarded, json(R"({"count":10})"), localOwner);
    REQUIRE(byShape.has_value());
    CHECK(byShape->code == ErrorCode::InvalidParameters);
    CHECK_MSG(byShape->reason.find("count") != std::string::npos, byShape->reason.c_str());
    CHECK(admitCall(&guarded, json("{}"), localOwner)->code == ErrorCode::InvalidParameters);
    CHECK(admitCall(&guarded, json(R"({"count":3,"more":1})"), localOwner)->code == ErrorCode::InvalidParameters);
    CHECK(admitCall(&guarded, json(R"({"count":10})"), viewer)->code == ErrorCode::Forbidden);
    CHECK(admitCall(&guarded, json(R"({"count":10})"), unscoped)->code == ErrorCode::Forbidden);
}

TEST(topologyCallsRetainTheirOperationSpecificRolesAndDriveScope)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    for (const char* name : { "label.set", "tags.set", "workspace.rename", "tab.rename", "tab.move", "window.close",
             "window.activate", "tab.activate", "pane.focus", "window.moveFocus", "pane.zoom", "pane.move", "pane.swap",
             "pane.extractTab", "pane.extractWindow", "tab.extractWindow", "pane.moveToTab", "tab.moveToPane", "pane.moveToNewTab",
             "tab.balance", "pane.adjustRatio", "divider.setRatio", "workspace.addWindow", "workspace.addTab", "workspace.split",
             "workspace.closeTab", "workspace.closePane", "client.label" }) {
        const auto* method = Ubiquitty::Protocol::Control::findSyncMethod(name);
        REQUIRE(method);
        CHECK(method->target == CallTarget::Host && method->scope == Scope::Drive);
        CHECK(method->roles == (Sync::Roles { Role::Owner, Role::Driver, Role::Agent }));
        CHECK(Sync::isStructureMethod(name) || std::string_view(name) == "client.label");
        const auto example = Ubiquitty::Protocol::Control::parseRegistryJSON(method->example);
        const auto* parameters = example.find("params");
        REQUIRE(parameters && parameters->isObject());
        for (const Role role : Sync::everyRole) {
            CallerAuthority caller { .role = role, .scopes = ScopeSet::fromRaw(0x7F), .origin = CapabilityOrigin::Remote };
            const auto refusal = admitCall(method, *parameters, caller);
            CHECK(refusal.has_value() == (role == Role::Viewer));
            if (refusal)
                CHECK(refusal->code == ErrorCode::Forbidden);
            caller.scopes = { Scope::Observe, Scope::Control, Scope::NewSession };
            const auto withoutDrive = admitCall(method, *parameters, caller);
            REQUIRE(withoutDrive);
            CHECK(withoutDrive->code == ErrorCode::Forbidden);
        }
        // Registry admission is not permission to execute: agent structure
        // changes still wait for exact consent, and client.label checks the
        // existing standing authority for every affected session.
    }
}

TEST(captureRequiresRegionReadWithoutInferringItFromOtherAuthority)
{
    using Ubiquitty::Protocol::Control::findSyncMethod;
    using Ubiquitty::Protocol::Control::listSyncMethods;
    const MethodDescriptor* capture = findSyncMethod("pane.capture");
    REQUIRE(capture);
    const Value parameters = json(R"({"pane":1})");
    const std::array table { *capture };
    for (const Role role : Ubiquitty::Protocol::Sync::everyRole) {
        CallerAuthority caller { .role = role,
            .scopes = { Scope::Observe, Scope::Drive, Scope::Control, Scope::NewSession, Scope::FileRead, Scope::Manage },
            .origin = CapabilityOrigin::Remote };
        const auto refused = admitCall(capture, parameters, caller);
        REQUIRE(refused);
        CHECK(refused->code == ErrorCode::Forbidden);
        CHECK(refused->reason.find("pane.capture") != std::string::npos);
        CHECK(refused->reason.find("region") != std::string::npos);
        CHECK(listSyncMethods(caller, table).find("methods")->items().empty());
        // Authority is checked before parameter details; malformed input does
        // not reveal a different path to a caller lacking text-read authority.
        const auto malformed = admitCall(capture, json("{}"), caller);
        REQUIRE(malformed);
        CHECK(malformed->code == ErrorCode::Forbidden);
        caller.scopes = { Scope::Observe, Scope::RegionRead };
        CHECK(!admitCall(capture, parameters, caller));
        const Value listed = listSyncMethods(caller, table);
        REQUIRE(listed.find("methods")->items().size() == 1);
        CHECK(listed.find("methods")->items().front().stringOr("scope") == "region");
        const auto incomplete = admitCall(capture, json("{}"), caller);
        REQUIRE(incomplete);
        CHECK(incomplete->code == ErrorCode::InvalidParameters);
    }
}

TEST(layoutSizingRequiresExplicitRevisionScopedSelectors)
{
    struct Case {
        const char* method;
        const char* parameters;
    };
    for (const auto& entry : std::array {
             Case { "tab.balance", R"({"window":1,"tab":2,"expectedRevision":"7"})" },
             Case { "pane.adjustRatio", R"({"window":1,"tab":2,"pane":3,"direction":"left","step":0.1,"expectedRevision":"7"})" },
             Case { "divider.setRatio", R"({"window":1,"tab":2,"path":[],"ratio":0.5,"expectedRevision":"7"})" } }) {
        const auto* method = Ubiquitty::Protocol::Control::findSyncMethod(entry.method);
        REQUIRE(method);
        CHECK(method->target == CallTarget::Host && method->scope == Scope::Drive);
        const auto parameters = json(entry.parameters);
        for (const Role role : Ubiquitty::Protocol::Sync::everyRole) {
            CallerAuthority caller { .role = role, .scopes = { Scope::Drive }, .origin = CapabilityOrigin::Remote };
            CHECK(admitCall(method, parameters, caller).has_value() == (role == Role::Viewer));
            caller.scopes = { Scope::Observe, Scope::Control, Scope::NewSession };
            CHECK(admitCall(method, parameters, caller).has_value());
        }
        for (const auto field : { "window", "tab", "expectedRevision" }) {
            auto incomplete = Value::object();
            for (const auto& [name, value] : parameters.members())
                if (name != field)
                    incomplete.set(name, value);
            CHECK(admitCall(method, incomplete, localOwner).has_value());
        }
    }
    const auto* divider = Ubiquitty::Protocol::Control::findSyncMethod("divider.setRatio");
    REQUIRE(divider);
    for (const char* path : { "null", "true", "1", "[true]", "[0.5]", "[-1]", "[2]", "[[0]]" }) {
        const auto parameters = std::string(R"({"window":1,"tab":2,"path":)") + path + R"(,"ratio":0.5,"expectedRevision":"7"})";
        CHECK(admitCall(divider, json(parameters.c_str()), localOwner).has_value());
    }
    for (unsigned count : { 63u, 64u }) {
        std::string path = "[0";
        for (unsigned index = 1; index < count; ++index)
            path += ",1";
        path += "]";
        const auto parameters = std::string(R"({"window":1,"tab":2,"path":)") + path + R"(,"ratio":0.5,"expectedRevision":"7"})";
        CHECK(admitCall(divider, json(parameters.c_str()), localOwner).has_value() == (count == 64));
    }
}

TEST(clientLabelsCannotNameAnotherConnection)
{
    const auto* method = Ubiquitty::Protocol::Control::findSyncMethod("client.label");
    REQUIRE(method);
    CHECK(method->scope == Scope::Drive && method->target == CallTarget::Host);
    for (const Role role : Ubiquitty::Protocol::Sync::everyRole) {
        CallerAuthority caller { .role = role, .scopes = { Scope::Drive }, .origin = CapabilityOrigin::Remote };
        CHECK(admitCall(method, json(R"({"label":"laptop"})"), caller).has_value() == (role == Role::Viewer));
        caller.scopes = { Scope::Observe, Scope::Control, Scope::NewSession };
        CHECK(admitCall(method, json(R"({"label":"laptop"})"), caller).has_value());
    }
    CHECK(!admitCall(method, json(R"({"label":""})"), localOwner));
    for (const char* invalid : { "{}", R"({"label":1})", R"({"label":"x","client":1})", R"({"label":"x","object":"client:1"})" })
        CHECK(admitCall(method, json(invalid), localOwner).has_value());
}

TEST(arrayBoundsAreCheckedBeforeTheirItems)
{
    for (const char* keyword : { "minItems", "maxItems" }) {
        for (const char* bound : { "null", "true", "\"2\"", "-1", "0.5" }) {
            const auto schema = std::string(R"({"type":"array",")") + keyword + "\":" + bound + "}";
            CHECK(checkSchema(json(schema.c_str())).has_value());
        }
    }
    const auto schema = json(R"({"type":"object","properties":{"path":{"type":"array","maxItems":2,"items":{"type":"integer","minimum":0,"maximum":1}}}})");
    CHECK(!checkSchema(schema));
    for (const char* value : { R"({"path":[]})", R"({"path":[0]})", R"({"path":[0,1]})" })
        CHECK(!validate(json(value), schema));
    const auto oversized = validate(json(R"({"path":[false,0,1]})"), schema);
    REQUIRE(oversized);
    CHECK(oversized->path == "/path");
    CHECK(oversized->message == "'path' must contain at most 2 items.");
    const auto invalidItem = validate(json(R"({"path":[false]})"), schema);
    REQUIRE(invalidItem);
    CHECK(invalidItem->path == "/path/0");
    const auto emptyOnly = json(R"({"type":"array","maxItems":0})");
    CHECK(!checkSchema(emptyOnly) && !validate(json("[]"), emptyOnly));
    CHECK(validate(json("[0]"), emptyOnly).has_value());

    const auto exactPair = json(R"({"type":"object","properties":{"path":{"type":"array","minItems":2,"maxItems":2,"items":{"type":"integer"}}}})");
    CHECK(!checkSchema(exactPair));
    CHECK(!validate(json(R"({"path":[0,1]})"), exactPair));
    for (const char* value : { R"({"path":[]})", R"({"path":[false]})" }) {
        const auto undersized = validate(json(value), exactPair);
        REQUIRE(undersized);
        CHECK(undersized->path == "/path");
        CHECK(undersized->message == "'path' must contain at least 2 items.");
    }
    CHECK(!validate(json("[]"), json(R"({"type":"array","minItems":0})")));
}

TEST(workspaceSharingRequiresAtLeastOneSession)
{
    const auto* method = Ubiquitty::Protocol::Control::findSyncMethod("workspace.share");
    REQUIRE(method);
    const auto parameters = json(R"({"daemonInstanceID":"1","workspace":0,"arrangement":"encoded arrangement","sessions":[1]})");
    CHECK(!admitCall(method, parameters, localOwner));
    const auto empty = json(R"({"daemonInstanceID":"1","workspace":0,"arrangement":"encoded arrangement","sessions":[]})");
    const auto refusal = admitCall(method, empty, localOwner);
    REQUIRE(refusal);
    CHECK(refusal->code == ErrorCode::InvalidParameters);
    CHECK_MSG(refusal->reason.find("/sessions") != std::string::npos, refusal->reason.c_str());
}

TEST(windowCreationRequiresAnObservedSourceAndDrive)
{
    const auto* method = Ubiquitty::Protocol::Control::findSyncMethod("workspace.addWindow");
    REQUIRE(method);
    CHECK(method->target == CallTarget::Host && method->scope == Scope::Drive);
    const auto parameters = json(R"({"window":1,"pane":2,"expectedRevision":"7"})");
    for (const Role role : Ubiquitty::Protocol::Sync::everyRole) {
        CallerAuthority caller { .role = role, .scopes = { Scope::Drive, Scope::NewSession }, .origin = CapabilityOrigin::Remote };
        CHECK(admitCall(method, parameters, caller).has_value() == (role == Role::Viewer));
        caller.scopes = { Scope::Observe, Scope::Control, Scope::NewSession };
        CHECK(admitCall(method, parameters, caller).has_value());
    }
    for (const char* invalid : { "{}", R"({"window":1,"pane":2})", R"({"window":1,"expectedRevision":"7"})",
             R"({"pane":2,"expectedRevision":"7"})", R"({"window":1,"pane":2,"expectedRevision":7})",
             R"({"window":1,"pane":0,"expectedRevision":"7"})" })
        CHECK(admitCall(method, json(invalid), localOwner).has_value());
}

TEST(transfersRequireDriveAndExactDestinationPlacement)
{
    struct Case {
        const char* method;
        const char* parameters;
    };
    for (const auto& entry : std::array {
             Case { "pane.moveToTab", R"({"window":1,"pane":2,"tab":3,"target":4,"orientation":"vertical","side":"before","insertTarget":"root","expectedRevision":"7"})" },
             Case { "tab.moveToPane", R"({"window":1,"tab":2,"target":4,"orientation":"horizontal","side":"after","insertTarget":"pane","expectedRevision":"7"})" },
             Case { "pane.moveToNewTab", R"({"window":1,"pane":2,"targetWindow":3,"expectedRevision":"7"})" } }) {
        const auto* method = Ubiquitty::Protocol::Control::findSyncMethod(entry.method);
        REQUIRE(method);
        CHECK(method->target == CallTarget::Host && method->scope == Scope::Drive);
        const auto parameters = json(entry.parameters);
        for (const Role role : Ubiquitty::Protocol::Sync::everyRole) {
            CallerAuthority caller { .role = role, .scopes = { Scope::Drive }, .origin = CapabilityOrigin::Remote };
            CHECK(admitCall(method, parameters, caller).has_value() == (role == Role::Viewer));
            caller.scopes = { Scope::Observe, Scope::Control, Scope::NewSession };
            CHECK(admitCall(method, parameters, caller).has_value());
        }
        CHECK(admitCall(method, json("{}"), localOwner).has_value());
        for (const auto field : { "expectedRevision", "window" }) {
            auto malformed = Value::object();
            for (const auto& [name, value] : parameters.members()) {
                if (name != field)
                    malformed.set(name, value);
            }
            CHECK(admitCall(method, malformed, localOwner).has_value());
        }
    }
    for (const char* target : { "unknown", "" }) {
        const auto* method = Ubiquitty::Protocol::Control::findSyncMethod("pane.moveToTab");
        REQUIRE(method);
        const std::string invalid = std::string(R"({"window":1,"pane":2,"tab":3,"target":4,"orientation":"vertical","side":"before","insertTarget":")")
            + target + R"(","expectedRevision":"7"})";
        CHECK(admitCall(method, json(invalid.c_str()), localOwner).has_value());
    }
}

TEST(extractionsRequireDriveAndAnObservedSource)
{
    for (const char* name : { "pane.extractTab", "pane.extractWindow", "tab.extractWindow" }) {
        const auto* method = Ubiquitty::Protocol::Control::findSyncMethod(name);
        REQUIRE(method);
        CHECK(method->target == CallTarget::Host);
        CHECK(method->scope == Scope::Drive);
        const auto parameters = json(std::string_view(name) == "tab.extractWindow"
                ? R"({"window":1,"tab":2,"expectedRevision":"7"})"
                : R"({"window":1,"pane":2,"expectedRevision":"7"})");
        for (const Role role : Ubiquitty::Protocol::Sync::everyRole) {
            CallerAuthority caller { .role = role, .scopes = { Scope::Drive }, .origin = CapabilityOrigin::Remote };
            CHECK(admitCall(method, parameters, caller).has_value() == (role == Role::Viewer));
            caller.scopes = { Scope::Observe, Scope::Control, Scope::NewSession };
            CHECK(admitCall(method, parameters, caller).has_value());
        }
        for (const char* invalid : { "{}", R"({"window":1,"pane":2})", R"({"window":1,"tab":2})",
                 R"({"window":1,"pane":2,"expectedRevision":7})", R"({"window":1,"tab":2,"expectedRevision":7})",
                 R"({"window":0,"pane":2,"expectedRevision":"7"})", R"({"window":0,"tab":2,"expectedRevision":"7"})" })
            CHECK(admitCall(method, json(invalid), localOwner).has_value());
    }
}

TEST(paneRelocationsRequireDriveAndAnExactObservedPair)
{
    for (const char* name : { "pane.move", "pane.swap" }) {
        const auto* method = Ubiquitty::Protocol::Control::findSyncMethod(name);
        REQUIRE(method);
        CHECK(method->target == CallTarget::Host);
        CHECK(method->scope == Scope::Drive);
        const auto parameters = json(std::string_view(name) == "pane.move"
                ? R"({"window":1,"pane":2,"target":3,"orientation":"vertical","side":"before","expectedRevision":"7"})"
                : R"({"window":1,"pane":2,"target":3,"expectedRevision":"7"})");
        for (const Role role : Ubiquitty::Protocol::Sync::everyRole) {
            CallerAuthority caller { .role = role, .scopes = { Scope::Drive }, .origin = CapabilityOrigin::Remote };
            CHECK(admitCall(method, parameters, caller).has_value() == (role == Role::Viewer));
            caller.scopes = { Scope::Observe, Scope::Control, Scope::NewSession };
            CHECK(admitCall(method, parameters, caller).has_value());
        }
        CHECK(admitCall(method, json("{}"), localOwner).has_value());
        CHECK(admitCall(method, json(R"({"window":1,"pane":2,"target":3})"), localOwner).has_value());
    }
    const auto* move = Ubiquitty::Protocol::Control::findSyncMethod("pane.move");
    REQUIRE(move);
    for (const char* invalid : {
             R"({"window":1,"pane":2,"target":3,"orientation":"diagonal","side":"before","expectedRevision":"7"})",
             R"({"window":1,"pane":2,"target":3,"orientation":"vertical","side":"center","expectedRevision":"7"})",
             R"({"window":1,"pane":2,"target":0,"orientation":"vertical","side":"before","expectedRevision":"7"})",
             R"({"window":1,"pane":2,"target":3,"orientation":"vertical","side":"before","expectedRevision":7})" })
        CHECK(admitCall(move, json(invalid), localOwner).has_value());
}

TEST(navigationCallsRequireDriveAndExactStateDerivedIntent)
{
    struct Case {
        const char* method;
        const char* parameters;
    };
    for (const auto& entry : std::array {
             Case { "window.activate", R"({"window":1})" },
             Case { "tab.activate", R"({"tab":2})" },
             Case { "pane.focus", R"({"pane":3})" },
             Case { "window.moveFocus", R"({"window":1,"pane":3,"direction":"left","expectedRevision":"7"})" },
             Case { "pane.zoom", R"({"pane":3,"state":"zoomed","expectedRevision":"7"})" } }) {
        const auto* method = Ubiquitty::Protocol::Control::findSyncMethod(entry.method);
        REQUIRE(method);
        CHECK(method->target == CallTarget::Host);
        CHECK(method->scope == Scope::Drive);
        for (const Role role : Ubiquitty::Protocol::Sync::everyRole) {
            CallerAuthority caller { .role = role, .scopes = { Scope::Observe, Scope::Drive }, .origin = CapabilityOrigin::Remote };
            const auto allowed = admitCall(method, json(entry.parameters), caller);
            CHECK(allowed.has_value() == (role == Role::Viewer));
            caller.scopes = { Scope::Observe, Scope::Control, Scope::NewSession };
            CHECK(admitCall(method, json(entry.parameters), caller).has_value());
        }
        CHECK(admitCall(method, json("{}"), localOwner).has_value());
    }
    const auto* zoom = Ubiquitty::Protocol::Control::findSyncMethod("pane.zoom");
    REQUIRE(zoom);
    for (const char* invalid : { R"({"pane":3,"state":"toggle","expectedRevision":"7"})",
             R"({"pane":3,"state":"zoomed"})", R"({"pane":3,"state":"zoomed","expectedRevision":7})" })
        CHECK(admitCall(zoom, json(invalid), localOwner).has_value());
    const auto* move = Ubiquitty::Protocol::Control::findSyncMethod("window.moveFocus");
    REQUIRE(move);
    CHECK(admitCall(move, json(R"({"window":1,"direction":"left","expectedRevision":"7"})"), localOwner).has_value());
    CHECK(admitCall(move, json(R"({"window":1,"pane":3,"direction":"next","expectedRevision":"7"})"), localOwner).has_value());
}

TEST(aCallerIsListedTheMethodsItMayCall)
{
    using Ubiquitty::Protocol::Control::listSyncMethods;
    const std::array<MethodDescriptor, 2> table { guarded, *Ubiquitty::Protocol::Control::findMethod("pane.capture") };
    const auto names = [&](const CallerAuthority& caller) {
        std::string listed;
        const Value described = listSyncMethods(caller, table);
        for (const Value& method : described.find("methods")->items())
            listed += method.stringOr("name") + ":" + method.stringOr("target") + ":" + method.stringOr("scope") + " ";
        return listed;
    };
    CHECK_MSG(names(localOwner) == "host.guarded:host:manage pane.capture:pane:region ", names(localOwner).c_str());
    CallerAuthority viewer = localOwner;
    viewer.role = Role::Viewer;
    CHECK(names(viewer) == "pane.capture:pane:region ");
    CallerAuthority remote = localOwner;
    remote.origin = CapabilityOrigin::Remote;
    CHECK(names(remote) == "pane.capture:pane:region ");
    CallerAuthority nothing = localOwner;
    nothing.scopes = {};
    CHECK(names(nothing).empty());

    // What the real table lists is within host.methods' own result schema.
    // Passive retained metadata is available through host.refresh under
    // Observe. Other host-wide queries require Control or more. One under
    // Drive changes a workspace the caller names, as a pane verb does, so no
    // role that cannot drive may call it; which workspace it may change is
    // decided where the workspace is found. A consent answer or revocation
    // is a person's decision on a question the host put to that person: a
    // human Viewer with Observe may decide it, so the registry admits it
    // under Observe to people alone and the host checks each kind's stricter
    // authority where it serves the call (Docs/sync.md, consent.respond).
    const MethodDescriptor* methods = Ubiquitty::Protocol::Control::findSyncMethod("host.methods");
    REQUIRE(methods);
    CHECK(!validate(listSyncMethods(localOwner), Ubiquitty::Protocol::Control::parseRegistryJSON(methods->resultSchema)).has_value());
    const MethodDescriptor* refresh = Ubiquitty::Protocol::Control::findSyncMethod("host.refresh");
    REQUIRE(refresh);
    CHECK(refresh->target == CallTarget::Host);
    CHECK(refresh->scope == Scope::Observe);
    CHECK(refresh->roles.contains(Role::Viewer));
    CallerAuthority observer = viewer;
    observer.scopes = ScopeSet { Scope::Observe };
    CHECK(!admitCall(refresh, json("{}"), observer));
    CHECK(admitCall(refresh, json("{}"), nothing).has_value());
    using Ubiquitty::Protocol::Control::Principal;
    using Ubiquitty::Protocol::Control::Principals;
    const auto isConsentDecision = [](const MethodDescriptor& method) { return method.name == "consent.respond" || method.name == "consent.revoke"; };
    for (const MethodDescriptor& method : methodRegistry()) {
        if (isConsentDecision(method))
            CHECK_MSG(method.target == CallTarget::Host && method.scope == Scope::Observe && method.principals == Principals { Principal::Person }, std::string(method.name).c_str());
        else if (!method.syncMethod.empty() && method.target == CallTarget::Host && method.name != "host.refresh")
            CHECK_MSG(method.scope != Scope::Observe && (method.scope != Scope::Drive || !method.roles.contains(Role::Viewer)), std::string(method.name).c_str());
        // A pane-target method requires its attachment-local handle. A host
        // method may instead name a host-owned pane; the target column, not
        // a parameter's spelling, determines where the host routes it.
        if (!method.syncMethod.empty() && method.target == CallTarget::Pane) {
            const Value schema = Ubiquitty::Protocol::Control::parseRegistryJSON(method.parametersSchema);
            const Value* properties = schema.find("properties");
            const Value* pane = properties ? properties->find("pane") : nullptr;
            CHECK_MSG(pane && pane->stringOr("type") == "integer", std::string(method.name).c_str());
            const Value* required = schema.find("required");
            bool requiresPane = false;
            if (required) {
                for (const Value& parameter : required->items())
                    requiresPane = requiresPane || (parameter.isString() && parameter.asString() == "pane");
            }
            CHECK_MSG(requiresPane, std::string(method.name).c_str());
        }
    }
    for (const char* name : { "workspace.split", "workspace.closePane" }) {
        const MethodDescriptor* method = Ubiquitty::Protocol::Control::findSyncMethod(name);
        REQUIRE(method);
        CHECK_MSG(method->target == CallTarget::Host, name);
        const Value schema = Ubiquitty::Protocol::Control::parseRegistryJSON(method->parametersSchema);
        const Value* properties = schema.find("properties");
        const Value* pane = properties ? properties->find("pane") : nullptr;
        REQUIRE(pane);
        CHECK(pane->stringOr("type") == "integer");
        CHECK_EQ(pane->numberOr("minimum"), 1.0);
        CHECK_EQ(pane->numberOr("maximum"), 4294967295.0);
    }
}

TEST(captureResultNamesItsTargetGeometryAndCompleteness)
{
    const MethodDescriptor* capture = Ubiquitty::Protocol::Control::findSyncMethod("pane.capture");
    REQUIRE(capture);
    const Value schema = Ubiquitty::Protocol::Control::parseRegistryJSON(capture->resultSchema);
    const Value* properties = schema.find("properties");
    REQUIRE(properties);
    REQUIRE(properties->find("rows"));
    REQUIRE(properties->find("columns"));
    CHECK(properties->find("rows")->numberOr("maximum") == Ubiquitty::maxRows);
    CHECK(properties->find("columns")->numberOr("maximum") == Ubiquitty::maxColumns);
    CHECK(!validate(json(R"({"pane":1,"rows":2,"columns":80,"complete":true,"text":"\n\n"})"), schema));
    CHECK(!validate(json(R"({"pane":1,"rows":129,"columns":1024,"complete":false,"text":"partial\n"})"), schema));
    for (const char* invalid : {
             R"({"text":"old unqualified answer\n"})",
             R"({"pane":1,"rows":2,"columns":80,"text":"missing completeness\n"})",
             R"({"pane":1,"rows":2,"columns":80,"complete":1,"text":"not a boolean\n"})",
             R"({"pane":0,"rows":2,"columns":80,"complete":true,"text":"invalid handle\n"})",
             R"({"pane":1,"rows":0,"columns":80,"complete":true,"text":"invalid rows\n"})",
             R"({"pane":1,"rows":2.5,"columns":80,"complete":true,"text":"fractional rows\n"})",
             R"({"pane":1,"rows":1025,"columns":80,"complete":true,"text":"oversized rows\n"})",
             R"({"pane":1,"rows":2,"columns":1025,"complete":true,"text":"oversized columns\n"})",
             R"({"pane":1,"rows":2,"columns":80,"complete":true,"text":null})" }) {
        CHECK_MSG(validate(json(invalid), schema).has_value(), invalid);
    }
}

TEST(renameSchemasAdmitExactlyTheSharedByteLimitBeforeAnyEffect)
{
    using Ubiquitty::Protocol::Control::findSyncMethod;
    using Ubiquitty::Protocol::Control::parseRegistryJSON;
    constexpr std::size_t limit = Ubiquitty::Session::maxWorkspaceNameBytes;
    CHECK_EQ(limit, std::size_t { 4096 });
    std::string multibyte;
    for (std::size_t bytes = 0; bytes < limit; bytes += 2)
        multibyte += "\xc3\xa9";
    REQUIRE(multibyte.size() == limit);

    for (const char* name : { "workspace.rename", "tab.rename" }) {
        const MethodDescriptor* method = findSyncMethod(name);
        REQUIRE(method);
        const Value schema = parseRegistryJSON(method->parametersSchema);
        const Value* properties = schema.find("properties");
        const Value* nameSchema = properties ? properties->find("name") : nullptr;
        REQUIRE(nameSchema);
        CHECK_EQ(nameSchema->numberOr("maxLength"), static_cast<double>(limit));
        const auto parametersFor = [&](const std::string& text) {
            Value parameters = Value::object();
            parameters.set(method->name == "workspace.rename" ? "workspace" : "tab", Value::number(1));
            parameters.set("name", Value::string(text));
            return parameters;
        };
        for (const std::string& exact : { std::string(limit, 'n'), multibyte }) {
            const Value exactParameters = parametersFor(exact);
            CHECK(!validate(exactParameters, schema));
            CHECK(!admitCall(method, exactParameters, localOwner));

            const Value oversizedParameters = parametersFor(exact + "n");
            const auto violation = validate(oversizedParameters, schema);
            REQUIRE(violation);
            CHECK_STR_EQ(violation->path, "/name");
            CHECK_MSG(violation->message.find("at most 4096 bytes") != std::string::npos && violation->message.find("4097") != std::string::npos, violation->message.c_str());
            const auto refused = admitCall(method, oversizedParameters, localOwner);
            REQUIRE(refused);
            CHECK(refused->code == ErrorCode::InvalidParameters);
            CHECK_MSG(refused->reason.find("/name") != std::string::npos && refused->reason.find("4097") != std::string::npos, refused->reason.c_str());

            // Authority remains the first decision: an oversized name does
            // not disclose a method's parameters to a caller it forbids.
            CallerAuthority viewer = localOwner;
            viewer.role = Role::Viewer;
            CallerAuthority unscoped = localOwner;
            unscoped.scopes = { Scope::Observe, Scope::Control };
            for (const CallerAuthority& caller : { viewer, unscoped }) {
                const auto forbidden = admitCall(method, oversizedParameters, caller);
                REQUIRE(forbidden);
                CHECK(forbidden->code == ErrorCode::Forbidden);
                CHECK_MSG(forbidden->reason.find("/name") == std::string::npos && forbidden->reason.find("4097") == std::string::npos, forbidden->reason.c_str());
            }
        }
        CHECK(!admitCall(method, parametersFor(""), localOwner));
    }
}

TEST(searchPatternsMustBeUTF8BeforeCLIOrHostAdmission)
{
    using namespace Ubiquitty::Protocol;
    const auto* method = findCommandLineVerb("search");
    REQUIRE(method);
    CHECK(method == Control::findSyncMethod("pane.search"));
    const Value schema = Control::parseRegistryJSON(method->parametersSchema);
    const Control::CallerAuthority caller { .role = Sync::Role::Owner, .scopes = { Sync::Scope::Observe }, .origin = Ubiquitty::CapabilityOrigin::Remote };
    const auto parametersFor = [](const char* pattern) {
        Value parameters = Value::object();
        parameters.set("pane", Value::number(1));
        parameters.set("pattern", Value::string(pattern));
        return parameters;
    };
    for (const char* pattern : { "error: .*", "caf\xC3\xA9", "\xEF\xBF\xBD", "\xF0\x9F\x98\x80.*" }) {
        const Value parameters = parametersFor(pattern);
        CHECK(!validate(parameters, schema));
        CHECK(!Control::admitCall(method, parameters, caller));
    }
    for (const char* pattern : { "\xFF", "\x80", "\xC0\xAF", "\xE2\x82", "\xED\xA0\x80", "\xF4\x90\x80\x80" }) {
        const Value parameters = parametersFor(pattern);
        const auto violation = validate(parameters, schema);
        REQUIRE(violation);
        CHECK_STR_EQ(violation->path, "/pattern");
        CHECK_STR_EQ(violation->message, "'pattern' is not valid UTF-8.");
        const auto refusal = Control::admitCall(method, parameters, caller);
        REQUIRE(refusal);
        CHECK(refusal->code == Control::ErrorCode::InvalidParameters);
        CHECK(refusal->reason.find("pane.search") != std::string::npos);
        CHECK(refusal->reason.find("UTF-8") != std::string::npos);
    }
}

TEST(rawInputSchemasPreserveCommandAndTextBytes)
{
    for (const char* verb : { "new", "run", "send" }) {
        const auto* method = findCommandLineVerb(verb);
        REQUIRE(method);
        Value parameters = Value::object();
        parameters.set(method->name == "ubq.send" ? "text" : "command", Value::string("\xFF"));
        CHECK(!validate(parameters, Ubiquitty::Protocol::Control::parseRegistryJSON(method->parametersSchema)));
    }
}

TEST(aSchemaUsingAKeywordNothingEnforcesIsRefused)
{
    CHECK(!checkSchema(json(R"({"type":"object","properties":{"a":{"type":"string","maxLength":4}},"required":["a"]})")));
    CHECK(!checkSchema(json(R"({"type":"string","format":"utf8"})")));
    CHECK(checkSchema(json(R"({"type":"string","format":"unknown"})")).has_value());
    CHECK(checkSchema(json(R"({"type":"string","format":true})")).has_value());
    CHECK(checkSchema(json(R"({"type":"integer","format":"utf8"})")).has_value());
    CHECK_MSG(checkSchema(json(R"({"type":"object","properties":{"a":{"type":"string","pattern":"^x"}}})")).has_value(), "pattern would advertise a rule nothing enforces");
    CHECK(checkSchema(json(R"({"type":"text"})")).has_value());
    CHECK(checkSchema(json(R"({"type":"object","required":["missing"],"additionalProperties":false})")).has_value());
    CHECK(checkSchema(json(R"({"type":"object","additionalProperties":{"type":"string"}})")).has_value());
}

TEST(theValidatorRefusesWhatTheSchemaForbidsAndNamesWhere)
{
    const Value schema = json(R"({"type":"object","properties":{"n":{"type":"integer","minimum":1,"maximum":9},"s":{"type":"string","minLength":1,"maxLength":3},"e":{"type":"string","enum":["a","b"]},"l":{"type":"array","items":{"type":"integer"}},"o":{"type":"object","properties":{"x":{"type":"boolean"}},"additionalProperties":false}},"required":["n"],"additionalProperties":false})");
    CHECK(!validate(json(R"({"n":3,"s":"ab","e":"a","l":[1,2],"o":{"x":true}})"), schema));

    struct Case {
        const char* instance;
        const char* path;
    };
    for (const Case& refused : {
             Case { R"({})", "/n" },
             Case { R"({"n":0})", "/n" },
             Case { R"({"n":10})", "/n" },
             Case { R"({"n":2.5})", "/n" },
             Case { R"({"n":"3"})", "/n" },
             Case { R"({"n":3,"s":""})", "/s" },
             Case { R"({"n":3,"s":"abcd"})", "/s" },
             Case { R"({"n":3,"e":"c"})", "/e" },
             Case { R"({"n":3,"l":[1,"x"]})", "/l/1" },
             Case { R"({"n":3,"o":{"y":1}})", "/o/y" },
             Case { R"({"n":3,"extra":1})", "/extra" },
             Case { R"({"n":3,"n":4})", "/n" },
             Case { R"([1])", "" },
         }) {
        const auto violation = validate(json(refused.instance), schema);
        CHECK_MSG(violation.has_value(), refused.instance);
        if (violation)
            CHECK_STR_EQ(violation->path, refused.path);
    }
}

UBIQUITTY_TEST_MAIN()

TEST(processSnapshotIncludesBackgroundJobsAndBoundsEscapedCommands)
{
    using namespace Ubiquitty;
    std::vector<PAL::Process::Record> records {
        { .processID = 10, .parentProcessID = 1, .processGroupID = 10, .name = "zsh", .commandLine = "zsh", .isSessionLeader = true },
        { .processID = 11, .parentProcessID = 10, .processGroupID = 11, .name = "node", .commandLine = "node /tmp/server.js", .isForeground = true },
        { .processID = 12, .parentProcessID = 10, .processGroupID = 12, .name = "sleep", .commandLine = "sleep 60" },
    };
    auto snapshot = Protocol::Control::processSnapshot(7, records, true);
    REQUIRE(snapshot.find("processes"));
    CHECK_EQ(snapshot.find("processes")->items().size(), 3u);
    CHECK_STR_EQ(snapshot.find("processes")->items()[1].stringOr("name"), "server.js");
    CHECK(snapshot.find("complete")->asBool());
    const auto* method = Protocol::Control::findSyncMethod("pane.processes");
    REQUIRE(method);
    CHECK(!validate(snapshot, Protocol::Control::parseRegistryJSON(method->resultSchema)));
    records.resize(128, records[0]);
    for (auto& record : records)
        record.commandLine = std::string(4000, '\x01');
    snapshot = Protocol::Control::processSnapshot(7, records, true);
    CHECK(!snapshot.find("complete")->asBool());
    CHECK(Codec::JSON::serialize(snapshot).size() < 49000);
    CHECK(!validate(snapshot, Protocol::Control::parseRegistryJSON(method->resultSchema)));
    CHECK(!Protocol::Control::processSnapshot(7, {}, false).find("available")->asBool());
}
