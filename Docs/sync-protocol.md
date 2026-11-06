<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (C) 2026 Stanislav Yaglo -->

# Ubiquitty Sync Protocol

The remote byte-stream contract between ubiquitty-host, which owns the terminal
Core, and clients that replicate and render its state. The stream can travel over
a local socket, an SSH bridge or QUIC; its grammar does not depend on the carrier.

The authoritative declarations are
[Envelope.h](../Source/Core/Protocol/Sync/Envelope.h),
[Admission.h](../Source/Core/Protocol/Sync/Admission.h) and
[Messages.h](../Source/Core/Protocol/Sync/Messages.h); the exact body encoders and
decoders are in [Messages.cpp](../Source/Core/Protocol/Sync/Messages.cpp).
[Docs/sync.md](sync.md) specifies screen, history, input, effects and ordering in
greater detail. These implementations live in `Source/Core/Protocol/Sync/`
under the `Ubiquitty::Protocol::Sync` namespace.

A client receives state, not a second PTY byte stream to parse as VT. A workspace
connection can bind several panes. Workspace structure precedes data for panes it
first names; ending one pane does not end unrelated panes on the carrier.

## Framing

Every remote frame, starting with the first byte of a connection, is:

```text
kind:u8 channel:varint length:varint body[length]
```

Fixed-width integers are little-endian. A varint is unsigned LEB128 in its
shortest representation: an extra zero group, an unterminated maximum-width
encoding or an overflowing value is malformed.

- `kind` is one of the [frame catalog](#frame-catalog) values.
- `channel` is at most `0xFFFFFFFF`, encoded in at most five bytes. Channel 0
  is the connection; a nonzero channel is a connection-local pane handle.
- The frame body is bounded by `maxFrameBodyBytes` (65535 bytes), with a length
  varint of at most three bytes. The largest header is nine bytes.
- A kind has exactly one channel scope and an allowed sender direction.
  Pairing a pane kind with channel 0, or a connection kind with a pane channel,
  is malformed. A pane handle must have been bound before it is used.
- `BulkChunk` and `Paste` use the bulk lane. Every other kind uses the
  interactive lane. The lane is implied, not encoded.

There is no outer type-plus-u32-length header, `Sync` wrapper or `Catalog`
frame. A failed parse is final: no second grammar is tried. Every kind byte
has only its `FrameKind` meaning.

A streaming reader must tolerate every header/body split and coalesced frames.
Read and validate the kind, minimal varints, channel scope, direction, handshake
state and body size before allocating the body. Deliver each complete frame
before admitting the next header, since a greeting or refusal can change the
state within one carrier read. Unknown kinds, malformed headers and oversized
bodies fail closed; no suffix is interpreted after failure.

[FrameIngress](../Source/Core/Protocol/Sync/FrameIngress.h) provides the
header-first bounded owner used by the connection implementations.
[Envelope.cpp](../Source/Core/Protocol/Sync/Envelope.cpp) defines header parsing,
`directionOf`, `isConnectionKind` and `laneOf`. The granted-role check is a
separate check before handler dispatch, not a promise that no body was allocated.

## Frame catalog

These are all current `FrameKind` declarations. Values are decimal; directions
describe the physical sender. “pane” means a nonzero bound handle. A “both” row
does not grant permission to use it in every connection state.

<!-- sync-frame-catalog:start -->
| Kind | Value | Direction | Channel | Body |
| --- | --- | --- | --- | --- |
| `Greet` | 1 | client → host | 0 | Version, client identity, admission request, credential, facts and optional resume |
| `Welcome` | 2 | host → client | 0 | Selected serving identity, attachment, admission, limits and resume outcome |
| `Facts` | 3 | client → host | 0 | Client viewport, timing, link and power facts |
| `Ack` | 4 | client → host | 0 | Applied pane generations, history/effect cursors and transport receipt |
| `Probe` | 5 | both | 0 | Sender clock sample |
| `Echo` | 6 | both | 0 | Echoed sample and receiver hold time |
| `PaneBind` | 7 | host → client | 0 | Pane handle and stable pane ID |
| `PaneUnbind` | 8 | host → client | 0 | Exact handle and end reason |
| `BulkChunk` | 9 | both | 0 | Correlated transfer chunk, final digest and optional compression metadata |
| `Bye` | 10 | both | 0 | Typed close reason, detail and optional peer metadata |
| `TypographyFacts` | 11 | client → host | pane | Per-pane renderer revision, capabilities and typography table |
| `BulkAbort` | 12 | host → client | 0 | Abandoned host transfer ID |
| `Call` | 13 | client → host | 0 | One JSON-RPC request with an ID |
| `Result` | 14 | host → client | 0 | Inline JSON-RPC response or its bulk transfer ID |
| `Notify` | 15 | host → client | 0 | A `NotifyKind` and that kind's record |
| `WorkspaceStructure` | 16 | host → client | 0 | Revisioned workspace/window/tab/split membership |
| `PaneListed` | 17 | host → client | 0 | Metadata-only handle; no screen, history or input authority |
| `ProportionalFaces` | 18 | client → host | 0 | The faces the client draws proportional runs in, as its measurer counts them |
| `PanePalette` | 19 | host → client | pane | Dormant pane's palette block, when it moves |
| `CheckpointHead` | 32 | host → client | pane | Generation, transfer, frame class and state digest |
| `Update` | 33 | host → client | pane | Generation-bound row and state update |
| `Resync` | 34 | client → host | pane | Baseline refusal and section hashes |
| `View` | 36 | client → host | pane | Live, Dormant or RecordsOnly attention state |
| `PaneDirty` | 37 | host → client | pane | Dormant pane changed |
| `HistoryInline` | 40 | host → client | pane | Epoch-bound history lines that fit one frame |
| `HistoryHead` | 41 | host → client | pane | History transfer, epoch, range and order |
| `HistoryReset` | 42 | host → client | pane | New epoch and destructive-clear boundary |
| `HistoryWant` | 43 | client → host | pane | Wanted line ranges and priority |
| `PaneFacts` | 48 | host → client | pane | Revisioned title, directory and foreground facts |
| `StatusRecords` | 49 | host → client | pane | Revisioned program activity records |
| `Effect` | 50 | host → client | pane | Ordered effect record or batch completion |
| `ClipboardRequest` | 51 | host → client | pane | Correlated request to the authorized clipboard server |
| `ImageHead` | 52 | host → client | pane | Immutable decoded image identity and optional pixel transfer |
| `ImagePlacements` | 53 | host → client | pane | Whole image bindings and placements |
| `SampleHead` | 54 | host → client | pane | Immutable PCM identity and sample transfer |
| `RichTextMeasure` | 55 | host → client | pane | Correlated document/shaping or font-catalog page request, pinned to a renderer revision |
| `NativeDragCommand` | 56 | host → client | pane | Native drag registration, transfers and cancellation, fenced by owner epoch |
| `Receipt` | 64 | host → client | pane | Input written and settled cursors |
| `Keys` | 65 | client → host | pane | Sequenced key events, encoded under host modes |
| `Pointer` | 66 | client → host | pane | Sequenced pointer events |
| `Paste` | 67 | client → host | pane | Bounded paste piece |
| `PasteAbort` | 68 | client → host | pane | Cancellation of one paste |
| `Raw` | 69 | client → host | pane | Sequenced bytes written verbatim; never empty |
| `Focus` | 70 | client → host | pane | This pane view's keyboard-focus state |
| `PasteReceipt` | 71 | host → client | pane | Written paste cursor for flow control |
| `InputDiscarded` | 72 | host → client | pane | Input sequence range discarded unwritten |
| `Viewport` | 73 | client → host | pane | Driver's exact pane cell and pixel geometry |
| `ClipboardData` | 74 | client → host | pane | Correlated clipboard status and inline/bulk payload |
| `ImageWant` | 75 | client → host | pane | Missing verified image pixels |
| `RichTextMeasured` | 76 | client → host | pane | Correlated renderer revision and native geometry or validated font-catalog page, in bounded chunks |
| `NativeDragEvent` | 77 | client → host | pane | Authenticated native gestures, drops and correlated data replies |
<!-- sync-frame-catalog:end -->

`Scripts/check-sync-catalog-complete.py` holds the names, values and directions
of this table to the enum and explicit direction switch.

## Body primitives

[Wire.h](../Source/Core/Protocol/Sync/Wire.h) defines the shared reader/writer.
Unless a body explicitly says otherwise:

- `bool` is one byte, exactly 0 or 1. Enums reject unknown values.
- `bytes` is a minimal varint byte count followed by that many bytes.
  `text` uses the same byte count and contains valid UTF-8 with no NUL.
- Lists start with a minimal varint element count; each list also has its own
  maximum and minimum encoded-element-size check.
- `ID128` is `low:u64 high:u64`; fingerprints and digests are fixed byte
  sequences. JSON IDs use the canonical spelling required by their own schema,
  not a decimal conversion of the binary layout.
- Fields outside an explicitly present optional value are mandatory. The
  admission layouts below require exact end of body, including every trailing
  presence flag, rather than an early-end decoder for older layouts.

The following admission layouts use `text`, `bytes` and lists in that sense.

## Greeting and selection

A client opens with a channel-0 `Greet`. Before accepting a `Welcome`, it may
receive only that welcome or a typed `Bye`; a host likewise refuses non-greeting
traffic during its greeting state. A `Bye` can end the handshake without
granting an attachment.

### Greet

```text
wireVersion:u16 appVersion:text
engineFingerprint:[32] clientInstance:ID128 clientKind:u8 requestedRole:u8
target:u8 sessionID:u32 workspaceID:u32 expectedDaemonInstanceID:u64
workspaceName:text agentLabel:text sshHost:text
isAttachOnly:bool endsWhenDetached:bool servesClipboard:bool
hasCredential:bool [credential:bytes]
facts:Facts historyCacheLines:u32 backfillDepthLines:u32 capabilities:varint
hasResume:bool [resume:ResumeRequest]
```

`clientInstance` is an explicit stable client-instance identity, not an offset
into another handshake. `ClientKind` is Human=0, Agent=1 or Viewer=2;
`Role` is Owner=0, Driver=1, Viewer=2 or Agent=3. The requested role is an upper
bound, never a grant.

`appVersion` is at most 32 UTF-8 bytes. A present credential is nonempty opaque
bytes, at most 256 bytes; do not log it or treat it as a display identity.
`workspaceName` is at most 4096 bytes and `sshHost` at most 255.
An agent label is valid UTF-8/no-NUL within the frame bound; its presentation is
truncated only at a UTF-8 boundary to 256 bytes. It is not an authority selector.
Only the nine known capability bits (`0x1FF`) are accepted; they describe
supported features, not permissions.

The base `Facts` embedded in Greet contain, in order:

```text
viewportRows:u16 viewportColumns:u16
cellPixelWidth:u16 cellPixelHeight:u16
textAreaPixelWidth:u16 textAreaPixelHeight:u16
refreshIntervalMicroseconds:u32 link:u8 power:u8 isFocused:bool
```

`link` is Unmetered=0, Metered=1 or Constrained=2; `power` is Normal=0 or
Saver=1. The pane's later geometry uses `Viewport`, not a connection-wide resize.
A standalone Facts frame additionally carries its theme-palette presence flag
and optional palette block; the Greet's embedded facts never contain that block.
See `encode(Facts)` and `decodeFacts` in Messages.cpp for its exact body.

### Admission targets and pins

| Target | Value | Request and selection |
| --- | --- | --- |
| Session | 0 | Request has workspaceID=0; accepted selection has sessionID>0 |
| Workspace | 1 | Numeric ID or explicit create/attach name; accepted selection has both IDs>0 |
| Control | 2 | No session/workspace binding; both selected IDs are 0 |
| Management | 3 | No session/workspace binding; both selected IDs are 0 |

Any nonzero requested session or workspace ID requires `isAttachOnly=true`.
A stale saved numeric ID must refuse, not create a replacement. A request with
numeric IDs cannot also carry `sshHost`. Control and Management require both
requested IDs zero, empty workspaceName/sshHost, false attachOnly/endsWhenDetached,
and no resume. They accept neither resumed pane outcomes nor pane bindings.

Before publishing routing or accepting state, a client validates the selected
target and any requested numeric IDs. A nonzero `expectedDaemonInstanceID`
pins a fresh connection to that exact serving owner; it is not an instruction to
find another daemon or create a substitute session.

A retained resume can accept an inherited owner only with the client's own
prior host identity and daemon pin: the welcome's predecessor must name that
prior host, and both host and daemon must change. An unretained assertion by the
peer is not enough. Clients without a retained resume witness, including the
current editor clients, require exact equality for every nonzero pin.

Two answers to a stale pin are not substitutes, and one rule finds them. A
Workspace request whose pin names an earlier instance names, by that instance
and the workspace ID it held the workspace under, at most one workspace on the
host: a record whose holders name the pair, for an instance that no longer
runs, or a live workspace whose former holders name it, at most
`maxHostLayoutHolders` either way. A pair that two of them claim, a record and
a live workspace included, names neither, and the request is refused as one
that names nothing. The ID a workspace is listed under here plays no part.

- A record answers with the workspace recreated from it
  (`Docs/session-model.md` §8): fresh sessions, the workspace ID the restoring
  host lists it under, and `restoredFromDaemonInstanceID` set to the pinned
  instance. The workspace ID is the requested one when it was free on that host
  and another of its own when it was not. The requested session ID is not
  carried, because its process is gone.
- A live workspace that an earlier reference to an instance that no longer
  runs recreated from its record answers every later such reference the same
  way: the workspace ID it is listed under, its focused session, and
  `restoredFromDaemonInstanceID` set to the pinned instance. The record is
  spent and no shell starts; a request that may not restore a workspace may
  not join one either.
- A live workspace that the pinned instance handed over (`Docs/sync.md` §6.13)
  answers with the sessions it runs: the requested session while it runs
  there, the workspace ID it holds here, and `restoredFromDaemonInstanceID` set
  to the pinned instance, for every client that saved the reference. A request
  that resumes an attachment the earlier instance issued for the requested
  session is a resume, not this answer.

A client accepts either answer only when it requested a Workspace by ID and the
selection names its pin; it holds the workspace by the selected ID from then on,
and every other change of instance or workspace is still refused. A client that
accepts one moves its carrier's socket pin to the socket the selection names, as
it does for a successor: the answer may come over the socket of the pinned
instance, which the host that holds the workspace now serves. A live workspace's
panes keep the identities the instance that started their sessions gave them, so
the client takes each session's pane from the workspace's structure rather than
assuming the answering instance's. A pin nothing names is refused as before, and
no session is created for it. A host that is running reads the records of
sibling hosts that have stopped since it started only for such a request that
nothing here answers, and only for a connection that may restore.
[SyncClientCore](../Source/Core/Protocol/Sync/SyncClientCore.cpp) applies that
owner barrier before exposing the selected metadata.

### Welcome

```text
wireVersion:u16 appVersion:text
engineFingerprint:[32] hostInstanceID:ID128 attachmentID:ID128 resumeSecret:ID128
grantedRole:u8
target:u8 sessionID:u32 workspaceID:u32 daemonInstanceID:u64
socketName:text buildFlags:u8 hostPlatform:text restoredFromDaemonInstanceID:u64
admission:u8 consentRequestID:u32 consentSessionID:u32 principal:text
maxOutstandingCalls:u16 maxQueuedBulkBytes:u32 maxPendingRanges:u16
probeScheduleMicroseconds:[u32] hostTime:u64
hasPredecessor:bool [predecessorHostInstanceID:ID128]
resumed:[PaneResumed] hasPaste:bool [chunkBytes:u32 pasteBytes:u32 windowBytes:u32]
```

The selected daemon ID is nonzero. SocketName is a nonempty basename of ASCII
letters, digits, dot, underscore or hyphen, at most 65535 bytes; it is not a path.
Only build flag bits `0x03` are known. HostPlatform is UTF-8/no-NUL, at most
64 bytes, and may be empty when unknown. `restoredFromDaemonInstanceID` is
zero except for a Workspace selection that answers a saved reference to an
earlier instance, recreated from its record after a restart or held live since a
handover, where it names the earlier instance and differs from the selected
daemon ID; its `workspaceID` is the workspace's on this host (see "Admission
targets and pins"). Principal is UTF-8/no-NUL within the
frame bound, not the claimed agent label. The probe schedule has at most eight
steps. All presence flags and the resumed count are mandatory, even when zero.

`admission` is AwaitingConsent=0 or Granted=1. Granted has both consent IDs
zero. AwaitingConsent has both nonzero, consentSessionID equals the selected
session, and no resumed outcomes. A pending welcome is not a connected or
observable pane.

### Consent admission

The own-status sequence is strictly:

```text
AwaitingConsent Welcome → matching Pending Notify → matching Approved/Denied/Expired Notify
```

Each status is a `Notify` of kind `ConsentStatus`: `request:u32 session:u32
phase:u8`, ten bytes with the kind, as
[ConsentStatus.h](../Source/Core/Protocol/Sync/ConsentStatus.h) defines it.
Terminal-before-Pending, repeated Pending, wrong IDs, unsolicited status and
replayed transitions are refused. While waiting, the client accepts only this
bound status, Probe/Echo transport maintenance and Bye: no PaneBind, listing,
structure, Result, screen data or unrelated Notify. The host permits only its
bounded pending-state transport maintenance, not input or Calls. Denied/Expired remains non-observing and ends with Bye{NoConsent};
it does not replay an operation held before consent.

Human decision prompts and attachment rosters are typed `Notify` records
(`NotifyKind` in [Envelope.h](../Source/Core/Protocol/Sync/Envelope.h)), not
arbitrary data forwarded to UI. See
[Sync/Consent.h](../Source/Core/Protocol/Sync/Consent.h),
[Consent.h](../Source/Core/Protocol/Control/Consent.h) and
[Notify.h](../Source/Core/Protocol/Sync/Notify.h). A response matches the
immutable pending kind, recipient and target. Claimed labels/PIDs are not
authority. Attachment presence separates verified process, claimed label and
grant identity; grants are per pane. Producer audience and recipient
intersection checks still apply.

### Resume and pane lifetime

A `ResumeRequest` contains attachmentID, a required hasSecret flag and optional
resumeSecret, then at most 256 PaneResume entries. Each entry is:

```text
paneID:ID128 ackedGeneration:u64 ackedRowHashesDigest:[16] historyEpoch:u32
cachedRanges:[(from:u64,to:u64)] lastInputSeqSent:u64 view:u8 effectsAcknowledged:u64
```

There are at most 64 sorted, non-overlapping half-open cached ranges per pane.
Control/Management have no resume, even an empty one. Each Welcome PaneResumed
is `paneID:ID128 outcome:u8 hasInputWritten:bool [inputWritten:u64]`, where
outcome is Diffed=0, Checkpointed=1 or Gone=2. A diff requires an accepted retained
baseline; otherwise the pane checkpoints. Input replay follows the written/
settled receipt and deduplication contract, not the success of a carrier write.
See [attach and resume](sync.md#69-attach-resume-reconnect-and-keeping-the-picture).

`PaneBind` establishes a handle for one stable pane. `PaneListed` is a
metadata-only handle and permits no pane input or replica traffic. `PaneUnbind`
ends only the named handle; it is not a whole-workspace exit. A clean carrier
detach is `Bye{Normal}`, preserving sessions according to their lifecycle policy.

## Typed close and versioning

`Bye` is `reason:u8 detail:text hasPeer:bool`, followed, when hasPeer is true,
by `wireVersion:u16 appVersion:text platform:text`.
The presence flag is required. Peer appVersion/platform use the same 32/64-byte
text bounds. Unknown reasons or trailing fields are malformed. A pre-welcome
refusal can therefore preserve peer version/platform information without
pretending admission succeeded. There is no raw authentication-denial record.

The wire has one version, named by `wireVersion` in
[Envelope.h](../Source/Core/Protocol/Sync/Envelope.h); it is nonzero. Two
peers connect only when they state the same version. A peer at any other
version is refused with a typed Bye{VersionMismatch}, read from the version
alone before the rest of its body; it does not select another framing. There
is no minimum peer version and no version history (Docs/sync.md, §7.3
"Versions"). Fingerprints are diagnostic build identity, not authorization or
a substitute for the version.

## Authority and operation dispatch

A validated Greet does not itself authorize a target or mutation.
[Daemon::admitGreeting](../Source/Platform/Host/Daemon.cpp) resolves credentials,
origin, selected owner, target and admission.
[FrameGates.h](../Source/Core/Protocol/Sync/FrameGates.h) checks the granted
role before client-frame dispatch. Input and bulk roles, token scopes, exact
pane subscriptions, method registry gates and consent checks are additional
requirements, not alternatives.

In particular, Viewer cannot send Keys, Pointer, Paste, Raw or BulkChunk;
Viewport and ClipboardData are human Owner/Driver kinds. A general Call frame
is permitted to all granted roles, but the
[method registry](../Source/Core/Protocol/Control/MethodRegistry.cpp) independently
checks each method's role, scope and target. Structure decisions additionally
require Owner/Driver+Drive and only deny/once for the exact original pending
operation. No credential, client-kind field or claimed process fact can assert
kernel-local authority. `HelloIntent`, where retained in a client facade, is
not a remote wire bit table.

## Calls, results, state and effects

A Call body is one UTF-8 JSON-RPC 2.0 request with an ID. A Notify body is a
`kind:varint` naming a `NotifyKind` and that kind's record; it has no answer,
and a client skips a kind it does not know by the frame's length.
Result begins with a transfer varint: zero means the remaining body is the
JSON-RPC response; nonzero means that exact bulk transfer carries it. Method
schemas use strict fields and stable identifiers; a display label is never a
selector. Consult [Control.h](../Source/Core/Protocol/Sync/Control.h) and
[Control/](../Source/Core/Protocol/Control/) for actual schemas and bounds.

- Host listing uses `host.sessions.subscribe` with a revision barrier and
  typed WorkspaceStructure/PaneFacts projection, not a separate session-list
  encoding.
- Termination uses `session.terminate` or `host.session.terminate` under its
  respective authority. Only the exact correlated Result proves success;
  stream closure does not.
- Administration, consent responses/revocations, event subscriptions,
  file reads and pane capture use their registered Calls/Results/Notifies.
  Issued token secrets belong only in an explicit successful result, not
  snapshots, listings or logs.
- `paste.push` uses its correlated ready result and admitted BulkChunks.
  At most 4 MiB per upload. This is `maxPastePushBytes`, not the frame size.
- CheckpointHead and Update carry the authoritative terminal snapshot/delta
  contract. History has epochs and stable logical-line versions; fetched data
  is applied only to its matching pane/epoch/baseline. The row/state codecs and
  digest rules are in [ScreenPlane.h](../Source/Core/Protocol/Sync/ScreenPlane.h)
  and [HistoryPlane.h](../Source/Core/Protocol/Sync/HistoryPlane.h).
- PaneFacts and StatusRecords are the sole revisioned title, directory,
  foreground and activity records. Effects use ordered batch completion and
  acknowledgements. A client must resolve and validate required clipboard,
  image or sample assets before applying an effect or advancing its cursor.

Bulk is a bounded transfer mechanism, not a second message grammar. Its ID,
expected byte count, chunk order, final SHA-256 and any declared decompressed
size are checked under the original operation's admission. Per-attachment
limits in Welcome govern outstanding calls, queued bulk and pending ranges.
A Busy result is capacity admission, not an authorization grant or denial.
Neither an asset budget nor a transport queue bound is a terminal-history cap.

### Clipboard domains

The canonical remote clipboard bundle is defined by
[EffectCodec.h](../Source/Core/Protocol/Sync/EffectCodec.h), with MIME metadata
and binary item bytes. Canonical clipboard reads allow at most 32 items and
4 MiB of aggregate item bytes; clipboard-write effects allow at most 1 MiB.
MIME names are individually bounded at 65535 UTF-8/no-NUL bytes. Large encoded
bundles use admitted bulk; ClipboardRequest/ClipboardData remain correlated to
the exact pane and selected authorized provider. Local clipboard policy still
decides whether a client may read or write its native clipboard.

Native clipboard serving has its own adapter-domain constants in
[NativeAttachment.h](../Source/Core/Protocol/Sync/NativeAttachment.h):
at most 32 types and 4 MiB of aggregate item bytes. These are not an outer
remote frame grammar. A selected provider's refusal does not silently choose
another provider. Clipboard contents and credentials are not history records.

## Separate daemon-local operations

[Local/Control.h](../Source/Core/Protocol/Local/Control.h) defines a distinct
first-packet preamble beginning with `0xF9`, operation and nonzero correlation.
It is selected before the remote decoder, never nested inside Greet, Call or a
pane envelope. The same byte stream cannot switch between the two grammars.

Operation 1 reloads credentials; operation 2 obtains a bounded local QUIC
bootstrap grant; operation 4 serves the local clipboard helper. Authority comes
from the verified kernel peer and the operation's trusted-local/process binding,
not from fields in a packet or a remote token.

Operation 3 selects the descriptor-bearing local session handoff. Its readiness
reply is defined in [Local/Handoff.h](../Source/Core/Protocol/Local/Handoff.h);
the selected exchange uses [Local/AdoptWire.h](../Source/Core/Protocol/Local/AdoptWire.h)
and the ownership gate in [AdoptTransaction.h](../Source/Core/Session/AdoptTransaction.h).

- The sender waits for an exact correlated Ready reply before sending Prepare.
  Prepare binds an independently random operation ID, the serving daemon, and
  the complete `HandoverTerminal` bytes and SHA-256. The descriptor accompanies
  the first byte of that record only; other records carry no descriptor.
- The daemon verifies the current kernel process identity, trusted executable,
  and PTY ownership. It stages an inactive private candidate before replying
  Prepared. Passing a descriptor or knowing an operation ID grants no authority.
- The sender records uncertainty before the first possible Commit byte. The
  daemon reserves the result and activation resources before recording Committed
  and publishing the candidate. A lost reply cannot undo that ownership change.
- Fresh authenticated local channels can request Status or Abort for the same
  operation and daemon. Unknown, Prepared, EOF, timeout, or a changed daemon
  after a possible Commit do not permit the original owner to resume I/O.
  Retained terminal outcomes are not evicted while that physical authority lives.
- The native owner retains the complete unresolved session in bounded escrow;
  closing a pane, tab, window, or application cannot discard it. Only an exact
  outcome permits local resumption or retirement of the old local owner.

The complete terminal includes parser continuation, both screens, physical
history, links and text stores, images, sound and host-only state. A display
checkpoint alone is insufficient. Component wire limits are not an aggregate
decoded-memory allowance.

Operation 5 reserves a separate descriptor-free reverse-transfer exchange in
[Local/ReclaimWire.h](../Source/Core/Protocol/Local/ReclaimWire.h). Its codecs bind
the original adoption, exact pane/attachment, input fence, complete terminal,
unwritten PTY suffix and ownership partition to one capsule digest. Missing input
receipts remain distinct from known zero receipts. The
[ReclaimTransfer](../Source/Core/Session/ReclaimTransfer.h) owner stages that
complete capsule before Commit and permits local installation only after exact
digest-bound release evidence. These codecs are not a live reclaim implementation:
the daemon currently refuses operation 5. The existing `pane.reclaim` call does
not carry the complete terminal or pending input and must not be treated as a
lossless local takeover.

SCM_RIGHTS does not travel on remote Sync or QUIC. Neither local exchange is a
remote compatibility grammar.

## Implementing an independent client

Use the generated
[Emacs constants](../Source/Platform/Emacs/ubiquitty-sync-wire.el) or
[Lua constants](../Source/Platform/Neovim/lua/ubiquitty_sync/wire.lua) as
machine-readable examples, not a second authority. They are derived by
`Scripts/gen-wire-tables.py`. The
[Emacs client](../Source/Platform/Emacs/ubiquitty-sync.el) and
[Neovim client](../Source/Platform/Neovim/lua/ubiquitty_sync/init.lua) implement
the same first-byte protocol without linking C++.

Keep the incremental owner bounded, refuse unknown direction/scope/state before
retaining a declared body, and do not dispatch unauthorized operations. Preserve
stable client/host/attachment/pane identities and validate the selected owner
before displaying Connected. Apply snapshot, history and effects transactionally
under their existing generations and receipts; send Ack only for applied state.
A decoder callback can retire its carrier, so stop consuming that owner's old
tail rather than transferring it to a replacement connection.
