// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/HandoverGround.h"

#include "Core/Terminal.h"
#include "Core/VT/ParseState.h"

namespace Ubiquitty::Protocol::Sync {

HandoverBlockers terminalHandoverBlockers(const Terminal& terminal)
{
    HandoverBlockers blockers;
    blockers.set(HandoverBlocker::NativeDrag, terminal.dragDropSession && terminal.dragDropSession->hasPendingWork());
    blockers.set(HandoverBlocker::ErasureQualification, terminal.erasureMode != VT::ErasureMode::Protect || terminal.renditionCombination != VT::RenditionCombination::Cumulative || !terminal.qualifiedAreas.isEmpty());
    blockers.set(HandoverBlocker::RichTextMeasurement, !terminal.richTextMeasurements.empty() || terminal.typography.needsReport);
    blockers.set(HandoverBlocker::ParserMidSequence, !VT::isParserAtGround(&terminal));
    blockers.set(HandoverBlocker::IncompleteCharacter, terminal.parser.utf8.need || terminal.parser.inputEncoding.need);
    blockers.set(HandoverBlocker::LegacySequence, terminal.legacyState != 0);
    blockers.set(HandoverBlocker::DeviceControlString, terminal.dcsTmuxPassthroughDepth != 0);
    blockers.set(HandoverBlocker::ClipboardChunkOpen, terminal.osc52ChunkActive || terminal.osc52ChunkOverflow || terminal.osc52Chunk.length != 0);
    blockers.set(HandoverBlocker::ClipboardReadPending, terminal.isOSC52ClipboardReadInFlight);
    blockers.set(HandoverBlocker::ExtendedClipboardOpen, terminal.isExtendedClipboardWriteActive || terminal.didExtendedClipboardWriteOverflow || terminal.extendedClipboardWriteItemCount != 0 || terminal.extendedClipboardWriteTotalBytes != 0 || terminal.extendedClipboardBase64TailLength != 0 || terminal.isExtendedClipboardBase64Complete);
    // APC and Sixel byte buffers can retain their last completed sequence.
    // Parser ground rules out those open strings; Kitty's multi-APC transfer
    // has a separate lifetime, including gaps between complete APC strings.
    blockers.set(HandoverBlocker::ImageTransferOpen, terminal.images.kittyChunkActive);
    const auto hasPending = [](const std::unique_ptr<TaskQueue>& queue) { return queue && queue->pendingCount() != 0; };
    blockers.set(HandoverBlocker::ImageDecodePending, hasPending(terminal.asyncDecodes.sixel) || hasPending(terminal.asyncDecodes.kittyDecode) || hasPending(terminal.asyncDecodes.kittyInflate));
    blockers.set(HandoverBlocker::NotificationOpen, terminal.kittyNotifications.openCount() != 0);
    blockers.set(HandoverBlocker::SynchronizedOutput, terminal.modes.syncOutput);
    blockers.set(HandoverBlocker::DeferredScreenMutation, terminal.scrollLaneRun.active || terminal.bandScrollRun.active || terminal.pendingHistoryRewrapColumns != 0 || terminal.isAlignedTabActive);
    // These opaque sinks cannot be reopened from Terminal state. A future
    // explicit descriptor-transfer contract must replace this refusal.
    blockers.set(HandoverBlocker::ExternalSinkOpen, terminal.printSinkFile || terminal.sessionLogFile);
    blockers.set(HandoverBlocker::FontOperationPending, terminal.fontRequests.pendingCount() != 0);
    const auto& integration = terminal.integration;
    const auto& tmux = integration.tmux;
    blockers.set(HandoverBlocker::IntegrationActive, tmux.state != Tmux::TmuxState::Passthrough || tmux.lineLength != 0 || tmux.responseDepth != 0 || tmux.dcsPrefixPosition != 0 || tmux.dcsEnvelopeActive || tmux.dcsSawEscape || tmux.beginPrefixPosition != 0 || tmux.outputSubState != Tmux::TmuxOutputSubState::None || integration.cachedPaneTarget);
    return blockers;
}

} // namespace Ubiquitty::Protocol::Sync
