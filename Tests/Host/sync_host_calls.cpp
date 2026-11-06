// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/sync_host_calls.cpp — a Sync call addressed to the host, and
// the one check every call passes before it is served (Docs/sync.md §7.3).
//
// Six clients of one daemon, holding different authority:
//
//   1. a person whose token carries Control calls `host.methods` and
//      `host.describe` with no pane named, and is answered;
//   2. a person whose token does not is refused `host.methods` with the scope
//      named, stays attached, and is still served a pane call;
//   3. a person with Observe and Control, but no Drive or RegionRead, is a viewer
//      and is
//      refused `pane.pinGeometry` for its role, before its scope, and
//      `pane.capture` because observing grants no RegionRead authority;
//   4. a client the daemon verifies as automation is attached as an agent
//      whatever it asked to be, and is listed only what an agent may call;
//   5. a connection bound to no session, as a directory errand's is, is
//      welcomed, holds no pane and is served the host's methods.
//   6. a viewer granted RegionRead can capture text without gaining Drive.
//
// A call whose parameters do not fit its entry's schema is refused before any
// effect, and one for no method is not found. Every refusal carries a reason.
//
// Friend seam, the QUIC-off host build, as geometry_follows_hands. Exit 0 on
// success.
//

#include "Assertions.h"
#include "NotifyRead.h"

#include "Core/Accessibility/TextProjection.h"
#include "Core/CellGrid.h"
#include "Core/Codec/Base64.h"
#include "Core/Codec/JSON.h"
#include "Core/Protocol/Control/Consent.h"
#include "Core/Protocol/Control/JSONSchema.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/IDText.h"
#include "Core/Protocol/Sync/Roles.h"
#include "Core/Screen/ClearBuffer.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Session/BackgroundEvents.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/FullCarrier.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <array>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

/// One post-auth authority-model client and what it was answered.
/// Scope and interactive-user verdicts are fixture inputs, not credential/TLS
/// verification evidence. The actual Greet still performs target admission.
struct Caller {
    Daemon& daemon;
    Ubiquitty::HostTest::FullCarrier carrier;
    std::size_t index;
    Sync::AdmissionRequest admission;
    UbiquittyTest::ScreenClient client { 24, 80 };
    int nextID = 1;

    Caller(Daemon& d, std::uint32_t sessionID, Sync::ScopeSet scope, const char* tokenID)
        : daemon(d)
        , index(Screen::attach(d, sessionID, carrier.takeDaemonEnd()))
        , admission { .target = Sync::AdmissionTarget::Session, .sessionID = sessionID, .expectedDaemonInstanceID = Screen::daemonInstanceID(d), .isAttachOnly = true }
    {
        CHECK(carrier.isReady() && sessionID != 0);
        CHECK(tokenID && *tokenID);
        Screen::grantScope(d, index, scope);
        Screen::setTokenID(d, index, tokenID);
    }

    Caller(Daemon& d, Sync::AdmissionTarget target, Sync::ScopeSet scope, const char* tokenID)
        : daemon(d)
        , index(Screen::connect(d, carrier.takeDaemonEnd()))
        , admission { .target = target, .expectedDaemonInstanceID = Screen::daemonInstanceID(d) }
    {
        CHECK(carrier.isReady() && Sync::isUnboundTarget(target));
        CHECK(tokenID && *tokenID);
        Screen::grantScope(d, index, scope);
        Screen::setTokenID(d, index, tokenID);
    }

    void greet()
    {
        CHECK(client.start(admission));
        Screen::receive(daemon, index, client.takeStream(Screen::now()));
        Screen::deliver(daemon, index, client);
        const auto welcome = client.welcome();
        CHECK(welcome && welcome->admission == Sync::AttachmentAdmission::Granted);
        if (!welcome)
            return;
        CHECK(welcome->selection.target == admission.target
            && welcome->selection.sessionID == admission.sessionID
            && welcome->selection.daemonInstanceID == admission.expectedDaemonInstanceID);
        if (Sync::isUnboundTarget(admission.target)) {
            CHECK(welcome->selection.workspaceID == 0);
            CHECK(client.connection().boundPanes().empty());
        }
    }

    /// Calls a method and returns the one answer.
    std::string call(const std::string& method, const std::string& parameters = "{}")
    {
        const std::string request = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(nextID++) + ",\"method\":\"" + method + "\",\"params\":" + parameters + "}";
        CHECK(client.call(request));
        Screen::deliver(daemon, index, client);
        Screen::deliver(daemon, index, client);
        const std::vector<std::string> results = client.takeResults();
        CHECK_MSG(results.size() == 1, (method + " drew " + std::to_string(results.size()) + " results").c_str());
        return results.empty() ? std::string() : results.front();
    }

    [[nodiscard]] bool isAttached() const
    {
        const Sync::HostConnection* connection = Screen::connection(daemon, index);
        return connection && connection->state() == Sync::ConnectionState::Attached;
    }

    [[nodiscard]] Sync::Role role() const { return Screen::connection(daemon, index)->grantedRole(); }
};

bool has(const std::string& answer, const char* needle) { return answer.find(needle) != std::string::npos; }

Ubiquitty::Codec::JSON::Value captureResult(Caller& caller)
{
    using Ubiquitty::Codec::JSON::Value;
    const std::string answer = caller.call("pane.capture", R"({"pane":1})");
    bool isParsed = false;
    const Value parsed = Ubiquitty::Codec::JSON::parse(answer, isParsed);
    const Value* result = isParsed ? parsed.find("result") : nullptr;
    CHECK_MSG(result, "capture did not return a result");
    if (!result)
        return {};
    const auto* method = Ubiquitty::Protocol::Control::findSyncMethod("pane.capture");
    CHECK(method);
    if (method)
        CHECK(!Ubiquitty::Protocol::Control::validate(*result, Ubiquitty::Protocol::Control::parseRegistryJSON(method->resultSchema)));
    return *result;
}

void checkClearAuthorityAndExactCommand()
{
    using Ubiquitty::Screen::ClearCommand;
    for (const auto& [name, command] : { std::pair { "buffer", ClearCommand::Buffer }, { "scrollback", ClearCommand::Scrollback }, { "screen", ClearCommand::Screen }, { "previous-mark", ClearCommand::ToPreviousMark } }) {
        Daemon daemon;
        auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        auto* writer = stream.get();
        auto& session = Screen::makeSession(daemon, 24, 80, std::move(stream));
        Ubiquitty::Terminal expected;
        Ubiquitty::resize(expected, 24, 80);
        session.term->scrollbackLimit = expected.scrollbackLimit = 2000;
        for (int row = 0; row < 50; ++row) {
            Ubiquitty::processBytes(session.term.get(), "history\r\n");
            Ubiquitty::processBytes(&expected, "history\r\n");
        }
        Ubiquitty::processBytes(session.term.get(), "\033]133;A\aprompt$ typed");
        Ubiquitty::processBytes(&expected, "\033]133;A\aprompt$ typed");
        const auto before = Ubiquitty::Accessibility::visibleScreenText(*session.term, 0);
        const int historyBefore = Ubiquitty::Screen::scrollbackCount(session.term.get());
        Caller viewer(daemon, session.id, { Sync::Scope::Observe }, "clear-viewer");
        Screen::markInteractiveUser(daemon, viewer.index);
        viewer.greet();
        const auto parameters = std::string(R"({"pane":1,"command":")") + name + R"("})";
        CHECK(has(viewer.call("pane.clear", parameters), "-32002"));
        CHECK(Ubiquitty::Accessibility::visibleScreenText(*session.term, 0) == before);
        CHECK(Ubiquitty::Screen::scrollbackCount(session.term.get()) == historyBefore);
        CHECK(writer->written.empty());
        CHECK(viewer.isAttached());

        Caller driver(daemon, session.id, { Sync::Scope::Observe, Sync::Scope::Drive }, "clear-driver");
        Screen::markInteractiveUser(daemon, driver.index);
        driver.greet();
        for (const char* invalid : { R"({"pane":1,"command":"other"})", R"({"pane":1,"command":0})", R"({"pane":1})", R"({"pane":1,"command":"buffer","all":true})" })
            CHECK(has(driver.call("pane.clear", invalid), "-32602"));
        CHECK(has(driver.call("pane.clear", R"({"pane":2,"command":"buffer"})"), "-32003"));
        CHECK(Ubiquitty::Accessibility::visibleScreenText(*session.term, 0) == before);
        CHECK(Ubiquitty::Screen::scrollbackCount(session.term.get()) == historyBefore);
        CHECK(writer->written.empty());

        std::string expectedInput;
        Ubiquitty::Screen::runClearCommand(command, &expected, nullptr, [&](std::span<const char> bytes) { expectedInput.append(bytes.begin(), bytes.end()); });
        const auto answer = driver.call("pane.clear", parameters);
        bool parsed = false;
        const auto decoded = Ubiquitty::Codec::JSON::parse(answer, parsed);
        const auto* result = parsed ? decoded.find("result") : nullptr;
        CHECK(result);
        if (result) {
            CHECK(result->numberOr("pane") == 1 && result->stringOr("command") == name);
        }
        CHECK(Ubiquitty::Accessibility::visibleScreenText(*session.term, 0) == Ubiquitty::Accessibility::visibleScreenText(expected, 0));
        CHECK(Ubiquitty::Screen::scrollbackCount(session.term.get()) == Ubiquitty::Screen::scrollbackCount(&expected));
        CHECK(writer->written == expectedInput);
        CHECK(driver.isAttached());
    }
}

void checkClearInputProvenance()
{
    for (const std::string command : { "buffer", "screen", "previous-mark", "scrollback" }) {
        Daemon daemon;
        auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        auto* writer = stream.get();
        auto& session = Screen::makeSession(daemon, 24, 80, std::move(stream));
        // A program that owns the layout receives a redraw request instead
        // of a model-only clear, except when only scrollback is discarded.
        Ubiquitty::processBytes(session.term.get(), "\033[?1049h");
        Caller first(daemon, session.id, { Sync::Scope::Observe, Sync::Scope::Drive }, "clear-first-principal");
        Screen::markInteractiveUser(daemon, first.index);
        Screen::markBootstrapToken(daemon, first.index);
        first.greet();
        CHECK(first.isAttached() && first.role() == Sync::Role::Driver);
        const std::array<std::uint8_t, 1> input { 'x' };
        CHECK(first.client.sendRaw(input).has_value());
        Screen::deliver(daemon, first.index, first.client);
        CHECK(writer->written == "x");
        CHECK(session.firstInputPrincipal == "human-account:" + std::to_string(::geteuid()));
        CHECK(!session.hasMultipleInputPrincipals);
        const auto firstPrincipal = session.firstInputPrincipal;
        writer->written.clear();

        Ubiquitty::HostTest::FullCarrier secondCarrier;
        CHECK(secondCarrier.isReady());
        const auto secondIndex = Screen::attach(daemon, session.id, secondCarrier.takeDaemonEnd());
        Screen::grantScope(daemon, secondIndex, { Sync::Scope::Observe, Sync::Scope::Drive });
        Screen::setTokenID(daemon, secondIndex, "clear-second-principal");
        Screen::markInteractiveUser(daemon, secondIndex);
        Screen::markBootstrapToken(daemon, secondIndex);
        UbiquittyTest::ScreenClient second(24, 80, Sync::ID128 { 31, 32 });
        Screen::greet(daemon, secondIndex, second);
        CHECK(Screen::connection(daemon, secondIndex)->state() == Sync::ConnectionState::Attached);
        CHECK(Screen::connection(daemon, secondIndex)->grantedRole() == Sync::Role::Driver);
        CHECK(first.isAttached());
        CHECK(session.firstInputPrincipal == firstPrincipal && !session.hasMultipleInputPrincipals);

        const std::string request = "{\"jsonrpc\":\"2.0\",\"id\":\"clear-provenance\",\"method\":\"pane.clear\",\"params\":{\"pane\":1,\"command\":\"" + command + "\"}}";
        CHECK(second.call(request));
        Screen::deliver(daemon, secondIndex, second);
        Screen::deliver(daemon, secondIndex, second);
        const auto answers = second.takeResults();
        CHECK(answers.size() == 1);
        if (answers.size() == 1)
            CHECK(has(answers.front(), "\"result\"") && !has(answers.front(), "\"error\""));
        const bool shouldRedraw = command != "scrollback";
        CHECK(writer->written == (shouldRedraw ? "\x0c" : ""));
        CHECK(session.firstInputPrincipal == firstPrincipal);
        CHECK_MSG(!session.hasMultipleInputPrincipals,
            "another bootstrap for the same account changed input provenance");
    }
}

void checkCaptureTextAndCompleteness()
{
    {
        Daemon daemon;
        auto& session = Screen::makeSession(daemon, 6, 8);
        Caller caller(daemon, session.id, { Sync::Scope::Observe, Sync::Scope::RegionRead }, "range-reader");
        Screen::markInteractiveUser(daemon, caller.index);
        caller.greet();
        CHECK(has(caller.call("pane.capture", R"({"pane":1,"source":"last-command"})"), "-32003"));
        Ubiquitty::processBytes(session.term.get(), "cmd\r\n\x1b]133;C\aout\r\n\x1b]133;A\aprompt");
        for (const auto& [parameters, source, first, last, expected] : {
                 std::tuple { R"({"pane":1,"source":"last-command"})", "last-command", 1, 1, "out\n" },
                 std::tuple { R"({"pane":1,"source":"rows","start":-1,"end":1})", "rows", 0, 1, "cmd\nout\n" },
                 std::tuple { R"({"pane":1,"source":"tail","lines":1})", "tail", 5, 5, "\n" } }) {
            bool parsed = false;
            const auto reply = Ubiquitty::Codec::JSON::parse(caller.call("pane.capture", parameters), parsed);
            const auto* result = parsed ? reply.find("result") : nullptr;
            CHECK_MSG(result, "typed range capture did not return text");
            if (!result)
                continue;
            CHECK(result->stringOr("source") == source && result->stringOr("text") == expected);
            CHECK(result->numberOr("first") == first && result->numberOr("last") == last && result->numberOr("totalRows") == 6);
            CHECK(result->boolOr("complete") && result->numberOr("columns") == 8 && result->numberOr("pane") == 1);
        }
        for (const char* parameters : { R"({"pane":1,"source":"rows"})", R"({"pane":1,"source":"tail","lines":0})",
                 R"({"pane":1,"source":"screen","start":0})", R"({"pane":1,"source":"last-command","end":1})",
                 R"({"pane":1,"source":"rows","start":0.5,"end":1})", R"({"pane":1,"source":"selection"})" })
            CHECK(has(caller.call("pane.capture", parameters), "-32602"));
        CHECK(has(caller.call("pane.capture", R"({"pane":1,"source":"rows","start":2,"end":1})"), "-32003"));
        CHECK(caller.isAttached());
    }
    {
        Daemon daemon;
        auto& session = Screen::makeSession(daemon, 2, 8);
        Caller caller(daemon, session.id, { Sync::Scope::Observe, Sync::Scope::RegionRead }, "text-reader");
        Screen::markInteractiveUser(daemon, caller.index);
        caller.greet();
        const auto blank = captureResult(caller);
        CHECK(blank.numberOr("pane") == 1);
        CHECK(blank.numberOr("rows") == 2 && blank.numberOr("columns") == 8);
        CHECK(blank.boolOr("complete") && blank.stringOr("text") == "\n\n");

        Ubiquitty::processBytes(session.term.get(), "e\xcc\x81  \xf0\x9f\x98\x80\r\nend  ");
        const auto text = captureResult(caller);
        CHECK(text.boolOr("complete"));
        CHECK(text.stringOr("text") == Ubiquitty::Accessibility::visibleScreenText(*session.term, 0) + '\n');

        // A scalar inside a cell can itself be LF. Completeness is metadata,
        // not a count of separators guessed from the returned text.
        Ubiquitty::Cell embedded {};
        embedded.codePoint = '\n';
        session.term->grid->fillCellRun(Ubiquitty::Row(0), Ubiquitty::Column(0), embedded, 1);
        const auto withLF = captureResult(caller);
        CHECK(withLF.boolOr("complete") && withLF.numberOr("rows") == 2);
        CHECK(withLF.stringOr("text") == Ubiquitty::Accessibility::visibleScreenText(*session.term, 0) + '\n');
        CHECK(caller.isAttached());
    }
    // Each cell below contributes two four-byte scalars. The 128th whole
    // row reaches the text bound: it completes 128 rows, but not 129.
    for (const int rows : { 128, 129 }) {
        Daemon daemon;
        auto& session = Screen::makeSession(daemon, rows, 1024);
        Caller caller(daemon, session.id, { Sync::Scope::Observe, Sync::Scope::RegionRead }, "bounded-reader");
        Screen::markInteractiveUser(daemon, caller.index);
        caller.greet();
        Ubiquitty::Cell filled {};
        filled.codePoint = 0x13000;
        filled.combining[0] = 0x1D185;
        filled.setCombiningLength(1);
        for (int row = 0; row < rows; ++row)
            session.term->grid->fillCellRun(Ubiquitty::Row(row), Ubiquitty::Column(0), filled, 1024);
        const auto result = captureResult(caller);
        CHECK(result.numberOr("rows") == rows && result.numberOr("columns") == 1024);
        CHECK(result.boolOr("complete") == (rows == 128));
        CHECK(result.stringOr("text").size() == 128u * (1024u * 8u + 1u));
        CHECK(caller.isAttached());
        CHECK(has(caller.call("pane.capture", "{}"), "-32602"));
    }
}

void checkPassiveBackgroundRefresh()
{
    using Ubiquitty::Codec::JSON::Value;
    Daemon daemon;
    auto& session = Screen::makeSession(daemon, 5, 40);
    const auto paneIdentity = session.screenSource->paneID();
    const std::string pane = Ubiquitty::Session::backgroundPaneID(paneIdentity.high, paneIdentity.low);
    session.effects.push(Ubiquitty::Host::HostNotification { .title = "Build", .body = "Complete" });
    Ubiquitty::processBytes(session.term.get(), "hello\x1b]7501;state=blocked:id=agent/question:kind=question:title=QWdlbnQ=:msg=Q29udGludWU/\x07");
    Screen::servicePTY(daemon, session);
    // Settle the initial frame before measuring whether the passive reader changes publication.
    Screen::publish(daemon, session);
    const auto generation = session.screenSource->generation();
    const auto changes = session.screenSource->changes();
    const auto focusGains = session.screenSource->personFocusGains();
    const auto primary = session.lastHumanInputClientID;

    Caller observer(daemon, Sync::AdmissionTarget::Control, { Sync::Scope::Observe, Sync::Scope::Control, Sync::Scope::RegionRead }, "background");
    Screen::markInteractiveUser(daemon, observer.index);
    observer.greet();
    const auto refresh = [&](const std::string& parameters) {
        const std::string answer = observer.call("host.refresh", parameters);
        bool isParsed = false;
        const Value parsed = Ubiquitty::Codec::JSON::parse(answer, isParsed);
        const Value* result = isParsed ? parsed.find("result") : nullptr;
        CHECK_MSG(result, answer.c_str());
        if (!result) {
            return Value {};
        }
        const auto* method = Ubiquitty::Protocol::Control::findSyncMethod("host.refresh");
        CHECK(method);
        if (method) {
            CHECK(!Ubiquitty::Protocol::Control::validate(*result, Ubiquitty::Protocol::Control::parseRegistryJSON(method->resultSchema)));
        }
        return *result;
    };
    const Value first = refresh("{\"screenPane\":\"" + pane + "\"}");
    const Value* events = first.find("events");
    CHECK(events && events->items().size() == 2);
    const Value* panes = first.find("panes");
    CHECK(panes && panes->items().size() == 1);
    const Value* screen = first.find("screen");
    CHECK(screen);
    if (screen) {
        const std::string encoded = screen->stringOr("checkpoint");
        std::vector<char> bytes(encoded.size());
        const auto checkpoint = Ubiquitty::Codec::base64Decode(encoded, bytes);
        CHECK(checkpoint && !checkpoint->empty());
        if (checkpoint && !checkpoint->empty()) {
            Ubiquitty::Terminal replica;
            Sync::ScreenReplica baseline;
            // The decoder consumes the same bytes irrespective of char signedness.
            const auto body = std::span(reinterpret_cast<const std::uint8_t*>(checkpoint->data()), checkpoint->size());
            CHECK(Sync::applyCheckpoint(replica, body, baseline) == Sync::ApplyOutcome::Applied);
            CHECK(Ubiquitty::Accessibility::visibleScreenText(replica, 0).starts_with("hello"));
        }
        const auto unchanged = refresh("{\"screenPane\":\"" + pane + "\",\"screenRevision\":\"" + screen->stringOr("revision") + "\"}");
        CHECK(!unchanged.find("screen"));
    }
    const std::string otherEpoch = first.stringOr("epoch") == "1" ? "2" : "1";
    const auto reset = refresh("{\"epoch\":\"" + otherEpoch + "\",\"paneOffset\":2}");
    CHECK(reset.boolOr("gap"));
    CHECK(reset.find("panes") && reset.find("panes")->items().size() == 1);
    const auto replay = refresh("{}");
    CHECK(replay.find("events") && replay.find("events")->items().size() == 2);
    const auto caughtUp = refresh("{\"epoch\":\"" + first.stringOr("epoch") + "\",\"after\":\"" + first.stringOr("cursor") + "\"}");
    CHECK(caughtUp.find("events") && caughtUp.find("events")->items().empty());
    Ubiquitty::processBytes(session.term.get(), "\x1b]7501;state=working:id=agent/question\x07");
    const auto resolved = refresh("{}");
    CHECK(resolved.find("events") && resolved.find("events")->items().size() == 1);
    CHECK(session.term->rows == 5 && session.term->columns == 40);
    CHECK(session.screenSource->generation() == generation);
    CHECK(session.screenSource->changes() == changes);
    CHECK(session.screenSource->personFocusGains() == focusGains);
    CHECK(session.lastHumanInputClientID == primary);
    CHECK(Screen::connection(daemon, observer.index)->boundPanes().empty());
    CHECK(observer.role() == Sync::Role::Viewer);
    CHECK(has(observer.call("host.refresh", R"({"after":"18446744073709551616"})"), "-32602"));

    Caller noScreen(daemon, Sync::AdmissionTarget::Control, { Sync::Scope::Observe, Sync::Scope::Control }, "metadata-only");
    Screen::markInteractiveUser(daemon, noScreen.index);
    noScreen.greet();
    CHECK(has(noScreen.call("host.refresh"), "\"result\""));
    CHECK(has(noScreen.call("host.refresh", "{\"screenPane\":\"" + pane + "\"}"), "-32002"));
    Caller agent(daemon, Sync::AdmissionTarget::Control, { Sync::Scope::Observe, Sync::Scope::Control, Sync::Scope::RegionRead }, "automation");
    agent.greet();
    CHECK(has(agent.call("host.refresh"), "-32002"));
}

void checkBackgroundInventoryRestartsAfterSessionRemoval()
{
    using Ubiquitty::Codec::JSON::Value;
    Daemon daemon;
    std::uint32_t firstID = 0;
    std::string lastPane;
    for (std::size_t index = 0; index < 33; ++index) {
        auto& session = Screen::makeSession(daemon, 1, 1);
        if (!index)
            firstID = session.id;
        const auto pane = session.screenSource->paneID();
        lastPane = Ubiquitty::Session::backgroundPaneID(pane.high, pane.low);
    }
    Caller observer(daemon, Sync::AdmissionTarget::Control, { Sync::Scope::Observe, Sync::Scope::Control }, "background-inventory");
    Screen::markInteractiveUser(daemon, observer.index);
    observer.greet();
    bool isParsed = false;
    const Value first = Ubiquitty::Codec::JSON::parse(observer.call("host.refresh"), isParsed);
    CHECK(isParsed);
    const auto* page = first.find("result");
    CHECK(page);
    if (!page)
        return;
    CHECK(page->numberOr("nextPaneOffset") == 33);
    CHECK(page->find("panes") && page->find("panes")->items().size() == 32);
    const std::string inventory = page->stringOr("inventory");
    Screen::destroySession(daemon, firstID);
    const Value second = Ubiquitty::Codec::JSON::parse(observer.call("host.refresh",
                                                           "{\"inventory\":\"" + inventory + "\",\"paneOffset\":33}"),
        isParsed);
    CHECK(isParsed);
    const auto* restarted = second.find("result");
    CHECK(restarted);
    if (!restarted)
        return;
    CHECK(restarted->stringOr("inventory") != inventory);
    CHECK(restarted->numberOr("nextPaneOffset") == 0);
    const auto* panes = restarted->find("panes");
    CHECK(panes && panes->items().size() == 32);
    if (panes && !panes->items().empty())
        CHECK(panes->items().back().stringOr("paneID") == lastPane);
    CHECK(Screen::connection(daemon, observer.index)->boundPanes().empty());
}

void checkTypedTerminationAuthorityAndConsent()
{
    for (const auto variant : { "manage-human", "manage-agent", "observe-human" }) {
        Daemon daemon;
        const auto sessionID = Screen::makeSession(daemon, 2, 8).id;
        Sync::ScopeSet scope { Sync::Scope::Observe };
        const std::string_view mode(variant);
        if (mode != "observe-human")
            scope.add(Sync::Scope::Manage);
        else
            scope.add(Sync::Scope::Control);
        Caller caller(daemon, mode == "observe-human" ? Sync::AdmissionTarget::Control : Sync::AdmissionTarget::Management, scope, "administrator");
        if (mode != "manage-agent")
            Screen::markInteractiveUser(daemon, caller.index);
        caller.greet();
        CHECK(caller.role() == (mode == "manage-agent" ? Sync::Role::Agent : Sync::Role::Viewer));
        const auto answer = caller.call("host.session.terminate", "{\"session\":" + std::to_string(sessionID) + "}");
        const bool allowed = mode == "manage-human";
        CHECK(has(answer, allowed ? "\"outcome\":\"terminated\"" : "-32002"));
        Screen::settleClosures(daemon);
        CHECK(Screen::hasSession(daemon, sessionID) != allowed);
    }
    // An attached agent keeps its existing exemption, but Control is still
    // mandatory. Neither case sends the retired outer termination frame.
    for (const bool hasControl : { false, true }) {
        Daemon daemon;
        auto& target = Screen::makeSession(daemon, 2, 8);
        const auto sessionID = target.id;
        Sync::ScopeSet scope { Sync::Scope::Observe };
        if (hasControl)
            scope.add(Sync::Scope::Control);
        Caller caller(daemon, sessionID, scope, "attached-terminator");
        Screen::grantAgentSession(daemon, target, caller.index);
        caller.greet();
        const auto answer = caller.call("session.terminate", "{\"session\":" + std::to_string(sessionID) + "}");
        CHECK(has(answer, hasControl ? "\"outcome\":\"terminated\"" : "-32002"));
        Screen::settleClosures(daemon);
        CHECK(Screen::hasSession(daemon, sessionID) != hasControl);
    }
    for (const bool hasStandingGrant : { false, true }) {
        Daemon daemon;
        auto& target = Screen::makeSession(daemon, 2, 8);
        const auto sessionID = target.id;
        Caller agent(daemon, Sync::AdmissionTarget::Control, { Sync::Scope::Observe, Sync::Scope::Control }, "detached-terminator");
        agent.greet();
        if (hasStandingGrant)
            Screen::grantAgentSession(daemon, target, agent.index);
        const auto answer = agent.call("session.terminate", "{\"session\":" + std::to_string(sessionID) + "}");
        CHECK(has(answer, hasStandingGrant ? "\"outcome\":\"terminated\"" : "-32002"));
        Screen::settleClosures(daemon);
        CHECK(Screen::hasSession(daemon, sessionID) != hasStandingGrant);
    }
    for (const bool isHuman : { false, true }) {
        Daemon daemon;
        Screen::setLayoutRecordPath(daemon, "/not-written-by-this-fixture/layout");
        Screen::addRestorable(daemon, 71, {});
        Caller caller(daemon, Sync::AdmissionTarget::Control, { Sync::Scope::Observe, Sync::Scope::Control }, "record-owner");
        if (isHuman)
            Screen::markInteractiveUser(daemon, caller.index);
        caller.greet();
        const auto answer = caller.call("session.terminate", R"({"session":71})");
        CHECK(has(answer, isHuman ? "\"outcome\":\"forgotten\"" : "-32002"));
        CHECK(Screen::isLayoutRecordWriteOwed(daemon) == isHuman);
        if (isHuman)
            CHECK(has(caller.call("session.terminate", R"({"session":71})"), "-32003"));
    }
    for (const auto variant : { "allow", "always", "deny", "timeout", "principal", "scope", "expiry", "lost", "gone", "replacement", "pressure" }) {
        Ubiquitty::PAL::UnixFileDescriptor receiver;
        Daemon daemon;
        const auto sessionID = Screen::makeSession(daemon, 2, 8).id;
        std::array<int, 2> pipe { -1, -1 };
        CHECK(::pipe(pipe.data()) == 0);
        CHECK(::fcntl(pipe[0], F_SETFL, O_NONBLOCK) == 0);
        receiver = Ubiquitty::PAL::UnixFileDescriptor(pipe[0]);
        const auto human = Screen::attach(daemon, sessionID, Ubiquitty::PAL::UnixFileDescriptor(pipe[1]));
        Screen::markTrustedLocal(daemon, human);
        UbiquittyTest::ScreenClient humanClient(2, 8, Sync::ID128 { 71, 72 }, Sync::Role::Viewer);
        Screen::greet(daemon, human, humanClient);
        Caller agent(daemon, Sync::AdmissionTarget::Control, { Sync::Scope::Observe, Sync::Scope::Control }, "exact-principal");
        agent.greet();
        CHECK(agent.client.call("{\"jsonrpc\":\"2.0\",\"id\":\"held-termination\",\"method\":\"session.terminate\",\"params\":{\"session\":" + std::to_string(sessionID) + "}}"));
        Screen::deliver(daemon, agent.index, agent.client);
        CHECK(agent.client.takeResults().empty());
        const auto question = Screen::terminationQuestion(daemon, agent.index);
        CHECK_MSG(question != 0, variant);
        Screen::deliver(daemon, human, humanClient);
        bool sawQuestion = false;
        for (const auto& notification : humanClient.takeNotifies()) {
            const auto prompt = UbiquittyTest::consentQuestionOf(notification);
            if (!prompt)
                continue;
            CHECK(prompt->requestID == question && prompt->sessionID == sessionID && prompt->action == 1);
            sawQuestion = true;
        }
        CHECK(sawQuestion);
        CHECK(Screen::hasSession(daemon, sessionID));
        CHECK(has(agent.call("session.terminate", "{\"session\":" + std::to_string(sessionID) + "}"), "-32005"));
        CHECK_EQ(Screen::terminationQuestion(daemon, agent.index), question);
        const std::string_view mode(variant);
        if (mode == "principal")
            Screen::setTokenID(daemon, agent.index, "replacement-principal");
        if (mode == "scope")
            Screen::grantScope(daemon, agent.index, { Sync::Scope::Observe });
        if (mode == "expiry")
            Screen::setTokenExpiry(daemon, agent.index, 1);
        if (mode == "lost")
            Screen::drop(daemon, agent.index);
        if (mode == "gone" || mode == "replacement")
            Screen::destroySession(daemon, sessionID);
        if (mode == "replacement")
            Screen::makeSession(daemon, 2, 8).id = sessionID;
        if (mode == "timeout")
            Screen::expireTermination(daemon, agent.index);
        const auto decision = Ubiquitty::Protocol::Control::consentResponseRequest(
            { question, Sync::ConsentKind::Terminate, mode == "deny" ? Sync::ConsentChoice::Deny : mode == "always" ? Sync::ConsentChoice::AllowAlways
                                                                                                                    : Sync::ConsentChoice::AllowOnce },
            "termination-answer");
        CHECK(!decision.empty());
        CHECK(humanClient.call(decision));
        // A service pass moves a frame of the agent's queued answer to its
        // output, which reopens the budget. So the budget is filled after the
        // decision has been read and before it is served: the person's
        // connection, attached first, is served first in the pass, while the
        // agent's queue still holds the whole budget.
        std::uint32_t pressuredCall = 0;
        if (mode == "pressure") {
            Screen::receive(daemon, human, humanClient.takeStream(Screen::now()));
            pressuredCall = Screen::fillTerminationReplyCapacity(daemon, agent.index);
            CHECK(pressuredCall);
            CHECK(Screen::connection(daemon, agent.index)->queuedBulkBytes() >= Sync::provisionalLimits.maxQueuedBulkBytes);
        }
        Screen::deliver(daemon, human, humanClient);
        if (mode == "pressure")
            CHECK(Screen::isTerminationCallRetired(daemon, agent.index, pressuredCall));
        Screen::deliver(daemon, agent.index, agent.client);
        auto answers = agent.client.takeResults();
        if (mode == "pressure")
            std::erase_if(answers, [](const std::string& answer) { return answer.find("\"id\":\"held-termination\"") == std::string::npos; });
        if (mode == "lost") {
            CHECK(answers.empty());
        } else {
            CHECK_MSG(answers.size() == 1, variant);
            if (answers.size() == 1) {
                const auto expected = mode == "allow" || mode == "always" ? "terminated"
                    : mode == "deny"                                      ? "denied"
                    : mode == "timeout"                                   ? "timed-out"
                                                                          : "refused";
                const std::string outcome = mode == "pressure" ? "-32005" : std::string("\"outcome\":\"") + expected + "\"";
                CHECK_MSG(has(answers.front(), "\"id\":\"held-termination\"") && has(answers.front(), outcome.c_str()), answers.front().c_str());
            }
        }
        Screen::settleClosures(daemon);
        CHECK(Screen::hasSession(daemon, sessionID) == (mode != "allow" && mode != "always" && mode != "gone"));
        CHECK_EQ(Screen::alwaysAttachGrants(daemon), 0u);
        if (mode == "deny" || mode == "timeout" || mode == "pressure")
            CHECK(agent.isAttached());
        // Another answer cannot spend the same operation or produce a second
        // result, including when timeout and approval race.
        CHECK(humanClient.call(decision));
        Screen::deliver(daemon, human, humanClient);
        Screen::deliver(daemon, agent.index, agent.client);
        CHECK(agent.client.takeResults().empty());
        CHECK_EQ(Screen::terminationQuestion(daemon, agent.index), 0u);
    }
}

} // namespace

int main()
{
    // As the daemon's own process does: a write to a carrier whose reader has
    // gone, a full carrier closed before its daemon, is an error to handle.
    ::signal(SIGPIPE, SIG_IGN);
    checkClearAuthorityAndExactCommand();
    checkClearInputProvenance();
    checkTypedTerminationAuthorityAndConsent();
    checkCaptureTextAndCompleteness();
    checkPassiveBackgroundRefresh();
    checkBackgroundInventoryRestartsAfterSessionRemoval();
    Daemon d;
    auto& session = Screen::makeSession(d, 24, 80);

    // 1. A person whose token carries Control: the host answers a call that
    // names no pane.
    Caller controller(d, session.id, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::RegionRead }, "controller");
    Screen::markInteractiveUser(d, controller.index);
    controller.greet();
    CHECK_MSG(controller.role() == Sync::Role::Driver, "a person whose token may drive is not attached as a driver");
    const std::string listed = controller.call("host.methods");
    CHECK_MSG(has(listed, "\"result\"") && has(listed, "host.methods") && has(listed, "host.describe") && has(listed, "pane.pinGeometry") && has(listed, "pane.capture"), listed.c_str());
    const std::string described = controller.call("host.describe");
    CHECK_MSG(has(described, "\"result\"") && has(described, "\"wireVersion\":") && has(described, "\"appVersion\""), described.c_str());
    // It says what the caller's token carries, so a client can tell what it
    // may do before it does any of it.
    CHECK_MSG(has(described, "\"scopes\":[\"observe\",\"drive\",\"control\",\"region\"]"), described.c_str());

    // Parameters outside the entry's schema, refused before any effect; a
    // pane call that names no pane; and a method the host does not serve.
    const std::string extra = controller.call("host.methods", "{\"pane\":1}");
    CHECK_MSG(has(extra, "-32602") && has(extra, "host.methods"), extra.c_str());
    const std::string paneless = controller.call("pane.capture");
    CHECK_MSG(has(paneless, "-32602") && has(paneless, "pane"), paneless.c_str());
    const std::string unknown = controller.call("host.shutdown");
    CHECK_MSG(has(unknown, "-32601"), unknown.c_str());
    const std::string localOnly = controller.call("ls");
    CHECK_MSG(has(localOnly, "-32601"), "a control-socket method was served to a Sync connection");
    CHECK_MSG(controller.isAttached(), "a refused call closed the caller's connection");

    // 2. No Control scope: refused with the scope named, still attached, and
    // still served what its token does carry.
    Caller driver(d, session.id, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::RegionRead }, "driver");
    Screen::markInteractiveUser(d, driver.index);
    driver.greet();
    const std::string refused = driver.call("host.methods");
    CHECK_MSG(has(refused, "-32002") && has(refused, "control") && has(refused, "host.methods"), refused.c_str());
    CHECK_MSG(driver.isAttached(), "a caller refused for its scope was closed");
    const std::string captured = driver.call("pane.capture", "{\"pane\":1}");
    CHECK_MSG(has(captured, "\"result\"") && has(captured, "\"text\""), captured.c_str());

    // 3. A token that only observes attaches a viewer, refused what drives a
    // pane for its role.
    Caller viewer(d, session.id, { Sync::Scope::Observe, Sync::Scope::Control }, "viewer");
    Screen::markInteractiveUser(d, viewer.index);
    viewer.greet();
    CHECK_MSG(viewer.role() == Sync::Role::Viewer, "a person whose token only observes is not attached as a viewer");
    // The caller offers only its own clipboard. Observe is enough to give
    // that opt-in; it does not acquire anyone else's clipboard or Drive.
    CHECK(!Screen::servesClipboard(d, viewer.index));
    CHECK(!Screen::servesClipboard(d, controller.index));
    CHECK(has(viewer.call("client.clipboardServing", R"({"enabled":true})"), R"("enabled":true)"));
    CHECK(Screen::servesClipboard(d, viewer.index));
    CHECK(!Screen::servesClipboard(d, controller.index));
    for (const auto* parameters : { R"({})", R"({"enabled":1})", R"({"enabled":"true"})", R"({"enabled":false,"client":1})" }) {
        CHECK(has(viewer.call("client.clipboardServing", parameters), "-32602"));
        CHECK(Screen::servesClipboard(d, viewer.index));
        CHECK(!Screen::servesClipboard(d, controller.index));
    }
    CHECK(has(viewer.call("client.clipboardServing", R"({"enabled":false})"), R"("enabled":false)"));
    CHECK(!Screen::servesClipboard(d, viewer.index));
    const std::string pin = viewer.call("pane.pinGeometry", "{\"pane\":1,\"rows\":10,\"columns\":40}");
    CHECK_MSG(has(pin, "-32002") && has(pin, "viewer"), pin.c_str());
    const std::string viewerListed = viewer.call("host.methods");
    CHECK_MSG(has(viewerListed, "pane.search") && !has(viewerListed, "pane.capture") && !has(viewerListed, "pane.pinGeometry"), "a viewer was listed a method its role or scope refuses");
    const std::string captureRefused = viewer.call("pane.capture", "{\"pane\":1}");
    CHECK_MSG(has(captureRefused, "-32002") && has(captureRefused, "pane.capture") && has(captureRefused, "region"), captureRefused.c_str());
    CHECK_MSG(!has(captureRefused, "\"result\"") && !has(captureRefused, "\"text\""), "a scope-refused capture disclosed screen text");
    for (const auto* params : { R"({"pane":1,"source":"last-command"})", R"({"pane":1,"source":"rows","start":0,"end":1})", R"({"pane":1,"source":"tail","lines":1})" }) {
        const auto denied = viewer.call("pane.capture", params);
        CHECK(has(denied, "-32002") && !has(denied, "\"text\"") && !has(denied, "\"result\""));
    }
    const std::string afterRefusal = viewer.call("host.describe");
    CHECK_MSG(has(afterRefusal, "\"result\""), "a refused capture prevented a permitted call");
    CHECK_MSG(viewer.isAttached(), "a caller refused for its role was closed");

    // 4. Automation is an agent whatever its Greet asked to be.
    Caller agent(d, session.id, { Sync::Scope::Observe, Sync::Scope::Drive, Sync::Scope::Control, Sync::Scope::RegionRead }, "agent");
    Screen::grantAgentSession(d, session, agent.index);
    agent.greet();
    CHECK_MSG(agent.role() == Sync::Role::Agent, "a client the daemon verifies as automation was granted a person's role");
    const std::string agentListed = agent.call("host.methods");
    CHECK_MSG(has(agentListed, "\"result\"") && has(agentListed, "pane.pinGeometry"), agentListed.c_str());
    const std::string agentCapture = agent.call("pane.capture", "{\"pane\":1}");
    CHECK_MSG(has(agentCapture, "\"result\"") && has(agentCapture, "\"text\""), agentCapture.c_str());
    CHECK_MSG(agent.isAttached(), "an agent's authorized capture ended its attachment");

    // 5. A connection bound to no session, as a directory errand's is: it is
    // welcomed, holds no pane, and is served the host's methods.
    Caller directory(d, Sync::AdmissionTarget::Control, { Sync::Scope::Observe, Sync::Scope::Control, Sync::Scope::RegionRead }, "directory");
    Screen::markInteractiveUser(d, directory.index);
    directory.greet();
    CHECK_MSG(directory.isAttached(), "a connection bound to no session was not welcomed");
    CHECK_MSG(Screen::connection(d, directory.index)->boundPanes().empty(), "a connection bound to no session was bound a pane");
    const std::string directoryListed = directory.call("host.methods");
    CHECK_MSG(has(directoryListed, "\"result\"") && has(directoryListed, "host.describe"), directoryListed.c_str());
    const std::string nothingBound = directory.call("pane.capture", "{\"pane\":1}");
    CHECK_MSG(has(nothingBound, "no pane is bound"), nothingBound.c_str());

    // RegionRead does not grant input authority or upgrade a viewer's role.
    Caller regionReader(d, session.id, { Sync::Scope::Observe, Sync::Scope::RegionRead }, "region-reader");
    Screen::markInteractiveUser(d, regionReader.index);
    regionReader.greet();
    CHECK_MSG(regionReader.role() == Sync::Role::Viewer, "RegionRead upgraded a viewer to a driver");
    const std::string regionCapture = regionReader.call("pane.capture", "{\"pane\":1}");
    CHECK_MSG(has(regionCapture, "\"result\"") && has(regionCapture, "\"text\""), regionCapture.c_str());
    const std::string regionPin = regionReader.call("pane.pinGeometry", "{\"pane\":1,\"rows\":10,\"columns\":40}");
    CHECK_MSG(has(regionPin, "-32002") && has(regionPin, "viewer"), regionPin.c_str());
    CHECK_MSG(regionReader.isAttached(), "a viewer granted text reads lost its connection");

    Screen::setTokenExpiry(d, regionReader.index, 1);
    CHECK(has(regionReader.call("client.clipboardServing", R"({"enabled":true})"), "-32002"));
    CHECK(!Screen::servesClipboard(d, regionReader.index));

    if (failures == 0)
        std::printf("sync_host_calls: a call with no pane reaches the host; a caller is refused by role, then where it stands, then scope, then parameters, each with its reason, and stays attached\n");
    return failures == 0 ? 0 : 1;
}
