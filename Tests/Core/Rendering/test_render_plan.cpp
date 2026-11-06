// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include <map>

// test_render_plan.cpp — portable frame-damage model (Rendering/RenderPlan).
//
// Verifies the FrameKind classification (Full / Unchanged / Dirty / Scrolled),
// the per-row dirty mask + bounding band, uniform-scroll detection up and down,
// the ambiguous-match fallback to Dirty, the frame-epoch and dimension forces,
// and the plan()/commit() baseline split. Pure C++, links standalone.

#include "Core/Cell.h"
#include "Core/Rendering/AnimationSchedule.h"
#include "Core/Rendering/RenderPlan.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <vector>

using Ubiquitty::Rendering::FrameDamageTracker;
using Ubiquitty::Rendering::FrameKind;
using Ubiquitty::Rendering::FramePlan;
using Ubiquitty::Rendering::RowDigest;
using Ubiquitty::Rendering::rowDigest;

// Count set bits in a plan's dirty mask.
static int dirtyCount(const FramePlan& p)
{
    int n = 0;
    for (int r = 0; r < p.rows; ++r)
        n += (p.dirtyRows[r] ? 1 : 0);
    return n;
}

TEST(renderPlan)
{

    {
        using Ubiquitty::Rendering::AnimationScheduleInputs;
        using Ubiquitty::Rendering::nextAnimationDelaySeconds;
        constexpr double tickSeconds = 1.0 / 60.0;

        AnimationScheduleInputs inputs;
        CHECK(nextAnimationDelaySeconds(inputs) == 0.5);

        inputs.blinkDelayTicks = 8;
        CHECK(nextAnimationDelaySeconds(inputs) == 8.0 * tickSeconds);
        inputs.cursorDelayTicks = 2;
        CHECK(nextAnimationDelaySeconds(inputs) == 2.0 * tickSeconds);

        inputs.smoothAnimationActive = true;
        CHECK(nextAnimationDelaySeconds(inputs) == tickSeconds);
        inputs.smoothAnimationActive = false;
        inputs.blinkDelayTicks = 0;
        inputs.cursorDelayTicks = 0;
        inputs.imageDelayMilliseconds = 100;
        CHECK(nextAnimationDelaySeconds(inputs) == 0.1);

        inputs.imageDelayMilliseconds = 0;
        inputs.hiliteAwaiting = true;
        CHECK(nextAnimationDelaySeconds(inputs) == 0.5);
        inputs.blinkDelayTicks = 90;
        CHECK(nextAnimationDelaySeconds(inputs) == 0.5);
    }

    // ---- presentationFramesPerSecond: the frame clock's rate ladder ----
    {
        using Ubiquitty::Rendering::FramePresentation;
        using Ubiquitty::Rendering::presentationFramesPerSecond;

        // Presented: the panel maximum, clamped only by a set user cap.
        CHECK(presentationFramesPerSecond(120, 0, FramePresentation::Presented) == 120);
        CHECK(presentationFramesPerSecond(120, 60, FramePresentation::Presented) == 60);
        CHECK(presentationFramesPerSecond(60, 240, FramePresentation::Presented) == 60); // a cap above the panel does not raise it
        CHECK(presentationFramesPerSecond(120, -1, FramePresentation::Presented) == 120); // a non-positive cap is none

        // A pane nobody can see earns no link at all — zero, at any cap, so a
        // covered terminal drives no frame into a drawable nothing composites.
        CHECK(presentationFramesPerSecond(120, 0, FramePresentation::Background) == 0);
        CHECK(presentationFramesPerSecond(120, 60, FramePresentation::Background) == 0);
        CHECK(presentationFramesPerSecond(120, 5, FramePresentation::Background) == 0);
    }

    // ---- rowDigest: deterministic, content- and salt-sensitive ----
    {
        const std::array<unsigned char, 8> a { 1, 2, 3, 4, 5, 6, 7, 8 };
        std::array<unsigned char, 8> b { 1, 2, 3, 4, 5, 6, 7, 8 };
        CHECK(rowDigest(std::as_bytes(std::span(a)), 0) == rowDigest(std::as_bytes(std::span(b)), 0)); // same bytes, same salt
        b[3] = 99;
        CHECK(rowDigest(std::as_bytes(std::span(a)), 0) != rowDigest(std::as_bytes(std::span(b)), 0)); // one byte flip changes it
        CHECK(rowDigest(std::as_bytes(std::span(a)), 0) != rowDigest(std::as_bytes(std::span(a)), 1)); // salt changes it
        CHECK(rowDigest({}, 7) == rowDigest({}, 7)); // empty row keyed only by salt
    }

    // ---- first frame is always Full ----
    FrameDamageTracker tr;
    {
        RowDigest d[5] = { 10, 11, 12, 13, 14 };
        const FramePlan& p = tr.plan(d, 5, /*epoch*/ 1);
        CHECK(p.kind == FrameKind::Full);
        CHECK(dirtyCount(p) == 5);
        CHECK(p.dirtyFirst == 0 && p.dirtyLast == 4);
        tr.commit();
    }

    // ---- identical digests + epoch → Unchanged, nothing dirty ----
    {
        RowDigest d[5] = { 10, 11, 12, 13, 14 };
        const FramePlan& p = tr.plan(d, 5, 1);
        CHECK(p.kind == FrameKind::Unchanged);
        CHECK(dirtyCount(p) == 0);
        CHECK(p.dirtyLast < p.dirtyFirst); // empty band
        tr.commit();
    }

    // ---- one row edited → Dirty, only that row, tight band ----
    {
        RowDigest d[5] = { 10, 11, 77, 13, 14 };
        const FramePlan& p = tr.plan(d, 5, 1);
        CHECK(p.kind == FrameKind::Dirty);
        CHECK(dirtyCount(p) == 1);
        CHECK(p.dirtyRows[2] != 0);
        CHECK(p.dirtyFirst == 2 && p.dirtyLast == 2);
        tr.commit(); // baseline now {10,11,77,13,14}
    }

    // ---- scroll up by 1: content moves up, fresh row at the bottom ----
    {
        // prev {10,11,77,13,14} shifted up by 1 → {11,77,13,14, NEW}
        RowDigest d[5] = { 11, 77, 13, 14, 555 };
        const FramePlan& p = tr.plan(d, 5, 1);
        CHECK(p.kind == FrameKind::Scrolled);
        CHECK(p.scrollDelta == 1);
        CHECK(dirtyCount(p) == 1);
        CHECK(p.dirtyRows[4] != 0); // only the freshly-exposed bottom row
        CHECK(p.dirtyFirst == 4 && p.dirtyLast == 4);
        tr.commit(); // baseline now {11,77,13,14,555}
    }

    // ---- scroll down by 2: content moves down, two fresh rows at the top ----
    {
        // prev {11,77,13,14,555} shifted down by 2 → {NEW0, NEW1, 11,77,13}
        RowDigest d[5] = { 900, 901, 11, 77, 13 };
        const FramePlan& p = tr.plan(d, 5, 1);
        CHECK(p.kind == FrameKind::Scrolled);
        CHECK(p.scrollDelta == -2);
        CHECK(dirtyCount(p) == 2);
        CHECK(p.dirtyRows[0] != 0 && p.dirtyRows[1] != 0);
        CHECK(p.dirtyFirst == 0 && p.dirtyLast == 1);
        tr.commit();
    }

    // ---- epoch bump forces Full even with identical digests ----
    {
        RowDigest d[5] = { 900, 901, 11, 77, 13 };
        const FramePlan& p = tr.plan(d, 5, /*epoch*/ 2);
        CHECK(p.kind == FrameKind::Full);
        CHECK(dirtyCount(p) == 5);
        tr.commit();
    }

    // ---- dimension change forces Full ----
    {
        RowDigest d[3] = { 900, 901, 11 };
        const FramePlan& p = tr.plan(d, 3, 2);
        CHECK(p.kind == FrameKind::Full);
        CHECK(p.rows == 3);
        CHECK(dirtyCount(p) == 3);
        tr.commit();
    }

    // ---- ambiguous shift (repeated rows) falls back to Dirty, never Scrolled ----
    {
        FrameDamageTracker amb;
        RowDigest base[4] = { 7, 7, 7, 7 };
        amb.plan(base, 4, 1);
        amb.commit();
        RowDigest edit[4] = { 7, 7, 7, 9 }; // one edited row; up-by-1 and up-by-2
                                            // both "match" the all-7 prefix
        const FramePlan& p = amb.plan(edit, 4, 1);
        CHECK(p.kind == FrameKind::Dirty); // ambiguity → no scroll claim
        CHECK(dirtyCount(p) == 1);
        CHECK(p.dirtyRows[3] != 0);
    }

    // ---- plan() without commit() leaves the baseline; reset() forces Full ----
    {
        FrameDamageTracker t2;
        RowDigest d0[3] = { 1, 2, 3 };
        t2.plan(d0, 3, 1);
        t2.commit();
        RowDigest d1[3] = { 1, 9, 3 };
        const FramePlan& pa = t2.plan(d1, 3, 1); // no commit
        CHECK(pa.kind == FrameKind::Dirty && pa.dirtyRows[1] != 0);
        const FramePlan& pb = t2.plan(d1, 3, 1); // still vs baseline {1,2,3}
        CHECK(pb.kind == FrameKind::Dirty && pb.dirtyRows[1] != 0);
        t2.commit();
        const FramePlan& pc = t2.plan(d1, 3, 1); // now baseline is {1,9,3}
        CHECK(pc.kind == FrameKind::Unchanged);
        t2.reset();
        const FramePlan& pd = t2.plan(d1, 3, 1); // reset → Full again
        CHECK(pd.kind == FrameKind::Full);
    }

    // ---- forced digest equality cannot suppress changed pixels forever ----
    {
        FrameDamageTracker collision;
        RowDigest forcedEqual[3] = { 5, 5, 5 };
        collision.plan(forcedEqual, 3, 1);
        collision.commit();
        for (int frame = 1; frame < FrameDamageTracker::collisionRecoveryFrameInterval; ++frame) {
            const FramePlan& p = collision.plan(forcedEqual, 3, 1);
            CHECK(p.kind == FrameKind::Unchanged);
            collision.commit();
        }
        const FramePlan& recovery = collision.plan(forcedEqual, 3, 1);
        CHECK(recovery.kind == FrameKind::Full);
        CHECK(dirtyCount(recovery) == 3);
        collision.commit();
        CHECK(collision.plan(forcedEqual, 3, 1).kind == FrameKind::Unchanged);
    }

    // ---- recovery cadence preempts a digest-authorized cache shift ----
    {
        FrameDamageTracker collisionScroll;
        RowDigest base[4] = { 1, 2, 3, 4 };
        collisionScroll.plan(base, 4, 1);
        collisionScroll.commit();
        for (int frame = 1; frame < FrameDamageTracker::collisionRecoveryFrameInterval; ++frame) {
            collisionScroll.plan(base, 4, 1);
            collisionScroll.commit();
        }
        RowDigest wouldScroll[4] = { 2, 3, 4, 9 };
        const FramePlan& recovery = collisionScroll.plan(wouldScroll, 4, 1);
        CHECK(recovery.kind == FrameKind::Full);
        CHECK(recovery.scrollDelta == 0);
        CHECK(dirtyCount(recovery) == 4);
    }

    // ---- buildFrameDigests: per-row salt + epoch from portable inputs ----
    {
        using Ubiquitty::Rendering::buildFrameDigests;
        using Ubiquitty::Rendering::PlanInputs;
        const int rows = 3, columns = 4;
        std::vector<Ubiquitty::Cell> grid(static_cast<size_t>(rows) * columns);
        for (auto& cell : grid)
            cell = Ubiquitty::Cell {};
        std::uint32_t globals = 0x1234u;

        PlanInputs in;
        in.cells = grid.data();
        in.rows = rows;
        in.columns = columns;
        in.globalState = &globals;
        in.globalStateLength = sizeof(globals);

        RowDigest d0[3], d1[3];
        const std::uint64_t e0 = buildFrameDigests(in, d0);

        // Editing one cell changes only that row's digest; epoch holds.
        grid[static_cast<size_t>(columns) * 1 + 2].codePoint = 'X';
        const std::uint64_t e1 = buildFrameDigests(in, d1);
        CHECK(d0[0] == d1[0]);
        CHECK(d0[1] != d1[1]);
        CHECK(d0[2] == d1[2]);
        CHECK(e0 == e1);

        // A baked cursor on row 2 perturbs only row 2's salt.
        RowDigest d2[3];
        in.cursorRow = 2;
        in.cursorColumn = 1;
        in.cursorFadeBucket = 3;
        buildFrameDigests(in, d2);
        CHECK(d2[2] != d1[2]);
        CHECK(d2[0] == d1[0]);
        in.cursorRow = -1;

        // An SGR-5 blink cell folds the clock into the epoch (forces rebuild);
        // with no blink the clock is ignored.
        const std::uint64_t eNoBlink_a = (in.blinkClock = 100, buildFrameDigests(in, d2));
        const std::uint64_t eNoBlink_b = (in.blinkClock = 200, buildFrameDigests(in, d2));
        CHECK(eNoBlink_a == eNoBlink_b); // no blink ⇒ clock does not matter
        grid[0].pen.attributes.add(Ubiquitty::VT::Attribute::Blink);
        const std::uint64_t eBlink_a = (in.blinkClock = 100, buildFrameDigests(in, d2));
        const std::uint64_t eBlink_b = (in.blinkClock = 200, buildFrameDigests(in, d2));
        CHECK(eBlink_a != eBlink_b); // blink ⇒ clock changes epoch

        // blinkPresentHint=false skips the per-cell blink scan, so the clock is
        // not folded even with a blink cell present — the hint is a caller promise
        // that blink is absent and must only ever be a safe over-approximation.
        in.blinkPresentHint = false;
        const std::uint64_t eHinted_a = (in.blinkClock = 100, buildFrameDigests(in, d2));
        const std::uint64_t eHinted_b = (in.blinkClock = 200, buildFrameDigests(in, d2));
        CHECK(eHinted_a == eHinted_b); // scan skipped ⇒ blink clock ignored
        in.blinkPresentHint = true; // restore the default for the checks below

        // Any global-state byte change forces a new epoch.
        globals = 0x9999u;
        const std::uint64_t e2 = buildFrameDigests(in, d2);
        CHECK(e2 != eBlink_b);
    }

    // ---- PlanInputs.rowPointers == flat cells (the host daemon digests the
    // live screen's contiguous row pointers in place; must match the flat path)
    {
        const int rows = 6, columns = 10;
        std::vector<Ubiquitty::Cell> grid(static_cast<std::size_t>(rows) * columns);
        for (std::size_t i = 0; i < grid.size(); ++i)
            grid[i].codePoint = static_cast<std::uint32_t>('A' + (i % 26));
        std::uint32_t globals = 0x1234u;

        Ubiquitty::Rendering::PlanInputs flat;
        flat.cells = grid.data();
        flat.rows = rows;
        flat.columns = columns;
        flat.cursorRow = 2;
        flat.cursorColumn = 3;
        flat.globalState = &globals;
        flat.globalStateLength = sizeof(globals);

        std::vector<const Ubiquitty::Cell*> rowPointers(rows);
        for (int r = 0; r < rows; ++r)
            rowPointers[static_cast<std::size_t>(r)] = grid.data() + static_cast<std::size_t>(r) * columns;
        Ubiquitty::Rendering::PlanInputs viaPointers = flat;
        viaPointers.cells = nullptr;
        viaPointers.rowPointers = rowPointers.data();

        RowDigest flatDigests[6];
        RowDigest pointerDigests[6];
        const std::uint64_t flatEpoch = buildFrameDigests(flat, flatDigests);
        const std::uint64_t pointerEpoch = buildFrameDigests(viaPointers, pointerDigests);
        CHECK(flatEpoch == pointerEpoch);
        for (int r = 0; r < rows; ++r)
            CHECK(flatDigests[r] == pointerDigests[r]);
    }

    // ---- Row metadata participates per row, so a scrollback pan can shift
    // prompt marks with their rows instead of forcing a full-frame epoch.
    {
        const int rows = 4, columns = 3;
        std::vector<Ubiquitty::Cell> first(static_cast<std::size_t>(rows) * columns);
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < columns; ++c)
                first[static_cast<std::size_t>(r) * columns + c].codePoint = static_cast<std::uint32_t>('A' + r);
        std::uint8_t firstMarks[rows] = { 1, 0, 2, 0 };
        std::uint8_t secondMarks[rows] = { 0, 1, 0, 2 };
        std::uint32_t globals = 7;

        Ubiquitty::Rendering::PlanInputs input;
        input.cells = first.data();
        input.rows = rows;
        input.columns = columns;
        input.rowMarks = firstMarks;
        input.globalState = &globals;
        input.globalStateLength = sizeof(globals);

        RowDigest firstDigests[rows];
        const std::uint64_t firstEpoch = buildFrameDigests(input, firstDigests);
        FrameDamageTracker tracker;
        tracker.plan(firstDigests, rows, firstEpoch);
        tracker.commit();

        std::vector<Ubiquitty::Cell> second(static_cast<std::size_t>(rows) * columns);
        for (int r = 1; r < rows; ++r)
            for (int c = 0; c < columns; ++c)
                second[static_cast<std::size_t>(r) * columns + c] = first[static_cast<std::size_t>(r - 1) * columns + c];
        for (int c = 0; c < columns; ++c)
            second[c].codePoint = static_cast<std::uint32_t>('Z');
        input.cells = second.data();
        input.rowMarks = secondMarks;
        RowDigest secondDigests[rows];
        const std::uint64_t secondEpoch = buildFrameDigests(input, secondDigests);
        const FramePlan& p = tracker.plan(secondDigests, rows, secondEpoch);
        CHECK(p.kind == FrameKind::Scrolled);
        CHECK(p.scrollDelta == -1);
        CHECK(dirtyCount(p) == 1);
        CHECK(p.dirtyRows[0] != 0);
    }
}

// rowDigest over a REAL ROW, which is the only size that reaches the code the
// damage tracker actually runs.
//
// The existing case digests eight bytes. The aarch64 implementation stripes four
// independent CRC32C lanes across 32-byte blocks and only enters that loop at
// `i + 32 <= byteCount`, so an eight-byte buffer exercises the tail and nothing
// else. A row is 80 columns x 16-byte Cell = 1280 bytes and takes the striped
// path every time.
//
// Controlled: deleting `lane2 = __crc32cd(lane2, w2)` — a quarter of every row's
// bytes no longer reaching the digest, so one cell in four becomes invisible to
// damage and its changes leave stale pixels on screen — leaves the existing case
// PASSING. This one fails on the first byte of lane 2.
TEST(rowDigestSeesEveryByteOfARealRow)
{
    constexpr int columns = 80;
    const std::size_t rowBytes = static_cast<std::size_t>(columns) * sizeof(Ubiquitty::Cell);
    CHECK_MSG(rowBytes >= 32, "a row must reach the striped loop, or this case tests the tail again");

    std::vector<unsigned char> base(rowBytes);
    for (std::size_t i = 0; i < rowBytes; ++i)
        base[i] = static_cast<unsigned char>(i * 7 + 3);
    const RowDigest reference = rowDigest(std::as_bytes(std::span(base)), 0x1234u);

    // Every byte position must change the digest. A dropped lane, a mis-strided
    // load or a tail that starts at the wrong offset all show up here as a
    // position that does not matter, and none of them are visible to a test that
    // flips one byte of an eight-byte buffer.
    int positionsChecked = 0;
    int positionsIgnored = 0;
    std::size_t firstIgnored = 0;
    for (std::size_t i = 0; i < rowBytes; ++i) {
        std::vector<unsigned char> mutated = base;
        mutated[i] = static_cast<unsigned char>(mutated[i] ^ 0xFFu);
        ++positionsChecked;
        if (rowDigest(std::as_bytes(std::span(mutated)), 0x1234u) == reference) {
            if (positionsIgnored == 0)
                firstIgnored = i;
            ++positionsIgnored;
        }
    }
    // The sweep asserts its OWN size: a loop over an empty range reports no
    // ignored positions just as loudly as a correct digest does.
    CHECK_MSG(positionsChecked == static_cast<int>(rowBytes), "the sweep covered the whole row");
    CHECK_MSG(positionsIgnored == 0, "every byte of a row reaches its digest");
    if (positionsIgnored != 0)
        std::printf("  first ignored byte offset %zu (lane %zu)\n", firstIgnored, (firstIgnored % 32) / 8);

    // Two lanes moving at once. A fold whose per-lane multipliers were equal
    // would let a change in one cancel a change in another — a collision needs
    // two fields to move, and every assertion above moves one.
    std::map<RowDigest, std::pair<int, int>> seen;
    int pairsCompared = 0;
    int cancellations = 0;
    for (int a = 1; a < 32; ++a) {
        for (int b = 1; b < 32; ++b) {
            std::vector<unsigned char> mutated = base;
            mutated[0] = static_cast<unsigned char>(mutated[0] ^ a);
            mutated[8] = static_cast<unsigned char>(mutated[8] ^ b);
            const RowDigest d = rowDigest(std::as_bytes(std::span(mutated)), 0x1234u);
            ++pairsCompared;
            const auto found = seen.find(d);
            if (found != seen.end() && found->second != std::make_pair(a, b))
                ++cancellations;
            else
                seen.emplace(d, std::make_pair(a, b));
        }
    }
    CHECK_MSG(pairsCompared == 31 * 31, "the cross product was actually swept");
    CHECK_MSG(cancellations == 0, "no two-lane change cancels into an identical digest");
}

UBIQUITTY_TEST_MAIN()
