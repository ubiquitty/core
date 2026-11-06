// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Declarative WRITE recipe for every settings key a frontend persists. Each row
// is a [section] + key + toml* serializer; `descriptorFor(key)` yields the row and
// `serializeWrite(kind, value)` produces the TOML literal. Pure header (no platform types).

#include <optional>
#include <string>
#include <string_view>

#include "Core/Preferences/Preferences.h"

namespace Ubiquitty::Preferences {

/// How a preference's value is rendered into its TOML literal.
///
/// The kind travels with the key rather than with the value, because two
/// preferences of the same C++ type can want different literals: an enum's
/// canonical token and a person's free text are both strings here and are not
/// interchangeable in the file.
enum class SerializeKind {
    Bool, // tomlBool — a yes/no toggle
    Int, // tomlInt — a whole-number stepper / index
    Double, // tomlDouble — a fractional slider/stepper
    EnumToken, // tomlString of an enum's canonical token (closed vocabulary, not free text)
    String, // tomlString of arbitrary user text
    LegacyBoolOrToken, // [clipboard] remote_tools: a bool a toggle edits, or a token a person typed
};

/// Every preference a frontend can persist, as a stable handle.
///
/// The handle rather than the strings, so a caller cannot invent a section or
/// misspell a key: the two strings and the serializer come from one table,
/// through ``descriptorFor``. Enumerators may be added and must not be
/// renumbered by hand — nothing stores the numeric value, but the shipped
/// reference is generated against this order.
enum class Key {
    // [appearance]
    AppearanceFollowSystem,
    AppearanceTheme,
    AppearanceSystemLightTheme,
    AppearanceSystemDarkTheme,
    AppearanceBackgroundOpacity,
    AppearanceTextContrastOnBlur,
    AppearanceTextContrast,
    AppearanceBackgroundVibrancy,
    AppearanceBlurRadius,
    AppearanceBackgroundImage,
    AppearanceBackgroundImageOpacity,
    AppearanceBackgroundImageMode,
    AppearancePaddingLeft,
    AppearancePaddingRight,
    AppearancePaddingTop,
    AppearancePaddingBottom,
    AppearanceTitlelessPaddingTop,
    AppearanceSquareBottomCorners,
    AppearanceFolderIcon,
    AppearanceFolderIconAlways,
    AppearanceProcessIcons,
    AppearanceTitlebarStyle,
    AppearanceColorSpace,
    AppearancePaneTitleBars,
    AppearanceBlinkAnimation,
    AppearanceSnapWindowResize,
    AppearanceHideTitleBar,
    AppearanceColorblindMode,
    AppearanceShadeBlocks,
    AppearanceUIFontScale,
    AppearanceTagColorOpacity,

    // [font]
    FontFamily,
    FontProportionalFamily,
    FontFallback,
    FontSymbolFallback,
    FontEmojiFallback,
    FontCJKFallback,
    FontFeatures,
    FontVariations,
    FontSize,
    FontProportionalSize,
    FontLigatures,
    FontLineSpacing,
    FontCharacterSpacing,
    FontWeight,
    FontBoldWeight,
    FontWidth,
    FontBaselineOffset,
    FontWideGlyphBoost,
    FontMatchFallbackSize,
    FontBrailleGrid,
    FontBrailleStyle,
    FontWideGlyphScale,

    // [underline]
    UnderlineLinks,
    UnderlineLinkStyle,
    UnderlineLinkColorBlue,
    UnderlineLinkStylePerKind,
    UnderlineThickness,
    UnderlineOffset,
    UnderlineSkipInk,
    UnderlineFollowsInk,
    UnderlineSkipInkPad,

    // [text]
    TextBrightenBold,
    AppearanceBeamTerminalFont,

    // [tek]
    TekBrightPen,
    TekGlowVectors,
    TekColorMode,
    TekBeamColor,
    TekBackgroundColor,

    // [selection]
    SelectionSmartDoubleClick,
    SelectionCopyOnSelect,
    SelectionLinksOverrideReporting,
    SelectionOSC8ClickOpens,
    SelectionRestrictToText,
    SelectionCopyStyle,
    SelectionCopyStyleTheme,
    SelectionBlockSelectModifier,
    SelectionSelectModifier,
    SelectionStyle,
    TerminalCloseIgnoredProcesses,
    TerminalCloseConfirmation,

    // [paste]
    PasteConfirmMultiline,
    PasteConfirmMultilineThreshold,

    // [keys]
    KeysAccessorySymbols,

    // [clipboard]
    ClipboardRemoteTools,

    // [mouse]
    MouseWheelSpeed,
    MouseWheelLines,
    MouseTrackpadSpeed,
    MouseScrollBounce,

    // [terminal]
    TerminalScrollbackLines,
    TerminalEncoding,
    TerminalColumnModeResizesWindow,
    TerminalAmbiguousWidth,

    // [title]
    TitleWindowDirectory,
    TitleWindowDirectoryPath,
    TitleWindowAppTitle,
    TitleWindowProcess,
    TitleWindowProcessArguments,
    TitleWindowShellCommand,
    TitleWindowTTYName,
    TitleWindowProfileName,
    TitleWindowText,
    TitleWindowDimensions,
    TitleWindowProgramStatus,
    TitleTabDirectory,
    TitleTabDirectoryPath,
    TitleTabAppTitle,
    TitleTabProcess,
    TitleTabProcessArguments,
    TitleTabShellCommand,
    TitleTabTTYName,
    TitleTabProfileName,
    TitleTabText,
    TitleTabDimensions,
    TitleTabProgramStatus,
    TitlePaneDirectory,
    TitlePaneDirectoryPath,
    TitlePaneAppTitle,
    TitlePaneProcess,
    TitlePaneProcessArguments,
    TitlePaneShellCommand,
    TitlePaneTTYName,
    TitlePaneProfileName,
    TitlePaneText,
    TitlePaneDimensions,
    TitlePaneProgramStatus,
    TitleDirectoryHiddenFor,
    TitleDirectoryHiddenInFullScreen,

    // [cursor]
    CursorShape,
    CursorBlink,
    CursorLinger,
    CursorBlinkRate,
    CursorAlpha,
    CursorInvertText,
    CursorMotion,

    // [bell]
    BellAudible,
    BellVisual,

    // [tabs]
    TabsShowShortcuts,

    // [window]
    WindowDragModifier,
    WindowInitialColumns,
    WindowInitialRows,

    // [quake]
    QuakeEnabled,
    QuakeHotkey,
    QuakeHeight,
    QuakeAnimate,

    // [sound]
    SoundAudible,
    SoundVolume,

    // [notifications]
    NotificationsEnabled,
    NotificationsSound,

    // [remote]
    RemoteAutoInstallHost,
    RemoteHistoryCacheOnDisk,
    RemoteNonOwnerPresentation,

    // [dock]
    DockBounce,
    DockBadge,

    // [security]
    SecuritySecureKeyboardEntry,
    SecurityPasswordDetect,
    SecuritySecureKeyboardEntryAtPasswordPrompts,

    // [control]
    ControlEnable,
    ControlSocketPath,
    ControlToken,
    ControlApprove,

    // [experimental]
    ExperimentalSessionPersistence,

    // [debug]
    DebugShowBuildHash,

    // [updates]
    UpdatesCheckAutomatically,

    // [term]
    TermProgram,
    TermVersion,

    // [transfer]
    TransferZModem,
};

/// One row of the write recipe: where a preference lands and how it is spelled.
///
/// `section` and `key` are the strings a person sees in their file, and they are
/// the half of this table that must never follow a rename of the code around it.
struct WriteDescriptor {
    Key id;
    const char* section;
    const char* key;
    SerializeKind kind;
};

// Seven rows say `Double` for a setting a person usually types whole. They are
// the seven whose FIELD is a double — `[font] size`, `line_spacing`, and the
// five paddings — and the parse, the accessor and `effectiveValue` all answer a
// double for them. A row saying Int would write a value read as 15.5 back as 15,
// losing the fraction on a round trip that looks lossless. The cost is that a
// whole size writes as `15.0` rather than `15`; both parse to the same number,
// and the alternative is a table that disagrees with every other layer about
// what these keys hold.
inline constexpr WriteDescriptor writeDescriptors[] = {
    // [appearance]
    { Key::AppearanceFollowSystem, "appearance", "follow_system", SerializeKind::Bool },
    { Key::AppearanceTheme, "appearance", "theme", SerializeKind::String },
    { Key::AppearanceSystemLightTheme, "appearance", "system_light_theme", SerializeKind::String },
    { Key::AppearanceSystemDarkTheme, "appearance", "system_dark_theme", SerializeKind::String },
    { Key::AppearanceBackgroundOpacity, "appearance", "background_opacity", SerializeKind::Double },
    { Key::AppearanceTextContrastOnBlur, "appearance", "text_contrast_on_blur", SerializeKind::Bool },
    { Key::AppearanceTextContrast, "appearance", "text_contrast", SerializeKind::Bool },
    { Key::AppearanceBackgroundVibrancy, "appearance", "background_vibrancy", SerializeKind::Bool },
    { Key::AppearanceBlurRadius, "appearance", "blur_radius", SerializeKind::Double },
    { Key::AppearanceBackgroundImage, "appearance", "background_image", SerializeKind::String },
    { Key::AppearanceBackgroundImageOpacity, "appearance", "background_image_opacity", SerializeKind::Double },
    { Key::AppearanceBackgroundImageMode, "appearance", "background_image_mode", SerializeKind::EnumToken },
    { Key::AppearancePaddingLeft, "appearance", "padding_left", SerializeKind::Double },
    { Key::AppearancePaddingRight, "appearance", "padding_right", SerializeKind::Double },
    { Key::AppearancePaddingTop, "appearance", "padding_top", SerializeKind::Double },
    { Key::AppearancePaddingBottom, "appearance", "padding_bottom", SerializeKind::Double },
    { Key::AppearanceTitlelessPaddingTop, "appearance", "titleless_padding_top", SerializeKind::Double },
    { Key::AppearanceSquareBottomCorners, "appearance", "square_bottom_corners", SerializeKind::Bool },
    { Key::AppearanceFolderIcon, "appearance", "folder_icon", SerializeKind::Bool },
    { Key::AppearanceFolderIconAlways, "appearance", "folder_icon_always", SerializeKind::Bool },
    { Key::AppearanceProcessIcons, "appearance", "process_icons", SerializeKind::Bool },
    { Key::AppearanceTitlebarStyle, "appearance", "titlebar_style", SerializeKind::EnumToken },
    { Key::AppearanceColorSpace, "appearance", "color_space", SerializeKind::EnumToken },
    { Key::AppearancePaneTitleBars, "appearance", "pane_title_bars", SerializeKind::Bool },
    { Key::AppearanceBlinkAnimation, "appearance", "blink_animation", SerializeKind::Bool },
    { Key::AppearanceSnapWindowResize, "appearance", "snap_window_resize", SerializeKind::Bool },
    { Key::AppearanceHideTitleBar, "appearance", "hide_title_bar", SerializeKind::Bool },
    { Key::AppearanceColorblindMode, "appearance", "colorblind_mode", SerializeKind::EnumToken },
    { Key::AppearanceShadeBlocks, "appearance", "shade_blocks", SerializeKind::EnumToken },
    { Key::AppearanceUIFontScale, "appearance", "ui_font_scale", SerializeKind::Double },
    { Key::AppearanceTagColorOpacity, "appearance", "tag_color_opacity", SerializeKind::Double },

    // [font]
    { Key::FontFamily, "font", "family", SerializeKind::String },
    { Key::FontProportionalFamily, "font", "proportional_family", SerializeKind::String },
    { Key::FontFallback, "font", "fallback", SerializeKind::String },
    { Key::FontSymbolFallback, "font", "symbol_fallback", SerializeKind::String },
    { Key::FontEmojiFallback, "font", "emoji_fallback", SerializeKind::String },
    { Key::FontCJKFallback, "font", "cjk_fallback", SerializeKind::String },
    { Key::FontFeatures, "font", "features", SerializeKind::String },
    { Key::FontVariations, "font", "variations", SerializeKind::String },
    { Key::FontSize, "font", "size", SerializeKind::Double },
    { Key::FontProportionalSize, "font", "proportional_size", SerializeKind::Double },
    { Key::FontLigatures, "font", "ligatures", SerializeKind::Bool },
    { Key::FontLineSpacing, "font", "line_spacing", SerializeKind::Double },
    { Key::FontCharacterSpacing, "font", "character_spacing", SerializeKind::Double },
    { Key::FontWeight, "font", "weight", SerializeKind::Int },
    { Key::FontBoldWeight, "font", "bold_weight", SerializeKind::Int },
    { Key::FontWidth, "font", "width", SerializeKind::Double },
    { Key::FontBaselineOffset, "font", "baseline_offset", SerializeKind::Double },
    { Key::FontWideGlyphBoost, "font", "wide_glyph_boost", SerializeKind::Bool },
    { Key::FontMatchFallbackSize, "font", "match_fallback_size", SerializeKind::Bool },
    { Key::FontBrailleGrid, "font", "braille_grid", SerializeKind::Bool },
    { Key::FontBrailleStyle, "font", "braille_style", SerializeKind::EnumToken },
    { Key::FontWideGlyphScale, "font", "wide_glyph_scale", SerializeKind::Double },

    // [underline]
    { Key::UnderlineLinks, "underline", "links", SerializeKind::Bool },
    { Key::UnderlineLinkStyle, "underline", "link_style", SerializeKind::Int },
    { Key::UnderlineLinkColorBlue, "underline", "link_color_blue", SerializeKind::Bool },
    { Key::UnderlineLinkStylePerKind, "underline", "link_style_per_kind", SerializeKind::Bool },
    { Key::UnderlineThickness, "underline", "thickness", SerializeKind::Double },
    { Key::UnderlineOffset, "underline", "offset", SerializeKind::Double },
    { Key::UnderlineSkipInk, "underline", "skip_ink", SerializeKind::Bool },
    { Key::UnderlineFollowsInk, "underline", "follows_ink", SerializeKind::Bool },
    { Key::UnderlineSkipInkPad, "underline", "skip_ink_pad", SerializeKind::Double },

    // [text]
    { Key::TextBrightenBold, "text", "brighten_bold", SerializeKind::Bool },
    { Key::AppearanceBeamTerminalFont, "appearance", "beam_terminal_font", SerializeKind::Bool },

    // [tek]
    { Key::TekBrightPen, "tek", "bright_pen", SerializeKind::Bool },
    { Key::TekGlowVectors, "tek", "glow_vectors", SerializeKind::Bool },
    { Key::TekColorMode, "tek", "color_mode", SerializeKind::Int },
    { Key::TekBeamColor, "tek", "beam_color", SerializeKind::String },
    { Key::TekBackgroundColor, "tek", "background_color", SerializeKind::String },

    // [selection]
    { Key::SelectionSmartDoubleClick, "selection", "smart_double_click", SerializeKind::Bool },
    { Key::SelectionCopyOnSelect, "selection", "copy_on_select", SerializeKind::Bool },
    { Key::SelectionLinksOverrideReporting, "selection", "links_override_reporting", SerializeKind::Bool },
    { Key::SelectionBlockSelectModifier, "selection", "block_select_modifier", SerializeKind::EnumToken },
    { Key::SelectionSelectModifier, "selection", "select_modifier", SerializeKind::EnumToken },
    { Key::SelectionStyle, "selection", "style", SerializeKind::EnumToken },
    { Key::TerminalCloseIgnoredProcesses, "terminal", "close_ignored_processes", SerializeKind::String },
    { Key::TerminalCloseConfirmation, "terminal", "close_confirmation", SerializeKind::EnumToken },
    { Key::SelectionOSC8ClickOpens, "selection", "osc8_click_opens", SerializeKind::Bool },
    { Key::SelectionRestrictToText, "selection", "restrict_to_text", SerializeKind::Bool },
    { Key::SelectionCopyStyle, "selection", "copy_style", SerializeKind::String },
    { Key::SelectionCopyStyleTheme, "selection", "copy_style_theme", SerializeKind::String },

    // [paste]
    { Key::PasteConfirmMultiline, "paste", "confirm_multiline", SerializeKind::Bool },
    { Key::PasteConfirmMultilineThreshold, "paste", "confirm_multiline_threshold", SerializeKind::Int },

    // [keys]
    { Key::KeysAccessorySymbols, "keys", "accessory_symbols", SerializeKind::String },

    // [clipboard]
    // LegacyBoolOrToken, not String: the parser takes `true`/`false` for the
    // obvious reading as well as the names. A String descriptor would let a
    // writer emit a quoted bool the reader then takes as a token it does not
    // know.
    { Key::ClipboardRemoteTools, "clipboard", "remote_tools", SerializeKind::LegacyBoolOrToken },

    // [mouse]
    { Key::MouseWheelSpeed, "mouse", "wheel_speed", SerializeKind::Double },
    { Key::MouseWheelLines, "mouse", "wheel_lines", SerializeKind::Int },
    { Key::MouseTrackpadSpeed, "mouse", "trackpad_speed", SerializeKind::Double },
    { Key::MouseScrollBounce, "mouse", "scroll_bounce", SerializeKind::Bool },

    // [terminal]
    { Key::TerminalScrollbackLines, "terminal", "scrollback_lines", SerializeKind::Int },
    { Key::TerminalEncoding, "terminal", "encoding", SerializeKind::EnumToken },
    { Key::TerminalColumnModeResizesWindow, "terminal", "column_mode_resizes_window", SerializeKind::Bool },
    { Key::TerminalAmbiguousWidth, "terminal", "ambiguous_width", SerializeKind::Int },

    // [title]
    { Key::TitleWindowDirectory, "title", "window_directory", SerializeKind::Bool },
    { Key::TitleWindowDirectoryPath, "title", "window_directory_path", SerializeKind::Bool },
    { Key::TitleWindowAppTitle, "title", "window_app_title", SerializeKind::Bool },
    { Key::TitleWindowProcess, "title", "window_process", SerializeKind::Bool },
    { Key::TitleWindowProcessArguments, "title", "window_process_arguments", SerializeKind::Bool },
    { Key::TitleWindowShellCommand, "title", "window_shell_command", SerializeKind::Bool },
    { Key::TitleWindowTTYName, "title", "window_tty_name", SerializeKind::Bool },
    { Key::TitleWindowProfileName, "title", "window_profile_name", SerializeKind::Bool },
    { Key::TitleWindowText, "title", "window_text", SerializeKind::String },
    { Key::TitleWindowDimensions, "title", "window_dimensions", SerializeKind::Bool },
    { Key::TitleWindowProgramStatus, "title", "window_program_status", SerializeKind::Bool },
    { Key::TitleTabDirectory, "title", "tab_directory", SerializeKind::Bool },
    { Key::TitleTabDirectoryPath, "title", "tab_directory_path", SerializeKind::Bool },
    { Key::TitleTabAppTitle, "title", "tab_app_title", SerializeKind::Bool },
    { Key::TitleTabProcess, "title", "tab_process", SerializeKind::Bool },
    { Key::TitleTabProcessArguments, "title", "tab_process_arguments", SerializeKind::Bool },
    { Key::TitleTabShellCommand, "title", "tab_shell_command", SerializeKind::Bool },
    { Key::TitleTabTTYName, "title", "tab_tty_name", SerializeKind::Bool },
    { Key::TitleTabProfileName, "title", "tab_profile_name", SerializeKind::Bool },
    { Key::TitleTabText, "title", "tab_text", SerializeKind::String },
    { Key::TitleTabDimensions, "title", "tab_dimensions", SerializeKind::Bool },
    { Key::TitleTabProgramStatus, "title", "tab_program_status", SerializeKind::Bool },
    { Key::TitlePaneDirectory, "title", "pane_directory", SerializeKind::Bool },
    { Key::TitlePaneDirectoryPath, "title", "pane_directory_path", SerializeKind::Bool },
    { Key::TitlePaneAppTitle, "title", "pane_app_title", SerializeKind::Bool },
    { Key::TitlePaneProcess, "title", "pane_process", SerializeKind::Bool },
    { Key::TitlePaneProcessArguments, "title", "pane_process_arguments", SerializeKind::Bool },
    { Key::TitlePaneShellCommand, "title", "pane_shell_command", SerializeKind::Bool },
    { Key::TitlePaneTTYName, "title", "pane_tty_name", SerializeKind::Bool },
    { Key::TitlePaneProfileName, "title", "pane_profile_name", SerializeKind::Bool },
    { Key::TitlePaneText, "title", "pane_text", SerializeKind::String },
    { Key::TitlePaneDimensions, "title", "pane_dimensions", SerializeKind::Bool },
    { Key::TitlePaneProgramStatus, "title", "pane_program_status", SerializeKind::Bool },
    { Key::TitleDirectoryHiddenFor, "title", "directory_hidden_for", SerializeKind::String },
    { Key::TitleDirectoryHiddenInFullScreen, "title", "directory_hidden_in_full_screen", SerializeKind::Bool },

    // [cursor]
    { Key::CursorShape, "cursor", "shape", SerializeKind::EnumToken },
    { Key::CursorBlink, "cursor", "blink", SerializeKind::Bool },
    { Key::CursorLinger, "cursor", "linger", SerializeKind::Int },
    { Key::CursorBlinkRate, "cursor", "blink_rate", SerializeKind::Int },
    { Key::CursorAlpha, "cursor", "alpha", SerializeKind::Int },
    { Key::CursorInvertText, "cursor", "invert_text", SerializeKind::Bool },
    { Key::CursorMotion, "cursor", "motion", SerializeKind::EnumToken },

    // [bell]
    { Key::BellAudible, "bell", "audible", SerializeKind::Bool },
    { Key::BellVisual, "bell", "visual", SerializeKind::Bool },

    // [tabs]
    { Key::TabsShowShortcuts, "tabs", "show_shortcuts", SerializeKind::EnumToken },

    // [window]
    { Key::WindowDragModifier, "window", "drag_modifier", SerializeKind::EnumToken },
    { Key::WindowInitialColumns, "window", "initial_columns", SerializeKind::Int },
    { Key::WindowInitialRows, "window", "initial_rows", SerializeKind::Int },

    // [quake]
    { Key::QuakeEnabled, "quake", "enabled", SerializeKind::Bool },
    { Key::QuakeHotkey, "quake", "hotkey", SerializeKind::String },
    { Key::QuakeHeight, "quake", "height", SerializeKind::Double },
    { Key::QuakeAnimate, "quake", "animate", SerializeKind::Bool },

    // [sound]
    { Key::SoundAudible, "sound", "audible", SerializeKind::Bool },
    { Key::SoundVolume, "sound", "volume", SerializeKind::Double },

    // [notifications]
    { Key::NotificationsEnabled, "notifications", "enabled", SerializeKind::Bool },
    { Key::NotificationsSound, "notifications", "sound", SerializeKind::Bool },

    // [remote]
    { Key::RemoteAutoInstallHost, "remote", "auto_install_host", SerializeKind::Bool },
    { Key::RemoteHistoryCacheOnDisk, "remote", "history_cache_on_disk", SerializeKind::Bool },
    { Key::RemoteNonOwnerPresentation, "remote", "non_owner_presentation", SerializeKind::EnumToken },

    // [dock]
    { Key::DockBounce, "dock", "bounce", SerializeKind::Bool },
    { Key::DockBadge, "dock", "badge", SerializeKind::Bool },

    // [security]
    { Key::SecuritySecureKeyboardEntry, "security", "secure_keyboard_entry", SerializeKind::Bool },
    { Key::SecurityPasswordDetect, "security", "password_detect", SerializeKind::Bool },
    { Key::SecuritySecureKeyboardEntryAtPasswordPrompts, "security", "secure_keyboard_entry_at_password_prompts", SerializeKind::Bool },

    // [control] — local remote-control socket; serialized so the gate persists.
    { Key::ControlEnable, "control", "enable", SerializeKind::Bool },
    { Key::ControlSocketPath, "control", "socket_path", SerializeKind::String },
    { Key::ControlToken, "control", "token", SerializeKind::String },
    { Key::ControlApprove, "control", "approve", SerializeKind::Bool },

    // [experimental]
    { Key::ExperimentalSessionPersistence, "experimental", "session_persistence", SerializeKind::Bool },

    // [debug]
    { Key::DebugShowBuildHash, "debug", "show_build_hash", SerializeKind::Bool },

    // [updates]
    { Key::UpdatesCheckAutomatically, "updates", "check_automatically", SerializeKind::Bool },

    // [term]
    { Key::TermProgram, "term", "program", SerializeKind::String },
    { Key::TermVersion, "term", "version", SerializeKind::String },

    // [transfer] — the ZMODEM handshake watch. Serialized so a reader who turns
    // the scan off keeps it off.
    { Key::TransferZModem, "transfer", "zmodem", SerializeKind::Bool },
};

inline constexpr int writeDescriptorCount = static_cast<int>(sizeof(writeDescriptors) / sizeof(writeDescriptors[0]));

/// The write recipe for one preference.
///
/// The table is exhaustive over ``Key``, so a valid enumerator always resolves;
/// the null answer exists for a value cast in from outside the enumeration.
///
/// - Parameter id: The preference to look up.
/// - Returns: Its row, or nullptr when `id` names no enumerator.
inline constexpr const WriteDescriptor* descriptorFor(Key id)
{
    for (int i = 0; i < writeDescriptorCount; ++i) {
        if (writeDescriptors[i].id == id)
            return &writeDescriptors[i];
    }
    return nullptr;
}

/// Finds a scalar preference by its stable section and key address.
[[nodiscard]] inline std::optional<Key> keyForAddress(const std::string& address)
{
    for (const auto& descriptor : writeDescriptors) {
        if (address == std::string(descriptor.section) + "." + descriptor.key)
            return descriptor.id;
    }
    return std::nullopt;
}

/// Renders a toggle into its TOML literal.
///
/// The four overloads take the kind they already imply, which looks redundant
/// and is the point: a caller passes the descriptor's kind beside the value, so
/// a value whose C++ type has drifted from what the table says fails to compile
/// at the call rather than writing a literal the reader will not parse back.
///
/// - Parameters:
///   - kind: The descriptor's kind, `SerializeKind::Bool` here.
///   - value: The toggle to render.
/// - Returns: The TOML literal, `true` or `false`.
inline std::string serializeWrite(SerializeKind kind, bool value) // bool-ok: the payload being rendered, not a flag steering the call
{
    (void)kind; // Bool
    return tomlBool(value);
}

/// Renders a whole number into its TOML literal.
///
/// - Parameters:
///   - kind: The descriptor's kind, `SerializeKind::Int` here.
///   - value: The number to render.
/// - Returns: The TOML integer literal.
inline std::string serializeWrite(SerializeKind kind, long long value)
{
    (void)kind; // Int
    return tomlInt(value);
}

/// Renders a fractional value into its TOML literal.
///
/// - Parameters:
///   - kind: The descriptor's kind, `SerializeKind::Double` here.
///   - value: The number to render.
/// - Returns: The TOML float literal.
inline std::string serializeWrite(SerializeKind kind, double value)
{
    (void)kind; // Double
    return tomlDouble(value);
}

/// Renders a token or free text into its TOML literal.
///
/// `EnumToken` and `String` share this overload because they share their
/// quoting; what separates them is whether the vocabulary is closed, which the
/// reader enforces rather than the writer.
///
/// - Parameters:
///   - kind: The descriptor's kind, `EnumToken` or `String`.
///   - value: The text to render.
/// - Returns: The quoted TOML string literal.
inline std::string serializeWrite(SerializeKind kind, std::string_view value)
{
    (void)kind; // EnumToken or String
    return tomlString(value);
}

} // namespace Ubiquitty::Preferences
