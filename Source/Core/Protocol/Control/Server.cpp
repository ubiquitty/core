// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Server.cpp — see Core/Protocol/Control/Server.h.
//

#include "Core/Protocol/Control/Server.h"

#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Session/BoundedSocketServer.h"
#include "Core/Session/ControlPaths.h"
#include "PAL/Log.h"
#include "PAL/Socket.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <new>
#include <optional>
#include <utility>

void retainControlServer(Ubiquitty::Protocol::Control::Server* server)
{
    if (server)
        server->swiftReferences.fetch_add(1, std::memory_order_relaxed);
}

void releaseControlServer(Ubiquitty::Protocol::Control::Server* server)
{
    if (server && server->swiftReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete server;
}

extern "C" {

/// Appends a host's reply to the `std::string` behind `replyContext`.
///
/// C linkage because it is the `HostReply` the server hands a host.
static void collectControlReply(void* replyContext, const char* json, std::size_t length)
{
    if (json && length)
        static_cast<std::string*>(replyContext)->append(json, length);
}
}

namespace Ubiquitty::Protocol::Control {

namespace {

using Codec::JSON::Value;
using Security::AgentPolicy;
using Security::InjectionVerdict;

/// A refusal's message, or empty when the gate allowed the request.
///
/// The gate has already audited a refusal; this only words it.
std::string refusalMessage(InjectionVerdict verdict)
{
    switch (verdict) {
    case InjectionVerdict::Allowed:
        return {};
    case InjectionVerdict::DeniedByRule:
        return "blocked by the agent policy (deny)";
    case InjectionVerdict::NeedsApproval:
        return "requires approval — add an 'allow' rule to the agent policy, or set 'default allow'";
    }
    return "blocked by the agent policy";
}

/// The audit line's kind: the door and its target, in the ids `ls` reports.
///
/// A pane id of 0 names the window alone: `control-split(window=12)`.
std::string auditKind(std::string_view door, std::int64_t window, std::int64_t pane)
{
    std::string kind(door);
    kind += "(window=" + std::to_string(window);
    if (pane)
        kind += " pane=" + std::to_string(pane);
    return kind + ")";
}

/// What the policy screens an opening method as: its words in angle brackets,
/// `new-window` as `<new window>`, so a rule names what the audit log shows.
std::string actionDescription(std::string_view method)
{
    std::string description = "<";
    for (const char character : method)
        description += character == '-' ? ' ' : character;
    return description + ">";
}

/// The window and pane a gated verb resolved to, and the session behind them.
struct Target {
    std::int64_t window = 0;
    std::int64_t pane = 0;
    std::uint32_t sessionID = 0;
};

/// An integer member of a host reply; 0 when absent or not a number.
std::int64_t integerOf(const Value& object, const char* key)
{
    return static_cast<std::int64_t>(object.numberOr(key, 0));
}

} // namespace

Dispatcher::Dispatcher(std::string token, AgentPolicy policy, Security::ApprovalMode approval, HostOperations host, std::span<const MethodDescriptor> registry)
    : m_token(std::move(token))
    , m_policy(std::move(policy))
    , m_approval(approval)
    , m_host(host)
    , m_registry(registry)
{
}

std::string Dispatcher::handleLine(std::string_view line) const
{
    const DecodedRequest request = decodeRequest(line, m_token);
    switch (request.outcome) {
    case RequestOutcome::Ignore:
        return {};
    case RequestOutcome::Respond:
        return Codec::JSON::serialize(request.response);
    case RequestOutcome::Dispatch:
        break;
    }
    return Codec::JSON::serialize(dispatch(request.method, request.params, request.idOrNull()));
}

Dispatcher::HostAnswer Dispatcher::perform(std::string_view verb, const Value& parameters) const
{
    HostAnswer answer;
    if (!m_host.perform) {
        answer.error = "no frontend is serving the control socket";
        answer.code = ErrorCode::InternalError;
        return answer;
    }
    const std::string parametersJSON = Codec::JSON::serialize(parameters);
    std::string reply;
    m_host.perform(m_host.context, verb.data(), verb.size(), parametersJSON.data(), parametersJSON.size(), collectControlReply, &reply);

    bool isParsed = false;
    const Value parsed = Codec::JSON::parse(reply, isParsed);
    if (!isParsed || !parsed.isObject()) {
        answer.error = "the frontend gave no answer";
        answer.code = ErrorCode::InternalError;
        return answer;
    }
    if (const Value* error = parsed.find("error"); error && error->isString()) {
        answer.error = error->asString().empty() ? std::string("the frontend refused") : error->asString();
        return answer;
    }
    const Value* result = parsed.find("result");
    answer.result = result ? *result : Value::object();
    return answer;
}

Value Dispatcher::dispatch(const std::string& method, const Value& request, const Value* id) const
{
    // The registry is the whole of what this socket serves: a method is served
    // as its entry routes it, and one without a routed entry does not exist
    // here.
    const MethodDescriptor* entry = findControlMethod(method, m_registry);
    if (!entry || entry->controlRoute == ControlRoute::NotServed) {
        Value details = Value::object();
        details.set("method", Value::string(method));
        return rpcError(id, ErrorCode::MethodNotFound, "method not found: " + method, details);
    }
    if (!entry->principals.contains(Principal::LocalProcess))
        return rpcError(id, ErrorCode::Forbidden, "'" + method + "' is not served to a process at the control socket");

    // The token authenticates the request rather than parameterising the
    // method, and decodeRequest has already checked it.
    Value parameters = Value::object();
    for (const auto& [key, value] : request.members()) {
        if (key != "token")
            parameters.set(key, value);
    }
    // Validated whole before any branch runs, so a refused request has had no
    // effect: no host verb, no policy verdict, no audit line.
    if (const std::optional<SchemaViolation> violation = validate(parameters, parseRegistryJSON(entry->parametersSchema))) {
        Value details = Value::object();
        details.set("method", Value::string(method));
        details.set("parameter", Value::string(violation->path));
        return rpcError(id, ErrorCode::InvalidParameters, violation->message, details);
    }

    // A host answer becomes the response. A host error is, by default, the
    // caller's parameters naming nothing the frontend has.
    auto respond = [id](HostAnswer answer) {
        if (!answer.error.empty())
            return rpcError(id, answer.code, answer.error);
        return rpcResult(id, std::move(answer.result));
    };

    // The window and pane a request names, resolved by the host so the gate
    // and the audit line carry the ids `ls` reports rather than the 0 that
    // means "the key window". On failure `answer` holds the host's error.
    auto resolveTarget = [&](Target& target, HostAnswer& answer) {
        Value request = Value::object();
        request.set("window", Value::number(parameters.numberOr("window", 0)));
        request.set("pane", Value::number(parameters.numberOr("pane", 0)));
        answer = perform("target", request);
        if (!answer.error.empty())
            return false;
        target.window = integerOf(answer.result, "window");
        target.pane = integerOf(answer.result, "pane");
        target.sessionID = static_cast<std::uint32_t>(integerOf(answer.result, "sessionID"));
        return true;
    };

    // A verb that opens or splits is gated as the fixed description of what
    // it does: a policy line names the words the log will carry
    // (`deny <new window>`).
    auto refusedAction = [&](const std::string& kind, const std::string& description) {
        return refusalMessage(Security::evaluateAction(m_policy, 0, kind.c_str(), description, m_approval, Security::AuditOnAllow::Record, AgentPolicy::Subject::Description));
    };

    // The host is handed the method under the name the registry gives it.
    const std::string_view hostVerb = entry->controlMethod;

    switch (entry->controlRoute) {
    case ControlRoute::NotServed:
        break;

    case ControlRoute::Handshake: {
        Value result = Value::object();
        result.set("name", Value::string("ubiquitty-control"));
        Value methods = Value::array();
        for (const MethodDescriptor& served : m_registry) {
            if (served.controlRoute != ControlRoute::NotServed)
                methods.push(Value::string(std::string(served.controlMethod)));
        }
        result.set("methods", std::move(methods));
        return rpcResult(id, std::move(result));
    }

    case ControlRoute::ListOperations:
        return rpcResult(id, listMethods(Principal::LocalProcess, m_registry));

    case ControlRoute::DescribeOperation: {
        const std::string name = parameters.stringOr("method");
        const MethodDescriptor* described = findMethodFromSurface(name, Surface::ControlSocket, m_registry);
        if (!described) {
            Value details = Value::object();
            details.set("parameter", Value::string("/method"));
            return rpcError(id, ErrorCode::NotFound, "no operation is named '" + name + "'", details);
        }
        return rpcResult(id, describeMethod(*described));
    }

    // A raise, a display-string write and a listing are the Benign tier, and
    // the titles a listing reports are window chrome already on the peer's
    // own display.
    case ControlRoute::Host:
        return respond(perform(hostVerb, parameters));

    case ControlRoute::HostTypesText: {
        const std::string text = parameters.stringOr("text");
        Target target;
        HostAnswer failure;
        if (!resolveTarget(target, failure))
            return respond(std::move(failure));
        // Screened by the rule file and audit log that screen ubq, before a
        // byte joins typing's route; the route itself stays Local
        // (Core/Security/InjectionGate.h).
        const std::string line = Security::screenedLine(text);
        const InjectionVerdict verdict = Security::evaluateInjection(m_policy, target.sessionID, auditKind("control-send", target.window, target.pane).c_str(), line, m_approval);
        if (verdict == InjectionVerdict::NeedsApproval && Security::isHeldForShellIndirection(m_policy, line))
            return rpcError(id, ErrorCode::Forbidden, "requires approval — " + std::string(Security::shellIndirectionHold));
        if (const std::string refusal = refusalMessage(verdict); !refusal.empty())
            return rpcError(id, ErrorCode::Forbidden, refusal);
        Value request = Value::object();
        request.set("window", Value::number(static_cast<double>(target.window)));
        request.set("pane", Value::number(static_cast<double>(target.pane)));
        request.set("text", Value::string(text));
        return respond(perform(hostVerb, request));
    }

    case ControlRoute::HostReadsScreen: {
        const std::string source = parameters.stringOr("source", "screen");
        Target target;
        HostAnswer failure;
        if (!resolveTarget(target, failure))
            return respond(std::move(failure));
        // Screen text and the selection are both a screen read: what the user
        // last had on screen, read by a peer that is not the user.
        const InjectionVerdict verdict = Security::evaluateScreenRead(m_policy, target.sessionID, auditKind("control-screen", target.window, target.pane).c_str(), "source=" + source, m_approval);
        if (const std::string refusal = refusalMessage(verdict); !refusal.empty())
            return rpcError(id, ErrorCode::Forbidden, refusal);
        Value request = Value::object();
        request.set("window", Value::number(static_cast<double>(target.window)));
        request.set("pane", Value::number(static_cast<double>(target.pane)));
        request.set("source", Value::string(source));
        return respond(perform(hostVerb, request));
    }

    case ControlRoute::HostOpens: {
        if (const std::string refusal = refusedAction("control-new", actionDescription(hostVerb)); !refusal.empty())
            return rpcError(id, ErrorCode::Forbidden, refusal);
        return respond(perform(hostVerb, Value::object()));
    }

    case ControlRoute::HostSplits: {
        Target target;
        HostAnswer failure;
        if (!resolveTarget(target, failure))
            return respond(std::move(failure));
        // "down" stacks the panes; anything else splits side by side, the
        // menu's default.
        const bool isDown = parameters.stringOr("direction") == "down";
        if (const std::string refusal = refusedAction(auditKind("control-split", target.window, 0), isDown ? "<split down>" : "<split right>"); !refusal.empty())
            return rpcError(id, ErrorCode::Forbidden, refusal);
        Value request = Value::object();
        request.set("window", Value::number(static_cast<double>(target.window)));
        request.set("direction", Value::string(isDown ? "down" : "right"));
        return respond(perform(hostVerb, request));
    }
    }
    return rpcError(id, ErrorCode::MethodNotFound, "method not found: " + method);
}

Server::Server(std::string socketPath, Dispatcher dispatcher)
    : m_socketPath(std::move(socketPath))
    , m_dispatcher(std::move(dispatcher))
{
}

Server::~Server()
{
    stop();
}

Server* Server::startIfEnabled(const Preferences::Values& values, HostOperations host)
{
    // Off unless the user opted in.
    if (!Preferences::Effective::controlEnabled(values))
        return nullptr;

    Configuration configuration;
    configuration.token = values.controlToken.value_or(std::string());
    configuration.approval = Preferences::Effective::controlApprove(values) ? Security::ApprovalMode::AutoApprove : Security::ApprovalMode::RequireConfirmation;
    if (const std::string& override = values.controlSocketPath.value_or(std::string()); !override.empty()) {
        configuration.socketPath = override;
    } else {
        std::array<char, 1024> path {};
        if (!Session::controlFilePath("control.sock", path)) {
            PAL::logLine(stderr, "ubiquitty: control socket: could not resolve the socket path");
            return nullptr;
        }
        configuration.socketPath = path.data();
    }
    return start(configuration, host);
}

Server* Server::start(const Configuration& configuration, HostOperations host)
{
    const std::string& path = configuration.socketPath;
    // The default path lives in the ownership- and symlink-validated control
    // directory; an override does not, but the directory is still where the
    // policy and the audit log live.
    if (!Session::ensureControlDirectory()) {
        PAL::logLine(stderr, "ubiquitty: control socket: the control directory failed validation");
        return nullptr;
    }

    // A configured path may point at someone's file: only an owner-matching
    // socket proven stale is ever removed.
    if (const auto prepared = Session::prepareSocketPathForBind(path); !prepared) {
        PAL::logLine(stderr, "ubiquitty: control socket: refusing to bind {}: {}", path, prepared.error());
        return nullptr;
    }
    auto listener = PAL::listenLocal(path, 8);
    if (!listener) {
        PAL::logLine(stderr, "ubiquitty: control socket: cannot listen on {}: {}", path, listener.error());
        return nullptr;
    }

    // The same rule file ubq is held to, read once at bind as the
    // agent reads it once at launch.
    AgentPolicy policy;
    policy.loadDefault();
    Server* server = new (std::nothrow) Server(path, Dispatcher(configuration.token, std::move(policy), configuration.approval, host));
    if (!server) {
        PAL::removeLocalSocket(path);
        return nullptr;
    }

    const Dispatcher* dispatcher = &server->m_dispatcher;
    server->m_boundedServer = std::make_unique<Session::BoundedSocketServer>(
        listener->release(), Session::BoundedSocketServer::Options {},
        [](const PAL::FileDescriptor& client) {
            // The peer must run as us; each platform reports the peer its own
            // way, so the rule lives in the PAL.
            return PAL::peerIsSameUser(client);
        },
        [dispatcher](const std::string& line) { return dispatcher->handleLine(line); });
    Session::BoundedSocketServer* bounded = server->m_boundedServer.get();
    server->m_thread = std::thread([bounded] { bounded->run(); });
    PAL::logLine(stderr, "ubiquitty: control socket: listening on {}{}", path, configuration.token.empty() ? "" : " (token required)");
    return server;
}

void Server::stop()
{
    if (m_boundedServer)
        m_boundedServer->stop();
    if (m_thread.joinable())
        m_thread.join();
    if (m_boundedServer) {
        m_boundedServer.reset();
        PAL::removeLocalSocket(m_socketPath);
    }
}

} // namespace Ubiquitty::Protocol::Control
