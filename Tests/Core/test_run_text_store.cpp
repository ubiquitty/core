// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_run_text_store — direct coverage of Ubiquitty::RunTextStore
// (Core/RunTextStore.h): one entry a proportional run under a 1-based id that
// fits a cell's cluster field, no deduplication, entries freed only by
// retainOnly against a reference set, freed ids handed out again, the sweep
// signal, and the snapshot rebuild path.

#include "Core/Cell.h"
#include "Core/RunTextStore.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using Ubiquitty::RunTextStore;

static std::span<const std::uint32_t> view(const std::vector<std::uint32_t>& v) { return { v.data(), v.size() }; }

static bool textEquals(const RunTextStore& store, std::uint16_t id, const std::vector<std::uint32_t>& want)
{
    const std::span<const std::uint32_t> got = store.text(id);
    if (got.size() != want.size())
        return false;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (got[i] != want[i])
            return false;
    }
    return true;
}

static void markReferenced(std::vector<std::uint64_t>& referenced, std::uint16_t id)
{
    referenced[id >> 6] |= std::uint64_t { 1 } << (id & 63);
}

TEST(aRunTextStoreRefusesTextPastItsCodePointBudget)
{
    RunTextStore store;
    const std::vector<std::uint32_t> full(RunTextStore::maxLiveCodePoints, 'a');
    const std::vector<std::uint32_t> extra { 'b' };
    CHECK(store.intern(full) == 1);
    CHECK(store.intern(extra) == 0);
    CHECK(store.slots().size() == 1);
    store.appendDecoded(3, extra);
    CHECK(store.text(3).empty());
    CHECK(store.liveCodePoints() == RunTextStore::maxLiveCodePoints);
    store.retainOnly({});
    CHECK(store.intern(extra) != 0);
    CHECK(store.intern(extra) != 0);
    CHECK(store.intern(extra) != 0);
    CHECK(store.slots().size() == 3);
}

TEST(runTextStoreCases)
{
    // --- empty and unknown ids ---
    {
        RunTextStore store;
        CHECK(store.intern({}) == 0);
        CHECK(store.text(0).empty());
        CHECK(store.text(1).empty());
        CHECK(store.liveCodePoints() == 0);
        CHECK(!store.isSweepDue());
    }

    // --- dense ids, no deduplication, text round trip ---
    {
        RunTextStore store;
        const std::vector<std::uint32_t> hello = { 'H', 'e', 'l', 'l', 'o' };
        const std::uint16_t first = store.intern(view(hello));
        const std::uint16_t second = store.intern(view(hello));
        CHECK(first == 1 && second == 2);
        CHECK(textEquals(store, first, hello));
        CHECK(textEquals(store, second, hello));
        CHECK(store.liveCodePoints() == 10);
    }

    // --- a payload-sized run is one entry, whatever cells it claims ---
    {
        RunTextStore store;
        std::vector<std::uint32_t> payload(4096);
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<std::uint32_t>('a' + i % 26);
        const std::uint16_t id = store.intern(view(payload));
        CHECK(id == 1);
        CHECK(textEquals(store, id, payload));
    }

    // --- retainOnly frees what the reference set leaves out, and only that ---
    {
        RunTextStore store;
        const std::vector<std::uint32_t> a = { 'a' }, b = { 'b', 'b' }, c = { 'c', 'c', 'c' };
        const std::uint16_t idA = store.intern(view(a));
        const std::uint16_t idB = store.intern(view(b));
        const std::uint16_t idC = store.intern(view(c));
        std::vector<std::uint64_t> referenced(RunTextStore::referenceWords, 0);
        markReferenced(referenced, idB);
        store.retainOnly(referenced);
        CHECK(store.text(idA).empty());
        CHECK(textEquals(store, idB, b));
        CHECK(store.text(idC).empty());
        CHECK(store.liveCodePoints() == 2);

        // A freed id is handed out again; the referenced one never is.
        const std::vector<std::uint32_t> d = { 'd' }, e = { 'e' };
        const std::uint16_t idD = store.intern(view(d));
        const std::uint16_t idE = store.intern(view(e));
        CHECK(idD != idB && idE != idB && idD != idE);
        CHECK((idD == idA || idD == idC) && (idE == idA || idE == idC));
        CHECK(textEquals(store, idB, b));
        CHECK(textEquals(store, idD, d) && textEquals(store, idE, e));
    }

    // --- a short reference set reads as unreferenced past its end ---
    {
        RunTextStore store;
        const std::vector<std::uint32_t> x = { 'x' };
        const std::uint16_t id = store.intern(view(x));
        store.retainOnly({});
        CHECK(store.text(id).empty());
        CHECK(store.liveCodePoints() == 0);
    }

    // --- the sweep signal: past the floor, then twice what the last sweep kept ---
    {
        RunTextStore store;
        std::vector<std::uint32_t> chunk(4096, 'z');
        std::vector<std::uint16_t> ids;
        while (store.liveCodePoints() < RunTextStore::sweepFloorCodePoints)
            ids.push_back(store.intern(view(chunk)));
        CHECK(store.isSweepDue());
        std::vector<std::uint64_t> referenced(RunTextStore::referenceWords, 0);
        for (const std::uint16_t id : ids)
            markReferenced(referenced, id);
        const std::size_t kept = store.liveCodePoints();
        store.retainOnly(referenced);
        CHECK(store.liveCodePoints() == kept);
        CHECK(!store.isSweepDue());
        while (store.liveCodePoints() < 2 * kept)
            ids.push_back(store.intern(view(chunk)));
        CHECK(store.isSweepDue());
    }

    // --- every id live: intern refuses, the sweep is due, a sweep frees ids ---
    {
        RunTextStore store;
        const std::vector<std::uint32_t> one = { '1' };
        for (int i = 0; i < RunTextStore::maxEntries; ++i)
            CHECK(store.intern(view(one)) == static_cast<std::uint16_t>(i + 1));
        CHECK(store.intern(view(one)) == 0);
        CHECK(store.isSweepDue());
        store.retainOnly({});
        CHECK(!store.isSweepDue());
        CHECK(store.intern(view(one)) != 0);
    }

    // --- the largest id fits the cell's cluster field ---
    {
        Ubiquitty::Cell cell {};
        cell.setCombiningLength(1);
        cell.setClusterID(static_cast<std::uint16_t>(RunTextStore::maxEntries));
        CHECK(cell.clusterID() == RunTextStore::maxEntries);
        CHECK(cell.combiningLength() == 1);
    }

    // --- snapshot rebuild: ids kept, gaps freed, a repeat keeps its text ---
    {
        RunTextStore store;
        const std::vector<std::uint32_t> first = { 'f' }, third = { 't', 't' }, other = { 'o' };
        store.appendDecoded(1, view(first));
        store.appendDecoded(3, view(third));
        store.appendDecoded(3, view(other));
        store.appendDecoded(0, view(other));
        store.appendDecoded(4, {});
        CHECK(textEquals(store, 1, first));
        CHECK(store.text(2).empty());
        CHECK(textEquals(store, 3, third));
        CHECK(store.liveCodePoints() == 3);
        CHECK(store.intern(view(other)) == 2);
        CHECK(store.slots().size() == 3);
    }

    // --- clear frees every id ---
    {
        RunTextStore store;
        const std::vector<std::uint32_t> text = { 'q' };
        (void)store.intern(view(text));
        store.clear();
        CHECK(store.text(1).empty());
        CHECK(store.liveCodePoints() == 0);
        CHECK(store.intern(view(text)) == 1);
    }
}

UBIQUITTY_TEST_MAIN()
