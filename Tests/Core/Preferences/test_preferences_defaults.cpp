// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_preferences_defaults.cpp — pins the single-sourced settings defaults and
// the settings schema ranges.
//
// Two contracts:
//   1. Defaults / PreferencesAccessors: the effective-value accessor for an
//      UNSET key returns the canonical default, and a SET key returns the
//      stored value. The headline regression guard is
//      underlineSkipInkPad == 1.75 — one value, read the same by the macOS
//      interop, the ctor and the renderer.
//   2. Preferences::Schema: rangeFor() reports the documented bounds, and the TOML
//      parse clamp honors a schema range for every key the table bounds.

#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/PreferencesDefaults.h"
#include "Core/Preferences/RenderConfigurationFold.h"
#include "Core/Preferences/Schema.h"
#include "Core/VT/Constants.h"

#include <cstdio>
#include <sstream>
#include <string>

using Ubiquitty::Preferences::CursorShape;
using Ubiquitty::Preferences::parse;
using Ubiquitty::Preferences::TabShortcutHints;
using Ubiquitty::Preferences::TitlebarStyle;
using Ubiquitty::Preferences::Values;
namespace Defaults = Ubiquitty::Preferences::Defaults;

namespace Schema = Ubiquitty::Preferences::Schema;

static Values parseString(const std::string& text)
{
    Values c;
    std::istringstream in(text);
    parse(in, c);
    return c;
}

/// The underlineSkipInkPad default is shared by every accessor.
///
/// Two places state it: Defaults holds the number and the accessor answers it
/// over an unset field. The bug this guards is those two disagreeing. A third
/// place is unrepresentable — an unset field is empty, so the constructor has no
/// default to state and cannot contradict one.
TEST(testSkipInkPadIsOneAuthoritativeDefault)
{
    CHECK(Defaults::underlineSkipInkPad == 1.75);
    Values c; // unset
    CHECK(!c.underlineSkipInkPad.has_value());
    CHECK(Ubiquitty::Preferences::Effective::underlineSkipInkPad(c) == 1.75);
}

/// Each target's built-in padding, and a stated value outranking it.
///
/// iOS pads 4 pt on every edge where a desktop window pads its sides and bottom
/// and leaves its top to the title bar. The default is Core's, chosen by
/// target, so a host test can ask for iOS's without being an iOS build.
TEST(testPaddingDefaultsPerTargetAndYieldsToAStatedValue)
{
    using Ubiquitty::Preferences::DefaultsTarget;
    const Values unset;
    const Values::PaddingEdges phone = unset.resolvedPadding(DefaultsTarget::IOS);
    CHECK_MSG(phone.left == 4.0 && phone.right == 4.0 && phone.top == 4.0 && phone.bottom == 4.0,
        "an iOS pane pads 4 pt on every edge when nothing is stated");
    const Values::PaddingEdges desktop = unset.resolvedPadding(DefaultsTarget::Desktop);
    CHECK_MSG(desktop.left == 6.0 && desktop.right == 6.0 && desktop.top == 0.0 && desktop.bottom == 6.0,
        "a desktop window keeps 6 pt sides and bottom and no top");
    const Values::PaddingEdges current = unset.resolvedPadding();
    const Values::PaddingEdges built = unset.resolvedPadding(Ubiquitty::Preferences::currentDefaultsTarget());
    CHECK_MSG(current.left == built.left && current.top == built.top, "the plain resolution is this build's target's");

    const Values oneEdge = parseString("[appearance]\npadding_left = 10\n");
    const Values::PaddingEdges stated = oneEdge.resolvedPadding(DefaultsTarget::IOS);
    CHECK_MSG(stated.left == 10.0, "a stated edge outranks the iOS default");
    CHECK_MSG(stated.right == 4.0 && stated.top == 4.0 && stated.bottom == 4.0, "the unstated edges keep the iOS default");

    const Values shorthand = parseString("[appearance]\npadding = 0\npadding_bottom = 12\n");
    const Values::PaddingEdges flat = shorthand.resolvedPadding(DefaultsTarget::IOS);
    CHECK_MSG(flat.left == 0.0 && flat.right == 0.0 && flat.top == 0.0, "the shorthand outranks the iOS default");
    CHECK_MSG(flat.bottom == 12.0, "a per-edge key outranks the shorthand on iOS too");
}

/// The effective accessors return the canonical default when a key is unset.
TEST(testAccessorsReturnDefaultsWhenUnset)
{
    Values c;
    CHECK(Ubiquitty::Preferences::Effective::fontSize(c) == Defaults::fontSize);
    CHECK(Ubiquitty::Preferences::Effective::fontSize(c) == 12.0);
    CHECK(Ubiquitty::Preferences::Effective::ligatures(c) == true);
    CHECK(Ubiquitty::Preferences::Effective::lineSpacing(c) == 2.0);
    CHECK(Ubiquitty::Preferences::Effective::wideGlyphBoost(c) == true);
    CHECK(Ubiquitty::Preferences::Effective::wideGlyphScale(c) == 1.2);
    CHECK(Ubiquitty::Preferences::Effective::underlineThickness(c) == 1.0);
    CHECK(Ubiquitty::Preferences::Effective::underlineOffset(c) == 0.0);
    CHECK(Ubiquitty::Preferences::Effective::underlineSkipInk(c) == true);
    CHECK(Ubiquitty::Preferences::Effective::underlineLinks(c) == true);
    CHECK(Ubiquitty::Preferences::Effective::linkUnderlineStyle(c) == 0);
    CHECK(Ubiquitty::Preferences::Effective::backgroundOpacity(c) == 1.0);
    CHECK(Ubiquitty::Preferences::Effective::textContrast(c) == Defaults::shouldEnhanceTextContrast);
    CHECK(!Ubiquitty::Preferences::Effective::textContrast(c));
    CHECK(Ubiquitty::Preferences::Effective::cursorShape(c) == CursorShape::Bar);
    CHECK(Ubiquitty::Preferences::Effective::cursorLinger(c) == 40);
    CHECK(Ubiquitty::Preferences::Effective::cursorBlinkRate(c) == 1000);
    CHECK(Ubiquitty::Preferences::Effective::cursorAlpha(c) == 255);
    CHECK(Ubiquitty::Preferences::Effective::bellAudible(c) == true);
    CHECK(Ubiquitty::Preferences::Effective::bellVisual(c) == false);
    CHECK(Ubiquitty::Preferences::Effective::bellVisualIntensity(c) == 0.35);
    CHECK(Ubiquitty::Preferences::Effective::wheelSpeed(c) == 1.0);
    CHECK(Ubiquitty::Preferences::Effective::wheelLines(c) == 1);
    CHECK(Ubiquitty::Preferences::Effective::trackpadSpeed(c) == Ubiquitty::Preferences::defaultTrackpadSpeed);
    CHECK(Ubiquitty::Preferences::Effective::scrollbackLines(c) == Ubiquitty::Preferences::defaultScrollbackLines);
    CHECK(Ubiquitty::Preferences::Effective::tabShortcutHints(c) == TabShortcutHints::Always);
    CHECK(Ubiquitty::Preferences::Effective::titlebarStyle(c) == TitlebarStyle::Themed);
    CHECK(Ubiquitty::Preferences::Effective::blockSelectModifier(c) == "opt");
    CHECK(Ubiquitty::Preferences::Effective::osc8ClickOpens(c) == true);
    CHECK(Ubiquitty::Preferences::Effective::autoDeployHostOnRemoteConnect(c) == true);
    CHECK(Ubiquitty::Preferences::Effective::colorSpaceP3(c) == true);
}

/// A set key reads through the accessor unchanged (the "field" half of the
/// field.Set ? field : DEFAULT contract).
TEST(testAccessorsPassThroughWhenSet)
{
    Values c = parseString("[font]\nsize = 18.0\n"
                           "[underline]\nskip_ink_pad = 2.0\n"
                           "[appearance]\ncolor_space = \"display-p3\"\n");
    CHECK(c.fontSize.has_value());
    CHECK(Ubiquitty::Preferences::Effective::fontSize(c) == 18.0);
    CHECK(c.underlineSkipInkPad.has_value());
    CHECK(Ubiquitty::Preferences::Effective::underlineSkipInkPad(c) == 2.0);
    CHECK(Ubiquitty::Preferences::Effective::colorSpaceP3(c) == true); // display-p3 -> P3
}

TEST(testTheRenderConfigurationResolvesOnceForEveryFrontend)
{
    Values c = parseString("[font]\nfamily = \"JetBrains Mono\"\nsize = 14\nline_spacing = 8\n"
                           "[appearance]\npadding_left = 2\npadding_right = 4\npadding_top = 6\npadding_bottom = 8\n"
                           "[underline]\nthickness = 2.5\noffset = -1\nskip_ink = false\nskip_ink_pad = 3\n");
    const Ubiquitty::Rendering::RenderConfiguration settings = Ubiquitty::Preferences::Effective::renderConfiguration(c);
    CHECK(settings.fontFamily.view() == "JetBrains Mono");
    CHECK(settings.fontSizePoints == 14.0);
    CHECK(settings.lineSpacingPoints == 8.0);
    CHECK(settings.paddingLeftPoints == 2.0);
    CHECK(settings.paddingRightPoints == 4.0);
    CHECK(settings.paddingTopPoints == 6.0);
    CHECK(settings.paddingBottomPoints == 8.0);
    CHECK(settings.underlineThicknessScale == 2.5);
    CHECK(settings.underlineOffsetPoints == -1.0);
    CHECK(!settings.shouldUnderlineSkipInk);
    CHECK(settings.underlineSkipInkPaddingPoints == 3.0);

    const Ubiquitty::Rendering::RenderConfiguration defaults = Ubiquitty::Preferences::Effective::renderConfiguration(Values {});
    CHECK(defaults.fontFamily.view().empty());
    CHECK(defaults.fontSizePoints == Defaults::fontSize);
    CHECK(defaults.lineSpacingPoints == Defaults::lineSpacing);
    CHECK(defaults.underlineThicknessScale == Defaults::underlineThickness);
}

/// [triggers] allow_run_command + allow_webhook are SECURITY GATES: both
/// default closed and only the explicit `= true` opens them. The webhook gate
/// mirrors the run_command gate so a webhook rule never POSTs unless opted in.
TEST(testTriggerSecurityGatesDefaultClosed)
{
    Values def = parseString("");
    CHECK(Ubiquitty::Preferences::Effective::allowRunCommand(def) == false);
    CHECK(Ubiquitty::Preferences::Effective::allowWebhook(def) == false);
    Values on = parseString("[triggers]\nallow_run_command = true\nallow_webhook = true\n");
    CHECK(Ubiquitty::Preferences::Effective::allowRunCommand(on) == true);
    CHECK(Ubiquitty::Preferences::Effective::allowWebhook(on) == true);

    // The [scripting] config_script hook is gated the same way.
    CHECK(Ubiquitty::Preferences::Effective::scriptingEnable(def) == false);
    Values scriptingOn = parseString("[scripting]\nenable = true\n");
    CHECK(Ubiquitty::Preferences::Effective::scriptingEnable(scriptingOn) == true);
}

/// The color_space accessor (the macOS Values-getter path) makes one P3
/// decision from the canonical token; `p3` is not a token, so the parse
/// refuses it and the default stands.
TEST(testColorSpaceTokensReconcile)
{
    CHECK(Ubiquitty::Preferences::Effective::colorSpaceP3(parseString("[appearance]\ncolor_space = \"display-p3\"\n")) == true);
    const Values alias = parseString("[appearance]\ncolor_space = \"p3\"\n");
    CHECK(!alias.colorSpace.has_value());
    CHECK(alias.skippedKeys.size() == 1 && alias.skippedKeys[0].reason == Ubiquitty::Preferences::SkipReason::WrongValueKind);
    CHECK(Ubiquitty::Preferences::Effective::colorSpaceP3(parseString("[appearance]\ncolor_space = \"srgb\"\n")) == false);
    CHECK(Ubiquitty::Preferences::Effective::colorSpaceP3(parseString("[appearance]\ncolor_space = \"rec2020\"\n")) == Defaults::colorSpaceP3);
    CHECK(Ubiquitty::Preferences::Effective::colorSpaceP3(parseString("")) == true);
}

/// The blur is `blur_radius` alone: `background_blur` is not a key, so a file
/// naming it is refused by the strict parse and moves no radius.
TEST(testBlurRadiusIsTheOnlyBlurKey)
{
    Values strict;
    std::istringstream strictInput("[appearance]\nbackground_blur = false\n");
    CHECK(!parse(strictInput, strict, Ubiquitty::Preferences::ParseMode::Strict));
    Values off = parseString("[appearance]\nbackground_blur = false\n");
    CHECK(Ubiquitty::Preferences::Effective::blurRadius(off) == Defaults::blurRadius);
    CHECK(off.skippedKeys.size() == 1 && off.skippedKeys[0].reason == Ubiquitty::Preferences::SkipReason::UnknownKey);

    Values explicitRadius = parseString("[appearance]\nblur_radius = 0.0\n");
    CHECK(Ubiquitty::Preferences::Effective::blurRadius(explicitRadius) == 0.0);
}

TEST(defaultFontPrefersSFMonoWithBundledFallbackAtTwelvePoints)
{
    CHECK_STR_EQ(Defaults::houseFontFamilyAt(0), "SF Mono");
    CHECK_STR_EQ(Defaults::houseFontFamilyAt(1), "JetBrains Mono");
    CHECK_EQ(Ubiquitty::Preferences::Effective::fontSize(Values {}), 12.0);
}

/// The schema reports the documented ranges.
TEST(testSchemaRanges)
{
    const Schema::NumericRange* fontSize = Schema::rangeFor("font", "size");
    CHECK(fontSize != nullptr);
    CHECK(fontSize->minimum == 6.0);
    CHECK(fontSize->maximum == 96.0);
    // A quarter point, the granularity the font code already works in: the
    // stepper reaches a size between two points rather than whole ones only.
    CHECK(fontSize->step == 0.25);

    const Schema::NumericRange* proportionalSize = Schema::rangeFor("font", "proportional_size");
    CHECK(proportionalSize != nullptr);
    CHECK(proportionalSize->step == 0.25);

    const Schema::NumericRange* lineSpacing = Schema::rangeFor("font", "line_spacing");
    CHECK(lineSpacing != nullptr);
    CHECK(lineSpacing->step == 0.25);

    const Schema::NumericRange* wideGlyphScale = Schema::rangeFor("font", "wide_glyph_scale");
    CHECK(wideGlyphScale != nullptr);
    CHECK(wideGlyphScale->minimum == 1.0);
    CHECK(wideGlyphScale->maximum == 1.5);
    CHECK(wideGlyphScale->step == 0.01);

    const Schema::NumericRange* pad = Schema::rangeFor("appearance", "padding_left");
    CHECK(pad != nullptr);
    CHECK(pad->minimum == 0.0);
    CHECK(pad->maximum == 64.0);

    const Schema::NumericRange* skipPad = Schema::rangeFor("underline", "skip_ink_pad");
    CHECK(skipPad != nullptr);
    CHECK(skipPad->minimum == 0.0);
    CHECK(skipPad->maximum == 3.0);

    // An unlisted key has no declared range.
    CHECK(Schema::rangeFor("font", "family") == nullptr);
    CHECK(Schema::rangeFor("nope", "nope") == nullptr);
}

/// The parse clamp is single-sourced from the schema for the keys that opt in
/// via the useSchema sentinel (the bounds that protect rendering / scrolling):
/// an over-range value clamps to the schema maximum, an under-range value to the
/// schema minimum — and the numbers match Schema.h, not a duplicate.
TEST(testParseClampHonorsSchema)
{
    // wheel_speed schema range is 0.1 .. 10.0.
    Values fast = parseString("[mouse]\nwheel_speed = 50\n");
    CHECK(fast.wheelSpeed.has_value());
    CHECK(fast.wheelSpeed == 10.0);
    CHECK(fast.wheelSpeed == Schema::rangeFor("mouse", "wheel_speed")->maximum);

    Values slow = parseString("[mouse]\nwheel_speed = 0.0\n");
    CHECK(slow.wheelSpeed == 0.1);
    CHECK(slow.wheelSpeed == Schema::rangeFor("mouse", "wheel_speed")->minimum);

    // Zero theme tint leaves the native material visible.
    Values ghost = parseString("[appearance]\nbackground_opacity = 0.0\n");
    CHECK(ghost.backgroundOpacity == 0.0);

    // wheel_lines is an int key driven by the schema (1 .. 20).
    Values many = parseString("[mouse]\nwheel_lines = 999\n");
    CHECK(many.wheelLines == 20);

    Values tallWide = parseString("[font]\nwide_glyph_scale = 10.0\n");
    CHECK(tallWide.wideGlyphScale == 1.5);
}

/// The display-only steppers (font size, skip-ink pad, …) are not
/// clamped at parse to the UI range, so a config value the slider can't reach is
/// still honored. The schema still publishes their UI bounds for the frontend.
TEST(testDisplaySteppersUnboundedAtParse)
{
    Values big = parseString("[font]\nsize = 99\n");
    CHECK(big.fontSize.has_value());
    CHECK(big.fontSize == 99.0); // honored past the UI's 96 ceiling
    CHECK(Schema::rangeFor("font", "size")->maximum == 96.0); // UI bound still published

    Values bigPad = parseString("[underline]\nskip_ink_pad = 99\n");
    CHECK(bigPad.underlineSkipInkPad == 99.0);
}

/// An explicit parse clamp wider than the schema (scrollback_lines 1..1000000)
/// is not narrowed to the schema's UI range (1000..100000) — the parse bound is
/// what a hand-edited config is read against, so it is the one that wins.
TEST(testParseExplicitClampOverridesSchema)
{
    Values c = parseString("[terminal]\nscrollback_lines = 50\n");
    CHECK(c.scrollbackLines.has_value());
    CHECK(c.scrollbackLines == 50); // honored, not raised to the UI floor of 1000
}

/// Schema::clampToRange is the one clamp every frontend settings setter calls;
/// for every key in ranges it must clamp to exactly that row's [minimum,
/// maximum], and pass an in-range value through unchanged. This is what stops a
/// setter's hardcoded literal from ever disagreeing with the schema (and with
/// the UI control reading the same row).
TEST(testClampToRangeMatchesKrangesForEveryKey)
{
    for (int i = 0; i < Schema::rangeCount; ++i) {
        const Schema::RangeEntry& e = Schema::ranges[i];
        const double lo = e.range.minimum;
        const double hi = e.range.maximum;
        const double mid = lo + (hi - lo) * 0.5;

        // Under-range -> minimum, over-range -> maximum, in-range -> unchanged.
        CHECK(Schema::clampToRange(lo - 1.0, e.section, e.key) == lo);
        CHECK(Schema::clampToRange(hi + 1.0, e.section, e.key) == hi);
        CHECK(Schema::clampToRange(mid, e.section, e.key) == mid);

        // The int overload agrees on the whole-number bounds.
        CHECK(Schema::clampToRange(static_cast<int>(lo) - 1, e.section, e.key) == static_cast<int>(lo));
        CHECK(Schema::clampToRange(static_cast<int>(hi) + 1, e.section, e.key) == static_cast<int>(hi));
    }

    // An unlisted key has no declared bounds, so the value is returned as-is.
    CHECK(Schema::clampToRange(123.0, "font", "family") == 123.0);
    CHECK(Schema::clampToRange(7, "nope", "nope") == 7);
}

/// The bug guard: a macOS scrollback setter clamping 1..1000000 disagrees with
/// the schema's 1000..100000. Routed through clampToRange, a
/// below-floor request rises to 1000 and an above-ceiling request falls to
/// 100000 — exactly the bounds the stepper shows.
TEST(testScrollbackSetterClampIs1000To100000)
{
    const Schema::NumericRange* r = Schema::rangeFor("terminal", "scrollback_lines");
    CHECK(r != nullptr);
    CHECK(r->minimum == 1000.0);
    CHECK(r->maximum == 100000.0);

    // The path the macOS settingsSetScrollbackLines: takes.
    CHECK(Schema::clampToRange(1, "terminal", "scrollback_lines") == 1000); // below the floor, raised to it
    CHECK(Schema::clampToRange(999, "terminal", "scrollback_lines") == 1000);
    CHECK(Schema::clampToRange(50000, "terminal", "scrollback_lines") == 50000); // in range, kept
    CHECK(Schema::clampToRange(1000000, "terminal", "scrollback_lines") == 100000); // above the ceiling, lowered to it
}

// The font-zoom clamp is single-sourced: both frontends call Zoom::clampFontZoom,
// which reads the one font.size schema row. Per-frontend pins ([6, 96] against
// [4, 256]) put the same keystroke on a different ceiling per platform.
TEST(testFontZoomClampSingleSourced)
{
    namespace Zoom = Ubiquitty::Preferences::Zoom;

    const Schema::NumericRange& range = Zoom::fontZoomRange();
    const Schema::NumericRange* fontSize = Schema::rangeFor("font", "size");
    CHECK(fontSize != nullptr);
    // The zoom range is the font.size row — one source, not a parallel literal.
    CHECK(range.minimum == fontSize->minimum);
    CHECK(range.maximum == fontSize->maximum);

    // Below the floor rises to the minimum; above the ceiling falls to the
    // maximum.
    CHECK(Zoom::clampFontZoom(4) == static_cast<int>(fontSize->minimum)); // 6
    CHECK(Zoom::clampFontZoom(256) == static_cast<int>(fontSize->maximum)); // 96
    CHECK(Zoom::clampFontZoom(5.0) == fontSize->minimum); // macOS [6,96] floor
    CHECK(Zoom::clampFontZoom(200.0) == fontSize->maximum);
    CHECK(Zoom::clampFontZoom(16) == 16); // a typical size passes through
}

// The `link_style` key is an index and the renderers take a VT code, and the
// two number their styles differently. A frontend casting one to the other
// draws a solid rule where the setting asked for dots — and because index 0
// happens to land on a code the builder's `default:` arm draws identically,
// the default configuration cannot show the mistake.
TEST(testLinkStyleIndexResolvesToItsVTCode)
{
    namespace Effective = Ubiquitty::Preferences::Effective;

    CHECK(Effective::linkUnderlineVTStyle(0) == Ubiquitty::VT::underlineSingle);
    CHECK(Effective::linkUnderlineVTStyle(1) == Ubiquitty::VT::underlineDotted);

    // The dotted code is not the dotted index: this is the whole reason the
    // conversion exists, and a mapping that lost it would still pass a test
    // written against index 0 alone.
    CHECK(Effective::linkUnderlineVTStyle(1) != 1);
    CHECK(Ubiquitty::VT::underlineDotted == 4);

    // Preferences.cpp accepts any integer for the key, so the out-of-range values a
    // file can carry resolve rather than reaching a renderer as a code nothing
    // draws.
    CHECK(Effective::linkUnderlineVTStyle(2) == Ubiquitty::VT::underlineDotted);
    CHECK(Effective::linkUnderlineVTStyle(99) == Ubiquitty::VT::underlineDotted);
    CHECK(Effective::linkUnderlineVTStyle(-1) == Ubiquitty::VT::underlineDotted);
}

/// Which pasteboard a remote session's PROGRAMS read.
///
/// Off by default, and the default is the security decision rather than a
/// convenience one: turning it on puts shims for xclip, wl-paste and pbpaste on
/// the session's PATH, which exposes the user's own pasteboard to every process
/// on that host that runs one of them.
TEST(testRemoteClipboardToolsDefaultsOffAndReadsItsNames)
{
    namespace Effective = Ubiquitty::Preferences::Effective;
    using Ubiquitty::Preferences::RemoteClipboardTools;

    CHECK(Effective::remoteClipboardTools(Values {}) == RemoteClipboardTools::Off);

    CHECK(Effective::remoteClipboardTools(parseString("[clipboard]\nremote_tools = \"client\"\n"))
        == RemoteClipboardTools::Client);
    CHECK(Effective::remoteClipboardTools(parseString("[clipboard]\nremote_tools = \"off\"\n"))
        == RemoteClipboardTools::Off);

    // A bool reads as the obvious thing, so a file that says true does not have
    // to know the vocabulary.
    CHECK(Effective::remoteClipboardTools(parseString("[clipboard]\nremote_tools = true\n"))
        == RemoteClipboardTools::Client);
    CHECK(Effective::remoteClipboardTools(parseString("[clipboard]\nremote_tools = false\n"))
        == RemoteClipboardTools::Off);

    // "host" names where the programs look rather than whether shims exist, and
    // means the same as off: the host's own pasteboard is what they get.
    CHECK(Effective::remoteClipboardTools(parseString("[clipboard]\nremote_tools = \"host\"\n"))
        == RemoteClipboardTools::Off);

    // The fold five call sites read rather than each comparing for themselves.
    // It answers in Clipboard::Serving because that is the type the clients
    // take, and a bool at each site is one more place to invert a security
    // decision.
    CHECK(Effective::clipboardServing(Values {}) == Ubiquitty::Clipboard::Serving::Disabled);
    CHECK(Effective::clipboardServing(parseString("[clipboard]\nremote_tools = \"client\"\n"))
        == Ubiquitty::Clipboard::Serving::Enabled);
    CHECK(Effective::clipboardServing(parseString("[clipboard]\nremote_tools = \"host\"\n"))
        == Ubiquitty::Clipboard::Serving::Disabled);

    // A value nobody defined leaves the key unset rather than guessing, so a
    // typo cannot silently expose a pasteboard. Asserted as ABSENT rather than
    // as reading Off: they are different claims — a stored Off would mean the
    // typo had been understood as a decision.
    const Values typo = parseString("[clipboard]\nremote_tools = \"clietn\"\n");
    CHECK(!typo.remoteClipboardTools.has_value());
    CHECK(Effective::remoteClipboardTools(typo) == RemoteClipboardTools::Off);
}

UBIQUITTY_TEST_MAIN()
