// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// SessionOutlineText.h — a session outline as text and as JSON.
//
// The outline (SessionOutline.h) is the rows a listing draws, with every
// decision already made: what each row is called, which levels fold, what its
// status and its colour are, how long ago anything under it wrote. A frontend
// with widgets draws those rows. Two readers have none: a command line prints
// them as lines, and a page or a tool call takes them as JSON. Both spellings
// are here so that neither decides anything again: every word, mark and field
// is read off a row.
//
// `outlineText` is one line a row, indented two spaces a level:
//
//     + work [nine] #prod  blocked 1/3  2 people, 1 agent  workspace:9
//       - ops  blocked 1/3  window:4
//           make all (make)  blocked: permission  /srv/app  132x43  out 2m  pane:7
//
// A line opens with `>` when the row matches the search. Then, in order and
// each only when the row has it: `+` or `-` for a row that is folded or
// unfolded; `*` for the row the host's layout has in front; the title, with
// the secondary title in parentheses; each label as `[label]`; each tag as
// `#tag`, or an untitled one as `●` and its spoken name; the status; progress; the tab colour a program set as `tab #rrggbb`;
// `bell` and `notified` for attention nobody has seen; the badge in braces;
// the directory; the grid; `out` and `in` with how long ago the pane last
// wrote and was typed into; who is attached; what a folded row holds; the
// workspace's state, on the workspace's row and on every row under one
// nothing runs in; and the identity a verb addresses the row by. Fields are two spaces apart, so a title with a space in it is
// still one field.
//
// `outlineJSON` is the same rows as objects, every field named, for a reader
// that draws them itself.
//

#include "Core/App/SessionOutline.h"
#include "Core/App/StatusRollup.h"
#include "Core/Codec/JSON.h"
#include "Core/Color.h"
#include "Core/OSC/Activity.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/Session/Identity.h"
#include "Core/Session/SessionTree.h"
#include "Core/Themes/StatusColors.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::App {

/// What a printed outline writes before an untitled tag's spoken name, where a
/// titled tag is written `#name`: U+25CF BLACK CIRCLE, the dot the tag is
/// drawn as.
inline constexpr std::string_view untitledTagMark = "\xE2\x97\x8F";

/// A status kind's word, as a listing and a row's JSON spell it.
///
/// - Parameter kind: The kind.
/// - Returns: The word; empty for `None`, which a listing does not say.
[[nodiscard]] constexpr std::string_view statusKindWord(StatusKind kind)
{
    switch (kind) {
    case StatusKind::None:
        return "";
    case StatusKind::Idle:
        return "idle";
    case StatusKind::Running:
        return "running";
    case StatusKind::Working:
        return "working";
    case StatusKind::Done:
        return "done";
    case StatusKind::Attention:
        return "attention";
    case StatusKind::Blocked:
        return "blocked";
    case StatusKind::Failed:
        return "failed";
    }
    return "";
}

/// What a blocked program waits for, as a word.
///
/// - Parameter kind: The block's kind.
/// - Returns: The word; empty when the program did not say.
[[nodiscard]] constexpr std::string_view blockKindWord(OSC::ProgramBlockKind kind)
{
    switch (kind) {
    case OSC::ProgramBlockKind::None:
        return "";
    case OSC::ProgramBlockKind::Permission:
        return "permission";
    case OSC::ProgramBlockKind::Question:
        return "question";
    case OSC::ProgramBlockKind::Auth:
        return "credential";
    }
    return "";
}

/// A progress report's state, as a word.
///
/// - Parameter state: One of `OSC::ProgressState`.
/// - Returns: The word; empty for no report.
[[nodiscard]] constexpr std::string_view progressStateWord(std::uint8_t state)
{
    switch (state) {
    case OSC::ProgressNormal:
        return "normal";
    case OSC::ProgressError:
        return "error";
    case OSC::ProgressIndeterminate:
        return "indeterminate";
    case OSC::ProgressPaused:
        return "paused";
    default:
        return "";
    }
}

/// A listed level's word.
///
/// - Parameter level: The level.
/// - Returns: `workspace`, `window`, `tab` or `pane`.
[[nodiscard]] constexpr std::string_view listedLevelWord(ListedLevel level)
{
    switch (level) {
    case ListedLevel::Workspace:
        return "workspace";
    case ListedLevel::Window:
        return "window";
    case ListedLevel::Tab:
        return "tab";
    case ListedLevel::Pane:
        return "pane";
    }
    return "";
}

/// A status colour slot's word, the one a theme file keys it by.
///
/// - Parameter slot: The slot.
/// - Returns: The word.
[[nodiscard]] constexpr std::string_view statusSlotWord(Themes::StatusColorSlot slot)
{
    switch (slot) {
    case Themes::StatusColorSlot::Failed:
        return "failed";
    case Themes::StatusColorSlot::Blocked:
        return "blocked";
    case Themes::StatusColorSlot::Attention:
        return "attention";
    case Themes::StatusColorSlot::Done:
        return "done";
    case Themes::StatusColorSlot::Working:
        return "working";
    case Themes::StatusColorSlot::Neutral:
        return "neutral";
    case Themes::StatusColorSlot::Progress:
        return "progress";
    }
    return "";
}

/// A workspace state's word.
///
/// - Parameter state: The state.
/// - Returns: `live`, `restored` or `restorable`.
[[nodiscard]] constexpr std::string_view workspaceStateWord(Session::TreeWorkspaceState state)
{
    switch (state) {
    case Session::TreeWorkspaceState::Live:
        return "live";
    case Session::TreeWorkspaceState::RestoredAfterHostRestart:
        return "restored";
    case Session::TreeWorkspaceState::Restorable:
        return "restorable";
    }
    return "";
}

/// Words an age the way a listing shows it: the largest whole unit.
///
/// - Parameter seconds: How long ago, in seconds.
/// - Returns: `now` under five seconds, then `12s`, `3m`, `2h` or `5d`.
[[nodiscard]] inline std::string ageWord(std::uint64_t seconds)
{
    if (seconds < 5)
        return "now";
    if (seconds < 60)
        return std::to_string(seconds) + "s";
    if (seconds < 3600)
        return std::to_string(seconds / 60) + "m";
    if (seconds < 86'400)
        return std::to_string(seconds / 3600) + "h";
    return std::to_string(seconds / 86'400) + "d";
}

/// The word a row's one mark is written as.
///
/// - Parameter accessory: The mark.
/// - Returns: `none`, `progress`, `bell`, `colouredBell`, `statusDot` or
///   `notificationDot`; `none` for a mark a row never shows.
[[nodiscard]] constexpr std::string_view accessoryWord(App::TabAccessory accessory)
{
    switch (accessory) {
    case App::TabAccessory::ProgressRing:
        return "progress";
    case App::TabAccessory::Bell:
        return "bell";
    case App::TabAccessory::ColouredBell:
        return "colouredBell";
    case App::TabAccessory::StatusDot:
        return "statusDot";
    case App::TabAccessory::NotificationDot:
        return "notificationDot";
    case App::TabAccessory::None:
    case App::TabAccessory::ActivityDots:
    case App::TabAccessory::ShortcutHint:
        break;
    }
    return "none";
}

/// Spells a colour as `#rrggbb`.
///
/// - Parameters:
///   - red: The red channel.
///   - green: The green channel.
///   - blue: The blue channel.
/// - Returns: The seven characters.
[[nodiscard]] inline std::string colorWord(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    std::array<char, 8> text {};
    std::snprintf(text.data(), text.size(), "#%02x%02x%02x", static_cast<unsigned>(red), static_cast<unsigned>(green), static_cast<unsigned>(blue));
    return std::string(text.data());
}

namespace SessionOutlineTextDetail {

/// Counts people and agents in words.
///
/// - Parameter attendance: Who is attached.
/// - Returns: `2 people, 1 agent`, either half left out at zero.
[[nodiscard]] inline std::string attendanceWords(const Session::TreeAttendance& attendance)
{
    std::string words;
    if (attendance.people > 0)
        words = std::to_string(attendance.people) + (attendance.people == 1 ? " person" : " people");
    if (attendance.agents > 0)
        words += std::string(words.empty() ? "" : ", ") + std::to_string(attendance.agents) + (attendance.agents == 1 ? " agent" : " agents");
    return words;
}

/// Counts something in words.
///
/// - Parameters:
///   - count: How many.
///   - one: The word for one of them.
/// - Returns: `1 tab`, `3 tabs`.
[[nodiscard]] inline std::string counted(std::uint32_t count, std::string_view one)
{
    return std::to_string(count) + " " + std::string(one) + (count == 1 ? "" : "s");
}

/// A row's status as a listing says it.
///
/// - Parameter status: The row's rolled status.
/// - Returns: `blocked: permission` for one pane, `blocked 1/3` for a level
///   over several; empty for a row of no status.
[[nodiscard]] inline std::string statusWords(const RolledStatus& status)
{
    std::string words(statusKindWord(status.kind));
    if (words.empty())
        return words;
    if (status.paneCount > 1)
        words += " " + std::to_string(status.count) + "/" + std::to_string(status.paneCount);
    if (status.kind == StatusKind::Blocked && status.blockKind != OSC::ProgramBlockKind::None)
        words += ": " + std::string(blockKindWord(status.blockKind));
    return words;
}

/// A row's progress as a listing says it.
///
/// - Parameter progress: The row's progress sample.
/// - Returns: `40%`, `paused 40%`, `progress error` or `in progress`; empty
///   with no report.
[[nodiscard]] inline std::string progressWords(const OSC::ProgressSample& progress)
{
    switch (progress.state) {
    case OSC::ProgressNormal:
        return std::to_string(progress.value) + "%";
    case OSC::ProgressPaused:
        return "paused " + std::to_string(progress.value) + "%";
    case OSC::ProgressError:
        return "progress error";
    case OSC::ProgressIndeterminate:
        return "in progress";
    default:
        return {};
    }
}

/// What a folded row holds, in words.
///
/// - Parameter summary: The row's summary.
/// - Returns: `2 windows, 3 tabs, 5 panes`, a level of none left out.
[[nodiscard]] inline std::string summaryWords(const OutlineSummary& summary)
{
    std::string holds;
    if (summary.windows > 0)
        holds += counted(summary.windows, "window") + ", ";
    if (summary.tabs > 0)
        holds += counted(summary.tabs, "tab") + ", ";
    return holds + counted(summary.panes, "pane");
}

/// What a drawn row says of its workspace's state.
///
/// - Parameter row: The row.
/// - Returns: `restored` or `restorable` on a workspace's own row; empty for
///   a live workspace and for every row under one, which is drawn as its
///   workspace's state without saying it.
[[nodiscard]] inline std::string stateWords(const OutlineRow& row)
{
    if (row.level != ListedLevel::Workspace || row.workspaceState == Session::TreeWorkspaceState::Live)
        return {};
    return std::string(workspaceStateWord(row.workspaceState));
}

/// What a workspace's row says of how long the workspace lasts.
///
/// - Parameter row: The row.
/// - Returns: `ends when detached` on the row of a workspace that ends when
///   the last person on it detaches; empty otherwise.
[[nodiscard]] inline std::string lifetimeWords(const OutlineRow& row)
{
    return row.level == ListedLevel::Workspace && row.endsWhenDetached ? std::string("ends when detached") : std::string();
}

/// The identity a verb addresses a row by, as text.
///
/// - Parameter row: The row.
/// - Returns: `pane:7`; empty for a row with no identity of its own.
[[nodiscard]] inline std::string identityWord(const OutlineRow& row)
{
    return row.id.number != 0 ? Session::Identity::format(row.id) : std::string();
}

} // namespace SessionOutlineTextDetail

/// Prints an outline as lines, one a row; the layout is described at the top
/// of this file.
///
/// - Parameter outline: The outline.
/// - Returns: The lines, each ended by a newline; empty for no rows.
[[nodiscard]] inline std::string outlineText(const SessionOutline& outline)
{
    using namespace SessionOutlineTextDetail;
    std::string text;
    for (const OutlineRow& row : outline.rows) {
        std::string line;
        line += row.isMatch ? '>' : ' ';
        line.append(static_cast<std::size_t>(row.depth > 0 ? row.depth : 0) * 2, ' ');
        line += row.isExpandable ? (row.isExpanded ? "- " : "+ ") : "  ";
        if (row.isHostCurrent)
            line += '*';
        line += row.title;
        if (!row.secondaryTitle.empty())
            line += " (" + row.secondaryTitle + ")";
        for (const std::string& chip : row.labelChips)
            line += " [" + chip + "]";
        // An untitled tag is a dot, written as one with the words a reader
        // would hear for it, so a line never ends in a bare "#".
        for (const Session::ResolvedTag& tag : row.tags)
            line += tag.isUntitled() ? " " + std::string(untitledTagMark) + Session::tagSpokenName(tag) : " #" + tag.name;
        const auto field = [&line](const std::string& words) {
            if (!words.empty())
                line += "  " + words;
        };
        field(statusWords(row.status));
        field(progressWords(row.status.progress));
        if (row.status.tabColor)
            field("tab " + colorWord(row.status.tabColor->red, row.status.tabColor->green, row.status.tabColor->blue));
        if (row.status.hasUnseenBell)
            field("bell");
        if (row.status.hasUnseenNotification)
            field("notified");
        if (!row.badge.empty())
            field("{" + row.badge + "}");
        field(row.directory);
        if (row.columns > 0 && row.rows > 0)
            field(std::to_string(row.columns) + "x" + std::to_string(row.rows));
        if (row.sinceOutput.isKnown)
            field("out " + ageWord(row.sinceOutput.seconds));
        if (row.sinceInput.isKnown)
            field("in " + ageWord(row.sinceInput.seconds));
        field(attendanceWords(row.attendance));
        if (row.collapsedSummary)
            field(summaryWords(*row.collapsedSummary));
        // A workspace says its state unless it is live. Every row
        // under one nothing runs in says so too: it has no identity, and the
        // word is why.
        const bool isRestorable = row.workspaceState == Session::TreeWorkspaceState::Restorable; // bool-ok: a local reading
        if (isRestorable || (row.level == ListedLevel::Workspace && row.workspaceState != Session::TreeWorkspaceState::Live))
            field(std::string(workspaceStateWord(row.workspaceState)));
        field(lifetimeWords(row));
        field(identityWord(row));
        text += line + '\n';
    }
    return text;
}

/// The second line a compact reader gives a row, when the row needs a
/// person: what its newest unseen notification says, else its status in
/// words when that is failed, blocked or asking for attention.
///
/// An open row has none: the row beneath that bears it has. A row with
/// nothing to say has none, and a compact reader draws it on one line.
///
/// - Parameter row: The row.
/// - Returns: The line; empty for a row that needs none.
[[nodiscard]] inline std::string compactSecondLine(const OutlineRow& row)
{
    if (row.isExpanded)
        return {};
    if (!row.attentionTitle.empty())
        return row.attentionTitle;
    if (!row.attentionBody.empty())
        return row.attentionBody;
    switch (row.status.kind) {
    case StatusKind::Failed:
    case StatusKind::Blocked:
    case StatusKind::Attention:
        return SessionOutlineTextDetail::statusWords(row.status);
    case StatusKind::None:
    case StatusKind::Idle:
    case StatusKind::Running:
    case StatusKind::Working:
    case StatusKind::Done:
        break;
    }
    return {};
}

/// One line a hover or a detail popover shows for a row: what the fact is,
/// and the fact.
struct OutlineDetailLine {
    std::string label;
    std::string value;

    friend bool operator==(const OutlineDetailLine&, const OutlineDetailLine&) = default;
};

/// The lines a hover or a detail popover shows for a row, worded and in the
/// order they are shown: what runs and where, how large, its status, what a
/// notification said, who is attached, when it last wrote and was typed
/// into, and its labels and tags. A fact the row does not have has no line.
///
/// - Parameter row: The row.
/// - Returns: The lines; none for a row with nothing to add to its title.
[[nodiscard]] inline std::vector<OutlineDetailLine> detailLines(const OutlineRow& row)
{
    using namespace SessionOutlineTextDetail;
    std::vector<OutlineDetailLine> lines;
    const auto add = [&lines](std::string label, std::string value) {
        if (!value.empty())
            lines.push_back({ .label = std::move(label), .value = std::move(value) });
    };
    const auto ago = [](std::uint64_t seconds) {
        const std::string age = ageWord(seconds);
        return age == "now" ? std::string("just now") : age + " ago";
    };
    const auto joined = [](const auto& items, const auto& text) {
        std::string list;
        for (const auto& item : items) {
            if (!list.empty())
                list.append(", ");
            list.append(text(item));
        }
        return list;
    };
    add("Program", row.program);
    add("Directory", row.directory);
    if (row.columns > 0 && row.rows > 0)
        add("Size", std::to_string(row.columns) + "×" + std::to_string(row.rows));
    add("Status", statusWords(row.status));
    add("Progress", progressWords(row.status.progress));
    add("Notification", row.attentionTitle.empty() ? row.attentionBody : row.attentionBody.empty() ? row.attentionTitle
                                                                                                   : row.attentionTitle + ": " + row.attentionBody);
    add("Attached", attendanceWords(row.attendance));
    if (row.sinceOutput.isKnown)
        add("Last output", ago(row.sinceOutput.seconds));
    if (row.sinceInput.isKnown)
        add("Last input", ago(row.sinceInput.seconds));
    add("Labels", joined(row.labelChips, [](const std::string& label) { return label; }));
    add("Tags", joined(row.tags, [](const Session::ResolvedTag& tag) { return Session::tagSpokenName(tag); }));
    if (row.level == ListedLevel::Workspace && row.workspaceState != Session::TreeWorkspaceState::Live)
        add("State", std::string(workspaceStateWord(row.workspaceState)));
    return lines;
}

/// Spells an outline as JSON: `{revision, matchCount, rows: [row]}`.
///
/// A row carries every field a listing draws, under the names used here:
/// `level`, `depth`, `id` (absent for a row with no identity of its own),
/// `folded` (`{window?, tab?, pane?}`, the identities a folded row stands
/// for), `recorded` (`{workspace, level, window, tab}`, for a row of a
/// restorable workspace), `state`, `endsWhenDetached`, `title`,
/// `secondaryTitle`, `labels`, `tags` (`[{name, color?, loud, icon?,
/// iconWeight?, showName?, label?}]`: `showName` is false for a titled tag
/// whose rule hides its name, and `label` is the spoken words of a tag drawn
/// without its name), `status`
/// (`{kind, count, panes, blockKind?, progress?, bell, notification, badges,
/// indicatorColor?, tabColor?}`), `statusSlot`, `statusColor`, `badge`,
/// `sinceOutputSeconds` and `sinceInputSeconds` (absent when unknown),
/// `attendance` (`{people, agents}`), `program`, `directory`, `columns`,
/// `rows`, `hostCurrent`, `expandable`, `expanded`, `collapsed` (`{windows,
/// tabs, panes, worstStatus, worstStatusCount}`, on a folded row), `match`,
/// and `words`. A colour is `#rrggbb`.
///
/// `words` is what `outlineText` says for the row, field by field, so a
/// reader that draws rows words nothing itself: `{status, progress,
/// sinceOutput, sinceInput, attendance, collapsed, state}`, each present only
/// when the line would say it.
///
/// - Parameter outline: The outline.
/// - Returns: The object.
[[nodiscard]] inline Codec::JSON::Value outlineJSON(const SessionOutline& outline)
{
    using Codec::JSON::Value;
    using Session::Identity::format;
    const auto text = [](std::string_view words) { return Value::string(std::string(words)); };
    const auto number = [](std::uint64_t count) { return Value::number(static_cast<double>(count)); };
    Value rows = Value::array();
    for (const OutlineRow& row : outline.rows) {
        Value entry = Value::object();
        entry.set("level", text(listedLevelWord(row.level)));
        entry.set("depth", number(static_cast<std::uint64_t>(row.depth > 0 ? row.depth : 0)));
        if (row.id.number != 0)
            entry.set("id", Value::string(format(row.id)));
        if (row.foldedWindow || row.foldedTab || row.foldedPane) {
            Value folded = Value::object();
            if (row.foldedWindow)
                folded.set("window", Value::string(format(*row.foldedWindow)));
            if (row.foldedTab)
                folded.set("tab", Value::string(format(*row.foldedTab)));
            if (row.foldedPane)
                folded.set("pane", Value::string(format(*row.foldedPane)));
            entry.set("folded", std::move(folded));
        }
        if (row.recordedKey) {
            Value recorded = Value::object();
            recorded.set("workspace", number(row.recordedKey->workspaceID));
            recorded.set("level", text(listedLevelWord(row.recordedKey->level)));
            recorded.set("window", number(row.recordedKey->window));
            recorded.set("tab", number(row.recordedKey->tab));
            entry.set("recorded", std::move(recorded));
        }
        entry.set("state", text(workspaceStateWord(row.workspaceState)));
        entry.set("endsWhenDetached", Value::boolean(row.endsWhenDetached));
        entry.set("title", Value::string(row.title));
        entry.set("secondaryTitle", Value::string(row.secondaryTitle));
        Value labels = Value::array();
        for (const std::string& chip : row.labelChips)
            labels.push(Value::string(chip));
        entry.set("labels", std::move(labels));
        Value tags = Value::array();
        for (const Session::ResolvedTag& tag : row.tags) {
            Value described = Value::object();
            described.set("name", Value::string(tag.name));
            if (tag.color.set)
                described.set("color", Value::string(colorWord(tag.color.red, tag.color.green, tag.color.blue)));
            described.set("loud", Value::boolean(tag.prominence == Session::TagProminence::Loud));
            // The icon a frontend draws in the dot's place, by its catalog
            // name, so a reader of the outline can draw the same marker.
            if (!tag.icon.empty()) {
                described.set("icon", Value::string(tag.icon));
                described.set("iconWeight", Value::string(Session::tagIconWeightToString(tag.iconWeight)));
            }
            // A tag whose rule hides its name is drawn as an untitled one is.
            if (!tag.isUntitled() && !tag.showsName)
                described.set("showName", Value::boolean(false));
            // A tag drawn without its name, untitled or hidden, is a dot to a
            // reader, spoken by these words.
            if (!tag.drawsName())
                described.set("label", Value::string(Session::tagSpokenName(tag)));
            tags.push(std::move(described));
        }
        entry.set("tags", std::move(tags));
        Value status = Value::object();
        status.set("kind", text(statusKindWord(row.status.kind)));
        status.set("count", number(row.status.count));
        status.set("panes", number(row.status.paneCount));
        if (row.status.blockKind != OSC::ProgramBlockKind::None)
            status.set("blockKind", text(blockKindWord(row.status.blockKind)));
        if (row.status.progress.state != OSC::ProgressNone) {
            Value progress = Value::object();
            progress.set("state", text(progressStateWord(row.status.progress.state)));
            progress.set("value", number(row.status.progress.value));
            status.set("progress", std::move(progress));
        }
        status.set("bell", Value::boolean(row.status.hasUnseenBell));
        status.set("notification", Value::boolean(row.status.hasUnseenNotification));
        status.set("badges", number(row.status.badgeCount));
        if (row.status.indicatorColor)
            status.set("indicatorColor", Value::string(colorWord(row.status.indicatorColor->red, row.status.indicatorColor->green, row.status.indicatorColor->blue)));
        if (row.status.tabColor)
            status.set("tabColor", Value::string(colorWord(row.status.tabColor->red, row.status.tabColor->green, row.status.tabColor->blue)));
        entry.set("status", std::move(status));
        entry.set("statusSlot", text(statusSlotWord(row.statusSlot)));
        entry.set("statusColor", Value::string(colorWord(row.statusColor.red, row.statusColor.green, row.statusColor.blue)));
        entry.set("badge", Value::string(row.badge));
        if (row.sinceOutput.isKnown)
            entry.set("sinceOutputSeconds", number(row.sinceOutput.seconds));
        if (row.sinceInput.isKnown)
            entry.set("sinceInputSeconds", number(row.sinceInput.seconds));
        Value attendance = Value::object();
        attendance.set("people", number(row.attendance.people));
        attendance.set("agents", number(row.attendance.agents));
        entry.set("attendance", std::move(attendance));
        entry.set("program", Value::string(row.program));
        if (!row.programStatus.empty())
            entry.set("programStatus", Value::string(row.programStatus));
        entry.set("directory", Value::string(row.directory));
        entry.set("columns", number(static_cast<std::uint64_t>(row.columns > 0 ? row.columns : 0)));
        entry.set("rows", number(static_cast<std::uint64_t>(row.rows > 0 ? row.rows : 0)));
        entry.set("hostCurrent", Value::boolean(row.isHostCurrent));
        entry.set("expandable", Value::boolean(row.isExpandable));
        entry.set("expanded", Value::boolean(row.isExpanded));
        if (row.collapsedSummary) {
            Value collapsed = Value::object();
            collapsed.set("windows", number(row.collapsedSummary->windows));
            collapsed.set("tabs", number(row.collapsedSummary->tabs));
            collapsed.set("panes", number(row.collapsedSummary->panes));
            collapsed.set("worstStatus", text(statusKindWord(row.collapsedSummary->worstStatus)));
            collapsed.set("worstStatusCount", number(row.collapsedSummary->worstStatusCount));
            entry.set("collapsed", std::move(collapsed));
        }
        entry.set("match", Value::boolean(row.isMatch));
        // What a row's actions read.
        {
            Value open = Value::object();
            const auto how = [](Session::AttachOpening opening) -> std::string_view {
                switch (opening) {
                case Session::AttachOpening::BySession:
                    return "session";
                case Session::AttachOpening::ByRestoring:
                    return "restore";
                case Session::AttachOpening::ByRevealing:
                    return "reveal";
                case Session::AttachOpening::Cannot:
                    break;
                }
                return "cannot";
            };
            open.set("how", text(how(row.open.opening)));
            if (row.open.sessionID != 0)
                open.set("session", Value::string(format({ Session::Identity::ObjectKind::Session, row.open.sessionID })));
            if (row.open.paneID != 0)
                open.set("pane", Value::string(format({ Session::Identity::ObjectKind::Pane, row.open.paneID })));
            if (row.open.workspaceID != 0)
                open.set("workspace", Value::string(format({ Session::Identity::ObjectKind::Workspace, row.open.workspaceID })));
            open.set("workspaceName", Value::string(row.open.workspaceName));
            if (row.open.obstacle != Session::AttachObstacle::None)
                open.set("whyNot", text(Session::attachObstacleSentence(row.open.obstacle)));
            entry.set("open", std::move(open));
            Value own = Value::array();
            for (const OutlineOwnValues& level : row.own) {
                Value held = Value::object();
                held.set("id", Value::string(format(level.id)));
                held.set("label", Value::string(level.label));
                Value tags = Value::array();
                for (const std::string& tag : level.tags)
                    tags.push(Value::string(tag));
                held.set("tags", std::move(tags));
                held.set("name", Value::string(level.name));
                own.push(std::move(held));
            }
            entry.set("own", std::move(own));
            if (row.tabPlace) {
                Value place = Value::object();
                place.set("tab", Value::string(format(row.tabPlace->tab)));
                place.set("window", Value::string(format(row.tabPlace->window)));
                place.set("index", number(row.tabPlace->index));
                place.set("tabCount", number(row.tabPlace->tabCount));
                place.set("canMoveEarlier", Value::boolean(row.tabPlace->canMoveEarlier));
                place.set("canMoveLater", Value::boolean(row.tabPlace->canMoveLater));
                Value accepted = Value::array();
                for (const std::uint32_t target : row.tabPlace->acceptedIndices)
                    accepted.push(number(target));
                place.set("acceptedIndices", std::move(accepted));
                Value others = Value::array();
                for (const OutlineTabDestination& other : row.tabPlace->otherWindows) {
                    Value destination = Value::object();
                    destination.set("window", Value::string(format(other.window)));
                    destination.set("title", Value::string(other.title));
                    destination.set("index", number(other.index));
                    others.push(std::move(destination));
                }
                place.set("otherWindows", std::move(others));
                entry.set("tabPlace", std::move(place));
            }
            if (!row.attentionTitle.empty() || !row.attentionBody.empty()) {
                Value attention = Value::object();
                attention.set("title", Value::string(row.attentionTitle));
                attention.set("body", Value::string(row.attentionBody));
                entry.set("attention", std::move(attention));
            }
            entry.set("accessory", text(accessoryWord(row.accessory)));
            if (row.here != OutlineHere::Elsewhere)
                entry.set("here", Value::string(row.here == OutlineHere::Here ? "is" : "holds"));
            if (row.accessoryInk)
                entry.set("accessoryInk", Value::string(colorWord(row.accessoryInk->red, row.accessoryInk->green, row.accessoryInk->blue)));
        }
        Value words = Value::object();
        const auto say = [&words](const char* name, const std::string& said) {
            if (!said.empty())
                words.set(name, Value::string(said));
        };
        say("status", SessionOutlineTextDetail::statusWords(row.status));
        say("progress", SessionOutlineTextDetail::progressWords(row.status.progress));
        if (row.sinceOutput.isKnown)
            say("sinceOutput", ageWord(row.sinceOutput.seconds));
        if (row.sinceInput.isKnown)
            say("sinceInput", ageWord(row.sinceInput.seconds));
        say("attendance", SessionOutlineTextDetail::attendanceWords(row.attendance));
        if (row.collapsedSummary)
            say("collapsed", SessionOutlineTextDetail::summaryWords(*row.collapsedSummary));
        say("state", SessionOutlineTextDetail::stateWords(row));
        say("hostCurrent", std::string(hostCurrentWords(row)));
        say("secondLine", compactSecondLine(row));
        Value details = Value::array();
        for (OutlineDetailLine& line : detailLines(row)) {
            Value detail = Value::object();
            detail.set("label", Value::string(std::move(line.label)));
            detail.set("value", Value::string(std::move(line.value)));
            details.push(std::move(detail));
        }
        entry.set("details", std::move(details));
        entry.set("words", std::move(words));
        rows.push(std::move(entry));
    }
    Value described = Value::object();
    described.set("revision", number(outline.revision));
    described.set("matchCount", number(outline.matchCount));
    described.set("rows", std::move(rows));
    return described;
}

} // namespace Ubiquitty::App
