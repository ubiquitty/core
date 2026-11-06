// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_profile.cpp — the Profile data model: scope-tag mapping, [[profiles]]
// parsing, layered resolution (base "Default" ← override), and the
// serialize→parse round-trip that keeps profiles wire-friendly for the
// future synced config store.

#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/Profile.h"
#include "Core/Preferences/Schema.h"

#include <cstdio>
#include <sstream>
#include <string>

using Ubiquitty::Preferences::applyCurrentPlatformOverride;
using Ubiquitty::Preferences::applyPlatformOverride;
using Ubiquitty::Preferences::currentPlatform;
using Ubiquitty::Preferences::parse;
using Ubiquitty::Preferences::Platform;
using Ubiquitty::Preferences::platformFromString;
using Ubiquitty::Preferences::platformToString;
using Ubiquitty::Preferences::Profile;
using Ubiquitty::Preferences::ProfileField;
using Ubiquitty::Preferences::profileFieldRange;
using Ubiquitty::Preferences::profileFromTransferText;
using Ubiquitty::Preferences::profileFromValues;
using Ubiquitty::Preferences::ProfileScope;
using Ubiquitty::Preferences::profileScopeCount;
using Ubiquitty::Preferences::profileScopeFromInt;
using Ubiquitty::Preferences::profileScopeFromString;
using Ubiquitty::Preferences::profileScopeLabel;
using Ubiquitty::Preferences::profileScopeToInt;
using Ubiquitty::Preferences::profileScopeToString;
using Ubiquitty::Preferences::resolve;
using Ubiquitty::Preferences::resolveLayers;
using Ubiquitty::Preferences::resolveProfileChain;
using Ubiquitty::Preferences::serializeProfile;
using Ubiquitty::Preferences::serializeProfiles;
using Ubiquitty::Preferences::Values;

static Values parseString(const std::string& text)
{
    Values c;
    std::istringstream in(text);
    parse(in, c);
    return c;
}

/// Scope tags map both ways; unknown strings fall back to local.
TEST(testScopeRoundtrip)
{
    CHECK(profileScopeFromString("local") == ProfileScope::Local);
    CHECK(profileScopeFromString("synced") == ProfileScope::Synced);
    CHECK(profileScopeFromString("ephemeral") == ProfileScope::Ephemeral);
    CHECK(profileScopeFromString("bogus") == ProfileScope::Local);
    CHECK(std::string(profileScopeToString(ProfileScope::Local)) == "local");
    CHECK(std::string(profileScopeToString(ProfileScope::Synced)) == "synced");
    CHECK(std::string(profileScopeToString(ProfileScope::Ephemeral)) == "ephemeral");
}

/// The Values scope-picker index <-> enum encoding the macOS ProfilesPane
/// round-trips through: every index in [0, count) survives the round-trip, an
/// out-of-range index clamps to Local, and the labels match the enum order.
TEST(testScopeIndexAndLabel)
{
    CHECK(profileScopeCount == 3);
    CHECK(profileScopeFromInt(0) == ProfileScope::Local);
    CHECK(profileScopeFromInt(1) == ProfileScope::Synced);
    CHECK(profileScopeFromInt(2) == ProfileScope::Ephemeral);
    CHECK(profileScopeFromInt(3) == ProfileScope::Local); // out of range -> default
    CHECK(profileScopeFromInt(-1) == ProfileScope::Local);

    CHECK(profileScopeToInt(ProfileScope::Local) == 0);
    CHECK(profileScopeToInt(ProfileScope::Synced) == 1);
    CHECK(profileScopeToInt(ProfileScope::Ephemeral) == 2);

    for (int i = 0; i < profileScopeCount; ++i)
        CHECK(profileScopeToInt(profileScopeFromInt(i)) == i);

    CHECK(std::string(profileScopeLabel(0)) == "Local");
    CHECK(std::string(profileScopeLabel(1)) == "Synced");
    CHECK(std::string(profileScopeLabel(2)) == "Ephemeral");
    CHECK(std::string(profileScopeLabel(99)) == "Local"); // out of range -> default label

    // The stored token and the index both name the same scope (the two ends of
    // the bridge): toString(fromInt(i)) is the token the TOML would carry.
    CHECK(std::string(profileScopeToString(profileScopeFromInt(0))) == "local");
    CHECK(std::string(profileScopeToString(profileScopeFromInt(1))) == "synced");
    CHECK(std::string(profileScopeToString(profileScopeFromInt(2))) == "ephemeral");
}

/// Each numeric profile field's editor range is drawn from the shared
/// Preferences::Schema table — the same min/max/step/unit the global control and the
/// TOML parse clamp use — so the editor and the parser can't disagree.
TEST(testProfileFieldRangesMatchSchema)
{
    namespace Schema = Ubiquitty::Preferences::Schema;
    struct {
        ProfileField field;
        const char* section;
        const char* key;
    } cases[] = {
        { ProfileField::FontSize, "font", "size" },
        { ProfileField::LineSpacing, "font", "line_spacing" },
        { ProfileField::PaddingLeft, "appearance", "padding_left" },
        { ProfileField::PaddingRight, "appearance", "padding_right" },
        { ProfileField::PaddingTop, "appearance", "padding_top" },
        { ProfileField::TitlelessPaddingTop, "appearance", "titleless_padding_top" },
        { ProfileField::PaddingBottom, "appearance", "padding_bottom" },
    };
    for (const auto& tc : cases) {
        const Schema::NumericRange* expected = Schema::rangeFor(tc.section, tc.key);
        CHECK(expected != nullptr);
        Schema::NumericRange got = profileFieldRange(tc.field);
        CHECK(got.minimum == expected->minimum);
        CHECK(got.maximum == expected->maximum);
        CHECK(got.step == expected->step);
        CHECK(std::string(got.unit) == expected->unit);
    }
    // Spot-check the concrete numbers the macOS ProfileEditorSheet had inlined
    // (6...96 / 0...16 / 0...64), to pin that the shared table really carries
    // them rather than just matching itself.
    CHECK(profileFieldRange(ProfileField::FontSize).minimum == 6.0);
    CHECK(profileFieldRange(ProfileField::FontSize).maximum == 96.0);
    CHECK(profileFieldRange(ProfileField::LineSpacing).maximum == 16.0);
    CHECK(profileFieldRange(ProfileField::PaddingTop).maximum == 64.0);
}

/// No [[profiles]] blocks → empty list.
TEST(testNoProfiles)
{
    Values c = parseString("[font]\nsize = 14\n");
    CHECK(c.profiles.empty());
}

/// A full [[profiles]] block parses every field with its set-flag.
TEST(testParseFullProfile)
{
    Values c = parseString("[[profiles]]\n"
                           "name = \"Production\"\n"
                           "scope = \"synced\"\n"
                           "theme = \"Demo Dark\"\n"
                           "font_family = \"JetBrains Mono\"\n"
                           "font_size = 13.5\n"
                           "line_spacing = 2\n"
                           "padding_left = 8\n"
                           "padding_right = 7\n"
                           "padding_top = 6\n"
                           "titleless_padding_top = 12\n"
                           "padding_bottom = 5\n"
                           "hide_title_bar = true\n"
                           "startup_host = \"ssh prod\"\n");
    CHECK(c.profiles.size() == 1);
    const Profile& p = c.profiles[0];
    CHECK(p.name == "Production");
    CHECK(p.scope == ProfileScope::Synced);
    CHECK(p.themeName == "Demo Dark");
    CHECK(p.fontFamily == "JetBrains Mono");
    CHECK(p.fontSize == 13.5);
    CHECK(p.lineSpacing == 2.0); // int literal → double
    CHECK(p.paddingLeft == 8.0);
    CHECK(p.paddingRight == 7.0);
    CHECK(p.paddingTop == 6.0);
    CHECK(p.titlelessPaddingTop == 12.0);
    CHECK(p.paddingBottom == 5.0);
    CHECK(p.hideTitleBar == true);
    CHECK(p.startupHost == "ssh prod");
}

/// A sparse profile sets only what it names; the rest stay unset.
TEST(testParseSparseProfile)
{
    Values c = parseString("[[profiles]]\n"
                           "name = \"Light\"\n"
                           "theme = \"Solarized Light\"\n");
    CHECK(c.profiles.size() == 1);
    const Profile& p = c.profiles[0];
    CHECK(p.name == "Light");
    CHECK(p.themeName.has_value());
    CHECK(!p.fontFamily.has_value());
    CHECK(!p.fontSize.has_value());
    CHECK(!p.paddingLeft.has_value());
    // Unset, and the default it stands in for is the reader's to supply — which
    // is why the second line asks value_or rather than the field.
    CHECK(!p.scope.has_value());
    CHECK(p.scope.value_or(ProfileScope::Local) == ProfileScope::Local);
}

/// Multiple [[profiles]] blocks accumulate in file order; a following
/// [section] stops routing keys into the last profile.
TEST(testMultipleProfilesAndSectionBreak)
{
    Values c = parseString("[[profiles]]\n"
                           "name = \"A\"\n"
                           "font_size = 12\n"
                           "[[profiles]]\n"
                           "name = \"B\"\n"
                           "font_size = 16\n"
                           "[font]\n"
                           "size = 99\n"); // must not leak into profile B
    CHECK(c.profiles.size() == 2);
    CHECK(c.profiles[0].name == "A" && c.profiles[0].fontSize == 12.0);
    CHECK(c.profiles[1].name == "B" && c.profiles[1].fontSize == 16.0);
    CHECK(c.fontSize == 99.0);
}

/// Resolution: override fields win; unset override fields inherit base;
/// name and an explicit scope come from the override.
TEST(testResolveLayers)
{
    Profile base("Default");
    base.themeName = "Demo Dark";
    base.fontFamily = "Menlo";
    base.fontSize = 14;
    base.paddingLeft = 4;
    base.paddingBottom = 4;
    Profile over("Production");
    over.fontSize = 13;
    // wins
    over.themeName = "Nord";
    // wins
    over.scope = ProfileScope::Synced;
    // fontFamily/padding unset on override → inherit base

    Profile r = resolve(base, over);
    CHECK(r.name == "Production");
    CHECK(r.scope == ProfileScope::Synced);
    CHECK(r.themeName == "Nord"); // override
    CHECK(r.fontSize == 13.0); // override
    CHECK(r.fontFamily == "Menlo"); // inherited
    CHECK(r.paddingLeft == 4.0); // inherited
    CHECK(r.paddingBottom == 4.0); // inherited
}

/// An explicit override value that equals the default still wins over a
/// different base value — the set-flag, not equality, drives resolution.
TEST(testResolveExplicitBeatsInherit)
{
    Profile base("Default");
    base.lineSpacing = 5;
    Profile over("X");
    over.lineSpacing = 0;
    // explicit 0
    Profile r = resolve(base, over);
    CHECK(r.lineSpacing == 0.0);
}

/// serialize → parse → equal: every set field survives a round-trip, and
/// unset fields stay unset (no defaults invented).
TEST(testSerializeParseRoundtrip)
{
    Profile p("Roundtrip");
    p.scope = ProfileScope::Local;
    p.themeName = "Gruvbox";
    p.fontFamily = "Fira Code";
    p.fontSize = 12.5;
    p.lineSpacing = 1.5;
    p.paddingLeft = 10;
    p.paddingRight = 9;
    p.paddingTop = 7;
    p.titlelessPaddingTop = 11;
    p.paddingBottom = 6;
    p.hideTitleBar = true;
    p.startupHost = "ssh box \"a\"";
    // quote escaping

    std::string toml = serializeProfile(p);
    Values c = parseString(toml);
    CHECK(c.profiles.size() == 1);
    const Profile& q = c.profiles[0];
    CHECK(q.name == p.name);
    CHECK(q.scope == p.scope);
    CHECK(q.themeName == p.themeName);
    CHECK(q.fontFamily == p.fontFamily);
    CHECK(q.fontSize == p.fontSize);
    CHECK(q.lineSpacing == p.lineSpacing);
    CHECK(q.paddingLeft == p.paddingLeft);
    CHECK(q.paddingRight == p.paddingRight);
    CHECK(q.paddingTop == p.paddingTop);
    CHECK(q.titlelessPaddingTop == p.titlelessPaddingTop);
    CHECK(q.paddingBottom == p.paddingBottom);
    CHECK(q.hideTitleBar == p.hideTitleBar);
    CHECK(q.startupHost == p.startupHost);
}

/// serializeProfiles emits N independent blocks that parse back to N.
TEST(testSerializeMultiple)
{
    std::vector<Profile> ps;
    {
        Profile a("A");
        a.fontSize = 11;
        ps.push_back(a);
    }
    {
        Profile b("B");
        b.themeName = "Nord";
        ps.push_back(b);
    }
    std::string toml = serializeProfiles(ps);
    Values c = parseString(toml);
    CHECK(c.profiles.size() == 2);
    CHECK(c.profiles[0].name == "A" && c.profiles[0].fontSize == 11.0);
    CHECK(c.profiles[1].name == "B" && c.profiles[1].themeName == "Nord");
}

/// profileFromValues mirrors the flat [appearance]/[font] sections into a
/// Default profile; the `padding` shorthand fans out to both axes.
TEST(testProfileFromValues)
{
    Values c = parseString("[appearance]\n"
                           "theme = \"Demo Dark\"\n"
                           "padding = 6\n"
                           "[font]\n"
                           "family = \"Menlo\"\n"
                           "size = 14\n"
                           "line_spacing = 2\n");
    Profile d = profileFromValues(c, "Default");
    CHECK(d.name == "Default");
    CHECK(d.themeName == "Demo Dark");
    CHECK(d.fontFamily == "Menlo");
    CHECK(d.fontSize == 14.0);
    CHECK(d.lineSpacing == 2.0);
    CHECK(d.paddingLeft == 6.0); // shorthand → all edges
    CHECK(d.paddingRight == 6.0);
    CHECK(d.paddingTop == 6.0);
    CHECK(d.paddingBottom == 6.0);
}

/// The real flow: a sparse profile resolved over the Default inherits the
/// global theme/font but overrides only what it names.
TEST(testProfileOverDefault)
{
    Values c = parseString("[appearance]\n"
                           "theme = \"Demo Dark\"\n"
                           "padding = 4\n"
                           "[font]\n"
                           "family = \"Menlo\"\n"
                           "size = 14\n"
                           "[[profiles]]\n"
                           "name = \"Big\"\n"
                           "font_size = 18\n");
    Profile def = profileFromValues(c, "Default");
    CHECK(c.profiles.size() == 1);
    Profile r = resolve(def, c.profiles[0]);
    CHECK(r.name == "Big");
    CHECK(r.fontSize == 18.0); // override
    CHECK(r.fontFamily == "Menlo"); // inherited
    CHECK(r.themeName == "Demo Dark"); // inherited
    CHECK(r.paddingLeft == 4.0); // inherited
}

/// hide_title_bar resolves per-profile: a profile's value wins over the global,
/// an unset profile falls through to the global, and the Effective accessor
/// matches the resolved profile either way. Both polarities (profile turns it ON
/// over an OFF global, and OFF over an ON global) are exercised.
TEST(testHideTitleBarPerProfile)
{
    using Ubiquitty::Preferences::Effective::hideTitleBar;

    // Global ON; one profile leaves it unset (inherit), one forces it OFF.
    Values c = parseString("[appearance]\n"
                           "hide_title_bar = true\n"
                           "[[profiles]]\n"
                           "name = \"Inherit\"\n"
                           "[[profiles]]\n"
                           "name = \"Chrome\"\n"
                           "hide_title_bar = false\n");
    CHECK(c.hideTitleBar == true);
    CHECK(c.profiles.size() == 2);

    // profileFromValues carries the global into the Default base layer.
    Profile def = profileFromValues(c, "Default");
    CHECK(def.hideTitleBar == true);

    // Inherit profile (unset) → resolves to the global ON.
    Profile inherit = resolve(def, c.profiles[0]);
    CHECK(inherit.hideTitleBar == true);
    CHECK(hideTitleBar(c, c.profiles[0]) == true); // accessor: falls through

    // Chrome profile (explicit false) → resolves to OFF, beating the global.
    Profile chrome = resolve(def, c.profiles[1]);
    CHECK(chrome.hideTitleBar == false);
    CHECK(hideTitleBar(c, c.profiles[1]) == false); // accessor: profile wins

    // Mirror: global OFF (default), a profile forces it ON.
    Values c2 = parseString("[[profiles]]\n"
                            "name = \"Chromeless\"\n"
                            "hide_title_bar = true\n");
    CHECK(!c2.hideTitleBar.has_value()); // global default off
    CHECK(hideTitleBar(c2) == false);
    CHECK(c2.profiles.size() == 1);
    CHECK(hideTitleBar(c2, c2.profiles[0]) == true); // profile turns it on

    // An unset profile over an unset global → the built-in default (off).
    Profile plain("Plain");
    CHECK(hideTitleBar(c2, plain) == false);
}

/// Platform name <-> enum mapping; unknown names are rejected so a section
/// like [appearance] is never mistaken for a platform block. The host build
/// defaults currentPlatform() to mac (no frontend macro defined).
TEST(testPlatformStrings)
{
    Platform p = Platform::Linux;
    CHECK(platformFromString("mac", p) && p == Platform::Mac);
    CHECK(platformFromString("linux", p) && p == Platform::Linux);
    CHECK(!platformFromString("classic", p));
    CHECK(!platformFromString("appearance", p));
    CHECK(!platformFromString("", p));
    CHECK(std::string(platformToString(Platform::Mac)) == "mac");
    CHECK(std::string(platformToString(Platform::Linux)) == "linux");
    // currentPlatform() reflects the build's own platform (mac on a macOS
    // checkout, linux on the Linux host-test CI), mirroring Profile.h's macros.
#if UBIQUITTY_PLATFORM_LINUX
    CHECK(currentPlatform() == Platform::Linux);
#else
    CHECK(currentPlatform() == Platform::Mac);
#endif
}

/// resolveLayers left-folds resolve() across N layers in §6.5 order: later
/// layers win per set field, unset fields are preserved across the whole
/// fold. Demonstrated on font, colors (theme), and padding together.
TEST(testResolveLayersFold)
{
    Profile builtin("Default"); // layer 0: built-in default
    builtin.themeName = "Demo Dark";
    builtin.fontFamily = "Menlo";
    builtin.fontSize = 14;
    builtin.paddingLeft = 4;
    Profile synced("Synced"); // layer 1: overrides theme only
    synced.themeName = "Nord";
    Profile local("Local"); // layer 2: overrides font size only
    local.fontSize = 16;
    Profile r = resolveLayers({ builtin, synced, local });
    CHECK(r.name == "Local"); // last named layer
    CHECK(r.themeName == "Nord"); // synced won over builtin
    CHECK(r.fontSize == 16.0); // local won over builtin
    CHECK(r.fontFamily == "Menlo"); // never overridden
    CHECK(r.paddingLeft == 4.0); // never overridden
}

/// Edge cases of the fold: empty list yields an empty Profile; a single
/// layer is returned unchanged.
TEST(testResolveLayersEdges)
{
    Profile empty = resolveLayers({});
    CHECK(empty.name.empty() && !empty.themeName.has_value() && !empty.fontSize.has_value());

    Profile only("Solo");
    only.fontSize = 11;
    Profile r = resolveLayers({ only });
    CHECK(r.name == "Solo" && r.fontSize == 11.0);
}

/// A [mac] block overrides a base field; applied on mac, ignored on
/// linux. Demonstrated on font + colors (theme).
TEST(testPlatformOverrideAppliedAndIgnored)
{
    Values c = parseString("[mac]\n"
                           "font_family = \"SF Mono\"\n"
                           "theme = \"Nord\"\n");
    CHECK(c.platformOverride(Platform::Mac).present);
    CHECK(!c.platformOverride(Platform::Linux).present);

    Profile base("Default");
    base.fontFamily = "Menlo";
    base.themeName = "Demo Dark";
    base.fontSize = 14;
    Profile onMac = applyPlatformOverride(base, c, Platform::Mac);
    CHECK(onMac.fontFamily == "SF Mono"); // overridden
    CHECK(onMac.themeName == "Nord"); // overridden
    CHECK(onMac.fontSize == 14.0); // inherited
    CHECK(onMac.name == "Default"); // identity kept

    Profile onLinux = applyPlatformOverride(base, c, Platform::Linux);
    CHECK(onLinux.fontFamily == "Menlo"); // no [linux] block → unchanged
    CHECK(onLinux.themeName == "Demo Dark");

    // The build's own platform routes through to its [section]. On macOS that
    // is [mac] (SF Mono / Nord); on the Linux host-test build there is no
    // [linux] block in this config, so the base profile is preserved.
    Profile here = applyCurrentPlatformOverride(base, c);
#if UBIQUITTY_PLATFORM_MAC
    CHECK(here.fontFamily == "SF Mono" && here.themeName == "Nord");
#else
    CHECK(here.fontFamily == base.fontFamily && here.themeName == base.themeName);
#endif
}

/// The documented full chain: built-in Default → profile → [platform], with
/// the platform layer winning last.
TEST(testFullResolutionOrder)
{
    Values c = parseString("[appearance]\n"
                           "theme = \"Demo Dark\"\n"
                           "[font]\n"
                           "family = \"Menlo\"\n"
                           "size = 14\n"
                           "[[profiles]]\n"
                           "name = \"Work\"\n"
                           "font_size = 16\n"
                           "[mac]\n"
                           "font_size = 18\n");
    Profile def = profileFromValues(c, "Default");
    Profile resolved = resolveLayers({ def, c.profiles[0] }); // built-in → profile
    CHECK(resolved.fontSize == 16.0); // profile won

    Profile final = applyPlatformOverride(resolved, c, Platform::Mac);
    CHECK(final.name == "Work"); // identity preserved
    CHECK(final.fontSize == 18.0); // [mac] won last
    CHECK(final.fontFamily == "Menlo"); // inherited all the way
    CHECK(final.themeName == "Demo Dark");

    // On linux the [mac] block is inert: the profile's value stands.
    Profile linuxFinal = applyPlatformOverride(resolved, c, Platform::Linux);
    CHECK(linuxFinal.fontSize == 16.0);
}

/// An empty [linux] header marks the slot present even with no keys, so an
/// apply step can tell "block exists but sets nothing" from "no block".
TEST(testEmptyPlatformBlockPresent)
{
    Values c = parseString("[linux]\n");
    CHECK(c.platformOverride(Platform::Linux).present);
    CHECK(!c.platformOverride(Platform::Mac).present);
    Profile base("Default");
    base.fontSize = 12;
    Profile r = applyPlatformOverride(base, c, Platform::Linux);
    CHECK(r.fontSize == 12.0); // present but empty → no change
}

/// A following normal [section] stops routing keys into the platform block.
TEST(testPlatformBlockSectionBreak)
{
    Values c = parseString("[mac]\n"
                           "font_size = 20\n"
                           "[font]\n"
                           "size = 99\n"); // must not leak into the [mac] override
    CHECK(c.platformOverride(Platform::Mac).profile.fontSize.has_value());
    CHECK(c.platformOverride(Platform::Mac).profile.fontSize == 20.0);
    CHECK(c.fontSize == 99.0);
}

/// Profile inheritance: a parent sets A,B; a child sets B,C with parent=parent.
/// Resolving the child over the global default yields A from the parent, B
/// overridden by the child, C from the child, and every other field from the
/// global default.
TEST(testInheritSingleParent)
{
    Profile globalDefault("Default");
    globalDefault.fontFamily = "Menlo"; // a field neither parent nor child sets
    Profile parent("Base"); // sets A (theme) and B (fontSize)
    parent.themeName = "Nord"; // A
    parent.fontSize = 12; // B
    Profile child("Work"); // sets B (override) and C (lineSpacing)
    child.parentProfile = "Base";
    child.fontSize = 16; // B — overrides parent
    child.lineSpacing = 2; // C
    auto findByName = [&](const std::string& n) -> const Profile* {
        if (n == "Base")
            return &parent;
        return nullptr;
    };

    Profile r = resolveProfileChain(child, globalDefault, findByName);
    CHECK(r.name == "Work"); // child identity wins
    CHECK(r.themeName == "Nord"); // A — from parent
    CHECK(r.fontSize == 16.0); // B — child overrides parent
    CHECK(r.lineSpacing == 2.0); // C — from child
    CHECK(r.fontFamily == "Menlo"); // global default
    CHECK(!r.paddingLeft.has_value()); // unset everywhere → stays unset
}

/// A child with no parent resolves straight over the global default (the
/// findByName lookup is never consulted).
TEST(testInheritNoParent)
{
    Profile globalDefault("Default");
    globalDefault.fontFamily = "Menlo";
    Profile child("Solo");
    child.fontSize = 18;
    bool consulted = false;
    auto findByName = [&](const std::string&) -> const Profile* {
        consulted = true;
        return nullptr;
    };
    Profile r = resolveProfileChain(child, globalDefault, findByName);
    CHECK(!consulted); // no parent link → no lookup
    CHECK(r.name == "Solo");
    CHECK(r.fontSize == 18.0); // child
    CHECK(r.fontFamily == "Menlo"); // global default
}

/// A 3-deep chain A->B->C (C is the active child): each level contributes a
/// distinct field, and a nearer level overrides a farther one for a shared
/// field. Resolution order is grandparent under parent under child.
TEST(testInheritThreeDeep)
{
    Profile globalDefault("Default");
    globalDefault.paddingLeft = 4; // only the global default sets this
    Profile a("A"); // grandparent
    a.themeName = "GrandTheme"; // unique to A
    a.fontSize = 10; // shared down the chain
    Profile b("B"); // parent
    b.parentProfile = "A";
    b.fontFamily = "Fira Code"; // unique to B
    b.fontSize = 12; // overrides A
    Profile c("C"); // child / active
    c.parentProfile = "B";
    c.lineSpacing = 3; // unique to C
    c.fontSize = 14; // overrides B (and A)
    auto findByName = [&](const std::string& n) -> const Profile* {
        if (n == "A")
            return &a;
        if (n == "B")
            return &b;
        return nullptr;
    };

    Profile r = resolveProfileChain(c, globalDefault, findByName);
    CHECK(r.name == "C");
    CHECK(r.themeName == "GrandTheme"); // from A (grandparent)
    CHECK(r.fontFamily == "Fira Code"); // from B (parent)
    CHECK(r.lineSpacing == 3.0); // from C (child)
    CHECK(r.fontSize == 14.0); // nearest (C) wins
    CHECK(r.paddingLeft == 4.0); // global default
}

/// A cycle (A.parent=B, B.parent=A) is handled without an infinite loop: the
/// walk stops when it revisits a name. Resolution still produces a sane result
/// (the active profile + whatever the un-cycled prefix contributed).
TEST(testInheritCycleGuard)
{
    Profile globalDefault("Default");
    globalDefault.fontFamily = "Menlo";
    Profile a("A");
    a.parentProfile = "B";
    a.fontSize = 11;
    Profile b("B");
    b.parentProfile = "A"; // cycle back to A
    b.themeName = "Loopy";
    auto findByName = [&](const std::string& n) -> const Profile* {
        if (n == "A")
            return &a;
        if (n == "B")
            return &b;
        return nullptr;
    };

    // Active = A. Chain: A -> B -> (A already visited, stop). Must terminate.
    Profile r = resolveProfileChain(a, globalDefault, findByName);
    CHECK(r.name == "A"); // active identity
    CHECK(r.fontSize == 11.0); // from A
    CHECK(r.themeName == "Loopy"); // from B (one hop)
    CHECK(r.fontFamily == "Menlo"); // global default

    // A self-cycle (A.parent = A) also terminates after one node.
    Profile selfA("Self");
    selfA.parentProfile = "Self";
    selfA.fontSize = 9;
    auto findSelf = [&](const std::string& n) -> const Profile* { return n == "Self" ? &selfA : nullptr; };
    Profile rs = resolveProfileChain(selfA, globalDefault, findSelf);
    CHECK(rs.name == "Self" && rs.fontSize == 9.0);
    CHECK(rs.fontFamily == "Menlo");
}

/// A parent named in the link but absent from the lookup ends the walk
/// gracefully: the child resolves over the global default alone.
TEST(testInheritMissingParent)
{
    Profile globalDefault("Default");
    globalDefault.fontFamily = "Menlo";
    Profile child("Orphan");
    child.parentProfile = "DoesNotExist";
    child.fontSize = 20;
    auto findByName = [&](const std::string&) -> const Profile* { return nullptr; };
    Profile r = resolveProfileChain(child, globalDefault, findByName);
    CHECK(r.name == "Orphan");
    CHECK(r.fontSize == 20.0);
    CHECK(r.fontFamily == "Menlo"); // global default fallback
}

/// The `parent` key parses with its set-flag and survives a serialize->parse
/// round-trip; an omitted parent stays unset.
TEST(testParentParseAndRoundtrip)
{
    Values c = parseString("[[profiles]]\n"
                           "name = \"Child\"\n"
                           "parent = \"Base\"\n"
                           "font_size = 15\n");
    CHECK(c.profiles.size() == 1);
    CHECK(c.profiles[0].parentProfile.has_value() && c.profiles[0].parentProfile == "Base");

    std::string toml = serializeProfile(c.profiles[0]);
    Values rt = parseString(toml);
    CHECK(rt.profiles.size() == 1);
    CHECK(rt.profiles[0].parentProfile.has_value() && rt.profiles[0].parentProfile == "Base");

    // A profile with no parent neither writes nor parses the key.
    Profile noParent("Plain");
    CHECK(!noParent.parentProfile.has_value());
    Values np = parseString(serializeProfile(noParent));
    CHECK(np.profiles.size() == 1 && !np.profiles[0].parentProfile.has_value());
}

/// The end-to-end flow from parsed config: a child profile's parent chain is
/// walked over the profileFromValues Default, exactly as newWindowWithProfile
/// does (findByName closes over Values::profiles).
TEST(testInheritFromParsedConfig)
{
    Values c = parseString("[appearance]\n"
                           "theme = \"Demo Dark\"\n"
                           "[font]\n"
                           "family = \"Menlo\"\n"
                           "size = 14\n"
                           "[[profiles]]\n"
                           "name = \"Base\"\n"
                           "font_size = 18\n"
                           "padding_left = 9\n"
                           "[[profiles]]\n"
                           "name = \"Child\"\n"
                           "parent = \"Base\"\n"
                           "line_spacing = 2\n");
    CHECK(c.profiles.size() == 2);

    Profile def = profileFromValues(c, "Default");
    auto findByName = [&](const std::string& n) -> const Profile* {
        for (const Profile& p : c.profiles)
            if (p.name == n)
                return &p;
        return nullptr;
    };
    // Active profile is "Child" (index 1).
    Profile r = resolveProfileChain(c.profiles[1], def, findByName);
    CHECK(r.name == "Child");
    CHECK(r.lineSpacing == 2.0); // from child
    CHECK(r.fontSize == 18.0); // from Base parent
    CHECK(r.paddingLeft == 9.0); // from Base parent
    CHECK(r.fontFamily == "Menlo"); // global default
    CHECK(r.themeName == "Demo Dark"); // global default
}

TEST(testTemporaryProfilesCannotPersist)
{
    Profile temporary("Temporary");
    temporary.scope = ProfileScope::Ephemeral;
    temporary.fontSize = 23;
    CHECK(serializeProfile(temporary).empty());
    const Values reopened = parseString("[[profiles]]\nname = \"Temporary\"\nscope = \"ephemeral\"\nfont_size = 23\n");
    CHECK(reopened.profiles.empty());
}

TEST(testSharingRequiresOptInAndExcludesLocalParents)
{
    Values device;
    device.fontFamily = "Private global font";
    Profile local("Local");
    local.fontSize = 17;
    local.fontFamily = "/private/secret/font.otf";
    device.profiles.push_back(local);
    Profile shared("Shared");
    shared.scope = ProfileScope::Synced;
    shared.parentProfile = "Local";
    shared.themeName = "Solarized";
    shared.startupHost = "private-host";
    shared.processExit = "restart";
    device.profiles.push_back(shared);
    Profile child("Shared Child");
    child.scope = ProfileScope::Synced;
    child.parentProfile = "Shared";
    child.fontSize = 14;
    device.profiles.push_back(child);
    auto exported = profilesForSharing(device);
    CHECK(exported.size() == 2);
    CHECK(exported[0].themeName == "Solarized");
    CHECK(!exported[0].fontFamily);
    CHECK(!exported[0].fontSize);
    CHECK(!exported[0].parentProfile);
    CHECK(!exported[0].startupHost);
    CHECK(!exported[0].processExit);
    CHECK(exported[1].themeName == "Solarized");
    CHECK(exported[1].fontSize == 14);
    device.profiles[1].scope = ProfileScope::Local;
    exported = profilesForSharing(device);
    CHECK(exported.size() == 1);
    CHECK(!exported[0].themeName);
}

TEST(testSessionProfilePrecedence)
{
    Values device;
    Profile shared("Shared");
    shared.fontSize = 10;
    Profile temporary;
    CHECK(resolvedSessionProfile(device, "", shared, temporary).fontSize == 10);
    device.fontSize = 11;
    CHECK(resolvedSessionProfile(device, "", shared, temporary).fontSize == 11);
    auto& platform = device.platformOverride(currentPlatform());
    platform.present = true;
    platform.profile.fontSize = 12;
    CHECK(resolvedSessionProfile(device, "", shared, temporary).fontSize == 12);
    Profile parent("Parent");
    parent.fontSize = 13;
    device.profiles.push_back(parent);
    Profile selected("Selected");
    selected.parentProfile = "Parent";
    device.profiles.push_back(selected);
    CHECK(resolvedSessionProfile(device, "Selected", shared, temporary).fontSize == 13);
    device.profiles.back().fontSize = 14;
    CHECK(resolvedSessionProfile(device, "Selected", shared, temporary).fontSize == 14);
    temporary.fontSize = 15;
    CHECK(resolvedSessionProfile(device, "Selected", shared, temporary).fontSize == 15);
    temporary.fontSize.reset();
    CHECK(resolvedSessionProfile(device, "Selected", shared, temporary).fontSize == 14);
}

TEST(testReviewedTransferBetweenIndependentDevices)
{
    Values source;
    Profile shared("Work");
    shared.scope = ProfileScope::Synced;
    shared.fontSize = 23;
    shared.startupHost = "secret-host";
    source.profiles.push_back(shared);
    const auto exports = profilesForSharing(source);
    REQUIRE(exports.size() == 1);
    const auto received = profileFromTransferText(serializeProfile(exports.front()));
    REQUIRE(received);
    CHECK(!received->startupHost);

    Values destination;
    Profile existing("Work");
    existing.fontSize = 12;
    destination.profiles.push_back(existing);
    CHECK(resolvedSessionProfile(destination, "Work", {}, {}).fontSize == 12);
    CHECK(resolvedSessionProfile(destination, "Work", {}, *received).fontSize == 23);
    CHECK(destination.profiles.front().fontSize == 12);
    const auto restarted = parseString(serializeProfile(destination.profiles.front()));
    REQUIRE(restarted.profiles.size() == 1);
    CHECK(resolvedSessionProfile(restarted, "Work", {}, {}).fontSize == 12);
    source.profiles.front().scope = ProfileScope::Local;
    CHECK(profilesForSharing(source).empty());
    CHECK(!profileFromTransferText(serializeProfile(source.profiles.front())));
}

TEST(testTransferRejectsAmbiguousAndOversizedDocuments)
{
    const std::string shared = "[[profiles]]\nname = \"Work\"\nscope = \"synced\"\nfont_size = 23\n";
    CHECK(profileFromTransferText(shared));
    CHECK(!profileFromTransferText(shared + shared));
    CHECK(!profileFromTransferText(std::string(65537, ' ')));
    CHECK(!profileFromTransferText(shared + std::string(1, '\0')));
    const auto sanitized = profileFromTransferText(shared + "font_family = \"/private/font.otf\"\nstartup_host = \"secret\"\n");
    REQUIRE(sanitized);
    CHECK(!sanitized->fontFamily);
    CHECK(!sanitized->startupHost);
}

UBIQUITTY_TEST_MAIN()

TEST(processExitChoiceInheritsAndRoundTrips)
{
    Ubiquitty::Preferences::Values configuration;
    std::istringstream input("[[profiles]]\nname = \"Parent\"\nprocess_exit = \"restart\"\n[[profiles]]\nname = \"Child\"\nparent = \"Parent\"\n");
    (void)parse(input, configuration);
    auto resolved = Ubiquitty::Preferences::resolvedProfile(configuration, "Child");
    CHECK(resolved.processExit.value_or("") == "restart");
    resolved.processExit = "keep_output";
    Ubiquitty::Preferences::Values roundTrip;
    std::istringstream serialized(Ubiquitty::Preferences::serializeProfile(resolved));
    (void)parse(serialized, roundTrip);
    REQUIRE(roundTrip.profiles.size() == 1);
    CHECK(roundTrip.profiles[0].processExit.value_or("") == "keep_output");
}
