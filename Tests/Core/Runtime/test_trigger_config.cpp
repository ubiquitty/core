// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_trigger_config — applying [triggers]/[characterClass] to a Terminal.
//
// The cases that matter here are the two the engine reports identically: a
// pattern it cannot compile and an entry arriving past its fixed table. Both
// come back as a bare false, and a config naming either still loads clean — so a
// test that only exercises well-formed entries passes whether or not the caller
// keeps the refusal.

#include "Harness.h"

#include "Core/Runtime/TriggerConfig.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Triggers/Engine.h"
#include "Core/Triggers/Triggers.h"

#include <memory>
#include <string>

namespace {

namespace Rt = Ubiquitty::Runtime;
namespace Cfg = Ubiquitty::Preferences;

std::unique_ptr<Ubiquitty::Terminal> makeTerminal()
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::hardReset(*term);
    Ubiquitty::resize(*term, 24, 80);
    return term;
}

Cfg::TriggerEntry trigger(const char* pattern, const char* action)
{
    Cfg::TriggerEntry t;
    t.pattern = pattern;
    t.action = action;
    return t;
}

Cfg::CharacterClassEntry characterClass(const char* name, const char* pattern)
{
    Cfg::CharacterClassEntry c;
    c.name = name;
    c.pattern = pattern;
    return c;
}

} // namespace

TEST(testInstallsEveryWellFormedEntry)
{
    auto term = makeTerminal();
    Cfg::Values config;
    config.triggers.push_back(trigger("error", "highlight:red"));
    config.triggers.push_back(trigger("warning", "highlight:yellow"));
    config.characterClasses.push_back(characterClass("path", "[A-Za-z0-9_./-]+"));

    const Rt::TriggerConfigResult result = Rt::applyTriggerConfig(*term, config);

    CHECK(result.rulesInstalled == 2);
    CHECK(result.classesInstalled == 1);
    CHECK(result.complete());
    CHECK(Rt::describeTriggerConfig(result).empty());
    // The engine agrees, not just the return value.
    CHECK(Ubiquitty::Triggers::ruleCount(term.get()) == 2);
    CHECK(Ubiquitty::Triggers::characterClassCount(term.get()) == 1);
}

TEST(testEntriesPastMaxRuleCountAreReportedNotDropped)
{
    auto term = makeTerminal();
    Cfg::Values config;
    const int overshoot = 5;
    for (int i = 0; i < Ubiquitty::Triggers::maxRuleCount + overshoot; ++i)
        config.triggers.push_back(trigger(("rule" + std::to_string(i)).c_str(), "highlight:red"));

    const Rt::TriggerConfigResult result = Rt::applyTriggerConfig(*term, config);

    CHECK(result.rulesInstalled == Ubiquitty::Triggers::maxRuleCount);
    CHECK(static_cast<int>(result.rejectedRules.size()) == overshoot);
    CHECK(!result.complete());
    // Guarded rather than assumed: the failure this case exists to catch is an
    // EMPTY list, and reading front() to prove it is non-empty aborts instead of
    // reporting — a signal costs the summary line and names no case.
    if (!result.rejectedRules.empty()) {
        // The reason distinguishes a full table from a bad pattern: the fix for
        // one is to delete a rule, for the other to correct it.
        for (const Rt::RejectedTriggerEntry& entry : result.rejectedRules)
            CHECK(entry.reason == Rt::TriggerRejection::TableFull);
        // The names are the ones the user would have to find in their config.
        CHECK(result.rejectedRules.front().name == "rule" + std::to_string(Ubiquitty::Triggers::maxRuleCount));
    }

    const std::string report = Rt::describeTriggerConfig(result);
    CHECK(report.find("5 triggers not installed") != std::string::npos);
    CHECK(report.find("(table full)") != std::string::npos);
}

TEST(testMalformedPatternIsReportedAsMalformed)
{
    auto term = makeTerminal();
    Cfg::Values config;
    // Over maxPatternLength, which compile() refuses. A shorter bad pattern would
    // do, but this one cannot become valid under a future syntax extension.
    config.triggers.push_back(trigger(std::string(Ubiquitty::Triggers::maxPatternLength + 1, 'a').c_str(), "highlight:red"));
    config.triggers.push_back(trigger("fine", "highlight:red"));

    const Rt::TriggerConfigResult result = Rt::applyTriggerConfig(*term, config);

    CHECK(result.rulesInstalled == 1);
    CHECK(result.rejectedRules.size() == 1);
    if (!result.rejectedRules.empty())
        CHECK(result.rejectedRules.front().reason == Rt::TriggerRejection::Malformed);
    // A refusal must not abort the rest of the section.
    CHECK(Ubiquitty::Triggers::ruleCount(term.get()) == 1);
    CHECK(Rt::describeTriggerConfig(result).find("(malformed)") != std::string::npos);
}

TEST(testApplyReplacesRatherThanAppends)
{
    auto term = makeTerminal();
    Cfg::Values first;
    first.triggers.push_back(trigger("old", "highlight:red"));
    const Rt::TriggerConfigResult initial = Rt::applyTriggerConfig(*term, first);
    CHECK(initial.rulesInstalled == 1);

    Cfg::Values second;
    second.triggers.push_back(trigger("new", "highlight:blue"));
    const Rt::TriggerConfigResult reloaded = Rt::applyTriggerConfig(*term, second);

    // A reload that appended would leave 2, and the stale rule would keep firing.
    CHECK(reloaded.rulesInstalled == 1);
    CHECK(Ubiquitty::Triggers::ruleCount(term.get()) == 1);
}

TEST(testEmptyEntriesAreSkippedSilently)
{
    auto term = makeTerminal();
    Cfg::Values config;
    config.triggers.push_back(trigger("", "highlight:red"));
    config.triggers.push_back(trigger("pattern", ""));
    config.characterClasses.push_back(characterClass("", "[a-z]+"));
    config.characterClasses.push_back(characterClass("named", ""));

    const Rt::TriggerConfigResult result = Rt::applyTriggerConfig(*term, config);

    // Skipped, not refused: they name no instruction, so reporting them would
    // train people to ignore the report.
    CHECK(result.rulesInstalled == 0);
    CHECK(result.classesInstalled == 0);
    CHECK(result.complete());
}

UBIQUITTY_TEST_MAIN()
