<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Embedding the portable terminal Core

`libUbiquittyCore` provides a C++23 terminal model without a GUI toolkit.
Feed it terminal bytes, read cells and history, encode input, and build a render
plan. A caller supplies platform services through the PAL contract.

Include `UbiquittyCore/UbiquittyCore.h`. The supported external API is that
umbrella and the headers it re-exports, plus the separately public
`PAL/Socket.h` adapter contract described below. In-tree applications can use
internal modules; their includes are not a compatibility contract for embedders.

For session driving, live configuration, and local or remote transports, use
[UbiquittyKit](ubiquittykit-api.md).

The library and its headers are [Apache-2.0](../LICENSES/Apache-2.0.txt), so
any product, including a commercial one, may embed it; so is the rest of the
repository. See [licensing](licensing.md).

## Build and link

From the repository root:

```sh
cmake -S . -B /tmp/ubiquitty-core-build \
  -DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_QUIC=OFF
cmake --build /tmp/ubiquitty-core-build --target core-consumer
/tmp/ubiquitty-core-build/core-consumer
```

Link the CMake target `ubiquitty-core`, not just one archive file: the target
carries the model, behavior, and bundled-backend link dependencies.

For an installed package, use `find_package(UbiquittyCore REQUIRED)` and link
`Ubiquitty::ubiquitty-core`, or `Ubiquitty::ubiquitty-kit` for the session
facade. `Samples/find-package-consumer` demonstrates consuming the install
prefix without source-tree include paths.

The model and behavior code have no AppKit, GTK, Metal, or window-system
dependency. The aggregate library includes a platform backend; portable code
reaches operating-system services through PAL rather than making raw OS calls.

## Feed bytes and read a row

```cpp
#include "UbiquittyCore/UbiquittyCore.h"

Ubiquitty::Terminal terminal;
Ubiquitty::resize(terminal, 24, 80);
Ubiquitty::processBytes(&terminal, "hello\r\n");

Ubiquitty::Cell row[80];
Ubiquitty::Screen::getDisplayRow(terminal, 0, 0, row, 80);
```

The complete `Samples/core-consumer/main.cpp` supplies the platform stubs and
demonstrates both byte ingestion and key encoding. A real frontend must preserve
the model's ownership and synchronization rules when reading it.

## Public surface

The umbrella exposes:

- Terminal lifecycle and byte ingestion through `Core/TerminalOperations.h`,
  with the terminal and screen-addressing types.
- Cells, pens, and palette values.
- `CellGrid`, `RenderPlan`, `CellInstance`, the cell-instance builder, and
  the software renderer.
- Key events, key translation, the keymap, and Kitty keyboard flags.
- PAL services, streams, task queues, and terminal clients.
- The library version stamp.

Preferences, theme registries, parser internals, and session orchestration are
not part of the model umbrella. Use the Kit umbrella where the embedding
contract includes configuration or session behavior.

`PAL/Socket.h` is a separately includable public adapter header. Its
`PAL::adoptByteStream` factory takes ownership of an already-open descriptor,
including on failure, and returns a nonblocking `PAL::PollableByteStream`.
A caller implementing `PAL::Stream::duplicateAsPollableStream` can use it to
give a Kit session independently owned endpoints. This exception does not make
other PAL or implementation headers independently supported sample entry points.

## Renderer capabilities

Some terminal replies depend on what the renderer in front of the model can
draw, so the caller declares its capabilities on the `Terminal`. Both are off by
default, the answer for a caller with its own grid renderer:

- `drawsSizedText`: the renderer crops OSC 66 sized blocks. Off, a sized run
  prints at the ordinary size, and a proportional run at a scale is placed at
  scale 1.
- `drawsProportionalText`: the renderer draws OSC 66 proportional runs
  ([proportional text](proportional-text.md)). Off, the terminal looks like
  one without the extension: a measure query goes unanswered and a run prints
  as ordinary text.

Set a capability before feeding bytes, and only when the renderer draws what it
names; a reply that claims a feature the screen cannot show breaks the
application that trusted it.

## Capability policy

A `Terminal` gates each reply, and each effect that leaves the grid, on its
`capabilityPolicy` at its `currentCapabilityOrigin`. A terminal with no policy
installed answers from `defaultDecision` in `PAL/Capability.h`: the default
Local origin is allowed every effect, and an Agent or Remote origin is refused
host-escaping and exfiltrating effects while its benign replies still answer.
An embedder that connects clients installs its own `CapabilityPolicy` before
feeding untrusted output and keeps it alive while the terminal references it;
the [embedding contract](capability-origin.md#frontend-and-embedding-contract)
states the rest.

## Platform services

The bundled backend supplies storage, login-shell, and process-spawn services.
The caller supplies the remaining system-service functions declared in
`PAL/PAL.h`:

| Functions | Purpose |
| --- | --- |
| `monotonicTicks`, `monotonicMicros` | Monotonic time |
| `diagWrite` | Diagnostic output |
| `bellRing` | Audible or visual bell |
| `clipboardSetText`, `clipboardGetText` | Clipboard access |
| `preferencesLoad`, `preferencesSaveAtomic` | Preferences storage |

The headless sample uses stubs where it has no corresponding UI service.
Production frontends connect per-terminal clients for clipboard routing,
notifications, window operations, and related effects. A global platform hook
must not substitute for state that belongs to one terminal.

## Versioning

`UbiquittyCore/Version.h` is the source of the API version. It provides
`coreVersionMajor`, `coreVersionMinor`, `coreVersionPatch`, the packed
`coreVersion`, `coreVersionString`, and the `UBIQUITTY_CORE_REQUIRE`
compile-time guard.

The model API is versioned independently of the app and Kit:

- Major: a breaking public signature, layout, or required PAL contract.
- Minor: a compatible addition, including an optional hook with a default.
- Patch: a behavior-preserving fix without a surface change.

The promise covers the umbrella's reachable surface and the separately public
`PAL/Socket.h` adapter contract. Other module headers may change without a Core
API version change.

## Source and package checks

`Scripts/portable-core-sources.txt` defines the portable translation-unit set.
`Scripts/gen-portable-core-sources.sh --check` checks that it matches the tree.
CMake and the benchmark drivers consume the same list.

The [package layer contract](package-layers.md) defines model, behavior, and
backend responsibilities. `Scripts/check-boundaries.py` checks dependency
boundaries, and `Scripts/test-libubiquittycore-package.sh` checks an installed
consumer. These checks cover different failures: source-list completeness,
layering, and external-package usability.
