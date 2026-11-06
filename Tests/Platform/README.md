<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Tests/Platform

Tests for the platform layer: the PAL implementations under `Source/PAL/` and
`Source/Platform/`, and the frontends and clients built on them. They link the
production sources rather than doubles of them.

The directory holds these kinds of test:

- **Portable PAL and POSIX backends** (`test_*.cpp`): the task and output
  queues, terminal-client routing, PTYs, serial streams, owned children,
  descriptors and file permissions, local control sockets, handover and
  session handoff transports, QUIC datagram batching.
- **macOS and iOS** (`test_macos_*`, `test_ios_*`, `*.mm`, `*.swift`, and
  `Apple/`): pasteboards, workspace storage, settings search, Metal drawable
  persistence, resume sealing, protected files. They build only on Apple
  platforms.
- **Linux** (`test_linux_*` and `Linux/`): font inventory and fallback, color
  glyphs, rich text, the configuration watcher, updater security. Some need
  GTK, GIO or PangoCairo and are left out of a configure that lacks them.
- **Web** (`test_web_*`): the WebAssembly terminal, font inventory, and
  rasterizer.
- **Emacs and Neovim Sync clients** (`*.el`, `*.lua`, the `*.expected`
  upstream transcripts, and `sync_stream_fixture.cpp`, which writes the host
  stream they decode): run by the `emacs_*` and `neovim_*` stanzas, which skip
  when the editor is not installed.
- **Goldens** (`goldens/`): reference images for platform UI checks.

Each `test_*.cpp` is a CTest case. A test that needs a symbol only a frontend
supplies is named in `UBIQUITTY_DEFERRED_TESTS` in `Tests/CMakeLists.txt` and
runs as a `run-tests.sh` stanza instead. The `.mm` and `.swift` tests are
registered by name in `Tests/CMakeLists.txt` or run as stanzas, and the Emacs
and Neovim tests run as stanzas. `./Scripts/run-tests.sh --list-stanzas` lists the stanzas. A
failing registered test fails the run.
