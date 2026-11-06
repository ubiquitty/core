// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"
#include <span>

#include "Core/Rendering/BackgroundImage.h"
#include "Core/Rendering/BeamLayout.h"
#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/GlyphRasterizer.h"
#include "Core/Rendering/PaddingGeometry.h"
#include "Core/RichText/Drawing.h"
#include "Core/Tek/Colors.h"

#include <cstdint>
#include <vector>

namespace Ubiquitty {

struct Terminal;

namespace Rendering {
// Named in a parameter below; a caller that passes one includes its header.
class BundledRichText;
class SoftwareRenderer;
}

namespace Rendering {

// Deterministic terminal-to-PNG rendering through the embedded GNU Unifont
// rasterizer. Every input is portable and expressed in device pixels.
/// The pen and rule styling one frame is drawn with.
///
/// Scalars only, so a caller may keep a copy across frames: every borrowed
/// input, the per-index foreground alpha among them, lives on
/// ``TerminalSnapshotOptions`` instead, which is what lets a boundary-capture
/// registration hold a style durably.
struct TerminalSnapshotStyle {
    /// The integer upscale of a face Core rasterises itself, or 0 when the
    /// face is a CoreText or FreeType one — `unifontScaleForConfiguredFamily`'s
    /// verdict, which every frontend passes through. The software raster
    /// clamps it to at least 1, since its face is always the embedded bitmap;
    /// the frame inputs read it as the bitmap-tier fact. Zero by default so a
    /// frontend that never wrote it claims no tier, rather than the bitmap one.
    int unifontScale = 0;
    int lineSpacingInPixels = 0;
    int characterSpacingInPixels = 0;
    EdgeInsets paddingInPixels;
    float underlineThicknessScale = 1.0f;
    float underlineOffsetInPixels = 0.0f;
    bool shouldUnderlineSkipInk = false;
    /// Whether a run's outer underline reaches the letter's ink past the
    /// cell box (`[underline] follows_ink`).
    bool shouldUnderlineFollowInk = false;
    float underlineSkipInkPaddingInPixels = 1.0f;
    bool shouldBrightenBold = false;
    /// Whether braille draws as the renderer's 2x4 dot lattice rather
    /// than as glyphs from the face.
    ///
    /// True by default, which is the reader's default too: braille is a
    /// drawing surface at least as often as it is text, and a frontend
    /// that never wrote this field draws the lattice rather than silently
    /// taking the route the setting exists to move off.
    bool shouldDrawBrailleGrid = true;
    BrailleStyle brailleStyle = defaultBrailleStyle;
    /// How the shade glyphs draw (`[appearance] shade_blocks`).
    ShadeStyle shadeStyle = defaultShadeStyle;
    /// Whether the selection is the platform's colour under the text or the
    /// tint over it (`[selection] style`).
    SelectionStyle selectionStyle = defaultSelectionStyle;
    /// The platform's selection background for this frame, sRGB 0..1, or zero
    /// alpha where the platform reports none.
    ///
    /// A fact, resolved by the frontend for the appearance and focus the frame
    /// is drawn in; whether it is drawn is ``selectionStyle``'s decision, made
    /// in the capture.
    Vec4 platformSelectionColor {};
    /// Whether resolved text colors receive a perceptual contrast floor.
    bool shouldEnhanceTextContrast = false;
    /// Routes secondary and colored ink to the frontend's native vibrancy plane.
    bool usesNativeTextVibrancy = false;
    bool preservesBackgroundTints = false; // bool-ok: predicate describing whether explicit background ramps survive zero theme opacity
    bool usesNativeBackgroundVibrancy = false;
    Vec4 nativeSecondaryInk {};
    Vec4 nativeTertiaryInk {};
    Vec4 promptMarkInk {};
    float backgroundOpacity = 1.0f;
    /// Opacity of the default foreground, for the cells naming no index.
    std::uint8_t defaultForegroundAlpha = 255;

    // The whole-frame facts ``deriveFrameUniforms`` reads, carried with the
    // style because the frontend resolves them at the same moment: a second
    // channel for them beside the capture is a fork the two ends of a seam
    // fill from different frames.

    /// The whole-pass cell-background alpha the frame uniforms carry.
    ///
    /// Not ``backgroundOpacity`` above: that one the builder folds into each
    /// cell's alpha, for a backend with nowhere else to put it. A frontend
    /// whose fragment stage applies opacity from its uniforms feeds this and
    /// leaves the other at one, or the window darkens by the square.
    float uniformBackgroundOpacity = 1.0f;
    /// The terminal default background, sRGB 0..1. `.w` is the default-bg-only
    /// opacity factor the cell fragment applies when a cell's background
    /// matches the default, so SGR-coloured cells stay solid.
    Vec4 uniformDefaultBackground { 0.0f, 0.0f, 0.0f, 1.0f };
};

/// Lays the user's rule style over the metrics a face reported.
///
/// Applied once, on the metrics a builder is about to read. A backend that also
/// folded the multiplier into the metrics it passes would square it, and the
/// two applications are far enough apart that the result reads as a font
/// difference rather than as double counting.
///
/// Every rule weight takes the multiplier, not just the two the regular
/// underline uses: a configured weight that reached the plain rule and left the
/// bold and the strike at the font default would make one line in a paragraph
/// disagree with its neighbours.
///
/// - Parameters:
///   - metrics: The face's metrics, styled in place.
///   - style: The frame style the configuration filled.
inline void applyRuleStyle(CellMetrics& metrics, const TerminalSnapshotStyle& style)
{
    const float thicknessScale = style.underlineThicknessScale > 0.0f ? style.underlineThicknessScale : 0.0f;
    metrics.underlineThickness *= thicknessScale;
    metrics.underlineThicknessBold *= thicknessScale;
    metrics.underlineCurlyThickness *= thicknessScale;
    metrics.strikeThickness *= thicknessScale;
    metrics.strikeThicknessBold *= thicknessScale;
    // Folded into the drop rather than carried separately: the rule line is the
    // only thing either one moves.
    metrics.underlineDrop += style.underlineOffsetInPixels;
    metrics.underlineDropBold += style.underlineOffsetInPixels;
    metrics.underlineOffset = style.underlineOffsetInPixels;
    metrics.skipInk = style.shouldUnderlineSkipInk;
    metrics.shouldUnderlineFollowInk = style.shouldUnderlineFollowInk;
    metrics.skipInkPadding = style.underlineSkipInkPaddingInPixels > 0.0f ? style.underlineSkipInkPaddingInPixels : 0.0f;
}

/// Everything one frame's capture and build is steered by.
///
/// Nonescapable: it carries borrowed spans — the status row, the selection runs,
/// the damage mask and the link-kind resolver's context — every one owned by the
/// frame being built.
struct UBIQUITTY_NONESCAPABLE TerminalSnapshotOptions {
    int viewOffset = 0;
    bool drawCursor = true;
    // Whether this frame's cursor paints filled rather than as a hollow
    // outline. Keyboard focus is the usual reason and not the only one: a
    // frontend with native tabs also fills a tab behind the front one, where
    // the outline could only ever surface as a stale frame on reselection.
    // A filled block is the cursor tier, the cell re-emitted with the cursor's
    // colours at each stream's tail; the outline is drawn over unmodified
    // text. A frontend that leaves this true on a background window paints a
    // solid cursor there, which reads as the window being active.
    bool isFocused = true;
    // The cursor's cross-fade weight, `cursor.a/255 * blinkAlpha`. A caller that
    // owns a frame clock resolves the blink and passes the result; this capture
    // has none, because the same terminal must produce the same
    // frame for a golden image to mean anything. The default is the solid phase,
    // so a caller that never sets it draws an unblinking cursor rather than an
    // absent one.
    float cursorMixAlpha = 1.0f;
    // Whether the glyph under a block cursor takes a contrasting grey.
    // A user setting wherever a cursor is drawn, so it has to arrive from the
    // caller: a capture that answers for it draws grey text in the one cell the
    // reader is looking at, on every frontend whose user turned it off.
    //
    // True by default because that is what the setting defaults to wherever it
    // exists, so a caller that does not model it is unaffected.
    bool shouldCursorInvertText = true;
    // Where the grid's top-left sits within the drawable, in device pixels.
    // Zero for a surface sized to the grid; a frontend that insets the grid
    // passes its padding so glyph instances land where the cells do.
    FloatPoint originInPixels {};

    // The focused search match, in VISIBLE rows. Its highlight is bright and
    // nearly opaque, so the glyph over it takes a contrasting ink rather than
    // its own colour — the builder computes that from `searchMatchHighlight`.
    // Only the focused match: the others are a wash the glyph reads through.
    const SelectionRowSpan* searchMatchSpans = nullptr;
    int searchMatchSpanCount = 0;
    // What the focused highlight is drawn in, so the contrast is computed
    // against the colour actually behind the glyph.
    Vec4 searchMatchHighlight { 0.0f, 0.0f, 0.0f, 0.0f };
    // Every match except the focused one, in visible rows. Neutral overlay quads the glyph reads
    // through, which is what keeps the focused match the one the eye lands on.
    // A frontend drawing its own search overlays leaves these empty.
    const SelectionRowSpan* otherSearchMatchSpans = nullptr;
    int otherSearchMatchSpanCount = 0;
    // Trigger-action highlights, in visible rows. A violet tint distinct from
    // find's yellow. Empty for a frontend that draws its own.
    const SelectionRowSpan* triggerHighlightSpans = nullptr;
    int triggerHighlightSpanCount = 0;
    const SelectionRowSpan* hiliteRegionSpans = nullptr;
    int hiliteRegionSpanCount = 0;

    // The bell flash's current strength, 0 for none. The envelope is the
    // frontend's clock; the seam sees only the number — and a frontend that
    // never sets it ships a flash tier that assembles at zero alpha, invisible
    // on exactly the arm that draws through the assembly.
    float bellFlashAlpha = 0.0f;

    /// The window's background image, composited beneath the frame by
    /// ``compositeBackgroundImage``; the default draws none. A renderer with a
    /// sampler of its own reads the same placement and blend from it.
    BackgroundImageLayer backgroundImage {};

    // The cursor's opacity, 1 for solid. The reader's `[cursor] alpha`, which
    // is display policy rather than content: the model's palette carries three
    // channels and no alpha, so a translucent cursor cannot travel with the
    // colour and has to arrive beside it. A frontend that never sets this
    // draws a solid cursor, which is what an unconfigured terminal shows.
    float cursorAlpha = 1.0f;

    // Selected row spans, in VISIBLE rows, as the frontend's selection model
    // resolved them. Carried here so the CPU composite and the instance path
    // read one description of the selection instead of each frontend supplying
    // its own to whichever it happens to use — a frontend that fed spans to only
    // one of them drew a selection that vanished when the renderer changed.
    const SelectionRowSpan* selectionSpans = nullptr;
    int selectionSpanCount = 0;
    // The characters the selection covers in each rich text block, as
    // `Selection::blockSelections` resolved them from the same cut a copy
    // reads; the capture draws one highlight per line from them.
    RichText::BlockSelections richTextSelections {};
    int richTextSelectionCount = 0;

    // OSC 8 / regex link styling. `shouldUnderlineLinks` is the user setting; with it
    // off no cell is flagged and the capture allocates nothing. The capture
    // resolves which cells are links from the cells it already holds and the
    // hover band below, so the rule lives here once instead of in each frontend
    // — the flag array is what `BuildInputs::linkCells` needs, and a frontend
    // that built its own would be reimplementing `linkHoverContainsCell`.
    bool shouldUnderlineLinks = false;
    // The underline a link cell carries absent an SGR underline of its own, as a
    // Ubiquitty::VT::underline* value, and the colour its text and rule take.
    std::uint8_t linkUnderlineStyle = defaultLinkUnderlineStyle;
    std::uint8_t linkUnderlineStyleFile = defaultLinkUnderlineStyleFile;
    std::uint8_t linkUnderlineStyleMailto = defaultLinkUnderlineStyleMailto;
    // Unset (alpha zero) takes the palette's own link colour in the capture —
    // the legible blue `CellPalette::linkColor` picks against the background —
    // so a frontend with no system accent of its own needs no colour of its
    // own. A frontend that has one, or forces the system blue, names it here.
    Vec4 linkColor { 0.0f, 0.0f, 0.0f, 0.0f };
    // The status line's cells, replacing the LAST visible row. Null means the
    // frame has no status line and every row comes from the grid.
    //
    // Substituted into the captured copy rather than steered around in the cell
    // walk: the capture already owns a copy of the grid, so one row-sized write
    // reaches every consumer at once and no renderer needs a per-cell "is this the
    // status row" test. `statusRowColumns` is checked against the frame's own
    // width — a shorter array is a caller whose status line has not caught up with
    // a resize, and reading it would run off the end of the row.
    //
    // The status row carries no line attribute, no hovered link, no selection
    // recolour and no cursor: it is chrome the terminal writes, not content the
    // application addressed, so every affordance that points at a grid cell would
    // be pointing at the wrong thing.
    const Cell* statusRowCells = nullptr;
    int statusRowColumns = 0;

    // OSC 19 selection foreground: a selected cell draws its GLYPH in this colour,
    // over and above the translucent tint the selection already paints. Off unless
    // the host asked for it, because the default selection is a wash that leaves
    // the text its own colour.
    //
    // The selected cells come from `selectionSpans` above rather than a second
    // array: a frame where the tint covers one set of cells and the recolour
    // covers another has no reading a user could describe, and two arrays make
    // that state expressible.
    bool shouldOverrideSelectionForeground = false;
    Vec4 selectionForeground { 0.0f, 0.0f, 0.0f, 1.0f };

    // Whether the caller wants the subpixel mono glyph pass at all.
    //
    // The atlas margins say the pass is POSSIBLE; this says it is wanted. They are
    // different questions, and only the frontend can answer the second: turning
    // subpixel text off is a user setting that leaves the atlas exactly as it was,
    // so a capture reading the margins alone emits a phased glyph stream for a user
    // who asked for the cell-baked path.
    //
    // True by default, so a frontend whose atlas is its preference — one that
    // rasterises without margins when it does not want the pass — is unaffected.
    bool shouldUseSubpixelGlyphs = true;

    // The subpixel phase grid and the raster keying the backend's atlas uses
    // (CellInstanceBuilder.h BuildInputs for each). The atlas owns these facts
    // — the phase count is the grid CoreText quantises pens onto at the font's
    // device size, and tiers exist only where the rasteriser honours them — so
    // the frontend copies them in from the atlas it will draw with. The
    // defaults are the portable contract.
    int subpixelPhaseCount = 8;
    // The wide page's own count, 0 for the count above (BuildInputs).
    int widePhaseCount = 0;
    bool shouldFloorSubpixelPhase = false;
    bool hasRasterTiers = false;

    // The accent the HOVERED run takes, for text and rule. A separate colour from
    // the resting one: the resting link is the theme's legible link against this
    // background, and this is the interaction highlight, which stays the accent
    // whatever the theme.
    Vec4 linkHoverColor { 0.0f, 0.0f, 0.0f, 1.0f };
    // Resolves an OSC 8 link id to a `Ubiquitty::Attributes::LinkRun::Kind`, for
    // per-kind link styling. Null leaves every resting link on the base style,
    // which is what a frontend without the setting wants and costs it nothing.
    //
    // Supplied by the frontend rather than read here, for the reason
    // `clusterResolver` is: the id indexes a ring the TERMINAL owns, and a capture
    // running off a copied FrameSource must not reach back into the live model to
    // read it.
    std::uint8_t (*linkKindResolver)(std::uint16_t linkID, void* context) = nullptr;
    void* linkKindResolverContext = nullptr;
    /// Set by a caller of the Terminal-taking capture that has no resolver of
    /// its own: the capture installs ``Attributes::LinkRun::kindForLinkID`` over
    /// the terminal it is already reading. A resolver the caller supplies wins —
    /// the flag fills the field only when it is empty, and the source-only
    /// capture ignores it, having no terminal to read.
    bool shouldResolveLinkKindsFromSource = false;

    // The Cmd-hovered link run, as the frontend's pointer tracking resolved it:
    // a span from (rowFirst, columnFirst) to (rowLast, columnLast) in VISIBLE
    // rows, inclusive, which `hoverColumnBand` narrows to one row's columns. A
    // regex or path link has no OSC 8 id, which is why the span is carried
    // alongside the id rather than derived from it.
    bool isHoverLinkActive = false;
    std::uint16_t hoverLinkID = 0;
    int hoverLinkRowFirst = 0;
    int hoverLinkRowLast = -1;
    int hoverLinkColumnFirst = 0;
    int hoverLinkColumnLast = -1;
    // The pane span the hovered run was resolved inside; the middle rows of a
    // wrapped run clip to it. spanLast < 0 means the screen's own width.
    int hoverLinkSpanFirst = 0;
    int hoverLinkSpanLast = -1;
    // The digest of the run's text at the resolve, from `linkRunTextDigest`;
    // the capture draws the run only over cells that still digest the same.
    // Zero is a run resolved without one, drawn wherever its rows say.
    std::uint64_t hoverLinkTextDigest = 0;
    // The whole surface, in device pixels. Zero means it is exactly the grid,
    // and the frame-level passes that fill the leftover then fill nothing.
    FloatSize viewportSizeInPixels {};
    // The cell height and line spacing in POINTS, the domain the cursor bar's
    // thickness and inset are defined in. Zero means the caller keeps no point
    // space and the cursor geometry works at one pixel per point — the same
    // answer as passing the device-pixel cell height, so a caller that never
    // sets this is unaffected by its existence. A caller that has point space
    // passes it, or the bar's thickness tracks the backing scale instead of the
    // type size.
    double cellHeightPoints = 0.0;
    double lineSpacingPoints = 0.0;
    // The face the OSC 133 prompt-mark bracket is placed against. The bracket
    // is a fraction of the point size and is centred on the glyph ink box, so
    // these are the FONT's own metrics rather than the cell's — substituting
    // baselineFromTop and its complement is right only while line spacing is
    // zero. A zero fontSize leaves the mark pass off, which is what a caller
    // that tracks no marks needs.
    double promptMarkFontSize = 0.0;
    float promptMarkFontAscent = 0.0f;
    float promptMarkFontDescent = 0.0f;
    float promptMarkLineGapTopInPixels = 0.0f;
    // The face's `[` ink above and below the baseline, atlas pixels; both
    // zero when unmeasured (``PromptMarkParameters``).
    float promptMarkBracketInkTopInPixels = 0.0f;
    float promptMarkBracketInkBottomInPixels = 0.0f;
    /// The face's x-height, atlas pixels; zero when unmeasured. The search
    /// pills centre on the band it names when the face has no cap height.
    float promptMarkFontXHeight = 0.0f;
    /// The face's cap height, atlas pixels; zero when unmeasured. The search
    /// pills centre on the band it names rather than on the cell.
    float promptMarkFontCapHeight = 0.0f;
    /// The atlas oversample the mark geometry scales by. Zero means one.
    ///
    /// Carried rather than derived from the cell metrics: those measure the atlas
    /// against the CELL, and DECCOLM-132 resizes the cell without resizing the
    /// atlas.
    float promptMarkAtlasScale = 0.0f;
    /// The Tektronix tube's colours: the theme pair, or under `ColorMode::Custom`
    /// the beam and background the configuration names. Read only while the
    /// terminal is in Tek mode, when the frame is the tube: its background
    /// covers the grid and the beam draws the stored vectors over it.
    Tek::ColorPreference tekColors {};
    /// Whether the Tektronix beam draws its bloom halo, `[tek] glow_vectors`.
    bool shouldDrawTekGlow = false;
    /// The toolkit caret a bar cursor draws as, which the frontend reads from
    /// its toolkit's settings; absent, the bar is Core's capsule.
    ToolkitCaret toolkitCaret;
    /// Device pixels per point for the Tektronix frame, which sizes the beam,
    /// its halo and the projection's edge inset through `Tek::beamGeometry`.
    float tekDeviceScale = 1.0f;
    /// Whether Core stands the password-lock badge in the frame.
    ///
    /// A frontend with a view layer draws its own above the frame and leaves
    /// this false; one whose only surface is the frame sets it and gets the
    /// badge wherever the terminal reports a password prompt. The terminal's
    /// state is not consulted unless this is set, so no frontend is given a
    /// second lock by a Core change it did not ask for.
    bool shouldDrawPasswordLock = false;
    // Per-row damage, length `rows`, non-zero where the row changed since this
    // caller last emitted it. Forwarded to BuildInputs::dirtyRows, whose contract
    // governs: a clean row emits nothing, so only a caller still holding that
    // row's previous output may set it. A backend compositing a fresh frame from
    // the returned spans alone would drop every row it skipped.
    //
    // Null builds every row. That is the only safe default and also the shape a
    // MEASUREMENT wants to be careful about: a profiler that suppresses its
    // caller's retained target must leave this null too, or it compares a
    // builder that skips against an inline walk that cannot.
    const std::uint8_t* dirtyRows = nullptr;

    // SGR 5/6 blink, resolved against a tick the CALLER supplies for the same
    // reason `cursorMixAlpha` is: this capture owns no clock, and a capture that
    // read one would not produce the same frame twice from the same terminal.
    //
    // False leaves blinking text at full ink rather than absent, which is what a
    // caller with no frame clock needs and what a golden image wants. A caller
    // that has a clock must set both, and must hand the same tick to every path
    // that draws the frame: the cursor caret and the cells share one curve, so
    // two readings a frame apart put the caret and the text it sits in at
    // different phases of it.
    bool shouldResolveBlink = false;
    std::uint32_t blinkTicks = 0;

    // Motion smoothing, resolved by the caller that owns the frame clock, for
    // the same reason `cursorMixAlpha` is: this capture must be deterministic.
    bool isCursorSliding = false;
    float cursorColumnFractional = 0.0f;
    float cursorVRowFractional = 0.0f;
    TerminalSnapshotStyle style;
};

/// The atlas facts a frame's glyph phases and raster keying follow from.
///
/// The rasteriser is the authority on all three: the phase count is the grid it
/// quantises pens onto at this font's device size, the flooring mirrors whether
/// it quantises at all, and tiers exist only where it honours them. A frontend
/// reads them off the atlas it will draw with and hands them over; nothing here
/// asks a font question.
struct FrameRasterFacts {
    /// How many horizontal pen buckets one device pixel divides into.
    int subpixelPhaseCount = 8;
    /// How many it divides into for a two-cell glyph, where the rasteriser draws those at a device size of
    /// their own; zero for the same count.
    int widePhaseCount = 0;
    /// Whether the sheet was rasterised by CoreText.
    bool isCoreTextRaster = false;
    /// The bitmap tier's integer upscale; zero on a real face.
    int unifontScale = 0;
};

/// Writes the raster facts into the frame's options.
///
/// Floored phases and tiered rasters are what a CoreText sheet amounts to, so
/// both follow from the one fact rather than being asked for separately: CoreText
/// floors a fractional pen onto its own grid, and rounding to nearest over such
/// a rasteriser draws some columns one bucket right of where a native draw at
/// the same pen puts them (see ``PhaseRounding``). A bitmap family's sheet
/// keys on the portable grid, like the portable renderer it agrees with.
///
/// Every route that assembles a frame calls this, and that is the point: two
/// routes drawing one terminal must land each glyph on the same phase, or the
/// text shifts by a bucket on whichever frames the other route produced. The
/// boundary capture and the live capture alternate during typing, which is
/// where a second spelling of this rule is seen rather than reasoned about.
///
/// - Parameters:
///   - options: The frame options to write into. `style` must already be
///     assembled: the bitmap tier's scale is a style field.
///   - facts: The atlas's facts for this frame.
inline void applyFrameRaster(TerminalSnapshotOptions& options, const FrameRasterFacts& facts)
{
    options.subpixelPhaseCount = facts.subpixelPhaseCount;
    options.widePhaseCount = facts.widePhaseCount;
    options.shouldFloorSubpixelPhase = facts.isCoreTextRaster;
    options.hasRasterTiers = facts.isCoreTextRaster;
    options.style.unifontScale = facts.unifontScale;
}

/// Gives the frame the point space its caller keeps.
///
/// The cursor's thin shapes are defined in points: a bar two points wide, an
/// underline two points thick, a hollow stroke one point, each sitting inside
/// the padding the line spacing opens. Without a point space the geometry
/// works at one pixel per point, so at a backing scale of two the same cursor
/// is drawn half as thick and seated a padding lower than an arm that passes
/// one. Every arm that assembles a frame calls this with its own scale, so the
/// cursor is one shape across the arms rather than a shape per renderer.
///
/// - Parameters:
///   - options: The frame options to write into.
///   - cellHeightInPixels: The cell the frame lays out in, device pixels.
///   - backingScale: Device pixels per point. At or below zero, and a cell at
///     or below zero, leave the point space unset — the caller has none.
///   - lineSpacingPoints: The configured line spacing, points.
inline void applyPointSpace(TerminalSnapshotOptions& options, float cellHeightInPixels,
    double backingScale, double lineSpacingPoints)
{
    if (backingScale <= 0.0 || cellHeightInPixels <= 0.0f)
        return;
    options.cellHeightPoints = static_cast<double>(cellHeightInPixels) / backingScale;
    options.lineSpacingPoints = lineSpacingPoints;
}

/// The run under the pointer, in buffer rows rather than visible ones.
///
/// Absolute because that is what survives: a run recorded as a visible row is a
/// row number that means something different the moment output scrolls, and the
/// pointer has not moved. The frame converts when it draws.
struct FrameHoverLink {
    /// Whether a link is under the pointer at all.
    bool isActive = false;
    /// The declaring hyperlink's identifier, zero for a run the terminal read.
    std::uint16_t linkID = 0;
    /// First and last buffer rows of the run.
    int rowAbsoluteFirst = 0;
    int rowAbsoluteLast = -1;
    /// First and last columns of the run.
    int columnFirst = 0;
    int columnLast = -1;
    /// The pane span the run was resolved inside; middle rows clip to it.
    /// A last below zero means the screen's own width.
    int spanColumnFirst = 0;
    int spanColumnLast = -1;
    /// The digest of the run's text at the resolve; zero when none was taken.
    std::uint64_t textDigest = 0;
};

/// Writes the hovered run into the frame's options, converted to visible rows.
///
/// The conversion is one subtraction, and it is here for the reason
/// ``applyFrameRaster`` is: two routes assemble a frame for one terminal, and a
/// rule spelled once in each is a rule they can disagree about. The live route
/// draws the emphasis and the boundary route, which had no hover to draw at
/// all, drew the link resting — so a program redrawing under synchronized
/// output flickered the emphasis on and off under a pointer that never moved.
///
/// Converting at the moment the frame is drawn rather than when the pointer
/// resolved the run is what makes a hover survive the output arriving under it.
///
/// - Parameters:
///   - options: The frame options to write into.
///   - hover: The run under the pointer, in buffer rows.
///   - scrollbackCount: Rows of scrollback above the viewport this frame.
///   - viewOffset: How far the viewport is scrolled back this frame.
inline void applyFrameHoverLink(TerminalSnapshotOptions& options, const FrameHoverLink& hover,
    int scrollbackCount, int viewOffset)
{
    options.isHoverLinkActive = hover.isActive;
    options.hoverLinkID = hover.linkID;
    options.hoverLinkRowFirst = hover.rowAbsoluteFirst - scrollbackCount + viewOffset;
    options.hoverLinkRowLast = hover.rowAbsoluteLast - scrollbackCount + viewOffset;
    options.hoverLinkColumnFirst = hover.columnFirst;
    options.hoverLinkColumnLast = hover.columnLast;
    options.hoverLinkSpanFirst = hover.spanColumnFirst;
    options.hoverLinkSpanLast = hover.spanColumnLast;
    options.hoverLinkTextDigest = hover.textDigest;
}

/// Assembles the per-frame uniforms from the frame's resolved facts, before a
/// capture exists.
///
/// The mirror of ``deriveFrameUniforms(const BuildInputs&)`` for every field
/// the options carry: the capture copies each of those from the options and the
/// metrics into ``BuildInputs`` unchanged, so deriving from either side of the
/// copy is the same numbers. This overload exists for the caller whose frame
/// constants are consumed before its capture runs — an encode that digests the
/// uniforms as its cache key cannot wait for a capture that consumes the very
/// plan the digest decides.
///
/// `edgeToEdge` is the exception and stays zero here, because it is a fact
/// about the terminal rather than about the frame's style: DECSET 7402 and the
/// letterbox selection decide it, and the capture resolves them while its
/// caller holds the model lock. A frame path that filled it from the style
/// would be reading the mode at whatever moment it assembled the style, which
/// is after the capture on every arm that has one. The origin follows the
/// same fact: under the colour letterbox the frame has no padding, so the
/// capture zeroes whatever origin the options carried, and this side keeps
/// the padded one — a caller reading these constants under the mode reads
/// them off the capture, or resolves its geometry from the mode first, before
/// it assembles its options.
///
/// A caller must pass the metrics it will hand the capture. `applyRuleStyle`
/// never moves the cell size, so styling applied inside the capture cannot make
/// the two sides disagree here.
///
/// - Parameters:
///   - options: The frame's resolved options, style and placement filled.
///   - metrics: The cell metrics the capture will be handed.
/// - Returns: The constants every pass reads.
[[nodiscard]] inline FrameUniforms deriveFrameUniforms(const TerminalSnapshotOptions& options, const CellMetrics& metrics)
{
    FrameUniforms uniforms {};
    uniforms.viewportSizeInPixels = { options.viewportSizeInPixels.width, options.viewportSizeInPixels.height };
    uniforms.cellSizeInPixels = { metrics.cellWidth, metrics.cellHeight };
    uniforms.originInPixels = { options.originInPixels.x, options.originInPixels.y };
    uniforms.backgroundOpacity = options.style.uniformBackgroundOpacity;
    uniforms.defaultBackground = options.style.uniformDefaultBackground;
    // The same verdict the BuildInputs overload folds: the style's unifont
    // scale is positive only for a face Core rasterises itself.
    uniforms.bitmapTier = options.style.unifontScale > 0 ? 1 : 0;
    return uniforms;
}

/// Draws a Tektronix frame over a finished raster, when the terminal is in
/// Tek mode.
///
/// A storage tube's frame is the tube: its background covers the whole surface
/// and the beam draws over it, in the colours Core resolves from the theme, the
/// configuration and OSC 10/11/15/16 together — the resolvers the Metal arm
/// calls, so a custom phosphor is one picture on every arm. The strokes are
/// Core's one walk, the runs the Metal pass draws, rastered by `Tek::BeamRun`.
/// A software frontend calls this after its grid raster, as the snapshot entry
/// below does, rather than drawing a tube of its own.
///
/// - Parameters:
///   - terminal: The terminal; nothing is drawn outside Tek mode.
///   - options: The frame options, for `tekColors`, `shouldDrawTekGlow` and
///     `tekDeviceScale`.
///   - rgba: The RGBA8 raster, row stride 4 * `width`, drawn over in place.
///   - width: The raster's width in pixels.
///   - height: The raster's height in pixels.
void compositeTektronixFrame(const Terminal& terminal, const TerminalSnapshotOptions& options, std::vector<std::uint8_t>& rgba, int width, int height);

// Renders the terminal to a tightly packed RGBA8 image, row stride 4*width.
// Empty on invalid input or a raster failure, both dimensions left 0.
//
// This is the whole composition — grid raster, padding fill, prompt marks and
// the Tektronix overlay — and the PNG entry point below is an encode layered on
// it. A consumer that wants pixels rather than a file (a canvas blit, a texture
// upload) takes this one; encoding a PNG only to decode it again is the same
// work twice.
//
// - Parameters:
//   - widthOut: Receives the image width in pixels.
//   - heightOut: Receives the image height in pixels.
/// `chrome`, when non-null, is composited over the grid exactly as it is on the
/// instance path — so the CPU raster and the GPU build show the same picture. A
/// frontend that passed it to only one of them would draw a palette that
/// appeared and vanished with the renderer, which reads as a rendering fault
/// rather than as a missing argument.
/// `rasterizer`, when non-null, draws the glyphs instead of the embedded
/// Unifont — so a frontend can render this raster with the same face its GPU
/// path uses.
///
/// Null keeps Unifont, which is what a portable snapshot wants: a PNG that looks
/// the same on a machine with none of the author's fonts installed. The two
/// answers are both correct and they are correct for different callers, so this
/// is a parameter rather than a change of default.
///
/// A frontend comparing this raster against its own GPU output must pass the
/// rasterizer that GPU path uses. Otherwise the comparison is between two
/// typefaces, every pixel of the difference is real, and none of it is about the
/// renderer under test.
///
/// A caller rendering repeatedly — a frontend, rather than a one-shot
/// screenshot — should pass its own renderer. The one built here is built per
/// call: over a megabyte of atlas allocated and zeroed, and a glyph cache that
/// starts empty, so every glyph on screen re-rasterises. That cache is designed
/// to live across frames, and a renderer that does not is a cache that never
/// hits. The renderer must be built over the same rasterizer passed here, since
/// its atlas holds that face's glyphs.
///
/// `richText`, when non-null, draws the frame's rich text blocks in the faces
/// Core bundles, over the grid and under the cursor and the overlays; the
/// caller published those faces' table (`BundledRichText::typographyFacts`)
/// to the terminal, since a block drawn in faces other than the ones it was
/// laid out from lands where Core did not put it. Null leaves each block's
/// covered cells in its pen's background, for a frontend that draws blocks
/// in a layer of its own.
[[nodiscard]] std::vector<std::uint8_t> renderTerminalSnapshotRGBA(const Terminal&, const TerminalSnapshotOptions&, int& widthOut, int& heightOut,
    ChromeLayers chrome = {}, GlyphRasterizer* rasterizer = nullptr, SoftwareRenderer* renderer = nullptr, BundledRichText* richText = nullptr);

// Returns a self-contained RGBA PNG. An empty vector indicates invalid input
// or a raster/encode failure.
/// Composites the OSC 133 prompt-mark brackets into a padded image's gutters.
///
/// A separate pass rather than part of the overlay stream, because of where the
/// brackets go: they sit in the padding, and every CPU arm renders the grid
/// first and puts the padding around it afterwards. An overlay quad in the
/// gutter is therefore either outside the surface the renderer drew or under the
/// background the padding paints, which is why an arm can gather its row marks
/// correctly and still show none.
///
/// Call it on the padded image, after the padding exists.
///
/// - Parameters:
///   - terminal: The terminal whose row marks are read.
///   - firstAbsoluteRow: The absolute row the viewport's top row is, so a
///     scrolled-back view reads the marks the user is looking at.
///   - metrics: The cell geometry the grid was drawn with.
///   - face: The face's own metrics, whose `[` the brackets centre on. A zero
///     font size draws nothing, which is what an arm with no face reports.
///   - gridWidth: The grid's width in pixels, which with `left` and the image
///     width gives both gutters.
///   - left: The left padding in pixels.
///   - top: The top padding in pixels.
///   - rgba: The padded image, written in place.
///   - width: The padded image's width.
///   - height: The padded image's height.
void compositePromptMarksIntoPadding(const Terminal& terminal, int firstAbsoluteRow, const CellMetrics& metrics, const FaceMetrics& face,
    int gridWidth, int left, int top, std::vector<std::uint8_t>& rgba, int width, int height);

[[nodiscard]] std::vector<std::uint8_t> renderTerminalSnapshotPNG(const Terminal&, const TerminalSnapshotOptions&, BundledRichText* richText = nullptr);

} // namespace Rendering
} // namespace Ubiquitty
