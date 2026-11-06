<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Package layers

Ubiquitty's package boundaries are enforced within one repository. Higher
layers depend on lower layers through their published surfaces; operating-system
implementations supply the portable platform contracts.

## The layers

| Layer | Responsibility | Main locations |
| --- | --- | --- |
| L0 | Foundation values and platform contracts | `Source/Base/`, `Source/PAL/` |
| L1 | Portable terminal model and render plan | `Source/Core/` model modules, `Source/UbiquittyCore/` |
| L2 | Session behavior, preferences, and transport composition | Core behavior modules listed below |
| L3 | Embedding facade, Apple renderer, platform backends, and applications | `Source/UbiquittyKit/`, `Source/UbiquittyRenderKit/`, `Source/Platform/`, `Source/Agent/` |

`Source/ThirdParty/` holds vendored libraries outside this layering; the
boundary check lets Core include them beside Base and PAL.

The table describes architectural responsibilities, not a recursive directory
rule: `Source/Core/` contains both L1 and L2. The exact compilation partition
is defined by `Scripts/check-core-layer-partition.sh` over
`Scripts/portable-core-sources.txt`.

### Foundation and platform contracts

`Source/Base/` provides portable primitives. `Source/PAL/` declares clocks,
streams, process operations, task dispatch, and terminal clients. Backend
implementations belong under `Source/Platform/`; their operating-system
dependencies must not enter portable headers.

Portable PAL implementation files are classified separately from concrete
backends. The partition calls bundled POSIX and WebAssembly backends `L3b`
so they are not counted as terminal-model code merely because the aggregate
library carries them.

### Terminal model

L1 contains byte parsing, cells, screens, history, image decoding, render plans,
Unicode and codec support, key encoding, diagnostics, and the tmux model.
It must not depend on preferences, themes, session orchestration, or transport
policy.

The supported external model surface is
`UbiquittyCore/UbiquittyCore.h` and the headers it re-exports. See
[libUbiquittyCore](libubiquittycore.md) for embedding and versioning.

Mutual dependencies inside one compiled layer are distinct from upward package
dependencies. For example, terminal/image and terminal/tmux relationships do
not give the model permission to include a higher-layer session API.

### Behavior and transport

L2 composes the model with configuration and session behavior. Its modules
include `App`, `Preferences`, `Themes`, `Terminfo`, `Input`, `Runtime`,
`Selection`, `Transport`, `Network`, `Session`, `Security`, and the control,
local and synchronization protocols. Key encoding belongs to the model; binding
selection and paste policy belong to behavior. One model header reaches the
synchronization protocol: the row form codec in `Core/Screen` writes through
the wire's varint reader and writer, which carry no behavior, and the boundary
check names that single include rather than the module.

Theme application belongs in the Themes module and accepts a terminal or
palette value. The renderer does not include the theme registry. Shared value
types needed by the model belong in a lower layer rather than forcing an
upward dependency.

### Facade and consumers

`UbiquittyKit::Session` composes a terminal with local or remote session
driving. It depends on model and behavior, so it belongs above both. Its public
umbrella and `kitVersion` are separate from the model umbrella and
`coreVersion`; see [UbiquittyKit](ubiquittykit-api.md).

Platform applications, the daemon, command-line clients, and external samples
consume these layers. The in-tree applications co-evolve with internal APIs;
they are not examples of restricting an external embedder to published
headers. The installed-package sample is the check for that restriction.

## Rule for new files

Classify a new file by its responsibility and update the partition if necessary.
Includes may reach its own layer and lower layers, using published headers
across package boundaries. If a file needs a higher-layer service, pass that
service in through a lower-layer interface or move the shared value type down.

Do not add a catch-all protocol classification: tmux modeling and synchronized
session orchestration have different dependencies. Per-file exceptions must
be explicit in the partition.

## Build and enforcement

The Core CMake configuration defines:

- `ubiquitty-core-model`: portable foundation and model code.
- `ubiquitty-behaviour`: behavior code, linked to the model.
- `ubiquitty-core`: aggregate library with the bundled platform backend.
- `ubiquitty-kit`: the embedding session facade over the aggregate.

CMake verifies that the union of classified sources equals the canonical
portable source list. Layer-specific staged include directories restrict
compile-time header visibility; published install headers define the external
consumer surface. Backend include directories remain private.

`Scripts/check-boundaries.py` checks model-to-behavior dependencies and
published-header use by external consumers. Its “Rule for new files” diagnostics
refer to this document. `Scripts/check-core-layer-partition.sh` rejects
unclassified source files and validates source-list subsets.

These checks complement each other: a source-list classification alone does
not establish a valid dependency, and an include check alone does not establish
that every required translation unit is compiled.
