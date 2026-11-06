// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// UAX #15 normalization: decomposition, canonical ordering, and (for composed
// forms) canonical composition. Hangul is algorithmic (UAX #15 §18); other
// mappings come from Normalize.tables.inc. See Normalize.h.

#include "Core/Unicode/Normalize.h"
#include "Core/Unicode/RangeTable.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <cstddef>

namespace {

struct CombiningClassRange {
    std::uint32_t first;
    std::uint32_t last;
    std::uint8_t ccc;
};

struct DecompMapping {
    std::uint32_t codePoint;
    std::uint32_t offset; // index into decompData
    std::uint16_t length; // scalars at offset
    std::uint8_t compat; // 1 = compatibility-only mapping, 0 = canonical
};

struct CompositionPair {
    std::uint32_t a; // starter
    std::uint32_t b; // combining
    std::uint32_t result;
};

#include "Core/Unicode/Normalize.tables.inc"

constexpr int combiningClassCount = static_cast<int>(sizeof(combiningClassRanges) / sizeof(combiningClassRanges[0]));
constexpr int decompMappingCount = static_cast<int>(sizeof(decompMappings) / sizeof(decompMappings[0]));
constexpr int compositionPairCount = static_cast<int>(sizeof(compositionPairs) / sizeof(compositionPairs[0]));

// Hangul algorithmic decomposition/composition constants (UAX #15 §18).
constexpr std::uint32_t hangulSBase = 0xAC00;
constexpr std::uint32_t hangulLBase = 0x1100;
constexpr std::uint32_t hangulVBase = 0x1161;
constexpr std::uint32_t hangulTBase = 0x11A7;
constexpr std::uint32_t hangulLCount = 19;
constexpr std::uint32_t hangulVCount = 21;
constexpr std::uint32_t hangulTCount = 28;
constexpr std::uint32_t hangulNCount = hangulVCount * hangulTCount; // 588
constexpr std::uint32_t hangulSCount = hangulLCount * hangulNCount; // 11172

std::uint8_t combiningClassOf(std::uint32_t codePoint)
{
    const CombiningClassRange* r = Ubiquitty::Unicode::findRange(combiningClassRanges, combiningClassCount, codePoint);
    return r ? r->ccc : std::uint8_t(0);
}

bool tokenEq(std::string_view name, const char* canon)
{
    std::size_t j = 0;
    for (char raw : name) {
        char ch = raw;
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
        if (ch == '-' || ch == '_' || ch == ' ')
            continue; // separator-insensitive
        if (canon[j] == '\0' || canon[j] != ch)
            return false;
        ++j;
    }
    return canon[j] == '\0';
}

// Recursively append the full decomposition of one code point. A canonical
// mapping is always followed; a compatibility mapping only when `compatibility`
// is set. Hangul is computed inline.
void decomposeInto(std::uint32_t codePoint, bool compatibility, std::vector<std::uint32_t>& out)
{
    if (codePoint >= hangulSBase && codePoint < hangulSBase + hangulSCount) {
        std::uint32_t sIndex = codePoint - hangulSBase;
        std::uint32_t l = hangulLBase + sIndex / hangulNCount;
        std::uint32_t v = hangulVBase + (sIndex % hangulNCount) / hangulTCount;
        std::uint32_t t = hangulTBase + sIndex % hangulTCount;
        out.push_back(l);
        out.push_back(v);
        if (t != hangulTBase)
            out.push_back(t);
        return;
    }
    const DecompMapping* m = Ubiquitty::Unicode::findByKey(decompMappings, decompMappingCount, &DecompMapping::codePoint, codePoint);
    // No mapping, or a compat-only mapping under canonical decomposition:
    // codePoint is decomposition-stable.
    if (!m || (m->compat && !compatibility)) {
        out.push_back(codePoint);
        return;
    }
    for (std::uint16_t i = 0; i < m->length; ++i)
        decomposeInto(decompData[m->offset + i], compatibility, out);
}

// Stable-sort the combining marks into canonical order: within each run of
// non-zero combining classes, order by class ascending; class-0 starters never
// move and break runs.
void canonicalOrder(std::vector<std::uint32_t>& cps)
{
    const std::size_t n = cps.size();
    for (std::size_t i = 1; i < n; ++i) {
        std::uint8_t ci = combiningClassOf(cps[i]);
        if (ci == 0)
            continue;
        std::uint32_t value = cps[i];
        std::size_t j = i;
        while (j > 0) {
            std::uint8_t cj = combiningClassOf(cps[j - 1]);
            if (cj == 0 || cj <= ci)
                break;
            cps[j] = cps[j - 1];
            --j;
        }
        cps[j] = value;
    }
}

// Look up the primary composition of starter `a` with combining `b`, or 0.
std::uint32_t composePair(std::uint32_t a, std::uint32_t b)
{
    // Hangul: L + V and LV + T compose algorithmically.
    if (a >= hangulLBase && a < hangulLBase + hangulLCount && b >= hangulVBase && b < hangulVBase + hangulVCount) {
        std::uint32_t lIndex = a - hangulLBase;
        std::uint32_t vIndex = b - hangulVBase;
        return hangulSBase + (lIndex * hangulVCount + vIndex) * hangulTCount;
    }
    if (a >= hangulSBase && a < hangulSBase + hangulSCount && (a - hangulSBase) % hangulTCount == 0 && b > hangulTBase && b < hangulTBase + hangulTCount) {
        return a + (b - hangulTBase);
    }
    // Table lookup, sorted by (a, b).
    int low = 0;
    int high = compositionPairCount - 1;
    while (low <= high) {
        int middle = low + (high - low) / 2;
        const CompositionPair& p = compositionPairs[middle];
        if (a < p.a || (a == p.a && b < p.b))
            high = middle - 1;
        else if (a > p.a || (a == p.a && b > p.b))
            low = middle + 1;
        else
            return p.result;
    }
    return 0;
}

// Canonical composition (UAX #15 §1.3 R1): fold marks back onto the last starter
// when a primary composition exists and no blocking mark of equal-or-higher
// combining class intervenes.
void compose(std::vector<std::uint32_t>& cps)
{
    if (cps.empty())
        return;
    std::size_t starterPosition = 0; // index of the last starter in the output
    std::uint32_t starter = cps[0];
    std::uint8_t lastClass = 0; // combining class of the last appended mark
    bool haveStarter = combiningClassOf(cps[0]) == 0;
    std::size_t writePosition = 1;
    for (std::size_t i = 1; i < cps.size(); ++i) {
        std::uint32_t c = cps[i];
        std::uint8_t cClass = combiningClassOf(c);
        std::uint32_t composed = haveStarter ? composePair(starter, c) : 0;
        // Block composition if a same-or-higher class mark already sits between
        // the starter and c (lastClass >= cClass with lastClass != 0).
        if (composed != 0 && !(lastClass != 0 && lastClass >= cClass)) {
            cps[starterPosition] = composed;
            starter = composed;
            continue; // c is absorbed; do not advance writePosition
        }
        if (cClass == 0) {
            starterPosition = writePosition;
            starter = c;
            haveStarter = true;
            lastClass = 0;
        } else {
            lastClass = cClass;
        }
        cps[writePosition++] = c;
    }
    cps.resize(writePosition);
}

} // namespace

namespace Ubiquitty::Unicode {

std::optional<NormalizationForm> normalizationFormFromName(std::string_view name)
{
    if (tokenEq(name, "none") || tokenEq(name, "off"))
        return NormalizationForm::None;
    if (tokenEq(name, "nfc"))
        return NormalizationForm::NFC;
    if (tokenEq(name, "nfd"))
        return NormalizationForm::NFD;
    if (tokenEq(name, "nfkc"))
        return NormalizationForm::NFKC;
    if (tokenEq(name, "nfkd"))
        return NormalizationForm::NFKD;
    return std::nullopt;
}

const char* normalizationFormToName(NormalizationForm form)
{
    switch (form) {
    case NormalizationForm::NFC:
        return "nfc";
    case NormalizationForm::NFD:
        return "nfd";
    case NormalizationForm::NFKC:
        return "nfkc";
    case NormalizationForm::NFKD:
        return "nfkd";
    case NormalizationForm::None:
        break;
    }
    return "none";
}

void normalizeCodePoints(std::vector<std::uint32_t>& codePoints, NormalizationForm form)
{
    if (form == NormalizationForm::None || codePoints.empty())
        return;

    // ASCII is a fixed point of all four forms: no code point below 0x80 has a
    // canonical or compatibility decomposition, none carries a nonzero
    // combining class, and none takes part in a composition. So a run that is
    // entirely ASCII is already normalized, and the work below — a vector, a
    // binary search per code point, and an ordering pass — would return it
    // unchanged. The hot caller runs this once per cell over a whole
    // scrollback, where cells are overwhelmingly one ASCII code point.
    bool isAllASCII = true;
    for (const std::uint32_t codePoint : codePoints) {
        if (codePoint >= 0x80) {
            isAllASCII = false;
            break;
        }
    }
    if (isAllASCII)
        return;

    const bool compatibility = (form == NormalizationForm::NFKC || form == NormalizationForm::NFKD);
    const bool composing = (form == NormalizationForm::NFC || form == NormalizationForm::NFKC);

    std::vector<std::uint32_t> decomposed;
    decomposed.reserve(codePoints.size() * 2);
    for (std::uint32_t codePoint : codePoints)
        decomposeInto(codePoint, compatibility, decomposed);

    canonicalOrder(decomposed);

    if (composing)
        compose(decomposed);

    codePoints.swap(decomposed);
}

std::string normalizeUTF8(std::string_view input, NormalizationForm form)
{
    if (form == NormalizationForm::None || input.empty())
        return std::string(input);

    // An undecodable byte is carried as (rawByteSentinel | byte) — above the
    // valid Unicode range, so it passes through normalization untouched and
    // re-emits as the original raw byte (byte-for-byte lossless on invalid UTF-8).
    constexpr std::uint32_t rawByteSentinel = 0x80000000u;
    std::vector<std::uint32_t> codePoints;
    codePoints.reserve(input.size());
    const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
    const std::size_t n = input.size();
    std::size_t i = 0;
    while (i < n) {
        unsigned char lead = bytes[i];
        std::uint32_t codePoint;
        int extra;
        std::uint32_t minimum;
        if (lead < 0x80) {
            codePoints.push_back(lead);
            ++i;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            codePoint = lead & 0x1F;
            extra = 1;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            codePoint = lead & 0x0F;
            extra = 2;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            codePoint = lead & 0x07;
            extra = 3;
            minimum = 0x10000;
        } else {
            // Stray continuation or 5/6-byte lead: pass the raw byte through.
            codePoints.push_back(rawByteSentinel | lead);
            ++i;
            continue;
        }
        if (i + static_cast<std::size_t>(extra) >= n) {
            codePoints.push_back(rawByteSentinel | lead);
            ++i;
            continue;
        }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cont = bytes[i + static_cast<std::size_t>(k)];
            if ((cont & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            codePoint = (codePoint << 6) | (cont & 0x3F);
        }
        if (!ok || codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            codePoints.push_back(rawByteSentinel | lead);
            ++i;
            continue;
        }
        codePoints.push_back(codePoint);
        i += static_cast<std::size_t>(extra) + 1;
    }

    normalizeCodePoints(codePoints, form);

    std::string out;
    out.reserve(input.size());
    for (std::uint32_t codePoint : codePoints) {
        if (codePoint & rawByteSentinel)
            out.push_back(static_cast<char>(codePoint & 0xFF));
        else
            appendUTF8(out, codePoint);
    }
    return out;
}

} // namespace Ubiquitty::Unicode
