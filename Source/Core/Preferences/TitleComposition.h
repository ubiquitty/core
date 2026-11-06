// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// TitleComposition.h — the table behind the 3×11 title toggles.
//
// Window, tab, and pane each expose the same eleven parts (ten booleans plus the
// static base text). The Values struct carries them as 30 optionals, Effective::
// window/tab/paneTitleComposition resolves them (pane inherits tab), and the
// macOS Settings UI renders them as three cards. Without a table the three preferences-vocabulary-ok: the macOS Settings window, a platform's own
// layers spell the same 30 names separately: the Objective-C setters each name
// their Key and their Values field, the effective resolvers each name their
// defaults, and the Swift catalog each names its row title. A second spelling
// is where a paste error hides — a setter wired to the wrong Key still
// compiles, still persists, and the UI still toggles, just not the key the
// title weave reads.
//
// This header is the single source of truth: one row per boolean toggle and
// one per text field, each naming the surface, the logical part, the
// persistence Key, the Values optional it writes, the
// Effective::TitleComposition member it fills, and the value an unset
// optional resolves to. The resolvers in PreferencesAccessors.cpp, the
// effective reads, the setters, the Objective-C bridge, and the tests all walk
// these rows, so a key renamed without its row fails at the call site rather
// than silently diverging.

#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/PreferencesDefaults.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty::Preferences::TitleComposition {

/// Identifies which title surface a part belongs to.
///
/// Window, tab, and pane each have their own set of toggles, and the table
/// carries the surface so a card builder can slice rows by surface without
/// naming the prefix separately.
enum class Surface : std::uint8_t {
    Window,
    Tab,
    Pane,
};

/// Identifies one boolean title part within a surface.
///
/// The nine booleans are the toggles the Settings UI shows; the tenth part preferences-vocabulary-ok: the macOS Settings window, a platform's own
/// (base text) travels separately as a string.
enum class Part : std::uint8_t {
    Directory,
    DirectoryPath,
    AppTitle,
    Process,
    Dimensions,
    ProcessArguments,
    ShellCommand,
    TTYName,
    ProfileName,
    ProgramStatus,
};

/// Describes one boolean title toggle.
///
/// One row names the surface, the logical part, the persistence Key, the
/// Values field it writes, the Effective::TitleComposition member it fills,
/// and what an unset field resolves to, so the spellings of the same toggle
/// are one row.
struct BoolDescriptor {
    Surface surface;
    Part part;
    Key key;
    std::optional<bool> Values::* valuesField;
    bool Effective::TitleComposition::* compositionField;
    /// The effective value while the Values field is unset: a literal, or
    /// `std::nullopt` for a pane row that inherits the tab's resolved value.
    std::optional<bool> fallback;
};

/// Describes one base-text field.
///
/// One row per surface, naming the persistence Key and the Values field that
/// carries the static leading title.
struct TextDescriptor {
    Surface surface;
    Key key;
    std::optional<std::string> Values::* valuesField;
};

// One row per boolean toggle — 10 parts × 3 surfaces = 30 rows. Ordered by
// surface (Window, Tab, Pane) and within each surface by part declaration
// order so a surface's rows are contiguous for a card builder that slices by
// surface. The fallback column carries the historical defaults: a pane row
// inherits the tab's resolved value unless it says otherwise.
inline constexpr BoolDescriptor boolDescriptors[] = {
    { Surface::Window, Part::Directory, Key::TitleWindowDirectory, &Values::titleWindowDirectory, &Effective::TitleComposition::showsDirectory, true },
    { Surface::Window, Part::DirectoryPath, Key::TitleWindowDirectoryPath, &Values::titleWindowDirectoryPath, &Effective::TitleComposition::showsDirectoryFullPath, false }, // the window showed the folder name, not the path
    { Surface::Window, Part::AppTitle, Key::TitleWindowAppTitle, &Values::titleWindowAppTitle, &Effective::TitleComposition::showsAppTitle, true },
    { Surface::Window, Part::Process, Key::TitleWindowProcess, &Values::titleWindowProcess, &Effective::TitleComposition::showsProcess, true },
    { Surface::Window, Part::Dimensions, Key::TitleWindowDimensions, &Values::titleWindowDimensions, &Effective::TitleComposition::showsDimensions, true }, // the window title showed dimensions
    { Surface::Window, Part::ProcessArguments, Key::TitleWindowProcessArguments, &Values::titleWindowProcessArguments, &Effective::TitleComposition::showsProcessArguments, false }, // argv is opt-in
    { Surface::Window, Part::ShellCommand, Key::TitleWindowShellCommand, &Values::titleWindowShellCommand, &Effective::TitleComposition::showsShellCommand, false }, // opt-in
    { Surface::Window, Part::TTYName, Key::TitleWindowTTYName, &Values::titleWindowTTYName, &Effective::TitleComposition::showsTTYName, false }, // opt-in
    { Surface::Window, Part::ProfileName, Key::TitleWindowProfileName, &Values::titleWindowProfileName, &Effective::TitleComposition::showsProfileName, false }, // opt-in
    { Surface::Window, Part::ProgramStatus, Key::TitleWindowProgramStatus, &Values::titleWindowProgramStatus, &Effective::TitleComposition::showsProgramStatus, true }, // the window has room, and the selected tab shows no status dot

    { Surface::Tab, Part::Directory, Key::TitleTabDirectory, &Values::titleTabDirectory, &Effective::TitleComposition::showsDirectory, true },
    { Surface::Tab, Part::DirectoryPath, Key::TitleTabDirectoryPath, &Values::titleTabDirectoryPath, &Effective::TitleComposition::showsDirectoryFullPath, true },
    { Surface::Tab, Part::AppTitle, Key::TitleTabAppTitle, &Values::titleTabAppTitle, &Effective::TitleComposition::showsAppTitle, true },
    { Surface::Tab, Part::Process, Key::TitleTabProcess, &Values::titleTabProcess, &Effective::TitleComposition::showsProcess, true },
    { Surface::Tab, Part::Dimensions, Key::TitleTabDimensions, &Values::titleTabDimensions, &Effective::TitleComposition::showsDimensions, false }, // the tab strip never showed dimensions
    { Surface::Tab, Part::ProcessArguments, Key::TitleTabProcessArguments, &Values::titleTabProcessArguments, &Effective::TitleComposition::showsProcessArguments, false }, // argv is opt-in
    { Surface::Tab, Part::ShellCommand, Key::TitleTabShellCommand, &Values::titleTabShellCommand, &Effective::TitleComposition::showsShellCommand, false }, // opt-in
    { Surface::Tab, Part::TTYName, Key::TitleTabTTYName, &Values::titleTabTTYName, &Effective::TitleComposition::showsTTYName, false }, // opt-in
    { Surface::Tab, Part::ProfileName, Key::TitleTabProfileName, &Values::titleTabProfileName, &Effective::TitleComposition::showsProfileName, false }, // opt-in
    { Surface::Tab, Part::ProgramStatus, Key::TitleTabProgramStatus, &Values::titleTabProgramStatus, &Effective::TitleComposition::showsProgramStatus, false }, // opt-in: a label that changes as the program works would re-lay out the strip

    { Surface::Pane, Part::Directory, Key::TitlePaneDirectory, &Values::titlePaneDirectory, &Effective::TitleComposition::showsDirectory, std::nullopt },
    { Surface::Pane, Part::DirectoryPath, Key::TitlePaneDirectoryPath, &Values::titlePaneDirectoryPath, &Effective::TitleComposition::showsDirectoryFullPath, std::nullopt },
    { Surface::Pane, Part::AppTitle, Key::TitlePaneAppTitle, &Values::titlePaneAppTitle, &Effective::TitleComposition::showsAppTitle, std::nullopt },
    { Surface::Pane, Part::Process, Key::TitlePaneProcess, &Values::titlePaneProcess, &Effective::TitleComposition::showsProcess, std::nullopt },
    { Surface::Pane, Part::Dimensions, Key::TitlePaneDimensions, &Values::titlePaneDimensions, &Effective::TitleComposition::showsDimensions, false }, // never inherited: a pane label never showed dimensions
    { Surface::Pane, Part::ProcessArguments, Key::TitlePaneProcessArguments, &Values::titlePaneProcessArguments, &Effective::TitleComposition::showsProcessArguments, std::nullopt },
    { Surface::Pane, Part::ShellCommand, Key::TitlePaneShellCommand, &Values::titlePaneShellCommand, &Effective::TitleComposition::showsShellCommand, std::nullopt },
    { Surface::Pane, Part::TTYName, Key::TitlePaneTTYName, &Values::titlePaneTTYName, &Effective::TitleComposition::showsTTYName, std::nullopt },
    { Surface::Pane, Part::ProfileName, Key::TitlePaneProfileName, &Values::titlePaneProfileName, &Effective::TitleComposition::showsProfileName, std::nullopt },
    { Surface::Pane, Part::ProgramStatus, Key::TitlePaneProgramStatus, &Values::titlePaneProgramStatus, &Effective::TitleComposition::showsProgramStatus, std::nullopt },
};

inline constexpr int boolDescriptorCount = static_cast<int>(sizeof(boolDescriptors) / sizeof(boolDescriptors[0]));

// One row per static base text — 3 surfaces.
inline constexpr TextDescriptor textDescriptors[] = {
    { Surface::Window, Key::TitleWindowText, &Values::titleWindowText },
    { Surface::Tab, Key::TitleTabText, &Values::titleTabText },
    { Surface::Pane, Key::TitlePaneText, &Values::titlePaneText },
};

inline constexpr int textDescriptorCount = static_cast<int>(sizeof(textDescriptors) / sizeof(textDescriptors[0]));

/// Returns the resolved composition for a surface.
///
/// Pane inherits tab via `Effective::paneTitleComposition`, so the same value
/// the title weave captures is returned here.
///
/// - Parameters:
///   - values: The configuration to resolve.
///   - surface: Which surface to resolve.
/// - Returns: The resolved composition.
inline Effective::TitleComposition effectiveComposition(const Values& values, Surface surface)
{
    switch (surface) {
    case Surface::Window:
        return Effective::windowTitleComposition(values);
    case Surface::Tab:
        return Effective::tabTitleComposition(values);
    case Surface::Pane:
        return Effective::paneTitleComposition(values);
    }
    return Effective::windowTitleComposition(values);
}

/// Locates the boolean row for a part on a surface.
///
/// - Parameters:
///   - surface: The surface to look up.
///   - part: The part to look up.
/// - Returns: Its row, or nullptr when no row carries that pair.
inline const BoolDescriptor* descriptorForPart(Surface surface, Part part)
{
    for (const BoolDescriptor& row : boolDescriptors) {
        if (row.surface == surface && row.part == part)
            return &row;
    }
    return nullptr;
}

/// Returns the effective boolean for one part on one surface.
///
/// The value the weave and the Settings toggle must agree on, read through preferences-vocabulary-ok: the macOS Settings window, a platform's own
/// the row's composition member so the table is the only Part-to-member map.
///
/// - Parameters:
///   - values: The configuration to resolve.
///   - surface: Which surface to read.
///   - part: Which part to read.
/// - Returns: Whether that part is shown; false for a pair no row carries.
inline bool effectiveBool(const Values& values, Surface surface, Part part)
{
    const BoolDescriptor* row = descriptorForPart(surface, part);
    if (!row)
        return false;
    return effectiveComposition(values, surface).*(row->compositionField);
}

/// Locates the boolean row for a persistence Key.
///
/// - Parameter key: The Key to look up.
/// - Returns: Its row, or nullptr when `key` is not a title toggle.
inline const BoolDescriptor* descriptorForBoolKey(Key key)
{
    for (int i = 0; i < boolDescriptorCount; ++i) {
        if (boolDescriptors[i].key == key)
            return &boolDescriptors[i];
    }
    return nullptr;
}

/// Locates the text row for a persistence Key.
///
/// - Parameter key: The Key to look up.
/// - Returns: Its row, or nullptr when `key` is not a title text Key.
inline const TextDescriptor* descriptorForTextKey(Key key)
{
    for (int i = 0; i < textDescriptorCount; ++i) {
        if (textDescriptors[i].key == key)
            return &textDescriptors[i];
    }
    return nullptr;
}

/// Returns the effective boolean addressed by a title Key.
///
/// The toggle the Settings checkbox shows when it persists that Key. preferences-vocabulary-ok: the macOS Settings window, a platform's own
///
/// - Parameters:
///   - values: The configuration to resolve.
///   - key: The title Key to read.
/// - Returns: Whether that toggle is on.
inline bool effectiveBoolForKey(const Values& values, Key key)
{
    const BoolDescriptor* descriptor = descriptorForBoolKey(key);
    if (!descriptor)
        return false;
    return effectiveBool(values, descriptor->surface, descriptor->part);
}

/// Writes the boolean addressed by a title Key.
///
/// The single mutator the setters share so a Key cannot be written to the
/// wrong field.
///
/// - Parameters:
///   - values: The configuration to mutate.
///   - key: The title Key to write.
///   - enabled: Whether the part is shown.
inline void setBool(Values& values, Key key, bool enabled)
{
    const BoolDescriptor* descriptor = descriptorForBoolKey(key);
    if (!descriptor)
        return;
    values.*(descriptor->valuesField) = enabled;
}

/// Returns the effective base text for a surface.
///
/// - Parameters:
///   - values: The configuration to resolve.
///   - surface: Which surface to read.
/// - Returns: The base title, borrowed from `values` or from the inherited tab.
inline std::string_view effectiveText(const Values& values, Surface surface)
{
    return effectiveComposition(values, surface).baseTitle;
}

/// Locates the text row for a surface.
///
/// - Parameter surface: The surface to look up.
/// - Returns: Its row, or nullptr for an unknown surface.
inline const TextDescriptor* descriptorForTextSurface(Surface surface)
{
    for (int i = 0; i < textDescriptorCount; ++i) {
        if (textDescriptors[i].surface == surface)
            return &textDescriptors[i];
    }
    return nullptr;
}

/// Writes the base text for a surface.
///
/// - Parameters:
///   - values: The configuration to mutate.
///   - surface: Which surface to write.
///   - text: The base title to store.
inline void setText(Values& values, Surface surface, std::string text)
{
    if (const TextDescriptor* row = descriptorForTextSurface(surface))
        values.*(row->valuesField) = std::move(text);
}

/// Resolves one surface's composition from the rows.
///
/// Every boolean comes from its row: the Values field when set, else the
/// row's fallback, else the inherited composition's member. The base text
/// follows the same rule through the text row.
///
/// - Parameters:
///   - values: The configuration to resolve.
///   - surface: Which surface to resolve.
///   - inherited: The composition a row without a literal fallback inherits;
///     nullptr when no row on `surface` inherits.
/// - Returns: The composition, its base text borrowed from `values` or from
///   `inherited`.
inline Effective::TitleComposition compose(const Values& values, Surface surface, const Effective::TitleComposition* inherited)
{
    Effective::TitleComposition composition {};
    for (const BoolDescriptor& row : boolDescriptors) {
        if (row.surface != surface)
            continue;
        composition.*(row.compositionField) = (values.*(row.valuesField)).value_or(row.fallback.value_or(inherited ? inherited->*(row.compositionField) : false));
    }
    if (const TextDescriptor* row = descriptorForTextSurface(surface)) {
        const std::optional<std::string>& text = values.*(row->valuesField);
        if (text)
            composition.baseTitle = std::string_view(*text);
        else if (inherited)
            composition.baseTitle = inherited->baseTitle;
    }
    // One rule for every surface: what holds the terminal says where it is.
    composition.directoryHiddenFor = values.directoryHiddenFor ? std::string_view(*values.directoryHiddenFor) : std::string_view(Defaults::directoryHiddenFor);
    composition.hidesDirectoryInFullScreen = values.directoryHiddenInFullScreen.value_or(Defaults::directoryHiddenInFullScreen);
    return composition;
}

} // namespace Ubiquitty::Preferences::TitleComposition
