// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/Log.h"
#include "PAL/TerminalClients.h"

#include <cstdio>
#include <string_view>
#include <utility>

namespace Ubiquitty::Agent {

/// The agent session's capability policy: nothing escapes, whoever asks.
///
/// This module's Terminal is a mirror. `SyncClientCore` assembles it from the
/// screen plane's checkpoints and updates and never calls `processBytes`, so the
/// parser that owns every capability gate site does not run here — no gate is
/// reachable today, and this policy is consulted by nothing.
///
/// It is installed anyway, because "no policy" is not the same answer as "no".
/// An unset policy answers from PAL's `defaultDecision`, which allows an `Agent`
/// principal its benign, informational and rendering effects and every effect
/// of a `Local` one. This module's answer is no to all of them, and the first
/// client wired onto this Terminal should land on that answer rather than on a
/// fallback chosen for terminals nobody configured.
///
/// Deny rather than a tier table, and that is the whole design decision: a
/// mirror exists to be READ. An effect reaching a client from here would be a
/// bug rather than a feature, so there is no capability whose answer should be
/// yes and nothing to adjudicate per tier. The daemon's `HostCapabilityPolicy`
/// is where an `Agent` principal is weighed against what it asked for; this
/// terminal is not that terminal, and a second tier table here would be a second
/// answer to one question.
///
/// So the day someone wires a client here it will not work until they decide
/// what this principal may do. That is the failure this is for: a feature that
/// does not arrive is noticed by whoever is adding it, and an effect that
/// escapes for every origin is noticed by nobody.
class AgentCapabilityPolicy final : public Ubiquitty::CapabilityPolicy {
public:
    /// Refuses every capability for every origin.
    ///
    /// - Returns: Deny.
    [[nodiscard]] Ubiquitty::CapabilityDecision check(Ubiquitty::Capability capability, Ubiquitty::CapabilityOrigin, std::string_view) override
    {
        reportFirstRefusal(capability);
        return Ubiquitty::CapabilityDecision::Deny;
    }

private:
    /// Says so the first time, and only the first time.
    ///
    /// A refusal that produces silence is indistinguishable from a feature
    /// nobody implemented, and whoever meets it will be adding a client rather
    /// than reading this header — so the first one names where the rule is
    /// written. Once rather than per effect: a mirror that starts receiving
    /// them would flood, the flood gets muted, and that is silence again with
    /// extra steps.
    ///
    /// - Parameters:
    ///   - capability: What was asked for, as its enumerator value — there is
    ///     no name table in Core, and a number a reader can grep for beats a
    ///     second copy of the enum kept here.
    void reportFirstRefusal(Ubiquitty::Capability capability)
    {
        if (m_didReportRefusal)
            return;
        m_didReportRefusal = true;
        Ubiquitty::PAL::logLine(stderr,
            "ubiquitty: the agent session's terminal refuses every capability "
            "(asked for {}). It is a mirror, not a surface that acts.",
            std::to_underlying(capability));
    }

    bool m_didReportRefusal = false;
};

} // namespace Ubiquitty::Agent
