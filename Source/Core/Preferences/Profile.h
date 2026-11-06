// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A named, serializable bundle of presentation intent (theme / font /
// line-spacing / padding / optional startup host). Declarative — names, sizes,
// and choices only, no pixels or platform handles. Each field pairs with a
// `<field>Set` flag so an absent key is distinguishable from an explicit default
// (required for layered resolution: an unset override field inherits the base).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Core/Preferences/Schema.h"

// Compile-time platform identity for the [platform] override axis; keys off
// compiler macros only. Exactly one UBIQUITTY_PLATFORM_* is 1, the rest 0 (works
// in `#if` and `if constexpr`). Mac is the host-test default.
#if defined(__linux__)
#define UBIQUITTY_PLATFORM_MAC 0
#define UBIQUITTY_PLATFORM_LINUX 1
#else
#define UBIQUITTY_PLATFORM_MAC 1
#define UBIQUITTY_PLATFORM_LINUX 0
#endif

namespace Ubiquitty::Preferences {

/// The [platform] override slots (Docs/platform-config-and-sync.md).
/// currentPlatform() resolves to this build's platform via the
/// UBIQUITTY_PLATFORM_* selector.
enum class Platform {
    Mac = 0,
    Linux
};

// This build's platform, fixed at compile time.
constexpr Platform currentPlatform()
{
#if UBIQUITTY_PLATFORM_LINUX
    return Platform::Linux;
#else
    return Platform::Mac;
#endif
}

/// The kind of frontend a build's built-in defaults are chosen for.
///
/// Wider than ``Platform``, which names only the `[platform]` override slots a
/// file can carry and which an iOS build reads as `mac`. A default that suits a
/// desktop window and not a touch screen is told apart here, in Core, rather
/// than by a frontend overriding the value privately.
enum class DefaultsTarget : std::uint8_t {
    Desktop,
    IOS
};

/// This build's target, fixed at compile time from the compiler's own iOS
/// environment macro, which an iOS device or simulator build predefines.
///
/// - Returns: ``DefaultsTarget/IOS`` in an iOS build, else ``DefaultsTarget/Desktop``.
constexpr DefaultsTarget currentDefaultsTarget()
{
#if defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
    return DefaultsTarget::IOS;
#else
    return DefaultsTarget::Desktop;
#endif
}

const char* platformToString(Platform p);
// Map a [platform] header name ("mac"|"linux") to a Platform.
// Returns false on any other name (the section is then a normal section).
bool platformFromString(const std::string& s, Platform& out);

// Lifetime / scope of a profile's settings (Docs/platform-config-and-sync.md).
// Temporary profiles belong to a live session; only explicitly shared profiles
// are eligible for export. Persistence excludes temporary profiles.
enum class ProfileScope {
    Local = 0, // per-machine, persists, never leaves the device (default)
    Synced, // travels with the session/user
    Ephemeral // this session only; neither persists nor syncs
};

// The number of ProfileScope values — the count a UI picker iterates and the
// bound profileScopeFromInt validates against. Keep in step with the enum.
inline constexpr int profileScopeCount = 3;

const char* profileScopeToString(ProfileScope s);
ProfileScope profileScopeFromString(const std::string& s); // unknown → Local

// Index <-> enum for the Values scope picker (0 = Local, 1 = Synced,
// 2 = Ephemeral). profileScopeFromInt clamps an out-of-range index to Local.
int profileScopeToInt(ProfileScope s);
ProfileScope profileScopeFromInt(int index);

// The picker label for a scope index (0 = "Local", 1 = "Synced",
// 2 = "Ephemeral"); an out-of-range index yields "Local".
const char* profileScopeLabel(int index);

// One named profile. The "Default" profile is synthesized from the live global
// config (see profileFromValues); user `[[profiles]]` blocks add more. Theme is
// addressed by *name* (portable across machines) with an optional index escape hatch.
struct Profile {
    std::string name; // mandatory; identifies the profile

    // Optional parent profile *name*: an unset field falls back to the parent's
    // value, then its parent's, … then the global default. Empty == no parent.
    // resolveProfileChain() guards against cycles.
    std::optional<std::string> parentProfile;

    std::optional<ProfileScope> scope;

    // Presentation intent — applied via the renderer's existing setters.
    std::optional<std::string> themeName; // theme keyed by name; an index is never persisted
    std::optional<std::string> fontFamily;
    std::optional<std::string> fallbackFont;
    std::optional<double> fontSize; // points
    std::optional<double> lineSpacing; // extra pt
    std::optional<double> paddingLeft; // points, per edge
    std::optional<double> paddingRight; // points, per edge
    std::optional<double> paddingTop; // points, per edge
    std::optional<double> titlelessPaddingTop; // chromeless top inset, points
    std::optional<double> paddingBottom; // points, per edge

    // macOS chromeless window. Unset → inherit global [appearance] hide_title_bar;
    // set → this profile opens chromeless (or chrome'd) regardless. ⌃⌘H still overrides at runtime.
    std::optional<bool> hideTitleBar;

    // Optional startup host: when set, "New Window with Profile" opens against this
    // host (e.g. an ssh target) instead of the local shell. Empty == local shell.
    std::optional<std::string> startupHost;

    /// The local process exit choice; unset inherits the parent or keeps output.
    std::optional<std::string> processExit;

    Profile() = default;
    explicit Profile(std::string n)
        : name(std::move(n))
    {
    }
};

// The numeric Profile fields a Values editor bounds with a stepper. Each maps
// to the matching [section] key in Preferences::Schema for its min/max/step.
enum class ProfileField {
    FontSize,
    LineSpacing,
    PaddingLeft,
    PaddingRight,
    PaddingTop,
    TitlelessPaddingTop,
    PaddingBottom,
};

// The shared clamp range for a numeric Profile field, from Schema::rangeFor.
// Always resolves (every ProfileField has a schema row), so returned by value.
Schema::NumericRange profileFieldRange(ProfileField field);

// --- Platform-override presentation fields -----------------------------
// The overridable presentation fields for the [mac]/[linux] editor
// (Docs/platform-config-and-sync.md).
// `token` is the stable bridge id (must match the frontend field id); `tomlKey`
// is the key in the [<plat>] section; `kind` says how the value serializes.
// font_size is the one Integer key the parser reads back as int.
enum class PlatformFieldKind {
    String, // free text (theme name, font family, startup host)
    Number, // fractional number (line spacing, padding)
    Integer, // whole number read back as int (font size)
};

struct PlatformOverrideField {
    const char* token; // bridge token (matches the frontend field id)
    const char* tomlKey; // key in the [<plat>] section
    PlatformFieldKind kind;
};

inline constexpr PlatformOverrideField platformOverrideFields[] = {
    { "theme", "theme", PlatformFieldKind::String },
    { "fontFamily", "font_family", PlatformFieldKind::String },
    { "fallbackFont", "font_fallback", PlatformFieldKind::String },
    { "fontSize", "font_size", PlatformFieldKind::Integer },
    { "lineSpacing", "line_spacing", PlatformFieldKind::Number },
    { "paddingLeft", "padding_left", PlatformFieldKind::Number },
    { "paddingRight", "padding_right", PlatformFieldKind::Number },
    { "paddingTop", "padding_top", PlatformFieldKind::Number },
    { "titlelessPaddingTop", "titleless_padding_top", PlatformFieldKind::Number },
    { "paddingBottom", "padding_bottom", PlatformFieldKind::Number },
    { "startupHost", "startup_host", PlatformFieldKind::String },
};

inline constexpr int platformOverrideFieldCount = static_cast<int>(sizeof(platformOverrideFields) / sizeof(platformOverrideFields[0]));

// The override-field descriptor for a bridge token, or nullptr for an unknown
// token (the bridge then rejects the write). constexpr string compare.
inline constexpr const PlatformOverrideField* platformOverrideFieldForToken(const char* token)
{
    for (int i = 0; i < platformOverrideFieldCount; ++i) {
        const char* a = platformOverrideFields[i].token;
        const char* b = token;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == *b) // both reached the terminator → equal
            return &platformOverrideFields[i];
    }
    return nullptr;
}

// Resolve `override` against `base`, field by field: a set override field wins;
// an unset one inherits the base's value (and set-flag). Result's name/scope
// come from the override.
Profile resolve(const Profile& base, const Profile& override);

// Left-fold resolve() across N layers: layers[0] is the base, each later layer
// overrides per set field. Empty list → empty Profile; single layer as-is.
Profile resolveLayers(const std::vector<Profile>& layers);

// Cap on parentProfile chain depth, so a long or cyclic chain can't run away.
inline constexpr int maxProfileChainDepth = 32;

// Resolve `child` over `globalDefault`, walking the parentProfile chain first
// (child → parent → … → globalDefault), nearer layers winning. `findByName` looks
// a profile up (nullptr when absent). Cycles / depth past maxProfileChainDepth
// stop the walk; the result's identity (name/scope/parent) is the child's.
Profile resolveProfileChain(const Profile& child, const Profile& globalDefault, const std::function<const Profile*(const std::string&)>& findByName);

// Serialize one profile as a `[[profiles]]` TOML block (header included),
// emitting only set fields. `name` is always written.
std::string serializeProfile(const Profile& p);

// Serialize a list of profiles back-to-back (each its own block).
std::string serializeProfiles(const std::vector<Profile>&);

} // namespace Ubiquitty::Preferences

// profileFromValues needs the full Values type, so it's declared after Preferences.h
// pulls Profile.h in. Definition in Profile.cpp.

namespace Ubiquitty::Preferences {
struct Values;

/// Synthesizes the implicit "Default" profile from a set of preferences.
///
/// The theme, font, spacing and padding a file states outside any `[[profiles]]`
/// block are a profile in everything but name, and this is that profile. A
/// chosen profile resolves over it, so a field the chosen one omits inherits
/// the global value rather than a compile-time default.
///
/// - Parameters:
///   - c: The preferences to read the global fields from.
///   - name: The name to give the synthesized profile.
/// - Returns: A profile carrying the global fields under `name`.
Profile profileFromValues(const Values& c, const char* name);
/// Resolves one named profile against a configuration, chain and all.
///
/// The lookup and the chain walk are the same wherever a profile is named —
/// a menu item, a command line, a restored window — so they resolve here
/// rather than in each frontend's own loop over `Values::profiles`. A name
/// that matches nothing answers the documented default, which is the profile
/// a window with no named one opens under.
///
/// - Parameters:
///   - c: The resolved configuration holding the profiles.
///   - name: The profile's name.
/// - Returns: The fully resolved profile.
[[nodiscard]] Profile resolvedProfile(const Values& c, const std::string& name);

/// Resolves a session over shared intent, local defaults, platform, and profile layers.
///
/// - Parameters:
///   - configuration: The device configuration.
///   - name: The selected local profile.
///   - shared: The reviewed shared presentation values.
///   - temporary: The session-owned overrides, applied last.
/// - Returns: The effective presentation for this session.
[[nodiscard]] Profile resolvedSessionProfile(const Values& configuration, const std::string& name, const Profile& shared, const Profile& temporary);

/// The configuration with one profile's set fields laid over it.
///
/// A profile is a scoped override of keys the configuration already carries,
/// so it resolves into `Values` before anything derives from them: the
/// renderer fold, the launch fold and the window facts read one overlaid
/// configuration, and a field the profile leaves unset keeps the
/// configuration's answer. The profile's fallback font is the glyph fallback
/// (`[font] symbol_fallback`), not the CJK one.
///
/// - Parameters:
///   - configuration: The live configuration.
///   - profile: The resolved profile to lay over it.
/// - Returns: The overlaid configuration.
[[nodiscard]] Values overlaidWithProfile(const Values& configuration, const Profile& profile);

/// Projects explicitly shared profiles without local parent values or execution settings.
///
/// - Parameter configuration: The device configuration.
/// - Returns: Standalone presentation profiles eligible for explicit transfer.
[[nodiscard]] std::vector<Profile> profilesForSharing(const Values& configuration);

/// Removes execution settings and local paths from a received presentation profile.
///
/// - Parameter profile: The untrusted presentation proposal.
/// - Returns: A standalone profile containing only transferable settings.
[[nodiscard]] Profile presentationProfile(const Profile& profile);

/// Reads one explicitly shared profile as a sanitized review proposal.
///
/// - Parameter text: A bounded TOML profile transfer, not device configuration.
/// - Returns: The presentation proposal, or no value when the transfer is invalid.
[[nodiscard]] std::optional<Profile> profileFromTransferText(const std::string& text);

} // namespace Ubiquitty::Preferences
