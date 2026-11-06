// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/PredictionExposure.h"

#include <algorithm>

namespace Ubiquitty::Session {

std::expected<void, PredictionExposureError> PredictionExposure::configure(std::optional<PredictionExposurePolicy> policy)
{
    reset();
    m_policy.reset();
    if (!policy)
        return {};
    if (policy->hideAtMicroseconds > policy->showAboveMicroseconds
        || policy->clearFlagAtMicroseconds > policy->flagAboveMicroseconds
        || policy->flagAboveMicroseconds < policy->showAboveMicroseconds
        || policy->clearFlagAtMicroseconds < policy->hideAtMicroseconds
        || !policy->glitchAfterMicroseconds
        || policy->forceFlagAfterMicroseconds < policy->glitchAfterMicroseconds)
        return std::unexpected(PredictionExposureError::InvalidPolicy);
    m_policy = policy;
    return {};
}

void PredictionExposure::noteEchoLatency(std::uint64_t microseconds)
{
    if (!m_policy)
        return;
    if (microseconds > m_policy->showAboveMicroseconds)
        m_isLatencyVisible = true;
    else if (microseconds <= m_policy->hideAtMicroseconds)
        m_isLatencyVisible = false;
    if (microseconds > m_policy->flagAboveMicroseconds)
        m_isLatencyFlagged = true;
    else if (microseconds <= m_policy->clearFlagAtMicroseconds)
        m_isLatencyFlagged = false;
}

void PredictionExposure::reset()
{
    m_isLatencyVisible = false;
    m_isLatencyFlagged = false;
}

PredictionTreatment PredictionExposure::treatment(std::uint64_t now, std::optional<std::uint64_t> pendingSince) const
{
    if (!m_policy || !pendingSince || now < *pendingSince)
        return {};
    const auto age = now - *pendingSince;
    const bool isVisible = m_isLatencyVisible || age >= m_policy->glitchAfterMicroseconds;
    return { .isVisible = isVisible,
        .isFlagged = isVisible && (m_isLatencyFlagged || age >= m_policy->forceFlagAfterMicroseconds) };
}

std::optional<std::uint64_t> PredictionExposure::nextWakeDelay(std::uint64_t now, std::optional<std::uint64_t> pendingSince) const
{
    if (!m_policy || !pendingSince || now < *pendingSince)
        return std::nullopt;
    const auto age = now - *pendingSince;
    std::optional<std::uint64_t> delay;
    if (!m_isLatencyVisible && age < m_policy->glitchAfterMicroseconds)
        delay = m_policy->glitchAfterMicroseconds - age;
    if (!m_isLatencyFlagged && age < m_policy->forceFlagAfterMicroseconds) {
        const auto flagDelay = m_policy->forceFlagAfterMicroseconds - age;
        delay = delay ? std::min(*delay, flagDelay) : flagDelay;
    }
    return delay;
}

} // namespace Ubiquitty::Session
