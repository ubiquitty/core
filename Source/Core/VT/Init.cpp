// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// VT/Init.cpp
//
// Cold lifecycle + scrollback-mutation slice: init/dispose, scrollback-limit +
// buffer clears, scrollback ring mutation (drop/pull/delete rows), and the
// clear-to-previous-mark path. The public API declared in Terminal.h is defined here.

#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellGrid.h"
#include "Core/Rendering/StatusRow.h"

#include "Core/CellPen.h"
#include "Core/LineStore.h"
#include "Core/OSC/MousePointerShape.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/ScrollSearch.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/TerminalOperations.h"
#include "Core/Triggers/Engine.h"
#include "Core/VT/Internal.h"
#include "Core/VT/ParseState.h"
#include "PAL/PAL.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace Ubiquitty::VT {

using ParserState = Ubiquitty::VT::Parser::State;

} // namespace Ubiquitty::VT

namespace Ubiquitty {

namespace {

struct HardResetFileCloser {
    void operator()(std::FILE* file) const
    {
        if (file)
            std::fclose(file);
    }
};

// One of the colours OSC 13 to OSC 19 set, or kitty's cursor_text, carried across RIS.
struct CarriedDynamicColor {
    uint16_t red;
    uint16_t green;
    uint16_t blue;
    bool isSet;
};

struct CarriedDynamicColorFields {
    uint16_t Ubiquitty::Terminal::* red;
    uint16_t Ubiquitty::Terminal::* green;
    uint16_t Ubiquitty::Terminal::* blue;
    bool Ubiquitty::Terminal::* isSet;
};

constexpr auto carriedDynamicColors = std::to_array<CarriedDynamicColorFields>({
    { &Ubiquitty::Terminal::mousePointerForegroundRed, &Ubiquitty::Terminal::mousePointerForegroundGreen, &Ubiquitty::Terminal::mousePointerForegroundBlue, &Ubiquitty::Terminal::mousePointerForegroundSet },
    { &Ubiquitty::Terminal::mousePointerBackgroundRed, &Ubiquitty::Terminal::mousePointerBackgroundGreen, &Ubiquitty::Terminal::mousePointerBackgroundBlue, &Ubiquitty::Terminal::mousePointerBackgroundSet },
    { &Ubiquitty::Terminal::tektronixForegroundRed, &Ubiquitty::Terminal::tektronixForegroundGreen, &Ubiquitty::Terminal::tektronixForegroundBlue, &Ubiquitty::Terminal::tektronixForegroundSet },
    { &Ubiquitty::Terminal::tektronixBackgroundRed, &Ubiquitty::Terminal::tektronixBackgroundGreen, &Ubiquitty::Terminal::tektronixBackgroundBlue, &Ubiquitty::Terminal::tektronixBackgroundSet },
    { &Ubiquitty::Terminal::selectionBackgroundRed, &Ubiquitty::Terminal::selectionBackgroundGreen, &Ubiquitty::Terminal::selectionBackgroundBlue, &Ubiquitty::Terminal::selectionBackgroundSet },
    { &Ubiquitty::Terminal::tektronixCursorRed, &Ubiquitty::Terminal::tektronixCursorGreen, &Ubiquitty::Terminal::tektronixCursorBlue, &Ubiquitty::Terminal::tektronixCursorSet },
    { &Ubiquitty::Terminal::selectionForegroundRed, &Ubiquitty::Terminal::selectionForegroundGreen, &Ubiquitty::Terminal::selectionForegroundBlue, &Ubiquitty::Terminal::selectionForegroundSet },
    { &Ubiquitty::Terminal::cursorTextRed, &Ubiquitty::Terminal::cursorTextGreen, &Ubiquitty::Terminal::cursorTextBlue, &Ubiquitty::Terminal::hasCursorTextColor },
});

struct HardResetPersistentState {
    int rows;
    int columns;
    uint16_t cellPixelWidth;
    uint16_t cellPixelHeight;
    uint16_t textAreaPixelWidth;
    uint16_t textAreaPixelHeight;
    uint16_t drawablePixelWidth;
    uint16_t drawablePixelHeight;
    uint16_t screenPixelWidth;
    uint16_t screenPixelHeight;
    int32_t windowPositionX;
    int32_t windowPositionY;
    bool windowFocused;
    bool isWindowIconified;
    bool drawsSizedText;
    bool drawsProportionalText;

    decltype(Ubiquitty::Terminal::answerback) answerback;
    bool isAnswerbackConcealed = false;
    Ubiquitty::Unicode::Encoding inputEncoding;
    // Set for the length of a UTF-8 parse, which a reset partway through it
    // does not end: the bytes after RIS in the same write are still UTF-8, so
    // a raw C1 byte among them is a stray byte rather than a control.
    bool isParserInputUTF8;
    Ubiquitty::Unicode::InvalidUTF8Policy invalidUTF8Policy;
    VT::Emulation emulation;
    bool passwordDetect;
    int scrollbackLimit;
    uint64_t imageBytesBudget;
    // A block placed after the reset must not reuse a serial an attachment
    // still holds a document for.
    uint32_t richTextNextSerial;
    // The host's source-storage quota is local policy, not screen content.
    std::size_t richTextStorageLimit;
    RichText::PublishedTypography typography;
    std::uint32_t richTextMeasurementToken;

    bool allowTitleOperations;
    bool allowFontOperations;
    std::string font;
    OSC::FontRequestQueue fontRequests;
    bool fontRequestChanged;
    bool allowColorOperations;
    bool allowWindowOperations;
    bool allowTerminalCapabilityOperations;
    bool allowMouseOperations;
    bool allowPasteControls;
    // What XTSAVE saved outlives a reset, as in xterm: RIS restores the
    // terminal's state, not the program's record of an earlier one.
    decltype(Ubiquitty::Terminal::savedPrivateModeNumbers) savedPrivateModeNumbers;
    decltype(Ubiquitty::Terminal::savedPrivateModeStates) savedPrivateModeStates;
    Ubiquitty::OSC::SequenceGate sequenceGate;
    bool isScrollLaneEnabled;
    bool isBandScrollLaneEnabled;

    Ubiquitty::BellMode bellMode;
    uint32_t lastBellTick;
    uint8_t cursorShapePreference;
    bool cursorBlinkPreference;
    uint32_t cursorHoldFrames;
    uint32_t cursorBlinkPeriodMilliseconds;
    Ubiquitty::Rendering::CursorMotionMode cursorMotionModePreference;
    Ubiquitty::Tek::ColorPreference tekColorPreference;
    uint32_t cursorMotionDurationMilliseconds;
    uint32_t lastActivityTick;
    // Monotonic facts a publish gate compares between frames; zeroing them
    // would read as a close and a hide that never happened.
    uint32_t syncOutputCloseCount;
    uint32_t cursorHideCount;

    // Carried as the gated wrapper rather than a raw pointer: a hard reset
    // preserves the wiring across the wipe, which is a copy of the field and not
    // an effect, so it needs no capability and asks for none.
    Ubiquitty::GatedClient<Ubiquitty::WindowClient> windowClient;
    Ubiquitty::ClipboardClient* clipboardClient;
    Ubiquitty::ShellClient* shellClient;
    Ubiquitty::GatedClient<Ubiquitty::DragDrop::Client> dragDropClient;
    std::unique_ptr<Ubiquitty::DragDrop::Session> dragDropSession;
    Ubiquitty::DiagnosticsClient* diagnosticsClient;
    Ubiquitty::FileReadClient* fileReadClient;
    Ubiquitty::TriggerClient* triggerClient;
    // The route is the frontend's wiring, not the program's state: a program
    // that sends RIS must not unhook its pane's effects.
    Ubiquitty::EffectRoute* effectRoute;
    Ubiquitty::TerminalClock* tickClock;
    Ubiquitty::ReplyAuthority replyAuthority;
    Ubiquitty::GatedPolicy capabilityPolicy;
    Ubiquitty::CapabilityOrigin capabilityOrigin;

    bool isOSC52Allowed;
    Ubiquitty::OSC::ClipboardReadPolicy osc52ClipboardReadPolicy;
    Ubiquitty::OSC::ClipboardWritePolicy osc52ClipboardWritePolicy;
    bool isOSC52ClipboardReadInFlight;
    // The rest of the outstanding read: which protocol asked, and what its
    // answer must echo. Carried WITH the flag or a RIS mid-prompt splits the
    // state — the completion would still fire (the flag survived) but frame
    // the answer as the other protocol's.
    bool isPendingClipboardReadExtended;
    std::array<char, 65> extendedClipboardReadID;
    std::array<char, 256> extendedClipboardReadMimes;
    bool isExtendedClipboardReadListRequested;
    bool isExtendedClipboardReadPrimary;

    Ubiquitty::TermPalette paletteBaseline;
    // The dynamic colours, OSC 10-19, outlive RIS: xterm's ReallyReset resets
    // the indexed palette (charproc.c:14405) and leaves the colours those
    // controls set as they are, so only OSC 110-119 put them back.
    Ubiquitty::PaletteRGB defaultForeground;
    Ubiquitty::PaletteRGB defaultBackground;
    Ubiquitty::PaletteRGB cursorColor;
    std::array<CarriedDynamicColor, carriedDynamicColors.size()> dynamicColors;
    std::unique_ptr<Ubiquitty::Triggers::Engine> triggersEngine;
    std::unique_ptr<std::FILE, HardResetFileCloser> sessionLogFile;
    std::unique_ptr<std::FILE, HardResetFileCloser> printSinkFile;
    bool sessionLogCaptureEnabled;

    OSC::ContextState contexts;
    // What nobody has seen yet is a fact about the people, not the program: a
    // reset the program sends does not make a bell heard.
    bool hasUnseenBell;
    bool hasUnseenNotification;
    std::string unseenNotificationTitle;
    std::string unseenNotificationBody;
    uint32_t attentionGeneration;
    uint64_t nextActivityGeneration;
    uint32_t nextTabStatusGeneration;
    Ubiquitty::Terminal::Integration integration;
    int dcsTmuxPassthroughDepth;
    uint32_t nextAbsoluteRowEpoch;
    uint32_t nextGeneration;
    // The palette's epoch keys every cache of resolved colours, a retained
    // frame among them, and RIS replaces the palette with its baseline. Reset
    // to its starting value, the epoch can equal one a cache was filled at
    // while the colours differ; one past the epoch before the reset equals
    // none that was already drawn.
    uint64_t nextPaletteEpoch;
    // RIS destroys the history with the Terminal, so it opens a history epoch;
    // the numbering resumes above every number the old epoch reached.
    uint32_t nextHistoryEpoch;
    uint64_t historyLineHead;
    // Line names never repeat: a position a frontend held across the reset
    // must name nothing afterwards, not a line the new history numbers alike.
    Ubiquitty::Screen::LineIdentityContinuation lineIdentity;

    // The live grid, buffers and all. Reconstruction must not free working
    // buffers to reallocate them: under pressure the realloc is refused where
    // keeping would have succeeded, and a refused fresh grid initializes to a
    // degenerate geometry the Terminal then disagrees with. Carried across,
    // re-blanked below, its capacity reused without allocating.
    std::unique_ptr<Ubiquitty::CellGrid> grid;
};

// Binds the per-row metadata views to their CellGrid + screen index. One
// spelling: initializeTerminal binds the fresh grid, hardReset rebinds the
// carried one after the handoff swaps the object the views point at.
static void bindScreenViewsToGrid(Ubiquitty::Terminal& term)
{
    Ubiquitty::CellGrid* grid = term.grid.get();
    term.screens.primary.lineAttribute.owner = grid;
    term.screens.primary.lineAttribute.screen = 0;
    term.screens.primary.lineWrapped.owner = grid;
    term.screens.primary.lineWrapped.screen = 0;
    term.screens.primary.mark.owner = grid;
    term.screens.primary.mark.screen = 0;
    term.screens.alternate.lineAttribute.owner = grid;
    term.screens.alternate.lineAttribute.screen = 1;
    term.screens.alternate.lineWrapped.owner = grid;
    term.screens.alternate.lineWrapped.screen = 1;
    term.screens.alternate.mark.owner = grid;
    term.screens.alternate.mark.screen = 1;
}

HardResetPersistentState takeHardResetPersistentState(Ubiquitty::Terminal& term)
{
    HardResetPersistentState state;
    state.rows = term.rows;
    // RIS returns a DECCOLM-widened page to 80. A width the USER chose (window
    // resize, DECCOLM never used) is preserved — only the escape-driven 132 is
    // undone.
    state.columns = term.modes.columns132 ? defaultColumns : term.columns;
    state.cellPixelWidth = term.cellPixelWidth;
    state.cellPixelHeight = term.cellPixelHeight;
    state.textAreaPixelWidth = term.textAreaPixelWidth;
    state.textAreaPixelHeight = term.textAreaPixelHeight;
    state.drawablePixelWidth = term.drawablePixelWidth;
    state.drawablePixelHeight = term.drawablePixelHeight;
    state.screenPixelWidth = term.screenPixelWidth;
    state.screenPixelHeight = term.screenPixelHeight;
    state.windowPositionX = term.winPosX;
    state.windowPositionY = term.winPosY;
    state.windowFocused = term.windowFocused;
    state.isWindowIconified = term.windowIconified;
    state.drawsSizedText = term.drawsSizedText;
    state.drawsProportionalText = term.drawsProportionalText;

    state.answerback = term.answerback;
    state.isAnswerbackConcealed = term.isAnswerbackConcealed;
    state.inputEncoding = term.parser.inputEncoding.enc;
    state.isParserInputUTF8 = term.parser.parserInputIsUTF8;
    state.invalidUTF8Policy = term.parser.invalidUTF8Policy;
    state.emulation = term.emulation;
    state.passwordDetect = term.passwordDetect;
    state.scrollbackLimit = term.scrollbackLimit;
    state.imageBytesBudget = term.images.kittyImageBytesBudget;
    state.richTextNextSerial = term.richTextBlocks.nextSerial();
    state.richTextStorageLimit = term.richTextBlocks.storageLimit();
    state.typography = term.typography;
    state.richTextMeasurementToken = term.richTextMeasurementToken;

    state.allowTitleOperations = term.allowTitleOperations;
    state.allowFontOperations = term.allowFontOperations;
    // Font adoption outlives parsing: RIS cannot discard accepted requests or
    // invalidate an application already in progress on the frontend.
    state.font = std::move(term.font);
    state.fontRequests = std::move(term.fontRequests);
    state.fontRequestChanged = term.fontRequestChanged;
    state.allowColorOperations = term.allowColorOperations;
    state.allowWindowOperations = term.allowWindowOperations;
    state.allowTerminalCapabilityOperations = term.allowTerminalCapabilityOperations;
    state.allowMouseOperations = term.allowMouseOperations;
    state.allowPasteControls = term.allowPasteControls;
    state.savedPrivateModeNumbers = term.savedPrivateModeNumbers;
    state.savedPrivateModeStates = term.savedPrivateModeStates;
    state.sequenceGate = term.sequenceGate;
    // The deferred scroll lanes are the host's choice, made when it created
    // the terminal; a program's RIS cannot turn them off for the session.
    state.isScrollLaneEnabled = term.isScrollLaneEnabled;
    state.isBandScrollLaneEnabled = term.isBandScrollLaneEnabled;

    state.bellMode = term.bellMode;
    state.lastBellTick = term.lastBellTick;
    state.cursorShapePreference = term.cursorShapePref;
    state.cursorBlinkPreference = term.cursorBlinkPref;
    state.cursorHoldFrames = term.cursorHoldFrames;
    state.cursorBlinkPeriodMilliseconds = term.cursorBlinkPeriodMilliseconds;
    state.cursorMotionModePreference = term.cursorMotionModePref;
    state.tekColorPreference = term.tekColorPreference;
    state.cursorMotionDurationMilliseconds = term.cursorMotionDurationMilliseconds;
    state.lastActivityTick = term.lastActivityTick;
    state.syncOutputCloseCount = term.syncOutputCloseCount;
    state.cursorHideCount = term.cursorHideCount;

    state.windowClient = term.windowClient;
    state.clipboardClient = term.clipboardClient.get();
    state.shellClient = term.shellClient.get();
    state.dragDropClient = term.dragDropClient;
    state.dragDropSession = std::move(term.dragDropSession);
    state.diagnosticsClient = term.diagnosticsClient.get();
    state.fileReadClient = term.fileReadClient.get();
    state.triggerClient = term.triggerClient.get();
    state.effectRoute = term.effectRoute.get();
    state.tickClock = term.tickClock.get();
    state.replyAuthority = term.replyAuthority;
    state.capabilityPolicy = term.capabilityPolicy;
    state.capabilityOrigin = term.currentCapabilityOrigin;

    state.isOSC52Allowed = term.isOSC52Allowed;
    state.osc52ClipboardReadPolicy = term.osc52ClipboardReadPolicy;
    state.osc52ClipboardWritePolicy = term.osc52ClipboardWritePolicy;
    // A read query outstanding with the frontend outlives RIS. The reply still has to
    // arrive, and the same stream that floods the query can emit RIS between two of
    // them — clearing the flag here would make the cap something the attacker resets.
    state.isOSC52ClipboardReadInFlight = term.isOSC52ClipboardReadInFlight;
    state.isPendingClipboardReadExtended = term.isPendingClipboardReadExtended;
    state.extendedClipboardReadID = term.extendedClipboardReadID;
    state.extendedClipboardReadMimes = term.extendedClipboardReadMimes;
    state.isExtendedClipboardReadListRequested = term.isExtendedClipboardReadListRequested;
    state.isExtendedClipboardReadPrimary = term.isExtendedClipboardReadPrimary;

    state.paletteBaseline = term.paletteBaseline;
    state.defaultForeground = term.palette.defaultForeground;
    state.defaultBackground = term.palette.defaultBackground;
    state.cursorColor = term.palette.cursor;
    for (std::size_t index = 0; index < carriedDynamicColors.size(); ++index) {
        const CarriedDynamicColorFields& fields = carriedDynamicColors[index];
        state.dynamicColors[index] = { term.*fields.red, term.*fields.green, term.*fields.blue, term.*fields.isSet };
    }
    state.triggersEngine = std::move(term.triggersEngine);
    state.sessionLogFile.reset(static_cast<std::FILE*>(term.sessionLogFile));
    state.printSinkFile.reset(static_cast<std::FILE*>(term.printSinkFile));
    state.sessionLogCaptureEnabled = term.sessionLogCaptureEnabled;

    state.integration = term.integration;
    state.dcsTmuxPassthroughDepth = term.dcsTmuxPassthroughDepth;
    state.nextAbsoluteRowEpoch = term.absoluteRowEpoch + 1;
    state.nextHistoryEpoch = Ubiquitty::Screen::hasNumberedHistoryInEpoch(term) ? term.historyEpoch + 1 : term.historyEpoch;
    state.historyLineHead = term.lineStore ? std::max(term.lineStore->maxHeadLineNumber(), term.historyEpochBase) : term.historyEpochBase;
    state.lineIdentity = Ubiquitty::Screen::lineIdentityContinuation(term);
    state.nextGeneration = term.generation + 1;
    state.nextPaletteEpoch = term.paletteEpoch + 1;
    state.contexts = std::move(term.contexts);
    state.nextActivityGeneration = term.activityGeneration + 1;
    state.nextTabStatusGeneration = term.tabStatusGeneration + 1;
    state.hasUnseenBell = term.hasUnseenBell;
    state.hasUnseenNotification = term.hasUnseenNotification;
    state.unseenNotificationTitle = std::move(term.unseenNotificationTitle);
    state.unseenNotificationBody = std::move(term.unseenNotificationBody);
    state.attentionGeneration = term.attentionGeneration;

    // These resources are configured outside the terminal protocol. Transfer
    // their ownership so reconstruction cannot close or delete them.
    term.sessionLogFile = nullptr;
    term.printSinkFile = nullptr;
    // The grid goes the same way for a different reason: its buffers are
    // already the right size, and freeing them here forces a fresh allocation
    // below that pressure can refuse. Moved out so the destructor's
    // grid.reset() finds nothing to free.
    state.grid = std::move(term.grid);
    return state;
}

void restoreHardResetPersistentState(Ubiquitty::Terminal& term, HardResetPersistentState& state)
{
    term.drawsSizedText = state.drawsSizedText;
    term.drawsProportionalText = state.drawsProportionalText;
    setScrollbackLimit(&term, state.scrollbackLimit);
    resize(term, state.rows, state.columns, Modes::ReflowMode::Fixed);
    term.cellPixelWidth = state.cellPixelWidth;
    term.cellPixelHeight = state.cellPixelHeight;
    term.textAreaPixelWidth = state.textAreaPixelWidth;
    term.textAreaPixelHeight = state.textAreaPixelHeight;
    term.drawablePixelWidth = state.drawablePixelWidth;
    term.drawablePixelHeight = state.drawablePixelHeight;
    term.screenPixelWidth = state.screenPixelWidth;
    term.screenPixelHeight = state.screenPixelHeight;
    term.winPosX = state.windowPositionX;
    term.winPosY = state.windowPositionY;
    term.windowFocused = state.windowFocused;
    term.windowIconified = state.isWindowIconified;

    term.answerback = state.answerback;
    term.isAnswerbackConcealed = state.isAnswerbackConcealed;
    Modes::setInputEncoding(&term, state.inputEncoding);
    term.parser.parserInputIsUTF8 = state.isParserInputUTF8;
    Modes::setInvalidUTF8Policy(&term, state.invalidUTF8Policy);
    Modes::setEmulation(&term, state.emulation);
    setPasswordDetect(&term, state.passwordDetect);
    term.images.kittyImageBytesBudget = state.imageBytesBudget;
    term.richTextBlocks.resumeSerials(state.richTextNextSerial);
    // The reset store holds nothing, so it admits any quota the old one held.
    (void)term.richTextBlocks.setStorageLimit(state.richTextStorageLimit);
    term.typography = std::move(state.typography);
    term.typography.subscriptionID = 0;
    term.typography.needsReport = false;
    term.richTextMeasurementToken = state.richTextMeasurementToken;

    term.allowTitleOperations = state.allowTitleOperations;
    term.allowFontOperations = state.allowFontOperations;
    term.font = std::move(state.font);
    term.fontRequests = std::move(state.fontRequests);
    term.fontRequestChanged = state.fontRequestChanged;
    term.allowColorOperations = state.allowColorOperations;
    term.allowWindowOperations = state.allowWindowOperations;
    term.allowTerminalCapabilityOperations = state.allowTerminalCapabilityOperations;
    term.allowMouseOperations = state.allowMouseOperations;
    term.allowPasteControls = state.allowPasteControls;
    term.savedPrivateModeNumbers = state.savedPrivateModeNumbers;
    term.savedPrivateModeStates = state.savedPrivateModeStates;
    term.sequenceGate = state.sequenceGate;
    term.isScrollLaneEnabled = state.isScrollLaneEnabled;
    term.isBandScrollLaneEnabled = state.isBandScrollLaneEnabled;

    term.bellMode = state.bellMode;
    term.lastBellTick = state.lastBellTick;
    term.cursorShapePref = state.cursorShapePreference;
    term.cursorBlinkPref = state.cursorBlinkPreference;
    term.cursorHoldFrames = state.cursorHoldFrames;
    term.cursorBlinkPeriodMilliseconds = state.cursorBlinkPeriodMilliseconds;
    term.cursorMotionModePref = state.cursorMotionModePreference;
    term.tekColorPreference = state.tekColorPreference;
    term.cursorMotionDurationMilliseconds = state.cursorMotionDurationMilliseconds;
    term.lastActivityTick = state.lastActivityTick;
    term.syncOutputCloseCount = state.syncOutputCloseCount;
    term.cursorHideCount = state.cursorHideCount;

    term.windowClient = state.windowClient;
    term.clipboardClient = state.clipboardClient;
    term.shellClient = state.shellClient;
    term.dragDropClient = state.dragDropClient;
    term.dragDropSession = std::move(state.dragDropSession);
    term.diagnosticsClient = state.diagnosticsClient;
    term.fileReadClient = state.fileReadClient;
    term.triggerClient = state.triggerClient;
    term.effectRoute = state.effectRoute;
    term.tickClock = state.tickClock;
    term.replyAuthority = state.replyAuthority;
    term.capabilityPolicy = state.capabilityPolicy;
    term.currentCapabilityOrigin = state.capabilityOrigin;

    term.isOSC52Allowed = state.isOSC52Allowed;
    term.osc52ClipboardReadPolicy = state.osc52ClipboardReadPolicy;
    term.osc52ClipboardWritePolicy = state.osc52ClipboardWritePolicy;
    term.isOSC52ClipboardReadInFlight = state.isOSC52ClipboardReadInFlight;
    term.isPendingClipboardReadExtended = state.isPendingClipboardReadExtended;
    term.extendedClipboardReadID = state.extendedClipboardReadID;
    term.extendedClipboardReadMimes = state.extendedClipboardReadMimes;
    term.isExtendedClipboardReadListRequested = state.isExtendedClipboardReadListRequested;
    term.isExtendedClipboardReadPrimary = state.isExtendedClipboardReadPrimary;

    term.paletteBaseline = state.paletteBaseline;
    term.palette = state.paletteBaseline;
    term.palette.defaultForeground = state.defaultForeground;
    term.palette.defaultBackground = state.defaultBackground;
    term.palette.cursor = state.cursorColor;
    for (std::size_t index = 0; index < carriedDynamicColors.size(); ++index) {
        const CarriedDynamicColorFields& fields = carriedDynamicColors[index];
        const CarriedDynamicColor& color = state.dynamicColors[index];
        term.*fields.red = color.red;
        term.*fields.green = color.green;
        term.*fields.blue = color.blue;
        term.*fields.isSet = color.isSet;
    }
    term.triggersEngine = std::move(state.triggersEngine);
    term.sessionLogFile = state.sessionLogFile.release();
    term.printSinkFile = state.printSinkFile.release();
    term.sessionLogCaptureEnabled = state.sessionLogCaptureEnabled;

    term.contexts = std::move(state.contexts);
    term.activityGeneration = state.nextActivityGeneration;
    term.tabStatusGeneration = state.nextTabStatusGeneration;
    term.hasUnseenBell = state.hasUnseenBell;
    term.hasUnseenNotification = state.hasUnseenNotification;
    term.unseenNotificationTitle = std::move(state.unseenNotificationTitle);
    term.unseenNotificationBody = std::move(state.unseenNotificationBody);
    term.attentionGeneration = state.attentionGeneration;
    term.integration = state.integration;
    term.dcsTmuxPassthroughDepth = state.dcsTmuxPassthroughDepth;
    term.absoluteRowEpoch = state.nextAbsoluteRowEpoch;
    term.generation = state.nextGeneration;
    term.paletteEpoch = state.nextPaletteEpoch;
    term.historyEpoch = state.nextHistoryEpoch;
    term.historyEpochBase = state.historyLineHead;
    Ubiquitty::Screen::restoreLineIdentity(term, state.lineIdentity);
}

} // namespace

void initializeTerminal(Ubiquitty::Terminal& t, TerminalInitialization initialization)
{
    if (initialization == TerminalInitialization::Complete)
        t.parser.osc.data = static_cast<char*>(std::calloc(VT::oscBufferInitialCapacity, 1));
    t.parser.osc.capacity = t.parser.osc.data ? VT::oscBufferInitialCapacity : 0;
    t.parser.osc.length = 0;
    t.rows = defaultRows;
    t.columns = defaultColumns;
    // Seed neutral palette defaults (frontend overwrites on theme apply).
    // paletteBaseline mirrors it so OSC 110/111/112 and OSC 4 resets revert.
    t.palette.seedDefaults();
    t.paletteBaseline = t.palette;
    VT::screenInit(&t.screens.primary, t.rows, t.columns);
    VT::screenInit(&t.screens.alternate, t.rows, t.columns);
    t.screens.activate(Ubiquitty::Modes::ScreenBuffer::Primary);
    t.modes.allowAlternateScreen = true; // DECSET 1046 default: alternate-screen feature enabled
    t.modes.isCursorVisible = true;
    t.cursorBlinkPref = true;
    t.cursorBlinkAtt610 = false;
    t.modes.allow80To132 = false;
    t.modes.clearScreenOnColumnChange = true;
    // DECSCL default: VT500 (highest advertised), 8-bit C1 allowed. DECSDM (?80)
    // defaults reset.
    t.conformanceLevel = VT::ConformanceLevel::VT500;
    t.modes.sixelDisplayMode = false;
    t.bellMode = Ubiquitty::BellMode::Audio;
    t.modes.autowrap = true;
    t.windowFocused = true; // assume focused until the frontend says otherwise
    // Scroll-on-output (1010) off, scroll-on-keypress (1011) on.
    t.modes.scrollOnKeypress = true;
    VT::resetTabStops(t);
    t.lineTabStops.reset();
    t.characterSetG[0] = VT::characterSetUSASCII;
    t.characterSetG[1] = VT::characterSetUSASCII;
    t.characterSetG[2] = VT::characterSetUSASCII;
    t.characterSetG[3] = VT::characterSetUSASCII;
    t.characterSetGR = 0; // GR default: inert (Latin-1), no LS*R invoked
    t.parser.is96CharacterSet = false;
    t.singleShift = 0;
    t.modes.lnm = false;
    t.modes.srm = true;
    t.modes.autoRepeat = true; // DECARM on by default (auto-repeat enabled)
    // Meta/Alternate keyboard (1034/1036/1039): metaSendsEscape on at power-on
    // (Option-as-Meta ESC-prefix), the other two off. Not in the DECSTR list, so
    // softReset leaves them untouched.
    t.modes.metaSendsEscape = true;
    t.modes.shouldHonorNumLock = true;
    t.modes.keyboardType = Keys::KeyboardType::Default;
    t.modes.termcapProfile = Keys::TermcapProfile::Ubiquitty;
    t.modes.alternateSendsEscape = false;
    t.modes.eightBitInput = false;
    // Edit-keypad Delete (1037) reset → Delete sends CSI 3 ~ (VT220 Remove).
    // Not in DECSTR, so softReset leaves it untouched.
    t.modes.deleteKeySendsDelete = false;
    // Font-shifting (35) and keepClipboard (1044) reset. Not in DECSTR.
    t.modes.fontShifting = false;
    t.modes.keepClipboard = false;
    // DECECM (117), the sixel register and cursor modes (1070, 8452), rewrap
    // on resize (7723), ambiguous-width reporting (7700) and the input method
    // editor (77096): power-on defaults. None is in the DECSTR list.
    t.modes.shouldEraseToDefaultColors = false;
    t.modes.hasPrivateSixelColorRegisters = true;
    t.modes.shouldLeaveCursorRightOfSixel = false;
    t.modes.shouldRewrapOnResize = true;
    t.modes.shouldReportAmbiguousWidth = false;
    t.modes.allowsInputMethodEditor = true;
    // xterm's resource defaults (charproc.c): fastScroll and keepSelection
    // on, selectToClipboard off.
    t.modes.shouldScrollFast = true;
    t.modes.shouldKeepSelection = true;
    t.modes.shouldSelectToClipboard = false;
    Ubiquitty::Image::Sixel::defaultPalette(t.sixelColorRegisters);
    // rxvt show/hide (10 toolbar / 30 scrollbar) default shown; more(1) fix (41)
    // and logging (46) off.
    t.toolbarVisible = true;
    t.scrollbarVisible = true;
    t.moreFix = false;
    t.loggingEnabled = false;
    t.sessionLogCaptureEnabled = false;
    // DEC MC print sink: no file installed; DECPEX the scrolling region (the
    // VT510 default), DECPFF off, auto-print off.
    t.printSinkFile = nullptr;
    t.printFormFeed = false;
    t.printExtentFullScreen = false;
    t.autoPrint = false;
    // DECSLPP page memory: one page, page 0 current.
    // DECUDK: no programmed keys, unlocked (RIS clears + unlocks).
    t.udk.reset();
    t.modes.insertMode = false;
    t.modes.reverseVideo = false;
    // Permissive allow-* defaults, so ordinary apps work unconfigured. Hosts gate as needed.
    t.allowTitleOperations = true;
    t.allowFontOperations = true;
    t.allowColorOperations = true;
    t.allowWindowOperations = true;
    t.allowTerminalCapabilityOperations = true;
    t.allowMouseOperations = true;
    t.allowPasteControls = true;
    // Margin bell off by default (10 columns from the right). Armed starts
    // true so the first cursor crossing fires once.
    t.marginBell = false;
    t.marginBellColumn = (t.columns > 10) ? (t.columns - 10) : 0;
    t.marginBellArmed = true;
    // OSC 8 hyperlink table: empty ring, no open link. Ids are 1-based
    // so 0 reads as "no link"; the first interned URI mints id 1. The
    // reclaim pool empties with the cells whose absence justified it: a
    // reconstruct discards every cell, so every id is mintable again from
    // the sequential counter, and a stale pool would only shadow that.
    Ubiquitty::Attributes::Hyperlinks::reset(t);
    // Grapheme-cluster overflow pool: empty (ids restart at 1).
    t.clusterPool.clear();
    // Proportional-run text: empty with the cells whose anchors named it.
    t.runTextStore.clear();
    // OSC 133 command completion: none yet; seed the exit code to "unknown"
    // so a premature read never reads as 0.
    t.lastCommandExitCode = -1;
    // No prompt awaiting a command (row 0 is a valid row, so -1 not 0).
    t.pendingPromptRow = -1;
    // Input characterSet: default UTF-8 pass-through.
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::UTF8);
    // Image-pixel budget default (a 0 budget would evict every image on the
    // first transmit).
    t.images.kittyImageBytesBudget = Image::terminalImageBytesBudgetDefault;
    // Trigger highlight ring empty; autowrap eval-guard is -1 (row 0 must not be
    // mistaken for "already evaluated").
    t.triggerHighlightCount = 0;
    t.triggerHighlightHead = 0;
    t.triggerWrapEvaluatedRow = -1;
    Ubiquitty::Protocol::Tmux::init(&t.integration.tmux);
    // Tek 4014 state: apply non-zero defaults (alpha cursor home, default char
    // size / line style).
    Ubiquitty::tekInit(t);
    if (initialization == TerminalInitialization::StateOnly)
        return;
    // The display owns the cell grid and renderer shadow for this terminal.
    if (!t.grid)
        t.grid = std::make_unique<Ubiquitty::CellGrid>();
    t.grid->init(t.rows, t.columns);
    t.grid->requestFullRedraw();
    // Bind the per-row metadata views to their CellGrid + screen index.
    // VT::screenInit clears owner/screen; rebind now that the
    // CellGrid exists so Ubiquitty::Screen::lineAttribute / lineWrapped resolve through
    // CellGrid::m_rowMeta (the single store of record).
    bindScreenViewsToGrid(t);
}

void setAnswerback(Ubiquitty::Terminal* t, std::string_view utf8)
{
    if (!t)
        return;
    setAnswerbackString(*t, utf8);
}

void setScrollbackLimit(Ubiquitty::Terminal* t, int lines)
{
    if (!t)
        return;
    // A history of fewer than one screen is meaningless; clamp up. The
    // upper bound guards a pathological config value from over-allocating
    // the descriptor ring.
    constexpr int minScrollback = defaultRows;
    constexpr int maxScrollback = 1000000;
    if (lines < minScrollback)
        lines = minScrollback;
    else if (lines > maxScrollback)
        lines = maxScrollback;
    t->scrollbackLimit = lines;
}

void destroyTerminal(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    // Bump the generation counter so any in-flight async-decode
    // jobs detect the disposal in their completion and drop the
    // pixels without dereferencing the now-defunct Terminal.
    t->generation++;
    // Drop what the async queues still hold before the state their completions
    // write into goes away. The queues belong to this Terminal, so no other
    // pane's work is in them and nothing else will drain them.
    VT::discardPendingSixelDecodes(t);
    Ubiquitty::Kitty::discardPendingDecodes(t);
    Ubiquitty::Image::disposeAllImages(*t);
    VT::resetSixelAccumulator(t);
    VT::resetKittyBuffers(t);
    t->osc52Chunk.reset();
    t->osc52ChunkActive = false;
    t->osc52ChunkOverflow = false;
    for (auto& item : t->extendedClipboardWriteItems)
        item.reset();
    t->extendedClipboardSessionAliases.reset();
    t->extendedClipboardServedAliases.reset();
    t->isExtendedClipboardWriteActive = false;
    t->extendedClipboardBase64Tail = {};
    t->extendedClipboardBase64TailLength = 0;
    t->isExtendedClipboardBase64Complete = false;
    t->didExtendedClipboardWriteOverflow = false;
    t->bulkOutput.reset();
    t->lineStore.reset();
    t->triggersEngine.reset();
    t->scrollSearchState.reset();
    t->scrollbackCapacity = 0;
    t->scrollbackCount = 0;
    if (t->sessionLogFile) {
        std::fclose(static_cast<FILE*>(t->sessionLogFile));
        t->sessionLogFile = nullptr;
    }
    if (t->printSinkFile) {
        std::fclose(static_cast<FILE*>(t->printSinkFile));
        t->printSinkFile = nullptr;
    }
    t->grid.reset();
    if (t->parser.osc.data) {
        free(t->parser.osc.data);
        t->parser.osc.data = nullptr;
    }
    t->parser.osc.capacity = t->parser.osc.length = 0;
}

void hardReset(Ubiquitty::Terminal& terminal)
{
    Ubiquitty::DragDrop::reset(terminal);
    terminal.graphicCancellation = {};
    Ubiquitty::Terminal* term = &terminal;
    // Accepted effects retain their immutable owners across the reset. The
    // mutable bank is reset with the terminal; its ordered stop follows every
    // already accepted sound, even when the bounded request ring is full.
    auto pendingSound = std::move(term->pendingSound);
    HardResetPersistentState state = takeHardResetPersistentState(*term);
    term->~Terminal();
    new (term) Ubiquitty::Terminal();
    if (state.grid) {
        // Hand back the live buffers, then blank in place: init reuses backing
        // store that already covers the geometry (no allocation under pressure)
        // and leaves a freshly-blanked grid, which is the RIS postcondition a
        // fresh grid's init provides. A refusal with buffers in hand
        // clamps to what they cover; the Terminal and the grid still agree.
        term->grid = std::move(state.grid);
        // The memo is sized to the old screen: drop its sizing (not its
        // laziness — it re-sizes on first shaped fetch, refusing gracefully),
        // then blank in place over the reused cell buffers.
        term->grid->dropShapedRowCache();
        term->grid->init(state.rows, state.columns);
        // The handoff swapped the grid object initializeTerminal bound the
        // per-row metadata views to — rebind them or every lineAttribute /
        // lineWrapped read follows a freed owner.
        bindScreenViewsToGrid(*term);
    }
    restoreHardResetPersistentState(*term, state);
    term->pendingSound = std::move(pendingSound);
    if (term->pendingSound)
        term->pendingSound->queueResetStop();

    // The frontend cache may still contain colors changed through OSC. The
    // reset restored the configured palette baseline, so force a resync.
    if (auto* client = Ubiquitty::clientForEffectThatEscapesNothing(term->windowClient))
        client->invalidate();
    OSC::notifyMousePointerShape(*term);

    // A sequence still sounding belongs to the program the reset ends: a
    // request at volume 0 on channel 0 stops every DECPS channel.
    if (Ubiquitty::PAL::PlayTonesCallback playTones = Ubiquitty::PAL::playTones())
        playTones(Ubiquitty::PAL::PlayTonesRequest {});
}

void clearBuffer(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    ++t->absoluteRowEpoch;
    if (t->lineStore)
        t->lineStore->clear();
    t->scrollbackCount = 0;
    Ubiquitty::Screen::retireHistory(*t);
    // Trigger highlights are keyed by absolute row; the clear reset
    // scrollbackCount, so drop the now-dangling spans.
    t->triggerHighlightCount = 0;
    t->triggerHighlightHead = 0;
    t->triggerWrapEvaluatedRow = -1;
    // A prompt awaiting its command (A fired, C pending) died with the screen;
    // else the next C would promote a bracket onto the old prompt's row.
    t->pendingPromptRow = -1;
    Ubiquitty::Image::disposeAllImages(*t);
    VT::screenClearDefault(t->screens.active, t->rows, t->columns);
    t->parser.precedingItem.setNone();
    VT::mirrorClearActiveDisplay(*t); // also resets RowMeta (marks, lineAttribute, wrap)
    // The scrollback is gone here, so the two grids hold every cell that can
    // still name a spec — the population the reclaim's mask has to cover.
    Ubiquitty::Screen::reclaimTextSizeSpecs(*t);
    Ubiquitty::Screen::markAllDirty(*t);
}

// Clear like clearBuffer but preserve the cursor's current row — a single-line
// approximation of "preserve the in-progress shell input" (multi-line editors
// lose prior rows until OSC 133 markers land).
void clearBufferPreserveInput(Ubiquitty::Terminal* t)
{
    CellScratchBuffer<maxColumns> rowSnapshotStorage;
    Cell* rowSnapshot = rowSnapshotStorage.data();
    int snapLength;
    int row;
    int c;
    if (!t)
        return;
    ++t->absoluteRowEpoch;
    row = t->screens.active->cursorY;
    if (row < 0)
        row = 0;
    if (row >= t->rows)
        row = t->rows - 1;
    // Capture columns [0, cursorX): the cursor column is the shell's "end of
    // in-progress input", so every cell to its left is preserved VERBATIM
    // (including embedded/trailing spaces — right-trimming would eat the prompt's
    // or the user's trailing whitespace and misplace the cursor).
    snapLength = t->screens.active->cursorX;
    if (snapLength < 0)
        snapLength = 0;
    if (snapLength > t->columns)
        snapLength = t->columns;
    if (snapLength > maxColumns)
        snapLength = maxColumns;
    if (t->grid) {
        CellScratchBuffer<maxColumns> temporaryStorage;
        Cell* temporary = temporaryStorage.data();
        t->grid->readRow(Ubiquitty::Row(row), temporary);
        for (c = 0; c < snapLength; c++)
            rowSnapshotStorage.constructAt(static_cast<std::size_t>(c), temporary[c]);
    }
    const int oldHistory = t->scrollbackCount;
    if (t->lineStore)
        t->lineStore->clear();
    t->scrollbackCount = 0;
    Ubiquitty::Screen::retireHistory(*t);
    // Snapshot the preserved row's marker before the clear wipes all
    // RowMeta; re-applied to its new home (row 0) below so a prompt-mark
    // survives Cmd-K mid-typing.
    const uint8_t cursorRowMark = t->screens.active->mark[row];
    // A prompt awaiting its command (A fired, C pending) rides the preserved
    // input row to row 0; any other pending row is gone with the cleared
    // screen. Without this, the next C would promote the mark onto a stale
    // absolute row (the bracket drifts off the prompt after Cmd-K).
    t->pendingPromptRow = (t->pendingPromptRow == oldHistory + row) ? 0 : -1;
    Ubiquitty::Image::disposeAllImages(*t);
    VT::screenClearDefault(t->screens.active, t->rows, t->columns);
    VT::mirrorClearActiveDisplay(*t); // also resets RowMeta (marks, lineAttribute, wrap)
    // Re-blit the captured cells verbatim into row 0 — including any
    // embedded or trailing spaces — and restore the preserved row's marker.
    if (t->grid) {
        Ubiquitty::RowView span = { rowSnapshot, snapLength };
        t->grid->setRowSpan(Ubiquitty::Row(0), Ubiquitty::Column(0), span);
    }
    t->screens.active->mark[0] = cursorRowMark;
    // The preserved row is a slice of one row of whatever was on screen: a
    // middle band of a block re-blitted alone, a top band whose rows below
    // are gone, or a chain the cursor column cut. Each is a fragment the
    // split rule condemns, and row 0 is where they all land. The reclaim
    // then runs on the same population the plain clear scans — the
    // scrollback went with the clear — so the ids the erasure released come
    // back for the next block instead of leaking until the table refuses.
    Ubiquitty::Screen::scrubBlockDamage(*t, 0, 0);
    Ubiquitty::Screen::reclaimTextSizeSpecs(*t);
    // Cursor lands at the same column the shell had it on, so subsequent input
    // and prompt-aware editing keys see the same column layout.
    t->screens.active->cursorX = snapLength;
    t->screens.active->cursorY = 0;
    t->screens.active->wrapPending = false;
    t->parser.precedingItem.setNone();
    // markAllDirty sets fullRedraw so the next paint clears stale glyph rows.
    Ubiquitty::Screen::markAllDirty(*t);
}

void softReset(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    VT::softResetInternal(*t);
    VT::releaseProgramInputModes(*t);
}

} // namespace Ubiquitty

namespace Ubiquitty::Screen::ScrollSearch {

// Here, beside the terminal that owns the search block, and not in
// ScrollSearch.cpp with the readers it belongs among: the host and agent
// subsets trim that translation unit and keep TerminalFrameInputs.cpp, which
// asks this on every frame. Declared and documented in ScrollSearch.h.
uint32_t activeMatchEpoch(const Ubiquitty::Terminal& t)
{
    return t.scrollSearchState ? t.scrollSearchState->activeEpoch : 0u;
}

} // namespace Ubiquitty::Screen::ScrollSearch

Ubiquitty::Terminal::Terminal()
    : Terminal(TerminalInitialization::Complete)
{
}

Ubiquitty::Terminal::Terminal(TerminalInitialization initialization) { Ubiquitty::initializeTerminal(*this, initialization); }

Ubiquitty::Terminal::~Terminal() { Ubiquitty::destroyTerminal(this); }
