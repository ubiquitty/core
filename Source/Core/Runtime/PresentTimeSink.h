// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Portable accumulator for the felt-latency metric. Single consumer (main) — no
// internal locking. Two metrics, milliseconds, reported as p50/p99:
//   feltLatency = presentedTime - keystroke_ts   (keystroke -> glass; felt)
//   presentLeg  = presentedTime - model_updated  (the leg we own)

#include <algorithm>
#include <cstddef>
#include <vector>

namespace Ubiquitty::Runtime {

class PresentTimeSink {
public:
    struct Quantiles {
        double p50 = 0.0; // milliseconds
        double p99 = 0.0;
        std::size_t count = 0;
    };

    // Ring keeps the most recent `capacity` samples so a long session stays bounded.
    explicit PresentTimeSink(std::size_t capacity = 4096)
        : m_felt(capacity)
        , m_presentLeg(capacity)
    {
    }

    void recordFeltLatencyMilliseconds(double milliseconds) { m_felt.push(milliseconds); }
    void recordPresentLegMilliseconds(double milliseconds) { m_presentLeg.push(milliseconds); }

    [[nodiscard]] Quantiles feltLatency() const { return m_felt.quantiles(); }
    [[nodiscard]] Quantiles presentLeg() const { return m_presentLeg.quantiles(); }

    Quantiles drainPresentLeg()
    {
        Quantiles quantiles = m_presentLeg.quantiles();
        m_presentLeg.clear();
        return quantiles;
    }

    void reset()
    {
        m_felt.clear();
        m_presentLeg.clear();
    }

private:
    // Fixed-capacity ring of samples; overwrites the oldest once full.
    class Ring {
    public:
        explicit Ring(std::size_t capacity)
            : m_data(capacity > 0 ? capacity : 1)
        {
        }

        void push(double value)
        {
            m_data[m_head] = value;
            m_head = (m_head + 1) % m_data.size();
            if (m_size < m_data.size())
                ++m_size;
        }

        void clear()
        {
            m_head = 0;
            m_size = 0;
        }

        [[nodiscard]] Quantiles quantiles() const
        {
            Quantiles q;
            q.count = m_size;
            if (m_size == 0)
                return q;
            std::vector<double> sorted(m_data.begin(), m_data.begin() + static_cast<std::ptrdiff_t>(m_size));
            std::sort(sorted.begin(), sorted.end());
            q.p50 = nearestRank(sorted, 0.50);
            q.p99 = nearestRank(sorted, 0.99);
            return q;
        }

    private:
        // Nearest-rank quantile.
        static double nearestRank(const std::vector<double>& sorted, double quantile)
        {
            const std::size_t index = std::min(sorted.size() - 1, static_cast<std::size_t>(static_cast<double>(sorted.size()) * quantile));
            return sorted[index];
        }

        std::vector<double> m_data;
        std::size_t m_head = 0;
        std::size_t m_size = 0;
    };

    Ring m_felt;
    Ring m_presentLeg;
};

} // namespace Ubiquitty::Runtime
