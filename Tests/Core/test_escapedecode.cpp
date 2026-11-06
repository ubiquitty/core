// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_escapedecode.cpp — pins the C-string escape decoder in
// Source/Core/Diagnostics/EscapeDecode.h (the --feed diagnostic fallback) against the exact
// behaviour the macOS snapshot command's own copy shipped, so routing the frontend
// through it can't change a single decoded byte. Header-only; bare libc++.

#include "Core/Diagnostics/EscapeDecode.h"

#include <cstdio>
#include <string>

using Ubiquitty::Diagnostics::decodeEscapes;

namespace {
// Compare against an explicit expected std::string (handles embedded NULs).
void checkEqual(const std::string& got, const std::string& want, const char* what)
{
    if (got != want) {
        char ubqMessage[512];
        std::snprintf(ubqMessage, sizeof(ubqMessage), "%s — got %zu bytes, want %zu bytes", what, got.size(), want.size());
        CHECK_MSG(false, ubqMessage);
    }
}

} // namespace

TEST(escapedecode)
{

    // No escapes: verbatim pass-through.
    checkEqual(decodeEscapes("hello world"), "hello world", "plain text");
    checkEqual(decodeEscapes(""), "", "empty");

    // Each single-char escape.
    checkEqual(decodeEscapes("a\\nb"), std::string("a\nb"), "\\n");
    checkEqual(decodeEscapes("\\r"), std::string("\r"), "\\r");
    checkEqual(decodeEscapes("\\t"), std::string("\t"), "\\t");
    checkEqual(decodeEscapes("\\e"), std::string("\x1b"), "\\e -> ESC");
    checkEqual(decodeEscapes("\\a"), std::string("\a"), "\\a");
    checkEqual(decodeEscapes("\\b"), std::string("\b"), "\\b");
    checkEqual(decodeEscapes("\\f"), std::string("\f"), "\\f");
    checkEqual(decodeEscapes("\\v"), std::string("\v"), "\\v");
    checkEqual(decodeEscapes("\\\\"), std::string("\\"), "\\\\ -> backslash");

    // \0 produces an embedded NUL byte (string_view-driven, so the rest still decodes).
    {
        std::string want;
        want.push_back('x');
        want.push_back('\0');
        want.push_back('y');
        checkEqual(decodeEscapes("x\\0y"), want, "\\0 embedded NUL");
    }

    // A realistic OSC 133 prompt feed mixing ESC and CR/LF.
    checkEqual(decodeEscapes(R"(\e]133;A\a$ ls\r\n)"), std::string("\x1b]133;A\a$ ls\r\n"), "OSC 133 prompt feed");

    // \xHH — two hex digits, both cases.
    checkEqual(decodeEscapes("\\x1b"), std::string("\x1b"), "\\x1b two digits");
    checkEqual(decodeEscapes("\\x1B"), std::string("\x1b"), "\\x1B uppercase");
    checkEqual(decodeEscapes("\\xff"), std::string("\xff"), "\\xff high byte");

    // \xH — a single hex digit (the next char is not hex).
    checkEqual(decodeEscapes("\\x9z"), std::string("\x09z"), "\\x9 single digit then 'z'");
    checkEqual(decodeEscapes("\\x7"), std::string("\x07"), "\\x7 single digit at end");

    // \x with no hex digit: backslash kept, 'x' emitted verbatim (shipped behaviour).
    checkEqual(decodeEscapes("\\xz"), std::string("\\xz"), "\\x no-hex keeps backslash+x");
    checkEqual(decodeEscapes("\\x"), std::string("\\x"), "\\x at end keeps backslash+x");

    // Unknown escape: backslash kept, escaped char emitted verbatim.
    checkEqual(decodeEscapes("\\q"), std::string("\\q"), "unknown \\q verbatim");
    checkEqual(decodeEscapes("\\."), std::string("\\."), "unknown \\. verbatim");

    // Lone trailing backslash: emitted literally.
    checkEqual(decodeEscapes("end\\"), std::string("end\\"), "trailing backslash literal");

    // Backslash-digit other than 0 is unknown (only \0 is special); kept verbatim.
    checkEqual(decodeEscapes("\\1"), std::string("\\1"), "\\1 not an octal escape");
}

UBIQUITTY_TEST_MAIN()
