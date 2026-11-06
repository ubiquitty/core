<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Session model

The host owns terminal sessions and their live presentation topology. A
frontend projects that topology into native windows, tabs, and panes and sends
operations back to the authority that owns it.

The live tree is `Ubiquitty::Session::Layout` in
`Source/Core/Session/LayoutTree.h`. The durable arrangement is
`Ubiquitty::Session::Workspace` in `Source/Core/Session/Workspace.h`;
`WorkspaceCapture.h` bridges the two. A durable arrangement is a recipe for
recreating a layout, not a live PTY, process, authorization grant, or resume
credential. Client-saved arrangements and the host's named-workspace records
have separate ownership and persistence.

## 1. The tree

```text
Host
  HostedWorkspace[]
    Layout::Workspace
      Surface[]                 top-level presentation containers
        Tab[]                   ordered tabs, one active
          Panes::Tree           binary split tree
            Pane                opaque identity
              -> Session       host-owned PTY and terminal
```

A daemon may own several workspaces. `Daemon::HostedWorkspace` binds each
workspace's pane identities to its sessions. The tree itself owns no terminal
or operating-system handle.

- A **workspace** owns surfaces, the active surface, and stable identities.
- A **surface** contains ordered tabs, an active tab, and the zoomed-pane
  presentation state.
- A **tab** contains a split tree and its title.
- A **pane** is a leaf naming the session displayed there. Internal nodes carry
  split orientation and ratio.
- A **session** owns the program's PTY and terminal state. A pane's position is
  not the session's identity.

The layout handles `SurfaceID`, `TabID`, and `PaneID` are distinct scoped
enums backed by `uint32_t`; zero means none. They are not interchangeable.
`Session::SurfaceID` is a separate `uint64_t` durable metadata handle.
The shared word “surface” does not authorize conversion between these spaces.

## 2. Surfaces and native containers

A surface is a platform-neutral top-level container. Desktop frontends map it
to a native window, mobile frontends to a scene or selected full-screen
container, and browser frontends to a viewport. A single-surface display may
show one surface at a time without changing the model.

The host owns surfaces it never draws. Reattaching clients reconstruct hosted
topology from the host's structure record. Native window frames, backing scale,
and display placement remain local presentation preferences.

## 3. Pane topology

`Source/Core/Panes/PaneTree.h` supplies the binary split tree. Leaves carry
opaque pane identifiers; internal nodes carry orientation and ratio.
Operations include split, close, focus, and ratio adjustment.

The topology uses fixed-capacity pools. Each tab holds its `Panes::Tree` by
value, and a workspace holds its bounded surface and tab arrays. This does not
mean the entire terminal, session, or host is allocation-free.

## 4. Invariants

- **No pointers across the seam.** A leaf names its session through an opaque
  identity. The host resolves it to an authoritative terminal; a client
  resolves it to a replica. They are different objects.
- **Stable identity.** Reordering or moving tabs and panes does not renumber
  their identities. A stale identity must not address a new object.
- **One authority.** Hosted layout changes are requests to the host, not local
  mutations of a client mirror. A local workspace has its own local authority.
- **Revision-checked operations.** Authorization and target identity are
  checked when an operation is accepted and when delayed consent is applied.
  Consent for an earlier topology cannot authorize a different operation.
- **Bounded structure.** Counts, depth, strings, and allocation are validated
  before a received topology is adopted. Refusal leaves the previous coherent
  tree intact.

## 5. Model and chrome

| Core and host | Frontend |
| --- | --- |
| Logical tree, order, active tab, focus and zoom | Native containers, fullscreen and display placement |
| Split orientation and ratios | Pixel geometry, dividers, padding and chrome |
| Pane title, directory, process, status and progress facts | Font selection, rasterization and accessibility presentation |
| Input destination and operation authority | Native input capture and key-equivalent glyphs |

Ratios cross the seam, not platform handles or physical pixel sizes. Rendering,
hit testing, and grid fitting must use the same usable viewport after pane
chrome and padding.

## 6. Resource bounds

`LayoutTree.h` defines 16 surfaces per workspace, 32 tabs per surface, and
64 panes per tab. These are topology bounds, not a promise that the host will
allocate their product in PTYs. Session, persistence, protocol, and aggregate
memory limits also apply. In particular, `HostLayoutRecord.h` bounds the total
panes in a persisted record independently.

## 7. Input, geometry, and nested multiplexers

Each live leaf has its own program and PTY. Splitting creates a new leaf and
program only after the host admits the operation. Closing a presentation
attachment is distinct from terminating the session.

Geometry follows the human driver whose typing is newest, as defined by
`Protocol::Sync::GeometryArbiter` and [Sync §6.10](sync.md). Before anyone
types, the newest eligible driver viewport applies. Agents and viewers do not
become geometry owners through pointer motion or viewport reports. A pin
freezes the size; an authorized explicit override lasts until the driver takes
ownership back. Resizes are coalesced, and a pane sharing a window uses its
own viewport rather than the whole window's dimensions. A zoomed pane still
shares its tab: the viewport the zoom gives it is stated and arbitrated as any
resize is, and unzooming states the split's viewport again. The zoomed pane
keeps its header, whose contract button undoes the zoom.

Model focus and operating-system focus are distinct. Native focus changes are
intents; the authoritative tree determines the selected target. Competing
requests are serialized by the host, not independently resolved by each
frontend.

An inner tmux or another terminal multiplexer does not acquire the outer
workspace's identities. Its layout bytes and OSC metadata remain pane output
unless a separately owned control-mode connection explicitly interprets them.
The host's tree is changed only by authorized workspace operations and session
lifetimes. See [tmux control mode](tmux-control-mode.md).

## 8. Workspaces across a host restart

A workspace outlives its clients, so it has to outlive the host process too: a
crash, a reboot, or a restart that replaces the binary would otherwise leave
the person's arrangement as one empty window. The host keeps a record of every
workspace that does not end with its last person, named or not
(`Source/Core/Session/HostLayoutRecord.h`): its windows, tabs and split trees,
each pane's directory (OSC 7, or the foreground process's when the shell
reports none), the `[user@]host` of a pane launched as a remote session, titles,
workspace, window, tab, pane and session labels, the hand-put tags of each of
those, and each pane's grid. Pixel geometry stays the
frontend's (§5), so a window's frame is not recorded; its grid is.

- **Where.** One file per daemon socket, beside it: `<socket>.layouts`, mode
  0600, written to a temporary file and renamed over the old one. The encoding
  opens with four bytes that name it, the version it was written as, and a
  count; each record is preceded by its length. A reader reads only its own
  version, and a field is added by taking the next version. No other form is
  read. A file this build cannot read, because its version is another, its
  records do not read whole, or it is in a form written before the file
  carried a version, is logged by name with the reason and moved to
  `<socket>.layouts.unread`, so the next write does not replace what it
  held. It is never read as a file that holds no record. A file set aside
  earlier is kept as well, a place older each time (`.unread.1`, `.unread.2`,
  `.unread.3`); the oldest of four is removed, and the log names it.
- **When.** One second after the topology changes, a directory reported by
  OSC 7 changes, or a line of input reaches a recorded workspace (the cheap
  sign that a silent shell may have changed directory). A write that would not
  change the file is skipped.
- **Restorable.** A host that starts reads the file, and the files of hosts on
  sibling sockets whose startup lock it can take and on which nothing
  listens, which is how a host replaced by another binary hands its records
  on. A listener whose backlog is full still listens, and when that cannot
  be told the socket counts as listened on. A chain of handovers keeps its
  records beside the first host's socket after that host has gone, served by
  the host holding the chain, so they are neither read nor removed while it
  runs. A host that takes a chain over keeps the records it took in as it
  started among the chain's, in the chain's file, and removes the file it
  first wrote them to. A host that is already running reads its siblings'
  files again only when a saved reference names an instance that no longer
  runs and no record it holds answers it, and only for a connection that may
  restore a workspace; it takes them in as it would at its start, within the
  same bounds. Each record is listed in the
  host's tree as one workspace in the `Restorable` state, under an id from the
  session id space. Nothing runs in it.
- **Identity.** A record keeps the id its host listed the workspace under,
  and the daemon instances that held it live, newest first, at most four,
  each with the id the workspace had there. A host that starts lists the
  record under its id when nothing else holds it, so the workspace comes back
  under the id its clients saved; when another workspace holds that id, as it
  does when two hosts each numbered their first workspace 1, the record is
  listed under a fresh id, and its holders still say what each client saved.
  A record carries a name, an id or both; an unnamed workspace is found by
  its id alone, and comes back unnamed.
- **Bound.** A file keeps at most 64 records. Each carries when a host last
  held its workspace live, to the hour, so a workspace in use is not rewritten
  for the time alone. The live workspaces are kept first; when the restorable
  ones would take the file past the bound, the least recently held go, and
  the log names each.
- **Restore.** A person's HELLO naming a named workspace, its listed id from a
  client that names this host instance, or a saved reference from a client
  that names one of the instances that held it and the id it held it under,
  recreates the
  arrangement with a fresh
  shell in each recorded directory at each recorded grid, and the client gets
  the whole topology and every pane as for a live workspace. The workspace and
  its sessions carry `sessionInfoRestoredAfterHostRestart`, which the macOS and
  iOS clients show as a one-line notice. An agent cannot restore one. The
  saved-reference case is how a window reopened after a relaunch comes back:
  the frontend holds the instance, workspace and session it saved, not the
  workspace's name, and the Welcome says which instance the workspace was
  restored from so the client accepts the instance that runs now, and the
  workspace under the id it is listed by there
  (`Docs/sync-protocol.md`, "Admission targets and pins"). One rule finds
  what a reference names: the instance and the id it held the workspace
  under, among the pairs a record keeps of its holders and a live workspace of
  its former holders, both bounded. A reference to an instance that handed its
  workspaces over by an upgrade (`Docs/sync.md` §6.13) is answered the same
  way by the host that holds the workspace live now, with its running
  sessions rather than fresh ones, for every client that saved it. A later
  reference to a workspace an earlier one already recreated joins that
  restoration under the same id and session, spends no record and starts no
  shell, and needs the same authority to restore. A reference that names
  nothing, or one that two workspaces both claim, a record and a live one
  among them, is refused and starts nothing.
- **A client that stayed open.** The restored workspace has the restoring
  host's own ids: its windows, tabs, panes and sessions are numbered afresh.
  A client still showing the workspace when its daemon stopped keeps its
  windows and pane views. Core pairs each pane its mirror held with the pane
  restored in its place, windows, tabs and leaves in the record's order,
  wherever the arrangement is the same one. A pane the host could not start
  leaves the record's arrangement short of it, so a restoration with fewer
  panes pairs each place that every way of losing that many panes from the
  mirror's arrangement fills with the same pane, and leaves a place two ways
  disagree on unpaired. Each paired pane's endpoint moves to its restored
  session, and every other pane ends, whatever its number
  (`Layout::pairRestoredPanes` in `Source/Core/Session/WorkspaceWire.h`).
  Frontends take the pairing to move what they keep by window, tab or pane id.
- **Forgetting.** A workspace whose last pane ends — its program exits, a person
  closes it, or a client terminates it — leaves no record. A pane whose program
  is killed by a signal is lost rather than ended, as a machine shutting down
  kills it, and leaves the record as last written, so a workspace that loses
  its last pane that way stays restorable. Terminating a restorable entry
  forgets it.

## 9. Names and labels

Every level of the tree can carry two different strings, and a listing treats
them differently.

- A **name** or **title** is what is displayed. A workspace has a name (what a
  HELLO reaches it by, unique on its host). A tab has a title a person gave it,
  and otherwise the title composed from its focused pane. A window and a pane
  have no name of their own: they show the title composed for them.
- A **label** is the addressable handle: what a target such as `ubq … -t logs`
  resolves, kept in the host's one label book (`Identity::LabelBook`) under the
  same bound and refusals at every level. Labels need not be unique; an
  ambiguous one is refused with its candidates.

The rule, `App::listedName` in `Core/App/TitleCompose.h`:

| Level | Displayed | Label |
| --- | --- | --- |
| Workspace | its name | a chip beside the name |
| Tab | its title | a chip beside the title |
| Window | its label when it has one, else its active tab's title | is the title; no chip |
| Pane | its label when it has one, else its composed title | is the title; no chip |

A label never replaces a name. The two exceptions are the levels with no name,
where a label is already the title in a window's row and a pane's header, and
a listing agrees with those. A workspace with no name is displayed as its
active window is; its label is still only a chip.

## 10. Tags

A tag is a marker a thing wears, not a handle: it is drawn and searched for,
and no target resolves through it. A listing shows tags from two sources.

- **Rules.** The reader's `[[tags]]` entries that match a host are applied to
  every pane on that host, by the reader, from its own configuration. Nothing
  is stored on the host.
- **By hand.** The tags a person puts on a workspace, a window, a tab, a pane
  or a session are host state, kept in the host's tag book
  (`Identity::TagBook`) beside its labels, bounded the same way, and recorded
  with a recorded workspace (§8). A pane's row wears its own and then those of
  the session behind it. The host keeps names only; the colour a name is
  drawn in is whatever the reader's rules give it.

Labels, tags and names are changed by calls a Sync connection makes
(Docs/sync.md §7.3): `label.set` and `tags.set` on any of the five levels,
`workspace.rename` and `tab.rename`, and beside them `tab.move` and
`window.close`. `ubq label`, `tag`, `rename`, `move-tab` and `close` are those
calls. Each is refused with its reason as the answer's message, and each
change is a new revision of the workspace's structure, so every listing of
the host shows it.

Tree-edit requests issued through `ubq` require an attached person's consent;
a local command invocation is not proof of a person. Consent binds the original
requester, exact operation and arguments, workspace revision, and live attachment.
A request that ends sessions is also confirmed, and Core words the
question (`Runtime::wordDestructiveRequest`): closing a window, terminating a
session, terminating a workspace. The question names what ends, how many
sessions, what runs in them, and its two buttons. Asking returns a token, and
a reader sends the request only with that token, once, and only while it
would still end the sessions the question named.

A parent wears its own tags and those of everything under it, each name once.

A rule that matches a **profile** is the reader's alone. It applies to a pane
the reader has open with that profile applied, and does not apply to a pane
the reader only sees listed: a listed pane has no profile until someone opens
it.
