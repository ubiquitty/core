// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Preferences/Configuration.h"
#include "Core/Preferences/Preferences.h"

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

/// Runs `body` with stderr captured, and returns what it wrote.
std::string stderrDuring(const std::function<void()>& body)
{
    char pattern[] = "/tmp/ubiquitty-stderr-XXXXXX";
    const int descriptor = ::mkstemp(pattern);
    if (descriptor >= 0)
        ::close(descriptor);
    const std::string temporaryPath = pattern;
    std::fflush(stderr);
    const int saved = dup(fileno(stderr));
    FILE* redirected = std::freopen(temporaryPath.c_str(), "w", stderr);
    body();
    std::fflush(stderr);
    if (redirected)
        (void)redirected;
    dup2(saved, fileno(stderr));
    close(saved);

    std::string captured;
    if (FILE* f = std::fopen(temporaryPath.c_str(), "r")) {
        char chunk[512];
        std::size_t n = 0;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
            captured.append(chunk, n);
        std::fclose(f);
    }
    std::remove(temporaryPath.c_str());
    return captured;
}

std::string writeConfig(const char* body)
{
    char pattern[] = "/tmp/ubiquitty-skipped-config-XXXXXX";
    const int descriptor = ::mkstemp(pattern);
    if (descriptor >= 0)
        ::close(descriptor);
    const std::string path = pattern;
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fputs(body, f);
        std::fclose(f);
    }
    return path;
}

}

/// A key the parse could not place is named, with its line, so the user has a
/// place to look. Permissive parsing keeps the rest of the file, which is why
/// nothing else would say anything.
TEST(testASkippedKeyIsNamedOnStderr)
{
    const std::string path = writeConfig("[cursor]\nshpae = \"bar\"\nblink = true\n");
    Ubiquitty::Preferences::ConfigureResult configured;
    const std::string said = stderrDuring([&] {
        configured = Ubiquitty::Preferences::configureFromFile(path, Ubiquitty::Preferences::ParseMode::Permissive);
    });

    CHECK(configured.isApplied); // permissive: the file still applies
    CHECK(Ubiquitty::Preferences::values().cursorBlink.has_value()); // and the keys that landed still landed
    // The fixture, asserted before the conclusion: the parse did skip one.
    CHECK_EQ(static_cast<int>(configured.diagnostics.size()), 1);

    CHECK(said.find("shpae") != std::string::npos);
    CHECK(said.find("cursor.shpae") != std::string::npos); // section-qualified
    CHECK(said.find(":2:") != std::string::npos); // the line to open
    CHECK(said.find(path) != std::string::npos); // the file to open
    std::remove(path.c_str());
}

/// A file whose every key landed says nothing. A notice on a clean load would
/// train the reader to ignore the channel.
TEST(testACleanConfigurationIsSilent)
{
    const std::string path = writeConfig("[cursor]\nshape = \"bar\"\nblink = true\n");
    Ubiquitty::Preferences::ConfigureResult configured;
    const std::string said = stderrDuring([&] {
        configured = Ubiquitty::Preferences::configureFromFile(path, Ubiquitty::Preferences::ParseMode::Permissive);
    });

    CHECK(configured.diagnostics.empty());
    CHECK(said.empty());
    std::remove(path.c_str());
}

/// Parsing a string says nothing: there is no file for the user to open, so a
/// line naming one would point at nothing.
TEST(testParsingAStringIsSilent)
{
    std::istringstream in("[cursor]\nshpae = \"bar\"\n");
    Ubiquitty::Preferences::Values configuration;
    const std::string said = stderrDuring([&] {
        (void)Ubiquitty::Preferences::parse(in, configuration);
    });

    CHECK_EQ(static_cast<int>(configuration.skippedKeys.size()), 1); // still recorded
    CHECK(said.empty()); // but not announced
}

/// A key that exists but was handed the wrong kind of value says so, and names
/// what it takes. Reporting it as an unknown key sends the reader looking for a
/// typo in a name that is spelled correctly.
TEST(testAKnownKeyGivenTheWrongKindNamesWhatItTakes)
{
    // [font] family is a string key; an integer misses the string table and
    // every other table too, which is exactly what an unknown key does.
    const std::string path = writeConfig("[font]\nfamily = 12\n");
    Ubiquitty::Preferences::ConfigureResult configured;
    const std::string said = stderrDuring([&] {
        configured = Ubiquitty::Preferences::configureFromFile(path, Ubiquitty::Preferences::ParseMode::Permissive);
    });

    CHECK_EQ(static_cast<int>(configured.diagnostics.size()), 1);
    if (configured.diagnostics.size() == 1) {
        CHECK(configured.diagnostics[0].reason == Ubiquitty::Preferences::SkipReason::WrongValueKind);
        CHECK(configured.diagnostics[0].expectedValueKind == "a string");
    }
    CHECK(said.find("takes a string") != std::string::npos);
    CHECK(said.find("no setting named") == std::string::npos); // not the other sentence
    std::remove(path.c_str());
}

/// A key the special-cased assignment owns has no table describing its shape,
/// so the notice says less rather than guessing a kind.
TEST(testASpecialKeyGivenTheWrongKindSaysSoWithoutNamingAKind)
{
    // appearance.system_light_theme is keyed by theme NAME; an index is the
    // mistake a user makes when the comment beside it mentions one.
    const std::string path = writeConfig("[appearance]\nsystem_light_theme = 1\n");
    Ubiquitty::Preferences::ConfigureResult configured;
    const std::string said = stderrDuring([&] {
        configured = Ubiquitty::Preferences::configureFromFile(path, Ubiquitty::Preferences::ParseMode::Permissive);
    });

    CHECK_EQ(static_cast<int>(configured.diagnostics.size()), 1);
    if (configured.diagnostics.size() == 1) {
        CHECK(configured.diagnostics[0].reason == Ubiquitty::Preferences::SkipReason::WrongValueKind);
        CHECK(configured.diagnostics[0].expectedValueKind.empty());
    }
    CHECK(said.find("does not take this value") != std::string::npos);
    CHECK(said.find("no setting named") == std::string::npos);
    std::remove(path.c_str());
}

/// And a genuine typo still reads as one.
TEST(testAnUnknownKeyStillReadsAsAName)
{
    const std::string path = writeConfig("[appearance]\ncorner_radius_top = 0\n");
    Ubiquitty::Preferences::ConfigureResult configured;
    const std::string said = stderrDuring([&] {
        configured = Ubiquitty::Preferences::configureFromFile(path, Ubiquitty::Preferences::ParseMode::Permissive);
    });

    CHECK_EQ(static_cast<int>(configured.diagnostics.size()), 1);
    if (configured.diagnostics.size() == 1)
        CHECK(configured.diagnostics[0].reason == Ubiquitty::Preferences::SkipReason::UnknownKey);
    CHECK(said.find("no setting named appearance.corner_radius_top") != std::string::npos);
    std::remove(path.c_str());
}

UBIQUITTY_TEST_MAIN()
