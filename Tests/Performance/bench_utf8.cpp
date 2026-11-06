// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_utf8.cpp — scalar utf8DecodeRun (current) vs a simdutf-backed decode,
// over representative multibyte streams. Parity-checked, then timed.
//
//   c++ -std=c++17 -O2 -ISource Tests/Performance/bench_utf8.cpp \
//       Source/Core/Unicode/UTF8Decoder.cpp \
//       -I/opt/homebrew/include -L/opt/homebrew/lib -lsimdutf -o /tmp/bu && /tmp/bu
#include "Core/Unicode/UTF8Decoder.h"
#include "simdutf.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;

// simdutf-backed equivalent of utf8DecodeRun: bound the leading multibyte run
// to <= maxOut sequences (stop at first ASCII / incomplete trailing), convert
// that span; on a mid-span invalid, convert the valid prefix and stop there.
// First byte with the high bit CLEAR (ASCII/control), or length. Word-at-a-time,
// no per-codepoint branching — bounds the multibyte run cheaply.
static inline int firstASCIIOrEnd(const char* in, int length)
{
    int i = 0;
    while (i + 8 <= length) {
        std::uint64_t w;
        std::memcpy(&w, in + i, 8);
        std::uint64_t clear = ~w & 0x8080808080808080ull; // per-byte: set if <0x80
        if (clear) {
            for (int k = 0; k < 8; ++k)
                if ((std::uint8_t)in[i + k] < 0x80)
                    return i + k;
        }
        i += 8;
    }
    while (i < length && (std::uint8_t)in[i] >= 0x80)
        ++i;
    return i;
}

static int decodeSimd(std::span<const char> input, std::span<std::uint32_t> out, int* bytesConsumed)
{
    const char* in = input.data();
    const int length = static_cast<int>(input.size());
    std::uint32_t* outputBuffer = out.data();
    int mb = firstASCIIOrEnd(in, length);
    if (mb == 0) {
        *bytesConsumed = 0;
        return 0;
    }
    // Size-gate: simdutf's per-call (validate + convert) overhead loses to the
    // tight scalar loop on tiny runs (prose: sparse 1-2 codePoint multibyte). Only take
    // the SIMD path once the run is long enough to amortize it.
    static const int simdMinBytes = 32;
    if (mb < simdMinBytes)
        return Ubiquitty::Unicode::utf8DecodeRun(input, out, bytesConsumed);
    auto* o = reinterpret_cast<char32_t*>(outputBuffer);
    // Fast path: SIMD-validate the bounded span, then SIMD convert-valid,
    // rather than the validating-convert (_with_errors).
    if (simdutf::validate_utf8(in, mb)) {
        size_t nc = simdutf::convert_valid_utf8_to_utf32(in, mb, o);
        *bytesConsumed = mb;
        return (int)nc;
    }
    // Rare invalid span: scalar handles the exact stop + bytesConsumed.
    return Ubiquitty::Unicode::utf8DecodeRun(input, out, bytesConsumed);
}

static void emit(std::string& s, std::uint32_t codePoint)
{
    if (codePoint < 0x80)
        s.push_back((char)codePoint);
    else if (codePoint < 0x800) {
        s.push_back((char)(0xC0 | (codePoint >> 6)));
        s.push_back((char)(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        s.push_back((char)(0xE0 | (codePoint >> 12)));
        s.push_back((char)(0x80 | ((codePoint >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (codePoint & 0x3F)));
    } else {
        s.push_back((char)(0xF0 | (codePoint >> 18)));
        s.push_back((char)(0x80 | ((codePoint >> 12) & 0x3F)));
        s.push_back((char)(0x80 | ((codePoint >> 6) & 0x3F)));
        s.push_back((char)(0x80 | (codePoint & 0x3F)));
    }
}

static std::string stream(const char* kind)
{
    std::string s;
    const size_t N = 1u << 20; // ~1M codepoints
    if (!strcmp(kind, "emoji"))
        for (size_t i = 0; i < N; i++)
            emit(s, 0x1F300 + (i % 0x300));
    else if (!strcmp(kind, "cjk"))
        for (size_t i = 0; i < N; i++)
            emit(s, 0x4E00 + (i % 0x2000));
    else if (!strcmp(kind, "latin1"))
        for (size_t i = 0; i < N; i++)
            emit(s, 0x00A1 + (i % 0x5E));
    else if (!strcmp(kind, "mixed"))
        for (size_t i = 0; i < N; i++)
            emit(s, (i % 3 == 0) ? 0x1F600 + (i % 80) : (i % 3 == 1) ? 0x4E2D
                                                                     : 0x00E9);
    else if (!strcmp(kind, "prose")) {
        // ~realistic: mostly ASCII words with an occasional accent/emoji, so the
        // multibyte runs are TINY (1-2 codePoint) — worst case for per-call overhead.
        const char* w = "the quick brown fox jumps over ";
        for (size_t i = 0; i < N;) {
            for (const char* p = w; *p && i < N; ++p, ++i)
                s.push_back(*p);
            if (i < N) {
                emit(s, (i % 7 == 0) ? 0x1F642 : 0x00E9);
                ++i;
            }
        }
    }
    return s;
}

int main()
{
    const char* kinds[] = { "emoji", "cjk", "latin1", "mixed", "prose" };
    std::printf("simdutf impl: %s\n", std::string(simdutf::get_active_implementation()->name()).c_str());
    std::printf("%-8s %12s %12s %8s   (parity)\n", "stream", "scalar ns/codePoint", "simd ns/codePoint", "ratio");
    for (const char* k : kinds) {
        std::string src = stream(k);
        const char* in = src.data();
        int length = (int)src.size();
        std::vector<std::uint32_t> a(length + 8), b(length + 8);
        // parity: decode the whole stream both ways (big maxOut), compare.
        bool parity = true;
        int pa = 0, pb = 0;
        while (pa < length && pb < length) {
            int ca = 0, cb = 0;
            int na = Ubiquitty::Unicode::utf8DecodeRun(std::span { in + pa, static_cast<std::size_t>(length - pa) }, std::span { a.data(), static_cast<std::size_t>(length) }, &ca);
            int nb = decodeSimd(std::span { in + pb, static_cast<std::size_t>(length - pb) }, std::span { b.data(), static_cast<std::size_t>(length) }, &cb);
            if (na != nb || ca != cb || memcmp(a.data(), b.data(), (size_t)na * 4)) {
                parity = false;
                break;
            }
            if (ca == 0) { // ASCII boundary — caller's fast path skips it
                if (pa < length && (std::uint8_t)in[pa] < 0x80) {
                    while (pa < length && (std::uint8_t)in[pa] < 0x80)
                        ++pa;
                    pb = pa;
                    continue;
                }
                break;
            }
            pa += ca;
            pb += cb;
        }
        auto run = [&](int (*f)(std::span<const char>, std::span<std::uint32_t>, int*)) {
            std::vector<std::uint32_t> o(length + 8);
            double best = 1e18;
            for (int rep = 0; rep < 5; rep++) {
                auto t0 = clk::now();
                long codePoints = 0;
                int p = 0;
                while (p < length) {
                    int c = 0;
                    int n = f(std::span { in + p, static_cast<std::size_t>(length - p) }, std::span { o.data(), static_cast<std::size_t>(length) }, &c);
                    codePoints += n;
                    if (c == 0) {
                        if (p < length && (std::uint8_t)in[p] < 0x80) {
                            while (p < length && (std::uint8_t)in[p] < 0x80)
                                ++p;
                            continue;
                        }
                        break;
                    }
                    p += c;
                }
                double ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / (codePoints ? codePoints : 1);
                if (ns < best)
                    best = ns;
            }
            return best;
        };
        double sc = run(Ubiquitty::Unicode::utf8DecodeRun);
        double sd = run(decodeSimd);
        std::printf("%-8s %12.3f %12.3f %8.3f   %s\n", k, sc, sd, sd / sc, parity ? "OK" : "MISMATCH");
    }
    return 0;
}
