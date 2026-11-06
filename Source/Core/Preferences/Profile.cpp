// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Scope-tag mapping, layered resolution, and TOML round-trip for the Profile
// model. Parsing of `[[profiles]]` blocks lives in Preferences.cpp.

#include "Core/Preferences/Profile.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <sstream>

#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/Schema.h"

namespace Ubiquitty::Preferences {

const char* profileScopeToString(ProfileScope s)
{
    switch (s) {
    case ProfileScope::Synced:
        return "synced";
    case ProfileScope::Ephemeral:
        return "ephemeral";
    case ProfileScope::Local:
        break;
    }
    return "local";
}

ProfileScope profileScopeFromString(const std::string& s)
{
    if (s == "synced")
        return ProfileScope::Synced;
    if (s == "ephemeral")
        return ProfileScope::Ephemeral;
    return ProfileScope::Local; // default + unknown
}

int profileScopeToInt(ProfileScope s) { return static_cast<int>(s); }

ProfileScope profileScopeFromInt(int index)
{
    for (const ProfileScope s : { ProfileScope::Local, ProfileScope::Synced, ProfileScope::Ephemeral }) {
        if (index == static_cast<int>(s))
            return s;
    }
    return ProfileScope::Local; // out of range → default
}

const char* profileScopeLabel(int index)
{
    switch (profileScopeFromInt(index)) {
    case ProfileScope::Synced:
        return "Synced";
    case ProfileScope::Ephemeral:
        return "Ephemeral";
    case ProfileScope::Local:
        break;
    }
    return "Local";
}

Schema::NumericRange profileFieldRange(ProfileField field)
{
    // The [section].key each profile field shares with the global control, so
    // the editor reads its bounds from the one Preferences::Schema table.
    const char* section = "appearance";
    const char* key = "padding";
    switch (field) {
    case ProfileField::FontSize:
        section = "font";
        key = "size";
        break;
    case ProfileField::LineSpacing:
        section = "font";
        key = "line_spacing";
        break;
    case ProfileField::PaddingLeft:
        key = "padding_left";
        break;
    case ProfileField::PaddingRight:
        key = "padding_right";
        break;
    case ProfileField::PaddingTop:
        key = "padding_top";
        break;
    case ProfileField::TitlelessPaddingTop:
        key = "titleless_padding_top";
        break;
    case ProfileField::PaddingBottom:
        key = "padding_bottom";
        break;
    }
    if (const Schema::NumericRange* r = Schema::rangeFor(section, key))
        return *r;
    // Fallback for a future field added without its schema row.
    return { 0.0, 0.0, 0.0, "" };
}

const char* platformToString(Platform p)
{
    switch (p) {
    case Platform::Linux:
        return "linux";
    case Platform::Mac:
        break;
    }
    return "mac";
}

bool platformFromString(const std::string& s, Platform& out)
{
    if (s == "mac") {
        out = Platform::Mac;
        return true;
    }
    if (s == "linux") {
        out = Platform::Linux;
        return true;
    }
    return false;
}

Profile resolve(const Profile& base, const Profile& override)
{
    Profile r = base; // inherit every base field + set-flag

    // Identity (name/parent/scope) comes from the override; an unnamed override
    // (e.g. a [platform] block) inherits the base's so folding never wipes identity.
    if (!override.name.empty())
        r.name = override.name;
    if (override.parentProfile.has_value()) {
        r.parentProfile = override.parentProfile;
    }
    if (override.scope.has_value()) {
        r.scope = override.scope;
    }

    if (override.themeName.has_value()) {
        r.themeName = override.themeName;
    }
    if (override.fontFamily.has_value()) {
        r.fontFamily = override.fontFamily;
    }
    if (override.fallbackFont.has_value()) {
        r.fallbackFont = override.fallbackFont;
    }
    if (override.fontSize.has_value()) {
        r.fontSize = override.fontSize;
    }
    if (override.lineSpacing.has_value()) {
        r.lineSpacing = override.lineSpacing;
    }
    if (override.paddingLeft.has_value()) {
        r.paddingLeft = override.paddingLeft;
    }
    if (override.paddingRight.has_value()) {
        r.paddingRight = override.paddingRight;
    }
    if (override.paddingTop.has_value()) {
        r.paddingTop = override.paddingTop;
    }
    if (override.titlelessPaddingTop.has_value()) {
        r.titlelessPaddingTop = override.titlelessPaddingTop;
    }
    if (override.paddingBottom.has_value()) {
        r.paddingBottom = override.paddingBottom;
    }
    if (override.hideTitleBar.has_value()) {
        r.hideTitleBar = override.hideTitleBar;
    }
    if (override.processExit.has_value()) {
        r.processExit = override.processExit;
    }
    if (override.startupHost.has_value()) {
        r.startupHost = override.startupHost;
    }

    return r;
}

Profile resolveLayers(const std::vector<Profile>& layers)
{
    if (layers.empty())
        return {};
    Profile acc = layers[0];
    for (std::size_t i = 1; i < layers.size(); ++i)
        acc = resolve(acc, layers[i]);
    return acc;
}

Profile resolveProfileChain(const Profile& child, const Profile& globalDefault, const std::function<const Profile*(const std::string&)>& findByName)
{
    // Collect the chain nearest-first: child, parent, grandparent, …. Stop on
    // a missing parent, a name already seen (cycle), a self-link, or the depth
    // cap — so a malformed config can't loop or recurse without bound.
    std::vector<const Profile*> chain;
    std::vector<std::string> visited;
    const Profile* node = &child;
    for (int depth = 0; node && depth < maxProfileChainDepth; ++depth) {
        bool seen = false;
        for (const std::string& n : visited) {
            if (n == node->name) {
                seen = true;
                break;
            }
        }
        if (seen)
            break; // cycle — drop this and everything above it
        chain.push_back(node);
        visited.push_back(node->name);
        if (!node->parentProfile.has_value() || node->parentProfile->empty() || (findByName == nullptr))
            break;
        node = findByName(*node->parentProfile);
    }

    // Fold global default (base) ← farthest ancestor ← … ← child (wins last),
    // so a nearer layer's set field overrides a farther one's; unset fields
    // fall through to the next ancestor and finally the global default.
    std::vector<Profile> layers;
    layers.reserve(chain.size() + 1);
    layers.push_back(globalDefault);
    for (auto& link : std::views::reverse(chain))
        layers.push_back(*link);
    return resolveLayers(layers);
}

std::string serializeProfile(const Profile& p)
{
    if (p.scope == ProfileScope::Ephemeral) {
        return {};
    }
    std::string out = "[[profiles]]\n";
    out += "name = ";
    out += tomlString(p.name);
    out += '\n';
    if (p.parentProfile.has_value()) {
        out += "parent = ";
        out += tomlString(*p.parentProfile);
        out += '\n';
    }
    if (p.scope.has_value()) {
        out += "scope = ";
        out += tomlString(profileScopeToString(*p.scope));
        out += '\n';
    }
    if (p.themeName.has_value()) {
        out += "theme = ";
        out += tomlString(*p.themeName);
        out += '\n';
    }
    if (p.fontFamily.has_value()) {
        out += "font_family = ";
        out += tomlString(*p.fontFamily);
        out += '\n';
    }
    if (p.fallbackFont.has_value()) {
        out += "font_fallback = ";
        out += tomlString(*p.fallbackFont);
        out += '\n';
    }
    if (p.fontSize.has_value()) {
        out += "font_size = ";
        out += tomlDouble(*p.fontSize);
        out += '\n';
    }
    if (p.lineSpacing.has_value()) {
        out += "line_spacing = ";
        out += tomlDouble(*p.lineSpacing);
        out += '\n';
    }
    if (p.paddingLeft.has_value()) {
        out += "padding_left = ";
        out += tomlDouble(*p.paddingLeft);
        out += '\n';
    }
    if (p.paddingRight.has_value()) {
        out += "padding_right = ";
        out += tomlDouble(*p.paddingRight);
        out += '\n';
    }
    if (p.paddingTop.has_value()) {
        out += "padding_top = ";
        out += tomlDouble(*p.paddingTop);
        out += '\n';
    }
    if (p.titlelessPaddingTop.has_value()) {
        out += "titleless_padding_top = ";
        out += tomlDouble(*p.titlelessPaddingTop);
        out += '\n';
    }
    if (p.paddingBottom.has_value()) {
        out += "padding_bottom = ";
        out += tomlDouble(*p.paddingBottom);
        out += '\n';
    }
    if (p.hideTitleBar.has_value()) {
        out += "hide_title_bar = ";
        out += *p.hideTitleBar ? "true" : "false";
        out += '\n';
    }
    if (p.processExit.has_value()) {
        out += "process_exit = ";
        out += tomlString(*p.processExit);
        out += '\n';
    }
    if (p.startupHost.has_value()) {
        out += "startup_host = ";
        out += tomlString(*p.startupHost);
        out += '\n';
    }
    return out;
}

std::string serializeProfiles(const std::vector<Profile>& profiles)
{
    std::string out;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (i != 0)
            out += '\n';
        out += serializeProfile(profiles[i]);
    }
    return out;
}

Profile profileFromValues(const Values& c, const char* name)
{
    Profile p(name ? std::string(name) : std::string("Default"));

    // Theme by NAME, mirroring the [appearance] resolution the renderer does.
    if (c.themeName.has_value()) {
        p.themeName = c.themeName;
    }

    if (c.fallbackFont.has_value()) {
        p.fallbackFont = c.fallbackFont;
    }
    if (c.fontFamily.has_value()) {
        p.fontFamily = c.fontFamily;
    }
    if (c.fontSize.has_value()) {
        p.fontSize = c.fontSize;
    }
    if (c.lineSpacingPoints.has_value()) {
        p.lineSpacing = c.lineSpacingPoints;
    }

    // Padding: per-edge field wins, else the `padding` shorthand. The Default
    // profile always carries a concrete per-edge value so an omitting profile inherits it.
    if (c.paddingLeft.has_value()) {
        p.paddingLeft = c.paddingLeft;
    } else if (c.padding.has_value()) {
        p.paddingLeft = c.padding;
    }
    if (c.paddingRight.has_value()) {
        p.paddingRight = c.paddingRight;
    } else if (c.padding.has_value()) {
        p.paddingRight = c.padding;
    }
    if (c.paddingTop.has_value()) {
        p.paddingTop = c.paddingTop;
    } else if (c.padding.has_value()) {
        p.paddingTop = c.padding;
    }
    if (c.titlelessPaddingTop.has_value()) {
        p.titlelessPaddingTop = c.titlelessPaddingTop;
    }
    if (c.paddingBottom.has_value()) {
        p.paddingBottom = c.paddingBottom;
    } else if (c.padding.has_value()) {
        p.paddingBottom = c.padding;
    }

    // Chromeless: carry the global only when explicitly set. An unset global
    // leaves the flag unset so an omitting profile falls through to the built-in default (off).
    if (c.hideTitleBar.has_value()) {
        p.hideTitleBar = c.hideTitleBar;
    }

    return p;
}

Profile resolvedProfile(const Values& c, const std::string& name)
{
    const Profile fallback = profileFromValues(c, "Default");
    const auto findByName = [&c](const std::string& wanted) -> const Profile* {
        for (const Profile& candidate : c.profiles) {
            if (candidate.name == wanted)
                return &candidate;
        }
        return nullptr;
    };
    const Profile* named = findByName(name);
    if (named == nullptr)
        return fallback;
    return resolveProfileChain(*named, fallback, findByName);
}

Profile resolvedSessionProfile(const Values& configuration, const std::string& name, const Profile& shared, const Profile& temporary)
{
    const Profile defaults = applyCurrentPlatformOverride(resolve(shared, profileFromValues(configuration, "Default")), configuration);
    const auto find = [&configuration](const std::string& wanted) -> const Profile* {
        for (const Profile& profile : configuration.profiles) {
            if (profile.name == wanted) {
                return &profile;
            }
        }
        return nullptr;
    };
    const Profile* selected = find(name);
    return resolve(selected ? resolveProfileChain(*selected, defaults, find) : defaults, temporary);
}

Values overlaidWithProfile(const Values& configuration, const Profile& profile)
{
    Values overlaid = configuration;
    if (!profile.fontFamily.value_or("").empty())
        overlaid.fontFamily = profile.fontFamily;
    if (profile.fontSize.value_or(0.0) > 0.0)
        overlaid.fontSize = Schema::clampToRange(*profile.fontSize, "font", "size");
    if (!profile.fallbackFont.value_or("").empty())
        overlaid.symbolFallbackFont = profile.fallbackFont;
    if (profile.lineSpacing)
        overlaid.lineSpacingPoints = profile.lineSpacing;
    if (profile.paddingLeft)
        overlaid.paddingLeft = profile.paddingLeft;
    if (profile.paddingRight)
        overlaid.paddingRight = profile.paddingRight;
    if (profile.paddingTop)
        overlaid.paddingTop = profile.paddingTop;
    if (profile.titlelessPaddingTop)
        overlaid.titlelessPaddingTop = profile.titlelessPaddingTop;
    if (profile.paddingBottom)
        overlaid.paddingBottom = profile.paddingBottom;
    if (profile.hideTitleBar.has_value())
        overlaid.hideTitleBar = profile.hideTitleBar;
    if (!profile.themeName.value_or("").empty())
        overlaid.themeName = profile.themeName;
    return overlaid;
}

Profile presentationProfile(const Profile& profile)
{
    Profile result;
    const auto safeName = [](const std::string& value) {
        return value.size() <= 256 && value.find_first_of("/\\") == std::string::npos
            && std::ranges::none_of(value, [](unsigned char byte) { return byte < 32 || byte == 127; });
    };
    result.name = safeName(profile.name) ? profile.name : "Received Profile";
    result.scope = ProfileScope::Synced;
    for (auto member : { &Profile::themeName, &Profile::fontFamily, &Profile::fallbackFont }) {
        const auto& value = profile.*member;
        if (value && safeName(*value)) {
            result.*member = value;
        }
    }
    const auto bounded = [](std::optional<double> value, ProfileField field) -> std::optional<double> {
        const auto range = profileFieldRange(field);
        if (!value || !std::isfinite(*value) || *value < range.minimum || *value > range.maximum) {
            return std::nullopt;
        }
        return value;
    };
    result.fontSize = bounded(profile.fontSize, ProfileField::FontSize);
    result.lineSpacing = bounded(profile.lineSpacing, ProfileField::LineSpacing);
    result.paddingLeft = bounded(profile.paddingLeft, ProfileField::PaddingLeft);
    result.paddingRight = bounded(profile.paddingRight, ProfileField::PaddingRight);
    result.paddingTop = bounded(profile.paddingTop, ProfileField::PaddingTop);
    result.titlelessPaddingTop = bounded(profile.titlelessPaddingTop, ProfileField::TitlelessPaddingTop);
    result.paddingBottom = bounded(profile.paddingBottom, ProfileField::PaddingBottom);
    result.hideTitleBar = profile.hideTitleBar;
    return result;
}

std::vector<Profile> profilesForSharing(const Values& configuration)
{
    const auto findShared = [&configuration](const std::string& name) -> const Profile* {
        for (const Profile& profile : configuration.profiles) {
            if (profile.name == name && profile.scope == ProfileScope::Synced) {
                return &profile;
            }
        }
        return nullptr;
    };
    std::vector<Profile> result;
    for (const Profile& profile : configuration.profiles) {
        if (profile.scope == ProfileScope::Synced) {
            result.push_back(presentationProfile(resolveProfileChain(profile, {}, findShared)));
        }
    }
    return result;
}

std::optional<Profile> profileFromTransferText(const std::string& text)
{
    if (text.size() > 65536 || text.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    std::istringstream input(text);
    Values proposal;
    if (!parse(input, proposal, ParseMode::Strict) || proposal.profiles.size() != 1
        || proposal.profiles.front().scope != ProfileScope::Synced || proposal.profiles.front().name.empty()) {
        return std::nullopt;
    }
    return presentationProfile(proposal.profiles.front());
}

} // namespace Ubiquitty::Preferences
