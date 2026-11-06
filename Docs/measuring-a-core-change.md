<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Measuring a Core change

Use a focused benchmark to test a specific performance hypothesis. Keep the
workload and correctness checks public, and report the configuration actually
measured. For app-level behavior, see [measuring the live app](measuring-live-app.md);
for publication requirements, see [benchmarks](benchmarks.md).

## Select the workload

Choose an existing group in `Tests/Performance/bench_stack.cpp` that exercises the
changed path, or add a representative workload. Verify that the selected group
produces benchmark rows: a successful process exit or a `machine_control` result
alone does not prove that the intended work ran.

Build and run a focused, non-PGO measurement:

```sh
nice -n 19 env UBQ_BUILD_JOBS=1 UBIQUITTY_PGO=OFF \
  Scripts/build-perf-bench.sh bench_stack
bench_root="$(python3 Scripts/build_root.py .)"
nice -n 19 "$bench_root/BuildBench/bench_stack" --only render
```

`--only` matches group names such as `render`, `parse`, `snapshot`,
`scrollback`, `reflow`, `tmux`, `kitty`, and `sync`. Record the exact filter,
iterations, geometry, input corpus, and resulting binary hash.

Match the workload to the mechanism. Sparse and dense grids, incremental damage
and full rebuilds, ASCII and mixed-width text, and warm and cold caches exercise
different work. A workload that cannot enter the changed branch is a control,
not evidence of the branch's performance.

## Build comparable binaries

Use isolated build outputs for baseline and candidate. Keep the compiler,
optimization flags, dependencies, workload, and machine configuration fixed.
Save both binaries before changing the source or build configuration.

PGO is an experimental variable. A source change can invalidate profile counters;
a retrained profile can alter unrelated functions. Either disable PGO for both
binaries and report that fact, or use an appropriate profile for each revision.
Validate a profile change on workloads independent of its training set.
A profile applying without warnings does not establish a performance gain.

Measure correctness before timing: run the relevant unit, replay, and rendering
checks. A faster result that omits required work is a regression.

## Profile separately from timing

On macOS, Instruments requires a full Xcode installation:

```sh
nice -n 19 Scripts/profile-lane.sh render
nice -n 19 Scripts/profile-lane.sh parse --symbol "processBytesImpl<false>"
```

Set `UBQ_BENCH` to profile a particular saved binary. The first command reports
self and inclusive samples; the second attributes samples within a symbol.
Self samples identify a function's own work. Inclusive samples include its
callees. Neither is an elapsed-time measurement.

Use a separate uninstrumented run for timing. Record enough samples to support
attribution, and retain the raw trace. Instruction counts, code size, and static
assembly changes are explanations to test, not substitutes for timing.

For allocation changes:

```sh
nice -n 19 Scripts/count-allocations.sh --groups delta
nice -n 19 Scripts/count-allocations.sh --binary /tmp/bench_base --groups delta
```

This macOS interposer counts process-wide allocation traffic, including runtime
startup. Compare equivalent runs; do not interpret the total as allocations
owned only by the changed function.

## Compare with controls

```sh
nice -n 19 Scripts/bench-ab.py /tmp/bench_base /tmp/bench_candidate \
  --lane render_build_full --only-tags render,parse,snapshot --pairs 24
```

The selected groups must include the subject and the harness's control lanes.
The harness interleaves runs and reports paired ratios, control-normalized
results, and best observed throughput. Retain the individual observations.
A disagreement between summaries is a reason to investigate, not to select the
more favorable number.

Measure the null floor with identical binaries and the same configuration
using `--null-floor`. Do not reuse a noise threshold from a different machine
or workload. Controls can detect some shared drift; they cannot remove
uncorrelated load, thermal effects, or all memory-system noise.

Separately linked binaries also differ in code placement. For a small,
localized change, a diagnostic binary containing both paths with a runtime
selector can help isolate the mechanism. Keep the selector outside the timed
loop and verify both paths against the same correctness tests. This diagnostic
does not replace measurements of the release configuration.

Run timing experiments on a quiet machine with stable power and thermal state.
Do not overlap them with builds, regression suites, or screenshot rendering.
Repeat comparisons and report unresolved results when variation obscures the
effect.

## Keep the claim within the measurement

A Core replay excludes PTY transport, frontend scheduling, and GPU presentation.
A CPU instance-builder benchmark cannot establish a GPU improvement. An isolated
shader pass does not include the costs of a complete frame. Use the relevant
live-app or rendering instrument for those claims.

Report the source revisions, binary hashes, compiler and SDK, flags and profiles,
hardware and OS, workload, raw repetitions, correctness checks, and uncertainty.
Do not extrapolate synthetic throughput into typing latency, battery life, or
a ranking against another terminal.
