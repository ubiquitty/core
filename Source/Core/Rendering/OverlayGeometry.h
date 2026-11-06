// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"
#include "Base/SwiftInterop.h"

// OverlayGeometry — renderer-neutral pixel math for the cursor, selection
// highlight, and scrollback-position indicator. Frontends gather inputs, call
// these, and upload the resulting OverlayQuad in their own vertex form. No simd
// / AppKit / Metal here — plain floats, device pixels, origin top-left.
// Selection RESOLUTION stays in Source/Selection; this is only the cell-rect →
// pixel-rect + cursor-shape math.

#include "Core/CellGrid.h"
#include "Core/Modes/TerminalModes.h"
#include "Core/Rendering/CellInstance.h"
#include "Core/RowMeta.h"

#include <cstdint>
#include <string_view>

namespace Ubiquitty::Rendering {

// One overlay rectangle in device pixels. cornerRadius>0 → capsule/rounded
// corners; strokeWidth>0 → hollow outline of that width. (Shades, Powerline SDF,
// and block-mosaic masks belong to glyph quads, not overlays.)
/// How a rounded corner curves.
enum class CornerStyle : std::uint8_t {
    /// A circular arc: the corner meets the straight edges at a tangent, and the
    /// curvature jumps from zero to 1/r at that point.
    Circular,
    /// A superellipse, curving into the edges instead of meeting them. This is
    /// the corner macOS draws everywhere, and beside it a circular one reads as
    /// an arc pasted onto a rectangle.
    Continuous,
};

struct OverlayQuad {
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    float red = 0.0f, green = 0.0f, blue = 0.0f, alpha = 0.0f;
    float cornerRadius = 0.0f;
    float strokeWidth = 0.0f;
    CornerStyle cornerStyle = CornerStyle::Circular;
};

// A renderer-neutral RGBA color (channels 0..1), used for the baked-in overlay
// colors a frontend can't theme yet.
struct OverlayColor {
    float red, green, blue, alpha;
};

// The selection-highlight tint, baked here so every frontend's selection overlay
// matches. A translucent slate tone, readable over the default dark background.
// A single fixed value keeps the CPU↔GPU oracle in lockstep.
constexpr OverlayColor selectionHighlightColor() { return { 0.40f, 0.55f, 0.85f, 0.35f }; }

/// How much of a native selection's colour lies over a cell's own background:
/// a wash the way the desktop's text views draw one, through which a coloured
/// background still reads as itself, rather than an opaque block that erases
/// it. The remainder is the cell's own background.
constexpr float nativeSelectionOpacity = 0.62f;

/// Where a selection's colour goes: under the text, or over it.
enum class UBIQUITTY_CLOSED_ENUM SelectionStyle : std::uint8_t {
    /// The platform's own selection colour, washed over the cell's own
    /// background under the glyphs — what a text view on the same desktop
    /// draws. Where the platform reports no colour, the selection falls back
    /// to ``Tint``.
    Native,
    /// ``selectionHighlightColor``, translucent, over the glyphs.
    Tint,
};

/// The style a frame draws its selection in when the preferences name none.
inline constexpr SelectionStyle defaultSelectionStyle = SelectionStyle::Native;

/// Resolves a configuration token, using the default style for unknown tokens.
///
/// Inline, with the resolver below, because the preferences read it and the
/// headless daemon, which reads preferences, does not build this module.
///
/// - Parameter name: The token.
/// - Returns: The style it names.
[[nodiscard]] constexpr SelectionStyle selectionStyleFromString(std::string_view name)
{
    if (name == "native")
        return SelectionStyle::Native;
    if (name == "tint")
        return SelectionStyle::Tint;
    return defaultSelectionStyle;
}

/// Returns the configuration token for a selection style.
///
/// - Parameter style: The style.
/// - Returns: Its token.
[[nodiscard]] constexpr std::string_view selectionStyleToString(SelectionStyle style)
{
    switch (style) {
    case SelectionStyle::Native:
        return "native";
    case SelectionStyle::Tint:
        return "tint";
    }
    return "native";
}

/// The colour a frame fills its selected cells with, and where.
struct SelectionFill {
    /// RGBA 0..1. Opaque when ``isBehindText``.
    Vec4 color { 0.0f, 0.0f, 0.0f, 0.0f };
    /// Whether the fill is each selected cell's background, under its glyph,
    /// rather than a translucent quad over it.
    bool isBehindText = false;
};

/// Resolves the selection's fill from the style, the platform and the host.
///
/// A host colour (`OSC 17`) names a colour and not an opacity, so it takes the
/// place of whichever colour the style would otherwise draw and keeps that
/// style's placement: under the text at ``nativeSelectionOpacity`` for
/// ``SelectionStyle/Native``, the tint's alpha over it for
/// ``SelectionStyle/Tint``. A native style with neither a host colour nor a
/// platform one has nothing native to draw and draws the tint.
///
/// - Parameters:
///   - style: The configured style.
///   - platformColor: The platform's selection background, sRGB 0..1, or zero
///     alpha where the platform reports none.
///   - hostColor: The `OSC 17` colour, RGB 0..1, when the host named one.
/// - Returns: The fill every renderer draws.
[[nodiscard]] inline SelectionFill resolveSelectionFill(SelectionStyle style, Vec4 platformColor, const Vec4* hostColor)
{
    const OverlayColor tint = selectionHighlightColor();
    if (style == SelectionStyle::Native && (hostColor || platformColor.w > 0.0f)) {
        const Vec4 color = hostColor ? *hostColor : platformColor;
        return { { color.x, color.y, color.z, nativeSelectionOpacity }, true };
    }
    if (hostColor)
        return { { hostColor->x, hostColor->y, hostColor->z, tint.alpha }, false };
    return { { tint.red, tint.green, tint.blue, tint.alpha }, false };
}

// The hilite mouse-tracking (DEC mode 1001) region tint — a warmer amber than
// the slate selection so a host-driven highlight reads as distinct. Baked here.
constexpr OverlayColor hiliteRegionColor() { return { 0.85f, 0.65f, 0.30f, 0.35f }; }

/// One span on one visible row, inclusive at both columns.
///
/// Every overlay tier that tints cells rather than drawing a shape resolves to
/// these: the selection, the non-focused search matches, the trigger highlights
/// and the hilite band. The frontend resolves them from its own state, so the
/// builder needs no callback and emits one translucent `OverlayInstance` per
/// span over the cells it covers.
struct UBIQUITTY_ESCAPABLE SelectionRowSpan {
    int visibleRow = 0;
    int column0 = 0;
    int column1 = 0;
    /// Code points of `column0`'s text the span leaves out, and of `column1`'s
    /// it keeps; zero is the whole cell. Nonzero only where a character
    /// selection's end stands inside a proportional run's cell, between two
    /// letters whose ink begins in it: the same cut the copy takes, so the
    /// letters lit are the letters taken.
    int subColumn0 = 0;
    int subColumn1 = 0;
    /// The letters of a scaled proportional run the span lights, `[firstLetter,
    /// endLetter)` of the run's text, as the selection holds them; equal names
    /// none, and -1 has the renderer find them from the columns and cuts by its
    /// own layout. Core
    /// names them where the measurer's layout and the renderer's may put a
    /// letter in different cells, so the letters lit are the letters held.
    int firstLetter = -1;
    int endLetter = -1;

    friend constexpr bool operator==(const SelectionRowSpan&, const SelectionRowSpan&) = default;
};

/// Resolves a hilite band into the rows it covers.
///
/// The host sends four 1-based scalars in one `CSI T` and the frontend has to
/// turn them into rows: clamp the band into the live screen, start at the given
/// column on its first row and at zero after, and drop rows the viewport has
/// scrolled away from. That derivation is the same on every frontend and was
/// written out per frontend, which is one copy per renderer of a rule the host
/// states once.
///
/// - Parameters:
///   - startColumn: Host's 1-based start column.
///   - startRow: Host's 1-based start row, in screen rows.
///   - firstRow: Host's 1-based first row of the band.
///   - lastRow: Host's 1-based last row of the band.
///   - rows: Visible rows, which bounds the band.
///   - columns: Visible columns, which bounds the start column.
///   - viewOffset: Scrollback rows above the viewport.
///   - out: Receives one span per covered row, cleared first.
///   - maxOut: Capacity of `out`.
/// - Returns: How many spans were written, which is zero for an empty or
///   inverted band.
int hiliteRegionRowSpans(int startColumn, int startRow, int firstRow, int lastRow, int rows, int columns, int viewOffset, SelectionRowSpan* out, int maxOut);

/// Resolves one row of a hilite band into its tinted quad.
///
/// - Parameters:
///   - visibleRow: Row in the viewport, zero at the top.
///   - column0: First tinted column, inclusive.
///   - column1: Last tinted column, inclusive.
///   - cellSizeInPixels: Cell size the frame was laid out at.
///   - origin: Grid origin in the same pixel space.
///   - out: Receives the quad when one is produced.
/// - Returns: Whether a quad was written, which is false for an inverted span.
bool hiliteRegionQuadAtVisibleRow(int visibleRow, int column0, int column1, FloatSize cellSizeInPixels, FloatPoint origin, OverlayQuad& out);

// Map an inclusive [col0,col1] cell span on visible row `vrow` to a solid
// pixel quad in the given color. Shared by the selection + search
// highlights (the cursor block uses the same formula with col0==col1).
// Caller guarantees col1 >= col0 and that vrow is on-screen.
/// Rows of scrollback the absolute-to-visible mapping should assume.
///
/// A selection gesture encodes its rows in absolute buffer space, and the decode
/// maps them back with `visible = absolute - history + viewOffset`. On the
/// alternate screen the gesture forces history to 0, because the alternate
/// buffer has no scrollback of its own — so the decode has to zero it too. The
/// raw count there is the PRIMARY buffer's surviving scrollback, and using it
/// shifts every alternate-screen highlight off the pointer by that many rows.
///
/// - Parameters:
///   - buffer: The buffer the gesture was made against.
///   - scrollbackCount: The primary buffer's scrollback row count.
/// - Returns: The history to decode against.
constexpr int selectionHistoryRows(Modes::ScreenBuffer buffer, int scrollbackCount)
{
    return buffer == Modes::ScreenBuffer::Alternate ? 0 : scrollbackCount;
}

/// Builds the quad covering an inclusive run of cells on one visible row.
///
/// The span is inclusive at both ends, which is what the trigger engine reports;
/// a caller holding a half-open `[column, column + length)` passes
/// `column + length - 1`.
///
/// - Parameters:
///   - column0: First column of the run.
///   - column1: Last column of the run, inclusive.
///   - visibleRow: Row within the viewport, not an absolute row.
///   - cellSizeInPixels: One cell's extent in device pixels.
///   - origin: The grid's top-left corner in device pixels.
///   - color: The tint, channels 0..1.
/// - Returns: The quad, with corner radius and stroke left at zero for the
///   caller to set.
OverlayQuad cellSpanQuad(int column0, int column1, int visibleRow, FloatSize cellSizeInPixels, FloatPoint origin, OverlayColor color);

/// Which of the two weights a search match is painted at.
///
/// The find grammar every native control uses: the hit the user is on reads
/// brightly, the rest stay neutral so they locate without competing.
enum class SearchMatchEmphasis : std::uint8_t {
    /// One of the other matches on screen.
    Other,
    /// The match the user is currently on.
    Focused
};

/// Places one scrollback-search match's highlight quad.
///
/// - Parameters:
///   - absoluteRow: The row in the scrollback-plus-live address space.
///   - column: Where the matched span starts.
///   - length: How many columns it covers, `[column, column + length)`.
///   - history: Rows of scrollback above the live grid.
///   - viewOffset: How far the viewport has scrolled back.
///   - rows: The visible row count, which bounds the mapped row. Together these
///     three map the match: `vrow = absoluteRow - history + viewOffset`.
///   - cellSizeInPixels: One cell's extent.
///   - origin: The grid's top-left corner in the surface.
///   - emphasis: Which weight to paint it at.
///   - out: Receives the quad.
/// - Returns: False, drawing nothing, when the match scrolled off-screen or the
///   span is empty.
bool searchMatchQuad(int absoluteRow, int column, int length, int history, int viewOffset, int rows, FloatSize cellSizeInPixels, FloatPoint origin, SearchMatchEmphasis emphasis, OverlayQuad& out);

/// Places one non-focused match's highlight quad from a row already mapped into
/// the viewport.
///
/// How far a search pill stands in from its row's top and bottom.
struct SearchPillInsets {
    float top = 0.0f; ///< Device pixels between the row's top and the pill's.
    float bottom = 0.0f; ///< Device pixels between the pill's bottom and the row's.
};

/// The room a search pill leaves its row, in points, above and below.
///
/// One point each side, so pills on consecutive rows stand apart rather than
/// meeting in a band, and a pill reads as surrounding its words rather than as
/// the row's own ground.
inline constexpr float searchPillBreathingPoints = 1.0f;

/// Where a search pill stands in its row, so it centres on the row's text.
///
/// Text sits optically centred in a container when its cap-height band — from
/// the baseline up to the capitals — is: that is the band ascenders and
/// capitals reach and the eye reads as the line, and the one badges and find
/// highlights are balanced on. Centring on the lowercase band alone leaves a
/// word with ascenders sitting low in its pill, with twice the space under
/// the baseline that it has above the letters. A face with no cap height
/// falls back to the lowercase band, from the baseline up by the x-height.
///
/// The pill is centred on the band's midpoint and reaches as far each way as
/// the nearer row edge allows, less ``searchPillBreathingPoints``, so the room
/// above the capitals equals the room below the baseline and a pill never
/// reaches into the row below, whose cursor and ascenders the overlay pass
/// would paint over. Whole device pixels so the straight edges stay crisp; at
/// least two thirds of the row tall, so an eccentric face cannot squeeze the
/// pill to a line. Zero both ways when the face's ascent and both heights are
/// unmeasured, which keeps the pill on the cell's box rather than guessing.
///
/// - Parameters:
///   - cellHeightInPixels: The row's height in device pixels.
///   - cellHeightPoints: The row's height in points; zero reads as one pixel
///     per point.
///   - atlasScale: Atlas pixels per point, which the face metrics are in.
///   - fontAscent: Baseline to the top of the type, atlas pixels.
///   - lineGapTopInPixels: The line gap above the ascent, atlas pixels.
///   - xHeight: The face's x-height, atlas pixels; zero when unmeasured.
///   - capHeight: The face's cap height, atlas pixels; zero when unmeasured.
/// - Returns: The pill's insets from its row, in device pixels.
[[nodiscard]] SearchPillInsets searchPillInsets(float cellHeightInPixels, double cellHeightPoints, double atlasScale, float fontAscent, float lineGapTopInPixels, float xHeight, float capHeight);

/// The same quad `searchMatchQuad` produces for `SearchMatchEmphasis::Other`,
/// reached by a caller that resolved the visible row itself. The builder holds
/// spans in visible-grid coordinates because that is what the cell walk works
/// in, while a frontend driving the search engine holds absolute rows; both
/// arrive here so one geometry answers for the highlight's inset, radius and
/// corner style rather than each side deriving them.
///
/// There is no focused counterpart. That match is a bake into its own cells, so
/// there is no quad to place at any coordinate.
///
/// - Parameters:
///   - visibleRow: The row within the viewport, already mapped.
///   - column0: First column of the span, inclusive.
///   - column1: Last column of the span, inclusive.
///   - cellSizeInPixels: One cell's extent.
///   - origin: The grid's top-left corner in the surface.
///   - out: Receives the quad.
/// - Returns: False, drawing nothing, when the span is empty or inverted.
bool otherSearchMatchQuadAtVisibleRow(int visibleRow, int column0, int column1, FloatSize cellSizeInPixels, FloatPoint origin, OverlayQuad& out);

/// ``otherSearchMatchQuadAtVisibleRow`` stood in from its row by
/// ``searchPillInsets``' answer.
///
/// - Parameters:
///   - visibleRow: The row on screen.
///   - column0: The first column of the match.
///   - column1: The last column of the match, inclusive.
///   - cellSizeInPixels: The cell size.
///   - origin: The grid's origin in pixels.
///   - insets: How far the pill stands in from the row's top and bottom.
///   - out: Receives the quad.
/// - Returns: Whether a quad was produced.
bool otherSearchMatchQuadAtVisibleRow(int visibleRow, int column0, int column1, FloatSize cellSizeInPixels, FloatPoint origin, SearchPillInsets insets, OverlayQuad& out);

/// The colour a focused match's highlight is drawn in.
///
/// Named so a caller can ask what is behind the glyph without re-deriving it.
/// The builder computes the contrasting ink from this, so a highlight tuned
/// lighter or darker carries its text with it rather than leaving a caller to
/// re-tune a constant somewhere else.
[[nodiscard]] OverlayColor focusedSearchMatchColor();

/// The focused match's pill, in fractions of the row height: the corner
/// radius, which is also how far the pill reaches past the matched cells
/// each side so the arc ends exactly at the cells' edge and the cells
/// themselves hold a flat fill; the reach of the shadow it casts; and the
/// shadow's opacity at the pill's edge. Safari's find highlight is the
/// reference: a pill padded past the word, its ends above the neighbouring
/// letters, a short soft shadow.
inline constexpr float focusedPillCornerFraction = 0.22f;
inline constexpr float focusedPillShadowFraction = 0.25f;
inline constexpr float focusedPillShadowAlpha = 0.35f;

/// How long the pill's bounce runs after a match takes focus, in seconds.
/// A frontend keeps its frame clock armed for this long after the active
/// match changes; past it the pill is at rest and no frame is owed.
inline constexpr float focusedPillBounceSeconds = 0.35f;

/// The `OverlayInstance::shapeKind` of the focused pill's ring: `plA` is the
/// pill's rect, `plB` the shadow's reach and opacity and the x extent of the
/// cells' box the fragment keeps the destination inside, `cornerRadius` the
/// pill's. Negative because the Powerline kinds are positive and the mosaic
/// mask is minus one.
inline constexpr float focusedPillRingShapeKind = -3.0f;

/// The factor the focused pill's rect scales by about its centre at a given
/// age since the match took focus: a damped spring that overshoots to about
/// a fifth larger in the first fifty milliseconds, dips a little under, and
/// rests at one. A negative age is a pill at rest, which is what a snapshot
/// renders.
///
/// - Parameter ageSeconds: Seconds since the match took focus; negative for
///   a pill with no animation.
/// - Returns: The scale factor, one at rest.
[[nodiscard]] float focusedPillScale(float ageSeconds);

/// Places one Triggers `highlight`-action match's quad.
///
/// Maps the row exactly as ``searchMatchQuad`` does, and tints a translucent
/// violet so a trigger match cannot be mistaken for find's yellow or orange.
///
/// - Parameters:
///   - absoluteRow: The row in the scrollback-plus-live address space.
///   - column0: The first matched column, INCLUSIVE.
///   - column1: The last matched column, also INCLUSIVE — unlike
///     ``searchMatchQuad``'s half-open span.
///   - history: Rows of scrollback above the live grid.
///   - viewOffset: How far the viewport has scrolled back.
///   - rows: The visible row count, which bounds the mapped row.
///   - cellSizeInPixels: One cell's extent.
///   - origin: The grid's top-left corner in the surface.
///   - out: Receives the quad.
/// - Returns: False, drawing nothing, when the row scrolled off-screen or the
///   span is empty.
bool triggerHighlightQuad(int absoluteRow, int column0, int column1, int history, int viewOffset, int rows, FloatSize cellSizeInPixels, FloatPoint origin, OverlayQuad& out);

/// Places one trigger highlight's quad from a row already mapped into the
/// viewport.
///
/// The same quad `triggerHighlightQuad` produces, reached by a caller that
/// resolved the visible row itself. The builder holds spans in visible-grid
/// coordinates because that is what the cell walk works in, while a frontend
/// reading `Triggers::highlightAt` holds absolute rows; both arrive here so one
/// geometry answers for the tint rather than each side choosing its own.
///
/// - Parameters:
///   - visibleRow: The row within the viewport, already mapped.
///   - column0: First column of the span, inclusive.
///   - column1: Last column of the span, inclusive.
///   - cellSizeInPixels: One cell's extent.
///   - origin: The grid's top-left corner in the surface.
///   - out: Receives the quad.
/// - Returns: False, drawing nothing, when the span is empty or inverted.
bool triggerHighlightQuadAtVisibleRow(int visibleRow, int column0, int column1, FloatSize cellSizeInPixels, FloatPoint origin, OverlayQuad& out);

/// The desktop toolkit's text insertion caret, as a frontend reports it.
///
/// A frontend whose bar must be the caret its toolkit draws in a text field
/// reports the toolkit's facts here, and the bar becomes that caret: a
/// square-cornered stem on the toolkit's logical-pixel grid, as tall as the
/// font's line and as wide as the toolkit's aspect ratio makes it. Only the
/// focused bar is the caret; unfocused, the bar is the hollow outline every
/// unfocused cursor is. GTK is the toolkit that defines it today
/// (`gtk_css_style_snapshot_caret`). A `logicalScale` of zero reports no
/// toolkit, and the bar is Core's own capsule.
struct ToolkitCaret {
    /// The stem's width per logical pixel of line height: GTK's
    /// `gtk-cursor-aspect-ratio`, read from the toolkit's settings. Zero is a
    /// valid setting, a stem one logical pixel wide at every height.
    double aspectRatio = 0.0;
    /// Device pixels per toolkit logical pixel, the window's scale factor;
    /// zero when no toolkit caret is reported.
    int logicalScale = 0;

    [[nodiscard]] bool isPresent() const { return logicalScale > 0; }
    bool operator==(const ToolkitCaret&) const = default;
};

/// The `OverlayInstance::shapeKind` of the toolkit caret: a square stem
/// blended as GTK's software renderer blends a faded caret. GtkText fades its
/// caret with an opacity node, which GSK's cairo renderer draws as
/// `cairo_paint_with_alpha` over a group holding the colour: pixman's source-over of
/// the opaque colour under an 8-bit mask, the two products each rounded on
/// their own (`MUL_UN8`). `color.w` carries that mask exactly, as
/// ``toolkitCaretMask`` divided by 255. Below the pill ring's kind; a backend
/// tests for it before it tests for the ring's.
inline constexpr float toolkitCaretShapeKind = -4.0f;

/// The 8-bit mask cairo composites an opacity with: `_cairo_color_double_to_short`
/// rounds it to sixteen bits and pixman's solid fill keeps the high byte.
///
/// - Parameter opacity: The caret's opacity, 0...1.
/// - Returns: The mask, 0...255.
[[nodiscard]] constexpr int toolkitCaretMask(double opacity)
{
    const double clamped = opacity < 0.0 ? 0.0 : (opacity > 1.0 ? 1.0 : opacity);
    return static_cast<int>(clamped * 65535.0 + 0.5) >> 8;
}

// Cursor quad inputs. column/vrow are VISIBLE-grid coords (already shifted
// by viewOffset); rows/columns bound them. blinkAlpha + focused come from the
// resolved Cursor::RenderState + focus state. cursor{R,G,B,A} is the palette
// cursor color in 0..1 (A = its own per-color opacity). cellHeightPoints +
// lineSpacingPoints drive the points→pixels scale and the line-spacing inset.
struct CursorQuadParameters {
    int column = 0;
    int vrow = 0;
    int rows = 0;
    int columns = 0;
    uint8_t lineAttribute = lineAttributeNormal; // DECDWL/DECDHL: 2× width on non-normal rows
    // The columns the cursor's glyph claims from `column`: the underline and
    // the block span them all; the bar keeps its width at the claim's edge.
    int widthCells = 1;
    Ubiquitty::Cursor::Shape shape = Ubiquitty::Cursor::Shape::Block;
    float blinkAlpha = 1.0f;
    bool focused = true;
    // Motion smoothing: when sliding, the quad is positioned at the FRACTIONAL
    // (columnFractional, vrowFractional) cell, and a focused block is drawn as a
    // FILLED moving quad (overriding the usual "focused block is the tier,
    // draw nothing" rule — the builder builds no tier while sliding). Off by default.
    bool sliding = false;
    float columnFractional = 0.0f;
    float vrowFractional = 0.0f;
    float cellWidthInPixels = 0.0f;
    float cellHeightInPixels = 0.0f;
    FloatPoint origin {};
    float cursorRed = 0.0f, cursorGreen = 0.0f, cursorBlue = 0.0f, cursorAlpha = 1.0f;
    double cellHeightPoints = 0.0;
    double lineSpacingPoints = 0.0;
    /// The toolkit caret the bar draws as, or none for Core's capsule.
    ToolkitCaret toolkitCaret;
};

// Compute the cursor overlay quad for this frame. Returns false (draw
// nothing) when faded out, off-screen, or a settled FOCUSED block — that one
// is the cursor tier, the cell re-emitted with the cursor's colours at each
// stream's tail (CellInstanceBuilder.h, InstanceCounts::cursorTier), so the
// glyph stays visible on top. The bar/underline snap to the device-pixel
// grid; the block rides the fractional cell grid.
bool buildCursorQuad(const CursorQuadParameters&, OverlayQuad& out);

// Scrollback-position thumb inputs. `alpha` is the caller-resolved fade
// (Ubiquitty::scrollIndicatorAlpha); <= 0 draws nothing. scrollbackCount /
// rows / viewOffset position the thumb (viewOffset 0 = live tail = bottom);
// viewport is the drawable size in pixels; scale is pixels-per-point.
struct ScrollIndicatorParameters {
    float alpha = 0.0f;
    int scrollbackCount = 0;
    int rows = 0;
    int viewOffset = 0;
    FloatSize viewportSizeInPixels {};
    bool isFullScreen = false;
    float scale = 1.0f;
};

// Compute the scroll-indicator thumb quad. Returns false at the live tail
// with no history, when fully faded, or for a degenerate viewport.
bool buildScrollIndicatorQuad(const ScrollIndicatorParameters&, OverlayQuad& out);

// OSC 133 prompt-mark gutter bracket inputs. The bracket is a small '[' / ']'
// pair drawn in both padding gutters next to a row whose command actually ran
// (osc133MarkCmdExecuted). Geometry scales with the font: cellHeightPoints +
// fontSize set the point→pixel scale and the spine height; fontAscent /
// fontDescent / lineGapTopInPixels (atlas pixels, divided by the atlas-space
// line height = cellHeightPoints*atlasScale) place it on the text line. okRGB /
// failRGB are the two bracket colors in 0..1; their alphas are baked.

// ANSI-16 palette indices the prompt-mark indicator colors from: theme blue for
// an ordinary executed prompt, red for a command that exited non-zero. Each
// frontend reads its own palette at these indices and passes okRGB / failRGB.
inline constexpr int promptMarkOkColorIndex = 4; // ANSI blue
inline constexpr int promptMarkFailColorIndex = 1; // ANSI red
/// How much of the mark's ink shows against the default background: an
/// ordinary prompt's mark is half blue, a failed command's nine tenths red.
/// Mixed in linear light by the assembly and drawn solid.
inline constexpr double promptMarkOkWeight = 0.5;
inline constexpr double promptMarkFailWeight = 0.9;

struct PromptMarkParameters {
    GridSize grid {};
    FloatSize cellSizeInPixels {};
    FloatPoint origin {};
    // Width alone, not a FloatSize: the bracket needs the viewport's right edge
    // to find the right gutter, and `grid` places it vertically. A height here
    // would be a field nothing reads.
    float viewportWidthInPixels = 0.0f;
    double cellHeightPoints = 0.0;
    double atlasScale = 1.0;
    double fontSize = 0.0;
    float fontAscent = 0.0f;
    float fontDescent = 0.0f;
    float lineGapTopInPixels = 0.0f;
    // The face's own `[` glyph: how far its ink reaches above the baseline
    // and below it, in atlas pixels, both non-negative. The mark is centred
    // on this box rather than on the ascent/descent midpoint: a face's
    // vertical metrics say where its line fits, its bracket says where it
    // frames a line of its own text — Monaspace Neon's `[` sits 6.7% of an
    // em below its metrics' midpoint, Fira Code's 5.6% above. Both zero means
    // unmeasured, and the midpoint stands in.
    float bracketInkTopInPixels = 0.0f;
    float bracketInkBottomInPixels = 0.0f;
    // The ink colours (theme blue / ANSI red) and the default background they
    // mix over. `buildPromptMarkBrackets` mixes them in linear light at the
    // weights above and draws the result solid, so every caller — the overlay
    // assembly and the portable compositor — softens the mark the same way.
    float okRed = 0.0f, okGreen = 0.0f, okBlue = 0.0f;
    float failRed = 0.0f, failGreen = 0.0f, failBlue = 0.0f;
    float groundRed = 0.0f, groundGreen = 0.0f, groundBlue = 0.0f;
};

// The font-invariant bracket geometry resolved once from PromptMarkParameters,
// reused for every row. `perRow` is how many quads a single executed row emits
// (0 when neither gutter has room — both drawLeft and drawRight false). The
// left/right bracket each emit 3 quads (spine + top stub + bottom stub) when
// its gutter is wide enough.
// The ceiling `perRow` reaches, when both gutters have room. Callers sizing a
// buffer for the whole grid need this rather than the resolved `perRow`, which
// is not known until the frame's geometry is.
constexpr int maxPromptMarkQuadsPerRow = 6;

struct PromptMarkGeometry {
    bool drawLeft = false;
    bool drawRight = false;
    int perRow = 0; // (drawLeft?3:0) + (drawRight?3:0)
    // resolved spine/stub metrics + gutter X positions + baseline fractions
    float thick = 0.0f;
    float barHeight = 0.0f;
    float minLeftX = 0.0f, maxLeftX = 0.0f; // left bracket spine + stub span
    float minRightX = 0.0f, maxRightX = 0.0f; // right bracket span
    float baselineFrac = 0.0f, ascentFrac = 0.0f, descentFrac = 0.0f;
    // The bracket glyph's centre as a fraction of the cell above the
    // baseline, and whether the face measured one.
    bool hasBracketInk = false;
    float bracketMidAboveBaselineFrac = 0.0f;
    float pixelsPerPoint = 1.0f;
};

// Resolve the once-per-frame bracket geometry. When both gutters are too narrow
// (perRow == 0) the caller can skip the row loop entirely.
PromptMarkGeometry resolvePromptMarkGeometry(const PromptMarkParameters&);

// Emit the prompt-mark bracket quads for visible row `rowIndex` whose command
// executed (`failed` picks the error color). Writes up to geo.perRow quads into
// out[0..]; returns the count written (0 if the gutters are too narrow, or if
// fewer than geo.perRow slots remain in `maxOut`). The cell-position math uses
// the same originY + rowIndex*cellHeight the grid walk uses.
int buildPromptMarkBrackets(const PromptMarkParameters& params, const PromptMarkGeometry& geo, int rowIndex, bool failed, OverlayQuad* out, int maxOut);

/// How much of the lock's ink shows against the background it stands on.
///
/// The badge is a state indicator beside a caret, not a warning: at full
/// foreground it competes with the text it sits after, and below about half it
/// reads as a smudge at a terminal's cell size. Mixed in linear light and drawn
/// solid, the same way a prompt mark is, so a translucent surface does not
/// thicken it.
inline constexpr double passwordLockInkWeight = 0.75;

/// The badge that says a terminal is taking a password.
///
/// A frontend with a view layer draws its own — macOS stands an SF Symbol after
/// the caret — and leaves ``OverlayAssemblyInputs`` unset for it, so that one
/// implementation in Core does not give that frontend a second lock. This is
/// for the frontends whose only surface is the frame: the Vulkan and software
/// arms, and the browser. Two quads rather than a glyph, because a glyph would
/// need a face that carries U+1F512 and a colour-emoji path to draw it, and
/// neither is a dependency a lock is worth.
struct PasswordLockParameters {
    GridSize grid {};
    FloatSize cellSizeInPixels {};
    FloatPoint origin {};
    /// The caret's cell. The badge stands in the cell after it, on its row,
    /// which is where a reader's eye already is.
    int cursorRow = -1;
    int cursorColumn = -1;
    /// The ink and the ground it mixes over, 0..1. Resolved by the caller from
    /// the palette so the badge follows a theme rather than a baked constant.
    float inkRed = 0.0f, inkGreen = 0.0f, inkBlue = 0.0f;
    float groundRed = 0.0f, groundGreen = 0.0f, groundBlue = 0.0f;
};

/// The two quads a lock is: the shackle's arc, then the body over its legs.
constexpr int maxPasswordLockQuads = 2;

/// The resolved badge, or one that does not fit.
///
/// `doesFit` is false when the caret is off-grid, when the cell after it is, or
/// when a cell is too small to hold a shape that would still read as a lock.
/// A caller checking it skips the emit entirely.
struct PasswordLockGeometry {
    bool doesFit = false;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    float bodyTop = 0.0f;
    float bodyMinX = 0.0f, bodyMaxX = 0.0f;
    float shackleMinX = 0.0f, shackleMaxX = 0.0f;
    float shackleStroke = 0.0f;
};

/// Resolves where the badge stands, once per frame.
///
/// - Parameter params: The grid, the cell size, and the caret to stand after.
/// - Returns: The resolved geometry, whose `doesFit` is false when there is no
///   room for one.
PasswordLockGeometry resolvePasswordLockGeometry(const PasswordLockParameters& params);

/// Emits the badge's quads.
///
/// The shackle is written first and the body second: the shackle is a hollow
/// rect whose lower half is not part of a lock, and the body is what covers
/// it. A caller drawing them out of order draws a keyhole-less padlock
/// with a bar through it.
///
/// - Parameters:
///   - params: The parameters the geometry was resolved from.
///   - geometry: The resolved geometry.
///   - out: Where to write, at least `maxOut` slots.
///   - maxOut: How many slots remain.
/// - Returns: The number of quads written, zero when the badge does not fit or
///   fewer than `maxPasswordLockQuads` slots remain.
int buildPasswordLockQuads(const PasswordLockParameters& params, const PasswordLockGeometry& geometry, OverlayQuad* out, int maxOut);

} // namespace Ubiquitty::Rendering
