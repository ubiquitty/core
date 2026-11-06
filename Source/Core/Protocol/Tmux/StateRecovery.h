// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The attach screen-content recovery state machine (pure half). tmux %output carries
// only NEW app output, never the screen drawn before we attached, so on pane
// discovery the frontend issues a capture-pane query bracketed by a UBCAPBEGIN
// marker. The capture is delimited by the protocol's %begin/%end command-response
// block boundaries, not by body text, so screen content (even a line that looks
// like a marker) can't corrupt or prematurely end it.
//
// This machine consumes the response body lines + block-end boundary, parses the
// marker, accumulates rows under a 1 MiB limit, and on the capture block's end decides
// whether to feed the screen, whether it was blank, and whether a bounded retry is
// due. Single-threaded; holds no locks and dispatches nothing itself.

#include "Core/Protocol/Tmux/BlockAccumulator.h"
#include "Core/Protocol/Tmux/Commands.h"
#include "Core/Protocol/Tmux/PaneID.h"
#include "Core/Protocol/Tmux/Tmux.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty::Protocol::Tmux {

/// One row a capture printed.
struct CapturedRow {
    /// The row's text, SGR-escaped as `capture-pane -e` prints it.
    std::string text;
    /// Whether tmux wrapped the row into the one below it.
    bool isWrapped = false;
};

/// A pane's screen as one capture recovered it.
struct RecoveredScreen {
    /// Which capture printed the rows, and so how they are written back.
    CaptureScope scope = CaptureScope::CurrentScreen;
    /// The rows top to bottom; a full-history capture's begin at the oldest
    /// history row and end at the screen's last.
    std::vector<CapturedRow> rows;
    /// tmux's `#{cursor_x}`, 0-indexed: the pane's width while a wrap is
    /// deferred at the right edge.
    int cursorX = 0;
    /// tmux's `#{cursor_y}`, 0-indexed from the screen's top row.
    int cursorY = 0;
};

/// Composes the bytes that write a recovered screen back, cut where each row
/// joined to a wrapped one begins.
///
/// A current-screen feed resets the pen, clears the screen and writes each
/// row at its own row, so blank rows and the row count hold without a scroll.
/// A full-history feed starts at home on a terminal the caller has cleared and
/// separates the rows with CR LF, so the rows past the screen scroll into
/// history as output would, and the last rows are the screen; a separator after
/// the last row would scroll one row further than tmux's screen. Both end by
/// putting the cursor where tmux reported it.
///
/// No sequence marks a row as wrapped, and a scroll clears the join of the row
/// it moves up from the bottom, so the feed is cut where the cursor has just
/// reached the row after a wrapped one, and the writer marks the row above the
/// cursor at each cut, as a wrap the terminal makes itself is marked once its
/// line feed is done. A wrapped last row has no row after it to join.
///
/// - Parameter screen: The recovered screen.
/// - Returns: One segment per row joined to a wrapped one above it, and a last
///   segment holding the cursor restore; each segment before the last ends
///   where the cursor has reached such a row.
[[nodiscard]] inline std::vector<std::string> recoveryFeed(const RecoveredScreen& screen)
{
    std::vector<std::string> segments(1);
    const bool isFullHistory = screen.scope == CaptureScope::FullHistory;
    segments.back() = isFullHistory ? "\x1b[H" : "\x1b[0m\x1b[2J";
    for (std::size_t index = 0; index < screen.rows.size(); ++index) {
        if (isFullHistory) {
            if (index > 0)
                segments.back() += "\r\n";
        } else {
            segments.back() += "\x1b[" + std::to_string(index + 1) + ";1H";
        }
        if (index > 0 && screen.rows[index - 1].isWrapped)
            segments.emplace_back();
        segments.back() += screen.rows[index].text;
    }
    segments.back() += "\x1b[" + std::to_string(screen.cursorY + 1) + ";" + std::to_string(screen.cursorX + 1) + "H";
    return segments;
}

// Bound the accumulated capture so a hostile/buggy server can't stream unbounded
// "content" into memory. Past the limit, rows are still swallowed (so the block
// end routes cleanly — dropping the machine's state mid-block would misparse the
// rest of the response as pane records) but no longer appended.
constexpr size_t maxCaptureBytes = 1u << 20; // 1 MiB

// How many times a pane's recovery re-issues the capture when it comes back blank.
// A pane whose app hasn't painted its first frame yet captures as just row newlines
// (the filling %output raced ahead of registration); a bounded re-capture lets a
// slow first paint land, while a real-blank pane stops retrying.
constexpr int maxCaptureRetries = 3;

// What captureBlockEnded() decided the frontend should do once a capture block
// closes. Pure data — the frontend acts on it (feed under the model lock,
// schedule the retry on a timer) without re-deriving any of the policy.
struct CaptureOutcome {
    bool didComplete = false; // a capture block actually closed (false: marker block / idle)
    PaneID paneID = invalidPaneID; // the pane this capture recovered
    bool blank = false; // the capture was effectively blank (no printable byte)
    bool shouldRetry = false; // schedule one more capture (blank && under the retry limit)
    int nextRetry = 0; // the retry count to stamp on that re-issued capture
    RecoveredScreen screen; // the rows, their wraps and the cursor, for the pane
};

// The recovery machine. Single-threaded; the frontend feeds it the
// command-response body lines (consumeLine) and the block-end boundary
// (blockEnded), both on its ingest thread.
class CaptureRecovery {
public:
    // Abandon any in-flight capture — e.g. on a fresh attach / %exit — so a
    // half-captured state from a prior session can't swallow the next session's
    // response rows. Safe to call repeatedly.
    void reset()
    {
        m_block.reset();
        m_paneID = invalidPaneID;
        m_cursorX = 0;
        m_cursorY = 0;
        m_retry = 0;
        m_scope = CaptureScope::CurrentScreen;
        m_form = CaptureForm::Text;
    }

    // Whether a capture is currently in flight (a marker seen but its content
    // block not yet closed). Idle outside that window.
    [[nodiscard]] bool active() const { return m_block.active(); }

    /// Routes one command-response body line.
    ///
    /// A line belongs to a capture when it is the UBCAPBEGIN marker, or a screen
    /// row while capturing. The capture is closed by the protocol block boundary
    /// (`blockEnded`), never by body text — a captured screen can contain
    /// anything, including text that looks like a marker.
    ///
    /// - Parameter line: The body line, without its terminator.
    /// - Returns: True when the line belonged to a capture, in which case the
    ///   caller must not parse it as a pane record.
    bool consumeLine(std::string_view line)
    {
        std::string_view payload;
        const BlockAccumulator::LineKind kind = m_block.consumeLine(line, payload);
        if (kind == BlockAccumulator::LineKind::Marker)
            parseMarker(payload);
        return kind != BlockAccumulator::LineKind::NotOurs;
    }

    // A command-response block ended (%end or %error). Advances the machine by
    // protocol boundary: the marker's block end arms Capturing (the NEXT block is
    // the capture-pane content); the capture block's end produces the
    // CaptureOutcome the frontend acts on (and resets to Idle). Outside a capture
    // (or on the marker's own block end) returns an outcome with didComplete false.
    [[nodiscard]] CaptureOutcome blockEnded()
    {
        CaptureOutcome outcome;
        std::string rows;
        if (!m_block.blockEnded(rows))
            return outcome;
        outcome.didComplete = true;
        outcome.paneID = m_paneID;
        outcome.screen.scope = m_scope;
        outcome.screen.cursorX = m_cursorX;
        outcome.screen.cursorY = m_cursorY;
        outcome.screen.rows = splitRows(rows, m_form);
        outcome.blank = std::ranges::all_of(outcome.screen.rows, [](const CapturedRow& row) { return isEffectivelyBlank(row.text); });
        // A real-blank pane must not capture forever, hence the limit.
        outcome.shouldRetry = outcome.blank && m_retry < maxCaptureRetries;
        outcome.nextRetry = m_retry + 1;
        reset();
        return outcome;
    }

    // "Effectively blank" = no printable byte. A capture trims trailing
    // whitespace, so an unpainted pane comes back as just row newlines/spaces.
    [[nodiscard]] static bool isEffectivelyBlank(std::string_view rows)
    {
        for (char c : rows) {
            if (c != '\n' && c != ' ' && c != '\t' && c != '\r')
                return false;
        }
        return true;
    }

    /// Splits a capture block's newline-terminated rows.
    ///
    /// A flagged row is its flags, one space and its text; it is wrapped when
    /// the flags hold `W`. A flagged row without the space is all flags and
    /// no text.
    ///
    /// - Parameters:
    ///   - rows: The block's rows, each ending in a newline.
    ///   - form: How the capture printed them.
    /// - Returns: The rows top to bottom.
    [[nodiscard]] static std::vector<CapturedRow> splitRows(std::string_view rows, CaptureForm form)
    {
        std::vector<CapturedRow> split;
        while (!rows.empty()) {
            const std::size_t end = rows.find('\n');
            std::string_view line = rows.substr(0, end);
            rows.remove_prefix(end == std::string_view::npos ? rows.size() : end + 1);
            CapturedRow row;
            if (form == CaptureForm::FlaggedRows) {
                const std::size_t space = line.find(' ');
                const std::string_view flags = line.substr(0, space);
                row.isWrapped = flags.find('W') != std::string_view::npos;
                line = space == std::string_view::npos ? std::string_view {} : line.substr(space + 1);
            }
            row.text = line;
            split.push_back(std::move(row));
        }
        return split;
    }

private:
    // "<pane> <cursorX> <cursorY> <retry> [<scope> [<form>]]". Fields are
    // positional and optional from the right; scanUInt leaves a missing one at
    // its 0 default.
    void parseMarker(std::string_view payload)
    {
        const char* line = payload.data();
        const int length = static_cast<int>(payload.size());
        int i = 0, cursorX = 0, cursorY = 0, retry = 0, scope = 0, form = 0;
        PaneID paneID = 0;
        scanUInt({ line, static_cast<std::size_t>(length) }, i, paneID);
        for (int* field : { &cursorX, &cursorY, &retry, &scope, &form }) {
            if (i >= length || line[i] != ' ')
                break;
            i++;
            scanUInt({ line, static_cast<std::size_t>(length) }, i, *field);
        }
        m_paneID = paneID;
        m_cursorX = cursorX;
        m_cursorY = cursorY;
        m_retry = retry;
        m_scope = (scope != 0) ? CaptureScope::FullHistory : CaptureScope::CurrentScreen;
        m_form = (form != 0) ? CaptureForm::FlaggedRows : CaptureForm::Text;
    }

    BlockAccumulator m_block { captureBeginPrefix, maxCaptureBytes, BlockAccumulator::LineJoin::TerminateEachLine };
    PaneID m_paneID = invalidPaneID;
    int m_cursorX = 0;
    int m_cursorY = 0;
    int m_retry = 0; // retries already done for this pane (rides the UBCAPBEGIN marker)
    CaptureScope m_scope = CaptureScope::CurrentScreen; // rides the marker's 5th field
    CaptureForm m_form = CaptureForm::Text; // rides the marker's 6th field
};

// What clipboardBlockEnded() decided. The frontend resolves the pane and queries
// its capability policy — both Terminal-typed — then calls shouldWrite.
struct ClipboardPullOutcome {
    bool didComplete = false; // a buffer block closed (false: marker block / idle)
    int paneID = -1; // from the SERVER-controlled marker; -1 when absent/malformed
    std::string text; // the pulled buffer
};

// The copy-mode clipboard pull (tmux paste buffer -> system clipboard). Same
// marker-then-content shape as CaptureRecovery over the same accumulator; the
// buffer is newline-SEPARATED, so it carries no trailing newline.
class ClipboardPullRecovery {
public:
    void reset()
    {
        m_block.reset();
        m_paneID = -1;
    }

    [[nodiscard]] bool active() const { return m_block.active(); }

    /// Routes one command-response body line.
    ///
    /// - Parameter line: The body line, without its terminator.
    /// - Returns: True when the line belonged to the pull, in which case the
    ///   caller must not parse it as a pane record.
    bool consumeLine(std::string_view line)
    {
        std::string_view payload;
        const BlockAccumulator::LineKind kind = m_block.consumeLine(line, payload);
        if (kind == BlockAccumulator::LineKind::Marker)
            m_paneID = parseClipboardPullMarkerPane(payload);
        return kind != BlockAccumulator::LineKind::NotOurs;
    }

    [[nodiscard]] ClipboardPullOutcome blockEnded()
    {
        ClipboardPullOutcome outcome;
        std::string text;
        if (!m_block.blockEnded(text))
            return outcome;
        outcome.didComplete = true;
        outcome.paneID = m_paneID;
        outcome.text = std::move(text);
        m_paneID = -1;
        return outcome;
    }

private:
    BlockAccumulator m_block { clipboardPullPrefix, maxClipboardPullBytes,
        BlockAccumulator::LineJoin::SeparateLines };
    int m_paneID = -1;
};

} // namespace Ubiquitty::Protocol::Tmux
