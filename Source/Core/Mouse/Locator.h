// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/Mouse/MouseEvent.h"
#include "Forward.h"
#include <cstdint>
#include <span>

// DEC locator reporting — DECELR / DECSLE / DECRQLP / DECEFR.
//
// A VT340-era pointing-device protocol, distinct from xterm mouse tracking
// (Core/Mouse/Mouse.h): the host arms the locator, optionally filters reports to a
// rectangle, and the terminal answers with button transitions and positions.
// Rows and columns are 1-based here, as the protocol reports them.

namespace Ubiquitty::Mouse::Locator {

[[nodiscard]] bool isActive(const Ubiquitty::Terminal* t);

/// Encodes a DECRQLP/DECEFR locator reply into `output`.
///
/// - Parameters:
///   - type: Which transition the frontend observed.
///   - button: The xterm Cb value of the button involved.
///   - row: A one-based locator row.
///   - column: A one-based locator column.
///   - buttonState: The DEC button-state bitmap, built with ``buttonBit``.
///   - output: Receives the reply.
/// - Returns: Bytes written, or 0 when the event is filtered out, the locator is
///   disarmed, or `output` is too short for the reply.
int reportEvent(Ubiquitty::Terminal* t, Ubiquitty::Mouse::EventType type, int button, int row, int column, int buttonState, std::span<char> output);

/// Writes one DECRQLP reply, with no filtering.
///
/// The wire form behind ``reportEvent``, which reaches it only after deciding
/// the event should be reported at all. Called directly to answer an explicit
/// DECRQLP, where the request itself is the reason to reply.
///
/// - Parameters:
///   - event: The DEC event code the reply reports.
///   - buttons: The DEC button-state bitmap.
///   - row: A one-based locator row, or 0 when the position is undefined.
///   - column: A one-based locator column, or 0 when the position is undefined.
///   - page: The DEC page number, 1 for a terminal with one page.
///   - output: Receives the reply.
/// - Returns: Bytes written, or 0 when `output` is too short for the reply.
int encodeReport(int event, int buttons, int row, int column, int page, std::span<char> output);

/// Which DEC 1001 hilite end report to write.
///
/// The two differ in arity, and a reader counting parameters is the only way to
/// tell them apart on the wire.
enum class HiliteEndForm : std::uint8_t {
    /// Four parameters: the highlighted region alone.
    RegionOnly,
    /// Six parameters: the region, then the pointer's own position.
    RegionAndPointer
};

/// Writes a DEC 1001 hilite end report.
///
/// - Parameters:
///   - startColumn: A one-based column where the highlighted region starts.
///   - startRow: A one-based row where it starts.
///   - endColumn: A one-based column where it ends.
///   - endRow: A one-based row where it ends.
///   - mouseColumn: The pointer's one-based column, carried only by the long form.
///   - mouseRow: The pointer's one-based row, likewise.
///   - form: Which of the two wire variants to write.
///   - output: Receives the report.
/// - Returns: Bytes written, or 0 when `output` is too short for the report.
int encodeHiliteEndReport(int startColumn, int startRow, int endColumn, int endRow, int mouseColumn, int mouseRow, HiliteEndForm form, std::span<char> output);

/// Bit for `xtermButton` in the DECRQLP button mask, or 0 when unmapped.
[[nodiscard]] int buttonBit(int xtermButton);

} // namespace Ubiquitty::Mouse::Locator
