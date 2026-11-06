// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/SizedBlock.h"

#include "Core/CellPen.h"
#include "Core/Diagnostics/DiagLog.h"
#include "Core/GridBounds.h"
#include "Core/LineStore.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Margins.h"
#include "Core/StrongIndex.h"
#include "Core/Terminal.h"
#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/Width.h"
#include "Core/VT/Internal.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace Ubiquitty::Screen {

void writeSizedBlock(Ubiquitty::CellGrid& grid, int row, int column, int columns, int rows,
    std::uint32_t codePoint, const Ubiquitty::Pen& pen, unsigned specID, const Ubiquitty::Cell& blank, std::uint16_t linkID)
{
    if (row < 0 || column < 0 || columns < 2 || rows < 1)
        return;
    if (row + rows > grid.rows() || column + columns > grid.columns())
        return;
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> runStorage;
    Ubiquitty::Cell* runCells = runStorage.data();
    for (int band = 0; band < rows; ++band) {
        Ubiquitty::Cell anchor {};
        anchor.codePoint = codePoint;
        Ubiquitty::Pen stamped = pen;
        stamped.attributes.add(Ubiquitty::VT::Attribute::Wide);
        stamped.attributes.remove(Ubiquitty::VT::Attribute::WideContinuation);
        stamped.extendedAttributes.setTextSizeSpecID(specID);
        Ubiquitty::penSetTextBand(&stamped, static_cast<uint8_t>(band));
        Ubiquitty::cellSetPen(anchor, stamped);
        anchor.setLinkID(linkID);
        runCells[0] = anchor;

        Ubiquitty::Cell continuation {};
        Ubiquitty::Pen continuationPen = stamped;
        continuationPen.attributes.remove(Ubiquitty::VT::Attribute::Wide);
        continuationPen.attributes.add(Ubiquitty::VT::Attribute::WideContinuation);
        Ubiquitty::cellSetPen(continuation, continuationPen);
        continuation.setLinkID(linkID);
        for (int i = 1; i < columns; ++i)
            runCells[i] = continuation;

        grid.writeRunRow(Ubiquitty::Row(row + band), Ubiquitty::Column(column),
            Ubiquitty::RowView { runCells, columns }, blank);
    }
}

namespace {

/// One chain of `row`: its start, its length and the facts the band judgment
/// reads from its anchor.
struct Chain {
    int start = 0;
    int length = 0;
    unsigned specID = 0;
    uint8_t band = 0;
};

bool chainAt(const Ubiquitty::Cell* row, int columns, int from, Chain& chain)
{
    int i = from;
    while (i < columns && !Ubiquitty::cellPen(row[i]).attributes.contains(Ubiquitty::VT::Attribute::Wide))
        ++i;
    if (i >= columns)
        return false;
    chain.start = i;
    chain.length = 1;
    while (i + chain.length < columns
        && Ubiquitty::cellPen(row[i + chain.length]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
        ++chain.length;
    const Ubiquitty::Pen& anchorPen = Ubiquitty::cellPen(row[i]);
    chain.specID = anchorPen.extendedAttributes.textSizeSpecID();
    chain.band = Ubiquitty::penTextBand(&anchorPen);
    return true;
}

/// Whether `above` holds, at exactly `chain`'s columns, a chain one band up
/// the same block.
bool backs(const Ubiquitty::Cell* above, int columns, const Chain& chain)
{
    if (!above)
        return false;
    Chain aboveChain;
    // The backing chain must START at the same column: a walk from anywhere
    // left of it would find a different chain first, so the probe asks at the
    // exact column and checks the shape.
    if (!Ubiquitty::cellPen(above[chain.start]).attributes.contains(Ubiquitty::VT::Attribute::Wide))
        return false;
    if (!chainAt(above, columns, chain.start, aboveChain))
        return false;
    return aboveChain.start == chain.start && aboveChain.length == chain.length
        && aboveChain.specID == chain.specID && aboveChain.band == chain.band - 1;
}

} // namespace

void scrubUnbackedBands(const Ubiquitty::Cell* rowAbove, Ubiquitty::Cell* row, int columns,
    const Ubiquitty::Cell& blank, int& firstTouched, int& lastTouched)
{
    int from = 0;
    Chain chain;
    while (chainAt(row, columns, from, chain)) {
        from = chain.start + chain.length;
        if (chain.band == 0)
            continue;
        if (backs(rowAbove, columns, chain))
            continue;
        for (int i = chain.start; i < chain.start + chain.length; ++i) {
            row[i] = blank;
            if (i < firstTouched)
                firstTouched = i;
            if (i > lastTouched)
                lastTouched = i;
        }
    }
}

void scrubShortenedBlocks(Ubiquitty::Cell* row, const Ubiquitty::Cell* rowBelow, int columns,
    const Ubiquitty::TextSizeTable* table, const Ubiquitty::Cell& blank, int& firstTouched, int& lastTouched)
{
    if (!table)
        return;
    int from = 0;
    Chain chain;
    while (chainAt(row, columns, from, chain)) {
        from = chain.start + chain.length;
        if (chain.specID == 0)
            continue;
        const Ubiquitty::TextSizeSpec* spec = table->lookup(chain.specID);
        if (!spec)
            continue;
        const int lastBand = spec->scale - 1;
        bool isCutShort = chain.band > lastBand;
        if (!isCutShort && chain.band < lastBand) {
            Chain below;
            isCutShort = !rowBelow
                || !Ubiquitty::cellPen(rowBelow[chain.start]).attributes.contains(Ubiquitty::VT::Attribute::Wide)
                || !chainAt(rowBelow, columns, chain.start, below) || below.start != chain.start
                || below.length != chain.length || below.specID != chain.specID
                || below.band != chain.band + 1;
        }
        if (!isCutShort)
            continue;
        for (int i = chain.start; i < chain.start + chain.length; ++i) {
            row[i] = blank;
            if (i < firstTouched)
                firstTouched = i;
            if (i > lastTouched)
                lastTouched = i;
        }
    }
}

namespace {

void scrubScreenRange(Ubiquitty::Terminal& t, Ubiquitty::ScreenMode mode, int firstRow, int lastRow)
{
    if (!t.grid || t.textSizeTable.liveCount() == 0)
        return;
    const int rows = t.rows;
    const int columns = t.columns;
    if (firstRow < 0)
        firstRow = 0;
    if (lastRow >= rows)
        lastRow = rows - 1;
    if (lastRow < firstRow)
        return;
    Ubiquitty::Cell blank;
    // The erase pen belongs to the screen being judged, not to the one on
    // display: each screen carries its own background, and blanking the
    // alternate with the primary's pen paints its holes the wrong colour.
    Ubiquitty::Screen::eraseCell(&blank, mode == Ubiquitty::ScreenMode::Alternate ? &t.screens.alternate : &t.screens.primary);

    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> aboveStorage;
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> rowStorage;
    Ubiquitty::Cell* aboveBuffer = aboveStorage.data();
    Ubiquitty::Cell* rowBuffer = rowStorage.data();

    // Top-down: erase what lost its backing. The row just past the edited
    // span is always judged — its backing is edited content whether or not
    // the scrub blanked there — and beyond it the sweep continues only while
    // it blanks: a row the scrub leaves whole backs everything below it
    // exactly as before.
    int sweepBottom = lastRow;
    for (int row = firstRow; row < rows; ++row) {
        if (row > 0)
            t.grid->readRow(mode, Ubiquitty::Row(row - 1), aboveBuffer);
        t.grid->readRow(mode, Ubiquitty::Row(row), rowBuffer);
        int firstTouched = columns, lastTouched = -1;
        scrubUnbackedBands(row > 0 ? aboveBuffer : nullptr, rowBuffer, columns, blank, firstTouched, lastTouched);
        if (lastTouched >= firstTouched) {
            Ubiquitty::RowView span = { &rowBuffer[firstTouched], lastTouched - firstTouched + 1 };
            t.grid->setRowSpan(mode, Ubiquitty::Row(row), Ubiquitty::Column(firstTouched), span);
            if (row > sweepBottom)
                sweepBottom = row;
        } else if (row > lastRow) {
            break;
        }
    }

    // Bottom-up: erase what the rows below cut short, from as far as the
    // first sweep reached back up past the span while it still blanks.
    for (int row = sweepBottom; row >= 0; --row) {
        t.grid->readRow(mode, Ubiquitty::Row(row), rowBuffer);
        if (row + 1 < rows)
            t.grid->readRow(mode, Ubiquitty::Row(row + 1), aboveBuffer);
        int firstTouched = columns, lastTouched = -1;
        scrubShortenedBlocks(rowBuffer, row + 1 < rows ? aboveBuffer : nullptr, columns,
            &t.textSizeTable, blank, firstTouched, lastTouched);
        if (lastTouched >= firstTouched) {
            Ubiquitty::RowView span = { &rowBuffer[firstTouched], lastTouched - firstTouched + 1 };
            t.grid->setRowSpan(mode, Ubiquitty::Row(row), Ubiquitty::Column(firstTouched), span);
        } else if (row < firstRow) {
            break;
        }
    }
}

} // namespace

void scrubBlockDamage(Ubiquitty::Terminal& t, int firstRow, int lastRow)
{
    const Ubiquitty::ScreenMode mode = (t.screens.active == &t.screens.alternate)
        ? Ubiquitty::ScreenMode::Alternate
        : Ubiquitty::ScreenMode::Primary;
    scrubScreenRange(t, mode, firstRow, lastRow);
}

void scrubRearrangedScreens(Ubiquitty::Terminal& t)
{
    if (!t.grid || t.textSizeTable.liveCount() == 0)
        return;
    scrubScreenRange(t, Ubiquitty::ScreenMode::Primary, 0, t.rows - 1);
    scrubScreenRange(t, Ubiquitty::ScreenMode::Alternate, 0, t.rows - 1);
}

bool placeSizedGlyph(Ubiquitty::Terminal& t, std::uint32_t codePoint,
    const Ubiquitty::TextSizeSpec& spec, unsigned specID)
{
    if (!t.grid || specID == 0 || spec.scale < 1)
        return false;
    Ubiquitty::Screen::State* s = t.screens.active;
    const int glyphWidth = spec.widthCells != 0
        ? spec.widthCells
        : Ubiquitty::Unicode::Width::forCodePoint(codePoint, t.ambiguousWidth);
    const int columns = spec.scale * (glyphWidth > 0 ? glyphWidth : 1);
    const int rows = spec.scale;
    // The writable band: the horizontal margins when DECSLRM is on, the whole
    // row otherwise — the same band the wrapping and the rect scrolls use.
    const bool hasMargins = isRowNarrowedByMargins(t, s);
    const int left = hasMargins ? s->scrollLeft : 0;
    const int right = hasMargins ? s->scrollRight : t.columns - 1;
    if (columns > right - left + 1)
        return false;

    int column = s->cursorX;
    int row = s->cursorY;
    if (column < left)
        column = left;
    // The margin arrives mid-text: the block moves whole to the next row
    // rather than splitting across it.
    if (column + columns - 1 > right || s->wrapPending) {
        if (t.modes.autowrap) {
            column = left;
            ++row;
        } else
            column = right - columns + 1;
    }

    // The band the block must stay inside: the scroll region when the cursor
    // is in it, the screen otherwise. A block crossing that edge would be cut
    // by the next scroll of the region, and a cut block can only be erased.
    const bool isInRegion = s->cursorY >= s->scrollTop && s->cursorY <= s->scrollBottom;
    const int bottom = isInRegion ? s->scrollBottom : t.rows - 1;
    if (row + rows - 1 > bottom)
        return false;

    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    // Presentation belongs to this code point, not to the pen the run inherited.
    // The ordinary emit strips Attribute::Emoji from the screen pen and re-adds it
    // per glyph (`emitCharacter` and `emitWideCharacter` in VT.cpp), which is what
    // keeps a monochrome character in an SGR-76 forced run monochrome. A block
    // written straight from `s->pen` skips that step: it keeps whatever the last
    // run left and loses what this code point is, so a sized emoji reached
    // CellInstanceBuilder without the bit isColorOnOwnSheet routes on and drew from
    // the mono sheet — no colour, and no glyph at all where the mono face has none.
    Ubiquitty::Pen blockPen = s->pen;
    blockPen.attributes.remove(Ubiquitty::VT::Attribute::Emoji);
    if (codePoint >= 0x80 && Ubiquitty::Unicode::Width::isEmojiPresentation(codePoint))
        blockPen.attributes.add(Ubiquitty::VT::Attribute::Emoji);
    if (columns == 1) {
        Cell single {};
        single.codePoint = codePoint;
        blockPen.attributes.remove({ VT::Attribute::Wide, VT::Attribute::WideContinuation });
        blockPen.extendedAttributes.setTextSizeSpecID(specID);
        penSetTextBand(&blockPen, 0);
        cellSetPen(single, blockPen);
        single.setLinkID(t.currentLinkID);
        t.grid->clearWideOverlap(Row(row), Column(column), Column(column), blank);
        t.grid->setCell(Row(row), Column(column), single);
    } else
        writeSizedBlock(*t.grid, row, column, columns, rows, codePoint, blockPen, specID, blank, t.currentLinkID);
    scrubBlockDamage(t, row, row + rows - 1);
    s->cursorX = std::min(column + columns, right);
    s->cursorY = row;
    s->wrapPending = column + columns > right;
    // A block is a graphic the terminal presented, so REP repeats it — as
    // ordinary text, because the size travelled with the code that carried
    // the run and no sizing state outlives it. Recorded only on the placing
    // path: a refusal leaves the run to the plain emit, which records its own.
    VT::discardGraphicCancellation(t);
    t.parser.precedingItem.setGraphic(codePoint, codePoint, 0);
    return true;
}

bool placeSizedCluster(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints,
    const Ubiquitty::TextSizeSpec& spec, unsigned specID)
{
    if (codePoints.empty() || !placeSizedGlyph(t, codePoints.front(), spec, specID))
        return false;
    auto& screen = *t.screens.active;
    const int columns = spec.scale * (spec.widthCells ? spec.widthCells : 1);
    const int column = screen.cursorX - columns + (screen.wrapPending ? 1 : 0);
    const int row = screen.cursorY;
    const auto stored = codePoints.first(std::min(codePoints.size(), static_cast<std::size_t>(clusterPoolMaximumCodePoints)));
    const auto clusterID = stored.size() > cellMaxCombining + 1 ? t.clusterPool.intern(stored) : 0;
    bool isEmoji = Unicode::Width::isEmojiPresentation(codePoints.front());
    const bool isPictographic = Unicode::isExtendedPictographic(codePoints.front());
    for (const auto codePoint : codePoints) {
        if (isPictographic && codePoint == Unicode::Width::variationSelector15)
            isEmoji = false;
        if ((isPictographic && codePoint == Unicode::Width::variationSelector16)
            || (Unicode::isKeycapBase(codePoints.front()) && codePoint == 0x20E3))
            isEmoji = true;
    }
    for (int band = 0; band < spec.scale; ++band) {
        auto anchor = *t.grid->cellAt(Row(row + band), Column(column));
        anchor.setClusterID(clusterID);
        if (clusterID == 0) {
            const auto count = std::min(stored.size() - 1, static_cast<std::size_t>(cellMaxCombining));
            std::copy_n(stored.begin() + 1, count, anchor.combining.begin());
            anchor.setCombiningLength(static_cast<std::uint8_t>(count));
        }
        if (isEmoji)
            anchor.pen.attributes.add(VT::Attribute::Emoji);
        else
            anchor.pen.attributes.remove(VT::Attribute::Emoji);
        t.grid->setCell(Row(row + band), Column(column), anchor);
    }
    auto& parser = t.parser;
    parser.clusterActive = true;
    parser.clusterRow = row;
    parser.clusterColumn = column;
    parser.clusterScreen = t.screens.isUsingAlternateScreen ? 1u : 0u;
    parser.clusterAtX = screen.cursorX;
    parser.clusterAtY = screen.cursorY;
    parser.clusterAtWrap = screen.wrapPending;
    parser.clusterCodepointLength = static_cast<std::uint8_t>(stored.size());
    std::copy(stored.begin(), stored.end(), parser.clusterCodePoints.begin());
    Unicode::graphemeStateReset(&parser.clusterState, codePoints.front());
    for (std::size_t at = 1; at < codePoints.size(); ++at)
        (void)Unicode::graphemeExtends(&parser.clusterState, codePoints[at - 1], codePoints[at]);
    parser.clusterLast = codePoints.back();
    return true;
}

namespace {

/// Interns run text with a bounded retry cadence when all retained text is live.
///
/// An erase can free text during the backoff, so a refusal is retried within
/// 64 placements even when the store has not crossed its next sweep threshold.
std::uint16_t internRunText(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints)
{
    const bool canSweep = t.runTextReclaimBackoffInterns == 0;
    if (!canSweep) {
        --t.runTextReclaimBackoffInterns;
    }
    const bool didSweep = canSweep && t.runTextStore.isSweepDue();
    if (didSweep) {
        sweepRunText(t);
    }
    if (const auto id = t.runTextStore.intern(codePoints)) {
        return id;
    }
    if (!canSweep) {
        return 0;
    }
    if (!didSweep) {
        sweepRunText(t);
        if (const auto id = t.runTextStore.intern(codePoints)) {
            return id;
        }
    }
    t.runTextReclaimBackoffInterns = 63;
    Ubiquitty::Diagnostics::diagLog(t, "run-text-store-exhausted",
        static_cast<std::uint32_t>(t.runTextStore.liveCodePoints()));
    return 0;
}

} // namespace

std::uint16_t appendRunText(Ubiquitty::Terminal& t, std::uint16_t textID, std::span<const std::uint32_t> suffix)
{
    const auto held = t.runTextStore.text(textID);
    if (held.empty() || suffix.size() > runTextMaximumCodePoints - held.size())
        return 0;
    std::vector<std::uint32_t> text(held.begin(), held.end());
    text.insert(text.end(), suffix.begin(), suffix.end());
    return internRunText(t, text);
}

bool placeSizedTextRun(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints,
    const Ubiquitty::TextSizeSpec& spec, unsigned specID)
{
    if (codePoints.empty() || !spec.isTextRun)
        return false;
    const auto textID = internRunText(t, codePoints);
    if (!textID || !placeSizedGlyph(t, codePoints.front(), spec, specID))
        return false;
    const auto& screen = *t.screens.active;
    const int column = screen.cursorX - spec.scale * spec.widthCells + (screen.wrapPending ? 1 : 0);
    for (int band = 0; band < spec.scale; ++band) {
        auto anchor = *t.grid->cellAt(Row(screen.cursorY + band), Column(column));
        anchor.setClusterID(textID);
        anchor.setCombiningLength(0);
        t.grid->setCell(Row(screen.cursorY + band), Column(column), anchor);
    }
    auto& parser = t.parser;
    parser.clusterActive = true;
    parser.clusterRow = screen.cursorY;
    parser.clusterColumn = column;
    parser.clusterScreen = t.screens.isUsingAlternateScreen ? 1u : 0u;
    parser.clusterAtX = screen.cursorX;
    parser.clusterAtY = screen.cursorY;
    parser.clusterAtWrap = screen.wrapPending;
    std::size_t last = 0;
    Unicode::graphemeStateReset(&parser.clusterState, codePoints.front());
    for (std::size_t at = 1; at < codePoints.size(); ++at) {
        if (!Unicode::graphemeExtends(&parser.clusterState, codePoints[at - 1], codePoints[at])) {
            last = at;
            Unicode::graphemeStateReset(&parser.clusterState, codePoints[at]);
        }
    }
    const auto cluster = codePoints.subspan(last).first(std::min(codePoints.size() - last, static_cast<std::size_t>(clusterPoolMaximumCodePoints)));
    parser.clusterCodepointLength = static_cast<std::uint8_t>(cluster.size());
    std::copy(cluster.begin(), cluster.end(), parser.clusterCodePoints.begin());
    parser.clusterLast = codePoints.back();
    return true;
}

namespace {

/// The last column a claim on `row` may reach: the writable band's right
/// edge, or the last visible column of a row DECDWL or DECDHL doubles.
int claimRightOnRow(const Ubiquitty::Terminal& t, const Ubiquitty::Screen::State* s, int row, int right)
{
    if (row < 0 || row >= t.rows)
        return right;
    const int visible = t.columns / Ubiquitty::textScaleForLineAttribute(s->lineAttribute[row]).widthScale - 1;
    return std::min(right, visible);
}

/// Scrolls the region a line under a claim that wraps past its last row,
/// leaving the claim's row the region's fresh last row.
void scrollForClaim(Ubiquitty::Terminal& t, const ProportionalRunPlacement& placement)
{
    if (!placement.shouldScrollFirst)
        return;
    t.screens.active->wrapPending = false;
    VT::linefeed(t);
}

/// Stands the cursor after a claim, as a sized block leaves it: past the
/// claim, or on its last cell with the wrap pending where the claim ends at
/// the row's right edge, so the text after it starts on the next row.
void standAfterClaim(Ubiquitty::Terminal& t, int row, int column, int columns)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const bool hasMargins = isRowNarrowedByMargins(t, s);
    const int right = claimRightOnRow(t, s, row, hasMargins ? s->scrollRight : t.columns - 1);
    s->cursorY = row;
    s->cursorX = std::min(column + columns, right);
    s->wrapPending = column + columns > right;
}

/// One grapheme of a run's text as the grid sets it.
struct GridGrapheme {
    /// Where it begins and ends in the text.
    std::size_t first = 0;
    std::size_t end = 0;
    /// The cells the grid's print path gives it.
    int cells = 1;
    /// Whether it draws in colour, as an emoji presentation or a selector
    /// makes it.
    bool isEmoji = false;
};

/// The grapheme of `codePoints` from `first`, at the width the grid's print
/// path gives it: its base's width, two where VS16 or a keycap makes an
/// emoji of it, and one for a grapheme that opens on a mark.
GridGrapheme gridGraphemeAt(std::span<const std::uint32_t> codePoints, std::size_t first, Unicode::Width::AmbiguousWidth ambiguous)
{
    GridGrapheme grapheme { .first = first, .end = first + 1 };
    Unicode::GraphemeState state {};
    Unicode::graphemeStateReset(&state, codePoints[first]);
    while (grapheme.end < codePoints.size() && Unicode::graphemeExtends(&state, codePoints[grapheme.end - 1], codePoints[grapheme.end]))
        ++grapheme.end;
    const std::uint32_t base = codePoints[first];
    int cells = Unicode::Width::forCodePoint(base, ambiguous);
    bool isEmoji = base >= 0x80 && Unicode::Width::isEmojiPresentation(base);
    const bool isPictographic = Unicode::isExtendedPictographic(base);
    for (std::size_t index = first + 1; index < grapheme.end; ++index) {
        if (isPictographic && codePoints[index] == Unicode::Width::variationSelector15)
            isEmoji = false;
        if ((isPictographic && codePoints[index] == Unicode::Width::variationSelector16) || (Unicode::isKeycapBase(base) && codePoints[index] == 0x20E3)) {
            isEmoji = true;
            cells = 2;
        }
    }
    grapheme.cells = std::clamp(cells, 1, 2);
    grapheme.isEmoji = isEmoji;
    return grapheme;
}

} // namespace

std::optional<ProportionalRunPlacement> fitProportionalRun(const Ubiquitty::Terminal& t, const Ubiquitty::TextSizeSpec& spec)
{
    if (!t.grid || !spec.isProportional)
        return std::nullopt;
    const Ubiquitty::Screen::State* s = t.screens.active;
    // A scaled run is a block: `scale` rows, each of its cells `scale` columns.
    const int rows = std::max<int>(spec.scale, 1);
    const int columns = spec.runWidthCells * rows;
    const bool hasMargins = isRowNarrowedByMargins(t, s);
    const int left = hasMargins ? s->scrollLeft : 0;
    const int marginRight = hasMargins ? s->scrollRight : t.columns - 1;
    const int right = claimRightOnRow(t, s, s->cursorY, marginRight);
    if (columns < 1 || columns > right - left + 1)
        return std::nullopt;

    ProportionalRunPlacement placement { .row = s->cursorY, .column = std::max(s->cursorX, left) };
    // The margin arrives mid-claim, or the row's last letter is printed and
    // its wrap pending: the claim moves whole to the next row, or with
    // autowrap reset takes the row's last cells, as a sized block does.
    if (placement.column + columns - 1 > right || s->wrapPending) {
        if (t.modes.autowrap) {
            placement.column = left;
            ++placement.row;
        } else
            placement.column = right - columns + 1;
    }
    // Every band inside the band the cursor is in, for the reason a sized
    // block's are: a block the region's edge crosses is cut by its next scroll.
    const bool isInRegion = s->cursorY >= s->scrollTop && s->cursorY <= s->scrollBottom;
    const int bottom = isInRegion ? s->scrollBottom : t.rows - 1;
    if (placement.row + rows - 1 > bottom) {
        // One row wrapping past the region's last row scrolls the region a
        // line, as the text the claim stands for would; a block's bands
        // cannot be split across the scroll, and print as ordinary text.
        if (rows != 1 || !isInRegion || s->cursorY != s->scrollBottom || placement.row != bottom + 1)
            return std::nullopt;
        placement.row = bottom;
        placement.shouldScrollFirst = true;
    } else if (placement.row != s->cursorY && placement.column + columns - 1 > claimRightOnRow(t, s, placement.row, marginRight))
        return std::nullopt;
    return placement;
}

void setProportionalRunInGridFace(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints,
    const Ubiquitty::TextSizeSpec& spec, ProportionalRunPlacement placement)
{
    const int claim = spec.runWidthCells * std::max<int>(spec.scale, 1);
    if (!t.grid || codePoints.empty() || claim < 1 || claim > Ubiquitty::maxColumns)
        return;
    scrollForClaim(t, placement);
    if (placement.row < 0 || placement.row >= t.rows || placement.column < 0 || placement.column + claim > t.columns)
        return;
    Ubiquitty::Screen::State* s = t.screens.active;
    // The whole graphemes the claim holds, at the cells the grid gives them;
    // a mark rides its letter, so the cut never separates the two.
    int textCells = 0;
    std::size_t held = 0;
    while (held < codePoints.size()) {
        const GridGrapheme grapheme = gridGraphemeAt(codePoints, held, t.ambiguousWidth);
        if (textCells + grapheme.cells > claim)
            break;
        textCells += grapheme.cells;
        held = grapheme.end;
    }
    const int slack = claim - textCells;
    // Right (1) and centred (2) move the text by the slack; left and
    // justified start it at the claim's first cell.
    int column = 0;
    if (spec.horizontalAlign == 1)
        column = slack;
    else if (spec.horizontalAlign == 2)
        column = slack / 2;

    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> storage;
    for (int index = 0; index < claim; ++index)
        storage.constructAt(static_cast<std::size_t>(index), blank);
    Ubiquitty::Pen pen = s->pen;
    pen.attributes.remove({ VT::Attribute::Wide, VT::Attribute::WideContinuation, VT::Attribute::Emoji });
    pen.extendedAttributes.setTextSizeSpecID(0);
    Ubiquitty::penSetTextBand(&pen, 0);
    for (std::size_t at = 0; at < held;) {
        const GridGrapheme grapheme = gridGraphemeAt(codePoints, at, t.ambiguousWidth);
        const auto stored = codePoints.subspan(at, std::min(grapheme.end - at, static_cast<std::size_t>(clusterPoolMaximumCodePoints)));
        Ubiquitty::Cell lead {};
        lead.codePoint = stored.front();
        Ubiquitty::Pen leadPen = pen;
        if (grapheme.isEmoji)
            leadPen.attributes.add(VT::Attribute::Emoji);
        if (grapheme.cells == 2)
            leadPen.attributes.add(VT::Attribute::Wide);
        Ubiquitty::cellSetPen(lead, leadPen);
        lead.setLinkID(t.currentLinkID);
        const std::uint16_t clusterID = stored.size() > cellMaxCombining + 1 ? t.clusterPool.intern(stored) : 0;
        lead.setClusterID(clusterID);
        if (clusterID == 0) {
            const auto count = std::min(stored.size() - 1, static_cast<std::size_t>(cellMaxCombining));
            std::copy_n(stored.begin() + 1, count, lead.combining.begin());
            lead.setCombiningLength(static_cast<std::uint8_t>(count));
        }
        storage.constructAt(static_cast<std::size_t>(column), lead);
        if (grapheme.cells == 2) {
            Ubiquitty::Cell continuation {};
            continuation.codePoint = Ubiquitty::wideContinuationSentinelCodePoint;
            Ubiquitty::Pen continuationPen = leadPen;
            continuationPen.attributes.remove(VT::Attribute::Wide);
            continuationPen.attributes.add(VT::Attribute::WideContinuation);
            Ubiquitty::cellSetPen(continuation, continuationPen);
            continuation.setLinkID(t.currentLinkID);
            storage.constructAt(static_cast<std::size_t>(column + 1), continuation);
        }
        column += grapheme.cells;
        at = grapheme.end;
    }
    const Ubiquitty::Row row(placement.row);
    t.grid->clearWideOverlap(row, Ubiquitty::Column(placement.column), Ubiquitty::Column(placement.column + claim - 1), blank);
    t.grid->setRowSpan(row, Ubiquitty::Column(placement.column), Ubiquitty::RowView { storage.data(), claim });
    scrubBlockDamage(t, placement.row, placement.row);
    // Nothing printed later joins a cluster this claim does not hold.
    VT::discardGraphicCancellation(t);
    t.parser.clusterActive = false;
    t.parser.precedingItem.setGraphic(codePoints[0], codePoints[0], 0);
    standAfterClaim(t, placement.row, placement.column, claim);
}

bool placeProportionalRun(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints,
    const Ubiquitty::TextSizeSpec& spec, unsigned specID)
{
    if (!t.grid || specID == 0 || !spec.isProportional || codePoints.empty())
        return false;
    const std::optional<ProportionalRunPlacement> placement = fitProportionalRun(t, spec);
    if (!placement)
        return false;
    Ubiquitty::Screen::State* s = t.screens.active;
    const int rows = std::max<int>(spec.scale, 1);
    // A single column carries no chain to anchor, and the block writer refuses
    // a span it cannot give a continuation; a one-cell run is written as a
    // stamped plain cell instead.
    const int columns = spec.runWidthCells * rows;

    const std::uint16_t textID = internRunText(t, codePoints);
    if (textID == 0)
        return false;
    scrollForClaim(t, *placement);
    const int row = placement->row;
    const int column = placement->column;

    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    Ubiquitty::Pen runPen = s->pen;
    runPen.attributes.remove(Ubiquitty::VT::Attribute::Emoji);
    if (columns >= 2) {
        writeSizedBlock(*t.grid, row, column, columns, rows, codePoints[0], runPen, specID, blank, t.currentLinkID);
    } else {
        Ubiquitty::Cell single = blank;
        single.codePoint = codePoints[0];
        Ubiquitty::Pen singlePen = runPen;
        singlePen.extendedAttributes.setTextSizeSpecID(specID);
        Ubiquitty::penSetTextBand(&singlePen, 0);
        Ubiquitty::cellSetPen(single, singlePen);
        single.setLinkID(t.currentLinkID);
        t.grid->setCell(Ubiquitty::Row(row), Ubiquitty::Column(column), single);
    }
    // Every band's anchor names the entry, as every band of a sized block
    // carries its glyph, so a row draws its slice without walking to the top;
    // the proportional spec is what tells a reader the id is not a
    // cluster-pool id.
    for (int band = 0; band < rows; ++band) {
        Ubiquitty::Cell anchor = *t.grid->cellAt(Ubiquitty::Row(row + band), Ubiquitty::Column(column));
        anchor.setCombiningLength(0);
        anchor.setClusterID(textID);
        t.grid->setCell(Ubiquitty::Row(row + band), Ubiquitty::Column(column), anchor);
    }
    scrubBlockDamage(t, row, row + rows - 1);
    // The rows now hold a run set at its own advances, and every reader
    // that maps pixels to text asks the flag before asking the grid pitch.
    for (int band = 0; band < rows; ++band)
        t.grid->rowMeta(row + band).set(Ubiquitty::RowFlag::Flowed, true);
    standAfterClaim(t, row, column, columns);
    // A mark printed after the run joins no cluster from before it.
    VT::discardGraphicCancellation(t);
    t.parser.clusterActive = false;
    t.parser.precedingItem.setGraphic(codePoints[0], codePoints[0], 0);
    return true;
}

void sweepRunText(Ubiquitty::Terminal& t)
{
    std::vector<std::uint64_t> referenced(Ubiquitty::RunTextStore::referenceWords, 0);
    // Every sized cell that is not a continuation is marked, not only the
    // anchors whose spec still resolves as proportional. The spec table can
    // free an id scrollback cells still carry, and a later spec taking that id
    // would make such a cell read as an anchor again; keeping its entry costs
    // memory and can never hand the cell another run's text.
    const auto markCells = [&referenced](const Ubiquitty::Cell* cells, int count) {
        for (int i = 0; i < count; ++i) {
            const Ubiquitty::Cell& cell = cells[i];
            const std::uint16_t id = cell.clusterID();
            const Ubiquitty::Pen& pen = Ubiquitty::cellPen(cell);
            if (id == 0 || pen.extendedAttributes.textSizeSpecID() == 0 || pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                continue;
            referenced[id >> 6] |= std::uint64_t { 1 } << (id & 63);
        }
    };
    if (t.grid) {
        for (int row = 0; row < t.grid->rows(); ++row) {
            const Ubiquitty::RowView primary = t.grid->rowCells(Ubiquitty::ScreenMode::Primary, Ubiquitty::Row(row));
            markCells(primary.data, primary.count);
            const Ubiquitty::RowView alternate = t.grid->rowCells(Ubiquitty::ScreenMode::Alternate, Ubiquitty::Row(row));
            markCells(alternate.data, alternate.count);
        }
    }
    if (t.lineStore) {
        for (int line = 0; line < t.lineStore->count(); ++line) {
            if (const Ubiquitty::StoredLine* stored = t.lineStore->at(line))
                markCells(stored->cells, stored->length);
        }
    }
    t.runTextStore.retainOnly(referenced);
}

void latchFlowedRow(Ubiquitty::Terminal& t, int screen, int row)
{
    if (!t.grid || row < 0 || row >= t.rows)
        return;
    const Ubiquitty::RowView cells = t.grid->rowCells(screen == 0 ? Ubiquitty::ScreenMode::Primary : Ubiquitty::ScreenMode::Alternate, Ubiquitty::Row(row));
    if (!cells.data)
        return;
    for (int column = 0; column < cells.count; ++column) {
        const Ubiquitty::Pen& pen = Ubiquitty::cellPen(cells.data[column]);
        const unsigned specID = pen.extendedAttributes.textSizeSpecID();
        if (specID == 0)
            continue;
        const Ubiquitty::TextSizeSpec* spec = t.textSizeTable.lookup(specID);
        if (spec && spec->isProportional) {
            t.grid->rowMetaAt(screen, row).set(Ubiquitty::RowFlag::Flowed, true);
            return;
        }
    }
}

bool flattenSizedRun(Ubiquitty::Cell* cells, int count, const Ubiquitty::Cell& blank,
    int* firstTouched, int* lastTouched, std::uint16_t specIDMask, const Ubiquitty::TextSizeTable& table)
{
    int first = count;
    int last = -1;
    for (int column = 0; column < count; ++column) {
        const Ubiquitty::Pen& pen = Ubiquitty::cellPen(cells[column]);
        const unsigned specID = pen.extendedAttributes.textSizeSpecID();
        if (specID == 0 || (specIDMask & (1u << (specID - 1))) == 0)
            continue;
        const bool isChain = pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)
            || pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation);
        const bool isAnchorRow = pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)
            && Ubiquitty::penTextBand(&pen) == 0;
        if (isAnchorRow || !isChain) {
            // The anchor keeps the glyph the block drew, at one cell: the run as
            // this terminal would have printed it.
            //
            // A cell carrying an id and no chain keeps its glyph for a different
            // reason: it belongs to no block, so there is no block to reduce,
            // and erasing it would turn a soft degradation into data loss.
            // scrubShortenedBlocks states that rule for its own case — an id
            // resolving to nothing renders unsized rather than erasing what
            // carries it. writeSizedBlock never emits the shape, so this is the
            // answer to a cell that arrived some other way rather than a path
            // the terminal takes.
            Ubiquitty::Pen plain = pen;
            plain.attributes.remove(Ubiquitty::VT::Attribute::Wide);
            plain.attributes.remove(Ubiquitty::VT::Attribute::WideContinuation);
            plain.extendedAttributes.setTextSizeSpecID(0);
            Ubiquitty::penSetTextBand(&plain, 0);
            Ubiquitty::cellSetPen(cells[column], plain);
            // The spec distinguishes RunTextStore IDs from ClusterPool IDs.
            const auto* spec = table.lookup(specID);
            if (spec && (spec->isProportional || spec->isTextRun)) {
                cells[column].setClusterID(0);
            }
        } else {
            // The rest of a block: continuations, and the anchors of every band
            // below the first. The band-zero anchor already keeps the one glyph.
            cells[column] = blank;
        }
        if (column < first)
            first = column;
        if (column > last)
            last = column;
    }
    if (firstTouched)
        *firstTouched = first;
    if (lastTouched)
        *lastTouched = last;
    return last >= first;
}

namespace {

void flattenSizedGrid(Ubiquitty::Terminal& t, std::uint16_t specIDMask)
{
    if (!t.grid || !t.grid->hasSizedCellHint()) {
        return;
    }
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, t.screens.active);
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> storage;
    for (const auto mode : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        for (int row = 0; row < t.rows; ++row) {
            const auto cells = t.grid->rowCells(mode, Ubiquitty::Row(row));
            assert(cells.count >= 0 && cells.count <= Ubiquitty::maxColumns);
            bool needsFlatten = false;
            for (int column = 0; column < cells.count; ++column) {
                const unsigned specID = Ubiquitty::cellPen(cells.data[column]).extendedAttributes.textSizeSpecID();
                if (specID && (specIDMask & (1u << (specID - 1)))) {
                    needsFlatten = true;
                    break;
                }
            }
            if (!needsFlatten) {
                continue;
            }
            std::copy_n(cells.data, cells.count, storage.data());
            int firstTouched = 0;
            int lastTouched = 0;
            if (flattenSizedRun(storage.data(), cells.count, blank,
                    &firstTouched, &lastTouched, specIDMask, t.textSizeTable)) {
                t.grid->setRowSpan(mode, Ubiquitty::Row(row), Ubiquitty::Column(firstTouched),
                    { storage.data() + firstTouched, lastTouched - firstTouched + 1 });
            }
        }
    }
}

} // namespace

std::uint16_t undrawnTextSizeSpecIDs(const Ubiquitty::Terminal& t, const Ubiquitty::TextSizeTable& table)
{
    if (t.drawsSizedText && t.drawsProportionalText)
        return 0;
    if (!t.drawsSizedText && !t.drawsProportionalText)
        return Ubiquitty::allTextSizeSpecIDs;
    std::uint16_t undrawn = 0;
    for (unsigned specID = 1; specID <= static_cast<unsigned>(Ubiquitty::textSizeTableCapacity); ++specID) {
        const Ubiquitty::TextSizeSpec* spec = table.lookup(specID);
        // Missing specifications cannot be rendered as sized text. A scaled
        // run is drawn in a sized block's bands, so it needs both facts.
        const bool isDrawn = spec
            && (spec->isProportional ? t.drawsProportionalText && (spec->scale <= 1 || t.drawsSizedText) : t.drawsSizedText);
        if (!isDrawn)
            undrawn |= static_cast<std::uint16_t>(1u << (specID - 1));
    }
    return undrawn;
}

void flattenSizedGridCells(Ubiquitty::Terminal& t)
{
    flattenSizedGrid(t, undrawnTextSizeSpecIDs(t, t.textSizeTable));
}

void flattenSizedCells(Ubiquitty::Terminal& t)
{
    flattenSizedGrid(t, Ubiquitty::allTextSizeSpecIDs);
    t.textSizeTable.retainOnly(0);
    if (t.grid) {
        t.grid->clearSizedCellHintAfterWholeGridScan();
    }
}

namespace {

/// Refused interns a barren census defers the next census by: enough that a
/// run of distinct specs over a table whose every id is still named shares one
/// walk, few enough that ids an erase has since released come back within a
/// screenful of runs.
constexpr int barrenCensusBackoffInterns = 64;

/// The ids `count` cells name, bit `id - 1` per id.
std::uint16_t specIDsNamedBy(const Ubiquitty::Cell* cells, int count)
{
    std::uint16_t named = 0;
    for (int column = 0; column < count; ++column) {
        const unsigned specID = Ubiquitty::cellPen(cells[column]).extendedAttributes.textSizeSpecID();
        if (specID != 0)
            named |= static_cast<std::uint16_t>(1u << (specID - 1));
    }
    return named;
}

} // namespace

void reclaimTextSizeSpecs(Ubiquitty::Terminal& t)
{
    // A barren verdict describes the cells an earlier census read, not these.
    t.textSizeReclaimBackoffInterns = 0;
    const std::uint16_t live = t.textSizeTable.liveMask();
    if (!t.grid || live == 0)
        return;
    // The population is every cell that can hand an id back to a lookup: both
    // screens whole, because the inactive one returns; the DECSSDT status row,
    // which a one-cell run written under DECSASD stamps and the renderer draws
    // through this table; and every retained scrollback line, because the view
    // scrolls back to it. The walk stops once every live id has been seen,
    // since nothing past that point can free one.
    std::uint16_t named = 0;
    for (int row = 0; row < t.grid->rows() && (named & live) != live; ++row) {
        const Ubiquitty::RowView primary = t.grid->rowCells(Ubiquitty::ScreenMode::Primary, Ubiquitty::Row(row));
        const Ubiquitty::RowView alternate = t.grid->rowCells(Ubiquitty::ScreenMode::Alternate, Ubiquitty::Row(row));
        if (primary.data)
            named |= specIDsNamedBy(primary.data, primary.count);
        if (alternate.data)
            named |= specIDsNamedBy(alternate.data, alternate.count);
    }
    const std::span<const Ubiquitty::Cell> status = Ubiquitty::Rendering::statusRow(t);
    if ((named & live) != live && !status.empty())
        named |= specIDsNamedBy(status.data(), static_cast<int>(status.size()));
    if (t.lineStore) {
        for (int line = 0; line < t.lineStore->count() && (named & live) != live; ++line) {
            const Ubiquitty::StoredLine* stored = t.lineStore->at(line);
            if (stored && stored->cells)
                named |= specIDsNamedBy(stored->cells, stored->length);
        }
    }
    t.textSizeTable.retainOnly(named);
}

unsigned internTextSizeSpec(Ubiquitty::Terminal& t, const Ubiquitty::TextSizeSpec& spec)
{
    if (const unsigned specID = t.textSizeTable.intern(spec); specID != 0)
        return specID;
    if (t.textSizeReclaimBackoffInterns > 0) {
        --t.textSizeReclaimBackoffInterns;
        return 0;
    }
    const int liveBefore = t.textSizeTable.liveCount();
    reclaimTextSizeSpecs(t);
    const int freed = liveBefore - t.textSizeTable.liveCount();
    if (freed == 0) {
        t.textSizeReclaimBackoffInterns = barrenCensusBackoffInterns;
        return 0;
    }
    Ubiquitty::Diagnostics::diagLog(t, "text-size-specs-reclaimed", static_cast<std::uint32_t>(freed));
    return t.textSizeTable.intern(spec);
}

void adoptTextSizeTable(Ubiquitty::Terminal& t, const Ubiquitty::TextSizeTable& incoming, TextSizeTableFrame frame)
{
    std::uint16_t renamed = 0;
    for (unsigned specID = 1; specID <= static_cast<unsigned>(Ubiquitty::textSizeTableCapacity); ++specID) {
        const Ubiquitty::TextSizeSpec* next = incoming.lookup(specID);
        const Ubiquitty::TextSizeSpec* held = t.textSizeTable.lookup(specID);
        if ((next && (!held || *held != *next)) || (held && !next))
            renamed |= static_cast<std::uint16_t>(1u << (specID - 1));
    }
    if (renamed == 0) {
        t.textSizeTable = incoming;
        return;
    }
    if (frame == TextSizeTableFrame::Scrollback) {
        flattenSizedGrid(t, renamed);
    }
    if (frame != TextSizeTableFrame::Snapshot && !Ubiquitty::Rendering::statusRow(t).empty()) {
        const Ubiquitty::Rendering::StatusRowCells status = Ubiquitty::Rendering::ensureStatusRow(t);
        flattenSizedRun(status.data(), static_cast<int>(status.size()), Ubiquitty::defaultBlankCell, nullptr, nullptr, renamed, t.textSizeTable);
    }
    if (t.lineStore) {
        for (int line = 0; line < t.lineStore->count(); ++line) {
            const Ubiquitty::StoredLine* stored = t.lineStore->at(line);
            if (!stored || !stored->cells || stored->length <= 0 || (specIDsNamedBy(stored->cells, stored->length) & renamed) == 0)
                continue;
            // The write must land in the store, not in the decode of a folded
            // block that the cache may drop; only a line naming a renamed spec
            // pays the unfold.
            Ubiquitty::StoredLine* writable = t.lineStore->writableAt(line);
            flattenSizedRun(writable->cells, writable->length, Ubiquitty::defaultBlankCell, nullptr, nullptr, renamed, t.textSizeTable);
        }
    }
    t.textSizeTable = incoming;
}

void adoptRunTextStore(Ubiquitty::Terminal& t, Ubiquitty::RunTextStore&& incoming)
{
    const auto held = t.runTextStore.slots();
    if (held.empty()) {
        t.runTextStore = std::move(incoming);
        return;
    }
    std::array<bool, Ubiquitty::RunTextStore::maxEntries + 1> renamed {};
    bool hasRenamedRun = false;
    for (std::size_t slot = 0; slot < held.size(); ++slot) {
        renamed[slot + 1] = !held[slot].empty()
            && !std::ranges::equal(held[slot], incoming.text(static_cast<std::uint16_t>(slot + 1)));
        hasRenamedRun |= renamed[slot + 1];
    }
    const auto namesRenamedRun = [&](const Ubiquitty::Cell& cell) {
        const auto* spec = t.textSizeTable.lookup(Ubiquitty::cellPen(cell).extendedAttributes.textSizeSpecID());
        return spec && (spec->isProportional || spec->isTextRun) && renamed[cell.clusterID()];
    };
    if (hasRenamedRun && t.lineStore) {
        for (int line = 0; line < t.lineStore->count(); ++line) {
            const Ubiquitty::StoredLine* stored = t.lineStore->at(line);
            if (!stored || !stored->cells) {
                continue;
            }
            // `at` and `writableAt` hand back the same descriptor, so once a
            // run needs rewriting the block is unfolded and `stored->cells`
            // reads the store's own slab from then on.
            bool isWritable = false;
            for (int column = 0; column < stored->length; ++column) {
                if (!namesRenamedRun(stored->cells[column])) {
                    continue;
                }
                if (!isWritable) {
                    stored = t.lineStore->writableAt(line);
                    isWritable = true;
                }
                int end = column + 1;
                while (end < stored->length
                    && Ubiquitty::cellPen(stored->cells[end]).attributes.contains(Ubiquitty::VT::Attribute::WideContinuation)) {
                    ++end;
                }
                flattenSizedRun(stored->cells + column, end - column, Ubiquitty::defaultBlankCell,
                    nullptr, nullptr, Ubiquitty::allTextSizeSpecIDs, t.textSizeTable);
                column = end - 1;
            }
        }
    }
    t.runTextStore = std::move(incoming);
}

} // namespace Ubiquitty::Screen
