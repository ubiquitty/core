// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// An app version as a value: what its text is made of, how two are ordered,
// and the one integer a bundle states it as.
//
// A named version is `MAJOR.MINOR.PATCH`. A build between two named versions
// is `MAJOR.MINOR.PATCH.N`, where N counts the commits it has that the named
// version lacks; a named version's own N is 0 and is not written. The text is
// canonical: decimal digits, no leading zeros, and never a written `.0` count,
// so one version has one spelling.
//
// The build derives the text from the commit (Scripts/cmake/BuildVersion.cmake)
// under the same bounds, and a peer states its own in the handshake. Ordering
// versions is this header's alone: a frontend that needs to know which of two
// is newer asks ``compareAppVersions`` and never compares the text.
//
// Which of two builds may talk is not a question for these numbers. The wire
// range answers it (WireVersion.h), so an app version only names a build to a
// person.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Ubiquitty::Protocol::Sync {

/// The most each field of an app version holds.
///
/// They size the decimal fields of ``bundleVersionNumber``, so widening one
/// moves the others' positions.
inline constexpr std::uint32_t maxAppVersionMajor = 999;
inline constexpr std::uint32_t maxAppVersionMinor = 99;
inline constexpr std::uint32_t maxAppVersionPatch = 99;
inline constexpr std::uint32_t maxAppVersionCommits = 99'999;

/// The longest canonical app version, `999.99.99.99999`.
inline constexpr std::size_t maxCanonicalAppVersionBytes = 15;

/// An app version's fields, ordered as versions are: by named version, then by
/// the commits past it.
struct AppVersion {
    std::uint32_t major { 0 };
    std::uint32_t minor { 0 };
    std::uint32_t patch { 0 };
    /// The commits this build has that the named version lacks; 0 for the
    /// named version itself.
    std::uint32_t commits { 0 };

    [[nodiscard]] friend constexpr auto operator<=>(const AppVersion&, const AppVersion&) = default;
};

/// Reads a canonical app version.
///
/// - Parameter text: The candidate, from a build or a peer.
/// - Returns: The version, or nothing when the text is not canonical: another
///   field count than three or four, an empty field, a character that is not
///   a decimal digit, a leading zero, a written count of 0, or a field above
///   its maximum.
[[nodiscard]] constexpr std::optional<AppVersion> parseAppVersion(std::string_view text)
{
    if (text.empty() || text.size() > maxCanonicalAppVersionBytes)
        return std::nullopt;
    constexpr std::array<std::uint32_t, 4> maxima { maxAppVersionMajor, maxAppVersionMinor, maxAppVersionPatch, maxAppVersionCommits };
    std::array<std::uint32_t, 4> fields { 0, 0, 0, 0 };
    std::size_t fieldCount = 0;
    std::size_t offset = 0;
    while (true) {
        if (fieldCount == fields.size())
            return std::nullopt;
        const std::size_t start = offset;
        std::uint32_t value = 0;
        while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') {
            value = value * 10 + static_cast<std::uint32_t>(text[offset] - '0');
            // The longest field is five digits, so the value cannot wrap
            // before this refuses it.
            if (value > maxima[fieldCount])
                return std::nullopt;
            ++offset;
        }
        const std::size_t digitCount = offset - start;
        if (digitCount == 0 || (digitCount > 1 && text[start] == '0'))
            return std::nullopt;
        fields[fieldCount++] = value;
        if (offset == text.size())
            break;
        if (text[offset] != '.')
            return std::nullopt;
        ++offset;
    }
    if (fieldCount < 3 || (fieldCount == 4 && fields[3] == 0))
        return std::nullopt;
    return AppVersion { .major = fields[0], .minor = fields[1], .patch = fields[2], .commits = fields[3] };
}

/// Orders two app versions by their text.
///
/// - Parameters:
///   - left: One version's text.
///   - right: The other's.
/// - Returns: How `left` orders against `right`, or nothing when either is
///   not a canonical version: a build that states none, or one from before
///   versions had this form, is not older or newer than anything.
[[nodiscard]] constexpr std::optional<std::strong_ordering> compareAppVersions(std::string_view left, std::string_view right)
{
    const std::optional<AppVersion> leftVersion = parseAppVersion(left);
    const std::optional<AppVersion> rightVersion = parseAppVersion(right);
    if (!leftVersion || !rightVersion)
        return std::nullopt;
    return *leftVersion <=> *rightVersion;
}

/// The one integer a bundle states a version as.
///
/// A system and an updater compare a bundle's build number as an integer, and
/// read at most three dotted fields of it, so the four fields are packed into
/// decimal positions, widest first: two digits each for the minor and patch,
/// five for the count. A later version is a larger integer whichever field
/// moved, and a named version's integer is below every build counted from it.
///
/// - Parameter version: The version.
/// - Returns: `major * 10^9 + minor * 10^7 + patch * 10^5 + commits`.
[[nodiscard]] constexpr std::uint64_t bundleVersionNumber(const AppVersion& version)
{
    return static_cast<std::uint64_t>(version.major) * 1'000'000'000 + static_cast<std::uint64_t>(version.minor) * 10'000'000
        + static_cast<std::uint64_t>(version.patch) * 100'000 + version.commits;
}

} // namespace Ubiquitty::Protocol::Sync
