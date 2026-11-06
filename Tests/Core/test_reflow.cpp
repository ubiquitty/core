// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

//
// test_reflow.cpp — a golden + property harness for grid reflow.
//
// Reflow (rewrap on resize) is the heaviest bug-churn area but had only
// behavioral integration tests. This pins it two ways:
//   - PROPERTY: the logical text of the buffer (rows joined across soft wraps,
//     scrollback + screen) is INVARIANT under any width change, and survives a
//     width round-trip. A violation means reflow lost / duplicated / reordered
//     content — a real bug, not a layout preference.
//   - GOLDEN: exact wrapped grids for known scenarios (durable because RowMeta
//     owns the reflow region).
//

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string trimTrailing(std::string s)
{
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

// A physical visible row as a string (no trimming) — for golden checks.
static std::string rowString(Ubiquitty::Terminal& t, int row)
{
    std::string s;
    for (int c = 0; c < t.columns; c++) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, row, c);
        s += (cell && cell->codePoint) ? static_cast<char>(cell->codePoint) : ' ';
    }
    return s;
}

// The whole buffer (scrollback + screen) read as LOGICAL lines: a row that is
// soft-wrapped joins the next without a break; a hard-ended row closes the
// logical line (trailing blanks trimmed). Generated content is space-free, so
// the trim is exact. Trailing empty lines (blank screen tail) are dropped.
static std::vector<std::string> logicalLines(Ubiquitty::Terminal& t)
{
    std::vector<std::string> out;
    std::string current;
    bool inLine = false;
    int sb = Ubiquitty::Screen::scrollbackCount(&t);
    for (int i = 0; i < sb; i++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (!line)
            continue;
        for (int c = 0; c < line->length; c++) {
            const std::uint32_t ch = line->cells ? line->cells[c].codePoint : 0;
            current += ch ? static_cast<char>(ch) : ' ';
        }
        inLine = true;
        if (!line->wrapped) {
            out.push_back(trimTrailing(current));
            current.clear();
            inLine = false;
        }
    }
    for (int r = 0; r < t.rows; r++) {
        current += [&] {
            std::string row;
            for (int c = 0; c < t.columns; c++) {
                const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, r, c);
                row += (cell && cell->codePoint) ? static_cast<char>(cell->codePoint) : ' ';
            }
            return row;
        }();
        inLine = true;
        if (!Ubiquitty::Screen::visibleRowWrapped(&t, 0, r)) {
            out.push_back(trimTrailing(current));
            current.clear();
            inLine = false;
        }
    }
    if (inLine)
        out.push_back(trimTrailing(current));
    while (!out.empty() && out.back().empty())
        out.pop_back();
    return out;
}

// Feed `count` space-free logical lines of varying length (some longer than the
// narrow test widths, so they wrap), each ended by a hard CR/LF break.
static std::vector<std::string> feedVariedLines(Ubiquitty::Terminal& t, int count)
{
    std::vector<std::string> sent;
    for (int i = 0; i < count; i++) {
        std::string line(static_cast<std::size_t>(8 + i * 6), static_cast<char>('A' + (i % 26)));
        sent.push_back(line);
        feed(t, line.c_str());
        feed(t, "\r\n");
    }
    return sent;
}

static void dumpDiff(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    printf("  expected %zu lines, got %zu\n", a.size(), b.size());
    for (std::size_t i = 0; i < a.size() || i < b.size(); i++) {
        const char* ea = i < a.size() ? a[i].c_str() : "<none>";
        const char* eb = i < b.size() ? b[i].c_str() : "<none>";
        if (i >= a.size() || i >= b.size() || a[i] != b[i])
            printf("  [%zu] exp=\"%s\" got=\"%s\"\n", i, ea, eb);
    }
}

// Order-sensitive fingerprint of every non-blank base code point in the buffer
// (scrollback then screen). Unlike logicalLines it keeps full code points, so wide
// (CJK) content compares exactly; a lost row changes the sequence. Stable across a
// width round-trip: it's the content sequence regardless of the scrollback/screen
// split. Wide-continuation cells appear identically before/after, so equality still
// means "nothing lost".
static std::vector<std::uint32_t> contentSignature(Ubiquitty::Terminal& t)
{
    std::vector<std::uint32_t> out;
    const int sb = Ubiquitty::Screen::scrollbackCount(&t);
    for (int i = 0; i < sb; i++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (!line)
            continue;
        for (int c = 0; c < line->length; c++) {
            const std::uint32_t ch = line->cells ? line->cells[c].codePoint : 0;
            if (ch)
                out.push_back(ch);
        }
    }
    for (int r = 0; r < t.rows; r++)
        for (int c = 0; c < t.columns; c++) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, r, c);
            if (cell && cell->codePoint)
                out.push_back(cell->codePoint);
        }
    return out;
}

// the reflow ring is sized to CONTENT, not the scrollback limit, with a
// WIDE-CELL-SAFE bound (ceil(width/(newColumns-1))+1 per input row). A double-width
// pair never splits across a wrap, so at an ODD narrow width each output row carries
// as few as newColumns-1 cells — a naive ceil(width/newColumns) bound under-sizes
// the ring and spills content that is well under the scrollback limit (real loss).
// This feeds a wide-CJK history that wraps far past the naive bound at width 5, then
// round-trips the width; the content signature must be byte-identical (no row lost).
// Verify-by-revert: change the bound's divisor from (newColumns-1) to newColumns and
// this fails — the widest lines lose their oldest wrapped rows.
// a huge scrollback LIMIT with small CONTENT must not allocate a limit-sized
// ring (that was 6.4 GB at the 1M cap — alloc fails, reflowPrimary returns early, and
// the resize leaves content un-rewrapped). Content-sized allocation succeeds, so the
// reflow actually runs and the logical text survives a width sweep.
// PROPERTY: the buffer's logical text is invariant across a sweep of widths.
// PROPERTY: a width round-trip (W -> narrow -> W) restores the logical content.
// GOLDEN: a 10-char line autowraps, then rewraps exactly on a width change.
// GOLDEN: a hard line break is not joined by reflow (stays its own logical line).
// GOLDEN: anchorAlternateBottom pins the ACTIVE alternate screen to the bottom
// across height-only changes (a top-edge drag, bottom edge fixed): a shrink
// drops the TOP rows, a grow blanks new TOP rows, and the cursor rides the
// shift. Without the flag the alt screen stays top-anchored (rows clipped /
// added at the bottom) — the default for every other resize path.
// GOLDEN: anchorBottom on the gate-suppressed primary (allowReflow=false, the
// app-owns-screen geometry-only path — claude code's inline mode) exchanges
// rows with scrollback so the BOTTOM rows stay pinned across height changes:
// shrink pushes the top rows into history (even across a trailing blank band),
// grow pulls them back. Without the flag, geometry-only stays top-anchored.
// A Modes::ReflowMode::Fixed resize (tmux/agent/sync/daemon panes: the peer owns
// authoritative content, so the client must not locally rewrap) never rewraps
// a row's cells to the new width. CellGrid::resize must therefore leave the
// row's tracked content-width metadata alone on that path — re-stamping it to
// the (narrower) new width on shrink would misrecord "content erased" for
// cells that are still physically there, and nothing ever raises the
// recorded width back up on a later grow (real characters
// beyond the shrink point would stay invisible even after growing back).
// Every cell past a row's written prefix must be BYTE-IDENTICAL to the
// canonical default blank — the value the [filled, columns) tail invariant is
// memcmp-stated against. A capacity-growing realloc (CellGrid::ensureCapacity)
// fills every newly exposed cell with that same blank, whatever headroom the
// allocation carries beyond the logical geometry.
static void checkBlankTail(Ubiquitty::Terminal& t, int row, int contentColumns)
{
    for (int c = contentColumns; c < t.columns; c++) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, row, c);
        CHECK(cell && memcmp(cell, &Ubiquitty::defaultBlankCell, sizeof(Ubiquitty::Cell)) == 0);
    }
}

// A live drag delivers a resize per event — dozens of one-cell steps, each
// potentially crossing the previous allocation. Content, tracked widths and
// blank tails must survive any grow/shrink/grow sequence identically to a
// single big resize, in both reflow modes.
// EXACT-CARRY: a width rewrap stamps each output row's needsShaping while it
// copies the cells (reflowAppendSpan), not via a second per-row rescan. Pins
// that a rewrapped row bearing cursive/RTL cells keeps needsShaping and a
// pure-ASCII row does not. Verify-by-revert: drop the accumulation (or the
// threaded flag) and the Arabic scrollback rows read needsShaping=false.
// Full-state fingerprint for the deferral equivalence oracle: dimensions,
// cursor, every scrollback line (length, flags, cell text) and every visible
// row (text, wrap flag, line attribute). Two Terms with equal fingerprints
// carry the same user-observable buffer.
static std::string bufferFingerprint(Ubiquitty::Terminal& t)
{
    std::string out;
    char header[96];
    snprintf(header, sizeof header, "dims=%dx%d sb=%d cur=%d,%d|", t.rows, t.columns, Ubiquitty::Screen::scrollbackCount(&t), t.screens.primary.cursorY, t.screens.primary.cursorX);
    out += header;
    const int sb = Ubiquitty::Screen::scrollbackCount(&t);
    for (int i = 0; i < sb; i++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (!line) {
            out += "<null>";
            continue;
        }
        char meta[48];
        snprintf(meta, sizeof meta, "[%d,%d,%u,%u]", line->length, line->wrapped ? 1 : 0, line->lineAttribute, line->mark);
        out += meta;
        for (int c = 0; c < line->length; c++) {
            const std::uint32_t ch = line->cells ? line->cells[c].codePoint : 0;
            out += ch ? static_cast<char>(ch) : ' ';
        }
        out += '\n';
    }
    out += "--screen--\n";
    for (int r = 0; r < t.rows; r++) {
        out += rowString(t, r);
        out += Ubiquitty::Screen::visibleRowWrapped(&t, 0, r) ? "~" : ".";
        out += '\n';
    }
    return out;
}

// the equivalence oracle, both regimes:
// (a) content that fits the scrollback limit at every step: N Interactive
//     (live-drag) width steps + one settle are bit-for-bit identical to the
//     same N steps applied Settled — the deferral changes when the history
//     rewraps, never the result.
// (b) content that would OVERFLOW the limit at a narrow intermediate width:
//     Settled-per-step evicts the oldest lines while squeezed through the
//     narrow widths; the deferral never materializes those transients, so
//     Interactive + settle equals one Settled resize to the final width — the
//     drag's end state, with no history lost to widths the user only passed
//     through. That asymmetry is the deferral's intended semantic.
// Logical content is invariant even MID-deferral: with the history wrap-stale
// (mixed stored widths), the buffer still reads back the same logical lines —
// the transient state a user scrolling mid-drag would render.
// Height-only Interactive steps keep the O(rows) fast path: no wrap-stale
// marker, and the result equals the Settled height change.
// A drag that returns to its starting width still settles: the re-appended
// overflow rows and the wrap-stale marker are real even though the final
// dimensions match the pre-drag ones.
// The history/screen SEAM: a logical line long enough that its head rows
// scrolled into history while its tail is still on screen (the newest history
// rows carry the soft-wrap join). An Interactive step must pull and rewrap
// that joined run whole — splitting it would tear the logical line at the seam.
TEST(testReflowPreservesLogicalContentAcrossWidths)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 40);
    std::vector<std::string> sent = feedVariedLines(t, 8);
    std::vector<std::string> baseline = logicalLines(t);
    CHECK(baseline == sent); // the extractor agrees with what we fed

    const int widths[] = { 20, 30, 60, 80, 13, 40, 7, 100 };
    for (int w : widths) {
        Ubiquitty::resize(t, 24, w);
        std::vector<std::string> got = logicalLines(t);
        if (got != sent) {
            printf("width %d: logical content changed under reflow\n", w);
            dumpDiff(sent, got);
        }
        CHECK(got == sent);
    }
}

TEST(testReflowCarriesShapingFlag)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 40);

    // 16 wrapping lines alternating pure-ASCII and Arabic (U+0628 BEH, UTF-8
    // D8 A8) — at 6 rows most spill into scrollback, so the width shrink below
    // exercises the full-history rewrap on both kinds of row.
    for (int i = 0; i < 16; i++) {
        if (i % 2 == 0) {
            std::string ascii(34, static_cast<char>('A' + i));
            feed(t, ascii.c_str());
        } else {
            for (int k = 0; k < 30; k++)
                feed(t, "\xD8\xA8");
        }
        feed(t, "\r\n");
    }

    Ubiquitty::resize(t, 6, 12); // width change -> full history rewrap

    // Every scrollback row the rewrap produced: a BEH-bearing row must carry
    // needsShaping (exact), a pure-ASCII row must leave it clear (freshly
    // built, so no stale-superset noise as the live rowMeta can have).
    int arabicRows = 0, arabicShaped = 0, asciiRows = 0, asciiClear = 0;
    const int sb = Ubiquitty::Screen::scrollbackCount(&t);
    for (int i = 0; i < sb; i++) {
        const Ubiquitty::Screen::ScrollLine* line = Ubiquitty::Screen::scrollbackLine(&t, i);
        if (!line || !line->length || !line->cells)
            continue;
        bool arabic = false;
        for (int c = 0; c < line->length; c++)
            if (line->cells[c].codePoint == 0x0628u) {
                arabic = true;
                break;
            }
        if (arabic) {
            arabicRows++;
            if (line->needsShaping)
                arabicShaped++;
        } else {
            asciiRows++;
            if (!line->needsShaping)
                asciiClear++;
        }
    }
    // Live rows too (reflow rebuilds them): only the positive direction, since
    // rowMeta.has(Ubiquitty::RowFlag::NeedsShaping) is a set-only superset that never clears.
    for (int r = 0; r < t.rows; r++) {
        bool arabic = false;
        for (int c = 0; c < t.columns; c++) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, r, c);
            if (cell && cell->codePoint == 0x0628u) {
                arabic = true;
                break;
            }
        }
        if (arabic) {
            arabicRows++;
            if (Ubiquitty::Screen::rowMayNeedShaping(t, 0, r))
                arabicShaped++;
        }
    }

    if (arabicRows == 0 || asciiRows == 0)
        printf("shaping-flag test saw arabicRows=%d asciiRows=%d (want both > 0)\n", arabicRows, asciiRows);
    CHECK(arabicRows > 0);
    CHECK(asciiRows > 0);
    CHECK(arabicShaped == arabicRows); // every Arabic row kept the flag
    CHECK(asciiClear == asciiRows); // no ASCII row falsely set it
}

TEST(testReflowWidthRoundtrip)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 20, 50);
    std::vector<std::string> sent = feedVariedLines(t, 6);
    std::vector<std::string> at50 = logicalLines(t);

    Ubiquitty::resize(t, 20, 11); // squeeze
    Ubiquitty::resize(t, 20, 50); // back
    std::vector<std::string> roundtrip = logicalLines(t);
    if (roundtrip != at50) {
        printf("width round-trip 50->11->50 changed logical content\n");
        dumpDiff(at50, roundtrip);
    }
    CHECK(roundtrip == at50);
}

TEST(testReflowGoldenRewrap)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 4);
    feed(t, "abcdefghij"); // 10 chars, autowrap at width 4

    CHECK(rowString(t, 0) == "abcd");
    CHECK(rowString(t, 1) == "efgh");
    CHECK(rowString(t, 2).substr(0, 2) == "ij");
    CHECK(Ubiquitty::Screen::visibleRowWrapped(&t, 0, 0));
    CHECK(Ubiquitty::Screen::visibleRowWrapped(&t, 0, 1));
    CHECK(!Ubiquitty::Screen::visibleRowWrapped(&t, 0, 2));

    Ubiquitty::resize(t, 6, 5); // rewrap to width 5
    CHECK(rowString(t, 0) == "abcde");
    CHECK(rowString(t, 1) == "fghij");
    CHECK(Ubiquitty::Screen::visibleRowWrapped(&t, 0, 0));
    CHECK(!Ubiquitty::Screen::visibleRowWrapped(&t, 0, 1));
}

TEST(testReflowGoldenHardBreakPreserved)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 8);
    feed(t, "AAA\r\nBBB"); // two hard-broken short lines
    Ubiquitty::resize(t, 6, 20); // widen — must not merge AAA+BBB
    CHECK(rowString(t, 0).substr(0, 3) == "AAA");
    CHECK(rowString(t, 1).substr(0, 3) == "BBB");
    CHECK(!Ubiquitty::Screen::visibleRowWrapped(&t, 0, 0)); // hard break, not a soft wrap
}

TEST(testVisibleTopHeightGrowthKeepsHistoryAboveTheScreen)
{
    for (const auto reflow : { Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::ReflowMode::Fixed }) {
        for (const auto urgency : { Ubiquitty::Modes::ResizeUrgency::Settled, Ubiquitty::Modes::ResizeUrgency::Interactive }) {
            Ubiquitty::Terminal terminal;
            Ubiquitty::hardReset(terminal);
            Ubiquitty::resize(terminal, 4, 8);
            feed(terminal, "L1\r\nL2\r\nL3\r\nL4\r\nL5\r\nL6\0337\033[2;2H");
            REQUIRE(terminal.scrollbackCount == 2);
            REQUIRE(rowString(terminal, 0) == "L3      ");
            const auto content = logicalLines(terminal);

            Ubiquitty::resize(terminal, 7, 8, reflow, Ubiquitty::Modes::AnchorMode::VisibleTop, urgency);

            CHECK(terminal.scrollbackCount == 2);
            CHECK(logicalLines(terminal) == content);
            for (int row = 0; row < 4; ++row) {
                CHECK(trimTrailing(rowString(terminal, row)) == "L" + std::to_string(row + 3));
            }
            for (int row = 4; row < terminal.rows; ++row) {
                checkBlankTail(terminal, row, 0);
                CHECK(!Ubiquitty::Screen::visibleRowWrapped(&terminal, 0, row));
            }
            CHECK(terminal.screens.primary.cursorY == 1);
            CHECK(terminal.screens.primary.cursorX == 1);
            CHECK(terminal.screens.primary.savedY == 3);
            CHECK(terminal.screens.primary.savedX == 2);
            CHECK(terminal.pendingHistoryRewrapColumns == 0);
        }
    }
}

static void feedVisibleTopSeam(Ubiquitty::Terminal& terminal)
{
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 4, 8);
    feed(terminal, "H1\r\nH2\r\nabcdefghIJKLMNOPqrstuvwxYZ0123456789ABCD\r\nTAIL\0337");
}

TEST(testVisibleTopWidthGrowthMapsTheHistorySeamWithoutLosingText)
{
    Ubiquitty::Terminal terminal;
    feedVisibleTopSeam(terminal);
    REQUIRE(terminal.scrollbackCount == 4);
    REQUIRE(rowString(terminal, 0) == "qrstuvwx");
    const auto content = logicalLines(terminal);

    Ubiquitty::resize(terminal, 7, 12, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::VisibleTop);

    CHECK(terminal.scrollbackCount == 3);
    CHECK(rowString(terminal, 0) == "MNOPqrstuvwx");
    CHECK(rowString(terminal, 1) == "YZ0123456789");
    CHECK(rowString(terminal, 2) == "ABCD        ");
    CHECK(rowString(terminal, 3) == "TAIL        ");
    CHECK(Ubiquitty::Screen::visibleRowWrapped(&terminal, 0, 0));
    CHECK(Ubiquitty::Screen::visibleRowWrapped(&terminal, 0, 1));
    CHECK(!Ubiquitty::Screen::visibleRowWrapped(&terminal, 0, 2));
    for (int row = 4; row < terminal.rows; ++row) {
        checkBlankTail(terminal, row, 0);
        CHECK(!Ubiquitty::Screen::visibleRowWrapped(&terminal, 0, row));
    }
    CHECK(logicalLines(terminal) == content);
    CHECK(terminal.screens.primary.cursorY == 3);
    CHECK(terminal.screens.primary.cursorX == 4);
    CHECK(terminal.screens.primary.savedY == 3);
    CHECK(terminal.screens.primary.savedX == 4);
}

TEST(testVisibleTopFallsBackWhenTheMappedSuffixCannotFit)
{
    for (const auto urgency : { Ubiquitty::Modes::ResizeUrgency::Settled, Ubiquitty::Modes::ResizeUrgency::Interactive }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::Terminal reference;
        feedVisibleTopSeam(terminal);
        feedVisibleTopSeam(reference);
        const auto content = logicalLines(terminal);

        Ubiquitty::resize(terminal, 5, 4, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::VisibleTop, urgency);
        Ubiquitty::settleResize(&terminal);
        Ubiquitty::resize(reference, 5, 4);

        CHECK(bufferFingerprint(terminal) == bufferFingerprint(reference));
        CHECK(logicalLines(terminal) == content);
        CHECK(terminal.screens.primary.savedY == reference.screens.primary.savedY);
        CHECK(terminal.screens.primary.savedX == reference.screens.primary.savedX);
    }
}

TEST(testVisibleTopInteractiveReflowKeepsItsAnchorWhenSettled)
{
    Ubiquitty::Terminal interactive;
    Ubiquitty::Terminal reference;
    feedVisibleTopSeam(interactive);
    feedVisibleTopSeam(reference);
    const auto content = logicalLines(interactive);

    Ubiquitty::resize(interactive, 7, 12, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::VisibleTop, Ubiquitty::Modes::ResizeUrgency::Interactive);
    REQUIRE(rowString(interactive, 0) == "MNOPqrstuvwx");
    CHECK(interactive.scrollbackCount == 3);
    CHECK(interactive.pendingHistoryRewrapColumns == 12);
    CHECK(logicalLines(interactive) == content);
    const auto beforeSettlement = bufferFingerprint(interactive);

    Ubiquitty::settleResize(&interactive);
    Ubiquitty::resize(reference, 7, 12, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::VisibleTop);

    CHECK(interactive.pendingHistoryRewrapColumns == 0);
    CHECK(bufferFingerprint(interactive) == beforeSettlement);
    CHECK(bufferFingerprint(interactive) == bufferFingerprint(reference));
    CHECK(logicalLines(interactive) == content);
    CHECK(interactive.screens.primary.savedY == reference.screens.primary.savedY);
    CHECK(interactive.screens.primary.savedX == reference.screens.primary.savedX);
}

TEST(testVisibleTopWidthReflowMapsASavedCursorIndependently)
{
    for (const auto urgency : { Ubiquitty::Modes::ResizeUrgency::Settled, Ubiquitty::Modes::ResizeUrgency::Interactive }) {
        Ubiquitty::Terminal terminal;
        feedVisibleTopSeam(terminal);
        feed(terminal, "\033[3;2H\0337\033[4;5H");
        const auto content = logicalLines(terminal);

        Ubiquitty::resize(terminal, 7, 12, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::VisibleTop, urgency);
        CHECK(terminal.screens.primary.cursorY == 3);
        CHECK(terminal.screens.primary.cursorX == 4);
        CHECK(terminal.screens.primary.savedY == 1);
        CHECK(terminal.screens.primary.savedX == 9);
        Ubiquitty::settleResize(&terminal);
        CHECK(terminal.screens.primary.savedY == 1);
        CHECK(terminal.screens.primary.savedX == 9);
        CHECK(logicalLines(terminal) == content);

        feed(terminal, "\0338");
        CHECK(terminal.screens.primary.cursorY == 1);
        CHECK(terminal.screens.primary.cursorX == 9);
        CHECK(rowString(terminal, terminal.screens.primary.cursorY)[terminal.screens.primary.cursorX] == '7');
    }
}

TEST(testVisibleTopDimensionNoopSettlesWithThePendingAnchor)
{
    Ubiquitty::Terminal terminal;
    feedVisibleTopSeam(terminal);
    Ubiquitty::resize(terminal, 7, 12, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::VisibleTop, Ubiquitty::Modes::ResizeUrgency::Interactive);
    REQUIRE(terminal.pendingHistoryRewrapColumns == 12);
    REQUIRE(rowString(terminal, 0) == "MNOPqrstuvwx");
    const auto beforeSettlement = bufferFingerprint(terminal);

    Ubiquitty::resize(terminal, terminal.rows, terminal.columns);

    CHECK(terminal.pendingHistoryRewrapColumns == 0);
    CHECK(terminal.pendingHistoryRewrapAnchor == Ubiquitty::Modes::AnchorMode::Top);
    CHECK(bufferFingerprint(terminal) == beforeSettlement);
}

TEST(testVisibleTopPendingAnchorDoesNotOverrideANewDefaultResize)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::Terminal reference;
    feedVisibleTopSeam(terminal);
    feedVisibleTopSeam(reference);
    Ubiquitty::resize(terminal, 7, 12, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::VisibleTop, Ubiquitty::Modes::ResizeUrgency::Interactive);
    REQUIRE(terminal.pendingHistoryRewrapColumns == 12);

    Ubiquitty::resize(terminal, 8, 12);
    Ubiquitty::resize(reference, 8, 12);

    CHECK(terminal.pendingHistoryRewrapColumns == 0);
    CHECK(terminal.pendingHistoryRewrapAnchor == Ubiquitty::Modes::AnchorMode::Top);
    CHECK(rowString(terminal, 0) == "H1          ");
    CHECK(bufferFingerprint(terminal) == bufferFingerprint(reference));
}

TEST(testVisibleTopSurvivesHistoryEvictionDuringWidthReflow)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::setScrollbackLimit(&terminal, 24);
    Ubiquitty::resize(terminal, 4, 12);
    for (int line = 0; line < 24; ++line) {
        feed(terminal, std::string(12, static_cast<char>('A' + line)));
        feed(terminal, "\r\n");
    }
    feed(terminal, "L1\r\nL2\r\nL3\r\nL4");
    feed(terminal, "\033[3;2H\0337\033[4;3H");
    REQUIRE(terminal.scrollbackCount == 24);
    REQUIRE(trimTrailing(rowString(terminal, 0)) == "L1");

    Ubiquitty::resize(terminal, 6, 4, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::VisibleTop);

    CHECK(terminal.scrollbackCount == 24);
    for (int row = 0; row < 4; ++row) {
        CHECK(trimTrailing(rowString(terminal, row)) == "L" + std::to_string(row + 1));
    }
    checkBlankTail(terminal, 4, 0);
    checkBlankTail(terminal, 5, 0);
    CHECK(terminal.screens.primary.cursorY == 3);
    CHECK(terminal.screens.primary.cursorX == 2);
    CHECK(terminal.screens.primary.savedY == 2);
    CHECK(terminal.screens.primary.savedX == 1);
    std::vector<std::string> retained;
    for (int line = 16; line < 24; ++line) {
        retained.emplace_back(12, static_cast<char>('A' + line));
    }
    for (int line = 1; line <= 4; ++line) {
        retained.push_back("L" + std::to_string(line));
    }
    CHECK(logicalLines(terminal) == retained);
}

// A top-edge drag asks for the bottom anchor, and the alternate screen does not
// take it: a full-screen program lays out from row 0 and repaints the rest on
// SIGWINCH, so a shrink drops the bottom rows and keeps its top bar, and a grow
// adds blank rows under the last one.
TEST(testAlternateScreenKeepsItsTopRowAcrossABottomAnchoredResize)
{
    for (const int shrink : { 1, 2, 3 }) {
        for (const auto reflow : { Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::ReflowMode::Fixed }) {
            Ubiquitty::Terminal t;
            Ubiquitty::hardReset(t);
            Ubiquitty::resize(t, 6, 8);
            feed(t, "\x1b[?1049h"); // enter the alternate screen
            feed(t, "\x1b[1;1HA1\x1b[2;1HB2\x1b[3;1HC3\x1b[4;1HD4\x1b[5;1HE5\x1b[6;1HF6");
            CHECK(t.screens.alternate.cursorY == 5);

            const int rows = 6 - shrink;
            Ubiquitty::resize(t, rows, 8, reflow, Ubiquitty::Modes::AnchorMode::Bottom);
            CHECK(t.rows == rows);
            const char* const labels[] = { "A1", "B2", "C3", "D4", "E5", "F6" };
            for (int row = 0; row < rows; ++row)
                CHECK(rowString(t, row).substr(0, 2) == labels[row]);
            CHECK(t.screens.alternate.cursorY == rows - 1); // clamped onto the last row

            // Grown back, the top row still leads. A reflowing screen blanks
            // the rows the shrink dropped, which the program repaints; a
            // fixed one reveals what its backing store still holds.
            Ubiquitty::resize(t, 6, 8, reflow, Ubiquitty::Modes::AnchorMode::Bottom);
            CHECK(rowString(t, 0).substr(0, 2) == "A1");
            if (reflow == Ubiquitty::Modes::ReflowMode::Reflow)
                CHECK(trimTrailing(rowString(t, 5)).empty());
        }
    }
}

// The primary screen does take the bottom anchor: a shrink by one, two or three
// rows ages its top rows into history and keeps the prompt on the last row.
TEST(testPrimaryScreenKeepsItsBottomRowAcrossABottomAnchoredShrink)
{
    for (const int shrink : { 1, 2, 3 }) {
        for (const auto reflow : { Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::ReflowMode::Fixed }) {
            Ubiquitty::Terminal t;
            Ubiquitty::hardReset(t);
            Ubiquitty::resize(t, 6, 8);
            feed(t, "L1\r\nL2\r\nL3\r\nL4\r\nL5\r\nL6");
            CHECK(t.scrollbackCount == 0);

            const int rows = 6 - shrink;
            Ubiquitty::resize(t, rows, 8, reflow, Ubiquitty::Modes::AnchorMode::Bottom);
            CHECK(t.rows == rows);
            CHECK(rowString(t, 0).substr(0, 2) == std::string("L") + std::to_string(shrink + 1));
            CHECK(rowString(t, rows - 1).substr(0, 2) == "L6");
            CHECK(t.scrollbackCount == shrink);
            CHECK(t.screens.primary.cursorY == rows - 1);
        }
    }
}

TEST(testPrimaryBottomAnchorGeometryOnlyHeightResize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 6, 8);
    // 8 hard lines on 6 rows: L1/L2 scroll into history, screen = L3..L8.
    feed(t, "L1\r\nL2\r\nL3\r\nL4\r\nL5\r\nL6\r\nL7\r\nL8");
    CHECK(t.scrollbackCount == 2);
    CHECK(rowString(t, 0).substr(0, 2) == "L3");
    CHECK(t.screens.primary.cursorY == 5);

    // Gate-suppressed shrink, anchored: L3 pushes to history, L4..L8 -> rows 0..4.
    Ubiquitty::resize(t, 5, 8, Ubiquitty::Modes::ReflowMode::Fixed, Ubiquitty::Modes::AnchorMode::Bottom);
    CHECK(rowString(t, 0).substr(0, 2) == "L4");
    CHECK(rowString(t, 4).substr(0, 2) == "L8");
    CHECK(t.scrollbackCount == 3);
    CHECK(t.screens.primary.cursorY == 4);

    // Gate-suppressed grow, anchored: L3 comes back from history on top.
    Ubiquitty::resize(t, 6, 8, Ubiquitty::Modes::ReflowMode::Fixed, Ubiquitty::Modes::AnchorMode::Bottom);
    CHECK(rowString(t, 0).substr(0, 2) == "L3");
    CHECK(rowString(t, 5).substr(0, 2) == "L8");
    CHECK(t.scrollbackCount == 2);
    CHECK(t.screens.primary.cursorY == 5);

    // Grow past the history: blanks pad the TOP, pulled lines sit above content.
    Ubiquitty::resize(t, 9, 8, Ubiquitty::Modes::ReflowMode::Fixed, Ubiquitty::Modes::AnchorMode::Bottom);
    CHECK(trimTrailing(rowString(t, 0)).empty());
    CHECK(rowString(t, 1).substr(0, 2) == "L1");
    CHECK(rowString(t, 2).substr(0, 2) == "L2");
    CHECK(rowString(t, 3).substr(0, 2) == "L3");
    CHECK(rowString(t, 8).substr(0, 2) == "L8");
    CHECK(t.scrollbackCount == 0);
    CHECK(t.screens.primary.cursorY == 8);

    // Without the flag the geometry-only shrink clips the BOTTOM (top-anchored).
    Ubiquitty::resize(t, 8, 8, Ubiquitty::Modes::ReflowMode::Fixed);
    CHECK(trimTrailing(rowString(t, 0)).empty());
    CHECK(rowString(t, 3).substr(0, 2) == "L3");
    CHECK(rowString(t, 7).substr(0, 2) == "L7"); // L8 clipped off the bottom
}

// A resize is not a print: the deferred-wrap latch armed by filling the last
// column survives it exactly while the cursor still sits at the margin. A
// height change leaves the cursor (and the latch) where they were, so the
// next print wraps instead of overwriting the margin cell.
TEST(testPendingWrapSurvivesAHeightResize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    feed(t, "0123456789"); // fills row 0; latch armed, cursor on column 9
    CHECK(t.screens.primary.wrapPending);
    CHECK(t.screens.primary.cursorX == 9);

    Ubiquitty::resize(t, 8, 10);
    CHECK(t.screens.primary.wrapPending);
    CHECK(t.screens.primary.cursorX == 9);

    feed(t, "Y");
    // Wrapped, not overwritten: row 0 intact, Y opens row 1.
    CHECK(rowString(t, 0).substr(0, 10) == "0123456789");
    CHECK(rowString(t, 1).substr(0, 1) == "Y");
}

// Widening moves the margin past the cursor: there is room now, so the latch
// clears and the next print lands on the cursor cell rather than wrapping.
TEST(testPendingWrapClearsWhenWideningMakesRoom)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    feed(t, "0123456789");
    CHECK(t.screens.primary.wrapPending);

    Ubiquitty::resize(t, 5, 20);
    CHECK(!t.screens.primary.wrapPending);

    feed(t, "X");
    CHECK(rowString(t, 0).substr(0, 10) == "012345678X");
    CHECK(t.screens.primary.cursorX == 10);
}

// Under DECSLRM the latch arms at the region's right margin, not the edge —
// and a height-only resize preserves that margin, so the latch survives it
// the same way: the next print wraps instead of overwriting the margin cell.
TEST(testPendingWrapSurvivesHeightResizeInsideMargins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 20);
    feed(t, "\x1b[?69h\x1b[6;15s"); // margins over 1-based columns 6..15
    CHECK(t.screens.primary.scrollRight == 14);
    feed(t, "\x1b[1;6H"); // row 0, first margin column
    feed(t, "0123456789"); // fills to the region margin; latch armed there
    CHECK(t.screens.primary.wrapPending);
    CHECK(t.screens.primary.cursorX == 14);

    Ubiquitty::resize(t, 12, 20);
    CHECK(t.screens.primary.scrollRight == 14);
    CHECK(t.screens.primary.wrapPending);

    feed(t, "Y");
    CHECK(rowString(t, 0).substr(5, 10) == "0123456789");
    CHECK(rowString(t, 1).substr(5, 1) == "Y");
}

// The alternate screen carries its own latch through the same rule: fill its
// margin, change only the height, and the next print wraps.
TEST(testPendingWrapSurvivesHeightResizeOnAlternateScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 10, 20);
    feed(t, "\x1b[?1049h"); // alternate screen, cleared; the cursor is already home
    feed(t, "01234567890123456789"); // fills row 0; latch armed at the edge
    CHECK(t.screens.active == &t.screens.alternate);
    CHECK(t.screens.alternate.wrapPending);

    Ubiquitty::resize(t, 12, 20);
    CHECK(t.screens.alternate.wrapPending);

    feed(t, "Z");
    CHECK(rowString(t, 0).substr(0, 20) == "01234567890123456789");
    CHECK(rowString(t, 1).substr(0, 1) == "Z");
    feed(t, "\x1b[?1049l");
}

// Narrowing clamps the cursor to the margin it still faces, so the latch
// survives that too: the next print wraps rather than overwriting it.
TEST(testPendingWrapSurvivesNarrowingToTheMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 20);
    feed(t, "01234567890123456789"); // fills row 0; latch armed at column 19
    CHECK(t.screens.primary.wrapPending);

    Ubiquitty::resize(t, 5, 10);
    CHECK(t.screens.primary.wrapPending);

    feed(t, "W");
    CHECK(t.screens.primary.cursorX == 1);
    const Ubiquitty::Cell* wrapped = Ubiquitty::Screen::visibleCell(&t, 0, 2, 0);
    CHECK(wrapped != nullptr && wrapped->codePoint == 'W');
}

TEST(testFixedResizePreservesRowFillAcrossWidthShrinkGrow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // 40 of 80 columns written; CellGrid tracks that width as the parser goes.
    feed(t, "0123456789012345678901234567890123456789");
    CHECK(t.grid->rowContentWidth(Ubiquitty::Row(0)) >= 40);

    // Shrink well past the written content, then grow back — geometry-only,
    // exactly as a tmux pane resize (Session::Pane::setGeometry) does it.
    Ubiquitty::resize(t, 24, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::resize(t, 24, 80, Ubiquitty::Modes::ReflowMode::Fixed);
    // The real characters at columns [10, 40) must still be there and still
    // reported as filled — physically present, and invisible to
    // whatever trusts rowContentWidth() to skip painting past it.
    CHECK(t.grid->rowContentWidth(Ubiquitty::Row(0)) >= 40);
    CHECK(rowString(t, 0).substr(0, 40) == "0123456789012345678901234567890123456789");

    // A capacity-GROWING realloc (past what was ever allocated before) must
    // carry the same tracked width forward too (CellGrid::ensureCapacity).
    Ubiquitty::resize(t, 24, 10, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::resize(t, 24, 200, Ubiquitty::Modes::ReflowMode::Fixed);
    CHECK(t.grid->rowContentWidth(Ubiquitty::Row(0)) >= 40);
    CHECK(rowString(t, 0).substr(0, 40) == "0123456789012345678901234567890123456789");
}

TEST(testGrowShrinkGrowStepsPreserveContentAndBlankTails)
{
    // FIXED mode: cells never rewrap; content + tracked width ride across
    // every capacity change.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 10, 20, Ubiquitty::Modes::ReflowMode::Fixed);
        feed(t, "ABCDEFGH\r\nIJKL");
        CHECK(t.grid->rowContentWidth(Ubiquitty::Row(0)) >= 8);
        // Enlarging drag: one-cell width steps, then one-cell height steps.
        for (int w = 21; w <= 60; w++)
            Ubiquitty::resize(t, 10, w, Ubiquitty::Modes::ReflowMode::Fixed);
        for (int h = 11; h <= 30; h++)
            Ubiquitty::resize(t, h, 60, Ubiquitty::Modes::ReflowMode::Fixed);
        CHECK(rowString(t, 0).substr(0, 8) == "ABCDEFGH");
        CHECK(rowString(t, 1).substr(0, 4) == "IJKL");
        CHECK(t.grid->rowContentWidth(Ubiquitty::Row(0)) >= 8);
        checkBlankTail(t, 0, 8);
        checkBlankTail(t, 25, 0); // row revealed by the height drag
        // Shrink hard, then grow far past every prior allocation at once.
        Ubiquitty::resize(t, 5, 8, Ubiquitty::Modes::ReflowMode::Fixed);
        Ubiquitty::resize(t, 40, 120, Ubiquitty::Modes::ReflowMode::Fixed);
        CHECK(rowString(t, 0).substr(0, 8) == "ABCDEFGH");
        CHECK(t.grid->rowContentWidth(Ubiquitty::Row(0)) >= 8);
        checkBlankTail(t, 0, 8);
        checkBlankTail(t, 39, 0);
    }
    // REFLOW mode: the buffer's logical text is invariant across the same
    // drag-plus-round-trip churn.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 24, 40);
        const std::vector<std::string> sent = feedVariedLines(t, 8);
        for (int w = 41; w <= 100; w++)
            Ubiquitty::resize(t, 24, w);
        Ubiquitty::resize(t, 24, 30); // shrink
        Ubiquitty::resize(t, 24, 100); // grow again
        const std::vector<std::string> got = logicalLines(t);
        if (got != sent) {
            printf("FAIL %s:%d: reflow drag churn changed logical content\n", __FILE__, __LINE__);
            dumpDiff(sent, got);
            failures++;
        }
    }
}

TEST(testReflowRingBoundWidePairs)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::setScrollbackLimit(&t, 100000); // content stays well under the limit
    Ubiquitty::resize(t, 24, 40);
    // 40 lines of repeated CJK (each cell double-width), long enough that at width 5
    // (2 wide chars per row, 1 wasted column) the row count exceeds the naive bound.
    for (int i = 0; i < 40; i++) {
        std::string line;
        for (int c = 0; c < 60; c++)
            line += "\xE4\xB8\x96"; // U+4E16 世 (wide)
        feed(t, line.c_str());
        feed(t, "\r\n");
    }
    const std::vector<std::uint32_t> before = contentSignature(t);
    CHECK(!before.empty());
    Ubiquitty::resize(t, 24, 5); // odd narrow — worst case for wide-pair straddle
    Ubiquitty::resize(t, 24, 40); // back
    const std::vector<std::uint32_t> after = contentSignature(t);
    if (before != after)
        printf("wide-pair reflow lost content: before=%zu after=%zu codepoints\n", before.size(), after.size());
    CHECK(before == after);
}

TEST(testReflowRingContentNotLimit)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::setScrollbackLimit(&t, 1000000); // a limit-sized ring would be multi-GB
    Ubiquitty::resize(t, 24, 40);
    std::vector<std::string> sent = feedVariedLines(t, 12);
    for (int w : { 20, 7, 60, 13, 40 }) {
        Ubiquitty::resize(t, 24, w);
        std::vector<std::string> got = logicalLines(t);
        if (got != sent) {
            printf("1M-limit width %d: content lost — ring likely alloc-failed at limit size\n", w);
            dumpDiff(sent, got);
        }
        CHECK(got == sent);
    }
}

TEST(testInteractiveResizeSettlesToSettledEquivalence)
{
    const int steps[] = { 100, 87, 61, 42, 55, 78, 90, 120, 84 };

    { // (a) no-spill regime: exact sequence equivalence.
        Ubiquitty::Terminal interactive, settled;
        Ubiquitty::hardReset(interactive);
        Ubiquitty::hardReset(settled);
        Ubiquitty::resize(interactive, 24, 120);
        Ubiquitty::resize(settled, 24, 120);
        feedVariedLines(interactive, 40);
        feedVariedLines(settled, 40);

        for (int w : steps)
            Ubiquitty::resize(interactive, 24, w, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
        CHECK(interactive.pendingHistoryRewrapColumns == 84); // wrap-stale, target = final width
        Ubiquitty::settleResize(&interactive);
        CHECK(interactive.pendingHistoryRewrapColumns == 0);

        for (int w : steps)
            Ubiquitty::resize(settled, 24, w);

        const std::string a = bufferFingerprint(interactive);
        const std::string b = bufferFingerprint(settled);
        CHECK(a == b);
    }

    { // (b) spill regime: equivalence to the single final-width resize.
        Ubiquitty::Terminal interactive, finalOnly;
        Ubiquitty::hardReset(interactive);
        Ubiquitty::hardReset(finalOnly);
        Ubiquitty::resize(interactive, 24, 120);
        Ubiquitty::resize(finalOnly, 24, 120);
        feedVariedLines(interactive, 120);
        feedVariedLines(finalOnly, 120);

        for (int w : steps)
            Ubiquitty::resize(interactive, 24, w, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
        Ubiquitty::settleResize(&interactive);

        Ubiquitty::resize(finalOnly, 24, 84);

        const std::string a = bufferFingerprint(interactive);
        const std::string b = bufferFingerprint(finalOnly);
        CHECK(a == b);
        if (a != b) {
            FILE* fa = fopen("/tmp/oracle_a.txt", "w");
            FILE* fb = fopen("/tmp/oracle_b.txt", "w");
            if (fa) {
                fputs(a.c_str(), fa);
                fclose(fa);
            }
            if (fb) {
                fputs(b.c_str(), fb);
                fclose(fb);
            }
        }
    }
}

TEST(testInteractiveMiddragContentIntact)
{
    Ubiquitty::Terminal t, reference;
    Ubiquitty::hardReset(t);
    Ubiquitty::hardReset(reference);
    Ubiquitty::resize(t, 24, 120);
    Ubiquitty::resize(reference, 24, 120);
    const std::vector<std::string> sent = feedVariedLines(t, 90);
    feedVariedLines(reference, 90);

    Ubiquitty::resize(t, 24, 73, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
    CHECK(t.pendingHistoryRewrapColumns == 73);
    const std::vector<std::string> mid = logicalLines(t);
    const std::vector<std::string> ref = logicalLines(reference);
    CHECK(mid == ref);

    // A settle mid-sequence then further Interactive steps still converge.
    Ubiquitty::settleResize(&t);
    Ubiquitty::resize(t, 24, 96, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
    Ubiquitty::settleResize(&t);
    Ubiquitty::resize(reference, 24, 73);
    Ubiquitty::resize(reference, 24, 96);
    CHECK(bufferFingerprint(t) == bufferFingerprint(reference));
}

TEST(testInteractiveHeightOnlyUnchanged)
{
    Ubiquitty::Terminal t, reference;
    Ubiquitty::hardReset(t);
    Ubiquitty::hardReset(reference);
    Ubiquitty::resize(t, 24, 100);
    Ubiquitty::resize(reference, 24, 100);
    feedVariedLines(t, 60);
    feedVariedLines(reference, 60);

    Ubiquitty::resize(t, 18, 100, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
    CHECK(t.pendingHistoryRewrapColumns == 0);
    Ubiquitty::resize(reference, 18, 100);
    CHECK(bufferFingerprint(t) == bufferFingerprint(reference));
}

TEST(testInteractiveRoundtripWidthSettles)
{
    Ubiquitty::Terminal t, reference;
    Ubiquitty::hardReset(t);
    Ubiquitty::hardReset(reference);
    Ubiquitty::resize(t, 24, 100);
    Ubiquitty::resize(reference, 24, 100);
    feedVariedLines(t, 70);
    feedVariedLines(reference, 70);

    Ubiquitty::resize(t, 24, 70, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
    Ubiquitty::resize(t, 24, 100, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
    CHECK(t.pendingHistoryRewrapColumns == 100);
    Ubiquitty::settleResize(&t);
    CHECK(t.pendingHistoryRewrapColumns == 0);

    Ubiquitty::resize(reference, 24, 70);
    Ubiquitty::resize(reference, 24, 100);
    CHECK(bufferFingerprint(t) == bufferFingerprint(reference));
}

TEST(testInteractiveSeamLineRewrapsWhole)
{
    Ubiquitty::Terminal t, reference;
    Ubiquitty::hardReset(t);
    Ubiquitty::hardReset(reference);
    Ubiquitty::resize(t, 24, 120);
    Ubiquitty::resize(reference, 24, 120);
    feedVariedLines(t, 30);
    feedVariedLines(reference, 30);
    // One giant unterminated line: ~104 full rows at width 120 — the top rows
    // scroll into history still soft-wrap-joined to the on-screen tail, and the
    // joined run is LONGER than the optional pull budget (4x rows), so only the
    // mandatory seam pull keeps the logical line whole.
    const std::string giant(static_cast<std::size_t>(104 * 120 + 7), 'Z');
    feed(t, giant.c_str());
    feed(reference, giant.c_str());

    Ubiquitty::resize(t, 24, 97, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
    CHECK(logicalLines(t) == logicalLines(reference)); // seam intact mid-deferral
    Ubiquitty::settleResize(&t);

    Ubiquitty::resize(reference, 24, 97);
    const std::string a = bufferFingerprint(t);
    const std::string b = bufferFingerprint(reference);
    CHECK(a == b);
}

// An interactive width-drag while the ALTERNATE screen shows: the deferred
// path reflows the primary it cannot see directly, flipping the grid to read
// it and flipping back — the arm every existing interactive case skips by
// dragging with the primary active. The settle then equals the settled-only
// oracle, exactly as the primary-active cases prove.
TEST(testInteractiveDragWhileAlternateShows)
{
    const int steps[] = { 100, 73, 91, 84 };
    Ubiquitty::Terminal interactive, settled;
    Ubiquitty::hardReset(interactive);
    Ubiquitty::hardReset(settled);
    Ubiquitty::resize(interactive, 24, 120);
    Ubiquitty::resize(settled, 24, 120);
    feedVariedLines(interactive, 40);
    feedVariedLines(settled, 40);
    feed(interactive, "\033[?1049h");
    feed(interactive, "alt-screen-content");
    feed(settled, "\033[?1049h");
    feed(settled, "alt-screen-content");

    for (int w : steps)
        Ubiquitty::resize(interactive, 24, w, Ubiquitty::Modes::ReflowMode::Reflow, Ubiquitty::Modes::AnchorMode::Top, Ubiquitty::Modes::ResizeUrgency::Interactive);
    Ubiquitty::settleResize(&interactive);
    for (int w : steps)
        Ubiquitty::resize(settled, 24, w);

    feed(interactive, "\033[?1049l");
    feed(settled, "\033[?1049l");
    CHECK(bufferFingerprint(interactive) == bufferFingerprint(settled));
}

// DECSTBM's saved region and DECSLRM's margins ride a reflow-resize: a
// non-default pair is clamped where it still fits and reset where it cannot,
// and an inverted survivor resets whole.
TEST(testMarginsRideTheReflowResize)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 100);
    feedVariedLines(t, 10);
    // DECLRMM on, then a non-default column pair and a non-default row pair;
    // DECSC saves the state a reflow must carry.
    feed(t, "\033[?69h\033[10;40s\033[5;20r\0337");

    // Wider: both pairs still fit and survive as set.
    Ubiquitty::resize(t, 24, 120);
    CHECK(t.screens.active->scrollLeft == 9);
    CHECK(t.screens.active->scrollRight == 39);

    // Narrow past the right member: the pair clamps, and once the clamp
    // inverts it, resets to full width.
    Ubiquitty::resize(t, 24, 30);
    CHECK(t.screens.active->scrollLeft >= 0);
    CHECK(t.screens.active->scrollRight <= 29);
    Ubiquitty::resize(t, 24, 8);
    CHECK(t.screens.active->scrollLeft == 0);
    CHECK(t.screens.active->scrollRight == 7);

    // The saved (DECSC) state took the same ride; restoring must land on the
    // clamped grid, not the old one.
    feed(t, "\0338");
    CHECK(t.screens.active->cursorX >= 0 && t.screens.active->cursorX < 8);
    CHECK(t.screens.active->cursorY >= 0 && t.screens.active->cursorY < 24);
}

// A cursor parked past the retained content: the reflow's cursor ordinal
// lands beyond what was fed, and the clamp brings it to the last written row
// rather than leaving it on a row the new grid does not have.
TEST(testCursorPastContentClampsThroughReflow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "one-line");
    feed(t, "\033[20;70H"); // park the cursor far below the content
    Ubiquitty::resize(t, 6, 30);
    CHECK(t.screens.active->cursorY >= 0 && t.screens.active->cursorY < 6);
    CHECK(t.screens.active->cursorX >= 0 && t.screens.active->cursorX < 30);
    // The shrink pushed the line above the viewport; it survives as history.
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) > 0);
}

UBIQUITTY_TEST_MAIN()
