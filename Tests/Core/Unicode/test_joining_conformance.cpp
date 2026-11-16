// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Unicode/Joining.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef DERIVED_JOINING_TYPE_CORPUS
#error "DERIVED_JOINING_TYPE_CORPUS must name the vendored DerivedJoiningType.txt"
#endif
#ifndef DERIVED_BIDI_CLASS_CORPUS
#error "DERIVED_BIDI_CLASS_CORPUS must name the vendored DerivedBidiClass.txt"
#endif

namespace {

template<typename Callback>
bool readRanges(const char* path, Callback callback)
{
    std::ifstream input(path);
    if (!input.good())
        return false;
    std::vector<std::string> explicitLines;
    std::string line;
    const auto read = [&](std::string_view body) {
        const std::size_t semicolon = body.find(';');
        if (semicolon == std::string_view::npos)
            return;
        const std::string interval(body.substr(0, semicolon));
        const std::size_t dots = interval.find("..");
        std::uint32_t first = 0;
        std::istringstream(interval) >> std::hex >> first;
        std::uint32_t last = first;
        if (dots != std::string::npos)
            std::istringstream(interval.substr(dots + 2)) >> std::hex >> last;
        std::string value;
        std::istringstream(std::string(body.substr(semicolon + 1))) >> value;
        callback(first, last, value);
    };
    while (std::getline(input, line)) {
        if (line.starts_with("# @missing:"))
            read(std::string_view(line).substr(11));
        else if (!line.empty() && line.front() != '#')
            explicitLines.push_back(line);
    }
    for (const std::string& explicitLine : explicitLines)
        read(explicitLine);
    return true;
}

} // namespace

TEST(joiningTypesMatchTheOfficialDataIncludingUnlistedDefaults)
{
    using Ubiquitty::Unicode::JoiningType;
    std::vector<JoiningType> expected(0x110000, JoiningType::NonJoining);
    REQUIRE(readRanges(DERIVED_JOINING_TYPE_CORPUS, [&](std::uint32_t first, std::uint32_t last, const std::string& value) {
        JoiningType type = JoiningType::NonJoining;
        if (value == "T")
            type = JoiningType::Transparent;
        else if (value == "R")
            type = JoiningType::RightJoining;
        else if (value == "L")
            type = JoiningType::LeftJoining;
        else if (value == "D")
            type = JoiningType::DualJoining;
        else if (value == "C")
            type = JoiningType::JoinCausing;
        for (std::uint32_t codePoint = first; codePoint <= last; ++codePoint)
            expected[codePoint] = type;
    }));
    std::size_t mismatches = 0;
    for (std::uint32_t codePoint = 0; codePoint < expected.size(); ++codePoint) {
        if (Ubiquitty::Unicode::joiningType(codePoint) != expected[codePoint]) {
            if (mismatches < 10)
                std::printf("joining mismatch U+%04X\n", codePoint);
            ++mismatches;
        }
    }
    CHECK_EQ(mismatches, 0u);
}

TEST(strongRTLMatchesTheOfficialRAndALPropertiesIncludingMissingDefaults)
{
    std::vector<bool> expected(0x110000);
    REQUIRE(readRanges(DERIVED_BIDI_CLASS_CORPUS, [&](std::uint32_t first, std::uint32_t last, const std::string& value) {
        const bool isStrong = value == "R" || value == "AL" || value == "Right_To_Left" || value == "Arabic_Letter";
        for (std::uint32_t codePoint = first; codePoint <= last; ++codePoint)
            expected[codePoint] = isStrong;
    }));
    std::size_t mismatches = 0;
    for (std::uint32_t codePoint = 0; codePoint < expected.size(); ++codePoint) {
        if (Ubiquitty::Unicode::isStrongRTL(codePoint) != expected[codePoint]) {
            if (mismatches < 10)
                std::printf("bidi property mismatch U+%04X\n", codePoint);
            ++mismatches;
        }
    }
    CHECK_EQ(mismatches, 0u);
}

UBIQUITTY_TEST_MAIN()
