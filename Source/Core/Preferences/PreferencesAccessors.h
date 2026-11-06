// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Effective-value accessors over a Values. Each implements "field.Set ? c.field :
// DEFAULT" for one settings key, reading the canonical default from PreferencesDefaults.h.
// Pure C++ (const Values& -> ints / floats / plain enums; no platform types).

#include "Core/Clipboard/Scope.h"
#include "Core/Image/ImagePresentation.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesDefaults.h"
#include "Core/Preferences/Profile.h"
#include "Core/Preferences/Schema.h"
#include "Core/Rendering/BrailleLattice.h"
#include "Core/Rendering/Font/FontFallbackPolicy.h"
#include "Core/Rendering/FontMetrics.h"
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Rendering/RemoteViewport.h"
#include "Core/Rendering/ShadeStyle.h"
#include "Core/Screen/ScrollTuning.h"
#include "Core/Unicode/Encodings.h"
#include "PAL/BellMode.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Preferences {

// The runtime font-size zoom clamps against the single font.size range from
// Schema.h (unit-agnostic: CoreText pt on macOS, FreeType px on Linux).
namespace Zoom {

// The canonical zoom range = the font.size schema row.
inline const Schema::NumericRange& fontZoomRange()
{
    // font.size is a row of the schema table, and this initializer is the
    // proof: a table without that row answers null, dereferencing null is not a
    // constant expression, and the build stops. A runtime fallback would have
    // answered a second copy of the same numbers instead, leaving the two free
    // to disagree. Stated as an initializer rather than as an assertion because
    // GCC folds the lookup and then refuses to compare the result against null.
    static constexpr const Schema::NumericRange& row = *Schema::rangeFor("font", "size");
    return row;
}

// The size one Zoom In / Zoom Out command moves, in the range's units.
//
// A whole unit rather than the schema row's step: the row's step is a quarter
// point, so a size can be set between two points, and a menu command that moved
// by that would need four presses to grow the text by one. The zoom is an offset added to the
// configured size, so a whole-unit step over a fractional base keeps the
// fraction — 12.5 zooms to 13.5, never to 13.
inline constexpr double fontZoomStep = 1.0;

// Clamp a font-zoom request to [minimum, maximum] from the shared range.
inline double clampFontZoom(double size)
{
    const Schema::NumericRange& range = fontZoomRange();
    if (size < range.minimum)
        return range.minimum;
    if (size > range.maximum)
        return range.maximum;
    return size;
}

// Integer (pixel-size) overload for the Linux FreeType path; the schema bounds
// are whole numbers, so the narrowing is exact.
inline int clampFontZoom(int size)
{
    const Schema::NumericRange& range = fontZoomRange();
    if (size < static_cast<int>(range.minimum))
        return static_cast<int>(range.minimum);
    if (size > static_cast<int>(range.maximum))
        return static_cast<int>(range.maximum);
    return size;
}

/// Renders a window's zoom over the configured size.
///
/// A zoom is an offset in size units over the configured size, and nothing
/// about it persists: the configured size is the reader's, the offset is the
/// window's. The sum is clamped to the schema range, so an offset that would
/// push past a bound renders at the bound.
///
/// - Parameters:
///   - configuredSize: The size the configuration names.
///   - zoomOffset: The window's offset over it, in the same units.
/// - Returns: The size the window renders, within the schema range.
inline double zoomedFontSize(double configuredSize, double zoomOffset)
{
    return clampFontZoom(clampFontZoom(configuredSize) + zoomOffset);
}

/// Answers the offset that renders `size` over `configuredSize`.
///
/// The inverse of `zoomedFontSize` for every size the range admits, so a
/// step taken from a window's live size lands on a whole size the range
/// contains.
///
/// - Parameters:
///   - configuredSize: The size the configuration names.
///   - size: The size the window is to render, clamped to the range.
/// - Returns: The offset `zoomedFontSize` renders `size` from.
inline double fontZoomOffsetForSize(double configuredSize, double size)
{
    return clampFontZoom(size) - clampFontZoom(configuredSize);
}

/// Answers the size a companion face renders at beside a zoomed grid.
///
/// A face set beside the grid — the proportional face — is configured at a
/// size of its own, and a zoom that moves the grid moves it by the same
/// ratio, so the two keep the proportion the configuration set. A companion
/// size of zero means "none configured" and stays zero, as does a grid
/// without a configured size to take a ratio from.
///
/// - Parameters:
///   - configuredSize: The grid size the configuration names.
///   - zoomedSize: The grid size the window renders.
///   - companionSize: The companion face's configured size, or zero.
/// - Returns: The companion face's size beside the zoomed grid.
inline double zoomedCompanionSize(double configuredSize, double zoomedSize, double companionSize)
{
    if (companionSize <= 0.0 || configuredSize <= 0.0 || zoomedSize <= 0.0)
        return companionSize;
    return companionSize * (zoomedSize / configuredSize);
}

} // namespace Zoom

namespace Effective {

// [font]
double fontSize(const Values& c);
/// The em size proportional runs are set at, in points.
///
/// - Parameter c: The configuration to read.
/// - Returns: The size, or zero for one optical step over `fontSize`.
double proportionalFontSize(const Values& c);
bool ligatures(const Values& c);
double lineSpacing(const Values& c); // extra pt (lineSpacingPoints)
bool wideGlyphBoost(const Values& c);
/// Resolves whether fallback text matches the primary font's visual size.
///
/// - Parameter c: The configured values, including any explicit override.
/// - Returns: The configured choice, or the enabled default.
bool matchFallbackSize(const Values& c);

/// Whether braille draws as the renderer's dot lattice.
///
/// - Parameter c: The configuration to read.
/// - Returns: The setting, or its default.
bool brailleGrid(const Values& c);
/// Returns the procedural braille dot style.
///
/// - Parameter values: The configuration to read.
/// - Returns: The configured style, or its default.
Rendering::BrailleStyle brailleStyle(const Values& values);
/// Returns how the shade glyphs draw.
///
/// - Parameter values: The configuration to read.
/// - Returns: The `[appearance] shade_blocks` style, or the pattern default.
Rendering::ShadeStyle shadeStyle(const Values& values);
/// Whether the renderer presents in step with the display's refresh.
///
/// Read through this accessor, never the field: a raw optional in a boolean
/// context answers whether it is set, so `[render] vsync = false` would read
/// as enabled.
[[nodiscard]] bool vsync(const Values& c);
/// Whether the title bar shows the working directory's folder proxy icon.
///
/// Without an accessor the frontend read the field directly and guarded on
/// `has_value()`, so deleting the key left the last state standing rather than
/// restoring the default.
[[nodiscard]] bool folderIcon(const Values& c);
/// Whether that folder proxy icon stays resident rather than being revealed by
/// a hover over the title.
///
/// The accessibility setting for title-bar icons forces the same residency, so
/// a frontend reads the two together rather than either alone.
[[nodiscard]] bool folderIconAlways(const Values& c);
/// Whether a title draws a program's icon before its name: `[appearance]
/// process_icons`, on by default.
[[nodiscard]] bool processIcons(const Values& c);
/// The programs a title leaves the working directory out for, comma- or
/// newline-separated: `[title] directory_hidden_for`.
[[nodiscard]] std::string directoryHiddenFor(const Values& c);
/// Whether a title leaves the directory out while a program fills the
/// screen: `[title] directory_hidden_in_full_screen`, off by default.
[[nodiscard]] bool directoryHiddenInFullScreen(const Values& c);
double wideGlyphScale(const Values& c);
/// The points added to each cell's advance, which may be negative.
///
/// - Parameter c: The configuration to read.
/// - Returns: The tracking, in points.
double characterSpacing(const Values& c);

/// The weight upright text is drawn at, or zero for the face's own.
///
/// Zero is the absence of a setting rather than a light weight: a family whose
/// upright member is not 400 would be moved by a default that named a number.
///
/// - Parameter c: The configuration to read.
/// - Returns: The weight, 100 through 900, or zero.
int fontWeight(const Values& c);

/// The weight bold text is drawn at, or zero for the face's own bold.
///
/// Separate from ``fontWeight`` so the gap between normal and bold can be
/// widened, closed, or inverted.
///
/// - Parameter c: The configuration to read.
/// - Returns: The weight, 100 through 900, or zero.
int boldFontWeight(const Values& c);

/// The width text is drawn at, in percent, or zero for the face's own.
///
/// Meaningful only on a face that ships a width axis or width members; the
/// matcher leaves a single-width family alone, so the setting reads as inert
/// there rather than as a broken face.
///
/// - Parameter c: The configuration to read.
/// - Returns: The width percent, 50 through 200, or zero.
double fontWidthPercent(const Values& c);

// [underline]
double underlineThickness(const Values& c);
double underlineOffset(const Values& c);
bool underlineSkipInk(const Values& c);
/// Whether a run's outer underline reaches the letter's ink past the cell:
/// `[underline] follows_ink`.
///
/// - Parameter c: The config fold.
/// - Returns: The setting, or its default.
bool underlineFollowsInk(const Values& c);
double underlineSkipInkPad(const Values& c); // authoritative default 1.75
bool underlineLinks(const Values& c);
int linkUnderlineStyle(const Values& c);

/// Which pasteboard a remote session's PROGRAMS read: `[clipboard] remote_tools`.
///
/// Not the one Cmd-V pastes from — that is always the machine the user is
/// sitting at. This decides whether the shims for xclip, wl-paste and pbpaste
/// go on the session's PATH, so a program that shells out for a clipboard finds
/// one at all.
///
/// - Parameter c: The configuration to read.
/// - Returns: The setting, or its default.
[[nodiscard]] RemoteClipboardTools remoteClipboardTools(const Values& c);

/// Whether this machine offers its clipboard to a remote session's programs.
///
/// One fold for every call site — comparing the preference against Client —
/// because it decides whether every process on a remote host can read what the
/// user copied. Each spelling of one security question is a chance for the
/// first edit to diverge, and the frontends that ask it are the ones with no
/// test between them.
///
/// Returns Clipboard::Serving rather than a bool because that is the type the
/// answer is consumed as: RemoteSessionCore and WebTerminal both take it, and a
/// bool at the call site is one more place to invert.
///
/// - Parameter c: The preferences to read.
/// - Returns: Enabled only when the preference names the client's clipboard.
[[nodiscard]] Clipboard::Serving clipboardServing(const Values& c);

/// The host's verdict for a `tier` side effect driven from `origin` when no
/// grant exists: `[capabilities.<origin>]`, or `defaultDecision` where the file
/// is silent. Benign and Rendering answer their default whatever the file says,
/// because they have no key.
///
/// - Parameters:
///   - c: The configuration to read.
///   - origin: Who is driving the session.
///   - tier: The side effect's tier (`tierOf`).
/// - Returns: The configured verdict, or the per-origin default.
[[nodiscard]] CapabilityDecision capabilityDecision(const Values& c, CapabilityOrigin origin, CapabilityTier tier);

/// One origin's verdict for one capability, without building the whole table.
///
/// The configured cell for the capability's tier, or the capability's own
/// default, which refines its tier's; the same answer
/// ``capabilityDecisions`` gives for that pair.
///
/// - Parameters:
///   - c: The configuration to read.
///   - origin: Who is driving the session.
///   - capability: The side effect.
/// - Returns: The verdict.
[[nodiscard]] CapabilityDecision capabilityDecision(const Values& c, CapabilityOrigin origin, Capability capability);

/// Every origin's verdicts in one immutable table, built once per configuration
/// and handed to each session's capability policy.
///
/// - Parameter c: The configuration to read.
/// - Returns: The per-origin defaults with the file's overrides folded in.
[[nodiscard]] CapabilityDecisionTable capabilityDecisions(const Values& c);

/// The VT underline code a `[underline] link_style` index selects.
///
/// The key is an index — 0 solid, anything else dotted — and the renderers
/// take a VT code, which numbers its styles differently: dotted is 4, and 1
/// is the solid rule index 1 asks not to be. A frontend passing the index
/// through draws a solid rule wherever the setting asked for dots, and
/// against the portable backend that reads as the two agreeing.
///
/// - Parameter index: The setting, as `linkUnderlineStyle` returns it.
/// - Returns: The VT underline code to render with.
std::uint8_t linkUnderlineVTStyle(int index);

/// Builds the wheel and trackpad scroll policy from the [mouse] knobs.
///
/// One builder for every frontend, so two views over one config cannot
/// scroll at two speeds.
///
/// - Parameters:
///   - c: The configuration whose knobs apply.
///   - pointsPerLine: The view's cell height in device points; non-positive
///     falls back to one, keeping the precise-delta division defined.
/// - Returns: The policy the knobs describe.
[[nodiscard]] Screen::ScrollTuning::Policy wheelPolicy(const Values& c, double pointsPerLine);
bool linkColorSystemBlue(const Values& c);
bool linkStylePerKind(const Values& c);
bool smartDoubleClick(const Values& c);

// [text]
bool brightenBold(const Values& c);

// [term]

/// Whether to hand a spawned shell the user's locale in LANG.
///
/// - Parameter c: The configuration whose key applies.
/// - Returns: Whether the terminal sets the locale, defaulting to yes.
bool setsLocaleEnvironment(const Values& c);

bool beamTerminalFont(const Values& c);

// [tek] bright_pen — storage-tube bright spot at the writing beam.
bool tekBrightPen(const Values& c);
// [tek] glow_vectors — additive bloom on all Tek vectors (lit-CRT look).
bool tekGlowVectors(const Values& c);
// [tek] color_mode — 0 = terminal theme, 1 = custom beam/background. Clamped.
int tekColorMode(const Values& c);
// [tek] beam_color / background_color — custom-mode colors as packed 0xRRGGBB.
// Falls back to green-phosphor / near-black defaults when unset or unparseable.
unsigned tekBeamColor(const Values& c);
unsigned tekBackgroundColor(const Values& c);

// [appearance]
/// Returns the whole-window cell-background alpha, 0–1.
///
/// The key's value when set, then the theme's opacity, else opaque.
/// Blur and vibrancy only take effect below full opacity.
///
/// - Parameter c: The configuration to read.
/// - Returns: The alpha the window's cell backgrounds carry.
double backgroundOpacity(const Values& c);
/// Returns the extra alpha for cells carrying the default background alone.
///
/// 0–1; 1 means they follow `background_opacity` like every other cell. For a
/// translucent window only — the caller decides that, blur included: against
/// an opaque backing the compositor keeps the premultiplied colour and drops
/// the alpha, so a sub-unity value there darkens the theme instead of
/// revealing the desktop.
///
/// - Parameter c: The configuration to read.
/// - Returns: The key's value, or the default when the file leaves it unset.
double defaultBackgroundOpacity(const Values& c);
// Explicit radius, theme radius, or legacy switch when set; otherwise the default.
double blurRadius(const Values& c);
/// Resolves the requested text vibrancy from configuration and theme defaults.
///
/// - Parameter c: The configuration whose explicit setting takes precedence.
/// - Returns: Whether text requests vibrancy, before accessibility and blur policy.
bool textVibrancy(const Values& c);

/// Resolves perceptual text contrast independently of transparency and blur.
///
/// - Parameter c: The configuration to read.
/// - Returns: The explicit override, theme choice, or enabled default.
[[nodiscard]] bool textContrast(const Values& c);
/// Resolves the requested cell-background vibrancy from configuration and theme defaults.
///
/// - Parameter c: The configuration whose explicit setting takes precedence.
/// - Returns: Whether cell backgrounds request vibrancy, before accessibility and blur policy.
bool backgroundVibrancy(const Values& c);
// Path to the background image painted behind the grid; empty when unset.
std::string backgroundImage(const Values& c);
// Image alpha (0..1) the background image is multiplied by when composited.
double backgroundImageOpacity(const Values& c);
// How the background image is sized onto the window (fill/fit/stretch/tile/center).
BackgroundImageMode backgroundImageMode(const Values& c);
bool squareBottomCorners(const Values& c);
/// Returns the chromeless content's top inset in points.
///
/// Explicit `titleless_padding_top` wins; an unset value follows effective
/// `padding_bottom` for vertical symmetry. The result is clamped to [0, 64].
///
/// - Parameter c: The configuration whose inset is resolved.
/// - Returns: The top inset for a window without a title bar.
double titlelessPaddingTop(const Values& c);
TitlebarStyle titlebarStyle(const Values& c);

BeamSurface beamSurface(const Values& c);
bool paneTitleBars(const Values& c);
/// Whether host output is watched for a ZMODEM handshake.
///
/// - Parameter c: The configuration to read.
/// - Returns: Whether the watch is armed for a new session.
bool shouldWatchForZModem(const Values& c);
bool blinkAnimation(const Values& c); // true ⇒ fade; false ⇒ hard on/off
bool snapWindowResize(const Values& c);
// macOS: chromeless window — hide title bar + traffic lights. Default off.
bool hideTitleBar(const Values& c);
// Per-profile variant: the profile's hide_title_bar wins when set, else the global default.
bool hideTitleBar(const Values& c, const Profile& p);
bool columnModeResizesWindow(const Values& c);
/// Whether a program's `CSI 10 t` enters the system's own full screen.
///
/// The native transition changes Space and animates; the alternative fills the
/// screen's visible frame in place, synchronously, leaving the window
/// resizable so a geometry request behind it lands. One reading of the word,
/// because a frontend comparing the string itself spells the default a second
/// time and the two drift the day it moves.
///
/// - Parameter c: The configuration to read.
/// - Returns: Whether `[window] program_fullscreen` asks for native.
bool programFullscreenUsesNative(const Values& c);
bool colorSpaceP3(const Values& c);
/// Returns the colorspace decoded image pixels are expressed in.
///
/// The same setting that gives a drawable its colorspace, so a decode into this
/// space and the sampler that reads it agree by construction. Shared because
/// every frontend makes this decision identically, and an unmanaged decode is
/// the defect it exists to prevent.
///
/// - Parameter c: The configuration to read.
/// - Returns: The colorspace images are presented in.
Ubiquitty::Image::PresentationColorSpace imagePresentationColorSpace(const Values& c);
bool followSystemAppearance(const Values& c);
/// Returns the colour-vision deficiency the palette should be adapted for.
///
/// - Parameter c: The parsed configuration.
/// - Returns: The `[appearance] colorblind_mode` token resolved to a mode, or
///   `ColorblindMode::None` when it is absent or unrecognised.
Accessibility::ColorblindMode colorblindMode(const Values& c);
// Chrome font multiplier (tabs/HUD/settings), clamped to the schema range.
double uiFontScale(const Values& c);
/// How strongly a tag's colour is drawn in the title bar and on tabs.
///
/// The opacity a frontend draws a tag's fill, dot or icon at, over whatever
/// the title row or tab shows behind it; a title-bar wash from a loud tag is
/// not dimmed by it.
///
/// - Parameter c: The parsed configuration.
/// - Returns: `[appearance] tag_color_opacity`, clamped to the schema range,
///   or the default when it is absent.
double tagColorOpacity(const Values& c);

// [cursor]
CursorShape cursorShape(const Values& c);
CursorMotionMode cursorMotion(const Values& c);
bool cursorBlink(const Values& c);
int cursorLinger(const Values& c);
int cursorBlinkRate(const Values& c);
int cursorAlpha(const Values& c);
/// How long the cursor takes to reach its target cell, in milliseconds.
///
/// Paired with ``cursorMotion``: a caller that reads one without the other has
/// to guard two optionals whose presence is independent, and the site that did
/// dereferenced both under an `||`.
[[nodiscard]] int cursorMotionDuration(const Values& c);
/// The frame cap for continuous output, or 0 for the panel's own rate.
[[nodiscard]] int maxFPS(const Values& c);
bool cursorInvertText(const Values& c);

// [bell]
bool bellAudible(const Values& c);
/// Whether the DECPS synth may make any sound at all.
///
/// Turning it off silences what is already playing, not only what starts next:
/// a person who reaches for it wants the room quiet now. A program learns the
/// state through the sound-status report and can show something visible
/// instead of chiming into silence.
///
/// - Parameters:
///   - c: The configuration to read.
/// - Returns: Whether sound is on.
bool soundAudible(const Values& c);

/// The master level every DECPS request is mixed at.
///
/// Applied to the mix rather than to each request, so it reaches what is
/// already sounding. `Schema.h` carries the range the parser clamps to.
///
/// - Parameters:
///   - c: The configuration to read.
/// - Returns: The level, 0 silent to 1 full.
double soundVolume(const Values& c);
bool bellVisual(const Values& c);
double bellVisualIntensity(const Values& c);

/// The bell cue the two switches name, as the model carries it.
///
/// `Terminal::bellMode` is one enum over the pair, and the pair is two
/// independent settings, so somebody has to fold them. Here, because a
/// frontend folding them keeps its own answer to what an unset switch means:
/// a frontend reading `audible ? true : ...` beside Core's
/// `Defaults::bellAudible` could give a reader who never touched the setting
/// a different bell on a different frontend.
///
/// - Parameter c: The config fold.
/// - Returns: Off, Audible, Visual or Both.
Ubiquitty::BellMode bellMode(const Values& c);

/// The input encoding the file names, as the decoder takes it.
///
/// The value is stored as a name and consumed as an enum, so somebody has to
/// resolve it. Here, because a frontend that resolved it itself could leave a
/// reader's `[terminal] encoding` unapplied, so single-byte text would decode
/// as UTF-8 whatever the file said.
///
/// An unset, empty or unrecognised name answers UTF-8 rather than failing,
/// which is the encoding a terminal decodes in when nothing says otherwise.
///
/// - Parameter c: The config fold.
/// - Returns: The encoding, UTF-8 when the file names none this build knows.
Ubiquitty::Unicode::Encoding inputEncoding(const Values& c);

// [selection]
bool copyOnSelect(const Values& c);
/// The chord that makes a pointer drag rectangular, as the file spells it.
///
/// A `"+"`-joined token list — `"opt"`, `"shift+opt"`, `"cmd+opt"`,
/// `"ctrl+opt"` — parsed by `Input::chordFromSpec` and matched for exactly the
/// modifiers it names, so it cannot also read as the Shift bypass that reaches
/// a flowing selection through an application holding the mouse. A spec naming
/// nothing switches the gesture off.
///
/// - Parameter c: The config fold.
/// - Returns: The spec, `"opt"` when the file names none.
std::string blockSelectModifier(const Values& c);
/// Returns the modifier token that overrides application mouse reporting.
///
/// - Parameter c: The stored preferences, including absent values.
/// - Returns: The configured value or its default.
std::string selectModifier(const Values& c);
/// Returns where the selection's colour is drawn.
///
/// - Parameter values: The configuration to read.
/// - Returns: The configured style, or its default.
Rendering::SelectionStyle selectionStyle(const Values& values);
/// Returns the executable names excluded from running-process confirmation.
///
/// - Parameter c: The stored preferences, including absent values.
/// - Returns: The configured value or its default.
std::string closeIgnoredProcesses(const Values& c);
/// Returns the close-confirmation policy token.
///
/// - Parameter c: The stored preferences, including absent values.
/// - Returns: The configured value or its default.
std::string closeConfirmation(const Values& c);
bool linksOverrideReporting(const Values& c);
bool osc8ClickOpens(const Values& c);
bool restrictSelectionToText(const Values& c);

// [paste]
bool confirmMultilinePaste(const Values& c);
int confirmMultilinePasteThreshold(const Values& c); // lines; confirm when count > this

// [mouse]
double wheelSpeed(const Values& c);
int wheelLines(const Values& c);
double trackpadSpeed(const Values& c);
/// Resolves whether the frontend enables elastic scrollback.
///
/// - Parameter c: The configuration to resolve.
/// - Returns: The configured choice, or the default when unset.
bool scrollBounce(const Values& c);

// [tabs]
TabShortcutHints tabShortcutHints(const Values& c);

/// The `[link_handlers]` command the reader configured for a scheme.
///
/// Matched case-insensitively, first entry in file order winning, because a
/// scheme comparison is case-insensitive and a reader who wrote a key twice
/// meant the one they wrote first.
///
/// Only a scheme the reader explicitly listed has a command: there is no
/// implicit mapping, so a link cannot reach a program nobody named.
///
/// - Parameters:
///   - c: The configuration.
///   - scheme: The URI's scheme, in whatever case the link spelled it.
/// - Returns: The command template, or empty when none is configured.
std::string linkHandlerCommand(const Values& c, std::string_view scheme);

/// The command the reader configured for opening a file at a line.
///
/// `[links] line_handler`, a template `LinkRun::lineHandlerArgv` turns into
/// an argv. Empty when none is configured, and then a file link that names a
/// line opens the way any file link does.
///
/// - Parameter c: The configuration.
/// - Returns: The command template, or empty.
std::string linkLineHandler(const Values& c);

/// The family that answers a symbol code point the primary face lacks.
///
/// `font.symbol_fallback` when set, else `font.fallback`, else the bundled icon face.
/// One fold, so a frontend seeding a renderer's symbol face cannot honour one
/// key and ignore the other.
///
/// Defined here rather than in the accessor TU because the arms that need it
/// are cut differently: the agent trims `PreferencesAccessors.cpp` and keeps
/// the font-route builder, the macOS app does the reverse, and a fold in
/// either TU is an undefined symbol on the arm that trimmed it.
///
/// - Parameter c: The configuration whose keys apply.
/// - Returns: The configured family, or the bundled symbol family.
[[nodiscard]] inline std::string symbolFallbackFamily(const Values& c)
{
    std::string family = c.symbolFallbackFont.value_or(c.fallbackFont.value_or(std::string()));
    return family.empty() ? std::string(Rendering::Font::FontFallbackPolicy::defaultSymbolFallbackFamily) : family;
}

/// The launch grid's columns: the `[window] initial_columns` pin when the
/// user set one (> 0), else the compile-time default. One fold for every
/// frontend, so a pin honoured by one window shell and ignored by another
/// cannot happen.
///
/// - Parameter c: The configuration whose pin applies.
/// - Returns: The columns a new window opens with.
int launchColumns(const Values& c);

/// The launch grid's rows, resolved the way ``launchColumns`` is.
///
/// - Parameter c: The configuration whose pin applies.
/// - Returns: The rows a new window opens with.
int launchRows(const Values& c);

/// A window shell's launch facts: the grid it opens at, the shape of its
/// title row, the snap of its live resize and the host it opens on.
///
/// Facts of the window rather than of the frame: everything the renderer draws
/// is ``Rendering::RenderConfiguration``'s, so this carries only what a shell
/// decides before there is a frame to draw. Owned values and `ptrdiff_t`
/// because it crosses language seams by value.
struct LaunchSettings {
    /// The launch grid's columns — the pin when set, else the default.
    std::ptrdiff_t columns = 0;
    /// The launch grid's rows, resolved the same way.
    std::ptrdiff_t rows = 0;
    /// Whether both counts above came from a config pin rather than the
    /// default. A pinned grid ignores a remembered window frame: the frame
    /// is a by-product of a drag and names no grid.
    bool isGridPinned = false;
    /// Whether the window hides its title bar and traffic lights.
    bool shouldHideTitleBar = false;
    /// Whether the title row is painted the theme background or left native.
    TitlebarStyle titlebarStyle = TitlebarStyle::Themed;
    /// Whether a live resize quantises the window to whole cells.
    bool shouldSnapWindowResize = true;
    /// The host a profile's window opens a remote shell on, as the profile
    /// spelled it; empty opens the local login shell.
    ///
    /// Carried unvalidated: what the value may say is
    /// `Network::buildStartupHostArguments`'s rule, applied at the spawn
    /// where the tool paths are known, so a refusal can name the profile.
    std::string startupHost;
};

/// Folds a configuration into ``LaunchSettings``.
///
/// - Parameter c: The configuration whose knobs apply.
/// - Returns: The resolved launch settings.
[[nodiscard]] LaunchSettings launchSettings(const Values& c);

/// The launch fold with one profile's set fields laid over it.
///
/// A profile is a scoped override of the same values the fold carries, so it
/// resolves into the fold rather than beside it: the frontend builds one
/// window from one struct whether a profile named it or not, and a field the
/// profile leaves unset keeps the configuration's answer. This is the window
/// rung of the per-scope styling ladder — the same overlay a tab or a pane
/// scope will apply to narrower scopes.
///
/// - Parameters:
///   - c: The resolved configuration.
///   - profile: The profile to lay over it.
/// - Returns: The overlaid settings.
[[nodiscard]] LaunchSettings launchSettings(const Values& c, const Profile& profile);

/// The fold over the live configuration, for a caller with no `Values` in
/// hand — the spelling that keeps the `Values` type on this side of a
/// language seam entirely.
///
/// - Returns: The resolved launch settings.
[[nodiscard]] LaunchSettings launchSettings();

/// Whether the system asks for translucency to be given up.
///
/// Not a preference: a machine-wide accessibility switch (Reduce Transparency
/// on macOS) that only a GUI frontend can read, reported in so the rule below
/// is written once rather than once per shell.
enum class SystemTransparency : std::uint8_t {
    /// The system allows a translucent, blurred ground.
    Allowed = 0,
    /// Reduce Transparency is on: effects fall back to a plain opaque window.
    Reduced = 1,
};

/// The ground a reader asked for, before the system has its say.
///
/// Its own two fields rather than a whole fold, because a shell holding a
/// window's live overrides — a value the reader moves on screen applies to the
/// window in front of them, not to the file — has these numbers and not a
/// configuration to put them in.
struct BackgroundRequest {
    /// The whole-window cell-background alpha, 0-1; 1 is opaque.
    double backgroundOpacity = 1;
    /// The behind-window blur radius in points; 0 is no blur.
    double blurRadius = 0;
    /// The extra alpha asked for cells carrying the default background alone.
    double defaultBackgroundOpacity = 1;
    /// Whether low-contrast text is strengthened over an active blur.
    bool shouldEnhanceTextOnBlur = false;
    bool usesBackgroundVibrancy = false;
};

/// What a window's ground realizes.
///
/// The two answers travel together because they are one decision: a blur is
/// in effect only on a translucent window. A shell that derives the two apart
/// can hang a blur material under an opaque window's panes, where it shows in
/// the gap before the first frame.
struct BackgroundEffects {
    /// Whether the window and its layers stop declaring themselves opaque.
    bool isTranslucent = false;
    /// Whether the blur backdrop is shown behind the grid.
    bool isBlurred = false;
    double blurRadius = 0;
    /// The alpha the renderer draws cell backgrounds with: the asked opacity
    /// on a translucent window, 1 on an opaque one — against an opaque
    /// backing the compositor drops the alpha and keeps the darkened colour,
    /// so a sub-unity tint there blackens the theme rather than revealing
    /// the desktop.
    double rendererOpacity = 1;
    /// The extra alpha for default-background cells, on the same rule.
    double defaultBackgroundOpacity = 1;
    /// Whether low-contrast text is strengthened over an active blur.
    bool shouldEnhanceTextOnBlur = false;
    bool usesBackgroundVibrancy = false;
};

/// Resolves a window's ground from what was asked for and the system switch.
///
/// Reduce Transparency wins over both keys: the system's own materials go
/// flat under it, and a terminal that kept its blur would be the one
/// see-through window on a desktop the reader asked to be opaque.
///
/// - Parameters:
///   - asked: The opacity and blur radius in force for this window.
///   - system: What the machine's accessibility switch says.
/// - Returns: The ground to realize.
[[nodiscard]] BackgroundEffects backgroundEffects(const BackgroundRequest& asked, SystemTransparency system);

/// Resolves a window's ground from the configuration, the theme it shows and
/// the system switch: a key the configuration sets wins, then the theme's
/// appearance, then the key's default.
///
/// - Parameters:
///   - c: The configuration, a window's profile laid over it.
///   - system: What the machine's accessibility switch says.
///   - appearance: The appearance of the theme the window shows.
/// - Returns: The ground to realize.
[[nodiscard]] BackgroundEffects backgroundEffects(const Values& c, SystemTransparency system, const Themes::ThemeAppearance& appearance);

/// Resolves perceptual text contrast for a window showing a theme, by the same
/// precedence as ``backgroundEffects``.
///
/// - Parameters:
///   - c: The configuration, a window's profile laid over it.
///   - appearance: The appearance of the theme the window shows.
/// - Returns: Whether ordinary terminal text receives contrast correction.
[[nodiscard]] bool textContrast(const Values& c, const Themes::ThemeAppearance& appearance);

// [window] — the drag-from-any-cell modifier as a raw "+"-joined token spec
// (e.g. "control+command"); "" / "none" disables the gesture. The frontend
// parses the tokens into its native modifier mask.
std::string windowDragModifier(const Values& c);

/// Which Option keys act as the terminal Meta (Alt) modifier.
///
/// - Parameter c: The parsed configuration.
/// - Returns: `[keys] option_as_alt`, or `OptionAsAlternate::None` when it is
///   absent.
[[nodiscard]] OptionAsAlternate optionAsAlternate(const Values& c);

// [quake] — drop-down terminal. enabled gates the whole feature (and the
// global hotkey registration); the rest only matter when enabled.
bool quakeEnabled(const Values& c);
std::string quakeHotkey(const Values& c); // toggle spec, e.g. "control+grave"
double quakeHeight(const Values& c); // screen-height fraction, clamped 0.2..1.0
bool quakeAnimate(const Values& c);

// [dock]
bool dockBounce(const Values& c);
bool dockBadge(const Values& c);

// [security]
bool secureKeyboardEntry(const Values& c);
/// Whether the password-prompt heuristic runs: `[security] password_detect`.
///
/// - Parameter c: The config fold.
/// - Returns: The setting, or its default.
bool passwordDetect(const Values& c);
/// Whether Secure Keyboard Entry is held while a pane is at a password
/// prompt: `[security] secure_keyboard_entry_at_password_prompts`.
///
/// - Parameter c: The config fold.
/// - Returns: The setting, or its default.
bool secureKeyboardEntryAtPasswordPrompts(const Values& c);

// [control]
// SECURITY GATE: true only when control.enable = true; the listener must not bind until then.
bool controlEnabled(const Values& c);
/// Reports whether a control-socket request may stand on a `confirm` verdict.
///
/// The socket's twin of the CLI's `--yes`: with it on, a `confirm` from the
/// agent policy counts as approved and is audited as such. Off by default.
///
/// - Parameters:
///   - c: The configuration in force.
/// - Returns: Whether a `confirm` verdict counts as approved.
bool controlApprove(const Values& c);

// [experimental]
bool sessionPersistence(const Values& c);

// [notifications]
bool notifications(const Values& c);
/// Whether an inbound file transfer starts without asking.
///
/// Here rather than read off the field, because a frontend reading the raw
/// optional has to know the default too — and Swift cannot call `value_or` on
/// an imported `std::optional` at all, so the seam is the only way across.
[[nodiscard]] bool transferAutoReceive(const Values& c);
bool notificationSound(const Values& c);

// [triggers]
// SECURITY GATE: true only when allow_run_command = true; run_command must not spawn until then.
bool allowRunCommand(const Values& c);
// SECURITY GATE: true only when allow_webhook = true; the webhook must not send until then.
bool allowWebhook(const Values& c);

// [scripting]
// SECURITY GATE: true only when [scripting] enable = true; config_script must not run until then.
bool scriptingEnable(const Values& c);

/// The program the `[scripting]` hook runs, or empty when there is none to run.
///
/// Empty unless `enable = true` — the gate is closed by default — and a
/// `config_script` is named, so a frontend that runs whatever this answers
/// cannot run a script the reader never opened the gate for.
///
/// - Parameter c: The configuration as loaded from the file.
/// - Returns: The executable's path, or empty.
[[nodiscard]] std::string scriptingHookProgram(const Values& c);

// [debug]
[[nodiscard]] bool debugShowBuildHash(const Values& c);
[[nodiscard]] bool debugShowBuildHash();

// [updates]
/// Whether the app's updater checks for a new version on its own schedule:
/// `[updates] check_automatically`.
///
/// - Parameter c: The configuration in force.
/// - Returns: The setting, or true when it is absent.
[[nodiscard]] bool shouldCheckForUpdatesAutomatically(const Values& c);

// [remote]
bool autoDeployHostOnRemoteConnect(const Values& c);

/// Whether authenticated remote history may be encrypted on disk.
///
/// - Parameter c: The current configuration.
/// - Returns: The explicit opt-in, false when absent.
[[nodiscard]] bool shouldPersistRemoteHistory(const Values& c);

/// Resolves the encrypted-history opt-in against the live configuration.
///
/// - Returns: Whether encrypted remote history persistence is enabled.
[[nodiscard]] bool shouldPersistRemoteHistory();

/// Resolves the local presentation policy for a pane whose size belongs to another attachment.
/// - Parameter values: The merged configuration.
/// - Returns: Letterbox by default, or the requested Fit or Pan policy.
[[nodiscard]] Rendering::RemoteViewportPolicy nonOwnerPresentation(const Values& values);

/// The same answer, resolved against the live configuration.
///
/// The zero-argument form exists for the same reason ``launchSettings``'s
/// does: a frontend asking a policy question should not have to hold a
/// `Values` to ask it.
///
/// - Returns: Whether an automatic host install is allowed.
[[nodiscard]] bool autoDeployHostOnRemoteConnect();
// [terminal]
int scrollbackLines(const Values& c);

/// How many cells an East_Asian_Width=Ambiguous code point occupies.
///
/// A frontend that spells this default itself, as `value_or(1)`, agrees with
/// `value_or(Defaults::ambiguousWidth)` only until the default changes, and
/// a width two frontends disagree about places every later cell on the row
/// differently.
///
/// The parse clamps the key to 1..2 and `Unicode::Width::ambiguousWidthFromCells`
/// maps anything else to narrow, so this resolves without a third clamp of its own.
///
/// - Parameter c: The config fold.
/// - Returns: 1 for narrow, 2 for wide.
[[nodiscard]] int ambiguousWidth(const Values& c);

// [title] — the effective composition for one title surface: which parts
// (directory, its name-vs-path spelling, app-set title, process chain, the
// chain leaf's argv, dimensions, a program's status label) the woven title shows.
struct TitleComposition {
    bool showsDirectory;
    bool showsDirectoryFullPath; // true = full tilde path, false = folder name
    bool showsAppTitle;
    bool showsProcess;
    bool showsProcessArguments; // the chain leaf's full argv ("Arguments" sub-toggle)
    bool showsShellCommand; // the session-leader shell's name
    bool showsTTYName; // the replica tty basename ("ttys003")
    bool showsProfileName; // the applied profile's name
    bool showsDimensions;
    bool showsProgramStatus; // the label a program gave its status colour (OSC 21337)
    // The static base Title text (leads the weave); empty = absent. Borrows Values.
    std::string_view baseTitle;
    // The programs a title leaves the directory out for ([title]
    // directory_hidden_for), on every surface. Borrows Values.
    std::string_view directoryHiddenFor {};
    bool hidesDirectoryInFullScreen = false; // [title] directory_hidden_in_full_screen
};

// The resolved window composition (title.window_*). The defaults are the
// fallback column of the table in TitleComposition.h.
TitleComposition windowTitleComposition(const Values& c);

// The resolved tab composition (title.tab_*); defaults as above.
TitleComposition tabTitleComposition(const Values& c);

// The resolved pane composition (title.pane_*): an unset pane flag inherits the
// RESOLVED tab flag except where its row says otherwise (dimensions: a pane
// label never showed them, and inheriting tab_dimensions=true would change it).
TitleComposition paneTitleComposition(const Values& c);

/// The window composition over the live configuration, for a caller with no
/// `Values` in hand — the spelling that keeps the `Values` type on this side of
/// a language seam entirely.
///
/// - Returns: The resolved window title composition.
[[nodiscard]] TitleComposition windowTitleComposition();

/// The tab composition over the live configuration.
///
/// - Returns: The resolved tab title composition.
[[nodiscard]] TitleComposition tabTitleComposition();

/// The pane composition over the live configuration.
///
/// - Returns: The resolved pane title composition.
[[nodiscard]] TitleComposition paneTitleComposition();

} // namespace Effective
} // namespace Ubiquitty::Preferences
