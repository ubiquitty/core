// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Unicode/Encodings.h"

#include "Core/Unicode/RangeTable.h"

#include <cstddef>

namespace Ubiquitty::Unicode {

#include "Core/Unicode/Encodings.tables.inc"

namespace {

// Case- and separator-insensitive name compare against a lowercase,
// separator-free literal `canon`. Both sides drop '-', '_', '.', ' '.
bool nameEq(std::string_view name, std::string_view canon)
{
    // Separators are noise in an encoding name — "ISO-8859-1", "iso8859_1"
    // and "iso 8859 1" name one encoding — so they are dropped rather than
    // compared, which is why this is not a case-folded equality.
    std::size_t matched = 0;
    for (const char raw : name) {
        if (raw == '-' || raw == '_' || raw == '.' || raw == ' ')
            continue;
        const char folded = (raw >= 'A' && raw <= 'Z') ? static_cast<char>(raw - 'A' + 'a') : raw;
        if (matched == canon.size() || canon[matched] != folded)
            return false;
        ++matched;
    }
    return matched == canon.size();
}

// Look up a packed (lead<<8|trail) key in a lead-indexed map. Returns the
// code point, or 0 when the pair maps to nothing.
//
// Two array reads and no branches, which is what the single-byte sets have
// always done. The sorted array this replaced was the same two-level structure
// written flat — keys are contiguous per lead byte — so a binary search over
// it spent about fifteen data-dependent probes rediscovering the lead byte it
// was handed, once per CJK character on the whole ingest path.
template<std::size_t RowCount>
[[nodiscard]] static inline std::uint32_t dbcsLookup(const std::uint8_t (&leadRow)[256],
    const std::uint16_t (&rows)[RowCount][256], std::uint16_t key)
{
    return rows[leadRow[key >> 8]][key & 0xFF];
}

// GB18030 4-byte BMP plane: map a linear index to a codePoint via the
// coalesced run table. Returns 0 when the index falls in no run.
std::uint32_t gb4BmpLookup(std::uint32_t lin)
{
    std::size_t lo = 0, hi = gb4BmpCount;
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        const Gb4Range& r = gb4Bmp[mid];
        if (lin < r.linFirst)
            hi = mid;
        else if (lin > r.linLast)
            lo = mid + 1;
        else
            return r.cpFirst + (lin - r.linFirst);
    }
    return 0;
}

// Single-byte high-half decode (0x80..0xFF). Returns the mapped
// codePoint, or 0xFFFF when the table marks the slot unmapped.
inline std::uint32_t sbLookup(const std::uint16_t* high, std::uint8_t b) { return high[b - 0x80]; }

// Emit one codePoint and reset to ground.
inline EncStatus emit(EncDecoder* d, std::uint32_t codePoint, std::uint32_t* outputBuffer)
{
    d->pending = 0;
    d->acc = 0;
    d->need = 0;
    *outputBuffer = codePoint;
    return EncStatus::Emit;
}

inline EncStatus invalid(EncDecoder* d)
{
    d->pending = 0;
    d->acc = 0;
    d->need = 0;
    return EncStatus::Invalid;
}

} // namespace

std::optional<Encoding> encodingFromName(std::string_view name)
{
    struct Alias {
        const char* canon;
        Encoding enc;
    };
    static const Alias aliases[] = {
        { "utf8", Encoding::UTF8 },
        { "latin1", Encoding::Latin1 },
        { "iso88591", Encoding::Latin1 },
        { "l1", Encoding::Latin1 },
        { "iso885915", Encoding::Iso8859_15 },
        { "latin9", Encoding::Iso8859_15 },
        { "l9", Encoding::Iso8859_15 },
        { "koi8r", Encoding::Koi8R },
        { "koi8", Encoding::Koi8R },
        { "windows1252", Encoding::Windows1252 },
        { "cp1252", Encoding::Windows1252 },
        { "shiftjis", Encoding::ShiftJis },
        { "sjis", Encoding::ShiftJis },
        { "cp932", Encoding::ShiftJis },
        { "eucjp", Encoding::EucJp },
        { "euckr", Encoding::EucKr },
        { "cp949", Encoding::EucKr },
        { "uhc", Encoding::EucKr },
        { "gb18030", Encoding::Gb18030 },
        { "gbk", Encoding::Gb18030 },
        { "gb2312", Encoding::Gb18030 },
        { "big5", Encoding::Big5 },
    };
    for (const Alias& a : aliases) {
        if (nameEq(name, a.canon))
            return a.enc;
    }
    return std::nullopt;
}

const char* encodingName(Encoding e)
{
    switch (e) {
    case Encoding::UTF8:
        return "utf-8";
    case Encoding::Latin1:
        return "latin1";
    case Encoding::Iso8859_15:
        return "iso-8859-15";
    case Encoding::Koi8R:
        return "koi8-r";
    case Encoding::Windows1252:
        return "windows-1252";
    case Encoding::ShiftJis:
        return "shift_jis";
    case Encoding::EucJp:
        return "euc-jp";
    case Encoding::EucKr:
        return "euc-kr";
    case Encoding::Gb18030:
        return "gb18030";
    case Encoding::Big5:
        return "big5";
    }
    return "utf-8";
}

void encReset(EncDecoder* d, Encoding e)
{
    d->enc = e;
    d->pending = 0;
    d->acc = 0;
    d->need = 0;
}

// GB18030 byte feed. State across calls:
//   need==0          ground.
//   need==1, acc=0   one lead byte held (b1 in pending); 2-byte vs 4-byte
//                    still undetermined (decided by the next byte).
//   need==2, acc!=0  4-byte form confirmed; acc packs (b1<<8|b2); awaiting
//                    b3 (0x81..0xFE).
//   need==1, acc!=0  4-byte; acc packs (b1<<16|b2<<8|b3); awaiting b4.
static EncStatus gb18030Feed(EncDecoder* d, std::uint8_t b, std::uint32_t* outputBuffer)
{
    if (d->need == 0) {
        if (b < 0x80)
            return emit(d, b, outputBuffer);
        if (b == 0x80 || b == 0xFF) // undefined lead bytes
            return invalid(d);
        d->pending = b; // 0x81..0xFE lead
        d->acc = 0;
        d->need = 1;
        return EncStatus::Need;
    }

    if (d->acc == 0 && d->need == 1) { // second byte; pick the form
        std::uint8_t b1 = static_cast<std::uint8_t>(d->pending);
        if (b >= 0x30 && b <= 0x39) { // 4-byte form
            d->acc = (static_cast<std::uint32_t>(b1) << 8) | b;
            d->need = 2;
            return EncStatus::Need;
        }
        // 2-byte GBK form.
        if (b == 0x7F || b < 0x40)
            return invalid(d);
        std::uint16_t key = static_cast<std::uint16_t>((b1 << 8) | b);
        std::uint32_t codePoint = dbcsLookup(gbkLeadRow, gbkRows, key);
        if (codePoint == 0)
            return invalid(d);
        return emit(d, codePoint, outputBuffer);
    }

    if (d->need == 2) { // b3 of the 4-byte form
        if (b < 0x81 || b > 0xFE)
            return invalid(d);
        d->acc = (d->acc << 8) | b;
        d->need = 1;
        return EncStatus::Need;
    }

    // need==1, acc!=0: b4 of the 4-byte form.
    if (b < 0x30 || b > 0x39)
        return invalid(d);
    std::uint8_t b1 = static_cast<std::uint8_t>((d->acc >> 16) & 0xFF);
    std::uint8_t b2 = static_cast<std::uint8_t>((d->acc >> 8) & 0xFF);
    std::uint8_t b3 = static_cast<std::uint8_t>(d->acc & 0xFF);
    std::uint8_t b4 = b;
    // Linear index over the 4-byte space (b1∈81..FE, b2/b4∈30..39, b3∈81..FE).
    std::uint32_t lin = ((static_cast<std::uint32_t>(b1) - 0x81) * 10 + (b2 - 0x30)) * 1260 + (static_cast<std::uint32_t>(b3) - 0x81) * 10 + (b4 - 0x30);
    // SMP plane: U+10000..U+10FFFF is a single affine range (b1 0x90..0xE3).
    // Its linear base is the index of the first 4-byte form 90 30 81 30.
    const std::uint32_t smpLinBase = ((0x90u - 0x81u) * 10) * 1260; // b2=b3lead offset 0; = 15*10*1260
    if (lin >= smpLinBase) {
        std::uint32_t codePoint = 0x10000u + (lin - smpLinBase);
        if (codePoint > 0x10FFFF)
            return invalid(d);
        return emit(d, codePoint, outputBuffer);
    }
    // BMP plane: coalesced run table.
    std::uint32_t codePoint = gb4BmpLookup(lin);
    if (codePoint == 0)
        return invalid(d);
    return emit(d, codePoint, outputBuffer);
}

// `need`/`pending` encode the in-progress multibyte state per characterSet:
//   need == 0          ground.
//   ShiftJis/Big5/Kr   need==1, pending = lead byte (one trail expected).
//   EucJp G1           need==1, pending = lead.
//   EucJp SS2 (0x8E)   need==1, pending = 0xFF00 sentinel (kana trail).
//   EucJp SS3 (0x8F)   need==2 then 1, pending accumulates the two trail
//                      bytes of the JIS X 0212 form.
//   GB18030 2-byte     need==1, pending = lead.
//   GB18030 4-byte     need==3/2/1, pending packs the consumed bytes.
//
// The GB18030 4-byte path is recognized only once the SECOND byte arrives
// (a 0x30..0x39 trailing a 0x81..0xFE lead); until then the lead is held
// as a possible 2-byte form.
EncStatus encFeed(EncDecoder* d, std::uint8_t b, std::uint32_t* outputBuffer)
{
    switch (d->enc) {

    // Single-byte: ASCII low half, table high half.
    case Encoding::Latin1:
    case Encoding::Iso8859_15:
    case Encoding::Koi8R:
    case Encoding::Windows1252: {
        if (b < 0x80)
            return emit(d, b, outputBuffer);
        const std::uint16_t* high = d->enc == Encoding::Latin1 ? latin1High : d->enc == Encoding::Iso8859_15 ? iso885915High
            : d->enc == Encoding::Koi8R                                                                      ? koi8rHigh
                                                                                                             : cp1252High;
        std::uint32_t codePoint = sbLookup(high, b);
        if (codePoint == 0xFFFF)
            return invalid(d);
        return emit(d, codePoint, outputBuffer);
    }

    case Encoding::ShiftJis: {
        if (d->need == 0) {
            if (b < 0x80)
                return emit(d, b, outputBuffer);
            if (b >= 0xA1 && b <= 0xDF) // half-width katakana
                return emit(d, 0xFF61u + (b - 0xA1), outputBuffer);
            if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
                d->pending = b;
                d->need = 1;
                return EncStatus::Need;
            }
            return invalid(d);
        }
        std::uint16_t key = static_cast<std::uint16_t>((d->pending << 8) | b);
        std::uint32_t codePoint = dbcsLookup(shiftJisLeadRow, shiftJisRows, key);
        if (codePoint == 0)
            return invalid(d);
        return emit(d, codePoint, outputBuffer);
    }

    // EUC-JP. `pending` holds the in-progress state, with two sentinel bits:
    //   0xFF00  — SS2 (0x8E) seen; the next byte is a half-width kana trail.
    //   0x8000  — SS3 (0x8F) path (JIS X 0212 / G3); the low byte holds the
    //             first G3 trail once seen (0 while still expecting it).
    // Otherwise `pending` is a plain G1 lead (0xA1..0xFE) awaiting its trail.
    case Encoding::EucJp: {
        if (d->need == 0) {
            if (b < 0x80)
                return emit(d, b, outputBuffer);
            if (b == 0x8E) { // SS2: half-width kana
                d->pending = 0xFF00;
                d->need = 1;
                return EncStatus::Need;
            }
            if (b == 0x8F) { // SS3: JIS X 0212 (G3)
                d->pending = 0x8000;
                d->need = 2;
                return EncStatus::Need;
            }
            if (b >= 0xA1 && b <= 0xFE) { // G1 lead
                d->pending = b;
                d->need = 1;
                return EncStatus::Need;
            }
            return invalid(d);
        }
        if (d->pending == 0xFF00) { // SS2 kana trail
            if (b >= 0xA1 && b <= 0xDF)
                return emit(d, 0xFF61u + (b - 0xA1), outputBuffer);
            return invalid(d);
        }
        if (d->pending & 0x8000) { // SS3 / G3 path
            if (b < 0xA1 || b > 0xFE)
                return invalid(d);
            if (d->need == 2) { // first G3 trail byte
                d->pending = static_cast<std::uint16_t>(0x8000 | b);
                d->need = 1;
                return EncStatus::Need;
            }
            std::uint16_t key = static_cast<std::uint16_t>(((d->pending & 0xFF) << 8) | b);
            std::uint32_t codePoint = dbcsLookup(eucJpG3LeadRow, eucJpG3Rows, key);
            if (codePoint == 0)
                return invalid(d);
            return emit(d, codePoint, outputBuffer);
        }
        // G1 second byte.
        std::uint16_t key = static_cast<std::uint16_t>((d->pending << 8) | b);
        std::uint32_t codePoint = dbcsLookup(eucJpLeadRow, eucJpRows, key);
        if (codePoint == 0)
            return invalid(d);
        return emit(d, codePoint, outputBuffer);
    }

    case Encoding::EucKr: {
        if (d->need == 0) {
            if (b < 0x80)
                return emit(d, b, outputBuffer);
            if (b >= 0x81 && b <= 0xFE) {
                d->pending = b;
                d->need = 1;
                return EncStatus::Need;
            }
            return invalid(d);
        }
        std::uint16_t key = static_cast<std::uint16_t>((d->pending << 8) | b);
        std::uint32_t codePoint = dbcsLookup(cp949LeadRow, cp949Rows, key);
        if (codePoint == 0)
            return invalid(d);
        return emit(d, codePoint, outputBuffer);
    }

    case Encoding::Big5: {
        if (d->need == 0) {
            if (b < 0x80)
                return emit(d, b, outputBuffer);
            if (b >= 0x81 && b <= 0xFE) {
                d->pending = b;
                d->need = 1;
                return EncStatus::Need;
            }
            return invalid(d);
        }
        std::uint16_t key = static_cast<std::uint16_t>((d->pending << 8) | b);
        std::uint32_t codePoint = dbcsLookup(big5LeadRow, big5Rows, key);
        if (codePoint == 0)
            return invalid(d);
        return emit(d, codePoint, outputBuffer);
    }

    case Encoding::Gb18030:
        return gb18030Feed(d, b, outputBuffer); // see gb18030Feed for the state encoding

    case Encoding::UTF8:
        // Pass-through: the caller never routes UTF-8 through here.
        return emit(d, b, outputBuffer);
    }
    return invalid(d);
}

// Bytes before the first one with its high bit set, eight at a time.
//
// Every encoding this decoder speaks maps a byte below 0x80 to itself while no
// multi-byte sequence is open — the single-byte tables have no open state at
// all, and each multi-byte case guards on it — so such a run is a copy, not a
// decode. That identity is what this scan is for, and it is only sound where
// the caller has checked the state.
[[nodiscard]] static inline std::size_t asciiRunLength(std::span<const std::uint8_t> bytes)
{
    const std::uint8_t* in = bytes.data();
    const std::size_t length = bytes.size();
    std::size_t i = 0;
    while (i + 8 <= length) {
        std::uint64_t word;
        std::memcpy(&word, in + i, 8);
        const std::uint64_t highBits = word & 0x8080808080808080ull;
        if (highBits)
            return i + (static_cast<std::size_t>(__builtin_ctzll(highBits)) >> 3); // little-endian
        i += 8;
    }
    while (i < length && in[i] < 0x80)
        ++i;
    return i;
}

std::size_t encDecodeToUTF8(EncDecoder* d, std::span<const std::uint8_t> in, std::span<std::uint8_t> outputBuffer, std::size_t* consumed)
{
    const std::size_t inputLength = in.size();
    const std::size_t outputCapacity = outputBuffer.size();
    std::size_t w = 0, i = 0;
    auto putCodePoint = [&](std::uint32_t codePoint) -> bool {
        // UTF-8 encode; needs <= 4 bytes. Bail if it won't fit.
        if (codePoint < 0x80) {
            if (w + 1 > outputCapacity)
                return false;
            outputBuffer[w++] = static_cast<std::uint8_t>(codePoint);
        } else if (codePoint < 0x800) {
            if (w + 2 > outputCapacity)
                return false;
            outputBuffer[w++] = static_cast<std::uint8_t>(0xC0 | (codePoint >> 6));
            outputBuffer[w++] = static_cast<std::uint8_t>(0x80 | (codePoint & 0x3F));
        } else if (codePoint < 0x10000) {
            if (w + 3 > outputCapacity)
                return false;
            outputBuffer[w++] = static_cast<std::uint8_t>(0xE0 | (codePoint >> 12));
            outputBuffer[w++] = static_cast<std::uint8_t>(0x80 | ((codePoint >> 6) & 0x3F));
            outputBuffer[w++] = static_cast<std::uint8_t>(0x80 | (codePoint & 0x3F));
        } else {
            if (w + 4 > outputCapacity)
                return false;
            outputBuffer[w++] = static_cast<std::uint8_t>(0xF0 | (codePoint >> 18));
            outputBuffer[w++] = static_cast<std::uint8_t>(0x80 | ((codePoint >> 12) & 0x3F));
            outputBuffer[w++] = static_cast<std::uint8_t>(0x80 | ((codePoint >> 6) & 0x3F));
            outputBuffer[w++] = static_cast<std::uint8_t>(0x80 | (codePoint & 0x3F));
        }
        return true;
    };

    while (i < inputLength) {
        // An ASCII run with no sequence open is the same bytes out. Real
        // output in these encodings is mostly that — paths, numbers, the text
        // around an escape sequence — and byte at a time it cost a call, a
        // ten-way switch and a capacity check each.
        if (d->need == 0) {
            const std::size_t run = asciiRunLength(in.subspan(i));
            if (run > 0) {
                const std::size_t room = outputCapacity - w;
                const std::size_t copied = run < room ? run : room;
                if (copied == 0)
                    break; // no room for even one byte
                std::memcpy(outputBuffer.data() + w, in.data() + i, copied);
                w += copied;
                i += copied;
                if (copied < run)
                    break; // the output filled mid-run
                continue;
            }
        }
        std::uint8_t b = in[i];
        bool wasMid = (d->need > 0);
        std::uint32_t codePoint;
        EncStatus st = encFeed(d, b, &codePoint);
        if (st == EncStatus::Emit) {
            if (!putCodePoint(codePoint))
                break;
        } else if (st == EncStatus::Invalid) {
            if (!putCodePoint(0xFFFD))
                break;
            // Mid-sequence break: re-feed the offending byte as a fresh
            // start (it may itself begin a valid sequence), matching the
            // UTF-8 path's recovery contract.
            if (wasMid) {
                std::uint32_t cp2;
                EncStatus st2 = encFeed(d, b, &cp2);
                if (st2 == EncStatus::Emit) {
                    if (!putCodePoint(cp2))
                        break;
                } else if (st2 == EncStatus::Invalid) {
                    if (!putCodePoint(0xFFFD))
                        break;
                }
            }
        }
        // Need: carry to the next byte, nothing emitted.
        ++i;
    }
    *consumed = i;
    return w;
}

} // namespace Ubiquitty::Unicode
