// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/ScreenPlane.h"

#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellContent.h"
#include "Core/CellGrid.h"
#include "Core/ClusterPool.h"
#include "Core/Image/Sixel/Decoder.h"
#include "Core/OSC/Activity.h"
#include "Core/Protocol/Sync/CheckpointLedger.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/RichTextForm.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/RunTextStore.h"
#include "Core/Screen/ScrollLine.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/TextSizeTable.h"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace Ubiquitty::Protocol::Sync {

namespace {

using Ubiquitty::Terminal;

// ---- Field transfer ---------------------------------------------------
//
// A block is spelled once, as a list of member pairs a visitor sees. Encoding
// visits the host terminal with itself and writes each member; decoding
// visits a scratch terminal with itself and reads each member; committing
// visits the client and the scratch terminal and assigns. One list per block
// means a member cannot travel one way and not the other.

void put(WireWriter& writer, bool value) { writer.u8(value ? 1 : 0); } // bool-ok: the codec of the bool members a block carries

void take(WireReader& reader, bool& value)
{
    const std::uint8_t byte = reader.u8();
    if (byte > 1)
        reader.fail();
    value = byte == 1;
}

template<std::unsigned_integral Integer>
void put(WireWriter& writer, Integer value) { writer.varint(value); }

template<std::unsigned_integral Integer>
void take(WireReader& reader, Integer& value) { value = static_cast<Integer>(reader.varint(std::numeric_limits<Integer>::max())); }

template<std::signed_integral Integer>
void put(WireWriter& writer, Integer value) { writer.svarint(value); }

template<std::signed_integral Integer>
void take(WireReader& reader, Integer& value)
{
    const std::int64_t wide = reader.svarint();
    if (wide < std::numeric_limits<Integer>::min() || wide > std::numeric_limits<Integer>::max()) {
        reader.fail();
        return;
    }
    value = static_cast<Integer>(wide);
}

// The enumerators a peer may name. A value outside them refuses the body
// rather than landing in a field no switch over the type handles.
constexpr bool isKnown(Ubiquitty::Unicode::Width::AmbiguousWidth value)
{
    return value == Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow || value == Ubiquitty::Unicode::Width::AmbiguousWidth::Wide;
}
constexpr bool isKnown(CharacterTabAlignment value) { return std::to_underlying(value) <= std::to_underlying(CharacterTabAlignment::TargetCharacter); }
constexpr bool isKnown(Ubiquitty::VT::Emulation value) { return std::to_underlying(value) <= std::to_underlying(Ubiquitty::VT::Emulation::Hp); }
constexpr bool isKnown(Ubiquitty::VT::ConformanceLevel value)
{
    return std::to_underlying(value) >= std::to_underlying(Ubiquitty::VT::ConformanceLevel::VT100)
        && std::to_underlying(value) <= std::to_underlying(Ubiquitty::VT::ConformanceLevel::VT500);
}
constexpr bool isKnown(Ubiquitty::VT::ProtectedMode value) { return std::to_underlying(value) <= std::to_underlying(Ubiquitty::VT::ProtectedMode::Both); }
constexpr bool isKnown(Ubiquitty::Cursor::Style value) { return std::to_underlying(value) <= std::to_underlying(Ubiquitty::Cursor::Style::SteadyBar); }
constexpr bool isKnown(Ubiquitty::MousePointerShape value) { return std::to_underlying(value) <= std::to_underlying(Ubiquitty::MousePointerShape::NorthwestResize); }
constexpr bool isKnown(Ubiquitty::FullscreenContentFit value) { return std::to_underlying(value) <= std::to_underlying(Ubiquitty::FullscreenContentFit::Scale); }
constexpr bool isKnown(Ubiquitty::PrecedingItem::Kind value) { return std::to_underlying(value) <= std::to_underlying(Ubiquitty::PrecedingItem::Kind::CSISequence); }
constexpr bool isKnown(Ubiquitty::FullscreenContentLetterbox value) { return std::to_underlying(value) <= std::to_underlying(Ubiquitty::FullscreenContentLetterbox::Passthrough); }
constexpr bool isKnown(Ubiquitty::Keys::KeyboardType value) { return std::to_underlying(value) <= std::to_underlying(Ubiquitty::Keys::KeyboardType::VT220); }
constexpr bool isKnown(Ubiquitty::Keys::TermcapProfile value) { return std::to_underlying(value) <= std::to_underlying(Ubiquitty::Keys::TermcapProfile::Linux); }

template<typename Enumeration>
    requires std::is_enum_v<Enumeration>
void put(WireWriter& writer, Enumeration value)
{
    writer.varint(static_cast<std::uint64_t>(std::to_underlying(value)));
}

template<typename Enumeration>
    requires std::is_enum_v<Enumeration>
void take(WireReader& reader, Enumeration& value)
{
    using Underlying = std::underlying_type_t<Enumeration>;
    const auto raw = static_cast<Underlying>(reader.varint(std::numeric_limits<Underlying>::max()));
    if (!isKnown(static_cast<Enumeration>(raw))) {
        reader.fail();
        return;
    }
    value = static_cast<Enumeration>(raw);
}

void put(WireWriter& writer, const Ubiquitty::Modes::AlternateScrollMode& value) { put(writer, value.asRequested()); }

void put(WireWriter& writer, Ubiquitty::VT::ChecksumExtensions value) { writer.u8(value.toRaw()); }

void take(WireReader& reader, Ubiquitty::VT::ChecksumExtensions& value)
{
    const std::uint8_t raw = reader.u8();
    if (raw > Ubiquitty::VT::checksumSupportedExtensions.toRaw())
        reader.fail();
    value = Ubiquitty::VT::ChecksumExtensions::fromRaw(raw);
}

void take(WireReader& reader, Ubiquitty::Modes::AlternateScrollMode& value)
{
    bool isRequested = false;
    take(reader, isRequested);
    value = isRequested;
}

void put(WireWriter& writer, const PaletteRGB& value)
{
    writer.u16(value.red);
    writer.u16(value.green);
    writer.u16(value.blue);
}

void take(WireReader& reader, PaletteRGB& value)
{
    value.red = reader.u16();
    value.green = reader.u16();
    value.blue = reader.u16();
}

void put(WireWriter& writer, const Pen& value) { writePen(writer, value); }
void take(WireReader& reader, Pen& value) { value = readPen(reader); }

// The longest string a block carries: a title, by the Core's own ceiling.
constexpr std::size_t maxBlockStringLength = Terminal::maxTitleLength;

void put(WireWriter& writer, const FallibleString& value)
{
    writer.bytes({ reinterpret_cast<const std::uint8_t*>(value.data()), value.size() });
}

template<typename Element, std::size_t count>
void put(WireWriter& writer, const std::array<Element, count>& values)
{
    for (const Element& value : values)
        put(writer, value);
}

template<typename Element, std::size_t count>
void take(WireReader& reader, std::array<Element, count>& values)
{
    for (Element& value : values)
        take(reader, value);
}

template<typename Element, std::size_t count>
void put(WireWriter& writer, const Element (&values)[count])
{
    for (const Element& value : values)
        put(writer, value);
}

template<typename Element, std::size_t count>
void take(WireReader& reader, Element (&values)[count])
{
    for (Element& value : values)
        take(reader, value);
}

// A sequence written as the elements that differ from a reference both ends
// hold: a gap-coded list of (index, element). A palette a theme set, the sixel
// registers and the tab stops are mostly their defaults.
template<typename Element>
void putDelta(WireWriter& writer, std::span<const Element> value, std::span<const Element> reference)
{
    std::size_t count = 0;
    for (std::size_t index = 0; index < value.size(); ++index)
        count += value[index] == reference[index] ? 0 : 1;
    writer.varint(count);
    std::size_t previous = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == reference[index])
            continue;
        writer.varint(index - previous);
        previous = index;
        put(writer, value[index]);
    }
}

template<typename Element>
void takeDelta(WireReader& reader, std::span<Element> value, std::span<const Element> reference)
{
    std::ranges::copy(reference, value.begin());
    const std::size_t count = reader.count(value.size(), 2);
    std::size_t index = 0;
    for (std::size_t entry = 0; entry < count && reader.ok(); ++entry) {
        const std::uint64_t gap = reader.varint(value.size());
        // Entries ascend strictly, so only the first may sit at a gap of zero.
        if (entry > 0 && gap == 0)
            reader.fail();
        index += static_cast<std::size_t>(gap);
        if (index >= value.size()) {
            reader.fail();
            break;
        }
        take(reader, value[index]);
    }
}

// The palette a terminal starts with, which a live palette is written against.
TermPalette seededPalette()
{
    TermPalette palette {};
    palette.seedDefaults();
    return palette;
}

void putPalette(WireWriter& writer, const TermPalette& value, const TermPalette& reference)
{
    putDelta<PaletteRGB>(writer, value.indexed, reference.indexed);
    put(writer, value.defaultForeground);
    put(writer, value.defaultBackground);
    put(writer, value.cursor);
    putDelta<std::uint8_t>(writer, value.indexedAlpha, reference.indexedAlpha);
    put(writer, value.special);
    put(writer, value.specialSet);
}

void takePalette(WireReader& reader, TermPalette& value, const TermPalette& reference)
{
    takeDelta<PaletteRGB>(reader, value.indexed, reference.indexed);
    take(reader, value.defaultForeground);
    take(reader, value.defaultBackground);
    take(reader, value.cursor);
    takeDelta<std::uint8_t>(reader, value.indexedAlpha, reference.indexedAlpha);
    take(reader, value.special);
    take(reader, value.specialSet);
}

constexpr std::array<CharacterTabAlignment, maxColumns> noTabAlignments {};
constexpr std::array<std::uint32_t, maxColumns> noTabAlignmentTargets {};

// Sync encodes tab stops as boolean deltas, independently of their packed storage.
template<std::size_t count>
void putTabStops(WireWriter& writer, const std::bitset<count>& stops)
{
    if (stops.none()) {
        writer.varint(0);
        return;
    }
    constexpr std::array<bool, count> noStops {};
    std::array<bool, count> expanded {};
    for (std::size_t row = 0; row < expanded.size(); ++row)
        expanded[row] = stops[row];
    putDelta<bool>(writer, expanded, noStops);
}

template<std::size_t count>
void takeTabStops(WireReader& reader, std::bitset<count>& stops)
{
    constexpr std::array<bool, count> noStops {};
    std::array<bool, count> expanded {};
    takeDelta<bool>(reader, expanded, noStops);
    stops.reset();
    for (std::size_t row = 0; row < expanded.size(); ++row)
        stops[row] = expanded[row];
}

// The sixel registers a terminal starts with.
std::array<std::uint8_t, Ubiquitty::Image::Sixel::paletteBytes> defaultSixelRegisters()
{
    std::array<std::uint8_t, Ubiquitty::Image::Sixel::paletteBytes> registers {};
    Ubiquitty::Image::Sixel::defaultPalette(registers);
    return registers;
}

void put(WireWriter& writer, const FullscreenContentDisplay& value)
{
    put(writer, value.fit);
    put(writer, value.aspectWidth);
    put(writer, value.aspectHeight);
    put(writer, value.scaleMultiplier);
    put(writer, value.letterbox);
    put(writer, value.letterboxColor);
}

void take(WireReader& reader, FullscreenContentDisplay& value)
{
    take(reader, value.fit);
    take(reader, value.aspectWidth);
    take(reader, value.aspectHeight);
    take(reader, value.scaleMultiplier);
    take(reader, value.letterbox);
    take(reader, value.letterboxColor);
}

// A visitor that writes the first member of each pair.
struct EncodeVisit {
    WireWriter& writer;
    template<typename Value>
    void operator()(const Value& value, const Value&) const { put(writer, value); }
};

// A visitor that reads into the first member of each pair.
struct DecodeVisit {
    WireReader& reader;
    bool* didRefuseAllocation = nullptr;
    template<typename Value>
    void operator()(Value& value, Value&) const { take(reader, value); }
    void operator()(FallibleString& value, FallibleString&) const
    {
        const auto bytes = reader.bytes(maxBlockStringLength);
        if (!reader.ok())
            return;
        if (!value.tryAssign(std::string_view { reinterpret_cast<const char*>(bytes.data()), bytes.size() })) {
            if (didRefuseAllocation)
                *didRefuseAllocation = true;
            reader.fail();
        }
    }
    template<typename Element, std::size_t count>
    void operator()(std::array<Element, count>& values, std::array<Element, count>&) const
    {
        for (auto& value : values)
            (*this)(value, value);
    }
};

bool addStorageCount(std::size_t& destination, std::size_t count)
{
    if (count > std::numeric_limits<std::size_t>::max() - destination)
        return false;
    destination += count;
    return true;
}

template<typename Value>
void takePlanned(WireReader& reader, Value& value, CheckpointStoragePlan&)
{
    take(reader, value);
}

void takePlanned(WireReader& reader, FallibleString&, CheckpointStoragePlan& plan)
{
    const auto bytes = reader.bytes(maxBlockStringLength);
    if (!addStorageCount(plan.stateStringBytes, bytes.size())
        || (!bytes.empty() && !addStorageCount(plan.stateStringCount, 1)))
        reader.fail();
}

template<typename Element, std::size_t count>
void takePlanned(WireReader& reader, std::array<Element, count>& values, CheckpointStoragePlan& plan)
{
    for (auto& value : values)
        takePlanned(reader, value, plan);
}

struct PlanningVisit {
    WireReader& reader;
    CheckpointStoragePlan& plan;
    template<typename Value>
    void operator()(Value& value, Value&) const { takePlanned(reader, value, plan); }
};

// Consumes validated scratch members; owned strings must not allocate again
// after destination mutation has begun.
struct CommitVisit {
    template<typename Value>
    void operator()(Value& to, Value& from) const noexcept
    {
        static_assert(std::is_nothrow_move_assignable_v<Value>);
        to = std::move(from);
    }
};

// ---- Blocks -----------------------------------------------------------

constexpr bool Terminal::Modes::* modeFlags[] = {
    &Terminal::Modes::isCursorVisible,
    &Terminal::Modes::autowrap,
    &Terminal::Modes::appCursorKeys,
    &Terminal::Modes::appKeypad,
    &Terminal::Modes::bracketedPaste,
    &Terminal::Modes::focusReporting,
    &Terminal::Modes::allowAlternateScreen,
    &Terminal::Modes::scrollOnOutput,
    &Terminal::Modes::scrollOnKeypress,
    &Terminal::Modes::colorThemeReporting,
    &Terminal::Modes::originMode,
    &Terminal::Modes::reverseWrapExtended,
    &Terminal::Modes::keyboardLocked,
    &Terminal::Modes::lnm,
    &Terminal::Modes::srm,
    &Terminal::Modes::insertMode,
    &Terminal::Modes::reverseVideo,
    &Terminal::Modes::syncOutput,
    &Terminal::Modes::vt52Mode,
    &Terminal::Modes::vt52Graphics,
    &Terminal::Modes::sixelDisplayMode,
    &Terminal::Modes::clearScreenOnColumnChange,
    &Terminal::Modes::columns132,
    &Terminal::Modes::leftRightMarginMode,
    &Terminal::Modes::mouseX10,
    &Terminal::Modes::mouseButtonTracking,
    &Terminal::Modes::mouseSGR,
    &Terminal::Modes::mouseCellMotion,
    &Terminal::Modes::mouseAnyEvent,
    &Terminal::Modes::mouseUTF8Coordinates,
    &Terminal::Modes::mouseURXVT,
    &Terminal::Modes::mouseSGRPixels,
    &Terminal::Modes::mouseRelativePointerLock,
    &Terminal::Modes::mouseHilite,
    &Terminal::Modes::hiliteRegionValid,
    &Terminal::Modes::hiliteAwaiting,
    &Terminal::Modes::reverseWrap,
    &Terminal::Modes::smoothScroll,
    &Terminal::Modes::autoRepeat,
    &Terminal::Modes::nrcMode,
    &Terminal::Modes::backspaceKeySendsBackspace,
    &Terminal::Modes::metaSendsEscape,
    &Terminal::Modes::alternateSendsEscape,
    &Terminal::Modes::eightBitInput,
    &Terminal::Modes::deleteKeySendsDelete,
    &Terminal::Modes::graphemeClustering,
    &Terminal::Modes::fontShifting,
    &Terminal::Modes::keepClipboard,
    &Terminal::Modes::inBandResize,
    &Terminal::Modes::fullscreenContent,
    &Terminal::Modes::allow80To132,
    &Terminal::Modes::shouldEraseToDefaultColors,
    &Terminal::Modes::hasPrivateSixelColorRegisters,
    &Terminal::Modes::shouldLeaveCursorRightOfSixel,
    &Terminal::Modes::shouldRewrapOnResize,
    &Terminal::Modes::shouldReportAmbiguousWidth,
    &Terminal::Modes::allowsInputMethodEditor,
};

constexpr bool Terminal::* terminalModeFlags[] = {
    &Terminal::transmit8bitC1,
    &Terminal::cursorBlinkAtt610,
    &Terminal::marginBell,
    &Terminal::moreFix,
    &Terminal::loggingEnabled,
    &Terminal::toolbarVisible,
    &Terminal::scrollbarVisible,
    &Terminal::bellRaisesUrgency,
    &Terminal::popOnBell,
    &Terminal::printFormFeed,
    &Terminal::printExtentFullScreen,
    &Terminal::autoPrint,
    &Terminal::locatorEnabled,
    &Terminal::locatorOneShot,
    &Terminal::locatorUseRows,
    &Terminal::locatorReportDown,
    &Terminal::locatorReportUp,
    &Terminal::locatorFilterActive,
    &Terminal::screenBlanked,
    &Terminal::passwordPromptActive,
    &Terminal::isTekModeActive,
};

// The modes block: every DEC and ANSI mode, the input-encoding state, and the
// device state that answers a report.
template<typename Visit, typename Destination, typename Source>
void modesBlock(Visit&& visit, Destination& a, Source& b)
{
    for (const auto flag : modeFlags)
        visit(a.modes.*flag, b.modes.*flag);
    visit(a.modes.sace, b.modes.sace);
    visit(a.modes.syncOutputTouchTicks, b.modes.syncOutputTouchTicks);
    visit(a.modes.hiliteStartX, b.modes.hiliteStartX);
    visit(a.modes.hiliteStartY, b.modes.hiliteStartY);
    visit(a.modes.hiliteFirstRow, b.modes.hiliteFirstRow);
    visit(a.modes.hiliteLastRow, b.modes.hiliteLastRow);
    visit(a.modes.hiliteDeadlineTick, b.modes.hiliteDeadlineTick);
    visit(a.modes.alternateScrollMode, b.modes.alternateScrollMode);
    for (const auto flag : terminalModeFlags)
        visit(a.*flag, b.*flag);
    visit(a.ambiguousWidth, b.ambiguousWidth);
    visit(a.emulation, b.emulation);
    visit(a.conformanceLevel, b.conformanceLevel);
    visit(a.protectedMode, b.protectedMode);
    visit(a.checksumExtensions, b.checksumExtensions);
    visit(a.kittyKeyboardFlags, b.kittyKeyboardFlags);
    visit(a.kittyKeyboardStack, b.kittyKeyboardStack);
    visit(a.kittyKeyboardStackLength, b.kittyKeyboardStackLength);
    visit(a.xtermKeyModifiers, b.xtermKeyModifiers);
    visit(a.xtermKeyFormats, b.xtermKeyFormats);
    constexpr auto numLockMode = &Terminal::Modes::shouldHonorNumLock;
    visit(a.modes.*numLockMode, b.modes.*numLockMode);
    visit(a.modes.keyboardType, b.modes.keyboardType);
    visit(a.modes.termcapProfile, b.modes.termcapProfile);
    visit(a.savedPrivateModeNumbers, b.savedPrivateModeNumbers);
    visit(a.savedPrivateModeStates, b.savedPrivateModeStates);
    visit(a.titleModes, b.titleModes);
    visit(a.savedRenditionMasks, b.savedRenditionMasks);
    visit(a.savedRenditionPens, b.savedRenditionPens);
    visit(a.savedRenditionCount, b.savedRenditionCount);
    visit(a.marginBellColumn, b.marginBellColumn);
    visit(a.printMode, b.printMode);
    visit(a.locatorFilterTop, b.locatorFilterTop);
    visit(a.locatorFilterLeft, b.locatorFilterLeft);
    visit(a.locatorFilterBottom, b.locatorFilterBottom);
    visit(a.locatorFilterRight, b.locatorFilterRight);
    visit(a.screenBlankTimeout, b.screenBlankTimeout);
    visit(a.vesaPowerdownTimeout, b.vesaPowerdownTimeout);
    visit(a.statusLineType, b.statusLineType);
    visit(a.statusLineActive, b.statusLineActive);
    visit(a.statusCursorX, b.statusCursorX);
    visit(a.fullscreenContentDisplay, b.fullscreenContentDisplay);
    visit(a.tekBrightPenOverride, b.tekBrightPenOverride);
    visit(a.modes.shouldQuotePasteCharacters, b.modes.shouldQuotePasteCharacters);
    visit(a.modes.shouldPreservePasteNewlines, b.modes.shouldPreservePasteNewlines);
    visit(a.modes.shouldMoveReadlinePointOnClick, b.modes.shouldMoveReadlinePointOnClick);
    visit(a.modes.shouldMoveReadlinePointBeforePaste, b.modes.shouldMoveReadlinePointBeforePaste);
    visit(a.modes.shouldDeleteReadlineSelection, b.modes.shouldDeleteReadlineSelection);
    visit(a.shouldIgnorePrinterNUL, b.shouldIgnorePrinterNUL);
    visit(a.isStatusNationalReplacementEnabled, b.isStatusNationalReplacementEnabled);
    visit(a.shouldCopyRightToLeft, b.shouldCopyRightToLeft);
    visit(a.shouldWriteRightToLeft, b.shouldWriteRightToLeft);
    visit(a.modes.shouldReportPasteEvents, b.modes.shouldReportPasteEvents);
    visit(a.modes.shouldSaveCRT, b.modes.shouldSaveCRT);
    visit(a.isCRTBlanked, b.isCRTBlanked);
    visit(a.modes.shouldReportKeyPositions, b.modes.shouldReportKeyPositions);
    visit(a.keyPositionReportedModifiers, b.keyPositionReportedModifiers);
    visit(a.keyPositionDisabledModifiers, b.keyPositionDisabledModifiers);
    visit(a.modes.isHorizontalCursorCoupled, b.modes.isHorizontalCursorCoupled);
    visit(a.modes.isVerticalCursorUncoupled, b.modes.isVerticalCursorUncoupled);
    visit(a.modes.isPageCursorUncoupled, b.modes.isPageCursorUncoupled);
}

// The xterm dynamic colours, which follow the two palettes in the palette
// block.
template<typename Visit, typename Destination, typename Source>
void dynamicColourFields(Visit&& visit, Destination& a, Source& b)
{
    constexpr std::uint16_t Terminal::* channels[] = {
        &Terminal::selectionBackgroundRed,
        &Terminal::selectionBackgroundGreen,
        &Terminal::selectionBackgroundBlue,
        &Terminal::selectionForegroundRed,
        &Terminal::selectionForegroundGreen,
        &Terminal::selectionForegroundBlue,
        &Terminal::mousePointerForegroundRed,
        &Terminal::mousePointerForegroundGreen,
        &Terminal::mousePointerForegroundBlue,
        &Terminal::mousePointerBackgroundRed,
        &Terminal::mousePointerBackgroundGreen,
        &Terminal::mousePointerBackgroundBlue,
        &Terminal::tektronixForegroundRed,
        &Terminal::tektronixForegroundGreen,
        &Terminal::tektronixForegroundBlue,
        &Terminal::tektronixBackgroundRed,
        &Terminal::tektronixBackgroundGreen,
        &Terminal::tektronixBackgroundBlue,
        &Terminal::tektronixCursorRed,
        &Terminal::tektronixCursorGreen,
        &Terminal::tektronixCursorBlue,
    };
    for (const auto channel : channels)
        visit(a.*channel, b.*channel);
    constexpr bool Terminal::* setFlags[] = {
        &Terminal::selectionBackgroundSet,
        &Terminal::selectionForegroundSet,
        &Terminal::mousePointerForegroundSet,
        &Terminal::mousePointerBackgroundSet,
        &Terminal::tektronixForegroundSet,
        &Terminal::tektronixBackgroundSet,
        &Terminal::tektronixCursorSet,
    };
    for (const auto flag : setFlags)
        visit(a.*flag, b.*flag);
}

// kitty's cursor_text colour, which follows the xterm dynamic colours in the
// palette block.
template<typename Visit, typename Destination, typename Source>
void cursorTextColourFields(Visit&& visit, Destination& a, Source& b)
{
    visit(a.cursorTextRed, b.cursorTextRed);
    visit(a.cursorTextGreen, b.cursorTextGreen);
    visit(a.cursorTextBlue, b.cursorTextBlue);
    visit(a.hasCursorTextColor, b.hasCursorTextColor);
}

Ubiquitty::Screen::State& screenState(Terminal& terminal, ScreenMode screen) { return screen == ScreenMode::Alternate ? terminal.screens.alternate : terminal.screens.primary; }
const Ubiquitty::Screen::State& screenState(const Terminal& terminal, ScreenMode screen) { return screen == ScreenMode::Alternate ? terminal.screens.alternate : terminal.screens.primary; }

ScreenMode activeScreenOf(const Terminal& terminal) { return terminal.screens.isUsingAlternateScreen ? ScreenMode::Alternate : ScreenMode::Primary; }

// The cursor block: where the active screen's cursor is and how it is drawn.
template<typename Visit, typename Destination, typename Source>
void cursorBlock(Visit&& visit, Destination& a, Source& b, ScreenMode active)
{
    auto& to = screenState(a, active);
    auto& from = screenState(b, active);
    visit(to.cursorX, from.cursorX);
    visit(to.cursorY, from.cursorY);
    visit(to.wrapPending, from.wrapPending);
    visit(a.cursorStyle, b.cursorStyle);
}

// Where a screen's cursor travels: the active screen's in the cursor block,
// the inactive screen's beside its margins.
enum class CursorPlacement : std::uint8_t {
    CursorBlock,
    ScreenState,
};

// One screen's margins, pen and saved cursor; the inactive screen's cursor
// with them, since the cursor block holds only the active one's.
template<typename Visit, typename Destination, typename Source>
void screenStateFields(Visit&& visit, Destination& to, Source& from, CursorPlacement cursor)
{
    if (cursor == CursorPlacement::ScreenState) {
        visit(to.cursorX, from.cursorX);
        visit(to.cursorY, from.cursorY);
        visit(to.wrapPending, from.wrapPending);
    }
    visit(to.scrollTop, from.scrollTop);
    visit(to.scrollBottom, from.scrollBottom);
    visit(to.scrollLeft, from.scrollLeft);
    visit(to.scrollRight, from.scrollRight);
    visit(to.rememberedMarginCount, from.rememberedMarginCount);
    for (std::size_t index = 0; index < std::size(to.rememberedMargins); ++index) {
        visit(to.rememberedMargins[index].left, from.rememberedMargins[index].left);
        visit(to.rememberedMargins[index].right, from.rememberedMargins[index].right);
    }
    visit(to.pen, from.pen);
    visit(to.savedX, from.savedX);
    visit(to.savedY, from.savedY);
    visit(to.savedPen, from.savedPen);
    visit(to.savedCharacterSetActive, from.savedCharacterSetActive);
    visit(to.savedCharacterSetGR, from.savedCharacterSetGR);
    visit(to.savedCharacterSetG0, from.savedCharacterSetG0);
    visit(to.savedCharacterSetG1, from.savedCharacterSetG1);
    visit(to.savedCharacterSetG2, from.savedCharacterSetG2);
    visit(to.savedCharacterSetG3, from.savedCharacterSetG3);
    visit(to.savedAutowrap, from.savedAutowrap);
    visit(to.savedOriginMode, from.savedOriginMode);
    visit(to.savedWrapPending, from.savedWrapPending);
    visit(to.savedScrollTop, from.savedScrollTop);
    visit(to.savedScrollBottom, from.savedScrollBottom);
    visit(to.hasSavedCursor, from.hasSavedCursor);
}

template<typename Visit, typename Destination, typename Source>
void screenStatesBlock(Visit&& visit, Destination& a, Source& b, ScreenMode active)
{
    const auto placement = [active](ScreenMode screen) { return screen == active ? CursorPlacement::CursorBlock : CursorPlacement::ScreenState; };
    screenStateFields(visit, a.screens.primary, b.screens.primary, placement(ScreenMode::Primary));
    screenStateFields(visit, a.screens.alternate, b.screens.alternate, placement(ScreenMode::Alternate));
}

// The character and text state. The sixel registers, the tab stops, the
// user-defined keys, the text-size table and the status row are written after
// it by hand: their shapes are not fields.
template<typename Visit, typename Destination, typename Source>
void textStateFields(Visit&& visit, Destination& a, Source& b)
{
    visit(a.characterSetG, b.characterSetG);
    visit(a.characterSetActive, b.characterSetActive);
    visit(a.characterSetGR, b.characterSetGR);
    visit(a.singleShift, b.singleShift);
    visit(a.currentLinkID, b.currentLinkID);
    visit(a.titleUTF8, b.titleUTF8);
    visit(a.iconNameUTF8, b.iconNameUTF8);
    visit(a.titleUTF8Stack, b.titleUTF8Stack);
    visit(a.iconNameUTF8Stack, b.iconNameUTF8Stack);
    visit(a.titleStackLength, b.titleStackLength);
    visit(a.titleStackValid, b.titleStackValid);
    visit(a.iconNameStackValid, b.iconNameStackValid);
    visit(a.mousePointerShapeStack, b.mousePointerShapeStack);
    visit(a.mousePointerShapeStackLength, b.mousePointerShapeStackLength);
    visit(a.alternateMousePointerShapeStack, b.alternateMousePointerShapeStack);
    visit(a.alternateMousePointerShapeStackLength, b.alternateMousePointerShapeStackLength);
    visit(a.shouldEndSemanticRoleAtLineBreak, b.shouldEndSemanticRoleAtLineBreak);
    visit(a.lineHome, b.lineHome);
}

// ---- Validation -------------------------------------------------------

bool isWithin(int value, int low, int high) { return value >= low && value <= high; }

bool isValidModes(const Terminal& t, int columns)
{
    for (int screen = 0; screen < 2; ++screen) {
        if (t.kittyKeyboardStackLength[static_cast<std::size_t>(screen)] > Ubiquitty::Kitty::maxKeyStackDepth)
            return false;
    }
    return t.modes.sace <= 2 && isWithin(t.marginBellColumn, 0, columns) && t.statusLineType <= 2 && t.statusLineActive <= 1
        && isWithin(t.statusCursorX, 0, columns) && t.tekBrightPenOverride <= 2
        && isKnown(t.modes.keyboardType)
        && isKnown(t.modes.termcapProfile);
}

bool isValidScreenState(const Ubiquitty::Screen::State& state, int rows, int columns)
{
    const auto isCharacterSetIndex = [](int value) { return isWithin(value, 0, 3); };
    // A designated set is a small id, or the soft font's; any of them a byte.
    const auto isCharacterSet = [](int value) { return isWithin(value, 0, 255); };
    if (!isWithin(state.cursorX, 0, columns - 1) || !isWithin(state.cursorY, 0, rows - 1))
        return false;
    if (!isWithin(state.scrollTop, 0, rows - 1) || !isWithin(state.scrollBottom, state.scrollTop, rows - 1))
        return false;
    if (!isWithin(state.scrollLeft, 0, columns - 1) || !isWithin(state.scrollRight, state.scrollLeft, columns - 1))
        return false;
    if (!isWithin(state.rememberedMarginCount, 0, static_cast<int>(std::size(state.rememberedMargins))))
        return false;
    // A resize may leave the saved cursor and margins past the screen: a
    // later DECRC clamps the cursor to the screen then (Screen/Cursor.cpp),
    // and applies no saved margin. So the saved fields are bounded by the
    // largest screen, not this one; a value there is a terminal's state and
    // not a malformed one.
    if (!isWithin(state.savedX, 0, Ubiquitty::maxColumns - 1) || !isWithin(state.savedY, 0, Ubiquitty::maxRows - 1))
        return false;
    if (!isWithin(state.savedScrollTop, 0, Ubiquitty::maxRows - 1) || !isWithin(state.savedScrollBottom, 0, Ubiquitty::maxRows - 1))
        return false;
    return isCharacterSetIndex(state.savedCharacterSetActive) && isCharacterSetIndex(state.savedCharacterSetGR)
        && isCharacterSet(state.savedCharacterSetG0) && isCharacterSet(state.savedCharacterSetG1)
        && isCharacterSet(state.savedCharacterSetG2) && isCharacterSet(state.savedCharacterSetG3);
}

bool isValidPrecedingItem(const Ubiquitty::PrecedingItem& item)
{
    const auto isParameter = [](std::uint32_t value) { return value <= static_cast<std::uint32_t>(Ubiquitty::VT::maxParameterValue); };
    return isWithin(static_cast<int>(item.multiplier), 1, static_cast<int>(Ubiquitty::PrecedingItem::maxRepeatEffects))
        && isWithin(item.parameterCount, 0, Ubiquitty::VT::maxParameters) && std::ranges::all_of(item.parameters, isParameter)
        && std::ranges::all_of(item.parameterSeparatorWasColon, [](std::uint8_t flag) { return flag <= 1; });
}

// The parser's ground state, which a checkpoint carries in a section of its
// own after the history's address: the REP record, what CSI b repeats. It changes with
// every character printed, so no update block holds it; a parser continuing
// from a checkpoint taken at ground reads it (Docs/sync.md §11).
template<typename Visit, typename Destination, typename Source>
void parserGroundFields(Visit&& visit, Destination& to, Source& from)
{
    visit(to.kind, from.kind);
    visit(to.multiplier, from.multiplier);
    visit(to.codePoint, from.codePoint);
    visit(to.resolvedCodePoint, from.resolvedCodePoint);
    visit(to.attachedSelector, from.attachedSelector);
    visit(to.controlByte, from.controlByte);
    visit(to.escIntermediate, from.escIntermediate);
    visit(to.drcsIntermediate, from.drcsIntermediate);
    visit(to.privatePrefix, from.privatePrefix);
    visit(to.intermediate, from.intermediate);
    visit(to.parameterCount, from.parameterCount);
    visit(to.parameters, from.parameters);
    visit(to.parameterSeparatorWasColon, from.parameterSeparatorWasColon);
}

bool isValidTextState(const Terminal& t)
{
    for (const int set : t.characterSetG) {
        if (!isWithin(set, 0, 255))
            return false;
    }
    return isWithin(t.lineHome, 0, maxColumns - 1) && isWithin(t.characterSetActive, 0, 3) && isWithin(t.characterSetGR, 0, 3)
        && (t.singleShift == 0 || t.singleShift == 2 || t.singleShift == 3)
        && t.titleStackLength <= t.titleUTF8Stack.size()
        && t.mousePointerShapeStackLength <= t.mousePointerShapeStack.size()
        && t.alternateMousePointerShapeStackLength <= t.alternateMousePointerShapeStack.size();
}

// ---- Rows -------------------------------------------------------------

RowAttributes rowAttributesOf(const Ubiquitty::RowMeta& meta)
{
    return { .lineAttribute = meta.lineAttribute, .isWrapped = meta.has(Ubiquitty::RowFlag::Wrapped), .mark = meta.oscMark };
}

bool containsRunTextAnchor(const Terminal& terminal, std::span<const Cell> cells)
{
    for (const Cell& cell : cells) {
        if (cell.clusterID() != 0 && Ubiquitty::isRunTextAnchor(&terminal, cell))
            return true;
    }
    return false;
}

template<typename Append>
void writeStateDigestInput(ScreenMode active, int rows, int columns, std::span<const std::uint64_t> activeRows,
    std::span<const std::uint64_t> inactiveRows, const StateBlockHashes& blocks, Append&& append)
{
    append(active == ScreenMode::Alternate ? 1 : 0);
    append(static_cast<std::uint8_t>(rows));
    append(static_cast<std::uint8_t>(rows >> 8));
    append(static_cast<std::uint8_t>(columns));
    append(static_cast<std::uint8_t>(columns >> 8));
    const auto hashBytes = [&](std::uint64_t hash) {
        for (int shift = 0; shift < 64; shift += 8)
            append(static_cast<std::uint8_t>(hash >> shift));
    };
    for (const std::uint64_t hash : activeRows)
        hashBytes(hash);
    for (const std::uint64_t hash : inactiveRows)
        hashBytes(hash);
    for (const std::uint64_t hash : blocks)
        hashBytes(hash);
}

// The serialized input of the state digest; see ScreenPlane.h.
Digest128 stateDigest(ScreenMode active, int rows, int columns, std::span<const std::uint64_t> activeRows,
    std::span<const std::uint64_t> inactiveRows, const StateBlockHashes& blocks)
{
    assert(activeRows.size() <= static_cast<std::size_t>(maxRows) && inactiveRows.size() <= static_cast<std::size_t>(maxRows));
    std::array<std::uint8_t, 5 + 8 * (2 * maxRows + stateBlockCount)> input;
    std::size_t size = 0;
    writeStateDigestInput(active, rows, columns, activeRows, inactiveRows, blocks,
        [&](std::uint8_t byte) { input[size++] = byte; });
    return digestOf(std::span(input).first(size));
}

// Pens compare by their wire bytes, which name every field the wire carries.
bool isSamePen(const Pen& a, const Pen& b)
{
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    WireWriter firstWriter(first);
    WireWriter secondWriter(second);
    writePen(firstWriter, a);
    writePen(secondWriter, b);
    return first == second;
}

void putDigest(WireWriter& writer, const Digest128& digest)
{
    writer.u64(digest.low);
    writer.u64(digest.high);
}

Digest128 takeDigest(WireReader& reader)
{
    Digest128 digest;
    digest.low = reader.u64();
    digest.high = reader.u64();
    return digest;
}

template<typename Definition>
void writeLinkSlot(Ubiquitty::Attributes::Hyperlinks::LinkTable& table, std::size_t slot, const Definition& definition)
{
    table.ids[slot] = definition.id;
    table.keys[slot] = {};
    table.uris[slot] = {};
    std::memcpy(table.keys[slot].data(), definition.key.data(), definition.key.size());
    std::memcpy(table.uris[slot].data(), definition.uri.data(), definition.uri.size());
    table.hashes[slot] = Ubiquitty::Attributes::Hyperlinks::dedupHash(definition.key.empty() ? definition.uri : definition.key);
    table.mruID = 0;
}

// Writes a link into the ring a client keeps, under the host's id: over the
// slot that holds the id, else into the next slot, evicting the oldest when
// the ring is full, as the host's own ring would.
void installLink(Terminal& terminal, const LinkDefinition& definition)
{
    Ubiquitty::Attributes::Hyperlinks::LinkTable& table = Ubiquitty::Attributes::Hyperlinks::ensureTable(terminal);
    int slot = -1;
    for (int index = 0; index < terminal.linkCount; ++index) {
        const int candidate = (terminal.linkHead + index) % Ubiquitty::VT::linkTableCapacity;
        if (table.ids[static_cast<std::size_t>(candidate)] == definition.id) {
            slot = candidate;
            break;
        }
    }
    if (slot < 0) {
        if (terminal.linkCount < Ubiquitty::VT::linkTableCapacity) {
            slot = (terminal.linkHead + terminal.linkCount) % Ubiquitty::VT::linkTableCapacity;
            ++terminal.linkCount;
        } else {
            slot = terminal.linkHead;
            terminal.linkHead = (terminal.linkHead + 1) % Ubiquitty::VT::linkTableCapacity;
        }
    }
    writeLinkSlot(table, static_cast<std::size_t>(slot), definition);
    if (definition.id >= terminal.linkNextID && terminal.linkNextID != 0)
        terminal.linkNextID = definition.id == 0xFFFF ? 0 : static_cast<std::uint16_t>(definition.id + 1);
}

void writeLinkDefinition(WireWriter& writer, const LinkDefinition& definition)
{
    writer.u16(definition.id);
    writer.bytes({ reinterpret_cast<const std::uint8_t*>(definition.key.data()), definition.key.size() }); // chars as bytes
    writer.bytes({ reinterpret_cast<const std::uint8_t*>(definition.uri.data()), definition.uri.size() }); // chars as bytes
}

struct LinkDefinitionView {
    std::uint16_t id = 0;
    std::string_view key;
    std::string_view uri;
};

std::optional<LinkDefinitionView> readLinkDefinitionView(WireReader& reader)
{
    LinkDefinitionView definition;
    definition.id = reader.u16();
    // One byte of each field is its terminator in the ring.
    const std::span<const std::uint8_t> key = reader.bytes(Ubiquitty::VT::maxLinkKeyLength - 1);
    const std::span<const std::uint8_t> uri = reader.bytes(Ubiquitty::VT::maxLinkURILength - 1);
    if (!reader.ok() || definition.id == 0)
        return std::nullopt;
    definition.key = { reinterpret_cast<const char*>(key.data()), key.size() };
    definition.uri = { reinterpret_cast<const char*>(uri.data()), uri.size() };
    // A NUL inside would end the stored string early, and the ring would hold
    // a different link than the one defined.
    if (definition.key.find('\0') != std::string::npos || definition.uri.find('\0') != std::string::npos) {
        reader.fail();
        return std::nullopt;
    }
    return definition;
}

std::optional<LinkDefinition> readLinkDefinition(WireReader& reader)
{
    const auto view = readLinkDefinitionView(reader);
    if (!view)
        return std::nullopt;
    return LinkDefinition { view->id, std::string(view->key), std::string(view->uri) };
}

void writeCodePoints(WireWriter& writer, std::span<const std::uint32_t> codePoints)
{
    writer.varint(codePoints.size());
    for (const std::uint32_t codePoint : codePoints)
        writer.varint(codePoint);
}

template<typename Resize>
bool readCodePointsInto(WireReader& reader, std::size_t maximum, Resize&& resize)
{
    const std::size_t count = reader.count(maximum);
    if (count == 0)
        reader.fail();
    for (std::uint32_t& codePoint : resize(count))
        codePoint = static_cast<std::uint32_t>(reader.varint(std::numeric_limits<std::uint32_t>::max()));
    return reader.ok();
}

bool readCodePoints(WireReader& reader, std::size_t maximum, std::vector<std::uint32_t>& codePoints)
{
    return readCodePointsInto(reader, maximum, [&](std::size_t count) {
        codePoints.resize(count);
        return std::span(codePoints);
    });
}

// The text-size table: the live mask, then every slot, dead ones included,
// since a pen names its spec by slot.
void writeTextSizeTable(WireWriter& writer, const Ubiquitty::TextSizeTable& table)
{
    writer.u16(table.liveMask());
    for (int slot = 0; slot < Ubiquitty::textSizeTableCapacity; ++slot) {
        const Ubiquitty::TextSizeSpec& spec = table.specAtSlot(slot);
        writer.u8(spec.scale);
        writer.u8(spec.widthCells);
        writer.u8(spec.fractionNumerator);
        writer.u8(spec.fractionDenominator);
        writer.u8(spec.verticalAlign);
        writer.u8(spec.horizontalAlign);
        writer.u8(spec.isProportional | (spec.isTextRun << 1));
        writer.u16(spec.runWidthCells);
        writer.u16(spec.claimedRunWidthCells);
        writer.u8(spec.isContinued);
    }
}

void readTextSizeTable(WireReader& reader, Ubiquitty::TextSizeTable& table)
{
    const std::uint16_t mask = reader.u16();
    if ((mask & ~Ubiquitty::allTextSizeSpecIDs) != 0)
        reader.fail();
    std::array<Ubiquitty::TextSizeSpec, Ubiquitty::textSizeTableCapacity> specs {};
    for (Ubiquitty::TextSizeSpec& spec : specs) {
        spec.scale = reader.u8();
        spec.widthCells = reader.u8();
        spec.fractionNumerator = reader.u8();
        spec.fractionDenominator = reader.u8();
        spec.verticalAlign = reader.u8();
        spec.horizontalAlign = reader.u8();
        const auto textKind = reader.u8();
        spec.isProportional = textKind & 1;
        spec.isTextRun = (textKind >> 1) & 1;
        if (textKind > 2)
            reader.fail();
        spec.runWidthCells = reader.u16();
        spec.claimedRunWidthCells = reader.u16();
        spec.isContinued = reader.u8();
        if (spec.isContinued > 1)
            reader.fail();
    }
    table.restore(mask, specs);
}

void writeUserDefinedKeys(WireWriter& writer, const Ubiquitty::VT::UserDefinedKeys* keys)
{
    writer.u8(keys ? 1 : 0);
    if (!keys)
        return;
    writer.u8(keys->locked ? 1 : 0);
    writer.varint(static_cast<std::uint64_t>(keys->count));
    for (int index = 0; index < keys->count; ++index) {
        const Ubiquitty::VT::UDKEntry& entry = keys->entries[index];
        writer.u16(entry.keyCode);
        writer.bytes({ reinterpret_cast<const std::uint8_t*>(entry.value), entry.length }); // chars as bytes
    }
}

std::expected<void, ApplyOutcome> readUserDefinedKeys(WireReader& reader, std::unique_ptr<Ubiquitty::VT::UserDefinedKeys>& keys, CheckpointStoragePlan* plan = nullptr)
{
    const std::uint8_t isPresent = reader.u8();
    if (isPresent > 1)
        reader.fail();
    keys.reset();
    if (isPresent != 1)
        return {};
    Ubiquitty::VT::UserDefinedKeys fixed;
    if (plan) {
        plan->hasUserDefinedKeys = true;
        plan->userDefinedKeysBytes = sizeof(Ubiquitty::VT::UserDefinedKeys);
    } else {
        keys.reset(new (std::nothrow) Ubiquitty::VT::UserDefinedKeys {});
        if (!keys)
            return std::unexpected(ApplyOutcome::AllocationRefused);
    }
    auto& target = plan ? fixed : *keys;
    const std::uint8_t locked = reader.u8();
    if (locked > 1)
        reader.fail();
    target.locked = locked == 1;
    target.count = static_cast<int>(reader.count(Ubiquitty::VT::udkMaxEntries, 3));
    for (int index = 0; index < target.count && reader.ok(); ++index) {
        Ubiquitty::VT::UDKEntry& entry = target.entries[index];
        entry.keyCode = reader.u16();
        const std::span<const std::uint8_t> value = reader.bytes(Ubiquitty::VT::udkMaxValueBytes);
        entry.length = static_cast<std::uint16_t>(value.size());
        std::memcpy(entry.value, value.data(), value.size());
    }
    return {};
}

// Everything a checkpoint decodes before it touches the client.
struct CheckpointImage {
    std::uint64_t generation = 0;
    std::uint32_t dictionaryEpoch = 0;
    int rows = 0;
    int columns = 0;
    ScreenMode active = ScreenMode::Primary;
    std::array<std::span<const std::uint8_t>, stateBlockCount> blocks;
    ReplicaStorage storage;
    std::unique_ptr<Ubiquitty::Attributes::Hyperlinks::LinkTable> links;
    std::size_t linkCount = 0;
    std::uint16_t nextLinkID = 1;
    Ubiquitty::ClusterPool clusters;
    Ubiquitty::RunTextStore runTexts;
    std::array<std::unique_ptr<Cell[]>, 2> cells;
    std::array<std::unique_ptr<RowAttributes[]>, 2> attributes;
    std::uint32_t historyEpoch = 0;
    std::uint64_t historyHead = 0;
    Ubiquitty::PrecedingItem precedingItem;
    Digest128 digest;
};

/// Reads the fixed header used by both admission and full checkpoint decode.
bool readCheckpointHeader(WireReader& reader, CheckpointImage& image)
{
    image.generation = reader.u64();
    image.dictionaryEpoch = reader.u32();
    image.rows = reader.u16();
    image.columns = reader.u16();
    const std::uint8_t activeScreen = reader.u8();
    if (!reader.ok() || image.rows < 1 || image.rows > Ubiquitty::maxRows || image.columns < 1
        || image.columns > Ubiquitty::maxColumns || activeScreen > 1)
        return false;
    image.active = activeScreen == 1 ? ScreenMode::Alternate : ScreenMode::Primary;
    return true;
}

std::optional<std::size_t> decodedScreenCellBytes(int rows, int columns)
{
    const auto rowCount = static_cast<std::size_t>(rows);
    const auto columnCount = static_cast<std::size_t>(columns);
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (!rowCount || !columnCount || columnCount > maximum / rowCount)
        return std::nullopt;
    const auto cells = rowCount * columnCount;
    if (cells > maximum / (2 * sizeof(Cell)))
        return std::nullopt;
    return 2 * cells * sizeof(Cell);
}

// The most bytes the parser's ground state takes: the REP record's fields,
// each a varint of at most five bytes.
constexpr std::size_t maxParserGroundBytes = 16 * 5 + 2 * static_cast<std::size_t>(Ubiquitty::VT::maxParameters) * 5;

using StateBlocks = std::array<std::vector<std::uint8_t>, stateBlockCount>;

constexpr std::size_t indexOf(StateBlock block) { return std::to_underlying(block); }

// The order a body's blocks are decoded in: the screen states before the
// cursor, since checking the screen states reads the active cursor at the
// origin and the cursor block then writes it.
constexpr std::array decodeOrder { StateBlock::Modes, StateBlock::Palette, StateBlock::ScreenStates, StateBlock::Cursor, StateBlock::TextState, StateBlock::Activity };

// The checkpoint's blocks travel in this order, which is not `StateBlock`'s.
constexpr std::array checkpointBlockOrder { StateBlock::Modes, StateBlock::Palette, StateBlock::Cursor, StateBlock::ScreenStates, StateBlock::TextState, StateBlock::Activity, StateBlock::RichText };

// The longest string each activity field may hold, as the OSC parser bounds
// it; a longer one on the wire is refused before it is copied.
constexpr std::size_t maxContextIDBytes = 64;
constexpr std::size_t maxContextFieldBytes = 255;

void writeContext(WireWriter& writer, const OSC::Context& context)
{
    writer.bytes({ reinterpret_cast<const std::uint8_t*>(context.id.data()), context.id.size() });
    for (const auto& field : context.fields)
        writer.bytes({ reinterpret_cast<const std::uint8_t*>(field.data()), field.size() });
}

// Writes the activity block: the active contexts, the last ended one, and the
// program status records in eviction order.
void writeActivity(WireWriter& writer, const Terminal& terminal)
{
    writer.varint(terminal.contexts.active.size());
    for (const OSC::Context& context : terminal.contexts.active)
        writeContext(writer, context);
    writer.u8(terminal.contexts.lastEnded ? 1 : 0);
    if (terminal.contexts.lastEnded)
        writeContext(writer, *terminal.contexts.lastEnded);
    writer.varint(terminal.programStatus.records.size());
    // A status record's bytes are the ones a StatusRecords frame carries.
    for (const OSC::ProgramStatus& record : terminal.programStatus.records)
        writeStatusRecord(writer, record);
}

OSC::ContextView readContextView(WireReader& reader, OSC::ContextPhase phase)
{
    const auto text = [&](std::size_t maximum) {
        const auto bytes = reader.bytes(maximum);
        return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    OSC::ContextView context;
    context.id = text(maxContextIDBytes);
    for (auto& field : context.fields)
        field = text(maxContextFieldBytes);
    if (!reader.ok() || !OSC::validContext(context, phase))
        reader.fail();
    return context;
}

// Reads the activity block into empty stores, refusing what the OSC parser
// itself would not have stored.
std::expected<void, ApplyOutcome> readActivity(WireReader& reader, OSC::ContextState& contexts, OSC::ProgramStatusStore& statuses, CheckpointStoragePlan* plan = nullptr)
{
    contexts = {};
    statuses = {};
    const std::size_t contextCount = reader.count(OSC::contextDepthLimit, 1 + static_cast<std::size_t>(OSC::ContextField::Count));
    std::array<std::string_view, OSC::contextDepthLimit> contextIDs;
    const auto ownContext = [&](const OSC::ContextView& view) -> std::expected<OSC::Context, ApplyOutcome> {
        OSC::Context owned;
        if (!owned.id.tryAssign(view.id))
            return std::unexpected(ApplyOutcome::AllocationRefused);
        for (std::size_t index = 0; index < view.fields.size(); ++index) {
            if (!owned.fields[index].tryAssign(view.fields[index]))
                return std::unexpected(ApplyOutcome::AllocationRefused);
        }
        return owned;
    };
    const auto noteContext = [&](const OSC::ContextView& view) {
        if (!plan)
            return;
        if (!addStorageCount(plan->stateStringBytes, view.id.size())
            || (!view.id.empty() && !addStorageCount(plan->stateStringCount, 1)))
            reader.fail();
        for (const auto field : view.fields) {
            if (!addStorageCount(plan->stateStringBytes, field.size())
                || (!field.empty() && !addStorageCount(plan->stateStringCount, 1)))
                reader.fail();
        }
    };
    if (plan)
        plan->activeContexts = contextCount;
    else if (!contexts.active.tryReserve(contextCount))
        return std::unexpected(ApplyOutcome::AllocationRefused);
    for (std::size_t index = 0; index < contextCount && reader.ok(); ++index) {
        const auto context = readContextView(reader, OSC::ContextPhase::Active);
        const auto previous = std::span(contextIDs).first(index);
        if (!reader.ok() || std::ranges::find(previous, context.id) != previous.end()) {
            reader.fail();
            return {};
        }
        contextIDs[index] = context.id;
        noteContext(context);
        if (!plan) {
            auto owned = ownContext(context);
            if (!owned || !contexts.active.tryPushBack(std::move(*owned)))
                return std::unexpected(ApplyOutcome::AllocationRefused);
        }
    }
    const std::uint8_t hasEnded = reader.u8();
    if (hasEnded > 1) {
        reader.fail();
        return {};
    }
    if (hasEnded == 1) {
        const auto context = readContextView(reader, OSC::ContextPhase::Ended);
        if (!reader.ok()) {
            reader.fail();
            return {};
        }
        noteContext(context);
        if (plan)
            plan->hasEndedContext = true;
        else {
            auto owned = ownContext(context);
            if (!owned)
                return std::unexpected(owned.error());
            contexts.lastEnded = std::move(*owned);
        }
    }
    const std::size_t statusCount = reader.count(OSC::programStatusLimit, minStatusRecordBytes);
    std::array<std::string_view, OSC::programStatusLimit> statusIDs;
    if (plan)
        plan->statusRecords = statusCount;
    else if (!statuses.records.tryReserve(statusCount))
        return std::unexpected(ApplyOutcome::AllocationRefused);
    for (std::size_t index = 0; index < statusCount && reader.ok(); ++index) {
        const auto record = readStatusRecordView(reader);
        const auto previous = std::span(statusIDs).first(index);
        if (!reader.ok() || std::ranges::find(previous, record.id) != previous.end()) {
            reader.fail();
            return {};
        }
        statusIDs[index] = record.id;
        if (plan) {
            for (const auto text : { record.id, record.app, record.title, record.message }) {
                if (!addStorageCount(plan->stateStringBytes, text.size())
                    || (!text.empty() && !addStorageCount(plan->stateStringCount, 1)))
                    reader.fail();
            }
        } else {
            OSC::ProgramStatus owned;
            if (!owned.id.tryAssign(record.id) || !owned.app.tryAssign(record.app)
                || !owned.title.tryAssign(record.title) || !owned.message.tryAssign(record.message))
                return std::unexpected(ApplyOutcome::AllocationRefused);
            owned.state = record.state;
            owned.kind = record.kind;
            owned.progress = record.progress;
            if (!statuses.records.tryPushBack(std::move(owned)))
                return std::unexpected(ApplyOutcome::AllocationRefused);
        }
    }
    return {};
}

// Writes the palette block: the live palette and its baseline, the dynamic
// colours, the cursor's text colour and the saved palette stack.
void writePaletteBlock(WireWriter& writer, const Terminal& terminal)
{
    putPaletteBlock(writer, terminal.palette, terminal.paletteBaseline);
    dynamicColourFields(EncodeVisit { writer }, terminal, terminal);
    cursorTextColourFields(EncodeVisit { writer }, terminal, terminal);
    put(writer, terminal.savedPaletteValid);
    put(writer, terminal.savedPalettePosition);
    put(writer, terminal.savedPaletteCount);
    for (size_t slot = 0; slot < Terminal::paletteStackDepth; ++slot) {
        if (!(terminal.savedPaletteValid & (1u << slot)))
            continue;
        const auto& saved = (*terminal.savedPalettes)[slot];
        putPalette(writer, saved.palette, terminal.paletteBaseline);
        put(writer, saved.dynamicChannels);
        put(writer, saved.dynamicSet);
        put(writer, saved.cursorTextColor.red);
        put(writer, saved.cursorTextColor.green);
        put(writer, saved.cursorTextColor.blue);
        put(writer, saved.hasCursorTextColor);
    }
}

// Encodes every state block of a terminal. The text state goes first, since
// its status row may define pens and links the update's definitions carry.
bool encodeStateBlocks(const Terminal& terminal, PaneDictionaries& dictionaries, StateBlocks& blocks)
{
    const ScreenMode active = activeScreenOf(terminal);
    for (std::vector<std::uint8_t>& block : blocks)
        block.clear();
    {
        WireWriter writer(blocks[indexOf(StateBlock::TextState)]);
        textStateFields(EncodeVisit { writer }, terminal, terminal);
        putDelta<std::uint8_t>(writer, terminal.sixelColorRegisters, defaultSixelRegisters());
        putTabStops(writer, terminal.tabStops);
        putDelta<CharacterTabAlignment>(writer, terminal.tabAlignments, noTabAlignments);
        putDelta<std::uint32_t>(writer, terminal.tabAlignmentTargets, noTabAlignmentTargets);
        putTabStops(writer, terminal.lineTabStops);
        writeUserDefinedKeys(writer, terminal.udk.get());
        writeTextSizeTable(writer, terminal.textSizeTable);
        const std::span<const Cell> statusRow = Ubiquitty::Rendering::statusRow(terminal);
        writer.u8(statusRow.empty() ? 0 : 1);
        if (!statusRow.empty()) {
            std::vector<std::uint8_t> record;
            if (!encodeRowRecord(terminal, statusRow, {}, dictionaries, RunTextDefinitions::Present, record))
                return false;
            writer.bytes(record);
        }
    }
    if (terminal.currentLinkID != 0)
        dictionaries.noteLink(terminal, terminal.currentLinkID);
    WireWriter modesWriter(blocks[indexOf(StateBlock::Modes)]);
    modesBlock(EncodeVisit { modesWriter }, terminal, terminal);
    WireWriter paletteWriter(blocks[indexOf(StateBlock::Palette)]);
    writePaletteBlock(paletteWriter, terminal);
    WireWriter cursorWriter(blocks[indexOf(StateBlock::Cursor)]);
    cursorBlock(EncodeVisit { cursorWriter }, terminal, terminal, active);
    WireWriter screenStatesWriter(blocks[indexOf(StateBlock::ScreenStates)]);
    screenStatesBlock(EncodeVisit { screenStatesWriter }, terminal, terminal, active);
    WireWriter activityWriter(blocks[indexOf(StateBlock::Activity)]);
    writeActivity(activityWriter, terminal);
    WireWriter richTextWriter(blocks[indexOf(StateBlock::RichText)]);
    writeRichTextBlock(richTextWriter, terminal.richTextBlocks);
    return true;
}

// The status row a text-state block carries, decoded beside the scratch
// terminal, which has no status row of its own to hold it.
struct DecodedStatusRow {
    bool isPresent = false;
    std::unique_ptr<Ubiquitty::Rendering::StatusRowStorage> cells;
};

// Decodes one state block into a scratch terminal and checks it against the
// geometry it lands in. Only the members the block names are written; the
// rest of the scratch terminal is whatever it held.
template<typename Pens, typename Resolves>
std::expected<void, ApplyOutcome> decodeStateBlockWithNames(StateBlock block, std::span<const std::uint8_t> bytes, Terminal& scratch, ScreenMode active, int rows, int columns,
    const Pens& pens, Resolves&& resolves, DecodedStatusRow& statusRow, CheckpointStoragePlan* plan = nullptr)
{
    WireReader reader(bytes);
    bool didRefuseAllocation = false;
    const auto decode = [&](auto& to, auto& from) {
        if (plan)
            PlanningVisit { reader, *plan }(to, from);
        else
            DecodeVisit { reader, &didRefuseAllocation }(to, from);
    };
    switch (block) {
    case StateBlock::Cursor: {
        cursorBlock(decode, scratch, scratch, active);
        const Ubiquitty::Screen::State& state = screenState(scratch, active);
        if (!isWithin(state.cursorX, 0, columns - 1) || !isWithin(state.cursorY, 0, rows - 1))
            reader.fail();
        break;
    }
    case StateBlock::Modes:
        modesBlock(decode, scratch, scratch);
        if (reader.ok() && !isValidModes(scratch, columns))
            reader.fail();
        break;
    case StateBlock::Palette:
        takePaletteBlock(reader, scratch.palette, scratch.paletteBaseline);
        dynamicColourFields(decode, scratch, scratch);
        cursorTextColourFields(decode, scratch, scratch);
        take(reader, scratch.savedPaletteValid);
        take(reader, scratch.savedPalettePosition);
        take(reader, scratch.savedPaletteCount);
        if (scratch.savedPaletteValid >= (1u << Terminal::paletteStackDepth)
            || scratch.savedPalettePosition > Terminal::paletteStackDepth
            || scratch.savedPaletteCount > Terminal::paletteStackDepth)
            reader.fail();
        scratch.savedPalettes.reset();
        if (scratch.savedPaletteValid && reader.ok()) {
            if (plan)
                plan->paletteStackBytes = sizeof(Terminal::PaletteStack);
            else {
                scratch.savedPalettes.reset(new (std::nothrow) Terminal::PaletteStack {});
                if (!scratch.savedPalettes)
                    return std::unexpected(ApplyOutcome::AllocationRefused);
            }
            for (size_t slot = 0; slot < Terminal::paletteStackDepth; ++slot) {
                if (!(scratch.savedPaletteValid & (1u << slot)))
                    continue;
                Terminal::SavedPalette fixed;
                auto& saved = plan ? fixed : (*scratch.savedPalettes)[slot];
                takePalette(reader, saved.palette, scratch.paletteBaseline);
                take(reader, saved.dynamicChannels);
                take(reader, saved.dynamicSet);
                take(reader, saved.cursorTextColor.red);
                take(reader, saved.cursorTextColor.green);
                take(reader, saved.cursorTextColor.blue);
                take(reader, saved.hasCursorTextColor);
            }
        }
        break;
    case StateBlock::ScreenStates: {
        // The active screen's cursor travels in the cursor block, so the
        // check reads it at the origin rather than whatever the scratch held.
        Ubiquitty::Screen::State& activeState = screenState(scratch, active);
        activeState.cursorX = 0;
        activeState.cursorY = 0;
        screenStatesBlock(decode, scratch, scratch, active);
        if (reader.ok() && (!isValidScreenState(scratch.screens.primary, rows, columns) || !isValidScreenState(scratch.screens.alternate, rows, columns)))
            reader.fail();
        break;
    }
    case StateBlock::TextState: {
        textStateFields(decode, scratch, scratch);
        takeDelta<std::uint8_t>(reader, scratch.sixelColorRegisters, defaultSixelRegisters());
        takeTabStops(reader, scratch.tabStops);
        scratch.tabAlignments.clear();
        scratch.tabAlignmentTargets.clear();
        auto alignments = noTabAlignments;
        auto targets = noTabAlignmentTargets;
        takeDelta<CharacterTabAlignment>(reader, alignments, noTabAlignments);
        takeDelta<std::uint32_t>(reader, targets, noTabAlignmentTargets);
        for (auto target : targets) {
            if (target > 0x10FFFF || (target >= 0xD800 && target <= 0xDFFF))
                reader.fail();
        }
        if (reader.ok() && (alignments != noTabAlignments || targets != noTabAlignmentTargets)) {
            if (plan)
                plan->alignedTabBytes = sizeof(alignments) + sizeof(targets);
            else if (!scratch.tabAlignments.tryAssign(alignments.begin(), alignments.end()) || !scratch.tabAlignmentTargets.tryAssign(targets.begin(), targets.end()))
                return std::unexpected(ApplyOutcome::AllocationRefused);
        }
        takeTabStops(reader, scratch.lineTabStops);
        if (auto decoded = readUserDefinedKeys(reader, scratch.udk, plan); !decoded)
            return decoded;
        readTextSizeTable(reader, scratch.textSizeTable);
        const std::uint8_t hasStatusRow = reader.u8();
        if (hasStatusRow > 1)
            reader.fail();
        statusRow.isPresent = hasStatusRow == 1 && reader.ok();
        if (plan)
            plan->hasStatusRow = statusRow.isPresent;
        statusRow.cells.reset();
        if (statusRow.isPresent) {
            const std::span<const std::uint8_t> record = reader.bytes(reader.remaining());
            WireReader recordReader(record);
            RowAttributes ignored;
            Ubiquitty::Rendering::StatusRowStorage fixed;
            if (!plan) {
                statusRow.cells.reset(new (std::nothrow) Ubiquitty::Rendering::StatusRowStorage);
                if (!statusRow.cells)
                    return std::unexpected(ApplyOutcome::AllocationRefused);
            }
            auto& cells = plan ? fixed.cells : statusRow.cells->cells;
            std::fill(cells.begin(), cells.end(), defaultBlankCell);
            if (!decodeRowRecordWithNames(recordReader, pens, resolves, cells, ignored) || !recordReader.isAtEnd())
                reader.fail();
        }
        if (reader.ok() && !isValidTextState(scratch))
            reader.fail();
        break;
    }
    case StateBlock::Activity:
        if (auto decoded = readActivity(reader, scratch.contexts, scratch.programStatus, plan); !decoded)
            return decoded;
        break;
    case StateBlock::RichText:
        // Its placements name documents this decoder does not see;
        // readRichTextBlock decodes it with them.
        return std::unexpected(ApplyOutcome::Malformed);
    }
    if (didRefuseAllocation)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(ApplyOutcome::Malformed);
    return {};
}

std::expected<void, ApplyOutcome> decodeStateBlock(StateBlock block, std::span<const std::uint8_t> bytes, Terminal& scratch, ScreenMode active, int rows, int columns,
    std::span<const Pen> pens, const RowReferences& references, DecodedStatusRow& statusRow)
{
    return decodeStateBlockWithNames(block, bytes, scratch, active, rows, columns, pens, [&](std::uint64_t cluster) { return cluster < references.clusters.size() && references.clusters[static_cast<std::size_t>(cluster)]; }, statusRow);
}

// Assigns what one decoded block names from the scratch terminal to the
// client's. Nothing here can refuse.
void commitStateBlock(StateBlock block, Terminal& terminal, Terminal& scratch, ScreenMode active, DecodedStatusRow& statusRow)
{
    switch (block) {
    case StateBlock::Cursor:
        cursorBlock(CommitVisit {}, terminal, scratch, active);
        return;
    case StateBlock::Modes:
        modesBlock(CommitVisit {}, terminal, scratch);
        return;
    case StateBlock::Palette:
        terminal.palette = scratch.palette;
        terminal.paletteBaseline = scratch.paletteBaseline;
        terminal.savedPalettes = std::move(scratch.savedPalettes);
        terminal.savedPaletteValid = scratch.savedPaletteValid;
        terminal.savedPalettePosition = scratch.savedPalettePosition;
        terminal.savedPaletteCount = scratch.savedPaletteCount;
        dynamicColourFields(CommitVisit {}, terminal, scratch);
        cursorTextColourFields(CommitVisit {}, terminal, scratch);
        ++terminal.paletteEpoch;
        return;
    case StateBlock::ScreenStates:
        screenStatesBlock(CommitVisit {}, terminal, scratch, active);
        return;
    case StateBlock::TextState:
        textStateFields(CommitVisit {}, terminal, scratch);
        terminal.tabStops = scratch.tabStops;
        terminal.tabAlignments = std::move(scratch.tabAlignments);
        terminal.tabAlignmentTargets = std::move(scratch.tabAlignmentTargets);
        terminal.lineTabStops = scratch.lineTabStops;
        terminal.sixelColorRegisters = scratch.sixelColorRegisters;
        terminal.udk = std::move(scratch.udk);
        terminal.textSizeTable = scratch.textSizeTable;
        terminal.statusRow = std::move(statusRow.cells);
        terminal.titleChanged = true;
        return;
    case StateBlock::Activity:
        terminal.contexts = std::move(scratch.contexts);
        terminal.programStatus = std::move(scratch.programStatus);
        OSC::activityChanged(terminal);
        return;
    case StateBlock::RichText:
        terminal.richTextBlocks = std::move(scratch.richTextBlocks);
        terminal.richTextLayouts.retainOnly(terminal.richTextBlocks);
        return;
    }
}

// A client's row references: every pool cluster and run text it holds.
RowReferences heldReferences(const Terminal& terminal)
{
    RowReferences references;
    references.clusters.assign(static_cast<std::size_t>(RunTextStore::maxEntries) + 1, false);
    const std::size_t knownClusters = terminal.clusterPool.entries().size();
    for (std::size_t id = 1; id <= knownClusters; ++id)
        references.clusters[id] = true;
    const auto runTexts = terminal.runTextStore.slots();
    for (std::size_t slot = 0; slot < runTexts.size(); ++slot) {
        if (!runTexts[slot].empty())
            references.clusters[slot + 1] = true;
    }
    return references;
}

// The live links of a terminal's ring, oldest first.
std::vector<LinkDefinition> liveLinks(const Terminal& terminal)
{
    std::vector<LinkDefinition> links;
    if (!terminal.linkTable)
        return links;
    const Ubiquitty::Attributes::Hyperlinks::LinkTable& table = *terminal.linkTable;
    for (int index = 0; index < terminal.linkCount; ++index) {
        const auto slot = static_cast<std::size_t>((terminal.linkHead + index) % Ubiquitty::VT::linkTableCapacity);
        if (table.ids[slot] == 0)
            continue;
        LinkDefinition definition;
        definition.id = table.ids[slot];
        definition.key = table.keys[slot].data();
        definition.uri = table.uris[slot].data();
        links.push_back(std::move(definition));
    }
    return links;
}

} // namespace

void putPaletteBlock(WireWriter& writer, const Ubiquitty::TermPalette& live, const Ubiquitty::TermPalette& baseline)
{
    putPalette(writer, live, seededPalette());
    putPalette(writer, baseline, live);
}

void takePaletteBlock(WireReader& reader, Ubiquitty::TermPalette& live, Ubiquitty::TermPalette& baseline)
{
    takePalette(reader, live, seededPalette());
    takePalette(reader, baseline, live);
}

bool RowEncodingCache::refresh(const Ubiquitty::Terminal& terminal, ScreenMode screen, PaneDictionaries& dictionaries)
{
    m_encodedCount = 0;
    if (!terminal.grid)
        return false;
    const int rows = terminal.rows;
    const int columns = terminal.columns;
    if (m_epoch != dictionaries.epoch() || m_columns != columns || static_cast<int>(m_rows.size()) != rows) {
        m_rows.assign(static_cast<std::size_t>(rows), Row {});
        m_epoch = dictionaries.epoch();
        m_columns = columns;
    }
    m_scratch.resize(static_cast<std::size_t>(columns));
    const int screenIndex = screen == ScreenMode::Alternate ? 1 : 0;
    for (int row = 0; row < rows; ++row) {
        Row& entry = m_rows[static_cast<std::size_t>(row)];
        terminal.grid->readRow(screen, Ubiquitty::Row(row), m_scratch.data());
        const RowAttributes attributes = rowAttributesOf(terminal.grid->rowMetaAt(screenIndex, row));
        if (entry.isValid && entry.attributes == attributes
            && std::memcmp(entry.cells.data(), m_scratch.data(), m_scratch.size() * sizeof(Cell)) == 0)
            continue;
        // The encoding this one replaces is what a client one capture behind
        // holds, and what a `Spans` op is cut against.
        entry.hasPrevious = entry.isValid;
        if (entry.isValid) {
            std::swap(entry.previousCells, entry.cells);
            entry.previousAttributes = entry.attributes;
            entry.previousHash = entry.hash;
        }
        entry.record.clear();
        if (!encodeRowRecord(terminal, m_scratch, attributes, dictionaries, RunTextDefinitions::Present, entry.record)) {
            entry.isValid = false;
            entry.hasPrevious = false;
            return false;
        }
        entry.cells = m_scratch;
        entry.attributes = attributes;
        entry.hash = rowHash(entry.record);
        entry.hasRunTextAnchor = containsRunTextAnchor(terminal, m_scratch);
        entry.isValid = true;
        ++m_encodedCount;
    }
    return true;
}

bool ScreenPublisher::refreshScreens(const Ubiquitty::Terminal& terminal)
{
    return m_screens[0].refresh(terminal, ScreenMode::Primary, m_dictionaries) && m_screens[1].refresh(terminal, ScreenMode::Alternate, m_dictionaries)
        && encodeStateBlocks(terminal, m_dictionaries, m_blocks) && m_dictionaries.takeClusters(terminal);
}

bool ScreenPublisher::capture(const Ubiquitty::Terminal& terminal)
{
    m_isCaptured = false;
    if (!terminal.grid)
        return false;
    // A cluster pool the dictionaries can no longer mirror by appending, or
    // a dictionary past its cap, starts a new epoch, which every attachment
    // then takes as a checkpoint.
    if (!refreshScreens(terminal) || m_dictionaries.isPastEpochLimits()) {
        m_dictionaries.roll();
        if (!refreshScreens(terminal))
            return false;
    }
    for (std::size_t block = 0; block < stateBlockCount; ++block)
        m_blockHashes[block] = blockHash(m_blocks[block]);
    m_isAlternate = terminal.screens.isUsingAlternateScreen;
    m_rows = terminal.rows;
    m_columns = terminal.columns;

    m_historyAddress = Ubiquitty::Screen::historyAddress(terminal);
    m_isCaptured = true;
    return true;
}

namespace {

std::vector<std::uint64_t> hashesOf(const RowEncodingCache& rows)
{
    std::vector<std::uint64_t> hashes;
    hashes.reserve(static_cast<std::size_t>(rows.rowCount()));
    for (int row = 0; row < rows.rowCount(); ++row)
        hashes.push_back(rows.hash(row));
    return hashes;
}

} // namespace

void ScreenPublisher::noteLinks(const Ubiquitty::Terminal& terminal, std::span<const std::uint16_t> ids)
{
    for (const std::uint16_t id : ids)
        m_dictionaries.noteLink(terminal, id);
}

std::optional<ScreenPublisher::Continuation> ScreenPublisher::continuation() const
{
    if (!m_isCaptured)
        return std::nullopt;
    return Continuation { .dictionaries = m_dictionaries.snapshot(),
        .rowHashes = { hashesOf(m_screens[0]), hashesOf(m_screens[1]) },
        .blockHashes = m_blockHashes,
        .history = m_historyAddress,
        .rows = m_rows,
        .columns = m_columns,
        .isAlternate = m_isAlternate };
}

bool ScreenPublisher::restore(const Ubiquitty::Terminal& terminal, Continuation continuation)
{
    if (terminal.rows != continuation.rows || terminal.columns != continuation.columns
        || terminal.screens.isUsingAlternateScreen != continuation.isAlternate)
        return false;
    const auto epoch = continuation.dictionaries.epoch;
    const DictionaryMarks marks { continuation.dictionaries.pens.size(), continuation.dictionaries.links.size(), continuation.dictionaries.clusterHashes.size() };
    auto dictionaries = PaneDictionaries::restored(std::move(continuation.dictionaries));
    if (!dictionaries)
        return false;
    ScreenPublisher candidate;
    candidate.m_dictionaries = std::move(*dictionaries);
    if (!candidate.capture(terminal))
        return false;
    if (candidate.m_dictionaries.epoch() != epoch
        || candidate.m_dictionaries.pens().size() != marks.pens || candidate.m_dictionaries.links().size() != marks.links
        || candidate.m_dictionaries.clusterCount() != marks.clusters
        || candidate.m_blockHashes != continuation.blockHashes
        || candidate.m_historyAddress.epoch != continuation.history.epoch || candidate.m_historyAddress.oldest != continuation.history.oldest
        || candidate.m_historyAddress.head != continuation.history.head || candidate.m_historyAddress.clearedThrough != continuation.history.clearedThrough)
        return false;
    for (std::size_t screen = 0; screen < candidate.m_screens.size(); ++screen) {
        const auto& rows = candidate.m_screens[screen];
        if (continuation.rowHashes[screen].size() != static_cast<std::size_t>(rows.rowCount()))
            return false;
        for (int row = 0; row < rows.rowCount(); ++row)
            if (rows.hash(row) != continuation.rowHashes[screen][static_cast<std::size_t>(row)])
                return false;
    }
    *this = std::move(candidate);
    return true;
}

bool ScreenPublisher::checkpoint(const Ubiquitty::Terminal& terminal, std::uint64_t generation, std::vector<std::uint8_t>& body, AttachmentBaseline& attachment)
{
    if (!m_isCaptured)
        return false;
    const ScreenMode active = m_isAlternate ? ScreenMode::Alternate : ScreenMode::Primary;

    if (!m_dictionaries.takeClusters(terminal))
        return false;

    const RowEncodingCache& activeRows = m_screens[m_isAlternate ? 1 : 0];
    const RowEncodingCache& inactiveRows = m_screens[m_isAlternate ? 0 : 1];
    std::vector<std::uint8_t> output;
    WireWriter writer(output);
    writer.u64(generation);
    writer.u32(m_dictionaries.epoch());
    writer.u16(static_cast<std::uint16_t>(m_rows));
    writer.u16(static_cast<std::uint16_t>(m_columns));
    writer.u8(m_isAlternate ? 1 : 0);
    for (const StateBlock block : checkpointBlockOrder)
        writer.bytes(m_blocks[indexOf(block)]);

    writer.varint(m_dictionaries.pens().size());
    for (const Pen& pen : m_dictionaries.pens())
        writePen(writer, pen);
    // The ring itself, oldest first, so the client's ring retires what the
    // host's retires.
    const std::vector<LinkDefinition> links = liveLinks(terminal);
    writer.varint(links.size());
    for (const LinkDefinition& link : links)
        writeLinkDefinition(writer, link);
    const std::span<const ClusterPool::Entry> clusters = terminal.clusterPool.entries();
    writer.varint(clusters.size());
    for (const ClusterPool::Entry& entry : clusters)
        writeCodePoints(writer, { entry.codePoints.data(), entry.length });
    const auto runTexts = terminal.runTextStore.slots();
    std::size_t liveRunTexts = 0;
    for (const auto& text : runTexts)
        liveRunTexts += text.empty() ? 0 : 1;
    writer.varint(liveRunTexts);
    for (std::size_t slot = 0; slot < runTexts.size(); ++slot) {
        if (runTexts[slot].empty())
            continue;
        writer.varint(slot + 1);
        writeCodePoints(writer, runTexts[slot]);
    }
    std::size_t blockCount = 0;
    for (const ScreenMode screen : { ScreenMode::Primary, ScreenMode::Alternate })
        blockCount += terminal.richTextBlocks.blocks(screen).size();
    writer.varint(blockCount);
    for (const ScreenMode screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const RichText::Block& block : terminal.richTextBlocks.blocks(screen)) {
            writer.varint(block.serial);
            writeDocument(writer, block.document);
        }
    }

    for (int row = 0; row < m_rows; ++row)
        writer.bytes(activeRows.record(row));
    for (int row = 0; row < m_rows; ++row)
        writer.bytes(inactiveRows.record(row));
    writer.u32(m_historyAddress.epoch);
    writer.u64(m_historyAddress.head);
    std::vector<std::uint8_t> parserGround;
    WireWriter parserGroundWriter(parserGround);
    parserGroundFields(EncodeVisit { parserGroundWriter }, terminal.parser.precedingItem, terminal.parser.precedingItem);
    writer.bytes(parserGround);
    const std::vector<std::uint64_t> activeHashes = hashesOf(activeRows);
    const std::vector<std::uint64_t> inactiveHashes = hashesOf(inactiveRows);
    putDigest(writer, stateDigest(active, m_rows, m_columns, activeHashes, inactiveHashes, m_blockHashes));

    body.insert(body.end(), output.begin(), output.end());
    m_dictionaries.noteCheckpointed();
    attachment.dictionaryEpoch = m_dictionaries.epoch();
    attachment.marks = { .pens = m_dictionaries.pens().size(), .links = m_dictionaries.links().size(), .clusters = m_dictionaries.clusterCount() };
    attachment.isAlternate = m_isAlternate;
    attachment.columns = m_columns;
    attachment.rowHashes[0] = hashesOf(m_screens[0]);
    attachment.rowHashes[1] = hashesOf(m_screens[1]);
    attachment.blockHashes = m_blockHashes;
    attachment.richTextSerials = richTextSerials(terminal.richTextBlocks);
    return true;
}

namespace {

/// The most rows a `ScrollRect` moves its rows by: a scroll further than a
/// screen's worth on the largest screens says less than its rows would.
constexpr int maxScrollDelta = 64;

/// The fewest rows a scroll must carry for a `ScrollRect` to say them.
constexpr int minScrolledRows = 2;

/// The columns a run of unchanged cells must span before two changed spans
/// are cut apart rather than sent as one.
constexpr int spanJoinGap = 8;

/// A scroll of rows within one screen: rows `top` to `bottom` take the rows
/// `delta` below them.
struct ScrollAlignment {
    int top = 0;
    int bottom = 0;
    int delta = 0;
};

/// Finds the scroll that best explains how a screen's rows moved: the longest
/// run of rows whose current hash is the hash the client holds `delta` rows
/// away and not its own.
///
/// - Parameters:
///   - held: The hashes the client holds.
///   - current: The hashes of the frame being sent.
/// - Returns: The scroll, or `nullopt` when no run of `minScrolledRows` moved.
std::optional<ScrollAlignment> alignScroll(std::span<const std::uint64_t> held, std::span<const std::uint64_t> current)
{
    const int rows = static_cast<int>(current.size());
    int bestLength = 0;
    ScrollAlignment best;
    for (int delta = -std::min(rows - 1, maxScrollDelta); delta <= std::min(rows - 1, maxScrollDelta); ++delta) {
        if (delta == 0)
            continue;
        int runStart = 0;
        int runLength = 0;
        for (int row = 0; row < rows; ++row) {
            const int source = row + delta;
            const bool isMoved = source >= 0 && source < rows && current[static_cast<std::size_t>(row)] == held[static_cast<std::size_t>(source)]
                && current[static_cast<std::size_t>(row)] != held[static_cast<std::size_t>(row)];
            if (!isMoved) {
                runLength = 0;
                continue;
            }
            if (runLength++ == 0)
                runStart = row;
            if (runLength > bestLength) {
                bestLength = runLength;
                best = { .top = std::min(runStart, runStart + delta), .bottom = std::max(row, row + delta), .delta = delta };
            }
        }
    }
    if (bestLength < minScrolledRows)
        return std::nullopt;
    return best;
}

/// Moves hashes as a `ScrollRect` moves rows: a row whose source is outside
/// the rectangle is revealed with hash 0.
///
/// - Parameters:
///   - hashes: One screen's row hashes.
///   - scroll: The scroll.
void applyScroll(std::span<std::uint64_t> hashes, const ScrollAlignment& scroll)
{
    // The source is ahead for a positive delta and behind for a negative
    // delta, so walking in the opposite direction preserves every source.
    for (int index = 0; index <= scroll.bottom - scroll.top; ++index) {
        const int row = scroll.delta > 0 ? scroll.top + index : scroll.bottom - index;
        const int source = row + scroll.delta;
        hashes[static_cast<std::size_t>(row)] = source >= scroll.top && source <= scroll.bottom ? hashes[static_cast<std::size_t>(source)] : 0;
    }
}

/// The cells of a row that differ from what it held, as ranges that never
/// split a wide pair, joined across short unchanged gaps.
///
/// - Parameters:
///   - previous: The cells the row held.
///   - current: Its cells now.
/// - Returns: The `[begin, end)` ranges, ascending.
std::vector<std::pair<int, int>> changedSpans(std::span<const Cell> previous, std::span<const Cell> current)
{
    const int columns = static_cast<int>(current.size());
    std::vector<std::pair<int, int>> spans;
    for (int column = 0; column < columns;) {
        if (std::memcmp(&previous[static_cast<std::size_t>(column)], &current[static_cast<std::size_t>(column)], sizeof(Cell)) == 0) {
            ++column;
            continue;
        }
        int begin = column;
        int end = column + 1;
        while (end < columns) {
            int gap = end;
            while (gap < columns && gap - end < spanJoinGap && std::memcmp(&previous[static_cast<std::size_t>(gap)], &current[static_cast<std::size_t>(gap)], sizeof(Cell)) == 0)
                ++gap;
            if (gap == columns || gap - end >= spanJoinGap)
                break;
            end = gap + 1;
        }
        // A wide pair travels as one unit, so a span takes both of its cells.
        const auto isWideLead = [&](int at) { return current[static_cast<std::size_t>(at)].pen.attributes.contains(Ubiquitty::VT::Attribute::Wide) || previous[static_cast<std::size_t>(at)].pen.attributes.contains(Ubiquitty::VT::Attribute::Wide); };
        if (begin > 0 && isWideLead(begin - 1))
            --begin;
        if (end < columns && isWideLead(end - 1))
            ++end;
        if (!spans.empty() && begin <= spans.back().second)
            spans.back().second = std::max(spans.back().second, end);
        else
            spans.emplace_back(begin, end);
        column = end;
    }
    return spans;
}

/// Whether a byte names an update op.
///
/// - Parameter op: The byte.
/// - Returns: Whether `applyRowUpdate` reads it.
bool isKnownUpdateOp(std::uint8_t op)
{
    switch (static_cast<UpdateOp>(op)) {
    case UpdateOp::DefinePen:
    case UpdateOp::DefineLink:
    case UpdateOp::DefineCluster:
    case UpdateOp::Row:
    case UpdateOp::InactiveRow:
    case UpdateOp::Block:
    case UpdateOp::DefineRichText:
    case UpdateOp::Spans:
    case UpdateOp::ScrollRect:
    case UpdateOp::EchoContext:
        return true;
    }
    return false;
}

namespace {

/// Writes an `EchoContext` op.
void putEchoContext(WireWriter& writer, const EchoContext& context)
{
    writer.u8(std::to_underlying(UpdateOp::EchoContext));
    writer.u32(context.modesRevision);
    writer.u32(context.foregroundGeneration);
    writer.u8(context.flags.toRaw());
    const InterruptCharacters& characters = context.interruptCharacters;
    writer.u8(static_cast<std::uint8_t>((characters.interrupt ? 1 : 0) | (characters.quit ? 2 : 0) | (characters.suspend ? 4 : 0) | (context.eraseCharacter ? 8 : 0) | 16));
    writer.u8(characters.interrupt.value_or(0));
    writer.u8(characters.quit.value_or(0));
    writer.u8(characters.suspend.value_or(0));
    if (context.eraseCharacter)
        writer.u8(*context.eraseCharacter);
}

/// Reads an `EchoContext` op's fields, failing the reader on a flag no
/// version defines.
EchoContext takeEchoContext(WireReader& reader)
{
    EchoContext context;
    context.modesRevision = reader.u32();
    context.foregroundGeneration = reader.u32();
    const std::uint8_t flags = reader.u8();
    const std::uint8_t present = reader.u8();
    const std::uint8_t interrupt = reader.u8();
    const std::uint8_t quit = reader.u8();
    const std::uint8_t suspend = reader.u8();
    if ((flags & ~0x07) != 0 || (present & ~0x1f) != 0 || ((present & 8) && !(present & 16))) {
        reader.fail();
        return context;
    }
    if (present & 16)
        context.eraseCharacter = (present & 8) ? std::optional { reader.u8() } : std::nullopt;
    context.flags = EchoFlags::fromRaw(flags);
    context.interruptCharacters = { .interrupt = (present & 1) ? std::optional { interrupt } : std::nullopt, .quit = (present & 2) ? std::optional { quit } : std::nullopt, .suspend = (present & 4) ? std::optional { suspend } : std::nullopt };
    return context;
}

} // namespace

/// Moves a screen's rows as a `ScrollRect` says, cells and row facts
/// together; a revealed row is blank with default facts, for a later op of the
/// same update to write.
///
/// - Parameters:
///   - terminal: The client's terminal.
///   - screen: `0` primary, `1` alternate.
///   - columns: The grid's width.
///   - scroll: The scroll, checked against the grid.
void commitScroll(Ubiquitty::Terminal& terminal, int screen, int columns, const ScrollAlignment& scroll)
{
    const ScreenMode mode = screen == 1 ? ScreenMode::Alternate : ScreenMode::Primary;
    const auto height = static_cast<std::size_t>(scroll.bottom - scroll.top + 1);
    std::vector<Cell> cells(height * static_cast<std::size_t>(columns));
    std::vector<Ubiquitty::RowMeta> metas(height);
    for (int row = scroll.top; row <= scroll.bottom; ++row) {
        const auto index = static_cast<std::size_t>(row - scroll.top);
        terminal.grid->readRow(mode, Ubiquitty::Row(row), cells.data() + index * static_cast<std::size_t>(columns));
        metas[index] = terminal.grid->rowMetaAt(screen, row);
    }
    const std::vector<Cell> blank(static_cast<std::size_t>(columns), defaultBlankCell);
    for (int row = scroll.top; row <= scroll.bottom; ++row) {
        const int source = row + scroll.delta;
        const bool isRevealed = source < scroll.top || source > scroll.bottom;
        const Cell* from = isRevealed ? blank.data() : cells.data() + static_cast<std::size_t>(source - scroll.top) * static_cast<std::size_t>(columns);
        terminal.grid->setRowSpan(mode, Ubiquitty::Row(row), Ubiquitty::Column(0), Ubiquitty::RowView { from, columns });
        terminal.grid->rowMetaAt(screen, row) = isRevealed ? Ubiquitty::RowMeta {} : metas[static_cast<std::size_t>(source - scroll.top)];
    }
}

} // namespace

UpdateOutcome ScreenPublisher::rowUpdate(const Ubiquitty::Terminal& terminal, AttachmentBaseline& attachment, std::vector<std::uint8_t>& body, ChainOps chainOps, const EchoContext* echoContext)
{
    const auto rowCount = static_cast<std::size_t>(m_rows);
    if (!m_isCaptured || attachment.dictionaryEpoch != m_dictionaries.epoch() || attachment.columns != m_columns
        || attachment.rowHashes[0].size() != rowCount || attachment.rowHashes[1].size() != rowCount)
        return UpdateOutcome::NeedsCheckpoint;

    const std::size_t activeIndex = m_isAlternate ? 1 : 0;
    // What the client holds, as the ops before the rows leave it: a screen
    // switch keeps each screen's rows, a scroll moves the active one's.
    std::array<std::vector<std::uint64_t>, 2> held = attachment.rowHashes;
    std::optional<ScrollAlignment> scroll;
    if (chainOps.contains(ChainOp::ScrollRect) && attachment.isAlternate == m_isAlternate) {
        scroll = alignScroll(held[activeIndex], hashesOf(m_screens[activeIndex]));
        if (scroll)
            applyScroll(held[activeIndex], *scroll);
    }

    std::vector<std::uint8_t> rowOps;
    WireWriter rowWriter(rowOps);
    std::size_t rowOpCount = 0;
    std::size_t rowsSent = 0;
    std::size_t spansSent = 0;
    std::vector<std::uint8_t> spansOp;
    std::vector<std::uint8_t> fragment;
    std::vector<Cell> fragmentCells;
    for (const std::size_t screen : { activeIndex, 1 - activeIndex }) {
        const RowEncodingCache& rows = m_screens[screen];
        for (int row = 0; row < m_rows; ++row) {
            const std::uint64_t holds = held[screen][static_cast<std::size_t>(row)];
            if (rows.hash(row) == holds)
                continue;
            if (rows.hasRunTextAnchor(row))
                return UpdateOutcome::NeedsCheckpoint;
            ++rowOpCount;
            ++rowsSent;
            const std::span<const std::uint8_t> record = rows.record(row);
            // A row the client holds as the encoding before this one, with the
            // same row facts, may travel as the cells that changed.
            const bool isScrolled = scroll && screen == activeIndex && row >= scroll->top && row <= scroll->bottom;
            const std::uint64_t* previous = rows.previousHash(row);
            if (chainOps.contains(ChainOp::Spans) && !isScrolled && previous && *previous == holds && rows.previousAttributes(row) == rows.attributes(row)) {
                spansOp.clear();
                WireWriter spansWriter(spansOp);
                spansWriter.u8(std::to_underlying(UpdateOp::Spans));
                spansWriter.u8(screen == activeIndex ? 0 : 1);
                spansWriter.varint(static_cast<std::uint64_t>(row));
                spansWriter.u64(holds);
                spansWriter.u64(rows.hash(row));
                const std::vector<std::pair<int, int>> spans = changedSpans(rows.previousCells(row), rows.cells(row));
                spansWriter.varint(spans.size());
                bool isEncoded = !spans.empty();
                for (const auto& [begin, end] : spans) {
                    fragment.clear();
                    fragmentCells.assign(rows.cells(row).begin() + begin, rows.cells(row).begin() + end);
                    if (!encodeRowRecord(terminal, fragmentCells, rows.attributes(row), m_dictionaries, RunTextDefinitions::Present, fragment)) {
                        isEncoded = false;
                        break;
                    }
                    spansWriter.u16(static_cast<std::uint16_t>(begin));
                    spansWriter.u16(static_cast<std::uint16_t>(end - begin));
                    spansWriter.bytes(fragment);
                }
                std::vector<std::uint8_t> rowOp;
                WireWriter rowOpWriter(rowOp);
                rowOpWriter.u8(0);
                rowOpWriter.varint(static_cast<std::uint64_t>(row));
                rowOpWriter.bytes(record);
                if (isEncoded && spansOp.size() < rowOp.size()) {
                    rowOps.insert(rowOps.end(), spansOp.begin(), spansOp.end());
                    ++spansSent;
                    continue;
                }
            }
            rowWriter.u8(std::to_underlying(screen == activeIndex ? UpdateOp::Row : UpdateOp::InactiveRow));
            rowWriter.varint(static_cast<std::uint64_t>(row));
            rowWriter.bytes(record);
        }
    }
    // A screen switch moves which screen each block's cursor belongs to, so
    // every block travels with it.
    std::vector<std::size_t> blocks;
    for (std::size_t block = 0; block < stateBlockCount; ++block) {
        if (attachment.isAlternate != m_isAlternate || attachment.blockHashes[block] != m_blockHashes[block])
            blocks.push_back(block);
    }

    // The documents of blocks placed since the attachment's last body; a
    // serial names one placement, so a document it holds is never owed again.
    std::vector<const RichText::Block*> owedDocuments;
    for (const ScreenMode screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        for (const RichText::Block& block : terminal.richTextBlocks.blocks(screen)) {
            if (!std::ranges::binary_search(attachment.richTextSerials, block.serial))
                owedDocuments.push_back(&block);
        }
    }

    std::vector<std::uint8_t> output;
    WireWriter writer(output);
    writer.u8(m_isAlternate ? 1 : 0);
    writer.u16(static_cast<std::uint16_t>(m_rows));
    writer.u16(static_cast<std::uint16_t>(m_columns));
    writer.u32(m_dictionaries.epoch());
    const std::span<const Pen> pens = m_dictionaries.pens();
    const std::span<const LinkDefinition> links = m_dictionaries.links();
    const std::size_t clusterCount = m_dictionaries.clusterCount();
    writer.varint((echoContext ? 1 : 0) + (pens.size() - attachment.marks.pens) + (links.size() - attachment.marks.links)
        + (clusterCount - attachment.marks.clusters) + owedDocuments.size() + blocks.size() + (scroll ? 1 : 0) + rowOpCount);
    if (echoContext)
        putEchoContext(writer, *echoContext);
    for (std::size_t id = attachment.marks.pens; id < pens.size(); ++id) {
        writer.u8(std::to_underlying(UpdateOp::DefinePen));
        writer.varint(id);
        writePen(writer, pens[id]);
    }
    for (std::size_t index = attachment.marks.links; index < links.size(); ++index) {
        writer.u8(std::to_underlying(UpdateOp::DefineLink));
        writeLinkDefinition(writer, links[index]);
    }
    const std::span<const ClusterPool::Entry> clusters = terminal.clusterPool.entries();
    for (std::size_t slot = attachment.marks.clusters; slot < clusterCount; ++slot) {
        writer.u8(std::to_underlying(UpdateOp::DefineCluster));
        writer.varint(slot + 1);
        writeCodePoints(writer, { clusters[slot].codePoints.data(), clusters[slot].length });
    }
    for (const RichText::Block* block : owedDocuments) {
        writer.u8(std::to_underlying(UpdateOp::DefineRichText));
        writer.varint(block->serial);
        writeDocument(writer, block->document);
    }
    for (const std::size_t block : blocks) {
        writer.u8(std::to_underlying(UpdateOp::Block));
        writer.u8(static_cast<std::uint8_t>(block));
        writer.bytes(m_blocks[block]);
    }
    if (scroll) {
        writer.u8(std::to_underlying(UpdateOp::ScrollRect));
        writer.u8(0);
        writer.u16(static_cast<std::uint16_t>(scroll->top));
        writer.u16(static_cast<std::uint16_t>(scroll->bottom));
        writer.svarint(scroll->delta);
    }
    output.insert(output.end(), rowOps.begin(), rowOps.end());
    std::array<std::vector<std::uint64_t>, 2> hashes { hashesOf(m_screens[0]), hashesOf(m_screens[1]) };
    putDigest(writer, stateDigest(m_isAlternate ? ScreenMode::Alternate : ScreenMode::Primary, m_rows, m_columns, hashes[activeIndex], hashes[1 - activeIndex], m_blockHashes));

    body.insert(body.end(), output.begin(), output.end());
    m_lastUpdateRows = rowsSent;
    m_lastUpdateSpans = spansSent;
    m_lastUpdateScrollRects = scroll ? 1 : 0;
    attachment.marks = { .pens = pens.size(), .links = links.size(), .clusters = clusterCount };
    attachment.isAlternate = m_isAlternate;
    attachment.rowHashes = std::move(hashes);
    attachment.blockHashes = m_blockHashes;
    attachment.richTextSerials = richTextSerials(terminal.richTextBlocks);
    return UpdateOutcome::Encoded;
}

Digest128 screenHashesDigest(const std::array<std::span<const std::uint64_t>, 2>& rowHashes, const StateBlockHashes& blockHashes)
{
    assert(rowHashes[0].size() <= static_cast<std::size_t>(maxRows) && rowHashes[1].size() <= static_cast<std::size_t>(maxRows));
    std::array<std::uint8_t, 8 + (2 * maxRows + stateBlockCount) * 8> bytes;
    std::size_t size = 0;
    const auto number = [&](std::uint64_t value, int length) {
        for (int index = 0; index < length; ++index)
            bytes[size++] = static_cast<std::uint8_t>(value >> (8 * index));
    };
    for (const auto screen : rowHashes) {
        number(screen.size(), 4);
        for (const std::uint64_t hash : screen)
            number(hash, 8);
    }
    for (const std::uint64_t hash : blockHashes)
        number(hash, 8);
    return digestOf(std::span(bytes).first(size));
}

Digest128 baselineDigest(const AttachmentBaseline& baseline)
{
    return screenHashesDigest({ baseline.rowHashes[0], baseline.rowHashes[1] }, baseline.blockHashes);
}

Digest128 replicaDigest(const ScreenReplica& replica)
{
    return screenHashesDigest(replica.rowHashes(), replica.blockHashes);
}

std::optional<std::size_t> checkpointCellStorageBytes(std::span<const std::uint8_t> body)
{
    WireReader reader(body);
    CheckpointImage image;
    if (!readCheckpointHeader(reader, image))
        return std::nullopt;
    return decodedScreenCellBytes(image.rows, image.columns);
}

struct PreparedCheckpoint::Data {
    CheckpointImage image;
    std::unique_ptr<Terminal> scratch;
    DecodedStatusRow statusRow;
    StateBlockHashes blockHashes;
    std::size_t preparationStorageCost = 0;
    std::size_t maxStorageCost = std::numeric_limits<std::size_t>::max();
};

PreparedCheckpoint::PreparedCheckpoint(std::unique_ptr<Data> data)
    : m_data(std::move(data))
{
}

PreparedCheckpoint::~PreparedCheckpoint() = default;
PreparedCheckpoint::PreparedCheckpoint(PreparedCheckpoint&&) noexcept = default;
PreparedCheckpoint& PreparedCheckpoint::operator=(PreparedCheckpoint&&) noexcept = default;

std::uint64_t PreparedCheckpoint::generation() const
{
    assert(m_data);
    return m_data->image.generation;
}

int PreparedCheckpoint::rows() const
{
    assert(m_data);
    return m_data->image.rows;
}

int PreparedCheckpoint::columns() const
{
    assert(m_data);
    return m_data->image.columns;
}

std::uint32_t PreparedCheckpoint::historyEpoch() const
{
    assert(m_data);
    return m_data->image.historyEpoch;
}

std::uint64_t PreparedCheckpoint::historyHead() const
{
    assert(m_data);
    return m_data->image.historyHead;
}

Digest128 PreparedCheckpoint::stateDigest() const
{
    assert(m_data);
    return m_data->image.digest;
}

std::expected<PreparedCheckpoint, ApplyOutcome> prepareCheckpoint(std::span<const std::uint8_t> body)
{
    return prepareCheckpoint(body, std::numeric_limits<std::size_t>::max());
}

namespace {

struct EncodedPens {
    std::span<const std::uint8_t> bytes;
    [[nodiscard]] std::size_t size() const { return bytes.size() / penWireBytes; }
    [[nodiscard]] bool empty() const { return bytes.empty(); }
    [[nodiscard]] Pen operator[](std::size_t index) const
    {
        assert(index < size());
        WireReader reader(bytes.subspan(index * penWireBytes, penWireBytes));
        return readPen(reader);
    }
};

struct DecodedCheckpoint {
    CheckpointImage image;
    std::unique_ptr<Terminal> scratch;
    DecodedStatusRow statusRow;
    StateBlockHashes blockHashes;
};

// Planning and preparation use this one field walk. Planning leaves every
// input-sized owner empty; only its fixed scratch Terminal is constructed.
std::expected<DecodedCheckpoint, ApplyOutcome> readCheckpoint(std::span<const std::uint8_t> body,
    std::size_t maxDecodedScreenCellBytes, CheckpointStoragePlan* plan, const CheckpointStoragePlan* capacities = nullptr)
{
    WireReader reader(body);
    CheckpointImage image;
    if (!readCheckpointHeader(reader, image))
        return std::unexpected(ApplyOutcome::Malformed);
    const auto cellBytes = decodedScreenCellBytes(image.rows, image.columns);
    if (!cellBytes || *cellBytes > maxDecodedScreenCellBytes)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    if (plan) {
        plan->rows = image.rows;
        plan->columns = image.columns;
        plan->screenCellBytes = *cellBytes;
        constexpr auto maximum = std::numeric_limits<std::size_t>::max();
        const auto rows = static_cast<std::size_t>(image.rows);
        if (rows > maximum / (2 * sizeof(RowAttributes)) || rows > maximum / (2 * sizeof(std::uint64_t)))
            return std::unexpected(ApplyOutcome::AllocationRefused);
        plan->rowAttributeBytes = 2 * rows * sizeof(RowAttributes);
        plan->rowHashBytes = 2 * rows * sizeof(std::uint64_t);
    }
    for (const StateBlock block : checkpointBlockOrder)
        image.blocks[indexOf(block)] = reader.bytes(reader.remaining());

    // Dictionaries.
    const auto penCount = reader.count(std::numeric_limits<std::uint32_t>::max(), penWireBytes);
    const auto penBegin = reader.offset();
    for (std::size_t index = 0; index < penCount && reader.ok(); ++index)
        (void)readPen(reader);
    const EncodedPens pens { reader.consumed(penBegin, reader.offset()) };
    const std::size_t linkCount = reader.count(Ubiquitty::VT::linkTableCapacity, 4);
    std::array<bool, static_cast<std::size_t>(RunTextStore::maxEntries) + 1> references {};
    std::array<std::uint64_t, 1024> definedLinks {};
    const auto resolves = [&](std::uint64_t cluster) { return cluster < references.size() && references[static_cast<std::size_t>(cluster)]; };
    if (plan) {
        plan->penCount = penCount;
        const auto bytes = ReplicaStorageBuilder::storageBytesForCapacity(penCount, { static_cast<std::size_t>(image.rows), static_cast<std::size_t>(image.rows) });
        if (!bytes)
            return std::unexpected(ApplyOutcome::AllocationRefused);
        plan->replicaStorageBytes = *bytes;
        plan->linkCount = linkCount;
        plan->linkTableBytes = linkCount ? sizeof(Ubiquitty::Attributes::Hyperlinks::LinkTable) : 0;
    } else if (linkCount) {
        image.links.reset(new (std::nothrow) Ubiquitty::Attributes::Hyperlinks::LinkTable);
        if (!image.links)
            return std::unexpected(ApplyOutcome::AllocationRefused);
        image.linkCount = linkCount;
    }
    for (std::size_t index = 0; index < linkCount && reader.ok(); ++index) {
        const auto link = readLinkDefinitionView(reader);
        if (!link || (definedLinks[link->id >> 6] & (std::uint64_t { 1 } << (link->id & 63)))) {
            reader.fail();
            break;
        }
        definedLinks[link->id >> 6] |= std::uint64_t { 1 } << (link->id & 63);
        if (plan) {
            if (!addStorageCount(plan->linkTextBytes, link->key.size()) || !addStorageCount(plan->linkTextBytes, link->uri.size()))
                return std::unexpected(ApplyOutcome::AllocationRefused);
        } else {
            writeLinkSlot(*image.links, index, *link);
            if (image.nextLinkID && link->id >= image.nextLinkID)
                image.nextLinkID = link->id == 0xFFFF ? 0 : static_cast<std::uint16_t>(link->id + 1);
        }
    }
    const auto clusterCount = reader.count(ClusterPool::maxEntries, 2);
    if (plan) {
        plan->clusterCount = clusterCount;
        const auto bytes = ClusterPool::storageBytesForCapacity(clusterCount);
        if (!bytes)
            return std::unexpected(ApplyOutcome::AllocationRefused);
        plan->clusterStorageBytes = *bytes;
    } else if (auto reserved = image.clusters.reserve(clusterCount); !reserved)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    for (std::size_t index = 0; index < clusterCount && reader.ok(); ++index) {
        std::array<std::uint32_t, ClusterPool::maxCodePoints> fixed;
        std::size_t length = 0;
        if (readCodePointsInto(reader, ClusterPool::maxCodePoints, [&](std::size_t count) -> std::span<std::uint32_t> {
                length = count;
                if (plan) {
                    if (!addStorageCount(plan->clusterCodePoints, count))
                        reader.fail();
                }
                return std::span(fixed).first(count);
            })) {
            if (!plan && !image.clusters.appendDecoded(static_cast<std::uint16_t>(index + 1), std::span(fixed).first(length)))
                return std::unexpected(ApplyOutcome::Malformed);
            references[index + 1] = true;
        }
    }
    const std::size_t runTextCount = reader.count(RunTextStore::maxEntries, 3);
    std::size_t previousRunText = 0;
    std::size_t runTextCodePoints = 0;
    if (plan)
        plan->runTextCount = runTextCount;
    else {
        assert(capacities);
        if (!image.runTexts.reserveDecoded(capacities->runTextSlots))
            return std::unexpected(ApplyOutcome::AllocationRefused);
    }
    for (std::size_t index = 0; index < runTextCount && reader.ok(); ++index) {
        const auto id = static_cast<std::size_t>(reader.varint(RunTextStore::maxEntries));
        std::array<std::uint32_t, runTextMaximumCodePoints> fixed;
        std::size_t count = 0;
        if (id <= previousRunText || !readCodePointsInto(reader, runTextMaximumCodePoints, [&](std::size_t length) -> std::span<std::uint32_t> {
                count = length;
                return std::span(fixed).first(length);
            })) {
            reader.fail();
            break;
        }
        if (count > RunTextStore::maxLiveCodePoints - runTextCodePoints) {
            reader.fail();
            break;
        }
        runTextCodePoints += count;
        if (!plan && !image.runTexts.tryAppendDecoded(static_cast<std::uint16_t>(id), std::span(fixed).first(count)))
            return std::unexpected(ApplyOutcome::AllocationRefused);
        references[id] = true;
        previousRunText = id;
    }
    if (plan) {
        plan->runTextSlots = previousRunText;
        plan->runTextCodePoints = runTextCodePoints;
    }
    const auto richTextCount = reader.count(2 * RichText::maxBlocksPerScreen, 6);
    std::array<RichTextDefinitionView, 2 * RichText::maxBlocksPerScreen> documentViews;
    FallibleVector<RichTextDefinition> richTexts;
    if (plan)
        plan->richTextDefinitionCount = richTextCount;
    else if (!richTexts.tryResize(richTextCount))
        return std::unexpected(ApplyOutcome::AllocationRefused);
    for (std::size_t index = 0; index < richTextCount && reader.ok(); ++index) {
        auto& view = documentViews[index];
        view.serial = static_cast<std::uint32_t>(reader.varint(std::numeric_limits<std::uint32_t>::max()));
        const auto begin = reader.offset();
        if (plan) {
            const auto document = readDocumentView(reader);
            if (!document)
                break;
            view.document = *document;
            if (!addStorageCount(plan->richTextCodePoints, document->codePoints) || !addStorageCount(plan->richTextRuns, document->runs)
                || !addStorageCount(plan->richTextFonts, document->fonts))
                return std::unexpected(ApplyOutcome::AllocationRefused);
        } else {
            auto& definition = richTexts[index];
            definition.serial = view.serial;
            if (auto document = tryReadDocument(reader, definition.document); !document)
                return std::unexpected(document.error());
            view.document = { reader.consumed(begin, reader.offset()), definition.document.text.size(), definition.document.runs.size(), definition.document.fonts.size() };
        }
    }
    if (!reader.ok())
        return std::unexpected(ApplyOutcome::Malformed);

    // Both screens' rows, the active one first. Every record is at least its
    // length prefix, three row facts, an extent, a text length and five list
    // counts, so a header naming more rows than the body can hold is refused
    // before the grid it names is allocated.
    constexpr std::size_t minRecordBytes = 1 + 3 + 1 + 1 + 5;
    if (reader.remaining() / minRecordBytes < 2 * static_cast<std::size_t>(image.rows))
        return std::unexpected(ApplyOutcome::Malformed);
    std::array<std::array<std::uint64_t, Ubiquitty::maxRows>, 2> fixedHashes;
    std::array<Cell, Ubiquitty::maxColumns> fixedRow;
    ReplicaStorageBuilder storage;
    if (!plan) {
        const auto cellsPerScreen = *cellBytes / (2 * sizeof(Cell));
        for (auto& cells : image.cells) {
            cells.reset(new (std::nothrow) Cell[cellsPerScreen]);
            if (!cells)
                return std::unexpected(ApplyOutcome::AllocationRefused);
            std::fill_n(cells.get(), cellsPerScreen, defaultBlankCell);
        }
        auto created = ReplicaStorageBuilder::create(penCount, { static_cast<std::size_t>(image.rows), static_cast<std::size_t>(image.rows) }, penCount);
        if (!created)
            return std::unexpected(ApplyOutcome::AllocationRefused);
        storage = std::move(*created);
        for (std::size_t index = 0; index < penCount; ++index)
            storage.pens()[index] = pens[index];
    }
    for (int screen = 0; screen < 2; ++screen) {
        const auto index = static_cast<std::size_t>(screen);
        if (!plan) {
            image.attributes[index].reset(new (std::nothrow) RowAttributes[static_cast<std::size_t>(image.rows)]);
            if (!image.attributes[index])
                return std::unexpected(ApplyOutcome::AllocationRefused);
        }
        for (int row = 0; row < image.rows; ++row) {
            const std::span<const std::uint8_t> record = reader.bytes(reader.remaining());
            if (!reader.ok())
                return std::unexpected(ApplyOutcome::Malformed);
            WireReader recordReader(record);
            const auto cells = plan ? std::span(fixedRow).first(static_cast<std::size_t>(image.columns))
                                    : std::span<Cell>(image.cells[index].get() + static_cast<std::size_t>(row) * static_cast<std::size_t>(image.columns), static_cast<std::size_t>(image.columns));
            RowAttributes fixedAttributes;
            auto& attributes = plan ? fixedAttributes : image.attributes[index][static_cast<std::size_t>(row)];
            const bool isRowValid = decodeRowRecordWithNames(recordReader, pens, resolves, cells, attributes);
            if (!isRowValid || !recordReader.isAtEnd())
                return std::unexpected(ApplyOutcome::Malformed);
            const auto hash = rowHash(record);
            fixedHashes[index][static_cast<std::size_t>(row)] = hash;
            if (!plan)
                storage.rowHashes()[screen == 0 ? static_cast<std::size_t>(image.active) : 1 - static_cast<std::size_t>(image.active)][static_cast<std::size_t>(row)] = hash;
        }
    }

    // The history's address; its lines travel on the history plane.
    image.historyEpoch = reader.u32();
    image.historyHead = reader.u64();
    {
        WireReader parserGroundReader(reader.bytes(maxParserGroundBytes));
        parserGroundFields(DecodeVisit { parserGroundReader }, image.precedingItem, image.precedingItem);
        if (!parserGroundReader.ok() || !parserGroundReader.isAtEnd() || !isValidPrecedingItem(image.precedingItem))
            reader.fail();
    }
    image.digest = takeDigest(reader);
    if (!reader.ok() || !reader.isAtEnd())
        return std::unexpected(ApplyOutcome::Malformed);

    // The blocks, decoded into a scratch terminal whose members carry the
    // same names and types as the client's, then checked.
    std::unique_ptr<Terminal> scratch(new (std::nothrow) Terminal(TerminalInitialization::StateOnly));
    if (!scratch)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    if (plan)
        plan->validationScratchBytes = sizeof(Terminal);
    DecodedStatusRow statusRow;
    for (const StateBlock block : decodeOrder) {
        const auto decoded = decodeStateBlockWithNames(block, image.blocks[indexOf(block)], *scratch, image.active, image.rows, image.columns, pens, resolves, statusRow, plan);
        if (!decoded)
            return std::unexpected(decoded.error());
    }
    // A checkpoint defines every document it places.
    const auto placements = richTextPlacementStorage(image.blocks[indexOf(StateBlock::RichText)], image.rows, image.columns,
        std::span(documentViews).first(richTextCount));
    if (!placements)
        return std::unexpected(ApplyOutcome::Malformed);
    if (plan) {
        plan->richTextBlocks = placements->blocks;
        plan->placedRichTextCodePoints = placements->codePoints;
        plan->placedRichTextRuns = placements->runs;
        plan->placedRichTextFonts = placements->fonts;
        plan->placedRichTextRectangles = placements->visibleRectangles;
    } else if (auto richText = tryReadRichTextBlock(image.blocks[indexOf(StateBlock::RichText)], image.rows, image.columns, richTexts, RichText::BlockStore {}, scratch->richTextBlocks); !richText)
        return std::unexpected(richText.error());
    StateBlockHashes blockHashes {};
    for (std::size_t block = 0; block < stateBlockCount; ++block)
        blockHashes[block] = blockHash(image.blocks[block]);

    // The image holds the active screen's rows first, as the body does.
    std::array<std::uint8_t, 5 + 8 * (2 * Ubiquitty::maxRows + stateBlockCount)> digestBytes;
    std::size_t digestSize = 0;
    writeStateDigestInput(image.active, image.rows, image.columns,
        std::span(fixedHashes[0]).first(static_cast<std::size_t>(image.rows)),
        std::span(fixedHashes[1]).first(static_cast<std::size_t>(image.rows)), blockHashes,
        [&](std::uint8_t byte) {
            assert(digestSize < digestBytes.size());
            digestBytes[digestSize++] = byte;
        });
    const Digest128 digest = digestOf(std::span(digestBytes).first(digestSize));
    if (digest != image.digest)
        return std::unexpected(ApplyOutcome::DigestMismatch);
    if (!plan)
        image.storage = std::move(storage).publish();

    // Only decoding and hashing borrow these spans. The prepared owner must
    // remain valid after the carrier or sealed plaintext has been released.
    image.blocks = {};
    return DecodedCheckpoint { std::move(image), std::move(scratch), std::move(statusRow), blockHashes };
}

} // namespace

std::expected<CheckpointStoragePlan, ApplyOutcome> checkpointStoragePlan(std::span<const std::uint8_t> body)
{
    CheckpointStoragePlan plan;
    const auto decoded = readCheckpoint(body, std::numeric_limits<std::size_t>::max(), &plan, nullptr);
    if (!decoded)
        return std::unexpected(decoded.error());
    std::size_t cost = sizeof(PreparedCheckpoint::Data);
    const auto add = [&](std::size_t bytes) { return addStorageCount(cost, bytes); };
    const auto addElements = [&](std::size_t count, std::size_t bytes) {
        if (count > std::numeric_limits<std::size_t>::max() / bytes)
            return false;
        return add(count * bytes);
    };
    // Row hashes belong to replica storage, hyperlink bytes to the fixed
    // ring, and cluster code points to fixed pool entries. Charging their
    // component counts again would count one owner twice.
    if (!add(plan.screenCellBytes) || !add(plan.rowAttributeBytes) || !add(plan.replicaStorageBytes)
        || !add(plan.linkTableBytes) || !add(plan.clusterStorageBytes) || !add(plan.userDefinedKeysBytes) || !add(plan.alignedTabBytes) || !add(plan.paletteStackBytes)
        || !add(plan.validationScratchBytes)
        || !addElements(plan.runTextSlots, sizeof(FallibleVector<std::uint32_t>))
        || !addElements(plan.runTextSlots, sizeof(std::uint16_t))
        || !addElements(plan.runTextCodePoints, sizeof(std::uint32_t))
        || !addElements(plan.richTextDefinitionCount, sizeof(RichTextDefinition))
        || !addElements(plan.richTextCodePoints, sizeof(std::uint32_t))
        || !addElements(plan.richTextRuns, sizeof(RichText::Run))
        || !addElements(plan.richTextFonts, sizeof(RichText::FontEntry))
        || !addElements(plan.richTextBlocks[0], sizeof(RichText::Block))
        || !addElements(plan.richTextBlocks[1], sizeof(RichText::Block))
        || !addElements(plan.placedRichTextCodePoints, sizeof(std::uint32_t))
        || !addElements(plan.placedRichTextRuns, sizeof(RichText::Run))
        || !addElements(plan.placedRichTextFonts, sizeof(RichText::FontEntry))
        || !addElements(plan.placedRichTextRectangles, sizeof(RichText::VisibleRect))
        || !addElements(plan.activeContexts, sizeof(OSC::Context))
        || !addElements(plan.statusRecords, sizeof(OSC::ProgramStatus))
        || !add(plan.stateStringBytes) || !add(plan.stateStringCount)
        || (plan.hasStatusRow && !add(sizeof(Ubiquitty::Rendering::StatusRowStorage))))
        return std::unexpected(ApplyOutcome::AllocationRefused);
    plan.preparationStorageCost = cost;
    return plan;
}

std::expected<std::size_t, ApplyOutcome> checkpointStorageCost(const CheckpointStoragePlan& plan, const Ubiquitty::Terminal& destination)
{
    if (!destination.grid)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    const auto backing = destination.grid->backingStoragePlan(plan.rows, plan.columns);
    if (!backing)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    auto cost = plan.preparationStorageCost;
    if (!addStorageCount(cost, backing->peakBytes))
        return std::unexpected(ApplyOutcome::AllocationRefused);
    return cost;
}

std::expected<PreparedCheckpoint, ApplyOutcome> prepareCheckpoint(std::span<const std::uint8_t> body, std::size_t maxDecodedScreenCellBytes)
{
    const auto cellBytes = checkpointCellStorageBytes(body);
    if (!cellBytes)
        return std::unexpected(ApplyOutcome::Malformed);
    if (*cellBytes > maxDecodedScreenCellBytes)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    const auto plan = checkpointStoragePlan(body);
    if (!plan)
        return std::unexpected(plan.error());
    auto decoded = readCheckpoint(body, maxDecodedScreenCellBytes, nullptr, &*plan);
    if (!decoded)
        return std::unexpected(decoded.error());
    std::unique_ptr<PreparedCheckpoint::Data> data(new (std::nothrow) PreparedCheckpoint::Data {
        .image = std::move(decoded->image), .scratch = std::move(decoded->scratch), .statusRow = std::move(decoded->statusRow), .blockHashes = decoded->blockHashes });
    if (!data)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    return PreparedCheckpoint(std::move(data));
}

std::expected<PreparedCheckpoint, ApplyOutcome> prepareCheckpoint(std::span<const std::uint8_t> body,
    const Ubiquitty::Terminal& destination, CheckpointPreparationLimits limits)
{
    const auto cellBytes = checkpointCellStorageBytes(body);
    if (!cellBytes)
        return std::unexpected(ApplyOutcome::Malformed);
    if (*cellBytes > limits.maxDecodedScreenCellBytes)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    std::size_t minimumStorage = sizeof(PreparedCheckpoint::Data);
    if (!addStorageCount(minimumStorage, sizeof(Terminal)) || !addStorageCount(minimumStorage, *cellBytes)
        || minimumStorage > limits.maxStorageCost)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    const auto plan = checkpointStoragePlan(body);
    if (!plan)
        return std::unexpected(plan.error());
    const auto cost = checkpointStorageCost(*plan, destination);
    if (!cost || *cost > limits.maxStorageCost)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    auto decoded = readCheckpoint(body, limits.maxDecodedScreenCellBytes, nullptr, &*plan);
    if (!decoded)
        return std::unexpected(decoded.error());
    std::unique_ptr<PreparedCheckpoint::Data> data(new (std::nothrow) PreparedCheckpoint::Data {
        .image = std::move(decoded->image), .scratch = std::move(decoded->scratch), .statusRow = std::move(decoded->statusRow), .blockHashes = decoded->blockHashes, .preparationStorageCost = plan->preparationStorageCost, .maxStorageCost = limits.maxStorageCost });
    if (!data)
        return std::unexpected(ApplyOutcome::AllocationRefused);
    return PreparedCheckpoint(std::move(data));
}

ApplyOutcome reserveCheckpointDestination(Ubiquitty::Terminal& terminal, const PreparedCheckpoint& checkpoint)
{
    assert(checkpoint.m_data);
    const auto& data = *checkpoint.m_data;
    if (!terminal.grid || data.preparationStorageCost > data.maxStorageCost
        || !terminal.grid->reserveCapacity(data.image.rows, data.image.columns, data.maxStorageCost - data.preparationStorageCost))
        return ApplyOutcome::AllocationRefused;
    return ApplyOutcome::Applied;
}

ApplyOutcome commitCheckpoint(Ubiquitty::Terminal& terminal, PreparedCheckpoint checkpoint, ScreenReplica& replica)
{
    assert(checkpoint.m_data);
    CheckpointImage& image = checkpoint.m_data->image;
    Terminal& scratch = *checkpoint.m_data->scratch;
    DecodedStatusRow& statusRow = checkpoint.m_data->statusRow;
    const StateBlockHashes& blockHashes = checkpoint.m_data->blockHashes;
    const int active = image.active == ScreenMode::Alternate ? 1 : 0;

    // Resize preserves the old geometry when its grid allocation is refused.
    // Reserve before committing so none of the following row writes can use
    // checkpoint dimensions against that smaller grid.
    if (const auto outcome = reserveCheckpointDestination(terminal, checkpoint); outcome != ApplyOutcome::Applied)
        return outcome;

    // Commit only after the requested grid capacity is held.
    if (terminal.rows != image.rows || terminal.columns != image.columns)
        Ubiquitty::resize(terminal, image.rows, image.columns, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::Screen::resetScrollbackStore(&terminal);
    terminal.grid->setAlternateActive(image.active);
    terminal.screens.activate(image.active == ScreenMode::Alternate ? Ubiquitty::Modes::ScreenBuffer::Alternate : Ubiquitty::Modes::ScreenBuffer::Primary);

    // All carried link bytes already live in the private fixed ring. Moving it
    // cannot allocate after the destination's cells/history have been changed.
    terminal.linkTable = std::move(image.links);
    terminal.linkHead = 0;
    terminal.linkCount = static_cast<int>(image.linkCount);
    terminal.currentLinkID = 0;
    terminal.linkNextID = image.nextLinkID;
    terminal.linkFreeIDs.clear();
    terminal.linkReclaimBackoffMints = 0;
    terminal.clusterPool = std::move(image.clusters);
    Ubiquitty::Screen::adoptRunTextStore(terminal, std::move(image.runTexts));

    // A keyframe replaces every row of both screens: a pinned line on either
    // may now hold anything.
    Ubiquitty::Screen::notePositionsScreenReplaced(terminal, Ubiquitty::Screen::Buffer::Primary);
    Ubiquitty::Screen::notePositionsScreenReplaced(terminal, Ubiquitty::Screen::Buffer::Alternate);
    for (int screen = 0; screen < 2; ++screen) {
        const ScreenMode mode = screen == 1 ? ScreenMode::Alternate : ScreenMode::Primary;
        const std::size_t source = screen == active ? 0 : 1;
        for (int row = 0; row < image.rows; ++row) {
            const Cell* cells = image.cells[source].get() + static_cast<std::size_t>(row) * static_cast<std::size_t>(image.columns);
            terminal.grid->setRowSpan(mode, Ubiquitty::Row(row), Ubiquitty::Column(0), Ubiquitty::RowView { cells, image.columns });
            Ubiquitty::RowMeta& meta = terminal.grid->rowMetaAt(screen, row);
            const RowAttributes& attributes = image.attributes[source][static_cast<std::size_t>(row)];
            meta.lineAttribute = attributes.lineAttribute;
            meta.set(Ubiquitty::RowFlag::Wrapped, attributes.isWrapped);
            meta.oscMark = attributes.mark;
        }
    }
    for (const StateBlock block : decodeOrder)
        commitStateBlock(block, terminal, scratch, image.active, statusRow);
    // A checkpoint replaces the picture that gave a block serial its meaning.
    // A terminal lent by a client may have cached a different block with the
    // same serial; retaining that layout would draw the old document.
    terminal.richTextLayouts = {};
    commitStateBlock(StateBlock::RichText, terminal, scratch, image.active, statusRow);
    // Flowed is derived from the cells and their text-size specs. Resolve it
    // only after TextState installs this checkpoint's table; the previous
    // table may be empty or use the same slots for ordinary sized text.
    for (int screen = 0; screen < 2; ++screen) {
        for (int row = 0; row < image.rows; ++row)
            Ubiquitty::Screen::latchFlowedRow(terminal, screen, row);
    }
    terminal.parser.precedingItem = image.precedingItem;
    if (Ubiquitty::Screen::undrawnTextSizeSpecIDs(terminal, terminal.textSizeTable))
        Ubiquitty::Screen::flattenSizedGridCells(terminal);
    terminal.grid->requestFullRedraw();

    // The row hashes are kept per screen, primary first.
    replica.generation = image.generation;
    replica.dictionaryEpoch = image.dictionaryEpoch;
    replica.storage = std::move(image.storage);
    replica.maxPens = replica.pens().size() > std::numeric_limits<std::size_t>::max() - maxPensPerEpoch
        ? std::numeric_limits<std::size_t>::max()
        : replica.pens().size() + maxPensPerEpoch;
    replica.blockHashes = blockHashes;
    replica.historyEpoch = image.historyEpoch;
    replica.historyHead = image.historyHead;
    replica.stateDigest = image.digest;
    return ApplyOutcome::Applied;
}

ApplyOutcome applyCheckpoint(Ubiquitty::Terminal& terminal, std::span<const std::uint8_t> body, ScreenReplica& replica)
{
    auto prepared = prepareCheckpoint(body);
    if (!prepared)
        return prepared.error();
    return commitCheckpoint(terminal, std::move(*prepared), replica);
}

bool copyPicture(const Ubiquitty::Terminal& source, Ubiquitty::Terminal& destination, CheckpointPreparationLimits limits)
{
    ScreenPublisher publisher;
    AttachmentBaseline baseline;
    std::vector<std::uint8_t> body;
    if (!publisher.capture(source) || !publisher.checkpoint(source, 1, body, baseline))
        return false;
    ScreenReplica drawn;
    if (applyCheckpoint(destination, body, drawn, limits) != ApplyOutcome::Applied)
        return false;
    destination.replyAuthority = ReplyAuthority::Elsewhere; // its host answers
    return true;
}

ApplyOutcome applyCheckpoint(Ubiquitty::Terminal& terminal, std::span<const std::uint8_t> body, ScreenReplica& replica, CheckpointPreparationLimits limits)
{
    auto prepared = prepareCheckpoint(body, terminal, limits);
    if (!prepared)
        return prepared.error();
    return commitCheckpoint(terminal, std::move(*prepared), replica);
}

ApplyOutcome applyRowUpdate(Ubiquitty::Terminal& terminal, std::span<const std::uint8_t> body, ScreenReplica& replica, std::vector<std::uint64_t>* refusedRowHashes)
{
    WireReader reader(body);
    const std::uint8_t activeScreen = reader.u8();
    const int rows = reader.u16();
    const int columns = reader.u16();
    const std::uint32_t epoch = reader.u32();
    if (!reader.ok() || activeScreen > 1)
        return ApplyOutcome::Malformed;
    const auto rowCount = static_cast<std::size_t>(rows);
    if (!terminal.grid || rows != terminal.rows || columns != terminal.columns || epoch != replica.dictionaryEpoch
        || replica.rowHashes()[0].size() != rowCount || replica.rowHashes()[1].size() != rowCount)
        return ApplyOutcome::BaseMissing;
    const ScreenMode active = activeScreen == 1 ? ScreenMode::Alternate : ScreenMode::Primary;
    const std::size_t activeIndex = activeScreen;

    auto storage = ReplicaStorageBuilder::create(replica.pens().size(), { rowCount, rowCount }, replica.maxPens);
    if (!storage)
        return ApplyOutcome::AllocationRefused;
    std::ranges::copy(replica.pens(), storage->pens().begin());
    auto hashes = storage->rowHashes();
    for (std::size_t screen = 0; screen < 2; ++screen)
        std::ranges::copy(replica.rowHashes()[screen], hashes[screen].begin());
    StateBlockHashes blockHashes = replica.blockHashes;
    std::span<const Pen> pens = storage->pens();
    std::vector<LinkDefinition> links;
    std::optional<ClusterPool> clusters;
    RowReferences references = heldReferences(terminal);
    const std::size_t knownClusters = terminal.clusterPool.entries().size();

    struct RowWrite {
        std::size_t screen;
        int row;
        std::vector<Cell> cells;
        RowAttributes attributes;
    };
    std::vector<RowWrite> writes;
    std::array<int, 2> lastRow { -1, -1 };
    // At most one scroll per screen, ahead of its rows.
    std::array<std::optional<ScrollAlignment>, 2> scrolls;
    bool isBaseMismatch = false;
    std::array<std::span<const std::uint8_t>, stateBlockCount> blocks;
    std::array<bool, stateBlockCount> isBlockPresent {};
    std::vector<RichTextDefinition> richTexts;
    std::optional<EchoContext> echoContext;
    const std::size_t opCount = reader.count(reader.remaining(), 2);
    for (std::size_t index = 0; index < opCount && reader.ok(); ++index) {
        const std::uint8_t op = reader.u8();
        switch (static_cast<UpdateOp>(op)) {
        case UpdateOp::DefinePen: {
            // A superset update defines from the acknowledged watermark, so a
            // replica that holds a later update meets definitions it already
            // has; each must restate what it holds.
            const std::uint64_t id = reader.varint();
            const Pen pen = readPen(reader);
            if (id < pens.size()) {
                if (!isSamePen(pens[static_cast<std::size_t>(id)], pen))
                    reader.fail();
            } else if (id == pens.size() && id < replica.maxPens) {
                if (auto appended = storage->appendPen(pen); !appended)
                    return appended.error() == ReplicaStorageError::AllocationRefused ? ApplyOutcome::AllocationRefused : ApplyOutcome::Malformed;
                pens = storage->pens();
            } else {
                reader.fail();
            }
            break;
        }
        case UpdateOp::DefineLink: {
            std::optional<LinkDefinition> link = readLinkDefinition(reader);
            if (!link) {
                reader.fail();
                break;
            }
            links.push_back(std::move(*link));
            break;
        }
        case UpdateOp::DefineCluster: {
            const std::size_t nextID = (clusters ? clusters->entries().size() : knownClusters) + 1;
            const std::uint64_t id = reader.varint();
            std::vector<std::uint32_t> codePoints;
            if (id == 0 || id > nextID || id > ClusterPool::maxEntries || !readCodePoints(reader, ClusterPool::maxCodePoints, codePoints)) {
                reader.fail();
                break;
            }
            if (id < nextID) {
                // A restated definition, as for pens above.
                const std::span<const std::uint32_t> held = clusters ? clusters->sequence(static_cast<std::uint16_t>(id)) : terminal.clusterPool.sequence(static_cast<std::uint16_t>(id));
                if (!std::ranges::equal(held, codePoints))
                    reader.fail();
                break;
            }
            references.clusters[static_cast<std::size_t>(id)] = true;
            if (!clusters) {
                clusters.emplace();
                if (auto reserved = clusters->reserve(nextID); !reserved)
                    return ApplyOutcome::AllocationRefused;
                for (const auto& entry : terminal.clusterPool.entries()) {
                    if (!clusters->appendDecoded(entry.id, std::span(entry.codePoints).first(entry.length)))
                        return ApplyOutcome::Malformed;
                }
                if (!clusters->restoreContinuation(terminal.clusterPool.continuation()))
                    return ApplyOutcome::Malformed;
            }
            // The next dense id and pool bound were checked above. Appending
            // can now refuse only storage; the pool owns amortized growth.
            if (!clusters->appendDecoded(static_cast<std::uint16_t>(id), codePoints))
                return ApplyOutcome::AllocationRefused;
            break;
        }
        case UpdateOp::Row:
        case UpdateOp::InactiveRow: {
            // Rows of one screen ascend strictly, so an update writes each row
            // at most once and holds at most two screens of cells.
            const std::size_t screen = static_cast<UpdateOp>(op) == UpdateOp::Row ? activeIndex : 1 - activeIndex;
            const auto row = static_cast<int>(reader.varint(static_cast<std::uint64_t>(rows - 1)));
            const std::span<const std::uint8_t> record = reader.bytes(reader.remaining());
            if (!reader.ok() || row <= lastRow[screen]) {
                reader.fail();
                break;
            }
            lastRow[screen] = row;
            RowWrite write { .screen = screen, .row = row, .cells = std::vector<Cell>(static_cast<std::size_t>(columns)), .attributes = {} };
            WireReader recordReader(record);
            if (!decodeRowRecord(recordReader, pens, references, write.cells, write.attributes) || !recordReader.isAtEnd()) {
                reader.fail();
                break;
            }
            hashes[screen][static_cast<std::size_t>(row)] = rowHash(record);
            writes.push_back(std::move(write));
            break;
        }
        case UpdateOp::Spans: {
            const std::uint8_t screenByte = reader.u8();
            const std::size_t screen = screenByte == 0 ? activeIndex : 1 - activeIndex;
            const auto row = static_cast<int>(reader.varint(static_cast<std::uint64_t>(rows - 1)));
            const std::uint64_t baseHash = reader.u64();
            const std::uint64_t newHash = reader.u64();
            const std::size_t spanCount = reader.count(static_cast<std::size_t>(columns), 5);
            const std::optional<ScrollAlignment>& scroll = scrolls[screen];
            if (!reader.ok() || screenByte > 1 || row <= lastRow[screen] || (scroll && row >= scroll->top && row <= scroll->bottom)) {
                reader.fail();
                break;
            }
            lastRow[screen] = row;
            // The spans rewrite the row the client holds; one it does not hold
            // as the host meant is a divergence the digest would report.
            if (hashes[screen][static_cast<std::size_t>(row)] != baseHash)
                isBaseMismatch = true;
            const ScreenMode mode = screen == 1 ? ScreenMode::Alternate : ScreenMode::Primary;
            RowWrite write { .screen = screen, .row = row, .cells = std::vector<Cell>(static_cast<std::size_t>(columns)), .attributes = rowAttributesOf(terminal.grid->rowMetaAt(static_cast<int>(screen), row)) };
            terminal.grid->readRow(mode, Ubiquitty::Row(row), write.cells.data());
            int nextColumn = 0;
            for (std::size_t span = 0; span < spanCount && reader.ok(); ++span) {
                const int column = reader.u16();
                const int length = reader.u16();
                const std::span<const std::uint8_t> record = reader.bytes(reader.remaining());
                if (!reader.ok() || length < 1 || column < nextColumn || length > columns - column) {
                    reader.fail();
                    break;
                }
                nextColumn = column + length;
                RowAttributes spanAttributes;
                WireReader recordReader(record);
                if (!decodeRowRecord(recordReader, pens, references, std::span<Cell>(write.cells).subspan(static_cast<std::size_t>(column), static_cast<std::size_t>(length)), spanAttributes)
                    || !recordReader.isAtEnd())
                    reader.fail();
            }
            if (!reader.ok())
                break;
            hashes[screen][static_cast<std::size_t>(row)] = newHash;
            writes.push_back(std::move(write));
            break;
        }
        case UpdateOp::ScrollRect: {
            const std::uint8_t screenByte = reader.u8();
            const std::size_t screen = screenByte == 0 ? activeIndex : 1 - activeIndex;
            const int top = reader.u16();
            const int bottom = reader.u16();
            const std::int64_t delta = reader.svarint();
            // Checked before any row moves: the rectangle within the screen, a
            // move within it, and ahead of every row op of its screen.
            if (!reader.ok() || screenByte > 1 || top > bottom || bottom >= rows || delta == 0 || delta > bottom - top || delta < top - bottom
                || scrolls[screen] || lastRow[screen] != -1) {
                reader.fail();
                break;
            }
            scrolls[screen] = ScrollAlignment { .top = top, .bottom = bottom, .delta = static_cast<int>(delta) };
            applyScroll(hashes[screen], *scrolls[screen]);
            break;
        }
        case UpdateOp::Block: {
            const std::uint8_t block = reader.u8();
            const std::span<const std::uint8_t> bytes = reader.bytes(reader.remaining());
            if (!reader.ok() || block >= stateBlockCount || isBlockPresent[block]) {
                reader.fail();
                break;
            }
            isBlockPresent[block] = true;
            blocks[block] = bytes;
            blockHashes[block] = blockHash(bytes);
            break;
        }
        case UpdateOp::EchoContext: {
            // At most one, and it changes nothing the digest covers.
            if (echoContext) {
                reader.fail();
                break;
            }
            echoContext = takeEchoContext(reader);
            break;
        }
        case UpdateOp::DefineRichText: {
            RichTextDefinition definition;
            definition.serial = static_cast<std::uint32_t>(reader.varint(std::numeric_limits<std::uint32_t>::max()));
            const bool isRepeated = std::ranges::find(richTexts, definition.serial, &RichTextDefinition::serial) != richTexts.end();
            if (isRepeated || richTexts.size() == 2 * RichText::maxBlocksPerScreen || !readDocument(reader, definition.document)) {
                reader.fail();
                break;
            }
            richTexts.push_back(std::move(definition));
            break;
        }
        }
        if (!isKnownUpdateOp(op))
            reader.fail();
    }
    const Digest128 carried = takeDigest(reader);
    if (!reader.ok() || !reader.isAtEnd())
        return ApplyOutcome::Malformed;

    // A screen switch moves every block with it, so a client never holds a
    // block that describes the other screen's cursor.
    const bool isSwitch = active != (terminal.screens.isUsingAlternateScreen ? ScreenMode::Alternate : ScreenMode::Primary);
    if (isSwitch && std::ranges::find(isBlockPresent, false) != isBlockPresent.end())
        return ApplyOutcome::Malformed;
    if (!replica.scratch)
        replica.scratch = std::make_shared<Terminal>();
    Terminal& scratch = *replica.scratch;
    DecodedStatusRow statusRow;
    for (const StateBlock block : decodeOrder) {
        if (isBlockPresent[indexOf(block)]) {
            if (auto decoded = decodeStateBlock(block, blocks[indexOf(block)], scratch, active, rows, columns, pens, references, statusRow); !decoded)
                return decoded.error();
        }
    }
    const bool hasRichText = isBlockPresent[indexOf(StateBlock::RichText)];
    if (hasRichText && !readRichTextBlock(blocks[indexOf(StateBlock::RichText)], rows, columns, richTexts, terminal.richTextBlocks, scratch.richTextBlocks))
        return ApplyOutcome::Malformed;

    const Digest128 computed = stateDigest(active, rows, columns, hashes[activeIndex], hashes[1 - activeIndex], blockHashes);
    if (isBaseMismatch || computed != carried) {
        if (refusedRowHashes)
            refusedRowHashes->assign(hashes[activeIndex].begin(), hashes[activeIndex].end());
        return ApplyOutcome::DigestMismatch;
    }

    auto nextStorage = std::move(*storage).publish();
    for (const LinkDefinition& link : links)
        installLink(terminal, link);
    if (clusters)
        terminal.clusterPool = std::move(*clusters);
    if (isSwitch) {
        terminal.grid->setAlternateActive(active);
        terminal.screens.activate(active == ScreenMode::Alternate ? Ubiquitty::Modes::ScreenBuffer::Alternate : Ubiquitty::Modes::ScreenBuffer::Primary);
        terminal.grid->requestFullRedraw();
    }
    for (const StateBlock block : decodeOrder) {
        if (isBlockPresent[indexOf(block)])
            commitStateBlock(block, terminal, scratch, active, statusRow);
    }
    if (hasRichText)
        commitStateBlock(StateBlock::RichText, terminal, scratch, active, statusRow);
    for (std::size_t screen = 0; screen < 2; ++screen) {
        if (!scrolls[screen])
            continue;
        // A pin follows the rows the host moved; on the hidden screen its
        // lines are rewritten wholesale, which is all a copy can say.
        if (screen == activeIndex && Ubiquitty::Screen::hasActivePins(terminal))
            Ubiquitty::Screen::notePositionsShiftedInPlace(terminal, scrolls[screen]->top, scrolls[screen]->bottom, scrolls[screen]->delta);
        else if (screen != activeIndex)
            Ubiquitty::Screen::notePositionsScreenReplaced(terminal, screen == 1 ? Ubiquitty::Screen::Buffer::Alternate : Ubiquitty::Screen::Buffer::Primary);
        commitScroll(terminal, static_cast<int>(screen), columns, *scrolls[screen]);
    }
    for (const RowWrite& write : writes) {
        if (Ubiquitty::Screen::hasActivePins(terminal) && write.screen == activeIndex)
            Ubiquitty::Screen::notePositionsRewritten(terminal, write.row, write.row);
        else if (write.screen != activeIndex)
            Ubiquitty::Screen::notePositionsScreenReplaced(terminal, write.screen == 1 ? Ubiquitty::Screen::Buffer::Alternate : Ubiquitty::Screen::Buffer::Primary);
        if (write.screen == activeIndex) {
            terminal.grid->setRowSpan(Ubiquitty::Row(write.row), Ubiquitty::Column(0), Ubiquitty::RowView { write.cells.data(), columns });
            terminal.grid->setLineAttribute(write.row, write.attributes.lineAttribute);
            terminal.grid->setRowWrapped(write.row, write.attributes.isWrapped);
            terminal.grid->setRowMark(write.row, write.attributes.mark);
            Ubiquitty::Screen::latchFlowedRow(terminal, static_cast<int>(write.screen), write.row);
            continue;
        }
        const auto screen = static_cast<int>(write.screen);
        terminal.grid->setRowSpan(screen == 1 ? ScreenMode::Alternate : ScreenMode::Primary, Ubiquitty::Row(write.row), Ubiquitty::Column(0), Ubiquitty::RowView { write.cells.data(), columns });
        Ubiquitty::RowMeta& meta = terminal.grid->rowMetaAt(screen, write.row);
        meta.lineAttribute = write.attributes.lineAttribute;
        meta.set(Ubiquitty::RowFlag::Wrapped, write.attributes.isWrapped);
        meta.oscMark = write.attributes.mark;
        Ubiquitty::Screen::latchFlowedRow(terminal, screen, write.row);
    }
    if (Ubiquitty::Screen::undrawnTextSizeSpecIDs(terminal, terminal.textSizeTable))
        Ubiquitty::Screen::flattenSizedGridCells(terminal);
    replica.storage = std::move(nextStorage);
    replica.blockHashes = blockHashes;
    replica.stateDigest = computed;
    if (echoContext)
        replica.echoContext = echoContext;
    return ApplyOutcome::Applied;
}

void encodePaletteState(const Ubiquitty::Terminal& terminal, std::vector<std::uint8_t>& block)
{
    block.clear();
    WireWriter writer(block);
    writePaletteBlock(writer, terminal);
}

ApplyOutcome applyPaletteState(Ubiquitty::Terminal& terminal, std::span<const std::uint8_t> block, ScreenReplica& replica)
{
    if (!replica.scratch)
        replica.scratch = std::make_shared<Terminal>();
    Terminal& scratch = *replica.scratch;
    const ScreenMode active = activeScreenOf(terminal);
    // The palette block names no pen, link or cluster, so it resolves nothing.
    DecodedStatusRow statusRow;
    const auto decoded = decodeStateBlockWithNames(StateBlock::Palette, block, scratch, active, terminal.rows, terminal.columns, std::span<const Pen> {}, [](std::uint64_t) { return false; }, statusRow);
    if (!decoded)
        return decoded.error();
    commitStateBlock(StateBlock::Palette, terminal, scratch, active, statusRow);
    replica.blockHashes[indexOf(StateBlock::Palette)] = blockHash(block);
    // A replica holding this terminal's picture keeps the digest of it, which
    // a resume presents and the host matches against what it sent.
    const auto rowHashes = replica.rowHashes();
    const auto rowCount = static_cast<std::size_t>(terminal.rows);
    if (terminal.grid && rowHashes[0].size() == rowCount && rowHashes[1].size() == rowCount) {
        const std::size_t activeIndex = active == ScreenMode::Alternate ? 1 : 0;
        replica.stateDigest = stateDigest(active, terminal.rows, terminal.columns, rowHashes[activeIndex], rowHashes[1 - activeIndex], replica.blockHashes);
    }
    return ApplyOutcome::Applied;
}

} // namespace Ubiquitty::Protocol::Sync
