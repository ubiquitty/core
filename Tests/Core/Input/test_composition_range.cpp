// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_composition_range — the ranges a terminal answers an input method with.
//
// The composition is the only text an input method can ask a terminal about,
// so every answer is a range inside it or "not found". The input method's own
// offsets are untrusted: the cases below send selections and proposals past the
// end and the toolkit's not-found sentinel, and require an answer inside the
// composition, never one a substring call would index past.

#include "Core/Input/CompositionRange.h"

#include <cstdint>
#include <limits>

namespace {

using namespace Ubiquitty::Input;

/// The value AppKit's NSNotFound carries once widened to an unsigned offset.
constexpr std::uint64_t notFound = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

bool same(CompositionRange range, std::uint64_t location, std::uint64_t length)
{
    return range.isPresent && range.location == location && range.length == length;
}

} // namespace

TEST(noCompositionAnswersNotFoundToEveryQuestion)
{
    CHECK(!markedCompositionRange(0).isPresent);
    CHECK(!selectedCompositionRange(0, 0, 0).isPresent);
    CHECK(!selectedCompositionRange(0, 3, 1).isPresent);
    CHECK(!compositionSubstringRange(0, 0, 4).isPresent);
}

// "🍣" is one grapheme and two UTF-16 units; the composition is measured in
// the units the input method counts in, so its caret after the emoji (at 2)
// sits inside the marked range rather than one past it.
TEST(theMarkedRangeIsTheWholeCompositionInUTF16Units)
{
    CHECK(same(markedCompositionRange(2), 0, 2));
    CHECK(same(selectedCompositionRange(2, 2, 0), 2, 0));
}

TEST(theInputMethodsSelectionIsKeptAndHeldInsideTheComposition)
{
    CHECK_MSG(same(selectedCompositionRange(6, 1, 2), 1, 2), "a selection inside the composition is answered as sent");
    CHECK_MSG(same(selectedCompositionRange(6, 4, 9), 4, 2), "a selection running past the end stops at it");
    CHECK_MSG(same(selectedCompositionRange(6, 9, 1), 6, 0), "a caret past the end is the caret at the end");
    CHECK_MSG(same(selectedCompositionRange(6, notFound, 0), 6, 0), "the not-found sentinel cannot index past the composition");
}

TEST(aSubstringQueryIsAnsweredFromTheComposition)
{
    CHECK(same(compositionSubstringRange(6, 0, 6), 0, 6));
    CHECK(same(compositionSubstringRange(6, 2, 2), 2, 2));
    CHECK_MSG(same(compositionSubstringRange(6, 4, 100), 4, 2), "a proposal overlapping the end is cut at it");
    CHECK_MSG(same(compositionSubstringRange(6, 7, 3), 0, 6), "a proposal outside the composition is answered with all of it");
    CHECK_MSG(same(compositionSubstringRange(6, notFound, notFound), 0, 6), "so is the not-found sentinel");
    CHECK_MSG(same(compositionSubstringRange(6, 3, 0), 0, 6), "and an empty proposal");
}

UBIQUITTY_TEST_MAIN()
