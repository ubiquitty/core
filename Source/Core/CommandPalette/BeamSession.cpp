// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/CommandPalette/BeamSession.cpp — see BeamSession.h.

#include "Core/CommandPalette/BeamSession.h"

#include "Core/CommandPalette/SettingsCommands.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Ubiquitty::CommandPalette {

namespace {

// Drops the last whole UTF-8 sequence. Backspacing a byte at a time would leave
// a truncated sequence in the query, which the ranker then matches nothing
// against and the field draws as a replacement character — a delete that
// appears to do nothing on exactly the input that most needs it to work.
void popCodePoint(std::string& text)
{
    while (!text.empty()) {
        const auto byte = static_cast<unsigned char>(text.back());
        text.pop_back();
        // Continuation bytes are 10xxxxxx; anything else starts a sequence.
        if ((byte & 0xC0) != 0x80)
            return;
    }
}

const PaletteCommand* findCommand(const std::vector<PaletteCommand>& commands, const std::string& id)
{
    for (const PaletteCommand& command : commands)
        if (command.id == id)
            return &command;
    return nullptr;
}

// The editor field's text as a number, or `fallback` when it does not spell one
// whole.
//
// Strict on both ends: the WHOLE trimmed field must parse, so "12px" and a
// half-typed "1." fall back rather than reading as 12 and 1. Stepping off a
// misread would discard what the user was part-way through typing and replace it
// with a value they never asked for, and accepting one would apply it.
//
// Infinities and NaN parse (strtod spells them) and are refused here rather than
// downstream: gridFloor them is not a value — max(NaN, low) is NaN under IEEE
// comparison, so a NaN survives the clamp and reaches the setter.
double parseNumber(const std::string& text, double fallback)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return fallback;
    const std::string trimmed = text.substr(first, text.find_last_not_of(" \t") - first + 1);

    char* end = nullptr;
    const double parsed = std::strtod(trimmed.c_str(), &end);
    if (end != trimmed.c_str() + trimmed.size())
        return fallback;
    return std::isfinite(parsed) ? parsed : fallback;
}

double clampToRange(double value, const PaletteNumber& schema)
{
    return std::min(std::max(value, schema.minimum), schema.maximum);
}

} // namespace

void BeamSession::setCommands(Scope scope, PaletteCommandList commands)
{
    // Held per scope rather than replacing the active set: a prefix retargets the
    // palette mid-keystroke, and a caller re-supplying a list it already gave is
    // a caller that has to know which scope the user is about to type.
    m_scopeCommands[scope] = std::move(commands);
    if (scope == m_scope)
        m_engine.setActive(scope, m_scopeCommands[scope]);
    reRank();
}

// Point the engine at a scope's registered list. An unregistered scope ranks
// nothing rather than the previous scope's commands, which would answer a
// question about a list the user is no longer looking at.
void BeamSession::activateScope(Scope scope)
{
    m_scope = scope;
    m_state.scope = scope;
    const auto found = m_scopeCommands.find(scope);
    m_engine.setActive(scope, found != m_scopeCommands.end() ? found->second : PaletteCommandList {});
}

void BeamSession::setUsageScores(UsageScoreMap scores)
{
    m_engine.setUsageScores(std::move(scores));
    reRank();
}

void BeamSession::setOptions(std::string commandID, std::vector<PaletteOption> options, int currentIndex)
{
    OptionSet set;
    set.options = std::move(options);
    set.currentIndex = currentIndex;
    m_options[std::move(commandID)] = std::move(set);
}

void BeamSession::setNumber(std::string commandID, PaletteNumber number)
{
    m_numbers[std::move(commandID)] = std::move(number);
}

void BeamSession::enterDrill(const std::string& commandID, const std::string& title)
{
    m_savedQuery = m_state.query;
    m_drillID = commandID;
    m_state.drillTitle = title;
    const auto number = m_numbers.find(commandID);
    m_state.isValueEditor = number != m_numbers.end();
    // A value editor opens ON the live value, selected: the field is the value,
    // so an empty one reads as "unset" and the first Enter applies the fallback
    // rather than what the setting currently holds. An options drill opens with
    // its filter cleared, so every value shows.
    m_state.query = m_state.isValueEditor
        ? Settings::formatNumber(number->second.value, number->second.step)
        : std::string {};
    reRank();
}

void BeamSession::leaveDrill()
{
    m_drillID.clear();
    m_state.drillTitle.clear();
    m_state.isValueEditor = false;
    m_state.query = m_savedQuery;
    m_savedQuery.clear();
    reRank();
}

void BeamSession::setViewportRows(int rows)
{
    m_viewportRows = rows > 0 ? rows : 0;
    clampSelection();
}

// Setting VALUES answer the top-level query too, so a user who knows the value
// can type it. Someone who wants the Noir theme types "noir" — expecting them to
// know the command is called "Choose Theme", find it, and drill in first is a
// map of the menu, not a search.
//
// macOS reaches this by walking its menus into the Theme and Font submenus, so
// every theme is a top-level command there (its command curation skips
// value-picker submenus but not those two). This is the same behaviour for
// frontends that register values as an option set rather than as menu items.
//
// Only with a query typed. An empty palette listing every theme buries the
// twelve commands under thirty-four values, and the empty state is the one a
// user reads rather than filters.
void BeamSession::appendMatchingValues()
{
    if (m_state.query.empty() || m_options.empty())
        return;

    for (const auto& [commandID, set] : m_options) {
        const std::vector<PaletteResult> ranked = m_engine.rankedOptions(m_state.query, set.options, set.currentIndex);
        const PaletteCommand* owner = findCommand(m_engine.commands(), commandID);
        for (const PaletteResult& result : ranked) {
            const int index = std::atoi(result.id.c_str());
            if (index < 0 || index >= static_cast<int>(set.options.size()))
                continue;
            BeamRow row;
            row.title = set.options[static_cast<std::size_t>(index)].label;
            // The owning command names what the value BELONGS to, so a bare
            // "Noir" is not a row whose effect the user has to guess.
            row.detail = owner ? owner->title : commandID;
            row.matchedIndices = result.matchedIndices;
            row.isChecked = result.checked;
            row.commandID = commandID;
            row.optionIndex = index;
            m_state.rows.push_back(std::move(row));
        }
    }
}

void BeamSession::reRankOptions()
{
    const auto found = m_options.find(m_drillID);
    if (found == m_options.end()) {
        m_state.rows.clear();
        m_state.selectedIndex = -1;
        m_state.scrollTop = 0;
        m_state.status = "no values";
        return;
    }
    const OptionSet& set = found->second;
    const std::vector<PaletteResult> ranked = m_engine.rankedOptions(m_state.query, set.options, set.currentIndex);

    m_state.rows.clear();
    m_state.rows.reserve(ranked.size());
    for (const PaletteResult& result : ranked) {
        // `id` is the option's ORIGINAL index, which is what an accept must
        // carry: ranking reorders the list, so the row position names a
        // different value the moment a query narrows it.
        const int index = std::atoi(result.id.c_str());
        if (index < 0 || index >= static_cast<int>(set.options.size()))
            continue;
        BeamRow row;
        row.title = set.options[static_cast<std::size_t>(index)].label;
        row.matchedIndices = result.matchedIndices;
        row.isChecked = result.checked;
        row.commandID = m_drillID;
        row.optionIndex = index;
        m_state.rows.push_back(std::move(row));
    }
    // Opening a drill lands on the CURRENT value rather than the top, so Enter
    // straight after Descend is a no-op instead of silently setting whatever
    // sorted first. Once a query narrows the list the top is the right answer,
    // because the user has said what they are looking for.
    m_state.selectedIndex = m_state.rows.empty() ? -1 : 0;
    if (m_state.query.empty()) {
        for (std::size_t i = 0; i < m_state.rows.size(); ++i) {
            if (m_state.rows[i].isChecked) {
                m_state.selectedIndex = static_cast<int>(i);
                break;
            }
        }
    }
    m_state.scrollTop = 0;
    clampSelection();
    m_state.status = m_state.rows.empty() && !m_state.query.empty() ? "no matching values" : std::string {};
}

// A number editor has no list to rank: the field holds the value, and the row
// area carries the range hint instead. Rebuilt on every event that reaches
// reRank so the hint follows a schema re-registered between opens.
void BeamSession::refreshNumberEditor()
{
    m_state.rows.clear();
    m_state.selectedIndex = -1;
    m_state.scrollTop = 0;
    const auto found = m_numbers.find(m_drillID);
    m_state.status = found == m_numbers.end()
        ? std::string {}
        : Settings::rangeHint(found->second.minimum, found->second.maximum, found->second.step, found->second.unit);
}

// One arrow press: step off what is in the field, not off the registered value,
// so stepping continues from a number the user typed rather than jumping back.
void BeamSession::stepNumber(int direction)
{
    const auto found = m_numbers.find(m_drillID);
    if (found == m_numbers.end())
        return;
    const PaletteNumber& schema = found->second;
    const double base = parseNumber(m_state.query, schema.value);
    m_state.query = Settings::formatNumber(clampToRange(base + direction * schema.step, schema), schema.step);
    refreshNumberEditor();
}

void BeamSession::reRank()
{
    if (m_state.isValueEditor) {
        refreshNumberEditor();
        return;
    }
    if (!m_drillID.empty()) {
        reRankOptions();
        return;
    }
    const std::vector<PaletteResult> ranked = m_engine.rankedResults(m_state.query);
    const std::vector<PaletteCommand>& commands = m_engine.commands();

    m_state.rows.clear();
    m_state.rows.reserve(ranked.size());
    for (const PaletteResult& result : ranked) {
        const PaletteCommand* command = findCommand(commands, result.id);
        if (!command)
            continue; // a ranked id with no command is a caller bug, not a row
        BeamRow row;
        row.title = command->title;
        row.detail = command->shortcut.empty() ? command->group : command->shortcut;
        row.matchedIndices = result.matchedIndices;
        // Keyed on REGISTRATION rather than on the command's declared kind: a
        // drill whose values were never registered would otherwise advertise an
        // affordance that opens an empty box, which looks like a broken feature
        // instead of an absent one.
        row.hasChildren = m_options.count(command->id) != 0 || m_numbers.count(command->id) != 0;
        row.commandID = command->id;
        m_state.rows.push_back(std::move(row));
    }

    appendMatchingValues();
    // Typing re-ranks, so the previous selection points at a different row or at
    // none. Returning to the top is the only answer that is right regardless of
    // what moved — keeping the index would silently select whatever landed there.
    m_state.selectedIndex = m_state.rows.empty() ? -1 : 0;
    m_state.scrollTop = 0;
    m_state.status = m_state.rows.empty() && !m_state.query.empty() ? "no matches" : std::string {};
}

void BeamSession::clampSelection()
{
    const int rowCount = static_cast<int>(m_state.rows.size());
    if (rowCount == 0) {
        m_state.selectedIndex = -1;
        m_state.scrollTop = 0;
        return;
    }
    m_state.selectedIndex = std::clamp(m_state.selectedIndex, 0, rowCount - 1);

    if (m_viewportRows <= 0) {
        m_state.scrollTop = 0;
        return;
    }
    // Scroll only as far as it takes to bring the selection into view, so a
    // selection already visible does not move the list under the user.
    if (m_state.selectedIndex < m_state.scrollTop)
        m_state.scrollTop = m_state.selectedIndex;
    else if (m_state.selectedIndex >= m_state.scrollTop + m_viewportRows)
        m_state.scrollTop = m_state.selectedIndex - m_viewportRows + 1;
    m_state.scrollTop = std::clamp(m_state.scrollTop, 0, std::max(0, rowCount - m_viewportRows));
}

BeamAction BeamSession::apply(BeamEvent event, std::uint32_t codePoint)
{
    // Toggle is the one event a closed palette answers. Everything else while
    // closed is a caller that has not checked isOpen(), and doing nothing is the
    // safe reading — a closed palette must never consume or act.
    if (!m_state.isOpen && event != BeamEvent::Toggle)
        return {};

    switch (event) {
    case BeamEvent::Toggle:
        if (m_state.isOpen) {
            m_state.isOpen = false;
            // The toggle key closes the palette outright, drill or not — unlike
            // Escape, it is not a "back" gesture and never was.
            if (!m_drillID.empty())
                leaveDrill();
            bump();
            return BeamAction { BeamAction::Kind::Closed, {} };
        }
        m_state.isOpen = true;
        // Opening starts from nothing typed. A palette that reopened with the
        // last query would filter against a list the user has since changed, and
        // the first keystroke would then append rather than replace.
        m_state.query.clear();
        reRank();
        bump();
        return {};

    case BeamEvent::Dismiss:
        // Inside a drill this backs out and never closes, which is what macOS's
        // palette does and therefore what the behaviour is. Closing from a value
        // list would throw away the command the user had already found, and the
        // key that means "back" everywhere else in the palette would be the one
        // key that means "give up".
        if (!m_drillID.empty()) {
            leaveDrill();
            bump();
            return {};
        }
        // A scope is a place the user navigated to, so Escape backs out of it the
        // way it backs out of a drill. Closing from inside one would throw away
        // the scope and the palette for a single mistyped prefix.
        if (m_scope != Scope::Actions) {
            activateScope(Scope::Actions);
            m_state.query.clear();
            reRank();
            bump();
            return {};
        }
        m_state.isOpen = false;
        bump();
        return BeamAction { BeamAction::Kind::Closed, {} };

    case BeamEvent::Character: {
        Unicode::appendUTF8(m_state.query, codePoint);
        // The prefix table, the single strip and the terminal gate are all
        // Engine's. A session that appended and re-ranked without asking left
        // every frontend but the one that reimplemented this filtering its
        // actions for a literal '@'.
        const Engine::ParsedInput parsed
            = Engine::interpretInput(m_state.query, m_scope, m_terminalAvailability);
        if (parsed.didSwitchScope) {
            activateScope(parsed.scope);
            m_state.query = parsed.query;
        }
        reRank();
        bump();
        return {};
    }

    case BeamEvent::Backspace:
        if (m_state.query.empty())
            return {};
        popCodePoint(m_state.query);
        reRank();
        bump();
        return {};

    // In a value editor the vertical arrows are the stepper — there is no list
    // for them to move through, and a stepper is what every settings control
    // spells this with. Up increases, matching the ± affordance's top half.
    case BeamEvent::SelectPrevious:
        if (m_state.isValueEditor) {
            stepNumber(1);
            bump();
            return {};
        }
        m_state.selectedIndex -= 1;
        clampSelection();
        bump();
        return {};

    case BeamEvent::SelectNext:
        if (m_state.isValueEditor) {
            stepNumber(-1);
            bump();
            return {};
        }
        m_state.selectedIndex += 1;
        clampSelection();
        bump();
        return {};

    case BeamEvent::PageUp:
        m_state.selectedIndex -= std::max(1, m_viewportRows);
        clampSelection();
        bump();
        return {};

    case BeamEvent::PageDown:
        m_state.selectedIndex += std::max(1, m_viewportRows);
        clampSelection();
        bump();
        return {};

    case BeamEvent::SelectFirst:
        m_state.selectedIndex = 0;
        clampSelection();
        bump();
        return {};

    case BeamEvent::SelectLast:
        m_state.selectedIndex = static_cast<int>(m_state.rows.size()) - 1;
        clampSelection();
        bump();
        return {};

    case BeamEvent::Accept:
    case BeamEvent::AcceptAlternate: {
        // A value editor commits the FIELD, not a row — it has none, so the
        // selection guard below would refuse every Enter it ever saw.
        if (m_state.isValueEditor) {
            const auto found = m_numbers.find(m_drillID);
            if (found == m_numbers.end()) {
                leaveDrill();
                bump();
                return {};
            }
            BeamAction action;
            action.kind = BeamAction::Kind::Run;
            action.commandID = m_drillID;
            // Parsed and clamped here so every frontend applies the same number:
            // a caller handed the raw text would each write its own parse, and
            // the one that forgot the bounds would set a value the schema says
            // cannot exist.
            action.numberValue = clampToRange(parseNumber(m_state.query, found->second.value), found->second);
            action.isAlternate = event == BeamEvent::AcceptAlternate;
            m_state.isOpen = false;
            leaveDrill();
            bump();
            return action;
        }
        if (m_state.selectedIndex < 0 || m_state.selectedIndex >= static_cast<int>(m_state.rows.size()))
            return {};
        // Read from the ROW the user is looking at. Re-ranking here and indexing
        // the fresh result by selectedIndex would be a second ranking, and
        // therefore a second chance to disagree with what is on screen — and it
        // could not express a value row at all, because those come from a
        // different ranking than the commands beside them.
        const BeamRow& chosenRow = m_state.rows[static_cast<std::size_t>(m_state.selectedIndex)];
        if (chosenRow.commandID.empty())
            return {};
        // A row with children OPENS rather than runs. Enter on "Choose Theme"
        // means "show me the themes" — running it does nothing, because a drill
        // command has no effect of its own, so accepting it closed the palette
        // and left the user with no theme chosen and no explanation. Enter and
        // Right now do the same thing here, which is what every palette does.
        if (chosenRow.hasChildren && m_drillID.empty()) {
            enterDrill(chosenRow.commandID, chosenRow.title);
            bump();
            return {};
        }
        BeamAction action;
        action.kind = BeamAction::Kind::Run;
        action.commandID = chosenRow.commandID;
        action.optionIndex = chosenRow.optionIndex;
        action.isAlternate = event == BeamEvent::AcceptAlternate;
        // Accepting closes. A palette that stayed open after running something
        // leaves the user typing into a filter while the command's own output
        // arrives underneath it.
        m_state.isOpen = false;
        if (!m_drillID.empty())
            leaveDrill();
        bump();
        return action;
    }

    case BeamEvent::Descend: {
        // Already inside one: a drill has no children of its own, and a second
        // level would have no way back that Ascend could express.
        if (!m_drillID.empty())
            return {};
        if (m_state.selectedIndex < 0 || m_state.selectedIndex >= static_cast<int>(m_state.rows.size()))
            return {};
        // Read from the ROW the user is looking at, as Accept does. Re-ranking
        // here to recover an id is a second ranking, and so a second chance to
        // disagree with what is on screen; it also cannot describe a value row
        // at all, because those come from a different ranking than the commands
        // beside them and index past the end of this one.
        const BeamRow& chosenRow = m_state.rows[static_cast<std::size_t>(m_state.selectedIndex)];
        if (!chosenRow.hasChildren || chosenRow.commandID.empty())
            return {}; // a row with nothing registered has nothing to enter
        enterDrill(chosenRow.commandID, chosenRow.title);
        bump();
        return {};
    }

    case BeamEvent::Ascend:
        // Out of the drill first. Undoing the filter here would strand the user
        // inside a value list with no visible way back to the commands.
        if (!m_drillID.empty()) {
            leaveDrill();
            bump();
            return {};
        }
        // Backing out of a filter before backing out of the palette — the query
        // is what the user most recently built, so it is what Ascend undoes.
        if (!m_state.query.empty()) {
            m_state.query.clear();
            reRank();
            bump();
            return {};
        }
        m_state.isOpen = false;
        bump();
        return BeamAction { BeamAction::Kind::Closed, {} };
    }
    return {};
}

} // namespace Ubiquitty::CommandPalette
