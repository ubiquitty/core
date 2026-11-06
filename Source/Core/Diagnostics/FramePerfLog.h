// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The frame-timing log UBIQUITTY_PERFLOG turns on: ingest, render and pacing,
// aggregated per half-second window and printed as one set of lines per window.
//
// Every frontend feeds the same aggregator, so a `[perf]` line reads the same
// whichever shell printed it and the rule for what counts as a dropped frame is
// written once. Off unless the environment asks, and a log that is off is never
// fed: the frontend asks `isActive()` before it measures anything.
//
// Columns of the `[perf]` line:
//   in MB/s     — bytes ingested per second this window
//   KB/Ndr      — bytes / ingest deliveries
//   ing avg|max — per-delivery time in the read+parse loop (ms)
//   Nfr / fps   — frames presented this window
//   draw        — per-frame split: total, snapshot, drawable wait, encode, commit, GPU (ms)
//   gap         — largest interval between consecutive frames (ms)
//   drop        — dropped frames, counted only while host output drives the screen
//   inst        — avg|max instance counts; cap = largest buffer capacity seen

#include "Base/SwiftInterop.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Diagnostics {

/// One ingest delivery's cost, or several a frontend coalesced into one hop.
struct UBIQUITTY_ESCAPABLE PerfDrainSample {
    /// Bytes parsed.
    std::uint64_t bytes = 0;
    /// Deliveries the sample covers.
    std::uint32_t deliveries = 1;
    /// Time in the read+parse loop, summed over the deliveries.
    double ingestSeconds = 0.0;
    /// The slowest single delivery's time.
    double longestIngestSeconds = 0.0;
    /// The grid the bytes landed on.
    int columns = 0;
    int rows = 0;
};

/// Where a frame's box-drawing cells went, by the builder's fill routes.
struct UBIQUITTY_ESCAPABLE PerfBoxFillCensus {
    std::uint64_t sourceCells = 0;
    std::uint64_t fullRects = 0;
    std::uint64_t partialRects = 0;
    std::uint64_t shadeQuads = 0;
    std::uint64_t shapeQuads = 0;
    std::uint64_t triangleRects = 0;
    std::uint64_t arcRects = 0;
    std::uint64_t mosaicQuads = 0;
};

/// One presented frame's cost.
struct UBIQUITTY_ESCAPABLE PerfFrameSample {
    /// The frame from the snapshot's release to the commit.
    double drawSeconds = 0.0;
    /// Taking the model lock and copying the grid, before the draw.
    double snapshotSeconds = 0.0;
    /// Waiting for a drawable.
    double drawableWaitSeconds = 0.0;
    /// Encoding the passes.
    double encodeSeconds = 0.0;
    /// Committing and presenting.
    double commitSeconds = 0.0;
    /// GPU time completed since the last frame: summed, counted, and longest.
    double gpuSumSeconds = 0.0;
    std::uint64_t gpuSamples = 0;
    double gpuLongestSeconds = 0.0;
    /// Instances the frame built, and the buffer capacities behind them.
    std::uint64_t cells = 0;
    std::uint64_t overlays = 0;
    std::uint64_t decorations = 0;
    std::uint64_t decorationCapacity = 0;
    std::uint64_t boxFills = 0;
    std::uint64_t boxFillCapacity = 0;
    PerfBoxFillCensus boxFillCensus;
    /// The panel's refresh period the gaps are measured against; zero keeps
    /// the last one given.
    double refreshSeconds = 0.0;
};

/// What one flushed window measured.
struct UBIQUITTY_ESCAPABLE PerfWindowReport {
    /// The window's length.
    double seconds = 0.0;
    /// Bytes ingested and the deliveries that carried them.
    std::uint64_t bytes = 0;
    std::uint32_t drains = 0;
    /// Frames presented.
    std::uint32_t frames = 0;
    /// The largest interval between consecutive frames that counted.
    double longestGapSeconds = 0.0;
    /// Refreshes missed while host output drove the screen.
    std::int64_t droppedFrames = 0;
    /// The slowest frame's draw time.
    double longestDrawSeconds = 0.0;
};

/// The UBIQUITTY_PERFLOG aggregator.
///
/// Fed from one thread — the frontend's main thread — with that thread's clock:
/// every entry takes `now` in seconds, so the windows and the gaps are measured
/// on the clock the frames were stamped with.
class FramePerfLog {
public:
    /// How long one window aggregates before it prints.
    static constexpr double windowSeconds = 0.5;

    /// How recently host bytes must have arrived for a gap to count as a
    /// drop. A static screen presents nothing, so a gap while the terminal
    /// waits for a keypress was never a frame owed.
    static constexpr double activelyDrivenSeconds = 0.10;

    /// A gap longer than this many refresh periods missed at least one.
    static constexpr double droppedGapInRefreshes = 1.5;

    /// Whether the log is on: UBIQUITTY_PERFLOG in the environment, read on
    /// the first ask, or an earlier ``activate()``.
    [[nodiscard]] bool isActive();

    /// Turns the log on whatever the environment says.
    void activate();

    /// Turns the log off whatever the environment says, for a caller that
    /// turned it on for a measurement of its own.
    void deactivate();

    /// Keeps the log's lines off stderr; ``lastLines()`` still holds them.
    void silence();

    /// Records ingest the frontend was handed.
    ///
    /// A sample carrying bytes stamps the drop gate: frames in the next
    /// ``activelyDrivenSeconds`` are driven by host output.
    ///
    /// - Parameters:
    ///   - sample: The deliveries' bytes and parse time.
    ///   - now: When they reached the frontend.
    void noteDrain(const PerfDrainSample& sample, double now);

    /// A handler slower than this blocks the frame clock for a whole frame,
    /// and prints a `[perf-drain] SPIKE` line when it happens.
    static constexpr double drainHandlerSpikeSeconds = 0.008;

    /// Records how long the frontend's own handling of a delivery took.
    ///
    /// A spike under a flood means the main thread sat in the handler between
    /// frames, starving the frame clock.
    ///
    /// - Parameters:
    ///   - seconds: The handler's duration.
    ///   - bytes: The bytes the delivery carried, for the spike line.
    void noteDrainHandler(double seconds, std::uint64_t bytes);

    /// Records the frame clock's idle-to-active edge.
    ///
    /// The next frame's gap spans a pause the clock asked for, not a stall, so
    /// it counts toward neither the longest gap nor the drops. Without this the
    /// first frame of every burst books the whole idle pause as drops: the
    /// burst's first delivery stamps the drop gate before its frame lands.
    ///
    /// - Parameter now: When the clock resumed.
    void noteResume(double now);

    /// Records one presented frame.
    ///
    /// - Parameters:
    ///   - sample: The frame's costs and instance counts.
    ///   - now: When the frame finished.
    void noteFrame(const PerfFrameSample& sample, double now);

    /// Records the streamed-frame pacing counters, as absolute totals; the
    /// window prints their growth on a `[perf-pace]` line.
    ///
    /// - Parameters:
    ///   - promoted: Frames that reached the screen.
    ///   - overwritten: Frames replaced before they were promoted — the judder.
    ///   - discarded: Frames dropped on a foreground edge.
    ///   - slippedVsyncs: Refreshes presents latched late by.
    void notePacing(std::uint64_t promoted, std::uint64_t overwritten, std::uint64_t discarded, std::uint64_t slippedVsyncs);

    /// Records which renderer arm drew, as absolute totals; the window prints
    /// their growth on a `[perf-arm]` line.
    ///
    /// - Parameters:
    ///   - drawn: Frames the Swift arm drew.
    ///   - declinedCaptures: Frames it declined at the capture.
    ///   - declinedEncodes: Frames it declined at the encode.
    ///   - walksBuilt: Frames the walk built.
    ///   - reusedGrids: Frames that kept the retained grid.
    void noteArm(std::uint64_t drawn, std::uint64_t declinedCaptures, std::uint64_t declinedEncodes, std::uint64_t walksBuilt,
        std::uint64_t reusedGrids);

    /// The last flushed window's figures.
    [[nodiscard]] PerfWindowReport lastReport() const { return m_lastReport; }

    /// How many windows have flushed.
    [[nodiscard]] std::uint64_t reportCount() const { return m_reportCount; }

    /// The window being aggregated, as it stands.
    [[nodiscard]] PerfWindowReport currentWindow() const;

    /// The lines the last flush printed.
    [[nodiscard]] const std::vector<std::string>& lastLines() const { return m_lastLines; }

    /// When the frame clock last resumed, as ``noteResume(_:)`` was told.
    [[nodiscard]] double lastResumeSeconds() const { return m_lastResume; }

private:
    /// Opens a window at the first sample after the log turned on.
    ///
    /// - Parameter now: The sample's time.
    void startWindowIfIdle(double now);

    /// Prints and closes the window once it has run its length.
    ///
    /// - Parameter now: The latest sample's time.
    void flushIfDue(double now);

    /// Prints one line to stderr unless silenced, and keeps it for
    /// ``lastLines()``.
    ///
    /// - Parameter line: The line, without its newline.
    void emit(std::string line);

    /// Empties the window's aggregates and starts the next one.
    ///
    /// - Parameter now: When the next window starts.
    void clearWindow(double now);

    bool m_hasReadEnvironment = false;
    bool m_isActive = false;
    bool m_isSilenced = false;
    bool m_hasWindow = false;

    double m_windowStart = 0.0;
    double m_lastFrame = 0.0;
    double m_lastDrain = 0.0;
    double m_lastResume = 0.0;
    double m_refreshSeconds = 0.0;

    std::uint64_t m_bytes = 0;
    std::uint32_t m_drains = 0;
    double m_ingestSum = 0.0;
    double m_ingestLongest = 0.0;
    double m_drainHandlerSum = 0.0;
    double m_drainHandlerLongest = 0.0;

    std::uint32_t m_frames = 0;
    double m_drawSum = 0.0;
    double m_drawLongest = 0.0;
    double m_snapshotSum = 0.0;
    double m_snapshotLongest = 0.0;
    double m_waitSum = 0.0;
    double m_waitLongest = 0.0;
    double m_encodeSum = 0.0;
    double m_encodeLongest = 0.0;
    double m_commitSum = 0.0;
    double m_commitLongest = 0.0;
    double m_gpuSum = 0.0;
    double m_gpuLongest = 0.0;
    std::uint64_t m_gpuSamples = 0;
    double m_gapLongest = 0.0;
    std::int64_t m_dropped = 0;

    std::uint64_t m_cellSum = 0;
    std::uint64_t m_cellMost = 0;
    std::uint64_t m_overlaySum = 0;
    std::uint64_t m_overlayMost = 0;
    std::uint64_t m_decorationSum = 0;
    std::uint64_t m_decorationMost = 0;
    std::uint64_t m_decorationCapacityMost = 0;
    std::uint64_t m_boxFillSum = 0;
    std::uint64_t m_boxFillMost = 0;
    std::uint64_t m_boxFillCapacityMost = 0;
    PerfBoxFillCensus m_boxFillCensusSum;
    int m_columns = 0;
    int m_rows = 0;

    std::array<std::uint64_t, 4> m_pacingLatest {};
    std::array<std::uint64_t, 4> m_pacingBase {};
    std::array<std::uint64_t, 5> m_armLatest {};
    std::array<std::uint64_t, 5> m_armBase {};

    PerfWindowReport m_lastReport;
    std::uint64_t m_reportCount = 0;
    std::vector<std::string> m_lastLines;
};

} // namespace Ubiquitty::Diagnostics
