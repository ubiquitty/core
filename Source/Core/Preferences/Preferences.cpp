// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Minimal TOML-subset parser + Values struct defaults.
// Grammar (strict subset, flat sectioned config):
// line     := blank | comment | section | keyval
// comment  := '#' ... end-of-line
// section  := '[' bare-key ('.' bare-key)* ']'
// keyval   := key '=' value
// value    := string | integer | float | isBoolean
// string   := '"' chars '"'   (with \" \\ \n \t \r escapes)
// isBoolean  := 'true' | 'false'
// A trailing inline `#` comment is honored except inside a quoted string.
// Unrecognized keys/sections are ignored (forward compatibility).

#include "Core/Preferences/Preferences.h"
#include "Core/Cursor/Blink.h"
#include "Core/Preferences/CodePointRange.h"
#include "Core/Preferences/Configuration.h"

#include "Core/Preferences/Internal.h"
#include "Core/Preferences/PreferencesDefaults.h"
#include "Core/Preferences/Schema.h"
#include "Core/Preferences/Vocabulary.h"
#include "Core/Rendering/ShadeStyle.h"
#include "Core/Unicode/ASCII.h"
#include "Core/Unicode/UTF8Encoder.h"
#include "PAL/Log.h"
#include "PAL/PAL.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace Ubiquitty::Preferences {

namespace {

bool iequals(const std::string& a, const char* b) { return Unicode::equalIgnoringASCIICase(a, b); }

// Trim ASCII whitespace from both ends.
std::string trim(const std::string& s)
{
    std::string::size_type b = 0;
    std::string::size_type e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

// One parsed scalar. A single TOML line yields exactly one of these.
struct Value {
    enum class Kind {
        None,
        String,
        Int,
        Float,
        Bool,
        StringArray
    } kind;
    std::string str;
    long long i;
    double f;
    bool isBoolean;
    std::vector<std::string> strs;
    Value()
        : kind(Kind::None)
        , i(0)
        , f(0.0)
        , isBoolean(false)
    {
    }
};

// Decode one hex digit, returning -1 on non-hex input.
int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// A decoded quoted string and where it ended. Returned together because five of
// the seven callers want only the value and were passing an end position they
// never read.
struct QuotedString {
    std::string value;
    std::string::size_type end; // just past the closing quote
};

// Parse a double-quoted string starting at raw[start].
std::optional<QuotedString> parseQuotedString(const std::string& raw, std::string::size_type start)
{
    if (start >= raw.size() || raw[start] != '"')
        return std::nullopt;
    std::string body;
    std::string::size_type i = start + 1;
    for (; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            char n = raw[++i];
            switch (n) {
            case 'n':
                body += '\n';
                break;
            case 't':
                body += '\t';
                break;
            case 'r':
                body += '\r';
                break;
            case '"':
                body += '"';
                break;
            case '\\':
                body += '\\';
                break;
            case 'b':
                body += '\b';
                break;
            case 'f':
                body += '\f';
                break;
            case 'u':
            case 'U': {
                // TOML's scalar escapes. Without them the default arm below
                // drops the backslash and keeps the letters, so `A` is
                // stored as the four characters `u0041` — accepted, and a value
                // the file did not ask for. A rejection is diagnosable; a wrong
                // string is a setting that silently is not the one written.
                const int digits = (n == 'u') ? 4 : 8;
                if (i + static_cast<std::string::size_type>(digits) >= raw.size())
                    return std::nullopt;
                std::uint32_t codePoint = 0;
                for (int digit = 0; digit < digits; ++digit) {
                    const int value = hexDigit(raw[i + 1 + static_cast<std::string::size_type>(digit)]);
                    if (value < 0)
                        return std::nullopt;
                    codePoint = (codePoint << 4) | static_cast<std::uint32_t>(value);
                }
                i += static_cast<std::string::size_type>(digits);
                // A surrogate or an out-of-range scalar is not a character, and
                // appendUTF8 emits nothing for either — which would swallow the
                // escape rather than refuse it.
                if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
                    return std::nullopt;
                Ubiquitty::Unicode::appendUTF8(body, codePoint);
                break;
            }
            default:
                body += n;
                break;
            }
        } else if (c == '"') {
            return QuotedString { std::move(body), i + 1 };
        } else {
            body += c;
        }
    }
    return std::nullopt;
}

bool parseValue(const std::string& raw, Value& v)
{
    if (raw.empty())
        return false;

    // String array: `[ "a", "b", "c" ]`. Only string elements are
    // supported (sufficient for the [[themes]] hex-color list).
    if (raw[0] == '[') {
        std::string::size_type i = 1;
        while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i])))
            ++i;
        v.kind = Value::Kind::StringArray;
        v.strs.clear();
        if (i < raw.size() && raw[i] == ']')
            return true; // empty
        for (;;) {
            if (i >= raw.size() || raw[i] != '"')
                return false;
            std::optional<QuotedString> element = parseQuotedString(raw, i);
            if (!element)
                return false;
            i = element->end;
            v.strs.push_back(std::move(element->value));
            while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i])))
                ++i;
            if (i < raw.size() && raw[i] == ',') {
                ++i;
                while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i])))
                    ++i;
                continue;
            }
            if (i < raw.size() && raw[i] == ']')
                return true;
            return false;
        }
    }

    if (raw[0] == '"') {
        std::optional<QuotedString> quoted = parseQuotedString(raw, 0);
        if (!quoted)
            return false;
        std::string::size_type end = quoted->end;
        // Only whitespace or an inline comment may follow the closing quote.
        // Accepting anything else reads the first quoted run and discards the
        // rest, so `"""Menlo"""` parses as the empty string between the first
        // two quotes and stores nothing — a value the file does not contain,
        // from a line nothing complained about. An empty family is also the
        // "use the bundled face" setting, so the loss is indistinguishable from
        // a choice.
        while (end < raw.size() && std::isspace(static_cast<unsigned char>(raw[end])))
            ++end;
        if (end < raw.size() && raw[end] != '#')
            return false;
        v.kind = Value::Kind::String;
        v.str = std::move(quoted->value);
        return true;
    }

    // Unquoted scalar: strip an inline `#` comment, then re-trim.
    std::string body = raw;
    std::string::size_type hash = body.find('#');
    if (hash != std::string::npos)
        body = body.substr(0, hash);
    body = trim(body);
    if (body.empty())
        return false;

    if (iequals(body, "true")) {
        v.kind = Value::Kind::Bool;
        v.isBoolean = true;
        return true;
    }
    if (iequals(body, "false")) {
        v.kind = Value::Kind::Bool;
        v.isBoolean = false;
        return true;
    }

    // Numeric: a float if it carries a '.', 'e', or 'E'; integer
    // otherwise. strtod / strtoll must consume the whole body.
    bool isFloatLiteral = (body.find('.') != std::string::npos) || (body.find('e') != std::string::npos) || (body.find('E') != std::string::npos);
    const char* cstr = body.c_str();
    char* end = nullptr;
    if (isFloatLiteral) {
        double d = std::strtod(cstr, &end);
        if (end == cstr || *end != '\0')
            return false;
        // Reject NaN/Inf so non-finite values can't reach layout/render.
        if (!std::isfinite(d))
            return false;
        v.kind = Value::Kind::Float;
        v.f = d;
        return true;
    }
    long long n = std::strtoll(cstr, &end, 10);
    if (end == cstr || *end != '\0')
        return false;
    v.kind = Value::Kind::Int;
    v.i = n;
    return true;
}

// Strip leading `#`/`[`-free section markers; returns the bare key path
// inside the brackets, or empty on a malformed header.
std::string parseSectionHeader(const std::string& line)
{
    // line is trimmed and known to start with '['.
    std::string::size_type close = line.find(']');
    if (close == std::string::npos)
        return {};
    return trim(line.substr(1, close - 1));
}

// Scalar key registry: one row per recognized "section.key" pair, grouped by
// value type. Doubles accept Int and Float; numeric values clamp to
// [minValue, maxValue]. Dual-typed / enum-mapped keys live in assignSpecial().
struct StringKey {
    const char* section;
    const char* key;
    std::optional<std::string> Values::* value;
};

struct BoolKey {
    const char* section;
    const char* key;
    std::optional<bool> Values::* value;
};

struct DoubleKey {
    const char* section;
    const char* key;
    std::optional<double> Values::* value;
    double minValue;
    double maxValue;
};

struct IntKey {
    const char* section;
    const char* key;
    std::optional<int> Values::* value;
    int minValue;
    int maxValue;
};

constexpr double anyDouble = std::numeric_limits<double>::lowest();
constexpr double maxAnyDouble = std::numeric_limits<double>::max();
constexpr int anyInt = std::numeric_limits<int>::lowest();
constexpr int maxAnyInteger = std::numeric_limits<int>::max();

// Sentinel min/max meaning "clamp this key to its Preferences::Schema range" (bound
// lives once, in the schema table). Distinct from the `any*` sentinels ("do not
// clamp at parse"). NaN can't appear in a real value or bound, so it's unambiguous.
constexpr double useSchema = std::numeric_limits<double>::quiet_NaN();
constexpr int useSchemaInt = std::numeric_limits<int>::max() - 1;

constexpr StringKey stringKeys[] = {
    { "font", "family", &Values::fontFamily },
    { "font", "proportional_family", &Values::proportionalFontFamily },
    { "title", "window_text", &Values::titleWindowText },
    { "title", "tab_text", &Values::titleTabText },
    { "title", "pane_text", &Values::titlePaneText },
    { "title", "directory_hidden_for", &Values::directoryHiddenFor },
    { "font", "fallback", &Values::fallbackFont },
    { "font", "symbol_fallback", &Values::symbolFallbackFont },
    { "font", "emoji_fallback", &Values::emojiFallbackFont },
    { "font", "cjk_fallback", &Values::cjkFallbackFont },
    { "font", "features", &Values::fontFeatures },
    { "font", "braille_style", &Values::brailleStyle },
    { "font", "variations", &Values::fontVariations },
    { "appearance", "color_space", &Values::colorSpace },
    { "appearance", "colorblind_mode", &Values::colorblindMode },
    { "appearance", "shade_blocks", &Values::shadeBlocks },
    { "appearance", "background_image", &Values::backgroundImage },
    { "security", "clipboard_read", &Values::clipboardRead },
    { "security", "clipboard_write", &Values::clipboardWrite },
    { "security", "osc_allow", &Values::oscAllow },
    { "security", "osc_deny", &Values::oscDeny },
    { "security", "dcs_allow", &Values::dcsAllow },
    { "security", "dcs_deny", &Values::dcsDeny },
    { "security", "apc_allow", &Values::apcAllow },
    { "security", "apc_deny", &Values::apcDeny },
    { "control", "socket_path", &Values::controlSocketPath },
    { "control", "token", &Values::controlToken },
    { "term", "program", &Values::termProgram },
    { "term", "version", &Values::termProgramVersion },
    { "term", "identifier", &Values::termIdentifier },
    { "terminal", "encoding", &Values::encoding },
    { "terminal", "invalid_utf8", &Values::invalidUTF8 },
    { "terminal", "emulation", &Values::emulation },
    { "unicode", "normalization", &Values::normalization },
    { "selection", "copy_style", &Values::copyStyle },
    { "selection", "copy_style_theme", &Values::copyStyleTheme },
    { "tek", "beam_color", &Values::tekBeamColor },
    { "tek", "background_color", &Values::tekBackgroundColor },
    { "network", "proxy_kind", &Values::proxyKind },
    { "network", "proxy_host", &Values::proxyHost },
    { "transfer", "download_dir", &Values::downloadDir },
    { "window", "drag_modifier", &Values::windowDragModifier },
    { "selection", "block_select_modifier", &Values::blockSelectModifier },
    { "selection", "select_modifier", &Values::selectModifier },
    { "selection", "style", &Values::selectionStyle },
    { "remote", "non_owner_presentation", &Values::nonOwnerPresentation },
    { "terminal", "close_ignored_processes", &Values::closeIgnoredProcesses },
    { "terminal", "close_confirmation", &Values::closeConfirmation },
    { "window", "program_fullscreen", &Values::windowProgramFullscreen },
    { "keys", "compose_key", &Values::composeKey },
    { "links", "line_handler", &Values::linkLineHandler },
    { "quake", "hotkey", &Values::quakeHotkey },
    { "scripting", "config_script", &Values::scriptingConfigScript },
};

constexpr BoolKey boolKeys[] = {
    { "font", "ligatures", &Values::ligatures },
    { "font", "wide_glyph_boost", &Values::wideGlyphBoost },
    { "font", "match_fallback_size", &Values::shouldMatchFallbackSize },
    { "font", "braille_grid", &Values::brailleGrid },
    { "underline", "skip_ink", &Values::underlineSkipInk },
    { "underline", "follows_ink", &Values::underlineFollowsInk },
    { "underline", "links", &Values::underlineLinks },
    { "underline", "link_color_blue", &Values::linkColorSystemBlue },
    { "underline", "link_style_per_kind", &Values::linkStylePerKind },
    { "text", "brighten_bold", &Values::brightenBold },
    { "term", "set_locale_environment", &Values::setsLocaleEnvironment },
    { "appearance", "beam_terminal_font", &Values::beamTerminalFont },
    { "tek", "bright_pen", &Values::tekBrightPen },
    { "tek", "glow_vectors", &Values::tekGlowVectors },
    { "appearance", "square_bottom_corners", &Values::squareBottomCorners },
    { "appearance", "folder_icon", &Values::folderIcon },
    { "appearance", "folder_icon_always", &Values::folderIconAlways },
    { "appearance", "process_icons", &Values::processIcons },
    { "appearance", "pane_title_bars", &Values::paneTitleBars },
    { "appearance", "blink_animation", &Values::blinkAnimation },
    { "appearance", "snap_window_resize", &Values::snapWindowResize },
    { "appearance", "hide_title_bar", &Values::hideTitleBar },
    { "appearance", "follow_system", &Values::followSystemAppearance },
    { "appearance", "text_contrast_on_blur", &Values::shouldEnhanceTextOnBlur },
    { "appearance", "text_contrast", &Values::textContrast },
    { "appearance", "background_vibrancy", &Values::backgroundVibrancy },
    { "cursor", "blink", &Values::cursorBlink },
    { "cursor", "invert_text", &Values::cursorInvertText },
    { "render", "vsync", &Values::vsync },
    { "bell", "audible", &Values::bellAudible },
    { "bell", "visual", &Values::bellVisual },
    { "sound", "audible", &Values::soundAudible },
    { "selection", "copy_on_select", &Values::copyOnSelect },
    { "selection", "smart_double_click", &Values::smartDoubleClick },
    { "selection", "links_override_reporting", &Values::linksOverrideReporting },
    { "selection", "osc8_click_opens", &Values::osc8ClickOpens },
    { "selection", "restrict_to_text", &Values::restrictSelectionToText },
    { "mouse", "scroll_bounce", &Values::scrollBounce },
    { "paste", "confirm_multiline", &Values::confirmMultilinePaste },
    { "title", "window_directory", &Values::titleWindowDirectory },
    { "title", "window_directory_path", &Values::titleWindowDirectoryPath },
    { "title", "window_app_title", &Values::titleWindowAppTitle },
    { "title", "window_process", &Values::titleWindowProcess },
    { "title", "window_process_arguments", &Values::titleWindowProcessArguments },
    { "title", "window_shell_command", &Values::titleWindowShellCommand },
    { "title", "window_tty_name", &Values::titleWindowTTYName },
    { "title", "window_profile_name", &Values::titleWindowProfileName },
    { "title", "window_dimensions", &Values::titleWindowDimensions },
    { "title", "window_program_status", &Values::titleWindowProgramStatus },
    { "title", "tab_directory", &Values::titleTabDirectory },
    { "title", "tab_directory_path", &Values::titleTabDirectoryPath },
    { "title", "tab_app_title", &Values::titleTabAppTitle },
    { "title", "tab_process", &Values::titleTabProcess },
    { "title", "tab_process_arguments", &Values::titleTabProcessArguments },
    { "title", "tab_shell_command", &Values::titleTabShellCommand },
    { "title", "tab_tty_name", &Values::titleTabTTYName },
    { "title", "tab_profile_name", &Values::titleTabProfileName },
    { "title", "tab_dimensions", &Values::titleTabDimensions },
    { "title", "tab_program_status", &Values::titleTabProgramStatus },
    { "title", "pane_directory", &Values::titlePaneDirectory },
    { "title", "pane_directory_path", &Values::titlePaneDirectoryPath },
    { "title", "pane_app_title", &Values::titlePaneAppTitle },
    { "title", "pane_process", &Values::titlePaneProcess },
    { "title", "pane_process_arguments", &Values::titlePaneProcessArguments },
    { "title", "pane_shell_command", &Values::titlePaneShellCommand },
    { "title", "pane_tty_name", &Values::titlePaneTTYName },
    { "title", "pane_profile_name", &Values::titlePaneProfileName },
    { "title", "pane_dimensions", &Values::titlePaneDimensions },
    { "title", "pane_program_status", &Values::titlePaneProgramStatus },
    { "title", "directory_hidden_in_full_screen", &Values::directoryHiddenInFullScreen },
    { "dock", "bounce", &Values::dockBounce },
    { "dock", "badge", &Values::dockBadge },
    { "security", "secure_keyboard_entry", &Values::secureKeyboardEntry },
    { "security", "password_detect", &Values::passwordDetect },
    { "security", "secure_keyboard_entry_at_password_prompts", &Values::secureKeyboardEntryAtPasswordPrompts },
    { "control", "enable", &Values::controlEnabled },
    { "control", "approve", &Values::controlApprove },
    { "experimental", "session_persistence", &Values::sessionPersistence },
    { "notifications", "enabled", &Values::notifications },
    { "notifications", "sound", &Values::notificationSound },
    { "remote", "auto_install_host", &Values::autoDeployHostOnRemoteConnect },
    { "remote", "history_cache_on_disk", &Values::shouldPersistRemoteHistory },
    { "network", "ssh_forward_agent", &Values::sshForwardAgent },
    { "transfer", "auto_receive", &Values::transferAutoReceive },
    { "transfer", "zmodem", &Values::shouldWatchForZModem },
    { "triggers", "allow_run_command", &Values::allowRunCommand },
    { "triggers", "allow_webhook", &Values::allowWebhook },
    { "terminal", "column_mode_resizes_window", &Values::columnModeResizesWindow },
    { "quake", "enabled", &Values::quakeEnabled },
    { "quake", "animate", &Values::quakeAnimate },
    { "scripting", "enable", &Values::scriptingEnable },
    { "debug", "show_build_hash", &Values::debugShowBuildHash },
    { "updates", "check_automatically", &Values::shouldCheckForUpdatesAutomatically },
};

constexpr DoubleKey doubleKeys[] = {
    { "font", "size", &Values::fontSize, anyDouble, maxAnyDouble },
    { "font", "proportional_size", &Values::proportionalFontSize, anyDouble, maxAnyDouble },
    { "font", "line_spacing", &Values::lineSpacingPoints, anyDouble, maxAnyDouble },
    { "font", "baseline_offset", &Values::baselineOffset, useSchema, useSchema },
    { "font", "character_spacing", &Values::characterSpacingPoints, useSchema, useSchema },
    { "font", "width", &Values::fontWidthPercent, useSchema, useSchema },
    { "font", "wide_glyph_scale", &Values::wideGlyphScale, useSchema, useSchema },
    { "underline", "thickness", &Values::underlineThickness, anyDouble, maxAnyDouble },
    { "underline", "offset", &Values::underlineOffset, anyDouble, maxAnyDouble },
    { "underline", "skip_ink_pad", &Values::underlineSkipInkPad, anyDouble, maxAnyDouble },
    { "appearance", "padding", &Values::padding, anyDouble, maxAnyDouble },
    { "appearance", "padding_left", &Values::paddingLeft, anyDouble, maxAnyDouble },
    { "appearance", "padding_right", &Values::paddingRight, anyDouble, maxAnyDouble },
    { "appearance", "padding_top", &Values::paddingTop, anyDouble, maxAnyDouble },
    { "appearance", "padding_bottom", &Values::paddingBottom, anyDouble, maxAnyDouble },
    { "appearance", "titleless_padding_top", &Values::titlelessPaddingTop, anyDouble, maxAnyDouble },
    // Opacity floor keeps the window from becoming a ghost with
    // unreadable text; the speed clamps keep a stray value from
    // freezing or rocketing the wheel/trackpad. These bounds are
    // single-sourced from Schema.h via the useSchema sentinel.
    { "appearance", "background_opacity", &Values::backgroundOpacity, useSchema, useSchema },
    { "appearance", "background_image_opacity", &Values::backgroundImageOpacity, useSchema, useSchema },
    { "appearance", "default_background_opacity", &Values::defaultBackgroundOpacity, 0.0, 1.0 },
    { "appearance", "blur_radius", &Values::blurRadius, useSchema, useSchema },
    { "appearance", "ui_font_scale", &Values::uiFontScale, useSchema, useSchema },
    { "appearance", "tag_color_opacity", &Values::tagColorOpacity, useSchema, useSchema },
    { "bell", "visual_intensity", &Values::bellVisualIntensity, useSchema, useSchema },
    { "sound", "volume", &Values::soundVolume, useSchema, useSchema },
    { "mouse", "wheel_speed", &Values::wheelSpeed, useSchema, useSchema },
    { "mouse", "trackpad_speed", &Values::trackpadSpeed, useSchema, useSchema },
    { "quake", "height", &Values::quakeHeight, 0.2, 1.0 },
};

constexpr IntKey intKeys[] = {
    { "font", "weight", &Values::fontWeight, useSchemaInt, useSchemaInt },
    { "font", "bold_weight", &Values::boldFontWeight, useSchemaInt, useSchemaInt },
    { "underline", "link_style", &Values::linkUnderlineStyle, anyInt, maxAnyInteger },
    // system_dark_theme / system_light_theme route through assignSpecial: the
    // value is a theme NAME (string), with an int accepted only for back-compat.
    { "cursor", "linger", &Values::cursorLinger, anyInt, maxAnyInteger },
    { "cursor", "blink_rate", &Values::cursorBlinkRate, useSchemaInt, useSchemaInt },
    { "cursor", "alpha", &Values::cursorAlpha, anyInt, maxAnyInteger },
    { "cursor", "motion_duration", &Values::cursorMotionDuration, 1, 1000 },
    { "render", "max_fps", &Values::maxFPS, 0, 1000 },
    { "mouse", "wheel_lines", &Values::wheelLines, useSchemaInt, useSchemaInt },
    { "paste", "confirm_multiline_threshold", &Values::confirmMultilinePasteThreshold, useSchemaInt, useSchemaInt },
    // Wider than the UI stepper (1000..100000): a power user can
    // dial scrollback well past the slider, so this explicit clamp stays.
    { "terminal", "scrollback_lines", &Values::scrollbackLines, 1, 1000000 },
    { "terminal", "ambiguous_width", &Values::ambiguousWidth, 1, 2 },
    { "network", "proxy_port", &Values::proxyPort, 0, 65535 },
    { "tek", "color_mode", &Values::tekColorMode, 0, 1 },
    // Grid-pin keys: narrower than maxColumns/maxRows (1024/1024) so a stray
    // huge value can't pin to a grid the display can't fit.
    { "window", "initial_columns", &Values::windowInitialColumns, 10, 1000 },
    { "window", "initial_rows", &Values::windowInitialRows, 4, 500 },
};

// Keys whose value doesn't map onto one scalar member: theme routes by value
// type, while shape and show_shortcuts map strings onto enums.
enum class AssignmentResult {
    NotRecognized,
    Assigned,
    InvalidValue,
};

// The `[capabilities.<origin>]` tables. Their header keeps its dotted name
// through the parse; see the header handling in parse().
constexpr std::string_view capabilitiesSectionPrefix = "capabilities.";

/// The override slot one `[capabilities.<origin>]` key names.
///
/// - Parameters:
///   - c: The configuration holding the three origins' overrides.
///   - section: The whole dotted section name.
///   - key: The tier's key.
/// - Returns: The slot, or null when the section names no origin or the key no
///   configurable tier — Benign and Rendering among them, which have no key.
std::optional<CapabilityDecision>* capabilityOverrideSlot(Values& c, const std::string& section, const std::string& key)
{
    if (!section.starts_with(capabilitiesSectionPrefix))
        return nullptr;
    const std::string_view origin = std::string_view(section).substr(capabilitiesSectionPrefix.size());
    CapabilityOverrides* overrides = nullptr;
    if (origin == "local")
        overrides = &c.localCapabilities;
    else if (origin == "agent")
        overrides = &c.agentCapabilities;
    else if (origin == "remote")
        overrides = &c.remoteCapabilities;
    if (!overrides)
        return nullptr;
    if (key == "informational")
        return &overrides->informational;
    if (key == "host_escaping")
        return &overrides->hostEscaping;
    if (key == "read_exfiltration")
        return &overrides->readExfiltration;
    return nullptr;
}

AssignmentResult assignSpecial(Values& c, const std::string& section, const std::string& key, const Value& v)
{
    if (std::optional<CapabilityDecision>* slot = capabilityOverrideSlot(c, section, key)) {
        // "allow" | "prompt" | "deny". Anything else leaves the per-origin
        // default in place and is reported as a skipped key.
        if (v.kind == Value::Kind::String) {
            if (auto decision = capabilityDecisionFromString(v.str)) {
                *slot = *decision;
                return AssignmentResult::Assigned;
            }
        }
        return AssignmentResult::InvalidValue;
    }
    if (section == "appearance" && key == "color_space") {
        // Stored as its canonical token, so a token the reader does not know
        // is refused here rather than resolving to sRGB at every read.
        if (v.kind == Value::Kind::String) {
            if (auto space = colorSpaceFromString(v.str)) {
                c.colorSpace = colorSpaceToString(*space);
                return AssignmentResult::Assigned;
            }
        }
        return AssignmentResult::InvalidValue;
    }
    if (section == "appearance" && key == "theme") {
        // Themes are keyed by NAME only; a non-string value is not honored.
        if (v.kind == Value::Kind::String) {
            c.themeName = v.str;
        }
        return v.kind == Value::Kind::String ? AssignmentResult::Assigned : AssignmentResult::InvalidValue;
    }
    if (section == "appearance" && (key == "system_dark_theme" || key == "system_light_theme")) {
        // Per-mode themes are keyed by NAME only; a non-string value is ignored.
        if (v.kind == Value::Kind::String) {
            const bool isDark = (key == "system_dark_theme");
            (isDark ? c.systemDarkThemeName : c.systemLightThemeName) = v.str;
        }
        return v.kind == Value::Kind::String ? AssignmentResult::Assigned : AssignmentResult::InvalidValue;
    }
    if (section == "cursor" && key == "shape") {
        if (v.kind == Value::Kind::String) {
            if (auto shape = cursorShapeFromString(v.str)) {
                c.cursorShape = *shape;
                return AssignmentResult::Assigned;
            }
        }
        return AssignmentResult::InvalidValue;
    }
    if (section == "cursor" && key == "motion") {
        // "off" | "short" | "long" | "always" (case-insensitive), the
        // distance-based glide modes. Anything else leaves the compiled default
        // (Off) in place.
        if (v.kind == Value::Kind::String) {
            if (auto mode = cursorMotionFromString(v.str)) {
                c.cursorMotion = *mode;
                return AssignmentResult::Assigned;
            }
        }
        return AssignmentResult::InvalidValue;
    }
    if (section == "keys" && key == "accessory_symbols") {
        const std::string_view supported = Defaults::keyboardAccessorySymbols;
        if (v.kind != Value::Kind::String || v.str.size() > supported.size() || v.str.find_first_not_of(supported) != std::string::npos)
            return AssignmentResult::InvalidValue;
        for (size_t index = 0; index < v.str.size(); ++index) {
            if (v.str.find(v.str[index]) != index)
                return AssignmentResult::InvalidValue;
        }
        c.keyboardAccessorySymbols = v.str;
        return AssignmentResult::Assigned;
    }
    if (section == "keys" && key == "option_as_alt") {
        // A token outside the four leaves the default in place, so a typo
        // never turns an Option key into Meta behind the reader's back.
        if (v.kind == Value::Kind::String) {
            if (auto policy = optionAsAlternateFromString(v.str)) {
                c.optionAsAlternate = *policy;
                return AssignmentResult::Assigned;
            }
        }
        return AssignmentResult::InvalidValue;
    }
    if (section == "appearance" && key == "beam_surface") {
        if (v.kind == Value::Kind::String) {
            if (auto surface = beamSurfaceFromString(v.str)) {
                c.beamSurface = *surface;
                return AssignmentResult::Assigned;
            }
        }
        return AssignmentResult::InvalidValue;
    }
    if (section == "appearance" && key == "titlebar_style") {
        if (v.kind == Value::Kind::String) {
            if (auto style = titlebarStyleFromString(v.str)) {
                c.titlebarStyle = *style;
                return AssignmentResult::Assigned;
            }
        }
        return AssignmentResult::InvalidValue;
    }
    if (section == "appearance" && key == "background_image_mode") {
        if (v.kind == Value::Kind::String) {
            if (auto mode = backgroundImageModeFromString(v.str)) {
                c.backgroundImageMode = *mode;
                return AssignmentResult::Assigned;
            }
        }
        return AssignmentResult::InvalidValue;
    }
    if (section == "clipboard" && key == "remote_tools") {
        // A bool for the obvious reading (true = serve the client's), or the
        // names, so a file can say which pasteboard rather than only whether.
        if (v.kind == Value::Kind::Bool) {
            c.remoteClipboardTools = v.isBoolean ? RemoteClipboardTools::Client : RemoteClipboardTools::Off;
            return AssignmentResult::Assigned;
        }
        if (v.kind == Value::Kind::String) {
            if (v.str == "off" || v.str == "none" || v.str == "host") {
                c.remoteClipboardTools = RemoteClipboardTools::Off;
                return AssignmentResult::Assigned;
            }
            if (v.str == "client" || v.str == "local" || v.str == "on") {
                c.remoteClipboardTools = RemoteClipboardTools::Client;
                return AssignmentResult::Assigned;
            }
        }
        return AssignmentResult::InvalidValue;
    }
    if (section == "tabs" && key == "show_shortcuts") {
        if (v.kind == Value::Kind::String) {
            if (auto hints = tabShortcutHintsFromString(v.str)) {
                c.tabShortcutHints = *hints;
                return AssignmentResult::Assigned;
            }
        }
        return AssignmentResult::InvalidValue;
    }
    return AssignmentResult::NotRecognized;
}

// Keys whose default is a sentinel outside their clamp range: `width = 0` is
// the face's own width and `initial_columns = 0` is no pin, while every number
// a person picks lies inside the range. Stating the sentinel must read as the
// default it is rather than be clamped into a value — otherwise a document that
// writes the default out configures something an empty document does not.
struct SentinelDefault {
    const char* section;
    const char* key;
    double value;
};

constexpr SentinelDefault sentinelDefaults[] = {
    { "font", "width", Defaults::fontWidthPercent },
    { "window", "initial_columns", static_cast<double>(Defaults::windowInitialColumns) },
    { "window", "initial_rows", static_cast<double>(Defaults::windowInitialRows) },
};

[[nodiscard]] bool isSentinelDefault(const char* section, const char* key, double value)
{
    for (const SentinelDefault& entry : sentinelDefaults) {
        if (std::strcmp(section, entry.section) == 0 && std::strcmp(key, entry.key) == 0)
            return value == entry.value;
    }
    return false;
}

// Resolve the effective [min, max]: a `useSchema` bound is filled from
// Schema.h, an `any*` bound means "do not clamp at parse", anything else
// is a literal bound.
void resolveClampRange(const char* section, const char* key, double minValue, double maxValue, double& minOutputRange, double& maxOutputRange)
{
    const Schema::NumericRange* r = Schema::rangeFor(section, key);
    minOutputRange = (minValue != minValue) ? (r ? r->minimum : anyDouble) : minValue; // NaN==useSchema
    maxOutputRange = (maxValue != maxValue) ? (r ? r->maximum : maxAnyDouble) : maxValue;
}

void resolveClampRange(const char* section, const char* key, int minValue, int maxValue, int& minOutputRange, int& maxOutputRange)
{
    const Schema::NumericRange* r = Schema::rangeFor(section, key);
    minOutputRange = (minValue == useSchemaInt) ? (r ? static_cast<int>(r->minimum) : anyInt) : minValue;
    maxOutputRange = (maxValue == useSchemaInt) ? (r ? static_cast<int>(r->maximum) : maxAnyInteger) : maxValue;
}

// Assign one recognized "section.key" pair into the Values. The return value
// distinguishes a successful assignment from an unknown pair or wrong type;
// permissive parsing ignores false while strict parsing rejects it.

/// Returns the value kind a key is declared under, or null when no table names it.
///
/// A refusal has to say whether the key is unknown or the value is the wrong
/// shape for it, and the generic assignment cannot tell: each table is searched
/// by the VALUE's kind, so a string key handed an integer misses all of them
/// and looks exactly like a typo. Asking the tables directly separates the two
/// without keeping a second list of what exists.
///
/// - Parameters:
///   - section: The `[section]` in force, empty at top level.
///   - key: The key as written.
/// - Returns: A phrase naming the kind — "a string", "a number", "true or
///   false" — or null when the key is in no table.
[[nodiscard]] const char* declaredValueKindForKey(const std::string& section, const std::string& key)
{
    for (const StringKey& entry : stringKeys)
        if (section == entry.section && key == entry.key)
            return "a string";
    for (const BoolKey& entry : boolKeys)
        if (section == entry.section && key == entry.key)
            return "true or false";
    for (const IntKey& entry : intKeys)
        if (section == entry.section && key == entry.key)
            return "a number";
    for (const DoubleKey& entry : doubleKeys)
        if (section == entry.section && key == entry.key)
            return "a number";
    return nullptr;
}

/// Whether `spec` is a modifier chord `[window] drag_modifier` can read: names
/// joined by `+`, each one a modifier the pointer ladder knows, or `none`.
[[nodiscard]] bool isValidDragChord(const std::string& spec)
{
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t plus = spec.find('+', start);
        const std::size_t end = plus == std::string::npos ? spec.size() : plus;
        std::string token = spec.substr(start, end - start);
        const auto isSpace = [](char c) { return c == ' ' || c == '\t'; };
        while (!token.empty() && isSpace(token.front()))
            token.erase(token.begin());
        while (!token.empty() && isSpace(token.back()))
            token.pop_back();
        for (char& c : token)
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        const bool isKnown = token == "cmd" || token == "command" || token == "super" || token == "win" || token == "ctrl"
            || token == "control" || token == "alt" || token == "opt" || token == "option" || token == "meta" || token == "shift"
            || token == "none";
        if (!token.empty() && !isKnown)
            return false;
        if (plus == std::string::npos)
            break;
        start = plus + 1;
    }
    return true;
}

/// Applies one key to the configuration and reports which refusal it was.
///
/// The caller records the refusal, and the two read differently to whoever is
/// holding the file: one says the name is wrong, the other that the name is
/// right and the value is not. `assignSpecial` already separates them for the
/// keys it owns, and the distinction was collapsed to a bool one frame up.
///
/// - Parameters:
///   - c: The configuration to fill.
///   - section: The `[section]` in force, empty at top level.
///   - key: The key as written.
///   - v: The parsed value.
/// - Returns: Whether the key was assigned, unrecognised, or given a value it
///   does not take.
AssignmentResult assignReporting(Values& c, const std::string& section, const std::string& key, const Value& v)
{
    const AssignmentResult special = assignSpecial(c, section, key, v);
    if (special != AssignmentResult::NotRecognized)
        return special;

    switch (v.kind) {
    case Value::Kind::String:
        for (const StringKey& entry : stringKeys) {
            if (section == entry.section && key == entry.key) {
                if (section == "window" && key == "drag_modifier" && !isValidDragChord(v.str))
                    return AssignmentResult::InvalidValue;
                // A closed vocabulary: a token the renderer would read as the
                // default is refused here, so the file's author hears of it.
                if (section == "appearance" && key == "shade_blocks"
                    && Rendering::shadeStyleToString(Rendering::shadeStyleFromString(v.str)) != v.str)
                    return AssignmentResult::InvalidValue;
                c.*entry.value = v.str;
                return AssignmentResult::Assigned;
            }
        }
        return declaredValueKindForKey(section, key) ? AssignmentResult::InvalidValue : AssignmentResult::NotRecognized;
    case Value::Kind::Bool:
        for (const BoolKey& entry : boolKeys) {
            if (section == entry.section && key == entry.key) {
                c.*entry.value = v.isBoolean;
                return AssignmentResult::Assigned;
            }
        }
        return declaredValueKindForKey(section, key) ? AssignmentResult::InvalidValue : AssignmentResult::NotRecognized;
    case Value::Kind::Int:
        for (const IntKey& entry : intKeys) {
            if (section == entry.section && key == entry.key) {
                int lo, hi;
                resolveClampRange(entry.section, entry.key, entry.minValue, entry.maxValue, lo, hi);
                const int stated = static_cast<int>(v.i);
                c.*entry.value = isSentinelDefault(entry.section, entry.key, stated) ? stated : std::clamp(stated, lo, hi);
                return AssignmentResult::Assigned;
            }
        }
        [[fallthrough]]; // an Int is welcome anywhere a Float is
    case Value::Kind::Float:
        for (const DoubleKey& entry : doubleKeys) {
            if (section == entry.section && key == entry.key) {
                double d = (v.kind == Value::Kind::Int) ? static_cast<double>(v.i) : v.f;
                double lo, hi;
                resolveClampRange(entry.section, entry.key, entry.minValue, entry.maxValue, lo, hi);
                c.*entry.value = isSentinelDefault(entry.section, entry.key, d) ? d : std::clamp(d, lo, hi);
                return AssignmentResult::Assigned;
            }
        }
        return declaredValueKindForKey(section, key) ? AssignmentResult::InvalidValue : AssignmentResult::NotRecognized;
    default:
        return declaredValueKindForKey(section, key) ? AssignmentResult::InvalidValue : AssignmentResult::NotRecognized;
    }
}

} // namespace

// Every field is either an optional, which default-constructs empty, or a
// container that default-constructs empty. Empty is the only spelling of unset, and the
// default lives once, at the read; seeding a field with a default as well would
// leave a number no reader may believe.
Values::Values() = default;

Values::PaddingEdges Values::resolvedPadding(DefaultsTarget target) const
{
    // Three layers, narrowest first: the per-edge key, the `padding` shorthand,
    // then the target's built-in. Written as one chain of value_or because each
    // layer is exactly "use this if it is there", which is what the optional says.
    const auto edge = [this](const std::optional<double>& perEdge, double builtIn) {
        return std::clamp(perEdge.value_or(padding.value_or(builtIn)), 0.0, 64.0);
    };
    const PaddingEdges builtIn = Defaults::paddingEdges(target);
    PaddingEdges e;
    e.left = edge(paddingLeft, builtIn.left);
    e.right = edge(paddingRight, builtIn.right);
    e.top = edge(paddingTop, builtIn.top);
    e.bottom = edge(paddingBottom, builtIn.bottom);
    return e;
}

int parseScriptingRules(const std::string& scriptOutput, ScriptingRules& outValue)
{
    int accepted = 0;
    std::string::size_type pos = 0;
    while (pos <= scriptOutput.size()) {
        std::string::size_type nl = scriptOutput.find('\n', pos);
        std::string raw = (nl == std::string::npos) ? scriptOutput.substr(pos) : scriptOutput.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? scriptOutput.size() + 1 : nl + 1;

        std::string line = trim(raw);
        if (line.empty() || line[0] == '#')
            continue;

        // <directive> <key> = <value>: the directive is the first whitespace-
        // delimited word; the rest splits on the FIRST '='.
        std::string::size_type sp = line.find_first_of(" \t");
        if (sp == std::string::npos)
            continue; // a bare word with no key/value
        std::string directive = line.substr(0, sp);
        std::string rest = trim(line.substr(sp + 1));
        std::string::size_type eq = rest.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(rest.substr(0, eq));
        std::string value = trim(rest.substr(eq + 1));
        if (key.empty())
            continue;

        if (directive == "trigger") {
            TriggerEntry t;
            t.pattern = key;
            t.action = value;
            outValue.triggers.push_back(std::move(t));
            ++accepted;
        } else if (directive == "macro") {
            MacroEntry m;
            m.name = key;
            m.body = value;
            outValue.macros.push_back(std::move(m));
            ++accepted;
        } else if (directive == "charclass") {
            CharacterClassEntry cc;
            cc.name = key;
            cc.pattern = value;
            outValue.characterClasses.push_back(std::move(cc));
            ++accepted;
        }
        // Unknown directives are ignored (forward-compatible).
    }
    return accepted;
}

int mergeScriptingRules(Values& into, const std::string& scriptOutput)
{
    // Past the bound, only the whole lines before it: a directive the bound
    // cut through is a different directive, and merging its first half would
    // install a rule the script never wrote.
    std::string bounded = scriptOutput;
    if (bounded.size() > scriptingOutputLimit) {
        const std::string::size_type lastNewline = bounded.rfind('\n', scriptingOutputLimit - 1);
        bounded.resize(lastNewline == std::string::npos ? 0 : lastNewline + 1);
    }
    ScriptingRules rules;
    const int accepted = parseScriptingRules(bounded, rules);
    if (accepted <= 0)
        return 0;
    for (TriggerEntry& trigger : rules.triggers)
        into.triggers.push_back(std::move(trigger));
    for (MacroEntry& macro : rules.macros)
        into.macros.push_back(std::move(macro));
    for (CharacterClassEntry& characterClass : rules.characterClasses)
        into.characterClasses.push_back(std::move(characterClass));
    return accepted;
}

const std::string* findMacroBody(const Values& c, const std::string& name)
{
    for (const MacroEntry& m : c.macros) {
        if (m.name == name)
            return &m.body;
    }
    return nullptr;
}

bool parseHexColor(const std::string& s, ColorEntry& outValue)
{
    std::string t;
    t.reserve(s.size());
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            t += c;
    }
    if (t.size() < 2 || t[0] != '#')
        return false;
    const char* p = t.c_str() + 1;
    const size_t n = t.size() - 1;
    if (n != 6 && n != 8)
        return false;
    int v[8] = { 0 };
    for (size_t k = 0; k < n; ++k) {
        int d = hexDigit(p[k]);
        if (d < 0)
            return false;
        v[k] = d;
    }
    outValue.red = static_cast<std::uint8_t>((v[0] << 4) | v[1]);
    outValue.green = static_cast<std::uint8_t>((v[2] << 4) | v[3]);
    outValue.blue = static_cast<std::uint8_t>((v[4] << 4) | v[5]);
    outValue.alpha = (n == 8) ? static_cast<std::uint8_t>((v[6] << 4) | v[7]) : 255;
    outValue.set = true;
    return true;
}

std::optional<CursorShape> cursorShapeFromString(const std::string& s)
{
    std::string t = trim(s);
    if (iequals(t, "block"))
        return CursorShape::Block;
    if (iequals(t, "underline"))
        return CursorShape::Underline;
    if (iequals(t, "bar"))
        return CursorShape::Bar;
    return std::nullopt;
}

const char* cursorShapeToString(CursorShape shape)
{
    switch (shape) {
    case CursorShape::Block:
        return "block";
    case CursorShape::Underline:
        return "underline";
    case CursorShape::Bar:
        return "bar";
    }
    return "block";
}

std::optional<CursorShape> cursorShapeFromInt(int value)
{
    for (const CursorShape shape : { CursorShape::Block, CursorShape::Underline, CursorShape::Bar }) {
        if (value == static_cast<int>(shape))
            return shape;
    }
    return std::nullopt;
}

std::optional<CursorMotionMode> cursorMotionFromString(const std::string& s)
{
    std::string t = trim(s);
    if (iequals(t, "off"))
        return CursorMotionMode::Off;
    if (iequals(t, "short"))
        return CursorMotionMode::Short;
    if (iequals(t, "long"))
        return CursorMotionMode::Long;
    if (iequals(t, "always"))
        return CursorMotionMode::Always;
    return std::nullopt;
}

std::optional<TabShortcutHints> tabShortcutHintsFromString(const std::string& s)
{
    std::string t = trim(s);
    if (iequals(t, "off"))
        return TabShortcutHints::Off;
    if (iequals(t, "always"))
        return TabShortcutHints::Always;
    if (iequals(t, "command"))
        return TabShortcutHints::OnCommand;
    return std::nullopt;
}

const char* tabShortcutHintsToString(TabShortcutHints hints)
{
    switch (hints) {
    case TabShortcutHints::Off:
        return "off";
    case TabShortcutHints::Always:
        return "always";
    case TabShortcutHints::OnCommand:
        return "command";
    }
    return "always";
}

const char* cursorMotionToString(CursorMotionMode mode)
{
    switch (mode) {
    case CursorMotionMode::Off:
        return "off";
    case CursorMotionMode::Short:
        return "short";
    case CursorMotionMode::Long:
        return "long";
    case CursorMotionMode::Always:
        return "always";
    }
    return "off";
}

std::optional<CursorMotionMode> cursorMotionFromInt(int value)
{
    for (const CursorMotionMode mode : { CursorMotionMode::Off, CursorMotionMode::Short, CursorMotionMode::Long, CursorMotionMode::Always }) {
        if (value == static_cast<int>(mode))
            return mode;
    }
    return std::nullopt;
}

std::optional<CapabilityDecision> capabilityDecisionFromString(const std::string& s)
{
    std::string t = trim(s);
    if (iequals(t, "allow"))
        return CapabilityDecision::Allow;
    if (iequals(t, "prompt"))
        return CapabilityDecision::Prompt;
    if (iequals(t, "deny"))
        return CapabilityDecision::Deny;
    return std::nullopt;
}

const char* capabilityDecisionToString(CapabilityDecision decision)
{
    switch (decision) {
    case CapabilityDecision::Allow:
        return "allow";
    case CapabilityDecision::Prompt:
        return "prompt";
    case CapabilityDecision::Deny:
        return "deny";
    }
    return "deny";
}

std::optional<OptionAsAlternate> optionAsAlternateFromString(const std::string& s)
{
    std::string t = trim(s);
    if (iequals(t, "none"))
        return OptionAsAlternate::None;
    if (iequals(t, "left"))
        return OptionAsAlternate::Left;
    if (iequals(t, "right"))
        return OptionAsAlternate::Right;
    if (iequals(t, "both"))
        return OptionAsAlternate::Both;
    return std::nullopt;
}

std::optional<TitlebarStyle> titlebarStyleFromString(const std::string& s)
{
    std::string t = trim(s);
    if (iequals(t, "themed"))
        return TitlebarStyle::Themed;
    if (iequals(t, "native"))
        return TitlebarStyle::Native;
    return std::nullopt;
}

const char* titlebarStyleToString(TitlebarStyle style)
{
    switch (style) {
    case TitlebarStyle::Themed:
        return "themed";
    case TitlebarStyle::Native:
        return "native";
    }
    return "themed";
}

std::optional<TitlebarStyle> titlebarStyleFromInt(int value)
{
    for (const TitlebarStyle style : { TitlebarStyle::Themed, TitlebarStyle::Native }) {
        if (value == static_cast<int>(style))
            return style;
    }
    return std::nullopt;
}

std::optional<BeamSurface> beamSurfaceFromString(const std::string& s)
{
    std::string t = trim(s);
    if (iequals(t, "auto"))
        return BeamSurface::Auto;
    if (iequals(t, "native"))
        return BeamSurface::Native;
    // Three spellings for one value: a hyphen matches the other tokens in this
    // section, an underscore matches the KEY names, and "terminal" is what the
    // setting is called in conversation. Rejecting two of them would be a typo
    // class nobody can diagnose from the file.
    if (iequals(t, "in-terminal") || iequals(t, "in_terminal") || iequals(t, "terminal"))
        return BeamSurface::InTerminal;
    return std::nullopt;
}

const char* beamSurfaceToString(BeamSurface surface)
{
    switch (surface) {
    case BeamSurface::Auto:
        return "auto";
    case BeamSurface::Native:
        return "native";
    case BeamSurface::InTerminal:
        return "in-terminal";
    }
    return "auto";
}

std::optional<BeamSurface> beamSurfaceFromInt(int value)
{
    for (const BeamSurface surface : { BeamSurface::Auto, BeamSurface::Native, BeamSurface::InTerminal }) {
        if (value == static_cast<int>(surface))
            return surface;
    }
    return std::nullopt;
}

BeamPresentation resolveBeamSurface(BeamSurface configured, NativeBeam native, InGridBeam inGrid)
{
    const bool hasNative = native == NativeBeam::Available;
    const bool hasInGrid = inGrid == InGridBeam::Available;

    // What was asked for, then the other one, then the in-grid answer as the
    // last resort. The order is what makes an explicit request survive a
    // frontend that cannot honour it: it degrades to the surface that exists
    // rather than to nothing.
    switch (configured) {
    case BeamSurface::Native:
        if (hasNative)
            return BeamPresentation::Native;
        // In-grid even when it too is unavailable: with neither surface the
        // caller should not be asking, and of the two wrong answers, drawing
        // into its own buffer beats calling a toolkit it does not have.
        return BeamPresentation::InTerminal;
    case BeamSurface::InTerminal:
        if (hasInGrid)
            return BeamPresentation::InTerminal;
        return hasNative ? BeamPresentation::Native : BeamPresentation::InTerminal;
    case BeamSurface::Auto:
        // Native first where both exist: a frontend that built one did so
        // because its platform's users expect it.
        if (hasNative)
            return BeamPresentation::Native;
        return BeamPresentation::InTerminal;
    }
    return BeamPresentation::InTerminal;
}

std::optional<ColorSpace> colorSpaceFromString(const std::string& s)
{
    std::string t = trim(s);
    if (iequals(t, "srgb"))
        return ColorSpace::SRGB;
    if (iequals(t, "display-p3"))
        return ColorSpace::DisplayP3;
    return std::nullopt;
}

const char* colorSpaceToString(ColorSpace space)
{
    switch (space) {
    case ColorSpace::SRGB:
        return "srgb";
    case ColorSpace::DisplayP3:
        return "display-p3";
    }
    return "srgb";
}

std::optional<ColorSpace> colorSpaceFromInt(int value)
{
    for (const ColorSpace space : { ColorSpace::SRGB, ColorSpace::DisplayP3 }) {
        if (value == static_cast<int>(space))
            return space;
    }
    return std::nullopt;
}

int colorSpaceToInt(ColorSpace space) { return static_cast<int>(space); }

std::optional<BackgroundImageMode> backgroundImageModeFromString(const std::string& s)
{
    std::string t = trim(s);
    if (iequals(t, "fill"))
        return BackgroundImageMode::Fill;
    if (iequals(t, "fit"))
        return BackgroundImageMode::Fit;
    if (iequals(t, "stretch"))
        return BackgroundImageMode::Stretch;
    if (iequals(t, "tile"))
        return BackgroundImageMode::Tile;
    if (iequals(t, "center"))
        return BackgroundImageMode::Center;
    return std::nullopt;
}

const char* backgroundImageModeToString(BackgroundImageMode mode)
{
    switch (mode) {
    case BackgroundImageMode::Fill:
        return "fill";
    case BackgroundImageMode::Fit:
        return "fit";
    case BackgroundImageMode::Stretch:
        return "stretch";
    case BackgroundImageMode::Tile:
        return "tile";
    case BackgroundImageMode::Center:
        return "center";
    }
    return "fill";
}

std::optional<BackgroundImageMode> backgroundImageModeFromInt(int value)
{
    for (const BackgroundImageMode mode : { BackgroundImageMode::Fill, BackgroundImageMode::Fit, BackgroundImageMode::Stretch, BackgroundImageMode::Tile, BackgroundImageMode::Center }) {
        if (value == static_cast<int>(mode))
            return mode;
    }
    return std::nullopt;
}

int backgroundImageModeToInt(BackgroundImageMode mode) { return static_cast<int>(mode); }

std::optional<Accessibility::ColorblindMode> colorblindModeFromString(const std::string& s)
{
    std::string t = trim(s);
    if (iequals(t, "none") || t.empty())
        return Accessibility::ColorblindMode::None;
    if (iequals(t, "deuteranopia"))
        return Accessibility::ColorblindMode::Deuteranopia;
    if (iequals(t, "protanopia"))
        return Accessibility::ColorblindMode::Protanopia;
    if (iequals(t, "tritanopia"))
        return Accessibility::ColorblindMode::Tritanopia;
    return std::nullopt;
}

const char* colorblindModeToString(Accessibility::ColorblindMode mode)
{
    switch (mode) {
    case Accessibility::ColorblindMode::None:
        return "none";
    case Accessibility::ColorblindMode::Deuteranopia:
        return "deuteranopia";
    case Accessibility::ColorblindMode::Protanopia:
        return "protanopia";
    case Accessibility::ColorblindMode::Tritanopia:
        return "tritanopia";
    }
    return "none";
}

std::optional<Accessibility::ColorblindMode> colorblindModeFromInt(int value)
{
    for (const Accessibility::ColorblindMode mode : { Accessibility::ColorblindMode::None, Accessibility::ColorblindMode::Deuteranopia, Accessibility::ColorblindMode::Protanopia, Accessibility::ColorblindMode::Tritanopia }) {
        if (value == static_cast<int>(mode))
            return mode;
    }
    return std::nullopt;
}

int colorblindModeToInt(Accessibility::ColorblindMode mode) { return static_cast<int>(mode); }

std::optional<TabShortcutHints> tabShortcutHintsFromInt(int value)
{
    for (const TabShortcutHints hints : { TabShortcutHints::Off, TabShortcutHints::Always, TabShortcutHints::OnCommand }) {
        if (value == static_cast<int>(hints))
            return hints;
    }
    return std::nullopt;
}

int tabShortcutHintsToInt(TabShortcutHints hints) { return static_cast<int>(hints); }

int cursorShapeToInt(CursorShape shape) { return static_cast<int>(shape); }

int cursorMotionToInt(CursorMotionMode mode) { return static_cast<int>(mode); }

// Apply one key/value into the current [[tags]] entry. Unknown keys and type
// mismatches are silently dropped, the way every array-of-tables block here
// treats them.
static void assignTag(TagEntry& tag, const std::string& key, const Value& v)
{
    // A pattern key also takes an array, one pattern for each name a machine
    // or profile goes by; it is kept as the comma-separated spelling a single
    // string may also use.
    if ((key == "match_host" || key == "match_profile") && v.kind == Value::Kind::StringArray) {
        std::string joined;
        for (const std::string& pattern : v.strs) {
            if (pattern.find_first_not_of(" \t") == std::string::npos)
                continue;
            if (!joined.empty())
                joined += ", ";
            joined += pattern;
        }
        (key == "match_host" ? tag.matchHost : tag.matchProfile) = std::move(joined);
        return;
    }
    if (v.kind == Value::Kind::Bool) {
        if (key == "show_name")
            tag.showsName = v.isBoolean;
        else if (key == "show_in_title_bar")
            tag.showsInTitleBar = v.isBoolean;
        else if (key == "show_in_tabs")
            tag.showsInTabs = v.isBoolean;
        else if (key == "show_in_pane_headers")
            tag.showsInPaneHeaders = v.isBoolean;
        return;
    }
    if (v.kind != Value::Kind::String)
        return;
    if (key == "name") {
        tag.name = v.str;
        return;
    }
    if (key == "color") {
        tag.color = v.str;
        return;
    }
    if (key == "match_host") {
        tag.matchHost = v.str;
        return;
    }
    if (key == "match_profile") {
        tag.matchProfile = v.str;
        return;
    }
    if (key == "prominence") {
        tag.prominence = v.str;
        return;
    }
    if (key == "icon") {
        tag.icon = v.str;
        return;
    }
    if (key == "icon_weight")
        tag.iconWeight = v.str;
}

// Apply one key/value into the current [[themes]] entry. Unknown keys
// and type mismatches are silently dropped.
static void assignTheme(ThemeEntry& th, const std::string& key, const Value& v)
{
    const auto appearanceNumber = [&](std::optional<double>& destination, double maximum) {
        if (v.kind == Value::Kind::Float || v.kind == Value::Kind::Int) {
            const double number = v.kind == Value::Kind::Float ? v.f : static_cast<double>(v.i);
            if (std::isfinite(number)) {
                destination = std::clamp(number, 0.0, maximum);
            }
        }
    };
    if (key == "background_opacity") {
        appearanceNumber(th.appearance.backgroundOpacity, 1);
        return;
    }
    if (key == "default_background_opacity") {
        appearanceNumber(th.appearance.defaultBackgroundOpacity, 1);
        return;
    }
    if (key == "blur_radius") {
        appearanceNumber(th.appearance.blurRadius, 50);
        return;
    }
    if (key == "text_vibrancy" && v.kind == Value::Kind::Bool) {
        th.appearance.textVibrancy = v.isBoolean;
        return;
    }
    if (key == "text_contrast" && v.kind == Value::Kind::Bool) {
        th.appearance.textContrast = v.isBoolean;
        return;
    }
    if (key == "background_vibrancy" && v.kind == Value::Kind::Bool) {
        th.appearance.backgroundVibrancy = v.isBoolean;
        return;
    }
    if (key == "system_colors" && v.kind == Value::Kind::Bool) {
        th.appearance.usesSystemColors = v.isBoolean;
        return;
    }
    if (key == "terminal_marks" && v.kind == Value::Kind::String) {
        if (v.str == "palette") {
            th.appearance.terminalMarks = Themes::TerminalMarkStyle::Palette;
        } else if (v.str == "primary") {
            th.appearance.terminalMarks = Themes::TerminalMarkStyle::Primary;
        } else if (v.str == "secondary") {
            th.appearance.terminalMarks = Themes::TerminalMarkStyle::Secondary;
        } else if (v.str == "tertiary") {
            th.appearance.terminalMarks = Themes::TerminalMarkStyle::Tertiary;
        }
        return;
    }

    if (key == "name" && v.kind == Value::Kind::String) {
        th.name = v.str;
        return;
    }
    if (key == "is_dark" && v.kind == Value::Kind::Bool) {
        th.isDark = v.isBoolean;
        return;
    }
    if (key == "fg" && v.kind == Value::Kind::String) {
        parseHexColor(v.str, th.foreground);
        return;
    }
    if (key == "bg" && v.kind == Value::Kind::String) {
        parseHexColor(v.str, th.background);
        return;
    }
    if (key == "cursor" && v.kind == Value::Kind::String) {
        parseHexColor(v.str, th.cursor);
        return;
    }
    // status_failed … status_neutral: the colour a status is drawn in.
    static constexpr std::string_view statusPrefix = "status_";
    if (key.starts_with(statusPrefix) && v.kind == Value::Kind::String) {
        const std::string_view name = std::string_view(key).substr(statusPrefix.size());
        for (std::size_t slot = 0; slot < Themes::statusColorSlotCount; ++slot) {
            if (name == Themes::statusColorSlotName(static_cast<Themes::StatusColorSlot>(slot)))
                parseHexColor(v.str, th.status[slot]);
        }
        return;
    }
    if (key == "ansi" && v.kind == Value::Kind::StringArray) {
        const size_t n = (v.strs.size() < 16) ? v.strs.size() : 16;
        for (size_t i = 0; i < n; ++i)
            parseHexColor(v.strs[i], th.ansi[i]);
        return;
    }
    // ansi0..ansi15: per-slot shorthand for callers that can't write
    // an array literal (or want to override one swatch on top of a base).
    if (key.size() >= 5 && key.starts_with("ansi") && v.kind == Value::Kind::String) {
        char* end = nullptr;
        long index = std::strtol(key.c_str() + 4, &end, 10);
        if (end != key.c_str() + 4 && *end == '\0' && index >= 0 && index < 16)
            parseHexColor(v.str, th.ansi[index]);
        return;
    }
}

// Apply one key/value into the current [[profiles]] entry. Unknown keys
// and type mismatches are silently dropped (forward compatibility). Both
// int and float are accepted for the point-valued fields so `font_size =
// 13` and `font_size = 13.5` both round-trip.
static void assignProfile(Profile& p, const std::string& key, const Value& v)
{
    // A value the file did not carry as a number leaves the field alone, which
    // with an optional is the same statement as "does not set it" — the witness
    // and the value were two ways of saying that and could disagree.
    const auto asDouble = [&](std::optional<double>& destination) {
        if (v.kind == Value::Kind::Float)
            destination = v.f;
        else if (v.kind == Value::Kind::Int)
            destination = static_cast<double>(v.i);
    };
    if (key == "name" && v.kind == Value::Kind::String) {
        p.name = v.str;
        return;
    }
    if (key == "parent" && v.kind == Value::Kind::String) {
        p.parentProfile = v.str;
        return;
    }
    if (key == "scope" && v.kind == Value::Kind::String) {
        p.scope = profileScopeFromString(v.str);
        return;
    }
    if (key == "theme" && v.kind == Value::Kind::String) {
        p.themeName = v.str;
        return;
    }
    if (key == "font_family" && v.kind == Value::Kind::String) {
        p.fontFamily = v.str;
        return;
    }
    if (key == "font_fallback" && v.kind == Value::Kind::String) {
        p.fallbackFont = v.str;
        return;
    }
    if (key == "font_size") {
        asDouble(p.fontSize);
        return;
    }
    if (key == "line_spacing") {
        asDouble(p.lineSpacing);
        return;
    }
    if (key == "padding_left") {
        asDouble(p.paddingLeft);
        return;
    }
    if (key == "padding_right") {
        asDouble(p.paddingRight);
        return;
    }
    if (key == "padding_top") {
        asDouble(p.paddingTop);
        return;
    }
    if (key == "titleless_padding_top") {
        asDouble(p.titlelessPaddingTop);
        return;
    }
    if (key == "padding_bottom") {
        asDouble(p.paddingBottom);
        return;
    }
    if (key == "hide_title_bar" && v.kind == Value::Kind::Bool) {
        p.hideTitleBar = v.isBoolean;
        return;
    }
    if (key == "process_exit" && v.kind == Value::Kind::String) {
        p.processExit = v.str;
        return;
    }
    if (key == "startup_host" && v.kind == Value::Kind::String) {
        p.startupHost = v.str;
        return;
    }
}

// Build one KeyBindingEntry from a raw [keys] line's key + string value.
// The spec is unquoted when quoted so it can carry '+' and spaces (chords).
static KeyBindingEntry makeKeyBinding(const std::string& key, const std::string& action)
{
    KeyBindingEntry kb;
    if (!key.empty() && key.front() == '"') {
        const std::optional<QuotedString> unquoted = parseQuotedString(key, 0);
        kb.spec = unquoted ? unquoted->value : key;
    } else {
        kb.spec = key;
    }
    kb.action = action;
    return kb;
}

// Build one LinkHandlerEntry from a raw [link_handlers] line's key + string
// value. The scheme key is unquoted when quoted (a bare token works too); the
// command template is stored raw for the frontend to tokenize into argv.
static LinkHandlerEntry makeLinkHandler(const std::string& key, const std::string& command)
{
    LinkHandlerEntry lh;
    if (!key.empty() && key.front() == '"') {
        const std::optional<QuotedString> unquoted = parseQuotedString(key, 0);
        lh.scheme = unquoted ? unquoted->value : key;
    } else {
        lh.scheme = key;
    }
    lh.command = command;
    return lh;
}

// Build one TriggerEntry from a raw [triggers] line. The regex is the KEY
// (unquoted when quoted so it can carry spaces / brackets / '='); the action
// spec is the string VALUE, stored raw for the engine builder to split on ':'.
static TriggerEntry makeTrigger(const std::string& key, const std::string& action)
{
    TriggerEntry tr;
    if (!key.empty() && key.front() == '"') {
        const std::optional<QuotedString> unquoted = parseQuotedString(key, 0);
        tr.pattern = unquoted ? unquoted->value : key;
    } else {
        tr.pattern = key;
    }
    tr.action = action;
    return tr;
}

// Build one CharacterClassEntry from a raw [characterClass] line. The class NAME is the
// key (bare token or quoted); the regex is the string value stored raw.
static CharacterClassEntry makeCharacterClass(const std::string& key, const std::string& pattern)
{
    CharacterClassEntry cc;
    if (!key.empty() && key.front() == '"') {
        const std::optional<QuotedString> unquoted = parseQuotedString(key, 0);
        cc.name = unquoted ? unquoted->value : key;
    } else {
        cc.name = key;
    }
    cc.pattern = pattern;
    return cc;
}

// Build one MacroEntry from a raw [macros] line. The macro NAME is the key
// (bare token or quoted); the body is the string value stored raw (escapes are
// decoded by the frontend at invoke time, same as [keys] send:).
static MacroEntry makeMacro(const std::string& key, const std::string& body)
{
    MacroEntry m;
    if (!key.empty() && key.front() == '"') {
        const std::optional<QuotedString> unquoted = parseQuotedString(key, 0);
        m.name = unquoted ? unquoted->value : key;
    } else {
        m.name = key;
    }
    m.body = body;
    return m;
}

static bool isValidFontFallbackMode(const std::string& mode) { return iequals(mode, "auto") || iequals(mode, "system") || iequals(mode, "regional") || iequals(mode, "custom"); }

static bool isValidFontScript(const std::string& script)
{
    static constexpr const char* scripts[] = { "han", "kana", "hangul", "cyrillic", "greek", "arabic", "hebrew", "devanagari", "bengali", "gurmukhi", "thai", "lao", "khmer", "myanmar", "emoji", "symbols", "math", "latin", "other" };
    for (const char* candidate : scripts) {
        if (iequals(script, candidate))
            return true;
    }
    return false;
}

// An omitted language names the script default, as does "*". Han routing
// tries an exact language preference before these defaults.
static bool isValidLanguageFallback(const LanguageFallbackEntry& fallback)
{
    if (!isValidFontScript(fallback.script))
        return false;
    if (!isValidFontFallbackMode(fallback.mode))
        return false;
    if (iequals(fallback.mode, "custom") && fallback.family.empty())
        return false;
    return true;
}

static bool isValidRangeMap(const RangeMapEntry& map)
{
    if (map.range.empty() || !isValidFontFallbackMode(map.mode))
        return false;
    if (iequals(map.mode, "custom") && map.family.empty())
        return false;
    return parseCodePointRange(map.range).has_value();
}

static void assignLanguageFallback(LanguageFallbackEntry& fallback, const std::string& key, const Value& v)
{
    if (v.kind != Value::Kind::String)
        return;
    if (key == "script")
        fallback.script = v.str;
    else if (key == "language")
        fallback.language = v.str;
    else if (key == "mode")
        fallback.mode = v.str;
    else if (key == "family")
        fallback.family = v.str;
}

static void assignRangeMap(RangeMapEntry& map, const std::string& key, const Value& v)
{
    if (key == "match_size" && v.kind == Value::Kind::Bool) {
        map.shouldMatchSize = v.isBoolean;
        return;
    }
    if (v.kind != Value::Kind::String)
        return;
    if (key == "range") {
        map.range = v.str;
        if (const std::optional<CodePointRange> range = parseCodePointRange(map.range)) {
            map.firstCodePoint = range->firstCodePoint;
            map.lastCodePoint = range->lastCodePoint;
        }
    } else if (key == "mode") {
        map.mode = v.str;
    } else if (key == "family") {
        map.family = v.str;
    }
}

namespace {

/// One line outside every `[[...]]` entry and platform block that the parse
/// applied: a setting, or an entry in one of the tables keyed by name.
///
/// The live configuration keeps the list beside its values, so a later
/// document whose line for the same key is refused keeps what this one applied,
/// placed the way it was placed the first time.
struct Placement {
    std::string section;
    std::string key;
    Value value;
};

/// A key whose line the parse could not apply, so the value it had stands.
struct Refusal {
    /// The table the key was written in; absent for a key under a header the
    /// parse could not read, whose table has no name. Such a key keeps its
    /// value in whichever table set it.
    std::optional<std::string> section;
    std::string key;
};

/// Whether a trimmed line opens a table or assigns a key.
///
/// Neither can sit inside an array value, so a multi-line array that meets one
/// was never closed: the line is the document's next one, not an element.
///
/// - Parameter line: A trimmed, non-empty line.
/// - Returns: Whether it is a `[table]` or `[[table]]` header with a bare name,
///   or a bare or quoted key followed by `=`.
[[nodiscard]] bool beginsTableOrAssignment(const std::string& line)
{
    const auto isBareKeyCharacter = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '-' || character == '.';
    };
    const auto skipBlanks = [&line](std::string::size_type index) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t'))
            ++index;
        return index;
    };
    if (line.empty())
        return false;
    std::string::size_type index = 0;
    if (line.front() == '[') {
        const int brackets = (line.size() > 1 && line[1] == '[') ? 2 : 1;
        index = skipBlanks(static_cast<std::string::size_type>(brackets));
        const std::string::size_type nameStart = index;
        while (index < line.size() && isBareKeyCharacter(line[index]))
            ++index;
        if (index == nameStart)
            return false;
        index = skipBlanks(index);
        for (int closed = 0; closed < brackets; ++closed, ++index) {
            if (index >= line.size() || line[index] != ']')
                return false;
        }
        // An element that is itself an array, `[1],`, has more after it.
        index = skipBlanks(index);
        return index == line.size() || line[index] == '#';
    }
    if (line.front() == '"') {
        const std::string::size_type closingQuote = line.find('"', 1);
        if (closingQuote == std::string::npos)
            return false;
        index = closingQuote + 1;
    } else {
        while (index < line.size() && isBareKeyCharacter(line[index]))
            ++index;
        if (!index)
            return false;
    }
    index = skipBlanks(index);
    return index < line.size() && line[index] == '=';
}

/// Applies one line that sits in no `[[...]]` entry and no platform block.
///
/// Five tables take any name — `[keys]`, `[link_handlers]`, `[triggers]`,
/// `[characterClass]` and `[macros]` — each a string kept in file order. Every
/// other key is a setting.
///
/// - Parameters:
///   - configuration: The configuration to fill.
///   - section: The `[section]` in force, empty at top level.
///   - key: The key as written.
///   - value: The parsed value.
/// - Returns: Whether the line was applied, named no setting, or held a value
///   its key does not take.
AssignmentResult placeTopLevel(Values& configuration, const std::string& section, const std::string& key, const Value& value)
{
    if (value.kind == Value::Kind::String) {
        // [keys] entries bind a key-spec to an action. The spec key is
        // usually a quoted string ("cmd+shift+k", or a chord
        // "ctrl+a ctrl+x") so it can carry '+' and spaces; a bare token
        // works too. The action is a string. Both are stored raw;
        // Input::BindingTable parses them so the parse stays portable and
        // host-testable apart from the TOML layer. The keyboard's own
        // settings share the table and are not bindings.
        const bool isKeyboardSetting = key == "compose_key" || key == "option_as_alt" || key == "accessory_symbols";
        if (section == "keys" && !isKeyboardSetting) {
            configuration.keyBindings.push_back(makeKeyBinding(key, value.str));
            return AssignmentResult::Assigned;
        }
        // [link_handlers] map a URL scheme to a command template (e.g.
        // jira = "open -a Jira %s"), stored raw. The frontend tokenizes into an
        // explicit argv (SECURITY: URL substituted as one argument, never a shell).
        if (section == "link_handlers") {
            configuration.linkHandlers.push_back(makeLinkHandler(key, value.str));
            return AssignmentResult::Assigned;
        }
        // [triggers] entries map a regex (the key) to an action spec (e.g.
        // "[0-9]+ tests? passed" = "notify:Tests"). Stored raw in file order;
        // the engine builder compiles the regex + splits the action on ':'.
        if (section == "triggers") {
            configuration.triggers.push_back(makeTrigger(key, value.str));
            return AssignmentResult::Assigned;
        }
        // [characterClass] entries map a smart-selection class name (the key) to its
        // regex (e.g. path = "[A-Za-z0-9_./-]+"). Stored raw in file order; the
        // double-click word-selection path compiles them into the engine.
        if (section == "characterClass") {
            configuration.characterClasses.push_back(makeCharacterClass(key, value.str));
            return AssignmentResult::Assigned;
        }
        // [macros] entries map a macro name (the key) to its body (e.g.
        // deploy = "git push\n"). Stored raw in file order; the frontend decodes
        // the body's escapes when the macro is invoked from a menu / keybinding.
        if (section == "macros") {
            configuration.macros.push_back(makeMacro(key, value.str));
            return AssignmentResult::Assigned;
        }
    }
    return assignReporting(configuration, section, key, value);
}

/// ``parse``, keeping for each key whose line it refuses the value the
/// document before this one applied.
///
/// A key the document states correctly takes what it states, and a key it
/// omits reads as its default. A key on a line the parse refuses — a value of
/// the wrong kind, a value that does not parse, an array that never closes, a
/// key under a header that cannot be read — keeps what `previous` applied, so
/// one bad line in a saved file moves nothing but itself. A key the reader does
/// not know is ignored: it may be a newer build's, and there is nothing to keep.
///
/// - Parameters:
///   - in: The TOML text to read.
///   - outValue: Filled with what parsed, populated first as for ``parse``.
///   - mode: As for ``parse``.
///   - previous: What the document before this one applied, in its order;
///     empty when there was none.
///   - placed: Receives what this document applied, kept values included, to
///     be the next document's `previous`.
/// - Returns: As for ``parse``.
bool parseKeepingRefusedValues(std::istream& in, Values& outValue, ParseMode mode, std::span<const Placement> previous, std::vector<Placement>& placed)
{
    std::string section; // current [section]; empty = top level
    bool isInTags = false; // current section is the most recent [[tags]]
    bool isInThemes = false; // current section is the most recent [[themes]]
    bool isInProfiles = false; // current section is the most recent [[profiles]]
    bool inLanguageFallback = false; // current section is the most recent [[font.language_fallbacks]]
    bool inRangeMap = false; // current section is the most recent [[font.range_map]]
    bool inPlatform = false; // current section is a [<plat>] override (or [<plat>.keys])
    // Set by a header the parse cannot read and cleared by the next one it can.
    // The lines between belong to a table nobody can name, so none of them is
    // applied to the table before it: a key there would land in the wrong one.
    bool isInUnreadableTable = false;
    Platform platformTarget = Platform::Mac;
    std::string line;
    bool ok = true;
    placed.clear();
    std::vector<Refusal> refusals;
    // The line a multi-line value met where it should have closed, handed back
    // so the walk reads it as the document's next line.
    std::optional<std::string> heldLine;
    const auto readLine = [&in, &heldLine](std::string& into) {
        if (heldLine) {
            into = std::move(*heldLine);
            heldLine.reset();
            return true;
        }
        return static_cast<bool>(std::getline(in, into));
    };
    // Cleared rather than appended to, unlike every config value here: a value
    // absent from this file keeps what an earlier layer set, which is the merge
    // this parse exists to do, but a key skipped while reading a different file
    // says nothing about this one. A caller reading the list after two parses
    // would otherwise be told about a file it did not ask about.
    outValue.skippedKeys.clear();
    // Counts every line the stream yields, including blanks, comments and the
    // continuation lines the nested-table skip consumes below, so the number
    // names the line an editor would jump to.
    int lineNumber = 0;
    // The header line of each `[[font.language_fallbacks]]` and
    // `[[font.range_map]]` entry, in entry order, so an entry the validation
    // below drops is named where it was written.
    std::vector<int> languageFallbackLines;
    std::vector<int> rangeMapLines;

    // Records a key the parse declined. Called at the assign sites rather than
    // inside assign(), because assign() is also how a value's own validation
    // reports a wrong type, and the caller is where the section and the line
    // are both still in hand. The line is the one the value starts on, however
    // many lines it spans.
    auto noteSkippedKey = [&outValue](const std::string& sectionName, const std::string& keyName, AssignmentResult why, int startLine) {
        // `why` carries which refusal it was; the expected kind is looked up
        // only to name it, and is empty for a key assignSpecial owns — those
        // take a shape no table describes, so the notice says less rather than
        // guessing.
        const char* expected = declaredValueKindForKey(sectionName, keyName);
        outValue.skippedKeys.push_back(SkippedKey { startLine, sectionName, keyName,
            why == AssignmentResult::InvalidValue ? SkipReason::WrongValueKind : SkipReason::UnknownKey,
            expected ? std::string(expected) : std::string() });
    };
    // Records a line the grammar refused before any key could be read from
    // it. Every structural refusal below goes through here, so a document a
    // frontend shows diagnostics for never fails without saying where.
    auto noteMalformedLine = [&outValue, &section](const std::string& text, int startLine) {
        outValue.skippedKeys.push_back(SkippedKey { startLine, section, text.substr(0, malformedLineQuoteLimit), SkipReason::MalformedLine, std::string() });
    };

    while (readLine(line)) {
        ++lineNumber;
        // Drop a trailing CR so CRLF files behave.
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);

        std::string t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;

        if (t[0] == '[') {
            isInUnreadableTable = false;
            // Array-of-tables header `[[themes]]` opens a fresh theme entry.
            if (t.size() >= 4 && t[1] == '[') {
                std::string::size_type close = t.find("]]");
                if (close == std::string::npos) {
                    noteMalformedLine(t, lineNumber);
                    ok = false;
                    isInUnreadableTable = true;
                    continue;
                }
                std::string hdr = trim(t.substr(2, close - 2));
                if (hdr == "themes") {
                    outValue.themes.emplace_back();
                    isInThemes = true;
                    isInTags = false;
                    isInProfiles = false;
                    inLanguageFallback = false;
                    inRangeMap = false;
                    inPlatform = false;
                    section.clear();
                    continue;
                }
                if (hdr == "tags") {
                    outValue.tags.emplace_back();
                    isInTags = true;
                    isInThemes = false;
                    isInProfiles = false;
                    inLanguageFallback = false;
                    inRangeMap = false;
                    inPlatform = false;
                    section.clear();
                    continue;
                }
                if (hdr == "profiles") {
                    outValue.profiles.emplace_back();
                    isInProfiles = true;
                    isInTags = false;
                    isInThemes = false;
                    inLanguageFallback = false;
                    inRangeMap = false;
                    inPlatform = false;
                    section.clear();
                    continue;
                }
                if (hdr == "font.language_fallbacks") {
                    outValue.languageFallbacks.emplace_back();
                    languageFallbackLines.push_back(lineNumber);
                    inLanguageFallback = true;
                    inRangeMap = false;
                    isInTags = false;
                    isInThemes = false;
                    isInProfiles = false;
                    inPlatform = false;
                    section.clear();
                    continue;
                }
                if (hdr == "font.range_map") {
                    outValue.rangeMap.emplace_back();
                    rangeMapLines.push_back(lineNumber);
                    inRangeMap = true;
                    inLanguageFallback = false;
                    isInThemes = false;
                    isInProfiles = false;
                    inPlatform = false;
                    section.clear();
                    continue;
                }
                // Unknown array-of-tables header: stop routing keys anywhere.
                isInTags = false;
                isInThemes = false;
                isInProfiles = false;
                inLanguageFallback = false;
                inRangeMap = false;
                inPlatform = false;
                section.clear();
                continue;
            }
            std::string hdr = parseSectionHeader(t);
            if (hdr.empty()) {
                noteMalformedLine(t, lineNumber);
                ok = false;
                isInUnreadableTable = true;
                continue;
            }
            isInTags = false;
            isInThemes = false;
            isInProfiles = false;
            inLanguageFallback = false;
            inRangeMap = false;

            // [platform] override axis (Docs/platform-config-and-sync.md): a
            // header whose first dotted segment is a platform name routes into
            // that platform's override slot.
            // `[mac]` sets Profile fields; `[mac.keys]` collects keybinding overrides.
            // present is marked so an empty block differs from an absent one.
            {
                std::string::size_type firstDot = hdr.find('.');
                std::string head = (firstDot == std::string::npos) ? hdr : hdr.substr(0, firstDot);
                Platform plat;
                if (platformFromString(head, plat)) {
                    inPlatform = true;
                    platformTarget = plat;
                    outValue.platformOverride(plat).present = true;
                    // Sub-section after the platform prefix; "keys" routes to
                    // the keybinding list, anything else (incl. bare) to the
                    // profile-field parser via the leaf name.
                    section = (firstDot == std::string::npos) ? std::string() : hdr.substr(hdr.rfind('.') + 1);
                    continue;
                }
            }
            inPlatform = false;

            // Flatten dotted headers to the leaf name only; this build
            // keys on single-level sections (font, appearance, ...). The
            // capability tables keep their whole name: their leaves are origin
            // names, and `[capabilities.remote]` flattened would read as the
            // unrelated `[remote]` section.
            if (hdr.starts_with(capabilitiesSectionPrefix)) {
                section = hdr;
                continue;
            }
            std::string::size_type dot = hdr.rfind('.');
            section = (dot == std::string::npos) ? hdr : hdr.substr(dot + 1);
            continue;
        }

        if (isInUnreadableTable) {
            // Applied to no table, and not named one by one: the header's own
            // diagnostic covers them. Each key keeps its value wherever it had one.
            if (const std::string::size_type separator = t.find('='); separator != std::string::npos) {
                if (std::string name = trim(t.substr(0, separator)); !name.empty())
                    refusals.push_back(Refusal { std::nullopt, std::move(name) });
            }
            continue;
        }

        std::string::size_type eq = t.find('=');
        if (eq == std::string::npos) {
            noteMalformedLine(t, lineNumber);
            ok = false;
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string rhs = trim(t.substr(eq + 1));
        if (key.empty()) {
            noteMalformedLine(t, lineNumber);
            ok = false;
            continue;
        }

        // Outside every `[[...]]` entry and platform block: a line whose key a
        // refusal keeps, and whose application a later document can keep.
        const bool isTopLevel = !inPlatform && !inLanguageFallback && !inRangeMap && !isInTags && !isInThemes && !isInProfiles;
        // Where the value starts; a diagnostic names this line however many
        // lines the value spans, because that is where its key is written.
        const int valueLine = lineNumber;
        bool isLeftOpen = false;

        // Multi-line array: fold subsequent lines into rhs until the
        // matching `]` is seen so a TOML literal split across rows
        // (e.g. ansi = [\n ... \n]) parses as one value.
        if (!rhs.empty() && rhs[0] == '[') {
            int depth = 0;
            bool inputString = false;
            for (size_t k = 0; k < rhs.size(); ++k) {
                char ch = rhs[k];
                if (!inputString) {
                    if (ch == '"')
                        inputString = true;
                    else if (ch == '[')
                        ++depth;
                    else if (ch == ']')
                        --depth;
                } else if (ch == '\\') {
                    if (k + 1 < rhs.size())
                        ++k;
                } else if (ch == '"') {
                    inputString = false;
                }
            }
            while (depth > 0) {
                if (!readLine(line)) {
                    isLeftOpen = true;
                    break;
                }
                // Counted here too: a continuation line consumed without
                // counting would put every later report one line short, and
                // the drift grows with each multi-line value in the file.
                ++lineNumber;
                if (!line.empty() && line[line.size() - 1] == '\r')
                    line.erase(line.size() - 1);
                std::string tt = trim(line);
                if (tt.empty())
                    continue;
                if (beginsTableOrAssignment(tt)) {
                    // The array was never closed. What follows it is the
                    // document's own, read again by the walk under its own
                    // number, rather than folded into a value that cannot parse.
                    heldLine = std::move(line);
                    --lineNumber;
                    isLeftOpen = true;
                    break;
                }
                rhs += ' ';
                rhs += tt;
                for (size_t k = 0; k < tt.size(); ++k) {
                    char ch = tt[k];
                    if (!inputString) {
                        if (ch == '"')
                            inputString = true;
                        else if (ch == '[')
                            ++depth;
                        else if (ch == ']')
                            --depth;
                    } else if (ch == '\\') {
                        if (k + 1 < tt.size())
                            ++k;
                    } else if (ch == '"') {
                        inputString = false;
                    }
                }
            }
        }

        Value v;
        if (isLeftOpen || !parseValue(rhs, v)) {
            // A setting the reader knows, written with something that is not a
            // value, was given the wrong kind of value: the sentence names
            // what it takes rather than quoting the line back.
            if (isTopLevel && declaredValueKindForKey(section, key))
                noteSkippedKey(section, key, AssignmentResult::InvalidValue, valueLine);
            else
                noteMalformedLine(t, valueLine);
            if (isTopLevel)
                refusals.push_back(Refusal { section, key });
            ok = false;
            continue;
        }

        // [platform] override block: route into the platform slot. A
        // `[<plat>.keys]` line is a keybinding override; a bare `[<plat>]`
        // line sets a Profile field. Keys/values reuse the same parsers as
        // the global [keys] section and [[profiles]] blocks.
        if (inPlatform) {
            PlatformOverride& po = outValue.platformOverride(platformTarget);
            if (section == "keys" && v.kind == Value::Kind::String)
                po.keyBindings.push_back(makeKeyBinding(key, v.str));
            else
                assignProfile(po.profile, key, v);
            continue;
        }

        if (inLanguageFallback && !outValue.languageFallbacks.empty()) {
            assignLanguageFallback(outValue.languageFallbacks.back(), key, v);
            continue;
        }

        if (inRangeMap && !outValue.rangeMap.empty()) {
            assignRangeMap(outValue.rangeMap.back(), key, v);
            continue;
        }

        if (isInTags && !outValue.tags.empty()) {
            assignTag(outValue.tags.back(), key, v);
            continue;
        }
        if (isInThemes && !outValue.themes.empty()) {
            assignTheme(outValue.themes.back(), key, v);
            continue;
        }
        if (isInProfiles && !outValue.profiles.empty()) {
            assignProfile(outValue.profiles.back(), key, v);
            continue;
        }
        const AssignmentResult why = placeTopLevel(outValue, section, key, v);
        if (why == AssignmentResult::Assigned) {
            placed.push_back(Placement { section, key, std::move(v) });
            continue;
        }
        noteSkippedKey(section, key, why, valueLine);
        // An unknown key keeps nothing: there is no value of it to keep, and
        // the name may be one only a newer build knows.
        if (why == AssignmentResult::InvalidValue)
            refusals.push_back(Refusal { section, key });
        if (mode == ParseMode::Strict)
            ok = false;
    }

    // A key whose line was refused keeps what the document before this one
    // applied, unless this document applied the key on another line. Placed
    // the way it was then, so a kept value is the value that was in force.
    if (!refusals.empty() && !previous.empty()) {
        const auto qualifiedName = [](const std::string& sectionName, const std::string& keyName) { return sectionName + '\n' + keyName; };
        std::unordered_set<std::string> applied;
        for (const Placement& placement : placed)
            applied.insert(qualifiedName(placement.section, placement.key));
        std::unordered_set<std::string> refusedInTable;
        std::unordered_set<std::string> refusedInAnyTable;
        for (const Refusal& refusal : refusals) {
            if (refusal.section)
                refusedInTable.insert(qualifiedName(*refusal.section, refusal.key));
            else
                refusedInAnyTable.insert(refusal.key);
        }
        for (const Placement& kept : previous) {
            const std::string name = qualifiedName(kept.section, kept.key);
            if (applied.contains(name) || !(refusedInTable.contains(name) || refusedInAnyTable.contains(kept.key)))
                continue;
            if (placeTopLevel(outValue, kept.section, kept.key, kept.value) == AssignmentResult::Assigned)
                placed.push_back(kept);
        }
    }

    std::erase_if(outValue.profiles, [](const Profile& profile) { return profile.scope == ProfileScope::Ephemeral; });
    // An entry the router cannot use is dropped, and named: a script it does
    // not know, a mode it does not know, a custom mode without a family, or a
    // range that does not parse is a rule the reader wrote and will not get.
    const auto dropInvalid = [&outValue, &ok, mode](auto& entries, const std::vector<int>& lines, const char* header, const char* takes,
                                 auto isValid) {
        std::size_t kept = 0;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (isValid(entries[index])) {
                if (kept != index)
                    entries[kept] = std::move(entries[index]);
                ++kept;
                continue;
            }
            const int line = index < lines.size() ? lines[index] : 0;
            outValue.skippedKeys.push_back(SkippedKey { line, std::string(), header, SkipReason::WrongValueKind, takes });
            if (mode == ParseMode::Strict)
                ok = false;
        }
        entries.resize(kept);
    };
    dropInvalid(outValue.languageFallbacks, languageFallbackLines, "[[font.language_fallbacks]]",
        "a known script, a known mode and, for a custom mode, a family",
        [](const LanguageFallbackEntry& entry) { return isValidLanguageFallback(entry); });
    dropInvalid(outValue.rangeMap, rangeMapLines, "[[font.range_map]]", "a code-point range, a known mode and, for a custom mode, a family",
        [](const RangeMapEntry& entry) { return isValidRangeMap(entry); });
    // Diagnostics read in document order, and the dropped entries were found
    // after the walk.
    std::stable_sort(outValue.skippedKeys.begin(), outValue.skippedKeys.end(),
        [](const SkippedKey& left, const SkippedKey& right) { return left.line < right.line; });
    return ok;
}

} // namespace

bool parse(std::istream& in, Values& outValue, ParseMode mode)
{
    std::vector<Placement> placed;
    return parseKeepingRefusedValues(in, outValue, mode, {}, placed);
}

std::optional<std::string> readWholeFile(const std::string& path)
{
    const int size = PAL::storageReadFile(path.c_str(), nullptr, 0);
    if (size < 0)
        return std::nullopt;
    // Sized before the read, so a read that then fails leaves a buffer of that
    // length holding nothing. Returned rather than written through a reference,
    // the caller never sees the intermediate.
    std::string body(static_cast<std::size_t>(size), '\0');
    if (size == 0)
        return body;
    const int read = PAL::storageReadFile(path.c_str(), &body[0], size);
    if (read < 0)
        return std::nullopt;
    body.resize(static_cast<std::size_t>(read));
    return body;
}

// A key the parse could not place is a setting the user wrote and will not get.
// Permissive parsing keeps the rest of the file working, which is the right
// trade for a typo in one line — but it leaves the user watching a setting have
// no effect with nothing anywhere saying why, and a misspelling is the likeliest
// thing in a hand-written file.
//
// Said on the way past, on the same terms as the unattended-capabilities notice:
// a line here is what makes it discoverable rather than invisible, and a
// frontend that can say so in its window should. Named per key with its line, so
// the message is a place to look rather than a claim that something is wrong.
namespace {

// The accepted tokens of a closed-vocabulary key, as `"a", "b" or "c"`, so a
// rejected value says what the key does take. Empty when the key has none.
std::string acceptedTokens(const SkippedKey& skipped)
{
    const std::span<const VocabularyTerm> terms = vocabularyFor(skipped.section.c_str(), skipped.key.c_str());
    std::string list;
    for (std::size_t index = 0; index < terms.size(); ++index) {
        if (index > 0)
            list += index + 1 == terms.size() ? " or " : ", ";
        list += std::string("\"") + terms[index].token + "\"";
    }
    return list;
}

// What a diagnostic says about its line, without saying which line.
std::string diagnosticBody(const SkippedKey& skipped)
{
    const std::string name = skipped.section.empty() ? skipped.key : skipped.section + "." + skipped.key;
    switch (skipped.reason) {
    case SkipReason::WrongValueKind:
        if (skipped.section == "window" && skipped.key == "drag_modifier")
            return name + " takes modifiers joined by \"+\", such as \"control+command\", \"shift\" or \"none\" — ignored";
        if (!skipped.expectedValueKind.empty())
            return name + " takes " + skipped.expectedValueKind + " — ignored";
        if (const std::string accepted = acceptedTokens(skipped); !accepted.empty())
            return name + " takes " + accepted + " — ignored";
        return name + " does not take this value — ignored";
    case SkipReason::MalformedLine:
        return "not a setting this reader understands: " + skipped.key + " — ignored";
    case SkipReason::UnknownKey:
        break;
    }
    return "no setting named " + name + " — ignored";
}

} // namespace

std::string diagnosticSentence(const SkippedKey& skipped) { return "line " + std::to_string(skipped.line) + ": " + diagnosticBody(skipped); }

// Only a file goes through here. A parse from a string is a caller building
// configuration in memory, which has no line for a user to open.
static void reportSkippedKeys(const std::string& path, const Values& configuration)
{
    for (const SkippedKey& skipped : configuration.skippedKeys)
        Ubiquitty::PAL::logLine(stderr, "ubiquitty: {}:{}: {}", path, skipped.line, diagnosticBody(skipped));
}

Profile applyPlatformOverride(const Profile& base, const Values& c, Platform p)
{
    const PlatformOverride& o = c.platformOverride(p);
    if (!o.present)
        return base;
    return resolve(base, o.profile); // platform fields win; unset inherit
}

Profile applyCurrentPlatformOverride(const Profile& base, const Values& c) { return applyPlatformOverride(base, c, currentPlatform()); }

std::vector<KeyBindingEntry> resolveKeyBindings(const Values& c, Platform p)
{
    std::vector<KeyBindingEntry> outValue = c.keyBindings; // global layer

    const PlatformOverride& o = c.platformOverride(p);
    for (const KeyBindingEntry& kb : o.keyBindings) {
        // A platform binding for an existing spec replaces it in place
        // (later layer wins); a new spec is appended.
        bool replaced = false;
        for (KeyBindingEntry& g : outValue) {
            if (g.spec == kb.spec) {
                g.action = kb.action;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            outValue.push_back(kb);
    }
    return outValue;
}

std::vector<KeyBindingEntry> resolveCurrentKeyBindings(const Values& c) { return resolveKeyBindings(c, currentPlatform()); }

// The configuration text, acquired through the PAL preferences seam.
//
// By KEY, not by path, because a path is not a thing every backend has. The
// browser stores bytes under a name and cannot open a file, so a Core computing
// `~/.config/...` is a Core that arm can only decline to serve, and an arm that
// declines grows a settings channel of its own that diverges from this one.
//
// Each backend decides what the key means: the file a person edits on macOS and
// Linux, page-supplied bytes in the browser. Core does not know where it lives.
std::optional<std::string> readConfigurationBlob()
{
    const int size = PAL::preferencesLoad(PAL::configurationBlobKey, nullptr, 0);
    if (size < 0)
        return std::nullopt;
    std::string body(static_cast<std::size_t>(size), '\0');
    if (size == 0)
        return body;
    const int read = PAL::preferencesLoad(PAL::configurationBlobKey, body.data(), size);
    if (read < 0)
        return std::nullopt;
    body.resize(static_cast<std::size_t>(read));
    return body;
}

namespace {

// Everything the one door owns, in one place: the values every reader aliases,
// the text they were parsed from, the epoch that moves with each replacement,
// and who is told. Kept together so no path can move one without the others.
struct LiveConfiguration {
    Values values;
    std::string document;
    /// What the `[scripting]` program last printed; merged into every
    /// document that opens the gate, so a by-key edit keeps its rules.
    std::string scriptOutput;
    /// What the parse of `document` did not apply.
    std::vector<SkippedKey> diagnostics;
    /// What the parse of `document` applied, kept values included: where the
    /// next document finds the value a key keeps when its line is refused.
    std::vector<Placement> placements;
    std::uint64_t epoch = 0;
    std::vector<const ConfigurationSubscription::Registration*> subscribers;
};

LiveConfiguration& live()
{
    assert(PAL::isMainThread());
    // Built to completion before it is published: a function-local static of
    // non-constant initialisation runs its initialiser exactly once and blocks
    // other threads until it finishes, so no caller can see a Values still
    // being filled, and no two first callers can each allocate one. A
    // constant-initialised pointer assigned before the parse carries neither
    // guarantee.
    //
    // Never deleted: holders keep the reference for the life of the process
    // (see values()), and a static destructor would race the shutdown
    // paths that still read it.
    static LiveConfiguration* const loaded = [] {
        auto* fresh = new LiveConfiguration();
        // Absent configuration is not a failure: an arm with nothing stored keeps
        // Core's compiled-in defaults, which is what a first run reads and what
        // a page reads until it supplies one.
        if (std::optional<std::string> body = readConfigurationBlob()) {
            std::istringstream in(*body);
            // Nothing applied before it, so a key on a refused line reads as
            // its default.
            (void)parseKeepingRefusedValues(in, fresh->values, ParseMode::Permissive, {}, fresh->placements);
            // Named by key rather than by path: the location is the backend's,
            // and a diagnostic naming the wrong file is worse than one naming
            // none.
            reportSkippedKeys(PAL::configurationBlobKey.name, fresh->values);
            // The text is kept beside what it parsed to, so a by-key edit
            // rewrites the document these values came from rather than an
            // empty one that would drop every other line the reader wrote.
            fresh->document = std::move(*body);
            fresh->diagnostics = fresh->values.skippedKeys;
        }
        return fresh;
    }();
    return *loaded;
}

} // namespace

const Values& values() { return live().values; }

PAL::TerminalIdentity terminalIdentity(const Values& configuration)
{
    // A view of the instance's own storage, never of a temporary. `value_or`
    // would build a string and return a view of it, which the compiler catches
    // as a dangling return -- the whole reason these are views is that the
    // caller reads them after this frame is gone.
    const auto view = [](const std::optional<std::string>& field) {
        return field.has_value() ? std::string_view { *field } : std::string_view {};
    };
    return { view(configuration.termIdentifier),
        configuration.termProgram.has_value() ? view(configuration.termProgram) : std::string_view { "Ubiquitty" },
        view(configuration.termProgramVersion) };
}

// Every configure path ends here: the values are replaced wholesale, because a
// parse is the unit a configure has and it cannot say which keys moved.
// Main-thread only — the replacement writes over storage every values()
// reference aliases.
static void replaceLiveValues(const Values& c)
{
    LiveConfiguration& state = live();
    state.values = c;
    // The blink curve every frontend's cursor and SGR blink read is the
    // configuration's, so it moves with every replacement rather than waiting
    // for a frontend to copy it across.
    Cursor::Blink::setAnimation(c.blinkAnimation.value_or(Defaults::blinkAnimation) ? Cursor::Blink::Animation::Fade : Cursor::Blink::Animation::OnOff);
    ++state.epoch;
    notifyConfigurationSubscribers(state.values);
}

std::uint64_t configurationEpoch() { return live().epoch; }

const std::string& liveDocument() { return live().document; }

std::string liveDiagnosticSummary()
{
    const std::vector<SkippedKey>& diagnostics = live().diagnostics;
    if (diagnostics.empty())
        return {};
    std::string summary = diagnosticSentence(diagnostics.front());
    if (diagnostics.size() > 1)
        summary += " (and " + std::to_string(diagnostics.size() - 1) + " more)";
    return summary;
}

namespace {

struct ParsedDocument {
    Values values;
    std::vector<Placement> placements;
    int mergedScriptDirectives = 0;
};

// One parse for every path into the live values: the document's own keys, each
// refused line keeping what `previous` applied, then the script's rules when
// the document opens the `[scripting]` gate.
ParsedDocument parseDocument(const std::string& text, ParseMode mode, const std::string& scriptOutput, std::span<const Placement> previous)
{
    ParsedDocument parsed;
    std::istringstream in(text);
    (void)parseKeepingRefusedValues(in, parsed.values, mode, previous, parsed.placements);
    // The gate `Effective::scriptingHookProgram` reads, spelled on the fields:
    // that accessor's unit links the theme registry, which this one must not.
    const bool isGateOpen = parsed.values.scriptingEnable.value_or(Defaults::scriptingEnable)
        && !parsed.values.scriptingConfigScript.value_or(std::string()).empty();
    if (!scriptOutput.empty() && isGateOpen)
        parsed.mergedScriptDirectives = mergeScriptingRules(parsed.values, scriptOutput);
    return parsed;
}

} // namespace

Values previewConfiguration(const std::string& document)
{
    return parseDocument(document, ParseMode::Permissive, live().scriptOutput, live().placements).values;
}

ConfigureResult configure(const std::string& document, ParseMode mode)
{
    std::string text = document;
    ParsedDocument parsed = parseDocument(text, mode, live().scriptOutput, live().placements);
    ConfigureResult result;
    result.diagnostics = parsed.values.skippedKeys;
    // Strict refuses on any diagnostic, not only on the ones `parse` itself
    // fails for: a fixture that draws with a key missing is the defect the
    // strict mode exists to catch.
    if (mode == ParseMode::Strict && !result.diagnostics.empty()) {
        result.epoch = configurationEpoch();
        return result;
    }
    live().document = std::move(text);
    live().diagnostics = result.diagnostics;
    live().placements = std::move(parsed.placements);
    replaceLiveValues(parsed.values);
    result.isApplied = true;
    result.epoch = configurationEpoch();
    return result;
}

int configureScriptOutput(const std::string& output)
{
    LiveConfiguration& state = live();
    if (output.empty() && state.scriptOutput.empty())
        return 0;
    state.scriptOutput = output;
    ParsedDocument parsed = parseDocument(state.document, ParseMode::Permissive, state.scriptOutput, state.placements);
    state.placements = std::move(parsed.placements);
    replaceLiveValues(parsed.values);
    return parsed.mergedScriptDirectives;
}

ConfigureResult configureFromStoredDocument(ParseMode mode)
{
    const ConfigureResult result = configure(readConfigurationBlob().value_or(std::string()), mode);
    Values reported;
    reported.skippedKeys = result.diagnostics;
    reportSkippedKeys(PAL::configurationBlobKey.name, reported);
    return result;
}

ConfigureResult configureFromFile(const std::string& path, ParseMode mode)
{
    const std::optional<std::string> body = readWholeFile(path);
    const ConfigureResult result = configure(body.value_or(std::string()), mode);
    Values reported;
    reported.skippedKeys = result.diagnostics;
    reportSkippedKeys(path, reported);
    return result;
}

ConfigurationSubscription::ConfigurationSubscription(Callback callback)
    : m_registration(std::make_unique<Registration>(Registration { std::move(callback) }))
{
    live().subscribers.push_back(m_registration.get());
}

ConfigurationSubscription ConfigurationSubscription::notifying(std::function<void()> callback)
{
    return ConfigurationSubscription([callback = std::move(callback)](const Values&) {
        if (callback)
            callback();
    });
}

ConfigurationSubscription::ConfigurationSubscription(ConfigurationSubscription&&) noexcept = default;

ConfigurationSubscription& ConfigurationSubscription::operator=(ConfigurationSubscription&& other) noexcept
{
    if (this != &other) {
        if (m_registration)
            std::erase(live().subscribers, m_registration.get());
        m_registration = std::move(other.m_registration);
    }
    return *this;
}

ConfigurationSubscription::~ConfigurationSubscription()
{
    if (m_registration)
        std::erase(live().subscribers, m_registration.get());
}

void notifyConfigurationSubscribers(const Values& values)
{
    // Walks a copy and re-checks membership before each call, because a
    // callback may destroy a subscription — its own or another's — and a
    // destroyed one must not be called through the pointer the copy still holds.
    const std::vector<const ConfigurationSubscription::Registration*> snapshot = live().subscribers;
    for (const ConfigurationSubscription::Registration* registration : snapshot) {
        const std::vector<const ConfigurationSubscription::Registration*>& current = live().subscribers;
        if (std::find(current.begin(), current.end(), registration) == current.end())
            continue;
        // Called through a copy: a callback that ends its own subscription
        // frees the registration while it runs.
        const ConfigurationSubscription::Callback callback = registration->callback;
        if (callback)
            callback(values);
    }
}

} // namespace Ubiquitty::Preferences
