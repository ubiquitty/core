<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Program activity and tab attention (OSC 3008, 7501, 21337, 9, 99, 777)

Ubiquitty shows small indicators on **inactive** tabs so an app can signal that a
session wants attention without stealing focus. There are two kinds, driven by
different OSC sequences:

1. a **persistent colored status dot** the app sets and clears explicitly
   (OSC 21337), and
2. a one-shot **desktop notification** that also drops a transient blue dot on
   the background tab (OSC 9 / 99 / 777).

Both are recognised by Core (`Source/Core/OSC/Dispatch.cpp`); the macOS frontend renders
them through `TabAccessoryPresenter` and Core's `App::tabAccessory` rule.

## OSC 7501 — program status

[Program Status Protocol](https://www.superlogical.com/rex/docs/build/program-status)
reports replace a record atomically. Ubiquitty supports `idle`, `working`, `done`,
`blocked`, `error`, and `clear`, hierarchical `id` paths, inherited `app` names,
base64 UTF-8 `title` and `msg`, blocked `kind`, and optional `progress`.

```sh
printf '\033]7501;state=working:app=build:progress=40\033\\'
printf '\033]7501;state=blocked:kind=permission:msg=Q29udGludWU/\033\\'
printf '\033]7501;state=done:app=build\033\\'
printf '\033]7501;state=clear\033\\'
```

`OSC 7501;? ST` replies with `OSC 7501;? ST`; terminfo advertises `Pst`.
Both BEL and ESC-backslash terminate reports. At most 256 records are retained;
the least recently updated record is evicted when necessary. Reports are bounded
by 4096 bytes including framing (the payload limit reserves a two-byte ST), with
2048 decoded message bytes, 192 decoded title bytes, and eight ID levels. Invalid
base64, UTF-8, controls, or excessive fields discard the whole report.

Working and blocked records expire at a shell prompt or process exit. Done and
error records survive those events. RIS clears every status; DECSTR and screen
switches preserve them. Clearing an ID removes its descendants even if the parent
has no record.

The primary status drives the tab indicator and title-status segment:
error, blocked, done, working, then idle, with the most recent report breaking ties.
OSC 7501 takes precedence over OSC 21337 while records exist. Display text removes
directional and invisible formatting controls and is shortened for tab labels;
the model retains the full report. Desktop tab presentation does not generate
a notification. A hosted pane's unresolved question, permission, or authentication
block is also retained in the passive catch-up inbox for phones that opt into
[background refresh](ios-interface.md#background-catch-up).

On macOS a window's title carries the status after the title, set off with
the process tree's ` ▸ ` (`Title::chainArrowText`): the status's symbol in the
theme's colour for it, then what the program says it is doing, its record's
title and message whole and in regular weight (`App::programActivityText`).
A window with a tab bar or split panes leaves the symbol to its tabs and pane
titles, and keeps the words, which a tab is too narrow for. A pane's title bar
carries the same mark as a background tab, at its trailing end: the symbol,
the ellipsis for working, the ring for progress, glowing and nudging until the
pane has been seen, focused in its selected tab. The focused pane leaves
working and progress to its bar.

### Process icons

A program a title knows is drawn by its icon before its name in window, tab
and pane titles, 16 points as a browser draws a site's icon, after any tag
icon: Claude Code, Codex, opencode, pi, Gemini CLI, Grok Build, Prime Agent,
Deep Agents and Kimi Code. A pane's program is the one its most urgent OSC
7501 record's `app` names, or failing that its foreground process
(`Title::processIconFor`), so Claude Code is found as `claude-code` by its
report and as `claude` by its process; once the shell has the terminal back,
a record its program left behind names no one. A name matches whatever its
case, and with a Windows `.exe` left off. A one-colour icon is drawn in the
title's ink, so it reads on any theme like the words beside it; one in its
owner's colours keeps them, and is set on a plate in the title's ink where its
colour measures under 3:1 against the title's ground
(`App::processIconNeedsPlate`), unless it brings a tile of its own.
`[appearance] process_icons = false` (Settings > Window Titles > Process
Icons) draws the names alone.

`Resources/process-icons/catalog.json` gives each program's names and where
its icon comes from; `Scripts/import-process-icons.py` fetches the icons,
keeps only their drawing, and regenerates
`Source/Core/Title/ProcessIconCatalog.h`, so every frontend matches the same
names. Adding a program is an entry there and a run of the script. The icons
are their owners' trademarks; see `Resources/process-icons/NOTICE.md`.

## OSC 3008 — hierarchical contexts

[UAPI.15](https://github.com/uapi-group/specifications/blob/main/specs/osc_context.md)
contexts are stored per terminal in ancestor order. `start=id` pushes a context;
restarting an existing ID replaces its metadata and ends its descendants.
`end=id` closes that context and its descendants. The most recently explicitly
ended context retains its exit metadata for consumers.

```sh
printf '\033]3008;start=shell-1;type=shell;user=alice;cwd=/tmp\033\\'
printf '\033]3008;start=job-1;type=command;cmdline=make test\033\\'
printf '\033]3008;end=job-1;exit=failure;status=2\033\\'
```

All defined metadata fields are supported. Semicolons and backslashes in values
use `\x3b` and `\x5c`. Invalid or unknown fields are ignored individually.
Storage is limited to 64 active contexts, 64-byte identifiers, 255-byte metadata
values, and 16 KiB command bodies. Terminal resets preserve the stack; session
hangup clears it. Contexts are available to model consumers; they do not change
working-directory authority, execute commands, or recolor terminal output.

Both protocols honor the per-terminal OSC allow/deny lists and can be carried
through tmux passthrough. Sync checkpoints retain contexts and program-status
records; `PaneFacts` and `StatusRecords` also publish pane metadata independently
of changed grid rows. A status-only update must notify readers without relying
on a cell diff. Reattachment must restore the same bounded state.

## OSC 21337 — tab-status indicator (the colored dot)

```
OSC 21337 ; indicator=#RRGGBB ST     set the dot to #RRGGBB
OSC 21337 ; indicator ST             clear it (bare key, or indicator=)
OSC 21337 ; status=… ; status-color=… ; indicator=#RRGGBB ST
```

`indicator=#RRGGBB` sets a persistent dot in the app's chosen color on the
session's tab while it is in the background; a bare `indicator` (or `indicator=`)
clears it and its label. `status` supplies a UTF-8 label, retained up to
`OSC::tabStatusTextCapacity` (48 bytes) at a code-point boundary, alongside a
valid indicator. A report with no valid indicator changes nothing.
`status-color` and `detail` are ignored; the indicator supplies the colour.
Values may contain `\;` and `\\` escapes. ST is `BEL` or `ESC \`.

```sh
printf '\033]21337;indicator=#5f87ff\007'   # a blue "waiting" dot
printf '\033]21337;indicator=#ff9500\007'   # busy / orange
printf '\033]21337;indicator\007'           # clear
```

OSC 21337 is iTerm2's session status sequence, defined by iTerm2's
[Session Status](https://iterm2.com/documentation-session-status.html)
documentation. Its terminal state is the bounded label
and colour plus `tabStatusGeneration`; a remote reader receives the
corresponding pane facts. The private OSC number is an interoperability
identifier, not an authority claim by the sending program.

## OSC 9 / 99 / 777 — desktop notifications

The following notification formats share the terminal's notification route:

```
OSC 9 ; <text> ST                         iTerm2 / Windows Terminal
OSC 99 ; <metadata> ; <body> ST           kitty desktop-notification protocol
OSC 777 ; notify ; <title> [; <body>] ST  urxvt notify module
```

Each fires the notification hook (`ShellClient::postNotification`), which posts a
real OS banner (`UNUserNotification`), pings Dock attention, and — when the
session's tab is in the background — lights a **system-blue accent dot** on it
until the tab is focused.

`OSC 9 ; 4 ; <state> ; <value>` is *not* a notification — that is the ConEmu
progress protocol, parsed separately and shown in pane and tab progress chrome.
Its bounded state is terminal presentation metadata and replicates to remote
viewers without granting a host-escaping effect. A consumer that updates the
system Dock or taskbar must check `Capability::ProgressIndicator` at that effect
boundary. Recording progress grants no notification, attention, or focus
permission.

### Unseen attention

A bell and a notification are edges: each reaches whoever is attached at that
moment. The terminal also latches each (`Ubiquitty::unseenAttention`: a bell
flag, a notification flag and the newest notification's title and body, 255
bytes each), so a listing or a viewer that arrives later can still mark the
pane. The latch is set whether or not a client is there to be told, is not
cleared by the frontend draining its bell cue or by a hard reset the program
sends, and goes only when `Ubiquitty::markAttentionSeen` records that a person
looked. A notification the host policy denies is neither delivered nor latched.
Which viewer's look counts, and when, is the caller's rule, not Core's.

## One ordering: what a pane, tab, window and workspace show

The vocabularies above each order themselves (`ProgramState` by its enum,
progress by `progressRank`). `Source/Core/App/StatusRollup.h` is the one place
they are ordered against each other. A pane's facts amount to one kind, from
most to least urgent:

| Kind | A pane is this when |
|---|---|
| Failed | a status record is `error`, OSC 9;4 reports its error state, or the prompt's last command exited non-zero |
| Blocked | a status record is `blocked` (the leaf says whether on a permission, a question or a credential) |
| Attention | a bell rang or a notification was posted and nobody has seen it |
| Done | a status record is `done` |
| Working | a status record is `working`, or OSC 9;4 reports any live state (paused included) |
| Running | a program holds the terminal and has reported nothing |
| Idle | a shell is at its prompt, or a status record is `idle` |

An agent's status is an OSC 7501 record and ranks as any other. A failing exit
status counts only while its prompt is showing it. The OSC 21337 indicator
colour, iTerm2's tab colour and its badge ride with the pane and do not change
its kind.

A node above a pane takes the greatest kind among its children
(`rollUpStatus`), with how many panes share it, the first such child's colours,
any unseen bell or notification beneath it, and the panes' progress folded by
`aggregateProgress`. The fold is the same from panes to a tab, tabs to a
window and windows to a workspace, and gives the same answer however the panes
are grouped. `App::paneStatusFacts` reads a pane's facts off its terminal; who
holds the terminal (the prompt or a program) comes from whoever owns the PTY.

### Colours

A kind is drawn in the theme's colour for it (`Themes::StatusColorSlot`:
failed, blocked, attention, done, working, neutral; `statusColorSlot` maps a
kind to its slot). A theme's defaults are its own ANSI red, yellow, magenta,
green and blue, and for neutral the midpoint of its foreground and background
(`Themes::deriveStatusColors`, `Theme::statusColors`). The colour Core hands a
frontend for an OSC 7501 record follows the pane's theme, read from the palette
the theme gave the terminal, so a program repainting an ANSI slot with OSC 4
does not move it. An OSC 21337 indicator is the program's own colour and is
drawn as sent.

The ranking below is a different question, which mark a one-slot tab strip
draws, and keeps its own reasons (a ring is redrawn while a job runs and a bell
waits).

## Inactive-tab indicator priority

On a background tab, `Source/Core/App/TabAccessory.h` selects one mark,
rendered by `TabAccessoryPresenter`:

1. Progress: with a percent, a ring filled to it; of unknown length, the
   ring turning. The macOS frontend counts a program reporting working with
   OSC 7501, and no OSC 9;4 progress of its own, as progress
   (`App::progressWithProgramStatus`): of its record's `progress` when it
   gives one, and of unknown length when it does not, for this mark, the
   selected tab's bar and the window's own bar alike. Working with no percent
   shows as an ellipsis whose dots light in turn instead of the turning ring.
2. Unheard bell: a bell, using the program's status colour when present.
3. Program status: an OSC 7501 record's symbol in the theme's colour for it,
   a blocked one's saying what it waits for (`questionmark.bubble.fill` an
   answer, `hand.raised.fill` a permission, `key.fill` a sign-in); an OSC 21337
   colour alone, one dot.
4. Unseen notification: one notification-colour dot.
5. Unseen output: three dim circles.

On macOS a status that arrives while its tab is not the one being looked at is
news until the tab is selected, and so is a bell until it is heard. A question,
a failure, done and a bell glow: a halo traced from the symbol's own outline,
with spikes travelling round it, behind a gap the colour of the symbol's
cutout. An unseen question and an unheard bell also wiggle, and an unseen
failure bounces, every few seconds. A blocked program that comes to wait for
something else is news again. The symbols, the ellipsis and the
ring share the ring's frame, so one gives way to another, the old shrinking
away and the new expanding into place, each in its own colour, without moving
the tab's title. With Reduce Motion on, they change in place and stay still.

With shortcut hints enabled, the selected tab suppresses attention marks. A
shortcut hint can occupy the slot only when no attention mark takes precedence.

With `[tabs] show_shortcuts = "off"` the slot has no hint to fall back to and
carries the tab's status instead, on the selected tab too. The status dot
shows whenever the program set a colour. An unheard bell keeps its mark on the
selected tab of a window that was not key when it rang, until the window is
activated. The ring does not show on the selected tab, whose progress the bar
in its pill draws, and unseen output shows no ellipsis. A notification still
marks only a background tab. Focusing a tab does not clear a program's
persistent status record or progress value; attention
acknowledgement and presentation suppression are distinct.

### Tracing accessory decisions (macOS)

Set `UBIQUITTY_TAB_ACCESSORY_TRACE=1` in the app's environment to write one
line to standard error for every accessory decision the macOS shell makes:

```sh
UBIQUITTY_TAB_ACCESSORY_TRACE=1 /Applications/Ubiquitty.app/Contents/MacOS/Ubiquitty 2>tab-accessory.log
```

Each line is a `[tab-accessory]` record of one `TabAccessoryPresenter.apply`:
the tab's position and the group's size (`tab=1/2`), which position the group
reports as selected, the `isSelected` fact handed to Core, whether the window
is key and the application active, the unread, bell, notification, progress and
status-colour facts, whether the shortcut hint is wanted (`held=`) and whether
it is turned off (`hintOff=`), Core's answer
(`kind=`), and what the slot holds after the write (`slot=`, with `fading` while
a leaving mark is still on its way out). A tab whose slot disagrees with what
its tab should show is diagnosed by the last line naming it: either Core was
handed a wrong fact, or the slot never followed Core's answer.
