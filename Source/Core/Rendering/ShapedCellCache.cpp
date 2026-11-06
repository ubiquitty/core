// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/ShapedCellCache.h"

#include <algorithm>
#include <cstring>

namespace Ubiquitty::Rendering {

namespace {

// FNV-1a over the segment and its style, widened to 64 bits so the index
// narrows well before the code points decide. The style is folded in rather
// than compared separately for the same reason a bold cell keys its own glyph:
// one segment drawn two ways is two answers.
std::uint64_t foldSegment(std::span<const std::uint32_t> codePoints, std::uint32_t shapeIdentity)
{
    std::uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= (value & 0xFFu);
            hash *= 1099511628211ull;
            value >>= 8;
        }
    };
    for (std::uint32_t codePoint : codePoints)
        mix(codePoint);
    mix(shapeIdentity);
    return hash;
}

} // namespace

int ShapedCellCache::find(const Frame& frame, std::uint64_t hash, ShapedSegmentSpan codePoints, std::uint32_t shapeIdentity)
{
    if (frame.index.empty())
        return -1;
    const std::size_t mask = frame.index.size() - 1;
    for (std::size_t probe = static_cast<std::size_t>(hash) & mask;; probe = (probe + 1) & mask) {
        const std::uint32_t slot = frame.index[probe];
        // The first empty slot ends the run this fold could have landed in: an
        // insert stops at the first empty slot too, so nothing it holds lies past
        // one.
        if (slot == noSlot)
            return -1;
        const Entry& entry = frame.entries[slot];
        // The fold is compared before the text because a probe run holds
        // whatever folds share its home, not only this one.
        if (entry.hash == hash && entry.shapeIdentity == shapeIdentity && entry.textCount == codePoints.size()
            && std::equal(codePoints.begin(), codePoints.end(), frame.text.begin() + entry.textOffset))
            return static_cast<int>(slot);
    }
}

void ShapedCellCache::indexEntry(Frame& frame, std::uint64_t hash, std::uint32_t position)
{
    const std::size_t mask = frame.index.size() - 1;
    std::size_t probe = static_cast<std::size_t>(hash) & mask;
    while (frame.index[probe] != noSlot)
        probe = (probe + 1) & mask;
    frame.index[probe] = position;
}

void ShapedCellCache::reserveIndex(Frame& frame)
{
    const std::size_t wanted = frame.entries.size() + 1;
    if (!frame.index.empty() && wanted * 4 <= frame.index.size() * 3)
        return;
    std::size_t capacity = frame.index.empty() ? frameIndexInitialCapacity : frame.index.size();
    while (wanted * 4 > capacity * 3)
        capacity *= 2;
    frame.index.assign(capacity, noSlot);
    for (std::size_t position = 0; position < frame.entries.size(); ++position)
        indexEntry(frame, frame.entries[position].hash, static_cast<std::uint32_t>(position));
}

ShapedCellSpan ShapedCellCache::append(Frame& frame, std::uint64_t hash, ShapedSegmentSpan codePoints, std::uint32_t shapeIdentity, ShapedCellSpan cells)
{
    Entry entry;
    entry.hash = hash;
    entry.shapeIdentity = shapeIdentity;
    entry.textOffset = static_cast<std::uint32_t>(frame.text.size());
    entry.textCount = static_cast<std::uint32_t>(codePoints.size());
    entry.cellOffset = static_cast<std::uint32_t>(frame.cells.size());
    entry.cellCount = static_cast<std::uint32_t>(cells.size());
    frame.text.insert(frame.text.end(), codePoints.begin(), codePoints.end());
    frame.cells.insert(frame.cells.end(), cells.begin(), cells.end());
    // Before the write, and against the entries the frame already holds: growing
    // re-indexes them, and the new one is not among them yet.
    reserveIndex(frame);
    indexEntry(frame, hash, static_cast<std::uint32_t>(frame.entries.size()));
    frame.entries.push_back(entry);
    return { frame.cells.data() + entry.cellOffset, entry.cellCount };
}

ShapedCellSpan ShapedCellCache::lookup(ShapedSegmentSpan codePoints UBIQUITTY_NOESCAPE, std::uint32_t shapeIdentity)
{
    const std::uint64_t hash = foldSegment(codePoints, shapeIdentity);
    if (const int index = find(m_current, hash, codePoints, shapeIdentity); index >= 0) {
        ++m_hits;
        const Entry& entry = m_current.entries[static_cast<std::size_t>(index)];
        return { m_current.cells.data() + entry.cellOffset, entry.cellCount };
    }
    if (const int index = find(m_previous, hash, codePoints, shapeIdentity); index >= 0) {
        ++m_hits;
        // Migrated forward rather than read in place: the previous frame is
        // cleared at the next swap, and a span into it would then point at
        // storage the cache reuses.
        const Entry& entry = m_previous.entries[static_cast<std::size_t>(index)];
        const ShapedCellSpan answer { m_previous.cells.data() + entry.cellOffset, entry.cellCount };
        return append(m_current, hash, codePoints, shapeIdentity, answer);
    }
    // Out of both frames but not yet evicted: answered from the kept set and
    // handed back to the current frame, which owns it again.
    if (const std::uint32_t slot = findKept(hash, codePoints, shapeIdentity); slot != noSlot) {
        ++m_hits;
        const Kept& kept = m_kept[slot];
        const ShapedCellSpan answer = append(m_current, hash, codePoints, shapeIdentity,
            ShapedCellSpan(kept.cells).first(kept.cellCount));
        forgetKept(slot);
        return answer;
    }
    ++m_misses;
    return {};
}

std::uint32_t ShapedCellCache::findKept(std::uint64_t hash, ShapedSegmentSpan codePoints, std::uint32_t shapeIdentity) const
{
    if (m_keptIndex.empty())
        return noSlot;
    constexpr std::size_t mask = keptIndexCapacity - 1;
    for (std::size_t cell = static_cast<std::size_t>(hash) & mask;; cell = (cell + 1) & mask) {
        const std::uint32_t slot = m_keptIndex[cell];
        if (slot == noSlot)
            return noSlot;
        const Kept& kept = m_kept[slot];
        if (kept.hash == hash && kept.shapeIdentity == shapeIdentity && kept.textCount == codePoints.size()
            && std::ranges::equal(codePoints, std::span<const std::uint32_t>(kept.text).first(kept.textCount)))
            return slot;
    }
}

void ShapedCellCache::forgetKept(std::uint32_t slot)
{
    Kept& kept = m_kept[slot];
    if (kept.newer != noSlot)
        m_kept[kept.newer].older = kept.older;
    else
        m_keptNewest = kept.older;
    if (kept.older != noSlot)
        m_kept[kept.older].newer = kept.newer;
    else
        m_keptOldest = kept.newer;

    // Out of the index by backward shift: each later occupant whose home lies
    // on the probe path up to the hole moves into it, so the chain stays dense
    // and no tombstone is left to lengthen the probes behind it.
    constexpr std::size_t mask = keptIndexCapacity - 1;
    std::size_t hole = static_cast<std::size_t>(kept.hash) & mask;
    while (m_keptIndex[hole] != slot)
        hole = (hole + 1) & mask;
    for (std::size_t scan = (hole + 1) & mask;; scan = (scan + 1) & mask) {
        const std::uint32_t occupant = m_keptIndex[scan];
        if (occupant == noSlot)
            break;
        const std::size_t home = static_cast<std::size_t>(m_kept[occupant].hash) & mask;
        if (((hole - home) & mask) < ((scan - home) & mask)) {
            m_keptIndex[hole] = occupant;
            hole = scan;
        }
    }
    m_keptIndex[hole] = noSlot;

    kept.newer = m_keptFree;
    m_keptFree = slot;
    --m_keptCount;
}

void ShapedCellCache::keepSegment(std::uint64_t hash, ShapedSegmentSpan codePoints, std::uint32_t shapeIdentity, ShapedCellSpan cells)
{
    if (codePoints.size() > keptSegmentWords || cells.size() > keptSegmentCells)
        return;
    if (m_kept.empty()) {
        m_kept.resize(keptSegmentCapacity);
        m_keptIndex.assign(keptIndexCapacity, noSlot);
    }
    if (findKept(hash, codePoints, shapeIdentity) != noSlot)
        return;
    if (m_keptCount == keptSegmentCapacity)
        forgetKept(m_keptOldest);

    std::uint32_t slot = noSlot;
    if (m_keptFree != noSlot) {
        slot = m_keptFree;
        m_keptFree = m_kept[slot].newer;
    } else {
        slot = m_keptTaken++;
    }
    Kept& kept = m_kept[slot];
    kept.hash = hash;
    kept.shapeIdentity = shapeIdentity;
    kept.textCount = static_cast<std::uint16_t>(codePoints.size());
    kept.cellCount = static_cast<std::uint16_t>(cells.size());
    std::ranges::copy(codePoints, kept.text.begin());
    std::ranges::copy(cells, kept.cells.begin());
    kept.older = m_keptNewest;
    kept.newer = noSlot;
    if (m_keptNewest != noSlot)
        m_kept[m_keptNewest].newer = slot;
    else
        m_keptOldest = slot;
    m_keptNewest = slot;

    constexpr std::size_t mask = keptIndexCapacity - 1;
    std::size_t cell = static_cast<std::size_t>(hash) & mask;
    while (m_keptIndex[cell] != noSlot)
        cell = (cell + 1) & mask;
    m_keptIndex[cell] = slot;
    ++m_keptCount;
}

void ShapedCellCache::keepDroppedSegments()
{
    for (const Entry& entry : m_previous.entries) {
        const ShapedSegmentSpan codePoints { m_previous.text.data() + entry.textOffset, entry.textCount };
        if (find(m_current, entry.hash, codePoints, entry.shapeIdentity) >= 0)
            continue; // migrated forward: the current frame holds it
        keepSegment(entry.hash, codePoints, entry.shapeIdentity,
            ShapedCellSpan { m_previous.cells.data() + entry.cellOffset, entry.cellCount });
    }
}

ShapedCellSpan ShapedCellCache::publish(ShapedSegmentSpan codePoints UBIQUITTY_NOESCAPE, std::uint32_t shapeIdentity, ShapedCellSpan cells UBIQUITTY_NOESCAPE)
{
    const std::uint64_t hash = foldSegment(codePoints, shapeIdentity);
    if (const int index = find(m_current, hash, codePoints, shapeIdentity); index >= 0) {
        const Entry& entry = m_current.entries[static_cast<std::size_t>(index)];
        return { m_current.cells.data() + entry.cellOffset, entry.cellCount };
    }
    return append(m_current, hash, codePoints, shapeIdentity, cells);
}

bool ShapedCellCache::holdsNoAnswer(ShapedSegmentSpan codePoints UBIQUITTY_NOESCAPE, std::uint32_t shapeIdentity)
{
    const int index = find(m_current, foldSegment(codePoints, shapeIdentity), codePoints, shapeIdentity);
    return index >= 0 && m_current.entries[static_cast<std::size_t>(index)].cellCount == 0;
}

void ShapedCellCache::endFrame()
{
    keepDroppedSegments();
    m_previous.clear();
    m_previous.entries.swap(m_current.entries);
    m_previous.text.swap(m_current.text);
    m_previous.cells.swap(m_current.cells);
    m_previous.index.swap(m_current.index);
    m_hits = 0;
    m_misses = 0;
}

} // namespace Ubiquitty::Rendering
