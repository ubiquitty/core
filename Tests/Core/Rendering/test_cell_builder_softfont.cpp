// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_cell_builder_softfont — what a DECDLD cell draws when the backend cannot
// stage it, and what the cache is left holding afterwards.
//
// A soft-font glyph is the one glyph Core rasterises itself: the matrix arrives
// off the wire, so there is no code point to look up and the backend's whole
// obligation is to move the finished plane into a slot
// (`GlyphBackend::stageMonoCoverage`). Four frontends implement that seam, and
// the question this file settles is what happens in the one that has not.
//
// The PIXEL gate cannot ask any of this. The builder-frames gate compares
// the portable builder against a second implementation, and both draw blank for an
// absent DRCS glyph — as does the defect. Removing both the absence test and the
// unwritten-slot bail leaves that script passing, because a slot nobody claimed
// is zeroed and reads as blank. The state that separates them is not in the
// frame; it is in the CACHE, which is why the assertions below are about
// residency rather than about pixels.

#include "Harness.h"
#include "StrictAtlas.h"

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/GlyphKey.h"
#include "Core/Rendering/GlyphSlotCache.h"
#include "Core/RowMeta.h"
#include "Core/TermPalette.h"
#include "Core/TextSizeTable.h"
#include "Core/VT/SoftFont.h"

#include <algorithm>
#include <span>
#include <vector>

using namespace Ubiquitty::Rendering;

namespace {

constexpr int rows = 1;
constexpr int columns = 4;
constexpr int atlasColumns = 8;
constexpr int atlasCapacity = atlasColumns * 8;
constexpr int slotWidth = 8;
constexpr int slotHeight = 16;
// The soft font's own serial. Any nonzero value: it only has to be the one the
// key is built from, and building the key here from a literal rather than from
// the font would let the two drift apart without the test noticing.
constexpr std::uint32_t fontSerial = 7;

// Whether the fixture models a frontend that can stage a Core-rasterised plane.
enum class Staging {
    Refuses,
    Accepts,
};

// A mono atlas that records what it was asked to stage, and can refuse.
//
// Refusing is not a fault injection — it is the state every frontend is in
// before it implements the entry, and the state `declineMonoCoverage` puts a
// backend in permanently. The fixture models it so the case can assert what the
// builder does about it.
struct SoftFontAtlas : UbiquittyTest::StrictAtlas {
    GlyphSlotCache cache { atlasCapacity, recommendedHashBuckets(atlasCapacity), atlasColumns };
    GlyphSlotGeometry geometry = [] {
        GlyphSlotGeometry g {};
        g.columns = atlasColumns;
        g.slotWidthInPixels = slotWidth;
        g.slotHeightInPixels = slotHeight;
        g.glyphWidthInPixels = slotWidth;
        g.glyphMarginInPixels = { 0, 1 };
        return g;
    }();
    Staging staging = Staging::Accepts;
    int stageCalls = 0;
    int lastStagedCount = 0;
    bool sawInk = false;

    void beginFrame() { cache.beginFrame(); }
    MonoAtlasBinding bindMonoAtlas()
    {
        MonoAtlasBinding binding;
        binding.cache = &cache;
        binding.geometry = geometry;
        binding.slotCapacity = atlasCapacity;
        binding.weight = 0u;
        return binding;
    }
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }
    AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t) { return {}; }
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }

    bool stageMonoCoverage(int, std::span<const std::uint8_t> coverage)
    {
        ++stageCalls;
        lastStagedCount = static_cast<int>(coverage.size());
        sawInk = std::ranges::any_of(coverage, [](std::uint8_t byte) { return byte != 0; });
        return staging == Staging::Accepts;
    }
};

// A backend that never heard of soft fonts: it declares no staging entry at all,
// so `glyphBackendFor` leaves `stageMonoCoverage` at its default. This is the
// shape of every frontend that has not implemented the seam, and the reason the
// default has to be safe rather than null.
struct UnawareAtlas : SoftFontAtlas {
    bool stageMonoCoverage(int, std::span<const std::uint8_t>) = delete;
};

// One 8x8 soft font with cell 0 defined solid and every other cell undefined.
Ubiquitty::VT::SoftFont solidFirstCell()
{
    Ubiquitty::VT::SoftFont font {};
    font.valid = true;
    font.serial = fontSerial;
    font.matrixWidth = 8;
    font.matrixHeight = 8;
    font.glyphSet[0] = true;
    for (int row = 0; row < 8; ++row)
        font.glyph[0][row] = 0xFF;
    return font;
}

// A row whose first cell is a DEFINED soft-font position and whose second is an
// UNDEFINED one, so one build answers both questions and neither can pass by
// being the only case present.
struct Scene {
    Ubiquitty::TermPalette palette;
    Ubiquitty::VT::SoftFont font = solidFirstCell();
    std::vector<Ubiquitty::Cell> cells { static_cast<std::size_t>(rows) * columns, Ubiquitty::defaultBlankCell };
    std::vector<CellInstance> cellsOut { static_cast<std::size_t>(rows) * columns };
    BuildInputs in;
    InstanceBuffers out;

    Scene()
    {
        palette.seedDefaults();
        cells[0].codePoint = Ubiquitty::VT::drcsCodePointBase + 0;
        cells[1].codePoint = Ubiquitty::VT::drcsCodePointBase + 1;
        in.cells = cells.data();
        in.rows = rows;
        in.columns = columns;
        in.palette = &palette;
        in.softFont = &font;
        in.metrics.cellWidth = static_cast<float>(slotWidth);
        in.metrics.cellHeight = static_cast<float>(slotHeight);
        in.metrics.baselineFromTop = 13.0f;
        out.cells = cellsOut.data();
        out.cellCapacity = cellsOut.size();
    }
};

// Whether a built cell carries any glyph at all. A blank cell is emitted with a
// degenerate rect rather than left out, so the count is no answer to this.
bool isCellInked(const CellInstance& instance)
{
    return instance.atlasBottomRight.x > instance.atlasTopLeft.x
        && instance.atlasBottomRight.y > instance.atlasTopLeft.y;
}

} // namespace

TEST(softFontCellDrawsThroughTheStagingEntry)
{
    Scene scene;
    SoftFontAtlas atlas;
    buildCellInstances(scene.in, UbiquittyTest::glyphBackendFor(atlas), scene.out);

    CHECK_MSG(atlas.stageCalls == 1,
        "a defined DRCS position stages once, and an undefined one never reaches the backend");
    CHECK_MSG(atlas.lastStagedCount == slotWidth * slotHeight,
        "the plane handed over is the slot's own size, which is what lets a backend refuse a wrong one");
    CHECK_MSG(atlas.sawInk,
        "Core rasterises the matrix before staging it; an all-zero plane means the raster never ran");
    CHECK_MSG(isCellInked(scene.cellsOut[0]), "a defined soft-font position draws its glyph");
    CHECK_MSG(!isCellInked(scene.cellsOut[1]),
        "an undefined soft-font position draws BLANK, not the character at that code point");
}

TEST(softFontBlanksWhenTheBackendCannotStage)
{
    // The frontend that has not implemented the entry, which is where every
    // backend starts. The cell must lose its glyph rather than gain a wrong one:
    // a slot the builder claimed and nobody filled still holds whatever the
    // eviction before it left there, and blitting that draws another glyph's ink
    // under this cell's colours with every count in the frame correct.
    Scene refusing;
    SoftFontAtlas refuser;
    refuser.staging = Staging::Refuses;
    buildCellInstances(refusing.in, UbiquittyTest::glyphBackendFor(refuser), refusing.out);
    CHECK_MSG(refuser.stageCalls == 1, "the builder does reach the entry, so the blank below is its answer");
    CHECK_MSG(!isCellInked(refusing.cellsOut[0]), "a refused stage draws blank");

    // The same, through the DEFAULT rather than through a refusal, which is the
    // case no frontend opts into. `UnawareAtlas` declares no staging entry, so
    // the table carries `declineMonoCoverage` — and the cell has to reach the
    // same blank by that route, or the safe default is only safe for backends
    // that already thought about it.
    Scene unaware;
    UnawareAtlas ignorant;
    buildCellInstances(unaware.in, UbiquittyTest::glyphBackendFor(ignorant), unaware.out);
    CHECK_MSG(!isCellInked(unaware.cellsOut[0]),
        "a backend that never declared the entry draws blank, not the character at that code point");
}

TEST(softFontAliasedKeyRerasterises)
{
    // `drcsGlyphKey` keeps 22 bits of serial, and the atlas is shared across
    // panes, so two soft fonts can name one slot — about 100MB of terminal output
    // walks the counter round and nothing rate-limits it. The glyph that comes
    // back has to be this font's.
    //
    // Both scenes use serials that collide under the mask and carry DIFFERENT
    // bitmaps, on one shared atlas. Keying on the serial alone, the second build
    // is a hit and paints the first font's glyph.
    constexpr std::uint32_t aliasingSerial = fontSerial + (1u << 22);
    CHECK_MSG(drcsGlyphKey(fontSerial, 0u) == drcsGlyphKey(aliasingSerial, 0u),
        "the two serials must actually collide, or this case proves nothing");

    SoftFontAtlas shared;

    Scene first;
    buildCellInstances(first.in, UbiquittyTest::glyphBackendFor(shared), first.out);
    CHECK_MSG(shared.stageCalls == 1, "the first font rasterises");

    // Same key, different font: a hollow glyph rather than a solid one.
    Scene second;
    second.font.serial = aliasingSerial;
    second.font.glyph[0].fill(0x81); // outer columns only
    shared.sawInk = false;
    buildCellInstances(second.in, UbiquittyTest::glyphBackendFor(shared), second.out);

    CHECK_MSG(shared.stageCalls == 2,
        "an aliased key re-rasterises: the contents decide, not the serial");
    CHECK_MSG(isCellInked(second.cellsOut[0]), "the aliasing font still draws");
}

TEST(softFontDownloadsDoNotAccumulateSlots)
{
    // Every committed DECDLD takes a fresh serial, so its glyphs key into the
    // SHARED mono atlas under names the previous font's rasterisations can never
    // be reached by again. Left resident they age out through the LRU competing
    // with ordinary text, and a stream of downloads evicts real glyphs that then
    // re-rasterise through the frontend's font engine — which costs far more than
    // the DRCS raster that displaced them.
    //
    // Naming the retired serial has to RECLAIM the slot, not merely unname it: a
    // slot dropped from the hash but left at its LRU position is still capacity
    // spent on pixels nobody can ask for.
    SoftFontAtlas shared;
    const int slotsBefore = shared.cache.used();

    Scene first;
    buildCellInstances(first.in, UbiquittyTest::glyphBackendFor(shared), first.out);
    const int slotsAfterFirst = shared.cache.used();
    CHECK_MSG(slotsAfterFirst == slotsBefore + 1, "the first download takes one slot");

    // Twenty more downloads, each a new serial with a different bitmap, each
    // retiring the one before it. Distinct bitmaps matter: identical ones would
    // verify as a hit and take no slot, so the case would pass without reclaiming
    // anything.
    std::uint32_t live = fontSerial;
    for (std::uint32_t generation = 1; generation <= 20; ++generation) {
        Scene next;
        next.font.serial = fontSerial + generation;
        next.font.glyph[0].fill(static_cast<std::uint16_t>(0xFF00u | generation));
        next.in.retiredSoftFontSerial = live;
        live = next.font.serial;
        buildCellInstances(next.in, UbiquittyTest::glyphBackendFor(shared), next.out);
        CHECK_MSG(isCellInked(next.cellsOut[0]), "every generation still draws its own glyph");
    }

    CHECK_MSG(shared.cache.used() == slotsAfterFirst,
        "a retired soft font's slot is reused, so downloads do not accumulate atlas capacity");
}

TEST(softFontRefusalLeavesNoSlotClaimed)
{
    // The assertion the pixel gate cannot make, and the one that separates
    // "blank because nothing was drawn" from "blank because the slot happened to
    // be zeroed".
    //
    // `slotForKey` reports a slot fresh exactly once. A builder that takes a slot,
    // fails to fill it and leaves the key resident draws blank on this frame and
    // blits the unwritten slot on every frame after — and by then the slot may
    // have held a real glyph. So the state to assert is residency: after a failed
    // stage the key must be gone, and the next lookup must come back fresh.
    Scene scene;
    SoftFontAtlas atlas;
    atlas.staging = Staging::Refuses;
    buildCellInstances(scene.in, UbiquittyTest::glyphBackendFor(atlas), scene.out);

    const std::uint32_t key = drcsGlyphKey(fontSerial, 0u);
    bool isFresh = false;
    atlas.cache.slotForKey(key, &isFresh);
    CHECK_MSG(isFresh, "a stage that failed unclaims its slot, so the next frame rasterises again");

    // The control: the same lookup after a stage that SUCCEEDED must report the
    // key resident. Without it, an assertion that "fresh is true" would hold just
    // as well for a builder that never touched the cache at all.
    Scene staged;
    SoftFontAtlas stager;
    buildCellInstances(staged.in, UbiquittyTest::glyphBackendFor(stager), staged.out);

    bool isStagedFresh = false;
    stager.cache.slotForKey(key, &isStagedFresh);
    CHECK_MSG(!isStagedFresh, "a stage that succeeded keeps its slot, so a resident glyph is not re-rasterised");
}

// An atlas with no room left for this frame drops the overflow rather than
// evicting a slot the frame already placed: every slot touched since
// beginFrame is holding pixels an instance already points at, so evicting one
// would corrupt a cell that is already recorded.
TEST(aSaturatedAtlasDropsTheOverflowRatherThanEvictingThisFrame)
{
    // Two slots, four distinct soft-font positions in one frame.
    Ubiquitty::VT::SoftFont font {};
    font.valid = true;
    font.serial = fontSerial;
    font.matrixWidth = 8;
    font.matrixHeight = 8;
    for (int index = 0; index < 4; ++index) {
        font.glyphSet[index] = true;
        for (int row = 0; row < 8; ++row)
            font.glyph[index][row] = static_cast<std::uint8_t>(0x0F + index);
    }

    struct TinyAtlas : SoftFontAtlas {
        GlyphSlotCache tiny { 2, recommendedHashBuckets(2), 1 };
        void beginFrame() { tiny.beginFrame(); }
        MonoAtlasBinding bindMonoAtlas()
        {
            MonoAtlasBinding binding;
            binding.cache = &tiny;
            // One column, so the two slots tile the sheet exactly: a capacity
            // that does not fill its columns leaves atlasRectForSlot degenerate and
            // every cell would read as blank for the wrong reason.
            binding.geometry = geometry;
            binding.geometry.columns = 1;
            binding.slotCapacity = 2;
            binding.weight = 0u;
            return binding;
        }
    } atlas;

    Ubiquitty::TermPalette palette;
    palette.seedDefaults();
    std::vector<Ubiquitty::Cell> cells(4, Ubiquitty::defaultBlankCell);
    for (int index = 0; index < 4; ++index)
        cells[static_cast<std::size_t>(index)].codePoint = Ubiquitty::VT::drcsCodePointBase + static_cast<std::uint32_t>(index);
    std::vector<CellInstance> cellsOut(4);
    BuildInputs in;
    in.cells = cells.data();
    in.rows = 1;
    in.columns = 4;
    in.palette = &palette;
    in.softFont = &font;
    in.metrics.cellWidth = static_cast<float>(slotWidth);
    in.metrics.cellHeight = static_cast<float>(slotHeight);
    in.metrics.baselineFromTop = 13.0f;
    InstanceBuffers out;
    out.cells = cellsOut.data();
    out.cellCapacity = cellsOut.size();

    const InstanceCounts counts = buildCellInstances(in, UbiquittyTest::glyphBackendFor(atlas), out);
    CHECK(counts.cells == 4); // every cell is emitted; two draw blank
    int inked = 0;
    for (const CellInstance& instance : cellsOut)
        if (isCellInked(instance))
            ++inked;
    CHECK(inked == 2); // exactly the two the atlas had room for
}

// A geometry whose slot has no area cannot hold a plane, so the glyph is
// refused and its key unclaimed — an atlas configured this way must draw
// blank, not stage a zero-length plane into a slot it then reports resident.
TEST(aSlotWithNoAreaRefusesTheGlyphAndUnclaimsItsKey)
{
    struct FlatAtlas : SoftFontAtlas {
        FlatAtlas()
        {
            geometry.slotWidthInPixels = 0;
            geometry.slotHeightInPixels = 0;
            geometry.glyphWidthInPixels = 0;
        }
    } atlas;

    Scene scene;
    const InstanceCounts counts = buildCellInstances(scene.in, UbiquittyTest::glyphBackendFor(atlas), scene.out);
    CHECK(counts.cells == static_cast<std::size_t>(rows) * columns);
    CHECK(!isCellInked(scene.cellsOut[0]));
    CHECK(atlas.stageCalls == 0); // nothing was staged into the empty slot

    // The key was unclaimed, so a later frame on a working geometry rasterises
    // rather than being told the glyph is resident.
    bool isFresh = false;
    const std::uint32_t key = Ubiquitty::Rendering::drcsGlyphKey(fontSerial, 0);
    atlas.cache.slotForKey(key, &isFresh);
    CHECK(isFresh);
}

TEST(fractionalSoftFontsUseTheDownloadedBitmapInsideTheDrawnArea)
{
    for (bool isDoubled : { false, true }) { // bool-ok: exercise both sides of the rendering fixture.
        struct Atlas : SoftFontAtlas {
            int sizedRequests = 0;
        } atlas;
        auto font = solidFirstCell();
        Ubiquitty::TermPalette palette;
        palette.seedDefaults();
        Ubiquitty::TextSizeTable table;
        Ubiquitty::TextSizeSpec spec { 2, 1 };
        spec.fractionNumerator = 1;
        spec.fractionDenominator = 2;
        spec.verticalAlign = spec.horizontalAlign = 2;
        const unsigned id = table.intern(spec);
        constexpr int blockRows = 2;
        constexpr int blockColumns = 8;
        std::vector<Ubiquitty::Cell> cells(blockRows * blockColumns, Ubiquitty::defaultBlankCell);
        for (int band = 0; band < blockRows; ++band) {
            for (int offset = 0; offset < 2; ++offset) {
                auto& cell = cells[static_cast<std::size_t>(band * blockColumns + 1 + offset)];
                cell.codePoint = offset == 0 ? Ubiquitty::VT::drcsCodePointBase : 0;
                cell.pen.attributes.add(offset == 0 ? Ubiquitty::VT::Attribute::Wide : Ubiquitty::VT::Attribute::WideContinuation);
                cell.pen.extendedAttributes.setTextSizeSpecID(id);
                Ubiquitty::penSetTextBand(&cell.pen, static_cast<std::uint8_t>(band));
            }
        }
        const std::uint8_t lineAttributes[] = { isDoubled ? Ubiquitty::lineAttributeDoubleWidth : Ubiquitty::lineAttributeNormal, Ubiquitty::lineAttributeNormal };
        BuildInputs in;
        in.cells = cells.data();
        in.rows = blockRows;
        in.columns = blockColumns;
        in.palette = &palette;
        in.textSizeTable = &table;
        in.softFont = &font;
        in.lineAttributes = lineAttributes;
        in.subpixelGlyphs = true;
        in.metrics.cellWidth = 8.0f;
        in.metrics.cellHeight = 16.0f;
        std::vector<CellInstance> cellInstances(64);
        std::vector<GlyphInstance> glyphs(16);
        InstanceBuffers out;
        out.cells = cellInstances.data();
        out.cellCapacity = cellInstances.size();
        out.glyphs = glyphs.data();
        out.glyphCapacity = glyphs.size();
        auto backend = UbiquittyTest::glyphBackendFor(atlas);
        backend.sizedGlyph = [](void* context, std::uint32_t, FontFace, std::uint32_t, std::uint32_t, GlyphPass, SizedTile) {
            ++static_cast<Atlas*>(context)->sizedRequests;
            return AtlasGlyph {};
        };
        const InstanceCounts counts = buildCellInstances(in, backend, out);
        REQUIRE(counts.glyphs == 2);
        CHECK(atlas.stageCalls == 1);
        CHECK(atlas.sawInk);
        CHECK(atlas.sizedRequests == 0);
        const float widthScale = isDoubled ? 2.0f : 1.0f;
        for (std::size_t index = 0; index < counts.glyphs; ++index) {
            CHECK(glyphs[index].originInPixels.x == 12.0f * widthScale);
            CHECK(glyphs[index].sizeInPixels.x == 8.0f * widthScale);
            CHECK(glyphs[index].sizeInPixels.y == 8.0f);
        }
        CHECK(glyphs[0].originInPixels.y == 8.0f);
        CHECK(glyphs[1].originInPixels.y == 16.0f);
        for (std::size_t index = 0; index < counts.cells; ++index)
            CHECK(!isCellInked(cellInstances[index]));
    }
}

UBIQUITTY_TEST_MAIN()
