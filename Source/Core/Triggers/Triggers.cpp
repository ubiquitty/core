// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The Terminal-level trigger surface: lazily builds the engine and forwards to it.
// Matching a pattern against output is not sequence parsing; the engine is
// Core/Triggers.{h,cpp} and these are the entry points into it.

#include "Core/Triggers/Triggers.h"
#include "Core/Terminal.h"
#include "Core/Triggers/Engine.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string_view>

namespace Ubiquitty::Triggers {

// Lazily allocate the per-Ubiquitty::Terminal Triggers engine on the first rule / characterClass.
static Ubiquitty::Triggers::Engine* engineEnsure(Ubiquitty::Terminal* t)
{
    if (!t)
        return nullptr;
    if (!t->triggersEngine)
        t->triggersEngine = std::make_unique<Ubiquitty::Triggers::Engine>();
    return t->triggersEngine.get();
}

bool addRule(Ubiquitty::Terminal& t, const char* pattern, uint8_t action, const char* arg, Ubiquitty::CaseSensitivity caseSensitivity)
{
    Ubiquitty::Triggers::Engine* eng = engineEnsure(&t);
    if (!eng || !pattern)
        return false;
    return eng->addRule(pattern, static_cast<Ubiquitty::Triggers::Action>(action), arg, caseSensitivity);
}

// Map a config action-spec NAME to the Triggers::Action wire value. Returns
// true + the action on a known name; false on an unknown one. Case-insensitive,
// and accepts the spelled-out names the [triggers] config uses.
static std::optional<Ubiquitty::Triggers::Action> actionFromName(std::string_view name)
{
    struct Entry {
        std::string_view name;
        Ubiquitty::Triggers::Action action;
    };
    // Spelled lowercase, because the comparison folds only the caller's side.
    static constexpr auto table = std::to_array<Entry>({
        { "notify", Ubiquitty::Triggers::Action::Notify },
        { "copy", Ubiquitty::Triggers::Action::Copy },
        { "set_title", Ubiquitty::Triggers::Action::SetTitle },
        { "highlight", Ubiquitty::Triggers::Action::Highlight },
        { "run_command", Ubiquitty::Triggers::Action::RunCommand },
        { "webhook", Ubiquitty::Triggers::Action::Webhook },
    });
    constexpr auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (const Entry& e : table) {
        if (std::ranges::equal(name, e.name, {}, lower))
            return e.action;
    }
    return std::nullopt;
}

bool addRuleSpec(Ubiquitty::Terminal* t, const char* pattern, const char* actionSpec, Ubiquitty::CaseSensitivity caseSensitivity)
{
    if (!t || !pattern || !actionSpec)
        return false;
    // Split the spec on the FIRST ':' — "<action>" or "<action>:<arg>".
    const char* colon = nullptr;
    for (const char* p = actionSpec; *p; ++p) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }
    const std::string_view name(actionSpec, colon ? static_cast<std::size_t>(colon - actionSpec) : std::strlen(actionSpec));
    const std::optional<Ubiquitty::Triggers::Action> action = actionFromName(name);
    if (!action)
        return false;
    const char* arg = colon ? colon + 1 : "";
    return addRule(*t, pattern, static_cast<uint8_t>(*action), arg, caseSensitivity);
}

bool addCharacterClass(Ubiquitty::Terminal& t, const char* name, const char* pattern, Ubiquitty::CaseSensitivity caseSensitivity)
{
    Ubiquitty::Triggers::Engine* eng = engineEnsure(&t);
    if (!eng || !name || !pattern)
        return false;
    return eng->addCharacterClass(name, pattern, caseSensitivity);
}

void clear(Ubiquitty::Terminal* t)
{
    if (t)
        t->triggersEngine.reset();
}

int ruleCount(const Ubiquitty::Terminal* t) { return (t && t->triggersEngine) ? t->triggersEngine->ruleCount() : 0; }

int characterClassCount(const Ubiquitty::Terminal* t) { return (t && t->triggersEngine) ? t->triggersEngine->characterClassCount() : 0; }

std::optional<MatchSpan> expandWordSelection(const Ubiquitty::Terminal* t, std::string_view utf8, int column)
{
    // No characterClass configured ⇒ keep TODAY's default word selection: report no
    // match and leave the span untouched, so the caller's existing word walk runs.
    if (!t || !t->triggersEngine || t->triggersEngine->characterClassCount() == 0)
        return std::nullopt;
    if (utf8.empty())
        return std::nullopt;
    return t->triggersEngine->expandSelection(utf8, column);
}

int highlightCount(const Ubiquitty::Terminal* t) { return t ? t->triggerHighlightCount : 0; }

bool highlightAt(const Ubiquitty::Terminal* t, int index, int* absoluteRow, int* column0, int* column1)
{
    if (!t || index < 0 || index >= t->triggerHighlightCount)
        return false;
    const int cap = Ubiquitty::Terminal::triggerHighlightCapacity;
    const int slot = (t->triggerHighlightHead + index) % cap;
    const Ubiquitty::Terminal::TriggerHighlight& h = t->triggerHighlights[slot];
    if (absoluteRow)
        *absoluteRow = h.absoluteRow;
    if (column0)
        *column0 = h.column0;
    if (column1)
        *column1 = h.column1;
    return true;
}

} // namespace Ubiquitty::Triggers
