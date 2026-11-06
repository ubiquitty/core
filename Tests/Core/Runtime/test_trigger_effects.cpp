// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_trigger_effects.cpp — what a matched trigger asks a frontend to do, and
// the session's queue that carries it to a frontend installing no client.
//
// A trigger matches attacker-controllable output: whatever the program printed.
// Two of its acts reach beyond the terminal, and each is contained twice — a
// `[triggers]` gate closed by default, and a shape the match cannot escape: one
// argv element for `run_command`, one JSON value for `webhook`. Those are the
// cases that carry the weight here, each put to both answers, because a gate
// checked only in the direction that passes is a gate nobody has seen close.
//
// The session half pins the route: a Swift frontend cannot subclass a C++
// client, so without the queue every `notify`, `copy`, `set_title`,
// `run_command` and `webhook` rule is evaluated and dropped on that arm.

#include "Core/Preferences/Preferences.h"
#include "Core/Runtime/TerminalEvents.h"
#include "Core/Runtime/TerminalSession.h"
#include "Core/Runtime/TriggerEffect.h"
#include "Core/Terminal.h"
#include "Core/Triggers/Engine.h"
#include "PAL/TerminalClients.h"

#include <memory>
#include <string>
#include <vector>

using Ubiquitty::Runtime::EventKind;
using Ubiquitty::Runtime::resolveTriggerEffect;
using Ubiquitty::Runtime::TriggerEffect;
using Ubiquitty::Runtime::TriggerEffectKind;
using Ubiquitty::Triggers::Action;
using Session = Ubiquitty::Runtime::TerminalSession;
namespace Cfg = Ubiquitty::Preferences;

namespace {

/// The action as the engine reports it.
std::uint8_t wire(Action action)
{
    return static_cast<std::uint8_t>(action);
}

/// A configuration with both reach-beyond gates set as asked.
///
/// - Parameters:
///   - runCommand: What `allow_run_command` holds.
///   - webhook: What `allow_webhook` holds.
Cfg::Values gates(Cfg::OptionalBool runCommand, Cfg::OptionalBool webhook)
{
    Cfg::Values values;
    values.allowRunCommand = runCommand;
    values.allowWebhook = webhook;
    return values;
}

/// A trigger rule as the config spells it.
Cfg::TriggerEntry rule(const char* pattern, const char* action)
{
    Cfg::TriggerEntry entry;
    entry.pattern = pattern;
    entry.action = action;
    return entry;
}

/// Feeds a string to the session as though a program had written it.
void feed(Session* session, const std::string& bytes)
{
    session->feedBytes(Ubiquitty::Runtime::ByteSpan(bytes.data(), bytes.size()));
}

/// Every act the queue holds, drained under one configuration.
std::vector<TriggerEffect> drain(Session* session, const Cfg::Values& configuration)
{
    std::vector<TriggerEffect> taken;
    for (;;) {
        TriggerEffect effect = session->takeTriggerEffect(configuration);
        if (effect.kind == TriggerEffectKind::None)
            break;
        taken.push_back(std::move(effect));
    }
    return taken;
}

/// A client that records, standing in for a frontend that installs its own.
struct RecordingTriggerClient final : Ubiquitty::TriggerClient {
    int calls = 0;
    void onTrigger(std::uint8_t, std::string_view, std::string_view, int, int) override { ++calls; }
};

/// What an attacker who controls the output would print to escape a shell.
constexpr const char* hostileMatch = "x; rm -rf ~ | $(curl evil) `id` \"q\"\nnext";

} // namespace

/// A `run_command` rule spawns nothing until the reader opens its gate.
///
/// Closed is the default, and a default nobody wrote down is a gate that was
/// never closed — so the unset key is put to it as well as the explicit false.
TEST(aRunCommandTriggerResolvesToNothingUntilItsGateIsOpen)
{
    for (const Cfg::Values& closed : { gates(Cfg::OptionalBool(), Cfg::OptionalBool()), gates(Cfg::OptionalBool(false), Cfg::OptionalBool()) }) {
        const TriggerEffect effect = resolveTriggerEffect(wire(Action::RunCommand), "notify-send %s", "BUILD FAILED", closed);
        CHECK(effect.kind == TriggerEffectKind::None);
        CHECK_EQ(effect.argumentCount(), 0);
    }

    const TriggerEffect open = resolveTriggerEffect(wire(Action::RunCommand), "notify-send %s", "BUILD FAILED", gates(Cfg::OptionalBool(true), Cfg::OptionalBool()));
    CHECK(open.kind == TriggerEffectKind::RunCommand);
    CHECK_EQ(open.argumentCount(), 2);
    CHECK(open.argumentAt(0) == "notify-send");
    CHECK(open.argumentAt(1) == "BUILD FAILED");
}

/// The match is one argument, whatever it holds.
///
/// There is no shell on this path, so the containment is the argv's shape: the
/// template splits into words, the match never does. A match carrying every
/// metacharacter a shell reads arrives as exactly the element it replaced.
TEST(aRunCommandTriggerCarriesTheMatchAsOneInertArgument)
{
    const Cfg::Values open = gates(Cfg::OptionalBool(true), Cfg::OptionalBool());
    const TriggerEffect effect = resolveTriggerEffect(wire(Action::RunCommand), "/usr/bin/logger -t ubiquitty %s %%", hostileMatch, open);
    CHECK(effect.kind == TriggerEffectKind::RunCommand);
    CHECK_EQ(effect.argumentCount(), 5);
    if (effect.argumentCount() == 5) {
        CHECK(effect.argumentAt(0) == "/usr/bin/logger");
        CHECK(effect.argumentAt(1) == "-t");
        CHECK(effect.argumentAt(2) == "ubiquitty");
        CHECK(effect.argumentAt(3) == hostileMatch);
        CHECK(effect.argumentAt(4) == "%");
    }
    // Out of range answers empty rather than reading past the vector.
    CHECK(effect.argumentAt(5).empty());
    CHECK(effect.argumentAt(-1).empty());

    // A template that names no program names nothing to spawn, gate or not.
    CHECK(resolveTriggerEffect(wire(Action::RunCommand), "   ", hostileMatch, open).kind == TriggerEffectKind::None);
}

/// A `webhook` rule sends nothing until its gate is open, and only over http(s).
///
/// Both halves, because each refuses a different thing: the gate refuses a rule
/// the reader never opened, the scheme refuses a URL that would make a webhook
/// a local opener.
TEST(aWebhookTriggerResolvesToNothingUntilItsGateIsOpenAndOnlyOverHTTP)
{
    const char* url = "https://hooks.example.test/build";
    CHECK(resolveTriggerEffect(wire(Action::Webhook), url, "BUILD FAILED", gates(Cfg::OptionalBool(), Cfg::OptionalBool())).kind == TriggerEffectKind::None);
    CHECK(resolveTriggerEffect(wire(Action::Webhook), url, "BUILD FAILED", gates(Cfg::OptionalBool(), Cfg::OptionalBool(false))).kind == TriggerEffectKind::None);
    // The run_command gate does not open this one.
    CHECK(resolveTriggerEffect(wire(Action::Webhook), url, "BUILD FAILED", gates(Cfg::OptionalBool(true), Cfg::OptionalBool())).kind == TriggerEffectKind::None);

    const Cfg::Values open = gates(Cfg::OptionalBool(), Cfg::OptionalBool(true));
    for (const char* refused : { "file:///etc/passwd", "x-apple-helpviewer://open", "ssh://host", "https:", "" })
        CHECK(resolveTriggerEffect(wire(Action::Webhook), refused, "BUILD FAILED", open).kind == TriggerEffectKind::None);

    const TriggerEffect effect = resolveTriggerEffect(wire(Action::Webhook), url, hostileMatch, open);
    CHECK(effect.kind == TriggerEffectKind::Webhook);
    CHECK(effect.url == url);
    // The match is a JSON value: its quote is escaped, its newline is escaped,
    // and nothing of it reaches the URL.
    CHECK(effect.requestBody == "{\"match\":\"x; rm -rf ~ | $(curl evil) `id` \\\"q\\\"\\nnext\"}");
    CHECK(effect.url.find("rm -rf") == std::string::npos);
    // Nor does the run_command gate open a spawn on this rule's behalf.
    CHECK_EQ(effect.argumentCount(), 0);
}

/// `set_title` names the window with the rule's text, or with the match when
/// the rule gave none.
TEST(aSetTitleTriggerFallsBackToTheMatch)
{
    const Cfg::Values closed;
    const TriggerEffect literal = resolveTriggerEffect(wire(Action::SetTitle), "Deploying", "deploy started", closed);
    CHECK(literal.kind == TriggerEffectKind::SetTitle);
    CHECK(literal.title == "Deploying");

    const TriggerEffect fallback = resolveTriggerEffect(wire(Action::SetTitle), "", "deploy started", closed);
    CHECK(fallback.kind == TriggerEffectKind::SetTitle);
    CHECK(fallback.title == "deploy started");

    // Nothing to name the window with is nothing to do.
    CHECK(resolveTriggerEffect(wire(Action::SetTitle), "", "", closed).kind == TriggerEffectKind::None);
}

/// `notify` heads with the rule's text over the match, and promotes a bare
/// match into the heading, by the policy OSC 9 banners already take.
TEST(aNotifyTriggerHeadsWithItsTextOrTheMatch)
{
    const Cfg::Values closed;
    const TriggerEffect titled = resolveTriggerEffect(wire(Action::Notify), "Build", "BUILD FAILED", closed);
    CHECK(titled.kind == TriggerEffectKind::Notify);
    CHECK(titled.title == "Build");
    CHECK(titled.body == "BUILD FAILED");

    const TriggerEffect bare = resolveTriggerEffect(wire(Action::Notify), "", "BUILD FAILED", closed);
    CHECK(bare.kind == TriggerEffectKind::Notify);
    CHECK(bare.title == "BUILD FAILED");
    CHECK(bare.body.empty());
}

/// `copy` carries the match, and neither gate stands in its way.
TEST(aCopyTriggerCarriesTheMatch)
{
    const TriggerEffect effect = resolveTriggerEffect(wire(Action::Copy), "", hostileMatch, Cfg::Values());
    CHECK(effect.kind == TriggerEffectKind::Copy);
    CHECK(effect.text == hostileMatch);
}

/// Highlight is painted, so nothing is dispatched for it, and a value that names
/// no action dispatches nothing either.
TEST(aHighlightTriggerDispatchesNothing)
{
    CHECK(!Ubiquitty::Runtime::doesTriggerActionDispatch(wire(Action::Highlight)));
    CHECK(!Ubiquitty::Runtime::doesTriggerActionDispatch(wire(Action::None)));
    CHECK(!Ubiquitty::Runtime::doesTriggerActionDispatch(200));
    for (const Action action : { Action::Notify, Action::Copy, Action::SetTitle, Action::RunCommand, Action::Webhook })
        CHECK(Ubiquitty::Runtime::doesTriggerActionDispatch(wire(action)));

    const Cfg::Values open = gates(Cfg::OptionalBool(true), Cfg::OptionalBool(true));
    CHECK(resolveTriggerEffect(wire(Action::Highlight), "red", "BUILD FAILED", open).kind == TriggerEffectKind::None);
    CHECK(resolveTriggerEffect(200, "anything", "BUILD FAILED", open).kind == TriggerEffectKind::None);
}

/// A matched line reaches a frontend that installs no client, as the act it
/// resolves to, with the event that says to look.
TEST(theTriggerQueueCarriesAMatchedLineToTheFrontend)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    CHECK(session->installTriggerQueue());

    Cfg::Values configuration;
    configuration.triggers.push_back(rule("TOKEN-[0-9]+", "copy"));
    configuration.triggers.push_back(rule("deploy [a-z]+", "set_title"));
    configuration.triggers.push_back(rule("FAILED", "highlight:red"));
    CHECK(session->applyTriggerConfiguration(configuration).empty());

    const auto bit = 1u << static_cast<unsigned>(EventKind::TriggerMatched);
    (void)session->takeEventFlags();

    // A line nothing matches raises nothing.
    feed(session.get(), "nothing to see\r\n");
    CHECK((session->takeEventFlags() & bit) == 0);
    CHECK(!session->hasPendingTriggerMatch());

    // A highlight is painted rather than dispatched, so a line only it matches
    // wakes nobody.
    feed(session.get(), "tests FAILED\r\n");
    CHECK((session->takeEventFlags() & bit) == 0);
    CHECK(!session->hasPendingTriggerMatch());

    feed(session.get(), "issued TOKEN-4242 for you\r\n");
    feed(session.get(), "deploy started\r\n");
    CHECK((session->takeEventFlags() & bit) != 0);

    const std::vector<TriggerEffect> taken = drain(session.get(), configuration);
    CHECK_EQ(taken.size(), std::size_t { 2 });
    if (taken.size() == 2) {
        CHECK(taken[0].kind == TriggerEffectKind::Copy);
        CHECK(taken[0].text == "TOKEN-4242");
        CHECK(taken[1].kind == TriggerEffectKind::SetTitle);
        CHECK(taken[1].title == "deploy started");
    }
    CHECK(!session->hasPendingTriggerMatch());
}

/// The gates are read when a report is taken, not when it was queued.
///
/// A report queued while `allow_run_command` was open and taken after the
/// reader closed it must not spawn: the configuration that answers is the one
/// standing when the frontend acts.
TEST(theTriggerQueueAsksTheGatesWhenAReportIsTaken)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    CHECK(session->installTriggerQueue());

    Cfg::Values configuration = gates(Cfg::OptionalBool(true), Cfg::OptionalBool());
    configuration.triggers.push_back(rule("BUILD FAILED", "run_command:/usr/bin/true %s"));
    configuration.triggers.push_back(rule("TOKEN-[0-9]+", "copy"));
    CHECK(session->applyTriggerConfiguration(configuration).empty());

    feed(session.get(), "BUILD FAILED\r\n");
    feed(session.get(), "TOKEN-7\r\n");
    CHECK(session->hasPendingTriggerMatch());

    // Closed now: the spawn is consumed and skipped, and the copy behind it is
    // still delivered rather than lost with it.
    const std::vector<TriggerEffect> closed = drain(session.get(), gates(Cfg::OptionalBool(false), Cfg::OptionalBool()));
    CHECK_EQ(closed.size(), std::size_t { 1 });
    if (closed.size() == 1)
        CHECK(closed[0].kind == TriggerEffectKind::Copy);

    // And the other direction, which is what keeps the case above from passing
    // on a queue that never carried the spawn at all.
    feed(session.get(), "BUILD FAILED\r\n");
    const std::vector<TriggerEffect> open = drain(session.get(), configuration);
    CHECK_EQ(open.size(), std::size_t { 1 });
    if (open.size() == 1) {
        CHECK(open[0].kind == TriggerEffectKind::RunCommand);
        CHECK_EQ(open[0].argumentCount(), 2);
        CHECK(open[0].argumentAt(0) == "/usr/bin/true");
        CHECK(open[0].argumentAt(1) == "BUILD FAILED");
    }
}

/// A program cannot grow the queue without bound.
TEST(theTriggerQueueRefusesToGrowPastItsBound)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    CHECK(session->installTriggerQueue());

    Cfg::Values configuration;
    configuration.triggers.push_back(rule("TOKEN-[0-9]+", "copy"));
    CHECK(session->applyTriggerConfiguration(configuration).empty());

    std::string flood;
    for (std::size_t index = 0; index < Session::triggerQueueLimit * 2; ++index)
        flood += "TOKEN-" + std::to_string(index) + "\r\n";
    feed(session.get(), flood);

    const std::vector<TriggerEffect> taken = drain(session.get(), configuration);
    CHECK_EQ(taken.size(), Session::triggerQueueLimit);
    // The ones kept are the first ones: dropping the oldest would reorder what
    // survives against what the program wrote.
    if (!taken.empty())
        CHECK(taken[0].text == "TOKEN-0");
}

/// A reload replaces the rules rather than appending to them.
TEST(applyingTriggerConfigurationReplacesTheRules)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);
    CHECK(session->installTriggerQueue());

    Cfg::Values first;
    first.triggers.push_back(rule("alpha", "copy"));
    CHECK(session->applyTriggerConfiguration(first).empty());
    Cfg::Values second;
    second.triggers.push_back(rule("beta", "copy"));
    CHECK(session->applyTriggerConfiguration(second).empty());

    feed(session.get(), "alpha beta\r\n");
    const std::vector<TriggerEffect> taken = drain(session.get(), second);
    CHECK_EQ(taken.size(), std::size_t { 1 });
    if (taken.size() == 1)
        CHECK(taken[0].text == "beta");

    // A refused rule is reported, so a reload that installed less than it was
    // given says so.
    Cfg::Values malformed;
    malformed.triggers.push_back(rule("gamma", "no_such_action"));
    CHECK(!session->applyTriggerConfiguration(malformed).empty());
}

/// The queue never displaces a frontend that installed its own client.
TEST(theTriggerQueueYieldsToAFrontendThatInstalledItsOwnClient)
{
    const std::unique_ptr<Session, decltype(&releaseTerminalSession)> session(Session::make(), releaseTerminalSession);
    REQUIRE(session != nullptr);
    session->start(24, 80);

    Ubiquitty::Terminal* terminal = session->terminal();
    REQUIRE(terminal != nullptr);
    // The session installed its queue at start; a frontend with its own client
    // replaces it by assignment, and asking again must not take it back.
    RecordingTriggerClient client;
    terminal->triggerClient = &client;
    CHECK(!session->installTriggerQueue());

    Cfg::Values configuration;
    configuration.triggers.push_back(rule("TOKEN-[0-9]+", "copy"));
    CHECK(session->applyTriggerConfiguration(configuration).empty());
    feed(session.get(), "TOKEN-9\r\n");
    CHECK_EQ(client.calls, 1);
    CHECK(!session->hasPendingTriggerMatch());

    terminal->triggerClient = nullptr;
}

UBIQUITTY_TEST_MAIN()
