// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/PaletteRGB.h"
#include "Core/Tek/ColorPreference.h"
#include "Core/Tek/Primitive.h"
#include "Core/Tek/Project.h"
#include "Core/Tek/Tek4014.h"

#include <cstdint>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Tek {

/// What the host asked a storage-tube frame to draw with, which outranks the
/// user's preferences: `OSC 15`, `16` and `18`, and `DECSET 7400`.
struct HostColorRequests {
    /// The beam colour `OSC 15` set; read only when ``isBeamSet``.
    TekColor beam {};
    /// The clear colour `OSC 16` set; read only when ``isBackgroundSet``.
    TekColor background {};
    /// The crosshair and writing-spot colour `OSC 18` set; read only when
    /// ``isCursorSet``.
    TekColor cursor {};
    /// Whether the host set the beam colour.
    bool isBeamSet = false;
    /// Whether the host set the clear colour.
    bool isBackgroundSet = false;
    /// Whether the host set the cursor colour.
    bool isCursorSet = false;
    /// `DECSET 7400`: 1 forces the writing spot on, 2 forces it off, 0 defers
    /// to the user's setting.
    std::uint8_t brightPenOverride = 0;
};

/// The storage tube as one frame shows it, copied under the model lock so a
/// view draws it without the lock: the primitives the tube displays, the GIN
/// crosshair, the host's colour requests and the theme's own colours, which
/// together are everything a Tek frame reads of the terminal.
class TubeFrame {
public:
    /// Copies what `terminal`'s tube shows now, or empties the frame outside
    /// Tek mode. Reuses the storage an earlier copy grew, so only the first
    /// Tek frame allocates. Caller holds the model lock.
    ///
    /// - Parameter terminal: The terminal whose tube is copied.
    void capture(const Terminal& terminal);

    /// Whether the terminal was in Tek mode, so the frame shows the tube
    /// rather than the grid.
    ///
    /// - Returns: Whether it was.
    [[nodiscard]] bool isTekFrame() const { return m_isTekFrame; }

    /// Returns the primitives the tube displays: the committed frame, or the
    /// one being drawn before any commit.
    ///
    /// - Returns: The primitives, owned by the frame; empty outside Tek mode.
    [[nodiscard]] PrimitiveSpan primitives() const { return { m_primitives.data(), m_primitives.size() }; }

    /// Whether the GIN crosshair is shown.
    ///
    /// - Returns: Whether it is.
    [[nodiscard]] bool isGraphicInputActive() const { return m_isGraphicInputActive; }

    /// Returns the GIN crosshair's column, in Tek space.
    ///
    /// - Returns: The column.
    [[nodiscard]] int graphicInputX() const { return m_graphicInputX; }

    /// Returns the GIN crosshair's row, in Tek space.
    ///
    /// - Returns: The row.
    [[nodiscard]] int graphicInputY() const { return m_graphicInputY; }

    /// Returns what the host asked the frame to draw with.
    ///
    /// - Returns: The requests.
    [[nodiscard]] HostColorRequests hostColorRequests() const { return m_hostColorRequests; }

    /// Returns the terminal's own default background, which the clear colour
    /// falls back to.
    ///
    /// - Returns: The colour, 16-bit channels.
    [[nodiscard]] PaletteRGB themeBackground() const { return m_themeBackground; }

    /// Returns the terminal's own default foreground, which the beam falls
    /// back to.
    ///
    /// - Returns: The colour, 16-bit channels.
    [[nodiscard]] PaletteRGB themeForeground() const { return m_themeForeground; }

private:
    std::vector<Primitive> m_primitives;
    HostColorRequests m_hostColorRequests;
    PaletteRGB m_themeBackground {};
    PaletteRGB m_themeForeground {};
    int m_graphicInputX = 0;
    int m_graphicInputY = 0;
    bool m_isGraphicInputActive = false;
    bool m_isTekFrame = false;
};

} // namespace Ubiquitty::Tek
