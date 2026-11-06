<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (C) 2026 Stanislav Yaglo -->

# Sync: a coherent, acknowledged, attention-paced remote terminal wire

This document defines the synchronization contract between `ubiquitty-host`
and its clients: the state, invariants, grammar, and conformance requirements.
Statements describe required protocol behavior, not an implementation schedule.
The [wire reference](sync-protocol.md) defines the canonical envelope; numeric
wire assignments and the wire version are recorded in the protocol headers.

Constants marked *provisional* are design bounds requiring workload validation.
The [measurement guide](sync-measurements.md) describes the harnesses and records
the current policy constants. Captured results are kept outside this repository.

---

## 1. Summary

The host Core is the only emulator. A client applies state to a passive
`Terminal`. Around that state, Sync holds these rules:

1. **Frames are coherent.** The host takes a frame only when the parser is at
   ground, no DEC 2026 block is open, no cursor-hidden redraw bracket is open,
   and the PTY read is quiescent. Each frame says why it was taken. The daemon
   publishes through the shared `TerminalRuntime` drain, so local and remote
   panes publish under one rule (`TerminalRuntime::publishSettledFrameIfCoherent`,
   `Source/Core/Runtime/TerminalRuntime.cpp`).
2. **Each attachment has its own acknowledged baseline.** An `Ack` frame
   exists. A lost QUIC datagram costs nothing, because datagram updates are
   supersets based on the acknowledged generation. A client that skipped
   generations receives a diff, never a forced keyframe.
3. **Latest state wins on every carrier.** The sender is window-paced from
   acknowledged bytes and an unbiased RTT, per connection. A full window marks
   the pane *owed* and queues nothing. No fixed frame interval, withhold line or drop
   line exists.
4. **Bytes are proportional to change.** Rows travel as UTF-8 text plus pen
   runs, encoded once per change and shared by every attachment. Margined
   scrolls travel as rectangle ops. Clusters, links, DECDWL rows and palette
   changes cost the rows they touch.
5. **Cost follows attention.** A pane nobody looks at is *dormant* and costs one
   dirty bit. Agents subscribe to records and text, not rows. Phones report
   link and power facts that cap frame rate, `Ack` cadence and history policy.
6. **One SSH stream, two lanes.** No frame body exceeds 65,535 B. Checkpoints, history
   and images travel as chunked bulk transfers behind an interactive lane, so an
   `Ack`, a receipt or a keystroke never waits behind megabytes.
7. **History is geometry-free.** History travels as logical lines with absolute
   numbers and epochs. A resize never resends it, each client wraps it to its
   own width, and resume fetches only the gaps.
8. **Input is intent.** Keys, pointer events and pastes go up as events, and
   the host encodes them against its live modes. Sequence numbers, receipts and
   a stable per-window client identity make resume exactly-once.
9. **Drift is observed.** Every update carries a digest of the state it
   produces. A mismatch is a resync plus a journal entry; no timer resends
   state on a schedule.
10. **Effects and status are typed state.** Clipboard writes reach the user.
    Notifications carry their origin, and one device posts them. Images are
    content-addressed. Program status follows whole-record rules.
11. **One version.** Each peer states the `wireVersion` it speaks, and peers
    connect only when the two are the same (§7.3, "Versions"). Capabilities describe supported
    encodings, not a second version negotiation or permission grant. There is
    no separate snapshot version. The screen-plane envelope itself
    adds no pane multiplexing wrapper; its carrier framing is defined in §7.2.
12. **The session outlives the transport and the client process.** An
    attachment is resumed, not reattached, wherever the resume comes from: a
    redial on the same process, a relaunch after the OS killed the app, or a
    still-open transport the host has not yet noticed is dead. Input is
    delivered exactly once across all three, and a host upgrade hands live
    panes to the new daemon (§6.13).

### Tradeoffs

State delivery avoids client emulation drift but cannot eliminate the round trip
and coherence hold of raw-mode input. Coherence is a bounded heuristic and must
be checked against a replayable corpus. Unicode widths are part of the wire
contract. Resume records contain screen text and require encryption and purge
semantics. Host handover is a separate same-host transfer with an explicitly
versioned body; it is not another remote-wire version.

## 2. Goals and non-goals

### Goals

1. **Correctness first.**
   - No client shows a torn frame: no half-drawn redraw, and no frame taken
     inside a cursor-hidden repaint.
   - No client shows a silently wrong screen, because divergence is detected on
     the frame that causes it.
2. **Felt latency.**
   - A keystroke echo costs one RTT plus at most one coherence hold.
   - Ctrl-C becomes visible within about one RTT even under a flood, because
     catch-up jumps to the newest state instead of draining a backlog.
3. **The phone pays for what it shows.**
   - Bandwidth is bounded by screen size times the client's frame rate, not by
     program output volume.
   - Background panes, history nobody scrolls to, and duplicate notifications
     cost nothing.
4. **Host cost per extra client is near zero** when clients share a generation.
   Each attachment costs a memcpy of cached row encodings.
5. **Resume, not re-attach.** Screen, history and input continue across a
   network change, a client process relaunch and a planned host upgrade.
6. **Placement is invisible to the frontend.** One `PaneLink` API serves local
   and remote panes.
7. **One version rule, and a grammar that does not run out of bits.**

### Non-goals

- **A client-side parser fed by PTY bytes.** The grammar reserves the identity
  it would need (§11); Sync does not build it.
- **Compatibility with a peer at another wire version.**
- **A hosted relay or a built-in mesh network.** SSH is the spine. QUIC is the
  pinned, token-gated carrier (`Docs/quic-trust.md`). Networking between
  machines belongs to the user.
- **Per-client live-screen geometry.** One PTY has one size. History, which is
  geometry-free, can be viewed at any width.

---

## 3. Principles

1. **One emulator, many views.** Only the host Core turns PTY bytes into state.
   A client renders a passive `Terminal` plus a history cache.
2. **A frame is taken only at a coherent point, and says why.** Coherence is
   decided host-side from facts only the parser has.
3. **Diff from what the peer confirmed, and send only the newest state.**
   Catch-up is a jump to now, never a replay of a backlog. A sender that knows
   which state the receiver acknowledged can always reach the newest state in
   one diff.
4. **Prove convergence on every frame.** Every update carries a digest of the
   state it produces. This is the [single-owner rule](../AGENTS.md#architecture-and-correctness)
   applied across machines.
5. **Validate whole, then mutate. Cap everything a program can grow.** Apply is
   transactional, and a cap is checked before the state it guards changes.
6. **Identity is typed, immutable and never reused. Structure moves by
   revision.** A record is replaced whole, keyed by its revision.
7. **Every client owns its viewport.** Scroll, selection, search and history
   browsing never move another client's screen.
8. **History has no geometry.** Lines are logical, numbered absolutely and
   grouped in epochs. A resize never touches them.
9. **Input is intent. Encoding belongs to the Core that owns the modes.**
10. **Cost follows attention.** A frame is sent only to someone who can see it,
    at a rate their display and link can use.
11. **Interactive never waits for bulk.** No frame body exceeds 65,535 B, and
    bulk payloads are chunked behind the interactive lane.
12. **State, not presentation.** Status, progress, cwd and foreground process
    are typed records; the frontend presents them. Their lifetimes follow
    events the Core already sees, process exit and prompt start, never
    heartbeats.
13. **Unknown means ignore, except for state.** Unknown control-plane fields are
    ignored. An unknown *enumerated state* invalidates the whole record.
    Screen-plane frames are read at the version the peers speak and fail
    closed; the sender never sends a newer kind (§7.3, "Versions").
14. **Authority keys on the principal, not the transport.**
15. **The host answers every program query, synchronously.** A program never
    waits for a network round trip.

### 3.1 Security and framing invariants

These are cheap and make recovery trivial:

- A checkpoint is self-contained.
- Apply is transactional: validate, then mutate.
- Integers are written field by field, never by memcpy.
- History precedes the screen update that scrolls it.
- The DEC 2026 hold is the host's decision alone.
- Every client-to-host kind has a fail-closed role gate in
  `Source/Core/Protocol/Sync/FrameGates.h`. The connection also checks direction,
  phase, channel and view; the Host checks credential scope and operation authority.
- SSH authenticates and QUIC carries. A resume continues an attachment that SSH
  authorised; it never creates a principal, a pane or a shell (§6.9, D16).

### 3.2 Why state goes down, not bytes

Sync sends host state rather than PTY bytes, so a client needs no copy of the
host's Core and its cost follows changed cells rather than program output;
§11 reserves a log mode that forwards PTY bytes to matched clients.

---

## 4. Architecture

```
host: ubiquitty-host (one poll loop)                               client
┌──────────────────────────────────────────────────────────────┐  ┌──────────────────────────────────────┐
│ PTY ─▶ Runtime::runDrainPass ─▶ Core                          │  │ RemotePaneLink                        │
│          (parser, grid, LineStore, status records, effects)   │  │  ├─ passive Terminal (live screen)    │
│          │                                                    │  │  ├─ HistoryCache (logical lines;      │
│          ▼ coherence gate (§6.1) ─▶ generation G              │  │  │   memory; optional encrypted disk) │
│   Publisher (per pane, shared by every attachment):           │  │  ├─ Predictor overlay (§6.7)          │
│     row hashes, cached row encodings (memcmp-confirmed),      │  │  ├─ StatusView (facts, records)       │
│     touched-row ring, dictionaries, image store by hash,      │  │  └─ Effects sink (primary-aware)      │
│     effect log stamped by generation                          │  │             ▲                         │
│          │                                                    │  │ Receiver: validate → apply → digest   │
│          ▼                                                    │  │   check → Ack (RTT/power-aware)       │
│   Sender, per attachment: view state, sent/acked generation,  │◀═▶ Input: KeyChord / pointer / paste    │
│     row-hash vector, dictionary marks; per connection:        │SSH│   events with (clientInstance, seq)  │
│     pacing (srtt, delivery rate, window), owed panes, lanes   │/  │ Facts: viewport, cell px, refresh,    │
│          ▲                                                    │QUIC  link, power, appearance          │
│   Input: events ─▶ Core encoder (live modes) ─▶ PTY           │  │ Control: JSON-RPC (shared method      │
│   Control: JSON-RPC methods shared with the control socket    │  │   table with the control socket)      │
└──────────────────────────────────────────────────────────────┘  └──────────────────────────────────────┘
                                                                   LocalPaneLink: same API over an
                                                                   in-process TerminalRuntime; no wire
```

| Component | Module | Why there |
|---|---|---|
| Coherence gate | `Source/Core/Runtime/` | One pass and one rule for local and remote. `Daemon::servicePTY` reads through the drain pass `TerminalRuntime::drainAvailable` reads through (`Runtime::runDrainPass`), and `Daemon::publishFrameIfAdmitted` takes a frame only when `FrameCoherence` admits it; `Tests/Host/test_daemon_core_rules.cpp` holds the corpus coherence gates through that path. |
| Row codec, hashes, dictionaries, digest | `Source/Core/Protocol/Sync/` | Portable, so it is unit-testable and fuzzable host-side. |
| Publisher, sender, pacing, lanes | `Source/Core/Protocol/Sync/` (portable, with a fake clock) and `Source/Platform/Host/Daemon.cpp` (sockets) | Pacing is pure arithmetic over acks and is tested without sockets. |
| Receiver and `HistoryCache` | `Source/Core/Protocol/Sync/`, with adapters in `Source/Core/Session/` | Swift and iOS consume the session API through cxx-interop ([interop guide](swift-cxx-interop.md)); the protocol layer must not depend on frontend or session orchestration. |
| Program-status records, effect log | `Source/Core/OSC/` and `Terminal` | Programs observe them, so they belong to the Core ([Core/frontend boundary](architecture.md)). |
| `PaneLink` | `Source/Core/Session/` | The placement-neutral seam. |

---

## 5. Identities and versions

| Identity | Form | Scope | Notes |
|---|---|---|---|
| `hostInstanceID` | ID128, time-ordered | one daemon lifetime | Pins resume to one daemon instance, or to the predecessor it took a handover from (`Welcome.predecessorHostInstanceID`, §6.13). |
| `paneID` | ID128 | never reused | Labels are mutable and separate. Survives a host handover. |
| `clientInstance` | random 128-bit, one per client window | stable across reconnects, network changes and process relaunches | Persisted with the window's restoration identity (on iOS, the scene session). Keys input deduplication and resume. |
| `attachmentID` + `resumeSecret` | ID128 + u128 | one attachment incarnation | Issued by `Welcome`. A resume consumes the secret; every `Welcome` issues a fresh one. Only `Welcome` issues the secret; on SSH stdio `Greet.resume` carries it back, and on QUIC it is a MAC key that is never sent after `Welcome` (§6.9, §7.1). |
| Resume record | encrypted file, one per client window | survives a process relaunch | Holds the resume identity, the certificate pin, `[PaneResume]` and the last acknowledged picture (§6.9). Encrypted with the D6 key and purged on `HistoryReset`. |
| Pane handle | varint | per connection | `PaneBind` binds it to a `paneID`. A handle is bound again only once an `Ack` counts past its `PaneUnbind`, so no frame the client wrote for the old pane is taken for the new one. |
| `generation` | u64 per pane | never reused within a host instance | One per coherent frame taken. |
| `historyEpoch`, line number | u32, u64 | per pane | The line number counts completed logical lines: a stored row that is not soft-wrapped completes one (`Source/Core/Screen/HistoryLines.h`). A reflow rewraps rows and keeps every number on its line, which a row count could not. The epoch changes only on a destructive clear, and only when a line was numbered in the epoch it ends; numbers never decrease across epochs. |
| `dictionaryEpoch` | u32 per pane | per pane | Rolls when a dictionary reaches its cap. |
| `topologyRevision`, `statusRevision`, `factsRevision`, `placementsRevision` | u32/u64 | per record | Whole-record replacement by revision. |
| `wireVersion` | u16 | protocol | The version a peer speaks; peers at different versions are refused (§7.3, "Versions"). It also pins the Unicode width-table version (§7.4). |
| `engineFingerprint` | 32 B (`wireFingerprint`, the SHA-256 of every body layout) | protocol | Carried in `Greet` and `Welcome`; peers whose fingerprints differ are refused like peers at different versions (§7.3, "Versions"). |

---

## 6. Mechanisms

### 6.1 Coherence: when the host takes a frame

A fixed dirty-pane interval alone is insufficient for coherence. A publish rule must account for
three ways:

1. the tick fires while the parser is mid-sequence;
2. a TUI hides the cursor (DECTCEM reset), repaints across several writes and
   shows the cursor again, and a frame lands inside that window — the usual
   full-screen editor shape represented in the corpus;
3. a redraw larger than one PTY read arrives as two reads with a would-block
   between them.

**Rule.** The host takes generation `G+1` for a pane when every condition
holds:

1. The pane is dirty since `G`.
2. The parser is at ground. This is the predicate in
   `TerminalRuntime::publishSettledFrameIfCoherent`.
3. No DEC 2026 block is open, unless its own timeout has fired
   (`Modes::SyncOutput::shouldWithholdFrame`, `Source/Core/Modes/SyncOutput.cpp`).
4. **No redraw bracket is open.**
   - A bracket opens when DECTCEM goes from shown to hidden while the pane is
     dirty, and closes when the cursor is shown again.
   - A cursor already hidden when the last frame was taken opens no bracket.
     Programs that keep it hidden (process monitors, pagers, component-rendered
     agent TUIs) are never held.
   - A bracket open longer than the deadline `D` is *persistent*. It stops
     holding until the cursor is next shown. A TUI that hides the cursor during
     an input burst therefore never adds `D` to every echo.
5. **Read quiescence.** The PTY read returned would-block, nothing was read
   for the quiescence window since, and one more zero-timeout `poll` of the PTY
   finds nothing. The poll alone can split a repaint between reads, so the rule waits a window
   (2 ms, `readQuiescenceMicroseconds`) and a holder of a frame keeps a timer
   to ask again when it ends; that costs one wake per settle. The window is
   waived for a DEC 2026 block closed since the last frame, which the program
   has declared complete, and for a writer still writing one refresh after the
   pane went dirty, whose frame is `Flood`.

**Deadline.**

- If dirty state has waited longer than the attachment-independent budget `D`,
  the host takes the frame and marks it `Forced`. `D` defaults to twice the
  fastest attached refresh interval, about 33 ms at 60 Hz (provisional).
- A saturated drain pass publishes at the pacing rate, marked `Flood`
  (`SyncFrameReason::SaturatedPass`, `Source/Core/Runtime/TerminalRuntime.h`).
- No two frames are taken closer than the refresh interval, in any class. A
  frame the pacing holds is deferred, never dropped. A host that paces frames
  itself (the local drain host, to its display) passes its own interval.
- A frame may be taken in any parser state: under a deadline, a flood, or a
  hostile program that never returns to ground. This is safe because the
  client never parses, so a checkpoint never has to restart a parser.

**Frame class.** Every frame has one of five classes: `Boundary` (2026
commit), `Settled`, `Flood`, `Forced` or `Structural` (resize, reset,
checkpoint). An `Update` carries one of the first four; `Structural` travels
only on a `CheckpointHead` (§7.4).

- The predictor confirms only against `Boundary` and `Settled` frames.
- Per-class counters and a hold-time histogram go to the halt journal.
- Two gates fail the corpus run (provisional thresholds): a `Forced` share above
  1%, and a p99 hold after input above one frame interval.

Taking a frame and sending it are separate decisions. The host takes frames
at most once per fastest attached refresh interval; §6.4's per-connection
pacing decides which of them each connection receives, at most one update per
refresh interval per pane on that connection.

### 6.2 Generations, row hashes and the state digest

- **Row hash.** Each visible row has a canonical encoding (§7.4) and a row hash,
  `XXH3-64` over that encoding.
- **Dirty detection.** The RenderPlan per-row digest
  (`Source/Core/Rendering/RenderPlan.h`, `buildFrameDigests`) selects candidate
  rows. That digest is a cache hint, not a correctness boundary, so:
  - a row whose digest is equal is confirmed by a `memcmp` against the previous
    grid row before its cached encoding is reused;
  - a row whose digest differs is re-encoded and re-hashed.
  Canonical work is O(dirty cells). The O(rows×cols) digest pass already runs
  per publish.
- **State digest.** `XXH3-128` over the row-hash vector of both screens, the
  cursor block, and the modes, palette and placements revisions.
  - The client computes it from *the bytes it received*: it hashes each row's
    received encoding and shifts hashes for scroll ops. It never re-encodes.
  - A mismatch triggers `Resync{DigestMismatch, sectionHashes}` (§8, F1).
- **History is not in the digest.** It travels on its own plane (§6.6), and
  `Ack.historyAcked` and the line hashes are its check.
- **Touched-row ring.** For each of the last `K = 64` generations The host keeps
  a bitset of the rows written: about 480 B per pane at 60 rows. Superset diffs
  are built from it (§6.3). Beside each bitset it keeps `dirtySince`, the host
  time of the first change in that generation, which `Update.lagMicros` reads
  (§7.4).
- **Diagnostic builds** keep every update since the last good digest, so a
  divergence becomes a replayable unit-test fixture.

Drift is observed on the frame that causes it. No timer resends a keyframe on
a schedule.

### 6.3 Per-attachment baselines: chain and superset

Each attachment on a pane holds:

- `sentGeneration` and `ackedGeneration`;
- `sentRowHashes`, one per row;
- one watermark per dictionary;
- its view state (§6.8).

Pacing is not per pane: it belongs to the connection (§6.4).

**Chain mode** is used on reliable lanes.

- The base is `sentGeneration`. An update applies only onto exactly that base.
- If the base is the previous published generation, the host emits **span
  diffs** against the retained previous grid.
- Otherwise the client skipped generations. The host compares `sentRowHashes`
  with the current hashes and emits:
  - **whole rows** from the cached encodings, one memcpy per row per
    attachment;
  - **`ScrollRect` ops** found by hash alignment. A run of `k ≥ 2` rows whose
    hashes equal the old vector shifted by `n` inside one rectangle becomes one
    op. This recovers DECSTBM and DECSLRM margined scrolls from any client
    state.

**Superset mode** is used on QUIC datagrams.

- The base is `ackedGeneration`. The update carries every row whose bit is set
  in the touched ring between `base+1` and the target generation.
- It applies on any client state `g` with `base ≤ g < target`, so a lost or
  reordered datagram costs only the next update.
- It carries no scroll ops, because they are not idempotent.
- The host sends a chain update on the reliable lane instead when
  `target − base > K`, or when the encoded update exceeds the datagram ceiling
  (`quicDatagramMaxBytes = 1100`, `Source/Platform/Host/HostQUIC.h`).

`ackedGeneration` moves only on an `Ack`. An attachment is never marked synced
because a datagram was queued.

### 6.4 Pacing: latest state wins on every carrier

- **Scope.** Pacing state is kept per connection, matching the connection-wide
  `Ack.bytesReceived`. The window counts every host-to-client frame on every
  plane, not only screen updates, so records and effects cannot fill the
  remote SSH server's buffer of a suspended reader behind the window's back.
- **RTT.** Each update carries `hostTime` in microseconds. Each `Ack` echoes it
  with `holdMicros`, the time the client held the `Ack` before sending it.
  `rtt = now − echo − holdMicros`: one clock, no clock sync, and no bias from
  the client's ack coalescing.
- **Idle probing.**
  - `Probe` and `Echo` run in both directions. A `Probe` carries the sender's
    clock and the `Echo` returns it with `holdMicros`, so each side measures
    RTT on its own clock. On QUIC both travel as datagrams.
  - After screen data stops, the host sends `Probe` at 2 s, then 4, 8 and
    16 s, then every 30 s (provisional; `Limits.probeScheduleMicros`). Any
    screen data or input resets the schedule. The probe keeps the RTT fresh
    and lets the host see a dead path on an idle connection; the backoff lets
    a phone's radio sleep between probes.
  - Probes go only while some attachment on the connection is `Live` and
    `Facts.focused` is set. A connection whose panes are all `Dormant` or
    `RecordsOnly` is never probed, so a backgrounded phone's radio stays
    asleep.
  - An `Echo` counts as an `Ack` for The 30 s `Lost` rule.
  - An estimate older than 10 s drops the window to its floor, so a
    frame after an idle period is never paced from a stale RTT.
  - The client sends its own `Probe` when it suspects the path (§6.9).
  - The 30 s runs from the first send after the client was last
    heard, and only while bytes or a probe await an answer, so an idle
    connection is never lost and one whose `Echo`s arrive never is either.
    The host answers a run of probes with one `Echo`, of the newest, and takes
    only the `Echo` of its outstanding probe as a sample.
- **Delivery rate.** Acknowledged bytes per ack interval, max-filtered over
  10 s (provisional).
- **Window.** At most `max(floor, 2 × rate × srtt)` unacknowledged bytes on
  the connection, across every plane. The floor is 256 KiB; use the shaped-link
  harness in `Docs/sync-measurements.md` to validate pacing changes.
  - When the window is full, the host does not encode and does not queue. It
    marks the pane **owed**.
  - `PaneFacts` and `StatusRecords` go owed like updates. They are
    whole-record, so the newest revision is sent when the window opens.
    Effects coalesce across the owed span under §6.11's rules.
  - When an `Ack` opens the window, the host encodes, for each owed pane, one
    fresh update from `sentGeneration` to the newest generation.
- **Owed order.** When the window opens, owed `Live` panes are served in this
  order:
  1. any pane holding input whose `inputSettled` has not reached its newest
     `inputSeq`;
  2. the rest, by deficit round-robin over encoded bytes.

  A flooding pane therefore never delays the echo in the pane beside it. A
  busy pane's intermediate generations are skipped, not rationed.

  Deficit round-robin serves the owed pane furthest behind the most-served
  pane in bytes, capped at one quantum of one bounded frame. Service stops
  at the first frame the window refuses. Whole replacement records coalesce
  while owed; events follow their explicit retention rules.

- **Rate cap.** At most one update per `Facts.refreshIntervalMicros` per pane
  on each connection, so a 120 Hz desktop and a 60 Hz phone on one pane each
  get their own rate. `Facts.power = Saver` halves the cap; `Facts.link = Constrained`
  caps it at 20 Hz (provisional).
- **`Ack` cadence** is RTT-, link- and power-aware:
  - `ackInterval = clamp(srtt / 4, 4 ms, 50 ms)`;
  - under `Facts.link ∈ {Metered, Constrained}`,
    `ackInterval = clamp(srtt / 2, 20 ms, 100 ms)` (provisional);
  - doubled under `Saver`;
  - immediate after a `Checkpoint`;
  - never sent while idle;
  - a pending `Ack` leaves in the same carrier write as outgoing input. A due
    `Ack` is never held back for input.

  A fixed short coalesce interval keeps a cellular radio's uplink awake; this
  cadence does not, and `holdMicros` keeps RTT exact when acks are delayed. At
  an srtt of 100 ms the unmetered interval sends up to 40 uplink `Ack`s a
  second during a token stream; the metered interval sends at most 20.
- **SSH stdio.** The window is computed from application acks
  (`bytesReceived`), so it bounds queuing in the remote SSH server's buffer,
  which `TCP_NOTSENT_LOWAT` cannot see through the stdio relay. On direct TCP and unix
  sockets the host also sets `TCP_NOTSENT_LOWAT`. On direct TCP it sets
  `TCP_NODELAY` as well, so the kernel never holds a small write back behind an
  unacknowledged segment for a delayed-ACK interval.

  No carrier is direct TCP. The daemon's peers are a unix socket
  (local clients, and the SSH relay's far end) and QUIC, and
  `TCP_NOTSENT_LOWAT` and `TCP_NODELAY` are TCP options with no socket to set
  them on; the window bounds the queue in each. The `Ack` interval's round trip
  is the client's own, from its `Probe`s (§6.9); before it has one the client
  holds an `Ack` for the interval's floor, not its ceiling: early `Ack`s cost a
  few bytes, and they let the window pace the first frames, and the `Welcome`
  probe gives a sample within a round trip anyway. Bytes that carry only the
  host's `Probe`s owe no `Ack`, since the `Echo` answers them: an idle client
  wakes its uplink once per probe.

Queue length must not substitute for acknowledged-state pacing. A connection that holds
unacknowledged bytes, a `Probe` included, and receives no `Ack` or `Echo` for
30 s moves its attachment to `Lost` (§6.9). It is never dropped by queue size.
The client's liveness check (§6.9) usually detects a dead path long before
that.

### 6.5 Lanes: interactive never waits for bulk

One SSH stream multiplexes everything; lanes keep it from blocking.

- **Every frame body is at most 65,535 B.** Its canonical header adds at most
  9 B. A larger payload travels as a **bulk transfer**: bounded chunks, with a SHA-256 over the reassembled
  payload in the last chunk (`Source/Core/Crypto/SHA256.h`).
- At 1 Mbit/s a maximum-size envelope occupies the stream for about 524 ms.
  Large payloads cannot become a single multi-megabyte remote frame.
- Each kind has a fixed lane, implied by the kind. The `Lane` column of the
  §7 tables is authoritative; in outline:
  - **Interactive lane:** control, `Ack`, `Probe`/`Echo`, receipts, `Keys`,
    `Pointer`, `Raw`, `Focus`, `PasteAbort`, `View`, `Resync`, `Update`,
    `CheckpointHead`, `PaneDirty`, `PanePalette`, `BulkAbort`, the history heads and wants,
    image heads and wants, clipboard reads, `PaneFacts`, `StatusRecords`,
    effects.
  - **Bulk lane:** checkpoint chunks, history chunks, image chunks, large
    topology, and, upward, `Paste` chunks.
- **Scheduler.** Interactive frames go first. Bulk is guaranteed 25% of sent
  bytes whenever bulk is pending (provisional), so a checkpoint is never starved
  by a flood of updates. On QUIC each lane is its own stream, and the guarantee
  holds per connection.
- **Upward bulk.** The client runs the same scheduler. `Paste` chunks travel in
  its bulk share behind `Keys`, `Pointer`, `Ack` and `Echo`, so a keystroke and
  an interrupt never wait behind megabytes of paste (§6.7).
- **One write per publish.** Every frame for a connection from one publish
  pass leaves in one gathered write or one QUIC stream write, up to the window.
  A keystroke echo (`Receipt`, `Update`) is one packet.
  The daemon's loop publishes every pane, services every
  connection and only then flushes, so a pass's frames for a client share its
  output buffer and leave in one `write` or one QUIC stream hand-off.
- **Bulk is encoded lazily**, one chunk at a time as the scheduler admits it.
  No transfer is materialised up front.

  A checkpoint body is one generation, so it is encoded whole,
  when it is about to leave: one checkpoint body is in the lanes per
  connection at a time, and the next a pane owes is cut as the lanes take the
  last chunk of the one before. However many panes owe a checkpoint, the
  connection holds one body.
- **Per-attachment bulk caps** (§7.9) bound queued bulk bytes, pending
  `HistoryWant` ranges and outstanding `Call`s. A `Call` over a cap is
  answered `Result{busy}`. An over-cap `HistoryWant` is refused whole, and
  silently, because `HistoryWant` has no reply: `Limits` advertises the cap, so
  a conforming client never reaches it, and one that does re-sends a want
  within the cap (§7.9). The attachment stays connected. An agent
  calling `pane.capture` in a loop therefore grows neither host memory nor its
  claim on the bulk share of other attachments.
- **Bulk in flight.** bulk bytes are admitted by the window and
  also by a bulk window of `max(one capped frame, 2 × rate × srtt)`, so the
  window's floor never queues a floor's worth of bulk in a relay ahead of the
  interactive frames after it; that is what bounds F7 by one capped frame's
  time on a link whose window the floor sets.
- **`BulkAbort`.** The host may abandon a transfer it has started. It sends
  `BulkAbort{transfer}`, sends no further chunk of it, and the client frees the
  partial reassembly.
  `BulkAbort` is kind 12, a connection kind. The checkpoint is
  a transfer whose latest pane want wins: a
  `Resync`, a `View` leaving `Live` and an unbind each abort the body in
  transfer, and the pane owes a fresh checkpoint. The abort follows its head on
  the interactive lane, so it never overtakes it, and the client closes
  `Malformed` on one naming a transfer no head announced.
- A **`Checkpoint` is bulk but atomic to the receiver.** The client applies it
  only when every chunk has arrived and verified. Updates that arrive meanwhile
  chain after the checkpoint's generation and are buffered, bounded at 1 MiB;
  overflow causes a `Resync`.
  A refused checkpoint is asked for again at once, then after
  250 ms doubling to 8 s, and after six refusals in a row the client stops,
  reports the pane failed with the reason and holds it `RecordsOnly`. The host
  keeps the state digest of a body the client refused; one refusal may be a
  body the path spoiled, so the state is sent once more, and after a second
  no body of that state is sent again; the pane owes nothing until it publishes another
  generation, and the daemon logs the withholding.

### 6.6 History: logical lines, epochs, and a pull-first policy

- **Address.** A history line is addressed by `(historyEpoch, lineNumber)`.
  - `lineNumber` counts completed logical lines. A line takes the number
    `head` held when its last row left the screen, and `head` advances. A
    reflow never renumbers a line; it moves `head` only as far as it moves
    the seam, and advances `oldest` by whole lines when the ring holds fewer.
  - `historyEpoch` increments only on a destructive clear: ED 3, RIS, the
    clear-buffer verb, the clear commands that drop history, and the clear to
    the previous mark. A clear in an epoch where no line was numbered opens
    none, so a `clear` repeated at an empty prompt costs nothing. Resize,
    reflow and eviction never change it.
  - The new epoch's `clearedThrough` is the highest head the old epoch
    reached, counting numbers a pull-back withdrew, since a client may still
    hold those lines. The lines that survive a partial clear are renumbered
    from it, and RIS carries it across the reconstruction. A number below
    `clearedThrough` names nothing in the current epoch.
- **Head retreat.** A taller screen pulls rows back and retreats `head` by
  whole lines; the client hides the lines at or above it. A later shrink
  pushes the same lines out under the same numbers, and a line whose hash is
  what it was costs nothing. A line edited while it was on the screen is
  another line, and is sent again.
- **Logical lines.** On the wire, a history entry is the physical rows of one
  soft-wrapped line joined together, in the canonical row form with no width,
  plus a 64-bit line hash.
  - The host emits the entry when the line's last physical row leaves the
    screen.
  - **The open line.** A line whose first rows have left the screen and
    whose last has not straddles the seam. Its rows so far travel as the open
    line: flagged `Open`, under the number `head` it takes when it completes,
    alone in a body (§7.5). The completed line, or a longer open line,
    replaces it in place; one with no cells withdraws it. A pane has at most
    one, sent with the appends, at most one per update; a screen that holds
    still sends it after the checkpoint. The client shows it after the
    newest run, its last row continuing onto the screen.
  - Each client wraps history at its own width.
  - During a drag-resize the client re-wraps its cache at once. Only the live
    grid waits for the host's `Structural` checkpoint.
- **Appends.** Every update carries `historyHead`. Lines between an attachment's
  `historyAcked` and `historyHead` precede the update that scrolls them off.
  This holds in every view state except `Dormant` and `RecordsOnly`.
- **The client's cache size.** `Greet.historyCacheLines` is how many lines of
  Each pane's history the client keeps. A client that keeps none, as the
  editors' clients, is sent no history but `HistoryReset`. No backfill goes
  deeper than the client keeps.
- **Backfill follows `Facts.link`.**
  - **`Unmetered`:** after a checkpoint, history is sent newest-first in chunks
    of at most 256 lines, using the bulk lane's spare share, down to a
    client-chosen depth (`Greet.backfillDepthLines`, default 2,000 lines).
  - **`Metered` or `Constrained`:** nothing is pushed beyond the first viewport
    above the screen: the attach is sent `[head - viewportRows, head)`, and an
    update's appends only the newest viewport's worth, the older a gap. The
    client pulls with `HistoryWant` when the user scrolls or searches, and a
    `Backfill` want is ignored.
  - A link that turns metered aborts the backfill in flight; one that turns
    unmetered resumes it from the current head.
  - **On demand, in every mode:** `HistoryWant{ranges, priority}` moves a range
    to the front of the bulk lane.
  - **Where the link's cost comes from.** The platform reports the network
    path's cost as a fact (`PAL::NetworkCost`): Apple's `NWPath` is
    `Expensive` when `isExpensive` and `Constrained` when `isConstrained`;
    a platform with no source reports nothing and the link stays
    `Unmetered`. Core states `Expensive` as `Metered`, sends a `Facts` when
    the cost changes, and carries it in every later `Greet`.
- **The client pulls on scroll.** A frontend reports where its view sits
  (`PaneLink::setViewOffset`) and Core decides. A view within one screen of
  the oldest row the replica projects sends one `Viewport` want for the gaps
  in the two screens of lines just older than the newest run it holds. The
  range depends on the run, not on where in that screen the view is, so a
  view moving there sends nothing more until the lines arrive and the run
  grows; a host holding nothing older is not asked again. A replica whose
  ring is full asks for nothing, and a new epoch or a reconnect forgets the
  want sent.
- **Latest want wins.**
  - A `HistoryWant` replaces the client's outstanding set at its `priority`;
    it never adds to it. Flicking through a long transcript is therefore a
    jump to the newest viewport, not a backlog the metered user pays for
    (principle 3).
  - The host drops unsent chunks of transfers that intersect no wanted range,
    and sends `BulkAbort` for each. A transfer still wanted is not owed again.
  - A want is clipped to `[max(oldest, clearedThrough), head)`. The host
    holds at most one transfer per priority, cut once the one before has left
    and sized to the queued bulk's room; `Viewport` leaves before `Search`,
    and `Search` before `Backfill`. A client's `Backfill` want replaces the
    backfill the host chose.
  - `Viewport` and `Search` wants are never rate-limited. `Backfill` already
    follows `Facts.link`.
- **Host-side search.** The control method `pane.search{pattern, range}` runs
  over the host's full history, so search is not limited to what a phone has
  fetched. A match is `(line, startCell, endCell)` in the epoch the result
  names, its cells counted in the line as the history plane carries it, so a
  match across a soft wrap is one match and survives a reflow; the client
  fetches a match's line with a `Search` want. The Core search is
  `ScrollSearch::searchHistory`; §7.3's `Call` and `Result` carry the method.
- **Clear is a purge, without discretion.** `HistoryReset{newEpoch,
  clearedThrough}` obliges the client to purge every cached line of the old
  epoch, including on-disk copies and the persisted resume record (§6.9),
  before it applies anything else. A clear is a user's privacy action.
  - The host sends it ahead of everything else the pane sends, in every view
    state, and with it the lines that survived a partial clear as appends.
  - A checkpoint names its epoch too. One naming a later epoch than the
    client's purges first, as a reset would; one naming an earlier epoch is
    a frame in flight before the reset, and leaves the client's epoch alone.
  - Local adoption carries one complete `HandoverTerminal` record, not a
    display checkpoint followed by network history bodies. Its history
    section preserves every retained physical row, the ring placement, epoch
    and numbering continuation, alongside the terminal's parser, dictionaries,
    screens, images and host-only state. Both endpoints apply independent
    finite component limits and a maximum 36 MiB encoded-record limit. A
    terminal that cannot fit is refused whole; history is never truncated to
    make a handoff succeed. Unfinished parser, image-transfer or rewrap work
    is refused without being flushed or completed during capture.
  - Adoption selects local control operation 3 before exchanging separate
    `Prepare`, `Commit`, `Abort`, and `Status` records. Only `Prepare` carries
    a descriptor, accompanying its isolated first byte. It names independently
    random operation and daemon identities and a length/digest-bound whole
    terminal body. Record correlations identify replies, not process authority.
  - The daemon retains an inactive candidate after `Prepared`. It reserves
    registry and reply capacity and freshly rechecks the physical process,
    executable, PTY session-leader birth, ancestry, and nonblocking state before
    recording `Committed`. Publication follows the decoder callback, including
    when the requester has disconnected. `Status` and `Abort` may use a fresh
    authenticated local channel without another descriptor or `Prepare`.
  - The sender freezes local I/O before capture and records uncertainty before
    sending any `Commit` byte. EOF, refusal, `Unknown`, a repeated `Prepared`,
    or a different daemon never permits resuming after that point. A bounded
    process-owned escrow retains the complete local session while unresolved;
    close and normal process exit must wait for exact terminal evidence.
  - The local admission policy retains at most 64 physical authorities and
    4,096 operation outcomes, with no live-authority eviction. Prepared leases
    use twice the maximum preparation-answer allowance (currently 76 seconds).
    Expiry only destroys an inactive candidate. Terminal evidence retires only
    after positive kernel exit evidence for the exact original process and
    removal of every admitted local channel, with no activation still pending.
    These bounds do not establish aggregate decoded-terminal memory admission.
- **Epoch fence.**
  - The client discards any `HistoryInline`, `HistoryHead` or history
    `BulkChunk` whose epoch is not its current epoch. A history chunk's epoch
    is the epoch of the `HistoryHead` that opened its transfer.
  - On `HistoryReset`, the host aborts every in-flight history transfer of the
    old epoch with `BulkAbort`.
  - A late chunk can never repopulate a purged cache.
- **Client cache.**
  - `HistoryCache` is keyed by `(hostInstanceID, paneID, historyEpoch)`.
  - It is memory-only by default; on-disk persistence follows decision D6.
  - Its cap is independent of the host ring.
  - Lines older than the host's oldest line may be kept for display and local
    search, flagged *beyond host horizon*. They are never requested.

### 6.7 Input, receipts and prediction

- **Input events.** The client sends intent, never bytes encoded against
  mirrored modes:
  - `Keys` carries `KeyChord` events.
  - `Pointer` carries cell and pixel coordinates, buttons and kind. The Core's
    pointer ladder encodes it per mouse mode.
  - `Paste` carries UTF-8. The host applies bracketing and normalisation under
    live modes (`Source/Platform/PasteNormalize.*`). Its chunks travel in the
    client's upward bulk share (§6.5).
  - `Raw` carries bytes, for agents.
- **Paste abort.** Ctrl-C never waits behind megabytes of upward paste.
  - The host writes a client's input in `inputSeq` order, so a key typed
    during an unfinished paste follows the paste, unless it aborts it.
  - A key that matches the live `VINTR`, `VQUIT` or `VSUSP`
    (`EchoContext.interruptChars`), typed while a paste is unfinished, makes
    the client drop its unsent paste chunks and send `PasteAbort` under the
    paste's `inputSeq` on the interactive lane, before the key. The abort is
    its own kind rather than a `Paste` state because `Paste` rides the upward
    bulk lane, where an abort would wait behind the chunks it cancels.
  - The host stops writing the paste and discards any of its chunks still in
    flight. If a bracket is open, it writes the bracketed-paste end marker, so
    the program is never left in paste mode, and then writes the key.
  - The bracket opens at the paste's first chunk when DECSET
    2004 is live then. The normaliser (`Source/Core/Input/PasteNormalize.h`,
    `PasteNormalizer`) carries a CR/LF pair and a split fence across chunks,
    so an end marker cut between two chunks is neutralised as one inside a
    chunk is. A paste counts as written, for `inputWritten`, once its end is.
    A client whose attachment is freed mid-paste, detached or past its
    grace, with no other attachment, has its paste given up: the end marker
    is still written if the bracket opened. The host's caps travel as the
    `Welcome`'s `pasteChunkBytes:u32, pasteBytes:u32`, after the resume's
    fields, and `pasteWindowBytes:u32` after them; a client sends no `Paste`
    to a host that states none.
  - A paste is windowed by its client against the host's
    receipts. The host states a window in its `Welcome`
    (`pasteWindowBytes`, 256 KiB, a measured constant) and sends
    `PasteReceipt{inputSeq, bytesWritten}` as it writes an unfinished paste
    to the program, at most one a service pass. The client sends a piece
    only while what it has sent is less than the window past `bytesWritten`,
    so a program that reads nothing holds a window of the paste and the rest
    stays with the client; a paste waiting on its window holds no other
    pane's paste behind it. The host never stops reading a client that sends
    Sync input: an interrupt or a `PasteAbort` behind unread input would
    never be seen. A host that states no window, or one below 15, is sent a
    paste as it comes.
  - Unwritten input is bounded three ways, and a sender past any of them is
    closed with `Oversize`, a client that ignores the window included:
    - Each client instance holds a share of a pane: 512 KiB, or 4,096
      events, unwritten. That is the window, and as much again for what is
      typed around a paste. Another instance's input never counts against
      it, so one sender's flood closes nobody else, and two people pasting
      into a program that reads nothing both stay.
    - A connection holds at most 1 MiB unwritten across all of its panes:
      two shares, so a person pasting into two busy panes at once fits.
    - The host holds at most 64 MiB unwritten across all of its panes: a
      connection's bound for each of the 64 clients it attaches. Input
      accepted from a lost link stays to be written, and without this bound
      connections that come and go could hold more than live ones can. At
      this bound the sender that finds no room is closed, though it kept to its
      share and to its connection's bound. No one sender can reach it: it
      takes the input of many connections holding the Drive scope.

    A pane holds at most 64 instances' shares, 32 MiB; the host's bound is
    what keeps 64 such panes from holding 2 GiB. Signals stand outside all
    three, within their own 4 KiB a pane. The pane's own
    focus report waits as one entry, a newer report replacing it, so a client
    that turns its focus over and over holds no more.
  - A sender the host closes for breaking the protocol (`Malformed`,
    `Oversize`, `UnknownKind`, `UnboundHandle`, `TransferRefused`,
    `DuplicateHandshake`) has what it queued unwritten dropped, and its
    paste's bracket is closed. On an ordinary `Bye`, a lost link, or a
    refusal of something it asked, the input already accepted stays and is
    written in its turn, and a resume continues it, so no keystroke is
    lost.
  - An interrupt always gets through. The
    host asks the pane's line discipline, the one the PTY stream and the
    echo context read, what a key's encoded bytes do. With signals on
    (`ISIG`) a key that is the interrupt, quit or suspend character is
    written at once, ahead of everything the pane holds unwritten, and past
    every input bound but its own. Where the terminal discards its unread
    input at a signal (`NOFLSH` off), the pane's unwritten input goes with it, whoever
    sent it: every paste in progress is ended, an open bracket's end marker
    written with the key in one write, and each client's dropped events are
    consumed in the pane's ledger, so none is taken again and the client's
    `inputWritten` moves past them. Each client is told of its own with
    `InputDiscarded{inputWritten, throughSeq}`, sent ahead of any update or
    `Receipt` whose count passes the run, so no count says the program was
    written them; a client counts them in its input status and sends no more
    of a paste among them. Under `NOFLSH` nothing is dropped, and the key
    counts as written once everything its client numbered before it is.
    With signals off the key is data, written in its turn, and the echo
    context names no interrupt character. Only a client that may drive
    reaches any of this: input from one that may not closes its connection.
    A run dropped while a client was away is reported by the resume's
    `inputWritten` alone.
  - The client hands its transport at most 8 KiB of paste at
    once, below the host's chunk cap, so a key behind one piece on a 1 Mbit/s
    uplink waits about 66 ms. A client with no echo context yet matches a new
    terminal's `^C`, `^\` and `^Z`. A key matches by its legacy byte: Ctrl with
    a letter, the standard layout's letter where the layout's is not Latin,
    or Ctrl-\. An attachment keeps a paste an interrupt gave up as its abort,
    so a resume sends the `PasteAbort` again and never the paste.
- **Sequencing.** Every input batch carries `inputSeq`, a u64 scoped to the
  `clientInstance`.
- **Receipts.** Two numbers come back, coalesced onto updates.
  - **`inputWritten`** is the highest `inputSeq` accepted into the PTY write
    queue. A receipt means written, not executed (`Docs/sync-input-feedback.md`).
    If no update leaves within 20 ms of input (provisional), a standalone
    `Receipt` is sent.
  - **`inputSettled`** is the highest `inputSeq` `s` such that:
    - `s` was written to the PTY at least `E` ago (provisional `E` = 50 ms);
    - a PTY read begun after `write(s) + E` returned would-block before this
      frame was taken.

    Why the timer: an application takes tens of milliseconds to be scheduled,
    read its input and echo it. A boundary taken at write time arrives without
    the echo, refutes a correct prediction and flickers it away. Only a frame
    with `inputSettled ≥ s` can confirm or refute a prediction for `s`. When
    the timer alone advances `inputSettled` and no `Update` leaves within the
    receipt delay, the host sends a standalone `Receipt`.
- **Exactly-once across a drop.**
  - The host keeps `lastWritten[clientInstance]` per pane for the pane's
    lifetime, not the attachment's grace. It costs 8 B per client instance
    that has typed into the pane, with least-recently-written eviction under
    the §7.9 cap.
  - `Welcome.resumed` returns the host's `inputWritten` per pane. The client
    resends only events above it, and the host drops anything at or below
    `lastWritten`. The client numbers new input from `max(local, host) + 1`, so
    a client restored from an older record never reuses a sequence number the
    host has already written.
  - If the host returns no value (the pane is `Gone`, the entry was evicted,
    or the resume was refused, §6.9), the client does not resend. A resume
    served by the successor of the instance the client held (§6.13) returns
    the inherited value like any other. It shows the events a connection
    carried and the host did not report as *in doubt* and sends them only on
    the user's instruction. A duplicated `y⏎` into an agent's permission
    prompt is worse than a lost one.
  - An event no connection carried cannot have reached the host. It is never
    in doubt and never counted written: it goes once a connection carries the
    pane, behind any events in doubt, numbered after the host's count.
  - The client identity survives a change between cellular and wireless LAN,
    a NAT rebind and a process relaunch, so input sent at the drop is
    delivered once. This is what the deduplication contract in
    `Docs/sync-input-feedback.md` (Efficient protocol contract) requires.
  - The client's attachment (`Source/Core/Protocol/Sync/ClientAttachment.h`)
    outlives its connections and numbers Each pane's input across them. An
    event is resent, or sent on the user's instruction, under the number it
    first had, so the pane's ledger drops it if it was written. While a pane
    has events in doubt, its new input waits behind them, within the 64 KiB
    bound. The person resends the events in doubt, which go first, in order,
    or discards them, which are counted. Either way the waiting input follows.
    No later event is written before an event in doubt, so the ledger's
    highest number tells written from not. A pane that ended takes its
    unwritten events with it, counted on its ended replica with the reason:
    the host restarted unplanned, or no longer has the pane. Input meant for
    a program that is gone never reaches another. The attachment decides each
    pane's input status (`PaneInputStatus`); a frontend shows it and offers
    the resend or the discard.
- **Echo context.** An `EchoContext` op travels whenever it changes. It carries:
  - the modes revision;
  - the foreground-process generation;
  - `secureEntry`: PTY `ECHO` off, read with `tcgetattr` on the master (
    verifies this on Linux and macOS);
  - `atPrompt`: OSC 133 B seen and C not yet seen;
  - `foreignInput`: another writer's input was written since the last frame;
  - `interruptChars`: the live `VINTR`, `VQUIT` and `VSUSP`, read with
    `tcgetattr` like `secureEntry`, which the paste abort above matches;
  - `eraseCharacter`: the live `VERASE`, or absent when disabled, for readline selection deletion.
  - The op carries no epoch of its own. An echo epoch is the
    pair (modes revision, foreground generation): it changes when either
    does, and that change is what ends the predictor's epochs below.
  - A checkpoint replaces everything a client held of the
    pane, the context with it, which may be from before a reconnect. The host owes a client that
    takes the op the context whenever the last one it sent differs from the
    pane's, a checkpoint having reset what it sent: when the screen stands
    still it takes the published frame again under the next generation, and
    the update to it, with no row to say, carries the context. The context
    is unknown for one frame after a checkpoint, never until the program
    writes. An update carrying the op goes on the stream, never as a
    datagram. A program that changed its line discipline and wrote nothing
    published no frame; the daemon reads the discipline again when a
    client's input is written, and takes a frame only if the facts moved.
    A pane whose line discipline cannot be read, as one behind a pipe, is
    said to have a new terminal's `^C`, `^\` and `^Z`.
- **Predictor.**
  - It lives in the client Core, and the frontend draws it as an overlay.
  - It predicts only printable insertion and simple backspace.
  - **Hard vetoes.** It predicts nothing unless the context is known,
    `secureEntry` is false, `atPrompt` is true and the screen is primary.
  - **Tentative epochs.** Within those vetoes the predictor runs over
    tentative epochs:
    - Every key it does not model opens a new tentative epoch: control keys,
      Tab, Enter, Up and Down. So do a wide grapheme, a combining mark and
      typing in the last column.
    - An epoch is hidden until one of its predictions is confirmed by a frame
      with `inputSettled` past it. That confirmation reveals the whole epoch.
    - A refuted hidden epoch is dropped silently. A refuted displayed
      prediction clears the overlay and resets the predictor.
    - A prediction equal to the cell's content before the keystroke earns no
      credit, and neither does a blank or unknown cell. Typing over identical
      text therefore never confirms an epoch.
    - A `Structural` frame, a change of the echo epoch (the pair
      `EchoContext.modesRevision`, `foregroundGeneration`) and `foreignInput`
      end every epoch.

    A secret typed after an unmodelled key is never displayed, because its
    epoch is never confirmed.
  - **Display.** The predictor always predicts while the vetoes allow it. It
    *exposes* the overlay only when:
    - the client's measured echo latency, from sending `inputSeq = s` to the
      first frame with `inputSettled ≥ s`, is above a show threshold, with a
      lower hide threshold for hysteresis; or
    - a prediction has been pending longer than a glitch threshold.

    Overlay cells are flagged for the pending treatment
    (`Docs/sync-input-feedback.md`) above a higher flag threshold, again with
    hysteresis. Starting values (provisional, D15): show above 30 ms and hide
    at 20 ms or below; flag above 80 ms and clear at 50 ms or below; glitch at
    250 ms; flag unconditionally after 5 s pending. On a link of a few
    milliseconds, predictions are never shown, because the real echo arrives
    within a frame.
  - Predictions never enter the grid, history, selection, clipboard or the
    accessibility transcript.
  - In raw-mode agent TUIs it does nothing. The felt-latency floor there is one
    RTT plus coherence. A raw-mode arm is a measured bet that is permitted only on measured
    accuracy (D18).

### 6.8 Attention: view states

Each attachment on each pane is in exactly one view state. The client sets it
with `View{handle, state}`, and the host enforces it.

| State | Receives | Costs the host | Used for |
|---|---|---|---|
| `Live` | updates, history appends, facts, status, effects, images | full pacing | visible panes |
| `Dormant` | one `PaneDirty` the first time the pane changes after entering `Dormant`; a `PanePalette` each time a frame taken of the pane moves its palette; facts, status and effects stay live | a dirty bit, and a palette epoch read per frame | background tabs, occluded windows, a backgrounded iOS app |
| `RecordsOnly` | `PaneFacts`, `StatusRecords`, effects; screen text only through `pane.capture` on request | zero rows | agents (`clientKind = Agent` default), watch faces and widgets |

Leaving `Dormant` diffs from the attachment's `sentRowHashes`, not from
nothing. The client's last picture is its acknowledged state, so waking a
background tab costs only its changed rows.

A newly bound pane starts `Dormant` until the client sets its view, except on
an attachment whose `clientKind` is `Agent`, where it starts `RecordsOnly`
(D11). A resume
names Each pane's view in `PaneResume.view`, so a phone resuming eight panes
with one visible receives one diff and nothing for the other seven.

`View` is kind 36 and `PaneDirty` kind 37, both on the pane's
channel. A fresh binding states its view; a resume names the retained view.
The host captures a frame only of a pane some attachment holds `Live`. A `Dormant` pane is told
once per `Dormant` span that it changed, counted on the source rather than by
generation, since nothing captures a pane no one shows. The frontends set the
view from what they present: a macOS tab in the back, a miniaturized window
and an app in the background are `Dormant`; the agent stays `RecordsOnly`
unless a verb reads the screen, and a region read leaves it so.

A `Dormant` pane's palette still reaches its client, because a client draws
more of a pane than its screen: a tab's label and icon, a pane's header and
its status marks wear the pane's colours. `PanePalette`, kind 19 on the
pane's channel and on the stream only, carries the palette block whole: the
bytes an update's `Block` op carries for `StateBlock::Palette`, a live
palette, its baseline and at most ten saved palettes, which always fit one
frame. The host looks at the palette once for each frame it takes of the
pane, so a burst of colour changes costs one look and at most one block, and
sends the block when it differs from the one the client holds: the one its
updates carried, then the span's newest `PanePalette`. A pane never sent a
frame keeps the client's own colours until its palette moves; one whose sent
and acknowledged blocks disagree as it goes `Dormant` is sent the block at
its first look. The client applies the block to the pane's terminal as it
applies an update's, and both ends move the block's hash in what they hold,
so the update that wakes the pane carries no palette and a resume matches
the picture with its new colours. No other screen state travels, and a
`RecordsOnly` pane is sent no palette.

The client's attachment keeps the view it set for each pane
across connections, and its resume names it. A pane the host continues
starts in that view on both ends, and no `View` is sent for it. A refused
resume binds each pane fresh, so the client restates the view it set: a
shown pane takes its checkpoint, and a hidden one costs nothing.

### 6.9 Attach, resume, reconnect, and keeping the picture

- **Attach.** `Greet` → `Welcome` → `WorkspaceStructure` and
  `Notify{Attachments}` → `PaneBind` for each pane. Each bound pane starts in
  its initial view state (§6.8), and the host sends its `PaneFacts` and
  `StatusRecords` at once, since facts and status are live in every view
  state. Then, for each pane the client sets `Live`, the host sends a
  `Checkpoint` (bulk) and history under the policy of §6.6.
- **Resume.** `Greet` carries `resume{attachmentID, resumeSecret}` and, per
  pane: `ackedGeneration`; `ackedRowHashesDigest`, an XXH3-128 over the
  client's row-hash vector; `historyEpoch`; `cachedRanges`;
  `lastInputSeqSent`; and `view`. On QUIC the secret is proved in the
  connection preamble instead (*Resume without SSH*, below).

  The host continues the attachment only when all of these hold:
  - the instance the client held is this daemon's `hostInstanceID`, or the
    predecessor named in `Welcome.predecessorHostInstanceID`, whose resume
    table this daemon inherited (§6.13);
  - the attachment is `Live` or `Lost`;
  - the new connection's principal equals the attachment's principal.

  Otherwise the resume is refused, and the client attaches fresh. A continued
  attachment diffs from the client's state when the digest equals the
  retained `sentRowHashes`. It compares hashes, not grids, so this works
  however old the state is. Otherwise it sends a `Checkpoint`. Either way only
  missing or potentially rewritten history is sent,
  and only for panes the resume names `Live`. A cached range suppresses
  transmission only within a retained publication's history head, when that
  publication matches the client's generation and history epoch and its
  history rewrite revision is still current. Appends beyond that head are
  not suppressed. Unknown or changed publication metadata requires a
  conservative resend under the history policy; a screen digest alone
  cannot establish that offscreen history is unchanged. `Welcome.resumed` reports each
  pane's outcome and the host's `inputWritten` (§6.7).
  The host keeps its attachments in a table, and the panes'
  sent and acknowledged baselines live in each attachment, not in the
  connection, so a new connection continues from them. An attachment's ID
  carries the issuing host instance in its high half: on SSH, where `Greet`
  names no instance, a resume naming another instance's attachment is
  unknown. The digest matches the acknowledged baseline or one sent after it
  whose acknowledgement the drop lost (the newest generation sent is always
  one of the two); a match continues the pane with an update from the
  generation the client acknowledged, and no match sends a checkpoint. A pane
  the host no longer has is `Gone`. `inputWritten` is the pane ledger's, which
  outlives the attachment, and stays absent rather than 0. The principal is
  checked before the secret, which is compared in constant time.
  A resume of an attachment a transport still serves is admitted, and the
  table hands its owner the old connection to close with `Bye{Superseded}`;
  that connection then reads nothing more. The resume's handshake window
  lasts until the new connection's client sends its first frame after the
  `Welcome`: a second resume presenting the consumed secret inside it is
  answered `Bye{ResumeInProgress}`, not attached; after it, the consumed
  secret is refused as a wrong one and the connection attaches fresh.
  The daemon holds one table for all its clients and names a connection's
  principal by its token when it presented one, by the daemon's uid when the
  peer is a verified local binary, and otherwise by the connection itself, so
  an unauthenticated remote connection never resumes. A resume continues only
  panes its attachment held; any other pane it names is `Gone`. A clean `Bye`
  detaches the attachment; a dropped transport leaves it `Lost`, and the
  daemon's loop forgets it past the grace.
  `Bye{SessionGone}` ends the held panes and retires their resume record before
  the client reports the refusal. Pending input cannot be sent to a replacement
  workspace, and a later carrier drop or record write cannot restore the retired
  authority. Temporary refusals such as `HostFull` keep the record for a retry.
- **The secret is single-use.** A resume consumes `resumeSecret`, and every
  `Welcome` issues a fresh one. The client rewrites its resume record with the
  fresh secret before it sends any input on the new connection.
- **Swap after success.** A resume for an attachment whose old transport is
  still open succeeds first and closes the old transport second, with
  `Bye{Superseded}`.
  - Geometry ownership, the primary tag, `sentRowHashes` and `lastWritten`
    carry over. Pacing does not: the new transport is a new connection, and
    it starts its own RTT estimate and window at the floor (§6.4).
  - A failed resume leaves the old transport serving, so a wrong or replayed
    secret cannot disconnect the client that holds the right one.
  - A second resume for the same attachment during a resume handshake gets
    `Bye{ResumeInProgress}`.

  A phone that changes networks redials before the host has seen the old
  transport die. That redial must not cost a checkpoint or leave a stale
  attachment holding the geometry and the primary tag.
- **Resume without SSH.** On QUIC, the authentication preamble accepts, in
  place of the one-time token (`Docs/quic-trust.md`):

  ```
  attachmentID
  HMAC-SHA256(resumeSecret, tlsExporter ‖ attachmentID ‖ hostInstanceID)
  ```

  - `tlsExporter` is this connection's TLS keying-material exporter, so the
    proof cannot be replayed onto another connection and costs no round trip.
  - The proof is valid only for an attachment that is `Live` or `Lost`, under
    the pinned certificate. It continues that attachment and never creates a
    principal, a pane or a shell (D16).
  - `hostInstanceID` in the MAC is the instance the client held. After a
    handover the new daemon verifies it against its predecessor's ID, the
    instance that issued the secret (§6.13).
  - A `Detached` attachment, a `hostInstanceID` that is neither this daemon
    nor its predecessor, and every fresh attach go through SSH.
  - A QUIC connection resumes only through this preamble, and its `Greet`
    carries no secret.
  - The MAC's key is the secret's 16 wire bytes and its
    message the exporter's 32 bytes, then the attachment's and the host
    instance's 16 each (`resumeProofMAC`). The proof travels as the
    attachment's identity and the MAC, 48 bytes. `AttachmentTable::admitProof`
    checks the MAC before it says anything of the attachment's state, and
    answers `UnknownAttachment` for another instance's identity or a freed
    attachment, `WrongSecret` for a MAC that is not this connection's, and
    `InProgress` while another resume of it is. An admitted connection may
    send one `Greet`: a resume of that attachment with no secret. Anything
    else, a fresh attach, another attachment, a secret, or a resume the
    table refuses, is answered `Bye{Malformed}` and creates nothing. The
    resume consumes the secret, so the same proof is refused afterwards. A
    connection that goes before its `Greet` gives the attachment up
    (`abandonProof`). A resume with no secret on a connection no proof
    admitted is refused as a wrong secret.
  - The preamble leads with a kind byte:
    `[1][length:u8][token]` for a bootstrap token, `[2][proof]` for a resume
    proof. `Sync::PreambleReader` holds a preamble's bytes and no more, 66
    at most, whatever an unauthenticated peer sends, and an unknown kind or
    a token of no length or past 64 bytes is dropped at the byte that says
    so. Both ends export the 32 bytes with no context under the label
    `EXPORTER-Ubiquitty-Sync-Resume`. The carrier does not know what a
    proof is: `QUICClient` takes an `ExportedPreamble`, a label and a
    function it calls once with what the handshake exported, and
    `Sync::resumePreamble` is that function's body. The daemon keeps, for
    each attachment a QUIC client created, what that client's bootstrap
    token granted (its scope, its token digest and whether it stands in for
    the person), and serves a connection a proof admits under exactly that;
    the grant goes when the attachment is freed, and a proof for an
    attachment with no grant is refused. A refused connection is dropped
    with no answer. A connection a proof admitted that has not greeted
    gives way to a newer proof for the same attachment and is closed: the
    host may not see a dial die for as long as its idle timeout, and the
    client that redialled holds the same secret. Once a `Greet` has begun
    the resume, a second is `InProgress` as before.

  Without it, every redial after a dead QUIC connection pays TCP, the SSH key
  exchange and authentication, an exec and the QUIC handshake: about 5–7 RTT,
  and possibly a biometric prompt, to deliver a resume under 1 KB.
- **Grace.**
  - A dropped transport moves the attachment to `Lost`, distinct from
    `Detached`: a lost connection is not a user's decision to leave. So does
    `Bye{Suspending}` (below).
  - `Lost` keeps about 2 KiB of resume state per pane for 10 minutes
    (provisional; configurable), then becomes `Detached` and is freed.
    `lastWritten` outlives it (§6.7).
  - Session lifetime is independent of attachments
    (`detachedSessionGraceMilliseconds`, `Daemon.cpp`).
  - A `Lost` attachment is freed before its grace when the
    same principal's same client instance holds a newer attachment: at the
    newer one's creation, or when the host finds the older one's transport
    dead afterwards. That client came back through a login, as after a
    proof that only ran out of time, and would otherwise hold a second
    attachment, with its geometry and its grant, for ten minutes. The
    instance is the client's own word, so it counts only between
    attachments one principal authenticated for; an attachment still served
    is never freed this way, so two windows that share an instance both
    stay up.
- **Liveness on demand.**
  - If an input batch has no `Receipt` within `max(4·srtt, 750 ms)`
    (provisional), the client sends `Probe`.
  - If no frame of any kind arrives within another `max(4·srtt, 750 ms)`, the
    client marks the transport dead, enters `Lost` locally, and resumes at once
    over a new transport. The swap rule above makes that safe while the host
    still holds the old one.
  - The same probe runs when the app returns to the foreground and on an OS
    network-path event, before the client trusts an existing connection.
  - The client also probes once at `Welcome`, so its first stall bound has
    an RTT to scale from.
  - Only a receipt covering every event sent stops the stall
    clock. Any frame answers the probe: contact restarts the clock, and the
    input draws another probe only after another bound. The client's
    attachment turns a probe left unanswered into a lost transport and wants
    a redial at once; `linkState` gives the frontend the two indicators
    (`Source/Core/Protocol/Sync/Liveness.h`).
  - The probe a client sends before it trusts a connection
    again, after a wake or a path event (`ClientConnection::checkLiveness`),
    is stricter than the stall probe. Only its own `Echo` answers it: frames
    the host sent before it read the probe, queued while the process was
    suspended or the old path was up, arrive first and say nothing of the
    path the connection is on now. It travels on the stream, where the
    carrier resends it, so one lost packet on a path that is a moment from
    being back is not a verdict. The connection does not declare the silence
    a loss; its owner times it. `RemoteSessionCore` gives it 3 s, holds what
    is typed meanwhile, and replaces a carrier that stays silent. A
    connection whose `Welcome` has not come cannot be asked. Lifecycle
    recovery may replace that attempt once; repeated notifications cannot
    keep cancelling its replacement before it can answer.
  - A remote session keeps one `ClientAttachment` for its
    life, so every reconnect is a resume. Inside a stream the login
    authenticated the `Greet` carries the secret. A QUIC redial presents the
    proof at the address, port and pin its factory kept from the bootstrap,
    and runs no bootstrap; it has 2 s (provisional) to reach its handshake,
    because a dead path may say nothing. The host sends a generic application
    close for a refused proof, disclosing no attachment detail. When that
    reason arrives, the client bootstraps at once and may use QUIC again. A proof dial
    that closes without that reason, runs out of time, or is interrupted by
    a wake or path change bootstraps over SSH at once, with no backoff.
    A proof is dialled only by a
    session whose last handshake rode QUIC. Once a QUIC carrier fails before
    `Welcome`, this session stays on SSH, including through later path and
    wake reports. Lifecycle notifications cannot interrupt that bootstrap
    or reset the backoff of its retries. A carrier the
    attachment declared dead is replaced at once: the backoff spaces
    attempts at a host that refused one.
  - A frontend's own terminal is lent to the attachment
    (`ClientPaneDelegate::terminalForPane`), and what is known of it is kept
    across connections as for a terminal the attachment owns. Before a
    resume names what it holds the lent terminal is read: one that no longer
    hashes to the replica, because its owner wrote to it or lends another,
    is named as unknown and sent whole.
  - `RemoteSessionCore::suspend` sends `Bye{Suspending}` and
    lets the carrier go; no timer redials a suspended client, and
    `resumeAfterSuspension` continues it. `onLinkState` reports the two
    indicators as they turn: the session wakes at the moment one would
    (`nextStallMicroseconds`), since nothing arriving is the event.
  - The frontends' part. The factory behind a macOS or Linux
    attach (`PlainChannelFactory`) and the iOS one (`PinnedQUICFactory`)
    keep the address, port and pin of the QUIC carrier their bootstrap
    reached, and dial it again by a resume key with no ssh run; a host
    install forgets the reach. macOS tells each remote session `suspend` at
    `willSleep`, and iOS at `didEnterBackground` for a QUIC session, which
    is the one a foreground return resumes. `TerminalSession` carries a
    client instance a frontend sets before attaching and reads after
    (`setRemoteClientInstance`). The clients that keep one across launches,
    and their resume record with it, are listed under "Keep the picture
    across a relaunch" below. To check by hand on a Mac and a phone against a real host, with
    `ssh` logging on the host (`journalctl -u ssh -f` or `log stream`):
    1. Attach over QUIC, sleep the Mac for two minutes, wake it. The session
       comes back, and the host logs no new ssh login.
    2. On the phone, attach over QUIC, switch to another app for ten
       seconds and return. The session comes back with no new ssh login,
       and the screen shows what was printed meanwhile.
    3. Restart the daemon on the host while the Mac sleeps. On wake the
       session reports the host restarted, after one ssh login and within
       a few seconds.
    4. Block UDP to the host (`pfctl` or the router) while connected, then
       sleep and wake. The session comes back over ssh within about five
       seconds of the wake.
  - Every client's input is intents on the screen plane.
    `RemoteSessionCore::send` hands an event to the session's attachment,
    which numbers it per pane and keeps it until the host reports it
    written: typed into a carrier that was already dead, or while the
    session reconnects, it goes on the connection that continues the
    session, once. Events a resume cannot account for are in doubt
    (`onInDoubt`): shown in Core's words (`InputInDoubtNotice`), never resent
    unasked, and holding back what is typed after them until the person
    resends or discards. A frontend's key
    press travels as `Keys`, a paste as `Paste`, a click the program tracks
    as `Pointer`, each encoded by the host under the modes live when it
    writes it; bytes a client already holds (an agent's input, a reply a
    frontend composed, a wheel step's arrow) travel as `Raw`. A pane's focus
    travels as `Focus` on its channel, from every client, and is sent again
    at each bind; the daemon records the pane-focused event when a pane goes
    from no driver holding it to one.
  - Nothing probes while nobody is looking.
  - Transport timeouts (tens of seconds: the QUIC idle timeout and the SSH
    keepalive budget) remain the backstop, not the detector.
- **Stalled is not lost.** The `PaneLink` connection state carries
  `lastHeardMicros` and the age of the oldest unreceipted `inputSeq` (§6.12).
  The frontend shows "no contact for N s" when the downlink is silent beyond
  the probe cadence, and "input not delivered" when frames arrive but receipts
  do not. Any received frame, a `Probe` included, is downlink contact and
  clears the first; Only a receipt clears the second. `linkStalled` is set
  while either indicator shows, and is the one bit a frontend needs to mark
  the pane.
- **Keep the picture.** While `Lost`, the passive `Terminal` and `HistoryCache`
  stay fully interactive: scroll, select, search and copy. Input queues locally
  with an unsent mark, bounded at 64 KiB.
- **Keep the picture across a relaunch.** The client persists a resume record
  per window:

  ```
  {hostInstanceID, clientInstance, attachmentID, resumeSecret, certificate pin,
   [PaneResume], checkpoint body of the passive Terminal at ackedGeneration}
  ```

  - The record is written atomically under the platform's strongest file
    protection and, where the platform has a keychain, encrypted with the D6
    key, because it holds screen text. A client with no keychain keeps it
    where only its owner can read it, and keeps no history in it (see which
    clients persist, below).
  - It is rewritten after every `Welcome`, before any input is sent on the new
    connection.
  - A protected store bound to a TLS certificate cannot persist an SSH-stdio
    connection, whose peer is authenticated by SSH instead. It returns
    `ResumeBindingOutcome::MemoryOnly`; Core must retire the previous stored
    record before accepting the Welcome. The live picture, resume secret,
    effect receipts and input ordering then remain in memory across reconnects,
    with no persistent writes. A later pinned carrier binds and commits a new
    record before releasing input. Identity, authorization and retirement
    failures still refuse admission; lack of a TLS pin alone does not.
  - It is purged with the history cache on `HistoryReset`.
  - The stored record names the wire version that encoded its pictures. A
    different wire version is refused, so an app moving from wire 1 to wire 2
    discards that cached resume state and attaches for a fresh checkpoint.
    The host keeps the running sessions; this does not migrate saved pictures
    or unsent drafts between protocol versions.
  - On a cold start the client draws the stored checkpoint first, verified by
    its digest and labelled as reconnecting, and then resumes.
  - A crash between `Welcome` and the rewrite leaves a consumed secret. That
    costs one checkpoint, which is the safe failure.
  - Unsent input in the record is shown as a draft and is never replayed
    automatically. The foreground program may have changed since it was typed.
  - `ClientAttachment` keeps the record through a
    `ResumeRecordStore`, whose persistent representation must meet the encryption
    and purge requirements above. Besides every
    `Welcome`, the record is written when the transport goes and when the
    owner asks (`persist`), so the picture is the newest the client held.
    - A pane's picture is two things: A checkpoint body the client encodes
      from its passive terminal, and what the client held of the screen
      plane in the host's numbering (row and block hashes, pens, generation),
      which the resume names and the next update builds on. The record keeps
      a digest of the body's bytes, because a checkpoint's own digest covers
      its rows and state blocks and not the pens, links and clusters they
      name. A body that does not match is not drawn, and the host
      checkpoints that pane.
    - A `HistoryReset` purges the stored record inside the read that carries
      it. The record is written again when that read returns, without the
      pane's picture until the replica holds a generation newer than the one
      the reset found: the screen before the clear is never stored again.
    - A record whose pictures pass the client's checkpoint cap is not
      written, and the stored one is purged, since a `Welcome` may have
      consumed its secret. A write the store refuses purges it the same way.
    - Drafts are input not reported written when the record was made, within
      the 64 KiB a lost client queues. They are kept across relaunches until
      the person dismisses them, and a paste an interrupt gave up is not one.
    - The relaunched client greets as the record's `clientInstance`.
  - Which clients persist, and where each keeps its bytes. Every one uses
    the same record (`Source/Core/Session/ResumeRecordFormat.h`) and the same rules
    (`ClientAttachment`); a client owns only where the bytes live.
    - iOS keeps one record per saved workspace in a sealed file under the
      app's protected container (`SealedResumeRecordStore`, the Apple
      keychain key), with its client instance in a protected keychain
      descriptor (`ColdResumeBridge`).
    - The web page keeps one record per tab and host in the tab's session
      storage, beside its client instance, which is the lifetime of a
      browser window: a reload and a restored tab resume, a new tab is a new
      client (`EncodedResumeRecordStore` through a synchronous slot). The
      browser protects the origin's storage; the record is not sealed.
    - The Emacs and Neovim clients speak the wire themselves and run no Core
      in-process. Each keeps its client instance and one record in a private
      directory under its own data directory, and the host binary stores and
      reads the record for it (`ubiquitty-host resume-record`): the client
      hands over the pane's picture as the host's own checkpoint body, Core
      derives the record from it, and on a relaunch returns the picture to
      draw and the tail the client's Greet ends with. The record is not
      sealed: these clients attach to a daemon on the same machine and
      account, whose own screen it repeats.
    - macOS keeps one record per remote window identity (its transport,
      QUIC host and workspace) in a sealed file under Application Support,
      sealed as the phone's is (`SealedResumeRecordStore`, the Apple
      keychain key). Its client instance, the peer a Welcome bound and the
      QUIC reach the connection would resume by are kept in a descriptor in
      the data protection keychain (`RemoteResume`). A window reopened after
      a restart draws the stored picture before it dials, and its first
      carrier is a resume proof. Quitting suspends every remote window, which
      writes its record; closing a window retires it. The data protection
      keychain opens only to an app whose signature carries an application
      identifier and a keychain access group, so a team-signed build with a
      provisioning profile keeps windows across a restart; an ad-hoc build
      cannot, says so once in the log, and attaches fresh.
- **Suspending.** A client the OS is about to suspend sets every pane
  `Dormant`. At its background deadline, or when protected data becomes
  unavailable, it writes its record, then sends `Bye{Suspending}` and closes.
  - The host moves the attachment to `Lost` at once and re-elects the primary
    (§6.11).
  - Geometry stays with the suspended owner. Pocketing a phone never resizes
    the agent's PTY.
  - `ClientAttachment::suspend` does this in order: the panes
    it shows go `Dormant`, the record is written while the connection is
    still open, and `Bye{Suspending}` is the last frame sent. The views the
    client set are kept, so the resume shows the panes again. The host
    treats every `Bye` but `Normal` as a lost transport, so the attachment is
    `Lost` in the pass that reads it.
- **The client moves off a dead path.** On QUIC, the client opens a fresh UDP
  socket and migrates the connection before it declares F8 when:
  - a socket-level send or receive error occurs;
  - an OS path event removes the interface in use;
  - about 10 s pass without a successful round trip while data is outstanding
    (provisional).
  - `Transport::RepathPolicy`
    (`Source/Core/Transport/RepathPolicy.h`) is the decision, with no socket
    and no clock of its own. The QUIC client's I/O thread reports to it:
    data outstanding (bytes in flight), a round trip (a packet from the
    daemon read), a send the socket refused, and `QUICClient::repath()`,
    which `RemoteSessionCore::networkPathChanged` calls through
    `Channel::repath()` when a frontend's path monitor fires. When the
    policy says so the client opens a fresh UDP socket and calls
    `ngtcp2_conn_initiate_immediate_migration`: a new local address and a
    new connection ID to the same daemon, with loss recovery resending what
    was unanswered. The move is tried once and judged: a round trip on the
    fresh path is its success. The connection is closed as lost (F8) only
    when the fresh path fails as a socket, stays silent for the same bound,
    or could not be opened. An established connection first tries a path move after a send error;
    before the handshake completes, the error ends the attempt.
  - The host routes a connection by every connection ID it issued for it,
    not only the first: a client on a fresh path addresses the daemon by a
    spare ID (RFC 9000 §9.5). The spares are kept per connection, retired as
    the client retires them, and at most 32.

  The client dials QUIC at the peer address the SSH connection reached, not at
  a re-resolved name. The certificate pin keeps identity.
  - The address is what the SSH connection that ran the
    bootstrap says of itself, and nothing is resolved to find it. A subprocess
    `ssh` runs the bootstrap under `-v`, and `Session::readSSHReach`
    (`Source/Core/Session/SSHReach.h`) reads its transcript: the address in
    `debug1: Connecting to <name> [<address>] port <port>.` for the attempt
    that `debug1: Connection established.` follows, since ssh prints a
    connect line for every address it tries. Nothing about how ssh dials is
    changed to learn it, so its fallback across a name's addresses and its
    host-key lookup stay its own. The iOS client's in-process SSH reports
    its socket's remote address. `QUICBootstrapInfo.reachedAddress` carries
    it; the bootstrap line never names where to dial, and a field that tries
    is ignored.
    - A run over a connection another ssh holds (ControlMaster) connects
      nowhere itself. One probe, the same `ssh` sharing no connection, says
      where ssh connects. The remote end's `SSH_CONNECTION` is not used: a
      host behind address translation reports an address nothing outside
      can dial.
      - The probe never logs in. It is ended as soon as its transcript names
        an established connection, which comes before the key exchange, and
        it runs with `StrictHostKeyChecking=yes`, no known-hosts file and
        `VerifyHostKeyDNS=no`, so one not ended in time stops at the key
        exchange. The host sees one TCP connection closed before
        authentication, whatever credentials the person's own ssh needs.
        (A configuration whose `KnownHostsCommand` supplies the key is the
        one way past the key exchange; ending the probe at the connect line
        is what covers it.)
      - It is made once for a shared connection, not once for a dial.
        `SharedConnectionReaches` keeps the address for every session in
        the process, keyed by the control socket's path as ssh expanded it
        and by when that socket was made, so a master made anew at the same
        path is probed again. A master that is gone leaves ssh to connect
        itself, and that run's own connect line is read.
      - It runs beside the bootstrap from the moment ssh says it is sharing
        a connection, lives no longer than ssh's connect timeout, and is
        waited for at most 2 s past the host's line. A probe that hangs
        costs the session that and no more: no carrier is offered and the
        session goes on over SSH.
    - A run through `ProxyJump` or `ProxyCommand`, a probe that could not
      say, and a transcript that names no connection each offer no QUIC
      carrier, with the reason in the transport's fallback diagnostic
      (`sshReachRefusal`), and the session stays on SSH.
    - What authenticates the QUIC peer is the certificate pin the bootstrap
      printed over SSH: the client checks it before it presents its token
      or proof. An address that is another machine, wrong or hostile, sees
      a handshake and is refused.
    - The transcript is SSH's standard error, which also carries text the
      host chose (its banner). Only whole lines in ssh's own form are read,
      the first established attempt stands, and an address is taken only if
      it is numeric. It is kept to 64 KiB and never shown.
    - The local daemon is dialled at the loopback address as it stands.
    - To check by hand where no test reaches, on a machine with a real
      host to connect to:
      1. macOS, direct. Connect to a host by name from the connect window.
         The session's transport reads QUIC, and
         `ssh -v <host> true 2>&1 | grep -A1 'Connecting to'` names the same
         address the daemon's log shows the QUIC connection arriving from
         the client at.
      2. macOS, shared connection. Add `ControlMaster auto`,
         `ControlPath ~/.ssh/cm-%C` and `ControlPersist 10m` for the host,
         run `ssh <host> true` once, then connect. The transport reads QUIC.
         On the host, `journalctl -u ssh` (or `/var/log/auth.log`) shows one
         `Connection closed by <client> port <n> [preauth]` for the first
         connect and none for a second window opened to the same host.
      3. macOS, proxied. Give the host a `ProxyJump` and connect. The
         session comes up over SSH, and its transport detail reads "ssh
         reaches the host through a proxy, so no address here leads to it".
      4. macOS, a password-only account with a shared connection open.
         Connect three times. The host's log shows no `Failed password` and
         no `Failed publickey` from the client's address.
      5. iOS. Connect to a host whose name has both an A and an AAAA
         record. The session's connection log reads the QUIC bootstrap as
         answered and the carrier as QUIC; with the phone on a network that
         has no IPv6 route, the same.
- **Host restarted.** If `hostInstanceID` differs and
  `Welcome.predecessorHostInstanceID` does not name the instance the client
  held, the restart was unplanned. The client keeps the old pane as a
  read-only *ended session* view, labelled as such, and attaches fresh. Cached
  lines are never mixed across instances. A planned upgrade hands the panes
  over instead, and the resume continues (§6.13).
- **Backoff.** 1 s three times, then doubling to 30 s
  (`reconnectDelayForAttempt`, `Source/Core/Session/RemoteSessionCore.cpp`).
  An OS path change may accelerate one attempt
  (`RemoteSessionCore::networkPathChanged`), and an app foreground replaces a
  carrier that may have expired while the process was suspended
  (`RemoteSessionCore::resumeAfterSuspension`), after the liveness probe above.
  Neither event resets the count of failures: only a Welcome or an explicit
  user retry does. After one lifecycle retry or a proof's bootstrap fallback,
  subsequent path and wake reports leave the attempt and its retry schedule
  intact until a Welcome. Every reconnect is a resume.

### 6.10 Multi-client geometry: the size follows the hands

- **Roles.** Each attachment has a principal (SSH user or unix uid) and a role:
  `Owner`, `Driver`, `Viewer` or `Agent`. Authority checks key on the principal
  and role, never on the transport.
  - A client asks for a role in its `Greet` and the host answers with the one
    it grants: the less privileged of what was asked and what the client's
    credential allows. A client whose kind is `Agent` is granted `Agent`
    whatever it asks, and one whose kind is `Viewer` nothing above `Viewer`.
    A role byte past `Agent` is a malformed handshake.
  - A method a connection may call names, in the method registry, the roles
    that may call it, the scope the call needs, what it is addressed to (a
    pane bound to the caller, or the host) and where the caller must stand
    (anywhere, or on the host as its user). The host makes one check before
    any effect, `Control::admitCall`, in that order and then the parameters
    against the entry's schema; the first that fails is the answer, with a
    sentence saying what was refused and what was needed, and the caller
    stays attached (F17). A call addressed to the host names no pane and
    needs none bound: `host.describe` and `host.methods`, which lists what
    the caller may call, are served under the Control scope.
  - A host verifies a client as an agent or as a person. An agent's
    attachment is an `Agent`; the host's own user's is an `Owner`; a person
    whose token may drive is a `Driver`, and one whose token may not a
    `Viewer`.
  - A connection that binds no pane is still an attachment: a client that
    asks a host what it runs greets as any client does, is granted a role,
    and calls the host's methods on that connection. It says `Bye{Normal}`
    when its errand is done, so the host holds nothing for a resume.
  - `host.refresh` is a passive catch-up call for human Owner, Driver, or
    Viewer connections with Observe scope. It requires no pane binding;
    optional screen prefetch additionally requires RegionRead. Agent callers
    are excluded. Its cursor is independent of live attachment acknowledgement,
    focus, geometry, and primary notification ownership.
    `epoch`, `after`, and `inventory` are lossless decimal strings; `paneOffset`
    is an opaque continuation, with zero starting an inventory. A continuation
    echoes the inventory revision; session insertion or removal changes that
    revision and restarts the page at zero. The client discards its old in-progress
    seen-set and only retires absent panes after a complete same-revision sweep.
    The answer includes retained
    `events`, current `panes` using the existing PaneFacts and StatusFragment
    encodings in base64, `cursor`, `hasMore`, `gap`, `inventory`, and `nextPaneOffset`.
    `screenPane` requests one optional checkpoint, and `screenRevision` avoids
    resending an unchanged checkpoint. Each answer holds at most 64 events,
    32 panes, and 1 MiB total; a checkpoint is capped at 256 KiB. A metadata
    record too large to fit is skipped and sets `panesOmitted`.
    The host persists the bounded event journal before returning its cursor.
    A new epoch resets the client's event and pane inventory cursors.
- **Geometry owner.** The owner is the `Driver` whose most recent *typing*
  input is newest. Typing input is `Keys`, `Paste` or `Raw`, never pointer
  motion. Ownership changes apply at once, and the owner's further size changes
  coalesce over 50 ms, which covers a drag. `pane.pinGeometry` freezes the
  size. Agents never own geometry.
  - Each pane's `GeometryArbiter`
    (`Source/Core/Protocol/Sync/Geometry.h`) decides the size. Until anyone
    types, the newest viewport a driver reported applies, as a lone client's
    window resize always has. Every size change, an owner change included,
    is applied at most once per 50 ms: the first at once, the trailing value
    at the window's end. Two clients typing in turn therefore cost the
    program one SIGWINCH per window. A driver reports two sizes, in two
    frames, and they are never the same statement:

    | Frame | Says | Sent |
    |---|---|---|
    | `Viewport`, on the pane's channel | the size this client shows *that pane* at, with the pixels it draws it in | once the pane is bound, and whenever the pane's size or pixels change |
    | `Facts.viewport`, on channel 0 | the size of the client's *window* | in the `Greet`, and again whenever the window changes |

    Which of the two a pane takes depends on where it stands in its tab
    (`PanePlacement`), for the hand whose size applies:

    | The pane | Its `Viewport` | Its window's `Facts.viewport` | The size it takes |
    |---|---|---|---|
    | is all its tab shows (`WholeWindow`) | counts | counts | the newer of the two |
    | shares its tab with others (`BesideOthers`) | counts | does not count | its `Viewport` |

    So a client attaching to a workspace never sizes a laid-out pane to
    its window: the window's size is kept, and counts again when the pane
    is alone. A client with one pane in one window, a web page among them,
    sends both at the same size. An agent has no viewport of a pane and
    sends no `Viewport`. Its explicit resize is the call
    `pane.overrideGeometry{pane, rows, columns}`, which needs the Drive
    scope and is answered with the size set: an override, which applies
    once and holds until a driver types or reports a different size, and
    which the agent's own typing does not end. The control socket's
    `terminal.resize` is the same override, not a pin: the next typist, any
    driver, takes the size back, and only `pane.pinGeometry` holds it against
    typing. No driver claims a size by being first: the newest typist's grid
    replaces an older driver's. The grid in an agent's
    `Greet` sizes no session it attaches to, and a person's
    attaching alone does not undo a pin or an override: its window takes an
    override back through its `Facts`, as any driver's new size does. The arbiter's
    `GeometryStatus` (owner, pinned, overridden) is the fact a frontend
    shows as "size set by …". `pane.pinGeometry` needs the Drive scope;
    with no size it unpins. The primary of decision D12 is
    `AttachmentTable::primary`.
- **Pixel geometry is session state.** The owner's `cellPixels` and
  `textAreaPixels` feed the Core's arithmetic: graphics-protocol cell boxes
  and the CSI 14/16/18 t reports. Program-observable answers therefore come from one device
  ([Core/frontend boundary](architecture.md)).
- **Non-owners** present the grid locally: *Fit* (scale down to a legibility
  floor), *Pan* (native size, follow the cursor) or *Letterbox*. That is a
  frontend choice.
- **A client's window is its person's.** Nothing a host says of a pane's size
  moves a client's window: not an attach, not a tab brought to the front, not
  another client's resize. A client holds two grids for a pane and neither
  overwrites the other. The grid its drawable holds is its own, stated in the
  pane's `Viewport` when it or the published pixels change and once at each
  acknowledgement (`RemoteSessionCore::setSize`); the grid the host sized the
  pane to arrives in its frames and is the replica's. A drawable that comes to
  hold the grid the host is already at is still a change of the client's size
  and is stated.
- **Resize pipeline.**
  - At most one SIGWINCH per 50 ms per pane, with the trailing value applied.
  - A resize produces one `Structural` checkpoint per pane, encoded once and
    sent to every attachment, about 3 KB at 80×24.
  - History is untouched.
- **Attachments are visible.** `Notify{Attachments, revision, list}` (§7.3)
  tells every client:
  - which attachments exist;
  - their roles and kinds;
  - which one owns geometry and which is primary.

  Each entry's principal is sent only to recipients whose role is `Owner`,
  for every entry in the list. A letterboxed or silent client can
  then say why, and that typing here takes the size.
- **Topology never overtakes content.**
  - Each topology leaf carries the pane's `rows`, `cols` and
    `structuralGeneration` (§7.3).
  - A client keeps drawing the pane's previous picture, letterboxed, until its
    applied generation reaches `structuralGeneration`.
  - The host queues a topology `Notify` after every pane frame taken before
    the topology change (§7.8). On QUIC the lanes are separate streams and can
    reorder, so the leaf generation, not arrival order, is what the client
    trusts.
  - Every structure the client receives for its workspace is noted as its
    topology, all tabs under one root, whether or not it changes the mirror;
    a pane's rectangle (`RemoteSessionCore::paneRectangle`) is the leaf's
    size only once the picture held reached the leaf's generation at that
    size.

### 6.11 Effects, clipboard, status, images, query replies

- **Effects** are typed: `Bell`, `Notification{title, body, identifier, occasion}`,
  `CommandFinished{exitStatus, durationMicros, currentDirectory, commandLine}`, `Sound`, `Attention` and
  `ClipboardWrite`.
  - Each is stamped with the generation after which it occurred, its origin
    `paneID` and a nonzero per-pane sequence. Its primary identity is immutable;
    electing a new primary changes future effects, never retained ones.
  - Effects in generations a client skipped are **coalesced**: one
    `Bell{count}`, the last 8 notifications, the last `ClipboardWrite` per
    target, and no sounds, because a skipped span's tones are stale.
  - Each effect is tagged **primary**: the `clientInstance` of the geometry
    owner if it is not `Lost`, otherwise the most recent typing human driver
    that is `Live`. Only the primary posts an OS notification or writes the
    clipboard. Other human clients badge. Without the failover, a user who
    types on the phone and pockets it misses the agent's next permission
    prompt: the desktop only badges and the phone is offline.
  - **Retention across `Lost`.** The effect log keeps coalesced effects from
    the oldest `Lost` attachment's acknowledged effect sequence for the grace window.
    - A resume delivers the remaining effects. A client retains its consumed
      sequence separately from its last complete batch acknowledgement, so an
      interruption after posting but before the final batch packet cannot post
      the same effect twice. A never-consumed effect may still be posted by its
      original primary; primary identity alone is not a delivery receipt.
    - `CommandFinished` coalesces to the last one per pane.
  - Notifications are rate-limited to 4 per pane per 10 s, with the excess
    folded into a count.
- **Clipboard write (OSC 52 set).** Delivered only to the primary, and only
  after the host's capability policy allowed it, as `ClipboardWrite`. A
  viewer's clipboard is never written by someone else's program. A client
  whose host is on another machine applies its own Remote-origin verdict to a
  delivered clipboard write, notification, sound or attention request before
  applying it
  ([capability origin](capability-origin.md#effects-a-host-delivers-to-a-client)).
- **Clipboard read.** The host sends `ClipboardRequest` to the exact selected
  clipboard source, naming a nonzero request ID, buffer and either a MIME
  inventory or one MIME item. Its authority remains bound to that source,
  session and original requesting operation through final payload delivery.
  Timeout, source change, detach or authority withdrawal retires the request;
  no answer from another client substitutes for it. `Empty`, `Refused` and
  `TooLarge` remain distinct outcomes. An unconfigured sink answers `Refused`
  without accessing a local clipboard. Clipboard writes do not grant read access.
- **Bell routing.** Bell is a per-`Terminal` effect route, not the
  process-global hook, because two
  panes in one daemon must not share one bell.
- **Program status.** The Core parses the program-status sequence OSC 7501 as a
  program-facing surface, under these rules:
  - whole-record replacement;
  - an unknown state invalidates the report;
  - validate before mutate;
  - 256 records with least-recently-updated eviction;
  - `working` and `blocked` dropped on exit and on OSC 133 `A`;
  - nothing echoed back except the fixed feature-detection reply.

  The wire carries Sync's own whole-set `StatusRecords` with its own enums.
  OSC 7501 is a sequence programs write; no sync frame contains it.
- **`PaneFacts`** is one whole-replacement record holding the title; cwd as a
  `file://` URL; the foreground process `{pid, name, generation}`; OSC 9;4
  progress and the OSC 21337 indicator; the OSC 1337 user variables
  (`Terminal::userVariables`); and the exit status. Without the user
  variables, a frontend that reads them would need an `if remote` branch
  (§6.12).
- **Images** are content-addressed by the first 16 bytes of
  `SHA-256(domain ‖ format:u8 ‖ width:u32LE ‖ height:u32LE ‖ pixels)`.
  The domain is the ASCII string `Ubiquitty.Image` terminated by NUL (16 bytes
  total). The digest's
  first 16 bytes retain their order on the wire, independent of machine endian.
  A reader verifies this identity before caching received decoded pixels; a
  placement change reuses that verified identity without rehashing pixels.
  - Placements are pane state, carried as a whole-list replacement by revision.
  - Pixels go once per attachment as a bulk transfer, behind screen and history:
    one zlib stream of the decoded pixels to a client that names
    `DeflateImages`, the raw pixels to one that does not (§7.7).
  - A picture replaced in place keeps showing until its replacement is
    verified. When a record binds new pixels at a placement whose geometry is
    unchanged, a reader keeps drawing the picture it verified there, of the
    same format and dimensions, until the new pixels are verified, whether the
    record keeps the image ID or binds the placement to a new one. Deleting the
    image or the placement, moving or resizing the placement, other dimensions,
    and a replacement that needs the picture's room retire it at once. A
    dashboard that sends each chart again every second under the same image
    and placement IDs therefore shows each chart without a gap.
  - The host captures the pixels of every picture a record names when it sends
    the record, and finishes those pictures before it samples a newer record
    that replaces them, so pictures replaced together arrive together.
  - The host sends a small `ImageHead` for each asset. Pixels follow immediately
    on a fresh attachment; after a resume they follow only an `ImageWant` for
    pixels the client no longer holds. `Greet` carries no cached-image list.
  - Hashes are scoped to one session. An `ImageWant` cannot retrieve another
    session's pixels, and an unknown hash is ignored. Outstanding requests are
    bounded across the connection, including all its panes.
- **Host-local file transports never reach the client.** The graphics-protocol
  file transports `t=f`, `t=t` and `t=s` are read by the host Core, and only
  the decoded image travels. No frame can name a client path, so a remote
  program cannot read the client's files.
- **Query replies are always the host's, synchronous**, at parse time
  (principle 15).
  - DA, DSR/CPR, DECRQM, DECRQSS, XTVERSION and XTGETTCAP come from Core state.
  - Colour and pixel-size queries come from Core state set by the live geometry
    owner's presentation.
  - The owner's pixels and theme are the session's, whether
    the pixels arrive in its `Viewport` of the pane or its `Facts`, and the
    theme in its `Facts`.
    While that owner has a live, authorized human connection, a client that
    does not own the size changes neither. If that connection is absent, the
    most recently presented palette from a live human with Drive scope supplies
    colours, without taking the retained size or pixels. Agents, Observe-only
    clients, expired tokens and superseded connections cannot supply this
    fallback. A palette-only `Facts` reaches an idle DEC 2031 subscriber without
    input or a resize. A presenter change applies at most once per coalesce
    window, as an ownership change does. From version
    12 a `Facts` of its own ends with `themePalette:optional PaletteBlock`
    (live and baseline palettes, coded as the checkpoint codes them). The
    client sends it first after the `Welcome`, ahead of any input, and
    withholds it from an older host; the `Greet`'s `Facts` never carries
    it. `appearance` is not carried: DSR 996 answers from the palette's
    background, so a byte beside it would say nothing more.
  - Focus reports (mode 1004) fire when the "any driver focused" fact changes.
    A driver is focused on a pane when its last `Focus` frame on that pane's
    channel said focused and its connection's `Facts.focused` is set. The
    per-pane `Focus` frame says which pane holds the client's keyboard focus;
    `Facts.focused` says whether the client window has focus at all.
  - The OSC 52 read is the asynchronous exchange above.
  - OSC 66 proportional runs use the host's grid-face width, because the
    headless host has no measurer (`TextMeasureClient`,
    `Source/PAL/TerminalClients.h`). The
    client renderer shapes for drawing only.

### 6.12 Placement neutrality: `PaneLink`

The frontend holds one `PaneLink` per pane. It offers:

- `view()`: the passive `Terminal`, `HistoryCache` and `StatusView`;
- `send(InputEvent)`;
- `setView(ViewState)`;
- `wantHistory(range)`;
- `facts(Facts)`;
- `observe(Observer&)`, with callbacks for a frame ready (with its class),
  status, effect, clipboard, connection state and the prediction overlay. The
  connection state carries `lastHeardMicros`, the age of the oldest
  unreceipted `inputSeq`, and `linkStalled` (§6.9), and the frame-ready
  callback carries `Update.lagMicros`, so the frontend can say how far behind
  the picture is. A local pane reports a link that never stalls. A frame is
  reported only while the view is `Live`: a `Dormant` or `RecordsOnly` view
  presents no screen and is told of no frame, and once `Live` again it is
  told of the newest.

It has three implementations:

- `LocalPaneLink` wraps an in-process `TerminalRuntime`. It produces the same
  frame classes, records, logical-line addressing and effects, with no
  encoding.
- `RemotePaneLink` decodes the wire.
- `LoopbackPaneLink` is test-only. It runs a local Core through the full encoder
  and decoder in memory and asserts `host grid == client grid` and
  `host history == client history` after every frame. That proves the single
  authoritative model survives transfer across the wire.

Input handoff borrows the semantic event and its already-resolved local
encoding for the duration of the call. The local endpoint writes that encoding
outside the model lock; a tmux pane uses its in-process control route. The remote
endpoint copies semantic keys, pastes and reported pointer events into its
bounded attachment queue, and uses the supplied encoding only for raw input.
An empty local key encoding does not discard a remote release or repeat: the
host resolves the event under its live modes. An event with neither key identity
nor text is ignored. Raw input with no bytes is refused on every link: it
writes nothing, and the wire has no empty `Raw`.

Frontend code (macOS Swift, iOS, Linux, UbiquittyKit `onFrame`) has no
`if remote` branch.

### 6.13 Host handover: live sessions survive a host upgrade

- **Purpose.** A host upgrade preserves running sessions across a breaking
  grammar change (D9), including for clients whose application updates arrive
  independently. Handover is a local authority-preserving transfer, not permission
  for a remote peer to adopt arbitrary PTYs.
- **Local admission.** Before quiescing, the holder requires a first-record
  handover preamble, the same kernel user, a successor it trusts, and that
  successor's bound listening Unix socket passed by `SCM_RIGHTS`. A byte relay
  or an ordinary attachment is not this authority. On Linux the successor is
  the exact running image at the holder's install path. On macOS a holder the
  release team signed (Developer ID team `5TC8J8TCS3`) trusts a successor whose
  running code the same team signed as `ubiquitty-host`, wherever it lies: the
  signature is read once per candidate, from its audit token, against the
  requirement `anchor apple generic and certificate leaf[subject.OU] =
  "5TC8J8TCS3"`, and the holder's own once as it starts. A release build trusts
  nothing else, so an unsigned, ad hoc or other signer's binary never succeeds
  it, and a release holder that is not the team's admits no successor. A
  development build (UBIQUITTY_RELEASE_SIGNING off) also trusts the exact
  running image at its install path, binding running-code validation to the
  peer audit token and the pinned install image, as it runs unsigned or under
  a development certificate
  ([SignaturePolicy.h](../Source/Platform/Host/SignaturePolicy.h)). Linux
  handover admission requires `SO_PEERPIDFD` (Linux 6.5 or a backport, with
  headers exposing that option); without it, handover is refused rather than
  trusting a recyclable PID. These requirements are for local handover only;
  ordinary attachment does not require `SO_PEERPIDFD`.
- **Mechanism.**
  1. The new daemon asks the old one, over the local socket, to hand over.
  2. The old daemon brings each pane to a `Boundary` or `Settled` frame. It
     passes the PTY master fd with `SCM_RIGHTS`, as the local handoff already
     does (`Local/Handoff.h`). With each fd it passes the pane's checkpoint
     body, its `LineStore` history, `lastWritten`, the geometry owner and the
     primary tag, and the resume table. Each resume-table entry holds what the
     §6.9 resume check reads: `attachmentID`, `resumeSecret`, the principal,
     the role, `clientKind`, `clientInstance`, the per-pane view state and
     `sentRowHashes`. Every handed-over attachment enters the new daemon as
     `Lost`.
  3. The old daemon sends `Bye{HandingOver}` to every connection and exits.
  4. The new daemon takes a fresh `hostInstanceID` and records its predecessor
     in `Welcome.predecessorHostInstanceID`. A resume naming the predecessor is
     served from the inherited table, and `paneID`s are unchanged.
- **Pane identity.** A pane keeps the `paneID` the host that started it
  minted for as long as it lives, across any number of handovers, while the
  host identities a successor inherits are the resume window, at most 8
  ancestors. The predecessor is this install's own authenticated binary, so
  a pane it hands over names its minting host for itself: that host is known
  to the successor even after it has left the window. The `paneID` must name
  a host (a non-zero minting half) and no other pane may hold it. Baselines
  an attachment keeps for a pane it no longer holds must name a host in the
  window or the host of a pane handed over; any other such pane has closed
  after its host left the window, so the successor forgets those baselines
  and a later bind of the pane is sent a checkpoint.
- **Transaction.** A daemon starting from an install path asks every older
  sibling host in its control directory, after it binds its own listener and
  before it accepts a client or reports itself listening. It walks their
  sockets oldest first and asks each host once, at the first of its sockets
  the walk reaches; a socket it has taken over is its own and is not asked.
  A sibling that refuses keeps its panes and serves beside it. The records,
  each length-prefixed on the exclusive local channel, are:
  1. `Request` (the requester's newest body version and instance), carrying
     the requester's listening socket.
  2. The holder first waits, for at most 2 s, until nothing blocks the
     handover. While it waits it reads no pane, so a flood makes no new
     output for a client, and it goes on writing to its clients and answering
     their transports: a frame or a reply part-written, or QUIC data a client
     has not acknowledged, is sent and acknowledged, and a request part-way
     can finish. Then it brings every pane to ground, captures every pane and the
     host tables, and creates an anonymous service lease bound to the
     requester's process and instance. `Offer` (body version, holder
     instance, pane count, table bytes, and how many listening and datagram
     sockets `Release` will carry) carries that lease. A holder that cannot
     refuses the whole handover with `Refuse`, naming
     why, so the person running the install learns what to finish first:
     `PaneBusy` when a pane did not reach ground in time, `ApprovalPending`
     when an agent's change to a workspace's tree waits for a person's
     approval, `GrantPending` when a capability question waits for an
     answer, `OutputUnsent` when a client has not yet been sent everything
     owed it, `RequestPending` when a client's request is part-way (a frame
     half read, a call, a consent, a clipboard read or a local request), and
     `AdoptPending` when a local session adoption is under way. None of
     these is carried across a handover, so each waits to be finished first;
     a blocker still standing when the wait ends is the refusal. A second
     request that arrives while one waits is refused with `RequestPending`.
  3. Per pane, `PaneFollows` (index, `paneID`, record bytes, the session leader
     and its parent as the holder's kernel reports them) carries the PTY
     master; the pane's record follows it. The host tables follow the last
     pane. The receiver bounds every announced size before reading it, and
     takes a master only when the kernel confirms the leader's session, group,
     parent and birth on it.
  4. The requester restores every session and the tables privately, with no
     PTY read and nothing published, and answers `Accept` or `Refuse`.
  5. On `Accept`, the holder sends `Bye{HandingOver}` on every connection
     (a QUIC connection hears it as the reason of its close), marks every
     child to outlive it, releases the lease and sends `Release`. `Release`
     carries the holder's listening socket and those of its own predecessors
     that it still serves, then its bound QUIC socket. It carries at most 8
     listening sockets: the holder's own, the one its records are kept
     beside, and the newest of the rest; the oldest of the rest are let go.
     The requester claims the lease, which is the commit point, publishes the
     sessions and tables, answers `Serving` and starts serving. The holder
     exits without hanging up a PTY.
  Neither daemon reads a handed-over PTY between the capture and the claim. A
  refusal, a closed channel or an elapsed deadline (10 s for the whole
  transaction) before `Release` cancels the lease and returns the holder to
  service with every pane where it was. After `Release`, a requester that never
  claimed lets the holder cancel and resume; one that claimed owns every pane
  whether or not `Serving` arrived. The successor adopts the holder's
  workspace and event records in place; the holder writes neither after it
  relinquishes. The records stay beside the socket of the first host in the
  chain, and a successor takes them only through that socket, so no holder
  lets it go.
- **Paths and ports.** The successor accepts on every listening socket it
  inherited, each still bound at its path in the control directory, so a
  client or a reattach that pinned the holder's socket reaches it; it removes
  those paths when it exits, and the holder leaves them. The holder closes
  the sockets past the bound, which go to nobody, and removes their paths
  unless something else listens there, holding each path's startup lock
  while it looks. It serves QUIC on the
  holder's socket, whatever port its own start named, so the port and the
  certificate pin are unchanged and a resume proof bound to the holder's
  instance is answered from the inherited table. The successor admits a
  listener only if it is a listening Unix socket bound at a host socket leaf
  in its own control directory.
- **Host log.** The holder logs the request, every refusal or abandonment with
  its reason, and on completion the number of sessions handed over with the
  time the panes were held and the part of it spent reaching ground. The
  successor logs the sessions it took and the time the transaction took. A
  successor a holder refused logs the reason the holder gave. A successor
  that refuses logs the reason it sends and the check that failed.
- **Install policy.** A handover never ends a session. Installing a host build
  starts the new binary, which asks the running host to hand over. If the
  running host refuses (a pane that cannot reach ground, a receiver limit, a
  body or channel it cannot read, any refusal), it keeps every session and
  keeps serving; the installer leaves it so, leaves the new binary installed
  beside it, and tells the person in one sentence that the running host kept
  its sessions and which reason it gave. The new binary takes the host over at
  its next start, once the running host has exited with its last session.
  Until then `ubq` and the apps show that host's update as pending. Connect's
  remote Update uses the same policy. Update When They End stages the executable
  until the current sessions end. Update Now installs it and runs
  `ubiquitty-host --take-over`, which starts the successor and checks whether
  older daemons remain. Exit 75 reports a pending handover; exit 1 reports a
  startup failure. Neither outcome stops a daemon holding sessions. An idle
  successor may be retired and started again to retry a refused handover;
  a successor with sessions is left serving. The app reconnects only after
  the takeover succeeds (`HostInstallMode` in
  [HostInstall.h](../Source/Core/Session/HostInstall.h)).
  Remote bootstrap, control, probe and upload scripts are encoded by
  `Transport::remoteShellCommand` as `exec /bin/sh -c` with one quoted script.
  OpenSSH and native SSH exec channels send that command after substituting
  the daemon pin and executable. The login shell interprets no script body;
  zsh word splitting and unmatched globs cannot change its arguments or stop
  installation. Quotes and backslashes in the script survive the wrapper.
- **This machine's host.** The macOS app installs the host it bundles. At
  launch, before it restores a window, and when a person chooses Update Host
  for This Mac, it makes the control directory's `ubiquitty-host` (the path
  an SSH bootstrap runs) the bundled helper's own file, by a hard link staged
  beside the install and renamed over it, and leaves `ubiquitty-host.interface`
  naming the app. A host installed at either path, the bundle's helper or the
  control directory's, then finds the successor's executable at its install
  path and hands over; a release's holder takes it over by its team signature
  wherever it lies. A bundled file another user owns, others can write, or
  on another volume is copied owner-only instead: a development holder
  installed at the control directory's path does not admit a copy. A
  development host started from the install trusts the named app, and that
  app's helper, while the helper is the install's own file or holds the same
  signed code, read again at each connection; a release's host trusts the app
  and any host binary by the team's signature alone. An installed host whose `--version` is newer than the app's is
  left as it is, as is one that does not say its version in time. The app asks
  each host on the machine who it is, through the bundled helper's relay: a
  Control greeting, answered with the host's build and, by one that trusts the
  relay, its instance, so a socket a successor inherited is the successor's.
  When a host of another build, not a newer one, answers, the app starts the
  bundled helper wherever that host runs from. One already running that holds
  no session's terminal, its own or one handed to it, is asked to stop
  (`SIGUSR2`, which a host handles before its socket answers, answers once
  no client is still greeting, and ignores while it holds a session, a
  workspace, an adoption or a handover), and is started again once it has
  exited, so it asks the older hosts once more. A successor that has taken one older host's sessions
  refuses the next, which keeps its own and serves beside it, as after a
  refusal. What is left undone, a host that kept its sessions, is said to the
  person who asked, with the advice to move the app only where its being on
  another volume is why a host refused; nothing is dialled again into the same
  refusal. A window whose saved reference names a holder's instance is
  answered on the holder's socket by the host that holds its workspace now,
  and moves its pin to that host's socket. A build made in a checkout
  upgrades only a control directory `UBIQUITTY_HOME` names.
  ([LocalHostInstall.h](../Source/Platform/POSIX/LocalHostInstall.h))
- **Format rule.** The handover body is the checkpoint encoding with a version
  prefix. It is a same-host format, passed over the local socket beside the
  fds, and it never appears on the wire. A successor must read the format of
  the last published release as well as its own (N-1 handover compatibility).
  `Source/Core/Protocol/Sync/HandoverBody.h` names the writer's
  `handoverBodyVersion`, the release's `lastPublishedHandoverBodyVersion` and
  the reader's `oldestHandoverBodyVersion`, which is pinned to that release.
  A grammar change increments the writer version and retains a versioned
  reader for the published format. The frozen released pane record and its
  minting procedure are documented in `Tests/Corpus/sync/handover-release.md`;
  the current encoder has its own fixture. Publishing a release updates the
  release constant and fixture together, after validating handover from the
  preceding release.
  The header and every handover section carry a version. Versions below the
  supported floor or above the writer are refused before their fields are
  interpreted; sections must match their enclosing record's version. A
  supported version selects its own grammar, never a mixture of two. A holder
  whose body the successor refuses keeps every session, as above.
  - **Screen.** The checkpoint carries the rendition stack, ten-entry title
    stacks, checksum extensions and saved palettes, the keyboard type, termcap
    profile and NumLock policy, readline paste quoting, literal newline, mouse
    movement and selection deletion modes, line tabulation stops, aligned
    character tab stops and their target characters, independent screen
    pointer stacks and line-scoped semantic roles, the explicit line home, the
    printer NUL policy, an independent national replacement setting for the
    status line, the right-to-left copy and writing modes, paste-event mode,
    CRT-saver mode, key-position mode and modifier-key actions, kitty's cursor
    text colour in the palette block and in each saved palette, and the cursor
    coupling modes in the modes block. The pen's underline word holds the
    colour index in bits 0–8, the role in bits 9–10 (0 output, 1 prompt,
    2 input), and in bits 11 and 12 whether the foreground and background are
    the DECAC normal text colour standing in for the default; bits 13–15 are
    zero, and the checkpoint row form carries the same bits.
  - **Host state.** It carries the proportional renderer declaration
    independently of sized text, the VTE progress properties, nested shell
    command identities, pending marks and timing state, the PTY erase
    character, the preceding graphic character and its cancellation state, the
    blanking reason and idle timer, answerback concealment beside the
    host-owned message, the drag-and-drop session, the DECAC colour
    assignments and the DECATC colour table, the line limit, the implicit
    movement direction and the start of the string QUAD positions, the OSC 176
    App ID, the host rich-text source-byte quota, and the native renderer
    revision and rendering capability with the font catalog scope. Rich-text
    placements carry application source revisions and scalar viewport anchors.
    A graphic undo carries no selection shift: a selection follows in-place
    moves through the positions its ends name.
  - **Sessions and authority.** A session carries its first input principal
    and whether more than one principal has injected input; these facts stay
    sticky across handover, because a background process outlives its input
    sender. Each resumable attachment carries its published proportional
    faces, and a successor counts those validated faces for panes the
    attachment still holds until its connection publishes a replacement. The
    offer stays bounded to 64 per pane, with a per-code-point and per-weight
    maximum envelope over every counted face deciding a measurement. The
    successor elects a fresh live measurement authority; native connections
    and pending queries are not handed over. Pastes in progress keep their
    quoting and newline options.
  - **Blockers.** An open aligned tab string blocks handover until it is
    terminated, and a partial printer-controller sequence until it is
    complete.
  - **Configuration and tables.** The configuration a holder hands over, its
    own and each session's, is record version 7, and the host-table sections
    each carry their own version: 2 for attachment authority, workspaces and
    the per-pane runtime, 1 for the rest. Each is read in its own version
    alone, and a new option or field takes the next one.
  - **Policy and consent.** Session policy retains explicit overrides
    separately from unset defaults. Pending clipboard consent keeps its
    original operation, principal and request identity; undelivered clipboard
    writes and notifications keep their selected recipients and order.
    Restoring them neither grants permission nor records a notification again.
    Grant and consent tables are separate host-wide state; carrying a
    session's policy alone cannot authorize a successor to serve it.
- **Crashes.** A daemon crash is not covered. The Core state dies with the
  daemon, and a PTY without its state would show a wrong screen. F9 covers
  that case. A workspace's arrangement does survive one: the next host
  lists it as restorable and recreates it with fresh shells on attach
  (`Docs/session-model.md` §8), under its recorded workspace id when that id
  is free there and under one of its own when it is not, for a person
  naming it, choosing it from the listing, or holding a saved reference to it. That is the workspace coming
  back with new sessions, not a resume.

---

## 7. Frame grammar

### 7.1 Primitives

- `varint`: unsigned LEB128, at most 10 B. An overlong encoding is rejected.
- `svarint`: zigzag encoding, then LEB128.
- `u8` to `u128`: little-endian fixed width, written field by field, never by
  memcpy.
- `bytes`: a varint length, then the bytes.
- `utf8`: `bytes` that must be valid UTF-8. Invalid input rejects the whole
  frame.
- `ID128`: 16 bytes, time-ordered, never reused.
- Every list starts with a varint count, checked against its cap (§7.9) before
  any element is read.
- `resumeSecret` is a MAC key, and only `Welcome` issues it. `Greet.resume`
  carries it back only on SSH stdio, inside the SSH channel. On QUIC it is
  never sent after `Welcome`: a resume proves it with the exporter-bound MAC
  of §6.9, and `Greet.resume` omits it.

### 7.2 Envelope

```
frame := kind:u8  channel:varint  length:varint  body[length]      length ≤ 65,535
```

- `channel 0` is the connection and control channel.
- `channel n ≥ 1` is a pane handle bound by `PaneBind`. The screen-plane
  envelope needs no additional pane wrapper or second inner header.
- The lane is implied by `kind` (tables below) and is never encoded.
- These fail closed with `Bye{reason}`: `length` above 65,535 or above the
  kind's cap; an unknown kind; an unbound handle; a frame failing its direction,
  handshake, role or view checks. The role table lives in
  `Source/Core/Protocol/Sync/FrameGates.h`; Host operation admission checks
  credential scope and the exact target independently.
- A QUIC datagram carries exactly one envelope.
- The overhead is 3–9 B per frame.
- SSH and QUIC stream bytes use this envelope directly. There is no outer
  message header, wrapper or `Catalog` kind. A stream read may contain any
  portion or number of canonical frames; each complete frame is admitted and
  applied before the next one is dispatched.
- Client code builds one handshake request, independent of its wire encoding.
  `Greet` carries the client identity, credential, explicit admission target and
  requested daemon pin alongside its facts. Session, workspace, Control and
  Management targets are distinct. `Welcome.selection` names the authenticated
  serving owner and exact selected target; a peer cannot choose another target
  by sending a later pane frame. Local operations do not greet a remote attachment.
- `wireVersion` covers this grammar, the typed records, and the Unicode
  width table; §7.3, "Versions", says
  which peers speak which version.

The handshake is carried in `FrameKind` frames like every other message;
there is no outer handshake or catalog codec and no second reader. A numeric
byte has only its `FrameKind` meaning. `PaneFacts` supplies presentation and foreground state, `StatusRecords`
supplies program activity, and typed Calls carry operations. Deployment must
coordinate matching hosts and clients without destroying an existing owner's
sessions.

#### Daemon-local control

Credential-store reload, QUIC bootstrap, clipboard helpers and the bridge's
SSH relay mark are daemon-local operations, not Sync envelopes or `Call`s. On a fresh Unix-stream connection, the first byte `0xF9` selects the
fixed local-control reader before any carrier decoding. Any other first byte
selects the ordinary carrier permanently, without consuming that byte. A late
local preamble therefore cannot turn a partial or greeted Sync connection into
an owner request.

`Source/Core/Protocol/Local/Control.h` defines the bounded codecs and readers;
the Host facade owns the authority checks and admitted effects. The
16-byte preamble is `F9 55 42 51 4C 4F 43 41 4C 00 00 00 00 00 00 01`.
The 25-byte request appends an operation and a nonzero `correlation:u64`.
Credential reload uses operation `1`; its frozen 34-byte reply appends
operation `0x81`, `status:u8` (`0` reloaded, `1` refused), the same correlation
and a nonzero `daemonInstanceID:u64`. Integer
fields are little-endian. Truncation, trailing bytes, an unknown operation or
status, and a zero identity are refused. The request reader retains at most
25 bytes and can yield its request only once. An unfinished local request
participates in the client's inbound footprint, completion deadline and
handover quiescence; consumed fixed storage is not charged as unfinished data.

The SSH relay mark uses operation `6`. The attach bridge sends it before the
client's first byte when sshd started the bridge, which the bridge reads from
its own `SSH_CONNECTION` or `SSH_CLIENT`. Unlike every other request, it may
be followed in the same delivery by the client's own stream, and once taken
the reader hands the rest of the connection to the ordinary carrier. The
daemon applies it only from its own trusted bridge, under the same check as
reload and bootstrap, and records that the client's person is on another
machine: the client keeps the owner's authority, and the sessions it drives
are gated at the Remote capability origin
([capability origin](capability-origin.md#a-client-that-reached-the-host-over-ssh)).
The mark never ends a connection: any other same-user stream's mark is
acknowledged as not applied and its connection continues unlowered.

Either way the daemon answers it with the fixed 34-byte reply, operation
`0x86`, ahead of any of the carrier's bytes. Its status byte says whether the
mark was applied, and it names the daemon's instance only when it was, to the
daemon's own bridge. The bridge reads exactly that reply before it relays
anything, and relays only on an applied mark. A daemon that closes first, as
one older than the mark does on an operation it does not know and a current
one does when it fails, a reply that is anything else, an acknowledgement that
the mark was not applied, and no reply within the bridge's wait all end the
attach with a sentence for the person.

QUIC bootstrap uses operation `2`. Its reply starts with the same preamble,
operation `0x82`, `status:u8` (`0` granted, `1` disabled, `2` refused), the
exact correlation and a nonzero daemon instance ID. A grant appends
`port:u16` (nonzero), `tokenLength:u8` (1–64), opaque token bytes, and exactly
32 certificate SHA-256 bytes. Disabled and refused replies end at the
34-byte header and carry no token, port or certificate. The reply reader
retains at most 133 bytes, refuses trailing bytes in any supplied chunk,
and releases only one exactly correlated reply. The producer verifies the
server's kernel same-user identity before sending, asks for a token only
through this operation, and clears all grant outputs on failure. Its
three-second absolute read deadline bounds waiting for a reply, not the
local connect call. It cannot observe a packet arriving after it has returned.

The reload and bootstrap operation boundary requires a kernel-verified same-user Unix peer whose
executable is one of the daemon's trusted owner binaries, with no remote
greeting and no received descriptors. QUIC peers, untrusted local agents and
self-claimed identities cannot reload credentials or mint bootstrap tokens.
An admitted reload refreshes persisted consent and revalidates live and
retained token grants; only then does the owner acknowledge it. Before minting,
the daemon rechecks transport authority, validates the live listener's port
and complete certificate, and reserves bounded reply capacity. The token
retains the authenticated person's Observe, Drive, Control, RegionRead,
NewSession and FileRead scope and interactive-user mark. The existing
60-second lifetime, 64-entry pool and same-client-only eviction are unchanged.
The caller closes this one-request
connection after the reply. An old daemon that cannot acknowledge this
protocol is not notified; the caller must report that outcome, never signal a
process named by a PID file as a fallback. The reload is an acceleration, not
the revocation itself: a daemon follows the store files before it dispatches
anything that store-bound authority admits
([agent tokens](agent-token-auth.md#expiry-and-revocation)), so an owner that
refused or never received the request still applies the edit. A bootstrap refusal likewise grants
no permission to restart a daemon or to retry the old carrier exchange.

Clipboard helpers use operation `4` on a fresh same-user Unix connection, with
no greeting or descriptors. Their authority is the kernel peer process's unique
membership in a hosted process tree, not a claimed session, label or trusted
owner-binary grant. The host rechecks that membership after receiving the body;
where process-instance information is available, it brackets the walk and
retains the instance through delayed output. This does not turn a numeric PID
into a proof against reuse before the socket was accepted. A detected instance
mismatch remains a refusal, not an unknown identity on a later check.

The operation's 25-byte prefix is followed by `bodyLength:u32`, `kind:u8`
(`0` Types, `1` Item, `2` Route), `mimeLength:u16` and the MIME's UTF-8 bytes.
`bodyLength` is exactly `3 + mimeLength`. Only Item has a nonempty MIME, bounded
to 65,535 bytes without NUL. Route inspects selection without reading clipboard
data. The body reader validates the fixed header and reserves inbound capacity
before allocating MIME storage. That owner stays charged after parsing, until
the helper connection retires. A partial body keeps the existing progress
deadline and prevents handover quiescence.

A reply contains the preamble, operation `0x84`, the canonical clipboard status
(`0` OK, `1` Empty, `2` Refused, `3` TooLarge), the exact `correlation:u64`,
nonzero `daemonInstanceID:u64`, `hostSelected:u8` and `payloadLength:u32`.
Only OK carries a payload: the same canonical clipboard bundle used by typed
pane delivery, with at most 32 distinct nonempty MIME names of at most 65,535
bytes and at most 4 MiB aggregate data. The entire encoded metadata and payload,
not merely its data bytes, is measured before reserving output storage.
HostSelected requires Empty and no payload; no refusal permits native fallback.
The helper validates correlation, identity, bounds and the complete bundle
before any output, file write or native clipboard invocation. Types responses
contain no item data; Item responses contain exactly the requested MIME.

Selection remains the last input driver's clipboard, or one unambiguous eligible
provider before anyone types. A disconnected driver, ambiguity or permission
failure is not a native-host selection. The host pins the selected provider for
every queued result, including empty answers and native routing, and revalidates
it and credential expiry before writes. The existing ten-second pending-read deadline, consent,
revocation and cancellation rules still apply. The former helper tags `0x19`,
`0xA2` and `0x13`, and the former remote request `0x9D`, are refused; none is an
alternate decoder or fallback.

### 7.3 Connection and control (channel 0)

| Kind | Dir | Lane | Fields |
|---|---|---|---|
| `Greet` | C→H | I | Wire version, `appVersion` and wire fingerprint, stable client instance/kind and requested role; explicit admission request, optional opaque credential, base Facts, history/backfill limits, capability bits and optional resume. The exact field order and admission constraints are in [the wire reference](sync-protocol.md#greet). |
| `Welcome` | H→C | I | Wire version, `appVersion` and wire fingerprint, host/attachment/resume identities and granted role; mandatory serving selection, consent admission/binding, principal, limits and host time; predecessor presence, resumed outcomes and paste presence/limits. The exact field order is in [the wire reference](sync-protocol.md#welcome). No omitted or unknown tail is accepted. |
| `Facts` | C→H | I | `viewportCells:(u16,u16)`, `cellPixels:(u16,u16)`, `textAreaPixels:(u16,u16)`, `refreshIntervalMicros:u32`, `link:u8 {Unmetered, Metered, Constrained}`, `power:u8 {Normal, Saver}`, `focused:u8`, `appearance:u8`, `themePalette:optional PaletteBlock`, `acceptsClipboard:u8` |
| `TypographyFacts` | C→H | I, pane channel | `rendererRevision:u64` (nonzero), `cellWidth:u16`, `cellHeight:u16`, `baseline:u16`, `proportionalSize:u16`, `monospaceSize:u16` (1/64 pt), `rendersBlocks:u8`, `shaping:u8`, `fontCatalogScope:u8` (0 unavailable, 1 exposed, 2 system), `faces:u8` (at most 16), and per face `name:utf8`, `class:u8`, `styles:u8`, `ascent`, `descent`, `lineHeight`, `averageAdvance` and 560 advances for U+0020–U+024F, each `u16` in 1/2048 em. Bound to one live pane handle; sent after bind and when its renderer or font environment changes. Render withdrawal clears `rendersBlocks`, `shaping` and `fontCatalogScope`. The host retains one eligible measurement authority per pane and invalidates its measurement epoch when that owner or publication changes (Docs/rich-text.md). |
| `ProportionalFaces` | C→H | I | `cellAdvance:u32`, `untabledAdvances:u16×4`, then four faces × 1,072 weight-zero `u16` advances and four faces × 1,072 maximum-weight `u8` advances for U+0020–U+024F, U+0300–U+036F, U+0370–U+03FF and U+0400–U+04FF. Weight zero is measured in 1/64 of the client measurer's pixel; the maximum of weights one through seven is rounded up in sixteenths of a cell. Each body is 12,876 bytes, below the 65,535-byte frame cap. Sent after a hosted pane the client may drive attaches and again when its run faces change; the host answers an OSC 66 measure from a per-code-point, per-weight maximum envelope over all counted driving viewers. At most 64 bodies are offered per pane (824,064 bytes plus a 34,304-byte envelope), and 64 current plus 256 inherited attachment bodies and at most 64 envelopes are retained per daemon (6,315,776 bytes), with bodies shared across panes (Docs/proportional-text.md). |
| `Ack` | C→H | I | `hostTimeEcho:u64`, `holdMicros:u32`, `bytesReceived:u64`, `panes:[(handle:varint, generation:u64, historyAcked:u64, effectsAcknowledged:u64)]` |
| `Probe` / `Echo` | both / both | I (datagram on QUIC) | `time:u64` in the sender's clock / `timeEcho:u64, holdMicros:u32` |
| `PaneBind` | H→C | I | `handle:varint`, `paneID:ID128` |
| `PaneUnbind` | H→C | I | `handle:varint`, `reason:u8`. Ends a binding, or a listing. Reason 0 is `Ended`; 1 is `CheckpointTooLarge`, a pane whose checkpoint passed the host cap (§7.9), which the client reports to the person as a failure until the host binds the pane again. A client reads an unknown reason as `Ended`. |
| `WorkspaceStructure` (16) | H→C | I (B past one frame) | `workspaceID:u32`, `revision:u64`, `flags:u8 {gone}`, `transfer:varint`, then the workspace's structure record (below) when `transfer` is 0 and the workspace is not gone. A record past one frame follows as the bulk transfer `transfer` names. `gone` says the host no longer holds the workspace and carries nothing. A client keeps the newest revision of each workspace; an older one arriving late is dropped. |
| `PaneListed` (17) | H→C | I | `handle:varint`, `paneID:ID128`, the handle at or above 2^30. The client is sent the pane's `PaneFacts` and `StatusRecords` on the handle and nothing else of it. |
| `Call` / `Result` | `Call` C→H, `Result` H→C | I (B when > 64 KiB) | JSON-RPC 2.0 text. It uses the method table of the control socket (`Docs/control-socket.md`) plus consent, `pane.search`, `pane.capture`, `pane.pinGeometry`, `pane.overrideGeometry`, `host.sessions.subscribe` and `describe`. `describe` serves every method's schema. A `Call` over a per-attachment cap is answered `Result{busy}` (§6.5), and a method the principal and role may not call is answered `Result{forbidden}` (F17). Each operation's schema governs its fields; consent calls require exact keys and refuse duplicates or unknown fields. |
| `Notify` (15) | H→C | I | `kind:varint` (a `NotifyKind`), then that kind's record to the end of the body (below). A client skips a kind it does not know by the frame's length. Nothing answers a `Notify`, and one from a client is malformed. |
| `BulkChunk` | both | B | `transfer:u32`, `index:u32`, `last:u8`, `bytes`. The last chunk adds `sha256:[32]`, then `compression:u8 {Deflate}` and `inflatedBytes:u32` only when the payload crossed deflated. Client chunks require an outstanding authorized clipboard reply or credited `paste.push` upload; they are always raw. |
| `BulkAbort` | H→C | I | `transfer:u32`. No further chunk of the transfer follows; the client frees its partial reassembly. |
| `Bye` | both | I | `reason:u8`, `detail:utf8`, peer-metadata presence flag and optional wire version, app version and platform. Current reasons and bounds are defined by `ByeReason` and the exact body in [the wire reference](sync-protocol.md#typed-close-and-versioning). |

`Bye` reasons:

- `Malformed`: a frame failed validation or its gate row (§7.2, F13).
- `VersionMismatch`: the two peers speak different wire versions (F14, D9).
- `Superseded`: a resume for this attachment succeeded on another transport
  (§6.9, F20).
- `ResumeInProgress`: a second resume raced one still in its handshake (§6.9).
- `Suspending`: the client is about to be suspended; the host moves the
  attachment to `Lost` at once (§6.9).
- `HandingOver`: the daemon is handing its panes to a newer one (§6.13).
- `TokenMissing`, `TokenInvalid`, `TokenExpired`, `TokenRevoked`,
  `OutsideScope`, `DuplicateHandshake`, `NoConsent`, `RemoteHostRefused`,
  `SessionGone` and `HostFull`: the host refuses the handshake or an
  operation (§7.2, §8).
- `OutputBacklog` (§9): the client read the host's output too slowly, and
  what the host held for it passed the per-client mark. The host discards
  what it had queued, finishes the frame it was partway through writing,
  sends this `Bye` and closes. It writes the `Bye` past the mark, because it
  is one small frame, and waits a bounded time for it to drain before it
  drops the connection anyway. On a QUIC carrier the stream holds the
  backlog the `Bye` would wait behind, so the host closes the connection with
  an application CONNECTION_CLOSE instead: its error code is
  `quicByeCloseCodeBase` (`0x55425900`) plus the reason's wire value, and its
  phrase the reason's notice. A client reads that code as it reads a `Bye`.
  The client shows the reason and reconnects, and each consecutive drop for
  this reason doubles its wait before redialling, from two seconds to the
  reconnect ceiling. A drop for any other reason, or a reconnect the person
  asks for, resets that wait. This is not a refusal.
- `Silent` (§6.4): the host heard no `Ack` or `Echo` for the silence bound
  while it awaited one, so the attachment is `Lost`. The host sends this
  `Bye` on the way out, as it sends `OutputBacklog`, and drops the
  connection; the attachment keeps what a resume needs. A path that is
  really gone never delivers it. This is not a refusal.

On the wire the reasons are `Normal` 0, `VersionMismatch` 1,
`Malformed` 2, `Oversize` 3, `UnknownKind` 4, `UnboundHandle` 5,
`TransferRefused` 6, `Superseded` 7, `ResumeInProgress` 8, `Suspending` 9,
`HandingOver` 10, `TokenMissing` 11, `TokenInvalid` 12, `TokenExpired` 13,
`TokenRevoked` 14, `OutsideScope` 15, `DuplicateHandshake` 16, `NoConsent` 17,
`RemoteHostRefused` 18, `SessionGone` 19, `HostFull` 20, `OutputBacklog`
21 and `Silent` 22. The set of reasons is part of `wireVersion`; adding or
changing a reason advances the wire version with its fingerprint.

For resume (§6.9): `Greet` may end with
`hasResume:u8 [ResumeRequest]`, where `ResumeRequest := attachmentID:u128
hasSecret:u8 [resumeSecret:u128] panes:[PaneResume]` and `PaneResume :=
paneID:u128 ackedGeneration:u64 ackedRowHashesDigest:u128 historyEpoch:u32
cachedRanges:[(from:u64, to:u64)] lastInputSeqSent:u64 view:u8 effectsAcknowledged:u64`. `Welcome`
may end with `hasPredecessor:u8 [predecessorHostInstanceID:u128]
resumed:[PaneResumed]`, `PaneResumed := paneID:u128 outcome:u8 {Diffed,
Checkpointed, Gone} hasInputWritten:u8 [inputWritten:u64]`. Each tail is
written only when it carries something, at most 256 panes and 64 cached
ranges a pane, ranges ascending and disjoint; a peer that does not know a
tail ignores it, as every handshake does a newer peer's trailing fields.

`Call` = 13, `Result` = 14 and `Notify` = 15 are channel-0
kinds. `Call := text`, UTF-8 to the end of the body.
`Result := transfer:varint`, then, when `transfer` is 0, the response's text to
the end of the body; a non-zero `transfer` names the bulk transfer (ids start
at 1) whose chunks carry the text, and the `Result` is its head. A `Call` names
a pane by its handle on this connection, `{"pane": handle}`; a client resolves
a `pane:N` or a label to the handle before it calls. A `Result` from the
client answers nothing the host called and is malformed, and so is a `Call`
from the host: no client serves a method. `directionOf` says the same, so
each end refuses the other's kind where it refuses any kind sent the wrong
way.

`Notify := kind:varint record`: what the host tells a client unasked, with no
answer. The kind is a `NotifyKind` (`Envelope.h`) and the record is that
kind's, to the end of the body. A client skips a kind it does not know by the
frame's length and counts it, so a newer host may add a kind without breaking
an older client; a known kind whose record does not read whole is malformed
where the connection itself reads it (the own consent status), and dropped
where a frontend reads it. JSON method names are not part of a `Notify`.

| Kind | Value | Record |
| --- | --- | --- |
| `Attachments` | 1 | `revision:u64 entries:[Entry]` (at most 1,024); `Entry := attachmentID:u128 role:u8 clientKind:u8 state:u8 geometryOwner:u8 primary:u8 self:u8 panes:[u128] hasPrincipal:u8 [principal:utf8] hasPresence:u8 [Presence]`; `Presence := clientID:u32 processID:u32 process:utf8 claimedLabel:utf8 grantIdentity:utf8 quic:u8 sharedPanes:[(paneID:u128 grant:u8)]` |
| `Events` | 2 | `records:[Record]` (1 to 16); `Record := cursor:u64 kind:u8 session:u32 pane:u32 exitStatus:svarint state:u8 atPrompt:u8 count:u64 text:utf8 detail:utf8` |
| `ConsentStatus` | 3 | `request:u32 session:u32 phase:u8 {Pending, Approved, Denied, Expired}` |
| `ConsentQuestion` | 4 | `kind:u8 {Attach, Terminate, Capability} request:u32 session:u32 peerProcessID:u32 peerProcess:utf8 agentLabel:utf8 [capabilityCode:u16 capabilityOrigin:u8 isRecipientsOwnRequest:u8 when Capability]` |
| `StructureConsent` | 5 | `request:u32 session:u32 workspace:u32 revision:u64 workspaceName:utf8 method:utf8 parameters:utf8 agentLabel:utf8 peerProcess:utf8 peerProcessID:u32` |
| `ConsentResolved` | 6 | `kind:u8 {Attach, Terminate, Capability, Structure} request:u32 session:u32` |
| `PasteReady` | 7 | `request:utf8 pane:u32 transfer:u32` |

An `Events` record with `kind = ScreenRead (13)` reports an agent's successful
text capture or the start of its screen stream. `count` is the Unix timestamp
in milliseconds, `text` is the agent's claimed label, and `detail` is the
host-verified process name when available. It carries no screen contents.
The host sends these records only to live human Observe attachments of the
same pane, independently of the Control-scoped event subscription. Clients
deduplicate by event cursor and retain at most 32 entries per pane. Opening
the activity list acknowledges only the cursor shown there; later reads stay
unread, and disconnecting the reader does not clear the indicator. This is
bounded in-memory activity, not a persistent audit log.

Every identifier a record names is nonzero, every enumeration byte is one the
table lists, every flag is 0 or 1, and every text is UTF-8 with no NUL within
its stated bound. A record ends exactly where the body does.

`paste.push` preserves the existing 4 MiB raw-payload limit without putting
payload-sized base64 into a `Call`. It requires Drive scope and an Owner,
Driver or Agent attachment, names a currently bound `pane` handle, and uses a
nonempty connection-unique string request id. Its exact params are
`{pane:u32, mime:utf8, bytes:u32, sha256:lowercaseHex64}`: bytes is 1 through
4,194,304; MIME is at most 65,535 UTF-8 bytes, with an empty or unrecognized
MIME retaining the generic `.bin` extension. The complete serialized Call
still fits 65,535 bytes, including escaped text and correlation.

Before allocating raw storage, the host validates the operation's authority,
the exact live pane/attachment, its existing 256-landings-per-connection file
budget, and the complete allocation against its per-client and aggregate
inbound budgets. Each physical connection has at most one active paste upload.
An admitted call remains outstanding and the host sends
`Notify{PasteReady, request, pane, transfer}` naming the call's request id
and pane handle.
The fresh nonzero transfer names only this upload on this physical connection.
Credit is not a final Result or proof of a successful landing. A client sends
no upload bytes before matching the exact request id and original pane handle;
repeated identical credit does not restart its upload or duplicate a chunk.

The upload uses existing channel-0 `BulkChunk` records, starting at index zero,
with nonempty raw chunks of at most `bulkChunkPayloadBytes`, in exact index
order. Compression and inflated-byte trailers are refused. The final marker
must coincide with the declared byte count, and both its SHA-256 and the hash
of the complete bytes must equal the Call's immutable manifest. Orphan,
reused, oversized, reordered or otherwise malformed chunks close the connection
without publishing partial bytes. Authority is checked again before each copy
and immediately before landing; timeout, pane unbind, authority withdrawal or
transport replacement releases all held bytes and never replays a file landing.

Only the complete verified body can be landed and its path typed, preserving
the host's existing bracketed-paste, input-origin and backpressure behavior.
The exact final Result is `{pane:handle,status,path}` with status one of
`landed`, `too-large`, `io-error`, `unsupported` or `busy`. A refused call uses
the ordinary correlated JSON-RPC error; neither silence nor upload credit
establishes success. File creation and path injection happen at most once per
admitted call, even if an identical credit was retried.

`Greet` ends with `capabilities:varint`, a bit per thing a
client can take past what every client takes. The bits a client may name
are part of the wire fingerprint, so builds that differ in them meet as
different versions before either reads the other's Greet; a host refuses a
bit it does not know as malformed, since a peer of its fingerprint names
only the bits it names. A client that
names none of the first three, as the Emacs and Neovim clients do by naming
the echo context and that they show no questions, is sent every body and
every row whole.

| Bit | Name | Effect |
|---|---|---|
| 0 | `DeflateCheckpoints` | A checkpoint body deflated |
| 1 | `SpanOps` | `Spans` in a chain update (§7.4) |
| 2 | `ScrollRectOps` | `ScrollRect` in a chain update (§7.4) |
| 5 | `AsksAboutHostEffects` | A clipboard write or notification gated at the Remote origin, without a question of the host's own; the client asks ([capability origin](capability-origin.md#effects-a-host-delivers-to-a-client)) |
| 6 | `ShowsNoQuestions` | No consent question: the client shows a person none, so it is never one a question waits on |
| 7 | `DeflateImages` | Every image's pixels as one zlib stream (§7.7) |
| 8 | `PublishesFaces` | This client draws proportional runs and will publish its face advances for hosted measure answers; only an attached driver offering this bit can owe the host faces (Docs/proportional-text.md) |

The last `BulkChunk` of a deflated payload adds, after the SHA-256,
`compression` and `inflatedBytes:u32`; an undeflated one ends at the SHA-256,
so a client that names neither `DeflateCheckpoints` nor `DeflateImages` never
meets the byte. An explicit `None` byte is malformed. The SHA-256 is over the
bytes as they crossed. A checkpoint body above 4 KiB crosses deflated when the client
takes deflate and the stream is the smaller. A client inflates into the
declared length and no more: a declared length past the transfer's cap, or a
stream that inflates past it, is refused (`TransferRefused`); a stream that
does not inflate, or ends short, is `Malformed`. The bound is the transfer's
existing negotiated cap. The independent caps in §7.9 apply to the screen
checkpoint and image planes separately.


`PaneResume := {paneID:ID128, ackedGeneration:u64, ackedRowHashesDigest:u128,
historyEpoch:u32, cachedRanges:[(from:u64, to:u64)], lastInputSeqSent:u64,
view:u8 {Live, Dormant, RecordsOnly}, effectsAcknowledged:u64}`. It carries no handle; handles are per
connection.

`Limits` carries the caps of §7.9 that a client must respect or can hit,
including the per-attachment caps, and `probeScheduleMicros:[u32]`, the host's
idle `Probe` schedule (§6.4), from which the client derives its silence bound.
The schedule is `steps:varint` then `steps` × `u32`, at most 8;
the last step repeats, and an empty schedule says the host does not probe.

A split is a tree of nodes, each a pane or a split of two or more nodes:

```
node := split{axis, ratio, children:[node]} | leaf{paneID, rows, cols, structuralGeneration}
```

- `structuralGeneration` is the pane's latest `Structural` generation
  (`PaneSource::structuralGeneration`); a client applies a leaf's rectangle
  only once it holds that generation (§6.10).
- Focus is per client and is not in the tree.
- The tree reaches a client in its workspace's structure, below. A host
  sends an owed structure at the end of a service pass, behind the pane
  frames that pass takes, so a frame taken before the structure changed goes
  ahead of the record that names the change. A frame the window held back
  may still follow it, which the leaf's generation covers. The structure is
  whole, so one owed and unsent is replaced by a newer.
- The client's rule is `paneRectangleFor(leaf, picture)`: the leaf's size once
  the picture is at least the leaf's generation and is that size, otherwise
  the picture's own size, letterboxed. A picture newer than the tree at
  another size is letterboxed too, since the tree that says so has not come.
  A rectangle that is not letterboxed is always the size of the grid in it.

A host describes each workspace it holds in one record, the
workspace's structure (`Source/Core/Session/SessionTreeWire.h`). It is the one
description of a host's layout: a client attached to a workspace reads that
workspace's, and a listing, a switcher and an agent's `ls` read every
workspace's.

```
structure := revision:u64 state:u8 {Live, RestoredAfterHostRestart, Restorable}
             flags:u8 {endsWhenDetached} id:u32 name:bytes label:bytes tags:[bytes]
             windows:count hostActiveWindow:varint [window]
window    := id:u32 label:bytes tags:[bytes] flags:u8 {zoomed} tabs:count hostActiveTab:varint
             (zoomedTab:varint zoomedPane:varint)? [tab]
tab       := id:u32 label:bytes tags:[bytes] name:bytes panes:count hostFocusedPane:varint node
node      := 0 paneID:ID128 pane:u32 sessionID:u32 cells structuralGeneration:u64
               label:bytes tags:[bytes] sessionLabel:bytes sessionTags:[bytes]
               attendees:[clientID:u32 kind:u8 {Person, Agent}]
               (title:bytes directory:bytes host:bytes when the workspace is Restorable)
           | 1 axis:u8 {Horizontal, Vertical} ratio:f64 cells children:count [node]
cells     := rows:u16 columns:u16 offsetRows:u16 offsetColumns:u16
```

- The record is structure and nothing a pane does. A pane's title,
  directory, program, status and activity travel only in `PaneFacts` and
  `StatusRecords` (§7.7), for a pane a client is bound to and for one it
  lists alike, and Core assembles the tree a listing draws from the
  structures and the records a client holds (`assembleSessionTree`). A pane
  whose records have not arrived is listed with its structure alone.
- A leaf names its pane three ways: `paneID` as the screen plane names it,
  `pane` as the handle a layout verb names it by, and the session behind it.
  `structuralGeneration` is the generation of the frame that gave the pane
  its grid, under the rule above.
- Every node carries its size and its place in cells as the host's layout
  computed them: a leaf's grid, a split's extent (the sum of its children's
  along its axis, the larger across), and an offset from the tab's top left.
  The rule is the one the layout string of `Tmux::emitPaneLayout` is written
  by, so no client rounds for itself and no two descriptions of a layout
  differ by a cell. `Horizontal` lays a split's children side by side.
- A tab's leaves, first child first, are its panes in order; `hostFocusedPane`
  and the window's zoom name a pane by that order. A window has one zoom, as
  the layout model's surface does.
- A restorable workspace lists what its record holds, under ids of zero below
  the workspace: only the workspace can be addressed. Nothing runs in its
  panes and no pane record is sent for them, so each leaf carries the title,
  the directory and the host its record holds.
- Nothing computed crosses: no status, activity, attendance count or title
  input. Both ends run one derivation (`deriveSessionTree`).
- No entry names a principal: an attendee is a client id and a kind.
- Caps, each checked before an element is read: 16 windows a workspace, 32
  tabs a window, 64 panes a tab and 64 panes a workspace, 16 tags an object,
  a label or a tag 64 B, a name 4 KiB (a longer one is cut), 256 attendees a
  pane, and a split no deeper than the one nesting bound of a topology tree.
  A window or a tab with nothing in it, an index or a zoom naming nothing,
  and a split whose leaves are not the tab's panes are refused. An encoder
  refuses a workspace no host holds rather than write a body a reader would
  refuse.
- A host sends a workspace's structure to whoever reads the workspace: a
  person attached to one of its panes, and a client that lists the host
  (`host.sessions.subscribe`, under the Control scope), which is sent every
  workspace's. An agent attached to one session is sent none unless it
  lists. A structure is sent when it reads differently from the one last
  sent and not otherwise, under the next revision; the host owes each reader
  the newest and never queues an older one behind it, and a later revision
  waits for a transfer of the same workspace still leaving. With nobody to
  read one, none is built.
- A client that lists is also listed every pane it has not bound
  (`PaneListed`): the host sends it that pane's `PaneFacts` and
  `StatusRecords` (§7.7) and nothing else. A listed pane has no view, no
  screen and no history, takes no input, is nobody's attachment and has no
  say in the pane's size; a frame a client addresses to its handle is one
  for a handle nothing was bound to, and closes the connection. Its records
  leave at the listing's cadence, at most once a second a pane however fast
  they move, and a pane only a listing reads is read by its host no more
  often. `host.sessions.subscribe` answers with the workspaces to expect,
  parallel `structureRevisions` as canonical decimal uint64 strings, and
  `paneRevisions` entries containing nonzero `session` and `revision` numbers.
  The latter includes bound panes, whose facts do not use listing handles.
  It also states how many panes were listed and the host's wall clock, which
  activity stamps are read against. A new request uses a new JSON-RPC id.
  Its snapshot is complete only after every promised structure and pane facts
  revision has arrived; an older cached tree cannot satisfy a fresh request.
  A one-shot reader ends the subscription with `host.sessions.unsubscribe` on
  completion, refusal or timeout; a watcher retains one subscription until its
  connection ends. Unsubscribing removes listing handles, not bound panes.
  Flat session rows are a local projection of this same tree; no other frame
  lists sessions.
- A person's subscription may also include `instances`: up to 32 live local
  daemon endpoints, each with a socket basename, absolute attach executable,
  and socket creation time. The host verifies private same-user sockets and
  reads executable identity from kernel peer credentials without sending a
  request to the other daemon. Discovery metadata occupies at most 16 KiB of
  the result. Clients validate and quote these paths, then authenticate with
  the selected daemon through its own executable. Session and workspace
  numbers remain scoped to that daemon; discovery transfers no authority.
- `workspace.share` publishes one native window after its PTYs have been
  adopted. It requires a live trusted local human attachment, Drive scope,
  the current daemon incarnation, and distinct adopted session handles in
  layout leaf order. Its bounded arrangement keeps tabs, splits, and focus;
  `workspace: 0` creates a workspace and a nonzero value appends a window to
  the selected workspace. The host stages the complete layout before changing
  ownership, preserves session and pane identities, and returns the workspace
  number. An identical retry on the same daemon returns the existing result.
  Agents and remote connections cannot publish a local handoff.
- Session termination is a typed, correlated operation. `session.terminate`
  requires Control and names a nonzero `session`. A verified person, or an
  agent already attached to that exact target or holding its standing grant,
  may terminate directly. Other agents need a live target human's one-use
  consent; an absent human is refusal, never implicit approval. The held
  operation binds its original attachment, principal, scope and stable target
  pane, all rechecked before completion. Target exit retires the question.
  `host.session.terminate` is a separate Manage-scoped operation for verified
  humans, including a Viewer without Drive; it does not grant Control.
  Results echo the exact request id and session with `terminated`, `denied`,
  `timed-out`, or `refused`. The Control operation may return `forgotten` when
  a person removes a remembered workspace without a live session. Silence,
  transport closure, EXIT and consent notices do not establish success.
  No other frame or management operation terminates a session.
- Host administration uses five Manage-scoped Calls with the existing caller
  role and principal admission: `host.management.list {daemonInstance}`,
  `host.client.disconnect {daemonInstance, client}`,
  `host.grant.revoke {daemonInstance, identity}`,
  `host.token.issue {daemonInstance, label, scopes, expiryUnix}`, and
  `host.token.revoke {daemonInstance, identity}`. `daemonInstance` is a nonzero
  canonical decimal uint64 string; `expiryUnix` is the same representation,
  and `scopes` is the explicit nonempty seven-bit scope mask. Issue requires
  a label of 1–128 bytes without control characters and a future expiry.
  Calls wait for the validated Welcome and refuse a changed incarnation.
  Results echo method, incarnation, exact selectors and JSON-RPC id, with
  `status` (`success`, `refused`, `not-found`, `invalid`, or `failed`) and
  `detail`. A list returns owned `clients`, `grants`, and token metadata, at
  most 1024 records per collection; the sessions are the listing's tree; strings hold at most 4096
  bytes and the complete Result text at most the existing 4 MiB management
  bound. Only an explicit successful issue includes `issuedToken`; lists and
  failed issues never carry a bearer secret. Mutations preflight their worst
  bounded, correlated answer against the inline Result and existing queue
  capacities before any effect; otherwise they answer Busy without mutation.
  This capacity admission does not change Call id syntax or authority.
  Committed revocation immediately withdraws dependent consent work and
  revalidates token clients after its inline answer is queued and flushed;
  disconnect refuses its own management connection. Management travels only
  as these Calls; a frame kind outside §7 is refused at its header, before
  payload allocation or observer delivery.
- The listing is least privilege. It is admitted under the Control scope
  alone, and that scope serves what a listing is for: the structure, every
  name, label and tag, a pane's title, status, activity, grid, directory and
  terminal name. A pane's command line is what its screen is (a token or a
  password typed after a flag), so `processChainWithArguments` is sent whole
  only to a connection whose credential would let it attach and read that
  screen: one that also carries Observe, or a trusted local client. Any
  other listing is sent the chain of program names in its place
  (`ListedDetail::WithoutArguments`, fixed when the pane is listed). A token
  with Observe and no Control is refused the listing. The directory and the
  terminal name stay with Control: they are what tells one session from
  another in a list, where arguments identify nothing a name does not and are the one
  field of a pane's facts that routinely holds a secret. A program can still
  put its command line in its title; a title is the program's own
  publication, shown wherever the pane is named.
- A client assembles the tree from what it was sent (`Session::HostListing`):
  each structure decoded once a revision, Each pane's records read where the
  connection holds them (on the listing, or on the binding of a pane the
  client has open), and every level derived as the host derives its own. The
  tree is whole once every workspace the answer named and the facts of every
  pane that runs have arrived. The client keeps the host's clock running by
  its own from the answer on, so an age is the host's however far apart the
  two clocks are. A directory errand lists a host this way on the connection
  it asks the flat rows on: a refresh delivers the tree once, whole, or after
  two seconds with what arrived, and a watch delivers it again on every
  change. A watch services its connection on a timer as an attached session
  does, so a host with nothing to say does not find it silent, and when its
  carrier drops it says so and dials again on the reconnect schedule; what
  it held is marked stale until the new connection has described the host. `Runtime::buildHostOutline` turns the tree into the rows every
  frontend draws, from the person's title settings, tag rules and theme.
- What a structure says is changed by calls addressed to the host, each
  naming its object as a listing names it: `label.set` and `tags.set` take a
  workspace, a window, a tab, a pane or a session written `kind:number` and
  set or clear its label or its hand-put tags; `workspace.rename` and
  `tab.rename` give a name or take it away; `tab.move` puts a tab at a place
  among the tabs of a window of its workspace, reordering within its own
  window and arriving shown in another; `window.close {window, expectedRevision}` closes a window and
  ends the sessions in its panes, and closing a workspace's only window ends
  the workspace. `expectedRevision` is the containing workspace's nonzero
  revision as a canonical decimal uint64 string. The host compares it before
  requesting consent and again immediately before mutation. A changed revision
  requires a fresh tree and confirmation; an unconfirmed local close intent
  cannot authorize this operation. `ubq --json tree` exposes exact workspace revisions
  in `workspaceRevisions`. `workspace.addWindow {window, pane, expectedRevision}`
  creates a window, tab and session from an explicit source pane in the named
  window. Its reply echoes sourceWindow/sourcePane and returns the new
  window/tab/pane/session identities. Creation requires both Drive and
  NewSession from the original requester, including after consent; a failed
  creation preserves the source window, tab, focus and zoom. The raw AddSurface
  tag cannot authorize creation. `workspace.addTab {window}` creates a tab and its session;
  `workspace.split {pane, orientation, ratio}` splits a pane in its window's
  active tab, with an explicit horizontal/vertical divider and ratio, and
  creates the new pane's session. `workspace.closeTab {tab}` and
  `workspace.closePane {pane}` end the sessions in the named object, closing
  its empty parents as necessary. A closed pane takes no input from its close
  on: input the host reads behind the close, or held from before it, is
  dropped rather than written to the session it ends. Creation returns the new pane/session ids
  (and tab id for addTab); closing returns the object id and sessionsEnded.
  `tab.balance {window, tab, expectedRevision}` balances one explicit tab;
  `pane.adjustRatio {window, tab, pane, direction, step, expectedRevision}`
  nudges an explicit pane's divider by a positive step no greater than one.
  `divider.setRatio {window, tab, path, ratio, expectedRevision}` sets a
  split's first-child fraction. `path` is an array of at most 63 binary child
  choices (0 first, 1 second); `[]` selects the root split. It is scoped to
  the exact workspace revision, not a persistent divider identity or a pool
  index. Missing nodes and leaf paths refuse. Ratios are requested fractions
  from zero through one; the pane model's interior clamp applies. Replies
  echo the exact selectors and requested step or ratio; Structure carries
  the resulting geometry. These calls permit an inactive tab and never
  substitute the active tab or alter selection. Native delayed drags retain
  the exact surface and adopted mirror generation before converting an index
  to a path; changed topology or surface retires the drag. Stale-revision
  refusal requires a fresh user intent, with no automatic retry.
  `client.label {label}` changes only the sending live client's optional label;
  no target identity is accepted. Its reply names that physical client and the
  exact label. It requires Drive and authority to rearrange its attached
  workspace: an agent must hold standing for every session and no person may
  be attached. This operation does not create a consent grant or relabel a
  neighboring connection. Labels retain the shared 64-byte, control-character
  and reserved-name rules; an empty label clears it. A raw LabelClient verb
  cannot authorize the change. The label lasts as long as the connection: the
  host forgets it when the connection goes, and a resume, which is a new
  connection, starts without one.
  `window.activate {window}`, `tab.activate {tab}` and `pane.focus {pane}`
  name explicit navigation targets; focus requires the pane's tab to be active.
  `window.moveFocus {window, pane, direction, expectedRevision}` names the
  observed focused pane and a direction (`left`, `right`, `up`, or `down`).
  `pane.zoom {pane, state, expectedRevision}` sets `zoomed` or `unzoomed`,
  never an implicit toggle. Both state-derived operations require the exact
  workspace revision, including when requesting agent consent. A stale
  revision or changed source focus is refused without mutation; the caller
  refreshes the tree and does not automatically retry the intent.
  `pane.move {window, pane, target, orientation, side, expectedRevision}`
  places a pane before or after another pane in that window's active tab.
  Orientation is `horizontal` or `vertical`; side is `before` or `after`.
  `pane.swap {window, pane, target, expectedRevision}` exchanges their positions.
  Both require distinct panes in the same active tab and the exact observed
  workspace revision, preserve session identity, and refuse stale placement
  without mutation or automatic retry.
  `pane.extractTab {window, pane, expectedRevision}` moves the pane into a
  new tab in its current window. `pane.extractWindow` takes the same parameters
  and moves it into a new window and tab. `tab.extractWindow
  {window, tab, expectedRevision}` moves a whole tab into a new window.
  Extraction accepts an inactive source tab, checks its exact source window
  and observed revision, and preserves every session and pane identity.
  Replies echo `sourceWindow`, the original `pane` and `sourceTab` for pane
  extraction, or the original `tab` for tab extraction, and name the resulting
  `window` and `tab`. A minted container is not permission to create a session.
  `pane.moveToTab {window, pane, tab, target, orientation, side, insertTarget,
  expectedRevision}` moves the pane into the target pane's named tab.
  `tab.moveToPane {window, tab, target, orientation, side, insertTarget,
  expectedRevision}` merges the source tab into the target pane's tab.
  `insertTarget` is `pane` to split the target leaf or `root` to wrap that
  target's complete containing layout. Both accept inactive source and
  destination tabs; a pane can also be rearranged within its own tab.
  `pane.moveToNewTab {window, pane, targetWindow, expectedRevision}` gives
  the pane a separate tab in the named window, reusing its source tab when
  it already has only that pane. Transfers require one shared workspace and
  the exact observed revision; they never create or end a session. Replies
  distinguish `sourceWindow`/`sourceTab` from destination `window`/`tab`
  even when they are equal, and echo the pane/target and exact placement.
  A call that is refused says why in its error's message (a
  label that reads as an identity, a name another workspace holds or one
  longer than a structure carries, a place a window does not have) and
  changes nothing. A change that is served is written to the host's record
  and is a new revision of the workspace's structure, so every reader of it
  is sent it; the sessions a closed window held end after the pass that
  served the call.
- All tree operations require a driving role and Drive scope. Add-tab and
  split additionally require the original requester's NewSession scope both
  when asked and when applied; approval supplies no missing token scope.
  An agent's created sessions retain that agent's creator grant, never the
  approver's identity. A daemon-verified
  person's own call retains the workspace rules below. An agent's call waits
  for an attached person's approval of that exact operation: standing attach
  grants do not answer it, and the host cannot distinguish a person typing
  `ubq` from automation running it. Without an eligible attached person the
  call refuses immediately. Agent label and split requests cannot bypass this
  exchange; they refuse instead.

  | Caller | `label.set`, `tags.set` | Rename, move, add-tab, split and close calls (creation also needs NewSession) |
  | --- | --- | --- |
  | The host's own user at the host, through a client the host trusts (the app, the attach bridge) | served, in any workspace | served, in any workspace |
  | A person on another machine whose token may drive, in the workspace their connection is attached to | served | served |
  | The same person, in any other workspace | refused: names the workspace the connection is attached to | refused: the same |
  | A viewer, and a person whose token only observes | refused: the role | refused: the role |
  | An agent whose token cannot drive | refused: the Drive scope | refused: the Drive scope |
  | An agent whose token may drive, with no eligible person attached to the affected workspace | refused: no attached approver | refused: the same |
  | The same agent, with a verified Owner/Driver person holding Drive scope attached | held for that person's approval of one exact operation | held for the same approval |
  | `ubq` typed by a person in a shell on the host | an agent to the host: the agent rows | the agent rows |

- An exact-operation question is a `StructureConsent` notify naming the
  request, a session, the workspace and its revision, the workspace's name,
  the method and its parameters as one JSON object the method's schema
  accepts, the claimed agent label and the verified peer process. The session routes
  it to one of the recipient's attached panes. Core supplies sanitized prompt
  text and Allow Once/Deny actions. The unmodified original request stays at
  the host; no UI-supplied operation parameters are accepted. The person's
  `consent.respond` call contains only
  `{request:u32, kind:"structure", decision:"deny"|"once"}`. Approval
  revalidates the requester connection, attachment incarnation, role, principal
  and required scopes; the target workspace revision; and the answering
  person's verified identity, attachment, Owner/Driver role and Drive scope.
  Permission for the exact operation never promotes the requester or creates
  a reusable grant. A denial, changed target, lost authority or expiry changes
  nothing. Each question is consumed at most once.
- One question may be pending per requester, at most sixteen at the host;
  its parameters are at most 32 KiB and the complete notification must fit one
  frame, which names of 4096 bytes do. Questions
  expire after thirty seconds; CLI tree calls wait thirty-five seconds to
  receive that refusal. An eligible person who attaches, or resumes, while a
  question waits is asked it on that connection. When no eligible person
  remains attached, a question waits out its thirty seconds if one it was
  asked of lost the link, and is withdrawn at once otherwise. The host
  retires a question with a `ConsentResolved` notify naming its kind, request
  and session,
  including on requester disconnect or authority withdrawal. Frontends dismiss
  only the matching presentation. Core presentation tokens are never reused
  across reconnect, so an old dialog cannot approve a new daemon's question.
  Retirement names only the actual recipients to whom the question was
  admitted, and dismisses only that exact kind, request and session.

- Attach, termination and capability questions are `ConsentQuestion`
  notifies naming their kind, request and session. They carry the verified
  `peerProcessID`/`peerProcess` separately from the claimed `agentLabel`; an
  unknown process is PID 0 with an empty process string. Capability questions
  additionally carry `capabilityCode`, `capabilityOrigin` and
  `isRecipientsOwnRequest`. A `ConsentResolved` notify retires a question of
  any kind by its kind, request and session alone. The host retires every
  question unanswered thirty seconds after asking it, and refuses for itself.
  An answer from any recipient decides a question for all of them, so a
  client answers only a question it has shown and declines the rest as
  `unshown`. A truncated or extended
  record, an unknown enumeration, malformed UTF-8 or an embedded NUL refuses
  before presentation.
- `consent.respond` carries exact `{request,kind,decision}` parameters;
  decision is `deny`, `once`, `session` or `always`, narrowed by the operation's
  lifetime policy. A human Viewer with Observe may decide a generic question
  only while still subscribed to its exact session and recorded as an actual
  recipient. Structure retains its stricter Owner/Driver plus Drive gate and
  only `deny`/`once`. Registry admission never replaces these per-kind checks.
  A recipient that cannot show a question, its line full or the question
  beyond what it can present, sends `unshown` for any kind instead of an
  answer: the question stays with the others asked, the host never asks that
  connection it again, and it refuses the question at once when nobody else
  is still asked. A recipient that departs, or that the host disconnects or
  refuses, leaves every question it was asked the same way, at once, and a
  pane leaving a connection declines the questions it holds. An attach,
  termination or tree change is asked again of a person who resumes, so it
  also waits for an approver who may return, whose attachment the host keeps
  to be resumed: one whose link was lost or went silent, whose client said
  it was suspending, or whose attachment the host moved to a resuming
  connection that can be asked. A capability question is asked once and
  waits on no one who left. An answer already given stands until it is
  delivered, unless the owner disconnects the approver or their token is
  revoked or expires first. A client whose `Greet` names `ShowsNoQuestions`,
  as the editor clients do, is asked none, though the person on it is
  present and may revoke.
- `consent.revoke` carries a canonical `session:"session:N"` and one exact
  action: `disconnect` with the stable lowercase attachment ID,
  `forgetAlways`/`forgetSession` with the daemon-derived grant identity, or
  `disconnectAgents` without a selector. Numeric display client IDs are never
  revocation authority. Session grant selectors remain meaningful after their
  originating client leaves. Both methods answer their exact correlated Call.

- A record a legal host builds always fits. A host runs at most 64 panes, a
  tab holds a pane at least and a window a tab, so a workspace at every cap
  is under one mebibyte (`maxWorkspaceStructureBytes`, checked at compile
  time against the caps above). A record of a few panes is a few hundred
  bytes.

The notification is an `Attachments` notify (above): a nonzero revision and
the entries, each with its fixed fields, its panes and the optional principal
and presence each flagged present or absent. Duplicate attachment or pane IDs
refuse the list.

- `attachmentID` and each pane ID are nonzero; `role` is `Driver` or
  `Observer`, from whether the attachment may send input; `clientKind` is
  `Human`, `Agent` or `Viewer`; `state` is `Live` or `Lost`. These display
  values do not promote the recipient's granted role.
- A client is listed the attachments that hold a pane it has bound, itself
  among them, and no other: who else is on the host is not its to know. Each
  row's panes are that exact intersection, not all panes the peer holds;
  a self row may remain with no panes on a control-only connection.
  `geometryOwner` and `primary` say it of any pane the recipient has bound;
  `PaneFacts` say it pane by pane.
- `principal` is present on every entry for an `Owner` recipient and omitted
  on every entry for a `Driver`, `Viewer`, or `Agent` recipient. Each connection
  derives its own list after the role is granted.
- A list is sent when it differs from the last one the connection sent.
  Its notification revision advances for presentation changes too, without
  manufacturing attachment-table authority changes. A client takes a new
  connection's first list whatever its revision. The existing client
  attachment owns one canonical current roster; a newly opened native pane
  derives its presence from it using the exact current bound pane identity,
  not a reconstructed daemon instance or a queue of old roster snapshots.
- Only a live row may carry `presence`, and only for the current human Observe
  audience. Its fields are `clientID`, `processID`, `process`, `claimedLabel`,
  `grantIdentity`, `quic` and `sharedPanes`. `clientID` is display-only; verified
  process facts are distinct from the claimed label and from the verified
  server-derived grant key. The latter is at most 65535 UTF-8 bytes with no
  NUL, empty when unavailable, and never contains raw token material.
  `sharedPanes` contains exactly one `(paneID, grant)` for every base row pane,
  where grant is `None`, `Session` or `Always`; no missing, extra or duplicate
  pane is accepted. A standing grant must not be flattened across sessions.
- One validation and size walk admits the complete record before it is
  encoded. If optional presence
  enrichment would exceed one frame, the producer omits **all** enrichment
  for that snapshot, not selected peers or grants. Base rows, pane intersections
  and Owner-only principals remain intact. The UI still counts peers, while
  unavailable grant selectors are disabled rather than invented. A mandatory
  base roster that cannot fit closes with `Bye{Oversize}` and is never cached
  as sent; a stale roster is not reported as current.
- The connection and the client attachment hold the list as values. The
  record's codec and the two calls an owner makes each pass, `serveNotifies`
  on the host and `readNotifies` on the client, are in `Sync/Notify.h`.

#### Versions

Every handshake opens with `wireVersion`, the one version of the wire the
sender speaks. `Greet` and `Welcome` also carry `appVersion` (the build's
version, e.g. `0.2.0.3`; `Sync/AppVersion.h`). The constant lives in
`Source/Core/Protocol/Sync/Envelope.h`; what a build says about itself to a
peer lives in `Source/Core/Protocol/Sync/WireVersion.h`.

The wire version is the protocol identity and ordering. Every change to the
wire grammar, typed operations, records, capabilities or Unicode width table
increments `wireVersion`. `appVersion` is a display fact only; it never
changes admission or chooses which peer speaks the older protocol.

`Greet` and `Welcome` also carry `engineFingerprint`, this build's
`wireFingerprint` (Envelope.h): the SHA-256 of a fixed corpus of every body the
wire defines. `Tests/Core/Protocol/Sync/test_sync_wire_fingerprint.cpp` pins
that corpus to the wire version and fails until a grammar change advances the
version, re-mints the fingerprint and regenerates the editor wire tables.
Its `Greet` names every capability bit a client may name. The fingerprint also
refuses a development build that changed its grammar without advancing its
version. Equal versions with different fingerprints are different builds,
not an older and a newer protocol.

`lastPublishedWireVersion` and `lastPublishedWireFingerprint` in Envelope.h
freeze the identity shipped in v0.3.3. The fingerprint suite refuses a changed
identity without a version increment. At publication, update both release
pins from the release tree; they are evidence, not an additional admitted
protocol. The current and released wire identities are independent of the
handover reader floor in §6.13.

- **One active version.** Two peers connect only when they speak the same
  version and fingerprint; a peer at any other is refused. There is no
  negotiated range or old-peer decoder: each wire change takes the next
  version, and both ends move together. `Scripts/check-wire-version-pins.py`
  fails when a minimum peer version or a negotiation history returns to the
  headers. Emacs and Neovim get the version and fingerprint from
  `Scripts/gen-wire-tables.py`; Python transport probes read the same header.
- **The build is read first.** A reader takes the prefix `wireVersion
  appVersion engineFingerprint`, which every build lays out alike, before the
  rest of a handshake body, because a peer of another build lays the rest out
  its own way. Another version or another fingerprint is refused there with
  `Bye{VersionMismatch}`, and the person reads that this app and the host are
  from different builds, which one is older when that is known, and which to
  update. A body that fails to decode after a matching handshake is damage on
  the way, not skew, and fails its pane with its own sentence. The full `Greet` and `Welcome` readers require every field
  of the one contract and its exact end. Optional peer metadata on `Bye`
  identifies a refusing build; it does not make another handshake layout
  acceptable.
- **Welcome is one exact latest contract.** Its admission, consent binding,
  predecessor flag, resumed count and paste flag are all required, even when
  empty. `Granted` carries zero consent identifiers. `AwaitingConsent` carries
  one nonzero request and session and no resumed panes. It grants no resume,
  pane, observation or operation authority, and is not persisted as a resume
  identity. The client accepts exactly one matching `ConsentStatus` Pending
  notify followed by one Approved, Denied or Expired. Only Approved
  presents Connected or permits pane data; Denied and Expired require
  `Bye{NoConsent}`. Neither a duplicate transition nor another question can
  change that state. The host queues Approved and promotes the same pending
  attachment in one owner-thread transaction, after output admission and
  fresh authority, target and deadline checks. Backpressure never extends an
  approval's lifetime.
- **One complete handshake schema.** Unknown suffix bytes and every proper
  truncation are refused. Empty optional fields still carry their presence flag
  or count; a writer cannot omit an earlier field or stop after its last
  nonempty value. `test_sync_wire_version` checks all proper prefixes and an
  appended suffix for both handshakes. No other codec admits a peer.
- **The refusal names the facts.** When the versions differ, the host answers
  `Greet` with `Bye{VersionMismatch}` carrying its version and app version, and
  a client refuses a `Welcome` at another version with that reason. Core
  decides which side is older — the one with the lower version
  (`olderSideOfVersions`) — and composes the sentence a person reads
  (`versionRefusalSentence` and `versionRefusalTitle` in
  `Source/Core/Session/DeployPolicy.h`). Frontends show it:

  App versions appear in the sentence as display facts. Different wire
  versions identify the end to update. Equal wire versions with different
  fingerprints say "Runs a Different Build" and ask for matching builds,
  without ordering the app versions. An unreadable legacy host can be offered
  an update, but no app-version comparison grants compatibility.

  The update action installs this app's own host. It is offered when the host
  speaks the older protocol, states no build, or states the same protocol with
  a different fingerprint. In the last case the person explicitly chooses to
  install a matching build; the dialog claims no older side. When this app
  speaks the older protocol, it directs the person to update the app and never
  offers to replace a known newer host.


### 7.4 Screen plane (pane channels)

| Kind | Dir | Lane | Fields |
|---|---|---|---|
| `View` | C→H | I | `state:u8 {Live, Dormant, RecordsOnly}` |
| `PaneDirty` | H→C | I | (empty) |
| `PanePalette` | H→C | I | the palette block, whole, as an update's `Block` op carries it (§6.8): the live palette and its baseline, the dynamic colours, the cursor's text colour and the saved palettes |
| `CheckpointHead` | H→C | I | `generation:u64`, `transfer:u32` (the `BulkChunk` transfer carrying the body), `frameClass:u8`, `stateDigest:u128` |
| `Update` | H→C | I (datagram if superset and fits) | `mode:u8 {Chain, Superset}`, `baseGeneration:u64`, `generation:u64`, `frameClass:u8 {Boundary, Settled, Flood, Forced}`, `hostTime:u64`, `lagMicros:varint`, `inputWritten:u64`, `inputSettled:u64`, `historyHead:u64`, `effectsThrough:u64`, `ops:[Op]`, `stateDigest:u128` |
| `Resync` | C→H | I | `reason:u8 {DigestMismatch, BaseMissing, DecodeFailed, CheckpointRefused, BufferOverflow}`, `generation:u64`, `sectionHashes:[u64]` |

`Update.lagMicros` is `hostTime − dirtySince(baseGeneration + 1)`, read from
the touched ring (§6.2), or from its oldest entry when `baseGeneration + 1`
has left the ring: the age of the oldest change this update delivers. `hostTime` is a send time, so without
it a client cannot tell a program that is thinking from a picture that is
three seconds behind. The frontend may show how far behind the picture is.
The fields sit between `hostTime` and the row-update body, and
`ops` and `stateDigest` are that body (`RowUpdate`, ScreenPlane.h).
`historyHead` is the history lines
the host's terminal had appended when the update was cut. Frames a daemon takes
while no client shows the pane advance no generation, so the next published
generation's `dirtySince` is the first of their changes.

`Update.effectsThrough` is an advisory effect-generation boundary; zero advances
no boundary. It is not an effect receipt and cannot complete a partial batch.
The explicit `Effect` packet generation and record countdown establish the
picture dependency and batch completion (§7.7), including when there is no
screen update or every selected sound is skipped.

**Checkpoint body** (bulk, deflated when above 4 KiB):

- **Header:** `generation`, `dictionaryEpoch`, `rows`, `cols`, `cellPixels`,
  `textAreaPixels`, `activeScreen`.
- **`ModesBlock`:** the full DEC and ANSI mode set *for both screens*, the input
  modes, the progressive keyboard-enhancement stacks and the mouse modes.
- **`PaletteBlock`:** live and baseline palettes, plus the dynamic colours and the cursor text colour, and the saved palettes with theirs.
- **Cursor state:** `CursorBlock`, `SavedCursor` per screen, scroll region, and
  `DECLRMM` with left and right margins.
- **Character and text state:** tab stops (bitset), live G0–G3 with GL and GR,
  `titleStack`, UDK, the REP record, the status line, the text-size table, and
  `syncOutputOpen` with its entry clock.
- **Dictionaries:** `{pens, links, longClusters}`.
- **`screens`:** the active screen and, when present, the inactive one, as
  `[RowForm + RowAttributes]`.
- **Images:** `placements` plus the image-store metadata (ids, numbers,
  `nextImageID`, budget, LRU order).
- **Remaining sections:** the soft-font (DECDLD) and vector-graphics sections,
  `historyEpoch`, `historyHead`, and `stateDigest`.

The client verifies `stateDigest` on import *before* swapping the result in.
The checkpoint is complete: the checkpoint ledger (§10) classifies every `Terminal`
member, and an unclassified member fails the build.

**Ops** apply in order, and defines precede uses.

| Op | Body | Notes |
|---|---|---|
| `DefinePen` | `id:varint`, `Pen` | `id` must equal the watermark. A `Pen` is the full attribute tuple: colours, flags, underline style and colour, sizing. |
| `DefineLink` | `id:u16`, `key:bytes`, `uri:bytes` | The host's own link id and the OSC 8 `id=` key it was opened with, as the host's ring holds them. Opaque, NUL-free key and URI bytes, at most 31 and 2,047 bytes. |
| `DefineCluster` | `id:varint`, `utf8` | Only for clusters over 32 B |
| `ScrollRect` | `screen:u8`, `top:u16`, `bottom:u16`, `left:u16`, `right:u16`, `delta:svarint` | Chain mode only. Revealed cells take the canonical blank of the rectangle's erase pen. |
| `Row` | `screen:u8`, `row:u16`, `RowForm` | Whole-row replace |
| `Spans` | `screen:u8`, `row:u16`, `count:varint`, `(column:u16, RowForm)` × count | Chain mode with base = the previous generation |
| `RowAttributes` | `screen:u8`, `row:u16`, `lineAttribute:u8`, `flags:u8 {Wrapped, PromptMark, ExplicitWidths}` | DECDWL and DECDHL are values, not keyframe triggers. Row records carry their line attribute and wrap/mark metadata (§7.4 `RowForm`). |
| `Cursor` | `CursorBlock` | When changed |
| `Modes` | `revision:u32`, `ModesBlock` | When the revision changed |
| `Palette` | `entries:[(index:u16, rgb:u24)]`, `dynamic:[(slot:u8, rgb:u24)]` | Changed entries only |
| `ScreenSwitch` | `active:u8` | The other screen's rows follow as `Row` ops if the client lacks them |
| `Placements` | `revision:u32`, `PlacementList` | Whole list. Each placement names an image hash. |
| `EchoContext` | `modesRevision:u32`, `foregroundGeneration:u32`, `flags:u8 {SecureEntry, ForeignInput, AtPrompt}`, `present:u8 {Interrupt=1, Quit=2, Suspend=4, Erase=8, EraseKnown=16}`, `interrupt:u8`, `quit:u8`, `suspend:u8`, optional `erase:u8` (the live `VINTR`, `VQUIT`, `VSUSP` and `VERASE`; `EraseKnown` with no `Erase` means disabled; no `EraseKnown` means DEL; the erase byte appears only with `Erase`) | When changed and after every checkpoint, on the stream, to a client whose `Greet` names `EchoContext`; outside the digest. |

The block encoding carries the cursor, the modes, the palette, the per-screen state and
the character and text state each as a whole `Block` op holding the bytes the
checkpoint carries for it, sent when those bytes move. Op 7 is unassigned;
history travels on §7.5's plane.
A sixth block, `Activity`, carries the OSC 3008 contexts and the OSC 7501
program status records as a whole set; `StatusRecords` (§7.7) defines their
record semantics. A
seventh, `RichText`, carries where each screen's OSC 7404 blocks are
(Docs/rich-text.md); a block's document is defined once per attachment by a
`DefineRichText` op naming the block's serial, which names one placement for
the life of the host terminal, and a checkpoint defines every document it
places. Placement records include the application document revision and Unicode scalar
viewport anchor. The layout each viewer draws and hit-tests is derived from its
own typography and never replicated as screen state. Native geometry requested
for measurement or capture travels separately, pinned to the selected authority
and renderer revision. A checkpoint replaces the replica's block
serial namespace and discards layouts cached for its previous picture, even
when a serial matches; an update may reuse layouts within the same picture.
A screen switch is an update that names the other screen active, with every
block and the rows of both screens that
moved; the state digest covers both screens and the seven blocks, for
checkpoint and update alike, and no history term
(`Source/Core/Protocol/Sync/ScreenPlane.h`,
`Source/Core/Protocol/Sync/RichTextForm.h`).

`Spans` (9) and `ScrollRect` (10) go only in a chain update, to a
client whose `Greet` names the capability (`SpanOps`, `ScrollRectOps`, §7.3);
an editor client names neither and is sent whole rows. `screen`
is 0 for the active screen and 1 for the other. `Spans := screen:u8 row:varint
baseHash:u64 rowHash:u64 count:varint (column:u16 cells:u16 RowRecord)×count`:
the client checks it holds the row as `baseHash`, decodes each record into
`cells` cells from `column`, keeps the row's line attribute, flags and mark,
and takes `rowHash` as the row's hash, which the state digest then checks; the
two hashes are what lets a client that never re-encodes a row check it. A
span never splits a wide pair. `ScrollRect` is full-width: `screen:u8 top:u16
bottom:u16 delta:svarint`, row `r` taking row `r + delta`, a row whose source
falls outside the rectangle revealed blank with hash 0 for a later `Row` to
write; it precedes the rows of its screen, at most one per screen, and no
`Spans` names a row it moved. A margined scroll with left and right margins
travels as rows.

A client whose frontend draws no sized text (OSC 66) turns every sized run it
takes — on either screen and in the history it projects — back into
ordinary cells, keeping the text; one that draws sized text keeps the runs
and the size table they name.

`RowForm := extent:varint, text:utf8, runs:[(units:varint, pen:varint)],
links:[(units:varint, link:varint)], clusters:[(gap:varint, codePoints:varint)],
clusterRefs:[(gap:varint, cluster:varint)], wideKept:[gap:varint]`. A row travels
as `RowRecord := lineAttribute:u8, flags:u8 {Wrapped}, mark:u8, RowForm`, and its
hash is XXH3-64 over the record. Its rules (`Source/Core/Protocol/Sync/RowForm.h`;
the codec is `Source/Core/Screen/RowForm.h`, which the scrollback store folds
cold blocks with as well):

- Cells at and past `extent` are the canonical blank, so trailing blanks cost
  nothing.
- The cells before `extent` group into *units*: one cell each, except a wide
  lead and the continuation the Core writes beside it, which are one unit whose
  continuation never travels. A wide lead with any other partner is listed in
  `wideKept`, and the partner travels as its own unit. Counting units rather
  than cells keeps a CJK row one text entry and one run per character, though
  the two halves carry different pens.
- `text` holds each unit's lead cell: its base code point, then its inline
  combining marks; an unwritten cell is U+0000, so a cell the program never
  touched stays distinct from a space it wrote. A unit is one code point unless
  `clusters` gives its count. Cell boundaries are carried, not re-derived with
  the segmenter, because the Core's cells follow cursor motion and mode 2027 as
  well as the text.
- `runs` and `links` are run-length over units and cover every one; an empty
  `links` means no link. A pen is a dictionary id. A link and a cluster are the
  host's own ids, because a cell carries them and the client's cell must equal
  the host's byte for byte: the link dictionary defines (id, key, URI) as the
  host's ring holds it, and the cluster dictionary is the host's cluster pool,
  append-only below its id ceiling. Every pool cluster rides `clusterRefs`, of
  whatever length, for the same reason.
- An id no live link slot holds is a link the host's ring retired; it resolves
  to no target on either side and travels undefined.
- `widths` is not carried: the Core stores no width override a cell does not
  already spell through its pen.

The checkpoint body carries no history line: it names the history's
`historyEpoch` and `historyHead`, and the lines travel on §7.5's plane.

### 7.5 History plane (pane channels)

| Kind | Dir | Lane | Fields |
|---|---|---|---|
| `HistoryInline` | H→C | I | `historyEpoch:u32`, `firstLine:u64`, `lines:[LogicalLine]` |
| `HistoryHead` | H→C | I | `transfer:u32`, `historyEpoch:u32`, `firstLine:u64`, `count:u16`, `order:u8 {Ascending, Descending}` (the body follows as `BulkChunk`s) |
| `HistoryReset` | H→C | I | `newEpoch:u32`, `clearedThrough:u64`, `historyHead:u64`. The client purges before it applies anything else. |
| `HistoryWant` | C→H | I | `historyEpoch:u32`, `ranges:[(from:u64, to:u64)]`, `priority:u8 {Viewport, Search, Backfill}`. Replaces the outstanding set at its `priority` (§6.6). |

A `HistoryInline` after its header, and the payload of a `HistoryHead`'s
transfer, are one history body:

```
HistoryBody := pens:[Pen] links:[(hostID:u16, key:bytes, uri:bytes)]
               clusters:[(hostID:u16, codePoints:[varint])] lines:[LogicalLine]
               runTexts?
LogicalLine := flags:u8 {Truncated, Open}, RowRecord (never wrapped), lineHash:u64
runTexts    := [(line:varint, column:varint, hostID:u16, codePoints:[varint])]
```

- **A body is self-contained.** A line's `RowForm` names pens, links and long
  clusters by their index in the body's own tables, never through the pane's
  dictionaries: an append precedes the update whose definitions it would need
  (§7.8), and a dictionary epoch that rolls would strand any id a cached line
  named. A link or cluster entry also carries the host's own id for it, the
  id the client's mirrored link ring and cluster pool hold it under, so the
  client resolves a history cell as it resolves a screen cell and never
  mints an id the screen plane may later define otherwise.
- **Link keys and URIs** are opaque, NUL-free byte strings, at most 31 and
  2,047 bytes as the host's ring stores them. They need not be valid UTF-8:
  a field clipped to the ring can end within a multibyte sequence. History
  preserves the same bytes as the screen plane.
- **A logical line** is its rows joined with no width: each row's trailing
  unwritten cells are dropped before the next is joined, which drops the pad
  a wide character leaves at a margin it did not fit. Its `RowRecord` carries
  the first row's line attribute and mark.
- **`lineHash`** is XXH3-64 over the line attribute and the line's `RowForm`
  written with tables of its own, numbered in the order the line first names
  each entry, followed by those tables. It does not depend on the width, the
  body or the ids either end used, nor on the mark.
- **`runTexts`** give each OSC 66 proportional run in the body's lines its
  text: the line's index in the body, the anchor's column in it, and the
  host's run-text id. A checkpoint mirrors the host's whole run-text store,
  and a run written since may reach history without the client seeing it on
  a screen, so the client gives the anchor that id, defining the text there
  when its mirror holds none under it; an id its mirror holds for another
  text names none until the next checkpoint settles it. The host keeps an id
  while any line of its history names it. The section follows the last line
  only when some line holds a run. Each entry names a sized anchor of its
  line, in order of line and column, with one to 4,096 scalar values, all of
  them together within a run-text store's 262,144; anything else refuses the
  body. `lineHash` does not cover the section, so a line hashes alike with
  or without it. What a line shows is its content hash: `lineHash` for a line with no
  run, and otherwise a hash of `lineHash` and each run's column and text. The
  host tells a rewritten line, and its open line, by the content hash, so a
  line whose only change is a run's words is sent again; the client keeps its
  projection of the open line by it too.
- **A chain moves whole.** The client wraps a line at its own width the way
  the host's rewrap does: a wide lead and the continuation cells after it — a
  wide character, a sized glyph, a proportional run's claim — start the next
  row together rather than straddle the margin. A chain wider than the
  client's row is laid flat, as a client that draws no sized text lays it;
  the client never mints the narrower specification the host would clamp it
  under.
- **`Truncated`:** a line longer than 16,384 cells travels cut to that many.
- **`Open`:** the line straddling the seam (§6.6), alone in its body and
  numbered `head`; one with no cells withdraws the open line the client holds.
- A `HistoryReset` whose `historyHead` is below its `clearedThrough` is
  refused, and so is a `HistoryHead` whose `count` is 0. A want's ranges are
  non-empty, sorted and disjoint (adjacent ranges are allowed), at most 64;
  an empty list withdraws the set at its priority.

`HistoryInline`, `HistoryHead` and history `BulkChunk`s are discarded unless
their epoch is the client's current epoch; a chunk takes the epoch of its
`HistoryHead` (§6.6).

Appends that fit in one frame ride as `HistoryInline` on the interactive lane,
ahead of their `Update`. History therefore precedes the update that scrolls it
without paying bulk-transfer latency for one scrolled line.

### 7.6 Input plane (pane channels)

| Kind | Dir | Lane | Fields |
|---|---|---|---|
| `Keys` | C→H | I | `inputSeq:u64`, `events:[{key:u32, modifiers:u16, kind:u8 {Press, Repeat, Release}, text:utf8, unshifted:u32, shifted:u32, baseLayout:u32, flags:u8 {Modified}}]`. `key` is a `Keys::VirtualKey`, `modifiers` carries the key encoder's flags in the low byte and DECEKBD sided modifier state in the high byte. The three code points are the layout's key with every modifier removed, with Shift applied when it differs, and the standard layout's key at the same position (0 when the platform cannot say): the facts the host's key encoder reads, since the host decides the bytes. At most 64 events; `text` at most 32 bytes. |
| `Pointer` | C→H | I | `inputSeq:u64`, `events:[{kind:u8 {Press, Release, Motion}, button:u8, modifiers:u16, cell:(row:u16, column:u16), pixel:(x:u16, y:u16)}]`. `button` is xterm's button code and `modifiers` its report modifier bits; a pixel of 65535 is none. Only events the client's pointer ladder routed to the program travel. At most 64 events. |
| `Paste` | C→H | B (upward) | `inputSeq:u64`, `offset:u64`, `state:u8 {More, Final}`, `utf8`. Chunked at 32 KiB with one `inputSeq`, at most 16 MiB total. `offset` is the paste's bytes before the chunk: the host takes only bytes past what it has of the paste, so a resend from 0 after a drop writes nothing twice, and a chunk past what it has is `Malformed`. Each chunk is whole UTF-8 scalars. |
| `PasteAbort` | C→H | I | `inputSeq:u64`. Ends the unfinished paste with that `inputSeq` (§6.7). It rides the interactive lane, so it reaches the host ahead of the key that follows it. A `PasteAbort` for another `inputSeq` than the paste in progress changes nothing; one that overtakes its paste's first chunk ends the paste before it starts. |
| `PasteReceipt` | H→C | I | `inputSeq:u64`, `bytesWritten:u64`. How much of the unfinished paste with that `inputSeq` the host has written to the program, from the paste's start; sent when it advanced, at most once a service pass. The paste's end is reported by `inputWritten`. |
| `InputDiscarded` | H→C | I | `inputWritten:u64`, `throughSeq:u64`. The client's events numbered above `inputWritten` and through `throughSeq` were dropped unwritten: a signal character discarded them with the input the terminal held unread. Sent ahead of any count that passes the run; a body whose run is empty is malformed. |
| `Raw` | C→H | I | `inputSeq:u64`, `bytes` |
| `Focus` | C→H | I | `focused:u8` |
| `Viewport` | C→H | I | `rows:u16`, `cols:u16`, `cellPixels:u16×2` (width, height), `textAreaPixels:u16×2`, `drawablePixels:u16×2`. A driver's size of this pane, and the pixels it draws the pane in, zero where it has measured none (§6.10). Only a client standing as a person with the Drive scope sends one: from an agent, a viewer or a client that may not drive it closes the connection `Malformed`, with the reason in the `Bye`. A size of no rows or no columns is dropped, and one past what a terminal holds is ceilinged. Never in a datagram. |
| `Receipt` | H→C | I | `inputWritten:u64`, `inputSettled:u64` |

The input kinds take 65 to 70 in this table's order after
`Receipt`'s 64 (`Raw` is 69). A client numbers its input from 1, per pane and
per client instance; the host writes `s` only when it is above everything that
instance has had written or queued on the pane, so a resend after a drop is
never written twice. The host stamps the client's own `inputWritten` on every
`Update`, and sends a standalone `Receipt` when no update has carried a write
20 ms after it (provisional, D15). A pane's unwritten input is bounded per
client instance, per connection and per host (§6.7). The host keeps reading a
client that sends input and closes one past its bound. Input from a `Viewer`, or from a client without the Drive scope, is
`Bye{Malformed}` (F17).

For `inputSettled`: A write settles once it was written at least
`E` (50 ms, `Docs/sync-measurements.md`) before a read of the
program's output began and found nothing, so the read's start, never its
end, is what is compared. The host also checks the PTY for quiet at the
oldest unsettled write's deadline, which counts as such a read begun then.
An `Update` reports the client's settled mark as it stood when its generation
was published, so a frame never reports an echo it does not hold. A
standalone `Receipt` advances `inputSettled` only while the client was sent
the newest generation and the terminal still holds it.

### 7.7 Status, effects, clipboard, images (pane channels)

| Kind | Dir | Lane | Fields |
|---|---|---|---|
| `PaneFacts` (48) | H→C | I | `revision:u32`; the title's inputs `programTitle`, `currentDirectory`, `homeDirectory`, `currentFileURL`, `processChain`, `processChainWithArguments`, `shellCommandName`, `terminalName`, `hostName`, each `bytes`; `rows:u16`, `columns:u16`; `end:{flags:u8 {didEnd, hasStatus, wasSignalled}, code:svarint}`; `foreground:{pid:u32, name:bytes, generation:u32}`; `flags:u8 {isAtPrompt, hasIndicator, hasTabColor, hasExitStatus, isAdopted}`; `progress:{state:u8, value:u8}`; `indicator:{colour:u24, label:bytes}` when flagged; `tabColor:u24` when flagged; `badge:bytes`; `userVars:[(name:bytes, value:bytes)]` (whole list); `exitStatus:svarint` when flagged; `pixels:{cellWidth, cellHeight, textAreaWidth, textAreaHeight: u16}`; `geometry:{flags:u8 {hasOwner, isPinned, isOverridden, hasPinnedBy}, owner:ID128?, pinnedBy:ID128?}`; `lastOutput:u64`, `lastInput:u64` (Unix milliseconds); `created:u64` (Unix seconds); `attention:{flags:u8 {bell, notification}, title:bytes, body:bytes}`. Whole, in one frame. A flag bit no flag names, an unknown progress state, a repeated variable name, a `pinnedBy` without a pin, or notification words with no notification invalidates the frame. The two activity stamps do not move `revision` by themselves. |
| `StatusRecords` (49) | H→C | I | `revision:u32`, `total:varint`, `first:varint`, `records:[{id:bytes, app:bytes, title:bytes, message:bytes, state:u8 {Idle = 1, Working, Done, Blocked, Error}, kind:u8 {None, Permission, Question, Auth}, progress:svarint (-1 for none)}]`. A record's bytes are the ones a checkpoint's activity block carries for it. Whole set: a set too large for one frame travels as several, back to back, `first` naming the index of each frame's first record; a frame with `first = 0` begins a set and abandons one in assembly, and the set replaces the client's when the frame reaching `total` arrives. An unknown `state`, a repeated id, or a frame that continues no set invalidates the frame. |
| `Effect` (50) | H→C | I | `generation:u64`, `throughSequence:u64`, `remainingRecords:u16`, optionally one complete effect record (below) |
| `ClipboardRequest` (51) | H→C | I | `requestID:u32`, `buffer:u8`, `kind:u8 {Types=0, Item=1}`, `mime:bytes` |
| `NativeDragCommand` (56) | H→C | B | `epoch:u64`, `kind:u8`, `session:u64`, `request:u64`, `final:u8`, metadata, `payload:bytes` |
| `NativeDragEvent` (77) | C→H | B | `epoch:u64`, `kind:u8`, `session:u64`, `request:u64`, `x:i64`, `y:i64`, `X:i64`, `Y:i64`, `operation:u32`, `error:u8`, `flags:u8`, `payload:bytes` |
| `ImageHead` (52) | H→C | I | `hash:u128`, `format:u8`, `width:u32`, `height:u32`, `transfer:u32` |
| `ImagePlacements` (53) | H→C | I | `revision:u32`, `images:[ImageBinding]`, `placements:[ImagePlacement]`; whole replacement |
| `SampleHead` (54) | H→C | I | `slot:u8`, `serial:u32`, `frames:u32`, `sha256:[32]`, `transfer:u32` |
| `ClipboardData` (74) | C→H | I | `requestID:u32`, `status:u8 {OK=0, Empty=1, Refused=2, TooLarge=3}`; a clipboard payload descriptor only for OK |
| `ImageWant` (75) | C→H | I | `hash:u128` |

An effect record is `generation:u64, sequence:u64, primary:u128, kind:u8,
count:u32, body`. The authenticated pane binding supplies its stable pane ID;
the record does not supply a different target pane. `sequence` and `count` are
nonzero. Kinds are Bell0, Notification1, CommandFinished2, Sound3, Attention4
and ClipboardWrite5. Bell and Attention have empty bodies. Notification carries
`title:bytes, body:bytes, identifier:bytes, occasion:u8` (Always0, Unfocused1,
Invisible2), bounded to255,255 and32 UTF-8 bytes. CommandFinished carries
`exitStatus:svarint, durationMicros:u64, currentDirectory:bytes,
commandLine:bytes`, bounded to4096 and8192 UTF-8 bytes for the strings.

An immutable batch contains at most256 records. Every packet repeats the same
generation and through-sequence and decrements `remainingRecords` exactly to
zero; record sequences increase and never exceed the through-sequence. A packet
without a record is valid only as a standalone completion with zero remaining
records. A `Live` pane must have applied the packet generation before consuming
it. Dormant and RecordsOnly views can consume effects without changing their
picture. Payload readiness and recipient authority precede consumption.
Only a complete batch advances `effectsAcknowledged`; the host accepts no
acknowledgement beyond the final batch packet and all required payload bytes it
has actually sent. Each bound handle retains its own countdown and acknowledgement;
aliases of one stable pane share a consumed high-water mark to prevent duplicate
presentation. Transport loss preserves that mark; definitive retirement of the
last binding clears it. Listing-only handles do not own effect receipts.
A resume naming an `effectsAcknowledged` beyond what its attachment was sent is
malformed. One behind what the attachment acknowledged comes from a client that
lost its receipts, as one restarted from an older record does: the pane resumes
`Checkpointed`, the effects it acknowledged are not sent again, and an effect
completion through the attachment's acknowledged sequence tells it which it will
not receive.

ClipboardWrite carries `buffer:u8` followed by a payload descriptor:
`transfer:u32, encodedBytes:u32, [inlineBytes]`. A zero transfer includes exactly
`encodedBytes` inline bytes; a nonzero transfer includes none and names raw
`BulkChunk` data. A clipboard bundle is `count:varint` followed by that many
`mime:bytes, data:bytes` pairs. At most32 distinct, nonempty MIME names are
permitted, each at most65535 UTF-8 bytes and containing no NUL. Data is binary;
its aggregate ceiling is1MiB for writes and4MiB for reads, excluding bounded MIME
metadata. A zero-count write is an explicit clear. Oversized or malformed bundles
are refused whole, never truncated. Only the immutable write recipient receives
the descriptor or bytes; other readers receive batch completion without those
contents.

ClipboardRequest requires Types exactly when MIME is empty; Item requires one
nonempty bounded MIME name. ClipboardData matches the same physical connection,
pane and request ID. A successful Types answer contains no item data; an Item
answer contains exactly the named MIME type. Non-OK answers carry no descriptor.
The host admits only Owner/Driver replies and verifies the original operation
again before allocation, each chunk and delivery. Raw transfer chunks must be
nonempty, consecutive and within the declared total; the final length and SHA-256
must match before any sink or receipt advances. A retired request cannot be
revived by a late reply. Local helper IPC remains independently framed and
authorized; it is not an alternate remote clipboard carrier.

Sound is `volume:u8, wave:u8, channel:u8, attackMillis:u16,
releaseMillis:u16, semitonesDown:u8, durationMillis:u32, gapMillis:u32,
sampleSlot:u8, decayMillis:u16, sustainLevel:u8, reverbSend:u8, gridUnits:u8,
lowpassHertz:u16, highpassHertz:u16, pan:u8, repeat:u8, count:u8`, followed by
at most 16 notes, each `frequency:u32, volume:u8, effect:u8, parameter:u8`.
These values obey the DECPS bounds in `Source/Core/VT/Sound.h`; no native pointer
is serialized. When wave is 6, the body ends with the exact sample identity
`serial:u32, frames:u32, sha256:[32]`; other waves carry no sample identity.
Samples contain mono signed16 little-endian PCM at44,100Hz, at most441000 frames.
SampleHead names slot 1–25. Empty state has zero frames, a zero digest and
transfer0; nonempty state requires a nonzero transfer with exactly twice the
frame count in raw bytes. The final transfer digest must match the head and the
Sound identity before that sound is consumed. A mutable sample-bank slot is not
an identity. Sample data is captured when the program enqueues the sound, before
later overwrite or reset. A sender replacing a slot's identity waits for that
handle's previous sample-dependent batch acknowledgement, not an alias's receipt.

ImageBinding is `imageID:u32, imageNumber:u32, hash:u128, format:u8,
width:u32, height:u32, isVirtual:u8, virtualColumns:u16, virtualRows:u16,
virtualPlacementID:u32`. ImagePlacement is `hash:u128, imageID:u32,
placementID:u32, hasPlacementID:u8, cursorX:u16, cursorY:i16,
cellColumns:u16, cellRows:u16, pixelOffsetX:u16, pixelOffsetY:u16,
sourceX:u16, sourceY:u16, sourceWidth:u16, sourceHeight:u16,
drawnPixelWidth:u16, drawnPixelHeight:u16, zIndex:i32, cursorPolicy:u8,
onAlternateScreen:u8`. The drawn size is both zero or both nonzero: nonzero
draws the picture at that many device pixels, the cell box then naming only
the cells it covers. Counts are bounded to 32 image bindings
and 128 placements. Formats 24 and 32 mean decoded RGB and RGBA bytes; image
dimensions and decoded size obey the terminal image limits.

A reader draws only pixels it verified. A binding whose pixels are missing
withholds its placements, with one exception, the retained picture: a picture
the reader verified and drew at a placement keeps being drawn at that
placement, under the image ID it was drawn with, until the binding's own pixels
are verified, when all of these hold:

- the record binds new pixels to the same image ID, or binds the placement to a
  new image ID and no longer binds the old one;
- the old and new pixels have the same format, width and height;
- the placement's geometry is unchanged: its anchor, cell box, pixel offset,
  source rectangle, drawn size, z-index, cursor policy and screen.

A retained picture occupies the binding's own image slot, and is kept only
while the terminal's image budget holds it beside every missing destination
of the record. A record that deletes the image or the placement, or changes
any of the above, retires it at once, and so does a destination that does not
fit beside it in the terminal's budget or the connection's installed-image
allowance (§7.9). Each placement is judged alone: a binding whose other
placements moved draws its retained picture only where it stood.

A sender captures the pixels of every asset a record names when it sends the
record, before their heads leave, so a program that replaces them in the host's
model has not taken them away. A newer record waits while any single-placement
picture it replaces at an unchanged geometry and size is still to leave, either
transferring or captured behind another, and is sampled once every such picture
has left; a record that removes a picture without replacing it does not wait.
Two pictures replaced together therefore both arrive before the next pair is
sampled, and a producer faster than the link drops whole frames rather than
starving one picture. Deletion, view withdrawal, and memory-pressure preemption
still retire transfers and captured pixels. Bulk chunks may be cut below their
maximum size to fit the remaining pacing credit.

Image bulk data is the decoded pixels, exactly the size `n` declared by format
and dimensions. A client that does not name `DeflateImages` is sent them raw. A
client that names it is sent one zlib stream (RFC 1950) of them, at most
`2 + n + 5 × max(1, ⌈n / 65535⌉) + 4` bytes, the length of stored blocks, and
its last chunk declares `Deflate` and `inflatedBytes = n`. The reader admits
and allocates exactly `n` bytes, with its inflater, before the first chunk, and
inflates each chunk into them as it arrives. A stream that would write past
`n`, ends short of it, has bytes past its end or past that length, fails its
Adler-32, or declares another coding or size is refused, and the connection
ends with `Bye{TransferRefused}`. The final SHA-256 is over the bytes as they
crossed, and the content hash from §6.11 is checked over the decoded pixels
before any are installed. Placement changes do not invalidate transfers for
unchanged asset identities. A discarded transfer
keeps its bounded transfer slot until final or abort, preventing a premature Want
from racing an older send; an aborted asset waits for a new offer.

Large payloads use their typed transfer head and bounded `BulkChunk` records
(§6.5), not fragments of another message grammar. Each transfer retains its
kind, pane and request ownership until verified completion or explicit retirement.

`PaneFacts` is sent to a bound pane in every view state, ahead of its
picture (§7.8). A pane's status reports have one carrier per reader: a pane
the client holds `Live` has them in its checkpoint's activity block and the
updates after it, and is sent no `StatusRecords`; a bound pane in any other
view is sent `StatusRecords`, and is owed the set again when it leaves
`Live`. A client states its view of every pane it is bound, in a `View` or in
its resume, and until it has the host sends that pane no status set: the
host starts a pane `Dormant` and cannot tell a pane the client means to hold
that way from one whose `View{Live}` is on its way. The unseen attention is in `PaneFacts` because no checkpoint carries
it. Both records are counted in the window and owed rather than queued: a
frame the window refuses is not kept, and the next opening sends the newest
revision. A status set's frames leave one a pass, each counted in the window
once the transport has taken it, and the pane's screen waits behind the set.
A client applies a record whole. Every text is `bytes`, not `utf8`:
a title, a path and a variable's value are what a program or a file system
gave, and a stray byte in one is not a reason to end a connection.

`PaneFacts.userVars` is whole, keyed by the record's `revision`, and capped as
the Core caps it (§7.9). The checkpoint ledger classifies what a record
carries into a replica as *recorded*: user variables, progress, the indicator,
the tab colour, the badge, the open document and the unseen attention. The
loopback oracle compares each. A replica keeps generations of its own for
them, moved when a record changes the member.

A host reads both records from a pane's terminal while some connection
reads the pane's facts, and computes nothing for a pane nobody reads: no
process walk, no probe it was owed, no record. A reader that arrives is read
for at once. While a pane has a reader its records are read when a frame is
published, when its foreground probe answers, when someone types, when the
size is pinned or released and when what a client shows changes, and no more
often than its foreground probe runs (50 ms a pane); a read asked for inside
that window is made when it closes. An idle pane asks for nothing. The activity stamps travel with whatever else moves the facts and
move the revision by themselves at most once a second. Attention is seen when
an attachment the host verified as a person holds the pane `Live` in a window
that has the focus, and is cleared before the facts are read; an agent's
attachment, a hidden pane and an unfocused window see nothing.

### 7.8 Ordering within one pane channel per publish

1. `HistoryReset`, when a clear opened an epoch since the pane was last
   served: before anything else
2. `PaneFacts`
3. `StatusRecords`
4. `HistoryInline` or `HistoryHead` (appends, and the open line)
5. `Update` or `CheckpointHead`
6. `Effect`
7. Bulk chunks (scheduler share)

History precedes the screen update that scrolls it, and records precede the
screen.

Across channels, two rules hold.

A workspace's structure that only moves what its panes already are (their
cells, focus, titles, zoom, who is attached) is queued after every
pane-channel `Update` or `CheckpointHead` taken before the change. On QUIC,
where the lanes are separate streams and can reorder, the leaf's
`structuralGeneration` (§7.3) is what makes the rule hold: a client never
applies a leaf's rectangle to a picture older than that generation.

A structure that adds or removes a pane is ordered against that pane's
frames, on every connection sent the workspace's structure and a pane of it:

- The revision that first names a pane is sent, whole, before any frame of
  that pane on the connection, its `PaneBind` first among them. The host
  leaves the pane unbound, and holds what it would send it, until that
  revision has left: a client slow to read is sent them in the same order,
  later.
- The revision that no longer names a pane is sent after the pane's last
  frame and its `PaneUnbind`.

So a reader that mirrors a workspace knows every pane it is sent a frame of,
and holds nothing for a pane it has not been told. A frame for a pane its
structure does not name is a fault of the host's, and the reader may end the
connection over it. A connection that only lists the host has no pane bound
and is sent structures as they change.

### 7.9 Caps (checked before any mutation; all provisional)

| Item | Cap |
|---|---|
| Any frame body | 65,535 B; at most 9 B of canonical envelope header |
| Bulk transfer | Checkpoint transfer and inflated body are bounded by `ClientPolicy::maxCheckpointBytes`, 128 MiB, equal to the host's cap below; 4 MiB history head and 64 MiB image caps are independent. |
| Host checkpoint body | 128 MiB (`HostPolicy::maxCheckpointBytes`), above twice the largest body a legal terminal produces at the largest legal geometry ([measurements](sync-measurements.md#memory-admission)). A pane past it is unbound with `PaneUnbind{CheckpointTooLarge}`, and the client tells the person why the pane went. The host binds the pane again, under a new handle, once a newer frame's checkpoint fits; it measures only after the pane publishes a newer frame and a back-off has passed, 250 ms doubling with each refusal in a row up to 30 s, so a pane hovering at the cap costs one encode per back-off. The client shows the pane again when it is bound. |
| Decoded checkpoint screen cells | 32 MiB for the two temporary screen-cell arrays (`ClientPolicy::maxDecodedScreenCellBytes`), independently of encoded size and the complete preparation allowance. Checked before preparation allocations, also on persisted restore. |
| Checkpoint preparation and destination backing | 128 MiB combined (`ClientPolicy::maxCheckpointStorageCost`): exact requested capacities for private preparation owners and decoding temporaries, plus the destination grid's backing peak, including old/new backing overlap. This per-checkpoint allowance is charged whole to the receive owner while the checkpoint is prepared. |
| Connection receive owner | 776 MiB (`ClientPolicy::maxReceiveBytes`): checkpoint, history, result and structure assemblies at their payload capacity, an inflated body before it is allocated, the checkpoint being prepared, installed image pixels and reserved destinations summed over every pane, and the catalog families. A checkpoint past it is refused by the bounded retry policy; any other transfer past it closes with `Bye{TransferRefused}` and the reason. |
| Installed images per connection | 256 MiB summed over every pane (`ClientPolicy::maxInstalledImageBytes`), inside the receive owner and beside each terminal's own image budget. An image refused room is asked for again when room frees. |
| Catalog families per connection | 72 MiB (`maxCatalogMessageBytes`): image transfers, samples, deferred effects and clipboard payloads, inside the receive owner. |
| History cache per pane | 10,000 lines and 64 MiB (`ClientPolicy::historyCacheLines`, `historyCacheBytes`); past either the oldest line goes first, on live ingest and restore alike. |
| Retained replicas per attachment | 512 MiB (`ClientPolicy::maxRetainedReplicaBytes`): every picture's grid backing and scrollback store, once per picture, ended ones included, and every history cache. A checkpoint whose grid would not fit is refused with a reason; history past it evicts the ingesting pane's oldest lines; cold restore admits all panes or none. |
| Transfers a client fenced after giving them up | 64, the oldest released first |
| History line | 16,384 cells; a longer one travels cut, flagged `Truncated` |
| Pens / links / long clusters per dictionary epoch | 65,535 / 4,096 or 1 MiB / 4,096. Overflow rolls the epoch with a checkpoint. |
| Status records per pane | 256, LRU host-side. Id 128 B (8 segments of 32 B); app 32 B; title 192 B; message 2 KiB, as the OSC parser stores them. |
| `PaneFacts` texts | Title, directory and open document 4 KiB each; home directory 1 KiB; process chain 1 KiB, 2 KiB with arguments; a program, shell, terminal or host name 255 B; indicator label 48 B; badge 256 B. Every text at its cap fits one frame. |
| Unseen notification words | 255 B each for title and body |
| Clipboard write | 1 MiB |
| Pending local input while `Lost` | 64 KiB |
| Buffered updates during a checkpoint | 1 MiB |
| Panes per connection | 256 |
| `lastWritten` entries per pane | 64, least-recently-written evicted |
| Outstanding `Call`s per attachment | 32 |
| Queued bulk bytes per attachment | 16 MiB |
| Pending `HistoryWant` ranges per attachment, summed over every priority | 64 |
| `Paste` | 32 KiB per chunk, 16 MiB per paste |
| A paste sent beyond the host's `PasteReceipt` | 256 KiB (`Welcome.pasteWindowBytes`) |
| Unwritten input per client instance per pane | 512 KiB, or 4,096 events |
| Unwritten input per connection, across its panes | 1 MiB |
| Unwritten input per pane | 64 instances' shares: 32 MiB, and its signals |
| Unwritten input per host, across its panes | 64 MiB, and each pane's signals |
| Signals a pane holds unwritten, past every other bound | 4 KiB |
| Discarded runs remembered per client instance | 8, the oldest two merged past it |
| User variables per pane | 32; name 128 B; value 1 KiB (`Terminal::userVariableCapacity`, `maxUserVarNameLength`, `maxUserVarValueLength`) |
| Persisted resume record | the checkpoint cap |

Checkpoint admission validates the canonical grammar and state digest through
`checkpointStoragePlan` before creating input-sized owners. Its fixed state-only
scratch terminal allocates fallibly and owns no grid or parser buffer. The plan
accounts for screen cells and row attributes; immutable pens and row hashes;
hyperlinks; cluster storage; sparse proportional-text slots, free IDs, and text;
rich-text definitions and placed document copies, blocks, runs, and visible
rectangles; state and activity strings, contexts, and status records; user-defined
keys; status-row storage; and the prepared owner itself. Strings include their
terminators. Overlapping component fields are charged once; summing temporaries
whose lifetimes do not overlap makes the result a conservative bound rather
than the minimum peak.

Every materialized container reserves its exact planned capacity through a
fallible API. The client reserves the destination grid before retiring prior
history, and commit rechecks its actual backing requirement. A refused cost or
allocation preserves the previous logical picture, dictionaries, and history;
the pane follows the bounded checkpoint-refusal retry policy. Persisted restore
checks every candidate picture and destination before materializing pictures,
then reserves all destinations before publishing any restored pane.

The per-checkpoint allowances exclude allocator bookkeeping, stack scratch, the
already-owned wire body, and previously retained non-grid model owners; the
wire body is charged to the receive owner instead, from its first chunk until
it has been applied. Zero in either per-checkpoint allowance admits no
checkpoint. Neither changes the wire format or replaces the independent
encoded-body limit.

Each owner is a `MemoryBudget` its holders charge themselves
(`Source/Core/Protocol/Sync/BulkSource.h`). Growth is admitted before it is
allocated; bytes another structure already holds, such as installed pixels, are
mirrored into the owner before each admission so it sees every holding. One
receive owner per connection bounds what a peer can make the client hold across
all its panes at once: 256 panes each announcing a large checkpoint fill the
owner, and the panes past it are refused and asked for again, never dropped.

The receiving host uses an independent provisional 256 MiB checkpoint allowance
(`HandoverTerminalLimits::maxCheckpointStorageCost`) for local adoption and host
handover. Its existing 64 MiB decoded-cell ceiling permits both screens at the
maximum legal geometry; substituting the client's 32 MiB/128 MiB policy would
reject valid authoritative terminals. The host's allowance covers checkpoint
preparation and the unpublished candidate's grid peak. The same exclusions and
zero-refuses rule apply. A receiver may lower this allowance without changing
the carried format.

One decode owner per receiving process, 512 MiB
(the `decodeMemory` argument of `decodeHandoverTerminal`, `decodePaneHandover` and `ReclaimTransfer::prepare`; `defaultMaxHostDecodeBytes`), bounds
every unpublished adopt, reclaim and handover candidate at once. A candidate is
charged before anything input-sized is allocated: its checkpoint preparation and
grid peak, its history store's planned capacity, and every other continuation
section at its carried length. The charge lives with the candidate until it is
published or dropped. A candidate past the owner is refused as
`AllocationRefused`, which an adopt reports as `NoMemory`.

The daemon's outbound caps derive from the same accounting
(`Source/Platform/Host/OutboundCapacity.h`): a client's pending output holds at
most the catalog allowance in its envelope beside one screen frame, and every
client's together at most four times that.

`pane.capture` requires RegionRead for every source. Its default `screen`
source returns visible rows. `last-command` selects the latest OSC 133 output
range; `rows` takes inclusive signed 32-bit `start`/`end` bounds; `tail`
takes `lines` (1–100,000), selecting extent and text in one host snapshot.
Ranges clamp to the retained buffer. A missing command or wholly outside
range returns NotFound; selection remains client-owned, not a shared host
capture source. Non-screen replies name `source`, `first`, `last`,
`totalRows`, `columns`, `pane`, `complete` and `text`. Each returned row
ends in LF. The host stops after a whole row reaches 1 MiB and reports
`complete: false` if rows were omitted. Readers must not report that text as
a complete capture. Continuous text waits issue fresh calls at no more than
four per second, stop on refusal, and never substitute a cached replica.

`pane.clear` takes a bound `pane` handle and one `command`: `buffer`,
`scrollback`, `screen`, or `previous-mark`. It requires Drive and a driving
attachment role. The host performs the same operation as a local session and
returns the accepted pane and command. Every mirror receives the ordinary
screen update and any destructive `HistoryReset`; no client clears its
replica speculatively.

`client.clipboardServing` takes only `enabled`, a boolean, and changes the
calling connection's own clipboard offer. Every live attachment with Observe
may offer or revoke its own clipboard; no parameter can select another
client or pane. The host echoes the accepted state in the Result. A client
serves reads only after the matching enable Result, revokes locally as soon
as the user disables it, and ignores stale answers after another change or
reconnect. Disabling also cancels host clipboard helpers waiting for that
source.

`file.read` takes a bound `pane` handle and a `path` of at most 65,535 UTF-8
bytes without NUL. It requires FileRead independently of Drive or Control;
the existing human/agent and presentation-role policy is unchanged. The
host confines the resolved path to the kernel-reported foreground process
working-directory subtree, never an OSC 7 claim. It opens without following
a final symlink or blocking on a FIFO, then checks the opened regular file
before reading at most 4 MiB. The connection serves at most 256 reads.
The correlated result echoes `pane` and `path` and contains `status`, `bytes`
and base64 `data`. Status is `ok`, `not-found`, `too-large`,
`outside-confinement`, `not-regular` or `denied`; every refusal has zero
bytes and empty data. An empty regular file is `ok`. Results too large for
one frame use the existing verified bulk transfer; partial bytes never
reach the file consumer. FILE_REQUEST and FILE_DATA are refused, not
alternate carriers. Native readers keep at most 16 pending calls on one
physical connection; endpoint retirement or transport teardown invalidates
their delivery, without redirecting a late reply to a replacement pane.

The bound includes every §7.3 `Call`, including `file.read`. A call is
outstanding from its arrival until its `Result` has left. The host refuses
new calls over the outstanding cap before dispatch, and an answer over the
queued-byte cap becomes `Result{busy}` (JSON-RPC error `-32005`, `busy`).
A host holds at most 1 MiB of unsent
interactive frames for an attachment and stops reading the client past it,
so a caller that writes calls and reads no answer holds the host to that
bound. `Limits` is `maxOutstandingCalls:u16
maxQueuedBulkBytes:u32 maxPendingRanges:u16`, after `Welcome.principal`. The
pen, link and long-cluster caps count what updates define past the epoch's
last checkpoint: a dictionary past one
rolls the epoch at the next frame taken, and a client refuses an update that
defines a pen past its checkpoint's pens and a cap's worth more.

A `Call` over a per-attachment cap is answered `Result{busy}`. A `HistoryWant`
whose ranges, added to the outstanding sets at the other priorities, exceed
the range cap is refused whole and silently, and leaves every outstanding set
unchanged; either way the attachment stays connected (§6.5).
`Limits` advertises these caps (§7.3).


---

## 8. Failure modes and detection

| # | Failure | Detection | Response |
|---|---|---|---|
| F1 | Client state diverges (encoder, decoder or width-table skew) | `stateDigest` mismatch on apply | `Resync{DigestMismatch, sectionHashes}` → `Checkpoint`. The journal records the generation and the first mismatching row. Diagnostic builds keep the span as a replayable fixture. |
| F2 | A frame tears despite the gate | Per-class counters, and the corpus oracle | Tighten the gate. Field reports carry `frameClass`. |
| F3 | The gate holds too long | Hold-time histogram per class. Gates: `Forced` > 1%, and p99 hold after input > one frame. | Tune `D`, quiescence and the persistent-bracket rule. |
| F4 | Lost or reordered datagram | No `Ack` covering the generation within the client's current `Ack` interval (§6.4) plus `max(2·srtt, 20 ms)` | The next superset covers it. |
| F5 | Chain base missing | `baseGeneration` differs from the held generation | Drop the update. `Resync{BaseMissing}`, with one outstanding. |
| F6 | Slow link or stalled reader | Window full, counted across every plane | `owed`, with no queue growth: updates, `PaneFacts` and `StatusRecords` are owed and effects coalesce. `Lost` after 30 s without acks while bytes are outstanding. |
| F7 | Bulk transfer blocks interactive | Interactive-lane latency histogram | Bounded by one 64 KiB frame time by construction; the bulk-lane latency test asserts it. |
| F8 | Transport drop | EOF or read error, the client liveness deadline (§6.9), QUIC idle timeout | `Lost` → resume. On QUIC the client first migrates off a dead path. The picture stays interactive and input queues, bounded. |
| F9 | Unplanned host restart (a planned one is §6.13) | `hostInstanceID` differs and `Welcome.predecessorHostInstanceID` does not name the old instance | Ended-session view, then a fresh attach. |
| F10 | Duplicate input after resume | `inputSeq ≤ lastWritten[clientInstance]` | Dropped host-side. |
| F11 | Row-hash collision | The 128-bit state digest is independent of chunking | The F1 path. Probability about 2⁻⁶⁴ per compare. |
| F12 | Hostile program grows dictionaries, status, images or clipboard | Caps before mutation | Discard the report, or roll the epoch. |
| F13 | Hostile or buggy peer frame | Validation before mutation | `Bye{Malformed}`. Nothing is applied. |
| F14 | Version skew | The peers speak different wire versions | `Bye{VersionMismatch}`; the refusal names the older end and both builds → deploy policy (D9). |
| F15 | Prediction shows wrong text or a secret | Confirmation mismatch, `secureEntry`, `foreignInput` | A refuted displayed prediction clears the overlay and resets the predictor; a refuted hidden epoch is dropped silently (§6.7). The synthetic-secret tests from `Docs/sync-input-feedback.md` gate it. |
| F16 | Cleared history survives on a client | `HistoryReset` | A mandatory purge, memory and disk. A negative history-reset test. |
| F17 | Unauthorized verb, clipboard read or configuration change | Principal and role check per JSON-RPC method; for frame kinds, the role column of the per-kind gate row (§7.2) | A method: `Result{forbidden}`, and the connection stays. A frame kind the role may not send: `Bye{Malformed}`. A negative test per verb and per gated kind. |
| F18 | Duplicate OS notifications across devices | `primary` tag | Only the primary posts. |
| F19 | Transport up, receipts stop | Client stall predicate (§6.9) | `Probe`; at the second miss, local `Lost` and an immediate resume. The indicator shows the stall. |
| F20 | Resume while the old transport is still open | A valid resume for a `Live` attachment | Swap after success; `Bye{Superseded}` to the old transport. A failed resume leaves the old transport serving. |
| F21 | Input outcome unknown after a long outage | `Welcome.resumed.inputWritten` absent | The events a connection carried are shown as in doubt and are never resent automatically; events typed while no connection carried the pane go once one does. |
| F22 | Primary unreachable | The primary is `Lost` | Primary failover; retained effects delivered on resume without reposting. |
| F23 | Old-epoch history arrives after a clear | Epoch fence | Discarded; the transfer is aborted with `BulkAbort`. |
| F24 | A client floods bulk requests | Per-attachment caps | `Result{busy}` or a refused want. Host memory stays flat. |

---

## 9. Resource model

Publish work is shared per pane: encode changed rows once and retain their hashes.
Per-attachment work compares acknowledged state and chooses the required rows;
per-connection scheduling admits bounded interactive and bulk work. Datagrams
must not advance the acknowledged baseline.

Memory is bounded independently for frames, dictionaries, checkpoints, history
wants, input, effects, image assets, and attachment state, and together by the
owners of §7.9: one receive owner per connection, one replica allowance per
client attachment, and one decode owner per receiving process. Admission
accounts for retained capacity before allocating or retaining more input; a
completed transfer stays charged while a callback borrows it. Buffering is not
permission to exceed §7.9. A refusal is typed and reaches the person as a
reason: a failed pane, an unbound pane, or a `Bye` with its detail.
A connection whose queued output passes the host's per-client mark is closed
with `Bye{OutputBacklog}` (§7.3), never left to grow and never closed silently.

Client CPU covers decode/apply, state digest verification, grapheme segmentation,
and any optional predictor. Persisted screen/history data follows the encrypted
storage and purge rules; local panes acquire no serialization step solely to
share the client API.

Measure bytes, work, staleness, and convergence on the
[public corpus](sync-measurements.md).

## 10. Conformance

Conformance requires deterministic positive and negative cases for each boundary
below. These are protocol requirements, not a release checklist. The portable
suites under `Tests/Core/Protocol/Sync/`, the corpus oracle, and host integration
fixtures provide the corresponding test seams.

### Corpus and effect isolation

- `Tests/Core/Protocol/Sync/test_sync_corpus_oracle.cpp`: the oracle asserts
  grid and history equality over the corpus.
- `Tests/Core`: two `Terminal`s in one process. A BEL on one fires only that
  one's route. The test fails while the bell is process-global.

### Coherence

- A corpus test replays a cursor-hidden redraw without 2026, split across two
  reads. It asserts that no published frame differs from both the pre-redraw
  and the post-redraw grid.
- The `Forced` share stays at or below 1% and the p99 input hold at or below
  one frame on the corpus.

### Screen state and pacing

- The oracle compares host and client state after every corpus frame.
- A decoder fuzzer.
- A mutation test: a corrupted op fires F1.
- A datagram-loss simulation: 10% loss produces no stall.
- A lane test: a `Receipt` queued behind an 8 MiB checkpoint leaves within one
  64 KiB frame time.
- A checkpoint round-trip of tab stops, G0–G3 after SO, and DECLRMM margins.

- One `Live` pane floods and another receives `Keys`. The echo appears
  within one window opening.
- A suspended reader with ten panes updating status holds at most one window
  of unacknowledged bytes.
- On a packet capture, a keystroke echo over direct TCP and over QUIC is one
  packet carrying `Receipt`, `Update` and `Cursor`.
- 1,000 concurrent `pane.capture` calls beyond the cap are answered `busy`,
  and host RSS stays flat.
- Fake clock: an idle `Live` attachment never enters `Lost`, and the host's
  probes follow the backing-off schedule. With `Facts.focused` clear, the
  same connection receives no `Probe`.
- Fake clock: after 10 s without an RTT sample, the next frame is paced
  from the floor window.
- Under `link = Metered` at an srtt of 100 ms, a token stream draws at most
  20 `Ack`s a second.
- On a shaped link, the oracle asserts `lagMicros` at or below the injected
  delay plus one frame.
- The ledger build fails if parser-pending state is unclassified.
- Record byte counts under the resource model in §9, together with the public
  corpus, encoding version, and reproducible report command.

### History

- A resize sends 0 history bytes.
- `link = Metered` pushes nothing beyond one viewport.
- On-demand fetch fills a gap.
- **Negative:** after a clear, a fetch below `clearedThrough` returns nothing
  and the cache holds no cleared line.
- **Negative:** a `HistoryReset` delivered between two chunks of an
  old-epoch transfer leaves nothing of that transfer in the cache.
- A thousand rapid wants leave at most one transfer per priority queued, and
  10,000 wants leave host RSS flat.

### Input, resume, and geometry

- A mode-flip race: DECCKM changes between a keypress and its write, and the
  host encodes under the new mode.
- A drop between write and receipt delivers exactly once.
- A drop between write and receipt, then a resume after the grace has
  expired: the PTY receives the event once.
- A client restored from a record at `inputSeq` 100, with the host at 150,
  numbers its next event 151.
- A PTY peer that echoes 30 ms after its read yields no frame with
  `inputSettled ≥ s` that lacks the echo. It fails under a write-time
  boundary.
- A resume while the old transport is open moves ownership and the primary
  tag at once. A wrong secret leaves the live transport serving. Two racing
  resumes yield one attachment and exactly-once input.
- **Negative:** a resume carrying the right secret over a connection
  authenticated as another principal is refused, and the attachment and
  any transport it holds are unchanged.
- **Negative:** after a `HistoryReset`, the persisted resume record holds no
  line of the old epoch.
- With a resolver that answers a different address for the host name, the
  client dials QUIC at the address the SSH connection reached.
- Fake clock: blackhole the path with no OS event. A keystroke takes the
  client to `Lost`, and it starts a redial within 2 s at 100 ms RTT. A
  `Dormant`-only connection receives no `Probe`.
- A `Suspending` client is `Lost` within one host loop, and geometry stays
  with it.
- A QUIC redial after an idle-timeout death resumes with no SSH process
  spawned. A replayed proof on a second connection is refused, and so is a
  proof for a `Detached` attachment.
- Closing the client's UDP socket under it migrates the connection without
  `Lost`.
- 8 MiB of paste on a 1 Mbit/s uplink, then Ctrl-C. SIGINT arrives within
  2 RTT, and the PTY received the end bracket.
- Kill and relaunch the client. The first frame drawn is the persisted
  picture, the resume answers `Diffed`, and no input is duplicated or lost.
  A replayed record is refused after one use.
- A resume naming eight panes, one `Live`, sends one diff and nothing for
  the other seven.
- Reorder a topology `Notify` ahead of its checkpoint on a shaped QUIC link.
  No frame draws a grid whose size differs from its rectangle.
- A resume after 5 idle minutes costs under 1 KB.
- Pointer motion from a second driver does not take geometry, and typing
  does.

### Attention, effects, records, and roles

- A `Dormant` pane under a flood receives zero `Update`s and exactly one
  `PaneDirty`.
- A `Dormant` pane whose theme changes receives one `PanePalette` and no
  `Update`; its client's terminal wears the new palette, and showing the
  pane again sends no palette and no checkpoint.
- Ten bells in a skipped span arrive as one `Bell{count=10}`.
- Only the primary receives `ClipboardWrite`.
- The OSC 7501 negative cases: an unknown state, over-cap reports and bad
  encoding.
- **Negative:** a viewer is refused every topology verb.
- **Negative:** a recorded stream naming `~/.ssh/id_ed25519` through a
  graphics-protocol `t=f` transmission produces no client file access and no frame carrying a path.
- With the primary `Lost`, one notification is posted once, by the `Live`
  driver, and the resuming primary does not repost it.
- The loopback oracle compares user variables.
- **Negative:** `principal` appears in the `attachments` notify only when
  the recipient is an `Owner`, and then on every entry.

### Placement neutrality and prediction

- One conformance suite runs against `LocalPaneLink`, `RemotePaneLink` and
  `LoopbackPaneLink`. Each refuses raw input with no bytes, and tells a
  `Dormant` or `RecordsOnly` view of no frame and a view `Live` again of the
  newest.
- The synthetic-secret prediction tests: nothing is predicted with `ECHO` off
  or while `foreignInput` is set, and a secret typed after an unmodelled key
  is never displayed.
- Fake clock: Up-arrow then typing shows no prediction until a fresh
  confirmation.
- Typing over identical text never confirms.
- Fake clock: at an srtt of 5 ms nothing is exposed. After a 400 ms stall the
  overlay is exposed at 250 ms.
- Fake clock: frames arrive and receipts stop. The connection state shows
  "input not delivered" and `linkStalled`; a received `Probe` clears "no
  contact" and leaves the stall; a receipt clears it.

### Host handover

- Hand over a pane running a margined full-screen editor (DECSTBM). The
  oracle grids match, and a client resumes `Diffed` with 0 history bytes,
  over SSH and over QUIC with a proof bound to the predecessor's
  `hostInstanceID`.
- Input sent across the handover is written once.
- **Negative:** a handover body newer than the reading build is refused, and
  the daemon that holds the panes keeps serving them.

## 11. Reserved: log mode

Sync builds no client parser. The grammar and the Core reserve what one
requires, so adding it does not change the frame grammar:

- `Greet` and `Welcome` carry `engineFingerprint`, which already refuses a
  peer of another build (§7.3, "Versions"), so a log mode's matched-fingerprint
  rule needs no field of its own.
- The checkpoint carries the full parse-affecting state at ground: modes on
  both screens, live charsets, margins, tab stops, the image LRU, and the 2026
  open flag with its clock. A Core can continue parsing from a checkpoint of
  class `Boundary` or `Settled`, both taken at ground, and a log mode starts
  only from such a checkpoint. A `Flood` or `Forced` frame may be taken in any
  parser state (§6.1), and in-progress parser state (a pending CSI, OSC or DCS,
  or a partial UTF-8 sequence) is classified host-only in the checkpoint ledger,
  so no checkpoint carries it.

  `parserLedger` (`CheckpointLedger.h`) classifies every
  `Terminal::Parser` member as pending, ground or policy, and the build fails
  when one is missing or when a pending member is carried. The open grapheme
  cluster is classified pending, as a partial UTF-8 sequence is. The REP record
  is the parser's ground state; it changes with each character printed, so it
  travels in a checkpoint-only `parserGround` section after the history rather
  than in the text-state block updates resend. A `Boundary` or `Settled` frame
  is taken with `parseState` at ground; the UTF-8 decoder may still hold a
  partial sequence then, which a log mode must close before it starts.
- The per-`Terminal` effect routing, reply-disposition chokepoint and
  clock hook are the first entries of a determinism ledger.

A log mode is one more `PaneLink` delivery on the same grammar: it forwards PTY
bytes and recorded host facts, applies only to matched-fingerprint clients on
`Unmetered` links, and falls back to updates on skew. Its admission gate is a
differential replay test: the corpus, split at random chunk boundaries,
produces digests equal to the host's at every fact. A missed non-byte input
invalidates that mode because it can silently produce a wrong screen; clients
must fall back to authoritative state updates when equivalence is not established.

---

## 12. Decisions

- **D1. Base model.** State goes down. The host Core is the only emulator; the
  replica path is reserved as §11 describes.
- **D2. Coherence gate.** The redraw-bracket and read-quiescence gate applies to
  local and remote frontends alike, and must meet the corpus bounds (`Forced` at or below 1%, p99 input hold at or below one frame).
- **D3. Multi-client size.** Geometry follows the hands. The owner is the most
  recent *typing* driver, never pointer motion. Owner changes apply at once, the
  owner's own resizes coalesce over 50 ms, and `pane.pinGeometry` freezes the
  size.
- **D4. Prediction.** On by default, with every rule below in force:
  - it runs over tentative epochs, and an epoch is shown only once a frame
    with `inputSettled` past it confirms one of its predictions (§6.7);
  - it is displayed only when the measured echo latency crosses the show
    threshold, or a prediction has been pending past the glitch threshold;
  - it is vetoed by `secureEntry`, outside `atPrompt` and on the alternate
    screen. The wire carries no raw-mode fact; the `atPrompt` veto is what
    keeps prediction out of raw-mode programs.

  It is controlled by a setting. A raw-mode arm is a separate decision
  (D18).
- **D5. History on metered or constrained links.** Pull-first: push one viewport
  above the screen and fetch the rest on scroll or search, with `pane.search`
  running host-side. On `Unmetered`, backfill newest-first to 2,000 lines.
- **D6. On-disk history cache for remote panes.** Off by default. Opt-in,
  encrypted at rest with a key held in the platform keychain and scoped to
  `(hostInstanceID, principal)`. The purge-on-clear rule applies either way.
  The same key encrypts the resume record where the client seals it, whose
  rules §6.9 states.
  `remote.history_cache_on_disk = true` enables it. History is part of the
  same authenticated, atomic record, keyed by host, pane and epoch. Encoded
  record bytes and decoded history payloads have independent bounds; oldest
  whole lines are omitted first. A policy change retires protected records
  before it is accepted. A failed retirement must withhold the old view and
  input, not acknowledge a clear or silently retain readable history.
- **D7. Program status.** The Core parses OSC 7501 as a program-facing surface
  carried on the wire's own `StatusRecords` frame. Terminfo advertises it only
  when the OSC 7501 contract and negative cases are satisfied.
- **D8. Local panes.** Local panes stay in-process. `PaneLink` keeps their
  placement a separate decision, and the local frame path gains no
  serialisation step without a measured renderer budget ([performance rules](../AGENTS.md#implementation-conventions)).
- **D9. Version skew.** Each peer states the `wireVersion` it speaks and its
  `wireFingerprint`, and peers that differ in either are refused at the
  handshake (§7.3, "Versions"). A newer client
  upgrades the older host through the deploy policy
  (`Source/Core/Session/`), and an older client is told it is the older end
  and to update the app. `wireVersion` changes only when the grammar or the
  width table changes, never on a Core change. A host upgrade hands live panes
  to the new daemon (D17), or the old daemon keeps serving them, so a version
  change never strands a running session.
- **D10. Hash widths.** XXH3-64 row hashes with an XXH3-128 state digest. The
  digest covers the collision case (F11); 128-bit row hashes would cost 8 B more
  per row per attachment.
- **D11. Agent subscription.** `RecordsOnly` for `clientKind = Agent`, with
  screen text through `pane.capture` and history through `pane.search`. An
  agent may opt into `Live` explicitly.
- **D12. Notification and clipboard primary.** The primary is the geometry
  owner if it is not `Lost`, otherwise the most recent typing human driver that
  is `Live`. Only it posts OS notifications and receives `ClipboardWrite`.
  Other human clients badge. Effects retained across `Lost` keep their original
  primary; consumed sequence receipts prevent a resuming primary from posting
  twice, including an interrupted batch whose final acknowledgement was not sent.
- **D13. Multi-driver arbitration.** Input from several drivers interleaves in
  arrival order, surfaced through `foreignInput` and gated by roles. There is
  no typing lease.
- **D14. Unicode widths on the wire.** The client derives widths from the
  Core's table, pinned by `wireVersion`. Explicit per-row `widths` travel only
  for overrides and OSC 66. The digest catches skew.
- **D15. Constants.** These are provisional:
  - `D`, quiescence, the persistent-bracket timeout, grace, the receipt delay,
    the bulk share and the `Ack` cadence bounds;
  - the echo timer `E`;
  - the idle probe schedule, the stall multiplier and its 750 ms floor;
  - the prediction show, hide, flag and glitch thresholds, and the
    unconditional flag after 5 s pending;
  - the metered `Ack` interval bounds and the re-path timer;
  - every cap, the per-attachment caps of §7.9 included.

  Each must have a public workload measurement or an explicit provisional
  rationale; the measurement reference records those distinctions.
- **D16. Session continuation.** A resume may continue an SSH-authorised
  attachment over QUIC, without SSH, by the MAC of `resumeSecret` bound to the
  connection's TLS exporter (§6.9). The proof is single-use, bound to one
  connection, and valid only for a `Live` or `Lost` attachment under the
  pinned certificate. Every new principal, pane and shell still goes through
  SSH.
- **D17. Host handover.** A host upgrade hands live panes to the new daemon
  (§6.13). Its versioned body passes only between daemons on one host, and a
  successor reads only the body version it writes; it is not another
  remote-wire version. This preserves sessions across a breaking `wireVersion`
  change (D9) while the body version holds; a holder whose body a successor
  refuses keeps serving its sessions. An unplanned crash ends the session (F9).
- **D18. Measurement-gated options.** Raw-mode prediction and per-frame
  deflate require their measured thresholds to be met.
  - **Raw-mode prediction.** It predicts when `secureEntry` is the only
    refusal, ICANON is off, the cursor is visible, and the echo timer and
    epoch rules are in force; `EchoContext.flags` gains `Canonical`. It is permitted
    only if the agent-TUI corpus entry shows a useful display share with zero
    wrong displays. Without it, the felt-latency floor in raw mode is one RTT
    plus the coherence hold (§6.7).
  - **Per-frame deflate.** Bit 7 of `kind` marks a deflated body on `Update`,
    `HistoryInline` and `Result` above 512 B; the deflated form is kept only
    when smaller, and caps apply to the inflated size. It requires a
    median saving of at least 25% on agent-TUI redraws.

  A measurement below the threshold must not enable the corresponding option.

### Native drag ownership

OSC 72 state belongs to the host terminal. A native gesture or drop arrives
from its current human driver, on that pane's bound channel and current owner
epoch. The client must advertise `ClientCapability::NativeDrag` (bit 4). Viewers and
agents cannot supply native drag events. An owner change
revokes active drags, reads and directory handles before replaying registration.
A handover carries registration only; an active drag blocks handover.

Native drag messages use the bulk lane. Each payload is at most 32,768 bytes;
read completions contain at most 3,072 decoded bytes. Metadata stores the OSC
command type, a nine-bit presence mask, present coordinates as signed 64-bit
values restricted to the signed/unsigned 32-bit union, and optional 32-bit
identity and operation fields. Mask bits 0–5 select x, y, X, Y, identity and
operation; bits 6–8 record explicit type, explicit chunking and continuation.
Event flag bits 0 and 1 select final completion and same-window origin.

The native client owns pasteboard objects and file descriptors. Terminal output
can request files only through the current drop's URI list or directory handles.
Directory children are opened relative to retained directory descriptors, with
symlinks returned as link text. Received remote files occupy a private directory;
entry names cannot escape it and symlinks cannot act as parents. Closing a drag
releases its handles and unfinished files. The AppKit frontend maps these
operations to pasteboard data, file promises and native dragging sessions.
