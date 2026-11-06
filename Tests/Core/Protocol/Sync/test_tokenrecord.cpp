// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_tokenrecord — the on-disk capability-token record format.

#include "Harness.h"

#include "Core/Protocol/Sync/TokenRecord.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using Ubiquitty::Protocol::Sync::parseTokenLine;
using Ubiquitty::Protocol::Sync::tokenExpiry;
using Ubiquitty::Protocol::Sync::TokenExpiry;
using Ubiquitty::Protocol::Sync::tokenExpiryName;
using Ubiquitty::Protocol::Sync::TokenRecord;

const std::string digest(64, 'a');

std::vector<TokenRecord> parseAll(const std::vector<std::string>& lines)
{
    std::vector<TokenRecord> records;
    for (const std::string& line : lines)
        parseTokenLine(line, records);
    return records;
}

TEST(testWellFormedRecord)
{
    const auto records = parseAll({ digest + " 7 1700000000 my label" });
    CHECK_MSG(records.size() == 1, "a well-formed line yields one record");
    CHECK_MSG(records[0].hashHex == digest, "the digest is carried through");
    CHECK_MSG(records[0].scope.toRaw() == 7, "the scope bits are decimal");
    CHECK_MSG(records[0].expiryUnix == 1700000000u, "the expiry is a Unix timestamp");
    CHECK_MSG(records[0].label == "my label", "the label keeps its spaces");
}

// The label is the last field and runs to end of line, so it may contain
// anything the user typed, including the field separator.
TEST(testLabelRunsToEndOfLine)
{
    const auto records = parseAll({ digest + " 1 0 a b  c   d" });
    CHECK_MSG(records.size() == 1, "the line parses");
    CHECK_MSG(records[0].label == "a b  c   d", "internal spacing inside the label is preserved");
}

TEST(testNeverExpires)
{
    const auto records = parseAll({ digest + " 3 0 forever" });
    CHECK_MSG(records[0].expiryUnix == 0, "zero means the token never expires");
}

TEST(testAbsentLabel)
{
    const auto records = parseAll({ digest + " 3 0" });
    CHECK_MSG(records.size() == 1, "a record without a label still parses");
    CHECK_MSG(records[0].label.empty(), "and its label is empty");
}

// Fields are separated by runs of spaces, so a store written with irregular
// spacing stays readable.
TEST(testRunsOfSpacesSeparateFields)
{
    const auto records = parseAll({ "  " + digest + "   5    42     spaced label " });
    CHECK_MSG(records.size() == 1, "leading and repeated spaces do not break the record");
    CHECK_MSG(records[0].scope.toRaw() == 5, "the scope is found past the run");
    CHECK_MSG(records[0].expiryUnix == 42u, "so is the expiry");
    CHECK_MSG(records[0].label == "spaced label ", "the label starts at its first non-space");
}

TEST(testBlanksAndCommentsAreSkipped)
{
    const auto records = parseAll({ "", "   ", "# a comment", "#" + digest + " 1 0 x" });
    CHECK_MSG(records.empty(), "blanks and comments yield no records");
}

// A short, long, or absent digest is malformed: accepting one would let a
// truncated write become a principal.
TEST(testMalformedDigestIsSkipped)
{
    const std::vector<std::string> bad = {
        std::string(63, 'a') + " 1 0 short",
        std::string(65, 'a') + " 1 0 long",
        "1 0 no-digest",
        " 1 0",
    };
    for (const std::string& line : bad) {
        std::vector<TokenRecord> records;
        parseTokenLine(line, records);
        CHECK_MSG(records.empty(), "a malformed digest yields no record");
    }
}

// A damaged line costs its own record and nothing else.
TEST(testOneBadLineDoesNotStopTheRest)
{
    const auto records = parseAll({ digest + " 1 0 first", "garbage", digest + " 2 0 second" });
    CHECK_MSG(records.size() == 2, "the surrounding records still parse");
    CHECK_MSG(records[0].label == "first" && records[1].label == "second", "and keep their order");
}

TEST(testLineTerminatorsAreStripped)
{
    for (const char* terminator : { "\n", "\r\n", "\r", "" }) {
        std::vector<TokenRecord> records;
        parseTokenLine(digest + " 1 0 label" + terminator, records);
        CHECK_MSG(records.size() == 1, "the line parses with any terminator");
        CHECK_MSG(records[0].label == "label", "and the terminator is not part of the label");
    }
}

// Non-numeric scope and expiry read as zero rather than rejecting the record,
// so a store written by a newer version stays usable.
TEST(testNonNumericFieldsReadAsZero)
{
    const auto records = parseAll({ digest + " x y label" });
    CHECK_MSG(records.size() == 1, "the record still parses");
    CHECK_MSG(records[0].scope.toRaw() == 0, "an unreadable scope grants nothing");
    CHECK_MSG(records[0].expiryUnix == 0, "and an unreadable expiry reads as never");
}

// A token lapses *at* its expiry second, not after it. The authorization gate,
// the revocation sweep, and the listings all read this boundary from
// tokenExpiry, so none of them can honor a token another shows as spent.
TEST(testExpiryBoundaryIncludesTheExpirySecond)
{
    const auto records = parseAll({ digest + " 1 100 boundary" });
    CHECK_MSG(records.size() == 1, "the record parses");
    CHECK_MSG(tokenExpiry(records[0], 99) == TokenExpiry::Active, "a second before its expiry the token is still active");
    CHECK_MSG(tokenExpiry(records[0], 100) == TokenExpiry::Expired, "at its expiry second it has lapsed");
    CHECK_MSG(tokenExpiry(records[0], 101) == TokenExpiry::Expired, "and it stays lapsed");
}

TEST(testNeverExpiringRecordIgnoresTheClock)
{
    const auto records = parseAll({ digest + " 1 0 forever" });
    CHECK_MSG(tokenExpiry(records[0], 0) == TokenExpiry::Never, "a zero expiry reads as never");
    CHECK_MSG(tokenExpiry(records[0], ~std::uint64_t { 0 }) == TokenExpiry::Never, "however late the clock reads");
}

// The daemon's `token list` and the app's Agents pane print these words, so the
// mapping is part of the format rather than either surface's presentation.
TEST(testExpiryNames)
{
    CHECK_MSG(std::string(tokenExpiryName(TokenExpiry::Never)) == "never", "a token that never lapses reads never");
    CHECK_MSG(std::string(tokenExpiryName(TokenExpiry::Active)) == "active", "an unlapsed token reads active");
    CHECK_MSG(std::string(tokenExpiryName(TokenExpiry::Expired)) == "expired", "a lapsed token reads expired");
}

} // namespace

UBIQUITTY_TEST_MAIN()
