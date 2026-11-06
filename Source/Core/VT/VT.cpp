// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Codec/Base64.h"
#include "Core/Cursor/RenderState.h"
#include "Core/Diagnostics/DiagLog.h"
#include "Core/Image/Sixel/Decoder.h"
#include "Core/Keys/Keymap.h"
#include "Core/LineStore.h"
#include "Core/Modes/DisplayIdle.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/ReplyBuilder.h"
#include "Core/RichText/Blocks.h"
#include "Core/SGR.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Margins.h"
#include "Core/Screen/Resize.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Screen/WidePair.h"
#include "Core/TerminalOperations.h"
#include "Core/Terminfo.h"
#include "Core/Triggers/Engine.h"
#include "Core/Unicode/Joining.h"
#include "Core/Unicode/Width.h"
#include "Core/VT/ControlStringScan.h"
#include "Core/VT/Internal.h"
#include "Core/VT/Parser.h"
#include "PAL/Capability.h"
#include "PAL/PAL.h"
#include "PAL/Stream.h"
#include "PAL/TaskQueue.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty {
namespace VT {

// VT/VT220-ish terminal emulator: the hot per-byte/per-cell core (parser
// DFA, dispatch, putCharacter/emitCharacter, cursor gridFloor, the CSI/ESC/DCS jump
// tables). The Pen struct is the single source of truth for SGR state, living
// on the screen ("current pen") and on each cell ("rendered pen").

using ParserState = Ubiquitty::VT::Parser::State;

enum class GraphicCapture : uint8_t {
    Retain,
    Discard
};

static inline int countByteRun(const char*, int, unsigned, unsigned);

// A complete successor makes the current graphic unavailable to CCH. An
// incomplete successor keeps the journal across the next input boundary.
//
// Out of line, so the per-cell callers carry only graphicCaptureBefore's
// checks.
[[gnu::noinline]] static GraphicCapture graphicCaptureBeforeAnySuccessor(const char* buffer, int next, int length);

// graphicCaptureBeforeAnySuccessor's answer, decided inline at the per-cell
// callers for the successors that follow a graphic most often: the end of the
// input, text, a line end, and a control sequence whose final byte lies in the
// word after its introducer. Every other successor takes the full
// classification.
template<GraphicCapture Capture> [[gnu::always_inline]] static inline GraphicCapture graphicCaptureBefore(const char* buffer, int next, int length)
{
    if constexpr (Capture == GraphicCapture::Discard)
        return GraphicCapture::Discard;
    if (next >= length)
        return GraphicCapture::Retain;
    const auto byte = static_cast<uint8_t>(buffer[next]);
    if ((byte >= 0x20 && byte < 0x7F) || byte == '\r' || byte == '\n')
        return GraphicCapture::Discard;
    if (byte == 0x1B && length - next >= 10 && buffer[next + 1] == '[') {
        // The full scan reads this same word first, so a byte that ends the
        // parameters inside it ends them there too.
        const int start = next + 2;
        const int final = start + countByteRun(buffer + start, 8, 0x20, 0x3F);
        if (final < start + 8) {
            const auto finalByte = static_cast<uint8_t>(buffer[final]);
            return finalByte >= 0x40 && finalByte <= 0x7E ? GraphicCapture::Discard : GraphicCapture::Retain;
        }
    }
    return graphicCaptureBeforeAnySuccessor(buffer, next, length);
}

class GraphicCaptureScope {
public:
    GraphicCaptureScope(Terminal& terminal, GraphicCapture capture)
        : m_terminal(terminal)
        , m_didBeginCapture(capture == GraphicCapture::Retain)
    {
        if (m_didBeginCapture)
            beginGraphicCancellation(terminal);
        else
            discardGraphicCancellation(terminal);
    }

    ~GraphicCaptureScope()
    {
        if (m_didBeginCapture && m_terminal.graphicCancellation)
            m_terminal.graphicCancellation->isCapturing = false;
    }

private:
    Terminal& m_terminal;
    bool m_didBeginCapture;
};

// ---------- pen helpers ----------

static void resetUTF8(Ubiquitty::Terminal& t) { Ubiquitty::Unicode::utf8Reset(&t.parser.utf8); }

// ---------- cell helpers ----------

// Pin Cell's size so a future field addition can't silently blow the per-Terminal
// memory budget; the OSC 8 link id reuses Cell's spare reserved bytes.
static_assert(sizeof(Ubiquitty::Cell) == 32, "sizeof(Cell) must stay 32 bytes");

// Erase using the screen's current SGR pen (preserves background color).

// Blank carrying the FULL current pen (BCE colors and attributes) for
// scrollRegion fills — distinct from eraseCell, which clears char attributes.

// ---------- dirty / screen lifecycle ----------

// markAllDirty's contract: every cell needs repaint and future writes shouldn't
// trust the shadow — requestFullRedraw yields every cell, invalidateShadow
// resets the comparison.

// Seed scroll-region geometry from the dimensions. Cell content lives
// in CellGrid (which Init/resize seeds blank).
void screenInit(Ubiquitty::Screen::State* s, int rows, int columns)
{
    *s = Ubiquitty::Screen::State {}; // value-init: Screen embeds Pen (non-trivial OptionSet fields), so memset is UB
    penReset(&s->pen);
    s->scrollTop = 0;
    s->scrollBottom = rows - 1;
    // Default left/right margins span the full width. DECSLRM only
    // narrows them when DECLRMM (DECSET 69) is set.
    s->scrollLeft = 0;
    s->scrollRight = columns - 1;
}

void screenClearDefault(Ubiquitty::Screen::State* s, int rows, int columns)
{
    // Cell content is cleared by mirrorClearActiveDisplay; here only cursor + wrap.
    (void)rows;
    (void)columns;
    s->cursorX = 0;
    s->cursorY = 0;
    s->wrapPending = false;
}

// Erase every active-CellGrid row with the canonical default-pen blank. Separate
// from screenClearDefault because that has only Ubiquitty::Screen::State*, which can't reach CellGrid.
void mirrorClearActiveDisplay(Ubiquitty::Terminal& t)
{
    if (Ubiquitty::Screen::hasActivePins(t)) [[unlikely]]
        Ubiquitty::Screen::notePositionsScreenReplaced(t, Ubiquitty::Screen::activeBuffer(t));
    for (int r = 0; r < t.rows; ++r) {
        t.grid->eraseRow(Ubiquitty::Row(r), Ubiquitty::defaultBlankCell);
        // A cleared row must lose its RowMeta too (line attribute, OSC 133 mark,
        // soft-wrap join, shaping gate), else Cmd-K leaves prompt brackets and
        // 2x scaling latched on now-blank rows.
        t.grid->resetRowMeta(r);
    }
}

// ---------- region scrolling ----------

// Scroll the active screen up by n as if n line feeds happened at the bottom
// (captures scrollback; slides landed images up on a full-screen scroll).
// DECSLRM helpers: the margin the cursor is constrained to. DECLRMM (DECSET 69)
// off → full screen regardless of scrollLeft/scrollRight; on → the stored values.
// The single point every horizontal-motion path routes through.

// A double-width line renders at 2x, so only its left half is visible: the
// cursor's visible right column is halved. Tab / cursor-tab clamp here so a DWL
// frame's right border lands at columns/2-1, not in the off-screen right half.
int effectiveRightVisible(const Ubiquitty::Terminal& t)
{
    const int right = Ubiquitty::Screen::effectiveRight(t);
    const Ubiquitty::Screen::State* s = t.screens.active;
    if (s->cursorY < 0 || s->cursorY >= t.rows || s->lineAttribute[s->cursorY] == lineAttributeNormal)
        return right;
    const int halfRight = t.columns / 2 - 1;
    return right < halfRight ? right : halfRight;
}

// ---------- cursor positioning ----------

// Full-screen clamp: ignores left/right margins. Used by absolute
// positioning operations (CUP/HVP/CHA/HPA) so they can land anywhere on the
// page regardless of DECLRMM.
void cursorClampFull(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (s->cursorX < 0)
        s->cursorX = 0;
    if (s->cursorX >= t.columns)
        s->cursorX = t.columns - 1;
    if (s->cursorY < 0)
        s->cursorY = 0;
    if (s->cursorY >= t.rows)
        s->cursorY = t.rows - 1;
    // On a double-width line the page ends at the line's middle.
    const int visibleRight = effectiveRightVisible(t);
    if (s->cursorX > visibleRight && s->lineAttribute[s->cursorY] != lineAttributeNormal)
        s->cursorX = visibleRight;
}

// Record a highlight-action match's span on the highlight ring. absoluteRow is the
// scrollback+live address (as Screen::ScrollSearch::Match) so the span tracks the row into
// history. Oldest-evicting fixed ring.
static void recordTriggerHighlight(Ubiquitty::Terminal& t, int absoluteRow, int column0, int column1)
{
    if (column1 < column0)
        return;
    const int cap = Ubiquitty::Terminal::triggerHighlightCapacity;
    int slot;
    if (t.triggerHighlightCount < cap) {
        slot = (t.triggerHighlightHead + t.triggerHighlightCount) % cap;
        t.triggerHighlightCount++;
    } else {
        // Ring full: overwrite the oldest and advance the head.
        slot = t.triggerHighlightHead;
        t.triggerHighlightHead = (t.triggerHighlightHead + 1) % cap;
    }
    t.triggerHighlights[slot].absoluteRow = absoluteRow;
    t.triggerHighlights[slot].column0 = column0;
    t.triggerHighlights[slot].column1 = column1;
}

// Password input mode. Sets the flag and signals the frontend (lock indicator +
// local-echo masking) only on a real transition, so a repeated set/clear doesn't
// spam the client.
void setPasswordPromptActive(Ubiquitty::Terminal& t, Ubiquitty::Modes::PasswordPrompt prompt)
{
    const bool isActive = (prompt == Ubiquitty::Modes::PasswordPrompt::Active);
    if (t.passwordPromptActive == isActive)
        return;
    t.passwordPromptActive = isActive;
    // The host's verdict drives both local presentation and the replicated flag.
    if (auto* c = Ubiquitty::clientForEffectThatEscapesNothing(t.windowClient))
        c->setPasswordPromptActive(isActive);
}

// Lower-case ASCII helper (locale-free; the heuristic only cares about ASCII).
static inline char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Password-prompt heuristic: true when `line` contains "password"/"passphrase"/
// "passcode" (case-insensitive) and ends (ignoring trailing spaces) with ':'.
// Conservative — a line merely mentioning "password" without a colon-prompt
// doesn't trigger, so log lines don't flip it.
static bool lineLooksLikePasswordPrompt(std::string_view line)
{
    // The colon must be the last thing on the row, trailing blanks aside: a
    // colon anywhere else is prose, not a prompt.
    const std::size_t last = line.find_last_not_of(' ');
    if (last == std::string_view::npos || line[last] != ':')
        return false;
    const std::string_view prompt = line.substr(0, last + 1);
    static constexpr auto keywords = std::to_array<std::string_view>({ "password", "passphrase", "passcode" });
    return std::ranges::any_of(keywords, [prompt](std::string_view keyword) {
        return !std::ranges::search(prompt, keyword, {}, asciiLower).empty();
    });
}

// Only the parser owner infers a password prompt. Canonical input makes echo
// authoritative; raw or unavailable discipline leaves the cursor-row heuristic
// useful for an SSH peer or a line editor. Facts are sampled once per batch.
static void evaluatePasswordPromptAtCursor(Ubiquitty::Terminal& t, PAL::ProcessFacts* process)
{
    // An application's mode-7401 assertion is its own; the heuristic only
    // decides while no assertion stands.
    if (t.replyAuthority != ReplyAuthority::ThisTerminal || !t.passwordDetect || !t.grid || t.isPasswordPromptAsserted)
        return;
    const auto discipline = process ? process->lineDiscipline() : std::nullopt;
    if (discipline && discipline->isCanonical) {
        setPasswordPromptActive(t, discipline->isEchoEnabled ? Ubiquitty::Modes::PasswordPrompt::Inactive : Ubiquitty::Modes::PasswordPrompt::Active);
        return;
    }
    Ubiquitty::Screen::State* s = t.screens.active;
    const int row = s->cursorY;
    if (row < 0 || row >= t.rows)
        return;
    // Assemble printable ASCII up to the cursor column (the prompt text precedes
    // the cursor where typed input appears), the same byte-column convention
    // the trigger / search paths use.
    char buffer[Ubiquitty::maxColumns];
    int n = s->cursorX;
    if (n > t.columns)
        n = t.columns;
    if (n > Ubiquitty::maxColumns)
        n = Ubiquitty::maxColumns;
    int length = 0;
    for (int c = 0; c < n; ++c) {
        const auto cell = t.grid->logicalCellValueAt(Ubiquitty::Row(row), Ubiquitty::Column(c));
        uint16_t ch = cell && !Ubiquitty::isTextContinuation(*cell) ? static_cast<uint16_t>(cell->codePoint) : ' ';
        buffer[c] = (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : ' ';
        if (ch != 0 && ch != ' ')
            length = c + 1;
    }
    setPasswordPromptActive(t, lineLooksLikePasswordPrompt({ buffer, static_cast<std::size_t>(length) }) ? Ubiquitty::Modes::PasswordPrompt::Active : Ubiquitty::Modes::PasswordPrompt::Inactive);
}

// Triggers line-commit hook. Evaluate the just-completed live row
// `screenRow` against the configured trigger rules and dispatch each match to
// the TriggerClient. Called from linefeed as the cursor leaves the row, and
// from the autowrap fill point in emitCharacter (so a full-width line that never
// sees an explicit LF is matched too).
//
// HOT-PATH GATE: the inline evaluateRowTriggers wrapper below early-returns
// when no triggers are configured (the overwhelming default), so the byte-hot
// line-feed path pays two loads and a compare. noinline keeps the maxColumns
// stack buffer, its stack check, and the assemble/match machinery out of the
// per-line callers' bodies and frames.
[[gnu::noinline]] static void evaluateRowTriggersMatch(Ubiquitty::Terminal& t, int screenRow)
{
    if (!t.grid || screenRow < 0 || screenRow >= t.rows)
        return;

    // Assemble the row's printable bytes exactly as ScrollSearch does: one
    // ASCII byte per cell, non-printables substituted with a space (the engine
    // matches in byte-column space, the same convention its docs name).
    char buffer[Ubiquitty::maxColumns];
    int n = t.columns;
    if (n > Ubiquitty::maxColumns)
        n = Ubiquitty::maxColumns;
    int length = 0;
    for (int c = 0; c < n; ++c) {
        const Ubiquitty::Cell* cell = t.grid->cellAt(Ubiquitty::Row(screenRow), Ubiquitty::Column(c));
        uint16_t ch = cell && !Ubiquitty::isTextContinuation(*cell) ? static_cast<uint16_t>(cell->codePoint) : ' ';
        buffer[c] = (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : ' ';
        if (ch != 0)
            length = c + 1; // trim trailing blanks past the last content cell
    }
    if (length == 0)
        return;

    Ubiquitty::Triggers::Matched matched[Ubiquitty::Triggers::maxMatched];
    const int count = t.triggersEngine->evaluateLine({ buffer, static_cast<std::size_t>(length) }, { matched, Ubiquitty::Triggers::maxMatched });
    if (count <= 0)
        return;
    // Absolute address (stable as the row scrolls into history, so a recorded
    // highlight tracks the content).
    const int absoluteRow = t.scrollbackCount + screenRow;
    for (int i = 0; i < count; ++i) {
        const Ubiquitty::Triggers::Matched& m = matched[i];
        const int c0 = m.column0;
        const int c1 = (m.column1 < length) ? m.column1 : length - 1;
        const int span = (c1 >= c0) ? (c1 - c0 + 1) : 0;
        // Highlight is painted from the recorded span, so record it regardless of
        // a TriggerClient (the paint pass reads the ring directly).
        if (m.action == Ubiquitty::Triggers::Action::Highlight)
            recordTriggerHighlight(t, absoluteRow, c0, c1);
        if (!t.triggerClient)
            continue;
        // ExecuteTrigger: a RunCommand trigger is arbitrary host exec — an
        // installed policy must allow it (deny drops just this run). Unset ⇒ PAL's default.
        if (m.action == Ubiquitty::Triggers::Action::RunCommand) {
            if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::ExecuteTrigger, m.arg ? std::string_view(m.arg) : std::string_view()))
                continue; // host capability policy denied the command run
        }
        t.triggerClient->onTrigger(static_cast<uint8_t>(m.action), m.arg ? std::string_view(m.arg) : std::string_view(), std::string_view(buffer + c0, static_cast<std::size_t>(span)), m.column0, m.column1);
    }
}

// The gate every hot caller inlines: two loads and a compare in the
// overwhelming no-triggers default; only a configured session calls out.
static inline void evaluateRowTriggers(Ubiquitty::Terminal& t, int screenRow)
{
    if (!t.triggersEngine || t.triggersEngine->ruleCount() == 0)
        return; // no triggers configured — byte-hot path stays cheap
    evaluateRowTriggersMatch(t, screenRow);
}

// Autowrap fill hook: a full-width tail line won't commit (and so wouldn't match)
// until the next char, which may never come. Evaluate the now-complete row here
// and record triggerWrapEvaluatedRow so the later wrap-commit linefeed doesn't
// match it twice. Null-gated on the engine.
static inline void evaluateFilledRowTriggers(Ubiquitty::Terminal* t, int screenRow)
{
    if (!t || !t->triggersEngine || t->triggersEngine->ruleCount() == 0)
        return; // no triggers configured — byte-hot fill path stays untouched
    evaluateRowTriggersMatch(*t, screenRow);
    t->triggerWrapEvaluatedRow = screenRow;
}

// printSinkWriteRow (DEC MC print-sink row dump) is declared in
// VT/Internal.h; the body lives in VT/CSI.cpp with the media-copy
// cluster. linefeed's auto-print path reaches it across the cut.

void linefeed(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    // Triggers: the row the cursor leaves is a completed line — match it before
    // the LF. Autowrap dedupe: a full-width line already evaluated at fill (which
    // set triggerWrapEvaluatedRow) must not match again on the following LF;
    // emitCharacter clears the guard on any rewrite, so only an UNCHANGED full row is skipped.
    if (s->cursorY != t.triggerWrapEvaluatedRow)
        evaluateRowTriggers(t, s->cursorY);
    t.triggerWrapEvaluatedRow = -1; // consume the guard; the row is leaving now
    // Auto-print (DEC MC ? 5 i): copy the completed line to the print sink before
    // the feed advances. No-op without a sink.
    if (t.autoPrint && t.printSinkFile)
        printSinkWriteRow(t, s->cursorY);
    s->wrapPending = false;
    if (s->cursorY == s->scrollBottom)
        Ubiquitty::Screen::scrollUp(t, 1);
    else if (s->cursorY < t.rows - 1)
        s->cursorY++;
}

// Wrap-commit line feed: the autowrap sites in emitCharacter JOIN an already-open
// run rather than interleaving classic per-line scrolls into it. A wrap never
// OPENS a run: a lone wrap in prompt-shaped output would pay a run open and an
// immediate flush for one line, so runs open at explicit LFs. An open run
// implies lane eligibility (every invalidator closes it), so no re-check here.
static inline void wrapCommitLinefeed(Ubiquitty::Terminal& t)
{
    if ((t.graphicCancellation && t.graphicCancellation->isCapturing))
        cancellableWrapLinefeed(t);
    else if (t.scrollLaneRun.active)
        Ubiquitty::Screen::scrollLaneLinefeed(t);
    else
        linefeed(t);
    // The wrapped part of a string is a string of its own on the new row.
    Ubiquitty::Screen::noteStringStart(t);
}

} // namespace VT

void termFlushScrollLane(Ubiquitty::Terminal& t) { Ubiquitty::Screen::scrollLaneFlush(t); }

namespace VT {

void reverseIndex(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    s->wrapPending = false;
    if (s->cursorY == s->scrollTop)
        Ubiquitty::Screen::scrollDown(t, 1);
    else if (s->cursorY > 0)
        s->cursorY--;
}

// ---------- character placement ----------

// Idempotency probe: true when a cell already holds this pen + attributes, so a
// same-char-same-pen write can skip the dirty-row mark (curses redraws the same
// content constantly). Every pen byte a renderer reads is compared: a field
// left out here is a field a reprint cannot refresh, and the skip leaves the
// FIRST pen's styling under the second print with nothing reporting it. The
// RGB channels are the one exception — meaningful only under their colorRGB
// index, and stale channel bytes under an indexed pen must not force a
// rewrite of a cell that renders identically.
static bool cellMatchesPen(const Ubiquitty::Cell* cell, const Pen* pen, VT::AttributeSet attributes)
{
    const Pen& cellPenValue = Ubiquitty::cellPen(*cell);
    return (cellPenValue.attributes == attributes) && (cellPenValue.foregroundIndex == pen->foregroundIndex) && (cellPenValue.backgroundIndex == pen->backgroundIndex)
        && (cellPenValue.underlineIndex == pen->underlineIndex)
        && (cellPenValue.semanticRole == pen->semanticRole)
        && (cellPenValue.isForegroundAssignedDefault == pen->isForegroundAssignedDefault) && (cellPenValue.isBackgroundAssignedDefault == pen->isBackgroundAssignedDefault)
        && (cellPenValue.underlineStyle == pen->underlineStyle)
        && (cellPenValue.extendedAttributes.toRaw() == pen->extendedAttributes.toRaw())
        && (cellPenValue.font == pen->font)
        && ((pen->foregroundIndex != Ubiquitty::colorRGB) || (cellPenValue.foregroundRed == pen->foregroundRed && cellPenValue.foregroundGreen == pen->foregroundGreen && cellPenValue.foregroundBlue == pen->foregroundBlue))
        && ((pen->backgroundIndex != Ubiquitty::colorRGB) || (cellPenValue.backgroundRed == pen->backgroundRed && cellPenValue.backgroundGreen == pen->backgroundGreen && cellPenValue.backgroundBlue == pen->backgroundBlue))
        && ((pen->underlineIndex != Ubiquitty::colorRGB) || (cellPenValue.underlineRed == pen->underlineRed && cellPenValue.underlineGreen == pen->underlineGreen && cellPenValue.underlineBlue == pen->underlineBlue));
}

// Build a Cell with the given pen + attributes and write it to CellGrid — the
// single point of cell mutation in VT.cpp (the parser keeps no cell store).
static inline void displaySetCell(Ubiquitty::Terminal& t, int row, int column, const Pen* pen, uint32_t ch, VT::AttributeSet attributes)
{
    if (!t.grid)
        return;
    Ubiquitty::Cell cell;
    cell.codePoint = ch;
    Pen cellPenValue = *pen;
    cellPenValue.attributes = attributes;
    Ubiquitty::cellSetPen(cell, cellPenValue);
    Ubiquitty::Screen::clearCluster(&cell);
    cell.setLinkID(t.currentLinkID);
    // Both callers reach here only inside their `!same` idempotency branch,
    // so the new value is known to differ — skip setCell's redundant memcmp.
    t.grid->setCellKnownDiff(Ubiquitty::Row(row), Ubiquitty::Column(column), cell);
}

// Ubiquitty::Screen::insertCharacters (IRM path, used by emitCharacter) is declared in VT/Internal.h;
// the body is in Screen/Edit.cpp.

// Forget any open grapheme cluster. A subsequent extending code point
// then starts a fresh cluster instead of attaching to a stale cell.
static inline void clusterReset(Ubiquitty::Terminal& t) { t.parser.clusterActive = false; }

// Snapshot the just-placed cluster's lead cell + the cursor state
// emitCharacter leaves behind, so the next emitCharacter can tell whether the
// cluster is still live (cursor unmoved) and where to append.
static inline void clusterOpen(Ubiquitty::Terminal& t, int row, int column, uint32_t base)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    t.parser.clusterActive = true;
    t.parser.clusterRow = row;
    t.parser.clusterColumn = column;
    t.parser.clusterScreen = t.screens.isUsingAlternateScreen ? 1u : 0u;
    t.parser.clusterLast = base;
    Ubiquitty::Unicode::graphemeStateReset(&t.parser.clusterState, base);
    t.parser.clusterAtX = s->cursorX;
    t.parser.clusterAtY = s->cursorY;
    t.parser.clusterAtWrap = s->wrapPending;
    t.parser.clusterCodePoints[0] = base;
    t.parser.clusterCodepointLength = 1;
}

static inline void clusterOpenWithInlineContinuation(Ubiquitty::Terminal& t, int row, int column, uint32_t base, uint32_t continuation)
{
    clusterOpen(t, row, column, base);
    (void)Ubiquitty::Unicode::graphemeExtends(&t.parser.clusterState, base, continuation);
    t.parser.clusterLast = continuation;
    t.parser.clusterCodePoints[1] = continuation;
    t.parser.clusterCodepointLength = 2;
}

// Intern the Terminal's running open-cluster sequence into its ClusterPool and
// return the 1-based id to stamp on the lead cell. A thin helper over
// ClusterPool::intern so the append sites read cleanly.
static inline uint16_t internOpenCluster(Ubiquitty::Terminal& t)
{
    return t.clusterPool.intern({ t.parser.clusterCodePoints.data(), static_cast<size_t>(t.parser.clusterCodepointLength) });
}

// Re-interns the open cluster after it grew, for a lead cell that holds
// `heldID`. A full pool refuses a sequence it doesn't hold, and a cell that
// already shows a pooled cluster keeps it: the code point is dropped, as one
// past the length limit is, instead of the cell falling back to its base and
// inline mark and losing the text it showed.
static inline uint16_t internGrownCluster(Ubiquitty::Terminal& t, uint16_t heldID)
{
    const uint16_t id = internOpenCluster(t);
    return id != 0 ? id : heldID;
}

// True when the open cluster is still the cell under the cursor: same
// screen, and the cursor + wrapPending match the snapshot emitCharacter left.
// Any CR / LF / cursor-positioning between calls perturbs the snapshot,
// so a combining mark cannot retro-attach across a cursor move.
static bool clusterIsLive(const Ubiquitty::Terminal& t)
{
    if (!t.parser.clusterActive)
        return false;
    if (t.parser.clusterScreen != (t.screens.isUsingAlternateScreen ? 1u : 0u))
        return false;
    const Ubiquitty::Screen::State* s = t.screens.active;
    return s->cursorX == t.parser.clusterAtX && s->cursorY == t.parser.clusterAtY && s->wrapPending == t.parser.clusterAtWrap;
}

// U+20E3 COMBINING ENCLOSING KEYCAP — the mark that, after a keycap base
// (digit / '#' / '*') and an optional VS16, completes an emoji keycap.
static constexpr uint32_t combiningEnclosingKeycap = 0x20E3;

// A retroactive width upgrade leaves the same cursor as placing the wide
// glyph initially: on its lead at the margin, otherwise after both cells.
// Derive this from the cluster's column, not the already-advanced narrow cursor.
static void advanceWidenedCluster(Ubiquitty::Terminal& t)
{
    auto* screen = t.screens.active;
    if (t.shouldWriteRightToLeft) {
        const int left = std::max(Screen::effectiveLeft(t), Screen::lineHomeColumn(t));
        screen->cursorX = std::max(left, t.parser.clusterColumn - 1);
        screen->wrapPending = t.parser.clusterColumn <= left;
    } else if (t.parser.clusterColumn + 1 >= effectiveRightVisible(t)) {
        screen->cursorX = t.parser.clusterColumn;
        screen->wrapPending = true;
    } else {
        screen->cursorX = t.parser.clusterColumn + 2;
    }
    t.parser.clusterAtX = screen->cursorX;
    t.parser.clusterAtWrap = screen->wrapPending;
}

static void writeClusterBands(Ubiquitty::Terminal& terminal, const Ubiquitty::Cell& anchor)
{
    const unsigned specID = anchor.pen.extendedAttributes.textSizeSpecID();
    const auto* spec = specID ? terminal.textSizeTable.lookup(specID) : nullptr;
    if (!spec || Ubiquitty::penTextBand(&anchor.pen) != 0)
        return;
    const int row = terminal.parser.clusterRow;
    const int column = terminal.parser.clusterColumn;
    for (int band = 1; band < spec->scale && row + band < terminal.rows; ++band) {
        const auto* current = terminal.grid->logicalCellAt(Ubiquitty::Row(row + band), Ubiquitty::Column(column));
        if (!current || current->pen.extendedAttributes.textSizeSpecID() != specID
            || Ubiquitty::penTextBand(&current->pen) != band)
            break;
        auto next = anchor;
        Ubiquitty::penSetTextBand(&next.pen, static_cast<std::uint8_t>(band));
        captureGraphicCells(terminal, row + band, column, column);
        terminal.grid->setCell(Ubiquitty::Row(row + band), Ubiquitty::Column(column), next);
    }
}

// Append `codePoint` to the open cluster's lead cell. Continuation codepoints
// fill the cell's inline `combining` buffer first; on overflow (long ZWJ
// families) the whole sequence moves to the cluster pool and the cell carries a
// pool id in Cell::clusterID. VS16 (U+FE0F) upgrades the lead cell to emoji
// presentation (widened to a two-cell pair if narrow); VS15 (U+FE0E) forces text.
// A keycap base (digit / '#' / '*') gets the same upgrade only when U+20E3 closes
// the sequence (its base isn't Extended_Pictographic, so VS16 alone leaves it untagged).
static void clusterAppend(Ubiquitty::Terminal& t, uint32_t codePoint)
{
    if (!t.grid)
        return;
    // logicalCellAt: the lead cell of an open cluster sits below a live
    // deferral's start (this same run wrote it), so the value is real bytes and
    // no fill is needed. Materialising here would blank the rest of a row that
    // the edge lanes left deferred for the next write to consume.
    const Ubiquitty::Cell* current = t.grid->logicalCellAt(Ubiquitty::Row(t.parser.clusterRow), Ubiquitty::Column(t.parser.clusterColumn));
    if (!current) {
        t.parser.clusterLast = codePoint;
        return;
    }
    Ubiquitty::Cell next = *current;
    // Read the lead pen once; the edits below mutate this local and it is written
    // back to the cell in one store.
    Pen nextPen = Ubiquitty::cellPen(next);
    const unsigned specID = nextPen.extendedAttributes.textSizeSpecID();
    const auto* sizeSpec = specID ? t.textSizeTable.lookup(specID) : nullptr;
    const bool hasExplicitWidth = sizeSpec && sizeSpec->widthCells != 0;

    // Track the full running sequence so an overflow can intern the complete
    // cluster (the inline buffer alone would be lossy).
    if (t.parser.clusterCodepointLength < Ubiquitty::clusterPoolMaximumCodePoints) {
        t.parser.clusterCodePoints[t.parser.clusterCodepointLength] = codePoint;
        t.parser.clusterCodepointLength++;
    }

    if (sizeSpec && sizeSpec->isTextRun) {
        if (const auto textID = Screen::appendRunText(t, next.clusterID(), { &codePoint, 1 })) {
            next.setClusterID(textID);
            t.grid->setCell(Row(t.parser.clusterRow), Column(t.parser.clusterColumn), next);
            writeClusterBands(t, next);
        }
        t.parser.clusterLast = codePoint;
        return;
    }

    // VS overrides presentation only for an Extended_Pictographic base (a VS after
    // a letter/digit must not retag). SGR 76 (ForceEmojiPresentation) pins the run.
    bool isPictographicBase = Ubiquitty::Unicode::isExtendedPictographic(t.parser.clusterCodePoints[0]) && !nextPen.attributes.contains(VT::Attribute::ForceEmojiPresentation);
    // A keycap base (digit / '#' / '*') becomes a wide emoji only when U+20E3
    // closes the sequence; `1` or `1`+VS16 alone stays a plain digit.
    bool isKeycapComplete = Ubiquitty::Unicode::isKeycapBase(t.parser.clusterCodePoints[0]) && codePoint == combiningEnclosingKeycap && !nextPen.attributes.contains(VT::Attribute::ForceEmojiPresentation);
    bool isEmojiUpgrade = (isPictographicBase && codePoint == Ubiquitty::Unicode::Width::variationSelector16) || isKeycapComplete;
    if (isEmojiUpgrade) {
        nextPen.attributes.add(VT::Attribute::Emoji);
        // Claim the adjacent cell in the writing direction. A wide glyph's
        // lead is always its leftmost cell, even when writing right-to-left.
        Ubiquitty::Screen::State* s = t.screens.active;
        if (!hasExplicitWidth && !nextPen.attributes.contains(VT::Attribute::Wide) && (t.shouldWriteRightToLeft ? t.parser.clusterColumn > std::max(Screen::effectiveLeft(t), Screen::lineHomeColumn(t)) : t.parser.clusterColumn + 1 < t.columns)) {
            nextPen.attributes.add(VT::Attribute::Wide);
            Ubiquitty::Cell continuation;
            continuation.codePoint = Ubiquitty::wideContinuationSentinelCodePoint;
            Pen continuationPen = nextPen;
            continuationPen.attributes.remove({ VT::Attribute::Wide, VT::Attribute::Emoji });
            continuationPen.attributes.add(VT::Attribute::WideContinuation);
            Ubiquitty::cellSetPen(continuation, continuationPen);
            Ubiquitty::Screen::clearCluster(&continuation);
            continuation.setLinkID(next.linkID());
            Ubiquitty::Cell blank;
            Ubiquitty::Screen::eraseCell(&blank, s);
            // Clear any glyph (and its wide partner) sitting in the claimed
            // continuation column, then stamp the continuation half. The
            // lead cell itself is written once below, after the inline /
            // pool update.
            if (t.shouldWriteRightToLeft) {
                --t.parser.clusterColumn;
                t.grid->clearWideOverlap(Ubiquitty::Row(t.parser.clusterRow), Ubiquitty::Column(t.parser.clusterColumn), Ubiquitty::Column(t.parser.clusterColumn), blank);
            } else {
                t.grid->clearWideOverlap(Ubiquitty::Row(t.parser.clusterRow), Ubiquitty::Column(t.parser.clusterColumn + 1), Ubiquitty::Column(t.parser.clusterColumn + 1), blank);
            }
            t.grid->setCell(Ubiquitty::Row(t.parser.clusterRow), Ubiquitty::Column(t.parser.clusterColumn + 1), continuation);
            advanceWidenedCluster(t);
        }
    } else if (isPictographicBase && codePoint == Ubiquitty::Unicode::Width::variationSelector15) {
        nextPen.attributes.remove(VT::Attribute::Emoji);
    }

    if (next.clusterID() != 0 || next.combiningLength() >= Ubiquitty::cellMaxCombining) {
        // Overflowed or the inline buffer just filled: (re-)intern the full
        // sequence. intern() dedups by content, so an unchanged cluster keeps its
        // id and the pool keeps the shorter prefix (never evicts a live id).
        next.setClusterID(internGrownCluster(t, next.clusterID()));
    } else {
        next.combining[next.combiningLength()] = codePoint;
        next.setCombiningLength(static_cast<uint8_t>(next.combiningLength() + 1));
    }

    Ubiquitty::cellSetPen(next, nextPen);
    t.grid->setCell(Ubiquitty::Row(t.parser.clusterRow), Ubiquitty::Column(t.parser.clusterColumn), next);
    if (specID) [[unlikely]]
        writeClusterBands(t, next);
    t.parser.clusterLast = codePoint;
}

// Batched clusterAppend: attach `count` continuation codepoints to the open
// cluster's lead cell in one CellGrid read-modify-write. Equivalent — in final
// cell, pool, cursor, and snapshot state — to calling clusterAppend once per
// code point in order; emitClusterRun's ZWJ-sequence scan lands here so a
// seven-code point emoji family pays one cellAt + setCell round trip instead
// of six (and probes the base's pictographic/keycap class once, not per
// mark). The per-code point presentation effects (VS16 emoji upgrade with the
// wide-cell claim, VS15 text downgrade, keycap completion) still apply in
// wire order against the in-flight cell.
static void clusterAppendRun(Ubiquitty::Terminal& t, std::span<const uint32_t> run)
{
    const uint32_t* const codePoints = run.data();
    const int count = static_cast<int>(run.size());
    if (!t.grid || count <= 0)
        return;
    // logicalCellAt: the lead cell of an open cluster sits below a live
    // deferral's start (this same run wrote it), so the value is real bytes and
    // no fill is needed. Materialising here would blank the rest of a row that
    // the edge lanes left deferred for the next write to consume.
    const Ubiquitty::Cell* current = t.grid->logicalCellAt(Ubiquitty::Row(t.parser.clusterRow), Ubiquitty::Column(t.parser.clusterColumn));
    if (!current) {
        t.parser.clusterLast = codePoints[count - 1];
        return;
    }
    if (isRunTextAnchor(&t, *current)) {
        for (const auto codePoint : run)
            clusterAppend(t, codePoint);
        return;
    }
    Ubiquitty::Cell next = *current;
    // Resolve the lead pen once for the whole batch; presentation edits
    // mutate this local and re-intern at write-back (pool entries are
    // immutable).
    Pen nextPen = Ubiquitty::cellPen(next);
    const unsigned specID = nextPen.extendedAttributes.textSizeSpecID();
    const auto* sizeSpec = specID ? t.textSizeTable.lookup(specID) : nullptr;
    const bool hasExplicitWidth = sizeSpec && sizeSpec->widthCells != 0;

    // SGR 76 forcing on the lead cell pins presentation: a VS / keycap arriving
    // as a later grapheme-extension byte must not flip a forced run.
    const bool isPresentationForced = nextPen.attributes.contains(VT::Attribute::ForceEmojiPresentation);
    const bool isPictographicBase = Ubiquitty::Unicode::isExtendedPictographic(t.parser.clusterCodePoints[0]) && !isPresentationForced;
    const bool isKeycapBase = Ubiquitty::Unicode::isKeycapBase(t.parser.clusterCodePoints[0]) && !isPresentationForced;
    bool didInlineOverflow = false;
    for (int index = 0; index < count; ++index) {
        const uint32_t codePoint = codePoints[index];
        if (t.parser.clusterCodepointLength < Ubiquitty::clusterPoolMaximumCodePoints) {
            t.parser.clusterCodePoints[t.parser.clusterCodepointLength] = codePoint;
            t.parser.clusterCodepointLength++;
        }
        const bool isKeycapComplete = isKeycapBase && codePoint == combiningEnclosingKeycap;
        const bool isEmojiUpgrade = (isPictographicBase && codePoint == Ubiquitty::Unicode::Width::variationSelector16) || isKeycapComplete;
        if (isEmojiUpgrade) {
            nextPen.attributes.add(VT::Attribute::Emoji);
            // Widen a narrow base to the two-cell emoji span (clusterAppend's
            // upgrade): claim the cell to the right as the continuation half and
            // step the cursor + snapshot past it.
            Ubiquitty::Screen::State* s = t.screens.active;
            if (!hasExplicitWidth && !nextPen.attributes.contains(VT::Attribute::Wide) && t.parser.clusterColumn + 1 < t.columns) {
                nextPen.attributes.add(VT::Attribute::Wide);
                Ubiquitty::Cell continuation;
                continuation.codePoint = Ubiquitty::wideContinuationSentinelCodePoint;
                Pen continuationPen = nextPen;
                continuationPen.attributes.remove({ VT::Attribute::Wide, VT::Attribute::Emoji });
                continuationPen.attributes.add(VT::Attribute::WideContinuation);
                Ubiquitty::cellSetPen(continuation, continuationPen);
                Ubiquitty::Screen::clearCluster(&continuation);
                continuation.setLinkID(next.linkID());
                Ubiquitty::Cell blank;
                Ubiquitty::Screen::eraseCell(&blank, s);
                t.grid->clearWideOverlap(Ubiquitty::Row(t.parser.clusterRow), Ubiquitty::Column(t.parser.clusterColumn + 1), Ubiquitty::Column(t.parser.clusterColumn + 1), blank);
                t.grid->setCell(Ubiquitty::Row(t.parser.clusterRow), Ubiquitty::Column(t.parser.clusterColumn + 1), continuation);
                advanceWidenedCluster(t);
            }
        } else if (isPictographicBase && codePoint == Ubiquitty::Unicode::Width::variationSelector15) {
            nextPen.attributes.remove(VT::Attribute::Emoji);
        }
        if (next.clusterID() == 0) {
            if (next.combiningLength() < Ubiquitty::cellMaxCombining) {
                next.combining[next.combiningLength()] = codePoint;
                next.setCombiningLength(static_cast<uint8_t>(next.combiningLength() + 1));
            } else {
                didInlineOverflow = true;
            }
        }
    }

    // Sync the overflow pool once for the batch: interning the full tracked
    // sequence yields the same id when unchanged, a fresh one when it grew.
    if (next.clusterID() != 0 || didInlineOverflow)
        next.setClusterID(internGrownCluster(t, next.clusterID()));

    // Only the inline buffer, pool id, and attribute bits change above, so this
    // three-field probe decides the write exactly as setCell's memcmp would: a
    // pool-only append leaves the cell bit-identical and skips the write.
    const bool didCellChange = next.clusterPacked != current->clusterPacked || nextPen.attributes != Ubiquitty::cellPen(*current).attributes;
    if (didCellChange) {
        Ubiquitty::cellSetPen(next, nextPen);
        t.grid->setCellKnownDiff(Ubiquitty::Row(t.parser.clusterRow), Ubiquitty::Column(t.parser.clusterColumn), next);
    }
    if (specID) [[unlikely]]
        writeClusterBands(t, next);
    t.parser.clusterLast = codePoints[count - 1];
}

// `attachedSelector` (0 = none) is a VS15/VS16 the caller already decoded after
// `codePoint`, so the cell is written once with the selector attached and the
// final presentation/width. Semantics mirror clusterAppend: VS16 upgrades only an
// Extended_Pictographic base; VS15 forces text but keeps the base's width. If
// `codePoint` itself extends an open cluster, both fall back to the unfused path.
// Wide (width-2) placement, kept out of line so the narrow-glyph path does not
// carry it. Wrap commit has already run; row/column are final.
[[gnu::noinline]] static void emitWideCharacter(Ubiquitty::Terminal& t, int row, int column, uint32_t codePoint, uint32_t ch, uint32_t attachedSelector, bool isEmoji, int rightMargin)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const VT::AttributeSet penAttributes = s->pen.attributes - VT::Attribute::Emoji;
    const VT::AttributeSet emojiAttribute = isEmoji ? VT::AttributeSet { VT::Attribute::Emoji } : VT::AttributeSet {};
    VT::AttributeSet leadAttributes = penAttributes | VT::Attribute::Wide | emojiAttribute;
    // The continuation half carries no presentation flags — strip the force
    // bit too (penAttributes already dropped Emoji).
    VT::AttributeSet continuationAttributes = (penAttributes - VT::Attribute::ForceEmojiPresentation) | VT::Attribute::WideContinuation;
    // logicalCellAt: the idempotency probe must not materialize a deferred
    // recycle blank — the write lane below blanks only the stale flanks. The
    // continuation probe runs only once the lead half matches: a changed lead already
    // decides the write, and a flood changes every lead.
    const Ubiquitty::Cell* currentLead = t.grid ? t.grid->logicalCellAt(Ubiquitty::Row(row), Ubiquitty::Column(column)) : nullptr;
    // Repainting the same base can replace a different selector or a longer
    // cluster; equal pens and code points alone do not make the pair unchanged.
    bool samePair = currentLead && currentLead->codePoint == ch && currentLead->clusterID() == 0
        && ((attachedSelector == 0 && currentLead->combiningLength() == 0)
            || (attachedSelector != 0 && currentLead->combiningLength() == 1 && currentLead->combining[0] == attachedSelector))
        && currentLead->linkID() == t.currentLinkID && cellMatchesPen(currentLead, &s->pen, leadAttributes);
    if (samePair) {
        const Ubiquitty::Cell* currentContinuation = t.grid->logicalCellAt(Ubiquitty::Row(row), Ubiquitty::Column(column + 1));
        samePair = currentContinuation && currentContinuation->codePoint == Ubiquitty::wideContinuationSentinelCodePoint && currentContinuation->clusterID() == 0 && currentContinuation->combiningLength() == 0 && currentContinuation->linkID() == t.currentLinkID && cellMatchesPen(currentContinuation, &s->pen, continuationAttributes);
    }
    if (t.grid && !samePair) {
        Ubiquitty::Cell lead, continuation, blank;
        lead.codePoint = ch;
        Pen leadPen = s->pen;
        leadPen.attributes = leadAttributes;
        Ubiquitty::cellSetPen(lead, leadPen);
        Ubiquitty::Screen::clearCluster(&lead);
        if (attachedSelector) {
            lead.combining[0] = attachedSelector;
            lead.setCombiningLength(1);
        }
        lead.setLinkID(t.currentLinkID);
        continuation.codePoint = Ubiquitty::wideContinuationSentinelCodePoint;
        Pen continuationPen = s->pen;
        continuationPen.attributes = continuationAttributes;
        Ubiquitty::cellSetPen(continuation, continuationPen);
        Ubiquitty::Screen::clearCluster(&continuation);
        continuation.setLinkID(t.currentLinkID);
        Ubiquitty::Screen::eraseCell(&blank, s);
        t.grid->writeWideCell(Ubiquitty::Row(row), Ubiquitty::Column(column), lead, continuation, blank);
    }
    if (t.shouldWriteRightToLeft) {
        const int left = std::max(Ubiquitty::Screen::effectiveLeft(t), Screen::lineHomeColumn(t));
        s->cursorX = std::max(left, column - 1);
        s->wrapPending = column <= left;
        if (s->wrapPending)
            evaluateFilledRowTriggers(&t, row);
    } else if (column + 1 >= rightMargin) {
        s->wrapPending = true;
        // Full-width line just completed (wide glyph landed at the margin):
        // evaluate it now so it fires even if no further char ever wraps it.
        evaluateFilledRowTriggers(&t, row);
    } else
        s->cursorX += 2;
    // Open the cluster on the lead cell so a following combining mark
    // / VS / ZWJ continuation extends this wide glyph in place.
    if (attachedSelector)
        clusterOpenWithInlineContinuation(t, row, column, codePoint, attachedSelector);
    else
        clusterOpen(t, row, column, codePoint);
}

// DECSASD redirect, out of line: writes target the dedicated statusRow buffer
// (1-cell-per-glyph, no width-2 / emoji / clustering). Renderer reads statusRow
// at DECSSDT type 2. Cold for every stream that isn't writing a status line.
[[gnu::noinline]] static void emitCharacterToStatusRow(Ubiquitty::Terminal& t, uint32_t ch)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    clusterReset(t);
    if (t.statusCursorX < 0)
        t.statusCursorX = 0;
    if (t.statusCursorX < t.columns) {
        auto status = Ubiquitty::Rendering::ensureStatusRow(t);
        if (t.shouldWriteRightToLeft && t.modes.insertMode)
            std::move(status.begin() + 1, status.begin() + t.statusCursorX + 1, status.begin());
        Ubiquitty::Cell& cell = status[t.statusCursorX];
        cell.codePoint = ch;
        cellSetPenFromScreen(cell, s);
        Ubiquitty::Screen::clearCluster(&cell);
        cell.setLinkID(t.currentLinkID);
        if (t.shouldWriteRightToLeft)
            t.statusCursorX = std::max(0, t.statusCursorX - 1);
        else
            t.statusCursorX++;
    }
}

// VS15/VS16 pair fusion (declared here, defined after emitCharacter — it re-enters
// the unfused path). Returns true when the pair was fully handled.

static void emitAlignedTabCharacter(Ubiquitty::Terminal&, uint32_t, uint32_t, uint32_t);

static int columnsBeforeLowerTextBand(const Ubiquitty::Terminal& terminal, int row, int column, int count)
{
    if (!terminal.grid || !terminal.grid->hasSizedCellHint() || !terminal.grid->rowMeta(row).has(Ubiquitty::RowFlag::HasMulticell))
        return count;
    for (int offset = 0; offset < count; ++offset) {
        const auto* cell = terminal.grid->logicalCellAt(Ubiquitty::Row(row), Ubiquitty::Column(column + offset));
        if (!cell)
            break;
        const auto& pen = cell->pen;
        if (Ubiquitty::penTextBand(&pen) != 0 && terminal.textSizeTable.lookup(pen.extendedAttributes.textSizeSpecID()))
            return offset;
    }
    return count;
}

// Ordinary text may replace a block's first row; lower rows reserve their cells.
[[gnu::noinline]] static bool movePastLowerTextBands(Ubiquitty::Terminal& terminal, int width, int left, int right)
{
    auto& screen = *terminal.screens.active;
    int feeds = 0;
    if (terminal.shouldWriteRightToLeft) {
        for (;;) {
            const int first = std::max(left, screen.cursorX - width + 1);
            const int count = screen.cursorX - first + 1;
            if (columnsBeforeLowerTextBand(terminal, screen.cursorY, first, count) == count)
                return true;
            screen.wrapPending = false;
            if (first > left) {
                --screen.cursorX;
                continue;
            }
            if (++feeds > terminal.rows || (screen.cursorY == terminal.rows - 1 && screen.cursorY != screen.scrollBottom))
                return false;
            wrapCommitLinefeed(terminal);
            screen.cursorX = effectiveRightVisible(terminal);
            if (screen.cursorY > 0)
                screen.lineWrapped[screen.cursorY - 1] = true;
        }
    }
    for (;;) {
        const int count = std::min(width, right - screen.cursorX + 1);
        if (count <= 0)
            return false;
        const int before = columnsBeforeLowerTextBand(terminal, screen.cursorY, screen.cursorX, count);
        if (before == count)
            return true;
        int next = screen.cursorX + before + 1;
        while (next <= right) {
            const auto* cell = terminal.grid->logicalCellAt(Ubiquitty::Row(screen.cursorY), Ubiquitty::Column(next));
            if (!cell || !cell->pen.attributes.contains(VT::Attribute::WideContinuation))
                break;
            ++next;
        }
        screen.wrapPending = false;
        if (next <= right && (width == 1 || next < right || !terminal.modes.autowrap)) {
            screen.cursorX = next;
            continue;
        }
        if (++feeds > terminal.rows || (screen.cursorY == terminal.rows - 1 && screen.cursorY != screen.scrollBottom))
            return false;
        screen.cursorX = left;
        wrapCommitLinefeed(terminal);
        if (screen.cursorY > 0)
            screen.lineWrapped[screen.cursorY - 1] = true;
    }
}

static bool canFuseGraphicPair(const Terminal& terminal, uint32_t selector)
{
    if (terminal.isAlignedTabActive || terminal.statusLineActive || terminal.shouldWriteRightToLeft || terminal.triggersEngine)
        return false;
    if (selector != Unicode::Width::variationSelector16)
        return true;
    if (terminal.modes.insertMode)
        return false;
    const auto& screen = *terminal.screens.active;
    const int column = screen.wrapPending && terminal.modes.autowrap
        ? std::max(Screen::effectiveLeft(terminal), Screen::lineHomeColumn(terminal))
        : screen.cursorX;
    return column + 1 <= effectiveRightVisible(terminal) && column + 1 < terminal.columns;
}

// Replacing a block's top band removes every lower band before later input can reach it.
[[gnu::noinline]] static void eraseOverwrittenTextBands(Ubiquitty::Terminal& terminal, int row, int first, int last)
{
    if (!terminal.grid->rowMeta(row).has(RowFlag::HasMulticell))
        return;
    Cell blank;
    Screen::eraseCell(&blank, terminal.screens.active);
    for (int column = first; column <= last;) {
        const auto* current = terminal.grid->logicalCellAt(Row(row), Column(column));
        if (!current || !current->pen.extendedAttributes.textSizeSpecID() || penTextBand(&current->pen) != 0) {
            ++column;
            continue;
        }
        int anchor = column;
        while (anchor > 0 && terminal.grid->logicalCellAt(Row(row), Column(anchor))->pen.attributes.contains(Attribute::WideContinuation))
            --anchor;
        const auto* lead = terminal.grid->logicalCellAt(Row(row), Column(anchor));
        const auto* spec = terminal.textSizeTable.lookup(lead->pen.extendedAttributes.textSizeSpecID());
        const int columns = spec ? claimedRunColumns(*lead, &terminal.textSizeTable, terminal.ambiguousWidth) : 1;
        if (spec && spec->scale > 1) {
            const int end = std::min(terminal.columns - 1, anchor + columns - 1);
            captureGraphicCells(terminal, row, anchor, end);
            for (int band = 1; band < spec->scale && row + band < terminal.rows; ++band) {
                const auto* below = terminal.grid->logicalCellAt(Row(row + band), Column(anchor));
                if (!below || below->pen.extendedAttributes.textSizeSpecID() != lead->pen.extendedAttributes.textSizeSpecID()
                    || penTextBand(&below->pen) != static_cast<unsigned>(band))
                    break;
                captureGraphicCells(terminal, row + band, anchor, end);
                terminal.grid->fillCellRun(Row(row + band), Column(anchor), blank, end - anchor + 1);
            }
        }
        column = std::max(column + 1, anchor + columns);
    }
}

static void emitCharacter(Ubiquitty::Terminal& t, uint32_t codePoint, uint32_t ch, uint32_t attachedSelector = 0, GraphicCapture capture = GraphicCapture::Retain)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (attachedSelector && (capture == GraphicCapture::Retain || !canFuseGraphicPair(t, attachedSelector))) {
        emitCharacter(t, codePoint, ch, 0, GraphicCapture::Discard);
        emitCharacter(t, attachedSelector, attachedSelector, 0, capture);
        t.parser.precedingItem.setGraphic(codePoint, ch, attachedSelector);
        return;
    }
    GraphicCaptureScope captureScope(t, capture);
    // Record the graphic as the REP predecessor up front, before any placement
    // branch returns — every graphic must be repeatable exactly as it arrived
    // (ECMA-48 §8.3.103).
    t.parser.precedingItem.setGraphic(codePoint, ch, attachedSelector);
    if (t.isAlignedTabActive) [[unlikely]] {
        captureGraphicCells(t, t.alignedTabRow, 0, t.columns - 1);
        emitAlignedTabCharacter(t, codePoint, ch, attachedSelector);
        return;
    }
    // Triggers autowrap dedupe: a CR that returned to a full row already fired at
    // fill (wrap cleared) makes this an in-place REWRITE, so drop the eval-guard
    // and let the next LF re-evaluate. wrapPending still set ⇒ the wrap-commit
    // handles dedupe. Guard is -1 unless a [triggers] rule is configured.
    if (t.triggerWrapEvaluatedRow == s->cursorY && !s->wrapPending)
        t.triggerWrapEvaluatedRow = -1;
    int width;
    int row;
    int column;
    VT::AttributeSet attributes;
    VT::AttributeSet emojiAttribute;
    // Printable ASCII is always width 1 and never emoji/combining, so skip the
    // trie probe; everything else takes one flat-table probe for width + emoji,
    // its width resolved under this terminal's ambiguous-width choice.
    const std::uint8_t codePointClass = (codePoint >= 0x20 && codePoint < 0x7F) ? static_cast<std::uint8_t>(0x01) : Ubiquitty::Unicode::Width::classify(codePoint);
    const int cpWidth = Ubiquitty::Unicode::Width::cellsForWidthClass(Ubiquitty::Unicode::detail::PropWidthField::get(codePointClass), t.ambiguousWidth);
    bool isEmoji = (codePoint >= 0x80) && (codePointClass & 0x04) != 0;

    // SGR 76 forcing overrides the codePoint default and any VS selector below,
    // meaningful only for dual-presentation (Extended_Pictographic) bases. The
    // direction rides the pen's Attribute::Emoji bit; presentation + width are
    // pinned after the VS-fusion block so force always wins.
    const bool forcePresentation = s->pen.attributes.contains(VT::Attribute::ForceEmojiPresentation) && codePoint >= 0x80 && Ubiquitty::Unicode::isExtendedPictographic(codePoint);
    const bool forceEmojiDirection = forcePresentation && s->pen.attributes.contains(VT::Attribute::Emoji);

    int fusedWidth = cpWidth;
    if (attachedSelector && Unicode::isExtendedPictographic(codePoint)) {
        isEmoji = attachedSelector == Unicode::Width::variationSelector16;
        if (isEmoji)
            fusedWidth = 2;
    }

    // SGR 76 force wins over the default and any VS: forced emoji = wide (2),
    // forced text = narrow (1). The clusterAppend VS path re-checks the force bit
    // on the lead cell, so a later VS can't flip a forced run either.
    if (forcePresentation) {
        isEmoji = forceEmojiDirection;
        fusedWidth = forceEmojiDirection ? 2 : 1;
    }

    // DECSASD redirect (out of line; see emitCharacterToStatusRow).
    if (t.statusLineActive) [[unlikely]] {
        captureGraphicCells(t, 0, 0, t.columns - 1);
        emitCharacterToStatusRow(t, ch);
        return;
    }

    // Grapheme-cluster extension (UAX #29). When an open cluster sits under the
    // cursor and the code point extends it (combining mark, VS, ZWJ, RI-pair half,
    // skin-tone modifier), append to the lead cell without advancing the cursor —
    // the whole cluster keeps its single-cell (or Wide pair) span. Clustering is
    // unconditional (mode 2027's flag round-trips but never gates this).
    //
    // Fast reject: every UAX-29 rule hinges on a code point >= U+0300, so when
    // both the last and incoming code points are below that, graphemeExtends can
    // only return false — skip the whole check on the dominant ASCII/Latin-1 path
    // (the integer test short-circuits before clusterIsLive's field loads).
    const bool shouldAskClusterRules = (codePoint >= 0x300 || t.parser.clusterLast >= 0x300) && clusterIsLive(t);
    if (shouldAskClusterRules && Ubiquitty::Unicode::graphemeExtends(&t.parser.clusterState, t.parser.clusterLast, codePoint)) {
        const bool hasMulticell = t.grid->rowMeta(t.parser.clusterRow).has(RowFlag::HasMulticell);
        captureGraphicCells(t, t.parser.clusterRow, hasMulticell ? 0 : t.parser.clusterColumn - 2,
            hasMulticell ? t.columns - 1 : t.parser.clusterColumn + 2);
        clusterAppend(t, codePoint);
        if (attachedSelector && Ubiquitty::Unicode::graphemeExtends(&t.parser.clusterState, t.parser.clusterLast, attachedSelector))
            clusterAppend(t, attachedSelector);
        return;
    }

    if (isEmoji) {
        // Attribute::Emoji flags emoji presentation so the frontend folds at paint.
        emojiAttribute = VT::Attribute::Emoji;
    }
    width = fusedWidth;

    // emojiAttribute is the single source of truth for this cell's presentation;
    // strip Attribute::Emoji from the pen contribution so non-pictographic cells
    // in an SGR-76 forced run stay monochrome.
    const VT::AttributeSet penAttributes = s->pen.attributes - VT::Attribute::Emoji;

    if (width == 0) {
        // The rules put a boundary before this code point, so the open cluster
        // ends here though the code point takes no cell. The break state has
        // moved past it, and a cluster left open would take the next code point
        // under the rule that follows this one, as GB9b joins whatever comes
        // after a Prepend. A code point the fast reject passed over leaves the
        // state where it was, and the cluster stays open.
        if (shouldAskClusterRules)
            clusterReset(t);
        return;
    }
    // A double-width or double-height line shows only its left half, so text
    // wraps at the middle of it, as DEC STD 070 and xterm have it.
    int rightMargin = effectiveRightVisible(t);
    int leftMargin = std::max(Ubiquitty::Screen::effectiveLeft(t), Screen::lineHomeColumn(t));
    if (t.shouldWriteRightToLeft && s->cursorX < leftMargin)
        leftMargin = 0;
    if (s->wrapPending && t.modes.autowrap) {
        s->cursorX = t.shouldWriteRightToLeft ? rightMargin : leftMargin;
        wrapCommitLinefeed(t);
        if (t.shouldWriteRightToLeft)
            s->cursorX = effectiveRightVisible(t);
        s->wrapPending = false;
        // Mark the row just left as soft-wrap-joined to the next. The wrapped row
        // is always cursorY-1 — true both when linefeed moved down and when it
        // scrolled at the region bottom — so a soft-wrapped URL stays one line.
        if (s->cursorY > 0)
            s->lineWrapped[s->cursorY - 1] = true;
    }
    if (width == 2 && (t.shouldWriteRightToLeft ? s->cursorX <= leftMargin : s->cursorX >= rightMargin) && t.modes.autowrap) {
        s->cursorX = t.shouldWriteRightToLeft ? rightMargin : leftMargin;
        wrapCommitLinefeed(t);
        if (t.shouldWriteRightToLeft)
            s->cursorX = effectiveRightVisible(t);
        s->wrapPending = false;
        if (s->cursorY > 0)
            s->lineWrapped[s->cursorY - 1] = true;
    }

    if (t.grid && t.grid->hasSizedCellHint() && !movePastLowerTextBands(t, width, leftMargin, rightMargin))
        return;

    const int captureColumn = t.shouldWriteRightToLeft ? std::max(leftMargin, s->cursorX - width + 1) : s->cursorX;
    const bool shouldCaptureWholeRow = t.modes.insertMode || t.grid->rowMeta(s->cursorY).has(RowFlag::HasMulticell);
    captureGraphicCells(t, s->cursorY, shouldCaptureWholeRow ? 0 : captureColumn - 1,
        shouldCaptureWholeRow ? t.columns - 1 : captureColumn + width);

    // IRM opens cells in the writing direction, discarding text at that margin.
    if (t.modes.insertMode && t.grid) {
        if (t.shouldWriteRightToLeft)
            Ubiquitty::Screen::insertCharactersRightToLeft(t, width);
        else
            Ubiquitty::Screen::insertCharacters(t, width);
    }

    row = s->cursorY;
    column = t.shouldWriteRightToLeft ? std::max(leftMargin, s->cursorX - width + 1) : s->cursorX;
    if (t.grid->hasSizedCellHint()) [[unlikely]]
        eraseOverwrittenTextBands(t, row, column, std::min(t.columns - 1, column + width - 1));

    if (width == 2 && column + 1 < t.columns) {
        emitWideCharacter(t, row, column, codePoint, ch, attachedSelector, isEmoji, rightMargin);
        return;
    }

    attributes = penAttributes | emojiAttribute;
    {
        bool same = false;
        bool shouldClearWideOverlap = false;
        {
            const auto read = t.grid ? t.grid->logicalCellReadAt(Ubiquitty::Row(row), Ubiquitty::Column(column)) : CellRead {};
            const auto* current = read.get();
            // The idempotency probe also requires an empty combining buffer:
            // a cell holding a cluster is rewritten as a lone base when the
            // same base lands again, dropping its continuation codepoints.
            same = current && current->codePoint == ch && ((attachedSelector == 0 && current->combiningLength() == 0) || (current->combiningLength() == 1 && current->clusterID() == 0 && current->combining[0] == attachedSelector)) && current->linkID() == t.currentLinkID
                && cellMatchesPen(current, &s->pen, attributes);
            shouldClearWideOverlap = !same && current && Ubiquitty::cellPen(*current).attributes.containsAny({ VT::Attribute::Wide, VT::Attribute::WideContinuation });
        }
        if (t.grid && !same) {
            // clearWideOverlap only has work when the cell being
            // overwritten is itself half of a wide pair (its orphaned
            // other half must be blanked). A narrow cell — the entire
            // plain-text path — has no pair to break, so skip the call
            // and the blank-cell construction it needs.
            if (shouldClearWideOverlap) {
                Ubiquitty::Cell blank;
                Ubiquitty::Screen::eraseCell(&blank, s);
                t.grid->clearWideOverlap(Ubiquitty::Row(row), Ubiquitty::Column(column), Ubiquitty::Column(column), blank);
            }
            if (attachedSelector) {
                Ubiquitty::Cell cell;
                cell.codePoint = ch;
                Pen cellPenValue = s->pen;
                cellPenValue.attributes = attributes;
                Ubiquitty::cellSetPen(cell, cellPenValue);
                Ubiquitty::Screen::clearCluster(&cell);
                cell.combining[0] = attachedSelector;
                cell.setCombiningLength(1);
                cell.setLinkID(t.currentLinkID);
                t.grid->setCellKnownDiff(Ubiquitty::Row(row), Ubiquitty::Column(column), cell);
            } else {
                displaySetCell(t, row, column, &s->pen, ch, attributes);
            }
        }
    }

    if (t.shouldWriteRightToLeft ? s->cursorX <= leftMargin : s->cursorX >= rightMargin) {
        s->wrapPending = true;
        // Full-width line just completed (narrow glyph landed at the margin):
        // evaluate it now so a tail line that fills exactly to the edge fires
        // even when no further char ever arrives to commit the wrap.
        evaluateFilledRowTriggers(&t, row);
    } else
        s->cursorX += t.shouldWriteRightToLeft ? -1 : 1;
    // Open the cluster on this cell so a following combining mark / VS /
    // ZWJ continuation / RI-pair half extends it in place.
    if (attachedSelector)
        clusterOpenWithInlineContinuation(t, row, column, codePoint, attachedSelector);
    else
        clusterOpen(t, row, column, codePoint);
}

void finishAlignedTabString(Terminal& terminal)
{
    if (!terminal.isAlignedTabActive)
        return;
    terminal.isAlignedTabActive = false;
    terminal.alignedTabOriginalRow.clear();
    clusterReset(terminal);
}

static void beginAlignedTabString(Terminal& terminal)
{
    const auto& screen = *terminal.screens.active;
    if (terminal.tabAlignments.empty() || !terminal.tabStops[screen.cursorX] || terminal.tabAlignments[screen.cursorX] == CharacterTabAlignment::Plain)
        return;
    if (!terminal.alignedTabOriginalRow.tryResize(static_cast<std::size_t>(terminal.columns)))
        return;
    if (!terminal.alignedTabTerminal)
        terminal.alignedTabTerminal.reset(new (std::nothrow) Terminal);
    if (!terminal.alignedTabTerminal)
        return;
    auto& staging = *terminal.alignedTabTerminal;
    hardReset(staging);
    resize(staging, 1, maxColumns, Modes::ReflowMode::Fixed);
    staging.modes.autowrap = false;
    staging.ambiguousWidth = terminal.ambiguousWidth;
    terminal.grid->readRow(Row(screen.cursorY), terminal.alignedTabOriginalRow.data());
    terminal.alignedTabRow = screen.cursorY;
    terminal.alignedTabStop = screen.cursorX;
    terminal.didAlignedTabOverflow = false;
    terminal.isAlignedTabActive = true;
    clusterReset(terminal);
}

static void emitAlignedTabCharacter(Terminal& terminal, uint32_t codePoint, uint32_t character, uint32_t attachedSelector)
{
    auto& staging = *terminal.alignedTabTerminal;
    auto& stagedScreen = *staging.screens.active;
    auto& screen = *terminal.screens.active;
    if (terminal.didAlignedTabOverflow)
        return;
    if (stagedScreen.wrapPending) {
        auto clusterState = staging.parser.clusterState;
        if (!clusterIsLive(staging) || !Unicode::graphemeExtends(&clusterState, staging.parser.clusterLast, codePoint)) {
            terminal.didAlignedTabOverflow = true;
            return;
        }
    }
    stagedScreen.pen = screen.pen;
    staging.currentLinkID = terminal.currentLinkID;
    if (terminal.graphicCancellation)
        terminal.graphicCancellation->didEmitAlignedGraphic = true;
    emitCharacter(staging, codePoint, character, attachedSelector);
    const int length = stagedScreen.cursorX + (stagedScreen.wrapPending ? 1 : 0);
    if (!length)
        return;
    const auto staged = staging.grid->rowCells(Row(0)).data;
    const int firstWidth = cellPen(staged[0]).attributes.contains(Attribute::Wide) ? 2 : 1;
    int lastColumn = length - 1;
    while (lastColumn > 0 && cellPen(staged[lastColumn]).attributes.contains(Attribute::WideContinuation))
        --lastColumn;
    int offset = 0;
    switch (terminal.tabAlignments[terminal.alignedTabStop]) {
    case CharacterTabAlignment::Plain:
        break;
    case CharacterTabAlignment::Centered:
        offset = (firstWidth + lastColumn) / 2;
        break;
    case CharacterTabAlignment::LeadingEdge:
        offset = lastColumn;
        break;
    case CharacterTabAlignment::TrailingEdge:
        offset = firstWidth;
        break;
    case CharacterTabAlignment::TargetCharacter:
        offset = firstWidth;
        for (int column = 0; column < length; ++column) {
            if (!cellPen(staged[column]).attributes.contains(Attribute::WideContinuation)
                && staged[column].codePoint == terminal.tabAlignmentTargets[terminal.alignedTabStop]) {
                const int width = cellPen(staged[column]).attributes.contains(Attribute::Wide) ? 2 : 1;
                offset = column + width / 2;
                break;
            }
        }
        break;
    }
    const int firstColumn = terminal.alignedTabStop - offset;
    const int left = Screen::effectiveLeft(terminal);
    const int right = effectiveRightVisible(terminal);
    std::array<Cell, maxColumns> row;
    std::copy(terminal.alignedTabOriginalRow.begin(), terminal.alignedTabOriginalRow.end(), row.begin());
    Cell blank {};
    Screen::eraseCell(&blank, terminal);
    if (terminal.modes.insertMode) {
        const int start = std::clamp(firstColumn, left, right + 1);
        const int count = std::min(length, right + 1 - start);
        std::move_backward(row.begin() + start, row.begin() + right + 1 - count, row.begin() + right + 1);
        std::fill_n(row.begin() + start, count, blank);
    }
    for (int column = 0; column < length; ++column) {
        const int destination = firstColumn + column;
        if (destination < left || destination > right)
            continue;
        Cell next = staged[column];
        if (next.clusterID()) {
            std::array<uint32_t, ClusterPool::maxCodePoints> codePoints;
            const int count = cellClusterCodePoints(&staging, &next, codePoints.data(), static_cast<int>(codePoints.size()));
            next.setClusterID(terminal.clusterPool.intern({ codePoints.data(), static_cast<std::size_t>(count) }));
        }
        row[destination] = next;
    }
    int firstTouched = 0;
    int lastTouched = terminal.columns - 1;
    Screen::scrubMulticellOrphans(row.data(), terminal.columns, blank, &terminal.textSizeTable,
        terminal.ambiguousWidth, firstTouched, lastTouched);
    terminal.grid->setRowSpan(Row(terminal.alignedTabRow), Column(0), { row.data(), terminal.columns });
    screen.cursorY = terminal.alignedTabRow;
    screen.cursorX = std::clamp(firstColumn + length, left, right);
    screen.wrapPending = firstColumn + length > right;
}

bool isNationalReplacementEnabled(const Ubiquitty::Terminal& terminal)
{
    return terminal.statusLineActive ? terminal.isStatusNationalReplacementEnabled : terminal.modes.nrcMode;
}

// Translate one GL-range position byte (0x20..0x7F) through a designated
// G-set. Handles both 94-sets (DEC special, DEC technical, NRCS, UK) and
// 96-sets (a single-shift / GR set can be a 96-set, reached in GL by SS2/SS3):
// a 96-set byte is folded up into its 0xA0..0xFF home before set96Translate.
static inline uint32_t resolveGLByte(int cs, uint8_t c)
{
    if (cs == characterSetUSASCII)
        return c;
    if (isCharacterSet96(cs))
        return set96Translate(cs, static_cast<uint8_t>(c | 0x80));
    if (cs == characterSetDECSpecial && c >= 0x60 && c <= 0x7E)
        return decSpecialToUnicode(c);
    if (cs == characterSetDECTechnical)
        return decTechnicalToUnicode(c);
    if (cs == characterSetUK && c == '#')
        return static_cast<uint32_t>(0x00A3); // £
    if (cs >= characterSetFrench && cs <= characterSetFrenchCanadian && c >= 0x23 && c <= 0x7E)
        return nrcsTranslate(cs, c);
    return c;
}

// Map a GL byte through the downloaded DRCS soft set. A defined glyph slot
// becomes a private sentinel code point the renderer looks up in t->softFont; an
// undefined slot falls back to the raw byte. glyph index = byte - 0x20.
static inline uint32_t resolveSoftDRCS(const Ubiquitty::Terminal& t, uint8_t b)
{
    if (b < 0x20 || b > 0x7F)
        return b;
    const int index = b - 0x20;
    if (index >= 0 && index < Ubiquitty::VT::drcsMaxCells && Ubiquitty::hasValidSoftFont(t) && t.softFont->glyphSet[index])
        return Ubiquitty::VT::drcsCodePointBase + static_cast<uint32_t>(index);
    return b;
}

static inline uint32_t resolveCodePoint(Ubiquitty::Terminal& t, uint32_t codePoint)
{
    // Fast path: no single-shift and the active GL set is ASCII (every normal
    // text stream) — no translation applies, so the code point passes through.
    // GR invocation doesn't affect GL resolution, so characterSetGR is irrelevant.
    if (t.singleShift == 0 && t.characterSetG[t.characterSetActive] == characterSetUSASCII && !t.modes.vt52Graphics)
        return codePoint;
    // VT52 graphics mode (ESC F) maps 0x60..0x7E to the DEC special set,
    // the same glyphs ANSI reaches via characterSetDECSpecial.
    if (t.modes.vt52Graphics && codePoint >= 0x60 && codePoint <= 0x7E)
        return decSpecialToUnicode(static_cast<uint8_t>(codePoint));
    int activeG = t.characterSetActive;
    if (t.singleShift == 2 || t.singleShift == 3) {
        activeG = t.singleShift;
        t.singleShift = 0;
    }
    if (t.characterSetG[activeG] == characterSetSoftDRCS && codePoint < 0x80)
        return resolveSoftDRCS(t, static_cast<uint8_t>(codePoint));
    if (codePoint < 0x80)
        return resolveGLByte(t.characterSetG[activeG], static_cast<uint8_t>(codePoint));
    return codePoint;
}

// GR (0xA0..0xFF) exists only in an 8-bit coding: under a single-byte input
// encoding processEncodedBytesImpl hands those bytes to the parser raw while a
// GR set is invoked or a single shift is pending, and they resolve through that
// set. Under UTF-8, and under every multibyte encoding, they are parts of
// encoded characters and no GR invocation or designation applies to them; the
// invocation is still recorded for DECSC, DECRQSS and DECCIR.
static inline bool isGRGraphicByte(const Ubiquitty::Terminal& t, uint8_t c)
{
    return c >= 0xA0 && t.parser.utf8.need == 0 && (t.characterSetGR != 0 || t.singleShift != 0) && !t.parser.parserInputIsUTF8
        && Ubiquitty::Unicode::encodingIsSingleByte(t.parser.inputEncoding.enc);
}

// Resolve one GR-range byte (0xA0..0xFF) through the invoked GR set or a pending
// single-shift. Only reached for an isGRGraphicByte byte. SS2/SS3 overrides GR
// (one-shot); a 94-set folds the byte down into GL before resolve.
static inline uint32_t resolveGRByte(Ubiquitty::Terminal& t, uint8_t c)
{
    int activeG;
    if (t.singleShift == 2 || t.singleShift == 3) {
        activeG = t.singleShift;
        t.singleShift = 0;
    } else {
        activeG = t.characterSetGR; // 1..3; 0 handled by caller (default Latin-1)
    }
    int cs = t.characterSetG[activeG];
    if (cs == characterSetSoftDRCS) {
        if ((!t.softFont || !t.softFont->set96) && (c == 0xA0 || c == 0xFF))
            return c; // 94-set GR leaves 10/00 and 15/15 unused.
        return resolveSoftDRCS(t, static_cast<uint8_t>(c & 0x7F)); // fold GR→GL
    }
    if (isCharacterSet96(cs))
        return set96Translate(cs, c);
    // A 94-set invoked into GR addresses its glyphs via the low 7 bits, but
    // the 10/00 and 15/15 endpoints are outside the set and stay raw if they
    // still arrive.
    if (c == 0xA0 || c == 0xFF)
        return c;
    return resolveGLByte(cs, static_cast<uint8_t>(c & 0x7F));
}

/// Selects the invoked graphic character set.
enum class GraphicInvocationArea : uint8_t {
    GL,
    GR,
};

static void emitTranslatedGraphic(Terminal& terminal, uint32_t codePoint, GraphicInvocationArea area, GraphicCapture capture = GraphicCapture::Retain)
{
    const int singleShift = terminal.singleShift;
    const auto character = area == GraphicInvocationArea::GR ? resolveGRByte(terminal, static_cast<uint8_t>(codePoint)) : resolveCodePoint(terminal, codePoint);
    emitCharacter(terminal, codePoint, character, 0, capture);
    if (terminal.graphicCancellation)
        terminal.graphicCancellation->singleShift = singleShift;
}

uint32_t resolveGraphicCharacter(Terminal& terminal, int value)
{
    const int shift = terminal.singleShift;
    uint32_t character = static_cast<uint32_t>(value);
    if (value < 128)
        character = resolveCodePoint(terminal, character);
    else if (terminal.characterSetGR || shift)
        character = resolveGRByte(terminal, static_cast<uint8_t>(value));
    terminal.singleShift = shift;
    return character;
}

void csiH_alignedTab(Terminal& terminal, char final)
{
    const int position = parameterOr(terminal, 0, 0);
    if (position < 1 || position > terminal.columns)
        return;
    uint32_t target = 0;
    if (final == 'c') {
        const int value = parameterOmitted(terminal, 1)
            ? 32
            : static_cast<int>(terminal.parser.parameters[1]);
        if (!((value >= 32 && value <= 127) || (value >= 160 && value <= 255)))
            return;
        target = resolveGraphicCharacter(terminal, value);
    }
    if (!terminal.tabAlignments.tryResize(maxColumns) || !terminal.tabAlignmentTargets.tryResize(maxColumns))
        return;
    terminal.tabStops[position - 1] = true;
    terminal.tabAlignmentTargets[position - 1] = target;
    terminal.tabAlignments[position - 1] = final == 'a' ? CharacterTabAlignment::LeadingEdge
        : final == 'b'                                  ? CharacterTabAlignment::Centered
        : final == 'c'                                  ? CharacterTabAlignment::TargetCharacter
                                                        : CharacterTabAlignment::TrailingEdge;
}

// Substitute for one offending UTF-8 byte per the configured policy. Off the
// valid fast path entirely — only reached when utf8Feed already returned
// Invalid, so valid decoding pays nothing for it per code point.
//   FFFD   — emit U+FFFD REPLACEMENT CHARACTER (default).
//   Drop   — emit nothing.
//   Latin1 — reinterpret the byte as ISO-8859-1 (byte -> U+0080..U+00FF). The
//            cell holds canonical Unicode, so codePoint == ch == the byte.
// The substitute is a graphic like any other: CCH can cancel it, so it is
// journaled by what follows it, as `capture` says, and not by default.
static inline void emitInvalidUTF8(Ubiquitty::Terminal& t, uint8_t offendingByte, GraphicCapture capture)
{
    switch (t.parser.invalidUTF8Policy) {
    case Ubiquitty::Unicode::InvalidUTF8Policy::Drop:
        return;
    case Ubiquitty::Unicode::InvalidUTF8Policy::Latin1:
        emitTranslatedGraphic(t, offendingByte, GraphicInvocationArea::GL, capture);
        return;
    case Ubiquitty::Unicode::InvalidUTF8Policy::FFFD:
        break;
    }
    emitCharacter(t, 0xFFFD, 0xFFFD, 0, capture); // U+FFFD has width 1
}

// Close an incomplete UTF-8 sequence that the next byte cannot continue.
//
// The decoder only hears about bytes that reach putCharacter, and a C0 control
// or an ESC is dispatched before that — so a stream that leaves a sequence
// truncated and then does something else had its pending lead dropped in
// silence, and the reader saw one character fewer than arrived. Unicode's
// recommended practice is one U+FFFD for the maximal subpart, which is what the
// configured policy substitutes here before the control acts.
//
// - Parameter capture: Whether the substitute stays journaled for CCH, decided
//   from the control that closes the sequence as from any other successor.
static void closeTruncatedUTF8(Ubiquitty::Terminal& t, GraphicCapture capture)
{
    if (t.parser.utf8.need == 0)
        return;
    resetUTF8(t);
    emitInvalidUTF8(t, t.parser.utf8LeadByte, capture);
}

// A C1 control decoded from UTF-8 (U+0080..U+009F, spelled C2 80..C2 9F) has
// no graphic to draw. U+0085 is the one that is a control in a UTF-8 stream:
// ECMA-48 8.3.86 gives NEL the representation 08/05, and Unicode 5.8 makes
// U+0085 a newline function, so it acts as NEL. The others are ignored, as
// xterm ignores them, leaving the cursor, the cell under it and the REP
// predecessor as they were: a code point that introduces a control string or a
// sequence must not be reachable from text that merely contains it. A byte the
// Latin-1 policy reinterprets is the policy's to place and does not come
// through here.
static constexpr bool isC1CodePoint(uint32_t codePoint) { return codePoint >= 0x80 && codePoint <= 0x9F; }
static constexpr uint32_t nextLineCodePoint = 0x85;

static void actC1NextLine(Ubiquitty::Terminal& t, uint8_t b);

// Perform what a C1 code point decoded from UTF-8 means, if it means anything.
static void performDecodedC1CodePoint(Ubiquitty::Terminal& t, uint32_t codePoint)
{
    if (codePoint == nextLineCodePoint)
        actC1NextLine(t, static_cast<uint8_t>(codePoint));
}

// Feed one byte through the UTF-8 decoder; emit a cell when a code point
// is complete or the byte is plain ASCII. Malformed / truncated sequences
// fall back to the configured invalid-UTF-8 policy (default U+FFFD).
// Non-ASCII / mid-sequence half of putCharacter, kept out of line so the ASCII
// path does not carry the GR, decoder and invalid-recovery machinery.
[[gnu::noinline]] static void putCharacterNonASCII(Ubiquitty::Terminal& t, uint8_t c, GraphicCapture capture)
{
    if (isGRGraphicByte(t, c)) {
        emitTranslatedGraphic(t, c, GraphicInvocationArea::GR, capture);
    } else {
        uint32_t codePoint;
        bool wasMid = (t.parser.utf8.need > 0);
        std::uint32_t fed;
        Ubiquitty::Unicode::UTF8Status st = Ubiquitty::Unicode::utf8Feed(&t.parser.utf8, c, &fed);
        if (st == Ubiquitty::Unicode::UTF8Status::Emit) {
            codePoint = fed;
            if (isC1CodePoint(codePoint))
                performDecodedC1CodePoint(t, codePoint);
            else
                emitTranslatedGraphic(t, codePoint, GraphicInvocationArea::GL, capture);
        } else if (st == Ubiquitty::Unicode::UTF8Status::Need) {
            // Under the Latin1 policy only, remember a fresh sequence's lead byte
            // so a later substitution can reinterpret it if a bad continuation
            // aborts the sequence (the decoder discards it).
            if (!wasMid && t.parser.invalidUTF8Policy == Ubiquitty::Unicode::InvalidUTF8Policy::Latin1)
                t.parser.utf8LeadByte = c;
        } else if (st == Ubiquitty::Unicode::UTF8Status::Invalid) {
            if (wasMid) {
                // A byte that cannot continue broke the in-progress sequence: the
                // abandoned LEAD is what failed to decode (substitute that),
                // then re-feed the offending byte — it may itself begin a fresh
                // sequence (or be plain ASCII).
                // The lead's substitute is journaled only when its successor
                // can be CCH: the one lead that decodes to a C1 control is C2.
                // Every other successor is a graphic this call emits, or a
                // sequence that ends in one, which CCH cancels instead. The
                // successor is read off a copy of the decoder, so the stream's
                // order stands: the substitute first, then the re-fed byte.
                Ubiquitty::Unicode::UTF8Decoder probe = t.parser.utf8;
                std::uint32_t probed;
                const bool isC1Lead = c == 0xC2 && Ubiquitty::Unicode::utf8Feed(&probe, c, &probed) == Ubiquitty::Unicode::UTF8Status::Need;
                emitInvalidUTF8(t, t.parser.utf8LeadByte, isC1Lead ? GraphicCapture::Retain : GraphicCapture::Discard);
                std::uint32_t codePoint2;
                Ubiquitty::Unicode::UTF8Status st2 = Ubiquitty::Unicode::utf8Feed(&t.parser.utf8, c, &codePoint2);
                if (st2 == Ubiquitty::Unicode::UTF8Status::Emit) {
                    if (!isC1CodePoint(codePoint2))
                        emitTranslatedGraphic(t, codePoint2, GraphicInvocationArea::GL, capture);
                } else if (st2 == Ubiquitty::Unicode::UTF8Status::Need)
                    t.parser.utf8LeadByte = c;
                else // Invalid: c is itself a bad lead / lone continuation.
                    emitInvalidUTF8(t, c, capture);
            } else {
                // Byte c is itself a bad lead or lone continuation.
                emitInvalidUTF8(t, c, capture);
            }
        }
    }
}

// Attribution harness for the parse path. Each bit skips one phase so its cost
// shows up as the delta against the same binary with the mask at zero, which is
// the only comparison that holds: a separate build moves code layout and the
// difference stops being the phase.
//
// Read once per process rather than per call — a getenv on a per-byte path would
// be measuring the harness, and a function-local static would put a
// thread-safe-init guard on every call. Zero by default and never set in
// production, so each test is a loop-invariant load the predictor never misses.
//
// Output is WRONG under any non-zero mask. This answers "what does this phase
// cost", never "is the parse right" — the suite runs with the mask unset.
//
// The variable is UBIQUITTY_PARSE_ABLATE. A mask set under any other name is not
// read and every phase reports its full cost, which looks like a phase that is
// free rather than like a switch that was never thrown, so a run that reports no
// deltas at all should suspect the spelling before the code.
enum ParseAblateBit : unsigned {
    // The run writer's minimum: batching is refused, so every printable takes
    // the per-character path. Sizes what batching is worth on a given stream.
    AblateBatchedRuns = 1u << 0,
    // The lone-printable placement: a single character re-enters emitCharacter
    // rather than taking the guarded fast placement.
    AblatePlainCellPlacement = 1u << 1,
    // The idempotency probe on the batched writer's destination, which costs a
    // second row resolution per run.
    AblateRunIdempotencyProbe = 1u << 2,
};

// Compiled out unless the build asks for it. An environment read would put a
// load and a branch inside functions the profile counts, which detaches them
// from it and costs a retrain to get back — for a harness that ships switched
// off. `if constexpr` on a zero mask leaves codegen identical to no harness at
// all, and an attributing build is a build flag away:
//
//     UBQ_EXTRA_CXXFLAGS=-DUBIQUITTY_PARSE_ABLATE=2 ./Scripts/build-perf-bench.sh bench_core
#if !defined(UBIQUITTY_PARSE_ABLATE)
#define UBIQUITTY_PARSE_ABLATE 0
#endif
inline constexpr unsigned parseAblateMask = UBIQUITTY_PARSE_ABLATE;

// One printable ASCII cell, placed without entering emitCharacter.
//
// emitCharacter answers for every graphic there is — selectors, clusters, wide
// pairs, insert mode, the status row, the wrap commit — and pays for all of them
// on entry, which a stream writing one character between escapes pays per cell.
//
// The guard admits only the shape where every one of those answers is already
// known, and each clause names the branch it stands in for. A margin-adjacent
// column is refused outright rather than handled: leaving it to emitCharacter
// keeps the wrap arming, the filled-row trigger and the whole wrap commit out
// of here, and it costs one cell in a row.
//
// - Returns: Whether the cell was placed. False leaves the terminal untouched.
[[nodiscard]] static inline bool emitPlainASCIICell(Ubiquitty::Terminal& t, uint8_t c, GraphicCapture capture)
{
    if (t.isAlignedTabActive) [[unlikely]]
        return false;
    if constexpr ((parseAblateMask & AblatePlainCellPlacement) != 0)
        return false;
    if (c < 0x20 || c >= 0x7F)
        return false; // width 1, non-emoji, non-grapheme-relevant is what the rest assumes
    Ubiquitty::Screen::State* s = t.screens.active;
    if (!t.grid || t.statusLineActive || t.modes.insertMode || s->wrapPending || t.shouldWriteRightToLeft)
        return false;
    // resolveCodePoint is the identity only under these; otherwise the byte is
    // not its own code point.
    if (t.singleShift != 0 || t.modes.vt52Graphics || t.characterSetG[t.characterSetActive] != characterSetUSASCII)
        return false;
    // An open cluster whose last codepoint could still extend belongs to the
    // cluster arm. Below U+0300 on both sides, no rule joins them.
    if (t.parser.clusterLast >= 0x300)
        return false;
    const int row = s->cursorY;
    const int column = s->cursorX;
    if (row < 0 || row >= t.rows || column < 0)
        return false;
    // Strictly inside the margin: the cell at it arms the deferred wrap and
    // evaluates the row's triggers, which is emitCharacter's tail.
    //
    // This is effectiveRightVisible's edge in two reads. A doubled row ends at
    // its middle, so its attribute decides the answer only from that column
    // on; before it, every row has room.
    if (column >= Ubiquitty::Screen::effectiveRight(t))
        return false;
    if (column >= t.columns / 2 - 1 && s->lineAttribute[row] != lineAttributeNormal)
        return false;
    if (columnsBeforeLowerTextBand(t, row, column, 1) == 0)
        return false;

    GraphicCaptureScope captureScope(t, capture);
    if (capture == GraphicCapture::Retain)
        captureGraphicCells(t, row, column - 1, column + 1);
    if (t.grid->hasSizedCellHint()) [[unlikely]]
        eraseOverwrittenTextBands(t, row, column, column);
    const uint32_t codePoint = c;
    // The REP predecessor is recorded before any placement, exactly as the
    // general path records it (ECMA-48 §8.3.103).
    t.parser.precedingItem.setGraphic(codePoint, codePoint, 0);
    if (t.triggerWrapEvaluatedRow == row)
        t.triggerWrapEvaluatedRow = -1;

    const VT::AttributeSet attributes = s->pen.attributes - VT::Attribute::Emoji;
    bool same = false;
    bool shouldClearWideOverlap = false;
    {
        const auto read = t.grid->logicalCellReadAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
        const auto* current = read.get();
        same = current && current->codePoint == codePoint && current->combiningLength() == 0
            && current->linkID() == t.currentLinkID && cellMatchesPen(current, &s->pen, attributes);
        shouldClearWideOverlap = !same && current && Ubiquitty::cellPen(*current).attributes.containsAny({ VT::Attribute::Wide, VT::Attribute::WideContinuation });
    }
    if (!same) {
        if (shouldClearWideOverlap) {
            Ubiquitty::Cell blank;
            Ubiquitty::Screen::eraseCell(&blank, s);
            t.grid->clearWideOverlap(Ubiquitty::Row(row), Ubiquitty::Column(column), Ubiquitty::Column(column), blank);
        }
        displaySetCell(t, row, column, &s->pen, codePoint, attributes);
    }
    s->cursorX++;
    clusterOpen(t, row, column, codePoint);
    return true;
}

static void putCharacter(Ubiquitty::Terminal& t, uint8_t c, GraphicCapture capture = GraphicCapture::Retain)
{
    // Fast path: a lone ASCII byte with no UTF-8 sequence in progress is its own
    // code point — emit directly, skipping the utf8Feed() decoder (byte-for-byte
    // equivalent to feeding it).
    if (t.parser.utf8.need == 0 && c < 0x80) [[likely]] {
        if (!emitPlainASCIICell(t, c, capture))
            emitTranslatedGraphic(t, c, GraphicInvocationArea::GL, capture);
    } else
        putCharacterNonASCII(t, c, capture);
    // Margin bell — fire once per pass through the threshold column
    // (columns - nMarginBell). Re-arm when cursorX drops strictly below it; the
    // fire-once-then-disarm rule stops the bell firing on every overwrite there.
    if (t.marginBell) {
        if (t.screens.active->cursorX < t.marginBellColumn)
            t.marginBellArmed = true;
        if (t.marginBellArmed && t.screens.active->cursorX == t.marginBellColumn) {
            Ubiquitty::ringBell(t);
            t.marginBellArmed = false;
        }
    }
}

// SGR parameter folding lives in Core/SGR.cpp; the parser dispatches to
// Ubiquitty::SGR::apply at the CSI 'm' final byte.

int parameterOr(const Ubiquitty::Terminal& t, int index, int dflt)
{
    if (index >= t.parser.parameterCount || t.parser.parameters[index] == 0)
        return dflt;
    return t.parser.parameters[index];
}

// ---- DEC MC (Media Copy) print sink ----
// An opaque FILE* (Terminal::printSinkFile). When non-null, MC operations route
// output there (print-screen / print-line dump UTF-8 + LF; auto-print copies each
// line). With no sink the operations are inert (mode bits still round-trip).

using CSIHandler = void (*)(Ubiquitty::Terminal&, char);
struct CSIEntry {
    char final;
    CSIHandler fn;
};

// Sorted by final byte (ASCII). H and f share csiH_cup (CUP/HVP).
static constexpr CSIEntry csiHandlers[] = {
    { '@', csiH_ich },
    { 'A', csiH_cuu },
    { 'B', csiH_cud },
    { 'C', csiH_cuf },
    { 'D', csiH_cub },
    { 'E', csiH_cnl },
    { 'F', csiH_cpl },
    { 'G', csiH_cha },
    { 'H', csiH_cup },
    { 'I', csiH_cht },
    { 'J', csiH_ed },
    { 'K', csiH_el },
    { 'L', csiH_il },
    { 'M', csiH_dl },
    { 'N', csiH_ef },
    { 'O', csiH_ea },
    { 'P', csiH_dch },
    { 'S', csiH_su },
    { 'T', csiH_sd },
    { 'W', csiH_ctc },
    { 'X', csiH_ech },
    { 'Y', csiH_cvt },
    { 'Z', csiH_cbt },
    { ']', csiH_linuxPower }, // Linux-console power/display (CSI 9/13/14 ])
    { '^', csiH_simd }, // SIMD, not xterm's obsolete SD alias
    { '`', csiH_hpa },
    { 'a', csiH_hpr }, // HPR — data-position movement
    { 'b', csiH_rep },
    { 'c', csiH_da },
    { 'd', csiH_vpa },
    { 'e', csiH_vpr }, // VPR — data-position movement
    { 'f', csiH_cup },
    { 'g', csiH_tbc },
    { 'h', csiH_sm },
    { 'i', csiH_mc },
    { 'j', csiH_hpb },
    { 'k', csiH_vpb },
    { 'l', csiH_rm },
    { 'm', csiH_sgr },
    { 'n', csiH_dsr },
    { 'o', csiH_daq },
    { 'r', csiH_decstbm },
    { 's', csiH_scosc },
    { 't', csiH_xtwinops },
    { 'u', csiH_scorc },
    { 'x', csiH_decreqtparm },
};

// Compile-time sortedness check (strictly increasing finals → also no dupes).
template<size_t N> constexpr bool CSITableSortedStrict(const CSIEntry (&a)[N], size_t i = 1) { return (i >= N) ? true : ((static_cast<unsigned char>(a[i - 1].final) < static_cast<unsigned char>(a[i].final)) && CSITableSortedStrict(a, i + 1)); }
static_assert(CSITableSortedStrict(csiHandlers), "csiHandlers must be sorted by final byte with no duplicates");

// O(1) final-byte dispatch: a 256-slot table maps each CSI final to its handler
// (nullptr = unknown), built constexpr from the same csiHandlers array so it
// can't drift. Stores the handler pointer directly, so dispatch is one indexed load.
struct CSIJumpTable {
    CSIHandler slot[256];
};
static constexpr CSIJumpTable makeCSIJumpTable()
{
    CSIJumpTable t {}; // all slots null (unknown final -> nullptr)
    for (auto csiHandler : csiHandlers)
        t.slot[static_cast<unsigned char>(csiHandler.final)] = csiHandler.fn;
    return t;
}
static constexpr CSIJumpTable cSIJumpTable = makeCSIJumpTable();

static CSIHandler csiLookup(char final) { return cSIJumpTable.slot[static_cast<unsigned char>(final)]; }

// Prefixed CSI dispatch table: (priv, intermediate, final) lookup in one
// binary-search hop. Key = (priv << 16) | (intermediate << 8) | final; priv == 0
// means no prefix, intermediate == 0 means none. Handlers walk
// Terminal::parameters themselves.
struct CSIPrefixedEntry {
    unsigned char priv; // 0 if none
    unsigned char intermediate; // 0 if none
    char final;
    void (*fn)(Ubiquitty::Terminal&, char);
};

static constexpr unsigned csiPrefixedKey(unsigned char p, unsigned char i, char f) { return (static_cast<unsigned>(p) << 16) | (static_cast<unsigned>(i) << 8) | static_cast<unsigned char>(f); }

// Sorted by the (priv, intermediate, final) key. priv 0 < '=' < '>' < '?';
// intermediate 0 (none) < ' ' < '!' < '"' < '#' < '$' < '*'.
static constexpr CSIPrefixedEntry csiPrefixedHandlers[] = {
    { 0, ' ', '@', csiH_sl },
    { 0, ' ', 'A', csiH_sr },
    { 0, ' ', 'H', csiH_quad },
    { 0, ' ', 'S', csiH_spd },
    { 0, ' ', 'U', csiH_slh },
    { 0, ' ', 'V', csiH_sll },
    { 0, ' ', '`', csiH_alignedTab },
    { 0, ' ', 'a', csiH_alignedTab },
    { 0, ' ', 'b', csiH_alignedTab },
    { 0, ' ', 'c', csiH_alignedTab },
    { 0, ' ', 'd', csiH_tsr },
    { 0, ' ', 'q', csiH_decscusr },
    { 0, '!', 'p', csiH_decstr },
    { 0, '"', 'p', csiH_decscl },
    { 0, '"', 'q', csiH_decsca },
    { 0, '"', 'v', csiH_decrqde },
    { 0, '#', 'P', csiH_xtpushcolors },
    { 0, '#', 'Q', csiH_xtpopcolors },
    { 0, '#', 'R', csiH_xtreportcolors },
    { 0, '#', 'S', csiH_xttitlepos },
    { 0, '#', 'p', csiH_xtpushsgr }, // xterm's alias of CSI # {
    { 0, '#', 'q', csiH_xtpopsgr }, // xterm's alias of CSI # }
    { 0, '#', 'y', csiH_xtchecksum },
    { 0, '#', '{', csiH_xtpushsgr },
    { 0, '#', '|', csiH_xtreportsgr },
    { 0, '#', '}', csiH_xtpopsgr },
    { 0, '$', 'p', csiH_decrqm_ansi },
    { 0, '$', 'r', csiH_deccara },
    { 0, '$', 't', csiH_decrara },
    { 0, '$', 'u', csiH_decrqtsr },
    { 0, '$', 'v', csiH_deccra },
    { 0, '$', 'w', csiH_decrqpsr },
    { 0, '$', 'x', csiH_decfra },
    { 0, '$', 'z', csiH_decera },
    { 0, '$', '{', csiH_decsera }, // DECSERA — selective erase rect
    { 0, '$', '|', csiH_decscpp },
    { 0, '$', '}', csiH_decsasd },
    { 0, '$', '~', csiH_decssdt },
    { 0, '&', 'u', csiH_decrqupss },
    { 0, '\'', 'w', csiH_decefr },
    { 0, '\'', 'z', csiH_decelr },
    { 0, '\'', '{', csiH_decsle },
    { 0, '\'', '|', csiH_decrqlp },
    { 0, '\'', '}', csiH_decic }, // DECIC — insert columns
    { 0, '\'', '~', csiH_decdc }, // DECDC — delete columns
    { 0, '*', 'x', csiH_decsace },
    { 0, '*', 'y', csiH_decrqcra },
    { 0, '*', '|', csiH_decsnls },
    { 0, '+', 'p', csiH_decsr },
    { 0, '+', 'r', csiH_decsmkr },
    { 0, ',', 'u', csiH_decrqkt },
    { 0, ',', '|', csiH_decac },
    { 0, ',', '}', csiH_decatc },
    { 0, ',', '~', csiH_decps },
    { '<', 0, 'u', csiH_kitty_pop },
    { '=', 0, 'c', csiH_da3 },
    { '=', 0, 'u', csiH_kitty_set },
    { '>', 0, 'T', csiH_resettitlemode },
    { '>', 0, 'c', csiH_da2 },
    { '>', 0, 'f', csiH_xtmod_set_f },
    { '>', 0, 'g', csiH_xtfmt_query_g },
    { '>', 0, 'm', csiH_xtmod_set_m },
    { '>', 0, 'n', csiH_xtmod_clear_n },
    { '>', 0, 'q', csiH_xtversion },
    { '>', 0, 't', csiH_settitlemode },
    { '>', 0, 'u', csiH_kitty_push },
    { '?', 0, 'J', csiH_decsed },
    { '?', 0, 'K', csiH_decsel },
    { '?', 0, 'S', csiH_xtsmgraphics },
    { '?', 0, 'W', csiH_decst8c },
    { '?', 0, 'g', csiH_xtfmt_query_g },
    { '?', 0, 'h', csiH_decset },
    { '?', 0, 'i', csiH_mc_dec },
    { '?', 0, 'l', csiH_decrst },
    { '?', 0, 'm', csiH_xtmod_query_m },
    { '?', 0, 'n', csiH_dsr_dec },
    { '?', 0, 'r', csiH_xtrestore },
    { '?', 0, 's', csiH_xtsave },
    { '?', 0, 'u', csiH_kitty_query },
    { '?', '$', 'p', csiH_decrqm_dec },
};

template<size_t N> constexpr bool CSIPrefixedTableSorted(const CSIPrefixedEntry (&a)[N], size_t i = 1) { return (i >= N) || ((csiPrefixedKey(a[i - 1].priv, a[i - 1].intermediate, a[i - 1].final) < csiPrefixedKey(a[i].priv, a[i].intermediate, a[i].final)) && CSIPrefixedTableSorted(a, i + 1)); }
static_assert(CSIPrefixedTableSorted(csiPrefixedHandlers), "csiPrefixedHandlers must be sorted by (priv,int,final) key");

static const CSIPrefixedEntry* csiPrefixedLookup(unsigned char priv, unsigned char intermediate, char final)
{
    unsigned want = csiPrefixedKey(priv, intermediate, final);
    int lo = 0, hi = static_cast<int>(sizeof(csiPrefixedHandlers) / sizeof(csiPrefixedHandlers[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        unsigned got = csiPrefixedKey(csiPrefixedHandlers[mid].priv, csiPrefixedHandlers[mid].intermediate, csiPrefixedHandlers[mid].final);
        if (got == want)
            return &csiPrefixedHandlers[mid];
        if (got < want)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return nullptr;
}

// Stands in the private-marker slot once a CSI has proved malformed, so the
// rest of it is collected and the sequence dispatches nothing. No control has
// this byte as its marker.
static constexpr int malformedCSIMarker = 0x7F;

// A CSI is malformed when a private marker follows a parameter, an
// intermediate or another marker, or a parameter follows an intermediate:
// `CSI 2 ? 3 H`, `CSI ? ? 5 h`, `CSI ? 1 $ 2 p`. The VT500 state machine
// ignores such a sequence through to its final byte, as xterm does; acting on
// it reads parameters the program never meant for that control.
static inline void markCSIMalformed(Ubiquitty::Terminal& t) { t.parser.privatePrefix = malformedCSIMarker; }

// Whether a colon separated any of the sequence's parameters.
static bool hasSubparameters(const Ubiquitty::Terminal& t)
{
    const int count = std::min(t.parser.parameterCount, static_cast<int>(t.parser.parameterSeparatorWasColon.size()));
    for (int i = 1; i < count; ++i) {
        if (t.parser.parameterSeparatorWasColon[static_cast<std::size_t>(i)])
            return true;
    }
    return false;
}

// Only SGR and DECPS give a colon a meaning. Any other control carrying one is
// ignored, as xterm ignores it: `CSI 2 : 3 H` is not a cursor position.
static bool takesSubparameters(const Ubiquitty::Terminal& t, char final)
{
    if (t.parser.privatePrefix == 0 && t.parser.intermediate == 0)
        return final == 'm';
    return t.parser.privatePrefix == 0 && t.parser.intermediate == ',' && final == '~';
}

static void csiDispatch(Ubiquitty::Terminal& t, char final)
{
    discardGraphicCancellation(t);
    if (t.parser.privatePrefix == malformedCSIMarker)
        return;
    if (final != 'm' && hasSubparameters(t) && !takesSubparameters(t, final))
        return;
    if (t.isAlignedTabActive && !((t.parser.privatePrefix == 0 && t.parser.intermediate == 0 && (final == 'm' || final == 'b' || final == 'n' || final == 'c' || final == 'x')) || (t.parser.intermediate == '$' && (final == 'p' || final == 'w'))))
        finishAlignedTabString(t);
    if (t.parser.privatePrefix != 0 || t.parser.intermediate != 0) {
        const CSIPrefixedEntry* pe = csiPrefixedLookup(static_cast<unsigned char>(t.parser.privatePrefix), static_cast<unsigned char>(t.parser.intermediate), final);
        if (pe)
            pe->fn(t, final);
        else
            Diagnostics::diagLog(t, "csi-miss", (static_cast<uint32_t>(static_cast<uint8_t>(t.parser.privatePrefix)) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(t.parser.intermediate)) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(final)));
        // Record this CSI as REP's predecessor; an unrecognized sequence records
        // too (its repeatable effect is nothing — REP must not replay an older item).
        return;
    }

    const CSIHandler fn = csiLookup(final);
    if (fn)
        fn(t, final);
    else
        Diagnostics::diagLog(t, "csi-miss", static_cast<uint32_t>(static_cast<uint8_t>(final)));
    discardGraphicCancellation(t);
}

// The ESC dispatch group lives in VT/Escape.cpp. OSC payload parsing lives in
// Core/OSC/Dispatch.cpp; the VT100 parser accumulates into Terminal::osc and calls
// Ubiquitty::OSC::dispatch on the terminator (BEL or ST).

static void resetCSI(Ubiquitty::Terminal& t)
{
    // Both arrays whole, at a size the compiler knows, so the clear stays
    // inline; a runtime length turns it into a library call.
    std::memset(t.parser.parameters.data(), 0, sizeof(t.parser.parameters));
    std::memset(t.parser.parameterSeparatorWasColon.data(), 0, sizeof(t.parser.parameterSeparatorWasColon));
    t.parser.parameterWasEmpty = ~0u; // every slot starts digit-less
    t.parser.didParametersOverflow = false;
    t.parser.parameterCount = 0;
    t.parser.privatePrefix = 0;
    t.parser.intermediate = 0;
}

// Reset tab stops to every-8-columns default for the active dimension.
void resetTabStops(Ubiquitty::Terminal& t)
{
    t.tabAlignments.clear();
    t.tabAlignmentTargets.clear();
    finishAlignedTabString(t);
    for (int i = 0; i < Ubiquitty::maxColumns; i++)
        t.tabStops[i] = (i < t.columns && i % 8 == 0 && i != 0);
}

// Forward declaration: the inner VT100 state machine runs after the
// tmux filter has decoded any control-mode framing.
static void processByteInner(Ubiquitty::Terminal& t, uint8_t c, GraphicCapture capture = GraphicCapture::Retain);

} // namespace VT
} // namespace Ubiquitty
namespace Ubiquitty {
template<bool StopAtSyncClear, VT::GraphicCapture Capture = VT::GraphicCapture::Retain> static int processBytesImpl(Ubiquitty::Terminal& t, std::span<const char> input, bool* stoppedAtSyncClear);
}

namespace Ubiquitty::VT {

/// Emits one code point as graphic text, exactly as the parser would.
void emitGraphicCodePoint(Ubiquitty::Terminal& t, std::uint32_t codePoint)
{
    emitCharacter(t, codePoint, codePoint);
}

static Ubiquitty::Terminal* tmuxDeliveryTarget(Ubiquitty::Terminal& t)
{
    if (!Ubiquitty::Protocol::Tmux::isActive(&t.integration.tmux) || !t.integration.paneRouter.lookup)
        return &t;
    const int paneID = t.integration.tmux.activePaneID;
    if (t.integration.cachedPaneTarget && t.integration.cachedPaneID == paneID)
        return t.integration.cachedPaneTarget;

    t.integration.cachedPaneID = 0;
    t.integration.cachedPaneTarget = nullptr;
    Ubiquitty::Terminal* target = t.integration.paneRouter.route(paneID);
    if (target && target != &t) {
        t.integration.cachedPaneID = paneID;
        t.integration.cachedPaneTarget = target;
    }
    return target;
}

// With tmux active + a paneLookup installed, route the decoded %output byte to
// the Terminal backing the active pane. A NULL lookup means the pane is unknown
// (queued for async spawn); dropping the byte is correct — writing to the primary
// would corrupt the pre-tmux shell screen, and tmux redraws via %output later.
static void tmuxDeliver(void* ctx, uint8_t byte)
{
    Ubiquitty::Terminal* t = static_cast<Ubiquitty::Terminal*>(ctx);
    if (Ubiquitty::Protocol::Tmux::isActive(&t->integration.tmux)) {
        if (t->integration.paneRouter.lookup) {
            Ubiquitty::Terminal* target = tmuxDeliveryTarget(*t);
            if (target && target != t) {
                processByteInner(*target, byte);
            }
            // target == NULL or self-loop → drop.
            return;
        }
        // Active but no router installed (host doesn't model panes) —
        // render in-place so the user at least sees something.
    }
    processByteInner(*t, byte);
}

static void tmuxDeliverSpan(void* ctx, std::string_view bytes)
{
    if (bytes.empty())
        return;
    Ubiquitty::Terminal* t = static_cast<Ubiquitty::Terminal*>(ctx);
    Ubiquitty::Terminal* target = tmuxDeliveryTarget(*t);
    if (Ubiquitty::Protocol::Tmux::isActive(&t->integration.tmux) && t->integration.paneRouter.lookup) {
        if (target && target != t)
            Ubiquitty::processBytesImpl<false>(*target, bytes, nullptr);
        return;
    }
    if (target && target != t) {
        Ubiquitty::processBytesImpl<false>(*target, bytes, nullptr);
        return;
    }
    if (target == t && !Ubiquitty::Protocol::Tmux::isActive(&t->integration.tmux)) {
        Ubiquitty::processBytesImpl<false>(*t, bytes, nullptr);
        return;
    }
    for (const char byte : bytes)
        processByteInner(*t, static_cast<uint8_t>(byte));
}

// End-of-pass companion to tmuxDeliver: lets the frontend release any per-pane
// lock/ref it took to keep a looked-up child Terminal alive across this pass.
static inline void tmuxDeliverFlush(Ubiquitty::Terminal& t)
{
    // Drop the route memo before the child lock/ref is released, so the next pass
    // must re-look-up (and re-acquire) the target.
    Ubiquitty::invalidateTmuxPaneRoute(&t);
    t.integration.paneRouter.endDeliveryPass();
}

static void recordTraceByte(Ubiquitty::Terminal& t, uint8_t c)
{
#ifndef NDEBUG
    // Debug-only sliding 32-byte input window; kept out of release builds so
    // throughput doesn't pay for a diagnostic.
    const int capacity = static_cast<int>(t.integration.traceBytes.size());
    if (t.integration.traceLength < capacity) {
        t.integration.traceBytes[t.integration.traceLength++] = c;
    } else {
        int i;
        for (i = 0; i < capacity - 1; i++)
            t.integration.traceBytes[i] = t.integration.traceBytes[i + 1];
        t.integration.traceBytes[capacity - 1] = c;
    }
#else
    (void)t;
    (void)c;
#endif
}

// Public entry: route the byte through the tmux filter, which either
// passes it through unchanged (passthrough mode) or decodes tmux control mode
// (https://github.com/tmux/tmux/wiki/Control-Mode) and delivers only the inner
// %output payload bytes to the VT100 inner parser.

} // namespace Ubiquitty::VT

namespace Ubiquitty {

void processByte(Ubiquitty::Terminal* t, uint8_t c)
{
    if (!t)
        return;
    // Sliding-window trace: keep the most recent 32 bytes so the
    // debug-window display reflects "what just arrived" rather than
    // only the connection handshake.
    VT::recordTraceByte(*t, c);
    Ubiquitty::Protocol::Tmux::feedInput(&t->integration.tmux, c, t, VT::tmuxDeliver);
}

void invalidateTmuxPaneRoute(Ubiquitty::Terminal* t)
{
    if (!t)
        return;
    t->integration.cachedPaneID = 0;
    t->integration.cachedPaneTarget = nullptr;
}

} // namespace Ubiquitty

namespace Ubiquitty::VT {

// Sixel storage helpers. The accumulator buffer is grown lazily on first sixel
// byte and released in dispose / resetSixelAccumulator, so a session that never
// receives one pays nothing. Per-slot pixel buffers are sized to the worst case
// the parser will accept and freed when the slot is reclaimed by LRU eviction
// or by Dispose/Reset.
static bool sixelAccumulatorReserve(Ubiquitty::Terminal& t)
{
    if (t.images.sixelAccumulatorBuffer)
        return true;
    // Start small; sixelAccumulatorGrow doubles up to maxSixelAccumulatorCapacity on demand so a
    // tiny sixel doesn't pre-allocate the ceiling.
    t.images.sixelAccumulatorBuffer = static_cast<uint8_t*>(std::malloc(sixelAccumulatorInitialCapacity));
    if (!t.images.sixelAccumulatorBuffer) {
        t.images.sixelAccumulatorCapacity = 0;
        return false;
    }
    t.images.sixelAccumulatorCapacity = sixelAccumulatorInitialCapacity;
    t.images.sixelAccumulatorLength = 0;
    t.images.sixelAccumulatorOverflow = false;
    return true;
}

// Double the accumulator up to the maxSixelAccumulatorCapacity ceiling. Returns false at
// the ceiling (or on OOM) — the caller then flags overflow and drops the rest,
// so a malicious / never-terminated DCS is bounded, never allowed to exhaust
// memory. Capacity stays <= maxSixelAccumulatorCapacity (fits int; no overflow in the *2).
static bool sixelAccumulatorGrow(Ubiquitty::Terminal& t)
{
    if (t.images.sixelAccumulatorCapacity >= VT::maxSixelAccumulatorCapacity)
        return false;
    long next = static_cast<long>(t.images.sixelAccumulatorCapacity) * 2;
    if (next > VT::maxSixelAccumulatorCapacity)
        next = VT::maxSixelAccumulatorCapacity;
    uint8_t* grown = static_cast<uint8_t*>(std::realloc(t.images.sixelAccumulatorBuffer, static_cast<size_t>(next)));
    if (!grown)
        return false;
    t.images.sixelAccumulatorBuffer = grown;
    t.images.sixelAccumulatorCapacity = static_cast<int>(next);
    return true;
}

// Empties the accumulator between payloads. One grown past its initial
// allocation is released, so a terminal that received one large sixel does not
// hold up to maxSixelAccumulatorCapacity until it closes; the next payload
// reserves the initial allocation again.
static void sixelAccumulatorReset(Ubiquitty::Terminal& t)
{
    if (t.images.sixelAccumulatorCapacity > sixelAccumulatorInitialCapacity) {
        std::free(t.images.sixelAccumulatorBuffer);
        t.images.sixelAccumulatorBuffer = nullptr;
        t.images.sixelAccumulatorCapacity = 0;
    }
    t.images.sixelAccumulatorLength = 0;
    t.images.sixelAccumulatorOverflow = false;
}

static void sixelAccumulatorAppend(Ubiquitty::Terminal& t, uint8_t b)
{
    if (t.images.sixelAccumulatorOverflow)
        return;
    if (!t.images.sixelAccumulatorBuffer && !sixelAccumulatorReserve(t)) {
        t.images.sixelAccumulatorOverflow = true;
        return;
    }
    if (t.images.sixelAccumulatorLength >= t.images.sixelAccumulatorCapacity && !sixelAccumulatorGrow(t)) {
        t.images.sixelAccumulatorOverflow = true; // hit the ceiling — bounded, drop the rest
        return;
    }
    t.images.sixelAccumulatorBuffer[t.images.sixelAccumulatorLength++] = b;
}

// Bulk form of sixelAccumulatorAppend: identical growth/ceiling semantics
// (fill up to maxSixelAccumulatorCapacity, then set overflow and drop the rest),
// one memcpy per capacity step instead of a call per byte.
static void sixelAccumulatorAppendRun(Ubiquitty::Terminal& t, std::span<const char> run)
{
    const char* const bytes = run.data();
    const int count = static_cast<int>(run.size());
    if (t.images.sixelAccumulatorOverflow)
        return;
    if (!t.images.sixelAccumulatorBuffer && !sixelAccumulatorReserve(t)) {
        t.images.sixelAccumulatorOverflow = true;
        return;
    }
    int offset = 0;
    while (offset < count) {
        if (t.images.sixelAccumulatorLength >= t.images.sixelAccumulatorCapacity && !sixelAccumulatorGrow(t)) {
            t.images.sixelAccumulatorOverflow = true; // hit the ceiling — bounded, drop the rest
            return;
        }
        const int space = t.images.sixelAccumulatorCapacity - t.images.sixelAccumulatorLength;
        const int chunk = count - offset < space ? count - offset : space;
        std::memcpy(t.images.sixelAccumulatorBuffer + t.images.sixelAccumulatorLength, bytes + offset, static_cast<std::size_t>(chunk));
        t.images.sixelAccumulatorLength += chunk;
        offset += chunk;
    }
}

// DEC sixel "Pa" pixel-aspect-ratio code → vertical stretch numerator
// (height:width). 0/1/5/6 = 2:1 (legacy default), 2 = 5:1, 3/4 = 3:1,
// 7/8/9 = 1:1. Mirrors the renderer's blit-time sixel row scale.
static int sixelYStretchNumber(uint8_t aspectRatioSelector)
{
    switch (aspectRatioSelector) {
    case 0:
    case 1:
    case 5:
    case 6:
        return 2;
    case 2:
        return 5;
    case 3:
    case 4:
        return 3;
    case 7:
    case 8:
    case 9:
        return 1;
    default:
        return 2;
    }
}

// Async sixel decode: submit to the TaskQueue; the tick drain installs the
// result. The payload is snapshotted into a job-owned buffer so the parser can
// reset its accumulator before the worker finishes.
struct SixelDecodeJob {
    Ubiquitty::Terminal* term;
    uint32_t terminalGeneration; // snapshot at submit; checked in completion
    Ubiquitty::Image::OwnedPixels accumulatorCopy; // owned; released with the job
    int accumulatorLength;
    uint16_t originRow;
    uint16_t originColumn;
    uint16_t maxWidth;
    uint16_t maxHeight;
    uint8_t aspectRatioSelector;
    // backgroundSelect is the sixel DCS P2 (background-select): 1 = unset pixels stay
    // transparent; 0/2 = unset pixels take background color 0 (opaque). chafa
    // emits 1.
    uint8_t backgroundSelect;
    uint8_t horizontalGridSize;
    bool displayMode; // DECSDM (?80) snapshot at submit: when true
    // the completion leaves the cursor in place.
    bool shouldLeaveCursorRight; // ?8452 at submit: the cursor lands right of the image
    // ?1070 at submit. Reset: `initialPalette` is the terminal's registers and
    // the completion writes the image's final palette back into them.
    bool hasPrivateColorRegisters;
    std::array<std::uint8_t, Ubiquitty::Image::Sixel::paletteBytes> initialPalette;
    // Worker fills these:
    Ubiquitty::Image::OwnedPixels pixelBuffer; // owned; backs image.pixels
    int pixelBufferCapacity;
    Ubiquitty::Image::Sixel::Image image;
    Ubiquitty::Image::Sixel::DecodeStatus rc;
};

// Constructed on this Terminal's first sixel, never eagerly.
static Ubiquitty::TaskQueue* sixelEnsureDecodeQueue(Ubiquitty::Terminal& t)
{
    if (!t.asyncDecodes.sixel)
        t.asyncDecodes.sixel = std::make_unique<Ubiquitty::TaskQueue>();
    return t.asyncDecodes.sixel.get();
}

static void sixelDecodeWorker(void* p)
{
    SixelDecodeJob* j = static_cast<SixelDecodeJob*>(p);
    // Compute pixel count in size_t to prevent signed-int overflow when both
    // dimensions are near uint16_t max (65535 × 65535 overflows int).
    const std::size_t pixelCount = static_cast<std::size_t>(j->maxWidth) * static_cast<std::size_t>(j->maxHeight);
    const bool doesP2AskForTransparency = j->backgroundSelect == 1;
    const std::size_t byteCount = doesP2AskForTransparency ? pixelCount * 2 : pixelCount;
    if (byteCount == 0
        || byteCount > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        j->rc = Ubiquitty::Image::Sixel::DecodeStatus::InvalidBuffer;
        return;
    }
    // One allocation backs the palette-index bitmap [0, pixelCount) and, where
    // P2 asked for transparency, the written mask [pixelCount, 2*pixelCount)
    // the completion reads. Only P2=1 reads it, which img2sixel does not emit
    // by default: for everything else the second half was cleared and then
    // stored into once per painted pixel for data nothing looks at. The decoder
    // takes a null mask as "untracked" and the completion already asks whether
    // it exists.
    Ubiquitty::Image::OwnedPixels buffer(static_cast<unsigned char*>(std::malloc(byteCount)));
    if (!buffer) {
        j->rc = Ubiquitty::Image::Sixel::DecodeStatus::InvalidBuffer;
        return;
    }
    unsigned char* const opaqueMask = doesP2AskForTransparency ? buffer.get() + pixelCount : nullptr;
    const std::span<const std::uint8_t> initialPalette = j->hasPrivateColorRegisters ? std::span<const std::uint8_t> {} : std::span<const std::uint8_t>(j->initialPalette);
    j->rc = Ubiquitty::Image::Sixel::decode(std::string_view(reinterpret_cast<const char*>(j->accumulatorCopy.get()), static_cast<size_t>(j->accumulatorLength)), buffer.get(), j->maxWidth, j->maxHeight, &j->image, opaqueMask, initialPalette);
    j->pixelBuffer = std::move(buffer);
    j->pixelBufferCapacity = static_cast<int>(byteCount);
}

static void sixelDecodeCompletion(void* p)
{
    // The job owns every buffer it allocated, so each early return below only
    // has to destroy the job. `queued` does that on whichever path returns.
    const std::unique_ptr<SixelDecodeJob> queued(static_cast<SixelDecodeJob*>(p));
    SixelDecodeJob* j = queued.get();
    j->accumulatorCopy.reset(); // decoded already; hold it no longer than needed
    // Terminal-lifecycle gate: a reset between submit and completion bumps
    // generation, so drop a result the Terminal no longer expects. The queue
    // belongs to that same Terminal, so `term` cannot have been freed.
    if (!j->term || j->term->generation != j->terminalGeneration)
        return;
    if (j->rc != Ubiquitty::Image::Sixel::DecodeStatus::OK || j->image.width == 0 || j->image.height == 0)
        return;
    // Surface the decode-window clip (detail: clamped width << 16 | height)
    // instead of silently showing a cropped image.
    if (j->image.truncated)
        Diagnostics::diagLog(*j->term, "sixel-clipped", (static_cast<uint32_t>(j->image.width) << 16) | j->image.height);
    // Convert the palette-indexed sixel to RGBA and hand it to the same image
    // store the kitty-graphics and OSC 1337 paths use, so sixel inherits
    // scrollback retention, eviction, and scroll-to-fit. The cell footprint
    // comes from the real cell metrics.
    const int sixWidth = static_cast<int>(j->image.width);
    const int sixHeight = static_cast<int>(j->image.height);
    // Normalize sixel's pixel aspect into square-pixel image storage. The
    // placement stays at native size; the text footprint is rounded separately.
    const uint64_t aspectNumerator = j->image.rasterPan > 0 && j->image.rasterPad > 0
        ? j->image.rasterPan
        : sixelYStretchNumber(j->aspectRatioSelector);
    const uint64_t aspectDenominator = j->image.rasterPan > 0 && j->image.rasterPad > 0
        ? j->image.rasterPad
        : 1;
    const uint64_t displayHeight = (static_cast<uint64_t>(sixHeight) * aspectNumerator + aspectDenominator - 1) / aspectDenominator;
    const uint64_t decodedBytes = static_cast<uint64_t>(sixWidth) * displayHeight * 4;
    if (!displayHeight || displayHeight > std::numeric_limits<uint16_t>::max()
        || decodedBytes > Ubiquitty::Image::maxDecodedImageBytes) {
        return;
    }
    Ubiquitty::Image::OwnedPixels rgba(static_cast<unsigned char*>(std::malloc(static_cast<size_t>(decodedBytes))));
    if (!rgba)
        return;
    // P2=1 selects a transparent background: pixels no sixel bit ever painted
    // stay clear instead of taking color 0.
    const bool transparentBackground = (j->backgroundSelect == 1) && (j->image.opaque != nullptr);
    // The palette packed into RGBA words once, so a pixel costs one indexed
    // load and one 32-bit store rather than three palette lookups and four
    // byte stores. Little-endian order puts the low byte first, which is the
    // red the destination expects at offset 0.
    std::uint32_t paletteWords[256];
    for (int entry = 0; entry < 256; ++entry) {
        paletteWords[entry] = static_cast<std::uint32_t>(j->image.palette[entry][0])
            | (static_cast<std::uint32_t>(j->image.palette[entry][1]) << 8)
            | (static_cast<std::uint32_t>(j->image.palette[entry][2]) << 16)
            | (std::uint32_t { 255 } << 24);
    }
    for (uint64_t y = 0; y < displayHeight; ++y) {
        const auto sourceRow = static_cast<size_t>(y * static_cast<uint64_t>(sixHeight) / displayHeight);
        const uint8_t* srow = j->image.pixels + sourceRow * j->image.stride;
        const uint8_t* orow = transparentBackground ? j->image.opaque + sourceRow * j->image.stride : nullptr;
        // malloc's result is aligned for any type, and every row starts a whole
        // number of pixels into it.
        std::uint32_t* destinationWords = reinterpret_cast<std::uint32_t*>(rgba.get()) + static_cast<size_t>(y) * sixWidth;
        // The transparency question is the same for every pixel of the image,
        // so it picks the loop instead of riding inside one.
        if (orow) {
            for (int x = 0; x < sixWidth; ++x)
                destinationWords[x] = orow[x] ? paletteWords[srow[x]] : std::uint32_t { 0 };
        } else {
            for (int x = 0; x < sixWidth; ++x)
                destinationWords[x] = paletteWords[srow[x]];
        }
    }
    j->pixelBuffer.reset(); // also backs j->image.pixels; consumed above

    // Cell footprint (coarse pixel estimate when the frontend hasn't published
    // cell metrics — the headless host/agent path).
    const int cellWidth = j->term->cellPixelWidth ? j->term->cellPixelWidth : 6;
    const int cellHeight = j->term->cellPixelHeight ? j->term->cellPixelHeight : VT::sixelLineHeightInPixels;
    int cellColumns = (sixWidth + cellWidth - 1) / cellWidth;
    int cellRows = (static_cast<int>(displayHeight) + cellHeight - 1) / cellHeight;
    if (cellColumns < 1)
        cellColumns = 1;
    if (cellRows < 1)
        cellRows = 1;

    // Anchor at the sixel origin (where the DCS began), then place via the
    // shared store (creates the image + a placement at the cursor).
    if (j->term->screens.active) {
        j->term->screens.active->cursorY = static_cast<int>(j->originRow);
        j->term->screens.active->cursorX = static_cast<int>(j->originColumn);
    }
    if (!Ubiquitty::Image::storeRGBAImage(j->term, rgba.get(), sixWidth, static_cast<int>(displayHeight), 0, 0))
        return; // no free slot — drop the frame, rgba frees itself
    rgba.release(); // stored: the image owns the pixels now

    // Leave the cursor on the image's last row via the shared post-image
    // advance, scrolling up to make room. DECSDM (?80) leaves it put.
    if (!j->displayMode && j->term->screens.active) {
        Ubiquitty::Screen::State* screen = j->term->screens.active;
        // The image moves the cursor's row and column and leaves a pending
        // wrap pending, so text after an image placed from the last column
        // starts on the next line (xterm graphics_sixel.c, finished_parsing:
        // set_cur_row and set_cur_col leave do_wrap).
        const bool wasWrapPending = screen->wrapPending;
        screen->cursorY = static_cast<int>(j->originRow);
        Ubiquitty::Image::advanceCursorAfterImage(j->term, { 0, cellRows }, Ubiquitty::Image::CursorMotion::Advance);
        screen->cursorX = static_cast<int>(j->originColumn);
        screen->wrapPending = wasWrapPending;
        if (j->shouldLeaveCursorRight) {
            screen->wrapPending = false;
            // xterm ?8452: the text that follows continues beside the image on
            // its last row. An image reaching the right edge leaves the cursor
            // on the last column with the wrap deferred, as a printed
            // character would.
            const int right = static_cast<int>(j->originColumn) + cellColumns;
            if (right < j->term->columns) {
                screen->cursorX = right;
            } else {
                screen->cursorX = j->term->columns - 1;
                screen->wrapPending = j->term->modes.autowrap;
            }
        }
    }
    // Shared registers (?1070 reset): the next image starts from what this one
    // left, so an encoder that defines its colours once keeps them.
    if (!j->hasPrivateColorRegisters)
        std::memcpy(j->term->sixelColorRegisters.data(), &j->image.palette[0][0], Ubiquitty::Image::Sixel::paletteBytes);
}

// Decode the accumulated sixel payload asynchronously.
static void termDrainSixel(Ubiquitty::Terminal* t);

static void sixelFinalize(Ubiquitty::Terminal& t)
{
    // Measuring one pixel beyond the shared limit distinguishes refusal from clipping.
    constexpr int measuredDimensionLimit = Ubiquitty::Image::maxDecodedImageDimension + 1;
    constexpr Ubiquitty::IntSize sixelCanvasCeiling { measuredDimensionLimit, measuredDimensionLimit };
    if (t.images.sixelAccumulatorOverflow || t.images.sixelAccumulatorLength <= 0)
        return;
    // Snapshot the accumulator into a job-owned buffer so the parser
    // can reset state before the worker finishes.
    Ubiquitty::Image::OwnedPixels copy(static_cast<unsigned char*>(std::malloc(static_cast<size_t>(t.images.sixelAccumulatorLength))));
    if (!copy)
        return;
    std::memcpy(copy.get(), t.images.sixelAccumulatorBuffer, static_cast<size_t>(t.images.sixelAccumulatorLength));
    auto job = std::make_unique<SixelDecodeJob>();
    job->term = &t;
    job->terminalGeneration = t.generation;
    job->accumulatorCopy = std::move(copy);
    job->accumulatorLength = t.images.sixelAccumulatorLength;
    job->originRow = t.images.sixelOriginRow;
    job->originColumn = t.images.sixelOriginColumn;
    // Allocate the measured canvas, bounded by the same RGBA budget as other protocols.
    const Ubiquitty::Image::Sixel::PayloadMeasure measured = Ubiquitty::Image::Sixel::measurePayload(
        std::string_view(reinterpret_cast<const char*>(job->accumulatorCopy.get()), static_cast<std::size_t>(job->accumulatorLength)),
        sixelCanvasCeiling);
    if (Ubiquitty::Image::isDecodedImageBytesRefused(measured.canvas.width, measured.canvas.height)) {
        Diagnostics::diagLog(t, "sixel-size-refused", 0);
        return;
    }
    // Refuse a payload that would repaint its own canvas more times than any
    // encoder has reason to. The canvas bounds one run, not the number of them,
    // so without this the decode time is set by the payload length times the
    // canvas width — and it is spent inside the model lock.
    const long long bands = (measured.canvas.height + Ubiquitty::Image::Sixel::bandHeight - 1) / Ubiquitty::Image::Sixel::bandHeight;
    const long long budget = static_cast<long long>(measured.canvas.width) * bands * VT::maxSixelRepaintsOfItsCanvas;
    if (measured.emittedColumns > budget) {
        Diagnostics::diagLog(t, "sixel-work-refused", static_cast<uint32_t>(measured.emittedColumns / (budget ? budget : 1)));
        return;
    }
    job->maxWidth = static_cast<uint16_t>(measured.canvas.width);
    job->maxHeight = static_cast<uint16_t>(measured.canvas.height);
    job->aspectRatioSelector = static_cast<uint8_t>(t.dcsParamCount > 0 ? t.dcsParameters[0] : 0);
    job->backgroundSelect = static_cast<uint8_t>(t.dcsParamCount > 1 ? t.dcsParameters[1] : 0);
    job->horizontalGridSize = static_cast<uint8_t>(t.dcsParamCount > 2 ? t.dcsParameters[2] : 0);
    job->displayMode = t.modes.sixelDisplayMode;
    job->shouldLeaveCursorRight = t.modes.shouldLeaveCursorRightOfSixel;
    job->hasPrivateColorRegisters = t.modes.hasPrivateSixelColorRegisters;
    if (!job->hasPrivateColorRegisters)
        job->initialPalette = t.sixelColorRegisters;
    Ubiquitty::TaskQueue* q = sixelEnsureDecodeQueue(t);
    if (q && q->submit(sixelDecodeWorker, job.get()))
        job.release(); // the queue holds it until the drain's completion
    // Drain at once, as the kitty dispatch does. The worker ran inline in the
    // submit, so the pixels are ready; what a drain at the end of the read
    // added was distance between the image and the cursor that anchors it.
    // The completion parks the cursor on the image's last row from the DCS
    // origin, and a later drain discarded whatever had moved the cursor in
    // between: the newline an emitter prints after its image, or the prompt
    // after that, both of which then landed on the picture.
    termDrainSixel(&t);
}

// Drain the sixel queue, pulling completed decodes into the model. Fast-path
// skips when the queue was never created or nothing is pending — one pointer
// compare + one integer load on every non-sixel byte burst.
static void termDrainSixel(Ubiquitty::Terminal* t)
{
    if (!t || !t->asyncDecodes.sixel || t->asyncDecodes.sixel->pendingCount() == 0)
        return;
    t->asyncDecodes.sixel->drain(sixelDecodeCompletion);
}

// Free every sixel this Terminal submitted but never drained, without running a
// completion: at teardown the Terminal a completion would write into is going
// away. The job owns its buffers, so destroying it releases them.
void discardPendingSixelDecodes(Ubiquitty::Terminal* t)
{
    if (!t || !t->asyncDecodes.sixel)
        return;
    t->asyncDecodes.sixel->drain([](void* p) { delete static_cast<SixelDecodeJob*>(p); });
}

static bool c1StringTerminatorEnabled(const Ubiquitty::Terminal& t) { return !t.parser.parserInputIsUTF8 && !Ubiquitty::Unicode::encodingIsUTF8(t.parser.inputEncoding.enc); }

static bool isStringState(Ubiquitty::VT::Parser::State state)
{
    using State = Ubiquitty::VT::Parser::State;
    switch (state) {
    case State::OSC:
    case State::OSCEscape:
    case State::DCS:
    case State::DCSPassthrough:
    case State::DCSEscape:
    case State::APC:
    case State::APCEscape:
    case State::SOSPMAPC:
    case State::SOSPMAPCEscape:
    case State::SOS:
    case State::SOSEscape:
        return true;
    default:
        return false;
    }
}

// DCS dispatch. The only recognised payload is tmux's allow-passthrough wrapper:
// `DCS tmux; <doubled-payload> ST`, where every inner ESC is doubled so it can't
// be confused with the closing ST. After un-doubling, the inner bytes re-feed
// through processBytes. Nesting is bounded by maxDCSTmuxPassthroughDepth. The
// payload buffers in the OSC accumulator, admitted up to maxOSCNonImageSize.
static void dcsDispatch(Ubiquitty::Terminal& t)
{
    static constexpr char tmuxPrefix[] = "tmux;";
    constexpr int prefixLength = sizeof(tmuxPrefix) - 1;
    if (t.parser.osc.length < prefixLength || std::memcmp(t.parser.osc.data, tmuxPrefix, prefixLength) != 0) {
        t.parser.osc.length = 0;
        return;
    }
    if (t.dcsTmuxPassthroughDepth >= VT::maxDCSTmuxPassthroughDepth) {
        t.parser.osc.length = 0;
        return;
    }
    // Un-double ESC ESC -> ESC in place: the result only shrinks, so it compacts
    // within the bytes it reads. A stray ESC followed by something else is
    // preserved verbatim (defensive against malformed producers).
    int innerLength = 0;
    int srcIndex = prefixLength;
    while (srcIndex < t.parser.osc.length) {
        uint8_t b = static_cast<uint8_t>(t.parser.osc.data[srcIndex++]);
        if (b == 0x1B && srcIndex < t.parser.osc.length && static_cast<uint8_t>(t.parser.osc.data[srcIndex]) == 0x1B) {
            srcIndex++; // skip the second ESC of an ESC ESC pair
        }
        t.parser.osc.data[innerLength++] = static_cast<char>(b);
    }
    // Detach the accumulator rather than copying into a local: the inner feed
    // reuses the accumulator, and a fixed local would cap the payload — feeding a
    // truncated prefix executes half a sequence and parks the parser mid-string.
    Ubiquitty::ByteBuffer inner = t.parser.osc;
    t.parser.osc = {};
    ++t.dcsTmuxPassthroughDepth;
    Ubiquitty::processBytes(&t, std::string_view(inner.data, static_cast<std::size_t>(innerLength)));
    --t.dcsTmuxPassthroughDepth;
    // An unterminated string in the payload leaves its bytes in the buffer the
    // inner feed allocated, so that one wins and carries the parser state.
    if (t.parser.osc.data) {
        inner.reset();
    } else {
        t.parser.osc = inner;
        t.parser.osc.length = 0;
    }
}

// What a cursor or function key capability sends now, or nothing for any other
// capability. xterm answers a key by pretending to press it (do_dcs, its
// `case 'q'`, through Input), so the answer follows DECCKM and the
// modifyCursorKeys and modifyFunctionKeys levels rather than the compiled
// entry's application-mode strings. The forms are Keys::Keymap's
// encodeXtermSpecial's, which a test holds this to; the encoder itself is not
// in every arm this parser is.
static std::optional<std::string> xtgettcapKeyValue(const Ubiquitty::Terminal& t, std::string_view name)
{
    struct CursorKey {
        std::string_view name;
        char final;
    };
    static constexpr CursorKey unmodifiedCursorKeys[] = { { "kcuu1", 'A' }, { "kcud1", 'B' }, { "kcuf1", 'C' }, { "kcub1", 'D' }, { "khome", 'H' }, { "kend", 'F' } };
    static constexpr CursorKey shiftedCursorKeys[] = { { "kri", 'A' }, { "kind", 'B' }, { "kRIT", 'C' }, { "kLFT", 'D' }, { "kHOM", 'H' }, { "kEND", 'F' }, { "kUP", 'A' }, { "kDN", 'B' } };
    static constexpr CursorKey modifiedCursorKeys[] = { { "kUP", 'A' }, { "kDN", 'B' }, { "kRIT", 'C' }, { "kLFT", 'D' }, { "kHOM", 'H' }, { "kEND", 'F' } };
    char final = 0;
    int modifier = 1; // xterm's modifier parameter: 1 + shift 1, alt 2, ctrl 4
    bool isFunctionKey = false;
    int functionKey = 0;
    for (const auto& key : unmodifiedCursorKeys)
        if (name == key.name)
            final = key.final;
    for (const auto& key : shiftedCursorKeys)
        if (name == key.name) {
            final = key.final;
            modifier = 2;
        }
    if (!final && name.size() >= 2 && name.back() >= '3' && name.back() <= '7')
        for (const auto& key : modifiedCursorKeys)
            if (name.substr(0, name.size() - 1) == key.name) {
                final = key.final;
                modifier = name.back() - '0';
            }
    if (!final && name.size() >= 3 && name.size() <= 4 && name.starts_with("kf")) {
        int number = 0;
        for (const char digit : name.substr(2)) {
            if (digit < '0' || digit > '9')
                return std::nullopt;
            number = number * 10 + (digit - '0');
        }
        // kf13-24 shift, 25-36 ctrl, 37-48 ctrl+shift, 49-60 alt, 61-63 alt+shift.
        static constexpr int groupModifiers[] = { 1, 2, 5, 6, 3, 4 };
        if (number < 1 || number > 63)
            return std::nullopt;
        isFunctionKey = true;
        functionKey = (number - 1) % 12 + 1;
        modifier = groupModifiers[(number - 1) / 12];
    }
    if (!final && !isFunctionKey)
        return std::nullopt;

    if (t.modes.keyboardType != Keys::KeyboardType::Default) {
        Keys::KeyEvent event;
        event.modifiers = static_cast<uint32_t>(modifier - 1);
        if (isFunctionKey)
            event.keyCode = Keys::Keymap::keymapKeyF1 + functionKey - 1;
        else {
            switch (final) {
            case 'A':
                event.keyCode = Keys::Keymap::keymapKeyUp;
                break;
            case 'B':
                event.keyCode = Keys::Keymap::keymapKeyDown;
                break;
            case 'C':
                event.keyCode = Keys::Keymap::keymapKeyRight;
                break;
            case 'D':
                event.keyCode = Keys::Keymap::keymapKeyLeft;
                break;
            case 'H':
                event.keyCode = Keys::Keymap::keymapKeyHome;
                break;
            case 'F':
                event.keyCode = Keys::Keymap::keymapKeyEnd;
                break;
            default:
                return std::nullopt;
            }
        }
        Keys::Keymap::EncodeModes modes;
        modes.keyboardType = t.modes.keyboardType;
        modes.termcapProfile = t.modes.termcapProfile;
        modes.modifyKeyboard = t.xtermKeyModifiers[0];
        std::array<char, 64> encoded {};
        const int length = Keys::Keymap::encodeKeyEvent(event,
            t.modes.appCursorKeys ? Modes::CursorKeyMode::Application : Modes::CursorKeyMode::Normal,
            {}, t.xtermKeyModifiers[4], t.xtermKeyFormats[4], t.xtermKeyModifiers[1], t.xtermKeyModifiers[2], t.xtermKeyModifiers[3],
            encoded, modes, userDefinedKeysOrNull(t));
        return std::string(encoded.data(), static_cast<std::size_t>(length));
    }

    const int level = t.xtermKeyModifiers[isFunctionKey ? 2 : 1];
    if (level == Ubiquitty::Keys::Keymap::keymapModifyLevelDisabled)
        modifier = 1;
    std::array<char, 24> buffer {};
    int length = 0;
    if (isFunctionKey && functionKey >= 5) {
        static constexpr int tildeParameters[] = { 15, 17, 18, 19, 20, 21, 23, 24 };
        const int parameter = tildeParameters[functionKey - 5];
        if (modifier == 1)
            length = std::snprintf(buffer.data(), buffer.size(), "\033[%d~", parameter);
        else if (level == 3)
            length = std::snprintf(buffer.data(), buffer.size(), "\033[>%d;%d~", parameter, modifier);
        else
            length = std::snprintf(buffer.data(), buffer.size(), "\033[%d;%d~", parameter, modifier);
    } else {
        if (isFunctionKey)
            final = static_cast<char>('O' + functionKey);
        if (modifier == 1)
            length = std::snprintf(buffer.data(), buffer.size(), "\033%c%c", isFunctionKey || t.modes.appCursorKeys ? 'O' : '[', final);
        else if (level == 1)
            length = std::snprintf(buffer.data(), buffer.size(), "\033[%d%c", modifier, final);
        else if (level == 3)
            length = std::snprintf(buffer.data(), buffer.size(), "\033[>1;%d%c", modifier, final);
        else
            length = std::snprintf(buffer.data(), buffer.size(), "\033[1;%d%c", modifier, final);
    }
    if (length <= 0 || length >= static_cast<int>(buffer.size()))
        return std::nullopt;
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

// XTGETTCAP: one reply for the whole request (xterm do_dcs, its `case 'q'`).
// The names are answered in order as `name=value`, each name echoed as
// sent and each value in capital hex, and a `;` follows every answered name the
// request continues past. The first name this terminal does not know ends the
// reply; when that is the first name, the reply is `DCS 0 + r ST`.
static void xtgettcapDispatch(Ubiquitty::Terminal& t)
{
    // allowTerminalCapabilityOperations gates the WHOLE XTGETTCAP reply:
    // when denied a locked-down session swallows the query with no DCS
    // response, so the host learns nothing about our terminfo. Still drop
    // the accumulated payload so the next frame starts clean.
    if (!t.allowTerminalCapabilityOperations) {
        t.parser.osc.length = 0;
        return;
    }
    static constexpr char hexDigits[] = "0123456789ABCDEF";
    std::string reply = "\033P1+r";
    int answeredCount = 0;
    const char* p = t.parser.osc.data;
    int remaining = t.parser.osc.length;
    while (remaining > 0) {
        int tokLength = 0;
        while (tokLength < remaining && p[tokLength] != ';')
            tokLength++;
        char capabilityName[32];
        const int nameLength = Ubiquitty::Terminfo::hexDecode({ p, static_cast<std::size_t>(tokLength) }, capabilityName);
        std::optional<std::string_view> capability;
        if (nameLength > 0) {
            const std::string_view name { capabilityName, static_cast<std::size_t>(nameLength) };
            // XTGETTCAP RGB is bits per component; terminfo RGB instead changes
            // setaf/setab to packed-color operands, which our indexed entry lacks.
            capability = name == "RGB" ? std::optional<std::string_view> { "8" } : Ubiquitty::Terminfo::lookup(name);
        }
        if (!capability)
            break;
        // A key the entry names answers what it sends now.
        const std::optional<std::string> keyValue = xtgettcapKeyValue(t, Ubiquitty::Terminfo::canonicalName({ capabilityName, static_cast<std::size_t>(nameLength) }));
        const std::string_view value = keyValue ? std::string_view { *keyValue } : *capability;
        reply.append(p, static_cast<std::size_t>(tokLength));
        reply += '=';
        for (const char byte : value) {
            const auto value = static_cast<unsigned char>(byte);
            reply += hexDigits[value >> 4];
            reply += hexDigits[value & 0xF];
        }
        ++answeredCount;
        if (tokLength >= remaining)
            break;
        reply += ';';
        p += tokLength + 1;
        remaining -= tokLength + 1;
    }
    t.parser.osc.length = 0;
    if (answeredCount == 0) {
        (void)respondBulk(t, Ubiquitty::Capability::TerminfoRead, { "\033P0+r\033\\", 7 });
        return;
    }
    reply += "\033\\";
    // The bulk channel, because the reply's length is the request's to decide:
    // at most the request's names plus one entry value each.
    (void)respondBulk(t, Ubiquitty::Capability::TerminfoRead, { reply.data(), reply.size() });
}

// DECRQSS — Request Selection or Setting. Reply DCS 1 $ r <Pt> ST on success,
// DCS 0 $ r ST on an unsupported selector. Pt (accumulated into the OSC accumulator by
// parseDCS) selects: "m" SGR pen, "r" DECSTBM, " q" DECSCUSR, "\"q" DECSCA,
// "\"p" DECSCL, "s" DECSLRM.
static void decrqssDispatch(Ubiquitty::Terminal& t)
{
    // The accumulator holds the Pt selector; null only on an OOM at init (defensive).
    if (!t.parser.osc.data)
        return;
    char* p = t.parser.osc.data;
    int plen = t.parser.osc.length;
    if (plen < 0 || plen >= t.parser.osc.capacity) {
        plen = 0;
    }
    p[plen] = 0;
    // Opened as though the setting will be recognised, so the body is assembled
    // where it is going. A miss starts the reply over rather than moving it.
    Ubiquitty::ReplyBuilder body;
    body.put("\033P1$r");
    // Header: DCS 1 $ r <Pt> = <value> ST  on success.
    // DCS 0 $ r <Pt> ST            on miss.
    bool handled = false;
    if (plen == 1 && p[0] == 'm') {
        // Re-emit the SGR pen as `;`-separated CSI parameters ending in `m`,
        // the attributes in the order xterm writes them (xtermFormatSGR):
        // 1, 4, 5, 7, 8, 2, 3, 9, 21. ECMA-48 leaves the order open.
        // An indexed or direct colour is written in the colon form ITU-T T.416
        // defines, the direct one with its empty colour-space identifier, as
        // xterm writes it (xtermFormatSGR).
        Ubiquitty::SGR::writeParameters(t.screens.active->pen, body);
        body.put("m");
        handled = true;
    } else if (plen == 1 && p[0] == 'r') {
        body.putDecimal(static_cast<int>(static_cast<unsigned>(t.screens.active->scrollTop + 1)));
        body.put(";");
        body.putDecimal(static_cast<int>(static_cast<unsigned>(t.screens.active->scrollBottom + 1)));
        body.put("r");
        handled = true;
    } else if (plen == 2 && p[0] == ' ' && p[1] == 'q') {
        body.putDecimal(static_cast<int>(static_cast<unsigned>(Ubiquitty::Cursor::effectiveStyleCode(t))));
        body.put(" q");
        handled = true;
    } else if (plen == 2 && p[0] == '"' && p[1] == 'q') {
        // DECSCA — 1 if the current pen marks cells selective-erase
        // protected, else 0.
        int sca = t.screens.active->pen.attributes.contains(VT::Attribute::Protected) ? 1 : 0;
        body.putDecimal(static_cast<int>(static_cast<unsigned>(sca)));
        body.put("\"q");
        handled = true;
    } else if (plen == 2 && p[0] == '"' && p[1] == 'p') {
        // DECSCL — operating conformance level (61..65) + the C1 transmission
        // select (Pc: 1 = 7-bit, 2 = 8-bit).
        int pc = t.transmit8bitC1 ? 2 : 1;
        body.putDecimal(static_cast<int>(static_cast<unsigned>(std::to_underlying(t.conformanceLevel))));
        body.put(";");
        body.putDecimal(static_cast<int>(static_cast<unsigned>(pc)));
        body.put("\"p");
        handled = true;
    } else if (plen == 2 && p[0] == '$' && p[1] == '|') {
        // DECSCPP — the columns per page, which is the page's width here
        // (VT510 DECRQSS, DECSCPP).
        body.putDecimal(t.columns);
        body.put("$|");
        handled = true;
    } else if (plen == 1 && p[0] == 's') {
        // DECSLRM — left/right margins, 1-based; full width when unset.
        body.putDecimal(static_cast<int>(static_cast<unsigned>(Ubiquitty::Screen::effectiveLeft(t) + 1)));
        body.put(";");
        body.putDecimal(static_cast<int>(static_cast<unsigned>(Ubiquitty::Screen::effectiveRight(t) + 1)));
        body.put("s");
        handled = true;
    } else {
        // Values reported straight out of Terminal state (DECAC, DECSACE,
        // DECSASD, DECSLPP, DECSNLS, DECSSDT).
        // Reports straight into the body, so a setting that matched is already
        // where it belongs.
        if (decrqssSettingReport(t, { p, static_cast<std::size_t>(plen) }, body) > 0)
            handled = true;
    }
    // A setting that was not recognised, or a body that outgrew the frame,
    // reports the miss instead — a truncated answer is a different answer, not
    // a shorter one, because the host reads whatever follows it as part of it.
    if (!handled || body.isTruncated()) {
        body.reset();
        body.put("\033P0$r");
    }
    body.put("\033\\");
    if (body.isTruncated()) {
        // The body fit to the byte and its terminator did not: the answer is
        // the miss, whole, rather than silence.
        body.reset();
        body.put("\033P0$r\033\\");
    }
    if (body.isSendable())
        respond(t, Ubiquitty::Capability::CursorStateRead, body.span());
    t.parser.osc.length = 0;
}

// DCS sub-protocol dispatch: a sorted-by-(intermediate, final) constexpr table
// (static_asserted). Handlers read the payload from osc.data / the sixel
// accumulator themselves; tmux-passthrough is the fall-back on a table miss.
using DCSHandler = void (*)(Ubiquitty::Terminal&);

static void dcsH_sixel(Ubiquitty::Terminal& t)
{
    sixelFinalize(t);
    sixelAccumulatorReset(t);
}
static void dcsH_decrqss(Ubiquitty::Terminal& t)
{
    if (t.conformanceLevel > VT::ConformanceLevel::VT100)
        decrqssDispatch(t);
}

// Answers a download with the slot it filled, 0 when it filled none.
//
// Sent only when the caller asked something it cannot otherwise learn — a slot
// to be chosen, or a tagged sample to be found — so a program that names its
// own slot and no tag sees exactly the bytes it saw before.
static void sampleAcknowledge(Ubiquitty::Terminal& t, int slot, uint16_t tag)
{
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033P");
    reply.putDecimal(slot);
    reply.put(";0;");
    reply.putDecimal(tag);
    reply.put("+s\033\\");
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::None, reply.span());
}

// DCS Pslot ; Pformat ; Ptag + s <base64> ST — a sample download, DECDLD's
// counterpart for sound (Docs/extensions.md): a WAV, the only Pformat (0),
// decoded to the synth's mono 44.1 kHz and kept in a slot, the number a note
// names on DECPS's sample voice. Bounded before any allocation: the base64
// length bounds the decode, and the bank cuts a sample at ten seconds.
//
// Pslot 0 asks the bank to choose, and Ptag is the program's own name for its
// sample. Both are here because the slot numbers are global to the terminal:
// with the caller picking, two programs reaching for slot 1 overwrite each other
// with nothing said, and a program that reattaches to a session finds the slot
// full with no way to tell its own chime from whatever replaced it. A
// tagged download lands back on the slot its tag already holds, so re-uploading
// is idempotent rather than a second slot spent.
//
// An empty payload clears a slot instead — the way a zero-length DECUDK value
// clears a key — so a program can give its slot back.
static void dcsH_sample(Ubiquitty::Terminal& t)
{
    const int requestedSlot = t.dcsParamCount > 0 ? t.dcsParameters[0] : 0;
    const int format = t.dcsParamCount > 1 ? t.dcsParameters[1] : 0;
    const uint16_t tag = static_cast<uint16_t>(t.dcsParamCount > 2 ? t.dcsParameters[2] : 0);
    if (requestedSlot < 0 || requestedSlot > Ubiquitty::VT::sampleSlotCount || format != 0)
        return;
    const bool wantsAnswer = requestedSlot == 0 || tag != 0;
    // The download is the sound capability's other half: it is the bytes a later
    // play request makes audible, and ten seconds of them per slot.
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::PlaySound)) {
        if (wantsAnswer)
            sampleAcknowledge(t, 0, tag);
        return;
    }
    Ubiquitty::VT::SampleBank& bank = Ubiquitty::ensureSampleBank(t);
    if (!t.parser.osc.data || t.parser.osc.length <= 0) {
        const int slot = requestedSlot > 0 ? requestedSlot : bank.slotForTag(tag);
        bank.clearSlot(slot);
        if (wantsAnswer)
            sampleAcknowledge(t, slot, tag);
        return;
    }
    const std::string_view encoded(t.parser.osc.data, static_cast<std::size_t>(t.parser.osc.length));
    // Room for no byte is no WAV, refused below without a buffer to decode into.
    std::vector<char> raw(Ubiquitty::Codec::base64DecodedCapacity(encoded));
    std::optional<std::span<char>> wav;
    if (!raw.empty())
        wav = Ubiquitty::Codec::base64Decode(encoded, raw);
    std::vector<int16_t> pcm;
    if (!wav || wav->empty() || !Ubiquitty::VT::decodeWav({ reinterpret_cast<const uint8_t*>(wav->data()), wav->size() }, pcm)) {
        if (wantsAnswer)
            sampleAcknowledge(t, 0, tag);
        return;
    }
    // An explicit slot wins; a tagged re-upload returns to where it was;
    // otherwise the bank chooses. A full bank refuses rather than evicting
    // someone else's sample.
    int slot = requestedSlot;
    if (slot == 0)
        slot = bank.slotForTag(tag);
    if (slot == 0)
        slot = bank.firstFreeSlot();
    if (slot == 0) {
        if (wantsAnswer)
            sampleAcknowledge(t, 0, tag);
        return;
    }
    // One tag names one slot: an explicit slot that steals a tag leaves no
    // second claimant behind for slotForTag to find.
    if (tag != 0)
        for (int i = 0; i < Ubiquitty::VT::sampleSlotCount; ++i)
            if (i != slot - 1 && bank.tags[i] == tag)
                bank.clearSlot(i + 1);
    bank.slots[slot - 1] = std::move(pcm);
    bank.tags[slot - 1] = tag;
    ++bank.serial;
    if (wantsAnswer)
        sampleAcknowledge(t, slot, tag);
}
static void dcsH_decrsts(Ubiquitty::Terminal& t) { decrstsApply(t); }

static void actCharacterSetByte(Ubiquitty::Terminal& t, uint8_t b);

// DECRSPS (DCS Ps $ t D…D ST) puts back a presentation state DECRQPSR reported:
// Ps 1 a DECCIR, Ps 2 a DECTABSR (VT510 DECRSPS, DECCIR, DECTABSR). A DECCIR is
// read whole before any of it is applied, so a malformed one changes nothing.
static void decrspsRestoreTabStops(Ubiquitty::Terminal& t, std::string_view stops)
{
    // The stops, 1-based and `/`-separated; a column past the page is skipped
    // (xterm restore_DECTABSR).
    for (const char c : stops)
        if ((c < '0' || c > '9') && c != '/')
            return;
    finishAlignedTabString(t);
    t.tabStops.reset();
    t.tabAlignments.clear();
    t.tabAlignmentTargets.clear();
    int stop = 0;
    const auto setStop = [&] {
        if (stop >= 1 && stop <= t.columns)
            t.tabStops[static_cast<std::size_t>(stop - 1)] = true;
        stop = 0;
    };
    for (const char c : stops) {
        if (c == '/')
            setStop();
        else if (stop <= Ubiquitty::maxColumns)
            stop = stop * 10 + (c - '0');
    }
    setStop();
}

static void decrspsRestoreCursorInformation(Ubiquitty::Terminal& t, std::string_view report)
{
    // Pr ; Pc ; Pp ; Srend ; Satt ; Sflag ; Pgl ; Pgr ; Scss ; Sdesig
    std::array<std::string_view, 10> fields;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const std::size_t semicolon = report.find(';');
        if ((semicolon == std::string_view::npos) != (index == fields.size() - 1))
            return;
        fields[index] = report.substr(0, semicolon);
        report.remove_prefix(semicolon == std::string_view::npos ? report.size() : semicolon + 1);
    }
    const auto number = [](std::string_view field, int maximum) {
        int value = 0;
        if (field.empty())
            return -1;
        for (const char c : field) {
            if (c < '0' || c > '9' || value > maximum)
                return -1;
            value = value * 10 + (c - '0');
        }
        return value <= maximum ? value : -1;
    };
    const auto flags = [](std::string_view field, int mask) {
        return field.size() == 1 && (field[0] & ~mask) == 0x40 ? field[0] & mask : -1;
    };
    const int row = number(fields[0], t.rows);
    const int column = number(fields[1], t.columns);
    const int rendition = flags(fields[3], 0x1F);
    const int attributes = flags(fields[4], 0x01);
    const int sflag = flags(fields[5], 0x0F);
    const int invokedGL = number(fields[6], 3);
    const int invokedGR = number(fields[7], 3);
    const int sizes = flags(fields[8], 0x0F);
    if (row < 1 || column < 1 || number(fields[2], 1) != 1 || rendition < 0 || attributes < 0 || sflag < 0 || invokedGL < 0 || invokedGR < 0 || sizes < 0)
        return;
    // Sdesig: one designation per set, G0 first, each a final optionally
    // preceded by an intermediate.
    std::array<std::pair<uint8_t, uint8_t>, 4> designations {};
    std::string_view designators = fields[9];
    for (auto& designation : designations) {
        if (!designators.empty() && designators[0] >= 0x20 && designators[0] <= 0x2F) {
            designation.first = static_cast<uint8_t>(designators[0]);
            designators.remove_prefix(1);
        }
        if (designators.empty() || designators[0] < 0x30 || designators[0] > 0x7E)
            return;
        designation.second = static_cast<uint8_t>(designators[0]);
        designators.remove_prefix(1);
    }
    if (!designators.empty())
        return;

    Ubiquitty::Screen::State* s = t.screens.active;
    s->cursorY = row - 1;
    s->cursorX = column - 1;
    Pen& pen = s->pen;
    const auto setAttribute = [&](VT::Attribute attribute, bool isSet) {
        if (isSet)
            pen.attributes.add(attribute);
        else
            pen.attributes.remove(attribute);
    };
    setAttribute(VT::Attribute::Bold, rendition & 0x01);
    setAttribute(VT::Attribute::Underline, rendition & 0x02);
    setAttribute(VT::Attribute::Blink, rendition & 0x04);
    setAttribute(VT::Attribute::Reverse, rendition & 0x08);
    setAttribute(VT::Attribute::Hidden, rendition & 0x10);
    setAttribute(VT::Attribute::Protected, attributes & 0x01);
    t.modes.originMode = sflag & 0x01;
    t.singleShift = (sflag & 0x04) ? 3 : ((sflag & 0x02) ? 2 : 0);
    s->wrapPending = sflag & 0x08;
    t.characterSetActive = invokedGL;
    // G2 in GR is the reset invocation, which DECCIR reports for no locking
    // shift at all.
    t.characterSetGR = invokedGR == 2 ? 0 : invokedGR;
    const auto precedingItem = t.parser.precedingItem;
    for (std::size_t slot = 0; slot < designations.size(); ++slot) {
        t.parser.characterSetSelect = static_cast<int>(slot);
        t.parser.is96CharacterSet = (sizes >> slot) & 1;
        t.parser.characterSetIntermediate = designations[slot].first;
        actCharacterSetByte(t, designations[slot].second);
    }
    t.parser.is96CharacterSet = false;
    t.parser.precedingItem = precedingItem;
}

static void dcsH_decrsps(Ubiquitty::Terminal& t)
{
    if (t.dcsParamCount != 1)
        return;
    const std::string_view payload { t.parser.osc.data ? t.parser.osc.data : "", static_cast<std::size_t>(std::max(t.parser.osc.length, 0)) };
    const int selector = t.dcsParamCount > 0 ? t.dcsParameters[0] : 0;
    if (selector == 1)
        decrspsRestoreCursorInformation(t, payload);
    else if (selector == 2)
        decrspsRestoreTabStops(t, payload);
    t.parser.osc.length = 0;
}
// DECAUPSS (DCS Ps ! u Dscs ST) assigns the user-preference supplemental set:
// Ps 0 with `% 5` is DEC Supplemental, Ps 1 with `A` is ISO Latin-1, and any
// other assignment is ignored (DEC STD 070 5.8.2).
static void dcsH_decaupss(Ubiquitty::Terminal& t)
{
    const std::string_view designation { t.parser.osc.data ? t.parser.osc.data : "", static_cast<std::size_t>(std::max(t.parser.osc.length, 0)) };
    t.parser.osc.length = 0;
    if (t.dcsParamCount != 1)
        return;
    const int size = t.dcsParameters[0];
    if (size == 0 && designation == "%5")
        t.modes.userPreferredSupplementalSet = VT::SupplementalSet::DECSupplemental;
    else if (size == 1 && designation == "A")
        t.modes.userPreferredSupplementalSet = VT::SupplementalSet::ISOLatin1;
}
static void dcsH_xtgettcap(Ubiquitty::Terminal& t) { xtgettcapDispatch(t); }

// XTGETXRES reports resources by name in hex (xterm do_dcs, its `case 'Q'`):
// `DCS 1 + R name=value ST`, names joined by `;` and values in hex. The first
// name this terminal does not have ends the reply, echoed with no value, and
// makes the status 0 when it is the first. The resources are the ones Core
// holds an equivalent of, under xterm's names; gated with XTGETTCAP, since
// both disclose configuration.
static void dcsH_xtgetxres(Ubiquitty::Terminal& t)
{
    const std::string_view payload { t.parser.osc.data ? t.parser.osc.data : "", static_cast<std::size_t>(std::max(t.parser.osc.length, 0)) };
    t.parser.osc.length = 0;
    if (!t.allowTerminalCapabilityOperations || payload.empty())
        return;
    static constexpr char hexDigits[] = "0123456789ABCDEF";
    std::string reply = "\033P1+R";
    std::string_view rest = payload;
    bool isFirst = true;
    while (!rest.empty()) {
        const std::string_view token = rest.substr(0, rest.find(';'));
        std::array<char, 64> name {};
        const int nameLength = Ubiquitty::Terminfo::hexDecode(token, name);
        if (nameLength <= 0)
            break;
        const std::string_view resource { name.data(), static_cast<std::size_t>(nameLength) };
        std::string value;
        bool isKnown = true;
        if (resource == "saveLines")
            value = std::to_string(t.scrollbackLimit);
        else if (resource == "allowTcapOps")
            value = t.allowTerminalCapabilityOperations ? "true" : "false";
        else if (resource == "allowWindowOps")
            value = t.allowWindowOperations ? "true" : "false";
        else if (resource == "allowTitleOps")
            value = t.allowTitleOperations ? "true" : "false";
        else if (resource == "allowColorOps")
            value = t.allowColorOperations ? "true" : "false";
        else if (resource == "allowFontOps")
            value = t.allowFontOperations ? "true" : "false";
        else if (resource == "allowMouseOps")
            value = t.allowMouseOperations ? "true" : "false";
        else if (resource == "answerbackString")
            value.assign(t.answerback.data(), t.answerback.size());
        else if (resource == "decTerminalID") {
            // The model DA2 names for the operating level.
            switch (t.conformanceLevel) {
            case VT::ConformanceLevel::VT100:
                value = "100";
                break;
            case VT::ConformanceLevel::VT200:
                value = "220";
                break;
            case VT::ConformanceLevel::VT300:
                value = "320";
                break;
            case VT::ConformanceLevel::VT400:
                value = "420";
                break;
            case VT::ConformanceLevel::VT500:
                value = "520";
                break;
            }
        } else
            isKnown = false;
        if (isFirst && !isKnown)
            reply[2] = '0';
        isFirst = false;
        reply.append(token);
        if (!isKnown)
            break;
        reply += '=';
        for (const char byte : value) {
            reply += hexDigits[static_cast<unsigned char>(byte) >> 4];
            reply += hexDigits[static_cast<unsigned char>(byte) & 0xF];
        }
        if (token.size() == rest.size())
            break;
        reply += ';';
        rest.remove_prefix(token.size() + 1);
    }
    if (isFirst)
        return;
    reply += "\033\\";
    // The bulk channel, because the reply's length is the request's to decide.
    (void)respondBulk(t, Ubiquitty::Capability::TerminfoRead, { reply.data(), reply.size() });
}

// XTSETTCAP selects the terminal description XTGETTCAP answers from, by name in
// hex (xterm do_dcs, its `case 'p'`, and set_termcap). The portable keyboard
// descriptions include Ubiquitty and Linux. Unknown names ring the bell and
// leave the selected description intact. Gated with XTGETTCAP.
static void dcsH_xtsettcap(Ubiquitty::Terminal& t)
{
    const std::string_view payload { t.parser.osc.data ? t.parser.osc.data : "", static_cast<std::size_t>(std::max(t.parser.osc.length, 0)) };
    t.parser.osc.length = 0;
    if (!t.allowTerminalCapabilityOperations)
        return;
    std::array<char, 64> name {};
    const int nameLength = Ubiquitty::Terminfo::hexDecode(payload, name);
    if (nameLength <= 0) {
        Ubiquitty::ringBell(t);
        return;
    }
    const std::string_view decoded { name.data(), static_cast<std::size_t>(nameLength) };
    if (decoded == "linux")
        t.modes.termcapProfile = Keys::TermcapProfile::Linux;
    else if (Ubiquitty::Terminfo::isEntryName(decoded))
        t.modes.termcapProfile = Keys::TermcapProfile::Ubiquitty;
    else
        Ubiquitty::ringBell(t);
}
static void dcsH_tmux(Ubiquitty::Terminal& t) { dcsDispatch(t); }
static int udkHexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Find the live UDK slot for keyCode, or -1.
static int udkFindSlot(const Ubiquitty::VT::UserDefinedKeys& udk, uint16_t keyCode)
{
    for (int i = 0; i < udk.count; ++i)
        if (udk.entries[i].keyCode == keyCode)
            return i;
    return -1;
}

// Program (or clear) one key. value==nullptr clears it; otherwise upserts the
// decoded bytes. A zero-length value clears the key (DECUDK convention).
static void udkSet(Ubiquitty::VT::UserDefinedKeys& udk, uint16_t keyCode, std::string_view value)
{
    int slot = udkFindSlot(udk, keyCode);
    if (value.empty()) {
        if (slot >= 0) {
            // Compact: move the last live entry into the freed slot.
            udk.entries[slot] = udk.entries[udk.count - 1];
            udk.entries[udk.count - 1] = Ubiquitty::VT::UDKEntry {};
            --udk.count;
        }
        return;
    }
    value = value.substr(0, std::min<std::size_t>(value.size(), Ubiquitty::VT::udkMaxValueBytes));
    if (slot < 0) {
        if (udk.count >= Ubiquitty::VT::udkMaxEntries)
            return; // table full — drop the binding
        slot = udk.count++;
        udk.entries[slot].keyCode = keyCode;
    }
    udk.entries[slot].length = static_cast<uint16_t>(value.size());
    std::memcpy(udk.entries[slot].value, value.data(), value.size());
}

// DECUDK — DCS Pc ; Pl | Ky1/St1 ; … ST. Parse the header (Pc clear-mode, Pl
// lock-mode) + `;`-separated Ky/hex-St payload into t->udk (the Input layer reads
// it). Once Pl=0 has locked the keys, further DECUDK is ignored until RIS.
static void dcsH_decudk(Ubiquitty::Terminal& t)
{
    // ProgramKeys-gated: a hostile stream could remap a key to inject input.
    // Default-deny for a denied origin; an unset policy answers PAL's default.
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::ProgramKeys))
        return; // host policy denied key reprogramming
    if (t.udk && t.udk->locked)
        return;
    const int clearMode = (t.dcsParamCount > 0) ? t.dcsParameters[0] : 0;
    const int lockMode = (t.dcsParamCount > 1) ? t.dcsParameters[1] : 0;
    // Pc = 0 (default): clear every definition before loading. An absent table
    // has nothing to clear, and zeroing one is not worth allocating it.
    if (clearMode == 0 && t.udk) {
        t.udk->count = 0;
        for (auto& entry : t.udk->entries)
            entry = Ubiquitty::VT::UDKEntry {};
    }

    const char* p = t.parser.osc.data;
    int remaining = t.parser.osc.length;
    if (remaining < 0)
        remaining = 0;
    int i = 0;
    while (i < remaining) {
        // Parse Ky (decimal key code) up to '/'.
        uint32_t keyCode = 0;
        bool haveKey = false;
        while (i < remaining && p[i] >= '0' && p[i] <= '9') {
            keyCode = keyCode * 10 + static_cast<uint32_t>(p[i] - '0');
            if (keyCode > 0xFFFF)
                keyCode = 0xFFFF;
            haveKey = true;
            ++i;
        }
        if (i < remaining && p[i] == '/') {
            ++i; // consume '/'
            // Decode the hex string St until ';' or end.
            char decoded[Ubiquitty::VT::udkMaxValueBytes];
            int decodedLength = 0;
            while (i + 1 < remaining && p[i] != ';') {
                int hi = udkHexNibble(p[i]);
                int lo = udkHexNibble(p[i + 1]);
                if (hi < 0 || lo < 0)
                    break;
                if (decodedLength < Ubiquitty::VT::udkMaxValueBytes)
                    decoded[decodedLength++] = static_cast<char>((hi << 4) | lo);
                i += 2;
            }
            if (haveKey)
                udkSet(Ubiquitty::ensureUserDefinedKeys(t), static_cast<uint16_t>(keyCode), { decoded, static_cast<std::size_t>(decodedLength) });
        }
        // Skip to the next pair (past a ';' separator or trailing garbage).
        while (i < remaining && p[i] != ';')
            ++i;
        if (i < remaining && p[i] == ';')
            ++i;
    }

    // Pl = 0 (default): lock the keys after loading.
    // A lock is state a later DECUDK reads, so it allocates where a clear does not.
    if (lockMode == 0)
        Ubiquitty::ensureUserDefinedKeys(t).locked = true;
}

// Clear cell `c`'s bitmap and mark it defined. Under Pe=1 a reloaded cell must be
// zeroed before its sixel bits or in, else the new glyph merges with the old.
static void drcsDefineCell(Ubiquitty::VT::SoftFont& sf, int c)
{
    if (c < 0 || c >= Ubiquitty::VT::drcsMaxCells)
        return;
    for (int r = 0; r < Ubiquitty::VT::drcsMaxMatrixHeight; ++r)
        sf.glyph[c][r] = 0;
    sf.glyphSet[c] = true;
}

// DECDLD (DCS Pfn;Pcn;Pe;Pcmw;Pw;Pt;Pcmh;Pss { Dscs Sxbp… ST) — download a soft
// character set (DRCS) into t->softFont; the SCS designator + renderer consume it.
// Glyph bitmap encoding (sixel-style): a data byte 0x3F..0x7E carries 6 vertical
// pixels of one column; successive bytes fill columns within a 6-row band, '/'
// ends the band, ';' ends the glyph. All indices are clamped to the SoftFont
// caps before any write — a hostile DECDLD can't allocate or write out of bounds.
static void dcsH_decdld(Ubiquitty::Terminal& t)
{
    // SetFont-gated: a hostile soft font could spoof other characters.
    // Default-deny for a denied origin; an unset policy answers PAL's default. The config
    // dcs_allow gate in dcsTerminate is the other axis.
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetFont))
        return; // host policy denied the soft-font download

    // (Pfn = dcsParameters[0] font number — one DRCS buffer on VT220, ignored.)
    const int Pcn = (t.dcsParamCount > 1) ? t.dcsParameters[1] : 0; // start cell
    const int Pe = (t.dcsParamCount > 2) ? t.dcsParameters[2] : 0; // erase control
    const int Pcmw = (t.dcsParamCount > 3) ? t.dcsParameters[3] : 0; // matrix width
    // (Pw = dcsParameters[4] set width, Pt = [5] text/full-cell — informational.)
    const int Pcmh = (t.dcsParamCount > 6) ? t.dcsParameters[6] : 0; // matrix height
    const int Pss = (t.dcsParamCount > 7) ? t.dcsParameters[7] : 0; // 1 = 96-set

    // Resolve the matrix dims (0 ⇒ device default) and reject anything past the
    // fixed caps — no wire-driven sizing. VT320 text cell is ~15×12; default 8×12.
    const int mw = (Pcmw > 0) ? Pcmw : 8;
    const int mh = (Pcmh > 0) ? Pcmh : 12;
    if (mw < 1 || mw > Ubiquitty::VT::drcsMaxMatrixWidth || mh < 1 || mh > Ubiquitty::VT::drcsMaxMatrixHeight)
        return; // out-of-range matrix — drop the whole download

    // Validate the Dscs designator before mutating the live SoftFont — a
    // malformed one must reject without disturbing the current set, else the Pe=1
    // keep-existing path leaves old glyphs under the new (rejected) geometry.
    const char* p = t.parser.osc.data;
    int n = t.parser.osc.length;
    if (n < 0)
        n = 0;
    int i = 0;
    // Dscs designator: optional intermediates (0x20..0x2F) then one final
    // (0x30..0x7E). This is the token SCS later matches to invoke the set.
    uint8_t designIntermediate = 0;
    while (i < n && static_cast<unsigned char>(p[i]) >= 0x20 && static_cast<unsigned char>(p[i]) <= 0x2F)
        designIntermediate = static_cast<uint8_t>(p[i++]); // last wins (multi is rare)
    if (i >= n || static_cast<unsigned char>(p[i]) < 0x30 || static_cast<unsigned char>(p[i]) > 0x7E)
        return; // no valid designating final — nothing to bind the set to
    const char designFinal = static_cast<char>(p[i++]);

    // Commit. Pe erase control: 0 = erase this set before load; 2 = erase all
    // (we keep one, identical); 1 = erase only reloaded cells. First load starts clean.
    Ubiquitty::VT::SoftFont& sf = Ubiquitty::ensureSoftFont(t);
    if (Pe != 1 || !sf.valid)
        sf = Ubiquitty::VT::SoftFont {};
    sf.set96 = (Pss == 1);
    sf.matrixWidth = static_cast<uint8_t>(mw);
    sf.matrixHeight = static_cast<uint8_t>(mh);
    sf.designIntermediate = designIntermediate;
    sf.designFinal = designFinal;

    // Glyph bitmaps. `cell` is the 0-based position (byte - 0x20); the first
    // glyph lands at Pcn. DEC reads Pcn 0 and 1 both as the first character of
    // the set, and a 94-set's first character is '!': SPACE is not a member of
    // one, so loading at 0 put the bitmap on every space in the run. A 96-set
    // does hold SPACE, and 0 lands there.
    // '/' = next band, ';' = next cell. drcsDefineCell zeroes each cell first, so a
    // Pe=1 reload REPLACES rather than merges onto stale bits.
    int cell = (Pcn < 0) ? 0 : Pcn;
    if (Pss != 1 && cell == 0)
        cell = 1;
    int col = 0;
    int band = 0;
    if (i < n)
        drcsDefineCell(sf, cell);
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(p[i++]);
        if (c == ';') {
            ++cell;
            col = 0;
            band = 0;
            if (cell >= Ubiquitty::VT::drcsMaxCells)
                break; // set full — drop the rest
            drcsDefineCell(sf, cell);
        } else if (c == '/') {
            col = 0;
            ++band;
        } else if (c >= 0x3F && c <= 0x7E) {
            if (cell < Ubiquitty::VT::drcsMaxCells && col < sf.matrixWidth) {
                const unsigned bits = static_cast<unsigned>(c - 0x3F); // 6 px column
                for (int b = 0; b < 6; ++b) {
                    const int row = band * 6 + b;
                    // row >= 0 is always true today (band is bounded by the DCS
                    // buffer cap), but keep it as a belt so a future larger cap
                    // can't let band*6 overflow into a negative index (conventions).
                    if (row >= 0 && row < sf.matrixHeight && (bits & (1u << b)) != 0)
                        sf.glyph[cell][row] |= static_cast<uint16_t>(1u << col);
                }
            }
            ++col;
        }
        // else: ignore (whitespace / unexpected byte within the data string)
    }
    sf.valid = true;
    // Every commit path funnels through here (the erase-first wipe above zeroes the
    // old serial), so one take is enough to give the new set an identity the shared
    // glyph atlas can key on and to retire the previous set's rasterisations.
    sf.serial = Ubiquitty::VT::nextSoftFontSerial();
}

struct DCSEntry {
    unsigned char intermediate; // 0 if none
    char final;
    DCSHandler fn;
};

static constexpr unsigned dcsKey(unsigned char i, char f) { return (static_cast<unsigned>(i) << 8) | static_cast<unsigned char>(f); }

// Sorted by (intermediate, final).
static constexpr DCSEntry dcsHandlers[] = {
    { 0, 'q', dcsH_sixel },
    { 0, '{', dcsH_decdld }, // 'q'=0x71 < '{'=0x7B < '|'=0x7C — stays sorted
    { 0, '|', dcsH_decudk },
    { '!', 'u', dcsH_decaupss }, // DECAUPSS — '!'=0x21 < '$'=0x24, stays sorted
    { '$', 'p', dcsH_decrsts }, // DECRSTS — 'p'=0x70 < 'q'=0x71, stays sorted
    { '$', 'q', dcsH_decrqss },
    { '$', 't', dcsH_decrsps }, // DECRSPS — 'q'=0x71 < 't'=0x74, stays sorted
    { '+', 'Q', dcsH_xtgetxres }, // 'Q'=0x51 < 'p'=0x70 < 'q'=0x71, stays sorted
    { '+', 'p', dcsH_xtsettcap },
    { '+', 'q', dcsH_xtgettcap },
    { '+', 's', dcsH_sample },
};

template<size_t N> constexpr bool dcsTableSorted(const DCSEntry (&a)[N], size_t i = 1) { return (i >= N) || (dcsKey(a[i - 1].intermediate, a[i - 1].final) < dcsKey(a[i].intermediate, a[i].final) && dcsTableSorted(a, i + 1)); }
static_assert(dcsTableSorted(dcsHandlers), "dcsHandlers must be sorted by (intermediate, final)");

static const DCSEntry* dcsLookup(unsigned char intermediate, char final)
{
    unsigned want = dcsKey(intermediate, final);
    int lo = 0;
    int hi = static_cast<int>(sizeof(dcsHandlers) / sizeof(dcsHandlers[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        unsigned got = dcsKey(dcsHandlers[mid].intermediate, dcsHandlers[mid].final);
        if (got == want)
            return &dcsHandlers[mid];
        if (got < want)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return nullptr;
}

// Reset DCS state at every dispatch boundary (terminator, mid-DCS interruption,
// hard reset) so no field leaks into the next sequence.
void dcsStateReset(Ubiquitty::Terminal& t)
{
    t.isSixelDCS = false;
    t.dcsHeaderDone = false;
    t.dcsTruncated = false;
    t.dcsIntermediate = 0;
    t.dcsFinal = 0;
    t.dcsParamCount = 0;
    t.didDCSParametersOverflow = false;
    t.dcsParameters.fill(0);
}

// Map a completed DCS's (intermediate, final) to its stable OSC::DCSKind for
// the per-DCS allowlist. A table miss is the tmux passthrough.
static Ubiquitty::OSC::DCSKind dcsKindFor(unsigned char intermediate, char final)
{
    using K = Ubiquitty::OSC::DCSKind;
    if (intermediate == 0 && final == 'q')
        return K::Sixel;
    if (intermediate == 0 && final == '|')
        return K::DECUDK;
    // DECRSPS shares DECRSTS's gate: both put back host-supplied state.
    if (intermediate == '$' && (final == 'p' || final == 't'))
        return K::DECRSTS;
    // DECAUPSS assigns host-supplied terminal state, and shares that gate.
    if (intermediate == '!' && final == 'u')
        return K::DECRSTS;
    if (intermediate == '$' && final == 'q')
        return K::DECRQSS;
    // XTSETTCAP and XTGETXRES share XTGETTCAP's gate: the terminal-capability
    // operations xterm's allowTcapOps governs, and the resource query beside them.
    if (intermediate == '+' && (final == 'q' || final == 'p' || final == 'Q'))
        return K::XTGETTCAP;
    if (intermediate == '+' && final == 's')
        return K::Sample;
    if (intermediate == 0 && final == '{')
        return K::DECDLD;
    return K::Tmux;
}

// Finalise an in-flight DCS frame on terminator. Looks up the
// sub-protocol handler in dcsHandlers using (intermediate, final);
// misses fall through to the tmux-passthrough handler.
static void dcsTerminate(Ubiquitty::Terminal& t)
{
    if (t.dcsIntermediate == 0x7F) {
        dcsStateReset(t);
        return;
    }
    // Per-DCS allowlist: a blocked sub-protocol is parsed + consumed but
    // never dispatched (no hook, no response, no passthrough). Empty lists (the
    // default) pass everything, so this is inert until a user opts in.
    if (!Ubiquitty::OSC::dcsAllowed(t, dcsKindFor(t.dcsIntermediate, static_cast<char>(t.dcsFinal)))) {
        dcsStateReset(t);
        return;
    }
    const DCSEntry* e = dcsLookup(t.dcsIntermediate, static_cast<char>(t.dcsFinal));
    if (e)
        e->fn(t);
    else
        dcsH_tmux(t);
    dcsStateReset(t);
}

// Legacy non-ANSI emulation grammars (Wyse / Televideo / ADM / Heath / HP).
// These predate ANSI X3.64: cursor addressing is `ESC = row col` (biased-byte
// pair, not CSI), erase is single ESC finals, display attributes are `ESC G n`.
// They reuse the same screen primitives as the VT path; only the grammar differs.
// legacyState: 0 ground, 1 saw ESC, 2 ESC = got row (awaiting column;
// legacyAddrRow holds row), 3 ESC = awaiting row.

// Place the cursor for an ESC = row col address. Both bytes are biased by 0x20
// on the wire (' ' == 0), matching VT52 ESC Y and the Wyse/Televideo/ADM convention.
static void legacyAddressCursor(Ubiquitty::Terminal& t, uint8_t rowByte, uint8_t columnByte)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int row = static_cast<int>(rowByte) - 0x20;
    int column = static_cast<int>(columnByte) - 0x20;
    if (row < 0)
        row = 0;
    if (row > t.rows - 1)
        row = t.rows - 1;
    if (column < 0)
        column = 0;
    if (column > t.columns - 1)
        column = t.columns - 1;
    s->cursorY = row;
    s->cursorX = column;
    s->wrapPending = false;
}

// ESC G n display-attribute region (Wyse / Televideo). `n`'s low nibble is a
// bitmask (bit0 blink, bit1 reverse, bit2 dim, bit3 underline); '0' clears all.
// Subsequent printables inherit the active pen.
static void legacyApplyAttribute(Ubiquitty::Terminal& t, uint8_t n)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const unsigned mask = static_cast<unsigned>(n) & 0x0Fu;
    VT::AttributeSet attributes;
    if (mask & 0x01u)
        attributes.add(VT::Attribute::Blink);
    if (mask & 0x02u)
        attributes.add(VT::Attribute::Reverse);
    if (mask & 0x04u)
        attributes.add(VT::Attribute::Dim);
    if (mask & 0x08u)
        attributes.add(VT::Attribute::Underline);
    s->pen.attributes = attributes;
}

// Common C0 control handling shared by every legacy grammar. Returns true
// when the byte was a control it consumed.
static bool legacyControl(Ubiquitty::Terminal& t, uint8_t c)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    switch (c) {
    case 0x07: // BEL
        Ubiquitty::ringBell(t);
        return true;
    case 0x08: // BS
        if (s->cursorX > 0)
            s->cursorX--;
        s->wrapPending = false;
        return true;
    case 0x09: // HT — advance to next 8-col tab stop (legacy fixed tabs)
        s->cursorX = (s->cursorX & ~7) + 8;
        if (s->cursorX > t.columns - 1)
            s->cursorX = t.columns - 1;
        return true;
    case 0x0A: // LF
    case 0x0B: // VT
    case 0x0C: // FF
        linefeed(t);
        return true;
    case 0x0D: // CR — under DECLRMM moves to the left margin when at/right of it,
        // else column 0 (Ubiquitty::Screen::effectiveLeft is 0 when DECLRMM is off).
        s->cursorX = Ubiquitty::Screen::returnColumn(t);
        s->wrapPending = false;
        return true;
    default:
        return false;
    }
}

// Single-byte ESC final dispatch shared by Wyse / Televideo / ADM (their core
// cursor-motion + erase grammar; their function-key and status extensions are
// not decoded, so those finals fall through to the unknown-final path).
static void legacyEscapeFinal(Ubiquitty::Terminal& t, uint8_t c)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    switch (c) {
    case '=': // load cursor address: next two bytes are row, column
        t.legacyState = 3; // await row byte
        return;
    case 'G': // Wyse/Televideo: ESC G n display-attribute region. The
              // attribute byte follows; stash a sub-state to grab it.
        if (t.emulation == VT::Emulation::Wyse || t.emulation == VT::Emulation::Televideo) {
            t.legacyState = 4; // await attribute byte
            return;
        }
        break;
    case 'T': // clear to end of line (Televideo / Wyse)
        Ubiquitty::Screen::eraseInLine(t, 0);
        break;
    case 'Y': // clear to end of screen (Televideo / Wyse)
        Ubiquitty::Screen::eraseInDisplay(t, 0);
        break;
    case ';': // ADM-3A / Televideo: home cursor
    case 'H':
        s->cursorX = 0;
        s->cursorY = 0;
        s->wrapPending = false;
        break;
    case '*': // clear screen + home (Televideo / Wyse)
    case '+':
        Ubiquitty::Screen::eraseInDisplay(t, 2);
        s->cursorX = 0;
        s->cursorY = 0;
        s->wrapPending = false;
        break;
    case ':': // Televideo: clear unprotected → full clear here, home
        Ubiquitty::Screen::eraseInDisplay(t, 2);
        s->cursorX = 0;
        s->cursorY = 0;
        s->wrapPending = false;
        break;
    default:
        // Unmapped legacy escape — an unrecognised final is dropped, never
        // printed as text.
        break;
    }
    t.legacyState = 0;
}

// The non-ANSI byte driver, routing by legacyState.
static void legacyProcessByte(Ubiquitty::Terminal& t, uint8_t c)
{
    switch (t.legacyState) {
    case 0: // ground
        if (c == 0x1B) {
            t.legacyState = 1;
            return;
        }
        if (legacyControl(t, c))
            return;
        if (c >= 0x20)
            putCharacter(t, c);
        return;
    case 1: // saw ESC
        t.legacyState = 0;
        legacyEscapeFinal(t, c);
        return;
    case 3: // ESC = awaiting row byte
        t.legacyAddrRow = c;
        t.legacyState = 2;
        return;
    case 2: // ESC = row received, this is the column byte
        legacyAddressCursor(t, t.legacyAddrRow, c);
        t.legacyState = 0;
        return;
    case 4: // ESC G awaiting attribute byte
        legacyApplyAttribute(t, c);
        t.legacyState = 0;
        return;
    default:
        t.legacyState = 0;
        return;
    }
}

// True when the emulation routes through legacyProcessByte rather than the
// DEC DFA. Linux/Sco stay on the VT path (they are ANSI); only the
// non-ANSI grammars divert here.
static inline bool emulationIsNonANSI(const Ubiquitty::Terminal& t)
{
    switch (t.emulation) {
    case VT::Emulation::Wyse:
    case VT::Emulation::Televideo:
    case VT::Emulation::Adm:
    case VT::Emulation::Heath:
    case VT::Emulation::Hp:
        return true;
    default:
        return false;
    }
}

// Action handlers — one named function per Action enum value, gathered in
// actionHandlers below. Uniform signature (Terminal*, uint8_t byte) so the
// dispatcher is a single fn-ptr call.
using ActionHandler = void (*)(Ubiquitty::Terminal&, uint8_t);

static void actNoop(Ubiquitty::Terminal&, uint8_t) { }

// Ground actions.
static void actPrint(Ubiquitty::Terminal& t, uint8_t b) { putCharacter(t, b); }
static void actExecuteBell(Ubiquitty::Terminal& t, uint8_t b)
{
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    Ubiquitty::ringBell(t);
}
// ENQ (0x05) — transmit the configured answerback string (empty = no reply).
static void actExecuteAnswerback(Ubiquitty::Terminal& t, uint8_t b)
{
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    if (!t.answerback.empty())
        respond(t, Ubiquitty::Capability::Answerback, t.answerback.c_str());
}
static void actExecuteBackspace(Ubiquitty::Terminal& t, uint8_t b)
{
    finishAlignedTabString(t);
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    if (statusCursorMoveBy(t, t.shouldWriteRightToLeft ? 1 : -1))
        return;
    // BS moves opposite the writing direction; CUB itself is unaffected.
    if (t.shouldWriteRightToLeft)
        cursorForward(t, 1);
    else
        cursorBackward(t, 1);
}
static void actExecuteHorizontalTab(Ubiquitty::Terminal& t, uint8_t b)
{
    finishAlignedTabString(t);
    resetUTF8(t);
    if (statusTab(t))
        return;
    Ubiquitty::Screen::State* s = t.screens.active;
    t.parser.precedingItem.setControl(b);
    // more(1) fix (DECSET 41): with the mode set, HT at an armed deferred wrap
    // commits the wrap first, so the tab lands on the next row's first stop.
    // Reset, the tab pins at the right margin and the latch stays armed.
    if (t.moreFix && s->wrapPending) {
        s->wrapPending = false;
        linefeed(t);
        s->cursorX = 0;
    }
    int rightBound = effectiveRightVisible(t);
    int x = s->cursorX + 1;
    while (x <= rightBound && !t.tabStops[x])
        x++;
    if (x > rightBound)
        x = rightBound;
    s->cursorX = x;
    beginAlignedTabString(t);
}
void finishLineSemanticRole(Ubiquitty::Terminal& terminal)
{
    if (!terminal.shouldEndSemanticRoleAtLineBreak || terminal.screens.isUsingAlternateScreen)
        return;
    finishAlignedTabString(terminal);
    terminal.screens.active->pen.semanticRole = SemanticRole::Output;
    terminal.shouldEndSemanticRoleAtLineBreak = false;
    terminal.isReadingCommand = false;
}

static void actExecuteLineFeed(Ubiquitty::Terminal& t, uint8_t b)
{
    finishAlignedTabString(t);
    finishLineSemanticRole(t);
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    if (t.modes.lnm)
        t.screens.active->cursorX = Screen::returnColumn(t);
    linefeed(t);
    if (t.modes.lnm && t.shouldWriteRightToLeft)
        t.screens.active->cursorX = Screen::returnColumn(t);
    Screen::noteStringStart(t);
}
static void actExecuteCarriageReturn(Ubiquitty::Terminal& t, uint8_t b)
{
    finishAlignedTabString(t);
    resetUTF8(t);
    if (statusCursorMoveTo(t, t.shouldWriteRightToLeft ? t.columns - 1 : 0))
        return;
    Ubiquitty::Screen::State* s = t.screens.active;
    t.parser.precedingItem.setControl(b);
    // Under DECLRMM, CR moves to the left margin when the cursor is at/right of it,
    // else to column 0. Ubiquitty::Screen::effectiveLeft is 0 when DECLRMM is off, so this stays a
    // plain column-0 CR there).
    s->cursorX = Ubiquitty::Screen::returnColumn(t);
    s->wrapPending = false;
}
static void actExecuteShiftOut(Ubiquitty::Terminal& t, uint8_t b)
{
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    t.characterSetActive = 1;
}
static void actExecuteShiftIn(Ubiquitty::Terminal& t, uint8_t b)
{
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    t.characterSetActive = 0;
}
static void actEnterEscape(Ubiquitty::Terminal& t, uint8_t) { resetUTF8(t); }

// C1 8-bit controls — equivalent to ESC <X> for the chosen byte.
static void actC1Index(Ubiquitty::Terminal& t, uint8_t b)
{
    finishAlignedTabString(t);
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    linefeed(t);
}
static void actC1NextLine(Ubiquitty::Terminal& t, uint8_t b)
{
    finishAlignedTabString(t);
    finishLineSemanticRole(t);
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    linefeed(t);
    t.screens.active->cursorX = Screen::returnColumn(t);
    Screen::noteStringStart(t);
}
static void actC1HorizontalTabSet(Ubiquitty::Terminal& t, uint8_t b)
{
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    t.tabStops[t.screens.active->cursorX] = true;
    if (!t.tabAlignments.empty()) {
        t.tabAlignments[t.screens.active->cursorX] = CharacterTabAlignment::Plain;
        if (!t.tabAlignmentTargets.empty())
            t.tabAlignmentTargets[t.screens.active->cursorX] = 0;
    }
}
static void actC1VerticalTabSet(Ubiquitty::Terminal& t, uint8_t b)
{
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    escapeH_vts(t, 'J');
}

static void actC1HorizontalTabJustify(Ubiquitty::Terminal& t, uint8_t b)
{
    t.parser.precedingItem.setControl(b);
    Screen::justifyActiveField(t);
}
static void actC1ReverseIndex(Ubiquitty::Terminal& t, uint8_t b)
{
    finishAlignedTabString(t);
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    reverseIndex(t);
    Screen::noteStringStart(t);
}
static void actC1EnterDCS(Ubiquitty::Terminal& t, uint8_t)
{
    resetUTF8(t);
    t.parser.precedingItem.setNone();
    t.parser.osc.length = 0;
    t.images.sixelAccumulatorLength = 0;
    t.images.sixelAccumulatorOverflow = false;
    dcsStateReset(t);
}
static void actC1EnterCSI(Ubiquitty::Terminal& t, uint8_t)
{
    resetUTF8(t);
    resetCSI(t);
}
static void actC1EnterOSC(Ubiquitty::Terminal& t, uint8_t)
{
    resetUTF8(t);
    t.parser.precedingItem.setNone();
    t.parser.osc.length = 0;
}
static void actC1EnterAPC(Ubiquitty::Terminal& t, uint8_t)
{
    resetUTF8(t);
    t.parser.precedingItem.setNone();
    t.images.kittyAPC.length = 0;
    t.images.kittyAPCOverflow = false;
}
static void actC1SingleShift2(Ubiquitty::Terminal& t, uint8_t b)
{
    t.parser.precedingItem.setControl(b);
    t.singleShift = 2;
}
static void actC1SingleShift3(Ubiquitty::Terminal& t, uint8_t b)
{
    t.parser.precedingItem.setControl(b);
    t.singleShift = 3;
}
static void actC1StartGuardedArea(Ubiquitty::Terminal& t, uint8_t b)
{
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    escapeH_spa(t, 'V');
}
static void actC1EndGuardedArea(Ubiquitty::Terminal& t, uint8_t b)
{
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    escapeH_epa(t, 'W');
}
static void actC1Identify(Ubiquitty::Terminal& t, uint8_t b)
{
    resetUTF8(t);
    t.parser.precedingItem.setControl(b);
    emitPrimaryDA(t);
}

// ESC state entries.
static void actEnterCSI(Ubiquitty::Terminal& t, uint8_t) { resetCSI(t); }
static void actEnterOSC(Ubiquitty::Terminal& t, uint8_t)
{
    t.parser.precedingItem.setNone();
    t.parser.osc.length = 0;
}
static void actEnterDCS(Ubiquitty::Terminal& t, uint8_t)
{
    t.parser.precedingItem.setNone();
    t.parser.osc.length = 0;
    t.images.sixelAccumulatorLength = 0;
    t.images.sixelAccumulatorOverflow = false;
    dcsStateReset(t);
}
static void actEnterAPC(Ubiquitty::Terminal& t, uint8_t)
{
    t.parser.precedingItem.setNone();
    t.images.kittyAPC.length = 0;
    t.images.kittyAPCOverflow = false;
}
static void actEnterCharacterSetG0(Ubiquitty::Terminal& t, uint8_t)
{
    t.parser.characterSetSelect = 0;
    t.parser.is96CharacterSet = false;
    t.parser.characterSetIntermediate = 0;
}
static void actEnterCharacterSetG1(Ubiquitty::Terminal& t, uint8_t)
{
    t.parser.characterSetSelect = 1;
    t.parser.is96CharacterSet = false;
    t.parser.characterSetIntermediate = 0;
}
static void actEnterCharacterSetG2(Ubiquitty::Terminal& t, uint8_t)
{
    t.parser.characterSetSelect = 2;
    t.parser.is96CharacterSet = false;
    t.parser.characterSetIntermediate = 0;
}
static void actEnterCharacterSetG3(Ubiquitty::Terminal& t, uint8_t)
{
    t.parser.characterSetSelect = 3;
    t.parser.is96CharacterSet = false;
    t.parser.characterSetIntermediate = 0;
}
// 96-set designation (ESC - / . / /) — into G1/G2/G3 only (ECMA-35: G0
// holds a 94-set). The final byte is read by actCharacterSetByte's 96-set arm.
static void actEnterCharacterSet96G1(Ubiquitty::Terminal& t, uint8_t)
{
    t.parser.characterSetSelect = 1;
    t.parser.is96CharacterSet = true;
    t.parser.characterSetIntermediate = 0;
}
static void actEnterCharacterSet96G2(Ubiquitty::Terminal& t, uint8_t)
{
    t.parser.characterSetSelect = 2;
    t.parser.is96CharacterSet = true;
    t.parser.characterSetIntermediate = 0;
}
static void actEnterCharacterSet96G3(Ubiquitty::Terminal& t, uint8_t)
{
    t.parser.characterSetSelect = 3;
    t.parser.is96CharacterSet = true;
    t.parser.characterSetIntermediate = 0;
}
// Accumulate the DRCS designator intermediate byte (ESC ( <0x20..0x2F>) so the
// final-byte handler can match it against the downloaded soft font's designator.
static void actCharacterSetIntermediateByte(Ubiquitty::Terminal& t, uint8_t b) { t.parser.characterSetIntermediate = b; }
static void actEscapeDispatch(Ubiquitty::Terminal& t, uint8_t b) { escDispatch(t, b == 0x94 ? 'T' : static_cast<char>(b)); }

// CSI accumulation + dispatch.
static void actCSIPrivatePrefix(Ubiquitty::Terminal& t, uint8_t b)
{
    if (t.parser.parameterCount == 0 && t.parser.privatePrefix == 0 && t.parser.intermediate == 0)
        t.parser.privatePrefix = static_cast<char>(b);
    else
        markCSIMalformed(t);
}
bool parameterOmitted(const Ubiquitty::Terminal& t, int index)
{
    if (index < 0 || index >= t.parser.parameterCount)
        return true;
    return (t.parser.parameterWasEmpty & (1u << index)) != 0;
}

static void actCSIParameterDigit(Ubiquitty::Terminal& t, uint8_t b)
{
    if (t.parser.intermediate != 0)
        markCSIMalformed(t);
    // Past the last slot the digits belong to a parameter this terminal does
    // not keep, and the slot they would reach is the last one it does. DEC STD
    // 070 3.5.3.1 lets a terminal ignore parameters past its limit, so they are
    // dropped. xterm instead runs them into its last slot (charproc.c
    // CASE_ESC_DIGIT), which rewrites a parameter the program did send.
    if (t.parser.didParametersOverflow)
        return;
    if (t.parser.parameterCount == 0)
        t.parser.parameterCount = 1;
    t.parser.parameterWasEmpty &= ~(1u << (t.parser.parameterCount - 1));
    t.parser.parameters[t.parser.parameterCount - 1] = decimalAppendSaturated(t.parser.parameters[t.parser.parameterCount - 1], b - '0', VT::maxParameterValue);
}
static void actCSINewParameter(Ubiquitty::Terminal& t, uint8_t sep)
{
    if (t.parser.intermediate != 0)
        markCSIMalformed(t);
    if (t.parser.parameterCount >= VT::maxParameters)
        t.parser.didParametersOverflow = true;
    if (t.parser.parameterCount < VT::maxParameters) {
        if (t.parser.parameterCount == 0)
            t.parser.parameterCount = 1;
        t.parser.parameterCount++;
        t.parser.parameters[t.parser.parameterCount - 1] = 0;
        t.parser.parameterSeparatorWasColon[t.parser.parameterCount - 1] = (sep == ':') ? 1 : 0;
    }
}
static void actCSIIntermediate(Ubiquitty::Terminal& t, uint8_t b)
{
    // Intermediate bytes jointly identify the function (ECMA-48 5.4).
    // Core implements only single-intermediate functions; a suffix must not
    // turn an unknown multi-intermediate sequence into a recognised control.
    if (t.parser.intermediate != 0)
        markCSIMalformed(t);
    t.parser.intermediate = static_cast<char>(b);
}
static void actCSIDispatch(Ubiquitty::Terminal& t, uint8_t b) { csiDispatch(t, static_cast<char>(b)); }

// How many bytes of its string the OSC being accumulated keeps, decided by its
// command: the clipboard (52) and iTerm2's images and files (1337) keep
// maxOSCImageSize, every other command maxOSCNonImageSize, so a runaway title
// cannot grow the shared buffer to the image ceiling. DCS payloads have their
// own ceiling.
//
// The command is the number the string begins with, and a read can end anywhere
// in it, so the number is read through the stored bytes and on into `incoming`,
// the bytes about to be appended. Read from the stored bytes alone, the ceiling
// would depend on where the reads fell: a run carrying the whole number would be
// clipped at the non-image ceiling, and the same bytes cut after the number
// would not.
//
// At most maxOSCPsDigits + 1 bytes are read. A further digit means a number no
// ceiling here routes on, and reading on would make an all-digit payload cost
// the square of its length, since this runs on every append. Digits that end at
// the bound are a complete number, so 00000052 is still the clipboard. A number
// that runs out of bytes is the whole number so far; the bytes that end it
// arrive with a later append, before any byte a ceiling could clip.
static int maxOSCBufferBytes(const Ubiquitty::Terminal& t, std::string_view incoming)
{
    const std::string_view stored(t.parser.osc.data, static_cast<std::size_t>(t.parser.osc.length));
    constexpr auto maxDigits = static_cast<std::size_t>(VT::maxOSCPsDigits);
    int ps = 0;
    for (std::size_t index = 0; index <= maxDigits; ++index) {
        char c;
        if (index < stored.size())
            c = stored[index];
        else if (index - stored.size() < incoming.size())
            c = incoming[index - stored.size()];
        else
            break;
        if (c < '0' || c > '9')
            break;
        if (index == maxDigits)
            return VT::maxOSCNonImageSize;
        ps = ps * 10 + (c - '0');
    }
    return (ps == 52 || ps == 1337) ? VT::maxOSCImageSize : VT::maxOSCNonImageSize;
}

// Grow osc.data to hold `need` bytes (doubling, up to maxOSCBufferSize).
// Returns false at the hard cap or on OOM — the caller drops the byte.
static bool reserveOSCBuffer(Ubiquitty::Terminal& t, int need)
{
    if (need <= t.parser.osc.capacity)
        return true;
    if (t.parser.osc.capacity >= VT::maxOSCBufferSize)
        return false;
    int capacity = t.parser.osc.capacity > 0 ? t.parser.osc.capacity : VT::oscBufferInitialCapacity;
    while (capacity < need && capacity < VT::maxOSCBufferSize)
        capacity <<= 1;
    if (capacity > VT::maxOSCBufferSize)
        capacity = VT::maxOSCBufferSize;
    if (capacity < need)
        return false;
    char* nb = static_cast<char*>(std::realloc(t.parser.osc.data, static_cast<size_t>(capacity)));
    if (!nb)
        return false;
    // Zero past what was ever written: a control string may read what an
    // earlier, longer one left after its own end (OSC::resetIndexedColor),
    // and stops at the first zero.
    const int previousCapacity = t.parser.osc.data ? t.parser.osc.capacity : 0;
    std::memset(nb + previousCapacity, 0, static_cast<size_t>(capacity - previousCapacity));
    t.parser.osc.data = nb;
    t.parser.osc.capacity = capacity;
    return true;
}

// Linux palette commands complete at their fixed length, without a terminator.
static bool beginsLinuxConsoleOSC(const Ubiquitty::Terminal& t, uint8_t next)
{
    const char first = t.parser.osc.length > 0 ? t.parser.osc.data[0] : static_cast<char>(next);
    return first == 'P' || first == 'R';
}

static bool linuxConsoleOSCPut(Ubiquitty::Terminal& t, uint8_t b)
{
    // Distinguished by the FIRST payload byte: 'P' (set) or 'R' (reset); any
    // other leading byte is an ordinary numeric OSC — leave it to the generic path.
    if (t.parser.osc.length == 0 && b != 'P' && b != 'R')
        return false;
    if (t.parser.osc.length > 0 && t.parser.osc.data[0] != 'P' && t.parser.osc.data[0] != 'R')
        return false;
    if (t.parser.osc.length >= 9)
        return true;

    if (!reserveOSCBuffer(t, t.parser.osc.length + 2))
        return true; // claimed, but out of buffer — drop the byte
    t.parser.osc.data[t.parser.osc.length++] = b;

    const bool hasITermSelector = t.parser.osc.length >= 2 && t.parser.osc.data[1] >= 'g' && t.parser.osc.data[1] <= 'm';
    if (t.parser.osc.data[0] == 'R' || (t.parser.osc.length == 8 && !hasITermSelector)) {
        Ubiquitty::OSC::dispatchFixedPalette(t, std::string_view(t.parser.osc.data, static_cast<std::size_t>(t.parser.osc.length)));
        t.parser.osc.length = 0;
        t.parser.parseState = ParserState::Ground;
    }
    return true;
}

static void actOSCPut(Ubiquitty::Terminal& t, uint8_t b)
{
    if (linuxConsoleOSCPut(t, b))
        return;
    const char byte = static_cast<char>(b);
    const int ceiling = maxOSCBufferBytes(t, std::string_view(&byte, 1));
    if (t.parser.osc.length >= ceiling)
        return; // past this OSC command's ceiling — clip further bytes
    if (!reserveOSCBuffer(t, t.parser.osc.length + 2))
        return; // byte + trailing NUL
    t.parser.osc.data[t.parser.osc.length++] = b;
    if (t.parser.osc.length == ceiling) // one-shot at the boundary
        Diagnostics::diagLog(t, "osc-clipped", static_cast<uint32_t>(ceiling));
}
// Release an image-sized buffer back to baseline so one large inline image
// doesn't retain megabytes; common multi-KB payloads keep their allocation.
static void releaseLargeOSCBuffer(Ubiquitty::Terminal& t)
{
    if (t.parser.osc.capacity > 64 * 1024) {
        char* nb = static_cast<char*>(std::realloc(t.parser.osc.data, VT::oscBufferInitialCapacity));
        if (nb) {
            t.parser.osc.data = nb;
            t.parser.osc.capacity = VT::oscBufferInitialCapacity;
        }
    }
}
static void actOSCEnd(Ubiquitty::Terminal& t, uint8_t terminator)
{
    if (t.parser.osc.capacity <= 0)
        return;
    t.parser.osc.data[t.parser.osc.length] = 0;
    // A reply ends with the terminator its request ended with (xterm do_osc).
    t.parser.isOSCRequestBellTerminated = terminator == 0x07;
    Ubiquitty::OSC::dispatch(t);
    t.parser.isOSCRequestBellTerminated = false;
    releaseLargeOSCBuffer(t);
}
// An ESC inside an OSC that is not followed by the string terminator abandons
// the string: ECMA-48 8.3.89 ends a command string only with ST, and xterm
// carries out an OSC only on its terminator (charproc.c doparsing), so a string
// cut short is never dispatched. The parser is already in the escape state, so
// the byte after the ESC is read as the start of that escape rather than
// swallowed into a string that would otherwise take every later byte.
static void actOSCEscapeOther(Ubiquitty::Terminal& t, uint8_t b)
{
    t.parser.osc.length = 0;
    releaseLargeOSCBuffer(t);
    processByteInner(t, b);
}

static int appendOSCPayloadRun(Ubiquitty::Terminal& t, const char* buffer, int start, int length)
{
    const bool doesC1Terminate = c1StringTerminatorEnabled(t);
    const int i = VT::controlStringTerminatorIndex(buffer, start, length, doesC1Terminate);
    const int scanned = i - start;
    if (scanned <= 0)
        return 0;
    // Per-command ceiling (mirrors actOSCPut's per-byte clip on this batched run).
    // No command keeps less than maxOSCNonImageSize, so a run that ends below it
    // is stored whole without reading the command, which is every run of a short
    // title or mark. A run that can reach a ceiling reads the number through
    // itself, so that a number this read completes names the command. Clipped
    // bytes are still consumed (returned in `scanned`) so the parser advances past
    // them. The stored length never passes the ceiling, so the clamp cannot wrap.
    static_assert(VT::maxOSCImageSize > VT::maxOSCNonImageSize);
    int ceiling = VT::maxOSCNonImageSize;
    if (scanned >= ceiling - t.parser.osc.length)
        ceiling = maxOSCBufferBytes(t, std::string_view(buffer + start, static_cast<std::size_t>(scanned)));
    int store = scanned;
    if (store > ceiling - t.parser.osc.length)
        store = ceiling - t.parser.osc.length; // 0 when already at the ceiling
    if (store > 0) {
        if (!reserveOSCBuffer(t, t.parser.osc.length + store + 1))
            return 0; // OOM — caller falls back to the per-byte path
        std::memcpy(t.parser.osc.data + t.parser.osc.length, buffer + start, static_cast<std::size_t>(store));
        t.parser.osc.length += store;
        if (t.parser.osc.length == ceiling) // one-shot at the boundary
            Diagnostics::diagLog(t, "osc-clipped", static_cast<uint32_t>(ceiling));
    }
    return scanned;
}

// DCS.
static void actDCSHeaderIntermediate(Ubiquitty::Terminal& t, uint8_t b)
{
    // No supported DCS has more than one intermediate or a private prefix.
    // Keep an unmatchable sentinel so a later byte cannot turn an unknown
    // header into a supported command by replacing its earlier intermediate.
    t.dcsIntermediate = t.dcsIntermediate || b < 0x20 || b > 0x2F ? 0x7F : b;
}
static void actDCSHeaderFinal(Ubiquitty::Terminal& t, uint8_t b)
{
    t.dcsHeaderDone = true;
    t.dcsFinal = b;
    // These request strings have no parameter field. Other DCS protocols,
    // including sixel, UDK and state restoration, do have numeric parameters.
    if (t.dcsParamCount && ((t.dcsIntermediate == '$' && b == 'q') || (t.dcsIntermediate == '+' && (b == 'q' || b == 'p' || b == 'Q'))))
        t.dcsIntermediate = 0x7F;
    const DCSEntry* e = dcsLookup(t.dcsIntermediate, static_cast<char>(b));
    if (e && e->fn == dcsH_sixel) {
        t.isSixelDCS = true;
        sixelAccumulatorReset(t);
        t.parser.osc.length = 0;
        if (t.screens.active) {
            t.images.sixelOriginRow = static_cast<uint16_t>(t.screens.active->cursorY);
            t.images.sixelOriginColumn = static_cast<uint16_t>(t.screens.active->cursorX);
        }
    } else if (e) {
        // DECRQSS / XTGETTCAP — payload accumulates into osc.data.
        t.parser.osc.length = 0;
    } else {
        // tmux passthrough: preserve the final byte in osc.data so
        // the "tmux;" prefix check inside dcsDispatch sees it.
        if (reserveOSCBuffer(t, t.parser.osc.length + 2))
            t.parser.osc.data[t.parser.osc.length++] = b;
    }
}
static void actDCSParameterDigit(Ubiquitty::Terminal& t, uint8_t b)
{
    if (t.dcsIntermediate) {
        t.dcsIntermediate = 0x7F;
        return;
    }
    if (t.didDCSParametersOverflow)
        return; // as the CSI digits: a dropped separator must not merge two parameters
    if (t.dcsParamCount == 0)
        t.dcsParamCount = 1;
    t.dcsParameters[t.dcsParamCount - 1] = decimalAppendSaturated(t.dcsParameters[t.dcsParamCount - 1], b - '0', 65535);
}
static void actDCSParameterSemicolon(Ubiquitty::Terminal& t, uint8_t)
{
    if (t.dcsIntermediate) {
        t.dcsIntermediate = 0x7F;
        return;
    }
    if (t.dcsParamCount >= VT::maxParameters)
        t.didDCSParametersOverflow = true;
    if (t.dcsParamCount < VT::maxParameters) {
        if (t.dcsParamCount == 0)
            t.dcsParamCount = 1;
        t.dcsParamCount++;
        t.dcsParameters[t.dcsParamCount - 1] = 0;
    }
}
static void actDCSPayloadPut(Ubiquitty::Terminal& t, uint8_t b)
{
    if (t.isSixelDCS) {
        sixelAccumulatorAppend(t, b);
        return;
    }
    // Non-sixel DCS (XTGETTCAP/DECRQSS/DECUDK/DECDLD) carries no image payload, so
    // clip at the 64 KiB non-image ceiling — not the 64 MiB image cap a bare
    // reserveOSCBuffer would admit. Mirrors actOSCPut's per-byte ceiling clip.
    if (t.parser.osc.length >= VT::maxOSCNonImageSize || !reserveOSCBuffer(t, t.parser.osc.length + 2)) {
        // Hit the non-image ceiling (or OOM). Surface the truncation once per DCS
        // frame so a halt-journal capture records the loss.
        if (!t.dcsTruncated) {
            Diagnostics::diagLog(t, "dcs-truncated", static_cast<uint32_t>(t.parser.osc.length));
            t.dcsTruncated = true;
        }
        return;
    }
    t.parser.osc.data[t.parser.osc.length++] = b;
}
static void actDCSTerminate(Ubiquitty::Terminal& t, uint8_t) { dcsTerminate(t); }

// ECMA-48 8.3.27 and 8.3.94 end a DCS or PM string
// only with ST; BEL is not among the bytes a command string may hold. As
// xterm does (charproc.c, CASE_BELL), BEL inside one rings the bell and the
// string goes on: BEL ends an OSC alone.
static void actStringBell(Ubiquitty::Terminal& t, uint8_t) { Ubiquitty::ringBell(t); }
static void actDCSEscapeOther(Ubiquitty::Terminal& t, uint8_t b)
{
    // An escape followed by neither the string terminator nor a second escape
    // abandons the string, as xterm abandons it: nothing in it is dispatched,
    // and the byte after the escape is read in the escape state the transition
    // entered, so `ESC P ... ESC [ H` positions the cursor rather than
    // vanishing into the payload with every byte after it until a cancel.
    if (b != 0x1B) {
        if (t.isSixelDCS)
            sixelAccumulatorReset(t);
        t.parser.osc.length = 0;
        dcsStateReset(t);
        processByteInner(t, b);
        return;
    }
    // Sixel payload: drop ESC silently. tmux passthrough: push
    // doubled ESC + byte for the un-double pass in dcsDispatch.
    if (t.isSixelDCS)
        return;
    // Same non-image ceiling as actDCSPayloadPut: the ESC + byte pair must not
    // grow a non-sixel DCS past 64 KiB (it writes two bytes, so leave room).
    if (t.parser.osc.length + 2 > VT::maxOSCNonImageSize || !reserveOSCBuffer(t, t.parser.osc.length + 3))
        return;
    t.parser.osc.data[t.parser.osc.length++] = 0x1B;
    t.parser.osc.data[t.parser.osc.length++] = b;
}

// Batches a DCSPassthrough payload run. The exit set mirrors the per-byte DFA:
// BEL/CAN/SUB terminate/abort, ESC enters DCSEscape, C1 ST (0x9C) terminates
// only when the input encoding admits C1 controls. Returns 0 when nothing batches.
static int appendDCSPayloadRun(Ubiquitty::Terminal& t, const char* buffer, int start, int length)
{
    const bool doesC1Terminate = c1StringTerminatorEnabled(t);
    const int i = VT::controlStringTerminatorIndex(buffer, start, length, doesC1Terminate);
    const int count = i - start;
    if (count <= 0)
        return 0;
    if (t.isSixelDCS) {
        sixelAccumulatorAppendRun(t, { buffer + start, static_cast<std::size_t>(count) });
        return count;
    }
    // Clip stored bytes at the non-image ceiling; clipped bytes are still CONSUMED
    // (returned in `count`) so the parser advances past them. Clamping before the
    // add also keeps the length arithmetic overflow-safe.
    int store = count;
    if (t.parser.osc.length + store > VT::maxOSCNonImageSize)
        store = maxOSCNonImageSize - t.parser.osc.length; // <= 0 when already at the ceiling
    if (store > 0) {
        if (!reserveOSCBuffer(t, t.parser.osc.length + store + 1))
            return 0; // OOM — caller falls back to the per-byte path
        std::memcpy(t.parser.osc.data + t.parser.osc.length, buffer + start, static_cast<std::size_t>(store));
        t.parser.osc.length += store;
    }
    if (t.parser.osc.length >= VT::maxOSCNonImageSize && !t.dcsTruncated) {
        Diagnostics::diagLog(t, "dcs-truncated", static_cast<uint32_t>(VT::maxOSCNonImageSize));
        t.dcsTruncated = true;
    }
    return count;
}

// APC — kitty graphics protocol
// (https://sw.kovidgoyal.net/kitty/graphics-protocol/).
static void actAPCPut(Ubiquitty::Terminal& t, uint8_t b)
{
    if (t.images.kittyAPCOverflow)
        return;
    if (!t.images.kittyAPC.append({ reinterpret_cast<const char*>(&b), 1 }, Ubiquitty::Kitty::maxAPCCapacity)) {
        t.images.kittyAPCOverflow = true;
        t.images.kittyAPC.length = 0;
    }
}
static void actAPCEnd(Ubiquitty::Terminal& t, uint8_t)
{
    if (t.images.kittyAPCOverflow) {
        t.images.kittyAPC.clearReleasingAbove(Ubiquitty::Kitty::maxRetainedAPCCapacity);
        return;
    }
    // Per-APC gate, keyed on the first payload byte (`G` is kitty graphics). A
    // blocked APC is consumed whole like a blocked DCS: no dispatch, no reply.
    // A kitty transfer left open by an earlier block is abandoned, so it cannot
    // finish later, under a reopened gate, without the blocks dropped here.
    const uint8_t introducer = t.images.kittyAPC.length > 0 ? static_cast<uint8_t>(t.images.kittyAPC.data[0]) : 0;
    if (!Ubiquitty::OSC::apcAllowed(t, introducer)) {
        if (introducer == 'G') {
            t.images.kittyChunkActive = false;
            t.images.kittyChunk.clearReleasingAbove(Ubiquitty::Kitty::maxRetainedAPCCapacity);
        }
        t.images.kittyAPC.clearReleasingAbove(Ubiquitty::Kitty::maxRetainedAPCCapacity);
        return;
    }
    if (t.images.kittyAPC.data)
        t.images.kittyAPC.data[t.images.kittyAPC.length] = 0;
    Ubiquitty::Kitty::dispatch(t);
    t.images.kittyAPC.clearReleasingAbove(Ubiquitty::Kitty::maxRetainedAPCCapacity);
}
static void actAPCEscapeOther(Ubiquitty::Terminal& t, uint8_t b)
{
    actAPCEnd(t, b);
    processByteInner(t, b);
}

static int appendAPCPayloadRun(Ubiquitty::Terminal& t, const char* buffer, int start, int length)
{
    if (t.images.kittyAPCOverflow)
        return 0;
    const bool doesC1Terminate = c1StringTerminatorEnabled(t);
    const int i = VT::controlStringTerminatorIndex(buffer, start, length, doesC1Terminate);
    const int count = i - start;
    if (count <= 0)
        return 0;
    if (!t.images.kittyAPC.append({ buffer + start, static_cast<std::size_t>(count) }, Ubiquitty::Kitty::maxAPCCapacity)) {
        t.images.kittyAPCOverflow = true;
        t.images.kittyAPC.length = 0;
    }
    return count;
}

// A cursor in the hidden right half of a double-width line moves to the last
// column it can be seen in, which is where the line's text ends.
static void clampCursorToVisibleHalf(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    const int right = effectiveRightVisible(t);
    if (s->cursorX > right) {
        s->cursorX = right;
        s->wrapPending = false;
    }
}

// ESC # <Ps> — DECDWL/DECDHL/DECSWL line attributes + DECALN.
static void actHashByte(Ubiquitty::Terminal& t, uint8_t b)
{
    finishAlignedTabString(t);
    // ESC # <X> is a repeatable ESC control function — record it as REP's
    // predecessor (unknown finals too; their repeatable effect is nothing).
    t.parser.precedingItem.setESC(b, '#', 0);
    Ubiquitty::Screen::State* sc = t.screens.active;
    // VT510 DECDWL/DECDHL discard the right half when a single-width line
    // becomes double-width. Returning to single width cannot reveal it again.
    if ((b == '3' || b == '4' || b == '6') && !t.modes.leftRightMarginMode
        && sc->lineAttribute[sc->cursorY] == lineAttributeNormal && t.grid && t.columns > 1) {
        int first = std::max(1, t.columns / 2);
        int last = t.columns - 1;
        Ubiquitty::Screen::expandRangeToWholeRuns(t.grid.get(), sc->cursorY, t.columns, first, last);
        Ubiquitty::Cell blank;
        Ubiquitty::Screen::eraseCell(&blank, sc);
        t.grid->eraseRowRange(Ubiquitty::Row(sc->cursorY), Ubiquitty::Column(first), Ubiquitty::Column(last), blank);
    }
    // While left and right margins are allowed (DECLRMM), a line cannot be made
    // double-width or double-height: DEC STD 070 ignores the request, as xterm
    // does, since a doubled line has no meaning inside a narrowed region.
    switch (b) {
    case '3':
        if (t.modes.leftRightMarginMode)
            return;
        sc->lineAttribute[sc->cursorY] = lineAttributeDoubleHeightTop;
        clampCursorToVisibleHalf(t);
        Ubiquitty::Screen::markAllDirty(t);
        return;
    case '4':
        if (t.modes.leftRightMarginMode)
            return;
        sc->lineAttribute[sc->cursorY] = lineAttributeDoubleHeightBottom;
        clampCursorToVisibleHalf(t);
        Ubiquitty::Screen::markAllDirty(t);
        return;
    case '5':
        sc->lineAttribute[sc->cursorY] = lineAttributeNormal;
        Ubiquitty::Screen::markAllDirty(t);
        return;
    case '6':
        if (t.modes.leftRightMarginMode)
            return;
        sc->lineAttribute[sc->cursorY] = lineAttributeDoubleWidth;
        clampCursorToVisibleHalf(t);
        Ubiquitty::Screen::markAllDirty(t);
        return;
    case '8':
        // DECALN — fill screen with 'E', reset both margin pairs (left/right as
        // well as top/bottom) to the full page, reset origin mode, home the
        // cursor, and restore every row to normal scale. Origin mode is reset
        // because the margins it is relative to were just discarded (xterm
        // charproc.c, CASE_DECALN: UIntClr ORIGIN).
        t.modes.originMode = false;
        sc->scrollTop = 0;
        sc->scrollBottom = t.rows - 1;
        sc->scrollLeft = 0;
        sc->scrollRight = t.columns - 1;
        Ubiquitty::Screen::forgetMargins(*sc);
        t.modes.leftRightMarginMode = false;
        sc->cursorX = 0;
        sc->cursorY = 0;
        sc->wrapPending = false;
        // The rendition goes back to normal, the colours and DECSCA staying
        // (xterm charproc.c, CASE_DECALN: resetRendition).
        sc->pen.attributes = sc->pen.attributes & VT::AttributeSet { VT::Attribute::Protected };
        for (int r = 0; r < t.rows; r++) {
            sc->lineAttribute[r] = lineAttributeNormal;
            sc->lineWrapped[r] = false;
        }
        if (t.grid) {
            Ubiquitty::Cell eCell;
            eCell.codePoint = 'E';
            Pen alignmentPen = sc->pen;
            alignmentPen.attributes = {};
            Ubiquitty::cellSetPen(eCell, alignmentPen);
            // Zero the cluster / link fields — an uninitialized clusterID or
            // combiningLength makes the renderer treat the 'E' as a cluster and
            // rasterise stray marks.
            Ubiquitty::Screen::clearCluster(&eCell);
            for (int r = 0; r < t.rows; r++)
                for (int cc = 0; cc < t.columns; cc++)
                    t.grid->setCell(Ubiquitty::Row(r), Ubiquitty::Column(cc), eCell);
        }
        Ubiquitty::Screen::markAllDirty(t);
        return;
    }
}

// True when the completing SCS designator (intermediate + final + 94/96 nature)
// matches the downloaded soft font's Dscs. A hard set never carries an
// intermediate, so a soft set that uses one can't collide with hard designations.
static bool characterSetMatchesSoftFont(const Ubiquitty::Terminal& t, uint8_t intermediate, uint8_t finalByte) { return Ubiquitty::hasValidSoftFont(t) && t.softFont->designFinal == static_cast<char>(finalByte) && t.softFont->designIntermediate == intermediate && t.softFont->set96 == t.parser.is96CharacterSet; }

// The set a hard designation names, or nothing for a sequence that names no
// set, which is consumed and changes nothing (xterm VTPrsTbl.c, scstable and
// scs96table: CASE_GROUND_STATE and CASE_ESC_IGNORE). A set that is named but
// that Core does not have is ASCII (xterm charproc.c, xtermDecodeSCS:
// nrc_ASCII). A national set is designated whether or not DECNRCM is set, at
// every level: DEC STD 070 SCS lets an emulator "recognize all NRC sets
// regardless of the current keyboard dialect", and the VT510 RM supports all
// DEC character sets in VT500 mode; DECNRCM picks the keyboard dialect's 7-bit
// set, which Core has no dialect for (xterm refuses a `need_nrc` set outside
// national mode instead).
static std::optional<int> designatedCharacterSet(const Ubiquitty::Terminal& t, uint8_t intermediate, uint8_t b)
{
    if (t.parser.is96CharacterSet) {
        if (intermediate != 0)
            return std::nullopt;
        switch (b) {
        case 'A':
            return characterSetLatin1; // ISO 8859-1 right half
        case '<':
            return characterSetDECSupplemental; // DEC Supplemental Graphic
        default:
            break;
        }
        // The ISO 8859 sets the VT510 adds; below VT500 level the designation
        // is not recognized and changes nothing (xterm charproc.c, CASE_GSETS5).
        int iso96;
        switch (b) {
        case 'B':
            iso96 = characterSetLatin2;
            break;
        case 'F':
            iso96 = characterSetGreek;
            break;
        case 'H':
            iso96 = characterSetHebrew;
            break;
        case 'L':
            iso96 = characterSetLatinCyrillic;
            break;
        case 'M':
            iso96 = characterSetLatin5;
            break;
        default:
            return std::nullopt;
        }
        if (t.conformanceLevel < VT::ConformanceLevel::VT500)
            return std::nullopt;
        return iso96;
    }
    switch (intermediate) {
    case 0:
        break;
    case '%':
        if (b == '5')
            return characterSetDECSupplemental; // DEC Supplemental Graphics
        if (b == '0' || b == '2' || b == '3' || b == '6' || b == '=')
            return characterSetUSASCII; // DEC Turkish, Turkish, SCS, Portuguese, Hebrew
        return std::nullopt;
    case '"':
        if (b == '4' || b == '>' || b == '?')
            return characterSetUSASCII; // DEC Hebrew, Greek, DEC Greek
        return std::nullopt;
    case '&':
        if (b == '4' || b == '5')
            return characterSetUSASCII; // DEC Cyrillic, Russian
        return std::nullopt;
    default:
        return std::nullopt;
    }
    switch (b) {
    case 'B':
        return characterSetUSASCII;
    case '0':
        return characterSetDECSpecial;
    case 'A':
        return characterSetUK;
    case '<':
        // DEC Supplemental in its 94-set designation form (ESC ( <);
        // GR-natured, so resolveGLByte folds GL up into 0xA0..0xFF first.
        return characterSetDECSupplemental;
    case '>':
        return characterSetDECTechnical;
    case '1': // VT100 alternate character ROM
    case '2': // VT100 alternate graphics ROM
    case 'I': // JIS Katakana
    case 'J': // JIS-Roman
        return characterSetUSASCII;
    default:
        break;
    }
    int national;
    switch (b) {
    case 'R':
    case 'f':
        national = characterSetFrench;
        break;
    case 'K':
        national = characterSetGerman;
        break;
    case 'Y':
        national = characterSetItalian;
        break;
    case 'E':
    case '6':
    case '`':
        national = characterSetNorwegianDanish;
        break;
    case 'H':
    case '7':
        national = characterSetSwedish;
        break;
    case 'C':
    case '5':
        national = characterSetFinnish;
        break;
    case '4':
        national = characterSetDutch;
        break;
    case '=':
        national = characterSetSwiss;
        break;
    case 'Z':
        national = characterSetSpanish;
        break;
    case 'Q':
    case '9':
        national = characterSetFrenchCanadian;
        break;
    default:
        return std::nullopt;
    }
    return national;
}

// ESC ( / ) / * / + <X> — designate G0..G3 characterSet.
static void actCharacterSetByte(Ubiquitty::Terminal& t, uint8_t b)
{
    int slot = t.parser.characterSetSelect;
    if (slot < 0 || slot > 3)
        slot = 0;
    const uint8_t designatorIntermediate = t.parser.characterSetIntermediate;
    t.parser.characterSetIntermediate = 0; // consumed — the next designation starts clean

    // CharacterSet designation is a repeatable ESC control function; record it (with
    // the designator reconstructed from the slot / 94-vs-96 nature) before dispatch.
    const uint8_t designator = (t.parser.is96CharacterSet && slot >= 1) ? static_cast<uint8_t>('-' + (slot - 1)) : static_cast<uint8_t>('(' + slot);
    t.parser.precedingItem.setESC(b, designator, designatorIntermediate);

    if (t.parser.is96CharacterSet && slot == 0)
        return; // ECMA-35: no 96-set in G0

    // The downloaded soft set's designator designates it unless it names a
    // hard set Core has.
    const std::optional<int> hardSet = designatedCharacterSet(t, designatorIntermediate, b);
    const bool isASCIIDesignation = !t.parser.is96CharacterSet && designatorIntermediate == 0 && b == 'B';
    if ((!hardSet || (*hardSet == characterSetUSASCII && !isASCIIDesignation)) && characterSetMatchesSoftFont(t, designatorIntermediate, b)) {
        t.characterSetG[slot] = characterSetSoftDRCS;
        return;
    }
    if (hardSet)
        t.characterSetG[slot] = *hardSet;
}

// ESC SP F / G — S7C1T / S8C1T.
static void actEscapeSpaceByte(Ubiquitty::Terminal& t, uint8_t b)
{
    // ESC SP <X> (S7C1T/S8C1T) is a repeatable ESC control function.
    t.parser.precedingItem.setESC(b, ' ', 0);
    if (b == 'F')
        t.transmit8bitC1 = false;
    else if (b == 'G')
        t.transmit8bitC1 = true;
}

// ECMA-35 DOCS: return to ISO 2022 with ESC % @, select UTF-8 with
// ESC % G (and xterm's older ESC % 8 spelling). The decoder boundary is
// after the final byte: the rest of this write uses the selected coding.
static void actSelectCodingSystem(Ubiquitty::Terminal& t, uint8_t b)
{
    if (b != '@' && b != 'G' && b != '8')
        return;
    t.parser.precedingItem.setESC(b, '%', 0);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, b == '@' ? Ubiquitty::Unicode::Encoding::Latin1 : Ubiquitty::Unicode::Encoding::UTF8);
    resetUTF8(t);
}

// VT52 ESC Y <row> <column>.
static void actVT52YRow(Ubiquitty::Terminal& t, uint8_t b)
{
    int row = static_cast<int>(b) - 0x20;
    if (row < 0)
        row = 0;
    if (row > t.rows - 1)
        row = t.rows - 1;
    t.screens.active->cursorY = row;
}
static void actVT52YColumn(Ubiquitty::Terminal& t, uint8_t b)
{
    int column = static_cast<int>(b) - 0x20;
    if (column < 0)
        column = 0;
    if (column > t.columns - 1)
        column = t.columns - 1;
    t.screens.active->cursorX = column;
}

// VT52 ESC <X> single-byte dispatch. May override parseState (ESC Y → VT52Y,
// ESC < → Ground), revising processByteInner's default VT52Ground assignment.
static void actVT52Escape(Ubiquitty::Terminal& t, uint8_t b) { vt52EscapeDispatch(t, b); }

// SOS / PM entry — the string body is consumed silently, but a control string is
// excluded from REP, so the predecessor record must clear.
static void actEnterSOSPM(Ubiquitty::Terminal& t, uint8_t) { t.parser.precedingItem.setNone(); }
// ESC <not-\\> inside PM: the string has ended, and the byte after the
// ESC is read in the escape state the transition already entered.
static void actStringEscapeOther(Ubiquitty::Terminal& t, uint8_t b) { processByteInner(t, b); }

// SUB draws the error character for the conformance level, through the same
// placement a printed character takes: one cell, the current rendition,
// autowrap and insert mode. The transition has already abandoned whatever
// sequence it interrupted.
static void actExecuteSubstitute(Ubiquitty::Terminal& t, uint8_t)
{
    const uint32_t character = t.conformanceLevel == VT::ConformanceLevel::VT100 ? VT::vt100SubstituteCharacter : VT::substituteCharacter;
    emitCharacter(t, character, character);
}

// Action dispatch table, indexed by Action enum value. Its order must match the
// enumeration exactly: sizing it to _ActionCount checks only the number of entries.
static const ActionHandler actionHandlers[static_cast<int>(Ubiquitty::VT::Parser::Action::_ActionCount)] = {
    actNoop, // Noop
    actPrint, // Print
    actExecuteBell,
    actExecuteAnswerback,
    actExecuteBackspace,
    actExecuteHorizontalTab,
    actExecuteLineFeed,
    actExecuteCarriageReturn,
    actExecuteShiftOut,
    actExecuteShiftIn,
    actEnterEscape,
    actC1Index,
    actC1NextLine,
    actC1HorizontalTabSet,
    actC1HorizontalTabJustify,
    actC1VerticalTabSet,
    actC1ReverseIndex,
    actC1EnterDCS,
    actC1EnterCSI,
    actC1EnterOSC,
    actC1EnterAPC,
    actC1SingleShift2,
    actC1SingleShift3,
    actC1StartGuardedArea,
    actC1EndGuardedArea,
    actC1Identify,
    actEnterCSI,
    actEnterOSC,
    actEnterDCS,
    actEnterAPC,
    actEnterCharacterSetG0,
    actEnterCharacterSetG1,
    actEnterCharacterSetG2,
    actEnterCharacterSetG3,
    actEnterCharacterSet96G1,
    actEnterCharacterSet96G2,
    actEnterCharacterSet96G3,
    actEscapeDispatch,
    actCSIPrivatePrefix,
    actCSIParameterDigit,
    actCSINewParameter, // CSIParameterSemicolon
    actCSINewParameter, // CSIParameterColon — same fn; receives the byte
    actCSIIntermediate,
    actCSIDispatch,
    actOSCPut,
    actOSCEnd, // OSCBellEnd
    actOSCEnd, // OSCStringTerminatorEnd
    actOSCEscapeOther,
    actDCSHeaderIntermediate,
    actDCSHeaderFinal,
    actDCSParameterDigit,
    actDCSParameterSemicolon,
    actDCSPayloadPut,
    actStringBell,
    actDCSTerminate, // DCSStringTerminatorEnd
    actDCSEscapeOther,
    actAPCPut,
    actAPCEnd, // APCStringTerminatorEnd
    actAPCEscapeOther,
    actHashByte,
    actCharacterSetByte,
    actCharacterSetIntermediateByte,
    actEscapeSpaceByte,
    actVT52YRow,
    actVT52YColumn,
    actVT52Escape,
    actEnterSOSPM,
    actStringEscapeOther,
    actExecuteSubstitute,
    actSelectCodingSystem,
};

static void dispatchAction(Ubiquitty::Terminal& t, uint8_t b, Ubiquitty::VT::Parser::Action a) { actionHandlers[static_cast<int>(a)](t, b); }

// ---- REP (CSI Ps b) — repeat the preceding data-stream item ---- *

static int emitASCIIFill(Ubiquitty::Terminal& t, uint8_t ch, int count);

// Re-dispatch the preceding data-stream item (REP, CSI Ps b). Every category
// replays through the same path that executed the original, so margins, autowrap,
// IRM, wide pairs, clustering, characterSet state, and capability gates all apply as
// if the item arrived again: Graphic → emitCharacter; ControlByte → the Ground DFA
// action; ESC/CSI → their dispatch; None → no-op. The executed count is
// n × item.multiplier (a preceding REP's effect is its whole expansion),
// saturated at maxRepeatEffects; the record is written back with the new product
// so REP-after-REP is well-defined without recursion.
void repeatPrecedingCharacter(Ubiquitty::Terminal& t, int n)
{
    using Kind = Ubiquitty::PrecedingItem::Kind;
    // No-effect REP returns before the copy below -- the copy is 100+ bytes
    // through the stack (plus its canary) and None is the common case in
    // adversarial streams.
    if (t.parser.precedingItem.kind == Kind::None || n < 0)
        return;

    const uint64_t product = static_cast<uint64_t>(n) * t.parser.precedingItem.multiplier;
    const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(product, Ubiquitty::PrecedingItem::maxRepeatEffects));

    if (t.parser.precedingItem.kind == Kind::Graphic) {
        // Three scalars rather than the record the other kinds copy. That copy
        // carries thirty CSI parameters and their separators — ninety bytes a
        // graphic replay never reads — and it is paid twice, once here and once
        // writing the record back. Locals answer the aliasing question the copy
        // answers: a re-record or an alternate-screen flip mid-loop reaches the
        // member, not these.
        const uint32_t codePoint = t.parser.precedingItem.codePoint;
        const uint32_t resolvedCodePoint = t.parser.precedingItem.resolvedCodePoint;
        const uint32_t attachedSelector = t.parser.precedingItem.attachedSelector;
        // Batch eligibility: printable ASCII, no selector, no translation —
        // exactly what emitASCIIFill models. It returns 0 for state it can't model;
        // the fallback emits one repeat through emitCharacter (advancing that state),
        // then batching resumes. Cell-for-cell identical to the per-char path.
        if (attachedSelector == 0 && codePoint == resolvedCodePoint
            && codePoint >= 0x20 && codePoint < 0x7F) {
            uint32_t remaining = count;
            while (remaining > 0) {
                const int chunk = remaining < static_cast<uint32_t>(Ubiquitty::maxColumns) ? static_cast<int>(remaining) : Ubiquitty::maxColumns;
                const int consumed = chunk >= 2 ? emitASCIIFill(t, static_cast<uint8_t>(codePoint), chunk) : 0;
                if (consumed > 0) {
                    remaining -= static_cast<uint32_t>(consumed);
                } else {
                    emitCharacter(t, codePoint, resolvedCodePoint, 0, GraphicCapture::Discard);
                    remaining--;
                }
            }
        } else {
            // CSI dispatch invalidates cancellation for the whole expansion.
            // Keep selector placement separate: fusing its width can change
            // how the base advances around reserved lower text bands.
            for (uint32_t i = 0; i < count; i++) {
                emitCharacter(t, codePoint, resolvedCodePoint, 0, GraphicCapture::Discard);
                if (attachedSelector)
                    emitCharacter(t, attachedSelector, attachedSelector, 0, GraphicCapture::Discard);
            }
        }
        // The replay re-recorded the graphic with multiplier 1; REP's own
        // repeatable effect is the count it just executed. Restating the three
        // fields leaves the record where assigning the whole copy left it — a
        // graphic item is read through its kind, so the stale CSI fields the
        // assignment also carried were never reachable.
        t.parser.precedingItem.setGraphic(codePoint, resolvedCodePoint, attachedSelector);
        t.parser.precedingItem.multiplier = count;
        return;
    }

    // Copy: re-dispatch re-records the live predecessor (same values,
    // multiplier 1) and an alternate-screen flip clears it mid-loop.
    const Ubiquitty::PrecedingItem item = t.parser.precedingItem;

    switch (item.kind) {
    case Kind::Graphic:
        break; // taken above
    case Kind::ControlByte: {
        const Ubiquitty::VT::Parser::Action action = Ubiquitty::VT::Parser::transition(Ubiquitty::VT::Parser::State::Ground, item.controlByte).action;
        for (uint32_t i = 0; i < count; i++)
            dispatchAction(t, item.controlByte, action);
        break;
    }
    // ECMA-48 §8.3.103 defines REP over the last GRAPHIC character, and the two
    // kinds below are neither graphic nor bounded by what a graphic costs. The
    // count is capped at maxRepeatEffects, which is sized for placing a glyph:
    // a few screenfuls, cheap. A control sequence costs whatever its handler
    // costs, and the cap does not know that.
    //
    // `ESC c` then `CSI 65535 b` is six bytes that ran hardReset 65535 times —
    // a full teardown, placement-new and reflow each, all under the model lock.
    // `ESC # 8` then the same is DECALN's rows*columns write loop 65535 times.
    // `CSI c` floods the host with device attributes. Every one arrives as
    // ordinary output from a program, a file, or a peer, which is the threat
    // model a terminal has.
    //
    // Recording them would make them repeatable, so the record does not
    // carry them: escDispatch and csiDispatch leave the predecessor alone, a
    // REP after one repeats the graphic before it, and nothing here has a kind
    // to replay.
    case Kind::ESCSequence:
    case Kind::CSISequence:
        // Not repeatable, per the reasoning above. A snapshot restored from a
        // peer that recorded one still decodes into these kinds, so they are
        // answered here rather than removed from the enum.
        break;
    case Kind::None:
        break;
    }

    // REP itself becomes the preceding item; its repeatable effect is the
    // count it just executed times the underlying item.
    t.parser.precedingItem = item;
    t.parser.precedingItem.multiplier = count;
}

// Count leading bytes of buffer[0..maxN) in [lo, hi]. Callers pass 1 <= lo,
// hi <= 126, which keeps the word tests exact. The word path is little-endian
// only: there a borrow or carry from an invalid byte reaches only later bytes,
// so the mask's first set bit is the boundary. Other byte orders take the
// scalar tail.
static inline int countByteRun(const char* buffer, int maxN, unsigned lo, unsigned hi)
{
    const std::uint64_t ones = 0x0101010101010101ULL;
    const std::uint64_t loSub = ones * lo;
    const std::uint64_t hiAdd = ones * (127u - hi);
    const std::uint64_t hibit = ones * 0x80u;
    int n = 0;
    while (n + 8 <= maxN) {
        std::uint64_t w;
        std::memcpy(&w, buffer + n, 8);
        const std::uint64_t less = (w - loSub) & ~w & hibit; // any byte < lo
        const std::uint64_t more = ((w + hiAdd) | w) & hibit; // any byte > hi
        const std::uint64_t invalid = less | more;
        if (invalid) {
            if constexpr (std::endian::native == std::endian::little)
                return n + std::countr_zero(invalid) / 8;
            break;
        }
        n += 8;
    }
    while (n < maxN) {
        const unsigned char c = static_cast<unsigned char>(buffer[n]);
        if (c < lo || c > hi)
            break;
        ++n;
    }
    return n;
}

static inline std::optional<uint32_t> decodeCombiningDiacriticPair(std::string_view buffer)
{
    if (buffer.size() < 2)
        return std::nullopt;
    const std::uint8_t lead = static_cast<std::uint8_t>(buffer[0]);
    const std::uint8_t trail = static_cast<std::uint8_t>(buffer[1]);
    if (lead == 0xCC && trail >= 0x80 && trail <= 0xBF)
        return 0x0300u + static_cast<uint32_t>(trail - 0x80);
    if (lead == 0xCD && trail >= 0x80 && trail <= 0xAF)
        return 0x0340u + static_cast<uint32_t>(trail - 0x80);
    return std::nullopt;
}

// Single authority for whether the batch fast-paths (emitASCIICombiningRun,
// emitASCIIRun, emitASCIIFill, emitTextRun) may write codepoints straight to
// cells: true only while "1 codepoint → its own cell(s), no translation/shift,
// cursor on a normal single-width row inside the margins" holds. All four gate
// here so a bail can't be added to one and missed in the others — which is the
// point of the list, so keep it exact when a fifth appears.
//
// Invariant for maintainers: a new mode that can change a cell's ADVANCE
// (wide/characterSet width), TRANSLATION (characterSet/single-shift), INSERTION (IRM), the
// active ROW (DWL/DHL, left/right margins, status line), or defer output must
// add its bail here. emitClusterRun uses the lighter !display/status/marginBell
// gate because its clusters route through emitCharacter.
static inline bool batchedWriteEligible(Ubiquitty::Terminal& t, Ubiquitty::Screen::State* s)
{
    if (!t.grid)
        return false;
    if (t.statusLineActive || t.isAlignedTabActive || t.modes.insertMode || s->wrapPending || t.marginBell || t.shouldWriteRightToLeft)
        return false;
    // A pending single shift and VT52 graphics both re-point GL for exactly one
    // stretch of bytes, which no prepared table describes.
    if (t.singleShift != 0 || t.modes.vt52Graphics)
        return false;
    // Narrowed margins are not themselves a bail: the batch writes rightward
    // from the cursor and every writer clamps its own last column to
    // effectiveRight. A cursor sitting outside the margins is, because that
    // clamp assumes the cursor is inside the region it clamps to.
    if (t.modes.leftRightMarginMode && Ubiquitty::Screen::isCursorOutsideMargins(t))
        return false;
    if (s->cursorY < 0 || s->cursorY >= t.rows || s->lineAttribute[s->cursorY] != lineAttributeNormal)
        return false;
    return true;
}

// Reached once a writer has cleared the bails: wrapPending is false, so writing
// a row that fired at fill is a genuine rewrite — drop its trigger guard.
static inline void dropWrapTrigger(Ubiquitty::Terminal& t, Ubiquitty::Screen::State* s)
{
    if (t.triggerWrapEvaluatedRow == s->cursorY)
        t.triggerWrapEvaluatedRow = -1;
}

// Prepare the designated GL set's run translation, reusing the one already held
// when neither the set nor the ambiguous-width choice has moved.
//
// A set qualifies only when every one of its 95 mappings stays inside a single
// cell and needs no cursive shaping, because the run writer advances one column
// per byte and marks no row for shaping. Both questions are asked of the same
// functions the per-character path asks, so a table cannot answer differently
// from the code it stands in for.
static const std::uint16_t* runTranslationForGL(Ubiquitty::Terminal& t, int cs)
{
    if (cs < 0 || cs >= Ubiquitty::runTranslationSetCount)
        return nullptr;
    const int ambiguousWidth = static_cast<int>(t.ambiguousWidth);
    if (t.glRunTranslationAmbiguousWidth != ambiguousWidth) {
        t.glRunTranslationAmbiguousWidth = ambiguousWidth;
        t.glRunTranslationState.fill(0);
    }
    if (t.glRunTranslationState[cs] == 0) {
        bool isUsable = true;
        for (int index = 0; index < Ubiquitty::runTranslationByteCount; ++index) {
            const std::uint8_t byte = static_cast<std::uint8_t>(Ubiquitty::runTranslationFirstByte + index);
            const std::uint32_t codePoint = resolveGLByte(cs, byte);
            t.glRunTranslations[cs][index] = static_cast<std::uint16_t>(codePoint);
            // Width 0 refuses for the same reason width 2 does: the writer puts
            // one byte in one cell, so any mapping that does not occupy exactly
            // one column has to take the per-character route.
            if (codePoint > 0xFFFF || Ubiquitty::Unicode::Width::forCodePoint(codePoint, t.ambiguousWidth) != 1
                || Ubiquitty::Unicode::mayNeedShaping(codePoint))
                isUsable = false;
        }
        t.glRunTranslationState[cs] = isUsable ? 1 : 2;
    }
    return t.glRunTranslationState[cs] == 1 ? t.glRunTranslations[cs].data() : nullptr;
}

static inline bool textFastPathEligible(Ubiquitty::Terminal& t, Ubiquitty::Screen::State* s)
{
    if (!batchedWriteEligible(t, s))
        return false;
    // These writers put the byte itself in the cell, so a designated set that
    // would translate it is a bail. emitASCIIRun carries a table instead.
    if (t.characterSetG[t.characterSetActive] != characterSetUSASCII)
        return false;
    // SGR 76 pins presentation against the code point's own default, and the
    // rules for that live on the per-glyph path. Rather than keep a second copy
    // of them here, a forced run leaves the batch.
    if (s->pen.attributes.contains(VT::Attribute::ForceEmojiPresentation))
        return false;
    dropWrapTrigger(t, s);
    return true;
}

// Batches ASCII bases with U+0300..036F continuations into complete cells.
// These marks only extend the cluster; they cannot change its width or presentation.
template<GraphicCapture Capture> static int emitASCIICombiningRun(Ubiquitty::Terminal& t, const char* buffer, int start, int length)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (!textFastPathEligible(t, s))
        return 0;

    const int rightMargin = Ubiquitty::Screen::effectiveRight(t);
    const int column = s->cursorX;
    if (column < 0 || column > rightMargin)
        return 0;

    const int row = s->cursorY;
    const int maxPairs = columnsBeforeLowerTextBand(t, row, column, rightMargin - column + 1);
    int p = start;
    int nCells = 0;
    std::array<uint32_t, Ubiquitty::clusterPoolMaximumCodePoints> lastCluster {};
    int lastClusterLength = 0;
    uint32_t lastMark = 0;
    int lastPairStart = start;
    uint32_t previousMark = 0;
    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> spanStorage;
    Ubiquitty::Cell* span = spanStorage.data();

    Ubiquitty::Cell tmpl;
    tmpl.codePoint = 0;
    cellSetPenFromScreen(tmpl, s);
    Ubiquitty::Screen::clearCluster(&tmpl);
    tmpl.setLinkID(t.currentLinkID);

    while (nCells < maxPairs && p + 2 < length) {
        const unsigned char base = static_cast<unsigned char>(buffer[p]);
        if (base < 0x20 || base > 0x7E)
            break;
        const std::optional<uint32_t> mark = decodeCombiningDiacriticPair({ buffer + p + 1, static_cast<std::size_t>(length - p - 1) });
        if (!mark)
            break;

        lastPairStart = p;
        previousMark = lastMark;
        Ubiquitty::Cell& cell = spanStorage.constructAt(static_cast<std::size_t>(nCells), tmpl);
        cell.codePoint = static_cast<uint32_t>(base);
        cell.combining[0] = *mark;
        cell.setCombiningLength(1);
        lastCluster[0] = cell.codePoint;
        lastCluster[1] = *mark;
        lastClusterLength = 2;
        lastMark = *mark;
        p += 3;
        while (p + 1 < length) {
            const auto continuation = decodeCombiningDiacriticPair({ buffer + p, static_cast<std::size_t>(length - p) });
            if (!continuation) {
                break;
            }
            if (lastClusterLength < Ubiquitty::clusterPoolMaximumCodePoints) {
                lastCluster[lastClusterLength++] = *continuation;
            }
            lastMark = *continuation;
            p += 2;
        }
        if (lastClusterLength > 2) {
            cell.setClusterID(t.clusterPool.intern({ lastCluster.data(), static_cast<std::size_t>(lastClusterLength) }));
        }
        ++nCells;
    }

    if (nCells < 2)
        return 0;
    if (graphicCaptureBefore<Capture>(buffer, p, length) == GraphicCapture::Retain) {
        --nCells;
        p = lastPairStart;
        lastClusterLength = cellClusterCodePoints(&t, &span[nCells - 1], lastCluster.data(), lastCluster.size());
        lastMark = previousMark;
    }
    discardGraphicCancellation(t);

    const int lastColumn = column + nCells - 1;
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    if (t.grid->hasSizedCellHint()) [[unlikely]]
        eraseOverwrittenTextBands(t, row, column, lastColumn);
    t.grid->clearWideOverlapEdges(Ubiquitty::Row(row), Ubiquitty::Column(column), Ubiquitty::Column(lastColumn), blank);
    t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(column), Ubiquitty::RowView { span, nCells });

    if (lastColumn >= rightMargin) {
        s->cursorX = rightMargin;
        s->wrapPending = true;
        // Evaluate the filled row now so it matches the per-char path regardless
        // of a later wrap.
        evaluateFilledRowTriggers(&t, row);
    } else {
        s->cursorX = lastColumn + 1;
    }
    // Last received graphic is the MARK, not the base, so REP re-attaches the
    // mark (matches the per-char path).
    t.parser.precedingItem.setGraphic(lastMark, lastMark, 0);
    clusterOpen(t, row, lastColumn, lastCluster[0]);
    for (int index = 1; index < lastClusterLength; ++index) {
        (void)Ubiquitty::Unicode::graphemeExtends(&t.parser.clusterState, t.parser.clusterLast, lastCluster[index]);
        t.parser.clusterLast = lastCluster[index];
        t.parser.clusterCodePoints[index] = lastCluster[index];
    }
    t.parser.clusterCodepointLength = lastClusterLength;
    t.parser.clusterLast = lastMark;
    return p - start;
}

template<GraphicCapture Capture> static int emitASCIIRun(Ubiquitty::Terminal& t, const char* buffer, int start, int length)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (!batchedWriteEligible(t, s))
        return 0;
    // A designated GL set does not cost this writer its batch: one byte still
    // becomes one cell, through a table rather than as itself.
    const std::uint16_t* translation = nullptr;
    if (t.characterSetG[t.characterSetActive] != characterSetUSASCII) {
        translation = runTranslationForGL(t, t.characterSetG[t.characterSetActive]);
        if (!translation)
            return 0;
    }
    dropWrapTrigger(t, s);
    const int rightMargin = Ubiquitty::Screen::effectiveRight(t);
    const int column = s->cursorX;
    if (column < 0 || column > rightMargin)
        return 0;
    const int avail = rightMargin - column + 1;
    int maxN = length - start;
    if (maxN > avail)
        maxN = avail;
    int n = columnsBeforeLowerTextBand(t, s->cursorY, column, countByteRun(buffer + start, maxN, 0x20, 0x7E));
    // HT replaces the data-screen predecessor, so CCH cannot undo this run's
    // last graphic afterward. Batch eligibility excludes the status line,
    // whose tab handler preserves the graphic predecessor.
    const bool isFollowedByTab = start + n < length && buffer[start + n] == '\t';
    if (!isFollowedByTab && graphicCaptureBefore<Capture>(buffer, start + n, length) == GraphicCapture::Retain)
        --n;
    if (n < 2 || (parseAblateMask & AblateBatchedRuns) != 0)
        return 0;
    discardGraphicCancellation(t);

    const int row = s->cursorY;
    const int lastColumn = column + n - 1;

    // setRowSpan overwrites the whole [column..lastColumn] interior, so only the
    // two edges can orphan a wide-pair half — the edge-only clear is O(1).
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    if (t.grid->hasSizedCellHint()) [[unlikely]]
        eraseOverwrittenTextBands(t, row, column, lastColumn);
    t.grid->clearWideOverlapEdges(Ubiquitty::Row(row), Ubiquitty::Column(column), Ubiquitty::Column(lastColumn), blank);

    // Every cell in the run shares one pen, an empty cluster, and the active link
    // id. Hand that template + raw bytes to the display, which writes straight into
    // the row store — no staging buffer, no second copy.
    Ubiquitty::Cell tmpl;
    tmpl.codePoint = 0;
    cellSetPenFromScreen(tmpl, s);
    Ubiquitty::Screen::clearCluster(&tmpl);
    tmpl.setLinkID(t.currentLinkID);
    const std::string_view runText { buffer + start, static_cast<std::size_t>(n) };
    if (translation)
        t.grid->writeTranslatedRun(Ubiquitty::Row(row), Ubiquitty::Column(column), tmpl, runText, translation);
    else
        t.grid->writeASCIIRun(Ubiquitty::Row(row), Ubiquitty::Column(column), tmpl, runText);

    // Cursor advance mirrors the per-byte tail: a glyph on the right margin arms
    // the deferred wrap instead of stepping past it.
    if (lastColumn >= rightMargin) {
        s->cursorX = rightMargin;
        s->wrapPending = true;
        // Full-width line completed by the batch: evaluate it now so it fires like
        // the per-char path.
        evaluateFilledRowTriggers(&t, row);
    } else {
        s->cursorX = lastColumn + 1;
    }
    // A combining mark arriving next attaches to the run's last cell.
    uint32_t lastCharacter = static_cast<uint32_t>(static_cast<unsigned char>(buffer[start + n - 1]));
    t.parser.precedingItem.setGraphic(lastCharacter, lastCharacter, 0);
    clusterOpen(t, row, lastColumn, lastCharacter);
    return n;
}

// Uniform-fill sibling of emitASCIIRun for REP expansion: same eligibility gate,
// margin clamp, wide-edge clear, and cursor/wrap tail; the cell write goes through
// fillCellRun. Returns cells written (>= 2), or 0 to fall back to the per-char path.
static int emitASCIIFill(Ubiquitty::Terminal& t, uint8_t ch, int count)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (!textFastPathEligible(t, s))
        return 0;
    const int rightMargin = Ubiquitty::Screen::effectiveRight(t);
    const int column = s->cursorX;
    if (column < 0 || column > rightMargin)
        return 0;
    const int avail = rightMargin - column + 1;
    const int n = columnsBeforeLowerTextBand(t, s->cursorY, column, std::min(count, avail));
    if (n < 2)
        return 0;

    const int row = s->cursorY;
    const int lastColumn = column + n - 1;

    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    if (t.grid->hasSizedCellHint()) [[unlikely]]
        eraseOverwrittenTextBands(t, row, column, lastColumn);
    t.grid->clearWideOverlapEdges(Ubiquitty::Row(row), Ubiquitty::Column(column), Ubiquitty::Column(lastColumn), blank);

    Ubiquitty::Cell cell;
    cell.codePoint = ch;
    cellSetPenFromScreen(cell, s);
    Ubiquitty::Screen::clearCluster(&cell);
    cell.setLinkID(t.currentLinkID);
    t.grid->fillCellRun(Ubiquitty::Row(row), Ubiquitty::Column(column), cell, n);

    if (lastColumn >= rightMargin) {
        s->cursorX = rightMargin;
        s->wrapPending = true;
        evaluateFilledRowTriggers(&t, row);
    } else {
        s->cursorX = lastColumn + 1;
    }
    t.parser.precedingItem.setGraphic(ch, ch, 0);
    clusterOpen(t, row, lastColumn, ch);
    return n;
}

// Fast path: write a run of wide (double-width) glyphs — CJK ideographs,
// fullwidth forms — as one setRowSpan instead of a putCharacter -> emitCharacter ->
// writeWideCell -> graphemeExtends -> clusterOpen round trip per code point.
// Returns bytes consumed (a whole number of UTF-8 sequences), or 0 to fall
// back to the per-byte path. Mirrors emitASCIIRun's gating: bails for status
// line, insert (IRM) mode, a pending wrap, the margin bell, rectangular
// margins, a DWL/DHL line, or a cursor with no room for a 2-cell glyph. Only
// codepoints that are width-2 and stand alone join the run — width-0 combining
// marks and ZWJ/RI sequences end it and are left for emitCharacter, which keeps
// clustering correct.
//
// A pictograph with no joining successor is one of the glyphs that joins, and it
// is the one to read twice: it draws from the colour sheet, the cell that says so
// carries `Attribute::Emoji`, and that bit is per code point while the span
// carries one template pen. The placement loop adds it per glyph for exactly that
// reason. This sentence read "non-emoji" while the branch below admitted them,
// and the gap between the two was four days of three arms reporting a colour
// glyph that never drew — the run placed the cell correctly in every respect a
// text or geometry reader can see, and untagged in the one a colour sheet gates
// on.
//
// Why admitting it keeps the clustering guarantee below, since that is the
// question a reader should ask next: two breaks already decide it. `ci + 1 >=
// nDec` sends the last glyph of a batch to emitCharacter, and
// `pictographStandsAloneBefore` ejects one whose successor joins it inside the
// same decode. So a pictograph joins the run only where a non-joining successor
// in the same decode has proved it opens no cluster — which is the condition the
// guarantee needs, rather than a weaker one that happens to hold. A modifier or
// ZWJ arriving in the next read meets a glyph that already left the run.
//
// Bailing while clusterIsLive guarantees no open
// cluster can be extended mid-run; and because no glyph in the run opens a
// cluster until the last, emitCharacter's per-glyph graphemeExtends (which here
// only ever returns false — a wide base never extends a base) is provably a
// no-op we can skip. Orphan wide-pair partners are broken only at the run's
// outer edges (matching writeWideCell); interior partners are overwritten by
// the span. A local UTF-8 decoder reuses the strict decode (overlong /
// surrogate rejection) and consumes only complete sequences for accepted
// glyphs, so t->parser.utf8 stays at need==0 and a truncated or invalid sequence is
// left untouched for the per-byte path.
// Unicode blocks that are entirely East-Asian-Wide with no emoji-presentation
// members, so emitTextRun can skip the Width + emoji + grapheme lookups. The
// tables stay authoritative for every other code point; emitTextRun's debug
// assert pins these ranges so they can't drift.
[[gnu::always_inline]] static inline bool isWideIdeographFast(uint32_t codePoint)
{
    return (codePoint >= 0x4E00 && codePoint <= 0x9FFF) // CJK Unified Ideographs
        || (codePoint >= 0x3400 && codePoint <= 0x4DBF) // CJK Extension A
        || (codePoint >= 0xAC00 && codePoint <= 0xD7A3) // Hangul Syllables
        || (codePoint >= 0xF900 && codePoint <= 0xFAFF) // CJK Compatibility Ideographs
        || (codePoint >= 0x20000 && codePoint <= 0x2FA1F); // CJK Ext B–F + compat suppl
}

static inline bool isPlainWideGraphic(uint32_t codePoint, Unicode::Width::AmbiguousWidth ambiguousWidth)
{
    if (isWideIdeographFast(codePoint))
        return true;
    const auto properties = Unicode::Width::classify(codePoint);
    if (properties & (Unicode::detail::PropGCBClassField::mask | Unicode::detail::PropExtPictField::mask | Unicode::detail::PropEmojiPresentationField::mask))
        return false;
    return Unicode::Width::cellsForWidthClass(Unicode::detail::PropWidthField::get(properties), ambiguousWidth) == 2
        && !Unicode::mayNeedShaping(codePoint);
}

// Box-drawing + block elements (U+2500..259F): all width-1, never emoji, never
// grapheme-relevant, so emitTextRun takes the width-1 path directly. The Width /
// grapheme tables stay authoritative — a debug assert pins this range to them.
[[gnu::always_inline]] static inline bool isNarrowGraphicFast(uint32_t codePoint) { return codePoint >= 0x2500 && codePoint <= 0x259F; }

#if defined(__aarch64__) || defined(_M_ARM64)
static inline bool isPlainNarrowGraphic(uint32_t codePoint, Unicode::Width::AmbiguousWidth ambiguousWidth)
{
    if (codePoint >= 0x20 && codePoint < 0x7F)
        return true;
    if (isC1CodePoint(codePoint))
        return false;
    const auto properties = Unicode::Width::classify(codePoint);
    if (properties & (Unicode::detail::PropGCBClassField::mask | Unicode::detail::PropExtPictField::mask | Unicode::detail::PropEmojiPresentationField::mask))
        return false;
    return Unicode::Width::cellsForWidthClass(Unicode::detail::PropWidthField::get(properties), ambiguousWidth) == 1
        && !Unicode::mayNeedShaping(codePoint);
}
#endif

// A code point read without consuming it, and how many bytes carry it.
struct PeekedCodePoint {
    uint32_t codePoint = 0;
    int length = 0; // 2..4, never 1 — an ASCII lead is not multibyte
};

static inline std::optional<PeekedCodePoint> peekMultibyteUTF8CodePoint(std::string_view buffer)
{
    if (buffer.empty())
        return std::nullopt;

    const std::uint8_t lead = static_cast<std::uint8_t>(buffer[0]);
    int trailCount = 0;
    uint32_t value = 0;
    uint32_t minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        trailCount = 1;
        value = static_cast<uint32_t>(lead & 0x1F);
        minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        trailCount = 2;
        value = static_cast<uint32_t>(lead & 0x0F);
        minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        trailCount = 3;
        value = static_cast<uint32_t>(lead & 0x07);
        minimum = 0x10000;
    } else {
        return std::nullopt;
    }

    if (buffer.size() < static_cast<std::size_t>(trailCount) + 1)
        return std::nullopt;
    for (int index = 1; index <= trailCount; ++index) {
        const std::uint8_t byte = static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(index)]);
        if ((byte & 0xC0) != 0x80)
            return std::nullopt;
        value = static_cast<uint32_t>((value << 6) | static_cast<uint32_t>(byte & 0x3F));
    }

    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        return std::nullopt;

    return PeekedCodePoint { value, trailCount + 1 };
}

// graphicCaptureBefore's full classification, for every successor shape.
[[gnu::noinline]] static GraphicCapture graphicCaptureBeforeAnySuccessor(const char* buffer, int next, int length)
{
    if (next >= length)
        return GraphicCapture::Retain;
    const auto byte = static_cast<uint8_t>(buffer[next]);
    if (byte >= 0x20 && byte < 0x7F)
        return GraphicCapture::Discard;
    if (byte == '\r' || byte == '\n')
        return GraphicCapture::Discard;
    // A whole scalar past the C1 range. The peek accepts exactly the sequences
    // utf8DecodeRun accepts, one at a time and without the run's setup.
    if (byte >= 0xC2) {
        const auto peeked = peekMultibyteUTF8CodePoint({ buffer + next, static_cast<std::size_t>(length - next) });
        if (peeked && peeked->codePoint >= 0xA0)
            return GraphicCapture::Discard;
    }
    if (byte == 0x1B && next + 1 < length) {
        // String introducers clear the predecessor before collecting payload.
        switch (buffer[next + 1]) {
        case ']':
        case 'P':
        case '_':
        case 'X':
        case '^':
            return GraphicCapture::Discard;
        case '[':
            break;
        default:
            return GraphicCapture::Retain;
        }
        const int start = next + 2;
        const int final = start + countByteRun(buffer + start, std::min(length - start, 64), 0x20, 0x3F);
        if (final < length && static_cast<uint8_t>(buffer[final]) >= 0x40 && static_cast<uint8_t>(buffer[final]) <= 0x7E)
            return GraphicCapture::Discard;
    }
    return GraphicCapture::Retain;
}

// With no CCH in this chunk, LF and completed CSI make every preceding
// graphic unavailable to cancellation. Keep the suffix conservative: the next
// chunk may complete a CCH, including one interrupting an unfinished CSI.
static int graphicCancellationBoundary(std::span<const char> input)
{
    // '%' also leaves coding-system selection, which can yield to the decoder,
    // on the conservative path. Ordinary T and % bytes are safe false positives.
    if (input.empty() || std::memchr(input.data(), 'T', input.size())
        || std::memchr(input.data(), 0x94, input.size()) || std::memchr(input.data(), '%', input.size()))
        return 0;
    const int length = static_cast<int>(input.size());
    for (int index = length - 1; index >= 0; --index) {
        if (input[index] == '\n')
            return index;
        if (input[index] != '\033' || index + 1 >= length || input[index + 1] != '[')
            continue;
        const int start = index + 2;
        const int final = start + countByteRun(input.data() + start, std::min(length - start, 64), 0x20, 0x3F);
        if (final < length && static_cast<uint8_t>(input[final]) >= 0x40 && static_cast<uint8_t>(input[final]) <= 0x7E)
            return final;
    }
    return 0;
}

static inline bool shouldPeekClusterLead(const char* buffer, int start, int length)
{
    const std::uint8_t lead = static_cast<std::uint8_t>(buffer[start]);
    if (lead == 0xEF || lead >= 0xF0)
        return true; // VS15/VS16, supplementary emoji, RI flags, skin tones, tag chars
    if (lead == 0xCC || lead == 0xCD)
        return true; // common U+0300..036F combining diacritics
    if (lead != 0xE2 || start + 1 >= length)
        return false;

    const std::uint8_t second = static_cast<std::uint8_t>(buffer[start + 1]);
    return second == 0x80 // ZWJ
        || second == 0x83 // keycap mark
        || (second >= 0x98 && second <= 0x9E) // BMP pictographs U+2600..U+27FF
        || second == 0xAC; // symbols such as U+2B50
}

// Whether a pictograph is a cluster on its own, judged against what follows it.
// A pictograph is itself grapheme-relevant, being what GB11 joins onto, so
// asking relevance instead keeps every one of them off the batched path: two
// adjacent pictographs are two clusters, not one. The break machinery answers
// from a state reset to the glyph, so the rule is stated in one place and both
// the routing gate and the run batcher ask it the same question.
static inline bool pictographStandsAloneBefore(uint32_t codePoint, uint32_t successor)
{
    Ubiquitty::Unicode::GraphemeState probe;
    Ubiquitty::Unicode::graphemeStateReset(&probe, codePoint);
    return !Ubiquitty::Unicode::graphemeExtendsHot(&probe, codePoint, successor);
}

// The code point beginning at `start`, if the buffer holds it whole. A lead
// sitting at the end of a chunk has no successor to judge yet, and the joiner
// deciding it may arrive in the next one.
static inline bool peekSuccessorCodePoint(const char* buffer, int start, int length, uint32_t& successor)
{
    if (start >= length)
        return false;
    const std::uint8_t byte = static_cast<std::uint8_t>(buffer[start]);
    if (byte < 0x80) {
        successor = byte;
        return true;
    }
    const std::optional<PeekedCodePoint> peeked = peekMultibyteUTF8CodePoint({ buffer + start, static_cast<std::size_t>(length - start) });
    if (!peeked)
        return false;
    successor = peeked->codePoint;
    return true;
}

static inline bool shouldRouteUTF8LeadToClusterRun(Ubiquitty::Terminal& t, const char* buffer, int start, int length)
{
    const std::optional<PeekedCodePoint> peeked = peekMultibyteUTF8CodePoint({ buffer + start, static_cast<std::size_t>(length - start) });
    if (!peeked)
        return false;
    const uint32_t firstCodePoint = peeked->codePoint;

    if (isWideIdeographFast(firstCodePoint) || isNarrowGraphicFast(firstCodePoint))
        return clusterIsLive(t) && Ubiquitty::Unicode::isGraphemeRelevant(firstCodePoint);

    const std::uint8_t firstClass = Ubiquitty::Unicode::Width::classify(firstCodePoint);
    // A pictograph with nothing joining it is a plain glyph, and the run batcher
    // places it as cheaply as any ideograph. Only a successor that joins needs
    // the per-glyph cluster path.
    //
    // Asked of every pictograph, not only the ones that default to emoji
    // presentation. Extended_Pictographic is the superset — a card suit, an
    // arrow, a trademark sign — and presentation decides how WIDE such a glyph
    // draws, not whether it clusters. The break class is what would make one
    // cluster on its own account, so a pictograph carrying none is a lone glyph
    // whenever its successor declines to join.
    const bool isPictograph = (firstClass & (Ubiquitty::Unicode::detail::PropEmojiPresentationField::mask | Ubiquitty::Unicode::detail::PropExtPictField::mask))
        != 0;
    const bool hasOwnBreakClass = (firstClass & Ubiquitty::Unicode::detail::PropGCBClassField::mask) != 0;
    if (isPictograph && !hasOwnBreakClass) {
        uint32_t successor = 0;
        if (!peekSuccessorCodePoint(buffer, start + peeked->length, length, successor))
            return true;
        return !pictographStandsAloneBefore(firstCodePoint, successor);
    }
    return (firstClass & Ubiquitty::Unicode::detail::propGraphemeRelevantMask) != 0;
}

/// The fewest bytes a batched text run is allowed to decode.
constexpr int textRunMinimumDecodeBytes = 24;

/// A decode budget that never ends a run's decode before the row does.
constexpr int textRunUnlimitedDecodeBytes = 1 << 28;

/// The bytes the next batched text run may decode, after a run that placed
/// `placedBytes` of the `decodedBytes` it decoded.
///
/// A run stops placing at the first code point the cluster path owns, and the
/// next run decodes again from there. A run that left more of its decode
/// unplaced than it placed, and more than the minimum, leaves the next one
/// twice what it placed; any other run doubles the budget, up to no limit. Text
/// without stops therefore decodes whole rows, text dense with them decodes
/// little beyond each stop, and a lone code point a run cannot place does not
/// shrink the budget.
///
/// - Parameters:
///   - budget: The budget the run decoded under.
///   - placedBytes: The bytes it placed.
///   - decodedBytes: The bytes it decoded.
/// - Returns: The budget for the next run.
static inline int nextTextRunDecodeBudget(int budget, int placedBytes, int decodedBytes)
{
    const int unplacedBytes = decodedBytes - placedBytes;
    if (unplacedBytes > std::max(placedBytes, textRunMinimumDecodeBytes))
        return std::max(textRunMinimumDecodeBytes, 2 * placedBytes);
    return std::min(textRunUnlimitedDecodeBytes, 2 * budget);
}

/// Whether the next sixteen bytes are all multibyte text, which is the shape
/// the batch decoder is for.
[[gnu::always_inline]] static inline bool isLongMultibyteStretch(const char* bytes)
{
    std::uint64_t first;
    std::uint64_t second;
    std::memcpy(&first, bytes, sizeof first);
    std::memcpy(&second, bytes + sizeof first, sizeof second);
    constexpr std::uint64_t highBits = 0x8080808080808080ull;
    return (first & second & highBits) == highBits;
}

/// Decodes a short stretch of multibyte sequences, stopping at the first byte
/// below 0xC2, the first malformed sequence, an incomplete trailing sequence
/// or `maxOut` code points; what it accepts and rejects is what utf8DecodeRun
/// does. In line: the stretches that text mixing scripts is made of are one
/// or two characters, where the batch decoder's setup costs more than the
/// decode.
///
/// - Parameters:
///   - input: The bytes, starting at a lead byte.
///   - out: Receives the code points; its size bounds the decode.
///   - bytesConsumed: Receives the bytes the returned code points stood for.
/// - Returns: The number of code points decoded.
[[gnu::always_inline]] static inline int decodeShortUTF8Stretch(std::span<const char> input, std::span<std::uint32_t> out, int* bytesConsumed)
{
    const char* in = input.data();
    const int length = static_cast<int>(input.size());
    const int maxOut = static_cast<int>(out.size());
    int p = 0;
    int n = 0;
    while (p < length && n < maxOut) {
        const auto b0 = static_cast<std::uint8_t>(in[p]);
        if (b0 < 0xC2)
            break;
        if (b0 < 0xE0) {
            if (length - p < 2)
                break;
            const auto b1 = static_cast<std::uint8_t>(in[p + 1]);
            if ((b1 & 0xC0) != 0x80)
                break;
            out[n++] = (static_cast<std::uint32_t>(b0 & 0x1F) << 6) | (b1 & 0x3F);
            p += 2;
            continue;
        }
        if (b0 < 0xF0) {
            if (length - p < 3)
                break;
            const auto b1 = static_cast<std::uint8_t>(in[p + 1]);
            const auto b2 = static_cast<std::uint8_t>(in[p + 2]);
            const std::uint8_t lowest = b0 == 0xE0 ? 0xA0 : 0x80;
            const std::uint8_t highest = b0 == 0xED ? 0x9F : 0xBF;
            if (b1 < lowest || b1 > highest || (b2 & 0xC0) != 0x80)
                break;
            out[n++] = (static_cast<std::uint32_t>(b0 & 0x0F) << 12) | (static_cast<std::uint32_t>(b1 & 0x3F) << 6) | (b2 & 0x3F);
            p += 3;
            continue;
        }
        if (b0 > 0xF4 || length - p < 4)
            break;
        const auto b1 = static_cast<std::uint8_t>(in[p + 1]);
        const auto b2 = static_cast<std::uint8_t>(in[p + 2]);
        const auto b3 = static_cast<std::uint8_t>(in[p + 3]);
        const std::uint8_t lowest = b0 == 0xF0 ? 0x90 : 0x80;
        const std::uint8_t highest = b0 == 0xF4 ? 0x8F : 0xBF;
        if (b1 < lowest || b1 > highest || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80)
            break;
        out[n++] = (static_cast<std::uint32_t>(b0 & 0x07) << 18) | (static_cast<std::uint32_t>(b1 & 0x3F) << 12)
            | (static_cast<std::uint32_t>(b2 & 0x3F) << 6) | (b3 & 0x3F);
        p += 4;
    }
    *bytesConsumed = p;
    return n;
}

/// The bytes one U+FFFD stands for at the start of a malformed sequence.
///
/// The lead and the continuations it accepts, up to the byte that breaks the
/// sequence, which is not among them: it begins the next, as the per-byte
/// decoder re-feeds it. A lead no sequence starts with is one byte.
///
/// - Parameters:
///   - bytes: The sequence's first byte and what follows it in the input.
///   - available: How many bytes there are.
/// - Returns: The substitute's length, or zero for a sequence that is valid,
///   or that the input ends inside and the decoder carries across the end.
[[gnu::noinline]] static int malformedUTF8SubpartLength(const char* bytes, int available)
{
    const auto lead = static_cast<std::uint8_t>(bytes[0]);
    if (lead < 0xC2 || lead > 0xF4)
        return 1;
    int need = 1;
    std::uint8_t lowest = 0x80;
    std::uint8_t highest = 0xBF;
    if (lead >= 0xF0) {
        need = 3;
        if (lead == 0xF0)
            lowest = 0x90;
        else if (lead == 0xF4)
            highest = 0x8F;
    } else if (lead >= 0xE0) {
        need = 2;
        if (lead == 0xE0)
            lowest = 0xA0;
        else if (lead == 0xED)
            highest = 0x9F;
    }
    for (int taken = 1; taken <= need; ++taken) {
        if (taken >= available)
            return 0;
        const auto byte = static_cast<std::uint8_t>(bytes[taken]);
        if (byte < lowest || byte > highest)
            return taken;
        lowest = 0x80;
        highest = 0xBF;
    }
    return 0;
}

/// The index of the first code point of a grapheme break class in a decoded
/// batch, or -1 when there is none.
///
/// Nothing below U+0300 has such a class, nor do the Greek, Cyrillic and
/// Armenian letters, which is most of the text that reaches this, so those
/// are answered by range and the rest read the table. Out of line: the caller
/// is the text run, which every byte of the input passes through.
[[gnu::noinline]] static int firstBreakClassCodePoint(std::span<const std::uint32_t> codePoints)
{
    for (std::size_t index = 0; index < codePoints.size(); ++index) {
        const std::uint32_t codePoint = codePoints[index];
        if (codePoint < 0x300 || (codePoint >= 0x370 && codePoint <= 0x482) || (codePoint >= 0x48A && codePoint <= 0x590))
            continue;
        if ((Ubiquitty::Unicode::graphemeBreakRawByte(codePoint) & Ubiquitty::Unicode::detail::PropGCBClassField::mask) != 0)
            return static_cast<int>(index);
    }
    return -1;
}

template<GraphicCapture Capture> static int emitTextRun(Ubiquitty::Terminal& t, const char* buffer, int start, int length, int& decodeBudget)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (!textFastPathEligible(t, s))
        return 0;
    const int rightMargin = Ubiquitty::Screen::effectiveRight(t);
    const int column = s->cursorX;
    if (column < 0 || column > rightMargin)
        return 0; // no room for even one cell without a wrap

    const int row = s->cursorY;
    const int writableColumns = columnsBeforeLowerTextBand(t, row, column, rightMargin - column + 1);
    // The presentation bit is a property of the GLYPH, not of the run. Stripped
    // here and re-added per wide glyph below, exactly as emitWideCharacter does
    // it — a run that shares one attribute set across its cells keeps an
    // emoji's width and loses its presentation, which draws correctly on every
    // arm except the one that gates a colour sheet on the bit.
    const VT::AttributeSet penAttributes = s->pen.attributes - VT::Attribute::Emoji;
    const VT::AttributeSet leadAttributes = penAttributes | VT::Attribute::Wide;
    const VT::AttributeSet continuationAttributes = penAttributes | VT::Attribute::WideContinuation;

    // Per-glyph cells share this base (pen, empty cluster, active link id);
    // the placement loop copies it and patches only ch + attributes.
    Ubiquitty::Cell tmpl;
    tmpl.codePoint = 0;
    cellSetPenFromScreen(tmpl, s);
    Ubiquitty::Screen::clearCluster(&tmpl);
    tmpl.setLinkID(t.currentLinkID);

    // The run's pen is fixed for the whole run, and every cell the placement
    // loop writes differs from the template in the attribute set alone: the
    // wide halves substitute loop-invariant sets, and a narrow glyph keeps the
    // template's. So no cell needs a pen of its own.

    Ubiquitty::CellScratchBuffer<Ubiquitty::maxColumns> spanStorage;
    Ubiquitty::Cell* span = spanStorage.data();
    int nCells = 0; // span cells filled (1 or 2 per glyph)
    int currentColumn = column;
    int lastLeadColumn = column; // lead column of the last glyph placed
    int p = start;
    uint32_t lastCodePoint = 0;
    int lastByteLength = 0; // the bytes the last placed code point stood for
    bool wrapAfter = false;

    // Decode only the row-fit prefix. Plain glyphs consume at least one cell, so
    // anything beyond the remaining cell count cannot be placed by this call and
    // would be decoded again after the wrap.
    std::array<std::uint32_t, Ubiquitty::maxColumns> codePoints;
    const int codePointCapacity = std::min<int>(static_cast<int>(std::size(codePoints)), writableColumns);
    int nDec = 0;
    // The input bytes each decoded code point stands for, kept only once a
    // substitute is among them: a substitute's are the malformed bytes it
    // replaces, which its own encoding does not say, and every other code
    // point's follow from its value.
    std::array<std::uint8_t, Ubiquitty::maxColumns> byteLengths;
    bool hasSubstitutes = false;
    const auto encodedLength = [](std::uint32_t codePoint) -> int {
        return codePoint < 0x80 ? 1 : codePoint < 0x800 ? 2
            : codePoint < 0x10000                       ? 3
                                                        : 4;
    };
    const auto noteDecodedLengths = [&](int first, int count) {
        for (int index = first; index < first + count; ++index)
            byteLengths[index] = static_cast<std::uint8_t>(encodedLength(codePoints[index]));
    };
    const auto noteSubstitute = [&](int length) {
        if (!hasSubstitutes) {
            noteDecodedLengths(0, nDec);
            hasSubstitutes = true;
        }
        byteLengths[nDec] = static_cast<std::uint8_t>(length);
    };
    const auto byteLength = [&](int index) -> int { return hasSubstitutes ? byteLengths[index] : encodedLength(codePoints[index]); };
    // The bytes a range of decoded code points stood for. Without a substitute
    // the sum is a pure function of the code points, which the compiler
    // vectorises; the lookup is kept to the run that needs it.
    const auto encodedLengthOfRun = [&](int first, int count) -> int {
        int total = 0;
        if (hasSubstitutes) {
            for (int index = first; index < first + count; ++index)
                total += byteLengths[index];
        } else {
            for (int index = first; index < first + count; ++index)
                total += encodedLength(codePoints[index]);
        }
        return total;
    };
    int decodedEnd = start;
    {
        // The decode ends at the budget; bytes past it wait for the next run.
        const int decodeLimit = decodeBudget < length - start ? start + decodeBudget : length;
        // A word break is one ASCII byte, and stopping the decode there hands
        // every gap to the per-glyph path and restarts the run on the far side.
        // An ASCII gap between two multibyte stretches belongs in the same run;
        // an ASCII tail does not, because emitASCIIRun writes four bytes a cell
        // against this path's whole cell. textFastPathEligible above pins the
        // character set to US-ASCII, which is what lets a byte here be its own
        // code point rather than a designation to resolve.
        int offset = start;
        while (nDec < codePointCapacity && offset < decodeLimit) {
            const unsigned char lead = static_cast<unsigned char>(buffer[offset]);
            if (lead >= 0x20 && lead < 0x7F) {
                // A short ASCII piece between the run's glyphs stays in the
                // run: text that mixes scripts character by character would
                // pay the run's setup once per non-ASCII glyph otherwise. A
                // word, or a tail that nothing multibyte follows, is left to
                // emitASCIIRun, which writes it narrower than the whole cells
                // this path stages, and a wide-led row keeps the wide path.
                constexpr int maxAbsorbedASCII = 5;
                int asciiEnd = offset + 1;
                while (asciiEnd < decodeLimit && asciiEnd - offset <= maxAbsorbedASCII
                    && static_cast<unsigned char>(buffer[asciiEnd]) >= 0x20 && static_cast<unsigned char>(buffer[asciiEnd]) < 0x7F)
                    ++asciiEnd;
                if (asciiEnd - offset > maxAbsorbedASCII || asciiEnd >= decodeLimit
                    || static_cast<unsigned char>(buffer[asciiEnd]) < 0x80 || nDec + (asciiEnd - offset) > codePointCapacity)
                    break;
                for (; offset < asciiEnd; ++offset) {
                    if (hasSubstitutes)
                        byteLengths[nDec] = 1;
                    codePoints[nDec++] = static_cast<unsigned char>(buffer[offset]);
                }
                continue;
            }
            if (lead < 0x80)
                break; // a control or DEL ends the run
            if (lead < 0xC2) {
                // A stray continuation or an overlong lead: one substitute per
                // byte, as the per-byte decoder makes it, under the policy that
                // substitutes U+FFFD; the other policies keep the per-byte path.
                if (t.parser.invalidUTF8Policy != Ubiquitty::Unicode::InvalidUTF8Policy::FFFD)
                    break;
                noteSubstitute(1);
                codePoints[nDec++] = 0xFFFD;
                ++offset;
                continue;
            }
            int runBytes = 0;
            const int remaining = decodeLimit - offset;
            // A long stretch is the batch decoder's; a short one, which is
            // what text that mixes scripts is made of, is decoded in place.
            const int decoded = remaining >= 16 && isLongMultibyteStretch(buffer + offset)
                ? Ubiquitty::Unicode::utf8DecodeRun(
                      std::span { buffer + offset, static_cast<std::size_t>(remaining) },
                      std::span { codePoints.data() + nDec, static_cast<std::size_t>(codePointCapacity - nDec) }, &runBytes)
                : decodeShortUTF8Stretch(std::span { buffer + offset, static_cast<std::size_t>(remaining) },
                      std::span { codePoints.data() + nDec, static_cast<std::size_t>(codePointCapacity - nDec) }, &runBytes);
            if (decoded <= 0) {
                // A malformed sequence takes one U+FFFD for the lead and the
                // continuations it accepts, and the run goes on from the byte
                // that broke it, as the per-byte decoder would. A sequence the
                // input ends inside, or one that only crosses the decode
                // budget, is left whole for the path that carries it.
                const int subpart = t.parser.invalidUTF8Policy == Ubiquitty::Unicode::InvalidUTF8Policy::FFFD
                    ? malformedUTF8SubpartLength(buffer + offset, length - offset)
                    : 0;
                if (subpart == 0)
                    break;
                noteSubstitute(subpart);
                codePoints[nDec++] = 0xFFFD;
                offset += subpart;
                continue;
            }
            if (hasSubstitutes)
                noteDecodedLengths(nDec, decoded);
            const int batchStart = nDec;
            nDec += decoded;
            offset += runBytes;
            // A code point of a break class ends the run where the placement
            // ends it: it may extend the cluster before it, which the
            // per-character path owns. Decoding past it would read a row of
            // text to place a word of it, and a decode with no such code point
            // can take the narrow path whole. Only a batch led by text that
            // can take that path is read for one: ideographs, kana, Hangul
            // syllables and pictographs are placed by the wide and staged
            // paths whichever way the batch ends, and the pass over a row of
            // them costs a tenth of its lane, so a mark there is left to the
            // budget.
            if (codePoints[batchStart] < 0x3000) {
                const int breakIndex = firstBreakClassCodePoint(std::span { codePoints.data() + batchStart, static_cast<std::size_t>(decoded) });
                if (breakIndex >= 0) {
                    for (int index = batchStart + breakIndex; index < nDec; ++index)
                        offset -= byteLength(index);
                    nDec = batchStart + breakIndex;
                    break;
                }
            }
            // Wide-led text usually fills the row before exhausting this decode.
            // Place that prefix before decoding more; mixed-width text can
            // resume in another run without consuming any unplaced code point.
            if (nDec >= (writableColumns + 1) / 2 && isWideIdeographFast(codePoints[0]))
                break;
        }
        // A pictograph is placed only beside the code point after it, so a
        // decode the budget ends on one reaches one code point further.
        if (offset == decodeLimit && offset < length && nDec > 0 && nDec < codePointCapacity
            && static_cast<unsigned char>(buffer[offset]) >= 0xC2
            && (Ubiquitty::Unicode::graphemeBreakRawByte(codePoints[nDec - 1])
                   & (Ubiquitty::Unicode::detail::PropEmojiPresentationField::mask | Ubiquitty::Unicode::detail::PropExtPictField::mask))
                != 0) {
            int runBytes = 0;
            const int decoded = Ubiquitty::Unicode::utf8DecodeRun(
                std::span { buffer + offset, static_cast<std::size_t>(length - offset) },
                std::span { codePoints.data() + nDec, std::size_t { 1 } }, &runBytes);
            if (decoded > 0) {
                if (hasSubstitutes)
                    noteDecodedLengths(nDec, decoded);
                nDec += decoded;
                offset += runBytes;
            }
        }
        decodedEnd = offset;
    }
    // A live cluster + grapheme-relevant first code point means it could EXTEND
    // the open cluster: hand it to emitCharacter via emitClusterRun. A plain base can't
    // extend, so proceed (it starts a fresh cluster exactly as emitCharacter would).
    if (nDec > 0 && clusterIsLive(t) && Ubiquitty::Unicode::isGraphemeRelevant(codePoints[0])) {
        // Whether it EXTENDS the open cluster, not merely whether it could: a
        // pictograph following a pictograph opens a new cluster (GB999), and
        // every batched run leaves one open behind it, so the relevance test
        // alone turns a stream of them back into per-glyph emits. The probe
        // copies the live state because the answer must not advance it.
        Ubiquitty::Unicode::GraphemeState probe = t.parser.clusterState;
        if (Ubiquitty::Unicode::graphemeExtends(&probe, t.parser.clusterLast, codePoints[0])) {
            decodeBudget = nextTextRunDecodeBudget(decodeBudget, 0, decodedEnd - start);
            return 0;
        }
    }
    int wideCodePointCount = 0;
    const int maxWideCodePoints = std::min(nDec, writableColumns / 2);
    while (wideCodePointCount < maxWideCodePoints && isPlainWideGraphic(codePoints[wideCodePointCount], t.ambiguousWidth)) {
        assert(Unicode::Width::forCodePoint(codePoints[wideCodePointCount], t.ambiguousWidth) == 2
            && !Unicode::Width::isEmojiPresentation(codePoints[wideCodePointCount]));
        ++wideCodePointCount;
    }
    // A short homogeneous prefix must not split a mixed-width row into extra
    // writes: every write may materialize the deferred blank it leaves behind.
    if (wideCodePointCount >= 2 && wideCodePointCount == maxWideCodePoints) {
        nCells = wideCodePointCount * 2;
        lastLeadColumn = column + nCells - 2;
        lastCodePoint = codePoints[wideCodePointCount - 1];
        lastByteLength = byteLength(wideCodePointCount - 1);
        wrapAfter = column + nCells - 1 == rightMargin;
        if (hasSubstitutes)
            p += encodedLengthOfRun(0, wideCodePointCount);
        else // no wide glyph is ASCII, and the three-way sum vectorises
            for (int index = 0; index < wideCodePointCount; ++index)
                p += codePoints[index] < 0x800 ? 2 : codePoints[index] < 0x10000 ? 3
                                                                                 : 4;
    } else
        wideCodePointCount = 0;

    int narrowCodePointCount = 0;
    int verifiedNarrowPrefixCount = 0;
#if defined(__aarch64__) || defined(_M_ARM64)
    if (wideCodePointCount == 0 && nDec >= 2) {
        while (narrowCodePointCount < nDec && isPlainNarrowGraphic(codePoints[narrowCodePointCount], t.ambiguousWidth)) {
            assert(Unicode::Width::forCodePoint(codePoints[narrowCodePointCount], t.ambiguousWidth) == 1
                && !Unicode::Width::isEmojiPresentation(codePoints[narrowCodePointCount]));
            ++narrowCodePointCount;
        }
        // A partial prefix would fragment a mixed-width span and repeat the
        // deferred-blank work. Keep the same span boundary as the staged path.
        if (narrowCodePointCount == nDec) {
            nCells = narrowCodePointCount;
            lastLeadColumn = column + nCells - 1;
            lastCodePoint = codePoints[narrowCodePointCount - 1];
            lastByteLength = byteLength(narrowCodePointCount - 1);
            wrapAfter = lastLeadColumn == rightMargin;
            p += encodedLengthOfRun(0, narrowCodePointCount);
        } else {
            verifiedNarrowPrefixCount = narrowCodePointCount;
            narrowCodePointCount = 0;
        }
    }
#endif

    for (int ci = 0; wideCodePointCount == 0 && narrowCodePointCount == 0 && ci < nDec; ++ci) {
        const uint32_t codePoint = codePoints[ci];
        if (isC1CodePoint(codePoint))
            break; // ignored or performed, which the per-character path owns

        int width;
        // Beside the width because it comes from the same probe: the flat table
        // answers width and presentation in one byte, and both are per glyph.
        bool isEmojiPresentation = false;
        if (ci < verifiedNarrowPrefixCount)
            width = 1;
        else if (codePoint < 0x80)
            width = 1; // printable ASCII: the decode admits nothing else below it
        else if (isWideIdeographFast(codePoint)) {
            width = 2; // CJK/Kana/Hangul/compat — skip the table lookups
            assert(Ubiquitty::Unicode::Width::forCodePoint(codePoint, t.ambiguousWidth) == 2 && !Ubiquitty::Unicode::Width::isEmojiPresentation(codePoint));
        } else if (isNarrowGraphicFast(codePoint)) {
            width = 1; // box / block — skip the three table lookups
            assert(Ubiquitty::Unicode::Width::forCodePoint(codePoint, t.ambiguousWidth) == 1 && !Ubiquitty::Unicode::Width::isEmojiPresentation(codePoint) && !Ubiquitty::Unicode::isGraphemeRelevant(codePoint));
        } else {
            // One flat-table probe for width + emoji + grapheme-relevance together.
            const std::uint8_t wb = Ubiquitty::Unicode::Width::classify(codePoint);
            isEmojiPresentation = (wb & Ubiquitty::Unicode::detail::PropEmojiPresentationField::mask) != 0;
            // Every question below comes from the byte already in hand: a break
            // class of its own is what lets a code point extend the cluster
            // before it, and pictograph-ness is what lets one stand alone.
            if (wb & Ubiquitty::Unicode::detail::PropEmojiPresentationField::mask) {
                // A pictograph standing alone is a plain wide glyph. The run is
                // already decoded, so the successor it is judged against costs
                // nothing to reach; one at the end of the decode is left to the
                // cluster path, where a joiner in the next chunk still reaches it.
                if (ci + 1 >= nDec)
                    break;
                if (!pictographStandsAloneBefore(codePoint, codePoints[ci + 1]))
                    break; // a successor joins it: cluster / ZWJ path
            } else if (wb & Ubiquitty::Unicode::detail::PropGCBClassField::mask) {
                break; // combining mark / SpacingMark / ZWJ / RI / jamo:
                       // could extend a cluster, so leave it to emitCharacter
            } else if (wb & Ubiquitty::Unicode::detail::PropExtPictField::mask) {
                // A text-default pictograph — a card suit, an arrow, a trademark
                // sign — asks the same question as an emoji-default one.
                // Presentation decides the width it draws at, and the branch
                // above has already ruled out a class of its own, so nothing but
                // a joining successor keeps it off the run.
                if (ci + 1 >= nDec)
                    break;
                if (!pictographStandsAloneBefore(codePoint, codePoints[ci + 1]))
                    break;
            }
            width = Ubiquitty::Unicode::Width::cellsForWidthClass(Ubiquitty::Unicode::detail::PropWidthField::get(wb), t.ambiguousWidth);
            if (width != 1 && width != 2)
                break; // zero-width control etc.: not a plain glyph
        }
        const int glyphEndColumn = currentColumn + width - 1;
        if (glyphEndColumn >= column + writableColumns)
            break; // glyph would need a wrap

        // Cells share the run's pen, empty cluster, and link id; only ch +
        // attributes vary. Copy the template then patch those two fields.
        if (width == 2) {
            Ubiquitty::Cell& leadCell = spanStorage.constructAt(static_cast<std::size_t>(nCells), tmpl);
            leadCell.codePoint = codePoint; // resolveCodePoint is identity for codePoint >= 0x80
            Ubiquitty::cellSetPenAttributes(leadCell,
                isEmojiPresentation ? (leadAttributes | VT::Attribute::Emoji) : leadAttributes);
            Ubiquitty::Cell& continuationCell = spanStorage.constructAt(static_cast<std::size_t>(nCells + 1), tmpl);
            continuationCell.codePoint = Ubiquitty::wideContinuationSentinelCodePoint;
            Ubiquitty::cellSetPenAttributes(continuationCell, continuationAttributes);
            nCells += 2;
        } else {
            // A narrow glyph carries the run's pen unchanged — the template was
            // built from the same screen pen the attributes come from — so the
            // template copy is the whole pen write.
            Ubiquitty::Cell& narrowCell = spanStorage.constructAt(static_cast<std::size_t>(nCells), tmpl);
            narrowCell.codePoint = codePoint;
            nCells += 1;
        }
        lastLeadColumn = currentColumn;
        lastCodePoint = codePoint;
        lastByteLength = byteLength(ci);
        p += lastByteLength;

        // Equality is the only reachable >= case: the placement guard above
        // already broke on glyphEndColumn > rightMargin.
        if (glyphEndColumn == rightMargin) { // reached the margin: defer wrap
            wrapAfter = true;
            break;
        }
        currentColumn = glyphEndColumn + 1;
    }

    if (nCells == 0) {
        decodeBudget = nextTextRunDecodeBudget(decodeBudget, 0, decodedEnd - start);
        return 0;
    }

    if (graphicCaptureBefore<Capture>(buffer, p, length) == GraphicCapture::Retain) {
        const int lastWidth = nCells - (lastLeadColumn - column);
        nCells -= lastWidth;
        if (nCells == 0) {
            decodeBudget = nextTextRunDecodeBudget(decodeBudget, 0, decodedEnd - start);
            return 0;
        }
        p -= lastByteLength;
        if (wideCodePointCount) {
            --wideCodePointCount;
            lastLeadColumn = column + nCells - 2;
            lastCodePoint = codePoints[wideCodePointCount - 1];
        } else if (narrowCodePointCount) {
            --narrowCodePointCount;
            lastLeadColumn = column + nCells - 1;
            lastCodePoint = codePoints[narrowCodePointCount - 1];
        } else {
            int previousLead = nCells - 1;
            if (span[previousLead].pen.attributes.contains(VT::Attribute::WideContinuation))
                --previousLead;
            lastLeadColumn = column + previousLead;
            lastCodePoint = span[previousLead].codePoint;
        }
        wrapAfter = false;
    }
    discardGraphicCancellation(t);

    const int lastColumn = column + nCells - 1;

    // Break only wide partners straddling the run's outer edges — interior
    // partners are overwritten by the span. clearWideOverlap's both-directions
    // clear would blank our own continuation halves, so break each edge by hand.
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, s);
    const Ubiquitty::Cell* edge0 = t.grid->logicalCellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
    if (edge0 && Ubiquitty::cellPen(*edge0).attributes.contains(VT::Attribute::WideContinuation) && column > 0)
        t.grid->eraseRowRange(Ubiquitty::Row(row), Ubiquitty::Column(column - 1), Ubiquitty::Column(column - 1), blank);
    const Ubiquitty::Cell* edgeN = t.grid->logicalCellAt(Ubiquitty::Row(row), Ubiquitty::Column(lastColumn));
    if (edgeN && Ubiquitty::cellPen(*edgeN).attributes.contains(VT::Attribute::Wide) && lastColumn + 1 < t.columns)
        t.grid->eraseRowRange(Ubiquitty::Row(row), Ubiquitty::Column(lastColumn + 1), Ubiquitty::Column(lastColumn + 1), blank);

    if (wideCodePointCount)
        t.grid->writeWideCodePointRun(Ubiquitty::Row(row), Ubiquitty::Column(column), tmpl, { codePoints.data(), static_cast<std::size_t>(wideCodePointCount) });
    else if (narrowCodePointCount)
        t.grid->writeNarrowCodePointRun(Ubiquitty::Row(row), Ubiquitty::Column(column), tmpl, { codePoints.data(), static_cast<std::size_t>(narrowCodePointCount) });
    else
        t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(column), Ubiquitty::RowView { span, nCells });

    if (wrapAfter) {
        s->cursorX = lastLeadColumn; // emitCharacter leaves the cursor on the lead
        s->wrapPending = true;
        // Full-width line completed by the batch: evaluate it now so it fires like
        // the per-char path.
        evaluateFilledRowTriggers(&t, row);
    } else {
        s->cursorX = lastColumn + 1;
    }
    // A combining mark / VS / ZWJ arriving next attaches to the run's last glyph.
    t.parser.precedingItem.setGraphic(lastCodePoint, lastCodePoint, 0);
    clusterOpen(t, row, lastLeadColumn, lastCodePoint);
    decodeBudget = nextTextRunDecodeBudget(decodeBudget, p - start, decodedEnd - start);
    return p - start;
}

// Assemble already-decoded, width-stable clusters into a row span. The caller
// has established a grapheme boundary before the first code point. Narrow
// bases without an adjacent VS16 and presentation-forced runs retain the
// character path: later selectors can claim a new column there. These clusters
// own two cells from placement onward, including the existing VS16 pair fusion.
[[gnu::always_inline]] static inline int emitWideClusterSpan(Ubiquitty::Terminal& t, std::span<const uint32_t> codePoints, GraphicCapture tailCapture)
{
    auto* screen = t.screens.active;
    if (!textFastPathEligible(t, screen) || t.triggersEngine)
        return 0; // filled-row triggers must observe the base before its marks
    const int rightMargin = Ubiquitty::Screen::effectiveRight(t);
    const int column = screen->cursorX;
    if (column < 0 || column + 1 > rightMargin)
        return 0;

    const int writableColumns = columnsBeforeLowerTextBand(t, screen->cursorY, column, rightMargin - column + 1);
    constexpr std::size_t maxDecodedCodePoints = 96;
    assert(codePoints.size() <= maxDecodedCodePoints);
    Ubiquitty::CellScratchBuffer<maxDecodedCodePoints * 2> cells;
    Ubiquitty::Cell cellTemplate;
    cellSetPenFromScreen(cellTemplate, screen);
    Ubiquitty::Screen::clearCluster(&cellTemplate);
    cellTemplate.setLinkID(t.currentLinkID);
    const auto attributes = screen->pen.attributes - VT::Attribute::Emoji;
    int cellCount = 0;
    std::size_t consumed = 0;
    std::size_t lastStart = 0;
    std::size_t lastEnd = 0;
    Ubiquitty::Unicode::GraphemeState lastState {};
    while (consumed < codePoints.size() && cellCount + 1 < writableColumns) {
        const uint32_t base = codePoints[consumed];
        const auto properties = Ubiquitty::Unicode::Width::classify(base);
        const bool isPictographic = Ubiquitty::Unicode::isExtendedPictographic(base);
        const bool hasWideSelector = isPictographic && consumed + 1 < codePoints.size()
            && codePoints[consumed + 1] == Ubiquitty::Unicode::Width::variationSelector16;
        if (!Ubiquitty::Unicode::isGraphemeRelevant(base)
            || (!hasWideSelector && Ubiquitty::Unicode::Width::cellsForWidthClass(Ubiquitty::Unicode::detail::PropWidthField::get(properties), t.ambiguousWidth) != 2))
            break;
        bool isEmoji = (properties & Ubiquitty::Unicode::detail::PropEmojiPresentationField::mask) != 0;
        Ubiquitty::Unicode::GraphemeState state {};
        Ubiquitty::Unicode::graphemeStateReset(&state, base);
        std::size_t end = consumed + 1;
        while (end < codePoints.size() && Ubiquitty::Unicode::isGraphemeRelevant(codePoints[end])) {
            auto nextState = state;
            if (!Ubiquitty::Unicode::graphemeExtendsHot(&nextState, codePoints[end - 1], codePoints[end]))
                break;
            state = nextState;
            if (isPictographic) {
                if (codePoints[end] == Ubiquitty::Unicode::Width::variationSelector16)
                    isEmoji = true;
                else if (codePoints[end] == Ubiquitty::Unicode::Width::variationSelector15)
                    isEmoji = false;
            }
            ++end;
        }
        auto& lead = cells.constructAt(static_cast<std::size_t>(cellCount), cellTemplate);
        lead.codePoint = base;
        Ubiquitty::cellSetPenAttributes(lead, attributes | VT::Attribute::Wide | (isEmoji ? VT::AttributeSet { VT::Attribute::Emoji } : VT::AttributeSet {}));
        const auto cluster = codePoints.subspan(consumed, end - consumed);
        if (cluster.size() > 1) {
            lead.combining[0] = cluster[1];
            lead.setCombiningLength(1);
            if (cluster.size() > 2)
                lead.setClusterID(t.clusterPool.intern(cluster.first(std::min(cluster.size(), static_cast<std::size_t>(Ubiquitty::clusterPoolMaximumCodePoints)))));
        }
        auto& continuation = cells.constructAt(static_cast<std::size_t>(cellCount + 1), cellTemplate);
        continuation.codePoint = Ubiquitty::wideContinuationSentinelCodePoint;
        Ubiquitty::cellSetPenAttributes(continuation, attributes | VT::Attribute::WideContinuation);
        cellCount += 2;
        lastStart = consumed;
        lastEnd = end;
        lastState = state;
        consumed = end;
    }
    if (cellCount == 0)
        return 0;

    const bool hasGraphicSuccessor = consumed < codePoints.size() && !isC1CodePoint(codePoints[consumed]);
    if (!hasGraphicSuccessor && (consumed < codePoints.size() || tailCapture == GraphicCapture::Retain)) {
        cellCount -= 2;
        if (cellCount == 0)
            return 0;
        consumed = lastStart;
        lastEnd = consumed;
        lastStart = 0;
        Unicode::graphemeStateReset(&lastState, codePoints[0]);
        for (std::size_t index = 1; index < consumed; ++index) {
            if (!Unicode::graphemeExtends(&lastState, codePoints[index - 1], codePoints[index])) {
                lastStart = index;
                Unicode::graphemeStateReset(&lastState, codePoints[index]);
            }
        }
    }
    discardGraphicCancellation(t);

    const int row = screen->cursorY;
    const int lastColumn = column + cellCount - 1;
    Ubiquitty::Cell blank;
    Ubiquitty::Screen::eraseCell(&blank, screen);
    if (t.grid->hasSizedCellHint()) [[unlikely]]
        eraseOverwrittenTextBands(t, row, column, lastColumn);
    t.grid->clearWideOverlapEdges(Ubiquitty::Row(row), Ubiquitty::Column(column), Ubiquitty::Column(lastColumn), blank);
    t.grid->setRowSpan(Ubiquitty::Row(row), Ubiquitty::Column(column), Ubiquitty::RowView { cells.data(), cellCount });
    const int lastLeadColumn = lastColumn - 1;
    screen->wrapPending = lastColumn == rightMargin;
    screen->cursorX = screen->wrapPending ? lastLeadColumn : lastColumn + 1;
    clusterOpen(t, row, lastLeadColumn, codePoints[lastStart]);
    t.parser.clusterState = lastState;
    t.parser.clusterLast = codePoints[lastEnd - 1];
    const std::size_t tracked = std::min(lastEnd - lastStart, static_cast<std::size_t>(Ubiquitty::clusterPoolMaximumCodePoints));
    std::copy_n(codePoints.data() + lastStart, tracked, t.parser.clusterCodePoints.data());
    t.parser.clusterCodepointLength = static_cast<uint8_t>(tracked);
    const uint32_t selector = lastStart + 1 < lastEnd && Ubiquitty::Unicode::Width::isEmojiVariationSelector(codePoints[lastStart + 1]) ? codePoints[lastStart + 1] : 0;
    t.parser.precedingItem.setGraphic(codePoints[lastStart], codePoints[lastStart], selector);
    return static_cast<int>(consumed);
}

// Companion to emitTextRun for grapheme-relevant codepoints it can't batch into a
// span (emoji bases, ZWJ sequences, flags, skin tones, combining marks). Emits via
// emitCharacter while grapheme-relevant, stopping at the first plain base (emitTextRun
// takes those) or a control/incomplete/invalid byte. Returns bytes consumed.
template<GraphicCapture Capture> static int emitClusterRun(Ubiquitty::Terminal& t, const char* buffer, int start, int length)
{
    if (t.isAlignedTabActive)
        return 0;
    if (!t.grid || t.statusLineActive || t.marginBell || t.shouldWriteRightToLeft)
        return 0;
    if (t.screens.active->wrapPending) {
        // The text writer leaves a pending wrap to scalar placement. A plain
        // base cannot enter this cluster writer, so avoid decoding its tail.
        const auto first = peekMultibyteUTF8CodePoint({ buffer + start, static_cast<std::size_t>(length - start) });
        if (!first || !Ubiquitty::Unicode::isGraphemeRelevant(first->codePoint))
            return 0;
    }
    // Consumes only the grapheme-relevant PREFIX, breaking at the first plain base.
    // Start with a small batch (prefixes are usually short); a cluster-DENSE stream
    // consumes every batch whole, so re-decode in place at the full batch size and
    // keep going, with one route-probe per row.
    constexpr int clusterDecodeFirst = 16;
    constexpr int clusterDecodeBatchSize = 96;
    std::uint32_t codePoints[clusterDecodeBatchSize];
    int p = start;
    int batch = clusterDecodeFirst;
    for (;;) {
        int rbytes = 0;
        const int nDec = Ubiquitty::Unicode::utf8DecodeRun(
            std::span { buffer + p, static_cast<std::size_t>(length - p) },
            std::span { codePoints, static_cast<std::size_t>(batch) }, &rbytes);
        const auto tailCapture = graphicCaptureBefore<Capture>(buffer, p + rbytes, length);
        int ci = 0;
        bool handBack = false; // plain base / control: caller's other paths own it
        while (ci < nDec) {
            const uint32_t codePoint = codePoints[ci];
            if (!Ubiquitty::Unicode::isGraphemeRelevant(codePoint)) {
                handBack = true;
                break; // a plain base — let emitTextRun batch it
            }
            // Batched cluster extension: scan the maximal run of codepoints that
            // keep extending the open cluster (UAX-29, via the same graphemeExtends
            // state emitCharacter threads) and attach them in one lead-cell RMW. The gate
            // mirrors emitCharacter's: a live cluster plus the >= U+0300 fast-reject.
            if (clusterIsLive(t) && (codePoint >= 0x300 || t.parser.clusterLast >= 0x300)) {
                const int scanStart = ci;
                uint32_t previous = t.parser.clusterLast;
                auto runState = t.parser.clusterState;
                auto stateBeforeLast = runState;
                bool boundary = false;
                while (ci < nDec) {
                    const uint32_t candidate = codePoints[ci];
                    if (!Ubiquitty::Unicode::isGraphemeRelevant(candidate)) {
                        handBack = true;
                        break; // a plain base — let emitTextRun batch it
                    }
                    if (candidate < 0x300 && previous < 0x300) {
                        // emitCharacter's fast-reject: both below the lowest combining
                        // mark, so no rule joins them (candidate is a fresh base).
                        boundary = true;
                        break;
                    }
                    auto nextState = runState;
                    if (!Ubiquitty::Unicode::graphemeExtendsHot(&nextState, previous, candidate)) {
                        boundary = true;
                        break;
                    }
                    stateBeforeLast = runState;
                    runState = nextState;
                    previous = candidate;
                    p += (candidate < 0x800) ? 2 : (candidate < 0x10000) ? 3
                                                                         : 4;
                    ++ci;
                }
                if (ci > scanStart) {
                    const auto capture = graphicCaptureBefore<Capture>(buffer, p, length);
                    if (capture == GraphicCapture::Discard) {
                        discardGraphicCancellation(t);
                        t.parser.clusterState = runState;
                        clusterAppendRun(t, { codePoints + scanStart, static_cast<std::size_t>(ci - scanStart) });
                        t.parser.precedingItem.setGraphic(codePoints[ci - 1], codePoints[ci - 1], 0);
                    } else {
                        if (ci - scanStart > 1) {
                            t.parser.clusterState = stateBeforeLast;
                            clusterAppendRun(t, { codePoints + scanStart, static_cast<std::size_t>(ci - scanStart - 1) });
                            t.parser.precedingItem.setGraphic(codePoints[ci - 2], codePoints[ci - 2], 0);
                        }
                        emitCharacter(t, codePoints[ci - 1], codePoints[ci - 1]);
                    }
                }
                if (boundary) {
                    // The break state is already reseeded for this base; clusterReset
                    // keeps the base emit from re-running the check (clusterOpen
                    // re-seeds identically).
                    clusterReset(t);
                    continue; // loop top emits codePoints[ci] as a fresh base
                }
                break; // plain base for emitTextRun, or decode batch exhausted
            }
            const int placed = emitWideClusterSpan(t, { codePoints + ci, static_cast<std::size_t>(nDec - ci) }, tailCapture);
            if (placed > 0) {
                for (int index = 0; index < placed; ++index) {
                    const uint32_t placedCodePoint = codePoints[ci + index];
                    p += placedCodePoint < 0x800 ? 2 : placedCodePoint < 0x10000 ? 3
                                                                                 : 4;
                }
                ci += placed;
                continue;
            }
            // Keep a decoded selector with its base for REP. Placement retains
            // the intermediate base state so CCH can cancel the selector alone.
            uint32_t selector = 0;
            if (ci + 1 < nDec && Ubiquitty::Unicode::Width::isEmojiVariationSelector(codePoints[ci + 1]))
                selector = codePoints[ci + 1];
            // resolveCodePoint is identity for >= 0x80 (GL translation covers
            // only the 7-bit range) and every codepoint decoded here is a
            // multi-byte sequence — skip the proven no-op call.
            assert(codePoint >= 0x80);
            const int end = p + (codePoint < 0x800 ? 2 : codePoint < 0x10000 ? 3
                                                                             : 4)
                + (selector ? 3 : 0);
            emitCharacter(t, codePoint, codePoint, selector, graphicCaptureBefore<Capture>(buffer, end, length));
            p += (codePoint < 0x800) ? 2 : (codePoint < 0x10000) ? 3
                                                                 : 4;
            ++ci;
            if (selector) {
                ++ci;
                p += 3; // VS15/VS16 are three UTF-8 bytes
            }
        }
        // Re-decode only when this batch was decoded full and consumed whole —
        // the cluster-dense case. Anything else (a plain base, an incomplete /
        // invalid tail, end of input) returns to the ground loop.
        if (handBack || ci < nDec || nDec < batch)
            break;
        batch = clusterDecodeBatchSize;
    }
    return p - start;
}

// Fast path: accumulate a run of CSI parameter bytes — digits and the ';' /
// ':' separators, all of which keep State::CSI — in a tight loop, bypassing
// the per-byte transition() + dispatchAction() round trip. Returns the number
// of bytes consumed (>= 1, since the caller only enters with a parameter byte).
// Only the parameter-accumulation handlers run, byte-for-byte what the dispatcher
// would route to (CSIParameterDigit -> actCSIParameterDigit, CSIParameterSemicolon /
// CSIParameterColon -> actCSINewParameter); the first non-parameter byte (final,
// intermediate, private prefix, control) is left for the normal driver. The
// range 0x30..0x3B is exactly [0-9 : ;] and excludes the 0x3C..0x3F prefix
// bytes, which are only meaningful as the sequence's first byte anyway.
static int scanCSIParameters(Ubiquitty::Terminal& t, const char* buffer, int start, int length)
{
    if (t.parser.intermediate != 0)
        markCSIMalformed(t);
    int n = 0;
    int parameterCount = t.parser.parameterCount;
    int currentParameter = parameterCount > 0 ? t.parser.parameters[parameterCount - 1] : 0;
    if (parameterCount == 0)
        parameterCount = 1;
    // Accumulated locally and folded into terminal state once at the end. The
    // per-digit op is a single or against a bit that only moves at a separator,
    // rather than a store into t.parser per digit.
    std::uint32_t nonEmptyMask = 0;
    std::uint32_t currentBit = 1u << (parameterCount - 1);
    // Carried on the terminal rather than local to this call: a sequence can
    // arrive split across writes, and the digits after a refused separator are
    // as unwelcome in the next buffer as in this one.
    bool didOverflow = t.parser.didParametersOverflow;
    // Overflow is refused at a separator, and refusing it leaves every slot
    // taken — so from that point a digit has nowhere of its own to go and a
    // separator has no slot to open. Both are inert, which makes the tail a
    // drain and lets the body below carry no overflow test at all.
    while (!didOverflow && start + n < length) {
        // One unsigned subtraction classifies the byte: 0..9 digit, 10 ':', 11 ';',
        // anything else ends the run.
        const unsigned digitValue = static_cast<unsigned>(static_cast<unsigned char>(buffer[start + n]) - 0x30u);
        if (digitValue > 11u)
            break;
        if (digitValue <= 9u) {
            // Saturating decimal append (post-multiply clamp ==
            // decimalAppendSaturated(.., maxParameterValue)).
            currentParameter = currentParameter * 10 + static_cast<int>(digitValue);
            if (currentParameter > VT::maxParameterValue)
                currentParameter = VT::maxParameterValue;
            nonEmptyMask |= currentBit;
        } else if (parameterCount < VT::maxParameters) {
            t.parser.parameters[parameterCount - 1] = currentParameter;
            // resetCSI zeroed the array for this sequence, so only a colon has
            // anything to write: a semicolon — nearly every separator a control
            // carries — would be storing the zero the slot already holds.
            if (digitValue == 10u)
                t.parser.parameterSeparatorWasColon[parameterCount] = 1;
            parameterCount++;
            currentParameter = 0;
            currentBit <<= 1;
        } else {
            didOverflow = true; // a separator with every slot taken
        }
        ++n;
    }
    // Past the last slot the digits belong to a parameter this terminal does not
    // keep, and the slot they would reach is the last one it does — so the last
    // parameter came back with the digits of the one after it appended,
    // saturated at 65535. Dropping them is what xterm does.
    while (didOverflow && start + n < length) {
        const unsigned digitValue = static_cast<unsigned>(static_cast<unsigned char>(buffer[start + n]) - 0x30u);
        if (digitValue > 11u)
            break;
        ++n;
    }
    t.parser.parameters[parameterCount - 1] = currentParameter;
    t.parser.parameterCount = parameterCount;
    t.parser.didParametersOverflow = didOverflow;
    t.parser.parameterWasEmpty &= ~nonEmptyMask;
    return n;
}

// The states inside an escape or control sequence, where the next byte is part
// of the sequence rather than text or a string's payload.
static constexpr bool isEscapeOrControlSequenceState(ParserState state)
{
    switch (state) {
    case ParserState::Escape:
    case ParserState::EscapeIntermediate:
    case ParserState::EscapeIntermediateGeneric:
    case ParserState::EscapeCodingSystem:
    case ParserState::Hash:
    case ParserState::CharacterSet:
    case ParserState::CharacterSetIntermediate:
    case ParserState::CSI:
    case ParserState::DCS:
        return true;
    default:
        return false;
    }
}

static void processByteInner(Ubiquitty::Terminal& t, uint8_t c, GraphicCapture capture)
{
    if (t.printMode == 5) [[unlikely]] {
        const char byte = static_cast<char>(c);
        (void)consumePrinterBytes(t, { &byte, 1 });
        return;
    }
    // A byte reaching the slow driver has left the scroll lane's owned alphabet;
    // commit deferred accounting before the handler can observe it.
    Ubiquitty::Screen::scrollLaneFlush(t);
    // Tek 4014 mode: route every byte into the Tek state machine. The only way
    // back to VT is the in-mode ESC ETX / ESC CAN dispatch (tekExit).
    if (t.isTekModeActive) {
        Ubiquitty::tekByte(t, c);
        return;
    }

    // Non-ANSI legacy emulation bypasses the VT DFA for its own state machine.
    if (t.emulation != VT::Emulation::VT && emulationIsNonANSI(t)) {
        legacyProcessByte(t, c);
        return;
    }

    // UTF-8 continuation routing: 0x80..0xFF mid-sequence are continuations —
    // route to putCharacter so the C1 actions don't fire on them. In UTF-8 input mode
    // the same holds at a boundary: 0x80..0x9F is not performed as an 8-bit C1
    // (it is a stray byte for the decoder), so a raw C1 introducer must not fire here.
    if ((t.parser.parseState == ParserState::Ground || t.parser.parseState == ParserState::VT52Ground) && c >= 0x80 && (t.parser.utf8.need > 0 || (t.parser.parserInputIsUTF8 && c <= 0x9F))) {
        putCharacter(t, c, capture);
        return;
    }

    // A C1 control spelled in UTF-8, C2 80..C2 9F, between the bytes of an
    // escape or control sequence is ignored and the sequence carries on: xterm
    // decodes UTF-8 before it parses, and ignores a C1 code point in every
    // state (charproc.c doparsing). Any other byte after a C2 is read as it
    // would have been without it.
    if (t.parser.parserInputIsUTF8 && isEscapeOrControlSequenceState(t.parser.parseState)) {
        if (t.parser.utf8.need == 0 && c == 0xC2) {
            std::uint32_t unused;
            Ubiquitty::Unicode::utf8Feed(&t.parser.utf8, c, &unused);
            return;
        }
        if (t.parser.utf8.need > 0) {
            std::uint32_t codePoint;
            const Ubiquitty::Unicode::UTF8Status status = Ubiquitty::Unicode::utf8Feed(&t.parser.utf8, c, &codePoint);
            resetUTF8(t);
            if (status == Ubiquitty::Unicode::UTF8Status::Emit && isC1CodePoint(codePoint))
                return;
        }
    }

    // A byte that cannot continue an incomplete sequence closes it. A printable
    // byte reaches the decoder through putCharacter and is handled there; a
    // control or an escape is dispatched below and never would be, so the
    // truncated character is substituted here — before the control acts, and in
    // the order the stream described.
    if (t.parser.utf8.need > 0 && c < 0x80)
        closeTruncatedUTF8(t, c == '\r' || c == '\n' ? GraphicCapture::Discard : GraphicCapture::Retain);

    if (c == 0x9C && c1StringTerminatorEnabled(t)) {
        switch (t.parser.parseState) {
        case ParserState::OSC:
        case ParserState::OSCEscape:
            t.parser.parseState = ParserState::Ground;
            actOSCEnd(t, c);
            return;
        case ParserState::DCS:
        case ParserState::DCSPassthrough:
        case ParserState::DCSEscape:
            t.parser.parseState = ParserState::Ground;
            actDCSTerminate(t, c);
            return;
        case ParserState::APC:
        case ParserState::APCEscape:
            t.parser.parseState = ParserState::Ground;
            actAPCEnd(t, c);
            return;
        case ParserState::SOSPMAPC:
        case ParserState::SOSPMAPCEscape:
        case ParserState::SOS:
        case ParserState::SOSEscape:
            t.parser.parseState = ParserState::Ground;
            return;
        default:
            break;
        }
    }

    Ubiquitty::VT::Parser::Transition tr = Ubiquitty::VT::Parser::transition(t.parser.parseState, c);
    t.parser.parseState = tr.next;
    dispatchAction(t, c, tr.action);
}

} // namespace Ubiquitty::VT

namespace Ubiquitty {

// Shared body for processBytes (StopAtSyncClear=false) and
// processBytesUpToSyncClear (=true). Returns bytes consumed. When
// StopAtSyncClear, returns right after the byte that CLEARS DECSET 2026 (the ESU
// final) — that byte leaves a COMPLETE synchronized frame on the grid.
// StopAtSyncClear is a template constant, so the false path compiles with no
// sync-tracking — codegen-identical to a loop that never knew about 2026.
template<bool StopAtSyncClear, VT::GraphicCapture Capture> static int processBytesImpl(Ubiquitty::Terminal& t, std::span<const char> input, bool* stoppedAtSyncClear)
{
    const char* const buffer = input.data();
    const int length = static_cast<int>(input.size());
    [[maybe_unused]] bool wasSyncOutput = StopAtSyncClear ? t.modes.syncOutput : false;
    const auto initialEncoding = t.parser.inputEncoding.enc;
    bool didChangeEncoding = false;
    int i = 0;
    // Non-ANSI legacy emulation runs a grammar the batchers don't model, so route
    // every byte through the legacy state machine. Hoisted above the batched loop
    // so the VT default is codegen-identical (tmux never selects a legacy emulation).
    if (t.emulation != VT::Emulation::VT && VT::emulationIsNonANSI(t)) {
        for (; i < length; i++) {
            uint8_t c = static_cast<uint8_t>(buffer[i]);
            VT::recordTraceByte(t, c);
            VT::legacyProcessByte(t, c);
        }
        Ubiquitty::Screen::scrollLaneFlush(t); // lane state never crosses a processBytes return
        return i;
    }
    if (!t.integration.tmux.enabled && !t.integration.tmux.autoDetect) {
        // Set by the sync-clear stop sites: their `break` exits only the inner
        // loop, so the stop must be re-asserted to exit the segment loop too.
        // Folds away when !StopAtSyncClear.
        [[maybe_unused]] bool stopAtSyncClearHit = false;
        // Reply-pressure stop: checked at the same dispatch sites as the
        // sync clear (replies only grow through a dispatch), sharing the
        // sites' break-out plumbing. Folds away when !StopAtSyncClear.
        [[maybe_unused]] bool didHitReplyPressure = false;
        int textRunDecodeBudget = VT::textRunUnlimitedDecodeBytes;
        while (i < length) {
            if (t.printMode == 5) [[unlikely]] {
                const auto consumed = VT::consumePrinterBytes(t, input.subspan(static_cast<std::size_t>(i)));
#ifndef NDEBUG
                for (std::size_t traced = 0; traced < consumed; ++traced)
                    VT::recordTraceByte(t, static_cast<uint8_t>(buffer[i + traced]));
#endif
                i += static_cast<int>(consumed);
                continue;
            }
            // Tek 4014 is a segment property: entered by one dispatch (DECSET 38), left
            // by one in-mode sequence (tekByte). A dedicated segment loop keeps isTekModeActive
            // out of the per-byte guard set (re-tested only after dispatch sites that set it).
            if (t.isTekModeActive) [[unlikely]] {
                Ubiquitty::Screen::scrollLaneFlush(t);
                for (; i < length && t.isTekModeActive; i++) {
                    const uint8_t c = static_cast<uint8_t>(buffer[i]);
                    VT::recordTraceByte(t, c);
                    Ubiquitty::tekByte(t, c);
                }
                continue;
            }
            for (; i < length; i++) {
                const uint8_t c = static_cast<uint8_t>(buffer[i]);
                VT::recordTraceByte(t, c);
                // State-first dispatch: parseState loaded once per byte, feeding a
                // mutually-exclusive state tree. Within a state, measure before
                // reordering the lanes.
                const VT::ParserState state = t.parser.parseState;
                {
                    if (state == VT::ParserState::Ground) {
                        if (t.parser.utf8.need == 0) {
                            // Bare LF, inlined: no sync-output side effect, exactly
                            // Action::ExecuteLineFeed without the DFA dispatch.
                            if (c == 0x0A) {
                                if (t.shouldEndSemanticRoleAtLineBreak) [[unlikely]]
                                    VT::finishLineSemanticRole(t);
                                VT::resetUTF8(t);
                                t.parser.precedingItem.setControl(0x0A);
                                if (t.modes.lnm)
                                    t.screens.active->cursorX = Screen::returnColumn(t);
                                // Deferred-state scroll lane: an eligible at-bottom flood
                                // defers per-scroll accounting to Ubiquitty::Screen::scrollLaneFlush. Once a
                                // run is open its eligibility is invariant (every
                                // invalidator closes the run), so only the first feed pays
                                // the full check.
                                if (t.scrollLaneRun.active || Ubiquitty::Screen::scrollLaneEligible(t))
                                    Ubiquitty::Screen::scrollLaneLinefeed(t);
                                else
                                    VT::linefeed(t);
                                if (t.modes.lnm && t.shouldWriteRightToLeft)
                                    t.screens.active->cursorX = Screen::returnColumn(t);
                                Screen::noteStringStart(t);
                                continue;
                            }
                            // Fast-path the CSI introducer: ESC '[' in Ground always
                            // means CSI entry — skip the two per-byte transition()
                            // round-trips (Ground->Escape, Escape->CSI).
                            if (c == 0x1B && i + 1 < length && static_cast<uint8_t>(buffer[i + 1]) == '[') {
                                // Blank-invariant SGR continuation: a plain SGR
                                // (parameter bytes then final 'm', both in this
                                // buffer) mutates only the pen — it cannot observe
                                // deferred scroll state — so the run stays open;
                                // csiH_sgr flushes after apply iff the BCE blank
                                // changed. Any other final, intermediates/privates,
                                // or a sequence split across the chunk boundary
                                // flushes here (flush timing is not observable
                                // state, so chunk-invariance holds either way).
                                bool laneSurvivesSGR = false;
                                if (t.scrollLaneRun.active || t.bandScrollRun.active) {
                                    const int parameterStart = i + 2;
                                    if (parameterStart < length) {
                                        const uint8_t firstByte = static_cast<uint8_t>(buffer[parameterStart]);
                                        if (firstByte >= 0x30 && firstByte <= 0x3B) {
                                            // Accumulation touches only CSI fields, which a
                                            // scroll commit does not observe. Parse once to
                                            // identify the final, then leave it to the usual
                                            // dispatcher and its sync/reply-pressure checks.
                                            VT::resetCSI(t);
                                            const int parameterEnd = parameterStart + VT::scanCSIParameters(t, buffer, parameterStart, length);
                                            if (parameterEnd == length || buffer[parameterEnd] != 'm')
                                                Ubiquitty::Screen::scrollLaneFlush(t);
                                            VT::recordTraceByte(t, static_cast<uint8_t>(buffer[i + 1]));
                                            VT::recordTraceByte(t, firstByte);
                                            t.parser.parseState = VT::ParserState::CSI;
                                            i = parameterEnd - 1; // the next iteration owns the unconsumed byte
                                            continue;
                                        }
                                        laneSurvivesSGR = firstByte == 'm';
                                    }
                                }
                                if (!laneSurvivesSGR)
                                    Ubiquitty::Screen::scrollLaneFlush(t); // leaving the lane's owned alphabet
                                VT::recordTraceByte(t, static_cast<uint8_t>(buffer[i + 1]));
                                VT::resetCSI(t);
                                t.parser.parseState = VT::ParserState::CSI;
                                ++i; // consume '['; the loop's i++ consumes the ESC
                                continue;
                            }
                            // CR, inlined (actExecuteCarriageReturn verbatim: status-line
                            // redirect, decoder reset, REP record, DECLRMM landing column).
                            if (c == 0x0D) {
                                VT::finishAlignedTabString(t);
                                VT::resetUTF8(t);
                                t.parser.precedingItem.setControl(0x0D);
                                if (VT::statusCursorMoveTo(t, t.shouldWriteRightToLeft ? t.columns - 1 : 0))
                                    continue;
                                Ubiquitty::Screen::State* s = t.screens.active;
                                s->cursorX = Ubiquitty::Screen::returnColumn(t);
                                s->wrapPending = false;
                                continue;
                            }
                            // Batch a run of non-ASCII printable glyphs into one span
                            // write. VT::emitTextRun bails to 0 at the first emoji/combining
                            // code point or incomplete sequence, leaving those to the
                            // per-byte path. Gated off a pending single shift, which the
                            // per-character resolve consumes, and off raw GR graphic bytes.
                            // A stray byte the run substitutes U+FFFD for starts
                            // a run too: malformed text is mostly text.
                            if ((static_cast<unsigned char>(c) >= 0xC2 || (static_cast<unsigned char>(c) >= 0x80 && t.parser.invalidUTF8Policy == Ubiquitty::Unicode::InvalidUTF8Policy::FFFD))
                                && t.singleShift == 0 && !VT::isGRGraphicByte(t, static_cast<uint8_t>(c))) {
                                int consumed = 0;
                                if (VT::shouldPeekClusterLead(buffer, i, length) && VT::shouldRouteUTF8LeadToClusterRun(t, buffer, i, length))
                                    consumed = VT::emitClusterRun<Capture>(t, buffer, i, length); // emoji/combining
                                if (consumed == 0) {
                                    consumed = VT::emitTextRun<Capture>(t, buffer, i, length, textRunDecodeBudget);
                                    if (consumed == 0)
                                        consumed = VT::emitClusterRun<Capture>(t, buffer, i, length); // emoji/combining
                                }
                                if (consumed > 0) {
                                    i += consumed - 1;
                                    continue;
                                }
                            }
                            if (c >= 0x20 && c < 0x7F) {
                                // Batch a printable-ASCII run into one span write, but
                                // only when the NEXT byte is also clean ASCII (a run of
                                // >= 2); a lone printable takes VT::putCharacter, which
                                // skips the run writer's per-call setup.
                                if (i + 1 < length) {
                                    const unsigned char c1 = static_cast<unsigned char>(buffer[i + 1]);
                                    if (c1 >= 0x20 && c1 < 0x7F) [[likely]] {
                                        int consumed = VT::emitASCIIRun<Capture>(t, buffer, i, length);
                                        if (consumed >= 2) {
                                            i += consumed - 1;
                                            continue;
                                        }
                                    } else if ((c1 == 0xCC || c1 == 0xCD) && i + 2 < length) {
                                        // ASCII base + U+0300..036F combining mark. The
                                        // batcher validates the trail and returns 0 for a
                                        // non-mark trail, falling to VT::putCharacter.
                                        int consumed = VT::emitASCIICombiningRun<Capture>(t, buffer, i, length);
                                        if (consumed > 0) {
                                            i += consumed - 1;
                                            continue;
                                        }
                                    }
                                }
                                VT::putCharacter(t, c, VT::graphicCaptureBefore<Capture>(buffer, i + 1, length));
                                continue;
                            }
                        }
                        // Mid-UTF-8 or an unhandled Ground byte: per-byte driver.
                    } else if (state == VT::ParserState::CSI) {
                        // Batch the CSI body: parameter runs in one scan, plus the final
                        // byte (0x40..0x7E) and private prefix (0x3C..0x3F) inline.
                        // Intermediates, embedded C0, and DEL keep the per-byte driver.
                        if (c >= 0x30 && c <= 0x7E) {
                            if (c <= 0x3B) {
                                int consumed = VT::scanCSIParameters(t, buffer, i, length);
                                // The byte after a parameter run is the final byte for
                                // any sequence without intermediates — every SGR.
                                if (i + consumed < length) {
                                    const uint8_t finalByte = static_cast<uint8_t>(buffer[i + consumed]);
                                    if (finalByte >= 0x40 && finalByte <= 0x7E) {
                                        VT::recordTraceByte(t, finalByte);
                                        t.parser.parseState = VT::ParserState::Ground;
                                        VT::actCSIDispatch(t, finalByte);
                                        i += consumed; // loop's i++ steps past the final byte
                                        // A second site (besides VT::processByteInner) where a
                                        // CSI dispatch can clear syncOutput (DECRST 2026), so
                                        // the sync-clear stop is checked here too.
                                        if constexpr (StopAtSyncClear) {
                                            if (wasSyncOutput && !t.modes.syncOutput) {
                                                ++i;
                                                stopAtSyncClearHit = true;
                                                break;
                                            }
                                            wasSyncOutput = t.modes.syncOutput;
                                            if (t.outputLength >= Ubiquitty::Terminal::outputReplyPressureBytes) {
                                                ++i;
                                                didHitReplyPressure = true;
                                                break;
                                            }
                                        }
                                        // DECSET 38 enters Tek right here.
                                        if (t.isTekModeActive || t.printMode == 5) [[unlikely]] {
                                            ++i;
                                            break;
                                        }
                                        continue;
                                    }
                                }
                                i += consumed - 1;
                                continue;
                            }
                            if (c >= 0x40) {
                                // Immediate final, no parameter run (CSI m, CSI H).
                                t.parser.parseState = VT::ParserState::Ground;
                                VT::actCSIDispatch(t, c);
                                // Third sync-clear dispatch site (a bare CSI final can
                                // never be DECRST 2026, but the template contract is
                                // checked at every dispatch).
                                if constexpr (StopAtSyncClear) {
                                    if (wasSyncOutput && !t.modes.syncOutput) {
                                        ++i;
                                        stopAtSyncClearHit = true;
                                        break;
                                    }
                                    wasSyncOutput = t.modes.syncOutput;
                                    if (t.outputLength >= Ubiquitty::Terminal::outputReplyPressureBytes) {
                                        ++i;
                                        didHitReplyPressure = true;
                                        break;
                                    }
                                }
                                // DECSET 38 enters Tek right here.
                                if (t.isTekModeActive || t.printMode == 5) [[unlikely]] {
                                    ++i;
                                    break;
                                }
                                continue;
                            }
                            VT::actCSIPrivatePrefix(t, c);
                            continue;
                        }
                    } else if (state == VT::ParserState::VT52Ground) {
                        // VT52 ground keeps the per-byte path for everything but a
                        // clean printable (its characterSet gating differs from ANSI
                        // Ground, so no run batching). Ordered before the >= OSC
                        // payload gate: VT52Ground sorts after OSC in the State enum,
                        // so the range test would otherwise swallow it.
                        if (t.parser.utf8.need == 0 && c >= 0x20 && c < 0x7F) {
                            VT::putCharacter(t, c);
                            continue;
                        }
                    } else if (state >= VT::ParserState::OSC) [[unlikely]] {
                        static_assert(VT::ParserState::CSI < VT::ParserState::OSC
                                && VT::ParserState::OSC < VT::ParserState::DCSPassthrough
                                && VT::ParserState::OSC < VT::ParserState::APC
                                && VT::ParserState::OSC < VT::ParserState::VT52Ground,
                            "the payload gate needs the control-string and VT52 states after CSI");
                        // The Linux console private OSC (ESC ] P / ESC ] R) is fixed-length
                        // and unterminated, so it must run per-byte: through actOSCPut,
                        // which recognises and dispatches it in the Linux emulation, and
                        // past the check that abandons it in every other. The bulk
                        // OSC-payload batcher would swallow the trailing text.
                        if (state == VT::ParserState::OSC && t.emulation != VT::Emulation::Linux && !VT::beginsLinuxConsoleOSC(t, c)) {
                            int consumed = VT::appendOSCPayloadRun(t, buffer, i, length);
                            if (consumed > 0) {
                                i += consumed - 1;
                                if (t.isTekModeActive || t.printMode == 5) [[unlikely]] {
                                    ++i;
                                    break;
                                }
                                continue;
                            }
                        } else if (state == VT::ParserState::APC) {
                            int consumed = VT::appendAPCPayloadRun(t, buffer, i, length);
                            if (consumed > 0) {
                                i += consumed - 1;
                                if (t.isTekModeActive || t.printMode == 5) [[unlikely]] {
                                    ++i;
                                    break;
                                }
                                continue;
                            }
                        } else if (state == VT::ParserState::DCSPassthrough) {
                            // Sixel / tmux-passthrough payloads are megabytes; batch the
                            // run so they skip the per-byte DFA like OSC and APC.
                            int consumed = VT::appendDCSPayloadRun(t, buffer, i, length);
                            if (consumed > 0) {
                                i += consumed - 1;
                                if (t.isTekModeActive || t.printMode == 5) [[unlikely]] {
                                    ++i;
                                    break;
                                }
                                continue;
                            }
                        }
                    }
                }
                // A scalar that falls out of the run writers (for example at a
                // pending wrap) still has a known successor when its final
                // continuation is in this buffer. Preserve cancellation only
                // when that successor could leave the graphic cancellable.
                const auto capture = state == VT::ParserState::Ground && t.parser.utf8.need == 1
                    ? VT::graphicCaptureBefore<Capture>(buffer, i + 1, length)
                    : VT::GraphicCapture::Retain;
                VT::processByteInner(t, c, capture);
                if (t.parser.inputEncoding.enc != initialEncoding) [[unlikely]] {
                    ++i;
                    didChangeEncoding = true;
                    break;
                }
                if constexpr (StopAtSyncClear) {
                    // syncOutput flips only through an escape/CSI dispatch (here and
                    // the CSI lanes above), never in VT::putCharacter or the batched runs.
                    if (wasSyncOutput && !t.modes.syncOutput) {
                        ++i;
                        stopAtSyncClearHit = true;
                        break;
                    }
                    wasSyncOutput = t.modes.syncOutput;
                    // Replies grow only through a dispatch too, so this site and
                    // the CSI lanes above cover every producer.
                    if (t.outputLength >= Ubiquitty::Terminal::outputReplyPressureBytes) {
                        ++i;
                        didHitReplyPressure = true;
                        break;
                    }
                }
                // DECSET 38 through the per-byte driver enters Tek.
                if (t.isTekModeActive || t.printMode == 5) [[unlikely]] {
                    ++i;
                    break;
                }
            }
            if (didChangeEncoding)
                break;
            if constexpr (StopAtSyncClear) {
                if (stopAtSyncClearHit || didHitReplyPressure)
                    break;
            }
        }
        if constexpr (StopAtSyncClear) {
            if (stopAtSyncClearHit && stoppedAtSyncClear)
                *stoppedAtSyncClear = true;
        }
    } else {
        if constexpr (!StopAtSyncClear) {
#ifndef NDEBUG
            for (int traceIndex = i; traceIndex < length; ++traceIndex)
                VT::recordTraceByte(t, static_cast<uint8_t>(buffer[traceIndex]));
#endif
            Ubiquitty::Protocol::Tmux::feedInputBytes(&t.integration.tmux, { buffer + i, static_cast<std::size_t>(length - i) }, &t,
                VT::tmuxDeliver, VT::tmuxDeliverSpan);
            i = length;
        } else {
            for (; i < length; i++) {
                processByte(&t, static_cast<uint8_t>(buffer[i]));
                if (wasSyncOutput && !t.modes.syncOutput) {
                    ++i;
                    if (stoppedAtSyncClear)
                        *stoppedAtSyncClear = true;
                    break;
                }
                wasSyncOutput = t.modes.syncOutput;
                if (t.outputLength >= Ubiquitty::Terminal::outputReplyPressureBytes) {
                    ++i;
                    break;
                }
            }
        }
    }
    // Lane state never crosses a processBytes return — except the flood
    // pipeline's input-exhausted bite return (<true>, i == length), whose small
    // bites would otherwise cap each run at a few lines. Paint readers flush at
    // their own entry instead (captureSnapshotFromTerm, the render model-read
    // seam); an early <true> return is a sync-clear stop, where a capture
    // follows immediately, so it still flushes here.
    if constexpr (!StopAtSyncClear)
        Ubiquitty::Screen::scrollLaneFlush(t);
    else if (i < length)
        Ubiquitty::Screen::scrollLaneFlush(t);
    return i;
}

// Under a single-byte encoding, a GR byte that an invoked GR set or a pending
// single shift claims bypasses the decoder and reaches the parser raw, where
// isGRGraphicByte resolves it. Outside Ground it is string or sequence content
// and decodes like any other byte.
static bool shouldRouteGRByteRaw(const Ubiquitty::Terminal& t, std::uint8_t byte)
{
    return byte >= 0xA0 && (t.characterSetGR != 0 || t.singleShift != 0) && t.parser.parseState == VT::ParserState::Ground;
}

template<bool StopAtSyncClear> static int processBytesWithGraphicCapture(Ubiquitty::Terminal& t, std::span<const char> input, bool* stoppedAtSyncClear)
{
    if (t.integration.tmux.enabled || t.integration.tmux.autoDetect
        || (t.emulation != VT::Emulation::VT && VT::emulationIsNonANSI(t)))
        return processBytesImpl<StopAtSyncClear>(t, input, stoppedAtSyncClear);
    if constexpr (StopAtSyncClear) {
        if (t.outputLength >= Ubiquitty::Terminal::outputReplyPressureBytes)
            return processBytesImpl<StopAtSyncClear>(t, input, stoppedAtSyncClear);
    }
    const int boundary = VT::graphicCancellationBoundary(input);
    if (boundary == 0)
        return processBytesImpl<StopAtSyncClear>(t, input, stoppedAtSyncClear);

    const auto prefixLength = static_cast<std::size_t>(boundary + 1);
    bool didStopAtSyncClear = false;
    const int consumed = processBytesImpl<StopAtSyncClear, VT::GraphicCapture::Discard>(t, input.first(prefixLength), &didStopAtSyncClear);
    if (didStopAtSyncClear && stoppedAtSyncClear)
        *stoppedAtSyncClear = true;
    bool shouldReturn = static_cast<std::size_t>(consumed) != prefixLength || didStopAtSyncClear;
    if constexpr (StopAtSyncClear) {
        if (t.outputLength >= Ubiquitty::Terminal::outputReplyPressureBytes)
            shouldReturn = true;
    }
    if (shouldReturn) {
        if constexpr (StopAtSyncClear) {
            if (static_cast<std::size_t>(consumed) < input.size())
                Ubiquitty::Screen::scrollLaneFlush(t);
        }
        return consumed;
    }
    if (prefixLength == input.size())
        return consumed;
    return consumed + processBytesImpl<StopAtSyncClear>(t, input.subspan(prefixLength), stoppedAtSyncClear);
}

template<bool StopAtSyncClear> static int processEncodedBytesImpl(Ubiquitty::Terminal& t, std::span<const char> input, bool* stoppedAtSyncClear)
{
    const auto* in = reinterpret_cast<const std::uint8_t*>(input.data());
    const int length = static_cast<int>(input.size());
    int off = 0;
    [[maybe_unused]] bool wasSyncOutput = t.modes.syncOutput;
    std::array<std::uint8_t, 4096> staging;
    constexpr int maxRunBytes = static_cast<int>(staging.size() / 4);
    while (off < length) {
        if (t.printMode == 5 && !t.integration.tmux.enabled && !t.integration.tmux.autoDetect) [[unlikely]] {
            off += static_cast<int>(VT::consumePrinterBytes(t, input.subspan(static_cast<std::size_t>(off))));
            continue;
        }
        if (Ubiquitty::Unicode::encodingIsUTF8(t.parser.inputEncoding.enc)) {
            const bool wasUTF8 = t.parser.parserInputIsUTF8;
            t.parser.parserInputIsUTF8 = true;
            const int consumed = processBytesWithGraphicCapture<StopAtSyncClear>(t, input.subspan(static_cast<std::size_t>(off)), stoppedAtSyncClear);
            t.parser.parserInputIsUTF8 = wasUTF8;
            off += consumed;
            // The UTF-8 parser yields at a coding-system change. Any other
            // short consumption belongs to the sync-clear/reply-pressure API.
            if (consumed == 0 || Ubiquitty::Unicode::encodingIsUTF8(t.parser.inputEncoding.enc))
                break;
            wasSyncOutput = t.modes.syncOutput;
            continue;
        }
        // A single-byte coding reserves raw C1 for controls. Multibyte
        // codings need those bytes as leads, continuations and single shifts;
        // only ST in an open string bypasses their decoder.
        const bool shouldRouteC1ToParser = Ubiquitty::Unicode::encodingIsSingleByte(t.parser.inputEncoding.enc);
        const std::uint8_t head = in[off];
        const bool isC1Control = shouldRouteC1ToParser ? (head >= 0x80 && head <= 0x9F) : (head == 0x9C && VT::isStringState(t.parser.parseState));
        if (isC1Control || (shouldRouteC1ToParser && shouldRouteGRByteRaw(t, head))) {
            VT::processByteInner(t, head);
            ++off;
        } else {
            // Runs end before ESC and sequence bytes are decoded one at a
            // time: a DOCS final can change the decoder for the very next byte.
            // Text between controls keeps the batched transcode path.
            const int windowLength = std::min(length - off, maxRunBytes);
            int runLength = 1;
            if constexpr (!StopAtSyncClear) {
                if (head != 0x1B && !VT::isEscapeOrControlSequenceState(t.parser.parseState)) {
                    while (runLength < windowLength) {
                        const std::uint8_t byte = in[off + runLength];
                        if (byte == 0x1B || (shouldRouteC1ToParser ? (byte >= 0x80 && byte <= 0x9F) || (byte >= 0xA0 && (t.characterSetGR != 0 || t.singleShift != 0)) : byte == 0x9C))
                            break;
                        ++runLength;
                    }
                }
            }
            std::size_t consumed = 0;
            const std::size_t written = Ubiquitty::Unicode::encDecodeToUTF8(&t.parser.inputEncoding, { in + off, static_cast<std::size_t>(runLength) }, staging, &consumed);
            if (written > 0) {
                const bool wasUTF8 = t.parser.parserInputIsUTF8;
                t.parser.parserInputIsUTF8 = true;
                processBytesWithGraphicCapture<StopAtSyncClear>(t, { reinterpret_cast<const char*>(staging.data()), written }, nullptr);
                t.parser.parserInputIsUTF8 = wasUTF8;
            }
            if (consumed == 0)
                break;
            off += static_cast<int>(consumed);
        }
        if constexpr (StopAtSyncClear) {
            if (wasSyncOutput && !t.modes.syncOutput) {
                if (stoppedAtSyncClear)
                    *stoppedAtSyncClear = true;
                break;
            }
            wasSyncOutput = t.modes.syncOutput;
            if (t.outputLength >= Ubiquitty::Terminal::outputReplyPressureBytes)
                break;
        }
    }
    return off;
}

void settlePasswordPrompt(Terminal& terminal, PAL::ProcessFacts* process)
{
    VT::evaluatePasswordPromptAtCursor(terminal, process);
}

void processBytes(Ubiquitty::Terminal* t, std::string_view buffer)
{
    if (!t)
        return;
    processBytes(t, buffer, nullptr);
}

void processBytes(Ubiquitty::Terminal* t, std::string_view buffer, PAL::ProcessFacts* process)
{
    if (!t)
        return;
    // Session log captures the raw wire before tmux framing decode.
    if ((t->sessionLogCaptureEnabled || t->loggingEnabled) && t->sessionLogFile && !buffer.empty()) {
        std::fwrite(buffer.data(), 1, buffer.size(), static_cast<FILE*>(t->sessionLogFile));
    }
    // The terminal's ambiguous-width choice is the frontend's to move and moves
    // outside any byte stream; the terminal notices it here, at the head of the
    // next output, so a program that asked (?7700) hears before the text it drew
    // is measured.
    if (t->modes.shouldReportAmbiguousWidth)
        VT::announceAmbiguousWidthIfMoved(*t);
    const int consumed = processEncodedBytesImpl<false>(*t, buffer, nullptr);
    if (consumed > 0) {
        if (t->modes.shouldSaveCRT || t->isCRTBlanked) {
            if (const auto now = terminalNow(*t))
                (void)Modes::DisplayIdle::noteInput(*t, *now);
        }
        ++t->modelEpoch;
    }
    // A pass that changed a block's cells removed the block; nothing reads the
    // blocks before the pass ends.
    Ubiquitty::RichText::reconcileBlocks(*t);
    VT::tmuxDeliverFlush(*t);
    // Drain completed async sixel decodes (the TaskQueue runs inline at submit).
    VT::termDrainSixel(t);
    // Output settled — re-check whether the cursor sits at a password prompt
    // (config-gated).
    settlePasswordPrompt(*t, process);
}

// Process up to (and including) the next DECSET 2026 clear or the reply-pressure
// line, returning bytes consumed, so the caller can snapshot the completed
// synchronized frame at the ESU instant — or drain the reply buffer before it
// overflows. `stoppedAtSyncClear` (nullable) tells the two stops apart; it is
// set only for the 2026 clear. Consumes everything when neither stop is present.
// The session log is written per consumed slice (concatenation matches
// processBytes's log).
int processBytesUpToSyncClear(Ubiquitty::Terminal* t, std::string_view buffer, bool* stoppedAtSyncClear)
{
    if (stoppedAtSyncClear)
        *stoppedAtSyncClear = false;
    if (!t)
        return 0;
    int consumed = processEncodedBytesImpl<true>(*t, buffer, stoppedAtSyncClear);
    if (consumed > 0) {
        if (t->modes.shouldSaveCRT || t->isCRTBlanked) {
            if (const auto now = terminalNow(*t))
                (void)Modes::DisplayIdle::noteInput(*t, *now);
        }
        ++t->modelEpoch;
    }
    Ubiquitty::RichText::reconcileBlocks(*t);
    VT::tmuxDeliverFlush(*t);
    if ((t->sessionLogCaptureEnabled || t->loggingEnabled) && t->sessionLogFile && consumed > 0) {
        std::fwrite(buffer.data(), 1, static_cast<size_t>(consumed), static_cast<FILE*>(t->sessionLogFile));
    }
    // No password settle here: a batch on this entry is one kernel quantum of
    // a flood, and the owner's facts cost an ioctl. The runtime settles once
    // the writer pauses (settlePasswordPrompt).
    if (consumed >= static_cast<int>(buffer.size()))
        VT::termDrainSixel(t);
    return consumed;
}

int processBytesUpToSyncClear(Ubiquitty::Terminal* t, std::string_view buffer)
{
    if (!t)
        return 0;
    return processBytesUpToSyncClear(t, buffer, nullptr);
}

// Drain every per-domain async-decode queue (Kitty PNG, direct-pixel inflate,
// sixel). Cheap per tick; processBytes() calls this internally too.
void drainAsyncDecodes(Ubiquitty::Terminal& t)
{
    VT::termDrainSixel(&t);
    Ubiquitty::Kitty::drainAsyncDecodes(&t);
}

} // namespace Ubiquitty

// VT::Parser dispatch API. The handler tables live in this TU (their handlers
// are file-static); VT/Parser.h exposes lookup APIs that forward here.

namespace Ubiquitty::VT::Parser {

Handler findCSIPlain(char final) { return csiLookup(final); }

Handler findCSIPrefixed(unsigned char priv, unsigned char intermediate, char final)
{
    const CSIPrefixedEntry* e = csiPrefixedLookup(priv, intermediate, final);
    return e ? e->fn : nullptr;
}

// findEscape lives in VT/Escape.cpp alongside escapeLookup.

DCSHandler findDCS(unsigned char intermediate, char final)
{
    const DCSEntry* e = dcsLookup(intermediate, final);
    return e ? e->fn : nullptr;
}

// A C0 control inside an escape sequence still waiting for its final byte,
// and DEL, which DEC STD 070 3.3.4 treats as a control character with no function
// to ignore and which never is a final byte. Cancel and substitute end the sequence (substitute drawing its error
// character), an escape abandons it for a new
// one, and any other control executes in place while the sequence carries on,
// as in the escape-intermediate state of the Williams parser. Read as the
// final byte instead, `ESC # ESC [ H` would swallow its second escape and
// print the rest as text.
static std::optional<Transition> escapeSequenceControl(State s, unsigned char b)
{
    if (b == 0x7F)
        return Transition { Action::Noop, s };
    if (b >= 0x20)
        return std::nullopt;
    switch (b) {
    case 0x18:
        return Transition { Action::Noop, State::Ground };
    case 0x1A:
        return Transition { Action::ExecuteSubstitute, State::Ground };
    case 0x1B:
        return Transition { Action::EnterEscape, State::Escape };
    case 0x07:
        return Transition { Action::ExecuteBell, s };
    case 0x08:
        return Transition { Action::ExecuteBackspace, s };
    case 0x09:
        return Transition { Action::ExecuteHorizontalTab, s };
    case 0x0A:
    case 0x0B:
    case 0x0C:
        return Transition { Action::ExecuteLineFeed, s };
    case 0x0D:
        return Transition { Action::ExecuteCarriageReturn, s };
    case 0x0E:
        return Transition { Action::ExecuteShiftOut, s };
    case 0x0F:
        return Transition { Action::ExecuteShiftIn, s };
    default:
        return Transition { Action::Noop, s };
    }
}

// transition(state, byte) — Paul Williams style DFA driver. Pure function over
// (State, byte) → (Action, NextState); this is the parser's state machine.
// Every (state, byte) has exactly one transition (no backtracking/lookahead).
// VT52 is not modeled here — the driver short-circuits to VT52 paths first.
// Exhaustive coverage is checked by test_vt100_parser.
Transition transition(State s, unsigned char b)
{
    switch (s) {

    case State::Ground:
        switch (b) {
        case 0x05:
            return { Action::ExecuteAnswerback, State::Ground };
        case 0x07:
            return { Action::ExecuteBell, State::Ground };
        case 0x08:
            return { Action::ExecuteBackspace, State::Ground };
        case 0x09:
            return { Action::ExecuteHorizontalTab, State::Ground };
        case 0x0A:
        case 0x0B:
        case 0x0C:
            return { Action::ExecuteLineFeed, State::Ground };
        case 0x0D:
            return { Action::ExecuteCarriageReturn, State::Ground };
        case 0x0E:
            return { Action::ExecuteShiftOut, State::Ground };
        case 0x0F:
            return { Action::ExecuteShiftIn, State::Ground };
        case 0x1A:
            return { Action::ExecuteSubstitute, State::Ground };
        case 0x1B:
            return { Action::EnterEscape, State::Escape };
            // C1 controls (8-bit). The action also handles the state
        // change for the four string-state openers.
        case 0x84:
            return { Action::C1Index, State::Ground };
        case 0x85:
            return { Action::C1NextLine, State::Ground };
        case 0x88:
            return { Action::C1HorizontalTabSet, State::Ground };
        case 0x89:
            return { Action::C1HorizontalTabJustify, State::Ground };
        case 0x8A:
            return { Action::C1VerticalTabSet, State::Ground };
        case 0x8D:
            return { Action::C1ReverseIndex, State::Ground };
        case 0x90:
            return { Action::C1EnterDCS, State::DCS };
        case 0x9B:
            return { Action::C1EnterCSI, State::CSI };
        case 0x9D:
            return { Action::C1EnterOSC, State::OSC };
        case 0x9F:
            return { Action::C1EnterAPC, State::APC };
        case 0x8E:
            return { Action::C1SingleShift2, State::Ground };
        case 0x8F:
            return { Action::C1SingleShift3, State::Ground };
        case 0x94:
            return { Action::EscapeDispatch, State::Ground };
        case 0x96:
            return { Action::C1StartGuardedArea, State::Ground };
        case 0x97:
            return { Action::C1EndGuardedArea, State::Ground };
        case 0x98:
            return { Action::EnterSOSPM, State::SOS }; // SOS (start of string)
        case 0x9E:
            return { Action::EnterSOSPM, State::SOSPMAPC }; // PM (privacy message)
        case 0x9A:
            return { Action::C1Identify, State::Ground }; // DECID — 8-bit ESC Z
        case 0x7F:
            return { Action::Noop, State::Ground };
        default:
            // Undefined C1 bytes (0x80..0x83, 0x86..0x87, 0x89, 0x8B..0x8C,
            // 0x91..0x93, 0x95, 0x99, 0x9C — the defined ones have their own cases)
            // reach here only on the single-byte legacy input path: in UTF-8
            // mode VT::processByteInner has already rerouted 0x80..0x9F to
            // VT::putCharacter (a lone continuation → U+FFFD). ECMA-48 says an undefined
            // C1 is ignored in an 8-bit environment, so consume them as Noop —
            // no stray U+FFFD glyph on a Latin-1 stream.
            if (b >= 0xA0 || (b >= 0x20 && b < 0x80))
                return { Action::Print, State::Ground }; // printable / UTF-8 lead
            // C0 without a case (0x00..0x06, 0x10..0x17, 0x19, 0x1C..0x1F) and
            // undefined 8-bit C1 (0x80..0x9F): consumed, no effect.
            return { Action::Noop, State::Ground };
        }

    case State::Escape:
        // C0 controls between ESC and its final byte execute in place;
        // the escape continues. Can/SUB cancel; a second ESC restarts.
        if (b < 0x20) {
            switch (b) {
            case 0x18:
                return { Action::Noop, State::Ground };
            case 0x1A:
                return { Action::ExecuteSubstitute, State::Ground };
            case 0x1B:
                return { Action::EnterEscape, State::Escape };
            case 0x07:
                return { Action::ExecuteBell, State::Escape };
            case 0x08:
                return { Action::ExecuteBackspace, State::Escape };
            case 0x09:
                return { Action::ExecuteHorizontalTab, State::Escape };
            case 0x0A:
            case 0x0B:
            case 0x0C:
                return { Action::ExecuteLineFeed, State::Escape };
            case 0x0D:
                return { Action::ExecuteCarriageReturn, State::Escape };
            case 0x0E:
                return { Action::ExecuteShiftOut, State::Escape };
            case 0x0F:
                return { Action::ExecuteShiftIn, State::Escape };
            default:
                return { Action::Noop, State::Escape };
            }
        }
        switch (b) {
        case 0x7F:
            return { Action::Noop, State::Escape };
        case '[':
            return { Action::EnterCSI, State::CSI };
        case ']':
            return { Action::EnterOSC, State::OSC };
        case 'P':
            return { Action::EnterDCS, State::DCS };
        case '_':
            return { Action::EnterAPC, State::APC };
        case 'X':
            return { Action::EnterSOSPM, State::SOS }; // SOS
        case '^':
            return { Action::EnterSOSPM, State::SOSPMAPC }; // PM
        case '(':
            return { Action::EnterCharacterSetG0, State::CharacterSet };
        case ')':
            return { Action::EnterCharacterSetG1, State::CharacterSet };
        case '*':
            return { Action::EnterCharacterSetG2, State::CharacterSet };
        case '+':
            return { Action::EnterCharacterSetG3, State::CharacterSet };
        case '-':
            return { Action::EnterCharacterSet96G1, State::CharacterSet };
        case '.':
            return { Action::EnterCharacterSet96G2, State::CharacterSet };
        case '/':
            return { Action::EnterCharacterSet96G3, State::CharacterSet };
        case '%':
            return { Action::Noop, State::EscapeCodingSystem };
        case '#':
            return { Action::Noop, State::Hash };
        case ' ':
            return { Action::Noop, State::EscapeIntermediate };
        default:
            // Any other 0x20-0x2F intermediate needs one more byte (its final)
            // consumed before returning to Ground; no sequence in this generic
            // bucket is implemented, so their finals are silently swallowed.
            if (b >= 0x20 && b <= 0x2F)
                return { Action::Noop, State::EscapeIntermediateGeneric };
            return { Action::EscapeDispatch, State::Ground };
        }

    case State::EscapeIntermediate:
        if (const std::optional<Transition> control = escapeSequenceControl(s, b))
            return *control;
        // A second intermediate makes another escape sequence (ECMA-35 13.2),
        // none of which Core defines: it is consumed through its final.
        if (b >= 0x20 && b <= 0x2F)
            return { Action::Noop, State::EscapeIntermediateGeneric };
        return { Action::EscapeSpaceByte, State::Ground };

    case State::EscapeCodingSystem:
        if (const std::optional<Transition> control = escapeSequenceControl(s, b))
            return *control;
        if (b >= 0x20 && b <= 0x2F)
            return { Action::Noop, State::EscapeIntermediateGeneric };
        return { Action::SelectCodingSystem, State::Ground };

    case State::EscapeIntermediateGeneric:
        // ECMA-35 escapes can carry many intermediates before the final (e.g.
        // "ESC % / G" DOCS, "ESC $ ( D" ISO-2022). Collect every 0x20-0x2F byte;
        // the first byte past the range is the final, consumed back to Ground.
        if (const std::optional<Transition> control = escapeSequenceControl(s, b))
            return *control;
        if (b >= 0x20 && b <= 0x2F)
            return { Action::Noop, State::EscapeIntermediateGeneric };
        return { Action::Noop, State::Ground };

    case State::Hash:
        if (const std::optional<Transition> control = escapeSequenceControl(s, b))
            return *control;
        // As after ESC SP: a second intermediate makes a sequence Core does
        // not define, consumed through its final.
        if (b >= 0x20 && b <= 0x2F)
            return { Action::Noop, State::EscapeIntermediateGeneric };
        return { Action::HashByte, State::Ground };

    case State::CharacterSet:
        // A DRCS designator carries an optional intermediate before its final;
        // stash it. Any other byte is a plain hard-set designation's final.
        if (const std::optional<Transition> control = escapeSequenceControl(s, b))
            return *control;
        if (b >= 0x20 && b <= 0x2F)
            return { Action::CharacterSetIntermediateByte, State::CharacterSetIntermediate };
        return { Action::CharacterSetByte, State::Ground };

    case State::CharacterSetIntermediate:
        // Further intermediates fold into the last; the first non-intermediate
        // byte is the final that designates the set.
        if (const std::optional<Transition> control = escapeSequenceControl(s, b))
            return *control;
        if (b >= 0x20 && b <= 0x2F)
            return { Action::CharacterSetIntermediateByte, State::CharacterSetIntermediate };
        return { Action::CharacterSetByte, State::Ground };

    case State::CSI:
        // C0 controls embedded in a CSI are EXECUTED in place and the sequence
        // continues (VT500 state machine). Can/SUB cancel; ESC starts a new one;
        // DEL is ignored. Aborting the whole CSI on an embedded control would
        // discard the sequence, so cursor motion and insert/delete carrying an
        // embedded BS/CR would silently stop working.
        if (b < 0x20) {
            switch (b) {
            case 0x18:
                return { Action::Noop, State::Ground };
            case 0x1A:
                return { Action::ExecuteSubstitute, State::Ground };
            case 0x1B:
                return { Action::EnterEscape, State::Escape };
            case 0x07:
                return { Action::ExecuteBell, State::CSI };
            case 0x08:
                return { Action::ExecuteBackspace, State::CSI };
            case 0x09:
                return { Action::ExecuteHorizontalTab, State::CSI };
            case 0x0A:
            case 0x0B:
            case 0x0C:
                return { Action::ExecuteLineFeed, State::CSI };
            case 0x0D:
                return { Action::ExecuteCarriageReturn, State::CSI };
            case 0x0E:
                return { Action::ExecuteShiftOut, State::CSI };
            case 0x0F:
                return { Action::ExecuteShiftIn, State::CSI };
            default:
                return { Action::Noop, State::CSI };
            }
        }
        if (b == 0x7F)
            return { Action::Noop, State::CSI };
        if (b >= '0' && b <= '9')
            return { Action::CSIParameterDigit, State::CSI };
        if (b == ';')
            return { Action::CSIParameterSemicolon, State::CSI };
        if (b == ':')
            return { Action::CSIParameterColon, State::CSI };
        if (b == '<' || b == '=' || b == '>' || b == '?')
            return { Action::CSIPrivatePrefix, State::CSI };
        if (b >= 0x20 && b <= 0x2F)
            return { Action::CSIIntermediate, State::CSI };
        if (b >= 0x40 && b <= 0x7E)
            return { Action::CSIDispatch, State::Ground };
        return { Action::Noop, State::Ground };

    case State::OSC:
        if (b == 0x18) // cancel abandons the string
            return { Action::Noop, State::Ground };
        if (b == 0x1A) // substitute abandons it and draws the error character
            return { Action::ExecuteSubstitute, State::Ground };
        if (b == 0x07)
            return { Action::OSCBellEnd, State::Ground };
        if (b == 0x1B)
            return { Action::Noop, State::OSCEscape };
        return { Action::OSCPut, State::OSC };

    case State::OSCEscape:
        if (b == 0x18)
            return { Action::Noop, State::Ground };
        if (b == 0x1A) // substitute abandons it and draws the error character
            return { Action::ExecuteSubstitute, State::Ground };
        if (b == '\\')
            return { Action::OSCStringTerminatorEnd, State::Ground };
        return { Action::OSCEscapeOther, State::Escape };

    case State::DCS:
        if (b == 0x18) // cancel abandons the string
            return { Action::Noop, State::Ground };
        if (b == 0x1A) // substitute abandons it and draws the error character
            return { Action::ExecuteSubstitute, State::Ground };
        if (b == 0x07)
            return { Action::StringBell, State::DCS };
        if (b == 0x1B)
            return { Action::Noop, State::DCSEscape };
        if (b >= '0' && b <= '9')
            return { Action::DCSParameterDigit, State::DCS };
        if (b == ';')
            return { Action::DCSParameterSemicolon, State::DCS };
        if (b >= ':' && b <= '?')
            return { Action::DCSHeaderIntermediate, State::DCS };
        if (b >= 0x20 && b <= 0x2F)
            return { Action::DCSHeaderIntermediate, State::DCS };
        if (b >= 0x40 && b <= 0x7E)
            return { Action::DCSHeaderFinal, State::DCSPassthrough };
        return { Action::Noop, State::DCS };

    case State::DCSPassthrough:
        if (b == 0x18) // cancel abandons the string
            return { Action::Noop, State::Ground };
        if (b == 0x1A) // substitute abandons it and draws the error character
            return { Action::ExecuteSubstitute, State::Ground };
        if (b == 0x07)
            return { Action::StringBell, State::DCSPassthrough };
        if (b == 0x1B)
            return { Action::Noop, State::DCSEscape };
        return { Action::DCSPayloadPut, State::DCSPassthrough };

    case State::DCSEscape:
        if (b == 0x18)
            return { Action::Noop, State::Ground };
        if (b == 0x1A) // substitute abandons it and draws the error character
            return { Action::ExecuteSubstitute, State::Ground };
        if (b == '\\')
            return { Action::DCSStringTerminatorEnd, State::Ground };
        // A doubled ESC is one literal payload ESC: fold it back into the
        // payload, or the '\' of a payload's own `ESC ESC \` reaches the
        // terminator arm above and ends the outer DCS early.
        if (b == 0x1B)
            return { Action::DCSEscapeOther, State::DCSPassthrough };
        // Any other byte: the ESC ended the string unterminated and began an
        // escape, which this byte continues.
        return { Action::DCSEscapeOther, State::Escape };

    case State::APC:
        if (b == 0x18) // cancel abandons the string
            return { Action::Noop, State::Ground };
        if (b == 0x1A) // substitute abandons it and draws the error character
            return { Action::ExecuteSubstitute, State::Ground };
        if (b == 0x07)
            return { Action::Noop, State::APC };
        if (b == 0x1B)
            return { Action::Noop, State::APCEscape };
        return { Action::APCPut, State::APC };

    case State::APCEscape:
        if (b == 0x18)
            return { Action::Noop, State::Ground };
        if (b == 0x1A) // substitute abandons it and draws the error character
            return { Action::ExecuteSubstitute, State::Ground };
        if (b == '\\')
            return { Action::APCStringTerminatorEnd, State::Ground };
        return { Action::APCEscapeOther, State::Escape };

    case State::VT52Y:
        return { Action::VT52YRow, State::VT52YColumn };

    case State::VT52YColumn:
        // VT52Y is entered only from the VT52 grammar, so the address completes
        // back in VT52 ground — not ANSI Ground, which would misparse the next ESC.
        return { Action::VT52YColumn, State::VT52Ground };

    case State::SOS:
        if (b == 0x1B)
            return { Action::Noop, State::SOSEscape };
        return { Action::Noop, State::SOS };

    case State::SOSEscape:
        if (b == '\\')
            return { Action::Noop, State::Ground };
        if (b == 0x1B)
            return { Action::Noop, State::SOSEscape };
        return { Action::Noop, State::SOS };

    case State::SOSPMAPC:
        if (b == 0x18) // cancel abandons the string
            return { Action::Noop, State::Ground };
        if (b == 0x1A) // substitute abandons it and draws the error character
            return { Action::ExecuteSubstitute, State::Ground };
        if (b == 0x1B)
            return { Action::Noop, State::SOSPMAPCEscape };
        if (b == 0x07)
            return { Action::StringBell, State::SOSPMAPC };
        return { Action::Noop, State::SOSPMAPC };

    case State::SOSPMAPCEscape:
        if (b == 0x18)
            return { Action::Noop, State::Ground };
        if (b == 0x1A) // substitute abandons it and draws the error character
            return { Action::ExecuteSubstitute, State::Ground };
        if (b == '\\')
            return { Action::Noop, State::Ground };
        return { Action::StringEscapeOther, State::Escape };

    case State::VT52Ground:
        // VT52 ground reuses the DEC ground actions for everything
        // except 0x1B, which routes to the VT52 escape grammar.
        switch (b) {
        case 0x07:
            return { Action::ExecuteBell, State::VT52Ground };
        case 0x08:
            return { Action::ExecuteBackspace, State::VT52Ground };
        case 0x09:
            return { Action::ExecuteHorizontalTab, State::VT52Ground };
        case 0x0A:
        case 0x0B:
        case 0x0C:
            return { Action::ExecuteLineFeed, State::VT52Ground };
        case 0x0D:
            return { Action::ExecuteCarriageReturn, State::VT52Ground };
        case 0x0E:
            return { Action::ExecuteShiftOut, State::VT52Ground };
        case 0x0F:
            return { Action::ExecuteShiftIn, State::VT52Ground };
        case 0x1B:
            return { Action::EnterEscape, State::VT52Escape };
        case 0x7F:
            return { Action::Noop, State::VT52Ground };
        default:
            if (b >= 0x20)
                return { Action::Print, State::VT52Ground };
            return { Action::Noop, State::VT52Ground };
        }

    case State::VT52Escape:
        // The VT52Escape action handles all VT52 ESC dispatches and may override
        // the next state (ESC Y → VT52Y, ESC < → Ground); default is VT52Ground.
        return { Action::VT52Escape, State::VT52Ground };
    }
    // Unreachable: every State value handled above.
    return { Action::Noop, State::Ground };
}

// findOSC forwards to the OSC dispatcher's handler lookup (extern decl below the
// namespace close, qualified to avoid nesting OSC inside VT::Parser).
OSCHandler findOSC(int ps);

} // namespace Ubiquitty::VT::Parser

namespace Ubiquitty::OSC {
extern ::Ubiquitty::VT::Parser::OSCHandler findOSCHandler(int ps);
}

namespace Ubiquitty {
namespace VT::Parser {
OSCHandler findOSC(int ps) { return ::Ubiquitty::OSC::findOSCHandler(ps); }
} // namespace VT::Parser

void publishPixelGeometry(Ubiquitty::Terminal& t, const PixelGeometry& geometry)
{
    const auto saturated = [](int pixels) { return static_cast<std::uint16_t>(std::clamp(pixels, 0, 65535)); };
    t.cellPixelWidth = saturated(geometry.cell.width);
    t.cellPixelHeight = saturated(geometry.cell.height);
    t.textAreaPixelWidth = saturated(geometry.textArea.width);
    t.textAreaPixelHeight = saturated(geometry.textArea.height);
    t.drawablePixelWidth = saturated(geometry.drawable.width);
    t.drawablePixelHeight = saturated(geometry.drawable.height);
}

/// Resizes the terminal and reports the new size to the host.
///
/// Lives here rather than beside the reflow it delegates to because the report is
/// wire-side and the screen model has no output queue. ``Screen::resizeGrid``
/// says whether the geometry moved; this decides what the host hears.
void resize(Ubiquitty::Terminal& t, int newRows, int newColumns, Modes::ReflowMode reflow, Modes::AnchorMode anchor, Modes::ResizeUrgency urgency)
{
    VT::discardGraphicCancellation(t);
    VT::finishAlignedTabString(t);
    const Ubiquitty::Screen::ResizeOutcome outcome = Ubiquitty::Screen::resizeGrid(&t, newRows, newColumns, reflow, anchor, urgency);
    // A no-op resize stays silent. Reporting an unchanged size would make a
    // subscribed application redraw for nothing on every settled frame of a width
    // drag, and mode 2048 asks for a report on change, not on request.
    if (outcome.geometryChanged)
        VT::emitInBandResize(t);
}

} // namespace Ubiquitty
