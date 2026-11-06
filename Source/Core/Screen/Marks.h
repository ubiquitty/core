// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Forward.h"

// Row marks and the clear-to-mark policy (the Cmd-L path).
//
// A mark records where a shell prompt began, so "clear" can mean "everything
// above the current command" rather than "everything". Screen-model work, not
// sequence parsing — the OSC handler only stamps the mark.

namespace Ubiquitty::Screen {

/// How clearToMarkOrFull resolved the clear, so the frontend can report / log it.
enum class ClearToMarkOutcome : uint8_t {
    ClearedToMark, // an executed prompt above the cursor was found
    FullClear, // none found -> fell back to clearBufferPreserveInput
    HandedOff, // a program owns the layout -> it got the redraw request, untouched
};

/// Drops every row above the most recent executed prompt.
///
/// Named for what it does rather than for the Cmd-L gesture, because
/// `ClearBuffer.h` declares a four-argument `clearToPreviousMark` that is the
/// whole gesture — selection, view offset and the bytes sent to the process —
/// and calls this one to do the dropping. Two functions with one name in one
/// namespace is an overload set nobody chose.
///
/// - Parameter t: The terminal to clear above.
/// - Returns: The row cleared to, or -1 when there is no executed prompt above.
///   The row is an index and not a count, so zero is a success: a mark can sit
///   at the first row of history, and a caller testing the result for
///   truthiness discards that case after the rows have already gone.
int dropRowsToPreviousMark(Ubiquitty::Terminal* t);

/// Applies the guarded clear-to-mark policy to a headless picture.
///
/// Delegates to `clearToPreviousMark` without a process or selection. A
/// program-owned layout returns `HandedOff` and keeps the picture and view
/// offset intact. Live callers use `ClearBuffer.h` to supply the input route.
///
/// Declared beside the outcome it returns but defined in `ClearBuffer.cpp`:
/// the policy it delegates to composes the selection model, so it is
/// behaviour, and a definition in this model unit would make the model call
/// up into it.
/// - Parameter viewOffset: Reset to zero only when rows are cleared.
ClearToMarkOutcome clearToMarkOrFull(Ubiquitty::Terminal* t, int* viewOffset = nullptr);

} // namespace Ubiquitty::Screen
