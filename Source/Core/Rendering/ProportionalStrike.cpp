// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/ProportionalStrike.h"

#include "Core/Codec/ZlibInflate.h"
#include "Core/Rendering/ProportionalStrikeData.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace Ubiquitty::Rendering::ProportionalStrike {

namespace {

struct PhaseRecord {
    std::uint32_t coverageAt; // offset into the inflated stream
    std::int8_t left;
    std::int8_t top;
    std::uint8_t width;
    std::uint8_t height;
};

struct GlyphRecord {
    std::uint32_t codePoint;
    std::uint32_t firstPhase; // index into the strike's phase records
    std::uint16_t advanceInUnits;
};

struct KernRecord {
    std::uint32_t pair; // first << 16 | second
    std::int16_t kern;
};

struct Strike {
    std::vector<GlyphRecord> glyphs; // sorted by code point
    std::vector<PhaseRecord> phases; // `phaseCount` per glyph, in glyph order
    std::vector<KernRecord> kerns; // sorted by pair
    int phaseCount = 0;
    int overhang = 0;
};

constexpr int strikeCount = 4; // two faces at two scales
bool isLoaded = false;
bool didLoadFail = false;
std::vector<std::uint8_t> blob;
Strike strikes[strikeCount];

int strikeIndex(Face face, int scale)
{
    if (scale != 1 && scale != 2)
        return -1;
    return static_cast<int>(face) * 2 + (scale - 1);
}

std::uint32_t readU16(std::size_t at) { return static_cast<std::uint32_t>(blob[at]) | (static_cast<std::uint32_t>(blob[at + 1]) << 8); }
std::uint32_t readU32(std::size_t at) { return readU16(at) | (readU16(at + 2) << 16); }

// Inflates the stream once and indexes every strike in it. The layout is
// Tools/gen-proportional-strike's; a stream that ends mid-record leaves the
// strikes read so far and marks the load failed, so a lookup answers absent
// rather than reading past the end.
void ensureLoaded()
{
    if (isLoaded || didLoadFail)
        return;
    blob.resize(proportionalStrikeBlobLength);
    if (!Ubiquitty::Codec::Zlib::inflateExact({ proportionalStrikeCompressed, static_cast<std::size_t>(proportionalStrikeCompressedLength) },
            { blob.data(), static_cast<std::size_t>(proportionalStrikeBlobLength) })) {
        didLoadFail = true;
        return;
    }
    std::size_t at = 0;
    const std::size_t total = blob.size();
    while (at + 12 <= total) {
        const int face = blob[at];
        const int scale = blob[at + 1];
        const int phases = blob[at + 2];
        const int overhang = blob[at + 3];
        const std::uint32_t glyphCount = readU32(at + 4);
        const std::uint32_t kernCount = readU32(at + 8);
        at += 12;
        const int index = face < 2 ? strikeIndex(static_cast<Face>(face), scale) : -1;
        Strike scratch;
        Strike& strike = index >= 0 ? strikes[index] : scratch;
        strike.phaseCount = phases;
        strike.overhang = overhang;
        strike.glyphs.reserve(glyphCount);
        strike.phases.reserve(static_cast<std::size_t>(glyphCount) * static_cast<std::size_t>(phases));
        for (std::uint32_t i = 0; i < glyphCount; ++i) {
            if (at + 4 > total) {
                didLoadFail = true;
                return;
            }
            GlyphRecord record;
            record.codePoint = readU16(at);
            record.advanceInUnits = static_cast<std::uint16_t>(readU16(at + 2));
            record.firstPhase = static_cast<std::uint32_t>(strike.phases.size());
            at += 4;
            for (int phase = 0; phase < phases; ++phase) {
                if (at + 4 > total) {
                    didLoadFail = true;
                    return;
                }
                PhaseRecord raster;
                raster.left = static_cast<std::int8_t>(blob[at]);
                raster.top = static_cast<std::int8_t>(blob[at + 1]);
                raster.width = blob[at + 2];
                raster.height = blob[at + 3];
                raster.coverageAt = static_cast<std::uint32_t>(at + 4);
                at += 4 + static_cast<std::size_t>(raster.width) * raster.height;
                if (at > total) {
                    didLoadFail = true;
                    return;
                }
                strike.phases.push_back(raster);
            }
            strike.glyphs.push_back(record);
        }
        strike.kerns.reserve(kernCount);
        for (std::uint32_t i = 0; i < kernCount; ++i) {
            if (at + 6 > total) {
                didLoadFail = true;
                return;
            }
            strike.kerns.push_back({ (readU16(at) << 16) | readU16(at + 2), static_cast<std::int16_t>(readU16(at + 4)) });
            at += 6;
        }
        // Glyphs are written in code point order and pairs in pair order; the
        // sort is a guard against a stream that was not, since the lookups
        // below bisect.
        std::vector<std::size_t> order(strike.glyphs.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return strike.glyphs[a].codePoint < strike.glyphs[b].codePoint; });
        std::vector<GlyphRecord> sorted;
        sorted.reserve(strike.glyphs.size());
        for (const std::size_t i : order)
            sorted.push_back(strike.glyphs[i]);
        strike.glyphs = std::move(sorted);
        std::sort(strike.kerns.begin(), strike.kerns.end(), [](const KernRecord& a, const KernRecord& b) { return a.pair < b.pair; });
    }
    isLoaded = true;
}

const Strike* strikeFor(Face face, int scale)
{
    ensureLoaded();
    const int index = strikeIndex(face, scale);
    if (index < 0 || !isLoaded)
        return nullptr;
    return &strikes[index];
}

} // namespace

int phaseCount(int scale)
{
    const Strike* strike = strikeFor(Face::Regular, scale);
    return strike ? strike->phaseCount : 0;
}

Glyph lookup(Face face, int scale, std::uint32_t codePoint, int phase)
{
    const Strike* strike = strikeFor(face, scale);
    if (!strike || phase < 0 || phase >= strike->phaseCount)
        return {};
    const auto it = std::lower_bound(strike->glyphs.begin(), strike->glyphs.end(), codePoint,
        [](const GlyphRecord& record, std::uint32_t wanted) { return record.codePoint < wanted; });
    if (it == strike->glyphs.end() || it->codePoint != codePoint)
        return {};
    const PhaseRecord& raster = strike->phases[static_cast<std::size_t>(it->firstPhase) + static_cast<std::size_t>(phase)];
    Glyph glyph;
    glyph.coverage = raster.width > 0 && raster.height > 0 ? blob.data() + raster.coverageAt : nullptr;
    glyph.width = raster.width;
    glyph.height = raster.height;
    glyph.left = raster.left;
    glyph.top = raster.top;
    glyph.advanceInUnits = it->advanceInUnits;
    glyph.present = true;
    return glyph;
}

int kern(Face face, int scale, std::uint32_t first, std::uint32_t second)
{
    const Strike* strike = strikeFor(face, scale);
    if (!strike || first > 0xFFFFu || second > 0xFFFFu)
        return 0;
    const std::uint32_t pair = (first << 16) | second;
    const auto it = std::lower_bound(strike->kerns.begin(), strike->kerns.end(), pair,
        [](const KernRecord& record, std::uint32_t wanted) { return record.pair < wanted; });
    return it != strike->kerns.end() && it->pair == pair ? it->kern : 0;
}

int overhangInPixels(int scale)
{
    const Strike* regular = strikeFor(Face::Regular, scale);
    const Strike* bold = strikeFor(Face::Bold, scale);
    return std::max(regular ? regular->overhang : 0, bold ? bold->overhang : 0);
}

} // namespace Ubiquitty::Rendering::ProportionalStrike
