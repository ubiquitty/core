// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_osc_string_bounds — Terminal's stored strings, cut through the sequences
// that set them.
//
// Seven caps sit on `Terminal`, and every one is reached from an escape sequence
// whose payload length the sender picks. Tests/Core/test_bounded_utf8.cpp calls
// the shared cut directly and pins what it does with a length; this file asks a
// different question, and it is the one a caller cares about: does the sequence
// that writes the field actually route through that cut, with that cap.
//
// The distinction is not academic. `udkMaxEntries` was crossed by the sync
// decoder and by nothing on the DECUDK path, and the two are separate copies of
// the same arithmetic — a cap reached by two paths is covered on neither until
// both are driven. Each case here goes in as bytes.
//
// Every payload is ASCII, so the byte cap and the character cap coincide and a
// short result is the cap rather than a multi-byte backoff. The UTF-8 walk has
// its own file.

#include "TerminalFixture.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <string>

using UbiquittyTest::feed;

namespace {

/// A terminal with a grid, ready to take sequences.
void reset(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
}

/// `count` copies of 'x' — a payload whose length is the only thing under test.
std::string filler(std::size_t count) { return std::string(count, 'x'); }

/// The base64 an OSC 1337 user-variable value is carried in.
std::string base64Of(const std::string& plain)
{
    static const char alphabet[]
        = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < plain.size(); i += 3) {
        const unsigned char a = static_cast<unsigned char>(plain[i]);
        const unsigned char b = (i + 1 < plain.size()) ? static_cast<unsigned char>(plain[i + 1]) : 0;
        const unsigned char c = (i + 2 < plain.size()) ? static_cast<unsigned char>(plain[i + 2]) : 0;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        out += (i + 1 < plain.size()) ? alphabet[(triple >> 6) & 0x3F] : '=';
        out += (i + 2 < plain.size()) ? alphabet[triple & 0x3F] : '=';
    }
    return out;
}

} // namespace

TEST(theThreeSequencesThatSetTheCurrentDirectoryDoNotShareOneLimit)
{
    // `maxCurrentDirectoryLength` is 4096 and two of the three writers reach it.
    // OSC 7 does not: `handleWorkingDirectory` in State.cpp URL-decodes into
    // `char path[256]` first, so that sequence stores at most 255 bytes whatever
    // the cap says.
    //
    // Pinned as measured rather than as declared, because the asymmetry is what
    // a caller meets. OSC 7 is the sequence shell integration actually emits, so
    // it is the path on which a long directory is lost, and the other two — which
    // no shell emits by default — are the ones that keep it.
    const std::size_t declared = Ubiquitty::Terminal::maxCurrentDirectoryLength;
    const std::size_t urlDecodeBuffer = 255;

    Ubiquitty::Terminal t;

    // A path inside every limit arrives whole on all three, so a difference below
    // is the cap and not the parse.
    for (const std::string& sequence : { std::string("\033]7;file://h/") + filler(10) + "\033\\",
             std::string("\033]1337;CurrentDir=/") + filler(10) + "\033\\",
             std::string("\033]633;P;Cwd=/") + filler(10) + "\033\\" }) {
        reset(t);
        feed(t, sequence);
        CHECK_EQ(t.currentDirectory.size(), std::size_t { 11 }); // the leading '/' plus ten
    }

    // Past the URL-decode buffer and well inside the declared cap: only OSC 7 cuts.
    reset(t);
    feed(t, "\033]7;file://h/" + filler(300) + "\033\\");
    CHECK_EQ(t.currentDirectory.size(), urlDecodeBuffer);

    reset(t);
    feed(t, "\033]1337;CurrentDir=/" + filler(300) + "\033\\");
    CHECK_EQ(t.currentDirectory.size(), std::size_t { 301 });

    reset(t);
    feed(t, "\033]633;P;Cwd=/" + filler(300) + "\033\\");
    CHECK_EQ(t.currentDirectory.size(), std::size_t { 301 });

    // Past the declared cap: the two that get there stop at it, and OSC 7 is
    // still held by its own buffer.
    reset(t);
    feed(t, "\033]1337;CurrentDir=/" + filler(declared * 2) + "\033\\");
    CHECK_EQ(t.currentDirectory.size(), declared);

    reset(t);
    feed(t, "\033]633;P;Cwd=/" + filler(declared * 2) + "\033\\");
    CHECK_EQ(t.currentDirectory.size(), declared);

    reset(t);
    feed(t, "\033]7;file://h/" + filler(declared * 2) + "\033\\");
    CHECK_EQ(t.currentDirectory.size(), urlDecodeBuffer);
}

TEST(osc7NeedsAFileURLAndStoresOnlyItsPath)
{
    // The host component goes; a bare path is not a file URL and stores nothing.
    // Both matter to the case above: a payload that never arrives and one cut to
    // 255 are different failures, and only the second is about a cap.
    Ubiquitty::Terminal t;

    reset(t);
    feed(t, "\033]7;/tmp/x\033\\");
    CHECK(t.currentDirectory.empty());

    reset(t);
    feed(t, "\033]7;file:///tmp/x\033\\");
    CHECK_STR_EQ(t.currentDirectory, "/tmp/x");

    reset(t);
    feed(t, "\033]7;file://host/tmp/x\033\\");
    CHECK_STR_EQ(t.currentDirectory, "/tmp/x");
}

TEST(osc50CutsTheFontAtItsLimit)
{
    Ubiquitty::Terminal t;
    reset(t);

    const std::size_t cap = Ubiquitty::Terminal::maxFontLength;
    feed(t, "\033]50;" + filler(cap) + "\033\\");
    CHECK_EQ(t.font.size(), cap);

    reset(t);
    feed(t, "\033]50;" + filler(cap + 1) + "\033\\");
    CHECK_EQ(t.font.size(), cap);
}

TEST(osc1337CutsTheRemoteHostAtItsLimit)
{
    Ubiquitty::Terminal t;
    reset(t);

    const std::size_t cap = Ubiquitty::Terminal::maxRemoteHostLength;
    feed(t, "\033]1337;RemoteHost=" + filler(cap) + "\033\\");
    CHECK_EQ(t.remoteHost.size(), cap);

    reset(t);
    feed(t, "\033]1337;RemoteHost=" + filler(cap + 1) + "\033\\");
    CHECK_EQ(t.remoteHost.size(), cap);
}

TEST(osc633CutsTheCommandLineAtItsLimit)
{
    Ubiquitty::Terminal t;
    reset(t);

    const std::size_t cap = Ubiquitty::Terminal::maxCommandLineLength;
    feed(t, "\033]633;E;" + filler(cap) + "\033\\");
    CHECK_EQ(t.commandLine.size(), cap);

    reset(t);
    feed(t, "\033]633;E;" + filler(cap + 1) + "\033\\");
    CHECK_EQ(t.commandLine.size(), cap);
}

TEST(osc1337CutsAUserVariableNameAndValueAtTheirOwnLimits)
{
    Ubiquitty::Terminal t;
    reset(t);

    const std::size_t nameLimit = Ubiquitty::Terminal::maxUserVarNameLength;
    const std::size_t valueLimit = Ubiquitty::Terminal::maxUserVarValueLength;

    // Two caps on one sequence, so each is crossed with the other well inside
    // its own: a single case crossing both cannot say which one cut.
    feed(t, "\033]1337;SetUserVar=" + filler(nameLimit + 10) + "=" + base64Of("short") + "\033\\");
    const Ubiquitty::Terminal::UserVariable* byName = nullptr;
    for (const auto& variable : t.userVariables) {
        if (variable.used)
            byName = &variable;
    }
    CHECK(byName != nullptr);
    CHECK_EQ(byName->name.size(), nameLimit);
    CHECK_EQ(byName->value.size(), std::size_t { 5 });

    reset(t);
    feed(t, "\033]1337;SetUserVar=name=" + base64Of(filler(valueLimit + 10)) + "\033\\");
    const Ubiquitty::Terminal::UserVariable* byValue = nullptr;
    for (const auto& variable : t.userVariables) {
        if (variable.used)
            byValue = &variable;
    }
    CHECK(byValue != nullptr);
    CHECK_EQ(byValue->name.size(), std::size_t { 4 });
    CHECK_EQ(byValue->value.size(), valueLimit);
}

TEST(osc1337DropsAUserVariablePastTheTableCapacity)
{
    Ubiquitty::Terminal t;
    reset(t);

    const int capacity = Ubiquitty::Terminal::userVariableCapacity;
    for (int i = 0; i < capacity; ++i)
        feed(t, "\033]1337;SetUserVar=v" + std::to_string(i) + "=" + base64Of("y") + "\033\\");

    int used = 0;
    for (const auto& variable : t.userVariables)
        used += variable.used ? 1 : 0;
    CHECK_EQ(used, capacity);

    // The table is full. A new name has nowhere to go and is dropped rather than
    // displacing one a session is already using.
    feed(t, "\033]1337;SetUserVar=overflow=" + base64Of("y") + "\033\\");
    used = 0;
    bool isOverflowStored = false;
    for (const auto& variable : t.userVariables) {
        used += variable.used ? 1 : 0;
        if (variable.used && variable.name == "overflow")
            isOverflowStored = true;
    }
    CHECK_EQ(used, capacity);
    CHECK(!isOverflowStored);

    // A name already in the table is rewritten rather than appended, so a full
    // table still accepts a change to what it holds.
    feed(t, "\033]1337;SetUserVar=v0=" + base64Of("zz") + "\033\\");
    for (const auto& variable : t.userVariables) {
        if (variable.used && variable.name == "v0")
            CHECK_EQ(variable.value.size(), std::size_t { 2 });
    }
}

UBIQUITTY_TEST_MAIN()
