<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# ubiquitty-core

| You are building | Reach for | Repository |
|---|---|---|
| A C++ host with its own UI, text system or renderer | libubiquitty's C++ session facade and Core headers | [ubiquitty-core](https://github.com/ubiquitty/ubiquitty-core) |
| Persistent sessions, a session daemon, attach and reattach across machines | the multiplexer: `ubiquitty-host`, Sync, the `ubq` command and the agent | [ubiquitty-core](https://github.com/ubiquitty/ubiquitty-core) |
| A terminal view to drop into a macOS or iOS app | UbiquittyKit's `TerminalSurface` (Metal) | [ubiquitty-kit](https://github.com/ubiquitty/ubiquitty-kit) |
| A terminal view in a Linux app, or a software-rendered surface | the Vulkan, FreeType and software renderers and the GTK platform layer | [ubiquitty-kit](https://github.com/ubiquitty/ubiquitty-kit) |
| Core's renderer drawing into a texture or layer your app owns | UbiquittyRenderKit (Metal) or the Vulkan renderer's exportable frames | [ubiquitty-kit](https://github.com/ubiquitty/ubiquitty-kit) |
| The Ubiquitty apps themselves | the macOS, iOS, Linux and web frontends | [ubiquitty](https://github.com/ubiquitty/ubiquitty) |

Ubiquitty is three repositories. Each depends only on the ones below it:
`ubiquitty` → `ubiquitty-kit` → `ubiquitty-core`. This is the bottom one.

## What this repository holds

**libubiquitty** is the terminal itself: a C++23 library with no window-system,
GPU or UI-toolkit dependency. It parses a program's bytes, keeps the screen,
history, modes and selection, encodes keys and mouse input, answers the
program's queries, and produces the render plan (resolved text runs, fills and
overlays) a renderer draws. Platform services come in through the PAL contract
(`Source/PAL/PAL.h`); the bundled POSIX backend supplies the rest. A host that
draws for itself embeds it directly: link the CMake target `ubiquitty-core`
and read [Embedding the portable terminal Core](Docs/libubiquittycore.md). The
C++ session facade (`Source/UbiquittyKit`, not the `ubiquitty-kit` repository)
drives sessions, live configuration and local or remote transports.

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

## Build

Needs Git, Python 3, CMake with C++23 support, Ninja or Make, Bash and a C++23
compiler. The first QUIC-enabled build of the host fetches and builds its QUIC
dependencies, which needs network access, Go and Perl.

```sh
git clone https://github.com/ubiquitty/ubiquitty-core.git
cd ubiquitty-core
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
