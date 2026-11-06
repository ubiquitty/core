// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/PAL/PAL.h — the seam between Core and the frontend. Core depends on
// this header; each frontend (macOS / Linux / host) implements every function
// under Source/Platform/<Frontend>/. Keep it minimal — when in doubt, push
// functionality into Core, not PAL. Layer map: Docs/architecture.md.
//
// Contract:
// - Naming: lowerCamelCase with a subsystem prefix. Stateless facilities are
//   free functions in Ubiquitty::PAL; anything holding a live handle is an
//   abstract class the platform derives (ByteStream and Stream in PAL/Stream.h,
//   ProcessFacts).
// - Types: the <cstdint> names and std::size_t (Base/Types.h guarantees them on
//   every host), char / int / bool / void*, STL vocabulary types
//   (std::span, std::string_view, std::unique_ptr, std::optional) and Core's own
//   small value types such as Ticks and BellMode. STL across the seam is
//   allowed — Docs/coding-conventions.md §1 permits it in Core, Runtime and
//   the PAL seam alike, and this header's own signatures use it.
//   A platform type is what cannot cross: no NSString, id, MTLBuffer, GTK or
//   Vulkan handle. That half is not a convention anyone has to remember — Core
//   compiles for wasm and for the Linux container, so a platform type here fails
//   those builds.
// - Lifecycle: a factory returns ownership, not a raw pointer to pair by hand —
//   openPTY / openSerial / connectLocal hand back a std::unique_ptr and the
//   destructor closes. -1 == failure; positive == byte/element count unless
//   noted.
// - Threading: single-threaded by construction (Docs/coding-conventions.md
//   §5.2); backends using threads must marshal callbacks back to the main
//   thread.
// - Scope: declarations only. Includes are Base/Types.h, PAL/BellMode.h, and the
//   standard headers the signatures below name.
//
// Two directions live here, and they bind differently. Core calls *down* through
// free functions, resolved at link time against one backend per binary. The
// frontend answers *up* through the function pointers in ``Context``, installed
// once at startup. Down-calls are the default; a hook earns its place only when
// the frontend has to be reached from inside Core's own call stack.
//

#include "Base/Types.h"
#include "PAL/BellMode.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Ubiquitty::PAL {

// ---- Clock / time ------------------------------------------------

/// The platform's monotonic clock, in 60 Hz ticks.
///
/// Backed by `CACurrentMediaTime() * 60` on macOS and `clock_gettime(MONOTONIC)`
/// on Linux. Called by Core and the application for diagnostics, the animation
/// throttle, and idle-band classification.
/// The tick type lives in Base/Types.h: the model layer counts in ticks too and
/// cannot see this header.
using Ubiquitty::Ticks;

Ticks monotonicTicks();

/// The platform's monotonic clock, in microseconds.
///
/// - Important: Wrap-around is best-effort. Use it only for deltas measured
///   against another call within a few hours.
uint32_t monotonicMicros();

/// How Core reads the clock.
///
/// Core translation units are host-tested against link sets that have no
/// platform clock, so they read through this nullable hook rather than the hard
/// ``monotonicTicks`` symbol. Each frontend installs it once at startup pointing
/// at ``monotonicTicks``; where it is unset the caller degrades instead of
/// failing — a command duration reads as unknown, for instance.
using MonotonicClockHook = Ticks (*)();

/// The installed clock hook.
///
/// Required in a real frontend, and non-null once a ``Context`` is installed.
MonotonicClockHook monotonicClock();

// ---- Clipboard ---------------------------------------------------

/// Replaces the clipboard's text, as UTF-8.
///
/// Core's OSC 52 handler is the caller.
///
/// - Parameter utf8: The text, which need not be NUL-terminated.
void clipboardSetText(std::string_view utf8);

/// Reads the clipboard's text as UTF-8.
///
/// - Returns: The byte count written, clamped to `outputCapacity`; -1 on failure
///   or when permission is denied.
int clipboardGetText(char* outputBuffer, int outputCapacity);

// ---- Preferences (atomic key-value blob) -------------------------

/// Names one opaque, app-owned blob.
struct PreferencesBlobKey {
    const char* name;
};

/// The blob holding the user's configuration text.
///
/// Named here rather than in Core because a backend has to recognise it: this is
/// the one blob a person edits by hand, so it lives where that person expects to
/// find it — `$XDG_CONFIG_HOME/ubiquitty/config.toml`, or `~/.config/...` — while
/// an opaque record can live wherever the platform files such things. That is the
/// latitude "the backend chooses how to honour it" was written for, and a
/// user-visible path is the case that uses it.
///
/// A backend that does not special-case this key still works; the user's existing
/// file is what stops being found, which is why every backend that had one before
/// this key existed keeps the same location for it.
inline constexpr PreferencesBlobKey configurationBlobKey { "config" };

/// Loads the blob stored under `key`.
///
/// The contract is an atomic write of an opaque byte range under a key, and the
/// backend chooses how to honour it — macOS could back this with a single binary
/// plist, Linux with `rename(2)`.
///
/// Key-addressed rather than path-addressed, because a path is not a thing every
/// backend has. The browser can store bytes under a name and cannot open a
/// file, so a seam asking for a path, as `storageReadFile` does, is one it can
/// only decline, and an arm that declines grows a settings channel of its own
/// that diverges from Core's.
///
/// - Parameters:
///   - key: Names the blob.
///   - outputBuffer: Where the bytes go, or null to probe the size.
///   - outputCapacity: The buffer's size, or 0 to probe.
/// - Returns: The byte count read, clamped to `outputCapacity`; -1 when the blob
///   is absent or unreadable.
/// - Note: Pass a null buffer, or a capacity of 0, to probe — the same
///   convention `storageReadFile` carries, and what lets one seam hold a payload
///   whose length the caller cannot know in advance. A fixed-size record needs no
///   probe; configuration text does, and both travel through here.
int preferencesLoad(PreferencesBlobKey key, void* outputBuffer, int outputCapacity);

/// Stores `buffer` under `key`, atomically.
///
/// - Parameters:
///   - key: Names the blob.
///   - buffer: The bytes to store.
/// - Returns: Zero on success, or a negative errno.
int preferencesSaveAtomic(PreferencesBlobKey key, std::span<const std::byte> buffer);

} // namespace Ubiquitty::PAL

namespace Ubiquitty::Wasm {

/// The blobs a page has supplied, by name, for the wasm preferences backend.
///
/// Declared on the seam rather than in a frontend header because the backend
/// that owns it (Platform/Wasm/Preferences.cpp) is bundled into Core, while the
/// exported entry point a page calls to fill it lives in the frontend. Absent on
/// every other build, where a key means a file.
///
/// - Returns: The store, by reference, so a caller may read or replace an entry.
///   Module-lifetime: the page re-supplies its blobs on load, and durability is
///   the page's to arrange.
std::unordered_map<std::string, std::vector<std::byte>>& preferenceBlobs();

} // namespace Ubiquitty::Wasm

namespace Ubiquitty::PAL {

// ---- Logging / diagnostics sink ----------------------------------

/// Writes one diagnostic message.
///
/// A single sink: the backend fans out to its own destinations — stderr
/// everywhere, plus the unified log on macOS. Core writes here from the halt
/// journal, the performance diagnostics, and the parser's miss counters.
///
/// - Parameter message: One line, without a terminator; the backend adds its own.
void diagWrite(std::string_view message);

/// The one Core-owned bell enumeration, re-exported so `PAL::BellMode` keeps
/// working at every call site (see PAL/BellMode.h).
using BellMode = ::Ubiquitty::BellMode;

// ---- Bell --------------------------------------------------------

/// Rings the bell in the requested mode.
///
/// The device a bell plays on, not where Core rings it. Core delivers each bell
/// to the ringing terminal's own `EffectRoute` and sets its
/// `Terminal::bellPending`, so two panes in one process never share a bell; the
/// application calls this once it has resolved which terminal rang and in what
/// mode. The backend turns the mode into its own audible cue alone: macOS asks
/// AudioToolbox for the user's preferred alert sound, Linux beeps the display.
/// The visible half of a bell never crosses this seam — the terminal view
/// resolves it through `Ubiquitty::resolveBellEvent` and hands the renderer a
/// flash start tick directly, because the flash needs the renderer's overlay
/// pass and the per-frame tick clock.
///
/// - Important: Backends must not throw and must be safe to re-enter. They need
///   no debounce of their own — the application's bell coalescer already guards
///   against rapid-fire duplicates.
void bellRing(BellMode mode);

// ---- Login shell ------------------------------------------------

/// Looks up the current user's login shell in the system account database.
///
/// Reads `getpwuid(getuid())->pw_shell` on POSIX. Runtime calls it from
/// `LaunchCommand::loginShell` when `$SHELL` is unset.
///
/// The seam returns a path rather than a `uid_t`, so no POSIX identity type
/// crosses it.
///
/// - Parameters:
///   - outputBuffer: Receives the NUL-terminated path.
///   - outputCapacity: Size of `outputBuffer`, including room for the NUL.
/// - Returns: The byte count written, excluding the NUL; -1 when no shell
///   resolves or the buffer is too small.
int systemLoginShellPath(char* outputBuffer, int outputCapacity);

/// Looks up the current user's login name in the system account database.
///
/// Reads `getpwuid(getuid())->pw_name` on POSIX. Runtime calls it from
/// `LaunchCommand::loginSession`, which names the user to the login program.
///
/// A name rather than a `uid_t`, so no POSIX identity type crosses the seam.
///
/// - Parameters:
///   - outputBuffer: Receives the NUL-terminated name.
///   - outputCapacity: Size of `outputBuffer`, including room for the NUL.
/// - Returns: The byte count written, excluding the NUL; -1 when no name
///   resolves or the buffer is too small.
int systemLoginUserName(char* outputBuffer, int outputCapacity);

/// Returns this machine's current hostname, or an empty string when unavailable.
///
/// Reads the platform on each call so network-driven hostname changes take effect.
///
/// - Returns: The current hostname, or an empty string when the platform has none.
std::string systemHostName();

/// How this platform turns a terminal's shell into a registered login session.
///
/// The kinds differ in where the registration happens, so a caller must branch
/// on this rather than call both: Darwin registers inside the argument vector,
/// Linux after the fork. Each kind leaves the other's call inert.
enum class LoginSessionKind : unsigned char {
    /// Sessions are not registered here, and a shell is launched directly.
    Unregistered,
    /// Registered by launching through ``loginSessionProgram``.
    ThroughLoginProgram,
    /// Registered by ``registerLoginSession`` once the pseudo-terminal exists.
    ByRecordingThePTY,
};

/// How this platform registers a login session.
///
/// - Returns: The kind, which decides which of the two calls below applies.
LoginSessionKind loginSessionKind();

/// Records `ptyName` as a login session for the current user.
///
/// Applies to ``LoginSessionKind::ByRecordingThePTY``; called once the
/// pseudo-terminal is open. A refusal is ordinary — the session table is
/// group-writable by a group a desktop process is usually not in — and never
/// fails a launch: the answer reports whether the record is readable, so a
/// caller must not treat false as an error.
///
/// - Parameter ptyName: The slave device path, as `/dev/pts/3` or `/dev/ttys003`.
/// - Returns: Whether the record was written.
bool registerLoginSession(const char* ptyName);

/// Removes the login-session record for `ptyName`.
///
/// Must run for every ``registerLoginSession`` that answered true: a record
/// left behind outlives the terminal and lists a user who has logged out.
///
/// - Parameter ptyName: The same path the record was written for.
void unregisterLoginSession(const char* ptyName);

/// Resolves the program that opens a real login session for a local shell.
///
/// A terminal that spawns the login shell directly gets a working shell and
/// nothing else: no utmp entry, so `who` and `w` do not list the session, and
/// no PAM session, so whatever a site hangs off one does not run. Opening
/// through the platform's login program registers both, which is what every
/// established terminal on the platform does and what a user comparing them
/// notices the absence of.
///
/// A path answers only for a program accepting `<program> -pf <user>` — env
/// preserved, authentication skipped for the calling user, the login shell
/// exec'd — plus `-l` to keep an inherited working directory. A platform
/// spelling any of that differently answers -1: a caller cannot tell the
/// difference and would build an argument vector the program rejects.
///
/// - Parameters:
///   - outputBuffer: Receives the NUL-terminated path.
///   - outputCapacity: Size of `outputBuffer`, including room for the NUL.
/// - Returns: The byte count written, excluding the NUL; -1 where the platform
///   has no such program, or the buffer is too small.
int loginSessionProgram(char* outputBuffer, int outputCapacity);

// ---- Home directory ---------------------------------------------

/// Resolves the user's home directory.
///
/// Prefers `$HOME`, falling back to the account database — `getpwuid(getuid())->pw_dir`
/// on POSIX — when it is unset or empty.
///
/// One resolution for the whole process tree, which is the point. An app that
/// reads the account database while the daemon it spawns reads `$HOME` puts the
/// two in different directories whenever the two disagree, and they disagree
/// under `sudo -E`, `CFFIXED_USER_HOME`, a direct-from-shell launch, or an unset
/// `HOME` — every case where someone chose a home the account database has never
/// heard of. `$HOME` wins for that reason.
///
/// - Parameters:
///   - outputBuffer: Receives the NUL-terminated path.
///   - outputCapacity: Size of `outputBuffer`, including room for the NUL.
/// - Returns: The byte count written, excluding the NUL; -1 when no home
///   resolves or the buffer is too small.
int systemHomeDirectory(char* outputBuffer, int outputCapacity);

// ---- Scheduler ---------------------------------------------------

/// Yields the remainder of the calling thread's quantum.
///
/// `sched_yield` on POSIX. Runtime calls it from the ingest drain loop's
/// mid-burst backoff. A hint only: a backend with no scheduler control may do
/// nothing.
void schedulerYield();

/// Places the calling thread in the band interactive ingest runs in.
///
/// Called once by the ingest thread as it starts, so the band is a property
/// of the loop rather than of whichever thread created it — a thread with no
/// band of its own inherits its creator's, which makes the answer depend on
/// a caller that has nothing to do with ingest. A hint only: a backend with
/// no priority control may do nothing, and one whose threads already sit in
/// the band gains nothing measurable.
void raiseCurrentThreadToInteractivePriority();

/// Places the calling thread in the band background work runs in.
///
/// The mirror of ``raiseCurrentThreadToInteractivePriority``, called once by a
/// worker that scans on the user's behalf but must never compete with ingest
/// or rendering: the search worker. A hint only, as that one is.
void lowerCurrentThreadToUtilityPriority();

/// A mutual-exclusion lock whose holder runs at the priority of the most
/// urgent thread waiting for it.
///
/// For a lock a low-priority worker holds for a bounded stretch while a
/// high-priority thread may wait on it: without the donation, the waiter waits
/// for the worker to be scheduled as well as for the stretch to end. A backend
/// with one thread answers with a lock that does nothing.
class PriorityInheritingLock {
public:
    virtual ~PriorityInheritingLock() = default;
    /// Takes the lock, waiting for its holder, whose priority the wait raises.
    virtual void lock() = 0;
    /// Releases the lock the calling thread holds.
    virtual void unlock() = 0;
};

/// Makes a priority-inheriting lock.
///
/// - Returns: The lock, or null when the platform refused one, which a caller
///   treats as an allocation refusal.
[[nodiscard]] std::unique_ptr<PriorityInheritingLock> makePriorityInheritingLock();

/// Whether the calling thread is the process's main thread.
///
/// The thread the frontend's event loop runs on, which is the one thread the
/// live preferences (``Preferences::values``) may be read or replaced from. A
/// backend with a single thread answers true.
///
/// - Returns: `true` on the main thread.
[[nodiscard]] bool isMainThread();

// ---- Atomic file storage ----------------------------------------
//
// A small portable file toolkit, distinct from the preferences surface above:
// these name paths the caller chose, such as ~/.config/ubiquitty/config.toml,
// where ``preferencesLoad`` keys an opaque app-owned blob. Settings reads
// config.toml and rewrites keys through here.

/// Reads a whole file.
///
/// - Returns: The byte count read, clamped to `outputCapacity`; -1 when the file
///   is absent or unreadable.
/// - Note: Pass a null buffer, or a capacity of 0, to probe the size — the full
///   byte count is returned, or -1 when absent.
int storageReadFile(const char* path, void* outputBuffer, int outputCapacity);

/// Creates a directory and any missing parents.
///
/// - Returns: True on success, and also when the directory already exists.
bool storageEnsureDirectory(const char* path);

/// Writes `buffer` over `path`, atomically.
///
/// Writes to a sibling temp file and `rename(2)`s it over `path`, so a reader
/// sees either the whole old content or the whole new content, never a torn
/// write. The parent directory is created on demand.
///
/// - Parameters:
///   - path: The destination, whose parent is created on demand.
///   - buffer: The bytes to write.
/// - Returns: True when the rename committed.
bool storageWriteAtomic(const char* path, std::span<const std::byte> buffer);

/// Removes a file, best-effort. No error is reported.
void storageRemoveFile(const char* path);

// ---- Process spawn ----------------------------------------------
//
// Terminfo's `tic` installer is the caller: it writes a compiled entry to a
// temp file, checks for the compiler, and runs it.

/// Whether `path` is an executable file — `access(path, X_OK) == 0`.
bool systemExecutableExists(const char* path);

/// Writes `buffer` to a uniquely named temp file.
///
/// The file is created under `directoryHint`, or the system temp directory when
/// that is null or empty, with a name ending in `suffix`.
///
/// - Parameters:
///   - directoryHint: Where to create the file, or the system temp directory
///     when null or empty.
///   - suffix: What the generated name ends in.
///   - buffer: The bytes to write.
///   - pathOut: Receives the NUL-terminated path of the file created, and
///     bounds it: a path that does not fit is a refusal rather than a truncation.
/// - Returns: True on success. The caller removes the file through
///   ``storageRemoveFile``.
bool storageWriteTempFile(const char* directoryHint, const char* suffix, std::span<const std::byte> buffer, std::span<char> pathOut);

/// Writes the canonical path of an already-open descriptor.
///
/// For a security check that has to name the object it is about to read rather
/// than the name it was asked for. Between resolving a path and opening it, the
/// name can be made to point somewhere else; the descriptor cannot. So a gate
/// that re-asks about *this* descriptor's own path is race-free in a way one
/// asking about the caller's string is not — which is what the kitty graphics
/// file transport does before it reads a file an escape sequence named.
///
/// Refuses a descriptor whose path no longer names it. The two platforms need
/// different amounts of help with that, and the difference is measured rather
/// than assumed: Darwin's `F_GETPATH` gives the last known path with no marker,
/// so once the name is gone it resolves cleanly to whatever holds it now — a
/// different file — and only comparing device and inode refuses that. Linux
/// answers through `/proc/self/fd`, which marks the dentry: unlinking the opened
/// name appends " (deleted)" whether or not the inode keeps other links, so the
/// stale path stops resolving and the refusal happens a step earlier.
///
/// The identity comparison runs on both legs regardless. One contract is worth
/// more than skipping a `stat` on the platform that currently does not need it.
///
/// - Parameters:
///   - descriptor: An open file descriptor.
///   - outputBuffer: Receives the NUL-terminated canonical path.
///   - outputCapacity: How many bytes `outputBuffer` holds. A path that does
///     not fit is a refusal rather than a truncation — a truncated path names a
///     different file, and a directory at that.
/// - Returns: The path length written, or -1 when the descriptor has no
///   resolvable path, when it no longer names the file it did, or on any error.
int fileCanonicalPathForDescriptor(int descriptor, char* outputBuffer, int outputCapacity);

/// Spawns `path` with the null-terminated `argv`, inheriting the current
/// environment, and waits for it.
///
/// - Returns: The child's exit code on a normal exit; -1 when the spawn failed
///   or the child did not exit normally.
int processSpawnAndWait(const char* path, const char* const argv[]);

/// The platform's default directory for received files, or an empty string
/// when the platform has none to name.
///
/// In-band file transfer saves where the user pointed it by default; the
/// frontend offers this before any file transfer runs, and the user's choice
/// (accepted or overridden) is what the transfer runs under.
///
/// Empty is a contract, not a failure: it says this platform delegates the
/// destination to the frontend's own picker, which is then the storage
/// handler — a sandboxed platform has no ambient folder a bare path could
/// name. A caller must not substitute a spelling of its own for an empty
/// answer; a hardcoded "~/Downloads" is a promise the platform never made.
///
/// - Returns: The platform default download directory (e.g. ~/Downloads on
///   macOS, $HOME/Downloads on Linux), or empty to route the choice to the
///   frontend's picker.
std::string downloadsDirectoryDefaultPath();

/// Spawns `path` with `argv` and the current environment, binding the child's
/// standard streams to `stdioDescriptor` — one already-open descriptor the
/// caller keeps alive for the call's duration — and waits for it.
///
/// A ZMODEM transfer agent runs against a PTY master: its stdin and stdout
/// are the terminal's data stream. Standard error goes to the null device so
/// agent progress chatter cannot corrupt it.
///
/// A realization without a host process table reports -1: the web's agent is
/// an in-process protocol codec, not a spawned binary, so a future web agent
/// speaks on the descriptor through the same caller without this hook.
///
/// The descriptor usually comes from a pollable endpoint, which opened it
/// nonblocking — and an agent assumes a blocking terminal stdin, treating an
/// EAGAIN on its first read as fatal. O_NONBLOCK is per open-file-description,
/// so a dup cannot decouple the child; the implementation clears the flag for
/// the child and restores it before returning (the caller keeps ingest stopped
/// for the whole call, so nothing else reads the descriptor meanwhile). The
/// terminal is likewise put in raw mode for the child and restored after, so
/// the line discipline neither echoes the handshake back into the stream nor
/// canonical-buffers it; the caller's shell reinitializes its own mode on the
/// way back anyway. Neither change discards a byte queued in either direction:
/// input typed ahead of the agent and output the far side wrote past its last
/// read are still queued when the caller reads on. A call that fails after
/// either change hands both back, and the child inherits the descriptor and
/// nothing else the caller holds.
///
/// - Parameters:
///   - path: The executable's absolute path.
///   - argv: Null-terminated argument vector, starting with the executable's
///     path as convention requires.
///   - workingDirectory: The child's working directory, or an empty string to
///     inherit the current one.
///   - stdioDescriptor: The open descriptor bound to the child's standard
///     input and output; standard error goes to the null device.
///   - limitSeconds: How long the child may run before it is terminated and
///     reaped; zero or less waits without limit. A transfer agent is a
///     third-party binary that can stop inside its own terminal setup and never
///     return, and this call is synchronous with the caller's session stopped —
///     so an unlimited wait is a session that never comes back. Nothing
///     available to this side separates a wedged agent from a slow one, so the
///     limit is a wall clock rather than a judgement, and a caller should pick
///     one longer than the slowest transfer it means to allow can take.
/// - Returns: The child's exit code on a normal exit; -1 when the spawn failed,
///   the child did not exit normally, or the limit terminated it.
int processSpawnWithTerminalAndWait(const char* path, const char* const argv[], const char* workingDirectory, int stdioDescriptor, double limitSeconds);

// ---- Window / event-loop callbacks -------------------------------
//
// The hooks Core fires up to the frontend. Their setters live here in
// Ubiquitty::PAL; the storage and dispatch getters live in Source/PAL/PAL.cpp.
//
// Every hook type below takes a `Ubiquitty::Terminal*`, opaque as far as this
// contract is concerned — Core owns the struct's shape. PAL.h forward-declares
// it below; the full definition lives in Core/TerminalOperations.h and is needed only by the
// implementations, which are Core's dispatchers and the backends themselves.

} // namespace Ubiquitty::PAL

// Keep PAL.h dependency-light by naming the terminal model without including
// its full definition.
namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::PAL {

// Resize (XTWINOPS op 8), DECCOLM relayout, and the window mutators (ops 1..7,
// 9, 10) route through Terminal::windowClient — WindowClient::resize,
// setColumnMode and performWindowOperation — not PAL.

// ---- OSC dispatch hooks ------------------------------------------
//
// Most OSCs never reach PAL: 52 routes through Terminal::clipboardClient, 7
// (currentDirectory) through Terminal::shellClient, and the color OSCs
// (4/10/11/12/110/111/112) mutate Core's Terminal::palette directly.

/// Installs a selection-color override — OSC 17 for background, 19 for foreground.
///
/// The OSC dispatcher parses the wire color spec (`rgb:...`, `#...`) and routes
/// the set through this hook; the selection paint override itself lives in the
/// application layer. Channel values are 16-bit, the xterm convention, matching
/// the OSC 4/10/11/12 hooks.
using SelectionColorSetHandler = void (*)(Ubiquitty::Terminal* t, uint16_t r16, uint16_t g16, uint16_t b16);

/// Clears a selection-color override — OSC 117 for background, 119 for foreground.
using SelectionColorResetHandler = void (*)(Ubiquitty::Terminal* t);

/// The installed OSC 17 handler, read from the installed ``Context``.
///
/// Optional, and unset in every shipping frontend today. Where it is null the
/// OSC is still parsed and consumed cleanly and the value is still stored on the
/// terminal, so the OSC 17 `?` query can answer — only the application callback
/// is skipped. The same holds for the three getters below.
SelectionColorSetHandler selectionBackgroundSetter();

/// The installed OSC 117 handler, read from the installed ``Context``.
SelectionColorResetHandler selectionBackgroundResetter();

/// The installed OSC 19 handler, read from the installed ``Context``.
SelectionColorSetHandler selectionForegroundSetter();

/// The installed OSC 119 handler, read from the installed ``Context``.
SelectionColorResetHandler selectionForegroundResetter();

// ---- Inline-image decode (OSC 1337 File=) ------------------------

/// Decodes an encoded image container — PNG, JPEG, GIF and the rest — to
/// tightly packed 8-bit RGBA.
///
/// A decoder returns false for a size `Image::isDecodedImageBytesRefused`
/// refuses, before it allocates the pixels, reading the size from the
/// container's header where it can. Core checks the size handed back as well,
/// but only once the decoder has allocated it (`Core/Image/TerminalImage.h`).
///
/// - Returns: True on success, with a `malloc`'d buffer in `rgbaOut` that the
///   caller releases with `std::free`, and the pixel dimensions in `widthOut`
///   and `heightOut`.
using ImageDecodeCallback = bool (*)(std::span<const std::uint8_t> bytes, uint8_t** rgbaOut, int* widthOut, int* heightOut);

/// The installed image decoder, read from the installed ``Context``.
///
/// Optional, and absent on Linux and the host build, where OSC 1337 and Kitty
/// `f=100` images are parsed and consumed but not rendered.
///
/// - Important: The Kitty path reads this on a decode worker thread, so the
///   context must be installed before any decode job is submitted.
ImageDecodeCallback imageDecoder();

// ---- DECPS play-sound (VT520 CSI Pv;Pd;Pn...,~) ------------------

/// One play-sound request: the VT520's parameters and the sub-parameters
/// Ubiquitty adds to them.
///
/// `freqs` holds `count` note frequencies in Hz, 0 a rest, sounded one after
/// another for `durationMilliseconds` each; DECPS is sequential, not a chord.
/// `volume` runs 0 to 7. `wave` picks the voice: 0 the square wave of the
/// VT520's speaker, 1 sine, 2 triangle, 3 sawtooth, 4 noise, 5 a drum whose
/// pitch falls through the note. `channel` 0 is the terminal's speaker and 1
/// to 8 are the tracker's; a request queues behind the channel's playing
/// sequence, so consecutive requests join without a gap, and a request at
/// volume 0 stops its channel and empties its queue, on channel 0 every
/// channel. `attackMilliseconds` and `releaseMilliseconds` shape each note,
/// `waveDefaultEnvelope` meaning the voice's own; `gapMilliseconds` is silence
/// at each note's end; `semitonesDown` transposes the sequence below the
/// VT520's range; a note's `noteVolumes` entry, 1 to 7, overrides `volume` for
/// that note. Wave 6 is the sample voice: every sounding note replays the slot
/// `sampleSlot` names, whose PCM, mono signed-16 at 44.1 kHz, rides
/// `samplePCM` for the call; a host forwarding the request sends the slot and
/// the window supplies the PCM from its own bank. `decayMilliseconds` and
/// `sustainLevel` (0 to 7 of the peak) complete the envelope. A note's
/// `noteEffects` entry is a tracker effect in the XM numbering, its
/// `noteParameters` entry the effect's byte: 0 arpeggio, 1 and 2 portamento
/// up and down, 3 tone portamento, 4 vibrato, 7 tremolo, 9 sample offset,
/// 10 volume slide, 12 set volume, 14 the extended set (9 retrigger, 12 note
/// cut, 13 note delay); effect 0 with byte 0 is none. Fixed arrays, so a
/// request is a plain value the seam copies whole.
struct PlayTonesRequest {
    static constexpr int maxNotes = 16;
    static constexpr uint16_t waveDefaultEnvelope = 0xFFFF;
    uint8_t volume = 0;
    uint8_t wave = 0;
    uint8_t channel = 0;
    uint16_t attackMilliseconds = waveDefaultEnvelope;
    uint16_t releaseMilliseconds = waveDefaultEnvelope;
    uint8_t semitonesDown = 0;
    uint32_t durationMilliseconds = 0;
    uint32_t gapMilliseconds = 0;
    std::array<uint32_t, maxNotes> freqs = {};
    std::array<uint8_t, maxNotes> noteVolumes = {};
    std::array<uint8_t, maxNotes> noteEffects = {};
    std::array<uint8_t, maxNotes> noteParameters = {};
    int count = 0;
    uint16_t decayMilliseconds = 0;
    uint8_t sustainLevel = 7;
    // The request's send to the shared hall, 0 to 7, and the grid it starts
    // on, in 1/32 s (0 = none): Core/VT/DECPSSynth.h places it.
    uint8_t reverbSend = 0;
    uint8_t gridUnits = 0;
    // The request's filters, cutoffs in hertz; 0 leaves the voice unfiltered.
    uint16_t lowpassHertz = 0;
    uint16_t highpassHertz = 0;
    uint8_t sampleSlot = 0;
    // Where the request sits across the stereo field: 0 hard left, 64 hard
    // right, 32 the centre a request that names no pan keeps. Centre is both
    // sides at full gain, so naming no pan costs no loudness.
    uint8_t pan = 32;
    // How many more times the request plays after its first pass: 0 plays it
    // once, `repeatForever` until the channel is stopped. A loop lives in the
    // synth so background music is one request rather than a program that must
    // stay alive to feed the channel, and gaps whenever it is busy.
    static constexpr uint8_t repeatForever = 255;
    uint8_t repeat = 0;
    const int16_t* samplePCM = nullptr;
    uint32_t sampleFrames = 0;
};

/// What a frontend's synth can do with a request right now.
///
/// A program that wants a chime has no way to learn that the person will not
/// hear it: a terminal with no synth, and one whose user turned sound off, both
/// consume the request in silence. Reported so a caller can fall back to
/// something visible rather than believing it made a noise.
struct SoundStatus {
    bool hasSynth = false; ///< A frontend installed a synth at all.
    bool isAudible = false; ///< Sound is on and above zero volume.
    uint8_t volumePercent = 0; ///< The user's master sound volume, 0 to 100.
};

/// Reports what the installed synth would do with a request.
using SoundStatusCallback = SoundStatus (*)();

/// Whether a frontend's synth may sound at all, as the user's setting leaves it.
enum class SoundOutput : uint8_t {
    Silenced, ///< Make no sound, and stop what is already sounding.
    Audible, ///< Play what a program asks for, at the user's level.
};

/// Plays a sequence of tones, or at volume 0 stops a channel.
using PlayTonesCallback = void (*)(const PlayTonesRequest& request);

/// The installed sound-status reporter, read from the installed ``Context``.
///
/// Optional, like the synthesizer beside it: a null reporter is answered as
/// "no synth", which is the truthful report for a frontend that installed none.
///
/// - Returns: The reporter, or null when no frontend installed one.
SoundStatusCallback soundStatus();

/// The installed tone synthesizer, read from the installed ``Context``.
///
/// Optional, following the image-decoder pattern: null by default, so DECPS is
/// parsed and consumed but silent until a frontend installs a synthesizer. macOS
/// installs an AudioToolbox output fed by Core's tone mixer at startup, and a
/// host test installs a recorder to assert the parsed arguments. Absent on
/// Linux and the host build.
PlayTonesCallback playTones();

// ---- Process-wide platform-service context -----------------------

/// The platform services a frontend supplies to Core.
///
/// The four hook groups gathered here — clock, selection colors, image decode
/// and DECPS tones — are stateless process-wide facilities. There is one clock,
/// one image decoder and one synthesizer per process, and the selection-color
/// source holds no per-terminal state, since it takes the terminal as an
/// argument. They are installed at frontend startup and never mutated after, so
/// they live in one immutable context behind a synchronized-once accessor
/// rather than in mutable file-scope globals (Docs/coding-conventions.md §5.1).
///
/// Immutability is what makes every read race-free by construction, including
/// the Kitty image decode that runs off the main thread. Per-terminal injection
/// is reserved for a hook that genuinely holds per-terminal state; none of these
/// do.
struct Context {
    /// Required. A real frontend must supply the monotonic clock, and
    /// ``installContext`` aborts when it is null rather than risk a later
    /// degraded read.
    MonotonicClockHook monotonicClock = nullptr;

    /// Optional. Null is a valid "feature absent", checked at each read site.
    ImageDecodeCallback imageDecoder = nullptr;
    PlayTonesCallback playTones = nullptr;
    SoundStatusCallback soundStatus = nullptr;
    SelectionColorSetHandler selectionBackgroundSet = nullptr;
    SelectionColorResetHandler selectionBackgroundReset = nullptr;
    SelectionColorSetHandler selectionForegroundSet = nullptr;
    SelectionColorResetHandler selectionForegroundReset = nullptr;

    /// Whether every required hook is present. ``installContext`` enforces it.
    [[nodiscard]] bool hasRequiredHooks() const { return monotonicClock != nullptr; }
};

/// Installs the process context, once, at frontend startup.
///
/// Copies `c` into process-lifetime storage, which the getters above read.
///
/// - Important: Install before any worker thread — PTY read, render, or the
///   decode queue — can read a hook. That ordering is the race invariant.
/// - Note: Aborts when a required hook is absent, and again when a context is
///   already installed: workers read the hooks unsynchronized, so a second
///   install would race a reader. Tests swap through ``ScopedContext``.
void installContext(const Context& c);

/// The installed context, or an all-null context before one is installed, so a
/// Core translation unit linked without a frontend degrades rather than crashes.
const Context& context();

/// A scope-bound context override, for tests.
///
/// The only sanctioned way to swap the active context, and it restores the prior
/// one on scope exit, so it cannot leak a mutation the way a public setter
/// would. Production installs use ``installContext`` instead.
///
/// - Note: Aborts when `c` lacks a required hook.
class ScopedContext {
public:
    explicit ScopedContext(const Context& c);
    ~ScopedContext();
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    Context m_context; // owns the swapped-in context for the scope's lifetime
    const Context* m_prior;
};

// Two more paths reach the host without PAL, because each needs per-Terminal
// dispatch rather than one process-wide hook: OSC 9 / 99 / 777 desktop
// notifications route through Terminal::shellClient (ShellClient::postNotification),
// and Kitty graphics t=f / t=t file transport through Terminal::fileReadClient
// (FileReadClient::readFile).

} // namespace Ubiquitty::PAL
