// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_history_plane.cpp — a history line on the wire (Docs/sync.md
// §6.6, §7.5): the rows of one soft-wrapped line joined with no width, in a
// body that carries the pens, links and clusters it names, and a hash that
// does not depend on the width the line was wrapped at.
//
// - The hash of a line is the same at every width the host reflows to.
// - A body decodes on its own: a line whose pen was defined by an update the
//   client never saw still decodes, because the body carries the pen.
// - A body is refused whole, before anything is allocated or applied, when it
//   names an entry its tables do not hold, claims more lines than the limit,
//   or carries a line whose hash does not match its cells.
// - A line too long for the cell cap travels truncated and says so.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellContent.h"
#include "Core/CellGrid.h"
#include "Core/ClusterPool.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/RowMeta.h"
#include "Core/RunTextStore.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/TextSizeTable.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::feed;

namespace {

std::unique_ptr<Ubiquitty::Terminal> terminalOf(int rows, int columns)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

// A line of three rows at 80 columns whose wide character cannot fit the first
// row's last column, so the first row ends on the pad the Core forces.
std::string wideAtTheMarginLine()
{
    return std::string(79, 'a') + "\xe4\xb8\x96" + std::string(100, 'b') + "\x1b[1mbold\x1b[0m";
}

// Writes `line`, then enough blank lines to push it off a screen of `rows`.
void writeAndScrollOff(Ubiquitty::Terminal& t, const std::string& line, int rows)
{
    feed(t, "lead\r\n" + line + "\r\n");
    for (int index = 0; index < rows; ++index)
        feed(t, "\r\n");
}

std::string textOf(const std::vector<Ubiquitty::Cell>& cells)
{
    std::string text;
    for (const Ubiquitty::Cell& cell : cells) {
        if (cell.codePoint >= 0x20 && cell.codePoint < 0x7F)
            text.push_back(static_cast<char>(cell.codePoint));
        else if (cell.codePoint > 0x7F)
            text.push_back('?');
    }
    return text;
}

std::vector<DecodedLine> roundTrip(const Ubiquitty::Terminal& t, HistoryPublisher& publisher, LineRange range, HistoryOrder order = HistoryOrder::Ascending)
{
    std::vector<std::uint8_t> body;
    const std::size_t written = publisher.writeBody(t, range, order, maxHistoryTransferBytes, body);
    std::vector<DecodedLine> lines;
    const std::uint64_t first = order == HistoryOrder::Ascending ? range.from : range.to - 1;
    if (written == 0 || !decodeHistoryBody(body, first, order, historyBodyLimits, lines))
        return {};
    return lines;
}

struct HistoryTestNames {
    std::uint64_t pen(const Ubiquitty::Pen&) { return 0; }
    std::uint64_t link(std::uint16_t id) { return id; }
    std::optional<std::uint64_t> cluster(const Ubiquitty::Cell& cell) { return cell.clusterID(); }
};

enum class HistoryTestIDs : std::uint8_t {
    Omitted,
    Included,
};

enum class HistoryTestWrapping : std::uint8_t {
    SeparateLines,
    JoinedLine,
};

enum class HistoryTestEncoding : std::uint8_t {
    UTF8,
    Latin1,
};

std::optional<DecodedLine> linkedHistoryLine(std::string_view key, std::string_view uri, HistoryTestEncoding encoding)
{
    auto terminal = terminalOf(4, 40);
    feed(*terminal, encoding == HistoryTestEncoding::UTF8 ? "\x1b%G" : "\x1b%@");
    std::string bytes = "\x1b]8;";
    if (!key.empty()) {
        bytes += "id=";
        bytes.append(key);
    }
    bytes += ';';
    bytes.append(uri);
    bytes += "\x1b\\x\x1b]8;;\x1b\\\r\n\r\n\r\n\r\n\r\n\r\n";
    feed(*terminal, bytes);
    HistoryPublisher publisher;
    std::vector<std::uint8_t> body;
    if (!publisher.capture(*terminal) || publisher.writeBody(*terminal, { 0, 1 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body) != 1)
        return std::nullopt;
    std::vector<DecodedLine> decoded;
    if (!decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, decoded) || decoded.size() != 1)
        return std::nullopt;
    return std::move(decoded.front());
}

bool appendDistinctClusterHistory(Ubiquitty::Terminal& terminal, std::size_t count, HistoryTestWrapping wrapping)
{
    for (std::size_t index = 0; index < count;) {
        std::vector<Ubiquitty::Cell> cells(static_cast<std::size_t>(terminal.columns), Ubiquitty::defaultBlankCell);
        for (std::size_t column = 0; column < cells.size() && index < count; ++column, ++index) {
            const auto value = static_cast<std::uint32_t>(index);
            const std::array<std::uint32_t, 5> sequence { 'a', 0x300 + (value & 15), 0x300 + ((value >> 4) & 15), 0x300 + ((value >> 8) & 15), 0x300 + ((value >> 12) & 15) };
            const auto cluster = terminal.clusterPool.intern(sequence);
            if (!cluster)
                return false;
            auto& cell = cells[column];
            cell.codePoint = sequence[0];
            cell.combining[0] = sequence[1];
            cell.setCombiningLength(1);
            cell.setClusterID(cluster);
        }
        terminal.grid->setRowSpan(Ubiquitty::Row(0), Ubiquitty::Column(0), { cells.data(), terminal.columns });
        terminal.grid->setRowWrapped(0, wrapping == HistoryTestWrapping::JoinedLine && index < count);
        Ubiquitty::Screen::scrollbackAppendGridRow(terminal, 0);
    }
    return true;
}

std::optional<Ubiquitty::Cell> sharedHistoryRun(Ubiquitty::Terminal& terminal, std::span<const std::uint32_t> text)
{
    Ubiquitty::TextSizeSpec spec;
    spec.isProportional = 1;
    spec.runWidthCells = 1;
    const auto specID = terminal.textSizeTable.intern(spec);
    const auto textID = terminal.runTextStore.intern(text);
    if (!specID || !textID || text.empty())
        return std::nullopt;
    auto cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = text.front();
    cell.pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
    cell.pen.extendedAttributes.setTextSizeSpecID(specID);
    cell.setClusterID(textID);
    return cell;
}

void appendRepeatedHistoryRow(Ubiquitty::Terminal& terminal, std::span<const Ubiquitty::Cell> cells, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index) {
        terminal.grid->setRowSpan(Ubiquitty::Row(0), Ubiquitty::Column(0), { cells.data(), static_cast<int>(cells.size()) });
        terminal.grid->setRowWrapped(0, false);
        Ubiquitty::Screen::scrollbackAppendGridRow(terminal, 0);
    }
}

std::vector<std::uint8_t> repeatedHistoryBody(std::span<const Ubiquitty::Cell> cells, std::size_t count, std::string_view key = {}, std::string_view uri = {}, std::span<const std::uint32_t> cluster = {}, std::uint8_t flags = 0)
{
    const auto& pen = cells.empty() ? Ubiquitty::defaultBlankCell.pen : cells.front().pen;
    const auto writeTables = [&](WireWriter& writer, HistoryTestIDs ids) {
        writer.varint(1);
        writePen(writer, pen);
        writer.varint(uri.empty() ? 0 : 1);
        if (!uri.empty()) {
            if (ids == HistoryTestIDs::Included)
                writer.u16(1);
            writer.bytes({ reinterpret_cast<const std::uint8_t*>(key.data()), key.size() });
            writer.bytes({ reinterpret_cast<const std::uint8_t*>(uri.data()), uri.size() });
        }
        writer.varint(cluster.empty() ? 0 : 1);
        if (!cluster.empty()) {
            if (ids == HistoryTestIDs::Included)
                writer.u16(1);
            writer.varint(cluster.size());
            for (const auto codePoint : cluster)
                writer.varint(codePoint);
        }
    };
    HistoryTestNames names;
    std::vector<std::uint8_t> row;
    WireWriter rowWriter(row);
    if (!Ubiquitty::Screen::writeRowForm(cells, names, rowWriter))
        return {};
    std::vector<std::uint8_t> canonical;
    WireWriter canonicalWriter(canonical);
    canonicalWriter.u8(0);
    canonicalWriter.raw(row);
    writeTables(canonicalWriter, HistoryTestIDs::Omitted);
    const auto hash = blockHash(canonical);
    std::vector<std::uint8_t> body;
    WireWriter writer(body);
    writeTables(writer, HistoryTestIDs::Included);
    writer.varint(count);
    for (std::size_t index = 0; index < count; ++index) {
        writer.u8(flags);
        writer.u8(0);
        writer.u8(0);
        writer.u8(0);
        writer.raw(row);
        writer.u64(hash);
    }
    return body;
}

} // namespace

// A line's hash names its content, not the rows a width cut
// it into. Reflowing the host from 80 to 53 columns re-wraps the line into
// other rows, with the pad before the wide character gone, and the hash
// stands; a terminal that wrote the line at 53 columns agrees.
TEST(aLogicalLineHashesTheSameAtEveryWidth)
{
    auto t80 = terminalOf(10, 80);
    writeAndScrollOff(*t80, wideAtTheMarginLine(), 10);
    const std::uint64_t number = 1;
    const std::optional<std::uint64_t> at80 = logicalLineHash(*t80, number);
    REQUIRE(at80.has_value());

    Ubiquitty::resize(*t80, 10, 53);
    CHECK(logicalLineHash(*t80, number) == at80);

    auto t53 = terminalOf(10, 53);
    writeAndScrollOff(*t53, wideAtTheMarginLine(), 10);
    CHECK(logicalLineHash(*t53, number) == at80);

    // And another line hashes otherwise.
    CHECK(logicalLineHash(*t80, 0) != at80);
}

TEST(aThreeRowLineDecodesToOneLineEqualToTheJoinedHostRows)
{
    auto t = terminalOf(10, 80);
    writeAndScrollOff(*t, wideAtTheMarginLine(), 10);
    REQUIRE(Ubiquitty::Screen::logicalLineRows(*t, 1)->rowCount == 3);
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    const std::vector<DecodedLine> lines = roundTrip(*t, publisher, { 0, 2 });
    REQUIRE(lines.size() == 2);
    CHECK_EQ(lines[1].number, 1u);
    const std::optional<JoinedLine> joined = joinedLine(*t, 1);
    REQUIRE(joined.has_value());
    CHECK_EQ(lines[1].cells.size(), joined->cells.size());
    CHECK_STR_EQ(textOf(lines[1].cells), textOf(joined->cells));
    CHECK_EQ(lines[1].lineHash, *logicalLineHash(*t, 1));
    CHECK_STR_EQ(textOf(lines[0].cells), "lead");

    // Descending order lists the newest first.
    const std::vector<DecodedLine> descending = roundTrip(*t, publisher, { 0, 2 }, HistoryOrder::Descending);
    REQUIRE(descending.size() == 2);
    CHECK_EQ(descending[0].number, 1u);
    CHECK_EQ(descending[1].number, 0u);
}

TEST(aDECDWLLineKeepsItsFlag)
{
    auto t = terminalOf(6, 40);
    feed(*t, "\x1b#6wide\r\n\x1b#5narrow\r\n");
    for (int index = 0; index < 6; ++index)
        feed(*t, "\r\n");
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    const std::vector<DecodedLine> lines = roundTrip(*t, publisher, { 0, 2 });
    REQUIRE(lines.size() == 2);
    CHECK_EQ(lines[0].lineAttribute, Ubiquitty::lineAttributeDoubleWidth);
    CHECK_EQ(lines[1].lineAttribute, Ubiquitty::lineAttributeNormal);
}

// The body carries its own pens: a line written in a colour defined by an
// update this client never saw decodes on its own, with its pen.
TEST(aLineScrolledInTheFrameThatDefinedItsPenDecodesAlone)
{
    auto t = terminalOf(4, 40);
    feed(*t, "\x1b[38;2;10;20;30mtinted\x1b[0m\r\n");
    for (int index = 0; index < 4; ++index)
        feed(*t, "\r\n");
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    const std::vector<DecodedLine> lines = roundTrip(*t, publisher, { 0, 1 });
    REQUIRE(lines.size() == 1);
    REQUIRE(!lines[0].cells.empty());
    CHECK_EQ(lines[0].cells[0].pen.foregroundRed, 10);
    CHECK_EQ(lines[0].cells[0].pen.foregroundGreen, 20);
    CHECK_EQ(lines[0].cells[0].pen.foregroundBlue, 30);
}

TEST(aLineStillOnScreenIsNotEncodable)
{
    auto t = terminalOf(4, 40);
    feed(*t, "one\r\ntwo\r\nthree\r\nfour\r\nfive");
    const Ubiquitty::Screen::HistoryAddress address = Ubiquitty::Screen::historyAddress(*t);
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    std::vector<std::uint8_t> body;
    CHECK_EQ(publisher.writeBody(*t, { address.head, address.head + 1 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body), std::size_t { 0 });
    CHECK(body.empty());
    CHECK(!logicalLineHash(*t, address.head));
    // The range is cut to what is history.
    CHECK_EQ(publisher.writeBody(*t, { 0, address.head + 5 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body), static_cast<std::size_t>(address.head));
}

TEST(aBodyNamingAPenBeyondItsTableIsRefusedUntouched)
{
    // No pens, one line whose one unit names pen 0.
    std::vector<std::uint8_t> body;
    WireWriter writer(body);
    writer.varint(0); // pens
    writer.varint(0); // links
    writer.varint(0); // clusters
    writer.varint(1); // lines
    writer.u8(0); // flags
    writer.u8(0); // lineAttribute
    writer.u8(0); // wrapped
    writer.u8(0); // mark
    writer.varint(1); // extent
    writer.varint(1);
    writer.u8('x');
    writer.varint(1); // runs
    writer.varint(1);
    writer.varint(0);
    writer.varint(0); // links
    writer.varint(0); // clusters
    writer.varint(0); // cluster references
    writer.varint(0); // kept wide leads
    writer.u64(0);
    std::vector<DecodedLine> lines(1);
    lines[0].number = 77;
    CHECK(!decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, lines));
    REQUIRE(lines.size() == 1);
    CHECK_EQ(lines[0].number, 77u);
}

TEST(aCountBeyondTheLimitIsRefusedBeforeAllocation)
{
    std::vector<std::uint8_t> body;
    WireWriter writer(body);
    writer.varint(0);
    writer.varint(0);
    writer.varint(0);
    writer.varint(historyBodyLimits.maxLines + 1);
    std::vector<DecodedLine> lines;
    CHECK(!decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, lines));
    CHECK(lines.empty());
    CHECK_EQ(lines.capacity(), std::size_t { 0 });

    // A pen table longer than the body can hold is refused at its count.
    body.clear();
    writer.varint(1'000'000);
    CHECK(!decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, lines));
}

TEST(aForgedLineHashIsRefused)
{
    auto t = terminalOf(4, 40);
    feed(*t, "honest\r\n");
    for (int index = 0; index < 4; ++index)
        feed(*t, "\r\n");
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    std::vector<std::uint8_t> body;
    REQUIRE(publisher.writeBody(*t, { 0, 1 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body) == 1);
    std::vector<DecodedLine> lines;
    REQUIRE(decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, lines));
    body.back() ^= 0x01;
    lines.clear();
    CHECK(!decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, lines));
    CHECK(lines.empty());
}

TEST(anOverlongLineTravelsTruncatedAndFlagged)
{
    auto t = terminalOf(4, 200);
    Ubiquitty::setScrollbackLimit(t.get(), 1'000);
    feed(*t, std::string(maxHistoryLineCells + 500, 'x') + "\r\n");
    for (int index = 0; index < 4; ++index)
        feed(*t, "\r\n");
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    const std::vector<DecodedLine> lines = roundTrip(*t, publisher, { 0, 1 });
    REQUIRE(lines.size() == 1);
    CHECK_EQ(lines[0].cells.size(), maxHistoryLineCells);
    CHECK((lines[0].flags & historyLineTruncated) != 0);
}

// The publisher keeps the hash of every line a pull-back may withdraw, so a
// line that comes back unchanged costs nothing to re-advance over.
TEST(thePublisherKeepsTheHashesAPullBackMayWithdraw)
{
    auto t = terminalOf(9, 80);
    for (int index = 0; index < 40; ++index)
        feed(*t, "line " + std::to_string(index) + "\r\n");
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    const std::uint64_t head = publisher.address().head;
    const std::optional<std::uint64_t> newest = publisher.publishedHash(head - 1);
    REQUIRE(newest.has_value());
    CHECK(newest == logicalLineHash(*t, head - 1));

    Ubiquitty::resize(*t, 12, 80);
    REQUIRE(publisher.capture(*t));
    CHECK_EQ(publisher.address().head, head - 3);
    CHECK(publisher.publishedHash(head - 1) == newest);
}

TEST(sharedHistoryDictionariesCannotExpandPastTheDecodedAllowance)
{
    Ubiquitty::Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = 'x';
    cell.setLinkID(1);
    cell.setClusterID(1);
    const std::array<std::uint32_t, 16> cluster { 'x', 0x301, 0x302, 0x303 };
    const auto body = repeatedHistoryBody(std::span(&cell, 1), 32, "id", std::string(1'024, 'u'), cluster);
    std::vector<DecodedLine> decoded;
    REQUIRE(decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, decoded));
    REQUIRE(decoded.size() == 32);
    CHECK_EQ(decoded.back().links.front().uri.size(), std::size_t { 1'024 });
    CHECK_EQ(decoded.back().clusters.front().size(), cluster.size());

    auto limits = historyBodyLimits;
    limits.maxDecodedBytes = 32 * 1'024;
    std::vector<DecodedLine> unchanged(1);
    unchanged.front().number = 77;
    CHECK(!decodeHistoryBody(body, 0, HistoryOrder::Ascending, limits, unchanged));
    REQUIRE(unchanged.size() == 1);
    CHECK_EQ(unchanged.front().number, std::uint64_t { 77 });

    std::size_t lower = 0;
    std::size_t upper = 128 * 1'024;
    while (lower < upper) {
        limits.maxDecodedBytes = lower + (upper - lower) / 2;
        decoded.clear();
        if (decodeHistoryBody(body, 0, HistoryOrder::Ascending, limits, decoded))
            upper = limits.maxDecodedBytes;
        else
            lower = limits.maxDecodedBytes + 1;
    }
    limits.maxDecodedBytes = upper;
    decoded.clear();
    REQUIRE(decodeHistoryBody(body, 0, HistoryOrder::Ascending, limits, decoded));
    CHECK_EQ(decoded.size(), std::size_t { 32 });
    limits.maxDecodedBytes = upper - 1;
    CHECK(!decodeHistoryBody(body, 0, HistoryOrder::Ascending, limits, unchanged));
    CHECK_EQ(unchanged.front().number, std::uint64_t { 77 });
}

TEST(packedWideHistoryRowsAreChargedForTheirExpandedCells)
{
    Ubiquitty::Cell lead = Ubiquitty::defaultBlankCell;
    lead.codePoint = 0x4E00;
    lead.pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
    std::vector<Ubiquitty::Cell> cells;
    for (int index = 0; index < 128; ++index) {
        cells.push_back(lead);
        cells.push_back(Ubiquitty::Screen::derivedContinuation(lead));
    }
    const auto body = repeatedHistoryBody(cells, 16);
    std::vector<DecodedLine> decoded;
    REQUIRE(decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK_EQ(decoded.back().cells.size(), cells.size());
    auto limits = historyBodyLimits;
    limits.maxDecodedBytes = 192 * 1'024;
    decoded.clear();
    CHECK(!decodeHistoryBody(body, 0, HistoryOrder::Ascending, limits, decoded));
    CHECK(decoded.empty());
}

TEST(historyLinksRespectTheHostsStringBounds)
{
    Ubiquitty::Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = 'x';
    cell.setLinkID(1);
    std::vector<DecodedLine> decoded;
    const auto cells = std::span(&cell, 1);
    REQUIRE(decodeHistoryBody(repeatedHistoryBody(cells, 1, std::string(Ubiquitty::VT::maxLinkKeyLength - 1, 'k'), std::string(Ubiquitty::VT::maxLinkURILength - 1, 'u')), 0, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK(!decodeHistoryBody(repeatedHistoryBody(cells, 1, std::string(Ubiquitty::VT::maxLinkKeyLength, 'k'), "u"), 0, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK(!decodeHistoryBody(repeatedHistoryBody(cells, 1, {}, std::string(Ubiquitty::VT::maxLinkURILength, 'u')), 0, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK_EQ(decoded.size(), std::size_t { 1 });
}

TEST(historyLineNumberArithmeticCannotWrap)
{
    Ubiquitty::Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = 'x';
    const auto body = repeatedHistoryBody(std::span(&cell, 1), 2);
    std::vector<DecodedLine> decoded;
    CHECK(!decodeHistoryBody(body, std::numeric_limits<std::uint64_t>::max(), HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK(decoded.empty());
    CHECK(!decodeHistoryBody(body, std::numeric_limits<std::uint64_t>::max() - 1, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK(!decodeHistoryBody(body, std::numeric_limits<std::uint64_t>::max(), HistoryOrder::Descending, historyBodyLimits, decoded));
    REQUIRE(decodeHistoryBody(body, std::numeric_limits<std::uint64_t>::max() - 2, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK_EQ(decoded.back().number, std::numeric_limits<std::uint64_t>::max() - 1);
    const auto open = repeatedHistoryBody(std::span(&cell, 1), 1, {}, {}, {}, historyLineOpen);
    REQUIRE(decodeHistoryBody(open, std::numeric_limits<std::uint64_t>::max(), HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK(decoded.back().flags & historyLineOpen);
}

TEST(theHistoryPublisherSplitsAtTheReceiversDecodedAllowance)
{
    auto t = terminalOf(4, 40);
    Ubiquitty::setScrollbackLimit(t.get(), 64);
    feed(*t, "\x1b]8;;" + std::string(1'024, 'u') + "\x1b\\");
    for (int index = 0; index < 32; ++index)
        feed(*t, "x\r\n");
    feed(*t, "\x1b]8;;\x1b\\\r\n\r\n\r\n\r\n");
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    auto limits = historyBodyLimits;
    limits.maxDecodedBytes = 16 * 1'024;
    std::size_t bodies = 0;
    for (std::uint64_t from = 0; from < 32;) {
        std::vector<std::uint8_t> body;
        const auto count = publisher.writeBody(*t, { from, 32 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body, limits.maxDecodedBytes);
        REQUIRE(count > 0);
        std::vector<DecodedLine> decoded;
        REQUIRE(decodeHistoryBody(body, from, HistoryOrder::Ascending, limits, decoded));
        CHECK_EQ(decoded.size(), count);
        from += count;
        ++bodies;
    }
    CHECK(bodies > 1);
}

TEST(aHistoryBodysLargestRowFitsBesideEveryLaterRowHeader)
{
    auto t = terminalOf(4, 80);
    Ubiquitty::setScrollbackLimit(t.get(), 128);
    feed(*t, std::string(64, 'x') + "\r\n");
    for (int index = 0; index < 64; ++index)
        feed(*t, "\r\n");
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    const auto head = publisher.address().head;
    REQUIRE(head > 32);
    auto limits = historyBodyLimits;
    limits.maxDecodedBytes = 40 * 1'024;
    std::size_t bodies = 0;
    for (std::uint64_t from = 0; from < head;) {
        std::vector<std::uint8_t> body;
        const auto count = publisher.writeBody(*t, { from, head }, HistoryOrder::Ascending, maxHistoryTransferBytes, body, limits.maxDecodedBytes);
        REQUIRE(count > 0);
        std::vector<DecodedLine> decoded;
        REQUIRE(decodeHistoryBody(body, from, HistoryOrder::Ascending, limits, decoded));
        CHECK_EQ(decoded.size(), count);
        if (!from)
            CHECK_EQ(decoded.front().cells.size(), std::size_t { 64 });
        from += count;
        ++bodies;
    }
    CHECK(bodies > 1);
}

TEST(emptyHistoryClusterReferencesIncludeTheirStorageWord)
{
    const auto body = repeatedHistoryBody({}, 0);
    auto limits = historyBodyLimits;
    limits.maxDecodedBytes = sizeof(Ubiquitty::Pen) + 1;
    std::vector<DecodedLine> decoded;
    CHECK(!decodeHistoryBody(body, 0, HistoryOrder::Ascending, limits, decoded));
    CHECK(decoded.empty());
    limits.maxDecodedBytes += sizeof(std::size_t);
    CHECK(decodeHistoryBody(body, 0, HistoryOrder::Ascending, limits, decoded));
    CHECK(decoded.empty());
}

TEST(shortHeapHistoryLinksShareThePublishersDecodedAllowance)
{
    auto t = terminalOf(4, 40);
    Ubiquitty::setScrollbackLimit(t.get(), 64);
    feed(*t, "\x1b]8;;" + std::string(24, 'u') + "\x1b\\");
    for (int index = 0; index < 32; ++index)
        feed(*t, "x\r\n");
    feed(*t, "\x1b]8;;\x1b\\\r\n\r\n\r\n\r\n");
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    auto limits = historyBodyLimits;
    limits.maxDecodedBytes = 4 * 1'024;
    std::size_t bodies = 0;
    for (std::uint64_t from = 0; from < 32;) {
        std::vector<std::uint8_t> body;
        const auto count = publisher.writeBody(*t, { from, 32 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body, limits.maxDecodedBytes);
        REQUIRE(count > 0);
        std::vector<DecodedLine> decoded;
        REQUIRE(decodeHistoryBody(body, from, HistoryOrder::Ascending, limits, decoded));
        CHECK_EQ(decoded.size(), count);
        CHECK_EQ(decoded.front().links.front().uri.size(), std::size_t { 24 });
        from += count;
        ++bodies;
    }
    CHECK(bodies > 1);
}

TEST(historyRunTextHeadersIncludeTheirGrowthStorage)
{
    Ubiquitty::Cell lead = Ubiquitty::defaultBlankCell;
    lead.codePoint = 'x';
    lead.pen.attributes.add(Ubiquitty::VT::Attribute::Wide);
    lead.pen.extendedAttributes.setTextSizeSpecID(1);
    constexpr std::size_t entries = 8;
    constexpr std::size_t points = Ubiquitty::runTextMaximumCodePoints;
    std::vector<Ubiquitty::Cell> cells;
    for (std::size_t index = 0; index < entries; ++index) {
        cells.push_back(lead);
        cells.push_back(Ubiquitty::Screen::derivedContinuation(lead));
    }
    auto body = repeatedHistoryBody(cells, 1);
    WireWriter writer(body);
    writer.varint(entries);
    for (std::size_t index = 0; index < entries; ++index) {
        writer.varint(0);
        writer.varint(index * 2);
        writer.u16(1);
        writer.varint(points);
        for (std::size_t point = 0; point < points; ++point)
            writer.varint('x');
    }
    std::vector<DecodedLine> decoded;
    REQUIRE(decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK_EQ(decoded.front().runTexts.size(), entries);
    auto limits = historyBodyLimits;
    limits.maxDecodedBytes = sizeof(Ubiquitty::Pen) + sizeof(std::size_t) + 1 + 2 * sizeof(DecodedLine)
        + cells.size() * sizeof(Ubiquitty::Cell) + entries * (2 * sizeof(DecodedLine::RunText) + points * sizeof(std::uint32_t));
    std::vector<DecodedLine> unchanged(1);
    unchanged.front().number = 77;
    CHECK(!decodeHistoryBody(body, 0, HistoryOrder::Ascending, limits, unchanged));
    REQUIRE(unchanged.size() == 1);
    CHECK_EQ(unchanged.front().number, std::uint64_t { 77 });
}

TEST(historyPublisherSplitsBodiesBeforeTheirClusterTableLimit)
{
    auto t = terminalOf(2, 128);
    Ubiquitty::setScrollbackLimit(t.get(), 64);
    REQUIRE(appendDistinctClusterHistory(*t, maxClustersPerEpoch + 1, HistoryTestWrapping::SeparateLines));
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    REQUIRE(publisher.address().head == 33);
    std::vector<std::uint8_t> body;
    const auto first = publisher.writeBody(*t, { 0, 33 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body);
    CHECK_EQ(first, std::size_t { 32 });
    std::vector<DecodedLine> decoded;
    REQUIRE(decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK_EQ(decoded.size(), first);
    body.clear();
    CHECK_EQ(publisher.writeBody(*t, { first, 33 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body), std::size_t { 1 });
    decoded.clear();
    REQUIRE(decodeHistoryBody(body, first, HistoryOrder::Ascending, historyBodyLimits, decoded));
    REQUIRE(decoded.front().clusterHostIDs.size() == 1);
    CHECK_EQ(decoded.front().clusterHostIDs.front(), maxClustersPerEpoch + 1);
}

TEST(aHistoryLinePastItsClusterTableLimitRefusesWithoutWriting)
{
    auto t = terminalOf(2, 128);
    Ubiquitty::setScrollbackLimit(t.get(), 64);
    REQUIRE(appendDistinctClusterHistory(*t, maxClustersPerEpoch + 1, HistoryTestWrapping::JoinedLine));
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    REQUIRE(publisher.address().head == 1);
    std::vector<std::uint8_t> body { 0xAA };
    CHECK_EQ(publisher.writeBody(*t, { 0, 1 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body), std::size_t { 0 });
    REQUIRE(body.size() == 1);
    CHECK_EQ(body.front(), 0xAA);
}

TEST(historyPublisherSplitsAtTheRunTextEntryCountLimit)
{
    auto t = terminalOf(2, 1024);
    Ubiquitty::setScrollbackLimit(t.get(), 64);
    const std::array<std::uint32_t, 1> text { 'x' };
    const auto anchor = sharedHistoryRun(*t, text);
    REQUIRE(anchor);
    const std::vector<Ubiquitty::Cell> cells(1024, *anchor);
    appendRepeatedHistoryRow(*t, cells, 32);
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    REQUIRE(publisher.address().head == 32);
    std::vector<std::uint8_t> body;
    const auto first = publisher.writeBody(*t, { 0, 32 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body);
    CHECK_EQ(first, std::size_t { 31 });
    std::vector<DecodedLine> decoded;
    REQUIRE(decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, decoded));
    REQUIRE(decoded.size() == first);
    CHECK_EQ(decoded.front().runTexts.size(), cells.size());
    body.clear();
    CHECK_EQ(publisher.writeBody(*t, { first, 32 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body), std::size_t { 1 });
    decoded.clear();
    REQUIRE(decodeHistoryBody(body, first, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK_EQ(decoded.front().runTexts.size(), cells.size());
}

TEST(historyPublisherSplitsAtTheRunTextCodePointBudget)
{
    auto t = terminalOf(2, 2);
    Ubiquitty::setScrollbackLimit(t.get(), 128);
    const std::vector<std::uint32_t> text(Ubiquitty::runTextMaximumCodePoints, 'x');
    const auto anchor = sharedHistoryRun(*t, text);
    REQUIRE(anchor);
    const std::vector<Ubiquitty::Cell> cells { *anchor, Ubiquitty::defaultBlankCell };
    appendRepeatedHistoryRow(*t, cells, 65);
    HistoryPublisher publisher;
    REQUIRE(publisher.capture(*t));
    REQUIRE(publisher.address().head == 65);
    std::vector<std::uint8_t> body;
    const auto first = publisher.writeBody(*t, { 0, 65 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body);
    CHECK_EQ(first, std::size_t { 64 });
    std::vector<DecodedLine> decoded;
    REQUIRE(decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK_EQ(decoded.size(), first);
    body.clear();
    CHECK_EQ(publisher.writeBody(*t, { first, 65 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body), std::size_t { 1 });
    decoded.clear();
    REQUIRE(decodeHistoryBody(body, first, HistoryOrder::Ascending, historyBodyLimits, decoded));
    CHECK_EQ(decoded.front().runTexts.front().codePoints.size(), text.size());
}

TEST(unrepresentableHistoryRunTextRefusesWithoutWriting)
{
    const std::array<std::vector<std::uint32_t>, 2> texts { std::vector<std::uint32_t>(Ubiquitty::runTextMaximumCodePoints + 1, 'x'), std::vector<std::uint32_t> { 'x', 0xD800 } };
    for (const auto& text : texts) {
        auto t = terminalOf(2, 2);
        const auto anchor = sharedHistoryRun(*t, text);
        REQUIRE(anchor);
        const std::vector<Ubiquitty::Cell> cells { *anchor, Ubiquitty::defaultBlankCell };
        appendRepeatedHistoryRow(*t, cells, 1);
        HistoryPublisher publisher;
        REQUIRE(publisher.capture(*t));
        std::vector<std::uint8_t> body { 0xAA };
        CHECK_EQ(publisher.writeBody(*t, { 0, 1 }, HistoryOrder::Ascending, maxHistoryTransferBytes, body), std::size_t { 0 });
        REQUIRE(body.size() == 1);
        CHECK_EQ(body.front(), 0xAA);
    }
}

TEST(historyLinksPreserveBytesOutsideUTF8)
{
    const std::string key("key\xFF");
    const std::string uri("https://example.test/\xFF");
    const auto line = linkedHistoryLine(key, uri, HistoryTestEncoding::UTF8);
    REQUIRE(line);
    REQUIRE(line->links.size() == 1);
    CHECK_STR_EQ(line->links.front().key, key);
    CHECK_STR_EQ(line->links.front().uri, uri);
}

TEST(historyLinksPreserveBytesClippedInsideUTF8Sequences)
{
    for (const auto encoding : { HistoryTestEncoding::UTF8, HistoryTestEncoding::Latin1 }) {
        const std::string accent = encoding == HistoryTestEncoding::UTF8 ? "\xC3\xA9" : "\xE9";
        const std::string key = std::string(Ubiquitty::VT::maxLinkKeyLength - 2, 'k') + accent;
        std::string uri = "https://example.test/";
        uri.append(static_cast<std::size_t>(Ubiquitty::VT::maxLinkURILength - 2) - uri.size(), 'u');
        uri += accent;
        const auto line = linkedHistoryLine(key, uri, encoding);
        REQUIRE(line);
        REQUIRE(line->links.size() == 1);
        const std::string expectedKey = std::string(Ubiquitty::VT::maxLinkKeyLength - 2, 'k') + "\xC3";
        const std::string expectedURI = uri.substr(0, Ubiquitty::VT::maxLinkURILength - 2) + "\xC3";
        CHECK_STR_EQ(line->links.front().key, expectedKey);
        CHECK_STR_EQ(line->links.front().uri, expectedURI);
    }
}

TEST(malformedHistoryLinkBytesRefuseWithoutChangingOutput)
{
    Ubiquitty::Cell cell = Ubiquitty::defaultBlankCell;
    cell.codePoint = 'x';
    cell.setLinkID(1);
    const auto cells = std::span(&cell, 1);
    const std::string invalidKey("k\0x", 3);
    const std::string invalidURI("u\0x", 3);
    const std::array<std::vector<std::uint8_t>, 2> nulBodies { repeatedHistoryBody(cells, 1, invalidKey, "u"), repeatedHistoryBody(cells, 1, "k", invalidURI) };
    std::vector<DecodedLine> unchanged(1);
    unchanged.front().number = 77;
    for (const auto& body : nulBodies) {
        CHECK(!decodeHistoryBody(body, 0, HistoryOrder::Ascending, historyBodyLimits, unchanged));
        REQUIRE(unchanged.size() == 1);
        CHECK_EQ(unchanged.front().number, std::uint64_t { 77 });
    }
    const std::string rawURI("u\xFF");
    const auto valid = repeatedHistoryBody(cells, 1, "k", rawURI);
    WireReader cursor(valid);
    const auto pens = cursor.varint();
    (void)cursor.take(static_cast<std::size_t>(pens) * penWireBytes);
    REQUIRE(cursor.varint() == 1);
    (void)cursor.u16();
    (void)cursor.bytes(Ubiquitty::VT::maxLinkKeyLength - 1);
    const auto uriBytes = cursor.varint();
    REQUIRE(cursor.ok() && uriBytes > 0);
    const auto truncated = std::span(valid).first(cursor.offset() + static_cast<std::size_t>(uriBytes) - 1);
    CHECK(!decodeHistoryBody(truncated, 0, HistoryOrder::Ascending, historyBodyLimits, unchanged));
    auto combined = valid;
    combined.insert(combined.end(), valid.begin(), valid.end());
    CHECK(!decodeHistoryBody(combined, 0, HistoryOrder::Ascending, historyBodyLimits, unchanged));
    REQUIRE(unchanged.size() == 1);
    CHECK_EQ(unchanged.front().number, std::uint64_t { 77 });
    REQUIRE(decodeHistoryBody(valid, 9, HistoryOrder::Ascending, historyBodyLimits, unchanged));
    REQUIRE(unchanged.size() == 2);
    CHECK_EQ(unchanged.front().number, std::uint64_t { 77 });
    CHECK_EQ(unchanged.back().number, std::uint64_t { 9 });
    REQUIRE(unchanged.back().links.size() == 1);
    CHECK_STR_EQ(unchanged.back().links.front().uri, rawURI);
}

UBIQUITTY_TEST_MAIN()
