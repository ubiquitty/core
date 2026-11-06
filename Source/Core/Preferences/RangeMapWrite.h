// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Writing `[[font.range_map]]` back to config.toml: the per-range font
// overrides the font route consults ahead of every fallback slot.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty::Preferences {

/// Formats a code-point range the way the reader spells one.
///
/// A single code point prints bare (`U+2318`) rather than as a degenerate
/// range, because that is what a person writing one by hand would write and
/// the file is read by people as well as by the parser.
///
/// - Parameters:
///   - firstCodePoint: The first code point in the range.
///   - lastCodePoint: The last, inclusive; equal to the first for one point.
/// - Returns: The range literal, without quotes.
[[nodiscard]] std::string formatCodePointRange(std::uint32_t firstCodePoint, std::uint32_t lastCodePoint);

/// Renders one `[[font.range_map]]` block, with a leading newline so it
/// appends cleanly after whatever precedes it.
///
/// Always `mode = "custom"`, because a written override names a family: the
/// other modes (auto, system, regional) describe a routing policy nobody picks
/// a font for, and the reader drops a custom entry whose family is empty.
///
/// - Parameters:
///   - firstCodePoint: The first code point the override covers.
///   - lastCodePoint: The last, inclusive.
///   - family: The family to route the range to.
///   - shouldMatchSize: The range sizing override, or unset to inherit the default.
/// - Returns: The block text.
[[nodiscard]] std::string renderRangeMapBlock(std::uint32_t firstCodePoint, std::uint32_t lastCodePoint, std::string_view family, std::optional<bool> shouldMatchSize = std::nullopt);

/// Routes `[firstCodePoint, lastCodePoint]` to `family`, rewriting `body`.
///
/// Last write wins, and an existing entry overlapping only part of the range is
/// split so the part this override does not cover keeps routing where it did. Refusing
/// the write would leave a person unable to correct a mistake, and replacing a
/// whole overlapping entry would silently discard routing they never asked to
/// lose; splitting is the only resolution that routes the named characters and
/// leaves every other character where the user already put it.
///
/// A block that does not overlap is not touched — not reparsed, not reformatted
/// and not moved — so comments and hand-authored spelling inside it survive.
///
/// - Parameters:
///   - body: The whole config file.
///   - firstCodePoint: The first code point to route.
///   - lastCodePoint: The last, inclusive.
///   - family: The family to route to.
///   - shouldMatchSize: The range sizing override, or unset to inherit the default.
/// - Returns: The rewritten file.
[[nodiscard]] std::string spliceRangeMap(std::string body, std::uint32_t firstCodePoint, std::uint32_t lastCodePoint, std::string_view family, std::optional<bool> shouldMatchSize = std::nullopt);

/// Removes any override covering `[firstCodePoint, lastCodePoint]`.
///
/// An entry extending beyond the removed range is split rather than dropped, so
/// removing one character's override leaves the rest of the block it sat in
/// still routed. Deleting rather than writing an explicit route back to the
/// primary: a file that accumulates no-op entries is worse to read, and the
/// absence of an entry is what "no override" already means.
///
/// - Parameters:
///   - body: The whole config file.
///   - firstCodePoint: The first code point to stop routing.
///   - lastCodePoint: The last, inclusive.
/// - Returns: The rewritten file.
[[nodiscard]] std::string removeRangeMap(std::string body, std::uint32_t firstCodePoint, std::uint32_t lastCodePoint);

} // namespace Ubiquitty::Preferences
