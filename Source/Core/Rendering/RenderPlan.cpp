// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/RenderPlan.h"
#include "Core/CellPen.h"

#include <algorithm>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#include <span>
#endif

#include "Core/TerminalOperations.h"

namespace Ubiquitty::Rendering {

std::uint64_t buildFrameDigests(const PlanInputs& in, RowDigest* out)
{
    const int rows = in.rows;
    const int columns = in.columns;
    bool blinkPresent = false;
    for (int r = 0; r < rows; ++r) {
        const Cell* row = in.rowPointers ? in.rowPointers[r] : in.cells + static_cast<std::size_t>(r) * columns;
        // Per-row salt: fold the influences that change a row's pixels without
        // changing its cell bytes. Frame-global state rides the epoch below.
        std::uint64_t salt = in.lineAttributes ? static_cast<std::uint64_t>(in.lineAttributes[r]) : 0;
        if (in.rowMarks)
            salt ^= static_cast<std::uint64_t>(in.rowMarks[r]) << 8;
        if (r >= in.hoverRow0 && r <= in.hoverRow1) {
            const int hc0 = (r == in.hoverRow0) ? in.hoverColumn0 : 0;
            const int hc1 = (r == in.hoverRow1) ? in.hoverColumn1 : columns - 1;
            salt ^= 0x484F5645ull ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(hc0)) << 17) ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(hc1)) << 37);
        }
        // A different constant and different shifts from the hover above, so a
        // hover and a match over the same columns do not cancel each other in
        // the exclusive or and leave the row reading clean.
        if (r >= in.matchRow0 && r <= in.matchRow1) {
            const int mc0 = (r == in.matchRow0) ? in.matchColumn0 : 0;
            const int mc1 = (r == in.matchRow1) ? in.matchColumn1 : columns - 1;
            salt ^= 0x4D415443ull ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(mc0)) << 5) ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(mc1)) << 29);
        }
        if (in.cursorRow == r) {
            salt ^= 0x43555253ull ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(in.cursorColumn)) << 8) ^ (static_cast<std::uint64_t>(in.cursorFadeBucket) << 32) ^ (static_cast<std::uint64_t>(in.cursorInvert ? 1u : 0u) << 48);
        }
        out[r] = rowDigest(std::as_bytes(std::span(row, static_cast<std::size_t>(columns))), salt);
        // The per-cell blink scan is a second full-grid pass; skip it when the
        // caller's sticky hint proves no blink can be present (the common case).
        if (in.blinkPresentHint && !blinkPresent) {
            for (int c = 0; c < columns; ++c)
                if (Ubiquitty::cellPen(row[c]).attributes.contains(Ubiquitty::VT::Attribute::Blink)) {
                    blinkPresent = true;
                    break;
                }
        }
    }
    // Epoch: the renderer's global blob, plus the blink clock only when SGR-5
    // blink text is on screen (so a blink flip forces Full without disabling the
    // cache the rest of the time).
    std::uint64_t epoch = rowDigest({ static_cast<const std::byte*>(in.globalState), in.globalStateLength }, 0xE9ull);
    if (blinkPresent)
        epoch = rowDigest(std::as_bytes(std::span(&in.blinkClock, 1)), epoch);
    return epoch;
}

// 64-bit content digest over the row bytes, then a splitmix64 finaliser that
// folds in the salt. The digest only has to change when content changes, not
// resist an adversary, and is never persisted or sent on a wire — so the bulk
// mix may differ per platform as long as the finaliser scatters near-identical
// inputs. AArch64 uses the CRC32C instruction (ARMv8 baseline) striped across
// four lanes to overlap its 3-cycle latency; elsewhere word-at-a-time FNV-1a.
RowDigest rowDigest(std::span<const std::byte> bytes, std::uint64_t salt)
{
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::size_t byteCount = bytes.size();
    std::size_t i = 0;
    std::uint64_t h;
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    // Four independent CRC32C lanes striped across the row; one lane would
    // serialize on the 3-cycle latency. Not a true CRC of the buffer — any
    // content-sensitive mix works, and the distinct fold multipliers below keep
    // a change in one lane from cancelling another.
    std::uint32_t lane0 = 0xFFFFFFFFu;
    std::uint32_t lane1 = 0x9E3779B9u;
    std::uint32_t lane2 = 0x85EBCA6Bu;
    std::uint32_t lane3 = 0xC2B2AE35u;
    for (; i + 4 * sizeof(std::uint64_t) <= byteCount; i += 4 * sizeof(std::uint64_t)) {
        std::uint64_t w0, w1, w2, w3;
        std::memcpy(&w0, p + i, sizeof(w0)); // alignment-safe; lowers to one load
        std::memcpy(&w1, p + i + 8, sizeof(w1));
        std::memcpy(&w2, p + i + 16, sizeof(w2));
        std::memcpy(&w3, p + i + 24, sizeof(w3));
        lane0 = __crc32cd(lane0, w0);
        lane1 = __crc32cd(lane1, w1);
        lane2 = __crc32cd(lane2, w2);
        lane3 = __crc32cd(lane3, w3);
    }
    // Word + byte tails for the small non-row callers (globalState,
    // blinkClock) whose byteCount may not be 32-aligned.
    for (; i + sizeof(std::uint64_t) <= byteCount; i += sizeof(std::uint64_t)) {
        std::uint64_t word;
        std::memcpy(&word, p + i, sizeof(word));
        lane0 = __crc32cd(lane0, word);
    }
    for (; i < byteCount; ++i)
        lane0 = __crc32cb(lane0, p[i]);
    h = ((static_cast<std::uint64_t>(lane0) << 32) | lane1) * 0x9E3779B97F4A7C15ull;
    h ^= ((static_cast<std::uint64_t>(lane2) << 32) | lane3) * 0xC2B2AE3D27D4EB4Full;
#else
    h = 1469598103934665603ull; // FNV offset basis
    // Mix 8 bytes per multiply (a row's byteCount is a multiple of 8).
    for (; i + sizeof(std::uint64_t) <= byteCount; i += sizeof(std::uint64_t)) {
        std::uint64_t word;
        std::memcpy(&word, p + i, sizeof(word)); // alignment-safe; lowers to one load
        h ^= word;
        h *= 1099511628211ull; // FNV prime
    }
    for (; i < byteCount; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
#endif
    // Fold in the salt, then splitmix64 finaliser so near-identical inputs scatter.
    h ^= salt + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

void FrameDamageTracker::reset()
{
    m_havePrev = false;
    m_prev.clear();
    m_prevEpoch = ~std::uint64_t { 0 };
    m_committedFramesSinceFull = 0;
    m_stagedWasFull = false;
}

const FramePlan& FrameDamageTracker::plan(const RowDigest* digests, int rows, std::uint64_t epoch)
{
    if (rows < 0)
        rows = 0;
    m_staged.assign(digests, digests + rows);
    if (static_cast<int>(m_dirty.size()) != rows)
        m_dirty.assign(static_cast<std::size_t>(rows), 0);

    m_plan.rows = rows;
    m_plan.scrollDelta = 0;
    m_stagedEpoch = epoch;
    m_stagedWasFull = false;

    // First frame, a dimension/global change, or the collision-recovery cadence
    // forces a rebuild. The cadence makes digest equality a bounded cache hint:
    // even an adversarial collision cannot leave stale pixels indefinitely.
    const bool dimsChanged = !m_havePrev || static_cast<int>(m_prev.size()) != rows;
    const bool epochChanged = epoch != m_prevEpoch;
    const bool collisionRecoveryDue = m_havePrev && m_committedFramesSinceFull >= collisionRecoveryFrameInterval - 1;
    if (dimsChanged || epochChanged || collisionRecoveryDue) {
        std::ranges::fill(m_dirty, std::uint8_t { 1 });
        m_plan.kind = FrameKind::Full;
        m_plan.dirtyFirst = 0;
        m_plan.dirtyLast = rows - 1;
        m_plan.dirtyRows = m_dirty.data();
        m_stagedWasFull = true;
        return m_plan;
    }

    // Same dims + same epoch: diff row-by-row.
    int first = rows, last = -1, changed = 0;
    for (int r = 0; r < rows; ++r) {
        const bool d = m_staged[r] != m_prev[r];
        m_dirty[r] = d ? 1 : 0;
        if (d) {
            if (r < first)
                first = r;
            last = r;
            ++changed;
        }
    }

    if (changed == 0) {
        m_plan.kind = FrameKind::Unchanged;
        m_plan.dirtyFirst = 0;
        m_plan.dirtyLast = -1;
        m_plan.dirtyRows = m_dirty.data();
        return m_plan;
    }

    // Explain the change as a single uniform vertical scroll: up by N maps new
    // row r to old r+N, leaving N fresh rows at the bottom; down is the mirror.
    // Accept only when exactly one delta gives a byte-perfect overlap — an
    // ambiguous match (repeated/blank rows) falls back to the per-row Dirty
    // path, which never under-paints.
    int foundDelta = 0, hits = 0;
    const int maxDelta = (rows - 1 < maxScrollRows) ? rows - 1 : maxScrollRows;
    for (int delta = 1; delta <= maxDelta && hits < 2; ++delta) {
        // Scroll up by `delta`: current[i] == prev[i+delta] for the whole overlap.
        bool up = true;
        for (int i = 0; i + delta < rows; ++i) {
            if (m_staged[i] != m_prev[i + delta]) {
                up = false;
                break;
            }
        }
        if (up) {
            ++hits;
            foundDelta = delta;
            if (hits >= 2)
                break;
        }

        // Scroll down by `delta`: current[i] == prev[i-delta] for the overlap.
        bool down = true;
        for (int i = delta; i < rows; ++i) {
            if (m_staged[i] != m_prev[i - delta]) {
                down = false;
                break;
            }
        }
        if (down) {
            ++hits;
            foundDelta = -delta;
            if (hits >= 2)
                break;
        }
    }

    if (hits == 1) {
        const int n = foundDelta; // >0 up, <0 down
        // Mark only the freshly-exposed band dirty; the shifted region is
        // reused by the renderer (texture blit / row-cache shift).
        std::ranges::fill(m_dirty, std::uint8_t { 0 });
        if (n > 0) {
            for (int r = rows - n; r < rows; ++r)
                m_dirty[r] = 1;
        } else {
            for (int r = 0; r < -n; ++r)
                m_dirty[r] = 1;
        }
        m_plan.kind = FrameKind::Scrolled;
        m_plan.scrollDelta = n;
        m_plan.dirtyFirst = (n > 0) ? rows - n : 0;
        m_plan.dirtyLast = (n > 0) ? rows - 1 : -n - 1;
        m_plan.dirtyRows = m_dirty.data();
        return m_plan;
    }

    m_plan.kind = FrameKind::Dirty;
    m_plan.dirtyFirst = first;
    m_plan.dirtyLast = last;
    m_plan.dirtyRows = m_dirty.data();
    return m_plan;
}

void FrameDamageTracker::commit()
{
    m_prev = m_staged;
    m_prevEpoch = m_stagedEpoch;
    m_havePrev = true;
    if (m_stagedWasFull)
        m_committedFramesSinceFull = 0;
    else
        ++m_committedFramesSinceFull;
}

} // namespace Ubiquitty::Rendering
