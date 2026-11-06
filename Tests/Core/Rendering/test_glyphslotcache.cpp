// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Portable glyph-atlas slot policy: key-to-slot open-addressing hash, LRU
// eviction, and capacity. Shared by the Metal and Vulkan atlas backends, and
// free of GPU and font types.

#include "Harness.h"

#include "Core/Rendering/GlyphBackendTypes.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/Rendering/GlyphSlotPolicy.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

using Ubiquitty::Rendering::GlyphSlotCache;

TEST(glyphslotcache)
{

    using Ubiquitty::Rendering::recommendedHashBuckets;
    // --- recommendedHashBuckets: smallest pow2 >= 2*capacity ---
    CHECK(recommendedHashBuckets(1) == 2); // 2*1 = 2
    CHECK(recommendedHashBuckets(2) == 4); // 2*2 = 4 (exact pow2)
    CHECK(recommendedHashBuckets(3) == 8); // 2*3 = 6 -> next pow2 = 8
    CHECK(recommendedHashBuckets(4) == 8); // 2*4 = 8 (exact pow2)
    CHECK(recommendedHashBuckets(5) == 16); // 2*5 = 10 -> 16
    // The macOS 128x128 = 16384-slot grid resolves to 32768.
    CHECK(recommendedHashBuckets(16384) == 32768);
    // capacity need not be a power of two (texture-limit clamps): still >= 2x.
    CHECK(recommendedHashBuckets(100) == 256); // 2*100 = 200 -> 256
    CHECK(recommendedHashBuckets(1000) == 2048); // 2*1000 = 2000 -> 2048
    // Result is always a power of two and >= 2*capacity (load factor <= 0.5).
    for (int cap = 1; cap <= 70000; cap = cap + (cap / 3) + 1) {
        int hb = recommendedHashBuckets(cap);
        CHECK(hb >= 2 * cap);
        CHECK((hb & (hb - 1)) == 0); // pow2
        CHECK(hb / 2 < 2 * cap); // smallest such pow2 (no bucket wasted)
    }

    // capacity 4, 8 hash buckets, 2 columns.
    GlyphSlotCache c(4, 8, 2);
    CHECK(c.capacity() == 4);
    CHECK(c.columns() == 2);

    bool isFresh = false;
    // Four distinct keys → four isFresh, distinct slots in [0,4).
    int sa = c.slotForKey(100, &isFresh);
    CHECK(isFresh);
    int sb = c.slotForKey(200, &isFresh);
    CHECK(isFresh);
    int sc = c.slotForKey(300, &isFresh);
    CHECK(isFresh);
    int sd = c.slotForKey(400, &isFresh);
    CHECK(isFresh);
    std::set<int> slots { sa, sb, sc, sd };
    CHECK(slots.size() == 4);
    for (int s : slots)
        CHECK(s >= 0 && s < 4);

    // Re-request is a hit (not isFresh) at the same slot.
    int sa2 = c.slotForKey(100, &isFresh);
    CHECK(!isFresh);
    CHECK(sa2 == sa);

    // LRU order now (MRU→LRU): 100, 400, 300, 200 → tail is 200.
    // A fifth key evicts the tail (200) and reuses its slot.
    int se = c.slotForKey(500, &isFresh);
    CHECK(isFresh);
    CHECK(se == sb);
    // 200 was evicted → re-request is a isFresh miss again.
    int sb2 = c.slotForKey(200, &isFresh);
    CHECK(isFresh);
    // 100 was never evicted → still a hit at its original slot.
    int sa3 = c.slotForKey(100, &isFresh);
    CHECK(!isFresh);
    CHECK(sa3 == sa);

    // No two live keys ever collide on a slot.
    {
        GlyphSlotCache d(4, 8, 2);
        bool f;
        int s100 = d.slotForKey(100, &f);
        int s200 = d.slotForKey(200, &f);
        CHECK(s100 != s200);
    }

    // reset() flushes: every key misses isFresh again.
    c.reset();
    int sa4 = c.slotForKey(100, &isFresh);
    CHECK(isFresh);
    (void)sa4;
    (void)sc;
    (void)sd;
    (void)sb2;

    // Stress: way more distinct keys than capacity. Slot always valid; the
    // most-recent capacity() keys stay resident (hit), older ones evicted.
    GlyphSlotCache big(256, 512, 16);
    for (std::uint32_t k = 0; k < 5000; ++k) {
        bool fr;
        int s = big.slotForKey(k, &fr);
        CHECK(s >= 0 && s < 256);
    }
    // The last-inserted key is definitely resident → a hit.
    {
        bool fr;
        (void)big.slotForKey(4999, &fr);
        CHECK(!fr);
    }
    // A long-ago key was evicted → isFresh miss.
    {
        bool fr;
        (void)big.slotForKey(0, &fr);
        CHECK(fr);
    }

    // --- Slot -> UV geometry (characterization vs the original UBRenderer
    // formula). Atlas 128 columns; slot 9px wide (8 box + 1 headroom) margin 0;
    // 20px tall, margin 2. capacity 128*128.
    {
        using Ubiquitty::Rendering::GlyphSlotGeometry;
        using Ubiquitty::Rendering::GlyphUV;
        GlyphSlotGeometry g { /*columns*/ 128, /*slotWidth*/ 9, /*slotHeight*/ 20,
            /*glyphWidth*/ 8, /*glyphMarginInPixels*/ { 0, 2 } };
        const int capacity = 128 * 128;

        auto approx = [](float a, float b) {
            float d = a - b;
            if (d < 0)
                d = -d;
            return d < 1e-6f;
        };

        // The slot -> UV characterization: the numbers the original UBRenderer
        // formula produced for a headroom slot, which is what atlasRectForSlot has to
        // keep producing. `maxU` is the glyph BOX — `minU + glyphWidth` — and
        // not the slot inset by its margins, which is a pixel wider.
        {
            // slot 0: column 0,row 0. atlasRectForSlot maps the glyph box inside margins.
            GlyphUV s0 = g.atlasRectForSlot(0, capacity);
            CHECK(approx(s0.minU, 0.0f));
            CHECK(approx(s0.minV, 2.0f));
            CHECK(approx(s0.maxU, 8.0f));
            // maxV = y0 + (slotHeight - 2*marginY) = 2 + (20 - 4) = 18.
            CHECK(approx(s0.maxV, 18.0f));

            // slot 130: column 2, row 1 (130 % 128 = 2, 130 / 128 = 1).
            GlyphUV s130 = g.atlasRectForSlot(130, capacity);
            CHECK(approx(s130.minU, (2 * 9 + 0)));
            CHECK(approx(s130.minV, (1 * 20 + 2)));
            CHECK(approx(s130.maxU, (2 * 9 + 0 + 8)));
        }

        // atlasRectForSlotFull carries no such assert, so the full-slot rect is pinned
        // either way — and it is the rect the subpixel blit actually samples.
        // Full slot spans the whole physical slot incl. headroom.
        GlyphUV f130 = g.atlasRectForSlotFull(130, capacity);
        CHECK(approx(f130.minU, (2 * 9)));
        CHECK(approx(f130.minV, (1 * 20)));
        CHECK(approx(f130.maxU, (3 * 9)));
        CHECK(approx(f130.maxV, (2 * 20)));

        CHECK(g.rows(capacity) == 128);
    }

    // --- The same geometry with a NON-ZERO horizontal margin. The block above
    // pins marginY only, so the marginX term of atlasRectForSlot went unasserted — and
    // that is the term every real text face uses, since Unifont is the only
    // zero-overhang rasterizer in the tree and everything else insets the glyph
    // box on both axes.
    {
        using Ubiquitty::Rendering::GlyphSlotGeometry;
        using Ubiquitty::Rendering::GlyphUV;
        // 12 wide = 8 box + 2 margin each side; 20 tall = 16 box + 2 each side.
        GlyphSlotGeometry g { /*columns*/ 64, /*slotWidth*/ 12, /*slotHeight*/ 20,
            /*glyphWidth*/ 8, /*glyphMarginInPixels*/ { 2, 2 } };
        const int capacity = 64 * 64;

        auto approx = [](float a, float b) {
            float d = a - b;
            if (d < 0)
                d = -d;
            return d < 1e-6f;
        };

        // slot 0: the box starts one margin in on each axis and spans the BOX,
        // so neither edge reaches the slot boundary.
        GlyphUV s0 = g.atlasRectForSlot(0, capacity);
        CHECK(approx(s0.minU, 2.0f));
        CHECK(approx(s0.minV, 2.0f));
        CHECK(approx(s0.maxU, 10.0f)); // 2 + glyphWidth 8
        CHECK(approx(s0.maxV, 18.0f)); // 2 + (20 - 2*2)

        // slot 65: column 1, row 1 — both insets ride the slot pitch.
        GlyphUV s65 = g.atlasRectForSlot(65, capacity);
        CHECK(approx(s65.minU, (1 * 12 + 2)));
        CHECK(approx(s65.minV, (1 * 20 + 2)));
        CHECK(approx(s65.maxU, (1 * 12 + 2 + 8)));
        CHECK(approx(s65.maxV, (1 * 20 + 2 + 16)));

        // The full slot ignores both margins, so it still tiles edge to edge:
        // the property that lets a subpixel blit cover the slot without a seam.
        GlyphUV f65 = g.atlasRectForSlotFull(65, capacity);
        CHECK(approx(f65.minU, (1 * 12)));
        CHECK(approx(f65.maxU, (2 * 12)));
    }

    // --- Frame-epoch guard: a single frame referencing > capacity distinct
    // keys must not evict a slot it already placed this frame — it returns -1
    // (caller draws blank) for the overflow, and recovers next frame.
    {
        GlyphSlotCache g(4, 8, 2); // capacity 4
        bool f;
        g.beginFrame();
        // Place 4 distinct glyphs this frame — all fit, all isFresh, all valid.
        int s0 = g.slotForKey(10, &f);
        CHECK(f);
        CHECK(s0 >= 0);
        int s1 = g.slotForKey(11, &f);
        CHECK(f);
        CHECK(s1 >= 0);
        int s2 = g.slotForKey(12, &f);
        CHECK(f);
        CHECK(s2 >= 0);
        int s3 = g.slotForKey(13, &f);
        CHECK(f);
        CHECK(s3 >= 0);
        // 5th distinct key this frame: cache full of this-frame slots → -1,
        // not an eviction of a placed glyph.
        int s4 = g.slotForKey(14, &f);
        CHECK(s4 == -1);
        CHECK(!f);
        // The 4 placed glyphs are still valid (not corrupted) — re-request hits.
        int h0 = g.slotForKey(10, &f);
        CHECK(!f);
        CHECK(h0 == s0);
        int h3 = g.slotForKey(13, &f);
        CHECK(!f);
        CHECK(h3 == s3);

        // Next frame: the overflow key can now take a slot (LRU-evicts an old
        // one), since last frame's slots are no longer "this frame".
        g.beginFrame();
        int s4b = g.slotForKey(14, &f);
        CHECK(f);
        CHECK(s4b >= 0 && s4b < 4);
    }

    // --- Regression: backward-shift deletion must keep probe chains intact.
    // A broken hashRemove (advancing the scan index by the moving gap instead
    // of one slot at a time) skips a slot after each shift and orphans a
    // displaced key, so the cache re-faults still-resident keys and thrashes.
    // The symptom only surfaces on the overflow-churn path (a frame references
    // far more distinct glyphs than capacity). Replay that workload and assert
    // the work checksum; the buggy backward-shift produces a different sum.
    //
    // The pinned value encodes the promotion and EVICTION policies as well as
    // the probe chains, so it moves when either legitimately does. A sampled
    // victim admits no oracle written against the header's contract —
    // reproducing it means reproducing the sampling, which is the
    // implementation — so this value is a CHANGE DETECTOR rather than an
    // independent derivation, and the case below carries the property the
    // policy is chosen for instead.
    {
        GlyphSlotCache c(1024, 2048, 128);
        std::uint64_t rng = 0x12345, acc = 0;
        auto nextKey = [&]() -> std::uint32_t {
            rng = rng * 1103515245ull + 12345ull;
            std::uint64_t bucket = (rng >> 16) % 100;
            std::uint32_t codePoint = bucket < 90 ? 0x20 + (std::uint32_t)((rng >> 8) % 95) : bucket < 99 ? 0x4E00 + (std::uint32_t)((rng >> 8) % 4000)
                                                                                                          : 0x1F600 + (std::uint32_t)((rng >> 8) % 80);
            return (codePoint << 3) | (std::uint32_t)((rng >> 5) & 0x7);
        };
        const int cells = 180 * 58;
        c.beginFrame();
        for (int i = 0; i < cells; ++i) {
            bool fr;
            c.slotForKey(nextKey(), &fr);
        } // warm
        for (int frame = 0; frame < 2000; ++frame) {
            c.beginFrame();
            for (int i = 0; i < cells; ++i) {
                bool fr;
                int s = c.slotForKey(nextKey(), &fr);
                acc += (std::uint64_t)(std::int64_t)s + (fr ? 1u : 0u);
            }
        }
        CHECK(acc == 9694804221ull);
    }

    // Scan resistance. A cyclic walk over more keys than the cache holds is what
    // reading code with a ligature font produces, and it is the shape a tail
    // victim handles worst: each key leaves just before it comes round again, so
    // every pass after the first scores zero. A sampled victim keeps a share.
    // The bound is loose because the share is statistical; what it pins is that
    // the policy does not collapse to nothing on the workload it exists for.
    {
        constexpr int scanCapacity = 512;
        constexpr int scanKeys = 800; // more than the cache holds
        constexpr int stripsPerFrame = 26; // a viewport's worth
        GlyphSlotCache scan(scanCapacity, 1024, 64);
        auto walk = [&]() {
            int hits = 0, total = 0;
            for (int first = 0; first < scanKeys; first += stripsPerFrame) {
                scan.beginFrame();
                for (int k = first; k < first + stripsPerFrame && k < scanKeys; ++k) {
                    bool isFresh = false;
                    const int slot = scan.slotForKey(static_cast<std::uint32_t>(k), &isFresh);
                    total++;
                    if (slot >= 0 && !isFresh)
                        hits++;
                }
            }
            return total > 0 ? (100 * hits) / total : 0;
        };
        walk(); // first pass populates; nothing can hit
        CHECK(walk() >= 20);
    }

    // --- clampAtlasGridToTextureLimit: the texture (columns*slotWidth x
    //     rows*slotHeight) must never exceed the GPU's max 2D dimension, or Metal
    //     aborts. A bounded slot count is not enough — large per-slot pixels alone
    //     blow past the limit. ---
    {
        using Ubiquitty::Rendering::AtlasGridFit;
        using Ubiquitty::Rendering::clampAtlasGridToTextureLimit;
        constexpr int maxDimension = 16384;

        // A normal font fits — the grid is untouched.
        AtlasGridFit fit = clampAtlasGridToTextureLimit(Ubiquitty::GridSize { 128, 128 }, Ubiquitty::IntSize { 20, 30 }, maxDimension);
        CHECK(fit.valid && fit.columns == 128 && fit.rows == 128);

        // A large font: 128*200 = 25600 > 16384 on both axes → shrink so each axis
        // fits. The slot count is already at the cap; only the pixel dimension
        // overflows, so the clamp shrinks columns/rows, not the count.
        fit = clampAtlasGridToTextureLimit(Ubiquitty::GridSize { 128, 128 }, Ubiquitty::IntSize { 200, 400 }, maxDimension);
        CHECK(fit.valid);
        CHECK(fit.columns * 200 <= maxDimension && fit.rows * 400 <= maxDimension);
        CHECK(fit.columns >= 1 && fit.rows >= 1); // never clamped to a zero-dim texture
        CHECK(fit.columns == 16384 / 200 && fit.rows == 16384 / 400);

        // Exact boundary (columns*slotWidth == maxDimension) is not over the limit → kept.
        fit = clampAtlasGridToTextureLimit(Ubiquitty::GridSize { 8, 4 }, Ubiquitty::IntSize { 2048, 4096 }, maxDimension);
        CHECK(fit.valid && fit.columns == 8 && fit.rows == 4); // 8*2048 == 16384, 4*4096 == 16384

        // Tall slots must use spare texture width before losing resident glyphs.
        fit = clampAtlasGridToTextureLimit(Ubiquitty::GridSize { 128, 384 }, Ubiquitty::IntSize { 82, 60 }, maxDimension);
        CHECK(fit.valid);
        CHECK(fit.columns * fit.rows >= 128 * 384);
        CHECK(fit.columns * 82 <= maxDimension && fit.rows * 60 <= maxDimension);

        // A wide grid can likewise trade columns for rows.
        fit = clampAtlasGridToTextureLimit(Ubiquitty::GridSize { 384, 128 }, Ubiquitty::IntSize { 60, 82 }, maxDimension);
        CHECK(fit.valid);
        CHECK(fit.columns * fit.rows >= 384 * 128);
        CHECK(fit.columns * 60 <= maxDimension && fit.rows * 82 <= maxDimension);

        // One slot alone exceeds the limit → refuse (no grid fits): caller returns nil.
        CHECK(!clampAtlasGridToTextureLimit(Ubiquitty::GridSize { 4, 4 }, Ubiquitty::IntSize { 20000, 30 }, maxDimension).valid);
        CHECK(!clampAtlasGridToTextureLimit(Ubiquitty::GridSize { 4, 4 }, Ubiquitty::IntSize { 30, 20000 }, maxDimension).valid);

        // Degenerate metrics → refuse rather than build a zero/negative texture.
        CHECK(!clampAtlasGridToTextureLimit(Ubiquitty::GridSize { 0, 4 }, Ubiquitty::IntSize { 20, 30 }, maxDimension).valid);
        CHECK(!clampAtlasGridToTextureLimit(Ubiquitty::GridSize { 4, 0 }, Ubiquitty::IntSize { 20, 30 }, maxDimension).valid);
        CHECK(!clampAtlasGridToTextureLimit(Ubiquitty::GridSize { 4, 4 }, Ubiquitty::IntSize { 0, 30 }, maxDimension).valid);
    }

    // --- slotForKeyVerified: colliders that are both LIVE each keep a slot.
    //
    // A ligature strip window split two ways carries the same code points, so
    // both variants fold to one key and both are referenced within one frame.
    // Re-owning a single slot would let the last rasterise win
    // while the other variant's already-recorded instances sampled its pixels
    // — an eight-cell hole on screen at scales where the phases coincide. One
    // key must own one slot PER distinct verified contents.
    {
        GlyphSlotCache v(4, 8, 2);
        bool isVerifiedFresh = false;
        const std::uint32_t runA[] = { 0x65u, 0x301u }; // e + combining acute
        const std::uint32_t runB[] = { 0x65u, 0x302u }; // e + combining circumflex
        const std::uint32_t collidingKey = 0x88000020u; // pretend both runs fold here

        const int slotA = v.slotForKeyVerified(collidingKey, runA, 2, &isVerifiedFresh);
        CHECK(slotA >= 0);
        CHECK(isVerifiedFresh); // miss → rasterise
        CHECK(v.slotForKeyVerified(collidingKey, runA, 2, &isVerifiedFresh) == slotA);
        CHECK(!isVerifiedFresh); // same contents → verified hit

        // Collision: same key, different run → a slot of its own.
        const int slotB = v.slotForKeyVerified(collidingKey, runB, 2, &isVerifiedFresh);
        CHECK(slotB >= 0);
        CHECK(slotB != slotA);
        CHECK(isVerifiedFresh);
        CHECK(v.slotForKeyVerified(collidingKey, runB, 2, &isVerifiedFresh) == slotB);
        CHECK(!isVerifiedFresh);
        // The sibling stayed resident: no ping-pong of rasterises.
        CHECK(v.slotForKeyVerified(collidingKey, runA, 2, &isVerifiedFresh) == slotA);
        CHECK(!isVerifiedFresh);

        // A shorter prefix of a stored run is different contents (length
        // matters), so it is a third collider with a third slot.
        const int slotC = v.slotForKeyVerified(collidingKey, runB, 1, &isVerifiedFresh);
        CHECK(slotC >= 0);
        CHECK(slotC != slotA && slotC != slotB);
        CHECK(isVerifiedFresh);

        // Eviction must unhash by (key, slot). runB's slot is the LRU victim
        // here while runA — hashed FIRST, so it owns the key's first chain
        // bucket — survives: a removal that went by key alone would take runA's
        // bucket instead and orphan a live strip.
        CHECK(v.slotForKeyVerified(collidingKey, runA, 2, &isVerifiedFresh) == slotA); // LRU: B is now oldest
        v.slotForKey(0x100u, &isVerifiedFresh); // takes the last free slot
        v.slotForKey(0x200u, &isVerifiedFresh); // capacity 4: evicts runB's slot
        CHECK(v.slotForKeyVerified(collidingKey, runA, 2, &isVerifiedFresh) == slotA);
        CHECK(!isVerifiedFresh); // the first chain bucket survived runB's eviction
        CHECK(v.slotForKeyVerified(collidingKey, runB, 1, &isVerifiedFresh) == slotC);
        CHECK(!isVerifiedFresh); // and so did the bucket past the removed one
        const int slotBAgain = v.slotForKeyVerified(collidingKey, runB, 2, &isVerifiedFresh);
        CHECK(slotBAgain >= 0);
        CHECK(isVerifiedFresh); // evicted → refaults isFresh

        // reset() drops contents with everything else.
        v.reset();
        CHECK(v.slotForKeyVerified(collidingKey, runB, 2, &isVerifiedFresh) >= 0);
        CHECK(isVerifiedFresh);
    }

    // --- atlasRectForSlot is the glyph BOX; atlasRectForBuilder carries the phase headroom.
    //
    // computeGlyphSlot builds a slot as `glyphWidth + 2*marginX + 1`, the odd
    // pixel being where a glyph shifted for its subpixel phase lands. So
    // `slotWidth - 2*marginX` is one pixel WIDER than the box, and the two rects
    // cannot coincide on any slot a policy produces.
    //
    // The characterization above runs at marginX = 0, where the two still differ
    // but the difference is not what it asserts. This states it directly, and is
    // what fails if the two are ever collapsed into one formula again.
    {
        auto approx = [](float a, float b) { return std::fabs(a - b) < 1e-6f; };
        Ubiquitty::Rendering::GlyphSlotGeometry g;
        g.columns = 4;
        g.glyphWidthInPixels = 10;
        g.glyphMarginInPixels.width = 2;
        g.glyphMarginInPixels.height = 1;
        g.slotWidthInPixels = g.glyphWidthInPixels + 2 * g.glyphMarginInPixels.width + 1; // 15
        g.slotHeightInPixels = 20;
        const int capacity = 16;

        const Ubiquitty::Rendering::GlyphUV box = g.atlasRectForSlot(0, capacity);
        const Ubiquitty::Rendering::GlyphUV built = g.atlasRectForBuilder(0, capacity);

        // Same left edge — both start after the margin.
        CHECK(approx(box.minU, built.minU));
        // The box is exactly glyphWidth wide.
        CHECK(approx(box.maxU - box.minU, static_cast<float>(g.glyphWidthInPixels)));
        // The builder's rect is one pixel wider: that is the headroom column.
        CHECK(approx(built.maxU - built.minU, static_cast<float>(g.glyphWidthInPixels + 1)));
        CHECK(approx(built.maxU - box.maxU, 1.0f));
    }
}

// The cache keys on 64 bits: two keys that agree in their low word and differ
// only above it — the same glyph at two smoothing tiers, or under an axis a
// later key adds above the tier — take two slots, and each probe finds its
// own. A cache that folded the high word away would hand the dark tier the
// light tier's raster and nothing would report it.
TEST(keysDifferingAboveTheLowWordTakeDistinctSlots)
{
    using Ubiquitty::Rendering::GlyphSlotCache;
    GlyphSlotCache cache(16, 64, 4);
    const GlyphSlotCache::Key low = 0x00004100u;
    const GlyphSlotCache::Key tiered = low | (GlyphSlotCache::Key { 3 } << 32);
    const GlyphSlotCache::Key thick = tiered | (GlyphSlotCache::Key { 1 } << 35);
    bool isFresh = false;
    const int a = cache.slotForKey(low, &isFresh);
    CHECK(a >= 0 && isFresh);
    const int b = cache.slotForKey(tiered, &isFresh);
    CHECK(b >= 0 && isFresh);
    const int c = cache.slotForKey(thick, &isFresh);
    CHECK(c >= 0 && isFresh);
    CHECK(a != b && b != c && a != c);
    // Re-probing each finds its own slot, not fresh.
    CHECK(cache.slotForKey(low, &isFresh) == a && !isFresh);
    CHECK(cache.slotForKey(tiered, &isFresh) == b && !isFresh);
    CHECK(cache.slotForKey(thick, &isFresh) == c && !isFresh);
    // A 32-bit key widens to the tier-0 / slot-0 key.
    CHECK(cache.slotForKey(std::uint32_t { 0x00004100u }, &isFresh) == a && !isFresh);
    // Forgetting one tier leaves the others resident.
    cache.forgetKey(tiered);
    CHECK(cache.slotForKey(low, &isFresh) == a && !isFresh);
    CHECK(cache.slotForKey(tiered, &isFresh) >= 0 && isFresh);
}

TEST(testAColourSlotEdgeTapCannotShowTheNeighbour)
{
    Ubiquitty::Rendering::ColorAtlasBinding sheet;
    sheet.columns = 4;
    sheet.rows = 4;
    sheet.slotInPixels = 36;
    sheet.slotCapacity = 16;

    // Half a level of an 8-bit channel. Below this the neighbour cannot move
    // the output by one representable step whatever its colour.
    const float invisible = 1.0f / 512.0f;

    for (int slot : { 0, 5, 15 }) {
        const Ubiquitty::Rendering::GlyphUV uv = sheet.atlasRectForSlot(slot);
        const int column = slot % sheet.columns;
        const int firstTexel = column * sheet.slotInPixels;
        const int lastTexel = firstTexel + sheet.slotInPixels - 1;

        for (const float edge : { uv.minU, uv.maxU }) {
            // Linear sampling clamps at texel centres after interpolation.
            const float sample = std::clamp(edge, uv.minU + 0.5f, uv.maxU - 0.5f) - 0.5f;
            const float lower = std::floor(sample);
            const float upper = lower + 1.0f;
            const float upperWeight = sample - lower;

            // Whatever the pair, the weight landing outside the slot is what a
            // neighbour could contribute.
            float outside = 0.0f;
            if (lower < static_cast<float>(firstTexel) || lower > static_cast<float>(lastTexel))
                outside += 1.0f - upperWeight;
            if (upper < static_cast<float>(firstTexel) || upper > static_cast<float>(lastTexel))
                outside += upperWeight;

            CHECK_MSG(outside < invisible,
                "a tap at a slot edge must not weight a neighbour texel enough "
                "to change an 8-bit channel");
        }
    }
}

/// The full master and both halves share pixel-boundary coordinates, so the
/// UV-to-quad mapping neither stretches the image nor separates its halves.
TEST(theDoubleSheetHalvesMeetAtTheMidlineWithoutASeam)
{
    using Ubiquitty::Rendering::DoubleRegion;
    using Ubiquitty::Rendering::doubleRegionUV;

    // Neither pair is square, so grid and slot extents are not interchangeable
    // by value either — the types stop the call being written, and these
    // numbers say what it would have cost.
    const Ubiquitty::GridSize grid { 4, 2 };
    const Ubiquitty::IntSize slotSize { 24, 40 };
    const int slot = 5;

    const auto top = doubleRegionUV(slot, grid, slotSize, DoubleRegion::Top);
    const auto bottom = doubleRegionUV(slot, grid, slotSize, DoubleRegion::Bottom);
    const auto full = doubleRegionUV(slot, grid, slotSize, DoubleRegion::Full);
    CHECK(top.present && bottom.present && full.present);

    CHECK_MSG(top.maxUV.y == bottom.minUV.y,
        "the halves share the midline exactly; a gap draws a seam and an "
        "overlap draws one row twice");
    CHECK(top.minUV.y == full.minUV.y);
    CHECK(bottom.maxUV.y == full.maxUV.y);

    const float x0 = static_cast<float>((slot % grid.columns) * slotSize.width);
    const float yTop = static_cast<float>((slot / grid.columns) * slotSize.height);
    const float halfSlot = static_cast<float>(slotSize.height) * 0.5f;
    const float wellUnderHalfATexel = 1.0f / 1024.0f;

    // Outer edges and the shared midline name pixel boundaries.
    CHECK(std::fabs(full.minUV.y - yTop) < wellUnderHalfATexel);
    CHECK(std::fabs(full.maxUV.y
              - (yTop + static_cast<float>(slotSize.height)))
        < wellUnderHalfATexel);
    CHECK(std::fabs(top.maxUV.y - (yTop + halfSlot)) < wellUnderHalfATexel);

    for (const auto& region : { top, bottom, full }) {
        CHECK(std::fabs(region.minUV.x - x0) < wellUnderHalfATexel);
        CHECK(std::fabs(region.maxUV.x
                  - (x0 + static_cast<float>(slotSize.width)))
            < wellUnderHalfATexel);
    }

    // Slots run row-major over `columns`, which is the half of the grid extent
    // a transposition would swap: slot 5 shares its column with slot 1 and its
    // row with slot 4.
    const auto sameColumn = doubleRegionUV(1, grid, slotSize, DoubleRegion::Full);
    const auto sameRow = doubleRegionUV(4, grid, slotSize, DoubleRegion::Full);
    CHECK(sameColumn.minUV.x == full.minUV.x);
    CHECK(sameRow.minUV.y == full.minUV.y);
    CHECK(sameColumn.minUV.y != full.minUV.y);
    CHECK(sameRow.minUV.x != full.minUV.x);

    // A degenerate extent yields a zero rect that is not present, so a caller
    // cannot sample it by mistake.
    for (const auto& degenerate : { doubleRegionUV(0, Ubiquitty::GridSize { 0, 2 }, slotSize, DoubleRegion::Full),
             doubleRegionUV(0, Ubiquitty::GridSize { 4, 0 }, slotSize, DoubleRegion::Full),
             doubleRegionUV(0, grid, Ubiquitty::IntSize { 0, 40 }, DoubleRegion::Full),
             doubleRegionUV(0, grid, Ubiquitty::IntSize { 24, 0 }, DoubleRegion::Full) }) {
        CHECK(!degenerate.present);
    }
}

// The retained-reference pins, driven as two views over one shared cache.
// The property under test is the corruption they exist to prevent: an idle
// view's retained frame references slots by UV, and an eviction that hands
// one to a new key makes that view show another window's ink until its own
// next rebuild. Seen live as one-frame pane corruption under a tmux scroll
// and rolling glitches in every window beside a bench.
TEST(testRetainedSlotsSurviveAnotherViewsFlood)
{
    // Small on purpose: 8 slots means the flood must evict, and the assert is
    // that it can only evict what nobody retains.
    Ubiquitty::Rendering::GlyphSlotCache cache(8, 32, 4);
    const int viewA = cache.acquireRetainLease();

    // View A's frame: four glyphs, retained.
    cache.beginFrame();
    bool isFresh = false;
    std::array<int, 4> slotsA;
    for (int i = 0; i < 4; ++i) {
        slotsA[i] = cache.slotForKey(static_cast<std::uint32_t>(0x100 + i), &isFresh);
        CHECK(slotsA[i] >= 0);
    }
    cache.retainTouchedSlots(viewA);

    // View B floods: three frames of four distinct keys each, more than the
    // unpinned half holds, so eviction pressure is real on every frame.
    for (int frame = 0; frame < 3; ++frame) {
        cache.beginFrame();
        for (int i = 0; i < 4; ++i)
            cache.slotForKey(static_cast<std::uint32_t>(0x900 + frame * 4 + i), &isFresh);
    }

    // A's keys still resolve to A's slots: no flood frame was handed one.
    cache.beginFrame();
    for (int i = 0; i < 4; ++i) {
        const int slot = cache.slotForKey(static_cast<std::uint32_t>(0x100 + i), &isFresh);
        CHECK_MSG(slot == slotsA[i] && !isFresh, "a retained slot was evicted under another view's flood");
    }

    cache.releaseRetainLease(viewA);
}

// Pins full is a refusal, never a pinned eviction: with every slot retained,
// a new key gets -1 and the caller draws its fallback. Release opens the
// cache again — the pins are the lease's, not the slots'.
TEST(testAPinnedFullCacheRefusesThenReleases)
{
    Ubiquitty::Rendering::GlyphSlotCache cache(4, 16, 2);
    const int view = cache.acquireRetainLease();
    cache.beginFrame();
    bool isFresh = false;
    for (int i = 0; i < 4; ++i)
        CHECK(cache.slotForKey(static_cast<std::uint32_t>(0x30 + i), &isFresh) >= 0);
    cache.retainTouchedSlots(view);

    cache.beginFrame();
    CHECK_MSG(cache.slotForKey(0x99, &isFresh) == -1, "a fully pinned cache refuses rather than evicts");

    cache.releaseRetainLease(view);
    cache.beginFrame();
    CHECK_MSG(cache.slotForKey(0x99, &isFresh) >= 0, "released pins open the cache again");
}

// A pin's job ends with the last command buffer that could sample the slot.
// The ring bounds that: a slot untouched for retainEpochDepth of its view's
// frames has no in-flight consumer left, and holding it longer is how one
// window's history once pinned a shared sheet solid against another window's
// redraws — live: pinned=4239/4239 with 242 refusals a frame, every refusal
// drawing an unhealed run.
TEST(testAQuietViewsFootprintDrainsThroughTheRing)
{
    Ubiquitty::Rendering::GlyphSlotCache cache(8, 32, 4);
    const int view = cache.acquireRetainLease();
    bool isFresh = false;

    cache.beginFrame();
    cache.slotForKey(0x70, &isFresh);
    cache.slotForKey(0x71, &isFresh);
    cache.retainTouchedSlots(view);
    CHECK_MSG(cache.pinnedSlots() == 2, "the frame's touches are pinned");

    // Quiet frames: the view keeps drawing (rotating its ring) but touches
    // nothing new — a pane whose content settled.
    for (int frame = 0; frame < 3; ++frame) {
        cache.beginFrame();
        cache.retainTouchedSlots(view);
        CHECK_MSG(cache.pinnedSlots() == 2, "in-flight frames keep the pins");
    }
    cache.beginFrame();
    cache.retainTouchedSlots(view);
    CHECK_MSG(cache.pinnedSlots() == 0, "past the ring, nothing samples the slots and the pins drain");

    cache.releaseRetainLease(view);
}

// A slot the view touches every frame rides the ring's head forever: draining
// applies to what a view STOPPED drawing, never to what it still draws.
TEST(testATouchedEveryFrameSlotStaysPinned)
{
    Ubiquitty::Rendering::GlyphSlotCache cache(8, 32, 4);
    const int view = cache.acquireRetainLease();
    bool isFresh = false;
    for (int frame = 0; frame < 10; ++frame) {
        cache.beginFrame();
        cache.slotForKey(0x80, &isFresh);
        cache.retainTouchedSlots(view);
        CHECK_MSG(cache.pinnedSlots() >= 1, "a live slot never drains");
    }
    cache.releaseRetainLease(view);
}

// A lease whose owner stopped drawing holds pins that protect nothing — its
// command buffers completed long ago. Those pins yield to pressure: the
// refusal path reclaims them and retries, so an idle window cannot starve a
// busy one sharing the sheet.
TEST(testAStaleLeaseYieldsToPressureInsteadOfRefusing)
{
    Ubiquitty::Rendering::GlyphSlotCache cache(4, 16, 2);
    const int idleView = cache.acquireRetainLease();
    bool isFresh = false;

    cache.beginFrame();
    for (int i = 0; i < 4; ++i)
        CHECK(cache.slotForKey(static_cast<std::uint32_t>(0x60 + i), &isFresh) >= 0);
    cache.retainTouchedSlots(idleView);

    // The idle view never draws again; the busy one advances past the stale
    // line. Frames alone age the lease — no touches needed.
    for (int frame = 0; frame < 242; ++frame)
        cache.beginFrame();

    cache.beginFrame();
    CHECK_MSG(cache.slotForKey(0xB0, &isFresh) >= 0, "a stale lease's pins are reclaimed before a refusal");
    CHECK_MSG(cache.refusalsThisFrame() == 0, "and the reclaim leaves no refusal behind");

    cache.releaseRetainLease(idleView);
}

// The pressure throttle: a refusal or imminent refusal corrects immediately —
// those frames already drew or are about to draw wrong pixels — while plain
// crowding waits out the short throttle, which bounds the full-rebuild rate a
// busy screen can sustain.
TEST(testThePressureThrottleRanksWrongPixelsOverEconomy)
{
    Ubiquitty::Rendering::SheetPressureThrottle throttle;
    CHECK_MSG(throttle.wantsCorrection(true, false, false), "an unarmed throttle lets crowding correct");
    throttle.noteRebaselined();
    CHECK_MSG(!throttle.wantsCorrection(true, false, false), "armed, crowding alone waits");
    CHECK_MSG(throttle.wantsCorrection(false, false, true), "a refusal does not");
    CHECK_MSG(throttle.wantsCorrection(false, true, false), "and neither does imminent refusal");
    // The wait is bounded: the throttle re-opens after its frame count.
    Ubiquitty::Rendering::SheetPressureThrottle counting;
    counting.noteRebaselined();
    int waited = 0;
    while (!counting.wantsCorrection(true, false, false) && waited < 100)
        ++waited;
    CHECK_MSG(waited == Ubiquitty::Rendering::SheetPressureThrottle::throttleFrames - 1,
        "crowding corrects again once the throttle lapses");
}

// A cache nobody leases allocates exactly as a cache without pins would:
// a frame that touches every slot gets a refusal for the next key, not a
// rescue from the recency list — the portable and Vulkan arms render
// against goldens minted under that refusal. And the refusal goes
// uncounted, because the counter asks a retained set to shrink and no
// re-baseline unblocks a frame that touched every slot itself.
TEST(testALeaselessFullFrameRefusesUncounted)
{
    Ubiquitty::Rendering::GlyphSlotCache cache(4, 16, 2);
    bool isFresh = false;
    cache.beginFrame();
    for (int i = 0; i < 4; ++i)
        CHECK(cache.slotForKey(static_cast<std::uint32_t>(0x50 + i), &isFresh) >= 0);

    CHECK_MSG(cache.slotForKey(0x99, &isFresh) == -1, "a frame that touched every slot is refused, not rescued");
    CHECK_MSG(cache.refusalsThisFrame() == 0, "a current-frame refusal does not ask the retained set to shrink");
    CHECK_MSG(cache.saturationsThisFrame() == 1, "and it is counted where it can be seen: the frame outgrew the capacity");

    cache.beginFrame();
    CHECK_MSG(cache.saturationsThisFrame() == 0, "the tally is the frame's, not the cache's lifetime");
    CHECK_MSG(cache.slotForKey(0x99, &isFresh) >= 0, "the next frame evicts normally");
}

// The two refusals are counted apart because their remedies differ: one asks a
// neighbouring view's retained set to shrink, the other says this frame alone
// needs more slots than exist. A single number cannot tell a pinning problem
// from a capacity one, and the fallback both draw is identical from outside.
TEST(theTwoRefusalsAreCountedApart)
{
    Ubiquitty::Rendering::GlyphSlotCache cache(4, 16, 2);
    bool isFresh = false;
    const int lease = cache.acquireRetainLease();
    cache.beginFrame();
    for (int i = 0; i < 4; ++i)
        CHECK(cache.slotForKey(static_cast<std::uint32_t>(0x60 + i), &isFresh) >= 0);
    cache.retainTouchedSlots(lease);

    cache.beginFrame();
    CHECK_MSG(cache.slotForKey(0x99, &isFresh) == -1, "every slot is pinned by the retained frame");
    CHECK_MSG(cache.refusalsThisFrame() == 1, "a pin-blocked refusal asks the retained set to shrink");
    CHECK_MSG(cache.saturationsThisFrame() == 0, "and it is not the frame outgrowing the capacity");
}

// Retention ACCUMULATES across frames, because a retained-grid view
// rebuilds only its dirty rows: one frame's touches cover those rows alone,
// and pins that followed a single frame would abandon every clean row.
// The slack this accumulates — slots referenced only by content that has
// scrolled away — is dropped by an explicit re-baseline, which the view
// schedules off the refusal counter after a full rebuild.
TEST(testRetentionAccumulatesAndRebaselinesExactly)
{
    Ubiquitty::Rendering::GlyphSlotCache cache(4, 16, 2);
    const int view = cache.acquireRetainLease();
    bool isFresh = false;

    // Frame 1 rebuilds rows carrying two glyphs; frame 2 rebuilds a
    // DIFFERENT dirty row with one more. All three stay pinned — the frame-2
    // build said nothing about the first rows' liveness.
    cache.beginFrame();
    cache.slotForKey(0x40, &isFresh);
    cache.slotForKey(0x41, &isFresh);
    cache.retainTouchedSlots(view);
    cache.beginFrame();
    cache.slotForKey(0x42, &isFresh);
    cache.retainTouchedSlots(view);

    cache.beginFrame();
    CHECK_MSG(cache.slotForKey(0xA0, &isFresh) >= 0, "one unpinned slot remains takeable");
    CHECK_MSG(cache.slotForKey(0xA1, &isFresh) == -1, "all other slots are pinned across builds");
    CHECK(cache.refusalsThisFrame() == 1);

    // The full rebuild: re-baseline, then touch exactly the live content.
    // The slack (0x40, 0x41) unpins; the live pair stays protected.
    cache.rebaselineRetainedSlots(view);
    cache.beginFrame();
    cache.slotForKey(0x42, &isFresh);
    cache.slotForKey(0xA0, &isFresh);
    cache.retainTouchedSlots(view);

    cache.beginFrame();
    CHECK_MSG(cache.slotForKey(0xB0, &isFresh) >= 0, "re-baselined slack is evictable again");
    CHECK_MSG(cache.slotForKey(0xB1, &isFresh) >= 0, "both stale slots came back");
    CHECK(cache.slotForKey(0x42, &isFresh) >= 0 && !isFresh);

    cache.releaseRetainLease(view);
}

// A slot re-touched over many frames pins once per lease, not once per
// frame: the membership byte is what keeps accumulation from inflating the
// count, and release must return the cache to exactly open.
TEST(testAccumulationPinsEachSlotOncePerLease)
{
    Ubiquitty::Rendering::GlyphSlotCache cache(4, 16, 2);
    const int view = cache.acquireRetainLease();
    bool isFresh = false;
    for (int frame = 0; frame < 5; ++frame) {
        cache.beginFrame();
        cache.slotForKey(0x40, &isFresh);
        cache.retainTouchedSlots(view);
    }
    cache.releaseRetainLease(view);
    // One release must fully unpin: fill the cache plus one to force an
    // eviction of that very slot.
    cache.beginFrame();
    for (int i = 0; i < 4; ++i)
        CHECK(cache.slotForKey(static_cast<std::uint32_t>(0x80 + i), &isFresh) >= 0);
    cache.beginFrame();
    CHECK_MSG(cache.slotForKey(0x99, &isFresh) >= 0, "a released lease leaves nothing pinned");
}

// A sheet cut for one screen cannot serve the screen after it: the first
// frame's pins hold every slot, so every glyph of the second frame is refused
// and draws blank — the cell that vanishes and comes back a second later,
// once the rate-limited re-baseline is finally allowed. Cutting the sheet for
// two screens is what makes the refusal impossible, and the one-screen sheet
// beside it is the control that says so.
TEST(aSheetCutForTwoScreensServesTheScreenAfterThePinsAreTaken)
{
    constexpr int screenCells = 64;
    auto refusalsServingASecondScreen = [](int capacity) {
        Ubiquitty::Rendering::GlyphSlotCache cache(capacity, 4 * capacity, 8);
        const int view = cache.acquireRetainLease();
        bool isFresh = false;
        cache.beginFrame();
        for (int cell = 0; cell < screenCells; ++cell)
            cache.slotForKey(static_cast<std::uint32_t>(0x1000 + cell), &isFresh);
        cache.retainTouchedSlots(view);
        // A whole screen of content the first frame never showed: a scroll,
        // or the eight subpixel phases of a line that moved by a fraction.
        cache.beginFrame();
        for (int cell = 0; cell < screenCells; ++cell)
            cache.slotForKey(static_cast<std::uint32_t>(0x2000 + cell), &isFresh);
        const std::uint32_t refusals = cache.refusalsThisFrame();
        cache.releaseRetainLease(view);
        return refusals;
    };

    CHECK_MSG(refusalsServingASecondScreen(screenCells) > 0,
        "a sheet cut for one screen must refuse — otherwise this test proves nothing");
    CHECK_MSG(refusalsServingASecondScreen(2 * screenCells) == 0,
        "a sheet cut for two screens refused a slot while a screen's pins were held");
}

// The pins are counted so a view can re-baseline before the refusal, not
// after it. Half the sheet is the line, because a sheet cut for two screens
// still holds a whole screen of evictable slots at that point.
TEST(crowdingIsReportedWhileEveryCellStillDraws)
{
    Ubiquitty::Rendering::GlyphSlotCache cache(16, 64, 4);
    const int view = cache.acquireRetainLease();
    bool isFresh = false;
    CHECK(cache.pinnedSlots() == 0);
    CHECK(cache.evictableSlots() == 16);
    CHECK(!Ubiquitty::Rendering::pinsCrowdOutNextFrame(cache.capacity(), cache.pinnedSlots()));

    cache.beginFrame();
    for (int i = 0; i < 8; ++i)
        cache.slotForKey(static_cast<std::uint32_t>(0x10 + i), &isFresh);
    cache.retainTouchedSlots(view);
    CHECK_MSG(cache.pinnedSlots() == 8, "a frame's touches are the lease's pins");
    CHECK(cache.evictableSlots() == 8);
    CHECK_MSG(!Ubiquitty::Rendering::pinsCrowdOutNextFrame(cache.capacity(), cache.pinnedSlots()),
        "half a sheet of pins is the sheet doing its job, not crowding");
    CHECK_MSG(cache.refusalsThisFrame() == 0, "crowding is read before any refusal happens");

    // A second frame of new content accumulates onto the same lease, which is
    // the slack a dirty-row build leaves behind.
    cache.beginFrame();
    for (int i = 0; i < 4; ++i)
        cache.slotForKey(static_cast<std::uint32_t>(0x40 + i), &isFresh);
    cache.retainTouchedSlots(view);
    CHECK(cache.pinnedSlots() == 12);
    CHECK_MSG(Ubiquitty::Rendering::pinsCrowdOutNextFrame(cache.capacity(), cache.pinnedSlots()),
        "pins past half the sheet must be reported before they refuse");

    cache.rebaselineRetainedSlots(view);
    CHECK_MSG(cache.pinnedSlots() == 0, "a re-baseline drops the whole retained set");
    CHECK(!Ubiquitty::Rendering::pinsCrowdOutNextFrame(cache.capacity(), cache.pinnedSlots()));
    cache.releaseRetainLease(view);
}

TEST(inkMeasurementsBelongToTheCurrentSlotIdentity)
{
    GlyphSlotCache cache(1, 4, 1);
    cache.enableOwnedInkRecords(1, 16);
    bool isFresh = false;
    const int slot = cache.slotForKey(100, &isFresh);
    CHECK(isFresh);
    cache.beginOwnedInkRecord(slot);
    cache.recordShapedGlyphInk(slot, 0, 0, 1, 2, 9, 0);
    CHECK(cache.slotHasOwnedInk(slot));
    CHECK(cache.ownedInkForSlot(slot, 0).rightInPixels == 9.0f);
    CHECK(cache.slotForKey(200, &isFresh) == slot);
    CHECK(isFresh);
    CHECK(!cache.slotHasOwnedInk(slot));
    cache.beginOwnedInkRecord(slot);
    cache.forgetKey(200);
    CHECK(!cache.slotHasOwnedInk(slot));
    CHECK(cache.slotForKey(300, &isFresh) == slot);
    cache.beginOwnedInkRecord(slot);
    cache.reset();
    CHECK(!cache.slotHasOwnedInk(slot));
}

// The fringe widens what a column draws and is settled against its neighbour;
// the ink keeps the extent the producer reported, overlap and all.
TEST(aFringeWidensTheDrawnSpanAndNeverTheInk)
{
    GlyphSlotCache cache(1, 4, 1);
    cache.enableOwnedInkRecords(2, 32);
    bool isFresh = false;
    const int slot = cache.slotForKey(100, &isFresh);
    cache.beginOwnedInkRecord(slot);
    cache.recordShapedGlyphInk(slot, 0, 0, 2, 2, 12, 1);
    cache.recordShapedGlyphInk(slot, 1, 0, 2, 10, 20, 1);
    cache.finalizeOwnedInkRecord(slot);
    const auto first = cache.ownedInkForSlot(slot, 0);
    const auto second = cache.ownedInkForSlot(slot, 1);
    CHECK_MSG(first.leftInPixels == 1.0f, "the drawn span starts a fringe before the ink");
    CHECK_MSG(first.rightInPixels == 11.0f && second.leftInPixels == 11.0f,
        "and the contested span is split at its midpoint");
    CHECK_MSG(second.rightInPixels == 21.0f, "the outer edge keeps its fringe");
    CHECK_MSG(first.inkLeftInPixels == 2.0f && first.inkRightInPixels == 12.0f, "the first column's ink is as recorded");
    CHECK_MSG(second.inkLeftInPixels == 10.0f && second.inkRightInPixels == 20.0f, "and so is the second's");
}

// A strip's inked rows are measured from its raster, exactly, and go with the
// slot's raster: a slot handed to another key holds nothing
// they describe.
TEST(aStripsInkedRowsComeFromItsRasterAndLeaveWithIt)
{
    GlyphSlotCache cache(1, 4, 1);
    cache.enableOwnedInkRecords(1, 4);
    bool isFresh = false;
    const int slot = cache.slotForKey(100, &isFresh);
    CHECK_MSG(!cache.inkRowsForSlot(slot).isMeasured, "a slot no raster reported is unmeasured");
    std::vector<std::uint8_t> plane(4 * 6, 0);
    plane[2 * 4 + 1] = 200; // row 2
    plane[3 * 4 + 2] = 10; // row 3
    cache.recordInkRows(slot, plane, 4, 6);
    const GlyphSlotCache::InkRows rows = cache.inkRowsForSlot(slot);
    CHECK(rows.isMeasured);
    CHECK_MSG(rows.topInPixels == 2.0f && rows.bottomInPixels == 4.0f, "rows 2 and 3: the first inked row and one past the last");
    std::ranges::fill(plane, std::uint8_t { 0 });
    cache.recordInkRows(slot, plane, 4, 6);
    CHECK_MSG(cache.inkRowsForSlot(slot).isMeasured && cache.inkRowsForSlot(slot).bottomInPixels <= cache.inkRowsForSlot(slot).topInPixels,
        "a blank raster is measured and empty");
    CHECK(cache.slotForKey(200, &isFresh) == slot);
    CHECK_MSG(!cache.inkRowsForSlot(slot).isMeasured, "a slot taken by another key forgets the rows");
}

TEST(failedVerifiedRasterForgetsOnlyItsExactCollider)
{
    GlyphSlotCache cache(4, 8, 2);
    cache.enableOwnedInkRecords(1, 16);
    cache.beginFrame();
    constexpr std::uint64_t key = 0x88000020u;
    const std::uint32_t first[] = { 'a', 'b' };
    const std::uint32_t second[] = { 'a', 'c' };
    bool isFresh = false;
    const int firstSlot = cache.slotForKeyVerified(key, first, 2, &isFresh);
    CHECK(firstSlot >= 0 && isFresh);
    cache.beginOwnedInkRecord(firstSlot);
    const int failedSlot = cache.slotForKeyVerified(key, second, 2, &isFresh);
    CHECK(failedSlot >= 0 && failedSlot != firstSlot && isFresh);
    cache.beginOwnedInkRecord(failedSlot);
    cache.forgetKeyAtSlot(key, failedSlot);
    CHECK(cache.slotHasOwnedInk(firstSlot));
    CHECK(!cache.slotHasOwnedInk(failedSlot));
    CHECK(cache.slotForKeyVerified(key, first, 2, &isFresh) == firstSlot);
    CHECK(!isFresh);
    CHECK(cache.slotForKeyVerified(key, second, 2, &isFresh) == failedSlot);
    CHECK(isFresh);
}

UBIQUITTY_TEST_MAIN()

TEST(colourSlotsMapEveryDestinationPixelToItsOwnTexel)
{
    using Ubiquitty::Rendering::ColorAtlasBinding;
    for (const int side : { 1, 14, 28, 36 }) {
        ColorAtlasBinding sheet;
        sheet.columns = 4;
        sheet.rows = 4;
        sheet.slotInPixels = side;
        sheet.slotCapacity = 16;
        for (const int slot : { 0, 5, 15 }) {
            const auto rect = sheet.atlasRectForSlot(slot);
            for (int pixel = 0; pixel < side; ++pixel) {
                const float fraction = (pixel + 0.5f) / side;
                CHECK(std::abs(rect.minU + fraction * (rect.maxU - rect.minU)
                          - (static_cast<float>((slot % sheet.columns) * side + pixel) + 0.5f))
                    < 0.0001f);
                CHECK(std::abs(rect.minV + fraction * (rect.maxV - rect.minV)
                          - (static_cast<float>((slot / sheet.columns) * side + pixel) + 0.5f))
                    < 0.0001f);
            }
        }
    }
}

// A frame can give back the slots it assigned after a mark, and only those:
// a key resolved before the mark, or resident from an earlier frame, keeps its
// slot, and the slots given back are the next ones the frame assigns.
TEST(forgetSlotsAssignedSinceGivesBackOnlyTheFramesNewSlots)
{
    GlyphSlotCache cache(4, Ubiquitty::Rendering::recommendedHashBuckets(4), 4);
    cache.beginFrame();
    bool isFresh = false;
    const int resident = cache.slotForKey(1, &isFresh);
    CHECK(resident >= 0 && isFresh);
    cache.beginFrame();
    CHECK(cache.slotForKey(1, &isFresh) == resident && !isFresh);
    const int before = cache.slotForKey(2, &isFresh);
    CHECK(isFresh);
    const int mark = cache.slotsAssignedThisFrame();
    const int first = cache.slotForKey(3, &isFresh);
    const int second = cache.slotForKey(4, &isFresh);
    CHECK(first >= 0 && second >= 0);
    // Full, every slot touched this frame: a fifth key is refused.
    CHECK(cache.slotForKey(5, &isFresh) < 0);
    cache.forgetSlotsAssignedSince(mark);
    CHECK(cache.slotsAssignedThisFrame() == mark);
    // The keys from before the mark are still where they were.
    CHECK(cache.slotForKey(1, &isFresh) == resident && !isFresh);
    CHECK(cache.slotForKey(2, &isFresh) == before && !isFresh);
    // The given-back slots take the next keys, within the same frame.
    const int reused = cache.slotForKey(5, &isFresh);
    CHECK(isFresh && (reused == first || reused == second));
    CHECK(cache.slotForKey(6, &isFresh) >= 0 && isFresh);
    CHECK(cache.slotForKey(3, &isFresh) < 0);
}
