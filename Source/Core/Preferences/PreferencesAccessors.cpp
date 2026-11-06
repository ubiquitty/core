// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Effective-value accessors: "field.Set ? c.field : DEFAULT" per settings key,
// default from PreferencesDefaults.h. See PreferencesAccessors.h.

#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/TitleComposition.h"

#include "Core/GridBounds.h"
#include "Core/Preferences/PreferencesDefaults.h"
#include "Core/Themes/Themes.h"
#include "Core/VT/Constants.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty::Preferences {

namespace {
// Parse a "#rrggbb" / "rrggbb" hex color into packed 0xRRGGBB. Leading '#' and
// surrounding whitespace are tolerated. Returns the fallback on any malformed
// input so an effective-color accessor always yields a valid color.
unsigned parseHexRGB(std::string_view s, unsigned fallback)
{
    std::size_t begin = 0, end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t'))
        ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t'))
        --end;
    if (begin < end && s[begin] == '#')
        ++begin;
    if (end - begin != 6)
        return fallback;
    unsigned value = 0;
    for (std::size_t i = begin; i < end; ++i) {
        const char ch = s[i];
        unsigned digit;
        if (ch >= '0' && ch <= '9')
            digit = static_cast<unsigned>(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            digit = static_cast<unsigned>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            digit = static_cast<unsigned>(ch - 'A' + 10);
        else
            return fallback;
        value = (value << 4) | digit;
    }
    return value;
}

// An ASCII case fold, which is the whole of what a scheme comparison needs: a
// scheme is ASCII by grammar, so no locale can widen or narrow this.
std::string asciiLowered(std::string_view text)
{
    std::string lowered(text);
    for (char& character : lowered) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return lowered;
}
} // namespace

namespace Effective {

// [font]
double fontSize(const Values& c) { return c.fontSize.value_or(Defaults::fontSize); }
double proportionalFontSize(const Values& c) { return c.proportionalFontSize.value_or(0.0); }
bool ligatures(const Values& c) { return c.ligatures.value_or(Defaults::ligatures); }
double lineSpacing(const Values& c) { return c.lineSpacingPoints.value_or(Defaults::lineSpacing); }
bool wideGlyphBoost(const Values& c) { return c.wideGlyphBoost.value_or(Defaults::wideGlyphBoost); }
bool matchFallbackSize(const Values& c) { return c.shouldMatchFallbackSize.value_or(Defaults::matchFallbackSize); }
bool brailleGrid(const Values& c) { return c.brailleGrid.value_or(Defaults::brailleGrid); }
Rendering::BrailleStyle brailleStyle(const Values& configuration)
{
    return configuration.brailleStyle ? Rendering::brailleStyleFromString(*configuration.brailleStyle) : Rendering::defaultBrailleStyle;
}
Rendering::ShadeStyle shadeStyle(const Values& values)
{
    return values.shadeBlocks ? Rendering::shadeStyleFromString(*values.shadeBlocks) : Rendering::defaultShadeStyle;
}
bool vsync(const Values& c) { return c.vsync.value_or(Defaults::vsync); }
bool folderIcon(const Values& c) { return c.folderIcon.value_or(Defaults::folderIcon); }
bool folderIconAlways(const Values& c) { return c.folderIconAlways.value_or(Defaults::folderIconAlways); }
bool processIcons(const Values& c) { return c.processIcons.value_or(Defaults::processIcons); }
std::string directoryHiddenFor(const Values& c) { return c.directoryHiddenFor.value_or(Defaults::directoryHiddenFor); }
bool directoryHiddenInFullScreen(const Values& c) { return c.directoryHiddenInFullScreen.value_or(Defaults::directoryHiddenInFullScreen); }
double wideGlyphScale(const Values& c) { return c.wideGlyphScale.value_or(Defaults::wideGlyphScale); }
double characterSpacing(const Values& c) { return c.characterSpacingPoints.value_or(Defaults::characterSpacing); }
int fontWeight(const Values& c) { return c.fontWeight.value_or(Defaults::fontWeight); }
int boldFontWeight(const Values& c) { return c.boldFontWeight.value_or(Defaults::boldFontWeight); }
double fontWidthPercent(const Values& c) { return c.fontWidthPercent.value_or(Defaults::fontWidthPercent); }

// [underline]
double underlineThickness(const Values& c) { return c.underlineThickness.value_or(Defaults::underlineThickness); }
double underlineOffset(const Values& c) { return c.underlineOffset.value_or(Defaults::underlineOffset); }
bool underlineSkipInk(const Values& c) { return c.underlineSkipInk.value_or(Defaults::underlineSkipInk); }
bool underlineFollowsInk(const Values& c) { return c.underlineFollowsInk.value_or(Defaults::underlineFollowsInk); }
double underlineSkipInkPad(const Values& c) { return c.underlineSkipInkPad.value_or(Defaults::underlineSkipInkPad); }
bool underlineLinks(const Values& c) { return c.underlineLinks.value_or(Defaults::underlineLinks); }

Screen::ScrollTuning::Policy wheelPolicy(const Values& c, double pointsPerLine)
{
    Screen::ScrollTuning::Policy p;
    p.baseLinesPerTick = c.wheelLines.has_value() ? static_cast<double>(*c.wheelLines) : 1.0;
    p.speed = c.wheelSpeed.value_or(1.0);
    p.trackpadSpeed = c.trackpadSpeed.value_or(defaultTrackpadSpeed);
    p.pointsPerLine = pointsPerLine > 0.0 ? pointsPerLine : 1.0;
    return p;
}
int linkUnderlineStyle(const Values& c) { return c.linkUnderlineStyle.value_or(Defaults::linkUnderlineStyle); }

std::uint8_t linkUnderlineVTStyle(int index)
{
    return index == 0 ? Ubiquitty::VT::underlineSingle : Ubiquitty::VT::underlineDotted;
}
bool linkColorSystemBlue(const Values& c) { return c.linkColorSystemBlue.value_or(Defaults::linkColorSystemBlue); }
bool linkStylePerKind(const Values& c) { return c.linkStylePerKind.value_or(Defaults::linkStylePerKind); }
bool smartDoubleClick(const Values& c) { return c.smartDoubleClick.value_or(Defaults::smartDoubleClick); }

// [text]
bool brightenBold(const Values& c) { return c.brightenBold.value_or(Defaults::brightenBold); }

// [term]
bool setsLocaleEnvironment(const Values& c) { return c.setsLocaleEnvironment.value_or(Defaults::setsLocaleEnvironment); }

bool beamTerminalFont(const Values& c) { return c.beamTerminalFont.value_or(Defaults::beamTerminalFont); }
bool tekBrightPen(const Values& c) { return c.tekBrightPen.value_or(Defaults::tekBrightPen); }
bool tekGlowVectors(const Values& c) { return c.tekGlowVectors.value_or(Defaults::tekGlowVectors); }
int tekColorMode(const Values& c)
{
    const int mode = c.tekColorMode.value_or(Defaults::tekColorMode);
    return std::clamp(mode, 0, 1);
}
unsigned tekBeamColor(const Values& c) { return parseHexRGB(c.tekBeamColor.value_or(Defaults::tekBeamColorHex), Defaults::tekBeamColor); }
unsigned tekBackgroundColor(const Values& c) { return parseHexRGB(c.tekBackgroundColor.value_or(Defaults::tekBackgroundColorHex), Defaults::tekBackgroundColor); }

// [appearance]
namespace {
Themes::ThemeAppearance selectedThemeAppearance(const Values& values)
{
    if (values.themeName) {
        if (const auto* theme = Themes::get(Themes::indexForName(values.themeName->c_str()))) {
            return theme->appearance;
        }
    }
    return {};
}
}

double backgroundOpacity(const Values& c)
{
    if (c.backgroundOpacity.has_value())
        return *c.backgroundOpacity;
    if (const auto opacity = selectedThemeAppearance(c).backgroundOpacity) {
        return *opacity;
    }
    return Defaults::backgroundOpacity;
}

// The parse table holds the key to 0–1; the accessor answers what it kept.
double defaultBackgroundOpacity(const Values& c) { return c.defaultBackgroundOpacity.value_or(selectedThemeAppearance(c).defaultBackgroundOpacity.value_or(Defaults::defaultBackgroundOpacity)); }
double blurRadius(const Values& c)
{
    if (c.blurRadius.has_value())
        return *c.blurRadius;
    if (const auto radius = selectedThemeAppearance(c).blurRadius) {
        return *radius;
    }
    return Defaults::blurRadius;
}
bool textVibrancy(const Values& c) { return c.shouldEnhanceTextOnBlur.value_or(selectedThemeAppearance(c).textVibrancy.value_or(false)); }
bool textContrast(const Values& c) { return c.textContrast.value_or(selectedThemeAppearance(c).textContrast.value_or(Defaults::shouldEnhanceTextContrast)); }
bool backgroundVibrancy(const Values& c) { return c.backgroundVibrancy.value_or(selectedThemeAppearance(c).backgroundVibrancy.value_or(Defaults::shouldUseBackgroundVibrancy)); }
std::string backgroundImage(const Values& c) { return c.backgroundImage.value_or(std::string {}); }
double backgroundImageOpacity(const Values& c) { return c.backgroundImageOpacity.value_or(Defaults::backgroundImageOpacity); }
BackgroundImageMode backgroundImageMode(const Values& c) { return c.backgroundImageMode.value_or(Defaults::backgroundImageMode); }
RemoteClipboardTools remoteClipboardTools(const Values& c) { return c.remoteClipboardTools.value_or(Defaults::remoteClipboardTools); }
Clipboard::Serving clipboardServing(const Values& c)
{
    return remoteClipboardTools(c) == RemoteClipboardTools::Client ? Clipboard::Serving::Enabled
                                                                   : Clipboard::Serving::Disabled;
}

namespace {

const CapabilityOverrides& capabilityOverridesFor(const Values& c, CapabilityOrigin origin)
{
    switch (origin) {
    case CapabilityOrigin::Local:
        return c.localCapabilities;
    case CapabilityOrigin::Agent:
        return c.agentCapabilities;
    case CapabilityOrigin::Remote:
        return c.remoteCapabilities;
    }
    return c.remoteCapabilities; // a value cast in from outside the enumeration reads the strictest origin's table
}

// What the file says for one tier, or nothing where it is silent.
std::optional<CapabilityDecision> configuredCapabilityDecision(const Values& c, CapabilityOrigin origin, CapabilityTier tier)
{
    const CapabilityOverrides& overrides = capabilityOverridesFor(c, origin);
    switch (tier) {
    case CapabilityTier::LowRiskInfo:
        return overrides.informational;
    case CapabilityTier::HostEscaping:
        return overrides.hostEscaping;
    case CapabilityTier::ReadExfil:
        return overrides.readExfiltration;
    case CapabilityTier::Benign:
    case CapabilityTier::Rendering:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

CapabilityDecision capabilityDecision(const Values& c, CapabilityOrigin origin, CapabilityTier tier)
{
    return capabilityDecisionFor(configuredCapabilityDecision(c, origin, tier), origin, tier);
}

CapabilityDecision capabilityDecision(const Values& c, CapabilityOrigin origin, Capability capability)
{
    return capabilityDecisionFor(configuredCapabilityDecision(c, origin, tierOf(capability)), origin, capability);
}

CapabilityDecisionTable capabilityDecisions(const Values& c)
{
    // Only the cells the file names are set: an unset cell answers the default,
    // which is per capability where a tier's default is refined.
    CapabilityDecisionTable table;
    for (const CapabilityOrigin origin : { CapabilityOrigin::Local, CapabilityOrigin::Agent, CapabilityOrigin::Remote }) {
        for (const CapabilityTier tier : { CapabilityTier::HostEscaping, CapabilityTier::ReadExfil, CapabilityTier::LowRiskInfo }) {
            if (const std::optional<CapabilityDecision> configured = configuredCapabilityDecision(c, origin, tier))
                table = table.replacing(origin, tier, *configured);
        }
    }
    return table;
}
bool squareBottomCorners(const Values& c) { return c.squareBottomCorners.value_or(Defaults::squareBottomCorners); }
double titlelessPaddingTop(const Values& c)
{
    // Unset → mirror the effective padding_bottom so titleless content is
    // vertically symmetric by default. Both share the 0..64 pt window-padding
    // bound (Preferences::Schema), so clamp the explicit value the same way.
    double points = c.titlelessPaddingTop.value_or(c.resolvedPadding().bottom);
    if (points < 0.0)
        points = 0.0;
    if (points > 64.0)
        points = 64.0;
    return points;
}
TitlebarStyle titlebarStyle(const Values& c) { return c.titlebarStyle.value_or(Defaults::titlebarStyle); }
BeamSurface beamSurface(const Values& c) { return c.beamSurface.value_or(Defaults::beamSurface); }
bool paneTitleBars(const Values& c) { return c.paneTitleBars.value_or(Defaults::paneTitleBars); }
bool shouldWatchForZModem(const Values& c) { return c.shouldWatchForZModem.value_or(Defaults::shouldWatchForZModem); }
bool blinkAnimation(const Values& c) { return c.blinkAnimation.value_or(Defaults::blinkAnimation); }
bool snapWindowResize(const Values& c) { return c.snapWindowResize.value_or(Defaults::snapWindowResize); }
bool hideTitleBar(const Values& c) { return c.hideTitleBar.value_or(Defaults::hideTitleBar); }
bool hideTitleBar(const Values& c, const Profile& p) { return p.hideTitleBar.value_or(hideTitleBar(c)); }

bool columnModeResizesWindow(const Values& c) { return c.columnModeResizesWindow.value_or(Defaults::columnModeResizesWindow); }
bool programFullscreenUsesNative(const Values& c) { return c.windowProgramFullscreen.value_or(std::string()) == "native"; }
Ubiquitty::Image::PresentationColorSpace imagePresentationColorSpace(const Values& c)
{
    return colorSpaceP3(c) ? Ubiquitty::Image::PresentationColorSpace::DisplayP3
                           : Ubiquitty::Image::PresentationColorSpace::SRGB;
}
bool colorSpaceP3(const Values& c)
{
    if (!c.colorSpace.has_value())
        return Defaults::colorSpaceP3;
    // Shared parser; an unrecognized token falls back to sRGB.
    return colorSpaceFromString(*c.colorSpace).value_or(ColorSpace::SRGB) == ColorSpace::DisplayP3;
}
bool followSystemAppearance(const Values& c) { return c.followSystemAppearance.value_or(Defaults::followSystemAppearance); }
Accessibility::ColorblindMode colorblindMode(const Values& c)
{
    if (!c.colorblindMode.has_value())
        return Defaults::colorblindMode;
    // Shared parser; unrecognized token falls back to None (no remap).
    return colorblindModeFromString(*c.colorblindMode).value_or(Accessibility::ColorblindMode::None);
}
double uiFontScale(const Values& c)
{
    const double v = c.uiFontScale.value_or(Defaults::uiFontScale);
    return Schema::clampToRange(v, "appearance", "ui_font_scale");
}
double tagColorOpacity(const Values& c)
{
    const double v = c.tagColorOpacity.value_or(Defaults::tagColorOpacity);
    return Schema::clampToRange(v, "appearance", "tag_color_opacity");
}

// [cursor]
CursorShape cursorShape(const Values& c) { return c.cursorShape.value_or(Defaults::cursorShape); }
CursorMotionMode cursorMotion(const Values& c) { return c.cursorMotion.value_or(Defaults::cursorMotion); }
bool cursorBlink(const Values& c) { return c.cursorBlink.value_or(Defaults::cursorBlink); }
int cursorLinger(const Values& c) { return c.cursorLinger.value_or(Defaults::cursorLinger); }
int cursorBlinkRate(const Values& c) { return c.cursorBlinkRate.value_or(Defaults::cursorBlinkRate); }
int cursorAlpha(const Values& c) { return c.cursorAlpha.value_or(Defaults::cursorAlpha); }
int cursorMotionDuration(const Values& c) { return c.cursorMotionDuration.value_or(Defaults::cursorMotionDuration); }
int maxFPS(const Values& c) { return c.maxFPS.value_or(Defaults::maxFPS); }
bool cursorInvertText(const Values& c) { return c.cursorInvertText.value_or(Defaults::cursorInvertText); }

// [bell]
bool bellAudible(const Values& c) { return c.bellAudible.value_or(Defaults::bellAudible); }

// [sound]
bool soundAudible(const Values& c) { return c.soundAudible.value_or(Defaults::soundAudible); }
double soundVolume(const Values& c) { return c.soundVolume.value_or(Defaults::soundVolume); }

Ubiquitty::Unicode::Encoding inputEncoding(const Values& c)
{
    Ubiquitty::Unicode::Encoding encoding = Ubiquitty::Unicode::Encoding::UTF8;
    if (c.encoding.value_or("").empty())
        return encoding;
    auto resolved = Ubiquitty::Unicode::encodingFromName(*c.encoding);
    if (!resolved)
        return Ubiquitty::Unicode::Encoding::UTF8;
    return *resolved;
}

Ubiquitty::BellMode bellMode(const Values& c)
{
    const unsigned cue = (bellAudible(c) ? 1u : 0u) | (bellVisual(c) ? 2u : 0u);
    return static_cast<Ubiquitty::BellMode>(cue);
}
bool bellVisual(const Values& c) { return c.bellVisual.value_or(Defaults::bellVisual); }
double bellVisualIntensity(const Values& c) { return c.bellVisualIntensity.value_or(Defaults::bellVisualIntensity); }

// [selection]
bool copyOnSelect(const Values& c) { return c.copyOnSelect.value_or(Defaults::copyOnSelect); }
std::string blockSelectModifier(const Values& c) { return c.blockSelectModifier.value_or(Defaults::blockSelectModifier); }
std::string selectModifier(const Values& c) { return c.selectModifier.value_or(Defaults::selectModifier); }
Rendering::SelectionStyle selectionStyle(const Values& values)
{
    return values.selectionStyle ? Rendering::selectionStyleFromString(*values.selectionStyle) : Rendering::defaultSelectionStyle;
}
std::string closeIgnoredProcesses(const Values& c) { return c.closeIgnoredProcesses.value_or(Defaults::closeIgnoredProcesses); }
std::string closeConfirmation(const Values& c) { return c.closeConfirmation.value_or(Defaults::closeConfirmation); }
bool linksOverrideReporting(const Values& c) { return c.linksOverrideReporting.value_or(Defaults::linksOverrideReporting); }
bool osc8ClickOpens(const Values& c) { return c.osc8ClickOpens.value_or(Defaults::osc8ClickOpens); }
bool restrictSelectionToText(const Values& c) { return c.restrictSelectionToText.value_or(Defaults::restrictSelectionToText); }

// [paste]
bool confirmMultilinePaste(const Values& c) { return c.confirmMultilinePaste.value_or(Defaults::confirmMultilinePaste); }
int confirmMultilinePasteThreshold(const Values& c) { return c.confirmMultilinePasteThreshold.value_or(Defaults::confirmMultilinePasteThreshold); }

// [mouse]
double wheelSpeed(const Values& c) { return c.wheelSpeed.value_or(Defaults::wheelSpeed); }
int wheelLines(const Values& c) { return c.wheelLines.value_or(Defaults::wheelLines); }
double trackpadSpeed(const Values& c) { return c.trackpadSpeed.value_or(Defaults::trackpadSpeed); }
bool scrollBounce(const Values& c) { return c.scrollBounce.value_or(Defaults::scrollBounce); }

// [tabs]
TabShortcutHints tabShortcutHints(const Values& c) { return c.tabShortcutHints.value_or(Defaults::tabShortcutHints); }

std::string linkHandlerCommand(const Values& c, std::string_view scheme)
{
    if (scheme.empty())
        return {};
    // Both sides are folded, not just the configured key: a caller that
    // asked with the scheme as the link spelled it would otherwise be told
    // there is no handler, which is the answer that opens the wrong thing.
    const std::string wanted = asciiLowered(scheme);
    for (const LinkHandlerEntry& entry : c.linkHandlers) {
        if (asciiLowered(entry.scheme) == wanted)
            return entry.command;
    }
    return {};
}

std::string linkLineHandler(const Values& c) { return c.linkLineHandler.value_or(Defaults::linkLineHandler); }

// [window]
std::string windowDragModifier(const Values& c) { return c.windowDragModifier.value_or(Defaults::windowDragModifier); }

// [keys]
OptionAsAlternate optionAsAlternate(const Values& c) { return c.optionAsAlternate.value_or(Defaults::optionAsAlternate); }

int launchColumns(const Values& c) { return c.windowInitialColumns.value_or(0) > 0 ? *c.windowInitialColumns : defaultColumns; }
int launchRows(const Values& c) { return c.windowInitialRows.value_or(0) > 0 ? *c.windowInitialRows : defaultRows; }

LaunchSettings launchSettings(const Values& c)
{
    LaunchSettings launch;
    launch.columns = launchColumns(c);
    launch.rows = launchRows(c);
    launch.isGridPinned = c.windowInitialColumns.value_or(0) > 0 && c.windowInitialRows.value_or(0) > 0;
    launch.shouldHideTitleBar = hideTitleBar(c);
    launch.titlebarStyle = titlebarStyle(c);
    launch.shouldSnapWindowResize = snapWindowResize(c);
    return launch;
}

LaunchSettings launchSettings(const Values& c, const Profile& profile)
{
    LaunchSettings settings = launchSettings(overlaidWithProfile(c, profile));
    settings.startupHost = profile.startupHost.value_or(std::string {});
    return settings;
}

bool autoDeployHostOnRemoteConnect()
{
    return autoDeployHostOnRemoteConnect(values());
}

LaunchSettings launchSettings() { return launchSettings(values()); }

BackgroundEffects backgroundEffects(const Values& c, SystemTransparency system, const Themes::ThemeAppearance& appearance)
{
    // A key the configuration sets wins over the appearance of the theme the
    // window shows, and that over the key's default. The theme the
    // configuration pins is not asked: a reader who follows the system keeps
    // one beside the pair, and its appearance composited a window showing
    // another theme as though it showed that one.
    const double opacity = c.backgroundOpacity.value_or(appearance.backgroundOpacity.value_or(Defaults::backgroundOpacity));
    // The per-default-cell alpha is the translucent window's: on an opaque one
    // it would darken the theme rather than reveal anything.
    const double defaultCellOpacity = opacity < 1.0 ? Defaults::defaultBackgroundOpacity : 1.0;
    return backgroundEffects(BackgroundRequest {
                                 .backgroundOpacity = opacity,
                                 .blurRadius = c.blurRadius.value_or(appearance.blurRadius.value_or(Defaults::blurRadius)),
                                 .defaultBackgroundOpacity = c.defaultBackgroundOpacity.value_or(appearance.defaultBackgroundOpacity.value_or(defaultCellOpacity)),
                                 .shouldEnhanceTextOnBlur = c.shouldEnhanceTextOnBlur.value_or(appearance.textVibrancy.value_or(false)),
                                 .usesBackgroundVibrancy = c.backgroundVibrancy.value_or(appearance.backgroundVibrancy.value_or(Defaults::shouldUseBackgroundVibrancy)),
                             },
        system);
}

bool textContrast(const Values& c, const Themes::ThemeAppearance& appearance)
{
    return c.textContrast.value_or(appearance.textContrast.value_or(Defaults::shouldEnhanceTextContrast));
}

BackgroundEffects backgroundEffects(const BackgroundRequest& asked, SystemTransparency system)
{
    if (system == SystemTransparency::Reduced) {
        return {};
    }
    BackgroundEffects effects;
    // A window is translucent exactly when its opacity is below one. At 1.0
    // the tint is solid and nothing behind the window reaches the reader
    // while the grid is drawn, so a blur there is pure cost — and it shows
    // where the grid is absent: a window is ordered in before its first frame
    // with its ground left clear, which is a tab opening onto a blurred
    // desktop rather than a terminal. So blur is a property of a translucent
    // window, asked for by the radius and in effect only where the opacity
    // lets it through. The toggles remain enabled while an opaque window
    // renders without either effect.
    effects.isTranslucent = asked.backgroundOpacity < 1;
    effects.isBlurred = effects.isTranslucent && asked.blurRadius > 0;
    effects.blurRadius = effects.isBlurred ? asked.blurRadius : 0;
    effects.shouldEnhanceTextOnBlur = effects.isBlurred && asked.shouldEnhanceTextOnBlur;
    effects.usesBackgroundVibrancy = effects.isBlurred && asked.usesBackgroundVibrancy;
    // The renderer's own alpha follows the window's: against an opaque
    // backing the compositor drops the alpha and keeps the premultiplied,
    // darkened colour, so a sub-unity tint on an opaque window darkens the
    // theme towards black instead of revealing the desktop. The per-default-
    // cell extra is the same rule again.
    effects.rendererOpacity = effects.isTranslucent ? asked.backgroundOpacity : 1;
    effects.defaultBackgroundOpacity = effects.isTranslucent ? asked.defaultBackgroundOpacity : 1;
    return effects;
}

// [quake]
bool quakeEnabled(const Values& c) { return c.quakeEnabled.value_or(Defaults::quakeEnabled); }
std::string quakeHotkey(const Values& c) { return c.quakeHotkey.value_or(Defaults::quakeHotkey); }
double quakeHeight(const Values& c)
{
    // The parse table already clamps a set value to [0.2, 1.0]; clamp the
    // default path too so a future default edit can't slip out of range.
    return std::clamp(c.quakeHeight.value_or(Defaults::quakeHeight), 0.2, 1.0);
}
bool quakeAnimate(const Values& c) { return c.quakeAnimate.value_or(Defaults::quakeAnimate); }

// [dock]
bool dockBounce(const Values& c) { return c.dockBounce.value_or(Defaults::dockBounce); }
bool dockBadge(const Values& c) { return c.dockBadge.value_or(Defaults::dockBadge); }

// [security]
bool secureKeyboardEntry(const Values& c) { return c.secureKeyboardEntry.value_or(Defaults::secureKeyboardEntry); }
bool passwordDetect(const Values& c) { return c.passwordDetect.value_or(Defaults::passwordDetect); }
bool secureKeyboardEntryAtPasswordPrompts(const Values& c)
{
    return c.secureKeyboardEntryAtPasswordPrompts.value_or(Defaults::secureKeyboardEntryAtPasswordPrompts);
}

// [control]
// SECURITY GATE: true only when control.enable = true; the listener must not bind until then.
bool controlEnabled(const Values& c) { return c.controlEnabled.value_or(Defaults::controlEnabled); }
bool controlApprove(const Values& c) { return c.controlApprove.value_or(Defaults::controlApprove); }

// [experimental]
bool sessionPersistence(const Values& c) { return c.sessionPersistence.value_or(Defaults::sessionPersistence); }

// [notifications]
bool notifications(const Values& c) { return c.notifications.value_or(Defaults::notifications); }
bool transferAutoReceive(const Values& c) { return c.transferAutoReceive.value_or(Defaults::transferAutoReceive); }
bool notificationSound(const Values& c) { return c.notificationSound.value_or(Defaults::notificationSound); }

// [triggers]
bool allowRunCommand(const Values& c) { return c.allowRunCommand.value_or(Defaults::allowRunCommand); }

bool allowWebhook(const Values& c) { return c.allowWebhook.value_or(Defaults::allowWebhook); }

bool scriptingEnable(const Values& c) { return c.scriptingEnable.value_or(Defaults::scriptingEnable); }

std::string scriptingHookProgram(const Values& c)
{
    if (!scriptingEnable(c))
        return {};
    return c.scriptingConfigScript.value_or(std::string());
}

// [debug]
bool debugShowBuildHash(const Values& c) { return c.debugShowBuildHash.value_or(Defaults::debugShowBuildHash); }
bool debugShowBuildHash() { return debugShowBuildHash(values()); }

// [updates]
bool shouldCheckForUpdatesAutomatically(const Values& c)
{
    return c.shouldCheckForUpdatesAutomatically.value_or(Defaults::shouldCheckForUpdatesAutomatically);
}

// [remote]
bool autoDeployHostOnRemoteConnect(const Values& c) { return c.autoDeployHostOnRemoteConnect.value_or(Defaults::autoDeployHostOnRemoteConnect); }
bool shouldPersistRemoteHistory(const Values& c) { return c.shouldPersistRemoteHistory.value_or(Defaults::shouldPersistRemoteHistory); }
bool shouldPersistRemoteHistory() { return shouldPersistRemoteHistory(values()); }
Rendering::RemoteViewportPolicy nonOwnerPresentation(const Values& values)
{
    return Rendering::remoteViewportPolicyFromString(values.nonOwnerPresentation.value_or(std::string {}));
}

// [terminal]
int scrollbackLines(const Values& c) { return c.scrollbackLines.value_or(Defaults::scrollbackLines); }
int ambiguousWidth(const Values& c) { return c.ambiguousWidth.value_or(Defaults::ambiguousWidth); }

// [title] — every default and inheritance rule is a column of the table in
// TitleComposition.h; these three name the surface and hand the pane its tab.
TitleComposition windowTitleComposition(const Values& c) { return Ubiquitty::Preferences::TitleComposition::compose(c, Ubiquitty::Preferences::TitleComposition::Surface::Window, nullptr); }

TitleComposition tabTitleComposition(const Values& c) { return Ubiquitty::Preferences::TitleComposition::compose(c, Ubiquitty::Preferences::TitleComposition::Surface::Tab, nullptr); }

TitleComposition paneTitleComposition(const Values& c)
{
    const TitleComposition tab = tabTitleComposition(c);
    return Ubiquitty::Preferences::TitleComposition::compose(c, Ubiquitty::Preferences::TitleComposition::Surface::Pane, &tab);
}

TitleComposition windowTitleComposition() { return windowTitleComposition(values()); }
TitleComposition tabTitleComposition() { return tabTitleComposition(values()); }
TitleComposition paneTitleComposition() { return paneTitleComposition(values()); }

} // namespace Effective
} // namespace Ubiquitty::Preferences
