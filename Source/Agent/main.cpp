// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Agent/main.cpp — `ubq`, the command line for people and agents.
//
// Every verb is a call to an operation the method registry describes
// (Source/Core/Protocol/Control/MethodRegistry.cpp): the command line turns
// its arguments into the operation's parameters, validates them against the
// registry's schema before anything reaches the daemon, runs the operation,
// and prints its result as text, or as the JSON the schema describes under
// --json. `ubq api call` reaches the same operations with the parameters
// given as JSON, so a script and a person run one code path.
//
// A verb aimed at a session or pane takes -s <session|label> or
// -p <pane|label>; inside a pane, with neither given, it acts `here`. `label`
// and `tag` also take -w <workspace>, -W <window> and -t <tab>. -H
// [user@]host reaches that host's daemon over ssh instead of this machine's.
//

#include "Agent/AgentMCP.h"
#include "Agent/AgentSession.h"

#include "Agent/CompletionScripts.h"
#include "Agent/SessionListing.h"

#include "Core/App/SessionCreationText.h"
#include "Core/App/SessionOutline.h"
#include "Core/App/SessionOutlineText.h"
#include "Core/Codec/JSON.h"
#include "Core/Codec/PNGDecoder.h"
#include "Core/Codec/ZlibInflate.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Runtime/HostOutline.h"
#include "Core/Session/EventLog.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LaunchContext.h"
#include "Core/Session/SessionTree.h"
#include "Core/Transport/SSHHost.h"
#include "Platform/Host/HostPaths.h"

#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using Ubiquitty::Agent::AgentSession;
using Ubiquitty::Agent::EventStart;
using Ubiquitty::Agent::ListedPane;
using Ubiquitty::Agent::sessionJSON;
using Ubiquitty::Codec::JSON::Value;
using Ubiquitty::Security::AgentPolicy;
using Ubiquitty::Security::ApprovalMode;
using Ubiquitty::Security::InjectionVerdict;
namespace Control = Ubiquitty::Protocol::Control;
namespace Identity = Ubiquitty::Session::Identity;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Tree = Ubiquitty::Session;

// What `ubq` returns to a shell, and the only place the numbers live.
// Source/Agent/README.md tabulates them; `run` returns the command's own
// status instead when the command ran, which --json tells apart.
enum class ExitCode : int {
    Success = 0,
    // No daemon, no first screen, a carrier that closed, or a write that failed.
    Unavailable = 1,
    // An argument the operation's schema refuses, or a verb or operation that
    // does not exist.
    Usage = 2,
    // The daemon refused the operation itself: a split or a label in a
    // workspace a person is using or the token holds no grant for, or a split
    // the tab has no room for.
    Refused = 3,
    // A `deny` rule, or a `confirm` the invocation did not answer with --yes.
    BlockedByPolicy = 4,
    ConsentDenied = 5,
    ConsentTimedOut = 6,
    // The daemon refused the token, or its scope does not reach the operation.
    AuthDenied = 7,
    // A target named nothing live, named more than one object, or `here` was
    // used outside a pane.
    NotFound = 8,
    // A wait or a command ended by its timeout rather than by what it awaited.
    TimedOut = 9,
    // A wait from a cursor the daemon's bounded log has already dropped
    // events after, so what it waited for may have happened unseen.
    EventsLost = 10,
    // An unmarked shell settled, but no command status was reported.
    Settled = 11,
};

constexpr int exitStatus(ExitCode code) { return static_cast<int>(code); }

// The JSON-RPC error codes the control socket answers with, reused so a
// script reads one vocabulary from either surface.
constexpr int invalidParametersCode = -32602;
constexpr int failedCode = -32000;

// What the whole invocation asked for, whatever the verb.
struct Invocation {
    std::string host;
    bool isJSON = false;
    // Whether --yes counts: it was given, from a person's terminal.
    bool isApproved = false;
    // Whether --yes was given where it does not count.
    bool isApprovalIgnored = false;
    // The session or pane -s or -p named for a call addressed to a pane by
    // its handle, which only the connection ubq opens can know.
    Value paneTarget = Value::object();
};

// What an operation produced: a result the schema describes and the text a
// person reads, or a failure with its exit code.
struct Outcome {
    ExitCode code = ExitCode::Success;
    Value result = Value::object();
    std::string text;
    std::string error;
    Value errorData = Value::object();
    // Set by `run`: the reported command status, or its distinct settled code.
    std::optional<int> processStatus;
    // Whether the result prints as JSON without --json: `api call` speaks
    // JSON to a script whatever the operation it called.
    bool isAlwaysJSON = false;
};

Outcome failure(ExitCode code, std::string message, std::string kind = "failed")
{
    Outcome outcome;
    outcome.code = code;
    outcome.error = std::move(message);
    outcome.errorData.set("kind", Value::string(std::move(kind)));
    return outcome;
}

std::string formatID(Identity::ObjectKind kind, std::uint64_t number)
{
    return number ? Identity::format({ kind, number }) : std::string();
}

// Milliseconds from a `timeout` parameter in seconds; nothing when absent.
std::optional<int> timeoutMilliseconds(const Value& params)
{
    const Value* timeout = params.find("timeout");
    if (!timeout || !timeout->isNumber())
        return std::nullopt;
    const double milliseconds = timeout->asNumber() * 1000.0;
    return milliseconds > INT_MAX ? INT_MAX : static_cast<int>(milliseconds);
}

std::int64_t integerParameter(const Value& params, const char* name, std::int64_t fallback)
{
    const Value* value = params.find(name);
    return value && value->isNumber() ? static_cast<std::int64_t>(value->asNumber()) : fallback;
}

// What an operation runs with besides its parameters.
struct Context {
    const Invocation& invocation;
    const AgentPolicy& policy;
    // The scopes the token must carry for this invocation, which a refusal
    // names: the verb's own, and what an option it was given needs besides.
    Sync::ScopeSet scopes {};
};

// ---- connections -----------------------------------------------------------

// A set of scopes as a person reads it: "observe, control and region".
std::string scopeWords(Sync::ScopeSet scopes)
{
    const Value names = Control::scopeNames(scopes);
    std::string text;
    const std::size_t count = names.items().size();
    for (std::size_t index = 0; index < count; ++index) {
        if (index)
            text += index + 1 == count ? " and " : ", ";
        text += names.items()[index].asString();
    }
    return text;
}

// How a person gets a token that carries the scopes, from the host that
// checks it: this machine's, or the one -H names.
std::string issueAdvice(Sync::ScopeSet scopes, const Invocation& invocation)
{
    const std::string command = "`ubiquitty-host token issue --scope " + Sync::scopeMaskToString(scopes) + "`";
    if (invocation.host.empty())
        return "Issue one with " + command + " and give it to ubq in UBIQUITTY_AGENT_TOKEN or the agent-token file.";
    return "Issue one on " + invocation.host + " with " + command + " and give it to ubq in UBQ_REMOTE_TOKEN.";
}

// Says that the token lacks a scope the invocation needs: which ones the verb
// needs, why ubq knows this one lacks them, and how to get a token that
// carries them.
Outcome scopeFailure(std::string_view verb, const Context& context, std::string_view because)
{
    const bool isOne = Control::scopeNames(context.scopes).items().size() == 1;
    std::string message = std::string(verb) + ": " + std::string(verb) + " needs a token with the " + scopeWords(context.scopes) + (isOne ? " scope" : " scopes");
    message += "; " + std::string(because) + ' ' + issueAdvice(context.scopes, context.invocation);
    Outcome outcome = failure(ExitCode::AuthDenied, std::move(message), "auth_denied");
    outcome.errorData.set("scopes", Control::scopeNames(context.scopes));
    return outcome;
}

// Reports why a connection could not be made, in the words a person can act on.
Outcome connectionFailure(const AgentSession& session, const char* verb, const Context& context)
{
    if (session.consentDenied()) {
        const bool isTimeout = session.consentDeniedReason() == Ubiquitty::Protocol::Sync::ConsentStatusPhase::Expired;
        return failure(isTimeout ? ExitCode::ConsentTimedOut : ExitCode::ConsentDenied,
            std::string(verb) + (isTimeout ? ": timed out awaiting the person's consent" : ": the person using the session refused"), "consent");
    }
    if (session.authDenied()) {
        using Reason = Sync::ByeReason;
        const Reason reason = session.authDeniedReason();
        if (reason == Reason::OutsideScope && !context.scopes.isEmpty()) {
            const std::string detail = session.closingDetail();
            return scopeFailure(verb, context, detail.empty() ? std::string("the host refused this one as outside its scope.") : "the host refused this one: " + detail);
        }
        // A session that went between the listing and the attach is a target
        // that names nothing; a host of another build, a full one, or one that
        // takes no second handshake is a daemon ubq cannot use. Every other
        // refusal is of the token, or of what it holds no grant for.
        if (reason == Reason::SessionGone)
            return failure(ExitCode::NotFound, std::string(verb) + ": " + session.refusalText(), "target");
        if (reason == Reason::VersionMismatch || reason == Reason::HostFull || reason == Reason::DuplicateHandshake || reason == Reason::RemoteHostRefused)
            return failure(ExitCode::Unavailable, std::string(verb) + ": " + session.refusalText(), "unavailable");
        return failure(ExitCode::AuthDenied, std::string(verb) + ": " + session.refusalText(), "auth_denied");
    }
    if (session.awaitingConsent())
        return failure(ExitCode::ConsentTimedOut, std::string(verb) + ": timed out awaiting the person's consent", "consent");
    if (!session.connectFailure().empty())
        return failure(ExitCode::Unavailable, std::string(verb) + ": " + session.connectFailure(), "unavailable");
    if (session.exited() && session.sawHelloAck())
        return failure(ExitCode::Unavailable, std::string(verb) + ": the session ended while attaching", "unavailable");
    return failure(ExitCode::Unavailable, std::string(verb) + ": no daemon answered (start one: ubiquitty-host --ensure-daemon)", "unavailable");
}

// Reports input the host did not take, or did not say it wrote, in words a
// person can act on; nothing when it was written.
std::optional<Outcome> inputFailure(AgentSession::InputDelivery delivery, const AgentSession& session, std::string_view verb, std::string_view what)
{
    using Delivery = AgentSession::InputDelivery;
    switch (delivery) {
    case Delivery::Written:
        return std::nullopt;
    case Delivery::NotSent: {
        // A host that closed rather than take the input says why.
        const std::string detail = session.closingDetail();
        return failure(ExitCode::Unavailable, std::string(verb) + ": " + std::string(what) + " could not be sent" + (detail.empty() ? std::string() : "; the host said: " + detail), "unavailable");
    }
    case Delivery::Unconfirmed:
        return failure(ExitCode::TimedOut,
            std::string(verb) + ": the daemon did not confirm writing " + std::string(what) + " to the session within "
                + std::to_string(AgentSession::inputWrittenWaitMilliseconds / 1000) + " s; it may be written later, or not at all",
            "unconfirmed");
    }
    return std::nullopt;
}

// Turns a capture the host did not complete into the verb's failure: a
// refusal of the token's authority exits as one, as `read`'s does, and
// anything else as a refusal.
Outcome captureFailure(std::string_view verb, const std::optional<Control::CaptureReply>& captured, const AgentSession& session, std::string_view request)
{
    if (!captured)
        return failure(ExitCode::Refused, std::string(verb) + ": " + session.unansweredText(request), "refused");
    const bool isAuthority = captured->status == Control::CaptureReplyStatus::Refused
        && (captured->errorCode == static_cast<std::int32_t>(Control::ErrorCode::Forbidden) || captured->errorCode == static_cast<std::int32_t>(Control::ErrorCode::Unauthorized));
    return failure(isAuthority ? ExitCode::AuthDenied : ExitCode::Refused, std::string(verb) + ": " + captured->reason, isAuthority ? "auth_denied" : "refused");
}

bool openControl(AgentSession& session, const Invocation& invocation)
{
    session.setAgentLabel("ubq");
    if (!invocation.host.empty())
        session.setRemoteHost(invocation.host);
    return session.connectControl();
}

/// What an attached verb reads.
enum class ScreenUse : std::uint8_t {
    /// The live screen: the pane is set `Live` while the verb runs.
    Screen,
    /// Records and regions only: the pane stays `RecordsOnly`.
    Records,
};

bool openAttached(AgentSession& session, const Invocation& invocation, std::uint32_t sessionID, int columns = 80, int rows = 24, ScreenUse use = ScreenUse::Screen)
{
    session.setAgentLabel("ubq");
    if (!invocation.host.empty())
        session.setRemoteHost(invocation.host);
    session.setConsentPendingNotice([](std::uint32_t held) {
        std::fprintf(stderr, "ubq: session:%u is in use; awaiting its person's consent…\n", static_cast<unsigned>(held));
    });
    if (!session.connect(columns, rows, sessionID))
        return false;
    return use == ScreenUse::Screen ? session.waitForFirstScreen(session.answerWaitMilliseconds()) : session.waitForAttach(session.answerWaitMilliseconds());
}

// ---- targets ---------------------------------------------------------------

struct Target {
    std::uint32_t session = 0;
    std::uint32_t pane = 0;
};

// The identities this process runs inside, when it runs in a pane of the
// daemon being asked. Every daemon and the app number their own objects, so
// the environment's IDs name nothing on a connection to any other: a remote
// daemon, a daemon found by probing, or a pane the app runs itself.
std::vector<Identity::ObjectID> hereIdentities(const AgentSession& control, const Invocation& invocation)
{
    if (!invocation.host.empty() || !control.isConnectedToPaneDaemon())
        return {};
    return Ubiquitty::Session::contextFromEnvironment([](const char* name) -> const char* { return ::getenv(name); }).identities();
}

// Resolves the operation's `session` or `pane` parameter, or `here` when
// neither is given, against what the daemon lists.
std::optional<Target> resolveTarget(const Value& params, AgentSession& control, const Invocation& invocation, Outcome& refused)
{
    std::string text;
    Identity::ObjectKind kind = Identity::ObjectKind::Session;
    if (const Value* pane = params.find("pane")) {
        text = pane->asString();
        kind = Identity::ObjectKind::Pane;
    } else if (const Value* session = params.find("session")) {
        text = session->asString();
    } else {
        const auto here = hereIdentities(control, invocation);
        if (here.empty()) {
            refused = failure(ExitCode::NotFound, "name a session with -s or a pane with -p; this process does not run in a pane of the daemon being asked", "no_target");
            return std::nullopt;
        }
        text = std::string(Identity::hereTarget);
        kind = std::ranges::any_of(here, [](const auto& id) { return id.kind == Identity::ObjectKind::Pane; }) ? Identity::ObjectKind::Pane : Identity::ObjectKind::Session;
    }
    std::vector<Ubiquitty::Session::SessionRow> sessions;
    if (!control.listSessions(sessions)) {
        refused = failure(ExitCode::Unavailable, "the daemon did not list its sessions", "unavailable");
        return std::nullopt;
    }
    std::vector<Identity::Candidate> candidates;
    for (const auto& row : sessions) {
        // A workspace the host could restore has no session to target.
        if (row.sessionID)
            candidates.push_back({ { Identity::ObjectKind::Session, row.sessionID }, row.label });
        if (row.paneID)
            candidates.push_back({ { Identity::ObjectKind::Pane, row.paneID }, row.paneLabel });
    }
    const auto here = hereIdentities(control, invocation);
    const Identity::Resolution resolution = Identity::resolveTarget(text, candidates, kind, here);
    if (resolution.outcome != Identity::ResolutionOutcome::Resolved) {
        refused = failure(ExitCode::NotFound, resolution.message, "target");
        Value matches = Value::array();
        for (const auto& id : resolution.candidates)
            matches.push(Value::string(Identity::format(id)));
        refused.errorData.set("candidates", std::move(matches));
        return std::nullopt;
    }
    Target target;
    for (const auto& info : sessions) {
        if ((kind == Identity::ObjectKind::Session && info.id == resolution.id.number) || (kind == Identity::ObjectKind::Pane && info.paneID == resolution.id.number)) {
            target.session = info.id;
            target.pane = info.paneID;
        }
    }
    return target;
}

// ---- policy ----------------------------------------------------------------

bool isAllowedByPolicy(const AgentPolicy& policy, std::uint32_t sessionID, const char* kind, const std::string& detail, const Invocation& invocation, Outcome& refused, AgentPolicy::Subject subject = AgentPolicy::Subject::AgentSupplied)
{
    switch (Ubiquitty::Security::evaluateAction(policy, sessionID, kind, detail, invocation.isApproved ? ApprovalMode::AutoApprove : ApprovalMode::RequireConfirmation, Ubiquitty::Security::AuditOnAllow::Record, subject)) {
    case InjectionVerdict::Allowed:
        return true;
    case InjectionVerdict::DeniedByRule:
        refused = failure(ExitCode::BlockedByPolicy, "blocked by policy (deny): " + detail, "policy");
        return false;
    case InjectionVerdict::NeedsApproval:
        if (subject == AgentPolicy::Subject::AgentSupplied && Ubiquitty::Security::isHeldForShellIndirection(policy, detail)) {
            refused = failure(ExitCode::BlockedByPolicy,
                "requires confirmation: " + std::string(Ubiquitty::Security::shellIndirectionHold)
                    + (invocation.isApprovalIgnored ? ", and --yes counts only when typed at a person's terminal: " : "; re-run with --yes: ") + detail,
                "policy");
            return false;
        }
        refused = failure(ExitCode::BlockedByPolicy,
            invocation.isApprovalIgnored ? "requires confirmation, and --yes counts only when typed at a person's terminal; add an 'allow' policy rule: " + detail
                                         : "requires confirmation; re-run with --yes, or add an 'allow' policy rule: " + detail,
            "policy");
        return false;
    }
    return false;
}

// The session a target names by number, for the audit line a policy check
// writes before anything is resolved; zero for a label, a pane or here.
std::uint32_t namedSessionNumber(const Value& params)
{
    const std::string text = params.stringOr("session");
    if (const auto id = Identity::parseObjectID(text); id && id->kind == Identity::ObjectKind::Session && id->number <= UINT32_MAX)
        return static_cast<std::uint32_t>(id->number);
    if (!text.empty() && text.size() < 10 && std::ranges::all_of(text, [](char character) { return character >= '0' && character <= '9'; }))
        return static_cast<std::uint32_t>(std::stoul(text));
    return 0;
}

// ---- operations ------------------------------------------------------------

// Asks the host what the token carries, and refuses an invocation it does not
// carry every scope for, before the invocation types or makes anything: a
// refusal found after typing would follow an effect. Called on a control
// connection the invocation opens anyway.
bool isTokenSufficient(AgentSession& control, const char* verb, const Context& context, Outcome& refused);

// ---- the host's tree -------------------------------------------------------

// Lists the host on a control connection, or says why it could not be.
std::optional<AgentSession::HostTree> listHost(AgentSession& control, const char* verb, const Context& context, Outcome& refused)
{
    std::optional<AgentSession::HostTree> host = control.listHost();
    if (!host)
        refused = control.authDenied() ? connectionFailure(control, verb, context) : failure(ExitCode::Unavailable, std::string(verb) + ": the daemon did not list itself", "unavailable");
    return host;
}

// Every pane a session draws into, by session, which is the order sessions
// were made in.
std::vector<ListedPane> sessionPanes(const Tree::SessionTree& tree)
{
    std::vector<ListedPane> listed;
    for (const Tree::TreeWorkspace& workspace : tree.workspaces) {
        for (const Tree::TreeWindow& window : workspace.windows) {
            for (const Tree::TreeTab& tab : window.tabs) {
                for (const Tree::TreePane& pane : tab.panes) {
                    if (pane.sessionID != 0)
                        listed.push_back({ &window, &tab, &pane });
                }
            }
        }
    }
    std::ranges::sort(listed, {}, [](const ListedPane& entry) { return entry.pane->sessionID; });
    return listed;
}

Outcome listSessions(const Value&, const Context& context)
{
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "ls", context);
    Outcome outcome;
    const std::optional<AgentSession::HostTree> host = listHost(control, "ls", context, outcome);
    if (!host)
        return outcome;
    Value list = Value::array();
    for (const ListedPane& listed : sessionPanes(host->tree)) {
        const Tree::TreePane& pane = *listed.pane;
        list.push(sessionJSON(listed));
        outcome.text += formatID(Identity::ObjectKind::Session, pane.sessionID) + '\t' + (pane.sessionLabel.empty() ? "-" : pane.sessionLabel) + '\t' + formatID(Identity::ObjectKind::Pane, std::to_underlying(pane.id)) + '\t'
            + std::to_string(pane.columns()) + 'x' + std::to_string(pane.rows()) + '\t' + (pane.facts.isAtPrompt ? "idle" : "busy") + '\t' + pane.program + '\t' + pane.directory() + '\n';
    }
    outcome.result.set("sessions", std::move(list));
    return outcome;
}

Outcome printTree(const Value& params, const Context& context)
{
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "tree", context);
    Outcome outcome;
    const std::optional<AgentSession::HostTree> host = listHost(control, "tree", context, outcome);
    if (!host)
        return outcome;
    // The outline every reader of a host draws, through the one entry that
    // builds it: this person's title compositions and tag rules, and the
    // colours a reader with no theme is given.
    Ubiquitty::Runtime::HostOutlineRequest request;
    request.query = params.stringOr("query");
    const Ubiquitty::App::SessionOutline outline = Ubiquitty::Runtime::buildHostOutline(host->tree, Ubiquitty::Preferences::values(), Ubiquitty::Themes::StatusColors {}, request, host->hostNowUnixMilliseconds);
    outcome.result = Ubiquitty::App::outlineJSON(outline);
    Value revisions = Value::array();
    for (const auto& workspace : host->tree.workspaces) {
        Value entry = Value::object();
        entry.set("workspace", Value::string(formatID(Identity::ObjectKind::Workspace, workspace.id)));
        entry.set("revision", Value::string(std::to_string(workspace.revision)));
        revisions.push(std::move(entry));
    }
    outcome.result.set("workspaceRevisions", std::move(revisions));
    outcome.text = Ubiquitty::App::outlineText(outline);
    return outcome;
}

// ---- calls to the host -----------------------------------------------------

// Makes a host call and returns its result, or turns the host's refusal into
// this command's failure, in the host's own words: they say what was refused
// and what would have been needed.
std::optional<Value> callHost(AgentSession& control, const char* verb, std::string_view method, const Value& parameters, const Context& context, Outcome& refused)
{
    std::optional<Value> answer = control.callHost(method, parameters);
    if (!answer) {
        refused = control.authDenied() ? connectionFailure(control, verb, context) : failure(ExitCode::Unavailable, std::string(verb) + ": the daemon did not answer", "unavailable");
        return std::nullopt;
    }
    if (const Value* error = answer->find("error")) {
        const auto code = static_cast<Control::ErrorCode>(static_cast<int>(error->numberOr("code")));
        const ExitCode exit = code == Control::ErrorCode::NotFound ? ExitCode::NotFound : code == Control::ErrorCode::InvalidParameters ? ExitCode::Usage
                                                                                                                                        : ExitCode::Refused;
        refused = failure(exit, error->stringOr("message", std::string(verb) + ": the daemon refused"), std::string(Control::errorKind(code)));
        return std::nullopt;
    }
    const Value* result = answer->find("result");
    return result ? *result : Value::object();
}

bool isTokenSufficient(AgentSession& control, const char* verb, const Context& context, Outcome& refused)
{
    const std::optional<Value> described = callHost(control, verb, "host.describe", Value::object(), context, refused);
    if (!described)
        return false;
    Sync::ScopeSet held;
    if (const Value* names = described->find("scopes"); names && names->isArray()) {
        for (const Value& name : names->items()) {
            for (const Sync::Scope scope : { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::RegionRead, Sync::Scope::NewSession, Sync::Scope::FileRead, Sync::Scope::Manage }) {
                if (name.isString() && name.asString() == Sync::scopeMaskToString(scope))
                    held.add(scope);
            }
        }
    }
    Sync::ScopeSet missing = context.scopes;
    missing.remove(held);
    if (missing.isEmpty())
        return true;
    refused = scopeFailure(verb, context, "the host says this one carries " + Sync::scopeMaskToString(held) + ", without " + scopeWords(missing) + ", so nothing was sent.");
    refused.errorData.set("missing", Control::scopeNames(missing));
    return false;
}

// ---- objects of every level ------------------------------------------------
// ---- objects of every level ------------------------------------------------

// The parameter an object of a kind is named by.
struct Level {
    const char* parameter;
    Identity::ObjectKind kind;
};
constexpr Level levels[] {
    { "workspace", Identity::ObjectKind::Workspace },
    { "window", Identity::ObjectKind::Window },
    { "tab", Identity::ObjectKind::Tab },
    { "pane", Identity::ObjectKind::Pane },
    { "session", Identity::ObjectKind::Session },
};

// Everything of a kind a tree lists, each with the label a target may name it
// by.
std::vector<Identity::Candidate> candidatesOf(const Tree::SessionTree& tree, Identity::ObjectKind kind)
{
    std::vector<Identity::Candidate> candidates;
    const auto add = [&](std::uint64_t number, std::string_view label) {
        if (number != 0)
            candidates.push_back({ { kind, number }, label });
    };
    for (const Tree::TreeWorkspace& workspace : tree.workspaces) {
        if (kind == Identity::ObjectKind::Workspace) {
            add(workspace.id, workspace.label);
            continue;
        }
        for (const Tree::TreeWindow& window : workspace.windows) {
            if (kind == Identity::ObjectKind::Window)
                add(std::to_underlying(window.id), window.label);
            for (const Tree::TreeTab& tab : window.tabs) {
                if (kind == Identity::ObjectKind::Tab)
                    add(std::to_underlying(tab.id), tab.label);
                for (const Tree::TreePane& pane : tab.panes) {
                    if (kind == Identity::ObjectKind::Pane)
                        add(std::to_underlying(pane.id), pane.label);
                    if (kind == Identity::ObjectKind::Session)
                        add(pane.sessionID, pane.sessionLabel);
                }
            }
        }
    }
    return candidates;
}

// Resolves the one object the parameters name, at whichever level, against
// the host's tree; with none named, `here` at the level given.
std::optional<Identity::ObjectID> resolveObject(const Value& params, Identity::ObjectKind whenUnnamed, AgentSession& control, const Invocation& invocation, const Tree::SessionTree& tree, Outcome& refused)
{
    std::string text(Identity::hereTarget);
    std::optional<Identity::ObjectKind> kind;
    for (const Level& level : levels) {
        const Value* named = params.find(level.parameter);
        if (!named)
            continue;
        if (kind) {
            refused = failure(ExitCode::Usage, "name one object: a workspace with -w, a window with -W, a tab with -t, a pane with -p or a session with -s", "bad_request");
            return std::nullopt;
        }
        kind = level.kind;
        text = named->asString();
    }
    const std::vector<Identity::ObjectID> here = hereIdentities(control, invocation);
    if (!kind && here.empty()) {
        refused = failure(ExitCode::NotFound, "name a workspace with -w, a window with -W, a tab with -t, a pane with -p or a session with -s; this process does not run in a pane of the daemon being asked", "no_target");
        return std::nullopt;
    }
    const std::vector<Identity::Candidate> candidates = candidatesOf(tree, kind.value_or(whenUnnamed));
    const Identity::Resolution resolution = Identity::resolveTarget(text, candidates, kind.value_or(whenUnnamed), here);
    if (resolution.outcome != Identity::ResolutionOutcome::Resolved) {
        refused = failure(ExitCode::NotFound, resolution.message, "target");
        Value matches = Value::array();
        for (const auto& id : resolution.candidates)
            matches.push(Value::string(Identity::format(id)));
        refused.errorData.set("candidates", std::move(matches));
        return std::nullopt;
    }
    return resolution.id;
}

// Refuses a label the label book would refuse, before anything is made or
// changed, so a refusal never follows an effect.
bool isLabelAccepted(const std::string& label, Outcome& refused)
{
    if (label.empty())
        return true;
    if (const Identity::LabelVerdict verdict = Identity::checkLabel(label); verdict != Identity::LabelVerdict::Accepted) {
        refused = failure(ExitCode::Usage, std::string(Identity::labelVerdictSentence(verdict)), "bad_request");
        refused.errorData.set("parameter", Value::string("/label"));
        return false;
    }
    return true;
}

Outcome newSession(const Value& params, const Context& context)
{
    const int columns = static_cast<int>(integerParameter(params, "columns", 80));
    const int rows = static_cast<int>(integerParameter(params, "rows", 24));
    const std::string label = params.stringOr("label");
    Outcome refused;
    if (!isLabelAccepted(label, refused))
        return refused;
    if (!isAllowedByPolicy(context.policy, 0, "new", std::to_string(columns) + "x" + std::to_string(rows), context.invocation, refused, AgentPolicy::Subject::Description))
        return refused;
    const std::string command = params.stringOr("command");
    if (!command.empty() && !isAllowedByPolicy(context.policy, 0, "run", command, context.invocation, refused))
        return refused;
    // The token is checked on the connection that later finds the new pane,
    // before the session is made: a refusal must not follow a new shell.
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "new", context);
    if (!isTokenSufficient(control, "new", context, refused))
        return refused;
    AgentSession session;
    if (!openAttached(session, context.invocation, 0, columns, rows))
        return connectionFailure(session, "new", context);
    const std::uint32_t sessionID = session.sessionID();
    std::uint32_t pane = 0;
    {
        std::vector<Ubiquitty::Session::SessionRow> sessions;
        if (control.listSessions(sessions)) {
            for (const auto& row : sessions) {
                if (row.sessionID == sessionID)
                    pane = row.paneID;
            }
        }
        if (!label.empty()) {
            Value call = Value::object();
            call.set("object", Value::string(formatID(Identity::ObjectKind::Session, sessionID)));
            call.set("label", Value::string(label));
            if (!callHost(control, "new", "label.set", call, context, refused)) {
                session.detach();
                refused.error = Ubiquitty::App::sessionCreatedWithoutLabelText(sessionID, pane, refused.error, !command.empty());
                refused.errorData.set("createdSession", Value::string(formatID(Identity::ObjectKind::Session, sessionID)));
                refused.errorData.set("createdPane", Value::string(formatID(Identity::ObjectKind::Pane, pane)));
                refused.errorData.set("labelApplied", Value::boolean(false));
                refused.errorData.set("commandStarted", Value::boolean(false));
                return refused;
            }
        }
    }
    if (!command.empty()) {
        if (std::optional<Outcome> unsent = inputFailure(session.sendInput(command + "\n"), session, "new", "its command")) {
            session.detach();
            unsent->errorData.set("createdSession", Value::string(formatID(Identity::ObjectKind::Session, sessionID)));
            unsent->errorData.set("createdPane", Value::string(formatID(Identity::ObjectKind::Pane, pane)));
            unsent->errorData.set("commandStarted", Value::boolean(false));
            return std::move(*unsent);
        }
        session.pump(200);
    }
    session.detach();
    Outcome outcome;
    outcome.result.set("session", Value::string(formatID(Identity::ObjectKind::Session, sessionID)));
    outcome.result.set("pane", Value::string(formatID(Identity::ObjectKind::Pane, pane)));
    outcome.text = formatID(Identity::ObjectKind::Session, sessionID) + '\n';
    return outcome;
}

Outcome runCommand(const Value& params, const Context& context)
{
    const std::string command = params.stringOr("command");
    Outcome refused;
    // Screened before any connection, so a refused line reaches no daemon.
    if (!isAllowedByPolicy(context.policy, namedSessionNumber(params), "run", command, context.invocation, refused))
        return refused;
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "run", context);
    if (!isTokenSufficient(control, "run", context, refused))
        return refused;
    const auto target = resolveTarget(params, control, context.invocation, refused);
    if (!target)
        return refused;
    control.detach();
    AgentSession session;
    if (!openAttached(session, context.invocation, target->session))
        return connectionFailure(session, "run", context);
    const auto run = session.runCommandWithExit(command, timeoutMilliseconds(params), AgentSession::RunObservation::Events);
    session.detach();
    if (run.completion == AgentSession::RunCompletion::TimedOut)
        return failure(ExitCode::TimedOut, "run: the command did not finish before its timeout and may still be running; ubq read shows its screen", "timeout");
    if (run.inputDelivery) {
        if (std::optional<Outcome> unsent = inputFailure(*run.inputDelivery, session, "run", "the command"))
            return std::move(*unsent);
    }
    if (run.completion == AgentSession::RunCompletion::Unavailable)
        return failure(ExitCode::Unavailable, "run: " + session.unansweredText("the command completion"), "unavailable");
    if (!run.capture || run.capture->status != Control::CaptureReplyStatus::Complete)
        return captureFailure("run", run.capture, session, "the command capture");
    if (!run.cursor)
        return failure(ExitCode::Unavailable, "run: the host did not report the event cursor");
    Outcome outcome;
    outcome.result.set("session", Value::string(formatID(Identity::ObjectKind::Session, target->session)));
    outcome.result.set("output", Value::string(run.capture->text));
    outcome.result.set("cursor", Value::number(static_cast<double>(*run.cursor)));
    if (run.completion == AgentSession::RunCompletion::Settled) {
        outcome.result.set("outcome", Value::string("settled"));
        outcome.processStatus = exitStatus(ExitCode::Settled);
    } else if (run.exitStatus && *run.exitStatus != Ubiquitty::Session::noExitStatus) {
        outcome.result.set("exitStatus", Value::number(*run.exitStatus));
        outcome.processStatus = *run.exitStatus & 0xFF;
    }
    outcome.text = run.capture->text;
    if (!outcome.text.empty() && outcome.text.back() != '\n')
        outcome.text.push_back('\n');
    if (run.completion == AgentSession::RunCompletion::Settled)
        outcome.text += "[settled; command status unknown]\n";
    return outcome;
}

Outcome sendText(const Value& params, const Context& context)
{
    // The policy screens the line as typed; Return after it is what `run`
    // adds too, and is not part of what a rule matches.
    const std::string line = params.stringOr("text");
    std::string text = line;
    if (params.boolOr("enter"))
        text.push_back('\n');
    Outcome refused;
    if (!isAllowedByPolicy(context.policy, namedSessionNumber(params), "send", line, context.invocation, refused))
        return refused;
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "send", context);
    if (!isTokenSufficient(control, "send", context, refused))
        return refused;
    const auto target = resolveTarget(params, control, context.invocation, refused);
    if (!target)
        return refused;
    control.detach();
    AgentSession session;
    if (!openAttached(session, context.invocation, target->session))
        return connectionFailure(session, "send", context);
    // The cursor the stream stood at before the text went in, so a later
    // `ubq wait --since` hears everything the text caused. The daemon answers
    // a subscription with that position first; the text is not typed until
    // it has, because no other cursor is the one from before the text.
    if (!session.subscribeEvents(EventStart::FromLatest))
        return failure(ExitCode::Unavailable, "send: the host did not acknowledge the event subscription");
    std::optional<std::uint64_t> cursor;
    session.pumpUntil(30000, [&] {
        for (const auto& event : session.takeEvents()) {
            if (event.kind == Ubiquitty::Session::EventKind::Gap && !cursor)
                cursor = event.cursor;
        }
        return cursor.has_value();
    });
    if (!cursor) {
        session.detach();
        return failure(ExitCode::Unavailable, "send: the daemon did not say where its event stream stands, so nothing was typed", "unavailable");
    }
    if (std::optional<Outcome> unsent = inputFailure(session.sendInput(text), session, "send", "the text"))
        return std::move(*unsent);
    session.pump(100);
    session.detach();
    Outcome outcome;
    outcome.result.set("sent", Value::number(static_cast<double>(text.size())));
    outcome.result.set("cursor", Value::number(static_cast<double>(*cursor)));
    return outcome;
}

Outcome readText(const Value& params, const Context& context)
{
    const std::string source = params.stringOr("source", "screen");
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "read", context);
    Outcome refused;
    const auto target = resolveTarget(params, control, context.invocation, refused);
    if (!target)
        return refused;
    control.detach();
    AgentSession session;
    // The last command is answered by a call and needs no rows. The screen
    // and the tail of the scrollback are read once the host has taken a
    // frame, which only a pane it sends rows to waits for: a read straight
    // after the attach would see the program mid-redraw.
    if (!openAttached(session, context.invocation, target->session, 80, 24, source == "last-command" ? ScreenUse::Records : ScreenUse::Screen))
        return connectionFailure(session, "read", context);
    std::string text;
    Value screen;
    {
        Control::CaptureRequest request;
        if (source == "last-command")
            request.source = Control::CaptureSource::LastCommand;
        else if (source == "scrollback") {
            request.source = Control::CaptureSource::Tail;
            request.lines = static_cast<std::uint32_t>(integerParameter(params, "lines", 100));
        }
        const auto capture = session.captureText(request);
        if (!capture || capture->status != Control::CaptureReplyStatus::Complete) {
            session.detach();
            if (!capture)
                return failure(ExitCode::Unavailable, "read: the host did not answer the capture");
            if (capture->status == Control::CaptureReplyStatus::Refused
                && capture->errorCode == static_cast<std::int32_t>(Control::ErrorCode::NotFound))
                return failure(ExitCode::NotFound, "read: " + capture->reason, source == "last-command" ? "no_command" : "not_found");
            const bool isAuthorizationRefused = capture->status == Control::CaptureReplyStatus::Refused
                && (capture->errorCode == static_cast<std::int32_t>(Control::ErrorCode::Forbidden)
                    || capture->errorCode == static_cast<std::int32_t>(Control::ErrorCode::Unauthorized));
            return failure(isAuthorizationRefused ? ExitCode::AuthDenied : ExitCode::Refused, "read: " + capture->reason);
        }
        text = capture->text;
        screen = capture->screen;
    }
    session.detach();
    Outcome outcome;
    outcome.result.set("source", Value::string(source));
    outcome.result.set("text", Value::string(text));
    if (screen.isObject())
        outcome.result.set("screen", std::move(screen));
    outcome.text = text;
    if (!outcome.text.empty() && outcome.text.back() != '\n')
        outcome.text.push_back('\n');
    return outcome;
}

// Runs pane.search over the connection that attaches to the named pane,
// whose handle the call is addressed by: the host searches every line it
// keeps, not what this connection was sent (Docs/sync.md §6.6).
Outcome searchHistory(const Value& params, const Context& context)
{
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "search", context);
    Outcome refused;
    const auto target = resolveTarget(context.invocation.paneTarget, control, context.invocation, refused);
    if (!target)
        return refused;
    control.detach();
    AgentSession session;
    if (!openAttached(session, context.invocation, target->session, 80, 24, ScreenUse::Records))
        return connectionFailure(session, "search", context);
    const std::uint32_t handle = session.paneHandle();
    if (!handle) {
        session.detach();
        return failure(ExitCode::Unavailable, "search: the host bound no pane for the session", "unavailable");
    }
    Value call = params;
    call.set("pane", Value::number(handle));
    const std::optional<Value> result = callHost(session, "search", "pane.search", call, context, refused);
    session.detach();
    if (!result)
        return refused;
    Outcome outcome;
    outcome.result = *result;
    // One match a line: the logical line's number and the cells it spans,
    // tab-separated as `ls` is.
    if (const Value* matches = result->find("matches"); matches && matches->isArray()) {
        for (const Value& match : matches->items()) {
            outcome.text += std::to_string(static_cast<std::uint64_t>(match.numberOr("line"))) + '\t'
                + std::to_string(static_cast<std::uint64_t>(match.numberOr("startCell"))) + '\t'
                + std::to_string(static_cast<std::uint64_t>(match.numberOr("endCell"))) + '\n';
        }
    }
    if (result->boolOr("truncated", false) && !context.invocation.isJSON)
        std::fprintf(stderr, "ubq: search: more lines matched than one search returns; narrow it with --from and --to\n");
    return outcome;
}

Outcome splitPane(const Value& params, const Context& context)
{
    const std::string direction = params.stringOr("direction", "right");
    Outcome refused;
    if (!isAllowedByPolicy(context.policy, namedSessionNumber(params), "split", direction == "down" ? "<split down>" : "<split right>", context.invocation, refused, AgentPolicy::Subject::Description))
        return refused;
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "split", context);
    const auto target = resolveTarget(params, control, context.invocation, refused);
    if (!target)
        return refused;
    if (!target->pane)
        return failure(ExitCode::NotFound, "split: the session fills no pane the daemon placed", "target");
    Value parameters = Value::object();
    parameters.set("pane", Value::number(target->pane));
    // A vertical divider stands the two panes side by side.
    parameters.set("orientation", Value::string(direction == "down" ? "horizontal" : "vertical"));
    parameters.set("ratio", Value::number(0.5));
    const auto result = callHost(control, "split", "workspace.split", parameters, context, refused);
    control.detach();
    if (!result)
        return refused;
    Outcome outcome;
    outcome.result = *result;
    outcome.text = result->stringOr("pane") + '\n';
    return outcome;
}

Outcome killSession(const Value& params, const Context& context)
{
    Outcome refused;
    if (!isAllowedByPolicy(context.policy, namedSessionNumber(params), "kill", "<terminate session>", context.invocation, refused, AgentPolicy::Subject::Description))
        return refused;
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "kill", context);
    const auto target = resolveTarget(params, control, context.invocation, refused);
    if (!target)
        return refused;
    using Result = AgentSession::TerminateOutcome;
    switch (control.terminateSession(target->session)) {
    case Result::Terminated:
        break;
    case Result::Denied:
        return failure(ExitCode::ConsentDenied, "kill: the person using the session refused", "consent");
    case Result::TimedOut:
        return failure(ExitCode::ConsentTimedOut, "kill: timed out awaiting the person's consent", "consent");
    case Result::Refused:
        if (control.authDenied())
            return connectionFailure(control, "kill", context);
        // The host answered with a refusal of its own: its words say why.
        if (const auto& refusal = control.terminationRefusal()) {
            const auto code = static_cast<Control::ErrorCode>(refusal->code);
            const bool isAuthority = code == Control::ErrorCode::Forbidden || code == Control::ErrorCode::Unauthorized;
            return failure(isAuthority ? ExitCode::AuthDenied : ExitCode::Refused, "kill: " + (refusal->message.empty() ? std::string("the host refused to end the session") : refusal->message),
                std::string(Control::errorKind(code)));
        }
        return failure(ExitCode::Refused, "kill: the host refused to end the session", "refused");
    case Result::Failed:
        return failure(ExitCode::Unavailable, "kill: the request could not be sent");
    }
    Outcome outcome;
    outcome.result.set("session", Value::string(formatID(Identity::ObjectKind::Session, target->session)));
    outcome.result.set("terminated", Value::boolean(true));
    return outcome;
}

Outcome labelObject(const Value& params, const Context& context)
{
    const std::string label = params.stringOr("label");
    Outcome refused;
    if (!isLabelAccepted(label, refused))
        return refused;
    // A label is where a later verb is aimed, so moving one is screened as the
    // verbs it redirects are.
    if (!isAllowedByPolicy(context.policy, namedSessionNumber(params), "label", "<label>", context.invocation, refused, AgentPolicy::Subject::Description))
        return refused;
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "label", context);
    const std::optional<AgentSession::HostTree> host = listHost(control, "label", context, refused);
    if (!host)
        return refused;
    // With nothing named, the session this process runs in.
    const std::optional<Identity::ObjectID> object = resolveObject(params, Identity::ObjectKind::Session, control, context.invocation, host->tree, refused);
    if (!object)
        return refused;
    Value call = Value::object();
    call.set("object", Value::string(Identity::format(*object)));
    call.set("label", Value::string(label));
    std::optional<Value> result = callHost(control, "label", "label.set", call, context, refused);
    if (!result)
        return refused;
    Outcome outcome;
    outcome.result = std::move(*result);
    return outcome;
}

Outcome tagObject(const Value& params, const Context& context)
{
    Value tags = Value::array();
    if (const Value* given = params.find("tags")) {
        for (const Value& tag : given->items())
            tags.push(tag);
    }
    Outcome refused;
    // A tag is drawn on a row a person reads, so putting one there is
    // screened as a label is.
    if (!isAllowedByPolicy(context.policy, namedSessionNumber(params), "tag", "<tag>", context.invocation, refused, AgentPolicy::Subject::Description))
        return refused;
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "tag", context);
    const std::optional<AgentSession::HostTree> host = listHost(control, "tag", context, refused);
    if (!host)
        return refused;
    const std::optional<Identity::ObjectID> object = resolveObject(params, Identity::ObjectKind::Session, control, context.invocation, host->tree, refused);
    if (!object)
        return refused;
    Value call = Value::object();
    call.set("object", Value::string(Identity::format(*object)));
    call.set("tags", std::move(tags));
    std::optional<Value> result = callHost(control, "tag", "tags.set", call, context, refused);
    if (!result)
        return refused;
    Outcome outcome;
    outcome.result = std::move(*result);
    return outcome;
}

std::string_view programStateName(Ubiquitty::OSC::ProgramState state)
{
    return Ubiquitty::Session::eventProgramStateName(static_cast<Ubiquitty::Session::EventProgramState>(std::to_underlying(state)));
}

Outcome programStatus(const Value& params, const Context& context)
{
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "status", context);
    Outcome refused;
    const auto target = resolveTarget(params, control, context.invocation, refused);
    if (!target)
        return refused;
    control.detach();
    AgentSession session;
    if (!openAttached(session, context.invocation, target->session))
        return connectionFailure(session, "status", context);
    const Ubiquitty::OSC::ProgramStatusStore& store = session.programStatus();
    Outcome outcome;
    Value records = Value::array();
    for (const auto& record : store.records) {
        Value entry = Value::object();
        entry.set("id", Value::string(record.id));
        entry.set("app", Value::string(std::string(store.effectiveApp(record))));
        entry.set("state", Value::string(std::string(programStateName(record.state))));
        entry.set("message", Value::string(record.message));
        entry.set("title", Value::string(record.title));
        if (record.progress >= 0)
            entry.set("progress", Value::number(record.progress));
        records.push(std::move(entry));
        outcome.text += std::string(programStateName(record.state)) + '\t' + (record.id.empty() ? "-" : record.id) + '\t' + std::string(store.effectiveApp(record)) + '\t' + record.message + '\n';
    }
    const Ubiquitty::OSC::ProgramStatus* primary = store.primary();
    outcome.result.set("session", Value::string(formatID(Identity::ObjectKind::Session, target->session)));
    outcome.result.set("state", Value::string(std::string(programStateName(primary ? primary->state : Ubiquitty::OSC::ProgramState::None))));
    outcome.result.set("records", std::move(records));
    if (outcome.text.empty())
        outcome.text = "none\n";
    session.detach();
    return outcome;
}

Outcome waitFor(const Value& params, const Context& context)
{
    const std::string condition = params.stringOr("for");
    const std::string needle = params.stringOr("text");
    if (condition == "text" && needle.empty()) {
        Outcome refused = failure(ExitCode::Usage, "'text' is required when waiting for text: --for text:<substring>.", "bad_request");
        refused.errorData.set("parameter", Value::string("/text"));
        return refused;
    }
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "wait", context);
    Outcome refused;
    const auto target = resolveTarget(params, control, context.invocation, refused);
    if (!target)
        return refused;
    control.detach();
    AgentSession session;
    if (!openAttached(session, context.invocation, target->session))
        return connectionFailure(session, "wait", context);
    // Events provide the returned cursor and state waits. Text waits below
    // additionally issue bounded authorized captures; pushed pictures alone
    // do not authorize a new read.
    const Value* since = params.find("since");
    const bool isSubscribed = since ? session.subscribeEvents(EventStart::AfterCursor, static_cast<std::uint64_t>(since->asNumber()))
                                    : session.subscribeEvents(EventStart::FromLatest);
    if (!isSubscribed)
        return failure(ExitCode::Unavailable, "wait: the host did not acknowledge the event subscription");
    if (condition == "text") {
        bool isFound = false;
        const auto captured = session.waitForText(needle, timeoutMilliseconds(params), isFound);
        std::uint64_t cursor = 0;
        for (const auto& event : session.takeEvents())
            cursor = std::max(cursor, event.cursor);
        session.detach();
        if (!captured || captured->status != Control::CaptureReplyStatus::Complete)
            return captureFailure("wait", captured, session, "the screen capture");
        if (!isFound)
            return failure(ExitCode::TimedOut, "wait: timed out", "timeout");
        Outcome outcome;
        outcome.result.set("matched", Value::boolean(true));
        outcome.result.set("for", Value::string(condition));
        outcome.result.set("cursor", Value::number(static_cast<double>(cursor)));
        outcome.text = "text\n";
        return outcome;
    }
    // The state is read after the subscription, on the same connection, so
    // the daemon has started streaming before it answers: a change after the
    // answer arrives as an event, and one before it is in the answer.
    bool isAtPrompt = false;
    if (condition == "idle") {
        std::vector<Ubiquitty::Session::SessionRow> sessions;
        if (session.listSessions(sessions)) {
            for (const auto& row : sessions)
                isAtPrompt = isAtPrompt || (row.sessionID == target->session && row.isAtPrompt());
        }
    }
    // A program's status reaches this client's screen with every frame, so it
    // is read again each time one arrives rather than only once.
    const auto isMetByState = [&] {
        if (condition == "idle")
            return isAtPrompt || session.foregroundIsShell();
        if (condition == "blocked")
            return std::ranges::any_of(session.programStatus().records, [](const auto& record) { return record.state == Ubiquitty::OSC::ProgramState::Blocked; });
        return false;
    };
    std::optional<Ubiquitty::Session::Event> matched;
    bool didSessionEnd = false;
    bool didLoseEvents = false;
    std::uint64_t cursor = 0;
    const bool isMet = session.pumpUntil(timeoutMilliseconds(params), [&] {
        for (auto& event : session.takeEvents()) {
            cursor = std::max(cursor, event.cursor);
            using Kind = Ubiquitty::Session::EventKind;
            // A gap with a count is the log saying it dropped events after the
            // cursor. A state can be read again; a command's end cannot.
            if (event.kind == Kind::Gap && event.count && condition == "exit")
                didLoseEvents = true;
            if (matched || event.sessionID != target->session)
                continue;
            const bool isMatch = (condition == "exit" && event.kind == Kind::CommandFinished)
                || (condition == "idle" && ((event.kind == Kind::ForegroundChanged && event.isAtPrompt) || (event.kind == Kind::ProgramStatus && event.state == Ubiquitty::Session::EventProgramState::Idle)))
                || (condition == "blocked" && event.kind == Kind::ProgramStatus && event.state == Ubiquitty::Session::EventProgramState::Blocked);
            if (isMatch)
                matched = std::move(event);
            else if (event.kind == Kind::SessionEnded)
                didSessionEnd = true;
        }
        return matched.has_value() || isMetByState() || didSessionEnd || didLoseEvents;
    });
    session.detach();
    if (didLoseEvents && !matched) {
        Outcome lost = failure(ExitCode::EventsLost, "wait: the daemon's event log no longer holds every event after that cursor, so the command may have finished unseen", "gap");
        lost.errorData.set("cursor", Value::number(static_cast<double>(cursor)));
        return lost;
    }
    if (didSessionEnd && !matched)
        return failure(ExitCode::Unavailable, "wait: the session ended first", "ended");
    if (!isMet)
        return failure(session.exited() ? ExitCode::Unavailable : ExitCode::TimedOut, session.exited() ? "wait: the daemon closed the connection" : "wait: timed out", "timeout");
    Outcome outcome;
    outcome.result.set("matched", Value::boolean(true));
    outcome.result.set("for", Value::string(condition));
    outcome.result.set("cursor", Value::number(static_cast<double>(matched ? matched->cursor : cursor)));
    if (matched) {
        outcome.result.set("event", Ubiquitty::Session::eventJSON(*matched));
        outcome.text = Ubiquitty::Codec::JSON::serialize(Ubiquitty::Session::eventJSON(*matched)) + '\n';
    } else {
        outcome.text = condition + '\n';
    }
    return outcome;
}

std::string eventLine(const Ubiquitty::Session::Event& event)
{
    std::string line = std::to_string(event.cursor) + '\t' + std::string(Ubiquitty::Session::eventKindName(event.kind)) + '\t' + (event.sessionID ? formatID(Identity::ObjectKind::Session, event.sessionID) : "-") + '\t'
        + (event.paneID ? formatID(Identity::ObjectKind::Pane, event.paneID) : "-");
    switch (event.kind) {
    case Ubiquitty::Session::EventKind::CommandFinished:
        line += '\t' + (event.exitStatus == Ubiquitty::Session::noExitStatus ? std::string("?") : std::to_string(event.exitStatus)) + '\t' + event.text;
        break;
    case Ubiquitty::Session::EventKind::ProgramStatus:
        line += '\t' + std::string(Ubiquitty::Session::eventProgramStateName(event.state)) + '\t' + event.text;
        break;
    case Ubiquitty::Session::EventKind::Gap:
        line += "\tdropped " + std::to_string(event.count);
        break;
    default:
        if (!event.text.empty())
            line += '\t' + event.text;
        break;
    }
    return line;
}

Outcome streamEvents(const Value& params, const Context& context)
{
    AgentSession control;
    if (!openControl(control, context.invocation))
        return connectionFailure(control, "events", context);
    std::optional<Target> target;
    if (params.find("session") || params.find("pane")) {
        Outcome refused;
        target = resolveTarget(params, control, context.invocation, refused);
        if (!target)
            return refused;
    }
    const Value* since = params.find("since");
    const bool isSubscribed = since ? control.subscribeEvents(EventStart::AfterCursor, static_cast<std::uint64_t>(since->asNumber()))
                                    : control.subscribeEvents(EventStart::FromLatest);
    if (!isSubscribed)
        return failure(ExitCode::Unavailable, "events: the host did not acknowledge the event subscription");
    const std::int64_t limit = integerParameter(params, "limit", 0);
    std::int64_t printed = 0;
    control.pumpUntil(timeoutMilliseconds(params), [&] {
        for (const auto& event : control.takeEvents()) {
            // The marker a from-latest stream opens with says where it starts
            // and reports nothing that happened.
            if (event.kind == Ubiquitty::Session::EventKind::Gap && !event.count)
                continue;
            if (target && event.kind != Ubiquitty::Session::EventKind::Gap && event.sessionID != target->session)
                continue;
            const std::string line = context.invocation.isJSON ? Ubiquitty::Codec::JSON::serialize(Ubiquitty::Session::eventJSON(event)) : eventLine(event);
            std::fprintf(stdout, "%s\n", line.c_str());
            std::fflush(stdout);
            if (limit && ++printed >= limit)
                return true;
        }
        return false;
    });
    const bool didClose = control.exited();
    control.detach();
    if (didClose && !(limit && printed >= limit))
        return failure(ExitCode::Unavailable, "events: the daemon closed the connection", "unavailable");
    Outcome outcome;
    // The lines are the output; nothing more is printed for a stream.
    outcome.result = Value::null();
    return outcome;
}

Value methodSummary(const Control::MethodDescriptor& method)
{
    Value entry = Value::object();
    entry.set("name", Value::string(std::string(method.name)));
    if (!method.commandLineVerb.empty())
        entry.set("verb", Value::string(std::string(method.commandLineVerb)));
    if (!method.controlMethod.empty())
        entry.set("controlMethod", Value::string(std::string(method.controlMethod)));
    if (!method.mcpTool.empty())
        entry.set("mcpTool", Value::string(std::string(method.mcpTool)));
    if (!method.syncMethod.empty())
        entry.set("syncMethod", Value::string(std::string(method.syncMethod)));
    entry.set("description", Value::string(std::string(method.description)));
    entry.set("params", Control::parseRegistryJSON(method.parametersSchema));
    entry.set("result", Control::parseRegistryJSON(method.resultSchema));
    return entry;
}

// The name an operation is reached by on the listed surface, or empty when
// that surface does not reach it. `all` lists every operation by its registry
// name.
std::string_view nameOnSurface(const Control::MethodDescriptor& method, std::string_view surface)
{
    if (surface == "control")
        return method.controlRoute == Control::ControlRoute::NotServed ? std::string_view() : method.controlMethod;
    if (surface == "mcp")
        return method.mcpTool;
    if (surface == "sync")
        return method.syncMethod;
    if (surface == "all")
        return method.name;
    return method.commandLineVerb;
}

Outcome listOperations(const Value& params, const Context&)
{
    const std::string surface = params.stringOr("surface", "command-line");
    Outcome outcome;
    Value methods = Value::array();
    for (const auto& method : Control::methodRegistry()) {
        const std::string_view name = nameOnSurface(method, surface);
        if (name.empty())
            continue;
        methods.push(methodSummary(method));
        outcome.text += std::string(name) + "\t" + std::string(method.description) + '\n';
    }
    outcome.result.set("methods", std::move(methods));
    return outcome;
}

Outcome describeOperation(const Value& params, const Context&)
{
    const std::string name = params.stringOr("method");
    const Control::MethodDescriptor* method = Control::findMethodFromSurface(name, Control::Surface::CommandLine);
    if (!method)
        return failure(ExitCode::Usage, "no operation is reached by '" + name + "'; `ubq api list` names them", "unknown_method");
    Outcome outcome;
    outcome.result = Control::describeMethod(*method);
    outcome.text = Ubiquitty::Codec::JSON::serialize(outcome.result) + '\n';
    return outcome;
}

Outcome callOperation(const Value& params, const Context& context);

// Checks a document against an operation's result schema with the validator
// the parameters go through, so a script can hold what --json printed to the
// shape `api describe` promised.
Outcome validateResult(const Value& params, const Context&)
{
    const std::string name = params.stringOr("method");
    const Control::MethodDescriptor* method = Control::findMethodFromSurface(name, Control::Surface::CommandLine);
    if (!method)
        return failure(ExitCode::Usage, "no operation is reached by '" + name + "'; `ubq api list` names them", "unknown_method");
    const Value* document = params.find("document");
    if (const auto violation = Control::validate(document ? *document : Value::object(), Control::parseRegistryJSON(method->resultSchema))) {
        Outcome refused = failure(ExitCode::Usage, violation->message, "invalid");
        refused.errorData.set("path", Value::string(violation->path));
        return refused;
    }
    Outcome outcome;
    outcome.result.set("valid", Value::boolean(true));
    outcome.text = "valid\n";
    return outcome;
}

Outcome serveMCP(const Value&, const Context& context)
{
    Outcome outcome;
    // The MCP server reads its own policy reference; the loop owns stdout.
    AgentPolicy policy = context.policy;
    outcome.processStatus = Ubiquitty::Agent::runMCP(policy, context.invocation.host);
    outcome.result = Value::null();
    return outcome;
}

Outcome printCompletions(const Value& params, const Context&)
{
    const std::string shell = params.stringOr("shell");
    Outcome outcome;
    outcome.text = shell == "zsh" ? Ubiquitty::Agent::zshCompletion : Ubiquitty::Agent::bashCompletion;
    outcome.result.set("script", Value::string(outcome.text));
    return outcome;
}

// The end-to-end health check: attach a fresh session, run a command, and
// read its output back off the mirrored screen.
// A self-test makes a session and types into it, as `new --command` does, so
// the policy screens both before anything is made: the size, and what it
// types, which ubq writes and a rule names in angle brackets.
bool isSelfTestAllowed(const char* test, const Context& context, Outcome& refused)
{
    return isAllowedByPolicy(context.policy, 0, "new", "80x24", context.invocation, refused, AgentPolicy::Subject::Description)
        && isAllowedByPolicy(context.policy, 0, "run", "<" + std::string(test) + ">", context.invocation, refused, AgentPolicy::Subject::Description);
}

Outcome selfTest(const Value&, const Context& context)
{
    Outcome refused;
    if (!isSelfTestAllowed("selftest", context, refused))
        return refused;
    AgentSession session;
    if (!openAttached(session, context.invocation, 0))
        return connectionFailure(session, "selftest", context);
    // Assembled by command substitution, so the marker appears only in the
    // command's output and never in the echoed command line.
    constexpr std::string_view marker = "agent_OK_7F3A";
    if (std::optional<Outcome> unsent = inputFailure(session.sendInput("echo \"ag$(echo ent)_OK_7F3A\"\n"), session, "selftest", "the command"))
        return std::move(*unsent);
    const bool isFound = session.pumpUntil(4000, [&] { return session.screenText().find(marker) != std::string::npos; });
    std::fprintf(stderr, "screen %d columns %d rows  updates=%d  marker=%s\n", session.columns(), session.rows(), session.updatesApplied(), isFound ? "FOUND" : "MISSING");
    session.detach();
    if (!isFound)
        return failure(ExitCode::Unavailable, "selftest: the marker did not reach the screen");
    Outcome outcome;
    outcome.result.set("passed", Value::boolean(true));
    outcome.text = "SELFTEST PASS\n";
    return outcome;
}

// Draws a one-pixel red image over an 8 x 4 cell block in a fresh session and
// checks that the pixels reach this client's screenshot: the IMAGE and
// IMAGE_PLACEMENTS frames carry what the client has no parser to make.
Outcome imageTest(const Value&, const Context& context)
{
    Outcome refused;
    if (!isSelfTestAllowed("imagetest", context, refused))
        return refused;
    AgentSession session;
    if (!openAttached(session, context.invocation, 0))
        return connectionFailure(session, "imagetest", context);
    session.pump(300);
    // /wAA is base64 of FF 00 00: red.
    if (std::optional<Outcome> unsent = inputFailure(session.sendInput("printf '\\033_Ga=T,t=d,f=24,s=1,v=1,c=8,r=4,i=4242;/wAA\\033\\\\'\n"), session, "imagetest", "the command"))
        return std::move(*unsent);
    if (!session.pumpUntil(6000, [&] { return session.imagePlacementCount() > 0; })) {
        session.detach();
        return failure(ExitCode::Unavailable, "imagetest: no image placement reached the client");
    }
    std::vector<std::uint8_t> png = session.renderPNG();
    session.detach();
    std::vector<std::uint8_t> scratch(64u * 1024u * 1024u, 0);
    auto inflater = std::make_unique<Ubiquitty::Codec::Zlib::Inflater>();
    const auto decoded = Ubiquitty::Codec::PNG::decode({ png.data(), png.size() }, { scratch.data(), scratch.size() }, *inflater);
    if (decoded.status != Ubiquitty::Codec::PNG::DecodeStatus::Success)
        return failure(ExitCode::Unavailable, "imagetest: the screenshot did not decode");
    long redPixels = 0;
    const int bytesPerPixel = decoded.bytesPerPixel;
    for (int y = 0; y < decoded.height; ++y) {
        for (int x = 0; x < decoded.width; ++x) {
            const std::uint8_t* pixel = decoded.pixels.data() + (static_cast<std::size_t>(y) * decoded.width + x) * bytesPerPixel;
            if (pixel[0] > 200 && pixel[1] < 80 && pixel[2] < 80)
                ++redPixels;
        }
    }
    std::fprintf(stderr, "imagetest: redPixels=%ld\n", redPixels);
    if (redPixels < 1000)
        return failure(ExitCode::Unavailable, "imagetest: the image's pixels are missing from the screenshot");
    Outcome outcome;
    outcome.result.set("passed", Value::boolean(true));
    outcome.text = "IMAGETEST PASS\n";
    return outcome;
}

// ---- dispatch --------------------------------------------------------------

using Handler = Outcome (*)(const Value&, const Context&);

struct Operation {
    std::string_view name;
    Handler handler;
};

// The operations this command line runs, by registry name.
constexpr Operation operations[] {
    { "ubq.ls", &listSessions },
    { "ubq.new", &newSession },
    { "ubq.run", &runCommand },
    { "ubq.send", &sendText },
    { "ubq.read", &readText },
    { "pane.search", &searchHistory },
    { "ubq.split", &splitPane },
    { "ubq.kill", &killSession },
    { "ubq.label", &labelObject },
    { "ubq.tag", &tagObject },
    { "ubq.tree", &printTree },
    { "ubq.status", &programStatus },
    { "ubq.wait", &waitFor },
    { "ubq.events", &streamEvents },
    { "ubq.api.list", &listOperations },
    { "ubq.api.describe", &describeOperation },
    { "ubq.api.call", &callOperation },
    { "ubq.api.validate", &validateResult },
    { "agent.mcp", &serveMCP },
    { "agent.completions", &printCompletions },
    { "agent.selftest", &selfTest },
    { "agent.imagetest", &imageTest },
};

Handler handlerFor(std::string_view name)
{
    for (const Operation& operation : operations) {
        if (operation.name == name)
            return operation.handler;
    }
    return nullptr;
}

// Validates parameters against the operation's schema and runs it. A
// refusal names the parameter and says what the schema requires, before any
// connection is made.
// The scopes an invocation needs: the verb's own, what an option it was given
// asks for besides, and for a tree call the control connection's and the
// call's.
Sync::ScopeSet neededScopes(const Control::MethodDescriptor& method, const Value& params)
{
    Sync::ScopeSet scopes = method.commandLineScopes;
    if (method.name == "ubq.new" && (params.find("command") || params.find("label")))
        scopes.add(Sync::Scope::Drive);
    if (method.name == "ubq.wait" && params.stringOr("for") == "text")
        scopes.add(Sync::Scope::RegionRead);
    if (method.commandLineVerb.empty() && Ubiquitty::Protocol::Sync::isStructureMethod(method.syncMethod)) {
        scopes.add(Sync::Scope::Control);
        scopes.add(method.scope);
    }
    return scopes;
}

Outcome invoke(const Control::MethodDescriptor& method, const Value& params, const Context& invoked)
{
    const Context context { invoked.invocation, invoked.policy, neededScopes(method, params) };
    const Handler handler = handlerFor(method.name);
    const bool isTreeCall = Ubiquitty::Protocol::Sync::isStructureMethod(method.syncMethod);
    if (!handler && !isTreeCall)
        return failure(ExitCode::Usage, "'" + std::string(method.name) + "' is not run by ubq; it is served by " + (method.controlMethod.empty() ? "the MCP server" : "the app's control socket"), "unsupported");
    // A call addressed by a pane handle is given that handle once ubq has
    // attached; its other parameters are refused before any connection.
    Value checked = params;
    if (handler && method.target == Control::CallTarget::Pane && !method.syncMethod.empty() && !checked.find("pane"))
        checked.set("pane", Value::number(1));
    if (const auto violation = Control::validate(checked, Control::parseRegistryJSON(method.parametersSchema))) {
        Outcome refused = failure(ExitCode::Usage, violation->message, "bad_request");
        refused.errorData.set("parameter", Value::string(violation->path));
        refused.errorData.set("method", Value::string(std::string(method.name)));
        return refused;
    }
    if (!handler) {
        // Only exact-operation tree methods are host-call doors here. Keep
        // ordinary CLI policy identities so api call cannot bypass a deny.
        const std::string kind = method.syncMethod == "label.set" ? "label" : method.syncMethod == "tags.set"                                ? "tag"
            : method.syncMethod == "workspace.split"                                                                                         ? "split"
            : method.syncMethod == "workspace.addTab"                                                                                        ? "new"
            : method.syncMethod == "window.close" || method.syncMethod == "workspace.closeTab" || method.syncMethod == "workspace.closePane" ? "kill"
                                                                                                                                             : std::string(method.syncMethod);
        const std::string description = kind == "split" ? (params.stringOr("orientation") == "horizontal" ? "<split down>" : "<split right>")
            : kind == "kill"                            ? "<terminate session>"
                                                        : "<" + kind + ">";
        Outcome refused;
        if (!isAllowedByPolicy(context.policy, 0, kind.c_str(), description, context.invocation, refused, AgentPolicy::Subject::Description))
            return refused;
        AgentSession control;
        if (!openControl(control, context.invocation))
            return connectionFailure(control, kind.c_str(), context);
        const auto result = callHost(control, kind.c_str(), method.syncMethod, params, context, refused);
        control.detach();
        if (!result)
            return refused;
        Outcome outcome;
        outcome.result = *result;
        outcome.text = Ubiquitty::Codec::JSON::serialize(*result) + '\n';
        return outcome;
    }
    return handler(params, context);
}

// Reads a JSON argument given inline, as `@file`, or as `-` for standard
// input; nothing, and the reason in `error`, when it cannot be read or does
// not parse.
std::optional<Value> readJSONArgument(const std::string& argument, std::string& error)
{
    std::string text;
    if (argument == "-" || argument.starts_with('@')) {
        std::FILE* file = argument == "-" ? stdin : std::fopen(argument.c_str() + 1, "r");
        if (!file) {
            error = "cannot read " + argument.substr(1);
            return std::nullopt;
        }
        std::array<char, 8192> buffer {};
        std::size_t got;
        // Parameters are a request, not a payload; a megabyte is far past any.
        while ((got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0 && text.size() < 1024 * 1024)
            text.append(buffer.data(), got);
        if (file != stdin)
            std::fclose(file);
    } else {
        text = argument;
    }
    bool isParsed = false;
    Value value = Ubiquitty::Codec::JSON::parse(text, isParsed);
    if (!isParsed) {
        const std::string source = argument == "-" ? std::string("standard input") : argument.starts_with('@') ? argument.substr(1)
                                                                                                               : std::string("the JSON argument");
        error = source + " does not parse as JSON";
        return std::nullopt;
    }
    return value;
}

Outcome callOperation(const Value& params, const Context& context)
{
    const std::string name = params.stringOr("method");
    const Control::MethodDescriptor* method = Control::findMethodFromSurface(name, Control::Surface::CommandLine);
    if (!method)
        return failure(ExitCode::Usage, "no operation is reached by '" + name + "'; `ubq api list` names them", "unknown_method");
    if (method->name == "ubq.api.call")
        return failure(ExitCode::Usage, "api call does not call itself", "bad_request");
    const Value* inner = params.find("params");
    Outcome outcome = invoke(*method, inner ? *inner : Value::object(), context);
    outcome.isAlwaysJSON = true;
    return outcome;
}

// ---- argument parsing ------------------------------------------------------

struct ParsedCommandLine {
    const Control::MethodDescriptor* method = nullptr;
    std::string verb;
    Value params = Value::object();
    Invocation invocation;
    std::string error;
    /// `--help` or `-h` was given: before any verb, for the command line;
    /// after one, for that verb.
    bool isHelpRequested = false;
};

// How each verb's words are written, after `ubq` and its global options.
struct Synopsis {
    std::string_view verb;
    std::string_view words;
};
constexpr Synopsis synopses[] {
    { "ls", "ls" },
    { "new", "new [--label L] [--columns N] [--rows N] [-- command]" },
    { "run", "run [-s S | -p P] [--timeout SECONDS] -- command" },
    { "send", "send [-s S | -p P] [--enter] [--] text" },
    { "read", "read [-s S | -p P] [--screen | --last-command | --scrollback [N]]" },
    { "search", "search [-s S | -p P] [--from L] [--to L] [--case-sensitive] [--] pattern" },
    { "split", "split [-s S | -p P] [right | down]" },
    { "kill", "kill [-s S | -p P]" },
    { "label", "label [-w W | -W W | -t T | -p P | -s S] [--] label" },
    { "tag", "tag [-w W | -W W | -t T | -p P | -s S] [--] [tags...]" },
    { "tree", "tree [--] [words...]" },
    { "status", "status [-s S | -p P]" },
    { "wait", "wait [-s S | -p P] --for exit|idle|blocked|text:<s> [--since C] [--timeout SECONDS]" },
    { "events", "events [-s S | -p P] [--since C] [--limit N] [--timeout SECONDS]" },
    { "api list", "api list [--surface command-line|control|mcp|sync|all]" },
    { "api describe", "api describe <name>" },
    { "api call", "api call <name> [json | @file | -]" },
    { "api validate", "api validate <name> <json | @file | ->" },
    { "mcp", "mcp" },
    { "completions", "completions bash|zsh" },
    { "selftest", "selftest" },
    { "imagetest", "imagetest" },
};

std::string_view synopsisOf(std::string_view verb)
{
    for (const Synopsis& synopsis : synopses) {
        if (synopsis.verb == verb)
            return synopsis.words;
    }
    return verb;
}

// The options that name the object a verb acts on, by the parameter each sets
// and the placeholder its value is written as.
struct TargetOption {
    std::string_view shortFlag;
    std::string_view longFlag;
    std::string_view parameter;
    std::string_view placeholder;
    std::string_view value;
};
constexpr TargetOption targetOptions[] {
    { "-s", "--session", "session", "S", "a session ID, a session label, or here" },
    { "-p", "--pane", "pane", "P", "a pane ID, a pane label, or here" },
    { "-w", "--workspace", "workspace", "W", "a workspace ID, a workspace label, or here" },
    { "-W", "--window", "window", "W", "a window ID, a window label, or here" },
    { "-t", "--tab", "tab", "T", "a tab ID, a tab label, or here" },
};

const TargetOption* targetOptionFor(std::string_view argument)
{
    for (const TargetOption& option : targetOptions) {
        if (argument == option.shortFlag || argument == option.longFlag)
            return &option;
    }
    return nullptr;
}

// Reads `--name value` into a parameter typed by the schema, so a number the
// schema wants arrives as a number and anything else reaches the validator as
// written and is refused there with the registry's words.
void setFlag(Value& params, const std::string& type, const std::string& name, const char* nextArgument, int& index)
{
    if (type == "boolean" || !nextArgument) {
        params.set(name, Value::boolean(true));
        return;
    }
    ++index;
    const std::string value = nextArgument;
    if (type == "integer" || type == "number") {
        bool isParsed = false;
        Value number = Ubiquitty::Codec::JSON::parse(value, isParsed);
        if (isParsed && number.isNumber()) {
            params.set(name, std::move(number));
            return;
        }
    }
    params.set(name, Value::string(value));
}

std::string camelCase(std::string_view flag)
{
    std::string name;
    bool isUpper = false;
    for (const char character : flag) {
        if (character == '-') {
            isUpper = true;
            continue;
        }
        name.push_back(isUpper && character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A') : character);
        isUpper = false;
    }
    return name;
}

// The flag a parameter is given by: `caseSensitive` is `--case-sensitive`.
std::string kebabCase(std::string_view parameter)
{
    std::string flag = "--";
    for (const char character : parameter) {
        if (character >= 'A' && character <= 'Z') {
            flag.push_back('-');
            flag.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            flag.push_back(character);
        }
    }
    return flag;
}

std::string joined(const std::vector<std::string>& words)
{
    std::string text;
    for (const auto& word : words) {
        if (!text.empty())
            text.push_back(' ');
        text += word;
    }
    return text;
}

// Whether a word reads as a negative number, which is a value and not an
// option: `--limit -1` is refused by the schema, not taken for a flag.
bool isNegativeNumber(std::string_view word)
{
    return word.size() > 1 && word[0] == '-' && std::isdigit(static_cast<unsigned char>(word[1]));
}

// The verbs whose words, after their options, are the parameter they set.
// Every other verb takes none, and a word given to it is refused.
bool takesWords(std::string_view verb)
{
    constexpr std::array<std::string_view, 12> takers { "run", "new", "send", "search", "label", "tag", "tree", "split", "completions", "api describe", "api call", "api validate" };
    return std::ranges::find(takers, verb) != takers.end();
}

ParsedCommandLine parseCommandLine(int argc, char** argv)
{
    ParsedCommandLine parsed;
    int index = 1;
    // Global options may come before the verb.
    std::vector<std::string> words;
    for (; index < argc; ++index)
        words.emplace_back(argv[index]);
    std::vector<std::string> remaining;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::string& word = words[i];
        if (word == "--") {
            remaining.insert(remaining.end(), words.begin() + static_cast<long>(i), words.end());
            break;
        }
        if ((word == "--help" || word == "-h") && remaining.empty()) {
            parsed.isHelpRequested = true;
            return parsed;
        }
        if (word == "--json") {
            parsed.invocation.isJSON = true;
        } else if (word == "--yes") {
            // A confirmation answers for a person, so --yes counts only from a
            // person's terminal, and never from a process that names itself an
            // agent. An agent's own flag would make its approval its own.
            const char* agentLabel = std::getenv("UBIQUITTY_AGENT_LABEL");
            const bool isPersonAtTerminal = Ubiquitty::Agent::isStandardInputTerminal() && !(agentLabel && *agentLabel);
            parsed.invocation.isApproved = isPersonAtTerminal;
            parsed.invocation.isApprovalIgnored = !isPersonAtTerminal;
        } else if (word == "-H" || word == "--host") {
            if (i + 1 >= words.size() || words[i + 1] == "--") {
                parsed.error = word + " needs a host: " + word + " [user@]host";
                return parsed;
            }
            parsed.invocation.host = words[++i];
        } else {
            remaining.push_back(word);
        }
    }
    if (remaining.empty() || remaining.front() == "--") {
        parsed.error = "name a verb; `ubq --help` lists them";
        return parsed;
    }
    parsed.verb = remaining.front();
    std::size_t first = 1;
    if (parsed.verb == "api") {
        const bool isNamed = remaining.size() > 1 && remaining[1] != "--";
        if (!isNamed || !Control::findCommandLineVerb("api " + remaining[1])) {
            parsed.error = "api takes list, describe, call or validate" + (isNamed ? ", not '" + remaining[1] + "'" : std::string()) + "; `ubq --help` lists them";
            return parsed;
        }
        parsed.verb = "api " + remaining[1];
        first = 2;
    }
    parsed.method = Control::findCommandLineVerb(parsed.verb);
    if (!parsed.method) {
        parsed.error = "'" + parsed.verb + "' is not a verb; `ubq --help` lists them";
        return parsed;
    }
    // A refused word is told where the verb's own words are listed.
    const std::string listed = "; `ubq " + parsed.verb + " --help` lists them";
    // A pane call's own `pane` is a handle on ubq's connection; -s and -p name
    // the pane ubq attaches to for it.
    const bool isPaneCall = parsed.method->target == Control::CallTarget::Pane && !parsed.method->syncMethod.empty();
    Value& targetParameters = isPaneCall ? parsed.invocation.paneTarget : parsed.params;
    const Value schema = Control::parseRegistryJSON(parsed.method->parametersSchema);
    const Value* properties = schema.find("properties");
    std::vector<std::string> positionals;
    std::vector<std::string> afterSeparator;
    std::vector<char*> arguments;
    for (std::size_t i = first; i < remaining.size(); ++i)
        arguments.push_back(remaining[i].data());
    const int count = static_cast<int>(arguments.size());
    for (int i = 0; i < count; ++i) {
        const std::string argument = arguments[static_cast<std::size_t>(i)];
        const char* next = i + 1 < count ? arguments[static_cast<std::size_t>(i) + 1] : nullptr;
        const bool hasNext = next && std::string_view(next) != "--";
        if (argument == "--") {
            for (int j = i + 1; j < count; ++j)
                afterSeparator.emplace_back(arguments[static_cast<std::size_t>(j)]);
            break;
        }
        if (argument == "--help" || argument == "-h") {
            parsed.isHelpRequested = true;
            return parsed;
        }
        if (const TargetOption* target = targetOptionFor(argument)) {
            const bool isTargetOfPaneCall = isPaneCall && (target->parameter == "session" || target->parameter == "pane");
            if (!isTargetOfPaneCall && !(properties && properties->find(std::string(target->parameter)))) {
                parsed.error = "'" + argument + "' is not an option of " + parsed.verb + listed;
                return parsed;
            }
            if (!hasNext) {
                parsed.error = argument + " needs " + std::string(target->value) + listed;
                return parsed;
            }
            (isTargetOfPaneCall ? targetParameters : parsed.params).set(std::string(target->parameter), Value::string(next));
            ++i;
        } else if (parsed.verb == "read" && (argument == "--screen" || argument == "--last-command")) {
            parsed.params.set("source", Value::string(argument.substr(2)));
        } else if (parsed.verb == "read" && argument == "--scrollback") {
            parsed.params.set("source", Value::string("scrollback"));
            // A number, negative included, is the count; the schema says
            // what a count may be.
            if (next && (std::isdigit(static_cast<unsigned char>(next[0])) || isNegativeNumber(next))) {
                int consumed = i;
                setFlag(parsed.params, "integer", "lines", next, consumed);
                i = consumed;
            }
        } else if (parsed.verb == "wait" && argument == "--for") {
            if (!hasNext) {
                parsed.error = "--for needs exit, idle, blocked or text:<s>" + listed;
                return parsed;
            }
            const std::string condition = next;
            ++i;
            if (condition.starts_with("text:")) {
                parsed.params.set("for", Value::string("text"));
                parsed.params.set("text", Value::string(condition.substr(5)));
            } else {
                parsed.params.set("for", Value::string(condition));
            }
        } else if (parsed.verb == "split" && (argument == "--right" || argument == "--down")) {
            parsed.params.set("direction", Value::string(argument.substr(2)));
        } else if (argument.starts_with("--") && argument.size() > 2) {
            const std::string name = camelCase(std::string_view(argument).substr(2));
            const Value* property = properties ? properties->find(name) : nullptr;
            if (!property) {
                parsed.error = "'" + argument + "' is not an option of " + parsed.verb + listed;
                return parsed;
            }
            const std::string type = property->stringOr("type");
            const bool hasValue = hasNext && !std::string_view(next).starts_with("--");
            if (type != "boolean" && !hasValue) {
                parsed.error = argument + " needs a value" + listed;
                return parsed;
            }
            setFlag(parsed.params, type, name, hasValue ? next : nullptr, i);
        } else if (argument.size() > 1 && argument.front() == '-' && !isNegativeNumber(argument)) {
            // An option this verb does not take is refused rather than read
            // as a word: `ubq new -x` would otherwise type `-x` into a shell.
            parsed.error = "'" + argument + "' is not an option of " + parsed.verb + listed + ", and a word that begins with '-' goes after --";
            return parsed;
        } else {
            positionals.push_back(argument);
        }
    }
    // Words after `--` are words like those before it, and never options.
    std::vector<std::string> given = positionals;
    given.insert(given.end(), afterSeparator.begin(), afterSeparator.end());
    if (!given.empty() && !takesWords(parsed.verb)) {
        parsed.error = "'" + given.front() + "' is not an argument " + parsed.verb + " takes" + listed;
        return parsed;
    }
    // Each verb's words, named by what the schema calls them.
    if (parsed.verb == "run" || parsed.verb == "new") {
        if (!given.empty())
            parsed.params.set("command", Value::string(joined(given)));
    } else if (parsed.verb == "send") {
        if (!given.empty())
            parsed.params.set("text", Value::string(joined(given)));
    } else if (parsed.verb == "search") {
        if (!given.empty())
            parsed.params.set("pattern", Value::string(joined(given)));
    } else if (parsed.verb == "label") {
        if (!given.empty())
            parsed.params.set("label", Value::string(joined(given)));
    } else if (parsed.verb == "tag") {
        // Each word is a tag; none clears them.
        Value tags = Value::array();
        for (const std::string& tag : given)
            tags.push(Value::string(tag));
        parsed.params.set("tags", std::move(tags));
    } else if (parsed.verb == "tree") {
        if (!given.empty())
            parsed.params.set("query", Value::string(joined(given)));
    } else if (parsed.verb == "split" || parsed.verb == "completions") {
        if (given.size() > 1) {
            parsed.error = "'" + given[1] + "' is one argument more than " + parsed.verb + " takes" + listed;
            return parsed;
        }
        if (!given.empty())
            parsed.params.set(parsed.verb == "split" ? "direction" : "shell", Value::string(given.front()));
    } else if (parsed.verb == "api describe" || parsed.verb == "api call" || parsed.verb == "api validate") {
        // An operation is named by any name it is reached by, and a verb of
        // two words, such as `api list`, by both of them.
        std::size_t named = 0;
        if (!given.empty()) {
            std::string name = given.front();
            named = 1;
            if (given.size() > 1 && Control::findCommandLineVerb(name + ' ' + given[1])) {
                name += ' ' + given[1];
                named = 2;
            }
            parsed.params.set("method", Value::string(std::move(name)));
        }
        const std::size_t taken = named + (parsed.verb == "api describe" || !named ? 0 : 1);
        if (given.size() > taken) {
            parsed.error = "'" + given[taken] + "' is one argument more than " + parsed.verb + " takes" + listed;
            return parsed;
        }
        if (parsed.verb == "api validate" && named && given.size() == named) {
            parsed.error = "api validate needs a document after the name: JSON, @file, or - for standard input";
            return parsed;
        }
        if (parsed.verb != "api describe" && named && given.size() > named) {
            std::string error;
            std::optional<Value> inner = readJSONArgument(given[named], error);
            if (!inner) {
                parsed.error = parsed.verb + ": " + error;
                return parsed;
            }
            parsed.params.set(parsed.verb == "api call" ? "params" : "document", std::move(*inner));
        }
    }
    return parsed;
}

void printUsage()
{
    std::fprintf(stderr, "usage: ubq [-H [user@]host] [--json] [--yes] <verb> [options] [-- words]\n");
    for (const Synopsis& synopsis : synopses)
        std::fprintf(stderr, "  ubq %.*s\n", static_cast<int>(synopsis.words.size()), synopsis.words.data());
    std::fprintf(stderr, "`ubq --help` describes every verb, and `ubq <verb> --help` one of them.\n");
}

// The answer to `ubq <verb> --help`, on standard output: how the verb is
// written, the registry's sentence for it, and each parameter as the flag it
// is given by, with the schema's words for it.
void printVerbHelp(const Control::MethodDescriptor& method)
{
    const std::string verb(method.commandLineVerb);
    const std::string_view words = synopsisOf(verb);
    std::printf("usage: ubq [-H [user@]host] [--json] [--yes] %.*s\n\n%.*s\n", static_cast<int>(words.size()), words.data(), static_cast<int>(method.description.size()), method.description.data());
    const Value schema = Control::parseRegistryJSON(method.parametersSchema);
    const Value* properties = schema.find("properties");
    const bool isPaneCall = method.target == Control::CallTarget::Pane && !method.syncMethod.empty();
    std::vector<std::pair<std::string, std::string>> options;
    // A pane call is aimed by -s or -p, which its schema does not name: its
    // `pane` is the handle ubq supplies.
    if (isPaneCall) {
        for (const TargetOption& target : { targetOptions[0], targetOptions[1] }) {
            std::string sentence(target.value);
            sentence.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(sentence.front())));
            options.emplace_back(std::string(target.shortFlag) + ", " + std::string(target.longFlag) + ' ' + std::string(target.placeholder), sentence + '.');
        }
    }
    if (properties) {
        for (const auto& [parameter, property] : properties->members()) {
            if (isPaneCall && parameter == "pane")
                continue;
            std::string flag;
            for (const TargetOption& target : targetOptions) {
                if (target.parameter == parameter)
                    flag = std::string(target.shortFlag) + ", " + std::string(target.longFlag) + ' ' + std::string(target.placeholder);
            }
            const std::string type = property.stringOr("type");
            // A list is the verb's words, not a flag: `tag` takes its tags so.
            if (flag.empty() && type == "array")
                flag = parameter + "...";
            if (flag.empty()) {
                flag = kebabCase(parameter);
                if (const Value* choices = property.find("enum"); choices && choices->isArray()) {
                    std::string listedChoices;
                    for (const Value& choice : choices->items())
                        listedChoices += (listedChoices.empty() ? "" : "|") + choice.asString();
                    flag += ' ' + listedChoices;
                } else if (type == "integer" || type == "number") {
                    flag += " N";
                } else if (type != "boolean") {
                    flag += " TEXT";
                }
            }
            std::string sentence = property.stringOr("description");
            if (const Value* fallback = property.find("default"))
                sentence += (sentence.empty() ? "" : " ") + std::string("Default ") + Ubiquitty::Codec::JSON::serialize(*fallback) + '.';
            options.emplace_back(std::move(flag), std::move(sentence));
        }
    }
    if (!options.empty()) {
        std::printf("\noptions:\n");
        std::size_t width = 0;
        for (const auto& option : options)
            width = std::max(width, option.first.size());
        for (const auto& [flag, sentence] : options)
            std::printf("  %-*s  %s\n", static_cast<int>(width), flag.c_str(), sentence.c_str());
    }
    if (!method.commandLineScopes.isEmpty()) {
        const std::string list = Sync::scopeMaskToString(method.commandLineScopes);
        std::printf("\nscopes: %s, which a token issued with\n`ubiquitty-host token issue --scope %s` carries.\n", scopeWords(method.commandLineScopes).c_str(), list.c_str());
        // An option that needs a scope more says so in its sentence.
        if (method.parametersSchema.find(" scope") != std::string_view::npos)
            std::printf("An option that needs another says so above.\n");
    }
    std::printf("\n`ubq api describe %s` gives its schemas and an example.\n", verb.c_str());
}

// The answer to `--help`, on standard output: the usage line, then every verb
// the method registry gives the command line, with the registry's own words.
void printHelp()
{
    std::printf("usage: ubq [-H [user@]host] [--json] [--yes] <verb> [options] [-- words]\n\nverbs:\n");
    std::size_t width = 0;
    for (const auto& method : Control::methodRegistry())
        width = std::max(width, method.commandLineVerb.size());
    for (const auto& method : Control::methodRegistry()) {
        if (method.commandLineVerb.empty())
            continue;
        std::printf("  %-*.*s  %.*s\n", static_cast<int>(width), static_cast<int>(method.commandLineVerb.size()), method.commandLineVerb.data(), static_cast<int>(method.description.size()),
            method.description.data());
    }
    std::printf("\nglobal options, anywhere before --:\n"
                "  -H, --host [user@]host  Reach that host's daemon over ssh, presenting UBQ_REMOTE_TOKEN.\n"
                "  --json                  Print the result, or a failure, as JSON on standard output.\n"
                "  --yes                   Answer a policy `confirm`; counts only when a person types it at a terminal.\n"
                "  -h, --help              This text; after a verb, that verb's usage and options.\n"
                "\nenvironment:\n"
                "  UBIQUITTY_AGENT_TOKEN   The token for this machine's daemon; else the agent-token file in UBIQUITTY_HOME.\n"
                "  UBQ_REMOTE_TOKEN        The token for -H, printed by that host's own `ubiquitty-host token issue`.\n"
                "  UBIQUITTY_HOME          The control directory: socket, token, policy and audit log. Default ~/.ubiquitty.\n"
                "  UBIQUITTY_HOST_BIN      The ubiquitty-host binary ubq starts when no daemon answers.\n"
                "  UBIQUITTY_AGENT_POLICY  The policy file; UBIQUITTY_AGENT_AUDIT names the audit log.\n"
                "\n`ubq <verb> --help` gives a verb's options, and `ubq api describe <verb>` its\nschemas. The reference is https://ubiquitty.org/docs/ubq/\n");
}

int report(const Outcome& outcome, const Invocation& invocation, const std::string& verb)
{
    if (outcome.code != ExitCode::Success) {
        if (invocation.isJSON) {
            Value error = Value::object();
            error.set("code", Value::number(outcome.code == ExitCode::Usage ? invalidParametersCode : failedCode));
            error.set("message", Value::string(outcome.error));
            error.set("data", outcome.errorData);
            Value document = Value::object();
            document.set("error", std::move(error));
            std::fprintf(stdout, "%s\n", Ubiquitty::Codec::JSON::serialize(document).c_str());
        } else {
            // A message that names its verb already, as a connection failure
            // does for the JSON error's sake, is not given the verb twice.
            const std::string prefix = verb + ": ";
            const std::string_view message = std::string_view(outcome.error).starts_with(prefix) ? std::string_view(outcome.error).substr(prefix.size()) : std::string_view(outcome.error);
            std::fprintf(stderr, "ubq: %s: %.*s\n", verb.c_str(), static_cast<int>(message.size()), message.data());
            // The operation is named the way `api describe` reaches it: by its
            // verb, or by its registry name when the command line has none.
            const Control::MethodDescriptor* method = outcome.errorData.find("parameter") ? Control::findMethod(outcome.errorData.stringOr("method")) : nullptr;
            if (method) {
                const std::string reached(method->commandLineVerb.empty() ? method->name : method->commandLineVerb);
                std::fprintf(stderr, "  %s — %s\n  `ubq api describe %s` gives its parameters.\n", reached.c_str(), std::string(method->description).c_str(), reached.c_str());
            }
        }
        return exitStatus(outcome.code);
    }
    if (!outcome.result.isNull()) {
        if (invocation.isJSON || outcome.isAlwaysJSON)
            std::fprintf(stdout, "%s\n", Ubiquitty::Codec::JSON::serialize(outcome.result).c_str());
        else
            std::fwrite(outcome.text.data(), 1, outcome.text.size(), stdout);
    }
    return outcome.processStatus.value_or(exitStatus(ExitCode::Success));
}

} // namespace

int main(int argc, char** argv)
{
    // A write to a daemon socket whose peer closed must fail with EPIPE
    // rather than end this process.
    ::signal(SIGPIPE, SIG_IGN);
    ParsedCommandLine parsed = parseCommandLine(argc, argv);
    if (parsed.isHelpRequested) {
        if (parsed.method)
            printVerbHelp(*parsed.method);
        else
            printHelp();
        return exitStatus(ExitCode::Success);
    }
    if (!parsed.error.empty()) {
        if (parsed.invocation.isJSON) {
            Outcome refused = failure(ExitCode::Usage, parsed.error, "bad_request");
            return report(refused, parsed.invocation, parsed.verb);
        }
        std::fprintf(stderr, "ubq: %s\n", parsed.error.c_str());
        // A refusal of a verb's own words names that verb's --help; the whole
        // usage answers a line that named no verb ubq has.
        if (!parsed.method)
            printUsage();
        return exitStatus(ExitCode::Usage);
    }
    if (!parsed.invocation.host.empty() && !Ubiquitty::Transport::validateSSHHost(parsed.invocation.host)) {
        std::fprintf(stderr, "ubq: '%s' is not a host ssh can be given\n", parsed.invocation.host.c_str());
        return exitStatus(ExitCode::Usage);
    }
    AgentPolicy policy;
    policy.loadDefault();
    const Context context { parsed.invocation, policy };
    return report(invoke(*parsed.method, parsed.params, context), parsed.invocation, parsed.verb);
}
