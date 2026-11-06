// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Title/Format.h — the config-gated rich-title weave.
//
// A window/tab/pane title is "<directory> — <app title> — <program status> —
// <process chain> — <cols×rows>", where each segment is independently gated by a
// [title] show-flag (titleWindow* / titleTab*). Frontends supply the resolved
// flags and segment contents; Core owns their order and inclusion rules.
//
// The gating + weave lives here once so the arms agree. The *inputs* stay
// platform-resolved: the caller passes the already-built lead (the
// working-directory segment via Title::titleDirectorySegment), osc (the host
// OSC 0/2 title), and chain (PAL::Process::formatChain over the gathered process
// subtree). Passing the segments in keeps this pure + host-testable — it touches
// no Values singleton, no Terminal, no platform string types.
//
// The answer comes in two shapes, and the second is built on the first:
//
//   resolveTitle() -> ResolvedTitle   the PARTS, each named by its role, with
//                                     the gated and the empty already dropped
//   joinTitle()    -> std::string     those parts in Core's canonical order,
//                                     joined with the caller's separator
//
// Parts first, because a frontend that only ever received the joined sentence
// had to cut it back up to draw it: the macOS title row searched the string
// for every em dash to kern around it, and the tab bake found where the dim
// folder run ended by testing for a prefix and then for " — " or "—". Handing
// over the parts lets a surface style by ROLE, and lets one that wants a
// second line, or a larger face for a single part, arrange them itself without
// Core changing at all. The order stays Core's, because it is a ruling rather
// than a preference and arms that each ordered for themselves would drift.
//
// buildRichTitle remains the one-call spelling of join(resolve(...)) for the
// callers that only want the line, so the gate and the order have exactly one
// implementation behind both entry points.
//
// Note: the join drops empty parts, so a caller may either pass "" for a hidden
// segment or rely on the show-flag here; doing the gate here keeps the policy
// ("show_process off must hide the chain even when one exists") in one place
// rather than smeared across each call site's segment builder.
//
// A remote pane supplies these inputs through Sync::PaneFacts; the title
// formatter reads the same parts whether the terminal is local or remote.

#include "PAL/Process.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Title {

// The [title] segment show-flags, resolved by the caller from the
// surface-appropriate Values keys (titleWindow* / titleTab* / titlePane*). A
// false flag drops that segment from the woven title regardless of whether the
// caller built a non-empty value for it.
struct TitleShowFlags {
    bool showsDirectory; // the leading working-directory segment
    bool showsAppTitle; // the OSC 0/2 host title
    bool showsShellCommand; // the session-leader shell's name
    bool showsProcess; // the "child ‹ parent" process chain
    bool showsProfileName; // the active profile's name
    bool showsTTYName; // the replica tty basename ("ttys003")
    bool showsDimensions; // the trailing "cols×rows" segment
    bool showsProgramStatus; // the label a program gave its status colour (OSC 21337)
};

// The already-resolved segment texts (any may be empty; an empty segment never
// appears regardless of its flag). baseTitle is the per-surface static Title
// text and has no flag — the text itself gates it (empty = absent).
struct TitleSegments {
    std::string_view baseTitle;
    std::string_view lead;
    std::string_view osc;
    std::string_view shellCommand;
    std::string_view chain;
    std::string_view profileName;
    std::string_view ttyName;
    std::string_view programStatus; // OSC 21337's `status`, empty when no colour is set
};

/// How a session's process ended, once it has.
///
/// `didEnd` is the one fact a title and a caret both turn on: there is nothing
/// running here any more. The status is a separate question because it is not
/// knowable at the same instant — end-of-file arrives when the last replica
/// descriptor closes, and the child is reaped after that. A session with
/// `hasStatus` false has still ended; only the number is missing, and a title
/// that waited for it would sit on a dead process name in the meantime.
struct SessionEnd {
    bool didEnd = false; ///< Nothing is running on this session.
    bool hasStatus = false; ///< The status below has been collected.
    bool wasSignalled = false; ///< A signal ended it, rather than its own exit.
    int code = 0; ///< The exit status, or the signal number when signalled.
};

/// A title's parts, by role, with the gated and the empty already dropped.
///
/// Keyed, not a list: there is no order to read out of this and no index to
/// get wrong. A frontend asks for the field it means to draw. Any field may be
/// empty, which is how a part that is switched off, or has nothing to show, is
/// spelled — so a surface that wants only the folder tests `directory` rather
/// than parsing a finished sentence to find where the folder ended.
///
/// Owned strings, not views: the answer outlives the call on every arm, one
/// handing it to AppKit, one to Swift, one to GTK, and a view into the
/// caller's inputs would be a lifetime rule every one of them had to keep.
struct ResolvedTitle {
    std::string baseTitle; ///< The per-surface static [title] text.
    std::string directory; ///< The leading working-directory segment.
    std::string appTitle; ///< The OSC 0/2 host title, or a name a person typed.
    std::string chain; ///< The "child ‹ parent" process chain.
    std::string shellCommand; ///< The session-leader shell's name.
    std::string profileName; ///< The active profile's name.
    std::string ttyName; ///< The replica tty basename ("ttys003").
    std::string dimensions; ///< The trailing "cols×rows" readout.
    std::string programStatus; ///< The label a program gave its status ("Waiting").
};

/// The parts a title is made of, gated by the show-flags.
///
/// The gate lives here rather than at each call site, so "show_process off
/// hides the chain even when one exists" is one rule instead of one per
/// frontend. A flag that is off, and a value that is empty, both come back as
/// an empty field — the two are the same fact to whoever draws it.
[[nodiscard]] inline ResolvedTitle resolveTitle(const TitleShowFlags& show, const TitleSegments& segments, int columns, int rows, const SessionEnd& end = {})
{
    const auto gated = [](bool isShown, std::string_view text) -> std::string {
        return isShown ? std::string { text } : std::string {};
    };
    ResolvedTitle parts;
    parts.baseTitle = std::string { segments.baseTitle };
    parts.directory = gated(show.showsDirectory, segments.lead);
    parts.appTitle = gated(show.showsAppTitle, segments.osc);
    parts.chain = gated(show.showsProcess, segments.chain);
    parts.shellCommand = gated(show.showsShellCommand, segments.shellCommand);
    parts.profileName = gated(show.showsProfileName, segments.profileName);
    parts.ttyName = gated(show.showsTTYName, segments.ttyName);
    parts.programStatus = gated(show.showsProgramStatus, segments.programStatus);
    if (show.showsDimensions && columns > 0 && rows > 0) {
        parts.dimensions = std::to_string(columns);
        parts.dimensions.append("×");
        parts.dimensions.append(std::to_string(rows));
    }
    // A session with nothing running names no process, and puts nothing in its
    // place. Both process parts go, not only the chain: the shell segment names
    // the same departed process by another spelling, so dropping one and
    // keeping the other would still advertise something that is not there.
    //
    // No stand-in marker here. A word in this position reads as the name of a
    // program called "exited", because that is what every other segment in the
    // line is. The end is said where it cannot be misread — in the terminal
    // itself, as a line of its own, written when the session ends. What the
    // title is left with is what is still true: the folder.
    //
    // A program's status label goes with them, for the same reason: it is the
    // state a program reported about itself, and once that program has ended
    // "Working" names work nobody is doing.
    if (end.didEnd) {
        parts.chain.clear();
        parts.shellCommand.clear();
        parts.programStatus.clear();
    }
    return parts;
}

/// Which part of a title a piece of text is.
///
/// A frontend styles by this rather than by finding punctuation in a finished
/// string: the directory recedes, the dimensions sit in their own smaller
/// face, and a surface that wants a second line decides which parts go on it.
/// Adding a case here is additive — a consumer that walks the order renders
/// whatever it is given, and one that reads named fields is untouched.
enum class TitlePart : std::uint8_t {
    BaseTitle,
    Directory,
    AppTitle,
    Chain,
    ShellCommand,
    ProfileName,
    TTYName,
    Dimensions,
    ProgramStatus
};

/// How many parts a title has, so a caller can size a walk over them.
inline constexpr std::size_t titlePartCount = 9;

/// Core's canonical reading order.
///
/// A ruling rather than a preference: the shell FOLLOWS the process chain
/// ("-zsh — zsh", ruled on title-shell-command-shows-login-wrapper). A
/// program's status follows the app title and precedes the process chain. It lives
/// here once so no arm re-derives it and the arms cannot drift apart. A
/// frontend arranging its own — a second line, a larger face for one part —
/// walks its own order instead and loses nothing.
[[nodiscard]] inline std::array<TitlePart, titlePartCount> canonicalTitleOrder()
{
    return { TitlePart::BaseTitle, TitlePart::Directory, TitlePart::AppTitle, TitlePart::ProgramStatus,
        TitlePart::Chain, TitlePart::ShellCommand, TitlePart::ProfileName, TitlePart::TTYName,
        TitlePart::Dimensions };
}

/// One part's text, for a caller walking an order rather than naming fields.
///
/// The named fields stay the direct way to reach a part a frontend styles
/// specifically; this is how a generic render walks whichever order it holds.
[[nodiscard]] inline std::string_view titleText(const ResolvedTitle& parts, TitlePart part)
{
    switch (part) {
    case TitlePart::BaseTitle:
        return parts.baseTitle;
    case TitlePart::Directory:
        return parts.directory;
    case TitlePart::AppTitle:
        return parts.appTitle;
    case TitlePart::Chain:
        return parts.chain;
    case TitlePart::ShellCommand:
        return parts.shellCommand;
    case TitlePart::ProfileName:
        return parts.profileName;
    case TitlePart::TTYName:
        return parts.ttyName;
    case TitlePart::Dimensions:
        return parts.dimensions;
    case TitlePart::ProgramStatus:
        return parts.programStatus;
    }
    return {};
}

/// The parts in Core's canonical order, joined with `separator`.
///
/// Empty when every part is empty, which is the caller's cue to apply its own
/// fallback (the application name, or a remote host label). (A per-tab ⌘N hint
/// has no part — the tab bar's shortcut-hint accessory owns that affordance.)
/// How a separator is spelled.
///
/// Every place that needs the spelling asks here — the join, the offsets into a
/// joined line, and each frontend that styles one run of that line differently
/// from the rest — so a line and whatever measures it cannot disagree about
/// what stands between its parts. A caller that writes the character out for
/// itself is right only while the surface it draws happens to be woven with
/// that spelling, and goes wrong in silence when the other one reaches it.
///
/// - Parameter separator: The spelling the weave uses.
/// - Returns: The characters `joinTitle` places between two parts.
[[nodiscard]] inline std::string_view titleSeparatorSpelling(PAL::Process::TitleSeparator separator)
{
    return separator == PAL::Process::TitleSeparator::Bare ? "—" : " — ";
}

/// The separator spelling, owned, for a frontend that composes with it.
///
/// Swift reaches `std::string` and not `std::string_view`, so a surface that
/// builds the same spelling the join used needs the value rather than a view of
/// one whose owner it cannot see.
///
/// - Parameter separator: The spelling the weave uses.
/// - Returns: A copy of what `titleSeparatorSpelling` names.
[[nodiscard]] inline std::string titleSeparatorText(PAL::Process::TitleSeparator separator)
{
    return std::string(titleSeparatorSpelling(separator));
}

/// The arrow a window's title puts before what its program says it is
/// doing: the process chain's own step.
///
/// A copy for Swift, which reaches `std::string` and not `std::string_view`.
///
/// - Returns: A copy of ``PAL::Process::chainArrow``.
[[nodiscard]] inline std::string chainArrowText()
{
    return std::string(PAL::Process::chainArrow);
}

[[nodiscard]] inline std::string joinTitle(const ResolvedTitle& parts, PAL::Process::TitleSeparator separator)
{
    std::string out;
    for (const TitlePart part : canonicalTitleOrder()) {
        const std::string_view text = titleText(parts, part);
        if (text.empty())
            continue;
        if (!out.empty())
            out.append(titleSeparatorSpelling(separator));
        out.append(text);
    }
    return out;
}

/// The byte boundary before one part in Core's joined order, with empty parts
/// omitted. A title row uses this to draw its program status between the text
/// before and after that part without searching the finished string.
[[nodiscard]] inline std::size_t joinedPrefixLengthBefore(const ResolvedTitle& parts, TitlePart boundary, PAL::Process::TitleSeparator separator)
{
    std::size_t length = 0;
    for (const TitlePart part : canonicalTitleOrder()) {
        if (part == boundary)
            break;
        const std::string_view text = titleText(parts, part);
        if (text.empty())
            continue;
        if (length > 0)
            length += titleSeparatorSpelling(separator).size();
        length += text.size();
    }
    return length;
}

/// How many bytes of a joined title belong to its LEADING part, separator
/// included.
///
/// For a surface that styles by role: the tab label dims the folder run and
/// draws the rest at full strength, so it needs to know where that run ends.
///
/// Core owns the join rule, so a frontend does not search the finished line for
/// the boundary: that would re-derive the rule from its output and break when
/// the separator changes or a leading part is a prefix of the line for another
/// reason.
///
/// Zero when there is nothing to dim differently: no parts, or only one, in
/// which case no separator was emitted and the whole line is the leading part.
///
/// - Parameters:
///   - parts: The resolved title.
///   - separator: The spelling `joinTitle` was given.
/// - Returns: Bytes from the start of the joined line, separator included.
[[nodiscard]] inline std::size_t joinedLeadingPartLength(const ResolvedTitle& parts, PAL::Process::TitleSeparator separator)
{
    std::string_view leading;
    bool didFindLeading = false;
    for (const TitlePart part : canonicalTitleOrder()) {
        const std::string_view text = titleText(parts, part);
        if (text.empty())
            continue;
        if (!didFindLeading) {
            leading = text;
            didFindLeading = true;
            continue;
        }
        // A second part exists, so the join emitted a separator after the first.
        const std::string_view spelling = titleSeparatorSpelling(separator);
        return leading.size() + spelling.size();
    }
    return 0;
}

/// The same answer for a caller holding the finished line and its leading part
/// rather than the resolved title.
///
/// An interim shape, and named as one. The parts-based overload above is the
/// real answer — a surface should style by ROLE and never see the joined line
/// at all — but the two tab bakes still take strings, and moving them onto
/// parts is a change to title rendering that no host test can see. What this
/// removes meanwhile is the frontend GUESSING at Core's separator: the bake
/// tested for " — " and then for "—", trying both spellings because it did not
/// know which one the join had used. Now it says which one it asked for.
///
/// - Parameters:
///   - joined: The finished line.
///   - leading: The part that leads it.
///   - separator: The spelling the join was given.
/// Takes `const std::string&` rather than views, because Swift calls it:
/// `std.string_view(aSwiftString)` compiles and hands over a view into a
/// temporary that has already died. Views here would be a trap set for the one
/// caller this exists for.
///
/// - Returns: Bytes of the leading run, separator included; zero when the line
///   does not begin with that part followed by that separator.
[[nodiscard]] inline std::size_t joinedLeadingPartLength(const std::string& joined, const std::string& leading, PAL::Process::TitleSeparator separator)
{
    const std::string_view line { joined };
    const std::string_view head { leading };
    if (head.empty() || !line.starts_with(head))
        return 0;
    const std::string_view spelling = titleSeparatorSpelling(separator);
    const std::string_view tail = line.substr(head.size());
    return tail.starts_with(spelling) ? head.size() + spelling.size() : 0;
}

/// The woven title, for a caller that wants the line rather than the parts.
///
/// One call spelling of `joinTitle(resolveTitle(...))`, so the gate and the
/// order have exactly one implementation behind both entry points.
[[nodiscard]] inline std::string buildRichTitle(const TitleShowFlags& show, const TitleSegments& segments, int columns, int rows, PAL::Process::TitleSeparator separator, const SessionEnd& end = {})
{
    return joinTitle(resolveTitle(show, segments, columns, rows, end), separator);
}

/// What a tab's state adds to its label.
///
/// A window title carries none of these: a window's own title bar is the thing
/// a mark would be pointing at, so a mark on it points at itself.
struct TabLabelMarks {
    bool isPinned = false; // bool-ok: names the one state it reports
    /// The host the tab's session lives on, as the frontend spells it; empty
    /// for a plain local shell. Tabs of two hosts in one strip read alike
    /// without it.
    std::string hostName {};
};

/// The separator between a host's name and the label it marks: the one a
/// tab's label already joins its parts with, so the host reads as the label's
/// first part rather than as a mark in a glyph of its own.
inline constexpr std::string_view hostMarkSeparator = " — ";

/// The pin a pinned tab's label opens with.
inline constexpr std::string_view pinMark = "📌 ";

/// What a tab's marks put before its label.
///
/// The marks LEAD, so the state reads down a column of labels at a glance
/// rather than at the end of whatever each label happens to say: the pin
/// first, then the host, then the label. A window's tag is not text in the
/// label: the strip draws it as a dot at the tab's leading edge, where it
/// neither moves the centred label nor varies with the label's length.
///
/// - Parameter marks: What this tab's state adds.
/// - Returns: The text before the label, empty when the tab earns no mark.
[[nodiscard]] inline std::string tabLabelMarkPrefix(const TabLabelMarks& marks)
{
    std::string prefix;
    if (marks.isPinned)
        prefix = pinMark;
    if (!marks.hostName.empty()) {
        prefix.append(marks.hostName);
        prefix.append(hostMarkSeparator);
    }
    return prefix;
}

/// Whether a label already names a host, so a host mark before it would say
/// the name twice.
///
/// It does when, without regard to case or the spaces around it, the label
/// is the host's name, opens with it, or ends with it — a label that fell back
/// to the host's name, or a title a session set to its own hostname. The name
/// has to stand as a word: `build` names no host in `build-server — zsh`.
///
/// - Parameters:
///   - label: The woven label.
///   - hostName: The host's name as the frontend spells it.
/// - Returns: Whether the label already carries the name.
[[nodiscard]] inline bool labelNamesHost(std::string_view label, std::string_view hostName)
{
    const auto trim = [](std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            text.remove_suffix(1);
        return text;
    };
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    const auto same = [&](std::string_view left, std::string_view right) {
        if (left.size() != right.size())
            return false;
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (lower(left[index]) != lower(right[index]))
                return false;
        }
        return true;
    };
    // A byte that continues a word: ASCII letters, digits, '-', '_', '.', and
    // every byte of a multibyte character, which this cannot classify.
    const auto isWordByte = [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
            || c == '_' || c == '.';
    };
    const std::string_view text = trim(label);
    const std::string_view host = trim(hostName);
    if (host.empty() || text.size() < host.size())
        return false;
    if (same(text.substr(0, host.size()), host) && (text.size() == host.size() || !isWordByte(text[host.size()])))
        return true;
    const std::size_t tail = text.size() - host.size();
    return same(text.substr(tail), host) && (tail == 0 || !isWordByte(text[tail - 1]));
}

/// The marks a label earns once what it already says is taken into account:
/// the host mark goes when the label names the host itself.
///
/// - Parameters:
///   - label: The woven label.
///   - marks: What the tab's state adds.
/// - Returns: The marks to draw.
[[nodiscard]] inline TabLabelMarks marksForLabel(const std::string& label, TabLabelMarks marks)
{
    if (labelNamesHost(label, marks.hostName))
        marks.hostName.clear();
    return marks;
}

/// The label with its marks, ``tabLabelMarkPrefix(_:)`` leading it.
///
/// A bake that styles the label's own segments measures them past the prefix,
/// which is why the prefix is a function of its own rather than something a
/// frontend finds in the marked text again. A label that already names the
/// host is not marked with it again (``marksForLabel(_:_:)``); a bake asks for
/// the prefix of the marks that returns.
///
/// - Parameters:
///   - label: The woven label.
///   - marks: What this tab's state adds.
/// - Returns: The label, marked.
[[nodiscard]] inline std::string markTabLabel(const std::string& label, const TabLabelMarks& marks)
{
    std::string marked = tabLabelMarkPrefix(marksForLabel(label, marks));
    marked.append(label);
    return marked;
}

} // namespace Ubiquitty::Title
