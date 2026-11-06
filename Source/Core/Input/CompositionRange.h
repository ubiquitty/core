// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The ranges a terminal answers an input method with while a composition is in
// flight.
//
// A terminal has no editable document: the only text an input method can ask
// about is the composition it is building itself, which sits at the cursor and
// reaches the host only when committed. So every range question has one of
// two answers — a range inside that composition, or "not found" — and the
// arithmetic deciding which is the same on every toolkit.
//
// Lengths and offsets are UTF-16 code units, because that is the unit both
// Apple toolkits' input methods count in. A grapheme count reports "🍣" as one
// unit long when the input method placed its caret after two, and a reconversion
// query then asks for a range the terminal claims not to have.
//
// Every offset an input method sends is untrusted: a selection past the end of
// the composition, or the toolkit's "not found" sentinel, is clamped here
// rather than at each frontend's substring call.

#include <algorithm>
#include <cstdint>

namespace Ubiquitty::Input {

/// A range inside the composition in flight, in UTF-16 code units.
struct CompositionRange {
    /// The first unit the range covers.
    std::uint64_t location = 0;
    /// How many units it covers.
    std::uint64_t length = 0;
    /// False when the answer is the toolkit's "not found": no composition is
    /// in flight, so there is nothing for the range to sit in.
    bool isPresent = false;
};

/// The range the composition occupies.
///
/// - Parameter markedLength: The composition's length in UTF-16 units; zero
///   when none is in flight.
/// - Returns: The whole composition, or not found when there is none.
[[nodiscard]] constexpr CompositionRange markedCompositionRange(std::uint64_t markedLength)
{
    if (!markedLength)
        return {};
    return { .location = 0, .length = markedLength, .isPresent = true };
}

/// The caret or selection the input method reported inside its composition,
/// held to the composition's bounds.
///
/// - Parameters:
///   - markedLength: The composition's length in UTF-16 units.
///   - location: Where the input method put the selection.
///   - length: How long it said the selection is.
/// - Returns: The selection clamped into the composition, or not found when no
///   composition is in flight.
[[nodiscard]] constexpr CompositionRange selectedCompositionRange(std::uint64_t markedLength, std::uint64_t location, std::uint64_t length)
{
    if (!markedLength)
        return {};
    const std::uint64_t start = std::min(location, markedLength);
    return { .location = start, .length = std::min(length, markedLength - start), .isPresent = true };
}

/// The part of the composition a substring query is answered with.
///
/// The overlap of the proposed range with the composition. A proposal that
/// misses the composition entirely is answered with the whole of it, because
/// the composition is the only text there is and an input method asking about
/// the text at the cursor is asking about that.
///
/// - Parameters:
///   - markedLength: The composition's length in UTF-16 units.
///   - location: The proposed range's start.
///   - length: The proposed range's length.
/// - Returns: The range to answer with, or not found when no composition is in
///   flight.
[[nodiscard]] constexpr CompositionRange compositionSubstringRange(std::uint64_t markedLength, std::uint64_t location, std::uint64_t length)
{
    if (!markedLength)
        return {};
    if (location >= markedLength || !length)
        return markedCompositionRange(markedLength);
    return { .location = location, .length = std::min(length, markedLength - location), .isPresent = true };
}

} // namespace Ubiquitty::Input
