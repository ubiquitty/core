// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_retained_frame_coverage.cpp — the retained plan against a full re-read.
//
// A frame that Loads the retained target repaints only the rows in the dirty
// set, so every other row must render exactly what it rendered last frame:
// the same cells, the same line attribute, under the same palette and the same
// screen-wide reverse video. That is the whole contract of the grid cache, and
// it is checkable without a GPU: re-read every row in full each frame, and
// require that a row the plan left clean is the row the last frame showed —
// shifted by the scroll the blit performs, when there is one.
//
// The feed walks the events that change pixels without necessarily changing a
// cell: palette writes, resets, line attributes, scroll regions, the alternate
// screen, resizes in both directions. A row missed by the dirty set is named
// with the step that missed it.

#include "Harness.h"

#include "Core/Cell.h"
#include "Core/CellGrid.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Rendering/FrameDamage.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Themes/RethemeDefaults.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

/// Everything a row's pixels are drawn from, read in full.
struct FrameReading {
    int rows = 0;
    int columns = 0;
    std::vector<Cell> cells;
    std::vector<std::uint8_t> lineAttributes;
    TermPalette palette {};
    bool isReverseVideo = false;
};

FrameReading readInFull(const Terminal& terminal, int viewOffset)
{
    FrameReading reading;
    reading.rows = terminal.rows;
    reading.columns = terminal.columns;
    reading.cells.assign(static_cast<std::size_t>(terminal.rows) * static_cast<std::size_t>(terminal.columns), Cell {});
    reading.lineAttributes.assign(static_cast<std::size_t>(terminal.rows), 0);
    std::vector<std::uint8_t> marks(static_cast<std::size_t>(terminal.rows), 0);
    CapturedRowRefresh full;
    full.viewOffset = viewOffset;
    const int history = Screen::scrollbackCount(&terminal);
    full.firstAbsoluteRow = history - std::clamp(viewOffset, 0, history);
    refreshCapturedRows(terminal, full, reading.cells, reading.lineAttributes, marks);
    reading.palette = terminal.palette;
    reading.isReverseVideo = isReverseVideo(&terminal);
    return reading;
}

// The cursor slot is the cursor tier's, drawn every frame over the grid, so
// a cursor colour is no colour a retained row was drawn with.
bool isSameCellPalette(TermPalette now, const TermPalette& before)
{
    now.cursor = before.cursor;
    return now == before;
}

bool isSameRow(const FrameReading& now, int row, const FrameReading& before, int sourceRow)
{
    const std::size_t columns = static_cast<std::size_t>(now.columns);
    return now.lineAttributes[static_cast<std::size_t>(row)] == before.lineAttributes[static_cast<std::size_t>(sourceRow)]
        && std::memcmp(now.cells.data() + static_cast<std::size_t>(row) * columns,
               before.cells.data() + static_cast<std::size_t>(sourceRow) * columns, columns * sizeof(Cell))
        == 0;
}

/// Whether the oracle's plans run with the grid cache on.
enum class GridCache : std::uint8_t {
    On,
    Off,
};

/// One frontend frame: the plan, then the oracle against the last frame.
class RetainedFrameOracle {
public:
    explicit RetainedFrameOracle(Terminal& terminal, GridCache cache = GridCache::On)
        : m_terminal(terminal)
        , m_isGridCacheEnabled(cache == GridCache::On)
    {
    }

    /// Plans one frame and reports every clean row that is not the row the
    /// retained target holds.
    ///
    /// - Returns: How many rows the plan left stale.
    int frame(const char* step, int viewOffset = 0)
    {
        Terminal& t = m_terminal;
        std::vector<std::uint8_t> rowDirty(static_cast<std::size_t>(t.rows), 0);
        RetainedFrameRequest request;
        request.rows = t.rows;
        request.columns = t.columns;
        request.viewOffset = viewOffset;
        // The target always survives: the frontend's own refusals are not
        // under test, so every Load is Core's decision alone.
        request.isTargetMatching = true;
        request.cellHeightInPixels = 16;
        request.textureHeightInPixels = 16 * t.rows;
        request.isGridCacheEnabled = m_isGridCacheEnabled;
        const RetainedFramePlan plan = planRetainedFrame(t, m_baseline, request, rowDirty);
        FrameReading now = readInFull(t, viewOffset);
        ++m_frames;
        int staleRows = 0;
        if (plan.shouldLoadRetainedTarget) {
            ++m_loadedFrames;
            const auto report = [&](const char* what, int row) {
                std::fprintf(stderr, "  step \"%s\": %s (row %d)\n", step, what, row);
                ++staleRows;
            };
            if (now.rows != m_last.rows || now.columns != m_last.columns) {
                report("the grid changed shape under a loaded target", -1);
            } else if (!isSameCellPalette(now.palette, m_last.palette)) {
                report("the palette changed under a loaded target", -1);
            } else if (now.isReverseVideo != m_last.isReverseVideo) {
                report("reverse video changed under a loaded target", -1);
            } else {
                for (int row = 0; row < now.rows; ++row) {
                    if (rowDirty[static_cast<std::size_t>(row)])
                        continue;
                    int sourceRow = row;
                    const int delta = plan.damage.scrollDelta;
                    if (delta != 0 && row >= plan.damage.scrollTop && row <= plan.damage.scrollBottom) {
                        sourceRow = row + delta;
                        if (sourceRow < plan.damage.scrollTop || sourceRow > plan.damage.scrollBottom) {
                            report("a row the scroll exposed is clean", row);
                            continue;
                        }
                    }
                    if (!isSameRow(now, row, m_last, sourceRow))
                        report("a clean row differs from the row the target holds", row);
                }
            }
        }
        m_last = std::move(now);
        return staleRows;
    }

    [[nodiscard]] int loadedFrames() const { return m_loadedFrames; }
    [[nodiscard]] int frames() const { return m_frames; }

private:
    Terminal& m_terminal;
    bool m_isGridCacheEnabled = true;
    RetainedFrameBaseline m_baseline;
    FrameReading m_last;
    int m_frames = 0;
    int m_loadedFrames = 0;
};

std::unique_ptr<Terminal> makeTerminal(int rows, int columns)
{
    auto t = std::make_unique<Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

void feed(Terminal& t, const std::string& bytes)
{
    Ubiquitty::processBytes(&t, bytes);
}

std::string screenOfText(int rows)
{
    std::string text = "\x1b[H";
    for (int row = 1; row <= rows; ++row)
        text += "\x1b[" + std::to_string(row) + ";1Hrow " + std::to_string(row) + " \x1b[31mred\x1b[0m \x1b[44mblue\x1b[0m text";
    return text;
}

struct Step {
    const char* name;
    std::string bytes;
    int viewOffset = 0;
    int resizeRows = 0;
    int resizeColumns = 0;
};

std::vector<Step> stressSteps()
{
    return {
        { "first frame", screenOfText(8) },
        { "settle", "" },
        { "edit one row", "\x1b[3;5Hedited" },
        { "cursor move only", "\x1b[7;9H" },
        { "scroll the whole screen", "\x1b[8;1H\r\nscrolled one\r\nscrolled two" },
        { "margined scroll up", "\x1b[3;6r\x1b[6;1H\r\nin region\r\nagain\x1b[r" },
        { "margined scroll down", "\x1b[2;5r\x1b[2;1H\x1bM\x1bMdown\x1b[r" },
        { "scroll up by SU", "\x1b[2S" },
        { "scroll down by SD", "\x1b[1T" },
        { "insert lines", "\x1b[4;1H\x1b[2L" },
        { "delete lines", "\x1b[3;1H\x1b[1M" },
        { "erase in line", "\x1b[2;3H\x1b[K" },
        { "erase characters", "\x1b[5;2H\x1b[4X" },
        { "insert and delete characters", "\x1b[6;2H\x1b[3@\x1b[7;2H\x1b[2P" },
        { "palette entry the cells use", "\x1b]4;1;rgb:00/ff/00\x07" },
        { "default foreground", "\x1b]10;rgb:ff/ff/00\x07" },
        { "default background", "\x1b]11;rgb:20/00/40\x07" },
        { "cursor colour", "\x1b]12;rgb:ff/00/ff\x07" },
        { "palette reset", "\x1b]104\x07\x1b]110\x07\x1b]111\x07\x1b]112\x07" },
        { "reverse video on", "\x1b[?5h" },
        { "reverse video off", "\x1b[?5l" },
        { "double width line", "\x1b[2;1H\x1b#6" },
        { "double height lines", "\x1b[4;1H\x1b#3\x1b[5;1H\x1b#4" },
        { "single width again", "\x1b[2;1H\x1b#5\x1b[4;1H\x1b#5\x1b[5;1H\x1b#5" },
        { "screen alignment", "\x1b#8" },
        { "refill", screenOfText(8) },
        { "alternate screen", "\x1b[?1049h\x1b[H\x1b[2Jalternate" },
        { "alternate edit", "\x1b[4;1Hmore alternate" },
        { "primary screen", "\x1b[?1049l" },
        { "wide and combining", "\x1b[6;1H\xe4\xb8\x96\xe7\x95\x8c e\xcc\x81 x" },
        { "hyperlink", "\x1b[7;1H\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\" },
        { "blink on", "\x1b[3;1H\x1b[5mblink\x1b[0m" },
        { "blink off", "\x1b[3;1H\x1b[2K" },
        { "synchronized update", "\x1b[?2026h\x1b[2;1Hsync body\x1b[?2026l" },
        { "erase display", "\x1b[2J" },
        { "refill again", screenOfText(8) },
        { "hard reset", "\x1b"
                        "c" },
        { "refill after reset", screenOfText(8) },
        { "background before reset", "\x1b]11;rgb:80/00/00\x07" },
        { "reset and a new background", "\x1b"
                                        "c"
                                        "\x1b]11;rgb:00/00/80\x07" },
        { "refill on the new background", screenOfText(8) },
        { "scrolled back", "\r\nmore\r\nlines\r\nfor\r\nhistory", 2 },
        { "scrolled back further", "", 3 },
        { "back to the tail", "", 0 },
        { "taller", "", 0, 11, 30 },
        { "wider", "\x1b[1;1Hwider row", 0, 11, 44 },
        { "shorter", "", 0, 6, 44 },
        { "narrower", "", 0, 6, 20 },
        { "after resizes", "\x1b[2;1Hsettled" },
        { "idle", "" },
    };
}

void runStress(Terminal& t, RetainedFrameOracle& oracle, int& staleRows)
{
    for (const Step& step : stressSteps()) {
        if (step.resizeRows > 0)
            Ubiquitty::resize(t, step.resizeRows, step.resizeColumns);
        if (!step.bytes.empty())
            feed(t, step.bytes);
        staleRows += oracle.frame(step.name, step.viewOffset);
    }
}

} // namespace

// Every frame of a feed that walks the pixel-changing events keeps a row only
// where the row is what the target holds. The count of loaded frames is the
// coverage bound: an oracle over a plan that never Loads checks nothing.
TEST(aLoadedTargetKeepsOnlyRowsThatDidNotChange)
{
    auto t = makeTerminal(8, 30);
    RetainedFrameOracle oracle(*t);
    int staleRows = 0;
    runStress(*t, oracle, staleRows);
    CHECK_MSG(staleRows == 0, "no clean row of a loaded frame differs from the row its target holds");
    CHECK_MSG(oracle.loadedFrames() * 2 > oracle.frames(), "and most frames of the feed Loaded the target");
}

// A theme applied by direct write, as a configuration reload applies one: the
// palette moves outside the OSC handlers.
TEST(aThemeAppliedOutsideTheEscapesRefusesTheTarget)
{
    auto t = makeTerminal(6, 24);
    RetainedFrameOracle oracle(*t);
    int staleRows = 0;
    feed(*t, screenOfText(6));
    staleRows += oracle.frame("first frame");
    staleRows += oracle.frame("settle");
    t->palette.defaultBackground = PaletteRGB { 0x1000, 0x2000, 0x3000 };
    Themes::rethemeDefaults(t.get());
    staleRows += oracle.frame("theme applied");
    CHECK_MSG(staleRows == 0, "the rethemed frame keeps no row drawn under the old colours");
}

// A hard reset rebuilds the terminal, and with it every counter the retained
// baseline is compared against. Reset to its starting value, the palette's
// epoch can return to the one the target was drawn at while the colours it
// stands for are different ones.
TEST(aHardResetCannotReturnThePaletteEpochToTheDrawnOne)
{
    auto t = makeTerminal(6, 24);
    RetainedFrameOracle oracle(*t);
    int staleRows = 0;
    feed(*t, "\x1b]11;rgb:80/00/00\x07");
    staleRows += oracle.frame("red background");
    staleRows += oracle.frame("settle");
    const std::uint64_t drawnEpoch = t->paletteEpoch;
    feed(*t, "\x1b"
             "c"
             "\x1b]11;rgb:00/00/80\x07");
    CHECK_MSG(t->paletteEpoch != drawnEpoch, "the epoch after a reset and a new colour is not the drawn one");
    staleRows += oracle.frame("reset to blue");
    CHECK_MSG(staleRows == 0, "and no blank row keeps the red it was drawn on");
}

// A hard reset blanks the screen. The rows it blanked must reach the next
// frame, whatever the palette did.
TEST(aHardResetOwesEveryRowItBlanked)
{
    auto t = makeTerminal(6, 24);
    RetainedFrameOracle oracle(*t);
    int staleRows = 0;
    feed(*t, screenOfText(6));
    staleRows += oracle.frame("text");
    staleRows += oracle.frame("settle");
    feed(*t, "\x1b"
             "c");
    staleRows += oracle.frame("hard reset");
    CHECK_MSG(staleRows == 0, "no row keeps the text the reset erased");
}

// The switch, end to end through the plan: off, no frame of the same feed
// Loads the target, so there is nothing for a stale row to hide in.
TEST(theCacheSwitchedOffLoadsNoFrameOfTheFeed)
{
    auto t = makeTerminal(8, 30);
    RetainedFrameOracle oracle(*t, GridCache::Off);
    int staleRows = 0;
    runStress(*t, oracle, staleRows);
    CHECK_MSG(oracle.loadedFrames() == 0, "the cache off Loads no frame");
    CHECK_MSG(staleRows == 0, "and so keeps no stale row");
}

UBIQUITTY_TEST_MAIN()
