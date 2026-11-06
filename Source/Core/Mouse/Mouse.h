// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Core/Mouse/MouseEvent.h"
#include "Forward.h"
#include <span>

// Mouse-report encoder. The DEC private-mode 9/1000/1002/1003/1006
// setters that flip Terminal's mouseX10 / mouseButtonTracking / mouseCellMotion /
// mouseAnyEvent / mouseSGR booleans live inline in VT.cpp's
// setPrivateMode (two lines each); only the wire-format encoder lives
// in this module. The encoder enforces X10 press-only semantics when
// DECSET 9 is the active mode (no richer 1000/1002/1003 set).
// Ubiquitty::encodeMouseReport (declared in TerminalOperations.h) is the
// public API and forwards to Ubiquitty::Mouse::encodeReport.

namespace Ubiquitty::Mouse {

/// Whether any DECSET tracking mode has the pointer.
///
/// One definition because the answer decides three unrelated things — whether a
/// frontend forwards pointer events at all, whether the text cursor or the
/// application owns a drag, and whether stranded tracking needs clearing — and a
/// site that spells the disjunction itself will omit a mode. DECSET 1001 is the
/// one that gets left out: it reports like 1000 with a highlight handshake on
/// top, so a reader that thinks of it as "the highlight feature" rather than as
/// tracking drops it.
///
/// - Returns: True while X10 (9), normal (1000), hilite (1001), cell-motion
///   (1002) or any-event (1003) is set.
[[nodiscard]] bool tracksPointer(const Ubiquitty::Terminal* t);

/// Encodes an xterm mouse report into `output`.
///
/// The tracking modes set on `t` decide whether an event reports at all, so a
/// caller forwards every event and lets this refuse the ones the client did not
/// ask for.
///
/// - Parameters:
///   - button: An xterm Cb value — ``Button::Left``, the wheel class 64-67, or
///     the extended class 128-131.
///   - row: A zero-based cell row, as every wire format but SGR-pixels wants.
///   - column: A zero-based cell column.
///   - modifiers: ``Modifier`` bits ORed together.
///   - pixelX: A 1-based device-pixel abscissa for SGR-pixels (DECSET 1016),
///     measured from the cell-grid origin in `Terminal::cellPixelWidth`'s basis.
///     A non-negative `pixelX` and `pixelY` together override the cell-quantized
///     fallback; a caller holding no pixel position passes -1 for both.
///   - pixelY: The matching ordinate.
/// - Returns: Bytes written, or 0 when no tracking mode selects this event.
int encodeReport(const Ubiquitty::Terminal* t, Ubiquitty::Mouse::EventType, int button, int row, int column, int modifiers, std::span<char> output, int pixelX = -1, int pixelY = -1);

} // namespace Ubiquitty::Mouse
