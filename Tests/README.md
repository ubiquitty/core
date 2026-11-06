<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Test suites

The normal host-buildable suite is the primary correctness gate:

```sh
./Scripts/run-tests.sh
```

The [script index](../Scripts/README.md) lists the test runners, harnesses, and
gates under `Scripts/`.

The suite needs CMake, Ninja, a C++23 compiler and standard library, and
Python 3; see [Common
prerequisites](../Docs/platform-builds.md#common-prerequisites). The QUIC
stanzas also need the QUIC dependency archives, whose first build needs
network access, Go, and Perl. Without them those stanzas report SKIP, which is
not a pass.

`Tests/` mirrors `Source/`: the test for `Source/<path>/Foo.cpp` lives in
`Tests/<path>/`, so a source path derives its test path and nothing has to be
searched for. Adding a `test_*.cpp` anywhere under `Tests/` registers it with
CTest; there is no list to append to.

Tests of the Apple frontends' Swift are Swift, and the tree holds no
Objective-C. A Swift test that needs only the file it pins, such as
`Tests/Platform/test_macos_lastresort_guard.swift` or
`test_drawable_persistence.swift`, is a `run-tests.sh` stanza built with plain
`xcrun swiftc` beside that file; a C++ suite that needs one Foundation answer,
such as `test_ios_protected_file.cpp`, gets it from a Swift probe behind a C
symbol. The GPU and language benches with Swift hosts are
`Tests/Performance/bench_shaders.swift` (driven by
`Scripts/bench-shader-ab.sh`), `Tools/seam-bench/` and `Tools/classify-bench/`.

Build products and coverage output live outside the checkout, under
`$(python3 Scripts/build_root.py .)/BuildHostTests/`. Set `UBQ_BUILD_ROOT` to
choose a different build root.

The full driver includes C++ unit tests and shell integration stanzas. List or
select stanzas with `./Scripts/run-tests.sh --list-stanzas` and
`./Scripts/run-tests.sh <name>`. CTest registers integration stanzas as
`shell_<name>`; excluding them does not run the full suite. A missing required
dependency can produce a skipped test, which is not a pass.

Under CTest the Host frontend and `ubq` are each built once, by the
`host_binary` and `agent_binary` stanzas, and every other stanza only reads
them. A stanza is ordered after those fixtures when its own body asks for the
binary through `ubq_host_binary`, `ubq_agent_host_binary`, or
`ubq_agent_binary`. A stanza given a binary any other way is not, and under
CTest the helper refuses to build for it, because a build there would relink
the binary while other stanzas run it. Run by name from a shell, a stanza
builds what it needs.

The portable snapshot renderer is built the same way, by
`snapshot_renderer_fixture` ahead of `shell_snapshots`. Under CTest the
`snapshots` stanza refuses a renderer older than its sources instead of
rebuilding it within its timeout. `./Scripts/run-tests.sh snapshots` rebuilds a
stale renderer before it renders.

## Seeded Sync stress

```sh
UBQ_SYNC_STRESS_SEED=1 Scripts/run-tests.sh sync_stress_mix
```

This stanza drives one in-process host through `ScreenPlaneAccess` and three
or four canonical `ScreenClient` connections in a temporary `UBIQUITTY_HOME`.
It mixes fresh attachments, detach/resume cycles, resizes, typed PTY input,
OSC 66 proportional runs and measure queries, and palette changes on an
unwatched pane. Kitty image 77, placement 9, is replaced every 100 ms.
Every quiet point compares all replicas with the host screen, including the
palette and run metadata; a fresh attachment also checks the dormant pane.
Every decoded frame witnesses image placement continuity and rejects
`Bye{Malformed}`. The quiet-point comparison also checks image pixels.

`UBQ_SYNC_STRESS_SECONDS` defaults to 60 (range 1–300),
`UBQ_SYNC_STRESS_CLIENTS` to 4 (3 or 4), and `UBQ_SYNC_STRESS_RSS_MIB` to 64
(range 1–1024). The RSS bound applies to peak growth of the combined host and
client process after warming each operation. The seed selects the same actions,
arguments and client order independently of machine speed; each tick is logged.
An overrun fails the time budget instead of omitting actions. This is a daemon
and protocol harness test; it does not exercise an external daemon reactor or
network carrier. CTest discovers it as `shell_sync_stress_mix`.

## In-memory Sync regressions

```sh
./Scripts/check-sync-memory.py
```

This focused gate builds and runs explicitly reviewed binaries covering
canonical admission, envelopes, reconnect/resume, checkpoint allocation refusal,
rich text, wire fingerprints, adoption/reclaim transactions, live and restored
checkpoint storage admission, exact-capacity text and document owners, handover
of those owners, and mocked child cleanup failures. Storage regressions cover
zero, exact, and insufficient allowances; failure of individual preparation
allocations; destination growth; and preservation of prior pictures and history.
The cleanup tests create no child processes. The gate runs named binaries
directly: no CTest build fixtures, Host processes, PTYs, sockets,
or tmux sessions are started. Each binary has a 60-second timeout and must
report a nonempty passing suite; a failed build never runs stale binaries.
Tests receive an empty temporary configuration directory, leaving the user's
configuration out of the run.

Use `--list` to inspect the source allowlist, `--jobs 2` to limit compilers, or
`--build-dir /path/outside/checkout` for a dedicated build directory. The default
is `BuildHostTests/sync-memory` under this checkout's derived build root.
An existing cache owned by another checkout is refused, never deleted.

The allowlist and its transitive fixtures need review before expansion. This
gate does not replace transport, device, live-host, or full-suite acceptance.

## Real Sync transport acceptance

`Scripts/check-sync-transport.py` is a separate, opt-in runner for real carriers.
It starts its own foreground host and deterministic PTY peer, then drives the
production remote-session client through attach, typed input, offscreen history
retrieval, Kitty pixels and placement, PTY resize, suspension/reconnect, input
after reconnect, and image deletion. It checks that the session and pane survive
reconnect; it does not exercise a native UI or establish iPhone lifecycle or
network-roaming coverage.

Build the excluded-by-default acceptance target in a dedicated directory. It
requires the Host frontend, QUIC, and a Debug build:

```sh
transport_build="$(mktemp -d /tmp/ubq-sync-transport-build.XXXXXX)"
cmake -S . -B "$transport_build" -G Ninja \
    -DUBIQUITTY_FRONTEND=Host -DUBIQUITTY_QUIC=ON \
    -DCMAKE_BUILD_TYPE=Debug \
    -DUBIQUITTY_BUILD_SYNC_TRANSPORT_ACCEPTANCE=ON \
    -DUBIQUITTY_BUILD_TESTS=OFF -DUBIQUITTY_BUILD_SSH_PACKAGE=OFF
cmake --build "$transport_build" --parallel 4 --target sync_transport_acceptance
python3 Scripts/check-sync-transport.py \
    --binary "$transport_build/sync_transport_acceptance" --transport quic
```

The local command tests QUIC only, on loopback, using a fixture-owned bootstrap
and temporary certificate. Python 3, OpenSSL, and the configured C++ build tools
must be available. Build dependencies may need network access. Runtime host
state, configuration, cache, control sockets, and keys live in fresh temporary
directories; no installed host, user configuration, or existing session is used.

Real SSH requires the disposable Linux container: OpenSSH invokes an account's
login shell even for a forced command, so using the developer's account could
run personal startup files. The runner refuses SSH outside its marked container
and dedicated fixture account. Build the provided image using the pinned `gtk`
base from `Scripts/linux-images.txt`, then mount only the checkout, read-only:

```sh
transport_base="$(sed -n 's/^gtk=//p' Scripts/linux-images.txt)"
docker build --build-arg BASE_IMAGE="$transport_base" \
    -t ubiquitty-sync-transport - < Tests/Host/sync-transport.Dockerfile
docker run --rm \
    --mount "type=bind,src=$(pwd),dst=/src,readonly" \
    ubiquitty-sync-transport
```

The container builds outside the checkout and runs both SSH and QUIC by default.
It creates ephemeral SSH host/client keys, a private known-hosts file, and a
loopback-only SSH listener; password authentication, agent forwarding, user RC
files, and connection sharing are disabled. No ports are published and no
credential directories are mounted. Use `--transport ssh` or `--transport quic`
with the runner to select one carrier; a single-carrier result does not claim
both passed.

Each client run has a timeout. PTY peers exit through fixture-owned control
sockets, and cleanup signals only verified, unreaped direct children; no process
name search or process-group kill is used. The runner is not part of the in-memory
gate or default CTest execution. A missing isolation prerequisite is reported as
a refusal, never replaced with a simulated pass.

### Seeded reconnect soak

The same opt-in binary and private QUIC fixture can run repeated carrier
interruptions. The default soak is 200 iterations; an explicit count makes a
shorter replay. Each iteration prints its seed and exact receive-byte cut,
checks the resumed screen and completion-effect receipts, and reports the
private daemon's descriptor count and resident size.
The same numbered flood input is replayed after resume: input lost before its
write must complete, while input already written must not execute twice.

```sh
python3 Scripts/check-sync-transport.py \
    --binary "$transport_build/sync_transport_acceptance" \
    --transport quic --soak --seed 1
```

Replay a failure with the same master seed and a count reaching its first
failing iteration, for example `--soak 17 --seed 1`. Resource growth is checked
after 20 warmup iterations: at most two additional descriptors and 16 MiB of
RSS by default. `--max-rss-growth-kib` selects the RSS allowance. These bounded
observations detect growth during this run; they do not establish absence of
all leaks. The soak exercises loopback QUIC stream interruption and resume;
it does not simulate IP roaming, SSH, or a daemon binary handover.

Add `--two-clients` to keep a second, independently identified client attached
to the same pane. Its carrier never reconnects. Every iteration also requires
its picture and cursor to match, each completion effect to arrive once, its
attachment to stay live and unchanged, and its effect receipts to remain
monotonic and agree with the resumed client after both settle.

The daemon-replacement soak uses that same binary with two production session
owners, separate panes, and the real `--ensure-daemon` handover. It replaces a
private copy at the same install path on each cycle, checks the new socket and
host identities, automatically resumed attachments, full flood pictures,
completion receipts, and scrollback fetched after evicting the client cache.
It defaults to 50 cycles and prints each seed. Private supervisor connections
own detached-daemon cleanup; no PID file or process-name search authorizes a
signal. Resource samples start checking growth after ten warmup cycles, allowing
two additional descriptors and 16 MiB of RSS. The raw samples include inherited
socket listeners.

```sh
python3 Scripts/check-daemon-replacement.py \
    --binary "$transport_build/sync_transport_acceptance" \
    --iterations 50 --seed 1 --report /tmp/replacement-soak.json
```

The stopped-instance UI variant is separately opt-in. It stops its private
predecessor without a handover, starts the current helper, and requires the
existing window to keep its workspace and pane, restored on the current daemon
under other session, tab and pane numbers, without closing and without a second
window; the directory to retire the old instance; and a new workspace to use
the current daemon.

```sh
UBIQUITTY_TEST_DAEMON_REPLACEMENT=1 Scripts/run-ui-tests.sh \
    connect_vanished_daemon_restores_workspace_and_opens_unpinned
```

Neither check is part of the default suite; both keep their normal failure
exit status and assertions.

## Coverage

```sh
./Scripts/run-coverage.sh              # build, run, and produce a per-file table
./Scripts/run-coverage.sh --gaps       # least-covered files first
./Scripts/run-coverage.sh --check 84   # fail below a floor
HTML=1 ./Scripts/run-coverage.sh       # per-file drill-down
```

It instruments the Core and the tests together, which is what lets llvm-cov
attribute a Core line to the test that ran it.

## Terminal snapshots and showcase regressions

`Tests/Snapshots` is the single visual and reply-regression harness. It combines
focused terminal cases with every `Tools/showcase` entry:

```sh
# Catalog and committed-baseline integrity; no application build required.
python3 Tests/Snapshots/run.py --check-catalog

# Portable Core + SoftwareRenderer catalog on macOS or Linux.
./Scripts/build-core-snapshot.sh
python3 Tests/Snapshots/run.py --backend portable

# Portable plus CoreText/Metal catalogs on macOS.
./Scripts/build-macos.sh --no-helpers
./Scripts/run-tests.sh snapshots
```

Portable goldens use the configured GNU Unifont family and are byte-stable
across supported hosts. Native goldens exercise the actual platform font and GPU
stack. See [the snapshot guide](Snapshots/README.md) for the required workflows for fixing a
rendering bug, adding regression coverage, introducing renderer features, and
reviewing baseline changes.

## Fuzzing

The bounded parser, image, and Sync fuzz targets run in the Linux battery:

```sh
./Scripts/run-tests.sh --with-fuzz
```

`FUZZ_REQUIRED=1` turns a missing fuzzer toolchain into a failure. Seed corpora
live beside their targets and are replayed before guided exploration. Corpus
builders are checked in with the seeds so regeneration remains deterministic.

## Diagnostic and test variables

These environment variables exist for measurement and for tests. None is a
setting: several draw wrong frames by design, and a pinned clock stops
every blink. The variables meant for people are documented beside their
features: `UBIQUITTY_HOME` and the agent variables in [ubq](../Docs/ubq.md#environment),
`UBIQUITTY_TERM` in
[Terminal identity](../Docs/guides/terminal-identity.md), the clipboard
helpers' variables in
[Clipboard tools](../Docs/guides/selection-clipboard-and-search.md#clipboard-tools-in-hosted-sessions),
and the Linux and wscons ones in [platform builds](../Docs/platform-builds.md).

| Variable | Read in | Effect |
|---|---|---|
| `UBIQUITTY_FAKE_TICKS` | every build, Apple clock and frame tick | Pins the monotonic tick to this number, so cursor blink and other time-derived render state match across processes. The offscreen snapshot path and `Scripts/check-core-builder-frames.sh` set it. |
| `UBIQUITTY_GRID_CACHE` | every build, renderer | `0` or `off` rebuilds every row on every frame; `1`, `on`, or unset keeps the retained grid. `Scripts/check-grid-cache-differential.py` and `Scripts/energy-scenarios.py` compare the two. |
| `UBIQUITTY_BUILDER_ABLATE` | every build, `buildCellInstances` | A bit mask of instance-building phases to skip, for `Scripts/ablate-sweep.py`. Any nonzero mask draws wrong frames. |
| `UBIQUITTY_PERFLOG` | every build, frontends | Prints `[perf]` frame-timing lines per half-second window; `Source/Core/Diagnostics/FramePerfLog.h` lists the columns. |
| `UBIQUITTY_METRICS_LOG` | every build, renderer | Prints the inputs of the curly underline and the link colour as `metrics-log` lines. |
| `UBIQUITTY_DUMP_LIGATURE_INSTANCES`, `UBIQUITTY_DUMP_SKIP_INK`, `UBIQUITTY_DUMP_GLYPH_ROW` | every build, Apple renderer | Print the ligature strip, the underline rules cut around ink, or the glyph row after each instance build. |
| `UBIQUITTY_PHASE_LOG` | every build, Apple renderer | Any value prints one `[phase]` line per live frame: the origin, advance, scale and drawable size each glyph's subpixel phase derives from, with a drift from the previous frame flagged. |
| `UBIQUITTY_NO_OCCLUSION_SKIP` | every build, Apple renderer | Any value makes a pane under DECSET 7402's opaque letterbox drain its damage and copy its grid like any other pane, for a pixel-parity comparison. |
| `UBIQUITTY_PASS_GPU` | every build, Apple renderer | `1` samples GPU timestamps around the grid and drawable passes and prints a `passgpu` line of their times per frame, on a device that samples at stage boundaries. |
| `UBIQUITTY_SHAPING_FILTER`, `UBIQUITTY_SHAPING_FILTER_VERIFY` | every build, Apple renderer | `0` on the first turns the shaping candidate filter off; `1` on the second compares its answers with CoreText's. The filter's contract is in `Source/Core/Rendering/OpenTypeShapingFilter.h`. |
| `UBIQUITTY_HARFBUZZ_IDENTITIES`, `UBIQUITTY_HARFBUZZ_VERIFY` | builds with `UBIQUITTY_HARFBUZZ_PROBE` | `1` on the first shapes single-face ASCII runs with HarfBuzz rather than CoreText; `1` on the second also shapes each run with CoreText and logs `uniform-shaping` mismatch counts. |
| `UBIQUITTY_ADAPTIVE_FPS` | every build, Apple surfaces | `0` holds the display link continuous instead of following how often the screen changes, which is what a bench's fixed-rate arms measure. |
| `UBIQUITTY_SKIP_UNCHANGED_FRAMES` | every build, macOS surfaces | `0` presents on every display-link callback, including those nothing asked a frame for, so both policies can be measured in one binary. |
| `UBIQUITTY_FRAME_TRACE` | every build, Apple surfaces | `1` writes a `[frame-trace]` line to standard error at each stage of a repaint: whether a frame was owed, withheld or presented, and the pane's frame counters. |
| `UBIQUITTY_TRACE_FRAME_LINK` | every build, macOS surfaces | `1` logs the display link's requested rate, callback rate and per-callback work once a second, and its state when it stops. |
| `UBIQUITTY_METAL_HUD` | every build, macOS app | A value not starting with `0` sets `MTL_HUD_ENABLED=1` before any Metal device exists, so the driver draws its per-frame overlay. A `MTL_HUD_ENABLED` already set wins. |
| `UBIQUITTY_NATIVE_TABBAR` | every build, macOS app | Any value leaves the tab strip as AppKit draws it, with no titlebar material behind it and no themed tab titles. |
| `UBIQUITTY_TAB_ACCESSORY_TRACE` | every build, macOS app | `1` writes each tab-accessory decision to standard error; see [tab status](../Docs/tab-status.md). |
| `UBIQUITTY_IOS_OPEN_HOST` | every build, iOS app | `first`, or a saved host's UUID, opens that host at launch, as choosing its row would. |
| `UBIQUITTY_SWIFT_FLOOD` | every build, `TerminalSession` | `0` keeps a flood on the reader loop's sequential passes rather than the flood pipeline, so a run can be paired against itself in one binary; `Source/Core/Runtime/IngestLoop.h` describes both paths. |
| `UBIQUITTY_DELIVERY_STREAM`, `UBIQUITTY_DELIVERY_LIMIT` | every build, macOS `--bench-deliveries` | The recording the delivery bench replays (default `/tmp/ubqstreams/agent_tmux_140x36.stream`, written by `Scripts/record-stream.py`), and how many of its deliveries each lane replays (default 4000). |
| `UBIQUITTY_BENCH_COMMIT` | `bench_base64`, `bench_damage_walk`, `bench_ingest_flood` | The commit those benches write into their JSON results, which the collector keys its time series by. |
| `UBQ_PERFLOG` | every build, Apple surfaces | Any value prints the `[perf] atlas capacity` line `UBIQUITTY_PERFLOG` also prints, without the frame-timing lines. |
| `UBQ_FONTLOG` | every build, macOS app and Apple surfaces | Any value logs each font change applied to the windows (how many it reached and skipped, and the feature string), and for the two frames after a surface takes new faces whether each reused its retained target and how many rows it planned and redrew. |
| `UBQ_SHEETLOG` | every build, Apple renderer | Any value logs the face, feature string and ligature participation of each ligature sheet built, and prints a `[perf] sheet` line for every frame in which a glyph sheet refused a glyph or is crowded, with a `healthy` line every 240 frames otherwise. |
| `UBQ_STRIPLOG` | every build, Apple renderer | Any value logs each ligature strip as it is drawn: its characters, left context, cells drawn, pen position and glyphs. |
| `UBQ_RASTER_BATCHLOG` | every build, Apple renderer | Any value prints a `[perf] raster-batch` line with the glyphs and draw calls of each batched raster. |
| `UBQ_PROBE_NO_STRETCH` | every build, Apple renderer | Any value measures the box-drawing stroke profile without the family's centring shift and 9-slice stretch, so the profile describes the font's glyphs rather than what reaches the screen. |
| `UBQ_REUSEWHY` | every build, Core frame damage | Any value prints a `[reusewhy]` line naming each eligibility term whenever a frame's damage cannot reuse its retained target. |
| `UBQ_LIVE_SSH_HOST`, `UBQ_LIVE_SSH_USER`, `UBQ_LIVE_SSH_KEY`, `UBQ_LIVE_SSH_PORT`, `UBQ_LIVE_WORKSPACE` | every build, iOS app launched with `--seed-live-host` | Save a QUIC host with that address, user, private key text and port (default 22) before the host list loads, for the live lane of `Scripts/run-ios-ui-tests.sh`; the last names the workspace it attaches to. Without the launch argument they are not read. |
| `UBIQUITTY_POINTER_LOCK_TRACE` | every build, Linux | Prints which pointer-lock backend engaged or why it refused, and the relative motion delivered. |
| `UBIQUITTY_PARTIAL_FRAME_DEADLINE_MS` | every build, daemon | Replaces the 10-second deadline for a stalled partial frame, read once at startup; zero or junk keeps 10 seconds. `adopt_in_parts` and `partial_frame_budget` set it. |
| `UBIQUITTY_MODEL_LOCK_BITE` | UI-test builds | Bytes parsed per model-lock hold, clamped to 256 through one read, for the contention sweep. |
| `UBIQUITTY_RESUME_TEST_KEYCHAIN` | UI-test builds | A directory that holds the remote-resume descriptors in place of the keychain. |
| `UBIQUITTY_UI_TEST_HOST`, `UBIQUITTY_DISPOSABLE_GUI_SESSION`, `UBIQUITTY_UI_TEST_RESUME_*`, `UBIQUITTY_UI_TEST_RESTART_*`, `UBIQUITTY_UITEST_DWELL`, `UBIQUITTY_RESIZE_CENSUS_PROGRAM`, `UBIQUITTY_RESIZE_ANCHOR_CAPTURES`, `UBIQUITTY_PASTE_PANEL_CAPTURE`, `UBIQUITTY_INSPECTOR_SNAPSHOT` | UI-test builds, scenarios | `Scripts/run-ui-tests.sh` passes the host binary and the disposable-session marker; a scenario that relaunches the app sets the phase, marker and root for the run it starts. The rest name where a scenario writes a capture, the program a resize census runs, or how many seconds a scene stays on screen for a person watching. |
| `UBQ_QUIC_PROBE_*`, `UBQ_HOST_LABELS_PHASE`, `UBQ_HOST_CHIP_CAPTURE`, `UBQ_SETTINGS_RENDER_DIR`, `UBQ_SETTINGS_RENDER_IMAGE`, `UBQ_UI_PICTURES`, `UBQ_UI_PICTURE_FOREGROUND`, `UBQ_PRINT_GOLDEN`, `UBQ_WRITE_RASTERS` | UI-test builds, scenarios | The first name the isolated daemon, SSH host and carrier the carrier probe drives; the rest name the phase or file a scenario writes its pictures under, or ask a scenario to print its golden table or write its rasters for a reader. |
| `UBIQUITTY_TEST_ATOMIC_SUFFIX`, `UBIQUITTY_TEST_TEMP_SUFFIX` | builds with `UBIQUITTY_ENABLE_TEST_HOOKS` | Fix the temporary name of an atomic write or a temporary file, so `test_atomic_storage_perms` can plant a symlink there and check it is refused. |
