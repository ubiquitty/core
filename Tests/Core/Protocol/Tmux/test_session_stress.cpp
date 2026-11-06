// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_session_stress.cpp — concurrency stress for the tmux Session-ownership
// model. The macOS/Linux frontends are thin renderers over
// Ubiquitty::Protocol::Tmux::Session: an ingest thread delivers %output + reconciles layout
// (splits/closes), while the render thread looks panes up, reads their Terms under
// the per-pane lock, and reclaims removed panes after it unbinds. This drives that
// exact two-thread protocol hard so ThreadSanitizer catches a missing lock / bad
// lock order and AddressSanitizer catches a use-after-free / double-free of a
// pane Terminal — the failure classes the cutover must rule out.
//
// It models, in portable code, the GUI scenarios that can't run headless:
//   - pre-split attach + live split/unsplit under an %output flood,
//   - kill-pane on the actively-written pane (pinned mid-"pass" removal — the UAF),
//   - window-close and %exit mid-flood,
//   - the renderer reading a pane's Ubiquitty::Terminal concurrently with delivery.
// Pure model: no AppKit/Metal, no live tmux. Pass = runs clean under the sanitizer.

#include "Core/Protocol/Tmux/Session.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"

#include "Core/TerminalOperations.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Ubiquitty::Protocol::Tmux;

namespace {

std::atomic<bool> g_stop { false };

// A tmux layout string for 1..3 side-by-side panes carrying `ids`. Matches the
// grammar test_session.cpp exercises (checksum prefix tolerated).
std::string layoutFor(const std::vector<int>& ids)
{
    char buffer[256];
    if (ids.size() <= 1) {
        std::snprintf(buffer, sizeof(buffer), "ssss,80x24,0,0,%d", ids.empty() ? 0 : ids[0]);
    } else if (ids.size() == 2) {
        std::snprintf(buffer, sizeof(buffer), "ssss,80x24,0,0{40x24,0,0,%d,39x24,41,0,%d}", ids[0], ids[1]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "ssss,80x24,0,0{26x24,0,0,%d,26x24,27,0,%d,25x24,54,0,%d}", ids[0], ids[1], ids[2]);
    }
    return buffer;
}

// The sole structure-mutating + delivering thread (mirrors the tmux ingest
// thread: %output, %layout-change, %window-close, %exit are all parsed here).
void ingestLoop(Session& session, int iterations)
{
    std::mt19937 rng(0xC0FFEEu);
    constexpr int windows = 3;
    int nextPaneID = 1;
    std::unordered_map<int, std::vector<int>> panesOf; // windowID -> live pane ids

    auto relayout = [&](int windowID) { session.applyLayout(windowID, layoutFor(panesOf[windowID])); };
    for (int w = 1; w <= windows; ++w) {
        panesOf[w] = { nextPaneID++ }; // pre-split attach: one pane each
        relayout(w);
    }

    const char* burst = "prompt$ \x1b[32mok\x1b[0m\r\nline two\r\n";
    for (int iter = 0; iter < iterations && !g_stop.load(std::memory_order_relaxed); ++iter) {
        const int w = 1 + static_cast<int>(rng() % windows);
        std::vector<int>& ids = panesOf[w];
        const int action = static_cast<int>(rng() % 100);

        if (action < 55) {
            // %output flood to every live pane of this window.
            for (int id : ids)
                session.deliverOutput(id, { reinterpret_cast<const std::uint8_t*>(burst), std::strlen(burst) });
        } else if (action < 70) {
            if (ids.size() < 3) { // split
                ids.push_back(nextPaneID++);
                relayout(w);
            }
        } else if (action < 84) {
            if (ids.size() > 1) { // unsplit / pane close
                ids.pop_back();
                relayout(w);
            }
        } else if (action < 93) {
            // kill-pane on the ACTIVELY-WRITTEN pane: pin + write it (a deliver
            // run), drop it from the layout WHILE pinned (removal mid-pass →
            // retired-but-pinned), then unpin (pass end). The render thread may
            // be reading it throughout — the UAF the pin + deferred reclaim guard.
            int victim = ids.back();
            Pane* p = session.pinPane(victim);
            if (p && p->term())
                p->withTermLocked([&](Ubiquitty::Terminal& t) { Ubiquitty::processBytes(&t, "Z"); });
            ids.pop_back();
            if (ids.empty())
                ids.push_back(nextPaneID++);
            relayout(w);
            if (p)
                p->unpin();
        } else if (action < 98) {
            session.closeWindow(w); // %window-close, then reopen
            ids = { nextPaneID++ };
            relayout(w);
        } else {
            session.retireAll(); // %exit, then re-attach every window
            for (int x = 1; x <= windows; ++x) {
                panesOf[x] = { nextPaneID++ };
                relayout(x);
            }
        }
    }
    g_stop.store(true, std::memory_order_relaxed);
}

// The render thread (mirrors the macOS main thread): bind each live Session pane
// (cache its Pane* like an UBPane caches the Session Terminal), read it under the
// pane lock, and — once a pane vanishes from the layout — UNBIND it then reclaim
// it via disposeRetiredPane (the single Ubiquitty::Terminal disposer, driven by the unbind).
void renderLoop(Session& session)
{
    std::unordered_map<int, Pane*> bound; // id -> cached Session pane (rendered)
    std::unordered_map<int, int> reclaiming; // id -> remaining retry ticks
    while (!g_stop.load(std::memory_order_relaxed)) {
        // Snapshot the live panes under the structural lock (mirrors -sessionPane:).
        std::unordered_map<int, Pane*> live;
        session.lockStructure();
        for (const auto& window : session.windows())
            for (const auto& pane : window->panes())
                live[pane->paneID()] = pane.get();
        session.unlockStructure();

        // Unbind panes that left the layout; schedule their reclaim.
        for (auto it = bound.begin(); it != bound.end();) {
            if (live.find(it->first) == live.end()) {
                reclaiming[it->first] = 8; // retry: a pinned deliver run defers it
                it = bound.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& kv : live)
            bound[kv.first] = kv.second; // (re)bind live panes

        // Reclaim unbound panes. disposeRetiredPane is a no-op once the pane is
        // gone, so retrying past the actual dispose is safe; the retry covers a
        // pane still pinned by an in-flight deliver run.
        for (auto it = reclaiming.begin(); it != reclaiming.end();) {
            session.disposeRetiredPane(it->first);
            if (--it->second <= 0)
                it = reclaiming.erase(it);
            else
                ++it;
        }

        // Render every bound (live) pane: read its Ubiquitty::Terminal under the canonical pane
        // lock — the same lock the deliver path takes.
        for (const auto& kv : bound) {
            kv.second->withTermLocked([](Ubiquitty::Terminal& t) {
                volatile int sb = Ubiquitty::Screen::scrollbackCount(&t);
                volatile int columns = t.columns;
                volatile int rows = t.rows;
                (void)sb;
                (void)columns;
                (void)rows;
            });
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    // Iterations: small default (CI runs this under a sanitizer); override via
    // arg or TMUX_STRESS_ITERS for a longer soak.
    int iterations = 20000;
    if (const char* env = std::getenv("TMUX_STRESS_ITERS"))
        iterations = std::atoi(env);
    if (argc > 1)
        iterations = std::atoi(argv[1]);
    if (iterations <= 0)
        iterations = 20000;

    Session session;
    std::thread ingest([&] { ingestLoop(session, iterations); });
    std::thread render([&] { renderLoop(session); });
    ingest.join();
    render.join();

    // Teardown sweep: with both threads stopped nothing is pinned, so retireAll +
    // takeRetired must dispose every remaining pane. A leak here (or a UAF/double
    // free anywhere above) is what the sanitizer build reports.
    session.retireAll();
    for (int i = 0; i < 1000; ++i)
        if (session.takeRetired().empty())
            break;
    if (!session.windows().empty()) {
        std::fprintf(stderr, "FAIL: windows not drained after retireAll\n");
        return 1;
    }

    std::printf("test_tmuxsession_stress: %d ingest iterations, no UAF/race/leak "
                "(deliver vs render vs kill-pane/window-close/%%exit reclaim)\n",
        iterations);
    return 0;
}
