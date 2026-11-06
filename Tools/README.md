<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Tools

Programs a person runs by hand: demonstrations, diagnostics and development
helpers. Gates and build scripts live in [`Scripts/`](../Scripts/README.md);
tests live in [`Tests/`](../Tests/README.md). Each tool's header says what it
is for and how to call it.

| Tool | Purpose | Command |
| --- | --- | --- |
| [`showcase/`](showcase/README.md) | The feature tour, one section per area, and the catalog the portable snapshot suite renders | `python3 Tools/showcase/showcase.py` |
| `showcase/Examples/evolution.py` | The first-launch show: the history of the terminal, one era per chapter | `python3 Tools/showcase/Examples/evolution.py` |
| `showcase/Examples/new_year.py` | Countdown, fireworks and a greeting | `python3 Tools/showcase/Examples/new_year.py` |
| `showcase/Examples/vms-shell.py` | A VAX/VMS DCL shell drawn with period VT220/VT320 sequences | `python3 Tools/showcase/Examples/vms-shell.py` |
| `showcase/shades-demo.sh` | Shade dithering and eighth-block progress bars | `Tools/showcase/shades-demo.sh [--no-anim]` |
| `showcase/gen_arabic_reference.py` | Regenerates the shaped-Arabic reference image the Fonts section shows (needs PIL with raqm) | `python3 Tools/showcase/gen_arabic_reference.py` |
| `demo/` | terminal_velocity: a nine-part escape-code demo whose frame rate measures the terminal's drain | `Tools/demo/run.sh [--part N] [--fps N] [--bench]` |
| `snapshot/` | Source of `ubiquitty-snapshot`, the portable Core and software-renderer snapshot driver | `Scripts/build-core-snapshot.sh` |
| `approve-golden.py` | Marks a golden as checked by eye so nothing re-mints it | `Tools/approve-golden.py <golden>... --note "..."` |
| `box-stroke-alignment.py` | Measures where a rendered box-drawing rule changes place or weight in a PNG | `Tools/box-stroke-alignment.py IMAGE [--pitch W H]` |
| `covering-tests.py` | Lists, and with `--run` runs, the tests covering what this branch changed | `Tools/covering-tests.py [--since REF] [--run]` |
| `host-tests.py` | Builds and runs a subset of host tests in this checkout's build root | `Tools/host-tests.py 'PATTERN'` |
| `ios-ui-test.py` | Runs one iOS XCUITest case without the whole battery | `Tools/ios-ui-test.py <TestCase>` |
| `merge-pr.py` | Rebase-merges a pull request and cancels the runs it leaves behind | `Tools/merge-pr.py [NUMBER] [--dry-run]` |
| `readme-screenshots.py` | Prepares and renders the README specimens into a staging directory, writes the remote screenshots' demo HOME (from `readme_demo_home.py`), and checks a capture's tab marks against its trace | `Tools/readme-screenshots.py --output-dir DIR [--render --binary APP]`, `--demo-home DIR`, `--check-status TRACE_LOG EXPECTED` |
| `remint-fingerprint.py` | Records a Sync wire body's new fingerprint after its version moved | `Tools/remint-fingerprint.py [VERSION-NAME]` |
| `ui-scenarios.py` | Runs a chosen set of macOS UI scenarios from one build | `Tools/ui-scenarios.py PREFIX \| --regex RE \| --list` |
| `font-diagnose.cpp` | Prints Core's font-route diagnostic for each code point of a string; compiled with the tests | `font-diagnose "TEXT"` |
| `font-route-bench.cpp` | Counts native cascade calls when a Han run routes through the fallback policy; compiled with the tests | `font-route-bench [UTF8-FILE]` |
| `gen-proportional-strike/` | Bakes Noto Sans into the bitmap tier's proportional face | see the file header (runs in the Linux container) |
| `pty-probe/` | Measures the macOS PTY transport with no parser attached | `Tools/pty-probe/run.sh` |
| `reflow-bench/` | Reflow and throughput measurement against the portable Core | `Tools/reflow-bench/run.sh` |
| `sgr-profile/` | gprof harness for `processBytes` on SGR-heavy streams | `Tools/sgr-profile/run.sh` |
| `seam-bench/` | Cost of one glyph-atlas seam crossing in C++ and Swift, from a Swift harness | `Tools/seam-bench/run.sh` |
| `classify-bench/` | Cost of bulk per-code-point work in C++ and Swift, from a Swift harness | `Tools/classify-bench/run.sh` |
| [`bench/`](bench/README.md) | Points to the performance harnesses in `Tests/Performance` and the ubench generator | none |
