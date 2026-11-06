// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_row_form.cpp — the Sync primitives, hashes and row form.
//
// The hashes are pinned to the reference implementation's known answers, so a
// vendored update that changed them, or a host and client built against
// different ones, is caught here rather than as a digest mismatch on every
// frame. The primitives refuse every non-canonical or out-of-bounds encoding.
// The row form reproduces each cell byte for byte, says a wide character in
// one unit, and refuses a record whose counts do not add up.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/RowForm.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;

namespace {

std::span<const std::uint8_t> bytesOf(std::string_view text) { return { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }; } // chars as bytes

std::unique_ptr<Ubiquitty::Terminal> terminalWith(std::string_view bytes, int rows = 4, int columns = 20)
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, rows, columns);
    UbiquittyTest::feed(*terminal, bytes);
    return terminal;
}

std::vector<Ubiquitty::Cell> rowOf(const Ubiquitty::Terminal& terminal, int row)
{
    std::vector<Ubiquitty::Cell> cells(static_cast<std::size_t>(terminal.columns));
    terminal.grid->readRow(Ubiquitty::Row(row), cells.data());
    return cells;
}

RowReferences everythingResolves()
{
    return { .clusters = std::vector<bool>(1u << 15, true) };
}

// Encodes and decodes a row, returning whether every cell came back identical.
bool roundTrips(const Ubiquitty::Terminal& terminal, std::span<const Ubiquitty::Cell> cells, std::vector<std::uint8_t>* recordOut = nullptr)
{
    PaneDictionaries dictionaries;
    std::vector<std::uint8_t> record;
    if (!encodeRowRecord(terminal, cells, { .lineAttribute = 1, .isWrapped = true, .mark = 2 }, dictionaries, RunTextDefinitions::Present, record))
        return false;
    std::vector<Ubiquitty::Cell> decoded(cells.size());
    RowAttributes attributes;
    WireReader reader(record);
    if (!decodeRowRecord(reader, dictionaries.pens(), everythingResolves(), decoded, attributes) || !reader.isAtEnd())
        return false;
    if (recordOut)
        *recordOut = record;
    return attributes == RowAttributes { .lineAttribute = 1, .isWrapped = true, .mark = 2 }
    && std::memcmp(decoded.data(), cells.data(), cells.size() * sizeof(Ubiquitty::Cell)) == 0;
}

bool decodes(std::span<const std::uint8_t> record, std::span<const Ubiquitty::Pen> pens, std::size_t width)
{
    std::vector<Ubiquitty::Cell> cells(width);
    RowAttributes attributes;
    WireReader reader(record);
    return decodeRowRecord(reader, pens, everythingResolves(), cells, attributes) && reader.isAtEnd();
}

} // namespace

TEST(theHashesAreXXH3)
{
    CHECK_EQ(rowHash({}), 0x2D06800538D394C2ull);
    CHECK_EQ(rowHash(bytesOf("ubiquitty")), 0xBBA0880283A52425ull);
    CHECK_EQ(blockHash(bytesOf("ubiquitty")), 0xBBA0880283A52425ull);
    CHECK(digestOf({}) == (Digest128 { .low = 0x6001C324468D497Full, .high = 0x99AA06D3014798D8ull }));
    CHECK(digestOf(bytesOf("ubiquitty")) == (Digest128 { .low = 0x0D4BB630B00FFC78ull, .high = 0x63C5EBA092200381ull }));
}

TEST(varintsRoundTripAndRefuseNonCanonicalForms)
{
    const std::uint64_t values[] = { 0, 1, 127, 128, 300, 0xFFFFFFFFull, ~std::uint64_t { 0 } };
    for (const std::uint64_t value : values) {
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writer.varint(value);
        WireReader reader(bytes);
        CHECK_EQ(reader.varint(), value);
        CHECK(reader.ok() && reader.isAtEnd());
    }
    const std::int64_t signedValues[] = { 0, -1, 1, -64, 64, INT64_MIN, INT64_MAX };
    for (const std::int64_t value : signedValues) {
        std::vector<std::uint8_t> bytes;
        WireWriter writer(bytes);
        writer.svarint(value);
        WireReader reader(bytes);
        CHECK_EQ(reader.svarint(), value);
        CHECK(reader.ok());
    }
    const std::vector<std::vector<std::uint8_t>> refused = {
        { 0x80, 0x00 }, // zero spelled in two bytes
        { 0xFF, 0x00 }, // 127 with a trailing zero group
        { 0x80 }, // cut short
        { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02 }, // past 64 bits
        { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 }, // eleven bytes
    };
    for (const auto& bytes : refused) {
        WireReader reader(bytes);
        (void)reader.varint();
        CHECK(!reader.ok());
    }
    const std::uint8_t tooLarge[] = { 0x81, 0x01 };
    WireReader bounded(tooLarge);
    (void)bounded.varint(128);
    CHECK(!bounded.ok());
}

TEST(aCountCannotPromiseMoreThanTheBodyHolds)
{
    const std::uint8_t body[] = { 0x05, 0x00, 0x00 };
    WireReader reader(body);
    (void)reader.count(100, 1);
    CHECK(!reader.ok());
    WireReader withinBody(body);
    CHECK_EQ(withinBody.count(100, 0), 5u);
    CHECK(withinBody.ok());
}

TEST(utf8IsValidatedStrictly)
{
    CHECK(isValidUTF8(bytesOf(std::string_view("\0a\xc3\xa9\xf4\x8f\xbf\xbf", 8))));
    CHECK(!isValidUTF8(bytesOf("\xc0\x80"))); // overlong NUL
    CHECK(!isValidUTF8(bytesOf("\xed\xa0\x80"))); // surrogate
    CHECK(!isValidUTF8(bytesOf("\xf4\x90\x80\x80"))); // past U+10FFFF
    CHECK(!isValidUTF8(bytesOf("\xe2\x82"))); // cut short
    CHECK(!isValidUTF8(bytesOf("\x80"))); // a continuation alone
}

TEST(rowsRoundTripByteForByte)
{
    const std::string_view rows[] = {
        "plain ascii",
        "\x1b[1;4;38;2;10;20;30mrgb\x1b[0m \x1b[7mreverse",
        "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97 cjk",
        "e\xcc\x81 combining",
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7 family",
        "a\x1b[5Cb", // cells never written between two that were
        "\x1b[42m\x1b[K", // an erase carrying a background colour
        "\x1b]8;;https://example.com/\x1b\\link\x1b]8;;\x1b\\",
        "\x1b(0lqqk\x1b(B", // DEC special graphics
    };
    for (const std::string_view bytes : rows) {
        auto terminal = terminalWith(bytes);
        CHECK_MSG(roundTrips(*terminal, rowOf(*terminal, 0)), ("a row written by \"" + std::string(bytes) + "\" did not round-trip").c_str());
    }
}

// A wide character is one unit: one code point of text and, across a row of
// them, one pen run, though its two cells carry different pens.
TEST(aRowOfWideCharactersIsOneRun)
{
    auto terminal = terminalWith("\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad");
    std::vector<std::uint8_t> record;
    CHECK(roundTrips(*terminal, rowOf(*terminal, 0), &record));
    // Three row facts, the extent, the text's length and twelve bytes of it,
    // one run of two varints behind its count, and four empty lists.
    CHECK_EQ(record.size(), 3u + 1 + 1 + 18 + 3 + 4);
}

// A wide lead whose partner is not the continuation the Core writes keeps
// the partner as a unit of its own, so the odd pair still round-trips.
TEST(aWideLeadWithAnUnusualPartnerIsKept)
{
    auto terminal = terminalWith("\xe4\xb8\xad");
    std::vector<Ubiquitty::Cell> cells = rowOf(*terminal, 0);
    cells[1].pen.foregroundIndex = 3;
    CHECK(roundTrips(*terminal, cells));
}

// Trailing canonical blanks cost nothing.
TEST(trailingBlanksAreNotWritten)
{
    auto narrow = terminalWith("x", 1, 10);
    auto wide = terminalWith("x", 1, 200);
    PaneDictionaries dictionaries;
    std::vector<std::uint8_t> narrowRecord;
    std::vector<std::uint8_t> wideRecord;
    CHECK(encodeRowRecord(*narrow, rowOf(*narrow, 0), {}, dictionaries, RunTextDefinitions::Present, narrowRecord));
    CHECK(encodeRowRecord(*wide, rowOf(*wide, 0), {}, dictionaries, RunTextDefinitions::Present, wideRecord));
    CHECK(narrowRecord == wideRecord);
}

// Every count is checked against what it must agree with.
TEST(aMalformedRecordIsRefused)
{
    auto terminal = terminalWith("abc");
    PaneDictionaries dictionaries;
    std::vector<std::uint8_t> record;
    CHECK(encodeRowRecord(*terminal, rowOf(*terminal, 0), {}, dictionaries, RunTextDefinitions::Present, record));
    CHECK(decodes(record, dictionaries.pens(), 20));
    // The extent past the row's width.
    CHECK(!decodes(record, dictionaries.pens(), 2));
    // A pen the dictionary never defined.
    CHECK(!decodes(record, {}, 20));
    // Bytes after the record.
    std::vector<std::uint8_t> trailing = record;
    trailing.push_back(0);
    CHECK(!decodes(trailing, dictionaries.pens(), 20));
    // Every prefix.
    for (std::size_t length = 0; length < record.size(); ++length)
        CHECK(!decodes({ record.data(), length }, dictionaries.pens(), 20));

    // Hand-built: three row facts, extent 2, text "ab", one run of three
    // units over two code points.
    const std::uint8_t overrun[] = { 0, 0, 0, 2, 2, 'a', 'b', 1, 3, 0, 0, 0, 0, 0 };
    CHECK(!decodes(overrun, dictionaries.pens(), 20));
    // A wide-kept entry on a unit whose pen is not wide.
    const std::uint8_t kept[] = { 0, 0, 0, 1, 1, 'a', 1, 1, 0, 0, 0, 0, 1, 0 };
    CHECK(!decodes(kept, dictionaries.pens(), 20));
    // A flags byte with a bit no row fact owns.
    const std::uint8_t flags[] = { 0, 2, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(!decodes(flags, dictionaries.pens(), 20));
}

// A pool cluster travels by the host's id, and a row naming one that the
// receiver does not hold is refused.
//
// The pool can hold more than the lead's cluster: the last graphic of a write
// stays cancellable by CCH, so its final code point is appended on its own and
// the prefix it extends is interned as well. The record names the lead's entry.
TEST(aClusterTheReceiverDoesNotHoldIsRefused)
{
    auto terminal = terminalWith("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7");
    PaneDictionaries dictionaries;
    std::vector<std::uint8_t> record;
    const std::vector<Ubiquitty::Cell> row = rowOf(*terminal, 0);
    CHECK(encodeRowRecord(*terminal, row, {}, dictionaries, RunTextDefinitions::Present, record));
    const std::uint16_t clusterID = row[0].clusterID();
    const std::array<std::uint32_t, 5> family { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467 };
    const std::span<const std::uint32_t> sequence = terminal->clusterPool.sequence(clusterID);
    CHECK(std::equal(sequence.begin(), sequence.end(), family.begin(), family.end()));
    CHECK(clusterID <= dictionaries.clusterCount());

    std::vector<Ubiquitty::Cell> cells(20);
    RowAttributes attributes;
    RowReferences lead { .clusters = std::vector<bool>(1u << 15, false) };
    lead.clusters[clusterID] = true;
    WireReader held(record);
    CHECK(decodeRowRecord(held, dictionaries.pens(), lead, cells, attributes));
    lead.clusters[clusterID] = false;
    WireReader reader(record);
    CHECK(!decodeRowRecord(reader, dictionaries.pens(), lead, cells, attributes));
}

// The pens a dictionary defines are numbered from zero, once each.
TEST(aPenIsDefinedOnce)
{
    PaneDictionaries dictionaries;
    Ubiquitty::Pen red {};
    Ubiquitty::penReset(&red);
    red.foregroundIndex = 1;
    Ubiquitty::Pen blue = red;
    blue.foregroundIndex = 4;
    CHECK_EQ(dictionaries.penID(red), 0u);
    CHECK_EQ(dictionaries.penID(blue), 1u);
    CHECK_EQ(dictionaries.penID(red), 0u);
    CHECK_EQ(dictionaries.pens().size(), 2u);
    const std::uint32_t epoch = dictionaries.epoch();
    dictionaries.roll();
    CHECK_EQ(dictionaries.epoch(), epoch + 1);
    CHECK(dictionaries.pens().empty());
}

// Every byte of a pen crosses the wire and comes back: the attributes, the
// three colour indices, nine colour bytes, the underline style byte, the
// extended attributes with their weight and spec subfields, and the font
// byte. Drawn at random so no bit pattern is spared.
//
// The word that packs the underline index with the semantic role is the one
// exception: the wire refuses an index past colorRGB, a role past Input and a
// reserved bit (test_sync_checkpoint's penWireRejectsUnknownSemanticRolesAndReservedBits),
// so each draw is folded into the range a pen can hold.
TEST(aPenRoundTripsEveryByte)
{
    std::uint64_t state = 0x6A09E667F3BCC909ull;
    const auto next = [&state] {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    };
    constexpr unsigned semanticRoleCount = static_cast<unsigned>(Ubiquitty::SemanticRole::Input) + 1;
    int mismatches = 0;
    for (int round = 0; round < 4000; ++round) {
        std::array<std::uint8_t, sizeof(Ubiquitty::Pen)> bytes {};
        for (std::uint8_t& byte : bytes)
            byte = static_cast<std::uint8_t>(next() & 0xFF);
        auto pen = std::bit_cast<Ubiquitty::Pen>(bytes);
        pen.underlineIndex = static_cast<std::uint16_t>(pen.underlineIndex % (Ubiquitty::colorRGB + 1));
        pen.semanticRole = static_cast<Ubiquitty::SemanticRole>(static_cast<unsigned>(pen.semanticRole) % semanticRoleCount);
        pen.underlineReserved = 0;
        std::vector<std::uint8_t> wire;
        WireWriter writer(wire);
        writePen(writer, pen);
        if (wire.size() != penWireBytes) {
            ++mismatches;
            continue;
        }
        WireReader reader(wire);
        const Ubiquitty::Pen back = readPen(reader);
        if (!reader.ok() || !reader.isAtEnd() || std::memcmp(&back, &pen, sizeof(Ubiquitty::Pen)) != 0)
            ++mismatches;
    }
    CHECK_EQ(mismatches, 0);
}

UBIQUITTY_TEST_MAIN()
