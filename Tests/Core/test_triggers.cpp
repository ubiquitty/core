// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_triggers — the portable Triggers rule engine: ordered {regex,
// action} line triggers, the smart-selection characterClass expansion table,
// and the raw regex compile/match primitives they share. Host-tested so a
// future frontend wires the feed (evaluateLine) without re-verifying the
// matcher.

#include "Core/Triggers/Engine.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

using namespace Ubiquitty;
using namespace Ubiquitty::Triggers;

// ---- raw regex primitives ----------------------------------------

TEST(testCompileRejectsBadPatterns)
{
    Program p;
    CHECK(!compile(nullptr, Ubiquitty::CaseSensitivity::Sensitive, &p));
    CHECK(!compile("[abc", Ubiquitty::CaseSensitivity::Sensitive, &p)); // unterminated class
    CHECK(!compile("*abc", Ubiquitty::CaseSensitivity::Sensitive, &p)); // quantifier with no atom
    CHECK(!p.valid);
    CHECK(compile("abc", Ubiquitty::CaseSensitivity::Sensitive, &p)); // plain literal sequence
    CHECK(p.valid);
}

TEST(testMatchSpanLeftmost)
{
    Program p;
    CHECK(compile("[0-9]+", Ubiquitty::CaseSensitivity::Sensitive, &p));
    const std::optional<MatchSpan> hit = matchSpan(p, "ab 42 cd 99");
    CHECK(hit);
    CHECK(hit && hit->begin == 3); // leftmost run "42"
    CHECK(hit && hit->end == 4);
}

TEST(testMatchSpanAnchors)
{
    Program p;
    CHECK(compile("^ERROR", Ubiquitty::CaseSensitivity::Sensitive, &p));
    const std::optional<MatchSpan> atStart = matchSpan(p, "ERROR: boom");
    CHECK(atStart && atStart->begin == 0 && atStart->end == 4);
    CHECK(!matchSpan(p, "an ERROR here")); // BOL-anchored

    Program q;
    CHECK(compile("done$", Ubiquitty::CaseSensitivity::Sensitive, &q));
    const std::optional<MatchSpan> atEnd = matchSpan(q, "all done");
    CHECK(atEnd && atEnd->begin == 4 && atEnd->end == 7);
    CHECK(!matchSpan(q, "done now")); // EOL-anchored
}

// A character class addresses all 256 byte values, because both sides of the
// bitmap are indexed by a byte.
//
// classContains takes its byte from the SUBJECT LINE — evaluateLine's utf8, which
// is a committed line of terminal output, so any program that can print picks it.
// classSet takes its byte from the pattern. The bitmap was 16 bytes, addressing
// 0x00-0x7F, and both sides indexed it with `c / 8` for c up to 255. So every
// non-ASCII byte in terminal output read up to 16 bytes past the end of its
// token, into the next token of the same array: a match decision made from
// adjacent compiled pattern data. A trigger action can run a command, so a
// wrong decision is not only a wrong highlight.
//
// Fails-without-it under ASan; without a sanitizer it reads neighbouring bytes
// and the assertions below are the observable half — a negated class must accept
// a byte no class holds, and a class built from a non-ASCII pattern must match
// the byte it was built from rather than whatever sits 16 bytes on.
TEST(testCharacterClassCoversTheWholeByteRange)
{
    // A negated ASCII class meets a non-ASCII subject byte. The read is the part
    // that was out of bounds; the answer must be "yes, it is not in [0-9]".
    {
        Program p;
        CHECK(compile("[^0-9]", Ubiquitty::CaseSensitivity::Sensitive, &p));
        CHECK(matchSpan(p, "\xC3\xA9")); // U+00E9 as UTF-8; neither byte is a digit
        CHECK(matchSpan(p, "\xFF"));
    }

    // A plain ASCII class rejects a high byte. Reading past the end could
    // answer either way depending on the neighbouring token.
    {
        Program p;
        CHECK(compile("[0-9]", Ubiquitty::CaseSensitivity::Sensitive, &p));
        CHECK(!matchSpan(p, "\xC3\xA9"));
        CHECK(!matchSpan(p, "\x80\x81\x82"));
    }

    // Every high byte is addressable from the write side too, so a class built
    // over one matches it and only it.
    {
        Program p;
        CHECK(compile("[\xC3]", Ubiquitty::CaseSensitivity::Sensitive, &p));
        CHECK(matchSpan(p, "a\xC3z"));
        CHECK(!matchSpan(p, "a\xC4z"));
    }

    // The top of the range is the index that ran furthest past the end: 0xFF/8
    // is 31, against a 16-byte array.
    {
        Program p;
        CHECK(compile("[\xFF]", Ubiquitty::CaseSensitivity::Sensitive, &p));
        CHECK(matchSpan(p, "\xFF"));
        CHECK(!matchSpan(p, "\xFE"));
    }

    // A class next to another token is the case where an over-read lands in real
    // compiled data rather than in padding.
    {
        Program p;
        CHECK(compile("[0-9]x", Ubiquitty::CaseSensitivity::Sensitive, &p));
        CHECK(matchSpan(p, "7x"));
        CHECK(!matchSpan(p, "\xC3x"));
    }
}

// A line cannot make one pattern search forever.
//
// The matcher backtracks, so adjacent quantifiers such as `a*a*a*a*a*a*a*x`
// explore every way to split the subject between them, and the work grows
// steeply with the subject's length.
//
// A discriminating literal between the quantifiers, as in `.*a.*b.*c.*d`,
// prunes, so this is not "regexes are slow". It is one shape, and the shape is
// reachable because the PATTERN is the user's own config while the SUBJECT is a
// committed line of terminal output — so whoever can print picks the input that
// makes an ordinary rule behave like a hostile one, on the parse path.
//
// The budget converts the hang into a no-match, and reports which it was.
//
// Fails-without-it: raise maxMatchSteps out of reach and `exhausted` stays false
// while the case runs for as long as the backtracking does.
TEST(testMatchIsBoundedAgainstAdversarialSubjects)
{
    // Adjacent quantifiers over one class, with a final byte the subject lacks —
    // so every split is explored and none succeeds.
    Program blowup;
    CHECK(compile("a*a*a*a*a*a*a*x", Ubiquitty::CaseSensitivity::Sensitive, &blowup));

    const std::string subject(60, 'a');
    bool didExhaustBudget = false;
    const std::optional<MatchSpan> hit = matchSpan(blowup, subject, &didExhaustBudget);
    CHECK(!hit); // there is no x, so no match either way
    CHECK(didExhaustBudget); // and the search stopped rather than finishing

    // A pattern that is not being made to backtrack finishes inside the budget,
    // so the ceiling never changes an ordinary answer.
    Program ordinary;
    CHECK(compile("[0-9]+ tests? passed", Ubiquitty::CaseSensitivity::Sensitive, &ordinary));
    bool didExhaustOnOrdinary = true; // seeded wrong, so a silent no-write shows up
    const std::optional<MatchSpan> ordinaryHit = matchSpan(ordinary, "42 tests passed", &didExhaustOnOrdinary);
    CHECK(ordinaryHit);
    CHECK(!didExhaustOnOrdinary);

    // Same shape as the blowup, but with literals between the quantifiers: it
    // prunes, so it must finish rather than exhaust.
    Program pruned;
    CHECK(compile(".*a.*b.*c.*d", Ubiquitty::CaseSensitivity::Sensitive, &pruned));
    bool didExhaustOnPruned = true;
    const std::optional<MatchSpan> prunedHit = matchSpan(pruned, std::string(80, 'a'), &didExhaustOnPruned);
    CHECK(!prunedHit); // no b/c/d in the subject
    CHECK(!didExhaustOnPruned); // but it got there by searching, not by giving up

    // The out-parameter is optional, and omitting it must not change the answer.
    CHECK(!matchSpan(blowup, subject));
}

TEST(testMatchSpanCaseInsensitive)
{
    Program ci;
    CHECK(compile("warn", Ubiquitty::CaseSensitivity::Insensitive, &ci));
    const std::optional<MatchSpan> insensitive = matchSpan(ci, "a WARN b");
    CHECK(insensitive && insensitive->begin == 2 && insensitive->end == 5);

    Program cs;
    CHECK(compile("warn", Ubiquitty::CaseSensitivity::Sensitive, &cs));
    CHECK(!matchSpan(cs, "a WARN b"));
}

// ---- ordered trigger rules ---------------------------------------

TEST(testSingleRuleMatch)
{
    Engine eng;
    CHECK(eng.addRule("[0-9]+ tests? passed", Action::Notify, "Tests", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(eng.ruleCount() == 1);

    Matched out[8];
    int n = eng.evaluateLine("42 tests passed", { out, 8 });
    CHECK(n == 1);
    CHECK(out[0].ruleIndex == 0);
    CHECK(out[0].action == Action::Notify);
    CHECK(std::strcmp(out[0].arg, "Tests") == 0);
    CHECK(out[0].column0 == 0);
    CHECK(out[0].column1 == 14);
}

TEST(testNoMatch)
{
    Engine eng;
    CHECK(eng.addRule("FATAL", Action::Notify, "", Ubiquitty::CaseSensitivity::Sensitive));
    Matched out[8];
    CHECK(eng.evaluateLine("everything is fine", { out, 8 }) == 0);
}

TEST(testMultipleRulesReportedInOrder)
{
    Engine eng;
    // Registration order is the report order, regardless of where each
    // matches on the line.
    CHECK(eng.addRule("done$", Action::SetTitle, "Build done", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(eng.addRule("[Ee]rror", Action::Highlight, "", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(eng.addRule("https?://[a-zA-Z0-9./]+", Action::Copy, "", Ubiquitty::CaseSensitivity::Sensitive));

    Matched out[8];
    int n = eng.evaluateLine("error at http://x.io build done", { out, 8 });
    CHECK(n == 3);
    // Rule 0 (done$) reported first even though it matches last on the line.
    CHECK(out[0].ruleIndex == 0);
    CHECK(out[0].action == Action::SetTitle);
    CHECK(std::strcmp(out[0].arg, "Build done") == 0);
    CHECK(out[1].ruleIndex == 1);
    CHECK(out[1].action == Action::Highlight);
    CHECK(out[1].column0 == 0); // "error" at the start
    CHECK(out[1].column1 == 4);
    CHECK(out[2].ruleIndex == 2);
    CHECK(out[2].action == Action::Copy);
    CHECK(out[2].column0 == 9); // "http://x.io"
}

TEST(testRunCommandStubIsClassifiedNotSpawned)
{
    Engine eng;
    CHECK(eng.addRule("deploy [a-z]+", Action::RunCommand, "notify-deploy", Ubiquitty::CaseSensitivity::Sensitive));
    Matched out[4];
    int n = eng.evaluateLine("deploy prod now", { out, 4 });
    CHECK(n == 1);
    CHECK(out[0].action == Action::RunCommand); // engine reports; never spawns
    CHECK(std::strcmp(out[0].arg, "notify-deploy") == 0);
}

TEST(testOutputCapacityClampsReports)
{
    Engine eng;
    CHECK(eng.addRule("a", Action::Highlight, "", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(eng.addRule("b", Action::Highlight, "", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(eng.addRule("c", Action::Highlight, "", Ubiquitty::CaseSensitivity::Sensitive));
    Matched out[2];
    int n = eng.evaluateLine("abc", { out, 2 }); // 3 match, cap 2
    CHECK(n == 2);
    CHECK(out[0].ruleIndex == 0);
    CHECK(out[1].ruleIndex == 1);
}

TEST(testBadRulePatternRejected)
{
    Engine eng;
    CHECK(!eng.addRule("[bad", Action::Notify, "", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(eng.ruleCount() == 0);
}

// ---- smart-selection characterClass expansion -------------------------

TEST(testCharacterClassExpandsPath)
{
    Engine eng;
    CHECK(eng.addCharacterClass("path", "[A-Za-z0-9_./-]+", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(eng.characterClassCount() == 1);

    const char* line = "open /usr/local/bin here";
    int length = static_cast<int>(std::strlen(line));
    // Click inside the path token (column of 'l' in "local").
    const std::optional<MatchSpan> span = eng.expandSelection({ line, static_cast<std::size_t>(length) }, 11);
    CHECK(span);
    CHECK(span && span->begin == 5); // start of "/usr/local/bin"
    CHECK(span && span->end == 18); // last char before the space
    CHECK(span && std::strncmp(line + span->begin, "/usr/local/bin", 14) == 0);
}

TEST(testCharacterClassPicksCoveringClassInOrder)
{
    Engine eng;
    // A narrow word class registered first, a wider path class second.
    CHECK(eng.addCharacterClass("word", "[A-Za-z]+", Ubiquitty::CaseSensitivity::Sensitive));
    CHECK(eng.addCharacterClass("path", "[A-Za-z0-9_./-]+", Ubiquitty::CaseSensitivity::Sensitive));

    const char* line = "ab cd/ef gh";
    int length = static_cast<int>(std::strlen(line));
    // Click on 'c' (column 3). The word class (first) matches "cd" and covers
    // the column, so it wins over the wider path class.
    const std::optional<MatchSpan> span = eng.expandSelection({ line, static_cast<std::size_t>(length) }, 3);
    CHECK(span && span->begin == 3 && span->end == 4); // "cd", not "cd/ef"
}

TEST(testCharacterClassNoCoverLeavesSpanUntouched)
{
    Engine eng;
    CHECK(eng.addCharacterClass("digits", "[0-9]+", Ubiquitty::CaseSensitivity::Sensitive));
    const char* line = "no numbers";
    int length = static_cast<int>(std::strlen(line));
    // No span to leave untouched: an absent optional cannot write one, which is
    // what the two out-parameters could only promise in a comment.
    CHECK(!eng.expandSelection({ line, static_cast<std::size_t>(length) }, 3));
}

UBIQUITTY_TEST_MAIN()
