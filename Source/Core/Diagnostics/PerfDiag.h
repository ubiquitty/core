// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
// Compile-time gate for hot-path render-diagnostic instrumentation.
// UBIQUITTY_PERF_DIAG=1 keeps the full set live; =0 expands every hot-path macro
// to (void)0. Hang-critical tags (wne-*, sleep-*, signal) live OUTSIDE this gate
// — they call HaltJournal_Log directly so a wedge dump works in any build.

// Default OFF in NDEBUG builds, ON in Debug. Pass -DUBIQUITTY_PERF_DIAG=1 to
// force it on in a release build for profiling.
#ifndef UBIQUITTY_PERF_DIAG
#ifdef NDEBUG
#define UBIQUITTY_PERF_DIAG 0
#else
#define UBIQUITTY_PERF_DIAG 1
#endif
#endif

// The hot-path PERF_* taps expand to no-ops; the macOS/host/agent frontends
// carry their own perf logging (UBIQUITTY_PERFLOG stderr) rather than these
// per-cell counters.
#define PERF_NOW() ((uint32_t)0)
#define PERF_JOURNAL(tag, detail) ((void)0)
#define PERF_PARSE_US(us) ((void)0)
#define PERF_DRAW_US(us) ((void)0)
#define PERF_FLUSH_US(us) ((void)0)
#define PERF_WNE_US(us) ((void)0)
#define PERF_KEY_US(us) ((void)0)
#define PERF_CELLS(n) ((void)0)
#define PERF_DRAW_CHAR(n) ((void)0)
#define PERF_GLYPH_MASK(n) ((void)0)
#define PERF_ROWS_DRAWN(n) ((void)0)
#define PERF_FULL_REDRAW(n) ((void)0)
#define PERF_DIRTY_REDRAW(n) ((void)0)
#define PERF_FLUSH_COUNT(n) ((void)0)
#define PERF_FLUSH_ROWS(n) ((void)0)
#define PERF_BLANK_RUN(n) ((void)0)
#define PERF_BLANK_CELLS(n) ((void)0)
#define PERF_GRAPHIC(n) ((void)0)
#define PERF_EMOJI(n) ((void)0)
