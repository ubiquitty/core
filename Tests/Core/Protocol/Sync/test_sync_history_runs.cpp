// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_sync_history_runs.cpp — an OSC 66 proportional run in the history a
// client projects (Docs/sync.md §6.6, Docs/proportional-text.md): the client
// wraps a history line at its own width, and a run in it lands as the host's
// rewrap leaves it, whole on one row, with its text.

#include "TerminalFixture.h"

#include "Core/Cell.h"
#include "Core/CellPen.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/HistoryPlane.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/TextSizeTable.h"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty;

namespace {

std::unique_ptr<Terminal> makeTerminal(int rows, int columns)
{
    auto t = std::make_unique<Terminal>();
    hardReset(*t);
    resize(*t, rows, columns);
    t->drawsSizedText = true;
    t->drawsProportionalText = true;
    return t;
}

/// A proportional run of `cells` columns.
std::string run(const std::string& text, int cells)
{
    return "\x1b]66;p=1:c=" + std::to_string(cells) + ";" + text + "\x1b\\";
}

std::string utf8(std::span<const std::uint32_t> codePoints)
{
    std::string out;
    for (const std::uint32_t codePoint : codePoints)
        out.push_back(codePoint < 0x80 ? static_cast<char>(codePoint) : '?');
    return out;
}

/// Whether a signature reads a run's text.
enum class RunText : std::uint8_t {
    Read,
    Skipped,
};

/// The history as rows: a run's anchor as `[text/claim]`, each cell it
/// continues into as `~`, a wrapped row ending in `\`.
std::string historySignature(const Terminal& t, RunText runText)
{
    std::string out;
    const int count = Screen::scrollbackCount(&t);
    for (int index = 0; index < count; ++index) {
        const Screen::ScrollLine* line = Screen::scrollbackLine(&t, index);
        if (!line)
            continue;
        std::string row;
        for (int column = 0; column < line->length; ++column) {
            const Cell& cell = line->cells[column];
            const Pen pen = cellPen(cell);
            if (isProportionalRunAnchor(&t, cell)) {
                const TextSizeSpec* spec = t.textSizeTable.lookup(pen.extendedAttributes.textSizeSpecID());
                row += "[" + (runText == RunText::Read ? utf8(runTextCodePoints(&t, cell)) : std::string {}) + "/" + std::to_string(spec ? spec->runWidthCells : 0) + "]";
            } else if (pen.attributes.contains(VT::Attribute::WideContinuation)) {
                row.push_back('~');
            } else {
                row.push_back(cell.codePoint ? static_cast<char>(cell.codePoint < 0x80 ? cell.codePoint : '?') : ' ');
            }
        }
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        out += row + (line->wrapped ? "\\\n" : "\n");
    }
    return out;
}

/// A client following a host: the screen by checkpoint, and the history the
/// checkpoint names on the history plane, projected at the client's width.
struct Mirror {
    Protocol::Sync::ScreenPublisher publisher;
    Protocol::Sync::AttachmentBaseline baseline;
    Protocol::Sync::ScreenReplica replica;
    std::unique_ptr<Terminal> client;
    std::uint64_t generation = 0;

    explicit Mirror(const Terminal& host)
        : client(makeTerminal(host.rows, host.columns))
    {
        follow(host);
    }

    void follow(const Terminal& host)
    {
        std::vector<std::uint8_t> body;
        REQUIRE(publisher.capture(host));
        REQUIRE(publisher.checkpoint(host, ++generation, body, baseline));
        CHECK(Protocol::Sync::applyCheckpoint(*client, body, replica) == Protocol::Sync::ApplyOutcome::Applied);
        Protocol::Sync::HistoryPublisher history;
        (void)history.capture(host);
        const Screen::HistoryAddress address = history.address();
        Protocol::Sync::HistoryCache cache({}, {}, 1'000'000);
        if (address.head > address.oldest) {
            body.clear();
            REQUIRE(history.writeBody(host, { address.oldest, address.head }, Protocol::Sync::HistoryOrder::Ascending, std::size_t { 64 } << 20, body) > 0);
            const Protocol::Sync::HistoryBodyLimits limits { .maxLines = Protocol::Sync::historyBodyLimits.maxLines, .maxLineCells = Protocol::Sync::maxHistoryLineCells, .maxBodyBytes = body.size() };
            std::vector<Protocol::Sync::DecodedLine> lines;
            REQUIRE(Protocol::Sync::decodeHistoryBody(body, address.oldest, Protocol::Sync::HistoryOrder::Ascending, limits, lines));
            REQUIRE(cache.insert(cache.key().epoch, lines) == Protocol::Sync::CacheInsert::Stored);
        }
        cache.setHead(cache.key().epoch, address.head);
        if (history.openLine()) {
            body.clear();
            std::vector<Protocol::Sync::DecodedLine> lines;
            REQUIRE(history.writeOpenLine(host, std::size_t { 64 } << 20, body));
            REQUIRE(Protocol::Sync::decodeHistoryBody(body, address.head, Protocol::Sync::HistoryOrder::Ascending, { .maxLines = 1, .maxLineCells = Protocol::Sync::maxHistoryLineCells, .maxBodyBytes = body.size() }, lines));
            REQUIRE(cache.setOpenLine(cache.key().epoch, std::move(lines.front())) == Protocol::Sync::CacheInsert::Stored);
        }
        cache.project(*client, projectedColumns ? projectedColumns : client->columns);
    }

    /// The width the history is projected at; zero for the client's own.
    int projectedColumns = 0;
};

/// A host whose history holds runs: one longer than some widths hold, one
/// beside wide cells, and short ones.
std::unique_ptr<Terminal> hostWithRunsInHistory()
{
    auto host = makeTerminal(4, 40);
    UbiquittyTest::feed(*host, "one two " + run("a run longer than a cluster holds", 18) + " three four five six seven\r\n");
    UbiquittyTest::feed(*host, "\xe6\x9d\xb1\xe4\xba\xac " + run("beside wide cells", 9) + " end\r\n");
    for (int line = 0; line < 6; ++line)
        UbiquittyTest::feed(*host, "scrolled " + std::to_string(line) + " " + run("away", 4) + "\r\n");
    return host;
}

} // namespace

// The host's rewrap moves a run whole to the row it fits on, or clamps it to
// a row it is wider than; the client's projection of the same line at the
// same width must land each run where the host's did.
TEST(aRunInHistoryLandsWhereTheHostsRewrapPutsIt)
{
    auto host = hostWithRunsInHistory();
    Mirror mirror(*host);
    CHECK_STR_EQ(historySignature(*mirror.client, RunText::Skipped), historySignature(*host, RunText::Skipped));
    for (const int columns : { 24, 13, 60, 40 }) {
        resize(*host, host->rows, columns);
        mirror.follow(*host);
        CHECK_STR_EQ(historySignature(*mirror.client, RunText::Skipped), historySignature(*host, RunText::Skipped));
    }
}

// A client narrower than a run the host's line holds has no row the run fits
// on. It lays that run flat rather than tearing its claim across rows or
// clamping it under a specification the host never defined.
TEST(aRunWiderThanTheClientsRowIsLaidFlat)
{
    auto host = hostWithRunsInHistory();
    Mirror mirror(*host);
    mirror.projectedColumns = 12;
    mirror.follow(*host);
    const Terminal& client = *mirror.client;
    int narrowRuns = 0;
    for (int index = 0; index < Screen::scrollbackCount(&client); ++index) {
        const Screen::ScrollLine* line = Screen::scrollbackLine(&client, index);
        REQUIRE(line);
        CHECK(line->length <= 12);
        for (int column = 0; column < line->length; ++column) {
            const Cell& cell = line->cells[column];
            if (isProportionalRunAnchor(&client, cell)) {
                const TextSizeSpec* spec = client.textSizeTable.lookup(cellPen(cell).extendedAttributes.textSizeSpecID());
                REQUIRE(spec);
                CHECK(column + spec->runWidthCells <= line->length);
                ++narrowRuns;
            }
        }
    }
    // The nine-cell run and the three four-cell runs in the history fit and
    // stay runs; the eighteen-cell one does not.
    CHECK_EQ(narrowRuns, 4);
}

// A run in the client's history holds its text, as the host's does: what a
// copy or a search of that history reads.
TEST(aRunInHistoryKeepsItsText)
{
    auto host = hostWithRunsInHistory();
    Mirror mirror(*host);
    CHECK_STR_EQ(historySignature(*mirror.client, RunText::Read), historySignature(*host, RunText::Read));
}

namespace {

/// The host's history as a body without run texts, its line count, and
/// the column of the first line's run.
struct OmittedBody {
    std::vector<std::uint8_t> bytes;
    std::uint64_t firstLine = 0;
    std::uint64_t lines = 0;
};

OmittedBody omittedBody(const Terminal& host)
{
    Protocol::Sync::HistoryPublisher history;
    (void)history.capture(host);
    const Screen::HistoryAddress address = history.address();
    OmittedBody body;
    body.firstLine = address.oldest;
    std::vector<std::uint8_t> carried;
    body.lines = history.writeBody(host, { address.oldest, address.head }, Protocol::Sync::HistoryOrder::Ascending, std::size_t { 64 } << 20, carried);
    // The run texts are the body's optional tail: the shortest prefix that
    // still decodes every line is the body without them.
    for (std::size_t size = 1; size <= carried.size(); ++size) {
        const std::span<const std::uint8_t> prefix = std::span(carried).first(size);
        std::vector<Protocol::Sync::DecodedLine> lines;
        const Protocol::Sync::HistoryBodyLimits limits { .maxLines = Protocol::Sync::historyBodyLimits.maxLines, .maxLineCells = Protocol::Sync::maxHistoryLineCells, .maxBodyBytes = size };
        if (Protocol::Sync::decodeHistoryBody(prefix, body.firstLine, Protocol::Sync::HistoryOrder::Ascending, limits, lines) && lines.size() == body.lines) {
            body.bytes.assign(prefix.begin(), prefix.end());
            break;
        }
    }
    return body;
}

/// One run text a test appends to a body.
struct AppendedRunText {
    std::uint64_t line = 0;
    std::uint64_t column = 0;
    std::uint16_t id = 0;
    std::span<const std::uint32_t> text;
};

/// Decodes a body with run texts appended.
bool decodesWithAll(const OmittedBody& body, std::span<const AppendedRunText> entries, std::vector<Protocol::Sync::DecodedLine>* decoded = nullptr)
{
    std::vector<std::uint8_t> bytes = body.bytes;
    Protocol::Sync::WireWriter writer(bytes);
    writer.varint(entries.size());
    for (const AppendedRunText& entry : entries) {
        writer.varint(entry.line);
        writer.varint(entry.column);
        writer.u16(entry.id);
        writer.varint(entry.text.size());
        for (const std::uint32_t codePoint : entry.text)
            writer.varint(codePoint);
    }
    std::vector<Protocol::Sync::DecodedLine> lines;
    const Protocol::Sync::HistoryBodyLimits limits { .maxLines = Protocol::Sync::historyBodyLimits.maxLines, .maxLineCells = Protocol::Sync::maxHistoryLineCells, .maxBodyBytes = bytes.size() };
    const bool isAccepted = Protocol::Sync::decodeHistoryBody(bytes, body.firstLine, Protocol::Sync::HistoryOrder::Ascending, limits, lines);
    if (decoded)
        *decoded = std::move(lines);
    return isAccepted;
}

/// Decodes a body with one run text appended.
bool decodesWith(const OmittedBody& body, std::uint64_t line, std::uint64_t column, std::uint16_t id, std::span<const std::uint32_t> text, std::vector<Protocol::Sync::DecodedLine>* decoded = nullptr)
{
    const AppendedRunText entry { .line = line, .column = column, .id = id, .text = text };
    return decodesWithAll(body, std::span(&entry, 1), decoded);
}

} // namespace

// A run text names a sized anchor of a line the body holds, with a text; any
// other refuses the whole body.
TEST(aRunTextNamingNoAnchorRefusesTheBody)
{
    auto host = hostWithRunsInHistory();
    const OmittedBody body = omittedBody(*host);
    REQUIRE(body.lines > 0);
    const std::vector<std::uint32_t> text { 'a', 'w', 'a', 'y' };
    // The first line is "one two " and its run: column 8 is the anchor.
    CHECK(decodesWith(body, 0, 8, 1, text));
    CHECK(!decodesWith(body, 0, 0, 1, text));
    CHECK(!decodesWith(body, 0, 9, 1, text));
    CHECK(!decodesWith(body, body.lines, 8, 1, text));
    CHECK(!decodesWith(body, 0, 8, 0, text));
    CHECK(!decodesWith(body, 0, 8, 1, {}));
    const std::vector<std::uint32_t> surrogate { 0xD800 };
    CHECK(!decodesWith(body, 0, 8, 1, surrogate));
    // Two texts for one anchor, or texts out of order, refuse it too.
    const std::array<AppendedRunText, 2> twice { { { .line = 0, .column = 8, .id = 1, .text = text }, { .line = 0, .column = 8, .id = 2, .text = text } } };
    CHECK(!decodesWithAll(body, twice));
}

// The client gives an anchor the host's id only when its mirror of the host's
// store holds that run's text under it: an id the store holds for another
// text names none, rather than another run's words.
TEST(aRunTextTheMirrorHoldsOtherwiseNamesNoText)
{
    auto host = hostWithRunsInHistory();
    Mirror mirror(*host);
    const OmittedBody body = omittedBody(*host);
    const Screen::ScrollLine* first = Screen::scrollbackLine(host.get(), 0);
    REQUIRE(first && first->length > 8 && isProportionalRunAnchor(host.get(), first->cells[8]));
    const std::uint16_t id = first->cells[8].clusterID();
    const std::span<const std::uint32_t> held = runTextCodePoints(host.get(), first->cells[8]);
    const std::vector<std::uint32_t> heldText(held.begin(), held.end());
    const std::vector<std::uint32_t> otherText { 'n', 'o', 't', ' ', 'i', 't' };
    for (const auto* text : { &heldText, &otherText }) {
        std::vector<Protocol::Sync::DecodedLine> lines;
        REQUIRE(decodesWith(body, 0, 8, id, *text, &lines));
        Protocol::Sync::HistoryCache cache({}, {}, 1'000'000);
        REQUIRE(cache.insert(cache.key().epoch, lines) == Protocol::Sync::CacheInsert::Stored);
        cache.setHead(cache.key().epoch, body.firstLine + lines.size());
        cache.project(*mirror.client, mirror.client->columns);
        const Screen::ScrollLine* projected = Screen::scrollbackLine(mirror.client.get(), 0);
        REQUIRE(projected && projected->length > 8);
        CHECK_STR_EQ(utf8(runTextCodePoints(mirror.client.get(), projected->cells[8])), text == &heldText ? utf8(heldText) : std::string {});
    }
}

UBIQUITTY_TEST_MAIN()
