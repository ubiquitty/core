// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_preferences_parse_differential — the config parser's accepted surface, pinned
// as text so a change of parser has to reproduce it rather than be believed.
//
// The goldens are what the parser produces, not what anyone thought it produced:
// parseQuotedString's escape switch handles \b, \f, \u and \U as well as
// \n \t \r \" \\, validates surrogates and out-of-range scalars, and rejects
// a malformed escape rather than storing the letters.
//
// The dump is canonical rather than pretty. Every list is printed in the order
// the parser stored it, because five of them — keys, link_handlers, triggers,
// characterClass, macros — are documented "in file order" and their consumers
// depend on it. A parser that sorted its tables would produce identical content
// in a different order, and only an order-preserving dump can tell the two apart.

#include "Harness.h"

#include "Core/Preferences/Preferences.h"

#include <cstdio>
#include <sstream>
#include <string>

namespace {

namespace Cfg = Ubiquitty::Preferences;

void appendString(std::string& out, const char* label, const std::string& value)
{
    out += label;
    out += "=";
    out += value;
    out += "\n";
}

// One Values rendered as text. Only the fields the corpus exercises: a dump of
// everything would fail on unrelated changes and teach people to re-bless it.
std::string dump(const Cfg::Values& c)
{
    std::string out;

    // Scalars, to show that plain [section] key = value still lands.
    if (c.fontFamily.has_value())
        appendString(out, "font.family", *c.fontFamily);
    if (c.fontSize.has_value())
        out += "font.size=" + std::to_string(static_cast<long long>(*c.fontSize * 100)) + "\n";
    if (c.themeName.has_value())
        appendString(out, "appearance.theme", *c.themeName);

    // The five order-sensitive lists, each printed with its index so a reorder
    // shows up as a diff rather than as the same set.
    for (std::size_t i = 0; i < c.keyBindings.size(); ++i)
        out += "keys[" + std::to_string(i) + "]=" + c.keyBindings[i].spec + " -> " + c.keyBindings[i].action + "\n";
    for (std::size_t i = 0; i < c.linkHandlers.size(); ++i)
        out += "link_handlers[" + std::to_string(i) + "]=" + c.linkHandlers[i].scheme + " -> " + c.linkHandlers[i].command + "\n";
    for (std::size_t i = 0; i < c.triggers.size(); ++i)
        out += "triggers[" + std::to_string(i) + "]=" + c.triggers[i].pattern + " -> " + c.triggers[i].action + "\n";
    for (std::size_t i = 0; i < c.characterClasses.size(); ++i)
        out += "characterClass[" + std::to_string(i) + "]=" + c.characterClasses[i].name + " -> " + c.characterClasses[i].pattern + "\n";
    for (std::size_t i = 0; i < c.macros.size(); ++i)
        out += "macros[" + std::to_string(i) + "]=" + c.macros[i].name + " -> " + c.macros[i].body + "\n";

    // Array-of-tables blocks, in declaration order.
    for (std::size_t i = 0; i < c.themes.size(); ++i) {
        out += "themes[" + std::to_string(i) + "].name=" + c.themes[i].name + "\n";
        int ansiSet = 0;
        for (const Cfg::ColorEntry& colour : c.themes[i].ansi) {
            if (colour.set)
                ++ansiSet;
        }
        if (ansiSet > 0)
            out += "themes[" + std::to_string(i) + "].ansi.count=" + std::to_string(ansiSet) + "\n";
    }
    for (std::size_t i = 0; i < c.profiles.size(); ++i) {
        out += "profiles[" + std::to_string(i) + "].name=" + c.profiles[i].name + "\n";
        if (c.profiles[i].fontFamily.has_value())
            out += "profiles[" + std::to_string(i) + "].font_family=" + c.profiles[i].fontFamily.value_or("") + "\n";
        if (c.profiles[i].startupHost.has_value())
            out += "profiles[" + std::to_string(i) + "].startup_host=" + c.profiles[i].startupHost.value_or("") + "\n";
    }

    // Platform overrides: present is the bit that distinguishes an empty block
    // from an absent one, so it is dumped even when nothing else is set.
    for (int p = 0; p < 2; ++p) {
        const Cfg::PlatformOverride& po = c.platformOverride(static_cast<Cfg::Platform>(p));
        if (!po.present)
            continue;
        out += "platform[" + std::to_string(p) + "].present=1\n";
        if (po.profile.fontFamily.has_value())
            out += "platform[" + std::to_string(p) + "].font_family=" + po.profile.fontFamily.value_or("") + "\n";
        for (std::size_t i = 0; i < po.keyBindings.size(); ++i)
            out += "platform[" + std::to_string(p) + "].keys[" + std::to_string(i) + "]=" + po.keyBindings[i].spec + " -> " + po.keyBindings[i].action + "\n";
    }

    return out;
}

std::string parseAndDump(const char* toml, bool* okOut = nullptr)
{
    std::istringstream in(toml);
    Cfg::Values c;
    const bool ok = Cfg::parse(in, c);
    if (okOut)
        *okOut = ok;
    return dump(c);
}

// One corpus entry: the input, and what the line parser made of it.
struct Case {
    const char* name;
    const char* input;
    const char* golden;
};

} // namespace

// Each case is a shape the parser accepts today. A replacement has to produce
// the same dump, byte for byte, or say in its commit why the difference is a fix.
TEST(testConfigParseSurfaceIsUnchanged)
{
    const Case cases[] = {
        { "scalars",
            "[font]\nfamily = \"JetBrains Mono\"\nsize = 13.5\n[appearance]\ntheme = \"Rose Pine\"\n",
            "font.family=JetBrains Mono\nfont.size=1350\nappearance.theme=Rose Pine\n" },

        // The keys are not in alphabetical order, which is what makes this case
        // able to fail: a parser iterating a sorted table produces
        // alpha/middle/zebra, and a test checking membership passes on both.
        { "triggers keep file order",
            "[triggers]\nzebra = \"notify:Z\"\nalpha = \"notify:A\"\nmiddle = \"notify:M\"\n",
            "triggers[0]=zebra -> notify:Z\ntriggers[1]=alpha -> notify:A\ntriggers[2]=middle -> notify:M\n" },

        { "keys, link_handlers, macros keep file order",
            "[keys]\n\"cmd+k\" = \"clear\"\n\"cmd+a\" = \"select_all\"\n"
            "[link_handlers]\nzed = \"open -a Zed %s\"\najira = \"open -a Jira %s\"\n"
            "[macros]\nzz = \"last\"\naa = \"first\"\n",
            "keys[0]=cmd+k -> clear\nkeys[1]=cmd+a -> select_all\n"
            "link_handlers[0]=zed -> open -a Zed %s\nlink_handlers[1]=ajira -> open -a Jira %s\n"
            "macros[0]=zz -> last\nmacros[1]=aa -> first\n" },

        { "array of tables",
            "[[themes]]\nname = \"One\"\n[[themes]]\nname = \"Two\"\n"
            "[[profiles]]\nname = \"Work\"\nfont_family = \"Menlo\"\n",
            "themes[0].name=One\nthemes[1].name=Two\nprofiles[0].name=Work\nprofiles[0].font_family=Menlo\n" },

        { "platform override and its keys",
            "[mac]\nfont_family = \"SF Mono\"\n[mac.keys]\n\"cmd+t\" = \"new_tab\"\n",
            "platform[0].present=1\nplatform[0].font_family=SF Mono\nplatform[0].keys[0]=cmd+t -> new_tab\n" },

        // A dotted header flattens to its leaf name, so a [x.font] table sets the
        // same fields a [font] table does — `parse` in Preferences.cpp keeps only
        // the segment after the last dot. Nested tables do not do this on their
        // own.
        { "dotted headers flatten to the leaf",
            "[deeply.nested.font]\nfamily = \"Menlo\"\n",
            "font.family=Menlo\n" },

        { "comments, CRLF and blank lines",
            "# a comment\r\n\r\n[font]\r\nfamily = \"Menlo\"  # trailing\r\n",
            "font.family=Menlo\n" },

        { "multi-line array folds",
            "[[themes]]\nname = \"Split\"\nansi = [\n  \"#000000\",\n  \"#ff0000\"\n]\n",
            "themes[0].name=Split\nthemes[0].ansi.count=2\n" },

        { "escapes",
            "[font]\nfamily = \"a\\tb\"\n",
            "font.family=a\tb\n" },
    };

    for (const Case& c : cases) {
        const std::string got = parseAndDump(c.input);
        CHECK_MSG(got == c.golden, c.name);
        if (got != c.golden)
            std::printf("  %s:\n    want |%s|\n    got  |%s|\n", c.name, c.golden, got.c_str());
    }
}

// \uXXXX decodes to its code point.
//
// The property is pinned whoever implements the parser: the surrounding switch
// also refuses a surrogate and an out-of-range scalar, and a replacement that
// accepted either would store a value no encoder can emit.
TEST(testUnicodeEscapeDecodesToItsCodePoint)
{
    const std::string got = parseAndDump("[font]\nfamily = \"caf\\u00e9\"\n");
    CHECK_MSG(got == "font.family=caf\xc3\xa9\n", "\\u00e9 decodes to U+00E9, not to the text u00e9");
    if (got != "font.family=caf\xc3\xa9\n")
        std::printf("  got |%s|\n", got.c_str());
}

UBIQUITTY_TEST_MAIN()
