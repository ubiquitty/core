// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>

namespace Ubiquitty::Runtime {

/// How long the PTY size is re-sent after a program takes the terminal.
///
/// An application reads TIOCGWINSZ once as it starts and installs its SIGWINCH
/// handler some time later. A size change landing in between — a tab bar
/// appearing and shrinking the grid before the program's first paint — reaches
/// no handler, and the program lays out for a size the terminal no longer is.
/// One re-send at the takeover is not enough: the shrink can land after it,
/// while the size sent was still the old one.
inline constexpr std::uint32_t windowSizeReassertWindowMicros = 1'000'000;

/// The least time between two re-sends inside the window: four in a second.
///
/// Spaced rather than sent on every sample, because a re-send makes a
/// full-screen program redraw, the redraw is output, and a rule that answered
/// every drain would keep the program redrawing for the whole window.
inline constexpr std::uint32_t windowSizeReassertSpacingMicros = 250'000;

/// What one process sample says about the program holding the terminal.
struct ForegroundSample {
    /// The foreground process group; zero when none resolved.
    std::uint32_t processGroup = 0;
    /// Whether that group is the session's own shell.
    bool isShell = false;
};

/// Decides when the PTY size is re-sent with a SIGWINCH after a takeover.
///
/// Stated once here and run by the session's runtime, so every frontend
/// re-sends on the same schedule rather than each window controller keeping
/// its own clock.
///
/// A takeover is a sampled group that is neither zero nor the group already
/// recorded, or the recorded group read again as a job after it read as the
/// shell's: a fork that has not exec'd runs the shell's code, and the job it
/// becomes in the same group is the takeover. A takeover by the shell opens no
/// window: a shell reads the size
/// itself as it draws its prompt, and bash redraws a multi-line prompt on every
/// SIGWINCH, printing its upper lines again. Times are the runtime's 32-bit
/// monotonic microseconds, compared by unsigned difference so a wrap inside the
/// window still closes it.
class WindowSizeReassert {
public:
    /// Records one process sample, opening the window on a takeover.
    ///
    /// - Parameters:
    ///   - sample: What the sample read about the foreground.
    ///   - nowMicros: When it was read.
    void noteSample(ForegroundSample sample, std::uint32_t nowMicros)
    {
        if (sample.processGroup == 0)
            return;
        const bool isSameGroup = sample.processGroup == m_foregroundProcessGroup;
        if (isSameGroup && !(m_isRecordedShell && !sample.isShell))
            return;
        m_foregroundProcessGroup = sample.processGroup;
        m_isRecordedShell = sample.isShell;
        m_isReasserting = !sample.isShell;
        m_takeoverMicros = nowMicros;
        m_hasSent = false;
    }

    /// Consumes the re-send due now, if one is.
    ///
    /// - Parameter nowMicros: The current time.
    /// - Returns: Whether the caller re-sends the size now.
    [[nodiscard]] bool shouldSendNow(std::uint32_t nowMicros)
    {
        const std::optional<std::uint32_t> wait = microsUntilDue(nowMicros);
        if (!wait || *wait > 0)
            return false;
        m_sentMicros = nowMicros;
        m_hasSent = true;
        return true;
    }

    /// How long until the next re-send is due.
    ///
    /// Also closes a window that has run out, which is why it is not const.
    ///
    /// - Parameter nowMicros: The current time.
    /// - Returns: Zero when one is due now, nothing when the window is closed.
    [[nodiscard]] std::optional<std::uint32_t> microsUntilDue(std::uint32_t nowMicros)
    {
        if (!m_isReasserting)
            return std::nullopt;
        if (nowMicros - m_takeoverMicros >= windowSizeReassertWindowMicros) {
            m_isReasserting = false;
            return std::nullopt;
        }
        if (!m_hasSent)
            return 0;
        const std::uint32_t sinceSent = nowMicros - m_sentMicros;
        return sinceSent >= windowSizeReassertSpacingMicros ? 0 : windowSizeReassertSpacingMicros - sinceSent;
    }

    /// The group the last takeover recorded; zero before any.
    ///
    /// The re-sends inside the window are decided for this group, so it is what
    /// the transport compares the foreground against before it signals.
    [[nodiscard]] std::uint32_t foregroundProcessGroup() const { return m_foregroundProcessGroup; }

private:
    std::uint32_t m_foregroundProcessGroup = 0;
    std::uint32_t m_takeoverMicros = 0;
    std::uint32_t m_sentMicros = 0;
    bool m_isRecordedShell = false;
    bool m_isReasserting = false;
    bool m_hasSent = false;
};

} // namespace Ubiquitty::Runtime
