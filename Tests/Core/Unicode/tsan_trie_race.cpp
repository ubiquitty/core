// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// tsan_trie_race — ThreadSanitizer negative for the lazy
// first-build of the Unicode width + grapheme-break tries being gated by
// std::call_once, so two PTY-parse threads (two panes) first-touching a trie
// concurrently can't race the build. Under call_once TSAN reports no data race
// (the build is serialized and happens-before its published reads). An
// unsynchronized `if (!ready) { build; ready = true; }` in its place races on
// the trie globals and TSAN flags it.
//
// Build (TSAN): see Scripts/run-tests.sh (test_unicode_trie_race section) —
//   c++ -std=c++23 -fsanitize=thread -pthread -DUBIQUITTY_UNICODE_NO_EAGER_PROPS_INIT \
//       -I Source Tests/Core/Unicode/tsan_trie_race.cpp -o <bin>
//
// The two trie builders (buildWidthTrie / buildGraphemeBreakTrie) and their
// global trie arrays live in file (anonymous-namespace) scope, and the only
// production caller that reaches them is the eager UnicodePropsInit global,
// which runs single-threaded before main and would pre-fire both call_once
// gates. We compile the real Width.cpp + GraphemeBreak.cpp into this
// translation unit with UBIQUITTY_UNICODE_NO_EAGER_PROPS_INIT defined, which
// omits only that eager instance — so the lazy call_once gate is reached for
// the very first time, concurrently, by this test's worker threads. That is
// exactly the two-pane first-touch race call_once closes, exercising the
// real builders, real trie globals, and the real synchronization under test.

#include "Core/Unicode/Width.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

// Pull in the production sources verbatim. The macro omits the eager
// UnicodePropsInit global so the call_once gates stay un-fired until the
// worker threads below first-touch them.
#include "Core/Unicode/GraphemeBreak.cpp"
#include "Core/Unicode/Width.cpp"

namespace {

// Code points chosen to force the trie path, never an ASCII / Latin fast path:
//   - U+0301 COMBINING ACUTE ACCENT (zero-width combining mark, Extend)
//   - U+4E00 CJK UNIFIED IDEOGRAPH (wide, width 2)
//   - U+1F600 GRINNING FACE (emoji, Extended_Pictographic, width 2)
//   - U+200D ZERO WIDTH JOINER (ZWJ grapheme class)
// Unicode::Width::forCodePoint(codePoint, ambiguous) with codePoint >= 0x300 walks the width trie via widthByte;
// graphemeBreakRawByteTrie(codePoint) walks the grapheme trie. Both are the gated
// first-build paths.
constexpr std::uint32_t triePathCodePoints[] = {
    0x0301,
    0x4E00,
    0x1F600,
    0x200D,
    0x0E31,
    0x1100,
    0xFE0F,
    0x1F1E6,
};

// One worker: hammer both gated first-build paths across the trie-forcing code
// points. The first thread to arrive triggers the build; the others must see a
// fully-published trie (call_once: build happens-before every probe).
void probeTries(std::atomic<std::uint64_t>* sink)
{
    std::uint64_t acc = 0;
    for (int rep = 0; rep < 64; ++rep) {
        for (std::uint32_t codePoint : triePathCodePoints) {
            // Width trie first-build path (widthByte under call_once).
            acc += static_cast<std::uint64_t>(Ubiquitty::Unicode::Width::forCodePoint(codePoint, Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow));
            acc += Ubiquitty::Unicode::Width::isEmojiPresentation(codePoint) ? 1u : 0u;
            // Grapheme-break trie first-build path (graphemeBreakRawByteTrie
            // under call_once) — file-static, reachable from this in-TU test.
            acc += graphemeBreakRawByteTrie(codePoint);
        }
    }
    sink->fetch_add(acc, std::memory_order_relaxed);
}

} // namespace

int main()
{
    // 4 threads all first-touch the two lazily-built tries at once. Under an
    // unsynchronized check-then-build this is a data race on the trie globals
    // (build state + the stage1/stage2 arrays); TSAN reports it. Under
    // std::call_once the build is serialized and published-before-read, so
    // TSAN is clean.
    constexpr int threads = 4;
    std::atomic<std::uint64_t> sink { 0 };
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int i = 0; i < threads; ++i)
        workers.emplace_back(probeTries, &sink);
    for (auto& t : workers)
        t.join();

    // Sanity: the tries resolve known properties (acc is non-trivial), so the
    // build actually ran and the probes read real data, not a zeroed table.
    if (sink.load() == 0) {
        std::fprintf(stderr, "tsan_trie_race: tries produced no data — build never ran\n");
        return 1;
    }
    std::printf("tsan_trie_race: %d threads, sink=%llu, TSAN-clean\n", threads, static_cast<unsigned long long>(sink.load()));
    return 0;
}
