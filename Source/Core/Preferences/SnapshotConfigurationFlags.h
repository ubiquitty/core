// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/EffectiveValue.h"
#include "Core/Preferences/Key.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Rendering/BackgroundImageLayout.h"

#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Preferences {

/// The headless snapshot flags that state a configuration key on the command
/// line rather than steer the driver.
///
/// A driver writes them into the case's document before configuring from it,
/// so the one fold reads the opacity, the backdrop, the tube and the inset the
/// flag named. A driver that applied one beside the fold would draw a window
/// the configuration does not describe, and the arms would disagree about a
/// setting the case stated once.
struct SnapshotConfigurationFlags {
    std::optional<double> backgroundOpacity; // --bg-opacity <0.2..1>
    std::optional<std::string> backgroundImage; // --bg-image <path>
    double backgroundImageOpacity = 1.0; // --bg-image-opacity <0..1>
    Rendering::BackgroundImageMode backgroundImageMode = Rendering::BackgroundImageMode::Fill; // --bg-image-mode
    bool shouldGlowTekVectors = false; // --tek-glow
    // --tek-color-mode 1, with --tek-beam and --tek-bg read only under it:
    // the tube's custom colour path, which the macOS driver also writes whole.
    bool isTekColorModeCustom = false;
    std::uint32_t tekBeamColor = 0x33FF66u;
    std::uint32_t tekBackgroundColor = 0x0A0A0Au;
    std::optional<double> paddingX; // --padding-x <points>, left and right
    std::optional<double> paddingY; // --padding-y <points>, top and bottom
};

/// What ``parseSnapshotConfigurationFlag`` made of one word.
enum class SnapshotFlagParse : std::uint8_t {
    NotConfiguration, // the word is not one of these flags
    Consumed, // the flag and its value, if any, were read
    Malformed, // the flag's value was missing or did not parse
};

/// Parses an `RRGGBB` colour with an optional leading `#`.
[[nodiscard]] inline std::optional<std::uint32_t> parseSnapshotHexColor(std::string_view text)
{
    if (text.starts_with('#'))
        text.remove_prefix(1);
    if (text.size() != 6)
        return std::nullopt;
    std::uint32_t color = 0;
    for (const char character : text) {
        std::uint32_t digit = 0;
        if (character >= '0' && character <= '9')
            digit = static_cast<std::uint32_t>(character - '0');
        else if (character >= 'a' && character <= 'f')
            digit = static_cast<std::uint32_t>(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F')
            digit = static_cast<std::uint32_t>(character - 'A' + 10);
        else
            return std::nullopt;
        color = (color << 4) | digit;
    }
    return color;
}

/// Reads one configuration flag and its value.
///
/// - Parameters:
///   - flag: The word under the cursor.
///   - next: Returns the following word, consumed, or null at the line's end.
///   - flags: Receives the value.
/// - Returns: Whether the word was one of these flags, and whether it parsed.
template<typename NextWord>
[[nodiscard]] SnapshotFlagParse parseSnapshotConfigurationFlag(std::string_view flag, NextWord&& next, SnapshotConfigurationFlags& flags)
{
    const auto number = [&](auto& into) {
        const char* text = next();
        if (!text)
            return SnapshotFlagParse::Malformed;
        char* end = nullptr;
        const double value = std::strtod(text, &end);
        if (end == text || *end != '\0')
            return SnapshotFlagParse::Malformed;
        into = value;
        return SnapshotFlagParse::Consumed;
    };
    const auto color = [&](std::uint32_t& into) {
        const char* text = next();
        const std::optional<std::uint32_t> parsed = text ? parseSnapshotHexColor(text) : std::nullopt;
        if (!parsed)
            return SnapshotFlagParse::Malformed;
        into = *parsed;
        return SnapshotFlagParse::Consumed;
    };
    if (flag == "--bg-opacity")
        return number(flags.backgroundOpacity);
    if (flag == "--bg-image-opacity")
        return number(flags.backgroundImageOpacity);
    if (flag == "--padding-x")
        return number(flags.paddingX);
    if (flag == "--padding-y")
        return number(flags.paddingY);
    if (flag == "--tek-beam")
        return color(flags.tekBeamColor);
    if (flag == "--tek-bg")
        return color(flags.tekBackgroundColor);
    if (flag == "--tek-glow") {
        flags.shouldGlowTekVectors = true;
        return SnapshotFlagParse::Consumed;
    }
    if (flag == "--bg-image") {
        const char* path = next();
        if (!path || !*path)
            return SnapshotFlagParse::Malformed;
        flags.backgroundImage = path;
        return SnapshotFlagParse::Consumed;
    }
    if (flag == "--bg-image-mode") {
        const char* token = next();
        const std::optional<Rendering::BackgroundImageMode> mode = token ? Preferences::backgroundImageModeFromString(token) : std::nullopt;
        if (!mode)
            return SnapshotFlagParse::Malformed;
        flags.backgroundImageMode = *mode;
        return SnapshotFlagParse::Consumed;
    }
    if (flag == "--tek-color-mode") {
        const char* text = next();
        if (!text || (std::string_view(text) != "0" && std::string_view(text) != "1"))
            return SnapshotFlagParse::Malformed;
        flags.isTekColorModeCustom = std::string_view(text) == "1";
        return SnapshotFlagParse::Consumed;
    }
    return SnapshotFlagParse::NotConfiguration;
}

/// Writes the flags into a configuration document as its own keys.
///
/// - Parameters:
///   - flags: What the command line stated.
///   - document: The case's document, empty when it named none.
/// - Returns: The document with every stated key written, or nothing when a
///   key refused its value.
[[nodiscard]] inline std::optional<std::string> stateSnapshotConfigurationFlags(const SnapshotConfigurationFlags& flags, std::string document)
{
    using Preferences::Key;
    using Preferences::SerializeKind;
    using Preferences::Value;
    bool didWriteAll = true;
    const auto state = [&](Key key, const Value& value) {
        std::optional<std::string> written = Preferences::writeValueIntoBody(key, value, document);
        if (written)
            document = std::move(*written);
        else
            didWriteAll = false;
    };
    const auto hex = [](std::uint32_t color) { return std::format("#{:06x}", color & 0xFFFFFFu); };
    if (flags.backgroundOpacity)
        state(Key::AppearanceBackgroundOpacity, Value::ofNumber(SerializeKind::Double, *flags.backgroundOpacity));
    if (flags.backgroundImage) {
        state(Key::AppearanceBackgroundImage, Value::ofText(SerializeKind::String, *flags.backgroundImage));
        state(Key::AppearanceBackgroundImageOpacity, Value::ofNumber(SerializeKind::Double, flags.backgroundImageOpacity));
        state(Key::AppearanceBackgroundImageMode,
            Value::ofText(SerializeKind::EnumToken, Preferences::backgroundImageModeToString(flags.backgroundImageMode)));
    }
    if (flags.shouldGlowTekVectors)
        state(Key::TekGlowVectors, Value::ofBool(SerializeKind::Bool, true));
    if (flags.isTekColorModeCustom) {
        state(Key::TekColorMode, Value::ofInteger(SerializeKind::Int, 1));
        state(Key::TekBeamColor, Value::ofText(SerializeKind::String, hex(flags.tekBeamColor)));
        state(Key::TekBackgroundColor, Value::ofText(SerializeKind::String, hex(flags.tekBackgroundColor)));
    }
    if (flags.paddingX) {
        state(Key::AppearancePaddingLeft, Value::ofNumber(SerializeKind::Double, *flags.paddingX));
        state(Key::AppearancePaddingRight, Value::ofNumber(SerializeKind::Double, *flags.paddingX));
    }
    if (flags.paddingY) {
        state(Key::AppearancePaddingTop, Value::ofNumber(SerializeKind::Double, *flags.paddingY));
        state(Key::AppearancePaddingBottom, Value::ofNumber(SerializeKind::Double, *flags.paddingY));
    }
    if (!didWriteAll)
        return std::nullopt;
    return document;
}

/// A launch's arguments as words, named so Swift can build one.
using SnapshotLaunchWords = std::vector<std::string>;

/// Writes the configuration flags among a launch's words into a document.
///
/// For a driver that holds its command line as words rather than walking it
/// itself — the iOS shell, whose launch arguments reach Swift as strings. A
/// word that is not one of these flags is passed over, so the rest of the
/// command line rides along untouched; a flag whose value does not parse
/// refuses the whole document, as the other drivers refuse the case.
///
/// - Parameters:
///   - words: The launch arguments, in order.
///   - document: The case's document.
/// - Returns: The document with every stated key written, or nothing when a
///   flag's value did not parse or a key refused it.
[[nodiscard]] inline std::optional<std::string> stateSnapshotConfigurationWords(const SnapshotLaunchWords& words, std::string document)
{
    SnapshotConfigurationFlags flags;
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto next = [&]() -> const char* { return index + 1 < words.size() ? words[++index].c_str() : nullptr; };
        if (parseSnapshotConfigurationFlag(words[index], next, flags) == SnapshotFlagParse::Malformed)
            return std::nullopt;
    }
    return stateSnapshotConfigurationFlags(flags, std::move(document));
}

} // namespace Ubiquitty::Preferences
