<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Embedding a host workspace

Include `UbiquittyKit/UbiquittyKit.h` and link `Ubiquitty::ubiquitty-kit`
from `find_package(UbiquittyCore)`. No renderer, window system, source-tree
include path or private transport header is needed.

`UbiquittyKit::Workspace` owns the replicated terminals and the shared carrier
for a host workspace. The host remains the authority for pane identities,
layout, permissions and terminal state. A second carrier watches its directory.
The C++ facade is versioned as Kit 1.5; it lives in Core, independently of the
graphical `ubiquitty-kit` repository.

```cpp
auto factory = UbiquittyKit::makeHostChannelFactory({
    .hostBinary = "/path/to/ubiquitty-host",
    .sshDestination = "user@host",
});
if (!factory)
    return; // report factory.error() in the embedding application's UI
UbiquittyKit::Workspace workspace(std::move(dispatcher), std::move(*factory), observer);
workspace.start({ .workspaceID = 42 }, 80, 24);
```

The application supplies the dispatcher and observer. Both carrier callbacks
and public calls run on that dispatcher's thread. The workspace owns the
dispatcher and factory; the observer outlives it. After draining dispatched
work, call `synchronize()` to reconcile the host's topology outside decoder
callbacks. `tree()` exposes the current workspace, `directory()` the watched
host directory, and `terminal(sessionID)` a borrowed replicated terminal.
Borrowed terminals remain valid until the next `synchronize()` or `stop()`.

The frontend chooses visible panes and their viewport sizes with `setView`.
Zero geometry makes a pane dormant without detaching it. Render from Core's
public screen API and `Rendering::resolveCell`; the latter resolves palettes,
reverse video and hidden-text colors consistently with other renderers.

Send exact input with `sendInput(sessionID, bytes)`; embedded NULs are preserved.
Send layout operations with `sendIntent(sessionID, intent)`. The host validates
the operation; `Observer::onIntentResult` reports its answer. The facade does
not choose key bindings, draw tabs, confirm destructive actions, or replay
uncertain input. Those are embedding-application decisions.

`state()`, `error()` and `hasEnded()` distinguish a dropped carrier, a failed
connection and an ended workspace. Core reconnects its carrier; destroying or
stopping the client leaves the host's sessions running. A new instance can
reattach using `socketName()` and a `Target` with a session or workspace ID.

The POSIX host factory starts an explicitly selected host daemon when no
socket was supplied. It pins reconnects to the acknowledged daemon. Remote
connections use noninteractive SSH; installation and authentication remain
explicit. Custom transports can supply their own `Session::ChannelFactory`.

`Tests/UbiquittyKit/test_kit_workspace.cpp` exercises the facade against the
real Sync screen plane, including binary input, topology reconciliation,
borrowed terminal lifetime and factory validation.
