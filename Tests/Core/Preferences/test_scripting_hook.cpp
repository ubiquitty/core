// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_scripting_hook.cpp — the `[scripting]` hook's policy: which program runs
// and what of its output is folded into the configuration.
//
// The hook executes a program named in the config file at every load, so the
// gate is the whole of its safety: a config that names a script and does not
// say `enable = true` must run nothing. And the bound is what keeps a script
// that loops printing from holding the load open while the buffer grows, so a
// directive past it — however it got into the buffer — is not merged.

#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"

#include <string>

namespace Cfg = Ubiquitty::Preferences;

/// No program runs until the gate is opened by name.
TEST(theScriptingHookRunsNothingUntilItsGateIsOpen)
{
    Cfg::Values values;
    values.scriptingConfigScript = std::string("/usr/local/bin/rules");
    // Unset is the default, and the default is shut.
    CHECK(Cfg::Effective::scriptingHookProgram(values).empty());
    values.scriptingEnable = false;
    CHECK(Cfg::Effective::scriptingHookProgram(values).empty());

    values.scriptingEnable = true;
    CHECK(Cfg::Effective::scriptingHookProgram(values) == "/usr/local/bin/rules");

    // Open with nothing named is nothing to run.
    values.scriptingConfigScript.reset();
    CHECK(Cfg::Effective::scriptingHookProgram(values).empty());
}

/// What the script prints lands after what the file declared.
TEST(theScriptingHookAppendsWhatTheScriptPrinted)
{
    Cfg::Values values;
    Cfg::TriggerEntry declared;
    declared.pattern = "declared";
    declared.action = "copy";
    values.triggers.push_back(declared);

    const std::string output = "# a comment\n"
                               "trigger HOOKED-[0-9]+ = copy\n"
                               "macro deploy = make deploy\\r\n"
                               "charclass word = [A-Za-z]+\n"
                               "nonsense line\n";
    CHECK_EQ(Cfg::mergeScriptingRules(values, output), 3);
    CHECK_EQ(values.triggers.size(), std::size_t { 2 });
    if (values.triggers.size() == 2) {
        CHECK(values.triggers[0].pattern == "declared");
        CHECK(values.triggers[1].pattern == "HOOKED-[0-9]+");
        CHECK(values.triggers[1].action == "copy");
    }
    CHECK_EQ(values.macros.size(), std::size_t { 1 });
    CHECK_EQ(values.characterClasses.size(), std::size_t { 1 });

    // Output with nothing in it merges nothing and says so.
    CHECK_EQ(Cfg::mergeScriptingRules(values, "# only a comment\n"), 0);
    CHECK_EQ(values.triggers.size(), std::size_t { 2 });
}

/// A directive past the bound is not merged, and neither is one the bound cuts
/// through.
TEST(theScriptingHookMergesNothingPastItsBound)
{
    // Comment lines up to ten bytes short of the bound, then a directive that
    // straddles it, then one wholly past it.
    const std::string fillerLine = "# padding padding padding padding padding padding padding padding\n";
    const std::size_t straddleStart = Cfg::scriptingOutputLimit - 10;
    std::string output = "trigger FIRST = copy\n";
    while (output.size() + fillerLine.size() + 2 <= straddleStart)
        output += fillerLine;
    output += "#" + std::string(straddleStart - output.size() - 2, ' ') + "\n";
    CHECK_EQ(output.size(), straddleStart);
    output += "trigger STRADDLES = copy\n";
    output += "trigger BEYOND = copy\n";
    CHECK(output.size() > Cfg::scriptingOutputLimit);

    Cfg::Values values;
    CHECK_EQ(Cfg::mergeScriptingRules(values, output), 1);
    CHECK_EQ(values.triggers.size(), std::size_t { 1 });
    if (values.triggers.size() == 1)
        CHECK(values.triggers[0].pattern == "FIRST");

    // And the same text under the bound merges all three, which is what keeps
    // the case above from passing on a merge that drops everything.
    Cfg::Values unbounded;
    CHECK_EQ(Cfg::mergeScriptingRules(unbounded, "trigger FIRST = copy\ntrigger STRADDLES = copy\ntrigger BEYOND = copy\n"), 3);
}

UBIQUITTY_TEST_MAIN()
