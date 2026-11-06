// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/HandoverQuiesce.h"

#include "Core/Protocol/Sync/HandoverGround.h"
#include "Core/Runtime/FrameCoherence.h"

namespace Ubiquitty::Runtime {

namespace {

using Protocol::Sync::HandoverBlocker;
using Protocol::Sync::HandoverBlockers;
using Protocol::Sync::terminalHandoverBlockers;

constexpr HandoverBlockers streamBlockers {
    HandoverBlocker::ParserMidSequence,
    HandoverBlocker::IncompleteCharacter,
    HandoverBlocker::LegacySequence,
    HandoverBlocker::DeviceControlString,
    HandoverBlocker::ClipboardChunkOpen,
    HandoverBlocker::ExtendedClipboardOpen,
    HandoverBlocker::ImageTransferOpen,
    HandoverBlocker::NotificationOpen,
    HandoverBlocker::SynchronizedOutput,
};

} // namespace

HandoverQuiesce::HandoverQuiesce(QuiesceTuning tuning, std::uint64_t nowMicroseconds)
    : m_tuning(tuning)
    , m_startedMicroseconds(nowMicroseconds)
    , m_lastMicroseconds(nowMicroseconds)
{
}

void HandoverQuiesce::fail(QuiesceFailure failure)
{
    if (m_phase == QuiescePhase::Failed || m_phase == QuiescePhase::Ready)
        return;
    m_phase = QuiescePhase::Failed;
    m_failure = failure;
    m_hasReadLease = false;
}

bool HandoverQuiesce::advanceClock(std::uint64_t nowMicroseconds)
{
    if (m_phase == QuiescePhase::Failed || m_phase == QuiescePhase::Ready)
        return false;
    if (nowMicroseconds < m_lastMicroseconds) {
        fail(QuiesceFailure::ClockRegressed);
        return false;
    }
    m_lastMicroseconds = nowMicroseconds;
    const auto elapsed = nowMicroseconds - m_startedMicroseconds;
    if (elapsed < m_tuning.naturalMicroseconds)
        return true;
    m_phase = QuiescePhase::ReadsFrozen;
    if (elapsed - m_tuning.naturalMicroseconds >= m_tuning.frozenMicroseconds) {
        fail(QuiesceFailure::Deadline);
        return false;
    }
    return true;
}

void HandoverQuiesce::notePass(const Terminal& terminal, std::optional<FrameClass> published, std::uint64_t nowMicroseconds)
{
    if (!advanceClock(nowMicroseconds))
        return;
    if (m_hasReadLease) {
        fail(QuiesceFailure::ReadContract);
        return;
    }
    if (published != FrameClass::Boundary && published != FrameClass::Settled)
        return;
    if (terminalHandoverBlockers(terminal).isEmpty())
        m_phase = QuiescePhase::Ready;
}

std::size_t HandoverQuiesce::takeGroundReadAllowance(const Terminal& terminal, std::uint64_t nowMicroseconds)
{
    if (!advanceClock(nowMicroseconds))
        return 0;
    if (m_hasReadLease) {
        fail(QuiesceFailure::ReadContract);
        return 0;
    }
    if (m_phase != QuiescePhase::ReadsFrozen)
        return 0;
    const auto blockers = terminalHandoverBlockers(terminal);
    if (blockers.containsAny({ HandoverBlocker::ExternalSinkOpen, HandoverBlocker::IntegrationActive }) || !blockers.containsAny(streamBlockers))
        return 0;
    if (m_groundBytesRead == m_tuning.maxGroundBytes) {
        fail(QuiesceFailure::ByteBudget);
        return 0;
    }
    m_hasReadLease = true;
    return 1;
}

void HandoverQuiesce::completeGroundRead(std::size_t bytesRead, std::uint64_t nowMicroseconds)
{
    if (m_phase == QuiescePhase::Failed || m_phase == QuiescePhase::Ready)
        return;
    if (!m_hasReadLease || bytesRead > 1) {
        fail(QuiesceFailure::ReadContract);
        return;
    }
    m_hasReadLease = false;
    m_groundBytesRead += bytesRead;
    (void)advanceClock(nowMicroseconds);
}

} // namespace Ubiquitty::Runtime
