// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>

namespace Ubiquitty::Screen {

/// The retained history as one output transition sees it.
///
/// Published beside every drain so a viewport policy can run without the
/// terminal. A held viewport moves by the growth of `appendedRows`, never of
/// `retainedRows`: at the history limit every appended row evicts one, so the
/// retained count stands still while the content still shifts a row per line.
struct HistoryFacts {
    /// Rows the view can scroll back over, zero on the alternate screen.
    int retainedRows = 0;
    /// Rows ever scrolled into history, monotonic through eviction and clears.
    std::int64_t appendedRows = 0;
    /// The coordinate space the rows are numbered in; a resize or a clear
    /// changes it, and rows counted under another epoch are not comparable.
    std::uint32_t absoluteRowEpoch = 0;
};

} // namespace Ubiquitty::Screen
