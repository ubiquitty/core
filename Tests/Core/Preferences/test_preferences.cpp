// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Themes/ThemeTOMLWrite.h"
#include "Harness.h"

// test_preferences.cpp — the TOML-subset parser feeding the config module.
// Pins sectioned key/value dispatch, typed scalars, comment handling,
// the is-set flags, and the forgiving "unknown keys are ignored"
// contract.

#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/PreferencesDefaults.h"
#include "Core/Preferences/RenderConfigurationFold.h"
#include "Core/Preferences/RenderStyle.h"
#include "Core/Preferences/TOMLString.h"
#include "Core/Themes/ThemeTOMLWrite.h"
#include "Core/Unicode/Encodings.h"
#include "Core/Unicode/Normalize.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using Ubiquitty::Preferences::BackgroundImageMode;
using Ubiquitty::Preferences::BeamPresentation;
using Ubiquitty::Preferences::BeamSurface;
using Ubiquitty::Preferences::beamSurfaceFromString;
using Ubiquitty::Preferences::beamSurfaceToString;
using Ubiquitty::Preferences::ColorSpace;
using Ubiquitty::Preferences::colorSpaceFromInt;
using Ubiquitty::Preferences::colorSpaceFromString;
using Ubiquitty::Preferences::colorSpaceToInt;
using Ubiquitty::Preferences::colorSpaceToString;
using Ubiquitty::Preferences::CursorMotionMode;
using Ubiquitty::Preferences::CursorShape;
using Ubiquitty::Preferences::cursorShapeFromInt;
using Ubiquitty::Preferences::cursorShapeFromString;
using Ubiquitty::Preferences::cursorShapeToInt;
using Ubiquitty::Preferences::cursorShapeToString;
using Ubiquitty::Preferences::InGridBeam;
using Ubiquitty::Preferences::NativeBeam;
using Ubiquitty::Preferences::OptionAsAlternate;
using Ubiquitty::Preferences::optionAsAlternateFromString;
using Ubiquitty::Preferences::parse;
using Ubiquitty::Preferences::ParseMode;
using Ubiquitty::Preferences::resolveBeamSurface;
using Ubiquitty::Preferences::TabShortcutHints;
using Ubiquitty::Preferences::tabShortcutHintsFromInt;
using Ubiquitty::Preferences::tabShortcutHintsToInt;
using Ubiquitty::Preferences::TitlebarStyle;
using Ubiquitty::Preferences::titlebarStyleFromString;
using Ubiquitty::Preferences::Values;

static Values parseString(const std::string& text)
{
    Values c;
    std::istringstream in(text);
    parse(in, c);
    return c;
}

static bool parseStrict(const std::string& text, Values& configuration)
{
    std::istringstream input(text);
    return parse(input, configuration, ParseMode::Strict);
}

TEST(remoteHistoryPersistenceRequiresAnExplicitBooleanOptIn)
{
    using Ubiquitty::Preferences::Effective::shouldPersistRemoteHistory;
    CHECK(!shouldPersistRemoteHistory(Values {}));
    CHECK(shouldPersistRemoteHistory(parseString("[remote]\nhistory_cache_on_disk = true\n")));
    CHECK(!shouldPersistRemoteHistory(parseString("[remote]\nhistory_cache_on_disk = false\n")));
    CHECK(!shouldPersistRemoteHistory(parseString("[remote]\nhistory_cache_on_disk = \"true\"\n")));
}

// `[updates] check_automatically`, read by its address the way a frontend's
// toggle and the updater reach it: on until the file says otherwise, and a
// boolean only.
TEST(updateChecksAreScheduledUntilTheFileTurnsThemOff)
{
    using Ubiquitty::Preferences::effectiveValue;
    using Ubiquitty::Preferences::keyForAddress;

    Values off;
    CHECK_MSG(parseStrict("[updates]\ncheck_automatically = false\n", off), "false is a value the key reads");
    Values on;
    CHECK_MSG(parseStrict("[updates]\ncheck_automatically = true\n", on), "true is a value the key reads");
    // A word is not a boolean: Strict refuses it, and Permissive drops it and
    // leaves the default rather than reading it as either answer.
    Values word;
    CHECK_MSG(!parseStrict("[updates]\ncheck_automatically = \"never\"\n", word), "a string is refused");

    const auto key = keyForAddress("updates.check_automatically");
    REQUIRE_MSG(key.has_value(), "updates.check_automatically is a preference");
    const auto checksAutomatically = [&](const Values& values) {
        const auto value = effectiveValue(*key, values);
        return value.has_value() && value->boolean;
    };
    CHECK_MSG(checksAutomatically(Values {}), "an untouched file checks on the schedule");
    CHECK_MSG(!checksAutomatically(off), "false turns the schedule off");
    CHECK_MSG(checksAutomatically(on), "true keeps the schedule on");
    CHECK_MSG(checksAutomatically(parseString("[updates]\ncheck_automatically = \"never\"\n")), "a string leaves the default");
}

// An empty stream leaves every field at its default with is-set false.
// [terminal] encoding — input characterSet selection + Encoding resolution.
// [unicode] normalization — string token, default "none", resolved to a
// NormalizationForm by the Unicode helper the frontend calls.
// [security] clipboard_read — OSC 52 read gate.
// [security] per-OSC / per-DCS allowlist — raw list strings stored on
// the Values, parsed downstream by the engine.
// A full [underline] table: float multiplier, int offset, bool, float pad.
// The standalone [tek] color customization: mode int + beam/background hex,
// the Effective accessors (clamp + hex parse + opaque default fallback).
// A full [font] table: string, int, bool, float points.
// Float font size is accepted and stored as a double.
// [appearance]: theme name and padding are parsed for the frontend.
// titlebar_style maps strings to the enum; default is Themed, bad input unset.
// Themes are keyed by NAME only; an integer `theme` value is not honored.
// Cursor shape strings map to enum values; bad input leaves it unset.
// cursorShapeToInt is the inverse of cursorShapeFromInt: the Values index
// round-trips through the enum and back (0 = Block, 1 = Underline, 2 = Bar).
// color_space: token <-> enum <-> Values index all round-trip, "display-p3"
// (case-insensitive, whitespace-trimmed) reaches DisplayP3 and "p3" does not,
// the canonical token is "display-p3", and an unknown token /
// out-of-range index is rejected (nullopt) rather than silently coerced.
// tab show_shortcuts: the Values segmented index round-trips through the
// TabShortcutHints enum (0 = Off, 1 = Always, 2 = On-Command) and an
// out-of-range index is rejected.
// [bell] + [selection] bools.
// [mouse] wheel tuning: float speed, int lines, gridFloor, type-mismatch
// drop, and defaults when absent.
// [notifications] enabled toggle.
// Comments (full-line and inline) and blank lines are ignored; the
// value before an inline `#` is what lands.
// A `#` inside a quoted string is part of the string, not a comment.
// Unknown sections and keys are silently ignored; recognized keys in
// the same file still apply.
// bool casing is accepted in either case.
// Type mismatches on a recognized key are dropped, not misapplied.
// The first-run default body parses without a structural error and
// sets nothing (every key is commented out).
// A [[themes]] block populates one entry on Values::themes with the
// name, is_dark, the ANSI array, and foreground/background/cursor hex colors.
// parseHexColor handles 6- and 8-digit hex with case-insensitive digits;
// rejects malformed input.
/// writeKeyValue round-trip helpers + tests.
static std::string scratchPath(const char* tag)
{
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "/tmp/ubiquitty-test-config-%d-%s.toml", static_cast<int>(::getpid()), tag);
    return { buffer };
}

static void writeFile(const std::string& path, const std::string& body)
{
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

static std::string readFile(const std::string& path)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool contains(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

// Existing `key = value` line gets the value rewritten in place; the
// key and indent stay; unrelated lines/comments are preserved.
// A commented-out `# key = ...` line inside the section is uncommented
// and updated, indent preserved.
// Missing key is appended at the end of the section body, before any
// trailing blank lines separating the next section.
// Missing section is created at the end of the file.
// Round-trip: after each write, re-parse and verify the value lands in
// the same field the live menu would have set.
// A font size in quarter points survives the whole trip — schema clamp, the
// key's serializer, the file, the parser, the effective accessor — for
// font.size, font.proportional_size and font.line_spacing alike.
// TOML value formatters: ints, bools, doubles (decimal point ensured),
// strings (escaped).
// --- Block-level TOML mutation (Values panes) ---------------------

// tomlHexColor packs RRGGBBAA, dropping the alpha byte only when opaque.
// escapeTOMLBasicString escapes the whole set the reader decodes, not just
// the two characters that look structural.
// isBindingLine recognizes only a quoted-spec value line.
// renderThemeBlock emits a [[themes]] block whose colors + flag the parser
// reads back unchanged (round-trip through the splice + parse).
// spliceNamedBlock replaces an existing same-named block in place rather
// than appending a duplicate; unrelated blocks + comments are untouched.
// removeNamedBlock drops just the matching block; the sibling stays.
// removeSectionKey deletes one key from a flat section, keeping comments,
// the section header, and unrelated keys.
// rewriteKeysSection swaps the binding lines but keeps the leading comment
// inside [keys] and any unrelated section; a missing [keys] is created.
// New config fields parse correctly.
// background_opacity parses + clamps to [0.0, 1.0]; background_image is a
// plain string path. Absent keys stay unset (opaque, no image).
// [keys] entries land in keyBindings as raw spec/action strings, in file
// order; the quoted key-spec is unquoted so it can carry '+' and spaces.
// compose_key is a scalar option inside [keys], not a spec->action binding:
// it must land in Values::composeKey and not become a keyBinding. Real
// bindings in the same block still parse.
// [security] password_detect bool toggle.
// A [mac.keys] sub-table collects per-platform keybinding overrides on the
// mac slot, separate from the global [keys] table; resolveKeyBindings then
// merges them — a platform binding for an existing spec replaces it, a new
// spec is appended. Other platforms ignore the [mac.keys] block.
// A bare [mac] block sets Profile fields without disturbing the global
// config; the platform sub-section for keys and the bare block coexist.
// [link_handlers] entries land in linkHandlers as raw scheme->command
// strings, in file order; a bare scheme token and a quoted one both work, and
// the command template (incl. a "%s" URL placeholder) is stored verbatim.
// [triggers] entries land in `triggers` as raw regex(key)->action(value) pairs
// in file order; a quoted regex key (carrying spaces / brackets) is unquoted.
// No [triggers] block ⇒ the list is empty (nothing fires — the safe default).
// [characterClass] entries land in `characterClasses` as raw name(key)->regex(value)
// pairs in file order. No [characterClass] block ⇒ empty (word selection unchanged).
// [macros] entries land in `macros` as raw name(key)->body(value) pairs in file
// order; a quoted name is unquoted. The body is stored raw (escapes decoded at
// invoke time). No [macros] block ⇒ empty.
// [scripting] config_script + enable: the script path is a string key; enable
// is a bool gate (default closed). The config_script hook is the minimal
// Lua/Python scripting extension.
// parseScriptingRules turns a script's stdout into trigger/macro/charclass
// directives, splitting key/value on the first '=', ignoring blanks + comments
// + unknown directives. This is what a Lua/Python config_script emits.
/// [triggers] allow_webhook is a SECURITY GATE: parsed into the raw bool +
/// is-set flag (the Effective:: gate is verified in test_preferences_defaults).
/// False/unset by default; mirrors allow_run_command.
TEST(scrollBounceParsesBothChoices)
{
    using Ubiquitty::Preferences::Effective::scrollBounce;
    CHECK(scrollBounce(parseString("")));
    CHECK(!scrollBounce(parseString("[mouse]\nscroll_bounce = false\n")));
    CHECK(scrollBounce(parseString("[mouse]\nscroll_bounce = true\n")));
}

TEST(testDefaultsWhenEmpty)
{
    // An empty file sets nothing, and the default a reader sees comes from the
    // accessor. Two claims, asserted separately: that the parse wrote no field,
    // and that each accessor answers its documented default over an unset one.
    Values c = parseString("");
    CHECK(!c.fontFamily.has_value());
    CHECK(!c.fontSize.has_value());
    CHECK(!c.themeName.has_value());
    CHECK(!c.cursorShape.has_value());
    CHECK(!c.bellAudible.has_value());
    CHECK(!c.underlineThickness.has_value());
    CHECK(!c.underlineSkipInk.has_value());
    CHECK(!c.notifications.has_value());
    CHECK(!c.notificationSound.has_value());
    CHECK(!c.clipboardRead.has_value());
    CHECK(!c.encoding.has_value());
    CHECK(!c.scrollbackLines.has_value());

    CHECK_EQ(c.fontSize.value_or(Ubiquitty::Preferences::Defaults::fontSize), 12.0);
    CHECK(c.ligatures.value_or(Ubiquitty::Preferences::Defaults::ligatures) == true);
    CHECK_EQ(c.lineSpacingPoints.value_or(Ubiquitty::Preferences::Defaults::lineSpacing), 2.0);
    CHECK_EQ(c.padding.value_or(Ubiquitty::Preferences::Defaults::padding), 6.0);
    CHECK(c.cursorShape.value_or(Ubiquitty::Preferences::Defaults::cursorShape) == CursorShape::Bar);
    CHECK(c.bellAudible.value_or(Ubiquitty::Preferences::Defaults::bellAudible) == true);
    CHECK(c.bellVisual.value_or(Ubiquitty::Preferences::Defaults::bellVisual) == false);
    CHECK_EQ(c.underlineThickness.value_or(Ubiquitty::Preferences::Defaults::underlineThickness), 1.0);
    CHECK(c.underlineSkipInk.value_or(Ubiquitty::Preferences::Defaults::underlineSkipInk) == true);
    CHECK_EQ(c.underlineSkipInkPad.value_or(Ubiquitty::Preferences::Defaults::underlineSkipInkPad), 1.75);
    CHECK(c.notifications.value_or(Ubiquitty::Preferences::Defaults::notifications) == true); // OS notifications default on
    CHECK(c.notificationSound.value_or(Ubiquitty::Preferences::Defaults::notificationSound) == true); // banner sound default on
    CHECK(c.clipboardRead.value_or("prompt") == "prompt"); // OSC 52 read gate defaults to prompt
    CHECK(c.encoding.value_or("utf-8") == "utf-8"); // input characterSet defaults to UTF-8
    CHECK_EQ(c.scrollbackLines.value_or(Ubiquitty::Preferences::Defaults::scrollbackLines), 10000); // history depth defaults to 10000
}

TEST(testStrictParseRejectsFixtureMistakes)
{
    Values valid;
    CHECK(parseStrict("[font]\nfamily = \"JetBrains Mono\"\nsize = 12\n"
                      "[underline]\nthickness = 0.5\nskip_ink = true\n",
        valid));
    CHECK(valid.fontFamily.has_value() && valid.fontSize.has_value());
    CHECK(valid.underlineThickness.has_value() && valid.underlineSkipInk.has_value());

    Values unknown;
    CHECK(!parseStrict("[font]\nline_spcing = 4\n", unknown));
    CHECK(!unknown.lineSpacingPoints.has_value());

    Values wrongType;
    CHECK(!parseStrict("[appearance]\npadding_left = \"wide\"\n", wrongType));
    CHECK(!wrongType.paddingLeft.has_value());

    Values badEnum;
    CHECK(!parseStrict("[cursor]\nshape = \"triangle\"\n", badEnum));
    CHECK(!badEnum.cursorShape.has_value());

    Values permissive;
    std::istringstream input("[font]\nfuture_key = true\n");
    CHECK(parse(input, permissive));
}

TEST(testUnderlineSection)
{
    Values c = parseString("[underline]\n"
                           "thickness = 1.5\n"
                           "offset = 2\n"
                           "skip_ink = false\n"
                           "skip_ink_pad = 2.5\n");
    CHECK(c.underlineThickness == 1.5);
    CHECK(c.underlineOffset == 2.0);
    CHECK(c.underlineSkipInk == false);
    CHECK(c.underlineSkipInkPad == 2.5);
}

TEST(testTekColorSection)
{
    namespace E = Ubiquitty::Preferences::Effective;

    // Unset: theme mode, defaults resolve to green phosphor on near-black.
    Values empty;
    CHECK(!empty.tekColorMode.has_value());
    CHECK(E::tekColorMode(empty) == 0);
    CHECK(E::tekBeamColor(empty) == 0x33FF66u);
    CHECK(E::tekBackgroundColor(empty) == 0x0A0A0Au);

    // Custom mode + explicit hex (with and without '#').
    Values c = parseString("[tek]\n"
                           "color_mode = 1\n"
                           "beam_color = \"#ffaa00\"\n"
                           "background_color = \"101015\"\n");
    CHECK(c.tekColorMode == 1);
    CHECK(E::tekColorMode(c) == 1);
    CHECK(c.tekBeamColor == "#ffaa00");
    CHECK(E::tekBeamColor(c) == 0xFFAA00u);
    CHECK(c.tekBackgroundColor == "101015");
    CHECK(E::tekBackgroundColor(c) == 0x101015u);

    // color_mode is clamped to [0, 1] (the parse table bounds reject 7 -> drops).
    Values oob = parseString("[tek]\n"
                             "color_mode = 7\n");
    CHECK(E::tekColorMode(oob) >= 0 && E::tekColorMode(oob) <= 1);

    // Malformed hex falls back to the default color, not garbage.
    Values bad = parseString("[tek]\n"
                             "beam_color = \"nothex\"\n");
    CHECK(E::tekBeamColor(bad) == 0x33FF66u);
}

TEST(testFontSection)
{
    Values c = parseString("[font]\n"
                           "family = \"JetBrains Mono\"\n"
                           "symbol_fallback = \"Symbols Nerd Font Mono\"\n"
                           "emoji_fallback = \"Apple Color Emoji\"\n"
                           "size = 16\n"
                           "proportional_family = \"Avenir Next\"\n"
                           "proportional_size = 13\n"
                           "ligatures = false\n"
                           "line_spacing = 2.5\n"
                           "wide_glyph_boost = true\n"
                           "wide_glyph_scale = 1.06\n");
    CHECK(c.fontFamily == "JetBrains Mono");
    CHECK(c.proportionalFontFamily == "Avenir Next");
    CHECK(c.proportionalFontSize == 13.0);
    CHECK(c.symbolFallbackFont == "Symbols Nerd Font Mono");
    CHECK(c.emojiFallbackFont == "Apple Color Emoji");
    CHECK(c.fontSize == 16.0);
    CHECK(c.ligatures == false);
    CHECK(c.lineSpacingPoints == 2.5);
    CHECK(c.wideGlyphBoost == true);
    CHECK(c.wideGlyphScale == 1.06);
}

TEST(testFontLanguageFallbacksAndRangeMap)
{
    Values c = parseString("[font]\n"
                           "cjk_fallback = \"ja\"\n"
                           "[[font.language_fallbacks]]\n"
                           "script = \"han\"\n"
                           "language = \"ja\"\n"
                           "mode = \"custom\"\n"
                           "family = \"Hiragino Sans\"\n"
                           "[[font.language_fallbacks]]\n"
                           "script = \"arabic\"\n"
                           "language = \"fa\"\n"
                           "mode = \"regional\"\n"
                           "[[font.range_map]]\n"
                           "range = \"U+4E00-U+9FFF\"\n"
                           "mode = \"custom\"\n"
                           "family = \"Hiragino Sans\"\n");
    CHECK(c.cjkFallbackFont == "ja");
    CHECK(c.languageFallbacks.size() == 2);
    CHECK(c.languageFallbacks[0].script == "han");
    CHECK(c.languageFallbacks[0].language == "ja");
    CHECK(c.languageFallbacks[0].mode == "custom");
    CHECK(c.languageFallbacks[0].family == "Hiragino Sans");
    CHECK(c.languageFallbacks[1].script == "arabic");
    CHECK(c.languageFallbacks[1].language == "fa");
    CHECK(c.languageFallbacks[1].mode == "regional");
    CHECK(c.rangeMap.size() == 1);
    CHECK(c.rangeMap[0].range == "U+4E00-U+9FFF");
    CHECK(c.rangeMap[0].firstCodePoint == 0x4E00);
    CHECK(c.rangeMap[0].lastCodePoint == 0x9FFF);
    CHECK(c.rangeMap[0].mode == "custom");
    CHECK(c.rangeMap[0].family == "Hiragino Sans");
}

TEST(testInvalidFontFallbackTablesAreDropped)
{
    Values c = parseString("[[font.language_fallbacks]]\n"
                           "script = \"notascript\"\n"
                           "language = \"ja\"\n"
                           "mode = \"custom\"\n"
                           "family = \"Hiragino Sans\"\n"
                           "[[font.language_fallbacks]]\n"
                           "script = \"han\"\n"
                           "language = \"ja\"\n"
                           "mode = \"custom\"\n"
                           "[[font.range_map]]\n"
                           "range = \"garbage\"\n"
                           "mode = \"custom\"\n"
                           "family = \"Hiragino Sans\"\n"
                           "[[font.range_map]]\n"
                           "range = \"U+4E00-U+9FFF\"\n"
                           "mode = \"custom\"\n");
    CHECK(c.languageFallbacks.empty());
    CHECK(c.rangeMap.empty());
}

TEST(testFloatSize)
{
    Values c = parseString("[font]\nsize = 13.5\n");
    CHECK(c.fontSize == 13.5);
}

TEST(testAppearanceSection)
{
    Values c = parseString("[appearance]\n"
                           "theme = \"One Dark\"\n"
                           "color_space = \"display-p3\"\n"
                           "padding = 8\n"
                           "folder_icon = false\n"
                           "process_icons = false\n");
    CHECK(c.themeName == "One Dark");
    CHECK(c.colorSpace == "display-p3");
    CHECK(c.padding == 8.0);
    CHECK(c.folderIcon == false);

    Values def;
    CHECK(!def.folderIcon.has_value());
    CHECK(def.folderIcon.value_or(true) == true); // default on when unset
    CHECK(c.processIcons == false);
    CHECK(!def.processIcons.has_value());
    CHECK(Ubiquitty::Preferences::Effective::processIcons(def));
    CHECK(!Ubiquitty::Preferences::Effective::processIcons(c));
}

TEST(testTitlebarStyle)
{
    Values def;
    CHECK(!def.titlebarStyle.has_value());
    CHECK(def.titlebarStyle.value_or(TitlebarStyle::Themed) == TitlebarStyle::Themed); // default when unset

    Values c = parseString("[appearance]\ntitlebar_style = \"native\"\n");
    CHECK(c.titlebarStyle == TitlebarStyle::Native);

    Values themed = parseString("[appearance]\ntitlebar_style = \"themed\"\n");
    CHECK(themed.titlebarStyle == TitlebarStyle::Themed);

    Values bad = parseString("[appearance]\ntitlebar_style = \"glass\"\n");
    CHECK(!bad.titlebarStyle.has_value());

    CHECK(titlebarStyleFromString("THEMED") == TitlebarStyle::Themed);
    CHECK(titlebarStyleFromString(" native ") == TitlebarStyle::Native);
    CHECK(titlebarStyleFromString("nope") == std::nullopt);
}

// beam_surface picks where the command palette is drawn. The resolver's job is
// the interesting half: it must never answer "native" to a frontend that has no
// native palette, INCLUDING when the config asked for one, because a keystroke
// that opens nothing reads as a broken binding rather than as a setting.
TEST(testBeamSurface)
{
    Values def;
    CHECK(!def.beamSurface.has_value());
    CHECK(def.beamSurface.value_or(BeamSurface::Auto) == BeamSurface::Auto); // default when unset

    Values native = parseString("[appearance]\nbeam_surface = \"native\"\n");
    CHECK(native.beamSurface == BeamSurface::Native);

    Values inTerminal = parseString("[appearance]\nbeam_surface = \"in-terminal\"\n");
    CHECK(inTerminal.beamSurface == BeamSurface::InTerminal);

    // Three spellings for the in-grid palette, because the hyphen matches the
    // neighbouring tokens and the underscore matches every KEY in the file.
    for (const char* token : { "in-terminal", "in_terminal", "terminal", "IN-TERMINAL" })
        CHECK_MSG(beamSurfaceFromString(token) == BeamSurface::InTerminal, token);

    Values bad = parseString("[appearance]\nbeam_surface = \"popover\"\n");
    CHECK(!bad.beamSurface.has_value());
    CHECK(beamSurfaceFromString("popover") == std::nullopt);

    // Round-trips through its own token.
    for (const BeamSurface surface : { BeamSurface::Auto, BeamSurface::Native, BeamSurface::InTerminal })
        CHECK(beamSurfaceFromString(beamSurfaceToString(surface)) == surface);

    // The browser's shape: no native palette, in-grid always drawable. Every
    // setting resolves to in-grid, including an explicit "native".
    for (const BeamSurface surface : { BeamSurface::Auto, BeamSurface::Native, BeamSurface::InTerminal })
        CHECK_MSG(resolveBeamSurface(surface, NativeBeam::Unavailable, InGridBeam::Available)
                == BeamPresentation::InTerminal,
            "no native palette means in-grid, whatever was asked for");

    // With both, the preference decides — and "in-terminal" is honoured rather
    // than overridden, which is the other half of the user's choice.
    CHECK(resolveBeamSurface(BeamSurface::Auto, NativeBeam::Available, InGridBeam::Available) == BeamPresentation::Native);
    CHECK(resolveBeamSurface(BeamSurface::Native, NativeBeam::Available, InGridBeam::Available) == BeamPresentation::Native);
    CHECK(resolveBeamSurface(BeamSurface::InTerminal, NativeBeam::Available, InGridBeam::Available) == BeamPresentation::InTerminal);

    // macOS's shape: a native palette exists, the renderer cannot composite
    // chrome into the grid, and the user asked for in-terminal. Falling back to
    // native is the only answer that opens anything; in-terminal would draw
    // nothing at all on macOS.
    CHECK_MSG(resolveBeamSurface(BeamSurface::InTerminal, NativeBeam::Available, InGridBeam::Unavailable)
            == BeamPresentation::Native,
        "an in-terminal request falls back to native when the renderer cannot draw one");
    CHECK(resolveBeamSurface(BeamSurface::Auto, NativeBeam::Available, InGridBeam::Unavailable) == BeamPresentation::Native);

    // Neither available is a caller that should not be offering the command.
    // It still answers, and answers in-grid: a caller that asks anyway draws
    // into its own buffer rather than into a toolkit it does not have.
    for (const BeamSurface surface : { BeamSurface::Auto, BeamSurface::Native, BeamSurface::InTerminal })
        CHECK_MSG(resolveBeamSurface(surface, NativeBeam::Unavailable, InGridBeam::Unavailable)
                == BeamPresentation::InTerminal,
            "with neither surface, the answer is the one a caller can fake");
}

TEST(testThemeNonStringIgnored)
{
    Values c = parseString("[appearance]\ntheme = 3\n");
    CHECK(!c.themeName.has_value());
}

TEST(testCursorSection)
{
    Values c = parseString("[cursor]\nshape = \"underline\"\nblink = false\nlinger = 70\nblink_rate = 500\n");
    CHECK(c.cursorShape == CursorShape::Underline);
    CHECK(c.cursorBlink == false);
    CHECK(c.cursorLinger == 70);
    CHECK(c.cursorBlinkRate == 500);

    Values bad = parseString("[cursor]\nshape = \"diamond\"\n");
    CHECK(!bad.cursorShape.has_value());

    CHECK(!c.cursorInvertText.has_value()); // absent → unset (default)
    Values inv = parseString("[cursor]\ninvert_text = true\n");
    CHECK(inv.cursorInvertText == true);

    // [cursor] motion — off | short | long | always (case-insensitive), the
    // distance-based glide modes, default Off when unset; motion_duration is the
    // clamped slide time.
    CHECK(!c.cursorMotion.has_value());
    CHECK(c.cursorMotion.value_or(CursorMotionMode::Off) == CursorMotionMode::Off); // compiled default

    Values off = parseString("[cursor]\nmotion = \"off\"\n");
    CHECK(off.cursorMotion == CursorMotionMode::Off);
    Values shortMode = parseString("[cursor]\nmotion = \"short\"\n");
    CHECK(shortMode.cursorMotion == CursorMotionMode::Short);
    Values longMode = parseString("[cursor]\nmotion = \"LONG\"\n");
    CHECK(longMode.cursorMotion == CursorMotionMode::Long);
    Values always = parseString("[cursor]\nmotion = \"ALWAYS\"\n");
    CHECK(always.cursorMotion == CursorMotionMode::Always);
    Values bogus = parseString("[cursor]\nmotion = \"program\"\n"); // an unrecognized token leaves the default
    CHECK(bogus.cursorMotion.value_or(CursorMotionMode::Off) == CursorMotionMode::Off);

    Values dur = parseString("[cursor]\nmotion_duration = 250\n");
    CHECK(dur.cursorMotionDuration == 250);
    Values durHi = parseString("[cursor]\nmotion_duration = 9000\n");
    CHECK(durHi.cursorMotionDuration == 1000); // clamped to the 1..1000 ceiling
}

TEST(testCursorShapeStrings)
{
    CHECK(cursorShapeFromString("block") == CursorShape::Block);
    CHECK(cursorShapeFromString("UNDERLINE") == CursorShape::Underline);
    CHECK(cursorShapeFromString(" bar ") == CursorShape::Bar);
    CHECK(cursorShapeFromString("nope") == std::nullopt);
}

TEST(testCursorShapeIntRoundtrip)
{
    for (int i = 0; i < 3; ++i) {
        auto shape = cursorShapeFromInt(i);
        CHECK(shape.has_value());
        CHECK(cursorShapeToInt(*shape) == i);
    }
    CHECK(cursorShapeFromInt(-1) == std::nullopt);
    CHECK(cursorShapeFromInt(3) == std::nullopt);
    CHECK(cursorShapeToInt(CursorShape::Block) == 0);
    CHECK(cursorShapeToInt(CursorShape::Underline) == 1);
    CHECK(cursorShapeToInt(CursorShape::Bar) == 2);
}

TEST(testColorSpaceMappings)
{
    // string -> enum, incl. casing + surrounding whitespace; no alias
    CHECK(colorSpaceFromString("srgb") == ColorSpace::SRGB);
    CHECK(colorSpaceFromString("sRGB") == ColorSpace::SRGB);
    CHECK(colorSpaceFromString("display-p3") == ColorSpace::DisplayP3);
    CHECK(colorSpaceFromString(" Display-P3 ") == ColorSpace::DisplayP3);
    CHECK(colorSpaceFromString("p3") == std::nullopt);
    CHECK(colorSpaceFromString("P3") == std::nullopt);
    CHECK(colorSpaceFromString("rec2020") == std::nullopt);
    CHECK(colorSpaceFromString("") == std::nullopt);

    // enum -> canonical token (the form the writer persists)
    CHECK(std::string(colorSpaceToString(ColorSpace::SRGB)) == "srgb");
    CHECK(std::string(colorSpaceToString(ColorSpace::DisplayP3)) == "display-p3");
    // token written by the writer parses back to the same enum
    CHECK(colorSpaceFromString(colorSpaceToString(ColorSpace::SRGB)) == ColorSpace::SRGB);
    CHECK(colorSpaceFromString(colorSpaceToString(ColorSpace::DisplayP3)) == ColorSpace::DisplayP3);

    // Values segmented-control index <-> enum (0 = sRGB, 1 = CellGrid P3)
    CHECK(colorSpaceFromInt(0) == ColorSpace::SRGB);
    CHECK(colorSpaceFromInt(1) == ColorSpace::DisplayP3);
    CHECK(colorSpaceFromInt(2) == std::nullopt);
    CHECK(colorSpaceFromInt(-1) == std::nullopt);
    CHECK(colorSpaceToInt(ColorSpace::SRGB) == 0);
    CHECK(colorSpaceToInt(ColorSpace::DisplayP3) == 1);
}

TEST(testTabShortcutHintsIndex)
{
    CHECK(tabShortcutHintsFromInt(0) == TabShortcutHints::Off);
    CHECK(tabShortcutHintsFromInt(1) == TabShortcutHints::Always);
    CHECK(tabShortcutHintsFromInt(2) == TabShortcutHints::OnCommand);
    CHECK(tabShortcutHintsFromInt(3) == std::nullopt);
    CHECK(tabShortcutHintsFromInt(-1) == std::nullopt);
    CHECK(tabShortcutHintsToInt(TabShortcutHints::Off) == 0);
    CHECK(tabShortcutHintsToInt(TabShortcutHints::Always) == 1);
    CHECK(tabShortcutHintsToInt(TabShortcutHints::OnCommand) == 2);
    for (int i = 0; i < 3; ++i) {
        auto h = tabShortcutHintsFromInt(i);
        CHECK(h.has_value() && tabShortcutHintsToInt(*h) == i);
    }
}

TEST(testBellAndSelection)
{
    Values c = parseString("[bell]\naudible = false\nvisual = true\n"
                           "[selection]\ncopy_on_select = true\n");
    CHECK(c.bellAudible == false);
    CHECK(c.bellVisual == true);
    CHECK(c.copyOnSelect == true);
}

TEST(testMouseSection)
{
    Values c = parseString("[mouse]\nwheel_speed = 2.5\nwheel_lines = 3\n");
    CHECK(c.wheelSpeed == 2.5);
    CHECK(c.wheelLines == 3);

    // An int wheel_speed is accepted as a float multiplier.
    Values ci = parseString("[mouse]\nwheel_speed = 2\n");
    CHECK(ci.wheelSpeed == 2.0);

    // Clamped to the usable range.
    Values hi = parseString("[mouse]\nwheel_speed = 50\nwheel_lines = 99\n");
    CHECK(hi.wheelSpeed == 10.0);
    CHECK(hi.wheelLines == 20);
    Values lo = parseString("[mouse]\nwheel_speed = 0.0\nwheel_lines = 0\n");
    CHECK(lo.wheelSpeed == 0.1);
    CHECK(lo.wheelLines == 1);

    // wheel_lines is int-typed; a float value is dropped.
    Values bad = parseString("[mouse]\nwheel_lines = 2.5\n");
    CHECK(!bad.wheelLines.has_value());

    // Absent → unset, and the default a reader sees comes from the accessor.
    Values def = parseString("[font]\nsize = 14\n");
    CHECK(!def.wheelSpeed.has_value());
    CHECK(!def.wheelLines.has_value());
}

TEST(testNotificationsSection)
{
    Values off = parseString("[notifications]\nenabled = false\n");
    CHECK(off.notifications == false);

    Values on = parseString("[notifications]\nenabled = true\n");
    CHECK(on.notifications == true);

    Values muted = parseString("[notifications]\nsound = false\n");
    CHECK(muted.notificationSound == false);

    Values sounded = parseString("[notifications]\nsound = true\n");
    CHECK(sounded.notificationSound == true);
}

TEST(testSecurityClipboardRead)
{
    Values prompt = parseString("[security]\nclipboard_read = \"prompt\"\n");
    CHECK(prompt.clipboardRead == "prompt");

    Values allow = parseString("[security]\nclipboard_read = \"allow\"\n");
    CHECK(allow.clipboardRead == "allow");

    Values deny = parseString("[security]\nclipboard_read = \"deny\"\n");
    CHECK(deny.clipboardRead == "deny");
}

TEST(testSecuritySequenceAllowlist)
{
    Values def = parseString("");
    // No [security] block sets none of the six, which is what the gate reads
    // as "no allowlist stated" rather than as an empty one.
    CHECK(!def.oscAllow.has_value());
    CHECK(!def.oscDeny.has_value());
    CHECK(!def.dcsAllow.has_value());
    CHECK(!def.dcsDeny.has_value());
    CHECK(!def.apcAllow.has_value());
    CHECK(!def.apcDeny.has_value());

    Values c = parseString("[security]\n"
                           "osc_allow = \"0 2 8\"\n"
                           "osc_deny = \"52 1337\"\n"
                           "dcs_allow = \"sixel decrqss\"\n"
                           "dcs_deny = \"decudk\"\n"
                           "apc_allow = \"G X\"\n"
                           "apc_deny = \"X\"\n");
    CHECK(c.oscAllow == "0 2 8");
    CHECK(c.oscDeny == "52 1337");
    CHECK(c.dcsAllow == "sixel decrqss");
    CHECK(c.dcsDeny == "decudk");
    CHECK(c.apcAllow == "G X");
    CHECK(c.apcDeny == "X");
}

TEST(testTerminalEncoding)
{
    Values def = parseString("[terminal]\nencoding = \"utf-8\"\n");
    CHECK(def.encoding == "utf-8");

    Values sjis = parseString("[terminal]\nencoding = \"shift_jis\"\n");
    CHECK(sjis.encoding == "shift_jis");

    Values koi = parseString("[terminal]\nencoding = \"KOI8-R\"\n");
    CHECK(koi.encoding == "KOI8-R");

    // A non-string value is dropped; the default stays.
    Values bad = parseString("[terminal]\nencoding = 7\n");
    CHECK(bad.encoding.value_or("utf-8") == "utf-8");

    // scrollback_lines — int history depth, clamped to [1, 1000000].
    Values sb = parseString("[terminal]\nscrollback_lines = 5000\n");
    CHECK(sb.scrollbackLines == 5000);
    Values sbHi = parseString("[terminal]\nscrollback_lines = 9999999\n");
    CHECK(sbHi.scrollbackLines == 1000000);
    Values sbLo = parseString("[terminal]\nscrollback_lines = 0\n");
    CHECK(sbLo.scrollbackLines == 1);
    Values sbBad = parseString("[terminal]\nscrollback_lines = \"lots\"\n");
    CHECK(sbBad.scrollbackLines.value_or(10000) == 10000);

    // The stored string resolves to an Encoding via the Unicode helper.
    auto eSjis = Ubiquitty::Unicode::encodingFromName(sjis.encoding->c_str());
    CHECK(eSjis && *eSjis == Ubiquitty::Unicode::Encoding::ShiftJis);
    auto eKoi = Ubiquitty::Unicode::encodingFromName(koi.encoding->c_str());
    CHECK(eKoi && *eKoi == Ubiquitty::Unicode::Encoding::Koi8R);
    auto eDef = Ubiquitty::Unicode::encodingFromName(def.encoding->c_str());
    CHECK(eDef && *eDef == Ubiquitty::Unicode::Encoding::UTF8);
}

TEST(testUnicodeNormalization)
{
    Values def;
    CHECK(def.normalization.value_or("none") == "none");

    Values nfc = parseString("[unicode]\nnormalization = \"nfc\"\n");
    CHECK(nfc.normalization == "nfc");

    Values nfkd = parseString("[unicode]\nnormalization = \"NFKD\"\n");
    CHECK(nfkd.normalization == "NFKD");

    // A non-string value is dropped; the default stays.
    Values bad = parseString("[unicode]\nnormalization = 7\n");
    CHECK(bad.normalization.value_or("none") == "none");

    // The stored string resolves to a form via the Unicode helper.
    CHECK(Ubiquitty::Unicode::normalizationFormFromName(nfc.normalization->c_str()) == Ubiquitty::Unicode::NormalizationForm::NFC);
    CHECK(Ubiquitty::Unicode::normalizationFormFromName(nfkd.normalization->c_str()) == Ubiquitty::Unicode::NormalizationForm::NFKD);
    CHECK(!def.normalization.has_value()); // no key, so no form to name — the default stands
}

TEST(testCommentsAndBlanks)
{
    Values c = parseString("# a leading comment\n"
                           "\n"
                           "[font]\n"
                           "size = 18   # inline comment after value\n"
                           "# ligatures = true (commented out, must not apply)\n");
    CHECK(c.fontSize == 18.0);
    CHECK(!c.ligatures.has_value());
}

TEST(testHashInString)
{
    Values c = parseString("[appearance]\ntheme = \"Color #3\"\n");
    CHECK(c.themeName == "Color #3");
}

TEST(testUnknownKeysIgnored)
{
    Values c = parseString("[future]\nwidgets = 42\n"
                           "[font]\nsize = 20\nunknown_key = \"x\"\n");
    CHECK(c.fontSize == 20.0);
}

TEST(testBoolCasing)
{
    Values c = parseString("[font]\nligatures = TRUE\n");
    CHECK(c.ligatures == true);
}

TEST(testTypeMismatchDropped)
{
    Values c = parseString("[font]\nsize = \"big\"\nligatures = 1\n");
    CHECK(!c.fontSize.has_value()); // string where a number is wanted
    CHECK(!c.ligatures.has_value()); // int where a bool is wanted
}

TEST(testDefaultFileParsesClean)
{
    std::string body = Ubiquitty::Preferences::defaultFileContents();
    std::istringstream in(body);
    Values c;
    CHECK(parse(in, c) == true);
    CHECK(!c.fontFamily.has_value());
    CHECK(!c.themeName.has_value());
    CHECK(!c.cursorShape.has_value());
}

TEST(testThemesArrayOfTables)
{
    using Ubiquitty::Preferences::ColorEntry;
    using Ubiquitty::Preferences::parseHexColor;
    Values c = parseString("[[themes]]\n"
                           "name = \"My Theme\"\n"
                           "is_dark = true\n"
                           "ansi = [\n"
                           "  \"#1d1f21\", \"#cc6666\", \"#b5bd68\", \"#f0c674\",\n"
                           "  \"#81a2be\", \"#b294bb\", \"#8abeb7\", \"#c5c8c6\",\n"
                           "  \"#666666\", \"#d54e53\", \"#b9ca4a\", \"#e7c547\",\n"
                           "  \"#7aa6da\", \"#c397d8\", \"#70c0b1\", \"#eaeaea\"\n"
                           "]\n"
                           "fg = \"#ffffff\"\n"
                           "bg = \"#282c34\"\n"
                           "cursor = \"#ffffffaa\"\n");
    CHECK(c.themes.size() == 1);
    if (c.themes.empty())
        return;
    const auto& t = c.themes[0];
    CHECK(t.name == "My Theme");
    CHECK(t.isDark.value_or(false));
    CHECK(t.ansi[0].set);
    CHECK(t.ansi[0].red == 0x1d && t.ansi[0].green == 0x1f && t.ansi[0].blue == 0x21);
    CHECK(t.ansi[15].red == 0xea && t.ansi[15].green == 0xea && t.ansi[15].blue == 0xea);
    CHECK(t.foreground.set && t.foreground.red == 0xff);
    CHECK(t.background.set && t.background.red == 0x28 && t.background.green == 0x2c && t.background.blue == 0x34);
    CHECK(t.cursor.set && t.cursor.alpha == 0xaa);
}

TEST(testHexColorAlpha)
{
    using Ubiquitty::Preferences::ColorEntry;
    using Ubiquitty::Preferences::parseHexColor;
    ColorEntry c;
    CHECK(parseHexColor(std::string("#aabbcc"), c));
    CHECK(c.red == 0xaa && c.green == 0xbb && c.blue == 0xcc && c.alpha == 255);
    CHECK(parseHexColor(std::string("#AABBCC80"), c));
    CHECK(c.red == 0xaa && c.alpha == 0x80);
    CHECK(!parseHexColor(std::string("aabbcc"), c)); // missing #
    CHECK(!parseHexColor(std::string("#zz0011"), c)); // bad digit
    CHECK(!parseHexColor(std::string("#abc"), c)); // short form unsupported
}

TEST(testNewAppearanceAndCursorFields)
{
    // system_*_theme accept a NAME (the persisted form).
    Values c = parseString("[appearance]\n"
                           "follow_system = true\n"
                           "system_dark_theme = \"Ayu Mirage\"\n"
                           "system_light_theme = \"Ayu Light\"\n"
                           "[cursor]\n"
                           "alpha = 200\n");
    CHECK(c.followSystemAppearance == true);
    CHECK(c.systemDarkThemeName == "Ayu Mirage");
    CHECK(c.systemLightThemeName == "Ayu Light");
    CHECK(c.cursorAlpha == 200);

    // Keyed by NAME only — a non-string system_*_theme value is not honored.
    Values ints = parseString("[appearance]\n"
                              "system_dark_theme = 2\n"
                              "system_light_theme = 1\n");
    CHECK(!ints.systemDarkThemeName.has_value() && !ints.systemLightThemeName.has_value());
}

TEST(testBackgroundOpacityAndImage)
{
    Values c = parseString("[appearance]\n"
                           "background_opacity = 0.8\n"
                           "background_image = \"/tmp/bg.png\"\n"
                           "background_image_opacity = 0.6\n"
                           "background_image_mode = \"center\"\n");
    CHECK(c.backgroundOpacity == 0.8);
    CHECK(c.backgroundImage == "/tmp/bg.png");
    CHECK(c.backgroundImageOpacity == 0.6);
    CHECK(c.backgroundImageMode == BackgroundImageMode::Center);

    Values lo = parseString("[appearance]\nbackground_opacity = 0.05\n");
    CHECK(lo.backgroundOpacity == 0.05);

    Values hi = parseString("[appearance]\nbackground_opacity = 2.0\n");
    CHECK(hi.backgroundOpacity == 1.0); // ceil

    // background_image_opacity clamps to [0, 1]; an unknown mode token is ignored.
    Values clampMode = parseString("[appearance]\nbackground_image_opacity = 1.7\nbackground_image_mode = \"bogus\"\n");
    CHECK(clampMode.backgroundImageOpacity == 1.0);
    CHECK(!clampMode.backgroundImageMode.has_value());

    Values none = parseString("[appearance]\n");
    CHECK(none.backgroundOpacity.value_or(1.0) == 1.0);
    CHECK(!none.backgroundImage.has_value());
    CHECK(!none.backgroundImageOpacity.has_value());
    CHECK(!none.backgroundImageMode.has_value());
}

TEST(testKeysSection)
{
    Values c = parseString("[keys]\n"
                           "\"cmd+shift+k\" = \"clear_scrollback\"\n"
                           "\"ctrl+a ctrl+x\" = \"send:\\\\x18\"\n"
                           "\"cmd+c\" = \"copy\"\n");
    CHECK(c.keyBindings.size() == 3);
    CHECK(c.keyBindings[0].spec == "cmd+shift+k");
    CHECK(c.keyBindings[0].action == "clear_scrollback");
    CHECK(c.keyBindings[1].spec == "ctrl+a ctrl+x"); // chord keeps its space
    CHECK(c.keyBindings[1].action == "send:\\x18"); // TOML \\\\ -> one backslash
    CHECK(c.keyBindings[2].spec == "cmd+c");
    CHECK(c.keyBindings[2].action == "copy");
}

TEST(testKeysComposeKeyIsScalar)
{
    Values c = parseString("[keys]\n"
                           "compose_key = \"menu\"\n"
                           "\"cmd+c\" = \"copy\"\n");
    CHECK(c.composeKey == "menu");
    CHECK(c.keyBindings.size() == 1); // compose_key did not become a binding
    CHECK(c.keyBindings[0].spec == "cmd+c");

    // Default: unset, empty.
    Values def = parseString("[keys]\n\"cmd+q\" = \"close\"\n");
    CHECK(!def.composeKey.has_value());
    CHECK(def.composeKey.value_or("").empty());
}

// [keys] option_as_alt is a scalar option like compose_key, not a binding, and
// it resolves to the setting the Option-as-Meta predicate takes. Unset means no
// Option key is Meta, so a layout's Option characters reach the input method.
TEST(testKeyboardAccessorySymbolsAreBoundedOrderedSubsets)
{
    const auto custom = parseString("[keys]\naccessory_symbols = \"[]/~\"\n\"cmd+c\" = \"copy\"\n");
    CHECK(custom.keyboardAccessorySymbols.value_or("") == "[]/~");
    CHECK(custom.keyBindings.size() == 1);
    const auto empty = parseString("[keys]\naccessory_symbols = \"\"\n");
    CHECK(empty.keyboardAccessorySymbols.has_value());
    CHECK(empty.keyboardAccessorySymbols->empty());
    for (const std::string& bad : std::vector<std::string> { "~~", "abc", "\\n", "☃", std::string(1000, '/') }) {
        Values configuration;
        CHECK(!parseStrict("[keys]\naccessory_symbols = " + Ubiquitty::Preferences::tomlString(bad) + "\n", configuration));
        CHECK(!configuration.keyboardAccessorySymbols.has_value());
        CHECK(configuration.keyBindings.empty());
    }
}

TEST(testKeysOptionAsAlternateParses)
{
    Values def = parseString("[keys]\n\"cmd+q\" = \"close\"\n");
    CHECK(!def.optionAsAlternate.has_value());
    CHECK(Ubiquitty::Preferences::Effective::optionAsAlternate(def) == OptionAsAlternate::None);

    struct Case {
        const char* token;
        OptionAsAlternate expected;
    };
    constexpr Case cases[] = {
        { "none", OptionAsAlternate::None },
        { "left", OptionAsAlternate::Left },
        { "right", OptionAsAlternate::Right },
        { "both", OptionAsAlternate::Both },
        { "BOTH", OptionAsAlternate::Both },
        { " Left ", OptionAsAlternate::Left },
    };
    for (const Case& c : cases) {
        const std::string text = std::string("[keys]\noption_as_alt = \"") + c.token + "\"\n\"cmd+c\" = \"copy\"\n";
        Values parsed;
        CHECK_MSG(parseStrict(text, parsed), c.token);
        CHECK_MSG(parsed.optionAsAlternate == c.expected, c.token);
        CHECK_MSG(Ubiquitty::Preferences::Effective::optionAsAlternate(parsed) == c.expected, c.token);
        // The option did not become a binding; the real binding beside it did.
        CHECK_MSG(parsed.keyBindings.size() == 1, c.token);
        CHECK_MSG(parsed.keyBindings[0].spec == "cmd+c", c.token);
        CHECK_MSG(parsed.skippedKeys.empty(), c.token);
    }
}

// A value outside the four is refused, reported with its line, and leaves the
// default standing: a typo must not make an Option key Meta, and it must not
// land as a binding named "option_as_alt" either.
TEST(testKeysOptionAsAlternateRejectsAnInvalidValue)
{
    Values bad = parseString("[keys]\noption_as_alt = \"sideways\"\n");
    CHECK(!bad.optionAsAlternate.has_value());
    CHECK(Ubiquitty::Preferences::Effective::optionAsAlternate(bad) == OptionAsAlternate::None);
    CHECK(bad.keyBindings.empty());
    CHECK_EQ(static_cast<int>(bad.skippedKeys.size()), 1);
    if (!bad.skippedKeys.empty()) {
        CHECK(bad.skippedKeys[0].section == "keys");
        CHECK(bad.skippedKeys[0].key == "option_as_alt");
        CHECK(bad.skippedKeys[0].line == 2);
        CHECK(bad.skippedKeys[0].reason == Ubiquitty::Preferences::SkipReason::WrongValueKind);
    }
    Values strict;
    CHECK(!parseStrict("[keys]\noption_as_alt = \"sideways\"\n", strict));

    // Not a string at all: a bool is not one of the four either.
    Values boolean = parseString("[keys]\noption_as_alt = true\n");
    CHECK(!boolean.optionAsAlternate.has_value());
    CHECK_EQ(static_cast<int>(boolean.skippedKeys.size()), 1);

    CHECK(optionAsAlternateFromString("both") == OptionAsAlternate::Both);
    CHECK(optionAsAlternateFromString("meta") == std::nullopt);
    CHECK(optionAsAlternateFromString("") == std::nullopt);
}

// [capabilities.<origin>] overrides one configurable tier per key, for each of
// the three origins, with the three verdict words in any case.
TEST(testCapabilitiesSectionsParseEveryOriginAndTier)
{
    using Ubiquitty::CapabilityDecision;
    using Ubiquitty::CapabilityOrigin;
    using Ubiquitty::CapabilityTier;
    using Ubiquitty::Preferences::Effective::capabilityDecision;

    Values parsed;
    CHECK(parseStrict("[capabilities.remote]\n"
                      "informational = \"deny\"\n"
                      "host_escaping = \"Prompt\"\n"
                      "read_exfiltration = \"allow\"\n"
                      "[capabilities.agent]\n"
                      "host_escaping = \"deny\"\n"
                      "[capabilities.local]\n"
                      "informational = \"PROMPT\"\n"
                      "read_exfiltration = \"deny\"\n",
        parsed));
    CHECK(parsed.skippedKeys.empty());
    CHECK(parsed.remoteCapabilities.informational == CapabilityDecision::Deny);
    CHECK(parsed.remoteCapabilities.hostEscaping == CapabilityDecision::Prompt);
    CHECK(parsed.remoteCapabilities.readExfiltration == CapabilityDecision::Allow);
    CHECK(parsed.agentCapabilities.hostEscaping == CapabilityDecision::Deny);
    CHECK(!parsed.agentCapabilities.informational.has_value());
    CHECK(parsed.localCapabilities.informational == CapabilityDecision::Prompt);
    CHECK(parsed.localCapabilities.readExfiltration == CapabilityDecision::Deny);

    // The accessor answers the file where it spoke and the default elsewhere.
    CHECK(capabilityDecision(parsed, CapabilityOrigin::Agent, CapabilityTier::HostEscaping) == CapabilityDecision::Deny);
    CHECK(capabilityDecision(parsed, CapabilityOrigin::Agent, CapabilityTier::ReadExfil) == CapabilityDecision::Prompt);
    CHECK(capabilityDecision(parsed, CapabilityOrigin::Local, CapabilityTier::HostEscaping) == CapabilityDecision::Allow);

    // Unset, the accessor is the per-origin default — remote informational Allow.
    const Values untouched;
    CHECK(capabilityDecision(untouched, CapabilityOrigin::Remote, CapabilityTier::LowRiskInfo) == CapabilityDecision::Allow);
    CHECK(capabilityDecision(untouched, CapabilityOrigin::Remote, CapabilityTier::HostEscaping) == CapabilityDecision::Deny);
    CHECK(capabilityDecision(untouched, CapabilityOrigin::Remote, CapabilityTier::ReadExfil) == CapabilityDecision::Deny);
}

// The capability tables keep their dotted name: `[capabilities.remote]` is not
// the `[remote]` section, and `[remote]` takes no capability key.
TEST(testCapabilitiesSectionIsNotTheRemoteSection)
{
    Values nested;
    CHECK(parseStrict("[capabilities.remote]\ninformational = \"deny\"\n", nested));
    CHECK(nested.remoteCapabilities.informational == Ubiquitty::CapabilityDecision::Deny);

    Values flat;
    CHECK(!parseStrict("[remote]\ninformational = \"deny\"\n", flat));
    CHECK(!flat.remoteCapabilities.informational.has_value());

    // And `[remote]`'s own key does not land under the capability table.
    Values misplaced;
    CHECK(!parseStrict("[capabilities.remote]\nauto_install_host = true\n", misplaced));
    CHECK(!misplaced.autoDeployHostOnRemoteConnect.has_value());
}

// A value outside allow | prompt | deny is refused through the parser's
// skipped-key path and leaves the default standing; Benign and Rendering have
// no key, and an unknown origin is no table.
TEST(testCapabilitiesSectionRejectsInvalidValuesAndUnknownKeys)
{
    Values typo = parseString("[capabilities.remote]\ninformational = \"alow\"\n");
    CHECK(!typo.remoteCapabilities.informational.has_value());
    CHECK_EQ(static_cast<int>(typo.skippedKeys.size()), 1);
    if (!typo.skippedKeys.empty()) {
        CHECK(typo.skippedKeys[0].section == "capabilities.remote");
        CHECK(typo.skippedKeys[0].key == "informational");
        CHECK(typo.skippedKeys[0].line == 2);
        CHECK(typo.skippedKeys[0].reason == Ubiquitty::Preferences::SkipReason::WrongValueKind);
    }
    Values strict;
    CHECK(!parseStrict("[capabilities.remote]\ninformational = \"alow\"\n", strict));

    // Not a string: a bool is not a verdict.
    Values boolean = parseString("[capabilities.agent]\nhost_escaping = false\n");
    CHECK(!boolean.agentCapabilities.hostEscaping.has_value());
    CHECK_EQ(static_cast<int>(boolean.skippedKeys.size()), 1);
    if (!boolean.skippedKeys.empty())
        CHECK(boolean.skippedKeys[0].reason == Ubiquitty::Preferences::SkipReason::WrongValueKind);

    // The tiers that are not configurable have no key to set.
    for (const char* key : { "benign", "rendering", "read_exfil" }) {
        Values unknown = parseString(std::string("[capabilities.remote]\n") + key + " = \"deny\"\n");
        CHECK_EQ(static_cast<int>(unknown.skippedKeys.size()), 1);
        if (!unknown.skippedKeys.empty())
            CHECK_MSG(unknown.skippedKeys[0].reason == Ubiquitty::Preferences::SkipReason::UnknownKey, key);
    }
    Values origin = parseString("[capabilities.peer]\ninformational = \"deny\"\n");
    CHECK_EQ(static_cast<int>(origin.skippedKeys.size()), 1);

    CHECK(Ubiquitty::Preferences::capabilityDecisionFromString(" deny ") == Ubiquitty::CapabilityDecision::Deny);
    CHECK(Ubiquitty::Preferences::capabilityDecisionFromString("ask") == std::nullopt);
    CHECK(std::string(Ubiquitty::Preferences::capabilityDecisionToString(Ubiquitty::CapabilityDecision::Prompt)) == "prompt");
}

TEST(testSecurityPasswordDetect)
{
    Values off = parseString("[security]\npassword_detect = false\n");
    CHECK(off.passwordDetect == false);
    Values def = parseString("");
    CHECK(def.passwordDetect.value_or(Ubiquitty::Preferences::Defaults::passwordDetect) == Ubiquitty::Preferences::Defaults::passwordDetect);
    CHECK(Ubiquitty::Preferences::Defaults::passwordDetect == true); // the lock and the echo mask are on out of the box
}

TEST(testPlatformKeybindings)
{
    using Ubiquitty::Preferences::Platform;
    using Ubiquitty::Preferences::resolveKeyBindings;
    Values c = parseString("[keys]\n"
                           "\"cmd+c\" = \"copy\"\n"
                           "\"cmd+k\" = \"clear_scrollback\"\n"
                           "[mac.keys]\n"
                           "\"cmd+k\" = \"new_window\"\n" // overrides the global cmd+k
                           "\"cmd+t\" = \"new_tab\"\n"); // mac-only addition
    // Global table is untouched by the platform block.
    CHECK(c.keyBindings.size() == 2);
    CHECK(c.platformOverride(Platform::Mac).present);
    CHECK(c.platformOverride(Platform::Mac).keyBindings.size() == 2);
    CHECK(c.platformOverride(Platform::Mac).keyBindings[0].spec == "cmd+k");
    CHECK(c.platformOverride(Platform::Mac).keyBindings[0].action == "new_window");

    // Resolved for mac: cmd+k is replaced in place, cmd+t appended.
    std::vector<Ubiquitty::Preferences::KeyBindingEntry> mac = resolveKeyBindings(c, Platform::Mac);
    CHECK(mac.size() == 3);
    CHECK(mac[0].spec == "cmd+c" && mac[0].action == "copy");
    CHECK(mac[1].spec == "cmd+k" && mac[1].action == "new_window"); // replaced
    CHECK(mac[2].spec == "cmd+t" && mac[2].action == "new_tab"); // appended

    // Resolved for linux: no [linux.keys] block, so the global table stands.
    std::vector<Ubiquitty::Preferences::KeyBindingEntry> lin = resolveKeyBindings(c, Platform::Linux);
    CHECK(lin.size() == 2);
    CHECK(lin[1].spec == "cmd+k" && lin[1].action == "clear_scrollback");
}

TEST(testPlatformBlockAndKeysCoexist)
{
    using Ubiquitty::Preferences::Platform;
    Values c = parseString("[mac]\n"
                           "font_family = \"SF Mono\"\n"
                           "[mac.keys]\n"
                           "\"cmd+w\" = \"close_tab\"\n");
    const auto& po = c.platformOverride(Platform::Mac);
    CHECK(po.present);
    CHECK(po.profile.fontFamily == "SF Mono");
    CHECK(po.keyBindings.size() == 1 && po.keyBindings[0].spec == "cmd+w");
    // No global keybindings leaked.
    CHECK(c.keyBindings.empty());
}

TEST(testLinkHandlersSection)
{
    Values c = parseString("[link_handlers]\n"
                           "jira = \"open -a Jira %s\"\n"
                           "file = \"/usr/local/bin/edit %s\"\n"
                           "\"x-custom\" = \"handle %u\"\n");
    CHECK(c.linkHandlers.size() == 3);
    CHECK(c.linkHandlers[0].scheme == "jira");
    CHECK(c.linkHandlers[0].command == "open -a Jira %s");
    CHECK(c.linkHandlers[1].scheme == "file");
    CHECK(c.linkHandlers[1].command == "/usr/local/bin/edit %s");
    CHECK(c.linkHandlers[2].scheme == "x-custom"); // quoted key unquoted
    CHECK(c.linkHandlers[2].command == "handle %u");
    // No [link_handlers] block ⇒ the map is empty (default OS-open path).
    Values empty = parseString("[font]\nsize = 12\n");
    CHECK(empty.linkHandlers.empty());
}

TEST(testLinkHandlerLookupIsCaseInsensitiveAndFirstWins)
{
    namespace E = Ubiquitty::Preferences::Effective;

    Values c = parseString("[link_handlers]\n"
                           "JIRA = \"open -a Jira %s\"\n"
                           "jira = \"open -a Other %s\"\n");
    // A scheme comparison is case-insensitive, so the key a reader shouted
    // still answers the link they wrote in lower case.
    CHECK(E::linkHandlerCommand(c, "jira") == "open -a Jira %s");
    // Two spellings of one scheme are one scheme: the first is the one the
    // reader wrote, and the second never silently replaces it.
    CHECK(E::linkHandlerCommand(c, "JIRA") == "open -a Jira %s");
    // A scheme nobody listed has no command at all -- there is no implicit
    // mapping for a link to reach a program through.
    CHECK(E::linkHandlerCommand(c, "https").empty());
    CHECK(E::linkHandlerCommand(parseString("[font]\nsize = 12\n"), "jira").empty());
}

// [links] line_handler is a scalar the parser keeps whole, placeholders and all,
// and a [link_handlers] key named like it stays a scheme entry.
TEST(testLinkLineHandlerIsReadWhole)
{
    namespace E = Ubiquitty::Preferences::Effective;

    Values c = parseString("[links]\n"
                           "line_handler = \"code --goto {path}:{line}:{column}\"\n"
                           "[link_handlers]\n"
                           "line_handler = \"open %s\"\n");
    CHECK(c.linkLineHandler == "code --goto {path}:{line}:{column}");
    CHECK(E::linkLineHandler(c) == "code --goto {path}:{line}:{column}");
    CHECK(c.linkHandlers.size() == 1 && c.linkHandlers[0].scheme == "line_handler");
    // None configured: such a link opens the way any file link does.
    Values empty = parseString("[font]\nsize = 12\n");
    CHECK(!empty.linkLineHandler.has_value());
    CHECK(E::linkLineHandler(empty).empty());
}

TEST(testTriggersSection)
{
    Values c = parseString("[triggers]\n"
                           "\"[0-9]+ tests? passed\" = \"notify:Tests\"\n"
                           "\"[Ee]rror\" = \"highlight\"\n"
                           "\"https?://[a-zA-Z0-9./]+\" = \"copy\"\n");
    CHECK(c.triggers.size() == 3);
    CHECK(c.triggers[0].pattern == "[0-9]+ tests? passed");
    CHECK(c.triggers[0].action == "notify:Tests");
    CHECK(c.triggers[1].pattern == "[Ee]rror");
    CHECK(c.triggers[1].action == "highlight");
    CHECK(c.triggers[2].pattern == "https?://[a-zA-Z0-9./]+");
    CHECK(c.triggers[2].action == "copy");
    Values empty = parseString("[font]\nsize = 12\n");
    CHECK(empty.triggers.empty());
}

TEST(testCharacterClassSection)
{
    Values c = parseString("[characterClass]\n"
                           "path = \"[A-Za-z0-9_./-]+\"\n"
                           "word = \"[A-Za-z]+\"\n");
    CHECK(c.characterClasses.size() == 2);
    CHECK(c.characterClasses[0].name == "path");
    CHECK(c.characterClasses[0].pattern == "[A-Za-z0-9_./-]+");
    CHECK(c.characterClasses[1].name == "word");
    CHECK(c.characterClasses[1].pattern == "[A-Za-z]+");
    Values empty = parseString("[font]\nsize = 12\n");
    CHECK(empty.characterClasses.empty());
}

TEST(testMacrosSection)
{
    Values c = parseString("[macros]\n"
                           "deploy = \"git push\\n\"\n"
                           "\"clear log\" = \"clear && tail -f log\\n\"\n");
    CHECK(c.macros.size() == 2);
    CHECK(c.macros[0].name == "deploy");
    CHECK(c.macros[0].body == "git push\n"); // TOML decodes \n in the value
    CHECK(c.macros[1].name == "clear log");
    Values empty = parseString("[font]\nsize = 12\n");
    CHECK(empty.macros.empty());

    // findMacroBody returns the raw body by name (first match), nullptr otherwise.
    const std::string* deploy = Ubiquitty::Preferences::findMacroBody(c, "deploy");
    CHECK(deploy != nullptr && *deploy == "git push\n");
    CHECK(Ubiquitty::Preferences::findMacroBody(c, "nope") == nullptr);
}

TEST(testScriptingSection)
{
    Values c = parseString("[scripting]\n"
                           "config_script = \"/usr/bin/rules.py\"\n"
                           "enable = true\n");
    CHECK(c.scriptingConfigScript.has_value());
    CHECK(c.scriptingConfigScript == "/usr/bin/rules.py");
    CHECK(c.scriptingEnable.value_or(false));
    Values off = parseString("[scripting]\nconfig_script = \"/x\"\n");
    CHECK(!off.scriptingEnable.has_value()); // gate stays closed unless explicitly enabled
}

TEST(testScriptingRulesParse)
{
    Ubiquitty::Preferences::ScriptingRules r;
    const std::string output = "# emitted by rules.py\n"
                               "trigger [0-9]+ errors = notify:Errors\n"
                               "macro deploy = git push\n"
                               "charclass path = [A-Za-z0-9_./-]+\n"
                               "\n"
                               "bogus directive = ignored\n"
                               "trigger BUILD [A-Z]+ = webhook:https://h/x=1\n"; // '=' in value kept
    int n = Ubiquitty::Preferences::parseScriptingRules(output, r);
    CHECK(n == 4);
    CHECK(r.triggers.size() == 2);
    CHECK(r.triggers[0].pattern == "[0-9]+ errors");
    CHECK(r.triggers[0].action == "notify:Errors");
    CHECK(r.triggers[1].pattern == "BUILD [A-Z]+");
    CHECK(r.triggers[1].action == "webhook:https://h/x=1"); // only FIRST '=' splits
    CHECK(r.macros.size() == 1);
    CHECK(r.macros[0].name == "deploy");
    CHECK(r.macros[0].body == "git push");
    CHECK(r.characterClasses.size() == 1);
    CHECK(r.characterClasses[0].name == "path");
    CHECK(r.characterClasses[0].pattern == "[A-Za-z0-9_./-]+");
}

TEST(testAllowWebhookGate)
{
    Values def = parseString("[triggers]\n\"x\" = \"webhook:https://h\"\n");
    CHECK(!def.allowWebhook.has_value()); // never set ⇒ gate closed
    Values on = parseString("[triggers]\nallow_webhook = true\n");
    CHECK(on.allowWebhook.value_or(false));
    Values off = parseString("[triggers]\nallow_webhook = false\n");
    CHECK(off.allowWebhook == false);
}

TEST(testWriteReplacesExistingKey)
{
    std::string p = scratchPath("replace");
    writeFile(p,
        "# leading comment\n"
        "[font]\n"
        "    family = \"Menlo\"\n"
        "    size = 14   # inline note\n"
        "\n"
        "[cursor]\n"
        "shape = \"block\"\n");
    bool ok = Ubiquitty::Preferences::writeKeyValue(p, "font", "size", Ubiquitty::Preferences::tomlInt(18));
    CHECK(ok);
    std::string body = readFile(p);
    CHECK(contains(body, "    size = 18"));
    CHECK(contains(body, "inline note")); // trailing comment preserved
    CHECK(contains(body, "family = \"Menlo\""));
    CHECK(contains(body, "# leading comment"));
    CHECK(contains(body, "shape = \"block\""));
    ::unlink(p.c_str());
}

TEST(testWriteUncommentsCommentedKey)
{
    std::string p = scratchPath("uncomment");
    writeFile(p,
        "[font]\n"
        "  # size = 12        # default\n");
    bool ok = Ubiquitty::Preferences::writeKeyValue(p, "font", "size", Ubiquitty::Preferences::tomlInt(20));
    CHECK(ok);
    std::string body = readFile(p);
    CHECK(contains(body, "  size = 20"));
    // No leftover '#' before the key.
    CHECK(!contains(body, "# size = 12"));
    ::unlink(p.c_str());
}

TEST(testWriteAppendsMissingKey)
{
    std::string p = scratchPath("append");
    writeFile(p,
        "[font]\n"
        "family = \"Menlo\"\n"
        "\n"
        "[cursor]\n"
        "shape = \"block\"\n");
    bool ok = Ubiquitty::Preferences::writeKeyValue(p, "font", "ligatures", Ubiquitty::Preferences::tomlBool(true));
    CHECK(ok);
    std::string body = readFile(p);
    CHECK(contains(body, "ligatures = true"));
    // Order: ligatures should land before the [cursor] header.
    size_t ligPosition = body.find("ligatures = true");
    size_t cursorPosition = body.find("[cursor]");
    CHECK(ligPosition != std::string::npos);
    CHECK(cursorPosition != std::string::npos);
    CHECK(ligPosition < cursorPosition);
    CHECK(contains(body, "shape = \"block\""));
    ::unlink(p.c_str());
}

TEST(testWriteCreatesMissingSection)
{
    std::string p = scratchPath("section");
    writeFile(p,
        "[font]\n"
        "size = 14\n");
    bool ok = Ubiquitty::Preferences::writeKeyValue(p, "bell", "visual", Ubiquitty::Preferences::tomlBool(true));
    CHECK(ok);
    std::string body = readFile(p);
    CHECK(contains(body, "[bell]"));
    CHECK(contains(body, "visual = true"));
    // Existing content untouched.
    CHECK(contains(body, "size = 14"));
    ::unlink(p.c_str());
}

TEST(testWriteThenParseRoundtrip)
{
    std::string p = scratchPath("rt");
    writeFile(p,
        "# header\n"
        "[appearance]\n"
        "padding = 4\n");
    Ubiquitty::Preferences::writeKeyValue(p, "appearance", "padding", Ubiquitty::Preferences::tomlInt(8));
    Ubiquitty::Preferences::writeKeyValue(p, "appearance", "color_space", Ubiquitty::Preferences::tomlString("display-p3"));
    Ubiquitty::Preferences::writeKeyValue(p, "cursor", "alpha", Ubiquitty::Preferences::tomlInt(128));
    Values c;
    std::ifstream in(p.c_str());
    parse(in, c);
    CHECK(c.padding == 8.0);
    CHECK(c.colorSpace == "display-p3");
    CHECK(c.cursorAlpha == 128);
    ::unlink(p.c_str());
}

TEST(testFractionalFontSizesRoundTripThroughTheFile)
{
    // The three [font] sizes a stepper sets in quarter points. Each goes the
    // whole way an edit goes — schema clamp, the descriptor's
    // serializer, the file, the parser, the effective accessor — because a
    // fraction is lost at whichever of those spells the value as a whole
    // number, and only the end-to-end trip says it is lost nowhere.
    struct FractionalKey {
        Ubiquitty::Preferences::Key id;
        const char* section;
        const char* key;
        double set;
        double (*effective)(const Values&);
    };
    const FractionalKey keys[] = {
        { Ubiquitty::Preferences::Key::FontSize, "font", "size", 12.5, &Ubiquitty::Preferences::Effective::fontSize },
        { Ubiquitty::Preferences::Key::FontProportionalSize, "font", "proportional_size", 13.75,
            &Ubiquitty::Preferences::Effective::proportionalFontSize },
        { Ubiquitty::Preferences::Key::FontLineSpacing, "font", "line_spacing", 2.25,
            &Ubiquitty::Preferences::Effective::lineSpacing },
    };

    for (const FractionalKey& entry : keys) {
        // The schema admits the fraction rather than snapping it to a point.
        CHECK(Ubiquitty::Preferences::Schema::clampToRange(entry.set, entry.section, entry.key) == entry.set);
        const Ubiquitty::Preferences::Schema::NumericRange* range
            = Ubiquitty::Preferences::Schema::rangeFor(entry.section, entry.key);
        CHECK(range != nullptr);
        CHECK(range->step == 0.25);

        std::string p = scratchPath(entry.key);
        writeFile(p, std::string("[font]\n") + entry.key + " = 12\n");
        const Ubiquitty::Preferences::WriteDescriptor* descriptor = Ubiquitty::Preferences::descriptorFor(entry.id);
        CHECK(descriptor != nullptr);
        CHECK(Ubiquitty::Preferences::writeKeyValue(p, descriptor->section, descriptor->key,
            Ubiquitty::Preferences::serializeWrite(descriptor->kind, entry.set)));

        // The literal keeps the fraction, so a reader opening the file sees
        // what the control was moved to.
        const std::string body = readFile(p);
        CHECK(contains(body, std::string(entry.key) + " = " + Ubiquitty::Preferences::tomlDouble(entry.set)));

        Values reloaded;
        std::ifstream in(p.c_str());
        parse(in, reloaded);
        CHECK(entry.effective(reloaded) == entry.set);
        ::unlink(p.c_str());
    }

    // A whole point still reads as a whole point, both in the file and back.
    std::string whole = scratchPath("whole_size");
    writeFile(whole, "[font]\nsize = 12.5\n");
    CHECK(Ubiquitty::Preferences::writeKeyValue(whole, "font", "size", Ubiquitty::Preferences::tomlDouble(14.0)));
    CHECK(contains(readFile(whole), "size = 14.0"));
    Values back;
    std::ifstream wholeIn(whole.c_str());
    parse(wholeIn, back);
    CHECK(Ubiquitty::Preferences::Effective::fontSize(back) == 14.0);
    ::unlink(whole.c_str());
}

TEST(testTOMLValueFormatters)
{
    CHECK(Ubiquitty::Preferences::tomlInt(42) == "42");
    CHECK(Ubiquitty::Preferences::tomlInt(-7) == "-7");
    CHECK(Ubiquitty::Preferences::tomlBool(true) == "true");
    CHECK(Ubiquitty::Preferences::tomlBool(false) == "false");
    CHECK(Ubiquitty::Preferences::tomlDouble(1.5) == "1.5");
    CHECK(Ubiquitty::Preferences::tomlDouble(2.0) == "2.0"); // forced .0
    CHECK(Ubiquitty::Preferences::tomlString("hi") == "\"hi\"");
    CHECK(Ubiquitty::Preferences::tomlString("a\"b") == "\"a\\\"b\"");
}

TEST(testTOMLHexColor)
{
    CHECK(Ubiquitty::Preferences::tomlHexColor(0x11223344) == "#11223344");
    CHECK(Ubiquitty::Preferences::tomlHexColor(0xaabbccff) == "#aabbcc"); // opaque
    CHECK(Ubiquitty::Preferences::tomlHexColor(0x00000000) == "#00000000");
}

TEST(testEscapeTOMLBasicString)
{
    using Ubiquitty::Preferences::escapeTOMLBasicString;

    // An ordinary value is untouched.
    CHECK(escapeTOMLBasicString("plain") == "plain");

    // The two characters that can end the string early.
    CHECK(escapeTOMLBasicString("a\"b") == "a\\\"b");
    // A binding spec already carrying a literal escape keeps it, doubled: the
    // file says \\x18 and the reader hands back \x18.
    CHECK(escapeTOMLBasicString("send:\\x18") == "send:\\\\x18");

    // The whitespace controls the reader spells with a short escape. A raw
    // newline is the injection itself: it ends the line, and the next one is
    // read as configuration.
    CHECK(escapeTOMLBasicString("a\nb") == "a\\nb");
    CHECK(escapeTOMLBasicString("a\tb") == "a\\tb");
    CHECK(escapeTOMLBasicString("a\rb") == "a\\rb");
    CHECK(escapeTOMLBasicString("a\bb") == "a\\bb");
    CHECK(escapeTOMLBasicString("a\fb") == "a\\fb");

    // The remaining C0 controls and DEL have no short escape, so they go out as
    // \uXXXX rather than raw. Split literals, because "\x01b" is one greedy hex
    // escape — 0x1B, not 0x01 followed by 'b'.
    CHECK(escapeTOMLBasicString(std::string("a\x01"
                                            "b"))
        == "a\\u0001b");
    CHECK(escapeTOMLBasicString(std::string("a\x1f"
                                            "b"))
        == "a\\u001Fb");
    CHECK(escapeTOMLBasicString("a\x7f") == "a\\u007F");

    // Multi-byte UTF-8 passes through: splitting it into escapes would change
    // the text.
    CHECK(escapeTOMLBasicString("caf\xc3\xa9") == "caf\xc3\xa9");
}

TEST(testIsBindingLine)
{
    CHECK(Ubiquitty::Preferences::isBindingLine("\"cmd+c\" = \"copy\""));
    CHECK(Ubiquitty::Preferences::isBindingLine("    \"cmd+c\" = \"copy\"")); // indented
    CHECK(!Ubiquitty::Preferences::isBindingLine("# a comment"));
    CHECK(!Ubiquitty::Preferences::isBindingLine("[keys]"));
    CHECK(!Ubiquitty::Preferences::isBindingLine(""));
}

TEST(testRenderAndSpliceThemeBlock)
{
    Ubiquitty::ColorRGBA ansi[16];
    for (int i = 0; i < 16; ++i)
        ansi[i] = Ubiquitty::ColorRGBA { static_cast<uint8_t>(i), 0x00, 0xff, 0xff };
    std::string block = Ubiquitty::Themes::renderThemeBlock("MyTheme", true, ansi, Ubiquitty::ColorRGBA { 0x11, 0x22, 0x33, 0xff }, Ubiquitty::ColorRGBA { 0x44, 0x55, 0x66, 0xff }, Ubiquitty::ColorRGBA { 0x77, 0x88, 0x99, 0xff });
    CHECK(contains(block, "[[themes]]"));
    CHECK(contains(block, "name = \"MyTheme\""));
    CHECK(contains(block, "is_dark = true"));
    CHECK(contains(block, "fg     = \"#112233\""));

    // Splice into a body that already carries comments + an unrelated key,
    // then parse: the comment/key survive and the theme is present.
    std::string body = "# top comment\n"
                       "[font]\n"
                       "size = 14\n";
    body = Ubiquitty::Preferences::spliceNamedBlock(std::move(body), "themes", "MyTheme", block);
    CHECK(contains(body, "# top comment"));
    CHECK(contains(body, "size = 14"));
    Values c = parseString(body);
    CHECK(c.fontSize == 14);
    CHECK(c.themes.size() == 1 && c.themes[0].name == "MyTheme");
    CHECK(c.themes[0].isDark.has_value() && c.themes[0].isDark);
}

// The half testRenderBlockEscapesTheName cannot reach: that the escaped block
// still PARSES, and parses to the name that was written rather than to a second
// theme somebody smuggled in. A hostile name only pays off if the reader accepts
// the file, so the reader is where it has to be denied.
TEST(testHostileThemeNameCannotInjectConfig)
{
    Ubiquitty::ColorRGBA ansi[16];
    for (int i = 0; i < 16; ++i)
        ansi[i] = Ubiquitty::ColorRGBA { static_cast<uint8_t>(i), 0x00, 0xff, 0xff };
    const Ubiquitty::ColorRGBA plain { 0x11, 0x22, 0x33, 0xff };

    // Close the string, open a section of the attacker's choosing, and set a key
    // that is not a theme key at all.
    const std::string hostile = "evil\"\n[[themes]]\nname = \"pwned\"\nis_dark = false\n[font]\nsize = 99";
    std::string body = "# top comment\n[font]\nsize = 14\n";
    body = Ubiquitty::Preferences::spliceNamedBlock(std::move(body), "themes", hostile,
        Ubiquitty::Themes::renderThemeBlock(hostile, /*isDark*/ true, ansi, plain, plain, plain));

    Values c = parseString(body);

    // The file still parses and the unrelated key is untouched: 14, not the 99
    // the payload asked for.
    CHECK(c.fontSize == 14);

    // Exactly one theme, carrying the hostile text as its NAME — data, not syntax.
    // The payload's bytes are still in the file, escaped; what the escape denies
    // it is a line of its own, and only the parse can say whether it got one.
    CHECK(c.themes.size() == 1);
    if (c.themes.empty())
        return;
    CHECK(c.themes[0].name == hostile);
}

TEST(testSpliceReplacesNamedBlock)
{
    std::string body = "[[themes]]\n"
                       "name = \"Keep\"\n"
                       "is_dark = false\n"
                       "\n"
                       "[[themes]]\n"
                       "name = \"Edit\"\n"
                       "is_dark = false\n";
    Ubiquitty::ColorRGBA ansi[16] = {};
    std::string block = Ubiquitty::Themes::renderThemeBlock("Edit", true, ansi, Ubiquitty::ColorRGBA { 0x01, 0x01, 0x01, 0xff }, Ubiquitty::ColorRGBA { 0x02, 0x02, 0x02, 0xff }, Ubiquitty::ColorRGBA { 0x03, 0x03, 0x03, 0xff });
    body = Ubiquitty::Preferences::spliceNamedBlock(std::move(body), "themes", "Edit", block);
    Values c = parseString(body);
    CHECK(c.themes.size() == 2); // no duplicate appended
    // "Edit" now reads back is_dark = true (the rewrite took).
    bool sawEditDark = false, sawKeep = false;
    for (const auto& t : c.themes) {
        if (t.name == "Edit")
            sawEditDark = (t.isDark.value_or(false));
        if (t.name == "Keep")
            sawKeep = true;
    }
    CHECK(sawEditDark);
    CHECK(sawKeep);
}

TEST(testRemoveNamedBlock)
{
    std::string body = "[[profiles]]\n"
                       "name = \"A\"\n"
                       "\n"
                       "[[profiles]]\n"
                       "name = \"B\"\n";
    body = Ubiquitty::Preferences::removeNamedBlock(std::move(body), "profiles", "A");
    CHECK(!contains(body, "name = \"A\""));
    CHECK(contains(body, "name = \"B\""));
}

// The three cases above read their result through the parser, which forgives a
// byte-range boundary off by one and cannot see where an appended block landed.
// findNamedBlock hands out a byte range and its callers splice on it, so these
// compare the text.
//
// Two controls fail here and pass against every other case in this file: ending
// the block one byte early, and reporting a hit on a name that is absent — the
// second turns an append into an insert at position zero, which parses to the
// same config and writes a different file.
TEST(testNamedBlockSplicingIsExactText)
{
    const std::string body = "[[profiles]]\n"
                             "name = \"A\"\n"
                             "font_size = 10\n"
                             "\n"
                             "[[profiles]]\n"
                             "name = \"B\"\n"
                             "font_size = 12\n";

    // A hit replaces exactly its own block: the blank line separating it from
    // the next belongs to the block, and B is untouched to the byte.
    CHECK(Ubiquitty::Preferences::spliceNamedBlock(body, "profiles", "A",
              "[[profiles]]\nname = \"A\"\nfont_size = 99\n\n")
        == "[[profiles]]\n"
           "name = \"A\"\n"
           "font_size = 99\n"
           "\n"
           "[[profiles]]\n"
           "name = \"B\"\n"
           "font_size = 12\n");

    // A miss appends. Where it lands is the whole difference between this and a
    // range a caller was handed for a block that does not exist.
    CHECK(Ubiquitty::Preferences::spliceNamedBlock(body, "profiles", "Z", "[[profiles]]\nname = \"Z\"\n")
        == body + "[[profiles]]\nname = \"Z\"\n");

    // Removal takes the same range, so the first block leaves nothing behind
    // and the last leaves the separator that preceded it.
    CHECK(Ubiquitty::Preferences::removeNamedBlock(body, "profiles", "A")
        == "[[profiles]]\nname = \"B\"\nfont_size = 12\n");
    CHECK(Ubiquitty::Preferences::removeNamedBlock(body, "profiles", "B")
        == "[[profiles]]\nname = \"A\"\nfont_size = 10\n\n");
}

TEST(testRemoveSectionKey)
{
    std::string body = "[mac]\n"
                       "# a note\n"
                       "font_family = \"SF Mono\"\n"
                       "font_size = 13\n";
    body = Ubiquitty::Preferences::removeSectionKey(std::move(body), "mac", "font_family");
    CHECK(!contains(body, "font_family"));
    CHECK(contains(body, "# a note"));
    CHECK(contains(body, "font_size = 13"));
    CHECK(contains(body, "[mac]"));
}

TEST(testRemoveSectionKeyPreservesFormattingAndRemovesRepeatedOverrides)
{
    const std::string document = "  [ font ] # main font\r\n# keep this note\r\nsize = 14\r\nsize = 20 # duplicate\r\nfamily = \"Menlo\"\r\n[mac.font]\r\nsize = 30\r\n[[profiles]]\r\nname = \"Work\"\r\nfont_size = 18\r\n";
    const std::string expected = "  [ font ] # main font\r\n# keep this note\r\nfamily = \"Menlo\"\r\n[mac.font]\r\nsize = 30\r\n[[profiles]]\r\nname = \"Work\"\r\nfont_size = 18\r\n";
    CHECK(Ubiquitty::Preferences::removeSectionKey(document, "font", "size") == expected);
    CHECK(Ubiquitty::Preferences::removeSectionKey(document, "font", "unknown") == document);
}

TEST(testRewriteKeysSection)
{
    std::string body = "[font]\n"
                       "size = 14\n"
                       "\n"
                       "[keys]\n"
                       "# my bindings\n"
                       "\"cmd+c\" = \"copy\"\n";
    std::vector<std::string> lines = {
        R"("cmd+x" = "cut")",
        R"("cmd+v" = "paste")",
    };
    body = Ubiquitty::Preferences::rewriteKeysSection(std::move(body), lines);
    CHECK(contains(body, "# my bindings")); // comment kept
    CHECK(!contains(body, "cmd+c")); // old binding gone
    CHECK(contains(body, "\"cmd+x\" = \"cut\""));
    CHECK(contains(body, "size = 14")); // unrelated section kept
    Values c = parseString(body);
    CHECK(c.keyBindings.size() == 2);
    CHECK(c.keyBindings[0].spec == "cmd+x");

    // No [keys] section yet → one is appended at EOF.
    std::string fresh = "[font]\nsize = 14\n";
    fresh = Ubiquitty::Preferences::rewriteKeysSection(std::move(fresh), { R"("cmd+q" = "quit")" });
    CHECK(contains(fresh, "[keys]"));
    Values c2 = parseString(fresh);
    CHECK(c2.keyBindings.size() == 1 && c2.keyBindings[0].spec == "cmd+q");
}

// The binding editor rewrites the whole [keys] table. The scalar
// options sharing that section are the reader's own lines, not bindings, so an
// edit to a binding must not take away the Option key they chose as Meta.
TEST(testRewriteKeysSectionKeepsOptionAsAlternate)
{
    std::string body = "[keys]\n"
                       "option_as_alt = \"left\"\n"
                       "\"cmd+c\" = \"copy\"\n";
    body = Ubiquitty::Preferences::rewriteKeysSection(std::move(body), { R"("cmd+x" = "cut")" });
    CHECK(contains(body, "option_as_alt = \"left\""));
    Values c = parseString(body);
    CHECK(c.optionAsAlternate == OptionAsAlternate::Left);
    CHECK(c.keyBindings.size() == 1);
}

// findNamedBlock is the one scan every writer of a named block calls, the macOS
// theme editor included, so the [[themes]] boundaries below are pinned on it.
TEST(testFindNamedBlockRangeStopsAtTheNextSection)
{
    // Two themes plus a trailing [keys] section. The scan must return the byte
    // range of exactly the matching [[themes]] block (header line through the
    // line before the next section header).
    std::string body = "# preamble\n"
                       "[[themes]]\n"
                       "name = \"Alpha\"\n"
                       "is_dark = true\n"
                       "fg = \"#111111\"\n"
                       "\n"
                       "[[themes]]\n"
                       "name = \"Beta\"\n"
                       "is_dark = false\n"
                       "fg = \"#222222\"\n"
                       "\n"
                       "[keys]\n"
                       "\"cmd+c\" = \"copy\"\n";

    const auto beta_range = Ubiquitty::Preferences::findNamedBlock(body, "themes", "Beta");
    CHECK(beta_range.has_value());
    std::string beta = body.substr(beta_range->start, beta_range->end - beta_range->start);
    CHECK(beta.find("name = \"Beta\"") != std::string::npos);
    // The range stops before the [keys] header — it must not swallow it.
    CHECK(beta.find("[keys]") == std::string::npos);
    CHECK(beta.find("\"cmd+c\"") == std::string::npos);
    // ...and must not bleed into the Alpha block.
    CHECK(beta.find("Alpha") == std::string::npos);

    // Alpha resolves to the first block, ending before the second [[themes]].
    const auto alpha_range = Ubiquitty::Preferences::findNamedBlock(body, "themes", "Alpha");
    CHECK(alpha_range.has_value());
    std::string alpha = body.substr(alpha_range->start, alpha_range->end - alpha_range->start);
    CHECK(alpha.find("name = \"Alpha\"") != std::string::npos);
    CHECK(alpha.find("Beta") == std::string::npos);

    // A name that is not present, and an empty body: nothing, which is what
    // sends spliceNamedBlock down its append path.
    CHECK(!Ubiquitty::Preferences::findNamedBlock(body, "themes", "Gamma"));
    CHECK(!Ubiquitty::Preferences::findNamedBlock("", "themes", "Alpha"));
}

// The editor's save path: render a block, splice it over the prior block of the
// same name, and confirm the spliced body re-parses to the new block. This is
// the render -> find -> overwrite round-trip the macOS editor performs.
TEST(testThemeEditorSaveRoundTrip)
{
    Ubiquitty::ColorRGBA ansi[16];
    for (int i = 0; i < 16; ++i)
        ansi[i] = Ubiquitty::ColorRGBA { 0x40, 0x50, 0x60, 0xFF };
    Ubiquitty::ColorRGBA fg { 0xAA, 0xAA, 0xAA, 0xFF };
    Ubiquitty::ColorRGBA bg { 0x11, 0x11, 0x11, 0xFF };
    Ubiquitty::ColorRGBA cursor { 0xCC, 0xCC, 0xCC, 0xFF };

    // Start with an existing "Mine" block followed by another section.
    std::string body = "[[themes]]\n"
                       "name = \"Mine\"\n"
                       "is_dark = false\n"
                       "fg = \"#000000\"\n"
                       "\n"
                       "[appearance]\n"
                       "follow_system = true\n";

    std::string fresh = Ubiquitty::Themes::renderThemeBlock("Mine", /*isDark*/ true, ansi, fg, bg, cursor);

    // The editor calls exactly this, rather than mirroring it: the find, the
    // leading-newline strip the renderer's output needs when it overwrites in
    // place, and the append are all spliceNamedBlock's.
    body = Ubiquitty::Preferences::spliceNamedBlock(std::move(body), "themes", "Mine", fresh);

    // The new block's content is present; the old fg color is gone.
    CHECK(body.find("is_dark = true\n") != std::string::npos);
    CHECK(body.find("fg     = \"#aaaaaa\"\n") != std::string::npos);
    CHECK(body.find("fg = \"#000000\"") == std::string::npos);
    // The unrelated [appearance] section survived intact.
    CHECK(body.find("[appearance]\n") != std::string::npos);
    CHECK(body.find("follow_system = true\n") != std::string::npos);

    // Re-finding "Mine" still yields a single, well-formed block.
    const auto again_range = Ubiquitty::Preferences::findNamedBlock(body, "themes", "Mine");
    CHECK(again_range.has_value());
    std::string again = body.substr(again_range->start, again_range->end - again_range->start);
    CHECK(again.find("name = \"Mine\"") != std::string::npos);
    CHECK(again.find("[appearance]") == std::string::npos);

    // An append path (no prior block) just concatenates a fresh block.
    const std::string appended = Ubiquitty::Preferences::spliceNamedBlock(
        body, "themes", "New One", Ubiquitty::Themes::renderThemeBlock("New One", false, ansi, fg, bg, cursor));
    CHECK(Ubiquitty::Preferences::findNamedBlock(appended, "themes", "New One").has_value());
}

// `[appearance] shade_blocks` is a closed vocabulary: both tokens reach the
// render configuration, an unknown one is refused at the parse and resolves
// to the pattern, and the pattern is what an absent key means.
TEST(shadeBlocksReachTheRenderConfiguration)
{
    using namespace Ubiquitty::Preferences;
    using Ubiquitty::Rendering::ShadeStyle;
    CHECK(Effective::shadeStyle(Values {}) == ShadeStyle::Pattern);
    CHECK(Effective::renderConfiguration(Values {}).shadeStyle == ShadeStyle::Pattern);
    const Values fill = parseString("[appearance]\nshade_blocks = \"fill\"\n");
    CHECK(Effective::shadeStyle(fill) == ShadeStyle::Fill);
    CHECK(Effective::renderConfiguration(fill).shadeStyle == ShadeStyle::Fill);
    CHECK(Effective::shadeStyle(parseString("[appearance]\nshade_blocks = \"pattern\"\n")) == ShadeStyle::Pattern);
    const Values unknown = parseString("[appearance]\nshade_blocks = \"dither\"\n");
    CHECK(!unknown.shadeBlocks.has_value());
    CHECK(Effective::shadeStyle(unknown) == ShadeStyle::Pattern);
    // A different style is a different typography: the Linux sheet bakes it.
    CHECK(!Effective::renderConfiguration(fill).hasSameTypography(Effective::renderConfiguration(Values {})));
}

TEST(proceduralBrailleStylesReachTheRenderConfiguration)
{
    using namespace Ubiquitty::Rendering;
    using namespace Ubiquitty::Preferences;
    for (const auto style : { BrailleStyle::Regular, BrailleStyle::Outline6Dot, BrailleStyle::Outline8Dot, BrailleStyle::Pinpoint6Dot, BrailleStyle::Pinpoint8Dot, BrailleStyle::Square, BrailleStyle::Raster }) {
        const auto token = brailleStyleToString(style);
        const Values configuration = parseString("[font]\nbraille_style = \"" + std::string(token) + "\"\n");
        CHECK(Effective::brailleStyle(configuration) == style);
        CHECK(Effective::renderConfiguration(configuration).brailleStyle == style);
        CHECK(Effective::brailleGrid(configuration));
    }
    CHECK(Effective::brailleStyle(Values {}) == BrailleStyle::Regular);
    CHECK(Effective::brailleStyle(parseString("[font]\nbraille_style = \"unknown\"\n")) == BrailleStyle::Regular);
}

TEST(selectionStyleReachesTheRenderConfiguration)
{
    using namespace Ubiquitty::Rendering;
    using namespace Ubiquitty::Preferences;
    for (const auto style : { SelectionStyle::Native, SelectionStyle::Tint }) {
        const auto token = selectionStyleToString(style);
        const Values configuration = parseString("[selection]\nstyle = \"" + std::string(token) + "\"\n");
        CHECK(Effective::selectionStyle(configuration) == style);
        CHECK(Effective::renderConfiguration(configuration).selectionStyle == style);
        TerminalSnapshotStyle frameStyle;
        frameStyle.selectionStyle = style == SelectionStyle::Native ? SelectionStyle::Tint : SelectionStyle::Native;
        Effective::applyRuleSettings(frameStyle, Effective::renderConfiguration(configuration), 1.0);
        CHECK(frameStyle.selectionStyle == style);
    }
    CHECK(Effective::selectionStyle(Values {}) == SelectionStyle::Native);
    CHECK(Effective::selectionStyle(parseString("[selection]\nstyle = \"unknown\"\n")) == SelectionStyle::Native);
}

TEST(remoteNonOwnerPresentationIsExplicitAndDefaultsToLetterbox)
{
    using namespace Ubiquitty::Rendering;
    using namespace Ubiquitty::Preferences;
    for (const auto policy : { RemoteViewportPolicy::Letterbox, RemoteViewportPolicy::Pan, RemoteViewportPolicy::Fit }) {
        const auto values = parseString("[remote]\nnon_owner_presentation = \"" + std::string(remoteViewportPolicyToString(policy)) + "\"\n");
        CHECK(Effective::nonOwnerPresentation(values) == policy);
        CHECK(Effective::renderConfiguration(values).nonOwnerPresentation == policy);
    }
    CHECK(Effective::nonOwnerPresentation(Values {}) == RemoteViewportPolicy::Letterbox);
    CHECK(Effective::nonOwnerPresentation(parseString("[remote]\nnon_owner_presentation = \"unknown\"\n")) == RemoteViewportPolicy::Letterbox);
}

UBIQUITTY_TEST_MAIN()

TEST(themeAppearanceRoundTrip)
{
    const auto parsed = parseString(R"(
[[themes]]
name = "Custom Glass"
background_opacity = 0
blur_radius = 20
text_vibrancy = true
text_contrast = false
background_vibrancy = true
terminal_marks = "tertiary"
default_background_opacity = 0.75
system_colors = true
)");
    CHECK(parsed.themes.size() == 1);
    if (parsed.themes.empty()) {
        return;
    }
    const auto& appearance = parsed.themes[0].appearance;
    CHECK(appearance.backgroundOpacity == 0);
    CHECK(appearance.defaultBackgroundOpacity == 0.75);
    CHECK(appearance.blurRadius == 20);
    CHECK(appearance.textVibrancy == true);
    CHECK(appearance.textContrast == false);
    CHECK(appearance.backgroundVibrancy == true);
    CHECK(appearance.terminalMarks == Ubiquitty::Themes::TerminalMarkStyle::Tertiary);
    CHECK(appearance.usesSystemColors);
    Ubiquitty::ColorRGBA colors[16] {};
    const auto body = Ubiquitty::Themes::renderThemeBlock("Custom Glass", false, colors, {}, {}, {}, appearance);
    const auto reparsed = parseString(body);
    CHECK(reparsed.themes.size() == 1);
    if (!reparsed.themes.empty()) {
        CHECK(reparsed.themes[0].appearance.backgroundOpacity == 0);
        CHECK(reparsed.themes[0].appearance.defaultBackgroundOpacity == 0.75);
        CHECK(reparsed.themes[0].appearance.blurRadius == 20);
        CHECK(reparsed.themes[0].appearance.textVibrancy == true);
        CHECK(reparsed.themes[0].appearance.textContrast == false);
        CHECK(reparsed.themes[0].appearance.backgroundVibrancy == true);
        CHECK(reparsed.themes[0].appearance.terminalMarks == Ubiquitty::Themes::TerminalMarkStyle::Tertiary);
    }
}
