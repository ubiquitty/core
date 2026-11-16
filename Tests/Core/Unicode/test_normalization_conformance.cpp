// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Unicode/Normalize.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef NORMALIZATION_TEST_CORPUS
#error "NORMALIZATION_TEST_CORPUS must name the vendored NormalizationTest.txt"
#endif

using Ubiquitty::Unicode::NormalizationForm;
using Ubiquitty::Unicode::normalizeUTF8;

TEST(everyOfficialNormalizationCaseAndUnlistedScalarConforms)
{
    std::ifstream input(NORMALIZATION_TEST_CORPUS);
    REQUIRE(input.good());
    std::vector<bool> listed(0x110000);
    std::string line;
    bool isPartOne = false;
    std::size_t cases = 0;
    std::size_t mismatches = 0;
    const auto check = [&](std::string_view text, NormalizationForm form, const std::string& expected) {
        if (normalizeUTF8(text, form) == expected)
            return;
        if (mismatches < 10)
            std::printf("normalization mismatch at corpus case %zu, form %u\n", cases, static_cast<unsigned>(form));
        ++mismatches;
    };
    while (std::getline(input, line)) {
        line.resize(line.find('#') == std::string::npos ? line.size() : line.find('#'));
        if (line.empty())
            continue;
        if (line.front() == '@') {
            isPartOne = line.starts_with("@Part1");
            continue;
        }
        std::array<std::string, 5> columns;
        std::istringstream fields(line);
        for (std::size_t column = 0; column < columns.size(); ++column) {
            std::string field;
            REQUIRE(static_cast<bool>(std::getline(fields, field, ';')));
            std::istringstream scalars(field);
            std::uint32_t codePoint = 0;
            while (scalars >> std::hex >> codePoint) {
                REQUIRE(Ubiquitty::Unicode::isScalarValue(codePoint));
                Ubiquitty::Unicode::appendUTF8(columns[column], codePoint);
                if (isPartOne && column == 0)
                    listed[codePoint] = true;
            }
        }
        for (std::size_t column = 0; column < columns.size(); ++column) {
            check(columns[column], NormalizationForm::NFC, columns[column < 3 ? 1 : 3]);
            check(columns[column], NormalizationForm::NFD, columns[column < 3 ? 2 : 4]);
            check(columns[column], NormalizationForm::NFKC, columns[3]);
            check(columns[column], NormalizationForm::NFKD, columns[4]);
        }
        ++cases;
    }
    REQUIRE(cases > 19000);
    CHECK_EQ(mismatches, 0u);

    // Part 1 specifies that every scalar it does not list normalizes to itself.
    for (std::uint32_t codePoint = 0; codePoint <= 0x10FFFF; ++codePoint) {
        if (listed[codePoint] || !Ubiquitty::Unicode::isScalarValue(codePoint))
            continue;
        std::string encoded;
        Ubiquitty::Unicode::appendUTF8(encoded, codePoint);
        for (const NormalizationForm form : { NormalizationForm::NFC, NormalizationForm::NFD, NormalizationForm::NFKC, NormalizationForm::NFKD })
            check(encoded, form, encoded);
    }
    std::printf("normalization: %zu official cases, %zu mismatches including unlisted scalars\n", cases, mismatches);
    CHECK_EQ(mismatches, 0u);
}

UBIQUITTY_TEST_MAIN()
