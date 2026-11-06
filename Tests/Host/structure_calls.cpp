// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/structure_calls.cpp — the calls that change what a workspace's
// structure says (Docs/sync.md §7.3): `label.set`, `tags.set`,
// `workspace.rename`, `tab.rename`, `tab.move`, `window.close`, and
// `workspace.addTab`, `workspace.split`, `workspace.closeTab`, `workspace.closePane`.
//
// One daemon holds a workspace of two windows and three tabs, a second
// workspace of one pane, a third an agent was granted, and a record of one it
// could restore. A connection that subscribed to the host's listing watches,
// and after every change that is served the structure it holds has taken a
// new revision and says what the call's answer said. The host records what a
// served call changed, and a person attached to the workspace is sent its
// structure after it.
//
//   1. the host's own user, on a connection bound to no session, sets and
//      clears a label and a set of tags on a workspace, a window, a tab, a
//      pane and a session, and what the host does not take is refused with
//      the book's own sentence;
//   2. renames a workspace and a tab, and a name another workspace holds, or
//      one longer than a structure carries, is refused with the reason in the
//      answer;
//   3. reorders the tabs of a window and moves a tab to a place in another,
//      the tab keeping its identity, name and label, and a place the window
//      does not have, or a window of another workspace, is refused with the
//      reason;
//   4. every call against every caller, as Docs/sync.md §7.3 tabulates
//      them: a person on another machine changes the workspace their
//      connection is attached to and no other; a viewer and a person whose
//      token only observes are refused every call for their role; an agent
//      waits for a verified attached person's approval of each exact operation,
//      whatever standing attach grant it holds; no person, denial, expiry,
//      changed target, lost authority and disconnected requester fail closed;
//   5. a closed window's sessions end after the pass that served the call,
//      and a workspace whose only window is closed is withdrawn from the
//      listing, the caller attached to it left standing until then.
//
// Friend seam, the QUIC-off host build, as sync_host_calls. Exit 0 on
// success.
//

#include "Assertions.h"
#include "NotifyRead.h"

#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Roles.h"
#include "Core/Protocol/Sync/StructureConsent.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"
#include "Host/FullCarrier.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

using Ubiquitty::Host::Daemon;
using Ubiquitty::Session::Identity::ObjectID;
using Ubiquitty::Session::Identity::ObjectKind;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Layout = Ubiquitty::Session::Layout;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Tree = Ubiquitty::Session;

// A fact the rest of the test stands on: checked, and the test ends where it
// does not hold.
#define PREMISE(expression)                                   \
    do {                                                      \
        const bool isPremise = static_cast<bool>(expression); \
        CHECK_MSG(isPremise, #expression);                    \
        if (!isPremise)                                       \
            return 1;                                         \
    } while (0)

namespace {

/// One client of the daemon and what it was answered.
struct Caller {
    Daemon& daemon;
    Ubiquitty::HostTest::FullCarrier carrier;
    std::size_t index;
    UbiquittyTest::ScreenClient client { 24, 80 };
    int nextID = 1;
    std::string lastParameters;

    Caller(Daemon& d, std::uint32_t sessionID)
        : Caller(d, Sync::AdmissionTarget::Session, sessionID)
    {
    }

    Caller(Daemon& d, Sync::AdmissionTarget target, std::uint32_t sessionID = 0)
        : daemon(d)
        , index(target == Sync::AdmissionTarget::Control ? Screen::connect(d, carrier.takeDaemonEnd()) : Screen::attach(d, sessionID, carrier.takeDaemonEnd()))
    {
        CHECK(carrier.isReady());
        CHECK((target == Sync::AdmissionTarget::Control && !sessionID) || (target == Sync::AdmissionTarget::Session && sessionID));
    }

    /// Calls a method and returns the one answer.
    std::string call(const std::string& method, const std::string& parameters)
    {
        send(method, parameters);
        const std::vector<std::string> results = client.takeResults();
        CHECK_MSG(results.size() == 1, (method + " drew " + std::to_string(results.size()) + " results").c_str());
        return results.empty() ? std::string() : results.front();
    }

    /// Sends a call without assuming it is answered before a person decides.
    void send(const std::string& method, const std::string& parameters)
    {
        lastParameters = confirmedParameters(method, parameters);
        const std::string request = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(nextID++) + ",\"method\":\"" + method + "\",\"params\":" + lastParameters + "}";
        CHECK(client.call(request));
        Screen::deliver(daemon, index, client);
        Screen::deliver(daemon, index, client);
    }

    /// Existing authority cases ask against the current tree; explicit stale
    /// and malformed revisions are preserved for the revision-negative cases.
    std::string confirmedParameters(const std::string& method, const std::string& parameters)
    {
        if (!Sync::requiresStructureRevision(method))
            return parameters;
        bool isJSON = false;
        auto params = Ubiquitty::Codec::JSON::parse(parameters, isJSON);
        if (!isJSON || params.find("expectedRevision"))
            return parameters;
        const auto revision = method == "pane.zoom" ? Screen::paneStructureRevision(daemon, static_cast<std::uint32_t>(params.numberOr("pane")))
                                                    : Screen::windowStructureRevision(daemon, static_cast<std::uint32_t>(params.numberOr("window")));
        params.set("expectedRevision", Ubiquitty::Codec::JSON::Value::string(std::to_string(revision ? revision : 1)));
        return Ubiquitty::Codec::JSON::serialize(params);
    }

    /// Sends two calls in one pass of the daemon and returns both answers,
    /// in the order they were made.
    std::vector<std::string> callBoth(const std::string& first, const std::string& second)
    {
        for (const std::string* call : { &first, &second }) {
            const std::string request = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(nextID++) + "," + *call + "}";
            bool isJSON = false;
            const auto decoded = Ubiquitty::Codec::JSON::parse(request, isJSON);
            CHECK(isJSON && decoded.find("params"));
            if (!isJSON || !decoded.find("params"))
                continue;
            auto current = Ubiquitty::Codec::JSON::Value::object();
            current.set("jsonrpc", Ubiquitty::Codec::JSON::Value::string("2.0"));
            current.set("id", *decoded.find("id"));
            current.set("method", *decoded.find("method"));
            current.set("params", Ubiquitty::Codec::JSON::parse(confirmedParameters(decoded.stringOr("method"), Ubiquitty::Codec::JSON::serialize(*decoded.find("params"))), isJSON));
            CHECK(client.call(Ubiquitty::Codec::JSON::serialize(current)));
        }
        Screen::deliver(daemon, index, client);
        Screen::deliver(daemon, index, client);
        std::vector<std::string> results = client.takeResults();
        CHECK_MSG(results.size() == 2, ("two calls drew " + std::to_string(results.size()) + " results").c_str());
        results.resize(2);
        return results;
    }

    [[nodiscard]] bool isAttached() const
    {
        const Sync::HostConnection* connection = Screen::connection(daemon, index);
        return connection && connection->state() == Sync::ConnectionState::Attached;
    }
};

bool has(const std::string& answer, const std::string& needle) { return answer.find(needle) != std::string::npos; }
bool isServed(const std::string& answer) { return has(answer, "\"result\"") && !has(answer, "\"error\""); }

std::string structureDecision(std::uint32_t requestID, Sync::ConsentChoice decision)
{
    return "{\"request\":" + std::to_string(requestID) + ",\"kind\":\"structure\",\"decision\":\""
        + (decision == Sync::ConsentChoice::Deny ? "deny" : "once") + "\"}";
}

bool resolves(const Sync::Notify& text, const Sync::StructureConsent& question)
{
    const auto resolution = UbiquittyTest::consentResolvedOf(text);
    return resolution && resolution->kind == Sync::ConsentKind::Structure && resolution->requestID == question.requestID && resolution->sessionID == question.sessionID;
}

std::string quoted(const std::string& text) { return "\"" + text + "\""; }
std::string named(ObjectID id) { return quoted(Ubiquitty::Session::Identity::format(id)); }

/// The structures a client holds, decoded, by workspace id.
std::map<std::uint32_t, Tree::TreeWorkspace> structuresOf(const UbiquittyTest::ScreenClient& client)
{
    std::map<std::uint32_t, Tree::TreeWorkspace> structures;
    for (const auto& [workspaceID, held] : client.connection().workspaceStructures()) {
        std::optional<Tree::TreeWorkspace> workspace = Tree::decodeWorkspaceStructure(held.record);
        CHECK_MSG(workspace.has_value(), "a structure the daemon sent does not decode");
        if (workspace)
            structures[workspaceID] = std::move(*workspace);
    }
    return structures;
}

/// The ids of a window's tabs, in the order the structure lists them.
std::vector<std::uint32_t> tabsOf(const Tree::TreeWindow& window)
{
    std::vector<std::uint32_t> tabs;
    for (const Tree::TreeTab& tab : window.tabs)
        tabs.push_back(std::to_underlying(tab.id));
    return tabs;
}

const Tree::TreeWindow* windowOf(const Tree::TreeWorkspace& workspace, Layout::SurfaceID id)
{
    for (const Tree::TreeWindow& window : workspace.windows) {
        if (window.id == id)
            return &window;
    }
    return nullptr;
}

const Tree::TreeTab* tabOf(const Tree::TreeWorkspace& workspace, Layout::TabID id)
{
    for (const Tree::TreeWindow& window : workspace.windows) {
        for (const Tree::TreeTab& tab : window.tabs) {
            if (tab.id == id)
                return &tab;
        }
    }
    return nullptr;
}

const Tree::TreePane* paneOf(const Tree::TreeWorkspace& workspace, std::uint32_t sessionID)
{
    for (const Tree::TreeWindow& window : workspace.windows) {
        for (const Tree::TreeTab& tab : window.tabs) {
            for (const Tree::TreePane& pane : tab.panes) {
                if (pane.sessionID == sessionID)
                    return &pane;
            }
        }
    }
    return nullptr;
}

/// All six operations travel through a real Call/Notify/answer exchange;
/// rejection, expiry and lost authority never execute the stored request.
int exactOperationConsent()
{
    enum class Decision {
        Allow,
        Deny,
        Expire,
        ChangedTarget,
        RequesterScope,
        RequesterCreationScope,
        RequesterPrincipal,
        ApproverScope,
        RequesterLeft
    };
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control };
    {
        Daemon daemon;
        const auto& standalone = Screen::makeSession(daemon, 24, 80);
        Caller agent(daemon, Sync::AdmissionTarget::Control);
        Screen::grantScope(daemon, agent.index, driving);
        Screen::setTokenID(daemon, agent.index, "standalone requester");
        Screen::greet(daemon, agent.index, agent.client);
        PREMISE(Screen::heldWorkspaces(daemon) == 0);
        for (std::string kind : { "session", "pane" }) {
            const auto refused = Screen::askStructureWithoutPublishing(daemon, agent.index,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"label.set\",\"params\":{\"object\":\"" + kind + ":" + std::to_string(standalone.id) + "\",\"label\":\"must not create\"}}");
            CHECK(refused && refused->find("error"));
            CHECK(Screen::heldWorkspaces(daemon) == 0);
            CHECK(Screen::pendingStructureConsents(daemon) == 0);
        }
    }
    {
        Daemon daemon;
        auto& session = Screen::makeSession(daemon, 24, 80);
        const auto workspace = Screen::nameWorkspace(daemon, session, "bounded");
        Caller person(daemon, session.id);
        Screen::markTrustedLocal(daemon, person.index);
        Screen::bindWorkspace(daemon, person.index, session);
        Screen::greet(daemon, person.index, person.client);
        std::vector<std::unique_ptr<Caller>> agents;
        for (std::size_t index = 0; index <= Sync::maxPendingStructureConsents; ++index) {
            auto agent = std::make_unique<Caller>(daemon, Sync::AdmissionTarget::Control);
            Screen::grantScope(daemon, agent->index, driving);
            Screen::setTokenID(daemon, agent->index, "bounded requester " + std::to_string(index));
            Screen::greet(daemon, agent->index, agent->client);
            const std::string parameters = "{\"object\":\"workspace:" + std::to_string(workspace) + "\",\"label\":\"held\"}";
            if (index == Sync::maxPendingStructureConsents)
                CHECK(has(agent->call("label.set", parameters), "approval queue is full"));
            else {
                agent->send("label.set", parameters);
                CHECK(agent->client.takeResults().empty());
            }
            agents.push_back(std::move(agent));
        }
        CHECK(Screen::pendingStructureConsents(daemon) == Sync::maxPendingStructureConsents);
        Screen::deliver(daemon, person.index, person.client);
        std::vector<std::uint32_t> questions;
        for (const auto& text : person.client.takeNotifies()) {
            if (const auto question = UbiquittyTest::structureConsentOf(text))
                questions.push_back(question->requestID);
        }
        PREMISE(questions.size() == Sync::maxPendingStructureConsents);
        PREMISE(Screen::fillNotifyQueue(daemon, person.index));
        Screen::expireStructureConsents(daemon, Screen::now() / 1000 + Sync::structureConsentTimeoutMilliseconds + 1);
        CHECK(Screen::pendingStructureConsents(daemon) == 0);
        CHECK(Screen::pendingStructureRetirements(daemon, person.index) == questions.size());
        CHECK(!Screen::isDropped(daemon, person.index));
        const auto refused = Screen::askStructureWithoutPublishing(daemon, agents.front()->index,
            "{\"jsonrpc\":\"2.0\",\"id\":999,\"method\":\"label.set\",\"params\":{\"object\":\"workspace:" + std::to_string(workspace) + "\",\"label\":\"must wait\"}}");
        CHECK(refused && refused->find("error"));
        CHECK(Screen::pendingStructureConsents(daemon) == 0);
        CHECK(Screen::pendingStructureRetirements(daemon, person.index) == questions.size());
        Screen::deliver(daemon, person.index, person.client);
        CHECK(Screen::pendingStructureRetirements(daemon, person.index) == 0);
        std::map<std::uint32_t, std::size_t> retirements;
        for (const auto& text : person.client.takeNotifies()) {
            if (const auto retired = UbiquittyTest::consentResolvedOf(text); retired && retired->kind == Sync::ConsentKind::Structure && retired->sessionID == session.id)
                ++retirements[retired->requestID];
        }
        CHECK(retirements.size() == questions.size());
        for (std::uint32_t question : questions)
            CHECK(retirements[question] == 1);
    }
    for (const std::string method : { "label.set", "tags.set", "workspace.rename", "tab.rename", "tab.move", "window.close", "workspace.addWindow", "workspace.addTab", "workspace.split", "workspace.closeTab", "workspace.closePane",
             "window.activate", "tab.activate", "pane.focus", "window.moveFocus", "pane.zoom", "pane.move", "pane.swap",
             "pane.extractTab", "pane.extractWindow", "tab.extractWindow",
             "pane.moveToTab", "tab.moveToPane", "pane.moveToNewTab", "tab.balance", "pane.adjustRatio", "divider.setRatio" }) {
        const bool doesCreateSession = Sync::createsSessionForStructure(method);
        const bool isExtraction = method == "pane.extractTab" || method == "pane.extractWindow" || method == "tab.extractWindow";
        const bool isTransfer = method == "pane.moveToTab" || method == "tab.moveToPane" || method == "pane.moveToNewTab";
        const bool isSizing = method == "tab.balance" || method == "pane.adjustRatio" || method == "divider.setRatio";
        for (Decision decision : { Decision::Allow, Decision::Deny, Decision::Expire, Decision::ChangedTarget, Decision::RequesterScope, Decision::RequesterCreationScope, Decision::RequesterPrincipal, Decision::ApproverScope, Decision::RequesterLeft }) {
            if (decision == Decision::RequesterCreationScope && !doesCreateSession)
                continue;
            Daemon daemon;
            Screen::setRestoreDefaults(daemon);
            auto& first = Screen::makeSession(daemon, 24, 80);
            auto& second = Screen::makeSession(daemon, 24, 80);
            auto& third = Screen::makeSession(daemon, 24, 80);
            const auto workspace = Screen::nameWorkspace(daemon, first, "before");
            const auto firstWindow = Screen::layout(daemon, first).activeSurface();
            const auto movingTab = Screen::tabInto(daemon, first, second);
            const auto closingWindow = Screen::windowInto(daemon, first, third);
            const std::uint32_t closingSession = third.id;
            std::uint32_t otherClosingSession = 0;
            std::uint32_t focusFrom = 0;
            if (method == "pane.focus" || method == "window.moveFocus" || method == "pane.zoom" || method == "pane.move" || method == "pane.swap"
                || method == "pane.extractTab" || method == "pane.extractWindow" || isTransfer || isSizing) {
                auto& sibling = Screen::makeSession(daemon, 24, 80);
                Screen::splitInto(daemon, third, sibling, Ubiquitty::Panes::Orientation::Vertical);
                focusFrom = Screen::layoutPane(daemon, sibling);
            }
            if (method == "workspace.closeTab") {
                auto& sibling = Screen::makeSession(daemon, 24, 80);
                otherClosingSession = sibling.id;
                Screen::splitInto(daemon, third, sibling, Ubiquitty::Panes::Orientation::Vertical);
            }
            const auto numbered = [](auto number) { return std::to_string(number); };
            std::string parameters;
            if (method == "label.set")
                parameters = "{\"object\":\"workspace:" + numbered(workspace) + "\",\"label\":\"after\"}";
            else if (method == "tags.set")
                parameters = "{\"object\":\"workspace:" + numbered(workspace) + "\",\"tags\":[\"after\"]}";
            else if (method == "workspace.rename")
                parameters = "{\"workspace\":" + numbered(workspace) + ",\"name\":\"after\"}";
            else if (method == "tab.rename")
                parameters = "{\"tab\":" + numbered(std::to_underlying(movingTab)) + ",\"name\":\"after\"}";
            else if (method == "tab.move")
                parameters = "{\"tab\":" + numbered(std::to_underlying(movingTab)) + ",\"window\":" + numbered(std::to_underlying(firstWindow)) + ",\"index\":0}";
            else if (isSizing) {
                parameters = "{\"window\":" + numbered(std::to_underlying(closingWindow)) + ",\"tab\":" + numbered(std::to_underlying(Screen::layout(daemon, first).activeTab(closingWindow)));
                if (method == "pane.adjustRatio")
                    parameters += ",\"pane\":" + numbered(focusFrom) + ",\"direction\":\"left\",\"step\":0.1";
                else if (method == "divider.setRatio")
                    parameters += ",\"path\":[],\"ratio\":0.4";
                parameters += "}";
            } else if (method == "workspace.addWindow")
                parameters = "{\"window\":" + numbered(std::to_underlying(firstWindow)) + ",\"pane\":" + numbered(Screen::layoutPane(daemon, second)) + "}";
            else if (method == "workspace.addTab")
                parameters = "{\"window\":" + numbered(std::to_underlying(firstWindow)) + "}";
            else if (method == "workspace.split")
                parameters = "{\"pane\":" + numbered(Screen::layoutPane(daemon, second)) + ",\"orientation\":\"horizontal\",\"ratio\":0.375}";
            else if (method == "workspace.closeTab")
                parameters = "{\"tab\":" + numbered(std::to_underlying(Screen::layout(daemon, first).activeTab(closingWindow))) + "}";
            else if (method == "workspace.closePane")
                parameters = "{\"pane\":" + numbered(Screen::layoutPane(daemon, third)) + "}";
            else if (method == "window.activate")
                parameters = "{\"window\":" + numbered(std::to_underlying(firstWindow)) + "}";
            else if (method == "tab.activate")
                parameters = "{\"tab\":" + numbered(std::to_underlying(Screen::layout(daemon, first).tabHolding(firstWindow, Layout::PaneID { Screen::layoutPane(daemon, first) }))) + "}";
            else if (method == "pane.focus")
                parameters = "{\"pane\":" + numbered(Screen::layoutPane(daemon, third)) + "}";
            else if (method == "window.moveFocus")
                parameters = "{\"window\":" + numbered(std::to_underlying(closingWindow)) + ",\"pane\":" + numbered(focusFrom) + ",\"direction\":\"left\"}";
            else if (method == "pane.zoom")
                parameters = "{\"pane\":" + numbered(Screen::layoutPane(daemon, third)) + ",\"state\":\"zoomed\"}";
            else if (method == "tab.extractWindow")
                parameters = "{\"window\":" + numbered(std::to_underlying(firstWindow)) + ",\"tab\":" + numbered(std::to_underlying(movingTab)) + "}";
            else if (method == "pane.extractTab" || method == "pane.extractWindow")
                parameters = "{\"window\":" + numbered(std::to_underlying(closingWindow)) + ",\"pane\":" + numbered(focusFrom) + "}";
            else if (isTransfer) {
                parameters = "{\"window\":" + numbered(std::to_underlying(closingWindow));
                if (method == "pane.moveToNewTab") {
                    parameters += ",\"pane\":" + numbered(focusFrom) + ",\"targetWindow\":" + numbered(std::to_underlying(firstWindow));
                } else {
                    parameters += method == "tab.moveToPane"
                        ? ",\"tab\":" + numbered(std::to_underlying(Screen::layout(daemon, first).activeTab(closingWindow)))
                        : ",\"pane\":" + numbered(focusFrom) + ",\"tab\":" + numbered(std::to_underlying(movingTab));
                    parameters += ",\"target\":" + numbered(Screen::layoutPane(daemon, second)) + ",\"orientation\":\"horizontal\",\"side\":\"after\",\"insertTarget\":\"root\"";
                }
                parameters += "}";
            } else if (method == "pane.move" || method == "pane.swap") {
                parameters = "{\"window\":" + numbered(std::to_underlying(closingWindow)) + ",\"pane\":" + numbered(focusFrom) + ",\"target\":" + numbered(Screen::layoutPane(daemon, third));
                if (method == "pane.move")
                    parameters += ",\"orientation\":\"horizontal\",\"side\":\"before\"";
                parameters += "}";
            } else
                parameters = "{\"window\":" + numbered(std::to_underlying(closingWindow)) + "}";
            Caller agent(daemon, Sync::AdmissionTarget::Control);
            Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::NewSession });
            Screen::setTokenID(daemon, agent.index, "requester");
            Screen::greet(daemon, agent.index, agent.client);
            CHECK(has(agent.call(method, parameters), "no such person is attached"));
            CHECK(Screen::pendingStructureConsents(daemon) == 0);
            Caller person(daemon, first.id);
            Screen::grantScope(daemon, person.index, driving);
            Screen::setTokenID(daemon, person.index, "person");
            Screen::markInteractiveUser(daemon, person.index);
            Screen::bindWorkspace(daemon, person.index, first);
            Screen::greet(daemon, person.index, person.client);
            Caller viewer(daemon, first.id);
            Screen::grantScope(daemon, viewer.index, { Sync::Scope::Observe });
            Screen::setTokenID(daemon, viewer.index, "viewer");
            Screen::markInteractiveUser(daemon, viewer.index);
            Screen::bindWorkspace(daemon, viewer.index, first);
            Screen::greet(daemon, viewer.index, viewer.client);
            std::unique_ptr<Caller> otherPerson;
            if (decision == Decision::Allow || decision == Decision::Deny) {
                otherPerson = std::make_unique<Caller>(daemon, first.id);
                Screen::grantScope(daemon, otherPerson->index, driving);
                Screen::setTokenID(daemon, otherPerson->index, "other person");
                Screen::markInteractiveUser(daemon, otherPerson->index);
                Screen::bindWorkspace(daemon, otherPerson->index, first);
                Screen::greet(daemon, otherPerson->index, otherPerson->client);
                (void)otherPerson->client.takeNotifies();
            }
            Screen::deliver(daemon, person.index, person.client);
            const auto before = structuresOf(person.client).at(workspace);
            (void)person.client.takeNotifies();
            const std::size_t sessionsBefore = Screen::sessionCount(daemon);
            CHECK(has(viewer.call(method, parameters), "attached as viewer"));
            Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Control, Sync::Scope::NewSession });
            CHECK(has(agent.call(method, parameters), "drive scope"));
            CHECK(Screen::pendingStructureConsents(daemon) == 0);
            Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::NewSession });
            if (doesCreateSession) {
                // The person approves the change, never supplies a missing
                // creation scope. Refusal happens before asking or moving focus.
                Screen::grantScope(daemon, agent.index, driving);
                CHECK(has(agent.call(method, parameters), "does not permit creating a session"));
                CHECK(Screen::pendingStructureConsents(daemon) == 0);
                CHECK(Screen::sessionCount(daemon) == sessionsBefore);
                Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::NewSession });
            }
            agent.send(method, parameters);
            CHECK(agent.client.takeResults().empty());
            PREMISE(Screen::pendingStructureConsents(daemon) == 1);
            Screen::deliver(daemon, person.index, person.client);
            std::vector<Sync::StructureConsent> questions;
            for (const Sync::Notify& text : person.client.takeNotifies()) {
                if (auto question = UbiquittyTest::structureConsentOf(text))
                    questions.push_back(std::move(*question));
            }
            PREMISE(questions.size() == 1);
            const auto& question = questions.front();
            CHECK(question.method == method && question.workspaceID == workspace && question.sessionID == first.id);
            if (otherPerson) {
                Screen::deliver(daemon, otherPerson->index, otherPerson->client);
                std::size_t matchingQuestions = 0;
                for (const Sync::Notify& text : otherPerson->client.takeNotifies()) {
                    if (const auto otherQuestion = UbiquittyTest::structureConsentOf(text))
                        matchingQuestions += otherQuestion->requestID == question.requestID;
                }
                CHECK(matchingQuestions == 1);
            }
            bool isRead = false;
            const auto original = Ubiquitty::Codec::JSON::parse(agent.lastParameters, isRead);
            CHECK(isRead && question.parameters == Ubiquitty::Codec::JSON::serialize(original));
            CHECK(structuresOf(person.client).at(workspace).revision == before.revision);
            CHECK(Screen::hasSession(daemon, closingSession));
            CHECK(Screen::sessionCount(daemon) == sessionsBefore);
            const std::string allow = structureDecision(question.requestID, Sync::ConsentChoice::AllowOnce);
            CHECK(has(viewer.call("consent.respond", allow), "Drive permission"));
            CHECK(has(agent.call("consent.respond", allow), "attached as agent"));
            CHECK(Screen::pendingStructureConsents(daemon) == 1);
            CHECK(!isServed(person.call("consent.respond", "{\"request\":" + numbered(question.requestID) + ",\"kind\":\"structure\",\"decision\":\"once\",\"params\":{\"name\":\"altered\"}}")));
            CHECK(Screen::pendingStructureConsents(daemon) == 1);
            // Per-client cap refuses a second question without replacing the first.
            CHECK(has(agent.call(method, parameters), "already pending"));
            if (decision == Decision::ChangedTarget)
                CHECK(isServed(person.call("label.set", "{\"object\":\"workspace:" + numbered(workspace) + "\",\"label\":\"intervening\"}")));
            if (decision == Decision::RequesterScope)
                Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe });
            if (decision == Decision::RequesterCreationScope)
                Screen::grantScope(daemon, agent.index, driving);
            if (decision == Decision::RequesterPrincipal)
                Screen::setTokenID(daemon, agent.index, "changed requester");
            if (decision == Decision::ApproverScope)
                Screen::grantScope(daemon, person.index, { Sync::Scope::Observe });
            if (decision == Decision::RequesterLeft)
                Screen::drop(daemon, agent.index);
            if (decision == Decision::Expire)
                Screen::expireStructureConsents(daemon, Screen::now() / 1000 + Sync::structureConsentTimeoutMilliseconds + 1);
            if (decision == Decision::RequesterScope || decision == Decision::RequesterCreationScope || decision == Decision::RequesterPrincipal || decision == Decision::ApproverScope)
                Screen::expireStructureConsents(daemon, Screen::now() / 1000);
            Caller& answering = decision == Decision::Allow ? *otherPerson : person;
            const std::string answer = answering.call("consent.respond", decision == Decision::Deny ? structureDecision(question.requestID, Sync::ConsentChoice::Deny) : allow);
            CHECK(isServed(answer) == (decision == Decision::Allow || decision == Decision::Deny));
            CHECK(Screen::pendingStructureConsents(daemon) == 0);
            if (decision != Decision::RequesterLeft) {
                Screen::deliver(daemon, agent.index, agent.client);
                const auto results = agent.client.takeResults();
                PREMISE(results.size() == 1);
                CHECK(isServed(results.front()) == (decision == Decision::Allow));
                if (method == "workspace.closeTab" && decision == Decision::Allow)
                    CHECK(has(results.front(), "\"sessionsEnded\":2"));
                if (doesCreateSession && decision == Decision::Allow) {
                    bool isParsed = false;
                    const auto response = Ubiquitty::Codec::JSON::parse(results.front(), isParsed);
                    const auto* result = response.find("result");
                    PREMISE(isParsed && result);
                    const auto created = Ubiquitty::Session::Identity::parseObjectID(result->stringOr("session"));
                    PREMISE(created && created->kind == ObjectKind::Session);
                    CHECK(Screen::hasStandingGrant(daemon, agent.index, static_cast<std::uint32_t>(created->number)));
                    CHECK(Screen::sessionCount(daemon) == sessionsBefore + 1);
                }
            }
            Screen::deliver(daemon, person.index, person.client);
            std::size_t retirements = 0;
            for (const Sync::Notify& text : person.client.takeNotifies())
                retirements += resolves(text, question);
            CHECK(retirements == 1);
            if (otherPerson) {
                Screen::deliver(daemon, otherPerson->index, otherPerson->client);
                std::size_t otherRetirements = 0;
                for (const Sync::Notify& text : otherPerson->client.takeNotifies())
                    otherRetirements += resolves(text, question);
                CHECK(otherRetirements == 1);
            }
            // A duplicate cannot execute the operation twice, even if the first
            // closed a window and its original caller is no longer present.
            CHECK(!isServed(person.call("consent.respond", allow)));
            if (method == "window.close" || method == "workspace.closeTab" || method == "workspace.closePane")
                CHECK(Screen::hasSession(daemon, closingSession) != (decision == Decision::Allow));
            if (otherClosingSession)
                CHECK(Screen::hasSession(daemon, otherClosingSession) != (decision == Decision::Allow));
            if (doesCreateSession && decision != Decision::Allow)
                CHECK(Screen::sessionCount(daemon) == sessionsBefore);
            const auto after = structuresOf(person.client).at(workspace);
            if (decision == Decision::Allow) {
                const auto& model = Screen::layout(daemon, first);
                if (method == "workspace.addWindow") {
                    CHECK(Screen::sessionCount(daemon) == sessionsBefore + 1);
                    CHECK(model.surfaceCount() == 3 && after.revision > before.revision);
                }
                if (method == "window.activate")
                    CHECK(model.activeSurface() == firstWindow);
                if (method == "tab.activate")
                    CHECK(model.activeTab(firstWindow) == model.tabHolding(firstWindow, Layout::PaneID { Screen::layoutPane(daemon, first) }));
                if (method == "pane.focus" || method == "window.moveFocus")
                    CHECK(std::to_underlying(model.focusedPane(closingWindow)) == Screen::layoutPane(daemon, third));
                if (method == "pane.zoom")
                    CHECK(std::to_underlying(model.zoomedPane(closingWindow)) == Screen::layoutPane(daemon, third));
                if (isExtraction || isTransfer) {
                    CHECK(Screen::sessionCount(daemon) == sessionsBefore);
                    CHECK(after.revision > before.revision);
                    CHECK(Screen::hasSession(daemon, first.id) && Screen::hasSession(daemon, second.id) && Screen::hasSession(daemon, third.id));
                }
                if (method == "pane.move" || method == "pane.swap") {
                    CHECK(Screen::sessionCount(daemon) == sessionsBefore);
                    CHECK(model.surfaceHolding(Layout::PaneID { focusFrom }) == closingWindow);
                    CHECK(model.surfaceHolding(Layout::PaneID { Screen::layoutPane(daemon, third) }) == closingWindow);
                    CHECK(after.revision > before.revision);
                }
            }
            if (decision != Decision::Allow && decision != Decision::ChangedTarget) {
                CHECK(after.name == before.name && after.label == before.label && after.tags == before.tags);
                CHECK(tabsOf(after.windows.front()) == tabsOf(before.windows.front()));
                if (method == "pane.move" || method == "pane.swap" || isExtraction || isTransfer || isSizing) {
                    std::vector<std::uint8_t> beforeBytes;
                    std::vector<std::uint8_t> afterBytes;
                    CHECK(Tree::encodeWorkspaceStructure(beforeBytes, before));
                    CHECK(Tree::encodeWorkspaceStructure(afterBytes, after));
                    CHECK(beforeBytes == afterBytes);
                }
            }
        }
    }
    return 0;
}

/// A failed creation restores the active tab, focus and zoom after the shared
/// mutation helper rolls back the pane it could not back with a session.
int failedCreationKeepsPresentation()
{
    for (const std::string method : { "workspace.addWindow", "workspace.addTab", "workspace.split" }) {
        Daemon daemon;
        auto& first = Screen::makeSession(daemon, 24, 80);
        auto& second = Screen::makeSession(daemon, 24, 80);
        auto& third = Screen::makeSession(daemon, 24, 80);
        Screen::nameWorkspace(daemon, first, "rollback");
        auto& model = Screen::layout(daemon, first);
        const auto window = model.activeSurface();
        const auto tab = model.activeTab(window);
        Screen::splitInto(daemon, first, second, Ubiquitty::Panes::Orientation::Vertical);
        (void)Screen::tabInto(daemon, first, third);
        PREMISE(model.setActiveTab(window, tab));
        const auto focused = model.focusedPane(window);
        PREMISE(model.toggleZoom(window, focused));
        auto& other = Screen::makeSession(daemon, 24, 80);
        (void)Screen::windowInto(daemon, first, other);
        PREMISE(model.setActiveSurface(window));
        const auto windowsBefore = model.surfaceCount();
        const std::string parameters = method == "workspace.addWindow"
            ? "{\"window\":" + std::to_string(std::to_underlying(window)) + ",\"pane\":" + std::to_string(Screen::layoutPane(daemon, first)) + "}"
            : method == "workspace.addTab"
            ? "{\"window\":" + std::to_string(std::to_underlying(window)) + "}"
            : "{\"pane\":" + std::to_string(Screen::layoutPane(daemon, first)) + ",\"orientation\":\"vertical\",\"ratio\":0.5}";
        Caller person(daemon, first.id);
        Screen::markTrustedLocal(daemon, person.index);
        Screen::bindWorkspace(daemon, person.index, first);
        Screen::greet(daemon, person.index, person.client);
        Caller agent(daemon, Sync::AdmissionTarget::Control);
        Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::NewSession });
        Screen::setTokenID(daemon, agent.index, "rollback requester");
        Screen::greet(daemon, agent.index, agent.client);
        Screen::exhaustSessionIDs(daemon);
        agent.send(method, parameters);
        PREMISE(agent.client.takeResults().empty());
        Screen::deliver(daemon, person.index, person.client);
        std::optional<Sync::StructureConsent> question;
        for (const auto& notify : person.client.takeNotifies())
            if (const auto asked = UbiquittyTest::structureConsentOf(notify))
                question = *asked;
        PREMISE(question);
        CHECK(isServed(person.call("consent.respond", structureDecision(question->requestID, Sync::ConsentChoice::AllowOnce))));
        Screen::deliver(daemon, agent.index, agent.client);
        const auto results = agent.client.takeResults();
        PREMISE(results.size() == 1);
        CHECK(!isServed(results.front()));
        CHECK(Screen::sessionCount(daemon) == 4);
        CHECK(model.surfaceCount() == windowsBefore && model.activeSurface() == window);
        CHECK(model.tabCount(window) == 2);
        CHECK(model.activeTab(window) == tab);
        CHECK(model.focusedPane(window) == focused);
        CHECK(model.zoomedPane(window) == focused);
        PREMISE(model.paneTree(window, tab));
        CHECK(model.paneTree(window, tab)->paneCount() == 2);
    }
    return 0;
}

/// A close can end the session both the requester and its approver are on.
/// Neither answer path may destroy that session before its Call has unwound.
int ownSessionClosesOnlyAfterBothAnswers()
{
    for (const std::string method : { "workspace.closePane", "workspace.closeTab", "window.close" }) {
        Daemon daemon;
        auto& session = Screen::makeSession(daemon, 24, 80);
        const std::uint32_t sessionID = session.id;
        Screen::nameWorkspace(daemon, session, "closing own session");
        const auto& model = Screen::layout(daemon, session);
        const auto window = model.activeSurface();
        const std::string parameters = method == "workspace.closePane"
            ? "{\"pane\":" + std::to_string(Screen::layoutPane(daemon, session)) + "}"
            : method == "workspace.closeTab"
            ? "{\"tab\":" + std::to_string(std::to_underlying(model.activeTab(window))) + "}"
            : "{\"window\":" + std::to_string(std::to_underlying(window)) + "}";
        Caller requester(daemon, sessionID);
        Screen::grantScope(daemon, requester.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
        Screen::setTokenID(daemon, requester.index, "closing requester");
        Screen::grantAgentSession(daemon, session, requester.index);
        Screen::greet(daemon, requester.index, requester.client);
        Caller approver(daemon, sessionID);
        Screen::markTrustedLocal(daemon, approver.index);
        Screen::bindWorkspace(daemon, approver.index, session);
        Screen::greet(daemon, approver.index, approver.client);
        requester.send(method, parameters);
        PREMISE(requester.client.takeResults().empty());
        Screen::deliver(daemon, approver.index, approver.client);
        std::optional<Sync::StructureConsent> question;
        for (const auto& notify : approver.client.takeNotifies())
            if (const auto asked = UbiquittyTest::structureConsentOf(notify))
                question = *asked;
        PREMISE(question && question->sessionID == sessionID);

        // Bypass only the harness's automatic settle, not the production
        // decoder, role gate, consent dispatcher or correlated result queues.
        const std::string answer = Ubiquitty::Protocol::Control::consentResponseRequest({ .requestID = question->requestID, .kind = Sync::ConsentKind::Structure, .decision = Sync::ConsentChoice::AllowOnce }, "closing:consent");
        PREMISE(approver.client.call(answer));
        const auto upstream = approver.client.takeStream(Screen::now());
        PREMISE(!upstream.empty());
        Screen::receive(daemon, approver.index, upstream);
        CHECK(Screen::hasSession(daemon, sessionID));
        Screen::serviceConnectionBeforeClosures(daemon, approver.index);
        CHECK(Screen::hasSession(daemon, sessionID));
        CHECK(requester.isAttached() && approver.isAttached());
        approver.client.read(Screen::takeOutput(daemon, approver.index), Screen::now());
        const auto answers = approver.client.takeResults();
        PREMISE(answers.size() == 1);
        CHECK(isServed(answers.front()));
        CHECK(Screen::hasSession(daemon, sessionID));
        Screen::serviceConnectionBeforeClosures(daemon, requester.index);
        requester.client.read(Screen::takeOutput(daemon, requester.index), Screen::now());
        const auto results = requester.client.takeResults();
        PREMISE(results.size() == 1);
        CHECK(isServed(results.front()) && has(results.front(), "\"sessionsEnded\":1"));
        CHECK(Screen::hasSession(daemon, sessionID));
        CHECK(requester.isAttached() && approver.isAttached());
        CHECK(Screen::pendingStructureConsents(daemon) == 0);

        Screen::settleClosures(daemon);
        CHECK(!Screen::hasSession(daemon, sessionID));
        CHECK(Screen::heldWorkspaces(daemon) == 0);
    }
    return 0;
}

/// A live attachment may move streams before its previous stream is reaped.
/// Neither side of a pending approval retains authority in that interval.
int resumedConnectionsCannotAuthorizeHeldOperations()
{
    enum class ResumingParty {
        Requester,
        Approver
    };
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control };
    for (const auto party : { ResumingParty::Requester, ResumingParty::Approver }) {
        Daemon daemon;
        auto& session = Screen::makeSession(daemon, 24, 80);
        const auto workspace = Screen::nameWorkspace(daemon, session, "held");
        Caller person(daemon, session.id);
        Screen::markTrustedLocal(daemon, person.index);
        Screen::bindWorkspace(daemon, person.index, session);
        Screen::greet(daemon, person.index, person.client);
        // The requester attaches to the session under a standing grant: an
        // unbound Control identity is transport-local and cannot resume at
        // all (AttachmentTable::createUnbound), checked below. A standing
        // grant answers no tree question, so the edit still waits.
        Caller agent(daemon, session.id);
        Screen::grantScope(daemon, agent.index, driving);
        Screen::setTokenID(daemon, agent.index, "requester");
        Screen::grantAgentSession(daemon, session, agent.index);
        Screen::greet(daemon, agent.index, agent.client);
        PREMISE(agent.isAttached() && Screen::connection(daemon, agent.index)->grantedRole() == Sync::Role::Agent);
        agent.send("label.set", "{\"object\":\"workspace:" + std::to_string(workspace) + "\",\"label\":\"must not apply\"}");
        PREMISE(Screen::pendingStructureConsents(daemon) == 1);
        if (party == ResumingParty::Requester) {
            Caller unbound(daemon, Sync::AdmissionTarget::Control);
            Screen::grantScope(daemon, unbound.index, driving);
            Screen::setTokenID(daemon, unbound.index, "unbound requester");
            Screen::greet(daemon, unbound.index, unbound.client);
            PREMISE(unbound.isAttached());
            CHECK(!Screen::resumeAttachmentElsewhere(daemon, unbound.index));
        }
        Screen::deliver(daemon, person.index, person.client);
        std::optional<std::uint32_t> request;
        for (const auto& text : person.client.takeNotifies()) {
            if (const auto question = UbiquittyTest::structureConsentOf(text))
                request = question->requestID;
        }
        PREMISE(request);
        const auto replacedIndex = party == ResumingParty::Requester ? agent.index : person.index;
        PREMISE(Screen::resumeAttachmentElsewhere(daemon, replacedIndex));
        CHECK(Screen::connection(daemon, replacedIndex)->state() == Sync::ConnectionState::Attached);
        const auto response = Screen::answerStructureWithoutServicing(daemon, person.index,
            Ubiquitty::Protocol::Control::consentResponseRequest({ .requestID = *request, .kind = Sync::ConsentKind::Structure, .decision = Sync::ConsentChoice::AllowOnce }, "resumed:consent"));
        CHECK(response.find("error"));
        CHECK(Screen::pendingStructureConsents(daemon) == 0);
        CHECK(Screen::label(daemon, { ObjectKind::Workspace, workspace }).empty());
    }
    return 0;
}

} // namespace

/// A tree change stays open exactly while some approver could still show it.
/// The only one asked declining it as unshown refuses it to its caller at once,
/// rather than at its deadline, and is not an answer the person gave.
int aTreeChangeNobodyCanShowIsRefusedAtOnce()
{
    Daemon daemon;
    auto& session = Screen::makeSession(daemon, 24, 80);
    const auto workspace = Screen::nameWorkspace(daemon, session, "unshown");
    Caller person(daemon, session.id);
    Screen::markTrustedLocal(daemon, person.index);
    Screen::bindWorkspace(daemon, person.index, session);
    Screen::greet(daemon, person.index, person.client);
    Caller agent(daemon, session.id);
    Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
    Screen::setTokenID(daemon, agent.index, "unshown requester");
    Screen::grantAgentSession(daemon, session, agent.index);
    Screen::greet(daemon, agent.index, agent.client);
    agent.send("label.set", "{\"object\":\"workspace:" + std::to_string(workspace) + "\",\"label\":\"must not apply\"}");
    PREMISE(Screen::pendingStructureConsents(daemon) == 1);
    Screen::deliver(daemon, person.index, person.client);
    std::optional<std::uint32_t> request;
    for (const auto& text : person.client.takeNotifies()) {
        if (const auto question = UbiquittyTest::structureConsentOf(text))
            request = question->requestID;
    }
    PREMISE(request);
    CHECK(isServed(person.call("consent.respond", "{\"request\":" + std::to_string(*request) + ",\"kind\":\"structure\",\"decision\":\"unshown\"}")));
    CHECK_MSG(Screen::pendingStructureConsents(daemon) == 0, "a tree change nobody could be shown waited for its deadline");
    Screen::deliver(daemon, agent.index, agent.client);
    const auto results = agent.client.takeResults();
    CHECK(results.size() == 1 && !results.empty() && !isServed(results.front()) && has(results.front(), "nobody attached could be shown"));
    CHECK(Screen::label(daemon, { ObjectKind::Workspace, workspace }).empty());
    return 0;
}

/// A tree change refused once everyone asked has declined it is settled for
/// each of them, those who declined first included, so no question about it
/// waits on any client.
int aTreeChangeRefusedAfterDeclinesIsSettledForEveryone()
{
    Daemon daemon;
    auto& session = Screen::makeSession(daemon, 24, 80);
    const auto workspace = Screen::nameWorkspace(daemon, session, "declined twice");
    Caller first(daemon, session.id);
    Screen::markTrustedLocal(daemon, first.index);
    Screen::bindWorkspace(daemon, first.index, session);
    Screen::greet(daemon, first.index, first.client);
    Caller second(daemon, session.id);
    Screen::markTrustedLocal(daemon, second.index);
    Screen::bindWorkspace(daemon, second.index, session);
    Screen::greet(daemon, second.index, second.client);
    Caller agent(daemon, session.id);
    Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
    Screen::setTokenID(daemon, agent.index, "declined requester");
    Screen::grantAgentSession(daemon, session, agent.index);
    Screen::greet(daemon, agent.index, agent.client);
    agent.send("label.set", "{\"object\":\"workspace:" + std::to_string(workspace) + "\",\"label\":\"never\"}");
    PREMISE(Screen::pendingStructureConsents(daemon) == 1);
    const auto requestOf = [&](Caller& person) {
        Screen::deliver(daemon, person.index, person.client);
        std::optional<std::uint32_t> request;
        for (const auto& text : person.client.takeNotifies()) {
            if (const auto question = UbiquittyTest::structureConsentOf(text))
                request = question->requestID;
        }
        return request;
    };
    const auto request = requestOf(first);
    PREMISE(request && requestOf(second) == request);
    const std::string decline = "{\"request\":" + std::to_string(*request) + ",\"kind\":\"structure\",\"decision\":\"unshown\"}";
    CHECK(isServed(first.call("consent.respond", decline)));
    PREMISE(Screen::pendingStructureConsents(daemon) == 1);
    CHECK(isServed(second.call("consent.respond", decline)));
    PREMISE(Screen::pendingStructureConsents(daemon) == 0);
    Screen::deliver(daemon, first.index, first.client);
    bool isSettled = false;
    for (const auto& text : first.client.takeNotifies()) {
        if (const auto resolved = UbiquittyTest::consentResolvedOf(text))
            isSettled = isSettled || (resolved->requestID == *request && resolved->kind == Sync::ConsentKind::Structure);
    }
    CHECK_MSG(isSettled, "a person who declined a tree change was never told it was settled");
    return 0;
}

/// A person attached through a client that shows no questions, as the editor
/// clients do, still rules on their session: they may revoke what agents
/// hold on it, though nobody asks them a question.
int aPersonShownNoQuestionsMayStillRevoke()
{
    Daemon daemon;
    auto& session = Screen::makeSession(daemon, 24, 80);
    (void)Screen::nameWorkspace(daemon, session, "editor person");
    Caller person(daemon, session.id);
    person.client.showsNoQuestions();
    Screen::markTrustedLocal(daemon, person.index);
    Screen::bindWorkspace(daemon, person.index, session);
    Screen::greet(daemon, person.index, person.client);
    PREMISE(person.isAttached());
    const std::string revoked = person.call("consent.revoke", "{\"session\":\"session:" + std::to_string(session.id) + "\",\"action\":\"disconnectAgents\"}");
    CHECK_MSG(isServed(revoked), revoked.c_str());
    return 0;
}

/// A tree change stays open for an approver whose link was lost, who may
/// resume and be asked again: neither that approver's own departure nor the
/// last other approver declining it refuses it at once.
int aTreeChangeWaitsForAnApproverWhoseLinkWasLost()
{
    Daemon daemon;
    auto& session = Screen::makeSession(daemon, 24, 80);
    const auto workspace = Screen::nameWorkspace(daemon, session, "lost approver");
    // Its own instance, so the later approvers' attachments do not stand for
    // it and its own stays to be resumed.
    Ubiquitty::HostTest::FullCarrier lostCarrier;
    const std::size_t lostIndex = Screen::attach(daemon, session.id, lostCarrier.takeDaemonEnd());
    UbiquittyTest::ScreenClient lostClient(24, 80, Sync::ID128 { .low = 0x1057, .high = 0x11 });
    Screen::markTrustedLocal(daemon, lostIndex);
    Screen::bindWorkspace(daemon, lostIndex, session);
    Screen::greet(daemon, lostIndex, lostClient);
    Caller other(daemon, session.id);
    Screen::markTrustedLocal(daemon, other.index);
    Screen::bindWorkspace(daemon, other.index, session);
    Screen::greet(daemon, other.index, other.client);
    Caller agent(daemon, session.id);
    Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
    Screen::setTokenID(daemon, agent.index, "lost-link requester");
    Screen::grantAgentSession(daemon, session, agent.index);
    Screen::greet(daemon, agent.index, agent.client);
    agent.send("label.set", "{\"object\":\"workspace:" + std::to_string(workspace) + "\",\"label\":\"waits\"}");
    PREMISE(Screen::pendingStructureConsents(daemon) == 1);
    const auto requestOf = [&](std::size_t index, UbiquittyTest::ScreenClient& client) {
        Screen::deliver(daemon, index, client);
        std::optional<std::uint32_t> request;
        for (const auto& text : client.takeNotifies()) {
            if (const auto question = UbiquittyTest::structureConsentOf(text))
                request = question->requestID;
        }
        return request;
    };
    PREMISE(requestOf(lostIndex, lostClient));
    const auto request = requestOf(other.index, other.client);
    PREMISE(request);
    const Sync::ID128 welcomeID = lostClient.connection().welcome() ? lostClient.connection().welcome()->attachmentID : Sync::ID128 {};
    Screen::drop(daemon, lostIndex);
    const auto* record = Screen::attachments(daemon).find(welcomeID);
    PREMISE(record && record->state == Sync::AttachmentState::Lost);
    CHECK_MSG(Screen::pendingStructureConsents(daemon) == 1, "a tree change was refused as its approver's link was lost");
    CHECK(isServed(other.call("consent.respond", "{\"request\":" + std::to_string(*request) + ",\"kind\":\"structure\",\"decision\":\"unshown\"}")));
    CHECK_MSG(Screen::pendingStructureConsents(daemon) == 1, "a tree change was refused while an approver whose link was lost could still be asked");
    Screen::deliver(daemon, agent.index, agent.client);
    CHECK_MSG(agent.client.takeResults().empty(), "the requester was answered before anyone ruled");
    CHECK(Screen::label(daemon, { ObjectKind::Workspace, workspace }).empty());
    return 0;
}

/// A newly added pane cannot be destroyed by an earlier window confirmation.
int windowCloseRequiresTheConfirmedRevision()
{
    Daemon daemon;
    auto& first = Screen::makeSession(daemon, 24, 80);
    const auto firstID = first.id;
    (void)Screen::nameWorkspace(daemon, first, "revision-bound close");
    const auto window = std::to_underlying(Screen::layout(daemon, first).activeSurface());
    Caller person(daemon, first.id);
    Screen::markTrustedLocal(daemon, person.index);
    Screen::bindWorkspace(daemon, person.index, first);
    Screen::greet(daemon, person.index, person.client);
    Caller agent(daemon, Sync::AdmissionTarget::Control);
    Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
    Screen::setTokenID(daemon, agent.index, "revision-bound requester");
    Screen::greet(daemon, agent.index, agent.client);
    const auto revision = Screen::windowStructureRevision(daemon, window);
    PREMISE(revision);
    auto& added = Screen::makeSession(daemon, 24, 80);
    const auto addedID = added.id;
    Screen::splitInto(daemon, first, added, Ubiquitty::Panes::Orientation::Vertical);
    const auto parameters = "{\"window\":" + std::to_string(window) + ",\"expectedRevision\":\"" + std::to_string(revision) + "\"}";
    for (Caller* caller : { &person, &agent }) {
        const auto answer = caller->call("window.close", parameters);
        CHECK_MSG(has(answer, "structure changed"), answer.c_str());
        CHECK(caller->isAttached());
        PREMISE(Screen::hasSession(daemon, firstID) && Screen::hasSession(daemon, addedID));
        CHECK(Screen::pendingStructureConsents(daemon) == 0);
    }
    for (const char* invalid : { "0", "01", "-1", "+1", "1x", "18446744073709551616" }) {
        const auto answer = person.call("window.close", "{\"window\":" + std::to_string(window) + ",\"expectedRevision\":\"" + invalid + "\"}");
        CHECK_MSG(has(answer, "expectedRevision"), answer.c_str());
        CHECK(!isServed(answer));
        CHECK(Screen::sessionCount(daemon) == 2);
    }
    Caller legacy(daemon, firstID);
    Screen::markTrustedLocal(daemon, legacy.index);
    Screen::bindWorkspace(daemon, legacy.index, first);
    Screen::greet(daemon, legacy.index, legacy.client);
    constexpr std::uint8_t undeclaredKind = 0x17;
    PREMISE(!Sync::frameKindOf(undeclaredKind));
    const std::vector<std::uint8_t> frame { undeclaredKind, 0, 0 };
    Screen::handleFrames(daemon, legacy.index, frame);
    Screen::serviceConnectionBeforeClosures(daemon, legacy.index);
    CHECK(Screen::connection(daemon, legacy.index)->state() == Sync::ConnectionState::Closed);
    CHECK(Screen::connection(daemon, legacy.index)->byeReason() == Sync::ByeReason::UnknownKind);
    Screen::settleClosures(daemon);
    CHECK(Screen::hasSession(daemon, firstID) && Screen::hasSession(daemon, addedID));
    CHECK(person.isAttached());
    const auto current = Screen::windowStructureRevision(daemon, window);
    PREMISE(current != revision);
    const auto closed = person.call("window.close", "{\"window\":" + std::to_string(window) + ",\"expectedRevision\":\"" + std::to_string(current) + "\"}");
    CHECK_MSG(isServed(closed) && has(closed, "\"sessionsEnded\":2"), closed.c_str());
    CHECK(!Screen::hasSession(daemon, firstID) && !Screen::hasSession(daemon, addedID));
    return 0;
}

/// Typed creation retains the explicitly selected source, and no retired
/// transport spelling can mutate the same tree through another admission path.
int nativeTreeCallsPreserveSourceAndRetireLegacyMutations()
{
    // Peers outlive the daemon, including its final transport flush.
    std::vector<Ubiquitty::PAL::UnixFileDescriptor> peers;
    Daemon daemon;
    Screen::setRestoreDefaults(daemon);
    auto& first = Screen::makeSession(daemon, 24, 80);
    auto& source = Screen::makeSession(daemon, 24, 80);
    auto& other = Screen::makeSession(daemon, 24, 80);
    (void)Screen::nameWorkspace(daemon, first, "typed native operations");
    Screen::splitInto(daemon, first, source, Ubiquitty::Panes::Orientation::Vertical);
    const auto window = Screen::layout(daemon, first).activeSurface();
    (void)Screen::windowInto(daemon, first, other);
    const auto pane = Screen::layoutPane(daemon, source);
    first.term->paletteBaseline.defaultBackground = { 0x1111, 0x2222, 0x3333 };
    source.term->paletteBaseline.defaultBackground = { 0x4444, 0x5555, 0x6666 };
    Caller person(daemon, first.id);
    Screen::markTrustedLocal(daemon, person.index);
    Screen::bindWorkspace(daemon, person.index, first);
    Screen::greet(daemon, person.index, person.client);
    std::vector<std::pair<std::vector<std::uint8_t>, Sync::ByeReason>> refusals;
    for (const std::uint8_t raw : { 0x17, 0x1C }) {
        PREMISE(!Sync::frameKindOf(raw));
        refusals.push_back({ { raw, 0, 0 }, Sync::ByeReason::UnknownKind });
    }
    const std::vector<std::uint8_t> truncatedCall { 0x80 };
    PREMISE(!Sync::decodeCall(truncatedCall));
    std::vector<std::uint8_t> malformed;
    PREMISE(Sync::appendFrame(malformed, Sync::FrameKind::Call, 0, truncatedCall));
    refusals.emplace_back(std::move(malformed), Sync::ByeReason::Malformed);
    for (const auto& [frame, reason] : refusals) {
        int descriptors[2] { -1, -1 };
        PREMISE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
        Ubiquitty::PAL::UnixFileDescriptor transport { descriptors[0] };
        peers.emplace_back(descriptors[1]);
        PREMISE(::fcntl(transport.get(), F_SETFL, O_NONBLOCK) == 0);
        PREMISE(::fcntl(peers.back().get(), F_SETFL, O_NONBLOCK) == 0);
        const auto retired = Screen::attach(daemon, first.id, std::move(transport));
        const auto retiredClientID = Screen::clientID(daemon, retired);
        Screen::markTrustedLocal(daemon, retired);
        Screen::bindWorkspace(daemon, retired, first);
        UbiquittyTest::ScreenClient client { 24, 80 };
        Screen::greet(daemon, retired, client);
        Screen::handleFrames(daemon, retired, frame);
        Screen::serviceConnectionBeforeClosures(daemon, retired);
        Screen::settleClosures(daemon);
        PREMISE(Screen::sessionCount(daemon) == 3);
        PREMISE(person.isAttached());
        Sync::FrameDecoder decoder;
        std::uint8_t bytes[4096];
        for (ssize_t count = ::read(peers.back().get(), bytes, sizeof(bytes)); count > 0;
            count = ::read(peers.back().get(), bytes, sizeof(bytes)))
            decoder.append(std::span { bytes, static_cast<std::size_t>(count) });
        bool sawRefusal = false;
        while (const auto decoded = decoder.next()) {
            CHECK(decoded->kind == Sync::FrameKind::Bye && decoded->channel == 0);
            if (decoded->kind != Sync::FrameKind::Bye)
                continue;
            const auto answer = Sync::decodeBye(decoded->body);
            CHECK(answer && answer->reason == reason);
            CHECK(!sawRefusal);
            sawRefusal = true;
        }
        CHECK(decoder.error() == Sync::EnvelopeError::None && !decoder.bufferedBytes());
        CHECK(sawRefusal);
        CHECK(Screen::label(daemon, { ObjectKind::Window, std::to_underlying(window) }).empty());
        CHECK(Screen::label(daemon, { ObjectKind::Pane, pane }).empty());
        CHECK(Screen::label(daemon, { ObjectKind::Session, source.id }).empty());
        CHECK(Screen::label(daemon, { ObjectKind::Client, retiredClientID }).empty());
    }
    const auto parameters = "{\"window\":" + std::to_string(std::to_underlying(window)) + ",\"pane\":";
    const auto foreign = person.call("workspace.addTab", parameters + std::to_string(Screen::layoutPane(daemon, other)) + "}");
    CHECK(!isServed(foreign) && has(foreign, "source pane"));
    CHECK(Screen::sessionCount(daemon) == 3);
    const auto opened = person.call("workspace.addTab", parameters + std::to_string(pane) + "}");
    PREMISE(isServed(opened));
    bool isJSON = false; // bool-ok: the parser's output flag
    const auto answer = Ubiquitty::Codec::JSON::parse(opened, isJSON);
    PREMISE(isJSON && answer.find("result"));
    const auto session = Ubiquitty::Session::Identity::parseObjectID(answer.find("result")->stringOr("session"));
    PREMISE(session && session->kind == ObjectKind::Session && session->number <= UINT32_MAX);
    const auto* created = Screen::sessionTerminal(daemon, static_cast<std::uint32_t>(session->number));
    PREMISE(created);
    CHECK(created->paletteBaseline.defaultBackground.red == 0x4444);
    CHECK(created->paletteBaseline.defaultBackground.green == 0x5555);
    CHECK(created->paletteBaseline.defaultBackground.blue == 0x6666);
    CHECK(Screen::sessionCount(daemon) == 4);
    return 0;
}

/// A structure revises for what it says of a pane, its cells, as well as for
/// how its panes are arranged. A layout operation asked about the
/// arrangement that still stands is served under its older revision, at once
/// and after an approval; a change to the arrangement still refuses it, and
/// closing still needs the exact revision.
int aLayoutOperationOutlivesAPaneResize()
{
    Daemon daemon;
    auto& first = Screen::makeSession(daemon, 24, 156);
    auto& second = Screen::makeSession(daemon, 24, 156);
    Screen::nameWorkspace(daemon, first, "resizing");
    Screen::splitInto(daemon, first, second, Ubiquitty::Panes::Orientation::Vertical);
    auto& model = Screen::layout(daemon, first);
    const auto window = model.activeSurface();
    const auto firstPane = Screen::layoutPane(daemon, first);
    Caller person(daemon, first.id);
    Screen::markTrustedLocal(daemon, person.index);
    Screen::bindWorkspace(daemon, person.index, first);
    Screen::greet(daemon, person.index, person.client);

    const auto asked = Screen::windowStructureRevision(daemon, std::to_underlying(window));
    PREMISE(asked);
    // The pane the zoom would fill resizes, as a zoom resizes it, before the
    // asker hears the structure that says so.
    Ubiquitty::resize(*first.term, 30, 120);
    const auto resized = Screen::windowStructureRevision(daemon, std::to_underlying(window));
    PREMISE(resized > asked);
    const auto zoomAsked = [&](const char* state, std::uint64_t revision) {
        return "{\"pane\":" + std::to_string(firstPane) + ",\"state\":\"" + state + "\",\"expectedRevision\":\"" + std::to_string(revision) + "\"}";
    };
    const auto zoomAnswer = person.call("pane.zoom", zoomAsked("zoomed", asked));
    CHECK_MSG(isServed(zoomAnswer), zoomAnswer.c_str());
    CHECK(std::to_underlying(model.zoomedPane(window)) == firstPane);
    // The zoom changed the arrangement: what was asked before it is refused.
    CHECK(has(person.call("pane.zoom", zoomAsked("unzoomed", asked)), "structure changed"));
    CHECK(std::to_underlying(model.zoomedPane(window)) == firstPane);
    const auto zoomed = Screen::windowStructureRevision(daemon, std::to_underlying(window));
    Ubiquitty::resize(*first.term, 24, 156);
    PREMISE(Screen::windowStructureRevision(daemon, std::to_underlying(window)) > zoomed);
    CHECK(isServed(person.call("pane.zoom", zoomAsked("unzoomed", zoomed))));
    CHECK(model.zoomedPane(window) == Layout::invalidPane);

    // Closing ends the sessions the asker counted, which a resize does not
    // move but the arrangement does not say: it needs the exact revision.
    const auto beforeClose = Screen::windowStructureRevision(daemon, std::to_underlying(window));
    Ubiquitty::resize(*second.term, 30, 120);
    PREMISE(Screen::windowStructureRevision(daemon, std::to_underlying(window)) > beforeClose);
    CHECK(has(person.call("window.close", "{\"window\":" + std::to_string(std::to_underlying(window)) + ",\"expectedRevision\":\"" + std::to_string(beforeClose) + "\"}"), "structure changed"));
    CHECK(Screen::hasSession(daemon, first.id) && Screen::hasSession(daemon, second.id));
    return 0;
}

/// Navigation derived from an observed tree cannot act on a newer tree or
/// on the focused pane of an unrelated window.
int navigationRequiresTheObservedState()
{
    Daemon daemon;
    auto& first = Screen::makeSession(daemon, 24, 156);
    auto& second = Screen::makeSession(daemon, 24, 156);
    auto& other = Screen::makeSession(daemon, 24, 156);
    Screen::nameWorkspace(daemon, first, "navigation");
    Screen::splitInto(daemon, first, second, Ubiquitty::Panes::Orientation::Vertical);
    auto& model = Screen::layout(daemon, first);
    const auto window = model.activeSurface();
    const auto otherWindow = Screen::windowInto(daemon, first, other);
    const auto firstPane = Screen::layoutPane(daemon, first);
    const auto secondPane = Screen::layoutPane(daemon, second);
    Caller person(daemon, first.id);
    Screen::markTrustedLocal(daemon, person.index);
    Screen::bindWorkspace(daemon, person.index, first);
    Screen::greet(daemon, person.index, person.client);
    Caller agent(daemon, Sync::AdmissionTarget::Control);
    Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
    Screen::setTokenID(daemon, agent.index, "navigation requester");
    Screen::greet(daemon, agent.index, agent.client);
    const auto revision = Screen::windowStructureRevision(daemon, std::to_underlying(window));
    PREMISE(revision);
    const auto zoom = "{\"pane\":" + std::to_string(firstPane) + ",\"state\":\"zoomed\",\"expectedRevision\":\"" + std::to_string(revision) + "\"}";
    CHECK(isServed(person.call("pane.zoom", zoom)));
    CHECK(std::to_underlying(model.zoomedPane(window)) == firstPane);
    for (Caller* caller : { &person, &agent }) {
        CHECK(has(caller->call("pane.zoom", zoom), "structure changed"));
        CHECK(std::to_underlying(model.zoomedPane(window)) == firstPane);
        CHECK(Screen::pendingStructureConsents(daemon) == 0);
    }
    const auto unzoom = "{\"pane\":" + std::to_string(firstPane) + ",\"state\":\"unzoomed\"}";
    CHECK(isServed(person.call("pane.zoom", unzoom)));
    CHECK(isServed(person.call("pane.zoom", unzoom)));
    CHECK(model.zoomedPane(window) == Layout::invalidPane);
    const auto move = "{\"window\":" + std::to_string(std::to_underlying(window)) + ",\"pane\":" + std::to_string(secondPane) + ",\"direction\":\"left\",\"expectedRevision\":\"" + std::to_string(Screen::windowStructureRevision(daemon, std::to_underlying(window))) + "\"}";
    CHECK(isServed(person.call("window.moveFocus", move)));
    CHECK(std::to_underlying(model.focusedPane(window)) == firstPane);
    CHECK(has(person.call("window.moveFocus", move), "structure changed"));
    CHECK(std::to_underlying(model.focusedPane(window)) == firstPane);
    for (Caller* caller : { &person, &agent }) {
        CHECK(has(caller->call("window.moveFocus", "{\"window\":" + std::to_string(std::to_underlying(otherWindow)) + ",\"pane\":" + std::to_string(firstPane) + ",\"direction\":\"right\"}"), "observed pane"));
        CHECK(Screen::pendingStructureConsents(daemon) == 0);
        CHECK(caller->isAttached());
    }
    for (const char* invalid : { "0", "01", "-1", "+1", "18446744073709551616" }) {
        const auto answer = person.call("pane.zoom", "{\"pane\":" + std::to_string(firstPane) + ",\"state\":\"zoomed\",\"expectedRevision\":\"" + invalid + "\"}");
        CHECK(has(answer, "expectedRevision"));
        CHECK(model.zoomedPane(window) == Layout::invalidPane);
    }
    auto& foreignSession = Screen::makeSession(daemon, 24, 156);
    Screen::nameWorkspace(daemon, foreignSession, "separate authority");
    Caller foreign(daemon, foreignSession.id);
    Screen::grantScope(daemon, foreign.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
    Screen::setTokenID(daemon, foreign.index, "foreign driver");
    Screen::markInteractiveUser(daemon, foreign.index);
    Screen::bindWorkspace(daemon, foreign.index, foreignSession);
    Screen::greet(daemon, foreign.index, foreign.client);
    const auto before = Screen::windowStructureRevision(daemon, std::to_underlying(window));
    for (const auto& [method, parameters] : std::vector<std::pair<std::string, std::string>> {
             { "window.activate", "{\"window\":" + std::to_string(std::to_underlying(window)) + "}" },
             { "tab.activate", "{\"tab\":" + std::to_string(std::to_underlying(model.activeTab(window))) + "}" },
             { "pane.focus", "{\"pane\":" + std::to_string(firstPane) + "}" },
             { "window.moveFocus", "{\"window\":" + std::to_string(std::to_underlying(window)) + ",\"pane\":" + std::to_string(firstPane) + ",\"direction\":\"right\"}" },
             { "pane.zoom", "{\"pane\":" + std::to_string(firstPane) + ",\"state\":\"zoomed\"}" } }) {
        CHECK(!isServed(foreign.call(method, parameters)));
        CHECK(foreign.isAttached());
        CHECK(Screen::windowStructureRevision(daemon, std::to_underlying(window)) == before);
        CHECK(Screen::pendingStructureConsents(daemon) == 0);
    }
    CHECK(Screen::sessionCount(daemon) == 4);
    return 0;
}

int paneRelocationsPreserveIdentityAndRejectUnseenPairs()
{
    for (const std::string method : { "pane.move", "pane.swap" }) {
        Daemon daemon;
        auto& first = Screen::makeSession(daemon, 24, 156);
        auto& second = Screen::makeSession(daemon, 24, 156);
        auto& other = Screen::makeSession(daemon, 24, 156);
        auto& hidden = Screen::makeSession(daemon, 24, 156);
        auto& foreign = Screen::makeSession(daemon, 24, 156);
        Screen::nameWorkspace(daemon, first, "pane pair");
        Screen::nameWorkspace(daemon, foreign, "foreign pair");
        Screen::splitInto(daemon, first, second, Ubiquitty::Panes::Orientation::Vertical);
        auto& model = Screen::layout(daemon, first);
        const auto window = model.activeSurface();
        const auto tab = model.activeTab(window);
        const auto otherWindow = Screen::windowInto(daemon, first, other);
        PREMISE(model.setActiveSurface(window));
        (void)Screen::tabInto(daemon, first, hidden);
        PREMISE(model.setActiveTab(window, tab));
        PREMISE(model.surfaceHolding(Layout::PaneID { Screen::layoutPane(daemon, hidden) }) == window);
        PREMISE(model.tabHolding(window, Layout::PaneID { Screen::layoutPane(daemon, hidden) }) != tab);
        const auto firstPane = Screen::layoutPane(daemon, first);
        const auto secondPane = Screen::layoutPane(daemon, second);
        const auto order = [&] {
            std::vector<int> panes;
            Ubiquitty::Panes::forEachLeaf(*model.paneTree(window, tab), [&](int pane) { panes.push_back(pane); });
            return panes;
        };
        Caller person(daemon, first.id);
        Screen::markTrustedLocal(daemon, person.index);
        Screen::bindWorkspace(daemon, person.index, first);
        Screen::greet(daemon, person.index, person.client);
        Caller agent(daemon, Sync::AdmissionTarget::Control);
        Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
        Screen::setTokenID(daemon, agent.index, "pane relocation requester");
        Screen::greet(daemon, agent.index, agent.client);
        const auto parameters = [&](std::uint32_t target, std::uint32_t sourceWindow, std::uint64_t revision) {
            return "{\"window\":" + std::to_string(sourceWindow) + ",\"pane\":" + std::to_string(secondPane)
                + ",\"target\":" + std::to_string(target) + ",\"expectedRevision\":\"" + std::to_string(revision) + "\""
                + (method == "pane.move" ? ",\"orientation\":\"horizontal\",\"side\":\"before\"" : "") + "}";
        };
        const auto revision = Screen::windowStructureRevision(daemon, std::to_underlying(window));
        const auto before = order();
        for (Caller* caller : { &person, &agent }) {
            for (const auto target : { secondPane, Screen::layoutPane(daemon, other), Screen::layoutPane(daemon, hidden), Screen::layoutPane(daemon, foreign), 99999u }) {
                CHECK(!isServed(caller->call(method, parameters(target, std::to_underlying(window), revision))));
                CHECK(Screen::pendingStructureConsents(daemon) == 0);
                CHECK(order() == before);
                CHECK(Screen::windowStructureRevision(daemon, std::to_underlying(window)) == revision);
            }
            CHECK(!isServed(caller->call(method, parameters(firstPane, std::to_underlying(otherWindow), revision))));
            CHECK(Screen::pendingStructureConsents(daemon) == 0);
        }
        const auto valid = parameters(firstPane, std::to_underlying(window), revision);
        const auto accepted = person.call(method, valid);
        CHECK_MSG(isServed(accepted), accepted.c_str());
        const std::vector<int> expected { static_cast<int>(secondPane), static_cast<int>(firstPane) };
        CHECK(order() == expected);
        if (method == "pane.move") {
            const auto* tree = model.paneTree(window, tab);
            CHECK(tree->node(tree->root()).orientation == Ubiquitty::Panes::Orientation::Horizontal);
        }
        for (Caller* caller : { &person, &agent }) {
            CHECK(has(caller->call(method, valid), "structure changed"));
            CHECK(order() == expected);
            CHECK(Screen::pendingStructureConsents(daemon) == 0);
            CHECK(caller->isAttached());
        }
        CHECK(Screen::sessionCount(daemon) == 5);
        CHECK(Screen::layoutPane(daemon, first) == firstPane && Screen::layoutPane(daemon, second) == secondPane);
        Caller outsider(daemon, foreign.id);
        Screen::grantScope(daemon, outsider.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
        Screen::setTokenID(daemon, outsider.index, "another workspace driver");
        Screen::markInteractiveUser(daemon, outsider.index);
        Screen::bindWorkspace(daemon, outsider.index, foreign);
        Screen::greet(daemon, outsider.index, outsider.client);
        const auto currentRevision = Screen::windowStructureRevision(daemon, std::to_underlying(window));
        CHECK(!isServed(outsider.call(method, parameters(firstPane, std::to_underlying(window), currentRevision))));
        CHECK(outsider.isAttached());
        CHECK(order() == expected);
        CHECK(Screen::windowStructureRevision(daemon, std::to_underlying(window)) == currentRevision);
    }
    return 0;
}

int extractionsPreserveInactiveSourcesAndRejectStaleAuthority()
{
    for (const std::string method : { "pane.extractTab", "pane.extractWindow", "tab.extractWindow" }) {
        Daemon daemon;
        auto& first = Screen::makeSession(daemon, 24, 156);
        auto& second = Screen::makeSession(daemon, 24, 156);
        auto& hidden = Screen::makeSession(daemon, 24, 156);
        auto& other = Screen::makeSession(daemon, 24, 156);
        auto& foreign = Screen::makeSession(daemon, 24, 156);
        Screen::nameWorkspace(daemon, first, "extraction");
        Screen::nameWorkspace(daemon, foreign, "foreign extraction");
        Screen::splitInto(daemon, first, second, Ubiquitty::Panes::Orientation::Vertical);
        auto& model = Screen::layout(daemon, first);
        const auto window = model.activeSurface();
        const auto sourceTab = model.activeTab(window);
        const auto activeTab = Screen::tabInto(daemon, first, hidden);
        const auto otherWindow = Screen::windowInto(daemon, first, other);
        PREMISE(model.activeTab(window) == activeTab && activeTab != sourceTab);
        const auto pane = Screen::layoutPane(daemon, second);
        const auto firstPane = Screen::layoutPane(daemon, first);
        Caller person(daemon, first.id);
        Screen::markInteractiveUser(daemon, person.index);
        Screen::setTokenID(daemon, person.index, "extraction driver");
        Screen::grantScope(daemon, person.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
        Screen::bindWorkspace(daemon, person.index, first);
        Screen::greet(daemon, person.index, person.client);
        Caller agent(daemon, Sync::AdmissionTarget::Control);
        Screen::setTokenID(daemon, agent.index, "extraction requester");
        Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
        Screen::greet(daemon, agent.index, agent.client);
        const bool isTab = method == "tab.extractWindow";
        const auto target = isTab ? std::to_underlying(sourceTab) : pane;
        const auto parameters = [&](std::uint32_t sourceWindow, std::uint32_t source, std::string_view revision) {
            return "{\"window\":" + std::to_string(sourceWindow) + (isTab ? ",\"tab\":" : ",\"pane\":")
                + std::to_string(source) + ",\"expectedRevision\":\"" + std::string(revision) + "\"}";
        };
        const auto revision = Screen::windowStructureRevision(daemon, std::to_underlying(window));
        const auto original = parameters(std::to_underlying(window), target, std::to_string(revision));
        for (Caller* caller : { &person, &agent }) {
            CHECK(!isServed(caller->call(method, parameters(std::to_underlying(otherWindow), target, std::to_string(revision)))));
            CHECK(!isServed(caller->call(method, parameters(std::to_underlying(window), 99999, std::to_string(revision)))));
            for (const char* invalid : { "0", "01", "18446744073709551616" })
                CHECK(!isServed(caller->call(method, parameters(std::to_underlying(window), target, invalid))));
            CHECK(Screen::pendingStructureConsents(daemon) == 0);
            CHECK(Screen::windowStructureRevision(daemon, std::to_underlying(window)) == revision);
            CHECK(model.tabHolding(window, Layout::PaneID { pane }) == sourceTab);
            CHECK(model.activeTab(window) == activeTab);
        }
        Caller outsider(daemon, foreign.id);
        Screen::markInteractiveUser(daemon, outsider.index);
        Screen::setTokenID(daemon, outsider.index, "foreign extraction driver");
        Screen::grantScope(daemon, outsider.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
        Screen::bindWorkspace(daemon, outsider.index, foreign);
        Screen::greet(daemon, outsider.index, outsider.client);
        CHECK(!isServed(outsider.call(method, original)));
        CHECK(Screen::windowStructureRevision(daemon, std::to_underlying(window)) == revision);
        const auto response = person.call(method, original);
        CHECK_MSG(isServed(response), response.c_str());
        bool isJSON = false;
        const auto decoded = Ubiquitty::Codec::JSON::parse(response, isJSON);
        const auto* result = decoded.find("result");
        PREMISE(isJSON && result);
        const auto destinationWindow = model.surfaceHolding(Layout::PaneID { pane });
        const auto destinationTab = model.tabHolding(destinationWindow, Layout::PaneID { pane });
        CHECK(result->stringOr("sourceWindow") == "window:" + std::to_string(std::to_underlying(window)));
        CHECK(result->stringOr("window") == "window:" + std::to_string(std::to_underlying(destinationWindow)));
        CHECK(result->stringOr("tab") == "tab:" + std::to_string(std::to_underlying(destinationTab)));
        CHECK((destinationWindow == window) == (method == "pane.extractTab"));
        CHECK((destinationTab == sourceTab) == isTab);
        if (!isTab) {
            CHECK(result->stringOr("sourceTab") == "tab:" + std::to_string(std::to_underlying(sourceTab)));
            CHECK(result->stringOr("pane") == "pane:" + std::to_string(pane));
            CHECK(model.surfaceHolding(Layout::PaneID { firstPane }) == window);
        } else
            CHECK(model.surfaceHolding(Layout::PaneID { firstPane }) == destinationWindow);
        for (Caller* caller : { &person, &agent }) {
            CHECK(!isServed(caller->call(method, original)));
            CHECK(Screen::pendingStructureConsents(daemon) == 0);
            CHECK(caller->isAttached());
        }
        CHECK(Screen::sessionCount(daemon) == 5);
        CHECK(Screen::layoutPane(daemon, first) == firstPane && Screen::layoutPane(daemon, second) == pane);
        CHECK(model.surfaceHolding(Layout::PaneID { pane }) == destinationWindow);
        CHECK(model.tabHolding(destinationWindow, Layout::PaneID { pane }) == destinationTab);
    }
    return 0;
}

int transfersPreserveLayoutsAndRejectForeignDestinations()
{
    using namespace Ubiquitty::Panes;
    for (const std::string method : { "pane.moveToTab", "tab.moveToPane", "pane.moveToNewTab" }) {
        for (const auto insertTarget : { InsertTarget::Pane, InsertTarget::Root }) {
            Daemon daemon;
            auto& first = Screen::makeSession(daemon, 24, 156);
            auto& second = Screen::makeSession(daemon, 24, 156);
            auto& hidden = Screen::makeSession(daemon, 24, 156);
            auto& other = Screen::makeSession(daemon, 24, 156);
            auto& sibling = Screen::makeSession(daemon, 24, 156);
            auto& foreign = Screen::makeSession(daemon, 24, 156);
            Screen::nameWorkspace(daemon, first, "transfer");
            Screen::nameWorkspace(daemon, foreign, "foreign transfer");
            Screen::splitInto(daemon, first, second, Orientation::Vertical);
            auto& model = Screen::layout(daemon, first);
            const auto sourceWindow = model.activeSurface();
            const auto sourceTab = model.activeTab(sourceWindow);
            const auto hiddenTab = Screen::tabInto(daemon, first, hidden);
            const auto destinationWindow = Screen::windowInto(daemon, first, other);
            Screen::splitInto(daemon, other, sibling, Orientation::Vertical);
            const auto destinationTab = model.activeTab(destinationWindow);
            const auto pane = Screen::layoutPane(daemon, second);
            const auto target = Screen::layoutPane(daemon, other);
            const auto sourceTree = *model.paneTree(sourceWindow, sourceTab);
            const auto destinationTree = *model.paneTree(destinationWindow, destinationTab);
            PREMISE(model.activeTab(sourceWindow) == hiddenTab && hiddenTab != sourceTab);
            Caller person(daemon, first.id);
            Screen::markInteractiveUser(daemon, person.index);
            Screen::setTokenID(daemon, person.index, "transfer driver");
            Screen::grantScope(daemon, person.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
            Screen::bindWorkspace(daemon, person.index, first);
            Screen::greet(daemon, person.index, person.client);
            Caller agent(daemon, Sync::AdmissionTarget::Control);
            Screen::setTokenID(daemon, agent.index, "transfer requester");
            Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
            Screen::greet(daemon, agent.index, agent.client);
            const bool isTab = method == "tab.moveToPane";
            const bool isNewTab = method == "pane.moveToNewTab";
            const auto revision = Screen::windowStructureRevision(daemon, std::to_underlying(sourceWindow));
            const auto parameters = [&](std::uint32_t fromWindow, std::uint32_t toPane, std::uint32_t toWindow, std::uint32_t toTab, std::uint64_t observedRevision) {
                std::string text = "{\"window\":" + std::to_string(fromWindow)
                    + (isTab ? ",\"tab\":" + std::to_string(std::to_underlying(sourceTab)) : ",\"pane\":" + std::to_string(pane))
                    + ",\"expectedRevision\":\"" + std::to_string(observedRevision) + "\"";
                if (isNewTab)
                    text += ",\"targetWindow\":" + std::to_string(toWindow);
                else {
                    if (!isTab)
                        text += ",\"tab\":" + std::to_string(toTab);
                    text += ",\"target\":" + std::to_string(toPane) + ",\"orientation\":\"horizontal\",\"side\":\"after\",\"insertTarget\":\""
                        + (insertTarget == InsertTarget::Root ? "root" : "pane") + "\"";
                }
                return text + "}";
            };
            const auto source = std::to_underlying(sourceWindow);
            const auto destination = std::to_underlying(destinationWindow);
            const auto tab = std::to_underlying(destinationTab);
            const auto valid = parameters(source, target, destination, tab, revision);
            const auto foreignWindow = std::to_underlying(Screen::layout(daemon, foreign).activeSurface());
            for (Caller* caller : { &person, &agent }) {
                CHECK(!isServed(caller->call(method, parameters(destination, target, destination, tab, revision))));
                CHECK(!isServed(caller->call(method, parameters(source, 99999, 99999, tab, revision))));
                CHECK(!isServed(caller->call(method, parameters(source, Screen::layoutPane(daemon, foreign), foreignWindow, tab, revision))));
                if (!isNewTab)
                    CHECK(!isServed(caller->call(method, parameters(source, pane, source, std::to_underlying(sourceTab), revision))));
                if (method == "pane.moveToTab")
                    CHECK(!isServed(caller->call(method, parameters(source, target, destination, std::to_underlying(sourceTab), revision))));
                CHECK(Screen::pendingStructureConsents(daemon) == 0);
                CHECK(Screen::windowStructureRevision(daemon, source) == revision);
                CHECK(sameShape(sourceTree, *model.paneTree(sourceWindow, sourceTab)));
                CHECK(sameShape(destinationTree, *model.paneTree(destinationWindow, destinationTab)));
            }
            const auto response = person.call(method, valid);
            CHECK_MSG(isServed(response), response.c_str());
            bool isJSON = false;
            const auto decoded = Ubiquitty::Codec::JSON::parse(response, isJSON);
            const auto* result = decoded.find("result");
            PREMISE(isJSON && result);
            const auto resultingTab = model.tabHolding(destinationWindow, Layout::PaneID { pane });
            CHECK(result->stringOr("sourceWindow") == "window:" + std::to_string(source));
            CHECK(result->stringOr("sourceTab") == "tab:" + std::to_string(std::to_underlying(sourceTab)));
            CHECK(result->stringOr("window") == "window:" + std::to_string(destination));
            CHECK(result->stringOr("tab") == "tab:" + std::to_string(std::to_underlying(resultingTab)));
            CHECK(model.surfaceHolding(Layout::PaneID { pane }) == destinationWindow);
            CHECK((resultingTab == destinationTab) == !isNewTab);
            if (!isTab)
                CHECK(result->stringOr("pane") == "pane:" + std::to_string(pane));
            if (isNewTab) {
                CHECK(sameShape(destinationTree, *model.paneTree(destinationWindow, destinationTab)));
                CHECK(model.paneTree(destinationWindow, resultingTab)->paneCount() == 1);
            } else {
                auto expected = destinationTree;
                if (isTab)
                    CHECK(expected.insertTree(sourceTree, static_cast<int>(target), Orientation::Horizontal, InsertSide::Second, insertTarget));
                else
                    CHECK(expected.insertPane(static_cast<int>(pane), static_cast<int>(target), Orientation::Horizontal, defaultRatio, InsertSide::Second, insertTarget) != invalidNode);
                CHECK(sameShape(expected, *model.paneTree(destinationWindow, destinationTab)));
                CHECK(result->stringOr("target") == "pane:" + std::to_string(target));
                CHECK(result->stringOr("insertTarget") == (insertTarget == InsertTarget::Root ? "root" : "pane"));
            }
            for (Caller* caller : { &person, &agent }) {
                CHECK(!isServed(caller->call(method, valid)));
                CHECK(Screen::pendingStructureConsents(daemon) == 0);
                CHECK(caller->isAttached());
            }
            CHECK(Screen::sessionCount(daemon) == 6);
            CHECK(Screen::layoutPane(daemon, second) == pane && Screen::layoutPane(daemon, other) == target);
            if (isTab)
                CHECK(!model.paneTree(sourceWindow, sourceTab));
            else
                CHECK(model.paneTree(sourceWindow, sourceTab)->paneCount() == 1);
            // Once isolated in its own tab, a same-window new-tab request
            // reuses that tab; it does not create another identity or session.
            if (isNewTab) {
                const auto count = model.tabCount(destinationWindow);
                const auto current = Screen::windowStructureRevision(daemon, destination);
                const auto repeated = person.call(method, parameters(destination, target, destination, tab, current));
                CHECK_MSG(isServed(repeated), repeated.c_str());
                CHECK(model.tabCount(destinationWindow) == count && model.tabHolding(destinationWindow, Layout::PaneID { pane }) == resultingTab);
            } else if (method == "pane.moveToTab") {
                const auto current = Screen::windowStructureRevision(daemon, destination);
                auto expected = *model.paneTree(destinationWindow, destinationTab);
                CHECK(expected.movePane(static_cast<int>(pane), static_cast<int>(target), Orientation::Horizontal, InsertSide::Second, insertTarget));
                const auto repeated = person.call(method, parameters(destination, target, destination, tab, current));
                CHECK_MSG(isServed(repeated), repeated.c_str());
                const auto returned = Ubiquitty::Codec::JSON::parse(repeated, isJSON);
                PREMISE(isJSON && returned.find("result"));
                CHECK(returned.find("result")->stringOr("sourceWindow") == returned.find("result")->stringOr("window"));
                CHECK(returned.find("result")->stringOr("sourceTab") == returned.find("result")->stringOr("tab"));
                CHECK(sameShape(expected, *model.paneTree(destinationWindow, destinationTab)));
                CHECK(Screen::sessionCount(daemon) == 6);
            }
        }
    }
    return 0;
}

/// Creating a window uses the observed source, never a foreign or focused
/// substitute, and returns every minted identity without moving that source.
int windowCreationRequiresTheObservedSource()
{
    Daemon daemon;
    Screen::setRestoreDefaults(daemon);
    auto& source = Screen::makeSession(daemon, 24, 80);
    auto& shown = Screen::makeSession(daemon, 24, 80);
    auto& foreign = Screen::makeSession(daemon, 24, 80);
    Screen::nameWorkspace(daemon, source, "window source");
    Screen::nameWorkspace(daemon, foreign, "foreign");
    auto& model = Screen::layout(daemon, source);
    const auto window = model.activeSurface();
    const auto sourceTab = model.activeTab(window);
    const auto shownTab = Screen::tabInto(daemon, source, shown);
    const auto sourcePane = Screen::layoutPane(daemon, source);
    source.term->paletteBaseline.defaultBackground.red = 0x1234;
    source.term->paletteBaseline.defaultBackground.green = 0x5678;
    source.term->paletteBaseline.defaultBackground.blue = 0x9abc;
    const auto sourceFocus = model.focusedPane(window);
    Caller person(daemon, source.id);
    Screen::markTrustedLocal(daemon, person.index);
    Screen::bindWorkspace(daemon, person.index, source);
    Screen::greet(daemon, person.index, person.client);
    const auto revision = Screen::windowStructureRevision(daemon, std::to_underlying(window));
    const auto parameters = [&](std::uint32_t pane, std::uint32_t inWindow, std::string_view observed) {
        return "{\"window\":" + std::to_string(inWindow) + ",\"pane\":" + std::to_string(pane)
            + ",\"expectedRevision\":\"" + std::string(observed) + "\"}";
    };
    const auto current = std::to_string(revision);
    for (const auto& invalid : {
             parameters(sourcePane, 999999, current),
             parameters(999999, std::to_underlying(window), current),
             parameters(Screen::layoutPane(daemon, foreign), std::to_underlying(window), current),
             parameters(sourcePane, std::to_underlying(window), std::to_string(revision + 1)),
             parameters(sourcePane, std::to_underlying(window), "01") }) {
        CHECK(!isServed(person.call("workspace.addWindow", invalid)));
        CHECK(Screen::sessionCount(daemon) == 3 && model.surfaceCount() == 1);
        CHECK(model.activeSurface() == window && model.activeTab(window) == shownTab && model.focusedPane(window) == sourceFocus);
        CHECK(model.tabHolding(window, Layout::PaneID { sourcePane }) == sourceTab);
    }
    const auto request = parameters(sourcePane, std::to_underlying(window), current);
    const auto response = person.call("workspace.addWindow", request);
    PREMISE(isServed(response));
    bool isJSON = false; // bool-ok: parser output
    const auto answer = Ubiquitty::Codec::JSON::parse(response, isJSON);
    PREMISE(isJSON && answer.find("result"));
    const auto& result = *answer.find("result");
    const auto identifier = [&](const char* name, ObjectKind kind) -> std::uint32_t {
        const auto parsed = Ubiquitty::Session::Identity::parseObjectID(result.stringOr(name));
        return parsed && parsed->kind == kind && parsed->number <= UINT32_MAX ? static_cast<std::uint32_t>(parsed->number) : 0;
    };
    CHECK(identifier("sourceWindow", ObjectKind::Window) == std::to_underlying(window));
    CHECK(identifier("sourcePane", ObjectKind::Pane) == sourcePane);
    const Layout::SurfaceID createdWindow { identifier("window", ObjectKind::Window) };
    const Layout::TabID createdTab { identifier("tab", ObjectKind::Tab) };
    const Layout::PaneID createdPane { identifier("pane", ObjectKind::Pane) };
    const auto createdSession = identifier("session", ObjectKind::Session);
    PREMISE(createdWindow != Layout::invalidSurface && createdTab != Layout::invalidTab && createdPane != Layout::invalidPane && createdSession);
    CHECK(createdWindow != window && model.activeSurface() == createdWindow && model.surfaceCount() == 2);
    CHECK(model.activeTab(createdWindow) == createdTab && model.focusedPane(createdWindow) == createdPane);
    CHECK(model.surfaceHolding(createdPane) == createdWindow && model.tabHolding(createdWindow, createdPane) == createdTab);
    CHECK(model.tabHolding(window, Layout::PaneID { sourcePane }) == sourceTab && model.activeTab(window) == shownTab);
    CHECK(model.focusedPane(window) == sourceFocus && Screen::sessionCount(daemon) == 4);
    const auto* created = Screen::sessionTerminal(daemon, createdSession);
    PREMISE(created);
    CHECK(created->paletteBaseline.defaultBackground == source.term->paletteBaseline.defaultBackground);
    CHECK(!isServed(person.call("workspace.addWindow", request)));
    CHECK(Screen::sessionCount(daemon) == 4 && model.surfaceCount() == 2);
    return 0;
}

/// Explicit sizing can change an inactive tab while leaving every selection
/// and neighboring layout intact; malformed and stale selectors change none.
int sizingUsesOnlyTheNamedRevisionAndTab()
{
    using Ubiquitty::Codec::JSON::Value;
    using namespace Ubiquitty::Panes;
    for (const std::string method : { "tab.balance", "pane.adjustRatio", "divider.setRatio" }) {
        Daemon daemon;
        auto& first = Screen::makeSession(daemon, 24, 80);
        auto& second = Screen::makeSession(daemon, 24, 80);
        auto& third = Screen::makeSession(daemon, 24, 80);
        auto& shown = Screen::makeSession(daemon, 24, 80);
        auto& foreign = Screen::makeSession(daemon, 24, 80);
        Screen::nameWorkspace(daemon, first, "sizing");
        Screen::nameWorkspace(daemon, foreign, "foreign");
        auto& model = Screen::layout(daemon, first);
        const auto window = model.activeSurface();
        const auto tab = model.activeTab(window);
        Screen::splitInto(daemon, first, second, Orientation::Vertical);
        Screen::splitInto(daemon, first, third, Orientation::Horizontal);
        const auto* tree = model.paneTree(window, tab);
        PREMISE(tree);
        const auto root = tree->root();
        const auto nested = tree->node(root).second;
        PREMISE(!tree->isLeaf(nested));
        PREMISE(model.setTabDividerRatio(window, tab, root, 0.2f));
        PREMISE(model.setTabDividerRatio(window, tab, nested, 0.7f));
        const auto hiddenFocus = tree->focusedPaneID();
        const auto visibleTab = Screen::tabInto(daemon, first, shown);
        const auto visibleFocus = model.focusedPane(window);
        Caller person(daemon, first.id);
        Screen::grantScope(daemon, person.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
        Screen::markInteractiveUser(daemon, person.index);
        Screen::bindWorkspace(daemon, person.index, first);
        Screen::greet(daemon, person.index, person.client);
        const auto revision = Screen::windowStructureRevision(daemon, std::to_underlying(window));
        auto params = Value::object();
        params.set("window", Value::number(std::to_underlying(window)));
        params.set("tab", Value::number(std::to_underlying(tab)));
        params.set("expectedRevision", Value::string(std::to_string(revision)));
        if (method == "pane.adjustRatio") {
            params.set("pane", Value::number(Screen::layoutPane(daemon, first)));
            params.set("direction", Value::string("right"));
            params.set("step", Value::number(0.1));
        } else if (method == "divider.setRatio") {
            auto path = Value::array();
            path.push(Value::number(1));
            params.set("path", std::move(path));
            params.set("ratio", Value::number(0.4));
        }
        const auto replace = [&](std::string_view field, Value replacement) {
            auto changed = Value::object();
            for (const auto& [name, value] : params.members())
                changed.set(name, name == field ? replacement : value);
            return Ubiquitty::Codec::JSON::serialize(changed);
        };
        const auto unchanged = [&] {
            CHECK(tree->node(root).ratio == 0.2f && tree->node(nested).ratio == 0.7f);
            CHECK(tree->focusedPaneID() == hiddenFocus && model.activeTab(window) == visibleTab && model.focusedPane(window) == visibleFocus);
            CHECK(Screen::sessionCount(daemon) == 5 && model.paneTree(window, visibleTab)->paneCount() == 1);
        };
        for (const auto& bad : { replace("window", Value::number(999999)), replace("tab", Value::number(999999)),
                 replace("expectedRevision", Value::string(std::to_string(revision + 1))), replace("expectedRevision", Value::string("0")) }) {
            CHECK(!isServed(person.call(method, bad)));
            unchanged();
        }
        if (method == "pane.adjustRatio") {
            for (const auto& bad : { replace("pane", Value::number(Screen::layoutPane(daemon, shown))),
                     replace("pane", Value::number(Screen::layoutPane(daemon, foreign))), replace("step", Value::number(0)),
                     replace("step", Value::number(-0.1)), replace("step", Value::number(2)), replace("direction", Value::string("next")) }) {
                CHECK(!isServed(person.call(method, bad)));
                unchanged();
            }
        } else if (method == "divider.setRatio") {
            for (const char* text : { "null", "true", "[true]", "[0.5]", "[2]", "[0]", "[1,0]", "[1,1,0]" }) {
                bool isJSON = false;
                const auto invalid = Ubiquitty::Codec::JSON::parse(text, isJSON);
                PREMISE(isJSON);
                CHECK(!isServed(person.call(method, replace("path", invalid))));
                unchanged();
            }
        }
        const auto request = Ubiquitty::Codec::JSON::serialize(params);
        const auto response = person.call(method, request);
        CHECK_MSG(isServed(response), response.c_str());
        CHECK(tree->focusedPaneID() == hiddenFocus && model.activeTab(window) == visibleTab && model.focusedPane(window) == visibleFocus);
        if (method == "tab.balance")
            CHECK(tree->node(root).ratio == 1.0f / 3.0f && tree->node(nested).ratio == 0.5f);
        else if (method == "pane.adjustRatio")
            CHECK(tree->node(root).ratio == 0.3f && tree->node(nested).ratio == 0.7f);
        else
            CHECK(tree->node(root).ratio == 0.2f && tree->node(nested).ratio == 0.4f);
        CHECK(!isServed(person.call(method, request)));
        CHECK(Screen::sessionCount(daemon) == 5);
    }
    return 0;
}

int clientLabelsPreserveTheExistingWorkspaceAuthority()
{
    enum class Authority {
        Person,
        StandingAgent,
        PartialStanding,
        UnrelatedAgent,
        HumanPresent,
        // A person is present on a client that shows no questions.
        EditorPresent,
        NoDrive,
        Viewer,
        NoWorkspace,
        Expired
    };
    for (const auto authority : { Authority::Person, Authority::StandingAgent, Authority::PartialStanding, Authority::UnrelatedAgent,
             Authority::HumanPresent, Authority::EditorPresent, Authority::NoDrive, Authority::Viewer, Authority::NoWorkspace, Authority::Expired }) {
        Daemon daemon;
        Screen::setRestoreDefaults(daemon);
        auto& first = Screen::makeSession(daemon, 24, 80);
        auto& second = Screen::makeSession(daemon, 24, 80);
        Screen::nameWorkspace(daemon, first, "self-label");
        Screen::splitInto(daemon, first, second, Ubiquitty::Panes::Orientation::Vertical);
        Caller caller(daemon, authority == Authority::NoWorkspace ? Sync::AdmissionTarget::Control : Sync::AdmissionTarget::Session,
            authority == Authority::NoWorkspace ? 0 : first.id);
        Screen::grantScope(daemon, caller.index, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control });
        Screen::setTokenID(daemon, caller.index, "self-label-principal");
        if (authority == Authority::Person || authority == Authority::NoDrive || authority == Authority::Viewer || authority == Authority::Expired)
            Screen::markInteractiveUser(daemon, caller.index);
        if (authority == Authority::Person || authority == Authority::NoDrive || authority == Authority::Viewer || authority == Authority::Expired)
            Screen::bindWorkspace(daemon, caller.index, first);
        const bool isPersonPresent = authority == Authority::HumanPresent || authority == Authority::EditorPresent;
        if (authority == Authority::StandingAgent || authority == Authority::PartialStanding || isPersonPresent || authority == Authority::UnrelatedAgent) {
            Screen::grantAgentSession(daemon, first, caller.index);
            if (authority == Authority::StandingAgent || isPersonPresent)
                Screen::grantAgentSession(daemon, second, caller.index);
        }
        Caller neighbor(daemon, isPersonPresent ? Sync::AdmissionTarget::Session : Sync::AdmissionTarget::Control, isPersonPresent ? first.id : 0);
        if (isPersonPresent) {
            if (authority == Authority::EditorPresent)
                neighbor.client.showsNoQuestions();
            Screen::markTrustedLocal(daemon, neighbor.index);
            Screen::bindWorkspace(daemon, neighbor.index, first);
            Screen::greet(daemon, neighbor.index, neighbor.client);
            PREMISE(neighbor.isAttached());
        }
        const ObjectID self { ObjectKind::Client, Screen::clientID(daemon, caller.index) };
        const ObjectID other { ObjectKind::Client, Screen::clientID(daemon, neighbor.index) };
        PREMISE(Screen::setLabel(daemon, other, "neighbor"));
        if (authority == Authority::Viewer)
            Screen::grantScope(daemon, caller.index, { Sync::Scope::Observe, Sync::Scope::Control });
        Screen::greet(daemon, caller.index, caller.client);
        PREMISE(caller.isAttached());
        if (authority == Authority::UnrelatedAgent) {
            // Only the admission setup was authorized. The self-label attempt
            // below has no standing authority over either workspace session.
            first.agentSessionGrantIdentities.clear();
            CHECK(!Screen::hasStandingGrant(daemon, caller.index, first.id));
            CHECK(!Screen::hasStandingGrant(daemon, caller.index, second.id));
        }
        if (authority == Authority::NoDrive)
            Screen::grantScope(daemon, caller.index, { Sync::Scope::Observe, Sync::Scope::Control });
        if (authority == Authority::Expired)
            Screen::setTokenExpiry(daemon, caller.index, 1);
        const bool isAllowed = authority == Authority::Person || authority == Authority::StandingAgent;
        const auto answer = caller.call("client.label", R"({"label":"laptop"})");
        CHECK_MSG(isServed(answer) == isAllowed, answer.c_str());
        CHECK(Screen::label(daemon, self) == (isAllowed ? "laptop" : ""));
        CHECK(Screen::label(daemon, other) == "neighbor" && Screen::sessionCount(daemon) == 2);
        CHECK(Screen::pendingStructureConsents(daemon) == 0);
        if (!isAllowed)
            continue;
        CHECK(has(answer, R"("client":)" + named(self)) && has(answer, R"("label":"laptop")"));
        const std::string invalidLabels[] = { R"({"label":"stolen","client":2})", R"({"label":"client:17"})", R"({"label":"here"})", R"({"label":"line\nfeed"})",
            std::string(R"({"label":")") + std::string(65, 'l') + R"("})" };
        for (const auto& invalid : invalidLabels) {
            CHECK(!isServed(caller.call("client.label", invalid)));
            CHECK(Screen::label(daemon, self) == "laptop" && Screen::label(daemon, other) == "neighbor");
        }
        const auto exact = std::string(64, 'l');
        CHECK(isServed(caller.call("client.label", R"({"label":")" + exact + R"("})")));
        CHECK(Screen::label(daemon, self) == exact);
        CHECK(isServed(caller.call("client.label", R"({"label":""})")));
        CHECK(Screen::label(daemon, self).empty() && Screen::label(daemon, other) == "neighbor");
        // A client's label goes with its connection, which no other is ever
        // numbered as: the book does not grow with every client that came.
        CHECK(isServed(caller.call("client.label", R"({"label":"laptop"})")));
        Screen::drop(daemon, caller.index);
        CHECK(Screen::label(daemon, self).empty() && Screen::label(daemon, other) == "neighbor");
    }
    return 0;
}

int main()
{
    // As the daemon's own process does: a write to a carrier whose reader has
    // gone is an error to handle, not a signal that ends the fixture.
    ::signal(SIGPIPE, SIG_IGN);
    if (clientLabelsPreserveTheExistingWorkspaceAuthority() != 0)
        return 1;
    if (sizingUsesOnlyTheNamedRevisionAndTab() != 0)
        return 1;
    if (windowCreationRequiresTheObservedSource() != 0)
        return 1;
    if (transfersPreserveLayoutsAndRejectForeignDestinations() != 0)
        return 1;
    if (extractionsPreserveInactiveSourcesAndRejectStaleAuthority() != 0)
        return 1;
    if (paneRelocationsPreserveIdentityAndRejectUnseenPairs() != 0)
        return 1;
    if (navigationRequiresTheObservedState() != 0)
        return 1;
    if (aLayoutOperationOutlivesAPaneResize() != 0)
        return 1;
    if (nativeTreeCallsPreserveSourceAndRetireLegacyMutations() != 0)
        return 1;
    if (windowCloseRequiresTheConfirmedRevision() != 0)
        return 1;
    if (exactOperationConsent() != 0)
        return 1;
    if (failedCreationKeepsPresentation() != 0)
        return 1;
    if (ownSessionClosesOnlyAfterBothAnswers() != 0)
        return 1;
    if (resumedConnectionsCannotAuthorizeHeldOperations() != 0)
        return 1;
    if (aTreeChangeNobodyCanShowIsRefusedAtOnce() != 0)
        return 1;
    if (aTreeChangeWaitsForAnApproverWhoseLinkWasLost() != 0)
        return 1;
    if (aPersonShownNoQuestionsMayStillRevoke() != 0)
        return 1;
    if (aTreeChangeRefusedAfterDeclinesIsSettledForEveryone() != 0)
        return 1;
    Daemon d;
    // The file the host records its named workspaces in.
    const std::string recordPath = "/tmp/ubq-structure-calls." + std::to_string(::getpid()) + ".layouts";
    Screen::setLayoutRecordPath(d, recordPath);
    const auto recordedWork = [&]() -> std::optional<Tree::HostLayoutRecord> {
        std::ifstream file(recordPath, std::ios::binary);
        const std::vector<char> bytes { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        Tree::HostLayoutFile read = Tree::readHostLayoutFile(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        for (Tree::HostLayoutRecord& record : read.records) {
            if (record.arrangement.name == "work" || record.arrangement.name == "client work")
                return std::move(record);
        }
        return std::nullopt;
    };
    const Sync::ScopeSet driving { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control };

    // "work": a window of two tabs, the first split in two, and a second
    // window of one tab. "other": one pane. A session an agent was granted,
    // in a workspace of its own. And "kept", which the host could restore.
    auto& a = Screen::makeSession(d, 24, 80);
    auto& b = Screen::makeSession(d, 24, 80);
    auto& c = Screen::makeSession(d, 24, 80);
    auto& e = Screen::makeSession(d, 24, 80);
    auto& alone = Screen::makeSession(d, 24, 80);
    auto& granted = Screen::makeSession(d, 24, 80);
    // Read while they run: a session a call ends is gone by the time the
    // test asks whether it did.
    const std::uint32_t endedByItsWindow = e.id;
    const std::uint32_t endedByItsAgent = granted.id;
    const std::uint32_t work = Screen::nameWorkspace(d, a, "work");
    Layout::Workspace& layout = Screen::layout(d, a);
    const Layout::SurfaceID firstWindow = layout.activeSurface();
    const Layout::TabID firstTab = layout.activeTab(firstWindow);
    Screen::splitInto(d, a, b, Ubiquitty::Panes::Orientation::Vertical);
    const Layout::TabID secondTab = Screen::tabInto(d, a, c);
    const Layout::SurfaceID secondWindow = Screen::windowInto(d, a, e);
    const Layout::TabID thirdTab = layout.activeTab(secondWindow);
    const std::uint32_t other = Screen::nameWorkspace(d, alone, "other");
    const std::uint32_t agents = Screen::workspaceID(d, granted);
    const Layout::SurfaceID otherWindow = Screen::layout(d, alone).activeSurface();
    const ObjectID paneOfA { ObjectKind::Pane, Screen::layoutPane(d, a) };
    CHECK_MSG(secondTab != Layout::invalidTab && secondWindow != Layout::invalidSurface && thirdTab != Layout::invalidTab, "premise: three tabs in two windows");
    Tree::HostLayoutRecord record;
    record.arrangement.name = "kept";
    Tree::WorkspaceWindow recordedWindow;
    Tree::WorkspaceTab recordedTab;
    recordedTab.root = Tree::PaneNode::makeLeaf({}, "shell");
    recordedWindow.tabs.push_back(recordedTab);
    record.arrangement.windows.push_back(recordedWindow);
    record.panes = { { 80, 24, "", "", {}, {} } };
    Screen::addRestorable(d, 900, record);

    // The host's own user, bound to no session.
    Caller owner(d, Sync::AdmissionTarget::Control);
    Screen::markTrustedLocal(d, owner.index);
    Screen::greet(d, owner.index, owner.client);
    CHECK_MSG(Screen::connection(d, owner.index)->grantedRole() == Sync::Role::Owner, "premise: the host's own user is its owner");

    // A listing that watches.
    Caller watcher(d, Sync::AdmissionTarget::Control);
    Screen::grantScope(d, watcher.index, { Sync::Scope::Observe, Sync::Scope::Control });
    Screen::setTokenID(d, watcher.index, "watcher");
    Screen::markInteractiveUser(d, watcher.index);
    Screen::greet(d, watcher.index, watcher.client);
    CHECK(isServed(watcher.call("host.sessions.subscribe", "{}")));
    const auto watched = [&] {
        Screen::deliver(d, watcher.index, watcher.client);
        return structuresOf(watcher.client);
    };
    std::map<std::uint32_t, Tree::TreeWorkspace> seen = watched();
    PREMISE(seen.contains(work) && seen.contains(other) && seen.contains(agents) && seen.contains(900));
    std::uint64_t revision = seen[work].revision;
    // What a served call changed is in the next structure, under a revision
    // above the last; a refused one leaves the structure as it was.
    const auto movedOn = [&](const char* what) {
        seen = watched();
        CHECK_MSG(seen[work].revision > revision, (std::string(what) + ": the workspace's structure did not take a new revision").c_str());
        revision = seen[work].revision;
    };
    const auto stoodStill = [&](const char* what) {
        seen = watched();
        CHECK_MSG(seen[work].revision == revision, (std::string(what) + ": a refused call moved the workspace's structure").c_str());
    };

    // 1. A label and tags at every level, set and cleared.
    struct Level {
        const char* name;
        ObjectID id;
    };
    const Level levels[] = {
        { "workspace", { ObjectKind::Workspace, work } },
        { "window", { ObjectKind::Window, std::to_underlying(secondWindow) } },
        { "tab", { ObjectKind::Tab, std::to_underlying(secondTab) } },
        { "pane", paneOfA },
        { "session", { ObjectKind::Session, b.id } },
    };
    // What the watcher's structure says of a level: its label and its tags.
    const auto said = [&](const Level& level) -> std::pair<std::string, std::vector<std::string>> {
        const Tree::TreeWorkspace& structure = seen[work];
        switch (level.id.kind) {
        case ObjectKind::Workspace:
            return { structure.label, structure.tags };
        case ObjectKind::Window:
            if (const Tree::TreeWindow* window = windowOf(structure, secondWindow))
                return { window->label, window->tags };
            break;
        case ObjectKind::Tab:
            if (const Tree::TreeTab* tab = tabOf(structure, secondTab))
                return { tab->label, tab->tags };
            break;
        case ObjectKind::Pane:
            if (const Tree::TreePane* pane = paneOf(structure, a.id))
                return { pane->label, pane->tags };
            break;
        case ObjectKind::Session:
            if (const Tree::TreePane* pane = paneOf(structure, b.id))
                return { pane->sessionLabel, pane->sessionTags };
            break;
        case ObjectKind::Client:
            break;
        }
        return { "<not listed>", {} };
    };
    for (const Level& level : levels) {
        const std::string label = std::string("the ") + level.name;
        const std::string set = owner.call("label.set", "{\"object\":" + named(level.id) + ",\"label\":" + quoted(label) + "}");
        CHECK_MSG(isServed(set) && has(set, named(level.id)) && has(set, quoted(label)), set.c_str());
        movedOn(level.name);
        CHECK_MSG(said(level).first == label, (std::string(level.name) + ": the listing does not show the label that was set").c_str());
        CHECK_MSG(Screen::label(d, level.id) == label, (std::string(level.name) + ": the host does not hold the label").c_str());

        const std::string tagged = owner.call("tags.set", "{\"object\":" + named(level.id) + ",\"tags\":[\"production\",\"" + level.name + "\",\"Production\"]}");
        CHECK_MSG(isServed(tagged) && has(tagged, "[\"production\"," + quoted(level.name) + "]"), tagged.c_str());
        movedOn(level.name);
        CHECK_MSG(said(level).second == (std::vector<std::string> { "production", level.name }), (std::string(level.name) + ": the listing does not show the tags that were set, each name once").c_str());
        CHECK_MSG(said(level).first == label, (std::string(level.name) + ": setting tags changed the label").c_str());
    }
    // Every level's label and tags are the level's own.
    for (const Level& level : levels)
        CHECK_MSG(said(level).first == std::string("the ") + level.name && said(level).second.size() == 2 && said(level).second[1] == level.name, (std::string(level.name) + ": a later call changed what an earlier one set").c_str());
    // What was set is what the host records of the workspace, and writes:
    // a served call owes the record file a write.
    {
        CHECK_MSG(Screen::isLayoutRecordWriteOwed(d), "a served call scheduled no write of the host's record");
        Screen::writeLayoutRecords(d);
        const std::optional<Tree::HostLayoutRecord> written = recordedWork();
        CHECK_MSG(written && written->workspaceLabel == "the workspace" && written->workspaceTags == (std::vector<std::string> { "production", "workspace" }), "the record file does not hold what the calls set");
        CHECK_MSG(written && written->panes.size() == 4 && written->panes[1].sessionTags == (std::vector<std::string> { "production", "session" }), "the record file does not hold a session's tags");
        CHECK(!Screen::isLayoutRecordWriteOwed(d));
        const auto recorded = Screen::captureLayoutRecord(d, a);
        PREMISE(recorded.has_value());
        CHECK_MSG(recorded->workspaceLabel == "the workspace" && recorded->workspaceTags == (std::vector<std::string> { "production", "workspace" }), "the record does not hold the workspace's label and tags");
        CHECK_MSG(recorded->windowTags.size() == 2 && recorded->windowTags[1] == (std::vector<std::string> { "production", "window" }) && recorded->tabTags.size() == 3 && recorded->tabTags[1] == (std::vector<std::string> { "production", "tab" }),
            "the record does not hold the window's and the tab's tags");
        CHECK_MSG(recorded->panes.size() == 4 && recorded->panes[0].tags == (std::vector<std::string> { "production", "pane" }) && recorded->panes[1].sessionTags == (std::vector<std::string> { "production", "session" }),
            "the record does not hold the pane's and the session's tags");
    }
    for (const Level& level : levels) {
        const std::string cleared = owner.call("label.set", "{\"object\":" + named(level.id) + ",\"label\":\"\"}");
        CHECK_MSG(isServed(cleared), cleared.c_str());
        movedOn(level.name);
        CHECK_MSG(said(level).first.empty() && said(level).second.size() == 2, (std::string(level.name) + ": clearing the label did not clear it, or took the tags with it").c_str());
        const std::string untagged = owner.call("tags.set", "{\"object\":" + named(level.id) + ",\"tags\":[]}");
        CHECK_MSG(isServed(untagged) && has(untagged, "\"tags\":[]"), untagged.c_str());
        movedOn(level.name);
        CHECK_MSG(said(level).second.empty(), (std::string(level.name) + ": setting no tags did not clear them").c_str());
    }
    // Refused, each with the reason, and nothing changed.
    {
        const std::string tab = named({ ObjectKind::Tab, std::to_underlying(secondTab) });
        const std::string looksLikeAnIdentity = owner.call("label.set", "{\"object\":" + tab + ",\"label\":\"pane:7\"}");
        CHECK_MSG(has(looksLikeAnIdentity, "-32602") && has(looksLikeAnIdentity, "cannot read as an ID"), looksLikeAnIdentity.c_str());
        const std::string longLabel = owner.call("label.set", "{\"object\":" + tab + ",\"label\":\"" + std::string(65, 'x') + "\"}");
        CHECK_MSG(has(longLabel, "-32602") && has(longLabel, "at most 64 bytes"), longLabel.c_str());
        const std::string reserved = owner.call("label.set", "{\"object\":" + tab + ",\"label\":\"here\"}");
        CHECK_MSG(has(reserved, "-32602") && has(reserved, "'here'"), reserved.c_str());
        CHECK_MSG(Screen::label(d, { ObjectKind::Tab, std::to_underlying(secondTab) }).empty(), "a refused label was stored");
        std::string many = "[";
        for (int index = 0; index < 17; ++index)
            many += (index ? ",\"t" : "\"t") + std::to_string(index) + "\"";
        const std::string tooMany = owner.call("tags.set", "{\"object\":" + tab + ",\"tags\":" + many + "]}");
        CHECK_MSG(has(tooMany, "-32602") && has(tooMany, "at most 16 tags"), tooMany.c_str());
        const std::string nameless = owner.call("tags.set", "{\"object\":" + tab + ",\"tags\":[\"kept\",\"\"]}");
        CHECK_MSG(has(nameless, "-32602") && has(nameless, "A tag has a name"), nameless.c_str());
        CHECK_MSG(Screen::tags(d, { ObjectKind::Tab, std::to_underlying(secondTab) }).empty(), "refused tags were stored");
        const std::string nothing = owner.call("label.set", "{\"object\":\"pane:99999\",\"label\":\"x\"}");
        CHECK_MSG(has(nothing, "-32003") && has(nothing, "pane:99999"), nothing.c_str());
        const std::string restorable = owner.call("tags.set", "{\"object\":\"workspace:900\",\"tags\":[\"x\"]}");
        CHECK_MSG(has(restorable, "-32003") && has(restorable, "workspace:900"), restorable.c_str());
        for (const char* object : { "\"tab\"", "\"tab:\"", "\"client:1\"", "\"7\"", "\"tab:07\"" }) {
            const std::string unnamed = owner.call("label.set", std::string("{\"object\":") + object + ",\"label\":\"x\"}");
            CHECK_MSG(has(unnamed, "-32602") && has(unnamed, "kind:number"), unnamed.c_str());
        }
        const std::string schema = owner.call("label.set", "{\"object\":" + tab + "}");
        CHECK_MSG(has(schema, "-32602") && has(schema, "label"), schema.c_str());
        stoodStill("refused labels and tags");
        // What cleared the labels and tags is owed to the record; a refused
        // call owes it nothing.
        CHECK(Screen::isLayoutRecordWriteOwed(d));
        Screen::writeLayoutRecords(d);
        const std::optional<Tree::HostLayoutRecord> cleared = recordedWork();
        CHECK_MSG(cleared && cleared->workspaceLabel.empty() && cleared->workspaceTags.empty(), "the record file still holds what the calls cleared");
        const std::string again = owner.call("label.set", "{\"object\":" + tab + ",\"label\":\"here\"}");
        CHECK_MSG(has(again, "-32602") && !Screen::isLayoutRecordWriteOwed(d), "a refused call scheduled a write of the host's record");
    }

    // 2. Names.
    {
        const std::string renamed = owner.call("workspace.rename", "{\"workspace\":" + std::to_string(work) + ",\"name\":\"client work\"}");
        CHECK_MSG(isServed(renamed) && has(renamed, "\"workspace:" + std::to_string(work) + "\"") && has(renamed, "\"client work\""), renamed.c_str());
        movedOn("workspace.rename");
        CHECK_MSG(seen[work].name == "client work", "the listing does not show the workspace's new name");
        const std::string same = owner.call("workspace.rename", "{\"workspace\":" + std::to_string(work) + ",\"name\":\"client work\"}");
        CHECK_MSG(isServed(same), "a workspace was refused the name it already has");
        const std::string taken = owner.call("workspace.rename", "{\"workspace\":" + std::to_string(work) + ",\"name\":\"other\"}");
        CHECK_MSG(has(taken, "-32602") && has(taken, "another workspace") && has(taken, "other"), taken.c_str());
        const std::string recorded = owner.call("workspace.rename", "{\"workspace\":" + std::to_string(work) + ",\"name\":\"kept\"}");
        CHECK_MSG(has(recorded, "-32602") && has(recorded, "another workspace"), recorded.c_str());
        const std::string longest(Tree::maxTreeNameBytes, 'n');
        const std::string tooLong = owner.call("workspace.rename", "{\"workspace\":" + std::to_string(work) + ",\"name\":\"" + longest + "n\"}");
        CHECK_MSG(has(tooLong, "-32602") && has(tooLong, "at most " + std::to_string(Tree::maxTreeNameBytes) + " bytes") && has(tooLong, std::to_string(Tree::maxTreeNameBytes + 1)), tooLong.substr(0, 400).c_str());
        const std::string missing = owner.call("workspace.rename", "{\"workspace\":4000000,\"name\":\"x\"}");
        CHECK_MSG(has(missing, "-32003") && has(missing, "workspace:4000000"), missing.c_str());
        stoodStill("refused workspace names");
        CHECK_MSG(seen[work].name == "client work", "a refused name replaced the workspace's");
        // The record is the workspace's under its new name.
        CHECK(Screen::isLayoutRecordWriteOwed(d));
        Screen::writeLayoutRecords(d);
        const std::optional<Tree::HostLayoutRecord> renamedRecord = recordedWork();
        CHECK_MSG(renamedRecord && renamedRecord->arrangement.name == "client work", "the record file does not hold the workspace under its new name");

        const std::string tab = std::to_string(std::to_underlying(secondTab));
        const std::string tabNamed = owner.call("tab.rename", "{\"tab\":" + tab + ",\"name\":\"logs\"}");
        CHECK_MSG(isServed(tabNamed) && has(tabNamed, "\"tab:" + tab + "\"") && has(tabNamed, "\"logs\""), tabNamed.c_str());
        movedOn("tab.rename");
        const Tree::TreeTab* listed = tabOf(seen[work], secondTab);
        CHECK_MSG(listed && listed->name == "logs", "the listing does not show the tab's new name");
        const std::string tabTooLong = owner.call("tab.rename", "{\"tab\":" + tab + ",\"name\":\"" + longest + "n\"}");
        CHECK_MSG(has(tabTooLong, "-32602") && has(tabTooLong, "at most " + std::to_string(Tree::maxTreeNameBytes) + " bytes"), tabTooLong.substr(0, 400).c_str());
        const std::string noTab = owner.call("tab.rename", "{\"tab\":99999,\"name\":\"x\"}");
        CHECK_MSG(has(noTab, "-32003") && has(noTab, "tab:99999"), noTab.c_str());
        stoodStill("refused tab names");
        const std::string longestTaken = owner.call("tab.rename", "{\"tab\":" + tab + ",\"name\":\"" + longest + "\"}");
        CHECK_MSG(isServed(longestTaken), "a name as long as a structure carries was refused");
        movedOn("the longest tab name");
        CHECK(isServed(owner.call("tab.rename", "{\"tab\":" + tab + ",\"name\":\"logs\"}")));
        movedOn("tab.rename back");
    }

    // 3. A tab's place.
    {
        const std::uint32_t first = std::to_underlying(firstTab);
        const std::uint32_t second = std::to_underlying(secondTab);
        const std::uint32_t third = std::to_underlying(thirdTab);
        const std::string here = std::to_string(std::to_underlying(firstWindow));
        const std::string there = std::to_string(std::to_underlying(secondWindow));
        CHECK(isServed(owner.call("label.set", "{\"object\":\"tab:" + std::to_string(second) + "\",\"label\":\"tailing\"}")));
        movedOn("a label before the move");
        const auto move = [&](std::uint32_t tab, const std::string& window, int index) {
            return owner.call("tab.move", "{\"tab\":" + std::to_string(tab) + ",\"window\":" + window + ",\"index\":" + std::to_string(index) + "}");
        };
        const std::string reordered = move(second, here, 0);
        CHECK_MSG(isServed(reordered) && has(reordered, "\"tab:" + std::to_string(second) + "\"") && has(reordered, "\"window:" + here + "\"") && has(reordered, "\"index\":0"), reordered.c_str());
        movedOn("tab.move within a window");
        const Tree::TreeWindow* window = windowOf(seen[work], firstWindow);
        PREMISE(window != nullptr);
        CHECK_MSG(tabsOf(*window) == (std::vector<std::uint32_t> { second, first }), "the listing does not show the tabs in their new order");
        CHECK_MSG(window->tabs[0].name == "logs" && window->tabs[0].label == "tailing" && window->tabs[0].panes.size() == 1 && window->tabs[0].panes[0].sessionID == c.id, "a moved tab did not keep its name, its label and its pane");

        // The furthest place within a window is its last; in another, one
        // past its last.
        const std::string past = move(second, here, 2);
        CHECK_MSG(has(past, "-32602") && has(past, "holds 2 tabs") && has(past, "is 1"), past.c_str());
        const std::string pastThere = move(second, there, 2);
        CHECK_MSG(has(pastThere, "-32602") && has(pastThere, "holds 1 tab,") && has(pastThere, "is 1"), pastThere.c_str());
        const std::string elsewhere = move(second, std::to_string(std::to_underlying(otherWindow)), 0);
        CHECK_MSG(has(elsewhere, "-32602") && has(elsewhere, "its own workspace") && has(elsewhere, "workspace:" + std::to_string(other)), elsewhere.c_str());
        const std::string noWindow = move(second, "99999", 0);
        CHECK_MSG(has(noWindow, "-32003") && has(noWindow, "window:99999"), noWindow.c_str());
        const std::string noTab = move(99999, here, 0);
        CHECK_MSG(has(noTab, "-32003") && has(noTab, "tab:99999"), noTab.c_str());
        stoodStill("refused tab moves");

        const std::string across = move(second, there, 1);
        CHECK_MSG(isServed(across), across.c_str());
        movedOn("tab.move to another window");
        const Tree::TreeWindow* from = windowOf(seen[work], firstWindow);
        const Tree::TreeWindow* to = windowOf(seen[work], secondWindow);
        PREMISE(from != nullptr && to != nullptr);
        CHECK_MSG(tabsOf(*from) == (std::vector<std::uint32_t> { first }) && tabsOf(*to) == (std::vector<std::uint32_t> { third, second }), "the listing does not show the tab in the window it moved to, at the place named");
        CHECK_MSG(to->tabs[1].label == "tailing" && to->tabs[1].name == "logs" && to->tabs[1].isHostActive, "a tab moved to another window did not keep what it is, or is not the one shown there");
        // A window that gave away its only tab is gone.
        CHECK(isServed(move(first, there, 0)));
        movedOn("tab.move of a window's only tab");
        CHECK_MSG(seen[work].windows.size() == 1 && tabsOf(seen[work].windows[0]) == (std::vector<std::uint32_t> { first, third, second }), "a window left with no tab is still listed, or its tab did not arrive");
        CHECK_MSG(Screen::hasSession(d, a.id) && Screen::hasSession(d, b.id), "moving a window's only tab ended its sessions");
    }

    // 4. Who may: every call against every caller (Docs/sync.md §7.3).
    {
        const auto number = [](std::uint32_t value) { return std::to_string(value); };
        // What a call is aimed at: a workspace, a window and a tab of it, a
        // session in it, and the name it has.
        struct Target {
            std::uint32_t workspace;
            std::uint32_t window;
            std::uint32_t tab;
            std::uint32_t session;
            std::string name;
        };
        const Layout::SurfaceID agentsSurface = Screen::layout(d, granted).activeSurface();
        const Target ofOther { other, std::to_underlying(otherWindow), std::to_underlying(Screen::layout(d, alone).activeTab(otherWindow)), alone.id, "other" };
        const Target ofWork { work, std::to_underlying(secondWindow), std::to_underlying(firstTab), a.id, "client work" };
        const Target ofAgents { agents, std::to_underlying(agentsSurface), std::to_underlying(Screen::layout(d, granted).activeTab(agentsSurface)), granted.id, "" };
        // A call's parameters, aimed. One that is served leaves what later
        // checks read as it was: the workspace keeps its name and the tab
        // moves to where it stands.
        const auto aimed = [&](const std::string& method, const Target& target) -> std::string {
            if (method == "label.set")
                return "{\"object\":\"session:" + number(target.session) + "\",\"label\":\"cell\"}";
            if (method == "tags.set")
                return "{\"object\":\"tab:" + number(target.tab) + "\",\"tags\":[\"cell\"]}";
            if (method == "workspace.rename")
                return "{\"workspace\":" + number(target.workspace) + ",\"name\":\"" + target.name + "\"}";
            if (method == "tab.rename")
                return "{\"tab\":" + number(target.tab) + ",\"name\":\"cell\"}";
            if (method == "tab.move")
                return "{\"tab\":" + number(target.tab) + ",\"window\":" + number(target.window) + ",\"index\":0}";
            return "{\"window\":" + number(target.window) + "}";
        };
        const std::vector<const char*> labelling { "label.set", "tags.set" };
        const std::vector<const char*> rearranging { "workspace.rename", "tab.rename", "tab.move" };
        const std::vector<const char*> closing { "window.close" };
        // One row of the table: these calls, by this caller, at this target,
        // served, or refused as forbidden with words that say why.
        const auto served = [&](Caller& caller, const char* who, const Target& target, const std::vector<const char*>& methods) {
            for (const char* method : methods) {
                const std::string answer = caller.call(method, aimed(method, target));
                CHECK_MSG(isServed(answer), (std::string(who) + " / " + method + " was not served: " + answer).c_str());
            }
        };
        const auto refused = [&](Caller& caller, const char* who, const Target& target, const std::vector<const char*>& methods, const char* reason) {
            for (const char* method : methods) {
                const std::string answer = caller.call(method, aimed(method, target));
                CHECK_MSG(has(answer, "-32002") && has(answer, method) && has(answer, reason), (std::string(who) + " / " + method + " was not refused for \"" + reason + "\": " + answer).c_str());
                CHECK_MSG(caller.isAttached(), (std::string(who) + " / " + method + ": a refused caller was closed").c_str());
            }
        };

        // A person on another machine whose token may drive, attached to
        // "other"; a viewer and a person whose token only observes, attached
        // to it too.
        Caller remote(d, alone.id);
        Screen::grantScope(d, remote.index, driving);
        Screen::setTokenID(d, remote.index, "remote");
        Screen::markInteractiveUser(d, remote.index);
        Screen::bindWorkspace(d, remote.index, alone);
        Screen::greet(d, remote.index, remote.client);
        CHECK_MSG(Screen::connection(d, remote.index)->grantedRole() == Sync::Role::Driver, "premise: a person whose token may drive is a driver");
        Caller viewer(d, alone.id);
        Screen::grantScope(d, viewer.index, { Sync::Scope::Observe, Sync::Scope::Control });
        Screen::setTokenID(d, viewer.index, "viewer");
        Screen::markInteractiveUser(d, viewer.index);
        Screen::bindWorkspace(d, viewer.index, alone);
        Screen::greet(d, viewer.index, viewer.client);
        Caller observer(d, alone.id);
        Screen::grantScope(d, observer.index, { Sync::Scope::Observe });
        Screen::setTokenID(d, observer.index, "observer");
        Screen::markInteractiveUser(d, observer.index);
        Screen::bindWorkspace(d, observer.index, alone);
        Screen::greet(d, observer.index, observer.client);
        CHECK_MSG(Screen::connection(d, viewer.index)->grantedRole() == Sync::Role::Viewer && Screen::connection(d, observer.index)->grantedRole() == Sync::Role::Viewer, "premise: a person whose token cannot drive is a viewer");
        // Agents: one bound to no session, one whose token cannot drive, one
        // whose pane is the granted workspace's, one whose pane is "other"'s.
        const auto agentOn = [&](Caller& caller, Sync::ScopeSet scope, const char* token) {
            Screen::grantScope(d, caller.index, scope);
            Screen::setTokenID(d, caller.index, token);
            Screen::greet(d, caller.index, caller.client);
            CHECK_MSG(Screen::connection(d, caller.index)->grantedRole() == Sync::Role::Agent, "premise: automation is an agent whatever its token carries");
        };
        Caller agent(d, Sync::AdmissionTarget::Control);
        agentOn(agent, driving, "agent");
        Caller observingAgent(d, Sync::AdmissionTarget::Control);
        agentOn(observingAgent, { Sync::Scope::Observe, Sync::Scope::Control }, "observing agent");
        Caller inside(d, granted.id);
        Screen::grantScope(d, inside.index, driving);
        Screen::setTokenID(d, inside.index, "agent inside");
        Screen::grantAgentSession(d, granted, inside.index);
        Screen::greet(d, inside.index, inside.client);
        PREMISE(inside.isAttached() && Screen::connection(d, inside.index)->grantedRole() == Sync::Role::Agent);
        Caller insideOther(d, alone.id);
        Screen::grantScope(d, insideOther.index, driving);
        Screen::setTokenID(d, insideOther.index, "agent inside other");
        Screen::grantAgentSession(d, alone, insideOther.index);
        Screen::greet(d, insideOther.index, insideOther.client);
        PREMISE(insideOther.isAttached() && Screen::connection(d, insideOther.index)->grantedRole() == Sync::Role::Agent);

        // An attached client's structure follows a call, whoever made it: a
        // person attached to "other", who lists nothing, is sent the
        // workspace's structure at its next revision.
        {
            Screen::deliver(d, remote.index, remote.client);
            const std::map<std::uint32_t, Tree::TreeWorkspace> before = structuresOf(remote.client);
            PREMISE(before.size() == 1 && before.contains(other));
            CHECK(isServed(owner.call("label.set", "{\"object\":\"workspace:" + number(other) + "\",\"label\":\"theirs to see\"}")));
            Screen::deliver(d, remote.index, remote.client);
            std::map<std::uint32_t, Tree::TreeWorkspace> after = structuresOf(remote.client);
            CHECK_MSG(after[other].revision > before.at(other).revision && after[other].label == "theirs to see", "a person attached to a workspace was not sent its structure after another caller labelled it");
            const std::string own = remote.call("label.set", "{\"object\":\"workspace:" + number(other) + "\",\"label\":\"mine\"}");
            CHECK_MSG(isServed(own), own.c_str());
            const std::uint64_t sent = after[other].revision;
            after = structuresOf(remote.client);
            CHECK_MSG(after[other].revision > sent && after[other].label == "mine" && after.size() == 1, "a person was not sent the structure of their own change, or was sent another workspace's");
            seen = watched();
            CHECK_MSG(seen[other].label == "mine", "a person's label on the workspace they are attached to is not listed");
        }

        // The table.
        served(remote, "a person's driver on its own workspace", ofOther, labelling);
        served(remote, "a person's driver on its own workspace", ofOther, rearranging);
        const char* notAttached = "attached to workspace:";
        refused(remote, "a person's driver on another workspace", ofWork, labelling, notAttached);
        refused(remote, "a person's driver on another workspace", ofWork, rearranging, notAttached);
        refused(remote, "a person's driver on another workspace", ofWork, closing, notAttached);
        refused(remote, "a person's driver on another workspace", ofAgents, closing, notAttached);
        for (const Target* target : { &ofOther, &ofWork }) {
            for (const std::vector<const char*>* methods : { &labelling, &rearranging, &closing }) {
                refused(viewer, "a viewer", *target, *methods, "attached as viewer");
                refused(observer, "a person whose token only observes", *target, *methods, "attached as viewer");
            }
        }
        // A standing attach grant never answers an exact tree question. With
        // no person attached every method refuses, including label and tags.
        Screen::grantAgentSession(d, granted, inside.index);
        for (Caller* caller : { &agent, &inside, &insideOther }) {
            for (const std::vector<const char*>* methods : { &labelling, &rearranging, &closing })
                refused(*caller, "an agent without an attached approver", ofAgents, *methods, "no such person is attached");
        }
        for (const Target* target : { &ofOther, &ofWork, &ofAgents }) {
            for (const std::vector<const char*>* methods : { &labelling, &rearranging, &closing })
                refused(observingAgent, "an agent whose token cannot drive", *target, *methods, "drive scope");
        }
        // Nothing a refused caller named was changed.
        seen = watched();
        CHECK_MSG(seen[work].name == "client work" && seen[other].name == "other" && seen[other].label == "mine", "a refused call changed a workspace");
        const Tree::TreePane* labelled = paneOf(seen[agents], granted.id);
        CHECK_MSG(labelled && labelled->sessionLabel.empty(), "an attach grant bypassed exact-operation consent");
        CHECK_MSG(paneOf(seen[work], a.id) && paneOf(seen[work], a.id)->sessionLabel.empty(), "a refused caller labelled a session of a workspace it may not change");
        revision = seen[work].revision;

        // 5. A closed window. The host's own user closes the one window of
        // the workspace the agents were granted: the workspace ends with it.
        // A call behind the close in the same pass finds the window already
        // gone: nothing is done to what is ending.
        const std::string agentsWindow = number(ofAgents.window);
        const std::vector<std::string> ending = owner.callBoth("\"method\":\"window.close\",\"params\":{\"window\":" + agentsWindow + "}",
            "\"method\":\"tags.set\",\"params\":{\"object\":\"window:" + agentsWindow + "\",\"tags\":[\"late\"]}");
        CHECK_MSG(isServed(ending[0]) && has(ending[0], "\"sessionsEnded\":1") && has(ending[0], "\"window:" + agentsWindow + "\""), ending[0].c_str());
        CHECK_MSG(has(ending[1], "-32003") && has(ending[1], "window:" + agentsWindow), ending[1].c_str());
        seen = watched();
        CHECK_MSG(!seen.contains(agents) && !Screen::hasSession(d, endedByItsAgent), "a workspace whose only window was closed is still listed, or its session still runs");

        // A window of "work" that holds one of its tabs: its session ends,
        // and the workspace stands.
        Layout::Workspace& model = Screen::layout(d, a);
        const std::string unknown = owner.call("window.close", "{\"window\":99999}");
        CHECK_MSG(has(unknown, "-32003") && has(unknown, "window:99999"), unknown.c_str());
        stoodStill("closing a window the host does not hold");
        const Layout::SurfaceID spare = model.extractTabToSurface(model.activeSurface(), thirdTab);
        PREMISE(spare != Layout::invalidSurface && model.surfaceCount() == 2);
        const std::string closed = owner.call("window.close", "{\"window\":" + number(std::to_underlying(spare)) + "}");
        CHECK_MSG(isServed(closed) && has(closed, "\"sessionsEnded\":1"), closed.c_str());
        movedOn("window.close");
        CHECK_MSG(seen[work].windows.size() == 1 && !Screen::hasSession(d, endedByItsWindow), "a closed window is still listed, or its session still runs");
        CHECK_MSG(Screen::hasSession(d, a.id) && Screen::hasSession(d, b.id) && Screen::hasSession(d, c.id), "closing one window ended the sessions of another");

        // A person closes the only window of the workspace they are attached
        // to, on the connection attached to it. The call is answered before
        // the session behind that connection ends, and a call behind it in
        // the same pass finds the workspace already gone.
        const std::uint32_t aloneID = alone.id;
        const std::vector<std::string> both = remote.callBoth("\"method\":\"window.close\",\"params\":{\"window\":" + number(ofOther.window) + "}",
            "\"method\":\"label.set\",\"params\":{\"object\":\"session:" + number(aloneID) + "\",\"label\":\"late\"}");
        CHECK_MSG(isServed(both[0]) && has(both[0], "\"sessionsEnded\":1"), both[0].c_str());
        CHECK_MSG(has(both[1], "-32003") && has(both[1], "session:" + number(aloneID)), both[1].c_str());
        seen = watched();
        CHECK_MSG(!seen.contains(other) && !Screen::hasSession(d, aloneID), "a workspace whose only window its own person closed is still listed, or its session still runs");
        CHECK_MSG(seen.contains(work) && seen.contains(900), "closing one workspace took another from the listing");
    }

    ::unlink(recordPath.c_str());
    if (failures == 0)
        std::printf("structure_calls: labels, tags, names, a tab's place and a closed window are each served to whoever may change the workspace, refused with the reason otherwise, and seen by a watching listing at a new revision\n");
    return failures == 0 ? 0 : 1;
}
