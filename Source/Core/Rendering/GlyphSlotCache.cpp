// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/GlyphSlotCache.h"

#include "Core/Rendering/GlyphBackendTypes.h"

#include <algorithm>
#include <cassert>

#include <ranges>

#include <cstring>

namespace Ubiquitty::Rendering {

namespace {
constexpr std::uint16_t noSlot = 0xFFFF; // LRU "no neighbour" sentinel
}

GlyphUV GlyphSlotGeometry::atlasRectForSlot(int slot, int capacity) const
{
    // The GLYPH BOX, which is narrower than the slot inset by its margins.
    //
    // A slot is the box plus a margin on each side PLUS one column of subpixel
    // phase headroom (GlyphSlotPolicy.h: `glyphWidth + 2*marginX + 1`), so
    // `slotWidth - 2*marginX` is one pixel wider than the box. That pixel is
    // where a glyph shifted by up to 7/8px for its phase lands, and it belongs
    // to the subpixel path, so `atlasRectForBuilder` includes it and this rect does not.
    //
    // This is the rect for the stretched-cell path, where the quad is the cell
    // and the box maps onto it. A rect carrying the headroom column samples a
    // fraction of the next phase into every cell — a glyph a hair too wide,
    // never anything that looks like a fault.
    const int column = columns > 0 ? slot % columns : 0;
    const int row = columns > 0 ? slot / columns : 0;
    const float boxLeft = static_cast<float>(column * slotWidthInPixels + glyphMarginInPixels.width);
    const float boxTop = static_cast<float>(row * slotHeightInPixels + glyphMarginInPixels.height);
    GlyphUV uv;
    uv.minU = boxLeft;
    uv.minV = boxTop;
    uv.maxU = boxLeft + static_cast<float>(glyphWidthInPixels);
    uv.maxV = boxTop + static_cast<float>(slotHeightInPixels - 2 * glyphMarginInPixels.height);
    (void)capacity;
    return uv;
}

GlyphUV GlyphSlotGeometry::atlasRectForSlotFull(int slot, int capacity, int cellsWide) const
{
    const int column = columns > 0 ? slot % columns : 0;
    const int row = columns > 0 ? slot / columns : 0;
    // A span narrower than one slot is a caller error, not a request; clamp
    // rather than emit an inverted rect that samples nothing.
    const int span = cellsWide > 1 ? cellsWide : 1;
    GlyphUV uv;
    uv.minU = static_cast<float>(column * slotWidthInPixels);
    uv.minV = static_cast<float>(row * slotHeightInPixels);
    uv.maxU = static_cast<float>((column + span) * slotWidthInPixels);
    uv.maxV = static_cast<float>((row + 1) * slotHeightInPixels);
    (void)capacity;
    return uv;
}

GlyphUV GlyphSlotGeometry::atlasRectForBuilder(int slot, int capacity) const
{
    const float marginX = static_cast<float>(glyphMarginInPixels.width);
    const float marginY = static_cast<float>(glyphMarginInPixels.height);
    GlyphUV uv = atlasRectForSlotFull(slot, capacity);
    uv.minU += marginX;
    uv.minV += marginY;
    uv.maxU -= marginX;
    uv.maxV -= marginY;
    return uv;
}

GlyphSlotCache::GlyphSlotCache(int capacity, int hashBuckets, int columns)
    : m_capacity(capacity)
    , m_columns(columns > 0 ? columns : 1)
    , m_hashMask(static_cast<std::uint32_t>(hashBuckets - 1))
    , m_hashKey(static_cast<std::size_t>(hashBuckets))
    , m_hashSlot(static_cast<std::size_t>(hashBuckets))
    , m_keyForSlot(static_cast<std::size_t>(capacity))
    , m_lruPrev(static_cast<std::size_t>(capacity))
    , m_lruNext(static_cast<std::size_t>(capacity))
    , m_slotFrame(static_cast<std::size_t>(capacity))
    , m_pinCount(static_cast<std::size_t>(capacity))
    , m_contentsForSlot(static_cast<std::size_t>(capacity))
{
    m_freshThisFrame.reserve(static_cast<std::size_t>(capacity));
    reset();
}

void GlyphSlotCache::reset()
{
    for (auto& k : m_hashKey)
        k = emptyKey;
    for (auto& slot : m_hashSlot)
        slot = 0;
    for (auto& k : m_keyForSlot)
        k = emptyKey;
    for (auto& p : m_lruPrev)
        p = noSlot;
    for (auto& n : m_lruNext)
        n = noSlot;
    for (auto& f : m_slotFrame)
        f = 0;
    for (auto& c : m_contentsForSlot)
        c.clear();
    std::ranges::fill(m_ownedInkMeasured, std::uint8_t { 0 });
    std::ranges::fill(m_inkRowTop, std::int16_t { 1 });
    std::ranges::fill(m_inkRowBottom, std::int16_t { 0 });
    m_lruHead = noSlot;
    m_lruTail = noSlot;
    m_used = 0;
    m_freshThisFrame.clear();
    m_frame = 0; // 0 = no pass open → plain LRU; beginFrame() opens pass 1+
    // The flush emptied every slot, so nothing a retained frame references
    // survives to protect; the sets refill on each view's next frame.
    for (auto& pin : m_pinCount)
        pin = 0;
    m_pinnedSlots = 0;
    for (auto& ring : m_retainRings) {
        for (auto& epoch : ring.epochs)
            epoch.clear();
        ring.head = 0;
        ring.lastRetainFrame = 0;
    }
    m_stampsThisFrame = 0;
    m_refusalsThisFrame = 0;
    m_saturationsThisFrame = 0;
}

int GlyphSlotCache::acquireRetainLease()
{
    if (!m_freeLeases.empty()) {
        const int lease = m_freeLeases.back();
        m_freeLeases.pop_back();
        return lease;
    }
    m_retainRings.emplace_back();
    return static_cast<int>(m_retainRings.size()) - 1;
}

void GlyphSlotCache::releaseRetainLease(int lease)
{
    if (lease < 0 || lease >= static_cast<int>(m_retainRings.size()))
        return;
    rebaselineRetainedSlots(lease);
    m_freeLeases.push_back(lease);
}

void GlyphSlotCache::dropEpochPins(std::vector<std::uint16_t>& epoch)
{
    for (const std::uint16_t slot : epoch) {
        if (m_pinCount[slot] > 0 && --m_pinCount[slot] == 0)
            --m_pinnedSlots;
    }
    epoch.clear();
}

void GlyphSlotCache::rebaselineRetainedSlots(int lease)
{
    if (lease < 0 || lease >= static_cast<int>(m_retainRings.size()))
        return;
    RetainRing& ring = m_retainRings[static_cast<std::size_t>(lease)];
    for (auto& epoch : ring.epochs)
        dropEpochPins(epoch);
    ring.head = 0;
}

void GlyphSlotCache::retainTouchedSlots(int lease)
{
    if (lease < 0 || lease >= static_cast<int>(m_retainRings.size()) || m_frame == 0)
        return;
    RetainRing& ring = m_retainRings[static_cast<std::size_t>(lease)];
    ring.lastRetainFrame = m_frame;
    // The rotation happens whether or not the frame stamped anything: a quiet
    // frame ages the ring exactly like a busy one, which is how a view that
    // stopped churning hands its footprint back without waiting for pressure.
    ring.head = (ring.head + 1) % retainEpochDepth;
    std::vector<std::uint16_t>& epoch = ring.epochs[static_cast<std::size_t>(ring.head)];
    dropEpochPins(epoch);
    if (m_stampsThisFrame == 0)
        return; // nothing fresh to pin; the rotation above already aged the ring
    // A slot may sit in several epochs at once — touched by several recent
    // frames — and carries one pin per appearance; the ring's rotation then
    // releases them symmetrically, so no membership set is needed.
    for (int slot = 0; slot < m_capacity; ++slot) {
        if (m_slotFrame[static_cast<std::size_t>(slot)] != m_frame)
            continue;
        epoch.push_back(static_cast<std::uint16_t>(slot));
        if (m_pinCount[static_cast<std::size_t>(slot)]++ == 0)
            ++m_pinnedSlots;
    }
}

bool GlyphSlotCache::reclaimStaleLeasePins()
{
    // The frame counter ticks once per drawing view per display frame, so the
    // stale bound is spelled in ticks by multiplying the ring depth by the
    // leases that can be ticking it. One is the floor: a leaseless caller
    // still advances the counter, and a zero bound would reclaim a lease the
    // same frame it retained.
    const int activeLeases = static_cast<int>(m_retainRings.size()) - static_cast<int>(m_freeLeases.size());
    const std::uint32_t staleAfterFrames = static_cast<std::uint32_t>(activeLeases > 1 ? activeLeases : 1)
        * static_cast<std::uint32_t>(retainEpochDepth) * staleLeaseRingSlack;
    bool didReclaim = false;
    for (int lease = 0; lease < static_cast<int>(m_retainRings.size()); ++lease) {
        RetainRing& ring = m_retainRings[static_cast<std::size_t>(lease)];
        if (ring.lastRetainFrame == 0 || m_frame - ring.lastRetainFrame <= staleAfterFrames)
            continue;
        bool hasPins = false;
        for (const auto& epoch : ring.epochs)
            hasPins = hasPins || !epoch.empty();
        if (!hasPins)
            continue;
        rebaselineRetainedSlots(lease);
        didReclaim = true;
    }
    return didReclaim;
}

int GlyphSlotCache::hashFind(Key key) const
{
    std::uint32_t h = hashOf(key);
    for (std::uint32_t probe = 0; probe <= m_hashMask; ++probe) {
        std::uint32_t i = (h + probe) & m_hashMask;
        Key v = m_hashKey[i];
        if (v == emptyKey)
            return -1;
        if (v == key)
            return static_cast<int>(i);
    }
    return -1;
}

void GlyphSlotCache::hashInsert(Key key, std::uint16_t slot)
{
    // First empty bucket, never an existing same-key bucket: a verified key
    // holds one bucket per live collider, and overwriting a same-key bucket
    // would orphan that sibling's slot while its instances still sample it.
    std::uint32_t h = hashOf(key);
    for (std::uint32_t probe = 0; probe <= m_hashMask; ++probe) {
        std::uint32_t i = (h + probe) & m_hashMask;
        if (m_hashKey[i] == emptyKey) {
            m_hashKey[i] = key;
            m_hashSlot[i] = slot;
            return;
        }
    }
}

void GlyphSlotCache::hashRemoveEntry(Key key, std::uint16_t slot)
{
    std::uint32_t h = hashOf(key);
    for (std::uint32_t probe = 0; probe <= m_hashMask; ++probe) {
        std::uint32_t i = (h + probe) & m_hashMask;
        Key v = m_hashKey[i];
        if (v == emptyKey)
            return;
        if (v == key && m_hashSlot[i] == slot) {
            hashRemoveAt(i);
            return;
        }
    }
}

void GlyphSlotCache::hashRemoveAt(std::uint32_t gap)
{
    // Backward-shift deletion: walk the probe chain and pull any entry whose
    // ideal bucket is at or before the gap back into it, so lookups for
    // displaced keys still terminate. The scan index `i` advances one slot per
    // step independently of `gap`; fusing them (i = gap + probe) skips the slot
    // after every shift and orphans keys behind their probe chain.
    m_hashKey[gap] = emptyKey;
    std::uint32_t i = gap;
    for (std::uint32_t step = 1; step <= m_hashMask; ++step) {
        i = (i + 1) & m_hashMask;
        Key v = m_hashKey[i];
        if (v == emptyKey)
            return;
        std::uint32_t ideal = hashOf(v);
        std::uint32_t distGap = (gap - ideal) & m_hashMask;
        std::uint32_t distI = (i - ideal) & m_hashMask;
        if (distGap < distI) {
            m_hashKey[gap] = v;
            m_hashSlot[gap] = m_hashSlot[i];
            m_hashKey[i] = emptyKey;
            gap = i;
        }
    }
}

void GlyphSlotCache::lruDetach(std::uint16_t slot)
{
    const std::uint16_t prev = m_lruPrev[slot];
    const std::uint16_t next = m_lruNext[slot];
    if (prev != noSlot)
        m_lruNext[prev] = next;
    if (next != noSlot)
        m_lruPrev[next] = prev;
    if (m_lruHead == slot)
        m_lruHead = next;
    if (m_lruTail == slot)
        m_lruTail = prev;
}

void GlyphSlotCache::lruTouch(std::uint16_t slot)
{
    if (m_lruHead == slot)
        return;
    lruDetach(slot);
    // Push to the MRU head.
    m_lruPrev[slot] = noSlot;
    m_lruNext[slot] = m_lruHead;
    if (m_lruHead != noSlot)
        m_lruPrev[m_lruHead] = slot;
    m_lruHead = slot;
    if (m_lruTail == noSlot)
        m_lruTail = slot;
}

int GlyphSlotCache::slotForKey(Key key, bool* fresh)
{
    int index = hashFind(key);
    if (index >= 0) {
        std::uint16_t slot = m_hashSlot[index];
        // Promote once per frame. Within a pass the order among touched slots
        // cannot matter — every one is exempt from eviction until the next
        // beginFrame — and text repeats a few dozen glyphs, so most hits
        // would re-splice a slot this frame already placed. Frame 0 means no
        // pass discipline (beginFrame never called); every touch then
        // promotes.
        if (m_frame == 0 || m_slotFrame[slot] != m_frame) {
            lruTouch(slot);
            touchThisFrame(slot);
        }
        if (fresh)
            *fresh = false;
        return slot;
    }
    return allocateSlot(key, fresh);
}

int GlyphSlotCache::allocateSlot(Key key, bool* fresh)
{
    std::uint16_t slot;
    if (m_lruTail != noSlot && m_keyForSlot[m_lruTail] == emptyKey && m_pinCount[m_lruTail] == 0) {
        // The pin test is defensive: a forgotten slot was fresh this frame and
        // a fresh slot is never in a retained set, so a pinned one here would
        // mean the bookkeeping broke — and serving it would repaint an idle
        // view's quad, the exact corruption the pins exist to prevent.
        // A slot whose key was forgotten holds pixels nothing can name, so it is
        // taken before fresh capacity is. `forgetKey` pushes each one to the tail,
        // so successive misses walk back through them in turn.
        //
        // Without this the reclaim is invisible until the atlas is FULL, and a
        // keyspace that turns over — a soft font retiring one set per DECDLD —
        // spends a slot per download until the whole cache has cycled once.
        slot = m_lruTail;
    } else if (m_used < m_capacity) {
        slot = static_cast<std::uint16_t>(m_used++);
    } else {
        // Full: evict a slot this frame has not touched. Which one is sampled
        // rather than taken from the LRU tail, because the workload that fills
        // this cache is a scan: reading code with a ligature font walks more
        // distinct strips than the atlas holds, and every one comes round again
        // on the way back. LRU is the worst policy for exactly that shape — each
        // key leaves just before its next use, so the second pass hits nothing.
        //
        // The sequence is seeded, so a run reproduces its own layout. A working
        // set that fits never reaches here at all: eviction begins only once
        // every slot is spoken for.
        slot = m_lruTail;
        if (m_frame != 0) {
            for (int probe = 0; probe < evictionSampleCount; ++probe) {
                m_evictionSeed = m_evictionSeed * 1664525u + 1013904223u;
                const auto candidate = static_cast<std::uint16_t>((m_evictionSeed >> 8) % static_cast<std::uint32_t>(m_capacity));
                if (m_slotFrame[candidate] != m_frame && m_pinCount[candidate] == 0) {
                    slot = candidate;
                    break;
                }
            }
        }
        if (m_frame != 0 && m_slotFrame[slot] == m_frame) {
            // A current-frame blocker refuses outright, pins or no pins: a
            // cache with no lease anywhere must allocate bit-for-bit as one
            // with no pins does — the portable and Vulkan arms render against
            // goldens minted under exactly that refusal, and a rescue walk here
            // changes their pixels. Not a counted refusal: the count asks a
            // retained set to shrink, and no re-baseline unblocks a frame that
            // touched every slot itself.
            // Tallied apart for exactly that reason — a frame that outgrew the
            // cache by itself and a frame crowded out by another view's pins
            // both draw the fallback, and a single number cannot tell a
            // capacity problem from a pinning one.
            ++m_saturationsThisFrame;
            if (fresh)
                *fresh = false;
            return -1;
        }
        // The tail can be pinned by an idle view's retained frame; walking a
        // bounded stretch of the recency list finds the coldest unpinned slot
        // instead of handing that view's pixels to a new glyph. Bounded, so a
        // cache pinned nearly solid costs a short walk and a refusal rather
        // than a scan — the refusal draws the caller's fallback, softer but
        // never another window's ink. Only a pin can be the blocker by now, so
        // a cache nobody pins never reaches the walk.
        if (m_frame != 0 && m_pinCount[slot] != 0) {
            constexpr int tailWalkBound = 64;
            // Two passes at most: pins held by leases whose owners stopped
            // drawing protect no in-flight frame, so the walk failing once
            // reclaims them and tries again before it charges a refusal.
            for (int attempt = 0; attempt < 2; ++attempt) {
                std::uint16_t candidate = m_lruTail;
                int walked = 0;
                while (candidate != noSlot && walked < tailWalkBound) {
                    if (m_slotFrame[candidate] != m_frame && m_pinCount[candidate] == 0) {
                        slot = candidate;
                        break;
                    }
                    candidate = m_lruPrev[candidate];
                    ++walked;
                }
                if (m_slotFrame[slot] != m_frame && m_pinCount[slot] == 0)
                    break;
                if (attempt == 0 && reclaimStaleLeasePins())
                    continue;
                ++m_refusalsThisFrame;
                if (fresh)
                    *fresh = false;
                return -1;
            }
        }
        Key victim = m_keyForSlot[slot];
        if (victim != emptyKey)
            hashRemoveEntry(victim, slot); // by (key, slot): the key may have live siblings
    }
    m_keyForSlot[slot] = key;
    hashInsert(key, slot);
    lruTouch(slot);
    touchThisFrame(slot);
    m_contentsForSlot[slot].clear(); // recycled slot: any verified-cluster contents are gone
    clearOwnedInkRecord(slot);
    if (m_freshThisFrame.size() < m_freshThisFrame.capacity())
        m_freshThisFrame.push_back(slot);
    if (fresh)
        *fresh = true;
    return slot;
}

void GlyphSlotCache::forgetSlotsAssignedSince(int mark)
{
    if (mark < 0)
        return;
    while (static_cast<int>(m_freshThisFrame.size()) > mark) {
        const std::uint16_t slot = m_freshThisFrame.back();
        m_freshThisFrame.pop_back();
        // A slot forgotten and reassigned within the frame appears once per
        // assignment; only its current key is this frame's to forget.
        if (m_slotFrame[slot] == m_frame)
            forgetKeyAtSlot(m_keyForSlot[slot], slot);
    }
}

void GlyphSlotCache::forgetKey(Key key)
{
    const int index = hashFind(key);
    if (index < 0)
        return;
    forgetKeyAtSlot(key, m_hashSlot[index]);
}

void GlyphSlotCache::forgetKeyAtSlot(Key key, int slot)
{
    if (key == emptyKey || slot < 0 || slot >= m_capacity || m_keyForSlot[slot] != key) {
        return;
    }
    hashRemoveEntry(key, static_cast<std::uint16_t>(slot));
    // The slot outlives the key, so its recorded owner has to go too: an
    // eviction later reads m_keyForSlot to unhash the victim, and a key already
    // unhashed would send it looking for an entry that is gone.
    m_keyForSlot[slot] = emptyKey;
    m_contentsForSlot[slot].clear();
    clearOwnedInkRecord(slot);
    // Nobody can want these pixels, so the slot goes to the front of the queue
    // for reuse rather than ageing to it behind every live glyph. Dropping the
    // frame stamp is what makes that reachable within this frame: slotForKey
    // refuses to evict a slot the current pass has touched, and this one was
    // touched by the fault that then failed.
    m_slotFrame[slot] = 0;
    lruEvictToTail(static_cast<std::uint16_t>(slot));
}

void GlyphSlotCache::lruEvictToTail(std::uint16_t slot)
{
    if (m_lruTail == slot)
        return;
    lruDetach(slot);
    m_lruNext[slot] = noSlot;
    m_lruPrev[slot] = m_lruTail;
    if (m_lruTail != noSlot)
        m_lruNext[m_lruTail] = slot;
    m_lruTail = slot;
    if (m_lruHead == noSlot)
        m_lruHead = slot;
}

int GlyphSlotCache::slotForKeyVerified(Key key, const std::uint32_t* contents, int count, bool* fresh)
{
    // A hit is a bucket whose key and stored contents both match. A key hit
    // whose contents differ is a live sibling collider, not this run — keep
    // probing.
    const std::uint32_t h = hashOf(key);
    for (std::uint32_t probe = 0; probe <= m_hashMask; ++probe) {
        const std::uint32_t i = (h + probe) & m_hashMask;
        const Key v = m_hashKey[i];
        if (v == emptyKey)
            break;
        if (v != key)
            continue;
        const std::uint16_t slot = m_hashSlot[i];
        const std::vector<std::uint32_t>& stored = m_contentsForSlot[slot];
        if (stored.size() == static_cast<std::size_t>(count)
            && std::memcmp(stored.data(), contents, static_cast<std::size_t>(count) * sizeof(std::uint32_t)) == 0) {
            // Once per frame, as slotForKey's hit path says.
            if (m_frame == 0 || m_slotFrame[slot] != m_frame) {
                lruTouch(slot);
                touchThisFrame(slot);
            }
            if (fresh)
                *fresh = false;
            return slot;
        }
    }
    bool isAllocationFresh = false;
    const int slot = allocateSlot(key, &isAllocationFresh); // clears any verified contents
    if (slot >= 0)
        m_contentsForSlot[slot].assign(contents, contents + count);
    if (fresh)
        *fresh = isAllocationFresh;
    return slot;
}

void GlyphSlotCache::enableOwnedInkRecords(int columnsPerSlot, int slotWidthInPixels)
{
    if (columnsPerSlot <= 0 || slotWidthInPixels <= 0 || m_capacity <= 0)
        return;
    m_ownedInkColumnsPerSlot = columnsPerSlot;
    m_ownedInkSlotWidthInPixels = slotWidthInPixels;
    const std::size_t entries = static_cast<std::size_t>(m_capacity) * static_cast<std::size_t>(columnsPerSlot);
    m_ownedInkLeft.assign(entries, 0);
    m_ownedInkRight.assign(entries, 0);
    m_inkLeft.assign(entries, 0);
    m_inkRight.assign(entries, 0);
    m_ownedInkMeasured.assign(static_cast<std::size_t>(m_capacity), 0);
    m_inkRowTop.assign(static_cast<std::size_t>(m_capacity), 1);
    m_inkRowBottom.assign(static_cast<std::size_t>(m_capacity), 0);
}

void GlyphSlotCache::beginOwnedInkRecord(int slot)
{
    if (m_ownedInkColumnsPerSlot <= 0 || slot < 0 || slot >= m_capacity)
        return;
    const std::size_t base = static_cast<std::size_t>(slot) * static_cast<std::size_t>(m_ownedInkColumnsPerSlot);
    std::fill_n(m_ownedInkLeft.begin() + static_cast<std::ptrdiff_t>(base), m_ownedInkColumnsPerSlot, std::uint16_t { 0 });
    std::fill_n(m_ownedInkRight.begin() + static_cast<std::ptrdiff_t>(base), m_ownedInkColumnsPerSlot, std::uint16_t { 0 });
    std::fill_n(m_inkLeft.begin() + static_cast<std::ptrdiff_t>(base), m_ownedInkColumnsPerSlot, std::uint16_t { 0 });
    std::fill_n(m_inkRight.begin() + static_cast<std::ptrdiff_t>(base), m_ownedInkColumnsPerSlot, std::uint16_t { 0 });
    m_ownedInkMeasured[static_cast<std::size_t>(slot)] = 1;
}

void GlyphSlotCache::recordShapedGlyphInk(int slot, int cluster, int leadingContext, int drawnCells,
    int inkLeftInPixels, int inkRightInPixels, int fringeInPixels)
{
    if (m_ownedInkColumnsPerSlot <= 0 || slot < 0 || slot >= m_capacity)
        return;
    if (m_ownedInkMeasured[static_cast<std::size_t>(slot)] == 0)
        return;
    // Context clusters drew, but their ink belongs to a neighbouring chunk;
    // recording them would hand a drawn cell a quad over the margin the
    // context insets exist to keep it out of.
    const int column = shapedGlyphColumn(cluster, leadingContext, drawnCells);
    if (column < 0 || column >= m_ownedInkColumnsPerSlot)
        return;
    const int inkLeft = std::clamp(inkLeftInPixels, 0, m_ownedInkSlotWidthInPixels);
    const int inkRight = std::clamp(inkRightInPixels, 0, m_ownedInkSlotWidthInPixels);
    if (inkRight <= inkLeft)
        return;
    const int fringe = std::max(fringeInPixels, 0);
    const int drawnLeft = std::clamp(inkLeftInPixels - fringe, 0, m_ownedInkSlotWidthInPixels);
    const int drawnRight = std::clamp(inkRightInPixels + fringe, 0, m_ownedInkSlotWidthInPixels);
    const std::size_t index = static_cast<std::size_t>(slot) * static_cast<std::size_t>(m_ownedInkColumnsPerSlot)
        + static_cast<std::size_t>(column);
    const auto widen = [index](std::vector<std::uint16_t>& lefts, std::vector<std::uint16_t>& rights, int left, int right) {
        if (rights[index] <= lefts[index]) {
            lefts[index] = static_cast<std::uint16_t>(left);
            rights[index] = static_cast<std::uint16_t>(right);
            return;
        }
        lefts[index] = std::min(lefts[index], static_cast<std::uint16_t>(left));
        rights[index] = std::max(rights[index], static_cast<std::uint16_t>(right));
    };
    widen(m_ownedInkLeft, m_ownedInkRight, drawnLeft, drawnRight);
    widen(m_inkLeft, m_inkRight, inkLeft, inkRight);
}

void GlyphSlotCache::finalizeOwnedInkRecord(int slot)
{
    if (m_ownedInkColumnsPerSlot <= 0 || slot < 0 || slot >= m_capacity)
        return;
    if (m_ownedInkMeasured[static_cast<std::size_t>(slot)] == 0)
        return;
    const std::size_t base = static_cast<std::size_t>(slot) * static_cast<std::size_t>(m_ownedInkColumnsPerSlot);
    for (int column = 0; column + 1 < m_ownedInkColumnsPerSlot; ++column) {
        const std::size_t here = base + static_cast<std::size_t>(column);
        const std::size_t next = here + 1;
        const bool hasInkHere = m_ownedInkRight[here] > m_ownedInkLeft[here];
        const bool hasInkNext = m_ownedInkRight[next] > m_ownedInkLeft[next];
        if (!hasInkHere || !hasInkNext)
            continue;
        if (m_ownedInkRight[here] <= m_ownedInkLeft[next])
            continue;
        const std::uint16_t midpoint = static_cast<std::uint16_t>(
            (static_cast<int>(m_ownedInkRight[here]) + static_cast<int>(m_ownedInkLeft[next])) / 2);
        m_ownedInkRight[here] = std::max(midpoint, m_ownedInkLeft[here]);
        m_ownedInkLeft[next] = std::min(midpoint, m_ownedInkRight[next]);
    }
}

void GlyphSlotCache::clearOwnedInkRecord(int slot)
{
    if (m_ownedInkColumnsPerSlot <= 0 || slot < 0 || slot >= m_capacity)
        return;
    m_ownedInkMeasured[static_cast<std::size_t>(slot)] = 0;
    // The rows belong to the raster too: a slot handed to another key or
    // dropped holds nothing they describe.
    m_inkRowTop[static_cast<std::size_t>(slot)] = 1;
    m_inkRowBottom[static_cast<std::size_t>(slot)] = 0;
}

bool GlyphSlotCache::slotHasOwnedInk(int slot) const
{
    if (m_ownedInkColumnsPerSlot <= 0 || slot < 0 || slot >= m_capacity)
        return false;
    return m_ownedInkMeasured[static_cast<std::size_t>(slot)] != 0;
}

GlyphSlotCache::OwnedInkExtent GlyphSlotCache::ownedInkForSlot(int slot, int coveredColumn) const
{
    if (!slotHasOwnedInk(slot) || coveredColumn < 0 || coveredColumn >= m_ownedInkColumnsPerSlot)
        return {};
    const std::size_t index = static_cast<std::size_t>(slot) * static_cast<std::size_t>(m_ownedInkColumnsPerSlot)
        + static_cast<std::size_t>(coveredColumn);
    return { static_cast<float>(m_ownedInkLeft[index]), static_cast<float>(m_ownedInkRight[index]),
        static_cast<float>(m_inkLeft[index]), static_cast<float>(m_inkRight[index]) };
}

void GlyphSlotCache::recordInkRows(int slot, InkPlaneSpan coverage, int widthInPixels, int heightInPixels)
{
    if (m_inkRowTop.empty() || slot < 0 || slot >= m_capacity || widthInPixels <= 0 || heightInPixels <= 0
        || coverage.size() < static_cast<std::size_t>(widthInPixels) * static_cast<std::size_t>(heightInPixels))
        return;
    int top = heightInPixels;
    int bottom = -1;
    for (int y = 0; y < heightInPixels; ++y) {
        const auto row = coverage.subspan(static_cast<std::size_t>(y) * static_cast<std::size_t>(widthInPixels),
            static_cast<std::size_t>(widthInPixels));
        if (std::ranges::any_of(row, [](std::uint8_t texel) { return texel != 0; })) {
            top = std::min(top, y);
            bottom = y;
        }
    }
    const std::size_t index = static_cast<std::size_t>(slot);
    if (bottom < 0) {
        m_inkRowTop[index] = 0;
        m_inkRowBottom[index] = 0;
        return;
    }
    m_inkRowTop[index] = static_cast<std::int16_t>(top);
    m_inkRowBottom[index] = static_cast<std::int16_t>(bottom + 1);
}

GlyphSlotCache::InkRows GlyphSlotCache::inkRowsForSlot(int slot) const
{
    if (m_inkRowTop.empty() || slot < 0 || slot >= m_capacity)
        return {};
    const std::size_t index = static_cast<std::size_t>(slot);
    if (m_inkRowTop[index] > m_inkRowBottom[index])
        return {};
    return InkRows { true, static_cast<float>(m_inkRowTop[index]), static_cast<float>(m_inkRowBottom[index]) };
}

} // namespace Ubiquitty::Rendering
