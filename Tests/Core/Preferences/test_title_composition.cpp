// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_title_composition — the table behind the 3×10 title toggles.
//
// The title weave and the Settings toggles must agree: setting preferences-vocabulary-ok: the macOS Settings window, a platform's own
// [title] window_directory to false must make windowTitleComposition().showsDirectory
// false, and a pane toggle must inherit the tab when unset. Without a table the
// three layers spell the same 30 names separately, so a paste error — a setter
// writing the wrong Values field or a getter reading the wrong composition
// member — still compiles and still persists, just not the key the weave
// reads. This file pins the table that makes the three spellings one.

#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/TitleComposition.h"

#include <algorithm>
#include <iterator>

using namespace Ubiquitty::Preferences;
using namespace Ubiquitty::Preferences::TitleComposition;

namespace {

const char* surfaceName(Surface surface)
{
    switch (surface) {
    case Surface::Window:
        return "window";
    case Surface::Tab:
        return "tab";
    case Surface::Pane:
        return "pane";
    }
    return "unknown";
}

const char* partName(Part part)
{
    switch (part) {
    case Part::Directory:
        return "directory";
    case Part::DirectoryPath:
        return "directoryPath";
    case Part::AppTitle:
        return "appTitle";
    case Part::Process:
        return "process";
    case Part::Dimensions:
        return "dimensions";
    case Part::ProcessArguments:
        return "processArguments";
    case Part::ShellCommand:
        return "shellCommand";
    case Part::TTYName:
        return "ttyName";
    case Part::ProfileName:
        return "profileName";
    case Part::ProgramStatus:
        return "programStatus";
    }
    return "unknown";
}

} // namespace

/// The `[title]` key suffix a part is persisted under.
const char* keySuffix(Part part)
{
    switch (part) {
    case Part::Directory:
        return "directory";
    case Part::DirectoryPath:
        return "directory_path";
    case Part::AppTitle:
        return "app_title";
    case Part::Process:
        return "process";
    case Part::Dimensions:
        return "dimensions";
    case Part::ProcessArguments:
        return "process_arguments";
    case Part::ShellCommand:
        return "shell_command";
    case Part::TTYName:
        return "tty_name";
    case Part::ProfileName:
        return "profile_name";
    case Part::ProgramStatus:
        return "program_status";
    }
    return "";
}

/// The `[title]` key prefix a surface is persisted under.
const char* keyPrefix(Surface surface)
{
    switch (surface) {
    case Surface::Window:
        return "window_";
    case Surface::Tab:
        return "tab_";
    case Surface::Pane:
        return "pane_";
    }
    return "";
}

TEST(testTitleTableCoversEveryTitleKeyExactlyOnce)
{
    // The population is Key.h's own write table: every `[title]` key it
    // persists must have exactly one row here, a boolean in the boolean table
    // and a string in the text table, and the tables must hold nothing else.
    // A key dropped from Key.h but kept here, or added there and forgotten
    // here, fails on the count; a key in the wrong table fails on the kind.
    // Two `[title]` keys choose no surface's part: they say when the directory
    // part is left out, for every surface at once, so they have no row.
    constexpr Key notParts[] = { Key::TitleDirectoryHiddenFor, Key::TitleDirectoryHiddenInFullScreen };
    int titleBools = 0;
    int titleTexts = 0;
    for (int i = 0; i < writeDescriptorCount; ++i) {
        const WriteDescriptor& descriptor = writeDescriptors[i];
        if (std::string_view(descriptor.section) != "title")
            continue;
        if (std::ranges::find(notParts, descriptor.id) != std::end(notParts)) {
            CHECK_MSG(descriptorForBoolKey(descriptor.id) == nullptr, descriptor.key);
            CHECK_MSG(descriptorForTextKey(descriptor.id) == nullptr, descriptor.key);
            continue;
        }
        if (descriptor.kind == SerializeKind::Bool) {
            ++titleBools;
            CHECK_MSG(descriptorForBoolKey(descriptor.id) != nullptr, descriptor.key);
            CHECK_MSG(descriptorForTextKey(descriptor.id) == nullptr, descriptor.key);
        } else {
            ++titleTexts;
            CHECK_MSG(descriptorForTextKey(descriptor.id) != nullptr, descriptor.key);
            CHECK_MSG(descriptorForBoolKey(descriptor.id) == nullptr, descriptor.key);
        }
    }
    CHECK_EQ(boolDescriptorCount, titleBools);
    CHECK_EQ(textDescriptorCount, titleTexts);

    // No Key appears twice in a table: a duplicated Key would let a setter
    // write one field while the getter reads another.
    for (int i = 0; i < boolDescriptorCount; ++i) {
        for (int j = i + 1; j < boolDescriptorCount; ++j)
            CHECK_MSG(boolDescriptors[i].key != boolDescriptors[j].key, partName(boolDescriptors[i].part));
    }
    for (int i = 0; i < textDescriptorCount; ++i) {
        for (int j = i + 1; j < textDescriptorCount; ++j)
            CHECK(textDescriptors[i].key != textDescriptors[j].key);
    }
    // Every (surface, part) pair has one row, reachable by pair as well as by Key.
    for (int i = 0; i < boolDescriptorCount; ++i) {
        const BoolDescriptor& row = boolDescriptors[i];
        CHECK_MSG(descriptorForPart(row.surface, row.part) == &row, partName(row.part));
    }
    // Non-title Keys have no row.
    CHECK(descriptorForBoolKey(Key::FontSize) == nullptr);
    CHECK(descriptorForTextKey(Key::FontSize) == nullptr);
}

TEST(testTitleTableKeysMatchWriteDescriptorSectionAndName)
{
    // A row's Key must persist under `[title]` as `<surface>_<part>`: a row
    // whose Key is one off (window vs tab, process vs process_arguments) still
    // compiles but round-trips the wrong line in config.toml. The expected name
    // is derived from the row itself, so there is no second list to drift.
    for (int i = 0; i < boolDescriptorCount; ++i) {
        const BoolDescriptor& row = boolDescriptors[i];
        const WriteDescriptor* descriptor = descriptorFor(row.key);
        CHECK_MSG(descriptor != nullptr, partName(row.part));
        if (!descriptor)
            continue;
        const std::string expected = std::string(keyPrefix(row.surface)) + keySuffix(row.part);
        CHECK_MSG(std::string(descriptor->section) == "title", expected.c_str());
        CHECK_MSG(std::string(descriptor->key) == expected, expected.c_str());
    }
    for (int i = 0; i < textDescriptorCount; ++i) {
        const TextDescriptor& row = textDescriptors[i];
        const WriteDescriptor* descriptor = descriptorFor(row.key);
        CHECK_MSG(descriptor != nullptr, surfaceName(row.surface));
        if (!descriptor)
            continue;
        const std::string expected = std::string(keyPrefix(row.surface)) + "text";
        CHECK_MSG(std::string(descriptor->section) == "title", expected.c_str());
        CHECK_MSG(std::string(descriptor->key) == expected, expected.c_str());
    }
}

TEST(testSettingViaTableAffectsOnlyThatPartOnThatSurface)
{
    // For each row: setting that one Key to the opposite of its effective
    // default must flip exactly that part on that surface in the effective
    // composition, leaving every other part on that surface and the same part
    // on other surfaces unchanged. A row whose valuesField and compositionField
    // disagree, or whose surface is wrong, flips the wrong cell here.
    for (int i = 0; i < boolDescriptorCount; ++i) {
        const BoolDescriptor& row = boolDescriptors[i];
        const Values untouched;
        const bool beforeEffective = effectiveBool(untouched, row.surface, row.part); // bool-ok: the payload being asserted
        const bool afterValue = !beforeEffective; // bool-ok: the payload being asserted

        Values changed;
        setBool(changed, row.key, afterValue);
        // The Values field itself was written — not a neighbouring one.
        CHECK_MSG((changed.*(row.valuesField)).has_value() && *(changed.*(row.valuesField)) == afterValue,
            surfaceName(row.surface));

        // The effective composition reflects the write, on that surface and part.
        const bool afterEffective = effectiveBool(changed, row.surface, row.part); // bool-ok: the payload being asserted
        CHECK_MSG(afterEffective == afterValue, partName(row.part));
        const bool viaKey = effectiveBoolForKey(changed, row.key); // bool-ok: the payload being asserted
        CHECK_MSG(viaKey == afterValue, partName(row.part));

        // No other part on the same surface flipped.
        for (int j = 0; j < boolDescriptorCount; ++j) {
            const BoolDescriptor& other = boolDescriptors[j];
            if (other.surface != row.surface)
                continue;
            if (other.part == row.part)
                continue;
            const bool otherBefore = effectiveBool(untouched, other.surface, other.part); // bool-ok: test state
            const bool otherAfter = effectiveBool(changed, other.surface, other.part); // bool-ok: test state
            CHECK_MSG(otherAfter == otherBefore, partName(other.part));
        }
        // The same part on a different surface kept its default, except pane
        // inheriting tab: setting tab must move pane when pane is still unset,
        // but setting pane must not move tab, and setting window must not move
        // either tab or pane.
        if (row.surface == Surface::Window) {
            for (int j = 0; j < boolDescriptorCount; ++j) {
                const BoolDescriptor& other = boolDescriptors[j];
                if (other.part != row.part)
                    continue;
                if (other.surface == Surface::Window)
                    continue;
                const bool otherAfter = effectiveBool(changed, other.surface, other.part); // bool-ok: test state
                const bool otherBefore = effectiveBool(untouched, other.surface, other.part); // bool-ok: test state
                // Window is isolated; tab and pane must not have moved.
                CHECK_MSG(otherAfter == otherBefore, partName(other.part));
            }
        } else if (row.surface == Surface::Tab) {
            // Tab change moves pane only while pane is still inheriting that part.
            // We changed tab to the opposite of its default, so pane's effective
            // should now match the new tab value if pane's field is still unset.
            // To keep this assertion independent of the current default, check
            // that pane's effective moved exactly when its own field is unset
            // (which it is in `changed`, since we only set the tab field).
            const bool tabAfter = effectiveBool(changed, Surface::Tab, row.part); // bool-ok: test state
            const bool paneAfter = effectiveBool(changed, Surface::Pane, row.part); // bool-ok: test state
            if (row.part == Part::Dimensions) {
                // Pane dimensions never inherit — it stays false regardless of tab.
                CHECK_MSG(paneAfter == effectiveBool(untouched, Surface::Pane, row.part), "pane dimensions never inherit");
            } else {
                CHECK_MSG(paneAfter == tabAfter, "pane follows tab while unset");
            }
        } else if (row.surface == Surface::Pane) {
            // Pane change must not move tab.
            const bool tabAfter = effectiveBool(changed, Surface::Tab, row.part); // bool-ok: test state
            const bool tabBefore = effectiveBool(untouched, Surface::Tab, row.part); // bool-ok: test state
            CHECK_MSG(tabAfter == tabBefore, partName(row.part));
        }
    }
}

TEST(testPaneInheritanceAndDecouplingThroughTable)
{
    // Pane inherits tab while unset, decouples once set, and dimensions never
    // inherit — all through the same effectiveComposition the weave reads.
    const Values untouched;
    // Window and tab differ where history left them differing: window shows the
    // folder name, tab the full path; window shows dimensions, tab does not.
    CHECK(!effectiveBool(untouched, Surface::Window, Part::DirectoryPath));
    CHECK(effectiveBool(untouched, Surface::Tab, Part::DirectoryPath));
    CHECK(effectiveBool(untouched, Surface::Window, Part::Dimensions));
    CHECK(!effectiveBool(untouched, Surface::Tab, Part::Dimensions));
    CHECK(!effectiveBool(untouched, Surface::Pane, Part::Dimensions));

    // Pane follows tab while unset — on every boolean except dimensions.
    for (int i = 0; i < boolDescriptorCount; ++i) {
        const BoolDescriptor& row = boolDescriptors[i];
        if (row.surface != Surface::Pane)
            continue;
        if (row.part == Part::Dimensions)
            continue;
        const bool pane = effectiveBool(untouched, Surface::Pane, row.part); // bool-ok: test state
        const bool tab = effectiveBool(untouched, Surface::Tab, row.part); // bool-ok: test state
        CHECK_MSG(pane == tab, partName(row.part));
    }

    // Setting a pane part decouples only that part.
    Values viaTab;
    setBool(viaTab, Key::TitleTabDirectory, false);
    CHECK(!effectiveBool(viaTab, Surface::Tab, Part::Directory));
    CHECK(!effectiveBool(viaTab, Surface::Pane, Part::Directory));
    Values decoupled = viaTab;
    setBool(decoupled, Key::TitlePaneDirectory, true);
    CHECK(!effectiveBool(decoupled, Surface::Tab, Part::Directory));
    CHECK(effectiveBool(decoupled, Surface::Pane, Part::Directory));
    // A sibling part still follows tab.
    CHECK(effectiveBool(decoupled, Surface::Pane, Part::AppTitle) == effectiveBool(decoupled, Surface::Tab, Part::AppTitle));

    // A program's status shows where there is room for a label that changes
    // as the program works: in the window, not in the tab strip, and a pane
    // follows the tab like every inheriting part.
    CHECK(effectiveBool(untouched, Surface::Window, Part::ProgramStatus));
    CHECK(!effectiveBool(untouched, Surface::Tab, Part::ProgramStatus));
    Values statusOnTabs;
    setBool(statusOnTabs, Key::TitleTabProgramStatus, true);
    CHECK_MSG(effectiveBool(statusOnTabs, Surface::Pane, Part::ProgramStatus),
        "a pane shows a program's status once the tab does, until it is told otherwise");
    setBool(statusOnTabs, Key::TitlePaneProgramStatus, false);
    CHECK(effectiveBool(statusOnTabs, Surface::Tab, Part::ProgramStatus));
    CHECK(!effectiveBool(statusOnTabs, Surface::Pane, Part::ProgramStatus));

    // Dimensions never inherit even when tab is true.
    Values tabDimensionsValues;
    setBool(tabDimensionsValues, Key::TitleTabDimensions, true);
    CHECK(effectiveBool(tabDimensionsValues, Surface::Tab, Part::Dimensions));
    CHECK(!effectiveBool(tabDimensionsValues, Surface::Pane, Part::Dimensions));
    setBool(tabDimensionsValues, Key::TitlePaneDimensions, true);
    CHECK(effectiveBool(tabDimensionsValues, Surface::Pane, Part::Dimensions));
}

TEST(testTextTableSurfacesAreIndependent)
{
    for (int i = 0; i < textDescriptorCount; ++i) {
        const TextDescriptor& row = textDescriptors[i];
        const Values untouched;
        CHECK(effectiveText(untouched, row.surface).empty());

        Values changed;
        setText(changed, row.surface, "hello");
        CHECK(std::string(effectiveText(changed, row.surface)) == "hello");
        // Other surfaces remain empty except pane inheriting tab.
        for (int j = 0; j < textDescriptorCount; ++j) {
            const TextDescriptor& other = textDescriptors[j];
            if (other.surface == row.surface)
                continue;
            if (row.surface == Surface::Tab && other.surface == Surface::Pane) {
                CHECK_MSG(std::string(effectiveText(changed, other.surface)) == "hello", surfaceName(other.surface));
                continue;
            }
            if (row.surface == Surface::Pane && other.surface == Surface::Tab) {
                // Pane does not affect tab.
                CHECK_MSG(effectiveText(changed, other.surface).empty(), surfaceName(other.surface));
                continue;
            }
            CHECK_MSG(effectiveText(changed, other.surface).empty(), surfaceName(other.surface));
        }
        // The text descriptor lookup matches the surface.
        const TextDescriptor* bySurface = descriptorForTextSurface(row.surface);
        CHECK(bySurface != nullptr);
        CHECK(bySurface->key == row.key);
        const TextDescriptor* byKey = descriptorForTextKey(row.key);
        CHECK(byKey != nullptr);
        CHECK(byKey->surface == row.surface);
    }
    // Pane text inherits tab while unset (the same rule the pane booleans have,
    // except dimensions). The effectiveComposition the weave reads does this.
    Values tabText;
    setText(tabText, Surface::Tab, "from-tab");
    CHECK(std::string(effectiveText(tabText, Surface::Pane)) == "from-tab");
    setText(tabText, Surface::Pane, "from-pane");
    CHECK(std::string(effectiveText(tabText, Surface::Pane)) == "from-pane");
    CHECK(std::string(effectiveText(tabText, Surface::Tab)) == "from-tab");
}

UBIQUITTY_TEST_MAIN()
