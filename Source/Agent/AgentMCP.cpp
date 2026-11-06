// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Agent/AgentMCP.h"

#include "Agent/AgentSession.h"
#include "Core/Codec/Base64.h"
#include "Core/Codec/JSON.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/ControlRPC.h"
#include "Core/Protocol/Sync/WireVersion.h"
#include "Core/Session/SessionRows.h"
#include "Platform/Host/HostPaths.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty {
namespace Agent {

using Security::AgentPolicy;
using Security::ApprovalMode;
using Security::AuditOnAllow;
using Security::auditRecord;
using Security::InjectionVerdict;
using Security::PolicyDecision;

// The JSON codec lives in Core/Codec beside the byte codecs; the agent is one of
// two consumers, not its owner.
namespace JSON = Ubiquitty::Codec::JSON;

namespace {

constexpr const char* serverName = "ubq";
constexpr const char* defaultProtocolVersion = "2024-11-05";

// The protocol versions this server actually implements. `initialize` answers
// with the client's version when it is one of these and with the server's own
// otherwise, which is what lets a client discover the dialect in force. Echoing
// whatever was asked for tells a client its version is in effect and then
// speaks a different one — a mismatch nothing on either side can detect.
constexpr std::string_view supportedProtocolVersions[] = { "2024-11-05" };

/// Answers whether this server implements a protocol version.
///
/// - Parameter version: The version string a client sent.
/// - Returns: true when the handshake can proceed on it.
[[nodiscard]] bool isSupportedProtocolVersion(std::string_view version)
{
    for (const std::string_view supported : supportedProtocolVersions)
        if (supported == version)
            return true;
    return false;
}

// The consent prompt's name for a client that gives none, or gives one this
// server will not show.
constexpr std::string_view defaultClientLabel = "mcp-client";
// What one line of a consent prompt holds beside the rest of the question.
constexpr std::size_t maxClientLabelLength = 32;

/// What one MCP connection knows about the client at its other end.
struct MCPClient {
    /// The name `initialize` gave in `clientInfo`, which the consent prompt
    /// shows as the client's own, unverified claim.
    std::string label { defaultClientLabel };
    std::string remoteHost;
};

/// Returns the name the consent prompt shows for a client, from the
/// parameters of its `initialize` request.
///
/// The name is the client's claim about itself and the prompt says so, so it
/// is never a grant key. It is held to printable ASCII, because a control or
/// escape byte would let the claim restyle the prompt that quotes it, and cut
/// to `maxClientLabelLength` characters.
///
/// - Parameter parameters: The request's `params`, or null when it has none.
/// - Returns: `clientInfo.name`, or `defaultClientLabel` when it is absent,
///   blank, or holds a byte outside printable ASCII.
[[nodiscard]] std::string clientLabel(const JSON::Value* parameters)
{
    const JSON::Value* information = parameters ? parameters->find("clientInfo") : nullptr;
    std::string name = information ? information->stringOr("name") : std::string();
    const bool isPrintable = std::ranges::all_of(name, [](char character) { return character >= 0x20 && character <= 0x7E; });
    const bool isBlank = std::ranges::all_of(name, [](char character) { return character == ' '; });
    if (!isPrintable || isBlank)
        return std::string(defaultClientLabel);
    name.resize(std::min(name.size(), maxClientLabelLength));
    return name;
}

// Per-session last-seen screen text, for terminal_changes ("what
// changed since the previous read"). Lives for the server process's
// lifetime — an MCP client keeps one process across many tool calls.
std::map<uint32_t, std::string> lastSeenScreens;

// ---- transport: one JSON message per line on stdout ---------------

void writeMessage(const JSON::Value& message)
{
    std::string line = JSON::serialize(message);
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

void sendResult(const JSON::Value* id, JSON::Value result)
{
    JSON::Value response = JSON::Value::object();
    response.set("jsonrpc", JSON::Value::string("2.0"));
    response.set("id", id ? *id : JSON::Value::null());
    response.set("result", std::move(result));
    writeMessage(response);
}

void sendError(const JSON::Value* id, int code, const std::string& message, const JSON::Value& details = JSON::Value::object())
{
    JSON::Value error = JSON::Value::object();
    error.set("code", JSON::Value::number(code));
    error.set("message", JSON::Value::string(message));
    // The control socket's shape: a kind a program can branch on, and the
    // details of what was refused.
    JSON::Value data = JSON::Value::object();
    data.set("kind", JSON::Value::string(std::string(Protocol::Control::errorKind(static_cast<Protocol::Control::ErrorCode>(code)))));
    for (const auto& [key, value] : details.members())
        data.set(key, value);
    error.set("data", std::move(data));
    JSON::Value response = JSON::Value::object();
    response.set("jsonrpc", JSON::Value::string("2.0"));
    response.set("id", id ? *id : JSON::Value::null());
    response.set("error", std::move(error));
    writeMessage(response);
}

// A tools/call result: one text content block, optionally an error.
// Why input did not reach the session, for a tool result.
std::string inputDeliveryText(AgentSession::InputDelivery delivery, std::string_view what)
{
    if (delivery == AgentSession::InputDelivery::Unconfirmed)
        return "the daemon did not confirm writing " + std::string(what) + " to the session within "
            + std::to_string(AgentSession::inputWrittenWaitMilliseconds / 1000) + " s; it may be written later, or not at all";
    return std::string(what) + " could not be sent";
}

JSON::Value textResult(const std::string& text, bool isError = false)
{
    JSON::Value block = JSON::Value::object();
    block.set("type", JSON::Value::string("text"));
    block.set("text", JSON::Value::string(text));
    JSON::Value content = JSON::Value::array();
    content.push(std::move(block));
    JSON::Value result = JSON::Value::object();
    result.set("content", std::move(content));
    if (isError)
        result.set("isError", JSON::Value::boolean(true));
    return result;
}

// The shared codec writes into a buffer the caller sizes; MCP wants a value, so
// the string is the buffer.
std::string base64Encode(const std::vector<uint8_t>& data)
{
    std::string out(Ubiquitty::Codec::base64EncodedLength(data.size()), '\0');
    const auto written = Ubiquitty::Codec::base64Encode(
        std::span<const char>(reinterpret_cast<const char*>(data.data()), data.size()), std::span<char>(out));
    out.resize(written ? written->size() : 0);
    return out;
}

// A tools/call result carrying a single PNG image block (base64).
JSON::Value imageResult(const std::string& base64png)
{
    JSON::Value block = JSON::Value::object();
    block.set("type", JSON::Value::string("image"));
    block.set("data", JSON::Value::string(base64png));
    block.set("mimeType", JSON::Value::string("image/png"));
    JSON::Value content = JSON::Value::array();
    content.push(std::move(block));
    JSON::Value result = JSON::Value::object();
    result.set("content", std::move(content));
    return result;
}

// ---- tool catalog (tools/list) ------------------------------------

// Generated from the method registry, so a tool's name, description and input
// schema are the ones `describe` reports and the arguments are validated
// against; a tool cannot be offered here without an entry there.
JSON::Value buildToolCatalog()
{
    JSON::Value tools = JSON::Value::array();
    for (const Protocol::Control::MethodDescriptor& method : Protocol::Control::methodRegistry()) {
        if (method.mcpTool.empty() || !method.principals.contains(Protocol::Control::Principal::Agent))
            continue;
        JSON::Value tool = JSON::Value::object();
        tool.set("name", JSON::Value::string(std::string(method.mcpTool)));
        tool.set("description", JSON::Value::string(std::string(method.description)));
        tool.set("inputSchema", Protocol::Control::parseRegistryJSON(method.parametersSchema));
        tools.push(std::move(tool));
    }
    return tools;
}

// ---- tool dispatch ------------------------------------------------

/// The state a tool needs before it can act on a session.
enum class AttachReadiness {
    IdentityOnly,
    FirstScreen,
};

// A screen-dependent tool proceeds only after its first checkpoint. Session
// creation needs only the identity acknowledged by the host.
std::string attachSession(AgentSession& session, const MCPClient& client, int columns, int rows, uint32_t id, AttachReadiness readiness = AttachReadiness::FirstScreen)
{
    session.setAgentLabel(client.label);
    session.setRemoteHost(client.remoteHost);
    if (!session.connect(columns, rows, id))
        return "no daemon is running (start one: ubiquitty-host --ensure-daemon)";
    if (!session.waitForFirstScreen(session.answerWaitMilliseconds())) {
        if (session.consentDenied())
            return session.consentDeniedReason() == Ubiquitty::Protocol::Sync::ConsentStatusPhase::Expired ? "timed out awaiting the user's consent to attach this session" : "the user denied access to this session";
        if (session.authDenied())
            return session.refusalText();
        if (readiness == AttachReadiness::IdentityOnly && session.sessionID())
            return {};
        const std::string ended = session.endedReason();
        if (session.didFirstScreenWaitExpire())
            return readiness == AttachReadiness::FirstScreen ? "timed out waiting for the first screen" : "timed out waiting for the session attachment";
        const std::string reason = ended.empty() ? "the connection ended" : ended;
        return readiness == AttachReadiness::FirstScreen ? "the first screen did not arrive: " + reason : reason;
    }
    return std::string();
}

// Admission failures such as HostFull also set authDenied. Only the reasons
// that enforce consent or credentials are authorization denials.
bool isAttachmentDenial(const AgentSession& session)
{
    if (session.consentDenied())
        return true;
    if (!session.authDenied())
        return false;
    using Protocol::Sync::ByeReason;
    switch (session.authDeniedReason()) {
    case ByeReason::TokenMissing:
    case ByeReason::TokenInvalid:
    case ByeReason::TokenExpired:
    case ByeReason::TokenRevoked:
    case ByeReason::OutsideScope:
    case ByeReason::NoConsent:
    case ByeReason::RemoteHostRefused:
        return true;
    default:
        return false;
    }
}

// Attach for a verb that already passed the policy gate and audited an allow.
// An allow must not stand alone for an action that never ran. Only consent or
// auth refusals are denials; a missing host or screen leaves the action incomplete.
// Read-only verbs never audit an allow, so there is nothing to reconcile.
std::string attachGated(AgentSession& session, const MCPClient& client, int columns, int rows, uint32_t id, const char* kind, const std::string& detail, AttachReadiness readiness = AttachReadiness::FirstScreen)
{
    std::string err = attachSession(session, client, columns, rows, id, readiness);
    if (!err.empty()) {
        if (isAttachmentDenial(session))
            auditRecord(id, kind, "[attach-denied] " + detail + ": " + err, PolicyDecision::Deny);
        else
            Security::auditIncompleteAction(id, kind, "[attach-incomplete] " + detail + ": " + err);
    }
    return err;
}

// Words the shared gate's verdict for the tool result: an empty string when
// the action may proceed, else the reason it may not. The gate audits.
std::string gateAndAudit(AgentPolicy& policy, uint32_t session, const char* kind, const std::string& detail, ApprovalMode approval, AuditOnAllow auditOnAllow = AuditOnAllow::Record, AgentPolicy::Subject subject = AgentPolicy::Subject::AgentSupplied)
{
    switch (Security::evaluateAction(policy, session, kind, detail, approval, auditOnAllow, subject)) {
    case InjectionVerdict::Allowed:
        return std::string();
    case InjectionVerdict::DeniedByRule:
        return "blocked by policy (deny): " + detail;
    case InjectionVerdict::NeedsApproval:
        if (subject == AgentPolicy::Subject::AgentSupplied && Security::isHeldForShellIndirection(policy, detail))
            return "requires approval — " + std::string(Security::shellIndirectionHold) + "; send each command on its own";
        return "requires approval — add an 'allow' policy rule or set 'default allow' in the agent policy";
    }
    return std::string();
}

// A verb that may create implicitly gates the creation as a creation.
//
// run / send / interrupt take `session` optionally — "omit for a fresh session" is
// their documented contract — and the daemon reads sessionID 0 in HELLO as CREATE.
// So the session appears without the "new" gate the read-only verbs were changed to
// avoid (see terminal_screen below), and a policy written to stop an agent spawning
// shells stops only the verb that says `new` in its name.
//
// Returns the block reason when the implied creation is refused, empty otherwise —
// including when nothing is being created, which is every call that names a session.
std::string gateImpliedCreation(AgentPolicy& policy, uint32_t session, int columns, int rows)
{
    if (session != 0)
        return std::string();
    return gateAndAudit(policy, session, "new", std::to_string(columns) + "x" + std::to_string(rows), ApprovalMode::RequireConfirmation, AuditOnAllow::Record, AgentPolicy::Subject::Description);
}

JSON::Value dispatchTool(const std::string& name, const JSON::Value& args, AgentPolicy& policy, const MCPClient& client)
{
    const int columns = static_cast<int>(args.numberOr("columns", 80));
    const int rows = static_cast<int>(args.numberOr("rows", 24));
    const uint32_t session = static_cast<uint32_t>(args.numberOr("session", 0));
    const int timeoutMilliseconds = static_cast<int>(args.numberOr("timeout_ms", 10000));
    // MCP callers are untrusted: ApprovalMode is always RequireConfirmation.
    // The human's approval channel is out-of-band policy rules, never an
    // in-band flag the agent controls.
    const char* noDaemon = "no daemon is running (start one: ubiquitty-host --ensure-daemon)";

    if (name == "terminal_new") {
        // Session creation drives a real login shell — agent-origin state
        // change; gate + audit it like run/send/interrupt/resize. No agent
        // action bypasses the gate, and the audit log records every new session.
        std::string blocked = gateAndAudit(policy, session, "new", std::to_string(columns) + "x" + std::to_string(rows), ApprovalMode::RequireConfirmation, AuditOnAllow::Record, AgentPolicy::Subject::Description);
        if (!blocked.empty())
            return textResult(blocked, true);
        AgentSession s;
        if (std::string err = attachGated(s, client, columns, rows, session, "new", std::to_string(columns) + "x" + std::to_string(rows), AttachReadiness::IdentityOnly); !err.empty())
            return textResult(err, true);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(s.sessionID()));
        s.detach();
        return textResult(std::string("session ") + buffer);
    }
    if (name == "terminal_screen") {
        // A read-only verb must not implicitly create a session. With
        // session==0 the daemon treats HELLO as CREATE and spawns a login shell,
        // which would let an agent under a `deny new` policy spawn (unaudited)
        // shells via a read tool. Require an existing id — creation goes through
        // the gated `terminal_new`.
        if (!session)
            return textResult("terminal_screen needs a 'session' id (from terminal_new)", true);
        AgentSession s;
        if (std::string err = attachSession(s, client, columns, rows, session); !err.empty())
            return textResult(err, true);
        const auto capture = s.captureScreen(timeoutMilliseconds);
        s.detach();
        if (!capture || capture->status != Protocol::Control::CaptureReplyStatus::Complete)
            return textResult(capture ? capture->reason : s.unansweredText("the screen capture"), true);
        return textResult(args.stringOr("format", "text") == "structured" ? JSON::serialize(capture->screen) : capture->text);
    }
    if (name == "terminal_changes") {
        if (!session)
            return textResult("terminal_changes needs a 'session' id (from terminal_new)", true);
        AgentSession s;
        if (std::string err = attachSession(s, client, columns, rows, session); !err.empty())
            return textResult(err, true);
        const auto capture = s.captureScreen(timeoutMilliseconds);
        s.detach();
        if (!capture || capture->status != Protocol::Control::CaptureReplyStatus::Complete)
            return textResult(capture ? capture->reason : s.unansweredText("the screen capture"), true);
        const std::string& current = capture->text;
        auto previous = lastSeenScreens.find(session);
        std::string diff = computeScreenDiff(previous != lastSeenScreens.end() ? previous->second : std::string(), current);
        lastSeenScreens[session] = current;
        return textResult(diff);
    }
    if (name == "terminal_run") {
        std::string command = args.stringOr("command");
        if (command.empty())
            return textResult("missing required 'command'", true);
        if (std::string refused = gateImpliedCreation(policy, session, columns, rows); !refused.empty())
            return textResult(refused, true);
        std::string blocked = gateAndAudit(policy, session, "run", command, ApprovalMode::RequireConfirmation);
        if (!blocked.empty())
            return textResult(blocked, true);
        AgentSession s;
        if (std::string err = attachGated(s, client, columns, rows, session, "run", command); !err.empty())
            return textResult(err, true);
        const auto run = s.runCommandWithExit(command, timeoutMilliseconds);
        s.detach();
        if (run.completion == AgentSession::RunCompletion::TimedOut)
            return textResult("the command did not complete within " + std::to_string(timeoutMilliseconds) + " ms; terminal_screen shows its screen", true);
        const auto& captured = run.capture;
        if (!captured || captured->status != Protocol::Control::CaptureReplyStatus::Complete)
            return textResult(captured ? captured->reason : s.unansweredText("the command capture"), true);
        std::string body = captured->text;
        if (run.exitStatus) {
            char trailer[48];
            std::snprintf(trailer, sizeof(trailer), "\n[exit=%d]", *run.exitStatus == INT32_MIN ? -1 : static_cast<int>(*run.exitStatus));
            body += trailer;
        }
        return textResult(body);
    }
    if (name == "terminal_send") {
        std::string text = args.stringOr("text");
        if (text.empty())
            return textResult("missing required 'text'", true);
        if (std::string refused = gateImpliedCreation(policy, session, columns, rows); !refused.empty())
            return textResult(refused, true);
        std::string blocked = gateAndAudit(policy, session, "send", text, ApprovalMode::RequireConfirmation);
        if (!blocked.empty())
            return textResult(blocked, true);
        AgentSession s;
        if (std::string err = attachGated(s, client, columns, rows, session, "send", text); !err.empty())
            return textResult(err, true);
        const AgentSession::InputDelivery delivery = s.sendInput(text);
        s.pump(300);
        s.detach();
        if (delivery != AgentSession::InputDelivery::Written)
            return textResult(inputDeliveryText(delivery, "the text"), true);
        return textResult("sent");
    }
    if (name == "terminal_interrupt") {
        // Ctrl-C is agent-origin input — gate it through the one policy like
        // run/send; no agent input bypasses the gate. gateAndAudit audits.
        if (std::string refused = gateImpliedCreation(policy, session, columns, rows); !refused.empty())
            return textResult(refused, true);
        std::string blocked = gateAndAudit(policy, session, "interrupt", "<Ctrl-C>", ApprovalMode::RequireConfirmation, AuditOnAllow::Record, AgentPolicy::Subject::Description);
        if (!blocked.empty())
            return textResult(blocked, true);
        AgentSession s;
        if (std::string err = attachGated(s, client, columns, rows, session, "interrupt", "<Ctrl-C>"); !err.empty())
            return textResult(err, true);
        const AgentSession::InputDelivery delivery = s.sendInput("\x03");
        s.pump(200);
        s.detach();
        if (delivery != AgentSession::InputDelivery::Written)
            return textResult(inputDeliveryText(delivery, "the interrupt"), true);
        return textResult("interrupted");
    }
    if (name == "terminal_resize") {
        int newColumns = static_cast<int>(args.numberOr("columns", 0));
        int newRows = static_cast<int>(args.numberOr("rows", 0));
        if (!session || newColumns <= 0 || newRows <= 0)
            return textResult("terminal_resize needs session, columns, rows", true);
        // Resize drives SIGWINCH + reflow on the session — agent-origin state
        // change; gate + audit it like run/send/interrupt. No agent action
        // bypasses the gate, and the audit log records every resize.
        // CallerRecords: the host answers a resize, so the record below is
        // of what it did, as terminal_kill's is.
        const std::string size = std::to_string(newColumns) + "x" + std::to_string(newRows);
        std::string blocked = gateAndAudit(policy, session, "resize", size, ApprovalMode::RequireConfirmation, AuditOnAllow::CallerRecords, AgentPolicy::Subject::Description);
        if (!blocked.empty())
            return textResult(blocked, true);
        AgentSession s;
        if (std::string err = attachGated(s, client, columns, rows, session, "resize", std::to_string(newColumns) + "x" + std::to_string(newRows)); !err.empty())
            return textResult(err, true);
        const bool didResize = s.resize(newColumns, newRows);
        const std::string ended = didResize ? std::string() : s.endedReason();
        s.pump(300);
        s.detach();
        auditRecord(session, "resize", size, didResize ? PolicyDecision::Allow : PolicyDecision::Deny);
        return didResize ? textResult("resized") : textResult(ended.empty() ? "the host did not resize the session" : ended, true);
    }
    if (name == "terminal_wait") {
        std::string needle = args.stringOr("substring");
        if (needle.empty())
            return textResult("missing required 'substring'", true);
        // Read-only verb — require an existing session, never implicitly
        // create one (a session==0 HELLO would spawn a shell, bypassing the `new` gate).
        if (!session)
            return textResult("terminal_wait needs a 'session' id (from terminal_new)", true);
        AgentSession s;
        if (std::string err = attachSession(s, client, columns, rows, session); !err.empty())
            return textResult(err, true);
        // Measured on the clock: a session that prints continuously wakes the
        // wait on every frame, long before the substring shows.
        bool found = false;
        const auto captured = s.waitForText(needle, timeoutMilliseconds, found);
        s.detach();
        if (!captured || captured->status != Protocol::Control::CaptureReplyStatus::Complete)
            return textResult(captured ? captured->reason : s.unansweredText("the screen capture"), true);
        return textResult((found ? "FOUND\n" : "TIMEOUT\n") + captured->text, !found);
    }
    if (name == "terminal_read_last") {
        if (!session)
            return textResult("terminal_read_last needs a 'session' id", true);
        AgentSession s;
        if (std::string err = attachSession(s, client, columns, rows, session); !err.empty())
            return textResult(err, true);
        const auto captured = s.captureText({ .source = Protocol::Control::CaptureSource::LastCommand }, timeoutMilliseconds);
        s.detach();
        if (!captured || captured->status != Protocol::Control::CaptureReplyStatus::Complete)
            return textResult(captured ? captured->reason : s.unansweredText("the command capture"), true);
        return textResult(captured->text);
    }
    if (name == "terminal_wait_idle") {
        if (!session)
            return textResult("terminal_wait_idle needs a 'session' id", true);
        AgentSession s;
        if (std::string err = attachSession(s, client, columns, rows, session); !err.empty())
            return textResult(err, true);
        bool idle = s.waitForIdle(timeoutMilliseconds);
        s.detach();
        return textResult(idle ? "idle" : "timeout", !idle);
    }
    if (name == "terminal_list") {
        // The host's Control-scoped listing owns disclosure. Listing neither
        // creates nor attaches to a session.
        AgentSession s;
        s.setAgentLabel(client.label);
        s.setRemoteHost(client.remoteHost);
        if (!s.connectControl()) // control-only: don't spawn a session
            return textResult(s.authDenied() ? s.refusalText() : noDaemon, true);
        const std::optional<AgentSession::HostTree> host = s.listHost(timeoutMilliseconds);
        s.detach();
        if (!host || !host->isWhole)
            return textResult("no response", true);
        std::string out;
        char line[512];
        for (const Session::SessionRow& info : Session::sessionRows(host->tree)) {
            std::snprintf(line, sizeof(line), "%u\t%dx%d\t%s\t%s\t%s\n", static_cast<unsigned>(info.id), info.columns, info.rows, info.isAtPrompt() ? "idle" : "busy", info.foregroundProcess.c_str(), info.currentDirectory.c_str());
            out += line;
        }
        return textResult(out.empty() ? std::string("(no sessions)") : out);
    }
    if (name == "terminal_kill") {
        if (!session)
            return textResult("terminal_kill needs a 'session' id", true);
        // Ending a session is the most destructive thing the agent surface can do,
        // and it was the one mutating verb the policy never saw: run, send,
        // interrupt, resize and new all gate here, and the CLI gates its own `kill`
        // (main.cpp) with a comment naming the requirement — a Deny rule must be
        // able to block it. The daemon's consent gate is a different boundary: it
        // protects the session's owner, and it steps aside for an agent holding a
        // standing grant, which every agent has for a session it created. So for
        // the sessions an agent is most likely to end, this was the only gate there
        // was to miss. Same detail string as the CLI, so both entry points audit
        // the same words.
        //
        // CallerRecords: the outcome audit below is this verb's record.
        std::string blocked = gateAndAudit(policy, session, "kill", "<terminate session>", ApprovalMode::RequireConfirmation, AuditOnAllow::CallerRecords, AgentPolicy::Subject::Description);
        if (!blocked.empty())
            return textResult(blocked, true);

        AgentSession s;
        s.setAgentLabel(client.label);
        s.setRemoteHost(client.remoteHost);
        if (!s.connectControl()) {
            // The terminate never reached the daemon — audit it as not carried out, never a premature Allow.
            auditRecord(session, "kill", "<terminate session>", PolicyDecision::Deny);
            return textResult(s.authDenied() ? s.refusalText() : noDaemon, true);
        }

        AgentSession::TerminateOutcome outcome = s.terminateSession(session);
        s.detach();
        if (outcome == AgentSession::TerminateOutcome::Terminated) {
            // The cached screen exists to diff the next terminal_changes against
            // the last one. A session that has ended has no next, and this
            // server is documented as one long-lived process across many tool
            // calls — so a cache nothing erases holds a full screen for every
            // session id it ever saw.
            lastSeenScreens.erase(session);
        }

        // Audit the REAL decision: terminate is consent-gated at the daemon, so a
        // human-denied / timed-out / failed kill must log Deny, not Allow (the
        // audit log must reflect what actually happened, not the request).
        auditRecord(session, "kill", "<terminate session>", outcome == AgentSession::TerminateOutcome::Terminated ? PolicyDecision::Allow : PolicyDecision::Deny);
        switch (outcome) {
        case AgentSession::TerminateOutcome::Terminated:
            return textResult("terminated");
        case AgentSession::TerminateOutcome::Denied:
            return textResult("the user denied terminating this session", true);
        case AgentSession::TerminateOutcome::TimedOut:
            return textResult("timed out awaiting the user's consent to terminate", true);
        case AgentSession::TerminateOutcome::Refused:
            // The daemon refused — say why, in its words when it answered and
            // as the connect path does when it closed. "the user denied"
            // would name a person who was never asked.
            if (!s.authDenied() && s.terminationRefusal())
                return textResult(s.terminationRefusal()->message.empty() ? std::string("the host refused to end the session") : s.terminationRefusal()->message, true);
            return textResult(s.refusalText(), true);
        case AgentSession::TerminateOutcome::Failed:
            break;
        }
        return textResult("failed", true);
    }
    if (name == "terminal_screenshot") {
        // Read-only verb — require an existing session (see terminal_screen);
        // never implicitly spawn a shell just to screenshot a blank one.
        if (!session)
            return textResult("terminal_screenshot needs a 'session' id (from terminal_new)", true);
        AgentSession s;
        if (std::string err = attachSession(s, client, columns, rows, session); !err.empty())
            return textResult(err, true);
        std::vector<uint8_t> png = s.renderPNG();
        std::string path = args.stringOr("path");
        if (!path.empty() && !png.empty()) {
            // An MCP client is untrusted: `path` is a bare leaf saved under
            // ~/.ubiquitty/screenshots, never a caller-chosen filesystem location
            // (a separator or ".." would escape, so reject it). The PNG is also
            // returned over MCP below, so the file write is a convenience.
            if (path.find('/') != std::string::npos || path.find('\0') != std::string::npos || path == "." || path == "..") {
                s.detach();
                return textResult("screenshot 'path' must be a bare filename (no '/' or '..'); the PNG is also returned in this result", true);
            }
            char directory[1024];
            if (Ubiquitty::Host::controlDirectory(directory) > 0) {
                std::string screenshotDirectory = std::string(directory) + "/screenshots";
                ::mkdir(screenshotDirectory.c_str(), 0700);
                std::string full = screenshotDirectory + "/" + path;
                if (std::FILE* file = std::fopen(full.c_str(), "wb")) {
                    std::fwrite(png.data(), 1, png.size(), file);
                    std::fclose(file);
                    // The file-write is a filesystem side-effect no policy
                    // rule covers (the PNG is also returned inline, and the leaf is
                    // sanitized above). Record it in the audit trail for visibility.
                    auditRecord(session, "screenshot", "wrote screenshots/" + path, PolicyDecision::Allow);
                }
            }
        }
        s.detach();
        if (png.empty())
            return textResult("screenshot failed", true);
        return imageResult(base64Encode(png));
    }
    return textResult("unknown tool: " + name, true);
}

// ---- JSON-RPC method dispatch -------------------------------------

// The JSON-RPC 2.0 / MCP request parameter object is `params`; no other
// spelling is read.
static const JSON::Value* requestParameters(const JSON::Value& message)
{
    return message.find("params");
}

void handleMessage(const JSON::Value& message, AgentPolicy& policy, MCPClient& client)
{
    const JSON::Value* id = message.find("id");
    const bool isNotification = (id == nullptr);
    const JSON::Value* methodValue = message.find("method");
    if (!methodValue || !methodValue->isString()) {
        if (!isNotification)
            sendError(id, -32600, "invalid request: no method");
        return;
    }
    const std::string method = methodValue->asString();

    if (method == "initialize") {
        std::string protocolVersion = defaultProtocolVersion;
        client.label = clientLabel(requestParameters(message));
        if (const JSON::Value* parameters = requestParameters(message)) {
            // A version this server implements is agreed to; anything else is
            // answered with the server's own, which is the client's signal to
            // proceed on that dialect or stop. The alternative — repeating the
            // requested string — is agreement to something this server cannot
            // speak.
            const std::string requested = parameters->stringOr("protocolVersion");
            if (isSupportedProtocolVersion(requested))
                protocolVersion = requested;
        }
        JSON::Value capabilities = JSON::Value::object();
        capabilities.set("tools", JSON::Value::object());
        JSON::Value serverInfo = JSON::Value::object();
        serverInfo.set("name", JSON::Value::string(serverName));
        serverInfo.set("version", JSON::Value::string(std::string(Protocol::Sync::appVersion())));
        JSON::Value result = JSON::Value::object();
        result.set("protocolVersion", JSON::Value::string(protocolVersion));
        result.set("capabilities", std::move(capabilities));
        result.set("serverInfo", std::move(serverInfo));
        sendResult(id, std::move(result));
        return;
    }
    if (method == "tools/list") {
        JSON::Value result = JSON::Value::object();
        result.set("tools", buildToolCatalog());
        sendResult(id, std::move(result));
        return;
    }
    if (method == "tools/call") {
        const JSON::Value* parameters = requestParameters(message);
        std::string name = parameters ? parameters->stringOr("name") : std::string();
        JSON::Value arguments = JSON::Value::object();
        if (parameters) {
            if (const JSON::Value* a = parameters->find("arguments"))
                arguments = *a;
        }
        if (name.empty()) {
            sendError(id, -32602, "tools/call: missing tool name");
            return;
        }
        const Protocol::Control::MethodDescriptor* tool = Protocol::Control::findMCPTool(name);
        if (!tool || !tool->principals.contains(Protocol::Control::Principal::Agent)) {
            JSON::Value details = JSON::Value::object();
            details.set("tool", JSON::Value::string(name));
            sendError(id, -32602, "unknown tool: " + name, details);
            return;
        }
        // Validated whole before any gate or attach, so a misspelt argument is
        // an answer rather than a call run without it.
        if (const auto violation = Protocol::Control::validate(arguments, Protocol::Control::parseRegistryJSON(tool->parametersSchema))) {
            JSON::Value details = JSON::Value::object();
            details.set("tool", JSON::Value::string(name));
            details.set("parameter", JSON::Value::string(violation->path));
            sendError(id, -32602, violation->message, details);
            return;
        }
        sendResult(id, dispatchTool(name, arguments, policy, client));
        return;
    }
    if (method == "ping") {
        sendResult(id, JSON::Value::object());
        return;
    }
    // Unhandled notifications (e.g. notifications/initialized) are silent.
    if (!isNotification)
        sendError(id, -32601, "method not found: " + method);
}

void processLine(const std::string& line, AgentPolicy& policy, MCPClient& client)
{
    bool blank = true;
    for (char c : line) {
        if (c != ' ' && c != '\t' && c != '\r') {
            blank = false;
            break;
        }
    }
    if (blank)
        return;
    bool ok = false;
    JSON::Value value = JSON::parse(line, ok);
    if (!ok) {
        sendError(nullptr, -32700, "parse error");
        return;
    }

    // JSON-RPC batch
    if (value.isArray()) {
        for (const JSON::Value& element : value.items()) {
            if (element.isObject())
                handleMessage(element, policy, client);
        }
        return;
    }
    if (value.isObject())
        handleMessage(value, policy, client);
}

// Maximum bytes allowed in the stdin accumulation buffer before a newline
// arrives. MCP JSON-RPC messages are small (tool call args, JSON text) — no
// legitimate message approaches this bound. A hostile client sending an endless
// non-newline stream is rejected at this cap rather than growing the buffer to
// OOM. 4 MiB is generous for any real MCP payload and still orders-of-magnitude
// below RAM exhaustion.
constexpr std::size_t maxMCPLineBytes = 4u * 1024u * 1024u; // 4 MiB

// Accumulate one more chunk into buffer; return false if the cap would be
// exceeded. Factored for unit-testing: the caller decides how to handle false
// (drop connection, return error). buffer is not modified on overflow.
bool appendMCPChunk(std::string& buffer, std::string_view chunk)
{
    if (buffer.size() + chunk.size() > maxMCPLineBytes)
        return false;
    buffer.append(chunk);
    return true;
}

} // namespace

int runMCP(AgentPolicy& policy, const std::string& remoteHost)
{
    MCPClient client;
    client.remoteHost = remoteHost;
    std::string buffer;
    char chunk[8192];
    for (;;) {
        std::size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            processLine(line, policy, client);
        }
        ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (n > 0) {
            if (!appendMCPChunk(buffer, { chunk, static_cast<std::size_t>(n) })) {
                sendError(nullptr, -32700, "line too long: MCP input exceeded buffer cap");
                return 1; // drop connection — fail closed
            }
            continue;
        }
        if (!n)
            break; // stdin EOF — client closed
        if (errno == EINTR)
            continue;
        break;
    }
    if (!buffer.empty())
        processLine(buffer, policy, client); // trailing line without a newline
    return 0;
}

} // namespace Agent
} // namespace Ubiquitty
