// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// TerminalLaunchSeed.h — the per-terminal config seeding a pane needs at
// launch, folded once rather than assembled by each frontend beside its own
// spawn — the invalid-UTF-8 policy, the theme, the cursor-blink
// preference, the OSC 52 clipboard default, the trigger and character-class
// rules — and a knob present in one arm's copy and absent from another's is
// exactly how a config setting loads clean and never fires. The window model
// seeds every pane it mints through this; a frontend with its own spawn path
// calls the same fold.

#include "Core/Accessibility/Palette.h"
#include "Core/Modes/Emulation.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Preferences/CursorPreferences.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Runtime/LaunchTheme.h"
#include "Core/Runtime/RuntimeLink.h"
#include "Core/Runtime/TriggerConfig.h"
#include "Core/Tek/Colors.h"
#include "Core/TerminalOperations.h"
#include "Core/Themes/TermPaletteFromTheme.h"
#include "Core/Themes/Themes.h"
#include "Core/Unicode/UTF8Decoder.h"

#include <string>

namespace Ubiquitty::Runtime {

/// The `[security]` per-OSC / per-DCS sequence gate the config describes.
///
/// Parses the four raw lists whole; an unset list is empty (everything
/// passes). One fold for every terminal a config governs, so a terminal a
/// Core module mints without reading config — a tmux pane — carries the
/// same value the seeded ones do.
///
/// A deny list that does not fit closes its dimension instead of truncating
/// open: the fold records the overflow on the gate, and the gate denies
/// everything while it stands. The truncation is still logged where the parse
/// drops it, so the closed gate never arrives unexplained.
///
/// - Parameter config: The resolved config naming the lists, if any.
/// - Returns: The gate, ready to install.
[[nodiscard]] inline OSC::SequenceGate sequenceGateFromPreferences(const Preferences::Values& config)
{
    // Each parser writes at most the list's capacity and returns what it wrote,
    // so the count it hands back is the count the list holds. Only the deny
    // dimensions report drops: an over-capacity ALLOW list truncates
    // fail-closed on its own (fewer sequences pass than asked for).
    OSC::SequenceGate gate;
    int droppedDeny = 0, droppedDCSDeny = 0, droppedAPCDeny = 0;
    gate.oscAllowCount = config.oscAllow.has_value() ? OSC::parseOSCList(*config.oscAllow, gate.oscAllow.data(), OSC::oscListCapacity) : 0;
    gate.oscDenyCount = config.oscDeny.has_value() ? OSC::parseOSCList(*config.oscDeny, gate.oscDeny.data(), OSC::oscListCapacity, &droppedDeny) : 0;
    gate.dcsAllowCount = config.dcsAllow.has_value() ? OSC::parseDCSList(*config.dcsAllow, gate.dcsAllow.data(), OSC::dcsListCapacity) : 0;
    gate.dcsDenyCount = config.dcsDeny.has_value() ? OSC::parseDCSList(*config.dcsDeny, gate.dcsDeny.data(), OSC::dcsListCapacity, &droppedDCSDeny) : 0;
    gate.apcAllowCount = config.apcAllow.has_value() ? OSC::parseAPCList(*config.apcAllow, gate.apcAllow) : 0;
    gate.apcDenyCount = config.apcDeny.has_value() ? OSC::parseAPCList(*config.apcDeny, gate.apcDeny, &droppedAPCDeny) : 0;
    gate.hasOSCDenyOverflow = droppedDeny > 0;
    gate.hasDCSDenyOverflow = droppedDCSDeny > 0;
    gate.hasAPCDenyOverflow = droppedAPCDeny > 0;
    return gate;
}

/// The `[security]` OSC 52 clipboard policy the config describes.
///
/// An unset key resolves to the policy's default — `prompt` for a read,
/// `allow` for a write. One fold for every terminal a config governs, so a
/// terminal a Core module mints without reading config — a tmux pane —
/// carries the same pair the seeded ones do.
///
/// - Parameter config: The resolved config naming the policies, if any.
/// - Returns: The policy pair, ready to install.
[[nodiscard]] inline OSC::OSC52Policy osc52PolicyFromPreferences(const Preferences::Values& config)
{
    OSC::OSC52Policy policy;
    if (config.clipboardRead.has_value())
        policy.read = OSC::clipboardReadPolicyFromString(*config.clipboardRead);
    if (config.clipboardWrite.has_value())
        policy.write = OSC::clipboardWritePolicyFromString(*config.clipboardWrite);
    return policy;
}

/// Seeds the storage-tube colours the user chose, which an unset `OSC 15`,
/// `OSC 16` or `OSC 18` colour reports (Tek/Colors.h). The renderers fold the
/// same keys through `applyTekSettings`, so a reply names the colour drawn.
///
/// - Parameters:
///   - term: The terminal that answers the queries.
///   - config: The resolved configuration.
inline void seedTekColorPreference(Ubiquitty::Terminal& term, const Preferences::Values& config)
{
    term.tekColorPreference.mode = Preferences::Effective::tekColorMode(config) == 1 ? Tek::ColorMode::Custom : Tek::ColorMode::Theme;
    term.tekColorPreference.beam = Tek::tekColorFromPackedRGB(Preferences::Effective::tekBeamColor(config));
    term.tekColorPreference.background = Tek::tekColorFromPackedRGB(Preferences::Effective::tekBackgroundColor(config));
}

/// Installs the `[security]` OSC 52 clipboard policy on one terminal.
///
/// The policy is per-terminal state, so a reload re-runs this on every live
/// terminal, and a session created under a configuration of its own — a
/// daemon's — is seeded from that configuration and no other.
///
/// - Parameters:
///   - term: The terminal the policy gates.
///   - config: The resolved config naming the policies, if any.
inline void seedOSC52Policy(Ubiquitty::Terminal& term, const Preferences::Values& config)
{
    OSC::applyOSC52Policy(term, osc52PolicyFromPreferences(config));
}

/// Installs the `[security]` per-OSC / per-DCS sequence gate on one terminal.
///
/// The gate is per-terminal state, so a reload re-runs this on every live
/// terminal, and a terminal the daemon adopts carries the value it was
/// seeded with.
///
/// - Parameters:
///   - term: The terminal the gate protects.
///   - config: The resolved config naming the lists, if any.
inline void seedSequenceGate(Ubiquitty::Terminal& term, const Preferences::Values& config)
{
    term.sequenceGate = sequenceGateFromPreferences(config);
}

/// The ground a pane opens on under the launch theme, before any session
/// exists to publish one.
///
/// The theme is applied and adapted as a session applying it would be, and
/// its default background is folded by ``chromeTint``, the fold a published
/// snapshot takes: a shell that paints with this before its first pane draws
/// and with the snapshot afterwards paints one colour, and reads one polarity.
///
/// - Parameter config: The resolved configuration.
/// - Returns: The tint, black when the registry holds no theme.
[[nodiscard]] inline ChromeTint launchGround(const Preferences::Values& config)
{
    const Themes::Theme* theme = Themes::get(launchThemeIndex(config));
    if (!theme)
        return chromeTint(RuntimeSnapshot {});
    TermPalette palette;
    palette.seedDefaults();
    Themes::applyToTermPalette(*theme, palette);
    Accessibility::PaletteAdaptation adaptation = Accessibility::paletteAdaptation();
    adaptation.mode = Preferences::Effective::colorblindMode(config);
    Accessibility::adaptPalette(palette, adaptation);
    constexpr float fullChannel = 65535.0F;
    RuntimeSnapshot snapshot;
    snapshot.defaultBackgroundRed = static_cast<float>(palette.defaultBackground.red) / fullChannel;
    snapshot.defaultBackgroundGreen = static_cast<float>(palette.defaultBackground.green) / fullChannel;
    snapshot.defaultBackgroundBlue = static_cast<float>(palette.defaultBackground.blue) / fullChannel;
    return chromeTint(snapshot);
}

/// Seeds one terminal's configured theme, and nothing else.
///
/// The headless render paths want the theme without the rest of the launch
/// fold — a snapshot seeds triggers separately, and a canned render seeds
/// nothing else at all.
///
/// - Parameters:
///   - term: The terminal to paint.
///   - config: The resolved config naming the theme, if any.
/// - Returns: Whether the config's theme is now on the terminal: true when
///   none is named (the palette a fresh Terminal is constructed with is the
///   answer) or the named one applied; false when a name did not resolve. A
///   headless caller refuses on false rather than render a palette nobody
///   asked for, and every arm refuses the same name the same way.
inline bool seedConfiguredTheme(Ubiquitty::Terminal& term, const Preferences::Values& config)
{
    // The reader's colour-vision mode, written where every theme application
    // reads it; the contrast strength beside it is the system's and stays
    // whatever the frontend last wrote.
    Accessibility::paletteAdaptation().mode = Preferences::Effective::colorblindMode(config);
    // The Terminal-bound theme form, never the bare-palette one: only it
    // moves paletteBaseline, and a theme applied without that survives until
    // the first reset and then lands on a colour the user never chose. A
    // user [[themes]] name resolves only after `registerThemesFromValues` ran.
    const int themeIndex = configuredThemeIndex(config);
    if (themeIndex >= 0)
        return Themes::applyThemeToTerminal(term, themeIndex);
    return config.themeName.value_or("").empty();
}

/// Applies the launch theme to a terminal under the current adaptation.
///
/// What a window opens on, and what it returns to when the adaptation moves:
/// a GUI frontend that sees the system's contrast switch change writes the
/// new strength and calls this for every open terminal, so the palette
/// re-derives from the theme rather than being adapted a second time.
///
/// - Parameters:
///   - term: The terminal to paint.
///   - config: The resolved config the theme ladder reads.
inline void seedLaunchTheme(Ubiquitty::Terminal& term, const Preferences::Values& config)
{
    Accessibility::paletteAdaptation().mode = Preferences::Effective::colorblindMode(config);
    const int launchTheme = launchThemeIndex(config);
    if (launchTheme >= 0)
        (void)Themes::applyThemeToTerminal(term, launchTheme);
}

/// Which theme a configuration seed lands on.
///
/// A window opens on a theme whether the config names one or not — the launch
/// ladder walks down to the house default. A headless render is held to a
/// golden minted for the config alone, so it wants only what the config named
/// and, when it named nothing, the palette a fresh Terminal is constructed
/// with. The same seed serves both; the caller names which.
enum class LaunchThemePolicy {
    LaunchLadder, ///< The window's screen: named theme, else the house default.
    ConfiguredOnly, ///< The configured theme, else the constructed palette.
};

/// Seeds one terminal's launch-time configuration from the config fold.
///
/// Applies the invalid-UTF-8 policy, the launch theme (through
/// ``launchThemeIndex``), the `[cursor]` preferences, the OSC 52
/// clipboard default, the per-OSC / per-DCS sequence gate, the
/// password-prompt heuristic's switch, the ambiguous-width choice, and the trigger and character-class rules. The trigger
/// report comes back rather than printing here — where a diagnostic lands is
/// the frontend's, not the model's.
///
/// - Parameters:
///   - term: The terminal the launch just created.
///   - config: The resolved config to seed from.
/// - Returns: The trigger-config report, empty when there is nothing to say.
inline std::string seedTerminalConfiguration(Ubiquitty::Terminal& term, const Preferences::Values& config,
    LaunchThemePolicy themePolicy = LaunchThemePolicy::LaunchLadder)
{
    if (auto policy = Unicode::invalidUTF8PolicyFromName(config.invalidUTF8.value_or("")))
        Modes::setInvalidUTF8Policy(&term, *policy);

    // The theme the caller's policy names: a window walks the launch ladder to
    // the house default, a headless render seeds only the configured theme so
    // its frame is the config's and not a screen the golden never recorded.
    if (themePolicy == LaunchThemePolicy::LaunchLadder)
        seedLaunchTheme(term, config);
    else
        (void)seedConfiguredTheme(term, config);

    // The `[cursor]` block, through the helper that owns its fields: the shape
    // and blink the cursor falls back to while no program has sent DECSCUSR,
    // the linger, the blink period and the glide. An explicit blink preference
    // defers mode 12 to the application. The shape is the field a seed written
    // by hand keeps leaving out, and it is left out silently: the cursor still
    // draws, as the struct's zero, a block, whatever the configuration names.
    Preferences::Effective::applyCursorPreferences(term, config);
    seedTekColorPreference(term, config);

    // The clipboard policy and the sequence gate are security controls and
    // per-terminal state; they are seeded here so a terminal minted through
    // any arm carries them.
    seedOSC52Policy(term, config);
    seedSequenceGate(term, config);
    // The heuristic is a per-terminal switch the frontend's lock and echo
    // mask depend on; seeded here so every frontend that seeds through this
    // fold runs it, rather than only the one that remembered to.
    setPasswordDetect(&term, Preferences::Effective::passwordDetect(config));
    // The bell cue, for the same reason and with the same consequence when it
    // is missed: `resolveBellEvent` reads this field alone, so a terminal
    // seeded without it stays at the enum's zero — Off — and a reader who has
    // never touched the setting gets no bell at all on the shell that skipped
    // the seed.
    term.bellMode = Preferences::Effective::bellMode(config);
    // The decoder's encoding, for the third time the same reason: the value is
    // a name in the file and an enum in the parser, and only one shell ever
    // resolved it. A terminal seeded without it decodes single-byte text as
    // UTF-8 whatever the reader configured.
    Modes::setInputEncoding(&term, Preferences::Effective::inputEncoding(config));
    // The ambiguous-width choice lives on the terminal and nowhere else, so a
    // terminal seeded without it measures East Asian Ambiguous text narrow
    // whatever the reader configured.
    term.ambiguousWidth = Unicode::Width::ambiguousWidthFromCells(Preferences::Effective::ambiguousWidth(config));
    // The history depth, a creation parameter: a local session and a daemon
    // session under one configuration keep the same history because both are
    // seeded here rather than each reading the key.
    setScrollbackLimit(&term, Preferences::Effective::scrollbackLines(config));

    return describeTriggerConfig(applyTriggerConfig(term, config));
}

/// Whether a live re-seed may move the decoder's encoding.
enum class EncodingReseed : std::uint8_t {
    /// The terminal decodes what its carrier delivers; follow the configuration.
    FollowConfiguration,
    /// The bytes arrive already decoded to UTF-8 — a tmux-routed pane — so a
    /// configured legacy encoding would decode correct bytes wrongly.
    KeepCurrent,
};

/// Re-seeds the per-terminal state a configuration change moves on a live
/// terminal.
///
/// The live subset of ``seedTerminalConfiguration``: the password-prompt
/// heuristic, the bell cue, the ambiguous width, the `[cursor]` preferences, the
/// storage-tube colours, the OSC 52 policy, the sequence gate and the encoding. Not the theme and not the
/// trigger rules, which a reload re-derives through their own paths, and never
/// `cursorStyle`, which is what DECSCUSR moved: a reload must not revoke what a
/// running program asked for.
///
/// The encoding is applied only when it moves, because applying it resets the
/// decoder and a half-consumed multibyte sequence would die for a change to an
/// unrelated key.
///
/// - Parameters:
///   - term: The live terminal.
///   - config: The configuration as it now stands.
///   - encoding: Whether the encoding may move.
inline void reseedTerminalConfiguration(Ubiquitty::Terminal& term, const Preferences::Values& config, EncodingReseed encoding)
{
    setPasswordDetect(&term, Preferences::Effective::passwordDetect(config));
    term.bellMode = Preferences::Effective::bellMode(config);
    term.ambiguousWidth = Unicode::Width::ambiguousWidthFromCells(Preferences::Effective::ambiguousWidth(config));
    Preferences::Effective::applyCursorPreferences(term, config);
    seedTekColorPreference(term, config);
    seedOSC52Policy(term, config);
    seedSequenceGate(term, config);
    if (encoding == EncodingReseed::KeepCurrent)
        return;
    const Unicode::Encoding wanted = Preferences::Effective::inputEncoding(config);
    if (Modes::inputEncoding(&term) != wanted)
        Modes::setInputEncoding(&term, wanted);
}

} // namespace Ubiquitty::Runtime
