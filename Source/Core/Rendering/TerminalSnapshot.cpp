// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/TerminalSnapshot.h"
#include "Core/Rendering/BoxStrokeProfile.h"
#include "Core/Screen/Addressing.h"
#include <span>

#include "Core/Cell.h"
#include "Core/RenderCell.h"
#include "Core/Rendering/BoxDrawingRasterizer.h"
#include "Core/Rendering/BundledRichText.h"
#include "Core/Rendering/CellCanvas.h"
#include "Core/Rendering/ColorResolve.h"
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Rendering/PNGEncoder.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/Rendering/UnifontRasterizer.h"
#include "Core/Tek/BeamRaster.h"
#include "Core/Tek/Project.h"
#include "Core/Tek/Tek4014.h"
#include "Core/TermPalette.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cmath>

namespace Ubiquitty::Rendering {

namespace {

// Mixes a colour into a pixel at an alpha byte: the software renderer's
// straight-byte mix, rounded half up, which the bitmap tier's Metal overlay
// fragments reproduce.
void mixPixel(std::uint8_t* pixel, RGB8 color, int alpha)
{
    const auto mix = [alpha](int destination, int source) {
        return static_cast<std::uint8_t>((destination * (255 - alpha) + source * alpha + 127) / 255);
    };
    pixel[0] = mix(pixel[0], color.red);
    pixel[1] = mix(pixel[1], color.green);
    pixel[2] = mix(pixel[2], color.blue);
    pixel[3] = 255;
}

// The pixels a run's raster can touch: its bounding box grown by `extent`,
// clipped to the frame. The Metal pass rasterises the run's oriented quad grown
// by the same extent, which holds every pixel the raster gives a non-zero value.
struct PixelBounds {
    int left;
    int top;
    int right;
    int bottom;
};

PixelBounds runBounds(const Tek::StrokeRun& run, float extent, int width, int height)
{
    return {
        std::max(static_cast<int>(std::floor(std::min(run.start.x, run.end.x) - extent)), 0),
        std::max(static_cast<int>(std::floor(std::min(run.start.y, run.end.y) - extent)), 0),
        std::min(static_cast<int>(std::ceil(std::max(run.start.x, run.end.x) + extent)), width - 1),
        std::min(static_cast<int>(std::ceil(std::max(run.start.y, run.end.y) + extent)), height - 1),
    };
}

// The bloom under the beam: each pixel is lifted, channel by channel, to the
// halo table's level at its distance from each run, by maximum — the Metal glow
// pipeline's blend, which commutes with the sRGB encoding, so both arms land on
// the table's bytes.
void compositeTektronixGlow(std::span<const Tek::StrokeRun> runs, const Tek::BeamGeometry& beam, RGB8 beamColor,
    std::vector<std::uint8_t>& rgba, int width, int height)
{
    std::vector<Tek::HaloLevel> levels(static_cast<std::size_t>(std::max(Tek::haloLevelCount(beam), 0)));
    const int levelCount = Tek::fillHaloLevels(beam, beamColor.red, beamColor.green, beamColor.blue, levels);
    if (levelCount <= 0)
        return;
    for (const Tek::StrokeRun& run : runs) {
        const Tek::BeamRun raster(run.start.x, run.start.y, run.end.x, run.end.y);
        const PixelBounds bounds = runBounds(run, beam.haloReach + 1.0f, width, height);
        for (int y = bounds.top; y <= bounds.bottom; ++y) {
            for (int x = bounds.left; x <= bounds.right; ++x) {
                const int index = Tek::haloLevelIndex(raster.distanceToPixel(x, y));
                if (index >= levelCount)
                    continue;
                const Tek::HaloLevel& level = levels[static_cast<std::size_t>(index)];
                std::uint8_t* pixel = &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4];
                pixel[0] = std::max(pixel[0], level.red);
                pixel[1] = std::max(pixel[1], level.green);
                pixel[2] = std::max(pixel[2], level.blue);
            }
        }
    }
}

// The beam's core, run by run in the order the Metal pass draws them: each
// pixel's coverage byte from Core's raster, mixed into the byte beneath as the
// bitmap tier's Metal fragment mixes it.
void compositeTektronixBeam(std::span<const Tek::StrokeRun> runs, const Tek::BeamGeometry& beam, RGB8 beamColor,
    std::vector<std::uint8_t>& rgba, int width, int height)
{
    const int halfThickness = Tek::BeamRun::units(beam.halfThickness);
    for (const Tek::StrokeRun& run : runs) {
        const Tek::BeamRun raster(run.start.x, run.start.y, run.end.x, run.end.y);
        const PixelBounds bounds = runBounds(run, beam.halfThickness + 1.0f, width, height);
        for (int y = bounds.top; y <= bounds.bottom; ++y) {
            for (int x = bounds.left; x <= bounds.right; ++x) {
                const int alpha = Tek::beamCoverageByte(Tek::beamCoverage(raster.distanceToPixel(x, y), halfThickness));
                if (!alpha)
                    continue;
                mixPixel(&rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4], beamColor, alpha);
            }
        }
    }
}

} // namespace

void compositeTektronixFrame(const Terminal& terminal, const TerminalSnapshotOptions& options, std::vector<std::uint8_t>& rgba, int width, int height)
{
    if (!terminal.isTekModeActive)
        return;
    const Tek::ColorPreference& preference = options.tekColors;
    const Tek::BeamGeometry beamGeometry = Tek::beamGeometry(options.tekDeviceScale);
    const Tek::Projection projection = Tek::makeBeamProjection(tekScreenWidth(terminal), tekScreenHeight(terminal), 0.0f, 0.0f,
        static_cast<float>(width), static_cast<float>(height), options.tekDeviceScale);
    const RGB8 themeForeground = resolvePaletteColor(terminal.palette, colorDefault, true);
    const RGB8 themeBackground = resolvePaletteColor(terminal.palette, colorDefault, false);
    const auto narrow = [](Tek::TekColor color) {
        return RGB8 { static_cast<std::uint8_t>((color.red + 128) / 257), static_cast<std::uint8_t>((color.green + 128) / 257), static_cast<std::uint8_t>((color.blue + 128) / 257) };
    };
    const RGB8 beam = narrow(Tek::tekBeam(terminal, ColorRGBA { themeForeground.red, themeForeground.green, themeForeground.blue, 255 }, preference));
    const RGB8 tube = narrow(Tek::tekBackground(terminal, ColorRGBA { themeBackground.red, themeBackground.green, themeBackground.blue, 255 }, preference));
    for (std::size_t offset = 0; offset + 3 < rgba.size(); offset += 4) {
        rgba[offset] = tube.red;
        rgba[offset + 1] = tube.green;
        rgba[offset + 2] = tube.blue;
        rgba[offset + 3] = 255;
    }
    const int committedCount = tekDisplayCount(terminal);
    const int count = committedCount > 0 ? committedCount : tekPrimitiveCount(terminal);
    const Tek::Primitive* primitives = committedCount > 0 ? tekDisplayPrimitives(terminal) : tekPrimitives(terminal);
    if (count <= 0)
        return;
    std::vector<Tek::StrokeRun> runs(static_cast<std::size_t>(count) * Tek::maxStrokeRunsPerLine);
    const int runCount = Tek::buildStrokeRuns(std::span { primitives, static_cast<std::size_t>(count) }, projection, runs);
    if (runCount <= 0)
        return;
    const std::span<const Tek::StrokeRun> drawn { runs.data(), static_cast<std::size_t>(runCount) };
    if (options.shouldDrawTekGlow)
        compositeTektronixGlow(drawn, beamGeometry, beam, rgba, width, height);
    compositeTektronixBeam(drawn, beamGeometry, beam, rgba, width, height);
}

std::vector<std::uint8_t> renderTerminalSnapshotRGBA(const Terminal& terminal, const TerminalSnapshotOptions& options, int& widthOut, int& heightOut,
    ChromeLayers chrome, GlyphRasterizer* injected, SoftwareRenderer* ownedRenderer, BundledRichText* richText)
{
    widthOut = 0;
    heightOut = 0;
    if (terminal.columns <= 0 || terminal.rows <= 0 || !terminal.screens.active)
        return {};

    // The embedded Unifont is the default rather than the only option. A caller
    // comparing this raster against its own GPU path passes that path's
    // rasterizer, because two typefaces disagree on most of their ink and none
    // of that difference is about the renderer being compared.
    const int scale = std::max(options.style.unifontScale, 1);
    const int lineSpacing = std::max(options.style.lineSpacingInPixels, 0);
    UnifontRasterizer unifont(scale, lineSpacing, options.style.characterSpacingInPixels);
    // Wrapped exactly as the frontends wrap whatever they draw with. Without it
    // this path was the only one taking box characters from the face's own
    // bitmaps while every frontend decomposed them procedurally, and the two
    // place a rule on different rows: measured against the GTK and browser arms,
    // a box_drawing render put its horizontal one row higher, and 306 pixels of
    // one small case differed. That matters more here than anywhere, because
    // this is the renderer whose images the other arms are compared against.
    BoxDrawingRasterizer boxes(unifont);
    boxes.setShadeStyle(options.style.shadeStyle);
    GlyphRasterizer& rasterizer = injected ? *injected : static_cast<GlyphRasterizer&>(boxes);
    // The cell comes from whichever rasterizer is drawing, so an injected face
    // sizes the grid as its own metrics ask. Taking it from Unifont regardless
    // would lay a real face out against a cell it does not fit.
    // A caller that renders every frame owns its renderer, so the glyph cache
    // it was given carries across frames instead of being allocated, zeroed and
    // refilled per call. The local one is for the one-shot callers.
    SoftwareRenderer localRenderer(rasterizer);
    SoftwareRenderer& renderer = ownedRenderer ? *ownedRenderer : localRenderer;

    TerminalFrameInputs frame;
    // An empty slot geometry, stated rather than defaulted: zero by omission is
    // the same value and a different fact. This arm keeps every glyph baked into
    // its cell: a nonzero margin selects the subpixel pass it has no blit for,
    // and a nonzero wide slot routes wide glyphs to a stream it does not offer.
    GlyphSlotGeometry cellBakedSlot {};
    // The one member measured rather than zeroed: the face's own box strokes,
    // so the procedural geometry — diagonals, arcs, the thin Powerline
    // separators — carries the face's rule weight instead of the em-derived
    // fallback's. Left empty, every procedural stroke on this arm draws
    // heavier than the face it sits beside.
    cellBakedSlot.boxStrokeProfile = BoxDrawing::measureBoxStrokeProfile(
        rasterizer, renderer.cellWidth(), renderer.cellHeight());
    // The hilite band is derived here rather than asked of the caller: this entry
    // takes the whole terminal, so a driver that forgot to pass the spans would
    // render a frame missing a region the host explicitly asked to be shown, and
    // nothing downstream can tell that apart from a host that named none.
    std::vector<SelectionRowSpan> hiliteSpans;
    TerminalSnapshotOptions optionsWithBand = options;
    if (terminal.modes.hiliteRegionValid && !options.hiliteRegionSpans) {
        hiliteSpans.resize(static_cast<std::size_t>(terminal.rows));
        const int count = hiliteRegionRowSpans(terminal.modes.hiliteStartX, terminal.modes.hiliteStartY,
            terminal.modes.hiliteFirstRow, terminal.modes.hiliteLastRow, terminal.rows, terminal.columns,
            options.viewOffset, hiliteSpans.data(), terminal.rows);
        hiliteSpans.resize(static_cast<std::size_t>(count));
        optionsWithBand.hiliteRegionSpans = hiliteSpans.empty() ? nullptr : hiliteSpans.data();
        optionsWithBand.hiliteRegionSpanCount = static_cast<int>(hiliteSpans.size());
    }
    // The DECSSDT status row, on the same terms as the hilite band above. The
    // row is not part of the grid — it is a one-row buffer beside it — so a
    // driver that does not ask for it renders the last row as whatever the grid
    // holds there, and downstream that is indistinguishable from a terminal
    // with no status line configured. A caller that filled its own is left
    // alone; the scratch outlives the capture below, which only borrows it.
    StatusRowStorage statusRowScratch;
    if (!options.statusRowCells
        && fillStatusRow(&terminal, terminal.rows, terminal.columns, options.viewOffset,
            StatusRowCells { statusRowScratch.cells.data(), statusRowScratch.cells.size() })) {
        optionsWithBand.statusRowCells = statusRowScratch.cells.data();
        optionsWithBand.statusRowColumns = terminal.columns;
    }
    // The padding is composited into rather than added afterwards. The grid
    // image and the padded one differ by a border, and building them separately
    // costs a second full-size allocation, a background written over every pixel
    // of it, and the grid copied in over most of what that just wrote — three
    // passes over the frame to place a border around it. The renderer already
    // takes an origin and a viewport, so the border is the only part left to
    // paint.
    const int left = std::max(static_cast<int>(std::lround(options.style.paddingInPixels.left)), 0);
    const int right = std::max(static_cast<int>(std::lround(options.style.paddingInPixels.right)), 0);
    const int top = std::max(static_cast<int>(std::lround(options.style.paddingInPixels.top)), 0);
    const int bottom = std::max(static_cast<int>(std::lround(options.style.paddingInPixels.bottom)), 0);
    // A caller's own viewport stays the GRID's extent, which is what it was when
    // the padding was added around the image the renderer returned.
    const int gridWidth = optionsWithBand.viewportSizeInPixels.width > 0.0f
        ? static_cast<int>(optionsWithBand.viewportSizeInPixels.width)
        : terminal.columns * renderer.cellWidth();
    const int gridHeight = optionsWithBand.viewportSizeInPixels.height > 0.0f
        ? static_cast<int>(optionsWithBand.viewportSizeInPixels.height)
        : terminal.rows * renderer.cellHeight();
    const int width = gridWidth + left + right;
    const int height = gridHeight + top + bottom;
    if (width <= 0 || height <= 0)
        return {};
    optionsWithBand.viewportSizeInPixels = { static_cast<float>(width), static_cast<float>(height) };
    optionsWithBand.originInPixels = { optionsWithBand.originInPixels.x + static_cast<float>(left),
        optionsWithBand.originInPixels.y + static_cast<float>(top) };

    if (!frame.capture(terminal, optionsWithBand, rasterizer.cellMetrics(), cellBakedSlot, chrome))
        return {};
    const BuildInputs& inputs = frame.inputs();

    // The blocks are drawn where the frame's chrome is not: a hint or the
    // palette composited into the cells stays over them.
    struct BlockPass {
        BundledRichText* richText = nullptr;
        const TerminalFrameInputs* frame = nullptr;
        std::vector<IntRect> occlusions;
    };
    BlockPass blockPass { richText, &frame, {} };
    BlockLayer blocks;
    if (richText) {
        const CellMetrics& cell = frame.metrics();
        for (const ChromePlacement& layer : chrome) {
            if (!layer.canvas || layer.canvas->rows <= 0 || layer.canvas->columns <= 0)
                continue;
            IntRect occlusion;
            occlusion.origin = { static_cast<int>(std::lround(inputs.originInPixels.x + static_cast<float>(layer.originColumn) * cell.cellWidth)),
                static_cast<int>(std::lround(inputs.originInPixels.y + static_cast<float>(layer.originRow) * cell.cellHeight)) };
            occlusion.size = { static_cast<int>(std::lround(static_cast<float>(layer.canvas->columns) * cell.cellWidth)),
                static_cast<int>(std::lround(static_cast<float>(layer.canvas->rows) * cell.cellHeight)) };
            blockPass.occlusions.push_back(occlusion);
        }
        blocks.function = [](void* context, BlockLayer::Pixels pixels, IntSize surface) {
            const BlockPass& pass = *static_cast<const BlockPass*>(context);
            pass.richText->composite(*pass.frame, pixels, surface, pass.occlusions);
        };
        blocks.context = &blockPass;
    }

    std::vector<std::uint8_t> rgba;
    int renderedWidth = 0;
    int renderedHeight = 0;
    const RGB8 background = resolvePaletteColor(terminal.palette, colorDefault, inputs.screenReverse);
    // Paint the margin before overlays so every search pill can overhang it.
    if (!renderer.render(inputs, rgba, renderedWidth, renderedHeight,
            { background.red, background.green, background.blue,
                static_cast<std::uint8_t>(std::lround(inputs.backgroundOpacity * 255.0f)) },
            blocks)
        || rgba.empty()) {
        return {};
    }
    compositePromptMarksIntoPadding(promptMarkParametersForPadding(terminal.palette,
                                        { terminal.columns, terminal.rows }, { frame.metrics().cellWidth, frame.metrics().cellHeight },
                                        { static_cast<float>(left), static_cast<float>(top) }, static_cast<float>(width),
                                        frame.metrics().cellHeight, rasterizer.faceMetrics()),
        terminal, frame.firstAbsoluteRow(), rgba, { width, height });
    compositeTektronixFrame(terminal, options, rgba, width, height);
    widthOut = width;
    heightOut = height;
    return rgba;
}

std::vector<std::uint8_t> renderTerminalSnapshotPNG(const Terminal& terminal, const TerminalSnapshotOptions& options, BundledRichText* richText)
{
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> rgba = renderTerminalSnapshotRGBA(terminal, options, width, height, {}, nullptr, nullptr, richText);
    if (rgba.empty())
        return {};
    return encodePNG(rgba, { width, height });
}

} // namespace Ubiquitty::Rendering
