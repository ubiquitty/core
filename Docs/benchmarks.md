<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Performance measurements

Performance checks measure distinct parts of the terminal: parser throughput,
input latency, rendering and presentation, memory, and energy. Record which part
is measured and keep correctness checks enabled for the workload.

## Portable regression checks

[`Scripts/bench-gate.sh`](../Scripts/bench-gate.sh) runs the portable `bench_stack`
harness against machine-specific baselines in the external directory selected
by `UBIQUITTY_PERF_BASELINE_DIR`. It covers parsing,
snapshot serialization and application, scrollback, and channel loopback.

Run it on a quiet machine. These baselines track Ubiquitty's performance over
time on the same hardware. Read the script's options before choosing a baseline
or recording one.

Keep captured baselines, profiles, and performance journals outside the product
repository. The portable and native profile gates,
[`Scripts/profile-gate.sh`](../Scripts/profile-gate.sh) and
[`Scripts/render-profile-gate.sh`](../Scripts/render-profile-gate.sh), use the same directory
variable. An update requires an explicit directory; `bench-gate.sh` also requires
`UBQ_BASELINE_REASON` to identify why the reference changed.

[`Tests/Performance/`](../Tests/Performance/) contains the public harnesses and
workloads. Use the build and run instructions beside each harness. Core-only
measurements exclude the cost of platform shaping, GPU submission, and display
presentation.

## Native rendering and energy

A native measurement identifies the renderer, application revision, display
refresh rate, scale, terminal geometry, font, and whether the window is visible
and focused. Report input-to-presentation latency separately from byte ingestion.

[`Scripts/energy-scenarios.py`](../Scripts/energy-scenarios.py) records CPU and
power observations for Ubiquitty's documented energy scenarios. Comparisons
between revisions require identical input bytes, replay timing, geometry, and
rendering settings. A private session recording cannot serve as a public
reproduction workload. Use synthetic or explicitly shareable input when
publishing measurements.

## Publishing a comparison

A public result includes the workload, harness source and revision, exact commands,
terminal versions, relevant configuration, hardware and OS, correctness criteria,
and raw results from repeated runs. Report variation and disclose exclusions.
Input throughput does not establish lower input latency or lower energy use.
