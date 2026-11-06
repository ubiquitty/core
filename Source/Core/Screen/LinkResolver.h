// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Screen/VisibleLink.h"
#include "Core/VT/Constants.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Screen {

/// A link scan that carries its own scratch.
///
/// ``linkAtVisibleCell`` takes its working storage from the caller, because it
/// runs on every pointer motion and a malloc there would be paid per move —
/// and because a frontend on a small stack has to place that storage itself.
/// Most callers have neither constraint and only want the answer, so this
/// holds the buffers across scans, hands the scan what it needs, and keeps the
/// result until the next one.
///
/// Held by whatever owns the pointer's position — one per view, not one per
/// process: two views hover independently.
class LinkResolver {
public:
    /// Finds the link under a visible cell, keeping the result.
    ///
    /// - Parameters:
    ///   - terminal: The model to read. Null answers false.
    ///   - viewOffset: Rows scrolled back from the live grid.
    ///   - row: A zero-based visible row.
    ///   - column: A zero-based column.
    /// - Returns: Whether a link covers the cell. False leaves ``uri`` empty.
    /// - Parameter scope: How much of the scan to run; see
    ///   ``Screen/LinkScanScope``.
    bool resolveAt(const Terminal* terminal, int viewOffset, int row, int column,
        LinkScanScope scope = LinkScanScope::Any)
    {
        m_uri.clear();
        m_link = VisibleLink {};
        if (!terminal)
            return false;
        if (m_codePoints.empty()) {
            m_codePoints.resize(scanCapacityInCells);
            m_linkIDs.resize(scanCapacityInCells);
        }
        const LinkScanBuffer scratch { m_codePoints.data(), m_linkIDs.data(), static_cast<int>(m_codePoints.size()) };
        char uri[Ubiquitty::VT::maxLinkURILength] = { 0 };
        if (!linkAtVisibleCell(terminal, viewOffset, row, column, scratch, uri, &m_link, scope))
            return false;
        m_uri.assign(uri);
        return !m_uri.empty();
    }

    /// The URI the last successful scan found, empty after a failed one.
    [[nodiscard]] std::string_view uri() const { return m_uri; }

    /// Where that link sits, in absolute buffer rows.
    [[nodiscard]] const VisibleLink& link() const { return m_link; }

private:
    // A wrapped link spans rows, and the scan flattens the whole soft-wrap
    // group before matching so a URL long enough to wrap is not found as its
    // truncated head. This is what that costs: enough cells for a group a
    // person would plausibly produce, allocated once.
    static constexpr int scanCapacityInCells = 8192;

    std::vector<std::uint32_t> m_codePoints;
    std::vector<std::uint16_t> m_linkIDs;
    std::string m_uri;
    VisibleLink m_link;
};

} // namespace Ubiquitty::Screen
