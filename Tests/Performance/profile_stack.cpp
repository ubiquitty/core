// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// profile_stack.cpp — portable latency profiler across the stack: the VT
// protocol parser, the screen plane's checkpoint, and the tmux Session deliver/render
// path. Each operation is sampled per-call and reported as a full latency
// distribution (p50/p90/p95/p99/max, µs/op) via Profiler.h — so a change is
// diffed against a prior run's distribution, not a single averaged number.
//
//   profile_stack [--platform NAME] [--commit SHA] [--iters N] [--warmup N]
//
// Pure CPU, no GUI, no PTY — comparable across platforms. The macOS render
// profiler emits the same JSON shape from the GPU side.

#include "Profiler.h"

#include "Core/LineStore.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Tmux/Session.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string repeatTo(const std::string& unit, std::size_t targetBytes)
{
    std::string out;
    out.reserve(targetBytes + unit.size());
    while (out.size() < targetBytes)
        out += unit;
    return out;
}

std::unique_ptr<Ubiquitty::Terminal> makeTerm(int rows, int columns)
{
    auto term = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*term, rows, columns);
    return term;
}

std::unique_ptr<Ubiquitty::Terminal> makeFilledTerm(int rows, int columns)
{
    auto term = makeTerm(rows, columns);
    const std::string fill = repeatTo("\x1b[1;36mfile\x1b[0m.cpp:42  const auto value = compute(x, y);  // note\r\n", static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns) * 4);
    Ubiquitty::processBytes(term.get(), fill);
    return term;
}

// --- protocol parse latency: process one representative burst per op ----------

void protocolProfiles(std::vector<Ubiquitty::Profiler::Distribution>& out, int warmup, int iters)
{
    struct Scene {
        const char* name;
        std::string burst;
    };
    const std::size_t burstBytes = 8u * 1024u;
    const Scene scenes[] = {
        { "protocol_plain", repeatTo("The quick brown fox jumps over the lazy dog 0123456789\r\n", burstBytes) },
        { "protocol_sgr", repeatTo("\x1b[1;31mERR\x1b[0m \x1b[32mok\x1b[0m \x1b[4;33mwarn\x1b[0m \x1b[7mrev\x1b[0m\r\n", burstBytes) },
        { "protocol_csi", repeatTo("\x1b[2J\x1b[10;20H\x1b[K\x1b[1A\x1b[3D\x1b[7m \x1b[0m\x1b[2;5r", burstBytes) },
        { "protocol_osc", repeatTo("\x1b]0;a title for the window\x07\x1b]8;;https://example.com/path\x07link\x1b]8;;\x07 ", burstBytes) },
        { "protocol_utf8", repeatTo("héllo wörld 日本語 \U0001f389 grüße\r\n", burstBytes) },
    };
    for (const Scene& scene : scenes) {
        auto term = makeTerm(24, 80);
        out.push_back(Ubiquitty::Profiler::profile(scene.name, warmup, iters, [&] { Ubiquitty::processBytes(term.get(), scene.burst); }));
        term.reset();
    }
}

// --- end-to-end ingest latency: realistic mixed workloads --------------------
// Real terminal traffic interleaves constructs — a build log mixes paths, SGR and
// plain text; a git log mixes colour, graph glyphs and hashes; a progress bar is
// carriage-return redraws of one line; an editor repaints the screen with absolute
// cursor moves. These exercise cache/branch behaviour the single-construct protocol
// bursts don't. Each op feeds one ~32 KiB burst of the workload.

void ingestProfiles(std::vector<Ubiquitty::Profiler::Distribution>& out, int warmup, int iters)
{
    const std::size_t burstBytes = 32u * 1024u;
    struct Scene {
        const char* name;
        std::string burst;
    };
    const Scene scenes[] = {
        // Compiler/CI build output: path:line:column, coloured warning/error, plain.
        { "ingest_buildlog",
            repeatTo("src/core/parser.cpp:142:18: \x1b[1;35mwarning:\x1b[0m unused variable 'x' "
                     "[\x1b[1m-Wunused\x1b[0m]\r\n    const int x = compute(lhs, rhs);\r\n"
                     "\x1b[32m[ 42%]\x1b[0m Building CXX object core/CMakeFiles/core.direction/parser.cpp.o\r\n",
                burstBytes) },
        // git log --graph --color: graph glyphs, coloured hash/refs, plain message.
        { "ingest_gitlog",
            repeatTo("\x1b[33m*\x1b[0m \x1b[33mab12cd3\x1b[0m \x1b[32m(2 days ago)\x1b[0m "
                     "\x1b[1;34m<dev>\x1b[0m refactor: tidy the parser dispatch table\r\n"
                     "\x1b[33m|\\\x1b[0m  \x1b[33m| *\x1b[0m \x1b[33mffee991\x1b[0m fix: off-by-one\r\n",
                burstBytes) },
        // Progress bar: carriage-return redraws of a single line (no newline).
        { "ingest_progress", repeatTo("\rDownloading \x1b[36m[####################----------]\x1b[0m 67%  3.4 MB/s  eta 12s", burstBytes) },
        // Full-screen editor repaint: absolute cursor moves + line clears + syntax SGR.
        { "ingest_editor",
            repeatTo("\x1b[5;1H\x1b[K\x1b[38;5;75mvoid\x1b[0m \x1b[38;5;221mrender\x1b[0m(\x1b[38;5;75mTerm\x1b[0m* t) {"
                     "\x1b[6;1H\x1b[K    \x1b[38;5;204mfor\x1b[0m (\x1b[38;5;75mint\x1b[0m i = 0; i < n; ++i)",
                burstBytes) },
    };
    for (const Scene& scene : scenes) {
        auto term = makeTerm(24, 80);
        out.push_back(Ubiquitty::Profiler::profile(scene.name, warmup, iters, [&] { Ubiquitty::processBytes(term.get(), scene.burst); }));
        term.reset();
    }
}

// --- screen-plane checkpoint latency ------------------------------------------

void syncProfiles(std::vector<Ubiquitty::Profiler::Distribution>& out, int warmup, int iters)
{
    namespace Sync = Ubiquitty::Protocol::Sync;
    auto host = makeFilledTerm(24, 80);

    Sync::ScreenPublisher publisher;
    Sync::AttachmentBaseline attachment;
    std::vector<uint8_t> body;
    if (!publisher.capture(*host) || !publisher.checkpoint(*host, 1, body, attachment)) {
        std::fprintf(stderr, "profile: sync_checkpoint_encode could not encode a checkpoint\n");
        std::exit(1);
    }

    // Cleared, not reconstructed, as the host's scratch is — a fresh vector
    // here would profile the geometric growth rather than the encode.
    std::vector<uint8_t> encoded;
    out.push_back(Ubiquitty::Profiler::profile("sync_checkpoint_encode", warmup, iters, [&] {
        encoded.clear();
        Sync::AttachmentBaseline scratch;
        (void)publisher.checkpoint(*host, 1, encoded, scratch);
    }));

    // An apply that refused would profile the refusal, so the outcome is read
    // once before sampling.
    auto applyTerm = makeTerm(24, 80);
    Sync::ScreenReplica replica;
    if (Sync::applyCheckpoint(*applyTerm, body, replica) != Sync::ApplyOutcome::Applied) {
        std::fprintf(stderr, "profile: sync_checkpoint_apply refused the checkpoint it samples\n");
        std::exit(1);
    }
    out.push_back(Ubiquitty::Profiler::profile("sync_checkpoint_apply", warmup, iters, [&] { (void)Sync::applyCheckpoint(*applyTerm, body, replica); }));
    applyTerm.reset();

    host.reset();
}

// --- tmux Session deliver/render latency -------------------------------------

void tmuxProfiles(std::vector<Ubiquitty::Profiler::Distribution>& out, int warmup, int iters)
{
    using namespace Ubiquitty::Protocol::Tmux;
    Session session;
    session.addWindow(0);
    const int paneID = 1;
    const std::string layout = "ssss,80x24,0,0,1";
    session.applyLayout(0, layout);
    if (!session.pane(paneID))
        return; // pane setup failed — skip rather than emit a bogus number

    // Deliver path (the ingest thread's %output write into the pane's Terminal).
    const std::string burst = "prompt$ \x1b[32mok\x1b[0m\r\nline two: the quick brown fox\r\n";
    out.push_back(Ubiquitty::Profiler::profile("tmux_deliver", warmup, iters, [&] { session.deliverOutput(paneID, { reinterpret_cast<const std::uint8_t*>(burst.data()), burst.size() }); }));

    // Render/serialize path (a frontend reading the pane's Ubiquitty::Terminal under its lock —
    // the same canonical lock the deliver path takes).
    Ubiquitty::Protocol::Tmux::Pane* pane = session.pane(paneID);
    Ubiquitty::Protocol::Sync::ScreenPublisher publisher;
    out.push_back(Ubiquitty::Profiler::profile("tmux_render_read", warmup, iters, [&] {
        pane->lock();
        (void)publisher.capture(*pane->term());
        pane->unlock();
    }));
}

// --- width-reflow latency: the resize hot path ------------------------------
// A resize that changes the column count rewraps the live screen + the scrollback
// ring — the most expensive thing a window drag triggers. Each op is one grow+
// shrink cycle (80→100→80), so the Ubiquitty::Terminal returns to its starting width and the
// distribution is a clean per-cycle latency.

void reflowProfiles(std::vector<Ubiquitty::Profiler::Distribution>& out, int warmup, int iters)
{
    auto term = makeTerm(24, 80);
    // Fill the scrollback with wrapped, mixed-width content so reflow has real
    // work (long lines that wrap differently at 80 vs 100 columns).
    const std::string body = repeatTo("the quick brown fox jumps over the lazy dog and then keeps going well past the "
                                      "right margin so the line wraps at least once at eighty columns 0123456789 ",
        256u * 1024u);
    Ubiquitty::processBytes(term.get(), body);

    out.push_back(Ubiquitty::Profiler::profile("reflow_cycle_80_100", warmup, iters / 8 + 1, [&] {
        Ubiquitty::resize(*term, 24, 100, Ubiquitty::Modes::ReflowMode::Reflow);
        Ubiquitty::resize(*term, 24, 80, Ubiquitty::Modes::ReflowMode::Reflow);
    }));

    term.reset();
}

// --- scrollback (LineStore) latency: history ingest + search scan -----------
// A flooding shell pays the push cost as rows age into history; a scrollback find
// pays the scan cost. Both isolated from the parser here.

void scrollbackProfiles(std::vector<Ubiquitty::Profiler::Distribution>& out, int warmup, int iters)
{
    const int capacity = Ubiquitty::VT::fallbackScrollbackLines;
    const int rowCells = 80;

    std::vector<Ubiquitty::Cell> row(static_cast<std::size_t>(rowCells));
    for (int i = 0; i < rowCells; ++i) {
        row[static_cast<std::size_t>(i)] = Ubiquitty::Cell {};
        row[static_cast<std::size_t>(i)].codePoint = static_cast<uint32_t>('a' + (i % 26));
    }

    Ubiquitty::LineStore store;
    store.init(capacity);
    for (int i = 0; i < capacity; ++i)
        store.append(row.data(), rowCells, 0, 0, false, false);

    // Push: append one page (a screen's worth) of rows, each evicting the oldest —
    // the steady-state cost of a page scrolling into history.
    uint32_t tag = 0;
    out.push_back(Ubiquitty::Profiler::profile("scrollback_push_page", warmup, iters, [&] {
        for (int i = 0; i < 24; ++i) {
            row[0].codePoint = 0x100u + (++tag & 0xffffu);
            store.append(row.data(), rowCells, 0, 0, false, false);
        }
    }));

    // Search: read every retained line's cells once (the per-pass cost a substring
    // find over the whole scrollback pays).
    volatile uint32_t sink = 0;
    out.push_back(Ubiquitty::Profiler::profile("scrollback_search_scan", warmup, iters, [&] {
        uint32_t acc = 0;
        const int count = store.count();
        for (int line = 0; line < count; ++line) {
            const Ubiquitty::StoredLine* stored = store.at(line);
            for (int i = 0; i < stored->length; ++i)
                acc += stored->cells[i].codePoint;
        }
        sink = acc;
    }));

    store.dispose();
}

} // namespace

int main(int argc, char** argv)
{
    const char* platform = "unknown";
    const char* commit = "unknown";
    int iters = 3000;
    int warmup = 200;
    for (int i = 1; i < argc; ++i) {
        auto eq = [&](const char* f) { return std::strcmp(argv[i], f) == 0; };
        if (eq("--platform") && i + 1 < argc)
            platform = argv[++i];
        else if (eq("--commit") && i + 1 < argc)
            commit = argv[++i];
        else if (eq("--iters") && i + 1 < argc)
            iters = std::atoi(argv[++i]);
        else if (eq("--warmup") && i + 1 < argc)
            warmup = std::atoi(argv[++i]);
    }

    std::vector<Ubiquitty::Profiler::Distribution> results;
    protocolProfiles(results, warmup, iters);
    ingestProfiles(results, warmup, iters);
    syncProfiles(results, warmup, iters);
    tmuxProfiles(results, warmup, iters);
    reflowProfiles(results, warmup, iters);
    scrollbackProfiles(results, warmup, iters);

    Ubiquitty::Profiler::report(results);
    Ubiquitty::Profiler::emitJSON(results, platform, commit);
    return 0;
}
