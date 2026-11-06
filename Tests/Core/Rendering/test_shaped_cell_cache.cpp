// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_shaped_cell_cache — the cache that holds what the shaper answered for a
// row segment, so an unchanged segment is not shaped twice.
//
// What these pin is the pair of properties that make the cache safe to key on
// text at all: it holds no pixels, and its retention is the frame rather than a
// capacity. The failure modes are a segment served under another's answer
// (wrong letterforms, every count correct) and a span into storage the swap
// reuses (right answer, freed memory).

#include "Core/Rendering/ShapedCellCache.h"

#include <span>
#include <vector>

using namespace Ubiquitty::Rendering;

namespace {

std::vector<std::uint32_t> text(const char* characters)
{
    std::vector<std::uint32_t> out;
    for (const char* c = characters; *c; ++c)
        out.push_back(static_cast<std::uint32_t>(static_cast<unsigned char>(*c)));
    return out;
}

std::vector<ShapedCell> answer(std::initializer_list<std::uint32_t> glyphIDs)
{
    std::vector<ShapedCell> cells;
    for (std::uint32_t id : glyphIDs)
        cells.push_back(ShapedCell { id, 0.0f, 1 });
    return cells;
}

} // namespace

TEST(aSegmentIsShapedOnceAndAnsweredAfterwards)
{
    ShapedCellCache cache;
    const auto segment = text("  win ");
    const auto shaped = answer({ 789u, 504u, 601u });

    CHECK_MSG(cache.lookup(segment, 0u).empty(), "an unseen segment has no answer");
    CHECK(cache.missesThisFrame() == 1 && cache.hitsThisFrame() == 0);

    cache.publish(segment, 0u, shaped);
    const auto served = cache.lookup(segment, 0u);
    CHECK(served.size() == shaped.size());
    CHECK(served[0].glyphID == 789u && served[1].glyphID == 504u && served[2].glyphID == 601u);
    CHECK(cache.hitsThisFrame() == 1);
}

TEST(oneSegmentDrawnTwoWaysIsTwoAnswers)
{
    ShapedCellCache cache;
    const auto segment = text("tmux");
    cache.publish(segment, 0u, answer({ 804u, 789u, 620u, 640u }));
    cache.publish(segment, ligatureShapeBold, answer({ 1804u, 1789u, 1620u, 1640u }));

    const auto regular = cache.lookup(segment, 0u);
    const auto bold = cache.lookup(segment, ligatureShapeBold);
    CHECK_MSG(regular[0].glyphID == 804u && bold[0].glyphID == 1804u,
        "the style is part of the question, so a bold row cannot be served the regular answer");
}

TEST(aDifferentSegmentOfTheSameLengthIsNotServedTheAnswer)
{
    ShapedCellCache cache;
    cache.publish(text("wim"), 0u, answer({ 735u, 601u, 869u }));
    CHECK_MSG(cache.lookup(text("wiw"), 0u).empty(),
        "the hash narrows and the code points decide; a near neighbour must miss");
}

TEST(retentionIsTheFrameAndThenTheKeptSet)
{
    ShapedCellCache cache;
    const auto drawn = text("illmw");
    const auto dropped = text("mmm");
    cache.publish(drawn, 0u, answer({ 601u, 602u, 603u, 789u, 735u }));
    cache.publish(dropped, 0u, answer({ 504u, 504u, 504u }));

    // Frame two draws only one of them. Both survive, because the previous
    // frame is still live — a segment leaves the frames a frame after it
    // stops being used, not the moment something else needs room.
    cache.endFrame();
    const auto served = cache.lookup(drawn, 0u);
    CHECK(served.size() == 5 && served[3].glyphID == 789u);
    CHECK_MSG(cache.keptSegments() == 0, "nothing has left the frames yet");

    // Frame three: only `drawn` was migrated forward in frame two, so
    // `dropped` falls out of both frames — into the kept set, not away.
    cache.endFrame();
    CHECK_MSG(!cache.lookup(drawn, 0u).empty(), "a segment that keeps being drawn keeps being held");
    CHECK_MSG(cache.keptSegments() == 1, "the segment that stopped being drawn is kept");
    cache.endFrame();
    cache.endFrame();
    CHECK_MSG(cache.keptSegments() == 2, "two undrawn frames later the other has left the frames too");
    const auto returned = cache.lookup(dropped, 0u);
    CHECK_MSG(returned.size() == 3 && returned[0].glyphID == 504u, "and answers when it is drawn again");
    CHECK_MSG(cache.keptSegments() == 1, "as the current frame's again, not the kept set's");
    CHECK(cache.hitsThisFrame() == 1 && cache.missesThisFrame() == 0);
}

TEST(aRunTheShaperCannotAnswerIsRememberedAsSuch)
{
    // A strip-only run publishes an empty answer. Empty reads as a miss to a
    // lookup, so the verdict is asked for by name — and it outlives the frame
    // like any answer, so the run is not shaped again while its strip holds.
    ShapedCellCache cache;
    const auto stripOnly = text("=>");
    CHECK_MSG(!cache.holdsNoAnswer(stripOnly, 0u), "an unseen segment is not a known no-answer");
    cache.publish(stripOnly, 0u, {});
    CHECK_MSG(cache.lookup(stripOnly, 0u).empty(), "the empty answer reads as nothing to a lookup");
    CHECK_MSG(cache.holdsNoAnswer(stripOnly, 0u), "and as a verdict when asked by name");

    cache.endFrame();
    CHECK_MSG(cache.lookup(stripOnly, 0u).empty() && cache.holdsNoAnswer(stripOnly, 0u),
        "a lookup migrates the verdict forward, so it is the current frame's again");
    CHECK(cache.hitsThisFrame() == 1 && cache.missesThisFrame() == 0);
}

TEST(theKeptSetEvictsTheLeastRecentPastItsCapacity)
{
    ShapedCellCache cache;
    // The first segment leaves the frames first, so it is the least recent
    // kept; as many distinct segments as the cap follow it through, each
    // dropped by the frame after it, and the cap is what puts it out.
    const auto first = text("first");
    cache.publish(first, 0u, answer({ 1u, 2u, 3u, 4u, 5u }));
    cache.endFrame();
    cache.endFrame();
    CHECK(cache.keptSegments() == 1);
    for (std::uint32_t i = 0; i < 4096u; ++i) {
        std::vector<std::uint32_t> segment = text("seg");
        segment.push_back(0x10000u + i);
        cache.publish(segment, 0u, answer({ 1u, 2u, 3u, 4u }));
        cache.endFrame();
        cache.endFrame();
    }
    CHECK_MSG(cache.keptSegments() == 4096, "the set holds its capacity and no more");
    CHECK_MSG(cache.lookup(first, 0u).empty(), "the least recent is the one evicted");
    std::vector<std::uint32_t> newest = text("seg");
    newest.push_back(0x10000u + 4095u);
    CHECK_MSG(!cache.lookup(newest, 0u).empty(), "the most recent is still answered");
}

TEST(aMigratedAnswerSurvivesTheSwapThatFreesItsOrigin)
{
    // The trap this pins: a hit in the previous frame returned as a span into
    // the previous frame's arrays reads correct until the next swap clears
    // them, and then serves whatever the reused storage holds. The lookup
    // migrates the record forward, so the span the caller holds belongs to the
    // frame that will still be live.
    ShapedCellCache cache;
    const auto segment = text("wi tm il mw");
    cache.publish(segment, 0u, answer({ 735u, 601u, 32u, 804u, 789u }));
    cache.endFrame();

    const auto migrated = cache.lookup(segment, 0u);
    CHECK(migrated.size() == 5);
    cache.endFrame(); // frees what was the previous frame

    CHECK_MSG(migrated[0].glyphID == 735u && migrated[4].glyphID == 789u,
        "the migrated answer is still the answer after the swap");
}

TEST(aCarrierAndItsCoveredCellsAreOneAnswer)
{
    // A multi-cell cluster is one image on the carrier and nothing on the
    // cells it covers, which is the shape the builder consumes: a covered cell
    // that drew its own glyph would draw it under the carrier's ink.
    ShapedCellCache cache;
    const auto segment = text("a => b");
    std::vector<ShapedCell> shaped {
        ShapedCell { 100u, 0.0f, 1 },
        ShapedCell { 32u, 0.0f, 1 },
        ShapedCell { 900u, 0.0f, 2 }, // the `=>` carrier
        ShapedCell { 0u, 0.0f, 0 }, // the cell it covers
        ShapedCell { 32u, 0.0f, 1 },
        ShapedCell { 101u, 0.0f, 1 },
    };
    cache.publish(segment, 0u, shaped);
    const auto served = cache.lookup(segment, 0u);
    CHECK(served[2].coveredCells == 2 && served[2].glyphID == 900u);
    CHECK_MSG(served[3].coveredCells == 0 && served[3].glyphID == 0u,
        "a covered cell answers nothing of its own");
}

UBIQUITTY_TEST_MAIN()
