// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Tests/Performance/Profiler.h — a distribution-capturing latency profiler for the
// cross-platform performance work.
//
// BenchHarness.h answers "how fast on average" (throughput, median + p95). This
// answers "what does the distribution look like" — it keeps every per-call
// sample and reports mean/p50/p90/p95/p99/min/max/stddev in microseconds per op.
// Render frames, protocol bursts, snapshot apply, tmux deliver: their tail
// (p99/max) is what's felt as a stall, and you profile a change by diffing two
// runs' distributions, not one averaged number. Lower is better (latency).
//
// Header-only, dependency-free (just <chrono>), so a profiler links only the
// portable code it measures. Every platform's profiler emits one JSON shape;
// Scripts/compare-profile.py diffs two of them.
//

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace Ubiquitty::Profiler {

// One operation's latency distribution, in microseconds per call.
struct Distribution {
    std::string name;
    std::size_t samples = 0;
    double mean = 0.0;
    double p50 = 0.0;
    double p90 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double min = 0.0;
    double max = 0.0;
    double stddev = 0.0;
};

// The percentile at fraction `q` (0..1) of an ASCENDING-sorted vector, by
// nearest-rank — no interpolation, so a reported value is always a real
// observed sample.
inline double percentile(const std::vector<double>& sortedAscending, double q)
{
    if (sortedAscending.empty())
        return 0.0;
    const std::size_t last = sortedAscending.size() - 1;
    std::size_t rank = static_cast<std::size_t>(std::lround(q * static_cast<double>(last)));
    if (rank > last)
        rank = last;
    return sortedAscending[rank];
}

// Run `fn` `warmup` times (untimed) to settle caches/branch predictors, then
// Build a Distribution from already-collected per-call microsecond samples —
// for a caller that measures its own phase timings (e.g. the render profiler
// splitting a frame into snapshot/encode/GPU). Sorts `microseconds` in place.
inline Distribution fromMicros(const std::string& name, std::vector<double>& microseconds)
{
    Distribution d;
    d.name = name;
    d.samples = microseconds.size();
    if (microseconds.empty())
        return d;

    double sum = 0.0;
    for (double v : microseconds)
        sum += v;
    d.mean = sum / static_cast<double>(microseconds.size());

    double variance = 0.0;
    for (double v : microseconds) {
        const double delta = v - d.mean;
        variance += delta * delta;
    }
    d.stddev = std::sqrt(variance / static_cast<double>(microseconds.size()));

    std::sort(microseconds.begin(), microseconds.end());
    d.min = microseconds.front();
    d.max = microseconds.back();
    d.p50 = percentile(microseconds, 0.50);
    d.p90 = percentile(microseconds, 0.90);
    d.p95 = percentile(microseconds, 0.95);
    d.p99 = percentile(microseconds, 0.99);
    return d;
}

// `iters` timed, recording each call's wall time. Returns the distribution in
// microseconds per call.
inline Distribution profile(const std::string& name, int warmup, int iters, const std::function<void()>& fn)
{
    for (int i = 0; i < warmup; ++i)
        fn();
    std::vector<double> microseconds;
    microseconds.reserve(static_cast<std::size_t>(iters > 0 ? iters : 1));
    for (int i = 0; i < iters; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto end = std::chrono::steady_clock::now();
        microseconds.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    }
    return fromMicros(name, microseconds);
}

// Emit all distributions as one JSON object (machine + per-op stats, µs) to
// stdout. Scripts/compare-profile.py keys these by name to diff two runs.
inline void emitJSON(const std::vector<Distribution>& results, const char* platform, const char* commit)
{
    std::printf("{\n");
    std::printf("  \"platform\": \"%s\",\n", platform ? platform : "");
    std::printf("  \"commit\": \"%s\",\n", commit ? commit : "");
    std::printf("  \"unit\": \"us\",\n");
    std::printf("  \"profiles\": [\n");
    for (std::size_t i = 0; i < results.size(); ++i) {
        const Distribution& d = results[i];
        std::printf("    {\"name\": \"%s\", \"samples\": %zu, \"mean\": %.4f, "
                    "\"p50\": %.4f, \"p90\": %.4f, \"p95\": %.4f, \"p99\": %.4f, "
                    "\"min\": %.4f, \"max\": %.4f, \"stddev\": %.4f}%s\n",
            d.name.c_str(), d.samples, d.mean, d.p50, d.p90, d.p95, d.p99, d.min, d.max, d.stddev, (i + 1 < results.size()) ? "," : "");
    }
    std::printf("  ]\n}\n");
}

// A human-readable table to stderr (so JSON on stdout stays clean for a pipe).
inline void report(const std::vector<Distribution>& results)
{
    std::fprintf(stderr, "%-28s %8s %9s %9s %9s %9s %9s\n", "profile (µs/op)", "samples", "p50", "p90", "p95", "p99", "max");
    std::fprintf(stderr, "%-28s %8s %9s %9s %9s %9s %9s\n", "----------------------------", "-------", "--------", "--------", "--------", "--------", "--------");
    for (const Distribution& d : results) {
        std::fprintf(stderr, "%-28s %8zu %9.3f %9.3f %9.3f %9.3f %9.3f\n", d.name.c_str(), d.samples, d.p50, d.p90, d.p95, d.p99, d.max);
    }
}

} // namespace Ubiquitty::Profiler
