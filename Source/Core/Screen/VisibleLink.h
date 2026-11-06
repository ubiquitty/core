// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/Core/Screen/VisibleLink.h — the link under a cell of the visible grid.
//
// `Core/Attributes/LinkRun.h` computes a span over cells a caller supplies through probes,
// which keeps it free of the model and is why it can be tested without one.
// This is the layer above: it reads the grid, stitches the soft-wrap group, and
// resolves OSC 8 ids through the terminal's link table, so a frontend asks
// "what is under this cell" rather than assembling the row itself.
//
// Every frontend needs that answer and none computes it. A frontend's own copy
// is where a scan of a single row finds a wrapped URL as its truncated head,
// and a frontend without one never fires the link rank of the shared
// `pointerShape`.
//

#include "Core/Attributes/LinkRun.h"

#include "Base/SwiftInterop.h"
#include <cstdint>
#include <span>

namespace Ubiquitty {

struct Terminal;

namespace Screen {

/// Scratch for one scan, owned by the caller.
///
/// The scan runs on every pointer motion, so allocating inside it would put a
/// malloc on a motion path. The caller also decides how much it can spare:
/// wasm's default stack is 64 KB, far less than a full wrapped group needs, so
/// a frontend places the arrays where it has room rather than discovering the
/// limit in a browser.
struct LinkScanBuffer {
    std::uint32_t* codePoints = nullptr;
    std::uint16_t* linkIDs = nullptr;
    int capacityInCells = 0;
};

/// Where a link found under a visible cell lives.
///
/// Rows are absolute buffer rows rather than visible ones, so an underline
/// painted from this stays on its text as the view scrolls. The span is
/// inclusive and covers several rows when the link soft-wraps.
struct VisibleLink {
    /// The program declared this link with `OSC 8`, rather than the text merely
    /// reading like a URL.
    bool isHyperlink = false;

    /// A path candidate. It is text that looks like a path and nothing more —
    /// the caller resolves it against a working directory and confirms it
    /// exists before treating it as a link.
    bool isFilePath = false;

    int absoluteRow0 = 0;
    int column0 = 0;
    int absoluteRow1 = 0;
    int column1 = 0;
    /// The columns the run was resolved inside: the screen's own edges unless
    /// a pane witness narrowed them. A painter clips every row of the run to
    /// this span -- the middle rows of a wrapped run cover the PANE edge to
    /// edge, not the screen, and an underline drawn to the screen's width
    /// strokes straight across the neighbour pane.
    int spanColumnFirst = 0;
    int spanColumnLast = 0;
};

/// Finds the link under a visible cell.
///
/// A hovered `OSC 8` id wins, and its run covers every cell resolving to the
/// same target rather than to the same id — which keeps a wrapped link whole
/// when a program re-emits each visual segment under a fresh `id=`. With no id,
/// the flattened text of the soft-wrap group is scanned for a URL and then for
/// a path; flattening first is what stops a URL long enough to wrap from being
/// found as its truncated head.
///
/// `scope` says how much of that is wanted: `Declared` stops after the id read,
/// which is what a modifier-less hover asks for, and `Any` runs the whole scan.
///
/// The group is stitched from visible rows only, so a link scrolled half off
/// screen underlines the half that is on screen. It is grouped by link id
/// rather than by the wrap flag whenever the hovered cell carries one, because
/// a program on the alternate screen may wrap a link without marking the
/// continuation row wrapped.
///
/// - Parameters:
///   - viewOffset: Rows scrolled back from the live grid.
///   - row: A zero-based visible row.
///   - column: A zero-based column.
///   - scratch: Storage for the scan. A scan needing more cells than this holds
///     is trimmed to what fits, and falls back to the hovered row alone.
///   - output: Receives the NUL-terminated URI, emptied when there is no link.
///   - outLink: Receives the span, untouched when there is no link.
/// How much of the scan a caller is asking for.
enum class LinkScanScope : std::uint8_t {
    /// Only a link the program declared with `OSC 8`: the hovered cell's link
    /// id, one read, and nothing else.
    ///
    /// What it spares is the rest of this function — the pane-edge witnesses,
    /// the soft-wrap flatten, and a regex sweep of the flattened row. A
    /// modifier-less hover runs on every pointer move over any text at all,
    /// and almost none of that text is a link; asking for the whole scan there
    /// spends a row walk per move to answer no.
    Declared UBIQUITTY_SWIFT_NAME(declared),
    /// A declared link, or text that reads like a URL or a path.
    Any UBIQUITTY_SWIFT_NAME(any),
};

/// - Returns: Whether a link covers the cell.
bool linkAtVisibleCell(const Terminal*, int viewOffset, int row, int column, const LinkScanBuffer& scratch,
    std::span<char> output, VisibleLink* outLink, LinkScanScope scope = LinkScanScope::Any);

} // namespace Screen
} // namespace Ubiquitty
