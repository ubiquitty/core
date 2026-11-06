<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Type and ownership map

This is a source index for the ownership boundaries described in
[Architecture](architecture.md). It names the types most useful when tracing
input, terminal state, presentation, and hosted sessions; it is not a generated
include graph or an inventory of every declaration.

## Module boundaries

```text
Native and browser frontends             Headless host and command clients
                │                                    │
                ├──── session and runtime APIs ──────┤
                │                                    │
       portable terminal model, rendering policy, and protocol
                │
       PAL and transport interfaces
                │
       operating-system and graphics backends
```

Core owns program-observable terminal behavior and portable policy.
Frontends own native UI integration and graphics resources. PAL supplies
operating-system facilities without moving their types into terminal headers.
See [Package layers](package-layers.md) for build targets and dependency rules.

## Terminal model

| Type or interface | Header | Responsibility |
| --- | --- | --- |
| `Terminal` | `Source/Core/Terminal.h` | Constructed, noncopyable terminal instance: parser state, screens, modes, palette, images, and client seams |
| `Screen::State` | `Source/Core/Screen/Screen.h` | Cursor, margins, pen, and per-screen state |
| `CellGrid` | `Source/Core/CellGrid.h` | Authoritative cell storage, row metadata, mutation generations, and paint access |
| `Cell`, `Pen` | `Source/Core/Cell.h`, `Source/Core/Pen.h` | Cell content and rendition state |
| `LineStore` | `Source/Core/LineStore.h` | Bounded retained history and line access |
| `Cursor::RenderState` | `Source/Core/Cursor/RenderState.h` | Resolved cursor geometry, opacity, and visibility |
| `RichText::Document`, `TypographyFacts` | `Source/Core/RichText/Document.h`, `Source/Core/RichText/TypographyFacts.h` | Styled text and the metrics used for deterministic block layout |

Terminal mutation belongs to the model's owning thread or lock scope. A
frontend does not independently reinterpret the parser or change cells while
another consumer reads them. Rendering captures and protocol publications own
the state needed after that scope ends.

`CheckedPtr` and `WeakPtr`, in `Source/Base/`, express different borrowed
lifetimes: the former requires the target to outlive its references; the latter
can become null when its target ends. Neither is ownership of the target.

## Runtime and platform seams

| Type or interface | Header | Responsibility |
| --- | --- | --- |
| `Runtime::TerminalRuntime` | `Source/Core/Runtime/TerminalRuntime.h` | Terminal and stream lifetime, draining, resizing, snapshots, and outcomes |
| `Runtime::DrainHost` | Same header | Model-lock and drain-owner boundary |
| `Runtime::TerminalSession` | `Source/Core/Runtime/TerminalSession.h` | Session lifecycle, local or remote input routing, observation, and presentation access |
| `Runtime::IngestLoop` | `Source/Core/Runtime/IngestLoop.h` | Readiness, bounded draining, delivery coalescing, and optional flood ingest |
| `Runtime::WorkspaceSession` | `Source/Core/Runtime/WorkspaceSession.h` | Frontend-owned collection of pane sessions and local workspace operations |
| `PAL::ByteStream`, `PAL::Stream` | `Source/PAL/Stream.h` | Byte transport and the process-backed session stream |
| `PAL::ProcessFacts` | Same header | Process, terminal, and foreground-group facts supplied by the platform |
| Terminal clients | `Source/PAL/TerminalClients.h` | Window, clipboard, shell, diagnostics, measurement, file-read, trigger, and capability-policy callbacks |

A process-backed stream and an opaque remote channel are different contracts.
A remote transport cannot acquire local PTY or process authority by presenting
the same byte interface.

`Source/PAL/PTYFacts.h` defines process-introspection operations.
`Source/PAL/OutputQueue.h` and `TaskQueue.h` provide output and work queues.
Platform implementations own native descriptors,
process queries, clipboard access, and main-thread UI hops.

## Rendering

The portable build entry is:

```cpp
InstanceCounts buildCellInstances(const BuildInputs&,
    const GlyphBackend&, const InstanceBuffers&);
```

Its declaration is in `Source/Core/Rendering/CellInstanceBuilder.h`.
Inputs and output spans are borrowed for that call. The caller must keep their
storage alive and must not retain borrowed terminal views past their scope.

| Type or interface | Header | Responsibility |
| --- | --- | --- |
| `TerminalFrameInputs` | `Source/Core/Rendering/TerminalFrameInputs.h` | Owned snapshot storage and the builder inputs derived from it |
| `FrameCapture`, `FrameRecordHeader` | `Source/Core/Rendering/FrameCapture.h` | Capture lifetime, publication identity, and full/delta lineage |
| `GlyphBackend` | `Source/Core/Rendering/GlyphBackend.h` | Platform raster and shaping callbacks consumed by the portable builder |
| `GlyphSlotCache` | `Source/Core/Rendering/GlyphSlotCache.h` | Glyph-slot identity and eviction without owning platform pixels |
| `FrameDamage` | `Source/Core/Rendering/FrameDamage.h` | Retained-frame damage and composition decisions |
| `SoftwareRenderer` | `Source/Core/Rendering/SoftwareRenderer.h` | CPU rendering of the portable streams |
| `FontRoutingSession` | `Source/Core/Rendering/Font/FontRoutingSession.h` | Per-renderer caches used by the shared font fallback policy |

Color resolution, procedural geometry, image placement, glyph keys, and
instance construction are portable. Font handles, raster storage, textures,
command buffers, and presentation belong to a backend. Equal inputs and equal
font facts must produce equal portable decisions; native font rasterization
is not assumed byte-identical across operating systems.

See [Font routing](font-routing.md).

## Sessions and topology

| Type or interface | Header | Responsibility |
| --- | --- | --- |
| `Session::Layout::Workspace` | `Source/Core/Session/LayoutTree.h` | Live surfaces, ordered tabs, pane trees, focus, zoom, and typed layout identities |
| `Session::Workspace` | `Source/Core/Session/Workspace.h` | Serializable arrangement, not live terminal or transport ownership |
| `Session::SurfaceRegistry` | `Source/Core/Session/Surface.h` | Registry of live surface metadata |
| `Session::RemoteSessionCore` | `Source/Core/Session/RemoteSessionCore.h` | Connection, greeting, replica updates, reconnect, and stop state |
| `Session::Dispatcher`, `ChannelFactory`, `RemoteSessionObserver` | Same header | Session-thread dispatch, carrier construction, and event delivery |
| `Session::PostedWork` | `Source/Core/Session/PostedWork.h` | One unit of session-thread work, held inline and run at most once |
| `Runtime::PostedDispatcher`, `PostedWorkTable`, `PostedWorkToken`, `PostedWorkLoop` | `Source/Core/Runtime/PostedDispatcher.h`, `PostedWorkTable.h` | Session work handed to a frontend's loop as a typed token Core redeems once |
| `Session::WorkspaceConnection` | `Source/Core/Session/WorkspaceConnection.h` | Shared authenticated carrier, topology mirror, and pane subscriptions |
| `Session::HostListing` | `Source/Core/Session/HostListing.h` | Host structure and facts joined into a coherent listing |
| `Runtime::HostOutlineRows` | `Source/Core/Runtime/HostOutline.h` | Presentation rows derived from host-owned structure and facts |

Live layout IDs, persistent arrangement handles, session IDs, and connection
identities are separate spaces. A position in a pane tree is not authority to
operate on a session. Delayed actions must revalidate their target, requester,
revision, and live connection.

A dispatcher runs remote-session methods on one session thread. Carrier
callbacks arrive from transport threads and cross that boundary as owned work.
Destroying a pane subscription must not destroy a carrier still owned by other
workspace panes.

See [Session model](session-model.md) and
[Shell workspace ownership](design/shell-workspace-session.md).

## Transport, synchronization, and control

`Transport::Channel`, in `Source/Core/Transport/Channel.h`, is an opaque
bidirectional carrier. Local-socket, subprocess, and QUIC adapters implement
its lifetime and callback contract. They do not decide terminal or workspace
semantics.

`Protocol::Sync`, in `Source/Core/Protocol/Sync/`, owns the canonical framing,
`SyncClientCore`, and the screen, history, attachment, input, pacing, and related
synchronization contracts.
`ScreenPublisher` and `ScreenReplica` in `ScreenPlane.h` publish and apply
coherent screen state; `HistoryPublisher` in `HistoryPlane.h` serves retained
lines separately. The public protocol is specified
in [Sync](sync.md) and the [carrier reference](sync-protocol.md).

`Protocol::Control`, in `Source/Core/Protocol/Control/`, owns the method
registry, schemas, and call admission. The host in `Source/Platform/Host/`
owns sessions, authentication, authorization, and authoritative layout changes.
A frontend control affordance is not a substitute for host admission.

`Source/Core/Protocol/Tmux/` interprets tmux control mode. It is a distinct
protocol with its own pane models, not an alternate grammar for the host's
workspace tree.

## Frontend adapters

| Area | Source | Role |
| --- | --- | --- |
| Apple surface | `Source/UbiquittyKit/Swift/` | `TerminalSurface` and `SurfaceRenderer` connect native input, capture, and display |
| Apple rendering and runtime adapter | `Source/UbiquittyRenderKit/` | `RuntimeDrive`, font objects, glyph rasters, Metal resources, and frame encoding |
| macOS shell | `Source/Platform/macOS/App/` | Window, tab, settings, inspector, and clipboard presentation |
| iOS shell | `Source/Platform/iOS/` | Mobile scene, input, and session presentation |
| Linux | `Source/Platform/Linux/` | GTK integration, FreeType rasterization, Vulkan rendering, and CPU fallback |
| Web | `Source/Platform/Web/` | Browser integration and rendering adapters |
| Agent client | `Source/Agent/` | Headless session observation and command/MCP operations |

The frontend consumes model decisions and reports local facts. It does not
invent protocol identity, terminal widths, consent, or a second configuration
default table. See [Configuration](configuration-seam.md),
[Swift renderer](swift-renderer.md), and [UbiquittyKit API](ubiquittykit-api.md).
