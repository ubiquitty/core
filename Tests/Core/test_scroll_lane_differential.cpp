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

    if (failures == 0) {
        std::printf("scroll-lane differential: all corpora chunk-invariant OK\n");
        return 0;
    }
    std::printf("scroll-lane differential: %d FAILURES\n", failures);
    return 1;
}
