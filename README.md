<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Ubiquitty Core

**The terminal engine and persistent multiplexer behind Ubiquitty.**

Embed the portable engine, build your own terminal UI, or keep shells running in
`ubiquitty-host` and attach from your apps, scripts and agents. Core owns the
terminal model, parser, input encoding, screen history and render plan. It needs
no window system, GPU or UI toolkit. The host enforces capability tokens, attach
consent and command policies for CLI and MCP clients.

[Embed Core](Docs/libubiquittycore.md) · [Use the host and CLI](Docs/ubq.md) · [Get the apps](https://ubiquitty.org/)

[![Recorded terminal compatibility](https://ubiquitty.org/reports/compatibility.svg)](https://ubiquitty.org/performance/#conformance)

Recorded tests cover terminal controls and Unicode. [Current results and scope](https://ubiquitty.org/performance/#conformance).

**The fastest parser in our comparative benchmarks, as of 7 November 2026.** [Conformance scope](https://github.com/ubiquitty/ubiquitty#conformance) · [Benchmark methods](https://github.com/ubiquitty/ubiquitty#benchmarks)

Legacy terminal software renders correctly. Core supports DEC rectangular
editing, selective erase, protected cells, cell checksums, national character
sets, double-width and double-height lines.
Modern protocols include Kitty keyboard input, animated Kitty graphics, Sixel,
iTerm2 images, OSC 8 hyperlinks and synchronized output.

Extended DECPS adds eight musical channels, waveform voices, WAV samples,
envelopes, filters, reverb and tracker effects. Programs can use it for
instruments, sequencers and game audio; the Mac app supplies playback.
[Sound protocol](https://ubiquitty.org/features/sound/).

## Choose your layer

| You are building | Reach for | Repository |
|---|---|---|
| A C++ host with its own UI, text system or renderer | libubiquitty's C++ session facade and Core headers | [ubiquitty-core](https://github.com/ubiquitty/core) |
| Persistent sessions, a session daemon, attach and reattach across machines | the multiplexer: `ubiquitty-host`, Sync, the `ubq` command and the agent | [ubiquitty-core](https://github.com/ubiquitty/core) |
| A terminal view to drop into a macOS or iOS app | UbiquittyKit's `TerminalView` or SwiftUI `Terminal` (Metal) | [ubiquitty-kit](https://github.com/ubiquitty/kit) |
| A terminal view in a Linux app, or a software-rendered surface | the Vulkan, FreeType and software renderers and the GTK platform layer | [ubiquitty-kit](https://github.com/ubiquitty/kit) |
| Core's renderer drawing into a texture or layer your app owns | UbiquittyRenderKit (Metal) or the Vulkan renderer's exportable frames | [ubiquitty-kit](https://github.com/ubiquitty/kit) |
| The Ubiquitty apps themselves | the macOS, iOS, Linux and web frontends | [ubiquitty](https://github.com/ubiquitty/ubiquitty) |

Ubiquitty is three repositories. Each depends only on the ones below it:
`ubiquitty` → `kit` → `core`. This is the bottom one.

## What this repository holds

**libubiquitty** is the terminal itself: a portable library with no window-system,
GPU or UI-toolkit dependency. It parses a program's bytes, keeps the screen,
history, modes and selection, encodes keys and mouse input, answers the
program's queries, and produces the render plan (resolved text runs, fills and
overlays) a renderer draws. Platform services come in through the PAL contract
(`Source/PAL/PAL.h`); the bundled POSIX backend supplies the rest. A host that
draws for itself embeds it directly: link the CMake target `ubiquitty-core`
and read [Embedding the portable terminal Core](Docs/libubiquittycore.md). The
C++ session facade (`Source/UbiquittyKit`, not the `ubiquitty-kit` repository)
drives sessions, live configuration and local or remote transports.
Its factories require an explicit `Session::EffectPolicy` choice. Use
`EffectPolicy::withPolicy(policy, origin)` with `RestrictiveCapabilityPolicy`
for grid content and normal replies without host-escaping or exfiltrating
effects; the [embedding guide](Docs/libubiquittycore.md#capability-policy)
explains policy selection and lifetime.
The [workspace embedding facade](Docs/embedding-workspaces.md) supplies the
host's pane replicas, workspace actions and directory to an independent client.

**The multiplexer** keeps shells alive between connections and lets any number
of clients attach to them, locally or across machines:

- `ubiquitty-host`, the session daemon. It owns the PTYs, the terminal state and
  the workspace, session and pane identities, and listens on a local socket and,
  when built with QUIC, a QUIC endpoint (`Source/Platform/Host`).
- Sync, the wire protocol between the host and its clients
  ([Sync](Docs/sync.md), [wire reference](Docs/sync-protocol.md)). A client
  sends input and applies state; it owns no session.
- The agent and `ubq`, the headless Sync client, command line and MCP server for
  scripts and AI agents ([ubq](Docs/ubq.md), `Source/Agent`).

Core owns everything a program can observe about its terminal, replies and modes
included. Renderers, terminal views and apps live in the repositories above.

## Correctness and performance

The [current recorded comparison](https://ubiquitty.org/performance/#conformance)
covers terminal controls and Unicode. The badge above follows its latest
published [summary data](https://ubiquitty.org/reports/compatibility.json).

A capability-query case can pass by correctly reporting an unsupported mode;
that query pass does not claim implementation of the mode itself.

**The fastest parser among the engines and workloads in our comparative runs, as of 7 November 2026.** See [benchmarks and measurement scope](https://github.com/ubiquitty/ubiquitty#benchmarks). Parser throughput, rendering, input latency and energy are separate measurements.

## Build

Needs Git, Python 3, CMake with C++23 support, Ninja or Make, Bash and a C++23
compiler. The first QUIC-enabled build of the host fetches and builds its QUIC
dependencies, which needs network access, Go and Perl.

```sh
git clone https://github.com/ubiquitty/core.git
cd core
```

```sh
Scripts/build-host.sh     # ubiquitty-host, the session daemon
Scripts/build-agent.sh    # ubq, the headless client and MCP server
Scripts/build-tests.sh    # compiles every test, runs none
```

Build output goes outside the checkout; `python3 Scripts/build_root.py .` prints
its root, and `UBQ_BUILD_ROOT` overrides it. The libraries and the consumer
sample:

```sh
BUILD="$(python3 Scripts/build_root.py .)/core"
cmake -S . -B "$BUILD" -G Ninja -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_QUIC=OFF
cmake --build "$BUILD" --target core-consumer
"$BUILD/core-consumer"
```

To compile Core inside another CMake project, include the modules under
`Scripts/cmake` from the project's top directory, in the order
`Scripts/cmake/UbiquittyCoreRoot.cmake` lays out. Core is consumed as source, so
link-time optimization and the training profile apply to the consumer's whole
program.

## Test

```sh
Scripts/install-hooks.sh   # the shell suite checks that the gating hooks are installed
BUILD="$(python3 Scripts/build_root.py .)/BuildHostTests/ctest"
cmake -S . -B "$BUILD" -G Ninja -DUBIQUITTY_FRONTEND=Core \
    -DUBIQUITTY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure
```

A `test_*.cpp` file anywhere under `Tests/` is a CTest case; no list names it.
`ctest --test-dir "$BUILD" -N` lists them and `-R '^test_name$'` runs one. The
[test guide](Tests/README.md) describes the suites. On a machine without
Linux, `Scripts/run-linux.sh` runs the same build and tests in a container.

Read [the architecture](Docs/architecture.md) and
[the coding conventions](Docs/coding-conventions.md) before changing behavior,
and [Contributing](.github/contributing.md) before opening a pull request.

## License

Apache-2.0; see [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt),
[NOTICE](NOTICE) and [the licensing guide](Docs/licensing.md). "Ubiquitty" and
the Ubiquitty logo are trademarks of Stanislav Yaglo; see
[TRADEMARKS.md](TRADEMARKS.md).
