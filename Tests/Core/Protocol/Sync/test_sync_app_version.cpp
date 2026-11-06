// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_app_version.cpp — what an app version's text is, how two order,
// and the integer a bundle states one as.
//
// - A version is three fields, or four when the build is past a named
//   version; the text is canonical, so anything else reads as no version.
// - Versions order by named version first, then by the commits past it, and
//   never by their text: `0.2.0.10` is after `0.2.0.9`.
// - A text that is not a version orders against nothing.
// - The bundle integer orders exactly as the versions do, across every field
//   boundary, and a named version's integer is below each build counted from
//   it.
// - Every canonical version fits the handshake's app-version field and passes
//   the reader a peer's bytes go through.

#include "Harness.h"

#include "Core/Protocol/Sync/AppVersion.h"
#include "Core/Protocol/Sync/WireVersion.h"

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

using namespace Ubiquitty::Protocol;
using Sync::AppVersion;
using Sync::bundleVersionNumber;
using Sync::compareAppVersions;
using Sync::parseAppVersion;

namespace {

/// Whether `text` reads as exactly `expected`.
bool reads(std::string_view text, AppVersion expected)
{
    const std::optional<AppVersion> version = parseAppVersion(text);
    return version && *version == expected;
}

} // namespace

TEST(aVersionIsThreeFieldsOrFourPastANamedVersion)
{
    CHECK(reads("0.2.0", { 0, 2, 0, 0 }));
    CHECK(reads("0.2.0.3", { 0, 2, 0, 3 }));
    CHECK(reads("12.34.56.7890", { 12, 34, 56, 7890 }));
    CHECK(reads("999.99.99.99999", { 999, 99, 99, 99'999 }));
    // The form the version had before commits were counted is still a version.
    CHECK(reads("0.9.1", { 0, 9, 1, 0 }));
}

TEST(aTextThatIsNotCanonicalIsNoVersion)
{
    constexpr std::array<std::string_view, 24> refused { {
        "",
        "0",
        "0.2",
        "0.2.0.3.1",
        "0.2.0.",
        ".2.0",
        "0..0",
        "0.2.0.0",
        "00.2.0",
        "0.02.0",
        "0.2.0.03",
        "v0.2.0",
        "0.2.0-3",
        "0.2.0+3",
        "0.2.0 ",
        " 0.2.0",
        "0.2.x",
        "0.2.0\n",
        "1000.0.0",
        "0.100.0",
        "0.0.100",
        "0.2.0.100000",
        "0.2.0.4294967297",
        "0.0.0-dev",
    } };
    for (const std::string_view text : refused)
        CHECK_MSG(!parseAppVersion(text), std::string(text).c_str());
    CHECK(!parseAppVersion(std::string_view("0.2.0\0", 6)));
    CHECK(!parseAppVersion(std::string(Sync::maxCanonicalAppVersionBytes + 1, '1')));
}

TEST(versionsOrderByNamedVersionThenByCommits)
{
    // Each row is later than the one before it.
    constexpr std::array<std::string_view, 11> ascending { {
        "0.1.99.99999",
        "0.2.0",
        "0.2.0.1",
        "0.2.0.9",
        "0.2.0.10",
        "0.2.0.99999",
        "0.2.1",
        "0.2.10",
        "0.10.0",
        "1.0.0",
        "10.0.0.1",
    } };
    for (std::size_t earlier = 0; earlier < ascending.size(); ++earlier) {
        for (std::size_t later = 0; later < ascending.size(); ++later) {
            const std::optional<std::strong_ordering> order = compareAppVersions(ascending[earlier], ascending[later]);
            REQUIRE(order);
            CHECK_MSG(*order == (earlier <=> later), (std::string(ascending[earlier]) + " against " + std::string(ascending[later])).c_str());
        }
    }
    // The text orders the other way: a comparison by characters puts 10 before 9.
    CHECK(std::string_view("0.2.0.10") < std::string_view("0.2.0.9"));
}

TEST(aTextThatIsNotAVersionOrdersAgainstNothing)
{
    CHECK(!compareAppVersions("0.2.0", ""));
    CHECK(!compareAppVersions("", "0.2.0"));
    CHECK(!compareAppVersions("0.0.0-dev", "0.2.0"));
    CHECK(!compareAppVersions("0.2.0", "1.0.0-beta.2+build.7"));
}

TEST(theBundleIntegerOrdersAsTheVersionsDo)
{
    CHECK_EQ(bundleVersionNumber(AppVersion { 0, 2, 0, 0 }), std::uint64_t(20'000'000));
    CHECK_EQ(bundleVersionNumber(AppVersion { 0, 2, 0, 3 }), std::uint64_t(20'000'003));
    CHECK_EQ(bundleVersionNumber(AppVersion { 0, 2, 1, 0 }), std::uint64_t(20'100'000));
    CHECK_EQ(bundleVersionNumber(AppVersion { 1, 0, 0, 0 }), std::uint64_t(1'000'000'000));
    CHECK_EQ(bundleVersionNumber(AppVersion { 999, 99, 99, 99'999 }), std::uint64_t(999'999'999'999));

    // The last build counted from one named version is below the next named
    // version, at each field's boundary.
    constexpr std::array<AppVersion, 8> ascending { {
        { 0, 2, 0, 0 },
        { 0, 2, 0, 99'999 },
        { 0, 2, 1, 0 },
        { 0, 2, 99, 99'999 },
        { 0, 3, 0, 0 },
        { 0, 99, 99, 99'999 },
        { 1, 0, 0, 0 },
        { 999, 99, 99, 99'999 },
    } };
    for (std::size_t index = 1; index < ascending.size(); ++index) {
        CHECK(ascending[index - 1] < ascending[index]);
        CHECK(bundleVersionNumber(ascending[index - 1]) < bundleVersionNumber(ascending[index]));
    }
}

TEST(everyCanonicalVersionFitsTheHandshakeField)
{
    static_assert(Sync::maxCanonicalAppVersionBytes <= Sync::maxAppVersionBytes);
    const std::string_view longest = "999.99.99.99999";
    CHECK_EQ(longest.size(), Sync::maxCanonicalAppVersionBytes);
    REQUIRE(parseAppVersion(longest));
    const std::string read = Sync::appVersionFromPeer({ reinterpret_cast<const std::uint8_t*>(longest.data()), longest.size() });
    CHECK(read == longest);
}

UBIQUITTY_TEST_MAIN()
