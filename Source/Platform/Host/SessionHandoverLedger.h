// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/HandoverLedger.h"
#include "Platform/Host/Daemon.h"

namespace Ubiquitty::Host {

/// Couples a physical Session member with its handover preservation requirement.
struct SessionHandoverMember {
    Protocol::Sync::LedgerEntry layout;
    Protocol::Sync::HandoverCarriage carriage;
};

/// Extracts member layout for the same completeness rule as the terminal ledger.
///
/// - Parameter members: Every session member, in physical declaration order.
/// - Returns: Layout entries suitable for the padding-aware tiling check.
template<std::size_t count>
consteval std::array<Protocol::Sync::LedgerEntry, count> sessionMemberLayouts(const std::array<SessionHandoverMember, count>& members)
{
    std::array<Protocol::Sync::LedgerEntry, count> entries {};
    for (std::size_t index = 0; index < count; ++index)
        entries[index] = members[index].layout;
    return entries;
}

/// Checks a complete session layout and explicit, unique preservation requirements.
///
/// No session AtGround entry is admitted without a session-level predicate.
/// Terminal ground alone establishes neither input nor authority quiescence.
///
/// - Parameters:
///   - members: Every member's layout and preservation requirement.
///   - size: The session structure's size.
///   - alignment: The session structure's alignment.
/// - Returns: Whether the named members tile the structure with valid dispositions.
template<std::size_t count>
consteval bool isSessionHandoverLedgerComplete(const std::array<SessionHandoverMember, count>& members,
    std::size_t size, std::size_t alignment)
{
    if (!Protocol::Sync::tiles(sessionMemberLayouts(members), size, alignment))
        return false;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& member = members[index];
        if (member.layout.name.empty())
            return false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (member.layout.name == members[prior].layout.name)
                return false;
        }
        if (member.carriage != Protocol::Sync::HandoverCarriage::Carried
            && member.carriage != Protocol::Sync::HandoverCarriage::Rewired
            && member.carriage != Protocol::Sync::HandoverCarriage::Recomputed)
            return false;
    }
    return true;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

/// Requires a portable disposition for every member of the daemon's session.
///
/// These are requirements, not a serializer. Carried compound owners must split
/// portable state from rewired references: capabilityPolicy contains an exact
/// held consent operation; inputEncoder contains in-progress paste normalization
/// and bracket state; screenSource owns input queues, identities and publication
/// baselines. Replacing any of these owners with an empty object loses work.
///
/// The stream requires explicit transfer of its process/descriptor and pending
/// output ownership, never pointer copying or a replacement shell. Client IDs
/// require the whole host's identity map, and exitReason requires a value codec.
///
/// Recomputed members are probe/encoding caches, except the session-consent
/// field agentSessionGrantIdentities, which restores to empty, requiring a
/// fresh person's approval. It never defaults to a grant.
/// All pending exact-operation consent and its origin remain carried requirements.
///
/// This does not cover Daemon-wide connections, grants, tokens, consent maps,
/// workspaces, listeners or timers. No runtime handover follows from this ledger.
struct SessionHandoverLedger {
private:
    using Session = Daemon::Session;
    using Carriage = Protocol::Sync::Carriage;
    using HandoverCarriage = Protocol::Sync::HandoverCarriage;

public:
    /// The size and alignment the member requirements must tile.
    inline static constexpr std::size_t size = sizeof(Session);
    inline static constexpr std::size_t alignment = alignof(Session);

    /// Every direct member; nested owned state requires its own portable codec.
    inline static constexpr std::array members {
#define MEMBER(member, disposition) \
    SessionHandoverMember { { offsetof(Session, member), sizeof(Session::member), alignof(decltype(Session::member)), Carriage::HostOnly, #member }, HandoverCarriage::disposition }
        MEMBER(id, Carried),
        MEMBER(createdUnix, Carried),
        MEMBER(placement, Carried),
        MEMBER(lastInputClientID, Carried),
        MEMBER(lastHumanInputClientID, Carried),
        MEMBER(activity, Carried),
        MEMBER(stampedTypedInputWrites, Carried),
        MEMBER(seenPersonFocusGains, Carried),
        MEMBER(lastRecordsMilliseconds, Carried),
        MEMBER(recordsOwedAtMilliseconds, Carried),
        MEMBER(lastActivityStampMilliseconds, Carried),
        MEMBER(describedStructuralGeneration, Carried),
        MEMBER(recordedActivityGeneration, Carried),
        MEMBER(end, Carried),
        MEMBER(outputOrigin, Carried),
        MEMBER(driverOriginFloor, Carried),
        MEMBER(firstInputPrincipal, Carried),
        MEMBER(hasMultipleInputPrincipals, Carried),
        MEMBER(capabilityPolicy, Carried),
        MEMBER(effects, Carried),
        MEMBER(clipboardClient, Rewired),
        MEMBER(shellClient, Rewired),
        // Reinstalled by installTerminalClients; a count not yet relayed
        // survives as the Terminal's carried bellPending, one bell.
        MEMBER(effectRoute, Rewired),
        // Ticked again before the successor parses its first read.
        MEMBER(clock, Recomputed),
        // Refilled before the successor parses its first read from carried
        // attachment faces, then from clients as they resume and republish.
        MEMBER(runFaces, Recomputed),
        MEMBER(configuration, Carried),
        MEMBER(stream, Carried),
        MEMBER(dragDropQueue, Recomputed),
        MEMBER(dragDropOwner, Recomputed),
        MEMBER(dragDropEpoch, Recomputed),
        MEMBER(term, Carried),
        // Native measurement belongs to a live connection, reselected on resume.
        MEMBER(richTextMeasurerClientID, Recomputed),
        MEMBER(richTextMeasurerElections, Recomputed),
        MEMBER(isRichTextMeasurerStale, Recomputed),
        MEMBER(isFrameDue, Carried),
        MEMBER(isDriverFocusRecorded, Carried),
        MEMBER(pendingGeometry, Carried),
        MEMBER(isWindowSizeDue, Carried),
        MEMBER(geometryChangesApplied, Carried),
        MEMBER(presentedApplied, Carried),
        MEMBER(presentedDueAt, Carried),
        MEMBER(presentedStandIn, Carried),
        MEMBER(unpublishedSince, Carried),
        MEMBER(coherence, Carried),
        MEMBER(inputSink, Rewired),
        MEMBER(inputEncoder, Carried),
        MEMBER(screenSource, Carried),
        MEMBER(census, Recomputed),
        MEMBER(lastEmittedCommandGeneration, Carried),
        MEMBER(lastEmittedCommandLineGeneration, Carried),
        MEMBER(lastEmittedCommandStartedGeneration, Carried),
        MEMBER(lastForegroundIsShell, Carried),
        MEMBER(lastForegroundName, Carried),
        MEMBER(foregroundGeneration, Carried),
        MEMBER(publishedEchoFacts, Carried),
        MEMBER(reportedProgramStatus, Carried),
        MEMBER(reportedActivityGeneration, Carried),
        MEMBER(lastForegroundProbeMilliseconds, Recomputed),
        MEMBER(lastForegroundProbeGroup, Recomputed),
        MEMBER(foregroundProbeSettled, Recomputed),
        MEMBER(isProbedForegroundShell, Recomputed),
        MEMBER(probedForegroundName, Recomputed),
        MEMBER(probedForegroundDirectory, Recomputed),
        MEMBER(foregroundProbeOwedAtMilliseconds, Recomputed),
        MEMBER(processTreeEdge, Recomputed),
        MEMBER(isTreeEdgeTracked, Recomputed),
        MEMBER(firstUnansweredTreeEdgeMilliseconds, Recomputed),
        MEMBER(latestTreeEdgeMilliseconds, Recomputed),
        MEMBER(paneFacts, Carried),
        MEMBER(lastPaneFactsWalkMilliseconds, Recomputed),
        MEMBER(paneFactsChainProcessGroup, Recomputed),
        MEMBER(paneFactsChainForegroundCount, Recomputed),
        MEMBER(shellExited, Carried),
        MEMBER(exitReason, Carried),
        // The successor's first reply drain sets it again.
        MEMBER(isReplyWriteRefused, Recomputed),
        MEMBER(mouseTrackingProcessGroup, Carried),
        MEMBER(agentSessionGrantIdentities, Recomputed),
        MEMBER(adopted, Carried),
        MEMBER(sharedWindowFingerprint, Recomputed),
        MEMBER(historyLinesNotCarried, Carried),
        MEMBER(workspaceName, Carried),
        MEMBER(endsWhenDetached, Carried),
        MEMBER(sshHost, Carried),
        MEMBER(wasRestoredAfterHostRestart, Carried),
        MEMBER(wasKilledBySignal, Carried),
        MEMBER(exitStatusDeadlineMilliseconds, Carried),
        MEMBER(observedDirectory, Carried),
        MEMBER(graceExpiresMilliseconds, Carried),
        MEMBER(isGraceAwaitingHuman, Carried),
        MEMBER(landedPastePaths, Carried),
        MEMBER(ownsLandedPastePaths, Rewired),
#undef MEMBER
    };
};

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static_assert(isSessionHandoverLedgerComplete(SessionHandoverLedger::members,
    SessionHandoverLedger::size, SessionHandoverLedger::alignment));

} // namespace Ubiquitty::Host
