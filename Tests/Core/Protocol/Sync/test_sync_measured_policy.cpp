// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_measured_policy — the session layer's constants in code are the
// ones Docs/sync-measurements.md records, "Input and liveness".
// The table there is read here, so neither can move without the
// other (decision D15: no constant is written into code before it is
// measured or recorded as provisional with its reason).

#include "Harness.h"

#include "Core/Protocol/Sync/Connection.h"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

using namespace Ubiquitty::Protocol::Sync;

namespace {

/// The section's constants table: name to value, in microseconds for a time
/// and as written for a multiple.
std::map<std::string, std::uint64_t> recordedConstants()
{
    std::ifstream file(UBQ_MEASUREMENTS_DOC);
    std::stringstream contents;
    contents << file.rdbuf();
    const std::string text = contents.str();
    std::map<std::string, std::uint64_t> constants;
    const std::size_t section = text.find("\n## Input and liveness\n");
    if (section == std::string::npos)
        return constants;
    const std::size_t table = text.find("\n### The constants\n", section);
    if (table == std::string::npos)
        return constants;
    std::istringstream lines(text.substr(table));
    std::string line;
    std::getline(lines, line);
    while (std::getline(lines, line)) {
        if (line.starts_with("## "))
            break;
        // | `name` | value unit | source |
        if (!line.starts_with("| `"))
            continue;
        const std::size_t nameEnd = line.find('`', 3);
        const std::size_t valueStart = line.find("| ", nameEnd) + 2;
        const std::size_t valueEnd = line.find(" |", valueStart);
        if (nameEnd == std::string::npos || valueEnd == std::string::npos)
            continue;
        const std::string name = line.substr(3, nameEnd - 3);
        const std::string_view value = std::string_view(line).substr(valueStart, valueEnd - valueStart);
        std::uint64_t number = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc {})
            continue;
        const std::string_view unit = std::string_view(end, value.data() + value.size());
        if (unit == " ms")
            number *= 1'000;
        else if (unit == " s")
            number *= 1'000'000;
        else if (unit == " min")
            number *= 60'000'000;
        else if (unit == " KiB")
            number *= 1'024;
        else if (unit != "×")
            continue;
        constants[name] = number;
    }
    return constants;
}

std::optional<std::uint64_t> recorded(const std::map<std::string, std::uint64_t>& constants, const std::string& name)
{
    const auto found = constants.find(name);
    CHECK_MSG(found != constants.end(), ("the measurements record no constant named " + name).c_str());
    return found == constants.end() ? std::nullopt : std::optional { found->second };
}

} // namespace

TEST(everyMeasuredConstantMatchesItsRecord)
{
    const std::map<std::string, std::uint64_t> constants = recordedConstants();
    CHECK_EQ(measuredInputPolicy().echoSettleMicroseconds, recorded(constants, "E").value_or(0));
    CHECK_EQ(measuredInputPolicy().receiptDelayMicroseconds, recorded(constants, "receipt delay").value_or(0));
    CHECK_EQ(measuredInputPolicy().pasteWindowBytes, recorded(constants, "paste window").value_or(0));
    CHECK_EQ(measuredLivenessPolicy().roundTripMultiple, recorded(constants, "stall multiple").value_or(0));
    CHECK_EQ(measuredLivenessPolicy().floorMicroseconds, recorded(constants, "stall floor").value_or(0));
    CHECK_EQ(measuredLivenessPolicy().repathMicroseconds, recorded(constants, "re-path timer").value_or(0));
    CHECK_EQ(measuredResumePolicy().lostGraceMicroseconds, recorded(constants, "Lost grace").value_or(0));
    CHECK_EQ(measuredGeometryPolicy().coalesceMicroseconds, recorded(constants, "geometry coalesce").value_or(0));
    // The silence rule is held in the pacing policy; the table
    // records it beside the rest so a reader finds every bound in one place.
    CHECK_EQ(measuredHostPolicy(0).pacing.lostSilenceMicroseconds, recorded(constants, "silence").value_or(0));
    // A host carries its input policy.
    CHECK_EQ(measuredHostPolicy(0).input.echoSettleMicroseconds, measuredInputPolicy().echoSettleMicroseconds);
}

UBIQUITTY_TEST_MAIN()
