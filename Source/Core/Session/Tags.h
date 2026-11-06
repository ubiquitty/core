// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Session/Tags.h — window tags: the named, coloured markers a window
// wears so what it is connected to is legible before it is read.
//
// A tag reaches a window three ways, and they compose rather than override: a
// `[[tags]]` rule matches the connected host or the active profile, the user
// sets one on the window itself, and the session sets one from the terminal
// stream. The rule is the one that can be trusted as a safety signal, because
// nothing has to remember to apply it — a window on a production host is
// marked by virtue of being on it.
//
// Only the rule half lives here: matching a host/profile against the config and
// resolving a colour name to a colour. The other two sources supply their tags
// already resolved, and every frontend renders the union.
//
// A rule also decides how its tag is drawn: whether the marker carries the
// name, and which of the window's title surfaces (its title bar, its tab, the
// header above each pane) draw it at all. Those answers travel on the
// resolved tag, and a frontend asks them of it rather than deciding them.
// Rules draw in the order the file lists them.

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Preferences/Preferences.h"
#include "Core/Session/ProcessState.h"
#include "Core/Session/SessionLocation.h"

namespace Ubiquitty::Session {

/// How loudly a tag announces itself.
///
/// `Quiet` earns a labelled marker beside the title. `Loud` additionally tints
/// the whole title bar, which is the signal terminals and their users actually
/// converged on for production — big enough to register before anything is
/// read. It is worth reserving: a window where everything shouts says nothing.
enum class TagProminence : std::uint8_t {
    Quiet,
    Loud,
};

/// Resolves the `prominence` token, defaulting to quiet for anything else so an
/// unknown spelling cannot silently make a tag shout.
///
/// - Parameter token: The config spelling.
/// - Returns: The prominence it names.
[[nodiscard]] inline TagProminence tagProminenceFromString(std::string_view token)
{
    if (token.size() != 4)
        return TagProminence::Quiet;
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return (lower(token[0]) == 'l' && lower(token[1]) == 'o' && lower(token[2]) == 'u' && lower(token[3]) == 'd')
        ? TagProminence::Loud
        : TagProminence::Quiet;
}

/// The config spelling of a prominence, for writing an entry back to the file.
///
/// - Parameter prominence: The value to spell.
/// - Returns: Its token.
[[nodiscard]] inline const char* tagProminenceToString(TagProminence prominence)
{
    return prominence == TagProminence::Loud ? "loud" : "quiet";
}

/// Which of an icon's two weights a tag draws it in.
///
/// `Fill` is the default: a solid glyph carries a tag's colour the way the
/// dot it replaces does, where an outline at tab size is mostly air.
enum class TagIconWeight : std::uint8_t {
    Fill,
    Regular,
};

/// Resolves the `icon_weight` token, defaulting to fill for anything else.
///
/// - Parameter token: The config spelling.
/// - Returns: The weight it names.
[[nodiscard]] inline TagIconWeight tagIconWeightFromString(std::string_view token)
{
    constexpr std::string_view regular = "regular";
    if (token.size() != regular.size())
        return TagIconWeight::Fill;
    for (std::size_t index = 0; index < token.size(); ++index) {
        const char c = token[index];
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (lower != regular[index])
            return TagIconWeight::Fill;
    }
    return TagIconWeight::Regular;
}

/// The config spelling of an icon weight, for writing an entry back to the
/// file.
///
/// - Parameter weight: The value to spell.
/// - Returns: Its token.
[[nodiscard]] inline const char* tagIconWeightToString(TagIconWeight weight)
{
    return weight == TagIconWeight::Regular ? "regular" : "fill";
}

/// Whether two tag names are the same name, ignoring case.
///
/// Literal comparison, never a glob: a tag whose name contains `*` is a name
/// with a star in it, not a pattern.
///
/// - Parameters:
///   - left: One name.
///   - right: The other.
/// - Returns: True when the two spell the same name.
[[nodiscard]] inline bool tagNamesEqual(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (lower(left[index]) != lower(right[index]))
            return false;
    }
    return true;
}

/// Where a window's tag came from.
///
/// A frontend that edits a window's tags edits only the hand-set ones; a tag a
/// rule supplied is shown as the rule's, so an editor neither lists it as the
/// reader's own nor offers to remove what the next match would put back.
enum class TagSource : std::uint8_t {
    /// A `[[tags]]` rule matched the window's host or profile.
    Rule,
    /// The reader set it on the window, or a session announced it.
    Manual,
};

/// A tag ready to draw: a display name and the colour of its marker.
///
/// An unrecognised colour name leaves `color.set` false, which is a tag that
/// names something without claiming a colour for it. An empty name is an
/// untitled tag: only a rule can supply one, and only with a colour, so it is
/// drawn as a dot of that colour and spoken by ``tagSpokenName``. A titled tag
/// whose rule hides its name draws the mark an untitled one does, its icon or
/// its dot, and keeps its name for everything else.
struct ResolvedTag {
    /// The tag's display name, as the config or the user spelled it; empty
    /// for an untitled rule tag.
    std::string name;
    /// The marker's colour, unset when the tag names no colour.
    Preferences::ColorEntry color;
    /// Whether the tag also tints the title bar.
    TagProminence prominence = TagProminence::Quiet;
    /// Whether a rule or a hand supplied the tag.
    TagSource source = TagSource::Manual;
    /// The `[[tags]]` entry that supplied the tag's colour, by its position in
    /// file order; -1 when no entry did.
    int ruleIndex = -1;
    /// What that entry matches, as ``tagRulePattern`` spells it; empty when
    /// the tag is not a rule's.
    std::string rulePattern {};
    /// The palette name of the colour, lower case, when it is one of the
    /// seven; empty for a `#rrggbb` colour or none.
    std::string colorName {};
    /// The Phosphor icon drawn in the marker's place, empty for none.
    std::string icon {};
    /// Which weight the icon is drawn in.
    TagIconWeight iconWeight = TagIconWeight::Fill;
    /// Whether the marker may draw the name, from the rule's `show_name`.
    bool showsName = true;
    /// Whether the window's title bar draws the tag, from `show_in_title_bar`.
    bool showsInTitleBar = true;
    /// Whether the window's tab draws the tag, from `show_in_tabs`.
    bool showsInTabs = true;
    /// Whether a pane's header draws the tag, from `show_in_pane_headers`.
    bool showsInPaneHeaders = true;

    /// Whether the tag has no title and is drawn as a bare dot.
    [[nodiscard]] bool isUntitled() const { return name.empty(); }

    /// Whether the marker draws the name: a titled tag whose rule shows it.
    ///
    /// A marker that does not is drawn as an untitled tag's is, its icon in
    /// its colour or a dot of the colour, and is still spoken, listed and
    /// matched by its name.
    [[nodiscard]] bool drawsName() const { return showsName && !isUntitled(); }

    /// Whether one of a window's title surfaces draws the tag.
    ///
    /// A list of everything a window or a session wears, an outline among
    /// them, draws the tag whatever this says.
    ///
    /// - Parameter surface: The title bar (`Window`), the tab, or a pane's
    ///   header.
    /// - Returns: True when the tag's rule places it there.
    [[nodiscard]] bool isDrawnOn(TitleSurface surface) const
    {
        switch (surface) {
        case TitleSurface::Window:
            return showsInTitleBar;
        case TitleSurface::Tab:
            return showsInTabs;
        case TitleSurface::Pane:
            return showsInPaneHeaders;
        }
        return true;
    }
};

/// Resolves one of the seven system tag colour names.
///
/// Each value is the colour the system itself resolves that name to, measured
/// rather than chosen by eye, so a tag reads as the same colour it does in
/// the system's own tag lists. Both spellings of grey are taken: the system
/// resolves only one of them, and which one is not a thing a config author
/// should have to know.
///
/// - Parameters:
///   - name: The colour name, matched without regard to case.
///   - out: Receives the colour when the name is one of the seven.
/// - Returns: True when the name named a colour.
/// One entry of the system tag palette: a colour name and its measured RGB.
struct TagPaletteEntry {
    std::string_view name;
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

/// The seven system tag colours every picker offers, in Finder's own order,
/// with both spellings of grey resolving and only one offered.
inline constexpr TagPaletteEntry tagPalette[] = {
    { "red", 252, 122, 107 },
    { "orange", 247, 184, 82 },
    { "yellow", 242, 222, 84 },
    { "green", 191, 217, 84 },
    { "blue", 105, 179, 255 },
    { "purple", 204, 161, 222 },
    { "gray", 181, 181, 181 },
};

/// How many colours a picker offers — the palette without the second grey.
inline constexpr std::size_t tagPaletteCount = sizeof(tagPalette) / sizeof(tagPalette[0]);

/// How many colours a picker offers, as a count a frontend language can read.
///
/// The table itself is a C array of views, which a Swift caller cannot index;
/// these two answer for it so one palette serves every picker.
[[nodiscard]] inline int tagPaletteSize() { return static_cast<int>(tagPaletteCount); }

/// The name of the palette colour at an index.
///
/// - Parameter index: Below ``tagPaletteSize``.
/// - Returns: The name, or empty past the end.
[[nodiscard]] inline std::string tagPaletteNameAt(int index)
{
    if (index < 0 || index >= static_cast<int>(tagPaletteCount))
        return {};
    return std::string { tagPalette[static_cast<std::size_t>(index)].name };
}

/// The packed 0xRRGGBB of the palette colour at an index.
///
/// - Parameter index: Below ``tagPaletteSize``.
/// - Returns: The colour, or zero past the end.
[[nodiscard]] inline std::uint32_t tagPaletteColorAt(int index)
{
    if (index < 0 || index >= static_cast<int>(tagPaletteCount))
        return 0;
    const TagPaletteEntry& entry = tagPalette[static_cast<std::size_t>(index)];
    return (static_cast<std::uint32_t>(entry.red) << 16) | (static_cast<std::uint32_t>(entry.green) << 8) | entry.blue;
}

[[nodiscard]] inline bool tagColorFromName(std::string_view name, Preferences::ColorEntry& out)
{
    // The second spelling of grey resolves to the first: the system accepts
    // only one, and which one is not a thing a config author should know.
    const std::string_view lookup = tagNamesEqual(name, "grey") ? std::string_view { "gray" } : name;
    for (const TagPaletteEntry& candidate : tagPalette) {
        if (!tagNamesEqual(candidate.name, lookup))
            continue;
        out.red = candidate.red;
        out.green = candidate.green;
        out.blue = candidate.blue;
        out.alpha = 255;
        out.set = true;
        return true;
    }
    return false;
}

/// The palette name a colour value resolves to: lower case, grey spelled
/// gray, empty when the value is not one of the seven.
///
/// - Parameter value: A `[[tags]]` colour, as the config spells it.
/// - Returns: The palette name, or empty.
[[nodiscard]] inline std::string tagPaletteName(std::string_view value)
{
    const std::string_view lookup = tagNamesEqual(value, "grey") ? std::string_view { "gray" } : value;
    for (const TagPaletteEntry& candidate : tagPalette) {
        if (tagNamesEqual(candidate.name, lookup))
            return std::string { candidate.name };
    }
    return {};
}

/// Resolves a `[[tags]]` colour value: a palette name, else a `#rrggbb`
/// literal.
///
/// - Parameters:
///   - value: The colour as the config spells it.
///   - out: Receives the colour when the value names one.
/// - Returns: True when the value named a colour.
[[nodiscard]] inline bool tagColorFromValue(const std::string& value, Preferences::ColorEntry& out)
{
    if (value.empty())
        return false;
    if (tagColorFromName(value, out))
        return true;
    return Preferences::parseHexColor(value, out) && out.set;
}

/// Whether a tag name has any character that is not a space or a tab.
///
/// A name of only blanks would draw as an empty capsule; it is an untitled
/// tag, read and written as one.
///
/// - Parameter name: The name as the config or the editor spelled it.
/// - Returns: True when the name titles the tag.
[[nodiscard]] inline bool isTagNameTitled(std::string_view name)
{
    for (const char c : name) {
        if (c != ' ' && c != '\t')
            return true;
    }
    return false;
}

/// The patterns one `match_host` or `match_profile` spelling holds: each
/// comma-separated pattern, trimmed, with the empty ones dropped.
///
/// A machine is often reached under several names (its own hostname, a
/// tailnet name, a saved label), and one rule names them all. No host or
/// profile name holds a comma, so a comma only ever separates.
///
/// - Parameter spelling: The key's value, a TOML array already joined.
/// - Returns: The patterns, in the order written; views into `spelling`.
[[nodiscard]] inline std::vector<std::string_view> tagPatternList(std::string_view spelling)
{
    std::vector<std::string_view> patterns;
    while (!spelling.empty()) {
        const std::size_t comma = spelling.find(',');
        std::string_view piece = spelling.substr(0, comma);
        while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t'))
            piece.remove_prefix(1);
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t'))
            piece.remove_suffix(1);
        if (!piece.empty())
            patterns.push_back(piece);
        if (comma == std::string_view::npos)
            break;
        spelling.remove_prefix(comma + 1);
    }
    return patterns;
}

/// Whether a `[[tags]]` entry describes a marker anything can draw.
///
/// A rule needs a title, a colour, or both. A title alone is a labelled
/// capsule; a colour alone is a dot. An untitled entry also needs a host or
/// profile pattern: with neither it marks no window by rule, and with no name
/// no window can be given it by hand, so it would mean nothing. Such entries
/// are refused by the editor and skipped by the resolver.
///
/// - Parameter rule: The entry to judge.
/// - Returns: True when the entry has a title, or a colour that resolves and
///   a pattern to match.
[[nodiscard]] inline bool isValidTagRule(const Preferences::TagEntry& rule)
{
    if (isTagNameTitled(rule.name))
        return true;
    if (tagPatternList(rule.matchHost).empty() && tagPatternList(rule.matchProfile).empty())
        return false;
    Preferences::ColorEntry color;
    return tagColorFromValue(rule.color, color);
}

/// What a rule matches, spelled for a reader: its host patterns, its profile
/// patterns, or the host patterns with the profile ones beside them, named,
/// since a host list is itself joined by commas.
///
/// - Parameter rule: The entry.
/// - Returns: The patterns, or empty for an entry that matches nothing.
[[nodiscard]] inline std::string tagRulePattern(const Preferences::TagEntry& rule)
{
    const auto joined = [](std::string_view spelling) {
        std::string out;
        for (const std::string_view pattern : tagPatternList(spelling)) {
            if (!out.empty())
                out += ", ";
            out += pattern;
        }
        return out;
    };
    const std::string hosts = joined(rule.matchHost);
    const std::string profiles = joined(rule.matchProfile);
    if (hosts.empty())
        return profiles;
    if (profiles.empty())
        return hosts;
    return hosts + " (profile " + profiles + ")";
}

/// The name a screen reader speaks for a tag.
///
/// A titled tag is its title. An untitled one is a dot, and a dot announced
/// as nothing is a marker a VoiceOver user cannot find: it is spoken as its
/// colour's name ("Blue"), or, for a `#rrggbb` colour that has no name, as
/// the pattern of the rule that supplied it.
///
/// - Parameter tag: The resolved tag.
/// - Returns: The words to speak; never empty for a tag a rule could supply.
[[nodiscard]] inline std::string tagSpokenName(const ResolvedTag& tag)
{
    if (!tag.isUntitled())
        return tag.name;
    if (!tag.colorName.empty()) {
        std::string spoken = tag.colorName;
        spoken[0] = static_cast<char>(spoken[0] - 'a' + 'A');
        return spoken;
    }
    return tag.rulePattern;
}

/// Whether two resolved tags are the same marker.
///
/// Titled tags are the same when their names are, ignoring case. Untitled tags
/// have no name to compare; two of them are one marker when they are the same
/// colour and the same icon, and an untitled tag is never the same as a titled
/// one.
///
/// - Parameters:
///   - left: One tag.
///   - right: The other.
/// - Returns: True when one of the two would draw the other again.
[[nodiscard]] inline bool isSameTag(const ResolvedTag& left, const ResolvedTag& right)
{
    if (left.isUntitled() != right.isUntitled())
        return false;
    if (!left.isUntitled())
        return tagNamesEqual(left.name, right.name);
    return left.color.set == right.color.set && left.color.red == right.color.red && left.color.green == right.color.green
        && left.color.blue == right.color.blue && left.icon == right.icon && left.iconWeight == right.iconWeight;
}

/// Whether a glob matches a value, where `*` stands for any run of characters.
///
/// The comparison ignores case: a host name is not case-sensitive, and a rule
/// that matched only the spelling the user happened to type would mark some
/// production windows and not others. Iterative with a single backtrack point,
/// so a pattern of many stars cannot make this exponential on a hostile value.
///
/// - Parameters:
///   - pattern: The glob.
///   - value: The host or profile name to test.
/// - Returns: True when the pattern matches the whole value.
[[nodiscard]] inline bool tagPatternMatches(std::string_view pattern, std::string_view value)
{
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    std::size_t patternIndex = 0;
    std::size_t valueIndex = 0;
    std::size_t starIndex = std::string_view::npos;
    std::size_t matchIndex = 0;
    while (valueIndex < value.size()) {
        if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
            starIndex = patternIndex;
            matchIndex = valueIndex;
            ++patternIndex;
            continue;
        }
        if (patternIndex < pattern.size() && lower(pattern[patternIndex]) == lower(value[valueIndex])) {
            ++patternIndex;
            ++valueIndex;
            continue;
        }
        if (starIndex == std::string_view::npos)
            return false;
        patternIndex = starIndex + 1;
        ++matchIndex;
        valueIndex = matchIndex;
    }
    while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
        ++patternIndex;
    return patternIndex == pattern.size();
}

/// Whether any of the patterns a rule spells for one key matches a name.
///
/// - Parameters:
///   - spelling: The key's patterns, as ``tagPatternList`` splits them.
///   - value: The host or profile name to test.
/// - Returns: True when one of the patterns matches the whole value.
[[nodiscard]] inline bool tagPatternListMatches(std::string_view spelling, std::string_view value)
{
    for (const std::string_view pattern : tagPatternList(spelling)) {
        if (tagPatternMatches(pattern, value))
            return true;
    }
    return false;
}

/// Gives a tag what a `[[tags]]` entry says about how it is drawn: its
/// prominence, colour, icon, whether it shows its name, and where.
///
/// One door for a tag a rule matched and for a name a rule declares, so a
/// hand-set tag of a rule's name is drawn as that rule draws it.
///
/// - Parameters:
///   - tag: The tag being resolved; its name, source and pattern are left as
///     they are.
///   - rule: The entry that supplies the look.
///   - ruleIndex: The entry's position in file order.
inline void applyTagRuleLook(ResolvedTag& tag, const Preferences::TagEntry& rule, std::size_t ruleIndex)
{
    tag.prominence = tagProminenceFromString(rule.prominence);
    (void)tagColorFromValue(rule.color, tag.color);
    tag.ruleIndex = static_cast<int>(ruleIndex);
    tag.colorName = tagPaletteName(rule.color);
    tag.icon = rule.icon;
    tag.iconWeight = tagIconWeightFromString(rule.iconWeight);
    tag.showsName = rule.showsName;
    tag.showsInTitleBar = rule.showsInTitleBar;
    tag.showsInTabs = rule.showsInTabs;
    tag.showsInPaneHeaders = rule.showsInPaneHeaders;
}

/// A resolved tag list under a name a frontend language can spell.
///
/// Swift/C++ interop cannot name a bare `std::vector<T>`, so the
/// instantiation carries an alias and the vector imports as a collection.
using ResolvedTags = std::vector<ResolvedTag>;

/// The tags a window wears by rule, in the order the config declared them.
///
/// An entry constrains on the host, the profile, or both; both present means
/// both must match. An entry with neither constraint matches nothing here — it
/// declares a tag's colour for the other sources to use by name, rather than
/// silently marking every window.
///
/// A host is matched under every name it answers to: a machine saved as
/// "Prod" on `prod.example.com` is the same machine to a rule written either
/// way.
///
/// - Parameters:
///   - rules: The `[[tags]]` entries, in file order.
///   - hostNames: The names the window's host answers to, none for a local
///     window.
///   - profileName: The active profile's name, empty when none is applied.
/// - Returns: The matching tags, deduplicated by name, first declaration wins.
[[nodiscard]] inline ResolvedTags resolveTags(
    const std::vector<Preferences::TagEntry>& rules, std::span<const std::string_view> hostNames, std::string_view profileName)
{
    const auto matchesHost = [&](std::string_view spelling) {
        if (hostNames.empty())
            return tagPatternListMatches(spelling, {});
        for (const std::string_view name : hostNames) {
            if (tagPatternListMatches(spelling, name))
                return true;
        }
        return false;
    };
    ResolvedTags out;
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const Preferences::TagEntry& rule = rules[index];
        if (!isValidTagRule(rule))
            continue;
        const bool hasHostPattern = !tagPatternList(rule.matchHost).empty();
        const bool hasProfilePattern = !tagPatternList(rule.matchProfile).empty();
        if (!hasHostPattern && !hasProfilePattern)
            continue;
        if (hasHostPattern && !matchesHost(rule.matchHost))
            continue;
        if (hasProfilePattern && !tagPatternListMatches(rule.matchProfile, profileName))
            continue;
        ResolvedTag resolved;
        if (isTagNameTitled(rule.name))
            resolved.name = rule.name;
        applyTagRuleLook(resolved, rule, index);
        resolved.source = TagSource::Rule;
        resolved.rulePattern = tagRulePattern(rule);
        bool isAlreadyDrawn = false;
        for (const ResolvedTag& seen : out) {
            if (isSameTag(seen, resolved)) {
                isAlreadyDrawn = true;
                break;
            }
        }
        if (!isAlreadyDrawn)
            out.push_back(std::move(resolved));
    }
    return out;
}

/// The tags a window wears by rule, for a host known by one name.
///
/// - Parameters:
///   - rules: The `[[tags]]` entries, in file order.
///   - host: The host's name, empty for a local window.
///   - profileName: The active profile's name, empty when none is applied.
/// - Returns: The matching tags, deduplicated by name, first declaration wins.
[[nodiscard]] inline ResolvedTags resolveTags(
    const std::vector<Preferences::TagEntry>& rules, std::string_view host, std::string_view profileName)
{
    if (host.empty())
        return resolveTags(rules, std::span<const std::string_view> {}, profileName);
    const std::string_view names[] = { host };
    return resolveTags(rules, std::span<const std::string_view> { names }, profileName);
}

/// The tags a pane's host earns by rule: Core's ``resolvePaneHost`` answer,
/// matched under its machine and its label.
///
/// The one door every frontend's tag rules go through, so the host a window's
/// tags matched is the host its chip and its tab name.
///
/// - Parameters:
///   - rules: The `[[tags]]` entries, in file order.
///   - host: Where the pane's session lives.
///   - profileName: The active profile's name, empty when none is applied.
/// - Returns: The matching tags, deduplicated by name, first declaration wins.
[[nodiscard]] inline ResolvedTags resolveTagsForPaneHost(
    const std::vector<Preferences::TagEntry>& rules, const PaneHost& host, const std::string& profileName)
{
    if (!host.isNamed())
        return resolveTags(rules, std::span<const std::string_view> {}, profileName);
    std::string_view names[2];
    std::size_t count = 0;
    if (!host.machine.empty())
        names[count++] = host.machine;
    if (!host.label.empty() && !tagNamesEqual(host.label, host.machine))
        names[count++] = host.label;
    return resolveTags(rules, std::span<const std::string_view> { names, count }, profileName);
}

/// Resolves a tag named outside the rules — set by hand on a window, or
/// announced by a session.
///
/// The name's colour, prominence and icon, and whether and where its name is
/// drawn, come first from a `[[tags]]` entry of that name, which is the
/// documented purpose of an entry with no match constraint; failing that the name itself may be a colour, one of the seven
/// system colours or a `#rrggbb` literal, which is how a picker's custom colour
/// reaches a window; failing both, the tag names something without claiming a
/// colour.
///
/// - Parameters:
///   - rules: The `[[tags]]` entries, consulted by name without regard to case.
///   - name: The tag's name as the user or the session spelled it.
/// - Returns: The tag, ready to draw.
[[nodiscard]] inline ResolvedTag resolveManualTag(const std::vector<Preferences::TagEntry>& rules, std::string_view name)
{
    ResolvedTag resolved;
    resolved.name = std::string { name };
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const Preferences::TagEntry& rule = rules[index];
        if (!isTagNameTitled(rule.name) || !tagNamesEqual(rule.name, name))
            continue;
        applyTagRuleLook(resolved, rule, index);
        return resolved;
    }
    if (tagColorFromValue(std::string { name }, resolved.color))
        resolved.colorName = tagPaletteName(name);
    return resolved;
}

/// Appends tags to a resolved list, keeping the first declaration of a name.
///
/// The union order is the trust order: a rule the config declared outranks a
/// hand-set tag of the same name, which outranks what a session announced.
///
/// - Parameters:
///   - tags: The list under construction.
///   - additions: The tags to append, skipped when ``isSameTag`` finds them
///     present.
inline void appendTags(ResolvedTags& tags, const ResolvedTags& additions)
{
    for (const ResolvedTag& addition : additions) {
        bool isAlreadyDrawn = false;
        for (const ResolvedTag& seen : tags) {
            if (isSameTag(seen, addition)) {
                isAlreadyDrawn = true;
                break;
            }
        }
        if (!isAlreadyDrawn)
            tags.push_back(addition);
    }
}

/// Tag names under a name a frontend language can spell, for the same reason
/// as ``ResolvedTags``.
using TagNames = std::vector<std::string>;

/// The tags a window wears: what its title bar, its tab and its title editor
/// all draw.
///
/// The rules that match the pane's host and the window's profile come first,
/// then the names set by hand, each resolved by ``resolveManualTag``. A blank
/// hand-set name is skipped: only a rule can supply an untitled tag.
///
/// - Parameters:
///   - rules: The `[[tags]]` entries, in file order.
///   - host: Where the pane the window's title speaks for lives.
///   - profileName: The active profile's name, empty when none is applied.
///   - manualNames: The names set on the window by hand, in the order set.
/// - Returns: The window's tags in trust order, deduplicated by ``isSameTag``.
[[nodiscard]] inline ResolvedTags resolveWindowTags(
    const std::vector<Preferences::TagEntry>& rules, const PaneHost& host, const std::string& profileName, const TagNames& manualNames)
{
    ResolvedTags tags = resolveTagsForPaneHost(rules, host, profileName);
    ResolvedTags manual;
    manual.reserve(manualNames.size());
    for (const std::string& name : manualNames) {
        if (isTagNameTitled(name))
            manual.push_back(resolveManualTag(rules, name));
    }
    appendTags(tags, manual);
    return tags;
}

/// The tags one of a window's title surfaces draws, in the order given.
///
/// The one answer the title bar, the tab and a pane's header each take their
/// markers from, so a rule placed off a surface is missing from it alone and
/// the order the rest keep is the window's.
///
/// - Parameters:
///   - tags: A window's tags, from ``resolveWindowTags``.
///   - surface: The surface asking.
/// - Returns: The tags whose rules place them there, in their order.
[[nodiscard]] inline ResolvedTags tagsDrawnOn(const ResolvedTags& tags, TitleSurface surface)
{
    ResolvedTags drawn;
    drawn.reserve(tags.size());
    for (const ResolvedTag& tag : tags) {
        if (tag.isDrawnOn(surface))
            drawn.push_back(tag);
    }
    return drawn;
}

/// The host a tab's or a pane header's tags are matched against: the pane's,
/// unless the pane is a host session's and has gone on from there.
///
/// A host session's pane is on the machine that was dialled, which is what its
/// chip names (``resolvePaneHost``). A shell in it can still run `ssh` on
/// that machine; the pane's own location, a shell's report or the walk, then
/// names where it went, and that machine's rules are the pane's. A Sync
/// link's pane facts name the host's own machine and carry no such report, so
/// over one the location this side holds says nothing further, and the answer
/// is the pane's host.
///
/// - Parameters:
///   - host: Where the pane lives, as ``resolvePaneHost`` answers for it.
///   - paneLocation: The sources of the pane's own location.
/// - Returns: The host the pane's rules match.
[[nodiscard]] inline PaneHost surfaceTagHost(const PaneHost& host, const LocationInputs& paneLocation)
{
    if (!host.isHostSession)
        return host;
    const SessionLocation onward = resolveSessionLocation(paneLocation);
    if (!onward.isRemote() || onward.name.empty() || tagNamesEqual(onward.name, host.machine) || tagNamesEqual(onward.name, host.label))
        return host;
    PaneHost onwardHost;
    onwardHost.place = HostPlace::Remote;
    onwardHost.machine = onward.name;
    onwardHost.label = onward.name;
    onwardHost.carrier = onward.via;
    return onwardHost;
}

/// Whether the tabs a tab stands among are on one host or on several, as
/// ``PaneHostMix`` answers it for their title panes.
enum class TabGroupHosts : std::uint8_t {
    /// Every tab of the group is on the same host.
    Shared,
    /// The group's tabs are on more than one host.
    Mixed,
};

/// What a tab or a pane's header shows of its tags.
struct SurfaceTags {
    /// The tags its marks draw, in order.
    ResolvedTags drawn;
    /// Every tag it is placed to show, the ones its marks leave to the title
    /// bar's host chip included, for its tooltip and its spoken label.
    ResolvedTags named;
};

/// The tags a tab or a pane's header draws, and the tags it names.
///
/// Each draws what tells it apart. In a window attached to a host over Sync
/// whose tabs are all on that host, a tag the window's own connection earns,
/// by its host or by the window's profile, says nothing one tab or one pane
/// does not say for all of them: the title bar's host chip carries it, and the
/// surface leaves it out. A tag the surface gets from elsewhere is still drawn:
/// from another machine its pane went on to (``surfaceTagHost``), or set on
/// the window by hand. Tabs on several hosts, and a local window's tabs and
/// panes, draw every tag they wear, since their tags are how they are told
/// apart. A rule's placement on the surface applies first, to both lists.
///
/// - Parameters:
///   - rules: The `[[tags]]` entries, in file order.
///   - host: Where the surface's pane lives, as ``resolvePaneHost`` answers.
///   - paneLocation: The sources of that pane's own location.
///   - profileName: The active profile's name, empty when none is applied.
///   - manualNames: The names set on the window by hand, in the order set.
///   - surface: The tab, or a pane's header.
///   - groupHosts: Whether the window's tab group is on one host or several.
/// - Returns: The tags drawn, and the tags named.
[[nodiscard]] inline SurfaceTags resolveSurfaceTags(const std::vector<Preferences::TagEntry>& rules, const PaneHost& host,
    const LocationInputs& paneLocation, const std::string& profileName, const TagNames& manualNames, TitleSurface surface,
    TabGroupHosts groupHosts)
{
    SurfaceTags tags;
    tags.named = tagsDrawnOn(resolveWindowTags(rules, surfaceTagHost(host, paneLocation), profileName, manualNames), surface);
    if (!host.isHostSession || groupHosts == TabGroupHosts::Mixed) {
        tags.drawn = tags.named;
        return tags;
    }
    const ResolvedTags connection = resolveTagsForPaneHost(rules, host, profileName);
    tags.drawn.reserve(tags.named.size());
    for (const ResolvedTag& tag : tags.named) {
        const bool isTheConnections = tag.source == TagSource::Rule // bool-ok: a local verdict
            && std::ranges::any_of(connection, [&](const ResolvedTag& carried) { return isSameTag(carried, tag); });
        if (!isTheConnections)
            tags.drawn.push_back(tag);
    }
    return tags;
}

/// The tags a window's tab draws, and the tags it names, by
/// ``resolveSurfaceTags``.
///
/// - Parameters:
///   - rules: The `[[tags]]` entries, in file order.
///   - windowHost: Where the pane the window's title speaks for lives.
///   - paneLocation: The sources of that pane's own location.
///   - profileName: The active profile's name, empty when none is applied.
///   - manualNames: The names set on the window by hand, in the order set.
///   - groupHosts: Whether the tab's group is on one host or several.
/// - Returns: The tags drawn, and the tags named.
[[nodiscard]] inline SurfaceTags resolveTabTags(const std::vector<Preferences::TagEntry>& rules, const PaneHost& windowHost,
    const LocationInputs& paneLocation, const std::string& profileName, const TagNames& manualNames, TabGroupHosts groupHosts)
{
    return resolveSurfaceTags(rules, windowHost, paneLocation, profileName, manualNames, TitleSurface::Tab, groupHosts);
}

/// The tags a pane's header draws, and the tags it names, by
/// ``resolveSurfaceTags``.
///
/// The panes of one window share its connection whatever its tab group holds,
/// so a pane of a Sync window leaves the connection's tags to the chip unless
/// it has gone on to another machine.
///
/// - Parameters:
///   - rules: The `[[tags]]` entries, in file order.
///   - paneHost: Where the pane lives, as ``resolvePaneHost`` answers.
///   - paneLocation: The sources of the pane's own location.
///   - profileName: The active profile's name, empty when none is applied.
///   - manualNames: The names set on the window by hand, in the order set.
/// - Returns: The tags drawn, and the tags named.
[[nodiscard]] inline SurfaceTags resolvePaneHeaderTags(const std::vector<Preferences::TagEntry>& rules, const PaneHost& paneHost,
    const LocationInputs& paneLocation, const std::string& profileName, const TagNames& manualNames)
{
    return resolveSurfaceTags(rules, paneHost, paneLocation, profileName, manualNames, TitleSurface::Pane, TabGroupHosts::Shared);
}

/// The first rule-supplied tag in a window's list, by the rule's index, for an
/// editor that opens the rule a window matched.
///
/// - Parameter tags: A window's tags, from ``resolveWindowTags``.
/// - Returns: The supplying entry's index in file order, or -1 when no rule
///   supplied a tag.
[[nodiscard]] inline int firstMatchedTagRule(const ResolvedTags& tags)
{
    for (const ResolvedTag& tag : tags) {
        if (tag.source == TagSource::Rule)
            return tag.ruleIndex;
    }
    return -1;
}

/// One `[[tags]]` block as config text, for writing an entry back to the file.
///
/// Only the fields the entry carries are emitted, so a rule that constrains
/// only the host does not grow an empty `match_profile` line that reads as a
/// constraint, an untitled rule has no `name` line, an icon in the default
/// fill weight has no `icon_weight` line, and a `show_*` key is written only
/// when it turns its default off. The result has no leading blank line; the
/// splicer adds the spacing it wants.
///
/// - Parameter tag: The entry to write.
/// - Returns: The block text, ending in a newline.
[[nodiscard]] inline std::string serializeTag(const Preferences::TagEntry& tag)
{
    const auto quoted = [](const std::string& value) {
        std::string out = "\"";
        for (const char c : value) {
            if (c == '\\' || c == '"')
                out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    };
    std::string out = "[[tags]]\n";
    // An untitled rule is written without a name line: a `name = ""` reads as
    // a name someone forgot, where its absence reads as the dot it draws.
    if (isTagNameTitled(tag.name))
        out += "name = " + quoted(tag.name) + "\n";
    if (!tag.color.empty())
        out += "color = " + quoted(tag.color) + "\n";
    // One pattern is written as a string and several as an array, which
    // reads back as the same patterns.
    const auto patternLine = [&](std::string_view key, std::string_view spelling) {
        const std::vector<std::string_view> patterns = tagPatternList(spelling);
        if (patterns.empty())
            return;
        out += key;
        out += " = ";
        if (patterns.size() == 1) {
            out += quoted(std::string(patterns.front()));
        } else {
            out += "[";
            for (std::size_t index = 0; index < patterns.size(); ++index) {
                if (index > 0)
                    out += ", ";
                out += quoted(std::string(patterns[index]));
            }
            out += "]";
        }
        out += "\n";
    };
    patternLine("match_host", tag.matchHost);
    patternLine("match_profile", tag.matchProfile);
    if (tagProminenceFromString(tag.prominence) == TagProminence::Loud)
        out += "prominence = \"loud\"\n";
    if (!tag.icon.empty()) {
        out += "icon = " + quoted(tag.icon) + "\n";
        if (tagIconWeightFromString(tag.iconWeight) == TagIconWeight::Regular)
            out += "icon_weight = \"regular\"\n";
    }
    if (!tag.showsName)
        out += "show_name = false\n";
    if (!tag.showsInTitleBar)
        out += "show_in_title_bar = false\n";
    if (!tag.showsInTabs)
        out += "show_in_tabs = false\n";
    if (!tag.showsInPaneHeaders)
        out += "show_in_pane_headers = false\n";
    return out;
}

/// Writes one `[[tags]]` entry into config text, by its position.
///
/// A rule is addressed by where it stands among the `[[tags]]` blocks rather
/// than by its name: an untitled rule has none, and two may share a colour.
/// The position is the one ``Preferences::Values::tags`` holds it at, since
/// the parser appends an entry for each block in the order the text gives.
///
/// - Parameters:
///   - document: The whole config text.
///   - index: The entry's position, or -1 (or any position past the last) to
///     append a new block.
///   - tag: The entry to write.
/// - Returns: The text with the block replaced or appended.
[[nodiscard]] inline std::string writeTagRule(std::string document, int index, const Preferences::TagEntry& tag)
{
    const std::string block = "\n" + serializeTag(tag);
    return Preferences::spliceIndexedBlock(std::move(document), "tags", index, block);
}

/// Removes one `[[tags]]` entry from config text, by its position.
///
/// - Parameters:
///   - document: The whole config text.
///   - index: The entry's position, as ``writeTagRule`` addresses it.
/// - Returns: The text without that block, unchanged when there is none.
[[nodiscard]] inline std::string removeTagRule(std::string document, int index)
{
    return Preferences::removeIndexedBlock(std::move(document), "tags", index);
}

/// Moves one `[[tags]]` entry to another position among the entries, in
/// config text.
///
/// Rules draw in file order, so this is how one comes to lead the others.
/// Each block moves as written, with every key it carries, the comments inside
/// it and the comments directly above its header, while the file's other
/// sections and the spacing between blocks stay where they were
/// (``Preferences::moveIndexedBlock``).
///
/// - Parameters:
///   - document: The whole config text.
///   - from: The entry's position, as ``writeTagRule`` addresses it.
///   - to: The position it takes once moved.
/// - Returns: The text with the entries in their new order, unchanged when
///   either position names no entry.
[[nodiscard]] inline std::string moveTagRule(std::string document, int from, int to)
{
    return Preferences::moveIndexedBlock(std::move(document), "tags", from, to);
}

} // namespace Ubiquitty::Session
