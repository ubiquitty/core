// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The protocol-agnostic terminal image model and its lifecycle. Owns the image /
// placement slot bookkeeping and the Ubiquitty::Image public surface; helpers
// shared with the decoder backends are declared in TerminalImageInternal.h.

#include "Core/Image/TerminalImageInternal.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Operations.h"
#include "Core/Terminal.h"
#include "Core/TerminalShared.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

namespace Ubiquitty::Image {

// Model-only helpers (not shared across translation units).
static void dropPlacement(Ubiquitty::Terminal& t, int i);

void dispose(TerminalImage* image)
{
    int i;
    if (!image)
        return;
    // frames[0] aliases image->pixels — never double-free. Frames 1..N
    // own their own buffers.
    for (i = 1; i < maxTerminalImageFrameCount; i++) {
        if (image->frames[i].used && image->frames[i].pixels)
            free(image->frames[i].pixels);
    }
    if (image->pixels)
        free(image->pixels);
    std::memset(image, 0, sizeof(*image));
}

// After a fresh base pixel buffer is installed, set frames[0] to alias it so
// animation logic reads frame pixels uniformly regardless of frame count.
void initFrameZero(TerminalImage* image)
{
    if (!image)
        return;
    image->frames[0].used = true;
    image->frames[0].gapless = false;
    image->frames[0].pixels = image->pixels;
    image->frames[0].pixelLength = image->pixelLength;
    // 0 = unspecified gap → default (40ms) at playback, so the root frame is a
    // real, visible frame 1 of the loop rather than being instantly skipped.
    image->frames[0].delayMilliseconds = 0;
    image->frameCount = 1;
    image->currentFrame = 0;
    image->frameClockMilliseconds = 0;
    image->animationState = AnimationState::Stopped;
}

struct ContentKeeper::Payload {
    std::array<TerminalImage, maxTerminalImageCount> stored {};
    std::array<TerminalImagePlacement, maxTerminalImagePlacementCount> placements {};
    uint32_t nextImageID {};
    uint32_t livePlacementCount {};
    uint64_t useSeq {};
    uint64_t contentSeq {};
    bool restored {};
};

ContentKeeper::ContentKeeper(Ubiquitty::Terminal& t)
    : m_payload(std::make_unique<Payload>())
{
    // Steal by copy-and-blank: the structs own raw malloc'd pixel pointers
    // (frames[0] aliasing the base buffer), so copying the array copies
    // ownership and zeroing the source leaves the refeed's clears nothing to
    // dispose. No pixel byte moves.
    m_payload->stored = t.images.stored;
    m_payload->placements = t.images.placements;
    m_payload->nextImageID = t.images.kittyNextImageID;
    m_payload->livePlacementCount = t.images.livePlacementCount;
    m_payload->useSeq = t.images.kittyImageUseSeq;
    m_payload->contentSeq = t.images.kittyImageContentSeq;
    t.images.stored = {};
    t.images.placements = {};
    t.images.livePlacementCount = 0;
}

void ContentKeeper::restoreInto(Ubiquitty::Terminal& t)
{
    if (!m_payload || m_payload->restored)
        return;
    // Whatever the refeed itself minted loses to the kept content: the refeed
    // was text, so anything here is a stray the capture replayed.
    disposeAllImages(t);
    t.images.stored = m_payload->stored;
    t.images.placements = m_payload->placements;
    t.images.livePlacementCount = m_payload->livePlacementCount;
    if (t.images.kittyNextImageID < m_payload->nextImageID)
        t.images.kittyNextImageID = m_payload->nextImageID;
    // Monotonic sources only ever move forward, whichever side is ahead.
    if (t.images.kittyImageUseSeq < m_payload->useSeq)
        t.images.kittyImageUseSeq = m_payload->useSeq;
    if (t.images.kittyImageContentSeq < m_payload->contentSeq)
        t.images.kittyImageContentSeq = m_payload->contentSeq;
    m_payload->restored = true;
}

ContentKeeper::~ContentKeeper()
{
    // Never restored: the pixels it holds belong to nobody else now.
    if (m_payload && !m_payload->restored)
        for (int i = 0; i < maxTerminalImageCount; i++)
            dispose(&m_payload->stored[i]);
}

void disposeAllImages(Ubiquitty::Terminal& t)
{
    int i;
    for (i = 0; i < maxTerminalImageCount; i++)
        dispose(&t.images.stored[i]);
    for (i = 0; i < maxTerminalImagePlacementCount; i++)
        std::memset(&t.images.placements[i], 0, sizeof(t.images.placements[i]));
    t.images.livePlacementCount = 0;
}

// Count of currently-used placements. The renderer uses this together
// with Ubiquitty::Image::placement() to walk in z-order.
int placementCount(const Ubiquitty::Terminal* t)
{
    if (!t)
        return 0;
    int i;
    int n = 0;
    for (i = 0; i < maxTerminalImagePlacementCount; i++)
        if (t->images.placements[i].used)
            n++;
    return n;
}

// Allocate a free placement slot, or NULL when all slots are taken (do nothing
// rather than evict, which would surprise apps relying on placement identity).
TerminalImagePlacement* allocatePlacement(Ubiquitty::Terminal& t)
{
    int i;
    for (i = 0; i < maxTerminalImagePlacementCount; i++) {
        if (!t.images.placements[i].used)
            return &t.images.placements[i];
    }
    return nullptr;
}

// Recompute TerminalImage.displayed from the placement table: whether any
// placement of this image is live.
void syncMirror(Ubiquitty::Terminal& t, TerminalImage* image)
{
    int i;
    if (!image || !image->used)
        return;
    for (i = 0; i < maxTerminalImagePlacementCount; i++) {
        const TerminalImagePlacement* p = &t.images.placements[i];
        if (p->used && p->imageID == image->imageID) {
            image->displayed = true;
            return;
        }
    }
    // A Unicode-placeholder virtual placement has no grid placement but is still
    // "displayed" (positioned by U+10EEEE cells); keep it off the LRU's
    // non-displayed eviction pass.
    image->displayed = image->virtualPlacement;
}
// Remove placements that match (imageID, [placementID]). When
// matchMode is RemoveAll, removes all placements for that imageID.
// Returns the number of placements actually removed.
int removePlacements(Ubiquitty::Terminal& t, uint32_t imageID, PlacementMatchMode matchMode, uint32_t placementID)
{
    int i;
    int removed = 0;
    for (i = 0; i < maxTerminalImagePlacementCount; i++) {
        TerminalImagePlacement* p = &t.images.placements[i];
        if (!p->used || p->imageID != imageID)
            continue;
        if (matchMode == PlacementMatchMode::MatchID && (!p->hasPlacementID || p->placementID != placementID))
            continue;
        std::memset(p, 0, sizeof(*p));
        removed++;
    }
    t.images.livePlacementCount -= static_cast<uint32_t>(removed);
    return removed;
}

int removeAllPlacements(Ubiquitty::Terminal& t)
{
    int removed = 0;
    for (int i = 0; i < maxTerminalImagePlacementCount; i++) {
        TerminalImagePlacement* p = &t.images.placements[i];
        if (!p->used)
            continue;
        std::memset(p, 0, sizeof(*p));
        removed++;
    }
    t.images.livePlacementCount -= static_cast<uint32_t>(removed);
    return removed;
}

bool hasPlacement(Ubiquitty::Terminal& t, uint32_t imageID)
{
    for (int i = 0; i < maxTerminalImagePlacementCount; i++) {
        const TerminalImagePlacement* p = &t.images.placements[i];
        if (p->used && p->imageID == imageID)
            return true;
    }
    return false;
}

void syncAllMirrors(Ubiquitty::Terminal& t)
{
    for (int i = 0; i < maxTerminalImageCount; i++)
        syncMirror(t, &t.images.stored[i]);
}

void disposeUnreferenced(Ubiquitty::Terminal& t)
{
    for (int i = 0; i < maxTerminalImageCount; i++) {
        TerminalImage* image = &t.images.stored[i];
        if (image->used && !hasPlacement(t, image->imageID))
            dispose(image);
    }
}

void markDirty(Ubiquitty::Terminal& t, const TerminalImage* image)
{
    if (!image || !image->displayed)
        return;
    // Image dims are pixels while cell metrics live in the renderer, so invalidate
    // the whole grid rather than guessing which cells the image overlaps.
    Ubiquitty::Screen::markAllDirty(t);
}

// Drop placement slot `i`, disposing its image when no other placement
// references it. Shared by the scroll-off and erase-region paths.
static void dropPlacement(Ubiquitty::Terminal& t, int i)
{
    uint32_t id = t.images.placements[i].imageID;
    std::memset(&t.images.placements[i], 0, sizeof(t.images.placements[i]));
    t.images.livePlacementCount--; // callers only drop a slot they confirmed used
    for (int j = 0; j < maxTerminalImagePlacementCount; j++)
        if (t.images.placements[j].used && t.images.placements[j].imageID == id)
            return; // still referenced by another placement
    for (int k = 0; k < maxTerminalImageCount; k++) {
        if (t.images.stored[k].used && t.images.stored[k].imageID == id) {
            dispose(&t.images.stored[k]);
            return;
        }
    }
}

namespace {

// One axis of an OSC 1337 size, in the unit its token named.
enum class InlineImageUnit : uint8_t {
    Automatic,
    Cells,
    Pixels,
    Percent,
};

struct InlineImageAxis {
    InlineImageUnit unit { InlineImageUnit::Automatic };
    int value {};
};

// "N" cells, "Npx" device pixels, "N%" of the grid axis; empty, "auto" or
// anything without leading digits is automatic.
InlineImageAxis parseInlineImageAxis(const char* token)
{
    if (!token || !*token)
        return {};
    int value = 0;
    const char* cursor = token;
    while (*cursor >= '0' && *cursor <= '9') {
        // Saturating, as every sibling numeric parser on this hostile surface
        // is: a digit run is attacker-length, and a signed accumulation that
        // wraps is undefined before it is ever wrong.
        value = Ubiquitty::decimalAppendSaturated(value, *cursor - '0', 0x7FFFFFFF);
        cursor++;
    }
    if (cursor == token)
        return {};
    if (*cursor == '%')
        return { InlineImageUnit::Percent, std::min(value, 100) };
    if (cursor[0] == 'p' && cursor[1] == 'x')
        return { InlineImageUnit::Pixels, value };
    return { InlineImageUnit::Cells, value };
}

// The requested length of one axis in device pixels and the cells it covers,
// or nothing when the axis is automatic and follows the other one.
struct InlineImageLength {
    double pixels {};
    double cells {};
};

std::optional<InlineImageLength> statedInlineImageLength(InlineImageAxis axis, int gridCells, int cellPixels)
{
    switch (axis.unit) {
    case InlineImageUnit::Pixels:
        return InlineImageLength { static_cast<double>(axis.value), std::ceil(static_cast<double>(axis.value) / cellPixels) };
    case InlineImageUnit::Percent: {
        const double fraction = axis.value / 100.0;
        return InlineImageLength { gridCells * fraction * cellPixels, std::ceil(gridCells * fraction) };
    }
    case InlineImageUnit::Cells:
        return InlineImageLength { static_cast<double>(axis.value) * cellPixels, static_cast<double>(axis.value) };
    case InlineImageUnit::Automatic:
        break;
    }
    return std::nullopt;
}

// Image dimensions travel in uint16 placement fields; a hostile request
// saturates there rather than wrapping.
int clampToPlacementField(double value)
{
    if (!(value > 0))
        return 0;
    return static_cast<int>(std::min(value, 65535.0));
}

} // namespace

InlineImageExtent inlineImageExtent(const char* widthToken, const char* heightToken, const char* preserveToken, IntSize imagePixels, IntSize cellPixels, GridSize grid, int cursorColumn)
{
    if (imagePixels.width <= 0 || imagePixels.height <= 0)
        return {};
    const int cellWidth = cellPixels.width > 0 ? cellPixels.width : 8;
    const int cellHeight = cellPixels.height > 0 ? cellPixels.height : 16;
    const InlineImageAxis widthAxis = parseInlineImageAxis(widthToken);
    const InlineImageAxis heightAxis = parseInlineImageAxis(heightToken);
    const bool isAutomaticWidth = widthAxis.unit == InlineImageUnit::Automatic;
    const bool isAutomaticHeight = heightAxis.unit == InlineImageUnit::Automatic;
    const double aspect = static_cast<double>(imagePixels.width) / imagePixels.height;

    // iTerm2's order: width first, then height (from the width when it is
    // automatic), then an automatic width from that height. Both automatic is
    // the image's own pixels.
    InlineImageLength width;
    InlineImageLength height;
    if (isAutomaticWidth && isAutomaticHeight) {
        width = { static_cast<double>(imagePixels.width), std::ceil(static_cast<double>(imagePixels.width) / cellWidth) };
        height = { static_cast<double>(imagePixels.height), std::ceil(static_cast<double>(imagePixels.height) / cellHeight) };
    } else if (isAutomaticHeight) {
        width = *statedInlineImageLength(widthAxis, grid.columns, cellWidth);
        height.pixels = width.pixels / aspect;
        height.cells = std::ceil(height.pixels / cellHeight);
    } else {
        height = *statedInlineImageLength(heightAxis, grid.rows, cellHeight);
        if (isAutomaticWidth) {
            width.pixels = height.pixels * aspect;
            width.cells = std::round(width.pixels / cellWidth);
        } else {
            width = *statedInlineImageLength(widthAxis, grid.columns, cellWidth);
        }
    }
    width.cells = std::max(1.0, width.cells);
    height.cells = std::max(1.0, height.cells);

    // An image wider than the columns left of the anchor narrows to them, and
    // one taller than 255 rows shortens to that, each scaling the other axis
    // and restating the box in whole cells, as iTerm2 does.
    bool isRestatedInCells = false;
    const int columnsLeft = std::max(1, grid.columns - std::max(0, cursorColumn));
    if (width.cells > columnsLeft) {
        const double scale = columnsLeft / width.cells;
        width.cells = columnsLeft;
        height.cells = std::max(1.0, std::floor(height.cells * scale));
        isRestatedInCells = true;
    }
    constexpr double maxRows = 255;
    if (height.cells > maxRows) {
        const double scale = maxRows / height.cells;
        height.cells = maxRows;
        width.cells = std::max(1.0, std::floor(width.cells * scale));
        isRestatedInCells = true;
    }
    if (isRestatedInCells) {
        width.pixels = width.cells * cellWidth;
        height.pixels = height.cells * cellHeight;
    }

    InlineImageExtent extent;
    extent.coveredCells = { clampToPlacementField(width.cells), clampToPlacementField(height.cells) };
    const bool shouldPreserveAspect = !preserveToken || preserveToken[0] != '0';
    const bool isStatedInCells = !isRestatedInCells
        && widthAxis.unit != InlineImageUnit::Pixels && heightAxis.unit != InlineImageUnit::Pixels
        && !(isAutomaticWidth && isAutomaticHeight);
    if (isStatedInCells) {
        // A box of cells is drawn in the renderer's own cells, which can be
        // fractional where the published cell size is not. Preserving the
        // aspect keeps only the constraining axis for the renderer to derive
        // the other from; an automatic axis is derived the same way.
        int columns = isAutomaticWidth ? 0 : extent.coveredCells.width;
        int rows = isAutomaticHeight ? 0 : extent.coveredCells.height;
        if (shouldPreserveAspect && columns > 0 && rows > 0) {
            if (static_cast<double>(columns) * cellWidth * imagePixels.height > static_cast<double>(rows) * cellHeight * imagePixels.width)
                columns = 0;
            else
                rows = 0;
        }
        extent.placementCells = { columns, rows };
        return extent;
    }
    // Device pixels were named, directly or by the image's own size: the
    // picture is drawn at exactly that many, top-left in its box.
    double drawnWidth = width.pixels;
    double drawnHeight = height.pixels;
    if (shouldPreserveAspect) {
        const double scale = std::min(width.pixels / imagePixels.width, height.pixels / imagePixels.height);
        drawnWidth = imagePixels.width * scale;
        drawnHeight = imagePixels.height * scale;
    }
    extent.placementCells = extent.coveredCells;
    extent.drawnPixels = { std::max(1, clampToPlacementField(std::round(drawnWidth))), std::max(1, clampToPlacementField(std::round(drawnHeight))) };
    return extent;
}

void completeCellBox(int pixelWidth, int pixelHeight, int cellPixelWidth, int cellPixelHeight, int* cellColumns, int* cellRows)
{
    if (!cellColumns || !cellRows)
        return;
    const int columns = *cellColumns;
    const int rows = *cellRows;
    // Nothing to complete: both axes are stated, or neither is and the caller
    // means the image's natural size.
    if ((columns > 0) == (rows > 0))
        return;
    if (cellPixelWidth <= 0 || cellPixelHeight <= 0 || pixelWidth <= 0 || pixelHeight <= 0)
        return;
    if (columns > 0) {
        const double derived = static_cast<double>(columns) * pixelHeight * cellPixelWidth
            / (static_cast<double>(pixelWidth) * cellPixelHeight);
        *cellRows = (derived < 1.0) ? 1 : static_cast<int>(derived + 0.5);
        return;
    }
    const double derived = static_cast<double>(rows) * pixelWidth * cellPixelHeight
        / (static_cast<double>(pixelHeight) * cellPixelWidth);
    *cellColumns = (derived < 1.0) ? 1 : static_cast<int>(derived + 0.5);
}

// The top half of the id range. The counter's low 31 bits go under it, so
// the sync bump that raises the counter past a received id keeps working and
// the result never falls into the half a client names from.
constexpr uint32_t autoImageIDBase = 0x80000000u;

uint32_t allocateImageID(Ubiquitty::Terminal& t)
{
    return autoImageIDBase | (++t.images.kittyNextImageID & 0x7FFFFFFFu);
}

bool storeRGBAImage(Ubiquitty::Terminal* t, unsigned char* pixels, int w, int h, int cellColumns, int cellRows, IntSize drawnPixels)
{
    if (!t || !t->screens.active || !pixels || w <= 0 || h <= 0)
        return false;
    // Both limits belong here rather than at the two callers, because this is
    // where sixel and OSC 1337 meet and neither bounds what it hands over: the
    // sixel raster clamp is 2048x2048, but a PAL image decoder reports whatever
    // dimensions it decoded. The kitty paths apply the same per-image cap before
    // they reach the store, which is the invariant enforceBudget's eviction loop
    // already assumes when it gives up with only `keep` left.
    //
    // 64-bit, so a w*h*4 that wraps uint32 cannot pass as a small byte count.
    // The dimensions are checked too: width and height are uint16_t fields, and
    // truncating into them turns a 65536-pixel-wide image into a 0-wide one that
    // every consumer silently skips.
    const std::uint64_t byteCount = static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) * 4ULL;
    if (w > std::numeric_limits<std::uint16_t>::max() || h > std::numeric_limits<std::uint16_t>::max())
        return false;
    if (byteCount > static_cast<std::uint64_t>(maxDecodedImageBytes))
        return false;
    TerminalImage* image = allocateSlot(*t);
    if (!image)
        return false;
    image->used = true;
    image->displayed = false;
    image->imageID = allocateImageID(*t);
    image->imageNumber = 0;
    image->width = static_cast<uint16_t>(w);
    image->height = static_cast<uint16_t>(h);
    image->format = 32; // RGBA8, as the PAL image decoder emits
    image->pixelLength = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4u;
    image->pixels = pixels; // ownership transferred
    initFrameZero(image);
    // Keep the total inside the per-Terminal budget, as the kitty transmit paths
    // do. Without it sixel and OSC 1337 accumulated up to maxTerminalImageCount
    // images of maxDecodedImageBytes each, whatever the budget said.
    enforceBudget(t, image);
    // Place at the cursor with the requested cell rectangle.
    TerminalImagePlacement* p = allocatePlacement(*t);
    if (p) {
        std::memset(p, 0, sizeof(*p));
        p->used = true;
        t->images.livePlacementCount++; // allocatePlacement handed out an unused slot
        p->imageID = image->imageID;
        p->cursorX = static_cast<uint16_t>(t->screens.active->cursorX);
        p->cursorY = static_cast<int16_t>(t->screens.active->cursorY);
        p->cellColumns = static_cast<uint16_t>(cellColumns);
        p->cellRows = static_cast<uint16_t>(cellRows);
        if (drawnPixels.width > 0 && drawnPixels.height > 0) {
            p->drawnPixelWidth = static_cast<uint16_t>(std::min(drawnPixels.width, 65535));
            p->drawnPixelHeight = static_cast<uint16_t>(std::min(drawnPixels.height, 65535));
        }
        p->onAlternateScreen = t->screens.isUsingAlternateScreen ? 1u : 0u;
    }
    // Cursor advancement is the caller's responsibility: each protocol states
    // where its cursor goes.
    syncMirror(*t, image);
    markDirty(*t, image);
    return true;
}

// Whole rows occupied by a picture, for scrolling and eviction only. Rendering
// retains the requested axis or native pixel size and does not use this ceiling.
static int placementRowExtent(const Ubiquitty::Terminal& terminal, const TerminalImagePlacement& placement)
{
    if (placement.cellRows) {
        return placement.cellRows;
    }
    for (const auto& image : terminal.images.stored) {
        if (!image.used || image.imageID != placement.imageID) {
            continue;
        }
        const int cellWidth = terminal.cellPixelWidth > 0 ? terminal.cellPixelWidth : 8;
        const int cellHeight = terminal.cellPixelHeight > 0 ? terminal.cellPixelHeight : 16;
        const int sourceWidth = placement.sourceWidth ? placement.sourceWidth : image.width - placement.sourceX;
        const int sourceHeight = placement.sourceHeight ? placement.sourceHeight : image.height - placement.sourceY;
        if (sourceWidth <= 0 || sourceHeight <= 0) {
            return 1;
        }
        const double height = placement.cellColumns
            ? static_cast<double>(placement.cellColumns) * cellWidth * sourceHeight / sourceWidth
            : sourceHeight;
        return static_cast<int>(std::clamp(std::ceil((height + placement.pixelOffsetY) / cellHeight), 1.0, 65535.0));
    }
    return 1;
}

void scrollPlacementsUp(Ubiquitty::Terminal& t, int n)
{
    if (n <= 0)
        return;
    // Gate on livePlacementCount, not kittyNextImageID: the latter stays zero for
    // explicit-id (a=T,i=N) placements, so gating on it would skip the shift and
    // pin those images to grid rows while text scrolled beneath them.
    if (t.images.livePlacementCount == 0)
        return;
    const bool alternateActive = t.screens.isUsingAlternateScreen;
    for (int i = 0; i < maxTerminalImagePlacementCount; i++) {
        TerminalImagePlacement* p = &t.images.placements[i];
        if (!p->used)
            continue;
        if (!placementIsOnActiveScreen(*p, alternateActive))
            continue;
        // Keep placements while their row is still reachable by scrolling
        // back through history; drop once scrolled past the oldest retained
        // scrollback line, or past the oldest row the anchor can hold.
        const int next = p->cursorY - n;
        if (next < minPlacementRow || next + placementRowExtent(t, *p) <= -t.scrollbackLimit) {
            dropPlacement(t, i);
            continue;
        }
        p->cursorY = static_cast<int16_t>(next);
    }
}

void clearPlacementsInRows(Ubiquitty::Terminal& t, int fromRow, int toRow)
{
    if (fromRow > toRow)
        return;
    const bool alternateActive = t.screens.isUsingAlternateScreen;
    for (int i = 0; i < maxTerminalImagePlacementCount; i++) {
        TerminalImagePlacement* p = &t.images.placements[i];
        if (!p->used)
            continue;
        if (!placementIsOnActiveScreen(*p, alternateActive))
            continue;
        int rows = placementRowExtent(t, *p);
        int top = p->cursorY;
        int bot = p->cursorY + rows - 1;
        if (top <= toRow && bot >= fromRow)
            dropPlacement(t, i);
    }
}

bool hasPlacementOnViewport(const Ubiquitty::Terminal& t, int viewOffset)
{
    if (t.images.livePlacementCount == 0)
        return false;
    const bool isAlternateActive = t.screens.isUsingAlternateScreen;
    for (int i = 0; i < maxTerminalImagePlacementCount; i++) {
        const TerminalImagePlacement* p = &t.images.placements[i];
        if (!p->used || !placementIsOnActiveScreen(*p, isAlternateActive))
            continue;
        if (t.modes.fullscreenContent)
            return true;
        // The same row shift the resolver applies: the view moves the anchor
        // down by the rows of history above the live tail.
        const int top = static_cast<int>(p->cursorY) + viewOffset;
        const int rows = placementRowExtent(t, *p);
        if (top < t.rows && top + rows > 0)
            return true;
    }
    return false;
}

void advanceCursorAfterImage(Ubiquitty::Terminal* t, IntSize coveredCells, CursorMotion motion)
{
    if (!t || !t->screens.active || motion == CursorMotion::Stay)
        return;
    Ubiquitty::Screen::State& screen = *t->screens.active;
    int newX = screen.cursorX + std::max(0, coveredCells.width);
    int newY = screen.cursorY + std::max(0, coveredCells.height - 1);
    if (newX >= t->columns) {
        newX = 0;
        newY++;
    }
    if (newY >= t->rows) {
        Ubiquitty::Screen::scrollViewportUp(t, newY - (t->rows - 1));
        newY = t->rows - 1;
    }
    screen.cursorX = std::max(0, newX);
    screen.cursorY = std::max(0, newY);
    screen.wrapPending = false;
}
// The host owns the live screen; client scrollback views are independent and
// cannot pin its cache. Virtual placements are conservatively protected because
// their anchors are encoded in text cells rather than in the placement table.
static bool hasLivePlacement(const Ubiquitty::Terminal& terminal, const TerminalImage& image)
{
    if (image.virtualPlacement) {
        return true;
    }
    for (const auto& placement : terminal.images.placements) {
        if (!placement.used || placement.imageID != image.imageID) {
            continue;
        }
        if (terminal.modes.fullscreenContent) {
            return true;
        }
        if (placement.cursorY < terminal.rows && static_cast<int>(placement.cursorY) + placementRowExtent(terminal, placement) > 0) {
            return true;
        }
    }
    return false;
}

TerminalImage* allocateSlot(Ubiquitty::Terminal& t)
{
    for (int i = 0; i < maxTerminalImageCount; i++)
        if (!t.images.stored[i].used)
            return &t.images.stored[i];
    // Every slot is live. The victim is the oldest image nothing is drawing,
    // by the rule the budget eviction already applies — and its placements go
    // with it, because a placement outliving its pixels addresses a slot whose
    // contents belong to a different picture.
    //
    // Taking slot zero instead would replace whichever image happens to sit
    // there, on screen or not, and leave its placements behind, each drawing
    // an image disposed under it.
    TerminalImage* victim = nullptr;
    for (int i = 0; i < maxTerminalImageCount; i++) {
        TerminalImage* image = &t.images.stored[i];
        if (hasLivePlacement(t, *image))
            continue;
        if (!victim || image->lastUsedSeq < victim->lastUsedSeq)
            victim = image;
    }
    // Nothing is evictable when every image is on screen, and the answer is
    // the refusal this returns: the transfer is dropped rather than replacing
    // a picture a reader is looking at.
    if (!victim)
        return nullptr;
    removePlacements(t, victim->imageID, PlacementMatchMode::RemoveAll, 0);
    dispose(victim);
    victim->used = false;
    victim->displayed = false;
    victim->lastUsedSeq = 0;
    return victim;
}

uint64_t imageBytes(const TerminalImage& image)
{
    if (!image.used)
        return 0;
    uint64_t total = image.pixelLength; // frames[0] aliases pixels — not double-counted
    for (int f = 1; f < image.frameCount && f < maxTerminalImageFrameCount; f++)
        if (image.frames[f].used)
            total += image.frames[f].pixelLength;
    return total;
}

uint64_t totalBytes(const Ubiquitty::Terminal* t)
{
    if (!t)
        return 0;
    uint64_t total = 0;
    for (int i = 0; i < maxTerminalImageCount; i++)
        total += imageBytes(t->images.stored[i]);
    return total;
}

void enforceBudget(Ubiquitty::Terminal* t, TerminalImage* keep)
{
    if (!t)
        return;
    if (keep)
        keep->lastUsedSeq = ++t->images.kittyImageUseSeq; // most-recently stored
    while (Ubiquitty::Image::totalBytes(t) > t->images.kittyImageBytesBudget) {
        // Victim: oldest stored (smallest lastUsedSeq), never `keep`. Pass 0
        // considers only non-displayed images; pass 1 falls back to any.
        TerminalImage* victim = nullptr;
        for (int pass = 0; pass < 2 && !victim; pass++) {
            const bool nonDisplayedOnly = (pass == 0);
            for (int i = 0; i < maxTerminalImageCount; i++) {
                TerminalImage* image = &t->images.stored[i];
                if (!image->used || image == keep)
                    continue;
                if (nonDisplayedOnly && hasLivePlacement(*t, *image))
                    continue;
                if (!victim || image->lastUsedSeq < victim->lastUsedSeq)
                    victim = image;
            }
        }
        if (!victim)
            break; // only `keep` remains (bounded by the 64 MiB per-image cap)
        removePlacements(*t, victim->imageID, PlacementMatchMode::RemoveAll, 0);
        dispose(victim);
        victim->used = false;
        victim->displayed = false;
        victim->lastUsedSeq = 0;
    }
}

// Public image accessors + animation tick.

const TerminalImage* stored(const Ubiquitty::Terminal* t, int index)
{
    if (!t || index < 0 || index >= maxTerminalImageCount)
        return nullptr;
    if (!t->images.stored[index].used)
        return nullptr;
    return &t->images.stored[index];
}

const TerminalImagePlacement* placement(const Ubiquitty::Terminal* t, int index)
{
    if (!t || index < 0 || index >= maxTerminalImagePlacementCount)
        return nullptr;
    if (!t->images.placements[index].used)
        return nullptr;
    return &t->images.placements[index];
}

bool anyAnimating(const Ubiquitty::Terminal* t)
{
    int i;
    if (!t)
        return false;
    for (i = 0; i < maxTerminalImageCount; i++) {
        const TerminalImage* image = &t->images.stored[i];
        if (!image->used)
            continue;
        if (image->animationState == AnimationState::Stopped)
            continue;
        if (image->frameCount <= 1)
            continue;
        return true;
    }
    return false;
}

void advanceFrames(Ubiquitty::Terminal* t, uint32_t elapsedMilliseconds)
{
    int i;
    if (!t || elapsedMilliseconds == 0)
        return;
    for (i = 0; i < maxTerminalImageCount; i++) {
        TerminalImage* image = &t->images.stored[i];
        uint32_t remaining;
        if (!image->used)
            continue;
        if (image->animationState == AnimationState::Stopped)
            continue;
        if (image->frameCount <= 1)
            continue;
        // The frame the walk starts from indexes `frames`, so one past the
        // count stops the animation rather than being read.
        if (image->currentFrame >= image->frameCount || image->frameCount > image->frames.size()) {
            image->animationState = AnimationState::Stopped;
            continue;
        }
        remaining = elapsedMilliseconds;
        // Whole cycles change nothing: a looping animation returns to this
        // frame with this clock after one, so dropping them lands on the same
        // frame rather than near it. Without that the iteration count is the
        // elapsed milliseconds over the smallest frame delay — a delay the
        // sender chooses, multiplied by a clock that has been idle through a
        // backgrounded app or a monotonic wrap.
        //
        // A non-looping animation needs no bound: it stops at its last frame,
        // so it can take at most one step per frame however long the gap.
        if (image->animationState == AnimationState::Looping) {
            std::uint64_t cycleMilliseconds = 0;
            for (int f = 0; f < image->frameCount; f++) {
                const TerminalImageFrame* cycleFrame = &image->frames[f];
                cycleMilliseconds += cycleFrame->gapless
                    ? 0u
                    : (cycleFrame->delayMilliseconds == 0 ? terminalImageDefaultFrameGapMilliseconds : cycleFrame->delayMilliseconds);
            }
            if (cycleMilliseconds > 0 && remaining >= cycleMilliseconds)
                remaining = static_cast<uint32_t>(remaining % cycleMilliseconds);
        }
        while (remaining > 0 && image->animationState != AnimationState::Stopped) {
            const TerminalImageFrame* frame = &image->frames[image->currentFrame];
            // Effective gap: a gapless frame (kitty z<0) skips instantly; a
            // 0/unspecified gap uses the 40ms default so the frame is shown.
            uint32_t delay = frame->gapless ? 0 : (frame->delayMilliseconds == 0 ? terminalImageDefaultFrameGapMilliseconds : frame->delayMilliseconds);
            uint32_t step;
            if (delay == 0) {
                // Gapless frame: advance immediately, don't burn ms.
                step = 0;
            } else if (image->frameClockMilliseconds + remaining < delay) {
                image->frameClockMilliseconds += remaining;
                remaining = 0;
                break;
            } else {
                step = delay - image->frameClockMilliseconds;
                remaining -= step;
            }
            image->frameClockMilliseconds = 0;
            image->currentFrame++;
            if (image->currentFrame >= image->frameCount) {
                if (image->animationState == AnimationState::Looping) {
                    image->currentFrame = 0;
                } else {
                    image->currentFrame = static_cast<uint16_t>(image->frameCount - 1);
                    image->animationState = AnimationState::Stopped;
                    break;
                }
            }
            // Guard against infinite loop on all-gapless frames.
            if (step == 0 && delay == 0)
                break;
        }
        markDirty(*t, image);
    }
}

} // namespace Ubiquitty::Image
