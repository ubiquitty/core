// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Cell operations that need the Terminal's cluster pool.

#include "Core/Cell.h"
#include "Core/ClusterPool.h"
#include "Core/Terminal.h"
#include "Core/Unicode/GraphemeBreak.h"
#include <algorithm>

namespace Ubiquitty {

int cellStoredClusterCodePoints(const Ubiquitty::Terminal& t, const Cell& cell, uint32_t* outputBuffer, int max)
{
    // A text-run anchor names its text in the run-text store
    // rather than the cluster pool; a caller sized for a cluster gets the run's
    // first `max` code points. A pool id the pool doesn't hold (a cell copied
    // from another terminal, a planted id) or a freed run id resolves to an
    // empty span and leaves the cell's inline storage to answer.
    const bool isTextRun = isRunTextAnchor(&t, cell);
    const std::span<const uint32_t> sequence = isTextRun
        ? t.runTextStore.text(cell.clusterID())
        : t.clusterPool.sequence(cell.clusterID());
    if (sequence.empty())
        return -1;
    const int count = std::min(static_cast<int>(sequence.size()), max);
    std::copy_n(sequence.begin(), count, outputBuffer);
    // Display rows may replace a cluster's base with its contextual form.
    // Preserve that form just as inline reads do, without changing stored
    // source text or a run whose text is shaped as a whole.
    if (!isTextRun && cell.codePoint != sequence.front()) {
        const auto base = Unicode::clusterBaseIndex(sequence);
        if (base < static_cast<std::size_t>(count))
            outputBuffer[base] = cell.codePoint;
    }
    return count;
}

bool isProportionalTextSizeSpec(const Ubiquitty::Terminal& t, unsigned specID)
{
    const TextSizeSpec* spec = t.textSizeTable.lookup(specID);
    return spec && spec->isProportional;
}

bool isRunTextAnchor(const Ubiquitty::Terminal* t, const Cell& cell)
{
    if (!t || cell.pen.attributes.contains(VT::Attribute::WideContinuation))
        return false;
    const auto* spec = t->textSizeTable.lookup(cell.pen.extendedAttributes.textSizeSpecID());
    return spec && (spec->isProportional || spec->isTextRun);
}

std::span<const uint32_t> runTextCodePoints(const Ubiquitty::Terminal* t, const Cell& cell)
{
    // No terminal, no store: a cell read without one has no text to name.
    if (!t || !isRunTextAnchor(t, cell))
        return {};
    return t->runTextStore.text(cell.clusterID());
}

} // namespace Ubiquitty
