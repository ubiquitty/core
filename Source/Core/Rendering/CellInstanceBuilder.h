// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"

#include "Base/Geometry.h"

#include "Core/CellGrid.h"
#include "Core/Rendering/BackgroundImage.h"
#include "Core/Rendering/BoxStrokeProfile.h"
#include "Core/Rendering/BrailleLattice.h"
#include "Core/Rendering/CellInstance.h"
#include "Core/Rendering/CellMetrics.h"
#include "Core/Rendering/GlyphBackend.h"
#include "Core/Rendering/GlyphKey.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/Rendering/LetterLayout.h"
#include "Core/Rendering/OverlayAssembly.h"
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Rendering/ShadeStyle.h"
#include "Core/Rendering/SubpixelPhase.h"
#include "Core/TextSizeTable.h"
#include "Core/VT/Constants.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace Ubiquitty::Image {
struct TerminalImage; // Image/TerminalImage.h — decoded inline-image pixels
struct TerminalImagePlacement; // Image/TerminalImage.h — where an image sits on the grid
}

namespace Ubiquitty {

struct Cell; // Core/Cell.h; Pen is reached via Cell::pen
struct TermPalette; // Core/TermPalette.h

namespace VT {
struct SoftFont; // Core/VT/SoftFont.h — the DECDLD downloaded character set
}

namespace Rendering {

struct CellInstanceScratch; // Core/Rendering/CellInstanceScratch.h — the builder's caller-owned row scratch

// Decoration kinds — kept in lockstep with the macOS Metal cell shader's
// decoration* selectors. Solid fills the rect (single/double rule, overline,
// strike, box); the rest are patterned.
enum class DecorationKind {
    Solid = 0,
    Dotted = 1,
    Dashed = 2,
    Curly = 3,
};

/// Whether an encoded decoration selector names this kind.
///
/// `DecorationInstance::kind` is a float because the shader reads it as one and
/// the struct's layout is the GPU's. Comparing it back with `==` against a
/// float-converted enum is an equality test on floats: exact for these four
/// values, and silently not exact for any kind added past the point a float stops
/// representing integers. Nothing in either type says so, which is the reason to
/// ask the question in one place rather than at each branch.
///
/// - Parameters:
///   - encoded: The selector as the instance carries it.
///   - kind: The kind to test for.
/// - Returns: True when they name the same decoration.
[[nodiscard]] inline bool isDecorationKind(float encoded, DecorationKind kind)
{
    return static_cast<int>(encoded) == std::to_underlying(kind);
}

// Which rule each link kind draws, absent a frontend saying otherwise.
//
// Named once and referenced by both the builder's inputs and the capture's
// options. A literal in each would be two defaults that agree until one is
// edited, and the capture overwrites the builder's — so the builder's copy would
// be the one that looks authoritative and never applies.
inline constexpr std::uint8_t defaultLinkUnderlineStyle = Ubiquitty::VT::underlineSingle;
inline constexpr std::uint8_t defaultLinkUnderlineStyleFile = Ubiquitty::VT::underlineDotted;
inline constexpr std::uint8_t defaultLinkUnderlineStyleMailto = Ubiquitty::VT::underlineDashed;

/// How a cell participates in an OSC 8 or regex link.
///
/// One per cell of `BuildInputs::linkCells`. A hovered cell is not a second kind
/// of link but a different presentation of the same one, which is why this is a
/// state and not a pair of flags: the hovered run reads solid in the accent colour
/// whatever the resting style is.
///
/// Numbered explicitly and never reordered. These are the bytes of an array the
/// capture fills and the builder reads, so the values are the wire format between
/// them rather than an internal ordering, and `None` is zero so a zeroed buffer
/// means no link — which is what every allocation starts as.
enum class LinkState : std::uint8_t {
    None = 0,
    Resting = 1,
    Hovered = 2,
    // Resting links whose URI scheme earns a style of its own, when the frontend
    // asked for per-kind styling. A frontend that did not ask sends `Resting` for
    // every scheme and these never appear.
    //
    // The KIND is resolved once, into this array, rather than per cell at draw
    // time: an id resolves against a ring by linear scan, and doing that inside a
    // renderer's cell loop puts an O(links) walk under an O(cells) one.
    RestingFile = 3,
    RestingMailto = 4,
    // A link in the terminal's own `ubiquitty:` scheme — a control the app
    // drew, such as a Welcome button. Resting, it is the text it is: the pen's
    // own colours and no link underline, so a label set on a plate keeps its
    // contrast. Hovered, the plate lightens rather than the text underlining.
    // Resolved whether or not per-kind styling is on, since it is not a style
    // but what the cell is.
    RestingAction = 5,
    HoveredAction = 6,
};

// CellMetrics (the per-cell pixel metrics driving decorations) lives in its own
// portable header, Rendering/CellMetrics.h.

/// Everything one frame's build reads, borrowed for the duration of the call.
///
/// Every pointer here points at storage the caller owns and the builder only
/// reads. Nothing may outlive the call that receives it: the rows are display
/// rows the terminal reuses, and a copy kept past the frame reads cells that
/// have since scrolled away.
struct UBIQUITTY_NONESCAPABLE BuildInputs {
    const Cell* cells = nullptr; // rows*columns, row-major, display order
    using ClusterResolver = int (*)(const Cell*, std::span<std::uint32_t>, void*);
    ClusterResolver clusterResolver = nullptr; // writes a cell's cluster code points; null draws each cell's base code point alone
    void* clusterResolverContext = nullptr; // handed to the resolver as its last argument, by the frontend that owns the cells
    int rows = 0;
    int columns = 0;
    const TermPalette* palette = nullptr;
    /// The terminal's OSC 66 spec store, for resolving a cell's
    /// ``textSizeSpecID`` back to the scale and width it was written under.
    ///
    /// Null where a caller keeps none, and a null table renders every sized
    /// block unsized rather than refusing the frame — the protocol's own
    /// degradation for a terminal without the feature, and the same answer
    /// ``claimedRunColumns`` gives when it cannot reach a table. That makes an
    /// unwired caller silently correct-looking, so a backend adding sized-text
    /// support has to be checked against a rendered block rather than against
    /// a clean build.
    const TextSizeTable* textSizeTable = nullptr;
    /// The terminal's ambiguous-width choice, which measures a sized anchor
    /// whose spec leaves the width to its glyph — the same measure the parser
    /// placed the block with, or the claim and the cells disagree.
    Unicode::Width::AmbiguousWidth ambiguousWidth = Unicode::Width::AmbiguousWidth::Narrow;
    bool screenReverse = false; // DECSCNM; folded into resolveCell's reverseAlso
    bool brightenBold = false;
    CursorRenderState cursor;
    /// A DECSSDT status line occupies the bottom grid row, so the data cursor
    /// must not paint there. Consulted by the overlay assembly's membership
    /// rule rather than by a frontend gate, so two arms cannot disagree.
    bool hasStatusLineOnBottomRow = false;
    /// The bell flash's current strength, 0 for none; the envelope is the
    /// frontend's clock and this seam sees only the number.
    float bellFlashAlpha = 0.0f;
    /// The window's background image, which ``SoftwareRenderer`` composites
    /// beneath the frame after the grid's layers and before the overlays;
    /// the default draws none.
    BackgroundImageLayer backgroundImage {};
    /// The cell box every origin and advance is measured in.
    ///
    /// One field, because `buildCellInstances` reads only this one: a second
    /// spelling that some callers fill and the builder ignores resolves to a
    /// zero cell, which collapses every glyph onto the grid corner while the
    /// cell COUNT stays right — a wrong picture rather than a crash.
    ///
    /// A caller with no metrics of its own leaves this zero and lets
    /// ``SoftwareRenderer`` fill it from the rasterizer it was built with.
    CellMetrics metrics;

    /// The cell height and line spacing in POINTS, the domain every geometry a
    /// fraction of the type size is defined in — the cursor bar's thickness and
    /// inset, the prompt-mark bracket's spine.
    ///
    /// Not derivable from `metrics`. That carries the LIVE cell; this is the
    /// REFERENCE the font was measured at, and `metrics.cellHeight /
    /// metrics.deviceScale` equals this only while the cell keeps its natural
    /// height. A line-height setting or a snapped row moves one and not the
    /// other, so the derivation is right in the common case and wrong exactly
    /// where the distinction exists.
    ///
    /// Frame-scoped rather than per-pass: two passes read it and a second
    /// spelling beside the first is how a caller comes to set one and not the
    /// other. Zero means the caller keeps no point space, and the geometry then
    /// works at one pixel per point — the same answer as passing the
    /// device-pixel cell height, so a backend that has never heard of these is
    /// unaffected by their existence.
    double cellHeightPoints = 0.0;
    double lineSpacingPoints = 0.0;
    /// The toolkit caret a bar cursor draws as (``ToolkitCaret``); absent, the
    /// bar is Core's capsule.
    ToolkitCaret toolkitCaret;

    /// Where the grid's top-left sits within the drawable, in device pixels.
    ///
    /// Glyph instances carry absolute positions, so a grid inset from the
    /// drawable edge needs this; cell instances carry grid coordinates and the
    /// shader applies the same origin, which is why they are unaffected by it.
    ///
    /// It participates in subpixel phase rather than being added afterwards: a
    /// fractional origin shifts which phase a pen lands on, and snapping first
    /// would pick the phase for a pen the glyph is never drawn at.
    ///
    /// Zero for a backend whose surface is sized to the grid, which is the
    /// default and leaves such a backend unaffected.
    Vec2 originInPixels { 0.0f, 0.0f };

    /// The whole surface being drawn into, in device pixels.
    ///
    /// Larger than the grid wherever the window does not divide evenly into
    /// cells. Zero means the surface is the grid, and the frame-level passes
    /// that fill the leftover area then have nothing to fill.
    Vec2 viewportSizeInPixels { 0.0f, 0.0f };

    /// DECCOLM-132 is active, so the grid is a fixed 132 columns inside whatever
    /// width the window happens to have rather than sized to fit it.
    ///
    /// The area past the grid is not terminal, and at 132 columns it can be wide
    /// enough to read as one — hence the rule that marks where the terminal
    /// ends. In every other mode the leftover is ordinary padding.
    bool isColumns132 = false;
    bool isScreenBlanked = false;

    // Inline-image (kitty / sixel / iTerm) placements to composite over the
    // grid. The arrays are the Terminal's image / placement tables; the renderer
    // resolves each placement to a device-pixel quad and alpha-blends the source
    // pixels. viewOffset scrolls placements with the grid (0 = live tail).
    // Empty ⇒ no images.
    const Ubiquitty::Image::TerminalImage* images = nullptr;
    int imageCount = 0;
    const Ubiquitty::Image::TerminalImagePlacement* placements = nullptr;
    /// The screen active at capture time. The renderers that resolve from this
    /// capture have no Terminal to ask, and a placement belongs to one screen.
    bool alternateActive = false;
    int placementCount = 0;
    int viewOffset = 0;

    // Translucent selection highlight: one filled overlay quad per selected
    // on-screen row span, in selectionColor (RGBA 0..1; alpha tints the cells so
    // the glyph stays readable). Empty ⇒ no selection.
    /// OSC 133 prompt-mark brackets, drawn in the padding gutters.
    ///
    /// Reaches the same overlay stream selection and the cursor use. Left
    /// default, the pass does not run, which is what a backend with no mark
    /// tracking needs; ``resolvePromptMarkGeometry`` independently reports zero
    /// per row when neither gutter has room, so a backend that fills this and
    /// draws edge to edge still emits nothing.
    PromptMarkInputs promptMarks;

    /// Whether this caller wants Core to stand the password badge in the frame.
    ///
    /// The intent, not the terminal's state: a frontend with a view layer draws
    /// its own badge above the frame and leaves this false, so that Core's one
    /// implementation serves the frontends whose only surface is the frame
    /// without giving the others a second lock.
    bool shouldDrawPasswordLock = false;
    /// Whether the terminal is taking a password.
    ///
    /// Carried beside the intent rather than folded into it, because folding
    /// the two needs a short-circuit in the capture, which keeps the block
    /// structure its PGO profile describes. Membership is the assembly's
    /// decision anyway, which is where the two meet.
    bool isPasswordPromptActive = false;

    const SelectionRowSpan* selectionSpans = nullptr;
    int selectionSpanCount = 0;
    Vec4 selectionColor { 0.0f, 0.0f, 0.0f, 0.0f };
    /// Whether `selectionColor` is the selected cells' background, under their
    /// glyphs, rather than a translucent quad over them.
    ///
    /// A selection behind the text is baked into the cells and emits no
    /// overlay; the capture marks the rows it reaches and leaves, since no cell
    /// byte changes when it moves.
    bool isSelectionBehindText = false;

    // OSC 19 selection-foreground (xterm): when set, every cell inside a
    // selectionSpan draws its glyph in selectionForeground (RGB; resolved
    // blink/palette alpha kept) instead of its SGR/link color; the translucent
    // selectionColor overlay still composites on top. false ⇒ normal foreground.
    bool selectionForegroundOverride = false;
    Vec4 selectionForeground { 0.0f, 0.0f, 0.0f, 1.0f };

    // The focused search match, as visible-row spans. Its highlight is a bright,
    // nearly opaque quad drawn beneath the glyph, so a light foreground over it is
    // unreadable — which is what a find on a dark theme looked like. A cell
    // inside one of these takes a colour contrasting with the highlight, the
    // same rule the block cursor uses for the glyph it sits on.
    //
    // Only the focused match: the other matches are a neutral wash the glyph
    // still reads through, and recolouring those would make every hit on screen
    // shout as loudly as the one the user is standing on.
    const SelectionRowSpan* searchMatchSpans = nullptr;
    int searchMatchSpanCount = 0;
    // What the highlight is drawn in, so the contrast is computed against the
    // colour actually behind the glyph rather than against an assumption.
    Vec4 searchMatchHighlight { 0.0f, 0.0f, 0.0f, 0.0f };
    /// Seconds since the focused match took focus, the pill's bounce clock, or
    /// negative for a pill at rest. A fact the frontend reports; the curve is
    /// Core's (`focusedPillScale`).
    float focusedMatchAgeSeconds = -1.0f;

    // Every match except the focused one, as visible-row spans. These become neutral
    // overlay quads rather than a bake: the glyph keeps its own colour and reads
    // through, which is what makes the focused match the one the eye lands on.
    //
    // Separate from the focused list rather than one list plus an index, because
    // the two are drawn by different passes — the focused match never reaches
    // the overlay span at all, and a single list would invite a caller to send
    // the focused span here and get it drawn twice.
    //
    // A frontend that builds its own overlays leaves these empty and calls
    // `searchMatchQuad` itself; one that takes the builder's overlay span gets
    // them here. Size that span with `maxOverlaysForRows`, passing this count.
    const SelectionRowSpan* otherSearchMatchSpans = nullptr;
    int otherSearchMatchSpanCount = 0;

    // Trigger-action highlights, as visible-row spans. A translucent violet tint
    // distinct from find's yellow, so a rule that fired and a search hit are not
    // read as the same thing. The spans come from `Triggers::highlightAt`, which
    // reports an inclusive column pair on a fixed oldest-evicting ring.
    //
    // Empty for a frontend that builds its own overlays; it calls
    // `triggerHighlightQuad` itself. Both routes end at one geometry.
    const SelectionRowSpan* triggerHighlightSpans = nullptr;
    int triggerHighlightSpanCount = 0;

    // The DEC mode 1001 hilite band, as visible-row spans. An amber tint, warmer
    // than the slate selection, so a host-driven region reads as distinct from
    // one the user dragged. The spans come from `hiliteRegionRowSpans`, which
    // owns the band's shape.
    //
    // Empty for a frontend that builds its own overlays; it calls
    // `hiliteRegionQuadAtVisibleRow` itself. Both routes end at one geometry.
    const SelectionRowSpan* hiliteRegionSpans = nullptr;
    int hiliteRegionSpanCount = 0;

    // Window background opacity (0..1): each cell's background alpha is scaled by
    // this so the desktop shows through behind the opaque text. 1.0 ⇒ fully opaque.
    /// Whether resolved text colors receive a perceptual contrast floor.
    bool shouldEnhanceTextContrast = false;
    /// Routes neutral secondary ink to the frontend's native vibrancy plane.
    bool usesNativeTextVibrancy = false;
    bool preservesBackgroundTints = false; // bool-ok: predicate describing whether explicit background ramps survive zero theme opacity
    bool usesNativeBackgroundVibrancy = false;
    Vec4 nativeSecondaryInk {};
    Vec4 nativeTertiaryInk {};
    float backgroundOpacity = 1.0f;

    // Per-index foreground palette OPACITY (0..255): a cell whose foreground is
    // an indexed/default palette color picks up that color's protocol alpha in
    // inst.foreground.w. The capture points this at the captured palette's own
    // `indexedAlpha`, so it is the live palette's answer for this frame. Null ⇒
    // every indexed foreground stays opaque (factor 1.0); RGB-direct
    // foregrounds have no protocol alpha and always stay 1.0. When set, points
    // at 256 bytes (one per index 0..255); defaultForegroundAlpha is the
    // colorDefault foreground's opacity.
    const std::uint8_t* paletteForegroundAlpha = nullptr;
    std::uint8_t defaultForegroundAlpha = 255;

    // The whole-frame facts ``deriveFrameUniforms`` assembles, copied from the
    // capture's style block so a renderer whose encode runs far from its
    // capture derives its uniforms from the frame they describe rather than
    // from an argument resolved beside it.

    /// The whole-pass cell-background alpha the frame uniforms carry.
    ///
    /// Not ``backgroundOpacity`` above, which the builder folds per cell for a
    /// backend with nowhere else to put it: a frontend whose fragment stage
    /// applies opacity from its uniforms feeds this one and leaves the other
    /// at one, or the window darkens by the square.
    float uniformBackgroundOpacity = 1.0f;
    /// The terminal default background, sRGB 0..1. `.w` is the default-bg-only
    /// opacity factor the cell fragment applies when a cell's background
    /// matches the default, so SGR-coloured cells stay solid.
    Vec4 uniformDefaultBackground { 0.0f, 0.0f, 0.0f, 1.0f };
    /// DECSET 7402: glyphless cells paint the letterbox opaquely.
    bool isEdgeToEdge = false;
    /// Whether pixel reports use the drawable origin for this captured picture.
    bool isFullscreenContentActive = false;

    // SGR 5 / 6 blink: when set, a cell carrying Attribute::Blink fades its
    // foreground alpha along the shared blink curve (Core/RenderCell.h) at
    // `blinkTicks`, folded into inst.foreground.w. Off ⇒ blinkAlpha stays 1.0.
    bool resolveBlink = false;
    std::uint32_t blinkTicks = 0;

    // Per-row DECDWL / DECDHL line attribute (LINE_ATTR_*), one entry per row in
    // display order. A DOUBLE_WIDTH/HEIGHT row paints each column at twice the
    // advance (xStep 2), drops cells whose 2x lead starts past the last column,
    // clips the straddling cell to the grid content width, and — for
    // DOUBLE_HEIGHT — halves the glyph atlas V range to show the top/bottom half
    // stretched to full cell height. Null ⇒ every row NORMAL (xStep 1, no clip,
    // no V-halve).
    const std::uint8_t* lineAttributes = nullptr;

    /// One ``RowFlag`` set per row, raw (``RowMeta::flags``), or null. A row
    /// without ``RowFlag/Flowed`` holds no proportional run, so the walk asks
    /// no cell of it for one; null means the caller did not read the flags,
    /// and every cell is asked, which is what a caller building from bare
    /// cells gets.
    const std::uint8_t* rowFlags = nullptr;

    // The session's DECDLD soft font, or null when it never downloaded one. A
    // cell whose code point sits in `[drcsCodePointBase, +drcsMaxCells)` draws
    // from this bitmap rather than from the font; nothing else reads it.
    //
    // Borrowed for the frame, and by POINTER because a SoftFont is ~4.7KB of
    // matrix that a capture copies only when its `serial` changes. A DRCS glyph
    // is rasterised here, in Core, and the backend is asked only to stage the
    // finished plane (``GlyphBackend::stageMonoCoverage``) — so the soft font
    // stops at this struct and never crosses the glyph seam, which owes its
    // implementers a contract naming no model type.
    const Ubiquitty::VT::SoftFont* softFont = nullptr;

    // The serial of a soft font this frame REPLACED, or 0 when none was.
    //
    // A committed DECDLD takes a fresh serial, so its glyphs key into the shared
    // mono atlas under names the old font's rasterisations can never be found by
    // again. Left resident they age out through the LRU competing with ordinary
    // text, so a stream of downloads evicts real glyphs and each one re-rasterises
    // through the frontend's font engine — which costs far more than the DRCS
    // raster that displaced it. Naming the retired serial lets the builder release
    // exactly the keys that went dead, bounding DRCS residency at the `drcsMaxCells`
    // one font can define.
    std::uint32_t retiredSoftFontSerial = 0;

    // An opaque fill occluding the whole grid, as DECSET 7402's Color letterbox
    // uses. Every cell paints `occludingColor` and nothing else is emitted — no
    // glyph, no decoration, no box fill.
    //
    // The colour has to be per-cell rather than a frame-level clear because the
    // cell shader paints each cell's OWN background opaquely under this mode and
    // has no letterbox uniform to reach for. The glyph pass is a separate pipeline
    // that does not know about the mode at all, so a glyph emitted here would draw
    // ON TOP of the letterbox it is supposed to be hidden behind — which is why
    // the suppression is part of this input rather than an optimisation a caller
    // may skip.
    bool isGridOccluded = false;
    Vec4 occludingColor { 0.0f, 0.0f, 0.0f, 1.0f };

    // OSC 8 / regex link styling. A link cell carries an always-on underline and
    // takes a link colour for both its text and that underline. `linkCells`, when
    // set, is a rows*columns row-major array of `LinkState`; null ⇒ no link
    // styling. The capture resolves OSC8 ids and the hovered run into it, because
    // the frontend owns the link ring the ids index and the pointer tracking the
    // hover comes from.
    //
    // A STATE rather than a flag, because hover is not a second kind of link but a
    // different presentation of the same one: the hovered run reads solid in the
    // accent colour whatever the resting style is, so a bool would force every
    // frontend to either lose the distinction or fall back to its own cell walk
    // for the duration of a pointer hover.
    //
    // The colour is unconditional rather than gated on a "should I override"
    // flag. A frontend that flags a cell as a link necessarily has a link colour
    // to draw it in, and a gate there can only express "flagged as a link but
    // drawn as if it were not" — a state no frontend wants and one that reads as
    // a link losing its affordance.
    const std::uint8_t* linkCells = nullptr;
    Vec4 linkColor { 0.0f, 0.0f, 0.0f, 1.0f };

    // The accent a HOVERED link run takes, for its text and its rule. Separate
    // from `linkColor` because the two answer different questions: the resting
    // colour is the theme's legible link against this background, and this is the
    // transient interaction highlight, which stays the accent whatever the theme.
    Vec4 linkHoverColor { 0.0f, 0.0f, 0.0f, 1.0f };

    // The rules a `RestingFile` / `RestingMailto` cell draws, as
    // Ubiquitty::VT::underline* values. Only consulted for those states, which a
    // frontend that did not ask for per-kind styling never produces.
    //
    // Carried per kind rather than mapped in Core for the same reason the base
    // style is: which scheme reads dotted and which reads dashed is presentation,
    // and a builder that picked would impose it on every frontend.
    std::uint8_t linkUnderlineStyleFile = defaultLinkUnderlineStyleFile;
    std::uint8_t linkUnderlineStyleMailto = defaultLinkUnderlineStyleMailto;

    // The underline a link cell carries when it has no SGR underline of its own,
    // as a Ubiquitty::VT::underline* value. Carried rather than fixed because it
    // is a user setting: a link reads solid or dotted by preference, and a
    // builder that picks one imposes it on every frontend.
    //
    // Defaulted to a visible rule, not to the zero of the enum. `underlineNone`
    // is 0, so a zero default would leave a cell flagged as a link drawing no
    // affordance at all — a frontend that filled `linkCells` and stopped there
    // would silently lose every link underline, and the flag it did set is
    // evidence it wanted one.
    std::uint8_t linkUnderlineStyle = defaultLinkUnderlineStyle;

    // Opt-in subpixel mono glyph pass. false ⇒ a mono glyph is baked into its
    // CellInstance atlas UV at phase 0 (the cell-baked path). true ⇒ a mono
    // INKED glyph is emitted as a separate pixel-snapped GlyphInstance at its
    // subpixel phase (0..subpixelPhaseCount-1) and its CellInstance carries
    // background only, so text reads crisp. Wide / emoji / box-drawing /
    // double-row / combining / blank cells keep the cell-baked path regardless
    // (the glyph pass is mono-only).
    //
    // Slot geometry is ATLAS-NATIVE, the size the glyph was rasterised at. It is
    // not the size it draws: the cell can be narrower than the font's advance
    // (DECCOLM) or a different height (window resize, font-size change), and the
    // glyph has to track its cell so text stays aligned with the background and
    // the cursor. The builder scales by cell-over-reference; a backend that
    // pre-scaled here would be doing that arithmetic per frontend.
    //
    // The defaults reduce a margin-free backend whose cell is its atlas cell to
    // origin = (snappedPenX, row*cellHeight), size = (cellWidth, cellHeight):
    // slot ≤ 0 gives the cell size, reference ≤ 0 gives unit scale.
    bool subpixelGlyphs = false;
    // The subpixel phase grid: how many phases a pen resolves to and how it
    // picks one (SubpixelPhase.h). The portable contract is 8 nearest-rounded
    // phases, and a backend whose rasteriser positions continuously keeps it;
    // the CoreText arm passes the grid CoreText itself quantises pens onto for
    // the font's device size and floors like it does, so the atlas holds one
    // mask per position CoreText distinguishes and each column lands where a
    // native draw at the same fractional pen would. The value here and the
    // divisor the backend rasterises its pen with are the same number: both
    // come from nativeSubpixelPhaseCount of the same pixel size, or both are 8.
    int subpixelPhaseCount = 8;
    PhaseRounding subpixelPhaseRounding = PhaseRounding::Nearest;
    // The phase count two-cell glyphs are keyed and blitted at, where the
    // backend's wide page has a pen grid of its own; 0 for the count above. A
    // rasteriser that grows two-cell glyphs draws them at a larger device
    // size than the mono text, and where its pen grid is a function of the
    // device size (SubpixelPhase.h nativeSubpixelPhaseCount) the grown glyph
    // lands on a coarser one. Like the count above, this and the divisor the
    // backend rasterises a wide pen with are the same number.
    int widePhaseCount = 0;
    // Whether the backend rasterises a glyph per smoothing tier (GlyphKey.h
    // rasterTierForLinearLuma). True for the CoreText arm, whose smoothing
    // weights a glyph by its fill colour; the builder then folds each cell's
    // foreground tier into its raster variant and slot key. False keeps every
    // variant at tier 0, so a backend that smooths nothing keys on no tier.
    bool hasRasterTiers = false;
    // Whether the frame's face is one Core rasterises itself (the embedded
    // bitmap tier). Core's own verdict, from the style's unifont scale — a
    // frontend passes the scale Core gave it and never decides the tier.
    bool isBitmapFace = false;
    // Whether braille draws as the renderer's 2x4 dot lattice rather than
    // from the face: the reader's `font.braille_grid`, carried here by the
    // frontend like every other setting the builder acts on.
    bool drawsBrailleGrid = true;
    BrailleStyle brailleStyle = defaultBrailleStyle;
    // How the shade glyphs draw (`[appearance] shade_blocks`), carried here
    // by the frontend like the braille route above.
    ShadeStyle shadeStyle = defaultShadeStyle;
    // Which stretches the row walk shapes for this frame's font — the sheet's
    // probe verdict, copied here by the frontend so the builder's prescan and
    // the backend's walk cannot disagree. OperatorRuns is the classic economy
    // and the safe default; a texture-healing font needs AllEligibleRuns or
    // the same words render two ways depending on invisible neighbours.
    LigatureParticipation ligatureParticipation = LigatureParticipation::OperatorRuns;
    /// The font's measured box strokes, for the procedural fills.
    ///
    /// Copied from the frontend's probe so a healed box character shares the
    /// exact centreline and thickness of the strokes the font draws itself.
    /// Bands left invalid keep the em-derived weights — an unprobed frontend
    /// changes nothing.
    BoxDrawing::BoxStrokeProfile boxStrokeProfile {};
    FloatSize glyphMarginInPixels {};
    float glyphSlotWidthInPixels = 0.0f; // ≤ 0 ⇒ metrics.cellWidth
    float glyphSlotHeightInPixels = 0.0f; // ≤ 0 ⇒ metrics.cellHeight

    /// The font's true horizontal advance, in the same atlas pixels as the slot.
    ///
    /// Native raster windows step by this advance, independently of the tracked
    /// cell pitch. It stays unrounded: stepping by the ceil'd slot box introduces
    /// gaps within a glyph. `metrics.atlasToDevicePixels` maps its atlas pixels
    /// to the display; cell width never scales native ink. Zero falls back to
    /// the glyph box or the caller's cell for a backend without a face advance.
    ///
    /// Read from the atlas each frame, never captured when the renderer is
    /// built. A backend that fits its surface to the grid keeps the cell at its
    /// natural size and cannot tell the difference — until a font-size change or
    /// a fixed-size surface moves one and not the other, and then every glyph is
    /// off by the same few percent. A uniform proportional error reads as a font
    /// problem rather than a stale field, which is what makes it expensive.
    float glyphReferenceAdvanceInPixels = 0.0f;
    /// The font's true line height, in atlas pixels. Zero means no scaling.
    ///
    /// Per frame, for the reason above.
    float glyphReferenceLineHeightInPixels = 0.0f;
    // The two-cell slot and its cell box, in atlas pixels. Zero width leaves
    // wide glyphs on their cells whatever InstanceBuffers offers.
    float glyphWideSlotWidthInPixels = 0.0f;
    float glyphWideSlotHeightInPixels = 0.0f;
    float glyphBoxWidthInPixels = 0.0f;
    // The 2x master slot a DECDWL/DECDHL quad scales against. The quad takes the
    // full slot width and HALF its height: a DHL pair's two rows sample the two
    // halves of one master, so a full-height quad would draw the whole master
    // twice. Zero width leaves doubled glyphs on their cells whatever
    // InstanceBuffers offers.
    float glyphDoubleSlotWidthInPixels = 0.0f;
    float glyphDoubleSlotHeightInPixels = 0.0f;

    // Per-row damage in RenderPlan::dirtyRows' shape: length `rows`, non-zero
    // where the row changed since this caller last emitted it. Null builds every
    // row, which is the only safe default.
    //
    // A clean row emits nothing — no cell, decoration, overlay or glyph
    // instance. Only a caller that still holds that row's previous output may
    // set this; a backend compositing a fresh frame out of the returned spans
    // alone would drop every row it skipped. The retained-target shape this
    // serves is the Metal frame encoder's grid cache, which draws a clean row
    // from the texture it kept rather than from this frame's spans.
    const std::uint8_t* dirtyRows = nullptr;
};

/// The most overlay instances ``buildCellInstances`` can write for a grid this
/// tall: the bell flash, the 132-column edge, the cursor and the two quads of
/// the password badge (the fixed five), one translucent quad per selection row,
/// and a prompt-mark bracket in each gutter of every executed row.
///
/// An undersized overlay span is not an error and produces no diagnostic. The
/// builder counts every instance it wanted to write and skips the ones past
/// capacity, so the caller reads a count it must clamp and the shortfall reaches
/// the screen as a missing bracket rather than as a failure.
///
/// The arithmetic lives here because it was open-coded in three frontends and in
/// ``SoftwareRenderer``: a pass added to the builder changes what every one of
/// them must allocate, and four copies of a sum is four places to not update.
/// Sizing from this function rather than from the span's description is the only
/// form that survives the next pass.
///
/// The search term is the caller's own span count rather than a per-row
/// constant. A row can hold any number of matches, so no multiple of `rows`
/// bounds it, and the caller already holds the list it is about to pass — asking
/// for its length is exact where a guessed ceiling would be either wasteful or
/// silently short. It is not the "size from a list of what writes into the span"
/// mistake above: this is the caller's input, not an inventory of the builder's
/// passes.
///
/// - Parameters:
///   - rows: Visible rows in the frame about to be built.
///   - otherSearchMatchSpanCount: Length of `BuildInputs::otherSearchMatchSpans`,
///     one quad each. Zero for a caller that draws its own search overlays.
///   - triggerHighlightSpanCount: Length of `BuildInputs::triggerHighlightSpans`,
///     one quad each. Zero for a caller that draws its own trigger overlays.
///   - hiliteRegionSpanCount: Length of `BuildInputs::hiliteRegionSpans`, one
///     quad each. Zero for a caller that draws its own hilite overlay.
/// - Returns: The span length that cannot overflow. Per visible row it holds
///   the cursor, the prompt-mark bracket quads and the focused search match's
///   pill ring.
constexpr std::size_t maxOverlaysForRows(int rows, std::size_t otherSearchMatchSpanCount = 0, std::size_t triggerHighlightSpanCount = 0, std::size_t hiliteRegionSpanCount = 0)
{
    const std::size_t visibleRows = rows > 0 ? static_cast<std::size_t>(rows) : 0;
    constexpr std::size_t focusedPillRingsPerRow = 1;
    return 3 + static_cast<std::size_t>(maxPasswordLockQuads) + visibleRows * (1 + static_cast<std::size_t>(maxPromptMarkQuadsPerRow) + focusedPillRingsPerRow) + otherSearchMatchSpanCount + triggerHighlightSpanCount + hiliteRegionSpanCount;
}

// Caller-owned output spans and the counts written. The focused block cursor
// is the cursor tier at the tail of the cell, glyph, rule and box-fill spans
// (``InstanceCounts::cursorTier``) rather than a quad in the overlay span.
//
// Size the overlay span with `maxOverlaysForRows`, never from a list of what
// currently writes into it: every pass added to the builder invalidates such a
// list, the span silently truncates rather than failing, and the reader who
// sized from the list is not the one who pays.
/// The spans one frame's build writes into, borrowed for the duration.
///
/// A count returned above its capacity is the builder reporting what it needed
/// rather than an error: the caller grows to exactly that and builds again. The
/// spans belong to the caller and are valid only for the call.
///
/// The storage must not be storage a submitted frame is still reading. The
/// build overwrites these spans from the first instance, so a caller that
/// hands the same buffer to consecutive frames while a device reads the
/// previous one presents a frame assembled out of two — and the corruption
/// lands wherever the new frame differs most from its predecessor, which under
/// a scroll is the shaped runs, so it reads as a font or shaping fault rather
/// than as a lifetime one. Two discharges: give the storage a slot per frame in
/// flight and advance per build (the macOS arm rings three deep, each slot
/// stamped with the command buffer that read it), or drain before reuse (the
/// Vulkan arm waits its queue idle after submit, which is correct and costs the
/// pipelining). A caller with one buffer and no drain has neither.
///
/// This cannot be checked from the pixels here: every snapshot and golden in
/// this tree renders behind a drain, which is the control arm of the race
/// rather than a witness to it. The requirement is stated because the failure
/// is invisible to the oracles that would otherwise be trusted to catch it.
struct UBIQUITTY_NONESCAPABLE InstanceBuffers {
    CellInstance* cells = nullptr;
    std::size_t cellCapacity = 0;
    DecorationInstance* decorations = nullptr;
    std::size_t decorationCapacity = 0;
    OverlayInstance* overlays = nullptr;
    std::size_t overlayCapacity = 0;
    // Mono glyphs for subpixel text and explicit-width OSC 66 blocks.
    GlyphInstance* glyphs = nullptr;
    std::size_t glyphCapacity = 0;
    // Two-cell glyphs, for a backend that keeps them on a sheet of their own.
    // Provided, a wide lead's glyph lands here and its cell carries no UV; a
    // cell quad is sampled with one texture bound and cannot reach another.
    // Null leaves the glyph on the cell, which is what a backend with a single
    // sheet needs and what every caller got before this stream existed.
    GlyphInstance* wideGlyphs = nullptr;
    std::size_t wideGlyphCapacity = 0;
    // Glyphs shaped in the proportional face, kept on their dedicated sheet.
    // Provided, the anchor's shaped glyphs land here and sample only that
    // sheet; null leaves the backend's strip or grid-face fallback in charge.
    // This cannot share ``wideGlyphs``: the two populations have independent
    // budgets and eviction, and one draw call can sample only one texture.
    GlyphInstance* proportionalGlyphs = nullptr;
    std::size_t proportionalGlyphCapacity = 0;
    // Bounded fallback strips for proportional runs. These cannot share the
    // ligature stream: one draw call samples one texture, and proportional
    // text owns a sheet whose cache and eviction are independent of the grid
    // face's ligature repertoire.
    GlyphInstance* proportionalStrips = nullptr;
    std::size_t proportionalStripCapacity = 0;
    // Colour-emoji glyphs, for a backend keeping them on an RGBA sheet apart from
    // the mono coverage one. Provided, an emoji's glyph lands here and its cell
    // carries no UV; null leaves it on the cell, which is what a single-sheet
    // backend needs. The quad is square and centred in the cell's span rather
    // than filling it: the slot is square, and a span box is not.
    GlyphInstance* colorGlyphs = nullptr;
    std::size_t colorGlyphCapacity = 0;
    // Ligature strip slices, for a backend that shapes runs onto a sheet of
    // their own. Provided, a covered cell's glyph lands here and its cell
    // carries no UV; null puts every cell on the mono path, so a backend with no
    // shaper renders the run's characters unligated rather than blank.
    GlyphInstance* ligatureGlyphs = nullptr;
    std::size_t ligatureGlyphCapacity = 0;
    // DECDWL/DECDHL glyphs, for a backend that keeps 2x masters on a sheet of
    // their own. Provided, a doubled cell's glyph lands here and its cell carries
    // no UV; null bakes the master onto the cell, which crops to one row and is
    // what every caller got before this stream existed.
    //
    // The quad is not the cell, and a reader who assumes it from the field's name
    // writes geometry that looks nearly right: it is two cells wide and half a
    // slot high with the margins backed off, and its vertical midline sits on the
    // DHL pair's shared row boundary. That midline is what makes the top and
    // bottom halves meet without a seam — cell-sized quads misregister at every
    // DHL boundary while still drawing a plausible letter.
    //
    // One stream carries Top, Bottom and Full together. The region is spent
    // before the instance exists — on the origin and on the UV the backend
    // returned — so nothing downstream discriminates, and a lane per region would
    // model a distinction the draw call does not make.
    GlyphInstance* doubleGlyphs = nullptr;
    std::size_t doubleGlyphCapacity = 0;
    // Box-drawing geometry, as overlay quads. Provided, a box-drawing cell is
    // decomposed into strokes and fills here and never reaches the glyph atlas;
    // null leaves it to the font, which draws a plausible character that does
    // not tile with its neighbours.
    OverlayInstance* boxFills = nullptr;
    std::size_t boxFillCapacity = 0;
    // Margin-restore strips beside each rebuilt row: the glyph-overhang
    // margins repainted to the clear colour. Their own stream because a
    // restore replaces pixels rather than compositing over them — at a
    // sub-unity background opacity a blended restore adds its alpha to the
    // margin it means to reset, and the gutters beside every dirty row read
    // denser than the frame on a translucent window.
    OverlayInstance* marginStrips = nullptr;
    std::size_t marginStripCapacity = 0;
    // Where this build's ink crosses from one row into the next: one byte per
    // row boundary, `rows + 1` of them, entry `b` the top edge of row `b`,
    // nonzero where a crossing stands. Set for every boundary the ink of a
    // glyph, rule or box quad drawn into the grid crosses — measured ink where
    // the backend measured it, the quad where it did not (a stacked accent
    // reaching up into the row above, a tall fallback glyph reaching down),
    // and cleared everywhere else; the cursor tier draws over
    // the grid rather than into it and marks nothing, and the grid's outer
    // edges, 0 and `rows`, are the margin strips' and stay clear. A backend
    // that keeps its target across frames reads it with
    // ``settleRowInkCrossings``: a row rebuilt over a target that ink crosses into
    // or out of is not the row a full frame draws. Null, or shorter than the
    // boundaries, is not written.
    std::uint8_t* rowInkCrossings = nullptr;
    std::size_t rowInkCrossingCapacity = 0;
    // Row scratch for a skip-ink rule under a run drawn glyph by glyph. A
    // caller that keeps one across frames passes it so the build reuses its
    // storage; null has the build keep one of its own for the call.
    CellInstanceScratch* scratch = nullptr;
    // Where each column's letter stands on the rows a proportional primary
    // face laid out as proportional runs (LetterLayout.h): `rows * columns`
    // extents, row-major, and one byte per row that is nonzero where the row
    // was laid out so. Written for every row the build walks, letters or not;
    // a row the build skipped under a dirty-row mask keeps what the last
    // build wrote, which is why a caller building from such a mask must pass
    // storage it keeps across frames — the overlay pass reads a clean row's
    // letters for the cursor and the selection, and the capture answers hit
    // tests from them. Null, or short, has the build keep storage of its own
    // for the call, which is right only for a whole build.
    LetterExtent* letters = nullptr;
    std::size_t letterCapacity = 0;
    std::uint8_t* letterRows = nullptr;
    std::size_t letterRowCapacity = 0;
};
/// Which of the decoration pass's emit sites a frame reached, one bit each.
///
/// The decoration stream carries seventeen sites and ``DecorationKind`` sorts
/// them into four, so fourteen arrive as `Solid` and are indistinguishable once
/// emitted: an overline, a strike, a framed edge and a plain underline are the
/// same instance shape in different places. A comparison that sees the stream
/// fed cannot tell a scene driving one site from a scene driving all fourteen,
/// which is how a pass with no input hides inside a stream that looks busy.
///
/// The bit is set where the instance is written rather than derived afterwards,
/// because geometry is what the sites differ in and reconstructing intent from
/// coordinates is guesswork.
enum class DecorationSite : std::uint32_t {
    UnderlineSingle = 1u << 0,
    UnderlineDoubleUpper = 1u << 1,
    UnderlineDoubleLower = 1u << 2,
    UnderlineCurly = 1u << 3,
    UnderlineDotted = 1u << 4,
    UnderlineDashed = 1u << 5,
    Strike = 1u << 6,
    Overline = 1u << 7,
    FramedTop = 1u << 8,
    FramedBottom = 1u << 9,
    FramedLeft = 1u << 10,
    FramedRight = 1u << 11,
    IdeogramUnderLeft = 1u << 12,
    IdeogramUnderRight = 1u << 13,
    IdeogramOverLeft = 1u << 14,
    IdeogramOverRight = 1u << 15,
    IdeogramStress = 1u << 16,
};

/// How many instances of each stream one frame needs.
///
/// Needs, not received: every count keeps rising after its buffer is full,
/// while the writes stop at the capacity in ``InstanceBuffers``. A count above
/// its capacity is therefore the caller's regrow signal — resize to the count,
/// build again, and the second pass is sized exactly. A count that stopped at
/// the capacity could never exceed it, and every such regrow check downstream
/// would hold by construction rather than by room.
struct InstanceCounts {
    std::size_t cells = 0;
    std::size_t wideGlyphs = 0;
    std::size_t proportionalGlyphs = 0;
    std::size_t proportionalStrips = 0;
    std::size_t decorations = 0;
    std::size_t overlays = 0;
    std::size_t glyphs = 0;
    std::size_t colorGlyphs = 0;
    std::size_t ligatureGlyphs = 0;
    std::size_t doubleGlyphs = 0;
    std::size_t boxFills = 0;
    /// Margin-restore strips beside rebuilt rows; see the outputs' field.
    std::size_t marginStrips = 0;
    /// Glyphs served from a DECDLD soft font rather than the text atlas.
    ///
    /// Counted apart from ``glyphs``, which it is also part of: a DRCS cell
    /// emits an ordinary mono glyph, so a scene that stops reaching the soft-font
    /// pass leaves ``glyphs`` a few short of where it was and still well above
    /// zero. Without its own count the pass has no figure that can go to zero,
    /// and a coverage gate reading only the streams cannot tell the difference.
    std::size_t softFontGlyphs = 0;
    /// The union of every ``DecorationSite`` this frame emitted.
    ///
    /// A parity comparison reads it to distinguish a covered pass from a fed
    /// stream; a coverage gate reads it to refuse a set that has shrunk. It
    /// lives here rather than on ``DecorationInstance`` because that struct is
    /// uploaded to the GPU and a field the shader never samples would cost
    /// every frame to answer a question only the harness asks.
    std::uint32_t decorationSites = 0;

    /// The cursor tier: how many instances at the END of each stream are the
    /// cursor's cell, re-emitted with the cursor's colours.
    ///
    /// A focused, settled block draws as the cell it stands on with the fill
    /// lerped toward the cursor colour and the glyph inverted under it. Those
    /// instances are built last, so they are each stream's tail, and every
    /// backend draws the tails as one layer of their own after the heads, the
    /// images and the overlays — a backend that keeps its grid across frames
    /// draws `count - tail` into the grid and the tail after the blit. The
    /// grid never holds a cursor, so a blink or a move owes it no row, and
    /// nothing a program draws around the cursor reaches the cursor's pixels.
    /// A backend drawing the tails in with the heads composites the cell's
    /// own ink twice, once in each colour. Every count is zero when the frame
    /// draws no such cursor.
    struct CursorTier {
        std::size_t cells = 0;
        std::size_t glyphs = 0;
        std::size_t wideGlyphs = 0;
        std::size_t proportionalGlyphs = 0;
        std::size_t proportionalStrips = 0;
        std::size_t colorGlyphs = 0;
        std::size_t ligatureGlyphs = 0;
        std::size_t doubleGlyphs = 0;
        std::size_t decorations = 0;
        std::size_t boxFills = 0;
    };
    CursorTier cursorTier;
};

// Build the frame's instances from `in`. Emits one CellInstance per visible
// cell (wide-continuation halves skipped; atlasTopLeft == atlasBottomRight
// ⇒ blank), the decoration pass (underline single/double/curly/dotted/dashed +
// color + skip-ink, strikethrough, overline, framed/encircled) in device pixels
// using in.metrics, and the overlay pass (cursor via OverlayGeometry, drawn over
// cells + decorations; the focused block is the cursor tier at each stream's
// tail, see ``InstanceCounts::cursorTier``). Returns the counts written (each
// may exceed its capacity — the caller sizes the buffers). Backend-neutral: no
// GPU / platform / simd types.
///
/// A backend ``isDrawable`` rejects writes nothing — no cell, decoration,
/// overlay or glyph instance, and every count comes back zero. A table lets a
/// required entry be absent, which an abstract base could not; the check is made
/// once per frame rather than per call site, and a blank frame is the loudest
/// symptom available.
///
/// - Parameters:
///   - in: The frame to build.
///   - out: The caller-owned spans to write into, sized by the caller.
/// - Returns: The counts written, each of which may exceed its capacity.
InstanceCounts buildCellInstances(const BuildInputs& in, const GlyphBackend&, const InstanceBuffers& out);

/// Assembles the per-frame uniforms from the capture's own facts.
///
/// One derivation, so a renderer whose encode runs far from its capture and
/// the frontend that resolved the frame agree by construction: every field
/// comes from the ``BuildInputs`` the capture filled, and a second assembly
/// beside an encode call would be a fork — two spellings of one frame that
/// can disagree about a pixel with no way to attribute it.
///
/// - Parameter in: The captured frame's inputs.
/// - Returns: The constants every pass reads.
[[nodiscard]] inline FrameUniforms deriveFrameUniforms(const BuildInputs& in)
{
    FrameUniforms uniforms {};
    uniforms.viewportSizeInPixels = in.viewportSizeInPixels;
    uniforms.cellSizeInPixels = { in.metrics.cellWidth, in.metrics.cellHeight };
    uniforms.originInPixels = in.originInPixels;
    uniforms.backgroundOpacity = in.uniformBackgroundOpacity;
    uniforms.defaultBackground = in.uniformDefaultBackground;
    uniforms.edgeToEdge = in.isEdgeToEdge ? 1 : 0;
    uniforms.bitmapTier = in.isBitmapFace ? 1 : 0;
    return uniforms;
}

} // namespace Rendering
} // namespace Ubiquitty
