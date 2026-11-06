// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Harness.h"
#include "SearchFixture.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

// test_scrollsearch.cpp — host coverage for the regex matcher +
// scrollback search API.

#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/ViewPlacement.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Constants.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

/// Pull individual symbols rather than `using namespace Ubiquitty;`
/// through the namespaced terminal model
/// would otherwise collide on unqualified `Terminal` reference.
namespace ScrollSearch = Ubiquitty::Screen::ScrollSearch;

// ---- regex compile + literal match ----

// The compiled program is opaque (an RE2 handle behind a shared_ptr), so the
// old token-array inspection tests became behavioral: compile, search a small
// screen, pin the exact span.

// RE2 syntax beyond the tiny-subset grammar: alternation, groups, counted
// repeats, word boundaries and Perl classes must MATCH.
// Compile failures and truncation guards: an unbalanced group reports a
// one-line diagnostic; an over-long pattern is rejected with the documented
// message; the diagnostic truncates to the caller's buffer, NUL-terminated.
// ---- search over a live screen ----

// ---- multi-line / cross-wrap matching ----

// A token split by a soft wrap matches as one continuous string, reported as
// one logical match split into a segment per visual row it covers.
// A '.*' pattern flows across the soft wrap (no separator between wrapped
// rows), covering the full logical line as one match.
// '.' must not cross a hard (explicit CR/LF) line end: the two halves of a
// pattern straddling a real newline never join.
// '^' / '$' anchor per logical line in the joined buffer (multi-line).
// A zero-length-capable quantifier landing on a line separator paints nothing
// but must not halt the scan: a match later in the buffer is still found.
// next/prev step by LOGICAL match even when a match spans rows: a cross-wrap
// match counts once for navigation + the "k of n" counter.
// ---- next / prev cursor ----

// ---- active-match index (the "k of n" counter) ----

// ---- reveal offset (pure integer policy) ----

// ---- Unicode normalization: precomposed <-> decomposed match ----

// The screen holds DECOMPOSED "café" (e + U+0301); searching for the
// PRECOMPOSED needle "café" (single U+00E9) matches once normalization is
// enabled (both sides fold to the same form), and does not match when
// normalization is off (the default ASCII-collapsing path).
// Screen holds PRECOMPOSED "café" (U+00E9); a DECOMPOSED needle matches under
// normalization — the symmetric case.
// ---- escapeLiteral (literal-query metacharacter escaping) ----

// ---- absoluteRowText (UTF-8 row extraction) ----

// ---- resolveCommandRegion (Ubiquitty::Terminal convenience over the shared Selection rule) ----

/// ---- matches() value collection ----

TEST(testEscapeLiteral)
{
    CHECK(ScrollSearch::escapeLiteral("plain") == "plain");
    CHECK(ScrollSearch::escapeLiteral("a.b*c+d?e") == "a\\.b\\*c\\+d\\?e");
    CHECK(ScrollSearch::escapeLiteral("[x]^$\\") == "\\[x\\]\\^\\$\\\\");
    CHECK(ScrollSearch::escapeLiteral("").empty());
    // RE2's extra metacharacters (grouping, alternation, counted repeats) and
    // plain punctuation/space are escaped too — QuoteMeta-style.
    CHECK(ScrollSearch::escapeLiteral("(a|b){2}") == "\\(a\\|b\\)\\{2\\}");
    CHECK(ScrollSearch::escapeLiteral("a b") == "a\\ b");
    /// And the escaped text compiles to an exact literal match.
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "x (a|b){2} y");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    const std::string esc = ScrollSearch::escapeLiteral("(a|b){2}");
    CHECK(ScrollSearch::compile(esc.c_str(), ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find(esc.c_str(), ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 2 && m->length == 8);
}

TEST(testAbsoluteRowText)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "hello");
    // Trailing blanks past the last glyph are trimmed.
    CHECK(Ubiquitty::Screen::absoluteRowText(t, 0, t.columns) == "hello");

    /// A wide CJK glyph occupies a lead + a wide-continuation cell; the
    /// continuation must be skipped so the character isn't doubled.
    Ubiquitty::Terminal w;
    Ubiquitty::hardReset(w);
    feed(w, "\xE4\xB8\x96"); // U+4E16 世
    CHECK(Ubiquitty::Screen::absoluteRowText(w, 0, w.columns) == "\xE4\xB8\x96");

    /// An empty row reads as an empty string, not a run of spaces.
    Ubiquitty::Terminal blank;
    Ubiquitty::hardReset(blank);
    CHECK(Ubiquitty::Screen::absoluteRowText(blank, 0, blank.columns).empty());
}

TEST(testResolveCommandRegion)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "user$ ls\n"); // row 0 = the command line, cursor drops to row 1
    const int totalRows = Ubiquitty::Screen::scrollbackCount(&t) + t.rows;

    // No CMD_EXECUTED mark yet → the row is not under any command.
    CHECK(!Ubiquitty::Screen::resolveCommandRegion(&t, totalRows, 0).found);

    /// Mark row 0 as an executed prompt (the OSC 133 A→C promotion).
    Ubiquitty::Screen::addRowMark(&t, 0, Ubiquitty::Screen::osc133MarkCmdExecuted);
    const auto region = Ubiquitty::Screen::resolveCommandRegion(&t, totalRows, 3);
    CHECK(region.found);
    CHECK(region.promptRow == 0);
    // No OUTPUT_START mark → the prompt row is its own last command-line row.
    CHECK(region.commandLastRow == 0);
    CHECK(region.outputStart == 0);

    /// With a second executed prompt at row 5, a query below it resolves the
    /// most recent prompt at/before the row (row 5), not the older row 0.
    Ubiquitty::Screen::addRowMark(&t, 5, Ubiquitty::Screen::osc133MarkCmdExecuted);
    CHECK(Ubiquitty::Screen::resolveCommandRegion(&t, totalRows, 7).promptRow == 5);
    // A query between the two prompts still resolves the earlier one.
    CHECK(Ubiquitty::Screen::resolveCommandRegion(&t, totalRows, 4).promptRow == 0);
}

TEST(testMatchesCollection)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "hello world\r\nthere is hello again");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("hello", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    const int n = search.find("hello", ScrollSearch::CaseSensitivity::Sensitive);
    const ScrollSearch::MatchList ms = ScrollSearch::matches(t);
    CHECK(static_cast<int>(ms.size()) == n);
    CHECK(ms.size() == 2);
    // Same first-segment values as matchAt(), returned by value (no pointer).
    CHECK(ms[0].absoluteRow == ScrollSearch::matchAt(t, 0)->absoluteRow);
    CHECK(ms[0].column == ScrollSearch::matchAt(t, 0)->column);
    CHECK(ms[1].absoluteRow == ScrollSearch::matchAt(t, 1)->absoluteRow);
    // An empty search yields an empty collection.
    ScrollSearch::clear(&t);
    CHECK(ScrollSearch::matches(t).empty());
}

TEST(testSlicedSearchPublishesWrappedMatchesAndLabels)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 5);
    feed(t, "ABCDEFGHIJ");
    UbiquittyTest::SearchFixture search(t);
    const auto status = search.searchScrollbackAndWait({ .pattern = "DEFGHIJ", .labelLimit = 1 });
    CHECK_EQ(status.foundCount, 1);
    CHECK_EQ(ScrollSearch::matchCount(t), 1);
    REQUIRE(ScrollSearch::segmentCount(t) == 2);
    const auto* first = ScrollSearch::segmentAt(t, 0);
    const auto* last = ScrollSearch::segmentAt(t, 1);
    CHECK(first->absoluteRow == 0 && first->column == 3 && first->length == 2);
    CHECK(last->absoluteRow == 1 && last->column == 0 && last->length == 5);
    CHECK(first->matchID == last->matchID);
    const auto labels = search.labels();
    REQUIRE(labels.size() == 1);
    CHECK(labels.front().text == "ABCDE");
}

TEST(testLabelsRefreshAndRemainValuesAcrossRequests)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "A\xE4\xB8\x96  ");
    UbiquittyTest::SearchFixture search(t);
    const Ubiquitty::Search::Request request { .pattern = "A", .labelLimit = 1 };
    CHECK_EQ(search.searchScrollbackAndWait(request).foundCount, 1);
    const auto original = search.labels();
    REQUIRE(original.size() == 1);
    CHECK(original.front().text == "A\xE4\xB8\x96");
    CHECK(original.front().text == Ubiquitty::Screen::absoluteRowText(t, 0, t.columns));
    feed(t, "B");
    CHECK_EQ(search.searchScrollbackAndWait(request).foundCount, 1);
    const auto refreshed = search.labels();
    REQUIRE(refreshed.size() == 1);
    CHECK(refreshed.front().text != original.front().text);
    CHECK_EQ(search.searchScrollbackAndWait({ .pattern = "A" }).foundCount, 1);
    CHECK(search.labels().empty());
    CHECK_EQ(search.searchScrollbackAndWait(request).foundCount, 1);
    CHECK(search.labels().size() == 1);
    CHECK_EQ(search.searchScrollbackAndWait({}).foundCount, 0);
    CHECK(original.front().text == "A\xE4\xB8\x96");
}

TEST(testRowTextStopsAtAScrollbackRowsStoredLength)
{
    /// A scrollback row's cells are trimmed to its stored length inside a slab it
    /// shares with the rows around it, so the columns past that length hold the
    /// NEXT row's cells rather than blanks. A label that reads them reports two
    /// lines as one, and every other row-text assertion here sits on the live
    /// screen, where the stored length is the full width and the boundary never
    /// arises.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 16);
    feed(t, "abc\r\nDEFGHIJKLMNOP\r\nlive one\r\nlive two");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) >= 2);
    UbiquittyTest::SearchFixture search(t);
    CHECK_EQ(search.searchScrollbackAndWait({ .pattern = "abc", .labelLimit = 1 }).foundCount, 1);
    const auto labels = search.labels();
    REQUIRE(labels.size() == 1);
    CHECK(labels.front().text == "abc");
    CHECK(labels.front().text == Ubiquitty::Screen::absoluteRowText(t, 0, t.columns));
}

TEST(testCompileLiteral)
{
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("hello", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(re.program != nullptr);
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "say hello");
    CHECK(search.find("hello", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 4 && m->length == 5);
}

TEST(testCompileAnyAndStar)
{
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("a.*b", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    /// Greedy leftmost-first: '.*' runs to the LAST 'b'.
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "axxbxb.");
    CHECK(search.find("a.*b", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 0 && m->length == 6); // "axxbxb"
}

TEST(testCompileClassAndRange)
{
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("[a-c]", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    /// a, b, c match; d does not.
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "abcd");
    CHECK(search.find("[a-c]", ScrollSearch::CaseSensitivity::Sensitive) == 3);
    // Search order, so index zero is the last match on the line: 'c'.
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 2 && m->length == 1);
}

TEST(testCompileNegClass)
{
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("q[^xyz]", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    /// "qa" matches; "qx" is excluded by the negated class.
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "qx qa");
    CHECK(search.find("q[^xyz]", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 3 && m->length == 2);
}

TEST(testCompileAnchors)
{
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("^abc$", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    /// Both anchors must hold on one logical line. Scrollback rows trim to
    /// their stored length, so a scrolled-out "abc" line satisfies '$' while
    /// the live "abcz" line fails it and "zabc" fails '^'.
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 8);
    feed(t, "abc\r\nabcz\r\nzabc");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) >= 1);
    CHECK(search.find("^abc$", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->absoluteRow == 0 && m->column == 0 && m->length == 3);
}

TEST(testCompileRe2Extensions)
{
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "error 42 warnings");

    // Alternation.
    CHECK(ScrollSearch::compile("error|failure", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("error|failure", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 0 && m->length == 5);

    // Grouping + quantifier over the group, Perl class, counted repeat.
    CHECK(ScrollSearch::compile("(err)+or", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("(err)+or", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    CHECK(ScrollSearch::compile("\\d+", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("\\d+", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 6 && m->length == 2); // "42"
    CHECK(ScrollSearch::compile("[a-z]{8}", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("[a-z]{8}", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 9 && m->length == 8); // "warnings"

    // Word boundary: "\berror\b" hits, "\brror\b" does not.
    CHECK(ScrollSearch::compile("\\berror\\b", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("\\berror\\b", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    CHECK(ScrollSearch::compile("\\brror\\b", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("\\brror\\b", ScrollSearch::CaseSensitivity::Sensitive) == 0);

    // Non-greedy variant stops at the FIRST closer.
    CHECK(ScrollSearch::compile("e.*?r", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("e.*?r", ScrollSearch::CaseSensitivity::Sensitive) >= 1);
    m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 0 && m->length == 2); // "er", not greedy "error"
}

TEST(testCompileQuantNoAtomError)
{
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(!ScrollSearch::compile("*abc", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    // RE2 diagnoses the dangling quantifier as a repetition-operator error.
    CHECK(err[0] != 0);
    CHECK(std::strstr(err, "repetition") != nullptr);
}

TEST(testCompileUnterminatedClass)
{
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(!ScrollSearch::compile("[abc", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(err[0] != 0);
}

TEST(testCompileErrorReporting)
{
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(!ScrollSearch::compile("(abc", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(err[0] != 0);
    CHECK(re.program == nullptr);

    char pattern[256];
    std::memset(pattern, 'a', sizeof(pattern) - 1);
    pattern[sizeof(pattern) - 1] = 0;
    CHECK(!ScrollSearch::compile(pattern, ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(std::strstr(err, "too long") != nullptr);

    char tiny[8] = { 0 };
    CHECK(!ScrollSearch::compile("[abc", ScrollSearch::CaseSensitivity::Sensitive, &re, tiny));
    CHECK(std::strlen(tiny) <= 7); // truncated + NUL-terminated
}

TEST(testSearchLiteralMatchInLiveScreen)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "hello world\r\nthere is hello again");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("hello", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("hello", ScrollSearch::CaseSensitivity::Sensitive);
    CHECK(n == 2);
    // Index zero is the newest match: the API runs in search order, which for a
    // terminal is up the buffer from where the reader is looking. The oldest
    // match — row 0, column 0, with no scrollback yet — is last.
    const ScrollSearch::Match* newest = ScrollSearch::matchAt(t, 0);
    CHECK(newest != nullptr);
    CHECK(newest->length == 5);
    const ScrollSearch::Match* oldest = ScrollSearch::matchAt(t, 1);
    CHECK(oldest != nullptr);
    CHECK(oldest->absoluteRow == 0);
    CHECK(oldest->column == 0);
    CHECK(oldest->length == 5);
    // The cursor starts on the newest.
    CHECK(ScrollSearch::activeMatch(t) == newest);
}

TEST(testSearchCaseInsensitive)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "Hello World HELLO");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("hello", ScrollSearch::CaseSensitivity::Insensitive, &re, err));
    int n = search.find("hello", ScrollSearch::CaseSensitivity::Insensitive);
    CHECK(n == 2);
}

TEST(testSearchDotStar)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "abc xyz");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("a.*z", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("a.*z", ScrollSearch::CaseSensitivity::Sensitive);
    CHECK(n == 1);
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 0);
    CHECK(m && m->length == 7); // "abc xyz"
}

TEST(testSearchClass)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "x1 y2 z3");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("[a-z][0-9]", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("[a-z][0-9]", ScrollSearch::CaseSensitivity::Sensitive);
    CHECK(n == 3);
}

TEST(testSearchAnchorBol)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "fooXYZ");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("^foo", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("^foo", ScrollSearch::CaseSensitivity::Sensitive);
    CHECK(n == 1);
}

TEST(testSearchAnchorEol)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "abc end   ");
    // The anchor follows the last written cell, not the grid's empty padding.
    CHECK(search.find(" +$", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const auto* match = ScrollSearch::activeMatch(t);
    REQUIRE(match);
    CHECK_EQ(match->column, 7);
    CHECK_EQ(match->length, 3);
}

TEST(testSearchAcrossSoftWrap)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 8); // 8 columns → "ABCDEFGHIJ" wraps
    feed(t, "ABCDEFGHIJ");
    // The autowrap split it into row0="ABCDEFGH" + row1="IJ", soft-wrapped.
    CHECK(Ubiquitty::Screen::absoluteRowWrapped(&t, 0));
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("DEFGHIJ", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("DEFGHIJ", ScrollSearch::CaseSensitivity::Sensitive);
    // One LOGICAL match (the "k of n" counter sees 1).
    CHECK(n == 1);
    CHECK(ScrollSearch::matchCount(t) == 1);
    // It paints across two rows: two segments sharing a matchID.
    CHECK(ScrollSearch::segmentCount(t) == 2);
    const ScrollSearch::Match* s0 = ScrollSearch::segmentAt(t, 0);
    const ScrollSearch::Match* s1 = ScrollSearch::segmentAt(t, 1);
    CHECK(s0 && s1);
    // First segment: "DEFGH" on row 0 starting at column 3.
    CHECK(s0->absoluteRow == 0);
    CHECK(s0->column == 3);
    CHECK(s0->length == 5);
    // Second segment: "IJ" on row 1 starting at column 0.
    CHECK(s1->absoluteRow == 1);
    CHECK(s1->column == 0);
    CHECK(s1->length == 2);
    // Both segments belong to the same logical match.
    CHECK(s0->matchID == s1->matchID);
    /// matchAt / activeMatch report the FIRST segment (the match's start).
    const ScrollSearch::Match* m0 = ScrollSearch::matchAt(t, 0);
    CHECK(m0 == s0);
    CHECK(ScrollSearch::activeMatch(t) == s0);
}

TEST(testSearchDotstarAcrossSoftWrap)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 8);
    feed(t, "ABCDEFGHIJ"); // wraps at col 8
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    // "A.*J" must bridge the wrap to reach the J on row 1.
    CHECK(ScrollSearch::compile("A.*J", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("A.*J", ScrollSearch::CaseSensitivity::Sensitive);
    CHECK(n == 1);
    CHECK(ScrollSearch::segmentCount(t) == 2);
    const ScrollSearch::Match* s0 = ScrollSearch::segmentAt(t, 0);
    const ScrollSearch::Match* s1 = ScrollSearch::segmentAt(t, 1);
    CHECK(s0 && s0->absoluteRow == 0 && s0->column == 0 && s0->length == 8); // ABCDEFGH
    CHECK(s1 && s1->absoluteRow == 1 && s1->column == 0 && s1->length == 2); // IJ
}

TEST(testSearchDotNoCrossHardNewline)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 12);
    feed(t, "foo\r\nbar"); // explicit CR/LF between foo and bar
    // row 0 is not soft-wrapped (an explicit line end terminated it).
    CHECK(!Ubiquitty::Screen::absoluteRowWrapped(&t, 0));
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("foo.bar", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("foo.bar", ScrollSearch::CaseSensitivity::Sensitive) == 0);
    // A literal straddling the newline likewise doesn't join.
    CHECK(ScrollSearch::compile("oobar", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("oobar", ScrollSearch::CaseSensitivity::Sensitive) == 0);
}

TEST(testSearchMultilineAnchors)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 12);
    feed(t, "foo\r\nbar\r\nbaz");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    // '^bar' matches the start of the second line, not just the buffer start.
    CHECK(ScrollSearch::compile("^bar", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("^bar", ScrollSearch::CaseSensitivity::Sensitive);
    CHECK(n == 1);
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->absoluteRow == 1 && m->column == 0 && m->length == 3);
}

TEST(testSearchOptionalPrefixPastNewline)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 6);
    feed(t, "ab\r\ncd\r\nef"); // three lines, two hard separators
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    // 'x*ef' matches zero x's at each line start (incl. right after the
    // separators) then the literal 'ef' on the last line. A scan that stopped
    // at the first separator's zero-length hit would miss it.
    CHECK(ScrollSearch::compile("x*ef", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("x*ef", ScrollSearch::CaseSensitivity::Sensitive);
    CHECK(n == 1);
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->absoluteRow == 2 && m->column == 0 && m->length == 2);
}

TEST(testSearchNavigationGroupsSegments)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 8);
    // Two wrapped tokens: each "ABCDEFGHIJ" + a separating newline.
    feed(t, "ABCDEFGHIJ\r\nABCDEFGHIJ");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("DEFGHIJ", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("DEFGHIJ", ScrollSearch::CaseSensitivity::Sensitive);
    // Two logical matches, four segments (two rows each).
    CHECK(n == 2);
    CHECK(ScrollSearch::matchCount(t) == 2);
    CHECK(ScrollSearch::segmentCount(t) == 4);
    // Navigation steps over logical matches, not segments, and the index it
    // reports is the position in search order: the newest match is the first
    // one reached, so a counter built from it reads "1 of 2" at the bottom.
    CHECK(ScrollSearch::activeMatchIndex(t) == 0);
    CHECK(search.step(Ubiquitty::Search::StepDirection::Older));
    CHECK(ScrollSearch::activeMatchIndex(t) == 1);
    CHECK(search.step(Ubiquitty::Search::StepDirection::Older)); // wraps to the newest
    CHECK(ScrollSearch::activeMatchIndex(t) == 0);
}

TEST(testNextAndPrevCursor)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "AAA AAA AAA");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("AAA", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("AAA", ScrollSearch::CaseSensitivity::Sensitive);
    CHECK(n == 3);
    // Three matches on one line, so search order runs right to left: the
    // cursor lands on the last one and next walks back toward the start.
    CHECK(ScrollSearch::activeMatch(t)->column == 8);
    CHECK(search.step(Ubiquitty::Search::StepDirection::Older));
    CHECK(ScrollSearch::activeMatch(t)->column == 4);
    CHECK(search.step(Ubiquitty::Search::StepDirection::Older));
    CHECK(ScrollSearch::activeMatch(t)->column == 0);
    // Wrap: next at the oldest returns to the newest.
    CHECK(search.step(Ubiquitty::Search::StepDirection::Older));
    CHECK(ScrollSearch::activeMatch(t)->column == 8);
    // Prev runs the other way, back down toward the bottom, and wraps likewise.
    CHECK(search.step(Ubiquitty::Search::StepDirection::Newer));
    CHECK(ScrollSearch::activeMatch(t)->column == 0);
}

TEST(historySearchStepsNewestMatchFirstWithinEachLine)
{
    for (const char* suffix : { "", " \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7" }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 2, 30);
        UbiquittyTest::SearchFixture search(terminal);
        feed(terminal, std::string("AAA AAA AAA") + suffix + "\r\nquiet\r\nquiet\r\n");
        CHECK(search.find("AAA") == 3);
        for (const int column : { 8, 4, 0 }) {
            const auto* match = ScrollSearch::activeMatch(terminal);
            REQUIRE(match);
            CHECK_EQ(match->column, column);
            CHECK(search.step(Ubiquitty::Search::StepDirection::Older));
        }
        CHECK_EQ(ScrollSearch::activeMatchIndex(terminal), 0);
    }
}

TEST(testActiveMatchIndex)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "AAA AAA AAA");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("AAA", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("AAA", ScrollSearch::CaseSensitivity::Sensitive) == 3);
    // Active starts at index 0.
    CHECK(ScrollSearch::activeMatchIndex(t) == 0);
    // Steps track the active cursor.
    CHECK(search.step(Ubiquitty::Search::StepDirection::Older));
    CHECK(ScrollSearch::activeMatchIndex(t) == 1);
    CHECK(search.step(Ubiquitty::Search::StepDirection::Older));
    CHECK(ScrollSearch::activeMatchIndex(t) == 2);
    // Wrap back to 0.
    CHECK(search.step(Ubiquitty::Search::StepDirection::Older));
    CHECK(ScrollSearch::activeMatchIndex(t) == 0);
    /// It agrees with an identity-against-matchAt walk done by hand.
    const ScrollSearch::Match* a = ScrollSearch::activeMatch(t);
    int byIdentity = -1;
    for (int i = 0; i < ScrollSearch::matchCount(t); ++i)
        if (ScrollSearch::matchAt(t, i) == a) {
            byIdentity = i;
            break;
        }
    CHECK(byIdentity == ScrollSearch::activeMatchIndex(t));
    // After clear there is no active index.
    ScrollSearch::clear(&t);
    CHECK(ScrollSearch::activeMatchIndex(t) == -1);
    /// A foreign Terminal (never searched) → -1, not a stale value.
    Ubiquitty::Terminal other;
    Ubiquitty::hardReset(other);
    CHECK(ScrollSearch::activeMatchIndex(other) == -1);
}

TEST(testViewOffsetToReveal)
{
    using Ubiquitty::Screen::RevealPlacement;
    using Ubiquitty::Screen::viewOffsetToReveal;
    /// The match lands one-third down: offset = scrollback - absoluteRow + rows/3.
    /// A command block lands higher; that placement is covered in
    /// test_view_placement, which owns the comparison between the two.
    /// With 24 rows, rows/3 == 8.
    const int rows = 24;
    // A live-tail match (absoluteRow == scrollbackCount) with no history reveals at
    // the top-third without scrolling past the (empty) history.
    CHECK(viewOffsetToReveal(/*scrollback*/ 0, rows, /*absoluteRow*/ 0, RevealPlacement::Match) == 0);

    // A match deep in history scrolls the viewport up. scrollback 100, match at
    // absolute row 40: 100 - 40 + 8 == 68, in range [0, 100].
    CHECK(viewOffsetToReveal(100, rows, 40, RevealPlacement::Match) == 68);

    // The oldest row clamps to scrollbackCount (cannot scroll past the top):
    // 100 - 0 + 8 == 108 → clamped to 100.
    CHECK(viewOffsetToReveal(100, rows, 0, RevealPlacement::Match) == 100);

    // A match at/near the live tail clamps to 0 (cannot scroll below the tail):
    // 50 - 50 + 8 == 8, still positive — but a row *below* the third line would
    // go negative. absoluteRow = scrollback + rows: 50 - 58 + 8 == 0.
    CHECK(viewOffsetToReveal(50, rows, 58, RevealPlacement::Match) == 0);
    // Further below still clamps to 0, never negative.
    CHECK(viewOffsetToReveal(50, rows, 200, RevealPlacement::Match) == 0);

    // rows/3 truncates toward zero (integer division): 23 rows → target 7.
    CHECK(viewOffsetToReveal(100, 23, 40, RevealPlacement::Match) == 100 - 40 + 7);
    // Zero-height viewport → target row 0; pure history offset.
    CHECK(viewOffsetToReveal(100, 0, 40, RevealPlacement::Match) == 60);
}

TEST(testClearDropsState)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "abc");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("abc", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    search.find("abc", ScrollSearch::CaseSensitivity::Sensitive);
    CHECK(ScrollSearch::matchCount(t) == 1);
    ScrollSearch::clear(&t);
    CHECK(ScrollSearch::matchCount(t) == 0);
    CHECK(ScrollSearch::activeMatch(t) == nullptr);
}

TEST(testNoMatch)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    feed(t, "hello");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("xyzzy", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    int n = search.find("xyzzy", ScrollSearch::CaseSensitivity::Sensitive);
    CHECK(n == 0);
    CHECK(ScrollSearch::activeMatch(t) == nullptr);
}

TEST(testSearchNormalizationPrecomposedNeedle)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 4);
    // "caf" + 'e' + COMBINING ACUTE ACCENT (U+0301 = 0xCC 0x81).
    feed(t, "caf\x65\xCC\x81");
    /// Precomposed needle: 'c','a','f', U+00E9 (0xC3 0xA9).
    const char* needle = "caf\xC3\xA9";

    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };

    // None (default): search still compares in NFC, so the precomposed needle
    // finds the decomposed text; the setting governs what copy does.
    ScrollSearch::setNormalizationForm(Ubiquitty::Unicode::NormalizationForm::None);
    CHECK(ScrollSearch::compile(needle, ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find(needle, ScrollSearch::CaseSensitivity::Sensitive) == 1);

    // NFC: needle and haystack both normalize to precomposed é -> match.
    ScrollSearch::setNormalizationForm(Ubiquitty::Unicode::NormalizationForm::NFC);
    CHECK(ScrollSearch::compile(needle, ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find(needle, ScrollSearch::CaseSensitivity::Sensitive) == 1);

    // NFD: both decompose to e + U+0301 -> still match.
    ScrollSearch::setNormalizationForm(Ubiquitty::Unicode::NormalizationForm::NFD);
    CHECK(ScrollSearch::compile(needle, ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find(needle, ScrollSearch::CaseSensitivity::Sensitive) == 1);

    // Restore the default for later tests.
    ScrollSearch::setNormalizationForm(Ubiquitty::Unicode::NormalizationForm::None);
}

TEST(testSearchNormalizationDecomposedNeedle)
{
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 4);
    feed(t, "caf\xC3\xA9"); // precomposed é on screen
    const char* needle = "caf\x65\xCC\x81"; // decomposed needle

    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    ScrollSearch::setNormalizationForm(Ubiquitty::Unicode::NormalizationForm::NFC);
    CHECK(ScrollSearch::compile(needle, ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find(needle, ScrollSearch::CaseSensitivity::Sensitive) == 1);

    ScrollSearch::setNormalizationForm(Ubiquitty::Unicode::NormalizationForm::None);
}

TEST(testAGrownScrollbackKeepsItsPrefixAndItsMatchRows)
{
    // Appending without eviction leaves the existing matches on their rows.
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 16);
    // Matches in the first and last retained rows both survive the append.
    feed(t, "needle one\r\nbravo\r\nneedle two\r\ncharlie\r\ndelta\r\n");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) >= 3);

    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("needle", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("needle", ScrollSearch::CaseSensitivity::Sensitive) == 2);
    // Search order runs newest first, so the oldest match is the last index.
    const ScrollSearch::Match* oldest = ScrollSearch::matchAt(t, 1);
    CHECK(oldest);
    const int oldestRow = oldest ? oldest->absoluteRow : -1;

    feed(t, "echo\r\nneedle three\r\nfoxtrot\r\n");
    CHECK(search.find("needle", ScrollSearch::CaseSensitivity::Sensitive) == 3);
    // The rows the old matches sat on are unchanged by the growth, and the new
    // match is below them — which is now index zero, the newest.
    const ScrollSearch::Match* oldestAgain = ScrollSearch::matchAt(t, 2);
    CHECK(oldestAgain && oldestAgain->absoluteRow == oldestRow);
    const ScrollSearch::Match* newest = ScrollSearch::matchAt(t, 0);
    CHECK(newest && newest->absoluteRow > oldestRow);
}

TEST(testAnEvictingScrollbackMovesItsMatchesDownByWhatItDropped)
{
    // Eviction renumbers retained matches. Compare to the row's actual text.
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 16);
    t.scrollbackLimit = 6;
    const auto needleRow = [&t]() {
        const int total = Ubiquitty::Screen::scrollbackCount(&t) + t.rows;
        for (int row = 0; row < total; ++row) {
            if (Ubiquitty::Screen::absoluteRowText(t, row, t.columns) == "needle")
                return row;
        }
        return -1;
    };

    feed(t, "one\r\ntwo\r\nthree\r\nfour\r\nneedle\r\nfive\r\n");
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("needle", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("needle", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* before = ScrollSearch::matchAt(t, 0);
    CHECK(before && before->absoluteRow == needleRow());

    // Enough output that the history fills and starts dropping its oldest.
    feed(t, "five\r\nsix\r\nseven\r\neight\r\n");
    const int shifted = needleRow();
    // Both guards keep the case from passing on a degeneracy: at row zero a
    // reused span that forgot to rebase still answers zero, and unmoved it is
    // not the eviction case at all.
    CHECK(shifted > 0);
    CHECK(shifted < (before ? before->absoluteRow : 0));
    CHECK(search.find("needle", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* after = ScrollSearch::matchAt(t, 0);
    CHECK(after && after->absoluteRow == shifted);

    // Once it is evicted outright, nothing survives to point at.
    feed(t, "nine\r\nten\r\neleven\r\ntwelve\r\nthirteen\r\nfourteen\r\n");
    CHECK(needleRow() == -1);
    CHECK(search.find("needle", ScrollSearch::CaseSensitivity::Sensitive) == 0);
}

TEST(repeatedSearchSeesALiveScreenEditThatMovesNoRowCount)
{
    // A repeated automation query must rescan text overwritten in place.
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 16);
    feed(t, "alpha");

    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    CHECK(ScrollSearch::compile("alpha", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("alpha", ScrollSearch::CaseSensitivity::Sensitive) == 1);

    // Overwrite in place: carriage return, not a newline, so nothing scrolls.
    feed(t, "\rbeta ");
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    CHECK(search.find("alpha", ScrollSearch::CaseSensitivity::Sensitive) == 0);

    ScrollSearch::CompiledRegex second;
    CHECK(ScrollSearch::compile("beta", ScrollSearch::CaseSensitivity::Sensitive, &second, err));
    CHECK(search.find("beta", ScrollSearch::CaseSensitivity::Sensitive) == 1);
}

TEST(testNormalizedMatchReportsTheCellColumnNotTheByteOffset)
{
    /// A cell is one byte of the haystack only while no normalization form is
    /// active; under one it is the cell's normalized UTF-8, which is several. A
    /// match is reported in cells, so a multi-byte cell preceding the match makes
    /// the byte offset and the column disagree, and only a match placed past one
    /// can tell them apart. The other normalization cases here assert a count,
    /// which every mapping satisfies.
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 8, 16);
    feed(t, "\xC3\xA9 caf\xC3\xA9"); // 'é', space, then "café" from cell column 2

    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    ScrollSearch::setNormalizationForm(Ubiquitty::Unicode::NormalizationForm::NFC);
    CHECK(ScrollSearch::compile("caf\xC3\xA9", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("caf\xC3\xA9", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* m = ScrollSearch::matchAt(t, 0);
    CHECK(m && m->column == 2);
    ScrollSearch::setNormalizationForm(Ubiquitty::Unicode::NormalizationForm::None);
}

// ---- history search by logical line ----

namespace {

using Ubiquitty::Screen::HistoryLineRange;
using Ubiquitty::Unicode::NormalizationForm;

// A terminal of 24 rows whose history holds `lines` lines, the one at
// `marked` carrying the word NEEDLE past column 100, so it wraps at every
// width below that.
std::unique_ptr<Ubiquitty::Terminal> historyWithNeedle(int lines, int marked, int columns = 80)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(t.get(), 20'000);
    Ubiquitty::resize(*t, 24, columns);
    for (int line = 0; line < lines; ++line) {
        std::string text = "line " + std::to_string(line);
        if (line == marked)
            text += std::string(100, '.') + "NEEDLE";
        feed(*t, text + "\r\n");
    }
    return t;
}

constexpr HistoryLineRange everyLine { .from = 0, .to = ~std::uint64_t { 0 } };

} // namespace

// The host search: a match in history is addressed by its
// logical line, which a resize does not move, so a client that never fetched
// the line can ask for it by number.
TEST(aHistorySearchAddressesLogicalLinesAcrossAReflow)
{
    auto t = historyWithNeedle(200, 50);
    const std::optional<ScrollSearch::HistorySearchResult> before = ScrollSearch::searchHistory(*t, "NEEDLE", everyLine, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::None, 16);
    REQUIRE(before && before->matches.size() == 1);
    CHECK_EQ(before->matches.front().line, std::uint64_t { 50 });
    CHECK_EQ(before->matches.front().startCell, 107u);
    CHECK_EQ(before->matches.front().endCell, 113u);
    Ubiquitty::resize(*t, 24, 37);
    const std::optional<ScrollSearch::HistorySearchResult> after = ScrollSearch::searchHistory(*t, "NEEDLE", everyLine, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::None, 16);
    REQUIRE(after && after->matches.size() == 1);
    CHECK(after->matches.front() == before->matches.front());
}

// A match across a soft wrap is one match on one line.
TEST(aMatchAcrossASoftWrapIsOneLogicalMatch)
{
    auto t = historyWithNeedle(100, 30, 54);
    const auto found = ScrollSearch::searchHistory(*t, "\\.{2}NEEDLE", everyLine, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::None, 16);
    REQUIRE(found && found->matches.size() == 1);
    CHECK_EQ(found->matches.front().line, std::uint64_t { 30 });
    CHECK_EQ(found->matches.front().endCell - found->matches.front().startCell, 8u);
}

// A clear opens an epoch: nothing below its first line is found, and the
// result names the new epoch.
TEST(aSearchAfterAClearFindsNoClearedLine)
{
    auto t = historyWithNeedle(100, 30);
    feed(*t, "\x1b[3J");
    const auto found = ScrollSearch::searchHistory(*t, "NEEDLE", everyLine, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::None, 16);
    REQUIRE(found.has_value());
    CHECK(found->matches.empty());
    CHECK_EQ(found->epoch, Ubiquitty::Screen::historyAddress(*t).epoch);
    CHECK_EQ(found->epoch, 2u);
}

TEST(anOverlongPatternIsRefused)
{
    auto t = historyWithNeedle(10, 3);
    CHECK(!ScrollSearch::searchHistory(*t, std::string(Ubiquitty::Screen::scrollSearchMaxPattern, 'a'), everyLine, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::None, 16));
    CHECK(!ScrollSearch::searchHistory(*t, "(", everyLine, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::None, 16));
}

TEST(resultsAreCappedAndSayTheyWereTruncated)
{
    auto t = historyWithNeedle(300, 3);
    const auto capped = ScrollSearch::searchHistory(*t, "line", everyLine, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::None, 20);
    REQUIRE(capped.has_value());
    CHECK_EQ(capped->matches.size(), std::size_t { 20 });
    CHECK(capped->isTruncated);
    const auto whole = ScrollSearch::searchHistory(*t, "line 1[0-9]$", everyLine, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::None, 20);
    REQUIRE(whole.has_value());
    CHECK_EQ(whole->matches.size(), std::size_t { 10 });
    CHECK(!whole->isTruncated);
}

// Lines the ring evicted, and a range wholly below them, yield nothing.
TEST(aRangeBelowOldestYieldsNothing)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::setScrollbackLimit(t.get(), 100);
    Ubiquitty::resize(*t, 24, 80);
    for (int line = 0; line < 1'000; ++line)
        feed(*t, "line " + std::to_string(line) + "\r\n");
    const std::uint64_t oldest = Ubiquitty::Screen::historyAddress(*t).oldest;
    REQUIRE(oldest > 500);
    const auto found = ScrollSearch::searchHistory(*t, "line", { .from = 0, .to = oldest }, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::None, 16);
    REQUIRE(found.has_value());
    CHECK(found->matches.empty());
}

// The form is the caller's: the process-wide setting the find field uses
// changes nothing here, and the result's form is the one asked for. NFKC folds
// the "fi" ligature to "fi" and NFC, which None compares in, does not.
TEST(searchIgnoresTheProcessNormalizationSetting)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 24, 80);
    feed(*t, "\xEF\xAC\x81le\r\n"); // U+FB01 LATIN SMALL LIGATURE FI, then "le"
    for (int line = 0; line < 30; ++line)
        feed(*t, "\r\n");
    ScrollSearch::setNormalizationForm(NormalizationForm::NFKC);
    const auto withoutForm = ScrollSearch::searchHistory(*t, "file", everyLine, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::None, 16);
    ScrollSearch::setNormalizationForm(NormalizationForm::None);
    const auto withForm = ScrollSearch::searchHistory(*t, "file", everyLine, ScrollSearch::CaseSensitivity::Sensitive, NormalizationForm::NFKC, 16);
    REQUIRE(withoutForm && withForm);
    CHECK(withoutForm->matches.empty());
    REQUIRE(withForm->matches.size() == 1);
    CHECK_EQ(withForm->matches.front().startCell, 0u);
    CHECK_EQ(withForm->matches.front().endCell, 3u);
}

// Find reads every script by default. Before, the default haystack kept only
// printable ASCII and turned every other character into a space, so none of
// these were found unless a normalization form had been configured.
TEST(searchFindsTextOfEveryScriptByDefault)
{
    ScrollSearch::setNormalizationForm(NormalizationForm::None);
    struct Case {
        const char* screen;
        const char* needle;
        ScrollSearch::CaseSensitivity sensitivity;
        int column;
        int length;
    };
    const Case cases[] = {
        { "ab \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", ScrollSearch::CaseSensitivity::Sensitive, 3, 6 }, // Привет
        { "\xD0\x9F\xD0\xA0\xD0\x98\xD0\x92\xD0\x95\xD0\xA2", "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", ScrollSearch::CaseSensitivity::Insensitive, 0, 6 }, // ПРИВЕТ, привет
        { "x \xCE\x91\xCE\x92\xCE\x93", "\xCE\xB1\xCE\xB2\xCE\xB3", ScrollSearch::CaseSensitivity::Insensitive, 2, 3 }, // ΑΒΓ, αβγ
        { "\xE6\x97\xA5\xE6\x9C\xAC abc", "abc", ScrollSearch::CaseSensitivity::Sensitive, 5, 3 }, // 日本 abc: two wide cells before
        { "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "\xE6\x9C\xAC\xE8\xAA\x9E", ScrollSearch::CaseSensitivity::Sensitive, 2, 4 }, // 日本語, 本語
        { "na\xC3\xAFve", "NA\xC3\x8FVE", ScrollSearch::CaseSensitivity::Insensitive, 0, 5 }, // naïve, NAÏVE
    };
    for (const Case& c : cases) {
        Ubiquitty::Terminal t;
        UbiquittyTest::SearchFixture search(t);
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 4, 40);
        feed(t, c.screen);
        ScrollSearch::CompiledRegex re;
        char err[64] = { 0 };
        REQUIRE(ScrollSearch::compile(ScrollSearch::escapeLiteral(c.needle).c_str(), c.sensitivity, &re, err));
        CHECK_MSG(search.find(ScrollSearch::escapeLiteral(c.needle).c_str(), c.sensitivity) == 1, c.needle);
        const ScrollSearch::Match* m = ScrollSearch::activeMatch(t);
        CHECK_MSG(m && m->column == c.column && m->length == c.length, c.needle);
    }
}

// A cluster that lives in the pool (a ZWJ family) is searched as the whole
// cluster, and a match after it lands on the right cells.
TEST(searchFindsPooledClustersAndCellsAfterThem)
{
    ScrollSearch::setNormalizationForm(NormalizationForm::None);
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 40);
    const char* family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    feed(t, (std::string(family) + " ok").c_str());
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    REQUIRE(ScrollSearch::compile(family, ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find(family, ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* m = ScrollSearch::activeMatch(t);
    CHECK(m && m->column == 0);
    REQUIRE(ScrollSearch::compile("ok", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("ok", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    m = ScrollSearch::activeMatch(t);
    CHECK(m && m->column == 3 && m->length == 2);
}

TEST(searchMeasuresPlainMatchesIndependentlyOfWiderCharacters)
{
    // Match extents count cells independently of another line's UTF-8 width.
    ScrollSearch::setNormalizationForm(NormalizationForm::None);
    Ubiquitty::Terminal t;
    UbiquittyTest::SearchFixture search(t);
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 10);
    feed(t, "foo bar\r\nx\r\nx\r\nx\r\n\xE6\x97\xA5\xE6\x9C\xAC");
    REQUIRE(t.scrollbackLimit > 0);
    ScrollSearch::CompiledRegex re;
    char err[64] = { 0 };
    REQUIRE(ScrollSearch::compile("bar", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("bar", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    const ScrollSearch::Match* m = ScrollSearch::activeMatch(t);
    REQUIRE(m);
    CHECK_EQ(m->absoluteRow, 0);
    CHECK_EQ(m->column, 4);
    CHECK_EQ(m->length, 3);
    REQUIRE(ScrollSearch::compile("\xE6\x9C\xAC", ScrollSearch::CaseSensitivity::Sensitive, &re, err));
    CHECK(search.find("\xE6\x9C\xAC", ScrollSearch::CaseSensitivity::Sensitive) == 1);
    m = ScrollSearch::activeMatch(t);
    REQUIRE(m);
    CHECK_EQ(m->column, 2);
    CHECK_EQ(m->length, 2);
}

UBIQUITTY_TEST_MAIN()
