// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tests/Assertions.h — the assertion vocabulary, and the counter behind it.
//
// Split out of Harness.h so a test that owns its own main() can share it. Those
// exist for a reason: a test whose subject is process startup, a signal, or an
// argv contract has to control main itself, and fifteen of them wrote their own
// CHECK because the harness offered assertions only bundled with a main they
// could not take.
//
// A private CHECK is not a smaller harness — it is a second one, free to drift,
// and drift is how twenty tests came to print FAIL and exit 0. One vocabulary,
// two entry points:
//
//     #include "Harness.h"       TEST(name) cases, UBIQUITTY_TEST_MAIN()
//     #include "Assertions.h"    your own main(), return UBIQUITTY_TEST_SUMMARY()
//
// `failures` is file-scope static because a test binary is one translation
// unit. A hand-written main returns UBIQUITTY_TEST_SUMMARY(), which is the same
// counter the CHECK macros feed — so an assertion cannot fail silently whichever
// entry point the file uses.
//

// The assertions are the thing under test, and NDEBUG deletes every one of them.
// It also changes sizeof(CellGrid) — CellGrid declares `m_paintInvariantBroken`
// inside #ifndef NDEBUG — so a test TU built with it disagrees with Core about the layout
// of the type it is checking. That is not a slower or weaker run; it is a
// different program, and it links.
//
// Here rather than in Harness.h because the twelve tests that own their main
// include this directly, and the other 235 reach it through Harness.h, which
// includes it.
#ifdef NDEBUG
#error "the test suite must not be built with NDEBUG: it deletes assert() and changes sizeof(CellGrid)"
#endif

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>

// Failure counter. Each test binary is a single translation unit, so file-scope
// static is correct and keeps the header self-contained.
static int failures = 0;

#define UBIQUITTY_TEST_FAIL_(fmt, ...)                                   \
    do {                                                                 \
        std::printf("%s:%d: test-harness: assertion failed — " fmt "\n", \
            __FILE__, __LINE__, __VA_ARGS__);                            \
        failures++;                                                      \
    } while (0)

// Truth assertion. Prints the expression, which is all there is to print.
#define CHECK(expr)                            \
    do {                                       \
        if (!(expr))                           \
            UBIQUITTY_TEST_FAIL_("%s", #expr); \
    } while (0)

// A truth assertion that STOPS the case, for a fact the lines below it depend on.
//
// Every CHECK above records and carries on, which is right when the assertions
// are independent and wrong when they are not: `CHECK(v.size() == 3)` followed by
// `v[2]` reads out of bounds once the size check fails, and the run reports a
// FATAL signal naming neither the assertion nor the cause. The failure that gets
// printed is then a crash in a case whose real defect was three lines earlier.
//
// Use it for a precondition — a size before an index, a pointer before a
// dereference, a status before reading a payload — and leave CHECK for the
// independent facts that follow.
//
// A case is a void function (see TEST in Harness.h), so this returns from it.
#define REQUIRE(expr)                          \
    do {                                       \
        if (!(expr)) {                         \
            UBIQUITTY_TEST_FAIL_("%s", #expr); \
            return;                            \
        }                                      \
    } while (0)

/// As REQUIRE, with a sentence saying what the precondition means.
#define REQUIRE_MSG(cond, msg)                 \
    do {                                       \
        if (!(cond)) {                         \
            UBIQUITTY_TEST_FAIL_("%s", (msg)); \
            return;                            \
        }                                      \
    } while (0)

// Truth assertion carrying a human message instead of the expression. Use it
// when the expression is opaque — a decoded pixel, a parsed field — and the
// message names the property being asserted. FILE:LINE rather than __func__,
// so the failure is clickable like every other one here.
#define CHECK_MSG(cond, msg)                   \
    do {                                       \
        if (!(cond))                           \
            UBIQUITTY_TEST_FAIL_("%s", (msg)); \
    } while (0)

// Equality on anything printable as a long long (ints, enums, chars, pointers
// via CHECK_PTR_EQ). Both operands are evaluated exactly once.
#define CHECK_EQ(actual, expected)                                 \
    do {                                                           \
        const auto ubqActual_ = (actual);                          \
        const auto ubqExpected_ = (expected);                      \
        if (!(ubqActual_ == ubqExpected_))                         \
            UBIQUITTY_TEST_FAIL_("%s == %s — got %lld, want %lld", \
                #actual, #expected,                                \
                static_cast<long long>(ubqActual_),                \
                static_cast<long long>(ubqExpected_));             \
    } while (0)

#define CHECK_NE(actual, unexpected)                     \
    do {                                                 \
        const auto ubqActual_ = (actual);                \
        const auto ubqUnexpected_ = (unexpected);        \
        if (!(ubqActual_ != ubqUnexpected_))             \
            UBIQUITTY_TEST_FAIL_("%s != %s — both %lld", \
                #actual, #unexpected,                    \
                static_cast<long long>(ubqActual_));     \
    } while (0)

// NUL-terminated string equality. Null-safe on both sides.
#define CHECK_CSTR_EQ(actual, expected)                                                                                                                        \
    do {                                                                                                                                                       \
        const char* ubqActual_ = (actual);                                                                                                                     \
        const char* ubqExpected_ = (expected);                                                                                                                 \
        const bool ubqIsSame_ = (ubqActual_ == nullptr && ubqExpected_ == nullptr) /* bool-ok: the ubq prefix keeps a macro local from shadowing the caller */ \
            || (ubqActual_ != nullptr && ubqExpected_ != nullptr                                                                                               \
                && std::strcmp(ubqActual_, ubqExpected_) == 0);                                                                                                \
        if (!ubqIsSame_)                                                                                                                                       \
            UBIQUITTY_TEST_FAIL_("%s == %s — got \"%s\", want \"%s\"",                                                                                         \
                #actual, #expected,                                                                                                                            \
                ubqActual_ ? ubqActual_ : "(null)",                                                                                                            \
                ubqExpected_ ? ubqExpected_ : "(null)");                                                                                                       \
    } while (0)

#define CHECK_PTR_EQ(actual, expected)                         \
    do {                                                       \
        const void* ubqActual_ = (actual);                     \
        const void* ubqExpected_ = (expected);                 \
        if (ubqActual_ != ubqExpected_)                        \
            UBIQUITTY_TEST_FAIL_("%s == %s — got %p, want %p", \
                #actual, #expected, ubqActual_, ubqExpected_); \
    } while (0)

// Equality on anything convertible to std::string_view — std::string, a
// string_view, a char array. CHECK_CSTR_EQ takes `const char*` and cannot
// compare a std::string without a .c_str() at every call site; four files had
// written their own CHECK_EQ_STR for that reason alone.
//
// The report quotes both sides in full. A diff of two long strings is the
// reader's job, and truncating it to fit a line is how a test stops explaining
// itself.
//
// std::string and not std::string_view: the argument is usually the return
// value of the function under test, and a view bound to that temporary dangles
// the moment the declaration ends. The copy costs nothing a test will notice.
#define CHECK_STR_EQ(actual, expected)                                       \
    do {                                                                     \
        const std::string ubqActual_ { (actual) };                           \
        const std::string ubqExpected_ { (expected) };                       \
        if (ubqActual_ != ubqExpected_)                                      \
            UBIQUITTY_TEST_FAIL_("%s == %s — got \"%.*s\", want \"%.*s\"",   \
                #actual, #expected,                                          \
                static_cast<int>(ubqActual_.size()), ubqActual_.data(),      \
                static_cast<int>(ubqExpected_.size()), ubqExpected_.data()); \
    } while (0)

// Substring presence, in both directions. Asserting on a formatted blob — an
// RTF fragment, a serialized frame, a rendered row — is a containment question
// far more often than an equality one, and spelling it as `CHECK(x.find(y) !=
// npos)` reports only that a comparison failed, never what was missing.
#define CHECK_CONTAINS(haystack, needle)                                             \
    do {                                                                             \
        const std::string ubqHaystack_ { (haystack) };                               \
        const std::string ubqNeedle_ { (needle) };                                   \
        if (ubqHaystack_.find(ubqNeedle_) == std::string::npos)                      \
            UBIQUITTY_TEST_FAIL_("%s contains %s — \"%.*s\" is missing from:\n%.*s", \
                #haystack, #needle,                                                  \
                static_cast<int>(ubqNeedle_.size()), ubqNeedle_.data(),              \
                static_cast<int>(ubqHaystack_.size()), ubqHaystack_.data());         \
    } while (0)

#define CHECK_NOT_CONTAINS(haystack, needle)                                               \
    do {                                                                                   \
        const std::string ubqHaystack_ { (haystack) };                                     \
        const std::string ubqNeedle_ { (needle) };                                         \
        if (ubqHaystack_.find(ubqNeedle_) != std::string::npos)                            \
            UBIQUITTY_TEST_FAIL_("%s does not contain %s — \"%.*s\" is present in:\n%.*s", \
                #haystack, #needle,                                                        \
                static_cast<int>(ubqNeedle_.size()), ubqNeedle_.data(),                    \
                static_cast<int>(ubqHaystack_.size()), ubqHaystack_.data());               \
    } while (0)

// Equality within a tolerance, for a value that arrives through floating-point
// arithmetic. The tolerance is the caller's because only the caller knows what
// the computation was allowed to lose.
#define CHECK_NEAR(actual, expected, tolerance)                                          \
    do {                                                                                 \
        const double ubqActual_ = static_cast<double>(actual);                           \
        const double ubqExpected_ = static_cast<double>(expected);                       \
        const double ubqTolerance_ = static_cast<double>(tolerance);                     \
        const double ubqDelta_ = ubqActual_ > ubqExpected_                               \
            ? ubqActual_ - ubqExpected_                                                  \
            : ubqExpected_ - ubqActual_;                                                 \
        if (!(ubqDelta_ <= ubqTolerance_))                                               \
            UBIQUITTY_TEST_FAIL_("%s ~= %s — got %f, want %f (off by %f, tolerance %f)", \
                #actual, #expected, ubqActual_, ubqExpected_,                            \
                ubqDelta_, ubqTolerance_);                                               \
    } while (0)

// Prints the verdict and yields the process exit status: 0 = pass.
// CTest keys on the exit code; the line is for a human reading the log.
#define UBIQUITTY_TEST_SUMMARY()                                               \
    (failures                                                                  \
            ? std::printf("test-harness: FAILED — %d assertion%s\n", failures, \
                  failures == 1 ? "" : "s")                                    \
            : std::printf("test-harness: ok (all assertions passed)\n"),       \
        failures ? 1 : 0)
