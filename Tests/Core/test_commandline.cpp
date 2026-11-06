// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_commandline — splitting a configured command string into argv.

#include "Harness.h"

#include "Core/CommandLine.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace {

using Ubiquitty::commandProgramName;
using Ubiquitty::splitCommandWords;

TEST(testSplitsOnWhitespaceRuns)
{
    const auto words = splitCommandWords("ssh   user@host   -p  22");
    CHECK_MSG(words.size() == 4, "runs of whitespace collapse");
    CHECK_MSG(words[0] == "ssh", "the program is first");
    CHECK_MSG(words[1] == "user@host", "arguments follow in order");
    CHECK_MSG(words[3] == "22", "and the last one survives");
}

TEST(testEveryWhitespaceKindSplits)
{
    const auto words = splitCommandWords("a\tb\nc\vd\fe\rf g");
    CHECK_MSG(words.size() == 7, "tab, newline, vertical tab, form feed and CR all split");
}

TEST(testLeadingAndTrailingWhitespaceYieldNoEmptyWords)
{
    const auto words = splitCommandWords("  \t spaced out \n ");
    CHECK_MSG(words.size() == 2, "surrounding whitespace produces no empty elements");
    CHECK_MSG(words[0] == "spaced" && words[1] == "out", "and the words are intact");
}

TEST(testEmptyInput)
{
    CHECK_MSG(splitCommandWords("").empty(), "an empty command has no words");
    CHECK_MSG(splitCommandWords("   \t\n ").empty(), "an all-whitespace command has no words");
}

TEST(testSingleWord)
{
    const auto words = splitCommandWords("bash");
    CHECK_MSG(words.size() == 1 && words[0] == "bash", "a lone program is one word");
}

// Quoting is not honoured: the result is what execve receives, and a quote is
// an ordinary byte in an argument.
TEST(testQuotesAreNotSyntax)
{
    const auto words = splitCommandWords("echo \"two words\"");
    CHECK_MSG(words.size() == 3, "a quoted phrase still splits on its space");
    CHECK_MSG(words[1] == "\"two", "the quote stays part of the word");
    CHECK_MSG(words[2] == "words\"", "on both sides");
}

// Shell metacharacters are ordinary bytes here: there is no shell on this path,
// so a word containing one is a single argument, not an operator.
TEST(testMetacharactersAreOrdinaryBytes)
{
    const auto words = splitCommandWords("run 'a;b' $(id) `x` a|b");
    CHECK_MSG(words.size() == 5, "each stays one word");
    CHECK_MSG(words[1] == "'a;b'", "a semicolon inside a word is data");
    CHECK_MSG(words[2] == "$(id)", "so is a command substitution");
    CHECK_MSG(words[3] == "`x`", "so are backticks");
    CHECK_MSG(words[4] == "a|b", "so is a pipe");
}

TEST(testProgramName)
{
    CHECK_MSG(commandProgramName("ssh user@host") == "ssh", "the first word names the program");
    CHECK_MSG(commandProgramName("  \t bash -l") == "bash", "leading whitespace is skipped");
    CHECK_MSG(commandProgramName("solo") == "solo", "a lone word is the program");
    CHECK_MSG(commandProgramName("").empty(), "an empty command names nothing");
    CHECK_MSG(commandProgramName("   ").empty(), "nor does an all-whitespace one");
}

TEST(testProgramNameMatchesTheFirstSplitWord)
{
    for (const char* command : { "", " ", "one", "one two", "\tlead  mid\ttail\n", "a|b c" }) {
        const auto words = splitCommandWords(command);
        const std::string expected = words.empty() ? std::string {} : words.front();
        CHECK_MSG(commandProgramName(command) == expected,
            "commandProgramName is the first word splitCommandWords yields");
    }
}

// `-e <program> [args…]`, the convention xterm set. A frontend that ignores it
// opens an ordinary login-shell window, which is indistinguishable from having
// run the command — so every case here asserts what reached `argv`, and the two
// that matter most are the ones where a plausible parser silently takes the
// wrong bytes.

// argv as `main` receives it: NUL-terminated strings, count separate.
struct Arguments {
    std::vector<const char*> storage;
    explicit Arguments(std::initializer_list<const char*> words)
        : storage(words)
    {
    }
    [[nodiscard]] int count() const { return static_cast<int>(storage.size()); }
    [[nodiscard]] const char* const* values() const { return storage.data(); }
};
} // namespace

UBIQUITTY_TEST_MAIN()
