// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Compiler.h"
#include "Base/SwiftInterop.h"
#include "Base/Types.h"
#include "Core/Title/Format.h"

#include <string_view>

namespace Ubiquitty::Session {

/// The requested behavior after a locally owned process ends.
enum class UBIQUITTY_CLOSED_ENUM ProcessExitBehavior {
    KeepOutput UBIQUITTY_SWIFT_NAME(keepOutput),
    CloseOnSuccess UBIQUITTY_SWIFT_NAME(closeOnSuccess),
    CloseAlways UBIQUITTY_SWIFT_NAME(closeAlways),
    Restart UBIQUITTY_SWIFT_NAME(restart),
};

/// The next action for a completed process.
enum class UBIQUITTY_CLOSED_ENUM ProcessExitAction {
    KeepOutput UBIQUITTY_SWIFT_NAME(keepOutput),
    AwaitStatus UBIQUITTY_SWIFT_NAME(awaitStatus),
    Close UBIQUITTY_SWIFT_NAME(close),
    Restart UBIQUITTY_SWIFT_NAME(restart),
    RestartLimitReached UBIQUITTY_SWIFT_NAME(restartLimitReached),
};

/// Resolves a stored exit choice, preserving output for unrecognized values.
///
/// - Parameter token: The configuration token.
/// - Returns: The requested behavior, or `KeepOutput`.
[[nodiscard]] inline ProcessExitBehavior processExitBehavior(std::string_view token)
{
    if (token == "close_on_success") {
        return ProcessExitBehavior::CloseOnSuccess;
    }
    if (token == "close_always") {
        return ProcessExitBehavior::CloseAlways;
    }
    if (token == "restart") {
        return ProcessExitBehavior::Restart;
    }
    return ProcessExitBehavior::KeepOutput;
}

/// Limits automatic relaunches across successive children of one session.
class ProcessExitPolicy {
public:
    /// Records a successful launch without discarding its restart budget.
    ///
    /// - Parameter now: The monotonic launch time.
    void didLaunch(Ticks now)
    {
        m_launchedAt = now;
        m_didDecide = false;
    }

    /// Decides once per child, waiting for a status when clean exit matters.
    ///
    /// A child that runs for a minute restores the budget. Repeated reads of
    /// one exit cannot consume it or request another restart.
    ///
    /// - Parameters:
    ///   - behavior: The effective profile choice.
    ///   - ended: The locally owned child's exit facts.
    ///   - now: The monotonic observation time.
    /// - Returns: The action, with at most three consecutive automatic restarts.
    [[nodiscard]] ProcessExitAction action(ProcessExitBehavior behavior, const Title::SessionEnd& ended, Ticks now)
    {
        if (!ended.didEnd || m_didDecide) {
            return ProcessExitAction::KeepOutput;
        }
        if (behavior == ProcessExitBehavior::CloseOnSuccess && !ended.hasStatus) {
            return ProcessExitAction::AwaitStatus;
        }
        m_didDecide = true;
        switch (behavior) {
        case ProcessExitBehavior::KeepOutput:
            return ProcessExitAction::KeepOutput;
        case ProcessExitBehavior::CloseOnSuccess:
            return !ended.wasSignalled && !ended.code ? ProcessExitAction::Close : ProcessExitAction::KeepOutput;
        case ProcessExitBehavior::CloseAlways:
            return ProcessExitAction::Close;
        case ProcessExitBehavior::Restart:
            if (now.since(m_launchedAt) >= Ticks::fromMilliseconds(60000).count) {
                m_restartCount = 0;
            }
            if (m_restartCount >= 3) {
                return ProcessExitAction::RestartLimitReached;
            }
            ++m_restartCount;
            return ProcessExitAction::Restart;
        }
        return ProcessExitAction::KeepOutput;
    }

private:
    Ticks m_launchedAt;
    unsigned m_restartCount = 0;
    bool m_didDecide = false;
};

} // namespace Ubiquitty::Session
