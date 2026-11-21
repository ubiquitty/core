// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_scroll_lane_differential.cpp — the differential harness gating the
// straight-to-scrollback bulk lane.
//
// Invariant under test: observable terminal state is a function of the BYTE
// STREAM, not of its chunking. Two terminals fed identical bytes through
// different processBytes splits must hold byte-identical visible grid,
// scrollback content + metadata (wrap joins, marks, line attributes),
// cursor, wrapPending, and scrollbackCount at every aligned checkpoint.
// That is exactly the boundary the bulk lane's per-chunk amortization must
// preserve (it flushes at every processBytes return and at every non-lane
// dispatch), so this harness is the lane's gate: it must stay green when
// the lane builds run through it, against classic builds and against
// differently-chunked lane builds.
//
// The lane and the ordinary path must agree at every aligned checkpoint,
// however the input is chunked:
//
//   Visible grid and scrollback     same cells, order, wrap joins, marks
//                                   and line attributes
//   Cursor and pending wrap         same position and wrap state
//   Margins and alternate screen    only a path eligible for the active
//                                   geometry runs
//   Image and rich-text placement   same scroll displacement and history
//                                   association
//   Terminal replies                observe fully committed state
//   Synchronized-output boundaries  publish a coherent frame
//   Triggers and auto-print         keep every required per-line
//                                   observation
//
// The comparison is of terminal semantics, not raw storage bytes: a
// deferred blank can legitimately have a different storage representation.
//
// The corpus: one directed segment stream per invalidation-matrix row
// (margins/alt-screen, soft-wrap, DECDWL, marks, DSR mid-flood, cursor
// motion, erases, BSU/ESU) plus seeded random floods. Checkpoints fall at
// segment ends — chunkings differ INSIDE a segment, align at its end.
//
//   c++ -std=c++23 -O2 -ISource Tests/Core/test_scroll_lane_differential.cpp
//       <core objects...> -o /tmp/slane && /tmp/slane
#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/TerminalOperations.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

static int failures = 0;

static uint64_t g = 0x6A09E667F3BCC909ull;
static uint32_t rnd()
{
    g ^= g << 13;
    g ^= g >> 7;
    g ^= g << 17;
    return static_cast<uint32_t>(g >> 32);
}

// FNV-1a over every observable surface. Visible grid via visibleCell (the
// deferred-blank slot makes RAW slab bytes legitimately diverge while the
// visible cell is identical — the harness hashes semantics, not storage).
// Cells are mixed FIELD-WISE in canonical order rather than as raw struct
// bytes, so the fingerprint is independent of Cell's in-memory layout and stays
// comparable across two builds whose Cell differs — which is what makes
// --fingerprint usable to diff one binary against another.
static uint64_t fingerprint(const Ubiquitty::Terminal& t)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    auto mixInt = [&](long long v) { mix(&v, sizeof(v)); };
    auto mixCell = [&](const Ubiquitty::Cell& cell) {
        mixInt(cell.codePoint);
        // Pen is zero-padding-free, so a raw byte mix of it is canonical and
        // does not depend on where the Cell stores it.
        mix(&Ubiquitty::cellPen(cell), sizeof(Ubiquitty::Pen));
        for (uint32_t continuation : cell.combining)
            mixInt(continuation);
        mixInt(cell.linkID());
        mixInt(cell.clusterPacked);
    };

    const Ubiquitty::Screen::State* s = t.screens.active;
    mixInt(t.rows);
    mixInt(t.columns);
    mixInt(s->cursorX);
    mixInt(s->cursorY);
    mixInt(s->wrapPending ? 1 : 0);
    mixInt(s->scrollTop);
    mixInt(s->scrollBottom);
    mixInt(t.screens.active == &t.screens.alternate ? 1 : 0);
    mixInt(t.scrollbackCount);

    static Ubiquitty::Cell blank; // zero cell for null resolutions
    for (int r = 0; r < t.rows; ++r) {
        for (int c = 0; c < t.columns; ++c) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, r, c);
            mixCell(cell ? *cell : blank);
        }
        mixInt(s->lineWrapped[r] ? 1 : 0);
        mixInt(s->lineAttribute[r]);
        mixInt(s->mark[r]);
    }
    // Image placements: the deferred-state lane batches scrollPlacementsUp
    // per run while the classic path shifts per line — anchors and drops
    // must land byte-identically (security-gate coverage).
    mixInt(static_cast<long long>(t.images.livePlacementCount));
    for (const Ubiquitty::Image::TerminalImagePlacement& placement : t.images.placements) {
        if (!placement.used)
            continue;
        mixInt(placement.imageID);
        mixInt(placement.placementID);
        mixInt(placement.cursorX);
        mixInt(placement.cursorY);
        mixInt(placement.cellColumns);
        mixInt(placement.cellRows);
        mixInt(placement.zIndex);
    }
    for (int i = 0; i < t.scrollbackCount; ++i) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (!line) {
            mixInt(-1);
            continue;
        }
        mixInt(line->length);
        mixInt(line->wrapped ? 1 : 0);
        mixInt(line->lineAttribute);
        mixInt(line->mark);
        for (int cellIndex = 0; cellIndex < line->length; ++cellIndex)
            mixCell(line->cells[cellIndex]);
    }
    return h;
}

// One replay arm: a terminal fed the segment corpus under a chunking policy,
// with the deferred-state scroll lane on or off. Terminals live on the heap:
// nine grids of arm state would blow the stack.
struct Arm {
    std::unique_ptr<Ubiquitty::Terminal> terminal;
    // 0 = whole segment in one call; k > 0 = fixed k-byte chunks;
    // -1 = random 1..97-byte chunks from a per-arm deterministic stream.
    int chunkPolicy;
    uint64_t chunkSeed;
    bool isLaneEnabled;

    void reset()
    {
        terminal = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::hardReset(*terminal);
        terminal->isScrollLaneEnabled = isLaneEnabled;
    }

    void feed(const std::string& segment)
    {
        const char* data = segment.data();
        int remaining = static_cast<int>(segment.size());
        if (chunkPolicy == 0) {
            Ubiquitty::processBytes(terminal.get(), std::string_view(data, static_cast<std::size_t>(remaining)));
            return;
        }
        uint64_t localSeed = chunkSeed;
        auto nextRandom = [&localSeed]() {
            localSeed ^= localSeed << 13;
            localSeed ^= localSeed >> 7;
            localSeed ^= localSeed << 17;
            return static_cast<int>((localSeed >> 33) % 97) + 1;
        };
        while (remaining > 0) {
            int n = (chunkPolicy > 0) ? chunkPolicy : nextRandom();
            if (n > remaining)
                n = remaining;
            Ubiquitty::processBytes(terminal.get(), std::string_view(data, static_cast<std::size_t>(n)));
            data += n;
            remaining -= n;
        }
    }
};

// On divergence, name the exact component so a failure is diagnosable from
// the log alone: first differing grid cell (raw bytes), row meta, scrollback
// line, or cursor/mode scalar.
static void reportDivergence(const char* name, int checkpoint, size_t armIndex, const Ubiquitty::Terminal& reference, const Ubiquitty::Terminal& arm)
{
    const Ubiquitty::Screen::State* rs = reference.screens.active;
    const Ubiquitty::Screen::State* as = arm.screens.active;
    if (rs->cursorX != as->cursorX || rs->cursorY != as->cursorY || rs->wrapPending != as->wrapPending)
        std::printf("  %s#%d arm%zu cursor: ref(%d,%d,w%d) arm(%d,%d,w%d)\n", name, checkpoint, armIndex,
            rs->cursorX, rs->cursorY, rs->wrapPending ? 1 : 0, as->cursorX, as->cursorY, as->wrapPending ? 1 : 0);
    if (reference.scrollbackCount != arm.scrollbackCount)
        std::printf("  %s#%d arm%zu scrollbackCount: ref %d arm %d\n", name, checkpoint, armIndex,
            reference.scrollbackCount, arm.scrollbackCount);
    int reported = 0;
    for (int r = 0; r < reference.rows && reported < 3; ++r) {
        for (int c = 0; c < reference.columns && reported < 3; ++c) {
            static Ubiquitty::Cell zero;
            const Ubiquitty::Cell* rc = Ubiquitty::Screen::visibleCell(&reference, 0, r, c);
            const Ubiquitty::Cell* ac = Ubiquitty::Screen::visibleCell(&arm, 0, r, c);
            if (std::memcmp(rc ? rc : &zero, ac ? ac : &zero, sizeof(Ubiquitty::Cell)) != 0) {
                const uint8_t* rb = reinterpret_cast<const uint8_t*>(rc ? rc : &zero);
                const uint8_t* ab = reinterpret_cast<const uint8_t*>(ac ? ac : &zero);
                std::printf("  %s#%d arm%zu grid(%d,%d): ref[", name, checkpoint, armIndex, r, c);
                for (size_t i = 0; i < sizeof(Ubiquitty::Cell); ++i)
                    std::printf("%02x", rb[i]);
                std::printf("] arm[");
                for (size_t i = 0; i < sizeof(Ubiquitty::Cell); ++i)
                    std::printf("%02x", ab[i]);
                std::printf("]\n");
                ++reported;
            }
        }
        if (rs->lineWrapped[r] != as->lineWrapped[r] || rs->lineAttribute[r] != as->lineAttribute[r] || rs->mark[r] != as->mark[r])
            std::printf("  %s#%d arm%zu rowmeta(%d): ref(w%d,a%d,m%d) arm(w%d,a%d,m%d)\n", name, checkpoint, armIndex, r,
                rs->lineWrapped[r] ? 1 : 0, rs->lineAttribute[r], rs->mark[r],
                as->lineWrapped[r] ? 1 : 0, as->lineAttribute[r], as->mark[r]);
    }
    reported = 0;
    const int commonCount = reference.scrollbackCount < arm.scrollbackCount ? reference.scrollbackCount : arm.scrollbackCount;
    for (int i = 0; i < commonCount && reported < 3; ++i) {
        const Ubiquitty::Screen::ScrollLine* rl = Ubiquitty::Screen::scrollbackLine(&reference, i);
        const Ubiquitty::Screen::ScrollLine* al = Ubiquitty::Screen::scrollbackLine(&arm, i);
        if (!rl || !al)
            continue;
        const bool hasMetaDifference = rl->length != al->length || rl->wrapped != al->wrapped || rl->lineAttribute != al->lineAttribute || rl->mark != al->mark;
        const bool hasCellDifference = !hasMetaDifference && rl->length == al->length && std::memcmp(rl->cells, al->cells, static_cast<size_t>(rl->length) * sizeof(Ubiquitty::Cell)) != 0;
        if (hasMetaDifference || hasCellDifference) {
            std::printf("  %s#%d arm%zu scrollback[%d]: %s ref(len%d,w%d,a%d,m%d) arm(len%d,w%d,a%d,m%d)\n", name, checkpoint, armIndex, i,
                hasMetaDifference ? "meta" : "cells",
                rl->length, rl->wrapped ? 1 : 0, rl->lineAttribute, rl->mark,
                al->length, al->wrapped ? 1 : 0, al->lineAttribute, al->mark);
            if (reported == 0) { // full cell dump for the first divergent line
                for (int side = 0; side < 2; ++side) {
                    const Ubiquitty::Screen::ScrollLine* line = side ? al : rl;
                    std::printf("    %s:", side ? "arm" : "ref");
                    for (int cellIndex = 0; cellIndex < line->length; ++cellIndex) {
                        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&line->cells[cellIndex]);
                        std::printf(" [");
                        for (size_t byteIndex = 0; byteIndex < sizeof(Ubiquitty::Cell); ++byteIndex)
                            std::printf("%02x", bytes[byteIndex]);
                        std::printf("]");
                    }
                    std::printf("\n");
                }
            }
            ++reported;
        }
    }
}

// --fingerprint: also print each corpus's final reference fingerprint as a
// stable "FP <corpus> <hex>" line. Two builds that agree on observable cell
// state must print identical lines — the cross-binary half of the differential;
// chunk-invariance above is the in-binary half.
static bool isFingerprintPrintEnabled = false;

static void runCorpus(const char* name, const std::vector<std::string>& segments)
{
    Arm arms[] = {
        { nullptr, 0, 0, false }, // whole segments, lane off — the reference
        { nullptr, 1, 0, false }, // pathological per-byte
        { nullptr, 7, 0, false }, // prime misalignment
        { nullptr, 1024, 0, false }, // the PTY quantum
        { nullptr, -1, 0x9E3779B97F4A7C15ull, false }, // random splits
        // Lane-ON arms: the deferred-state lane must be byte-identical to the
        // classic path across every chunking (its per-run flush points are
        // exactly what these splits exercise).
        { nullptr, 0, 0, true },
        { nullptr, 1, 0, true },
        { nullptr, 1024, 0, true },
        { nullptr, -1, 0x9E3779B97F4A7C15ull, true },
    };
    for (Arm& arm : arms)
        arm.reset();

    int checkpoint = 0;
    for (const std::string& segment : segments) {
        ++checkpoint;
        for (Arm& arm : arms)
            arm.feed(segment);
        const uint64_t reference = fingerprint(*arms[0].terminal.get());
        for (size_t a = 1; a < sizeof(arms) / sizeof(arms[0]); ++a) {
            const uint64_t got = fingerprint(*arms[a].terminal.get());
            if (got != reference) {
                std::printf("FAIL %s: checkpoint %d arm %zu (policy %d): %016llx != %016llx\n",
                    name, checkpoint, a, arms[a].chunkPolicy,
                    static_cast<unsigned long long>(got), static_cast<unsigned long long>(reference));
                reportDivergence(name, checkpoint, a, *arms[0].terminal.get(), *arms[a].terminal.get());
                ++failures;
            }
        }
    }
    if (isFingerprintPrintEnabled)
        std::printf("FP %s %016llx\n", name, static_cast<unsigned long long>(fingerprint(*arms[0].terminal.get())));
}

// ---- corpus builders -------------------------------------------------------

static std::string floodLines(int count, int width, uint32_t colorSeed)
{
    std::string out;
    for (int i = 0; i < count; ++i) {
        if (colorSeed)
            out += "\x1b[38;5;" + std::to_string((colorSeed + i) % 256) + "m";
        for (int c = 0; c < width; ++c)
            out.push_back(static_cast<char>('a' + ((i + c) % 26)));
        out += "\r\n";
    }
    return out;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--fingerprint") == 0)
            isFingerprintPrintEnabled = true;
    }
    // 1. Plain short-line flood — the lane's primary workload, deep enough to
    // cycle scrollback eviction at the default capacity.
    {
        std::vector<std::string> segments;
        for (int k = 0; k < 6; ++k)
            segments.push_back(floodLines(400, 1 + (k % 3), 0));
        runCorpus("plain-flood", segments);
    }

    // 2. Colored flood + mid-stream DSR (forces mid-chunk flush + a reply),
    // cursor motion, and erases — matrix rows: DSR, cursor/erase dispatch.
    {
        std::vector<std::string> segments;
        segments.push_back(floodLines(80, 4, 17));
        segments.emplace_back("\x1b[6n"); // CPR mid-flood
        segments.push_back(floodLines(40, 2, 91));
        segments.emplace_back("\x1b[5;10H\x1b[2Kmoved\x1b[999;1H"); // CUP + EL + back to bottom
        segments.push_back(floodLines(80, 3, 7));
        runCorpus("dsr-cursor-erase", segments);
    }

    // 3. Soft-wrap: lines longer than the grid autowrap-commit; a chunk split
    // inside the wrap point must not change the join. Matrix row: wrap carry.
    {
        std::vector<std::string> segments;
        std::string longLines;
        for (int i = 0; i < 60; ++i) {
            for (int c = 0; c < 200; ++c) // 200 > default 80 columns: wraps
                longLines.push_back(static_cast<char>('A' + ((i + c) % 26)));
            longLines += "\r\n";
        }
        segments.push_back(longLines);
        segments.push_back(floodLines(60, 2, 0));
        runCorpus("soft-wrap", segments);
    }

    // 4. Margins + regions + alt screen round-trip — matrix row: eligibility
    // transitions. DECSTBM, DECSLRM under DECLRMM, alt-screen enter/exit.
    {
        std::vector<std::string> segments;
        segments.push_back(floodLines(50, 3, 0));
        segments.emplace_back("\x1b[5;20r"); // DECSTBM region
        segments.push_back(floodLines(40, 3, 5));
        segments.emplace_back("\x1b[r"); // reset region
        segments.emplace_back("\x1b[?69h\x1b[10;60s"); // DECLRMM + DECSLRM
        segments.push_back(floodLines(30, 3, 9));
        segments.emplace_back("\x1b[?69l");
        segments.emplace_back("\x1b[?1049h"); // alt screen
        segments.push_back(floodLines(30, 3, 3));
        segments.emplace_back("\x1b[?1049l"); // back to primary
        segments.push_back(floodLines(40, 3, 0));
        runCorpus("margins-altscreen", segments);
    }

    // 5. DECDWL/DECDHL + OSC 133 marks riding into scrollback — matrix rows:
    // line attributes, marks.
    {
        std::vector<std::string> segments;
        segments.emplace_back("\x1b]133;A\x1b\\prompt$ cmd\r\n"); // marked prompt row
        segments.emplace_back("\x1b#6double width line\r\n"); // DECDWL
        segments.push_back(floodLines(50, 4, 0));
        segments.emplace_back("\x1b]133;A\x1b\\next-prompt\r\n");
        segments.push_back(floodLines(50, 2, 0));
        runCorpus("marks-dwl", segments);
    }

    // 6. BSU/ESU synchronized-output brackets around flood bursts — matrix
    // row: sync seam (processBytesUpToSyncClear stops at the clear).
    {
        std::vector<std::string> segments;
        segments.push_back("\x1b[?2026h" + floodLines(60, 3, 21) + "\x1b[?2026l");
        segments.push_back(floodLines(30, 2, 0));
        segments.push_back("\x1b[?2026h" + floodLines(60, 1, 33) + "\x1b[?2026l");
        runCorpus("bsu-esu", segments);
    }

    // 7. Blank-line floods: bare LFs scroll DEFERRED-BLANK rows out of the
    // grid (no write ever materializes them), interleaved with sparse text —
    // the deferred-slot/capture interaction under every chunking.
    {
        std::vector<std::string> segments;
        segments.emplace_back(120, '\n');
        segments.push_back(floodLines(30, 2, 9));
        std::string sparse;
        for (int i = 0; i < 60; ++i)
            sparse += (i % 3 == 0) ? "x\n" : "\n";
        segments.push_back(sparse);
        segments.push_back(floodLines(30, 1, 0));
        runCorpus("blank-lines", segments);
    }

    // 8. Kitty placements riding a scroll flood: transmit an explicit-id
    // placement, flood it up through the grid and past the top; transmit a
    // second one MID-flood (the APC closes any lane run); then flood far
    // enough to cross the drop threshold. Pins batched-vs-per-line
    // scrollPlacementsUp equivalence — anchors and drops byte-identical
    // under every chunking and lane arm (security-gate directed case).
    {
        // 2x2 RGB (f=24), 12 payload bytes, explicit image id.
        const char* transmitFirst = "\x1b_Ga=T,f=24,s=2,v=2,i=7;AAAAAAAAAAAAAAAA\x1b\\";
        const char* transmitSecond = "\x1b_Ga=T,f=24,s=2,v=2,i=9;AAAAAAAAAAAAAAAA\x1b\\";
        std::vector<std::string> segments;
        segments.push_back(std::string(transmitFirst) + floodLines(10, 3, 0));
        segments.push_back(floodLines(30, 2, 5)); // first placement scrolls off-grid (negative anchor)
        segments.push_back(std::string(transmitSecond) + floodLines(8, 2, 0));
        segments.push_back(floodLines(60, 1, 0)); // both ride deep negative anchors
        runCorpus("kitty-scroll", segments);

        // Non-vacuousness: the corpus must actually place images and scroll
        // them negative, or the equivalence above tested nothing.
        {
            auto probe = std::make_unique<Ubiquitty::Terminal>();
            Ubiquitty::hardReset(*probe);
            probe->isScrollLaneEnabled = true;
            for (const std::string& segment : segments)
                Ubiquitty::processBytes(probe.get(), segment);
            int usedPlacements = 0;
            bool hasNegativeAnchor = false;
            for (const Ubiquitty::Image::TerminalImagePlacement& placement : probe->images.placements) {
                if (!placement.used)
                    continue;
                ++usedPlacements;
                if (placement.cursorY < 0)
                    hasNegativeAnchor = true;
            }
            if (usedPlacements < 2 || !hasNegativeAnchor || probe->images.livePlacementCount != static_cast<uint32_t>(usedPlacements)) {
                std::printf("FAIL kitty-scroll vacuous: used=%d negativeAnchor=%d liveCount=%u\n",
                    usedPlacements, hasNegativeAnchor ? 1 : 0, probe->images.livePlacementCount);
                ++failures;
            }
        }
    }

    // 9. Seeded random soup: printables, CR/LF, SGRs, cursor moves, erases —
    // the fuzz arm. Segment boundaries land at random positions.
    {
        std::vector<std::string> segments;
        for (int seg = 0; seg < 8; ++seg) {
            std::string chunk;
            const int ops = 400 + static_cast<int>(rnd() % 400);
            for (int i = 0; i < ops; ++i) {
                switch (rnd() % 10) {
                case 0:
                    chunk += "\r\n";
                    break;
                case 1:
                    chunk += "\x1b[" + std::to_string(30 + rnd() % 8) + "m";
                    break;
                case 2:
                    chunk += "\x1b[" + std::to_string(1 + rnd() % 20) + ";" + std::to_string(1 + rnd() % 70) + "H";
                    break;
                case 3:
                    chunk += (rnd() & 1) ? "\x1b[K" : "\x1b[2J";
                    break;
                case 4:
                    chunk += "\x1b[38;5;" + std::to_string(rnd() % 256) + "m";
                    break;
                default:
                    for (int k = 0; k < static_cast<int>(rnd() % 12) + 1; ++k)
                        chunk.push_back(static_cast<char>(0x20 + rnd() % 0x5F));
                    break;
                }
            }
            segments.push_back(chunk);
        }
        runCorpus("random-soup", segments);
    }

    // ---- pen-diverse corpora ------------------------------------------------
    // Directed at pen-state fidelity: every SGR family the Pen packs, per-cell
    // distinct pens, in-place rectangle attribute rewrites, links over styled
    // clusters, and BCE blanks. These pin the canonical cell fingerprint against
    // any future change to how a Cell carries its pen.

    // 10. Full SGR churn: one line per family — weights/styles, underline
    // shapes + colors, blink/inverse/invisible/strike, 16/256/truecolor fg+bg,
    // ideogram, super/subscript, fonts, proportional, framed — then partial
    // resets, so intermediate pens (not just the final one) land in cells and
    // scroll into history.
    {
        static const char* const sgrPrefixes[] = {
            "\x1b[1m",
            "\x1b[2m",
            "\x1b[3m",
            "\x1b[9m",
            "\x1b[1;3m",
            "\x1b[4m",
            "\x1b[4:1m",
            "\x1b[4:2m",
            "\x1b[4:3m",
            "\x1b[4:4m",
            "\x1b[4:5m",
            "\x1b[58;5;99m",
            "\x1b[58;2;10;20;30m",
            "\x1b[59m",
            "\x1b[5m",
            "\x1b[7m",
            "\x1b[8m",
            "\x1b[28m",
            "\x1b[31m",
            "\x1b[97m",
            "\x1b[45m",
            "\x1b[106m",
            "\x1b[39m",
            "\x1b[49m",
            "\x1b[38;5;123m",
            "\x1b[48;5;231m",
            "\x1b[38;2;1;2;3m",
            "\x1b[48;2;250;128;7m",
            "\x1b[60m",
            "\x1b[61m",
            "\x1b[62m",
            "\x1b[63m",
            "\x1b[64m",
            "\x1b[65m",
            "\x1b[73m",
            "\x1b[74m",
            "\x1b[75m",
            "\x1b[11m",
            "\x1b[19m",
            "\x1b[20m",
            "\x1b[10m",
            "\x1b[26m",
            "\x1b[50m",
            "\x1b[51m",
            "\x1b[54m",
            "\x1b[22m",
            "\x1b[23m",
            "\x1b[24m",
            "\x1b[25m",
            "\x1b[27m",
            "\x1b[29m",
            "\x1b[0m",
        };
        std::vector<std::string> segments;
        std::string wave;
        for (const char* prefix : sgrPrefixes)
            wave += std::string(prefix) + "styled-run-text\r\n";
        segments.push_back(wave);
        segments.push_back(floodLines(30, 3, 41)); // scroll the styled block into history
        segments.push_back(wave); // repeat: identical pens must re-produce identical cells
        runCorpus("sgr-churn-full", segments);
    }

    // 11. Truecolor gradient: a DISTINCT pen on every cell — the worst case
    // for any pen-dedup scheme and the densest pen-fidelity surface — then
    // scrolled fully into scrollback so history carries the gradient too.
    std::vector<std::string> gradientSegments;
    {
        auto gradientRows = [](int rowCount, int rowOffset) {
            std::string out;
            for (int r = 0; r < rowCount; ++r) {
                for (int c = 0; c < 72; ++c) {
                    const int red = (rowOffset + r * 3 + c * 5) % 256;
                    const int green = (rowOffset + r * 7 + c * 11) % 256;
                    const int blue = (rowOffset + r * 13 + c * 2) % 256;
                    out += "\x1b[38;2;" + std::to_string(red) + ";" + std::to_string(green) + ";" + std::to_string(blue) + "m";
                    out += "\x1b[48;2;" + std::to_string(blue) + ";" + std::to_string(red) + ";" + std::to_string(green) + "m";
                    out.push_back(static_cast<char>('A' + (r + c) % 26));
                }
                out += "\x1b[0m\r\n";
            }
            return out;
        };
        gradientSegments.push_back(gradientRows(30, 0));
        gradientSegments.push_back(gradientRows(30, 97));
        gradientSegments.push_back(floodLines(5, 2, 0));
        runCorpus("truecolor-gradient", gradientSegments);

        // Non-vacuousness: the final state must actually hold a dense field of
        // distinct pens (grid + scrollback), or the corpus tested nothing.
        {
            auto probe = std::make_unique<Ubiquitty::Terminal>();
            Ubiquitty::hardReset(*probe);
            for (const std::string& segment : gradientSegments)
                Ubiquitty::processBytes(probe.get(), segment);
            std::vector<uint64_t> penHashes;
            auto penHash = [](const Ubiquitty::Pen& pen) {
                uint64_t h = 1469598103934665603ull;
                const uint8_t* b = reinterpret_cast<const uint8_t*>(&pen);
                for (size_t i = 0; i < sizeof(Ubiquitty::Pen); ++i) {
                    h ^= b[i];
                    h *= 1099511628211ull;
                }
                return h;
            };
            for (int i = 0; i < probe->scrollbackCount; ++i) {
                const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(probe.get(), i);
                for (int cellIndex = 0; line && cellIndex < line->length; ++cellIndex)
                    penHashes.push_back(penHash(Ubiquitty::cellPen(line->cells[cellIndex])));
            }
            std::ranges::sort(penHashes);
            const size_t distinctPens = static_cast<size_t>(std::ranges::unique(penHashes).begin() - penHashes.begin());
            if (distinctPens < 1000) {
                std::printf("FAIL truecolor-gradient vacuous: %zu distinct pens in scrollback\n", distinctPens);
                ++failures;
            }
        }
    }

    // 12. DECCARA/DECRARA rectangles: in-place attribute rewrites of already
    // written cells — the read-modify-write pen path. DECSACE toggles the
    // stream/rectangle interpretation mid-corpus; rectangles overlap.
    {
        std::vector<std::string> segments;
        segments.push_back(floodLines(20, 60, 33)); // a styled field to rewrite
        segments.emplace_back("\x1b[3;5;10;50;1;4$r"); // DECCARA: bold+underline the rect
        segments.emplace_back("\x1b[5;20;15;60;7$t"); // DECRARA: toggle reverse, overlapping
        segments.emplace_back("\x1b[2*x"); // DECSACE rectangle mode
        segments.emplace_back("\x1b[6;10;12;40;5$r"); // DECCARA under rectangle SACE
        segments.emplace_back("\x1b[1*x"); // back to stream
        segments.emplace_back("\x1b[8;1;9;80;0$r"); // DECCARA all-off across full rows
        segments.push_back(floodLines(40, 4, 7)); // scroll rewritten rows into history
        runCorpus("deccara-rectangles", segments);
    }

    // 13. Links over styled clusters: OSC 8 ids spanning SGR churn, combining
    // marks, VS-16 emoji presentation, and wide CJK — linkID and clusterPacked
    // must ride pen changes and cluster spills unchanged.
    {
        std::vector<std::string> segments;
        std::string linked;
        for (int i = 0; i < 24; ++i) {
            linked += "\x1b]8;id=k" + std::to_string(i) + ";https://example.test/" + std::to_string(i) + "\x1b\\";
            linked += "\x1b[3" + std::to_string(1 + i % 7) + "m";
            linked += "plain e\xCC\x81 \xE2\x98\x82\xEF\xB8\x8F \xE4\xB8\xAD\xE6\x96\x87 ";
            linked += "\x1b]8;;\x1b\\unlinked\r\n";
        }
        segments.push_back(linked);
        segments.push_back(floodLines(30, 2, 3)); // linked rows into history
        segments.push_back(linked); // same ids re-opened
        runCorpus("links-and-clusters", segments);
    }

    // 14. BCE blank probes: colored-background erases, fills, and line
    // insert/delete produce blanks whose PEN is not the default — every
    // blank-materialization path must stamp the same colored blank.
    {
        std::vector<std::string> segments;
        segments.push_back(floodLines(20, 40, 11));
        segments.emplace_back("\x1b[44m\x1b[2J\x1b[H"); // blue-bg full clear
        segments.push_back(floodLines(6, 30, 0));
        segments.emplace_back("\x1b[48;5;196m\x1b[3;1H\x1b[2K"); // 256-color EL on a row
        segments.emplace_back("\x1b[48;2;9;99;199m\x1b[5L\x1b[3M"); // truecolor IL/DL blanks
        segments.emplace_back("\x1b[4;10;8;40$z"); // DECERA rectangle erase
        segments.emplace_back("\x1b[63;2;5;6;70$x"); // DECFRA fill with '?'
        segments.push_back("\x1b[49m" + floodLines(40, 3, 0)); // colored blanks into history
        runCorpus("bce-blank-probes", segments);
    }

    // ---- text-flow corpora --------------------------------------------------
    // Text is runs of printable ASCII with line feeds, returns, tabs and wraps
    // between them, and the parser carries one run's eligibility on through
    // those. The per-byte arm never forms a run, so it is the baseline every
    // longer chunking has to match; the lane arms hold the alternate screen's
    // deferred scroll to its immediate one.

    // 15. Staircase text: a bare line feed leaves the column where it is, so
    // every row fills and wraps. On the primary screen and the alternate one,
    // in the default pen and in one that is not.
    const auto staircase = [](int bytes) {
        static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ \n\t ";
        std::string out;
        for (int i = 0; i < bytes; ++i)
            out.push_back(alphabet[rnd() % (sizeof(alphabet) - 1)]);
        return out;
    };
    {
        std::vector<std::string> segments;
        segments.push_back(staircase(20000));
        segments.emplace_back("\x1b[?1049h\x1b[H\x1b[2J");
        segments.push_back(staircase(20000));
        segments.emplace_back("\x1b[31m");
        segments.push_back(staircase(6000));
        segments.emplace_back("\x1b[m");
        segments.push_back(staircase(3000));
        segments.emplace_back("\x1b[?1049l");
        segments.push_back(staircase(8000));
        runCorpus("text-flow-staircase", segments);
    }

    // 16. What bends a flow or ends it, each with text flowing through it:
    // autowrap off, new-line mode, insert mode, margins, a double-width row in
    // the way, cleared and reset tab stops, the more(1) fix, a row of wide
    // glyphs under the text, and a line home that is not the first column.
    {
        std::vector<std::string> segments;
        segments.push_back("\x1b[?7l" + staircase(3000) + "\x1b[?7h");
        segments.push_back("\x1b[20h" + staircase(3000) + "\x1b[20l");
        segments.push_back("\x1b[4h" + staircase(2000) + "\x1b[4l");
        segments.push_back("\x1b[?69h\x1b[5;60s" + staircase(3000) + "\x1b[?69l");
        segments.push_back("\x1b[H\x1b[2J\x1b[6;1H\x1b#6\x1b[9;1H\x1b#3\x1b[1;1H" + staircase(1500));
        segments.push_back("\x1b[3g" + staircase(2000));
        segments.push_back("\x1b[1;5H\x1bH\x1b[1;37H\x1bH\x1b[1;79H\x1bH\x1b[H" + staircase(3000));
        segments.push_back("\x1b[?41h" + staircase(3000) + "\x1b[?41l");
        {
            std::string wideUnderText = "\x1b[H\x1b[2J";
            for (int row = 0; row < 30; ++row) {
                for (int glyph = 0; glyph < 40; ++glyph)
                    wideUnderText += "\xE4\xB8\xAD";
                wideUnderText += "\r";
                wideUnderText += std::string(static_cast<std::size_t>(3 + row * 2), static_cast<char>('a' + row % 26));
                wideUnderText += "\tmid\n";
            }
            segments.push_back(wideUnderText);
        }
        segments.push_back("\x1b[12 U" + staircase(3000) + "\x1b[1 U");
        segments.push_back("\x1b[?1049h" + staircase(2000) + "\x1b[5;20r" + staircase(3000) + "\x1b[r\x1b[?1049l");
        runCorpus("text-flow-modes", segments);
    }

    // 17. What a flow leaves behind it for the next byte to read: the cluster
    // a combining mark joins, the item a repeat replays, and the graphic a
    // cancel takes back, each behind a run that ended in the row, at the
    // margin, on a tab and on a line feed.
    {
        std::vector<std::string> segments;
        const std::string fullRow(80, 'm');
        const char* const followers[] = {
            "\xCC\x81", // a combining mark
            "\x1b[3b", // a repeat
            "\x1bT", // a cancel
            "\xCC\x81\x1bT",
            "\x1b[2b\x1bT",
            "\b\x1bT",
        };
        for (const char* follower : followers) {
            std::string edges;
            edges += std::string("plain run") + follower + "\r\n";
            edges += fullRow + follower + "\r\n";
            edges += fullRow + "ab" + follower + "\r\n";
            edges += fullRow + "x" + follower + "\r\n";
            edges += fullRow + "x\t" + follower + "\r\n";
            edges += std::string("tabbed\t") + follower + "\r\n";
            edges += std::string("fed\n") + follower + "\r\n";
            edges += std::string("returned\r") + follower + "\r\n";
            edges += std::string("a\tb\tc\n\n\td") + follower + "e\r\n";
            segments.push_back(edges);
            segments.push_back("\x1b[?1049h" + edges + staircase(600) + follower + "\x1b[?1049l");
        }
        runCorpus("text-flow-successors", segments);
    }

    // 18. A soup of the same pieces: runs of every length with whatever the
    // flow passes, ends on or hands back between them.
    {
        std::vector<std::string> segments;
        for (int seg = 0; seg < 10; ++seg) {
            std::string chunk;
            const int ops = 500 + static_cast<int>(rnd() % 500);
            for (int i = 0; i < ops; ++i) {
                switch (rnd() % 24) {
                case 0:
                case 1:
                    chunk += "\n";
                    break;
                case 2:
                    chunk += "\r";
                    break;
                case 3:
                case 4:
                    chunk += "\t";
                    break;
                case 5:
                    chunk += "\b";
                    break;
                case 6:
                    chunk += "\x1bT";
                    break;
                case 7:
                    chunk += "\xCC\x81";
                    break;
                case 8:
                    chunk += "\xE4\xB8\xAD\xE6\x96\x87";
                    break;
                case 9:
                    chunk += "\x1b[" + std::to_string(30 + rnd() % 8) + "m";
                    break;
                case 10:
                    chunk += "\x1b[" + std::to_string(1 + rnd() % 30) + ";" + std::to_string(1 + rnd() % 90) + "H";
                    break;
                case 11:
                    chunk += "\x1b[" + std::to_string(1 + rnd() % 5) + "b";
                    break;
                case 12:
                    chunk += (rnd() & 1) ? "\x1b[K" : "\x1b[m";
                    break;
                case 13:
                    if (rnd() % 8 == 0)
                        chunk += (rnd() & 1) ? "\x1b[?1049h" : "\x1b[?1049l";
                    else if (rnd() % 8 == 0)
                        chunk += (rnd() & 1) ? "\x1b[?7l" : "\x1b[?7h";
                    break;
                default:
                    for (int k = 0; k < static_cast<int>(rnd() % 120) + 1; ++k)
                        chunk.push_back(static_cast<char>(0x20 + rnd() % 0x5F));
                    break;
                }
            }
            segments.push_back(chunk + "\x1b[?7h");
        }
        runCorpus("text-flow-soup", segments);
    }

    // 19. Pages of wide glyphs: rows of ideographs, kana, syllables and
    // fullwidth punctuation that fill and wrap, in bare-line-feed staircases
    // and with returns, behind a narrow glyph that shifts the pairs onto odd
    // columns, with what can follow a wide glyph behind them (a mark, a
    // trailing jamo, a repeat, a cancel), and written over rows that already
    // hold pairs.
    {
        const auto appendUTF8 = [](std::string& out, uint32_t codePoint) {
            if (codePoint < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            } else if (codePoint < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            }
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        };
        const auto widePage = [&](int glyphs, bool hasReturns) {
            std::string out;
            for (int i = 0; i < glyphs; ++i) {
                const uint32_t pick = rnd();
                switch (pick % 40) {
                case 0:
                    out += hasReturns ? "\r\n" : "\n";
                    break;
                case 1:
                    out += "\t";
                    break;
                case 2:
                    out += "a"; // shifts the pairs behind it onto odd columns
                    break;
                case 3:
                    appendUTF8(out, 0xFF0C); // a fullwidth comma
                    break;
                case 4:
                    appendUTF8(out, 0x3042 + pick % 80); // hiragana
                    break;
                case 5:
                    appendUTF8(out, 0xAC00 + pick % 11172); // a Hangul syllable
                    break;
                case 6:
                    appendUTF8(out, 0x20000 + pick % 0x1000); // an ideograph past the basic plane
                    break;
                case 7: {
                    // A narrow glyph among them: a symbol, and the letters on
                    // either side of the first wide range and past the last
                    // of the plane's, which a range read too wide takes in.
                    static constexpr uint32_t narrow[] = { 0x2603, 0xA4D0, 0xA500, 0xA640, 0xFFE8, 0x3248, 0x1F100 };
                    appendUTF8(out, narrow[(pick >> 8) % (sizeof narrow / sizeof *narrow)]);
                    break;
                }
                default:
                    appendUTF8(out, 0x4E00 + pick % 0x5000);
                    break;
                }
            }
            return out;
        };
        std::vector<std::string> segments;
        segments.push_back(widePage(6000, true));
        segments.push_back(widePage(6000, false));
        segments.emplace_back("\x1b[?1049h\x1b[H\x1b[2J");
        segments.push_back(widePage(6000, false));
        segments.push_back("\x1b[35m" + widePage(2000, false) + "\x1b[m");
        segments.emplace_back("\x1b[?1049l");
        // Over rows that hold pairs already, from an odd column and an even one.
        segments.push_back("\x1b[H\x1b[2J" + widePage(1500, true) + "\x1b[3;2H" + widePage(300, false) + "\x1b[5;3H" + widePage(300, false));
        const std::string fullRow = [&] {
            std::string row;
            for (int glyph = 0; glyph < 40; ++glyph)
                appendUTF8(row, 0x4E2D);
            return row;
        }();
        const char* const followers[] = {
            "\xCC\x81", // a combining mark
            "\xE1\x86\xA8", // a trailing jamo, which extends a syllable
            "\x1b[3b", // a repeat
            "\x1bT", // a cancel
            "\xE3\x82\x99", // a combining voiced sound mark
        };
        for (const char* follower : followers) {
            std::string edges;
            edges += fullRow + follower + "\r\n";
            edges += fullRow + "\xE4\xB8\xAD\xE6\x96\x87" + follower + "\r\n";
            edges += fullRow + "\xEA\xB0\x80" + follower + "\r\n"; // a syllable wraps, then the follower
            edges += fullRow + "\xE4\xB8\xAD\t" + follower + "\r\n";
            edges += std::string("a") + fullRow + follower + "\r\n";
            edges += "\xE4\xB8\xAD\xE6\x96\x87\n" + std::string(follower) + "\r\n";
            edges += "\xE4\xB8\xAD\xE6\x96\x87\r" + std::string(follower) + "\r\n";
            segments.push_back(edges);
            segments.push_back("\x1b[?1049h" + edges + widePage(200, false) + follower + "\x1b[?1049l");
        }
        // A run that ends inside a row of pairs on the other parity cuts one
        // of them in half at each end it does not share: rows of pairs on
        // odd columns, then pairs on even ones written over them from the row
        // start, from a tab stop, and behind a wrap that scrolls nothing.
        {
            std::string oddRows = "\x1b[H\x1b[2J";
            for (int row = 0; row < 8; ++row) {
                oddRows += "a";
                for (int glyph = 0; glyph < 39; ++glyph)
                    appendUTF8(oddRows, 0x5B57);
                oddRows += "\r\n";
            }
            std::string over = "\x1b[1;1H" + fullRow;
            for (int glyph = 0; glyph < 10; ++glyph)
                appendUTF8(over, 0x4E00 + static_cast<uint32_t>(glyph));
            over += "\n";
            for (int glyph = 0; glyph < 6; ++glyph)
                appendUTF8(over, 0x4E10 + static_cast<uint32_t>(glyph));
            over += "\r\t";
            for (int glyph = 0; glyph < 5; ++glyph)
                appendUTF8(over, 0x4E20 + static_cast<uint32_t>(glyph));
            over += "\n\t\t";
            for (int glyph = 0; glyph < 4; ++glyph)
                appendUTF8(over, 0x4E30 + static_cast<uint32_t>(glyph));
            segments.push_back(oddRows + over);
            segments.push_back("\x1b[?1049h" + oddRows + over + "\x1b[?1049l");
        }
        segments.push_back("\x1b[?7l" + widePage(1500, false) + "\x1b[?7h");
        segments.push_back("\x1b[?69h\x1b[5;60s" + widePage(1500, false) + "\x1b[?69l");
        segments.push_back("\x1b[H\x1b[2J\x1b[6;1H\x1b#6\x1b[1;1H" + widePage(800, false));
        runCorpus("wide-flow", segments);
    }

    // 20. Text of other alphabets: letters of two and three bytes among ASCII
    // words, in lines that wrap and in bare-line-feed staircases, with what
    // is not plain among them (a mark, a pictograph, a soft hyphen, a joiner,
    // a selector, a leading jamo with the syllable it takes, an ideograph)
    // and what can follow a letter behind them.
    //
    // Each extender is followed by a plain letter, so no cluster grows past
    // the one continuation a cell holds inline: a longer one is interned, and
    // the number the pool gives it depends on what was interned before it,
    // which differs between a cluster grown a byte at a time and one written
    // whole. This fingerprint reads that number raw.
    {
        const auto appendUTF8 = [](std::string& out, uint32_t codePoint) {
            if (codePoint < 0x80) {
                out.push_back(static_cast<char>(codePoint));
                return;
            }
            if (codePoint < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            } else if (codePoint < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            }
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        };
        const auto lettersPage = [&](int pieces, bool hasReturns) {
            std::string out;
            for (int i = 0; i < pieces; ++i) {
                const uint32_t pick = rnd();
                const int length = 1 + static_cast<int>((pick >> 8) % 9);
                switch (pick % 32) {
                case 0:
                case 1:
                    out += hasReturns ? "\r\n" : "\n";
                    break;
                case 2:
                    out += "\t";
                    break;
                case 3:
                case 4:
                case 5:
                    for (int k = 0; k < length; ++k)
                        out.push_back(static_cast<char>('a' + (pick >> (k + 3)) % 26));
                    out += " ";
                    break;
                case 6:
                    // Pictographs, alone and in runs, and each thing that
                    // makes one more than a cluster of its own: a selector, a
                    // modifier, a joiner with another behind it, a keycap, a
                    // pair of regional indicators.
                    switch ((pick >> 5) % 9) {
                    case 0:
                        for (int k = 0; k < length; ++k)
                            appendUTF8(out, 0x1F600 + (pick >> (k + 9)) % 48);
                        break;
                    case 1:
                        appendUTF8(out, 0x1F44D);
                        appendUTF8(out, 0x1F3FB + (pick >> 9) % 5);
                        break;
                    case 2:
                        appendUTF8(out, 0x2764);
                        appendUTF8(out, 0xFE0F);
                        break;
                    case 3:
                        appendUTF8(out, 0x1F469);
                        appendUTF8(out, 0x200D);
                        appendUTF8(out, 0x1F4BB);
                        break;
                    case 4:
                        out += "1";
                        appendUTF8(out, 0xFE0F);
                        appendUTF8(out, 0x20E3);
                        break;
                    case 5:
                        appendUTF8(out, 0x1F1E6 + (pick >> 9) % 26);
                        appendUTF8(out, 0x1F1E6 + (pick >> 14) % 26);
                        break;
                    case 6:
                        appendUTF8(out, 0xA9);
                        appendUTF8(out, 0xAE);
                        appendUTF8(out, 0x2122);
                        break;
                    case 7:
                        appendUTF8(out, 0x231A); // a pictograph of the basic plane that draws as emoji
                        appendUTF8(out, 0x2614);
                        break;
                    default:
                        appendUTF8(out, 0x1F680);
                        appendUTF8(out, 0xFE0E); // and one told to draw as text
                        break;
                    }
                    out += (pick & 16) ? " " : "";
                    break;
                case 7:
                    for (int k = 0; k < length; ++k)
                        appendUTF8(out, 0x391 + (pick >> (k + 3)) % 25); // Greek capitals
                    break;
                case 8:
                    for (int k = 0; k < length; ++k)
                        appendUTF8(out, 0x430 + (pick >> (k + 3)) % 32); // Cyrillic
                    out += " ";
                    break;
                case 9:
                    appendUTF8(out, 0x2018 + pick % 6); // quotation marks, ambiguous in width
                    break;
                case 10:
                    appendUTF8(out, 0x2500 + pick % 0x80); // box drawing
                    break;
                case 11:
                    appendUTF8(out, 0x2192); // an arrow
                    break;
                case 12:
                    out += "e\xCC\x81x"; // a letter under a mark
                    break;
                case 13:
                    out += "o";
                    appendUTF8(out, 0x301 + pick % 4); // another, under one of four marks
                    out += "x";
                    break;
                case 14:
                    appendUTF8(out, 0x1F600 + pick % 16); // a pictograph
                    break;
                case 15:
                    appendUTF8(out, (pick & 64) ? 0xA9 : 0x2122); // a text pictograph
                    break;
                case 16:
                    appendUTF8(out, (pick & 64) ? 0xAD : 0xA0); // a soft hyphen, a no-break space
                    break;
                case 17:
                    out += "u";
                    appendUTF8(out, (pick & 64) ? 0x200D : 0xFE0F); // a joiner, a selector
                    out += "x";
                    break;
                case 18:
                    appendUTF8(out, 0x1100 + pick % 19); // a leading jamo
                    if (pick & 64)
                        appendUTF8(out, 0xAC00 + (pick >> 9) % 11172); // and the syllable it takes
                    else
                        appendUTF8(out, 0x1161 + (pick >> 9) % 21);
                    out += "x";
                    break;
                case 19:
                    appendUTF8(out, 0x4E00 + pick % 0x5000);
                    appendUTF8(out, 0x4E00 + (pick >> 7) % 0x5000);
                    break;
                default:
                    for (int k = 0; k < length; ++k)
                        appendUTF8(out, 0xC0 + (pick >> (k + 3)) % 0x40); // Latin-1 letters
                    if (pick & 32)
                        out += " ";
                    break;
                }
            }
            return out;
        };
        std::vector<std::string> segments;
        segments.push_back(lettersPage(5000, true));
        segments.push_back(lettersPage(5000, false));
        segments.emplace_back("\x1b[?1049h\x1b[H\x1b[2J");
        segments.push_back(lettersPage(5000, false));
        segments.push_back("\x1b[32m" + lettersPage(1500, false) + "\x1b[m");
        segments.emplace_back("\x1b[?1049l");
        const std::string fullRow = [&] {
            std::string row;
            for (int letter = 0; letter < 80; ++letter)
                appendUTF8(row, 0xE0 + static_cast<uint32_t>(letter % 24));
            return row;
        }();
        const char* const followers[] = {
            "\xCC\x81", // a combining mark
            "\x1b[3b", // a repeat
            "\x1bT", // a cancel
            "\xEF\xB8\x8F", // a selector
            "\xE2\x80\x8D", // a joiner
            "\xF0\x9F\x8F\xBB", // a modifier
        };
        for (const char* follower : followers) {
            std::string edges;
            // A pictograph with each follower behind it: in a run, alone, at
            // the margin, behind a wrap, before a tab and before a line end.
            edges += std::string("ok \xF0\x9F\x98\x80\xF0\x9F\x9A\x80") + follower + "\r\n";
            edges += std::string("\xF0\x9F\x98\x80") + follower + "\r\n";
            edges += fullRow.substr(0, 156) + "\xF0\x9F\x98\x80" + follower + "\r\n";
            edges += fullRow + "\xF0\x9F\x98\x80\xF0\x9F\x9A\x80" + follower + "\r\n";
            edges += fullRow.substr(0, 158) + "\xF0\x9F\x98\x80" + follower + "\r\n"; // one cell left where a pair has to go
            edges += std::string("\xC2\xA9\xC2\xAE") + follower + "\r\n";
            edges += std::string("\xF0\x9F\x98\x80\t") + follower + "\r\n";
            edges += std::string("\xF0\x9F\x98\x80\n") + follower + "\r\n";
            edges += std::string("\xC3\xA9t\xC3\xA9") + follower + "\r\n";
            edges += fullRow + follower + "\r\n";
            edges += fullRow + "\xC3\xA9\xC3\xA8" + follower + "\r\n";
            edges += fullRow + "\xC3\xA9" + follower + "\r\n";
            edges += fullRow + "\xC3\xA9\t" + follower + "\r\n";
            edges += "mot \xC3\xA9t\xC3\xA9\n" + std::string(follower) + "\r\n";
            edges += "mot \xC3\xA9t\xC3\xA9\r" + std::string(follower) + "\r\n";
            edges += std::string("a\t\xC3\xA9\xC3\xA8\tc\n\n\td\xC3\xA9") + follower + "e\r\n";
            segments.push_back(edges);
            segments.push_back("\x1b[?1049h" + edges + lettersPage(150, false) + follower + "\x1b[?1049l");
        }
        // Letters written over rows that hold pairs, which their ends cut.
        {
            std::string pairs = "\x1b[H\x1b[2J";
            for (int row = 0; row < 8; ++row) {
                pairs += (row & 1) ? "a" : "";
                for (int glyph = 0; glyph < 39; ++glyph)
                    appendUTF8(pairs, 0x5B57);
                pairs += "\r\n";
            }
            std::string over = "\x1b[1;1H" + fullRow + "\xC3\xA9\xC3\xA8\xC3\xA0 mot\n\xC3\xA9\xC3\xA8\r\t\xC3\xA9t\xC3\xA9\n\t\t\xC3\xA0 la";
            segments.push_back(pairs + over);
            segments.push_back("\x1b[?1049h" + pairs + over + "\x1b[?1049l");
        }
        segments.push_back("\x1b[?7l" + lettersPage(1200, false) + "\x1b[?7h");
        segments.push_back("\x1b[4h" + lettersPage(600, false) + "\x1b[4l");
        segments.push_back("\x1b[?69h\x1b[5;60s" + lettersPage(1200, false) + "\x1b[?69l");
        segments.push_back("\x1b[H\x1b[2J\x1b[6;1H\x1b#6\x1b[1;1H" + lettersPage(600, false));
        segments.push_back("\x1b(0" + lettersPage(600, false) + "\x1b(B"); // a designated set translates the ASCII among them
        runCorpus("letters-flow", segments);

        // Glyphs of both widths one at a time, as text that mixes scripts is:
        // a letter, an ideograph, a syllable, a pictograph and ASCII, each
        // beside any other, in lines that wrap with a pair at the margin, and
        // now and then what can join the glyph before it, with a letter
        // behind so no cluster grows past one continuation, and a malformed
        // sequence of each kind the decoder substitutes for.
        const char* const malformed[] = {
            "\x80",
            "\xBF", // stray continuations
            "\xC0\xAF",
            "\xC1\xBF", // overlong leads
            "\xC3",
            "\xE4\xB8",
            "\xF0\x9F\x98", // truncated sequences
            "\xE0\x80\xAF",
            "\xF0\x80\x80\xAF", // overlong forms
            "\xED\xA0\x80", // a surrogate
            "\xF4\x90\x80\x80", // past the last code point
            "\xF8",
            "\xFF", // bytes no sequence starts with
        };
        const auto mixedPage = [&](int glyphs) {
            std::string out;
            for (int i = 0; i < glyphs; ++i) {
                const uint32_t pick = rnd();
                switch (pick % 17) {
                case 0:
                case 1:
                    out.push_back(static_cast<char>('a' + (pick >> 8) % 26));
                    break;
                case 2:
                    out += " ";
                    break;
                case 3:
                case 4:
                    appendUTF8(out, 0xC0 + (pick >> 8) % 0x40); // a Latin-1 letter
                    break;
                case 5:
                    appendUTF8(out, 0x430 + (pick >> 8) % 32); // a Cyrillic one
                    break;
                case 6:
                case 7:
                    appendUTF8(out, 0x4E00 + (pick >> 8) % 0x5000); // an ideograph
                    break;
                case 8:
                    appendUTF8(out, 0x3041 + (pick >> 8) % 0x56); // a kana
                    break;
                case 9:
                    appendUTF8(out, 0xAC00 + (pick >> 8) % 11172); // a Hangul syllable
                    break;
                case 10:
                    appendUTF8(out, 0x20000 + (pick >> 8) % 0xA6D7); // an ideograph past the basic plane
                    break;
                case 11:
                    appendUTF8(out, 0x1F600 + (pick >> 8) % 0x50); // a pictograph drawn as emoji
                    break;
                case 12:
                    appendUTF8(out, (pick & 256) ? 0xA9 : 0x2122); // one drawn as text
                    break;
                case 13:
                    switch ((pick >> 8) % 5) {
                    case 0:
                        appendUTF8(out, 0x301); // a mark
                        break;
                    case 1:
                        appendUTF8(out, 0xFE0F); // a selector
                        break;
                    case 2:
                        appendUTF8(out, 0x200D); // a joiner
                        break;
                    case 3:
                        appendUTF8(out, 0x11A8 + (pick >> 11) % 27); // a trailing jamo
                        break;
                    default:
                        appendUTF8(out, 0x1F3FB); // a modifier
                        break;
                    }
                    out += "x";
                    break;
                case 14:
                    out += (pick & 256) ? "\r\n" : "\n";
                    break;
                case 15:
                    out += malformed[(pick >> 8) % std::size(malformed)];
                    out += (pick & 0x80000) ? "y" : "";
                    break;
                default:
                    out += "\t";
                    break;
                }
            }
            return out;
        };
        std::vector<std::string> mixed;
        for (int page = 0; page < 6; ++page)
            mixed.push_back(mixedPage(4000));
        mixed.emplace_back("\x1b[?1049h\x1b[H\x1b[2J");
        mixed.push_back(mixedPage(4000));
        mixed.push_back("\x1b[33m" + mixedPage(1500) + "\x1b[m");
        mixed.emplace_back("\x1b[?1049l");
        // A pair where one cell is left, where two are, and behind a wrap.
        for (int narrow = 76; narrow <= 80; ++narrow) {
            std::string edge(static_cast<std::size_t>(narrow), 'n');
            appendUTF8(edge, 0x5B57);
            appendUTF8(edge, 0xE9);
            appendUTF8(edge, 0xAC00);
            appendUTF8(edge, 0x5B57);
            appendUTF8(edge, 0x1F600);
            mixed.push_back(edge + "\r\n");
        }
        mixed.push_back("\x1b[?7l" + mixedPage(1200) + "\x1b[?7h");
        mixed.push_back("\x1b[4h" + mixedPage(600) + "\x1b[4l");
        runCorpus("mixed-widths-flow", mixed);
    }

    // Corpus 21: control sequences of every shape, read whole against read a
    // byte at a time.
    //
    // A sequence that lies whole in a read is scanned where it stands: the
    // introducer, a private marker, the parameters and the final byte in one
    // turn of the byte loop. One that a read cuts goes through the states,
    // and in the arm that reads a byte at a time every one does. The two
    // must leave the same screen, cursor and scrollback, so each sequence has
    // text behind it that shows what it did: where the cursor went, what the
    // pen became, which modes hold.
    //
    // The shapes are the well formed ones a program sends and the ones a
    // parser must not misread: an empty parameter, a zero in front, a number
    // past the limit, more parameters than there are slots, a colon where
    // only a rendition takes one, a marker in the wrong place or twice, an
    // intermediate byte, a control or a cancel inside the sequence, and an
    // escape that starts another before the first has ended.
    {
        std::vector<std::string> segments;
        uint64_t state = 0x2545F4914F6CDD1Dull;
        const auto next = [&state]() {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<uint32_t>(state >> 32);
        };
        const auto appendNumber = [&](std::string& out, uint32_t limit) {
            const uint32_t shape = next() % 16;
            if (shape == 0)
                return; // an empty parameter
            if (shape == 1) {
                out += "99999999999"; // past the limit, and past what a word of digits holds
                return;
            }
            if (shape == 2)
                out += "00"; // zeros in front
            out += std::to_string(next() % limit);
        };
        const auto appendText = [&](std::string& out) {
            const int length = static_cast<int>(next() % 14) + 1;
            for (int index = 0; index < length; ++index)
                out.push_back(static_cast<char>('a' + next() % 26));
        };
        const char* const renditions[] = {
            "",
            "0",
            "1",
            "3",
            "4",
            "7",
            "22",
            "24",
            "27",
            "31",
            "39",
            "42",
            "49",
            "91",
            "1;31",
            "0;1;4;33;44",
            "38;5;24",
            "48;5;200",
            "38;2;10;20;30",
            "48;2;125;136;147",
            "38:5:24",
            "38:2::10:20:30",
            "48:2:125:136:147",
            "4:3",
            "58:5:44",
            "58;5;44;2",
            "1;2;3;4:3;31",
            "38:5:24;48:2:125:136:147",
            "38;5",
            "38;2;1",
            ";",
            ";;1",
            "1;;31",
            "0031",
            "38;5;999",
            "4:9",
            "1;2;3;4;5;6;7;8;9;1;2;3;4;5;6;7;8;9;1;2;3;4;5;6;7;8;9;1;2;3;4;5;6;31", // more parameters than slots
        };
        const char* const privateModes[] = { "1", "5", "6", "7", "12", "25", "45", "69", "1004", "1049", "2004", "2026", "6;7", "1;25", "", "007", "1:2" };
        const char* const oddities[] = {
            "\x1b[1?h",
            "\x1b[??1h",
            "\x1b[?1?h",
            "\x1b[>4;2m",
            "\x1b[>c",
            "\x1b[<u",
            "\x1b[=1u",
            "\x1b[?u",
            "\x1b[2 q",
            "\x1b[ q",
            "\x1b[!p",
            "\x1b[1\"q",
            "\x1b[0\"q",
            "\x1b[?1$p",
            "\x1b[4$p",
            "\x1b[1;1;2;2$z",
            "\x1b[#P",
            "\x1b[#Q",
            "\x1b[5y",
            "\x1b[1~",
            "\x1b[3 1H",
            "\x1b[ 3H",
            "\x1b[1 2 q",
            "\x1b[1\n;5H",
            "\x1b[\r5C",
            "\x1b[5\bC",
            "\x1b[12\x18",
            "\x1b[3\x1a",
            "\x1b[12\x1b[5C",
            "\x1b[1\x7f"
            "2H",
            "\x1b[1\xC3\xA9H",
            "\x1b[1\xC2\x9B"
            "2H",
            "\x1b[\x1b\x1b[2C",
            "\x1b[;H",
            "\x1b[;;H",
            "\x1b[3;H",
            "\x1b[;7H",
            "\x1b[0;0H",
            "\x1b[999;999H",
            "\x1b[1:2H",
            "\x1b[3:C",
            "\x1b[2:J",
            "\x1b[c",
            "\x1b[5n",
            "\x1b[6n",
            "\x1b[?6n",
            "\x1b[0x",
            "\x1b[s",
            "\x1b[u",
            "\x1b"
            "7",
            "\x1b"
            "8",
            "\x1b[g",
            "\x1b[3g",
            "\x1bH",
            "\x1b[2I",
            "\x1b[2Z",
            "\t",
            "\n",
            "\r",
            "\x1b[4h",
            "\x1b[4l",
            "\x1b[20h",
            "\x1b[20l",
            "\x1b[r",
            "\x1b[5;18r",
            "\x1b[18;5r",
            "\x1b[;10r",
            "\x1b[5;60s",
            "\x1b[m\x1b[2J\x1b[H",
        };
        const auto appendSequences = [&](std::string& out, int count) {
            for (int sequence = 0; sequence < count; ++sequence) {
                const uint32_t pick = next();
                switch (pick % 16) {
                case 0:
                case 1:
                case 2: { // the cursor, to a place or by a distance
                    out += "\x1b[";
                    appendNumber(out, 26);
                    const char final = "ABCDEFGH`adefHHG"[(pick >> 4) % 16];
                    if (((pick >> 8) & 3) == 0 || final == 'H' || final == 'f') {
                        out += ((pick >> 10) & 15) == 0 ? ":" : ";";
                        appendNumber(out, 90);
                    }
                    out.push_back(final);
                    break;
                }
                case 3:
                case 4: { // erasing, inserting, deleting and scrolling
                    out += "\x1b[";
                    appendNumber(out, 4);
                    out.push_back("JKKKX@PLMST"[(pick >> 4) % 11]);
                    break;
                }
                case 5:
                case 6:
                case 7:
                    out += std::string("\x1b[") + renditions[(pick >> 4) % (sizeof renditions / sizeof *renditions)] + "m";
                    break;
                case 8:
                    out += std::string("\x1b[?") + privateModes[(pick >> 4) % (sizeof privateModes / sizeof *privateModes)] + (((pick >> 12) & 1) ? "h" : "l");
                    break;
                case 9:
                    out += oddities[(pick >> 4) % (sizeof oddities / sizeof *oddities)];
                    break;
                case 10: { // a glyph and its repeat
                    out.push_back(static_cast<char>('A' + (pick >> 4) % 26));
                    static constexpr int counts[] = { 0, 1, 2, 3, 78, 79, 80, 81, 159, 200, 5000 };
                    out += "\x1b[" + std::to_string(counts[(pick >> 9) % (sizeof counts / sizeof *counts)]) + "b";
                    break;
                }
                case 11: // a pair and its repeat
                    out += "\xE7\x95\x8C\x1b[" + std::to_string((pick >> 4) % 5) + "b";
                    break;
                default:
                    appendText(out);
                    break;
                }
            }
        };
        for (int segment = 0; segment < 48; ++segment) {
            std::string out;
            if (segment % 12 == 5)
                out += "\x1b[?1049h";
            if (segment % 12 == 9)
                out += "\x1b[?1049l";
            appendSequences(out, 160);
            segments.push_back(out);
        }
        // Back to a known screen, then the same over rows of pairs and over a
        // full screen of text, where a cursor that lands anywhere cuts both.
        {
            std::string pairs = "\x1b[!p\x1b[?1049l\x1b[?69l\x1b[?6l\x1b[?7h\x1b[4l\x1b[r\x1b[m\x1b[H\x1b[2J";
            for (int row = 0; row < 24; ++row) {
                pairs += (row & 1) ? "a" : "";
                for (int glyph = 0; glyph < 39; ++glyph)
                    pairs += "\xE7\x95\x8C";
                pairs += "\r\n";
            }
            appendSequences(pairs, 400);
            segments.push_back(pairs);
            std::string full = "\x1b[!p\x1b[?69l\x1b[?6l\x1b[?7h\x1b[4l\x1b[r\x1b[m\x1b[H\x1b[2J";
            for (int row = 0; row < 24; ++row)
                full += std::string(80, static_cast<char>('a' + row)) + (row < 23 ? "\r\n" : "");
            appendSequences(full, 400);
            segments.push_back(full);
        }
        runCorpus("control-sequences", segments);
    }

    if (failures == 0) {
        std::printf("scroll-lane differential: all corpora chunk-invariant OK\n");
        return 0;
    }
    std::printf("scroll-lane differential: %d FAILURES\n", failures);
    return 1;
}
