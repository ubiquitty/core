// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"
#include "Core/Tek/Primitive.h"
#include "Core/Tek/Project.h"

// Tektronix 4010/4014 storage-tube emulation, portable Core. A POD value member on Terminal decodes Tek
// graphics into a bounded display list; frontends draw the list and write no parsing logic. The wire
// format decoded here is the Tektronix 4010/4014 storage-tube command set, documented below in full.
//
// Mode entry/exit: DECSET 38 (CSI ? 38 h) sets isTekModeActive, routing host bytes into the Tek machine. ESC ETX
// or ESC CAN clears it and returns to VT100 ground (bare CAN/SUB do not exit; ESC is required).
//
// Coordinate space: 4096×4096 addressing (EGM), y in [0, 3119] visible; origin bottom-left (frontends flip
// Y). A graphic address is a coordinate-byte group decoded high-to-low, classified by the high two tag bits:
//   Hi-Y  tag 0x20: y[11:7]      Extra tag 0x60 (optional): y[1:0] + x[1:0]
//   Lo-Y  tag 0x60: y[6:2]       Hi-X  tag 0x20: x[11:7]      Lo-X tag 0x40: x[6:2]
// The decoder tells Lo-Y from Extra by position (Extra arrives between Hi-Y and Lo-Y) and yields (x,y) when
// the Lo-X byte completes the group. A sender omitting Extra leaves the low 2 bits zero (4010 behavior).
//
// Plot modes (C0 bytes): GS (0x1D) graph/vector — first coord after GS is a dark move, each later coord
// draws a line; FS (0x1C) point-plot; RS (0x1E) incremental (bit 0x04 = pen down, direction bits step
// ±x/±y); US (0x1F) alpha. PAGE (ESC FF) erases the tube, homes the alpha cursor, returns to alpha.
//
// Alpha mode: printable bytes append an alpha-char primitive and advance the cursor. CR→left margin,
// LF→down one line (y decreases), BS→left one cell, HT→one cell advance (Tek HT is one advance, not a tab).
//
// Char sizes ESC 8/9/:/; and line types ESC `..o map to CharSize/LineStyle enums; h..o (defocused/bold)
// fold onto the same five styles `..d. Both are captured per primitive so the frontend scales/dashes.
//
// GIN (ESC SUB): enables a crosshair; on a key/button press tekGINKey transmits key,Hi-X,Lo-X,Hi-Y,Lo-Y,CR
// (each coord byte 0x20 | ((v>>7|>>2)&0x1F)) on the VT outbound path, then auto-disables. ESC ENQ status
// report uses the same framing with a fixed 0x20 status byte.
//
// CellGrid list: fixed-capacity, accumulates until PAGE clears it. Overflow drops further primitives (POD,
// no heap).

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Tek {

// Tek coordinate space (origin bottom-left). Frontends flip Y.
constexpr int tekGridWidth = 4096;
constexpr int tekGridHeight = 3120;

// CellGrid-list capacity. Bounded so the Terminal POD never heap-allocates; overflow drops primitives.
constexpr int tekMaxPrimitives = 4096;

// Internal plot-mode of the Tek state machine.
enum class PlotMode : uint8_t {
    Alpha = 0, // US — text
    Graph, // GS — vector
    Point, // FS — point plot
    Incremental, // RS — incremental plot
};

// Tek parser sub-state (within isTekModeActive).
enum class TekParse : uint8_t {
    Ground = 0, // normal byte processing in the current PlotMode
    Escape, // ESC seen; next byte is the escape final
    Coordinate, // mid graphic-address group (graph/point modes)
    CSI, // ESC [ seen; collecting until the final byte
};

// Per-Terminal Tek state (value member; value-init zeroes it, then tekInit applies non-zero defaults).
struct State {
    // Storage tube: `primitives` is the parser's working buffer; `displayPrimitives` is the committed frame
    // the renderer shows (swapped on PAGE), so the renderer never sees a half-drawn tube.
    Primitive primitives[tekMaxPrimitives];
    int primitiveCount;
    Primitive displayPrimitives[tekMaxPrimitives];
    int displayCount;

    // Plot + parser state.
    PlotMode plotMode;
    TekParse parse;
    LineStyle lineStyle;
    CharSize charSize;

    // CSI accumulator, kept just long enough to recognize the DECRST-38 exit; longer sequences keep
    // counting so an overrun can't false-match.
    char csiBytes[4];
    uint8_t csiLength;

    // Storage-tube overflow: push() dropped a primitive since the last PAGE/mode entry; one diagnostic per overflow.
    bool didDropPrimitive;
    bool didSignalPrimitiveDrop;

    // Current graphic position (Tek space).
    int16_t currentX;
    int16_t currentY;

    // Alpha cursor position (Tek space, baseline).
    int16_t alphaX;
    int16_t alphaY;

    // Alpha left margin (0 = margin 1, tekGridWidth/2 = margin 2). 4014 two-column page: scrolling off the
    // bottom flips the margin and homes to the top so a second column fills the right half. PAGE → margin 1.
    int16_t alphaMargin;

    // Graphic-address decode accumulator (one coordinate group).
    int pendingX;
    int pendingY;
    bool coordHasHighY; // Hi-Y byte seen this group
    bool coordHasExtra; // Extra/EGM byte seen this group
    bool coordHasLowY; // Lo-Y byte seen this group
    bool coordHasHighX; // Hi-X byte seen this group

    // First-coordinate-after-GS is a dark move (no line).
    bool isNextVectorDark;

    // Incremental-plot: pen-down state.
    bool incrementalPenDown;

    // GIN crosshair.
    bool GINActive;
    int16_t GINX;
    int16_t GINY;

    // Bypass: after a GIN/ENQ report, swallow echoed report bytes (so the host's echo doesn't draw as alpha
    // garbage) until the next control byte clears it.
    bool bypass;
};

} // namespace Ubiquitty::Tek

namespace Ubiquitty {

// Each of these takes the terminal by reference because none of them accepts its
// absence: every body dereferences immediately, and a pointer here would promise
// a null the module has never handled.

/// Applies the non-zero Tek defaults that value-initialisation cannot.
///
/// The alpha cursor goes home and the character size and line style take their
/// documented defaults, none of which is the zero a fresh `Terminal` holds.
/// Idempotent, so a reset path may call it without checking whether it already ran.
///
/// - Parameters:
///   - t: The terminal whose Tek state is seeded.
void tekInit(Ubiquitty::Terminal& t);

/// Enters Tek mode, from DECSET 38.
///
/// Sets `Terminal::isTekModeActive`, after which host bytes belong to `tekByte` rather
/// than to the VT parser.
///
/// - Parameters:
///   - t: The terminal entering Tek mode.
void tekEnter(Ubiquitty::Terminal& t);

/// Leaves Tek mode, from an in-mode ESC ETX or ESC CAN.
///
/// Clears `Terminal::isTekModeActive` and returns the VT parser to ground, so a stream
/// that leaves Tek mid-sequence cannot strand it in a partial state.
///
/// - Parameters:
///   - t: The terminal leaving Tek mode.
void tekExit(Ubiquitty::Terminal& t);

/// Feeds one host byte to the Tek state machine.
///
/// - Parameters:
///   - b: The byte. Callers reach here only while `Terminal::isTekModeActive` is set.
void tekByte(Ubiquitty::Terminal& t, uint8_t b);

/// Reports a graphic-input key or button press and lowers the crosshair.
///
/// Transmits the framed report — key, Hi-X, Lo-X, Hi-Y, Lo-Y, CR — and disables
/// GIN, which is what makes the press one-shot rather than a mode.
///
/// - Parameters:
///   - key: The character the host receives as the first byte of the report.
///   - x: Horizontal position in Tek space.
///   - y: Vertical position in Tek space.
void tekGINKey(Ubiquitty::Terminal& t, char key, int x, int y);

/// Reports a keypress as the graphic-input trigger, if the crosshair is up.
///
/// The whole decision, so a frontend supplies only what it alone knows: the
/// byte the key produced and where the pointer sits in its own view. Whether
/// the press is a trigger at all — Tek mode, GIN armed — is read here, and the
/// projection is built over the FULL view rect, decoupled from the cell grid's
/// padding and top inset, because that is the rect the vectors were drawn
/// through: unprojecting through a different one answers with coordinates the
/// drawing never used.
///
/// The report is queued rather than written; the caller ships it, because the
/// host that asked for GIN is blocked reading for it and emits nothing that
/// would wake a frontend's read source.
///
/// - Parameters:
///   - t: The terminal whose mode and crosshair decide.
///   - key: The typed byte the report leads with.
///   - pointX: The pointer's x in the view, in points.
///   - pointY: Its y, measured from the top of the view.
///   - viewWidth: The view's width in points.
///   - viewHeight: Its height.
/// - Returns: Whether the press was the trigger and a report was queued.
[[nodiscard]] bool tekReportGraphicInputKey(Ubiquitty::Terminal& t, char key, float pointX, float pointY, float viewWidth, float viewHeight);

/// Moves the graphic-input crosshair as the pointer moves.
///
/// A no-op while GIN is off, so a frontend may forward every motion event
/// without first asking whether the crosshair is up.
///
/// - Parameters:
///   - x: Horizontal position in Tek space.
///   - y: Vertical position in Tek space.
void tekUpdateGINPosition(Ubiquitty::Terminal& t, int x, int y);

// Read-only accessors for frontends (portable types only).

/// The number of primitives in the working frame.
[[nodiscard]] int tekPrimitiveCount(const Ubiquitty::Terminal& t);

/// One primitive of the working frame.
///
/// - Parameters:
///   - index: A position below `tekPrimitiveCount`.
/// - Returns: The primitive, or null when the index is out of range.
[[nodiscard]] const Ubiquitty::Tek::Primitive* tekPrimitive(const Ubiquitty::Terminal& t, int index);

/// The working frame's array base, for iterating without a bounds check per primitive.
///
/// - Returns: A base that is never null. Only the first `tekPrimitiveCount`
///   entries are live; the rest are unspecified.
[[nodiscard]] const Ubiquitty::Tek::Primitive* tekPrimitives(const Ubiquitty::Terminal& t);

/// The number of primitives in the committed frame the renderer shows.
[[nodiscard]] int tekDisplayCount(const Ubiquitty::Terminal& t);

/// The committed frame's array base, holding what was current at the last PAGE.
///
/// - Returns: A base that is never null, with `tekDisplayCount` live entries.
[[nodiscard]] const Ubiquitty::Tek::Primitive* tekDisplayPrimitives(const Ubiquitty::Terminal& t);

/// The primitives a frame shows: the committed frame, else the working one.
///
/// The fallback is what makes store-mode and a static draw visible before the
/// first PAGE commits anything, and every renderer needs the same rule — a
/// second derivation of it is a frame that flickers empty on one arm and not
/// the other. The span is borrowed from the terminal for the frame being
/// painted; the model lock the caller already holds is its lifetime.
///
/// - Parameter t: The terminal.
/// - Returns: The live primitives, possibly empty.
[[nodiscard]] Ubiquitty::Tek::PrimitiveSpan tekVisiblePrimitives(const Ubiquitty::Terminal& t);

/// Commits the working frame to the display buffer.
///
/// Called by the frontend on idle, so a frame is shown whole rather than as the
/// partial state a mid-stream read would find.
///
/// - Parameters:
///   - t: The terminal whose working frame becomes the displayed one.
void tekCommitFrame(Ubiquitty::Terminal& t);

/// The alpha cursor's horizontal position, in Tek space.
[[nodiscard]] int tekAlphaCursorX(const Ubiquitty::Terminal& t);

/// The alpha cursor's vertical position, in Tek space.
[[nodiscard]] int tekAlphaCursorY(const Ubiquitty::Terminal& t);

/// The line style subsequent vectors are drawn with.
[[nodiscard]] Tek::LineStyle tekLineStyle(const Ubiquitty::Terminal& t);

/// The character size subsequent alpha text is drawn at.
[[nodiscard]] Tek::CharSize tekCharacterSize(const Ubiquitty::Terminal& t);

/// Whether the graphic-input crosshair is up and awaiting a press.
[[nodiscard]] bool tekGINActive(const Ubiquitty::Terminal& t);

/// The crosshair's horizontal position, in Tek space. Unspecified while GIN is off.
[[nodiscard]] int tekGINX(const Ubiquitty::Terminal& t);

/// The crosshair's vertical position, in Tek space. Unspecified while GIN is off.
[[nodiscard]] int tekGINY(const Ubiquitty::Terminal& t);

/// The Tek coordinate space's width, which is the storage tube's and not the window's.
///
/// A constant: the terminal is taken so a caller reads the geometry through the
/// same handle it reads everything else, and so this can become per-terminal
/// without changing a call site.
[[nodiscard]] int tekScreenWidth(const Ubiquitty::Terminal& t);

/// The Tek coordinate space's height, on the same terms as `tekScreenWidth`.
[[nodiscard]] int tekScreenHeight(const Ubiquitty::Terminal& t);

} // namespace Ubiquitty
