// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_themeimport.cpp — pins the portable third-party theme importer
// Source/Core/Themes/ThemeImport.{h,cpp}: parse an iTerm2 ".itermcolors" XML plist
// and a Ghostty theme file into a Ubiquitty palette (16 ANSI + fg/bg/cursor),
// with format auto-detection and darkness derivation.
//
// Foundation-free, so it links standalone: its only Core type is ColorRGBA,
// from Core/Color.h.

#include "Core/Themes/ThemeImport.h"
#include "Core/Themes/Themes.h"

#include <cstdio>
#include <string>

using Ubiquitty::Themes::ImportedTheme;
using Ubiquitty::Themes::importedThemeIsDark;
using Ubiquitty::Themes::ImportFormat;
using Ubiquitty::Themes::importThemeFromBytes;

// A minimal but realistic .itermcolors plist: all 16 ANSI slots plus the three
// named colors. Channels are floats in 0.0..1.0 (iTerm2's NSColor encoding).
// Ansi 0 = black (0,0,0), Ansi 1 = pure red (1,0,0), Ansi 2 = pure green,
// Foreground = white, Background = black, Cursor = red.
static std::string sampleITermColors()
{
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                    "<plist version=\"1.0\">\n"
                    "<dict>\n";
    // 16 ANSI slots. Make each slot's red channel = idx/15 so we can assert a
    // distinct, computable value, and keep green/blue at 0 for the check.
    for (int i = 0; i < 16; ++i) {
        double r = static_cast<double>(i) / 15.0;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "<key>Ansi %d Color</key>\n"
            "<dict>\n"
            "  <key>Color Space</key><string>sRGB</string>\n"
            "  <key>Red Component</key><real>%f</real>\n"
            "  <key>Green Component</key><real>0.0</real>\n"
            "  <key>Blue Component</key><real>0.0</real>\n"
            "</dict>\n",
            i, r);
        s += buf;
    }
    s += "<key>Foreground Color</key>\n"
         "<dict>\n"
         "  <key>Red Component</key><real>1.0</real>\n"
         "  <key>Green Component</key><real>1.0</real>\n"
         "  <key>Blue Component</key><real>1.0</real>\n"
         "</dict>\n"
         "<key>Background Color</key>\n"
         "<dict>\n"
         "  <key>Red Component</key><real>0.0</real>\n"
         "  <key>Green Component</key><real>0.0</real>\n"
         "  <key>Blue Component</key><real>0.0</real>\n"
         "</dict>\n"
         "<key>Cursor Color</key>\n"
         "<dict>\n"
         "  <key>Red Component</key><real>1.0</real>\n"
         "  <key>Green Component</key><real>0.0</real>\n"
         "  <key>Blue Component</key><real>0.0</real>\n"
         "</dict>\n"
         "</dict>\n"
         "</plist>\n";
    return s;
}

TEST(testItermcolorsParse)
{
    std::string xml = sampleITermColors();
    ImportedTheme t = importThemeFromBytes(xml);

    CHECK(t.format == ImportFormat::ITermColors);
    CHECK(t.ok); // all 16 ANSI slots present

    // Ansi 0 red = 0/15 → 0; Ansi 15 red = 15/15 → 255.
    CHECK(t.ansiSet[0]);
    CHECK(t.ansi[0].red == 0);
    CHECK(t.ansi[0].green == 0);
    CHECK(t.ansi[0].blue == 0);
    CHECK(t.ansiSet[15]);
    CHECK(t.ansi[15].red == 255);
    // Ansi 8 red = 8/15 ≈ 0.5333 → round(136.0) = 136.
    CHECK(t.ansi[8].red == 136);

    CHECK(t.foregroundSet);
    CHECK(t.foreground.red == 255 && t.foreground.green == 255 && t.foreground.blue == 255);
    CHECK(t.backgroundSet);
    CHECK(t.background.red == 0 && t.background.green == 0 && t.background.blue == 0);
    CHECK(t.cursorSet);
    CHECK(t.cursor.red == 255 && t.cursor.green == 0 && t.cursor.blue == 0);

    // Black background → dark theme.
    CHECK(importedThemeIsDark(t));
}

// A Ghostty theme file: `palette = N=#rrggbb` lines plus named colors, with
// comments and blank lines to exercise the line parser's skips.
static std::string sampleGhostty()
{
    std::string s = "# Sample Ghostty theme\n\n";
    static const char* hex[16] = { "#000000", "#ff0000", "#00ff00", "#ffff00", "#0000ff", "#ff00ff", "#00ffff", "#c0c0c0", "#808080", "#ff8080", "#80ff80", "#ffff80", "#8080ff", "#ff80ff", "#80ffff", "#ffffff" };
    for (int i = 0; i < 16; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "palette = %d=%s\n", i, hex[i]);
        s += buf;
    }
    s += "\nbackground = #1d2021\n";
    s += "foreground = #ebdbb2\n";
    s += "cursor-color = #fe8019\n";
    return s;
}

TEST(testGhosttyParse)
{
    std::string text = sampleGhostty();
    ImportedTheme t = importThemeFromBytes(text);

    CHECK(t.format == ImportFormat::Ghostty);
    CHECK(t.ok);

    CHECK(t.ansi[0].red == 0x00 && t.ansi[0].green == 0x00 && t.ansi[0].blue == 0x00);
    CHECK(t.ansi[1].red == 0xff && t.ansi[1].green == 0x00 && t.ansi[1].blue == 0x00);
    CHECK(t.ansi[7].red == 0xc0 && t.ansi[7].green == 0xc0 && t.ansi[7].blue == 0xc0);
    CHECK(t.ansi[15].red == 0xff && t.ansi[15].green == 0xff && t.ansi[15].blue == 0xff);

    CHECK(t.backgroundSet);
    CHECK(t.background.red == 0x1d && t.background.green == 0x20 && t.background.blue == 0x21);
    CHECK(t.foregroundSet);
    CHECK(t.foreground.red == 0xeb && t.foreground.green == 0xdb && t.foreground.blue == 0xb2);
    CHECK(t.cursorSet);
    CHECK(t.cursor.red == 0xfe && t.cursor.green == 0x80 && t.cursor.blue == 0x19);

    CHECK(importedThemeIsDark(t)); // #1d2021 is dark
}

TEST(testFormatDetectionAndFailures)
{
    // Auto-detect: explicit format overrides.
    std::string xml = sampleITermColors();
    ImportedTheme forced = importThemeFromBytes(xml, ImportFormat::ITermColors);
    CHECK(forced.ok);

    // Unrecognized bytes → not ok, Unknown format.
    ImportedTheme junk = importThemeFromBytes("not a theme at all");
    CHECK(!junk.ok);

    // Light background → not dark.
    std::string light = "background = #ffffff\nforeground = #000000\n";
    for (int i = 0; i < 16; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "palette = %d=#101010\n", i);
        light += buf;
    }
    ImportedTheme l = importThemeFromBytes(light);
    CHECK(l.ok);
    CHECK(!importedThemeIsDark(l));

    // A Ghostty file missing some palette slots → ok == false (incomplete),
    // but the named colors still parse.
    std::string partial = "palette = 0=#111111\nbackground = #222222\n";
    ImportedTheme p = importThemeFromBytes(partial);
    CHECK(!p.ok);
    CHECK(p.backgroundSet);
    CHECK(p.ansiSet[0]);
    CHECK(!p.ansiSet[1]);
}

// Real-world plists arrive without the whitespace the samples above carry, so
// this feeds a document with the values packed hard against their tags.
//
// What it does not pin, measured rather than assumed. The `<dict>` and `</dict>`
// skips do not change an answer: `nextTag` locates each component by searching
// FORWARD from the caret, so a skip one character short or long finds the same
// tag and parses the same value. Only the key-prefix skip changes an answer,
// because its result is indexed directly rather than searched from. Injecting an
// off-by-one into each: the prefix skip fails this file, the two dict skips do
// not, and that is a property of the parser rather than a gap here.
TEST(testValuesPackedAgainstTheirTagsStillParse)
{
    std::string xml = "<?xml version=\"1.0\"?>\n<plist version=\"1.0\">\n<dict>\n";
    for (int i = 0; i < 16; ++i) {
        char buf[512];
        // No spaces and no newline between <dict> and the first key: a skip that
        // stops one short reads '>' as the start of the component scan, and one
        // long eats the '<' that opens the key.
        std::snprintf(buf, sizeof(buf),
            "<key>Ansi %d Color</key>\n"
            "<dict><key>Red Component</key><real>1.0</real>"
            "<key>Green Component</key><real>0.0</real>"
            "<key>Blue Component</key><real>0.0</real></dict>\n",
            i);
        xml += buf;
    }
    xml += "<key>Foreground Color</key>\n<dict><key>Red Component</key><real>1.0</real>"
           "<key>Green Component</key><real>1.0</real><key>Blue Component</key><real>1.0</real></dict>\n"
           "<key>Background Color</key>\n<dict><key>Red Component</key><real>0.0</real>"
           "<key>Green Component</key><real>0.0</real><key>Blue Component</key><real>0.0</real></dict>\n"
           "</dict>\n</plist>\n";

    ImportedTheme t = importThemeFromBytes(xml, ImportFormat::ITermColors);
    CHECK(t.ok);
    for (int i = 0; i < 16; ++i)
        CHECK(t.ansiSet[i]);
    CHECK(t.ansi[0].red == 0xff && t.ansi[0].green == 0x00 && t.ansi[0].blue == 0x00);
    CHECK(t.foregroundSet && t.foreground.red == 0xff);
    CHECK(t.backgroundSet && t.background.red == 0x00);
}

// What the importer must REFUSE. Each case is a shape a scan that trusts its
// input would accept, and each corrupts a palette rather than failing loudly: a
// key that merely starts with the prefix, an index outside the 16 slots, and a
// truncated document whose closing tag never arrives.
//
// The index bound is not pinned by the assertions below, and cannot be. Both
// palettes are `[16]`, so widening the bound writes PAST the array — the slots
// this file can read are untouched and every CHECK still holds. Verified by
// widening `index < 16` to `index < 99` and feeding `palette = 42=…`: the host
// build passes, and an AddressSanitizer build reports `stack-buffer-overflow` in
// `parseGhostty`. The case earns its place by being the input ASan needs; the
// bound itself is held by the sanitizer run, not by a CHECK.
TEST(testMalformedInputIsRefusedRatherThanGuessed)
{
    // "AnsiX" is not "Ansi " + digits. A prefix test without the separator, or a
    // skip measured one short, would take 'X' as the start of an index run.
    ImportedTheme noSeparator = importThemeFromBytes(
        "<plist><dict><key>AnsiFoo Color</key>\n"
        "<dict><key>Red Component</key><real>1.0</real></dict>\n"
        "</dict></plist>",
        ImportFormat::ITermColors);
    CHECK(!noSeparator.ok);
    for (int i = 0; i < 16; ++i)
        CHECK(!noSeparator.ansiSet[i]);

    // An index past the palette. Accepting it writes outside the 16 slots.
    ImportedTheme outOfRange = importThemeFromBytes(
        "<plist><dict><key>Ansi 99 Color</key>\n"
        "<dict><key>Red Component</key><real>1.0</real></dict>\n"
        "</dict></plist>",
        ImportFormat::ITermColors);
    CHECK(!outOfRange.ok);
    for (int i = 0; i < 16; ++i)
        CHECK(!outOfRange.ansiSet[i]);

    // A dict that never closes: the scan must stop rather than run to the end of
    // the buffer looking for a terminator that is not there.
    ImportedTheme truncated = importThemeFromBytes(
        "<plist><dict><key>Ansi 0 Color</key>\n<dict><key>Red Component</key><real>1.0",
        ImportFormat::ITermColors);
    CHECK(!truncated.ok);

    // A Ghostty palette index outside 0..15, alongside a valid one: the valid
    // slot lands and the out-of-range slot is dropped, rather than the file being
    // taken whole or rejected whole.
    ImportedTheme mixed = importThemeFromBytes("palette = 0=#111111\npalette = 42=#222222\nbackground = #333333\n");
    CHECK(!mixed.ok); // incomplete: only one of sixteen slots
    CHECK(mixed.ansiSet[0]);
    CHECK(mixed.ansi[0].red == 0x11);
}

UBIQUITTY_TEST_MAIN()

TEST(exportedPaletteRoundTripsWithoutGuessingDarknessOrName)
{
    Ubiquitty::Themes::Theme theme {};
    theme.name = "Custom \"Palette\"";
    theme.isDark = false;
    theme.defaultForeground = { 1, 2, 3, 200 };
    theme.defaultBackground = { 4, 5, 6, 150 };
    theme.cursor = { 7, 8, 9, 100 };
    for (int index = 0; index < 16; ++index) {
        theme.ansi[index] = { static_cast<uint8_t>(index * 11), 23, 47, static_cast<uint8_t>(index + 100) };
    }
    const auto matches = [](Ubiquitty::ColorRGBA left, Ubiquitty::ColorRGBA right) {
        return left.red == right.red && left.green == right.green && left.blue == right.blue && left.alpha == right.alpha;
    };
    const auto imported = importThemeFromBytes(theme.serializeTOML());
    REQUIRE(imported.ok);
    CHECK(imported.format == ImportFormat::Ubiquitty);
    CHECK(imported.name == theme.name);
    CHECK(!importedThemeIsDark(imported));
    for (int index = 0; index < 16; ++index) {
        CHECK(matches(imported.ansi[index], theme.ansi[index]));
    }
    CHECK(matches(imported.foreground, theme.defaultForeground));
    CHECK(matches(imported.background, theme.defaultBackground));
    CHECK(matches(imported.cursor, theme.cursor));
    CHECK(!importThemeFromBytes(theme.serializeTOML() + theme.serializeTOML()).ok);
    CHECK(!importThemeFromBytes("not a theme").ok);
}
