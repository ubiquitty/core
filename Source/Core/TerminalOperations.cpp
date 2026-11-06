// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The implementation of Core/TerminalOperations.h: every declaration in that
// header is defined here, which is why the two sit beside each other rather
// than under the parser's folder.
//
// Cold accessor + policy surface — the read-only queries a frontend polls off
// the parse loop (OSC-8 links, user vars, policy gates, progress and tab
// status, the mouse encoder). None of it runs per byte.

#include "Core/TerminalOperations.h"
#include "Core/CellGrid.h"
#include "Core/Diagnostics/DiagLog.h"
#include "Core/Mouse/Mouse.h"
#include "Core/Mouse/MouseEvent.h"
#include "Core/Rendering/PaletteColors.h"
#include "Core/TerminalShared.h"
#include "Core/Triggers/Engine.h"
#include "Core/Unicode/Encodings.h"
#include "Core/Unicode/UTF8Decoder.h"
#include "Core/VT/Constants.h"
#include "Core/VT/Internal.h"
#include "PAL/PAL.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>

namespace Ubiquitty {

IntSize reportedTextAreaPixels(const Terminal& terminal)
{
    return reportedTextAreaPixels(terminal, { { terminal.cellPixelWidth, terminal.cellPixelHeight }, { terminal.textAreaPixelWidth, terminal.textAreaPixelHeight }, { terminal.drawablePixelWidth, terminal.drawablePixelHeight } });
}

IntSize reportedTextAreaPixels(const Terminal& terminal, const PixelGeometry& geometry)
{
    if (terminal.modes.fullscreenContent && geometry.drawable.width > 0 && geometry.drawable.height > 0)
        return geometry.drawable;
    return geometry.textArea;
}

ReplyDisposition replyDisposition(Ubiquitty::Terminal& t, Ubiquitty::Capability capability)
{
    if (t.replyAuthority == Ubiquitty::ReplyAuthority::Elsewhere)
        return ReplyDisposition::AnsweredElsewhere;
    if (!Ubiquitty::effectAllowed(t, capability))
        return ReplyDisposition::Refuse;
    return ReplyDisposition::Answer;
}

std::optional<Ticks> terminalNow(const Ubiquitty::Terminal& t)
{
    if (Ubiquitty::TerminalClock* clock = t.tickClock.get())
        return clock->now();
    if (Ubiquitty::PAL::MonotonicClockHook clock = Ubiquitty::PAL::monotonicClock())
        return clock();
    return std::nullopt;
}

void ringBell(Ubiquitty::Terminal& t)
{
    t.bellPending = true;
    // Latched beside the pending flag, which the next drain clears: the drain
    // is the frontend playing its cue, not a person having seen the pane.
    if (!t.hasUnseenBell) {
        t.hasUnseenBell = true;
        ++t.attentionGeneration;
    }
    if (Ubiquitty::EffectRoute* route = t.effectRoute.get())
        route->ringBell();
}

void respond(Ubiquitty::Terminal& t, Ubiquitty::Capability capability, const char* s)
{
    respond(t, capability, std::span<const char> { s, s ? strlen(s) : 0 });
}

void respond(Ubiquitty::Terminal& t, Ubiquitty::Capability capability, std::span<const char> bytes)
{
    // The single chokepoint for every host-facing reply (a reply exfiltrates host
    // state): unless this terminal answers and the policy allows `capability` at
    // the current origin, the bytes never queue. Prompt is deny-for-now here (no
    // synchronous consent for emitted output). An unset policy answers PAL's default.
    const int n = static_cast<int>(bytes.size());
    switch (replyDisposition(t, capability)) {
    case ReplyDisposition::Answer:
        break;
    case ReplyDisposition::Refuse:
        return;
    case ReplyDisposition::AnsweredElsewhere:
        Diagnostics::diagLog(t, "reply-answered-elsewhere", static_cast<uint32_t>(n));
        return;
    }
    if (Ubiquitty::TextMeasureClient* measurer = t.textMeasureClient.get(); measurer && measurer->holdReply(t, capability, bytes))
        return;
    // The fixed buffer drains first; overflow and subsequent replies follow it.
    if (t.bulkOutput.length > 0 || bytes.size() > t.outputBuffer.size() - static_cast<std::size_t>(t.outputLength)) {
        if (!respondBulk(t, capability, bytes))
            Diagnostics::diagLog(t, "reply-dropped", static_cast<uint32_t>(bytes.size()));
        return;
    }
    if (!t.transmit8bitC1) {
        std::memcpy(t.outputBuffer.data() + t.outputLength, bytes.data(), static_cast<std::size_t>(n));
        t.outputLength += n;
        return;
    }
    // S8C1T: collapse each 7-bit C1 escape (ESC Fe, Fe in 0x40..0x5F) to its
    // single 8-bit byte 0x80+(Fe-0x40). Generated replies emit ESC only as the
    // lead of such a control, so the scan is unambiguous.
    for (int i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]);
        if (c == 0x1B && i + 1 < n) {
            unsigned char f = static_cast<unsigned char>(bytes[static_cast<std::size_t>(i + 1)]);
            if (f >= 0x40 && f <= 0x5F) {
                t.outputBuffer[t.outputLength++] = static_cast<char>(0x80 + (f - 0x40));
                ++i;
                continue;
            }
        }
        t.outputBuffer[t.outputLength++] = static_cast<char>(c);
    }
}

int screenIndex(const Ubiquitty::Terminal& t) { return t.screens.isUsingAlternateScreen ? 1 : 0; }

bool isReverseVideo(const Ubiquitty::Terminal* t) { return t ? t->modes.reverseVideo : false; }

ColorRGBA chromeForegroundColor(const Ubiquitty::Terminal* t)
{
    if (!t)
        return { defaultForegroundRGB[0], defaultForegroundRGB[1], defaultForegroundRGB[2], 0xFF };
    return Rendering::narrowPaletteColor(t->palette.defaultForeground);
}

ColorRGBA chromeBackgroundColor(const Ubiquitty::Terminal* t)
{
    if (!t)
        return { defaultBackgroundRGB[0], defaultBackgroundRGB[1], defaultBackgroundRGB[2], 0xFF };
    return Rendering::narrowPaletteColor(t->palette.defaultBackground);
}

// The palette slot the accent is read from, named because 4 as a bare index
// says nothing about why blue is the accent.
constexpr int ansiBlueIndex = 4;

ColorRGBA chromeAccentColor(const Ubiquitty::Terminal* t)
{
    if (!t) {
        // The compiled-in palette's own blue, through the same narrowing the
        // live read takes, so a summon before the session exists wears the
        // colour the first frame will.
        TermPalette defaults;
        defaults.seedDefaults();
        return Rendering::narrowPaletteColor(defaults.indexed[ansiBlueIndex]);
    }
    return Rendering::narrowPaletteColor(t->palette.indexed[ansiBlueIndex]);
}

ColorRGBA chromeCursorColor(const Ubiquitty::Terminal* t)
{
    if (!t)
        return { defaultCursorRGB[0], defaultCursorRGB[1], defaultCursorRGB[2], 0xFF };
    return Rendering::narrowPaletteColor(t->palette.cursor);
}

Rendering::PaletteColors terminalColors(const Ubiquitty::Terminal& t)
{
    Rendering::PaletteColors colors = Rendering::paletteColors(t.palette);
    if (t.selectionBackgroundSet)
        colors.selectionBackground = Rendering::narrowPaletteColor({ t.selectionBackgroundRed, t.selectionBackgroundGreen, t.selectionBackgroundBlue });
    if (t.selectionForegroundSet)
        colors.selectionForeground = Rendering::narrowPaletteColor({ t.selectionForegroundRed, t.selectionForegroundGreen, t.selectionForegroundBlue });
    return colors;
}

// ---- OSC 8 hyperlink table ------------------------------------------
// A fixed-capacity ring of (id → URI) plus an optional dedup key.
// linkIntern is the single mutator (called from the OSC 8 dispatcher);
// linkURI is the read-only resolver the frontend uses to turn a cell's
// stamped id back into a URI.

std::optional<std::string_view> userVariable(const Ubiquitty::Terminal* t, std::string_view name)
{
    if (!t || name.empty())
        return std::nullopt;
    for (const auto& v : t->userVariables) {
        if (v.used && v.name == name)
            return std::string_view(v.value);
    }
    return std::nullopt;
}

bool setSessionLogPath(Ubiquitty::Terminal* t, const char* path)
{
    if (!t)
        return false;
    if (t->sessionLogFile) {
        std::fclose(static_cast<FILE*>(t->sessionLogFile));
        t->sessionLogFile = nullptr;
    }
    if (!path) {
        // Removing the path stops both frontend capture and the DEC mode's
        // capture request; a later ?46 h with no file installed is a silent no-op.
        t->sessionLogCaptureEnabled = false;
        t->loggingEnabled = false;
        return true;
    }
    FILE* f = std::fopen(path, "ab");
    if (!f)
        return false;
    t->sessionLogFile = f;
    // Installing a path is the gated permission point: enable frontend-owned
    // capture so the File-menu logging contract survives host resets. Mode ?46
    // remains a separate host-visible bit.
    t->sessionLogCaptureEnabled = true;
    return true;
}

bool setPrintSinkPath(Ubiquitty::Terminal* t, const char* path)
{
    if (!t)
        return false;
    if (t->printSinkFile) {
        std::fclose(static_cast<FILE*>(t->printSinkFile));
        t->printSinkFile = nullptr;
    }
    if (!path)
        return true;
    FILE* f = std::fopen(path, "ab");
    if (!f)
        return false;
    t->printSinkFile = f;
    return true;
}

void setPasswordDetect(Ubiquitty::Terminal* t, bool enable)
{
    if (!t)
        return;
    t->passwordDetect = enable;
    // Only the host clears a detected prompt. A replica keeps its host's
    // verdict, and an explicit mode-7401 assertion belongs to the application.
    if (!enable && t->replyAuthority == ReplyAuthority::ThisTerminal && t->passwordPromptActive && !t->isPasswordPromptAsserted)
        VT::setPasswordPromptActive(*t, Ubiquitty::Modes::PasswordPrompt::Inactive);
}

bool passwordPromptActive(const Ubiquitty::Terminal* t) { return t && t->passwordPromptActive; }

uint32_t progressGeneration(const Ubiquitty::Terminal* t) { return t ? t->progressGeneration : 0; }

void getProgress(const Ubiquitty::Terminal* t, uint8_t* state, uint8_t* value)
{
    if (state)
        *state = t ? t->progressState : 0;
    if (value)
        *value = t ? t->progressValue : 0;
}

uint32_t tabStatusGeneration(const Ubiquitty::Terminal* t) { return t ? t->tabStatusGeneration : 0; }

std::optional<ColorRGBA> tabStatusColor(const Ubiquitty::Terminal* t)
{
    if (!t || t->tabStatusSet == 0)
        return std::nullopt;
    return ColorRGBA { t->tabStatusR, t->tabStatusG, t->tabStatusB };
}

UnseenAttention unseenAttention(const Ubiquitty::Terminal* t)
{
    if (!t)
        return {};
    return { t->hasUnseenBell, t->hasUnseenNotification, t->unseenNotificationTitle, t->unseenNotificationBody };
}

bool markAttentionSeen(Ubiquitty::Terminal& t)
{
    if (!t.hasUnseenBell && !t.hasUnseenNotification)
        return false;
    t.hasUnseenBell = false;
    t.hasUnseenNotification = false;
    t.unseenNotificationTitle.clear();
    t.unseenNotificationBody.clear();
    ++t.attentionGeneration;
    return true;
}

std::optional<ColorRGBA> tabColor(const Ubiquitty::Terminal* t)
{
    if (!t || t->tabColorSet == 0)
        return std::nullopt;
    return ColorRGBA { t->tabColorRed, t->tabColorGreen, t->tabColorBlue };
}

std::string_view badgeText(const Ubiquitty::Terminal* t) { return t ? std::string_view(t->badgeText) : std::string_view(); }

std::string_view tabStatusText(const Ubiquitty::Terminal* t)
{
    if (!t || t->tabStatusSet == 0)
        return {};
    return { t->tabStatusText.data(), t->tabStatusTextLength };
}

std::string_view oscReplyTerminator(const Ubiquitty::Terminal& t)
{
    return t.parser.isOSCRequestBellTerminated ? std::string_view { "\a" } : std::string_view { "\033\\" };
}

bool respondBulk(Ubiquitty::Terminal& t, Ubiquitty::Capability capability, std::span<const char> bytes)
{
    // The same chokepoint rule as respond(): a suppressed reply never queues,
    // and the suppression is an answer — the policy's, or the terminal's that
    // answers for this one — so there is nothing for the composer to take back
    // and it reports as handled.
    if (replyDisposition(t, capability) != ReplyDisposition::Answer)
        return true;
    if (Ubiquitty::TextMeasureClient* measurer = t.textMeasureClient.get(); measurer && measurer->holdReply(t, capability, bytes))
        return true;
    if (!t.transmit8bitC1)
        return t.bulkOutput.append({ bytes.data(), bytes.size() }, Ubiquitty::VT::maxBulkOutputBytes);
    // S8C1T: the collapse respond() applies, so a host that asked for 8-bit
    // controls reads one framing whichever channel carried the reply. Staged
    // through a stack block so the buffer grows by appends the size of the
    // reply, not by one per byte.
    std::array<char, 4096> block;
    std::size_t filled = 0;
    const int previousLength = t.bulkOutput.length;
    const auto append = [&](std::span<const char> chunk) {
        if (t.bulkOutput.append({ chunk.data(), chunk.size() }, Ubiquitty::VT::maxBulkOutputBytes))
            return true;
        // A report can contain several OSC packets. Capacity or allocation
        // refusal must not leave a prefix that looks like an incomplete reply.
        t.bulkOutput.length = previousLength;
        if (t.bulkOutput.data)
            t.bulkOutput.data[previousLength] = 0;
        return false;
    };
    const int n = static_cast<int>(bytes.size());
    for (int i = 0; i < n; ++i) {
        char c = bytes[static_cast<std::size_t>(i)];
        if (static_cast<unsigned char>(c) == 0x1B && i + 1 < n) {
            const unsigned char f = static_cast<unsigned char>(bytes[static_cast<std::size_t>(i + 1)]);
            if (f >= 0x40 && f <= 0x5F) {
                c = static_cast<char>(0x80 + (f - 0x40));
                ++i;
            }
        }
        block[filled++] = c;
        if (filled == block.size()) {
            if (!append(std::span(block).first(filled)))
                return false;
            filled = 0;
        }
    }
    if (filled > 0 && !append(std::span(block).first(filled)))
        return false;
    return true;
}

void flushOutput(Terminal& terminal, bool (*accept)(void*, std::span<const char>), void* context)
{
    if (terminal.outputLength > 0) {
        if (!accept(context, { terminal.outputBuffer.data(), static_cast<std::size_t>(terminal.outputLength) }))
            return;
        terminal.outputLength = 0;
    }
    if (terminal.bulkOutput.length > 0 && accept(context, { terminal.bulkOutput.data, static_cast<std::size_t>(terminal.bulkOutput.length) }))
        terminal.bulkOutput.reset();
}

int drainOutput(Ubiquitty::Terminal* t, std::span<char> output)
{
    if (!t || output.empty())
        return 0;
    int n = t->outputLength;
    if (std::cmp_greater(n, output.size()))
        n = static_cast<int>(output.size());
    if (n > 0) {
        std::memcpy(output.data(), t->outputBuffer.data(), n);
        std::memmove(t->outputBuffer.data(), t->outputBuffer.data() + n, t->outputLength - n);
        t->outputLength -= n;
    }
    // The bulk channel drains behind the fixed one, into whatever the span
    // still holds. Two channels are not one queue: a small reply emitted
    // after a bulk one leaves first, which the escape stream tolerates —
    // every reply is self-delimiting, and the requesters of each are
    // different programs waiting on different framings. Freed at empty
    // because a bulk reply is rare and its capacity is not small.
    if (t->bulkOutput.length > 0 && std::cmp_less(n, output.size())) {
        int m = t->bulkOutput.length;
        const int room = static_cast<int>(output.size()) - n;
        if (m > room)
            m = room;
        std::memcpy(output.data() + n, t->bulkOutput.data, static_cast<std::size_t>(m));
        if (m < t->bulkOutput.length)
            std::memmove(t->bulkOutput.data, t->bulkOutput.data + m, static_cast<std::size_t>(t->bulkOutput.length - m));
        t->bulkOutput.length -= m;
        if (t->bulkOutput.length == 0)
            t->bulkOutput.reset();
        n += m;
    }
    return n;
}

int encodeMouseReport(const Ubiquitty::Terminal& t, Ubiquitty::Mouse::EventType type, int button, int row, int column, int modifiers, std::span<char> output, int pixelX, int pixelY) { return Ubiquitty::Mouse::encodeReport(&t, type, button, row, column, modifiers, output, pixelX, pixelY); }

// ---- DEC VT300 locator --------------------------------

// ---- Triggers engine ----------------------------------

} // namespace Ubiquitty
