// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// ScrollSearch — regex search over a Terminal's scrollback + live screen. The matcher is vendored RE2
// (linear-time, no backtracking), so a hostile find-field pattern can't stall. Full RE2 syntax except
// back-references and lookaround.
//
// Unicode (RE2 in UTF-8): the haystack is every row's text as UTF-8, each cell's cluster normalized as a
// unit, with every byte mapped back to its origin cell; case-insensitive search folds Unicode case. Patterns
// capped at scrollSearchMaxPattern and the compiled program at a bounded RE2 budget.
//
// The scanner matches one logical line at a time. The renderer reads a bounded
// projection of logical hits, split into visual-row segments as the grid wraps.
// SearchController owns the query, count and navigation cursor.

#include "Base/Types.h"
#include "Core/CaseSensitivity.h"
#include "Core/Search/Search.h"
#include "Core/Unicode/Normalize.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Wraps the compiled RE2 object, forward-declared only so re2 headers never enter this include graph
// (keeps it portable — consumers need no re2 -I — and Swift-importable).
namespace Ubiquitty {
/// The terminal model a search reads. Declared, not included, to keep this header portable.
struct Terminal;
/// One grid cell, whose search text ``ScrollSearch::cellSearchText`` answers.
struct Cell;
/// The compiled RE2 program behind a pattern. Opaque so re2 headers never enter this include graph.
struct ScrollSearchProgram;
}

namespace Ubiquitty::Screen {

/// A run of history lines, `[from, to)`, by logical line number
/// (Core/Screen/HistoryLines.h).
struct HistoryLineRange {
    std::uint64_t from = 0;
    std::uint64_t to = 0;
};

constexpr int scrollSearchMaxMatches = 256;
// Pattern byte-length ceiling; compile() rejects anything longer.
constexpr int scrollSearchMaxPattern = 128;
// A match spanning K rows emits K segments; budget a few rows per match for a bounded segment array.
constexpr int scrollSearchMaxSegments = scrollSearchMaxMatches * 4;

namespace ScrollSearch {

/// Whether a search treats upper and lower case as the same character.
///
/// An alias: the trigger engine asks the same question and must reach the answer
/// without including a matcher, so the enumeration is declared in
/// Core/CaseSensitivity.h and both subsystems name one type.
using CaseSensitivity = Ubiquitty::CaseSensitivity;
// A compiled pattern: a shared immutable handle to compile()'s RE2 program. Cheaply copyable; copies share
// one program. A default-constructed (or failed-compile) value is empty and matches nothing.
struct CompiledRegex {
    std::shared_ptr<const ScrollSearchProgram> program;
};

// One on-screen highlight segment: a single-row run in the scrollback + live address space.
// absoluteRow: 0..(scrollbackCount-1) scrollback (oldest first); scrollbackCount.. live.
// matchID groups the segments of one logical match (a multi-row match is several segments sharing a matchID).
struct Match {
    int absoluteRow;
    int column;
    int length;
    int matchID; // logical-match index this segment belongs to
};

// Named alias so value-returning matches() imports into Swift as a collection.
using MatchList = std::vector<Match>;

/// A sliced search's hits as the frame reads them: logical positions, and the
/// numbering their row segments were resolved under.
///
/// Rows renumber under output and resizes while the positions keep naming
/// their text, so the segments are resolved again whenever the numbering a
/// frame reads them in has moved.
struct LogicalProjection {
    /// The hits the segments show, newest first: a window of the search's
    /// hits around the active one, at most `scrollSearchMaxMatches`.
    std::vector<Ubiquitty::Search::Hit> hits;
    /// The active hit's index in `hits`, or -1.
    int activeIndex = -1;
    std::uint32_t absoluteRowEpoch = 0;
    std::uint64_t modelEpoch = 0;
    int historyRows = -1;
    bool isAlternate = false;
};

/// The bounded render projection of a sliced search.
struct ResultProjection {
    // Per-row highlight segments, grouped by matchID in scan order.
    Match segments[scrollSearchMaxSegments] {};
    int segmentCount {};
    // logicalCount distinct matches; firstSegment[k] indexes the first
    // segment of logical match k (its start row/column).
    int firstSegment[scrollSearchMaxMatches] {};
    int logicalCount {};
    int activeIndex { -1 }; // logical-match index; -1 when no active.
    // Counts every change of the active match — a search, a step forward or
    // back — so a reader that draws the active match's arrival (the pill's
    // bounce) can tell a new focus from the same one seen again.
    uint32_t activeEpoch {};
    bool valid {}; // false until a successful compile + search.
    /// Set when a sliced search published the hits; the segments are then
    /// its hits resolved, and kept current by every reader.
    std::unique_ptr<LogicalProjection> logical;
};

// The [unicode] normalization form, which copy applies. Search applies it to both needle and haystack so
// precomposed and decomposed spellings match, and with None it compares in NFC, so canonically equivalent
// text always matches while copy keeps text as written.
void setNormalizationForm(Ubiquitty::Unicode::NormalizationForm form);
[[nodiscard]] Ubiquitty::Unicode::NormalizationForm normalizationForm();

/// Returns the form a search compares text in for a configured form.
///
/// Search always treats canonically equivalent text as equal, so with no form
/// configured it compares in NFC.
///
/// - Parameter configured: The `[unicode]` normalization form.
/// - Returns: The configured form, or NFC for None.
[[nodiscard]] Ubiquitty::Unicode::NormalizationForm matchingNormalizationForm(Ubiquitty::Unicode::NormalizationForm configured);

/// Scratch a caller keeps across cells, so turning a long run of them into
/// search text allocates once.
struct CellTextScratch {
    std::vector<std::uint32_t> codePoints;
    std::string utf8;
};

/// Normalizes an owned cluster or run and returns its search text.
///
/// - Parameters:
///   - text: The captured code points, without continuation cells.
///   - form: The comparison normalization form.
///   - scratch: Reused conversion storage.
/// - Returns: Text borrowing scratch until its next use.
[[nodiscard]] std::string_view codePointSearchText(std::span<const std::uint32_t> text, Ubiquitty::Unicode::NormalizationForm form, CellTextScratch& scratch);

/// Returns the bytes one cell contributes to the text a search reads.
///
/// An ASCII character reads as itself; any other cluster as its UTF-8,
/// normalized as a unit under `form`; a control or an empty cell as a space. A
/// wide or sized character's continuation, and a proportional run's, adds no
/// text, since its anchor carries it; a run's anchor carries the run's whole
/// text. The find field's search and the history scan both read cells
/// through this, so the two agree on every cell.
///
/// - Parameters:
///   - terminal: The terminal whose cluster pool, run-text store and text-size
///     table a cell may name, or null for a reader without them.
///   - cell: The cell, or null for a column past a row's stored cells, which
///     reads as a space.
///   - form: The form the text is compared in, as ``matchingNormalizationForm``
///     answers.
///   - scratch: Reused storage; the answer may point into it.
/// - Returns: The bytes, valid until the next call with `scratch`; empty for a
///   continuation. Nothing when `terminal` is null and the cell names one of
///   its stores, whose text only the terminal holds.
[[nodiscard]] std::optional<std::string_view> cellSearchText(const Ubiquitty::Terminal* terminal, const Cell* cell, Ubiquitty::Unicode::NormalizationForm form, CellTextScratch& scratch);

/// Called with each match in a line, as its byte range in the line.
///
/// - Returns: Whether to look for the next match.
using LineMatchVisitor = bool (*)(void* context, std::size_t start, std::size_t length);

/// Runs a compiled pattern over one logical line's search text.
///
/// The line holds no line separator, so a match never crosses a hard line end
/// and `^` and `$` anchor to the line's edges. Matches are leftmost-first and
/// do not overlap; a zero-length match is reported, and the scan moves a byte
/// past it.
///
/// - Parameters:
///   - compiled: The pattern; an empty one matches nothing.
///   - line: The line's search text.
///   - visit: Called for each match.
///   - context: Handed to `visit`.
void forEachLineMatch(const CompiledRegex& compiled, std::string_view line, LineMatchVisitor visit, void* context);

/// Compiles a search pattern.
///
/// - Parameters:
///   - pattern: The RE2 pattern text, NUL-terminated.
///   - caseSensitivity: Whether the match folds case.
///   - out: Receives the compiled regex on success.
///   - output: Receives a one-line RE2 parse error on failure, truncated to fit.
/// - Returns: `true` when the pattern compiled.
bool compile(const char* pattern, CaseSensitivity caseSensitivity, CompiledRegex* out, std::span<char> output);

/// Compiles a search pattern in a stated normalization form, rather than the
/// process-wide one ``compile`` reads.
///
/// - Parameters:
///   - pattern: The RE2 pattern text.
///   - caseSensitivity: Whether the match folds case.
///   - form: The `[unicode]` form; None compares in NFC.
///   - error: Receives the reason when the pattern does not compile.
/// - Returns: The compiled pattern, or nothing.
[[nodiscard]] std::optional<CompiledRegex> compilePattern(std::string_view pattern, CaseSensitivity caseSensitivity, Ubiquitty::Unicode::NormalizationForm form, std::string& error);

/// One match in a history line: the cells of the line's joined form it
/// covers, `[startCell, endCell)`, counted as a history body carries the line
/// (Docs/sync.md §6.6), so a client that holds the line can show the match.
struct HistoryMatch {
    std::uint64_t line = 0;
    std::uint32_t startCell = 0;
    std::uint32_t endCell = 0;

    friend constexpr bool operator==(const HistoryMatch&, const HistoryMatch&) = default;
};

/// What a history search found.
struct HistorySearchResult {
    /// The epoch the line numbers are counted in.
    std::uint32_t epoch = 0;
    /// The matches, by line and then by cell.
    std::vector<HistoryMatch> matches;
    /// More matches were there than the cap allowed.
    bool isTruncated = false;
};

/// Searches a terminal's history by logical line, for a peer that has not
/// fetched it (Docs/sync.md §6.6, `pane.search`).
///
/// A line is its rows joined as the history plane carries it, so a match
/// across a soft wrap is one match and its address survives a reflow. The
/// search reads only the current epoch and names lines by number; it touches
/// no search state of the terminal's and reads no process-wide setting, the
/// normalization form included.
///
/// - Parameters:
///   - terminal: The terminal.
///   - pattern: The RE2 pattern, at most `scrollSearchMaxPattern` bytes less
///     one.
///   - range: The lines to search; cut to what the epoch holds.
///   - caseSensitivity: Whether the match folds case.
///   - form: The normalization form both pattern and lines are taken to; None
///     takes them to NFC, as the find field's search does.
///   - maxMatches: The most matches returned.
/// - Returns: The matches, or `nullopt` when the pattern does not compile.
[[nodiscard]] std::optional<HistorySearchResult> searchHistory(const Ubiquitty::Terminal& terminal, std::string_view pattern, HistoryLineRange range, CaseSensitivity caseSensitivity,
    Ubiquitty::Unicode::NormalizationForm form, std::size_t maxMatches);

// Escape regex metacharacters so a literal query matches verbatim (mirrors RE2::QuoteMeta; UTF-8 rides
// through raw). A literal (non-/re) find escapes the user's text through this before compiling.
[[nodiscard]] std::string escapeLiteral(std::string_view literal);

/// The first segment of the active logical match, or null when none is active.
///
/// - Parameter t: The terminal to read.
/// - Returns: The match's starting row and column.
const Match* activeMatch(const Ubiquitty::Terminal& t);
/// The number of logical matches, counting a multi-row match once.
///
/// - Parameter t: The terminal to read.
/// - Returns: How many matches the last search found.
int matchCount(const Ubiquitty::Terminal& t);
/// The first segment of logical match `index`, or null when out of range.
///
/// - Parameters:
///   - t: The terminal to read.
///   - index: Zero-based match position, oldest first.
/// - Returns: The match's starting row and column, or null.
const Match* matchAt(const Ubiquitty::Terminal& t, int index);
// Logical matches as a value collection (each the first segment, oldest first) — the Swift-facing read.
[[nodiscard]] MatchList matches(const Ubiquitty::Terminal& t);

/// The number of visual-row highlight runs across every match.
///
/// A match spanning soft-wrapped rows contributes one segment per row, so this
/// exceeds `matchCount` whenever a match wraps.
///
/// - Parameter t: The terminal to read.
/// - Returns: How many runs a renderer must paint.
int segmentCount(const Ubiquitty::Terminal& t);
/// The visual-row highlight run at `index`, or null when out of range.
///
/// A match spanning soft-wrapped rows contributes one segment per row, all sharing a matchID.
///
/// - Parameters:
///   - t: The terminal to read.
///   - index: Zero-based segment position.
/// - Returns: The run to paint, or null when out of range.
const Match* segmentAt(const Ubiquitty::Terminal& t, int index);

/// The zero-based index of the active match, or -1 when none is active.
///
/// A "k of n" label wants `activeMatchIndex() + 1` for k.
///
/// - Parameter t: The terminal to read.
/// - Returns: The active match's position, or -1.
int activeMatchIndex(const Ubiquitty::Terminal& t);

/// The count of times the active match has changed on this terminal — a
/// search, a step forward or back — and zero with no search state. A frame
/// that sees the number move knows a match just took focus.
///
/// Defined in Core/VT/Init.cpp beside the block's lifetime rather than here:
/// the frame capture that reads it is kept by every Core subset, and the host
/// and agent subsets trim this file's translation unit.
///
/// - Parameter t: The terminal.
/// - Returns: The epoch, zero without search state.
[[nodiscard]] uint32_t activeMatchEpoch(const Ubiquitty::Terminal& t);

/// Drops the terminal's published search projection.
void clear(Ubiquitty::Terminal* t);

/// Publishes a sliced search's hits as the terminal's search state, which the
/// frame and the step readers above read.
///
/// The state holds at most `scrollSearchMaxMatches` matches, so it shows the
/// window of hits around the active one; the search's own count is the one a
/// counter shows. The hits' row segments are resolved now and again by any
/// reader that finds the rows renumbered since. The caller holds the model
/// lock.
///
/// - Parameters:
///   - t: The terminal searched.
///   - hits: The search's hits, newest first.
///   - activeIndex: The active hit's index in `hits`, or -1.
///   - activeEpoch: The search controller's focus revision; unchanged by count updates.
void publishLogicalHits(Ubiquitty::Terminal& t, std::span<const Ubiquitty::Search::Hit> hits, int activeIndex, std::uint32_t activeEpoch);

} // namespace ScrollSearch
} // namespace Ubiquitty::Screen
