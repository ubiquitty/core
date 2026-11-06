// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OSC state slice: notifications that store onto the Terminal and optionally route to a client hook —
// OSC 6 (current file URL), 7 (working directory), 8 (hyperlinks), 50 (font). setCurrentDirectory lives
// here; OSC 633;P;Cwd and OSC 1337 CurrentDir reuse it.

#include "Core/Attributes/Hyperlinks.h"
#include "Core/Cursor/Style.h"
#include "Core/OSC/Internal.h"
#include "Core/ReplyBuilder.h"
#include "Core/Title/Directory.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Ubiquitty::OSC {

// Mirror a decoded currentDirectory into the Terminal (besides the shellClient hook) so a consumer with no
// shellClient (the host daemon) can read it for CommandDone / SessionList.
void setCurrentDirectory(Ubiquitty::Terminal& t, std::string_view path)
{
    // Latch only a real change: shells re-emit OSC 7 on every prompt with an
    // unchanged directory, and an unconditional latch would hop to main per
    // prompt for a no-op title recompute. Runs once per OSC dispatch — never
    // on a byte path.
    // C0 controls and DEL never belong in a directory path, and the value is
    // forwarded into titles and the frontend's shell client — the same
    // report-back injection surface the title filter guards. Filtered here so
    // every producer (OSC 7, iTerm2 CurrentDir, OSC 633 Cwd) inherits it.
    std::string filtered;
    filtered.reserve(path.size());
    for (const char character : path) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7F)
            continue;
        filtered.push_back(character);
    }
    // Recorded as the REPORT, beside the sampled directory rather than over it:
    // the two are arbitrated by paneDirectory, and the probe after these bytes
    // stamps the program that made it, so the report retires when it exits.
    if (Ubiquitty::setReportedDirectoryString(t, filtered))
        t.currentDirectoryChanged = true;
}

// OSC 6 ; <file-url>
// The current document/file URL — the file analog of OSC 7's working
// directory. Editors (vim's t_ts=ESC]6;) emit it so the title-bar
// proxy icon points at the focused file; the host segment (when present)
// distinguishes a local file from one opened over SSH. We store the URL
// verbatim — a proxy-icon / reveal-in-Finder consumer needs the whole URL,
// not just a stripped path. An empty payload clears it (editors send that for
// an unnamed buffer). Leading whitespace is trimmed to match OSC 7. The value
// is NUL-terminated and truncated to fit; there is no shellClient hook (OSC 6
// is consumer-read off the Terminal, like remoteHost).
void handleCurrentFileURL(Ubiquitty::Terminal& t, std::string_view p)
{
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t'))
        p.remove_prefix(1);
    // SetCurrentFileURL: OSC 6 sets the focused document/file URL — a metadata
    // write read off the Terminal (proxy-icon / reveal-in-Finder). Origin-aware; a
    // denied origin skips it. An unset policy answers PAL's default.
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetCurrentFileURL))
        return; // host policy denied the file-URL write — skip it
    Ubiquitty::setCurrentFileURLString(t, p);
}

// OSC 7 ; file://<authority>/<absolute-path>
// The wire format is a file:// URL. An empty authority, "localhost", and the
// current platform hostname name this machine and record no host. Any other authority names the machine
// the reporting shell is on and is recorded as Terminal::reportedHost, only
// where both SetWorkingDirectory and SetRemoteHost allow it. The path must be
// absolute, starting at the '/' that ends the authority; a report without one
// is dropped. %XX escapes are decoded into a bounded stack buffer, truncated
// to fit, so the store receives a plain POSIX path. The shellClient hook, if
// one is installed, receives the stored directory after the store's filter.
void handleWorkingDirectory(Ubiquitty::Terminal& t, std::string_view p)
{
    // Decode unconditionally (no early shellClient gate): the currentDirectory is mirrored
    // into the Ubiquitty::Terminal below so the host daemon — which installs no shellClient —
    // still captures it. The hook fires afterward only if one is installed.

    // Trim leading whitespace; some shells indent the OSC payload.
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t'))
        p.remove_prefix(1);

    // An empty report withdraws the last one, host and directory both, so a
    // shell leaving the place it named (an ssh that exits, a prompt that stops
    // reporting) is not left describing it. The same gate as a report applies.
    if (p.empty()) {
        if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetWorkingDirectory))
            return;
        if (Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetRemoteHost) && Ubiquitty::setReportedHostString(t, {}))
            t.currentDirectoryChanged = true;
        setCurrentDirectory(t, {});
        if (auto* shell = t.shellClient.get())
            shell->setCurrentDirectory(t.currentDirectory);
        return;
    }

    // Accept only file:// — the only scheme the sequence is defined to carry;
    // a non-file: URL (e.g. ssh://) names no path this process can resolve.
    static constexpr std::string_view scheme = "file://";
    if (!p.starts_with(scheme))
        return;
    p.remove_prefix(scheme.size());

    // The optional host segment runs up to (but not including) the leading '/'
    // of the absolute path. file:///foo, file://localhost/foo and
    // file://hostname/foo all land at the '/'.
    //
    // It is KEPT, not skipped. A shell on the far side of an ssh names the
    // machine it is on here, and that is the only thing on the wire that can
    // see past a nested ssh — this machine's process tree holds one ssh however
    // many the reader has opened. An empty authority and `localhost` are both
    // spellings of this machine, so neither is recorded as a host.
    //
    // The host is captured here and recorded only below the capability gates:
    // a stream the policy refuses does not name the machine either. The view
    // is into the payload parameter, which the rest of the handler advances
    // past and never rewrites.
    const std::size_t slash = p.find('/');
    if (slash == std::string_view::npos)
        return; // No absolute path component — drop silently.
    const std::string_view authority = p.substr(0, slash);
    const std::string_view reportedHost = Ubiquitty::Title::isLocalURLAuthority(authority) ? std::string_view {} : authority;
    p.remove_prefix(slash);

    // URL-decode into a fixed-size buffer. 255 chars + NUL matches the
    // Session.currentDir cap; longer paths are truncated, matching the
    // sink. We decode strictly: a stray '%' without two hex digits is
    // passed through as a literal '%' rather than the whole OSC being
    // rejected — shells in the wild are inconsistent and dropping the
    // sequence loses the currentDirectory hint entirely.
    char path[256];
    int outputLength = 0;
    const int capacity = static_cast<int>(sizeof(path)) - 1;
    while (!p.empty() && outputLength < capacity) {
        const unsigned char c = static_cast<unsigned char>(p.front());
        // A truncated escape at the end of the payload has no two digits to
        // read, so the size test is what makes the lookahead legal rather than
        // a NUL that happens to fail hexDigit.
        if (c == '%' && p.size() >= 3) {
            const int hi = hexDigit(p[1]);
            const int lo = (hi >= 0) ? hexDigit(p[2]) : -1;
            if (hi >= 0 && lo >= 0) {
                path[outputLength++] = static_cast<char>((hi << 4) | lo);
                p.remove_prefix(3);
                continue;
            }
            // Malformed escape: emit '%' literally and advance one byte.
        }
        path[outputLength++] = static_cast<char>(c);
        p.remove_prefix(1);
    }
    path[outputLength] = 0;

    // The authority split left p at '/', and the first decode iteration copies
    // that '/' to path[0]: it is not '%', so it takes the literal append. The
    // decoded path therefore always starts with '/'; even file:///%2e decodes
    // to "/.", not ".".
    //
    // SetWorkingDirectory: OSC 7 sets the session CWD — a metadata write the
    // host daemon reads (CommandDone / SessionList) plus the shellClient hook.
    // Origin-aware; a denied origin skips every write below: the host, the
    // directory and the shellClient hook. An unset policy answers PAL's default.
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetWorkingDirectory))
        return; // host policy denied the working-directory write — skip it
    // SetRemoteHost: the authority names the machine the session is on, the
    // identity OSC 1337 RemoteHost= writes, so it answers to that capability
    // too. A refused host is neither replaced nor cleared; the directory from
    // the same report still applies.
    if (Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetRemoteHost) && Ubiquitty::setReportedHostString(t, reportedHost))
        t.currentDirectoryChanged = true;
    setCurrentDirectory(t, std::string_view(path, static_cast<std::size_t>(outputLength)));

    // Pass the path to the client hook if one is installed — the stored value,
    // not the raw decode: the store filtered it.
    if (auto* shell = t.shellClient.get())
        shell->setCurrentDirectory(t.currentDirectory);
}

// OSC 8 — hyperlinks. Wire form, per the published OSC 8 specification
// (https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda):
// OSC 8 ; parameters ; URI ST   open a hyperlink covering subsequent cells
// OSC 8 ; ; ST              close the current hyperlink (empty URI)
//
// `parameters` is a colon-separated list of key=value pairs; the only one
// with defined meaning is `id=<token>`, a dedup key letting a logical
// link that spans multiple runs (e.g. wrapped output) share one table
// entry. Other parameters are parsed-and-ignored. The leading "8;" has been
// consumed by oscParseLeadingPsCode, so `p` points at the parameters field.
//
// The parameters field runs up to the first ';'; everything after that ';'
// is the URI (which may itself contain ';' — only the first separator
// splits parameters from URI). A missing second ';' (no URI field at all)
// is treated as a close. The handler interns the URI into the Terminal-side
// link table and sets the current link id; emitCharacter stamps it.
void handleHyperlink(Ubiquitty::Terminal& t, std::string_view p)
{
    // SetHyperlink: OSC 8 interns a URI and arms currentLinkID. LowRiskInfo: a
    // host that distrusts a remote peer aiming a link at a phishing target under
    // spoofed visible text restricts the tier, and a refused origin skips it. An
    // unset policy answers PAL's default.
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetHyperlink))
        return; // host policy denied the hyperlink write — skip it

    // Split parameters (up to the first ';') from the URI (the rest). No ';' at
    // all is malformed; the empty URI that leaves makes it a close, so a stray
    // OSC 8 does not leave a link wedged open.
    std::string_view parameters;
    std::string_view uri;
    if (const std::size_t semicolon = p.find(';'); semicolon != std::string_view::npos) {
        parameters = p.substr(0, semicolon);
        uri = p.substr(semicolon + 1);
    }

    // Scan parameters for an `id=<token>` pair. Parameters are ':'-separated.
    //
    // The key is clipped to the ring field here rather than in intern, because a
    // key is a dedup token and an over-long one has to arrive at the same bytes
    // every time: a shell emitting one per prompt would otherwise mint a fresh
    // id per prompt and churn the ring out of a link it keeps re-opening. intern
    // compares whole targets and so refuses to dedup one that does not fit,
    // which is right for a URI reaching it directly — that is the link itself,
    // and two targets differing past the field are two links.
    static constexpr std::string_view idKey = "id=";
    std::string_view key;
    while (!parameters.empty()) {
        const std::size_t colon = parameters.find(':');
        const std::string_view parameter = parameters.substr(0, colon);
        if (parameter.starts_with(idKey))
            key = parameter.substr(idKey.size(), static_cast<std::size_t>(Ubiquitty::VT::maxLinkKeyLength) - 1);
        if (colon == std::string_view::npos)
            break;
        parameters = parameters.substr(colon + 1);
    }

    Ubiquitty::Attributes::Hyperlinks::intern(&t, key, uri);
}

// OSC 50 — set / query / cycle font. Wire forms:
//   OSC 50 ; ? ST                 — query (reply: OSC 50 ; <name> ST)
//   OSC 50 ; <family> ST          — set family (macOS spec; see below)
//   OSC 50 ; <family>:size=N ST   — set family + absolute point size
//   OSC 50 ; #N ST                — rxvt: absolute point size, keep family
//   OSC 50 ; #+N / #-N ST         — rxvt: relative point bump, keep family
//   OSC 50 ;  ST                  — empty: reset to the config default font
//
// The historical payload is an X Logical Font Description (XLFD), meaningless
// on macOS. Ubiquitty defines a family-name grammar over the same [font] config
// axes (family + size) with a `:size=N` suffix instead. The parsed request
// is handed to WindowClient::setFont for the live swap; Terminal::font keeps the
// last family for the `?` query. Gate via allowFontOperations.
// Read a point size off the head of `text`, reporting whether a number was there:
// an optional sign, then digits with an optional fraction.
//
// Read here rather than by strtod, whose decimal point is LC_NUMERIC's: a
// frontend whose toolkit adopts a comma-decimal locale would read "15.5" as 15.
// A control's operand is never localized.
static bool readPointSize(std::string_view text, double& out)
{
    bool isNegative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        isNegative = text.front() == '-';
        text.remove_prefix(1);
    }
    // Past eighteen significant digits, more than a double carries, only the
    // scale of the integer part still matters.
    constexpr std::uint64_t mantissaLimit = 100'000'000'000'000'000ULL;
    std::uint64_t mantissa = 0;
    int decimalExponent = 0;
    int digitCount = 0;
    bool isInFraction = false;
    for (const char character : text) {
        if (character == '.' && !isInFraction) {
            isInFraction = true;
            continue;
        }
        if (character < '0' || character > '9')
            break;
        ++digitCount;
        if (mantissa < mantissaLimit) {
            mantissa = mantissa * 10 + static_cast<std::uint64_t>(character - '0');
            if (isInFraction)
                --decimalExponent;
        } else if (!isInFraction) {
            ++decimalExponent;
        }
    }
    if (!digitCount)
        return false;
    double value = static_cast<double>(mantissa);
    if (decimalExponent > 0)
        value *= std::pow(10.0, static_cast<double>(decimalExponent));
    else if (decimalExponent < 0)
        value /= std::pow(10.0, static_cast<double>(-decimalExponent));
    if (!std::isfinite(value))
        return false;
    out = isNegative ? -value : value;
    return true;
}

// Konsole's cursor-shape control, `OSC 50 ; CursorShape=N`: 0 is a block, 1
// a bar and 2 an underline, the blink left as it is. It shares OSC 50 with the
// font but names no font; vim and neovim configurations send it to Konsole for
// the insert-mode cursor. Any other value changes nothing.
static void applyKonsoleCursorShape(Ubiquitty::Terminal& t, std::string_view value)
{
    if (value.size() != 1 || value[0] < '0' || value[0] > '2')
        return;
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetCursorShape))
        return;
    using Style = Ubiquitty::Cursor::Style;
    const bool isBlinking = Ubiquitty::Cursor::isBlinking(t.cursorStyle);
    switch (value[0]) {
    case '0':
        t.cursorStyle = isBlinking ? Style::BlinkingBlock : Style::SteadyBlock;
        break;
    case '1':
        t.cursorStyle = isBlinking ? Style::BlinkingBar : Style::SteadyBar;
        break;
    default:
        t.cursorStyle = isBlinking ? Style::BlinkingUnderline : Style::SteadyUnderline;
        break;
    }
}

// Hands one font change to the frontend: its window client when it installed
// one, or the request queue a frontend applying off the parser thread drains.
// Both refuse what the SetFont capability does not grant. The axes are
// WindowClient::setFont's, whose contract keeps columns and rows.
static bool requestFontChange(Ubiquitty::Terminal& t, std::string_view family, double sizePoints, double sizeDelta)
{
    Ubiquitty::WindowClient* window = nullptr;
    if (t.windowClient.installed()) {
        window = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::SetFont, family);
        if (!window)
            return false;
    } else if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetFont, family)) {
        return false;
    }
    return window ? window->setFont(family, sizePoints, sizeDelta)
                  : Ubiquitty::recordFontRequest(t, family, sizePoints, sizeDelta);
}

// Hands mintty's size reset to the frontend the way requestFontChange hands a
// font change, under the same capability: the configured size, the family kept.
static void requestFontSizeReset(Ubiquitty::Terminal& t)
{
    if (t.windowClient.installed()) {
        if (Ubiquitty::WindowClient* window = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::SetFont, {}))
            (void)window->resetFontSize();
        return;
    }
    if (Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetFont, {}))
        (void)Ubiquitty::recordFontSizeReset(t);
}

void handleFont(Ubiquitty::Terminal& t, std::string_view p)
{
    static constexpr std::string_view cursorShapeKey = "CursorShape=";
    if (p.starts_with(cursorShapeKey)) {
        applyKonsoleCursorShape(t, p.substr(cursorShapeKey.size()));
        return;
    }
    if (p.starts_with('?') && (p.size() == 1 || p[1] == ';')) {
        const std::string_view family = t.fontRequests.actualFamily().empty() ? std::string_view(t.font) : std::string_view(t.fontRequests.actualFamily());
        const std::string reply = "\033]50;" + Ubiquitty::sanitizedReplyLabel(family) + std::string(Ubiquitty::oscReplyTerminator(t));
        Ubiquitty::respond(t, Ubiquitty::Capability::StatusReport, reply);
        return;
    }

    std::string_view family;
    double sizePoints = 0.0; // absolute, points; 0 ⇒ keep current
    double sizeDelta = 0.0; // relative bump, points; 0 ⇒ none

    const bool isSizeCycle = p.starts_with('#');
    if (isSizeCycle) {
        // rxvt size cycle: #N absolute, #+N / #-N relative. A malformed
        // numeric tail (no digits) is ignored — no swap.
        const std::string_view numeric = p.substr(1);
        const bool isRelative = numeric.starts_with('+') || numeric.starts_with('-');
        double value = 0.0;
        if (!readPointSize(numeric, value))
            return; // malformed
        if (isRelative)
            sizeDelta = value;
        else
            sizePoints = value;
    } else {
        // family[:size=N]: everything before ":size=" is the family, the
        // numeric tail after it is the absolute point size.
        static constexpr std::string_view sizeTag = ":size=";
        const std::size_t tag = p.find(sizeTag);
        family = p.substr(0, tag);
        if (tag != std::string_view::npos) {
            double value = 0.0;
            if (readPointSize(p.substr(tag + sizeTag.size()), value) && value > 0.0)
                sizePoints = value;
        }
    }

    if (!requestFontChange(t, family, sizePoints, sizeDelta))
        return;
    // Refusal leaves the accepted query family untouched. A delayed adoption
    // may publish its actual family only until a newer request names one.
    if (!isSizeCycle) {
        Ubiquitty::setFontString(t, family);
        t.fontRequests.noteAcceptedQueryFamily();
    }
}

// mintty's font-size queries, `OSC 7770 ; ? ST` and `OSC 7777 ; ? ST`: "When the
// font size is queried, a sequence that would restore the current font size is
// sent." The size is the one the frontend last reported drawing in; until one
// has, there is no size to restore and no reply.
static void reportFontSize(Ubiquitty::Terminal& t, int code)
{
    const double size = t.fontRequests.actualSizePoints();
    if (!(size > 0.0) || !std::isfinite(size))
        return;
    // The shortest decimal that reads back as the size, "16" or "15.5", with a
    // point whatever the process's locale.
    std::array<char, 32> digits;
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), size);
    if (converted.ec != std::errc {})
        return;
    ReplyBuilder reply;
    reply.put("\033]");
    reply.putDecimal(code);
    reply.put(';');
    reply.put(std::string_view(digits.data(), static_cast<std::size_t>(converted.ptr - digits.data())));
    reply.put(Ubiquitty::oscReplyTerminator(t));
    Ubiquitty::respond(t, Ubiquitty::Capability::StatusReport, reply.span());
}

// A mintty size operand: digits with an optional fraction, after an optional
// sign the caller has already read. Anything else is malformed.
[[nodiscard]] static std::optional<double> readMinttyPointSize(std::string_view text)
{
    if (text.empty() || text.size() > 16 || text.front() < '0' || text.front() > '9')
        return std::nullopt;
    bool hasPoint = false;
    for (const char character : text) {
        if (character == '.' && !hasPoint)
            hasPoint = true;
        else if (character < '0' || character > '9')
            return std::nullopt;
    }
    double value = 0.0;
    if (!readPointSize(text, value))
        return std::nullopt;
    return value;
}

// mintty's two font-size controls take the same operands: "?" queries, "num"
// sets the size, "+num" and "-num" change it, and an empty operand restores the
// default size, the family left as it is, so a family a program set with OSC 50
// stays the one OSC 50 ; ? reports. Both reach the frontend's font change, which
// keeps the character geometry: 7777 "adapts the window size so the terminal
// character geometry is kept", and 7770, whose mintty keeps the window instead,
// still sets the size its query reports, so the reply that query sends restores
// it.
static void applyMinttyFontSize(Ubiquitty::Terminal& t, int code, std::string_view p)
{
    if (p == "?") {
        reportFontSize(t, code);
        return;
    }
    if (p.empty()) {
        requestFontSizeReset(t);
        return;
    }
    double sizePoints = 0.0;
    double sizeDelta = 0.0;
    const bool isRelative = p.front() == '+' || p.front() == '-';
    const std::optional<double> value = readMinttyPointSize(isRelative ? p.substr(1) : p);
    if (!value)
        return;
    if (isRelative)
        sizeDelta = p.front() == '-' ? -*value : *value;
    else if (*value > 0.0)
        sizePoints = *value;
    else
        return;
    if (sizePoints == 0.0 && sizeDelta == 0.0)
        return;
    (void)requestFontChange(t, {}, sizePoints, sizeDelta);
}

void handleFontSize(Ubiquitty::Terminal& t, std::string_view p) { applyMinttyFontSize(t, 7770, p); }

void handleFontAndWindowSize(Ubiquitty::Terminal& t, std::string_view p) { applyMinttyFontSize(t, 7777, p); }

} // namespace Ubiquitty::OSC
