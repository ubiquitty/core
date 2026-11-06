// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The felt-latency present leg: model to glass, measured from the drawable's
// presented handler and reported as p50/p99 every so many frames.
//
// The handler runs on the compositor's present thread and the report on the
// frontend's main thread. A lock-free SPSC ring joins them: the handler posts,
// main pops. The ring is shared-owned, and what a handler holds is a `Poster`
// carrying its own reference, so a handler still in flight when the renderer
// is torn down keeps the ring alive and posts into it rather than through a
// freed member. Every frontend that presents to glass uses this; none keeps a
// ring of its own.

#include "Base/SwiftInterop.h"
#include "Core/Runtime/PresentTimeSink.h"
#include "Core/Runtime/RuntimeLink.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <string>

namespace Ubiquitty::Runtime {

/// Measures the present leg of the frames a renderer arms it for.
class PresentLegMeter {
public:
    /// The ring between the present thread and main.
    using Ring = CommandRing<double, 256>;

    /// How many drained frames a report covers.
    static constexpr std::uint64_t reportEveryFrames = 120;

    /// What the present thread writes into: the ring of legs, and the count of
    /// presents that reported no presented time.
    struct Channel {
        /// Model-to-glass milliseconds, one per present that reached the glass.
        Ring ring;
        /// Presents whose drawable never reached the glass.
        std::atomic<std::uint64_t> unshownPresents { 0 };
    };

    /// What a presented handler holds for one frame: the channel, by reference
    /// count, and the time the frame's model was taken.
    ///
    /// Copyable, so a handler captures it by value. A default-constructed one
    /// is unarmed and posts nothing.
    class Poster {
    public:
        /// Whether a frame was armed when this was taken.
        [[nodiscard]] bool isArmed() const { return m_channel != nullptr; }

        /// Posts the frame's present leg. Called on the present thread.
        ///
        /// A drawable that never reached the glass — replaced before the
        /// compositor latched it, or in a window no display shows — reports a
        /// presented time of zero and has no leg; it is counted apart, so the
        /// report is never a model time subtracted from nothing. A full ring
        /// drops the sample: the handler must not wait on main, and a report
        /// missing a sample under a stall is the stall showing.
        ///
        /// - Parameter presentedSeconds: The drawable's presented time, on the
        ///   clock the model time was taken on.
        void post(double presentedSeconds) const
        {
            if (!m_channel)
                return;
            if (presentedSeconds <= 0.0) {
                m_channel->unshownPresents.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            (void)m_channel->ring.post((presentedSeconds - m_modelSeconds) * 1000.0);
        }

    private:
        friend class PresentLegMeter;
        std::shared_ptr<Channel> m_channel;
        double m_modelSeconds = 0.0;
    };

    /// Arms the next present with the time its frame's model was taken.
    ///
    /// The channel and the sink are made on the first arm, so a renderer whose
    /// frames are never timed allocates neither.
    ///
    /// - Parameter modelSeconds: When the frame's model was captured.
    void arm(double modelSeconds)
    {
        if (!m_channel)
            m_channel = std::make_shared<Channel>();
        if (!m_sink)
            m_sink.emplace();
        m_armedModelSeconds = modelSeconds;
    }

    /// Takes the armed frame's poster, consuming the arm; the next frame is
    /// timed only if it is armed again.
    ///
    /// - Returns: The poster, unarmed when no frame was armed.
    [[nodiscard]] Poster takeArmedPoster()
    {
        Poster poster;
        if (m_armedModelSeconds != 0.0 && m_channel) {
            poster.m_channel = m_channel;
            poster.m_modelSeconds = m_armedModelSeconds;
        }
        m_armedModelSeconds = 0.0;
        return poster;
    }

    /// Collects what the present thread posted, once per drawn frame.
    ///
    /// Every ``reportEveryFrames``th call closes a report over the samples
    /// since the last one.
    ///
    /// - Returns: The closed report when this call closed one and it holds a
    ///   sample; a zero count otherwise.
    [[nodiscard]] PresentTimeSink::Quantiles drainFrame()
    {
        if (!m_channel || !m_sink)
            return {};
        double sample = 0.0;
        while (m_channel->ring.pop(sample)) {
            m_sink->recordPresentLegMilliseconds(sample);
            ++m_samplesRecorded;
        }
        if (++m_framesDrained % reportEveryFrames != 0)
            return {};
        const PresentTimeSink::Quantiles report = m_sink->drainPresentLeg();
        if (report.count > 0)
            m_lastReport = report;
        return report;
    }

    /// Samples collected from the present thread since this meter was made.
    [[nodiscard]] std::uint64_t samplesRecorded() const { return m_samplesRecorded; }

    /// Presents the handler reported with no presented time since this meter
    /// was made.
    [[nodiscard]] std::uint64_t unshownPresents() const
    {
        return m_channel ? m_channel->unshownPresents.load(std::memory_order_relaxed) : 0;
    }

    /// The last report that held a sample.
    [[nodiscard]] PresentTimeSink::Quantiles lastReport() const { return m_lastReport; }

    /// The log line a report prints as.
    ///
    /// - Parameter report: A report ``drainFrame()`` closed.
    /// - Returns: The `[perf] present_leg` line.
    [[nodiscard]] static std::string reportLine(const PresentTimeSink::Quantiles& report)
    {
        return std::format("[perf] present_leg p50={:.3f}ms p99={:.3f}ms n={}", report.p50, report.p99, report.count);
    }

    /// Prints a closed report's line to stderr, beside the perf log's.
    ///
    /// - Parameter report: A report ``drainFrame()`` closed.
    static void printReport(const PresentTimeSink::Quantiles& report)
    {
        std::string line = reportLine(report);
        line.push_back('\n');
        (void)std::fwrite(line.data(), 1, line.size(), stderr);
    }

private:
    std::shared_ptr<Channel> m_channel;
    std::optional<PresentTimeSink> m_sink;
    double m_armedModelSeconds = 0.0;
    std::uint64_t m_framesDrained = 0;
    std::uint64_t m_samplesRecorded = 0;
    PresentTimeSink::Quantiles m_lastReport;
};

} // namespace Ubiquitty::Runtime
