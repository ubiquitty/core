<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Benchmark tools

Public performance harnesses live in [`Tests/Performance`](../../Tests/Performance/).
See [the measurement guide](../../Docs/benchmarks.md) for regression baselines,
energy measurements, and requirements for publishing a comparison.

The scenario generator is the separate ubench repository;
[`Scripts/run-parse-scenarios.sh`](../../Scripts/run-parse-scenarios.sh) builds
its streams from a `UBENCH_DIR` checkout.
