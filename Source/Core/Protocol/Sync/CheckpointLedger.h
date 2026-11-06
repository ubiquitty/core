// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/CheckpointLedger.h — what becomes of every member of `Terminal` when a
// checkpoint is taken (Docs/sync.md §7.4).
//
// A checkpoint is complete when a client that applies it holds every piece of
// state a frame can show or an input can depend on. Completeness is not a
// property a review can hold for three hundred members, so every member is
// named here with its carriage, and the build refuses a `Terminal` this table
// does not account for:
//
// - The entries, sorted by offset, must tile the structure: each begins where
//   the previous one ends, rounded up to its own alignment, and the last ends
//   at the structure's size rounded the same way. A member added anywhere that
//   moves a later one, or grows the structure, breaks the tiling.
// - A member small enough to land in an existing padding hole moves nothing,
//   so the tiling cannot see it. `test_sync_checkpoint_ledger` enumerates
//   the members by name (`__builtin_dump_struct`) and fails on any name not
//   listed here.
//
// The nested structures whose members a checkpoint carries one by one —
// `Terminal::Modes` and `Screen::State` — have ledgers of their own, held to
// the same tiling.
//

#include "Core/Screen/Screen.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Ubiquitty::Protocol::Sync {

/// What a checkpoint does with one member.
enum class Carriage : std::uint8_t {
    /// The checkpoint carries it, and the state digest covers it, so a client
    /// that drifts from it is caught on the next update.
    Digested,
    /// The checkpoint carries it; no update re-checks it.
    Checkpointed,
    /// The client computes it from what the checkpoint carries.
    Derived,
    /// A record frame on the pane's channel carries it whole into the
    /// replica (`PaneFacts`, `StatusRecords`; Records.h), in every view
    /// state, and the oracle compares it. No checkpoint carries it.
    Recorded,
    /// It never leaves the host: parser and transport state, the host's
    /// policies and frontend facts, and state an update plane other than the
    /// screen's carries.
    HostOnly,
};

/// One member of a ledgered structure.
struct LedgerEntry {
    std::size_t offset;
    std::size_t size;
    std::size_t alignment;
    Carriage carriage;
    std::string_view name;
};

/// What a parser member holds between two bytes (Docs/sync.md §11).
enum class ParseRole : std::uint8_t {
    /// A sequence in progress: a CSI's parameters, an OSC or DCS payload, a
    /// partial UTF-8 sequence, an open grapheme cluster. A frame may be taken
    /// mid-sequence (`Flood`, `Forced`), so no checkpoint may carry it.
    Pending,
    /// State a parser at ground still reads, which a checkpoint taken at
    /// ground must carry for a parser continuing from it.
    Ground,
    /// How the host decodes its input: its policy, not the stream's state.
    Policy,
};

/// One member of `Terminal::Parser`, with what it holds.
struct ParserLedgerEntry {
    LedgerEntry entry;
    ParseRole role;
};

/// Whether entries tile a structure of `size` bytes aligned to `alignment`.
///
/// - Parameters:
///   - entries: Every member, in any order.
///   - size: `sizeof` the structure.
///   - alignment: `alignof` the structure.
/// - Returns: Whether the members, laid out by offset, leave no gap wider
///   than the next member's alignment padding.
template<std::size_t count>
consteval bool tiles(std::array<LedgerEntry, count> entries, std::size_t size, std::size_t alignment)
{
    std::ranges::sort(entries, {}, &LedgerEntry::offset);
    const auto alignUp = [](std::size_t value, std::size_t to) { return (value + to - 1) / to * to; };
    std::size_t end = 0;
    for (const LedgerEntry& entry : entries) {
        if (entry.offset != alignUp(end, entry.alignment))
            return false;
        end = entry.offset + entry.size;
    }
    return alignUp(end, alignment) == size;
}

// offsetof over a structure that is not standard-layout is conditionally
// supported; both compilers this tree builds with support it for a type with
// no virtual base, which `Terminal` asserts it is not polymorphic, let alone.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

// The only spelling offsetof and the member's declared type share is the
// member's name, which only the preprocessor can reuse in both.
#define UBIQUITTY_SYNC_LEDGER(Type, member, carriage) \
    LedgerEntry { offsetof(Type, member), sizeof(Type::member), alignof(decltype(Type::member)), Carriage::carriage, #member }

/// The carriage and role of every `Terminal::Parser` member.
inline constexpr std::array parserLedger {
#define PARSER(member, carriage, role) \
    ParserLedgerEntry { UBIQUITTY_SYNC_LEDGER(Ubiquitty::Terminal::Parser, member, carriage), ParseRole::role }
    PARSER(parseState, HostOnly, Pending),
    PARSER(parameterCount, HostOnly, Pending),
    PARSER(privatePrefix, HostOnly, Pending),
    PARSER(intermediate, HostOnly, Pending),
    PARSER(parameters, HostOnly, Pending),
    PARSER(parameterSeparatorWasColon, HostOnly, Pending),
    PARSER(parameterWasEmpty, HostOnly, Pending),
    PARSER(didParametersOverflow, HostOnly, Pending),
    // What CSI b repeats: read at ground, so the text-state block carries it.
    PARSER(precedingItem, Checkpointed, Ground),
    PARSER(osc, HostOnly, Pending),
    PARSER(isOSCRequestBellTerminated, HostOnly, Pending),
    PARSER(characterSetSelect, HostOnly, Pending),
    PARSER(is96CharacterSet, HostOnly, Pending),
    PARSER(characterSetIntermediate, HostOnly, Pending),
    PARSER(utf8, HostOnly, Pending),
    PARSER(invalidUTF8Policy, HostOnly, Policy),
    PARSER(utf8LeadByte, HostOnly, Pending),
    PARSER(inputEncoding, HostOnly, Policy),
    PARSER(parserInputIsUTF8, HostOnly, Policy),
    // An open grapheme cluster is segmentation in progress, as a partial
    // UTF-8 sequence is.
    PARSER(clusterActive, HostOnly, Pending),
    PARSER(clusterRow, HostOnly, Pending),
    PARSER(clusterColumn, HostOnly, Pending),
    PARSER(clusterScreen, HostOnly, Pending),
    PARSER(clusterLast, HostOnly, Pending),
    PARSER(clusterState, HostOnly, Pending),
    PARSER(clusterAtX, HostOnly, Pending),
    PARSER(clusterAtY, HostOnly, Pending),
    PARSER(clusterAtWrap, HostOnly, Pending),
    PARSER(clusterCodePoints, HostOnly, Pending),
    PARSER(clusterCodepointLength, HostOnly, Pending),
#undef PARSER
};

/// The plain entries of a parser ledger, for the tiling.
///
/// - Parameter ledger: The parser ledger.
/// - Returns: Its entries without their roles.
template<std::size_t count>
consteval std::array<LedgerEntry, count> entriesOf(const std::array<ParserLedgerEntry, count>& ledger)
{
    std::array<LedgerEntry, count> entries {};
    for (std::size_t index = 0; index < count; ++index)
        entries[index] = ledger[index].entry;
    return entries;
}

/// Whether every pending member never leaves the host and every ground
/// member is carried: the rule that lets a parser continue from a checkpoint
/// taken at ground, and from no other.
///
/// - Parameter ledger: The parser ledger.
/// - Returns: Whether the rule holds.
template<std::size_t count>
consteval bool isParseRoleRuleHeld(const std::array<ParserLedgerEntry, count>& ledger)
{
    for (const ParserLedgerEntry& member : ledger) {
        if (member.role == ParseRole::Pending && member.entry.carriage != Carriage::HostOnly)
            return false;
        if (member.role == ParseRole::Ground && (member.entry.carriage == Carriage::HostOnly || member.entry.carriage == Carriage::Derived))
            return false;
    }
    return true;
}

/// The carriage of every `Terminal::Modes` member: the checkpoint's modes
/// block carries all of them but the three resource modes that stay on the
/// host.
inline constexpr std::array terminalModesLedger {
#define MODE(member) UBIQUITTY_SYNC_LEDGER(Ubiquitty::Terminal::Modes, member, Digested)
    MODE(isCursorVisible),
    MODE(autowrap),
    MODE(appCursorKeys),
    MODE(appKeypad),
    MODE(bracketedPaste),
    MODE(shouldReportPasteEvents),
    MODE(shouldSaveCRT),
    MODE(shouldReportKeyPositions),
    MODE(shouldMoveReadlinePointOnClick),
    MODE(shouldMoveReadlinePointBeforePaste),
    MODE(shouldDeleteReadlineSelection),
    MODE(shouldQuotePasteCharacters),
    MODE(shouldPreservePasteNewlines),
    MODE(focusReporting),
    MODE(allowAlternateScreen),
    MODE(scrollOnOutput),
    MODE(scrollOnKeypress),
    MODE(colorThemeReporting),
    MODE(originMode),
    MODE(reverseWrapExtended),
    MODE(isHorizontalCursorCoupled),
    MODE(isVerticalCursorUncoupled),
    MODE(isPageCursorUncoupled),
    MODE(keyboardLocked),
    MODE(lnm),
    MODE(srm),
    MODE(insertMode),
    MODE(reverseVideo),
    MODE(syncOutput),
    MODE(vt52Mode),
    MODE(vt52Graphics),
    MODE(sixelDisplayMode),
    MODE(clearScreenOnColumnChange),
    MODE(columns132),
    MODE(sace),
    MODE(leftRightMarginMode),
    MODE(syncOutputTouchTicks),
    MODE(mouseX10),
    MODE(mouseButtonTracking),
    MODE(mouseSGR),
    MODE(mouseCellMotion),
    MODE(mouseAnyEvent),
    MODE(mouseUTF8Coordinates),
    MODE(mouseURXVT),
    MODE(mouseSGRPixels),
    MODE(mouseRelativePointerLock),
    MODE(mouseHilite),
    MODE(hiliteRegionValid),
    MODE(hiliteStartX),
    MODE(hiliteStartY),
    MODE(hiliteFirstRow),
    MODE(hiliteLastRow),
    MODE(hiliteAwaiting),
    MODE(hiliteDeadlineTick),
    MODE(alternateScrollMode),
    MODE(reverseWrap),
    MODE(smoothScroll),
    MODE(autoRepeat),
    MODE(nrcMode),
    MODE(backspaceKeySendsBackspace),
    MODE(metaSendsEscape),
    MODE(alternateSendsEscape),
    MODE(eightBitInput),
    MODE(deleteKeySendsDelete),
    MODE(shouldHonorNumLock),
    MODE(keyboardType),
    MODE(termcapProfile),
    MODE(graphemeClustering),
    MODE(fontShifting),
    MODE(keepClipboard),
    MODE(inBandResize),
    MODE(fullscreenContent),
    MODE(allow80To132),
    MODE(shouldEraseToDefaultColors),
    MODE(hasPrivateSixelColorRegisters),
    MODE(shouldLeaveCursorRightOfSixel),
    MODE(shouldRewrapOnResize),
    MODE(shouldReportAmbiguousWidth),
    MODE(allowsInputMethodEditor),
#undef MODE
    // xterm's resource modes 1014, 1040 and 1041 change nothing a client
    // draws: the host answers DECRQM for them, so they stay on the host.
    UBIQUITTY_SYNC_LEDGER(Ubiquitty::Terminal::Modes, shouldScrollFast, HostOnly),
    UBIQUITTY_SYNC_LEDGER(Ubiquitty::Terminal::Modes, shouldKeepSelection, HostOnly),
    UBIQUITTY_SYNC_LEDGER(Ubiquitty::Terminal::Modes, shouldSelectToClipboard, HostOnly),
    // DECGEPM and DECKBUM are recorded for DECRQM and change nothing a client
    // draws, so they stay on the host with the resource modes.
    UBIQUITTY_SYNC_LEDGER(Ubiquitty::Terminal::Modes, shouldExpandGraphicsPrint, HostOnly),
    UBIQUITTY_SYNC_LEDGER(Ubiquitty::Terminal::Modes, shouldUseDataProcessingKeys, HostOnly),
    // The user-preference supplemental set is read back only by the host's
    // DECRQUPSS reply.
    UBIQUITTY_SYNC_LEDGER(Ubiquitty::Terminal::Modes, userPreferredSupplementalSet, HostOnly),
};

/// The carriage of every `Screen::State` member. The active screen's cursor
/// is the digested cursor block; the rest of each screen's state rides the
/// checkpoint's screen-state section. The three views read the grid's row
/// metadata, which travels in each row record.
inline constexpr std::array screenStateLedger {
#define SCREEN(member, carriage) UBIQUITTY_SYNC_LEDGER(Ubiquitty::Screen::State, member, carriage)
    SCREEN(cursorX, Digested),
    SCREEN(cursorY, Digested),
    SCREEN(scrollTop, Digested),
    SCREEN(scrollBottom, Digested),
    SCREEN(scrollLeft, Digested),
    SCREEN(scrollRight, Digested),
    SCREEN(rememberedMargins, Digested),
    SCREEN(rememberedMarginCount, Digested),
    SCREEN(pen, Digested),
    SCREEN(savedX, Digested),
    SCREEN(savedY, Digested),
    SCREEN(savedPen, Digested),
    SCREEN(savedCharacterSetActive, Digested),
    SCREEN(savedCharacterSetGR, Digested),
    SCREEN(savedCharacterSetG0, Digested),
    SCREEN(savedCharacterSetG1, Digested),
    SCREEN(savedCharacterSetG2, Digested),
    SCREEN(savedCharacterSetG3, Digested),
    // A pending single shift is restored by DECRC on the host; no client
    // replays DECRC, so it stays on the host.
    SCREEN(savedSingleShift, HostOnly),
    SCREEN(savedAutowrap, Digested),
    SCREEN(savedOriginMode, Digested),
    SCREEN(savedWrapPending, Digested),
    SCREEN(savedScrollTop, Digested),
    SCREEN(savedScrollBottom, Digested),
    SCREEN(hasSavedCursor, Digested),
    SCREEN(wrapPending, Digested),
    SCREEN(lineAttribute, Derived),
    SCREEN(lineWrapped, Derived),
    SCREEN(mark, Derived),
#undef SCREEN
};

/// The carriage of every `Terminal` member.
inline constexpr std::array terminalLedger {
#define MEMBER(member, carriage) UBIQUITTY_SYNC_LEDGER(Ubiquitty::Terminal, member, carriage)
    // The screen plane proper: both screens' rows and cursors, the geometry,
    // the modes and the palette. The grid's rows travel as row records.
    MEMBER(screens, Digested),
    MEMBER(columns, Digested),
    MEMBER(rows, Digested),
    MEMBER(grid, Digested),
    MEMBER(modes, Digested),
    MEMBER(palette, Digested),
    MEMBER(paletteBaseline, Digested),
    MEMBER(savedPalettes, Digested),
    MEMBER(savedPaletteValid, Digested),
    MEMBER(savedPalettePosition, Digested),
    MEMBER(savedPaletteCount, Digested),
    MEMBER(cursorStyle, Digested),

    // Mode state kept beside `modes`, in the same block.
    MEMBER(ambiguousWidth, Digested),
    MEMBER(emulation, Digested),
    MEMBER(conformanceLevel, Digested),
    MEMBER(protectedMode, Digested),
    MEMBER(erasureMode, HostOnly),
    MEMBER(renditionCombination, HostOnly),
    MEMBER(qualifiedAreas, HostOnly),
    MEMBER(checksumExtensions, Digested),
    MEMBER(transmit8bitC1, Digested),
    MEMBER(kittyKeyboardFlags, Digested),
    MEMBER(kittyKeyboardStack, Digested),
    MEMBER(kittyKeyboardStackLength, Digested),
    MEMBER(xtermKeyModifiers, Digested),
    MEMBER(xtermKeyFormats, Digested),
    MEMBER(savedPrivateModeNumbers, Digested),
    MEMBER(savedPrivateModeStates, Digested),
    MEMBER(titleModes, Digested),
    MEMBER(savedRenditionMasks, Digested),
    MEMBER(savedRenditionPens, Digested),
    MEMBER(savedRenditionCount, Digested),
    MEMBER(cursorBlinkAtt610, Digested),
    MEMBER(marginBell, Digested),
    MEMBER(marginBellColumn, Digested),
    MEMBER(moreFix, Digested),
    MEMBER(loggingEnabled, Digested),
    MEMBER(toolbarVisible, Digested),
    MEMBER(scrollbarVisible, Digested),
    MEMBER(bellRaisesUrgency, Digested),
    MEMBER(popOnBell, Digested),
    MEMBER(printMode, Digested),
    MEMBER(shouldIgnorePrinterNUL, Digested),
    MEMBER(shouldCopyRightToLeft, Digested),
    MEMBER(shouldWriteRightToLeft, Digested),
    MEMBER(printFormFeed, Digested),
    MEMBER(printExtentFullScreen, Digested),
    MEMBER(autoPrint, Digested),
    MEMBER(locatorEnabled, Digested),
    MEMBER(locatorOneShot, Digested),
    MEMBER(locatorUseRows, Digested),
    MEMBER(locatorReportDown, Digested),
    MEMBER(locatorReportUp, Digested),
    MEMBER(locatorFilterActive, Digested),
    MEMBER(locatorFilterTop, Digested),
    MEMBER(locatorFilterLeft, Digested),
    MEMBER(locatorFilterBottom, Digested),
    MEMBER(locatorFilterRight, Digested),
    MEMBER(screenBlankTimeout, Digested),
    MEMBER(vesaPowerdownTimeout, Digested),
    MEMBER(screenBlanked, Digested),
    MEMBER(isCRTBlanked, Digested),
    MEMBER(keyPositionReportedModifiers, Digested),
    MEMBER(keyPositionDisabledModifiers, Digested),
    MEMBER(hasDisplayActivityTick, HostOnly),
    MEMBER(displayActivityTick, HostOnly),
    MEMBER(passwordPromptActive, Digested),
    // Whose prompt it is decides only what the heuristic may do, and the
    // heuristic runs where the bytes are parsed.
    MEMBER(isPasswordPromptAsserted, HostOnly),
    MEMBER(statusLineType, Digested),
    MEMBER(isStatusNationalReplacementEnabled, Digested),
    MEMBER(statusLineActive, Digested),
    MEMBER(statusCursorX, Digested),
    MEMBER(fullscreenContentDisplay, Digested),
    MEMBER(tekBrightPenOverride, Digested),
    MEMBER(isTekModeActive, Digested),

    // The xterm dynamic colours, in the palette block.
    MEMBER(selectionBackgroundRed, Digested),
    MEMBER(selectionBackgroundGreen, Digested),
    MEMBER(selectionBackgroundBlue, Digested),
    MEMBER(selectionForegroundRed, Digested),
    MEMBER(selectionForegroundGreen, Digested),
    MEMBER(selectionForegroundBlue, Digested),
    MEMBER(selectionBackgroundSet, Digested),
    MEMBER(selectionForegroundSet, Digested),
    MEMBER(cursorTextRed, Digested),
    MEMBER(cursorTextGreen, Digested),
    MEMBER(cursorTextBlue, Digested),
    MEMBER(hasCursorTextColor, Digested),
    MEMBER(mousePointerForegroundRed, Digested),
    MEMBER(mousePointerForegroundGreen, Digested),
    MEMBER(mousePointerForegroundBlue, Digested),
    MEMBER(mousePointerBackgroundRed, Digested),
    MEMBER(mousePointerBackgroundGreen, Digested),
    MEMBER(mousePointerBackgroundBlue, Digested),
    MEMBER(tektronixForegroundRed, Digested),
    MEMBER(tektronixForegroundGreen, Digested),
    MEMBER(tektronixForegroundBlue, Digested),
    MEMBER(tektronixBackgroundRed, Digested),
    MEMBER(tektronixBackgroundGreen, Digested),
    MEMBER(tektronixBackgroundBlue, Digested),
    MEMBER(tektronixCursorRed, Digested),
    MEMBER(tektronixCursorGreen, Digested),
    MEMBER(tektronixCursorBlue, Digested),
    MEMBER(mousePointerForegroundSet, Digested),
    MEMBER(mousePointerBackgroundSet, Digested),
    MEMBER(tektronixForegroundSet, Digested),
    MEMBER(tektronixBackgroundSet, Digested),
    MEMBER(tektronixCursorSet, Digested),
    MEMBER(tekColorPreference, HostOnly),

    // Character and text state: what the next printable lands as, and the
    // titles a pop restores.
    MEMBER(characterSetG, Digested),
    MEMBER(characterSetActive, Digested),
    MEMBER(characterSetGR, Digested),
    MEMBER(singleShift, Digested),
    MEMBER(currentLinkID, Digested),
    MEMBER(tabStops, Digested),
    MEMBER(tabAlignments, Digested),
    MEMBER(tabAlignmentTargets, Digested),
    MEMBER(alignedTabTerminal, HostOnly),
    MEMBER(alignedTabOriginalRow, HostOnly),
    MEMBER(alignedTabRow, HostOnly),
    MEMBER(alignedTabStop, HostOnly),
    MEMBER(isAlignedTabActive, HostOnly),
    MEMBER(didAlignedTabOverflow, HostOnly),
    MEMBER(lineTabStops, Digested),
    MEMBER(lineHome, Digested),
    // The line limit, the implicit movement direction and the start of the
    // string QUAD positions steer CR, NEL and QUAD on the host. No client
    // replays those controls, so they stay on the host.
    MEMBER(lineLimit, HostOnly),
    MEMBER(implicitMovementDirection, HostOnly),
    MEMBER(stringStartColumn, HostOnly),
    MEMBER(titleUTF8, Digested),
    MEMBER(iconNameUTF8, Digested),
    MEMBER(titleUTF8Stack, Digested),
    MEMBER(iconNameUTF8Stack, Digested),
    MEMBER(titleStackLength, Digested),
    MEMBER(titleStackValid, Digested),
    MEMBER(iconNameStackValid, Digested),
    MEMBER(udk, Digested),
    MEMBER(statusRow, Digested),
    MEMBER(sixelColorRegisters, Digested),
    MEMBER(mousePointerShapeStack, Digested),
    MEMBER(mousePointerShapeStackLength, Digested),
    MEMBER(alternateMousePointerShapeStack, Digested),
    MEMBER(alternateMousePointerShapeStackLength, Digested),
    MEMBER(shouldEndSemanticRoleAtLineBreak, Digested),
    MEMBER(shellCommands, HostOnly),
    MEMBER(shellCommandCount, HostOnly),
    MEMBER(textSizeTable, Digested),
    MEMBER(richTextBlocks, Digested),

    // The dictionaries cells name, and the history's epoch: its lines travel on
    // the history plane, and the checkpoint names the epoch and the head.
    MEMBER(linkTable, Checkpointed),
    MEMBER(graphicCancellation, HostOnly),
    MEMBER(clusterPool, Checkpointed),
    MEMBER(runTextStore, Checkpointed),
    MEMBER(lineStore, HostOnly),
    MEMBER(historyEpochBase, HostOnly),
    MEMBER(historyEpoch, Checkpointed),
    // Positions name lines on the side that captured them: a client pins its
    // own replica's lines, and the host's names never leave it.
    MEMBER(lineIdentity, HostOnly),

    // Recomputed by the client from what it applied.
    MEMBER(linkCount, Derived),
    MEMBER(linkHead, Derived),
    MEMBER(scrollbackCount, Derived),
    MEMBER(richTextLayouts, Derived),
    MEMBER(scrollbackCapacity, Derived),
    MEMBER(modelEpoch, Derived),
    MEMBER(paletteEpoch, Derived),
    MEMBER(eraseDisplayGeneration, Derived),
    MEMBER(absoluteRowEpoch, Derived),
    MEMBER(sourceAbsoluteRowEpoch, Derived),
    MEMBER(hasSourceAbsoluteRowEpoch, Derived),
    MEMBER(generation, Derived),
    MEMBER(titleChanged, Derived),
    MEMBER(glRunTranslations, Derived),
    MEMBER(glRunTranslationState, Derived),
    MEMBER(glRunTranslationAmbiguousWidth, Derived),

    // Parser state, classified member by member in `parserLedger`: the
    // checkpoint carries its ground state and none of what is pending.
    MEMBER(parser, Checkpointed),
    MEMBER(legacyState, HostOnly),
    MEMBER(legacyAddrRow, HostOnly),
    MEMBER(triggerWrapEvaluatedRow, HostOnly),
    MEMBER(marginBellArmed, HostOnly),
    MEMBER(isSixelDCS, HostOnly),
    MEMBER(dcsHeaderDone, HostOnly),
    MEMBER(dcsTruncated, HostOnly),
    MEMBER(dcsIntermediate, HostOnly),
    MEMBER(dcsFinal, HostOnly),
    MEMBER(dcsTmuxPassthroughDepth, HostOnly),
    MEMBER(dcsParameters, HostOnly),
    MEMBER(dcsParamCount, HostOnly),
    MEMBER(didDCSParametersOverflow, HostOnly),
    MEMBER(scrollLaneRun, HostOnly),
    MEMBER(isScrollLaneEnabled, HostOnly),
    MEMBER(bandScrollRun, HostOnly),
    MEMBER(isBandScrollLaneEnabled, HostOnly),
    MEMBER(pendingHistoryRewrapColumns, HostOnly),
    MEMBER(pendingHistoryRewrapAnchor, HostOnly),
    MEMBER(ambiguousWidthAnnounced, HostOnly),
    MEMBER(syncOutputCloseCount, HostOnly),
    MEMBER(cursorHideCount, HostOnly),
    MEMBER(asyncDecodes, HostOnly),

    // Replies to the program and the program's own streams, which the host
    // writes and the client never sees.
    MEMBER(outputBuffer, HostOnly),
    MEMBER(outputLength, HostOnly),
    MEMBER(bulkOutput, HostOnly),
    MEMBER(answerback, HostOnly),
    MEMBER(isAnswerbackConcealed, HostOnly),
    MEMBER(printSinkFile, HostOnly),
    MEMBER(printerControlPrefix, HostOnly),
    MEMBER(printerControlPrefixLength, HostOnly),
    MEMBER(sessionLogFile, HostOnly),
    MEMBER(sessionLogCaptureEnabled, HostOnly),
    MEMBER(replyAuthority, HostOnly),

    // The host's policies, configuration and installed clients.
    MEMBER(allowTitleOperations, HostOnly),
    MEMBER(allowFontOperations, HostOnly),
    MEMBER(allowColorOperations, HostOnly),
    MEMBER(allowWindowOperations, HostOnly),
    MEMBER(allowTerminalCapabilityOperations, HostOnly),
    MEMBER(allowMouseOperations, HostOnly),
    MEMBER(allowPasteControls, HostOnly),
    MEMBER(sequenceGate, HostOnly),
    MEMBER(passwordDetect, HostOnly),
    MEMBER(bellMode, HostOnly),
    MEMBER(drawsSizedText, HostOnly),
    MEMBER(drawsProportionalText, HostOnly),
    MEMBER(windowClient, HostOnly),
    MEMBER(clipboardClient, HostOnly),
    MEMBER(mirrorClient, HostOnly),
    MEMBER(effectRoute, HostOnly),
    MEMBER(tickClock, HostOnly),
    MEMBER(shellClient, HostOnly),
    MEMBER(dragDropClient, HostOnly),
    MEMBER(dragDropSession, HostOnly),
    MEMBER(diagnosticsClient, HostOnly),
    MEMBER(fileReadClient, HostOnly),
    MEMBER(textMeasureClient, HostOnly),
    MEMBER(typography, HostOnly),
    MEMBER(richTextMeasurements, HostOnly),
    MEMBER(richTextMeasurementToken, HostOnly),
    MEMBER(triggerClient, HostOnly),
    MEMBER(capabilityPolicy, HostOnly),
    MEMBER(currentCapabilityOrigin, HostOnly),
    MEMBER(isOSC52Allowed, HostOnly),
    MEMBER(osc52ClipboardReadPolicy, HostOnly),
    MEMBER(osc52ClipboardWritePolicy, HostOnly),
    MEMBER(scrollbackLimit, HostOnly),
    MEMBER(triggersEngine, HostOnly),
    MEMBER(triggerHighlights, HostOnly),
    MEMBER(triggerHighlightCount, HostOnly),
    MEMBER(triggerHighlightHead, HostOnly),
    MEMBER(integration, HostOnly),
    MEMBER(scrollSearchState, HostOnly),

    // Facts each frontend publishes about its own surface; a client keeps its own.
    MEMBER(windowFocused, HostOnly),
    MEMBER(locatorRow, HostOnly),
    MEMBER(locatorColumn, HostOnly),
    MEMBER(locatorButtons, HostOnly),
    MEMBER(drawablePixelWidth, HostOnly),
    MEMBER(drawablePixelHeight, HostOnly),
    MEMBER(lastActivityTick, HostOnly),
    MEMBER(cursorHoldFrames, HostOnly),
    MEMBER(cursorBlinkPeriodMilliseconds, HostOnly),
    MEMBER(cursorBlinkPref, HostOnly),
    MEMBER(cursorShapePref, HostOnly),
    MEMBER(cursorMotionModePref, HostOnly),
    MEMBER(cursorMotionDurationMilliseconds, HostOnly),
    MEMBER(cellPixelWidth, HostOnly),
    MEMBER(cellPixelHeight, HostOnly),
    MEMBER(textAreaPixelWidth, HostOnly),
    MEMBER(textAreaPixelHeight, HostOnly),
    MEMBER(screenPixelWidth, HostOnly),
    MEMBER(screenPixelHeight, HostOnly),
    MEMBER(windowIconified, HostOnly),
    MEMBER(winPosX, HostOnly),
    MEMBER(winPosY, HostOnly),
    MEMBER(font, HostOnly),
    MEMBER(fontRequests, HostOnly),
    MEMBER(fontRequestChanged, HostOnly),

    // The activity block carries the contexts and the program status records
    // until a status-record plane does; the commit bumps the generation.
    MEMBER(contexts, Digested),
    MEMBER(programStatus, Digested),
    MEMBER(activityGeneration, Derived),
    // Pane facts and effects: their own planes carry them. What a record
    // carries into a replica is Recorded; the generation beside it is the
    // replica's own, moved when a record changes the member.
    MEMBER(progressState, Recorded),
    MEMBER(progressValue, Recorded),
    MEMBER(progressGeneration, HostOnly),
    MEMBER(termpropProgressHint, HostOnly),
    MEMBER(termpropProgressValue, HostOnly),
    MEMBER(tabStatusSet, Recorded),
    MEMBER(tabStatusR, Recorded),
    MEMBER(tabStatusG, Recorded),
    MEMBER(tabStatusB, Recorded),
    MEMBER(tabStatusText, Recorded),
    MEMBER(tabStatusTextLength, Recorded),
    MEMBER(tabStatusGeneration, HostOnly),
    // The host answers the OSC 176 query, and no frontend applies the ID,
    // so it stays on the host.
    MEMBER(applicationIdentifier, HostOnly),
    MEMBER(tabColorSet, Recorded),
    MEMBER(tabColorRed, Recorded),
    MEMBER(tabColorGreen, Recorded),
    MEMBER(tabColorBlue, Recorded),
    MEMBER(badgeText, Recorded),
    MEMBER(hasUnseenBell, Recorded),
    MEMBER(hasUnseenNotification, Recorded),
    MEMBER(unseenNotificationTitle, Recorded),
    MEMBER(unseenNotificationBody, Recorded),
    MEMBER(attentionGeneration, HostOnly),
    MEMBER(kittyNotifications, HostOnly),
    MEMBER(currentDirectoryChanged, HostOnly),
    MEMBER(bellPending, HostOnly),
    MEMBER(lastBellTick, HostOnly),
    MEMBER(remoteHost, HostOnly),
    MEMBER(userVariables, Recorded),
    MEMBER(lastCommandExitCode, HostOnly),
    MEMBER(commandCompletedGeneration, HostOnly),
    MEMBER(commandExitCodeRing, HostOnly),
    MEMBER(commandStartedGeneration, HostOnly),
    MEMBER(commandStartedAtCompletionRing, HostOnly),
    MEMBER(hasCommandStartTick, HostOnly),
    MEMBER(commandStartTick, HostOnly),
    MEMBER(commandDurationTicksRing, HostOnly),
    MEMBER(pendingPromptRow, HostOnly),
    MEMBER(isReadingCommand, HostOnly),
    MEMBER(hasMarkedPrompt, HostOnly),
    MEMBER(commandLine, HostOnly),
    MEMBER(commandLineGeneration, HostOnly),
    MEMBER(currentDirectory, HostOnly),
    MEMBER(reportedDirectory, HostOnly),
    MEMBER(reportedHost, HostOnly),
    MEMBER(reportingProcessGroup, HostOnly),
    MEMBER(reportedHostProcessGroup, HostOnly),
    MEMBER(sampledDirectory, HostOnly),
    MEMBER(titleOwnerProcessGroup, HostOnly),
    MEMBER(foregroundProcessGroup, HostOnly),
    MEMBER(currentFileURL, Recorded),
    MEMBER(sampleBank, HostOnly),
    MEMBER(pendingSound, HostOnly),
    // DECAC's colours decide only how the parser resolves a pen; the pens and
    // cells they were resolved into are carried with the screens.
    MEMBER(normalTextColors, HostOnly),
    MEMBER(windowFrameColors, HostOnly),
    MEMBER(alternateTextColors, HostOnly),

    // Clipboard transfers in flight between the program and the host.
    MEMBER(isOSC52ClipboardReadInFlight, HostOnly),
    MEMBER(isClipboardReadRequestBellTerminated, HostOnly),
    MEMBER(osc52Chunk, HostOnly),
    MEMBER(osc52ChunkActive, HostOnly),
    MEMBER(osc52ChunkOverflow, HostOnly),
    MEMBER(osc52ChunkSet, HostOnly),
    MEMBER(isPendingClipboardReadExtended, HostOnly),
    MEMBER(extendedClipboardReadID, HostOnly),
    MEMBER(isExtendedClipboardReadPrimary, HostOnly),
    MEMBER(extendedClipboardWriteItems, HostOnly),
    MEMBER(extendedClipboardWriteItemMimes, HostOnly),
    MEMBER(extendedClipboardWriteItemCount, HostOnly),
    MEMBER(extendedClipboardWriteTotalBytes, HostOnly),
    MEMBER(isExtendedClipboardWriteActive, HostOnly),
    MEMBER(isExtendedClipboardWritePrimary, HostOnly),
    MEMBER(didExtendedClipboardWriteOverflow, HostOnly),
    MEMBER(extendedClipboardWriteID, HostOnly),
    MEMBER(extendedClipboardWriteMime, HostOnly),
    MEMBER(extendedClipboardBase64Tail, HostOnly),
    MEMBER(extendedClipboardBase64TailLength, HostOnly),
    MEMBER(isExtendedClipboardBase64Complete, HostOnly),
    MEMBER(extendedClipboardSessionAliases, HostOnly),
    MEMBER(extendedClipboardServedAliases, HostOnly),
    MEMBER(extendedClipboardReadMimes, HostOnly),
    MEMBER(isExtendedClipboardReadListRequested, HostOnly),

    // The host's id allocators: the client mints no link, cluster or run id.
    MEMBER(linkNextID, HostOnly),
    MEMBER(linkFreeIDs, HostOnly),
    MEMBER(linkReclaimBackoffMints, HostOnly),
    MEMBER(runTextReclaimBackoffInterns, HostOnly),
    MEMBER(textSizeReclaimBackoffInterns, HostOnly),

    // Images, the soft font and the Tektronix store travel on planes of their
    // own, by content hash.
    MEMBER(images, HostOnly),
    MEMBER(softFont, HostOnly),
    MEMBER(tek, HostOnly),
#undef MEMBER
};

#undef UBIQUITTY_SYNC_LEDGER

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static_assert(tiles(terminalModesLedger, sizeof(Ubiquitty::Terminal::Modes), alignof(Ubiquitty::Terminal::Modes)),
    "a Terminal::Modes member is missing from terminalModesLedger: classify it, and carry it in the modes block");
static_assert(tiles(screenStateLedger, sizeof(Ubiquitty::Screen::State), alignof(Ubiquitty::Screen::State)),
    "a Screen::State member is missing from screenStateLedger: classify it, and carry it if it is not host-only");
static_assert(tiles(entriesOf(parserLedger), sizeof(Ubiquitty::Terminal::Parser), alignof(Ubiquitty::Terminal::Parser)),
    "a Terminal::Parser member is missing from parserLedger: classify it, pending state as host-only");
static_assert(isParseRoleRuleHeld(parserLedger),
    "parserLedger carries pending parser state, or leaves ground state behind: a checkpoint must hold exactly the parser's ground state");
static_assert(tiles(terminalLedger, sizeof(Ubiquitty::Terminal), alignof(Ubiquitty::Terminal)),
    "a Terminal member is missing from terminalLedger: classify it, and carry it if it is not host-only");

} // namespace Ubiquitty::Protocol::Sync
