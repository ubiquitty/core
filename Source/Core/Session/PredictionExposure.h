// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <expected>
#include <optional>

namespace Ubiquitty::Session {

/// Measured presentation thresholds, supplied by the connection owner.
/// No absent policy or unconfirmed epoch can expose speculative text.
struct PredictionExposurePolicy {
    std::uint64_t showAboveMicroseconds = 0;
    std::uint64_t hideAtMicroseconds = 0;
    std::uint64_t flagAboveMicroseconds = 0;
    std::uint64_t clearFlagAtMicroseconds = 0;
    std::uint64_t glitchAfterMicroseconds = 0;
    std::uint64_t forceFlagAfterMicroseconds = 0;
};

enum class PredictionExposureError : std::uint8_t {
    InvalidPolicy,
};

/// The presentation of an already-confirmed epoch's pending cells.
struct PredictionTreatment {
    bool isVisible = false;
    bool isFlagged = false;
};

/// Keeps latency hysteresis separate from confidence in a predicted edit.
/// This object reads an injected clock and neither schedules work nor retains text.
class PredictionExposure {
public:
    /// Installs measured thresholds or disables all speculative display.
    /// Invalid thresholds disable display and refuse the configuration.
    [[nodiscard]] std::expected<void, PredictionExposureError> configure(std::optional<PredictionExposurePolicy>);

    /// Records the send-to-first-settled-frame latency, not a transport receipt.
    void noteEchoLatency(std::uint64_t microseconds);

    /// Forgets the old path's measurements without changing the configured policy.
    void reset();

    /// Describes pending text; an absent timestamp also represents a hidden epoch.
    [[nodiscard]] PredictionTreatment treatment(std::uint64_t now, std::optional<std::uint64_t> pendingSince) const;

    /// Returns a relative owner wake delay, absent for idle or hidden epochs.
    /// Relative delays avoid overflowing an absolute deadline near clock exhaustion.
    [[nodiscard]] std::optional<std::uint64_t> nextWakeDelay(std::uint64_t now, std::optional<std::uint64_t> pendingSince) const;

private:
    std::optional<PredictionExposurePolicy> m_policy;
    bool m_isLatencyVisible = false;
    bool m_isLatencyFlagged = false;
};

} // namespace Ubiquitty::Session
