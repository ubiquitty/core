// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/ModeSnapshot.h"

#include "Core/Terminal.h"

namespace Ubiquitty::Runtime {

ModeSnapshot sampleModes(const Ubiquitty::Terminal& term)
{
    static_assert(Ubiquitty::VT::xtermKeyModifierCategoryCount == xtermKeyModifierCategories, "ModeSnapshot xterm arrays must match Core xtermKeyModifierCategoryCount");
    ModeSnapshot modes;
    modes.applicationCursorKeys = term.modes.appCursorKeys;
    modes.applicationKeypad = term.modes.appKeypad;
    modes.shouldReportKeyPositions = term.modes.shouldReportKeyPositions;
    modes.shouldTransmit8BitC1 = term.transmit8bitC1;
    modes.keyPositionReportedModifiers = term.keyPositionReportedModifiers;
    modes.keyPositionDisabledModifiers = term.keyPositionDisabledModifiers;
    modes.shouldHonorNumLock = term.modes.shouldHonorNumLock;
    modes.keyboardType = term.modes.keyboardType;
    modes.termcapProfile = term.modes.termcapProfile;
    modes.shouldReportPasteEvents = term.modes.shouldReportPasteEvents;
    modes.bracketedPaste = term.modes.bracketedPaste;
    modes.sendReceiveMode = term.modes.srm;
    modes.vt52Mode = term.modes.vt52Mode;
    modes.lineFeedNewLineMode = term.modes.lnm;
    modes.autoRepeat = term.modes.autoRepeat;
    modes.metaSendsEscape = term.modes.metaSendsEscape;
    modes.allowsInputMethodEditor = term.modes.allowsInputMethodEditor;
    modes.alternateSendsEscape = term.modes.alternateSendsEscape;
    modes.eightBitInput = term.modes.eightBitInput;
    modes.deleteKeySendsDelete = term.modes.deleteKeySendsDelete;
    modes.isFullscreenContentActive = term.modes.fullscreenContent;
    modes.userDefinedKeyCount = term.udk ? term.udk->count : 0;
    modes.alternateScreenActive = term.screens.isUsingAlternateScreen;
    modes.isScrollbarVisible = term.scrollbarVisible;
    modes.isPasswordPromptActive = term.passwordPromptActive;
    modes.isScreenBlanked = term.screenBlanked;
    modes.kittyKeyboardFlags[0] = term.kittyKeyboardFlags[0];
    modes.kittyKeyboardFlags[1] = term.kittyKeyboardFlags[1];
    modes.xtermKeyModifiers = term.xtermKeyModifiers;
    modes.xtermKeyFormats = term.xtermKeyFormats;
    return modes;
}

const VT::UserDefinedKeys* sampleUserDefinedKeys(const Ubiquitty::Terminal& term)
{
    return Ubiquitty::userDefinedKeysOrNull(term);
}

} // namespace Ubiquitty::Runtime
