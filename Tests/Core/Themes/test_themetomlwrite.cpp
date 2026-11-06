// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_themetomlwrite.cpp — pins the TOML theme write-back the theme editor
// calls, in the portable header Source/Core/Themes/ThemeTOMLWrite.h:
//
//   - renderThemeBlock(...)     — serialize a live palette to a [[themes]]
//     stanza ("#rrggbb" opaque / "#rrggbbaa" with alpha, 4x4 ANSI grid).
//   - formatTOMLHexColor(...)   — the per-color hex formatter.
//
// The header is pure string + ColorRGBA logic, with ColorRGBA from
// Core/Color.h, so this links standalone.

#include "Core/Themes/ThemeTOMLWrite.h"

#include <cstddef>
#include <cstdio>
#include <string>

using Ubiquitty::Themes::formatTOMLHexColor;
using Ubiquitty::Themes::renderThemeBlock;

// How many lines of `text` start with `prefix`. A TOML header or key only means
// anything at the start of a line, so this is the shape an injection check has
// to take: the same characters sitting inside a quoted value are data.
static std::size_t countLinesBeginningWith(const std::string& text, const std::string& prefix)
{
    std::size_t found = 0;
    for (std::size_t lineStart = 0; lineStart <= text.size();) {
        if (text.compare(lineStart, prefix.size(), prefix) == 0)
            ++found;
        const std::size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            break;
        lineStart = lineEnd + 1;
    }
    return found;
}

TEST(testFormatHexColor)
{
    // Fully opaque → "#rrggbb" (no alpha digits).
    CHECK(formatTOMLHexColor(Ubiquitty::ColorRGBA { 0x12, 0x34, 0x56, 0xFF }) == "#123456");
    // Non-opaque → "#rrggbbaa".
    CHECK(formatTOMLHexColor(Ubiquitty::ColorRGBA { 0x12, 0x34, 0x56, 0x80 }) == "#12345680");
    // Lowercase hex, zero-padded.
    CHECK(formatTOMLHexColor(Ubiquitty::ColorRGBA { 0x0a, 0xff, 0x00, 0xFF }) == "#0aff00");
    CHECK(formatTOMLHexColor(Ubiquitty::ColorRGBA { 0, 0, 0, 0 }) == "#00000000");
}

TEST(testRenderBlockShape)
{
    Ubiquitty::ColorRGBA ansi[16];
    for (int i = 0; i < 16; ++i)
        ansi[i] = Ubiquitty::ColorRGBA { static_cast<uint8_t>(i), 0x20, 0x30, 0xFF };
    Ubiquitty::ColorRGBA fg { 0xAA, 0xBB, 0xCC, 0xFF };
    Ubiquitty::ColorRGBA bg { 0x10, 0x20, 0x30, 0xFF };
    Ubiquitty::ColorRGBA cursor { 0x01, 0x02, 0x03, 0x80 }; // alpha < 255 → 8-digit hex

    std::string block = renderThemeBlock("My Theme", /*isDark*/ true, ansi, fg, bg, cursor);

    // Leading newline so the block appends cleanly after prior content.
    CHECK(!block.empty() && block[0] == '\n');
    CHECK(block.find("[[themes]]\n") != std::string::npos);
    CHECK(block.find("name = \"My Theme\"\n") != std::string::npos);
    CHECK(block.find("is_dark = true\n") != std::string::npos);
    CHECK(block.find("ansi = [\n") != std::string::npos);
    // First ANSI color is index 0 → "#002030"; the 16th is index 15 → "#0f2030".
    CHECK(block.find("\"#002030\"") != std::string::npos);
    CHECK(block.find("\"#0f2030\"") != std::string::npos);
    CHECK(block.find("fg     = \"#aabbcc\"\n") != std::string::npos);
    CHECK(block.find("bg     = \"#102030\"\n") != std::string::npos);
    // Cursor carried alpha → 8-digit form.
    CHECK(block.find("cursor = \"#01020380\"\n") != std::string::npos);

    // is_dark false renders literally.
    std::string light = renderThemeBlock("L", /*isDark*/ false, ansi, fg, bg, cursor);
    CHECK(light.find("is_dark = false\n") != std::string::npos);
}

// A theme name is attacker-influenced: import takes it from a downloaded file's
// last path component, and the theme editor's field reaches the same writer. It
// is quoted into a TOML basic string, so anything that can close that string can
// write configuration of its own choosing.
TEST(testRenderBlockEscapesTheName)
{
    Ubiquitty::ColorRGBA ansi[16] = {};
    const Ubiquitty::ColorRGBA plain { 0, 0, 0, 0xFF };

    // The injection the escape exists to stop: close the string, close the line,
    // open a section. Every one of those characters has to come back escaped.
    const std::string hostile = "evil\"\n[[themes]]\nname = \"pwned";
    const std::string block = renderThemeBlock(hostile, /*isDark*/ true, ansi, plain, plain, plain);

    // The payload's own "[[themes]]" text is still in the file — escaped, between
    // the quotes — so the question is never whether those characters appear but
    // whether any of them BEGINS a line. Exactly one header, exactly one key.
    CHECK(countLinesBeginningWith(block, "[[themes]]") == 1);
    CHECK(countLinesBeginningWith(block, "name = ") == 1);

    // And the escapes are the ones the reader decodes.
    CHECK(block.find("evil\\\"\\n[[themes]]\\nname = \\\"pwned") != std::string::npos);

    // An ordinary name is untouched — the escape must not disturb the common case.
    // The escape set itself is pinned where it lives, in testEscapeTOMLBasicString;
    // what this case owns is that the name reaches it at all.
    const std::string ordinary = renderThemeBlock("Solarized Dark", /*isDark*/ true, ansi, plain, plain, plain);
    CHECK(ordinary.find("name = \"Solarized Dark\"\n") != std::string::npos);
}

UBIQUITTY_TEST_MAIN()
