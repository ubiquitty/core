<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Parser benchmark sensitivity to code placement

A parser benchmark can change when code placement changes even if the executed
algorithm does not. Repeated runs of one executable measure run-to-run
variation; they do not measure variation between independently linked
executables.

## Measurement contract

For a small parser optimization:

1. Keep the corpus, compiler, optimization flags, profile data, link mode, and
   machine conditions fixed. Record the executable identity for every sample.
2. Interleave baseline and candidate runs. Retain individual results rather
   than only a best run.
3. Compare multiple independently built baseline and candidate executables
   when the claimed change is comparable to build-to-build variation.
4. Inspect the generated code and a profile to establish that the proposed
   mechanism affects the workload.
5. Include unrelated control workloads. A broad shift after a cold-code edit
   is evidence to investigate placement, not proof that every hot algorithm
   improved.

Do not claim a universal percentage noise floor. The relevant uncertainty is
the measured variation for that workload, toolchain, machine, and build.
If baseline and candidate distributions overlap, report the result as
unresolved rather than selecting the favorable executable.

## Diagnosis

Function and loop alignment, link order, inlining, LTO, and profile-guided
layout can change instruction placement. A throughput change alone does not
identify branch-predictor aliasing, instruction-cache behavior, or any other
specific cause. Record the disassembly and counters needed to distinguish
those explanations.

An alignment flag that does not change one result does not establish that
placement is irrelevant. Likewise, a cold-path edit that changes a hot
benchmark does not establish a semantic regression.

Use the parse scenarios in `Tests/Performance/bench_core.cpp` with the
[Core measurement procedure](../measuring-a-core-change.md). Keep conclusions
about one executable separate from product-wide performance claims.
