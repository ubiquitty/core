// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Tmux/Tmux.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Pull the Base/Types.h aliases into this TU so the unqualified
// spellings (bool / uint8_t / uint32_t) work in function bodies and
// static helpers. The header already qualifies its public surface
// with Ubiquitty:: so the using directive does not bleed through.

// ---------- helpers ----------

namespace Ubiquitty::Protocol::Tmux {

static void resetLine(TmuxClient* tc)
{
    tc->lineLength = 0;
    tc->line[0] = 0;
}

static bool lineStartsWith(std::string_view line, std::string_view prefix)
{
    return line.starts_with(prefix);
}

// A genuine control-mode response frame is `%begin|%end|%error <ts> <num> <flags>`
// — the marker is always followed by a decimal timestamp. tmux does not escape the
// *body* of a command response (only %output/%extended-output notification payloads
// are octal-escaped), so a body line can legitimately start with these words — e.g.
// `capture-pane -e -p` of a screen showing "%end of file" or "%begin transaction".
// Requiring the trailing digit keeps such untrusted content from being mistaken for
// a frame, which would desync responseDepth and drop later rows to the top-level
// handler (where a row like "%exit" would tear the whole session down).
static bool isResponseFrame(std::string_view line, std::string_view marker)
{
    if (!lineStartsWith(line, marker))
        return false;
    return line.size() > marker.size() && line[marker.size()] >= '0' && line[marker.size()] <= '9';
}

// Parse a non-negative decimal integer at *p, advancing *p past it.
// Returns -1 if no digits. Used for protocol fields like pane IDs and
// the begin-response counters.
static int parseNonNegativeInteger(const char** p, const char* end)
{
    int v = 0;
    bool any = false;
    while (*p < end && **p >= '0' && **p <= '9') {
        if (v <= maxTmuxValue)
            v = v * 10 + (**p - '0');
        any = true;
        (*p)++;
    }
    if (v > maxTmuxValue)
        v = maxTmuxValue;
    return any ? v : -1;
}

// Parse a decimal layout dimension/coordinate, advancing *p. Caps at
// tmuxMaxLayoutDimension (5 digits) so a hostile field can't integer-overflow
// the accumulator. Returns -1 on no digits or overflow.
static int parseDimension(const char** p, const char* end)
{
    long v = 0;
    bool any = false;
    int digits = 0;
    while (*p < end && **p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
        any = true;
        if (++digits > 5 || v > tmuxMaxLayoutDimension)
            return -1;
    }
    return any ? static_cast<int>(v) : -1;
}

// Decode tmux's `%output` payload escapes back into raw bytes,
// delivering each byte via cb. tmux escapes only \\ and \xxx (octal,
// exactly 3 digits) for the data portion of %output. Unknown escapes
// pass through as the literal sequence so we don't drop bytes if the
// protocol gains new escape forms.
// ---------- line dispatch ----------

static void processSessionChangedLine(TmuxClient* tc, std::string_view body)
{
    // The owner must query the new session before retaining a restore identity.
    (void)body;
    if (tc->events.sessionChanged) {
        tc->events.sessionChanged(tc->events.context);
    }
}

// Parse the `@<windowID>` token at the head of a window notification
// body, advancing *p past it. Returns -1 if absent/malformed.
static int parseWindowID(const char** p, const char* end)
{
    if (*p >= end || **p != '@')
        return -1;
    (*p)++;
    return parseNonNegativeInteger(p, end);
}

static void processWindowAddLine(TmuxClient* tc, std::string_view body)
{
    // "%window-add @<windowID>"
    const char* p = body.data();
    int id = parseWindowID(&p, body.data() + body.size());
    if (id >= 0 && tc->events.windowAdd)
        tc->events.windowAdd(tc->events.context, id);
}

static void processWindowCloseLine(TmuxClient* tc, std::string_view body)
{
    // "%window-close @<windowID>"
    const char* p = body.data();
    int id = parseWindowID(&p, body.data() + body.size());
    if (id >= 0 && tc->events.windowClose)
        tc->events.windowClose(tc->events.context, id);
}

static void processWindowRenamedLine(TmuxClient* tc, std::string_view body)
{
    // "%window-renamed @<windowID> <name>" — name is the rest of the line (may
    // contain spaces). Hand it to the host as a borrowed view off the parse buffer.
    const char* end = body.data() + body.size();
    const char* p = body.data();
    int id = parseWindowID(&p, end);
    if (id < 0)
        return;
    if (p < end && *p == ' ')
        p++;
    if (tc->events.windowRenamed)
        tc->events.windowRenamed(tc->events.context, id, std::string_view(p, static_cast<size_t>(end - p)));
}

static void processSessionRenamedLine(TmuxClient* tc, std::string_view body)
{
    // "%session-renamed $<sessionID> <name>" — real tmux 3.x prefixes the session id
    // (the man page's name-only form is stale; verified against tmux 3.6a). The -CC
    // attach follows one session, so the id isn't needed: skip it, take the rest.
    const char* end = body.data() + body.size();
    const char* p = body.data();
    if (p < end && *p == '$') {
        p++;
        while (p < end && *p >= '0' && *p <= '9')
            p++;
    }
    if (p < end && *p == ' ')
        p++;
    if (tc->events.sessionRenamed)
        tc->events.sessionRenamed(tc->events.context, std::string_view(p, static_cast<size_t>(end - p)));
}

static void processPausedLine(TmuxClient* tc, std::string_view body)
{
    // "%pause %<paneID>" — hand to the host, which answers with a
    // refresh-client continue (this client does no flow control of its own,
    // so a paused pane must be resumed immediately or it freezes for good).
    const char* p = body.data();
    const char* end = body.data() + body.size();
    while (p < end && *p != '%')
        p++;
    if (p >= end)
        return;
    p++;
    int pane = parseNonNegativeInteger(&p, end);
    if (pane >= 0 && pane <= maxTmuxPaneID && tc->events.paneDidPause)
        tc->events.paneDidPause(tc->events.context, pane);
}

static void processPaneModeChangedLine(TmuxClient* tc, std::string_view body)
{
    // "%pane-mode-changed %<paneID>" — the pane's mode changed (copy/view mode,
    // or a `mouse on/off` toggle). tmux gives no value, so hand the pane id to
    // the host to re-query; without this the pane Terminal's DEC mouse flags go stale.
    const char* p = body.data();
    const char* end = body.data() + body.size();
    while (p < end && *p != '%')
        p++;
    if (p >= end)
        return;
    p++;
    int pane = parseNonNegativeInteger(&p, end);
    if (pane >= 0 && pane <= maxTmuxPaneID && tc->events.paneDidChangeMode)
        tc->events.paneDidChangeMode(tc->events.context, pane);
}

static void processPaneChangedLine(TmuxClient* tc, std::string_view body)
{
    // "%window-pane-changed @<windowID> %<paneID>" — switch our
    // keystroke target.
    const char* p = body.data();
    const char* end = body.data() + body.size();
    while (p < end && *p != '%')
        p++;
    if (p >= end)
        return;
    p++;
    int newPane = parseNonNegativeInteger(&p, end);
    if (newPane >= 0 && newPane <= maxTmuxPaneID)
        tc->activePaneID = newPane;
}

// One cell of a tmux layout tree: "<W>x<H>,<x>,<y>" then either ",<paneID>"
// (a leaf — record its rect) or "{...}" / "[...]" (a split container — recurse
// into the comma-separated child cells; tmux gives every cell ABSOLUTE x/y, so
// leaves are recorded as-is with no offset math). Advances *p. Returns false on
// a malformed cell or if the depth bound is exceeded. Panes past maxPanes or
// above the id ceiling are dropped (not a parse failure), so a too-large window
// still renders what fits.
static bool parseLayoutCell(const char** p, const char* end, int depth, TmuxPaneGeometry* outputBuffer, int* count, int maxPanes)
{
    if (depth > tmuxMaxLayoutDepth)
        return false;
    int w = parseDimension(p, end);
    if (w < 0 || *p >= end || **p != 'x')
        return false;
    (*p)++;
    int h = parseDimension(p, end);
    if (h < 0 || *p >= end || **p != ',')
        return false;
    (*p)++;
    int x = parseDimension(p, end);
    if (x < 0 || *p >= end || **p != ',')
        return false;
    (*p)++;
    int y = parseDimension(p, end);
    if (y < 0)
        return false;

    if (*p < end && **p == ',') {
        // Leaf: ",<paneID>".
        (*p)++;
        Ubiquitty::Protocol::Tmux::PaneID paneID = parseDimension(p, end);
        if (paneID < 0)
            return false;
        if (paneID <= maxTmuxPaneID && *count < maxPanes) {
            outputBuffer[*count] = TmuxPaneGeometry { paneID, x, y, w, h };
            (*count)++;
        }
        return true;
    }
    if (*p < end && (**p == '{' || **p == '[')) {
        const char close = (**p == '{') ? '}' : ']';
        (*p)++;
        for (;;) {
            if (!parseLayoutCell(p, end, depth + 1, outputBuffer, count, maxPanes))
                return false;
            if (*p < end && **p == ',') {
                (*p)++;
                continue;
            }
            if (*p < end && **p == close) {
                (*p)++;
                break;
            }
            return false; // unterminated / malformed container
        }
        return true;
    }
    return false; // a cell must be a leaf or a container
}

// Skip the 4-hex checksum + ',' then parse the root cell. Shared by the public
// parseLayout entry point and the %layout-change handler.
static int parseLayoutString(std::string_view layout, std::span<TmuxPaneGeometry> output)
{
    if (layout.empty() || output.empty())
        return -1;
    const char* p = layout.data();
    const char* end = layout.data() + layout.size();
    while (p < end && *p != ',') // checksum is hex up to the first ','
        p++;
    if (p >= end)
        return -1;
    p++; // skip ','
    int count = 0;
    if (!parseLayoutCell(&p, end, 0, output.data(), &count, static_cast<int>(output.size())))
        return -1;
    return count;
}

static void processLayoutChangeLine(TmuxClient* tc, std::string_view body)
{
    // "%layout-change @<windowID> <window-layout> [<visible-layout> <flags>]"
    const char* end = body.data() + body.size();
    const char* p = body.data();
    int windowID = parseWindowID(&p, end);
    if (windowID < 0 || !tc->events.layoutChange)
        return;
    if (p < end && *p == ' ')
        p++;
    const char* layoutStart = p;
    while (p < end && *p != ' ') // the layout field runs to the next space
        p++;
    // Hand the raw <window-layout> field to the frontend; it calls parseLayout()
    // to decode — the same entry point it uses for the initial list-windows layout.
    tc->events.layoutChange(tc->events.context, windowID, std::string_view(layoutStart, static_cast<std::size_t>(p - layoutStart)));
}

static void processLine(TmuxClient* tc, void* context, TmuxDeliverCallback deliver)
{
    std::string_view line { tc->line, static_cast<std::size_t>(tc->lineLength) };
    (void)context;
    (void)deliver;

    // Trim the trailing line terminator. processLine is invoked when
    // we see '\n', so the buffer always ends with '\n' (and tmux
    // uses CRLF, so usually '\r\n'). Strip both.
    if (line.ends_with('\n'))
        line.remove_suffix(1);
    if (line.ends_with('\r'))
        line.remove_suffix(1);

    if (tc->state == TmuxState::Response) {
        if (isResponseFrame(line, "%end ") || isResponseFrame(line, "%error ")) {
            if (tc->responseDepth > 0)
                tc->responseDepth--;
            if (tc->responseDepth == 0) {
                tc->state = TmuxState::Line;
                if (tc->events.commandResponseEnd)
                    tc->events.commandResponseEnd(tc->events.context);
            }
        } else if (isResponseFrame(line, "%begin ")) {
            tc->responseDepth++;
        } else if (tc->events.commandResponseLine) {
            // Body line of a command response — hand to the host;
            // the parser itself doesn't interpret response payloads.
            tc->events.commandResponseLine(tc->events.context, line);
        }
        resetLine(tc);
        return;
    }

    if (isResponseFrame(line, "%begin ")) {
        tc->state = TmuxState::Response;
        tc->responseDepth = 1;
    }
    // %output is handled by the streaming path in feedLineBuffered;
    // processLine never sees a complete %output line, so no branch
    // is needed here.
    else if (lineStartsWith(line, "%exit") || lineStartsWith(line, "%client-detached") || lineStartsWith(line, "%client-session-changed ")) {
        // tmux is detaching. A DCS-wrapped session emits %exit INSIDE the envelope
        // and still sends the closing `\e\\` ST; bouncing to PASSTHROUGH now would
        // leak the ESC + backslash to the VT100 parser, so stay in control mode and
        // let the dcsEnvelopeActive branch swallow the terminator. Non-DCS: PASSTHROUGH
        // immediately.
        tc->responseDepth = 0;
        tc->hasSignalledExit = true;
        if (tc->events.exit)
            tc->events.exit(tc->events.context);
        if (!tc->dcsEnvelopeActive) {
            tc->state = TmuxState::Passthrough;
        }
    } else if (lineStartsWith(line, "%session-changed ")) {
        processSessionChangedLine(tc, line.substr(17));
    } else if (lineStartsWith(line, "%session-renamed ")) {
        processSessionRenamedLine(tc, line.substr(17));
    } else if (lineStartsWith(line, "%window-pane-changed ")) {
        processPaneChangedLine(tc, line.substr(21));
    } else if (lineStartsWith(line, "%window-add ")) {
        processWindowAddLine(tc, line.substr(12));
    } else if (lineStartsWith(line, "%window-close ") || lineStartsWith(line, "%unlinked-window-close ")) {
        int off = lineStartsWith(line, "%window-close ") ? 14 : 23;
        processWindowCloseLine(tc, line.substr(static_cast<std::size_t>(off)));
    } else if (lineStartsWith(line, "%window-renamed ")) {
        processWindowRenamedLine(tc, line.substr(16));
    } else if (lineStartsWith(line, "%layout-change ")) {
        processLayoutChangeLine(tc, line.substr(15));
    } else if (lineStartsWith(line, "%pause ")) {
        processPausedLine(tc, line.substr(7));
    } else if (lineStartsWith(line, "%pane-mode-changed ")) {
        processPaneModeChangedLine(tc, line.substr(19));
    } else if (line.starts_with('%')) {
        // Other notifications we don't model yet
        // (%unlinked-window-add, %sessions-changed, ...), plus %continue — the
        // ack for the continuePane the host sends back from paneDidPause; output
        // just resumes, nothing to do. Silently consume.
    }
    // Non-`%` lines that aren't inside a response shouldn't normally
    // happen in well-behaved Control Mode, but tmux occasionally emits
    // blank lines as separators. Drop them.

    resetLine(tc);
}

// ---------- public API ----------

void init(TmuxClient* tc)
{
    if (!tc)
        return;
    *tc = TmuxClient {};
    tc->state = TmuxState::Passthrough;
    tc->activePaneID = 0;
    tc->atLineStart = true; // stream start counts as line start
}

void setEnterControlCallback(TmuxClient* tc, TmuxEnterControlHandler handler, void* context)
{
    if (!tc)
        return;
    tc->enterControl = handler;
    tc->enterControlContext = context;
}

void setEventClient(TmuxClient* tc, TmuxEventClient client)
{
    if (!tc)
        return;
    tc->events = std::move(client);
}

int parseLayout(std::string_view layout, std::span<TmuxPaneGeometry> outputBuffer) { return parseLayoutString(layout, outputBuffer); }

static void fireEnteredControlMode(TmuxClient* tc)
{
    if (tc->enterControl)
        tc->enterControl(tc->enterControlContext);
}

bool isActive(const TmuxClient* tc) { return tc && tc->state != TmuxState::Passthrough; }

void configure(TmuxClient* tc, IntegrationMode integration, AutoDetectMode autoDetectMode)
{
    if (!tc)
        return;
    const bool enabledFlag = (integration == IntegrationMode::Enabled);
    tc->enabled = enabledFlag;
    tc->autoDetect = (autoDetectMode == AutoDetectMode::Automatic);
    if (!enabledFlag) {
        tc->state = TmuxState::Passthrough;
        tc->responseDepth = 0;
        tc->dcsPrefixPosition = 0;
        tc->beginPrefixPosition = 0;
        tc->dcsEnvelopeActive = false;
        tc->dcsSawEscape = false;
        tc->atLineStart = true;
        resetLine(tc);
    }
}

void enterControlMode(TmuxClient* tc)
{
    if (!tc)
        return;
    if (tc->state == TmuxState::Passthrough) {
        tc->state = TmuxState::Line;
        tc->responseDepth = 0;
        resetLine(tc);
    }
}

// The 7-byte DCS introducer tmux uses to wrap a control-mode session.
// "ESC P 1 0 0 0 p" — we match this prefix byte-by-byte; on a
// mismatch the buffered prefix bytes are flushed verbatim back to
// deliver so they reach the VT100 parser as a normal escape sequence.
static constexpr char dCSIntro[] = "\033P1000p";
static constexpr int tmuxDCSIntroLength = sizeof(dCSIntro) - 1;

// The line-mode introducer for `tmux -C`/-CC's first response. Matching the bare `%`
// would be wildly false-positive in normal output (zsh prompts, percent encodings),
// so require the full 7-byte "%begin " prefix before flipping to control mode.
static constexpr char beginIntro[] = "%begin ";
static constexpr int tmuxBeginIntroLength = sizeof(beginIntro) - 1;

static void flushDCSPrefix(TmuxClient* tc, void* context, TmuxDeliverCallback deliver)
{
    for (int i = 0; i < tc->dcsPrefixPosition; i++) {
        uint8_t b = static_cast<uint8_t>(dCSIntro[i]);
        deliver(context, b);
        tc->atLineStart = (b == '\n');
    }
    tc->dcsPrefixPosition = 0;
}

static void flushBeginPrefix(TmuxClient* tc, void* context, TmuxDeliverCallback deliver)
{
    for (int i = 0; i < tc->beginPrefixPosition; i++) {
        uint8_t b = static_cast<uint8_t>(beginIntro[i]);
        deliver(context, b);
        tc->atLineStart = (b == '\n');
    }
    tc->beginPrefixPosition = 0;
}

// Passthrough: watch for `\eP1000p` (DCS) or `%begin ` (line) intros into control
// mode. Both are byte-level prefix matchers holding back at most tmuxDCSIntroLength
// bytes — buffering whole lines until '\n' would stall plain tmux output (its
// CR-only screen refreshes rarely end in '\n', so they'd render seconds late).
static void feedPassthrough(TmuxClient* tc, uint8_t c, void* context, TmuxDeliverCallback deliver)
{
    if (!(tc->enabled && tc->autoDetect)) {
        deliver(context, c);
        return;
    }
    // DCS introducer matcher first — `\eP1000p` only ever appears on
    // the wire from a tmux -CC server, no false positives from shell
    // output.
    if (tc->dcsPrefixPosition > 0) {
        if (static_cast<char>(c) == dCSIntro[tc->dcsPrefixPosition]) {
            tc->dcsPrefixPosition++;
            if (tc->dcsPrefixPosition == tmuxDCSIntroLength) {
                tc->dcsPrefixPosition = 0;
                tc->beginPrefixPosition = 0;
                tc->dcsEnvelopeActive = true;
                tc->dcsSawEscape = false;
                tc->hasSignalledExit = false;
                tc->state = TmuxState::Line;
                tc->responseDepth = 0;
                resetLine(tc);
                fireEnteredControlMode(tc);
            }
            return;
        }
        flushDCSPrefix(tc, context, deliver);
        // fall through to reprocess current byte
    }
    // `%begin ` line introducer matcher — only kicks in at the start
    // of a fresh line (or at stream start), so a literal `%` mid-line
    // is delivered immediately.
    if (tc->beginPrefixPosition > 0) {
        if (static_cast<char>(c) == beginIntro[tc->beginPrefixPosition]) {
            tc->beginPrefixPosition++;
            if (tc->beginPrefixPosition == tmuxBeginIntroLength) {
                // Full "%begin " matched. Switch to control
                // mode and replay the prefix into the line
                // buffer so processLine sees a complete
                // "%begin <args>\n" once \n arrives.
                tc->state = TmuxState::Line;
                tc->responseDepth = 0;
                tc->dcsEnvelopeActive = false;
                tc->hasSignalledExit = false;
                resetLine(tc);
                for (int i = 0; i < tmuxBeginIntroLength; i++) {
                    if (tc->lineLength < maxTmuxLineLength - 1)
                        tc->line[tc->lineLength++] = beginIntro[i];
                }
                tc->beginPrefixPosition = 0;
                fireEnteredControlMode(tc);
            }
            return;
        }
        flushBeginPrefix(tc, context, deliver);
        // fall through to reprocess current byte
    }
    // Start a fresh match. The DCS introducer is unambiguous and matches anywhere;
    // `%begin ` is gated on tc->atLineStart so content quoting the literal string
    // can't flip into control mode.
    if (c == 0x1B /*ESC*/) {
        tc->dcsPrefixPosition = 1;
        return;
    }
    if (c == '%' && tc->atLineStart) {
        tc->beginPrefixPosition = 1;
        return;
    }
    deliver(context, c);
    tc->atLineStart = (c == '\n');
}

// Ends the control session the envelope or the stream carried: back to
// passthrough, with its exit answered if no %exit answered it.
static void leaveControlMode(TmuxClient* tc)
{
    const bool shouldSignalExit = !tc->hasSignalledExit;
    tc->state = TmuxState::Passthrough;
    tc->dcsEnvelopeActive = false;
    tc->dcsSawEscape = false;
    tc->responseDepth = 0;
    tc->outputSubState = TmuxOutputSubState::None;
    tc->hasSignalledExit = false;
    resetLine(tc);
    if (shouldSignalExit && tc->events.exit)
        tc->events.exit(tc->events.context);
}

void endOfStream(TmuxClient* tc)
{
    if (!tc || tc->state == TmuxState::Passthrough)
        return;
    leaveControlMode(tc);
}

// DCS-wrapped control-mode: watch for the `\e\\` (ST) terminator that
// ends the wrap. When seen, drop back to passthrough; the next bytes
// go to the VT100 parser as normal terminal output. Returns true if
// the caller should `return` immediately (terminator consumed or ESC
// held back); false if processing should continue with the line/output
// paths below.
static bool feedDCSWrapWatch(TmuxClient* tc, uint8_t c)
{
    if (!tc->dcsEnvelopeActive)
        return false;
    if (tc->dcsSawEscape) {
        tc->dcsSawEscape = false;
        if (c == '\\') {
            leaveControlMode(tc);
            return true;
        }
        if (tc->lineLength < maxTmuxLineLength - 1)
            tc->line[tc->lineLength++] = '\033';
    }
    if (c == 0x1B /*ESC*/) {
        tc->dcsSawEscape = true;
        return true;
    }
    return false;
}

// "%output" body byte. Recognises CR/LF terminators and the leading
// '\\' that begins an escape sub-state, otherwise delivers the byte
// to the VT100 parser.
static void feedOutputBodyByte(TmuxClient* tc, uint8_t c, void* context, TmuxDeliverCallback deliver)
{
    if (c == '\r') {
        // CR of the line's CR-LF terminator — drop.
        return;
    }
    if (c == '\n') {
        tc->outputSubState = TmuxOutputSubState::None;
        resetLine(tc);
        return;
    }
    if (c == '\\') {
        tc->outputSubState = TmuxOutputSubState::Escape;
        return;
    }
    deliver(context, c);
}

// "%output" %-encoded byte streaming sub-state machine. Each branch
// shifts outputSubState to the next state; CR/LF/`\\` are handled by
// feedOutputBodyByte once we return to TmuxOutputSubState::Body.
static void feedOutputStream(TmuxClient* tc, uint8_t c, void* context, TmuxDeliverCallback deliver)
{
    switch (tc->outputSubState) {
    case TmuxOutputSubState::HeadPercent:
        if (c == '%') {
            tc->outputSubState = TmuxOutputSubState::HeadID;
        } else if (c == '\n') {
            tc->outputSubState = TmuxOutputSubState::None;
            resetLine(tc);
        }
        return;
    case TmuxOutputSubState::HeadID:
        if (c >= '0' && c <= '9') {
            tc->outPaneAccumulator = tc->outPaneAccumulator * 10 + (c - '0');
            if (tc->outPaneAccumulator > maxTmuxPaneID)
                tc->outPaneAccumulator = maxTmuxPaneID;
        } else if (c == ' ') {
            tc->activePaneID = tc->outPaneAccumulator;
            // %extended-output carries an "<age> [flags] :" header between the
            // pane id and the value; skip to the value. Plain %output goes
            // straight to the body.
            tc->outputSubState = tc->extendedOutput ? TmuxOutputSubState::ExtendedSkip : TmuxOutputSubState::Body;
        } else if (c == '\n') {
            tc->outputSubState = TmuxOutputSubState::None;
            resetLine(tc);
        }
        return;
    case TmuxOutputSubState::Body:
        feedOutputBodyByte(tc, c, context, deliver);
        return;
    case TmuxOutputSubState::Escape:
        if (c == '\\') {
            deliver(context, '\\');
            tc->outputSubState = TmuxOutputSubState::Body;
        } else if (c >= '0' && c <= '7') {
            tc->outOctalAccumulator = c - '0';
            tc->outputSubState = TmuxOutputSubState::Octal1;
        } else if (c == '\n') {
            deliver(context, '\\');
            tc->outputSubState = TmuxOutputSubState::None;
            resetLine(tc);
        } else {
            // Unknown escape — emit literally, treat current byte
            // as start of body again.
            deliver(context, '\\');
            deliver(context, c);
            tc->outputSubState = TmuxOutputSubState::Body;
        }
        return;
    case TmuxOutputSubState::Octal1:
        if (c >= '0' && c <= '7') {
            tc->outOctalAccumulator = (tc->outOctalAccumulator << 3) | (c - '0');
            tc->outputSubState = TmuxOutputSubState::Octal2;
        } else {
            // Malformed escape — emit what we have and resume.
            deliver(context, '\\');
            deliver(context, static_cast<uint8_t>('0' + (tc->outOctalAccumulator & 7)));
            tc->outputSubState = TmuxOutputSubState::Body;
            feedOutputBodyByte(tc, c, context, deliver);
        }
        return;
    case TmuxOutputSubState::Octal2:
        if (c >= '0' && c <= '7') {
            tc->outOctalAccumulator = (tc->outOctalAccumulator << 3) | (c - '0');
            deliver(context, static_cast<uint8_t>(tc->outOctalAccumulator));
            tc->outputSubState = TmuxOutputSubState::Body;
        } else {
            // Malformed third digit — emit accumulated bytes and resume.
            deliver(context, '\\');
            deliver(context, static_cast<uint8_t>('0' + ((tc->outOctalAccumulator >> 3) & 7)));
            deliver(context, static_cast<uint8_t>('0' + (tc->outOctalAccumulator & 7)));
            tc->outputSubState = TmuxOutputSubState::Body;
            feedOutputBodyByte(tc, c, context, deliver);
        }
        return;
    case TmuxOutputSubState::ExtendedSkip:
        // %extended-output: consume the "<age> [flags] " header up to the
        // ':' value delimiter (the age/flags are numeric, never ':').
        if (c == ':')
            tc->outputSubState = TmuxOutputSubState::ExtendedColon;
        else if (c == '\n') {
            tc->outputSubState = TmuxOutputSubState::None;
            resetLine(tc);
        }
        return;
    case TmuxOutputSubState::ExtendedColon:
        // The byte after ':' — tmux writes ": <value>", so skip exactly one
        // separator space; anything else is already the first value byte.
        tc->outputSubState = TmuxOutputSubState::Body;
        if (c == '\n') {
            tc->outputSubState = TmuxOutputSubState::None;
            resetLine(tc);
        } else if (c != ' ') {
            feedOutputBodyByte(tc, c, context, deliver);
        }
        return;
    default:
        tc->outputSubState = TmuxOutputSubState::None;
        return;
    }
}

// Buffer-based path for short control lines (%begin, %end, %window-*,
// etc.). Once the buffer matches "%output " we switch to streaming
// mode for the rest of the line.
static void feedLineBuffered(TmuxClient* tc, uint8_t c, void* context, TmuxDeliverCallback deliver)
{
    if (tc->lineLength < maxTmuxLineLength - 1) {
        tc->line[tc->lineLength++] = static_cast<char>(c);
    } else {
        // Line longer than the buffer (a wide, heavily-styled `capture-pane -e`
        // row can exceed it) and not %output. Don't DROP it — flush the truncated
        // content on the terminator so a command-response body keeps one entry per
        // row; dropping it would shift every following capture row up by one.
        if (c == '\n') {
            processLine(tc, context, deliver);
            resetLine(tc);
        }
        return;
    }
    // Detect %output prefix as soon as it's complete. We don't need
    // the line buffer's content for the rest of the line.
    if (tc->lineLength == 8 && std::memcmp(tc->line, "%output ", 8) == 0) {
        resetLine(tc);
        tc->outputSubState = TmuxOutputSubState::HeadPercent;
        tc->outPaneAccumulator = 0;
        tc->extendedOutput = false;
        return;
    }
    // "%extended-output %<pane> <age> [flags] : <value>" — the value is
    // escaped exactly like %output but is preceded by an "<age> [flags] :"
    // header. Without handling it the line falls to the catch-all and the
    // pane's output is silently dropped (tmux emits this under pause-after /
    // latency flags).
    if (tc->lineLength == 17 && std::memcmp(tc->line, "%extended-output ", 17) == 0) {
        resetLine(tc);
        tc->outputSubState = TmuxOutputSubState::HeadPercent;
        tc->outPaneAccumulator = 0;
        tc->extendedOutput = true;
        return;
    }
    if (c == '\n') {
        processLine(tc, context, deliver);
    }
}

void feedInput(TmuxClient* tc, uint8_t c, void* context, TmuxDeliverCallback deliver)
{
    if (!tc) {
        deliver(context, c);
        return;
    }

    if (tc->state == TmuxState::Passthrough) {
        feedPassthrough(tc, c, context, deliver);
        return;
    }

    // Control mode — line-buffer and dispatch. If we entered via DCS,
    // also watch for the `\e\\` (ST) terminator that ends the wrap.
    if (feedDCSWrapWatch(tc, c))
        return;

    // If we're streaming a %output payload, route the byte to the
    // decoder instead of the line buffer. Avoids the 2K cap on
    // buffered lines for full-pane redumps.
    if (tc->outputSubState != TmuxOutputSubState::None) {
        feedOutputStream(tc, c, context, deliver);
        return;
    }

    feedLineBuffered(tc, c, context, deliver);
}

void feedInputBytes(TmuxClient* tc, std::string_view bytes, void* context, TmuxDeliverCallback deliver, TmuxDeliverSpanCallback deliverSpan)
{
    if (bytes.empty())
        return;
    if (!tc || !deliverSpan) {
        for (std::size_t i = 0; i < bytes.size(); ++i)
            feedInput(tc, static_cast<uint8_t>(bytes[i]), context, deliver);
        return;
    }

    std::size_t i = 0;
    while (i < bytes.size()) {
        if (tc->state != TmuxState::Passthrough && tc->outputSubState == TmuxOutputSubState::Body) {
            const std::size_t start = i;
            while (i < bytes.size()) {
                const uint8_t c = static_cast<uint8_t>(bytes[i]);
                if (c == '\r' || c == '\n' || c == '\\')
                    break;
                ++i;
            }
            if (i > start) {
                deliverSpan(context, bytes.substr(start, i - start));
                continue;
            }
        }
        feedInput(tc, static_cast<uint8_t>(bytes[i]), context, deliver);
        ++i;
    }
}

// ---------- outbound key wrapping ----------

static int appendString(std::span<char> outputBuffer, int outputLength, const char* s)
{
    int schemeLength = static_cast<int>(std::strlen(s));
    if (static_cast<std::size_t>(outputLength + schemeLength) > outputBuffer.size())
        return -1;
    std::memcpy(outputBuffer.data() + outputLength, s, static_cast<size_t>(schemeLength));
    return outputLength + schemeLength;
}

static int appendByteHex(std::span<char> outputBuffer, int outputLength, uint8_t b)
{
    static const char hex[] = "0123456789abcdef";
    if (static_cast<std::size_t>(outputLength + 3) > outputBuffer.size())
        return -1;
    outputBuffer[outputLength++] = ' ';
    outputBuffer[outputLength++] = hex[(b >> 4) & 0xF];
    outputBuffer[outputLength++] = hex[b & 0xF];
    return outputLength;
}

int wrapKeyForPane(const TmuxClient* tc, PaneID paneID, std::string_view bytes, std::span<char> output)
{
    char* const outputBuffer = output.data();
    const int outputCapacity = static_cast<int>(output.size());
    int outputLength = 0;
    int written;
    char header[64];
    int i;

    if (bytes.empty() || output.empty())
        return 0;

    // Passthrough: tmux not active or not enabled, copy verbatim.
    if (!tc || tc->state == TmuxState::Passthrough) {
        if (bytes.size() > output.size())
            return 0;
        std::memcpy(output.data(), bytes.data(), bytes.size());
        return static_cast<int>(bytes.size());
    }

    // Control mode: emit `send-keys -t %<id> -H xx xx ...` followed by
    // newline. -H tells tmux to interpret each arg as a hex byte;
    // gives us full 0..0xFF coverage without quoting headaches. The
    // bytes are chunked so a long paste doesn't blow past tmux's
    // per-command argument list cap — sendKeysChunkBytes is what one
    // command carries.
    {
        int rc = std::snprintf(header, sizeof(header), "send-keys -t %%%d -H", paneID);
        if (rc < 0 || rc >= static_cast<int>(sizeof(header)))
            return 0;
    }

    i = 0;
    while (i < static_cast<int>(bytes.size())) {
        int chunk = static_cast<int>(bytes.size()) - i;
        if (chunk > sendKeysChunkBytes)
            chunk = sendKeysChunkBytes;
        written = appendString(output, outputLength, header);
        if (written < 0)
            return 0;
        outputLength = written;
        for (int b = 0; b < chunk; b++) {
            written = appendByteHex(output, outputLength, static_cast<uint8_t>(bytes[i + b]));
            if (written < 0)
                return 0;
            outputLength = written;
        }
        if (outputLength + 1 > outputCapacity)
            return 0;
        outputBuffer[outputLength++] = '\n';
        i += chunk;
    }
    return outputLength;
}

int wrapKey(const TmuxClient* tc, std::string_view bytes, std::span<char> output) { return wrapKeyForPane(tc, tc ? tc->activePaneID : 0, bytes, output); }

// ---- control-mode response-row parsing ----

bool scanUInt(std::string_view line, int& i, int& outValue)
{
    bool any = false;
    outValue = 0;
    while (i < static_cast<int>(line.size()) && line[i] >= '0' && line[i] <= '9') {
        if (outValue <= maxTmuxValue)
            outValue = outValue * 10 + (line[i] - '0');
        i++;
        any = true;
    }
    if (outValue > maxTmuxValue)
        outValue = maxTmuxValue;
    return any;
}

bool expectCharacter(std::string_view line, int& i, char c)
{
    if (i >= static_cast<int>(line.size()) || line[i] != c)
        return false;
    i++;
    return true;
}

bool parsePaneRecord(std::string_view line, int& windowID, PaneID& paneID, int& width, int& height, int& active, std::string& layout, std::string& session, std::string& name)
{
    int i = 0;
    windowID = paneID = width = height = 0;
    active = 1;
    layout.clear();
    session.clear();
    name.clear();
    if (!expectCharacter(line, i, '@') || !scanUInt(line, i, windowID))
        return false;
    if (!expectCharacter(line, i, ' ') || !expectCharacter(line, i, '%'))
        return false;
    if (!scanUInt(line, i, paneID))
        return false;
    if (!expectCharacter(line, i, ' ') || !scanUInt(line, i, width))
        return false;
    if (!expectCharacter(line, i, ' ') || !scanUInt(line, i, height))
        return false;
    if (i < static_cast<int>(line.size()) && line[i] == ' ') {
        i++;
        scanUInt(line, i, active);
    }
    // window_layout — a single space-free token (the tmux layout grammar is
    // `<csum>,<W>x<H>,<X>,<Y>` with `{}`/`[]`/`,`, never a space).
    if (i < static_cast<int>(line.size()) && line[i] == ' ') {
        i++;
        int start = i;
        while (i < static_cast<int>(line.size()) && line[i] != ' ')
            i++;
        layout = line.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(i - start));
    }
    if (i < static_cast<int>(line.size()) && line[i] == ' ') {
        i++;
        int start = i;
        while (i < static_cast<int>(line.size()) && line[i] != ' ')
            i++;
        session = line.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(i - start));
    }
    if (i < static_cast<int>(line.size()) && line[i] == ' ') {
        i++;
        name = line.substr(static_cast<std::size_t>(i));
    }
    return true;
}

} // namespace Ubiquitty::Protocol::Tmux
