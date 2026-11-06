// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Canonical per-key default constants — one source of truth for each settings
// key's value when the TOML did not supply it. The Values ctor seeds these; the
// effective-value accessors fall back to them for `field.Set ? c.field : DEFAULT`.
// Pure header (ints / floats / plain enums only, no platform types).

#include "Core/Preferences/Preferences.h"

namespace Ubiquitty::Preferences::Defaults {

/// Enables perceptual contrast correction for ordinary terminal text.
inline constexpr bool shouldEnhanceTextContrast = false;

// [font]

/// The families a window opens in when configuration names none, best first.
///
/// The order is the product's, so it belongs here rather than in a frontend:
/// two arms choosing their own opened the same configuration in two different
/// faces, and the one a user sees then depends on which of them built the
/// window. Whether a family is installed is the frontend's to ask — that is an
/// OS fact and each arm has its own font system to ask it of — but which to
/// prefer is not.
///
/// SF Mono leads where installed; bundled JetBrains Mono supplies the fallback.
inline constexpr const char* houseFontFamilies[] = { "SF Mono", "JetBrains Mono", "JetBrains Mono NL", "Menlo" };

/// How many families the preference list holds.
///
/// - Returns: The count, always at least one.
[[nodiscard]] inline int houseFontFamilyCount() { return static_cast<int>(sizeof(houseFontFamilies) / sizeof(houseFontFamilies[0])); }

/// One family from the preference list.
///
/// Indexed rather than handed over whole because the callers are a Swift arm
/// and an Objective-C one, and an array of pointers crosses to neither as
/// anything a caller can walk.
///
/// - Parameter index: A position below ``houseFontFamilyCount``.
/// - Returns: The family, or the last one for an index past the end — a
///   caller asking past the list still gets a family it can open a window in.
[[nodiscard]] inline const char* houseFontFamilyAt(int index)
{
    const int count = houseFontFamilyCount();
    if (index < 0)
        index = 0;
    return houseFontFamilies[index < count ? index : count - 1];
}

inline constexpr double fontSize = 12.0; // pt
inline constexpr bool ligatures = true;
inline constexpr double lineSpacing = 2.0; // extra pt
inline constexpr bool wideGlyphBoost = true;
inline constexpr bool matchFallbackSize = true;
// Braille draws as a lattice by default: the graphics use is the common
// one, and it is the use a font cannot serve.
inline constexpr bool brailleGrid = true; // bool-ok: mirrors a boolean config key
inline constexpr double characterSpacing = 0.0;
// Zero is not a weight; it is the absence of one, and what it selects is the
// face's own upright and bold instances. A face whose regular is not 400 would
// be moved by a default that named a number, and a face with no `wght` axis
// cannot honour one at all.
inline constexpr int fontWeight = 0;
inline constexpr int boldFontWeight = 0;
// Zero is the absence of a width for the reason it is the absence of a
// weight: what it selects is the face's own instance, and a face with
// neither a width axis nor width members cannot honour a number at all.
inline constexpr double fontWidthPercent = 0.0;
inline constexpr double wideGlyphScale = 1.2;

// [underline]
inline constexpr double underlineThickness = 1.0; // x font default
inline constexpr double underlineOffset = 0.0; // extra pt below baseline
inline constexpr bool underlineSkipInk = true;
// [underline] follows_ink — a run's outer rule reaches the letter's ink past
// the cell box. AppKit and CSS stop at the advance; the ink is what the eye
// reads as the word's end, and the product's choice.
inline constexpr bool underlineFollowsInk = true; // bool-ok: mirrors a boolean config key
inline constexpr double underlineSkipInkPad = 1.75; // skip-ink halo radius, pt
inline constexpr bool underlineLinks = true;
inline constexpr int linkUnderlineStyle = 0; // 0 = solid/single
inline constexpr bool linkColorSystemBlue = false;
inline constexpr bool linkStylePerKind = false; // off = all link kinds use link_style; on = OSC 8 kinds distinct
inline constexpr bool smartDoubleClick = false;

// [text]
inline constexpr bool brightenBold = false;

// [term]
// Match the system's sorting and date formats in spawned shells.
inline constexpr bool setsLocaleEnvironment = true;
// Beam's typeface: false = system UI font, true = the terminal's font.
inline constexpr bool beamTerminalFont = false;

// [tek]
inline constexpr bool tekBrightPen = false; // storage-tube bright-pen spot, off
inline constexpr bool tekGlowVectors = false; // additive bloom on vectors, off
// Tek color source: 0 = terminal theme, 1 = custom (colors below). Default theme.
inline constexpr int tekColorMode = 0;
// Custom-mode colors as packed 0xRRGGBB (opaque).
inline constexpr unsigned tekBeamColor = 0x33FF66u; // green phosphor
inline constexpr unsigned tekBackgroundColor = 0x0A0A0Au; // near-black
// Canonical hex spellings for the packed defaults above (parse + renderer must agree).
inline constexpr const char* tekBeamColorHex = "#33ff66";
inline constexpr const char* tekBackgroundColorHex = "#0a0a0a";

// [appearance]
inline constexpr double backgroundOpacity = 1.0;
inline constexpr bool shouldUseBackgroundVibrancy = true;
inline constexpr double blurRadius = 20.0; // pt
inline constexpr double backgroundImageOpacity = 1.0;
// Translucency for default-background cells only (SGR-colored cells stay solid).
// 1.0 = follow background_opacity uniformly.
inline constexpr double defaultBackgroundOpacity = 1.0;
inline constexpr BackgroundImageMode backgroundImageMode = BackgroundImageMode::Fill;
inline constexpr bool squareBottomCorners = true;
inline constexpr TitlebarStyle titlebarStyle = TitlebarStyle::Themed;

// Which palette a spawn draws. Auto takes the native palette where the
// frontend has one and the in-grid palette everywhere else, so no frontend
// is left with a setting that opens nothing.
inline constexpr BeamSurface beamSurface = BeamSurface::Auto;
inline constexpr bool paneTitleBars = true;
// On, because a reader who runs `sz` expects the download rather than a screen
// of binary frame. It is a setting because the watch judges every byte the host
// sends, and a session that will never see a handshake pays for the reading.
inline constexpr bool shouldWatchForZModem = true;
inline constexpr bool blinkAnimation = true; // cursor/text blink fades; false = hard on/off
inline constexpr bool snapWindowResize = false;
inline constexpr bool hideTitleBar = false; // macOS: chromeless window (hide title bar + traffic lights)
inline constexpr bool colorSpaceP3 = true;
// On, which is what the shipped template documents and what
// AppearanceMode.h resolves to. The three agree, because a reader that
// disagreed would make the answer depend on which one a frontend called.
inline constexpr bool followSystemAppearance = true;
// CVD-safe palette remap off by default; theme palette is used verbatim.
inline constexpr Accessibility::ColorblindMode colorblindMode = Accessibility::ColorblindMode::None;
// Chrome (tabs/HUD/settings) font multiplier; 1.0 = system size, no scaling.
inline constexpr double uiFontScale = 1.0;
// Half strength: a tag reads as its colour without outshining the title it
// sits beside, and a translucent window's blur still shows through it.
inline constexpr double tagColorOpacity = 0.5;

/// The `[appearance]` padding edges a target uses where neither the per-edge
/// key nor the `padding` shorthand is set.
///
/// A desktop window is 6 pt at the sides and bottom and 0 at the top, where the
/// title bar already separates the text from the edge. iOS is 4 pt on every
/// edge: a phone's screen has less room to give, and nothing above the pane's
/// first row separates it. The iOS pane is laid out inside the safe area, so
/// these 4 pt are measured from the safe area's edge, in addition to it.
///
/// - Parameter target: The kind of frontend asking.
/// - Returns: The four edges, in points.
[[nodiscard]] constexpr Values::PaddingEdges paddingEdges(DefaultsTarget target)
{
    switch (target) {
    case DefaultsTarget::IOS:
        return { .left = 4.0, .right = 4.0, .top = 4.0, .bottom = 4.0 };
    case DefaultsTarget::Desktop:
        break;
    }
    return { .left = 6.0, .right = 6.0, .top = 0.0, .bottom = 6.0 };
}

/// The desktop's built-in for the `padding` shorthand, which is its side and
/// bottom edge.
inline constexpr double padding = paddingEdges(DefaultsTarget::Desktop).left;
// Chromeless (hide_title_bar) content top inset. Unset → falls back to the
// effective padding_bottom so the titleless content is vertically symmetric by
// default; this constant only backs that fallback when neither is configured.
inline constexpr double titlelessPaddingTop = paddingEdges(currentDefaultsTarget()).bottom;

// [cursor]
inline constexpr CursorShape cursorShape = CursorShape::Bar;
inline constexpr bool cursorBlink = true;
inline constexpr int cursorLinger = 40; // frames solid after typing
inline constexpr int cursorBlinkRate = 1000; // blink period in ms (one full cycle)
inline constexpr int cursorAlpha = 255; // 0..255
inline constexpr bool cursorInvertText = true;
// Cursor glide toward its new cell. Off by default (opt-in). DISTANCE-based:
// Short glides small moves / snaps long jumps, Long is the inverse, Always glides
// every move. Reduce Motion snaps it instant (see CursorMotion.h).
inline constexpr CursorMotionMode cursorMotion = CursorMotionMode::Off;
inline constexpr int cursorMotionDuration = 110; // ms to reach the target cell

// [render] — present-loop pacing. 0 = uncapped (panel native refresh); >0 caps
// the continuous-output frame rate.
inline constexpr int maxFPS = 0;
// VSync on (default, tear-free) presents through CADisplayLink; off disables sync.
inline constexpr bool vsync = true;
// The cwd folder proxy icon in the title bar. On, and the accessor is what makes
// deleting the key restore that rather than leaving the last state standing.
inline constexpr bool folderIcon = true; // bool-ok: every default here mirrors its field name exactly, and a predicate prefix would break the mirror
inline constexpr bool folderIconAlways = false; // bool-ok: mirrors its field name, as every default here does
// A program's icon before its name in window, tab and pane titles.
inline constexpr bool processIcons = true; // bool-ok: mirrors its field name, as every default here does
// The programs whose terminal shows another place, or none: a title leaves the
// working directory out while one of them holds it.
inline constexpr const char* directoryHiddenFor = "tmux, tmate, zellij, screen, ssh, mosh-client, et";
inline constexpr bool directoryHiddenInFullScreen = false; // bool-ok: mirrors its field name, as every default here does

// [bell]
inline constexpr bool bellAudible = true;
inline constexpr bool bellVisual = false;
inline constexpr double bellVisualIntensity = 0.35; // == bellFlashPeakDefault

// [sound]
inline constexpr bool soundAudible = true; // bool-ok: the name mirrors the Values field one-for-one, as bellAudible above it does; renaming this one breaks the mirror the file is
inline constexpr double soundVolume = 1.0;

// [selection]
inline constexpr bool copyOnSelect = false;
// block_select_modifier: the chord that makes a drag rectangular, matched
// exactly. Option alone, because it is clear of Shift — which reaches a
// selection through an application that is tracking the mouse — and of a plain
// drag. Control is offered too and warned about: a Control-drag is how macOS
// raises the contextual menu.
inline constexpr const char* blockSelectModifier = "opt";
inline constexpr const char* selectModifier = "shift";
inline constexpr const char* closeIgnoredProcesses = "screen, tmux, less, more, view, mandoc, tail, log, top, htop";
inline constexpr const char* closeConfirmation = "running";
// links_override_reporting: link hover + click-open work without a modifier even
// under mouse reporting (default on). Off restores the modifier-gated behavior.
inline constexpr bool linksOverrideReporting = true;
// osc8_click_opens: a plain, unmodified click opens an OSC-8-stamped link. Read
// by macOS and Linux alike, so one setting and one default decide the behaviour
// on both. An ssh, sftp, ftp or ftps link needs the modifier even so, since it
// connects to a host the program named (LinkRun::requiresLinkModifier). A
// regex-guessed link needs the modifier — the program never said that text was
// a URL, the terminal decided it. The one exception is on macOS:
// a strict http(s) guess under mouse reporting with links_override_reporting,
// which Input::plainClickOpenableUnderReporting admits. Linux opens no link
// while a program reports the mouse. Either way the target then passes
// LinkRun's rules for what an opener may be handed.
inline constexpr bool osc8ClickOpens = true;
inline constexpr bool restrictSelectionToText = false;

// [paste] — multi-line paste confirmation. On by default; threshold 1 = confirm
// only when the paste spans more than one line.
inline constexpr bool confirmMultilinePaste = true;
inline constexpr int confirmMultilinePasteThreshold = 1; // lines

// [clipboard] remote_tools. Off, because the shims expose the user's own
// pasteboard to every process on the host that runs xclip — a disclosure on a
// shared machine, and one nobody should make by having installed a terminal.
inline constexpr RemoteClipboardTools remoteClipboardTools = RemoteClipboardTools::Off;

// [mouse]
inline constexpr double wheelSpeed = 1.0;
inline constexpr int wheelLines = 1;
// trackpadSpeed / scrollbackLines reuse the named constants already declared in
// Preferences.h so there is exactly one literal for each.
inline constexpr double trackpadSpeed = defaultTrackpadSpeed;
inline constexpr bool scrollBounce = true; // bool-ok: matches the boolean configuration key and its accessor.

// [tabs]
inline constexpr TabShortcutHints tabShortcutHints = TabShortcutHints::Always;

// [window] — drag-from-any-cell modifier. Default clear of ⌘-click and option-drag.
inline constexpr const char* windowDragModifier = "control+command";
// initial_columns / initial_rows — 0 means "no pin; use saved frame / compile-time default."
inline constexpr int windowInitialColumns = 0;
inline constexpr int windowInitialRows = 0;

/// The supported accessory symbols in their default order; an empty override hides them.
inline constexpr const char* keyboardAccessorySymbols = "~/|-_\\`><*$&;='\"[]{}";

// [keys] compose_key — terminal-decoded compose trigger. Empty = disabled.
inline constexpr const char* composeKey = "";
// [links] line_handler — no program named, so a file link that names a line
// opens the way any file link does.
inline constexpr const char* linkLineHandler = "";
// [keys] option_as_alt — no Option key is Meta, so every layout types its own
// characters.
inline constexpr OptionAsAlternate optionAsAlternate = OptionAsAlternate::None;

// [quake] — drop-down terminal. Opt-in; the rest only take effect when on.
inline constexpr bool quakeEnabled = false;
inline constexpr const char* quakeHotkey = "control+grave";
inline constexpr double quakeHeight = 0.5; // half the active screen
inline constexpr bool quakeAnimate = true;

// [dock]
inline constexpr bool dockBounce = true;
inline constexpr bool dockBadge = true;

// [security] secure_keyboard_entry — hold macOS Secure Keyboard Entry while
// this app is frontmost, so no other process can observe keystrokes. Default
// on: a terminal is where passwords, tokens and keys are typed, and the cost of
// holding it is a system-wide input mode this app already balances by count.
// A reader who needs another app to see the keys turns it off in one line.
inline constexpr bool secureKeyboardEntry = true; // bool-ok: mirrors a boolean config key
// [security] password_detect — password-prompt heuristic; default on: a
// "Password:" row is the moment the lock and the echo mask earn their keep.
inline constexpr bool passwordDetect = true; // bool-ok: mirrors a boolean config key
// [security] secure_keyboard_entry_at_password_prompts — hold Secure Keyboard
// Entry while a pane is at a password prompt (mode 7401 or the heuristic);
// default on: the prompt names the moment a keystroke is a secret.
inline constexpr bool secureKeyboardEntryAtPasswordPrompts = true; // bool-ok: mirrors a boolean config key

// [control] — local remote-control socket; off by default (security gate).
inline constexpr bool controlEnabled = false;
// A confirm verdict on a socket request stands as a refusal unless the user says otherwise.
inline constexpr bool controlApprove = false; // bool-ok: mirrors a boolean config key

// [experimental]
inline constexpr bool sessionPersistence = false;

// [notifications]
inline constexpr bool notifications = true;
inline constexpr bool notificationSound = true;

// [debug] — build-identity badge in the titlebar. Off by default: the hash
// distinguishes A/B builds for a developer and is noise for a normal user.
inline constexpr bool debugShowBuildHash = false; // bool-ok: mirrors a boolean config key

// [updates] check_automatically — the updater's daily check. On by default: a
// check that finds a release only shows it, and the person decides whether to
// install it, so what the default costs is one request a day.
inline constexpr bool shouldCheckForUpdatesAutomatically = true;

// [triggers]
// SECURITY: run_command gated OFF by default; never spawns until allow_run_command = true.
inline constexpr bool allowRunCommand = false;
// SECURITY: webhook gated OFF by default; never POSTs until allow_webhook = true.
inline constexpr bool allowWebhook = false;

// [scripting]
// SECURITY: config_script gated OFF by default; won't run until enable = true.
inline constexpr bool scriptingEnable = false;

// [remote]
inline constexpr bool autoDeployHostOnRemoteConnect = true;
inline constexpr bool shouldPersistRemoteHistory = false;

// [network] — proxy off by default; agent forwarding opt-in.
inline constexpr bool sshForwardAgent = false;
inline constexpr int proxyPort = 0;

// [transfer] — ZMODEM auto-receive on by default (the save-location confirmation
// dialog is the real security gate; auto only skips the "start the receiver?" ask).
inline constexpr bool transferAutoReceive = true;

// [terminal]
inline constexpr int scrollbackLines = defaultScrollbackLines;
// DECCOLM (80/132) resizes the window (xterm c132); false renders in place.
// Always in-place when fullscreen.
inline constexpr bool columnModeResizesWindow = true;
// East_Asian_Width=Ambiguous width: 1 cell (modern non-CJK); 2 = legacy CJK double-width.
inline constexpr int ambiguousWidth = 1;

// [unicode] normalization — none by default; search/copy use text as stored.
inline constexpr const char* normalization = "none";

} // namespace Ubiquitty::Preferences::Defaults
