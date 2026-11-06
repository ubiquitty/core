// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_ingest_corpora — ingest throughput per text class, straight through
// processBytes.
//
// Headless on purpose: no PTY, no renderer, no GUI, so a number here moves only
// when the parse/emit path does. Built to adjudicate a layout question where a
// live-pane reading could equally have been a render-side or transport artefact;
// kept because the corpora separate costs a single "unicode" lane conflates —
// byte width, cell width, combining marks and pen churn are different code paths
// and a change rarely moves them together.

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace {

using ClockType = std::chrono::steady_clock;

constexpr int benchRows = 25;
constexpr int benchColumns = 80;

// Every corpus is grown to the same byte count so MB/s is comparable across
// them, and each is far larger than the grid so scroll and emit both run hot.
std::string repeatToBytes(const std::string& unit, std::size_t targetBytes)
{
    std::string out;
    out.reserve(targetBytes + unit.size());
    while (out.size() < targetBytes)
        out += unit;
    return out;
}

struct Corpus {
    const char* name;
    std::string bytes;
};

// Fixed seeds, so every run and both arms of an A/B parse the same bytes.
std::uint32_t nextRandom(std::uint64_t& state)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(state >> 33);
}

// Lines of words two to ten letters long, one alphabet to a line, each line
// short of the grid so none wraps.
std::string multiscriptWords()
{
    constexpr std::uint32_t alphabets[][2] = {
        { 0x430, 0x44F }, // Cyrillic
        { 0x3B1, 0x3C9 }, // Greek
        { 0xE0, 0xFE }, // Latin-1 lowercase
        { 0x561, 0x586 }, // Armenian
    };
    std::uint64_t state = 0x5EED0001;
    std::string unit;
    for (int line = 0; line < 2048; ++line) {
        const auto& alphabet = alphabets[nextRandom(state) % std::size(alphabets)];
        const int target = 30 + static_cast<int>(nextRandom(state) % (benchColumns - 32));
        for (int used = 0; used < target;) {
            const int word = std::min(2 + static_cast<int>(nextRandom(state) % 9), target - used);
            for (int letter = 0; letter < word; ++letter)
                Ubiquitty::Unicode::appendUTF8(unit, alphabet[0] + nextRandom(state) % (alphabet[1] - alphabet[0] + 1));
            used += word + 1;
            unit += ' ';
        }
        unit += "\r\n";
    }
    return unit;
}

// Letters with a combining mark on about one in six, the way decomposed text
// carries them, in words, each line short of the grid.
std::string sparseMarks()
{
    std::uint64_t state = 0x5EED0002;
    std::string unit;
    for (int line = 0; line < 2048; ++line) {
        const int target = 30 + static_cast<int>(nextRandom(state) % (benchColumns - 32));
        for (int used = 0; used < target; ++used) {
            Ubiquitty::Unicode::appendUTF8(unit, nextRandom(state) % 2 ? 0xE0 + nextRandom(state) % 0x1F : 0x430 + nextRandom(state) % 0x20);
            if (nextRandom(state) % 6 == 0)
                Ubiquitty::Unicode::appendUTF8(unit, 0x300 + nextRandom(state) % 0x10);
            if (nextRandom(state) % 7 == 0) {
                unit += ' ';
                ++used;
            }
        }
        unit += "\r\n";
    }
    return unit;
}

std::vector<Corpus> buildCorpora(std::size_t targetBytes)
{
    std::vector<Corpus> corpora;

    // Single-byte, width 1: the layout's win case (half the bytes per cell to write).
    corpora.push_back({ "ascii",
        repeatToBytes("the quick brown fox jumps over the lazy dog 0123456789\n", targetBytes) });

    // Two-byte UTF-8, width 1 — no wide handling, no pen change per cell.
    corpora.push_back({ "latin1_multibyte",
        repeatToBytes("àéîõü ñçøåæ þðßµ¿¡ "
                      "àéîõü ñçøåæ þðßµ¿¡\n",
            targetBytes) });

    // Three-byte UTF-8, width 2: every glyph writes a lead cell and a
    // WideContinuation cell, so the pen alternates between two values per cell.
    corpora.push_back({ "cjk_wide",
        repeatToBytes("你好世界你好世界你好世界你好世界"
                      "你好世界你好世界你好世界你好世界\n",
            targetBytes) });

    // Plain emoji, width 2, no ZWJ/VS: every glyph routes through the cluster
    // lane to the per-glyph wide write (emitWideCharacter -> writeWideCell),
    // the path the batched cjk_wide corpus never takes. Sequential pairs on
    // scroll-recycled rows are the shape a full-width emoji stream feeds.
    corpora.push_back({ "emoji_wide",
        repeatToBytes("\xF0\x9F\x98\x80\xF0\x9F\x9A\x80\xF0\x9F\x94\xA5\xF0\x9F\x8E\xAF\xF0\x9F\x98\x80\xF0\x9F\x9A\x80\xF0\x9F\x94\xA5\xF0\x9F\x8E\xAF"
                      "\xF0\x9F\x98\x80\xF0\x9F\x9A\x80\xF0\x9F\x94\xA5\xF0\x9F\x8E\xAF\xF0\x9F\x98\x80\xF0\x9F\x9A\x80\xF0\x9F\x94\xA5\xF0\x9F\x8E\xAF\n",
            targetBytes) });

    // Base + combining mark per cell: exercises the Cell's inline combining slot.
    {
        std::string unit;
        for (int index = 0; index < 40; ++index) {
            unit += static_cast<char>('a' + (index % 26));
            unit += "́"; // COMBINING ACUTE ACCENT
        }
        unit += "\n";
        corpora.push_back({ "combining", repeatToBytes(unit, targetBytes) });
    }

    // A distinct truecolor pen per column: the only corpus here that makes the
    // pool mint entries, so it is the one that prices interning rather than
    // resolving. Columns repeat per line, so the pool converges rather than
    // running to its capacity limit.
    {
        std::string unit;
        for (int column = 0; column < benchColumns; ++column) {
            char escape[32];
            std::snprintf(escape, sizeof(escape), "\x1b[38;2;%d;%d;%dm#",
                column & 0xFF, (column * 7) & 0xFF, (column * 13) & 0xFF);
            unit += escape;
        }
        unit += "\n";
        corpora.push_back({ "sgr_truecolor", repeatToBytes(unit, targetBytes) });
    }

    // Four-byte UTF-8, width 1, from Symbols for Legacy Computing
    // (U+1FB00..U+1FB3A): the sextant range shares its F0 9F lead pair with
    // every emoji block, so this is the corpus that prices the cluster-route
    // peek on leads that can never cluster — the cost the other corpora
    // cannot see, because none of them carries a four-byte non-emoji glyph.
    // An SGR every fourth glyph is the shape a monitor's gradient bars feed:
    // each colour change closes the run, so the peek is paid per fragment,
    // not per row.
    {
        std::string unit;
        for (int column = 0; column < benchColumns; ++column) {
            if (column % 4 == 0) {
                char escape[16];
                std::snprintf(escape, sizeof(escape), "\x1b[38;5;%dm", 16 + (column % 216));
                unit += escape;
            }
            const uint32_t codePoint = 0x1FB00 + static_cast<uint32_t>(column % 0x3B);
            unit += static_cast<char>(0xF0 | (codePoint >> 18));
            unit += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
            unit += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            unit += static_cast<char>(0x80 | (codePoint & 0x3F));
        }
        unit += "\n";
        corpora.push_back({ "legacy_graphics", repeatToBytes(unit, targetBytes) });
    }

    // Prose outside ASCII: every multibyte run is one word between spaces, so a
    // decoder that pays off only on long runs never meets one here.
    corpora.push_back({ "multiscript_words", repeatToBytes(multiscriptWords(), targetBytes) });

    // Marks scattered through words: the batched writer hands each mark to the
    // cluster path and takes the row back after it, the hand-off a corpus with
    // a mark on every cell or on none never makes.
    corpora.push_back({ "combining_sparse", repeatToBytes(sparseMarks(), targetBytes) });

    return corpora;
}

// Best-of-N, because the floor is the only figure attributable to the parser on
// a machine with other tenants: a slow pass measures the scheduler, never a
// faster parser.
double bestMegabytesPerSecond(const std::string& bytes, int repetitions)
{
    Ubiquitty::Terminal term;
    Ubiquitty::hardReset(term);
    Ubiquitty::resize(term, benchRows, benchColumns);
    // One untimed pass so first-touch faults and table init stay out of the sample.
    Ubiquitty::processBytes(&term, bytes);

    double bestSeconds = 1e30;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        const auto start = ClockType::now();
        Ubiquitty::processBytes(&term, bytes);
        const double seconds = std::chrono::duration<double>(ClockType::now() - start).count();
        if (seconds < bestSeconds)
            bestSeconds = seconds;
    }
    return static_cast<double>(bytes.size()) / bestSeconds / 1e6;
}

} // namespace

int main(int argc, char** argv)
{
#if defined(__APPLE__)
    // Ask for the performance cores. A bench thread left at the default QoS is
    // eligible for the efficiency cluster, and a run that lands there reports a
    // lower throughput than the same binary's on a performance core. The two
    // clusters' runs then differ by more than the changes under measurement do.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    // --stream <file>: adds one corpus holding the file's bytes grown to the
    // standard size — the fast-iteration lane for
    // measuring a real captured stream (vtebench et al.) through the same
    // harness, without an app or a PTY in the loop.
    // UBQ_BENCH_STREAM names a captured byte stream to measure ALONGSIDE the
    // built-in corpora, as one more lane. Alongside and not instead: a single
    // lane per process gives a control-normalised A/B nothing to normalise
    // against, and raw ratios on a developer machine carry drift that the
    // built-in lanes cancel when they ride the same run.
    const char* streamPath = std::getenv("UBQ_BENCH_STREAM");
    int repetitions = 7;
    const char* loopCorpus = nullptr;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--stream") == 0 && index + 1 < argc)
            streamPath = argv[++index];
        else if (std::strcmp(argv[index], "--loop") == 0 && index + 1 < argc)
            loopCorpus = argv[++index];
        else if (std::strcmp(argv[index], "--repetitions") == 0 && index + 1 < argc)
            repetitions = std::atoi(argv[++index]);
        else {
            std::fprintf(stderr, "usage: bench_ingest_corpora [--repetitions N] [--loop <corpus>]\n");
            return 2;
        }
    }
    if (repetitions < 1)
        repetitions = 1;

    std::vector<Corpus> corpora = buildCorpora(8u * 1024u * 1024u);
    if (streamPath) {
        std::FILE* file = std::fopen(streamPath, "rb");
        if (!file) {
            std::fprintf(stderr, "bench_ingest_corpora: cannot open %s\n", streamPath);
            return 2;
        }
        std::string bytes;
        char chunk[65536];
        std::size_t got;
        while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0)
            bytes.append(chunk, got);
        std::fclose(file);
        corpora.push_back({ "stream", repeatToBytes(bytes, 8u * 1024u * 1024u) });
    }

    // --loop parses one corpus forever through the same call, so an external
    // sampler has a steady state to attribute. At -O3 with ThinLTO the
    // emit path folds into processBytesImpl and a sampler stops there; the pool
    // counters below are what actually separate the costs.
    if (loopCorpus) {
        for (const Corpus& corpus : corpora) {
            if (std::strcmp(corpus.name, loopCorpus) != 0)
                continue;
            Ubiquitty::Terminal term;
            Ubiquitty::hardReset(term);
            Ubiquitty::resize(term, benchRows, benchColumns);
            for (;;)
                Ubiquitty::processBytes(&term, corpus.bytes);
        }
        std::fprintf(stderr, "bench_ingest_corpora: no corpus named '%s'\n", loopCorpus);
        return 2;
    }

    // stdout carries the machine-readable document in the schema bench-ab.py
    // reads, so this bench can sit as either arm of a control-normalised A/B;
    // the human table rides stderr, where a pipe cannot swallow it into a
    // parser and a redirect cannot turn the result channel into prose.
    std::fprintf(stderr, "cell=%zuB repetitions=%d grid=%dx%d\n", sizeof(Ubiquitty::Cell), repetitions, benchColumns, benchRows);
    std::fprintf(stderr, "%-18s %12s\n", "corpus", "MB/s");
    std::printf(R"({"benchmarks":[)");
    bool first = true;
    for (const Corpus& corpus : corpora) {
        const double megabytesPerSecond = bestMegabytesPerSecond(corpus.bytes, repetitions);
        std::fprintf(stderr, "%-18s %12.1f\n", corpus.name, megabytesPerSecond);
        std::printf(R"(%s{"name":"%s","value":%.3f,"unit":"MB/s"})", first ? "" : ",", corpus.name, megabytesPerSecond);
        first = false;
    }
    std::printf("]}\n");
    return 0;
}
