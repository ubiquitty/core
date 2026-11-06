// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Panes/PaneTree.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Ubiquitty::Panes {

/// A split selector within one exact topology revision. Zero selects the
/// first child, one the second; an empty path selects the root split.
struct DividerPath {
    /// Ordered child choices; entries after length are unused.
    std::array<std::uint8_t, maxPanes - 1> children {};
    /// The number of choices from the root to the selected split.
    std::size_t length = 0;
};

/// Converts an internal node index into a revision-scoped topology selector.
/// - Parameters:
///   - tree: The authoritative adopted layout.
///   - splitIndex: A node index belonging to this exact layout.
/// - Returns: The root-to-split path, or nothing for a missing node or leaf.
[[nodiscard]] inline std::optional<DividerPath> dividerPath(const Tree& tree, int splitIndex)
{
    DividerPath path;
    const auto find = [&](auto&& self, int index) -> bool {
        if (index == invalidNode || tree.isLeaf(index))
            return false;
        if (index == splitIndex)
            return true;
        if (path.length == path.children.size())
            return false;
        const Node node = tree.node(index);
        for (std::uint8_t child : { 0, 1 }) {
            path.children[path.length++] = child;
            if (self(self, child ? node.second : node.first))
                return true;
            --path.length;
        }
        return false;
    };
    return find(find, tree.root()) ? std::optional { path } : std::nullopt;
}

/// Resolves a revision-scoped selector without interpreting pool indices.
/// - Parameters:
///   - tree: The exact named tab layout after revision validation.
///   - path: At most maxPanes-1 binary child selectors.
/// - Returns: The selected split's internal index, or nothing for a bad path.
[[nodiscard]] inline std::optional<int> dividerAtPath(const Tree& tree, std::span<const std::uint8_t> path)
{
    if (path.size() > maxPanes - 1)
        return std::nullopt;
    int index = tree.root();
    for (const auto child : path) {
        if (child > 1 || index == invalidNode || tree.isLeaf(index))
            return std::nullopt;
        const Node node = tree.node(index);
        index = child ? node.second : node.first;
    }
    return index != invalidNode && !tree.isLeaf(index) ? std::optional { index } : std::nullopt;
}

} // namespace Ubiquitty::Panes
