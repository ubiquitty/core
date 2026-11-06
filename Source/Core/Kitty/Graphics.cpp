// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Kitty graphics wire protocol (https://sw.kovidgoyal.net/kitty/graphics-protocol/)
// — a decoder backend for the TerminalImage model.
// Owns the payload-decode / dispatch step: parses APC graphics-transfer controls,
// resolves transports (base64 / zlib / file / shared-memory), decodes PNG and
// direct-pixel payloads, and installs the result into TerminalImage /
// TerminalImagePlacement slots. The VT100 parser owns APC-sequence recognition;
// this module owns what recognition hands off. The t=f / t=t file transport reads
// through Terminal::fileReadClient.

#include "Core/Kitty/Graphics.h"
#include "Core/Codec/Base64.h"
#include "Core/Codec/PNGDecoder.h"
#include "Core/Codec/ZlibInflate.h"
#include "Core/Image/TerminalImageInternal.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Terminal.h"
#include "PAL/Capability.h"
#include "PAL/PAL.h"
#include "PAL/TaskQueue.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Kitty {

// Async PNG decode pipeline. The decode is submitted to a process-global
// TaskQueue; each job carries the Terminal* it belongs to, and completion installs
// into that Terminal's images.stored[]. Transport resolution (base64 decode +
// file-read for t=f) stays on the parse thread because the file-read hook may
// touch the filesystem; only the PNG decode runs in the worker.

// The shared decode-buffer owner: frees on destruction, so an owned buffer can't
// leak down an early-return path; ownership transfer to a TerminalImage is an
// explicit .release(). Sixel decodes into the same store under the same free()
// contract, so the type lives with the store rather than here.
using KittyBytes = Ubiquitty::Image::OwnedPixels;

// RAII wrapper for malloc'd char buffers (path strings, etc.).
struct FreeChar {
    void operator()(char* p) const noexcept { std::free(p); }
};
using OwnedCharBuffer = std::unique_ptr<char, FreeChar>;

// What an extraction produces. The buffer arrives owned, so no raw pointer
// crosses this boundary for a caller to adopt — an early return between the two
// leaks it, and a refusal that reports success hands over a null to decode.
struct ExtractedBytes {
    KittyBytes bytes;
    int length;
};

// A decoded image. The five values only describe a picture together — a caller
// holding the pixels without the geometry has a buffer it cannot draw, and the
// buffer comes back owned so it is freed by dropping the answer.
struct DecodedImage {
    KittyBytes pixels;
    int pixelLength;
    int width;
    int height;
    int format;
};

// A refusal names itself, at the refusal. A dispatcher substituting one phrase
// for whichever path said nothing gives a client an answer that cannot tell one
// failure from another.
constexpr const char* fileTransportFailed = "EBADF:file transport failed";
constexpr const char* sharedMemoryTransportFailed = "EBADF:shared memory transport failed";

// Whether a t=t (temp-file) transport may unlink the client's file after reading.
// A real a=t transmit Consumes it (kitty lets the terminal delete a temp file);
// an a=q PROBE Preserves it — a query validates the medium without the side
// effect, running the same validation either way.
enum class TempFileUnlink : uint8_t {
    Consume,
    Preserve
};

// a=T transmits and displays the image (installs a placement); a=t transmits only.
enum class TransmitDisplay : uint8_t {
    TransmitOnly,
    TransmitAndDisplay
};

struct KittyPNGDecodeJob {
    Ubiquitty::Terminal* term;
    uint32_t terminalGeneration; // snapshot at submit; checked in completion
    Ubiquitty::Kitty::GraphicsControl ctrl; // copy; original is on caller's stack
    TransmitDisplay transmitDisplay; // a=T installs a placement on completion; a=t transmits only
    KittyBytes pngBytes; // owned; freed when the job is destroyed
    int pngLength;
    // Worker fills these:
    KittyBytes pixels; // owned; .release()d into the TerminalImage on success
    int pixelLength;
    int width;
    int height;
    int format;
    bool succeeded;
};

struct KittyDirectInflateJob {
    Ubiquitty::Terminal* term;
    uint32_t terminalGeneration;
    Ubiquitty::Kitty::GraphicsControl ctrl;
    TransmitDisplay transmitDisplay;
    KittyBytes compressed; // owned; freed when the job is destroyed
    int compressedLength;
    int expectedLength; // raw RGB(A) byte count
                        // Worker fills these:
    KittyBytes inflated; // owned; .release()d into the TerminalImage on success
    int inflatedLength;
    bool succeeded;
};

// Constructed on the Terminal's first decode submission, never eagerly, so the
// common no-decode APC allocates neither queue.
static Ubiquitty::TaskQueue* kittyEnsureDecodeQueue(Ubiquitty::Terminal& t)
{
    if (!t.asyncDecodes.kittyDecode)
        t.asyncDecodes.kittyDecode = std::make_unique<Ubiquitty::TaskQueue>();
    return t.asyncDecodes.kittyDecode.get();
}

static Ubiquitty::TaskQueue* kittyEnsureInflateQueue(Ubiquitty::Terminal& t)
{
    if (!t.asyncDecodes.kittyInflate)
        t.asyncDecodes.kittyInflate = std::make_unique<Ubiquitty::TaskQueue>();
    return t.asyncDecodes.kittyInflate.get();
}

// Why a direct payload did not decode. Each caller answers in its own words,
// because the reply text is what a client acts on.
enum class PayloadDecodeFailure : uint8_t {
    Malformed,
    Allocation
};

// Base64-decodes a direct-transport payload into one owned buffer of exactly
// its decoded size, so a caller keeps that buffer as its result instead of
// copying out of it. A payload that decodes to nothing is malformed: every
// transfer carries bytes.
static std::expected<ExtractedBytes, PayloadDecodeFailure> kittyDecodePayload(std::span<const char> payload)
{
    // Both accumulators that hand a payload here stop at `maxAPCCapacity`;
    // holding it to that here keeps the decoded length an `int` for any caller.
    if (payload.size() > static_cast<std::size_t>(Ubiquitty::Kitty::maxAPCCapacity))
        return std::unexpected(PayloadDecodeFailure::Malformed);
    const std::string_view characters(payload.data(), payload.size());
    const std::size_t capacity = Ubiquitty::Codec::base64DecodedCapacity(characters);
    // No room means no byte can decode. Refusing here also keeps the null that
    // `malloc(0)` may answer from reading as an allocation failure.
    if (!capacity)
        return std::unexpected(PayloadDecodeFailure::Malformed);
    KittyBytes bytes(static_cast<unsigned char*>(std::malloc(capacity)));
    if (!bytes)
        return std::unexpected(PayloadDecodeFailure::Allocation);
    // The codec decodes into char; the buffer is unsigned char because it ends
    // up as pixels.
    const auto decoded = Ubiquitty::Codec::base64Decode(characters, { reinterpret_cast<char*>(bytes.get()), capacity });
    if (!decoded || decoded->empty())
        return std::unexpected(PayloadDecodeFailure::Malformed);
    return ExtractedBytes { std::move(bytes), static_cast<int>(decoded->size()) };
}

// Forward decls of file-scope helpers used by the worker + completion;
// actual definitions appear later in this file.
static std::optional<DecodedImage> kittyDecodePNGBytes(std::span<const unsigned char> pngBytes);
static Image::TerminalImage* kittyFindImage(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c);
static void kittyApplyVirtualPlacement(Image::TerminalImage* image, const Ubiquitty::Kitty::GraphicsControl* c);
static void kittyFillPlacement(Image::TerminalImagePlacement* p, Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, uint32_t imageID);
static void kittyAdvanceCursorAfterImage(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, const Ubiquitty::Image::TerminalImage* image);
static void kittyRespond(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, std::string_view status);

// Direct-pixel (f=24/32) zlib inflate worker. Completion validates the size
// against the expected length before installing into a TerminalImage slot.
static void kittyDirectInflateWorker(void* p)
{
    KittyDirectInflateJob* j = static_cast<KittyDirectInflateJob*>(p);
    if (j->expectedLength <= 0) {
        j->succeeded = false;
        return;
    }
    KittyBytes outputBuffer(static_cast<unsigned char*>(malloc(static_cast<size_t>(j->expectedLength))));
    if (!outputBuffer) {
        j->succeeded = false;
        return;
    }
    // Stack-owned inflate state (~33 KB); this runs on a TaskQueue worker.
    Ubiquitty::Codec::Zlib::Inflater inflater;
    const Ubiquitty::Codec::Zlib::Inflater::DecodeResult result = inflater.decode({ j->compressed.get(), static_cast<std::size_t>(j->compressedLength) }, { outputBuffer.get(), static_cast<std::size_t>(j->expectedLength) });
    if (result.status != Ubiquitty::Codec::Zlib::InflateStatus::Done) {
        j->succeeded = false; // out auto-freed
        return;
    }
    j->inflated = std::move(outputBuffer);
    j->inflatedLength = static_cast<int>(result.bytesProduced);
    j->succeeded = true;
}

static void kittyDirectInflateCompletion(void* p)
{
    // Adopt the job so every early return frees it and its owned buffers.
    std::unique_ptr<KittyDirectInflateJob> j(static_cast<KittyDirectInflateJob*>(p));
    j->compressed.reset(); // input no longer needed once inflate finished
    if (!j->term || j->term->generation != j->terminalGeneration)
        return;
    if (!j->succeeded || j->inflatedLength != j->expectedLength) {
        kittyRespond(*j->term, &j->ctrl, "EINVAL:payload size mismatch");
        return;
    }
    Image::TerminalImage* image = kittyFindImage(*j->term, &j->ctrl);
    if (!image) {
        image = Image::allocateSlot(*j->term);
    } else {
        Image::removePlacements(*j->term, image->imageID, Image::PlacementMatchMode::RemoveAll, 0);
        Image::dispose(image);
    }
    if (!image) {
        kittyRespond(*j->term, &j->ctrl, "ENOSPC:no image slot");
        return;
    }
    image->used = true;
    image->displayed = false;
    image->imageID = j->ctrl.hasImageID ? j->ctrl.imageID : Image::allocateImageID(*j->term);
    image->imageNumber = j->ctrl.hasImageNumber ? j->ctrl.imageNumber : 0;
    image->width = static_cast<uint16_t>(j->ctrl.width);
    image->height = static_cast<uint16_t>(j->ctrl.height);
    image->format = static_cast<uint16_t>(j->ctrl.format);
    image->pixelLength = static_cast<uint32_t>(j->inflatedLength);
    image->pixels = j->inflated.release(); // ownership transfers to the TerminalImage
    image->contentGeneration = ++j->term->images.kittyImageContentSeq;
    Image::initFrameZero(image);
    Image::enforceBudget(j->term, image);
    if (j->transmitDisplay == TransmitDisplay::TransmitAndDisplay && j->ctrl.unicodePlacement == 1) {
        kittyApplyVirtualPlacement(image, &j->ctrl);
    } else if (j->transmitDisplay == TransmitDisplay::TransmitAndDisplay) {
        Image::TerminalImagePlacement* p = Image::allocatePlacement(*j->term);
        if (p)
            kittyFillPlacement(p, *j->term, &j->ctrl, image->imageID);
        kittyAdvanceCursorAfterImage(*j->term, &j->ctrl, image);
    }
    Image::syncMirror(*j->term, image);
    Image::markDirty(*j->term, image);
    kittyRespond(*j->term, &j->ctrl, "OK");
}

// Worker: runs inline in the TaskQueue at submit. Pure CPU; touches
// only the job struct + the heap buffer it owns.
static void kittyPNGDecodeWorker(void* p)
{
    KittyPNGDecodeJob* j = static_cast<KittyPNGDecodeJob*>(p);
    std::optional<DecodedImage> decoded = kittyDecodePNGBytes({ j->pngBytes.get(), static_cast<size_t>(j->pngLength) });
    j->succeeded = decoded.has_value();
    if (!decoded) {
        j->pixels.reset();
        return;
    }
    j->pixels = std::move(decoded->pixels);
    j->pixelLength = decoded->pixelLength;
    j->width = decoded->width;
    j->height = decoded->height;
    j->format = decoded->format;
}

// Completion: runs in main task. Installs pixels into TerminalImage
// slot, allocates placement (if requested), emits kitty response.
static void kittyPNGDecodeCompletion(void* p)
{
    // Adopt the job so every early return frees it (and its owned buffers).
    std::unique_ptr<KittyPNGDecodeJob> j(static_cast<KittyPNGDecodeJob*>(p));
    j->pngBytes.reset(); // the PNG bytes are consumed either way
    // Terminal disposed / re-init'd between submit and completion: drop the pixels.
    if (!j->term || j->term->generation != j->terminalGeneration)
        return;
    if (!j->succeeded) {
        kittyRespond(*j->term, &j->ctrl, "EINVAL:png decode failed");
        return;
    }
    // The decoder answers the real dimensions; the control payload's were only
    // the sender's claim, and the stored image carries sixteen bits of each.
    if (j->width > 0xFFFF || j->height > 0xFFFF) {
        kittyRespond(*j->term, &j->ctrl, "EINVAL:image too large");
        return;
    }
    Image::TerminalImage* image = kittyFindImage(*j->term, &j->ctrl);
    if (!image) {
        image = Image::allocateSlot(*j->term);
    } else {
        // Re-storing an existing imageID: drop old placements + pixels.
        Image::removePlacements(*j->term, image->imageID, Image::PlacementMatchMode::RemoveAll, 0);
        Image::dispose(image);
    }
    if (!image) {
        // No slot — the decoded pixels drop with the job.
        kittyRespond(*j->term, &j->ctrl, "ENOSPC:no image slot");
        return;
    }
    image->used = true;
    image->displayed = false;
    image->imageID = j->ctrl.hasImageID ? j->ctrl.imageID : Image::allocateImageID(*j->term);
    image->imageNumber = j->ctrl.hasImageNumber ? j->ctrl.imageNumber : 0;
    image->width = static_cast<uint16_t>(j->width);
    image->height = static_cast<uint16_t>(j->height);
    image->format = static_cast<uint16_t>(j->format);
    image->pixelLength = static_cast<uint32_t>(j->pixelLength);
    image->pixels = j->pixels.release(); // ownership transfers to the TerminalImage
    image->contentGeneration = ++j->term->images.kittyImageContentSeq;
    Image::initFrameZero(image);
    Image::enforceBudget(j->term, image);
    if (j->transmitDisplay == TransmitDisplay::TransmitAndDisplay && j->ctrl.unicodePlacement == 1) {
        kittyApplyVirtualPlacement(image, &j->ctrl);
    } else if (j->transmitDisplay == TransmitDisplay::TransmitAndDisplay) {
        Image::TerminalImagePlacement* p = Image::allocatePlacement(*j->term);
        if (p)
            kittyFillPlacement(p, *j->term, &j->ctrl, image->imageID);
        kittyAdvanceCursorAfterImage(*j->term, &j->ctrl, image);
    }
    Image::syncMirror(*j->term, image);
    Image::markDirty(*j->term, image);
    kittyRespond(*j->term, &j->ctrl, "OK");
}

static void kittyControlInit(Ubiquitty::Kitty::GraphicsControl* c)
{
    std::memset(c, 0, sizeof(*c));
    c->action = 't';
    c->transport = 'd';
    c->format = 32;
    c->width = 0;
    c->height = 0;
    c->deleteMode = 0;
}

// The cells a placement covers, which is what the cursor steps past: kitty's
// update_dest_rect. A stated c and r are the box; one stated axis derives the
// other from the source rectangle's aspect, rounded up; neither means the
// source rectangle at its own device pixels plus the X/Y offset, rounded up to
// whole cells.
static Ubiquitty::IntSize kittyPlacedCells(const Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, const Ubiquitty::Image::TerminalImage* image)
{
    // A stated box is clamped to the same ceiling the derived path uses, so a
    // c or r saturated to INT_MAX cannot carry through to the cursor advance,
    // where cursorX + columns would overflow a signed int.
    int columns = std::clamp(c->cellColumns, 0, 65535);
    int rows = std::clamp(c->cellRows, 0, 65535);
    if (!image || image->width == 0 || image->height == 0 || (columns > 0 && rows > 0))
        return { columns, rows };
    const double cellWidth = t.cellPixelWidth > 0 ? t.cellPixelWidth : 8;
    const double cellHeight = t.cellPixelHeight > 0 ? t.cellPixelHeight : 16;
    const int sourceX = std::clamp(c->sourceX, 0, static_cast<int>(image->width));
    const int sourceY = std::clamp(c->sourceY, 0, static_cast<int>(image->height));
    const int sourceWidth = c->sourceWidth > 0 ? std::min(c->sourceWidth, image->width - sourceX) : image->width - sourceX;
    const int sourceHeight = c->sourceHeight > 0 ? std::min(c->sourceHeight, image->height - sourceY) : image->height - sourceY;
    if (sourceWidth <= 0 || sourceHeight <= 0)
        return { columns, rows };
    const double offsetX = std::max(0, c->pixelOffsetX);
    const double offsetY = std::max(0, c->pixelOffsetY);
    const auto cells = [](double pixels, double cell) { return static_cast<int>(std::min(65535.0, std::ceil(pixels / cell))); };
    if (columns == 0 && rows == 0)
        return { cells(sourceWidth + offsetX, cellWidth), cells(sourceHeight + offsetY, cellHeight) };
    if (columns == 0)
        return { cells((cellHeight * rows + offsetY) * sourceWidth / sourceHeight, cellWidth), rows };
    return { columns, cells((cellWidth * columns + offsetX) * sourceHeight / sourceWidth, cellHeight) };
}

// C=1 leaves the cursor alone; otherwise it steps past the covered cells.
static void kittyAdvanceCursorAfterImage(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, const Ubiquitty::Image::TerminalImage* image)
{
    Ubiquitty::Image::advanceCursorAfterImage(&t, kittyPlacedCells(t, c, image), (c->cursorPolicy == 1) ? Ubiquitty::Image::CursorMotion::Stay : Ubiquitty::Image::CursorMotion::Advance);
}

// Record a Unicode-placeholder VIRTUAL placement (U=1). No grid placement is
// created and the cursor is not advanced — the image is positioned later by
// U+10EEEE cells. c/r become the virtual grid they index into (0 = derive).
static void kittyApplyVirtualPlacement(Image::TerminalImage* image, const Ubiquitty::Kitty::GraphicsControl* c)
{
    image->virtualPlacement = true;
    image->virtualColumns = static_cast<uint16_t>(c->cellColumns);
    image->virtualRows = static_cast<uint16_t>(c->cellRows);
    image->virtualPlacementID = c->hasPlacementID ? c->placementID : 0;
    image->displayed = true; // counts as displayed (kept off the LRU non-displayed pass)
}

// Populate placement geometry fields from the parsed control.
//
// An unspecified axis stays unspecified: the renderer derives its exact pixel
// extent from the source aspect, without rounding it to a whole cell.
static void kittyFillPlacement(Image::TerminalImagePlacement* p, Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, uint32_t imageID)
{
    std::memset(p, 0, sizeof(*p));
    p->used = true;
    // The memset above zeroes this, which reads as the primary screen; a kitty
    // placement made while the alternate screen is up belongs to that one.
    p->onAlternateScreen = t.screens.isUsingAlternateScreen ? 1u : 0u;
    t.images.livePlacementCount++; // allocatePlacement handed out an unused slot
    p->imageID = imageID;
    p->placementID = c->hasPlacementID ? c->placementID : 0;
    p->hasPlacementID = c->hasPlacementID;
    p->cursorX = static_cast<uint16_t>(t.screens.active->cursorX);
    p->cursorY = static_cast<uint16_t>(t.screens.active->cursorY);
    p->cellColumns = static_cast<uint16_t>(c->cellColumns);
    p->cellRows = static_cast<uint16_t>(c->cellRows);
    p->pixelOffsetX = static_cast<uint16_t>(c->pixelOffsetX);
    p->pixelOffsetY = static_cast<uint16_t>(c->pixelOffsetY);
    p->sourceX = static_cast<uint16_t>(c->sourceX);
    p->sourceY = static_cast<uint16_t>(c->sourceY);
    p->sourceWidth = static_cast<uint16_t>(c->sourceWidth);
    p->sourceHeight = static_cast<uint16_t>(c->sourceHeight);
    p->zIndex = c->hasZIndex ? c->zIndex : 0;
    p->cursorPolicy = static_cast<uint8_t>(c->cursorPolicy);
}

static int kittyParseUInt(const char* s)
{
    int n = 0;
    bool saw = false;
    while (*s >= '0' && *s <= '9') {
        n = Ubiquitty::decimalAppendSaturated(n, *s - '0', 0x7FFFFFFF);
        s++;
        saw = true;
    }
    if (!saw || *s != 0)
        return -1;
    return n;
}

// Parse a signed decimal integer (the `z` key may be negative for under-text
// layering). Returns true on success, writing *out.
static bool kittyParseSInt(const char* s, int32_t* outputBuffer)
{
    bool neg = false;
    int32_t n = 0;
    bool saw = false;
    if (*s == '-') {
        neg = true;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        n = static_cast<int32_t>(Ubiquitty::decimalAppendSaturated(static_cast<int>(n), *s - '0', 0x7FFFFFFF));
        s++;
        saw = true;
    }
    if (!saw || *s != 0)
        return false;
    *outputBuffer = neg ? -n : n;
    return true;
}

static void kittyParseControl(char* data, Ubiquitty::Kitty::GraphicsControl* outputBuffer)
{
    char* p = data;
    kittyControlInit(outputBuffer);
    while (p && *p) {
        char* token = p;
        char* comma = strchr(token, ',');
        char* eq;
        int n;
        if (comma) {
            *comma = 0;
            p = comma + 1;
        } else {
            p = nullptr;
        }
        eq = strchr(token, '=');
        if (!eq || eq == token || eq[1] == 0)
            continue;
        *eq++ = 0;
        n = kittyParseUInt(eq);
        switch (token[0]) {
        case 'a':
            outputBuffer->action = eq[0];
            outputBuffer->hasAction = true;
            break;
        case 't':
            outputBuffer->transport = eq[0];
            outputBuffer->hasTransferKey = true;
            break;
        case 'd':
            outputBuffer->deleteMode = eq[0];
            break;
        case 'f':
            outputBuffer->format = n;
            outputBuffer->hasTransferKey = true;
            break;
        case 'S':
            outputBuffer->payloadSize = (n > 0) ? n : 0;
            outputBuffer->hasTransferKey = true;
            break;
        case 's':
            outputBuffer->width = n;
            outputBuffer->hasTransferKey = true;
            break;
        case 'v':
            outputBuffer->height = n;
            outputBuffer->hasTransferKey = true;
            break;
        case 'i':
            if (n >= 0) {
                outputBuffer->imageID = static_cast<uint32_t>(n);
                outputBuffer->hasImageID = true;
            }
            break;
        case 'I':
            if (n >= 0) {
                outputBuffer->imageNumber = static_cast<uint32_t>(n);
                outputBuffer->hasImageNumber = true;
            }
            break;
        case 'p':
            if (n >= 0) {
                outputBuffer->placementID = static_cast<uint32_t>(n);
                outputBuffer->hasPlacementID = true;
            }
            break;
        case 'q':
            outputBuffer->quiet = (n < 0) ? 1 : n;
            break;
        case 'm':
            outputBuffer->more = n;
            break;
        case 'o':
            outputBuffer->compression = eq[0];
            outputBuffer->hasTransferKey = true;
            break;
        case 'c':
            if (n >= 0)
                outputBuffer->cellColumns = n;
            break;
        case 'r':
            if (n >= 0)
                outputBuffer->cellRows = n;
            break;
        case 'X':
            if (n >= 0)
                outputBuffer->pixelOffsetX = n;
            break;
        case 'Y':
            if (n >= 0)
                outputBuffer->pixelOffsetY = n;
            break;
        case 'x':
            if (n >= 0)
                outputBuffer->sourceX = n;
            break;
        case 'y':
            if (n >= 0)
                outputBuffer->sourceY = n;
            break;
        case 'w':
            if (n >= 0)
                outputBuffer->sourceWidth = n;
            break;
        case 'h':
            if (n >= 0)
                outputBuffer->sourceHeight = n;
            break;
        case 'z': {
            // Signed: under-text layering uses negative z.
            int32_t zv;
            if (kittyParseSInt(eq, &zv)) {
                outputBuffer->zIndex = zv;
                outputBuffer->hasZIndex = true;
            }
            break;
        }
        case 'C':
            if (n >= 0)
                outputBuffer->cursorPolicy = n;
            break;
        case 'U':
            if (n >= 0)
                outputBuffer->unicodePlacement = n;
            break;
        default:
            break;
        }
    }
}

// `status` is a view because every caller hands over a literal, whose length the
// compiler already knows; a `const char*` would make the OK test measure the
// whole string to read its first two bytes.
static void kittyRespond(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, std::string_view status)
{
    char buffer[96];
    int n;
    // q (quiet): 0 sends OK + errors, 1 suppresses only ok (errors still sent),
    // 2 suppresses everything.
    if (c && c->quiet >= 2)
        return;
    // Named images request acknowledgement. The protocol's "Querying support
    // and available transmission mediums" also requires replies to query
    // actions, including when an image identifier is inappropriate.
    if (c && !c->hasImageID && !c->hasImageNumber && c->action != 'q')
        return;
    if (c && c->quiet == 1 && status.starts_with("OK"))
        return;
    const int statusLength = static_cast<int>(status.size());
    if (c && c->hasImageID && c->hasPlacementID)
        n = snprintf(buffer, sizeof(buffer), "\033_Gi=%lu,p=%lu;%.*s\033\\", static_cast<unsigned long>(c->imageID), static_cast<unsigned long>(c->placementID), statusLength, status.data());
    else if (c && c->hasImageID)
        n = snprintf(buffer, sizeof(buffer), "\033_Gi=%lu;%.*s\033\\", static_cast<unsigned long>(c->imageID), statusLength, status.data());
    else
        n = snprintf(buffer, sizeof(buffer), "\033_G;%.*s\033\\", statusLength, status.data());
    if (n > 0 && n < static_cast<int>(sizeof(buffer)))
        // Protocol status ACK carrying the outcome of the operation only. For a
        // file transport that outcome does describe a host file — whether it
        // opened, whether its size matched the geometry, whether it decoded —
        // which is why the frontend gate reads only temporary directories. The
        // ACK rides Capability::None — the Benign tier every shipped policy's
        // defaultDecision resolves to Allow. That is a property of the policies,
        // not of the gate: respond() consults the policy for every reply
        // (PAL/Capability.h: never a check()-bypass sentinel), and a policy that
        // denies None silences this ACK too — the client is refused without being
        // told, which is the maximally-closed posture, expressible on purpose.
        // Both sides are pinned in test_kitty_graphics_refusals.
        Ubiquitty::respond(t, Ubiquitty::Capability::None, buffer);
}

static Image::TerminalImage* kittyFindImage(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c)
{
    int i;
    for (i = 0; i < Image::maxTerminalImageCount; i++) {
        Image::TerminalImage* image = &t.images.stored[i];
        if (!image->used)
            continue;
        if (c->hasImageID && image->imageID == c->imageID)
            return image;
        if (!c->hasImageID && c->hasImageNumber && image->imageNumber == c->imageNumber)
            return image;
    }
    // A placement ID alone names the image that placement draws.
    if (!c->hasImageID && !c->hasImageNumber && c->hasPlacementID) {
        for (const Image::TerminalImagePlacement& placement : t.images.placements) {
            if (!placement.used || !placement.hasPlacementID || placement.placementID != c->placementID)
                continue;
            for (Image::TerminalImage& image : t.images.stored) {
                if (image.used && image.imageID == placement.imageID)
                    return &image;
            }
        }
    }
    return nullptr;
}

// True when the payload begins with the 8-byte PNG signature.
static bool kittyIsPNGSignature(std::span<const unsigned char> bytes)
{
    return Codec::PNG::hasSignature(bytes);
}

// Decode a non-PNG container (HEIF / AVIF / WebP / APNG / …) via the PAL image
// decoder. Returns RGBA8 (format 32) in a malloc'd buffer. No PAL decoder →
// false, so kitty f=100 stays PNG-only there. Animated formats decode frame 0.
static std::optional<DecodedImage> kittyDecodeViaPlatform(std::span<const unsigned char> bytes)
{
    Ubiquitty::PAL::ImageDecodeCallback decode = Ubiquitty::PAL::imageDecoder();
    if (!decode)
        return std::nullopt;
    std::uint8_t* rgba = nullptr;
    int width = 0;
    int height = 0;
    if (!decode(bytes, &rgba, &width, &height))
        return std::nullopt;
    if (width <= 0 || height <= 0 || !rgba) {
        std::free(rgba);
        return std::nullopt;
    }
    // Enforce the same pixel-byte cap the PNG path applies.
    std::uint64_t pixelLength = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4ULL;
    if (pixelLength == 0 || pixelLength > Ubiquitty::Image::maxDecodedImageBytes) {
        std::free(rgba);
        return std::nullopt;
    }
    return DecodedImage { KittyBytes(rgba), static_cast<int>(pixelLength), width, height, 32 };
}

// Decode pre-decompressed PNG bytes into an RGB/RGBA pixel buffer. Non-PNG
// f=100 payloads route through the PAL decoder.
static std::optional<DecodedImage> kittyDecodePNGBytes(std::span<const unsigned char> pngBytes)
{
    int scratchCapacity;
    KittyBytes scratch;
    KittyBytes pixels;
    Ubiquitty::Codec::PNG::DecodeResult res;

    if (pngBytes.empty())
        return std::nullopt;
    // Non-PNG container: hand to the platform decoder (ImageIO on macOS).
    if (!kittyIsPNGSignature(pngBytes))
        return kittyDecodeViaPlatform(pngBytes);
    // Scratch holds the raw inflated scanlines (each row = 1 filter byte +
    // width*bpp); *2 + margin covers any image within the pixel-count cap.
    scratchCapacity = static_cast<int>(Ubiquitty::Image::maxDecodedImageBytes * 2UL + 4096UL);
    scratch = KittyBytes(static_cast<unsigned char*>(malloc(static_cast<size_t>(scratchCapacity))));
    if (!scratch)
        return std::nullopt;
    Ubiquitty::Codec::Zlib::Inflater inflater; // per-call stack state (~33 KB)
    res = Ubiquitty::Codec::PNG::decode({ pngBytes.data(), pngBytes.size() }, { scratch.get(), static_cast<std::size_t>(scratchCapacity) }, inflater);
    if (res.status != Ubiquitty::Codec::PNG::DecodeStatus::Success || res.pixels.empty())
        return std::nullopt;
    if (res.pixels.size() > Ubiquitty::Image::maxDecodedImageBytes)
        return std::nullopt;
    pixels = KittyBytes(static_cast<unsigned char*>(malloc(res.pixels.size())));
    if (!pixels)
        return std::nullopt;
    std::memcpy(pixels.get(), res.pixels.data(), res.pixels.size());
    return DecodedImage { std::move(pixels), static_cast<int>(res.pixels.size()), res.width, res.height, (res.colorType == 2) ? 24 : 32 };
}

// The result of an inflate: owned bytes and their length.
struct InflateResult {
    KittyBytes bytes;
    int length;
};

// Optionally zlib-inflate `in[0..inputLength)` into a fresh buffer.
// If `compression` is 0 the caller's bytes are copied as-is.
static std::expected<InflateResult, const char*> kittyMaybeInflate(char compression, std::span<const unsigned char> in, int maxLength)
{
    const int inputLength = static_cast<int>(in.size());
    if (maxLength <= 0)
        return std::unexpected("EINVAL:invalid max length");
    if (compression == 0) {
        if (inputLength > maxLength)
            return std::unexpected("EINVAL:payload too large");
        KittyBytes copy(static_cast<unsigned char*>(malloc(static_cast<size_t>(inputLength > 0 ? inputLength : 1))));
        if (!copy)
            return std::unexpected("ENOMEM:allocation failed");
        if (inputLength > 0)
            std::memcpy(copy.get(), in.data(), static_cast<size_t>(inputLength));
        return InflateResult { std::move(copy), inputLength };
    }
    if (compression == 'z') {
        KittyBytes inflated(static_cast<unsigned char*>(malloc(static_cast<size_t>(maxLength))));
        if (!inflated)
            return std::unexpected("ENOMEM:allocation failed");
        Ubiquitty::Codec::Zlib::Inflater inflater; // stack-owned per-call state (~33 KB)
        const Ubiquitty::Codec::Zlib::Inflater::DecodeResult result = inflater.decode({ in.data(), in.size() }, { inflated.get(), static_cast<std::size_t>(maxLength) });
        if (result.status != Ubiquitty::Codec::Zlib::InflateStatus::Done)
            return std::unexpected("EINVAL:zlib inflate failed");
        return InflateResult { std::move(inflated), static_cast<int>(result.bytesProduced) };
    }
    return std::unexpected("ENOTSUP:unsupported compression");
}

// Decode the APC payload envelope: base64-decode, then inflate if `o=z`. Returns
// owned bytes on success, or an error string on failure. `maxLength` caps the
// post-inflate size (defence against decompression bombs).
//
// Uncompressed, the decoded buffer is the result: it is already exactly the
// payload's size, and a copy would hold the image twice at the peak.
static std::expected<ExtractedBytes, const char*> kittyExtractPayload(char compression, std::span<const char> payload, int maxLength)
{
    if (payload.empty() || maxLength <= 0)
        return std::unexpected("EINVAL:invalid payload");
    auto decoded = kittyDecodePayload(payload);
    if (!decoded)
        return std::unexpected(decoded.error() == PayloadDecodeFailure::Allocation ? "ENOMEM:allocation failed" : "EINVAL:invalid payload");
    if (compression == 0) {
        if (decoded->length > maxLength)
            return std::unexpected("EINVAL:payload too large");
        return std::move(*decoded);
    }
    auto result = kittyMaybeInflate(compression, { decoded->bytes.get(), static_cast<std::size_t>(decoded->length) }, maxLength);
    if (!result)
        return std::unexpected(result.error());
    return ExtractedBytes { std::move(result->bytes), result->length };
}

// How many bytes the file may legitimately hold, which is not the size of what
// it decodes to.
//
// Uncompressed, one byte beyond the cap. A buffer sized to the cap exactly makes
// every file at least that long come back at exactly the cap, so the size check
// downstream compares a value the read was forced to produce and agrees with a
// file it never saw the end of — the image installs cropped, skewed by the
// difference from the second row on. The extra byte is what makes "longer than
// the geometry" reach that check as a different number.
//
// Compressed, deflate's own worst case. A stream with no redundancy comes out
// larger than it went in: the zlib header and adler tail, plus five bytes per
// stored block. Sizing that read to the inflated length truncates a valid file,
// and a truncated deflate stream is an inflate failure — so the transfer is
// refused for a reason about the buffer rather than about the image.
static int kittyFileReadCapacity(char compression, int maxLength)
{
    if (compression == 0)
        return maxLength + 1;
    constexpr int storedBlockLength = 65535;
    constexpr int storedBlockOverhead = 5;
    constexpr int streamOverhead = 6; // 2-byte header, 4-byte adler
    return maxLength + streamOverhead + (maxLength / storedBlockLength + 1) * storedBlockOverhead;
}

// A file-transport path as an absolute name with `.`, `..` and empty
// components resolved on the string, or nullopt when it is relative, holds a
// NUL, or climbs above the root.
//
// No root but `/`. Which directories may be read is the frontend gate's
// question, asked of the descriptor it opens, and a working directory is not
// an answer Core can give: every directory a pane knows is either program
// output (OSC 7, 633, 1337), which a remote host writes as easily as a local
// shell, or the foreground's kernel cwd, which under ssh is the local ssh
// client's. A relative path is refused for the same reason. It would have to
// be resolved against one of them, and `.ssh/id_ed25519` needs no username to
// aim at a home directory.
//
// Portable and frontend-independent: no filesystem access (Core performs no
// syscalls). Symlinks stay the descriptor gate's job; this rules out the `..`
// that needs none, for every embedder, including one whose FileReadClient
// reads plain paths.
static std::optional<std::string> kittyNormalizeAbsolutePath(std::string_view path)
{
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string_view::npos)
        return std::nullopt;
    std::vector<std::string_view> parts;
    for (std::string_view rest = path.substr(1);;) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part == "..") {
            if (parts.empty())
                return std::nullopt;
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }
    std::string normal;
    for (const std::string_view part : parts) {
        normal += '/';
        normal += part;
    }
    if (normal.empty())
        normal = "/";
    return normal;
}

// The name the kitty protocol tells a client to give a t=t file, and a terminal
// to require before deleting one. Without it a t=t is a request to delete
// whatever file the sender names in /tmp.
constexpr std::string_view kittyTemporaryFileMarker = "tty-graphics-protocol";

// Resolve an APC payload through the file transport (t=f / t=t). The payload is
// base64(path); the file's contents are the (possibly zlib) image bytes. Returns
// owned bytes on success, or an error string on failure.
static std::expected<ExtractedBytes, const char*> kittyExtractFromFile(Ubiquitty::Terminal* t, const Ubiquitty::Kitty::GraphicsControl* c, TempFileUnlink tempFileUnlink, std::span<const char> payload, int maxLength)
{
    if (!t)
        return std::unexpected(fileTransportFailed);
    auto* fileRead = t->fileReadClient.get();
    if (!fileRead)
        return std::unexpected(fileTransportFailed);
    if (payload.empty() || maxLength <= 0)
        return std::unexpected(fileTransportFailed);
    // Exactly the decoded path, with no terminator: it is read only as a view,
    // and the reader is handed the normalized copy. No room for a byte is no
    // path, refused before an allocation.
    const std::string_view encodedPath(payload.data(), payload.size());
    const std::size_t pathCapacity = Ubiquitty::Codec::base64DecodedCapacity(encodedPath);
    if (!pathCapacity)
        return std::unexpected(fileTransportFailed);
    OwnedCharBuffer path(static_cast<char*>(std::malloc(pathCapacity)));
    if (!path)
        return std::unexpected(fileTransportFailed);
    const auto decodedPath = Ubiquitty::Codec::base64Decode(encodedPath, { path.get(), pathCapacity });
    if (!decodedPath || decodedPath->empty())
        return std::unexpected(fileTransportFailed);
    // Lexical checks first: a relative path, or a `..` past the root, never
    // reaches the policy detail or the reader, on any frontend — including one
    // whose FileReadClient reads plain paths. The descriptor gate still
    // adjudicates where the file is; this rules out what needs no filesystem
    // to refuse. EPERM, which is what a client re-transmits after.
    const std::optional<std::string> normal = kittyNormalizeAbsolutePath(
        std::string_view(decodedPath->data(), decodedPath->size()));
    if (!normal)
        return std::unexpected("EPERM:outside confinement");
    // A t=t without the marker is refused unread, whether it is transmitted or
    // only asked about, so a probe answers what the transfer would.
    if (c->transport == 't' && normal->find(kittyTemporaryFileMarker) == std::string::npos)
        return std::unexpected("EPERM:outside confinement");
    if (!Ubiquitty::effectAllowed(*t, Ubiquitty::Capability::FileReadAccess, *normal))
        return std::unexpected("EPERM:capability denied");
    const int fileCapacity = kittyFileReadCapacity(c->compression, maxLength);
    KittyBytes fileBuffer(static_cast<unsigned char*>(malloc(static_cast<size_t>(fileCapacity))));
    if (!fileBuffer)
        return std::unexpected(fileTransportFailed);
    const int fileLength = fileRead->readFile(normal->c_str(), tempFileUnlink == TempFileUnlink::Consume, fileBuffer.get(), fileCapacity);
    if (fileLength < 0)
        return std::unexpected(fileTransportFailed);
    auto result = kittyMaybeInflate(c->compression, std::span<const unsigned char>(fileBuffer.get(), static_cast<size_t>(fileLength)), maxLength);
    if (!result)
        return std::unexpected(result.error());
    return ExtractedBytes { std::move(result->bytes), result->length };
}

// Kitty t=s shm object names are capped at the strictest platform limit (macOS
// PSHMNAMLEN = 31 bytes incl. the leading '/'), enforced Core-side.
constexpr int kittySharedMemoryNameMaximumLength = 31;

// A valid kitty t=s object name: leading '/', 1..30 further bytes, no second '/'
// (the POSIX shm namespace is flat — a second '/' is invalid or an escape),
// printable ASCII only.
static bool kittySharedMemoryNameValid(std::string_view name)
{
    if (name.size() < 2 || name.size() > kittySharedMemoryNameMaximumLength)
        return false;
    if (name[0] != '/')
        return false;
    for (std::size_t i = 1; i < name.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(name[i]);
        if (byte == '/' || byte < 0x21 || byte > 0x7E)
            return false;
    }
    return true;
}

// Resolve an APC payload through the shared-memory transport (t=s). The payload
// is base64(name) of a POSIX shared-memory object. Name validation + the
// capability gate run Core-side; the host client re-gates (defense in depth).
static std::expected<ExtractedBytes, const char*> kittyExtractFromSharedMemory(Ubiquitty::Terminal* t, const Ubiquitty::Kitty::GraphicsControl* c, TempFileUnlink tempFileUnlink, std::span<const char> payload, int maxLength)
{
    if (!t)
        return std::unexpected(sharedMemoryTransportFailed);
    auto* fileRead = t->fileReadClient.get();
    if (!fileRead)
        return std::unexpected(sharedMemoryTransportFailed);
    if (payload.empty() || maxLength <= 0)
        return std::unexpected(sharedMemoryTransportFailed);
    char name[kittySharedMemoryNameMaximumLength + 1] = { 0 };
    const auto decodedName = Ubiquitty::Codec::base64Decode({ payload.data(), payload.size() }, std::span(name).first(sizeof(name) - 1));
    if (!decodedName || decodedName->empty())
        return std::unexpected("EINVAL:invalid shared memory name");
    if (!kittySharedMemoryNameValid({ decodedName->data(), decodedName->size() }))
        return std::unexpected("EINVAL:invalid shared memory name");
    if (!Ubiquitty::effectAllowed(*t, Ubiquitty::Capability::FileReadAccess, name))
        return std::unexpected("EPERM:capability denied");
    const int bufferCapacity = (c->payloadSize > 0 && c->payloadSize < maxLength) ? c->payloadSize : maxLength;
    KittyBytes buffer(static_cast<unsigned char*>(malloc(static_cast<size_t>(bufferCapacity))));
    if (!buffer)
        return std::unexpected(sharedMemoryTransportFailed);
    const int bytesRead = fileRead->readSharedMemory(name, tempFileUnlink == TempFileUnlink::Consume, buffer.get(), bufferCapacity);
    if (bytesRead < 0)
        return std::unexpected("EBADF:shared memory read failed");
    auto result = kittyMaybeInflate(c->compression, std::span<const unsigned char>(buffer.get(), static_cast<size_t>(bytesRead)), maxLength);
    if (!result)
        return std::unexpected(result.error());
    return ExtractedBytes { std::move(result->bytes), result->length };
}

// Dispatch the APC payload to the correct extraction strategy based on
// the transport key. Sets *outError to a kitty-protocol error string on
// failure; on success leaves *outError untouched and returns the bytes.
static std::expected<ExtractedBytes, const char*> kittyResolveTransport(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, std::span<const char> payload, int maxLength, TempFileUnlink tempFileUnlink)
{
    if (c->transport == 'd' || c->transport == 0)
        return kittyExtractPayload(c->compression, payload, maxLength);
    if (c->transport == 'f' || c->transport == 't') {
        // A FileReadClient declares the file transport supported, and t=t
        // separately: see FileReadClient::supportsTemporaryFileTransport.
        if (!t.fileReadClient)
            return std::unexpected("ENOTSUP:file transport unavailable");
        if (c->transport == 't' && !t.fileReadClient->supportsTemporaryFileTransport())
            return std::unexpected("ENOTSUP:temporary file transport unavailable");
        return kittyExtractFromFile(&t, c, (c->transport == 't' && tempFileUnlink == TempFileUnlink::Consume) ? TempFileUnlink::Consume : TempFileUnlink::Preserve, payload, maxLength);
    }
    if (c->transport == 's') {
        // t=s needs a FileReadClient declaring shared-memory support; else ENOTSUP.
        if (!t.fileReadClient || !t.fileReadClient->supportsSharedMemoryTransport())
            return std::unexpected("ENOTSUP:shared memory unavailable");
        return kittyExtractFromSharedMemory(&t, c, tempFileUnlink, payload, maxLength);
    }
    return std::unexpected("ENOTSUP:unsupported transport");
}

static bool kittyStoreDirectImage(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, std::span<const char> payload, TransmitDisplay transmitDisplay)
{
    uint32_t bytesPerPixel;
    uint32_t expected;
    unsigned char* pixels;
    Image::TerminalImage* image;
    int pngWidth = 0;
    int pngHeight = 0;
    int pngFormat = 0;
    int pngPixelLength = 0;
    if (c->format == 100) {
        int maxPNGBytes = static_cast<int>(Ubiquitty::Image::maxDecodedImageBytes * 2UL + 4096UL);
        // Transport resolution stays on the parse thread — t=f touches the fs.
        auto extracted = kittyResolveTransport(t, c, payload, maxPNGBytes, TempFileUnlink::Consume);
        if (!extracted) {
            kittyRespond(t, c, extracted.error());
            return false;
        }
        KittyBytes pngBytes = std::move(extracted->bytes); // auto-frees on any early return below
        const int pngLength = extracted->length;
        // Submit the PNG decode to the TaskQueue (worker runs inline at submit;
        // next dispatch drain installs). Ownership of pngBytes transfers to the job.
        auto job = std::make_unique<KittyPNGDecodeJob>();
        job->term = &t;
        job->terminalGeneration = t.generation;
        job->ctrl = *c;
        job->transmitDisplay = transmitDisplay;
        job->pngBytes = std::move(pngBytes);
        job->pngLength = pngLength;
        Ubiquitty::TaskQueue* q = kittyEnsureDecodeQueue(t);
        if (!q) {
            kittyRespond(t, c, "ENOMEM:dispatch"); // job (+ pngBytes) auto-freed
            return false;
        }
        // Hand ownership to the TaskQueue (completion deletes it); reclaim on a
        // submit failure.
        KittyPNGDecodeJob* raw = job.release();
        if (!q->submit(kittyPNGDecodeWorker, raw)) {
            delete raw;
            kittyRespond(t, c, "ENOMEM:dispatch");
            return false;
        }
        // Suppress -Wunused-variable for the locals the async path leaves unfilled.
        (void)image;
        (void)pixels;
        (void)pngPixelLength;
        (void)pngWidth;
        (void)pngHeight;
        (void)pngFormat;
        return true;
    }
    if (!(c->format == 24 || c->format == 32) || c->width <= 0 || c->height <= 0) {
        kittyRespond(t, c, "EINVAL:missing image geometry");
        return false;
    }
    // The stored image carries its dimensions in sixteen bits, so a dimension
    // past that range is refused where the sender gets an answer — a cast at
    // the store would wrap it into a small image whose pixel length disagrees
    // with its geometry.
    if (c->width > 0xFFFF || c->height > 0xFFFF) {
        kittyRespond(t, c, "EINVAL:image too large");
        return false;
    }
    bytesPerPixel = (c->format == 32) ? 4UL : 3UL;
    if (static_cast<uint32_t>(c->width) > Ubiquitty::Image::maxDecodedImageBytes / bytesPerPixel || static_cast<uint32_t>(c->height) > Ubiquitty::Image::maxDecodedImageBytes / (static_cast<uint32_t>(c->width) * bytesPerPixel)) {
        kittyRespond(t, c, "EINVAL:image too large");
        return false;
    }
    expected = static_cast<uint32_t>(c->width) * static_cast<uint32_t>(c->height) * bytesPerPixel;
    // Direct-pixel + zlib + base64-direct transport takes the TaskQueue path.
    // Other transports (t=f) keep the direct path because the file read touches
    // the fs on the parse thread.
    if (c->compression == 'z' && (c->transport == 'd' || c->transport == 0)) {
        auto compressed = kittyDecodePayload(payload);
        if (!compressed) {
            kittyRespond(t, c, compressed.error() == PayloadDecodeFailure::Allocation ? "ENOMEM:b64" : "EINVAL:bad payload");
            return false;
        }
        auto job = std::make_unique<KittyDirectInflateJob>();
        job->term = &t;
        job->terminalGeneration = t.generation;
        job->ctrl = *c;
        job->transmitDisplay = transmitDisplay;
        job->compressed = std::move(compressed->bytes);
        job->compressedLength = compressed->length;
        job->expectedLength = static_cast<int>(expected);
        Ubiquitty::TaskQueue* q = kittyEnsureInflateQueue(t);
        if (!q) {
            kittyRespond(t, c, "ENOMEM:dispatch"); // job (+ its buffers) auto-freed
            return false;
        }
        // Hand ownership to the TaskQueue (completion deletes it); reclaim on a
        // submit failure.
        KittyDirectInflateJob* raw = job.release();
        if (!q->submit(kittyDirectInflateWorker, raw)) {
            delete raw;
            kittyRespond(t, c, "ENOMEM:dispatch");
            return false;
        }
        (void)image;
        (void)pixels;
        return true;
    }
    {
        auto extracted = kittyResolveTransport(t, c, payload, static_cast<int>(expected), TempFileUnlink::Consume);
        if (!extracted) {
            kittyRespond(t, c, extracted.error());
            return false;
        }
        KittyBytes owned = std::move(extracted->bytes);
        if (extracted->length != static_cast<int>(expected)) {
            kittyRespond(t, c, "EINVAL:payload size mismatch");
            return false;
        }
        // The image adopts the buffer; releasing here is what makes that a
        // transfer rather than a pointer into memory this scope frees.
        pixels = owned.release();
    }
    image = kittyFindImage(t, c);
    if (!image)
        image = Image::allocateSlot(t);
    else {
        Image::removePlacements(t, image->imageID, Image::PlacementMatchMode::RemoveAll, 0);
        Image::dispose(image);
    }
    if (!image) {
        // Every slot holds an image that is on screen, so the store has
        // nothing to give without taking a picture out from under a reader.
        // The two asynchronous transfer paths answer the same way, and the
        // buffer this scope already owns is freed rather than adopted.
        std::free(pixels);
        kittyRespond(t, c, "ENOSPC:no image slot");
        return false;
    }
    image->used = true;
    image->displayed = false; // recomputed by syncMirror
    image->imageID = c->hasImageID ? c->imageID : Image::allocateImageID(t);
    image->imageNumber = c->hasImageNumber ? c->imageNumber : 0;
    image->width = static_cast<uint16_t>(c->width);
    image->height = static_cast<uint16_t>(c->height);
    image->format = static_cast<uint16_t>(c->format);
    image->pixelLength = expected;
    image->pixels = pixels;
    image->contentGeneration = ++t.images.kittyImageContentSeq;
    Image::initFrameZero(image);
    Image::enforceBudget(&t, image);
    if (transmitDisplay == TransmitDisplay::TransmitAndDisplay && c->unicodePlacement == 1) {
        kittyApplyVirtualPlacement(image, c);
    } else if (transmitDisplay == TransmitDisplay::TransmitAndDisplay) {
        Image::TerminalImagePlacement* p = Image::allocatePlacement(t);
        if (p)
            kittyFillPlacement(p, t, c, image->imageID);
        kittyAdvanceCursorAfterImage(t, c, image);
    }
    Image::syncMirror(t, image);
    Image::markDirty(t, image);
    kittyRespond(t, c, "OK");
    return true;
}

// Every delete mode selects a set of placements and then answers one further
// question: whether the image data goes with them. The protocol settles that by
// case — an upper-case mode "will delete the image data as well, provided that
// the image is not referenced elsewhere" — so the selector and the disposal are
// separate decisions and only the selector differs between modes.
static bool kittyDeleteFreesImageData(char deleteMode)
{
    return deleteMode >= 'A' && deleteMode <= 'Z';
}

// Does one placement fall under a geometry selector? `x` and `y` are CELL
// coordinates here; the same keys carry the source rectangle's origin on a
// placement command, and the action decides which meaning applies.
//
// They are ONE-BASED, as cursor positions are — the protocol says x=1,y=1 is the
// top left cell — while a placement's anchor is stored from zero. Comparing them
// without the shift deletes the neighbouring row and column, which is a
// difference no reply reports.
static bool kittyExtentContains(int cell, int start, int extent)
{
    return cell >= start && cell < start + extent;
}

static bool kittyPlacementSelected(const Ubiquitty::Terminal& t, const Image::TerminalImagePlacement& p, const Ubiquitty::Kitty::GraphicsControl* c)
{
    const int column = static_cast<int>(p.cursorX);
    const int row = static_cast<int>(p.cursorY);
    // A placement with no stated cell extent occupies the one cell it is
    // anchored at, so the containment test has a width and a height either way.
    const int columns = (p.cellColumns > 0) ? static_cast<int>(p.cellColumns) : 1;
    const int rows = (p.cellRows > 0) ? static_cast<int>(p.cellRows) : 1;
    const bool isOverColumn = kittyExtentContains(c->sourceX - 1, column, columns);
    const bool isOverRow = kittyExtentContains(c->sourceY - 1, row, rows);
    const bool matchesLayer = c->hasZIndex && p.zIndex == c->zIndex;

    switch (c->deleteMode) {
    case 'c':
    case 'C':
        return kittyExtentContains(t.screens.active->cursorX, column, columns)
            && kittyExtentContains(t.screens.active->cursorY, row, rows);
    case 'p':
    case 'P':
        return isOverColumn && isOverRow;
    case 'q':
    case 'Q':
        // The cell selector narrowed by a layer, so a placement at the cell on
        // another layer stays. Without a z the narrowing is absent rather than
        // matching every layer.
        return isOverColumn && isOverRow && (!c->hasZIndex || matchesLayer);
    case 'x':
    case 'X':
        return isOverColumn;
    case 'y':
    case 'Y':
        return isOverRow;
    case 'z':
    case 'Z':
        return matchesLayer;
    default:
        return false;
    }
}

// Removes every placement the selector picks, then disposes the images left
// unreferenced when the mode's case asks for it.
// Frees the images the selector touched, and only those: an upper-case mode
// takes the data of what it deleted, not of every image the terminal happens to
// be holding without a placement. A client that transmitted an image ahead of
// placing it would otherwise lose it to somebody else's delete.
static void kittyDisposeTouchedIfUnreferenced(Ubiquitty::Terminal& t, std::span<const uint32_t> touched)
{
    for (const uint32_t imageID : touched) {
        if (Image::hasPlacement(t, imageID))
            continue;
        for (Image::TerminalImage& stored : t.images.stored) {
            if (stored.used && stored.imageID == imageID) {
                Image::dispose(&stored);
                break;
            }
        }
    }
}

static void kittyDeleteSelectedPlacements(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c)
{
    std::array<uint32_t, Image::maxTerminalImagePlacementCount> touched {};
    std::size_t touchedCount = 0;
    int removed = 0;
    for (int i = 0; i < Image::maxTerminalImagePlacementCount; ++i) {
        Image::TerminalImagePlacement& p = t.images.placements[i];
        if (!p.used || !kittyPlacementSelected(t, p, c))
            continue;
        for (int k = 0; k < Image::maxTerminalImageCount; ++k) {
            if (t.images.stored[k].used && t.images.stored[k].imageID == p.imageID) {
                Image::markDirty(t, &t.images.stored[k]);
                break;
            }
        }
        touched[touchedCount++] = p.imageID;
        p = {};
        ++removed;
    }
    // Every slot freed here was a live one, and the count gates the scroll,
    // capture and reflow walks: left high, they run over empty slots forever.
    t.images.livePlacementCount -= static_cast<uint32_t>(removed);
    if (removed > 0)
        Ubiquitty::Screen::markAllDirty(t);
    if (kittyDeleteFreesImageData(c->deleteMode))
        kittyDisposeTouchedIfUnreferenced(t, std::span<const uint32_t>(touched.data(), touchedCount));
    Image::syncAllMirrors(t);
    kittyRespond(t, c, "OK");
}

static void kittyDelete(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c)
{
    Image::TerminalImage* image;
    switch (c->deleteMode) {
    case 'c':
    case 'C':
    case 'p':
    case 'P':
    case 'q':
    case 'Q':
    case 'x':
    case 'X':
    case 'y':
    case 'Y':
    case 'z':
    case 'Z':
        kittyDeleteSelectedPlacements(t, c);
        return;
    case 'r':
    case 'R': {
        // An inclusive id range, which `x` and `y` carry for this mode alone.
        const uint32_t first = static_cast<uint32_t>(c->sourceX);
        const uint32_t last = (c->sourceY > 0) ? static_cast<uint32_t>(c->sourceY) : first;
        std::array<uint32_t, Image::maxTerminalImageCount> touched {};
        std::size_t touchedCount = 0;
        int removed = 0;
        for (int i = 0; i < Image::maxTerminalImageCount; ++i) {
            Image::TerminalImage& stored = t.images.stored[i];
            if (!stored.used || stored.imageID < first || stored.imageID > last)
                continue;
            Image::markDirty(t, &stored);
            touched[touchedCount++] = stored.imageID;
            removed += Image::removePlacements(t, stored.imageID, Image::PlacementMatchMode::RemoveAll, 0);
        }
        if (removed > 0)
            Ubiquitty::Screen::markAllDirty(t);
        if (kittyDeleteFreesImageData(c->deleteMode))
            kittyDisposeTouchedIfUnreferenced(t, std::span<const uint32_t>(touched.data(), touchedCount));
        Image::syncAllMirrors(t);
        kittyRespond(t, c, "OK");
        return;
    }
    case 'f':
    case 'F': {
        // Animation frames belong to one image, so the selector is the ordinary
        // id/number lookup and only the frames go.
        Image::TerminalImage* target = kittyFindImage(t, c);
        if (!target) {
            kittyRespond(t, c, "ENOENT:image not found");
            return;
        }
        // Frame zero aliases the image's own pixel buffer, so it is the image
        // rather than a frame and freeing it here would leave the slot pointing
        // at released memory. Only the frames added after it own anything.
        for (int f = 1; f < Image::maxTerminalImageFrameCount; ++f) {
            std::free(target->frames[f].pixels);
            target->frames[f].pixels = nullptr;
            target->frames[f].pixelLength = 0;
            target->frames[f].used = false;
            target->frames[f].gapless = false;
            target->frames[f].delayMilliseconds = 0;
        }
        target->frameCount = 1;
        target->currentFrame = 0;
        Image::markDirty(t, target);
        Image::syncMirror(t, target);
        kittyRespond(t, c, "OK");
        return;
    }
    default:
        break;
    }
    if (c->deleteMode == 0 || c->deleteMode == 'a' || c->deleteMode == 'A') {
        int removed = Image::removeAllPlacements(t);
        if (removed > 0)
            Ubiquitty::Screen::markAllDirty(t);
        if (c->deleteMode == 'A')
            Image::disposeUnreferenced(t);
        else
            Image::syncAllMirrors(t);
        kittyRespond(t, c, "OK");
        return;
    }
    // `n`/`N` selects by image NUMBER rather than by id, and the protocol says
    // the newest image carrying that number — a program that never learned the
    // id it was assigned addresses its image this way.
    if (c->deleteMode == 'n' || c->deleteMode == 'N') {
        image = nullptr;
        for (int i = 0; i < Image::maxTerminalImageCount; ++i) {
            Image::TerminalImage& stored = t.images.stored[i];
            if (!stored.used || stored.imageNumber != c->imageNumber || c->imageNumber == 0)
                continue;
            if (!image || stored.imageID > image->imageID)
                image = &stored;
        }
    } else {
        image = kittyFindImage(t, c);
    }
    if (!image) {
        kittyRespond(t, c, "ENOENT:image not found");
        return;
    }
    Image::markDirty(t, image);
    // With a placement id, only that placement goes (the image stays alive for
    // other placements). Uppercase delete modes free image data once unreferenced.
    Image::removePlacements(t, image->imageID, c->hasPlacementID ? Image::PlacementMatchMode::MatchID : Image::PlacementMatchMode::RemoveAll, c->placementID);
    Image::syncMirror(t, image);
    if (kittyDeleteFreesImageData(c->deleteMode) && !Image::hasPlacement(t, image->imageID))
        Image::dispose(image);
    kittyRespond(t, c, "OK");
}

// a=q (query support). A query carries a full transmit command and ATTEMPTS the
// load (honoring f/t/o/s/v and the payload), replying OK on a successful decode
// or the SPECIFIC error the transmit path would produce, but without storing the
// image or unlinking a t=t temp file. The side-effect-free capability probe.
static void kittyQuery(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, std::span<const char> payload)
{
    // A degenerate query with no transmit content (no payload and default direct
    // transport) is only an id-existence probe; keep the id lookup.
    const bool hasTransmitContent = !payload.empty() || c->transport == 'f' || c->transport == 't' || c->transport == 's';
    if (!hasTransmitContent) {
        kittyRespond(t, c, kittyFindImage(t, c) ? "OK" : "ENOENT:image not found");
        return;
    }

    // Route through the same load pipeline as a transmit, never unlinking and never
    // storing. FileReadAccess still gates t=f/t=t (in kittyResolveTransport).
    if (c->format == 100) {
        int maxPNGBytes = static_cast<int>(Ubiquitty::Image::maxDecodedImageBytes * 2UL + 4096UL);
        auto extracted = kittyResolveTransport(t, c, payload, maxPNGBytes, TempFileUnlink::Preserve);
        if (!extracted) {
            kittyRespond(t, c, extracted.error());
            return;
        }
        KittyBytes pngBytes = std::move(extracted->bytes); // discard after validation
        const int pngLength = extracted->length;
        // Decoded only to validate the format, never stored: the pixels free
        // with the answer rather than needing a free() the next edit can lose.
        if (!kittyDecodePNGBytes({ pngBytes.get(), static_cast<size_t>(pngLength) })) {
            kittyRespond(t, c, "EINVAL:invalid image data");
            return;
        }
        kittyRespond(t, c, "OK");
        return;
    }

    if (!(c->format == 24 || c->format == 32) || c->width <= 0 || c->height <= 0) {
        kittyRespond(t, c, "EINVAL:missing image geometry");
        return;
    }
    // The stored image carries its dimensions in sixteen bits, so a dimension
    // past that range is refused where the sender gets an answer — a cast at
    // the store would wrap it into a small image whose pixel length disagrees
    // with its geometry.
    if (c->width > 0xFFFF || c->height > 0xFFFF) {
        kittyRespond(t, c, "EINVAL:image too large");
        return;
    }
    uint32_t bytesPerPixel = (c->format == 32) ? 4UL : 3UL;
    if (static_cast<uint32_t>(c->width) > Ubiquitty::Image::maxDecodedImageBytes / bytesPerPixel || static_cast<uint32_t>(c->height) > Ubiquitty::Image::maxDecodedImageBytes / (static_cast<uint32_t>(c->width) * bytesPerPixel)) {
        kittyRespond(t, c, "EINVAL:image too large");
        return;
    }
    uint32_t expected = static_cast<uint32_t>(c->width) * static_cast<uint32_t>(c->height) * bytesPerPixel;
    auto extracted = kittyResolveTransport(t, c, payload, static_cast<int>(expected), TempFileUnlink::Preserve);
    if (!extracted) {
        kittyRespond(t, c, extracted.error());
        return;
    }
    KittyBytes raw = std::move(extracted->bytes); // discard after validation
    const int rawLength = extracted->length;
    if (rawLength != static_cast<int>(expected)) {
        kittyRespond(t, c, "EINVAL:payload size mismatch");
        return;
    }
    kittyRespond(t, c, "OK");
}

static void kittyDisplayExisting(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c)
{
    Image::TerminalImage* image = kittyFindImage(t, c);
    Image::TerminalImagePlacement* p;
    if (!image) {
        kittyRespond(t, c, "ENOENT:image not found");
        return;
    }
    Image::markDirty(t, image);
    if (c->unicodePlacement == 1) {
        // a=p,U=1: turn the image into a Unicode-placeholder virtual placement.
        kittyApplyVirtualPlacement(image, c);
        Image::syncMirror(t, image);
        Image::markDirty(t, image);
        kittyRespond(t, c, "OK");
        return;
    }
    p = Image::allocatePlacement(t);
    if (p)
        kittyFillPlacement(p, t, c, image->imageID);
    kittyAdvanceCursorAfterImage(t, c, image);
    Image::syncMirror(t, image);
    Image::markDirty(t, image);
    kittyRespond(t, c, "OK");
}

// Append a frame to an existing image (a=f). Payload is the frame's pixel bytes
// (same format/size as the base); `z` carries the display delay. Frames beyond
// maxTerminalImageFrameCount are dropped with a kitty error.
static void kittyAddFrame(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c, std::span<const char> payload)
{
    Image::TerminalImage* image;
    uint32_t expected;
    uint32_t bytesPerPixel;
    int rawLength = 0;
    if (!c->hasImageID) {
        kittyRespond(t, c, "EINVAL:frame requires image id");
        return;
    }
    image = kittyFindImage(t, c);
    if (!image) {
        kittyRespond(t, c, "ENOENT:image not found");
        return;
    }
    if (image->frameCount >= Image::maxTerminalImageFrameCount) {
        kittyRespond(t, c, "ENOSPC:too many frames");
        return;
    }
    if (image->format != 24 && image->format != 32) {
        // Frames are raw 24/32-bit only (no f=100 PNG frames).
        kittyRespond(t, c, "ENOTSUP:frame format unsupported");
        return;
    }
    bytesPerPixel = (image->format == 32) ? 4UL : 3UL;
    expected = static_cast<uint32_t>(image->width) * static_cast<uint32_t>(image->height) * bytesPerPixel;
    auto extracted = kittyResolveTransport(t, c, payload, static_cast<int>(expected), TempFileUnlink::Consume);
    if (!extracted) {
        kittyRespond(t, c, extracted.error());
        return;
    }
    // Every refusal below returns with ownedFrame still holding the buffer, so
    // each one frees it; the frame that keeps it releases at the transfer.
    KittyBytes ownedFrame = std::move(extracted->bytes);
    rawLength = extracted->length;
    if (rawLength != static_cast<int>(expected)) {
        kittyRespond(t, c, "EINVAL:frame size mismatch");
        return;
    }
    // maxDecodedImageBytes bounds the image, not just its base buffer: frames are
    // the same image, and each one is exactly width*height*bytesPerPixel, so
    // maxTerminalImageFrameCount of them reach 16x the ceiling from payloads that
    // arrive zlib-compressed. Refusing here is what makes enforceBudget's "only
    // `keep` remains (bounded by the per-image cap)" true on this path — it cannot
    // evict the image it was asked to keep, so it cannot bound one that grows.
    if (Image::imageBytes(*image) + static_cast<uint64_t>(rawLength) > static_cast<uint64_t>(Image::maxDecodedImageBytes)) {
        kittyRespond(t, c, "ENOSPC:frame exceeds the image size limit");
        return;
    }
    {
        Image::TerminalImageFrame* f = &image->frames[image->frameCount++];
        f->used = true;
        f->pixels = ownedFrame.release();
        f->pixelLength = static_cast<uint32_t>(rawLength);
        // Kitty `z` on a=f is the per-frame gap: z>0 = gap in ms, z=0/unspecified
        // = default gap (40ms, applied at playback via delay 0), z<0 = gapless
        // (instantly skipped — base data only, never shown).
        f->gapless = (c->hasZIndex && c->zIndex < 0);
        f->delayMilliseconds = (c->hasZIndex && c->zIndex > 0) ? static_cast<uint32_t>(c->zIndex) : 0;
    }
    // New frame = new content for this id; bump so a frame-aware cache re-uploads
    // even before currentFrame advances.
    image->contentGeneration = ++t.images.kittyImageContentSeq;
    // A frame adds to the Terminal total that totalBytes already counts, so the
    // budget needs enforcing here as it is after every other install.
    Image::enforceBudget(&t, image);
    kittyRespond(t, c, "OK");
}

// Animation control (a=a): c=1 stop, c=2 run-once, c=3 loop; r=N (1-based) jumps
// to a frame; z=delay overrides the current frame's delay. cellColumns holds `c`,
// cellRows holds `r`.
static void kittyAnimationControl(Ubiquitty::Terminal& t, const Ubiquitty::Kitty::GraphicsControl* c)
{
    Image::TerminalImage* image;
    if (!c->hasImageID) {
        kittyRespond(t, c, "EINVAL:animation requires image id");
        return;
    }
    image = kittyFindImage(t, c);
    if (!image) {
        kittyRespond(t, c, "ENOENT:image not found");
        return;
    }
    // cellColumns ('c'): 1=stop, 2=run-once, 3=loop. Other values left as-is.
    switch (c->cellColumns) {
    case 1:
        image->animationState = Image::AnimationState::Stopped;
        break;
    case 2:
        image->animationState = Image::AnimationState::RunOnce;
        image->frameClockMilliseconds = 0;
        break;
    case 3:
        image->animationState = Image::AnimationState::Looping;
        image->frameClockMilliseconds = 0;
        break;
    default:
        break;
    }
    // The frame number is compared as the integer it was sent as: narrowed
    // first, r=65536 reads as 0 and names a frame past the array.
    if (c->cellRows > 0 && c->cellRows <= static_cast<int>(image->frameCount)) {
        image->currentFrame = static_cast<uint16_t>(c->cellRows - 1);
        image->frameClockMilliseconds = 0;
    }
    if (c->hasZIndex && image->currentFrame < image->frameCount) {
        // z gap override: z>0 = ms, z<0 = gapless, z=0 = default gap.
        Image::TerminalImageFrame* f = &image->frames[image->currentFrame];
        f->gapless = (c->zIndex < 0);
        f->delayMilliseconds = (c->zIndex > 0) ? static_cast<uint32_t>(c->zIndex) : 0;
    }
    Image::markDirty(t, image);
    kittyRespond(t, c, "OK");
}

static void kittyProcessComplete(Ubiquitty::Terminal& t, Ubiquitty::Kitty::GraphicsControl* c, std::span<const char> payload)
{
    switch (c->action) {
    case 'T':
        (void)kittyStoreDirectImage(t, c, payload, TransmitDisplay::TransmitAndDisplay);
        break;
    case 't':
        (void)kittyStoreDirectImage(t, c, payload, TransmitDisplay::TransmitOnly);
        break;
    case 'p':
        kittyDisplayExisting(t, c);
        break;
    case 'd':
        kittyDelete(t, c);
        break;
    case 'q':
        kittyQuery(t, c, payload);
        break;
    case 'f':
        kittyAddFrame(t, c, payload);
        break;
    case 'a':
        kittyAnimationControl(t, c);
        break;
    default:
        kittyRespond(t, c, "ENOTSUP:unsupported action");
        break;
    }
}

// The protocol gives a continuation block only `m` and `q` (and `a=f` again on
// an animation frame's blocks); `i` and `p` are accepted on the final block
// because clients restate them there. A different action, or a key describing
// the data, therefore belongs to a new command and never to the open transfer.
static bool kittyBlockOpensCommand(const Ubiquitty::Kitty::GraphicsControl& opener, const Ubiquitty::Kitty::GraphicsControl& block)
{
    return (block.hasAction && block.action != opener.action) || block.hasTransferKey;
}

void dispatch(Ubiquitty::Terminal& t)
{
    char* semi;
    char* payload;
    int payloadLength;
    Ubiquitty::Kitty::GraphicsControl ctrl;
    if (t.images.kittyAPC.length <= 0 || t.images.kittyAPC.data[0] != 'G')
        return;
    semi = strchr(t.images.kittyAPC.data + 1, ';');
    const bool hasSeparator = (semi != nullptr);
    if (semi) {
        *semi = 0;
        payload = semi + 1;
        payloadLength = t.images.kittyAPC.length - static_cast<int>(payload - t.images.kittyAPC.data);
    } else {
        // No ';' separator → a control-only command with no payload (a kitty
        // delete/query or a chunked-transfer opener). Parse the control with an
        // empty payload; the m==1 path below remembers it as the chunk control so
        // m=0 finalizes with the real geometry.
        payload = t.images.kittyAPC.data + t.images.kittyAPC.length;
        payloadLength = 0;
    }
    kittyParseControl(t.images.kittyAPC.data + 1, &ctrl);
    // An open transfer whose next block is a command of its own was abandoned:
    // the sender exited, or was killed, between blocks. Taking that command as
    // the final block would refuse it under the dead sender's keys, so a delete
    // would leave the dead sender's images drawn and the next transfer would be
    // appended to pixels nobody finishes. The abandoned data is dropped and the
    // command runs as sent.
    if (t.images.kittyChunkActive && kittyBlockOpensCommand(t.images.kittyChunkControl, ctrl)) {
        t.images.kittyChunkActive = false;
        t.images.kittyChunk.clearReleasingAbove(Ubiquitty::Kitty::maxRetainedAPCCapacity);
    }
    // A direct transmit/query (a=t/T/q with t=d) needs its data after a ';'; with
    // no separator it's a framing error, not a size mismatch. Excludes control-only
    // actions, chunk openers (m=1), and chunk finalizes, which may omit the ';'.
    if (!hasSeparator && !t.images.kittyChunkActive && ctrl.more != 1 && (ctrl.action == 't' || ctrl.action == 'T' || ctrl.action == 'q') && (ctrl.transport == 'd' || ctrl.transport == 0)) {
        kittyRespond(t, &ctrl, "EINVAL:missing payload separator");
        return;
    }
    if (ctrl.more == 1) {
        if (!t.images.kittyChunkActive) {
            t.images.kittyChunk.length = 0;
            t.images.kittyChunkControl = ctrl;
            t.images.kittyChunkActive = true;
        }
        if (!t.images.kittyChunk.append({ payload, static_cast<std::size_t>(payloadLength) }, Ubiquitty::Kitty::maxAPCCapacity)) {
            t.images.kittyChunkActive = false;
            t.images.kittyChunk.clearReleasingAbove(Ubiquitty::Kitty::maxRetainedAPCCapacity);
            kittyRespond(t, &ctrl, "EINVAL:chunk too large");
        }
        return;
    }
    if (t.images.kittyChunkActive) {
        Ubiquitty::Kitty::GraphicsControl merged = t.images.kittyChunkControl;
        if (ctrl.hasImageID) {
            merged.imageID = ctrl.imageID;
            merged.hasImageID = true;
        }
        if (ctrl.hasPlacementID) {
            merged.placementID = ctrl.placementID;
            merged.hasPlacementID = true;
        }
        if (ctrl.quiet)
            merged.quiet = ctrl.quiet;
        if (!t.images.kittyChunk.append({ payload, static_cast<std::size_t>(payloadLength) }, Ubiquitty::Kitty::maxAPCCapacity)) {
            t.images.kittyChunkActive = false;
            t.images.kittyChunk.clearReleasingAbove(Ubiquitty::Kitty::maxRetainedAPCCapacity);
            kittyRespond(t, &merged, "EINVAL:chunk too large");
            return;
        }
        t.images.kittyChunkActive = false;
        kittyProcessComplete(t, &merged, std::span<const char>(t.images.kittyChunk.data, static_cast<size_t>(t.images.kittyChunk.length)));
        t.images.kittyChunk.clearReleasingAbove(Ubiquitty::Kitty::maxRetainedAPCCapacity);
        // Drain inline (symmetric with the non-chunked path below), so a chunked
        // o=z / PNG transmit doesn't wait a tick for the next per-tick drain.
        Ubiquitty::Kitty::drainAsyncDecodes(&t);
        return;
    }
    kittyProcessComplete(t, &ctrl, std::span<const char>(payload, static_cast<size_t>(payloadLength)));
    // Drain completed decodes. The workers ran synchronously inside submit, so
    // this delivers them in the same call stack as feed() — tests probing the grid
    // right after feed() see the integrated state.
    Ubiquitty::Kitty::drainAsyncDecodes(&t);
}

void drainAsyncDecodes(Ubiquitty::Terminal* t)
{
    // Only this Terminal's queues: a completion writes into the Terminal that
    // submitted it, so it belongs on the thread holding that Terminal's lock.
    // Fast-path skips the drain when nothing is pending (most APCs submit none).
    if (!t)
        return;
    if (t->asyncDecodes.kittyDecode && t->asyncDecodes.kittyDecode->pendingCount() > 0)
        t->asyncDecodes.kittyDecode->drain(kittyPNGDecodeCompletion);
    if (t->asyncDecodes.kittyInflate && t->asyncDecodes.kittyInflate->pendingCount() > 0)
        t->asyncDecodes.kittyInflate->drain(kittyDirectInflateCompletion);
}

// Free every decode this Terminal submitted but never drained, without running a
// completion: at teardown the Terminal a completion would write into is going
// away. Each job's buffers are owned, so destroying the job releases them.
void discardPendingDecodes(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    if (t->asyncDecodes.kittyDecode)
        t->asyncDecodes.kittyDecode->drain([](void* p) { delete static_cast<KittyPNGDecodeJob*>(p); });
    if (t->asyncDecodes.kittyInflate)
        t->asyncDecodes.kittyInflate->drain([](void* p) { delete static_cast<KittyDirectInflateJob*>(p); });
}

} // namespace Ubiquitty::Kitty
