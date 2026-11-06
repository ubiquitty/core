// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Core/Input/test_shellescape.cpp — pins Ubiquitty::Input::shellEscapedPath,
// the drag-drop path-escaping rule shared by the macOS and Linux GTK
// file-drop frontends.
//
// Every byte outside the shell-unreserved set [A-Za-z0-9_./-] gains a leading
// backslash; multibyte UTF-8 (>= 0x80) passes through verbatim. Header-only
// (pure std::string over UTF-8 bytes), so it links standalone.

#include "Core/Input/ShellEscape.h"

#include <span>

#include <cstdio>
#include <string>

using Ubiquitty::Input::shellEscapedPath;
static void expectEscaped(const std::string& input, const std::string& want, const char* label)
{
    const std::string got = shellEscapedPath(input);
    if (got != want) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), R"(%s: shellEscapedPath("%s") = "%s", want "%s")", label, input.c_str(), got.c_str(), want.c_str());
        CHECK_MSG(false, ubqMessage);
    }
}

TEST(shellescape)
{

    // Unreserved bytes pass through untouched.
    expectEscaped("/usr/local/bin", "/usr/local/bin", "plain absolute path");
    expectEscaped("file.txt", "file.txt", "dot is unreserved");
    expectEscaped("my-file_v2.tar.gz", "my-file_v2.tar.gz", "dash/underscore/dot unreserved");
    expectEscaped("ABCxyz0189", "ABCxyz0189", "alphanumerics unreserved");
    expectEscaped("", "", "empty input");

    // Spaces and shell metacharacters are backslash-escaped.
    expectEscaped("/tmp/My Documents", "/tmp/My\\ Documents", "space escaped");
    expectEscaped("a b c", "a\\ b\\ c", "every space escaped");
    expectEscaped("file(1).txt", "file\\(1\\).txt", "parens escaped");
    expectEscaped("$HOME", "\\$HOME", "dollar escaped");
    expectEscaped("a&b|c;d", R"(a\&b\|c\;d)", "ampersand/pipe/semicolon escaped");
    expectEscaped("a'b\"c", R"(a\'b\"c)", "quotes escaped");
    expectEscaped("a*b?c[d]", R"(a\*b\?c\[d\])", "glob metacharacters escaped");
    expectEscaped("back\\slash", "back\\\\slash", "literal backslash escaped");
    expectEscaped("tab\there", "tab\\\there", "tab escaped");
    expectEscaped("a\nb", "a\\\nb", "newline escaped");

    // Multibyte UTF-8 (bytes >= 0x80) is an ordinary word character: never
    // escaped, passes through byte-for-byte. "café/dökument" — the é and ö are
    // two-byte UTF-8 sequences.
    // The account name is the neutral placeholder on purpose: a real one here is
    // a personal path in a public tree, and check-personal-paths.py would need a
    // standing exemption for it that a real person of that name would then pass.
    expectEscaped("/Users/user/d\xC3\xB6kument/caf\xC3\xA9.txt", "/Users/user/d\xC3\xB6kument/caf\xC3\xA9.txt", "UTF-8 multibyte passes through");
    expectEscaped("\xE6\x97\xA5\xE6\x9C\xAC", "\xE6\x97\xA5\xE6\x9C\xAC", "CJK multibyte passes through");
    // A space adjacent to multibyte text still escapes only the space.
    expectEscaped("caf\xC3\xA9 menu", "caf\xC3\xA9\\ menu", "space among multibyte escaped");
}

// What a drop of several files inserts at a prompt.
TEST(shellEscapedDropTextReadsAsArguments)
{
    using Ubiquitty::Input::shellEscapedDropText;

    // Space-separated with a trailing space, so the next thing typed does not
    // run into the last path.
    const std::string two[] = { "/tmp/a b", "/tmp/c" };
    CHECK(shellEscapedDropText(two) == "/tmp/a\\ b /tmp/c ");

    // One path still gets its trailing space, for the same reason.
    const std::string one[] = { "/tmp/x" };
    CHECK(shellEscapedDropText(one) == "/tmp/x ");

    // An empty path contributes nothing rather than a bare separator, which
    // would land as an argument the reader never dropped.
    const std::string withEmpty[] = { "", "/tmp/y", "" };
    CHECK(shellEscapedDropText(withEmpty) == "/tmp/y ");

    // Nothing to insert is nothing, not a lone space: a caller reads the empty
    // result as a drop that inserted nothing. Spelled through the span, since a
    // bare {} names neither overload.
    const std::span<const std::string> nothing;
    CHECK(shellEscapedDropText(nothing).empty());
    const std::string allEmpty[] = { "", "" };
    CHECK(shellEscapedDropText(allEmpty).empty());
}

UBIQUITTY_TEST_MAIN()
