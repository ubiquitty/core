// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/HistoryPlane.h — the history body that `HistoryInline` and the
// transfer a `HistoryHead` announces carry (Docs/sync.md §6.6, §7.5):
//
//     HistoryBody := pens:[Pen] links:[(id:u16, key:bytes, uri:bytes)]
//                    clusters:[(id:u16, codePoints:[varint])] lines:[LogicalLine]
//                    runTexts?
//     LogicalLine := flags:u8 {Truncated, Open} RowRecord lineHash:u64
//     runTexts    := [(line:varint, column:varint, id:u16, codePoints:[varint])]
//
// `runTexts` gives each
// proportional run in the body's lines its text: the line's index in the
// body, the anchor's column in it, and the host's run-text id. A run's text
// lives in the host's run-text store, which a checkpoint mirrors whole; the
// client gives the anchor the host's id, defining the text there when its
// mirror holds none under it, since a run written since the last checkpoint
// may reach history without ever being on a screen the client saw. An id the
// mirror holds for another text names none. The section is written only
// when some line holds a run. `lineHash` does not cover it, so a line hashes
// alike with or without it.
//
// What a line shows is its `lineHash` and its runs' text together: the
// content hash, which is `lineHash` itself for a line with no run and a hash of
// `lineHash` and each run's column and text for one with runs. The host keeps a
// line's content hash to tell a rewritten line, so a line whose only change is
// a run's words is sent again; the client keeps its open line by it.
//
// A logical line is the rows of one soft-wrapped line joined, with no width:
// each row's trailing unwritten cells are dropped before the next is joined,
// which drops the pad a wide character leaves at a right margin it did not
// fit, so the line is the same at every width. Its `RowRecord` (RowForm.h)
// carries the first row's line attribute and mark and is never wrapped.
//
// A body is self-contained. Its `RowForm`s name pens, links and long clusters
// by their index in the body's own tables (a link or cluster index is one
// past it, zero naming none), never through the pane's dictionaries: a
// `HistoryInline` precedes the update whose definitions it would need
// (§7.8), and a dictionary epoch that rolls would strand every id a cached
// line named. A link or cluster entry also names the host's own id for it, the
// id the client's mirrored link ring and cluster pool hold it under, so the
// client resolves a history cell as it resolves a screen cell and never mints
// an id the screen plane may later define otherwise.
// Link keys and URIs are opaque, NUL-free byte strings, at most 31 and 2,047
// bytes as the host's ring holds them; clipping may split a UTF-8 sequence.
//
// `lineHash` is XXH3-64 over the line's canonical form: its line attribute
// and its `RowForm` written with tables of its own, numbered in the order the
// line first names each entry, followed by those tables. It depends on what
// the line shows, not on the width it was wrapped at, the body it travelled
// in, or the ids either end interned its entries under. The mark is not in
// it: a mark set on a line after it reached history is not a different line.
//
// A line longer than `maxHistoryLineCells` travels cut to the cap with
// `Truncated` set.
//
// The open line is the line that straddles the seam: its first rows have left
// the screen and its last has not, so it is not history yet and has no number
// of its own. It travels alone in a body, flagged `Open`, under the number
// `head`, which it takes when it completes; the completed line, or a longer
// open line, replaces it in place. A pane has at most one. An open line with
// no cells withdraws the one the receiver holds: the rows went back onto the
// screen.
//

#include "Core/Cell.h"
#include "Core/GridBounds.h"
#include "Core/Pen.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/RowForm.h"
#include "Core/Screen/HistoryLines.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Protocol::Sync {

/// The `flags` bit a line cut to `maxHistoryLineCells` carries.
inline constexpr std::uint8_t historyLineTruncated = 1;

/// The `flags` bit of the open line: the rows of the line that straddles the
/// seam that have left the screen, under the number it takes when it
/// completes.
inline constexpr std::uint8_t historyLineOpen = 2;

/// The most cells one logical line carries.
inline constexpr std::size_t maxHistoryLineCells = 16'384;

/// What a decoder accepts from one body.
struct HistoryBodyLimits {
    /// The most lines.
    std::size_t maxLines = 0;
    /// The most cells in one line.
    std::size_t maxLineCells = 0;
    /// The most bytes.
    std::size_t maxBodyBytes = 0;
    /// The storage charged to this body, including conservative working storage.
    /// Excludes prior output and allocator bookkeeping.
    std::size_t maxDecodedBytes = std::size_t { 64 } << 20;
};

/// The limits a client decodes every body under: a `HistoryHead` names at
/// most a `u16` of lines, and a body is at most one history transfer.
inline constexpr HistoryBodyLimits historyBodyLimits {
    .maxLines = 0xFFFF,
    .maxLineCells = maxHistoryLineCells,
    .maxBodyBytes = maxHistoryTransferBytes,
};

/// One decoded logical line, self-contained: its cells' link and cluster
/// fields name its own `links` and `clusters`, one past the index.
struct DecodedLine {
    std::uint64_t number = 0;
    std::uint8_t flags = 0;
    std::uint8_t lineAttribute = 0;
    std::uint8_t mark = 0;
    std::vector<Cell> cells;
    /// The links the cells name; each `id` is the host's.
    std::vector<LinkDefinition> links;
    /// The long clusters the cells name, and the host's pool id of each.
    std::vector<std::vector<std::uint32_t>> clusters;
    std::vector<std::uint16_t> clusterHostIDs;
    /// The text of each proportional run the line holds, by its anchor's
    /// column; empty from a body that carried none.
    struct RunText {
        std::uint32_t column = 0;
        /// The host's run-text id.
        std::uint16_t hostID = 0;
        std::vector<std::uint32_t> codePoints;
    };
    std::vector<RunText> runTexts;
    std::uint64_t lineHash = 0;

    /// What the line shows, its runs' text included (see the file comment).
    [[nodiscard]] std::uint64_t contentHash() const;
};

/// A host line's rows joined, as the wire carries it.
struct JoinedLine {
    std::vector<Cell> cells;
    std::uint8_t flags = 0;
    std::uint8_t lineAttribute = 0;
    std::uint8_t mark = 0;
};

/// Joins the rows of a host history line.
///
/// - Parameters:
///   - terminal: The host terminal.
///   - number: The line's number in the current epoch.
/// - Returns: The line, or `nullopt` when it is not history.
[[nodiscard]] std::optional<JoinedLine> joinedLine(const Ubiquitty::Terminal& terminal, std::uint64_t number);

/// Hashes a host history line as its receiver will.
///
/// - Parameters:
///   - terminal: The host terminal.
///   - number: The line's number in the current epoch.
/// - Returns: Its `lineHash`, or `nullopt` when it is not history or cannot
///   be said exactly.
[[nodiscard]] std::optional<std::uint64_t> logicalLineHash(const Ubiquitty::Terminal& terminal, std::uint64_t number);

/// Hashes what a host history line shows, its runs' text included, as a
/// receiver that read the runs' text will.
///
/// - Parameters:
///   - terminal: The host terminal.
///   - number: The line's number in the current epoch.
/// - Returns: Its content hash, or `nullopt` when it is not history or cannot
///   be said exactly.
[[nodiscard]] std::optional<std::uint64_t> logicalLineContentHash(const Ubiquitty::Terminal& terminal, std::uint64_t number);

/// The open line of a host terminal as a receiver would hash it.
struct OpenLine {
    /// The number it takes when it completes: the history's head.
    std::uint64_t number = 0;
    std::uint64_t lineHash = 0;
    /// What it shows, its runs' text included.
    std::uint64_t contentHash = 0;

    friend constexpr bool operator==(const OpenLine&, const OpenLine&) = default;
};

/// Joins the rows of a host terminal's open line.
///
/// - Parameter terminal: The host terminal.
/// - Returns: The line, flagged `Open`, or `nullopt` when no line straddles
///   the seam or the one that does shows nothing.
[[nodiscard]] std::optional<JoinedLine> joinedOpenLine(const Ubiquitty::Terminal& terminal);

/// Hashes a host terminal's open line as its receiver will.
///
/// - Parameter terminal: The host terminal.
/// - Returns: The line's number and hash, or `nullopt` when it has none.
[[nodiscard]] std::optional<OpenLine> openLineHash(const Ubiquitty::Terminal& terminal);

/// Encodes history lines for one pane, once for every attachment.
///
/// `capture` runs once per publish and keeps two things: the hashes of the
/// newest lines, which are the ones a taller screen can pull back and a later
/// shrink push out again, and the body of the lines completed since the last
/// capture, which every attachment that kept up sends as it is.
class HistoryPublisher {
public:
    /// The most hashes kept: a pull-back withdraws at most a screen of lines.
    static constexpr std::size_t maxPublishedHashes = static_cast<std::size_t>(Ubiquitty::maxRows);

    /// The most lines the body of one capture's appends holds; more travel as
    /// a transfer each attachment encodes.
    static constexpr std::size_t maxCapturedLines = 256;

    /// One line's captured identity and the revision that last changed it.
    struct PublishedHash {
        std::uint64_t number = 0;
        std::uint64_t hash = 0;
        std::uint64_t revision = 0;
    };

    /// The history publication state that cannot be inferred from retained rows.
    struct Continuation {
        Ubiquitty::Screen::HistoryAddress address;
        LineRange capturedAppends;
        std::vector<PublishedHash> hashes;
        std::uint64_t revision = 0;
        std::vector<std::uint16_t> capturedLinkIDs;
    };

    /// Copies retained rewrite identities, including lines pulled back to the screen.
    /// - Returns: Owned continuation, or no value before the first capture.
    [[nodiscard]] std::optional<Continuation> continuation() const;

    /// Restores publication identities against the rebuilt terminal's history.
    ///
    /// Cached encodings and the open-line key are recomputed. Invalid metadata
    /// or mismatching retained content leave this publisher unchanged.
    /// - Parameters:
    ///   - terminal: The exact restored terminal, held unchanged during the call.
    ///   - continuation: Admitted owned publication state.
    /// - Returns: Whether the history can continue under the same revision.
    [[nodiscard]] bool restore(const Ubiquitty::Terminal& terminal, Continuation continuation);

    /// Takes the terminal's history as it stands.
    ///
    /// - Parameter terminal: The pane's terminal.
    /// - Returns: False when a line completed since the last capture could not
    ///   be said exactly.
    [[nodiscard]] bool capture(const Ubiquitty::Terminal& terminal);

    /// Writes a body of the history lines in a range.
    ///
    /// The range is cut to the lines that are history, and the body to the
    /// lines that fit `maxBytes`; a line is never split.
    /// A line whose dictionaries or run texts exceed their wire limits stops
    /// the body before that line; an unrepresentable first line returns zero.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal, as last captured.
    ///   - range: The lines wanted.
    ///   - order: `Ascending` lists from the range's first line up,
    ///     `Descending` from its last down.
    ///   - maxBytes: The most bytes the body may take.
    ///   - output: The body is appended here.
    ///   - maxDecodedBytes: The receiver's decoded storage allowance.
    /// - Returns: The lines written; 0 leaves `output` unchanged.
    [[nodiscard]] std::size_t writeBody(const Ubiquitty::Terminal& terminal, LineRange range, HistoryOrder order, std::size_t maxBytes, std::vector<std::uint8_t>& output, std::size_t maxDecodedBytes = historyBodyLimits.maxDecodedBytes);

    /// Writes a body of one line: the open line, or the withdrawal of the one
    /// a receiver holds when there is none.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal, as last captured.
    ///   - maxBytes: The most bytes the body may take.
    ///   - output: The body is appended here.
    /// - Returns: False, leaving `output` unchanged, when the body does not
    ///   fit.
    [[nodiscard]] bool writeOpenLine(const Ubiquitty::Terminal& terminal, std::size_t maxBytes, std::vector<std::uint8_t>& output) const;

    /// The open line as the last capture found it.
    [[nodiscard]] std::optional<OpenLine> openLine() const { return m_openLine; }

    /// The content hash a line had when it was last captured, kept for the
    /// newest `maxPublishedHashes` lines, including ones a pull-back withdrew.
    [[nodiscard]] std::optional<std::uint64_t> publishedHash(std::uint64_t number) const;

    /// Where the history stood at the last capture.
    [[nodiscard]] Ubiquitty::Screen::HistoryAddress address() const { return m_address; }

    /// The lines the last capture found completed since the one before.
    [[nodiscard]] LineRange capturedAppends() const { return m_capturedAppends; }

    /// The links the lines the last capture hashed name, which the screen
    /// plane defines so the client's mirrored ring holds them.
    [[nodiscard]] std::span<const std::uint16_t> capturedLinkIDs() const { return m_capturedLinkIDs; }

    /// Counts the captures that found a line rewritten: a number a pull-back
    /// withdrew that came back with another hash.
    [[nodiscard]] std::uint64_t revision() const { return m_revision; }

    /// The lines rewritten after a revision, below a number.
    ///
    /// - Parameters:
    ///   - revision: The revision the caller last saw.
    ///   - below: The first number not wanted.
    /// - Returns: Their numbers, ascending.
    [[nodiscard]] std::vector<std::uint64_t> rewrittenSince(std::uint64_t revision, std::uint64_t below) const;

private:
    /// Takes the open line as it stands, rehashing it only when its rows moved.
    ///
    /// - Parameters:
    ///   - terminal: The pane's terminal.
    ///   - address: Where its history stands.
    void captureOpenLine(const Ubiquitty::Terminal& terminal, const Ubiquitty::Screen::HistoryAddress& address);

    Ubiquitty::Screen::HistoryAddress m_address;
    bool m_hasCaptured = false;
    LineRange m_capturedAppends;
    std::vector<std::uint8_t> m_capturedBody;
    std::size_t m_capturedBodyLines = 0;
    /// Ascending by number.
    std::deque<PublishedHash> m_hashes;
    std::uint64_t m_revision = 0;
    std::vector<std::uint16_t> m_capturedLinkIDs;
    /// What the open line's hash was taken from: rows are appended or drop
    /// back, and a reflow rewraps them, only by changing one of these.
    struct OpenLineKey {
        std::uint32_t epoch = 0;
        std::uint64_t head = 0;
        int rowCount = 0;
        long long rowsAppended = 0;
        int columns = 0;

        friend constexpr bool operator==(const OpenLineKey&, const OpenLineKey&) = default;
    };
    std::optional<OpenLineKey> m_openLineKey;
    std::optional<OpenLine> m_openLine;
};

/// Decodes a history body.
///
/// Refuses the whole body, leaving `lines` unchanged, when a count is over
/// its limit (checked before anything is allocated), a line is over the cell
/// limit or decoded storage allowance, a `RowForm` names a pen, link or cluster
/// past the body's tables, a `lineHash` does not match its line, a run text
/// names no sized anchor of its line or comes out of order, or the body does
/// not end where its last line or its run texts do.
///
/// - Parameters:
///   - body: The body.
///   - firstLine: The number of its first line.
///   - order: Whether the numbers ascend or descend from it.
///   - limits: What the decoder accepts.
///   - lines: Receives the lines, appended.
/// - Returns: Whether the body was accepted.
[[nodiscard]] bool decodeHistoryBody(std::span<const std::uint8_t> body, std::uint64_t firstLine, HistoryOrder order, const HistoryBodyLimits& limits, std::vector<DecodedLine>& lines);

/// Encodes one retained, self-contained line with the history plane's canonical
/// tables and proportional run text. This is the same body a wire decoder reads.
///
/// - Parameters:
///   - line: The retained line, with local link and cluster indices.
///   - maxBytes: The maximum encoded body, no greater than the history limit.
///   - output: Receives the body only on success.
/// - Returns: Whether the complete line fit and its recorded hash matched.
[[nodiscard]] bool encodeHistoryLine(const DecodedLine& line, std::size_t maxBytes, std::vector<std::uint8_t>& output);

} // namespace Ubiquitty::Protocol::Sync
