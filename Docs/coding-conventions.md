<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Ubiquitty Coding Conventions

The authoritative, deep reference for how Ubiquitty C++ is written, and the
standard the conventions gate reviews every PR against.

Every rule here is one of:

- **[WebKit]** — rules derived from WebKit house style. Source linked in §14.
- **[Core Guidelines]** — a C++ Core Guidelines rule, cited by ID.
- **[CERT]** — a SEI CERT secure-coding rule.
- **[Project]** — a Ubiquitty-specific rule, often *stricter* than upstream.
  Where we tighten an upstream rule, this is stated plainly with the reason, so
  nobody mistakes a local choice for a WebKit mandate.

Normative words: **MUST** / **MUST NOT** are hard gates (a PR is blocked).
**SHOULD** is the default; deviating needs a one-line justification in the PR.
**MAY** is allowed.

Each section anchors to a real file in the tree as the worked example. Read the
anchor before writing new code in that area — the existing well-tested module is
the precedent.

---

## 1. Language baseline & build constraints

- **C++23.** Use the language standard configured by the build, including
  `std::expected`, `std::span`, concepts, and ranges (§8). Toolchain
  requirements belong in the build guide.
- **must build with `-fno-exceptions` and `-fno-rtti`.** This is retained WebKit
  style — the Core stays exception-free across every backend. No `throw`, no
  `try`/`catch`, no `dynamic_cast`, no `typeid`. Error handling is by return
  value (§6).
- **The Core is portable and freestanding-of-platform.** `Source/Core`,
  `Source/Core/Runtime`, `Source/Core/Rendering`, `Source/Core/Protocol`, … contain no AppKit,
  no Metal, no sockets, no `#ifdef __APPLE__` business logic. Platform lives
  behind the PAL seam (`Source/Platform/`). A Core file that needs a platform
  type is a layering bug — drive it through the PAL. The companion rule for
  *behavior* vs *presentation* — program-observable = compatibility (Core),
  human-visible = design (frontend) — is the observe/see boundary in
  `Docs/architecture.md` §1.
- **STL is allowed throughout** (`std::array`, `std::vector`, `std::string`,
  `std::string_view`, `std::optional`, `std::span`, `std::expected`).
  `<iostream>` and `<print>` stay **out** of the Core — bloat, and the Core does
  no stream I/O. Hot paths still prefer fixed-size buffers over per-cell /
  per-byte allocation (that is the performance rule, not a ban on the STL).
- **Zero warnings on every arm [Project].** Finished work compiles without a
  single warning in every language and on every arm it touches: macOS, iOS,
  Linux, the host, the UI-test bundles and the benches. C and C++ build under
  `-Werror`; Swift builds under `-warnings-as-errors`, set once in
  `Scripts/cmake/UbiquittyToolchain.cmake` beside `UBIQUITTY_WERROR`, which
  every build includes at its top directory, so every Swift target and the
  generated Xcode projects inherit it. Fix a warning at its
  cause: replace a deprecated API with its supported successor, resolve the
  concurrency or unused-result diagnostic, and do not reach for
  `-suppress-warnings`, `@available` shims, or `-Wno-*` to make it disappear.
  `-DUBIQUITTY_WERROR=OFF` exists only to unblock a toolchain bump whose new
  diagnostics land in code that cannot be touched yet; it is never the state
  of finished work.
- **No speculative compatibility scaffolding [Project].** Replace obsolete
  contracts completely instead of retaining parallel APIs, decoders, aliases,
  or consent paths for unsupported formats. Compatibility behavior must serve
  an explicit supported contract and have tests. A version mismatch must not
  silently reinterpret input as another format.

---

## 2. Naming

WebKit CamelCase is the base. The table is the quick reference; the notes carry
the nuances a reviewer actually trips on.

| Entity | Rule | Example |
|---|---|---|
| Class / struct / enum / namespace | `UpperCamelCase` **[WebKit]** | `PaintSession`, `CellGrid`, `ScreenMode` |
| Function / method | `lowerCamelCase` **[WebKit]** | `scrollRegion`, `cellMatchesPen` |
| Local / parameter | `lowerCamelCase` **[WebKit]** | `rowIndex`, `tokenLength` |
| Instance data member | `m_name` **[WebKit]** | `m_rows`, `m_bbShadow` |
| Static data member | `s_name` **[WebKit]** | `s_instance` |
| File-scope / namespace constant | `lowerCamelCase`, **no `k`** **[Project/WebKit]** | `attributeRunBreakMask`, `csiHandlers` |
| Enum member | `UpperCamelCase` **[WebKit]** | `ScreenMode::Alternate` |
| Macro / `#define` constant | `ALL_CAPS_WITH_UNDERSCORES` **[WebKit]** | `UBIQUITTY_PERF_DIAG` |

### 2.1 Constants carry no `k` prefix — and that *is* WebKit

WebKit prescribes **no** prefix for constants; its only constant rule is "prefer
`const` to `#define`." The `k` prefix (`kFooBar`) is a Carbon / Chromium /
Apple-Foundation convention, **not** WebKit. We name a constant like any other
variable: `lowerCamelCase`. Re-introducing a `k` prefix fails the gate.

### 2.2 Acronyms are all-caps — but position decides the first letter [WebKit]

WebKit capitalizes *all* letters of an acronym, with one twist by position:

- In a **type / namespace** name, the whole acronym is uppercase:
  `HTMLDocument`, `URLParser`, `OSCState` — never `HtmlDocument`.
- In a **variable / function** name, a **leading** acronym is **lower-cased in
  full** to satisfy the lowercase-first-letter rule: `mimeType` (not
  `MIMEType`), `csiHandlers` (not `cSIHandlers` or `CSIHandlers`), `oscDispatch`.
  A **non-leading** acronym stays all-caps: `parseHTML`, `defaultURL`.

Spell acronyms all-caps, never InterCaps: `UTF8` not `Utf8`, `inputUTF8` not
`inputUtf8`. `Scripts/check-acronyms.py` enforces this and is the pre-commit
hook.

### 2.3 Spell words out — [Project], stricter than WebKit

WebKit says "use full words, *except* where an abbreviation is more canonical
and easier to understand" — it endorses `tabIndex` over `tabulationIndex`.
**Ubiquitty goes stricter:** spell words out in full. `foregroundIndex` not
`fgIdx`; `inputLength` not `inLen`; `outputBuffer` not `outBuf`; `codePoint` not
`cp`; `connection` not `conn`. Why we tighten it: three frontends and a headless
host read this Core; an unfamiliar abbreviation costs every new reader, and a
hard rule needs no per-word judgement call. Common expansions: `Idx → Index`,
`Len → Length`, `Buf → Buffer`, `Pos → Position`, `Params → Parameters`,
`Ctx → Context`, `Num → Number`, `Str → String`, `Attr → Attribute`,
`Src → Source`, `Dst → Destination`, `Tmp → Temporary`, `Msg → Message`,
`Err → Error`, `Cmd → Command`, `Fmt → Format`, `Val → Value`,
`Hdr → Header`, `fg → foreground`, `bg → background`, and an `Id` suffix is the
acronym `ID`. `Scripts/check-acronyms.py` enforces them and ends every report
with the full table.

**Semantic renames.** Use `Scripts/refactor.py`, not textual substitution.
Clangd resolves each symbol and rewrites its references; identically spelled
members and locals may be distinct symbols. Review the dry run before using
`--apply`. A rename must update every affected declaration, reference, and
language boundary.

Additional examples:

- `Spec → Specification`, `Pref → Preference`, `Cb → Callback`,
  `Async → Asynchronous`, `Coord → Coordinate`, `Util → Utility`,
  `Impl → Implementation`, `Dir → Directory`, `Cfg → Configuration`,
  `Mgr → Manager`, `Dim → Dimension`, and `Bot → Bottom` when those
  tokens abbreviate the stated word.
- `Ch` means `CodePoint` for a Unicode scalar value, otherwise
  `Character`. `Char` and `Charset` expand to `Character` and
  `CharacterSet` in project-owned names.
- Spell out `Capacity`, `Capability`, and `Certificate`; `Cap` is
  ambiguous. Whole words such as `capture`, `lineCap`, and `pin` remain.
- Protocol `ACK`, `ID` / `IDs`, `SHA256`, and `UDK` follow acronym
  casing (§2.2).
- Use `xOffset` / `yOffset` for offsets and `deltaX` / `deltaY` for
  deltas, not bare `dx` / `dy`.

**Exceptions [vendor / external contract]:** keep the upstream spelling — Apple /
vendor symbols (`OSStatus`, `CGContext`, `CTFont`, `kCFAllocatorDefault`,
`NSURLSessionConfiguration`, fontconfig `FcConfig`, CMake `PkgConfig`), ngtcp2's
`get_conn` / `user_data` / `scid` / `dcid` / `cid` (in transport context), the
WebKit smart-pointer suffixes `Ptr` / `Ref`, terminal protocol terms of art
(`sixel`, `tmux`, `DA1` / `DA2` / `DSR` / `DECRQSS`), and `Source/ThirdParty/`
wholesale. Also exempt the **user-facing** config file/path strings
(`config.toml`, `~/.config/`, `$XDG_CONFIG_HOME`, `.editorconfig`) — an external
CLI/XDG contract, not an identifier we own. The `char` C keyword and the vendor
spellings `UniChar` / `CChar` / `UTF16Char` (Apple) and `FcChar8` (fontconfig)
keep their names for the same reason — the language and the libraries own them,
not us. `Init` is left alone (it collides with `Initial`/`Initialize`). Never
re-spell a name the platform, a vendored library, or an external contract owns.

### 2.3.1 One word per concept, for bounds [Project]

Use one name for each kind of bound. The following terms have distinct meanings.

| word | means | example |
| --- | --- | --- |
| `max<Word>` / `min<Word>` | the most or least allowed, however it was arrived at | `maxCells`, `minColumns` |
| `<word>Capacity` | how much a container has room for | `glyphCapacity` |
| `<word>Limit` | a bound someone **configured**, or a region edge | `scrollbackLimit`, `topLimit` |
| `<word>Threshold` | a point where behaviour **changes** | `confirmMultilinePasteThreshold` |
| `<word>Budget` | an allowance that **depletes** as it is spent | `drainQuietSpinBudget` |
| `<word>Bounds` | geometry — a rect or an extent | `fontBoundingBox` |

`max` and `min` are the permitted abbreviations for value bounds and go
at the beginning of a name. Use `maxRows`, not `rowsMax` or
`rowsMaximum`. Do not use `Ceiling` or abbreviated `Cap` for a value
bound; choose the term from the table that states its meaning.

An Apple property keeps Apple's spelling — `screen.maximumFramesPerSecond` is not
ours to rename. Write the receiver so the rule can see whose name it is.

### 2.4 No type information encoded in names [Core Guidelines NL.5]

No Hungarian tags. The type is the compiler's job; the name states purpose. If a
variable's type changes, its name should not have to.

> Anchor: `Source/Core/CellGrid.h` (members `m_rows`, `m_bbShadow`, method
> `scrollRegion`), `Source/Core/VT/VT.cpp` (`csiHandlers`, `actionHandlers`).

---

### 2.5 Booleans read as an assertion [WebKit]

A boolean — variable, member, return, or the value behind a getter — reads as a
yes/no assertion. The predicate **LEADS**: `isVisible`, `hasFocus`, `shouldWrap`,
`didLoad`, `canScroll`, `wantsLayer`, `usesGPU`, `allowsSelection` — or the name
is a boolean **adjective**: `valid`, `empty`, `active`, `dirty`, `ready`. A bare
noun (`bool mode`, `bool result`, `bool state`) does not; nor does a subject-led
`xIsY` — `clientIsAgent` / `foregroundIsShell` should be `isAgentClient` /
`isForegroundShell`, the `is`/`has` at the front. Getters drop the `get`:
`isEnabled()`, not `getEnabled()`.

**Boolean arguments are a `enum class`, never a bare `bool`.** A `bool` at a call
site is unreadable — `setResizable(true)` says nothing; `setResizable(Resizable::Yes)`
does. Even two cases get a named enum. `Scripts/check-enums.sh` blocks a new bool
parameter; annotate a genuine boolean-DATA param with `// bool-ok: <reason>`.

This is a **C++ rule**. In Swift, an argument label names the value
at every call site — `findMatches(query, isRegex: true)` reads unambiguously, and
Apple's own APIs use it (`animated: Bool`). A labeled Swift `Bool` parameter is
therefore fine and idiomatic; `check-enums.sh` does not flag it. The predicate-led
**naming** rule (§2.5) still applies to Swift boolean properties
(`var isLoading: Bool`, not `var loading: Bool`) — only the no-boolean-*arguments*
gate is C++-only.

### 2.6 Naming review

Names must follow the rules above and state their role consistently with
neighboring APIs. The lints catch mechanical violations; review checks meaning,
units, and ownership. Keep semantic renames scoped and compiler-verified rather
than adding unrelated churn to a behavioral change.

---

## 3. Comments

A comment states an invariant, a precondition, or a reason the code cannot
express. It must not narrate work history or repeat the code.

This is WebKit's "comments explain why, not what" sharpened to a gate. Grounded
in [Core Guidelines] NL.1, NL.2 and NL.3: a comment states intent, says nothing
the code already says, and stays crisp.

### 3.1 Write

- The **invariant** a reader cannot recover from the code: "cells have exactly
  one owner; dirty is computed from `cells_ ≠ m_bbShadow`, never stored."
- The **precondition / contract**: "caller holds the model lock", "`offset <=
  length` before this point."
- The **why** of a non-obvious choice: "copy into an owned buffer here — the
  ngtcp2 buffer is freed when the callback returns."
- **File-level docs** at the top of a non-trivial public header (see the anchor).

### 3.2 Do not write

- **Temporal / change narration.** No "just added", "used to use X, now Y",
  "see the PR", no dates as prose, no "(new)". The present-tense invariant is
  the only thing worth writing; history lives in `git blame`. A temporal comment
  becomes a lie at the next edit.
- **No internal issue/PR/ticket numbers — in any form.** Not `see the ticket`, not a
  parenthetical tag (`// FileReadAccess (ticket): …`), not an inline phase tag,
  not a fixes marker. A ticket number anchors code to a development moment, not an
  invariant, and goes stale the instant the tracker moves — it is temporal by
  definition. State what the code *does*; the ticket lives in `git blame` and the
  PR. The only `#`-citations allowed are **external standards** (`UAX #29`,
  `UTS #51`, `RFC 1951`, `UAX #15 §18`, ECMA-48 / DEC spec sections) — stable
  references, not history.
- **Restatement.** `i++; // increment i` adds nothing, drifts, and
  trains readers to ignore comments (NL.1's exact failure mode). Delete it.
- **Verbose padding.** Use a clause where a clause is enough (NL.3).

### 3.3 Emphasis is markdown, not capitals

Comments are prose, so format them as prose: backticks for identifiers and
literals, `*italics*` for emphasis, a `-` list when there are genuinely several
points. Reserve **bold** for a rule a reader must not miss.

Never all-caps a word for emphasis. Capitals read as shouting, and once a file
has three of them none of them mean anything — the emphasis inverts into noise.

Use markup accepted by DocC so declaration comments can serve as reference
documentation.

```cpp
// GOOD:
// The *template* is tokenised on whitespace; the *match* never is, so `;` and
// `$(…)` in matched output stay data.

// BAD — shouting:
// The TEMPLATE is tokenised on whitespace; the MATCH never is. Past the cap
// rows are still SWALLOWED and must not be parsed as pane records.
```

### 3.4 Declaration comments are DocC

A comment on a declaration is reference documentation, so it follows [DocC]
markup and the [Apple Style Guide]. Swift and C++ share the shape, and the Swift
surface publishes from it directly.

```cpp
/// Parses one already-assembled line and appends the record it names.
///
/// Blanks, comments, and lines whose digest is not 64 characters are skipped,
/// so a damaged line costs its own record and nothing else.
///
/// - Parameters:
///   - entry: One line from the store, with or without its line terminator.
///   - records: Receives the parsed record when the line names a valid one.
inline void parseTokenLine(std::string entry, std::vector<TokenRecord>& records);
```

- `///` on the declaration; file-level `//` blocks keep their form.
- Summary is one sentence, third person, ends with a period: "Parses one line",
  not "Parse one line" or "This function parses a line".
- Blank `///` line, then discussion, then the parameter and return blocks last.
- Single backticks for symbols; double backticks for DocC symbol links in Swift.
- Present tense, active voice, sentence case, serial comma.
- No "simply", "just", "easy", "obviously" — they judge the reader. "App", not
  "application".

[DocC]: https://www.swift.org/documentation/docc/
[Apple Style Guide]: https://support.apple.com/en-gb/guide/applestyleguide/welcome/web

---

### 3.5 The one nuance: design rationale is *not* temporal

Explain the bug class a design prevents without recounting its development.
For example, a single authoritative cell store prevents independently maintained
arrays from drifting. That rationale remains useful when the implementation
changes; a dated account of the edit does not.

```cpp
// GOOD — invariant the code can't state:
// Sorted by final byte with no duplicates; the dispatch jump table is built from it.
static constexpr CSIEntry csiHandlers[] = { ... };

// BAD — restates the code:
i += 2; // add two to i

// BAD — temporal:
// Changed from memcmp to digestsEqual on <date> for <ticket>.
```

> Anchor: `Source/Core/CellGrid.h` (file-level "why this exists"),
> `Source/Core/Protocol/Sync/WireFormat.h` (the `Reader` latch-on-failure contract).

---

## 4. Memory, ownership & lifetime (Safe C++)

The Core is **not** broadly reference-counted; it is mostly single-owner value
and arena types. The ownership tools, in order of preference:

1. **Value / scoped object** [Core Guidelines R.5] — a local, member, or
   `constexpr` global. No heap, no lifetime question. Default to this.
2. **`std::unique_ptr`** [Core Guidelines R.20/R.21] — unique heap ownership.
   Prefer over `shared_ptr` unless ownership is genuinely shared: simpler,
   deterministic destruction, no atomic refcount. `QUICConnection` ownership
   uses `std::unique_ptr<QUICConnection>`.
3. **`CheckedPtr<T>` / `CanMakeCheckedPtr`** — the in-repo WebKit-style
   non-owning *checked* pointer. The target counts its referents and **ends the
   process if any remain at destruction**, in every configuration rather than
   only where assertions run: the ordering it catches happens on teardown paths
   a debug build rarely walks. Use it where the target must outlive the
   reference, so a violated ordering stops at the destruction that violated it
   rather than at the unrelated later read.
4. **`WeakPtr<T>` / `CanMakeWeakPtr`** — the same shape for the opposite
   contract. The referent may legitimately outlive the target and **reads as
   null once the target is gone**. Use it for work handed to something that
   outlives the call — a dispatcher, a timer, a factory completion — where the
   owner going away is an ordinary event and the work must then do nothing.
   Derive plainly (`class Thing : public CanMakeWeakPtr`); a destructor that
   has handed out references calls `revokeWeakPtrs()` before tearing down the
   fields that work would touch. A reference may be taken and released on any
   thread, and is read on the thread that destroys the target.
5. **`Ref<T>` / `RefPtr<T>`** [WebKit] — only where an object is genuinely
   reference-counted. `Ref` = non-null, `RefPtr` = nullable.
6. **Raw `T*` / `T&`** — **non-owning, always** [Core Guidelines R.3]. A raw
   pointer never owns. A reference is the non-null form; a pointer is the
   nullable form.

### 4.1 Hard rules

- **MUST NOT** use raw owning pointers. If a class allocates, a handle frees it.
  [Core Guidelines R.10/R.11: avoid `malloc`/`free`, avoid explicit
  `new`/`delete`.]
- **MUST NOT** store a raw `T&`/`T*` across a boundary where the target can die
  — across a callback return, an `async` hop, or a coroutine suspension. On a
  network-decode path this is the canonical remote use-after-free: copy
  attacker-adjacent bytes into an owned buffer at the callback boundary and parse
  from the copy.
- **SHOULD** hold a smart pointer on the **stack** while calling a non-trivial
  member function on a heap object whose lifetime the call could end [WebKit].
- **MUST** mark a never-reassigned smart-pointer member `const` [WebKit] — it
  states intent and helps the analyzer.
- **MUST NOT** capture raw `this` in a lambda that outlives the current scope
  [WebKit]. Capture a protecting/weak handle instead.
- **No `protected` data members** [Project] — protected accessors are fine;
  protected data drifts the same way cross-module cell access does.

### 4.2 Span over (pointer, length) [Core Guidelines F.24]

Return / accept a bounds-knowing view, never a separate pointer and count that a
caller can mismatch. Use `std::span<const T>` / `std::string_view` at API
boundaries, or the in-repo `RowView { const Cell* data; int count; }`.

### 4.3 The `Terminal` construction contract [Project]

`Ubiquitty::Terminal` is a constructed, non-copyable C++ object. Every owner uses
normal C++ lifetime (`Terminal` by value or `std::unique_ptr<Terminal>`); allocating
it with `calloc`, clearing it with `memset`, or bypassing its destructor is undefined
behavior. The type stays non-polymorphic, owns resources through RAII, and keeps its
per-byte parser state directly accessible. Callback seams use concrete, trivially
copyable function-pointer records rather than `std::function` or virtual dispatch.

> Anchor: `Source/Base/CheckedPtr.h` and `Source/Base/WeakPtr.h` (the two
> non-owning pointer tools),
> `Source/Core/RowView.h` (span-over-ptr+len), `Samples/core-consumer/main.cpp`
> (the documented constructed-`Terminal` contract).

---

## 5. Mutable globals & concurrency

### 5.1 No mutable globals — forbidden [Core Guidelines I.2, Project]

A mutable global / file-scope variable **MUST NOT** exist. This process has
PTY-read, render, decode-queue, and main threads; a shared writable global is a
data-race and a hidden dependency [I.2; CP.2 "avoid data races"; CP.3 "minimize
explicit sharing of writable data"]. Allowed:

- `const` / `constexpr` immutable globals (`lowerCamelCase`, **no `g` prefix** —
  that's GLib/Chromium, not WebKit).
- State that outlives a function is **owned by an object** and reached through it.
- A genuine process-singleton hides behind a thread-safe `singleton()` accessor
  that owns its own synchronization — never a bare mutable `g`-variable.

### 5.2 Concurrency model

- **Terminal model access has one synchronization owner.** The parser and
  model do not assume a UI thread or dispatcher. Runtime and frontend adapters
  coordinate threads and hold the required lock before model access.
- **Encapsulation must not add a lock or atomic-RMW to a per-cell / per-frame /
  per-byte hot path** without a measured justification. Prefer thread-local or
  per-instance ownership over a shared-and-locked singleton. When a lock is
  unavoidable on a hot path, a before/after measurement
  goes through the performance gate.
- **macOS off-main ingest:** `Ubiquitty::processBytes` runs on the
  pane's `org.ubiquitty.ingest` queue holding the pane's model lock, not on main.
  Every PAL hook or callback Core fires while parsing — the OSC color, clipboard
  and cwd handlers, the window-op forwarders, notifications — therefore runs on
  the ingest thread:
  - AppKit is main-thread-only. A Core-reachable callback that touches
    `NSWindow`, `NSView`, `NSAppearance`, `NSPasteboard`, the titlebar, menus or
    window geometry marshals to the main actor. Swift window and surface
    controllers isolate these operations with `@MainActor`.
  - Renderer mutable state (the palette, the default and cursor colors) is read
    on the render path, so it is mutated on main through the same hop.
  - A hook's pointer arguments point into Core-owned transient memory; copy
    them before the hop.
  - A synchronous getter whose reply Core needs inline (an OSC color query) may
    read a benign few-byte snapshot off-main, never AppKit. It never
    `dispatch_sync`s to main: the hook runs under the model lock, main may be
    waiting on that lock, and holding it across a hop to main deadlocks the two.
  - Prefer pull over push: write a plain field off-main (`Terminal::titleUTF8`)
    and sample it from a main-thread tick.
  - A violation crashes with `EXC_BREAKPOINT` and "Must only be used from the
    main thread" on `Dispatch queue: org.ubiquitty.ingest`.

> Anchor: `Docs/architecture.md` §11 *Threading Model*; the daemon's
> single-threaded `poll()` reactor in `Source/Platform/Host/Daemon.cpp`.

---

## 6. Error handling without exceptions

`-fno-exceptions` means fallibility is expressed in the return type, not thrown.

- **New fallible API: return `std::expected<T, E>`** (C++23) with an explicit
  error enum `E`. **MUST** be `[[nodiscard]]` so a caller cannot silently drop
  the error.
- **Established Core idiom:** a bounded reader that **latches a failure flag**
  and returns zeros after the first short read, so a malformed buffer surfaces as
  one `ok == false` rather than an out-of-bounds access; callers that must stop
  consuming check `ok` between reads. (`std::expected` is preferred for *new*
  fallible surfaces; the latch pattern stays where streaming decode needs it.)
- `std::optional<T>` for "value or nothing" where there is no error to carry.
- **MUST NOT** return a partially-initialized output alongside an unchecked
  error. An unread error that leads to use of partial state is a memory-safety
  bug, not a style nit.

> Anchor: `Source/Core/Protocol/Sync/WireFormat.h` (`Reader`, latch-on-failure).

### 6.1 Allocation failure: two tiers, both refuse [Project]

Under `-fno-exceptions` a throwing `new` that fails is `std::terminate`: there is
no null to check, and a `!ptr` guard after `std::make_unique` is dead code that
reads as reassurance. Which allocations may take the process down is decided
here, not per site.

1. **Bounded parse-path allocations** — anything sized by input bytes (an OSC or
   DCS payload buffer, a clipboard reply, a reflow arena, a decoded image).
   `new (std::nothrow)` behind a hard cap; a null **refuses the unit of work**
   (the byte is dropped, the reply is not sent, the resize keeps its geometry)
   and the session carries on.
2. **Structural per-session allocations** — the `Session` record, the
   `Terminal`, the `CellGrid` buffers. Every one is size-capped (`maxRows` ×
   `maxColumns`, `maxSessions`), so the failure is a memory-pressure event, not
   a bug, and the unit that refuses is **the session**: `new (std::nothrow)`,
   and a null returns a failure the caller reports (`createSession` → null → the
   HELLO client is dropped; a refused grow leaves the grid it had).

Neither tier aborts. The daemon hosts every tenant in one process, so an abort on one tenant's
grid takes every other tenant's PTY with it, while a refusal costs that one
tenant one session or one resize.

What that leaves on the default `operator new`: allocations whose size is a
constant of the program rather than of a tenant — a fixed-size object header,
a container's first node. A `Terminal` must own a `CellGrid` to be a
`Terminal`, so the grid *object* is constructed with it; the grid's *buffers*
are the sized part and are tier 2. A dead null-check after a throwing
allocation is a finding: either the allocation moves to `nothrow` and the
check becomes live, or the check goes.

A refusal is tested by injecting it: the nothrow `operator new` is replaceable,
and a test binary that replaces it can refuse on demand
(`Tests/Core/test_grid_allocation_refusal.cpp`, `Tests/Host/create_session_refusal.cpp`).

> Anchor: `Source/Core/CellGrid.cpp` (`allocBuffers` / `ensureCapacity`: tier 2),
> `Source/Core/VT/VT.cpp` (`reserveOSCBuffer`: tier 1),
> `Source/Platform/Host/Daemon.cpp` (`createSession`: the session refuses).

---

## 7. Integer, bounds & arithmetic safety

Attacker- and wire-controlled lengths are the highest-stakes arithmetic in the
codebase. [CERT INT30-C: unsigned wraps modulo 2^N; INT32-C: signed overflow is
undefined behavior — the optimizer may delete a naive `if (x+1 < x)` guard.]

- **must overflow-check arithmetic on any length/offset/count derived from
  input, *before* the operation, not after.** Rearrange to subtraction so the
  check itself cannot wrap:
  - bounds check: prove `offset <= length` first, then test `n > length - offset`
    — **never** `offset + n > length` (the add can wrap and pass the very input
    it should reject). Order matters.
  - addition: `if (max - a < b) error;` before `a + b`.
  - allocation size `itemSize * count`: reject `count > MAX / itemSize` (or use a
    checked product) before allocating — a wrapped multiply yields a too-small
    buffer and a full-size write (the classic heap overflow).
- **SHOULD** centralize the check in a checked type (WebKit `WTF::Checked<T>` /
  `safeAdd`/`safeMultiply`, or C23 `ckd_add`/`ckd_mul`) rather than hand-rolled
  `INT_MAX` tests at every call site — the missed-guard gap is what produced real
  allocation-overflow CVEs.
- **MUST** parse wire/byte input through a **bounded reader over a span** (§4.2):
  a cursor plus `remaining()`, every read range-checked, error on shortfall.
- **MUST NOT** do unbounded scans on untrusted input.
- Avoid lossy narrowing conversions [Core Guidelines ES.46]; name the cast and
  the intent when a conversion is real (§11).

> Anchor: `Source/Core/Protocol/Sync/WireFormat.h` (`Reader::need` / `bytes`),
> `Source/Platform/Host/TokenStore.h` (`digestsEqual`, constant-time compare —
> §13.1).

---

## 8. Modern C++23 idioms

Reach for these; they are baseline, not exotic.

- **`enum class` with a fixed underlying type** [Core Guidelines Enum.3] for
  every fixed value set — `enum class ScreenMode : uint8_t { Primary, Alternate
  }`. Emit the wire/underlying value with **`std::to_underlying`**, never a raw
  `static_cast`-and-hope. Prefer an `enum class` over a `bool` parameter
  (no boolean-trap): `setAlternate(ScreenMode)`, not `setAlternate(bool)`.
- **`std::span` / `std::string_view`** at boundaries (§4.2). `std::mdspan`
  (C++23) is the zero-overhead multi-dim view for grid/row access where it fits.
- **`std::expected`** for fallible returns (§6).
- **`std::bit_cast`** for type-punning (over `reinterpret_cast` / `memcpy` of a
  bit pattern); **`std::byteswap`** for endian conversion on the wire.
- **`constexpr` / `consteval`** for compile-time data — the sorted dispatch
  tables with `static_assert` sortedness are the model [Core Guidelines Con.5].
- **Designated initializers**, **structured bindings**, **`if`/`switch` with
  initializer** where they sharpen intent.
- **`[[nodiscard]]`** on any function whose result must be checked (fallible
  returns, `get()` accessors, factory functions).
- **Ranges / views**: use where they clarify and the cost is off the hot path.
  On a per-cell / per-byte loop, measure — a view pipeline can de-optimize versus
  a hand loop; the performance gate decides.
- **Deducing this** (explicit object parameter) to deduplicate const/non-const
  overloads where it genuinely removes duplication.
- **Not in the Core:** `<iostream>`, `std::print`/`std::println` (§1).

> Anchor: `Source/Core/VT/VT.cpp` + `VT/Constants.h` (`std::to_underlying`,
> `ConformanceLevel`), the many `enum class … : uint8_t` across `Source/`.

---

## 9. Const-correctness & types

- **By default make objects `const`** [Core Guidelines Con.1] and **member
  functions `const`** [Con.2]; drop `const` only when state actually changes.
- Prefer **strong types** over primitives where a unit or meaning matters
  (a column is not an arbitrary `int`).
- **References for non-null** parameters (`const Cell&`); **pointers for
  nullable** (`Terminal* term = nullptr`).
- Test booleans / null / zero **without** equality comparisons [WebKit]:
  `if (condition)`, not `if (condition == true)`.

---

## 10. Headers, includes & module layering

- **`#pragma once`**, never include guards [WebKit].
- **Forward-declare** to keep header dependencies thin; `Source/Forward.h` holds
  the shared forward declarations.
- **Include order** in a `.cpp`: the file's own header first, then project
  headers, then system headers; sorted within each group.
- **Module boundary:** a module owns its public header(s) and exposes only what
  others need. Cross-module access goes through the public header — never reach
  into another module's internals via extern-redeclaration. No bidirectional
  includes: if `A` includes `B`, `B` must not include `A` (break the cycle with a
  forward declaration or a third module). The Core→Rendering cycle break
  and the `CellGrid`/`PaintSession` single-mutator API are the canonical cases.
- **No forked shared logic across modules [Project].** A non-trivial algorithm — a
  cache/eviction policy, a parser, a state machine, an open-addressing hash — lives in
  one place and is reused: extract it to the owning module (`Source/Core/Rendering/`,
  `Source/Core/`, …) and parameterize it (a comparator, a verify hook, a policy) rather
  than copy it into a second translation unit. A second copy is a structure defect — a
  fix to one silently skips the other. Where a genuine constraint blocks reuse (a
  hot-path cost the performance gate weighs, a semantic the shared type cannot yet
  express — e.g. a variable-length key needing a memcmp-verify the unique-key cache
  lacks), the fork is allowed only as an **explicit, cross-referenced, tracked
  exception**: the two copies name each other in-comment and a filed follow-up item
  carries the unification, so the duplication is visible and owned, never silent.
- **Public C-ABI surface** consumed by C and C++ TUs wraps declarations in
  `extern "C"` (gated on `__cplusplus`); class declarations stay outside it.

> Anchor: `Source/Core/CellGrid.h` (file-level doc + single-mutator API),
> `Source/Forward.h`.

---

## 11. Formatting

`Scripts/format-code.sh` runs clang-format in **WebKit** style; run it on touched
files (or hand-match). The rules a reviewer still eyeballs:

- 4-space indent, no tabs. Spaces around binary/ternary operators; none around
  unary. A space between a control keyword and its `(`; none inside call parens.
- **Braces:** function *definitions* get the brace on its own line; every other
  block puts the open brace on the preceding line. Always brace control bodies
  except a single-line clause with no comment. Empty braces contain a space:
  `{ }`.
- **Pointer/reference declarator binds to the type** (C++): `Terminal* term`, not
  `Terminal *term`.
- **`nullptr`** for the null pointer (never `0`/`NULL` in C++).
- C-style headers as `<cstring>` / `<cstdio>`; the `std::memcpy` / `std::memset`
  names follow.

---

## 12. Functions & control flow

- **Early return** is encouraged [Core Guidelines NR.2 — the single-return rule
  is a myth]; guard clauses beat nested `if`s.
- **Declare variables at first use with an initializer** [Core Guidelines NR.1],
  not in a block at the top of the function — fewer uninitialized-variable bugs.
- **No C-style casts** [Core Guidelines ES.48/ES.49]. Use `static_cast` for
  numeric/pointer conversions, `const_cast` only against a legacy non-const API,
  `reinterpret_cast` / `std::bit_cast` for punning with a one-line comment naming
  the punned pair. C casts hide intent and break grep.
- **Replace magic constants with named symbolic constants** [Core Guidelines
  ES.45] — and where a set of flags is involved, an `enum class` + `OptionSet`,
  not a bare integer.
- **Debug-only bounds asserts at every cell-grid index site** [Project] —
  `assert()` is a no-op under `-DNDEBUG`; every index into the grid or a row /
  column from a public API gets one.

---

## 13. The conventions gate

What a conventions review checks, and the contract around it.

### 13.1 Review requirements

- Every rule above, against the **diff**, not only the CI summary. The reviewer reads the
  change and verifies completeness independently (a rename is verified by
  confirming the old names are *gone* and the new ones *resolve*, which is the
  missed-reference check a compile does).
- **Lifetime / ownership** on every attacker-reachable decode path — a UAF or
  OOB there is a remote-trigger primitive, not a nit. This overlaps the security
  gate by design: same lines, two readers.
- **Structure / organization / DRY** — module placement, layering (§10), and
  duplication are the conventions read even when the same lines are a hot path or a
  decode surface. Overlap with the performance / rendering / security gate means two
  readers on one diff, not a transfer of responsibility. Resolve a structural
  finding or document an approved,
  in-code-cross-referenced exception tracked by a filed follow-up item (§10, no
  forked shared logic) — never a silent second copy, never a floating "non-blocking"
  note. A performance-justified fork requires a recorded exception and an owner.
- **Comment discipline** (§3) — work-history and redundant comments block.
- **Constant-time compare for secrets** [CERT]: a token / secret comparison must
  use the constant-time `digestsEqual` (XOR-accumulate over all bytes, no
  first-mismatch early-out), never `==` / `memcmp` / `std::equal` that
  short-circuits and leaks the match position through timing. `digestsEqual`
  early-outs only on a length mismatch, so compare fixed-length secrets or
  fixed-size digests — then the length itself reveals nothing.
- **Test coverage [Project] — a hard merge gate.** Every behavior
  change ships a host unit test **in the same PR**, and the test **fails without
  the change** ("the existing test still passes" is not coverage of new
  behavior). A security / trust behavior change ships a **negative** test — the
  unauthorized case is *rejected* — because a missing test is a missing defense
  (the adopt-authz pattern). The only exemption is a change with no
  host-testable surface (AppKit/Swift UI, a bundled asset, pure visual/motion):
  it must **say so explicitly** in the PR and route to the maintainer's in-app
  verify. An unstated "no test" blocks; it does not pass.

### 13.2 The merge contract

- Changes to Sync, Transport, Daemon, HostQUIC, descriptor passing, MCP, or
  snapshot serialization require conventions review and the relevant domain
  review: security for trust boundaries, performance for hot paths, and terminal
  review for VT or snapshot semantics. Follow the contributor guide for pull
  requests and merge requirements.
- Each commit **MUST** build, link, and pass tests on its own. No `WIP` commits in
  `main`; rebase / squash before merge.
- **One behavior per commit.** A commit either *adds* (new files plus their tests —
  inert until wired) or *changes* (edits to shipping code); never both, and a pure
  deletion rides alone. An addition-commit that also touches working code couples the
  scaffold's fate to that edit: a regression forces reverting unrelated new work with
  it, and bisect lands on the whole commit instead of the guilty hunk. Land the
  foundation first; wire-up is its own commit.
- Work in an isolated branch or worktree, and preserve unrelated changes in the
  checkout.
- **A commit that removes a recently landed line names the commit it undoes and
  why.** Taking back someone else's recent work is a decision, and the message is
  where it is recorded: the abbreviated hash (seven digits or more) or the subject
  of each commit undone, and the reason. `Scripts/check-stale-landing.py` blames
  every line a commit removes at the commit's parent and fails a commit that,
  in at least one file, does nothing but restore the text a commit from the last
  14 days replaced, when that commit is not the branch's own and is not named.
  It runs in the commit-msg hook (on each step of a rebase too) and in
  `check-style.py` over the branch.
- **Every PR is rebased onto the current main immediately before merge**, and
  conflicts are resolved hunk by hunk, never by taking a whole file from either
  side. Rebasing alone does not undo anything; a conflict resolved by taking the
  branch's whole file does, silently, and it reads as the branch's own change.
  The merge goes through `Scripts/merge-pr.sh <number>`, which refuses a head that
  is not on the current tip, runs the stale-landing check over the PR's commits,
  posts the verdict as the `stale-landing` commit status, and only then merges.

### 13.3 The deterministic gates (`Scripts/check-*`)

The mechanical arm of the rules above. A noisy or missing gate is a dead gate, so
conventions keeps these sharp and fixes their false positives fast, and never lets one
weaken to pass a PR. `check-style.py` is the aggregator that runs them, over the whole
tree or over one branch's files with `--since origin/main`; `check-merge-ready.sh`
validates the branch itself — rebased, clean, indexed — rather than the gate set.

A gate is Python, subclasses `Gate` from `Scripts/gate_framework.py`, and writes
`examine()`. The framework owns the control flow, the levels a gate may speak at
(ok, skipped, advisory, failed, plus debug and info for detail) and the timing:
every run records a start and a finish carrying its elapsed seconds to the sink
named by `UBQ_GATE_LOG`. Durations stay out of a gate's own output because
`gate_verdict.py` caches that output and replays it, and a replayed duration
would be a measurement of a run that did not happen.

`Scripts/gate_report.py` fixes the wording of the verdict line itself, and
`Scripts/gate-report-lib.sh` is the same format for the gates still written in
shell. Those two are asserted byte-identical by `Tests/test-gate-report.sh` — a
format expressed by two implementations must stay identical.

| Script | Enforces |
|---|---|
| `check-acronyms.py` | All-caps acronyms (§2.2) and spelled-out abbreviations (§2.3), with full-tree and staged added-token checks. |
| `check-enums.sh` | No bare `bool` parameter — a named `enum class` instead (§2.5). |
| `check-macos-smokes.py` | Every headless mode the macOS app dispatches, derived from the dispatch. A skip names the app-source files the branch changed that then went unexamined. |
| `check-comments.sh` | Comment hygiene (§3): no work history, restatement, or internal ticket references. |
| `check-style.py` | Whitespace + conflict-marker hygiene, `clang-format` drift (WebKit, via `format-code.sh`), shell syntax, and the `Cap → Capacity/Capability` allowlist. |
| `check-boundaries.py` | Module dependency / layering rules under `Source/` — the portable Core stays free of Platform, no cross-module reach-in, no include cycles (§1, §10). |
| `check-capability-ssot.py` | The one-enum capability spine — a single source of truth for the `Capability` set. |
| `check-docs.py` | Documentation hygiene for Markdown + TOML. |
| `check-copy.py` | Interface text follows the writing rules in [Naming conventions](naming-conventions.md#writing): Apple's for the macOS and iOS apps, GNOME's for Linux, one lexicon for both. |
| `check-coverage-floor.sh` | Host test coverage stays at/above the documented per-file floors (§13.1). |
| `check-licenses.py` | Project license scope, and the third-party inventory (`Docs/third-party-inventory.json`) ↔ notices (`third-party-notices.md`) coverage, discovery, pins, and risks. |
| `check-personal-paths.py` | Public-tree privacy — no personal / local absolute paths leak into the tree. |
| `check-merge-ready.sh` | The branch's own readiness: rebased onto main, no uncommitted changes, index current. |

**Removing a rule.** Prefer types or API boundaries that make invalid states
inexpressible. Remove a gate only when equivalent enforcement remains and a
negative fixture proves the invalid operation is rejected. A clean scan alone
does not prove that a rule is unnecessary.

| Rule | Appropriate enforcement |
| --- | --- |
| A borrowed argument must not be null | A reference or other non-null type |
| A target must outlive its borrowers | Checked lifetime tools and teardown tests |
| Deferred work may outlive its target | Weak lifetime tools and cancellation tests |
| A measured byte view must not become an unbounded C string | Bounded APIs, source checks, and malformed-input tests |
| Identifier spelling | Naming lint |
| Comment content | Documentation lint and review |
| Bare Boolean parameters | Named enums and signature checks |
| Module layering | Include/dependency checks |

**Adding a rule.** A rule must have positive and negative fixtures. Where an
existing tree cannot satisfy a new whole-file rule, use a narrowly defined
added-token check so unrelated edits remain possible. Expand enforcement only
with the corresponding source cleanup. Do not weaken a rule to pass a change.

---

## 14. References (primary sources)

- WebKit Code Style Guidelines — <https://webkit.org/code-style-guidelines/>
- WebKit Safer-CPP Guidelines (smart pointers, lifetime) —
  <https://github.com/WebKit/WebKit/wiki/Safer-CPP-Guidelines>
- WebKit `WTF::Checked` / `CheckedArithmetic.h` (overflow-safe arithmetic) —
  `Source/WTF/wtf/CheckedArithmetic.h` in <https://github.com/WebKit/WebKit>
- C++ Core Guidelines — <https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines>
  (canonical raw: <https://github.com/isocpp/CppCoreGuidelines/blob/master/CppCoreGuidelines.md>).
  Cited rules: I.2, F.24, ES.45/46/48/49/103/104, R.1/3/5/10/11/20/21, Enum.3,
  Con.1/2/5, CP.2/3, NL.1/2/3/5/8/16, NR.1/2.
- GSL intro (span over pointer+length) —
  <https://github.com/isocpp/CppCoreGuidelines/blob/master/docs/gsl-intro.md>
- SEI CERT C — INT30-C (unsigned wrap) and INT32-C (signed overflow) —
  <https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/integers-int/int30-c/>
  and `.../int32-c/`
- C++23 feature list — <https://en.cppreference.com/w/cpp/23>

In-repo precedents to imitate: `Source/Core/CellGrid.h` (module shape),
`Source/Base/CheckedPtr.h` (Safe-C++ tool), `Source/Core/RowView.h` (span),
`Source/Core/Protocol/Sync/WireFormat.h` (bounded reader),
`Source/Platform/Host/TokenStore.h` (constant-time compare).
