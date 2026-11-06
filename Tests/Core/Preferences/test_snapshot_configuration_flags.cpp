// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Host tests for Source/Core/Preferences/SnapshotConfigurationFlags.h — the
// snapshot flags that state configuration keys, written into a case's
// document by every software driver.
//
// Pins:
//   - each flag lands as its own key, and the fold reads it back;
//   - the tube's colours are written only under `--tek-color-mode 1`;
//   - a missing or malformed value is refused rather than dropped;
//   - a word that is not one of these flags is left to the driver.

#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/SnapshotConfigurationFlags.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Ubiquitty::Preferences::parseSnapshotConfigurationFlag;
using Ubiquitty::Preferences::SnapshotConfigurationFlags;
using Ubiquitty::Preferences::SnapshotFlagParse;
using Ubiquitty::Preferences::stateSnapshotConfigurationFlags;
using Ubiquitty::Preferences::stateSnapshotConfigurationWords;

// Parses a whole command line the way a driver's loop does.
SnapshotFlagParse parseWords(const std::vector<const char*>& words, SnapshotConfigurationFlags& flags)
{
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto next = [&]() -> const char* { return index + 1 < words.size() ? words[++index] : nullptr; };
        const SnapshotFlagParse parsed = parseSnapshotConfigurationFlag(words[index], next, flags);
        if (parsed != SnapshotFlagParse::Consumed)
            return parsed;
    }
    return SnapshotFlagParse::Consumed;
}

// Configures from a document and returns the fold's values.
const Ubiquitty::Preferences::Values& configured(const std::string& document)
{
    const Ubiquitty::Preferences::ConfigureResult result = Ubiquitty::Preferences::configure(document, Ubiquitty::Preferences::ParseMode::Strict);
    CHECK(result.isApplied);
    return Ubiquitty::Preferences::values();
}

} // namespace

TEST(eachFlagLandsAsItsOwnKey)
{
    SnapshotConfigurationFlags flags;
    REQUIRE(parseWords({ "--bg-opacity", "0.5", "--padding-x", "40", "--padding-y", "30", "--tek-glow" }, flags) == SnapshotFlagParse::Consumed);
    const std::optional<std::string> document = stateSnapshotConfigurationFlags(flags, "[font]\nsize = 10\n");
    REQUIRE(document.has_value());
    const Ubiquitty::Preferences::Values& values = configured(*document);
    CHECK(values.backgroundOpacity && *values.backgroundOpacity == 0.5);
    const Ubiquitty::Preferences::Values::PaddingEdges padding = values.resolvedPadding();
    CHECK(padding.left == 40 && padding.right == 40 && padding.top == 30 && padding.bottom == 30);
    CHECK(values.tekGlowVectors == true);
    CHECK(values.fontSize && *values.fontSize == 10.0);
}

TEST(theTubeColoursAreWrittenOnlyUnderTheCustomMode)
{
    SnapshotConfigurationFlags alone;
    REQUIRE(parseWords({ "--tek-bg", "#802040" }, alone) == SnapshotFlagParse::Consumed);
    const std::optional<std::string> ignored = stateSnapshotConfigurationFlags(alone, "");
    REQUIRE(ignored.has_value());
    CHECK(ignored->find("background_color") == std::string::npos);

    SnapshotConfigurationFlags custom;
    REQUIRE(parseWords({ "--tek-color-mode", "1", "--tek-bg", "802040" }, custom) == SnapshotFlagParse::Consumed);
    const std::optional<std::string> written = stateSnapshotConfigurationFlags(custom, "");
    REQUIRE(written.has_value());
    CHECK(written->find("#802040") != std::string::npos);
    CHECK(written->find("#33ff66") != std::string::npos);
}

TEST(aMissingOrMalformedValueIsRefused)
{
    SnapshotConfigurationFlags flags;
    CHECK(parseWords({ "--bg-opacity" }, flags) == SnapshotFlagParse::Malformed);
    CHECK(parseWords({ "--bg-opacity", "half" }, flags) == SnapshotFlagParse::Malformed);
    CHECK(parseWords({ "--tek-bg", "#80204" }, flags) == SnapshotFlagParse::Malformed);
    CHECK(parseWords({ "--bg-image-mode", "sideways" }, flags) == SnapshotFlagParse::Malformed);
    CHECK(parseWords({ "--tek-color-mode", "2" }, flags) == SnapshotFlagParse::Malformed);
}

TEST(aWordThatIsNoConfigurationFlagIsLeftToTheDriver)
{
    SnapshotConfigurationFlags flags;
    CHECK(parseWords({ "--select-rect", "0,0,1,1" }, flags) == SnapshotFlagParse::NotConfiguration);
}

TEST(aLaunchsWordsStateTheirConfigurationFlagsAndLeaveTheRest)
{
    // The iOS launch's own arguments around the configuration flags: the
    // other words pass over, the flags land in the document.
    const std::vector<std::string> words { "--ios-snapshot", "--rows", "4", "--bg-opacity", "0.5", "--select-rect", "0,0,1,1",
        "--tek-glow", "--scale", "2" };
    const std::optional<std::string> stated = stateSnapshotConfigurationWords(words, "");
    REQUIRE(stated.has_value());
    CHECK(stated->find("background_opacity") != std::string::npos);
    CHECK(stated->find("glow_vectors") != std::string::npos);

    CHECK(!stateSnapshotConfigurationWords({ "--bg-opacity", "half" }, "").has_value());
    CHECK(!stateSnapshotConfigurationWords({ "--bg-opacity" }, "").has_value());
}

UBIQUITTY_TEST_MAIN()
