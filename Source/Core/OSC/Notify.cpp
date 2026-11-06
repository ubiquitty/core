// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OSC notification slice: desktop notifications across three dialects — OSC 9 (iTerm growl + ConEmu
// progress and prompt overloads), 99 (kitty), 777 (urxvt) — plus OSC 21337, iTerm2's session status
// (Docs/tab-status.md).

#include "Core/Codec/Base64.h"
#include "Core/OSC/Internal.h"
#include "Core/OSC/KittyNotification.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/OSC/TabStatus.h"
#include "Core/ReplyBuilder.h"
#include "Core/TerminalOperations.h"
#include "Core/Title/Truncate.h"
#include "PAL/Capability.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::OSC {

static void publishProgress(Ubiquitty::Terminal& terminal, uint8_t state, int value)
{
    if (auto* client = Ubiquitty::clientForEffectThatEscapesNothing(terminal.windowClient))
        client->reportProgress(state, value);
    const auto storedValue = static_cast<uint8_t>(std::max(0, value));
    if (terminal.progressState == state && terminal.progressValue == storedValue)
        return;
    terminal.progressState = state;
    terminal.progressValue = storedValue;
    ++terminal.progressGeneration;
    if (auto* client = Ubiquitty::clientForEffectThatEscapesNothing(terminal.windowClient))
        client->invalidate();
}

static int progressPropertyValue(std::string_view value, int minimum, int maximum)
{
    if (value.empty() || (minimum == 0 && value.front() == '-'))
        return -1;
    int parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc {} || result.ptr != value.data() + value.size() || parsed < minimum || parsed > maximum)
        return -1;
    return parsed;
}

void handleOSC666(Ubiquitty::Terminal& terminal, std::string_view payload)
{
    if (terminal.parser.isOSCRequestBellTerminated)
        return;
    // The selector and separating semicolon count toward VTE's 4096-codepoint limit.
    std::size_t characters = 4;
    for (const unsigned char byte : payload) {
        if ((byte & 0xC0) != 0x80 && ++characters > 4096)
            return;
    }
    constexpr std::string_view hintName = "vte.progress.hint";
    constexpr std::string_view valueName = "vte.progress.value";
    const int previousHint = terminal.termpropProgressHint;
    const int previousValue = terminal.termpropProgressValue;
    bool hasQuery = false;
    while (!payload.empty()) {
        const auto separator = payload.find(';');
        const auto statement = payload.substr(0, separator);
        payload = separator == std::string_view::npos ? std::string_view {} : payload.substr(separator + 1);
        const auto operation = statement.find_first_of("=?!");
        const auto name = statement.substr(0, operation);
        if (operation == std::string_view::npos) {
            if (name == hintName || (name.ends_with('.') && hintName.starts_with(name)))
                terminal.termpropProgressHint = -1;
            if (name == valueName || (name.ends_with('.') && valueName.starts_with(name)))
                terminal.termpropProgressValue = -1;
        } else if (statement[operation] == '=') {
            const auto value = statement.substr(operation + 1);
            if (name == hintName)
                terminal.termpropProgressHint = progressPropertyValue(value, 1, 4);
            else if (name == valueName)
                terminal.termpropProgressValue = progressPropertyValue(value, 0, 100);
        } else if (statement[operation] == '?' && operation + 1 == statement.size())
            hasQuery = true;
    }
    if (terminal.termpropProgressHint != previousHint || terminal.termpropProgressValue != previousValue) {
        const auto state = terminal.termpropProgressValue < 0 ? ProgressNone
            : terminal.termpropProgressHint < 0               ? ProgressNormal
                                                              : static_cast<ProgressState>(terminal.termpropProgressHint);
        publishProgress(terminal, state, terminal.termpropProgressValue);
    }
    if (hasQuery)
        respond(terminal, Capability::StatusReport, "\033]666\033\\");
}

// Latches a notification and hands it to the shell client. Drops silently
// with no client, and when the host's capability policy denies it.
static void dispatchNotification(Ubiquitty::Terminal& t, NotificationRequest request)
{
    // DesktopNotification (OSC 9/99/777): act-beyond-grid, so an installed policy must allow it
    // (origin-aware); deny drops it. Unset ⇒ PAL's default.
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::DesktopNotification))
        return; // host capability policy denied the notification

    // Bounded so a hostile stream cannot hand the desktop an unbounded string.
    request.title = request.title.substr(0, std::min(request.title.size(), attentionTextCapacity));
    request.body = request.body.substr(0, std::min(request.body.size(), attentionTextCapacity));

    // Latched whether or not anyone is there to be told: delivery reaches
    // whoever is attached now, and the latch is for whoever looks later.
    t.hasUnseenNotification = true;
    t.unseenNotificationTitle.assign(request.title);
    t.unseenNotificationBody.assign(request.body);
    ++t.attentionGeneration;

    if (auto* shell = t.shellClient.get())
        shell->postNotificationRequest(request);
}

// OSC 9 — overloaded across emulators.
// ConEmu progress:  9;4;<state>;<value>
// state 0 = none, 1 = normal (value 0..100), 2 = error,
// 3 = indeterminate, 4 = paused. Parsed into Ubiquitty::Terminal and bumps
// progressGeneration so the PAL repaints the progress indicator.
// ConEmu prompt: 9;12 — mark the cursor's row as a prompt location.
// iTerm notification: 9;<text>  (other payloads)
// Routed to the notification hook as a body-only message.
// The 9;4; form is the ConEmu progress shape; the prompt overload is handled
// before the notification fallback.
void handleOSC9(Ubiquitty::Terminal& t, std::string_view p)
{
    if (p == "9")
        return;
    if (p.starts_with("9;")) {
        if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetWorkingDirectory))
            return;
        std::string_view path = p.substr(2);
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        setCurrentDirectory(t, path);
        if (auto* shell = t.shellClient.get())
            shell->setCurrentDirectory(t.currentDirectory);
        return;
    }
    // ConEmu's prompt marker is a location, not notification text and not a
    // declaration that every subsequent character belongs to the prompt.
    if (p == "12" || p.starts_with("12;")) {
        setPromptMark(t, Screen::osc133MarkPromptStart);
        recordPendingPromptMark(t);
        return;
    }
    // ConEmu progress arrives strictly as "4;<state>...". Detect that
    // exact prefix; any other payload is iTerm notification text. The prefix
    // decides the sub-parameter too — "4" is the only digit run it admits
    // before the ';', so there is nothing left for a number parser to learn.
    static constexpr std::string_view progressPrefix = "4;";
    if (!p.starts_with(progressPrefix)) {
        if (!p.empty())
            dispatchNotification(t, { .title = {}, .body = p, .identifier = {}, .occasion = NotificationOccasion::Always });
        return;
    }
    p.remove_prefix(progressPrefix.size());

    int state = 0;
    int value = 0;
    bool haveValue = false;
    bool haveState = false;
    while (!p.empty() && p.front() >= '0' && p.front() <= '9') {
        state = Ubiquitty::decimalAppendSaturated(state, p.front() - '0', 255);
        p.remove_prefix(1);
        haveState = true;
    }
    if (!haveState)
        return;
    if (p.starts_with(';')) {
        p.remove_prefix(1);
        while (!p.empty() && p.front() >= '0' && p.front() <= '9') {
            value = Ubiquitty::decimalAppendSaturated(value, p.front() - '0', 255);
            p.remove_prefix(1);
            haveValue = true;
        }
    }
    if (state < 0 || state > Ubiquitty::OSC::ProgressPaused)
        state = Ubiquitty::OSC::ProgressNone;
    if (value < 0)
        value = 0;
    if (value > 100)
        value = 100;
    t.termpropProgressHint = state == ProgressNone ? -1 : state;
    t.termpropProgressValue = state == ProgressNone ? -1 : value;
    publishProgress(t, static_cast<uint8_t>(state), haveValue ? value : -1);
}

namespace {

// What an OSC 99 chunk's `p=` says its payload is.
enum class KittyPayload : std::uint8_t {
    Title,
    Body,
    // Withdraw the notification named: nothing is shown, and what was open
    // under the name is abandoned.
    Close,
    Query,
    // An icon, buttons, a query, a liveness probe, or a kind this does not
    // know: consumed, and the notification it names is left as it was.
    SetAside,
};

// One chunk's metadata, with the defaults the protocol gives a key left out.
struct KittyChunk {
    std::string identifier;
    KittyPayload payload = KittyPayload::Title;
    std::optional<NotificationOccasion> occasion;
    bool isDone = true;
    bool isBase64 = false;
};

// The protocol's alphabet for a name. Anything else is dropped rather than
// refused: the name is only ever compared with another name read the same
// way, and it becomes part of an identifier handed to the system.
constexpr bool isKittyIdentifierCharacter(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
        || character == '_' || character == '-' || character == '+' || character == '.';
}

KittyChunk parseKittyMetadata(std::string_view metadata)
{
    KittyChunk chunk;
    while (!metadata.empty()) {
        const std::size_t colon = metadata.find(':');
        const std::string_view pair = metadata.substr(0, colon);
        metadata = colon == std::string_view::npos ? std::string_view() : metadata.substr(colon + 1);
        // Every key the protocol defines is one letter followed by `=`.
        if (pair.size() < 2 || pair[1] != '=')
            continue;
        const std::string_view value = pair.substr(2);
        switch (pair[0]) {
        case 'i':
            chunk.identifier.clear();
            for (const char character : value) {
                if (chunk.identifier.size() == kittyNotificationIdentifierCapacity)
                    break;
                if (isKittyIdentifierCharacter(character))
                    chunk.identifier += character;
            }
            break;
        case 'd':
            chunk.isDone = value != "0";
            break;
        case 'e':
            chunk.isBase64 = value == "1";
            break;
        case 'p':
            if (value == "title")
                chunk.payload = KittyPayload::Title;
            else if (value == "body")
                chunk.payload = KittyPayload::Body;
            else if (value == "close")
                chunk.payload = KittyPayload::Close;
            else if (value == "?")
                chunk.payload = KittyPayload::Query;
            else
                chunk.payload = KittyPayload::SetAside;
            break;
        case 'o':
            if (value == "unfocused")
                chunk.occasion = NotificationOccasion::Unfocused;
            else if (value == "invisible")
                chunk.occasion = NotificationOccasion::Invisible;
            else
                chunk.occasion = NotificationOccasion::Always;
            break;
        default:
            break;
        }
    }
    return chunk;
}

// Appends a chunk's text to a field, never past the field's bound.
void appendKittyText(std::string& field, std::string_view text, bool isBase64) // bool-ok: the chunk's own flag, passed on
{
    const std::size_t room = attentionTextCapacity - std::min(field.size(), attentionTextCapacity);
    if (room == 0)
        return;
    if (!isBase64) {
        field.append(text.substr(0, room));
        return;
    }
    // Whole groups only, and no more of them than could fill the field:
    // the encoded text is read no further than its decoded bytes are kept.
    std::array<char, attentionTextCapacity + 3> decoded;
    const std::size_t maxEncoded = Codec::base64EncodedLength(attentionTextCapacity);
    if (text.size() > maxEncoded)
        text = text.substr(0, maxEncoded - maxEncoded % 4);
    const auto written = Codec::base64Decode(text, decoded);
    if (!written || written->empty())
        return;
    field.append(written->data(), std::min(written->size(), room));
}

} // namespace

std::optional<KittyNotification> KittyNotificationAssembler::accept(std::string_view payload, std::optional<Ticks> now)
{
    // An unfinished notification past its time is gone before anything joins it.
    if (now) {
        std::erase_if(m_open, [now](const Open& open) {
            return open.startedAt && now->since(*open.startedAt) > kittyNotificationAssemblyTicks;
        });
    }

    // The metadata runs to the first `;`. A payload with none is all text.
    std::string_view metadata;
    std::string_view text = payload;
    if (const std::size_t semicolon = payload.find(';'); semicolon != std::string_view::npos) {
        metadata = payload.substr(0, semicolon);
        text = payload.substr(semicolon + 1);
    }
    KittyChunk chunk = parseKittyMetadata(metadata);

    const auto held = std::ranges::find_if(m_open, [&chunk](const Open& open) { return open.notification.identifier == chunk.identifier; });
    if (chunk.payload == KittyPayload::Close) {
        if (held != m_open.end())
            m_open.erase(held);
        return std::nullopt;
    }
    if (chunk.payload == KittyPayload::SetAside || chunk.payload == KittyPayload::Query)
        return std::nullopt;

    Open current;
    if (held != m_open.end()) {
        current = std::move(*held);
        m_open.erase(held);
    } else {
        current.notification.identifier = std::move(chunk.identifier);
        current.startedAt = now;
    }
    if (chunk.occasion)
        current.notification.occasion = *chunk.occasion;
    appendKittyText(chunk.payload == KittyPayload::Body ? current.notification.body : current.notification.title, text, chunk.isBase64);

    if (!chunk.isDone) {
        // Oldest first, so the one abandoned for room is the one begun
        // longest ago.
        if (m_open.size() == maxOpenKittyNotifications)
            m_open.erase(m_open.begin());
        m_open.push_back(std::move(current));
        return std::nullopt;
    }
    if (current.notification.title.empty() && current.notification.body.empty())
        return std::nullopt;
    return std::move(current.notification);
}

void handleKittyNotification(Ubiquitty::Terminal& t, std::string_view p)
{
    const std::size_t separator = p.find(';');
    if (separator != std::string_view::npos) {
        const KittyChunk chunk = parseKittyMetadata(p.substr(0, separator));
        if (chunk.payload == KittyPayload::Query) {
            ReplyBuilder reply;
            reply.put("\033]99;i=");
            reply.put(chunk.identifier);
            reply.put(":p=?;p=title,body:o=always,unfocused,invisible\033\\");
            respond(t, Capability::StatusReport, reply.span());
            return;
        }
    }
    const std::optional<KittyNotification> finished = t.kittyNotifications.accept(p, Ubiquitty::terminalNow(t));
    if (!finished)
        return;
    dispatchNotification(t, { .title = finished->title, .body = finished->body, .identifier = finished->identifier, .occasion = finished->occasion });
}

// OSC 777 — urxvt perl extension dispatch.
// OSC 777 ; notify ; <title> [ ; <body> ] ST is the one extension with a
// terminal-side meaning. Any other extension is consumed here so its payload
// never reaches the grid, and reported to the diagnostics client as an unknown
// code, which is how a conformance probe can tell "seen and ignored" from
// "printed".
void handleOSC777(Ubiquitty::Terminal& t, std::string_view p)
{
    static constexpr std::string_view notifyModule = "notify;";
    if (!p.starts_with(notifyModule)) {
        if (auto* c = t.diagnosticsClient.get())
            c->onUnknownOSCCode(777, p);
        return;
    }
    p.remove_prefix(notifyModule.size());
    std::string_view title = p;
    std::string_view body;
    if (const std::size_t semicolon = p.find(';'); semicolon != std::string_view::npos) {
        title = p.substr(0, semicolon);
        body = p.substr(semicolon + 1);
    }
    if (title.empty() && body.empty())
        return;
    dispatchNotification(t, { .title = title, .body = body, .identifier = {}, .occasion = NotificationOccasion::Always });
}

// OSC 21337 — iTerm2's session status (Docs/tab-status.md).
// Payload: key=value;key=value;... with `\;` and `\\` escapes inside values.
// Two keys are kept. `indicator` is a #RRGGBB colour the app sets per session
// state — idle/busy/waiting — and a bare key or empty value clears it.
// `status` is the label for that state ("Waiting"), shown when the reader hovers
// the indicator. The label travels with the escape that sets the colour: an
// escape that sets one without a `status` clears the old label rather than
// leaving "Waiting" beside the colour for busy, and clearing the colour clears
// the label. `status-color` and `detail` are accepted and ignored; the
// indicator is the colour.
// Tells whoever draws the tab that its status marks moved: the generation the
// OSC 21337 indicator, the tab colour and the badge share, and a repaint.
static void noteTabStatusChanged(Ubiquitty::Terminal& t)
{
    t.tabStatusGeneration++;
    if (auto* c = Ubiquitty::clientForEffectThatEscapesNothing(t.windowClient))
        c->invalidate();
}

// OSC 6 ; 1 ; bg ; <red|green|blue> ; brightness ; <0-255>   one channel
// OSC 6 ; 1 ; bg ; * ; default                                reset
// iTerm2 sends a colour as three sequences, so a channel set on its own is a
// colour whose other channels are zero until theirs arrive.
void handleTabColor(Ubiquitty::Terminal& t, std::string_view p)
{
    if (p == "*;default") {
        if (t.tabColorSet == 0)
            return;
        t.tabColorSet = 0;
        t.tabColorRed = t.tabColorGreen = t.tabColorBlue = 0;
        noteTabStatusChanged(t);
        return;
    }
    const std::size_t channelEnd = p.find(';');
    if (channelEnd == std::string_view::npos)
        return;
    const std::string_view channel = p.substr(0, channelEnd);
    uint8_t Ubiquitty::Terminal::* member = nullptr;
    if (channel == "red")
        member = &Ubiquitty::Terminal::tabColorRed;
    else if (channel == "green")
        member = &Ubiquitty::Terminal::tabColorGreen;
    else if (channel == "blue")
        member = &Ubiquitty::Terminal::tabColorBlue;
    else
        return;
    static constexpr std::string_view brightness = "brightness;";
    std::string_view digits = p.substr(channelEnd + 1);
    if (!digits.starts_with(brightness))
        return;
    digits.remove_prefix(brightness.size());
    // One to three digits and no more: a longer run is not a brightness, and
    // reading it would be arithmetic on input of any length.
    if (digits.empty() || digits.size() > 3)
        return;
    unsigned value = 0;
    for (const char digit : digits) {
        if (digit < '0' || digit > '9')
            return;
        value = value * 10 + static_cast<unsigned>(digit - '0');
    }
    if (value > 255)
        return;
    if (t.tabColorSet != 0 && t.*member == value)
        return; // unchanged — don't wake the frontend
    t.tabColorSet = 1;
    t.*member = static_cast<uint8_t>(value);
    noteTabStatusChanged(t);
}

// OSC 1337 ; SetBadgeFormat=<base64>. The text is kept as sent, bounded, with
// control characters dropped: it is drawn as a label, and iTerm2's \(…)
// interpolations name session variables this terminal does not evaluate.
void handleBadgeFormat(Ubiquitty::Terminal& t, std::string_view encoded)
{
    // No more of the payload is read than decodes to the bound and one scalar
    // past it, so the cut below can see whether it would split a sequence.
    static constexpr std::size_t maxEncodedBytes = Codec::base64EncodedLength(badgeTextCapacity + 4);
    if (encoded.size() > maxEncodedBytes)
        encoded = encoded.substr(0, maxEncodedBytes);
    std::array<char, badgeTextCapacity + 8> decoded {};
    const auto badge = Codec::base64Decode(encoded, decoded);
    if (!badge)
        return; // malformed base64 — keep the badge that was there
    std::string text;
    text.reserve(badge->size());
    for (const char byte : *badge) {
        if (static_cast<unsigned char>(byte) >= 0x20 && byte != 0x7F)
            text.push_back(byte);
    }
    text = Ubiquitty::boundedUTF8String(text, badgeTextCapacity);
    if (text == t.badgeText)
        return; // unchanged — don't wake the frontend
    t.badgeText = std::move(text);
    noteTabStatusChanged(t);
}

void handleOSC21337(Ubiquitty::Terminal& t, std::string_view p)
{
    auto hexDigit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };

    bool sawIndicator = false;
    bool cleared = false;
    uint8_t r = 0, g = 0, b = 0;
    std::array<char, tabStatusTextCapacity> text {};
    std::size_t textLength = 0;

    std::string_view s = p;
    while (!s.empty()) {
        const std::size_t keyEnd = s.find_first_of("=;");
        const std::string_view key = s.substr(0, keyEnd);
        s.remove_prefix(key.size());

        // The value is unescaped as it is read (`\;` and `\\` stand for their
        // second byte), so it cannot be a view into the payload the way the key
        // is — a decoded byte has nowhere in the payload to point at.
        // Room for the longest label kept plus one more scalar, so the cut to
        // the label's budget can see whether it would split a sequence.
        std::array<char, tabStatusTextCapacity + 4> valBuffer {};
        int vlen = 0;
        if (s.starts_with('=')) {
            s.remove_prefix(1);
            while (!s.empty() && s.front() != ';') {
                char c = s.front();
                if (c == '\\' && s.size() >= 2 && (s[1] == ';' || s[1] == '\\')) {
                    c = s[1];
                    s.remove_prefix(2);
                } else {
                    s.remove_prefix(1);
                }
                if (vlen < static_cast<int>(valBuffer.size()) - 1)
                    valBuffer[vlen++] = c;
            }
        }
        valBuffer[vlen] = '\0';
        if (s.starts_with(';'))
            s.remove_prefix(1);

        if (key == "status") {
            const std::string_view label = Title::titleBytePrefix(
                std::string_view(valBuffer.data(), static_cast<std::size_t>(vlen)), tabStatusTextCapacity);
            std::ranges::copy(label, text.begin());
            textLength = label.size();
        } else if (key == "indicator") {
            sawIndicator = true;
            if (vlen == 0) { // bare key or key= → clear
                cleared = true;
            } else if (valBuffer[0] == '#' && vlen == 7) { // #RRGGBB
                int rh = hexDigit(valBuffer[1]), rl = hexDigit(valBuffer[2]);
                int gh = hexDigit(valBuffer[3]), gl = hexDigit(valBuffer[4]);
                int bh = hexDigit(valBuffer[5]), bl = hexDigit(valBuffer[6]);
                if ((rh | rl | gh | gl | bh | bl) < 0) { // malformed → ignore
                    sawIndicator = false;
                } else {
                    r = static_cast<uint8_t>(rh * 16 + rl);
                    g = static_cast<uint8_t>(gh * 16 + gl);
                    b = static_cast<uint8_t>(bh * 16 + bl);
                }
            } else { // unsupported form → ignore
                sawIndicator = false;
            }
        }
    }

    if (!sawIndicator)
        return;

    if (cleared)
        textLength = 0;
    const std::string_view keptText(t.tabStatusText.data(), t.tabStatusTextLength);
    const bool didTextChange = keptText != std::string_view(text.data(), textLength);
    bool changed;
    if (cleared) {
        changed = (t.tabStatusSet != 0) || didTextChange;
        t.tabStatusSet = 0;
    } else {
        changed = (t.tabStatusSet != 1 || t.tabStatusR != r || t.tabStatusG != g || t.tabStatusB != b) || didTextChange;
        t.tabStatusSet = 1;
        t.tabStatusR = r;
        t.tabStatusG = g;
        t.tabStatusB = b;
    }
    t.tabStatusText = text;
    t.tabStatusTextLength = static_cast<uint8_t>(textLength);
    if (!changed)
        return; // unchanged tab status — don't wake the frontend
    noteTabStatusChanged(t);
}

} // namespace Ubiquitty::OSC
