// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_utf8_simd_parity.cpp — fuzz the simdutf-backed utf8DecodeRun against a
// scalar reference. UTF8Decoder.cpp calls simdutf unconditionally, so the real
// SIMD path runs here; the scalar reference below is this file's own copy of the
// loop the decoder falls through to, and the two must agree.
// Adversarial corpus (valid multibyte, ASCII boundaries, invalid leads, stray
// continuations, truncated, overlong, surrogates) must produce identical
// (count, bytesConsumed, codepoints) on both paths.
//
//   c++ -std=c++17 -O2 -ISource -ISource/ThirdParty/simdutf \
//       Tests/Core/Unicode/test_utf8_simd_parity.cpp Source/Core/Unicode/UTF8Decoder.cpp \
//       Source/ThirdParty/simdutf/simdutf.cpp -o /tmp/ptest && /tmp/ptest
#include "Core/Unicode/UTF8Decoder.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

// ---- scalar reference: byte-identical to UTF8Decoder.cpp's scalar loop ----
static bool refValid(std::uint32_t codePoint, std::uint32_t min)
{
    if (codePoint < min)
        return false;
    if (codePoint > 0x10FFFF)
        return false;
    if (codePoint >= 0xD800 && codePoint <= 0xDFFF)
        return false;
    return true;
}
static int refDecode(const char* in, int length, std::uint32_t* outputBuffer, int maxOut, int* consumed)
{
    int p = 0, n = 0;
    while (p < length && n < maxOut) {
        const std::uint8_t b0 = (std::uint8_t)in[p];
        if (b0 < 0xC2)
            break;
        int need;
        std::uint32_t acc, min;
        if ((b0 & 0xE0) == 0xC0) {
            need = 1;
            acc = b0 & 0x1Fu;
            min = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            need = 2;
            acc = b0 & 0x0Fu;
            min = 0x800;
        } else if ((b0 & 0xF8) == 0xF0 && b0 <= 0xF4) {
            need = 3;
            acc = b0 & 0x07u;
            min = 0x10000;
        } else
            break;
        if (p + 1 + need > length)
            break;
        bool ok = true;
        for (int k = 1; k <= need; ++k) {
            std::uint8_t bk = (std::uint8_t)in[p + k];
            if ((bk & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            acc = (acc << 6) | (bk & 0x3Fu);
        }
        if (!ok || !refValid(acc, min))
            break;
        outputBuffer[n++] = acc;
        p += 1 + need;
    }
    *consumed = p;
    return n;
}

// ---- deterministic PRNG ----
static std::uint64_t g = 0x9E3779B97F4A7C15ull;
static std::uint32_t rnd()
{
    g ^= g << 13;
    g ^= g >> 7;
    g ^= g << 17;
    return (std::uint32_t)(g >> 32);
}

static void emit(std::vector<char>& v, std::uint32_t codePoint)
{
    if (codePoint < 0x80)
        v.push_back((char)codePoint);
    else if (codePoint < 0x800) {
        v.push_back((char)(0xC0 | (codePoint >> 6)));
        v.push_back((char)(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        v.push_back((char)(0xE0 | (codePoint >> 12)));
        v.push_back((char)(0x80 | ((codePoint >> 6) & 0x3F)));
        v.push_back((char)(0x80 | (codePoint & 0x3F)));
    } else {
        v.push_back((char)(0xF0 | (codePoint >> 18)));
        v.push_back((char)(0x80 | ((codePoint >> 12) & 0x3F)));
        v.push_back((char)(0x80 | ((codePoint >> 6) & 0x3F)));
        v.push_back((char)(0x80 | (codePoint & 0x3F)));
    }
}

static void testOutputCapacityUsesSequenceCount()
{
    std::vector<char> cjk;
    std::vector<char> emoji;
    for (int i = 0; i < 128; ++i) {
        emit(cjk, 0x4E00 + static_cast<std::uint32_t>(i));
        emit(emoji, 0x1F300 + static_cast<std::uint32_t>(i));
    }

    std::uint32_t outputBuffer[16];
    int consumed = 0;
    int n = Ubiquitty::Unicode::utf8DecodeRun(std::span { cjk.data(), static_cast<std::size_t>(static_cast<int>(cjk.size())) }, std::span { outputBuffer, static_cast<std::size_t>(16) }, &consumed);
    CHECK(n == 16);
    CHECK(consumed == 16 * 3);
    CHECK(outputBuffer[15] == 0x4E0F);

    consumed = 0;
    n = Ubiquitty::Unicode::utf8DecodeRun(std::span { emoji.data(), static_cast<std::size_t>(static_cast<int>(emoji.size())) }, std::span { outputBuffer, static_cast<std::size_t>(16) }, &consumed);
    CHECK(n == 16);
    CHECK(consumed == 16 * 4);
    CHECK(outputBuffer[15] == 0x1F30F);
}

/// The caller's output capacity, not the content, decides which path decodes.
///
/// `VT.cpp` sizes `codePointCapacity` as `min(4096, rightMargin - column + 1)`,
/// so the span the converter may scan is twice the cells left on the row. The
/// converter needs 96 contiguous non-ASCII bytes. On an 80-column row that
/// admits it at the left margin and forbids it from column 34 onward, and the
/// same bytes then decode through two different mechanisms depending on where
/// the cursor stands.
///
/// Both arms of that are asserted here, and the second assertion is what keeps
/// the first from being vacuous: identical output is the answer a build with no
/// converter also gives. The counter says the wide call entered it and the
/// narrow call did not, so the comparison is between two mechanisms rather than
/// between one mechanism and itself.
static void testTheOutputCapacityDecidesWhichPathDecodes()
{
    std::vector<char> cjk; // 3 bytes each, so 64 of them is a 192-byte run.
    for (int i = 0; i < 64; ++i)
        emit(cjk, 0x4E00 + static_cast<std::uint32_t>(i));
    const int length = static_cast<int>(cjk.size());

    // The gate admits a span of `maxOut * 2` bytes when it reaches 96, so 48
    // cells is the last capacity that clears it and 47 is the first that does
    // not. Written as the boundary rather than as one value on each side: a
    // capacity picked with slack cannot tell a moved gate from a gate that
    // stopped being reached at all.
    const int atLeftMargin = 80;
    const int lastCapacityThatAdmits = 48;
    const int pastTheWindow = 47;

    std::uint32_t wide[128];
    std::uint32_t narrow[128];
    int wideConsumed = 0;
    int narrowConsumed = 0;

    const std::uint64_t before = Ubiquitty::Unicode::simdRunCountForTest;
    const int wideCount = Ubiquitty::Unicode::utf8DecodeRun(std::span { cjk.data(), static_cast<std::size_t>(length) }, std::span { wide, static_cast<std::size_t>(atLeftMargin) }, &wideConsumed);
    const std::uint64_t afterWide = Ubiquitty::Unicode::simdRunCountForTest;
    const int narrowCount = Ubiquitty::Unicode::utf8DecodeRun(std::span { cjk.data(), static_cast<std::size_t>(length) }, std::span { narrow, static_cast<std::size_t>(pastTheWindow) }, &narrowConsumed);
    const std::uint64_t afterNarrow = Ubiquitty::Unicode::simdRunCountForTest;

    CHECK(afterWide > before);
    CHECK(afterNarrow == afterWide);

    // The other side of the same boundary, one cell wider.
    std::uint32_t admitted[128];
    int admittedConsumed = 0;
    const int admittedCount = Ubiquitty::Unicode::utf8DecodeRun(std::span { cjk.data(), static_cast<std::size_t>(length) }, std::span { admitted, static_cast<std::size_t>(lastCapacityThatAdmits) }, &admittedConsumed);
    CHECK(Ubiquitty::Unicode::simdRunCountForTest > afterNarrow);
    // 96 bytes of 3-byte sequences, so the span fills before the capacity does
    // and the call stops at 32 rather than 48. The gate is spelled in bytes and
    // the caller counts cells, which is why the two numbers differ at all.
    CHECK(admittedCount == 32);
    CHECK(admittedConsumed == 96);
    CHECK(std::memcmp(wide, admitted, static_cast<std::size_t>(admittedCount) * sizeof(std::uint32_t)) == 0);

    // The narrow call returns less, and what it returns has to be the wide
    // call's prefix code point for code point. A row that wraps decodes its
    // tail through the other path, and a disagreement here is a row whose
    // characters change with the column they started in.
    CHECK(narrowCount > 0);
    CHECK(narrowCount <= wideCount);
    CHECK(narrowConsumed == narrowCount * 3);
    CHECK(std::memcmp(wide, narrow, static_cast<std::size_t>(narrowCount) * sizeof(std::uint32_t)) == 0);

    // Walking the row in narrow steps reaches the same characters as one wide
    // call, which is the property a wrapped row depends on.
    std::vector<std::uint32_t> walked;
    int offset = 0;
    while (offset < length) {
        std::uint32_t chunk[128];
        int consumed = 0;
        const int n = Ubiquitty::Unicode::utf8DecodeRun(std::span { cjk.data() + offset, static_cast<std::size_t>(length - offset) }, std::span { chunk, static_cast<std::size_t>(pastTheWindow) }, &consumed);
        if (n <= 0 || consumed <= 0)
            break;
        walked.insert(walked.end(), chunk, chunk + n);
        offset += consumed;
    }
    CHECK(offset == length);
    CHECK(walked.size() == 64);
    CHECK(std::memcmp(walked.data(), wide, static_cast<std::size_t>(wideCount) * sizeof(std::uint32_t)) == 0);
}

// The SIMD path bounds its span by BYTES (maxOut*2), so a call may legally
// return FEWER than maxOut codePoints with more decodable input remaining.
// Pin the boundary math: whatever is returned must be a whole-sequence prefix
// that byte-for-byte matches the scalar reference, and looping the calls must
// reproduce the reference decode of the whole run.
static void testByteLimitReturnsWholeSequencePrefix()
{
    std::vector<char> cjk; // 3-byte sequences: maxOut*2 = 98 is never sequence-aligned
    for (int i = 0; i < 256; ++i)
        emit(cjk, 0x4E00 + static_cast<std::uint32_t>(i));

    std::uint32_t outputBuffer[64];
    std::uint32_t ref[512];
    int refConsumed = 0;
    const int refN = refDecode(cjk.data(), static_cast<int>(cjk.size()), ref, 512, &refConsumed);

    int consumed = 0;
    const int n = Ubiquitty::Unicode::utf8DecodeRun(std::span { cjk.data(), static_cast<std::size_t>(static_cast<int>(cjk.size())) }, std::span { outputBuffer, static_cast<std::size_t>(49) }, &consumed);
    CHECK(n > 0);
    CHECK(n <= 49);
    CHECK(consumed == n * 3); // whole sequences only — never a split tail
    CHECK(memcmp(outputBuffer, ref, static_cast<std::size_t>(n) * 4) == 0);

    // Looping the batched calls reproduces the full reference decode.
    int total = 0, p = 0;
    while (p < static_cast<int>(cjk.size())) {
        int c = 0;
        const int k = Ubiquitty::Unicode::utf8DecodeRun(std::span { cjk.data() + p, static_cast<std::size_t>(static_cast<int>(cjk.size()) - p) }, std::span { outputBuffer, static_cast<std::size_t>(49) }, &c);
        if (k == 0)
            break;
        CHECK(total + k <= refN);
        CHECK(memcmp(outputBuffer, ref + total, static_cast<std::size_t>(k) * 4) == 0);
        total += k;
        p += c;
    }
    CHECK(total == refN);
    CHECK(p == refConsumed);
}

// A sequence split by the END of the input (a PTY read boundary) must not
// poison the SIMD span: the valid prefix decodes, consumed stops before the
// split lead, exactly as the scalar reference does.
static void testSplitTailAtEndOfInput()
{
    std::vector<char> buffer;
    for (int i = 0; i < 64; ++i) // 256 bytes — well past the SIMD gate
        emit(buffer, 0x1F300 + static_cast<std::uint32_t>(i));
    buffer.push_back(static_cast<char>(0xF0)); // split 4-byte sequence: lead + 2 of 3 continuations
    buffer.push_back(static_cast<char>(0x9F));
    buffer.push_back(static_cast<char>(0x8C));

    std::uint32_t a[512], b[512];
    int ca = 0, cb = 0;
    const int na = Ubiquitty::Unicode::utf8DecodeRun(std::span { buffer.data(), static_cast<std::size_t>(static_cast<int>(buffer.size())) }, std::span { a, static_cast<std::size_t>(512) }, &ca);
    const int nb = refDecode(buffer.data(), static_cast<int>(buffer.size()), b, 512, &cb);
    CHECK(na == nb);
    CHECK(ca == cb);
    CHECK(memcmp(a, b, static_cast<std::size_t>(na) * 4) == 0);
}

// An invalid byte INSIDE a long span (the word-wide bound no longer stops
// before it) must fall through to the scalar loop's exact stop.
static void testInvalidInteriorFallsToScalarStop()
{
    std::vector<char> buffer;
    for (int i = 0; i < 40; ++i)
        emit(buffer, 0x4E00 + static_cast<std::uint32_t>(i)); // 120 valid bytes
    buffer.push_back(static_cast<char>(0xED)); // surrogate lead: 0xED 0xA0 0x80 = U+D800
    buffer.push_back(static_cast<char>(0xA0));
    buffer.push_back(static_cast<char>(0x80));
    for (int i = 0; i < 40; ++i)
        emit(buffer, 0x4E40 + static_cast<std::uint32_t>(i));

    std::uint32_t a[512], b[512];
    int ca = 0, cb = 0;
    const int na = Ubiquitty::Unicode::utf8DecodeRun(std::span { buffer.data(), static_cast<std::size_t>(static_cast<int>(buffer.size())) }, std::span { a, static_cast<std::size_t>(512) }, &ca);
    const int nb = refDecode(buffer.data(), static_cast<int>(buffer.size()), b, 512, &cb);
    CHECK(na == nb);
    CHECK(ca == cb); // stops exactly at the surrogate lead
    CHECK(na == 40);
    CHECK(memcmp(a, b, static_cast<std::size_t>(na) * 4) == 0);
}

int main()
{
    testOutputCapacityUsesSequenceCount();
    testTheOutputCapacityDecidesWhichPathDecodes();
    testByteLimitReturnsWholeSequencePrefix();
    testSplitTailAtEndOfInput();
    testInvalidInteriorFallsToScalarStop();
    const int ITERS = 400000;
    long mism = 0, simdRuns = 0;
    std::vector<char> buffer;
    std::vector<std::uint32_t> a(8192), b(8192);
    for (int it = 0; it < ITERS; ++it) {
        buffer.clear();
        // The vectorised path needs a clean multibyte span of at least 96 bytes,
        // so a purely adversarial mix reaches it only by rare accident. Every
        // fourth round leads with a long clean run so the fast path is compared
        // rather than merely available, and the adversarial tail still follows it.
        if ((rnd() % 4) == 0) {
            const int runLength = 40 + (int)(rnd() % 200);
            for (int k = 0; k < runLength; ++k) {
                switch (rnd() % 3) {
                case 0:
                    emit(buffer, 0x1F300 + (rnd() % 0x400));
                    break;
                case 1:
                    emit(buffer, 0x4E00 + (rnd() % 0x3000));
                    break;
                default:
                    emit(buffer, 0x00A1 + (rnd() % 0x5E));
                    break;
                }
            }
        }
        int tokens = 1 + (rnd() % 48);
        for (int t = 0; t < tokens; ++t) {
            switch (rnd() % 10) {
            case 0:
            case 1:
            case 2:
                emit(buffer, 0x1F300 + (rnd() % 0x400));
                break; // 4-byte
            case 3:
            case 4:
                emit(buffer, 0x4E00 + (rnd() % 0x3000));
                break; // 3-byte CJK
            case 5:
                emit(buffer, 0x00A1 + (rnd() % 0x5E));
                break; // 2-byte
            case 6:
                buffer.push_back((char)(0x20 + (rnd() % 0x5F)));
                break; // ASCII
            case 7:
                buffer.push_back((char)(0x80 + (rnd() % 0x40)));
                break; // stray continuation
            case 8:
                buffer.push_back((char)(0xC0 + (rnd() % 0x05)));
                break; // invalid/overlong lead (C0,C1,..)
            case 9: { // truncated multibyte (lead + too few continuations)
                buffer.push_back((char)0xE0);
                if (rnd() & 1)
                    buffer.push_back((char)0xA0);
                break;
            }
            }
        }
        const char* in = buffer.data();
        int length = (int)buffer.size();
        // Walk the whole buffer the way the caller does: decode multibyte runs,
        // skip ASCII boundaries, compare both paths step for step.
        int pa = 0, pb = 0;
        while (pa < length && pb < length) {
            int ca = 0, cb = 0;
            int na = Ubiquitty::Unicode::utf8DecodeRun(std::span { in + pa, static_cast<std::size_t>(length - pa) }, std::span { a.data(), static_cast<std::size_t>(8192) }, &ca);
            int nb = refDecode(in + pb, length - pb, b.data(), 8192, &cb);
            if (na != nb || ca != cb || (na > 0 && memcmp(a.data(), b.data(), (size_t)na * 4))) {
                if (++mism <= 5)
                    std::printf("MISMATCH it=%d off=%d: simd(n=%d,c=%d) scalar(n=%d,c=%d)\n", it, pa, na, ca, nb, cb);
                break;
            }
            // Read off the decoder rather than inferred from the input: every
            // quantity computable here is the same whether or not the fast path
            // was compiled, so a byte-count threshold counts runs in a build
            // that has no fast path to take.
            simdRuns = static_cast<long>(Ubiquitty::Unicode::simdRunCountForTest);
            if (ca == 0) { // ASCII/invalid boundary — advance one byte like the caller
                if (pa < length && (std::uint8_t)in[pa] < 0xC2) {
                    ++pa;
                    pb = pa;
                    continue;
                }
                break;
            }
            pa += ca;
            pb += cb;
        }
    }
    std::printf("iters=%d simd-path runs exercised=%ld mismatches=%ld\n", ITERS, simdRuns, mism);
    if (mism == 0 && simdRuns > 0 && failures == 0) {
        std::printf("PARITY OK\n");
        return 0;
    }
    std::printf("%s\n",
        simdRuns == 0 ? "FAIL: the vectorised path never ran — is the corpus below the length gate?"
                      : "FAIL: parity mismatch");
    return 1;
}
