// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_text_size_table — the spec store's whole policy surface: dedup on
// intern, refusal past the ceiling, soft failure on every bad lookup, and
// reclamation only through the caller's mask.
//
// The policies are the point, not the arithmetic. A live id is never
// reassigned — the stale-geometry guarantee — so the ceiling is reachable and
// the refusal observable; a freed id may mint again, because the only caller
// of retainOnly has proven nothing references it.

#include "Harness.h"

#include "Core/TextSizeTable.h"

#include <array>

using Ubiquitty::TextSizeSpec;
using Ubiquitty::TextSizeTable;
using Ubiquitty::textSizeTableCapacity;

namespace {

TextSizeSpec specWithScale(std::uint8_t scale)
{
    TextSizeSpec spec;
    spec.scale = scale;
    return spec;
}

} // namespace

TEST(internDeduplicatesByContent)
{
    TextSizeTable table;
    const unsigned first = table.intern(specWithScale(2));
    CHECK_EQ(static_cast<int>(first), 1);
    CHECK_EQ(static_cast<int>(table.intern(specWithScale(2))), 1);
    CHECK_EQ(static_cast<int>(table.intern(specWithScale(3))), 2);
    CHECK_EQ(table.liveCount(), 2);
}

TEST(lookupAnswersTheInternedSpecAndNothingElse)
{
    TextSizeTable table;
    const unsigned id = table.intern(specWithScale(4));
    const TextSizeSpec* found = table.lookup(id);
    CHECK(found != nullptr);
    CHECK_EQ(static_cast<int>(found->scale), 4);
    // Zero, out-of-range and never-minted ids fold into null — the render
    // unsized answer, never a wrong spec.
    CHECK(table.lookup(0) == nullptr);
    CHECK(table.lookup(static_cast<unsigned>(textSizeTableCapacity) + 1) == nullptr);
    CHECK(table.lookup(id + 1) == nullptr);
}

TEST(theSixteenthDistinctSpecIsRefusedWhole)
{
    TextSizeTable table;
    for (int scale = 0; scale < textSizeTableCapacity; ++scale) {
        TextSizeSpec spec = specWithScale(static_cast<std::uint8_t>(1 + (scale % 7)));
        spec.fractionNumerator = static_cast<std::uint8_t>(scale);
        CHECK(table.intern(spec) != 0u);
    }
    CHECK_EQ(table.liveCount(), textSizeTableCapacity);
    TextSizeSpec overflow = specWithScale(7);
    overflow.fractionNumerator = 15;
    overflow.fractionDenominator = 15;
    CHECK_EQ(static_cast<int>(table.intern(overflow)), 0);
    // A spec already live still answers its id at the ceiling: only DISTINCT
    // content is refused.
    TextSizeSpec existing = specWithScale(1);
    existing.fractionNumerator = 0;
    CHECK_EQ(static_cast<int>(table.intern(existing)), 1);
}

TEST(retainOnlyFreesTheUnreferencedAndTheirIDsMintAgain)
{
    TextSizeTable table;
    const unsigned kept = table.intern(specWithScale(2));
    const unsigned dropped = table.intern(specWithScale(3));
    CHECK_EQ(static_cast<int>(dropped), 2);
    table.retainOnly(static_cast<std::uint16_t>(1u << (kept - 1)));
    CHECK_EQ(table.liveCount(), 1);
    CHECK(table.lookup(dropped) == nullptr);
    CHECK(table.lookup(kept) != nullptr);
    // The freed slot is mintable again, and dedup does not resurrect the
    // freed content — a fresh intern of the dropped spec is a fresh id
    // landing in the first free slot.
    CHECK_EQ(static_cast<int>(table.intern(specWithScale(3))), 2);
    CHECK_EQ(table.liveCount(), 2);
}

TEST(theWireTrioRoundTripsSlotsAndTheirLiveness)
{
    // The sync wire walks slots by position, dead ones included, because a
    // cell names its spec by slot. The round trip has to preserve which slots
    // are live and what sits in each of them, or a restored cell resolves its id to
    // another cell's specification.
    TextSizeTable sender;
    const unsigned first = sender.intern(specWithScale(2));
    const unsigned second = sender.intern(specWithScale(3));
    const unsigned third = sender.intern(specWithScale(4));
    sender.retainOnly(static_cast<std::uint16_t>((1u << (first - 1)) | (1u << (third - 1))));

    std::array<TextSizeSpec, textSizeTableCapacity> slots {};
    for (int slot = 0; slot < textSizeTableCapacity; ++slot)
        slots[static_cast<std::size_t>(slot)] = sender.specAtSlot(slot);
    const std::uint16_t mask = sender.liveMask();

    TextSizeTable receiver;
    receiver.restore(mask, slots);
    CHECK_EQ(receiver.liveCount(), sender.liveCount());
    CHECK(receiver.lookup(first) != nullptr);
    CHECK_EQ(static_cast<int>(receiver.lookup(first)->scale), 2);
    CHECK(receiver.lookup(third) != nullptr);
    CHECK_EQ(static_cast<int>(receiver.lookup(third)->scale), 4);
    // The freed id stays freed across the wire: a receiver that revived it
    // would resolve a sender's dead slot to a live specification.
    CHECK(receiver.lookup(second) == nullptr);
    // And the freed slot is mintable on the receiver, landing where it was.
    CHECK_EQ(static_cast<int>(receiver.intern(specWithScale(5))), static_cast<int>(second));
}

UBIQUITTY_TEST_MAIN()
