// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The static per-setting facts table: min / max / step / unit. The one place
// these numbers live — the TOML parse clamp consults rangeFor(), and the
// Values UI reads the same table. Pure header (no platform types).

namespace Ubiquitty::Preferences::Schema {

// One numeric setting's bounds. `step`/`unit` describe the UI affordance; the
// TOML clamp uses only [minimum, maximum]. `unit` is a display suffix
// ("pt", "x", "ln", "fr") or "" for a unitless / percentage control.
struct NumericRange {
    double minimum;
    double maximum;
    double step;
    const char* unit;
};

// The section.key → range table. A key absent here is unbounded at parse time —
// a theme index or a raw string has no range to check against.
struct RangeEntry {
    const char* section;
    const char* key;
    NumericRange range;
};

inline constexpr RangeEntry ranges[] = {
    // [font]
    { "font", "size", { 6.0, 96.0, 0.25, "pt" } }, // Font Size stepper, quarter-point
    { "font", "proportional_size", { 0.0, 96.0, 0.25, "pt" } }, // Proportional Size stepper; 0 takes one optical step over font.size
    { "font", "line_spacing", { 0.0, 16.0, 0.25, "pt" } }, // Line Spacing stepper
    // Character Spacing stepper, subpixel. The step is finer than a tier can
    // always honour: a CoreText grid tracks by the exact value and leaves the
    // whole gap after the glyph, while the GNU Unifont bitmap grid and the
    // Linux FreeType grid round the value to whole backing pixels — so a
    // fraction under half a pixel spaces nothing there — and the bitmap grid
    // alone splits its gap around the glyph rather than banking it after.
    { "font", "character_spacing", { -2.0, 4.0, 0.01, "pt" } },
    { "font", "baseline_offset", { -96.0, 96.0, 0.25, "pt" } },
    { "font", "weight", { 0.0, 900.0, 50.0, "" } }, // Font Weight picker; 0 is the face's own
    { "font", "bold_weight", { 0.0, 900.0, 50.0, "" } }, // Bold Weight picker; 0 is the face's own
    { "font", "width", { 50.0, 200.0, 0.5, "%" } }, // Font Width stepper; unset is the face's own
    { "font", "wide_glyph_scale", { 1.0, 1.5, 0.01, "x" } }, // Wide Glyph Scale stepper
    // [underline]
    { "underline", "thickness", { 0.5, 4.0, 0.25, "x" } }, // Underline Thickness stepper
    { "underline", "offset", { -4.0, 8.0, 0.5, "pt" } }, // Underline Offset stepper
    { "underline", "skip_ink_pad", { 0.0, 3.0, 0.25, "pt" } }, // Skip-Ink Pad stepper
    // [appearance] padding — all four edges share one range
    { "appearance", "padding", { 0.0, 64.0, 1.0, "pt" } },
    { "appearance", "padding_top", { 0.0, 64.0, 1.0, "pt" } },
    { "appearance", "padding_bottom", { 0.0, 64.0, 1.0, "pt" } },
    { "appearance", "padding_left", { 0.0, 64.0, 1.0, "pt" } },
    { "appearance", "padding_right", { 0.0, 64.0, 1.0, "pt" } },
    { "appearance", "titleless_padding_top", { 0.0, 64.0, 1.0, "pt" } },
    // [appearance] window background
    { "appearance", "background_opacity", { 0.0, 1.0, 0.0, "" } }, // Opacity slider
    { "appearance", "blur_radius", { 0.0, 50.0, 0.0, "pt" } }, // Blur Radius slider
    { "appearance", "background_image_opacity", { 0.0, 1.0, 0.0, "" } }, // Background Image Opacity slider
    { "appearance", "ui_font_scale", { 0.5, 2.0, 0.05, "x" } }, // UI Font Scale slider (chrome)
    { "appearance", "tag_color_opacity", { 0.1, 1.0, 0.05, "" } }, // Tag Color Strength slider
    // [bell]
    { "bell", "visual_intensity", { 0.05, 1.0, 0.0, "" } }, // Visual-bell intensity
    // [sound]
    { "sound", "volume", { 0.0, 1.0, 0.05, "" } }, // DECPS master volume slider
    // [cursor]
    { "cursor", "linger", { 0.0, 240.0, 5.0, "fr" } }, // Cursor Linger stepper
    { "cursor", "blink_rate", { 200.0, 2000.0, 50.0, "ms" } }, // Cursor Blink Rate stepper (period, one full cycle)
    { "cursor", "alpha", { 0.0, 255.0, 1.0, "" } }, // Cursor Alpha slider (0..1 of 255)
    // [paste]
    { "paste", "confirm_multiline_threshold", { 1.0, 1000.0, 1.0, "ln" } }, // Multi-line paste threshold stepper
    // [mouse]
    { "mouse", "wheel_speed", { 0.1, 10.0, 0.0, "" } }, // Wheel Speed slider
    { "mouse", "wheel_lines", { 1.0, 20.0, 1.0, "ln" } }, // Wheel Lines stepper
    { "mouse", "trackpad_speed", { 0.1, 4.0, 0.0, "" } }, // Trackpad Speed slider
    // [terminal]
    { "terminal", "scrollback_lines", { 1000.0, 100000.0, 1000.0, "ln" } }, // Scrollback stepper
};

inline constexpr int rangeCount = static_cast<int>(sizeof(ranges) / sizeof(ranges[0]));

// Plain string compare so the lookup is constexpr-friendly and pulls in no
// <cstring> (this header is included by the portable Values parser).
constexpr bool sameKey(const char* a, const char* b)
{
    while (*a && *b) {
        if (*a != *b)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// The range for section.key, or nullptr when the key has no declared bounds.
constexpr const NumericRange* rangeFor(const char* section, const char* key)
{
    for (int i = 0; i < rangeCount; ++i) {
        if (sameKey(ranges[i].section, section) && sameKey(ranges[i].key, key))
            return &ranges[i].range;
    }
    return nullptr;
}

// Clamp `value` to section.key's declared [minimum, maximum]; a key with no
// declared range is returned unchanged.
inline double clampToRange(double value, const char* section, const char* key)
{
    const NumericRange* r = rangeFor(section, key);
    if (!r)
        return value;
    if (value < r->minimum)
        return r->minimum;
    if (value > r->maximum)
        return r->maximum;
    return value;
}

// Integer overload; table bounds are whole numbers so the narrowing is exact.
inline int clampToRange(int value, const char* section, const char* key)
{
    const NumericRange* r = rangeFor(section, key);
    if (!r)
        return value;
    if (value < static_cast<int>(r->minimum))
        return static_cast<int>(r->minimum);
    if (value > static_cast<int>(r->maximum))
        return static_cast<int>(r->maximum);
    return value;
}

} // namespace Ubiquitty::Preferences::Schema
