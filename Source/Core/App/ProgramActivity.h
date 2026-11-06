// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/App/StatusRollup.h"
#include "Core/App/TitleCompose.h"
#include "Core/Color.h"
#include "Core/OSC/Activity.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Title/Truncate.h"
#include "Core/Unicode/UTF8Decoder.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::App {

/// The theme slot a program's reported state is drawn in.
///
/// - Parameter state: A status record's state.
/// - Returns: The slot; the neutral one for a state with nothing to mark.
[[nodiscard]] constexpr Themes::StatusColorSlot programActivitySlot(OSC::ProgramState state)
{
    switch (state) {
    case OSC::ProgramState::Blocked:
        return Themes::StatusColorSlot::Blocked;
    case OSC::ProgramState::Error:
        return Themes::StatusColorSlot::Failed;
    case OSC::ProgramState::Working:
        return Themes::StatusColorSlot::Working;
    case OSC::ProgramState::Done:
        return Themes::StatusColorSlot::Done;
    case OSC::ProgramState::Idle:
    case OSC::ProgramState::None:
        break;
    }
    return Themes::StatusColorSlot::Neutral;
}

/// The status colours of a pane's theme.
///
/// Read from the palette the theme gave the terminal, not the live one: a
/// program that repaints an ANSI slot (OSC 4) recolours its own text, and the
/// status mark beside it is the window's to colour.
///
/// - Parameter terminal: The pane's terminal.
/// - Returns: The theme's status colours, derived as ``Themes::deriveStatusColors`` derives them.
[[nodiscard]] inline Themes::StatusColors statusColors(const Terminal& terminal)
{
    const auto narrow = [](PaletteRGB color) {
        return ColorRGBA { static_cast<std::uint8_t>(color.red >> 8), static_cast<std::uint8_t>(color.green >> 8), static_cast<std::uint8_t>(color.blue >> 8) };
    };
    std::array<ColorRGBA, 16> ansi {};
    for (std::size_t index = 0; index < ansi.size(); ++index)
        ansi[index] = narrow(terminal.paletteBaseline.indexed[index]);
    return Themes::deriveStatusColors(ansi, narrow(terminal.paletteBaseline.defaultForeground), narrow(terminal.paletteBaseline.defaultBackground));
}

/// The theme slot a pane's tab status is drawn in, when its colour is a theme's.
///
/// - Parameter terminal: The pane whose status is presented, or null.
/// - Returns: The primary status record's slot; nothing when there is no
///   record, where any colour shown is the program's own (OSC 21337).
[[nodiscard]] inline std::optional<Themes::StatusColorSlot> tabActivitySlot(const Terminal* terminal)
{
    if (terminal) {
        if (const auto* record = terminal->programStatus.primary())
            return programActivitySlot(record->state);
    }
    return std::nullopt;
}

/// Returns the program status color, falling back to an explicit tab indicator.
///
/// A program's status is drawn in the pane's theme colour for it; an OSC 21337
/// indicator is the program's own colour and is drawn as sent.
///
/// - Parameter terminal: The pane whose status is presented, or null.
/// - Returns: A color when either protocol supplies a status.
[[nodiscard]] inline std::optional<ColorRGBA> tabActivityColor(const Terminal* terminal)
{
    if (terminal) {
        if (const auto* record = terminal->programStatus.primary()) {
            return statusColors(*terminal).color(programActivitySlot(record->state));
        }
    }
    return tabStatusColor(terminal);
}

/// Reads one pane's status facts from its records, for ``paneStatus``.
///
/// The one reader of a pane's status, wherever its facts came from: a host
/// reads its terminal into the two records and a client is sent them, so a
/// roll-up over a remote pane and over a local one orders the same facts.
///
/// - Parameters:
///   - facts: What the pane's host says about it beside its picture.
///   - records: The status its programs posted.
/// - Returns: The facts the roll-up orders.
[[nodiscard]] inline PaneStatusFacts paneStatusFacts(const Protocol::Sync::PaneFacts& facts, const Protocol::Sync::StatusRecords& records)
{
    PaneStatusFacts status;
    status.isAtPrompt = facts.isAtPrompt;
    if (const auto* record = records.store.primary()) {
        status.programState = record->state;
        status.blockKind = record->kind;
    }
    status.progress = { facts.progress.state, facts.progress.value };
    if (facts.exitStatus)
        status.exitStatus = *facts.exitStatus;
    status.hasUnseenBell = facts.attention.hasUnseenBell;
    status.hasUnseenNotification = facts.attention.hasUnseenNotification;
    if (facts.indicator)
        status.indicatorColor = ColorRGBA { facts.indicator->red, facts.indicator->green, facts.indicator->blue };
    if (facts.tabColor)
        status.tabColor = ColorRGBA { facts.tabColor->red, facts.tabColor->green, facts.tabColor->blue };
    status.hasBadge = !facts.badgeText.empty();
    return status;
}

/// Reads one pane's status facts off its terminal, through the records a
/// host would send for it.
///
/// Everything the roll-up orders comes from the terminal except who holds it:
/// that is a fact about the process group, which whoever owns the PTY knows.
///
/// - Parameters:
///   - terminal: The pane's terminal, or null.
///   - holder: Who holds the terminal.
/// - Returns: The facts; those of an empty pane for a null terminal.
[[nodiscard]] inline PaneStatusFacts paneStatusFacts(const Terminal* terminal, TerminalHolder holder)
{
    Protocol::Sync::PaneHostFacts host;
    host.isAtPrompt = holder == TerminalHolder::Prompt;
    if (!terminal) {
        Protocol::Sync::PaneFacts empty;
        empty.isAtPrompt = host.isAtPrompt;
        return paneStatusFacts(empty, {});
    }
    return paneStatusFacts(Protocol::Sync::paneFactsOf(*terminal, host), Protocol::Sync::statusRecordsOf(*terminal));
}

/// Removes invisible formatting controls from text displayed outside the grid.
[[nodiscard]] inline std::string visibleActivityText(std::string_view text)
{
    std::string result;
    Unicode::UTF8Decoder decoder {};
    std::size_t start = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t codePoint = 0;
        const auto status = Unicode::utf8Feed(&decoder, static_cast<unsigned char>(text[index]), &codePoint);
        if (status == Unicode::UTF8Status::Need) {
            continue;
        }
        const bool isInvisible = codePoint == 0xAD || codePoint == 0x61C || codePoint == 0xFEFF
            || (codePoint >= 0x200B && codePoint <= 0x200F) || (codePoint >= 0x2028 && codePoint <= 0x202E)
            || (codePoint >= 0x2060 && codePoint <= 0x206F) || (codePoint >= 0xFFF9 && codePoint <= 0xFFFB)
            || (codePoint >= 0xE0000 && codePoint <= 0xE007F);
        if (status == Unicode::UTF8Status::Emit && codePoint >= 0x20 && !(codePoint >= 0x7F && codePoint <= 0x9F) && !isInvisible) {
            result.append(text.substr(start, index + 1 - start));
        }
        start = index + 1;
    }
    return result;
}

/// Formats the primary record as plain text for a tab or pane indicator.
/// What a program says it is doing: its primary record's title and message,
/// joined with a dash, and none of the name, state or percent a status mark
/// and the progress ring beside it already carry.
///
/// - Parameter store: A pane's program status records.
/// - Returns: The text, cleaned for display; empty when there is no record
///   or it says nothing.
[[nodiscard]] inline std::string programActivityText(const OSC::ProgramStatusStore& store)
{
    const auto* record = store.primary();
    if (!record) {
        return {};
    }
    std::string text;
    for (const std::string_view part : { std::string_view(record->title), std::string_view(record->message) }) {
        if (part.empty()) {
            continue;
        }
        if (!text.empty()) {
            text += " — ";
        }
        text += visibleActivityText(part);
    }
    return text;
}

[[nodiscard]] inline std::string programActivityLabel(const OSC::ProgramStatusStore& store)
{
    const auto* record = store.primary();
    if (!record) {
        return {};
    }
    std::string label(store.effectiveApp(*record));
    if (!label.empty()) {
        label += ": ";
    }
    switch (record->state) {
    case OSC::ProgramState::Idle:
        label += "Idle";
        break;
    case OSC::ProgramState::Working:
        label += "Working";
        break;
    case OSC::ProgramState::Done:
        label += "Done";
        break;
    case OSC::ProgramState::Blocked:
        label += record->kind == OSC::ProgramBlockKind::Permission ? "Approval needed" : record->kind == OSC::ProgramBlockKind::Auth ? "Authentication needed"
                                                                                                                                     : "Waiting for input";
        break;
    case OSC::ProgramState::Error:
        label += "Failed";
        break;
    case OSC::ProgramState::None:
        break;
    }
    if (record->progress >= 0) {
        label += " " + std::to_string(record->progress) + "%";
    }
    if (const std::string text = programActivityText(store); !text.empty()) {
        label += " — " + text;
    }
    return label;
}

/// The longest status help text, in UTF-8 bytes.
///
/// What a program may put into its status is bounded where it is read (a
/// report's title and message, `OSC::maxProgramStatusTitleBytes` and
/// `OSC::maxProgramStatusMessageBytes`; an indicator's label,
/// `OSC::tabStatusTextCapacity`), and the words a label adds around them are
/// Core's own. This is the bound on the whole, held where the text is
/// composed: a reader that lays the text out can size for it without knowing
/// what it was made of.
inline constexpr std::size_t maxStatusHelpBytes = 2560;

/// The whole of what a pane's status says, for a reader who asks for it: the
/// hover text of a tab, and what an assistive reader is told.
///
/// The words are those of the label a title shows, uncut: the primary status
/// record's, else the indicator's own. A title has room for a few words and
/// is cut to them; the help text is everything the program said, with the
/// controls and the invisible formatting characters a program might have put
/// in it removed (``visibleActivityText``), so a direction override or a
/// line separator in a program's message reaches no label.
///
/// - Parameters:
///   - terminal: The pane's terminal.
///   - activityLabel: ``programActivityLabel`` of the terminal's status, for
///     a caller that has composed it already.
/// - Returns: The text, at most ``maxStatusHelpBytes`` long and cut on a
///   scalar boundary; empty when the pane reports no status.
[[nodiscard]] inline std::string statusHelpText(const Terminal& terminal, std::string activityLabel)
{
    std::string text = std::move(activityLabel);
    if (text.empty())
        text = visibleActivityText(Ubiquitty::tabStatusText(&terminal));
    text.resize(Title::titleBytePrefix(text, maxStatusHelpBytes).size());
    return text;
}

/// The whole of what a pane's status says, composed from its terminal.
///
/// - Parameter terminal: The pane's terminal.
/// - Returns: The text, as the overload taking a composed label gives it.
[[nodiscard]] inline std::string statusHelpText(const Terminal& terminal)
{
    return statusHelpText(terminal, programActivityLabel(terminal.programStatus));
}

/// A number that moves when a status help text does, for a reader that
/// keeps what it last showed and asks for the text again only when it changed.
///
/// - Parameter text: A text from ``statusHelpText``.
/// - Returns: Zero for no text; otherwise a hash of it, never zero.
[[nodiscard]] inline std::uint32_t statusHelpDigest(std::string_view text)
{
    if (text.empty())
        return 0;
    const auto digest = static_cast<std::uint32_t>(std::hash<std::string_view> {}(text));
    return digest != 0 ? digest : 1;
}

/// Reads the host's half of what a pane's title is composed from, from the
/// pane's records.
///
/// The one reader of a pane's title inputs, wherever its facts came from; the
/// reader of the title adds its own `TitleViewerFacts`. The label a title
/// shows for the pane's status is the primary status record's, and the
/// indicator's own word when no record is posted. What only a layout knows is
/// left empty for whoever holds one: the pane's label, its tab's name and its
/// window's title.
///
/// - Parameters:
///   - facts: What the pane's host says about it beside its picture.
///   - records: The status its programs posted.
/// - Returns: The title facts.
[[nodiscard]] inline PaneTitleFacts paneTitleFacts(const Protocol::Sync::PaneFacts& facts, const Protocol::Sync::StatusRecords& records)
{
    PaneTitleFacts title;
    title.programTitle = facts.programTitle;
    title.currentDirectory = facts.currentDirectory;
    title.homeDirectory = facts.homeDirectory;
    title.processChain = facts.processChain;
    title.processChainWithArguments = facts.processChainWithArguments;
    title.shellCommandName = facts.shellCommandName;
    title.ttyName = facts.terminalName;
    title.programStatus = programActivityLabel(records.store);
    if (title.programStatus.empty() && facts.indicator)
        title.programStatus = facts.indicator->label;
    title.columns = facts.columns;
    title.rows = facts.rows;
    title.end = { .didEnd = facts.end.didEnd, .hasStatus = facts.end.hasStatus, .wasSignalled = facts.end.wasSignalled, .code = facts.end.code };
    title.hostName = facts.hostName;
    return title;
}

} // namespace Ubiquitty::App
