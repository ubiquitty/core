// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Modes/SyncOutput.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/Runtime/PaneFactsCapture.h"
#include "Core/Runtime/RuntimeLink.h"
#include "Core/Session/PaneLinkEvents.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>

namespace Ubiquitty::Runtime {

/// Coherence and publication metadata for one externally owned pane model.
/// Mutations hold that model's canonical lock. Readers retain a handle and
/// copy the published scalars without borrowing the model or its lock.
class HostedPanePublication {
public:
    explicit HostedPanePublication(const Ubiquitty::Terminal& terminal)
        : m_terminal(&terminal)
    {
    }

    [[nodiscard]] bool isFor(const Ubiquitty::Terminal& terminal) const { return m_terminal == &terminal; }

    /// The exact owner's presentation inputs and independent notification epochs.
    struct State {
        FramePresentationState presentation;
        std::uint64_t factsGeneration = 0;
        Session::FrameReady frame;
        bool hasFrame = false;
        /// The pane's open synchronized update, and the tick its budget runs
        /// from. The owner's coherence stamps and expires it as it advances,
        /// so a view holds its frame from here and never writes the model.
        Modes::SyncOutput::BlockMark block;
    };

    [[nodiscard]] State read() const { return m_state.read(); }

    /// The owner has stopped every writer before disposing the model.
    void retire() { m_state.publish({}); }

    /// Publishes model facts even when coherence has admitted no new frame.
    /// The caller holds this exact model's canonical lock.
    ///
    /// - Parameter terminal: The model this publication represents.
    void publishModelState(const Ubiquitty::Terminal& terminal)
    {
        Protocol::Sync::PaneHostFacts host;
        host.isAdopted = true;
        const auto textArea = Ubiquitty::reportedTextAreaPixels(terminal);
        host.pixels = { .cellWidth = terminal.cellPixelWidth, .cellHeight = terminal.cellPixelHeight, .textAreaWidth = static_cast<std::uint16_t>(std::clamp(textArea.width, 0, 65535)), .textAreaHeight = static_cast<std::uint16_t>(std::clamp(textArea.height, 0, 65535)) };
        m_facts.refresh(terminal, host);
        m_state.publish({ .presentation = captureFramePresentationState(&terminal),
            .factsGeneration = m_facts.generation(),
            .frame = m_lastFrame.value_or(Session::FrameReady {}),
            .hasFrame = m_lastFrame.has_value(),
            .block = Modes::SyncOutput::blockMark(terminal) });
    }

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
        publishModelState(terminal);
        return { .hasPublished = admission.isAdmitted, .askAfterMicroseconds = m_coherence.microsecondsUntilAsk(terminal, now, ticks) };
    }

    [[nodiscard]] std::optional<Session::FrameReady> lastFrame() const
    {
        const auto state = read();
        return state.hasFrame ? std::optional { state.frame } : std::nullopt;
    }

private:
    const Ubiquitty::Terminal* m_terminal;
    FrameCoherence m_coherence { CoherenceTuning { .isPaced = false } };
    std::uint64_t m_generation = 0;
    std::optional<Session::FrameReady> m_lastFrame;
    PaneFactsCapture m_facts;
    SnapshotPublisher<State> m_state;
    std::optional<Modes::FocusState> m_focus;
    int m_rows = 0;
    int m_columns = 0;
};

} // namespace Ubiquitty::Runtime
