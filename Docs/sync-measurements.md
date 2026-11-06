<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Synchronization measurement harnesses

Deterministic replay checks a fixed corpus and implementation. Keep captured
measurement tables and machine-specific PTY timings outside the product
repository. Regenerate byte counts when the encoding changes.

## Replay method

`Tests/Corpus/sync` contains PTY streams split into reads with timestamps and
would-block markers. `Tests/LoopbackPaneLink.h` replays them through the host's
drain/coherence rule and applies the transmitted state to a passive terminal.
Every admitted frame is compared with the host's screen and history.

The host replay's drain passes end at would-block or
`TerminalRuntime::drainPassCapacityBytes`, not at a short read. The elapsed
presentation budget for armed local displays does not apply to this headless
replay. A publish records its class:

| Class | Meaning |
|---|---|
| boundary | taken after the program closed a 2026 block |
| settled | ordinary output that reached a would-block |
| flood | taken coherent after a drain pass that spent its byte budget |
| forced | taken while incoherent: the parser mid-sequence, or a 2026 block expired on its budget |
| structural | the first frame, or a geometry change |

The first applicable class in structural, forced, flood, boundary, settled order
wins. "In ?25l bracket" counts frames captured while a newly opened cursor-hidden
redraw bracket remains open. "Inside a split redraw" uses generator-marked
synthetic redraw boundaries. Hold time starts at the first dirtying drain after
the previous frame; staleness starts at host publication and ends when the client
holds that frame or a newer one.

Reproduce the corpus and shaped-link reports from the public tree:

```sh
B="$(python3 Scripts/build_root.py .)/BuildHostTests/ctest"
cmake -S . -B "$B" -G Ninja -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$B" --target test_sync_corpus_oracle test_sync_shaped_link --parallel 1
UBQ_SYNC_CORPUS_REPORT=1 "$B/Tests/test_sync_corpus_oracle" theWireKeepsEveryCorpusClientOnItsHost
UBQ_SYNC_CORPUS_REPORT=1 "$B/Tests/test_sync_shaped_link"
```

The clock is the replay's. Current reports must be identified by their checkout,
corpus, options, and encoding version when used as new evidence.

## Input and liveness

This heading and the constants table are read by `test_sync_measured_policy`.
These values define the current session-layer policy. The policies are `measuredInputPolicy`,
`measuredLivenessPolicy`, `measuredResumePolicy`, and `measuredGeometryPolicy` in
`Source/Core/Protocol/Sync/Connection.cpp`.

### The constants

| constant | value | purpose |
|---|---|---|
| `E` | 50 ms | echo settle bound |
| `receipt delay` | 20 ms | standalone receipt delay |
| `stall multiple` | 4× | round-trip multiplier for detecting a stalled path |
| `stall floor` | 750 ms | minimum stall interval |
| `re-path timer` | 10 s | provisional path replacement delay |
| `Lost grace` | 10 min | provisional reconnect grace |
| `geometry coalesce` | 50 ms | provisional resize coalescing interval |
| `paste window` | 256 KiB | bound on unwritten input (`maxPendingInputBytes`) |
| `silence` | 30 s | provisional pacing silence bound |

The shaped-link harness is `test_sync_shaped_link`. The screen and history
reports are emitted by `test_sync_corpus` and `test_sync_history` when their
`UBQ_SYNC_CORPUS_REPORT` and `UBQ_SYNC_HISTORY_REPORT` variables are set. Preserve
results with the source revision, corpus, link settings, and toolchain in the
external measurement archive.

## Memory admission

This heading and the constants table are read by `test_sync_memory_owner`. The
values are the defaults of `ClientPolicy` and `measuredHostPolicy` in
`Source/Core/Protocol/Sync/Connection.h` and `Connection.cpp`, and of
`defaultMaxHostDecodeBytes` in `HandoverBody.h`. Docs/sync.md §7.9 describes
what each owner charges.

### The largest legal checkpoint

`test_sync_checkpoint_ceiling` fills both screens of a terminal at the largest
legal geometry (`maxRows` × `maxColumns`, 1024 × 1024) with the content that
lengthens encoded rows most, and encodes the checkpoint the host would send.
Measured with `UBQ_CHECKPOINT_CEILING_FULL=1 UBQ_PRINT_CHECKPOINT_CEILING=1` on
Linux 7.2 x86_64 (16 cores, GCC 16.2, Debug build of the Core test tree) with
the checkpoint encoding this document describes; the body length does not
depend on the machine.

| content in every cell of both screens | checkpoint body |
|---|---|
| a distinct pen (bold, italic, underline, strike, true-colour fg/bg/underline), astral code point | 15.16 MiB |
| a hyperlink with a 200-byte URI and its own id | 3.28 MiB |
| a base letter and 12 combining marks | 15.90 MiB |
| a distinct pen and 12 combining marks | 29.51 MiB |
| alternating pens with links and clusters | 13.33 MiB |

Dictionaries are capped (65,535 pens, 4,096 links or 1 MiB, 4,096 long
clusters per epoch), so cell references dominate. Rich text is bounded by its
own caps: 256 blocks per screen, each at most 4,096 code points and 512 runs,
under 13 MiB for both screens. The worst legal body is therefore below
43 MiB, and the cap is the next power of two above twice that.

### The constants

| constant | value | rationale |
|---|---|---|
| `maxCheckpointBytes` | 128 MiB | client transfer cap; equal to the host's, so the client takes every body the host may send |
| `hostMaxCheckpointBytes` | 128 MiB | host cap; above twice the worst legal body, so only a defect is unbound with `CheckpointTooLarge` |
| `maxCheckpointStorageCost` | 128 MiB | one checkpoint's preparation and destination grid peak (unchanged, now pinned) |
| `maxDecodedScreenCellBytes` | 32 MiB | the two decoded screen-cell arrays of one checkpoint (unchanged, now pinned) |
| `maxCatalogMessageBytes` | 72 MiB | image transfers, samples, deferred effects and clipboard payloads in flight; the host's per-client outbound cap derives from it |
| `maxInstalledImageBytes` | 256 MiB | installed pixels summed over every pane: one terminal's image budget |
| `maxReceiveBytes` | 776 MiB | the receive owner: one checkpoint deflated and inflated (256 MiB), its preparation (128 MiB), installed images (256 MiB), the catalog (72 MiB), and 64 MiB for history, result and structure assemblies |
| `historyCacheBytes` | 64 MiB | one pane's history cache: 10,000 lines at 200 columns of 32-byte cells |
| `maxRetainedReplicaBytes` | 512 MiB | every retained picture's grid and scrollback plus every history cache: eight panes at the history cap |
| `maxHostDecodeBytes` | 512 MiB | every unpublished adopt, reclaim and handover candidate: two at the host's 256 MiB checkpoint allowance |
