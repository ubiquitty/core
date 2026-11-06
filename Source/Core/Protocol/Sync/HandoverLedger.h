// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/CheckpointLedger.h"
#include "Core/Protocol/Sync/HandoverGround.h"

#include <bit>

// A handover needs host state that a screen replica never receives.
// These are preservation requirements, not a serialization implementation. Carried
// means preserve the portable meaning, never copy owning pointers, callbacks, or
// object bytes. Compound owners need a corresponding field-wise codec before
// they can be transferred. No entry authorizes dropping accepted program work.
//
// The normal checkpoint/record/image/effect planes and the session/host ledgers
// are additional requirements. In particular this table does not account for
// pending PTY bytes, grants, consent, process/descriptor ownership or connections.

namespace Ubiquitty::Protocol::Sync {

/// What a host snapshot must do with a host-only field.
enum class HandoverCarriage : std::uint8_t {
    /// Preserve its meaning in an explicitly encoded host-state section.
    Carried,
    /// Require the named transient operation to finish; never force completion.
    AtGround,
    /// Install a new host-owned callback or service binding.
    Rewired,
    /// Rebuild a derived cache without changing program-observable state.
    Recomputed,
};

/// One required disposition, with a blocker for any non-transferable transient.
struct HandoverLedgerEntry {
    std::string_view name;
    HandoverCarriage carriage;
    HandoverBlocker blocker;
};

/// Requires exactly one valid disposition for every host-only baseline member.
///
/// Carried compound owners can have a transient blocker while their stable
/// state remains required. AtGround entries always name a blocker. Callback
/// rewiring and cache reconstruction are not substitutes for carried policy.
///
/// - Parameters:
///   - baseline: The complete checkpoint classification.
///   - handover: Explicit host-only preservation requirements.
/// - Returns: Whether names, dispositions and blockers are exhaustive and unique.
template<std::size_t baselineCount, std::size_t handoverCount>
consteval bool isHandoverLedgerComplete(const std::array<LedgerEntry, baselineCount>& baseline,
    const std::array<HandoverLedgerEntry, handoverCount>& handover)
{
    std::size_t hostOnly = 0;
    for (const LedgerEntry& entry : baseline) {
        if (entry.carriage != Carriage::HostOnly)
            continue;
        ++hostOnly;
        std::size_t matches = 0;
        for (const HandoverLedgerEntry& requirement : handover)
            matches += requirement.name == entry.name;
        if (matches != 1)
            return false;
    }
    if (hostOnly != handoverCount)
        return false;
    for (const HandoverLedgerEntry& requirement : handover) {
        if (requirement.carriage != HandoverCarriage::Carried && requirement.carriage != HandoverCarriage::AtGround
            && requirement.carriage != HandoverCarriage::Rewired && requirement.carriage != HandoverCarriage::Recomputed)
            return false;
        const auto blocker = static_cast<std::uint32_t>(requirement.blocker);
        if (blocker && (!std::has_single_bit(blocker) || blocker > static_cast<std::uint32_t>(HandoverBlocker::ErasureQualification)))
            return false;
        if (requirement.carriage == HandoverCarriage::AtGround && !blocker)
            return false;
        if ((requirement.carriage == HandoverCarriage::Rewired || requirement.carriage == HandoverCarriage::Recomputed) && blocker)
            return false;
    }
    return true;
}

/// The required disposition of every host-only Terminal member.
///
/// Security configuration, pending program replies, history, identity allocators
/// and effect counters are carried, not reset to defaults. Opaque external file
/// sinks block because Terminal cannot safely reconstruct or transfer them.
/// Integration and image owners retain stable state but block unfinished work.
inline constexpr auto terminalHandoverLedger = std::to_array<HandoverLedgerEntry>({
    { "lineStore", HandoverCarriage::Carried, HandoverBlocker::None },
    { "erasureMode", HandoverCarriage::AtGround, HandoverBlocker::ErasureQualification },
    { "renditionCombination", HandoverCarriage::AtGround, HandoverBlocker::ErasureQualification },
    { "qualifiedAreas", HandoverCarriage::AtGround, HandoverBlocker::ErasureQualification },
    { "alignedTabTerminal", HandoverCarriage::AtGround, HandoverBlocker::DeferredScreenMutation },
    { "alignedTabOriginalRow", HandoverCarriage::AtGround, HandoverBlocker::DeferredScreenMutation },
    { "alignedTabRow", HandoverCarriage::AtGround, HandoverBlocker::DeferredScreenMutation },
    { "alignedTabStop", HandoverCarriage::AtGround, HandoverBlocker::DeferredScreenMutation },
    { "isAlignedTabActive", HandoverCarriage::AtGround, HandoverBlocker::DeferredScreenMutation },
    { "didAlignedTabOverflow", HandoverCarriage::AtGround, HandoverBlocker::DeferredScreenMutation },
    { "historyEpochBase", HandoverCarriage::Carried, HandoverBlocker::None },
    { "lineIdentity", HandoverCarriage::Recomputed, HandoverBlocker::None },
    { "legacyState", HandoverCarriage::AtGround, HandoverBlocker::LegacySequence },
    { "legacyAddrRow", HandoverCarriage::AtGround, HandoverBlocker::LegacySequence },
    { "triggerWrapEvaluatedRow", HandoverCarriage::Carried, HandoverBlocker::None },
    { "marginBellArmed", HandoverCarriage::Carried, HandoverBlocker::None },
    { "isSixelDCS", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "dcsHeaderDone", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "dcsTruncated", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "dcsIntermediate", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "dcsFinal", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "dcsTmuxPassthroughDepth", HandoverCarriage::AtGround, HandoverBlocker::DeviceControlString },
    { "dcsParameters", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "dcsParamCount", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "didDCSParametersOverflow", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "scrollLaneRun", HandoverCarriage::AtGround, HandoverBlocker::DeferredScreenMutation },
    { "isScrollLaneEnabled", HandoverCarriage::Carried, HandoverBlocker::None },
    { "bandScrollRun", HandoverCarriage::AtGround, HandoverBlocker::DeferredScreenMutation },
    { "isBandScrollLaneEnabled", HandoverCarriage::Carried, HandoverBlocker::None },
    { "pendingHistoryRewrapColumns", HandoverCarriage::AtGround, HandoverBlocker::DeferredScreenMutation },
    { "pendingHistoryRewrapAnchor", HandoverCarriage::AtGround, HandoverBlocker::DeferredScreenMutation },
    { "ambiguousWidthAnnounced", HandoverCarriage::Carried, HandoverBlocker::None },
    { "syncOutputCloseCount", HandoverCarriage::Carried, HandoverBlocker::None },
    { "cursorHideCount", HandoverCarriage::Carried, HandoverBlocker::None },
    { "asyncDecodes", HandoverCarriage::AtGround, HandoverBlocker::ImageDecodePending },
    { "outputBuffer", HandoverCarriage::Carried, HandoverBlocker::None },
    { "outputLength", HandoverCarriage::Carried, HandoverBlocker::None },
    { "bulkOutput", HandoverCarriage::Carried, HandoverBlocker::None },
    { "answerback", HandoverCarriage::Carried, HandoverBlocker::None },
    { "isAnswerbackConcealed", HandoverCarriage::Carried, HandoverBlocker::None },
    { "lineLimit", HandoverCarriage::Carried, HandoverBlocker::None },
    { "implicitMovementDirection", HandoverCarriage::Carried, HandoverBlocker::None },
    { "stringStartColumn", HandoverCarriage::Carried, HandoverBlocker::None },
    { "printSinkFile", HandoverCarriage::AtGround, HandoverBlocker::ExternalSinkOpen },
    { "printerControlPrefix", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "printerControlPrefixLength", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "sessionLogFile", HandoverCarriage::AtGround, HandoverBlocker::ExternalSinkOpen },
    { "sessionLogCaptureEnabled", HandoverCarriage::Carried, HandoverBlocker::None },
    { "replyAuthority", HandoverCarriage::Carried, HandoverBlocker::None },
    { "allowTitleOperations", HandoverCarriage::Carried, HandoverBlocker::None },
    { "allowFontOperations", HandoverCarriage::Carried, HandoverBlocker::None },
    { "allowColorOperations", HandoverCarriage::Carried, HandoverBlocker::None },
    { "allowWindowOperations", HandoverCarriage::Carried, HandoverBlocker::None },
    { "allowTerminalCapabilityOperations", HandoverCarriage::Carried, HandoverBlocker::None },
    { "allowMouseOperations", HandoverCarriage::Carried, HandoverBlocker::None },
    { "allowPasteControls", HandoverCarriage::Carried, HandoverBlocker::None },
    { "sequenceGate", HandoverCarriage::Carried, HandoverBlocker::None },
    { "passwordDetect", HandoverCarriage::Carried, HandoverBlocker::None },
    { "bellMode", HandoverCarriage::Carried, HandoverBlocker::None },
    { "drawsSizedText", HandoverCarriage::Carried, HandoverBlocker::None },
    { "drawsProportionalText", HandoverCarriage::Carried, HandoverBlocker::None },
    { "windowClient", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "clipboardClient", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "mirrorClient", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "effectRoute", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "tickClock", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "shellClient", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "dragDropClient", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "dragDropSession", HandoverCarriage::Carried, HandoverBlocker::NativeDrag },
    { "diagnosticsClient", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "fileReadClient", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "textMeasureClient", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "graphicCancellation", HandoverCarriage::Carried, HandoverBlocker::None },
    { "typography", HandoverCarriage::Carried, HandoverBlocker::None },
    { "richTextMeasurements", HandoverCarriage::AtGround, HandoverBlocker::RichTextMeasurement },
    { "richTextMeasurementToken", HandoverCarriage::Carried, HandoverBlocker::None },
    { "isPasswordPromptAsserted", HandoverCarriage::Carried, HandoverBlocker::None },
    { "triggerClient", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "capabilityPolicy", HandoverCarriage::Rewired, HandoverBlocker::None },
    { "currentCapabilityOrigin", HandoverCarriage::Carried, HandoverBlocker::None },
    { "isOSC52Allowed", HandoverCarriage::Carried, HandoverBlocker::None },
    { "osc52ClipboardReadPolicy", HandoverCarriage::Carried, HandoverBlocker::None },
    { "osc52ClipboardWritePolicy", HandoverCarriage::Carried, HandoverBlocker::None },
    { "scrollbackLimit", HandoverCarriage::Carried, HandoverBlocker::None },
    { "triggersEngine", HandoverCarriage::Carried, HandoverBlocker::None },
    { "triggerHighlights", HandoverCarriage::Carried, HandoverBlocker::None },
    { "triggerHighlightCount", HandoverCarriage::Carried, HandoverBlocker::None },
    { "triggerHighlightHead", HandoverCarriage::Carried, HandoverBlocker::None },
    { "integration", HandoverCarriage::Carried, HandoverBlocker::IntegrationActive },
    { "scrollSearchState", HandoverCarriage::Recomputed, HandoverBlocker::None },
    { "windowFocused", HandoverCarriage::Carried, HandoverBlocker::None },
    { "locatorRow", HandoverCarriage::Carried, HandoverBlocker::None },
    { "locatorColumn", HandoverCarriage::Carried, HandoverBlocker::None },
    { "locatorButtons", HandoverCarriage::Carried, HandoverBlocker::None },
    { "drawablePixelWidth", HandoverCarriage::Carried, HandoverBlocker::None },
    { "drawablePixelHeight", HandoverCarriage::Carried, HandoverBlocker::None },
    { "lastActivityTick", HandoverCarriage::Carried, HandoverBlocker::None },
    { "hasDisplayActivityTick", HandoverCarriage::Carried, HandoverBlocker::None },
    { "displayActivityTick", HandoverCarriage::Carried, HandoverBlocker::None },
    { "cursorHoldFrames", HandoverCarriage::Carried, HandoverBlocker::None },
    { "cursorBlinkPeriodMilliseconds", HandoverCarriage::Carried, HandoverBlocker::None },
    { "cursorBlinkPref", HandoverCarriage::Carried, HandoverBlocker::None },
    { "cursorShapePref", HandoverCarriage::Carried, HandoverBlocker::None },
    { "cursorMotionModePref", HandoverCarriage::Carried, HandoverBlocker::None },
    { "cursorMotionDurationMilliseconds", HandoverCarriage::Carried, HandoverBlocker::None },
    { "tekColorPreference", HandoverCarriage::Recomputed, HandoverBlocker::None },
    { "cellPixelWidth", HandoverCarriage::Carried, HandoverBlocker::None },
    { "cellPixelHeight", HandoverCarriage::Carried, HandoverBlocker::None },
    { "textAreaPixelWidth", HandoverCarriage::Carried, HandoverBlocker::None },
    { "textAreaPixelHeight", HandoverCarriage::Carried, HandoverBlocker::None },
    { "screenPixelWidth", HandoverCarriage::Carried, HandoverBlocker::None },
    { "screenPixelHeight", HandoverCarriage::Carried, HandoverBlocker::None },
    { "windowIconified", HandoverCarriage::Carried, HandoverBlocker::None },
    { "winPosX", HandoverCarriage::Carried, HandoverBlocker::None },
    { "winPosY", HandoverCarriage::Carried, HandoverBlocker::None },
    { "font", HandoverCarriage::Carried, HandoverBlocker::None },
    { "fontRequests", HandoverCarriage::Carried, HandoverBlocker::FontOperationPending },
    { "fontRequestChanged", HandoverCarriage::Carried, HandoverBlocker::None },
    { "progressGeneration", HandoverCarriage::Carried, HandoverBlocker::None },
    { "termpropProgressHint", HandoverCarriage::Carried, HandoverBlocker::None },
    { "termpropProgressValue", HandoverCarriage::Carried, HandoverBlocker::None },
    { "tabStatusGeneration", HandoverCarriage::Carried, HandoverBlocker::None },
    { "applicationIdentifier", HandoverCarriage::Carried, HandoverBlocker::None },
    { "attentionGeneration", HandoverCarriage::Carried, HandoverBlocker::None },
    { "kittyNotifications", HandoverCarriage::AtGround, HandoverBlocker::NotificationOpen },
    { "currentDirectoryChanged", HandoverCarriage::Carried, HandoverBlocker::None },
    { "bellPending", HandoverCarriage::Carried, HandoverBlocker::None },
    { "lastBellTick", HandoverCarriage::Carried, HandoverBlocker::None },
    { "remoteHost", HandoverCarriage::Carried, HandoverBlocker::None },
    { "lastCommandExitCode", HandoverCarriage::Carried, HandoverBlocker::None },
    { "commandCompletedGeneration", HandoverCarriage::Carried, HandoverBlocker::None },
    { "commandExitCodeRing", HandoverCarriage::Carried, HandoverBlocker::None },
    { "commandStartedGeneration", HandoverCarriage::Carried, HandoverBlocker::None },
    { "commandStartedAtCompletionRing", HandoverCarriage::Carried, HandoverBlocker::None },
    { "hasCommandStartTick", HandoverCarriage::Carried, HandoverBlocker::None },
    { "commandStartTick", HandoverCarriage::Carried, HandoverBlocker::None },
    { "commandDurationTicksRing", HandoverCarriage::Carried, HandoverBlocker::None },
    { "pendingPromptRow", HandoverCarriage::Carried, HandoverBlocker::None },
    { "shellCommands", HandoverCarriage::Carried, HandoverBlocker::None },
    { "shellCommandCount", HandoverCarriage::Carried, HandoverBlocker::None },
    { "isReadingCommand", HandoverCarriage::Carried, HandoverBlocker::None },
    { "hasMarkedPrompt", HandoverCarriage::Carried, HandoverBlocker::None },
    { "commandLine", HandoverCarriage::Carried, HandoverBlocker::None },
    { "commandLineGeneration", HandoverCarriage::Carried, HandoverBlocker::None },
    { "currentDirectory", HandoverCarriage::Carried, HandoverBlocker::None },
    { "reportedDirectory", HandoverCarriage::Carried, HandoverBlocker::None },
    { "reportedHost", HandoverCarriage::Carried, HandoverBlocker::None },
    { "reportingProcessGroup", HandoverCarriage::Carried, HandoverBlocker::None },
    { "reportedHostProcessGroup", HandoverCarriage::Carried, HandoverBlocker::None },
    { "sampledDirectory", HandoverCarriage::Carried, HandoverBlocker::None },
    { "titleOwnerProcessGroup", HandoverCarriage::Carried, HandoverBlocker::None },
    { "foregroundProcessGroup", HandoverCarriage::Carried, HandoverBlocker::None },
    { "sampleBank", HandoverCarriage::Carried, HandoverBlocker::None },
    { "pendingSound", HandoverCarriage::Carried, HandoverBlocker::None },
    { "normalTextColors", HandoverCarriage::Carried, HandoverBlocker::None },
    { "windowFrameColors", HandoverCarriage::Carried, HandoverBlocker::None },
    { "alternateTextColors", HandoverCarriage::Carried, HandoverBlocker::None },
    { "isOSC52ClipboardReadInFlight", HandoverCarriage::AtGround, HandoverBlocker::ClipboardReadPending },
    { "isClipboardReadRequestBellTerminated", HandoverCarriage::AtGround, HandoverBlocker::ClipboardReadPending },
    { "osc52Chunk", HandoverCarriage::AtGround, HandoverBlocker::ClipboardChunkOpen },
    { "osc52ChunkActive", HandoverCarriage::AtGround, HandoverBlocker::ClipboardChunkOpen },
    { "osc52ChunkOverflow", HandoverCarriage::AtGround, HandoverBlocker::ClipboardChunkOpen },
    { "osc52ChunkSet", HandoverCarriage::AtGround, HandoverBlocker::ClipboardChunkOpen },
    { "isPendingClipboardReadExtended", HandoverCarriage::AtGround, HandoverBlocker::ClipboardReadPending },
    { "extendedClipboardReadID", HandoverCarriage::AtGround, HandoverBlocker::ClipboardReadPending },
    { "isExtendedClipboardReadPrimary", HandoverCarriage::AtGround, HandoverBlocker::ClipboardReadPending },
    { "extendedClipboardWriteItems", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "extendedClipboardWriteItemMimes", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "extendedClipboardWriteItemCount", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "extendedClipboardWriteTotalBytes", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "isExtendedClipboardWriteActive", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "isExtendedClipboardWritePrimary", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "didExtendedClipboardWriteOverflow", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "extendedClipboardWriteID", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "extendedClipboardWriteMime", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "extendedClipboardBase64Tail", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "extendedClipboardBase64TailLength", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "isExtendedClipboardBase64Complete", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "extendedClipboardSessionAliases", HandoverCarriage::AtGround, HandoverBlocker::ExtendedClipboardOpen },
    { "extendedClipboardServedAliases", HandoverCarriage::Carried, HandoverBlocker::None },
    { "extendedClipboardReadMimes", HandoverCarriage::AtGround, HandoverBlocker::ClipboardReadPending },
    { "isExtendedClipboardReadListRequested", HandoverCarriage::AtGround, HandoverBlocker::ClipboardReadPending },
    { "linkNextID", HandoverCarriage::Carried, HandoverBlocker::None },
    { "linkFreeIDs", HandoverCarriage::Carried, HandoverBlocker::None },
    // Retry cadence decides whether the next program operation is admitted.
    { "linkReclaimBackoffMints", HandoverCarriage::Carried, HandoverBlocker::None },
    { "runTextReclaimBackoffInterns", HandoverCarriage::Carried, HandoverBlocker::None },
    { "textSizeReclaimBackoffInterns", HandoverCarriage::Carried, HandoverBlocker::None },
    { "images", HandoverCarriage::Carried, HandoverBlocker::ImageTransferOpen },
    { "softFont", HandoverCarriage::Carried, HandoverBlocker::None },
    { "tek", HandoverCarriage::Carried, HandoverBlocker::None },
});

/// Parser host-state requirements that the terminal's checkpoint entry hides.
///
/// A grapheme cluster can stay open at parser ground and must survive: finishing
/// or discarding it changes the next combining character. Input encoding policy
/// and VT52 versus ANSI ground mode are also carried, never reset to defaults.
/// Stale accumulator bytes are irrelevant only when their blocker is absent.
inline constexpr auto parserHandoverLedger = std::to_array<HandoverLedgerEntry>({
    { "invalidUTF8Policy", HandoverCarriage::Carried, HandoverBlocker::None },
    { "parserInputIsUTF8", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterActive", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterRow", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterColumn", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterScreen", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterLast", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterState", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterAtX", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterAtY", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterAtWrap", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterCodePoints", HandoverCarriage::Carried, HandoverBlocker::None },
    { "clusterCodepointLength", HandoverCarriage::Carried, HandoverBlocker::None },
    { "parseState", HandoverCarriage::Carried, HandoverBlocker::ParserMidSequence },
    { "inputEncoding", HandoverCarriage::Carried, HandoverBlocker::IncompleteCharacter },
    { "parameterCount", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "privatePrefix", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "intermediate", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "parameters", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "parameterSeparatorWasColon", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "parameterWasEmpty", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "didParametersOverflow", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "osc", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "isOSCRequestBellTerminated", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "characterSetSelect", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "is96CharacterSet", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "characterSetIntermediate", HandoverCarriage::AtGround, HandoverBlocker::ParserMidSequence },
    { "utf8", HandoverCarriage::AtGround, HandoverBlocker::IncompleteCharacter },
    { "utf8LeadByte", HandoverCarriage::AtGround, HandoverBlocker::IncompleteCharacter },
});

static_assert(isHandoverLedgerComplete(terminalLedger, terminalHandoverLedger));
static_assert(isHandoverLedgerComplete(entriesOf(parserLedger), parserHandoverLedger));

} // namespace Ubiquitty::Protocol::Sync
