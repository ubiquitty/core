// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>

// GlyphKey — portable, byte-exact glyph-atlas KEY derivations. A glyph's
// (codePoint, style, phase) or a grapheme cluster folds into a uint32 key that
// GlyphSlotCache maps to an atlas slot. Metal and Vulkan must derive IDENTICAL
// keys, or a process-wide shared atlas mis-resolves; pinned by
// Tests/Core/Rendering/test_glyph_key.cpp.
//
// Keyspace partition — one shared R8 atlas hash holds text + box + coverage-
// cluster keys, so they must never collide. The top two bits partition it:
//   bit 31 (0x80000000) — cluster keyspace (color cluster / coverage cluster)
//   bit 30 (0x40000000) — box/block-glyph-from-font keyspace
//   bits 29|30 (0x60000000) — wide glyph in a backend that shares one atlas
//   bit 29 (0x20000000) — DECDLD soft-font (DRCS) glyph keyspace
//   below               — plain text keyspace (cp <= 0x10FFFF is 21 bits, so
//                         cp<<8 stays under bit 29; all three top bits clear)
// The reserved empty-bucket sentinel is all-ones (GlyphSlotCache::emptyKey,
// 64-bit); the cluster folds dodge its low word so a tierless probe never
// forms it either.
//
// Pure integer arithmetic. No platform / Foundation dependency.

namespace Ubiquitty::Rendering {

// Reserved empty-bucket low word; GlyphSlotCache::emptyKey is this value
// widened to all 64 bits. Every derivation below stays disjoint from it (or
// explicitly dodges it).
inline constexpr std::uint32_t emptyGlyphKey = 0xFFFFFFFFu;

// Glyph styling face carried through the atlas seam: the two
// independent axes (SGR 1 bold, SGR 3 italic) packed as two bits.
// The bit values feed the key derivations below — never renumber.
enum class FontFace : std::uint8_t {
    Regular = 0,
    Bold = 1,
    Italic = 2,
    BoldItalic = 3,
};

constexpr FontFace fontFace(bool bold, bool italic) { return static_cast<FontFace>((bold ? 1u : 0u) | (italic ? 2u : 0u)); }

constexpr bool isBold(FontFace face) { return static_cast<std::uint8_t>(face) & 1u; }

constexpr bool isItalic(FontFace face) { return static_cast<std::uint8_t>(face) & 2u; }

/// Which part of a double-size master a doubled row draws.
///
/// A DECDHL row shows the top or bottom half of a glyph rasterised at twice
/// the cell height; DECDWL shows the whole master at twice the width. The
/// backend answers with the rect for the region asked for, because the inset
/// around a master inside its slot is per edge and is a fact about how that
/// backend packs its atlas — arithmetic the builder cannot do and should not
/// have to know.
enum class DoubleRegion : std::uint8_t {
    Full,
    Top,
    Bottom,
};

// Raster variant — everything besides the code point and face that changes
// the pixels a rasteriser produces for one glyph, folded into the uint32 the
// backend hooks already carry as `phase`:
//   bits 0-2   subpixel phase (SubpixelPhase.h; phaseCount <= 8)
//   bits 8-10  smoothing tier (0..4), see rasterTier below
// A backend that positions continuously and has no smoothing reads only the
// phase (variantPhase), and Core hands such a backend tier 0, so its variant
// equals its phase and its 64-bit key is the zero-extended 32-bit one.
//
// The tier is CoreText's own rule: its font smoothing dilates a glyph by an
// amount that depends on the FILL colour's Rec.709 linear luminance Y in five
// steps, tier = round(4 Y) with thresholds at 0.125 / 0.375 / 0.625 / 0.875 —
// not on the background, not on polarity. A mask rasterised at one tier and
// shown at another draws at the wrong weight, so the atlas keys the tier and
// rasterises each at its own fill.
inline constexpr std::uint32_t rasterTierCount = 5;

/// Folds a phase and a tier into one raster variant.
///
/// - Parameters:
///   - phase: The subpixel phase, masked to the key's 3 bits.
///   - tier: The smoothing tier, 0..4, masked to 3 bits.
/// - Returns: The variant word the backend hooks carry.
constexpr std::uint32_t makeRasterVariant(std::uint32_t phase, std::uint32_t tier)
{
    return (phase & 0x7u) | ((tier & 0x7u) << 8);
}

/// Returns the subpixel phase of a raster variant.
///
/// - Parameter variant: The variant word.
/// - Returns: The phase, 0..7.
constexpr std::uint32_t variantPhase(std::uint32_t variant) { return variant & 0x7u; }

/// Returns the smoothing tier of a raster variant.
///
/// - Parameter variant: The variant word.
/// - Returns: The tier, 0..4 for a CoreText sheet and 0 elsewhere.
constexpr std::uint32_t variantTier(std::uint32_t variant) { return (variant >> 8) & 0x7u; }

/// Returns the smoothing tier for a foreground luminance.
///
/// The thresholds are CoreText's, for gray, green and red primaries alike: the
/// selector is LINEAR luminance, and the sRGB-encoded value at each step
/// differs per primary.
///
/// - Parameter luma: The foreground's Rec.709 linear luminance in [0, 1].
/// - Returns: The tier, 0 (black) to 4 (white).
constexpr std::uint32_t rasterTierForLinearLuma(double luma)
{
    if (!(luma > 0.125))
        return 0;
    if (luma < 0.375)
        return 1;
    if (luma < 0.625)
        return 2;
    if (luma < 0.875)
        return 3;
    return 4;
}

/// Returns the slot key the cache is probed with.
///
/// The 32-bit derivation sits in the low word and the variant's tier above
/// it. The low word is the derivation the pinned tests name, so a widened
/// 32-bit key is the tier-0 / slot-0 key, and a backend that knows nothing of
/// tiers keys consistently by widening its 32-bit keys.
///
/// - Parameters:
///   - key: One of the 32-bit derivations in this header.
///   - variant: The raster variant; its phase is already inside `key`.
/// - Returns: The 64-bit key.
constexpr std::uint64_t glyphSlotKey(std::uint32_t key, std::uint32_t variant)
{
    return static_cast<std::uint64_t>(key)
        | (static_cast<std::uint64_t>(variantTier(variant)) << 32);
}

/// The resolution a strip's pen residual is keyed at: 1/65536 of a device pixel.
constexpr std::uint32_t penResidualCodes = 65536u;

/// Returns the code a strip's fractional pen keys under.
///
/// A rasteriser that quantises pens itself floors every glyph of a shaped line
/// against the line's true origin, so two strips of the same text whose runs
/// start at pens differing by less than a phase bucket still come out with
/// different glyphs moved, and a strip keyed on the bucket alone would draw
/// one of them a step from where the same word sits as single glyphs. The
/// residual is the fraction the run's origin pixel drops, at a resolution
/// finer than any pen the grid can produce.
///
/// - Parameter penResidualInPixels: The pen minus its floor, in [0, 1).
/// - Returns: The residual scaled to `penResidualCodes`, clamped to the range.
constexpr std::uint32_t penResidualCode(double penResidualInPixels)
{
    const double scaled = penResidualInPixels * static_cast<double>(penResidualCodes);
    if (!(scaled > 0.0))
        return 0u;
    if (scaled >= static_cast<double>(penResidualCodes - 1u))
        return penResidualCodes - 1u;
    return static_cast<std::uint32_t>(scaled);
}

/// Returns the slot key a ligature strip is probed with.
///
/// ``glyphSlotKey`` with the pen residual code above the tier, bits 40-55, so
/// the same run at two residuals occupies two slots rather than one the hash
/// chain has to verify apart.
///
/// - Parameters:
///   - stripKey: The ``ligatureStripKey`` derivation.
///   - variant: The raster variant whose tier the strip is drawn at.
///   - residualCode: The ``penResidualCode`` of the run's origin.
/// - Returns: The 64-bit key.
constexpr std::uint64_t ligatureStripSlotKey(std::uint32_t stripKey, std::uint32_t variant, std::uint32_t residualCode)
{
    return glyphSlotKey(stripKey, variant)
        | (static_cast<std::uint64_t>(residualCode & (penResidualCodes - 1u)) << 40);
}

// Mono text glyph: phase (bits 0-2), bold (3), italic (4), weight (5-7),
// codePoint (8+). `weight` is the renderer's font weight, 0..7. Unstyled
// phase-0 keys match across the subpixel / non-subpixel passes so unstyled
// text shares slots. The 3-bit phase field comes from Rendering/SubpixelPhase.h
// (phaseForPen), whose subpixelPhaseCount must stay <= 8 to fit here.
//
inline std::uint32_t monoGlyphKey(std::uint32_t codePoint, std::uint32_t weight, FontFace face, std::uint32_t phase) { return (codePoint << 8) | ((weight & 0x7u) << 5) | (isItalic(face) ? 0x10u : 0u) | (isBold(face) ? 0x08u : 0u) | (phase & 0x7u); }

/// A glyph the shaper chose, keyed by the face's own glyph id.
///
/// The healed letterform of `m` between two spaces is not the glyph `m` maps
/// to, and it is not a property of any code point: it is what the shaper
/// answered for that cell in its context. Keying it by the ANSWER rather than
/// by the question is what bounds the key space — a face's repertoire is
/// hundreds of glyphs actually used, where the text fragments that produce
/// them do not repeat at all. A cache keyed on the fragment reuses nothing,
/// evicts under pressure, and the cells it then refuses draw the unshaped
/// letterform beside the shaped ones — the flip a reader sees while scrolling.
///
/// Bits 31|30|29 tag the keyspace, which is the top of the range the shared R8
/// atlas leaves free. Every kind that atlas already holds sits below it: text
/// keys top out at bit 28, DRCS takes bit 29 alone, box bit 30, wide 30|29, the
/// coverage cluster bit 31 with a 23-bit payload under it, and ``sizedTileKey``
/// 31|29 with a 29-bit hash under that — which is why this cannot take 31|29
/// however free the bit pattern looks: a hash lands on structured values, and a
/// probe that does not verify would take the slot holding the other glyph's ink
/// with every count correct. ``ligatureStripKey`` writes overlapping values in
/// its OWN atlas, which the two never share; strips retiring is what makes that
/// range single-meaning again.
///
/// The style and phase lanes sit exactly where ``monoGlyphKey`` puts them,
/// because a reader who knows that layout is reading this one next. No hash: a
/// glyph id is already the identity, so unlike the strip key this one is
/// injective and needs no memcmp verification behind it.
///
/// - Parameters:
///   - glyphID: The face's glyph identifier, as the shaper reported it.
///   - weight: The renderer's variable weight, 0..7.
///   - face: The style the glyph was shaped in.
///   - phase: The subpixel phase, 0..7.
/// - Returns: The atlas key for that glyph.
inline std::uint32_t shapedGlyphKey(std::uint32_t glyphID, std::uint32_t weight, FontFace face, std::uint32_t phase)
{
    return 0xE0000000u | ((glyphID & 0xFFFFu) << 8) | ((weight & 0x7u) << 5) | (isItalic(face) ? 0x10u : 0u) | (isBold(face) ? 0x08u : 0u) | (phase & 0x7u);
}

/// Key for one glyph of a proportional run, by identity.
///
/// All 32 bits of the rasterizer's glyph identity survive: a browser may use
/// a Unicode scalar where a native font uses a 16-bit glyph index. Style and
/// raster variant occupy separate lanes. The owning atlas configuration
/// supplies font, variation, and scale identity.
///
/// - Parameters:
///   - glyphID: The proportional face's glyph identifier.
///   - weight: The variable weight lane, 0..7.
///   - face: The style the run was shaped in.
///   - phase: The subpixel phase the glyph's origin was drawn at, 0..7.
///   - tier: The smoothing tier, 0..4.
/// - Returns: The proportional-atlas key for that glyph.
inline std::uint64_t proportionalGlyphKey(std::uint32_t glyphID, std::uint32_t weight, FontFace face, std::uint32_t phase, std::uint32_t tier = 0)
{
    return (static_cast<std::uint64_t>(glyphID) << 8)
        | (static_cast<std::uint64_t>(tier & 0x7u) << 40)
        | ((weight & 0x7u) << 5) | (isItalic(face) ? 0x10u : 0u)
        | (isBold(face) ? 0x08u : 0u) | (phase & 0x7u);
}

/// Key for one window of a proportional glyph drawn larger than its slot: for
/// a run on a DECDWL row, on a DECDHL pair, or at an OSC 66 scale.
///
/// A larger glyph is another image of the same identity, and one larger than
/// a slot, so it is cut into slot-sized windows: bit 47 marks the sizing, bits
/// 43-46 name the window, bits 48-50 the rows of slots the raster spans, bits
/// 51-53 the row the window is cut from, and bit 54 a raster doubled across
/// past its scale — above the tier's lane and clear of every
/// ``proportionalGlyphKey``.
///
/// - Parameters:
///   - glyphID: The proportional face's glyph identifier.
///   - weight: The variable weight lane, 0..7.
///   - face: The style the run was shaped in.
///   - phase: The subpixel phase of the sized origin, 0..7.
///   - tier: The smoothing tier, 0..4.
///   - window: Which slot-wide cut of the sized glyph, 0..15.
///   - scale: How many times its size the glyph is drawn, 1..7.
///   - band: Which slot height of the raster the window is cut from, below `scale`.
///   - widthScale: The factor across past `scale`, 1 or 2.
/// - Returns: The proportional-atlas key for that window.
inline std::uint64_t sizedProportionalGlyphKey(std::uint32_t glyphID, std::uint32_t weight, FontFace face, std::uint32_t phase, std::uint32_t tier,
    std::uint32_t window, std::uint32_t scale, std::uint32_t band, std::uint32_t widthScale)
{
    return proportionalGlyphKey(glyphID, weight, face, phase, tier)
        | (static_cast<std::uint64_t>(window & 0xFu) << 43) | (std::uint64_t { 1 } << 47)
        | (static_cast<std::uint64_t>(scale & 0x7u) << 48) | (static_cast<std::uint64_t>(band & 0x7u) << 51)
        | (static_cast<std::uint64_t>(widthScale == 2 ? 1u : 0u) << 54);
}

/// Whether `key` came from ``shapedGlyphKey``.
///
/// - Parameter key: A key from any derivation here.
/// - Returns: Whether it names a shaped glyph.
inline bool isShapedGlyphKey(std::uint32_t key) { return (key & 0xE0000000u) == 0xE0000000u; }

// Box / block glyph rasterised from the font and edge-stretched. Bit 30
// tags the box keyspace; no phase. Variable weight (0..7) sits in bits 0-2
// (the slot monoGlyphKey gives the phase): the raster applies the renderer's
// variable weight via the weighted base face, so the key must carry it or a
// weight change would serve the stale stroke.
inline std::uint32_t boxFontKey(std::uint32_t codePoint, std::uint32_t weight, FontFace face) { return 0x40000000u | (codePoint << 5) | (isItalic(face) ? 0x10u : 0u) | (isBold(face) ? 0x08u : 0u) | (weight & 0x7u); }

// DECDLD soft-font glyph: position `index` (0..drcsMaxCells-1) of the soft font
// identified by `serial` (Core/VT/SoftFont.h — a fresh value per committed
// download). Bit 29 tags the DRCS keyspace, disjoint from all three above: text
// keys top out at bit 28 (0x10FFFF << 8), box sets bit 30, clusters bit 31.
//
// The serial keeps only the low 22 bits, so two distinct soft fonts alias after
// 4194304 downloads in one process, and the atlas is shared across panes — an
// alias is one pane's glyph served under another's key. A committed DECDLD is
// around 24 bytes, so roughly 100MB of terminal output walks the counter round
// and nothing rate-limits it. The fault path therefore verifies the glyph's own
// bitmap through `slotForKeyVerified`: this value narrows the search, and the
// contents decide.
inline std::uint32_t drcsGlyphKey(std::uint32_t serial, std::uint32_t index) { return 0x20000000u | ((serial & 0x3FFFFFu) << 7) | (index & 0x7Fu); }

// Wide (double-cell) glyph — lives in a separate wide atlas, so a different
// fold: bold in bit 0, italic in bit 1, variable weight (0..7) in bits 2-4,
// codePoint from bit 5 (0x10FFFF << 5 keeps bits 30-31 clear for backend
// tags). The raster applies the variable weight like the box path, so the
// key carries it for the same staleness reason.
inline std::uint32_t wideGlyphKey(std::uint32_t codePoint, std::uint32_t weight, FontFace face) { return (codePoint << 5) | ((weight & 0x7u) << 2) | (isItalic(face) ? 0x2u : 0u) | (isBold(face) ? 0x1u : 0u); }

/// Returns the wide-glyph key for a glyph rasterised at a subpixel phase.
///
/// The phase sits in bits 26-28, which ``wideGlyphKey`` leaves clear: its code
/// point ends at bit 25, and the shared-atlas tag is bits 29|30. Only a wide
/// page of its own takes this key, so no other keyspace meets it.
///
/// - Parameters:
///   - codePoint: The Unicode scalar the glyph draws.
///   - weight: Variable-font weight, 0-7, which the raster applies.
///   - face: The bold/italic pair the glyph is rendered in.
///   - phase: The subpixel phase the raster is drawn at, 0-7.
/// - Returns: The key; phase zero is ``wideGlyphKey``'s own.
inline std::uint32_t phasedWideGlyphKey(std::uint32_t codePoint, std::uint32_t weight, FontFace face, std::uint32_t phase)
{
    return wideGlyphKey(codePoint, weight, face) | ((phase & 0x7u) << 26);
}

/// Returns the wide-glyph key for a backend that shares one atlas across slot kinds.
///
/// Vulkan keeps mono, wide and cluster slots in a single atlas, so its wide
/// keys need a tag; Metal and the software renderer give wide glyphs an atlas
/// of their own and use `wideGlyphKey` untagged. An untagged wide key in a
/// shared atlas is a plain text key, which is why this cannot be optional.
///
/// The tag is bits 29|30 together. `wideGlyphKey` shifts the code point by 5
/// and so does `boxFontKey`, so tagging wide with bit 30 alone makes the two
/// keyspaces identical at regular weight for all 1114112 code points — and
/// nothing reports it, because the box and wide code point sets are disjoint by
/// Unicode width until a width table says otherwise, at which point one glyph's
/// pixels are served for another with every count and every gate still passing.
///
/// - Parameters:
///   - codePoint: The Unicode scalar the glyph draws.
///   - weight: Variable-font weight, 0-7, which the raster applies.
///   - face: The bold/italic pair the glyph is rendered in.
/// - Returns: A key disjoint from every other space in this header.
inline std::uint32_t sharedAtlasWideGlyphKey(std::uint32_t codePoint, std::uint32_t weight, FontFace face) { return 0x60000000u | wideGlyphKey(codePoint, weight, face); }

/// Whether `key` came from `sharedAtlasWideGlyphKey`.
///
/// An EXACT match on both tag bits, not a test that either is set. A backend
/// sharing one atlas reads this to size the slot it rasterises into, and "any
/// of these bits" answers yes for every box key and every cluster key as well
/// — so the predicate has to live beside the tag it tests, or the two drift the
/// first time a keyspace is added.
///
/// - Parameter key: A key from any of the spaces above.
/// - Returns: `true` only for a shared-atlas wide key.
inline bool isSharedAtlasWideGlyphKey(std::uint32_t key) { return (key & 0xE0000000u) == 0x60000000u; }

// Single color-emoji glyph in the RGBA color atlas — keyed by the raw
// codePoint (cp <= 0x10FFFF keeps bit 31 clear, disjoint from cluster
// keys).
inline std::uint32_t colorGlyphKey(std::uint32_t codePoint) { return codePoint; }

// FNV-1a over a codePoint run, each uint32 hashed LITTLE-ENDIAN (4 bytes
// low-to-high). Order-sensitive across the cluster. Offset basis
// 2166136261, prime 16777619. Shared by the two cluster keys below.
inline std::uint32_t fnv1aCluster(const std::uint32_t* codePoints, int n)
{
    std::uint32_t h = 2166136261u;
    for (int i = 0; i < n; ++i) {
        std::uint32_t v = codePoints[i];
        for (int b = 0; b < 4; ++b) {
            h ^= (v & 0xFFu);
            h *= 16777619u;
            v >>= 8;
        }
    }
    return h;
}

// Color-cluster (multi-codePoint emoji, e.g. ZWJ sequences) key in the RGBA
// atlas. n <= 1 returns the raw codePoint; otherwise FNV-1a | bit 31,
// dodging the empty sentinel.
inline std::uint32_t colorClusterKey(const std::uint32_t* codePoints, int n)
{
    if (n <= 1)
        return codePoints[0];
    std::uint32_t h = fnv1aCluster(codePoints, n);
    h |= 0x80000000u;
    if (h == emptyGlyphKey)
        h = 0x80000000u;
    return h;
}

// Coverage-cluster (combining-mark stack) key in the mono atlas. A 23-bit
// FNV-1a hash, then bit 31 | h<<5 | italic | bold | phase (the phase/bold/
// italic placement matches monoGlyphKey so styled clusters key cleanly),
// dodging the empty sentinel.
inline std::uint32_t coverageClusterKey(const std::uint32_t* codePoints, int n, int phase, FontFace face)
{
    std::uint32_t h = fnv1aCluster(codePoints, n) & 0x7FFFFFu; // 23-bit, below the flag bit
    std::uint32_t key = 0x80000000u | (h << 5) | (isItalic(face) ? 0x10u : 0u) | (isBold(face) ? 0x08u : 0u) | (static_cast<std::uint32_t>(phase) & 0x7u);
    if (key == emptyGlyphKey)
        key ^= 0x20u;
    return key;
}

// Double-size (DWL / DHL) master-glyph key: codePoint<<5 | weight<<2 |
// italic<<1 | bold. A DECDHL top row and its bottom row are independent grid
// rows carrying the same codepoints; they share one 2x master purely by
// deriving the same key — the row's line attribute picks which half to crop.
// No phase bits (double masters have no subpixel phases), but the raster
// runs through the shared coverage core, which applies the renderer's
// variable weight — so the key carries it like wideGlyphKey/boxFontKey.
inline std::uint32_t doubleMasterKey(std::uint32_t codePoint, std::uint32_t weight, FontFace face) { return (codePoint << 5) | ((weight & 0x7u) << 2) | (isItalic(face) ? 2u : 0u) | (isBold(face) ? 1u : 0u); }

/// Makes the key of a DECDWL master drawn from a face widened on its `wdth` axis.
///
/// A DECDWL row takes letterforms the DECDHL pair does not, so its master keys
/// apart from the uniform one the pair shares: bit 30 over ``doubleMasterKey``,
/// which a code point shifted by 5 never reaches (U+10FFFF lands at bit 25).
///
/// - Parameters:
///   - codePoint: The character the master draws.
///   - weight: Variable-font weight, 0-7.
///   - face: The bold/italic pair.
/// - Returns: A key distinct from every ``doubleMasterKey``.
inline std::uint32_t doubleWidthMasterKey(std::uint32_t codePoint, std::uint32_t weight, FontFace face) { return doubleMasterKey(codePoint, weight, face) | 0x40000000u; }

// Ligature STRIP key: a whole shaped run (e.g. "=>", "!=", "<=>") rasterised as
// one wide multi-cell coverage strip in its OWN ligature atlas. Carries the
// subpixel PHASE (same 3-bit axis as monoGlyphKey) so a run at each pen phase
// keys its own crisp strip. FNV-1a over the run (order- and length-sensitive)
// masked to 26 bits, then the bit31|bit30 tag (the one keyspace partition no
// other derivation occupies) | hash<<4 | phase<<1 | bold. The hash is not
// injective over variable-length runs, so the strip cache must memcmp-verify the
// run on a hash hit (a collision keeps probing). Bold is the only style axis the
// ligature cache tracks (no italic).
inline std::uint32_t ligatureStripKey(const std::uint32_t* codePoints, int n, FontFace face, std::uint32_t phase)
{
    std::uint32_t h = fnv1aCluster(codePoints, n) & 0x03FFFFFFu; // 26-bit, below the tag + phase + bold fields
    std::uint32_t key = 0xC0000000u | (h << 4) | ((phase & 0x7u) << 1) | (isBold(face) ? 1u : 0u);
    if (key == emptyGlyphKey)
        key ^= 0x10u; // flip the lowest hash bit (memcmp-verify absorbs the tiny collision), never a phase/bold bit
    return key;
}

/// How far a sized window steps across its block, which decides what the
/// window holds.
///
/// A glyph-stream quad stands at the face's own fractional advance from the one
/// before it, so its window is cut at that advance. A cell-baked quad is a grid
/// cell, mapped texel for texel, and stands a whole cell box from its neighbour,
/// so its window is cut at the box: cut at the advance and drawn on the cell it
/// would be stretched by the difference, and the block would end wider than the
/// glyph-stream's. The two windows of one glyph differ, so they key apart.
enum class SizedWindowStep : std::uint8_t {
    /// The face's reference advance, where the backend reports one.
    FaceAdvance,
    /// The slot's cell box.
    CellBox,
};

/// Packs a sized tile's parameters into the word its key folds and verifies.
///
/// One packing shared by the key and the verification buffer, so the two cannot
/// disagree about a field's position. The style/phase lanes sit where
/// monoGlyphKey puts them, for the reader who knows that layout already.
///
/// - Parameters:
///   - weight: Variable-font weight, 0-7.
///   - face: The bold/italic pair the window is rendered in.
///   - phase: Subpixel phase the anchor was positioned at, 0-7.
///   - scale: The block's row count, 2-7.
///   - band: Which row of the block, 0..scale-1.
///   - tile: Which window along the band.
/// - Returns: The packed word, second element of the verification buffer.
inline std::uint32_t sizedTileWord(std::uint32_t weight, FontFace face, std::uint32_t phase, std::uint32_t scale, std::uint32_t band, std::uint32_t tile)
{
    // The scale lane's low nibble sits at 24, its fraction numerator at 28 and
    // denominator at 20, beside a band that never needs more than four bits. The
    // lane's step bit rides above the window index, which a band of at most
    // `maxSizedWindowsPerBand` windows holds in seven bits.
    return ((scale & 0xFu) << 24) | (((scale >> 4) & 0xFu) << 28) | (((scale >> 8) & 0xFu) << 20) | (((scale >> 12) & 0x1u) << 15)
        | ((band & 0xFu) << 16) | ((tile & 0x7Fu) << 8)
        | ((weight & 0x7u) << 5) | (isItalic(face) ? 0x10u : 0u) | (isBold(face) ? 0x08u : 0u) | (phase & 0x7u);
}

/// Returns the scale lane a sized tile is keyed and verified under.
///
/// The row's horizontal doubling rides above the scale, which takes three
/// bits at most: a doubled window is another image of the same glyph, band
/// and tile, and keyed under the scale alone it would be served for the plain
/// one. An OSC 66 fraction rides above both, for the same reason: a window of
/// the raster at `scale * n / d` is another image again. The step the window is
/// cut at rides above the fraction, since a window cut at the cell box is
/// another picture of the same tile.
///
/// - Parameters:
///   - scale: The block's row count, 2-7, or 1-7 under a fraction.
///   - widthScale: The horizontal factor over the scale, 1 or 2.
///   - fractionNumerator: The raster's fraction of `scale`, numerator, or zero.
///   - fractionDenominator: The raster's fraction of `scale`, denominator, or zero.
///   - step: How far the window steps across its block.
/// - Returns: The value to pass as the scale to ``sizedTileKey`` and ``sizedTileWord``.
constexpr std::uint32_t sizedTileScaleLane(std::uint32_t scale, std::uint32_t widthScale,
    std::uint32_t fractionNumerator, std::uint32_t fractionDenominator, SizedWindowStep step)
{
    return (scale & 0x7u) | (widthScale == 2 ? 0x8u : 0u) | ((fractionNumerator & 0xFu) << 4) | ((fractionDenominator & 0xFu) << 8)
        | (step == SizedWindowStep::CellBox ? 0x1000u : 0u);
}

/// Sized-tile key, for the mono and wide atlases: one slot-shaped window of a
/// glyph rasterised at an OSC 66 scale (SizedTile in GlyphBackendTypes.h).
///
/// The parameters do not fit a direct fold — codePoint alone fills the plain
/// text keyspace — so this hashes (codePoint, sizedTileWord) and tags the
/// result, and the cache probe must memcmp-verify those same two words like
/// every other folded key here. Tag 0xA0000000 (bits 31|29): in the MONO atlas
/// the bit-31 space is otherwise coverageClusterKey's, whose 23-bit hash << 5
/// keeps bits 28-30 clear, so its keys top out at 0x8FFFFFFF and bit 29 never
/// rises; in the WIDE atlas bit 31 is also only coverage clusters. The payload
/// keeps bit 30 clear, so the key can never equal the 0xFFFFFFFF sentinel.
///
/// Vulkan shares ONE atlas across slot kinds and derives none of these: the
/// sized entries are optional, Vulkan leaves them null, and a sized key never
/// enters the shared atlas. A backend that adopts them there must first check
/// this tag against its shared-atlas partition, which is what this comment is
/// standing here to make it look up.
///
/// - Parameters:
///   - codePoint: The Unicode scalar the sized glyph draws.
///   - weight: Variable-font weight, 0-7.
///   - face: The bold/italic pair the window is rendered in.
///   - phase: Subpixel phase the anchor was positioned at, 0-7.
///   - scale: The block's row count, 2-7.
///   - band: Which row of the block, 0..scale-1.
///   - tile: Which window along the band.
/// - Returns: A key under the 0xA0000000 tag, never the sentinel.
inline std::uint32_t sizedTileKey(std::uint32_t codePoint, std::uint32_t weight, FontFace face, std::uint32_t phase, std::uint32_t scale, std::uint32_t band, std::uint32_t tile)
{
    const std::array<std::uint32_t, 2> words = { codePoint, sizedTileWord(weight, face, phase, scale, band, tile) };
    return 0xA0000000u | (fnv1aCluster(words.data(), 2) & 0x1FFFFFFFu);
}

/// Sized-tile key for the COLOUR atlas. A different tag because it lives in a
/// different partition: colour keys are raw code points (<= 0x10FFFF, bit 30
/// clear) and cluster hashes (bit 31 set), so bit 30 alone is free there and a
/// 30-bit payload cannot reach the sentinel. Folds the whole cluster plus the
/// tile word, and the probe verifies that same sequence — the caller appends
/// sizedTileWord(0, Regular, 0, scale, band, tile) to the cluster it probes
/// with, so a ZWJ family's tile and the family itself never collide silently.
///
/// - Parameters:
///   - codePoints: The cluster, base first.
///   - n: How many code points the cluster holds.
///   - tileWord: The window's `sizedTileWord`, appended to the verification.
/// - Returns: A key under the bit-30 tag, never the sentinel.
inline std::uint32_t sizedColorTileKey(const std::uint32_t* codePoints, int n, std::uint32_t tileWord)
{
    std::uint32_t h = fnv1aCluster(codePoints, n);
    h ^= fnv1aCluster(&tileWord, 1);
    return 0x40000000u | (h & 0x3FFFFFFFu);
}

} // namespace Ubiquitty::Rendering
