// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Legacy (non-UTF-8) streaming byte -> codePoint decoders sitting AHEAD of the
// UTF-8 stage. Default UTF8 is a pass-through the caller skips. Stateful: a
// multi-byte sequence may straddle a read boundary, carried in `pending`.
// Invalid resets to ground (caller substitutes U+FFFD); a lead byte that is
// itself a valid fresh start after a mid-sequence error is re-fed by the caller.

namespace Ubiquitty::Unicode {

enum class Encoding : std::uint8_t {
    UTF8 = 0, // default; pass-through (decoder not used)
    Latin1, // ISO-8859-1
    Iso8859_15, // ISO-8859-15 (Latin-9)
    Koi8R, // KOI8-R (Cyrillic)
    Windows1252, // CP1252 (Western, superset of Latin-1)
    ShiftJis, // Shift_JIS (Japanese)
    EucJp, // EUC-JP (Japanese)
    EucKr, // EUC-KR / CP949 (Korean)
    Gb18030, // GB18030 (Chinese; supersedes GBK/GB2312)
    Big5, // Big5 (Traditional Chinese)
};

// Map a config string ("utf-8", "latin1", "iso-8859-15", "koi8-r",
// "windows-1252", "shift_jis", "euc-jp", "euc-kr", "gb18030", "big5",
// and common aliases) to an Encoding, case-insensitively, ignoring '-'
// '_' '.' separators. Returns the Encoding on match, nullopt for an
// unrecognized name.
[[nodiscard]] std::optional<Encoding> encodingFromName(std::string_view name);

// Canonical lowercase name for an Encoding (for round-trip / logging).
const char* encodingName(Encoding e);

// True when `e` is the UTF-8 pass-through (caller skips the decoder).
inline bool encodingIsUTF8(Encoding e) { return e == Encoding::UTF8; }

// True for the single-byte legacy encodings, where a byte in 0x80-0x9F can only
// be a lone character (never a multibyte lead / single-shift). Gates whether raw
// C1 controls may be routed to the VT parser ahead of the characterSet decoder.
inline bool encodingIsSingleByte(Encoding e)
{
    switch (e) {
    case Encoding::Latin1:
    case Encoding::Iso8859_15:
    case Encoding::Koi8R:
    case Encoding::Windows1252:
        return true;
    case Encoding::UTF8:
    case Encoding::ShiftJis:
    case Encoding::EucJp:
    case Encoding::EucKr:
    case Encoding::Gb18030:
    case Encoding::Big5:
        return false;
    }
    return false;
}

enum class EncStatus : std::uint8_t {
    Need,
    Emit,
    Invalid,
};

// Streaming decoder state. POD; zero-init via encReset.
//   pending  — partial lead byte(s) of an in-progress sequence (+ EUC-JP SS2/SS3).
//   acc      — GB18030 4-byte accumulator (packs prior bytes b1..b3).
//   need     — trailing bytes still expected (0 = ground).
struct EncDecoder {
    Encoding enc;
    std::uint16_t pending;
    std::uint32_t acc;
    std::uint8_t need;
};

void encReset(EncDecoder* d, Encoding e);
EncStatus encFeed(EncDecoder* d, std::uint8_t byte, std::uint32_t* outCodePoint);

// Decode `in` to UTF-8 bytes in `out`, carrying partial state in `d`. Returns
// bytes written; *consumed gets input bytes consumed (always in.size() unless
// out filled). Unmapped / invalid bytes emit U+FFFD (EF BF BD). Precondition:
// out has room for >= 4 bytes per input byte (caller sizes it in.size()*4).
std::size_t encDecodeToUTF8(EncDecoder* d, std::span<const std::uint8_t> in, std::span<std::uint8_t> outputBuffer, std::size_t* consumed);

} // namespace Ubiquitty::Unicode
