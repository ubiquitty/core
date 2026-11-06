// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"
#include "Base/SwiftInterop.h"
#include "Core/Cursor/RenderState.h"
#include "Core/PaletteRGB.h"
#include "Core/Rendering/CellInstance.h"
#include "Core/Rendering/CellMetrics.h"
#include "Core/Rendering/LetterLayout.h"
#include "Core/Rendering/OverlayGeometry.h"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The overlay stream: which tiers draw, and in what order.
//
// The order is a chain of reasons, not a list — a list is what a reader
// copies, the chain is what decides where the next tier goes:
//
//   transient wash        (bell flash: beneath everything, so every annotation
//                          stays legible while the viewport pulses)
//   < viewport furniture  (the DECCOLM-132 content edge: where the terminal
//                          ends, beneath anything about its contents)
//   < asked-for emphasis  (search matches: locate without competing with
//                          anything the user is doing)
//   < host-proposed       (the mode-1001 hilite band)
//   < user-configured     (trigger highlights: the user's own rule firing
//                          outranks the host's proposal)
//   < user-dragged        (selection: what the user is doing right now wins
//                          over every standing rule)
//   < chrome              (prompt marks, then the cursor, then the password
//                          badge: the point of interaction is never buried,
//                          and the badge takes the cell after the caret so
//                          standing it highest costs nothing and no caret
//                          shape can swallow the one mark that says the
//                          keystrokes are not echoing)
//
// Membership is decided here too: the flash draws only at a live alpha, the
// 132 edge only when the leftover strip exceeds a cell, the cursor never on a
// DECSSDT status row. A frontend supplies resolved state and capacity; it does
// not choose what draws or in what order, because two frontends choosing
// independently disagree about, for one, whether the trigger or the hilite tier
// wins.
//
// Timing stays with the frontend: the flash and blink alphas arrive as
// numbers. Every oracle here compares static frames, so an envelope evaluated
// inside this seam would be motion nothing can hold.

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Rendering {

/// A read-only run of row spans, one tier's worth.
///
/// Named rather than spelled `std::span<const SelectionRowSpan>` at each use
/// because `std::span`'s extent parameter has no Swift spelling — the alias is
/// the interop annotation, the same reason ``Selection::ViewportSpans`` exists.
using ConstRowSpans = std::span<const SelectionRowSpan>;

/// The overlay stream a frame writes into.
using OverlayStream = std::span<OverlayInstance>;

/// The cursor a frame draws, or none.
///
/// A negative `row` means no cursor this frame. A focused block is the cursor
/// tier: its cell re-emitted with the cursor colour crossfaded in by `mixA`
/// and the glyph kept visible on top, drawn after the grid; every other case
/// — bar, underline, unfocused hollow — is one `OverlayInstance` from
/// ``OverlayGeometry::buildCursorQuad``.
struct UBIQUITTY_ESCAPABLE CursorRenderState {
    int row = -1;
    int column = 0;
    float mixA = 0.0f; // cross-fade weight (cursor.a/255 * blinkAlpha)
    Vec4 rgb { 0.0f, 0.0f, 0.0f, 0.0f }; // cursor color, 0..1
    bool shouldInvertText = false;
    /// Whether the program named the color of the glyph under a focused block
    /// with kitty's `cursor_text`; that color, 0..1, then takes the place of
    /// `shouldInvertText`'s contrast.
    bool hasTextColor = false;
    Vec4 textRGB { 0.0f, 0.0f, 0.0f, 1.0f };
    Ubiquitty::Cursor::Shape shape = Ubiquitty::Cursor::Shape::Block;
    bool focused = true;

    /// The columns the cursor's glyph claims, from its anchor at `column`.
    ///
    /// One for ordinary text. The builder's walk sets it to the claim when
    /// the cursor stands anywhere inside a wide pair, a sized block or a
    /// proportional run, and moves `column` to the anchor, so the underline
    /// and the hollow block span the glyph and the bar seats at its edge.
    int widthCells = 1;

    /// DECDWL/DECDHL on the cursor's row: the bar, underline and hollow outline
    /// scale and sit like the glyphs beside them. The focused block is the
    /// tier over its already-doubled cell instead.
    std::uint8_t lineAttribute = 0;

    /// Motion smoothing: while a slide is in flight the quad is placed at the
    /// fractional cell, and a focused block draws as a filled moving quad
    /// because the walk emits no cursor tier for a block in flight.
    bool isSliding = false;
    float columnFractional = 0.0f;
    /// Fractional offset from the logical grid cursor to a rich-text caret.
    float richTextOffset = 0.0f;
    float vrowFractional = 0.0f;
};

/// What the OSC 133 prompt-mark pass needs, as one field rather than five.
///
/// The bracket is placed against the FONT rather than the cell: its spine is a
/// fraction of the point size and it is centred on the glyph ink box, so the
/// ascent and descent here are the face's own and not `metrics.baselineFromTop`
/// with the line spacing folded in. Substituting the cell-derived pair gives the
/// right answer only while line spacing is zero — the same distinction
/// `cellHeightPoints` carries, and the same failure where it does not hold.
///
/// Grouped because a caller that fills some of these and not the others gets
/// degenerate geometry rather than a diagnostic, and five sibling fields on
/// ``BuildInputs`` is an invitation to set four. A backend either fills this or
/// leaves it whole.
struct UBIQUITTY_NONESCAPABLE PromptMarkInputs {
    /// Optional semantic ink for successful commands; zero alpha keeps palette ink.
    /// Negative alpha routes the mark to the frontend's native vibrancy plane.
    Vec4 ink {};
    /// One mark byte per visible row, `BuildInputs::rows` of them.
    ///
    /// Absent means no mark pass at all, which is also how a caller says the
    /// alternate screen owns the viewport: marks belong to the primary, so a
    /// backend drawing the alternate has none to give and needs no flag to be
    /// told so.
    const std::uint8_t* rowMarks = nullptr;
    /// The face's point size, and its ascent, descent and line-gap in pixels.
    double fontSize = 0.0;
    float fontAscent = 0.0f;
    float fontDescent = 0.0f;
    float lineGapTopInPixels = 0.0f;
    /// The face's `[` ink above and below the baseline, atlas pixels; zero
    /// when unmeasured.
    float bracketInkTopInPixels = 0.0f;
    float bracketInkBottomInPixels = 0.0f;
    /// The face's x-height, atlas pixels; zero when unmeasured. The search
    /// pills fall back to the band it names (``searchPillTopInset``).
    float xHeight = 0.0f;
    /// The face's cap height, atlas pixels; zero when unmeasured. The search
    /// pills centre on the band it names (``searchPillTopInset``).
    float capHeight = 0.0f;
    /// The atlas oversample: how many atlas pixels a device pixel spans.
    ///
    /// Carried rather than recovered from the cell metrics. `atlasToDevicePixels`
    /// measures the atlas against the CELL, so anything that resizes the cell
    /// without resizing the atlas — DECCOLM-132 compressing the grid to fit a
    /// fixed window is the one that ships — moves it while the oversample has not
    /// changed. A ratio taken from it is the oversample times the compression,
    /// and at the default scale the two agree, so the error appears only in the
    /// mode nobody renders while checking.
    ///
    /// Zero means one: a backend that rasterises at device resolution has no
    /// oversample to declare.
    float atlasScale = 0.0f;
};

/// Everything the overlay stream draws from, borrowed for the call.
///
/// One coordinate space: every quad lands in viewport pixels, with
/// `originInPixels` applied uniformly. The tiers share one buffer and one
/// shader, so a tier that skips the origin beside one that carries it draws a
/// selection displaced by exactly the padding — the space is a property of the
/// stream, not of a tier.
struct UBIQUITTY_NONESCAPABLE OverlayAssemblyInputs {
    GridSize grid;
    FloatSize cellSizeInPixels;
    FloatPoint originInPixels;
    FloatSize viewportSizeInPixels;
    /// Each visible row's DEC line attribute (Core/RowMeta.h), `grid.rows`
    /// entries, or null for single-width rows throughout. A span tier's
    /// columns are logical, and a doubled row draws each across two cells.
    const std::uint8_t* lineAttributes = nullptr;
    /// Device pixels per point, for hairline widths.
    float deviceScale = 1.0f;
    /// Whether the frame draws the bitmap tier, whose prompt mark is sized in
    /// device pixels as the software renderer sizes it.
    bool isBitmapFace = false;

    /// The bell flash's current strength, 0 for none. The envelope is the
    /// frontend's clock; this seam sees only the number.
    float bellFlashAlpha = 0.0f;
    /// Whether DECCOLM-132 is compressing the grid inside a wider window.
    bool isColumns132 = false;
    /// The default foreground, for the flash wash and the 132 edge.
    PaletteRGB defaultForeground;

    ConstRowSpans otherSearchMatchSpans;
    /// The focused search match. Its cells hold a flat fill of the highlight;
    /// the assembly draws the pill around them — the rounded ends reaching
    /// past the cells, above whatever the neighbours drew, and the shadow —
    /// and animates the pill's bounce. A frontend with no focused match
    /// leaves the spans empty.
    ConstRowSpans searchMatchSpans;
    Vec4 searchMatchHighlight { 0.0f, 0.0f, 0.0f, 0.0f };
    /// Seconds since the focused match took focus, or negative for a pill at
    /// rest; the bounce's clock, which a frontend reports as a fact.
    float focusedMatchAgeSeconds = -1.0f;
    ConstRowSpans hiliteRegionSpans;
    ConstRowSpans triggerHighlightSpans;
    ConstRowSpans selectionSpans;
    Vec4 selectionColor { 0.0f, 0.0f, 0.0f, 0.0f };

    /// Where each column's letter stands on the rows a proportional primary
    /// face laid out as proportional runs (LetterLayout.h): `grid.rows *
    /// grid.columns` extents, read for a row whose `letterRows` byte is set.
    /// Every span tier and the cursor stand on the letters there rather than
    /// at the pitch. Null keeps every row on the grid.
    const LetterExtent* letters = nullptr;
    const std::uint8_t* letterRows = nullptr;

    CursorRenderState cursor;
    /// A DECSSDT status line occupies the bottom grid row: the data cursor
    /// must not paint there, and the suppression is membership, so it lives
    /// here rather than in each frontend's gating.
    bool hasStatusLineOnBottomRow = false;
    float cellHeightPoints = 0.0f;
    float lineSpacingPoints = 0.0f;
    /// The toolkit caret a bar cursor draws as; absent, the bar is Core's capsule.
    ToolkitCaret toolkitCaret;

    PromptMarkInputs promptMarks;
    /// Whether to stand a lock badge after the caret.
    ///
    /// The terminal's own state is not enough to decide this: a frontend with a
    /// view layer draws its own badge above the frame, and a second one in the
    /// frame would be two locks. So the caller states the intent rather than the
    /// fact, the way it states its selection spans — a backend that draws its
    /// own leaves this false and nothing here asks why.
    bool shouldDrawPasswordLock = false;
    /// Whether the terminal is taking a password. Read only when the caller
    /// asked for the badge, so a frontend drawing its own is never consulted.
    bool isPasswordPromptActive = false;
    /// The palette the prompt-mark and lock colours resolve against; may be
    /// null when `promptMarks.rowMarks` is and no lock is asked for.
    const TermPalette* palette = nullptr;
};

/// Builds the prompt-mark parameters an arm composites its padding with.
///
/// The same derivation the overlay assembly makes, offered to the arms that
/// cannot use the overlay stream for this: the brackets sit in the padding, and
/// a CPU arm renders the grid first and puts the padding around it afterwards,
/// so a quad in the gutter is either outside the surface the renderer drew or
/// under the background the padding paints.
///
/// - Parameters:
///   - palette: The palette the mark colours resolve against.
///   - grid: The visible grid.
///   - cellSizeInPixels: The cell the grid was drawn with.
///   - origin: Where the grid's top-left sits in the padded image.
///   - viewportWidthInPixels: The padded image's width, which with the origin
///     and the grid gives both gutters.
///   - cellHeightPoints: The cell height in points; zero leaves the geometry
///     dividing by nothing and falling back to a fixed baseline fraction.
///   - face: The face's own metrics, whose `[` the brackets centre on.
/// - Returns: The parameters, whose `fontSize` is zero when the face has none.
[[nodiscard]] PromptMarkParameters promptMarkParametersForPadding(const TermPalette& palette, GridSize grid, FloatSize cellSizeInPixels,
    FloatPoint origin, float viewportWidthInPixels, double cellHeightPoints, const FaceMetrics& face);

/// Composites the prompt-mark brackets into a padded RGBA image.
///
/// Reads the terminal's row marks directly, because which rows carry a mark is
/// the one input an arm cannot have resolved earlier: the marks move under a
/// scroll and the padded image is built after the frame.
///
/// - Parameters:
///   - parameters: What ``promptMarkParametersForPadding`` returned.
///   - terminal: The terminal whose row marks are read. The alternate screen
///     carries none, and this draws nothing there.
///   - firstAbsoluteRow: The absolute row the viewport's top row is, so a
///     scrolled-back view marks the rows the user is looking at.
///   - rgba: The padded image, written in place.
///   - surface: That image's size.
void compositePromptMarksIntoPadding(const PromptMarkParameters& parameters, const Ubiquitty::Terminal& terminal, int firstAbsoluteRow,
    std::vector<std::uint8_t>& rgba, IntSize surface);

/// Assembles the frame's overlay stream in the canonical order.
///
/// Writes as many instances as fit and counts them all: a return larger than
/// `out.size()` is the grow-and-retry signal, sized by
/// ``maxOverlaysForRows``. Membership rules (a dead flash, a sub-cell 132
/// strip, a cursor on the status row, an off-screen span) are applied here, so
/// two callers cannot disagree about what draws.
///
/// - Parameters:
///   - in: The resolved per-tier state.
///   - out: Receives the stream; its size bounds what is written, never what
///     is counted.
/// - Returns: The number of instances the inputs produce, written or not.
[[nodiscard]] int assembleOverlays(const OverlayAssemblyInputs& in, OverlayStream out);

} // namespace Ubiquitty::Rendering
