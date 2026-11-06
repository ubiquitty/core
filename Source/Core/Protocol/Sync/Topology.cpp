// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Topology.h"

namespace Ubiquitty::Protocol::Sync {

namespace {

const TopologyNode* findLeafUnder(const TopologyNode& node, ID128 paneID)
{
    if (node.isLeaf())
        return node.paneID == paneID ? &node : nullptr;
    for (const TopologyNode& child : node.children) {
        if (const TopologyNode* found = findLeafUnder(child, paneID))
            return found;
    }
    return nullptr;
}

} // namespace

const TopologyNode* findLeaf(const TopologyTree& tree, ID128 paneID)
{
    return findLeafUnder(tree.root, paneID);
}

PaneRectangle paneRectangleFor(const TopologyNode& leaf, const PanePicture& picture)
{
    // Older than the leaf's generation, the picture is the one from before
    // the change. At or past it and another size, the pane changed again and
    // the tree that says so has not arrived. Either way the picture keeps its
    // own size.
    const bool isLeafsPicture = picture.generation >= leaf.structuralGeneration && picture.rows == leaf.rows && picture.columns == leaf.columns;
    if (isLeafsPicture)
        return { .rows = leaf.rows, .columns = leaf.columns, .isLetterboxed = false };
    return { .rows = picture.rows, .columns = picture.columns, .isLetterboxed = true };
}

} // namespace Ubiquitty::Protocol::Sync
