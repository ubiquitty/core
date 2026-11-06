<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# tmux control mode (`tmux -CC`) — code map

Ubiquitty projects each tmux window into a native tab and its pane tree into Swift terminal surfaces. The control connection owns the PTY; projected panes route input and resizing through that connection.

## Ownership and routing

- `Source/Core/Protocol/Tmux/Tmux.h` and `Tmux.cpp` parse the control protocol.
- `Source/Core/Protocol/Tmux/Session.h` owns the discovered window and pane tree.
- `Source/Core/Runtime/TmuxControlHost.h` and `TmuxControlHost.cpp` coordinate the control connection, discovery, output, and layout notifications.
- `Source/Platform/macOS/App/Windowing/TerminalWindowController+HostedWindows.swift` opens control connections and projects their windows.
- `Source/Platform/macOS/App/Windowing/TerminalWindowController+TmuxHosting.swift` binds each surface to the pane's terminal and content lock. Moving a pane transfers its existing surface; replacing its terminal creates a fresh adopting surface. Unbinding ends adoption before Core frees the model.

`TerminalSurface.makeAdopting` uses the shared surface initializer, which seeds local cursor preferences under the model lock. A pane does not acquire an independent PTY or ingest loop.

## Geometry

`TerminalWindowController.reportTmuxClientGrid` measures the whole window through `Rendering::fitGridToViewport` and reports it through `TerminalSession::reportTmuxClientSize`. It runs on content-frame changes and layout application. Reporting one pane's tile as the client size would shrink every window in the session to that tile.

Rendering, hit testing, and grid fitting use the renderer's cell metrics and grid insets. Padding, pane chrome, and progress indicators must therefore reduce the same usable viewport. Resizes reach tmux as client-size and pane-resize commands; tmux remains authoritative for hosted layout and output.

## Validation

`Source/Platform/macOS/UITest/Swift/TmuxHostingScenarios.swift` drives real control connections, pane/window creation, resize, font changes, detach, and reattachment. `TabSurvivorScenarios.swift` covers native tab and pane transfers; `ConsentPromptScenarios.swift` covers clipboard attribution for hosted panes. Portable protocol and runtime tests remain under `Tests/Core/`.
