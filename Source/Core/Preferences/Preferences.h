// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Accessibility/ColorblindMode.h"
#include "Core/Themes/StatusColors.h"
#include "Core/Themes/ThemeAppearance.h"
#include "PAL/Capability.h"
#include "PAL/TerminalIdentity.h"

// Declarative configuration loaded from ~/.config/ubiquitty/config.toml.
// writeKeyValue round-trips back to the file preserving comments and formatting.

#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Input/ModifierPolicy.h"
#include "Core/Preferences/Profile.h"
#include "Core/Rendering/BackgroundImageLayout.h"
#include "Core/Rendering/CursorMotion.h"

namespace Ubiquitty::Preferences {

/// One key the parser read and did not apply.
///
/// Permissive parsing skips an unrecognized key rather than failing, so a
/// config written for a newer build loads on an older one. A typo takes the
/// same path: the key is skipped and the field keeps its default. The two are
/// indistinguishable from the parse's side, so it records the skip and leaves
/// the load succeeding.
///
/// This is data, not a verdict. Core does not know whether a skipped key is a
/// typo worth reporting or a setting from a build the user also runs, so a
/// frontend decides whether it warrants a log line, a badge in Values, or
/// nothing.
/// Why a key the parse read did not land.
///
/// The two are a different sentence to the person holding the file: one says
/// the name is wrong, the other says the name is right and the value is not.
/// Collapsing them sends a user looking for a typo in a key that exists.
///
/// `MalformedLine` is the third sentence: the line is not TOML this reader
/// understands at all — an unclosed header, an assignment with no `=`, a
/// value no literal grammar accepts — so there is no key to name, and the
/// record carries the line's own text instead.
enum class SkipReason : std::uint8_t {
    UnknownKey,
    WrongValueKind,
    MalformedLine,
};

/// A key the parse read and did not apply, with where it was written.
///
/// Carries enough to point a person at the line rather than to tell them
/// something is wrong somewhere: the file's own coordinates, and which of the
/// two refusals it was.
struct SkippedKey {
    /// 1-based line in the source file, for a message that can point at it.
    int line = 0;
    /// The `[section]` in force, empty at top level.
    std::string section;
    /// The key as written, or for `MalformedLine` the trimmed line itself,
    /// cut at ``malformedLineQuoteLimit`` bytes.
    std::string key;
    /// Whether the name or the value is what the parse refused.
    SkipReason reason = SkipReason::UnknownKey;
    /// For `WrongValueKind`, what the key does take — "a string", "a number",
    /// "true or false". Empty for `UnknownKey`, which has nothing to suggest.
    std::string expectedValueKind;
};

/// The most of a malformed line a ``SkippedKey`` quotes back.
///
/// The document is untrusted text, and a single line of it can be the whole
/// file; a diagnostic needs enough to recognise the line, not a copy of it.
inline constexpr std::size_t malformedLineQuoteLimit = 120;

/// One colour slot parsed from a `#rrggbb[aa]` hex literal.
///
/// `set` distinguishes a colour the file supplied from one it left out, which a
/// colour value alone cannot: black and absent are the same four numbers. The
/// apply step falls back to a caller-chosen default when it is false.
struct ColorEntry {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;
    bool set = false;
};

/// One user-defined theme, parsed from a `[[themes]]` array-of-tables entry.
///
/// `name` is mandatory. A slot the entry omits stays unset, and the registrar
/// substitutes the matching slot from the active built-in theme, so a theme may
/// restate one colour without restating nineteen.
struct ThemeEntry {
    std::string name;
    std::optional<bool> isDark;
    Themes::ThemeAppearance appearance;
    ColorEntry ansi[16];
    ColorEntry foreground;
    ColorEntry background;
    ColorEntry cursor;
    // `status_failed`, `status_blocked`, `status_attention`, `status_done`,
    // `status_working`, `status_neutral`, `status_progress`: the colours a status is drawn in,
    // indexed by Themes::StatusColorSlot. An unset one is derived.
    ColorEntry status[Themes::statusColorSlotCount];
};

/// One raw `[keys]` entry: a key spec mapped to an action.
///
/// Both halves stay strings here — `"cmd+shift+k"` or the chord
/// `"ctrl+a ctrl+x"`, against `"copy"` or `"send:\\x01"`. Parsing them into the
/// typed table is Input/Keybindings' work, so a spec this build does not
/// understand survives a round-trip through the file rather than being dropped
/// by the reader.
struct KeyBindingEntry {
    std::string spec;
    std::string action;
};

/// One raw `[link_handlers]` entry: a URL scheme mapped to a command template.
///
/// `%s` and `%u` in the template are replaced by the link URL as ONE argument.
/// The URL is attacker-controllable output from the terminal, so a caller must
/// never re-split the result and never hand it to a shell. A scheme with no
/// entry keeps the default OS-open path.
struct LinkHandlerEntry {
    std::string scheme;
    std::string command;
};

/// One `[[tags]]` entry: a coloured marker, its optional title, and what it
/// matches.
///
/// A tag marks a window at a glance — a red `production` against a host whose
/// name says so. `color` is either one of the Finder tag names (`red`,
/// `orange`, `yellow`, `green`, `blue`, `purple`, `gray`) or a `#rrggbb`
/// literal. `name` is optional: a rule with a colour and no name is drawn as a
/// dot of that colour (`Session::isValidTagRule`). `matchHost` and
/// `matchProfile` are globs where `*` stands for any run of characters, or
/// several separated by commas, any of which matches (`Session::tagPatternList`;
/// the file also takes them as an array); an empty one places no constraint, and an entry with neither matches nothing
/// on its own — it is then only a name a window can carry manually or be
/// given from the terminal.
///
/// `Values::tags` holds the entries in file order, and that order is the
/// order a window draws the tags rules give it: the first rule's marker
/// comes first in the title bar, on the tab and in a pane's header.
struct TagEntry {
    std::string name;
    std::string color;
    std::string matchHost;
    std::string matchProfile;
    /// `quiet` (default) or `loud`. A loud tag also tints the whole title bar:
    /// the practice terminals converged on for production is a change big
    /// enough to be seen before anything is read, not a marker beside the
    /// title. Reserved for the few tags that carry real danger — every window
    /// shouting is the same as none of them shouting.
    std::string prominence;
    /// A Phosphor icon name (`database`, `cloud`, `flame`), drawn in the tag's
    /// colour where a frontend would otherwise draw a dot or the host's globe.
    /// Empty for none; a name the frontend's catalog does not carry draws as
    /// if it were empty.
    std::string icon {};
    /// `fill` (default) or `regular`: which of the icon's two weights it is
    /// drawn in.
    std::string iconWeight {};
    /// `show_name` (default true): whether the marker draws the name. A
    /// marker that does not is the icon alone, or without one a dot of the
    /// colour. The name is still the tag's: it is spoken, shown in tooltips,
    /// and matched against names set by hand or from the terminal.
    bool showsName = true;
    /// `show_in_title_bar` (default true): whether the window's title bar
    /// draws the tag, its host chip and a loud tag's wash included.
    bool showsInTitleBar = true;
    /// `show_in_tabs` (default true): whether the window's tab draws the tag.
    bool showsInTabs = true;
    /// `show_in_pane_headers` (default true): whether the header above a pane
    /// draws the tag.
    bool showsInPaneHeaders = true;
};

/// One raw `[triggers]` entry: a regex mapped to an action spec.
///
/// The regex is the table key and the spec is its value, written `<action>` or
/// `<action>:<arg>` over notify, copy, set_title, highlight and run_command. A
/// frontend compiles these into a `Triggers::Engine`; a file with no
/// `[triggers]` block fires nothing.
struct TriggerEntry {
    std::string pattern;
    std::string action;
};

/// One `[macros]` entry: a macro name mapped to its body.
///
/// The body is literal text sent to the focused session, taking the same
/// C-style escapes as a `[keys]` `send:` action — `\n \r \t \e \xNN \\`. A
/// `[keys]` entry binds it by `macro:<name>`.
struct MacroEntry {
    std::string name;
    std::string body;
};

/// One raw `[characterClass]` entry: a token-class name mapped to its regex.
///
/// For example `path = "[A-Za-z0-9_./-]+"`. Double-click selection consults
/// these in file order, so an earlier entry wins an overlap; a file with no
/// `[characterClass]` block keeps the default word-character behaviour.
struct CharacterClassEntry {
    std::string name;
    std::string pattern;
};

/// One `[[font.language_fallback]]` entry: which family covers a script.
///
/// `script` and `language` select when the rule applies, `mode` says how the
/// family joins the fallback order, and `family` names it. All four are the
/// strings the file carried; resolving them against installed fonts happens in
/// the font route, which is the only place that knows what a host has.
struct LanguageFallbackEntry {
    std::string script;
    std::string language;
    std::string mode;
    std::string family;
};

/// One `[[font.range_map]]` entry: a code-point range routed to a family.
///
/// The range is inclusive at both ends. `range` keeps the spelling the file
/// used, so a diagnostic can quote what a person wrote rather than the two
/// numbers it parsed to, and `mode` says how the family joins the fallback
/// order.
struct RangeMapEntry {
    std::uint32_t firstCodePoint = 0;
    std::uint32_t lastCodePoint = 0;
    std::string range;
    std::string mode;
    std::string family;
    std::optional<bool> shouldMatchSize {};
};

/// One [platform] override block (Docs/platform-config-and-sync.md): the
/// fields a `[mac]`/`[linux]` section sets, plus its `[mac.keys]` overrides.
/// `profile` carries only named fields (unset don't clobber on merge); `present`
/// is true once any platform key/section was seen so the apply step can skip an
/// empty slot.
struct PlatformOverride {
    Profile profile;
    std::vector<KeyBindingEntry> keyBindings;
    bool present = false;
};

/// Cursor shape, mirroring Terminal::cursorShapePref. The underlying values match
/// both that field and the View-menu item tags. Parsing an unrecognized
/// string yields std::nullopt so the apply step leaves the live shape alone.
enum class CursorShape : std::uint8_t {
    Block = 0,
    Underline = 1,
    Bar = 2,
};

// Cursor motion smoothing policy, DISTANCE-based. Off never glides; Short glides
// short moves / snaps long jumps; Long is the inverse; Always glides every move.
// Persisted under [cursor] motion as "off" | "short" | "long" | "always".
// Enum defined in Rendering/CursorMotion.h; aliased here for the Values surface.
using CursorMotionMode = Ubiquitty::Rendering::CursorMotionMode;

// Which Option keys act as the terminal Meta (Alt) modifier. Persisted under
// [keys] option_as_alt as "none" | "left" | "right" | "both". Enum defined in
// Input/ModifierPolicy.h, whose predicate consumes it; aliased here for the
// Values surface.
using OptionAsAlternate = Ubiquitty::Input::OptionAsAlternate;

/// Tab ⌘N shortcut-hint visibility. Persisted under [tabs] show_shortcuts as
/// "off" | "always" | "command"; OnCommand reveals the hint only while ⌘ is
/// held.
enum class TabShortcutHints : std::uint8_t {
    Off = 0,
    Always = 1,
    OnCommand = 2,
};

/// Window title-bar styling. Themed (default) paints the title row with the theme
/// background; Native leaves AppKit's system vibrant title bar untouched.
/// Persisted under [appearance] titlebar_style as "themed" | "native".
enum class TitlebarStyle : std::uint8_t {
    Themed = 0,
    Native = 1,
};

/// Where the command palette is drawn. Native hands it to the frontend's own UI
/// toolkit; InTerminal draws it into the cell grid, which every frontend can do
/// because the host composites it into the frame. Auto takes the native palette
/// where one exists and the in-grid one everywhere else.
///
/// Persisted under [appearance] beam_surface as "auto" | "native" | "in-terminal".
enum class BeamSurface : std::uint8_t {
    Auto = 0,
    Native = 1,
    InTerminal = 2,
};

/// Whether the frontend asking has a native palette to offer. An enum rather than
/// a bool because it is read at a call site that already passes a preference, and
/// two adjacent booleans is where the argument order stops being checkable.
enum class NativeBeam : std::uint8_t {
    Unavailable = 0,
    Available = 1,
};

/// Whether that frontend can draw a palette into the cell grid.
///
/// Not a constant, though it reads like one. An attached client composites chrome
/// into the frame it is given and can always do this; a frontend that renders
/// locally can only do it if its own shipping path composites chrome, and macOS's
/// does not — its frame is assembled by its own encode, and the Core capture
/// that takes a chrome pointer is reached only by the parity profiler.
///
/// Naming it is what stops "in-terminal" from being a setting that silently opens
/// nothing on the one frontend that has both surfaces.
enum class InGridBeam : std::uint8_t {
    Unavailable = 0,
    Available = 1,
};

/// Where the palette is actually drawn, once the preference and what the frontend
/// can offer have been reconciled. A SEPARATE type from BeamSurface, with no Auto:
/// resolving is exactly what removes that case, and returning the wider enum would
/// oblige every switch downstream to handle a value it can never receive.
enum class BeamPresentation : std::uint8_t {
    Native = 0,
    InTerminal = 1,
};

/// Color space the renderer composites in. Persisted under [appearance]
/// color_space as "srgb" | "display-p3". Values match the Values
/// segmented-control index (0 = sRGB, 1 = CellGrid P3).
enum class ColorSpace : std::uint8_t {
    SRGB = 0,
    DisplayP3 = 1,
};

// How [appearance] background_image is sized behind the grid. Persisted under
// background_image_mode as the lowercase token; values match the Values popup
// index. Fill (default) covers/crops; Fit letterboxes; Stretch ignores aspect;
// Tile repeats at native size; Center places once at native size, centered.
using BackgroundImageMode = Ubiquitty::Rendering::BackgroundImageMode;

/// What a program on the far side of a remote session sees when it reads "the
/// clipboard".
///
/// A host has no pasteboard to offer it: a headless box runs no display server,
/// so xclip and wl-paste have nothing to talk to and pbpaste is not installed.
/// Client puts shims for those tools early on the session's PATH and answers
/// them from the pasteboard of the machine the user is sitting at.
///
/// Off by default: a shim exposes that pasteboard to every process on the host
/// that runs xclip, which on a shared machine discloses what the user copied to
/// people who never asked for it.
///
/// Persisted under `[clipboard] remote_tools` as "off" | "client".
enum class RemoteClipboardTools : std::uint8_t {
    Off = 0,
    Client = 1,
};

/// One origin's overrides of the host's capability verdicts:
/// `[capabilities.local]`, `[capabilities.agent]` or `[capabilities.remote]`.
///
/// Each key names a configurable tier and holds "allow" | "prompt" | "deny".
/// Unset keeps the per-origin default (`defaultDecision` in PAL/Capability.h).
/// Benign and Rendering have no key: they are allowed for every origin, and a
/// file cannot spell anything else for them.
struct CapabilityOverrides {
    std::optional<CapabilityDecision> informational; // `informational` — CapabilityTier::LowRiskInfo
    std::optional<CapabilityDecision> hostEscaping; // `host_escaping` — CapabilityTier::HostEscaping
    std::optional<CapabilityDecision> readExfiltration; // `read_exfiltration` — CapabilityTier::ReadExfil
};

// Default [mouse] trackpad_speed: multiplier applied after precise point deltas
// are normalized to text rows. 1.0 = one row per row-height of finger travel.
inline constexpr double defaultTrackpadSpeed = 1.0;

// Default lines of scrollback history retained per session.
inline constexpr int defaultScrollbackLines = 10000;

/// Named specializations of the optional the fields carry, for Swift.
///
/// Swift's C++ interop cannot name an unspecialized class template, so
/// `std::optional<bool>` is unspellable from Swift and a field assignment from
/// that side fails to compile. A named alias is a specialization, which it can
/// spell — the same reason a span parameter needs one.
using OptionalBool = std::optional<bool>;
using OptionalInt = std::optional<int>;
using OptionalDouble = std::optional<double>;
using OptionalString = std::optional<std::string>;
using OptionalCursorMotionMode = std::optional<CursorMotionMode>;

/// One parsed set of preferences: every key the file can carry, flattened.
///
/// Each field arrives at a compile-time default and its `<field>Set` companion
/// says whether the file supplied the key. The pair is what lets a value be
/// merged rather than assigned — a profile, a platform override and a synced
/// peer each overlay this, and only a field the source actually named may
/// overwrite one below it. A default alone cannot carry that: it is
/// indistinguishable from a person configuring the same value on purpose.
struct Values {
    // [font]
    std::optional<std::string> fontFamily;
    // The face proportional runs (OSC 66 p=1, Docs/proportional-text.md) are
    // set in; empty takes the platform's UI face.
    std::optional<std::string> proportionalFontFamily;
    // Fallback font for codepoints the primary font lacks (Nerd Font icons,
    // Powerline, …). Empty uses STIX Two Math, with the bundled symbols as a fallback.
    std::optional<std::string> fallbackFont;
    std::optional<std::string> symbolFallbackFont;
    std::optional<bool> shouldMatchFallbackSize;
    std::optional<std::string> emojiFallbackFont;
    // CJK/Han fallback preference. Empty/auto follows the system cascade, while
    // zh-Hans/zh-Hant/ja/ko or a font family pins unified Han glyph forms.
    std::optional<std::string> cjkFallbackFont;
    std::vector<LanguageFallbackEntry> languageFallbacks;
    std::vector<RangeMapEntry> rangeMap;
    std::optional<double> fontSize; // points
    std::optional<double> proportionalFontSize; // points for proportional runs; unset or 0 takes one optical step over fontSize
    std::optional<double> baselineOffset; // pt — vertical glyph shift within the cell (+ = up)
    std::optional<bool> ligatures;
    // Comma-separated list of 4-char OpenType feature tags to ENABLE on top of
    // the ligature shaping (e.g. "zero,ss01,dlig"). Empty (the default) adds
    // nothing to the ligature shaping. `ssNN` selects a stylistic set.
    std::optional<std::string> fontFeatures;
    // Space/comma-separated `tag=N` variation-axis list (e.g. "slnt=-6
    // opsz=14"). A tag named here beats the axis a named setting derives —
    // `wght=450` overrides `font.weight` — the precedence CSS gives
    // font-variation-settings. Empty (the default) writes no axis.
    std::optional<std::string> fontVariations; // unset writes no axis
    std::optional<double> lineSpacingPoints; // extra pt, e.g. 2
    std::optional<double> characterSpacingPoints; // pt added to the cell advance, the horizontal twin of line_spacing
    std::optional<int> fontWeight; // unset leaves the face's own upright member
    std::optional<int> boldFontWeight; // unset leaves the face's own bold member
    std::optional<double> fontWidthPercent; // unset leaves the face's own width
    std::optional<bool> wideGlyphBoost; // optically scale full-width monochrome glyphs within their two-cell slot
    // Braille as a 2x4 dot lattice the renderer draws, rather than as
    // glyphs from the face. Braille is a drawing surface at least as often
    // as it is text — plots, sparklines and dithered images all use it —
    // and a face's braille carries bearings and its own dot pitch, so a
    // block of it seams where a picture needs a continuous grid. False
    // routes it to the font like any other character.
    std::optional<bool> brailleGrid;
    std::optional<std::string> brailleStyle;
    std::optional<double> wideGlyphScale; // multiplier for full-width monochrome glyph ink

    // [underline] — tuning layered on the font-derived underline defaults
    // the renderer computes from CoreText metrics.
    std::optional<double> underlineThickness; // x font default, 1.0
    std::optional<double> underlineOffset; // extra pt below baseline
    std::optional<bool> underlineSkipInk; // descenders break the rule
    std::optional<bool> underlineFollowsInk;
    std::optional<double> underlineSkipInkPad; // halo radius, pt
    std::optional<bool> underlineLinks; // OSC 8 link cells dotted-underlined
    std::optional<int> linkUnderlineStyle; // OSC 8 link underline: 0 = single, 1 = dotted
    std::optional<bool> linkColorSystemBlue; // force OSC 8 link text + underline to system blue
    std::optional<bool> linkStylePerKind; // OSC 8 kinds get distinct underlines (file:// dotted, mailto: dashed, url/other base);
                                          // off = all links use link_style
    std::optional<bool> smartDoubleClick; // double-click selects URL/path token, not a word

    // [text]
    std::optional<bool> brightenBold; // bold promotes foreground 0..7 → 8..15
    std::optional<bool> beamTerminalFont; // Beam renders in the terminal's font, not the system UI font

    // [tek]
    std::optional<bool> tekBrightPen; // storage-tube bright-pen spot at the writing beam
    std::optional<bool> tekGlowVectors; // additive bloom on all Tek vectors (lit-CRT look)
    std::optional<int> tekColorMode; // 0 = theme colors, 1 = custom beam/background below
    std::optional<std::string> tekBeamColor; // custom vector/beam color, hex (#rrggbb)
    std::optional<std::string> tekBackgroundColor; // custom storage-tube clear color, hex (#rrggbb)

    // [appearance]
    std::optional<std::string> themeName; // theme keyed by name; an index is never persisted
    std::optional<std::string> colorSpace; // srgb | display-p3
    std::optional<double> padding; // points; shorthand for all four edges
    std::optional<double> paddingLeft; // left inset, points; overrides padding
    std::optional<double> paddingRight; // right inset, points; overrides padding
    std::optional<double> paddingTop; // top inset, points; overrides padding. default 0
    std::optional<double> paddingBottom; // bottom inset, points; overrides padding
    std::optional<double> titlelessPaddingTop; // default = padding_bottom
    std::optional<bool> squareBottomCorners; // square off the window's bottom corners; default on
    std::optional<TitlebarStyle> titlebarStyle; // themed (default) vs native system title bar
    std::optional<BeamSurface> beamSurface; // one spelled isBeamSurfaceSet would read as a different KIND of flag
    std::optional<bool> folderIcon; // show the cwd folder proxy icon in the title bar; default on
    std::optional<bool> folderIconAlways; // keep that icon resident instead of revealing it on hover; default off
    std::optional<bool> processIcons; // a program's icon before its name in window, tab and pane titles; default on
    std::optional<bool> paneTitleBars; // per-pane title bar in split layouts
    std::optional<bool> shouldWatchForZModem; // watch host output for a ZMODEM handshake
    std::optional<bool> blinkAnimation; // cursor + SGR blink fades (true) vs. hard on/off (false); default true
    std::optional<bool> snapWindowResize; // interactive resize snaps to whole cells, except flush against a screen edge / dock; default on
    std::optional<bool> hideTitleBar; // macOS: hide title bar + traffic lights (keeps rounded corners/shadow);
                                      // Cmd-drag the body to move the window. Default off.
    std::optional<std::string> colorblindMode; // none | deuteranopia | protanopia | tritanopia; CVD-safe palette remap
    std::optional<std::string> shadeBlocks; // pattern | fill; how ░ ▒ ▓ draw (Rendering::ShadeStyle)
    std::optional<double> uiFontScale; // chrome (tabs/HUD/settings) font multiplier, 0.5..2.0; independent of the grid font
    std::optional<double> tagColorOpacity; // how strongly a tag's colour is drawn in the title bar and on tabs, 0.1..1.0

    // [title] — which parts the title shows, set independently for the WINDOW and
    // TAB title. Composed "<folder> — <app title> — <process> — <cols×rows>"; each
    // flag drops one segment. Defaults: all on, except tab never shows dimensions.
    std::optional<bool> titleWindowDirectory;
    // Directory as full tilde path ("~/Projects") vs folder name ("Projects").
    // Defaults: window = name, tab = path. Meaningful only when *Directory is on.
    std::optional<bool> titleWindowDirectoryPath;
    std::optional<bool> titleWindowAppTitle; // escape-sequence (OSC 0/1/2) title the app set
    std::optional<bool> titleWindowProcess; // foreground process chain
    std::optional<bool> titleWindowProcessArguments;
    std::optional<bool> titleWindowShellCommand;
    std::optional<bool> titleWindowTTYName;
    std::optional<bool> titleWindowProfileName;
    std::optional<std::string> titleWindowText; // static base Title text; empty = absent
    std::optional<bool> titleWindowDimensions; // columns×rows
    std::optional<bool> titleWindowProgramStatus; // the label a program gave its status colour (OSC 21337)
    std::optional<bool> titleTabDirectory;
    std::optional<bool> titleTabDirectoryPath;
    std::optional<bool> titleTabAppTitle;
    std::optional<bool> titleTabProcess;
    std::optional<bool> titleTabProcessArguments;
    std::optional<bool> titleTabShellCommand;
    std::optional<bool> titleTabTTYName;
    std::optional<bool> titleTabProfileName;
    std::optional<std::string> titleTabText; // static base Title text; empty = absent
    std::optional<bool> titleTabDimensions;
    std::optional<bool> titleTabProgramStatus;
    // [title] pane_* — pane title-bar composition, mirroring tab_*. Each flag
    // defaults UNSET = "inherit the resolved tab_* value". Exception: Dimensions
    // unset means off (not inherited) — a pane label never showed dims.
    std::optional<bool> titlePaneDirectory;
    std::optional<bool> titlePaneDirectoryPath;
    std::optional<bool> titlePaneAppTitle;
    std::optional<bool> titlePaneProcess;
    std::optional<bool> titlePaneProcessArguments;
    std::optional<bool> titlePaneShellCommand;
    std::optional<bool> titlePaneTTYName;
    std::optional<bool> titlePaneProfileName;
    std::optional<std::string> titlePaneText; // static base Title text; empty = absent
    std::optional<bool> titlePaneDimensions;
    std::optional<bool> titlePaneProgramStatus; // unset inherits the resolved tab_program_status
    std::optional<std::string> directoryHiddenFor; // programs whose titles leave the working directory out; default the multiplexers and remote shells
    std::optional<bool> directoryHiddenInFullScreen; // leave the directory out while a program fills the screen; default off
    std::optional<bool> followSystemAppearance; // auto-switch theme on system dark/light flip
    std::optional<std::string> systemDarkThemeName; // theme NAME used in dark mode when follow_system is on
    std::optional<std::string> systemLightThemeName; // theme NAME used in light mode when follow_system is on
    std::optional<double> backgroundOpacity; // window background alpha, 0..1; desktop shows through behind text
    std::optional<bool> shouldEnhanceTextOnBlur;
    std::optional<bool> textContrast;
    std::optional<bool> backgroundVibrancy;
    std::optional<double> blurRadius; // behind-window gaussian blur radius, pt, 0..50; > 0 enables the blur
    std::optional<std::string> backgroundImage; // path to a background image painted behind the grid
    std::optional<double> backgroundImageOpacity; // image alpha 0..1
    std::optional<double> defaultBackgroundOpacity; // alpha for default-background cells only (1.0 = follow background_opacity);
                                                    // < 1 keeps SGR-colored cells solid
    std::optional<BackgroundImageMode> backgroundImageMode; // fill | fit | stretch | tile | center

    // [cursor]
    std::optional<CursorShape> cursorShape; // default CursorShape::Bar (see ctor)
    std::optional<bool> cursorBlink;
    std::optional<int> cursorLinger; // frames solid after typing, 40
    std::optional<int> cursorBlinkRate; // blink period in milliseconds, 1000 (one full cycle)
    std::optional<int> cursorAlpha; // 0..255, cursor color alpha
    std::optional<bool> cursorInvertText; // block cursor: grayscale-invert the glyph under it
    std::optional<CursorMotionMode> cursorMotion; // default CursorMotionMode::Off (see ctor)
    std::optional<int> cursorMotionDuration; // ms to reach the target cell

    // [render]
    std::optional<int> maxFPS; // continuous-output frame cap; 0 = uncapped (panel native rate)
    std::optional<bool> vsync; // tear-free CADisplayLink present (default); false disables CAMetalLayer display sync

    // [bell]
    std::optional<bool> bellAudible;
    std::optional<bool> bellVisual;
    std::optional<double> bellVisualIntensity; // peak flash opacity 0.05..1.0, default 0.35

    // [sound] — DECPS play-sound (Docs/extensions.md, Sound). Separate from
    // [bell] because the bell is one short cue the terminal makes and this is a
    // synth a program drives: eight channels, downloadable samples, and a
    // request that can repeat until it is stopped. The one knob the bell has
    // never given anyone is a volume, and sound reaches people who are not
    // looking at the screen, so this is where "quieter" and "not now" live.
    std::optional<bool> soundAudible;
    std::optional<double> soundVolume; // 0..1 of the synth's own level

    // [selection]
    std::optional<bool> copyOnSelect;
    // block_select_modifier — the chord that makes a drag rectangular, as a
    // "+"-joined token list matched exactly ("opt", "shift+opt", "cmd+opt",
    // "ctrl+opt"). Default "opt".
    std::optional<std::string> blockSelectModifier;
    std::optional<std::string> selectModifier;
    // style — "native" draws the platform's selection colour under the text,
    // "tint" the translucent tint over it. Default "native".
    std::optional<std::string> selectionStyle;
    std::optional<std::string> closeIgnoredProcesses;
    std::optional<std::string> closeConfirmation;
    std::optional<bool> linksOverrideReporting;
    // Plain click (no modifier) opens an OSC 8 hyperlink and shows its hover
    // affordance without Cmd. Off reverts OSC 8 to Cmd-hover + Cmd-click. An
    // ssh, sftp, ftp or ftps link needs Cmd even so. A regex- or path-detected
    // link needs Cmd, but for a strict http(s) guess under mouse reporting with
    // links_override_reporting on macOS (Input::plainClickOpenableUnderReporting).
    // Default on.
    std::optional<bool> osc8ClickOpens;
    // Document-style pointer: I-beam only over printed text, arrow over blank area.
    // Off keeps the I-beam everywhere. Pointer shape only (selection semantics
    // unchanged). Scoped to primary screen, mouse reporting off. Default off.
    std::optional<bool> restrictSelectionToText;
    // Default ⌘C style: styled flavors written alongside the plain string. One of
    // "everything" (plain + RTF/HTML with fg+bg+attrs; default), "no_background",
    // "styles_only", "plain". Copy Special variants force their own mode.
    std::optional<std::string> copyStyle;
    // "Style for Copy": theme PALETTE the styled copy renders in. "default" (live
    // palette), "plain" (no styling), or a built-in theme name.
    std::optional<std::string> copyStyleTheme;

    // [paste] — multi-line paste safety guard. When on, a clipboard spanning more
    // than confirmMultilineThreshold lines prompts a confirm/cancel alert before it
    // reaches the PTY. Default on, threshold 1 (confirm when > 1 line).
    std::optional<bool> confirmMultilinePaste;
    std::optional<int> confirmMultilinePasteThreshold; // lines; confirm when count > threshold

    // [clipboard] remote_tools — which pasteboard a remote session's PROGRAMS
    // read, which is a different question from the one Cmd-V answers.
    std::optional<RemoteClipboardTools> remoteClipboardTools;

    // [mouse] — two independent scroll speeds. wheelSpeed (multiplier) / wheelLines
    // (base lines-per-notch) tune the MOUSE WHEEL; trackpadSpeed tunes the TRACKPAD
    // only (multiplier after point deltas are normalized to rows). Independent.
    std::optional<double> wheelSpeed; // mouse-wheel multiplier, default 1.0
    std::optional<int> wheelLines; // mouse-wheel base lines/notch, default 1
    std::optional<double> trackpadSpeed; // trackpad multiplier, default defaultTrackpadSpeed
    std::optional<bool> scrollBounce;

    // [tabs]
    std::optional<TabShortcutHints> tabShortcutHints; // ⌘N hints on tabs; default Always

    // [window] drag_modifier — modifier set that turns a grid drag into a window
    // move. A "+"-joined token list ("control+command", "command", …); empty /
    // "none" disables. Default "control+command" (clear of ⌘-click and option-drag).
    std::optional<std::string> windowDragModifier;
    // program_fullscreen — how XTWINOPS `CSI 10 t` from a PROGRAM enters full
    // screen. "fill" (default) resizes the window over the visible frame:
    // synchronous, stays resizable, and a following geometry request lands.
    // "native" uses the macOS transition, which moves to another Space, animates
    // for about a second, and refuses to resize while it holds. The green button
    // is always native either way; this governs only the escape sequence.
    std::optional<std::string> windowProgramFullscreen;
    // initial_columns / initial_rows — pin the FIRST window's grid to an exact cell
    // count. Both > 0 sizes the window every launch, over any saved pixel frame.
    // 0 (default) leaves launch size to the saved frame / compile-time default.
    std::optional<int> windowInitialColumns;
    std::optional<int> windowInitialRows;

    // [keys] compose_key — terminal-decoded compose (Multi_key) trigger key.
    // When set, starts a compose sequence (Compose ' e -> é, via Input::ComposeEngine)
    // before the key reaches the host. One key spec, e.g. "ctrl+space" or "menu".
    // Empty (default) disables compose-key decoding.
    std::optional<std::string> composeKey;
    // [keys] option_as_alt — which Option keys act as Meta. An Option key not
    // named here reaches the input method, which types the layout's character.
    std::optional<OptionAsAlternate> optionAsAlternate;
    /// The software keyboard’s ordered subset of supported accessory symbols.
    std::optional<std::string> keyboardAccessorySymbols;

    // [quake] — global-hotkey-toggled drop-down terminal: a borderless window that
    // slides from the top of the active screen and hides on losing focus. Opt-in.
    std::optional<bool> quakeEnabled; // register the global toggle hotkey; default off
    std::optional<std::string> quakeHotkey; // toggle hotkey spec, e.g. "control+grave"; default "control+grave"
    std::optional<double> quakeHeight; // fraction of the active screen height the panel covers, 0.2..1.0; default 0.5
    std::optional<bool> quakeAnimate; // slide animation on show/hide; default on

    // [dock] — attention cues when the app is in the background
    std::optional<bool> dockBounce; // bounce the Dock icon; default on
    std::optional<bool> dockBadge; // show a Dock badge count; default on

    // [security] — local-only, never synced (a security setting)
    std::optional<bool> secureKeyboardEntry; // macOS Secure Keyboard Entry (EnableSecureEventInput); default on
    std::optional<bool> passwordDetect; // password-prompt heuristic (lock indicator + echo mask); default on
    std::optional<bool> secureKeyboardEntryAtPasswordPrompts;
    std::optional<std::string> clipboardRead; // OSC 52 read gate: "prompt" (default) | "allow" | "deny"
    std::optional<std::string> clipboardWrite; // OSC 52 write gate: "allow" (default) | "deny"
    // Per-OSC / per-DCS / per-APC sequence allowlist. Raw lists, parsed by the
    // engine: osc_* are decimal OSC codes (e.g. "8, 52 1337"); dcs_* are kind
    // names (sixel decudk decrqss xtgettcap tmux); apc_* are APC first bytes
    // (G = kitty graphics). Deny wins; a non-empty allow list is exclusive.
    // All empty (default) = everything passes.
    std::optional<std::string> oscAllow;
    std::optional<std::string> oscDeny;
    std::optional<std::string> dcsAllow;
    std::optional<std::string> dcsDeny;
    std::optional<std::string> apcAllow;
    std::optional<std::string> apcDeny;

    // [capabilities.local] / [capabilities.agent] / [capabilities.remote] — the
    // host daemon's verdict for a program's side effects when no grant exists,
    // per driving origin. Read from the host's own file: a peer must not be able
    // to relax the policy that gates it. Resolved by Effective::capabilityDecisions.
    CapabilityOverrides localCapabilities;
    CapabilityOverrides agentCapabilities;
    CapabilityOverrides remoteCapabilities;

    // [control] — local remote-control socket (JSON-RPC over a unix-domain socket).
    // SECURITY: never synced; disabled by default. Socket is owner-only (0600) in
    // the 0700 ~/.ubiquitty directory; the listener verifies the peer's uid.
    std::optional<bool> controlEnabled; // serve the control socket; default OFF (security)
    std::optional<std::string> controlSocketPath; // override the default ~/.ubiquitty/control.sock
    std::optional<std::string> controlToken; // optional shared secret; clients must send it to authenticate
    // The socket's twin of the CLI's --yes: a `confirm` verdict from the agent
    // policy counts as approved. Default OFF; the socket has no human to ask.
    std::optional<bool> controlApprove;

    // [experimental] — opt-in features still being validated
    std::optional<bool> sessionPersistence; // restore windows/tabs/panes across app-close; default off

    // [notifications] — OS desktop notifications (OSC 9/99/777 + unfocused bell)
    std::optional<bool> notifications; // post UNUserNotifications; default on
    std::optional<bool> notificationSound; // play the default banner sound; default on

    // [debug] — developer diagnostics; the build-identity badge in the titlebar.
    std::optional<bool> debugShowBuildHash; // bool-ok: mirrors a boolean config key — show "<version> (<hash>)" badge in the titlebar; default off

    // [updates] check_automatically — whether the app's updater looks for a new
    // version on its own schedule. A check that finds one shows it and waits for
    // the person; nothing downloads or installs without them. Default on.
    std::optional<bool> shouldCheckForUpdatesAutomatically;

    // [remote] — thin-client (ubiquitty-host) behavior. Auto-install: when a remote
    // has no ubiquitty-host, push a bundled prebuilt binary over ssh on connect.
    // Off (or no bundled binary) shows manual-install instructions instead.
    std::optional<bool> autoDeployHostOnRemoteConnect; // default on
    std::optional<bool> shouldPersistRemoteHistory; // encrypted history cache; default off
    std::optional<std::string> nonOwnerPresentation; // letterbox (default), native-size pan, or uniform fit

    // [network] — SOCKS/HTTP proxy for a native ssh / mosh session (ssh
    // ProxyCommand=`nc -X <kind> -x host:port`; same in mosh's --ssh bootstrap).
    // proxyKind: "" / "none" (default), "socks5", "socks4", "http". Empty host /
    // 0 port = off regardless of kind.
    std::optional<std::string> proxyKind;
    std::optional<std::string> proxyHost;
    std::optional<int> proxyPort;
    // [network] ssh_forward_agent — default `-A` (forward the SSH auth agent) on a
    // new SSH/mosh session. Off by default; also a per-launch checkbox.
    std::optional<bool> sshForwardAgent;

    // [transfer] — in-band file transfer (ZMODEM rz/sz auto-detect). download_dir:
    // where received files save; empty = ~/Downloads (SECURITY: frontend confirms
    // the directory before any write). auto_receive: whether a detected SEND may
    // start the receive flow at all — on, it raises that confirmation, which still
    // gates every write; off, a detection does nothing and the user starts a
    // transfer from the menu. It cannot mean "write without confirming": the
    // directory confirmation is a security invariant, not a preference.
    // Consulted through Network::PromptBudget::mayPrompt, which weighs it against
    // the interruption budget so a frontend answers one question, not two.
    std::optional<std::string> downloadDir;
    std::optional<bool> transferAutoReceive; // default on (confirmation dialog still gates the write)

    // [term] — identity advertised to children via TERM_PROGRAM /
    // TERM_PROGRAM_VERSION. Capability-gated tools key off this.
    std::optional<std::string> termProgram;
    std::optional<std::string> termProgramVersion;
    // $TERM override. Empty = auto (xterm-ubiquitty if resolvable, else xterm-256color).
    std::optional<std::string> termIdentifier;
    // Whether a spawned shell is given the user's locale in LANG. A GUI process
    // inherits no locale from launchd, so without this the shell runs under
    // C/POSIX and sorts and formats unlike the rest of the system. Off for a
    // user who sets locale in their shell rc and does not want the terminal
    // choosing categories — LANG carries LC_NUMERIC, and a moved decimal
    // separator has broken build scripts.
    std::optional<bool> setsLocaleEnvironment;

    // [terminal] encoding — characterSet incoming bytes are decoded from before the
    // grapheme stage: "utf-8" (default), or a legacy value ("latin1",
    // "iso-8859-15", "koi8-r", "windows-1252", "shift_jis", "euc-jp", "euc-kr",
    // "gb18030", "big5") routing through the legacy decoder. Unrecognized keeps
    // default. Resolved to Ubiquitty::Unicode::Encoding via encodingFromName.
    std::optional<std::string> encoding;

    // [terminal] invalid_utf8 — substitution for invalid/truncated UTF-8: "fffd"
    // (default, U+FFFD), "drop" (emit nothing), or "latin1" (byte -> U+0080..U+00FF).
    // Resolved via Ubiquitty::Unicode::invalidUTF8PolicyFromName.
    std::optional<std::string> invalidUTF8;

    // [terminal] emulation — escape grammar the parser interprets. "vt" (default,
    // DEC/ANSI); "linux"/"sco" layer console sequences on the ANSI path;
    // "wyse"/"televideo"/"adm"/"heath"/"hp" are non-ANSI legacy grammars.
    // Resolved to Ubiquitty::Emulation via emulationFromName.
    std::optional<std::string> emulation;

    // [terminal] scrollback_lines — history depth per session. Default defaultScrollbackLines.
    std::optional<int> scrollbackLines;
    // [terminal] column_mode_resizes_window — DECCOLM (80/132) resizes the window
    // (xterm c132) by default; false renders in place. Fullscreen always in place.
    std::optional<bool> columnModeResizesWindow;

    // [terminal] ambiguous_width — cells an East_Asian_Width=Ambiguous codePoint
    // occupies: 1 (default) or 2 (legacy CJK). Clamped to {1,2}. Seeded into
    // each terminal's Terminal::ambiguousWidth at launch and on reload.
    std::optional<int> ambiguousWidth;

    // [unicode] normalization — form applied to search and copy text: "none"
    // (default), "nfc", "nfd", "nfkc", "nfkd". Both sides must normalize to the
    // same form to match. Resolved via Ubiquitty::Unicode::normalizationFormFromName.
    // Not applied per-cell on ingest — only search/copy paths.
    std::optional<std::string> normalization;

    // Keys the parse read and did not apply, in file order. Populated in both
    // parse modes: Strict already fails the load, and what it fails over is
    // worth naming too. Empty after a file whose every key landed, and cleared
    // at the start of each parse rather than accumulated, since a key skipped
    // in another file says nothing about this one.
    //
    // Not a config value: nothing reads it to decide behaviour. The layering
    // helpers cannot carry it either way — they resolve `Profile`, and this
    // lives on `Values`.
    std::vector<SkippedKey> skippedKeys;

    // [[themes]] — user-defined runtime themes; appended to the built-in registry.
    std::vector<TagEntry> tags;
    std::vector<ThemeEntry> themes;

    // [keys] — user key bindings as raw spec->action pairs, in file order.
    // Fed to Input::BindingTable, consulted before the default key path.
    std::vector<KeyBindingEntry> keyBindings;

    // [link_handlers] — user scheme->command templates, in file order. Consulted
    // before the default OS-open path; an unconfigured scheme falls through to NSWorkspace.
    std::vector<LinkHandlerEntry> linkHandlers;

    // [links] line_handler — the command a file link that names a line opens
    // in, `{path}`, `{line}` and `{column}` filled in (LinkRun::lineHandlerArgv).
    // Empty = such a link opens the way any file link does.
    std::optional<std::string> linkLineHandler;

    // [triggers] — ordered regex->action rules evaluated against each completed
    // line, in file order (priority). Empty = nothing fires. Compiled into a Triggers::Engine.
    std::vector<TriggerEntry> triggers;

    // [triggers] allow_run_command — SECURITY GATE for the run_command action.
    // DEFAULT FALSE: the rule parses + dispatches, but the command never spawns
    // until the user opts in. Matched text is attacker-controllable; the frontend
    // always builds an explicit argv (never a shell string).
    std::optional<bool> allowRunCommand; // default false (gate closed)

    // [scripting] — opt-in hook to a user executable that EMITS trigger/macro/
    // characterClass rules at startup. `config_script` (gated by `enable = true`,
    // default off) runs; each stdout line parses as:
    //   trigger <regex> = <action[:arg]>
    //   macro <name> = <body>
    //   charclass <name> = <regex>
    // (blank lines + '#' comments ignored).
    std::optional<std::string> scriptingConfigScript;
    std::optional<bool> scriptingEnable; // gate (default false): don't run config_script unless set

    // [triggers] allow_webhook — SECURITY GATE for the webhook action, mirroring
    // allow_run_command. DEFAULT FALSE: the rule parses + dispatches, but the HTTP
    // post never sends until the user opts in. Body is attacker-controllable.
    std::optional<bool> allowWebhook; // default false (gate closed)

    // [macros] — user-named text macros (name->body), in file order. A [keys]
    // `macro:<name>` binding sends the same body; escapes decoded at invoke time.
    std::vector<MacroEntry> macros;

    // [characterClass] — smart-selection token classes (name->regex) consulted by the
    // double-click word-selection path, in file order. Empty = default behavior.
    std::vector<CharacterClassEntry> characterClasses;

    // [[profiles]] — named, serializable presentation bundles, resolved over the
    // synthesized "Default" profile (the live global config) before applying.
    std::vector<Profile> profiles;

    // [mac]/[linux] — per-platform override blocks
    // (Docs/platform-config-and-sync.md), the final resolution layer for the
    // build's platform. Indexed by Platform: [0]=mac [1]=linux.
    PlatformOverride platformOverrides[2];

    /// Creates one set of preferences at its compile-time defaults.
    ///
    /// Every `<field>Set` flag starts false, so a freshly-constructed instance
    /// merges as though the file were absent rather than as though it had
    /// specified the defaults.
    Values();

    /// The override block for `p` (mutable + const). Indexing mirrors the
    /// Platform enum order so the apply step can pick currentPlatform()'s slot.
    ///
    /// - Parameter p: Which platform slot to reach.
    /// - Returns: That slot, by reference, so a caller may read or fill it.
    PlatformOverride& platformOverride(Platform p) { return platformOverrides[static_cast<int>(p)]; }
    [[nodiscard]] const PlatformOverride& platformOverride(Platform p) const { return platformOverrides[static_cast<int>(p)]; }

    /// The four padding edges, in points.
    struct PaddingEdges {
        double left;
        double right;
        double top;
        double bottom;
    };

    /// The four padding edges resolved to points for a given target.
    ///
    /// The per-edge field wins, else the `padding` shorthand, else the
    /// target's built-in default (``Defaults::paddingEdges(DefaultsTarget)``).
    /// Each edge is clamped to [0, 64].
    ///
    /// - Parameter target: Whose built-in default fills an edge nothing states.
    /// - Returns: The edges.
    [[nodiscard]] PaddingEdges resolvedPadding(DefaultsTarget target) const;

    /// The four padding edges resolved for this build's target.
    ///
    /// - Returns: The edges, as ``resolvedPadding(DefaultsTarget)`` answers for
    ///   ``currentDefaultsTarget()``.
    [[nodiscard]] PaddingEdges resolvedPadding() const { return resolvedPadding(currentDefaultsTarget()); }
};

/// One platform section's overrides, as the profile it holds.
///
/// A free function beside the member: Swift imports a member returning a
/// reference into its own value only under an unsafe name, and a settings
/// surface reading a section needs no more than this.
///
/// - Parameters:
///   - values: The configuration.
///   - platform: The section's platform.
/// - Returns: The section's profile, by reference into `values`.
[[nodiscard]] inline const Profile& platformOverrideProfile(const Values& values, Platform platform) { return values.platformOverride(platform).profile; }

/// The rules a [scripting] config_script emits on stdout. Each non-blank,
/// non-'#' line is one directive:
///   trigger <regex> = <action[:arg]>   -> triggers
///   macro <name> = <body>              -> macros
///   charclass <name> = <regex>         -> characterClasses
/// Key and value split on the FIRST '='; whitespace trimmed; unknown ignored.
/// parseScriptingRules returns the number of directives accepted.
struct ScriptingRules {
    std::vector<TriggerEntry> triggers;
    std::vector<MacroEntry> macros;
    std::vector<CharacterClassEntry> characterClasses;
};
///
/// - Parameters:
///   - scriptOutput: The directive text a script wrote.
///   - out: Filled with what parsed. Untouched entries keep their prior value,
///     so a caller may accumulate over several runs.
/// - Returns: How many directives were accepted; unknown ones are ignored.
int parseScriptingRules(const std::string& scriptOutput, ScriptingRules& out);

/// The most of a `config_script`'s standard output the hook reads.
///
/// The script is the reader's own program, but it runs on every load, and one
/// that loops printing would otherwise hold the load open while it grows the
/// buffer without bound. A frontend stops reading here and ends the script.
inline constexpr std::size_t scriptingOutputLimit = 256 * 1024;

/// Folds what a `config_script` printed into a configuration, after the rules
/// the file itself declared.
///
/// Reads no more than ``scriptingOutputLimit`` bytes of the output, so a caller
/// that read past the bound is still held to it: a directive beyond it is not
/// merged.
///
/// - Parameters:
///   - into: The configuration the rules are appended to.
///   - scriptOutput: What the script wrote to its standard output.
/// - Returns: How many directives were merged.
int mergeScriptingRules(Values& into, const std::string& scriptOutput);

/// The RAW body of the [macros] entry named `name` (first match in file order),
/// or nullptr when no such macro exists. The body still carries its escapes;
/// callers decode them with Input::decodeSendPayload before sending. Returns a
/// pointer into `c` (valid as long as `c` outlives the use).
///
/// - Parameters:
///   - c: The preferences to search.
///   - name: The macro name as `[macros]` spells it.
/// - Returns: The body, borrowed from `c`, or nullptr when no macro carries
///   that name. The pointer is valid while `c` is.
const std::string* findMacroBody(const Values& c, const std::string& name);

/// Parse "#rrggbb" or "#rrggbbaa" (case-insensitive). On success fills
/// `out` and returns true; out.set is true. Invalid input leaves out
/// untouched and returns false.
///
/// - Parameters:
///   - s: A `#rrggbb` or `#rrggbbaa` literal.
///   - out: Filled on success, untouched on failure, so a caller may parse
///     over a defaulted slot without losing it.
/// - Returns: Whether `s` was a well-formed literal.
bool parseHexColor(const std::string& s, ColorEntry& out);

// Map a cursor-shape string ("block" | "underline" | "bar", case-
// insensitive) to a CursorShape, or std::nullopt for unrecognized input.
[[nodiscard]] std::optional<CursorShape> cursorShapeFromString(const std::string& s);

// The canonical lowercase token for a CursorShape, for round-tripping to the
// config file. The inverse of cursorShapeFromString.
[[nodiscard]] const char* cursorShapeToString(CursorShape);

// Map an integer (a menu-item tag or the Swift settings index) to its
// CursorShape, or std::nullopt when out of range — the one place the
// shape's integer domain is validated.
[[nodiscard]] std::optional<CursorShape> cursorShapeFromInt(int value);

// Map a cursor-motion string ("off" | "short" | "long" | "always", case-
// insensitive) to a CursorMotionMode, or std::nullopt for unrecognized input.
[[nodiscard]] std::optional<CursorMotionMode> cursorMotionFromString(const std::string& s);

// The canonical lowercase token for a CursorMotionMode, for round-tripping to
// the config file. The inverse of cursorMotionFromString.
[[nodiscard]] const char* cursorMotionToString(CursorMotionMode);

// Map a show_shortcuts token ("off" | "always" | "command", case-insensitive)
// to a TabShortcutHints, or std::nullopt for anything else.
[[nodiscard]] std::optional<TabShortcutHints> tabShortcutHintsFromString(const std::string& s);

// The canonical token for a TabShortcutHints. The inverse of
// tabShortcutHintsFromString.
[[nodiscard]] const char* tabShortcutHintsToString(TabShortcutHints);

// Map an integer (the Swift settings segmented index) to its CursorMotionMode,
// or std::nullopt when out of range — the one place the mode's integer domain
// is validated.
[[nodiscard]] std::optional<CursorMotionMode> cursorMotionFromInt(int value);

// Map a capability-verdict string ("allow" | "prompt" | "deny", case-
// insensitive) to a CapabilityDecision, or std::nullopt for unrecognized input.
[[nodiscard]] std::optional<CapabilityDecision> capabilityDecisionFromString(const std::string& s);

// The canonical lowercase token for a CapabilityDecision, for round-tripping to
// the config file. The inverse of capabilityDecisionFromString.
[[nodiscard]] const char* capabilityDecisionToString(CapabilityDecision);

// Map an option-as-alt string ("none" | "left" | "right" | "both", case-
// insensitive) to an OptionAsAlternate, or std::nullopt for unrecognized input.
[[nodiscard]] std::optional<OptionAsAlternate> optionAsAlternateFromString(const std::string& s);

// Map a titlebar-style string ("themed" | "native", case-insensitive) to a
// TitlebarStyle, or std::nullopt for unrecognized input.
[[nodiscard]] std::optional<TitlebarStyle> titlebarStyleFromString(const std::string& s);

// The canonical lowercase token for a TitlebarStyle, for round-tripping to the
// config file. The inverse of titlebarStyleFromString.
[[nodiscard]] const char* titlebarStyleToString(TitlebarStyle);

// Map an integer (the Swift settings index) to its TitlebarStyle, or
// std::nullopt when out of range.
[[nodiscard]] std::optional<TitlebarStyle> titlebarStyleFromInt(int value);

// Map a beam-surface string ("auto" | "native" | "in-terminal", case-insensitive,
// "in_terminal" and "terminal" aliases) to a BeamSurface, or std::nullopt for
// unrecognized input.
[[nodiscard]] std::optional<BeamSurface> beamSurfaceFromString(const std::string& s);

// The canonical lowercase token for a BeamSurface. The inverse of
// beamSurfaceFromString.
[[nodiscard]] const char* beamSurfaceToString(BeamSurface);

// Map an integer (the Swift settings index) to its BeamSurface, or std::nullopt
// when out of range.
[[nodiscard]] std::optional<BeamSurface> beamSurfaceFromInt(int value);

// Reconcile the configured preference with what the frontend can offer.
//
// Falls back to the other surface whenever the requested one is unavailable,
// including when it was named explicitly. A frontend that has not built a native
// palette yet, or one whose renderer cannot composite chrome into the grid, must
// still open a palette when the user presses the key: the alternative is a
// keystroke that silently does nothing, which reads as a broken binding rather
// than as a setting.
//
// Both directions are real. The browser has no native palette; macOS has no
// in-grid one, because its shipping frame is assembled by its own encode and the
// Core capture that takes chrome is reached only by the parity profiler. The
// in-grid surface is an input rather than assumed: it is drawable for an
// attached client and not for a frontend that renders locally.
//
// - Parameters:
//   - configured: What [appearance] beam_surface asked for.
//   - native: Whether this frontend has a native palette.
//   - inGrid: Whether its renderer can draw one into the cell grid.
// - Returns: The surface to use. With neither available the caller should not be
//   offering the command at all; the in-grid answer is returned so a caller that
//   does anyway draws into its own buffer rather than into a toolkit it lacks.
[[nodiscard]] BeamPresentation resolveBeamSurface(BeamSurface configured, NativeBeam native, InGridBeam inGrid);

// --- color_space <-> Values index (0 = sRGB, 1 = CellGrid P3) ----------

// Map a color-space string ("srgb" | "display-p3", case-insensitive,
// surrounding whitespace ignored) to a ColorSpace, or std::nullopt for
// unrecognized input.
[[nodiscard]] std::optional<ColorSpace> colorSpaceFromString(const std::string& s);

// The canonical lowercase token for a ColorSpace, for round-tripping to the
// config file. SRGB → "srgb", DisplayP3 → "display-p3".
[[nodiscard]] const char* colorSpaceToString(ColorSpace);

// Map the Values segmented-control index (0 = sRGB, 1 = CellGrid P3) to a
// ColorSpace, or std::nullopt when out of range.
[[nodiscard]] std::optional<ColorSpace> colorSpaceFromInt(int value);

// The Values segmented-control index for a ColorSpace (the inverse of
// colorSpaceFromInt).
[[nodiscard]] int colorSpaceToInt(ColorSpace);

// --- background_image_mode <-> Values popup index (0 = Fill … 4 = Center) ---

// Map a mode string ("fill" | "fit" | "stretch" | "tile" | "center",
// case-insensitive, surrounding whitespace ignored) to a BackgroundImageMode,
// or std::nullopt for unrecognized input.
[[nodiscard]] std::optional<BackgroundImageMode> backgroundImageModeFromString(const std::string& s);

// The canonical lowercase token for a BackgroundImageMode, for round-tripping
// to the config file. The inverse of backgroundImageModeFromString.
[[nodiscard]] const char* backgroundImageModeToString(BackgroundImageMode);

// Map the Values popup index (0 = Fill … 4 = Center) to a
// BackgroundImageMode, or std::nullopt when out of range.
[[nodiscard]] std::optional<BackgroundImageMode> backgroundImageModeFromInt(int value);

// The Values popup index for a BackgroundImageMode (the inverse of
// backgroundImageModeFromInt).
[[nodiscard]] int backgroundImageModeToInt(BackgroundImageMode);

// --- colorblind_mode <-> Values popup index (0 = None … 3 = Tritanopia) ---

// Map a mode string ("none" | "deuteranopia" | "protanopia" | "tritanopia",
// case-insensitive, surrounding whitespace ignored) to a Accessibility::ColorblindMode, or
// std::nullopt for unrecognized input.
[[nodiscard]] std::optional<Accessibility::ColorblindMode> colorblindModeFromString(const std::string& s);

// The canonical lowercase token for a Accessibility::ColorblindMode, for round-tripping to the
// config file. The inverse of colorblindModeFromString.
[[nodiscard]] const char* colorblindModeToString(Accessibility::ColorblindMode);

// Map the Values popup index (0 = None … 3 = Tritanopia) to a Accessibility::ColorblindMode,
// or std::nullopt when out of range.
[[nodiscard]] std::optional<Accessibility::ColorblindMode> colorblindModeFromInt(int value);

// The Values popup index for a Accessibility::ColorblindMode (the inverse of
// colorblindModeFromInt).
[[nodiscard]] int colorblindModeToInt(Accessibility::ColorblindMode);

// --- show_shortcuts <-> Values index (0 = Off, 1 = Always, 2 = On ⌘) ---
// Index mirrors the TabShortcutHints enum value. String<->enum stays with the parser.

// Map the Values segmented-control index (0 = Off, 1 = Always, 2 = On ⌘) to a
// TabShortcutHints, or std::nullopt when out of range.
[[nodiscard]] std::optional<TabShortcutHints> tabShortcutHintsFromInt(int value);

// The Values segmented-control index for a TabShortcutHints (the inverse of
// tabShortcutHintsFromInt).
[[nodiscard]] int tabShortcutHintsToInt(TabShortcutHints);

// The Values segmented-control index for a CursorShape (the inverse of
// cursorShapeFromInt; 0 = Block, 1 = Underline, 2 = Bar).
[[nodiscard]] int cursorShapeToInt(CursorShape);

// The Values segmented-control index for a CursorMotionMode (the inverse of
// cursorMotionFromInt; 0 = Off, 1 = Short, 2 = Long, 3 = Always).
[[nodiscard]] int cursorMotionToInt(CursorMotionMode);

/// Parse TOML from a stream into out. Returns true when the stream was
/// read without a structural parse error; unknown keys/sections are
/// ignored (forward compatibility), and recognized keys with the wrong
/// value type are skipped without aborting the parse. Fields whose keys
/// are absent keep their defaults with the is-set flag false.
enum class ParseMode {
    Permissive,
    Strict,
};

/// Parses TOML from `in` into `out`.
///
/// A bad line costs that line only. The keys under a section header that
/// cannot be read land in no section, rather than in the one before it, and a
/// multi-line array that reaches a header or an assignment before its closing
/// bracket ends there, so the lines after it are read as written. Each
/// diagnostic names the line its value starts on.
///
/// - Parameters:
///   - in: The TOML text to read.
///   - out: Filled with what parsed. Must be populated first, normally
///     default-constructed, because a key the text omits keeps its value.
///   - mode: Whether an unrecognized key fails the parse or is recorded and
///     skipped.
/// - Returns: Whether the text parsed. A skipped key is not a failure.
bool parse(std::istream& in, Values& out, ParseMode = ParseMode::Permissive);

/// Resolve `base` against the override block for platform `p`: the platform
/// profile is folded over `base` as the final layer. Unset platform fields
/// inherit `base`; a missing/empty block returns `base`.
///
/// - Parameters:
///   - base: The profile the override folds over.
///   - c: The preferences carrying the override blocks.
///   - p: Which platform block to apply.
/// - Returns: `base` with that platform's named fields folded in.
Profile applyPlatformOverride(const Profile& base, const Values& c, Platform p);

/// Same, for the build's own platform (currentPlatform()). This is the call
/// the apply step makes after resolving a profile over the Default.
///
/// - Parameters:
///   - base: The profile the override folds over.
///   - c: The preferences carrying the override blocks.
/// - Returns: `base` with this build's platform block folded in.
Profile applyCurrentPlatformOverride(const Profile& base, const Values& c);

/// Merge the global [keys] bindings with platform `p`'s [<plat>.keys]
/// overrides (Docs/platform-config-and-sync.md): a platform binding for a spec
/// replaces the global one (later layer wins); platform-only specs are
/// appended. File order is otherwise preserved. Result is the table the
/// frontend feeds to Input::BindingTable.
///
/// - Parameters:
///   - c: The preferences carrying the global and per-platform blocks.
///   - p: Which platform's overrides to layer on.
/// - Returns: The merged table, in file order.
std::vector<KeyBindingEntry> resolveKeyBindings(const Values& c, Platform p);

/// Same, for the build's own platform.
///
/// - Parameter c: The preferences carrying the global and per-platform blocks.
/// - Returns: The merged table for this build's platform, in file order.
std::vector<KeyBindingEntry> resolveCurrentKeyBindings(const Values& c);

// The commented default config.toml written on first run, documenting
// every supported key with its default value. Returned as a string so
// the platform layer owns the actual file write.
[[nodiscard]] std::string defaultFileContents();

/// The process-wide live preferences.
///
/// The first call acquires the configuration blob through the PAL preferences
/// seam and parses it; a blob that is absent leaves a defaulted instance, which
/// is the first-run path rather than an error. The only way the instance moves
/// is a configure (Configuration.h): a whole document parsed through the one
/// default table, which moves ``configurationEpoch`` and tells each
/// ``ConfigurationSubscription`` once. A caller holding the reference sees each
/// configured document through it.
///
/// Main-thread only, readers and writers alike: every frontend reads it from
/// its event loop, and the ingest, render, decode and transport threads read
/// per-terminal fields seeded from it instead. That single thread is what
/// makes the in-place replacement below race-free; a debug build asserts it.
///
/// Const, with no writable handle beside it: a setting moves by configuring
/// its key or its document, never by writing a field a subscriber would not
/// hear about.
///
/// - Returns: The live instance, by reference. It outlives every caller, so a
///   borrowed view into it stays valid.
const Values& values();

/// The identity PAL stamps into a launched child's environment (TERM,
/// TERM_PROGRAM, TERM_PROGRAM_VERSION).
///
/// Read from the instance the caller hands over rather than from ``values()``,
/// so a process hosting several sessions with their own configuration stamps
/// each child with its session's identity. An unset TERM_PROGRAM identifies
/// Ubiquitty; an explicit value, including an empty string, takes precedence.
///
/// - Parameter configuration: The instance whose identity fields are viewed.
/// - Returns: Views into `configuration`'s own storage or static defaults;
///   they stay valid as long as that instance does.
[[nodiscard]] PAL::TerminalIdentity terminalIdentity(const Values& configuration);

/// Round-trip writer: replace (or append) `<key> = <tomlValue>` inside [section]
/// in `path`, preserving comments + unrelated lines + formatting. Behavior:
///  - existing `key = ...` line: rewrites the value text in place, keeping
///    the indent and any trailing `# comment`;
///  - commented `# key = ...` line inside the section: uncomments and updates;
///  - key not present: appends `<key> = <tomlValue>` at the end of the section;
///  - section header absent: appends `[section]\n<key> = <tomlValue>\n`;
///  - file missing: creates parent directory + writes a new file with the section.
/// Atomic: writes to `<path>.tmp` then renames over `path`. Returns true on
/// success.
///
/// - Parameters:
///   - path: The file to rewrite.
///   - section: The `[section]` the key belongs to.
///   - key: The key name, as the file spells it.
///   - tomlValue: The literal to write, already serialized.
/// - Returns: Whether the file was written.
bool writeKeyValue(const std::string& path, const std::string& section, const std::string& key, const std::string& tomlValue);

/// TOML value formatters for writeKeyValue.
///
/// - Parameter v: The number to render.
/// - Returns: Its TOML integer literal.
std::string tomlInt(long long v);
///
/// - Parameter v: The toggle to render.
/// - Returns: `true` or `false`.
std::string tomlBool(bool v);
///
/// - Parameter v: The number to render.
/// - Returns: Its TOML float literal.
std::string tomlDouble(double v);
///
/// - Parameter v: The text to render.
/// - Returns: Its quoted TOML basic-string literal.
std::string tomlString(std::string_view v);

// --- Block-level TOML mutation -----------------------------------------
//
// The Values Themes / Profiles / Keys panes edit *blocks* (array-of-
// tables entries or the whole [keys] table), which the line-oriented
// writeKeyValue can't model. These operate on an in-memory body string so
// they stay portable + host-testable; the platform layer owns file I/O.

/// "#rrggbb" / "#rrggbbaa" from a packed 0xRRGGBBAA color (the 'aa' suffix
/// is emitted only when the alpha byte is not 0xFF).
///
/// - Parameter rgba: The colour, red in the high byte.
/// - Returns: `"#rrggbb"` when opaque, `"#rrggbbaa"` otherwise, lowercase.
std::string tomlHexColor(std::uint32_t rgba);

/// True when the trimmed line is a `"..." = ...` binding entry (a [keys]
/// value line) rather than a comment, blank, or section header.
///
/// - Parameter line: One line of the file.
/// - Returns: Whether it is a binding entry rather than a comment, a blank,
///   or a section header.
bool isBindingLine(const std::string& line);

/// A half-open byte range of the config text.
///
/// The two ends only mean anything together, and a caller holding one of them
/// has nothing it can splice on. That is why this is a value rather than two
/// out-parameters: a range half-written is a range that erases the wrong bytes.
struct BlockRange {
    /// First byte of the block, at the start of its `[[header]]` line.
    std::size_t start;
    /// One past its last byte, at the start of the line that ends the block.
    std::size_t end;
};

/// Locates the `[[<header>]]` array-of-tables block carrying `name = "<wanted>"`.
///
/// A block runs from its header line to just before the next line starting with
/// `[` — any header, not only a matching one — or to the end of the text.
///
/// - Parameters:
///   - body: The whole config text the range indexes into.
///   - header: The array-of-tables name, without brackets.
///   - wanted: The value of the block's `name` key.
/// - Returns: The block's byte range, or nothing when none matches. The empty
///   result is what tells `spliceNamedBlock` to append rather than replace, so
///   a caller cannot read a range for a block that is not there.
std::optional<BlockRange> findNamedBlock(const std::string& body, const char* header, const std::string& wanted);

/// Replace the named block of `header` with `block`, or append it when no
/// match exists. Returns the spliced body. The block text comes from the
/// caller's serializer — themes use Themes/ThemeTOMLWrite.h's
/// renderThemeBlock; profiles render their own.
///
/// - Parameters:
///   - body: The file text to splice into.
///   - header: The array-of-tables header, such as `[[themes]]`.
///   - name: The block's `name` value.
///   - block: The replacement text, from the caller's serializer.
/// - Returns: The spliced body.
std::string spliceNamedBlock(std::string body, const char* header, const std::string& name, const std::string& block);

/// Remove the named block of `header` if present. Returns the new body.
///
/// - Parameters:
///   - body: The file text to remove from.
///   - header: The array-of-tables header, such as `[[themes]]`.
///   - name: The block's `name` value.
/// - Returns: The body with that block gone, or unchanged when absent.
std::string removeNamedBlock(std::string body, const char* header, const std::string& name);

/// Locates the `index`th `[[<header>]]` array-of-tables block, counting from
/// zero in the order the text gives them.
///
/// A header is recognised as the parser recognises it — the line's first
/// non-blank characters are `[[`, and the name between the brackets, trimmed,
/// is `header` — so the count agrees with the position the parser stores each
/// entry at. A commented-out header is not a header. A block runs to the next
/// line starting with `[` or to the end of the text.
///
/// - Parameters:
///   - body: The whole config text the range indexes into.
///   - header: The array-of-tables name, without brackets.
///   - index: The block's position among the blocks of that header.
/// - Returns: The block's byte range, or nothing when there are not that many.
std::optional<BlockRange> findIndexedBlock(const std::string& body, const char* header, int index);

/// Replace the `index`th block of `header` with `block`, or append `block`
/// when there is no such block (a negative index included).
///
/// - Parameters:
///   - body: The file text to splice into.
///   - header: The array-of-tables name, without brackets.
///   - index: The block's position, as ``findIndexedBlock`` counts it.
///   - block: The replacement text, from the caller's serializer, with the
///     leading newline an append wants; an in-place replace drops it.
/// - Returns: The spliced body.
std::string spliceIndexedBlock(std::string body, const char* header, int index, const std::string& block);

/// Remove the `index`th block of `header` if present.
///
/// - Parameters:
///   - body: The file text to remove from.
///   - header: The array-of-tables name, without brackets.
///   - index: The block's position, as ``findIndexedBlock`` counts it.
/// - Returns: The body with that block gone, or unchanged when absent.
std::string removeIndexedBlock(std::string body, const char* header, int index);

/// Move the `from`th block of `header` so that it becomes the `to`th, the
/// blocks between shifting one place to close the gap.
///
/// Each block moves as written: its header, every line under it up to the
/// blank lines that end it, and the comment lines directly above the header
/// with no blank line between. The blocks trade places in the slots the file
/// already holds them in, so another section between two of them, and the
/// spacing around each, stays where it was. A comment line directly above the
/// next header is read as that header's, not as the end of the block before.
///
/// - Parameters:
///   - body: The file text.
///   - header: The array-of-tables name, without brackets.
///   - from: The block's position, as ``findIndexedBlock`` counts it.
///   - to: The position it takes.
/// - Returns: The reordered body, unchanged when either position names no
///   block or the two are the same.
std::string moveIndexedBlock(std::string body, const char* header, int from, int to);

/// Remove a `key = ...` line from a single-level `[section]` (e.g. [mac]).
/// Keeps comments and unrelated keys; a commented `# key = ...` is left
/// alone. No-op when the section or key is absent. Returns the new body.
///
/// - Parameters:
///   - body: The file text to remove from.
///   - section: The `[section]` the key belongs to.
///   - key: The key name.
/// - Returns: The body with that line gone, or unchanged when absent.
std::string removeSectionKey(std::string body, const std::string& section, const std::string& key);

/// Sets `section`.`key` to `tomlValue` in an in-memory configuration body.
///
/// The half of ``writeKeyValue`` that is not I/O, so a frontend holding the
/// bytes rather than a path can make the same edit. A backend that keys its
/// storage by blob rather than by name — the browser, and the POSIX preferences
/// backend, which does not publish where a blob lives — has the bytes and not
/// the path, and would otherwise have to spell this rule a second time.
///
/// Comments, ordering and unrelated lines survive: an existing assignment is
/// rewritten in place, a commented-out one is uncommented, a missing key is
/// appended inside its section ahead of the blank lines that separate it from
/// the next, and a missing section is appended whole.
///
/// - Parameters:
///   - body: The current text, empty for a file that does not exist yet.
///   - section: The section to write into, without brackets.
///   - key: The key within it.
///   - tomlValue: The already-serialized literal, from `serializeWrite` or a
///     `toml*` formatter.
/// - Returns: The new text, newline-terminated.
std::string setSectionKey(std::string body, const std::string& section, const std::string& key, const std::string& tomlValue);

/// Rewrite the [keys] section's binding lines from `lines` (each already a
/// full `"spec" = "action"` line, no newline). Leading comment/blank lines
/// inside the section are kept; existing binding lines are dropped; the new
/// ones are appended at the section's end. A missing [keys] section is
/// created at EOF. Returns the new body.
///
/// - Parameters:
///   - body: The file text to rewrite.
///   - lines: The replacement `[keys]` entries, in the order they should appear.
/// - Returns: The body with the section replaced.
std::string rewriteKeysSection(std::string body, const std::vector<std::string>& lines);

} // namespace Ubiquitty::Preferences
