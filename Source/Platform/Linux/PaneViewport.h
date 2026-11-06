// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Runtime/TerminalSession.h"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace Ubiquitty::Linux {

enum class ViewportPublication : std::uint8_t {
    Unchanged,
    Pixels,
    Grid,
};

/// A GTK pane's last published drawable, not its program's current grid.
/// This cache follows the view even while another client owns remote geometry.
class PaneViewportPublisher {
public:
    /// Stages complete metrics before sending a single combined grid report.
    /// The pane link alone chooses whether that report resizes a local model
    /// or asks its remote owner. No GTK state or process is required here.
    [[nodiscard]] ViewportPublication publish(Runtime::TerminalSession& session, GridSize grid, PixelGeometry pixels)
    {
        if (grid.columns < 1 || grid.rows < 1)
            return ViewportPublication::Unchanged;
        const auto saturated = [](IntSize size) { return IntSize { std::clamp(size.width, 0, 65535), std::clamp(size.height, 0, 65535) }; };
        pixels = { saturated(pixels.cell), saturated(pixels.textArea), saturated(pixels.drawable) };
        const bool isGridChange = !m_grid || *m_grid != grid;
        const bool isPixelChange = !m_pixels || m_pixels->cell != pixels.cell || m_pixels->textArea != pixels.textArea || m_pixels->drawable != pixels.drawable;
        IntSize reportedPixels;
        session.lockForRead();
        if (const auto* const terminal = session.paneView().terminal)
            reportedPixels = reportedTextAreaPixels(*terminal, pixels);
        session.unlockForRead();
        if (!isGridChange && !isPixelChange && reportedPixels == m_reportedPixels)
            return ViewportPublication::Unchanged;
        m_grid = grid;
        m_pixels = pixels;
        m_reportedPixels = reportedPixels;
        if (isPixelChange)
            session.publishCellPixelSize(pixels.cell.width, pixels.cell.height, pixels.textArea.width, pixels.textArea.height,
                pixels.drawable.width, pixels.drawable.height);
        if (isGridChange) {
            session.resize(grid.rows, grid.columns, Runtime::TerminalSession::ResizeGesture::Settled, Modes::AnchorMode::Top,
                reportedPixels.width, reportedPixels.height);
            return ViewportPublication::Grid;
        }
        session.setWindowSize(grid.columns, grid.rows, reportedPixels.width, reportedPixels.height);
        return ViewportPublication::Pixels;
    }

private:
    std::optional<GridSize> m_grid;
    std::optional<PixelGeometry> m_pixels;
    IntSize m_reportedPixels;
};

} // namespace Ubiquitty::Linux
