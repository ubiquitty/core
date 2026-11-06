// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Title/BannerText.h — text made fit for a desktop banner.
//
// Every word a banner shows came from somewhere this process does not trust: a
// title a program set, a name a host sent, the payload of an escape sequence. A
// banner is drawn by the system, outside the terminal, in the system's own
// type, so text that reaches it whole can do there what it could never do in
// the grid — reorder the line with a bidi override, break it with a control,
// or run long enough to push the part that identifies the place off the edge.
//
// Two operations, both pure:
//
//   * `sanitizedBannerText` removes what has no business in one line of prose
//     and folds every run of blanks to one.
//   * `boundedBannerText` cuts to a width, between grapheme clusters, and says
//     so with an ellipsis.
//
// Width is counted in terminal columns — one for most scalars, two for a wide
// one — because that is the measure Core has and it tracks a proportional
// font's advance closely enough for a budget: a wide ideograph is about two
// Latin letters in a banner as it is in the grid.
//

#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/Width.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Title {

/// What becomes of the mark a place line writes between its levels.
enum class LevelMarks : std::uint8_t {
    /// The text stands alone, so the mark is a character like any other.
    Keep,
    /// The text is one level of a place line, where the mark would read as the
    /// start of another level.
    Remove,
};

/// The mark a place line writes between its levels, U+203A.
inline constexpr char32_t bannerLevelMark = 0x203A;

namespace detail {

/// One scalar read from UTF-8 text.
struct DecodedScalar {
    /// The scalar, meaningful only when `isValid`.
    char32_t codePoint = 0;
    /// The bytes consumed, at least one so a scan always advances.
    std::size_t length = 1;
    /// Whether the bytes were a well-formed, shortest-form scalar.
    bool isValid = false; // bool-ok: names the one outcome it reports
};

/// Reads the scalar starting at `index`.
///
/// Strict: an overlong form, a surrogate, a value past U+10FFFF, a stray
/// continuation byte and a sequence the text ends inside are all invalid
/// and consume one byte, so the scan resumes at the next byte that could
/// begin a scalar.
///
/// - Parameters:
///   - text: The bytes.
///   - index: Where to read, less than `text.size()`.
/// - Returns: The scalar and how many bytes it took.
[[nodiscard]] constexpr DecodedScalar decodeScalar(std::string_view text, std::size_t index) noexcept
{
    const auto byteAt = [text](std::size_t position) { return static_cast<std::uint8_t>(text[position]); };
    const std::uint8_t lead = byteAt(index);
    if (lead < 0x80)
        return { lead, 1, true };
    std::size_t length = 0;
    char32_t codePoint = 0;
    char32_t minScalar = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        codePoint = lead & 0x1F;
        minScalar = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        codePoint = lead & 0x0F;
        minScalar = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        codePoint = lead & 0x07;
        minScalar = 0x10000;
    } else {
        return {};
    }
    if (text.size() - index < length)
        return {};
    for (std::size_t offset = 1; offset < length; ++offset) {
        const std::uint8_t continuation = byteAt(index + offset);
        if ((continuation & 0xC0) != 0x80)
            return {};
        codePoint = (codePoint << 6) | (continuation & 0x3F);
    }
    if (codePoint < minScalar || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
        return {};
    return { codePoint, length, true };
}

/// Whether a scalar ends a line or is a control: C0, DEL, C1 and the two
/// Unicode line separators. Each reads as a blank, because it stood
/// between two words.
[[nodiscard]] constexpr bool isBannerBlank(char32_t codePoint) noexcept
{
    return codePoint <= 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F) || codePoint == 0x2028 || codePoint == 0x2029;
}

/// Whether a scalar is dropped outright: the bidirectional controls, which
/// reorder everything after them on the line, and the two zero-width
/// scalars that join nothing and only pad.
[[nodiscard]] constexpr bool isBannerInvisible(char32_t codePoint) noexcept
{
    return codePoint == 0x061C // ARABIC LETTER MARK
        || codePoint == 0x200B // ZERO WIDTH SPACE
        || codePoint == 0x200E || codePoint == 0x200F // LRM, RLM
        || (codePoint >= 0x202A && codePoint <= 0x202E) // embeddings and overrides
        || (codePoint >= 0x2066 && codePoint <= 0x2069) // isolates
        || codePoint == 0xFEFF; // ZERO WIDTH NO-BREAK SPACE
}

/// The columns one grapheme cluster takes: its widest scalar's, and two
/// once an emoji presentation selector asks for the coloured form.
struct ClusterWidth {
    int columns = 0;

    /// Widens the cluster by one of its scalars.
    ///
    /// - Parameter codePoint: The scalar.
    void add(char32_t codePoint)
    {
        if (codePoint == Unicode::Width::variationSelector16) {
            columns = 2;
            return;
        }
        const int cells = Unicode::Width::forCodePoint(codePoint, Unicode::Width::AmbiguousWidth::Narrow);
        if (cells > columns)
            columns = cells;
    }
};

/// Calls `visit(end, columns)` after each grapheme cluster of `text`, with
/// the byte offset the cluster ends at and the columns it takes, until
/// `visit` answers false.
///
/// - Precondition: `text` is well-formed UTF-8, as `sanitizedBannerText`
///   leaves it; an ill-formed byte is measured as one column of its own.
///
/// - Parameters:
///   - text: The text to segment.
///   - visit: Called per cluster; answers whether to go on.
template<typename Visitor>
void forEachBannerCluster(std::string_view text, Visitor&& visit)
{
    Unicode::GraphemeState state {};
    char32_t previous = 0;
    ClusterWidth width;
    std::size_t index = 0;
    while (index < text.size()) {
        const DecodedScalar scalar = decodeScalar(text, index);
        const char32_t codePoint = scalar.isValid ? scalar.codePoint : U'�';
        if (index == 0) {
            Unicode::graphemeStateReset(&state, codePoint);
        } else if (!Unicode::graphemeExtends(&state, previous, codePoint)) {
            if (!visit(index, width.columns))
                return;
            width = {};
            Unicode::graphemeStateReset(&state, codePoint);
        }
        width.add(codePoint);
        previous = codePoint;
        index += scalar.length;
    }
    if (!text.empty())
        visit(text.size(), width.columns);
}

} // namespace detail

/// Makes untrusted text fit to stand in one line of a banner.
///
/// A control — C0, DEL, C1, a line or paragraph separator — becomes a blank,
/// and every run of blanks folds to one, with none left at either end. The
/// bidirectional controls and the zero-width padding scalars are dropped
/// without a blank, as are bytes that are not UTF-8. Nothing else is touched:
/// the words are the sender's.
///
/// - Parameters:
///   - value: The text as it arrived.
///   - levelMarks: Whether the place line's level mark is removed too, which
///     it is for a name that becomes one level of that line.
/// - Returns: Well-formed UTF-8 with no control in it.
[[nodiscard]] inline std::string sanitizedBannerText(std::string_view value, LevelMarks levelMarks = LevelMarks::Keep)
{
    std::string out;
    out.reserve(value.size());
    bool isBlankOwed = false;
    for (std::size_t index = 0; index < value.size();) {
        const detail::DecodedScalar scalar = detail::decodeScalar(value, index);
        const std::string_view bytes = value.substr(index, scalar.length);
        index += scalar.length;
        if (!scalar.isValid || detail::isBannerInvisible(scalar.codePoint))
            continue;
        if (detail::isBannerBlank(scalar.codePoint) || (levelMarks == LevelMarks::Remove && scalar.codePoint == bannerLevelMark)) {
            isBlankOwed = !out.empty();
            continue;
        }
        if (isBlankOwed)
            out += ' ';
        isBlankOwed = false;
        out += bytes;
    }
    return out;
}

/// The columns a line of sanitized text takes.
///
/// - Parameter text: Text as `sanitizedBannerText` leaves it.
/// - Returns: The sum of its grapheme clusters' widths.
[[nodiscard]] inline int bannerTextColumns(std::string_view text)
{
    int columns = 0;
    detail::forEachBannerCluster(text, [&columns](std::size_t, int clusterColumns) {
        columns += clusterColumns;
        return true;
    });
    return columns;
}

/// Cuts sanitized text to a width, between grapheme clusters.
///
/// Text that fits comes back whole. Text that does not keeps as many whole
/// clusters as leave a column for the ellipsis that says it was cut, so the
/// result is never wider than the bound and a flag, a family or an accented
/// letter is never left half drawn.
///
/// - Parameters:
///   - text: Text as `sanitizedBannerText` leaves it.
///   - maxColumns: The most columns the result may take, at least one.
/// - Returns: The text, or its head and an ellipsis.
[[nodiscard]] inline std::string boundedBannerText(std::string_view text, int maxColumns)
{
    if (bannerTextColumns(text) <= maxColumns)
        return std::string(text);
    std::size_t cut = 0;
    int used = 0;
    detail::forEachBannerCluster(text, [&](std::size_t end, int clusterColumns) {
        if (used + clusterColumns > maxColumns - 1)
            return false;
        used += clusterColumns;
        cut = end;
        return true;
    });
    std::string out(text.substr(0, cut));
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    out += "…";
    return out;
}

} // namespace Ubiquitty::Title
