// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_cell_cluster — reading a cell's whole grapheme cluster back out.
//
// A Cell holds one base code point and a single inline continuation; anything
// longer spills into the terminal's cluster pool and the cell keeps an id. So
// this is the one function that answers "what does this cell actually say", and
// every consumer that copies, searches, exports or reads text aloud goes
// through it.
//
// The caller supplies the buffer and its size, and the cases below are mostly
// about that size: every existing call in the suite passes sixteen, which no
// cluster reaches, so the arm that shortens a long cluster to fit had never run.
// A caller with a smaller buffer is the one that would silently lose the end of
// a family emoji or a flag.

#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/ClusterPool.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/Width.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t poison = 0xDEADBEEF;

void feed(Ubiquitty::Terminal& t, const char* s)
{
    Ubiquitty::processBytes(&t, s);
}

const Ubiquitty::Cell* cellAt(const Ubiquitty::Terminal& t, int row, int column)
{
    return t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
}

// A five-code-point family: two people, a child, joined by zero-width joiners.
// Longer than a cell holds inline, so the lead cell carries a pool id.
constexpr const char* familyEmoji = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";

} // namespace

TEST(testAClusterLongerThanTheBufferIsShortenedRatherThanOverrun)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    feed(t, familyEmoji);

    const Ubiquitty::Cell* lead = cellAt(t, 0, 0);
    CHECK_MSG(lead->clusterID() != 0, "five code points spill into the pool");

    // The whole thing, for the control.
    std::uint32_t whole[16];
    const int full = Ubiquitty::cellClusterCodePoints(&t, lead, whole, 16);
    CHECK(full == 5);

    // Every buffer size shorter than the cluster returns exactly that many code
    // points, in order, and touches nothing past them. Without the clamp the
    // copy runs to the cluster's length whatever the caller asked for.
    for (int room = 1; room <= 5; ++room) {
        std::vector<std::uint32_t> buffer(16, poison);
        const int n = Ubiquitty::cellClusterCodePoints(&t, lead, buffer.data(), room);
        CHECK_MSG(n == room, "as many as there was room for");
        for (int i = 0; i < room; ++i)
            CHECK_MSG(buffer[static_cast<std::size_t>(i)] == whole[i], "and the same ones, in order");
        CHECK_MSG(buffer[static_cast<std::size_t>(room)] == poison, "and nothing past the room given");
    }

    // Room to spare returns the cluster and stops, rather than filling the rest.
    std::vector<std::uint32_t> roomy(16, poison);
    CHECK(Ubiquitty::cellClusterCodePoints(&t, lead, roomy.data(), 16) == 5);
    CHECK(roomy[5] == poison);
}

TEST(testAnInlineClusterIsAlsoBoundedByTheBuffer)
{
    // The other path: a base with one combining mark, which fits in the cell
    // and never reaches the pool. Its bound is written separately from the
    // pooled one, so it is checked separately.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    feed(t, "e\xCC\x81"); // 'e' + U+0301

    const Ubiquitty::Cell* base = cellAt(t, 0, 0);
    CHECK_MSG(base->clusterID() == 0, "a base and one mark stay inline");

    std::vector<std::uint32_t> both(8, poison);
    CHECK(Ubiquitty::cellClusterCodePoints(&t, base, both.data(), 8) == 2);
    CHECK(both[0] == 'e');
    CHECK(both[1] == 0x0301);
    CHECK(both[2] == poison);

    // With room for one, the base comes back and the mark does not.
    std::vector<std::uint32_t> tight(8, poison);
    CHECK(Ubiquitty::cellClusterCodePoints(&t, base, tight.data(), 1) == 1);
    CHECK(tight[0] == 'e');
    CHECK_MSG(tight[1] == poison, "the mark did not go past the room given");

    // A plain letter is a cluster of one.
    feed(t, "\r\nZ");
    const Ubiquitty::Cell* plain = cellAt(t, 1, 0);
    std::vector<std::uint32_t> one(8, poison);
    CHECK(Ubiquitty::cellClusterCodePoints(&t, plain, one.data(), 8) == 1);
    CHECK(one[0] == 'Z');
    CHECK(one[1] == poison);
}

TEST(testACellWithNothingToSayReadsBackAsNothing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    feed(t, familyEmoji);

    std::vector<std::uint32_t> buffer(8, poison);

    // A blank cell holds no code point, and so does the continuation half of a
    // wide glyph — the caller walks every column and asks about all of them.
    const Ubiquitty::Cell* blank = cellAt(t, 0, 5);
    CHECK(Ubiquitty::cellClusterCodePoints(&t, blank, buffer.data(), 8) == 0);
    CHECK_MSG(buffer[0] == poison, "and writes nothing");

    const Ubiquitty::Cell* continuation = cellAt(t, 0, 1);
    CHECK_MSG(continuation->codePoint == 0, "the second half of a wide glyph carries no code point");
    CHECK(Ubiquitty::cellClusterCodePoints(&t, continuation, buffer.data(), 8) == 0);
    CHECK(buffer[0] == poison);

    // Nothing to read from, nowhere to put it, or no room for it: each answers
    // zero rather than dereferencing what it was handed.
    const Ubiquitty::Cell* lead = cellAt(t, 0, 0);
    CHECK(Ubiquitty::cellClusterCodePoints(nullptr, lead, buffer.data(), 8) == 0);
    CHECK(Ubiquitty::cellClusterCodePoints(&t, nullptr, buffer.data(), 8) == 0);
    CHECK(Ubiquitty::cellClusterCodePoints(&t, lead, nullptr, 8) == 0);
    for (int room : { 0, -1, -1000 }) {
        CHECK_MSG(Ubiquitty::cellClusterCodePoints(&t, lead, buffer.data(), room) == 0,
            "no room means nothing written");
        CHECK(buffer[0] == poison);
    }

    // The control: the same cell and buffer do produce a cluster, so the zeros
    // above are about the arguments rather than about this cell being empty.
    CHECK(Ubiquitty::cellClusterCodePoints(&t, lead, buffer.data(), 8) == 5);
}

TEST(testACellNamingAClusterThePoolNoLongerHoldsFallsBackToItsBase)
{
    // A cell can name an id its terminal's pool doesn't hold: a cell copied in
    // from another terminal, or one planted by a peer. It has to read back as
    // what the cell itself still carries — its base and inline mark — rather
    // than as nothing, and never as some other cluster's text.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    feed(t, familyEmoji);

    const Ubiquitty::Cell* lead = cellAt(t, 0, 0);
    CHECK(lead->clusterID() != 0);
    CHECK_MSG(Ubiquitty::cellClusterCodePoints(&t, lead, std::vector<std::uint32_t>(16).data(), 16) == 5,
        "the cluster reads whole while the pool holds it");

    // The same cell, naming an id the pool does not hold. Written by planting
    // the cell rather than by spending the whole pool: what matters is that the
    // id resolves to nothing, and the pool's size is not part of the property.
    // The id fits the cell's 15-bit field, so the cell names the id checked.
    constexpr std::uint16_t unheldID = 30000;
    Ubiquitty::Cell stranded = *lead;
    stranded.setClusterID(unheldID);
    REQUIRE(stranded.clusterID() == unheldID);
    CHECK_MSG(t.clusterPool.sequence(unheldID).empty(), "the pool does not hold that id");
    t.grid->fillCellRun(Ubiquitty::Row(1), Ubiquitty::Column(0), stranded, 1);

    const Ubiquitty::Cell* orphan = cellAt(t, 1, 0);
    std::vector<std::uint32_t> buffer(16, poison);
    const int n = Ubiquitty::cellClusterCodePoints(&t, orphan, buffer.data(), 16);
    CHECK_MSG(n >= 1, "a cell whose pool entry is gone still says something");
    CHECK_MSG(buffer[0] == 0x1F468u, "and it starts with the base the cell holds");
    CHECK(buffer[static_cast<std::size_t>(n)] == poison);

    // The fallback obeys the buffer bound too.
    std::vector<std::uint32_t> tight(16, poison);
    CHECK(Ubiquitty::cellClusterCodePoints(&t, orphan, tight.data(), 1) == 1);
    CHECK(tight[0] == 0x1F468u);
    CHECK(tight[1] == poison);
}

UBIQUITTY_TEST_MAIN()

namespace {

// UTF-8 for one code point, appended.
void appendUTF8(std::string& out, std::uint32_t codePoint)
{
    if (codePoint < 0x80) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        out += static_cast<char>(0xC0 | (codePoint >> 6));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codePoint >> 12));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codePoint >> 18));
        out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

// The index-th of a family of distinct three-code-point clusters: a CJK base
// and two combining marks, so each one overflows the inline buffer and interns.
std::array<std::uint32_t, 3> distinctCluster(int index)
{
    return { static_cast<std::uint32_t>(0x4E00 + (index & 0x3FFF)), static_cast<std::uint32_t>(0x0300 + ((index >> 14) & 0x3F)), 0x0345 };
}

} // namespace

TEST(testAnOldCellKeepsItsClusterAfterThePoolFills)
{
    // Ids are never reused. Before, the pool recycled its oldest id at the
    // 15-bit ceiling, and a cell still naming that id read back the NEW
    // cluster: text it never held, on screen and in copies. Fill the pool past
    // its ceiling with distinct clusters written over one cell, and the cell
    // written first, which still holds an early id, must read back unchanged.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    feed(t, familyEmoji);
    const Ubiquitty::Cell* early = cellAt(t, 0, 0);
    REQUIRE(early->clusterID() != 0);
    std::vector<std::uint32_t> before(16, poison);
    const int beforeCount = Ubiquitty::cellClusterCodePoints(&t, early, before.data(), 16);
    REQUIRE(beforeCount == 5);

    const int total = Ubiquitty::ClusterPool::maxEntries + 600;
    std::string chunk;
    for (int index = 0; index < total; ++index) {
        chunk += "\x1b[2;1H";
        for (const std::uint32_t codePoint : distinctCluster(index))
            appendUTF8(chunk, codePoint);
        if (chunk.size() > 64 * 1024 || index + 1 == total) {
            feed(t, chunk.c_str());
            chunk.clear();
        }
    }
    CHECK_MSG(t.clusterPool.isFull(), "the clusters fed outnumber the ids");

    std::vector<std::uint32_t> after(16, poison);
    const int afterCount = Ubiquitty::cellClusterCodePoints(&t, cellAt(t, 0, 0), after.data(), 16);
    CHECK_EQ(afterCount, beforeCount);
    CHECK_MSG(std::equal(before.begin(), before.begin() + beforeCount, after.begin()), "the early cell reads the cluster it was written with");

    // The cluster written last arrived after the ceiling: it was refused, so
    // its cell keeps its base and inline mark, and is not another cluster.
    const auto last = distinctCluster(total - 1);
    std::vector<std::uint32_t> refused(16, poison);
    const int refusedCount = Ubiquitty::cellClusterCodePoints(&t, cellAt(t, 1, 0), refused.data(), 16);
    CHECK_EQ(refusedCount, 2);
    CHECK_EQ(refused[0], last[0]);
    CHECK_EQ(refused[1], last[1]);
}

TEST(testAClusterThatGrowsAsThePoolFillsKeepsTheTextItShowed)
{
    // Each mark re-interns the growing sequence, so a long cluster can take the
    // pool's last free id partway through. The next mark is refused, and the
    // cell must go on reading the cluster it already showed, not fall back to
    // its base and inline mark.
    Ubiquitty::Terminal t;
    Ubiquitty::resize(t, 3, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
    for (int index = 0; t.clusterPool.size() + 1 < Ubiquitty::ClusterPool::maxEntries; ++index) {
        const auto filler = distinctCluster(index);
        REQUIRE(t.clusterPool.intern(filler) != 0);
    }
    REQUIRE(!t.clusterPool.isFull());

    // A base and four marks, the last two in reads of their own, as a program
    // writing a mark at a time sends them: the first read's three code points
    // take the last id, and each later mark grows a sequence the pool refuses.
    const std::array<std::uint32_t, 5> grown { 0x0061, 0x0301, 0x0302, 0x0303, 0x0304 };
    std::string text;
    for (const std::uint32_t codePoint : std::span(grown).first(3))
        appendUTF8(text, codePoint);
    feed(t, text.c_str());
    REQUIRE(t.clusterPool.isFull());
    for (const std::uint32_t codePoint : std::span(grown).subspan(3)) {
        std::string mark;
        appendUTF8(mark, codePoint);
        feed(t, mark.c_str());
    }

    const Ubiquitty::Cell* cell = cellAt(t, 0, 0);
    CHECK(cell->clusterID() != 0);
    std::vector<std::uint32_t> shown(16, poison);
    const int shownCount = Ubiquitty::cellClusterCodePoints(&t, cell, shown.data(), 16);
    CHECK_EQ(shownCount, 3);
    CHECK_MSG(std::equal(grown.begin(), grown.begin() + 3, shown.begin()), "the cell reads the longest cluster the pool admitted");
}

namespace {

// How a cluster reaches the parser: the batched path a single read takes, or
// the per-code-point path a mark a read takes.
enum class ClusterReads : std::uint8_t {
    OneRead,
    OneCodePointARead,
};

// A base followed by marks, written as `reads` says.
void writeCluster(Ubiquitty::Terminal& t, std::uint32_t base, std::span<const std::uint32_t> marks, ClusterReads reads)
{
    std::string text;
    appendUTF8(text, base);
    for (const std::uint32_t mark : marks) {
        if (reads == ClusterReads::OneCodePointARead) {
            feed(t, text.c_str());
            text.clear();
        }
        appendUTF8(text, mark);
    }
    feed(t, text.c_str());
}

// A terminal is neither copyable nor movable, so each case declares its own
// and sizes it here.
void prepareClusterTerminal(Ubiquitty::Terminal& t)
{
    Ubiquitty::resize(t, 3, 40, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(t);
}

} // namespace

TEST(testCodePointsPastTheClusterMaximumAreConsumedAndDropped)
{
    // A cluster keeps its base and fifteen more code points. Output is
    // untrusted, so a longer one does not grow the pool entry or the cell: the
    // later code points join the cluster, the cursor stays put, and nothing
    // stores them. Both parser paths are covered, the batched one a single read
    // takes and the per-code-point one a mark a read takes. The pool is measured
    // once the cluster is full rather than against a second terminal, because
    // where a read ends decides which shorter prefixes the pool keeps.
    constexpr std::uint32_t acute = 0x0301;
    const std::vector<std::uint32_t> boundedMarks(Ubiquitty::clusterPoolMaximumCodePoints - 1, acute);
    const std::vector<std::uint32_t> overflowingMarks(4, acute);
    for (const ClusterReads reads : { ClusterReads::OneRead, ClusterReads::OneCodePointARead }) {
        Ubiquitty::Terminal t;
        prepareClusterTerminal(t);
        writeCluster(t, 'a', boundedMarks, reads);
        const int poolSizeWhenFull = t.clusterPool.size();
        std::string more;
        for (const std::uint32_t mark : overflowingMarks) {
            appendUTF8(more, mark);
            if (reads == ClusterReads::OneCodePointARead) {
                feed(t, more.c_str());
                more.clear();
            }
        }
        if (!more.empty())
            feed(t, more.c_str());
        CHECK_MSG(t.clusterPool.size() == poolSizeWhenFull, "marks past the maximum intern nothing");
        feed(t, "Z");

        std::vector<std::uint32_t> buffer(Ubiquitty::clusterPoolMaximumCodePoints * 2, poison);
        const int count = Ubiquitty::cellClusterCodePoints(&t, cellAt(t, 0, 0), buffer.data(), static_cast<int>(buffer.size()));
        CHECK_EQ(count, Ubiquitty::clusterPoolMaximumCodePoints);
        CHECK_EQ(buffer[0], std::uint32_t { 'a' });
        CHECK_MSG(std::all_of(buffer.begin() + 1, buffer.begin() + count, [](std::uint32_t codePoint) { return codePoint == acute; }), "the stored cluster is the base and the first fifteen marks");
        CHECK(buffer[static_cast<std::size_t>(count)] == poison);
        CHECK_MSG(cellAt(t, 0, 1)->codePoint == 'Z', "the dropped marks did not move the cursor");
    }
}

TEST(testAPresentationSelectorPastTheClusterMaximumStillSelectsTheBasesPresentation)
{
    // A VS16 past the cap is dropped from the stored cluster like any later
    // code point, but it still selects emoji presentation for a pictographic
    // base: presentation belongs to the base, and the cap bounds storage only.
    constexpr std::uint32_t heart = 0x2764;
    constexpr std::uint32_t acute = 0x0301;
    std::vector<std::uint32_t> marks(Ubiquitty::clusterPoolMaximumCodePoints - 1, acute);
    marks.push_back(Ubiquitty::Unicode::Width::variationSelector16);
    for (const ClusterReads reads : { ClusterReads::OneRead, ClusterReads::OneCodePointARead }) {
        Ubiquitty::Terminal t;
        prepareClusterTerminal(t);
        writeCluster(t, heart, marks, reads);

        const Ubiquitty::Cell* lead = cellAt(t, 0, 0);
        std::vector<std::uint32_t> buffer(Ubiquitty::clusterPoolMaximumCodePoints * 2, poison);
        const int count = Ubiquitty::cellClusterCodePoints(&t, lead, buffer.data(), static_cast<int>(buffer.size()));
        CHECK_EQ(count, Ubiquitty::clusterPoolMaximumCodePoints);
        CHECK_MSG(std::find(buffer.begin(), buffer.begin() + count, Ubiquitty::Unicode::Width::variationSelector16) == buffer.begin() + count, "the selector is not stored");
        const auto attributes = Ubiquitty::cellPen(*lead).attributes;
        CHECK_MSG(attributes.contains(Ubiquitty::VT::Attribute::Emoji), "the base takes emoji presentation");
        CHECK_MSG(attributes.contains(Ubiquitty::VT::Attribute::Wide), "and widens to two cells");
    }
}
