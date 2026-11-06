// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Themes/ThemeImport.h"
#include "Core/Color.h"
#include "Core/Preferences/Preferences.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>

namespace Ubiquitty::Themes {

namespace {

// Each of these is measured rather than counted: the length appeared as a bare
// 5, 6 and 7 beside the literal it belonged to, so editing a tag left the skip
// naming a different number of characters than the tag has.
constexpr std::string_view dictOpenTag = "<dict>";
constexpr std::string_view dictCloseTag = "</dict>";
constexpr std::string_view ansiKeyPrefix = "Ansi ";

uint8_t floatToByte(double v)
{
    if (v < 0.0)
        v = 0.0;
    if (v > 1.0)
        v = 1.0;
    return static_cast<uint8_t>(v * 255.0 + 0.5);
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

// Parse a #rrggbb / rrggbb / #rgb color string. Returns the color on
// success, nullopt on failure. Leading/trailing whitespace and a leading '#' are
// tolerated.
std::optional<ColorRGBA> parseHexColorString(std::string_view s)
{
    std::size_t start = 0, end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
        start++;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        end--;
    std::string_view t = s.substr(start, end - start);
    if (!t.empty() && t[0] == '#')
        t.remove_prefix(1);
    std::array<int, 8> v {};
    if (t.size() != 3 && t.size() != 6 && t.size() != 8)
        return std::nullopt;
    for (std::size_t i = 0; i < t.size(); ++i) {
        int d = hexDigit(t[i]);
        if (d < 0)
            return std::nullopt;
        v[i] = d;
    }
    ColorRGBA out;
    if (t.size() == 3) {
        // #rgb shorthand: each nibble doubled.
        out.red = static_cast<uint8_t>((v[0] << 4) | v[0]);
        out.green = static_cast<uint8_t>((v[1] << 4) | v[1]);
        out.blue = static_cast<uint8_t>((v[2] << 4) | v[2]);
        out.alpha = 255;
        return out;
    }
    out.red = static_cast<uint8_t>((v[0] << 4) | v[1]);
    out.green = static_cast<uint8_t>((v[2] << 4) | v[3]);
    out.blue = static_cast<uint8_t>((v[4] << 4) | v[5]);
    out.alpha = t.size() == 8 ? static_cast<uint8_t>((v[6] << 4) | v[7]) : 255;
    return out;
}

// iTerm2 .itermcolors (XML plist).

// Find the text between the first <tag>…</tag> at or after `from`. Returns the
// inner text and advances `from` past the closing tag. Returns false at EOF.
bool nextTag(std::string_view xml, std::size_t& from, std::string_view tag, std::string_view& inner)
{
    std::string open = "<" + std::string(tag) + ">";
    std::string close = "</" + std::string(tag) + ">";
    std::size_t o = xml.find(open, from);
    if (o == std::string_view::npos)
        return false;
    std::size_t textStart = o + open.size();
    std::size_t c = xml.find(close, textStart);
    if (c == std::string_view::npos)
        return false;
    inner = xml.substr(textStart, c - textStart);
    from = c + close.size();
    return true;
}

// Read a <dict>…</dict> of channel keys starting at `dictStart` (just past a
// <dict>). Extracts Red/Green/Blue (and optional Alpha) Component reals.
ColorRGBA parseColorDict(std::string_view xml, std::size_t dictStart, std::size_t dictEnd)
{
    ColorRGBA c { 0, 0, 0, 255 };
    std::string_view body = xml.substr(dictStart, dictEnd - dictStart);
    auto readComponent = [&](std::string_view component, uint8_t& dst) {
        std::string keyTag = "<key>" + std::string(component) + "</key>";
        std::size_t k = body.find(keyTag);
        if (k == std::string_view::npos)
            return;
        std::size_t from = k + keyTag.size();
        std::string_view value;
        // The value element is <real>…</real> (occasionally <integer>).
        std::size_t save = from;
        if (nextTag(body, from, "real", value) || (from = save, nextTag(body, from, "integer", value))) {
            dst = floatToByte(std::strtod(std::string(value).c_str(), nullptr));
        }
    };
    readComponent("Red Component", c.red);
    readComponent("Green Component", c.green);
    readComponent("Blue Component", c.blue);
    // Alpha Component is rarely present; default opaque.
    return c;
}

ImportedTheme parseITermColors(std::string_view xml)
{
    ImportedTheme out;
    out.format = ImportFormat::ITermColors;

    // The top-level <dict> alternates <key>SLOT</key><dict>…color…</dict>.
    // Walk every <key> and, when it names a known slot, parse the immediately
    // following <dict>.
    std::size_t pos = 0;
    int ansiFound = 0;
    while (pos < xml.size()) {
        std::string_view key;
        std::size_t keyScan = pos;
        if (!nextTag(xml, keyScan, "key", key))
            break;
        pos = keyScan;
        // The color value is the next <dict> after this key.
        std::size_t dictOpen = xml.find(dictOpenTag, pos);
        if (dictOpen == std::string_view::npos)
            break;
        std::size_t dictClose = xml.find(dictCloseTag, dictOpen);
        if (dictClose == std::string_view::npos)
            break;
        std::size_t dictStart = dictOpen + dictOpenTag.size();

        ColorRGBA color = parseColorDict(xml, dictStart, dictClose);

        // Match "Ansi N Color".
        if (key.starts_with(ansiKeyPrefix) && key.size() > ansiKeyPrefix.size()) {
            std::string_view rest = key.substr(ansiKeyPrefix.size());
            int index = 0;
            std::size_t i = 0;
            bool sawDigit = false;
            while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') {
                index = index * 10 + (rest[i] - '0');
                i++;
                sawDigit = true;
            }
            if (sawDigit && index >= 0 && index < 16) {
                out.ansi[index] = color;
                if (!out.ansiSet[index])
                    ansiFound++;
                out.ansiSet[index] = true;
            }
        } else if (key == "Foreground Color") {
            out.foreground = color;
            out.foregroundSet = true;
        } else if (key == "Background Color") {
            out.background = color;
            out.backgroundSet = true;
        } else if (key == "Cursor Color") {
            out.cursor = color;
            out.cursorSet = true;
        }
        // Advance past this color dict so the next <key> scan starts cleanly.
        pos = dictClose + dictCloseTag.size();
    }

    out.ok = (ansiFound == 16);
    return out;
}

// Ghostty theme (key = value).

std::string_view trim(std::string_view s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        b--;
    return s.substr(a, b - a);
}

ImportedTheme parseGhostty(std::string_view text)
{
    ImportedTheme out;
    out.format = ImportFormat::Ghostty;
    int ansiFound = 0;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, (nl == std::string_view::npos ? text.size() : nl) - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        std::string_view key = trim(line.substr(0, eq));
        std::string_view value = trim(line.substr(eq + 1));

        if (key == "palette") {
            // value is "N=#rrggbb".
            std::size_t inner = value.find('=');
            if (inner == std::string_view::npos)
                continue;
            std::string_view indexText = trim(value.substr(0, inner));
            std::string_view colorText = trim(value.substr(inner + 1));
            int index = 0;
            bool sawDigit = false;
            for (char c : indexText) {
                if (c < '0' || c > '9') {
                    sawDigit = false;
                    break;
                }
                index = index * 10 + (c - '0');
                sawDigit = true;
            }
            if (sawDigit && index >= 0 && index < 16) {
                if (auto color = parseHexColorString(colorText)) {
                    out.ansi[index] = *color;
                    if (!out.ansiSet[index])
                        ansiFound++;
                    out.ansiSet[index] = true;
                }
            }
        } else if (key == "foreground") {
            if (auto color = parseHexColorString(value)) {
                out.foreground = *color;
                out.foregroundSet = true;
            }
        } else if (key == "background") {
            if (auto color = parseHexColorString(value)) {
                out.background = *color;
                out.backgroundSet = true;
            }
        } else if (key == "cursor-color") {
            if (auto color = parseHexColorString(value)) {
                out.cursor = *color;
                out.cursorSet = true;
            }
        }
    }

    out.ok = (ansiFound == 16);
    return out;
}

ImportedTheme parseUbiquitty(std::string_view bytes)
{
    Preferences::Values configuration;
    std::istringstream input { std::string(bytes) };
    ImportedTheme result;
    if (!Preferences::parse(input, configuration, Preferences::ParseMode::Strict) || configuration.themes.size() != 1) {
        return result;
    }
    const auto& theme = configuration.themes.front();
    if (theme.name.empty() || !theme.isDark || !theme.foreground.set || !theme.background.set || !theme.cursor.set) {
        return result;
    }
    const auto color = [](const Preferences::ColorEntry& entry) {
        return ColorRGBA { entry.red, entry.green, entry.blue, entry.alpha };
    };
    for (int index = 0; index < 16; ++index) {
        if (!theme.ansi[index].set) {
            return result;
        }
        result.ansi[index] = color(theme.ansi[index]);
        result.ansiSet[index] = true;
    }
    result.foreground = color(theme.foreground);
    result.background = color(theme.background);
    result.cursor = color(theme.cursor);
    result.foregroundSet = result.backgroundSet = result.cursorSet = true;
    result.name = theme.name;
    result.isDark = theme.isDark;
    result.format = ImportFormat::Ubiquitty;
    result.ok = true;
    return result;
}

ImportFormat detect(std::string_view bytes)
{
    if (bytes.find("[[themes]]") != std::string_view::npos) {
        return ImportFormat::Ubiquitty;
    }
    // A plist starts (after optional whitespace / XML decl) with a DOCTYPE or
    // <plist>; iTerm2 files are always XML plists.
    if (bytes.find("<plist") != std::string_view::npos || bytes.find("Ansi 0 Color") != std::string_view::npos)
        return ImportFormat::ITermColors;
    if (bytes.find("palette") != std::string_view::npos || bytes.find("background") != std::string_view::npos)
        return ImportFormat::Ghostty;
    return ImportFormat::Unknown;
}

} // namespace

ImportedTheme importThemeFromBytes(std::string_view bytes, ImportFormat format)
{
    if (format == ImportFormat::Unknown)
        format = detect(bytes);
    switch (format) {
    case ImportFormat::Ubiquitty:
        return parseUbiquitty(bytes);
    case ImportFormat::ITermColors:
        return parseITermColors(bytes);
    case ImportFormat::Ghostty:
        return parseGhostty(bytes);
    default:
        return ImportedTheme {};
    }
}

bool importedThemeIsDark(const ImportedTheme& theme)
{
    if (theme.isDark) {
        return *theme.isDark;
    }
    // Rec. 709 luma, matching Themes::isDark / prefRGBIsDark.
    const ColorRGBA& b = theme.background;
    double luma = Ubiquitty::perceivedLightness(b);
    return luma < 128.0;
}

} // namespace Ubiquitty::Themes
