// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Tests/Performance/BenchHarness.h — a tiny portable benchmark harness for the
// automated cross-platform performance harness.
//
// Times a closure over a warmup + N timed iterations and reports the MEDIAN +
// p95 (never the mean — a terminal's tail latency is what bites), then emits the
// results as one JSON object with run metadata. Every platform's bench binary
// uses this, so Scripts/run-bench.sh collects one comparable JSON shape from each
// runner. Header-only, dependency-free (just <chrono>), so a bench links only the
// portable code it measures.
//

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Bench {

// Set only on the training driver's translation unit. Core keeps the shipping
// build flags, and ordinary benchmark binaries keep their adaptive timing.
#if defined(UBIQUITTY_PGO_TRAINING) && UBIQUITTY_PGO_TRAINING
constexpr bool isProfileTraining = true;
#else
constexpr bool isProfileTraining = false;
#endif

// ---- Optimiser barriers ------------------------------------------------
//
// A benchmark whose result nothing reads is a benchmark the optimiser may
// delete. Nothing in this tree stopped that: a lane that repeats a pure pass
// over an unchanging corpus and discards the answer can legally be folded to one
// pass, or to none, and it then reports a throughput figure for work that did
// not happen. The failure is silent and reads as a very fast lane.
//
// These are the two primitives Google Benchmark exposes for it, spelled for
// Clang/GCC. Both are empty asm: they emit no instruction and cost no cycle, and
// exist only to constrain what the compiler may assume.

/// Makes `value` observable, so the work that produced it survives.
///
/// The `"+r,m"` constraint lets the value stay in a register where it fits and
/// fall back to memory where it does not, which is what keeps this free: the asm
/// is empty and emits no instruction. Only the compiler's assumptions change.
///
/// - Parameter value: The result a lane must not have discarded.
template<typename T>
inline void doNotOptimize(T& value)
{
#if defined(__clang__)
    asm volatile("" : "+r,m"(value) : : "memory");
#else
    asm volatile("" : "+m,r"(value) : : "memory");
#endif
}

/// Marks every memory location as possibly written.
///
/// Stores made before it cannot be sunk past it or removed. Needed when a lane's
/// work lands in memory it never reads back — a parser filling an out-parameter,
/// or a decoder writing into a terminal.
inline void clobberMemory()
{
    asm volatile("" : : : "memory");
}

// ---- Tick counter ------------------------------------------------------
//
// The virtual counter, read directly. Two back-to-back reads are apart by the
// cost of the `mrs` itself, which is the real floor on measuring anything.
// `steady_clock` is backed by the same counter, so this buys no resolution; it
// buys a tick count with no library between the read and the register, which is
// what a lane reports when it wants cycles rather than seconds.
#if defined(__aarch64__) || defined(_M_ARM64)
/// Reads the virtual counter.
///
/// - Returns: The current tick, whose rate ``tickFrequency`` reports.
inline std::uint64_t tickCounter()
{
    std::uint64_t value;
    asm volatile("mrs %0, CNTVCT_EL0" : "=r"(value));
    return value;
}

/// The counter's rate.
///
/// - Returns: Ticks per second.
inline std::uint64_t tickFrequency()
{
    std::uint64_t value;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(value));
    return value;
}
#else
/// Reads the steady clock, where no architectural counter is exposed.
///
/// - Returns: The current tick, whose rate ``tickFrequency`` reports.
inline std::uint64_t tickCounter()
{
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

/// The steady clock's rate.
///
/// - Returns: Ticks per second.
inline std::uint64_t tickFrequency()
{
    return static_cast<std::uint64_t>(std::chrono::steady_clock::period::den);
}
#endif

// One measured metric. `unit`/`value` are whatever the bench reports (e.g.
// "MiB/s" throughput, "ns" per op); `p95` is the same unit at the 95th
// percentile (for throughput that's the SLOWER tail, computed from p95 time).
struct Result {
    std::string name;
    std::string unit;
    double value = 0.0;
    double p95 = 0.0;
};

struct Timing {
    double medianSeconds = 0.0;
    double p95Seconds = 0.0;
};

// The clock's resolution on this hardware, rather than an assumed one.
//
// `steady_clock` is derived from `mach_absolute_time`, whose timebase sets the
// tick below. Every interval it can report is an integer multiple of it.
//
// It is the RESOLUTION, not the cost of reading. A short region is not inflated
// by a fixed overhead — it is ROUNDED, and a lane whose true cost is a fraction
// of a tick reports zero or one tick depending on where the tick boundary fell.
constexpr double clockTickSeconds = 125.0 / 3.0 * 1.0e-9;

// The duration a timed region is grown to before it is trusted.
//
// At 10 us one tick is 0.4% of the reading, so quantisation is below the noise of
// anything else in the measurement. Regions are repeated up to this length rather
// than merely reported, because a warning leaves the number in the output where it
// gets quoted.
constexpr double shortestTrustworthyCallSeconds = 10.0e-6;

/// How many times a region must repeat before quantisation stops mattering.
///
/// Calibrated from an observed single-call duration rather than fixed: the same
/// count would over-repeat a millisecond lane into a minute-long run and
/// under-repeat a nanosecond one into the same quantised reading.
///
/// - Parameters:
///   - observedSeconds: One call's duration, from the warmup.
///   - warmup: The warmup count; zero means nothing was observed.
/// - Returns: The repeat count, at least 1.
inline int repeatsToClearClock(double observedSeconds, int warmup)
{
    // No warmup means no observation, and a repeat count from no observation is a
    // guess. It is also the shape a one-shot lane takes — `measure(0, 1, ...)` for
    // an operation that only happens once, like settling a resize — where the
    // second call and every one after it does nothing and the reported cost is the
    // real one divided by the repeat count. The call site already says this; the
    // harness reads it rather than asking to be told again.
    if (warmup <= 0)
        return 1;
    if (observedSeconds >= shortestTrustworthyCallSeconds)
        return 1;
    // A warmup that read ZERO ticks says the call is under one tick and gives no
    // scale to divide by. Start from the tick as the upper bound on its cost:
    // that is the smallest duration the instrument could not distinguish from it.
    const double floor = observedSeconds > 0.0 ? observedSeconds : clockTickSeconds;
    const double needed = shortestTrustworthyCallSeconds / floor;
    if (needed > 1.0e6)
        return 1000000; // a lane this fast is pathological; cap rather than hang
    const int repeats = static_cast<int>(needed) + 1;
    return repeats > 1 ? repeats : 1;
}

/// A count of how often the code under test was actually reached.
///
/// A lane that never enters the branch it is comparing reports two binaries
/// agreeing, which reads exactly like a change that costs nothing. The usual
/// causes are a knob the harness never set, a session that never armed the
/// feature, and a path no bench drives, and in each the numbers look ordinary
/// while meaning nothing.
///
/// So a lane whose point is a particular path counts its arrivals and hands the
/// count here. Zero is a hard error rather than a warning: the figure beside it
/// is not a slower or faster answer, it is an answer to a different question.
///
/// Costs nothing in the timed region — the counter is the lane's own variable,
/// and this is called once, after.
///
/// - Parameters:
///   - name: The lane the count belongs to, as it appears in the results.
///   - what: The path that was supposed to be taken, named as a reader would
///     look for it in the source.
///   - arrivals: How many times it was reached across the whole lane.
/// - Returns: Whether the lane measured what it claims to.
[[nodiscard]] inline bool witnessPathTaken(const char* name, const char* what, long long arrivals)
{
    if (arrivals > 0)
        return true;
    std::fprintf(stderr,
        "bench: %s measured nothing: %s was never reached.\n"
        "  A lane that does not enter the path it compares reports both arms agreeing,\n"
        "  which is indistinguishable from a change that costs nothing. Fix the lane —\n"
        "  arm the feature, set the knob, or drive the caller — before reading the number.\n",
        name ? name : "?", what ? what : "the path under test");
    return false;
}

/// Records that a region was repeated to clear the clock, once per call site.
///
/// Printed because it changes what the lane measured: repeated calls share warmed
/// caches and a settled branch predictor that a single cold call does not, so a
/// repeated region reports the steady-state cost rather than the first-call cost.
/// A reader comparing against an older number needs to know which one they have.
///
/// - Parameters:
///   - repeats: Calls per timed region.
///   - observedSeconds: The warmup duration the count was calibrated from.
///   - file: The `measure` call site's file.
///   - line: The `measure` call site's line.
inline void noteRegionRepeated(int repeats, double observedSeconds, const char* file, unsigned line)
{
    static std::set<std::pair<std::string, unsigned>> reported;
    if (!reported.insert({ std::string(file ? file : "?"), line }).second)
        return;
    std::fprintf(stderr,
        "bench: %s:%u repeats %d calls per timed region (one call read %.3f us, under the %.0f us floor).\n",
        file ? file : "?", line, repeats, observedSeconds * 1.0e6, shortestTrustworthyCallSeconds * 1.0e6);
}

/// Warns when a region is still short enough to be quantised after repeating.
///
/// Reached two ways, and both need saying: a lane that opted out with
/// `Repeat::Once`, and one so fast the repeat count hit its cap. Either way the
/// figure is a tick multiple, and the number of ticks is the useful part — at one
/// tick the lane cannot distinguish any two costs at all.
///
/// - Parameters:
///   - medianSeconds: Median seconds per call after division.
///   - repeats: Calls per timed region.
///   - file: The `measure` call site's file.
///   - line: The `measure` call site's line.
inline void warnIfStillQuantised(double medianSeconds, int repeats, const char* file, unsigned line)
{
    const double regionSeconds = medianSeconds * repeats;
    if (regionSeconds <= 0.0 || regionSeconds >= shortestTrustworthyCallSeconds)
        return;
    static std::set<std::pair<std::string, unsigned>> reported;
    if (!reported.insert({ std::string(file ? file : "?"), line }).second)
        return;
    std::fprintf(stderr,
        "bench: WARNING - %s:%u times a %.3f us region, which the clock resolves to "
        "%.1f tick(s) of %.1f ns.\n"
        "bench:           The reading is quantised: repeat the region or accept that it "
        "cannot separate two costs closer than one tick.\n",
        file ? file : "?", line, regionSeconds * 1.0e6, regionSeconds / clockTickSeconds,
        clockTickSeconds * 1.0e9);
}

/// Times `fn` and reports the median and p95 wall time PER CALL, in seconds.
///
/// `warmup` untimed calls settle caches and branch predictors, then `iters` timed
/// samples are taken and sorted.
///
/// A region shorter than the clock can resolve is REPEATED inside the timing
/// brackets until it clears `shortestTrustworthyCallSeconds`, and the sample is
/// divided by the repeat count. The reported unit is unchanged — still one call —
/// but the number stops being a tick multiple. Warning instead would leave the
/// quantised figure in the output, where it gets quoted.
///
/// The repeat count comes from the last warmup call, so a lane that got faster or
/// slower is re-calibrated rather than carrying a constant somebody chose once.
/// `clobberMemory` runs after each call: a lane whose work lands in memory it
/// never reads back is otherwise free for the optimiser to sink or drop, and every
/// lane pays this whether or not its author thought about it.
///
/// A callable whose work per call CHANGES when it is repeated — one that grows a
/// buffer, or consumes from a queue — must not be repeated. Such a lane passes
/// `Repeat::Once` and says why at the call site.
///
/// Profile training always calls once per iteration, including when a caller
/// explicitly asks to clear the clock. Elapsed time must not change the corpus's
/// weight. The observations remain timings, not evidence of a performance gain.
///
/// - Parameters:
///   - warmup: Untimed settling calls.
///   - iters: Timed samples.
///   - fn: The callable under test.
///   - repeat: Whether the harness may repeat a short region.
/// - Returns: Median and p95 seconds per single call.
enum class Repeat {
    ToClearClock,
    Once,
};

template<typename Fn>
Timing measure(int warmup, int iters, Fn&& fn, Repeat repeat = Repeat::ToClearClock,
    const char* file = __builtin_FILE(), unsigned line = __builtin_LINE())
{
    double lastWarmupSeconds = 0.0;
    for (int i = 0; i < warmup; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        clobberMemory();
        const auto end = std::chrono::steady_clock::now();
        lastWarmupSeconds = std::chrono::duration<double>(end - start).count();
    }

    const int repeats = !isProfileTraining && repeat == Repeat::ToClearClock ? repeatsToClearClock(lastWarmupSeconds, warmup) : 1;
    if (repeats > 1)
        noteRegionRepeated(repeats, lastWarmupSeconds, file, line);

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iters > 0 ? iters : 1));
    for (int i = 0; i < iters; ++i) {
        const auto start = std::chrono::steady_clock::now();
        for (int r = 0; r < repeats; ++r) {
            fn();
            clobberMemory();
        }
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double>(end - start).count() / repeats);
    }
    if (samples.empty())
        return {};
    std::sort(samples.begin(), samples.end());
    const std::size_t mid = samples.size() / 2;
    const std::size_t p95 = std::min(samples.size() - 1, static_cast<std::size_t>(samples.size() * 0.95));
    // Still checked after the repeat, because Repeat::Once opts out of the repeat
    // but not out of the quantisation, and a capped pathological lane can still
    // land short.
    warnIfStillQuantised(samples[mid], repeats, file, line);
    return { samples[mid], samples[p95] };
}

// Two variants timed against each other. `a` and `b` alternate within one
// iteration, and the leading side swaps each iteration, so drift between calls —
// clock frequency, thermals, a competing process — lands on both equally and
// neither holds the cold-cache position.
//
// `ratio` is the median of the per-iteration b/a ratios. The ratio of the two
// medians is a different number: it discards the pairing that cancels the drift.
struct PairedTiming {
    Timing a;
    Timing b;
    double ratio = 0.0; // median per-iteration b/a; > 1 means b is slower
};

// Templated on the callables, not std::function: a timed call through a
// type-erased wrapper cannot inline, and the per-call overhead is charged to
// whichever variant is faster.
template<typename A, typename B>
PairedTiming measurePaired(int warmup, int iters, A&& a, B&& b)
{
    const auto timeOnce = [](auto&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    for (int i = 0; i < warmup; ++i) {
        a();
        b();
    }
    std::vector<double> sa, sb, ratios;
    const std::size_t reserve = static_cast<std::size_t>(iters > 0 ? iters : 1);
    sa.reserve(reserve);
    sb.reserve(reserve);
    ratios.reserve(reserve);
    for (int i = 0; i < iters; ++i) {
        double ta, tb;
        if (i % 2 == 0) {
            ta = timeOnce(a);
            tb = timeOnce(b);
        } else {
            tb = timeOnce(b);
            ta = timeOnce(a);
        }
        sa.push_back(ta);
        sb.push_back(tb);
        if (ta > 0.0)
            ratios.push_back(tb / ta);
    }
    if (sa.empty())
        return {};
    const auto summarize = [](std::vector<double>& samples) {
        std::sort(samples.begin(), samples.end());
        const std::size_t mid = samples.size() / 2;
        const std::size_t p95 = std::min(samples.size() - 1, static_cast<std::size_t>(samples.size() * 0.95));
        return Timing { samples[mid], samples[p95] };
    };
    PairedTiming out;
    out.a = summarize(sa);
    out.b = summarize(sb);
    if (!ratios.empty()) {
        std::sort(ratios.begin(), ratios.end());
        out.ratio = ratios[ratios.size() / 2];
    }
    return out;
}

// A throughput Result (MiB/s) from a per-call timing over `bytesPerCall`.
inline Result throughput(const std::string& name, double bytesPerCall, const Timing& timing)
{
    const double mib = bytesPerCall / (1024.0 * 1024.0);
    Result r;
    r.name = name;
    r.unit = "MiB/s";
    r.value = timing.medianSeconds > 0.0 ? mib / timing.medianSeconds : 0.0;
    r.p95 = timing.p95Seconds > 0.0 ? mib / timing.p95Seconds : 0.0; // slower tail
    return r;
}

// Like measure() but keeps every per-call sample (seconds), not just the
// median/p95 — latency and frame-time metrics gate on the tail (p99/max), which
// a two-number summary discards.
template<typename Fn>
std::vector<double> measureSamples(int warmup, int iters, Fn&& fn)
{
    for (int i = 0; i < warmup; ++i)
        fn();
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iters > 0 ? iters : 1));
    for (int i = 0; i < iters; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double>(end - start).count());
    }
    return samples;
}

// The q-quantile (0..1) of sorted seconds, nearest-rank: element min(n-1, n*q)
// — the same convention measure() uses for p95.
inline double quantileSeconds(const std::vector<double>& sorted, double q)
{
    if (sorted.empty())
        return 0.0;
    return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(sorted.size() * q))];
}

// Append p50 + p99 latency Results in MILLISECONDS (lower-is-better) from raw
// per-iteration samples. `opsPerSample` amortizes the timer overhead: each
// sample timed `opsPerSample` units of work, so per-unit = sample/ops. unit
// "milliseconds" signals compare-bench.py the metric is lower-better — a
// latency rise gates as a regression.
inline void latencyMilliseconds(std::vector<Result>& out, const std::string& name, std::vector<double> samplesSeconds, double opsPerSample = 1.0)
{
    const double inv = opsPerSample > 0.0 ? 1.0 / opsPerSample : 1.0;
    for (double& s : samplesSeconds)
        s *= inv;
    std::sort(samplesSeconds.begin(), samplesSeconds.end());
    const double p95Milliseconds = quantileSeconds(samplesSeconds, 0.95) * 1.0e3;
    auto push = [&](const char* suffix, double q) {
        Result r;
        r.name = name + suffix;
        r.unit = "milliseconds";
        r.value = quantileSeconds(samplesSeconds, q) * 1.0e3;
        r.p95 = p95Milliseconds;
        out.push_back(r);
    };
    push("_p50_milliseconds", 0.50);
    push("_p99_milliseconds", 0.99);
}

// Append p99 + max frame-time Results in MILLISECONDS (lower-is-better) from raw
// per-frame samples — the "never stutter" tail (the mean hides a lone hitch).
// max is the single worst frame: report it, but it is too high-variance to gate
// at a fixed relative threshold, so exclude name+"_max_milliseconds" from the
// relative gate and hold it to an absolute --ceiling instead.
inline void frameTailMilliseconds(std::vector<Result>& out, const std::string& name, std::vector<double> framesSeconds)
{
    std::sort(framesSeconds.begin(), framesSeconds.end());
    const double p99Milliseconds = quantileSeconds(framesSeconds, 0.99) * 1.0e3;
    const double maxMilliseconds = (framesSeconds.empty() ? 0.0 : framesSeconds.back()) * 1.0e3;
    auto push = [&](const char* suffix, double value) {
        Result r;
        r.name = name + suffix;
        r.unit = "milliseconds";
        r.value = value;
        r.p95 = p99Milliseconds;
        out.push_back(r);
    };
    push("_p99_milliseconds", p99Milliseconds);
    push("_max_milliseconds", maxMilliseconds);
}

/// Returns how the binary emitting these numbers was compiled.
///
/// An unoptimised binary is indistinguishable from a slow one from the outside:
/// the same figure means opposite things at `-O0` and `-O3`, and a results file
/// carrying no trace of its own build type lets a number be quoted for hours as
/// a fact about the code while describing the compiler. This travels with the
/// numbers rather than with the reader's memory of how they configured the tree.
///
/// `NDEBUG` carries the most weight — asserts dominate the cost, every optimised
/// configuration sets it and no debug one does — with the optimisation level
/// beside it, because RelWithDebInfo and Release are both `NDEBUG` and a
/// shipping claim means only one of them.
///
/// `-O0 -DNDEBUG` is the case worth naming: asserts off, optimiser off, and it
/// reads as a release build to anything testing `NDEBUG` alone.
///
/// - Returns: A stable token naming the flavour, never null.
inline const char* buildFlavour()
{
#if defined(NDEBUG)
#if defined(__OPTIMIZE_SIZE__)
    return "optimised-size";
#elif defined(__OPTIMIZE__)
    return "optimised";
#else
    return "ndebug-unoptimised"; // asserts off, optimiser off — measures nothing real
#endif
#else
#if defined(__OPTIMIZE__)
    return "optimised-with-asserts";
#else
    return "debug";
#endif
#endif
}

/// Times a fixed synthetic workload no change to this tree can affect.
///
/// A delta is only a finding if it is larger than the machine moved underneath it,
/// and a run has no way to say how much that was. This lane is the reading: fixed
/// arithmetic over a fixed buffer, owned by the harness, touched by no product
/// code. Two runs' `machine_control` figures differ by exactly the amount the
/// machine drifted — frequency, thermals, another process on a core — so a lane
/// that moved less than the control did moved for no reason anyone can name.
///
/// Emitted from `emitJSON`, so a bench cannot forget it. A changed lane that moved
/// less than the control is a neutral change, not a small win.
///
/// It reads MACHINE drift only. It shares no cache footprint, branch history or
/// code layout with the lane under test, so it cannot see a regression caused by
/// the change moving other code around — for that the control has to be a real
/// lane in the same binary that the change does not touch.
///
/// - Returns: The control lane's result, in millions of accumulate steps a second.
inline Result machineControl()
{
    // Sized so its OWN spread is finer than the deltas it is there to arbitrate:
    // the referee has to be quieter than the thing it judges. A longer region
    // averages short-term jitter within one sample instead of showing it as
    // run-to-run spread.
    constexpr int elements = 4096;
    constexpr int passes = 240;
    static std::vector<std::uint32_t> buffer;
    if (buffer.empty()) {
        buffer.resize(elements);
        // A fixed, non-trivial pattern. A constant array folds; a random one makes
        // the lane differ between runs for a reason that is not the machine.
        for (int i = 0; i < elements; ++i)
            buffer[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(i * 2654435761u);
    }
    const Timing timing = measure(4, 24, [&] {
        std::uint32_t accumulator = 0;
        for (int pass = 0; pass < passes; ++pass) {
            for (int i = 0; i < elements; ++i)
                accumulator = accumulator * 31u + buffer[static_cast<std::size_t>(i)];
        }
        doNotOptimize(accumulator);
    });
    Result result;
    result.name = "machine_control";
    result.unit = "Mstep/s";
    const double steps = static_cast<double>(elements) * passes;
    result.value = timing.medianSeconds > 0.0 ? (steps / timing.medianSeconds) / 1.0e6 : 0.0;
    result.p95 = timing.p95Seconds > 0.0 ? (steps / timing.p95Seconds) / 1.0e6 : 0.0;
    return result;
}

// Emit all results as one JSON object (machine + per-benchmark metrics) to
// stdout. The collector keys these by commit for the time series.
inline void emitJSON(const std::vector<Result>& results, const char* platform, const char* commit)
{
    // Every run carries the control lane, appended here rather than asked of each
    // bench: a noise floor that a bench can forget to report is one that is missing
    // from exactly the run someone is trying to explain.
    std::vector<Result> withControl = results;
    withControl.push_back(machineControl());
    const std::vector<Result>& all = withControl;
    std::printf("{\n");
    std::printf("  \"platform\": \"%s\",\n", platform ? platform : "");
    std::printf("  \"commit\": \"%s\",\n", commit ? commit : "");
    std::printf("  \"build\": \"%s\",\n", buildFlavour());
    if constexpr (isProfileTraining)
        std::printf("  \"training_mode\": \"fixed-input-counts\",\n");
    std::printf("  \"benchmarks\": [\n");
    for (std::size_t i = 0; i < all.size(); ++i) {
        std::printf("    {\"name\": \"%s\", \"unit\": \"%s\", \"value\": %.4f, \"p95\": %.4f}%s\n", all[i].name.c_str(), all[i].unit.c_str(), all[i].value, all[i].p95, (i + 1 < all.size()) ? "," : "");
    }
    std::printf("  ]\n}\n");
}

} // namespace Ubiquitty::Bench
