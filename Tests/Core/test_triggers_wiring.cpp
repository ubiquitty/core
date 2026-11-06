// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Triggers/Triggers.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

// test_triggers_wiring — the Triggers engine wired into VT100's line-commit
// seam + the characterClass word-selection gate. The ENGINE itself
// (regex compile/match, rule ordering, expandSelection) is verified in
// Tests/Core/test_triggers.cpp; this test pins the WIRING through a live Terminal:
//
//   - a configured trigger fires its action (dispatched to a TriggerClient)
//     when a completed line matches;
//   - an empty trigger set is a no-op (no engine, nothing dispatched) — the
//     byte-hot line-feed path stays cheap;
//   - the config action-SPEC parser (addTriggerRuleSpec) maps notify/copy/...;
//   - a [characterClass] override changes word selection (triggerExpandWordSelection
//     reports a span) while the default (no characterClass) leaves it untouched, so
//     the caller keeps today's word walk.

#include "Core/TerminalOperations.h"
#include "Core/Triggers/Engine.h"

#include <cstdio>
#include <cstring>
#include <optional>

// Records the last dispatched trigger so the test can assert the wiring.
struct CaptureTrigger : public Ubiquitty::TriggerClient {
    int calls = 0;
    uint8_t lastAction = 0;
    char lastArgument[64] = { 0 };
    char lastMatch[128] = { 0 };
    int lastColumn0 = -1;
    int lastColumn1 = -1;

    void onTrigger(uint8_t action, std::string_view arg, std::string_view matchedText, int column0, int column1) override
    {
        ++calls;
        lastAction = action;
        int an = arg.size() < sizeof(lastArgument) - 1 ? static_cast<int>(arg.size()) : static_cast<int>(sizeof(lastArgument) - 1);
        memcpy(lastArgument, arg.data(), static_cast<size_t>(an));
        lastArgument[an] = 0;
        int mn = matchedText.size() < sizeof(lastMatch) - 1 ? static_cast<int>(matchedText.size()) : static_cast<int>(sizeof(lastMatch) - 1);
        memcpy(lastMatch, matchedText.data(), static_cast<size_t>(mn));
        lastMatch[mn] = 0;
        lastColumn0 = column0;
        lastColumn1 = column1;
    }
};

// A configured trigger fires its action on a matching completed line. The line
// is completed by the CR/LF that the linefeed seam evaluates.
TEST(testTriggerFiresOnMatchingLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    CaptureTrigger trig;
    t.triggerClient = &trig;

    CHECK(Ubiquitty::Triggers::addRule(t, "[0-9]+ tests? passed", static_cast<uint8_t>(Ubiquitty::Triggers::Action::Notify), "Tests", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(Ubiquitty::Triggers::ruleCount(&t) == 1);

    // No match yet on a non-matching line.
    feed(t, "building...\r\n");
    CHECK(trig.calls == 0);

    // A matching line, completed by the LF, fires the rule once.
    feed(t, "42 tests passed\r\n");
    CHECK(trig.calls == 1);
    CHECK(trig.lastAction == static_cast<uint8_t>(Ubiquitty::Triggers::Action::Notify));
    CHECK(strcmp(trig.lastArgument, "Tests") == 0);
    CHECK(strcmp(trig.lastMatch, "42 tests passed") == 0);
    CHECK(trig.lastColumn0 == 0);
    CHECK(trig.lastColumn1 == 14);

    t.triggerClient = nullptr; // clear the CheckedPtr before the stub destructs
}

// An empty trigger set is a no-op: with no rules configured the engine is never
// allocated, the per-line hook early-returns, and nothing is dispatched.
TEST(testEmptyTriggerSetIsNoop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    CaptureTrigger trig;
    t.triggerClient = &trig;

    CHECK(Ubiquitty::Triggers::ruleCount(&t) == 0); // no engine allocated

    feed(t, "42 tests passed\r\n");
    feed(t, "ERROR: boom\r\n");
    CHECK(trig.calls == 0); // nothing fires

    t.triggerClient = nullptr; // clear the CheckedPtr before the stub destructs
}

// The config action-SPEC parser maps the spelled-out [triggers] action names to
// the engine's Action, splitting "<action>:<arg>" on the first ':'.
TEST(testActionSpecParsing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    CaptureTrigger trig;
    t.triggerClient = &trig;

    CHECK(Ubiquitty::Triggers::addRuleSpec(&t, "deploy [a-z]+", "run_command:notify-deploy", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(Ubiquitty::Triggers::addRuleSpec(&t, "copy me", "copy", Ubiquitty::CaseSensitivity::Sensitive)); // no arg
    CHECK(!Ubiquitty::Triggers::addRuleSpec(&t, "x", "bogus_action", Ubiquitty::CaseSensitivity::Sensitive)); // unknown → rejected
    CHECK(Ubiquitty::Triggers::ruleCount(&t) == 2);

    feed(t, "deploy prod\r\n");
    CHECK(trig.calls == 1);
    CHECK(trig.lastAction == static_cast<uint8_t>(Ubiquitty::Triggers::Action::RunCommand));
    CHECK(strcmp(trig.lastArgument, "notify-deploy") == 0);

    feed(t, "copy me\r\n");
    CHECK(trig.calls == 2);
    CHECK(trig.lastAction == static_cast<uint8_t>(Ubiquitty::Triggers::Action::Copy));
    CHECK(trig.lastArgument[0] == 0); // empty arg

    t.triggerClient = nullptr; // clear the CheckedPtr before the stub destructs
}

// ExecuteTrigger: a RunCommand trigger spawns a host command from matched
// OUTPUT — arbitrary host exec, the top-severity capability. An installed policy
// gates it: deny drops the run before it reaches the client; allow dispatches.
struct GateExecPolicy : public Ubiquitty::CapabilityPolicy {
    Ubiquitty::CapabilityDecision decision = Ubiquitty::CapabilityDecision::Deny;
    Ubiquitty::Capability lastCapability {};
    Ubiquitty::CapabilityOrigin lastOrigin {};
    bool called = false;
    Ubiquitty::CapabilityDecision check(Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin origin, std::string_view) override
    {
        called = true;
        lastCapability = capability;
        lastOrigin = origin;
        return decision;
    }
};

TEST(testRunCommandGatedByCapabilityPolicy)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 5, 40);
        CaptureTrigger trig;
        GateExecPolicy policy;
        t.triggerClient = &trig;
        t.capabilityPolicy = &policy;
        policy.decision = Ubiquitty::CapabilityDecision::Deny;
        CHECK(Ubiquitty::Triggers::addRuleSpec(&t, "deploy [a-z]+", "run_command:do-it", Ubiquitty::CaseSensitivity::Sensitive));
        feed(t, "deploy prod\r\n");
        CHECK(policy.called);
        CHECK(policy.lastCapability == Ubiquitty::Capability::ExecuteTrigger);
        // Origin axis: the gate reads the Terminal's origin (default Local).
        CHECK(policy.lastOrigin == Ubiquitty::CapabilityOrigin::Local);
        CHECK(trig.calls == 0); // denied — the run never reached the client
        t.capabilityPolicy = nullptr;
        t.triggerClient = nullptr;
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 5, 40);
        CaptureTrigger trig;
        GateExecPolicy policy;
        t.triggerClient = &trig;
        t.capabilityPolicy = &policy;
        policy.decision = Ubiquitty::CapabilityDecision::Allow;
        // A stamped non-Local origin threads through to the ExecuteTrigger gate.
        t.currentCapabilityOrigin = Ubiquitty::CapabilityOrigin::Agent;
        CHECK(Ubiquitty::Triggers::addRuleSpec(&t, "deploy [a-z]+", "run_command:do-it", Ubiquitty::CaseSensitivity::Sensitive));
        feed(t, "deploy prod\r\n");
        CHECK(policy.lastOrigin == Ubiquitty::CapabilityOrigin::Agent);
        CHECK(trig.calls == 1); // allowed — dispatched
        CHECK(trig.lastAction == static_cast<uint8_t>(Ubiquitty::Triggers::Action::RunCommand));
        t.capabilityPolicy = nullptr;
        t.triggerClient = nullptr;
    }
}

// A [characterClass] override changes word selection: triggerExpandWordSelection
// reports the widest token a configured class matches. With no characterClass the
// gate returns false and the span is untouched, so the caller keeps today's
// default word walk.
TEST(testCharclassChangesWordSelection)
{
    const char* line = "open /usr/local/bin here";
    const int length = static_cast<int>(strlen(line));

    // Default: no characterClass configured → gate returns false, span untouched.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        CHECK(Ubiquitty::Triggers::characterClassCount(&t) == 0);
        CHECK(!Ubiquitty::Triggers::expandWordSelection(&t, { line, static_cast<std::size_t>(length) }, 11));
        // Nothing to leave untouched: the caller runs its default word walk.
    }

    // With a [characterClass] path override the click grows to the whole path token.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        CHECK(Ubiquitty::Triggers::addCharacterClass(t, "path", "[A-Za-z0-9_./-]+", Ubiquitty::CaseSensitivity::Sensitive));
        CHECK(Ubiquitty::Triggers::characterClassCount(&t) == 1);
        const std::optional<Ubiquitty::Triggers::MatchSpan> span = Ubiquitty::Triggers::expandWordSelection(&t, { line, static_cast<std::size_t>(length) }, 11);
        CHECK(span);
        CHECK(span && span->begin == 5); // start of "/usr/local/bin"
        CHECK(span && span->end == 18); // last char before the space
        CHECK(span && strncmp(line + span->begin, "/usr/local/bin", 14) == 0);
    }
}

// A `highlight` trigger records the matched span on the Terminal's highlight ring
// (absolute row + inclusive byte columns) so the renderer can paint it. The
// span must be the exact matched columns on the right absolute row.
TEST(testHighlightRecordsSpan)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    CaptureTrigger trig;
    t.triggerClient = &trig;

    // Match "ERROR" anywhere on the line and highlight it.
    CHECK(Ubiquitty::Triggers::addRuleSpec(&t, "ERROR", "highlight", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(Ubiquitty::Triggers::highlightCount(&t) == 0); // nothing yet

    // A non-matching line records nothing.
    feed(t, "all good\r\n");
    CHECK(Ubiquitty::Triggers::highlightCount(&t) == 0);

    // "  ERROR here" — ERROR starts at column 2, ends at column 6 (inclusive).
    feed(t, "  ERROR here\r\n");
    CHECK(Ubiquitty::Triggers::highlightCount(&t) == 1);
    int absoluteRow = -1, c0 = -1, c1 = -1;
    CHECK(Ubiquitty::Triggers::highlightAt(&t, 0, &absoluteRow, &c0, &c1));
    CHECK(c0 == 2); // start of "ERROR"
    CHECK(c1 == 6); // last char of "ERROR" (inclusive)
    // The row was the live cursor row at commit; with no scrollback yet it is
    // the screen row the text landed on (row 1 — row 0 held "all good").
    CHECK(absoluteRow == 1);

    // The dispatch seam also still fires for the highlight (so a frontend that
    // wants to react to it can), with the same span.
    CHECK(trig.calls == 1);
    CHECK(trig.lastAction == static_cast<uint8_t>(Ubiquitty::Triggers::Action::Highlight));
    CHECK(trig.lastColumn0 == 2 && trig.lastColumn1 == 6);
    CHECK(strcmp(trig.lastMatch, "ERROR") == 0);

    t.triggerClient = nullptr;
}

// A `highlight` span is recorded even with no TriggerClient installed: the
// renderer reads the ring directly, not the dispatch seam. (run_command and the
// other actions do need the client; highlight is painted, not dispatched.)
TEST(testHighlightRecordsWithoutClient)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    // No triggerClient set.
    CHECK(Ubiquitty::Triggers::addRuleSpec(&t, "WARN", "highlight", Ubiquitty::CaseSensitivity::Sensitive));
    feed(t, "WARN low disk\r\n");
    CHECK(Ubiquitty::Triggers::highlightCount(&t) == 1);
    int c0 = -1, c1 = -1;
    CHECK(Ubiquitty::Triggers::highlightAt(&t, 0, nullptr, &c0, &c1));
    CHECK(c0 == 0 && c1 == 3); // "WARN" at columns 0..3
}

// A run_command rule still CLASSIFIES + DISPATCHES in Core (the engine never
// executes — Runtime::resolveTriggerEffect gates the spawn on allow_run_command,
// tested in test_trigger_effects). Here we pin that Core reports it to the client
// with the command template as the arg and the match as matchedText.
TEST(testRunCommandClassifiesAndDispatches)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    CaptureTrigger trig;
    t.triggerClient = &trig;

    CHECK(Ubiquitty::Triggers::addRuleSpec(&t, "deploy [a-z]+", "run_command:/usr/bin/notify %s", Ubiquitty::CaseSensitivity::Sensitive));
    feed(t, "deploy prod\r\n");
    CHECK(trig.calls == 1);
    CHECK(trig.lastAction == static_cast<uint8_t>(Ubiquitty::Triggers::Action::RunCommand));
    // The whole command template rides as the arg; the match is the body.
    CHECK(strcmp(trig.lastArgument, "/usr/bin/notify %s") == 0);
    CHECK(strcmp(trig.lastMatch, "deploy prod") == 0);

    t.triggerClient = nullptr;
}

// A webhook rule classifies + dispatches in Core just like run_command: the
// engine never sends anything (the macOS frontend gates the POST on
// allow_webhook), but Core must report the action with the URL as the arg and
// the matched line as matchedText.
TEST(testWebhookClassifiesAndDispatches)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    CaptureTrigger trig;
    t.triggerClient = &trig;

    CHECK(Ubiquitty::Triggers::addRuleSpec(&t, "BUILD [A-Z]+", "webhook:https://example.com/hook", Ubiquitty::CaseSensitivity::Sensitive));
    feed(t, "BUILD FAILED\r\n");
    CHECK(trig.calls == 1);
    CHECK(trig.lastAction == static_cast<uint8_t>(Ubiquitty::Triggers::Action::Webhook));
    CHECK(strcmp(trig.lastArgument, "https://example.com/hook") == 0);
    CHECK(strcmp(trig.lastMatch, "BUILD FAILED") == 0);

    t.triggerClient = nullptr;
}

// An autowrap-committed line is evaluated too — not just an explicit LF. A
// full-width line that fills exactly to the right margin (no trailing char, no
// LF) must still fire. Pins the emitCharacter / batch fast-path fill hook.
TEST(testAutowrapCommittedLineEvaluated)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10); // 10 columns
    CaptureTrigger trig;
    t.triggerClient = &trig;

    CHECK(Ubiquitty::Triggers::addRule(t, "0123456789", static_cast<uint8_t>(Ubiquitty::Triggers::Action::Notify), "", Ubiquitty::CaseSensitivity::Sensitive));

    // Fill row 0 exactly to the right margin in one batch run — no LF, no
    // trailing char. The line-commit hook must still match it (the autowrap fill
    // coverage), firing exactly once.
    feed(t, "0123456789");
    CHECK(trig.calls == 1);
    CHECK(strcmp(trig.lastMatch, "0123456789") == 0);
    CHECK(trig.lastColumn0 == 0 && trig.lastColumn1 == 9);

    // Now feed one more char to commit the deferred wrap. The already-evaluated
    // full row must not fire a second time (no double-fire).
    feed(t, "Z");
    CHECK(trig.calls == 1); // still 1 — the wrap commit did not re-evaluate row 0

    t.triggerClient = nullptr;
}

// A full-width line followed by an explicit LF must not double-fire either: the
// fill-eval covered it, and the LF on the same still-wrapPending row is a no-op.
TEST(testAutowrapThenExplicitLfNoDoubleFire)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    CaptureTrigger trig;
    t.triggerClient = &trig;
    CHECK(Ubiquitty::Triggers::addRule(t, "abcdefghij", static_cast<uint8_t>(Ubiquitty::Triggers::Action::Notify), "", Ubiquitty::CaseSensitivity::Sensitive));

    feed(t, "abcdefghij"); // exact fill ⇒ fill-eval fires once
    CHECK(trig.calls == 1);
    feed(t, "\r\n"); // explicit LF on the same full row ⇒ must not re-fire
    CHECK(trig.calls == 1);

    t.triggerClient = nullptr;
}

// The autowrap eval guard is dropped on an in-place REWRITE. A CR that returns
// to the full row clears wrapPending but leaves the guard set; writing into the
// row then clears the guard, so the following LF evaluates the rewritten
// content (which may match a different rule) instead of silently skipping it.
TEST(testAutowrapGuardDroppedOnRewrite)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    CaptureTrigger trig;
    t.triggerClient = &trig;

    CHECK(Ubiquitty::Triggers::addRule(t, "0123456789", static_cast<uint8_t>(Ubiquitty::Triggers::Action::Notify), "", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(Ubiquitty::Triggers::addRule(t, "x123456789", static_cast<uint8_t>(Ubiquitty::Triggers::Action::Notify), "", Ubiquitty::CaseSensitivity::Sensitive));

    feed(t, "0123456789"); // exact fill: fill-eval fired, guard = row 0
    CHECK(trig.calls == 1);
    CHECK(strcmp(trig.lastMatch, "0123456789") == 0);
    feed(t, "\r"); // CR: back to column 0; wrapPending cleared, guard still row 0
    feed(t, "x"); // in-place rewrite of the full row: guard dropped
    feed(t, "\n"); // LF: the rewritten row is evaluated again, matching the new rule
    CHECK(trig.calls == 2);
    CHECK(strcmp(trig.lastMatch, "x123456789") == 0); // the rewritten content matched

    t.triggerClient = nullptr;
}

// The highlight ring is a fixed 64-entry oldest-evicting ring: overflowing it
// overwrites the oldest span and advances the head, so the oldest recorded
// highlight is gone and the newest line's first span sits at index 0.
TEST(testHighlightRingEvictsOldest)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 512);
    static const char* const words[] = {
        "K000",
        "K001",
        "K002",
        "K003",
        "K004",
        "K005",
        "K006",
        "K007",
        "K010",
        "K011",
        "K012",
        "K013",
        "K014",
        "K015",
        "K016",
        "K017",
        "K020",
        "K021",
        "K022",
        "K023",
        "K024",
        "K025",
        "K026",
        "K027",
        "K030",
        "K031",
        "K032",
        "K033",
        "K034",
        "K035",
        "K036",
        "K037",
        "K040",
        "K041",
        "K042",
        "K043",
        "K044",
        "K045",
        "K046",
        "K047",
        "K050",
        "K051",
        "K052",
        "K053",
        "K054",
        "K055",
        "K056",
        "K057",
        "K060",
        "K061",
        "K062",
        "K063",
        "K064",
        "K065",
        "K066",
        "K067",
        "K070",
        "K071",
        "K072",
        "K073",
        "K074",
        "K075",
        "K076",
        "K077",
    };
    for (const char* w : words)
        CHECK(Ubiquitty::Triggers::addRuleSpec(&t, w, "highlight", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(Ubiquitty::Triggers::ruleCount(&t) == 64);

    // One line carrying all 64 words: every rule matches, filling the ring.
    char line[384] = { 0 };
    int off = 0;
    for (const char* w : words)
        off += snprintf(line + off, sizeof(line) - off, "%s ", w);
    feed(t, line);
    feed(t, "\r\n"); // the LF commits the line and records all 64 spans
    CHECK(Ubiquitty::Triggers::highlightCount(&t) == 64);

    // The same line again: 64 more records, each evicting one oldest entry.
    feed(t, line);
    feed(t, "\r\n");
    CHECK(Ubiquitty::Triggers::highlightCount(&t) == 64); // ring stays full
    int absoluteRow = -1;
    CHECK(Ubiquitty::Triggers::highlightAt(&t, 0, &absoluteRow, nullptr, nullptr));
    CHECK(absoluteRow == 1); // row 0's spans evicted; index 0 holds row 1's first span

    t.triggerClient = nullptr;
}

// A completed line with no content cells never reaches the matcher: the
// assemble loop trims trailing blanks and bails on a zero-length line.
TEST(testEmptyLineDoesNotEvaluate)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 40);
    CaptureTrigger trig;
    t.triggerClient = &trig;

    CHECK(Ubiquitty::Triggers::addRule(t, "x", static_cast<uint8_t>(Ubiquitty::Triggers::Action::Notify), "", Ubiquitty::CaseSensitivity::Sensitive));
    feed(t, "\r\n"); // an empty completed line
    CHECK(trig.calls == 0);

    t.triggerClient = nullptr;
}

UBIQUITTY_TEST_MAIN()
