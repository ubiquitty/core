// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

//
// test_gridcaptureplan.cpp — exhaustive policy test for the damage-first
// capture eligibility predicate (Rendering::captureCanReuse). Each Full-copy
// trigger gets a positive and a verify-by-revert-style negative: start from a
// fully-eligible baseline and flip exactly one field to prove that field alone
// forces a full re-read. The dual-buffer generation-gap case (the stale-
// row bug) is the one that matters.
//

#include "Core/Rendering/GridCapturePlan.h"

#include <cstdio>
#include <cstdlib>

using Ubiquitty::Rendering::captureCanReuse;
using Ubiquitty::Rendering::captureCanSkipGrid;
using Ubiquitty::Rendering::CaptureEligibility;
using Ubiquitty::Rendering::GridCacheSetting;
using Ubiquitty::Rendering::GridSkipEligibility;
using Ubiquitty::Rendering::isRetainedGridCacheEnabled;
using Ubiquitty::Rendering::parseGridCacheSetting;

// A fully-eligible capture: trusted, initialized, no generation gap (the peer
// buffer drained nothing since this buffer's last capture), live viewOffset,
// same geometry, same reverse-video. This must reuse; every negative below
// flips one field of a fresh copy of this.
static CaptureEligibility eligibleBaseline()
{
    CaptureEligibility e;
    e.trustDamage = true;
    e.bufferInitialized = true;
    e.isSameGrid = true;
    e.bufferGeneration = 7;
    e.generationBeforeDrain = 7; // equal -> no peer drain in between
    e.lastViewOffset = 0;
    e.currentViewOffset = 0;
    e.lastRows = 60;
    e.lastColumns = 220;
    e.currentRows = 60;
    e.currentColumns = 220;
    e.lastReverseVideo = false;
    e.currentReverseVideo = false;
    return e;
}

TEST(gridcaptureplanCases)
{

    CHECK(parseGridCacheSetting(nullptr) == GridCacheSetting::On);
    CHECK(parseGridCacheSetting("") == GridCacheSetting::On);
    CHECK(parseGridCacheSetting("1") == GridCacheSetting::On);
    CHECK(parseGridCacheSetting("on") == GridCacheSetting::On);
    CHECK(parseGridCacheSetting("0") == GridCacheSetting::Off);
    CHECK(parseGridCacheSetting("off") == GridCacheSetting::Off);
    // Read whole, not by its first character: each of these meant something
    // and is not one of the four.
    for (const char* value : { "false", "true", "0ff", "OFF", "no", "00", "1 " })
        CHECK_MSG(parseGridCacheSetting(value) == GridCacheSetting::Unrecognized, value);

    // Positive: the eligible baseline reuses.
    CHECK(captureCanReuse(eligibleBaseline()));

    // trustDamage:No (sync pending / scrollback / reset) -> full.
    {
        CaptureEligibility e = eligibleBaseline();
        e.trustDamage = false;
        CHECK(!captureCanReuse(e));
    }

    // First capture into this buffer -> nothing retained -> full.
    {
        CaptureEligibility e = eligibleBaseline();
        e.bufferInitialized = false;
        CHECK(!captureCanReuse(e));
    }

    // Another grid drained this buffer last: its rows are on the target, and
    // the generation it left is a count that this grid can reach too -> full.
    {
        CaptureEligibility e = eligibleBaseline();
        e.isSameGrid = false;
        CHECK(!captureCanReuse(e));
    }

    // Generation gap: the peer buffer consumed a generation (8) this buffer
    // never saw (stored 7). This is the dual-buffer stale-row bug —
    // reuse here would render rows whose damage the peer already drank.
    {
        CaptureEligibility e = eligibleBaseline();
        e.generationBeforeDrain = 8; // peer drained once since our stored 7
        CHECK(!captureCanReuse(e));
    }
    // A larger gap (peer drained several times) is likewise full.
    {
        CaptureEligibility e = eligibleBaseline();
        e.bufferGeneration = 7;
        e.generationBeforeDrain = 12;
        CHECK(!captureCanReuse(e));
    }
    // The generation must never appear to run BACKWARDS (would wrap into a huge
    // unsigned delta); an equal generation is the only reuse case.
    {
        CaptureEligibility e = eligibleBaseline();
        e.bufferGeneration = 9;
        e.generationBeforeDrain = 8; // impossible in practice, but must be full
        CHECK(!captureCanReuse(e));
    }

    // Scrolled into history this frame -> live-screen damage can't describe the
    // shown rows -> full.
    {
        CaptureEligibility e = eligibleBaseline();
        e.currentViewOffset = 5;
        CHECK(!captureCanReuse(e));
    }
    // Returning from history (last capture was scrolled) -> the retained rows
    // are history, not the live screen -> full.
    {
        CaptureEligibility e = eligibleBaseline();
        e.lastViewOffset = 5;
        e.currentViewOffset = 0;
        CHECK(!captureCanReuse(e));
    }

    // Geometry change (rows or columns) -> retained buffer resized -> full.
    {
        CaptureEligibility e = eligibleBaseline();
        e.currentRows = 61;
        CHECK(!captureCanReuse(e));
    }
    {
        CaptureEligibility e = eligibleBaseline();
        e.currentColumns = 221;
        CHECK(!captureCanReuse(e));
    }

    // Reverse-video toggled (frame-global, repaints every cell) -> full.
    {
        CaptureEligibility e = eligibleBaseline();
        e.currentReverseVideo = true;
        CHECK(!captureCanReuse(e));
    }

    // A palette write recolours cells without touching them, so damage reports
    // the rows clean and reuse would keep the replaced colours.
    {
        CaptureEligibility e = eligibleBaseline();
        e.currentPaletteEpoch = e.lastPaletteEpoch + 1;
        CHECK(!captureCanReuse(e));
    }

    // An unchanged epoch reuses: the epoch moves only when a colour did, and a
    // tick per re-asserted palette would break reuse permanently.
    {
        CaptureEligibility e = eligibleBaseline();
        e.lastPaletteEpoch = 9;
        e.currentPaletteEpoch = 9;
        CHECK(captureCanReuse(e));
    }

    // A large but consistent generation value still reuses (no fixed origin).
    {
        CaptureEligibility e = eligibleBaseline();
        e.bufferGeneration = 1000000;
        e.generationBeforeDrain = 1000000;
        CHECK(captureCanReuse(e));
    }

    // Grid-skip policy: fullscreen content mode WITH the opaque Color letterbox
    // WITH the occlusion skip enabled is the only combination that skips the
    // grid copy. Any fact missing keeps the copy — without the mode nothing
    // letterboxes, under the mode a non-Color letterbox leaves grid cells
    // readable on glass, and with the occlusion skip disabled (diagnostic
    // knob) glyph ink still reaches glass and the knob's A/B must stay clean.
    {
        GridSkipEligibility e;
        CHECK(!captureCanSkipGrid(e)); // defaults: nothing established
    }
    {
        GridSkipEligibility e;
        e.isFullscreenContentActive = true;
        e.isOcclusionSkipEnabled = true;
        CHECK(!captureCanSkipGrid(e)); // no opaque letterbox: grid may show
    }
    {
        GridSkipEligibility e;
        e.isLetterboxOpaque = true;
        e.isOcclusionSkipEnabled = true;
        CHECK(!captureCanSkipGrid(e)); // letterbox fact alone, mode off
    }
    {
        GridSkipEligibility e;
        e.isFullscreenContentActive = true;
        e.isLetterboxOpaque = true;
        CHECK(!captureCanSkipGrid(e)); // occlusion skip disabled: ink on glass
    }
    {
        GridSkipEligibility e;
        e.isFullscreenContentActive = true;
        e.isLetterboxOpaque = true;
        e.isOcclusionSkipEnabled = true;
        CHECK(captureCanSkipGrid(e));
    }
}

// The process's switch is the environment's value, read once: set before the
// first ask, as a launch sets it, it turns the cache off for the process.
// Nothing in this binary asks earlier.
TEST(theEnvironmentTurnsTheProcessCacheOff)
{
    REQUIRE(::setenv("UBIQUITTY_GRID_CACHE", "0", 1) == 0);
    CHECK(!isRetainedGridCacheEnabled());
    REQUIRE(::setenv("UBIQUITTY_GRID_CACHE", "1", 1) == 0);
    CHECK_MSG(!isRetainedGridCacheEnabled(), "a later change does not reach a process that already read it");
}

UBIQUITTY_TEST_MAIN()
