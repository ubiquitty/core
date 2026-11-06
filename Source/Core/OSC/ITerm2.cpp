// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OSC 1337 slice — the iTerm2 escape-code protocol
// (https://iterm2.com/documentation-escape-codes.html). Handled subset: SetMark / CurrentDir / RemoteHost /
// SetUserVar / File= (inline image) / RequestAttention / CursorShape / StealFocus / SetColors= / Copy=.
// The user-var store lives here; File= decoding is a separate backend. Copy= reuses the OSC 52 codec + gate.

#include "Core/Codec/Base64.h"
#include "Core/OSC/Internal.h"
#include "Core/ReplyBuilder.h"

#include "Core/Image/Inline/Images.h"
#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include <cstring>
#include <string_view>

namespace Ubiquitty::OSC {

// OSC 1337 ; SetUserVar=NAME=BASE64VALUE — iTerm2 per-session user variable. Stored in the fixed-capacity
// table: overwrite on a repeat NAME, clear on an empty value, drop on malformed base64 or a full table.
static void setUserVariable(Ubiquitty::Terminal& t, std::string_view argument)
{
    const std::size_t equals = argument.find('=');
    if (equals == std::string_view::npos || equals == 0)
        return; // no NAME=, or empty NAME
    std::string name = Ubiquitty::boundedUTF8String(argument.substr(0, equals), Ubiquitty::Terminal::maxUserVarNameLength);

    const std::string_view b64 = argument.substr(equals + 1);
    const int b64Length = static_cast<int>(b64.size());

    // Empty value clears any existing slot for NAME.
    if (b64Length == 0) {
        for (auto& v : t.userVariables) {
            if (v.used && v.name == name) {
                v.used = false;
                v.name.clear();
                v.value.clear();
            }
        }
        return;
    }

    // Decoded length is <= b64Length*3/4.
    std::string value(static_cast<std::size_t>(b64Length), '\0');
    const auto decoded = Codec::base64Decode(b64, value);
    if (!decoded)
        return; // malformed base64 — drop
    value = Ubiquitty::boundedUTF8String({ decoded->data(), decoded->size() }, Ubiquitty::Terminal::maxUserVarValueLength);

    // Overwrite an existing slot for NAME, else claim a free one.
    Ubiquitty::Terminal::UserVariable* slot = nullptr;
    for (auto& v : t.userVariables) {
        if (v.used && v.name == name) {
            slot = &v;
            break;
        }
    }
    if (!slot) {
        for (auto& v : t.userVariables) {
            if (!v.used) {
                slot = &v;
                break;
            }
        }
    }
    if (!slot)
        return; // table full — drop

    slot->name = std::move(name);
    slot->value = std::move(value);
    slot->used = true;
}

static void reportCellSize(Ubiquitty::Terminal& terminal)
{
    if (!terminal.allowWindowOperations)
        return;
    const auto* facts = terminal.typography.facts.get();
    ReplyBuilder reply;
    reply.put("\033]1337;ReportCellSize=");
    const auto appendPoints = [&](unsigned value) {
        reply.putDecimal(static_cast<int>(value / 64));
        unsigned remainder = value % 64;
        if (remainder) {
            reply.put('.');
            do {
                remainder *= 10;
                reply.put(static_cast<char>('0' + remainder / 64));
                remainder %= 64;
            } while (remainder);
        }
    };
    // Without published typography, use the same nominal cell as XTWINOPS.
    appendPoints(facts && facts->cellHeight ? facts->cellHeight : 16 * 64);
    reply.put(';');
    appendPoints(facts && facts->cellWidth ? facts->cellWidth : 8 * 64);
    reply.put(oscReplyTerminator(terminal));
    respond(terminal, Capability::WindowGeometryRead, reply.span());
}

static void reportVariable(Ubiquitty::Terminal& terminal, std::string_view encodedName)
{
    std::array<char, Terminal::maxUserVarNameLength + 5> nameBuffer;
    const auto decodedName = Codec::base64Decode(encodedName, nameBuffer);
    if (!decodedName || decodedName->empty())
        return;
    std::string_view name(decodedName->data(), decodedName->size());
    if (name.starts_with("session."))
        name.remove_prefix(8);
    ReplyBuilder number;
    std::string_view value;
    Capability capability = Capability::StatusReport;
    if (name == "columns" || name == "rows") {
        if (!terminal.allowWindowOperations)
            return;
        capability = Capability::TerminalSizeRead;
        number.putDecimal(name == "columns" ? terminal.columns : terminal.rows);
        value = { number.span().data(), number.span().size() };
    } else if (name == "name" || name == "terminalWindowName" || name == "terminalIconName") {
        if (!terminal.allowWindowOperations)
            return;
        capability = Capability::WindowTitleRead;
        value = name == "terminalIconName" ? terminal.iconNameUTF8 : terminal.titleUTF8;
        // iTerm2's session name is never empty: it falls back to a default
        // where its terminalWindowName stays empty. A pane no program has
        // titled is shown under the application's name (Title/Format.h), so
        // that is this session's name until one does.
        if (name == "name" && value.empty())
            value = "Ubiquitty";
    } else if (name == "path")
        value = terminal.currentDirectory;
    else if (name.starts_with("user.")) {
        name.remove_prefix(5);
        for (const auto& variable : terminal.userVariables) {
            if (variable.used && variable.name == name) {
                value = variable.value;
                break;
            }
        }
    }
    static constexpr std::string_view prefix = "\033]1337;ReportVariable=";
    static constexpr auto maxValueLength = std::max({ Terminal::maxTitleLength, Terminal::maxCurrentDirectoryLength, Terminal::maxUserVarValueLength });
    std::array<char, prefix.size() + Codec::base64EncodedLength(maxValueLength) + 2> reply;
    std::memcpy(reply.data(), prefix.data(), prefix.size());
    const auto encoded = Codec::base64Encode({ value.data(), value.size() }, std::span(reply).subspan(prefix.size(), reply.size() - prefix.size() - 2));
    if (!encoded)
        return;
    std::size_t length = prefix.size() + encoded->size();
    for (char byte : oscReplyTerminator(terminal))
        reply[length++] = byte;
    respond(terminal, capability, std::span(reply).first(length));
}

// OSC 1337 — iTerm2 proprietary. Handled subset:
// 1337 ; SetMark              → prompt mark on the cursor row
// 1337 ; CurrentDir=<path>    → currentDirectory (reuses the OSC 7 currentDirectory hook)
// 1337 ; RemoteHost=<id>      → stored on Terminal::remoteHost
// 1337 ; SetUserVar=NAME=<b64> → per-session user variable on Terminal::userVariables
// 1337 ; SetBadgeFormat=<b64> → Terminal::badgeText (bounded; interpolations not evaluated)
// 1337 ; RequestAttention=… → WindowClient::requestAttention (dock bounce)
// 1337 ; CursorShape=N     → DECSCUSR cursor style
// 1337 ; StealFocus        → WindowClient::stealFocus (bring app forward)
// 1337 ; SetColors=preset=<name> → WindowClient::selectColorPreset (live theme switch)
// 1337 ; Copy=:<b64>       → ClipboardClient::writeClipboard (iTerm2's clipboard write)
// iTerm2 has no OSC 1337 notification sub-command — notifications come via OSC 9.
// Everything else (inline images, etc.) is ignored gracefully.
void handleOSC1337(Ubiquitty::Terminal& t, std::string_view p)
{
    static constexpr char setMark[] = "SetMark";
    static constexpr char currentDirectory[] = "CurrentDir=";
    static constexpr char remoteHost[] = "RemoteHost=";
    static constexpr char file[] = "File=";
    static constexpr char setUserVariablePrefix[] = "SetUserVar=";
    static constexpr std::string_view setBadgeFormat = "SetBadgeFormat=";

    if (p == "ReportCellSize") {
        reportCellSize(t);
        return;
    }
    static constexpr std::string_view reportVariablePrefix = "ReportVariable=";
    if (p.starts_with(reportVariablePrefix)) {
        reportVariable(t, p.substr(reportVariablePrefix.size()));
        return;
    }

    if (p == setMark) {
        setPromptMark(t, Screen::osc133MarkPromptStart);
        return;
    }

    if (p.starts_with(currentDirectory)) {
        const std::string_view path = p.substr(std::string_view(currentDirectory).size());
        // SetWorkingDirectory: OSC 1337 CurrentDir= sets the session CWD — a
        // metadata write the host daemon reads (CommandDone / SessionList) plus
        // the shellClient hook. Origin-aware; a denied origin skips both writes
        // below. An unset policy answers PAL's default.
        if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetWorkingDirectory))
            return; // host policy denied the working-directory write — skip it
        setCurrentDirectory(t, path);
        // The stored value, not the raw payload: the store filtered it.
        if (auto* shell = t.shellClient.get())
            shell->setCurrentDirectory(t.currentDirectory);
        return;
    }

    if (p.starts_with(remoteHost)) {
        // SetRemoteHost: OSC 1337 RemoteHost= rewrites the session's reported
        // host identity — a metadata write the host daemon reads. Origin-aware;
        // a denied origin skips it. An unset policy answers PAL's default.
        if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetRemoteHost))
            return; // host policy denied the remote-host write — skip it
        Ubiquitty::setRemoteHostString(t, p.substr(std::string_view(remoteHost).size()));
        return;
    }

    if (p.starts_with(setBadgeFormat)) {
        // A label drawn over the pane, like the OSC 21337 status beside it:
        // nothing outside the window reads it, so no capability gates it.
        handleBadgeFormat(t, p.substr(setBadgeFormat.size()));
        return;
    }

    if (p.starts_with(setUserVariablePrefix)) {
        // SetUserVariable: the value is read back by the host daemon and by
        // shell integration, so a stream that writes one is writing state a
        // consumer outside the grid acts on. Its tier says host-escaping; an
        // unset policy answers PAL's default, as everywhere else here.
        if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetUserVariable))
            return; // host policy denied the user-variable write
        setUserVariable(t, p.substr(std::string_view(setUserVariablePrefix).size()));
        return;
    }

    if (p.starts_with(file)) {
        Ubiquitty::Image::Inline::handleInlineImage(t, p.substr(std::string_view(file).size()).data());
        return;
    }

    static constexpr char requestAttention[] = "RequestAttention=";
    if (p.starts_with(requestAttention)) {
        const std::string_view value = p.substr(std::string_view(requestAttention).size());
        Ubiquitty::AttentionRequest kind = Ubiquitty::AttentionRequest::Continuous;
        if (value == "no")
            kind = Ubiquitty::AttentionRequest::Cancel;
        else if (value == "once")
            kind = Ubiquitty::AttentionRequest::Once;
        // "yes" / "fireworks" / "flash" → sustained attention (the default).
        // Bouncing the dock is the window escaping the grid, and the tier says
        // so.
        if (auto* w = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::RequestAttention))
            w->requestAttention(kind);
        return;
    }

    static constexpr char cursorShape[] = "CursorShape=";
    if (p.starts_with(cursorShape)) {
        // SetCursorShape: same LowRiskInfo gate as DECSCUSR (csiH_decscusr). A
        // host that treats a remote cursor reshape as display-spoofing restricts
        // the tier; a refused origin skips it. An unset policy answers PAL's default.
        if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetCursorShape))
            return; // host policy denied the cursor-shape write — skip it
        // iTerm2 CursorShape: 0 = block, 1 = vertical bar, 2 = underline. Map to
        // the DECSCUSR style, preserving the current blink (odd = blinking, 0 = default).
        const std::string_view value = p.substr(std::string_view(cursorShape).size());
        using Ubiquitty::Cursor::Style;
        // FollowUserPreference has no intrinsic blink; treat it as blinking so a
        // fresh terminal maps the CursorShape codes to the blinking variants.
        const bool isBlinkingStyle = (t.cursorStyle == Style::FollowUserPreference) || Ubiquitty::Cursor::isBlinking(t.cursorStyle);
        switch (value.empty() ? '\0' : value[0]) {
        case '0':
            t.cursorStyle = isBlinkingStyle ? Style::BlinkingBlock : Style::SteadyBlock;
            break;
        case '1':
            t.cursorStyle = isBlinkingStyle ? Style::BlinkingBar : Style::SteadyBar;
            break;
        case '2':
            t.cursorStyle = isBlinkingStyle ? Style::BlinkingUnderline : Style::SteadyUnderline;
            break;
        default:
            break; // unknown shape — ignore
        }
        return;
    }

    static constexpr char stealFocus[] = "StealFocus";
    if (p == stealFocus) {
        // Bringing the application forward over whatever the reader was doing
        // is the most host-escaping thing in this file, and it was the only
        // one with no gate.
        if (auto* w = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::StealFocus))
            w->stealFocus();
        return;
    }

    // OSC 1337 ; SetColors=preset=<name> — iTerm2's runtime theme switch. We
    // support the `preset=` key (select a named built-in / registered theme);
    // the per-color keys (fg=, bg=, …) are not accepted: OSC 4/10/11 already
    // set exactly those, and a second spelling is a second thing to keep correct. Gated on allowColorOperations like the rest of the
    // color cluster; the frontend resolves the name and applies it live.
    static constexpr char setColorsPreset[] = "SetColors=preset=";
    if (p.starts_with(setColorsPreset)) {
        if (!t.allowColorOperations)
            return;
        // SelectColorPreset: LowRiskInfo. A host that treats a remote theme switch
        // as display-spoofing restricts the tier (only named, host-registered
        // presets — weaker than arbitrary OSC 4/10/11, which are gated
        // separately). A refused origin skips it; an unset policy answers PAL's
        // default.
        const std::string_view name = p.substr(std::string_view(setColorsPreset).size());
        if (auto* w = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::SelectColorPreset))
            w->selectColorPreset(name);
        return;
    }

    // OSC 1337 ; Copy=:<base64> — iTerm2's clipboard write (distinct from
    // OSC 52). The colon immediately follows "Copy="; there is no clipboard
    // selector. Decode + apply through the same clipboardClient sink and the
    // same write policy as OSC 52, so a remote can't use this as an
    // unguarded second clipboard-write channel.
    static constexpr char copy[] = "Copy=:";
    if (p.starts_with(copy)) {
        // Copy= is the same programmatic-clipboard-write capability as OSC 52,
        // so it honors the same master kill-switch + write policy — no bypass.
        if (!osc52WriteAllowed(t))
            return;
        // ClipboardWrite: origin-aware capability gate, ADDITIVE to the
        // osc52WriteAllowed() layer above (both must allow). This second
        // clipboard-write channel must not let a Remote/Agent peer stage
        // paste-injection. Same pattern as the OSC 52 SET fold. Unset ⇒ PAL's default.
        if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::ClipboardWrite))
            return; // host policy denied the clipboard write — skip it
        auto* clip = t.clipboardClient.get();
        if (!clip)
            return;
        const std::string_view b64 = p.substr(std::string_view(copy).size());
        const int b64Length = static_cast<int>(b64.size());
        // Empty payload clears the clipboard, matching OSC 52's empty SET.
        char stackDecoded[256];
        if (b64Length <= static_cast<int>(sizeof(stackDecoded)) * 4 / 3) {
            const auto decoded = Codec::base64Decode(b64, stackDecoded);
            if (!decoded)
                return; // malformed base64 — silently drop, like OSC 52
            // Copy= has no clipboard selector — always the system clipboard.
            clip->writeClipboard(Ubiquitty::ClipboardBuffer::Clipboard, *decoded);
            return;
        }
        if (b64Length > Ubiquitty::VT::maxOSC52ChunkSize)
            return; // implausibly large — drop (same ceiling as OSC 52)
        // Past the stack buffer's reach, so there is room for at least one byte.
        const std::size_t decodedCapacity = Codec::base64DecodedCapacity(b64);
        const std::unique_ptr<char[]> heapDecoded(new (std::nothrow) char[decodedCapacity]);
        if (!heapDecoded)
            return;
        if (const auto decoded = Codec::base64Decode(b64, { heapDecoded.get(), decodedCapacity }))
            clip->writeClipboard(Ubiquitty::ClipboardBuffer::Clipboard, *decoded);
        return;
    }
    // Unhandled key — consumed, no effect.
}

} // namespace Ubiquitty::OSC
