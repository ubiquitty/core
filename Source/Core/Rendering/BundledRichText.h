// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Rendering/BundledRichText.h — rich text blocks set in the faces Core
// bundles, for the software renderer (Docs/rich-text.md, "Rendering").
//
// The software renderer has no font library and no paragraph shaper, so its
// class faces are the ones Core carries and its blocks take Core's table
// layout. The table this publishes is measured from exactly the faces the
// blocks are then drawn in, so a glyph lands where Core put it, the highlight
// sits on it and a click names it.

#include "Base/Geometry.h"
#include "Core/Rendering/CellMetrics.h"
#include "Core/Rendering/TerminalFrameInputs.h"
#include "Core/RichText/TypographyFacts.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Ubiquitty::Rendering {

/// Rich text blocks set in the faces Core bundles, at the positions Core
/// lays them out at.
///
/// The class faces:
///
/// - Sans-serif is Noto Sans, Regular and Bold, drawn from the outlines the
///   bitmap tier draws its proportional runs with. A code point the face
///   lacks is GNU Unifont's glyph, and the table measures it there.
/// - Monospace is GNU Unifont, the grid's own face, at the run's size. A run
///   at the grid's size in terminal line mode draws the grid's own pixels.
/// - There is no serif face. A serif entry resolves to the sans-serif class
///   face by the rule every unpublished face resolves by.
///
/// A face without a bold design, GNU Unifont, is emboldened by one texel;
/// italic is the upright face sheared as an oblique.
class BundledRichText {
public:
    /// The table Core lays blocks out by, for a grid drawn in GNU Unifont.
    ///
    /// The table names no shaper: each face is measured once, here, and Core
    /// lays every block out from the advances.
    ///
    /// - Parameters:
    ///   - cell: The grid's cell, in device pixels.
    ///   - unifontScale: The grid's integer upscale of GNU Unifont.
    ///   - pixelsPerPoint: Device pixels per point.
    /// - Returns: The cell and the sans-serif and monospace class faces in
    ///   their four styles.
    [[nodiscard]] static RichText::TypographyFacts typographyFacts(const CellMetrics& cell, int unifontScale, double pixelsPerPoint);

    /// Composites a frame's blocks: their `\highlight` backgrounds, the
    /// selection under the text or over it as the frame's selection style
    /// draws it, and each block's retained raster.
    ///
    /// A block is rasterised again only when its content key or its size
    /// moves; a raster for a block the frame no longer shows is dropped.
    ///
    /// - Parameters:
    ///   - frame: The captured frame, whose blocks were laid out from
    ///     ``typographyFacts``.
    ///   - rgba: The frame's pixels, RGBA8 with row stride `4 * surface.width`.
    ///   - surface: The frame's size in pixels.
    ///   - occlusions: Chrome drawn over the grid, which no block covers.
    void composite(const TerminalFrameInputs& frame, std::span<std::uint8_t> rgba, IntSize surface, std::span<const IntRect> occlusions = {});

    /// How many block rasters this has drawn, for the retained-rendering
    /// contract: a frame whose blocks did not change draws none.
    [[nodiscard]] std::size_t rasterCount() const { return m_rasterCount; }

private:
    /// One block's glyphs and decorations, premultiplied RGBA8 on a
    /// transparent ground, at the block's device pixels.
    struct Raster {
        std::uint64_t key = 0;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels;
        bool isSeen = false;
    };

    std::unordered_map<std::uint32_t, Raster> m_rasters;
    std::size_t m_rasterCount = 0;
};

} // namespace Ubiquitty::Rendering
