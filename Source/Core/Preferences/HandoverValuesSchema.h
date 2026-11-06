// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/Preferences.h"

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace Ubiquitty::Preferences::HandoverDetail {

/// Couples one encoded member to its physical layout and stable name.
template<typename Owner, typename Value>
struct Field {
    Value Owner::* member;
    std::size_t offset;
    std::string_view name;
};

template<typename T> struct Schema;

/// Checks that the encoded fields tile the object except for alignment padding.
///
/// Padding-hole additions are additionally checked by compiler member enumeration.
/// - Returns: Whether every physical region is accounted for in declaration order.
template<typename T>
consteval bool isComplete()
{
    std::size_t end = 0;
    bool isValid = true;
    std::apply([&](const auto&... field) {
        auto check = [&](const auto& entry) {
            using Value = std::remove_reference_t<decltype(std::declval<T>().*entry.member)>;
            const auto expected = (end + alignof(Value) - 1) / alignof(Value) * alignof(Value);
            isValid = isValid && entry.offset == expected;
            end = entry.offset + sizeof(Value);
        };
        (check(field), ...);
    },
        Schema<T>::fields);
    return isValid && (end + alignof(T) - 1) / alignof(T) * alignof(T) == sizeof(T);
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

#define MEMBER(Type, name) \
    Field { &Type::name, offsetof(Type, name), #name }

template<> struct Schema<SkippedKey> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(SkippedKey, line),
        MEMBER(SkippedKey, section),
        MEMBER(SkippedKey, key),
        MEMBER(SkippedKey, reason),
        MEMBER(SkippedKey, expectedValueKind),
    };
};
static_assert(isComplete<SkippedKey>());

template<> struct Schema<ColorEntry> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(ColorEntry, red),
        MEMBER(ColorEntry, green),
        MEMBER(ColorEntry, blue),
        MEMBER(ColorEntry, alpha),
        MEMBER(ColorEntry, set),
    };
};
static_assert(isComplete<ColorEntry>());

template<> struct Schema<Themes::ThemeAppearance> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(Themes::ThemeAppearance, backgroundOpacity),
        MEMBER(Themes::ThemeAppearance, defaultBackgroundOpacity),
        MEMBER(Themes::ThemeAppearance, blurRadius),
        MEMBER(Themes::ThemeAppearance, textVibrancy),
        MEMBER(Themes::ThemeAppearance, textContrast),
        MEMBER(Themes::ThemeAppearance, backgroundVibrancy),
        MEMBER(Themes::ThemeAppearance, usesSystemColors),
        MEMBER(Themes::ThemeAppearance, terminalMarks),
    };
};
static_assert(isComplete<Themes::ThemeAppearance>());

template<> struct Schema<ThemeEntry> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(ThemeEntry, name),
        MEMBER(ThemeEntry, isDark),
        MEMBER(ThemeEntry, appearance),
        MEMBER(ThemeEntry, ansi),
        MEMBER(ThemeEntry, foreground),
        MEMBER(ThemeEntry, background),
        MEMBER(ThemeEntry, cursor),
        MEMBER(ThemeEntry, status),
    };
};
static_assert(isComplete<ThemeEntry>());

template<> struct Schema<KeyBindingEntry> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(KeyBindingEntry, spec),
        MEMBER(KeyBindingEntry, action),
    };
};
static_assert(isComplete<KeyBindingEntry>());

template<> struct Schema<LinkHandlerEntry> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(LinkHandlerEntry, scheme),
        MEMBER(LinkHandlerEntry, command),
    };
};
static_assert(isComplete<LinkHandlerEntry>());

template<> struct Schema<TagEntry> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(TagEntry, name),
        MEMBER(TagEntry, color),
        MEMBER(TagEntry, matchHost),
        MEMBER(TagEntry, matchProfile),
        MEMBER(TagEntry, prominence),
        MEMBER(TagEntry, icon),
        MEMBER(TagEntry, iconWeight),
        MEMBER(TagEntry, showsName),
        MEMBER(TagEntry, showsInTitleBar),
        MEMBER(TagEntry, showsInTabs),
        MEMBER(TagEntry, showsInPaneHeaders),
    };
};
static_assert(isComplete<TagEntry>());

template<> struct Schema<TriggerEntry> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(TriggerEntry, pattern),
        MEMBER(TriggerEntry, action),
    };
};
static_assert(isComplete<TriggerEntry>());

template<> struct Schema<MacroEntry> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(MacroEntry, name),
        MEMBER(MacroEntry, body),
    };
};
static_assert(isComplete<MacroEntry>());

template<> struct Schema<CharacterClassEntry> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(CharacterClassEntry, name),
        MEMBER(CharacterClassEntry, pattern),
    };
};
static_assert(isComplete<CharacterClassEntry>());

template<> struct Schema<LanguageFallbackEntry> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(LanguageFallbackEntry, script),
        MEMBER(LanguageFallbackEntry, language),
        MEMBER(LanguageFallbackEntry, mode),
        MEMBER(LanguageFallbackEntry, family),
    };
};
static_assert(isComplete<LanguageFallbackEntry>());

template<> struct Schema<RangeMapEntry> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(RangeMapEntry, firstCodePoint),
        MEMBER(RangeMapEntry, lastCodePoint),
        MEMBER(RangeMapEntry, range),
        MEMBER(RangeMapEntry, mode),
        MEMBER(RangeMapEntry, family),
        MEMBER(RangeMapEntry, shouldMatchSize),
    };
};
static_assert(isComplete<RangeMapEntry>());

template<> struct Schema<Profile> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(Profile, name),
        MEMBER(Profile, parentProfile),
        MEMBER(Profile, scope),
        MEMBER(Profile, themeName),
        MEMBER(Profile, fontFamily),
        MEMBER(Profile, fallbackFont),
        MEMBER(Profile, fontSize),
        MEMBER(Profile, lineSpacing),
        MEMBER(Profile, paddingLeft),
        MEMBER(Profile, paddingRight),
        MEMBER(Profile, paddingTop),
        MEMBER(Profile, titlelessPaddingTop),
        MEMBER(Profile, paddingBottom),
        MEMBER(Profile, hideTitleBar),
        MEMBER(Profile, startupHost),
        MEMBER(Profile, processExit),
    };
};
static_assert(isComplete<Profile>());

template<> struct Schema<PlatformOverride> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(PlatformOverride, profile),
        MEMBER(PlatformOverride, keyBindings),
        MEMBER(PlatformOverride, present),
    };
};
static_assert(isComplete<PlatformOverride>());

template<> struct Schema<CapabilityOverrides> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(CapabilityOverrides, informational),
        MEMBER(CapabilityOverrides, hostEscaping),
        MEMBER(CapabilityOverrides, readExfiltration),
    };
};
static_assert(isComplete<CapabilityOverrides>());

template<> struct Schema<Values> {
    inline static constexpr auto fields = std::tuple {
        MEMBER(Values, fontFamily),
        MEMBER(Values, proportionalFontFamily),
        MEMBER(Values, fallbackFont),
        MEMBER(Values, symbolFallbackFont),
        MEMBER(Values, shouldMatchFallbackSize),
        MEMBER(Values, emojiFallbackFont),
        MEMBER(Values, cjkFallbackFont),
        MEMBER(Values, languageFallbacks),
        MEMBER(Values, rangeMap),
        MEMBER(Values, fontSize),
        MEMBER(Values, proportionalFontSize),
        MEMBER(Values, baselineOffset),
        MEMBER(Values, ligatures),
        MEMBER(Values, fontFeatures),
        MEMBER(Values, fontVariations),
        MEMBER(Values, lineSpacingPoints),
        MEMBER(Values, characterSpacingPoints),
        MEMBER(Values, fontWeight),
        MEMBER(Values, boldFontWeight),
        MEMBER(Values, fontWidthPercent),
        MEMBER(Values, wideGlyphBoost),
        MEMBER(Values, brailleGrid),
        MEMBER(Values, brailleStyle),
        MEMBER(Values, wideGlyphScale),
        MEMBER(Values, underlineThickness),
        MEMBER(Values, underlineOffset),
        MEMBER(Values, underlineSkipInk),
        MEMBER(Values, underlineFollowsInk),
        MEMBER(Values, underlineSkipInkPad),
        MEMBER(Values, underlineLinks),
        MEMBER(Values, linkUnderlineStyle),
        MEMBER(Values, linkColorSystemBlue),
        MEMBER(Values, linkStylePerKind),
        MEMBER(Values, smartDoubleClick),
        MEMBER(Values, brightenBold),
        MEMBER(Values, beamTerminalFont),
        MEMBER(Values, tekBrightPen),
        MEMBER(Values, tekGlowVectors),
        MEMBER(Values, tekColorMode),
        MEMBER(Values, tekBeamColor),
        MEMBER(Values, tekBackgroundColor),
        MEMBER(Values, themeName),
        MEMBER(Values, colorSpace),
        MEMBER(Values, padding),
        MEMBER(Values, paddingLeft),
        MEMBER(Values, paddingRight),
        MEMBER(Values, paddingTop),
        MEMBER(Values, paddingBottom),
        MEMBER(Values, titlelessPaddingTop),
        MEMBER(Values, squareBottomCorners),
        MEMBER(Values, titlebarStyle),
        MEMBER(Values, beamSurface),
        MEMBER(Values, folderIcon),
        MEMBER(Values, folderIconAlways),
        MEMBER(Values, processIcons),
        MEMBER(Values, paneTitleBars),
        MEMBER(Values, shouldWatchForZModem),
        MEMBER(Values, blinkAnimation),
        MEMBER(Values, snapWindowResize),
        MEMBER(Values, hideTitleBar),
        MEMBER(Values, colorblindMode),
        MEMBER(Values, shadeBlocks),
        MEMBER(Values, uiFontScale),
        MEMBER(Values, tagColorOpacity),
        MEMBER(Values, titleWindowDirectory),
        MEMBER(Values, titleWindowDirectoryPath),
        MEMBER(Values, titleWindowAppTitle),
        MEMBER(Values, titleWindowProcess),
        MEMBER(Values, titleWindowProcessArguments),
        MEMBER(Values, titleWindowShellCommand),
        MEMBER(Values, titleWindowTTYName),
        MEMBER(Values, titleWindowProfileName),
        MEMBER(Values, titleWindowText),
        MEMBER(Values, titleWindowDimensions),
        MEMBER(Values, titleWindowProgramStatus),
        MEMBER(Values, titleTabDirectory),
        MEMBER(Values, titleTabDirectoryPath),
        MEMBER(Values, titleTabAppTitle),
        MEMBER(Values, titleTabProcess),
        MEMBER(Values, titleTabProcessArguments),
        MEMBER(Values, titleTabShellCommand),
        MEMBER(Values, titleTabTTYName),
        MEMBER(Values, titleTabProfileName),
        MEMBER(Values, titleTabText),
        MEMBER(Values, titleTabDimensions),
        MEMBER(Values, titleTabProgramStatus),
        MEMBER(Values, titlePaneDirectory),
        MEMBER(Values, titlePaneDirectoryPath),
        MEMBER(Values, titlePaneAppTitle),
        MEMBER(Values, titlePaneProcess),
        MEMBER(Values, titlePaneProcessArguments),
        MEMBER(Values, titlePaneShellCommand),
        MEMBER(Values, titlePaneTTYName),
        MEMBER(Values, titlePaneProfileName),
        MEMBER(Values, titlePaneText),
        MEMBER(Values, titlePaneDimensions),
        MEMBER(Values, titlePaneProgramStatus),
        MEMBER(Values, directoryHiddenFor),
        MEMBER(Values, directoryHiddenInFullScreen),
        MEMBER(Values, followSystemAppearance),
        MEMBER(Values, systemDarkThemeName),
        MEMBER(Values, systemLightThemeName),
        MEMBER(Values, backgroundOpacity),
        MEMBER(Values, shouldEnhanceTextOnBlur),
        MEMBER(Values, textContrast),
        MEMBER(Values, backgroundVibrancy),
        MEMBER(Values, blurRadius),
        MEMBER(Values, backgroundImage),
        MEMBER(Values, backgroundImageOpacity),
        MEMBER(Values, defaultBackgroundOpacity),
        MEMBER(Values, backgroundImageMode),
        MEMBER(Values, cursorShape),
        MEMBER(Values, cursorBlink),
        MEMBER(Values, cursorLinger),
        MEMBER(Values, cursorBlinkRate),
        MEMBER(Values, cursorAlpha),
        MEMBER(Values, cursorInvertText),
        MEMBER(Values, cursorMotion),
        MEMBER(Values, cursorMotionDuration),
        MEMBER(Values, maxFPS),
        MEMBER(Values, vsync),
        MEMBER(Values, bellAudible),
        MEMBER(Values, bellVisual),
        MEMBER(Values, bellVisualIntensity),
        MEMBER(Values, soundAudible),
        MEMBER(Values, soundVolume),
        MEMBER(Values, copyOnSelect),
        MEMBER(Values, blockSelectModifier),
        MEMBER(Values, selectModifier),
        MEMBER(Values, selectionStyle),
        MEMBER(Values, closeIgnoredProcesses),
        MEMBER(Values, closeConfirmation),
        MEMBER(Values, linksOverrideReporting),
        MEMBER(Values, osc8ClickOpens),
        MEMBER(Values, restrictSelectionToText),
        MEMBER(Values, copyStyle),
        MEMBER(Values, copyStyleTheme),
        MEMBER(Values, confirmMultilinePaste),
        MEMBER(Values, confirmMultilinePasteThreshold),
        MEMBER(Values, remoteClipboardTools),
        MEMBER(Values, wheelSpeed),
        MEMBER(Values, wheelLines),
        MEMBER(Values, trackpadSpeed),
        MEMBER(Values, scrollBounce),
        MEMBER(Values, tabShortcutHints),
        MEMBER(Values, windowDragModifier),
        MEMBER(Values, windowProgramFullscreen),
        MEMBER(Values, windowInitialColumns),
        MEMBER(Values, windowInitialRows),
        MEMBER(Values, composeKey),
        MEMBER(Values, optionAsAlternate),
        MEMBER(Values, keyboardAccessorySymbols),
        MEMBER(Values, quakeEnabled),
        MEMBER(Values, quakeHotkey),
        MEMBER(Values, quakeHeight),
        MEMBER(Values, quakeAnimate),
        MEMBER(Values, dockBounce),
        MEMBER(Values, dockBadge),
        MEMBER(Values, secureKeyboardEntry),
        MEMBER(Values, passwordDetect),
        MEMBER(Values, secureKeyboardEntryAtPasswordPrompts),
        MEMBER(Values, clipboardRead),
        MEMBER(Values, clipboardWrite),
        MEMBER(Values, oscAllow),
        MEMBER(Values, oscDeny),
        MEMBER(Values, dcsAllow),
        MEMBER(Values, dcsDeny),
        MEMBER(Values, apcAllow),
        MEMBER(Values, apcDeny),
        MEMBER(Values, localCapabilities),
        MEMBER(Values, agentCapabilities),
        MEMBER(Values, remoteCapabilities),
        MEMBER(Values, controlEnabled),
        MEMBER(Values, controlSocketPath),
        MEMBER(Values, controlToken),
        MEMBER(Values, controlApprove),
        MEMBER(Values, sessionPersistence),
        MEMBER(Values, notifications),
        MEMBER(Values, notificationSound),
        MEMBER(Values, debugShowBuildHash),
        MEMBER(Values, shouldCheckForUpdatesAutomatically),
        MEMBER(Values, autoDeployHostOnRemoteConnect),
        MEMBER(Values, shouldPersistRemoteHistory),
        MEMBER(Values, nonOwnerPresentation),
        MEMBER(Values, proxyKind),
        MEMBER(Values, proxyHost),
        MEMBER(Values, proxyPort),
        MEMBER(Values, sshForwardAgent),
        MEMBER(Values, downloadDir),
        MEMBER(Values, transferAutoReceive),
        MEMBER(Values, termProgram),
        MEMBER(Values, termProgramVersion),
        MEMBER(Values, termIdentifier),
        MEMBER(Values, setsLocaleEnvironment),
        MEMBER(Values, encoding),
        MEMBER(Values, invalidUTF8),
        MEMBER(Values, emulation),
        MEMBER(Values, scrollbackLines),
        MEMBER(Values, columnModeResizesWindow),
        MEMBER(Values, ambiguousWidth),
        MEMBER(Values, normalization),
        MEMBER(Values, skippedKeys),
        MEMBER(Values, tags),
        MEMBER(Values, themes),
        MEMBER(Values, keyBindings),
        MEMBER(Values, linkHandlers),
        MEMBER(Values, linkLineHandler),
        MEMBER(Values, triggers),
        MEMBER(Values, allowRunCommand),
        MEMBER(Values, scriptingConfigScript),
        MEMBER(Values, scriptingEnable),
        MEMBER(Values, allowWebhook),
        MEMBER(Values, macros),
        MEMBER(Values, characterClasses),
        MEMBER(Values, profiles),
        MEMBER(Values, platformOverrides),
    };
};
static_assert(isComplete<Values>());

#undef MEMBER

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

} // namespace Ubiquitty::Preferences::HandoverDetail
