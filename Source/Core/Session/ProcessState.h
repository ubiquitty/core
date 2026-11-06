// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Source/Core/Session/ProcessState.h — one explicit value type for a session's
// process/title facts, plus the pure reducer + precedence that title
// composition, the Inspector, and the daemon foreground probe consume.
//
// One owner for the process/title facts keeps the "which source wins the title"
// precedence in one place to read, and lets a stale source be dumped and
// explained rather than inferred from scattered caches.
//
// The struct holds the facts; applyProcessEvent() folds a typed event into it
// and reports whether a rendered field changed; resolveAppTitle() encodes the
// tmux-rename > OSC-title precedence once. No Terminal, no Values, no platform
// string types — exception/rtti-free (WebKit style), host-tested.

#include "Base/SwiftInterop.h"
#include "Core/Session/CustomTitles.h"
#include "PAL/RemoteTarget.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Session {

/// Which input supplied the app-title segment a surface shows.
///
/// Recorded so a diagnostic dump can answer "why is the title X" — with the
/// precedence resolved in one place, the losing sources are otherwise invisible.
enum class TitleSource : uint8_t {
    None, // no OSC title, no tmux name — the caller applies its own fallback
    OSCTitle, // the host's OSC 0/2 window title (Terminal::titleUTF8)
    TmuxRename, // a tmux %window-renamed name (tab surface only)
};

/// Where a resolved title is shown.
///
/// The window and the tab differ only in whether a tmux window name may win:
/// on the tab it does, while the window title always shows the pane's own OSC
/// title. A pane, its header or any other place that names one pane among
/// several, answers only to what was put on that pane: a name given to the tab
/// or the window it sits in would read the same on every pane beside it.
enum class TitleSurface : uint8_t {
    Window UBIQUITTY_SWIFT_NAME(window),
    Tab UBIQUITTY_SWIFT_NAME(tab),
    Pane UBIQUITTY_SWIFT_NAME(pane)
};

/// The bound on one walk of a session's process subtree, the frontend's and
/// the host's alike. A process table longer than this is sampled by its first
/// entries, which still carry the foreground group the chain is built from.
inline constexpr std::size_t processChainWalkCapacity = 128;

/// A session's process and title facts.
///
/// Owned strings, so this is a value snapshot: no raw-byte serialization
/// depends on the layout. The default is the pre-exec blank a fresh session
/// starts from.
struct ProcessState {
    uint32_t foregroundProcessGroup = 0; // tcgetpgrp; 0 = none resolved. Gates the
                                         // costly chain rewalk; not itself rendered.
    std::string foregroundName; // foreground leaf basename; "" if it's our own pre-exec image
    std::string currentDirectory; // sampled foreground cwd; "" if unknown
    std::string processChain; // "zsh ▸ vim" display chain (already formatChain-formatted)
    std::string processChainWithArguments; // the same chain spelled with argv, for the [title] sub-option that asks
    std::string oscTitle; // last OSC 0/2 host title
    std::string tmuxWindowName; // last %window-renamed value
    std::string launchSeed; // spawn-command basename, shown before the first live sample
    // Where the foreground process leads, when it leads off this machine or
    // into a container. A fact about the session rather than a rendering of
    // its arguments, so it stands whether or not arguments are shown — a
    // window tag asking "am I on example-host" is not asking to see argv.
    PAL::Process::RemoteTarget remoteTarget;
    // The host a shell on the far side reported, from OSC 7's authority. Not
    // from the walk: it arrives on the drain that parsed the sequence, and it
    // is the only source that sees past a nested ssh. Cleared on the drain
    // whose probe finds the program that reported it gone from the foreground.
    std::string reportedHost;

    uint64_t generation = 0; // bumped on every render-visible change
    uint64_t lastPollTick = 0; // caller-stamped tick of the last PollResult
    uint64_t lastChangeTick = 0; // caller-stamped tick of the last render-visible change
};

// The inputs that mutate the state: the title poll, the OSC-title write, the
// tmux rename, the launch seed and the daemon probe each arrive as one of these.
/// Which input an event carries.
enum class ProcessEventKind : uint8_t {
    LaunchSeed, // the spawn command's basename (seeds the title pre-exec)
    PollResult, // a title-poll sample: pgid + foreground name + cwd + chain
    OSCTitle, // an OSC 0/2 title write from the host
    TmuxRename, // a tmux %window-renamed
};

/// One typed input to `applyProcessEvent`.
///
/// `PollResult` carries all four sampled facts; every other kind carries its
/// single string in `text` (LaunchSeed → seed, OSCTitle → title, TmuxRename →
/// window name). Unused fields stay empty or zero.
struct ProcessEvent {
    ProcessEventKind kind;
    uint32_t foregroundProcessGroup = 0; // PollResult only
    // Every string carries an explicit default, which is what lets a caller
    // name only the fields its event kind uses. GCC warns on an omitted member
    // that has none — `-Wmissing-field-initializers`, which this tree builds
    // with `-Werror` on Linux and which clang does not raise for a designated
    // initialiser — so a kind that legitimately fills three of six fields
    // stopped the whole test suite from building on that arm alone.
    std::string foregroundName {}; // PollResult only
    std::string currentDirectory {}; // PollResult only
    std::string processChain {}; // PollResult only
    std::string text {}; // LaunchSeed / OSCTitle / TmuxRename
    uint64_t tick = 0; // caller-stamped monotonic tick
    // Last, because callers build this aggregate positionally: a field put
    // among the others silently re-binds every one after it.
    std::string processChainWithArguments {}; // PollResult only; the argv spelling of the same walk
    // PollResult only. Read from the same walk, since finding it needs the
    // foreground leaf's argv and a second walk would pay for it twice.
    PAL::Process::RemoteTarget remoteTarget {};
};

/// Folds one event into the session's process state.
///
/// A bare foreground-pgid move is not a visible change — it only gates the
/// rewalk — so a consumer repainting on the return value does not repaint for
/// it. On a visible change `generation` bumps and `lastChangeTick` is stamped;
/// a `PollResult` always stamps `lastPollTick` whatever it returns.
///
/// - Parameters:
///   - state: The state to fold into, updated in place.
///   - event: The typed event to apply.
/// - Returns: `true` when a field a title or inspector consumer renders changed.
bool applyProcessEvent(ProcessState& state, const ProcessEvent& event);

// The app-title segment (the "<app title>" part of the woven rich title) and
// which source supplied it, for the given surface. Tab: a non-empty tmux window
// name wins over the pane's OSC title. Window: the OSC title. This is the single
// encoding of the precedence the window controller wove inline. The returned
// view aliases a `state` member, so it is valid only while `state` outlives it.
/// A resolved title and the source that won it.
///
/// `text` aliases a member of the `ProcessState` it was resolved from, so it is
/// valid only while that state outlives it.
struct ResolvedAppTitle {
    std::string_view text;
    TitleSource source;
};
[[nodiscard]] ResolvedAppTitle resolveAppTitle(const ProcessState& state, TitleSurface surface);

/// Which of the two names a program can put on a surface it shows.
///
/// A tab prefers what tmux calls its window, so a rename survives the pane's
/// next prompt redraw; a window title always shows the pane's own. The one
/// encoding of that: `Protocol::Tmux::chooseTabTitle` and the ranking below
/// both ask here rather than spelling it again.
///
/// - Parameters:
///   - tmuxWindowName: What tmux calls the window, or empty.
///   - oscTitle: What the program running in the pane set, or empty.
///   - surface: Which surface is being named.
/// - Returns: The winner, which is a view into one of the arguments.
[[nodiscard]] std::string_view appTitleForSurface(std::string_view tmuxWindowName, std::string_view oscTitle, TitleSurface surface);

/// The names put on a surface by a person or by the program running in it.
///
/// Any may be empty, which is how a name that was never set is spelled. The
/// views must outlive the call.
struct TitleNames {
    std::string_view tabTitle; // the tab's custom title (`CustomTitles::tab`)
    std::string_view windowTitle; // the window's custom title (`CustomTitles::window`)
    std::string_view tmuxWindowName; // what tmux calls this window
    std::string_view oscTitle; // what the program running here set
    std::string_view paneLabel; // the name put on this one pane; only a pane answers to it
};

/// What the names decided.
///
/// Owned strings, not views: the answer outlives the call on both shells —
/// one hands it to AppKit, the other to Swift — and a view into the caller's
/// inputs is a lifetime rule every caller would have to keep.
struct ResolvedTitleName {
    std::string replacement; // the whole title; empty means the weave proceeds
    std::string appTitle; // what the app-title segment carries when it does
    bool isCustomName; // bool-ok: names the one condition it reports — the segment is a name a person typed
};

/// Ranks the names a surface can answer to.
///
/// One rule for the window and the tab: a custom title replaces only the part
/// it names. It fills the app-title segment — what the running program would
/// call the surface — and the composition drops the process segments beside
/// it, which also describe what runs there and would contradict the name. The
/// parts that say where and how big stay as the surface's `[title]`
/// composition has them: the directory, the columns×rows readout, the
/// program's status, the host chip and the tab's marks. A name a person typed
/// shows whatever the app-title flag says, since a setting about titles a
/// program sets has no business hiding it.
///
/// The tab answers to its own custom title, else to its window's, so naming a
/// window names its tab until the tab is named apart; the window never
/// answers to its tab's. An empty title is automatic, and the segment goes
/// back to ``appTitleForSurface``'s answer at once.
///
/// A pane answers to neither: it is called by its own label, which replaces
/// its header outright because a label is the handle a pane is addressed by,
/// else by what its program set.
///
[[nodiscard]] ResolvedTitleName resolveTitleNames(const TitleNames& names, TitleSurface surface);

} // namespace Ubiquitty::Session
