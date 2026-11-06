// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Preferences/RenderConfigurationFold.h"

#include "Core/Preferences/Key.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/Profile.h"

#include <algorithm>
#include <string>

namespace Ubiquitty::Preferences::Effective {

namespace {

Rendering::FallbackRule ruleFrom(const LanguageFallbackEntry& entry)
{
    Rendering::FallbackRule rule;
    rule.script.assign(entry.script);
    rule.language.assign(entry.language);
    rule.mode.assign(entry.mode);
    rule.family.assign(entry.family);
    return rule;
}

Rendering::FallbackRule ruleFrom(const RangeMapEntry& entry)
{
    Rendering::FallbackRule rule;
    rule.mode.assign(entry.mode);
    rule.family.assign(entry.family);
    rule.firstCodePoint = entry.firstCodePoint;
    rule.lastCodePoint = entry.lastCodePoint;
    rule.sizing = !entry.shouldMatchSize ? Rendering::FontMetrics::FallbackSizing::Inherit
        : *entry.shouldMatchSize         ? Rendering::FontMetrics::FallbackSizing::MatchPrimary
                                         : Rendering::FontMetrics::FallbackSizing::Nominal;
    return rule;
}

} // namespace

Rendering::RenderConfiguration renderConfiguration(const Values& values)
{
    Rendering::RenderConfiguration configuration;

    configuration.fontFamily.assign(values.fontFamily.value_or(std::string()));
    // Through the clamp the zoom uses: a file asking for a size no renderer can
    // draw is brought back rather than reaching one.
    configuration.fontSizePoints = Zoom::clampFontZoom(fontSize(values));
    configuration.proportionalFontFamily.assign(values.proportionalFontFamily.value_or(std::string()));
    configuration.proportionalFontSizePoints = proportionalFontSize(values);
    configuration.symbolFallbackFamily.assign(symbolFallbackFamily(values));
    configuration.cjkFallbackFamily.assign(values.cjkFallbackFont.value_or(std::string()));
    configuration.emojiFallbackFamily.assign(values.emojiFallbackFont.value_or(std::string()));
    const std::size_t languageCount = std::min(values.languageFallbacks.size(), Rendering::fallbackRuleCapacity);
    for (std::size_t index = 0; index < languageCount; ++index)
        configuration.languageFallbacks[index] = ruleFrom(values.languageFallbacks[index]);
    configuration.languageFallbackCount = static_cast<std::uint8_t>(languageCount);
    const std::size_t rangeCount = std::min(values.rangeMap.size(), Rendering::fallbackRuleCapacity);
    for (std::size_t index = 0; index < rangeCount; ++index)
        configuration.rangeMaps[index] = ruleFrom(values.rangeMap[index]);
    configuration.rangeMapCount = static_cast<std::uint8_t>(rangeCount);
    configuration.fontFeatures.assign(values.fontFeatures.value_or(std::string()));
    configuration.fontVariations.assign(values.fontVariations.value_or(std::string()));
    configuration.hasLigatures = ligatures(values);
    configuration.fontWeight = fontWeight(values);
    configuration.boldFontWeight = boldFontWeight(values);
    configuration.fontWidthPercent = fontWidthPercent(values);
    configuration.lineSpacingPoints = lineSpacing(values);
    configuration.characterSpacingPoints = characterSpacing(values);
    configuration.baselineOffsetPoints = values.baselineOffset.value_or(0.0);
    configuration.shouldMatchFallbackSize = matchFallbackSize(values);
    configuration.shouldBoostWideGlyphs = wideGlyphBoost(values);
    configuration.wideGlyphScale = wideGlyphScale(values);
    configuration.shouldDrawBrailleGrid = brailleGrid(values);
    configuration.brailleStyle = brailleStyle(values);
    configuration.shadeStyle = shadeStyle(values);

    configuration.shouldBrightenBold = brightenBold(values);
    configuration.selectionStyle = selectionStyle(values);
    configuration.nonOwnerPresentation = nonOwnerPresentation(values);
    configuration.underlineThicknessScale = underlineThickness(values);
    configuration.underlineOffsetPoints = underlineOffset(values);
    configuration.shouldUnderlineSkipInk = underlineSkipInk(values);
    configuration.shouldUnderlineFollowInk = underlineFollowsInk(values);
    configuration.underlineSkipInkPaddingPoints = underlineSkipInkPad(values);
    configuration.shouldUnderlineLinks = underlineLinks(values);
    configuration.linkUnderlineStyle = linkUnderlineStyle(values);
    configuration.shouldStyleLinksPerKind = linkStylePerKind(values);
    configuration.shouldUseSystemBlueLinkColor = linkColorSystemBlue(values);
    const Values::PaddingEdges padding = values.resolvedPadding();
    configuration.paddingLeftPoints = padding.left;
    configuration.paddingRightPoints = padding.right;
    configuration.paddingTopPoints = padding.top;
    configuration.paddingBottomPoints = padding.bottom;
    configuration.titlelessPaddingTopPoints = titlelessPaddingTop(values);
    configuration.usesDisplayP3 = colorSpaceP3(values);
    configuration.colorblindMode = colorblindMode(values);

    configuration.backgroundOpacity = backgroundOpacity(values);
    configuration.defaultBackgroundOpacity = defaultBackgroundOpacity(values);
    configuration.blurRadiusPoints = blurRadius(values);
    configuration.shouldEnhanceTextContrast = textContrast(values);
    configuration.shouldEnhanceTextOnBlur = textVibrancy(values);
    configuration.usesBackgroundVibrancy = backgroundVibrancy(values);
    configuration.backgroundImagePath.assign(backgroundImage(values));
    configuration.backgroundImageOpacity = backgroundImageOpacity(values);
    configuration.backgroundImageMode = backgroundImageMode(values);

    configuration.cursorAlpha = static_cast<double>(std::clamp(cursorAlpha(values), 0, 255)) / 255.0;
    configuration.shouldCursorInvertText = cursorInvertText(values);
    configuration.shouldFadeBlink = blinkAnimation(values);
    configuration.isBellVisual = bellVisual(values);
    configuration.bellFlashPeakAlpha = bellVisualIntensity(values);

    configuration.usesCustomTekColors = tekColorMode(values) == 1;
    configuration.tekBeamRGB = tekBeamColor(values);
    configuration.tekBackgroundRGB = tekBackgroundColor(values);
    configuration.drawsTekWritingSpot = tekBrightPen(values);
    configuration.drawsTekGlowVectors = tekGlowVectors(values);

    configuration.maxFramesPerSecond = maxFPS(values);
    configuration.isPresentSynchronized = vsync(values);
    return configuration;
}

Rendering::RenderConfiguration renderConfiguration(const Values& values, const Profile& profile)
{
    return renderConfiguration(overlaidWithProfile(values, profile));
}

Rendering::RenderConfiguration liveRenderConfiguration() { return renderConfiguration(Preferences::values()); }

KeyReach reachOf(Key key)
{
    switch (key) {
    case Key::AppearanceBackgroundOpacity:
    case Key::AppearanceTextContrastOnBlur:
    case Key::AppearanceTextContrast:
    case Key::AppearanceBackgroundVibrancy:
    case Key::AppearanceBlurRadius:
    case Key::AppearanceBackgroundImage:
    case Key::AppearanceBackgroundImageOpacity:
    case Key::AppearanceBackgroundImageMode:
    case Key::AppearancePaddingLeft:
    case Key::AppearancePaddingRight:
    case Key::AppearancePaddingTop:
    case Key::AppearancePaddingBottom:
    case Key::AppearanceTitlelessPaddingTop:
    case Key::AppearanceColorSpace:
    case Key::AppearanceBlinkAnimation:
    case Key::AppearanceColorblindMode:
    case Key::FontFamily:
    case Key::FontProportionalFamily:
    case Key::FontFallback:
    case Key::FontSymbolFallback:
    case Key::FontEmojiFallback:
    case Key::FontCJKFallback:
    case Key::FontFeatures:
    case Key::FontVariations:
    case Key::FontSize:
    case Key::FontProportionalSize:
    case Key::FontLigatures:
    case Key::FontLineSpacing:
    case Key::FontCharacterSpacing:
    case Key::FontWeight:
    case Key::FontBoldWeight:
    case Key::FontWidth:
    case Key::FontBaselineOffset:
    case Key::FontMatchFallbackSize:
    case Key::FontWideGlyphBoost:
    case Key::FontBrailleGrid:
    case Key::FontBrailleStyle:
    case Key::AppearanceShadeBlocks:
    case Key::FontWideGlyphScale:
    case Key::UnderlineLinks:
    case Key::UnderlineLinkStyle:
    case Key::UnderlineLinkColorBlue:
    case Key::UnderlineLinkStylePerKind:
    case Key::UnderlineThickness:
    case Key::UnderlineOffset:
    case Key::UnderlineSkipInk:
    case Key::UnderlineFollowsInk:
    case Key::UnderlineSkipInkPad:
    case Key::TextBrightenBold:
    case Key::TekBrightPen:
    case Key::TekGlowVectors:
    case Key::TekColorMode:
    case Key::TekBeamColor:
    case Key::TekBackgroundColor:
    case Key::SelectionStyle:
    case Key::RemoteNonOwnerPresentation:
    case Key::CursorAlpha:
    case Key::CursorInvertText:
    case Key::BellVisual:
        return KeyReach::Renderer;

    case Key::AppearanceTheme:
    case Key::TerminalScrollbackLines:
    case Key::TerminalEncoding:
    case Key::TerminalAmbiguousWidth:
    case Key::CursorShape:
    case Key::CursorBlink:
    case Key::CursorLinger:
    case Key::CursorBlinkRate:
    case Key::CursorMotion:
    case Key::BellAudible:
    case Key::SecurityPasswordDetect:
        return KeyReach::Terminal;

    case Key::AppearanceFollowSystem:
    case Key::AppearanceSystemLightTheme:
    case Key::AppearanceSystemDarkTheme:
    case Key::AppearanceSquareBottomCorners:
    case Key::AppearanceFolderIcon:
    case Key::AppearanceFolderIconAlways:
    case Key::AppearanceProcessIcons:
    case Key::AppearanceTitlebarStyle:
    case Key::AppearancePaneTitleBars:
    case Key::AppearanceSnapWindowResize:
    case Key::AppearanceHideTitleBar:
    case Key::AppearanceUIFontScale:
    case Key::AppearanceTagColorOpacity:
    case Key::AppearanceBeamTerminalFont:
    case Key::SelectionSmartDoubleClick:
    case Key::SelectionCopyOnSelect:
    case Key::SelectionLinksOverrideReporting:
    case Key::SelectionOSC8ClickOpens:
    case Key::SelectionRestrictToText:
    case Key::SelectionCopyStyle:
    case Key::SelectionCopyStyleTheme:
    case Key::SelectionBlockSelectModifier:
    case Key::SelectionSelectModifier:
    case Key::TerminalCloseIgnoredProcesses:
    case Key::TerminalCloseConfirmation:
    case Key::PasteConfirmMultiline:
    case Key::PasteConfirmMultilineThreshold:
    case Key::KeysAccessorySymbols:
    case Key::ClipboardRemoteTools:
    case Key::MouseWheelSpeed:
    case Key::MouseWheelLines:
    case Key::MouseTrackpadSpeed:
    case Key::MouseScrollBounce:
    case Key::TerminalColumnModeResizesWindow:
    case Key::TitleWindowDirectory:
    case Key::TitleWindowDirectoryPath:
    case Key::TitleWindowAppTitle:
    case Key::TitleWindowProcess:
    case Key::TitleWindowProcessArguments:
    case Key::TitleWindowShellCommand:
    case Key::TitleWindowTTYName:
    case Key::TitleWindowProfileName:
    case Key::TitleWindowText:
    case Key::TitleWindowDimensions:
    case Key::TitleWindowProgramStatus:
    case Key::TitleTabDirectory:
    case Key::TitleTabDirectoryPath:
    case Key::TitleTabAppTitle:
    case Key::TitleTabProcess:
    case Key::TitleTabProcessArguments:
    case Key::TitleTabShellCommand:
    case Key::TitleTabTTYName:
    case Key::TitleTabProfileName:
    case Key::TitleTabText:
    case Key::TitleTabDimensions:
    case Key::TitleTabProgramStatus:
    case Key::TitlePaneDirectory:
    case Key::TitlePaneDirectoryPath:
    case Key::TitlePaneAppTitle:
    case Key::TitlePaneProcess:
    case Key::TitlePaneProcessArguments:
    case Key::TitlePaneShellCommand:
    case Key::TitlePaneTTYName:
    case Key::TitlePaneProfileName:
    case Key::TitlePaneText:
    case Key::TitlePaneDimensions:
    case Key::TitlePaneProgramStatus:
    case Key::TitleDirectoryHiddenFor:
    case Key::TitleDirectoryHiddenInFullScreen:
    case Key::TabsShowShortcuts:
    case Key::WindowDragModifier:
    case Key::WindowInitialColumns:
    case Key::WindowInitialRows:
    case Key::QuakeEnabled:
    case Key::QuakeHotkey:
    case Key::QuakeHeight:
    case Key::QuakeAnimate:
    case Key::SoundAudible:
    case Key::SoundVolume:
    case Key::NotificationsEnabled:
    case Key::NotificationsSound:
    case Key::RemoteAutoInstallHost:
    case Key::RemoteHistoryCacheOnDisk:
    case Key::DockBounce:
    case Key::DockBadge:
    case Key::SecuritySecureKeyboardEntry:
    case Key::SecuritySecureKeyboardEntryAtPasswordPrompts:
    case Key::ControlEnable:
    case Key::ControlSocketPath:
    case Key::ControlToken:
    case Key::ControlApprove:
    case Key::ExperimentalSessionPersistence:
    case Key::DebugShowBuildHash:
    case Key::UpdatesCheckAutomatically:
    case Key::TermProgram:
    case Key::TermVersion:
    case Key::TransferZModem:
        return KeyReach::Frontend;
    }
    return KeyReach::Frontend;
}

} // namespace Ubiquitty::Preferences::Effective
