<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Inspector and process-aware titles

The inspector presents facts and actions for one selected terminal session.
Process discovery belongs to the machine running that session; the frontend
must not infer a remote process tree from its own operating system.

## Process facts

`PAL::Stream::process()` exposes a `ProcessFacts` facet. Its
`gatherProcesses(std::span<PAL::Process::Record>)` method fills portable records
and returns the number written, bounded by the supplied span.

Platform code enumerates raw process identifiers and gathers names, arguments,
users, terminal names, and run states. Core's
`Session::Process::gatherSubtree` admits the session leader and descendants
whose parent chain reaches it. A missing parent, cycle, or exhausted traversal
bound does not admit an unrelated process. Membership is based on ancestry,
not merely a shared terminal name or process group.

A record is foreground when its process-group ID matches the session's
positive foreground group. The session leader is identified separately.
`PAL::Process::foregroundLeafProcessID` chooses the deepest foreground
process; `formatChain` supplies its concise process-chain label.

The process sample is session-owned. Inspector observation enables the session's
process updates, and closing or hiding the inspector relinquishes that
observation. Frontends consume `SessionInfo` rather than querying a process
table from a view callback.

## Native inspector

`InspectorController` hosts the shared macOS panel. It follows the active
terminal window and retargets away from a closing window, clearing its target
and hiding when no eligible terminal remains. The inspector identity can
distinguish an owning session from a selected pane. Identity changes must
discard stale selection and editing context.

`InspectorTarget` supplies the information and allowed actions.
`SurfaceInspectorTarget` adapts a terminal surface and window controller to
that interface. The model exposes titles, geometry, background, font, theme,
reported locations, launch details, and process rows. Process rows carry stable
process IDs for selection, with foreground and stopped/zombie presentation
derived from the shared sample.

Actions go through the target: resize, title edits, appearance changes, directory
clearing, and process signals. They must report refusal or a stale target
without mutating another pane. Signalling requires the selected process to
appear in the owning session's process records with a valid process group;
remote and externally supplied terminals refuse this local action.

## Titles

`Title::resolveTitle` takes independently enabled segments and produces named
parts. These include the configured base title, directory, application title,
process chain, shell command, profile, terminal name, dimensions, and program
status. Empty or disabled segments are omitted. `joinTitle` provides the
canonical joined form; a native titlebar may style named parts without parsing
punctuation out of a finished string.

Directory and process facts are resolved by their owners before title
composition. When a session ends, the process chain, shell command, and program
status disappear rather than naming a process that is no longer running.
Titles and inspector rows use the same session facts.

### Custom titles

A window and its tab each carry one custom title, set or automatic; empty is
automatic, so a cleared field and one never typed in are the same fact
(`Session::CustomTitles`, `Source/Core/Session/CustomTitles.h`). Every typed
title is kept as `Session::normalizedCustomTitle` keeps it: trimmed, free of
control characters, bounded on a character boundary, so a field of spaces
clears.

Where a title is held is its owner's. A local tab's is the workspace model's
(`Workspace::renameTab`), which the arrangement capture and the workspace
presentation read too. A hosted tab's is the host's: asked over Sync as
`tab.rename`, held once the host's layout states it, and read from that
layout. A window's title, and the tab title of a window no model owns (a tmux
window), are the frontend's, since no model carries them; the macOS shell
saves both with the window's presentation and restores them from it. The
macOS shell reads both through `customWindowTitle` and `customTabTitle` and
writes both through `setCustomTitles(window:tab:)`; the title popover, File ▸
Rename Tab…, the inspector, the scripting bridge, the control socket's
`set-title`, session capture and restore and the remote-workspace reconcile
all go through those, and none keeps a copy.

One composition rule, `Session::resolveTitleNames`: a custom title fills the
app-title segment, the part that names what runs there, and the process
segments beside it are dropped; the directory, the columns×rows readout, the
program's status, the host chip and the tab's marks stay as the surface's
`[title]` composition has them. A tab answers to its own title, else to its
window's; a window never answers to its tab's. A pane answers to neither: its
label replaces its header. A program's later title lands in the pane's facts
and moves nothing a person named.

## References

- `Source/PAL/Process.h` and `Source/PAL/Stream.h`: portable process facets.
- `Source/Core/Session/ProcessSubtree.h`: ancestry admission.
- `Source/Core/Title/Format.h`: title parts and composition.
- `Source/Core/Session/CustomTitles.h` and `Source/Core/Session/ProcessState.h`: the custom titles and the rule that ranks them.
- `Source/Platform/macOS/App/Windowing/InspectorTarget.swift`: frontend boundary.
- `Source/Platform/macOS/App/Windowing/TerminalInspector.swift`: native model and view.
