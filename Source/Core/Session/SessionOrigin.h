// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// SessionOrigin.h — where in the human's arrangement a session sits: the
// workspace holding it, the window and tab showing it, the pane it occupies and
// its own name. Anything that has to point a person at the terminal speaking —
// a capability-consent prompt, a desktop banner's place line — carries these
// facts rather than assembling its own words, so the same place reads the same
// way wherever it is named. The two say it in different forms, because they
// are read differently: a prompt is read through and must not be forgeable
// (`describeSessionOrigin`), a banner is glanced at and must be short
// (`describeBannerPlace`).
//
// Every field is optional and every arm fills what it holds: a frontend knows
// its window and tab, a daemon-attached session knows its workspace, a local
// pane knows its index. An empty field is a fact nobody has, and the
// description omits it rather than inventing a placeholder for it.
//
// Plain C++ (no ObjC/Swift/AppKit) so the wording is host-tested.

#include "Core/Title/BannerText.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::Session {

/// Names the places a session can be pointed at by, outermost first.
///
/// Strings rather than identifiers: the reader recognises the title on the tab,
/// not the session's wire id, and an arm holding only an ordinal writes "2"
/// here. Every field is optional, and a field nobody knows stays empty rather
/// than carrying a placeholder a prompt would then have to explain.
struct SessionOrigin {
    std::string workspace; // hosted workspace name; empty for the local one
    std::string window; // the window's title, or its ordinal label
    std::string tab; // the tab's title, or its ordinal label
    std::string pane; // the pane's label within its tab
    std::string session; // the session's own name

    // Whether nothing about the origin is known, which is the caller's cue to
    // say so in words rather than print an empty line.
    [[nodiscard]] bool isEmpty() const
    {
        return workspace.empty() && window.empty() && tab.empty() && pane.empty() && session.empty();
    }
};

// The longest an origin name is shown at. A title is program-settable and runs
// to thousands of bytes; a place the reader recognises is a few words, and the
// rest is either padding or an attempt to push the labelled parts off the line.
inline constexpr std::size_t originNameLimit = 64;

// `value` with everything that could forge this line's own structure removed:
// the quotation marks and the separator `describeSessionOrigin` writes between
// parts, the C0 bytes and DEL, and any run of blanks folded to one. Truncated on
// a UTF-8 boundary, with an ellipsis when it was cut.
//
// A window or tab title is set by the program in the session, which for a
// consent prompt is the very program the reader is being asked about. Left
// whole, a title reading `Build” › pane “1”. Drive access is held by …` renders
// as the prompt's own sentences, in the prompt's own words, inside the same
// paragraph. Nothing here trusts the value; it only stops it looking trusted.
[[nodiscard]] inline std::string sanitizedOriginName(std::string_view value)
{
    static constexpr std::string_view forged[] = { "“", "”", "›" };
    std::string out;
    out.reserve(value.size());
    bool didFold = false;
    for (std::size_t index = 0; index < value.size();) {
        bool skipped = false;
        for (const std::string_view mark : forged) {
            if (value.compare(index, mark.size(), mark) == 0) {
                index += mark.size();
                skipped = true;
                break;
            }
        }
        if (skipped)
            continue;
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte < 0x20 || byte == 0x7F || byte == ' ' || byte == '\t') {
            if (!out.empty() && !didFold) {
                out += ' ';
                didFold = true;
            }
            ++index;
            continue;
        }
        didFold = false;
        out += value[index];
        ++index;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    if (out.size() <= originNameLimit)
        return out;
    std::size_t cut = originNameLimit;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
        --cut;
    out.resize(cut);
    out += "…";
    return out;
}

// The origin as one line — `workspace “Beam” › window “Build” › tab “src”` —
// outermost place first, each part labelled so a bare title is never mistaken
// for the kind of thing it names. Empty when the origin is empty. Every value is
// sanitized, because several of them are titles the session's own program sets.
[[nodiscard]] inline std::string describeSessionOrigin(const SessionOrigin& origin)
{
    std::string description;
    const auto append = [&description](std::string_view kind, const std::string& value) {
        const std::string shown = sanitizedOriginName(value);
        if (shown.empty())
            return;
        if (!description.empty())
            description += " › ";
        description += kind;
        description += " “";
        description += shown;
        description += "”";
    };
    append("workspace", origin.workspace);
    append("window", origin.window);
    append("tab", origin.tab);
    append("pane", origin.pane);
    append("session", origin.session);
    return description;
}

/// What a frontend knows about where a banner's session sits.
///
/// Facts only: which names it holds and how many windows, tabs and panes the
/// person has to tell apart. Which of them a banner states, in what order and
/// at what length is ``describeBannerPlace``'s to say, so every frontend's
/// banner names a place the same way.
struct BannerPlaceFacts {
    /// The connection's name as the window's chrome states it. Empty for a
    /// session on this machine reached through no host.
    std::string host;
    /// The hosted workspace's name; empty for a session in none.
    std::string workspace;
    /// The name a person put on the window; empty when nobody did.
    std::string windowName;
    /// The window's title and the tab's, each as composed for a reader with
    /// the host mark left off — the host is a level of its own here.
    std::string windowTitle;
    std::string tabTitle;
    /// The name a person put on the pane; empty when nobody did.
    std::string paneLabel;
    /// The number the frontend knows the pane by; zero when it has none.
    int paneNumber = 0;
    /// How many windows the app shows, tabs the window holds and panes the
    /// tab holds. One is also what a frontend with no such level reports.
    int windowCount = 1;
    int tabCount = 1;
    int paneCount = 1;
};

// The widths a place line's parts are held to, in columns.
//
// A banner's line of body text is about forty columns in either system's
// banner; the usual place — a short host, a short workspace, a title and a
// pane number — is thirty to forty, so it fits one line. A title is the part
// that runs long and is program-set, so it has the largest share and still
// cannot take the line: thirty-two columns holds a directory and a command.
// A host or workspace name is a word or two a person chose.
inline constexpr int maxBannerHostColumns = 16;
inline constexpr int maxBannerWorkspaceColumns = 16;
inline constexpr int maxBannerTitleColumns = 32;
inline constexpr int maxBannerPaneColumns = 12;

// The most columns the whole line takes, marks included: two lines of a
// banner's body, where the system starts cutting at four. Reached only when
// several parts are at their own bounds at once, and then the parts after the
// host give way, last first, none below `minBannerPartColumns` — enough of a
// name to recognize it by.
inline constexpr int maxBannerPlaceColumns = 64;
inline constexpr int minBannerPartColumns = 8;

/// Whether `words` appears in `text` as whole words.
///
/// A match is a run of `text` that starts and ends at the text's edges or at
/// a space, the only separator sanitized banner text keeps between words.
///
/// - Parameters:
///   - text: Banner text as `Title::sanitizedBannerText` leaves it.
///   - words: The words looked for, sanitized the same way.
/// - Returns: Whether `words` is non-empty and appears in `text` as whole words.
[[nodiscard]] inline bool containsBannerWords(std::string_view text, std::string_view words)
{
    if (words.empty() || words.size() > text.size())
        return false;
    for (std::size_t at = text.find(words); at != std::string_view::npos; at = text.find(words, at + 1)) {
        const std::size_t end = at + words.size();
        const bool isStartAtBoundary = at == 0 || text[at - 1] == ' ';
        const bool isEndAtBoundary = end == text.size() || text[end] == ' ';
        if (isStartAtBoundary && isEndAtBoundary)
            return true;
    }
    return false;
}

/// The place a banner states beneath its message — `Studio › Main › ~ — make › 2`.
///
/// Outermost first: the host, the workspace, the window — by the name a
/// person gave it, else its title — the tab's title, the pane. No kind words and no quotation marks; ``describeSessionOrigin``
/// keeps those for a consent prompt, where a title must not pass for the
/// prompt's own sentences. Here a title can only ever be a title, because
/// every name is sanitized and loses the level mark.
///
/// A part is written when it distinguishes something:
///
/// - an empty one is not;
/// - one that repeats the part written before it is not, which makes one part
///   of a window and a tab that share a title;
/// - the pane is not unless its tab holds several, and then by its label, or
///   else its number alone.
///
/// A session on this machine that is the only pane of the only tab of the only
/// window has nothing to be told apart from, and the line is empty. A hosted
/// session always states its host, however alone it is.
///
/// Each part is cut to its own bound, so one long title cannot push the host
/// and workspace off the line, and the line to ``maxBannerPlaceColumns`` by
/// shortening the parts after the host from the last one back.
///
/// - Parameter facts: What the frontend holds about the session's place.
/// - Returns: The line, or empty when there is no place to state.
[[nodiscard]] inline std::string describeBannerPlace(const BannerPlaceFacts& facts)
{
    const bool isAlone = facts.windowCount <= 1 && facts.tabCount <= 1 && facts.paneCount <= 1;
    const std::string host = Title::sanitizedBannerText(facts.host, Title::LevelMarks::Remove);
    if (host.empty() && isAlone)
        return {};

    static constexpr std::string_view levelMark = " › ";
    static constexpr int levelMarkColumns = 3;
    static constexpr std::size_t maxParts = 5;
    std::array<std::string, maxParts> parts;
    std::array<int, maxParts> columns {};
    std::size_t count = 0;
    // The part as it would be written is what a repeat is judged on: two
    // names that differ only in what sanitizing removes read the same.
    const auto append = [&](std::string_view sanitized, int maxColumns) {
        std::string shown = Title::boundedBannerText(sanitized, maxColumns);
        if (shown.empty() || (count > 0 && parts[count - 1] == shown))
            return;
        columns[count] = Title::bannerTextColumns(shown);
        parts[count] = std::move(shown);
        ++count;
    };
    append(host, maxBannerHostColumns);
    const bool hasHost = count > 0;
    append(Title::sanitizedBannerText(facts.workspace, Title::LevelMarks::Remove), maxBannerWorkspaceColumns);
    // A window someone named is known by that name; the title woven for it
    // carries the name among other parts, and is not what they would look for.
    const bool isWindowNamed = !facts.windowName.empty();
    std::string window = Title::sanitizedBannerText(isWindowNamed ? facts.windowName : facts.windowTitle, Title::LevelMarks::Remove);
    std::string tab = Title::sanitizedBannerText(facts.tabTitle, Title::LevelMarks::Remove);
    // A window title woven around the tab's title already says the tab's, and
    // a tab title that holds the window's says the window's: the one holding
    // the other is written, judged before either is cut. A name a person gave
    // the window is always written.
    if (window != tab) {
        if (containsBannerWords(window, tab))
            tab.clear();
        else if (!isWindowNamed && containsBannerWords(tab, window))
            window.clear();
    }
    append(window, maxBannerTitleColumns);
    append(tab, maxBannerTitleColumns);
    if (facts.paneCount > 1) {
        if (const std::string label = Title::sanitizedBannerText(facts.paneLabel, Title::LevelMarks::Remove); !label.empty())
            append(label, maxBannerPaneColumns);
        else if (facts.paneNumber > 0)
            append(std::to_string(facts.paneNumber), maxBannerPaneColumns);
    }
    if (count == 0)
        return {};

    int total = levelMarkColumns * static_cast<int>(count - 1);
    for (std::size_t index = 0; index < count; ++index)
        total += columns[index];
    const std::size_t firstShortenable = hasHost ? 1 : 0;
    for (std::size_t index = count; index > firstShortenable && total > maxBannerPlaceColumns; --index) {
        const std::size_t part = index - 1;
        if (columns[part] <= minBannerPartColumns)
            continue;
        const int wanted = std::max(minBannerPartColumns, columns[part] - (total - maxBannerPlaceColumns));
        parts[part] = Title::boundedBannerText(parts[part], wanted);
        const int shortened = Title::bannerTextColumns(parts[part]);
        total -= columns[part] - shortened;
        columns[part] = shortened;
    }

    std::string line;
    for (std::size_t index = 0; index < count; ++index) {
        if (index > 0)
            line += levelMark;
        line += parts[index];
    }
    return line;
}

} // namespace Ubiquitty::Session
