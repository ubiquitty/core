// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The routes the single-byte driver takes that the batched one does not.
//
// processBytes hoists its mode decisions above the loop: Tek 4014 and the
// non-ANSI legacy grammars are each answered once for the whole buffer. The
// per-byte entry cannot hoist anything, so it carries its own copy of each
// decision — and every caller that feeds a terminal one byte at a time (the
// sync-output splitter, the ingest loop's tail, a PTY read that lands a
// single byte) goes through that copy rather than the hoisted one.
//
// A drift between the two reads as a mode that works on bulk output and
// stops working on a slow typist, which is the shape of bug that survives a
// test suite feeding whole strings.

#include "Harness.h"
#include "VT100Fixture.h"

#include "Core/Image/TerminalImage.h"
#include "Core/LineStore.h"
#include "Core/Modes/Emulation.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Constants.h"
#include "Core/VT/Parser.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

void feedPerByte(Ubiquitty::Terminal& t, std::string_view bytes)
{
    for (const char byte : bytes)
        Ubiquitty::processByte(&t, static_cast<std::uint8_t>(byte));
}

int liveImageCount(const Ubiquitty::Terminal& t)
{
    int n = 0;
    for (int i = 0; i < Ubiquitty::Image::maxTerminalImageCount; ++i)
        if (t.images.stored[i].used && t.images.stored[i].pixels)
            ++n;
    return n;
}

// These streams contain only isolated non-ASCII bytes, so every rendered cell
// has inline storage. Compare the observable grid, cursor, pen and replies;
// cancellation journals and batching bookkeeping may legitimately differ.
bool byteDriverStatesMatch(Ubiquitty::Terminal& actual, Ubiquitty::Terminal& expected)
{
    const auto& actualScreen = *actual.screens.active;
    const auto& expectedScreen = *expected.screens.active;
    if (actual.rows != expected.rows || actual.columns != expected.columns
        || actualScreen.cursorX != expectedScreen.cursorX || actualScreen.cursorY != expectedScreen.cursorY
        || actualScreen.wrapPending != expectedScreen.wrapPending || actual.scrollbackCount != expected.scrollbackCount
        || std::memcmp(&actualScreen.pen, &expectedScreen.pen, sizeof(actualScreen.pen)) != 0)
        return false;
    for (int row = 0; row < actual.rows; ++row) {
        if (actualScreen.lineWrapped[row] != expectedScreen.lineWrapped[row])
            return false;
        for (int column = 0; column < actual.columns; ++column) {
            if (std::memcmp(UbiquittyTest::cell(actual, row, column), UbiquittyTest::cell(expected, row, column), sizeof(Ubiquitty::Cell)) != 0)
                return false;
        }
    }
    std::array<char, 256> actualReplies {};
    std::array<char, 256> expectedReplies {};
    return drain(actual, actualReplies) == drain(expected, expectedReplies) && actualReplies == expectedReplies;
}

enum class ByteRunRange {
    PrintableASCII,
    CSIParameters
};

void checkByteRunBoundaries(ByteRunRange range)
{
    const bool isCSILookahead = range == ByteRunRange::CSIParameters;
    Ubiquitty::Terminal batched;
    Ubiquitty::Terminal perByte;
    Ubiquitty::hardReset(batched);
    Ubiquitty::hardReset(perByte);
    Ubiquitty::resize(batched, 2, 48, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::resize(perByte, 2, 48, Ubiquitty::Modes::ReflowMode::Fixed);
    for (int offset = 0; offset < 8; ++offset) {
        for (unsigned byte = 0; byte < 256; ++byte) {
            Ubiquitty::hardReset(batched);
            Ubiquitty::hardReset(perByte);
            std::string stream = isCSILookahead ? "ABCDEFGH\033[" : "";
            // One complete word precedes the variable byte's word. The tail
            // makes that second word readable at every candidate offset.
            stream.append(static_cast<std::size_t>(8 + offset), isCSILookahead ? '0' : 'a');
            stream.push_back(static_cast<char>(byte));
            if (!isCSILookahead)
                stream += "tailtext";
            // CCH observes whether an incomplete CSI correctly retained its
            // preceding graphic. The cancel control closes any remaining
            // sequence; a cursor report observes behavior after the boundary.
            stream += "\033T\030\033[6n";
            feed(batched, stream);
            // Keep the transport encoding adapter on both paths: processByte
            // bypasses it and accepts raw C1 controls instead of stray UTF-8.
            for (const char& inputByte : stream)
                feed(perByte, { &inputByte, 1 });
            char context[96];
            std::snprintf(context, sizeof(context), "%s scanner: byte 0x%02X at word offset %d",
                isCSILookahead ? "CSI parameter" : "ASCII", byte, offset);
            CHECK_MSG(byteDriverStatesMatch(batched, perByte), context);
        }
    }
}

} // namespace

TEST(compactASCIIRowsMatchEagerCellsAcrossInputSplitsAndHistory)
{
    const std::array streams {
        std::string_view("ABCDEFG\tHI\r\njklmnop\r\nqrstuvw\r\nXYZ\033T!\033[6n"),
        std::string_view("aaaaaaa\r\nbbbbbbb\r\nccccccc\r\nddddddd\033T\r\ne\xCC\x81\xEF\xB8\x8F\r\nend\033[6n"),
        std::string_view("first\r\nsecond\r\n\033[31mred\033[0m plain\r\n\033]8;;https://example.test\033\\link\033]8;;\033\\\r\nlast\033T\033[6n")
    };
    for (const auto screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        for (const auto stream : streams) {
            for (std::size_t split = 0; split <= stream.size(); ++split) {
                Ubiquitty::Terminal actual;
                Ubiquitty::Terminal expected;
                for (auto* terminal : { &actual, &expected }) {
                    Ubiquitty::hardReset(*terminal);
                    Ubiquitty::resize(*terminal, 3, 8, Ubiquitty::Modes::ReflowMode::Fixed);
                    terminal->grid->init(3, 8);
                    if (screen == Ubiquitty::ScreenMode::Alternate)
                        feed(*terminal, "\033[?1049h");
                }
                const auto materializeExpected = [&] {
                    for (const auto mode : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate })
                        for (int row = 0; row < expected.rows; ++row)
                            (void)expected.grid->rowCells(mode, Ubiquitty::Row(row));
                };
                materializeExpected();
                feed(actual, stream.substr(0, split));
                feed(actual, stream.substr(split));
                for (const char& byte : stream) {
                    feed(expected, { &byte, 1 });
                    materializeExpected();
                }
                CHECK(byteDriverStatesMatch(actual, expected));
                for (const auto mode : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
                    for (int row = 0; row < actual.rows; ++row) {
                        const auto a = actual.grid->rowCells(mode, Ubiquitty::Row(row));
                        const auto b = expected.grid->rowCells(mode, Ubiquitty::Row(row));
                        CHECK(std::memcmp(a.data, b.data, static_cast<std::size_t>(actual.columns) * sizeof(Ubiquitty::Cell)) == 0);
                    }
                }
                const int rows = std::min(actual.scrollbackCount, expected.scrollbackCount);
                for (int row = 0; row < rows; ++row) {
                    const auto* a = actual.lineStore->at(row);
                    const auto* b = expected.lineStore->at(row);
                    CHECK_EQ(a->length, b->length);
                    CHECK_EQ(a->wrapped, b->wrapped);
                    CHECK_EQ(a->lineAttribute, b->lineAttribute);
                    CHECK_EQ(a->mark, b->mark);
                    if (a->length == b->length && a->length > 0)
                        CHECK(std::memcmp(a->cells, b->cells, static_cast<std::size_t>(a->length) * sizeof(Ubiquitty::Cell)) == 0);
                }
            }
        }
    }
}

TEST(compactASCIIRowsStayCompactAcrossMarginsPasswordProbesAndHistory)
{
    enum class InputChunks {
        Whole,
        Bytes,
    };
    for (const auto screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        for (const auto chunks : { InputChunks::Whole, InputChunks::Bytes }) {
            Ubiquitty::Terminal terminal;
            Ubiquitty::hardReset(terminal);
            Ubiquitty::resize(terminal, 3, 12, Ubiquitty::Modes::ReflowMode::Fixed);
            terminal.grid->init(3, 12);
            terminal.passwordDetect = false;
            if (screen == Ubiquitty::ScreenMode::Alternate)
                feed(terminal, "\033[?1049h");
            const auto input = [&](std::string_view text) {
                if (chunks == InputChunks::Whole)
                    feed(terminal, text);
                else {
                    for (const char& byte : text)
                        feed(terminal, { &byte, 1 });
                }
            };
            constexpr std::string_view fullRow = "ABCDE FGHIJK";
            input(fullRow);
            CHECK(terminal.screens.active->wrapPending);
            auto bytes = terminal.grid->compactASCIIRow(Ubiquitty::Row(0));
            REQUIRE(bytes);
            REQUIRE(bytes->size() == fullRow.size());
            CHECK(std::memcmp(bytes->data(), fullRow.data(), fullRow.size()) == 0);
            input("M");
            CHECK_EQ(terminal.screens.active->cursorY, 1);
            CHECK_EQ(terminal.screens.active->cursorX, 1);
            CHECK(terminal.grid->compactASCIIRow(Ubiquitty::Row(0)).has_value());
            CHECK(terminal.grid->compactASCIIRow(Ubiquitty::Row(1)).has_value());
            terminal.passwordDetect = true;
            input("\r\nPassword: ");
            CHECK(terminal.passwordPromptActive);
            CHECK(terminal.grid->compactASCIIRow(Ubiquitty::Row(2)).has_value());
            input("\r\n");
            CHECK(!terminal.passwordPromptActive);
            for (int row = 0; row < 3; ++row)
                CHECK(terminal.grid->compactASCIIRow(Ubiquitty::Row(row)).has_value());
            if (screen == Ubiquitty::ScreenMode::Primary) {
                REQUIRE(terminal.lineStore);
                CHECK_EQ(terminal.scrollbackCount, 1);
                const auto* history = terminal.lineStore->at(0);
                REQUIRE(history);
                REQUIRE(history->length == static_cast<int>(fullRow.size()));
                for (int column = 0; column < history->length; ++column)
                    CHECK_EQ(history->cells[column].codePoint, static_cast<std::uint32_t>(fullRow[static_cast<std::size_t>(column)]));
            } else
                CHECK_EQ(terminal.scrollbackCount, 0);
            input("tail");
            CHECK(terminal.grid->compactASCIIRow(Ubiquitty::Row(2)).has_value());
            input("\033T");
            CHECK_EQ(terminal.screens.active->cursorX, 3);
            CHECK_EQ(terminal.grid->logicalCellValueAt(Ubiquitty::Row(2), Ubiquitty::Column(2))->codePoint, 'i');
            CHECK_EQ(terminal.grid->logicalCellValueAt(Ubiquitty::Row(2), Ubiquitty::Column(3))->codePoint, 0);
        }
    }
}

// Tek 4014 through the per-byte driver: every byte routes into the Tek state
// machine, and the only way back is the in-mode escape.
TEST(testTekModeRoutesThePerByteDriverToo)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Enter one byte at a time, so the decision is the per-byte driver's.
    feedPerByte(t, "\033[?38h");
    CHECK(t.isTekModeActive);

    // Alpha lands in the Tek display list rather than the grid.
    const int before = Ubiquitty::tekPrimitiveCount(t);
    feedPerByte(t, "AB");
    CHECK(Ubiquitty::tekPrimitiveCount(t) > before);
    CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint != static_cast<std::uint32_t>('A'));

    // And back out through the in-mode escape, one byte at a time.
    feedPerByte(t, "\033\003");
    CHECK(!t.isTekModeActive);
    feedPerByte(t, "v");
    CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == static_cast<std::uint32_t>('v'));
}

// A buffer whose middle enters Tek: the batched driver has to stop there and
// hand the rest to the Tek machine rather than parsing it as VT text.
TEST(testEnteringTekMidBufferStopsTheBatchedLoop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const int before = Ubiquitty::tekPrimitiveCount(t);
    Ubiquitty::processBytes(&t, std::string_view("vt-text\033[?38hTEK"));
    CHECK(t.isTekModeActive);
    // The head landed on the grid; the tail became Tek primitives.
    CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == static_cast<std::uint32_t>('v'));
    CHECK(Ubiquitty::tekPrimitiveCount(t) > before);
}

// The non-ANSI legacy grammars divert from the DEC state machine, and both
// drivers have to divert the same way: the per-byte entry cannot hoist the
// decision, so it repeats it.
TEST(testNonANSIEmulationDivertsBothDrivers)
{
    const Ubiquitty::VT::Emulation legacies[] = {
        Ubiquitty::VT::Emulation::Wyse,
        Ubiquitty::VT::Emulation::Televideo,
        Ubiquitty::VT::Emulation::Adm,
        Ubiquitty::VT::Emulation::Heath,
        Ubiquitty::VT::Emulation::Hp,
    };
    for (const Ubiquitty::VT::Emulation emulation : legacies) {
        // (a) One byte at a time.
        Ubiquitty::Terminal perByte;
        Ubiquitty::hardReset(perByte);
        Ubiquitty::Modes::setEmulation(&perByte, emulation);
        feedPerByte(perByte, "legacy");

        // (b) The same bytes in one call.
        Ubiquitty::Terminal batched;
        Ubiquitty::hardReset(batched);
        Ubiquitty::Modes::setEmulation(&batched, emulation);
        Ubiquitty::processBytes(&batched, std::string_view("legacy"));

        // Whatever the grammar did with them, it did the same both ways —
        // a driver that diverted only in bulk would differ here.
        for (int column = 0; column < 6; ++column) {
            const Ubiquitty::Cell* a = UbiquittyTest::cell(perByte, 0, column);
            const Ubiquitty::Cell* b = UbiquittyTest::cell(batched, 0, column);
            CHECK(a && b);
            if (a && b)
                CHECK(a->codePoint == b->codePoint);
        }
        CHECK(perByte.screens.active->cursorX == batched.screens.active->cursorX);
        CHECK(perByte.screens.active->cursorY == batched.screens.active->cursorY);
    }

    // An ANSI legacy (Linux) stays on the VT path in both drivers — the
    // diversion is for the non-ANSI grammars only, and picking the wrong side
    // of that line silently changes how ordinary output parses.
    Ubiquitty::Terminal linuxEmulation;
    Ubiquitty::hardReset(linuxEmulation);
    Ubiquitty::Modes::setEmulation(&linuxEmulation, Ubiquitty::VT::Emulation::Linux);
    feedPerByte(linuxEmulation, "\033[31mred");
    CHECK(UbiquittyTest::cell(linuxEmulation, 0, 0)->codePoint == static_cast<std::uint32_t>('r'));
    CHECK(UbiquittyTest::cell(linuxEmulation, 0, 0)->pen.foregroundIndex == 1);
}

// The sixel accumulator's ceiling latches for the bulk writer as well as the
// per-byte one: a payload that overflows and then keeps arriving in large
// chunks must be dropped by the chunk writer rather than re-entering the
// growth path on every chunk.
TEST(testSixelOverflowLatchesForTheBulkWriter)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    std::string frame = "\033P0;0;0q\"1;1;2;1";
    frame.append(17u * 1024u * 1024u, '~'); // past the accumulator ceiling
    Ubiquitty::processBytes(&t, frame);
    CHECK(t.images.sixelAccumulatorOverflow);
    const int lengthAtOverflow = t.images.sixelAccumulatorLength;

    // More payload, in bulk: the latch short-circuits the chunk writer, so
    // nothing more is stored.
    Ubiquitty::processBytes(&t, std::string(64u * 1024u, '~'));
    Ubiquitty::processBytes(&t, std::string(64u * 1024u, '~'));
    CHECK(t.images.sixelAccumulatorLength == lengthAtOverflow);
    CHECK(t.images.sixelAccumulatorLength <= Ubiquitty::VT::maxSixelAccumulatorCapacity);

    // The overflowed frame never becomes an image, and the next one is clean.
    Ubiquitty::processBytes(&t, std::string_view("\033\\"));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(liveImageCount(t) == 0);
    Ubiquitty::processBytes(&t, std::string_view("\033P0;0;0q#0;2;100;0;0#0~~\033\\"));
    Ubiquitty::drainAsyncDecodes(t);
    CHECK(liveImageCount(t) == 1);
}

// A grapheme cluster whose base cell is gone: a resize between the base and
// its combining mark leaves the open cluster addressing a cell the grid no
// longer has, and the append has to answer that rather than write through it.
TEST(testAClusterWhoseBaseCellVanishedIsDropped)
{
    // (a) One mark at a time.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 6, 20);
        Ubiquitty::processBytes(&t, std::string_view("\033[5;18Hbase"));
        // The cluster is open on row 4, near the right edge; shrink the grid
        // out from under it.
        Ubiquitty::resize(t, 2, 4, Ubiquitty::Modes::ReflowMode::Fixed);
        Ubiquitty::processBytes(&t, std::string_view("\xcc\x81")); // COMBINING ACUTE
        CHECK(t.rows == 2 && t.columns == 4);
    }

    // (b) A whole run of marks at once, which takes the batched append.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 6, 20);
        Ubiquitty::processBytes(&t, std::string_view("\033[5;18Hbase"));
        Ubiquitty::resize(t, 2, 4, Ubiquitty::Modes::ReflowMode::Fixed);
        Ubiquitty::processBytes(&t, std::string_view("\xcc\x81\xcc\x82\xcc\x83\xcc\x84"));
        CHECK(t.rows == 2 && t.columns == 4);
    }

    // The terminal still takes ordinary input afterwards.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 10);
    Ubiquitty::processBytes(&t, std::string_view("e\xcc\x81 fine"));
    CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == static_cast<std::uint32_t>('e'));
    CHECK(UbiquittyTest::cell(t, 0, 0)->combining[0] == 0x301u);
}

// The batched cluster scan walks a run of marks in one pass and stops at the
// first code point that does not extend the open cluster. Each way it can
// stop is a different exit, and picking the wrong one either swallows the
// next base into the cluster or re-scans it a second time.
TEST(testTheBatchedClusterScanStopsEveryWayItCan)
{
    // (a) A plain base after the marks: the scan hands the rest back so the
    // text batcher owns it. The base must not join the cluster.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 4, 20);
        Ubiquitty::processBytes(&t, std::string_view("e\xcc\x81\xcc\x82"
                                                     "abc"));
        const Ubiquitty::Cell* base = UbiquittyTest::cell(t, 0, 0);
        CHECK(base && base->codePoint == static_cast<std::uint32_t>('e'));
        CHECK(base && base->combining[0] == 0x301u);
        // The following letters are their own cells, in order.
        CHECK(UbiquittyTest::cell(t, 0, 1)->codePoint == static_cast<std::uint32_t>('a'));
        CHECK(UbiquittyTest::cell(t, 0, 2)->codePoint == static_cast<std::uint32_t>('b'));
        CHECK(UbiquittyTest::cell(t, 0, 3)->codePoint == static_cast<std::uint32_t>('c'));
    }

    // (b) A grapheme-relevant code point that starts a NEW cluster rather than
    // extending this one: the scan stops on the boundary instead of handing
    // back, because the code point is its own base.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 4, 20);
        // A flag (regional indicators) right after a marked base: both are
        // grapheme-relevant, and the rule joins neither pair across.
        Ubiquitty::processBytes(&t, std::string_view("e\xcc\x81\xf0\x9f\x87\xba\xf0\x9f\x87\xb8"));
        CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == static_cast<std::uint32_t>('e'));
        CHECK(UbiquittyTest::cell(t, 0, 0)->combining[0] == 0x301u);
        CHECK(UbiquittyTest::cell(t, 0, 1)->codePoint == 0x1F1FAu);
    }

    // (c) Both sides below the lowest combining mark: the fast reject, which
    // decides without consulting the rule table at all.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 4, 20);
        // A zero-width joiner opens a cluster whose last code point is high,
        // then two plain ASCII bases arrive in the same run.
        Ubiquitty::processBytes(&t, std::string_view("\xf0\x9f\x91\xa9\xe2\x80\x8d"
                                                     "xy"));
        CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == 0x1F469u);
    }

    // (d) The run ends mid-cluster: the next call continues it rather than
    // starting a new one, so a mark split across two reads still lands on its
    // base.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 4, 20);
        Ubiquitty::processBytes(&t, std::string_view("e\xcc\x81"));
        Ubiquitty::processBytes(&t, std::string_view("\xcc\x82"
                                                     "z"));
        const Ubiquitty::Cell* base = UbiquittyTest::cell(t, 0, 0);
        CHECK(base && base->codePoint == static_cast<std::uint32_t>('e'));
        CHECK(base && base->combining[0] == 0x301u);
        CHECK(UbiquittyTest::cell(t, 0, 1)->codePoint == static_cast<std::uint32_t>('z'));
    }
}

// Scan boundaries are protocol boundaries: no byte outside printable ASCII
// may become a cell merely because it landed inside an eight-byte load.
TEST(testBatchedASCIIStopsAtEveryByteValueAndWordOffset)
{
    checkByteRunBoundaries(ByteRunRange::PrintableASCII);
}

// The CSI lookahead uses the narrower parameter/intermediate range. Its
// boundary decides whether CCH may still cancel the preceding graphic.
TEST(testGraphicCaptureCSILookaheadStopsAtEveryByteValueAndWordOffset)
{
    checkByteRunBoundaries(ByteRunRange::CSIParameters);
}

TEST(testASCIIHorizontalTabMakesTheGraphicUnavailableToCancel)
{
    struct TabCase {
        std::string_view setup;
        int column;
        int row;
        bool isWrapPending;
    };
    const TabCase cases[] {
        { "", 8, 0, false },
        { "\033[3g", 16, 0, false },
        { "\033[3g\033[1;5H\033H\033[H", 4, 0, false },
        { "\033[1;16H", 16, 0, true },
        { "\033[?41h\033[1;16H", 8, 1, false },
        { "\033[?41h\033[3;16H", 8, 2, false },
        { "\033[?69h\033[2;6s\033[1;2H", 5, 0, false },
    };
    for (const auto screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        for (const auto& testCase : cases) {
            Ubiquitty::Terminal actual;
            Ubiquitty::Terminal expected;
            for (auto* terminal : { &actual, &expected }) {
                Ubiquitty::hardReset(*terminal);
                Ubiquitty::resize(*terminal, 3, 17, Ubiquitty::Modes::ReflowMode::Fixed);
                if (screen == Ubiquitty::ScreenMode::Alternate)
                    feed(*terminal, "\033[?1049h");
                feed(*terminal, testCase.setup);
                feed(*terminal, "AB\t");
                CHECK_EQ(terminal->screens.active->cursorX, testCase.column);
                CHECK_EQ(terminal->screens.active->cursorY, testCase.row);
                CHECK_EQ(terminal->screens.active->wrapPending, testCase.isWrapPending);
                CHECK(terminal->parser.precedingItem.kind == Ubiquitty::PrecedingItem::Kind::ControlByte);
            }
            feed(actual, "\033T");
            CHECK(byteDriverStatesMatch(actual, expected));
            // A later graphic is cancellable even when HT did not move or
            // when that graphic must wrap and scroll before being written.
            feed(actual, "X\033T");
            CHECK(byteDriverStatesMatch(actual, expected));
        }
    }
}

TEST(testASCIIHorizontalTabCancellationMatchesEveryInputSplit)
{
    for (const auto screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        for (const std::string_view setup : { "", "\033[3g", "\033[3g\033[1;5H\033H\033[H",
                 "\033[1;16H", "\033[?41h\033[3;16H", "\033[?69h\033[2;6s\033[1;2H",
                 "\033[2$~\033[1$}", "\033[3g\033[9 b\t" }) {
            const auto prepare = [&](Ubiquitty::Terminal& terminal) {
                Ubiquitty::hardReset(terminal);
                Ubiquitty::resize(terminal, 3, 17, Ubiquitty::Modes::ReflowMode::Fixed);
                if (screen == Ubiquitty::ScreenMode::Alternate)
                    feed(terminal, "\033[?1049h");
                feed(terminal, setup);
                if (setup == "\033[3g\033[9 b\t")
                    CHECK(terminal.isAlignedTabActive);
            };
            for (const std::string_view stream : { "AB\t\033T", "AB\033T\t", "AB\tCD\033T",
                     "AB\t\033TX\033T", "AB\t\xC2\x94", "AB\tX\xC2\x94", "AB\t\xCC\x81\033T" }) {
                Ubiquitty::Terminal expected;
                prepare(expected);
                for (const char& byte : stream)
                    feed(expected, { &byte, 1 });
                for (std::size_t split = 0; split <= stream.size(); ++split) {
                    Ubiquitty::Terminal actual;
                    prepare(actual);
                    feed(actual, stream.substr(0, split));
                    feed(actual, stream.substr(split));
                    CHECK(byteDriverStatesMatch(actual, expected));
                    CHECK_EQ(actual.statusCursorX, expected.statusCursorX);
                    CHECK_EQ(actual.isAlignedTabActive, expected.isAlignedTabActive);
                    for (int row = 0; row < actual.rows; ++row) {
                        const auto& actualMetadata = actual.grid->rowMeta(row);
                        const auto& expectedMetadata = expected.grid->rowMeta(row);
                        CHECK(std::memcmp(&actualMetadata, &expectedMetadata, sizeof(actualMetadata)) == 0);
                    }
                    for (int column = 0; column < actual.columns; ++column) {
                        const auto& actualCell = Ubiquitty::Rendering::statusCellAt(actual, column);
                        const auto& expectedCell = Ubiquitty::Rendering::statusCellAt(expected, column);
                        CHECK(std::memcmp(&actualCell, &expectedCell, sizeof(actualCell)) == 0);
                    }
                    for (int row = 0; row < std::min(actual.scrollbackCount, expected.scrollbackCount); ++row) {
                        const auto& actualLine = *actual.lineStore->at(row);
                        const auto& expectedLine = *expected.lineStore->at(row);
                        CHECK_EQ(actualLine.length, expectedLine.length);
                        CHECK_EQ(actualLine.wrapped, expectedLine.wrapped);
                        CHECK_EQ(actualLine.lineAttribute, expectedLine.lineAttribute);
                        CHECK_EQ(actualLine.mark, expectedLine.mark);
                        CHECK_EQ(actualLine.needsShaping, expectedLine.needsShaping);
                        if (actualLine.length > 0 && actualLine.length == expectedLine.length) {
                            CHECK(std::memcmp(actualLine.cells, expectedLine.cells,
                                      static_cast<std::size_t>(actualLine.length) * sizeof(Ubiquitty::Cell))
                                == 0);
                        }
                    }
                }
            }
        }
    }
}

TEST(testStatusHorizontalTabRetainsTheGraphicForCancel)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 3, 17, Ubiquitty::Modes::ReflowMode::Fixed);
    feed(terminal, "\033[2$~\033[1$}AB\t\033T");
    CHECK_EQ(terminal.statusCursorX, 1);
    CHECK_EQ(Ubiquitty::Rendering::statusCellAt(terminal, 0).codePoint, 'A');
    CHECK_EQ(Ubiquitty::Rendering::statusCellAt(terminal, 1).codePoint, 0u);
}

UBIQUITTY_TEST_MAIN()

// ---- residue ledger --------------------------------------------------
// VT.cpp is the most-covered large file in Core, and what remains dark is
// dominated by four classes that no input can reach:
//
// - The constexpr table-ordering checks (CSITableSortedStrict,
//   CSIPrefixedTableSorted, dcsTableSorted): evaluated at compile time by the
//   static_asserts that consume them, so the emitted bodies never run. They
//   are proofs, not code.
// - Allocation-failure arms: the sixel accumulator's reserve, the decode
//   worker's pixel buffer, the RGBA conversion, and the two payload-run
//   batchers whose OOM answer is "fall back to the per-byte path".
// - The `default:` arms of switches over enums whose every enumerator is
//   handled above them.
// - Guards a caller has already established: the null-grid checks in
//   displaySetCell, clusterAppend and clusterAppendRun (every caller holds a
//   grid), the cursor clamps' negative halves (the cursor is clamped on every
//   write), and the trigger-row assembly's `n > maxColumns` clamps (a grid is
//   never wider than that constant).
//
// Two further arms are structural duplicates rather than reachable states:
// the batched cluster scan repeats the outer loop's grapheme-relevance test
// on the code point that test just passed, and the async sixel completion's
// generation gate cannot fire because every generation bump in the tree
// (hardReset, destroyTerminal) also discards the queue the completion would
// have come from. The existing "stale completion dropped after reset" case
// passes through that teardown, not through the gate.
