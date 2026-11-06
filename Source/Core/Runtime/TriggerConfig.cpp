// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/TriggerConfig.h"

#include "Core/CaseSensitivity.h"
#include "Core/Triggers/Engine.h"
#include "Core/Triggers/Triggers.h"

namespace Ubiquitty::Runtime {

namespace {

// The engine reports a full table and a bad pattern with the same false, so the
// occupancy is read before the call: afterwards both look identical.
bool isTableFull(int occupied, int maxEntries)
{
    return occupied >= maxEntries;
}

void appendCount(std::string& out, std::size_t count, const char* singular, const char* plural)
{
    out += std::to_string(count);
    out += " ";
    out += count == 1 ? singular : plural;
}

void appendEntries(std::string& out, const std::vector<RejectedTriggerEntry>& entries)
{
    for (std::size_t i = 0; i < entries.size(); ++i) {
        out += i == 0 ? ": " : ", ";
        out += "\"";
        out += entries[i].name;
        out += "\"";
        out += entries[i].reason == TriggerRejection::TableFull ? " (table full)" : " (malformed)";
    }
}

} // namespace

TriggerConfigResult applyTriggerConfig(Ubiquitty::Terminal& t, const Preferences::Values& c)
{
    TriggerConfigResult result;

    Ubiquitty::Triggers::clear(&t);

    for (const Preferences::TriggerEntry& trigger : c.triggers) {
        if (trigger.pattern.empty() || trigger.action.empty())
            continue;
        const bool isFull = isTableFull(Ubiquitty::Triggers::ruleCount(&t), Ubiquitty::Triggers::maxRuleCount);
        if (Ubiquitty::Triggers::addRuleSpec(&t, trigger.pattern.c_str(), trigger.action.c_str(), Ubiquitty::CaseSensitivity::Sensitive)) {
            ++result.rulesInstalled;
            continue;
        }
        result.rejectedRules.push_back({ trigger.pattern, isFull ? TriggerRejection::TableFull : TriggerRejection::Malformed });
    }

    for (const Preferences::CharacterClassEntry& characterClass : c.characterClasses) {
        if (characterClass.name.empty() || characterClass.pattern.empty())
            continue;
        const bool isFull = isTableFull(Ubiquitty::Triggers::characterClassCount(&t), Ubiquitty::Triggers::maxClassCount);
        if (Ubiquitty::Triggers::addCharacterClass(t, characterClass.name.c_str(), characterClass.pattern.c_str(), Ubiquitty::CaseSensitivity::Sensitive)) {
            ++result.classesInstalled;
            continue;
        }
        result.rejectedClasses.push_back({ characterClass.name, isFull ? TriggerRejection::TableFull : TriggerRejection::Malformed });
    }

    return result;
}

std::string describeTriggerConfig(const TriggerConfigResult& result)
{
    if (result.complete())
        return {};

    std::string out = "config: ";
    if (!result.rejectedRules.empty()) {
        appendCount(out, result.rejectedRules.size(), "trigger", "triggers");
        out += " not installed";
        appendEntries(out, result.rejectedRules);
    }
    if (!result.rejectedRules.empty() && !result.rejectedClasses.empty())
        out += "; ";
    if (!result.rejectedClasses.empty()) {
        appendCount(out, result.rejectedClasses.size(), "character class", "character classes");
        out += " not installed";
        appendEntries(out, result.rejectedClasses);
    }
    return out;
}

} // namespace Ubiquitty::Runtime
