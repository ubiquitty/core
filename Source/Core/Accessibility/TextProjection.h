// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The one visible-grid → flat text projection. Named for its first consumer; every
// screen-read in the product must produce the same characters.
//
// Consumers: macOS VoiceOver (the view's accessibility text, which needs the
// UTF-16 ranges), Linux AT-SPI, agent/MCP (AgentSession::screenText), and the
// control socket's get-text.
//
// The rule — visible-cell walk, wide-cell and combining-mark handling,
// trailing-space trim, row join, UTF-16 range accounting — is defined here and
// nowhere else.

#include "Base/SwiftInterop.h"
#include "Core/RichText/Layout.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Accessibility {

/// One visible row's span in the flat text.
///
/// Measured in UTF-16 code UNITS — what `NSString` and the AX text APIs count —
/// not bytes and not codepoints. An astral codepoint counts as 2, being a
/// surrogate pair. A consumer that indexes the UTF-8 `text` with these will land
/// mid-character on the first non-ASCII row.
struct LineRange {
    uint32_t location = 0; // UTF-16 offset of this row's first char in `text`
    uint32_t length = 0; // UTF-16 length of the row (trailing spaces trimmed)
};

/// A cell boundary in the projection's UTF-16 and UTF-8 coordinate spaces.
struct CellTextBoundary {
    uint32_t location = 0;
    std::size_t byteOffset = 0;
};

/// One visible logical range and its row in the screen projection.
struct RichTextRange {
    std::uint32_t first = 0;
    std::uint32_t end = 0;
    int row = 0;
    int column = 0;
    std::uint32_t location = 0;
};

/// A rich-text placement and the geometry used for this screen snapshot.
struct RichTextProjection {
    RichText::Block block;
    std::uint32_t typographyEpoch = 0;
    /// The local row containing the source anchor, resolved by this layout.
    std::uint32_t viewportTop = 0;
    bool isShaped = false;
    bool isLayoutPending = false;
    std::vector<RichText::LineLayout> lines;
    std::vector<RichText::ShapedSpan> spans;
    std::vector<RichTextRange> visibleRanges;
};

/// The visible grid as flat text, with one range per row.
struct TextProjection {
    std::string text; // flat visible grid, rows joined by '\n', UTF-8
    std::vector<LineRange> lineRanges; // one per visible row, in text order
    /// Row-major boundaries, including the boundary after each row's last cell.
    std::vector<CellTextBoundary> cellBoundaries;
    int columns = 0;
    std::vector<RichTextProjection> richText;
};

/// Projects the terminal's visible grid to flat text and per-row ranges.
///
/// Wide-continuation cells are skipped, the lead cell having emitted the wide
/// glyph; the wide-continuation sentinel codepoint emits a single space;
/// Full clusters and proportional runs appear once; cell boundaries use the
/// proportional layout's text ownership, or anchor ownership without a measurer.
/// Combining marks follow their base codepoint; each row is
/// trailing-space-trimmed; rows join with `'\n'`. Ranges count UTF-16 units so
/// they align with an `NSString` built from `text`.
///
/// - Parameters:
///   - term: The terminal to read. A non-positive grid yields empty output.
///   - viewOffset: Rows scrolled back from the live tail; 0 is the tail.
/// - Returns: The flat text and one range per visible row, in text order.
[[nodiscard]] TextProjection projectVisibleText(const Ubiquitty::Terminal& term, int viewOffset);

/// Returns the projection's text alone, for callers that need no ranges.
///
/// Byte-identical to `projectVisibleText(...).text`: it is that call with the
/// range accounting discarded, rather than the grid walked a second way — which
/// is what keeps every screen-read in the product producing the same characters.
///
/// - Parameters:
///   - term: The terminal to read.
///   - viewOffset: Rows scrolled back from the live tail; 0 is the tail.
/// - Returns: The visible grid as UTF-8, rows joined by `'\n'`.
[[nodiscard]] std::string visibleScreenText(const Ubiquitty::Terminal& term, int viewOffset);

/// A half-open span in the projection, or absent.
///
/// UTF-16 units like ``LineRange``, for the same consumer. `isValid` false
/// means the subject — a caret, a selection — lies wholly off-screen, which a
/// consumer must answer as absent rather than as an empty range at zero: an
/// assistive reader treats the latter as a caret at the document's start.
struct ProjectionRange {
    std::uint32_t location = 0;
    std::uint32_t length = 0;
    bool isValid = false;
};

/// Resolves a cell boundary and a packed code-point offset into projected text.
///
/// - Parameters:
///   - projection: The projection containing the visible row.
///   - row: The visible row; an out-of-range row answers absent.
///   - column: The cell boundary, clamped to the visible grid.
///   - subColumn: Code points into the cell's text, clamped to its end.
/// - Returns: A zero-length UTF-16 range, or absent when no mapping exists.
[[nodiscard]] ProjectionRange positionInProjection(const TextProjection& projection, std::ptrdiff_t row, int column, int subColumn = 0);

/// The visible caret as an index into the projection.
///
/// The active screen's cursor, on the live view only — scrolled back, the
/// rows on display are not the cursor's. The cursor can sit past the trimmed
/// line end (typing at the right margin), so the index clamps to the line's
/// reported length.
///
/// - Parameters:
///   - projection: The projection to index into, from the same terminal at
///     the same offset.
///   - term: The terminal whose cursor is asked for.
///   - viewOffset: Rows scrolled back; nonzero answers absent.
/// - Returns: The caret as a zero-length range, or absent.
[[nodiscard]] ProjectionRange caretInProjection(const TextProjection& projection, const Ubiquitty::Terminal& term, std::ptrdiff_t viewOffset);

/// The value-changed notify coalescer: at most one post per interval, with a
/// trailing post so the state after a burst still reaches the reader.
///
/// The policy lives here — the ceiling and the three-way decision — and a
/// frontend supplies only its clock and its timer. A flood of screen updates
/// must not spam an assistive reader, and a burst's final state must not be
/// swallowed by the ceiling that tamed the burst.
struct NotifyThrottle {
    /// Seconds between posts — a 5 Hz ceiling.
    static constexpr double minIntervalSeconds = 0.2;

    /// What the caller does with one change notification.
    ///
    /// The Swift names carry the language's own case convention, so a Swift
    /// caller writes `.postNow` rather than the C++ spelling.
    enum class Action : std::uint8_t {
        /// Post now; the interval has passed.
        PostNow UBIQUITTY_SWIFT_NAME(postNow),
        /// Schedule a trailing post after ``delaySeconds``.
        ScheduleTrailing UBIQUITTY_SWIFT_NAME(scheduleTrailing),
        /// A trailing post is already scheduled; do nothing.
        AlreadyScheduled UBIQUITTY_SWIFT_NAME(alreadyScheduled),
    };

    /// The verdict and, for a trailing schedule, its delay.
    struct Decision {
        Action action = Action::PostNow;
        double delaySeconds = 0.0;
    };

    /// Records one content change and decides how it reaches the reader.
    ///
    /// - Parameter nowSeconds: The caller's monotonic clock.
    /// - Returns: The decision; a `PostNow` has already stamped the clock.
    [[nodiscard]] constexpr Decision noteChange(double nowSeconds)
    {
        if (nowSeconds - m_lastPostSeconds >= minIntervalSeconds) {
            m_lastPostSeconds = nowSeconds;
            return { Action::PostNow, 0.0 };
        }
        if (m_isTrailingScheduled)
            return { Action::AlreadyScheduled, 0.0 };
        m_isTrailingScheduled = true;
        return { Action::ScheduleTrailing, minIntervalSeconds - (nowSeconds - m_lastPostSeconds) };
    }

    /// Records the trailing post firing; the caller posts after this.
    ///
    /// - Parameter nowSeconds: The caller's monotonic clock.
    constexpr void noteTrailingFired(double nowSeconds)
    {
        m_isTrailingScheduled = false;
        m_lastPostSeconds = nowSeconds;
    }

private:
    double m_lastPostSeconds = 0.0;
    bool m_isTrailingScheduled = false;
};

} // namespace Ubiquitty::Accessibility
