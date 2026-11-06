// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Geometry.h"
#include "Base/Types.h"
#include "Forward.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

// The protocol-agnostic terminal image model. Kitty graphics, DEC sixel, and
// iTerm inline images all decode into a TerminalImage (RGBA/RGB pixels + optional
// animation frames) plus one or more TerminalImagePlacements (where on the grid it
// is drawn). The Terminal struct owns the image / placement arrays.

namespace Ubiquitty::Image {

constexpr int maxTerminalImageCount = 32;
constexpr int maxTerminalImagePlacementCount = 128;
constexpr int maxTerminalImageFrameCount = 16;

/// Frees a pixel buffer the way the image store does.
struct FreePixels {
    void operator()(unsigned char* pixels) const noexcept { std::free(pixels); }
};

/// A move-only owner for a decoded pixel buffer, released when the store takes it.
///
/// `dispose` frees a stored image's pixels with `free`, so a buffer destined for a
/// TerminalImage has to come from `malloc` — which rules out the containers that
/// would otherwise own it. This carries such a buffer while it is still the
/// decoder's, so no early return between allocating and storing can leak it, and
/// `storeRGBAImage` receives ownership only through an explicit `release`.
using OwnedPixels = std::unique_ptr<unsigned char, FreePixels>;

// kitty default frame gap: a frame with an unspecified/zero gap (including the
// root frame from a=T) is shown for this many milliseconds, not skipped. Only an
// explicitly negative z marks a frame `gapless` (instantly skipped).
constexpr uint32_t terminalImageDefaultFrameGapMilliseconds = 40;

// Default per-Terminal total image-pixel budget, capping the SUM across slots
// (each image is separately capped at maxDecodedImageBytes). On overflow the
// store evicts least-recently-used images (non-displayed first); clients
// re-transmit. Overridable via Terminal::kittyImageBytesBudget.
constexpr uint64_t terminalImageBytesBudgetDefault = 256ULL * 1024 * 1024;

// Per-image decoded ceiling (RGBA bytes), the decompression-bomb gate. It bounds
// a decoded buffer rather than any one protocol's transfer, so the kitty
// graphics path and the inline (sixel / iTerm2) path are held to the same limit
// — a ceiling that differed between them would be the smaller of the two in
// practice, silently.
//
// Where it is checked differs by path.
//
// Where Core computes the dimensions itself — the sixel and kitty raw-RGB paths
// — they are checked against this before allocating, with overflow-safe
// division, which is what the gate is for.
//
// Where a PLATFORM decoder produces the pixels (`PAL::ImageDecodeCallback`, used
// by `kittyDecodeViaPlatform` and `handleInlineImage` for PNG and friends), Core
// learns the dimensions only from what the callback returns, so the gate that
// prevents the allocation is the decoder's: one that can read the dimensions
// from a header refuses with `isDecodedImageBytesRefused` before allocating, as
// the Apple decoder does with what ImageIO reads from the header. Core checks
// what came back as well and frees a refusal. That is the backstop for a decoder
// that cannot read a header first, and it bounds what is stored but not what
// such a decoder allocates on the way through.
constexpr int maxDecodedImageBytes = 64 * 1024 * 1024;

// The per-axis ceiling a platform decoder refuses above, the dimension half of
// the gate the bytes ceiling above is the area half of.
//
// A decoder that can read dimensions from a header declines before allocating,
// which is the pre-check the paragraph above describes. It is stated here rather
// than by each decoder because a bound that differs between frontends is the
// smallest of them in practice and silently so: the same image is refused on one
// platform and stored on another, and nothing says which is intended. Every
// frontend decoder was spelling 16384 privately.
constexpr int maxDecodedImageDimension = 16384;

/// Whether a decoder should refuse an image of this size before allocating.
///
/// The axis half of the gate alone, which passes 16384 x 16384, a gibibyte of
/// RGBA. It is the whole rule only for a background picture the user
/// configured; a decoder producing an image for the terminal asks
/// `isDecodedImageBytesRefused`, which adds the byte ceiling.
///
/// - Parameters:
///   - width: The decoded width in pixels.
///   - height: The decoded height in pixels.
/// - Returns: Whether the image is outside what any frontend will store.
[[nodiscard]] constexpr bool isDecodedImageSizeRefused(long long width, long long height)
{
    return width <= 0 || height <= 0 || width > maxDecodedImageDimension || height > maxDecodedImageDimension;
}

/// Whether a decoder should refuse an image of this size and pixel depth before
/// allocating its pixels.
///
/// Both halves of the gate: the axis ceiling and `maxDecodedImageBytes`, so at
/// four bytes a pixel the largest square accepted is 4096 x 4096. The bytes are
/// compared by division, so no width and height can overflow a product.
///
/// - Parameters:
///   - width: The decoded width in pixels.
///   - height: The decoded height in pixels.
///   - bytesPerPixel: The bytes each decoded pixel takes: 4 for the RGBA every
///     decoder emits, 3 for RGB.
/// - Returns: Whether the image is outside either limit.
[[nodiscard]] constexpr bool isDecodedImageBytesRefused(long long width, long long height, int bytesPerPixel = 4)
{
    return isDecodedImageSizeRefused(width, height) || height > maxDecodedImageBytes / bytesPerPixel / width;
}

// Animation playback state per TerminalImage. AnimationState::Stopped stops on
// the current frame; AnimationState::RunOnce advances frames and then stops
// on the last one; AnimationState::Looping wraps back to frame 0 at the end.
enum class AnimationState : uint8_t {
    Stopped = 0,
    RunOnce = 1,
    Looping = 2,
};

// One animation frame. frames[0] always shadows the base pixel buffer so the
// renderer path can stay frame-unaware; multi-frame images add 1..N via a=f.
struct TerminalImageFrame {
    bool used;
    // `gapless` frames (kitty z=negative) are instantly skipped in playback; they
    // exist only as base data for diff frames. A non-gapless frame with
    // delayMilliseconds 0 is shown for terminalImageDefaultFrameGapMilliseconds.
    bool gapless;
    uint32_t pixelLength;
    /* owned */ unsigned char* pixels; // malloc'd; freed in dispose() or frame cleanup
    uint32_t delayMilliseconds;
};

struct TerminalImage {
    bool used;
    // "Image has at least one visible placement" view the renderer consumes.
    // Source-of-truth geometry is the TerminalImagePlacement array; recomputed
    // when placements are added or removed.
    bool displayed;
    uint32_t imageID;
    uint32_t imageNumber;
    uint16_t width;
    uint16_t height;
    uint16_t format; // 24=RGB, 32=RGBA
    uint32_t pixelLength;
    /* owned */ unsigned char* pixels; // malloc'd; freed in dispose(); frames[0].pixels aliases this
    // frames[0].pixels aliases this base buffer (not a separate allocation), so a
    // frame-unaware renderer reading image->pixels sees frames[currentFrame].
    // Frames 1..frameCount-1 own independent heap buffers.
    std::array<TerminalImageFrame, maxTerminalImageFrameCount> frames;
    uint16_t frameCount;
    uint16_t currentFrame;
    uint32_t frameClockMilliseconds; // accumulated against frames[currentFrame].delayMilliseconds
    AnimationState animationState;
    // Monotonic "stored at" stamp for LRU eviction; 0 until first stored.
    uint64_t lastUsedSeq;
    // Bumped every time pixel bytes are (re)assigned or a frame is added. Folded
    // into the GPU texture-cache stamp so a same-id re-transmit at identical
    // geometry/format/frame (the standard single-id animation pattern) still
    // invalidates the cached texture — without it the renderer freezes on frame 1.
    uint64_t contentGeneration;
    // Kitty Unicode-placeholder virtual placement (a=T/t with U=1). When set, no
    // grid placement is created; the image is positioned by U+10EEEE placeholder
    // cells whose diacritics carry (row, column) and foreground color the image id.
    // virtualColumns/virtualRows are the requested c/r (0 = derive from the
    // placeholder span); the resolver partitions the image into that grid and
    // shows cell (row, col) in the matching placeholder. virtualPlacementID
    // mirrors p= for matching from the placeholder's underline color.
    bool virtualPlacement;
    uint16_t virtualColumns;
    uint16_t virtualRows;
    uint32_t virtualPlacementID;
};

// A TerminalImagePlacement describes one instance of a TerminalImage drawn on
// the grid. Each image (one stored pixel buffer) can be displayed at
// multiple positions with independent geometry. The geometry mirrors the
// kitty graphics placement model (the richest of the three protocols):
// c, r — target cell-rectangle dimensions (0 = derive from pixel size).
// X, Y — pixel offsets relative to the anchor cell's top-left.
// x, y, w, h — source sub-rectangle inside the stored image.
// z — stacking order; higher values draw on top.
// C — cursor-movement policy. 0 (default) = advance cursor after the
// image like a wide-char run; 1 = leave cursor in place.
// The drawn size, when set, is a protocol's own device-pixel size (OSC 1337
// `Npx` and natural size); the cell box then names only the cells covered.
/// The highest row above the screen a placement can be anchored at.
///
/// `TerminalImagePlacement::cursorY` is 16 bits and a scrollback can hold a
/// million lines, so a placement scrolled past this row cannot be stored where
/// it is. It is dropped or held at this row; narrowed, its row would wrap to a
/// positive one and the image would reappear on the screen.
inline constexpr int minPlacementRow = std::numeric_limits<int16_t>::min();

struct TerminalImagePlacement {
    bool used;
    uint32_t imageID; // foreign key into TerminalImage.imageID
    uint32_t placementID;
    bool hasPlacementID;
    uint16_t cursorX; // anchor cell, captured when the placement was emitted
    int16_t cursorY; // signed: goes negative as the image scrolls above the top

    uint16_t cellColumns; // c — target cell width, 0 = use natural
    uint16_t cellRows; // r — target cell height, 0 = use natural
    uint16_t pixelOffsetX; // X — sub-cell pixel offset, 0..cellWidth-1
    uint16_t pixelOffsetY; // Y — sub-cell pixel offset, 0..cellHeight-1
    uint16_t sourceX; // x — source rect upper-left, 0..image.width-1
    uint16_t sourceY; // y
    uint16_t sourceWidth; // w — source rect dims, 0 = remainder from (sourceX, sourceY)
    uint16_t sourceHeight; // h
    // The device-pixel size the picture is drawn at, both axes or neither;
    // 0 sizes it from the cell box, or at the source's own pixels without one.
    uint16_t drawnPixelWidth;
    uint16_t drawnPixelHeight;
    int32_t zIndex; // z — stacking order (signed)
    uint8_t cursorPolicy; // C — 0=move, 1=stay
    // The screen active when this placement was emitted. A placement takes part
    // in rendering, scrolling and erasing only while its own screen is active:
    // the two buffers hold independent pictures, and a full-screen program's
    // erase must not reach the images the primary screen is holding.
    uint8_t onAlternateScreen;
};

/// Whether `placement` belongs to the screen that is currently active.
///
/// The one place the rule is stated. Rendering, scrolling and erasing each ask
/// it, and a copy of the comparison at each site is three chances for the
/// screens to disagree about which pictures are theirs.
///
/// - Parameters:
///   - placement: The placement to test.
///   - alternateActive: Whether the alternate screen is the active one.
/// - Returns: True when the placement was emitted on the active screen.
[[nodiscard]] constexpr bool placementIsOnActiveScreen(const TerminalImagePlacement& placement, bool alternateActive)
{
    return placement.onAlternateScreen == (alternateActive ? 1u : 0u);
}

// Ubiquitty::Image — the protocol-agnostic model lifecycle. Decoder backends
// call storeRGBAImage after decoding; the VT100 core drives scroll / clear /
// animation.

} // namespace Ubiquitty::Image

namespace Ubiquitty::Image {

/// Whether a decoded image has reached the GPU yet.
enum class TextureState : uint8_t {
    NoTexture,
    HasTexture
};
/// Where the cursor lands after an image is placed.
enum class CursorMotion : uint8_t {
    Advance,
    Stay
};

// Sum of every live image's owned pixel bytes (base buffer + heap-owned frames;
// frames[0] aliases the base, so not double counted). The quantity the per-Terminal
// byte budget caps.
/// Returns the bytes one image holds, base pixels plus every extra frame.
///
/// `frames[0]` aliases the base buffer, so it is counted once. This is the
/// quantity `maxDecodedImageBytes` bounds: an animation's frames are the same
/// image, and counting only the base would let `a=f` grow one image without
/// limit.
///
/// - Parameter image: The image to measure; an unused slot measures zero.
/// - Returns: The byte total for that image.
uint64_t imageBytes(const TerminalImage& image);

uint64_t totalBytes(const Ubiquitty::Terminal* t);

// GPU-upload-cache helpers (texture-backend-neutral). Every backend prepares an
// inline image identically: widen a format-24 RGB buffer to straight RGBA8, and
// key a per-imageID cache on a content stamp so a frame advance or same-id
// re-decode at a new size forces re-upload. The actual texture allocation stays
// platform-side.

// Widen `width`*`height` pixels of `source` (format 24 = RGB, 32 = RGBA) into
// `destination` as straight RGBA8 (RGB pixels get alpha 255). Preconditions:
// buffers must not overlap; `destination` >= width*height*4 bytes; `source` >=
// width*height*bytesPerPixel. Format 32 is already RGBA8 (single memcpy); any
// other format is treated as 24-bit and expanded.
inline void widenToRGBA8(const unsigned char* source, int width, int height, int format, unsigned char* destination)
{
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (format == 32) {
        std::memcpy(destination, source, pixelCount * 4);
        return;
    }
    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        destination[pixel * 4 + 0] = source[pixel * 3 + 0];
        destination[pixel * 4 + 1] = source[pixel * 3 + 1];
        destination[pixel * 4 + 2] = source[pixel * 3 + 2];
        destination[pixel * 4 + 3] = 255;
    }
}

// 64-bit content stamp folding dimensions + current frame + format +
// contentGeneration. A stored stamp differing from the live image's is stale and
// must re-upload — catches a frame advance, format change, same-id replace at a
// new size, and a same-id re-transmit at identical geometry (where only
// contentGeneration changes). contentGeneration is mixed through an odd
// multiplier so it can't alias the geometry/frame/format bits.
/// Returns a cache key identifying an image's decoded content.
///
/// Each field occupies bits no other field reaches. An XOR of overlapping
/// shifts lets a change in one field cancel a change in another, and the pair
/// then shares a key — `currentFrame << 4` against a raw `format` puts frame
/// bit 1 on top of format bit 5, so frame 2 with format 0 stamps identically to
/// frame 0 with format 32 and the renderer keeps the texture it already has.
/// `format` 0 is not hypothetical: it is the "unknown, treat as RGB24" code the
/// decoder passes.
///
/// The masks are the widths those bits allow, so a value past one wraps inside
/// its own field rather than corrupting its neighbour.
///
/// - Parameters:
///   - width: Decoded pixel width; distinct up to 1M.
///   - height: Decoded pixel height; distinct up to 1M.
///   - currentFrame: Animation frame index; distinct up to 4096.
///   - format: Bits per pixel, or 0 for unknown; distinct up to 256.
///   - contentGeneration: Bumped when a same-id re-transmit replaces the pixels
///     without changing any of the above.
/// - Returns: The key, equal for two images exactly when every argument is.
inline uint64_t imageContentStamp(int width, int height, int currentFrame, int format, uint64_t contentGeneration)
{
    return ((static_cast<uint64_t>(width) & 0xFFFFFULL) << 40)
        ^ ((static_cast<uint64_t>(height) & 0xFFFFFULL) << 20)
        ^ ((static_cast<uint64_t>(currentFrame) & 0xFFFULL) << 8)
        ^ (static_cast<uint64_t>(format) & 0xFFULL)
        ^ (contentGeneration * 0x9E3779B97F4A7C15ULL);
}

/// Returns the cache key for an image, read from the image itself.
///
/// The five-argument form is the definition; this is the one every backend
/// should call. Each of the three texture caches spelled the same five fields
/// out at its own call site. A backend that leaves `contentGeneration` out
/// compiles, caches, and freezes a kitty animation on its first frame, and the
/// call site it is missing from looks like one that meant to omit it.
///
/// - Parameter image: The image whose decoded content is being keyed.
/// - Returns: The key, equal for two images exactly when their content is.
inline uint64_t imageContentStamp(const TerminalImage& image)
{
    return imageContentStamp(static_cast<int>(image.width), static_cast<int>(image.height),
        static_cast<int>(image.currentFrame), static_cast<int>(image.format), image.contentGeneration);
}

// Whether a cached texture is still valid: entry exists and its stored stamp
// matches. False → (re-)upload. Pass the frame the renderer is showing
// (currentFrame for a frame-aware backend, 0 otherwise).
inline bool imageCacheStampMatches(TextureState textureState, uint64_t storedStamp, int width, int height, int currentFrame, int format, uint64_t contentGeneration) { return textureState == TextureState::HasTexture && storedStamp == imageContentStamp(width, height, currentFrame, format, contentGeneration); }

/// Completes a cell footprint for cursor movement.
///
/// The missing axis is derived from the source aspect and cell metrics. This
/// whole-cell footprint does not determine the image's drawn aspect ratio.
///
/// Both axes given, or neither, are returned unchanged: those are already
/// answers (an explicit box, and "natural size" respectively).
///
/// - Parameters:
///   - pixelWidth: The image's width in pixels.
///   - pixelHeight: Its height in pixels.
///   - cellPixelWidth: One cell's width in pixels; 0 leaves the box unchanged.
///   - cellPixelHeight: One cell's height in pixels; 0 likewise.
///   - cellColumns: The requested columns, 0 to derive; receives the result.
///   - cellRows: The requested rows, 0 to derive; receives the result.
void completeCellBox(int pixelWidth, int pixelHeight, int cellPixelWidth, int cellPixelHeight, int* cellColumns, int* cellRows);

/// Where an OSC 1337 image goes and how large it is drawn.
///
/// The protocol's units follow iTerm2, which reads `Npx` and a natural size as
/// device pixels (its default Retina inline images: N pixels are N divided by
/// the backing scale in points), a bare `N` as cells and `N%` as a share of the
/// grid. Each axis covers whole cells, and the picture is drawn from the
/// anchor's top-left inside the requested box, fitted to its aspect unless
/// `preserveAspectRatio=0` stretches it.
struct InlineImageExtent {
    /// The cells the image covers, which the cursor steps past.
    IntSize coveredCells;
    /// The placement's cell box. A preserved-aspect box of cells keeps only its
    /// constraining axis, so the renderer derives the other in its own cells.
    IntSize placementCells;
    /// The placement's drawn size in device pixels, zero when the request was
    /// stated in cells alone and the renderer sizes it from `placementCells`.
    IntSize drawnPixels;
};

/// Resolves OSC 1337 `width`, `height` and `preserveAspectRatio` tokens.
///
/// - Parameters:
///   - widthToken: The `width` value, empty for auto.
///   - heightToken: The `height` value, empty for auto.
///   - preserveToken: The `preserveAspectRatio` value; anything but `0` preserves.
///   - imagePixels: The decoded image's size in pixels.
///   - cellPixels: One cell in device pixels; zero falls back to a nominal 8x16.
///   - grid: The grid's extent, for `N%`.
///   - cursorColumn: The anchor column; the image narrows to the columns left.
/// - Returns: The covered cells, the placement's cell box and its drawn size.
[[nodiscard]] InlineImageExtent inlineImageExtent(const char* widthToken, const char* heightToken, const char* preserveToken, IntSize imagePixels, IntSize cellPixels, GridSize grid, int cursorColumn);

// Store a pre-decoded RGBA8 image (w*h*4 bytes, malloc'd) and place it at the
// cursor. cellColumns/cellRows give the target cell rect (0 = derive from pixel
// size); a nonzero drawnPixels draws it at that device-pixel size instead, the
// cell rect then naming the cells it covers. Cursor left in place. Returns true
// with ownership of `pixels` transferred; false (no free slot) leaves it to the
// caller to free.
bool storeRGBAImage(Ubiquitty::Terminal* t, unsigned char* pixels, int w, int h, int cellColumns, int cellRows, IntSize drawnPixels = {});

/// The id for an image whose protocol named none: a sixel, an OSC 1337 file,
/// or a kitty transmit without `i=`.
///
/// Every id below 2^31 belongs to kitty clients, which number from one, so
/// the terminal's own numbering runs above them. An inline image that took
/// id 1 was the image a client's `i=1` then found and replaced, and the
/// client's pixels were what its box showed.
///
/// - Parameter t: The terminal whose counter advances.
/// - Returns: An id no client can name by accident.
[[nodiscard]] uint32_t allocateImageID(Ubiquitty::Terminal& t);
/// Shifts every placement up, so images scroll with the text beneath them.
///
/// A placement carried entirely above the top is dropped, and its image freed
/// when nothing else references it.
///
/// - Parameters:
///   - t: The terminal whose placements move.
///   - n: How many rows. Zero or fewer is a no-op.
void scrollPlacementsUp(Ubiquitty::Terminal& t, int n);
/// Disposes placements overlapping a row span, so erasing the screen takes the
/// images in that region with it.
///
/// - Parameters:
///   - t: The terminal whose placements are cleared.
///   - fromRow: First row of the span, inclusive.
///   - toRow: Last row, inclusive. A span that runs backwards is a no-op.
void clearPlacementsInRows(Ubiquitty::Terminal& t, int fromRow, int toRow);
/// Whether a placement on the active screen covers a row of the viewport.
///
/// The frame planner asks this: a picture is painted into the retained pixels
/// with the cells, outside the row damage, so a frame that shows one cannot
/// reuse them.
///
/// - Parameters:
///   - t: The terminal.
///   - viewOffset: Scrollback rows above the live tail, as the renderer shows them.
/// - Returns: True when a placement's rows, shifted by the view, meet the
///   screen, or when the fullscreen content mode is on and any placement is
///   live, since that fit paints the drawable whatever row anchors it.
[[nodiscard]] bool hasPlacementOnViewport(const Ubiquitty::Terminal& t, int viewOffset);
/// Frees every image and placement the terminal holds.
///
/// - Parameters:
///   - t: The terminal to clear.
void disposeAllImages(Ubiquitty::Terminal& t);

/// Carries a terminal's image CONTENT across a text-only recovery refeed.
///
/// A tmux content resync is `capture-pane -e`: captured TEXT, which cannot
/// carry an image. The refeed then either clears the buffer (a full-history
/// resync) or rewrites the rows a placement anchors to — and an erase removes
/// the images it covers — so every resync destroyed the pane's images and
/// reconstructed text only. On screen that is a sixel that vanishes on the
/// resync cadence, and re-emitted protocol bytes stacking fresh copies.
///
/// The keeper steals the stored images and placements before the refeed (the
/// arrays are blanked, so the refeed's clears dispose nothing) and puts them
/// back after, disposing whatever the refeed itself minted first. The re-fed
/// grid is tmux's truth for the content the placements anchored to, so the
/// anchors land where they were; a placement whose row left for retained
/// history never resolves, which is what scrolling it away means.
///
/// Restore exactly once; a keeper destroyed unrestored frees what it holds.
class ContentKeeper {
public:
    /// Captures `t`'s image content and blanks the live tables.
    explicit ContentKeeper(Ubiquitty::Terminal& t);
    ~ContentKeeper();
    ContentKeeper(const ContentKeeper&) = delete;
    ContentKeeper& operator=(const ContentKeeper&) = delete;

    /// Disposes whatever the refeed minted, then restores the captured
    /// content into `t`.
    void restoreInto(Ubiquitty::Terminal& t);

private:
    struct Payload;
    std::unique_ptr<Payload> m_payload;
};
/// Moves the cursor past an image that covers `coveredCells` from the cursor.
///
/// The kitty graphics protocol and iTerm2 agree: the cursor ends on the image's
/// last row, one column right of its last cell, so the image is never under it.
/// Reaching the right margin wraps to the first column of the row below, as
/// kitty does, and running past the bottom scrolls. A zero column count leaves
/// the column alone, which is the sixel rule.
///
/// - Parameters:
///   - t: The terminal whose cursor moves.
///   - coveredCells: Columns and rows the image covers.
///   - motion: `Stay` (kitty `C=1`) leaves the cursor untouched.
void advanceCursorAfterImage(Ubiquitty::Terminal* t, IntSize coveredCells, CursorMotion motion);
// Advance per-image animation by `elapsedMilliseconds`, honoring each image's animationState
// and per-frame delays.
/// Advances every animated image by the elapsed time.
///
/// - Parameters:
///   - t: The terminal whose images animate.
///   - elapsedMilliseconds: Wall time since the previous advance.
void advanceFrames(Ubiquitty::Terminal* t, uint32_t elapsedMilliseconds);

// Read model — the renderer walks placements in z-order via placement()/placementCount().
// Declared here rather than in the parser header: a renderer asking what is on screen
// has no reason to parse DEC sequences.
[[nodiscard]] const TerminalImage* stored(const Ubiquitty::Terminal* t, int index);
[[nodiscard]] const TerminalImagePlacement* placement(const Ubiquitty::Terminal* t, int index);
[[nodiscard]] int placementCount(const Ubiquitty::Terminal* t);
// Whether any live image has more than one frame and is not Stopped — the animation
// tick's early-out.
[[nodiscard]] bool anyAnimating(const Ubiquitty::Terminal* t);

} // namespace Ubiquitty::Image
