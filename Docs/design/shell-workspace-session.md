<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The window model behind thin frontends

`Runtime::WorkspaceSession` owns local window topology and pane sessions.
Frontends project that model into native views and send intents back to it.
Remote topology is host-owned; see [the session model](../session-model.md)
and [Sync](../sync.md) for that boundary.

## Ownership

`Session::Layout::Workspace` is the portable split, tab, and surface model.
It allocates pane IDs, resolves spatial focus, and records zoom. It contains
topology, not platform views or terminal processes.

`Runtime::WorkspaceSession` composes that topology with one
`Runtime::TerminalSession` per local pane. One model can represent several
native windows; each window holds a `SurfaceID`. Calls to the model run on
the frontend's main thread.

A pane ID belongs to the layout model. A backend's foreign identifier, such as
a tmux pane ID, stays in that backend's namespace and reaches the layout through
explicit adoption facts. A frontend must not substitute a foreign ID for a
model-owned pane ID.

## Resolving actions

The model resolves split, close, zoom, focus, directional focus, and divider
actions through the selected pane's session. The session can route an action
to its backend or report that it requires local handling. For a local action,
the model changes topology and creates or closes the corresponding session.

A frontend uses the operation's result and the resulting model state to create,
remove, focus, or redraw views. It must not keep a second pane allocator, zoom
state, or backend-specific action implementation.

## Frontend projection

A window controller:

- Retains the surface ID that identifies its window.
- Creates a terminal view for each pane session exposed by the model.
- Projects the active pane tree into native split views.
- Sends user actions and divider changes to Core.
- Updates its views after the model reports the resulting state.

The model does not own native windows, renderers, menus, or pointer hit testing.

## Ingest and client lifetime

`WorkspaceSession::IngestPolicy` determines who drains a local PTY:

- `ThreadDrained`: the session's ingest thread owns the drain.
- `CallerDrained`: the frontend drains it on its event loop.

The Linux frontend uses `CallerDrained` with its file-descriptor watch. A model
must not also start an ingest thread for that session.

Platform terminal clients are installed before a newly created session starts
ingesting. Their owners must outlive the terminals that borrow them. An adopted
session is equipped under its model lock before the frontend exposes it.

## Configuration and remote state

Launch settings are applied per terminal. Pane, tab, surface, and default
configuration are distinct scopes; a frontend must not implement a per-pane
choice by changing process-global state.

A host-owned workspace is a projection of authoritative remote state. Local
UI state must not be mistaken for a committed host edit, and a denied remote
operation must leave the projected topology unchanged.
