// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/SessionHandoverFacts.h"

namespace Ubiquitty::Host {
namespace {

namespace Sync = Protocol::Sync;

// One mapping drives capture and restore so a renamed scalar cannot silently
// enter one direction alone. Compound owners remain separate admitted sections.
template<bool isCapturing, typename Session>
void transferFacts(Session& session, Sync::SessionFacts& facts)
{
    const auto field = [](auto& member, auto& value) {
        if constexpr (isCapturing)
            value = member;
        else
            member = std::move(value);
    };
    field(session.id, facts.sessionID);
    field(session.createdUnix, facts.createdUnix);
    field(session.lastInputClientID, facts.lastInputClientID);
    field(session.lastHumanInputClientID, facts.lastHumanInputClientID);
    field(session.activity.lastOutputUnixMilliseconds, facts.activity.lastOutputUnixMilliseconds);
    field(session.activity.lastInputUnixMilliseconds, facts.activity.lastInputUnixMilliseconds);
    field(session.stampedTypedInputWrites, facts.stampedTypedInputWrites);
    field(session.seenPersonFocusGains, facts.seenPersonFocusGains);
    field(session.lastRecordsMilliseconds, facts.lastRecordsMilliseconds);
    field(session.recordsOwedAtMilliseconds, facts.recordsOwedAtMilliseconds);
    field(session.lastActivityStampMilliseconds, facts.lastActivityStampMilliseconds);
    field(session.describedStructuralGeneration, facts.describedStructuralGeneration);
    field(session.recordedActivityGeneration, facts.recordedActivityGeneration);
    field(session.end, facts.end);
    field(session.outputOrigin, facts.outputOrigin);
    field(session.driverOriginFloor, facts.driverOriginFloor);
    field(session.firstInputPrincipal, facts.firstInputPrincipal);
    field(session.hasMultipleInputPrincipals, facts.hasMultipleInputPrincipals);
    field(session.isFrameDue, facts.isFrameDue);
    field(session.isDriverFocusRecorded, facts.isDriverFocusRecorded);
    field(session.isWindowSizeDue, facts.isWindowSizeDue);
    field(session.geometryChangesApplied, facts.geometryChangesApplied);
    field(session.presentedDueAt, facts.presentedDueAt);
    field(session.presentedStandIn, facts.presentedStandIn);
    field(session.unpublishedSince, facts.unpublishedSince);
    field(session.lastEmittedCommandGeneration, facts.lastEmittedCommandGeneration);
    field(session.lastEmittedCommandLineGeneration, facts.lastEmittedCommandLineGeneration);
    field(session.lastEmittedCommandStartedGeneration, facts.lastEmittedCommandStartedGeneration);
    field(session.lastForegroundIsShell, facts.wasForegroundShell);
    field(session.lastForegroundName, facts.lastForegroundName);
    field(session.foregroundGeneration, facts.foregroundGeneration);
    field(session.publishedEchoFacts, facts.publishedEchoFacts);
    field(session.reportedProgramStatus, facts.reportedProgramStatus);
    field(session.reportedActivityGeneration, facts.reportedActivityGeneration);
    // This frozen subsection carries only the sampled process strings. Its
    // former cache revision is zero on capture and ignored on restore: the
    // typed source continuation owns the live PaneFacts revision. Every other
    // PaneHostFacts field is derived anew by paneHostFactsOf before publication.
    if constexpr (isCapturing)
        facts.paneFacts.revision = 0;
    field(session.paneFacts.foregroundDirectory, facts.paneFacts.currentDirectory);
    field(session.paneFacts.processChain, facts.paneFacts.processChain);
    field(session.paneFacts.processChainWithArguments, facts.paneFacts.processChainWithArguments);
    field(session.paneFacts.foreground.name, facts.paneFacts.foregroundName);
    field(session.paneFacts.shellCommandName, facts.paneFacts.shellCommandName);
    field(session.paneFacts.terminalName, facts.paneFacts.ttyName);
    field(session.shellExited, facts.didShellExit);
    field(session.exitReason, facts.exitReason);
    field(session.mouseTrackingProcessGroup, facts.mouseTrackingProcessGroup);
    field(session.adopted, facts.isAdopted);
    field(session.historyLinesNotCarried, facts.historyLinesNotCarried);
    field(session.workspaceName, facts.workspaceName);
    field(session.endsWhenDetached, facts.shouldEndWhenDetached);
    field(session.sshHost, facts.sshHost);
    field(session.wasRestoredAfterHostRestart, facts.wasRestoredAfterHostRestart);
    field(session.wasKilledBySignal, facts.wasKilledBySignal);
    field(session.exitStatusDeadlineMilliseconds, facts.exitStatusDeadlineMilliseconds);
    field(session.observedDirectory, facts.observedDirectory);
    field(session.graceExpiresMilliseconds, facts.graceExpiresMilliseconds);
    field(session.isGraceAwaitingHuman, facts.isGraceAwaitingHuman);
    field(session.landedPastePaths, facts.landedPastePaths);
    if constexpr (isCapturing) {
        facts.placement = { std::to_underlying(session.placement.surface), std::to_underlying(session.placement.tab), std::to_underlying(session.placement.pane) };
        if (session.pendingGeometry)
            facts.pendingGeometry = Sync::PaneGeometry { static_cast<std::uint16_t>(session.pendingGeometry->second), static_cast<std::uint16_t>(session.pendingGeometry->first) };
        if (session.presentedApplied)
            facts.presentedApplied = Sync::SessionFacts::Presented { session.presentedApplied->owner, session.presentedApplied->revision, session.presentedApplied->appliedAt };
    } else {
        session.placement = { static_cast<Ubiquitty::Session::Layout::SurfaceID>(facts.placement.surface),
            static_cast<Ubiquitty::Session::Layout::TabID>(facts.placement.tab), static_cast<Ubiquitty::Session::Layout::PaneID>(facts.placement.pane) };
        if (facts.pendingGeometry)
            session.pendingGeometry = std::pair { static_cast<int>(facts.pendingGeometry->columns), static_cast<int>(facts.pendingGeometry->rows) };
        if (facts.presentedApplied)
            session.presentedApplied = typename Session::PresentedApplied { .owner = facts.presentedApplied->owner, .revision = facts.presentedApplied->revision, .panePixelsRevision = 0, .appliedAt = facts.presentedApplied->appliedAt };
    }
}

} // namespace

Protocol::Sync::SessionFacts SessionHandoverFacts::capture(const Daemon::Session& session)
{
    Protocol::Sync::SessionFacts facts;
    transferFacts<true>(session, facts);
    return facts;
}

bool SessionHandoverFacts::restore(Daemon::Session& session, Protocol::Sync::SessionFacts&& facts)
{
    if (session.id || session.term || session.stream || session.screenSource || !session.landedPastePaths.empty())
        return false;
    session.ownsLandedPastePaths = false;
    transferFacts<false>(session, facts);
    return true;
}

} // namespace Ubiquitty::Host
