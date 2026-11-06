// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Source/Core/Preferences/EffectiveValue.cpp — reading and writing one
// preference by its key.
//
// The switch below carries no `default:` arm, and that is the whole of its
// correctness argument. A key added to `Key` fails to compile here until
// somebody says what it reads as, which is a stronger guarantee than any table
// walk: a table can lose a row and answer nothing, and nothing notices until a
// settings surface shows a blank where a value belongs.
//
// Each arm defers to `Effective::` where an accessor exists rather than reading
// the field. That is not politeness — `blurRadius` falls back through the
// theme, `titlelessPaddingTop` clamps, `colorSpaceP3` parses a token — and an
// arm reading the field would answer differently from the app for exactly the
// keys whose resolution is interesting.
//

#include "Core/Preferences/EffectiveValue.h"

#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/PreferencesDefaults.h"

#include <string>

namespace Ubiquitty::Preferences {

namespace {

/// The keys whose value is not a field read or an accessor call.
///
/// Enum-valued keys whose resolution the parse side handles in `assignSpecial`,
/// and two accessors whose return type no `Value` slot holds — a colourblind
/// mode and a list of directories. Each is spelled out rather than approximated,
/// because a token this returns is one a person sees in their file.
std::optional<Value> specialValue(Key key, const Values& values)
{
    const auto token = [](SerializeKind kind, std::string text) {
        return Value::ofText(kind, std::move(text));
    };
    switch (key) {
    case Key::AppearanceTheme:
        return token(SerializeKind::String, values.themeName.value_or(std::string {}));
    case Key::AppearanceSystemLightTheme:
        return token(SerializeKind::String, values.systemLightThemeName.value_or(std::string {}));
    case Key::AppearanceSystemDarkTheme:
        return token(SerializeKind::String, values.systemDarkThemeName.value_or(std::string {}));
    case Key::AppearanceBackgroundImageMode:
        return token(SerializeKind::EnumToken,
            backgroundImageModeToString(values.backgroundImageMode.value_or(BackgroundImageMode::Fill)));
    case Key::AppearanceTitlebarStyle:
        return token(SerializeKind::EnumToken,
            titlebarStyleToString(values.titlebarStyle.value_or(TitlebarStyle::Themed)));
    case Key::AppearanceColorblindMode:
        return token(SerializeKind::EnumToken,
            colorblindModeToString(Effective::colorblindMode(values)));
    case Key::CursorShape:
        return token(SerializeKind::EnumToken, cursorShapeToString(Effective::cursorShape(values)));
    case Key::CursorMotion:
        return token(SerializeKind::EnumToken, cursorMotionToString(Effective::cursorMotion(values)));
    case Key::TabsShowShortcuts:
        return token(SerializeKind::EnumToken, tabShortcutHintsToString(Effective::tabShortcutHints(values)));
    default:
        // Reached only if a key is routed here without an arm, which the caller's
        // exhaustive switch makes a programming error rather than an input.
        return std::nullopt;
    }
}

} // namespace

// Seven keys are read as Double: [font] size and line_spacing, the four
// [appearance] paddings, and titleless_padding_top. The field, the accessor,
// the parse and the write descriptor are all double for each — `size = 15.5`
// parses to 15.5 — and a Value describes the value, so the read reports what
// is there.
std::optional<Value> effectiveValue(Key key, const Values& values)
{
    switch (key) {
    case Key::AppearanceFollowSystem:
        return Value::ofBool(SerializeKind::Bool, Effective::followSystemAppearance(values));
    case Key::AppearanceTheme:
        return specialValue(Key::AppearanceTheme, values);
    case Key::AppearanceSystemLightTheme:
        return specialValue(Key::AppearanceSystemLightTheme, values);
    case Key::AppearanceSystemDarkTheme:
        return specialValue(Key::AppearanceSystemDarkTheme, values);
    case Key::AppearanceBackgroundOpacity:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::backgroundOpacity(values)));
    case Key::AppearanceTextContrastOnBlur:
        return Value::ofBool(SerializeKind::Bool, Effective::textVibrancy(values));
    case Key::AppearanceTextContrast:
        return Value::ofBool(SerializeKind::Bool, Effective::textContrast(values));
    case Key::AppearanceBackgroundVibrancy:
        return Value::ofBool(SerializeKind::Bool, Effective::backgroundVibrancy(values));
    case Key::AppearanceBlurRadius:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::blurRadius(values)));
    case Key::AppearanceBackgroundImage:
        return Value::ofText(SerializeKind::String, values.backgroundImage.value_or(std::string {}));
    case Key::AppearanceBackgroundImageOpacity:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::backgroundImageOpacity(values)));
    case Key::AppearanceBackgroundImageMode:
        return specialValue(Key::AppearanceBackgroundImageMode, values);
    // The four edges resolve over the `padding` shorthand and clamp, as the
    // renderer's fold reads them; the per-edge field alone would show the
    // built-in beside a window drawn with the shorthand.
    case Key::AppearancePaddingLeft:
        return Value::ofNumber(SerializeKind::Double, values.resolvedPadding().left);
    case Key::AppearancePaddingRight:
        return Value::ofNumber(SerializeKind::Double, values.resolvedPadding().right);
    case Key::AppearancePaddingTop:
        return Value::ofNumber(SerializeKind::Double, values.resolvedPadding().top);
    case Key::AppearancePaddingBottom:
        return Value::ofNumber(SerializeKind::Double, values.resolvedPadding().bottom);
    case Key::AppearanceTitlelessPaddingTop:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::titlelessPaddingTop(values)));
    case Key::AppearanceSquareBottomCorners:
        return Value::ofBool(SerializeKind::Bool, Effective::squareBottomCorners(values));
    case Key::AppearanceFolderIcon:
        return Value::ofBool(SerializeKind::Bool, Effective::folderIcon(values));
    case Key::AppearanceFolderIconAlways:
        return Value::ofBool(SerializeKind::Bool, Effective::folderIconAlways(values));
    case Key::AppearanceProcessIcons:
        return Value::ofBool(SerializeKind::Bool, Effective::processIcons(values));
    case Key::AppearanceTitlebarStyle:
        return specialValue(Key::AppearanceTitlebarStyle, values);
    case Key::AppearanceColorSpace:
        // The resolved space, so an unset key reads as the default it applies
        // and a document stating that token configures identically.
        return Value::ofText(SerializeKind::EnumToken, colorSpaceToString(Effective::colorSpaceP3(values) ? ColorSpace::DisplayP3 : ColorSpace::SRGB));
    case Key::AppearancePaneTitleBars:
        return Value::ofBool(SerializeKind::Bool, Effective::paneTitleBars(values));
    case Key::TransferZModem:
        return Value::ofBool(SerializeKind::Bool, Effective::shouldWatchForZModem(values));
    case Key::AppearanceBlinkAnimation:
        return Value::ofBool(SerializeKind::Bool, Effective::blinkAnimation(values));
    case Key::AppearanceSnapWindowResize:
        return Value::ofBool(SerializeKind::Bool, Effective::snapWindowResize(values));
    case Key::AppearanceHideTitleBar:
        return Value::ofBool(SerializeKind::Bool, Effective::hideTitleBar(values));
    case Key::AppearanceColorblindMode:
        return specialValue(Key::AppearanceColorblindMode, values);
    case Key::AppearanceUIFontScale:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::uiFontScale(values)));
    case Key::AppearanceTagColorOpacity:
        return Value::ofNumber(SerializeKind::Double, Effective::tagColorOpacity(values));
    case Key::FontFamily:
        return Value::ofText(SerializeKind::String, values.fontFamily.value_or(std::string {}));
    case Key::FontProportionalFamily:
        return Value::ofText(SerializeKind::String, values.proportionalFontFamily.value_or(std::string {}));
    case Key::FontFallback:
        return Value::ofText(SerializeKind::String, values.fallbackFont.value_or(std::string {}));
    case Key::FontSymbolFallback:
        return Value::ofText(SerializeKind::String, values.symbolFallbackFont.value_or(std::string {}));
    case Key::FontEmojiFallback:
        return Value::ofText(SerializeKind::String, values.emojiFallbackFont.value_or(std::string {}));
    case Key::FontCJKFallback:
        return Value::ofText(SerializeKind::String, values.cjkFallbackFont.value_or(std::string {}));
    case Key::FontFeatures:
        return Value::ofText(SerializeKind::String, values.fontFeatures.value_or(std::string {}));
    case Key::FontVariations:
        return Value::ofText(SerializeKind::String, values.fontVariations.value_or(std::string {}));
    case Key::FontSize:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::fontSize(values)));
    case Key::FontProportionalSize:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::proportionalFontSize(values)));
    case Key::FontLigatures:
        return Value::ofBool(SerializeKind::Bool, Effective::ligatures(values));
    case Key::FontLineSpacing:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::lineSpacing(values)));
    case Key::FontCharacterSpacing:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::characterSpacing(values)));
    case Key::FontWeight:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::fontWeight(values)));
    case Key::FontBoldWeight:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::boldFontWeight(values)));
    case Key::FontWidth:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::fontWidthPercent(values)));
    case Key::FontBaselineOffset:
        return Value::ofNumber(SerializeKind::Double, values.baselineOffset.value_or(0));
    case Key::FontMatchFallbackSize:
        return Value::ofBool(SerializeKind::Bool, Effective::matchFallbackSize(values));
    case Key::FontWideGlyphBoost:
        return Value::ofBool(SerializeKind::Bool, Effective::wideGlyphBoost(values));
    case Key::AppearanceShadeBlocks:
        return Value::ofText(SerializeKind::EnumToken, std::string(Rendering::shadeStyleToString(Effective::shadeStyle(values))));
    case Key::FontBrailleStyle:
        return Value::ofText(SerializeKind::EnumToken, std::string(Rendering::brailleStyleToString(Effective::brailleStyle(values))));
    case Key::FontBrailleGrid:
        return Value::ofBool(SerializeKind::Bool, Effective::brailleGrid(values));
    case Key::FontWideGlyphScale:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::wideGlyphScale(values)));
    case Key::UnderlineLinks:
        return Value::ofBool(SerializeKind::Bool, Effective::underlineLinks(values));
    case Key::UnderlineLinkStyle:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::linkUnderlineStyle(values)));
    case Key::UnderlineLinkColorBlue:
        return Value::ofBool(SerializeKind::Bool, Effective::linkColorSystemBlue(values));
    case Key::UnderlineLinkStylePerKind:
        return Value::ofBool(SerializeKind::Bool, Effective::linkStylePerKind(values));
    case Key::UnderlineThickness:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::underlineThickness(values)));
    case Key::UnderlineOffset:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::underlineOffset(values)));
    case Key::UnderlineSkipInk:
        return Value::ofBool(SerializeKind::Bool, Effective::underlineSkipInk(values));
    case Key::UnderlineFollowsInk:
        return Value::ofBool(SerializeKind::Bool, Effective::underlineFollowsInk(values));
    case Key::UnderlineSkipInkPad:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::underlineSkipInkPad(values)));
    case Key::TextBrightenBold:
        return Value::ofBool(SerializeKind::Bool, Effective::brightenBold(values));
    case Key::AppearanceBeamTerminalFont:
        return Value::ofBool(SerializeKind::Bool, Effective::beamTerminalFont(values));
    case Key::TekBrightPen:
        return Value::ofBool(SerializeKind::Bool, Effective::tekBrightPen(values));
    case Key::TekGlowVectors:
        return Value::ofBool(SerializeKind::Bool, Effective::tekGlowVectors(values));
    case Key::TekColorMode:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::tekColorMode(values)));
    case Key::TekBeamColor:
        // The accessor resolves to a packed colour; the file holds hex text and
        // that is what a row edits.
        return Value::ofText(SerializeKind::String, values.tekBeamColor.value_or(std::string {}));
    case Key::TekBackgroundColor:
        return Value::ofText(SerializeKind::String, values.tekBackgroundColor.value_or(std::string {}));
    case Key::SelectionSmartDoubleClick:
        return Value::ofBool(SerializeKind::Bool, Effective::smartDoubleClick(values));
    case Key::SelectionCopyOnSelect:
        return Value::ofBool(SerializeKind::Bool, Effective::copyOnSelect(values));
    case Key::SelectionLinksOverrideReporting:
        return Value::ofBool(SerializeKind::Bool, Effective::linksOverrideReporting(values));
    case Key::SelectionOSC8ClickOpens:
        return Value::ofBool(SerializeKind::Bool, Effective::osc8ClickOpens(values));
    case Key::SelectionRestrictToText:
        return Value::ofBool(SerializeKind::Bool, Effective::restrictSelectionToText(values));
    case Key::SelectionCopyStyle:
        return Value::ofText(SerializeKind::String, values.copyStyle.value_or(std::string {}));
    case Key::SelectionCopyStyleTheme:
        return Value::ofText(SerializeKind::String, values.copyStyleTheme.value_or(std::string {}));
    case Key::PasteConfirmMultiline:
        return Value::ofBool(SerializeKind::Bool, Effective::confirmMultilinePaste(values));
    case Key::PasteConfirmMultilineThreshold:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::confirmMultilinePasteThreshold(values)));
    case Key::KeysAccessorySymbols:
        return Value::ofText(SerializeKind::String, values.keyboardAccessorySymbols.value_or(Defaults::keyboardAccessorySymbols));
    case Key::ClipboardRemoteTools:
        // A bool: the setting is binary — the host's own clipboard, or this
        // machine's — so a row edits a toggle, and the "host"/"local" aliases a
        // person may have typed round-trip through the writer unchanged. Effective:: rather than the raw field,
        // so an absent key reads as the safe default rather than as nothing.
        return Value::ofBool(SerializeKind::LegacyBoolOrToken,
            Effective::remoteClipboardTools(values) == RemoteClipboardTools::Client);
    case Key::MouseWheelSpeed:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::wheelSpeed(values)));
    case Key::MouseWheelLines:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::wheelLines(values)));
    case Key::MouseScrollBounce:
        return Value::ofBool(SerializeKind::Bool, Effective::scrollBounce(values));
    case Key::MouseTrackpadSpeed:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::trackpadSpeed(values)));
    case Key::TerminalScrollbackLines:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::scrollbackLines(values)));
    case Key::TerminalEncoding:
        return Value::ofText(SerializeKind::EnumToken, values.encoding.value_or(std::string {}));
    case Key::TerminalColumnModeResizesWindow:
        return Value::ofBool(SerializeKind::Bool, Effective::columnModeResizesWindow(values));
    case Key::TerminalAmbiguousWidth:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::ambiguousWidth(values)));
    case Key::TitleWindowDirectory:
        return Value::ofBool(SerializeKind::Bool, values.titleWindowDirectory.value_or(false));
    case Key::TitleWindowDirectoryPath:
        return Value::ofBool(SerializeKind::Bool, values.titleWindowDirectoryPath.value_or(false));
    case Key::TitleWindowAppTitle:
        return Value::ofBool(SerializeKind::Bool, values.titleWindowAppTitle.value_or(false));
    case Key::TitleWindowProcess:
        return Value::ofBool(SerializeKind::Bool, values.titleWindowProcess.value_or(false));
    case Key::TitleWindowProcessArguments:
        return Value::ofBool(SerializeKind::Bool, values.titleWindowProcessArguments.value_or(false));
    case Key::TitleWindowShellCommand:
        return Value::ofBool(SerializeKind::Bool, values.titleWindowShellCommand.value_or(false));
    case Key::TitleWindowTTYName:
        return Value::ofBool(SerializeKind::Bool, values.titleWindowTTYName.value_or(false));
    case Key::TitleWindowProfileName:
        return Value::ofBool(SerializeKind::Bool, values.titleWindowProfileName.value_or(false));
    case Key::TitleWindowText:
        return Value::ofText(SerializeKind::String, values.titleWindowText.value_or(std::string {}));
    case Key::TitleWindowDimensions:
        return Value::ofBool(SerializeKind::Bool, values.titleWindowDimensions.value_or(false));
    case Key::TitleWindowProgramStatus:
        return Value::ofBool(SerializeKind::Bool, values.titleWindowProgramStatus.value_or(false));
    case Key::TitleTabDirectory:
        return Value::ofBool(SerializeKind::Bool, values.titleTabDirectory.value_or(false));
    case Key::TitleTabDirectoryPath:
        return Value::ofBool(SerializeKind::Bool, values.titleTabDirectoryPath.value_or(false));
    case Key::TitleTabAppTitle:
        return Value::ofBool(SerializeKind::Bool, values.titleTabAppTitle.value_or(false));
    case Key::TitleTabProcess:
        return Value::ofBool(SerializeKind::Bool, values.titleTabProcess.value_or(false));
    case Key::TitleTabProcessArguments:
        return Value::ofBool(SerializeKind::Bool, values.titleTabProcessArguments.value_or(false));
    case Key::TitleTabShellCommand:
        return Value::ofBool(SerializeKind::Bool, values.titleTabShellCommand.value_or(false));
    case Key::TitleTabTTYName:
        return Value::ofBool(SerializeKind::Bool, values.titleTabTTYName.value_or(false));
    case Key::TitleTabProfileName:
        return Value::ofBool(SerializeKind::Bool, values.titleTabProfileName.value_or(false));
    case Key::TitleTabText:
        return Value::ofText(SerializeKind::String, values.titleTabText.value_or(std::string {}));
    case Key::TitleTabDimensions:
        return Value::ofBool(SerializeKind::Bool, values.titleTabDimensions.value_or(false));
    case Key::TitleTabProgramStatus:
        return Value::ofBool(SerializeKind::Bool, values.titleTabProgramStatus.value_or(false));
    case Key::TitlePaneDirectory:
        return Value::ofBool(SerializeKind::Bool, values.titlePaneDirectory.value_or(false));
    case Key::TitlePaneDirectoryPath:
        return Value::ofBool(SerializeKind::Bool, values.titlePaneDirectoryPath.value_or(false));
    case Key::TitlePaneAppTitle:
        return Value::ofBool(SerializeKind::Bool, values.titlePaneAppTitle.value_or(false));
    case Key::TitlePaneProcess:
        return Value::ofBool(SerializeKind::Bool, values.titlePaneProcess.value_or(false));
    case Key::TitlePaneProcessArguments:
        return Value::ofBool(SerializeKind::Bool, values.titlePaneProcessArguments.value_or(false));
    case Key::TitlePaneShellCommand:
        return Value::ofBool(SerializeKind::Bool, values.titlePaneShellCommand.value_or(false));
    case Key::TitlePaneTTYName:
        return Value::ofBool(SerializeKind::Bool, values.titlePaneTTYName.value_or(false));
    case Key::TitlePaneProfileName:
        return Value::ofBool(SerializeKind::Bool, values.titlePaneProfileName.value_or(false));
    case Key::TitlePaneText:
        return Value::ofText(SerializeKind::String, values.titlePaneText.value_or(std::string {}));
    case Key::TitlePaneDimensions:
        return Value::ofBool(SerializeKind::Bool, values.titlePaneDimensions.value_or(false));
    case Key::TitlePaneProgramStatus:
        return Value::ofBool(SerializeKind::Bool, values.titlePaneProgramStatus.value_or(false));
    case Key::TitleDirectoryHiddenFor:
        return Value::ofText(SerializeKind::String, Effective::directoryHiddenFor(values));
    case Key::TitleDirectoryHiddenInFullScreen:
        return Value::ofBool(SerializeKind::Bool, Effective::directoryHiddenInFullScreen(values));
    case Key::CursorShape:
        return specialValue(Key::CursorShape, values);
    case Key::CursorBlink:
        return Value::ofBool(SerializeKind::Bool, Effective::cursorBlink(values));
    case Key::CursorLinger:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::cursorLinger(values)));
    case Key::CursorBlinkRate:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::cursorBlinkRate(values)));
    case Key::CursorAlpha:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(Effective::cursorAlpha(values)));
    case Key::CursorInvertText:
        return Value::ofBool(SerializeKind::Bool, Effective::cursorInvertText(values));
    case Key::CursorMotion:
        return specialValue(Key::CursorMotion, values);
    case Key::BellAudible:
        return Value::ofBool(SerializeKind::Bool, Effective::bellAudible(values));
    case Key::BellVisual:
        return Value::ofBool(SerializeKind::Bool, Effective::bellVisual(values));
    case Key::SoundAudible:
        return Value::ofBool(SerializeKind::Bool, Effective::soundAudible(values));
    case Key::SoundVolume:
        return Value::ofNumber(SerializeKind::Double, Effective::soundVolume(values));
    case Key::TabsShowShortcuts:
        return specialValue(Key::TabsShowShortcuts, values);
    case Key::WindowDragModifier:
        return Value::ofText(SerializeKind::EnumToken, Effective::windowDragModifier(values));
    case Key::TerminalCloseConfirmation:
        return Value::ofText(SerializeKind::EnumToken, Effective::closeConfirmation(values));
    case Key::TerminalCloseIgnoredProcesses:
        return Value::ofText(SerializeKind::String, Effective::closeIgnoredProcesses(values));
    case Key::SelectionSelectModifier:
        return Value::ofText(SerializeKind::EnumToken, Effective::selectModifier(values));
    case Key::SelectionBlockSelectModifier:
        return Value::ofText(SerializeKind::EnumToken, Effective::blockSelectModifier(values));
    case Key::SelectionStyle:
        return Value::ofText(SerializeKind::EnumToken, std::string(Rendering::selectionStyleToString(Effective::selectionStyle(values))));
    case Key::WindowInitialColumns:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(values.windowInitialColumns.value_or(Defaults::windowInitialColumns)));
    case Key::WindowInitialRows:
        return Value::ofInteger(SerializeKind::Int, static_cast<long long>(values.windowInitialRows.value_or(Defaults::windowInitialRows)));
    case Key::QuakeEnabled:
        return Value::ofBool(SerializeKind::Bool, Effective::quakeEnabled(values));
    case Key::QuakeHotkey:
        return Value::ofText(SerializeKind::String, Effective::quakeHotkey(values));
    case Key::QuakeHeight:
        return Value::ofNumber(SerializeKind::Double, static_cast<double>(Effective::quakeHeight(values)));
    case Key::QuakeAnimate:
        return Value::ofBool(SerializeKind::Bool, Effective::quakeAnimate(values));
    case Key::NotificationsEnabled:
        return Value::ofBool(SerializeKind::Bool, Effective::notifications(values));
    case Key::NotificationsSound:
        return Value::ofBool(SerializeKind::Bool, Effective::notificationSound(values));
    case Key::RemoteAutoInstallHost:
        return Value::ofBool(SerializeKind::Bool, Effective::autoDeployHostOnRemoteConnect(values));
    case Key::RemoteHistoryCacheOnDisk:
        return Value::ofBool(SerializeKind::Bool, Effective::shouldPersistRemoteHistory(values));
    case Key::RemoteNonOwnerPresentation:
        return Value::ofText(SerializeKind::EnumToken, std::string(Rendering::remoteViewportPolicyToString(Effective::nonOwnerPresentation(values))));
    case Key::DockBounce:
        return Value::ofBool(SerializeKind::Bool, Effective::dockBounce(values));
    case Key::DockBadge:
        return Value::ofBool(SerializeKind::Bool, Effective::dockBadge(values));
    case Key::SecuritySecureKeyboardEntry:
        return Value::ofBool(SerializeKind::Bool, Effective::secureKeyboardEntry(values));
    case Key::SecurityPasswordDetect:
        return Value::ofBool(SerializeKind::Bool, Effective::passwordDetect(values));
    case Key::SecuritySecureKeyboardEntryAtPasswordPrompts:
        return Value::ofBool(SerializeKind::Bool, Effective::secureKeyboardEntryAtPasswordPrompts(values));
    case Key::ControlEnable:
        return Value::ofBool(SerializeKind::Bool, Effective::controlEnabled(values));
    case Key::ControlSocketPath:
        return Value::ofText(SerializeKind::String, values.controlSocketPath.value_or(std::string {}));
    case Key::ControlToken:
        return Value::ofText(SerializeKind::String, values.controlToken.value_or(std::string {}));
    case Key::ControlApprove:
        return Value::ofBool(SerializeKind::Bool, Effective::controlApprove(values));
    case Key::ExperimentalSessionPersistence:
        return Value::ofBool(SerializeKind::Bool, Effective::sessionPersistence(values));
    case Key::DebugShowBuildHash:
        return Value::ofBool(SerializeKind::Bool, Effective::debugShowBuildHash(values));
    case Key::UpdatesCheckAutomatically:
        return Value::ofBool(SerializeKind::Bool, Effective::shouldCheckForUpdatesAutomatically(values));
    case Key::TermProgram:
        return Value::ofText(SerializeKind::String, std::string { terminalIdentity(values).program });
    case Key::TermVersion:
        return Value::ofText(SerializeKind::String, values.termProgramVersion.value_or(std::string {}));
    }
    // No `default:` above, so a new enumerator fails to compile rather than
    // falling through. This is unreachable for any enumerator and answers a
    // value cast in from outside the enumeration.
    return std::nullopt;
}

// The literal a value serializes to, or nothing when its kind disagrees with
// the key's. Shared by both overloads so the two cannot refuse different things.
namespace {

std::optional<std::string> serializedFor(const WriteDescriptor& descriptor, const Value& value)
{
    // The kind is the contract between the two halves. A caller handing a bool
    // for a key the file stores as a number would otherwise write `true` where
    // a reader expects a digit, and the parse would skip it as the wrong value
    // kind -- a silent no-op rather than a refusal.
    if (value.kind != descriptor.kind)
        return std::nullopt;
    switch (descriptor.kind) {
    case SerializeKind::Bool:
        return serializeWrite(descriptor.kind, value.boolean);
    // `LegacyBoolOrToken` reads out of the boolean slot because that is the
    // slot `effectiveValue` fills for it -- see the `ClipboardRemoteTools` arm,
    // which resolves the token to a bool because a settings row edits a
    // checkbox. A token that is not a bool rides in `text`, which a boolean
    // value leaves empty.
    case SerializeKind::LegacyBoolOrToken:
        if (!value.text.empty())
            return tomlString(value.text);
        return serializeWrite(descriptor.kind, value.boolean);
    case SerializeKind::Int:
        return serializeWrite(descriptor.kind, value.integer);
    case SerializeKind::Double:
        return serializeWrite(descriptor.kind, value.number);
    case SerializeKind::EnumToken:
    case SerializeKind::String:
        return serializeWrite(descriptor.kind, std::string_view { value.text });
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> writeValueIntoBody(Key key, const Value& value, std::string body)
{
    const WriteDescriptor* descriptor = descriptorFor(key);
    if (descriptor == nullptr)
        return std::nullopt;
    const std::optional<std::string> serialized = serializedFor(*descriptor, value);
    if (!serialized.has_value())
        return std::nullopt;
    return setSectionKey(std::move(body), descriptor->section, descriptor->key, *serialized);
}

bool writeValue(Key key, const Value& value, const std::string& path)
{
    const WriteDescriptor* descriptor = descriptorFor(key);
    if (descriptor == nullptr)
        return false;
    const std::optional<std::string> serialized = serializedFor(*descriptor, value);
    if (!serialized.has_value())
        return false;
    return writeKeyValue(path, descriptor->section, descriptor->key, *serialized);
}

std::optional<std::string> configureKey(Key key, const Value& value)
{
    std::optional<std::string> document = writeValueIntoBody(key, value, liveDocument());
    if (!document.has_value())
        return std::nullopt;
    // Permissive: the edit itself is well formed, and a strict refusal here
    // would hold the whole document hostage to a line the reader wrote by hand
    // somewhere else in it.
    (void)configure(*document, ParseMode::Permissive);
    return document;
}

} // namespace Ubiquitty::Preferences
