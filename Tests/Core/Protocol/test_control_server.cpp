// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_control_server — the control socket's method surface over a fake
// frontend: what reaches the host, with which ids, and what the agent policy
// stops before it does.

#include "Harness.h"

#include "Core/Codec/JSON.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Control/Server.h"
#include "Core/Security/AgentPolicy.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using Ubiquitty::Codec::JSON::Value;
using Ubiquitty::Protocol::Control::Configuration;
using Ubiquitty::Protocol::Control::ControlRoute;
using Ubiquitty::Protocol::Control::Dispatcher;
using Ubiquitty::Protocol::Control::HostOperations;
using Ubiquitty::Protocol::Control::HostReply;
using Ubiquitty::Protocol::Control::MethodDescriptor;
using Ubiquitty::Protocol::Control::Principal;
using Ubiquitty::Protocol::Control::Principals;
using Ubiquitty::Protocol::Control::Server;
using Ubiquitty::Security::AgentPolicy;
using Ubiquitty::Security::ApprovalMode;

/// One verb the fake frontend was asked to perform.
struct HostCall {
    std::string verb;
    Value parameters;
};

/// A frontend with canned replies that records every verb it is handed.
class FakeHost {
public:
    /// Sets the JSON reply `verb` answers with.
    void answer(const std::string& verb, const std::string& json) { m_replies[verb] = json; }

    [[nodiscard]] const std::vector<HostCall>& calls() const { return m_calls; }

    /// The verbs performed, in order, comma-separated.
    [[nodiscard]] std::string verbs() const
    {
        std::string out;
        for (const HostCall& call : m_calls)
            out += (out.empty() ? "" : ",") + call.verb;
        return out;
    }

    [[nodiscard]] HostOperations operations()
    {
        HostOperations operations;
        operations.context = this;
        operations.perform = perform;
        return operations;
    }

private:
    static void perform(void* context, const char* verb, std::size_t verbLength, const char* parametersJSON, std::size_t parametersLength, HostReply reply, void* replyContext)
    {
        auto* host = static_cast<FakeHost*>(context);
        bool isParsed = false;
        HostCall call { std::string(verb, verbLength), Ubiquitty::Codec::JSON::parse(std::string(parametersJSON, parametersLength), isParsed) };
        const auto found = host->m_replies.find(call.verb);
        const std::string json = found == host->m_replies.end() ? std::string(R"({"result":{}})") : found->second;
        host->m_calls.push_back(std::move(call));
        reply(replyContext, json.data(), json.size());
    }

    std::map<std::string, std::string> m_replies;
    std::vector<HostCall> m_calls;
};

/// A scratch file for one case, removed with it.
class ScratchFile {
public:
    explicit ScratchFile(const std::string& contents = std::string())
    {
        char path[] = "/tmp/ubiquitty-control-server-XXXXXX";
        const int fileDescriptor = ::mkstemp(path);
        if (fileDescriptor >= 0) {
            if (!contents.empty())
                CHECK_EQ(::write(fileDescriptor, contents.data(), contents.size()), static_cast<ssize_t>(contents.size()));
            ::close(fileDescriptor);
        }
        m_path = path;
    }

    ~ScratchFile() { ::unlink(m_path.c_str()); }

    [[nodiscard]] const std::string& path() const { return m_path; }

    [[nodiscard]] std::vector<std::string> lines() const
    {
        std::vector<std::string> out;
        std::ifstream in(m_path);
        for (std::string line; std::getline(in, line);)
            out.push_back(line);
        return out;
    }

private:
    std::string m_path;
};

/// The audit log pointed at a scratch file for one case.
class ScratchAuditLog : public ScratchFile {
public:
    ScratchAuditLog() { ::setenv("UBIQUITTY_AGENT_AUDIT", path().c_str(), 1); }
    ~ScratchAuditLog() { ::unsetenv("UBIQUITTY_AGENT_AUDIT"); }
};

AgentPolicy policyFrom(const std::string& rules)
{
    const ScratchFile file(rules);
    AgentPolicy policy;
    policy.loadFromFile(file.path().c_str());
    return policy;
}

Value parsed(const std::string& json)
{
    bool isParsed = false;
    return Ubiquitty::Codec::JSON::parse(json, isParsed);
}

int errorCodeOf(const Value& response)
{
    const Value* error = response.find("error");
    return error ? static_cast<int>(error->numberOr("code", 0)) : 0;
}

/// The audit line with its timestamp removed.
std::string withoutStamp(const std::string& line)
{
    const std::size_t space = line.find(' ');
    return space == std::string::npos ? line : line.substr(space + 1);
}

} // namespace

TEST(testListingIsTheHostsWindowsVerbatim)
{
    FakeHost host;
    host.answer("ls", R"({"result":{"windows":[{"id":12,"title":"zsh","focused":true,"remote":false,"tabs":[{"index":0,"windowID":12,"title":"zsh","selected":true,"panes":[{"id":3,"focused":true,"columns":80,"rows":24,"title":"zsh"}]}]}]}})");
    const Dispatcher dispatcher("", AgentPolicy(), ApprovalMode::RequireConfirmation, host.operations());

    const Value response = parsed(dispatcher.handleLine(R"({"jsonrpc":"2.0","id":1,"method":"ls"})"));
    CHECK_STR_EQ(host.verbs(), "ls");
    const Value* result = response.find("result");
    CHECK_MSG(result && result->find("windows"), "the response carries the host's windows");
    if (result && result->find("windows")) {
        const auto& windows = result->find("windows")->items();
        CHECK_EQ(windows.size(), std::size_t(1));
        CHECK_EQ(windows.empty() ? 0.0 : windows[0].numberOr("id"), 12.0);
        const Value* tabs = windows.empty() ? nullptr : windows[0].find("tabs");
        CHECK_MSG(tabs && tabs->items().size() == 1 && tabs->items()[0].find("panes") && tabs->items()[0].find("panes")->items()[0].numberOr("columns") == 80, "the pane's grid survives the round trip");
    }
}

TEST(testSendTextResolvesItsTargetBeforeTheGateAndTypesThere)
{
    ScratchAuditLog audit;
    FakeHost host;
    host.answer("target", R"({"result":{"window":41,"pane":7,"sessionID":9}})");
    host.answer("send-text", R"({"result":{"sent":8}})");
    const Dispatcher dispatcher("", policyFrom("default allow\n"), ApprovalMode::RequireConfirmation, host.operations());

    const Value response = parsed(dispatcher.handleLine(R"({"jsonrpc":"2.0","id":2,"method":"send-text","params":{"text":"echo hi\n"}})"));
    CHECK_EQ(errorCodeOf(response), 0);
    CHECK_STR_EQ(host.verbs(), "target,send-text");
    if (host.calls().size() == 2) {
        CHECK_EQ(host.calls()[0].parameters.numberOr("window", -1), 0.0);
        CHECK_EQ(host.calls()[0].parameters.numberOr("pane", -1), 0.0);
        const Value& typed = host.calls()[1].parameters;
        CHECK_EQ(typed.numberOr("window"), 41.0);
        CHECK_EQ(typed.numberOr("pane"), 7.0);
        CHECK_STR_EQ(typed.stringOr("text"), "echo hi\n");
    }
    const Value* result = response.find("result");
    CHECK_EQ(result ? result->numberOr("sent") : 0.0, 8.0);

    const std::vector<std::string> lines = audit.lines();
    CHECK_EQ(lines.size(), std::size_t(1));
    CHECK_STR_EQ(lines.empty() ? std::string() : withoutStamp(lines[0]), "session=9 control-send(window=41 pane=7) allow: echo hi");
}

TEST(testADeniedSendNeverReachesTheHost)
{
    ScratchAuditLog audit;
    FakeHost host;
    host.answer("target", R"({"result":{"window":41,"pane":7,"sessionID":0}})");
    const Dispatcher dispatcher("", policyFrom("default allow\ndeny rm\n"), ApprovalMode::AutoApprove, host.operations());

    const Value response = parsed(dispatcher.handleLine(R"({"jsonrpc":"2.0","id":3,"method":"send-text","params":{"text":"rm -rf /"}})"));
    CHECK_EQ(errorCodeOf(response), -32002);
    CHECK_STR_EQ(host.verbs(), "target");
    const std::vector<std::string> lines = audit.lines();
    CHECK_STR_EQ(lines.empty() ? std::string() : withoutStamp(lines[0]), "session=0 control-send(window=41 pane=7) deny: rm -rf /");
}

TEST(testWithNoRuleFileNothingIsTypedOrRead)
{
    ScratchAuditLog audit;
    FakeHost host;
    host.answer("target", R"({"result":{"window":5,"pane":1,"sessionID":0}})");
    const Dispatcher dispatcher("", AgentPolicy(), ApprovalMode::RequireConfirmation, host.operations());

    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":1,"method":"send-text","params":{"text":"ls"}})"))), -32002);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":2,"method":"get-text"})"))), -32002);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":3,"method":"new-window"})"))), -32002);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":4,"method":"split"})"))), -32002);
    CHECK_STR_EQ(host.verbs(), "target,target,target");
}

TEST(testGetTextPassesTheSourceThroughAndAuditsTheRead)
{
    ScratchAuditLog audit;
    FakeHost host;
    host.answer("target", R"({"result":{"window":5,"pane":2,"sessionID":0}})");
    host.answer("get-text", R"({"result":{"source":"selection","text":"picked"}})");
    const Dispatcher dispatcher("", policyFrom("allow <read screen>\n"), ApprovalMode::RequireConfirmation, host.operations());

    const Value response = parsed(dispatcher.handleLine(R"({"id":1,"method":"get-text","params":{"pane":2,"source":"selection"}})"));
    CHECK_EQ(errorCodeOf(response), 0);
    CHECK_STR_EQ(host.verbs(), "target,get-text");
    if (host.calls().size() == 2) {
        CHECK_EQ(host.calls()[0].parameters.numberOr("pane"), 2.0);
        CHECK_STR_EQ(host.calls()[1].parameters.stringOr("source"), "selection");
        CHECK_EQ(host.calls()[1].parameters.numberOr("window"), 5.0);
    }
    const Value* result = response.find("result");
    CHECK_STR_EQ(result ? result->stringOr("text") : std::string(), "picked");
    const std::vector<std::string> lines = audit.lines();
    CHECK_MSG(!lines.empty() && withoutStamp(lines[0]).starts_with("session=0 control-screen(window=5 pane=2) allow"), "the read names its target in the audit");

    host.answer("get-text", R"({"result":{"source":"screen","text":""}})");
    (void)dispatcher.handleLine(R"({"id":2,"method":"get-text"})");
    CHECK_STR_EQ(host.calls().empty() ? std::string() : host.calls().back().parameters.stringOr("source"), "screen");
}

TEST(testAHostErrorBecomesTheResponse)
{
    FakeHost host;
    host.answer("target", R"({"error":"no window"})");
    const Dispatcher dispatcher("", policyFrom("default allow\n"), ApprovalMode::AutoApprove, host.operations());

    const Value response = parsed(dispatcher.handleLine(R"({"id":1,"method":"send-text","params":{"text":"x"}})"));
    CHECK_EQ(errorCodeOf(response), -32602);
    const Value* error = response.find("error");
    CHECK_STR_EQ(error ? error->stringOr("message") : std::string(), "no window");
    CHECK_STR_EQ(host.verbs(), "target");

    host.answer("ls", "not json");
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":2,"method":"ls"})"))), -32603);
}

TEST(testSplitAndCreationAreGatedAsDescriptions)
{
    ScratchAuditLog audit;
    FakeHost host;
    host.answer("target", R"({"result":{"window":8,"pane":3,"sessionID":0}})");
    const Dispatcher dispatcher("", policyFrom("default allow\ndeny <new tab>\n"), ApprovalMode::RequireConfirmation, host.operations());

    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":1,"method":"split","params":{"direction":"down"}})"))), 0);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":2,"method":"new-window"})"))), 0);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":3,"method":"new-tab"})"))), -32002);
    CHECK_STR_EQ(host.verbs(), "target,split,new-window");
    if (host.calls().size() == 3)
        CHECK_STR_EQ(host.calls()[1].parameters.stringOr("direction"), "down");
    const std::vector<std::string> lines = audit.lines();
    CHECK_EQ(lines.size(), std::size_t(3));
    if (lines.size() == 3) {
        CHECK_STR_EQ(withoutStamp(lines[0]), "session=0 control-split(window=8) allow: <split down>");
        CHECK_STR_EQ(withoutStamp(lines[1]), "session=0 control-new allow: <new window>");
        CHECK_STR_EQ(withoutStamp(lines[2]), "session=0 control-new deny: <new tab>");
    }
}

TEST(testFocusAndTitleAreUngatedAndUnknownMethodsAreNotFound)
{
    FakeHost host;
    const Dispatcher dispatcher("", AgentPolicy(), ApprovalMode::RequireConfirmation, host.operations());

    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":1,"method":"focus","params":{"window":4}})"))), 0);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":2,"method":"set-title","params":{"title":"build"}})"))), 0);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":3,"method":"close-everything"})"))), -32601);
    CHECK_STR_EQ(host.verbs(), "focus,set-title");
    if (host.calls().size() == 2) {
        CHECK_EQ(host.calls()[0].parameters.numberOr("window"), 4.0);
        CHECK_STR_EQ(host.calls()[1].parameters.stringOr("title"), "build");
    }

    const Value ping = parsed(dispatcher.handleLine(R"({"id":4,"method":"ping"})"));
    const Value* result = ping.find("result");
    CHECK_MSG(result && result->find("methods") && result->find("methods")->items().size() == 12, "ping lists the twelve control methods the registry names");
    CHECK_STR_EQ(host.verbs(), "focus,set-title");
}

TEST(testATokenGatesTheHost)
{
    FakeHost host;
    const Dispatcher dispatcher("secret", AgentPolicy(), ApprovalMode::RequireConfirmation, host.operations());
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":1,"method":"ls"})"))), -32001);
    CHECK_MSG(host.calls().empty(), "an unauthorised caller reaches no host verb");
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":2,"method":"ls","params":{"token":"secret"}})"))), 0);
    CHECK_STR_EQ(host.verbs(), "ls");
}

TEST(testTheServerAnswersOverAnOwnerOnlySocket)
{
    char directory[] = "/tmp/ubiquitty-control-home-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    ::setenv("UBIQUITTY_HOME", (std::string(directory) + "/home").c_str(), 1);
    const ScratchFile policyFile("default allow\n");
    ::setenv("UBIQUITTY_AGENT_POLICY", policyFile.path().c_str(), 1);

    FakeHost host;
    host.answer("ls", R"({"result":{"windows":[]}})");
    Configuration configuration;
    configuration.socketPath = std::string(directory) + "/control.sock";
    std::unique_ptr<Server, decltype(&releaseControlServer)> server(Server::start(configuration, host.operations()), &releaseControlServer);
    CHECK(server != nullptr);

    struct stat status {};
    CHECK_EQ(::lstat(configuration.socketPath.c_str(), &status), 0);
    CHECK_EQ(status.st_mode & 0777, mode_t(0600));

    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", configuration.socketPath.c_str());
    CHECK_EQ(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    const std::string request = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ls\"}\n";
    CHECK_EQ(::write(client, request.data(), request.size()), static_cast<ssize_t>(request.size()));
    std::string response;
    char buffer[512];
    while (response.find('\n') == std::string::npos) {
        const ssize_t count = ::read(client, buffer, sizeof(buffer));
        if (count <= 0)
            break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(client);
    CHECK_STR_EQ(response, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"windows\":[]}}\n");

    server.reset();
    CHECK_MSG(::access(configuration.socketPath.c_str(), F_OK) != 0, "the last release removes the socket file");
    ::unsetenv("UBIQUITTY_AGENT_POLICY");
    ::unsetenv("UBIQUITTY_HOME");
    // Cleanup, after every assertion has run: a directory left under /tmp
    // changes no verdict, so the removal's status is not this case's to check.
    [[maybe_unused]] const int removalStatus = std::system((std::string("rm -rf ") + directory).c_str());
}

// The negative half of schema validation: a request with a bad parameter is
// refused before it has any effect. No host verb runs — not even the `target`
// lookup — no policy verdict is reached, and nothing is written to the audit
// log, however permissive the rule file.
TEST(testABadParameterIsRefusedBeforeAnyEffect)
{
    ScratchAuditLog audit;
    FakeHost host;
    host.answer("target", R"({"result":{"window":5,"pane":2,"sessionID":0}})");
    const Dispatcher dispatcher("", policyFrom("default allow\nallow <read screen>\n"), ApprovalMode::AutoApprove, host.operations());

    struct Refusal {
        const char* line;
        const char* parameter;
    };
    for (const Refusal& refusal : {
             Refusal { R"({"id":1,"method":"send-text","params":{"text":"ls\n","txet":"rm -rf ~\n"}})", "/txet" },
             Refusal { R"({"id":2,"method":"send-text","params":{"text":5}})", "/text" },
             Refusal { R"({"id":3,"method":"send-text","params":{"pane":2}})", "/text" },
             Refusal { R"({"id":4,"method":"send-text","params":{"text":"ls","pane":-1}})", "/pane" },
             Refusal { R"({"id":5,"method":"get-text","params":{"source":"everything"}})", "/source" },
             Refusal { R"({"id":6,"method":"split","params":{"direction":"up"}})", "/direction" },
             Refusal { R"({"id":7,"method":"new-window","params":{"command":"sh"}})", "/command" },
             Refusal { R"({"id":8,"method":"focus","params":{"window":1.5}})", "/window" },
         }) {
        const Value response = parsed(dispatcher.handleLine(refusal.line));
        CHECK_MSG(errorCodeOf(response) == -32602, refusal.line);
        const Value* error = response.find("error");
        const Value* data = error ? error->find("data") : nullptr;
        CHECK_STR_EQ(data ? data->stringOr("kind") : std::string(), "bad_request");
        CHECK_STR_EQ(data ? data->stringOr("parameter") : std::string(), refusal.parameter);
        CHECK_MSG(error && !error->stringOr("message").empty(), "the refusal is a sentence a person can act on");
    }
    CHECK_STR_EQ(host.verbs(), "");
    CHECK_EQ(audit.lines().size(), std::size_t(0));

    // A token rides beside the parameters without being one.
    const Dispatcher tokened("secret", AgentPolicy(), ApprovalMode::RequireConfirmation, host.operations());
    CHECK_EQ(errorCodeOf(parsed(tokened.handleLine(R"({"id":9,"method":"focus","params":{"token":"secret","window":4}})"))), 0);
}

TEST(testTheSocketDescribesItselfFromTheRegistry)
{
    FakeHost host;
    const Dispatcher dispatcher("", AgentPolicy(), ApprovalMode::RequireConfirmation, host.operations());

    const Value listed = parsed(dispatcher.handleLine(R"({"id":1,"method":"list"})"));
    const Value* methods = listed.find("result") ? listed.find("result")->find("methods") : nullptr;
    CHECK_MSG(methods && methods->items().size() == 12, "list names every control method and nothing else");
    bool isDescribeListed = false;
    for (const Value& method : methods ? methods->items() : std::vector<Value>()) {
        isDescribeListed = isDescribeListed || method.stringOr("name") == "describe";
        CHECK_MSG(!method.stringOr("controlMethod").empty(), "every listed method is one the socket serves");
    }
    CHECK(isDescribeListed);

    const Value described = parsed(dispatcher.handleLine(R"({"id":2,"method":"describe","params":{"method":"send-text"}})"));
    const Value* result = described.find("result");
    const Value* schema = result ? result->find("params") : nullptr;
    CHECK_MSG(schema && schema->find("properties") && schema->find("properties")->find("text"), "describe carries the parameter schema");
    CHECK_MSG(result && result->find("result") && result->find("example") && result->find("principals"), "and the result schema, an example and its callers");

    // Any name an operation is reached by describes it.
    const Value byTool = parsed(dispatcher.handleLine(R"({"id":3,"method":"describe","params":{"method":"terminal_run"}})"));
    CHECK_STR_EQ(byTool.find("result") ? byTool.find("result")->stringOr("mcpTool") : std::string(), "terminal_run");
    const Value byVerb = parsed(dispatcher.handleLine(R"({"id":5,"method":"describe","params":{"method":"wait"}})"));
    CHECK_STR_EQ(byVerb.find("result") ? byVerb.find("result")->stringOr("name") : std::string(), "ubq.wait");

    const Value unknown = parsed(dispatcher.handleLine(R"({"id":4,"method":"describe","params":{"method":"teleport"}})"));
    CHECK_EQ(errorCodeOf(unknown), -32003);
    CHECK_STR_EQ(host.verbs(), "");
}

namespace {

constexpr std::string_view anyObject = R"json({"type":"object"})json";
constexpr std::string_view targetedText = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":0},"pane":{"type":"integer","minimum":0},"text":{"type":"string","minLength":1}},"required":["text"],"additionalProperties":false})json";
constexpr std::string_view targeted = R"json({"type":"object","properties":{"window":{"type":"integer","minimum":0},"pane":{"type":"integer","minimum":0},"direction":{"type":"string","enum":["right","down"]}},"additionalProperties":false})json";

/// One control method under a name no dispatcher has ever compared against.
constexpr MethodDescriptor routed(std::string_view name, ControlRoute route, std::string_view parameters = anyObject)
{
    return MethodDescriptor {
        .name = name,
        .description = "A method only its route serves.",
        .parametersSchema = parameters,
        .resultSchema = anyObject,
        .example = R"json({"params":{},"result":{}})json",
        .principals = Principals { Principal::LocalProcess },
        .controlMethod = name,
        .controlRoute = route,
        .commandLineVerb = "",
        .mcpTool = "",
    };
}

constexpr MethodDescriptor routedOnly[] {
    routed("hello", ControlRoute::Handshake),
    routed("enumerate", ControlRoute::ListOperations),
    routed("explain", ControlRoute::DescribeOperation, R"json({"type":"object","properties":{"method":{"type":"string"}},"required":["method"]})json"),
    routed("arrange", ControlRoute::Host, R"json({"type":"object","properties":{"layout":{"type":"string"}},"required":["layout"],"additionalProperties":false})json"),
    routed("paste", ControlRoute::HostTypesText, targetedText),
    routed("peek", ControlRoute::HostReadsScreen, targeted),
    routed("new-workspace", ControlRoute::HostOpens),
    routed("divide", ControlRoute::HostSplits, targeted),
};

} // namespace

TEST(testAMethodIsServedAsItsEntryRoutesItWithNoBranchOfItsOwn)
{
    ScratchAuditLog audit;
    FakeHost host;
    host.answer("target", R"({"result":{"window":5,"pane":2,"sessionID":0}})");
    const Dispatcher dispatcher("", policyFrom("allow echo\nallow <read screen>\nallow <split down>\ndeny <new workspace>\n"), ApprovalMode::RequireConfirmation, host.operations(), routedOnly);

    const Value hello = parsed(dispatcher.handleLine(R"({"id":1,"method":"hello"})"));
    const Value* greeting = hello.find("result");
    CHECK_STR_EQ(greeting ? greeting->stringOr("name") : std::string(), "ubiquitty-control");
    CHECK_MSG(greeting && greeting->find("methods") && greeting->find("methods")->items().size() == std::size(routedOnly), "the handshake names the table's methods");

    const Value listed = parsed(dispatcher.handleLine(R"({"id":2,"method":"enumerate"})"));
    const Value* methods = listed.find("result") ? listed.find("result")->find("methods") : nullptr;
    CHECK_MSG(methods && methods->items().size() == std::size(routedOnly), "the listing is the table's");
    const Value explained = parsed(dispatcher.handleLine(R"({"id":3,"method":"explain","params":{"method":"paste"}})"));
    CHECK_STR_EQ(explained.find("result") ? explained.find("result")->stringOr("name") : std::string(), "paste");
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":4,"method":"explain","params":{"method":"send-text"}})"))), -32003);
    CHECK_STR_EQ(host.verbs(), "");

    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":5,"method":"arrange","params":{"layout":"grid"}})"))), 0);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":6,"method":"paste","params":{"text":"echo hi"}})"))), 0);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":7,"method":"paste","params":{"text":"rm -rf /"}})"))), -32002);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":8,"method":"peek"})"))), 0);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":9,"method":"new-workspace"})"))), -32002);
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":10,"method":"divide","params":{"direction":"down"}})"))), 0);
    // A method the shipped registry serves is not served by a dispatcher over
    // another table: the table is the whole of what a socket answers.
    CHECK_EQ(errorCodeOf(parsed(dispatcher.handleLine(R"({"id":11,"method":"send-text","params":{"text":"echo hi"}})"))), -32601);

    CHECK_STR_EQ(host.verbs(), "arrange,target,paste,target,target,peek,target,divide");
    if (host.calls().size() == 8) {
        CHECK_STR_EQ(host.calls()[0].parameters.stringOr("layout"), "grid");
        CHECK_STR_EQ(host.calls()[2].parameters.stringOr("text"), "echo hi");
        CHECK_EQ(host.calls()[2].parameters.numberOr("pane"), 2.0);
        CHECK_STR_EQ(host.calls()[5].parameters.stringOr("source"), "screen");
        CHECK_STR_EQ(host.calls()[7].parameters.stringOr("direction"), "down");
    }
    const std::vector<std::string> lines = audit.lines();
    CHECK_EQ(lines.size(), std::size_t(5));
    if (lines.size() == 5) {
        CHECK_STR_EQ(withoutStamp(lines[0]), "session=0 control-send(window=5 pane=2) allow: echo hi");
        CHECK_MSG(withoutStamp(lines[1]).starts_with("session=0 control-send(window=5 pane=2) "), lines[1].c_str());
        CHECK_MSG(withoutStamp(lines[2]).starts_with("session=0 control-screen(window=5 pane=2) allow"), lines[2].c_str());
        CHECK_STR_EQ(withoutStamp(lines[3]), "session=0 control-new deny: <new workspace>");
        CHECK_STR_EQ(withoutStamp(lines[4]), "session=0 control-split(window=5) allow: <split down>");
    }
}

TEST(testEveryShippedControlMethodIsRoutedAndEveryRouteNamesAMethod)
{
    for (const MethodDescriptor& method : Ubiquitty::Protocol::Control::methodRegistry())
        CHECK_MSG(method.controlMethod.empty() == (method.controlRoute == ControlRoute::NotServed), std::string(method.name).c_str());
}

UBIQUITTY_TEST_MAIN()
