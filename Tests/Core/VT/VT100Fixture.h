// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>

// The vocabulary the VT test files share.
//
// Tests/TerminalFixture.h supplies what every terminal test needs — feed, drain,
// read a cell. This supplies what the VT files additionally need and nothing
// else does: the paint-cycle probes, the client doubles that stand in for a
// frontend, the PAL contexts that pin a clock or a decoder, and the fake file
// system the transfer paths read from.
//
// A helper belongs here when more than one VT file calls it. One that a single
// file calls stays in that file, where a reader meets it next to its only use.

#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellGrid.h"
#include "Core/Input/PointerInput.h"
#include "Core/Kitty/KeyboardFlags.h"
#include "Core/Modes/Focus.h"
#include "Core/Modes/HostPolicy.h"
#include "Core/Modes/Scroll.h"
#include "Core/Modes/SyncOutput.h"
#include "Core/Mouse/Hilite.h"
#include "Core/Mouse/Locator.h"
#include "Core/Mouse/MouseEvent.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Screen/Scrollback.h"
#include "Core/TerminalOperations.h"
#include "Core/Terminfo.h"
#include "Core/Themes/RethemeDefaults.h"
#include "Core/Triggers/Triggers.h"
#include "Harness.h"
#include "PAL/PAL.h"
#include "Platform/Host/HostCapabilityPolicy.h"
#include "TerminalFixture.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
namespace Kitty = Ubiquitty::Kitty;
using UbiquittyTest::cell;
using UbiquittyTest::drain;
using UbiquittyTest::feed;

// Property check: after every drained paint cycle, CellGrid's
// row-dirty hint must stay consistent with the back-buffer shadow +
// cell + selection state. Hard-fails the suite on drift so a missed
// dirty-mark in any mutator path surfaces immediately.
#ifndef NDEBUG
#define TEST_ASSERT_DISPLAY_INVARIANTS(d)                                                   \
    do {                                                                                    \
        if (!(d)->assertInvariants()) {                                                     \
            fprintf(stderr, "FAIL: %s:%d display invariants broken\n", __FILE__, __LINE__); \
            exit(1);                                                                        \
        }                                                                                   \
    } while (0)
#else
#define TEST_ASSERT_DISPLAY_INVARIANTS(d) ((void)0)
#endif

/// Returns the stored image carrying `imageID`, or `nullptr` when none does.
///
/// Scans the whole store rather than indexing it: an image's slot is an
/// allocation detail and a test that assumed one would pin the allocator
/// instead of the protocol.
///
/// - Parameters:
///   - t: The terminal holding the image store.
///   - imageID: The kitty image id the transmission assigned.
/// - Returns: The stored image, or `nullptr`.
inline const Ubiquitty::Image::TerminalImage* findKittyImage(Ubiquitty::Terminal& t, uint32_t imageID)
{
    int i;
    for (i = 0; i < Ubiquitty::Image::maxTerminalImageCount; i++) {
        const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(&t, i);
        if (img && img->imageID == imageID)
            return img;
    }
    return nullptr;
}

/// Writes `count` lines, each naming its own index, and leaves the cursor below
/// them.
///
/// A row whose content names its own position is what makes a scroll or a
/// reflow assertion readable: the test can say which line ended up where
/// without a separate map from row to expected text.
///
/// - Parameters:
///   - t: The terminal to write into.
///   - count: How many numbered lines to write.
inline void feedNumberedLines(Ubiquitty::Terminal& t, int count)
{
    int i;
    char buffer[16];
    for (i = 0; i < count; i++) {
        snprintf(buffer, sizeof(buffer), "L%03d", i);
        feed(t, buffer);
        if (i + 1 < count)
            feed(t, "\r\n");
    }
}

// --- CellGrid-API helpers -------------------------------------------
//
// The tests below interrogate paint-pending state through the C bridge
// for CellGrid + PaintSession. Each helper opens a PaintSession, walks
// the requested iterator(s), and tears the session down. Sessions are
// heap-boxed in the bridge — pair each Begin with End.

// Coverage maps for run iteration. Filled in by drainPaintCycle.
typedef struct {
    int runs;
    int pendingScrolls;
    /// Per-row: set true when a yielded run covered any column of the row.
    bool isRowYielded[Ubiquitty::maxRows];
    /// Per-cell: set true for every cell within a yielded run.
    bool isCellYielded[Ubiquitty::maxRows][Ubiquitty::maxColumns];
    /// Most recently yielded scroll (last wins; tests check the final
    /// merged scroll which is the only one outstanding pre-drain).
    int scrollTop;
    int scrollBottom;
    int scrollShift;
    bool isFullRedraw;
} PaintTrace;

/// Walk the iterator without acking — useful for tests that want to
/// inspect pending state and then bail. Sessions still End so the dtor
/// runs (which leaves m_anyMutationsSinceLastPaint set, as desired
/// for "did this op queue work" checks).
///
/// - Parameters:
///   - t: The terminal whose paint state is inspected.
///   - tr: Receives the run count, the pending scroll and the per-cell coverage.
inline void peekPaintTrace(Ubiquitty::Terminal& t, PaintTrace* tr)
{
    Ubiquitty::PaintSession s = t.grid->beginPaint();
    Ubiquitty::Screen::PendingScroll sc;
    Ubiquitty::PaintRun run;
    int r, c;
    tr->runs = 0;
    tr->pendingScrolls = 0;
    tr->scrollTop = -1;
    tr->scrollBottom = -1;
    tr->scrollShift = 0;
    tr->isFullRedraw = s.isFullRedraw();
    for (r = 0; r < Ubiquitty::maxRows; r++) {
        tr->isRowYielded[r] = false;
        for (c = 0; c < Ubiquitty::maxColumns; c++)
            tr->isCellYielded[r][c] = false;
    }
    while (s.nextPendingScroll(&sc)) {
        tr->pendingScrolls++;
        tr->scrollTop = sc.topRow.value();
        tr->scrollBottom = sc.botRow.value();
        tr->scrollShift = sc.shiftRows;
        // Don't ack — leave pending for caller to inspect, but the
        // iterator only ever exposes one slot so re-peeking from
        // inside the loop is fine. Break to avoid infinite-loop.
        break;
    }
    while (s.nextDirtyRun(&run)) {
        tr->runs++;
        int rowValue = run.row.value();
        if (rowValue >= 0 && rowValue < Ubiquitty::maxRows) {
            tr->isRowYielded[rowValue] = true;
            for (c = run.firstColumn.value(); c <= run.lastColumn.value() && c < Ubiquitty::maxColumns; c++)
                tr->isCellYielded[rowValue][c] = true;
        }
        // Don't ack — the dtor's "yielded != acked" invariant fires
        // in DEBUG builds, but the bookkeeping flag is local and
        // doesn't affect subsequent tests.
    }
}

/// Run a full paint cycle: drain pending scrolls, ack each; iterate
/// dirty runs, ack each. Shadow ends up synced — next paint yields
/// zero runs. Caller can pass nullptr for the trace if it only cares
/// about post-conditions.
///
/// - Parameters:
///   - t: The terminal to paint.
///   - tr: Receives the trace, or `nullptr` to run the cycle for its effect.
inline void drainPaintCycle(Ubiquitty::Terminal& t, PaintTrace* tr)
{
    PaintTrace local;
    if (!tr)
        tr = &local;
    {
        Ubiquitty::PaintSession s = t.grid->beginPaint();
        Ubiquitty::Screen::PendingScroll sc;
        Ubiquitty::PaintRun run;
        int r, c;
        tr->runs = 0;
        tr->pendingScrolls = 0;
        tr->scrollTop = -1;
        tr->scrollBottom = -1;
        tr->scrollShift = 0;
        tr->isFullRedraw = s.isFullRedraw();
        for (r = 0; r < Ubiquitty::maxRows; r++) {
            tr->isRowYielded[r] = false;
            for (c = 0; c < Ubiquitty::maxColumns; c++)
                tr->isCellYielded[r][c] = false;
        }
        while (s.nextPendingScroll(&sc)) {
            tr->pendingScrolls++;
            tr->scrollTop = sc.topRow.value();
            tr->scrollBottom = sc.botRow.value();
            tr->scrollShift = sc.shiftRows;
            s.ackScroll(sc);
        }
        while (s.nextDirtyRun(&run)) {
            tr->runs++;
            int rowValue = run.row.value();
            if (rowValue >= 0 && rowValue < Ubiquitty::maxRows) {
                tr->isRowYielded[rowValue] = true;
                for (c = run.firstColumn.value(); c <= run.lastColumn.value() && c < Ubiquitty::maxColumns; c++)
                    tr->isCellYielded[rowValue][c] = true;
            }
            s.ackRun(run);
        }
    } // s dtor runs here
    // Property: after a fully-drained, fully-acked paint cycle the hint
    // still agrees with the grid's own dirty state. Catches any mutator
    // path that forgot to set the row-dirty bit.
    TEST_ASSERT_DISPLAY_INVARIANTS(t.grid);
}

/// OSC 50 (set/query/cycle font): family, family:size=N, rxvt #N / #+N / #-N,
/// empty reset, malformed-ignore, plus the '?' query reply.
/// 14/16 t answer with real pixels once the frontend has published a
/// cell size; with none set they fall back to the nominal 8x16 cell.
/// CSI 14t (and CSI 15t's fallback) reports the frontend-published TRUE drawn
/// text-area extent when set — round(columns*cellWidthPoints*scale) — not the
/// per-cell product columns*cellPixelWidth, which accumulates rounding into a
/// horizontal drift on a fractional advance. CSI 16t (one cell) keeps cellPixelWidth.
/// (report-true-textarea-pixel-width)
/// RIS (ESC c) resets terminal state but the frontend-published window /
/// display geometry describes the host's physical window, not terminal
/// state, so it must survive the reset — otherwise the XTWINOPS pixel /
/// position reports fall back to 8x16 / 0,0 until the next window event.
/// The window-geometry reports (CSI 13 t position, 14 t text-area
/// pixels, 15 t screen pixels, 16 t cell pixels) are a fingerprinting /
/// clipboard-adjacent leak channel, so they must stay behind the same
/// allowWindowOperations flag that gates the set/move/resize ops. When the
/// flag is off they reply with nothing; when on they report as usual.
/// allowTerminalCapabilityOperations gates the XTGETTCAP reply
/// (DCS + q <hex> ST). Default (allowed) answers as usual; when denied a
/// locked-down session swallows the query with no DCS response at all, so
/// the host learns nothing about our terminfo.
/// allowMouseOperations gates the mouse-tracking ENABLEs
/// (DECSET 9/1000/1002/1003/1005/1006/1015/1016). When denied the ENABLE
/// is refused so the terminal never enters mouse reporting; a DISABLE
/// (DECRST) always applies. Default (allowed) enters reporting as usual.
/// allowPasteControls gates bracketed-paste mode enable (DECSET 2004).
/// When denied the ENABLE is refused so pasted text is never wrapped in the
/// paste-bracket escapes; a DISABLE (DECRST) always applies. Default
/// (allowed) enters bracketed paste as usual.
/// the three allow-gates default to ALLOWED on init, so default
/// behavior is byte-identical to a build without the gates.
/// Regression: pinning the exact "user types ls, presses Enter,
/// cursor advances" cycle. Guards against the cursor staying put after
/// Enter; this test reproduces the host path and asserts the
/// parser-side cursor advances.
/// Regression: CR + LF alone must not mark cells as dirty. The
/// renderer's cursor-only-XOR fast path depends on
/// hasUnpaintedWork() returning false after a pure cursor move. If
/// either CR or LF accidentally raises the flag, the fast path
/// skips and the cursor "ghosts" because the heavyweight repaint
/// path doesn't correctly erase the old position.
/// Regression: typing after backspace must leave a single cursor
/// position (guards against "more cursors" after backspace). Asserts
/// parser-side cursor advance is single-stepped, not duplicated.
/// Default_Ignorable_Code_Point=Yes that lands as its own standalone cell
/// (no live cluster to absorb it) must be DROPPED: no visible/occupying
/// cell, cursor not advanced. The drop folds into the existing per-char
/// width lookup (the codePoint is zero-width, so emitCharacter's `width == 0`
/// early return fires) — no new hot-path test. This must not disturb the clustering / combining path:
/// ZWJ, variation selectors, and combining marks are consumed by the
/// grapheme clusterer before width is consulted, so this test re-asserts a
/// ZWJ emoji family and a combining stack still compose.
// DCS payload growth: a frame whose payload exceeds the initial oscBufferInitialCapacity
// (256 byte) baseline grows the accumulator on demand instead of
// truncating, so no dcs-truncated diag fires for ordinary sizes.
/// Capturing DiagnosticsClient: counts log() calls per tag and records
/// the parsed unknown-OSC Ps. Assigned to Terminal::diagnosticsClient so Core
/// routes diagnostics through it.
namespace {
/// Counts diagnostic logs per tag and keeps the most recent detail word.
struct CaptureDiagClient : Ubiquitty::DiagnosticsClient {
    int dcsTrunc = 0;
    int csiMiss = 0;
    int sixelClipped = 0;
    uint32_t lastDetail = 0;
    /// Records one diagnostic, bucketed by tag.
    ///
    /// - Parameters:
    ///   - tag: The diagnostic's stable identifier.
    ///   - detail: The tag-specific word, kept only for the most recent call.
    void log(std::string_view tag, uint32_t detail) override
    {
        if (tag == "dcs-truncated") {
            dcsTrunc++;
            lastDetail = detail;
        } else if (tag == "csi-miss") {
            csiMiss++;
            lastDetail = detail;
        } else if (tag == "sixel-clipped") {
            sixelClipped++;
            lastDetail = detail;
        }
    }
};
}

/// A non-image OSC (title/hyperlink) is clipped at maxOSCNonImageSize so a
/// runaway unterminated title can't grow the shared accumulator to the 64 MiB
/// image cap; the image/clipboard codes (OSC 52, iTerm2 1337) keep the full
/// ceiling for their legitimate large base64.
/// A non-sixel DCS (XTGETTCAP +q, DECRQSS $q, DECUDK, DECDLD) carries no image
/// payload, so its accumulation is clipped at maxOSCNonImageSize — never the
/// 64 MiB image cap a bare reserveOSCBuffer allowed. Guards the multi-session
/// resource-exhaustion amplifier. Both the batched run path and the per-byte path
/// are covered.
/// Stub PAL image decoder: records the byte count it was handed and emits a
/// fixed 4x4 RGBA buffer so OSC 1337 tests stay deterministic.
namespace {
inline int stubDecodeCalls = 0;
inline int stubDecodeLength = 0;
/// The first bytes the stub decoder received, so a test can assert what was
/// decoded rather than how much. Capped at 8: the alphabet defects this exists
/// to catch show up in the first group.
inline std::vector<uint8_t> stubDecodeFirstBytes;
/// Decodes any payload to a fixed 4x4 opaque buffer and records the byte count.
///
/// The payload is ignored on purpose: a test asserting that a decode was
/// reached should not also depend on a real codec's output.
///
/// - Parameters:
///   - bytes: The encoded payload, unread.
///   - length: Its size, recorded in `stubDecodeLength`.
///   - rgbaOut: Receives a freshly allocated 4x4 RGBA buffer.
///   - widthOut: Receives 4.
///   - heightOut: Receives 4.
/// - Returns: True always; the stub has no failure mode.
inline bool stubImageDecoder(std::span<const std::uint8_t> bytes, uint8_t** rgbaOut, int* widthOut, int* heightOut)
{
    stubDecodeCalls++;
    stubDecodeLength = static_cast<int>(bytes.size());
    // The decoded PAYLOAD, not only its size. A base64 defect that maps one
    // alphabet character to another wrong sextet changes byte VALUES and leaves
    // the length alone, so a fixture recording only the length cannot see it —
    // and the image path's decode is otherwise unasserted.
    const auto prefix = bytes.first(std::min<std::size_t>(bytes.size(), 8));
    stubDecodeFirstBytes.assign(prefix.begin(), prefix.end());
    *rgbaOut = static_cast<uint8_t*>(malloc(4 * 4 * 4));
    if (!*rgbaOut)
        return false;
    memset(*rgbaOut, 0, 4 * 4 * 4);
    *widthOut = 4;
    *heightOut = 4;
    return true;
}
}

/// OSC 7 (file:// URL) and OSC 1337 CurrentDir mirror the currentDirectory into the Ubiquitty::Terminal so a
/// shellClient-less consumer (the host daemon) can read it for CommandDone.
/// OSC 6 (current document/file URL) — the file analog of OSC 7. The URL is
/// stored verbatim on the Ubiquitty::Terminal so a proxy-icon / reveal-in-Finder consumer can
/// read it; an empty payload clears it; an oversized URL is bounded (no overflow).
/// OSC 133 ; D ; n captures the command exit status and bumps the completion
/// generation so a host/daemon can surface a CommandDone event; A/B/C do not.
/// OSC 133 C stamps a start tick (via the PAL monotonic-clock hook); D
/// records the elapsed C->D ticks in the generation-indexed duration ring
/// (same indexing as the exit-code ring). A D with no preceding C — or no
/// installed clock — records Ubiquitty::commandDurationUnknown instead.
inline uint32_t osc133FakeTicks = 0;

/// Returns the tick count the test set, standing in for the monotonic clock.
///
/// - Returns: The value of `osc133FakeTicks`, so a test can advance time by
///   assignment rather than by waiting.
inline Ubiquitty::Ticks osc133FakeClock() { return Ubiquitty::Ticks(osc133FakeTicks); }

// Tests install PAL hooks through a scope-restoring PAL::ScopedContext
// (no mutable global setter). These build a Context carrying the required clock
// plus the one hook under test; ScopedContext restores the prior on scope exit,
// so a test's hook can't leak into the next (isolation by RAII, not order).
/// Builds a PAL context carrying the fake clock and one image decoder.
///
/// - Parameter decoder: The decoder the context installs.
/// - Returns: A context to hand to `PAL::ScopedContext`.
inline Ubiquitty::PAL::Context palContextWithImageDecoder(Ubiquitty::PAL::ImageDecodeCallback decoder)
{
    Ubiquitty::PAL::Context c;
    c.monotonicClock = &osc133FakeClock;
    c.imageDecoder = decoder;
    return c;
}

/// OSC 1337 File= decodes the base64 payload through the PAL image decoder
/// and places the result at the cursor through the shared placement model.
/// With no decoder installed, OSC 1337 File= is consumed without creating an
/// image — no crash, no placement.
/// A large OSC payload (well beyond the 256-byte baseline) grows the
/// accumulator so the whole base64 image reaches the handler intact.
/// DECSASD redirects char writes to statusRow regardless of DECSCNM.
/// DECSCNM is a renderer-side global (Terminal::reverseVideo); the parser
/// stores cells with whatever pen is current, and the render layer
/// applies the global foreground/background swap. Verify that with reverseVideo on
/// the status row still receives writes routed away from cell grid.
/// While the status line is the active display (DECSASD 1), the one-row
/// cursor/edit controls operate on statusRow/statusCursorX — and the data
/// screen stays untouched throughout (the issue's simple repro: write AB,
/// CR, write C — C must overwrite the first status cell, not append).
/// DECSCL (CSI Pl ; Pc " p) sets the operating conformance level; the
/// level is observable through DA1, whose reported device class tracks
/// it. Also verifies Pc wires to transmit8bitC1 and that VT100 level
/// forces 7-bit + the classic ?1;2c device class.
/// XTSMGRAPHICS — CSI ? Pi ; Pa ; Pv S. Apps query sixel color-register
/// count (Pi=1) and graphics geometry (Pi=2) to decide whether and how to
/// emit sixel. The reply mirrors the request with Ps=0 success. The
/// register count tracks Sixel decoder's palette[256][3]; the geometry cap
/// tracks maxWidth/maxHeight (1000×1000) in the decode path. The private
/// '?'-prefix form must not collide with the no-prefix SU (CSI Ps S).
/// In VT100 conformance, a VT400 op (here DECFRA) is inert: the
/// rectangle fill must not touch the screen. Raising the level back to
/// VT400 re-enables it.
/// DECSCA (cell selective-erase protection, CSI Ps " q) is a VT200+ feature.
/// At VT100 level it is inert — the protected attribute is never set, so a
/// selective erase clears the cell. At VT200+ the protect bit lands and the
/// cell survives a DECSEL.
/// "Query current conformance level": after DECSCL sets a level, DECRQSS
/// "p reports exactly that level back (61..65), so a host can read the
/// operating level it set. Covers each level.
/// DECRQSS for DECSCUSR (SP q) must report the RESOLVED style code the terminal
/// is actually showing — never the internal FollowUserPreference sentinel: a
/// query has to round-trip observable state. With no explicit DECSCUSR and a
/// user bar+blink preference, the reply is the blinking-bar code (5), not 0; an
/// explicit style reports verbatim.
/// DECTABSR (CSI 2 $ w) reports the current tab-stop columns as
/// DCS 2 $ u <column>/<column>/... ST. Clear all stops, set a custom few via
/// HTS, and verify the report lists exactly those 1-based columns.
/// DECSDM (DEC private mode ?80) round-trips through DECSET/DECRST and
/// DECRQM (CSI ? 80 $ p) reports its set/reset state.
/// DECSSDT / DECSASD must parse in the standard `$`-intermediate form
/// (CSI Ps $ ~ and CSI Ps $ }) — the spec form apps/the demo drive.
/// Verify the state effect lands and no garbage leaks into the cell
/// grid or the host response queue.
/// DECSSDT / DECSASD state-machine edges the macOS status-row paint pass
/// relies on: an out-of-range DECSSDT type clamps to 0 (none), each DECSASD 1
/// re-activation rewinds the status cursor to column 0 (so a fresh write
/// overwrites from the left, not appended), DECSASD 0 stops the redirect, and
/// the current pen (SGR) rides onto the status cell so the renderer paints it
/// with the host's attributes.
/// DECSSDT type selection: 0=none, 1=indicator, 2=host-writable each set a
/// distinct statusLineType the renderer keys off. Switching among them updates
/// the stored type; an out-of-range value clamps to 0. Type 1 (indicator) is
/// the terminal-drawn line — distinct from type 2 — so the engine must keep the
/// three values apart (the macOS paint pass renders 1 vs 2 differently).
/// Clears one terminal's six sequence-gate lists to the inert default.
///
/// - Parameter t: The terminal whose gate is cleared.
inline void seqGateClear(Ubiquitty::Terminal& t)
{
    CHECK(Ubiquitty::OSC::setOSCAllowList(t, nullptr, 0));
    CHECK(Ubiquitty::OSC::setOSCDenyList(t, nullptr, 0));
    CHECK(Ubiquitty::OSC::setDCSAllowList(t, nullptr, 0));
    CHECK(Ubiquitty::OSC::setDCSDenyList(t, nullptr, 0));
    CHECK(Ubiquitty::OSC::setAPCAllowList(t, {}));
    CHECK(Ubiquitty::OSC::setAPCDenyList(t, {}));
}

/// A mid-pass %layout-change makes the frontend release the deliver pane's
/// lock/pin (deadlock avoidance around Pane::setGeometry) and call
/// invalidateTmuxPaneRoute — this binds the event the way the frontend does and
/// pins the contract: %output for the same pane after the layout change
/// re-looks-up (re-acquires) instead of hitting the dead memo. Verify-by-revert:
/// drop the invalidate call from the handler and the count reads 1.
/// Regression: a kitty image placed with an EXPLICIT image id (a=T,i=N — the
/// standard form kitten icat emits) must scroll with the text on a full-screen
/// scroll. The scroll gate keys the shift on the live-placement count: the
/// explicit-id transmit path never bumps kittyNextImageID, so a gate keyed on
/// it skips Image::scrollPlacementsUp and leaves the placement pinned to its
/// grid row while text scrolls beneath it.
// The per-Ubiquitty::Terminal image-pixel budget evicts least-recently-used images
// (non-displayed first) so a flood of images can't grow the store without bound.
/// Pin the difference between f=24 (RGB) and f=32 (RGBA) image
/// storage. The parser must keep pixel layout / byte counts
/// distinct: 24-bit packed RGB triplets vs 32-bit aligned RGBA
/// quads, with the alpha channel preserved at f=32.
/// FileReadClient stub used by the t=f / t=t tests. The fake "file
/// system" is a single in-memory blob keyed on a literal path string.
inline const char* fakeFileSystemPath;

inline const unsigned char* fakeFileSystemBytes;

inline int fakeFileSystemLength;

inline int fakeFileSystemUnlinkCount;

/// Serves one file from the `fakeFileSystem*` variables and counts unlinks.
struct FakeFileReadClient : Ubiquitty::FileReadClient {
    [[nodiscard]] int readFile(const char* path, bool shouldUnlink, unsigned char* outputBuffer, int outputCapacity) override
    {
        if (!fakeFileSystemPath || strcmp(path, fakeFileSystemPath) != 0)
            return -1;
        if (fakeFileSystemLength > outputCapacity)
            return -1;
        memcpy(outputBuffer, fakeFileSystemBytes, (size_t)fakeFileSystemLength);
        if (shouldUnlink)
            fakeFileSystemUnlinkCount++;
        return fakeFileSystemLength;
    }
    // Declares t=t, so the cases reach the read and the unlink count.
    [[nodiscard]] bool supportsTemporaryFileTransport() const override { return true; }
    // t=s reuses the same single-entry fake store keyed on the object NAME.
    // Mirrors the real client's copy-up-to-capacity contract (the platform
    // object is page-rounded, so the Core-side S bound governs exactness).
    [[nodiscard]] bool supportsSharedMemoryTransport() const override { return true; }
    [[nodiscard]] int readSharedMemory(const char* name, bool shouldUnlink, unsigned char* outputBuffer, int outputCapacity) override // bool-ok: seam-defined, mirrors readFile
    {
        if (!fakeFileSystemPath || strcmp(name, fakeFileSystemPath) != 0)
            return -1;
        const int copyLength = fakeFileSystemLength < outputCapacity ? fakeFileSystemLength : outputCapacity;
        memcpy(outputBuffer, fakeFileSystemBytes, (size_t)copyLength);
        if (shouldUnlink)
            fakeFileSystemUnlinkCount++;
        return copyLength;
    }
};

inline FakeFileReadClient fakeFileReadClient;

/// t=f (regular file). The APC payload is the base64-encoded path; the
/// file's contents are the raw RGB image bytes. Asserts the file hook
/// was consulted, the contents land in a TerminalImage, and (since the
/// transport is t=f, not t=t) the unlink-after counter stays 0.
/// t=s (POSIX shared memory). The APC payload is the base64-encoded object
/// NAME; S carries the exact payload length (the kernel page-rounds object
/// sizes). Asserts the shm hook was consulted, the bytes land in a
/// TerminalImage, and the object is unlinked after the read (t=s semantics:
/// the terminal deletes the object).
/// A host CapabilityPolicy gates the t=f file read. Global (outlives the
/// Terminal, like fakeFileReadClient) so the CheckedPtr never dangles.
struct FakeCapabilityPolicy : Ubiquitty::CapabilityPolicy {
    Ubiquitty::CapabilityDecision decision = Ubiquitty::CapabilityDecision::Deny;
    Ubiquitty::Capability lastCapability {};
    Ubiquitty::CapabilityOrigin lastOrigin {};
    bool wasCalled = false;
    // The host file read (FileReadAccess) is gated separately from the kitty
    // protocol's EPERM ACK; capture it specifically, since the ACK now also
    // consults the policy (Capability::None) and moves lastCapability past it.
    bool sawFileRead = false;
    Ubiquitty::CapabilityOrigin fileReadOrigin {};
    [[nodiscard]] Ubiquitty::CapabilityDecision check(Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin origin, std::string_view) override
    {
        wasCalled = true;
        lastCapability = capability;
        lastOrigin = origin;
        if (capability == Ubiquitty::Capability::FileReadAccess) {
            sawFileRead = true;
            fileReadOrigin = origin;
        }
        // A pure protocol ACK (Capability::None) is always allowed — model the
        // real host policy's Benign tier so a denied op still ACKs (EPERM is not
        // a silent drop). A blanket-deny fake would otherwise suppress the kitty
        // error channel, which the production policy never does.
        if (capability == Ubiquitty::Capability::None)
            return Ubiquitty::CapabilityDecision::Allow;
        return decision;
    }
};

inline FakeCapabilityPolicy fakeCapabilityPolicy;

/// OSC 4 / 10 / 11 / 12 / 110 / 111 / 112 — palette + default foreground/background +
/// cursor. The dispatcher parses the wire color spec and writes Core's
/// Terminal::palette directly; a `?` query reports the stored slot; the
/// 110/111/112 resets revert from Terminal::paletteBaseline.

/// OSC 12 — cursor colour set / query, OSC 112 — cursor colour reset.
/// Drives handleCursorColor (set + query branches) writing Terminal::palette.cursor
/// and the OSC 112 revert from Terminal::paletteBaseline.cursor.
// OSC 7 — current-working-directory hint via file://[host]/<path>.
// Drives handleWorkingDirectory's URL-decode loop, the host-skip branch, the
// %XX escape decode, and the malformed-escape passthrough.
/// ShellClient capturing currentDirectory + notification routing for OSC 7 / 9 / 99 /
/// 777 / 1337 tests.
struct CaptureShell : Ubiquitty::ShellClient {
    char currentDirectory[512];
    int cwdLength = 0;
    int cwdCalls = 0;
    char notifyTitle[256];
    char notifyBody[256];
    int notifyTitleLength = -1;
    int notifyBodyLength = -1;
    int notifyCalls = 0;
    /// Records the reported working directory, truncated to the capture buffer.
    ///
    /// - Parameter path: The directory the terminal reported.
    void setCurrentDirectory(std::string_view path) override
    {
        cwdCalls++;
        int length = (int)path.size();
        cwdLength = length;
        int n = length < (int)sizeof(currentDirectory) - 1 ? length : (int)sizeof(currentDirectory) - 1;
        memcpy(currentDirectory, path.data(), (size_t)n);
        currentDirectory[n] = 0;
    }
    /// Records one notification and its untruncated lengths.
    ///
    /// - Parameters:
    ///   - title: The notification title.
    ///   - body: The notification body.
    void postNotification(std::string_view title, std::string_view body) override
    {
        notifyCalls++;
        int titleLength = (int)title.size();
        int bodyLength = (int)body.size();
        notifyTitleLength = titleLength;
        notifyBodyLength = bodyLength;
        int tn = titleLength < (int)sizeof(notifyTitle) - 1 ? titleLength : (int)sizeof(notifyTitle) - 1;
        int bn = bodyLength < (int)sizeof(notifyBody) - 1 ? bodyLength : (int)sizeof(notifyBody) - 1;
        // An empty title or body has no storage, and memcpy's source must be
        // non-null even for zero bytes.
        if (tn > 0)
            memcpy(notifyTitle, title.data(), (size_t)tn);
        notifyTitle[tn] = 0;
        if (bn > 0)
            memcpy(notifyBody, body.data(), (size_t)bn);
        notifyBody[bn] = 0;
    }
};

/// probe B, drain — B's response must not be contaminated by A's
/// echoed cap-hex.
/// ---------------------------------------------------------------------
/// Device attributes (DA) + Device status reports (DSR) + mode queries
/// -------------------------------------------------------------------
/// The exact byte sequences emitted here are the contract surface that pty
/// consumers (autocomplete probers, capability detectors, tmux's $TERM
/// detection) depend on. Reply forms are the ones specified for DA1/DA2/DA3,
/// DSR and DECRQM in the xterm control sequences reference
/// (https://invisible-island.net/xterm/ctlseqs/ctlseqs.html).
/// Per xterm: only Ps=0 is recognised for DA1. Any other parameter is
/// reserved and the request must produce no reply.
/// DA3 (tertiary device attributes) reply form per xterm is
/// DCS ! | <hex-id> ST. Ubiquitty does not implement DA3 — the `=` private
/// prefix is recognised by the CSI parser but no handler is registered,
/// so the request is silently swallowed.
/// ESC Z is the VT100-era DECID and must produce the same response as
/// DA1. Ubiquitty's ESC dispatcher does not handle 'Z' — it falls through
/// to the default no-op.
/// 0x9A is the 8-bit form of DECID (the C1 alias of ESC Z) and must reply with
/// the primary DA, byte-identical to CSI c — matching how every other 8-bit C1
/// mirrors its ESC form. Regression for the omission where 0x9A fell through to
/// Print and surfaced a stray U+FFFD instead.
/// S8C1T (ESC SP G) makes replies emit 8-bit C1 controls: the CSI that
/// introduces a CPR becomes the single byte 0x9B instead of ESC [.
/// S7C1T (ESC SP F) restores the 7-bit ESC [ form.
/// Under DECOM (origin mode) + DECSTBM, cursor row reports must be
/// relative to the scrolling region's top. CUP 1;1 inside DECOM with
/// scroll-top=5 means absolute row 5; DSR 6 n must reply 1;1, not 5;1.
/// CSI ? 6 n is DECXCPR (extended cursor position report): reply is
/// CSI ? Pl ; Pc ; Pp R where Pp is the page number. Ubiquitty's '?' DSR
/// handler only recognises kitty-keyboard query 996; other Ps values
/// are swallowed silently.
/// Unknown DSR parameters must be parsed-and-discarded — no crash, no
/// stray bytes injected into the output stream.
/// DECRQM (request mode) — CSI ? Ps $ p for DEC private modes. Reply is
/// CSI ? Ps ; Pm $ y where Pm = 1 (set), 2 (reset), 3 (permanent set),
/// 4 (permanent reset). Ubiquitty doesn't implement DECRQM: the CSI '$'
/// intermediate is only routed for the soft-reset shorthand.
/// DECRQM ANSI form — CSI Ps $ p (no '?' prefix) for ANSI modes.
/// DECREQTPARM — CSI Ps x → CSI Sol;Par;Nbits;Xspeed;Rspeed;Clkmul;Flags x.
/// No 'x' final handler is registered in Ubiquitty's CSI table.
/// ============================================================= * VT mode toggles (DECSET/DECRST) — DECAWM, DECCKM, DECPAM/DECPNM,
/// DECSCUSR, bracketed paste, focus reporting, mouse modes, sync output.
///
/// Each block pins the documented default, the set/reset round-trip, and
/// a representative side-effect (e.g. autowrap actually wrapping cells).
/// Where a hook isn't reachable from the test harness (focus-event
/// emission), the block pins what it can reach and says so.
/// ====================================================================

/// ---- DECAWM autowrap (DECSET/DECRST 7) ----

/// ---- DECCKM application cursor keys (DECSET/DECRST 1) ----

/// ---- Application keypad (ESC = / ESC >) ----

/// ---- DECSCUSR cursor style (CSI Ps SP q) ----

/// ---- Bracketed paste (DECSET/DECRST 2004) ----

/// ---- Focus reporting (DECSET/DECRST 1004) ----

/// The unfocused-bell desktop-notification gate. focusChanged records the
/// window-focus state on Ubiquitty::Terminal regardless of DECSET 1004, and
/// bellWantsNotification is true only while the window is unfocused.
/// ---- Mouse mode toggles (1000 / 1006 / independence) ----

/// Mouse modes 1002 (cell-motion tracking) and 1003 (any-event tracking)
/// are stored as separate flags from 1000/1006.
/// DECSET 9 — true X10 mouse compatibility: press-only, no release, no
/// motion, no modifier bits. Distinct from normal mode 1000.
/// Normal mode 1000 still emits the release event (unlike X10), and
/// DECRQM ?1000 reflects mouseButtonTracking.
/// A richer mode (1000/1002/1003) enabled after X10 (9) SUPERSEDES it —
/// the tracking-protocol family is mutually exclusive, so the
/// latest DECSET wins and the sibling clears rather than both staying set.
/// Release is reported again because normal mode (not X10) is now active.
/// DECSET 1003 (any-event) then DECSET 1000 (normal) — the tracking-
/// protocol family is mutually exclusive, so 1000 clears 1003 rather than
/// both staying set. DECRQM reports exactly one family member as set.
/// DECSET 1006 (SGR) then DECSET 1015 (URXVT) — the coordinate-
/// encoding family is mutually exclusive, so 1015 clears 1006 and the
/// encoder emits URXVT (not SGR) coordinates for the now-current mode.
/// CSI ? 1048 h/l routes to save/restore-cursor (xterm extension).
/// DECRQM ?1048 reports the save-cursor (DECSC-style) state: set (1) once a
/// cursor save is held, reset (2) otherwise — never unrecognized (0). 1048 is
/// implemented in the DECSET/DECRST path, so the report must mirror it.
/// DECRQM ?38 (DECTEK) reports the Tektronix-4014 mode state: reset (2) in VT
/// mode — recognized, not unrecognized (0). DECSET 38 flips isTekModeActive; the set
/// report (1) can't be observed via feed because the Tek state machine then
/// owns the byte stream (a DECRQM CSI never reaches the VT handler), but the
/// report mapping itself is the same `isTekModeActive ? 1 : 2`.
/// DECSTR (CSI ! p) is the VT510 soft-reset: cursor visible, autowrap
/// on, default scroll region, default SGR, syncOutput cleared. Does
/// not clear the screen content.
/// DECSTR resets the FULL reset list the VT510 contract requires — the
/// items history showed it missing: DECLRMM + its left/right margins,
/// DECSASD status-line input, hilite tracking (1001) and its pending
/// handshake — while PRESERVING the DECSCL operating level (DECSTR is not
/// a conformance-level selection; only RIS restores VT500).
/// DECSCL performs DECSTR semantics before applying the requested level:
/// cell contents survive, but modes/margins/pen return to defaults.
/// The C1 transmission policy (S7C1T/S8C1T) is not in the DECSTR reset list,
/// and the power-on default is 7-bit (S7C1T). So after a soft reset, replies
/// must stay ESC-based — DECSTR must never leave us in S8C1T. Drive S8C1T,
/// then DECSTR, then DA1 / DSR / OSC 11 queries and verify every reply leads
/// with ESC (0x1B) and carries no lone 8-bit C1 introducer/terminator.
/// ---- Synchronized output (DECSET/DECRST 2026) ----

// ---- Stored DEC private modes (4/8/42/67/2027/2048) round-trip ----

/// Each of these accepts DECSET/DECRST and reports its state through
/// DECRQM (CSI ? Ps $ p → CSI ? Ps ; 1|2 $ y). Behavior is cosmetic or
/// deferred; the contract verified here is the set/reset + report.
///
/// - Parameters:
///   - t: The terminal to drive.
///   - ps: The DEC private mode number.
///   - flag: The state the mode is expected to move.
///   - setSeq: The DECSET sequence for `ps`.
///   - rstSeq: The DECRST sequence for `ps`.
///   - rqmSet: The DECRQM reply expected while the mode is set.
///   - rqmRst: The DECRQM reply expected while it is reset.
inline void testStoredModeRoundtrip(Ubiquitty::Terminal& t, int ps, bool* flag, const char* setSeq, const char* rstSeq, const char* rqmSet, const char* rqmRst)
{
    char outputBuffer[64];
    char rqm[24];
    int n;
    snprintf(rqm, sizeof(rqm), "\033[?%d$p", ps);

    feed(t, setSeq);
    CHECK(*flag == true);
    feed(t, rqm);
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strcmp(outputBuffer, rqmSet) == 0);

    feed(t, rstSeq);
    CHECK(*flag == false);
    feed(t, rqm);
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    CHECK(strcmp(outputBuffer, rqmRst) == 0);
}
