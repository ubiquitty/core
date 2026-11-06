// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/CheckedPtr.h"
#include "Base/Types.h"
#include "PAL/Capability.h"
#include "PAL/MousePointerShape.h"
#include "PAL/TextShaping.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Per-Terminal frontend clients: platform primitives Core invokes. Each Terminal
// holds a CheckedPtr to whichever its frontend implements; unset means Core
// no-ops. CanMakeCheckedPtr traps a client outliving its Terminal.

namespace Ubiquitty {

struct Terminal;

/// How a hosted OSC 66 query proceeds while its driving faces are owed.
enum class ProportionalQueryDisposition : std::uint8_t {
    Answer,
    Defer,
    Grid,
};

/// OSC 1337 RequestAttention: bounce the Dock once, sustain it until the window
/// is focused, or cancel a pending bounce.
enum class AttentionRequest {
    Once,
    Continuous,
    Cancel
};

/// Linux-console power / display management (console_codes(4) CSI ... ] family).
/// Active = normal; Blanked = the app asked for the screen to go dark (DPMS-off
/// equivalent — the frontend blanks content and inhibits display sleep).
enum class DisplayPowerState : uint8_t {
    Active,
    Blanked
};

/// DECSET 7402 fullscreen-content fit, selected by OSC 7402. Contain = aspect-
/// preserving contain-fit (default). Stretch = fit a box of the app-declared W:H
/// aspect and stretch to fill. Native = source's native size at the largest
/// integer scale that fits, centered, nearest sampler. Scale = an app-set explicit
/// source-px -> device-px integer multiplier (scaleMultiplier), centered, nearest.
enum class FullscreenContentFit : uint8_t {
    Contain,
    Stretch,
    Native,
    Scale
};
/// Letterbox fill in the residual bars. Color = occlude with letterboxColor (solid,
/// terminal glyphs suppressed; default black). Passthrough = leave terminal content
/// visible around the image.
enum class FullscreenContentLetterbox : uint8_t {
    Color,
    Passthrough
};

// Complete app-selected fullscreen-content (DECSET 7402) display state, carried by
// one OSC 7402. Each OSC fully re-specifies it: named params override, unnamed
// take these defaults (Contain fit, black-occlude letterbox).
struct FullscreenContentDisplay {
    FullscreenContentFit fit { FullscreenContentFit::Contain };
    uint16_t aspectWidth { 0 }; // target display W for Stretch (0 for Contain/Native)
    uint16_t aspectHeight { 0 }; // target display H for Stretch
    uint16_t scaleMultiplier { 0 }; // source-px -> device-px integer multiplier for Scale (0 otherwise)
    FullscreenContentLetterbox letterbox { FullscreenContentLetterbox::Color };
    uint32_t letterboxColor { 0 }; // 0xRRGGBB, used when letterbox == Color (default black)
};

// XTWINOPS / DECCOLM / resize; invalidate() requests a repaint.
class WindowClient : public CanMakeCheckedPtr {
public:
    virtual ~WindowClient() = default;
    // Rows first, the order `CSI 8 ; rows ; columns t` writes them and the
    // order `ioctl TIOCSWINSZ` puts `ws_row` in. Named because unnamed they
    // told an implementer nothing, and this repo carries both orderings by
    // domain — the grid side counts rows first, the transport side counts
    // columns first — so which one a seam uses cannot be guessed from habit.
    virtual void resize(int /*rows*/, int /*columns*/) { }
    virtual void setColumnMode(bool /*isColumns132*/) { }
    // The `Ps` of a CSI t, then its two sub-parameters as the program wrote
    // them: -1 for an omitted axis, 0 for present-zero.
    virtual void performWindowOperation(int /*operation*/, int /*first*/, int /*second*/) { }
    virtual void requestAttention(AttentionRequest) { }
    // OSC 1337 StealFocus: unconditionally activate app + window (unlike
    // requestAttention, which only nudges the Dock). Unset ⇒ Core no-ops.
    virtual void stealFocus() { }
    // OSC 22: set the live mouse-pointer shape. Core owns the push/pop stack and
    // reports only the resolved top shape. Unset ⇒ Core no-ops.
    virtual void setMousePointerShape(MousePointerShape) { }
    // OSC 50: live font swap (xterm/rxvt). Core hands a resolved request; the
    // frontend re-metrics and resizes to keep columns×rows. Three axes:
    //   - `family` non-empty ⇒ that family; empty ⇒ keep current (or, when
    //     sizePoints/sizeDelta are also zero, reset to the config default).
    //   - `sizePoints` > 0 ⇒ absolute point size; ignored otherwise.
    //   - `sizeDelta` != 0 ⇒ relative bump on the frontend's current size.
    // True admits the request; false refuses it without changing query state.
    // An unavailable font keeps the current font. An unsupported client refuses.
    [[nodiscard]] virtual bool setFont(std::string_view /*family*/, double /*sizePoints*/, double /*sizeDelta*/) { return false; }
    // mintty's empty OSC 7770 / 7777 operand: the configured size, keeping the
    // family drawn, as a reset of the size alone. The frontend keeps
    // columns×rows as for setFont. True admits the request; false refuses it.
    // An unsupported client refuses.
    [[nodiscard]] virtual bool resetFontSize() { return false; }
    // OSC 1337 SetColors=preset=<name>: switch the live theme by name; unknown
    // name is a no-op. Unset ⇒ Core no-ops.
    virtual void selectColorPreset(std::string_view /*name*/) { }
    // Linux-console DPMS / screen-blank (console_codes CSI 9/13/14 ]). CRT degauss
    // / backlight have no display analogue, so Core acks them as no-ops and never
    // calls this for them. Unset ⇒ Core no-ops.
    virtual void setDisplayPower(DisplayPowerState) { }
    // Password input mode (private mode 7401 / config heuristic): frontend shows a
    // lock indicator and masks local echo while active. Unset ⇒ Core no-ops (flag
    // still round-trips for DECRQM).
    virtual void setPasswordPromptActive(bool) { }
    // Edge-to-edge "fullscreen content" (private mode 7402): frontend zeroes cell
    // padding, letterboxes the drawable, and contain-fits a Kitty image. Unset ⇒
    // Core no-ops (flag still round-trips for DECRQM).
    virtual void setEdgeToEdge(bool) { }
    // App-selected display controls for edge-to-edge (OSC 7402). Applied only while
    // edge-to-edge is active. Unset ⇒ Core no-ops (field still round-trips).
    virtual void setFullscreenContentDisplay(const FullscreenContentDisplay&) { }
    /// Receives one decoded OSC 9;4 report, including repeated reports within a feed.
    ///
    /// Recording this metadata grants no permission to update an external taskbar or Dock.
    ///
    /// - Parameters:
    ///   - state: The decoded state, 0..4.
    ///   - value: The percentage, 0..100, or -1 when omitted.
    virtual void reportProgress(uint8_t /*state*/, int /*value*/) { }
    virtual void invalidate() { }
};

// OSC 52 clipboard buffer selector. Core maps each parsed selector char
// (c/p/q/s/0-7) to one of these, each routing to its own store. `Clipboard` is
// the only buffer bridged to the system pasteboard; the rest are app-internal.
// Order is required: `Cut0`-`Cut7` are contiguous so `Cut0 + n` indexes cut
// buffer n. Count is the array sizer.
enum class ClipboardBuffer : uint8_t {
    Clipboard = 0, // c — the system pasteboard
    Primary, // p
    Secondary, // q
    Select, // s
    Cut0, // 0
    Cut1, // 1
    Cut2, // 2
    Cut3, // 3
    Cut4, // 4
    Cut5, // 5
    Cut6, // 6
    Cut7, // 7
    Count
};

/// Whether a Core-initiated clipboard read must be confirmed by the user
/// (the Terminal's read policy is Prompt) or the frontend should auto-approve
/// (Allow) — see ClipboardClient::requestClipboardRead.
enum class ClipboardReadConfirmation {
    NotRequired,
    Required
};

/// A Terminal whose contents mirror another's — an attached client's screen,
/// kept in step by frames the far end computes.
///
/// Each frame is a difference against the screen the far end believes this one
/// holds, so anything that rewrites the grid locally invalidates every later
/// frame. Sequence numbers cannot detect that: none was missed, so the
/// difference applies cleanly to the wrong content and nothing reports a fault.
///
/// Core calls this whenever it moves rows out from under such a grid, letting
/// the mirror re-baseline rather than drift. Null for a Terminal that is
/// nobody's mirror, which is the common case.
class MirrorClient : public CanMakeCheckedPtr {
public:
    /// Destroys the client. The frontend owns it and outlives the Terminal.
    virtual ~MirrorClient() = default;

    /// Declares that this grid no longer matches what the far end believes it
    /// shows, so the next difference frame must not be applied to it.
    virtual void invalidateBaseline() { }
};

/// The clock one terminal reads, on the 60 Hz tick scale.
///
/// Core reads time while it parses — an OSC 133 command's start and end — and
/// a frontend reads it for the cursor's blink phase and activity hold. Read
/// through the terminal rather than a process-wide hook, a replay, a test or a
/// replica parsing the same bytes later gives the terminal the instant the
/// bytes were written, and two terminals never have to agree on one clock.
/// Unset, a terminal reads `PAL::monotonicClock`.
class TerminalClock : public CanMakeCheckedPtr {
public:
    /// Destroys the clock. The frontend owns it and outlives the Terminal.
    virtual ~TerminalClock() = default;

    /// Returns the terminal's present instant.
    ///
    /// - Returns: Ticks on the 60 Hz clock; the counter wraps, so a caller
    ///   compares instants with `Ticks::since`.
    virtual Ticks now() = 0;
};

/// Where the effects a terminal's program causes are delivered, one route per
/// terminal.
///
/// Two panes in one process — two sessions in one daemon, two splits in one
/// window — must not share one effect sink: whatever plays a bell, badges it or
/// carries it to a client has to know which pane rang. So Core delivers each
/// effect to the route of the terminal that caused it, never to a process-wide
/// hook. `PAL::bellRing` is what a route plays a bell through, not where Core
/// rings it.
///
/// Called on the thread that parses, inside the parse: a route records the
/// effect and returns, and anything that touches a user interface hops to that
/// interface's thread first.
class EffectRoute : public CanMakeCheckedPtr {
public:
    /// Destroys the route. The frontend owns it and outlives the Terminal.
    virtual ~EffectRoute() = default;

    /// Receives one bell the program rang.
    ///
    /// The terminal's `bellPending` is set as well, so a frontend that resolves
    /// its bell on the drain through `resolveBellEvent` keeps its coalescer and
    /// preferences; the route is where the event goes, not what plays it.
    virtual void ringBell() { }
};

/// One typed clipboard payload: a MIME name and its bytes.
///
/// Views, not owners — an item borrows the write session's accumulation for
/// the length of the call that passes it.
struct ClipboardItem {
    std::string_view mime;
    std::span<const char> bytes;
};

/// Whether a MIME names text/plain, parameters ignored.
///
/// Here rather than in a handler because the seam's own defaults key on it:
/// the untyped client stores exactly one type, and both sides of the seam
/// must agree on which requests that type answers.
///
/// - Parameter mime: The MIME name, with or without parameters.
/// - Returns: Whether the bare type is text/plain, caselessly.
/// Whether two MIME names name the same type.
///
/// Parameters are part of the name on the wire but not of the match, and case
/// is not either: `IMAGE/PNG` and `image/png;base64` name one payload. Both
/// sides are stripped and folded, so neither argument has to arrive bare.
///
/// One function because the rule had grown three copies — this file's
/// text/plain test, the macOS pasteboard's lookup, and the browser page's —
/// and a client that matched a name another client missed would answer "no
/// such type" for a clipboard that holds it.
///
/// - Parameters:
///   - a: One name.
///   - b: The other.
/// - Returns: Whether they name the same type.
[[nodiscard]] inline bool mimeNamesSameType(std::string_view a, std::string_view b)
{
    const auto bare = [](std::string_view mime) {
        const std::size_t semicolon = mime.find(';');
        return mime.substr(0, semicolon == std::string_view::npos ? mime.size() : semicolon);
    };
    const std::string_view left = bare(a);
    const std::string_view right = bare(b);
    if (left.size() != right.size())
        return false;
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (fold(left[i]) != fold(right[i]))
            return false;
    }
    return true;
}

/// Whether a MIME names text/plain, by ``mimeNamesSameType``.
///
/// - Parameter mime: The name to test.
/// - Returns: Whether it names plain text.
[[nodiscard]] inline bool mimeIsTextPlain(std::string_view mime) { return mimeNamesSameType(mime, "text/plain"); }

/// A typed capture staged for a deferred clipboard read.
///
/// The value an asynchronous client's approval stages and its later
/// ``ClipboardClient/readClipboardTypes`` and
/// ``ClipboardClient/readClipboardItem`` calls serve from. Single-threaded
/// by contract: serve on the thread that stages, or wrap the calls in the
/// client's own lock. The capture persists until replaced, and staging an
/// empty set clears it — a source with no typed content falls back to the
/// untyped bytes.
class ClipboardReadItemStaging {
public:
    /// Stages a capture, replacing the previous one.
    ///
    /// - Parameter items: The capture, copied and owned; empty clears.
    void stage(std::span<const ClipboardItem> items)
    {
        m_items.clear();
        m_items.reserve(items.size());
        for (const ClipboardItem& item : items)
            m_items.push_back({ std::string { item.mime }, std::vector<char>(item.bytes.begin(), item.bytes.end()) });
    }

    /// The staged MIME names, space-separated — the seam's type-list shape.
    ///
    /// - Parameter out: Receives the list; a longer list names what fits,
    ///   never a torn name.
    /// - Returns: The count written, or -1 with nothing staged.
    [[nodiscard]] int serveTypes(std::span<char> out) const
    {
        if (m_items.empty())
            return -1;
        std::size_t length = 0;
        for (const StagedItem& item : m_items) {
            const std::size_t needed = item.mime.size() + (length > 0 ? 1 : 0);
            if (length + needed > out.size())
                break;
            if (length > 0)
                out[length++] = ' ';
            for (char c : item.mime)
                out[length++] = c;
        }
        return static_cast<int>(length);
    }

    /// One staged payload's bytes, matched caselessly by bare type.
    ///
    /// - Parameters:
    ///   - mime: The requested name, parameters ignored.
    ///   - out: Receives the bytes, bounded by the span.
    /// - Returns: The count, or -1 when no staged name matches.
    [[nodiscard]] int serveItem(std::string_view mime, std::span<char> out) const
    {
        const std::string_view bare = bareType(mime);
        for (const StagedItem& item : m_items) {
            if (!equalsCaseless(bareType(item.mime), bare))
                continue;
            const std::size_t n = item.bytes.size() < out.size() ? item.bytes.size() : out.size();
            for (std::size_t i = 0; i < n; ++i)
                out[i] = item.bytes[i];
            return static_cast<int>(n);
        }
        return -1;
    }

private:
    /// One staged payload, owned: the capture outlives the caller's views.
    struct StagedItem {
        std::string mime;
        std::vector<char> bytes;
    };

    [[nodiscard]] static std::string_view bareType(std::string_view mime)
    {
        const std::size_t semicolon = mime.find(';');
        return mime.substr(0, semicolon == std::string_view::npos ? mime.size() : semicolon);
    }

    [[nodiscard]] static bool equalsCaseless(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const char ca = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
            const char cb = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
            if (ca != cb)
                return false;
        }
        return true;
    }

    std::vector<StagedItem> m_items;
};

// OSC 52 and OSC 5522.
class ClipboardClient : public CanMakeCheckedPtr {
public:
    virtual ~ClipboardClient() = default;

    /// Writes a typed set to the named buffer.
    ///
    /// The default adapts to the untyped method below: the first text/plain
    /// item is the whole write, which is exactly what a text-only frontend
    /// stored before types existed — so a frontend that overrides nothing
    /// keeps its behaviour, and one that overrides this stores every item.
    ///
    /// - Parameters:
    ///   - buffer: The destination buffer.
    ///   - items: The payloads, one per MIME. Empty clears the buffer.
    virtual void writeClipboardItems(ClipboardBuffer buffer, std::span<const ClipboardItem> items)
    {
        for (const ClipboardItem& item : items) {
            if (mimeIsTextPlain(item.mime)) {
                writeClipboard(buffer, item.bytes);
                return;
            }
        }
        writeClipboard(buffer, {});
    }

    /// The MIME names this buffer can serve, space-separated.
    ///
    /// -1 declares an untyped client — Core then serves text/plain through
    /// ``readClipboard`` and nothing else — which is what the default
    /// declares, so a frontend opts into types by answering. An asynchronous
    /// client answers from what it staged, like ``readClipboard`` does.
    ///
    /// - Parameters:
    ///   - buffer: The buffer whose types are asked.
    ///   - out: Receives the space-separated list.
    /// - Returns: The count written, 0 for an empty buffer, or -1 for an
    ///   untyped client.
    [[nodiscard]] virtual int readClipboardTypes(ClipboardBuffer /*buffer*/, std::span<char> /*out*/) { return -1; }

    /// The bytes of one MIME's payload.
    ///
    /// The default serves text/plain from the untyped ``readClipboard`` and
    /// refuses every other name, mirroring what the untyped client can hold.
    ///
    /// - Parameters:
    ///   - buffer: The buffer to read.
    ///   - mime: The requested MIME name.
    ///   - out: Receives the bytes.
    /// - Returns: The count, or -1 when the name is not served.
    [[nodiscard]] virtual int readClipboardItem(ClipboardBuffer buffer, std::string_view mime, std::span<char> out)
    {
        if (!mimeIsTextPlain(mime))
            return -1;
        return readClipboard(buffer, out);
    }
    // Write `data` to the named buffer. Core calls this once per distinct selector
    // in the OSC 52 set. An empty span clears it.
    virtual void writeClipboard(ClipboardBuffer /*buffer*/, std::span<const char>) { }
    // Fills `out` with the bytes most recently staged by provideClipboardReadBytes
    // and returns the count, or -1 if none. Core calls it on the ingest thread,
    // where touching the platform pasteboard directly would be unsafe.
    [[nodiscard]] virtual int readClipboard(ClipboardBuffer /*buffer*/, std::span<char>) { return 0; }
    // Stage approved clipboard bytes for the next readClipboard. Called by the
    // frontend on its UI thread once a read is approved.
    virtual void provideClipboardReadBytes(ClipboardBuffer /*buffer*/, std::span<const char>) { }
    /// Stages an approved typed capture, one item per MIME.
    ///
    /// The typed sibling of ``provideClipboardReadBytes``: an asynchronous
    /// typed client captures on its UI thread and serves Core's later
    /// ``readClipboardTypes`` and ``readClipboardItem`` calls from what was
    /// staged here. The default drops the set — an untyped client serves
    /// bytes alone.
    ///
    /// - Parameters:
    ///   - buffer: The buffer the capture belongs to.
    ///   - items: The capture; empty clears the typed staging, so a capture
    ///     of a source with no typed content falls back to the staged bytes.
    virtual void provideClipboardReadItems(ClipboardBuffer /*buffer*/, std::span<const ClipboardItem> /*items*/) { }
    // True when readClipboard can't run inline on the ingest thread (must capture
    // the pasteboard on a UI thread). Then Core routes even Allow through
    // requestClipboardRead to stage bytes first; false (default) reads inline.
    [[nodiscard]] virtual bool requiresAsynchronousClipboardRead() { return false; }

    // Clipboard-read request from Core, on the ingest thread. `confirmation` is
    // Required when the read policy is Prompt (ask the user), NotRequired for Allow
    // (auto-approve, still async because pasteboard capture needs the main thread).
    // The frontend marshals back and calls OSC::completeClipboardReadQuery. Returns
    // true when accepted for async handling; false (default) ⇒ Core answers the
    // query itself with an empty reply, so the sender is not left waiting.
    //
    // Core calls this for at most one query per Terminal at a time and answers the
    // rest empty, so an implementation need not defend against a flood: whatever it
    // allocates here, and whatever UI it raises, exists once until it completes.
    [[nodiscard]] virtual bool requestClipboardRead(ClipboardBuffer /*buffer*/, std::string_view /*setSpec*/, ClipboardReadConfirmation /*confirmation*/) { return false; }

    /// Captures this machine's clipboard for a program on a remote host, now.
    ///
    /// A separate call from ``readClipboardTypes`` and ``readClipboardItem``,
    /// which serve a capture some earlier approval staged. This one asks for a
    /// fresh one, and Core makes it on the frontend's own loop — the same
    /// thread the frontend posted the session work to — so touching the
    /// platform pasteboard is safe here where it is not on ingest.
    ///
    /// The capture goes to `sink` rather than out through a return value
    /// because ``ClipboardItem`` is a pair of views: an implementation holds
    /// the bytes in its own storage for the duration of the call and Core
    /// copies what it needs before returning.
    ///
    /// The default captures nothing and answers false, so a frontend that has
    /// not implemented it refuses the host's program rather than telling it the
    /// clipboard is empty — a program can act on a refusal and cannot detect a
    /// wrong answer.
    ///
    /// - Parameters:
    ///   - buffer: The buffer to capture.
    ///   - sink: Receives the capture, valid only for the call.
    /// - Returns: Whether a capture was made at all.
    [[nodiscard]] virtual bool captureClipboardForRemote(
        ClipboardBuffer /*buffer*/, const std::function<void(std::span<const ClipboardItem>)>& /*sink*/)
    {
        return false;
    }
};

/// When a program wants its notification shown, as kitty's OSC 99 `o=` states it.
enum class NotificationOccasion : std::uint8_t {
    /// Whenever the terminal would show one.
    Always,
    /// Only while the pane does not have the keyboard.
    Unfocused,
    /// Only while the pane is not on screen.
    Invisible,
};

/// One notification a program asked for, with what it said about showing it.
///
/// Views into the parser's storage, valid for the call.
struct NotificationRequest {
    /// The title as parsed, possibly empty.
    std::string_view title;
    /// The body as parsed, possibly empty.
    std::string_view body;
    /// The program's own name for this notification (OSC 99 `i=`), or empty.
    /// A later one under the same name stands in place of the earlier.
    std::string_view identifier;
    /// When the program wants it shown.
    NotificationOccasion occasion = NotificationOccasion::Always;
};

// OSC 7 current directory, OSC 9 / 99 / 777 notification.
class ShellClient : public CanMakeCheckedPtr {
public:
    virtual ~ShellClient() = default;
    virtual void setCurrentDirectory(std::string_view) { }
    virtual void postNotification(std::string_view /*title*/, std::string_view /*body*/) { }

    /// Posts a notification with everything the program said about it.
    ///
    /// The parser calls this one. A client that carries only the words —
    /// one whose transport has fields for nothing else — leaves it alone and
    /// receives them through ``postNotification``.
    ///
    /// - Parameter request: The notification.
    virtual void postNotificationRequest(const NotificationRequest& request) { postNotification(request.title, request.body); }
};

// Parser trace sink + unhandled-OSC-code observer.
class DiagnosticsClient : public CanMakeCheckedPtr {
public:
    virtual ~DiagnosticsClient() = default;
    virtual void log(std::string_view, uint32_t) { }
    virtual void onUnknownOSCCode(int, std::string_view) { }
};

/// Measures proportional text for OSC 66 `p=1:q=1` (Docs/proportional-text.md).
///
/// The frontend owns the face and the type size, so only it can say how many
/// cells a run needs; Core carries the answer back to the application. A
/// frontend that installs none leaves Core to answer with the text's width in
/// the grid's own face.
class TextMeasureClient : public CanMakeCheckedPtr {
public:
    virtual ~TextMeasureClient() = default;
    /// Shapes a run in the frontend's proportional face at the grid's type
    /// size and answers its glyphs (Docs/proportional-text.md).
    ///
    /// The one thing only a frontend can answer. Core lays the glyphs out
    /// (Rendering/ProportionalRun.h) and derives from that the cells a run
    /// needs, which cell each code point's ink begins in, and where a pixel
    /// falls in the text — so a frontend that answers this gains all three.
    ///
    /// - Parameters:
    ///   - codePoints: The run's text, in order.
    ///   - style: The face and weight the run is set in, which the frontend
    ///     draws it in; shaping another face lays the letters out elsewhere.
    ///   - glyphsOut: Receives one shaped glyph per glyph, up to its size.
    /// - Returns: How many glyphs were written, or zero when the frontend
    ///   cannot shape; Core then measures in the grid face and the cells
    ///   take the text sixteen apiece.
    [[nodiscard]] virtual int shapeProportionalRun(std::span<const uint32_t> /*codePoints*/, ProportionalRunStyle /*style*/, std::span<Ubiquitty::ProportionalShapedGlyph> /*glyphsOut*/) { return 0; }
    /// One cell's advance in the pixels the shaped glyphs are measured in,
    /// or zero when the frontend cannot say.
    [[nodiscard]] virtual float proportionalCellAdvanceInPixels() { return 0.0f; }
    /// The cells a run needs, answered whole by a measurer that counts runs
    /// without shaping them: a host, which draws nothing, counting in the
    /// faces its clients draw runs in (Docs/proportional-text.md). Core asks
    /// this before it shapes, for a query's answer and a rewrap's pieces.
    ///
    /// - Parameters:
    ///   - codePoints: The run's text, in order.
    ///   - style: The face and weight the run is set in.
    /// - Returns: The cells, or zero when this measurer answers by shaping.
    [[nodiscard]] virtual int proportionalRunCells(std::span<const uint32_t> /*codePoints*/, ProportionalRunStyle /*style*/) { return 0; }

    /// Decides whether a proportional query waits for a driver's face facts.
    ///
    /// A host may keep the query for a bounded interval. Other frontends
    /// answer immediately; Core uses grid width when the bound is exhausted.
    ///
    /// - Parameters:
    ///   - terminal: The terminal that will send the answer.
    ///   - codePoints: The run to measure.
    ///   - style: Its face and weight.
    ///   - requestID: The program's query identity.
    ///   - terminator: The reply terminator chosen when the query arrived.
    /// - Returns: Whether to answer now, hold the query or answer in grid width.
    [[nodiscard]] virtual ProportionalQueryDisposition proportionalQuery(Ubiquitty::Terminal& /*terminal*/, std::span<const uint32_t> /*codePoints*/,
        ProportionalRunStyle /*style*/, std::uint32_t /*requestID*/, std::string_view /*terminator*/)
    {
        return ProportionalQueryDisposition::Answer;
    }

    /// Holds a later reply behind an owed proportional query.
    ///
    /// The host replays held replies in parse order when the face arrives or
    /// the deadline expires; other frontends hold nothing.
    ///
    /// - Parameters:
    ///   - terminal: The terminal that owns the reply.
    ///   - capability: The disclosure already allowed for this reply.
    ///   - bytes: The complete reply.
    /// - Returns: Whether the host took the reply for ordered replay.
    [[nodiscard]] virtual bool holdReply(Ubiquitty::Terminal& /*terminal*/, Capability /*capability*/, std::span<const char> /*bytes*/) { return false; }
};

// Kitty graphics file read; returns bytes read into out, or <0 on error.
class FileReadClient : public CanMakeCheckedPtr {
public:
    virtual ~FileReadClient() = default;
    [[nodiscard]] virtual int readFile(const char*, bool, unsigned char*, int) { return 0; }
    // Kitty t=s: read a POSIX shm object by name into out; bytes read, or <0 on
    // error. The unlink flag mirrors readFile's t=t (remove after a successful
    // read; here a shm_unlink). Default <0 = unsupported (ENOTSUP).
    [[nodiscard]] virtual int readSharedMemory(const char*, bool, unsigned char*, int) { return -1; }
    // Declares t=s support so the dispatch answers ENOTSUP without attempting a
    // read on hosts that implement only the file transports.
    [[nodiscard]] virtual bool supportsSharedMemoryTransport() const { return false; }
    /// Whether this reader takes kitty t=t, the file the terminal deletes once read.
    ///
    /// Declared rather than assumed because a client reads more into a t=t
    /// answer than t=t. kitten icat probes with a t=t file in its temporary
    /// directory and, once that succeeds, sends every PNG it can pass through
    /// as t=f naming the original file wherever it lives, with replies
    /// suppressed. A reader whose t=f admits only temporary directories
    /// therefore declares no t=t: the probe is refused and the client sends the
    /// image inline, instead of naming a file this reader refuses and showing
    /// nothing. Without the declaration Core answers t=t with ENOTSUP before
    /// any read.
    ///
    /// - Returns: Whether ``readFile`` may be asked to read and delete a t=t file.
    [[nodiscard]] virtual bool supportsTemporaryFileTransport() const { return false; }
};

// Triggers (Source/Core/Triggers/Engine.h). Core's engine matches completed lines
// against rules but never runs the effect; onTrigger is the dispatch seam the
// frontend implements. `action` is the Triggers::Action wire value; `arg` is the
// rule payload (possibly empty); `matchedText` is the matched substring;
// column0/column1 are its inclusive byte columns. Unset ⇒ Core drops (no effect).
class TriggerClient : public CanMakeCheckedPtr {
public:
    virtual ~TriggerClient() = default;
    virtual void onTrigger(uint8_t /*action*/, std::string_view /*arg*/, std::string_view /*matchedText*/, int /*column0*/, int /*column1*/) { }
};

// Default-deny gate for grid-escaping side effects. Core only asks, then obeys;
// it never acts on a denied request. Unset ⇒ Core answers PAL's defaultDecision
// for the origin; a set policy decides each request, defaulting to Deny so an
// installing host is fail-closed.
// `detail` is the request payload (e.g. a t=f path or OSC-52 selection); a
// FileReadAccess impl canonicalizes it (realpath) before deciding, so a `..`
// can't escape a confined root. `Capability` is Ubiquitty::Capability: what a
// program in the terminal may do, gated per origin.
class CapabilityPolicy : public CanMakeCheckedPtr {
public:
    virtual ~CapabilityPolicy() = default;
    [[nodiscard]] virtual CapabilityDecision check(Capability /*capability*/, CapabilityOrigin /*origin*/, std::string_view /*detail*/) { return CapabilityDecision::Deny; }
    /// Holds one already-decoded clipboard write after check returned Prompt.
    /// The default refuses; retaining bytes is not permission to deliver them.
    virtual void holdClipboardWrite(CapabilityOrigin, std::span<const ClipboardBuffer>, std::span<const char>) { }
};

} // namespace Ubiquitty
