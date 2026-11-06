// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_ligature_sheet_units — a strip rect is atlas pixels, not fractions.
//
// The builder ADDS a slice's atlas-pixel offsets to the strip rect's x — its
// own comment states the addition — so the rect's unit is part of the
// contract, and nothing else checks it: the software-pass tests stub their
// runs, the coverage asserts never read a UV, and a pixel oracle cannot see
// the mismatch because both render arms consume the same rect. Published as
// fractions, every slot's slices resolve near the sheet's origin and every
// ligatured run draws slot 0's ink — on a live session, whole prompt lines
// wearing another line's text, while slot 0's own row reads correct and hides
// the defect.
//
// The sheet is constructed without a rasterizer on purpose: geometry is
// decided at construction and `rectForSlot` must answer it whether or not the
// face arrived, which is exactly what lets this pin the unit with no font on
// the machine.

#include "Harness.h"

#include "Core/Rendering/LigatureSheet.h"

#include <algorithm>
#include <vector>

namespace {

using namespace Ubiquitty::Rendering;

constexpr int cellWidth = 10;
constexpr int cellHeight = 21;
constexpr int marginX = 2;
constexpr int marginY = 2;

// What a scripted face does to a multi-character run — the one axis the
// strip rule turns on.
enum class FaceSubstitution : std::uint8_t {
    // The common monospace face: one glyph per character, clusters in order,
    // ids the isolated mapping's — what a path or a flag shapes to on a font
    // carrying no coding ligatures.
    None,
    // A coding ligature drawn one for one, the shape JetBrains Mono gives
    // `=>`: count and clusters untouched, ids changed.
    OneForOne,
};

// A face that shapes, with substitution under the caller's control.
class ScriptedFace final : public GlyphRasterizer {
public:
    explicit ScriptedFace(FaceSubstitution substitution)
        : m_substitution(substitution)
    {
    }

    [[nodiscard]] CellMetrics cellMetrics() const override { return { cellWidth, cellHeight, cellHeight - 4 }; }

    bool rasterizeGlyph(std::uint32_t, FontFace, std::uint32_t, int, int, std::uint8_t*) override { return true; }

    [[nodiscard]] bool hasLigatureShaping() const override { return true; }

    int shapeTextRun(std::span<const std::uint32_t> codePoints, std::uint32_t, std::span<ShapedGlyph> glyphsOut) override
    {
        const int count = static_cast<int>(std::min(codePoints.size(), glyphsOut.size()));
        for (int i = 0; i < count; ++i) {
            ShapedGlyph glyph;
            // One for one, count and clusters untouched, which is how
            // JetBrains Mono draws `=>`: each character becomes a piece of
            // the joined image. Substitution shows only in the glyph id, so
            // a run gate that counts glyphs or reads clusters cannot see it.
            const bool isSubstituted = m_substitution == FaceSubstitution::OneForOne && count > 1;
            glyph.glyph = codePoints[static_cast<std::size_t>(i)] + (isSubstituted ? substitutionBias : 0u);
            glyph.advanceInPixels = cellWidth;
            glyph.cluster = i;
            glyphsOut[static_cast<std::size_t>(i)] = glyph;
        }
        return count;
    }

    // The offset a substituted glyph carries, so an isolated mapping — one
    // character, count 1 — answers the unsubstituted id and the comparison
    // has something to differ from.
    static constexpr unsigned int substitutionBias = 1000u;

    bool rasteriseShapedRun(std::span<const ShapedGlyph>, int, int, std::span<std::uint8_t>) override
    {
        ++rasterCalls;
        return true;
    }

    // Measured, so the sheet can derive identities: every glyph draws a
    // cell's width of ink at its own offset, and no glyph is blank.
    bool rasteriseShapedRunMeasured(std::span<const ShapedGlyph> glyphs, int, int, std::span<std::uint8_t>,
        std::span<GlyphInkExtent> inkOut) override
    {
        ++rasterCalls;
        for (std::size_t i = 0; i < glyphs.size() && i < inkOut.size(); ++i) {
            inkOut[i].leftInPixels = glyphs[i].offsetXInPixels;
            inkOut[i].rightInPixels = glyphs[i].offsetXInPixels + cellWidth;
        }
        return true;
    }

    int rasterCalls = 0;

private:
    FaceSubstitution m_substitution;
};

// The distilled row the sheet walks, spelled the way the builder hands it over.
std::vector<LigatureCell> operatorRow(const char* text)
{
    std::vector<LigatureCell> row;
    for (const char* c = text; *c != 0; ++c) {
        LigatureCell cell;
        cell.codePoint = static_cast<std::uint32_t>(static_cast<unsigned char>(*c));
        cell.shape = 1u; // one pen for the whole row, so no shape boundary splits it
        row.push_back(cell);
    }
    return row;
}

TEST(testStripRectIsAtlasPixels)
{
    // Over a shaping face, because a sheet is planned only for one: a face
    // that cannot shape gets a sheet with no slots at all, which
    // `test_ligature_sheet_without_shaper` states and this case has no
    // opinion about. The subject here is the unit its rects are in.
    ScriptedFace face(FaceSubstitution::None);
    LigatureSheet sheet(cellWidth, cellHeight, marginX, marginY, &face);
    const AtlasGlyph first = sheet.rectForSlot(0);
    const AtlasGlyph second = sheet.rectForSlot(1);
    CHECK(first.present);
    CHECK(second.present);
    // Slot 0 starts at the origin either way; slot 1's x is where the units
    // show. A fractional rect puts it below 1, a pixel rect a whole slot out
    // — and a slot is at least one cell plus margins wide.
    CHECK(first.minUV.x == 0.0f);
    CHECK(second.minUV.x >= static_cast<float>(cellWidth));
    // The slot's extent is its width in the same unit.
    CHECK(second.maxUV.x - second.minUV.x >= static_cast<float>(cellWidth));
    CHECK(first.maxUV.y - first.minUV.y >= static_cast<float>(cellHeight));
}

TEST(testConsecutiveSlotsTile)
{
    // A shaping face for the same reason the case above needs one: without
    // slots there is no tiling to check, and the assertion below passes on
    // two absent rects without touching the geometry it is here for.
    ScriptedFace face(FaceSubstitution::None);
    LigatureSheet sheet(cellWidth, cellHeight, marginX, marginY, &face);
    const AtlasGlyph first = sheet.rectForSlot(0);
    const AtlasGlyph second = sheet.rectForSlot(1);
    // Adjacent slots on one sheet row abut: slot 1 begins where slot 0 ends,
    // in the shared unit. A unit mismatch between the two breaks this even
    // when each rect is internally consistent.
    CHECK(second.minUV.x == first.maxUV.x);
}

TEST(testAFaceThatChangesNothingPublishesNoStrip)
{
    // `ls -la` on a face with no coding ligatures: the gated walk qualifies the
    // stretch because `-` is an operator character, and the face then shapes it
    // glyph for glyph. Publishing a strip there would draw the same characters
    // from a different raster than the columns beside it, which is what moves a
    // prompt sideways the moment an operator is typed.
    ScriptedFace face(FaceSubstitution::None);
    LigatureSheet sheet(cellWidth, cellHeight, marginX, marginY, &face);
    CHECK(sheet.isUsable());
    const std::vector<LigatureCell> row = operatorRow("ls -la");
    std::vector<LigatureRun> runs(row.size());
    sheet.shapeRow(std::span<const LigatureCell>(row.data(), row.size()), static_cast<float>(cellWidth),
        std::span<LigatureRun>(runs.data(), runs.size()));
    for (const LigatureRun& run : runs)
        CHECK_EQ(run.length, 0);
}

TEST(testAFaceThatSubstitutesStillPublishes)
{
    // The same walk on a face that ligates: the strip carries ink the per-cell
    // path cannot produce, so it must reach the columns it covers. Without this
    // arm the rule above would read as "never publish", which no ligature would
    // survive.
    ScriptedFace face(FaceSubstitution::OneForOne);
    LigatureSheet sheet(cellWidth, cellHeight, marginX, marginY, &face);
    CHECK(sheet.isUsable());
    const std::vector<LigatureCell> row = operatorRow("a->b");
    std::vector<LigatureRun> runs(row.size());
    sheet.shapeRow(std::span<const LigatureCell>(row.data(), row.size()), static_cast<float>(cellWidth),
        std::span<LigatureRun>(runs.data(), runs.size()));
    bool hasPublishedRun = false;
    for (const LigatureRun& run : runs)
        hasPublishedRun = hasPublishedRun || run.length > 0;
    CHECK(hasPublishedRun);
}

TEST(testARunAnswersByIdentityBesideItsStripAndStaysAnswered)
{
    // Over a face that substitutes one for one, each character of the run
    // maps to its own glyph in place and draws ink, so every covered cell
    // carries an identity beside the strip. The answer outlives the frame:
    // the next frame is a hit with no second raster, and three frames with
    // the row gone still answer it from the kept set.
    ScriptedFace face(FaceSubstitution::OneForOne);
    LigatureSheet sheet(cellWidth, cellHeight, marginX, marginY, &face);
    CHECK(sheet.isUsable());
    const std::vector<LigatureCell> row = operatorRow("a->b");
    std::vector<LigatureRun> runs(row.size());
    const auto shape = [&] {
        sheet.beginFrame();
        sheet.shapeRow(std::span<const LigatureCell>(row.data(), row.size()), static_cast<float>(cellWidth),
            std::span<LigatureRun>(runs.data(), runs.size()));
    };

    shape();
    int covered = 0;
    int answered = 0;
    for (const LigatureRun& run : runs) {
        if (run.length <= 0)
            continue;
        ++covered;
        if (run.hasShapedGlyph && run.shaped.coveredCells == 1 && run.shaped.glyphID >= ScriptedFace::substitutionBias)
            ++answered;
    }
    CHECK_MSG(covered > 0 && answered == covered, "every covered cell carries the substituted glyph as its identity");
    CHECK_MSG(sheet.answers().missesThisFrame() == 1 && sheet.answers().hitsThisFrame() == 0, "the first frame shapes");
    const int rastersAfterFirst = face.rasterCalls;

    shape();
    CHECK_MSG(sheet.answers().hitsThisFrame() == 1 && sheet.answers().missesThisFrame() == 0, "the second frame is answered");
    CHECK_EQ(face.rasterCalls, rastersAfterFirst);

    sheet.beginFrame();
    sheet.beginFrame();
    sheet.beginFrame();
    shape();
    CHECK_MSG(sheet.answers().hitsThisFrame() == 1, "a run that scrolls back after three frames is still answered");
    CHECK_EQ(face.rasterCalls, rastersAfterFirst);
}

} // namespace

UBIQUITTY_TEST_MAIN()
