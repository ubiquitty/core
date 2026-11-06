// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_preferences_accessors — the effective value of every settings key.
//
// Each accessor answers one question: did the file say, and if not what does
// Ubiquitty do. Every frontend reads settings through these, and the sync
// protocol writes through the same fields, so an accessor reading the wrong
// flag hands one surface a value the file never stated.
//
// Most of them are one line of the same shape, and hand-writing two assertions
// each would be seventy chances to paste the wrong field name — the exact
// mistake the code itself is exposed to. The sweep below states the shape once
// and applies it to every key: unset answers the default, set answers what was
// set. The rows are the population, so a key added without a row is a gap the
// count at the end of this file reports.

#include "Core/GridBounds.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/PreferencesDefaults.h"
#include "Core/Preferences/Profile.h"
#include "Core/Preferences/RenderConfigurationFold.h"
#include "Core/Preferences/Schema.h"
#include "Core/Themes/Themes.h"

#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

using namespace Ubiquitty::Preferences;

namespace {

/// Renders any accessor result as text, so one comparison covers every type.
template<typename T>
std::string text(const T& value)
{
    if constexpr (std::is_same_v<T, bool>)
        return value ? "true" : "false";
    else if constexpr (std::is_enum_v<T>)
        return std::to_string(static_cast<long long>(value));
    else if constexpr (std::is_same_v<T, std::string>)
        return value;
    else if constexpr (std::is_convertible_v<T, const char*>)
        return std::string(value);
    else
        return std::to_string(value);
}

/// A value of the same type that is not the one given.
///
/// Enums flip their low bit rather than counting up, so the result stays inside
/// the enumerated range whatever the enum holds.
template<typename T>
T changed(T value)
{
    if constexpr (std::is_same_v<T, bool>)
        return !value;
    else if constexpr (std::is_enum_v<T>)
        return static_cast<T>(static_cast<std::underlying_type_t<T>>(value) ^ 1);
    else if constexpr (std::is_same_v<T, std::string>)
        return value + "-set-by-the-file";
    else
        return static_cast<T>(value + 7);
}

struct Row {
    const char* key;
    void (*state)(Values&);
    std::string (*effective)(const Values&);
    std::string (*fallback)();
};

// One row per key: which field the file writes, and which accessor reads it.
// The default is named by the accessor, which is the coupling the sweep is
// checking.
//
// Writing the field is what sets it: there is no witness flag beside the value
// for a row to set and then forget, so a half-written row is not a shape this
// macro can express.
//
// The stated value is flipped away from what the accessor answers rather than
// from what the field holds. An unset field holds nothing, and `false` for a
// key whose default is true would state the default back and read as no change.
#define ROW(accessor, field)                                              \
    Row                                                                   \
    {                                                                     \
        #accessor,                                                        \
            [](Values& c) { c.field = changed(Effective::accessor(c)); }, \
            [](const Values& c) { return text(Effective::accessor(c)); }, \
            [] { return text(Defaults::accessor); }                       \
    }

const Row rows[] = {
    ROW(fontSize, fontSize),
    ROW(ligatures, ligatures),
    ROW(lineSpacing, lineSpacingPoints),
    ROW(wideGlyphBoost, wideGlyphBoost),
    ROW(wideGlyphScale, wideGlyphScale),
    ROW(underlineThickness, underlineThickness),
    ROW(underlineOffset, underlineOffset),
    ROW(underlineSkipInk, underlineSkipInk),
    ROW(underlineFollowsInk, underlineFollowsInk),
    ROW(underlineSkipInkPad, underlineSkipInkPad),
    ROW(underlineLinks, underlineLinks),
    ROW(linkUnderlineStyle, linkUnderlineStyle),
    ROW(linkColorSystemBlue, linkColorSystemBlue),
    ROW(linkStylePerKind, linkStylePerKind),
    ROW(smartDoubleClick, smartDoubleClick),
    ROW(brightenBold, brightenBold),
    ROW(beamTerminalFont, beamTerminalFont),
    ROW(tekBrightPen, tekBrightPen),
    ROW(tekGlowVectors, tekGlowVectors),
    ROW(backgroundOpacity, backgroundOpacity),
    ROW(defaultBackgroundOpacity, defaultBackgroundOpacity),
    ROW(backgroundImageOpacity, backgroundImageOpacity),
    ROW(backgroundImageMode, backgroundImageMode),
    ROW(squareBottomCorners, squareBottomCorners),
    ROW(titlebarStyle, titlebarStyle),
    ROW(beamSurface, beamSurface),
    ROW(paneTitleBars, paneTitleBars),
    ROW(shouldWatchForZModem, shouldWatchForZModem),
    ROW(blinkAnimation, blinkAnimation),
    ROW(snapWindowResize, snapWindowResize),
    ROW(hideTitleBar, hideTitleBar),
    ROW(columnModeResizesWindow, columnModeResizesWindow),
    ROW(followSystemAppearance, followSystemAppearance),
    ROW(cursorShape, cursorShape),
    ROW(cursorMotion, cursorMotion),
    ROW(cursorBlink, cursorBlink),
    ROW(cursorLinger, cursorLinger),
    ROW(cursorBlinkRate, cursorBlinkRate),
    ROW(cursorAlpha, cursorAlpha),
    ROW(cursorInvertText, cursorInvertText),
    ROW(bellAudible, bellAudible),
    ROW(bellVisual, bellVisual),
    ROW(bellVisualIntensity, bellVisualIntensity),
    ROW(copyOnSelect, copyOnSelect),
    ROW(blockSelectModifier, blockSelectModifier),
    ROW(linksOverrideReporting, linksOverrideReporting),
    ROW(osc8ClickOpens, osc8ClickOpens),
    ROW(restrictSelectionToText, restrictSelectionToText),
    ROW(confirmMultilinePaste, confirmMultilinePaste),
    ROW(confirmMultilinePasteThreshold, confirmMultilinePasteThreshold),
    ROW(wheelSpeed, wheelSpeed),
    ROW(wheelLines, wheelLines),
    ROW(trackpadSpeed, trackpadSpeed),
    ROW(scrollBounce, scrollBounce),
    ROW(tabShortcutHints, tabShortcutHints),
    ROW(windowDragModifier, windowDragModifier),
    ROW(optionAsAlternate, optionAsAlternate),
    ROW(quakeEnabled, quakeEnabled),
    ROW(quakeHotkey, quakeHotkey),
    ROW(quakeAnimate, quakeAnimate),
    ROW(dockBounce, dockBounce),
    ROW(dockBadge, dockBadge),
    ROW(secureKeyboardEntry, secureKeyboardEntry),
    ROW(passwordDetect, passwordDetect),
    ROW(secureKeyboardEntryAtPasswordPrompts, secureKeyboardEntryAtPasswordPrompts),
    ROW(controlEnabled, controlEnabled),
    ROW(controlApprove, controlApprove),
    ROW(sessionPersistence, sessionPersistence),
    ROW(notifications, notifications),
    ROW(notificationSound, notificationSound),
    ROW(allowRunCommand, allowRunCommand),
    ROW(allowWebhook, allowWebhook),
    ROW(scriptingEnable, scriptingEnable),
    ROW(autoDeployHostOnRemoteConnect, autoDeployHostOnRemoteConnect),
    ROW(shouldPersistRemoteHistory, shouldPersistRemoteHistory),
    ROW(shouldCheckForUpdatesAutomatically, shouldCheckForUpdatesAutomatically),
    ROW(scrollbackLines, scrollbackLines),
};

#undef ROW

} // namespace

TEST(testEveryKeyAnswersItsDefaultUntilTheFileSaysOtherwise)
{
    const Values untouched;
    int checked = 0;
    for (const Row& row : rows) {
        // Nothing set: the accessor answers the canonical default rather than
        // whatever the struct's member initialiser happens to hold. Those two
        // can disagree, and the default is the one a user reads about.
        CHECK_MSG(row.effective(untouched) == row.fallback(), row.key);

        // Set: the accessor answers the file. A row reading a neighbouring
        // field would answer the default here instead.
        Values stated;
        row.state(stated);
        CHECK_MSG(row.effective(stated) != row.effective(untouched), row.key);
        ++checked;
    }
    // The sweep covers a population, so its size is asserted: a row list that
    // lost most of itself would still pass every comparison above.
    CHECK_MSG(checked == static_cast<int>(std::size(rows)), "every row ran");
    CHECK_MSG(checked >= 68, "the sweep still covers the whole settings surface");
}

// The keys whose answer is more than "did the file say". Each of these decides
// something the sweep above cannot state: a bound, a token the parser resolves,
// or a value inherited from a neighbouring key.

// The bell cue folds the two switches into the one enum the model carries, and
// it does it in Core because a frontend folding it keeps its own answer to what
// an unset switch means. The macOS window arm did exactly that — `audible ?
// true : ...` beside `Defaults::bellAudible` — so the whole truth table is
// pinned here, unset cases included, since those are the ones two answers can
// differ on while every set case agrees.
TEST(theBellCueIsOneFoldOverTwoSwitches)
{
    using Ubiquitty::BellMode;
    using Ubiquitty::Preferences::Effective::bellMode;

    Ubiquitty::Preferences::Values unset;
    CHECK_MSG(bellMode(unset) == (Defaults::bellAudible ? (Defaults::bellVisual ? BellMode::Both : BellMode::Audio) : (Defaults::bellVisual ? BellMode::Visual : BellMode::Off)),
        "an untouched config answers what Core's own defaults name, not what an arm remembers");

    Ubiquitty::Preferences::Values c;
    c.bellAudible = false;
    c.bellVisual = false;
    CHECK(bellMode(c) == BellMode::Off);
    c.bellAudible = true;
    c.bellVisual = false;
    CHECK(bellMode(c) == BellMode::Audio);
    c.bellAudible = false;
    c.bellVisual = true;
    CHECK(bellMode(c) == BellMode::Visual);
    c.bellAudible = true;
    c.bellVisual = true;
    CHECK(bellMode(c) == BellMode::Both);

    // One switch set and the other not is the mixed case an arm's private fold
    // gets wrong first, because it is where a remembered default and Core's
    // default both apply and only one of them is the setting.
    Ubiquitty::Preferences::Values mixed;
    mixed.bellVisual = true;
    CHECK_MSG(bellMode(mixed) == (Defaults::bellAudible ? BellMode::Both : BellMode::Visual),
        "the unset half still answers Core's default while the set half answers the file");
}

// The input encoding is stored as a name and consumed as an enum, and until
// this fold existed three frontend sites resolved it separately while no other
// shell resolved it at all. The unrecognised case is the one worth pinning:
// resolving it to "leave the value alone" rather than to UTF-8 would hand the
// decoder whatever the caller happened to initialise, which is a different
// encoding per call site.
TEST(anEncodingNameResolvesOnceOrFallsBackToUTF8)
{
    using Ubiquitty::Preferences::Effective::inputEncoding;
    using Ubiquitty::Unicode::Encoding;

    Ubiquitty::Preferences::Values unset;
    CHECK_MSG(inputEncoding(unset) == Encoding::UTF8, "a file naming no encoding decodes as UTF-8");

    Ubiquitty::Preferences::Values empty;
    empty.encoding = "";
    CHECK_MSG(inputEncoding(empty) == Encoding::UTF8, "and so does one naming an empty string");

    Ubiquitty::Preferences::Values unknown;
    unknown.encoding = "not-an-encoding";
    CHECK_MSG(inputEncoding(unknown) == Encoding::UTF8,
        "an unrecognised name answers UTF-8 rather than leaving the caller's own value standing");

    // A name this build knows resolves to it, spelled the way a reader writes
    // it rather than the way the table stores it.
    Ubiquitty::Preferences::Values latin;
    latin.encoding = "iso-8859-1";
    auto expected = Ubiquitty::Unicode::encodingFromName("iso-8859-1");
    CHECK_MSG(expected.has_value(), "the fixture names an encoding this build carries");
    CHECK(inputEncoding(latin) == *expected);
    CHECK_MSG(inputEncoding(latin) != Encoding::UTF8, "and it is not the fallback wearing its name");
}

TEST(testAValueOutsideItsRangeIsBroughtBackInside)
{
    // The runtime zoom clamps against the one font.size range, so the two
    // frontends cannot drift into different limits. Both overloads, because the
    // pixel path is a separate function and a bound is exactly what gets
    // rounded wrong when one is written twice.
    const Schema::NumericRange& range = Zoom::fontZoomRange();
    CHECK(range.minimum < range.maximum);

    CHECK_EQ(Zoom::clampFontZoom(range.minimum - 100.0), range.minimum);
    CHECK_EQ(Zoom::clampFontZoom(range.maximum + 100.0), range.maximum);
    CHECK_EQ(Zoom::clampFontZoom(range.minimum), range.minimum);
    CHECK_EQ(Zoom::clampFontZoom(range.maximum), range.maximum);
    const double inside = (range.minimum + range.maximum) / 2.0;
    CHECK_EQ(Zoom::clampFontZoom(inside), inside);

    const int lowest = static_cast<int>(range.minimum);
    const int highest = static_cast<int>(range.maximum);
    CHECK(Zoom::clampFontZoom(lowest - 100) == lowest);
    CHECK(Zoom::clampFontZoom(highest + 100) == highest);
    CHECK(Zoom::clampFontZoom(lowest) == lowest);
    CHECK(Zoom::clampFontZoom(highest) == highest);
    CHECK(Zoom::clampFontZoom((lowest + highest) / 2) == (lowest + highest) / 2);

    // The interface scale clamps against its own row, on the default path too —
    // so a default edited out of range is brought back rather than shipped.
    Values low;
    low.uiFontScale = -50.0;
    Values high;
    high.uiFontScale = 500.0;
    CHECK(Effective::uiFontScale(low) > 0.0);
    CHECK(Effective::uiFontScale(high) < 500.0);
    CHECK(Effective::uiFontScale(low) < Effective::uiFontScale(high));
    CHECK_EQ(Effective::uiFontScale(Values {}), Schema::clampToRange(Defaults::uiFontScale, "appearance", "ui_font_scale"));

    // A tag's colour is drawn at half strength unless the reader says
    // otherwise, and never so faint that it stops marking the window.
    CHECK_EQ(Effective::tagColorOpacity(Values {}), 0.5);
    Values faint;
    faint.tagColorOpacity = 0.0;
    Values over;
    over.tagColorOpacity = 3.0;
    CHECK_EQ(Effective::tagColorOpacity(faint), 0.1);
    CHECK_EQ(Effective::tagColorOpacity(over), 1.0);

    // The drop-down height clamps on both paths for the same reason.
    Values tall;
    tall.quakeHeight = 9.0;
    Values flat;
    flat.quakeHeight = -9.0;
    CHECK_EQ(Effective::quakeHeight(tall), 1.0);
    CHECK_EQ(Effective::quakeHeight(flat), 0.2);
    Values half;
    half.quakeHeight = 0.5;
    CHECK_EQ(Effective::quakeHeight(half), 0.5);
    CHECK_EQ(Effective::quakeHeight(Values {}), Defaults::quakeHeight);

    // The Tek colour mode is an index into two choices, and it arrives as a
    // number the file wrote.
    Values beyond;
    beyond.tekColorMode = 99;
    Values negative;
    negative.tekColorMode = -5;
    CHECK(Effective::tekColorMode(beyond) == 1);
    CHECK(Effective::tekColorMode(negative) == 0);
    Values custom;
    custom.tekColorMode = 1;
    CHECK(Effective::tekColorMode(custom) == 1);
    CHECK(Effective::tekColorMode(Values {}) == Defaults::tekColorMode);
}

TEST(testAZoomIsAnOffsetOverTheConfiguredSize)
{
    // A window's zoom rides over the configured size and stays within the
    // schema range; the configured size itself is never what a zoom changes.
    const Schema::NumericRange& range = Zoom::fontZoomRange();
    const double configured = (range.minimum + range.maximum) / 2.0;
    // The zoom's own step, a whole unit, not the schema row's.
    const double step = Zoom::fontZoomStep;
    CHECK_EQ(step, 1.0);
    CHECK(range.step < step); // the schema row is the finer of the two

    CHECK_EQ(Zoom::zoomedFontSize(configured, 0.0), configured);
    CHECK_EQ(Zoom::zoomedFontSize(configured, step), configured + step);
    CHECK_EQ(Zoom::zoomedFontSize(configured, -step), configured - step);
    CHECK_EQ(Zoom::zoomedFontSize(configured, 1000.0), range.maximum);
    CHECK_EQ(Zoom::zoomedFontSize(configured, -1000.0), range.minimum);
    // A configured size outside the range renders at the bound the file
    // would have been clamped to, so a zoom offset is relative to what the
    // window actually shows.
    CHECK_EQ(Zoom::zoomedFontSize(range.maximum + 100.0, 0.0), range.maximum);
    CHECK_EQ(Zoom::zoomedFontSize(range.maximum + 100.0, -step), range.maximum - step);

    // The offset that lands a window on a size is the inverse of the size it
    // renders, so a step taken from the live size lands exactly one step on.
    const double stepped = Zoom::fontZoomOffsetForSize(configured, configured + step);
    CHECK_EQ(stepped, step);
    CHECK_EQ(Zoom::zoomedFontSize(configured, stepped), configured + step);
    // Stepping past a bound records the bound, not the request past it.
    CHECK_EQ(Zoom::fontZoomOffsetForSize(configured, range.maximum + step), range.maximum - configured);
    CHECK_EQ(Zoom::fontZoomOffsetForSize(configured, range.minimum - step), range.minimum - configured);
    // Clearing the offset renders the configured size, whatever it is.
    CHECK_EQ(Zoom::zoomedFontSize(14.0, 0.0), 14.0);
    CHECK_EQ(Zoom::zoomedFontSize(Defaults::fontSize, 0.0), Defaults::fontSize);

    // A whole step over a fractional base keeps the fraction: a reader who set
    // 12.5 pt zooms to 13.5 and back to 12.5, never onto a whole point.
    CHECK_EQ(Zoom::zoomedFontSize(12.5, step), 13.5);
    CHECK_EQ(Zoom::zoomedFontSize(12.5, -step), 11.5);
    CHECK_EQ(Zoom::zoomedFontSize(12.5, 0.0), 12.5);
    CHECK_EQ(Zoom::fontZoomOffsetForSize(12.5, 13.5), step);
}

TEST(testATokenTheParserDoesNotKnowFallsBackRatherThanIndexing)
{
    // Two keys arrive as words and leave as something else. An unknown word
    // is what a typo in the file produces, and each has to answer a value the
    // rest of the program can use rather than whatever the conversion yields.
    Values space;
    space.colorSpace = "no-such-space";
    CHECK_MSG(!Effective::colorSpaceP3(space), "an unknown colour space is sRGB");
    space.colorSpace = "display-p3";
    CHECK_MSG(Effective::colorSpaceP3(space), "the wide space is recognised");
    space.colorSpace = "srgb";
    CHECK(!Effective::colorSpaceP3(space));
    CHECK_EQ(Effective::colorSpaceP3(Values {}), Defaults::colorSpaceP3);

    Values vision;
    vision.colorblindMode = "no-such-vision";
    CHECK_MSG(Effective::colorblindMode(vision) == Ubiquitty::Accessibility::ColorblindMode::None,
        "an unknown vision mode remaps nothing");
    vision.colorblindMode = "deuteranopia";
    CHECK(Effective::colorblindMode(vision) == Ubiquitty::Accessibility::ColorblindMode::Deuteranopia);
    CHECK(Effective::colorblindMode(Values {}) == Defaults::colorblindMode);
}

TEST(testAColourArrivesAsTextAndLeavesAsAColour)
{
    // The Tek beam and background are written as hex in the file and read as
    // packed values. Anything the reader cannot make sense of has to answer the
    // built-in colour rather than a partial parse — a half-read colour is a
    // beam nobody can see.
    Values c;
    c.tekBeamColor = "#00ff80";
    CHECK(Effective::tekBeamColor(c) == 0x00FF80u);
    c.tekBeamColor = "00ff80"; // the hash is optional
    CHECK(Effective::tekBeamColor(c) == 0x00FF80u);
    c.tekBeamColor = "  #00FF80  "; // surrounding space and upper case
    CHECK(Effective::tekBeamColor(c) == 0x00FF80u);
    c.tekBeamColor = "\t#00ff80\t";
    CHECK(Effective::tekBeamColor(c) == 0x00FF80u);

    // Every way it can be malformed answers the built-in.
    for (const char* bad : { "", "#", "#fff", "#0000000", "00ff8", "00ff800",
             "#00ff8g", "gggggg", "#00 ff80", "   ", "#-00080" }) {
        c.tekBeamColor = bad;
        CHECK_MSG(Effective::tekBeamColor(c) == Defaults::tekBeamColor, bad);
    }

    // The background reads through the same parser and its own default, so a
    // row wired to the wrong fallback shows up here.
    Values b;
    b.tekBackgroundColor = "#102030";
    CHECK(Effective::tekBackgroundColor(b) == 0x102030u);
    b.tekBackgroundColor = "nope";
    CHECK(Effective::tekBackgroundColor(b) == Defaults::tekBackgroundColor);

    // Unset, each answers its own built-in through the same parser.
    CHECK(Effective::tekBeamColor(Values {}) == Defaults::tekBeamColor);
    CHECK(Effective::tekBackgroundColor(Values {}) == Defaults::tekBackgroundColor);
    CHECK_MSG(Defaults::tekBeamColor != Defaults::tekBackgroundColor,
        "the two built-ins differ, so a swapped fallback would show");
}

TEST(testAKeyWithNoValueOfItsOwnTakesOneFromItsNeighbour)
{
    // Three keys answer from somewhere other than a built-in when the file is
    // silent, which is what a sweep over flag-and-field cannot state.

    // The titleless top padding mirrors the effective bottom padding, so a
    // window with no title bar is vertically symmetric without the user saying
    // so twice.
    Values padded;
    padded.paddingBottom = 12.0;
    CHECK_EQ(Effective::titlelessPaddingTop(padded), padded.resolvedPadding().bottom);
    // And an explicit value wins, clamped to the same window-padding bound.
    padded.titlelessPaddingTop = 999.0;
    CHECK_EQ(Effective::titlelessPaddingTop(padded), 64.0);
    padded.titlelessPaddingTop = -5.0;
    CHECK_EQ(Effective::titlelessPaddingTop(padded), 0.0);
    padded.titlelessPaddingTop = 20.0;
    CHECK_EQ(Effective::titlelessPaddingTop(padded), 20.0);

    Values radius;
    CHECK_EQ(Effective::blurRadius(radius), Defaults::blurRadius);
    radius.blurRadius = 33.0;
    CHECK_MSG(Effective::blurRadius(radius) == 33.0, "an explicit radius wins over the default");
    CHECK_EQ(Effective::blurRadius(Values {}), Defaults::blurRadius);

    // A profile overrides the file for the title bar, and falls through to it
    // when the profile is silent.
    Values file;
    file.hideTitleBar = true;
    Profile silent;
    CHECK_MSG(Effective::hideTitleBar(file, silent), "a silent profile takes the file's answer");
    Profile stated;
    stated.hideTitleBar = false;
    CHECK_MSG(!Effective::hideTitleBar(file, stated), "a profile that speaks wins");
    stated.hideTitleBar = true;
    file.hideTitleBar = false;
    CHECK(Effective::hideTitleBar(file, stated));
}

namespace {

/// The eleven fields of a title composition as one comparable string.
std::string shape(const Effective::TitleComposition& t)
{
    std::string s;
    for (bool flag : { t.showsDirectory, t.showsDirectoryFullPath, t.showsAppTitle,
             t.showsProcess, t.showsProcessArguments, t.showsShellCommand,
             t.showsTTYName, t.showsProfileName, t.showsDimensions, t.showsProgramStatus })
        s += flag ? '1' : '0';
    s += '|';
    s += std::string(t.baseTitle);
    return s;
}

} // namespace

TEST(testTheThreeTitleSurfacesDifferWhereTheyAreMeantTo)
{
    const Values untouched;
    const Effective::TitleComposition window = Effective::windowTitleComposition(untouched);
    const Effective::TitleComposition tab = Effective::tabTitleComposition(untouched);
    const Effective::TitleComposition pane = Effective::paneTitleComposition(untouched);

    // The window shows the folder name and its dimensions; the tab shows the
    // full path and no dimensions. Asserted as the difference rather than as
    // two lists, because the two are written from one template and a copied
    // line is what makes them agree by accident.
    CHECK_MSG(!window.showsDirectoryFullPath, "the window shows the folder name");
    CHECK_MSG(tab.showsDirectoryFullPath, "the tab shows the whole path");
    CHECK_MSG(window.showsDimensions, "the window carries the size");
    CHECK_MSG(!tab.showsDimensions, "the tab strip has no room for it");
    CHECK_MSG(window.showsProgramStatus, "the window carries a program's status");
    CHECK_MSG(!tab.showsProgramStatus, "the tab strip would re-lay out as it changes");
    CHECK_MSG(shape(window) != shape(tab), "the two surfaces are not the same list");

    // A pane inherits the tab, which is what keeps a split from stating the
    // same preferences twice — except the size, which a pane label never shows.
    CHECK_MSG(pane.showsDirectory == tab.showsDirectory, "a pane follows the tab");
    CHECK_MSG(pane.showsDirectoryFullPath == tab.showsDirectoryFullPath, "including the path");
    CHECK_MSG(!pane.showsDimensions, "and never the size, whatever the tab says");

    // Everything opt-in is off until asked for: argv, the shell command, the
    // tty name and the profile name each expose something a screen share would
    // carry, on all three surfaces.
    for (const Effective::TitleComposition& t : { window, tab, pane }) {
        CHECK(!t.showsProcessArguments);
        CHECK(!t.showsShellCommand);
        CHECK(!t.showsTTYName);
        CHECK(!t.showsProfileName);
        CHECK(t.baseTitle.empty());
    }

    // Every field follows the file when the file speaks. Written per surface so
    // a field wired to a neighbour's flag answers the untouched shape here.
    Values stated;
    stated.titleWindowDirectory = !window.showsDirectory;
    stated.titleWindowDirectoryPath = !window.showsDirectoryFullPath;
    stated.titleWindowAppTitle = !window.showsAppTitle;
    stated.titleWindowProcess = !window.showsProcess;
    stated.titleWindowProcessArguments = true;
    stated.titleWindowShellCommand = true;
    stated.titleWindowTTYName = true;
    stated.titleWindowProfileName = true;
    stated.titleWindowDimensions = !window.showsDimensions;
    stated.titleWindowProgramStatus = !window.showsProgramStatus;
    stated.titleWindowText = "stated";
    const Effective::TitleComposition saidSo = Effective::windowTitleComposition(stated);
    CHECK(saidSo.showsDirectory != window.showsDirectory);
    CHECK(saidSo.showsDirectoryFullPath != window.showsDirectoryFullPath);
    CHECK(saidSo.showsAppTitle != window.showsAppTitle);
    CHECK(saidSo.showsProcess != window.showsProcess);
    CHECK(saidSo.showsProcessArguments && saidSo.showsShellCommand);
    CHECK(saidSo.showsTTYName && saidSo.showsProfileName);
    CHECK(saidSo.showsDimensions != window.showsDimensions);
    CHECK(saidSo.showsProgramStatus != window.showsProgramStatus);
    CHECK_STR_EQ(std::string(saidSo.baseTitle).c_str(), "stated");

    // The tab, and a pane inheriting it rather than the untouched values.
    Values tabStated;
    tabStated.titleTabDirectory = !tab.showsDirectory;
    tabStated.titleTabDirectoryPath = !tab.showsDirectoryFullPath;
    tabStated.titleTabAppTitle = !tab.showsAppTitle;
    tabStated.titleTabProcess = !tab.showsProcess;
    tabStated.titleTabProcessArguments = true;
    tabStated.titleTabShellCommand = true;
    tabStated.titleTabTTYName = true;
    tabStated.titleTabProfileName = true;
    tabStated.titleTabDimensions = true;
    tabStated.titleTabProgramStatus = true;
    tabStated.titleTabText = "from-the-tab";
    const Effective::TitleComposition tabSaidSo = Effective::tabTitleComposition(tabStated);
    const Effective::TitleComposition paneFollows = Effective::paneTitleComposition(tabStated);
    CHECK(tabSaidSo.showsDirectory != tab.showsDirectory);
    CHECK(tabSaidSo.showsDimensions);
    CHECK_MSG(paneFollows.showsDirectory == tabSaidSo.showsDirectory, "the pane follows what the tab was told");
    CHECK_MSG(paneFollows.showsTTYName == tabSaidSo.showsTTYName, "on every inherited field");
    CHECK_MSG(paneFollows.showsProgramStatus && tabSaidSo.showsProgramStatus, "a program's status among them");
    CHECK_MSG(!paneFollows.showsDimensions, "except the size, which is never inherited");
    CHECK_STR_EQ(std::string(paneFollows.baseTitle).c_str(), "from-the-tab");

    // And a pane that states its own values stops following.
    Values paneStated = tabStated;
    paneStated.titlePaneDirectory = !tabSaidSo.showsDirectory;
    paneStated.titlePaneDirectoryPath = !tabSaidSo.showsDirectoryFullPath;
    paneStated.titlePaneAppTitle = !tabSaidSo.showsAppTitle;
    paneStated.titlePaneProcess = !tabSaidSo.showsProcess;
    paneStated.titlePaneProcessArguments = false;
    paneStated.titlePaneShellCommand = false;
    paneStated.titlePaneTTYName = false;
    paneStated.titlePaneProfileName = false;
    paneStated.titlePaneDimensions = true;
    paneStated.titlePaneText = "from-the-pane";
    const Effective::TitleComposition own = Effective::paneTitleComposition(paneStated);
    CHECK(own.showsDirectory != tabSaidSo.showsDirectory);
    CHECK(own.showsDirectoryFullPath != tabSaidSo.showsDirectoryFullPath);
    CHECK(own.showsAppTitle != tabSaidSo.showsAppTitle);
    CHECK(own.showsProcess != tabSaidSo.showsProcess);
    CHECK(!own.showsProcessArguments && !own.showsShellCommand);
    CHECK(!own.showsTTYName && !own.showsProfileName);
    CHECK_MSG(own.showsDimensions, "a pane can be told to show the size");
    CHECK_STR_EQ(std::string(own.baseTitle).c_str(), "from-the-pane");
}

TEST(testTheRenderConfigurationAgreesWithTheAccessorsItIsBuiltFrom)
{
    // One struct the renderers read instead of calling twenty accessors. It has
    // to agree with them: a field wired to the wrong one is a setting that
    // applies everywhere except where it is drawn.
    Values c;
    c.fontSize = 18.0;
    c.fontFamily = "Menlo";
    c.cjkFallbackFont = "Hiragino";
    c.baselineOffset = 1.5;

    const Ubiquitty::Rendering::RenderConfiguration grid = Effective::renderConfiguration(c);
    CHECK(grid.fontFamily.view() == "Menlo");
    CHECK(grid.cjkFallbackFamily.view() == "Hiragino");
    CHECK(grid.symbolFallbackFamily.view() == Effective::symbolFallbackFamily(c));
    CHECK_EQ(grid.fontSizePoints, 18.0);
    CHECK_EQ(grid.baselineOffsetPoints, 1.5);

    CHECK_EQ(grid.lineSpacingPoints, Effective::lineSpacing(c));
    CHECK(grid.hasLigatures == Effective::ligatures(c));
    CHECK(grid.shouldBoostWideGlyphs == Effective::wideGlyphBoost(c));
    CHECK_EQ(grid.wideGlyphScale, Effective::wideGlyphScale(c));
    CHECK_EQ(grid.underlineThicknessScale, Effective::underlineThickness(c));
    CHECK_EQ(grid.underlineOffsetPoints, Effective::underlineOffset(c));
    CHECK(grid.shouldUnderlineSkipInk == Effective::underlineSkipInk(c));
    CHECK(grid.shouldUnderlineFollowInk == Effective::underlineFollowsInk(c));
    CHECK_EQ(grid.underlineSkipInkPaddingPoints, Effective::underlineSkipInkPad(c));
    CHECK(grid.shouldUnderlineLinks == Effective::underlineLinks(c));
    CHECK(grid.linkUnderlineStyle == Effective::linkUnderlineStyle(c));
    CHECK(grid.shouldUseSystemBlueLinkColor == Effective::linkColorSystemBlue(c));
    CHECK(grid.shouldStyleLinksPerKind == Effective::linkStylePerKind(c));
    CHECK(grid.brailleStyle == Effective::brailleStyle(c));
    CHECK(grid.selectionStyle == Effective::selectionStyle(c));
    CHECK_EQ(grid.paddingBottomPoints, c.resolvedPadding().bottom);

    // A silent file names no family, so a frontend supplies its native face.
    CHECK(Effective::renderConfiguration(Values {}).fontFamily.view().empty());

    // And the size travels through the same clamp the zoom uses, so a file
    // asking for a size no renderer can draw is brought back rather than
    // reaching one.
    Values huge;
    huge.fontSize = 10000.0;
    CHECK_EQ(Effective::renderConfiguration(huge).fontSizePoints, Zoom::fontZoomRange().maximum);
}

/// The symbol face resolves through two keys, in one order.
///
/// `font.symbol_fallback` is what Core routes symbols with and
/// `font.fallback` is the alias under it. A frontend seeding a renderer from
/// the alias alone shows a user their `symbol_fallback` being ignored, which
/// is the shape of the defect this fold exists to make impossible.
TEST(theSymbolFallbackKeyOutranksTheAlias)
{
    Values none;
    CHECK_MSG(Effective::symbolFallbackFamily(none) == "Symbols Nerd Font Mono", "an unset symbol fallback uses the bundled icon face");
    CHECK(Effective::renderConfiguration(none).symbolFallbackFamily.view() == "Symbols Nerd Font Mono");

    Values aliasOnly;
    aliasOnly.fallbackFont = "Alias Face";
    CHECK_MSG(Effective::symbolFallbackFamily(aliasOnly) == std::string("Alias Face"),
        "the alias answers where it is the only key set");

    Values symbolOnly;
    symbolOnly.symbolFallbackFont = "Symbol Face";
    CHECK_MSG(Effective::symbolFallbackFamily(symbolOnly) == std::string("Symbol Face"),
        "the symbol key answers where it is the only key set");

    Values both;
    both.fallbackFont = "Alias Face";
    both.symbolFallbackFont = "Symbol Face";
    CHECK_MSG(Effective::symbolFallbackFamily(both) == std::string("Symbol Face"),
        "the symbol key wins where both are set");

    // An empty symbol key is a stated value, not an absent one: it selects the
    // default face rather than falling through to whatever the alias names.
    Values emptySymbol;
    emptySymbol.fallbackFont = "Alias Face";
    emptySymbol.symbolFallbackFont = "";
    CHECK(Effective::symbolFallbackFamily(emptySymbol) == "Symbols Nerd Font Mono");
    CHECK(Effective::renderConfiguration(emptySymbol).symbolFallbackFamily.view() == "Symbols Nerd Font Mono");
    CHECK(Effective::renderConfiguration(both).symbolFallbackFamily.view() == "Symbol Face");
}

// A blur radius asks to see the desktop through the window, and the opacity is
// what decides whether anything can be. At 1.0 the tint is solid: nothing
// behind the window reaches the person while the grid is drawn, and a shell
// that treated the radius as translucency by itself hung a material under the
// panes that showed only in the gap before the first frame — a new tab opening
// onto a blurred desktop rather than a terminal.
TEST(testABlurRadiusDoesNotMakeAnOpaqueWindowTranslucent)
{
    Values opaqueWithBlur;
    opaqueWithBlur.backgroundOpacity = 1.0;
    opaqueWithBlur.blurRadius = 50.0;
    const auto opaque = Effective::renderConfiguration(opaqueWithBlur);
    CHECK_MSG(opaque.backgroundOpacity == 1.0, "the opacity is reported as set");
    CHECK_MSG(opaque.blurRadiusPoints == 50.0, "the radius is reported as set, so a settings pane shows it");
    Values opaqueDefaultCells = opaqueWithBlur;
    opaqueDefaultCells.defaultBackgroundOpacity.reset();
    CHECK_MSG(Effective::backgroundEffects(opaqueDefaultCells, Effective::SystemTransparency::Allowed, {}).defaultBackgroundOpacity == 1.0,
        "an opaque window reads the per-default-cell alpha as 1");
    opaqueWithBlur.defaultBackgroundOpacity = 0.5;
    CHECK_MSG(!Effective::backgroundEffects(opaqueWithBlur, Effective::SystemTransparency::Allowed, {}).isTranslucent,
        "and a radius does not make it see-through");

    // Below one the same keys mean what they say, so the case above is the
    // opacity deciding and not the fold ignoring the key everywhere.
    Values translucentWithBlur;
    translucentWithBlur.backgroundOpacity = 0.8;
    translucentWithBlur.blurRadius = 50.0;
    translucentWithBlur.defaultBackgroundOpacity = 0.5;
    CHECK_MSG(Effective::backgroundEffects(translucentWithBlur, Effective::SystemTransparency::Allowed, {}).defaultBackgroundOpacity == 0.5,
        "a translucent window reads the per-default-cell alpha the key sets");
}

TEST(blurAndVibrancyStayEnabledWithoutChangingOpacity)
{
    Values defaults;
    CHECK_EQ(Effective::blurRadius(defaults), 20.0);
    CHECK(Effective::backgroundVibrancy(defaults));
    const auto opaque = Effective::backgroundEffects(defaults, Effective::SystemTransparency::Allowed, {});
    CHECK(!opaque.isTranslucent && !opaque.isBlurred && !opaque.usesBackgroundVibrancy);
    defaults.backgroundOpacity = 0.5;
    const auto translucent = Effective::backgroundEffects(defaults, Effective::SystemTransparency::Allowed, {});
    CHECK(translucent.isTranslucent && translucent.isBlurred && translucent.usesBackgroundVibrancy);
    Values blurOnly;
    blurOnly.blurRadius = 20.0;
    CHECK_EQ(Effective::backgroundOpacity(blurOnly), 1.0);
    CHECK_EQ(Effective::renderConfiguration(blurOnly).backgroundOpacity, 1.0);
    CHECK(!Effective::backgroundEffects(blurOnly, Effective::SystemTransparency::Allowed, {}).isBlurred);

    Values blurOff;
    blurOff.blurRadius = 0.0;
    CHECK_EQ(Effective::backgroundOpacity(blurOff), Defaults::backgroundOpacity);
    CHECK_EQ(Effective::backgroundOpacity(Values {}), Defaults::backgroundOpacity);

    Values blurWithOpacity;
    blurWithOpacity.blurRadius = 20.0;
    blurWithOpacity.backgroundOpacity = 0.5;
    CHECK_MSG(Effective::backgroundOpacity(blurWithOpacity) == 0.5, "a set key is what it says");
}

// The renderer's alpha is part of the ground's answer: an opaque window,
// whether by the key or by Reduce Transparency, draws opaque, because the
// compositor keeps a translucent frame's darkened colour and drops its alpha.
TEST(anOpaqueWindowDrawsItsRendererOpaque)
{
    const Effective::BackgroundRequest tinted { .backgroundOpacity = 0.5, .defaultBackgroundOpacity = 0.3 };
    const auto allowed = Effective::backgroundEffects(tinted, Effective::SystemTransparency::Allowed);
    CHECK_MSG(allowed.rendererOpacity == 0.5, "a translucent window draws the asked tint");
    CHECK_MSG(allowed.defaultBackgroundOpacity == 0.3, "and the default-cell extra");
    const auto reduced = Effective::backgroundEffects(tinted, Effective::SystemTransparency::Reduced);
    CHECK_MSG(!reduced.isTranslucent, "Reduce Transparency makes the window opaque");
    CHECK_MSG(reduced.rendererOpacity == 1.0, "and the renderer with it");
    CHECK_MSG(reduced.defaultBackgroundOpacity == 1.0, "the default-cell extra too");

    const Effective::BackgroundRequest opaque { .backgroundOpacity = 1.0, .defaultBackgroundOpacity = 0.3 };
    const auto solid = Effective::backgroundEffects(opaque, Effective::SystemTransparency::Allowed);
    CHECK_MSG(solid.rendererOpacity == 1.0 && solid.defaultBackgroundOpacity == 1.0,
        "an opaque window by the key draws opaque whatever the default-cell key says");
}

// Both effects, from the same two facts, with the accessibility switch over
// them. The pairing is the point: a shell that derived translucency and blur
// apart is how a radius over an opaque window hung a material that showed only
// before the first frame.
TEST(reduceTransparencyFlattensBothBackgroundEffects)
{
    Values blurred;
    blurred.blurRadius = 20;
    blurred.backgroundOpacity = 0.8;
    CHECK_MSG(Effective::backgroundEffects(blurred, Effective::SystemTransparency::Allowed, {}).isBlurred,
        "a positive radius over a translucent window blurs");
    CHECK_MSG(Effective::backgroundEffects(blurred, Effective::SystemTransparency::Allowed, {}).isTranslucent,
        "and the window is see-through");

    Values opaqueWithRadius;
    opaqueWithRadius.blurRadius = 20;
    opaqueWithRadius.backgroundOpacity = 1;
    CHECK_MSG(!Effective::backgroundEffects(opaqueWithRadius, Effective::SystemTransparency::Allowed, {}).isBlurred,
        "a radius over an opaque window is asked for and not in effect");
    CHECK_MSG(!Effective::backgroundEffects(opaqueWithRadius, Effective::SystemTransparency::Allowed, {}).isTranslucent,
        "and it does not make the window see-through");

    Values sheer;
    sheer.blurRadius = 0;
    sheer.backgroundOpacity = 0.5;
    CHECK_MSG(Effective::backgroundEffects(sheer, Effective::SystemTransparency::Allowed, {}).isTranslucent,
        "an opacity below one is see-through");
    CHECK_MSG(!Effective::backgroundEffects(sheer, Effective::SystemTransparency::Allowed, {}).isBlurred,
        "and carries no blur it did not ask for");

    const Values plain;
    CHECK(!Effective::backgroundEffects(plain, Effective::SystemTransparency::Allowed, {}).isTranslucent);
    CHECK(!Effective::backgroundEffects(plain, Effective::SystemTransparency::Allowed, {}).isBlurred);

    // The switch outranks both keys: a reader who asked the system for opaque
    // materials gets them here too.
    for (const Values& asked : { blurred, opaqueWithRadius, sheer, plain }) {
        const Effective::BackgroundEffects reduced
            = Effective::backgroundEffects(asked, Effective::SystemTransparency::Reduced, {});
        CHECK_MSG(!reduced.isTranslucent, "reduce transparency answers opaque");
        CHECK_MSG(!reduced.isBlurred, "reduce transparency answers unblurred");
    }
}

UBIQUITTY_TEST_MAIN()

TEST(anUnpinnedLaunchGridIsTheDefaultGrid)
{
    // An empty document pins nothing.
    Values empty;
    std::istringstream nothing("");
    (void)parse(nothing, empty);
    CHECK(Effective::launchSettings(empty).columns == Ubiquitty::defaultColumns);
    CHECK(Effective::launchSettings(empty).rows == Ubiquitty::defaultRows);
    CHECK(!Effective::launchSettings(empty).isGridPinned);
    // Zero is written as "no pin", and reads as the default grid rather than
    // as a grid of no cells.
    Values zero;
    std::istringstream zeroes("[window]\ninitial_columns = 0\ninitial_rows = 0\n");
    (void)parse(zeroes, zero);
    CHECK(Effective::launchSettings(zero).columns == Ubiquitty::defaultColumns);
    CHECK(Effective::launchSettings(zero).rows == Ubiquitty::defaultRows);
    CHECK(!Effective::launchSettings(zero).isGridPinned);
    // One axis pinned leaves the other at its default and the grid unpinned.
    Values half;
    std::istringstream oneAxis("[window]\ninitial_columns = 120\n");
    (void)parse(oneAxis, half);
    CHECK(Effective::launchSettings(half).columns == 120);
    CHECK(Effective::launchSettings(half).rows == Ubiquitty::defaultRows);
    CHECK(!Effective::launchSettings(half).isGridPinned);
}

TEST(aProfilesStartupHostReachesItsLaunchSettings)
{
    Values configuration;
    std::istringstream input("[[profiles]]\nname = \"Remote\"\nstartup_host = \"prod.example\"\n"
                             "[[profiles]]\nname = \"Child\"\nparent = \"Remote\"\n"
                             "[[profiles]]\nname = \"Local\"\n");
    (void)parse(input, configuration);
    CHECK_STR_EQ(Effective::launchSettings(configuration, resolvedProfile(configuration, "Remote")).startupHost, "prod.example");
    CHECK_MSG(Effective::launchSettings(configuration, resolvedProfile(configuration, "Child")).startupHost == "prod.example",
        "a child profile inherits the host its parent opens on");
    CHECK(Effective::launchSettings(configuration, resolvedProfile(configuration, "Local")).startupHost.empty());
    CHECK_MSG(Effective::launchSettings(configuration).startupHost.empty(), "a window opened under no profile is local");
}

TEST(textContrastRequiresAnAllowedBlur)
{
    Values configuration;
    configuration.blurRadius = 20;
    configuration.backgroundOpacity = 0.5;
    CHECK(!Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, {}).shouldEnhanceTextOnBlur);
    configuration.shouldEnhanceTextOnBlur = true;
    CHECK(Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, {}).shouldEnhanceTextOnBlur);
    CHECK(!Effective::backgroundEffects(configuration, Effective::SystemTransparency::Reduced, {}).shouldEnhanceTextOnBlur);
    configuration.blurRadius = 0;
    CHECK(!Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, {}).shouldEnhanceTextOnBlur);
    configuration.blurRadius = 20;
    configuration.backgroundOpacity = 1;
    CHECK(!Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, {}).shouldEnhanceTextOnBlur);
}

TEST(perceptualTextContrastDefaultsOffAndIsIndependentOfBlur)
{
    Values configuration;
    CHECK(!Effective::textContrast(configuration));
    CHECK(!Effective::renderConfiguration(configuration).shouldEnhanceTextContrast);
    CHECK(!Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, {}).isBlurred);
    Ubiquitty::Themes::ThemeAppearance appearance;
    appearance.textContrast = false;
    CHECK(!Effective::textContrast(configuration, appearance));
    configuration.textContrast = true;
    CHECK_MSG(Effective::textContrast(configuration, appearance), "a set key outranks the theme");
    configuration.textContrast = false;
    configuration.shouldEnhanceTextOnBlur = true;
    configuration.blurRadius = 20;
    configuration.backgroundOpacity = 0.5;
    appearance.textContrast = true;
    CHECK(!Effective::textContrast(configuration, appearance));
    CHECK(Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, appearance).shouldEnhanceTextOnBlur);
    CHECK_MSG(Effective::textContrast(Values {}, appearance), "an unset key takes the theme's answer");
}

TEST(aWindowIsCompositedForTheThemeItShowsNotTheOnePinned)
{
    // A reader who follows the system keeps a pinned theme beside the pair:
    // here a glass one, while the window shows a theme with no opinion of
    // its own about its ground.
    auto pinned = *Ubiquitty::Themes::get(0);
    pinned.name = "Pinned glass";
    pinned.appearance.useGlass();
    CHECK(Ubiquitty::Themes::registerCustom(pinned) >= 0);
    Values followsSystem;
    followsSystem.followSystemAppearance = true;
    followsSystem.themeName = "Pinned glass";
    CHECK_MSG(Effective::backgroundOpacity(followsSystem) == 0.0, "the pinned theme is glass, so the case below can tell");
    const Ubiquitty::Themes::ThemeAppearance shown;
    const auto effects = Effective::backgroundEffects(followsSystem, Effective::SystemTransparency::Allowed, shown);
    CHECK_MSG(!effects.isTranslucent && effects.rendererOpacity == 1.0, "the pinned theme's glass does not reach a window showing another");
    CHECK_MSG(!effects.isBlurred && !effects.shouldEnhanceTextOnBlur, "nor its blur and vibrant text");
    CHECK_MSG(Effective::textContrast(followsSystem, shown) == Defaults::shouldEnhanceTextContrast, "nor its text contrast");
    // The theme the window shows still decides, and a key set still wins.
    Ubiquitty::Themes::ThemeAppearance glass;
    glass.useGlass();
    CHECK(Effective::backgroundEffects(followsSystem, Effective::SystemTransparency::Allowed, glass).isTranslucent);
    followsSystem.backgroundOpacity = 0.6;
    CHECK(Effective::backgroundEffects(followsSystem, Effective::SystemTransparency::Allowed, shown).rendererOpacity == 0.6);
    Ubiquitty::Themes::clearCustom();
}

TEST(themeAppearanceOverridesRespectAccessibility)
{
    Values configuration;
    Ubiquitty::Themes::ThemeAppearance appearance;
    appearance.useGlass();
    const auto glass = Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, appearance);
    CHECK(glass.isTranslucent);
    CHECK(glass.isBlurred);
    CHECK(glass.rendererOpacity == 0);
    CHECK(glass.blurRadius == 20);
    CHECK(glass.shouldEnhanceTextOnBlur);
    const auto reduced = Effective::backgroundEffects(configuration, Effective::SystemTransparency::Reduced, appearance);
    CHECK(!reduced.isTranslucent);
    CHECK(!reduced.isBlurred);
    CHECK(!reduced.shouldEnhanceTextOnBlur);
    CHECK(reduced.rendererOpacity == 1);
    configuration.backgroundOpacity = 0.6;
    configuration.blurRadius = 12;
    configuration.shouldEnhanceTextOnBlur = false;
    const auto adjusted = Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, appearance);
    CHECK(adjusted.rendererOpacity == 0.6);
    CHECK(adjusted.blurRadius == 12);
    CHECK(!adjusted.shouldEnhanceTextOnBlur);
    const auto inherited = Effective::backgroundEffects(Values {}, Effective::SystemTransparency::Allowed, {});
    CHECK(!inherited.isTranslucent);
    CHECK(!inherited.shouldEnhanceTextOnBlur);
}

TEST(backgroundVibrancyIsIndependentAndRequiresAllowedBlur)
{
    Values configuration;
    CHECK(Effective::backgroundVibrancy(configuration));
    configuration.blurRadius = 20;
    configuration.backgroundOpacity = 0.25;
    configuration.backgroundVibrancy = true;
    configuration.shouldEnhanceTextOnBlur = false;
    const auto allowed = Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, {});
    CHECK(allowed.usesBackgroundVibrancy);
    CHECK(!allowed.shouldEnhanceTextOnBlur);
    CHECK(!Effective::backgroundEffects(configuration, Effective::SystemTransparency::Reduced, {}).usesBackgroundVibrancy);
    configuration.blurRadius = 0;
    CHECK(!Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, {}).usesBackgroundVibrancy);
    configuration.blurRadius = 20;
    configuration.backgroundOpacity = 1;
    CHECK(!Effective::backgroundEffects(configuration, Effective::SystemTransparency::Allowed, {}).usesBackgroundVibrancy);

    Ubiquitty::Themes::ThemeAppearance appearance;
    appearance.useGlass();
    CHECK(appearance.backgroundVibrancy == true);
    CHECK(Effective::backgroundEffects(Values {}, Effective::SystemTransparency::Allowed, appearance).usesBackgroundVibrancy);
    Values off;
    off.backgroundVibrancy = false;
    CHECK(!Effective::backgroundEffects(off, Effective::SystemTransparency::Allowed, appearance).usesBackgroundVibrancy);
}

// The proportional face is configured at a size of its own, and a zoom that
// moves the grid moves it by the same ratio; an unconfigured companion stays
// unconfigured, and a grid with no size to take a ratio from leaves it alone.
TEST(testACompanionFaceZoomsByTheGridsRatio)
{
    CHECK(std::abs(Zoom::zoomedCompanionSize(11.25, 11.25, 12.0) - 12.0) < 1e-9);
    CHECK(std::abs(Zoom::zoomedCompanionSize(11.25, 22.5, 12.0) - 24.0) < 1e-9);
    CHECK(std::abs(Zoom::zoomedCompanionSize(10.0, 8.0, 12.0) - 9.6) < 1e-9);
    CHECK_EQ(Zoom::zoomedCompanionSize(11.25, 22.5, 0.0), 0.0);
    CHECK_EQ(Zoom::zoomedCompanionSize(0.0, 22.5, 12.0), 12.0);
}
