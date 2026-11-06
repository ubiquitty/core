// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_term_lock.cpp — the main/ingest lock discipline in portable form.
//
// UBTerminalView guards every Terminal/CellGrid access with a single non-recursive
// lock (_termLock, an os_unfair_lock): the ingest thread holds it across
// Ubiquitty::processBytes, and main-thread input/pointer code samples the
// parser-owned mode flags under the same lock instead of racing the writer.
//
// This test reproduces that discipline with a std::mutex (the invariant under
// test is the discipline, not the lock type) and checks three things:
//   1. No deadlock / starvation under sustained concurrent ingest + reads
//      (a deadlock shows up as a hung join; starvation as zero progress).
//   2. Race-freedom — run under -fsanitize=thread (TSan) and it must be clean,
//      proving no Ubiquitty::Terminal field is read or written outside the lock.
//   3. The locked read path is cheap (lock + flag read + unlock), so making
//      the mouse/mode reads lock-correct does not regress input latency.

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

// Stands in for UBTerminalView's _termLock — non-recursive, brief critical
// sections, one lock for the whole model.
static std::mutex g_termLock;

// Mirror UBTerminalView's termWantsMouseReports(): the read main makes.
static bool wantsMouseReports(const Ubiquitty::Terminal& t) { return t.modes.mouseX10 || t.modes.mouseButtonTracking || t.modes.mouseCellMotion || t.modes.mouseAnyEvent; }

// True under ThreadSanitizer (gcc spelling; clang via __has_feature below).
#if defined(__SANITIZE_THREAD__)
#define UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_TSAN 1
#endif
#endif
#ifndef UNDER_TSAN
#define UNDER_TSAN 0
#endif

int main()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    std::atomic<bool> stop { false };
    std::atomic<long> writerOps { 0 };
    std::atomic<long> readerOps { 0 };

    // The writer flips the very mouse-mode flags the readers sample (DECSET
    // 1000/1002/1003/1006 on and off) plus ordinary output, so reads and the
    // writes they race on genuinely overlap.
    static const char feed[] = "\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h"
                               "hello \x1b[31mworld\x1b[0m\r\n"
                               "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[2J\x1b[H";
    const int feedLength = (int)(sizeof(feed) - 1);

    std::thread writer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            {
                std::scoped_lock lk(g_termLock);
                Ubiquitty::processBytes(&t, std::string_view(feed, static_cast<std::size_t>(feedLength)));
                writerOps.fetch_add(1, std::memory_order_relaxed);
            }
            // Real ingest waits on the PTY between reads; it does not hold the
            // lock back-to-back. Yield so readers aren't starved by an unfair
            // lock — models the live contention, not a pathological hot loop.
            std::this_thread::yield();
        }
    });

    const int readerCount = 3;
    std::thread readers[readerCount];
    for (int i = 0; i < readerCount; ++i) {
        readers[i] = std::thread([&] {
            long ops = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                std::scoped_lock lk(g_termLock);
                volatile bool wantsReports = wantsMouseReports(t);
                (void)wantsReports;
                ++ops;
            }
            readerOps.fetch_add(ops, std::memory_order_relaxed);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(UNDER_TSAN ? 600 : 250));
    stop.store(true, std::memory_order_relaxed);
    writer.join(); // hangs forever iff the lock deadlocks
    for (auto& th : readers)
        th.join();

    if (writerOps.load() == 0) {
        printf("FAIL: writer starved (deadlock?)\n");
        return 1;
    }
    if (readerOps.load() == 0) {
        printf("FAIL: readers starved (deadlock?)\n");
        return 1;
    }
    printf("term_lock: no deadlock — writer=%ld iters, readers=%ld locked reads\n", writerOps.load(), readerOps.load());

    // Lock-overhead on the read path. The mouse/pointer handlers take this
    // lock per event; it must stay in the tens-of-ns range so input latency
    // is unaffected. (Skip the hard bound under TSan, where it is inflated.)
    const long N = 2000000;
    long sink = 0;
    auto p0 = std::chrono::steady_clock::now();
    for (long i = 0; i < N; ++i) {
        std::scoped_lock lk(g_termLock);
        sink += wantsMouseReports(t) ? 1 : 0;
    }
    auto p1 = std::chrono::steady_clock::now();
    const double nsPer = std::chrono::duration<double, std::nano>(p1 - p0).count() / (double)N;
    printf("term_lock: %.1f ns per locked read (sink=%ld)%s\n", nsPer, sink, UNDER_TSAN ? " [TSan]" : "");
    if (!UNDER_TSAN && nsPer > 500.0) {
        printf("FAIL: locked read path too slow (%.1f ns; expected < 500)\n", nsPer);
        return 1;
    }

    printf("test_term_lock: all passed\n");
    return 0;
}
