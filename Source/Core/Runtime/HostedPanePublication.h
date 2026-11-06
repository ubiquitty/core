// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Runtime/FrameCoherence.h"
#include "Core/Session/PaneLinkEvents.h"
#include "Core/Terminal.h"

namespace Ubiquitty::Runtime {

/// Coherence and publication metadata for one externally owned pane model.
/// Every call is made under that model's canonical lock. The host retains
/// the publication until the pane's views unbind and its model is disposed.
class HostedPanePublication {
public:
    explicit HostedPanePublication(const Ubiquitty::Terminal& terminal)
        : m_terminal(&terminal)
    {
    }

    [[nodiscard]] bool isFor(const Ubiquitty::Terminal& terminal) const { return m_terminal == &terminal; }

    /// Each model reports the first focus fact, then only its actual edges.
    [[nodiscard]] bool changeFocus(Modes::FocusState focus)
    {
        if (m_focus == focus)
            return false;
        m_focus = focus;
        return true;
    }

    void noteOutput(const Ubiquitty::Terminal& terminal, std::uint64_t now)
    {
        m_rows = terminal.rows;
        m_columns = terminal.columns;
        m_coherence.noteDrainPass(terminal, now, PassEnd::WouldBlock);
    }

    void noteRecovery(std::uint64_t now) { m_coherence.noteHostChange(now); }

    void noteGeometry(const Ubiquitty::Terminal& terminal, std::uint64_t now)
    {
        if (terminal.rows == m_rows && terminal.columns == m_columns)
            return;
        m_rows = terminal.rows;
        m_columns = terminal.columns;
        m_coherence.noteHostChange(now);
    }

    struct Progress {
        bool hasPublished = false;
        std::optional<std::uint64_t> askAfterMicroseconds;
    };

    /// Advances the same rule local PTY panes and the Sync host use.
    /// A quiet timer also ages a held redraw when no more output arrives.
    [[nodiscard]] Progress advance(Ubiquitty::Terminal& terminal, std::uint64_t now, Ticks ticks)
    {
        const auto admission = m_coherence.admit(terminal, now, ticks, ReadReadiness::Quiet);
        if (admission.isAdmitted) {
            const auto dirtySince = m_coherence.dirtySinceMicroseconds();
            m_lastFrame = Session::FrameReady { .frameClass = wireFrameClass(admission.frameClass),
                .generation = ++m_generation,
                .lagMicroseconds = dirtySince && now >= *dirtySince ? now - *dirtySince : 0 };
            m_coherence.noteFrameTaken(terminal, now);
        }
        return { .hasPublished = admission.isAdmitted, .askAfterMicroseconds = m_coherence.microsecondsUntilAsk(terminal, now, ticks) };
    }

    [[nodiscard]] std::optional<Session::FrameReady> lastFrame() const { return m_lastFrame; }

private:
    const Ubiquitty::Terminal* m_terminal;
    FrameCoherence m_coherence { CoherenceTuning { .isPaced = false } };
    std::uint64_t m_generation = 0;
    std::optional<Session::FrameReady> m_lastFrame;
    std::optional<Modes::FocusState> m_focus;
    int m_rows = 0;
    int m_columns = 0;
};

} // namespace Ubiquitty::Runtime
