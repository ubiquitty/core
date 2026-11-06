// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_preferences_template_parses.cpp — the shipped default config is valid input.
//
// `defaultFileContents()` writes the TOML the app drops on disk for a new user,
// and calls itself the reference for what can be set. Nothing parsed it. The
// macOS application delegate writes it out and the file
// then goes to a human, so a typo in a key name or a value reaches the reader
// as a line that silently does nothing.
//
// Parsing it as shipped proves almost nothing, and that is the trap here: every
// key line is commented out, so the document is inert and a file of pure
// nonsense behind `#` passes. The real check UNCOMMENTS each documented key and
// parses it under its own section, which is what a reader does when they enable
// one.
//
// Strict mode, because Permissive is the mode that forgives exactly the mistake
// this looks for.

#include "Core/Preferences/Preferences.h"

#include <sstream>
#include <string>
#include <vector>

using Ubiquitty::Preferences::parse;
using Ubiquitty::Preferences::ParseMode;
using Ubiquitty::Preferences::Values;

namespace {

struct Documented {
    std::string section;
    bool isArrayOfTables = false;
    std::string key;
    std::string value;
    int line = 0;
    // Which header the key sits under, counted in template order, so the keys
    // of one array-of-tables entry can be enabled together.
    int entry = 0;
};

// `# beam_color = "#33ff66"   # custom-mode beam color`
//
// The trailing `#` comment is kept: it is part of the line a reader uncomments,
// so dropping it here would test something the reader never types.
bool splitAssignment(const std::string& text, std::string& key, std::string& value)
{
    std::string body = text;
    if (body.starts_with("# "))
        body = body.substr(2);
    else if (body.starts_with("#"))
        body = body.substr(1);
    else
        return false;

    const std::string::size_type equals = body.find('=');
    if (equals == std::string::npos)
        return false;

    key = body.substr(0, equals);
    while (!key.empty() && key.back() == ' ')
        key.pop_back();
    if (key.empty())
        return false;
    for (const char c : key) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false; // prose, a URL, an example regex — not a key line
    }

    value = body.substr(equals + 1);
    while (!value.empty() && value.front() == ' ')
        value.erase(value.begin());
    if (value.empty())
        return false;
    // `ansi = [` opens an array that closes several lines later. One line is
    // not a document, so these are skipped rather than fed in half — the count
    // floor below is what keeps that from quietly emptying the set.
    return value.back() != '[';
}

// A section header, live `[selection]`, commented `# [scripting]`, or an
// array-of-tables `# [[profiles]]`. Dots are part of a name: the template
// carries `[[font.language_fallbacks]]` and `[[font.range_map]]`.
bool sectionName(const std::string& text, std::string& name, bool& isArrayOfTables)
{
    std::string body = text;
    if (body.starts_with("# "))
        body = body.substr(2);
    if (body.empty() || body.front() != '[')
        return false;
    isArrayOfTables = body.size() > 1 && body[1] == '[';
    const std::string::size_type start = isArrayOfTables ? 2 : 1;
    const std::string::size_type close = body.find(']', start);
    if (close == std::string::npos)
        return false;
    name = body.substr(start, close - start);
    if (name.empty())
        return false;
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || c == '_' || c == '.'))
            return false;
    }
    // A header is the whole line. "a [mac] or [linux] block whose" is prose.
    const std::string::size_type after = close + (isArrayOfTables ? 2 : 1);
    return after >= body.size() || body[after] == '\n';
}

std::vector<Documented> documentedKeys()
{
    std::vector<Documented> out;
    std::istringstream in(Ubiquitty::Preferences::defaultFileContents());
    std::string line;
    std::string section;
    bool isArraySection = false;
    int number = 0;
    int entry = 0;
    while (std::getline(in, line)) {
        ++number;
        std::string name;
        bool isArray = false;
        if (sectionName(line, name, isArray)) {
            section = name;
            isArraySection = isArray;
            ++entry;
            continue;
        }
        std::string key;
        std::string value;
        if (!section.empty() && splitAssignment(line, key, value))
            out.push_back({ section, isArraySection, key, value, number, entry });
    }
    return out;
}

bool parses(const std::string& document)
{
    Values c;
    std::istringstream in(document);
    return parse(in, c, ParseMode::Strict);
}

// One key under its header. An array-of-tables entry is a rule whose fields
// are read together — a range map without its mode is not a rule — so its key
// is enabled with the rest of the entry, the way a reader uncomments it.
std::string enable(const Documented& d, const std::vector<Documented>& keys)
{
    if (!d.isArrayOfTables)
        return "[" + d.section + "]\n" + d.key + " = " + d.value + "\n";
    std::string document = "[[" + d.section + "]]\n";
    for (const Documented& sibling : keys) {
        if (sibling.entry == d.entry)
            document += sibling.key + " = " + sibling.value + "\n";
    }
    return document;
}

} // namespace

TEST(testTemplateShipsAsValidTOML)
{
    CHECK(parses(Ubiquitty::Preferences::defaultFileContents()));
}

TEST(testEveryDocumentedKeyIsAcceptedUnderItsSection)
{
    const std::vector<Documented> keys = documentedKeys();

    // A scan that finds nothing would pass every assertion below it. The
    // template documents most of what Key.h persists, so this floor is
    // far under the real count and still catches a parser that stops matching.
    CHECK(keys.size() > 50);

    for (const Documented& d : keys) {
        // The line and its text, because a bare failure sends the reader hunting
        // through a template that is mostly comments.
        const std::string where = "template line " + std::to_string(d.line) + ": ["
            + d.section + "] " + d.key + " = " + d.value;
        CHECK_MSG(parses(enable(d, keys)), where.c_str());
    }
}

TEST(testStrictModeRejectsWhatThisWouldMiss)
{
    // The control. Without it a Strict mode that accepted anything would make
    // the test above vacuous, and the whole file would pass on a template full
    // of invented keys.
    CHECK(!parses("[quake]\nheihgt = 0.5\n")); // key misspelled
    CHECK(!parses("[qauke]\nheight = 0.5\n")); // section misspelled
    CHECK(!parses("[tek]\nbright_pen = \"yes\"\n")); // bool given a string

    // And the same lines spelled right, so the rejections above are about the
    // mistake rather than about Strict refusing everything.
    CHECK(parses("[quake]\nheight = 0.5\n"));
    CHECK(parses("[tek]\nbright_pen = true\n"));
}

TEST(testTheUpdateScheduleIsDocumentedAtItsDefault)
{
    // The template is where a reader learns the app checks for updates on its
    // own and how to stop it, so the line has to exist once, under [updates],
    // and show the value the updater follows while it stays commented.
    int found = 0;
    for (const Documented& d : documentedKeys()) {
        if (d.section != "updates" || d.key != "check_automatically")
            continue;
        ++found;
        const std::string where = "template line " + std::to_string(d.line) + ": " + d.value;
        CHECK_MSG(d.value == "true" || d.value.starts_with("true "), where.c_str());
    }
    CHECK_EQ(found, 1);
}

UBIQUITTY_TEST_MAIN()
