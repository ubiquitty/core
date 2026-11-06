// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_glyph_key — pins the portable glyph-atlas KEY derivations
// (Source/Core/Rendering/GlyphKey.h) to the exact bit folds the macOS Metal
// renderer's glyph atlas uses, so Metal and Vulkan key a process-wide shared
// atlas byte-identically. Expected values were computed from an independent
// transcription of the spec (offset basis 2166136261, prime 16777619, the bit
// layouts, the sentinel dodges) — a drift in either GlyphKey.h or the renderer
// breaks this test. Also includes CellInstance.h so its layout static_asserts
// compile here (the instance-struct half of the contract).

#include "Core/Rendering/CellInstance.h"
#include "Core/Rendering/GlyphBackendTypes.h"
#include "Core/Rendering/GlyphKey.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace Ubiquitty::Rendering;

TEST(proportionalGlyphIdentityKeepsEveryBitAndRasterAxis)
{
    const std::uint64_t basic = proportionalGlyphKey(0x41u, 0, FontFace::Regular, 0);
    CHECK(basic != proportionalGlyphKey(0x10041u, 0, FontFace::Regular, 0));
    CHECK(basic != proportionalGlyphKey(0x80000041u, 0, FontFace::Regular, 0));
    CHECK(basic != proportionalGlyphKey(0x41u, 1, FontFace::Regular, 0));
    CHECK(basic != proportionalGlyphKey(0x41u, 0, FontFace::Bold, 0));
    CHECK(basic != proportionalGlyphKey(0x41u, 0, FontFace::Italic, 0));
    CHECK(basic != proportionalGlyphKey(0x41u, 0, FontFace::Regular, 1));
    CHECK(basic != proportionalGlyphKey(0x41u, 0, FontFace::Regular, 0, 1));
    CHECK(proportionalGlyphKey(0xFFFFFFFFu, 7, FontFace::BoldItalic, 7, 7)
        != UINT64_MAX);
}

TEST(glyphKey)
{

    // --- mono text key: cp<<8 | weight<<5 | italic | bold | phase ---
    CHECK(monoGlyphKey(0x41u, 0u, FontFace::Regular, 0u) == 0x00004100u);
    CHECK(monoGlyphKey(0x41u, 0u, FontFace::Bold, 0u) == 0x00004108u);
    CHECK(monoGlyphKey(0x41u, 0u, FontFace::Italic, 0u) == 0x00004110u);
    CHECK(monoGlyphKey(0x41u, 3u, FontFace::Regular, 0u) == 0x00004160u);
    CHECK(monoGlyphKey(0x41u, 0u, FontFace::Regular, 2u) == 0x00004102u);
    CHECK(monoGlyphKey(0x10FFFFu, 7u, FontFace::BoldItalic, 7u) == 0x10FFFFFFu);

    // --- box/block-from-font key: 0x40000000 | cp<<5 | italic | bold | weight ---
    CHECK(boxFontKey(0x2500u, 0u, FontFace::Regular) == 0x4004A000u);
    CHECK(boxFontKey(0x2500u, 0u, FontFace::Bold) == 0x4004A008u);
    CHECK(boxFontKey(0x2500u, 0u, FontFace::Italic) == 0x4004A010u);
    // Variable weight is a key axis: the raster applies it, so each weight
    // owns a distinct slot (a weight change must never serve the stale stroke).
    CHECK(boxFontKey(0x2500u, 5u, FontFace::Regular) == 0x4004A005u);
    CHECK(boxFontKey(0x2500u, 5u, FontFace::Regular) != boxFontKey(0x2500u, 0u, FontFace::Regular));

    // --- wide key: cp<<5 | weight<<2 | italic<<1 | bold (distinct fold) ---
    CHECK(wideGlyphKey(0x4E00u, 0u, FontFace::Regular) == 0x0009C000u);
    CHECK(wideGlyphKey(0x4E00u, 0u, FontFace::Bold) == 0x0009C001u);
    CHECK(wideGlyphKey(0x4E00u, 0u, FontFace::Italic) == 0x0009C002u);
    CHECK(wideGlyphKey(0x4E00u, 5u, FontFace::Regular) == 0x0009C014u);
    // Max codePoint at max weight/style keeps the backend tag bits clear.
    CHECK((wideGlyphKey(0x10FFFFu, 7u, FontFace::BoldItalic) & 0xC0000000u) == 0u);
    // The phase takes bits 26-28, clear of the code point and of the shared tag.
    CHECK(phasedWideGlyphKey(0x4E00u, 0u, FontFace::Regular, 0u) == wideGlyphKey(0x4E00u, 0u, FontFace::Regular));
    CHECK(phasedWideGlyphKey(0x4E00u, 0u, FontFace::Regular, 3u) == (0x0009C000u | (3u << 26)));
    CHECK((phasedWideGlyphKey(0x10FFFFu, 7u, FontFace::BoldItalic, 7u) & 0x60000000u) == 0u);
    CHECK(phasedWideGlyphKey(0x10FFFFu, 7u, FontFace::BoldItalic, 7u) != phasedWideGlyphKey(0x10FFFFu, 7u, FontFace::BoldItalic, 6u));

    // --- double (DWL/DHL) master key: cp<<5 | weight<<2 | italic<<1 | bold.
    // A DHL top/bottom pair derives the same key, so both rows share one 2x master ---
    CHECK(doubleMasterKey(0x2588u, 0u, FontFace::Regular) == 0x0004B100u);
    CHECK(doubleMasterKey(0x2588u, 0u, FontFace::Bold) == 0x0004B101u);
    CHECK(doubleMasterKey(0x2588u, 0u, FontFace::Italic) == 0x0004B102u);
    CHECK(doubleMasterKey(0x2588u, 0u, FontFace::BoldItalic) == 0x0004B103u);
    CHECK(doubleMasterKey(0x2588u, 3u, FontFace::Regular) == 0x0004B10Cu);
    // A DECDWL master drawn from the widened face keys apart from the uniform
    // master the DECDHL pair shares, for every code point, weight and face.
    CHECK(doubleWidthMasterKey(0x2588u, 0u, FontFace::Regular) == 0x4004B100u);
    {
        bool isEveryKeyApart = true;
        const FontFace faces[] = { FontFace::Regular, FontFace::Bold, FontFace::Italic, FontFace::BoldItalic };
        for (const std::uint32_t codePoint : { 0x20u, 0x48u, 0x6771u, 0xFFFFu, 0x10FFFFu })
            for (std::uint32_t weight = 0; weight < 8; ++weight)
                for (const FontFace face : faces)
                    isEveryKeyApart = isEveryKeyApart && (doubleWidthMasterKey(codePoint, weight, face) & 0x40000000u) != 0
                        && (doubleMasterKey(codePoint, weight, face) & 0x40000000u) == 0;
        CHECK_MSG(isEveryKeyApart, "bit 30 separates the widened masters from the uniform ones");
    }

    // --- single color emoji: raw codePoint ---
    CHECK(colorGlyphKey(0x1F600u) == 0x0001F600u);

    // --- color cluster: n<=1 raw; else FNV-1a | bit31 ---
    {
        const std::uint32_t one[] = { 0x1F600u };
        CHECK(colorClusterKey(one, 1) == 0x0001F600u);
    }
    {
        const std::uint32_t fam[] = { 0x1F468u, 0x200Du, 0x1F469u };
        CHECK(colorClusterKey(fam, 3) == 0x881683B9u);
    }

    // --- coverage cluster: bit31 | (FNV&0x7FFFFF)<<5 | italic | bold | phase ---
    {
        const std::uint32_t cc[] = { 0x65u, 0x301u };
        CHECK(coverageClusterKey(cc, 2, 0, FontFace::Regular) == 0x88DADA00u);
    }
    {
        const std::uint32_t cc[] = { 0x65u, 0x301u };
        CHECK(coverageClusterKey(cc, 2, 3, FontFace::BoldItalic) == 0x88DADA1Bu);
    }

    // --- ligature strip: 0xC0000000 | (FNV&0x03FFFFFF)<<4 | (phase&0x7)<<1 | bold ---
    {
        const std::uint32_t eq[] = { 0x3Du, 0x3Du }; // "=="
        CHECK(ligatureStripKey(eq, 2, FontFace::Regular, 0) == 0xD76D4950u);
        CHECK(ligatureStripKey(eq, 2, FontFace::Bold, 0) == 0xD76D4951u);
        const std::uint32_t eqgt[] = { 0x3Du, 0x3Eu }; // "=>"
        CHECK(ligatureStripKey(eqgt, 2, FontFace::Regular, 0) == 0xDA313A60u);
        const std::uint32_t neq[] = { 0x21u, 0x3Du }; // "!="
        CHECK(ligatureStripKey(neq, 2, FontFace::Regular, 0) == 0xCC540A90u);
        const std::uint32_t spc[] = { 0x3Cu, 0x3Du, 0x3Eu }; // "<=>"
        CHECK(ligatureStripKey(spc, 3, FontFace::Regular, 0) == 0xEDBCE6A0u);
        CHECK(ligatureStripKey(spc, 3, FontFace::Bold, 0) == 0xEDBCE6A1u);
        // phase is an independent low-bit axis: same run, phase 3 => (3<<1) higher
        CHECK(ligatureStripKey(eq, 2, FontFace::Regular, 3) == 0xD76D4956u);
    }

    // --- keyspace-partition invariants (these must hold or keys collide) ---
    // Text keys keep both top bits clear, even at the codePoint maximum.
    CHECK((monoGlyphKey(0x10FFFFu, 7u, FontFace::BoldItalic, 7u) & 0xC0000000u) == 0u);
    // Box keys set bit 30 only.
    CHECK((boxFontKey(0x2500u, 7u, FontFace::Regular) & 0xC0000000u) == 0x40000000u);
    // Cluster keys set bit 31; coverage clusters leave bit 30 clear.
    {
        const std::uint32_t fam[] = { 0x1F468u, 0x200Du, 0x1F469u };
        CHECK((colorClusterKey(fam, 3) & 0x80000000u) != 0u);
        const std::uint32_t cc[] = { 0x65u, 0x301u };
        CHECK((coverageClusterKey(cc, 2, 0, FontFace::Regular) & 0xC0000000u) == 0x80000000u);
    }
    // No derivation may ever produce the reserved empty-bucket sentinel.
    CHECK(monoGlyphKey(0x10FFFFu, 7u, FontFace::BoldItalic, 7u) != emptyGlyphKey);
    {
        const std::uint32_t empty[] = { 0u };
        CHECK(coverageClusterKey(empty, 0, 7, FontFace::BoldItalic) != emptyGlyphKey);
    }

    // --- determinism + order sensitivity of the FNV fold ---
    //
    // Determinism is asserted across two DISTINCT arrays holding the same code
    // points, not by calling the fold twice on one. The failure worth catching
    // is a key derived from anything but the content — an address mixed in, a
    // counter, uninitialised padding — and every one of those returns the same
    // answer twice for one array while giving a different one for a copy. The
    // cluster pool hands the fold a different buffer on every occurrence of the
    // same emoji, so a key that moved with the buffer would give each occurrence
    // its own atlas slot and quietly exhaust the sheet.
    {
        const std::uint32_t a[] = { 0x41u, 0x42u, 0x43u };
        const std::uint32_t sameContentDifferentStorage[] = { 0x41u, 0x42u, 0x43u };
        const std::uint32_t b[] = { 0x43u, 0x42u, 0x41u };
        CHECK(&a[0] != &sameContentDifferentStorage[0]);
        CHECK(colorClusterKey(a, 3) == colorClusterKey(sameContentDifferentStorage, 3));
        CHECK(colorClusterKey(a, 3) != colorClusterKey(b, 3));
    }

    // --- ligature strip invariants: tag bits set, sentinel-safe, distinct from
    //     the cluster fold of the same run, and order/content/bold sensitive
    //     (its own atlas, but these guard a copy-pasted or weakened fold) ---
    {
        const std::uint32_t eq[] = { 0x3Du, 0x3Du }; // "=="
        const std::uint32_t neq[] = { 0x21u, 0x3Du }; // "!="
        const std::uint32_t eqgt[] = { 0x3Du, 0x3Eu }; // "=>"
        const std::uint32_t gteq[] = { 0x3Eu, 0x3Du }; // ">="
        CHECK((ligatureStripKey(eq, 2, FontFace::Regular, 0) & 0xC0000000u) == 0xC0000000u);
        CHECK(ligatureStripKey(eq, 2, FontFace::Regular, 0) != emptyGlyphKey);
        CHECK(ligatureStripKey(eq, 2, FontFace::Regular, 0) != colorClusterKey(eq, 2));
        CHECK(ligatureStripKey(eq, 2, FontFace::Regular, 0) != ligatureStripKey(neq, 2, FontFace::Regular, 0)); // content
        CHECK(ligatureStripKey(eqgt, 2, FontFace::Regular, 0) != ligatureStripKey(gteq, 2, FontFace::Regular, 0)); // order
        CHECK(ligatureStripKey(eq, 2, FontFace::Regular, 0) != ligatureStripKey(eq, 2, FontFace::Bold, 0)); // bold
        CHECK(ligatureStripKey(eq, 2, FontFace::Regular, 0) != ligatureStripKey(eq, 2, FontFace::Regular, 3)); // subpixel phase
    }

    // --- the shared-atlas wide keyspace, swept over the WHOLE code point range
    //     rather than sampled ---
    //
    // Sampling cannot find this class. `wideGlyphKey` and `boxFontKey` both
    // shift the code point by 5, so tagging wide with box's bit alone makes the
    // two keyspaces identical at regular weight for every one of the 1114112
    // code points. A spot-check picks one pair and reports it wrong; the sweep
    // is what says whether the overlap is an edge or the whole space.
    {
        int monoCollisions = 0, boxCollisions = 0, drcsCollisions = 0;
        for (std::uint32_t codePoint = 0; codePoint <= 0x10FFFFu; ++codePoint) {
            const std::uint32_t wide = sharedAtlasWideGlyphKey(codePoint, 0u, FontFace::Regular);
            if (wide == monoGlyphKey(codePoint, 0u, FontFace::Regular, 0u))
                ++monoCollisions;
            if (wide == boxFontKey(codePoint, 0u, FontFace::Regular))
                ++boxCollisions;
            if (wide == drcsGlyphKey(codePoint, 0u))
                ++drcsCollisions;
        }
        CHECK_MSG(monoCollisions == 0, "shared-atlas wide keys collide with mono keys");
        CHECK_MSG(boxCollisions == 0, "shared-atlas wide keys collide with box keys");
        CHECK_MSG(drcsCollisions == 0, "shared-atlas wide keys collide with DRCS keys");

        // The predicate must answer no for the keyspaces that share a bit with
        // the tag — the case a test written as "the tag round-trips" never
        // reaches, and the one that decides whether it is a mask or a match.
        CHECK(isSharedAtlasWideGlyphKey(sharedAtlasWideGlyphKey(0x4E00u, 0u, FontFace::Regular)));
        CHECK(!isSharedAtlasWideGlyphKey(boxFontKey(0x2588u, 0u, FontFace::Regular)));
        CHECK(!isSharedAtlasWideGlyphKey(drcsGlyphKey(1u, 0u)));
        CHECK(!isSharedAtlasWideGlyphKey(monoGlyphKey(0x41u, 0u, FontFace::Regular, 0u)));
        CHECK(!isSharedAtlasWideGlyphKey(wideGlyphKey(0x4E00u, 0u, FontFace::Regular))); // untagged wide is a text key
        CHECK(sharedAtlasWideGlyphKey(0x10FFFFu, 7u, FontFace::BoldItalic) != emptyGlyphKey);
    }
}

UBIQUITTY_TEST_MAIN()

// Two kinds of key in one cache are kept apart by a tag, not by luck.
//
// The value cases above pin each fold on its own. They cannot see the property
// that matters when a cache holds more than one kind: a collision hands a lookup
// the slot another kind rasterised, so the atlas answers `present` with a real
// glyph at a plausible UV and the wrong one is drawn sharply. Nothing downstream
// counts differently.
//
// Membership is what the call sites do, not a matter of taste:
//
//     mono cache   monoGlyphKey (000), coverageClusterKey (100), drcsGlyphKey (001)
//     wide cache   wideGlyphKey (000), coverageClusterKey (100)
//
// `coverageClusterKey` is in both, which is what makes this worth asserting:
// `SoftwareRenderer::wideClusterGlyph` keys the WIDE cache with the mono cluster
// fold, so a dense low space (`wideGlyphKey` is `codePoint << 5`) sits beside a
// 23-bit hash. The top three bits are the whole separation.
//
// This asserts the TAG rather than comparing the two sets. Comparing sets is the
// obvious shape and it does not work: drop the tag and 160 cluster keys still
// miss 320 wide keys almost every time, so the comparison passes over any sweep
// small enough to write down, and passes against exactly that mutant. A
// structural claim fails on the first key instead.
namespace {

const std::uint32_t sweepCodePoints[] = {
    0x20u, 0x41u, 0x7Fu, 0x2500u, 0x2588u, 0x4E00u, 0xFFFFu, 0x10000u, 0x1F600u, 0x10FFFFu
};
const FontFace sweepFaces[] = { FontFace::Regular, FontFace::Bold, FontFace::Italic, FontFace::BoldItalic };

/// The clusters the sweep keys, chosen to include the shapes a renderer meets.
const std::vector<std::vector<std::uint32_t>>& sweepClusters()
{
    static const std::vector<std::vector<std::uint32_t>> clusters = {
        { 0x65u, 0x301u }, // e + combining acute
        { 0x4E00u, 0xFE0Fu }, // wide base + presentation selector
        { 0x1F468u, 0x200Du, 0x1F469u }, // ZWJ pair
        { 0x1F44Du, 0x1F3FDu }, // base + skin tone
        { 0x41u, 0x20u }, // two plain code points
    };
    return clusters;
}

/// The top three bits, which are what a cache uses to tell one kind from another.
///
/// - Parameter key: Any glyph key.
/// - Returns: Its tag, shifted down to 0..7.
std::uint32_t tagOf(std::uint32_t key) { return key >> 29; }

} // namespace

// The raster variant and the 64-bit slot key around the 32-bit derivations:
// the low word is the derivation every pinned value above names, so a bare
// 32-bit key is the tier-0 / slot-0 key, and the tier and thickness slot sit
// above it where no 32-bit key can reach.
TEST(rasterVariantAndSlotKey)
{
    using Ubiquitty::Rendering::glyphSlotKey;
    using Ubiquitty::Rendering::makeRasterVariant;
    using Ubiquitty::Rendering::rasterTierForLinearLuma;
    using Ubiquitty::Rendering::variantPhase;
    using Ubiquitty::Rendering::variantTier;

    const std::uint32_t v = makeRasterVariant(5u, 3u);
    CHECK(variantPhase(v) == 5u);
    CHECK(variantTier(v) == 3u);
    // The phase field is the key's 3 bits; the 32-bit derivations mask it
    // themselves, so a variant passed where a phase is expected keys right.
    CHECK(monoGlyphKey(0x41u, 0u, FontFace::Regular, v) == monoGlyphKey(0x41u, 0u, FontFace::Regular, 5u));
    CHECK(makeRasterVariant(9u, 0u) == 1u); // phase wraps into 3 bits
    CHECK(makeRasterVariant(0u, 9u) == (1u << 8)); // tier into 3 bits

    const std::uint32_t key32 = monoGlyphKey(0x41u, 0u, FontFace::Regular, 5u);
    CHECK(glyphSlotKey(key32, makeRasterVariant(5u, 0u)) == static_cast<std::uint64_t>(key32));
    CHECK(glyphSlotKey(key32, v) == (static_cast<std::uint64_t>(key32) | (std::uint64_t { 3 } << 32)));
    // Tiers key distinct slots; the phase stays in the low word.
    CHECK(glyphSlotKey(key32, makeRasterVariant(5u, 0u)) != glyphSlotKey(key32, makeRasterVariant(5u, 1u)));
    // No variant forms the cache's all-ones empty key.
    CHECK(glyphSlotKey(0xFFFFFFFFu, makeRasterVariant(7u, 7u)) != ~std::uint64_t { 0 });

    // CoreText's tiers, measured: thresholds at linear luma 0.125 / 0.375 /
    // 0.625 / 0.875, so tier = round(4 Y).
    CHECK(rasterTierForLinearLuma(0.0) == 0u);
    CHECK(rasterTierForLinearLuma(0.124) == 0u);
    CHECK(rasterTierForLinearLuma(0.126) == 1u);
    CHECK(rasterTierForLinearLuma(0.374) == 1u);
    CHECK(rasterTierForLinearLuma(0.376) == 2u);
    CHECK(rasterTierForLinearLuma(0.624) == 2u);
    CHECK(rasterTierForLinearLuma(0.626) == 3u);
    CHECK(rasterTierForLinearLuma(0.874) == 3u);
    CHECK(rasterTierForLinearLuma(0.876) == 4u);
    CHECK(rasterTierForLinearLuma(1.0) == 4u);
    // Black text is tier 0, white tier 4; #d0d0d0 (Y 0.631) is tier 3, the
    // tier that was a distinct raster at 12px and read as white at 24px.
    CHECK(rasterTierForLinearLuma(0.631) == 3u);
}

// A strip is keyed on the fraction its run's origin pixel drops, not on the
// phase bucket alone: a rasteriser that floors every pen of a line against
// the line's true origin moves different glyphs for two origins in one bucket.
// Both the code and the slot key it lands in are checked, because a code that
// collapsed two residuals, or a key that left the code out, each drew the
// same word a step from where it sits as single glyphs whenever a neighbour
// changed where its run began.
TEST(stripSlotKeyCarriesThePenResidual)
{
    using Ubiquitty::Rendering::glyphSlotKey;
    using Ubiquitty::Rendering::ligatureStripSlotKey;
    using Ubiquitty::Rendering::makeRasterVariant;
    using Ubiquitty::Rendering::penResidualCode;
    using Ubiquitty::Rendering::penResidualCodes;

    CHECK(penResidualCode(0.0) == 0u);
    CHECK(penResidualCode(-0.25) == 0u);
    CHECK(penResidualCode(0.5) == penResidualCodes / 2u);
    // Two residuals one phase bucket apart at 24px (halves) and two inside one
    // bucket both code apart: 0.36 and 0.72 share no code, nor do 0.36 and 0.40.
    CHECK(penResidualCode(0.36) != penResidualCode(0.72));
    CHECK(penResidualCode(0.36) != penResidualCode(0.40));
    // The code never reaches the count, so it fits the field the key gives it.
    CHECK(penResidualCode(0.999999) < penResidualCodes);
    CHECK(penResidualCode(1.0) == penResidualCodes - 1u);

    const std::uint32_t strip = 0xC1234560u;
    const std::uint32_t v = makeRasterVariant(1u, 3u);
    // A zero residual is the plain slot key, so a strip at the margin keys as
    // a plain slot.
    CHECK(ligatureStripSlotKey(strip, v, 0u) == glyphSlotKey(strip, v));
    // The residual sits above the tier and below nothing else that is set.
    CHECK(ligatureStripSlotKey(strip, v, 1u) == (glyphSlotKey(strip, v) | (std::uint64_t { 1 } << 40)));
    CHECK(ligatureStripSlotKey(strip, v, penResidualCode(0.36)) != ligatureStripSlotKey(strip, v, penResidualCode(0.72)));
    // The tier still separates keys under one residual.
    CHECK(ligatureStripSlotKey(strip, makeRasterVariant(1u, 0u), 7u) != ligatureStripSlotKey(strip, v, 7u));
}

TEST(everyKeyOfAKindCarriesItsKindsTag)
{
    int monoKeys = 0, clusterKeys = 0, drcsKeys = 0, wideKeys = 0;

    for (std::uint32_t codePoint : sweepCodePoints) {
        for (FontFace face : sweepFaces) {
            for (std::uint32_t weight = 0; weight < 8; ++weight) {
                for (std::uint32_t phase = 0; phase < 8; ++phase) {
                    CHECK(tagOf(monoGlyphKey(codePoint, weight, face, phase)) == 0u);
                    ++monoKeys;
                }
                CHECK(tagOf(wideGlyphKey(codePoint, weight, face)) == 0u);
                ++wideKeys;
            }
        }
    }
    for (const auto& points : sweepClusters()) {
        for (FontFace face : sweepFaces) {
            for (int phase = 0; phase < 8; ++phase) {
                CHECK(tagOf(coverageClusterKey(points.data(), static_cast<int>(points.size()), phase, face)) == 4u);
                ++clusterKeys;
            }
        }
    }
    for (std::uint32_t serial = 0; serial < 4; ++serial) {
        for (std::uint32_t index = 0; index < 128; ++index) {
            CHECK(tagOf(drcsGlyphKey(serial, index)) == 1u);
            ++drcsKeys;
        }
    }

    // The sweep is the thing that can silently become nothing, and an empty one
    // satisfies every assertion above.
    CHECK(monoKeys == 2560 && wideKeys == 320 && clusterKeys == 160 && drcsKeys == 512);

    // And the tags a shared cache relies on are distinct. Stated as values so a
    // kind moving onto another kind's tag fails here rather than at a wrong
    // glyph — the two caches below are the ones with more than one kind in them.
    CHECK(tagOf(monoGlyphKey(0x41u, 0u, FontFace::Regular, 0u))
        != tagOf(coverageClusterKey(sweepClusters()[0].data(), 2, 0, FontFace::Regular)));
    CHECK(tagOf(monoGlyphKey(0x41u, 0u, FontFace::Regular, 0u)) != tagOf(drcsGlyphKey(0u, 0u)));
    CHECK(tagOf(drcsGlyphKey(0u, 0u))
        != tagOf(coverageClusterKey(sweepClusters()[0].data(), 2, 0, FontFace::Regular)));
    CHECK(tagOf(wideGlyphKey(0x4E00u, 0u, FontFace::Regular))
        != tagOf(coverageClusterKey(sweepClusters()[1].data(), 2, 0, FontFace::Regular)));
}

// The shaped-glyph key is the one derivation whose input is an ANSWER rather
// than a question — the glyph the shaper picked, not the code point that asked
// — so what it must guarantee is that two different answers never share a slot
// and that no other kind in the shared atlas lands on its tag. Swept over the
// whole 16-bit glyph-id range rather than sampled, because the failure that
// matters is a field overlapping its neighbour, and that shows at the top of a
// range and nowhere else.
TEST(everyShapedGlyphKeyIsItsOwnAnswer)
{
    int shapedKeys = 0;
    for (std::uint32_t glyphID = 0; glyphID <= 0xFFFFu; ++glyphID) {
        const std::uint32_t key = shapedGlyphKey(glyphID, 0u, FontFace::Regular, 0u);
        CHECK(tagOf(key) == 7u); // bits 31|30|29
        CHECK(isShapedGlyphKey(key));
        CHECK(key != emptyGlyphKey);
        // Disjoint from every kind sharing the R8 atlas, at the same index.
        CHECK(key != monoGlyphKey(glyphID, 0u, FontFace::Regular, 0u));
        CHECK(key != boxFontKey(glyphID, 0u, FontFace::Regular));
        CHECK(key != drcsGlyphKey(glyphID, 0u));
        CHECK(key != sharedAtlasWideGlyphKey(glyphID, 0u, FontFace::Regular));
        ++shapedKeys;
    }
    CHECK(shapedKeys == 65536);

    // The sized tile is the kind a partition test written from the header's
    // comment block misses: its tag is not in that list, it hashes a parameter
    // set into 29 bits rather than laying out fields, and it shares the mono
    // atlas. A structured key under a hash's tag collides at whichever inputs
    // happen to fold there, and the sized probe verifies its contents while a
    // glyph-id probe has nothing to verify — so the collision would serve one
    // glyph's ink under another's key with every count correct. Swept over the
    // parameter space the wire can express rather than sampled, because one
    // collision anywhere in it is the defect.
    {
        int sizedKeys = 0;
        for (std::uint32_t codePoint : { 0x41u, 0x4E00u, 0x1F600u, 0x10FFFFu }) {
            for (std::uint32_t scale = 2; scale <= 7; ++scale) {
                for (std::uint32_t band = 0; band < scale; ++band) {
                    for (std::uint32_t tile = 0; tile < 8; ++tile) {
                        for (FontFace face : sweepFaces) {
                            const std::uint32_t sized = sizedTileKey(codePoint, 3u, face, 2u, scale, band, tile);
                            CHECK_MSG(!isShapedGlyphKey(sized), "a sized tile reads as a shaped glyph");
                            ++sizedKeys;
                        }
                    }
                }
            }
        }
        CHECK(sizedKeys == 3456); // 4 code points x 27 (scale, band) pairs x 8 tiles x 4 faces
    }

    // Injective in the glyph id, which is what lets the atlas hold one slot per
    // shaped glyph: a shift overlapping the style lanes would alias
    // neighbouring ids and serve one letterform where another was shaped — the
    // defect the strip key could only catch by memcmp behind the hash.
    CHECK(shapedGlyphKey(0x1234u, 0u, FontFace::Regular, 0u) != shapedGlyphKey(0x1235u, 0u, FontFace::Regular, 0u));
    CHECK(shapedGlyphKey(0xFFFFu, 0u, FontFace::Regular, 0u) != shapedGlyphKey(0u, 0u, FontFace::Regular, 0u));

    // Every styling axis moves the key, in the lanes monoGlyphKey uses.
    const std::uint32_t base = shapedGlyphKey(0x0301u, 0u, FontFace::Regular, 0u);
    CHECK(base != shapedGlyphKey(0x0301u, 0u, FontFace::Regular, 3u)); // phase
    CHECK(base != shapedGlyphKey(0x0301u, 0u, FontFace::Bold, 0u)); // bold
    CHECK(base != shapedGlyphKey(0x0301u, 0u, FontFace::Italic, 0u)); // italic
    CHECK(base != shapedGlyphKey(0x0301u, 5u, FontFace::Regular, 0u)); // variable weight

    // And the predicate answers no for the kinds that share a bit with the tag,
    // which is the half a round-trip test never reaches.
    CHECK(!isShapedGlyphKey(drcsGlyphKey(1u, 0u))); // bit 29 alone
    CHECK(!isShapedGlyphKey(coverageClusterKey(sweepClusters()[0].data(), 2, 0, FontFace::Regular))); // bit 31 alone
    CHECK(!isShapedGlyphKey(sharedAtlasWideGlyphKey(0x4E00u, 0u, FontFace::Regular))); // bits 30|29
    CHECK(!isShapedGlyphKey(monoGlyphKey(0x41u, 0u, FontFace::Regular, 0u)));
}

// The sized-tile folds hash a parameter set no direct fold can hold, so the
// property to pin is not a formula but a partition: every key lands under its
// tag, inside its atlas's free region, and never on the empty sentinel. The
// sweep covers every scale, band and window the wire can express, because the
// masking that guarantees the tag is exactly the code a refactor would touch.
TEST(everySizedTileKeyStaysInItsPartition)
{
    int sizedKeys = 0, sizedColorKeys = 0;
    const std::uint32_t sweepCodePoints[] = { 0x41u, 0x4E00u, 0x1F600u, 0x10FFFFu };
    for (std::uint32_t codePoint : sweepCodePoints) {
        for (FontFace face : sweepFaces) {
            for (std::uint32_t scale = 2; scale <= 7; ++scale) {
                for (std::uint32_t band = 0; band < scale; ++band) {
                    for (std::uint32_t tile = 0; tile < scale; ++tile) {
                        const std::uint32_t key = sizedTileKey(codePoint, scale % 8, face, tile % 8, scale, band, tile);
                        // Bits 31|29 set, bit 30 clear: above coverage's
                        // ceiling (its 23-bit hash << 5 keeps bit 29 down),
                        // apart from DRCS (bit 29 alone), box (bit 30) and the
                        // shared-atlas wide tag (30|29), and short of the
                        // 0xFFFFFFFF sentinel by the clear bit 30.
                        CHECK((key & 0xE0000000u) == 0xA0000000u);
                        CHECK(key != emptyGlyphKey);
                        ++sizedKeys;
                    }
                }
            }
        }
    }
    for (const auto& points : sweepClusters()) {
        for (std::uint32_t scale = 2; scale <= 7; ++scale) {
            for (std::uint32_t band = 0; band < scale; ++band) {
                for (std::uint32_t tile = 0; tile < scale; ++tile) {
                    const std::uint32_t word = sizedTileWord(0, FontFace::Regular, 0, scale, band, tile);
                    const std::uint32_t key = sizedColorTileKey(points.data(), static_cast<int>(points.size()), word);
                    // Bit 30 alone: above every raw code point (<= 0x10FFFF),
                    // below every cluster hash (bit 31), and short of the
                    // sentinel by the clear bit 31.
                    CHECK((key & 0xC0000000u) == 0x40000000u);
                    CHECK(key != emptyGlyphKey);
                    ++sizedColorKeys;
                }
            }
        }
    }
    // The sweep is the thing that can silently become nothing.
    CHECK(sizedKeys == 4 * 4 * 139 && sizedColorKeys == 5 * 139);

    // Two windows differing only in their band or tile must fold apart, or
    // the memcmp verification re-rasterises one of them every frame — correct
    // pixels, quadratic fills. Not guaranteed by an FNV fold in principle,
    // which is why it is pinned for the values in actual use.
    CHECK(sizedTileKey(0x45u, 0, FontFace::Regular, 0, 3, 0, 0)
        != sizedTileKey(0x45u, 0, FontFace::Regular, 0, 3, 0, 1));
    CHECK(sizedTileKey(0x45u, 0, FontFace::Regular, 0, 3, 0, 0)
        != sizedTileKey(0x45u, 0, FontFace::Regular, 0, 3, 1, 0));
}

// An OSC 66 fraction is a raster at another size, so it is another slot: the
// window of `A` at 3 * 1/2 is not the window at 3, nor at 3 * 2/3, and the
// whole-scale lane is the one the doubled lane already spelled. The verifying
// word must separate them too, or a probe accepts the wrong size's pixels.
TEST(aFractionalSizedTileKeysApartFromEveryOtherSize)
{
    const std::uint32_t whole = sizedTileScaleLane(3, 1, 0, 0, SizedWindowStep::FaceAdvance);
    const std::uint32_t half = sizedTileScaleLane(3, 1, 1, 2, SizedWindowStep::FaceAdvance);
    const std::uint32_t twoThirds = sizedTileScaleLane(3, 1, 2, 3, SizedWindowStep::FaceAdvance);
    CHECK_EQ(whole, 3u);
    CHECK_EQ(sizedTileScaleLane(3, 2, 0, 0, SizedWindowStep::FaceAdvance), 11u);
    std::vector<std::uint32_t> words;
    for (std::uint32_t lane : { whole, half, twoThirds, sizedTileScaleLane(3, 1, 1, 3, SizedWindowStep::FaceAdvance), sizedTileScaleLane(2, 1, 1, 2, SizedWindowStep::FaceAdvance) }) {
        for (std::uint32_t band = 0; band < 3; ++band) {
            for (std::uint32_t tile = 0; tile < 3; ++tile)
                words.push_back(sizedTileWord(0, FontFace::Regular, 0, lane, band, tile));
        }
    }
    std::vector<std::uint32_t> sorted = words;
    std::ranges::sort(sorted);
    CHECK_MSG(std::ranges::adjacent_find(sorted) == sorted.end(), "every size, band and window packs to its own word");
    CHECK(sizedTileKey(0x41u, 0, FontFace::Regular, 0, whole, 0, 0) != sizedTileKey(0x41u, 0, FontFace::Regular, 0, half, 0, 0));
    for (std::uint32_t lane : { half, twoThirds }) {
        const std::uint32_t key = sizedTileKey(0x41u, 7, FontFace::BoldItalic, 7, lane, 6, 13);
        CHECK((key & 0xE0000000u) == 0xA0000000u);
    }

    // The builder asks for a fraction in its lowest terms, and for a whole
    // product as that whole scale.
    const SizedTile reduced = fractionalSizedTile(3, 2, 4);
    CHECK(reduced.scale == 3 && reduced.fractionNumerator == 1 && reduced.fractionDenominator == 2);
    const SizedTile wholeProduct = fractionalSizedTile(4, 1, 2);
    CHECK(wholeProduct.scale == 2 && !wholeProduct.isFractional());
    CHECK(fractionalSizedTile(2, 1, 2).scale == 0);
    CHECK(std::fabs(fractionalSizedTile(1, 1, 2).rasterScale() - 0.5) < 1e-9);
}
