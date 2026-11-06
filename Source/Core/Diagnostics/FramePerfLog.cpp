// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The UBIQUITTY_PERFLOG aggregator. See FramePerfLog.h.

#include "Core/Diagnostics/FramePerfLog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <utility>

namespace Ubiquitty::Diagnostics {

namespace {

enum PacingSlot {
    Promoted,
    Overwritten,
    Discarded,
    Slipped
};
enum ArmSlot {
    Drawn,
    DeclinedCaptures,
    DeclinedEncodes,
    WalksBuilt,
    ReusedGrids
};

/// A per-window mean in milliseconds, zero over an empty window.
double meanMilliseconds(double sumSeconds, std::uint64_t count)
{
    return count ? sumSeconds / static_cast<double>(count) * 1000.0 : 0.0;
}

/// A per-frame mean of an instance count, zero over an empty window.
std::uint64_t meanCount(std::uint64_t sum, std::uint32_t frames)
{
    return frames ? sum / frames : 0;
}

} // namespace

bool FramePerfLog::isActive()
{
    if (!m_hasReadEnvironment) {
        m_hasReadEnvironment = true;
        m_isActive = m_isActive || std::getenv("UBIQUITTY_PERFLOG") != nullptr;
    }
    return m_isActive;
}

void FramePerfLog::activate()
{
    m_isActive = true;
}

void FramePerfLog::deactivate()
{
    m_hasReadEnvironment = true;
    m_isActive = false;
}

void FramePerfLog::silence()
{
    m_isSilenced = true;
}

void FramePerfLog::startWindowIfIdle(double now)
{
    if (m_hasWindow)
        return;
    m_hasWindow = true;
    m_windowStart = now;
}

void FramePerfLog::noteDrain(const PerfDrainSample& sample, double now)
{
    startWindowIfIdle(now);
    m_bytes += sample.bytes;
    m_drains += sample.deliveries;
    m_ingestSum += sample.ingestSeconds;
    m_ingestLongest = std::max(m_ingestLongest, sample.longestIngestSeconds);
    if (sample.bytes > 0)
        m_lastDrain = now;
    m_columns = sample.columns;
    m_rows = sample.rows;
    flushIfDue(now);
}

void FramePerfLog::noteDrainHandler(double seconds, std::uint64_t bytes)
{
    m_drainHandlerSum += seconds;
    m_drainHandlerLongest = std::max(m_drainHandlerLongest, seconds);
    if (seconds > drainHandlerSpikeSeconds)
        emit(std::format("[perf-drain] SPIKE {:.1f}ms bytes={}", seconds * 1000.0, bytes));
}

void FramePerfLog::noteResume(double now)
{
    m_lastResume = now;
}

void FramePerfLog::noteFrame(const PerfFrameSample& sample, double now)
{
    startWindowIfIdle(now);
    if (sample.refreshSeconds > 0.0)
        m_refreshSeconds = sample.refreshSeconds;
    ++m_frames;
    m_drawSum += sample.drawSeconds;
    m_drawLongest = std::max(m_drawLongest, sample.drawSeconds);
    m_snapshotSum += sample.snapshotSeconds;
    m_snapshotLongest = std::max(m_snapshotLongest, sample.snapshotSeconds);
    m_waitSum += sample.drawableWaitSeconds;
    m_waitLongest = std::max(m_waitLongest, sample.drawableWaitSeconds);
    m_encodeSum += sample.encodeSeconds;
    m_encodeLongest = std::max(m_encodeLongest, sample.encodeSeconds);
    m_commitSum += sample.commitSeconds;
    m_commitLongest = std::max(m_commitLongest, sample.commitSeconds);
    m_gpuSum += sample.gpuSumSeconds;
    m_gpuSamples += sample.gpuSamples;
    m_gpuLongest = std::max(m_gpuLongest, sample.gpuLongestSeconds);
    m_cellSum += sample.cells;
    m_cellMost = std::max(m_cellMost, sample.cells);
    m_overlaySum += sample.overlays;
    m_overlayMost = std::max(m_overlayMost, sample.overlays);
    m_decorationSum += sample.decorations;
    m_decorationMost = std::max(m_decorationMost, sample.decorations);
    m_decorationCapacityMost = std::max(m_decorationCapacityMost, sample.decorationCapacity);
    m_boxFillSum += sample.boxFills;
    m_boxFillMost = std::max(m_boxFillMost, sample.boxFills);
    m_boxFillCapacityMost = std::max(m_boxFillCapacityMost, sample.boxFillCapacity);
    m_boxFillCensusSum.sourceCells += sample.boxFillCensus.sourceCells;
    m_boxFillCensusSum.fullRects += sample.boxFillCensus.fullRects;
    m_boxFillCensusSum.partialRects += sample.boxFillCensus.partialRects;
    m_boxFillCensusSum.shadeQuads += sample.boxFillCensus.shadeQuads;
    m_boxFillCensusSum.shapeQuads += sample.boxFillCensus.shapeQuads;
    m_boxFillCensusSum.triangleRects += sample.boxFillCensus.triangleRects;
    m_boxFillCensusSum.arcRects += sample.boxFillCensus.arcRects;
    m_boxFillCensusSum.mosaicQuads += sample.boxFillCensus.mosaicQuads;
    // A frame whose predecessor predates the resume spans a pause the clock
    // asked for: neither its gap nor its drops are the renderer's.
    if (m_lastFrame > 0.0 && m_lastFrame >= m_lastResume) {
        const double gap = now - m_lastFrame;
        m_gapLongest = std::max(m_gapLongest, gap);
        // A drop counts only while host bytes arrived within the gate: the
        // terminal presents on demand and pauses on a static screen, so an
        // idle gap was never a frame owed. Under a flood the drains are
        // continuous, and a gap past the threshold is a real stall.
        const bool isActivelyDriven = now - m_lastDrain < activelyDrivenSeconds;
        if (isActivelyDriven && m_refreshSeconds > 0.0 && gap > m_refreshSeconds * droppedGapInRefreshes)
            m_dropped += static_cast<std::int64_t>(gap / m_refreshSeconds + 0.5) - 1;
    }
    m_lastFrame = now;
    flushIfDue(now);
}

void FramePerfLog::notePacing(std::uint64_t promoted, std::uint64_t overwritten, std::uint64_t discarded, std::uint64_t slippedVsyncs)
{
    m_pacingLatest[Promoted] = promoted;
    m_pacingLatest[Overwritten] = overwritten;
    m_pacingLatest[Discarded] = discarded;
    m_pacingLatest[Slipped] = slippedVsyncs;
}

void FramePerfLog::noteArm(std::uint64_t drawn, std::uint64_t declinedCaptures, std::uint64_t declinedEncodes,
    std::uint64_t walksBuilt, std::uint64_t reusedGrids)
{
    m_armLatest[Drawn] = drawn;
    m_armLatest[DeclinedCaptures] = declinedCaptures;
    m_armLatest[DeclinedEncodes] = declinedEncodes;
    m_armLatest[WalksBuilt] = walksBuilt;
    m_armLatest[ReusedGrids] = reusedGrids;
}

PerfWindowReport FramePerfLog::currentWindow() const
{
    PerfWindowReport report;
    report.bytes = m_bytes;
    report.drains = m_drains;
    report.frames = m_frames;
    report.longestGapSeconds = m_gapLongest;
    report.droppedFrames = m_dropped;
    report.longestDrawSeconds = m_drawLongest;
    return report;
}

void FramePerfLog::emit(std::string line)
{
    if (!m_isSilenced) {
        std::string written = line;
        written.push_back('\n');
        (void)std::fwrite(written.data(), 1, written.size(), stderr);
    }
    m_lastLines.push_back(std::move(line));
}

void FramePerfLog::flushIfDue(double now)
{
    const double seconds = now - m_windowStart;
    if (seconds < windowSeconds)
        return;
    m_lastLines.clear();
    emit(std::format("[perf-drain] didDrain main avg={:.2f}ms max={:.2f}ms over {} deliveries",
        meanMilliseconds(m_drainHandlerSum, m_drains), m_drainHandlerLongest * 1000.0, m_drains));
    if (m_pacingLatest != m_pacingBase) {
        emit(std::format("[perf-pace] content promote={} overwrite={} discard={} slip={}",
            m_pacingLatest[Promoted] - m_pacingBase[Promoted], m_pacingLatest[Overwritten] - m_pacingBase[Overwritten],
            m_pacingLatest[Discarded] - m_pacingBase[Discarded], m_pacingLatest[Slipped] - m_pacingBase[Slipped]));
        m_pacingBase = m_pacingLatest;
    }
    if (m_armLatest[Drawn] != m_armBase[Drawn] || m_armLatest[DeclinedCaptures] != m_armBase[DeclinedCaptures]
        || m_armLatest[DeclinedEncodes] != m_armBase[DeclinedEncodes]) {
        const std::uint64_t drawn = m_armLatest[Drawn] - m_armBase[Drawn];
        const std::uint64_t declinedCaptures = m_armLatest[DeclinedCaptures] - m_armBase[DeclinedCaptures];
        const std::uint64_t declinedEncodes = m_armLatest[DeclinedEncodes] - m_armBase[DeclinedEncodes];
        const std::uint64_t walksBuilt = m_armLatest[WalksBuilt] - m_armBase[WalksBuilt];
        // `built twice` is the number to read: the walk runs before the arm is
        // consulted, so a frame in both counts paid for two builds and threw
        // one away. `reused` is the throughput number: a frame that could not
        // keep the retained grid rebuilds the whole screen.
        const std::uint64_t reused = m_armLatest[ReusedGrids] - m_armBase[ReusedGrids];
        emit(std::format("[perf-arm] swift drew={} reused={} fell back={} (capture {} + encode {}) | walk built={} | built twice={}",
            drawn, reused, declinedCaptures + declinedEncodes, declinedCaptures, declinedEncodes, walksBuilt,
            std::min(drawn, walksBuilt)));
        m_armBase = m_armLatest;
    }
    emit(std::format("[perf] {:.2f}s | in {:6.1f} MB/s {:7}KB/{:<3}dr  ing(ms) avg={:5.2f} max={:6.2f} | {:3}fr {:5.0f}fps "
                     "draw(ms) total={:5.2f}/{:6.2f} snap={:5.2f}/{:6.2f} wait={:5.2f}/{:6.2f} enc={:5.2f}/{:6.2f} "
                     "commit={:5.2f}/{:6.2f} gpu={:5.2f}/{:6.2f}/{} gap={:6.1f} drop={} | {}x{} | inst avg/max cell={}/{} "
                     "deco={}/{} cap={} box={}/{} cap={} ov={}/{} boxk avg src={} full={} part={} shade={} shape={} tri={} "
                     "arc={} mosaic={}",
        seconds, static_cast<double>(m_bytes) / 1e6 / seconds, m_bytes / 1024, m_drains, meanMilliseconds(m_ingestSum, m_drains),
        m_ingestLongest * 1000.0, m_frames, m_frames / seconds, meanMilliseconds(m_drawSum, m_frames), m_drawLongest * 1000.0,
        meanMilliseconds(m_snapshotSum, m_frames), m_snapshotLongest * 1000.0, meanMilliseconds(m_waitSum, m_frames),
        m_waitLongest * 1000.0, meanMilliseconds(m_encodeSum, m_frames), m_encodeLongest * 1000.0,
        meanMilliseconds(m_commitSum, m_frames), m_commitLongest * 1000.0, meanMilliseconds(m_gpuSum, m_gpuSamples),
        m_gpuLongest * 1000.0, m_gpuSamples, m_gapLongest * 1000.0, m_dropped, m_columns, m_rows, meanCount(m_cellSum, m_frames),
        m_cellMost, meanCount(m_decorationSum, m_frames), m_decorationMost, m_decorationCapacityMost,
        meanCount(m_boxFillSum, m_frames), m_boxFillMost, m_boxFillCapacityMost, meanCount(m_overlaySum, m_frames), m_overlayMost,
        meanCount(m_boxFillCensusSum.sourceCells, m_frames), meanCount(m_boxFillCensusSum.fullRects, m_frames),
        meanCount(m_boxFillCensusSum.partialRects, m_frames), meanCount(m_boxFillCensusSum.shadeQuads, m_frames),
        meanCount(m_boxFillCensusSum.shapeQuads, m_frames), meanCount(m_boxFillCensusSum.triangleRects, m_frames),
        meanCount(m_boxFillCensusSum.arcRects, m_frames), meanCount(m_boxFillCensusSum.mosaicQuads, m_frames)));
    m_lastReport = currentWindow();
    m_lastReport.seconds = seconds;
    ++m_reportCount;
    clearWindow(now);
}

void FramePerfLog::clearWindow(double now)
{
    m_windowStart = now;
    m_bytes = 0;
    m_drains = 0;
    m_ingestSum = 0.0;
    m_ingestLongest = 0.0;
    m_drainHandlerSum = 0.0;
    m_drainHandlerLongest = 0.0;
    m_frames = 0;
    m_drawSum = 0.0;
    m_drawLongest = 0.0;
    m_snapshotSum = 0.0;
    m_snapshotLongest = 0.0;
    m_waitSum = 0.0;
    m_waitLongest = 0.0;
    m_encodeSum = 0.0;
    m_encodeLongest = 0.0;
    m_commitSum = 0.0;
    m_commitLongest = 0.0;
    m_gpuSum = 0.0;
    m_gpuLongest = 0.0;
    m_gpuSamples = 0;
    m_gapLongest = 0.0;
    m_dropped = 0;
    m_cellSum = 0;
    m_cellMost = 0;
    m_overlaySum = 0;
    m_overlayMost = 0;
    m_decorationSum = 0;
    m_decorationMost = 0;
    m_decorationCapacityMost = 0;
    m_boxFillSum = 0;
    m_boxFillMost = 0;
    m_boxFillCapacityMost = 0;
    m_boxFillCensusSum = PerfBoxFillCensus {};
}

} // namespace Ubiquitty::Diagnostics
