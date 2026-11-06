// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/Records.h"

#include <utility>

namespace Ubiquitty::Runtime {

/// An owned record of one local or adopted model's canonical facts.
/// The actual model lock protects refresh and every borrowed read. Unchanged
/// display frames compare bounded strings and scalars without copying them or
/// rebuilding the user-variable vector. Notifications are the caller's job,
/// after it releases the lock on its presentation owner thread.
class PaneFactsCapture {
public:
    [[nodiscard]] const Protocol::Sync::PaneFacts* factsFor(const Ubiquitty::Terminal* terminal) const
    {
        return terminal && terminal == m_terminal ? &m_facts : nullptr;
    }
    [[nodiscard]] std::uint64_t generation() const { return m_generation; }

    void refresh(const Ubiquitty::Terminal& terminal, const Protocol::Sync::PaneHostFacts& host)
    {
        const Stamp stamp { terminal.rows, terminal.columns, terminal.progressGeneration, terminal.tabStatusGeneration,
            terminal.attentionGeneration, terminal.lastCommandExitCode };
        if (m_terminal == &terminal && m_stamp == stamp && m_host == host && matchesText(terminal))
            return;
        auto next = Protocol::Sync::paneFactsOf(terminal, host);
        next.revision = m_facts.revision;
        const bool didChange = m_terminal != &terminal || !(next == m_facts);
        if (didChange) {
            if (m_terminal != &terminal || Protocol::Sync::differsBeyondActivity(m_facts, next))
                ++next.revision;
            m_facts = std::move(next);
            ++m_generation;
        }
        m_terminal = &terminal;
        m_stamp = stamp;
        if (!(m_host == host))
            m_host = host;
    }

private:
    struct Stamp {
        int rows;
        int columns;
        std::uint32_t progress;
        std::uint32_t tabStatus;
        std::uint32_t attention;
        std::int32_t exitStatus;

        friend bool operator==(const Stamp&, const Stamp&) = default;
    };

    [[nodiscard]] bool matchesText(const Ubiquitty::Terminal& terminal) const
    {
        if (terminal.titleUTF8 != m_facts.programTitle || terminal.currentFileURL != m_facts.currentFileURL
            || (!terminal.currentDirectory.empty() && terminal.currentDirectory != m_facts.currentDirectory))
            return false;
        // An empty OSC directory falls back to the sampled host directory.
        if (terminal.currentDirectory.empty() && m_facts.currentDirectory != m_host.foregroundDirectory)
            return false;
        std::size_t index = 0;
        for (const auto& variable : terminal.userVariables) {
            if (!variable.used)
                continue;
            if (index == m_facts.userVariables.size() || variable.name != m_facts.userVariables[index].name
                || variable.value != m_facts.userVariables[index].value)
                return false;
            ++index;
        }
        return index == m_facts.userVariables.size();
    }

    const Ubiquitty::Terminal* m_terminal = nullptr;
    Stamp m_stamp {};
    Protocol::Sync::PaneHostFacts m_host;
    Protocol::Sync::PaneFacts m_facts;
    std::uint64_t m_generation = 0;
};

} // namespace Ubiquitty::Runtime
