// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Harness.h"
#include <memory>

// test_kitty_placeholders.cpp — kitty Unicode placeholders.
//
// Covers the two pieces of the protocol: the rowcolumn-diacritics decode (the
// Nth diacritic in the kitty table means index N, and the third diacritic is the
// image-id MSB) and the placement path — a virtual placement (a=T,U=1) plus
// U+10EEEE placeholder cells whose diacritics + foreground color position the
// image on the grid. Frontend-agnostic: drives the portable Core + the shared
// resolveImagePlacements resolver, no Metal/Vulkan.
#include "Core/Image/ImagePlacement.h"
#include "Core/Image/TerminalImage.h"
#include "Core/Kitty/UnicodePlaceholders.h"
#include "Core/Rendering/ImageQuads.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <span>
#include <string>
#include <vector>

static std::unique_ptr<Ubiquitty::Terminal> makeTerm(int rows, int columns)
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, rows, columns);
    return t;
}

static void feed(Ubiquitty::Terminal& t, const char* s, int n) { Ubiquitty::processBytes(&t, std::string_view(s, static_cast<std::size_t>(n))); }
static void feedz(Ubiquitty::Terminal& t, const char* s) { feed(t, s, static_cast<int>(std::strlen(s))); }

// Append the UTF-8 of `codePoint` to a growing string buffer.
static void appendCodePoint(std::string& out, uint32_t codePoint)
{
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// ---- 1. diacritic table decode ----
TEST(testDiacriticDecode)
{
    using Ubiquitty::Kitty::rowColumnDiacriticIndex;
    // Spec examples: U+0305=0, U+030D=1, U+030E=2.
    CHECK(rowColumnDiacriticIndex(0x0305) == 0);
    CHECK(rowColumnDiacriticIndex(0x030D) == 1);
    CHECK(rowColumnDiacriticIndex(0x030E) == 2);
    // A few more table positions (sorted-ascending order).
    CHECK(rowColumnDiacriticIndex(0x0310) == 3);
    CHECK(rowColumnDiacriticIndex(0x036F) == 29);
    // Last entry of the protocol's diacritic table.
    CHECK(rowColumnDiacriticIndex(0x1D244) >= 0);
    // Non-table codepoints decode to -1.
    CHECK(rowColumnDiacriticIndex('A') == -1);
    CHECK(rowColumnDiacriticIndex(0x0300) == -1); // a combining mark not in the table
    CHECK(rowColumnDiacriticIndex(0x10EEEE) == -1);
}

// Build a placeholder cell's bytes: U+10EEEE + the given diacritics.
static std::string placeholderCell(std::initializer_list<uint32_t> diacritics)
{
    std::string s;
    appendCodePoint(s, Ubiquitty::Kitty::placeholderCodePoint);
    for (uint32_t d : diacritics)
        appendCodePoint(s, d);
    return s;
}

// ---- 2. placement via a single placeholder cell ----
// Virtual placement of a 2x2px image, then one placeholder cell at grid (0,0)
// with the image id in the 256-color foreground → one quad sampling the
// top-left sub-rect of the image.
TEST(testSinglePlaceholderPlacement)
{
    auto t = makeTerm(24, 80);
    // Transmit + virtual-place a 2x2 RGB image, id 31, no grid placement.
    feedz(*t, "\033_Ga=T,U=1,i=31,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
    CHECK(Ubiquitty::Image::placementCount(t.get()) == 0); // virtual: no grid placement
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->virtualPlacement && img->imageID == 31);
    CHECK(img && img->virtualColumns == 2 && img->virtualRows == 2);

    // Home, set foreground to palette index 31 (the image id), write a
    // placeholder cell carrying row=0,col=0 (U+0305,U+0305).
    feedz(*t, "\033[H\033[38;5;31m");
    std::string cell = placeholderCell({ 0x0305, 0x0305 });
    feed(*t, cell.c_str(), static_cast<int>(cell.size()));

    CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), 0));
    Ubiquitty::Image::ImageQuad q[8];
    int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) });
    CHECK(n == 1);
    if (n == 1) {
        // Quad covers the single home cell (10x20 device px).
        CHECK(q[0].minXInPixels == 0.0f && q[0].minYInPixels == 0.0f);
        CHECK(q[0].maxXInPixels == 10.0f && q[0].maxYInPixels == 20.0f);
        // Samples the top-left quarter of the 2x2 grid: u 0..0.5, v 0..0.5.
        CHECK(q[0].u0 == 0.0f && q[0].v0 == 0.0f);
        CHECK(q[0].u1 == 0.5f && q[0].v1 == 0.5f);
        CHECK(q[0].imageID == 31);
    }
    t.reset();
}

// The placeholder path receives the same grid origin as a direct placement, and
// offsets each per-cell quad by it. Every other case here sits at (0,0), where
// an origin dropped or read off the wrong axis is invisible; this one is
// asymmetric in both axes so each term is reachable only from its own input.
TEST(testPlaceholderQuadsCarryTheGridOrigin)
{
    auto t = makeTerm(24, 80);
    feedz(*t, "\033_Ga=T,U=1,i=31,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
    feedz(*t, "\033[H\033[38;5;31m");
    std::string cell = placeholderCell({ 0x0305, 0x0305 });
    feed(*t, cell.c_str(), static_cast<int>(cell.size()));

    Ubiquitty::Image::ImageQuad q[8];
    int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 7.0f, 13.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) });
    CHECK(n == 1);
    if (n == 1) {
        CHECK(q[0].minXInPixels == 7.0f); // originX, not originY
        CHECK(q[0].minYInPixels == 13.0f); // originY, not originX
        CHECK(q[0].maxXInPixels == 17.0f); // originX + one 10px column
        CHECK(q[0].maxYInPixels == 33.0f); // originY + one 20px row
    }
    t.reset();
}

// ---- 3. full 2x2 grid via explicit per-cell diacritics ----
// Four placeholder cells (row,col) = (0,0)(0,1)(1,0)(1,1) tile the whole image.
TEST(testFullGridPlacement)
{
    auto t = makeTerm(24, 80);
    // 4x4 RGB = 48 bytes = 64 base64 chars.
    feedz(*t,
        "\033_Ga=T,U=1,i=7,f=24,s=4,v=4,c=2,r=2;"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\033\\");
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->virtualPlacement && img->imageID == 7 && img->width == 4 && img->height == 4);

    feedz(*t, "\033[H\033[38;5;7m");
    std::string r0c0 = placeholderCell({ 0x0305, 0x0305 }); // (0,0)
    std::string r0c1 = placeholderCell({ 0x0305, 0x030D }); // (0,1)
    feed(*t, r0c0.c_str(), static_cast<int>(r0c0.size()));
    feed(*t, r0c1.c_str(), static_cast<int>(r0c1.size()));
    feedz(*t, "\033[2;1H"); // row 2, col 1
    std::string r1c0 = placeholderCell({ 0x030D, 0x0305 }); // (1,0)
    std::string r1c1 = placeholderCell({ 0x030D, 0x030D }); // (1,1)
    feed(*t, r1c0.c_str(), static_cast<int>(r1c0.size()));
    feed(*t, r1c1.c_str(), static_cast<int>(r1c1.size()));

    Ubiquitty::Image::ImageQuad q[16];
    int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(16) });
    CHECK(n == 4);
    // Every quad should be present and the four sub-rects together cover [0,1]^2.
    bool sawTL = false, sawTR = false, sawBL = false, sawBR = false;
    for (int i = 0; i < n; ++i) {
        if (q[i].u0 == 0.0f && q[i].v0 == 0.0f)
            sawTL = true;
        if (q[i].u0 == 0.5f && q[i].v0 == 0.0f)
            sawTR = true;
        if (q[i].u0 == 0.0f && q[i].v0 == 0.5f)
            sawBL = true;
        if (q[i].u0 == 0.5f && q[i].v0 == 0.5f)
            sawBR = true;
        CHECK(q[i].imageID == 7);
    }
    CHECK(sawTL && sawTR && sawBL && sawBR);
    t.reset();
}

// ---- 4. run-length inheritance (Rule 1) ----
// First cell carries (row=0,col=0); a following bare placeholder (no diacritics)
// with the same fg/underline inherits row=0, col=+1.
TEST(testInheritanceRunlength)
{
    auto t = makeTerm(24, 80);
    feedz(*t,
        "\033_Ga=T,U=1,i=5,f=24,s=4,v=2,c=4,r=1;"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\033\\");
    feedz(*t, "\033[H\033[38;5;5m");
    std::string first = placeholderCell({ 0x0305, 0x0305 }); // (0,0)
    std::string bare1 = placeholderCell({}); // inherits (0,1)
    std::string bare2 = placeholderCell({}); // inherits (0,2)
    std::string bare3 = placeholderCell({}); // inherits (0,3)
    feed(*t, first.c_str(), static_cast<int>(first.size()));
    feed(*t, bare1.c_str(), static_cast<int>(bare1.size()));
    feed(*t, bare2.c_str(), static_cast<int>(bare2.size()));
    feed(*t, bare3.c_str(), static_cast<int>(bare3.size()));

    Ubiquitty::Image::ImageQuad q[16];
    int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(16) });
    CHECK(n == 4); // a 4-column strip
    // Columns tile u in quarters; the four cells should be the 4 vertical strips.
    bool col[4] = { false, false, false, false };
    for (int i = 0; i < n; ++i) {
        int c = static_cast<int>(q[i].u0 * 4.0f + 0.25f);
        if (c >= 0 && c < 4)
            col[c] = true;
    }
    CHECK(col[0] && col[1] && col[2] && col[3]);
    t.reset();
}

// ---- 5. high image id via the MSB (third) diacritic ----
// imageID 33554474 = 42 + (2 << 24): fg low byte = 42 (256-color), 3rd diacritic
// = index 2 supplies the MSB. The virtual image carries id 33554474.
TEST(testHighImageIdMsb)
{
    auto t = makeTerm(24, 80);
    // 33554474 won't fit a 256-color fg, so transmit the image with that exact id
    // and reference it with fg=42 + MSB diacritic 2.
    feedz(*t, "\033_Ga=T,U=1,i=33554474,f=24,s=2,v=2,c=1,r=1;AAAAAAAAAAAAAAAA\033\\");
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->imageID == 33554474u && img->virtualPlacement);

    feedz(*t, "\033[H\033[38;5;42m"); // low byte 42
    // row=0 (U+0305), col=0 (U+0305), MSB=2 (U+030E).
    std::string cell = placeholderCell({ 0x0305, 0x0305, 0x030E });
    feed(*t, cell.c_str(), static_cast<int>(cell.size()));

    Ubiquitty::Image::ImageQuad q[8];
    int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) });
    CHECK(n == 1);
    if (n == 1)
        CHECK(q[0].imageID == 33554474u); // 42 | (2 << 24)
    t.reset();
}

// ---- 6. truecolor foreground encodes a 24-bit id ----
TEST(testTruecolorID)
{
    auto t = makeTerm(24, 80);
    // id 0x0A0B0C = 658188.
    feedz(*t, "\033_Ga=T,U=1,i=658188,f=24,s=2,v=2,c=1,r=1;AAAAAAAAAAAAAAAA\033\\");
    feedz(*t, "\033[H\033[38;2;10;11;12m"); // RGB 10,11,12 = 0x0A0B0C
    std::string cell = placeholderCell({ 0x0305, 0x0305 });
    feed(*t, cell.c_str(), static_cast<int>(cell.size()));
    Ubiquitty::Image::ImageQuad q[8];
    int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) });
    CHECK(n == 1);
    if (n == 1)
        CHECK(q[0].imageID == 658188u);
    t.reset();
}

// ---- 7. no virtual image → no quad (placeholder cells ignored) ----
TEST(testNoVirtualImage)
{
    auto t = makeTerm(24, 80);
    feedz(*t, "\033[H\033[38;5;9m");
    std::string cell = placeholderCell({ 0x0305, 0x0305 });
    feed(*t, cell.c_str(), static_cast<int>(cell.size()));
    CHECK(!Ubiquitty::Kitty::hasPlaceholders(t.get(), 0)); // no virtual placement exists
    Ubiquitty::Image::ImageQuad q[8];
    int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) });
    CHECK(n == 0);
    t.reset();
}

// Pre-rowspan reference: the per-cell placeholder scan (visibleCell per column,
// same U+10EEEE predicate). hasPlaceholders' row-span scan must agree with this
// for every scenario, so each case cross-checks against it in addition to a
// literal expectation.
static bool referenceHasPlaceholders(const Ubiquitty::Terminal& t, int viewOffset)
{
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < t.columns; ++c) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, viewOffset, r, c);
            if (cell && cell->codePoint == Ubiquitty::Kitty::placeholderCodePoint)
                return true;
        }
    return false;
}

// ---- 8. hasPlaceholders detection parity: live screen, scrollback, absent,
// and adjacent to a wide cell. A virtual placement exists in every case so the
// "any virtual placement" gate is open and the row-span scan itself is exercised.
TEST(testHasPlaceholdersDetection)
{
    // (a) placeholder on the live screen.
    {
        auto t = makeTerm(24, 80);
        feedz(*t, "\033_Ga=T,U=1,i=31,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
        feedz(*t, "\033[H\033[38;5;31m");
        std::string cell = placeholderCell({ 0x0305, 0x0305 });
        feed(*t, cell.c_str(), static_cast<int>(cell.size()));
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), 0) == true);
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), 0) == referenceHasPlaceholders(*t, 0));
        t.reset();
    }

    // (b) placeholder aged into scrollback: false at viewOffset 0 (blank live
    // screen), true when scrolled back to the archived row.
    {
        auto t = makeTerm(24, 80);
        feedz(*t, "\033_Ga=T,U=1,i=31,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
        feedz(*t, "\033[H\033[38;5;31m");
        std::string cell = placeholderCell({ 0x0305, 0x0305 });
        feed(*t, cell.c_str(), static_cast<int>(cell.size()));
        for (int i = 0; i < 30; ++i)
            feedz(*t, "\n"); // scroll the placeholder row into history
        const int vo = Ubiquitty::Screen::scrollbackCount(t.get()); // shows oldest line (the placeholder) at row 0
        CHECK(vo > 0);
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), 0) == false);
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), 0) == referenceHasPlaceholders(*t, 0));
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), vo) == true);
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), vo) == referenceHasPlaceholders(*t, vo));
        t.reset();
    }

    // (c) virtual placement present but no placeholder cell → false (scan runs,
    // finds nothing).
    {
        auto t = makeTerm(24, 80);
        feedz(*t, "\033_Ga=T,U=1,i=31,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
        feedz(*t, "\033[Hplain text, no placeholder");
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), 0) == false);
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), 0) == referenceHasPlaceholders(*t, 0));
        t.reset();
    }

    // (d) placeholder immediately after a wide (double-width) cell: the wide
    // continuation cell is not a placeholder, the following cell is.
    {
        auto t = makeTerm(24, 80);
        feedz(*t, "\033_Ga=T,U=1,i=31,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
        feedz(*t, "\033[H\344\270\200"); // U+4E00, a double-width CJK glyph
        feedz(*t, "\033[38;5;31m");
        std::string cell = placeholderCell({ 0x0305, 0x0305 });
        feed(*t, cell.c_str(), static_cast<int>(cell.size()));
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), 0) == true);
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t.get(), 0) == referenceHasPlaceholders(*t, 0));

        // Same wide glyph with no placeholder → false.
        auto t2 = makeTerm(24, 80);
        feedz(*t2, "\033_Ga=T,U=1,i=31,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
        feedz(*t2, "\033[H\344\270\200 wide only");
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t2.get(), 0) == false);
        CHECK(Ubiquitty::Kitty::hasPlaceholders(t2.get(), 0) == referenceHasPlaceholders(*t2, 0));
        t2.reset();
        t.reset();
    }
}

// ---- 9. same-id re-transmit bumps contentGeneration (regression) ----
// The single-id animation pattern: re-transmit image id 1 with the same
// 2x2/f=24 geometry but different pixel bytes (solid red → solid blue). The Core
// model refills the pixels, and the GPU texture cache is validated by
// imageContentStamp, which folds contentGeneration beside geometry/frame/format:
// a stamp of geometry alone aliases frame-to-frame and the renderer freezes on
// frame 1. This pins that a re-transmit moves the generation (so the stamp
// changes and the texture re-uploads).
TEST(testSameIdRetransmitBumpsGeneration)
{
    auto t = makeTerm(24, 80);
    // 2x2 RGB solid red: FF0000 x4 → base64 "/wAA" x4 (uncompressed → stored
    // synchronously, no decode queue drain needed).
    feedz(*t, "\033_Ga=T,i=1,f=24,s=2,v=2;/wAA/wAA/wAA/wAA\033\\");
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->imageID == 1 && img->width == 2 && img->height == 2 && img->format == 24);
    const uint64_t gen1 = img ? img->contentGeneration : 0;
    CHECK(gen1 != 0); // nonzero on first store

    // Re-transmit id 1, identical geometry/format, solid blue: 0000FF x4 →
    // base64 "AAD/" x4. Same slot is reused (kittyFindImage → dispose → refill).
    feedz(*t, "\033_Ga=T,i=1,f=24,s=2,v=2;AAD/AAD/AAD/AAD/\033\\");
    const Ubiquitty::Image::TerminalImage* img2 = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img2 && img2->imageID == 1);
    // Geometry/format/frame are byte-identical — the exact case a
    // geometry-only stamp cannot distinguish.
    CHECK(img2 && img2->width == 2 && img2->height == 2 && img2->format == 24 && img2->currentFrame == 0);
    const uint64_t gen2 = img2 ? img2->contentGeneration : 0;
    CHECK(gen2 != gen1); // the re-transmit moved the generation
    CHECK(gen2 > gen1); // monotonic source

    // And the stamp the renderer keys on differs across the two transmits.
    using Ubiquitty::Image::imageContentStamp;
    CHECK(imageContentStamp(2, 2, 0, 24, gen1) != imageContentStamp(2, 2, 0, 24, gen2));
    t.reset();
}

UBIQUITTY_TEST_MAIN()

// ---- chafa's wire shape ----
// Everything above transmits in ONE APC block. chafa does not: it sends a
// control-only opener (no ';', `m=1`) carrying every key — a=T,U=1,q=2,f=32,
// s,v,c,r,i — then the pixels as `m=1` payload chunks, closed by an `m=0`
// chunk carrying the tail. The grid (c,r) travels on the opener alone, never
// on the payload chunks, so these cases pin the chunk-control merge: lose it
// and every placeholder cell derives its own grid, the top-left cell showing
// the whole image — which on screen is a screenful of copies, not a picture.

// Base64 without padding stripping — the kitty payload alphabet.
static std::string base64Bytes(const std::string& raw)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    const auto b = [&](std::size_t k) { return static_cast<uint32_t>(static_cast<unsigned char>(raw[k])); };
    for (; i + 3 <= raw.size(); i += 3) {
        const uint32_t triple = (b(i) << 16) | (b(i + 1) << 8) | b(i + 2);
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        out += alphabet[(triple >> 6) & 0x3F];
        out += alphabet[triple & 0x3F];
    }
    if (i + 1 == raw.size()) {
        const uint32_t one = b(i) << 16;
        out += alphabet[(one >> 18) & 0x3F];
        out += alphabet[(one >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == raw.size()) {
        const uint32_t two = (b(i) << 16) | (b(i + 1) << 8);
        out += alphabet[(two >> 18) & 0x3F];
        out += alphabet[(two >> 12) & 0x3F];
        out += alphabet[(two >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

// Feed a chafa-shaped chunked virtual transmit: control-only opener, payload
// chunks of `chunkLength` base64 characters, tail on the m=0 chunk.
static void feedChunkedVirtualTransmit(Ubiquitty::Terminal& t, const char* openerKeys, const std::string& rgba, std::size_t chunkLength)
{
    feedz(t, ("\033_G" + std::string(openerKeys) + "\033\\").c_str());
    const std::string encoded = base64Bytes(rgba);
    std::size_t at = 0;
    while (encoded.size() - at > chunkLength) {
        feedz(t, ("\033_Gm=1;" + encoded.substr(at, chunkLength) + "\033\\").c_str());
        at += chunkLength;
    }
    feedz(t, ("\033_Gm=0;" + encoded.substr(at) + "\033\\").c_str());
}

// The virtual grid survives the chunk-control merge: c,r arrive on the opener
// only, and the image the m=0 chunk completes must still carry them.
TEST(testChunkedTransmitKeepsTheVirtualGrid)
{
    auto t = makeTerm(24, 80);
    const std::string rgba(static_cast<std::size_t>(8) * 8 * 4, '\x7f');
    feedChunkedVirtualTransmit(*t, "a=T,U=1,q=2,f=32,s=8,v=8,c=2,r=2,i=1,m=1", rgba, 64);

    CHECK(Ubiquitty::Image::placementCount(t.get()) == 0); // virtual: no grid placement
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->used && img->imageID == 1);
    CHECK(img && img->virtualPlacement);
    CHECK(img && img->virtualColumns == 2 && img->virtualRows == 2);
    t.reset();
}

// A full placeholder grid after a chunked transmit draws the image a single
// time, partitioned across four cells, four quads, each sampling its own
// quarter, not the whole image redrawn four times. The failure this pins is
// every quad sampling 0..1 — four whole copies.
TEST(testChunkedPlaceholderGridPartitionsNotCopies)
{
    auto t = makeTerm(24, 80);
    const std::string rgba(static_cast<std::size_t>(8) * 8 * 4, '\x7f');
    feedChunkedVirtualTransmit(*t, "a=T,U=1,q=2,f=32,s=8,v=8,c=2,r=2,i=1,m=1", rgba, 64);

    // Two rows of two placeholder cells, chafa's own layout: fg carries the id,
    // each cell carries (row, column) diacritics, CR+LF between rows.
    feedz(*t, "\033[H\033[38;5;1m");
    const uint32_t rowDiacritic[2] = { 0x0305, 0x030D };
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            std::string cell = placeholderCell({ rowDiacritic[row], rowDiacritic[column] });
            feed(*t, cell.c_str(), static_cast<int>(cell.size()));
        }
        feedz(*t, "\r\n");
    }

    Ubiquitty::Image::ImageQuad q[8];
    const int n = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) });
    CHECK(n == 4);
    if (n == 4) {
        // Each quad samples its own quarter; together they tile 0..1 exactly.
        for (int i = 0; i < 4; ++i) {
            CHECK(q[i].u1 - q[i].u0 == 0.5f);
            CHECK(q[i].v1 - q[i].v0 == 0.5f);
        }
        // No two quads sample the same quarter (the "N copies" failure).
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j)
                CHECK(q[i].u0 != q[j].u0 || q[i].v0 != q[j].v0);
    }
    t.reset();
}

// Placeholder cells are cells: they scroll with their rows, and the image they
// point at survives the scroll. viewOffset 0 shows what is on screen; stepping
// back into scrollback shows the rows that left.
TEST(testPlaceholdersScrollWithTheirRowsAndTheImageSurvives)
{
    auto t = makeTerm(4, 20); // small screen so one LF scrolls
    const std::string rgba(static_cast<std::size_t>(8) * 8 * 4, '\x7f');
    feedChunkedVirtualTransmit(*t, "a=T,U=1,q=2,f=32,s=8,v=8,c=2,r=2,i=1,m=1", rgba, 64);

    feedz(*t, "\033[H\033[38;5;1m");
    const uint32_t rowDiacritic[2] = { 0x0305, 0x030D };
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            std::string cell = placeholderCell({ rowDiacritic[row], rowDiacritic[column] });
            feed(*t, cell.c_str(), static_cast<int>(cell.size()));
        }
        feedz(*t, "\r\n");
    }
    Ubiquitty::Image::ImageQuad q[8];
    CHECK(Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) }) == 4);

    // Drive the cursor to the last row and scroll by one: the top placeholder
    // row enters scrollback, the bottom one moves up a row.
    feedz(*t, "\033[4;1H\n");
    const int onScreen = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 0, { q, static_cast<std::size_t>(8) });
    CHECK(onScreen == 2);
    // One step back into scrollback, the full grid is whole again.
    const int scrolledBack = Ubiquitty::Rendering::collectImageQuads(t.get(), { 10.0f, 20.0f }, { 0.0f, 0.0f }, 1.0f, 1, { q, static_cast<std::size_t>(8) });
    CHECK(scrolledBack == 4);

    // The image itself outlives unrelated output: nothing here deletes it, so
    // an eviction on this path is the "picture vanishes when I type" bug.
    for (int i = 0; i < 64; ++i)
        feedz(*t, "unrelated output line\r\n");
    const Ubiquitty::Image::TerminalImage* img = Ubiquitty::Image::stored(t.get(), 0);
    CHECK(img && img->used && img->virtualPlacement && img->imageID == 1);
    t.reset();
}

// Whether two quads draw the same pixels from the same texels.
static bool isSameQuad(const Ubiquitty::Image::ImageQuad& a, const Ubiquitty::Image::ImageQuad& b)
{
    return a.minXInPixels == b.minXInPixels && a.minYInPixels == b.minYInPixels
        && a.maxXInPixels == b.maxXInPixels && a.maxYInPixels == b.maxYInPixels
        && a.u0 == b.u0 && a.v0 == b.v0 && a.u1 == b.u1 && a.v1 == b.v1
        && a.zIndex == b.zIndex && a.straightAlpha == b.straightAlpha
        && a.filter == b.filter && a.imageID == b.imageID;
}

// The rows a frame copies out of its terminal place every placeholder where
// the terminal itself does. The scene carries each rule the decode follows:
// row and column marks, a pooled cluster the copy reads back through its
// reader, bare cells inheriting the next column, a third mark naming the
// id's high byte, and a 2x2 grid split across two rows. Read live and read
// from the copies, the quads are the same quads in the same order, at the
// live tail and one row back in the scrollback, and a capacity short of the
// count keeps the first quads and stops.
TEST(testACapturedGridPlacesPlaceholdersAsTheTerminalDoes)
{
    auto t = makeTerm(5, 12);
    feedz(*t, "\033_Ga=T,U=1,i=5,f=24,s=4,v=2,c=4,r=1;AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\033\\");
    feedz(*t, "\033_Ga=T,U=1,i=33554474,f=24,s=2,v=2,c=1,r=1;AAAAAAAAAAAAAAAA\033\\");
    feedz(*t, "\033_Ga=T,U=1,i=9,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");

    // Row 1: a named first cell, then three bare cells inheriting columns 1-3.
    feedz(*t, "\033[H\033[38;5;5m");
    std::string text = placeholderCell({ 0x0305, 0x0305 });
    for (int bare = 0; bare < 3; ++bare)
        text += placeholderCell({});
    // Row 2: low byte 42 in the foreground, high byte 2 in the third mark.
    text += "\033[2;1H\033[38;5;42m" + placeholderCell({ 0x0305, 0x0305, 0x030E });
    // Rows 3 and 4: image 9's 2x2 grid.
    text += "\033[3;1H\033[38;5;9m" + placeholderCell({ 0x0305, 0x0305 }) + placeholderCell({ 0x0305, 0x030D });
    text += "\033[4;1H" + placeholderCell({ 0x030D, 0x0305 }) + placeholderCell({ 0x030D, 0x030D });
    // Row 5, then one line feed scrolls row 1 into the scrollback.
    text += "\033[0m\033[5;1H\n";
    feed(*t, text.c_str(), static_cast<int>(text.size()));

    const auto readCluster = [](const Ubiquitty::Cell* cell, std::span<uint32_t> codePoints, void* context) {
        return Ubiquitty::cellClusterCodePoints(static_cast<const Ubiquitty::Terminal*>(context), cell, codePoints.data(), static_cast<int>(codePoints.size()));
    };
    const Ubiquitty::FloatSize cell { 10.0f, 20.0f };
    const Ubiquitty::FloatPoint origin { 3.0f, 5.0f };
    for (int viewOffset : { 0, 1 }) {
        Ubiquitty::Image::ImageQuad live[16];
        const int liveCount = Ubiquitty::Kitty::resolvePlaceholders(t.get(), cell, origin, 1.0f, viewOffset, { live, std::size(live) });

        std::vector<Ubiquitty::Cell> rows(static_cast<std::size_t>(t->rows) * t->columns);
        for (int row = 0; row < t->rows; ++row)
            Ubiquitty::Screen::getDisplayRow(*t, viewOffset, row, &rows[static_cast<std::size_t>(row) * t->columns], t->columns);
        const Ubiquitty::Kitty::PlaceholderGrid grid {
            .cells = rows,
            .size = { t->columns, t->rows },
            .readCluster = readCluster,
            .readClusterContext = t.get(),
        };
        Ubiquitty::Image::ImageQuad captured[16];
        const int capturedCount = Ubiquitty::Kitty::resolvePlaceholders(grid, t->images.stored, cell, origin, { captured, std::size(captured) });

        // The live tail has lost the first row's four cells to the scrollback;
        // one row back, they are on screen again beside the other five.
        CHECK(liveCount == (viewOffset == 0 ? 5 : 9));
        CHECK(capturedCount == liveCount);
        for (int index = 0; index < std::min(liveCount, capturedCount); ++index)
            CHECK(isSameQuad(live[index], captured[index]));

        Ubiquitty::Image::ImageQuad shortOf[2];
        CHECK(Ubiquitty::Kitty::resolvePlaceholders(grid, t->images.stored, cell, origin, { shortOf, std::size(shortOf) }) == 2);
        CHECK(isSameQuad(shortOf[0], live[0]) && isSameQuad(shortOf[1], live[1]));
    }
    t.reset();
}

// A copy with no reader for pooled clusters cannot see a pooled cell's
// marks: its inline buffer keeps only the first, the row. Placed from that,
// the second cell of a row would inherit a column from a first cell whose
// column was never read, and the image would draw shifted by its guess. The
// copy draws no tile instead.
TEST(testACapturedGridWithoutAClusterReaderPlacesNoGuessedTile)
{
    auto t = makeTerm(2, 8);
    feedz(*t, "\033_Ga=T,U=1,i=9,f=24,s=2,v=2,c=2,r=2;AAAAAAAAAAAAAAAA\033\\");
    feedz(*t, "\033[H\033[38;5;9m");
    const std::string cells = placeholderCell({ 0x0305, 0x0305 }) + placeholderCell({ 0x0305, 0x030D })
        + "\033[2;1H" + placeholderCell({ 0x030D, 0x0305 }) + placeholderCell({ 0x030D, 0x030D });
    feed(*t, cells.c_str(), static_cast<int>(cells.size()));

    std::vector<Ubiquitty::Cell> rows(static_cast<std::size_t>(t->rows) * t->columns);
    for (int row = 0; row < t->rows; ++row)
        Ubiquitty::Screen::getDisplayRow(*t, 0, row, &rows[static_cast<std::size_t>(row) * t->columns], t->columns);
    const Ubiquitty::Kitty::PlaceholderGrid grid {
        .cells = rows,
        .size = { t->columns, t->rows },
    };
    Ubiquitty::Image::ImageQuad quads[4];
    CHECK(Ubiquitty::Kitty::resolvePlaceholders(grid, t->images.stored, { 10.0f, 20.0f }, {}, { quads, std::size(quads) }) == 0);
    t.reset();
}
