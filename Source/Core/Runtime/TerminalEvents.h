// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"

#include "Base/CheckedPtr.h"
#include <cstdint>
#include <utility>

// The runtime calls the sink INLINE on the writer context (the thread holding the
// model lock). A sink must only set a coalescing flag/atomic and return: never
// dispatch_async, never dispatch_sync to the main thread (the ingest→main deadlock).

namespace Ubiquitty::Runtime {

/// What a session raises, and the bit a coalescing frontend reads it at.
///
/// The flag surface is `1 << EventKind`, so an ordinal is a bit: a kind
/// inserted in the middle renumbers every bit above it, and a frontend that
/// kept its own copy of the layout would deliver the wrong event while both
/// files stayed internally consistent. There is no copy — Swift uses this enum,
/// and the names below carry the case convention it reads them in.
enum class EventKind : std::uint8_t {
    /// The model changed; a repaint should be scheduled.
    ScreenDirty UBIQUITTY_SWIFT_NAME(screenDirty),
    /// The child process exited or the stream reached its end.
    Exited UBIQUITTY_SWIFT_NAME(exited),
    /// BEL arrived; the style rides `data0`.
    Bell UBIQUITTY_SWIFT_NAME(bell),
    /// The OSC title or icon changed.
    TitleChanged UBIQUITTY_SWIFT_NAME(titleChanged),
    /// A transfer preamble arrived (data0 = Protocol, data1 = Direction).
    FileTransferDetected UBIQUITTY_SWIFT_NAME(fileTransferDetected),
    // A remote host has no ubiquitty-host and the user has not yet approved
    // pushing one there. The frontend asks, then calls approveHostDeploy or
    // declineHostDeploy; nothing is installed until it does, and the session
    // holds the attempt open meanwhile.
    DeployConfirmationPending UBIQUITTY_SWIFT_NAME(deployConfirmationPending),
    /// A pushed paste settled; `data0` is the `PastePushStatus`. The path it
    /// landed at is not a scalar, so it is read from the session rather than
    /// carried here — see ``TerminalSession::lastPastePushPath``.
    PastePushSettled UBIQUITTY_SWIFT_NAME(pastePushSettled),
    /// A remote session's connection state moved; `data0` is the
    /// `Session::State`.
    ///
    /// Without this a frontend on the portable session path cannot tell
    /// connecting from connected from a host that has gone: the state machine
    /// runs in Core and a frontend's own transport delegate is not a channel
    /// the portable path has. A window with no way to say "reconnecting" shows a frozen
    /// screen and no reason for it.
    RemoteStateChanged UBIQUITTY_SWIFT_NAME(remoteStateChanged),
    /// The host pinned this connection's session id; `data0` carries it.
    ///
    /// The id a frontend persists to reattach the same shell after a restart
    /// rather than spawning a fresh one. Nonzero by the time this fires.
    RemoteSessionEstablished UBIQUITTY_SWIFT_NAME(remoteSessionEstablished),
    /// An agent is asking the human to let it attach, kill or gain a
    /// capability on this session. `data0` is the request id, `data1` the
    /// action.
    ///
    /// Who is asking and what it calls itself are strings, so they are read
    /// beside this rather than carried in it — see
    /// ``TerminalSession::pendingConsentPeerProcess``. The frontend answers
    /// with ``TerminalSession::respondToConsent``; the daemon holds the agent
    /// for 30 seconds and then denies it, so a frontend that never answers
    /// fails closed but also cannot say yes.
    ConsentRequestPending UBIQUITTY_SWIFT_NAME(consentRequestPending),
    /// Who is attached to this session changed — someone joined, left, or had
    /// a grant altered, or screen-access activity arrived or was reviewed.
    ///
    /// The roster is a list of attendees with names, so it is read beside this
    /// rather than carried in it — see
    /// ``TerminalSession::presenceAttendeeCount``. A session with more than one
    /// attendee is one somebody else can see, which is why this is reported at
    /// all rather than left to whoever asked.
    PresenceChanged UBIQUITTY_SWIFT_NAME(presenceChanged),
    /// The reported working directory changed (OSC 7, OSC 633 Cwd, OSC 1337
    /// CurrentDir). Carries no payload; read ``Terminal::currentDirectory``
    /// under the model lock, the way the title is read after `TitleChanged`.
    DirectoryChanged UBIQUITTY_SWIFT_NAME(directoryChanged),
    /// The host's session listing was answered; read
    /// ``TerminalSession::hostOutline`` and decide with
    /// ``TerminalSession::decideHostAttach``.
    ///
    /// A tree is read beside this rather than carried in it — the shape
    /// ``PresenceChanged`` takes, for the same reason. The tree describes the
    /// instant the host answered: a picker showing it is showing what was
    /// true when it asked.
    HostSessionListChanged UBIQUITTY_SWIFT_NAME(hostSessionListChanged),
    /// A host session the frontend asked to terminate settled; `data0` is the
    /// `Session::TerminateOutcome`.
    ///
    /// The outcome is the whole answer: a terminate the daemon refused, denied
    /// or never answered is not a session that went away, and a frontend that
    /// dropped the row on the ask alone would show a session gone that is
    /// still running.
    HostSessionTerminateSettled UBIQUITTY_SWIFT_NAME(hostSessionTerminateSettled),
    /// The host's workspace was mirrored anew: on attach, and after every
    /// verb the host performed, whichever client sent it. The tree is read
    /// beside this — ``TerminalSession::remoteWorkspaceSurfaceCount`` and the
    /// getters after it — the shape ``PresenceChanged`` takes, for the same
    /// reason: a topology is not a scalar.
    WorkspaceChanged UBIQUITTY_SWIFT_NAME(workspaceChanged),
    /// The sampled process facts moved: a different foreground chain, or a
    /// session-leader shell resolved for the first time. Carries no payload;
    /// read ``TerminalSession::processChain`` and the getters beside it, the
    /// shape ``PresenceChanged`` takes.
    ///
    /// A title made of these segments has no other edge to redraw on. The
    /// OSC title and the directory each have their own, and a shell that
    /// launches a program while setting neither would otherwise leave the
    /// window named after the program that finished.
    ProcessStateChanged UBIQUITTY_SWIFT_NAME(processStateChanged),
    /// The program's OSC 9;4 progress report moved; the pair is read beside
    /// this, off the published snapshot's `progressState` and `progressValue`.
    ///
    /// Its own kind because a report changes no cell: a frontend watching
    /// `ScreenDirty` hears nothing on the drain that parsed it, and would
    /// show the band only once the program happened to print something.
    ProgressChanged UBIQUITTY_SWIFT_NAME(progressChanged),
    /// An application asked for a font swap with OSC 50.
    ///
    /// Appended rather than placed by topic: the enumerators index bits in a
    /// published word, so inserting one renumbers every bit after it and a
    /// frontend built against the old numbering reads a different event.
    ///
    /// Carried as an event because a font swap moves no cell: a frontend that
    /// reads its facts when something changed would never look, since the drain
    /// that parsed the request is the last one to run. The request itself is
    /// read beside this, under the model lock — see
    /// ``TerminalSession::beginFontRequest``.
    FontSwapRequested UBIQUITTY_SWIFT_NAME(fontSwapRequested),
    /// An application asked for something only a window can do: a CSI t window
    /// operation, a DECCOLM column change, OSC 1337 StealFocus, an attention
    /// request, or an OSC 1337 colour preset.
    ///
    /// Appended, for the reason the kind above gives: the enumerators index
    /// bits in a published word.
    ///
    /// One kind for five verbs rather than five kinds, because one thing
    /// answers all of them — a window — and five drains are five chances to
    /// answer four. Which verb it is, and its arguments, are read beside this:
    /// see ``TerminalSession::takeWindowRequest``.
    ///
    /// Carried as an event because none of the five moves a cell, and because
    /// the parser makes these calls inline while the model lock is held. A
    /// frontend cannot resize a window from there.
    WindowRequestPending UBIQUITTY_SWIFT_NAME(windowRequestPending),
    /// An application asked for a desktop notification: OSC 9 (iTerm), OSC 99
    /// (kitty) or OSC 777;notify (urxvt).
    ///
    /// Appended, for the reason the kinds above give: the enumerators index
    /// bits in a published word.
    ///
    /// The title and body are strings, so they are read beside this rather
    /// than carried in it — see ``TerminalSession::takeNotification``.
    ///
    /// Carried as an event because a notification moves no cell: a frontend
    /// watching `ScreenDirty` hears nothing on the drain that parsed one, and
    /// a program that notifies and then waits prints nothing after it.
    NotificationPosted UBIQUITTY_SWIFT_NAME(notificationPosted),
    /// The session's own tab status colour moved: OSC 21337 set one, changed
    /// one, or cleared it. The colour is read beside this, off the published
    /// snapshot's `isTabStatusColorSet` and the three channels after it.
    ///
    /// Appended, for the reason the kinds above give: the enumerators index
    /// bits in a published word.
    ///
    /// Its own kind for the reason ``ProgressChanged`` has one, and one
    /// sharper: the escape exists to report the state of a session whose
    /// screen nobody is looking at. A frontend that learned of it only when
    /// the program next printed something would show the colour for "waiting"
    /// exactly when the wait ended.
    TabStatusChanged UBIQUITTY_SWIFT_NAME(tabStatusChanged),
    /// A configured trigger matched a completed line and has an act for the
    /// frontend: a banner, a copy, a title, a spawn or a POST.
    ///
    /// Appended, for the reason the kinds above give: the enumerators index
    /// bits in a published word.
    ///
    /// The act is read beside this rather than carried in it — see
    /// ``TerminalSession::takeTriggerEffect`` — because it is strings, and
    /// because which acts stand depends on gates a frontend reads on its own
    /// thread.
    ///
    /// Carried as an event because a trigger is the engine's report on a line
    /// the drain already painted: the cells moved for the line, and nothing
    /// moves for what the line matched.
    TriggerMatched UBIQUITTY_SWIFT_NAME(triggerMatched),
    /// The push of `ubiquitty-host` to a remote host failed. The session has
    /// already gone Failed; what the push printed is read beside this — see
    /// ``TerminalSession::lastDeployFailure``.
    ///
    /// Appended, for the reason the kinds above give.
    ///
    /// Its own kind rather than read off `RemoteStateChanged`, because a
    /// failed install is something the person has to act on — the output
    /// names the missing permission or the full disk — where most Failed
    /// states are a host that went away and only need saying.
    DeployFailed UBIQUITTY_SWIFT_NAME(deployFailed),
    /// The remote host has no `ubiquitty-host` and no automatic install can
    /// supply one: the setting is off, or no bundled binary serves the host's
    /// platform. The session has already gone Failed; the commands a person
    /// runs by hand and why they are needed are read beside this — see
    /// ``TerminalSession::manualHostInstallCommands``.
    ///
    /// Appended, for the reason the kinds above give.
    DeployNeedsManualInstall UBIQUITTY_SWIFT_NAME(deployNeedsManualInstall),
    /// The host answered a file this session asked it for. What it answered —
    /// the status, the path it echoed and the bytes — is read beside this; see
    /// ``TerminalSession::hasRemoteFile`` and the getters after it.
    ///
    /// One bit for however many answers arrived: the word is coalesced, and
    /// the answers are a queue, so a reader of this drains until
    /// ``TerminalSession::hasRemoteFile`` says nothing is left rather than
    /// taking one and waiting for the next event.
    ///
    /// Appended, for the reason the kinds above give: the enumerators index
    /// bits in a published word.
    ///
    /// Carried as an event because a fetched file moves no cell, and because
    /// the answer arrives on the remote transport's thread, where nothing may
    /// touch the filesystem or the window.
    RemoteFileArrived UBIQUITTY_SWIFT_NAME(remoteFileArrived),
    /// The windows this session's program hosts changed: one appeared, was
    /// laid out anew, renamed or closed, or a pane of one has new content.
    ///
    /// Appended, for the reason the kinds above give: the enumerators index
    /// bits in a published word.
    ///
    /// The changes are a queue read beside this — see
    /// ``TerminalSession::takeHostedWindowEvent`` and
    /// ``TerminalSession::takeRepaintedHostedPanes`` — because a window's
    /// layout is a tree and its panes a list, not a scalar.
    HostedWindowsChanged UBIQUITTY_SWIFT_NAME(hostedWindowsChanged),
    /// A change asked of the host's structure ended: served, refused or
    /// unanswered. Carries no payload; read
    /// ``TerminalSession::lastHostStructureResult``, which says which
    /// request and why, the shape ``PresenceChanged`` takes.
    HostStructureSettled UBIQUITTY_SWIFT_NAME(hostStructureSettled),
    /// The pane's attachment phase or either independent stall reason changed.
    /// The indicator is read through `TerminalSession::paneLinkIndicatorText`.
    LinkStateChanged UBIQUITTY_SWIFT_NAME(linkStateChanged),
    /// Native drag commands are available from TerminalSession::takeDragDropCommand.
    DragDropPending UBIQUITTY_SWIFT_NAME(dragDropPending),
    /// Search results or focus changed; read RuntimeSnapshot::search.
    SearchChanged UBIQUITTY_SWIFT_NAME(searchChanged),
    /// How many of a remote pane's input events are in doubt, or were
    /// discarded, changed. Read ``TerminalSession::remoteInputStatus``. While
    /// it awaits a decision, what the person types waits behind the events in
    /// doubt until they are sent again or discarded at the person's word.
    ///
    /// Appended, for the reason the kinds above give: the enumerators index
    /// bits in a published word.
    InputInDoubtChanged UBIQUITTY_SWIFT_NAME(inputInDoubtChanged),
};

static_assert(std::to_underlying(EventKind::InputInDoubtChanged) < 32, "every kind is one bit of the 32-bit published word");

// Scalar-only payload — no pointers, no allocation. Specific values are
// interpreted per-kind (e.g. data0 = bell style for Bell).
struct Event {
    EventKind kind;
    std::uint32_t data0 = 0;
    std::uint32_t data1 = 0;
};

// CanMakeCheckedPtr so a sink torn down before the runtime is a debug trap,
// not a silent use-after-free.
class EventSink : public CanMakeCheckedPtr {
public:
    virtual ~EventSink() = default;
    virtual void onRuntimeEvent(const Event& event) = 0;
};

} // namespace Ubiquitty::Runtime
