// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/CellContent.h"
#include "Core/GridBounds.h"
#include "Core/Pen.h"
#include "Core/TextSizeTable.h"
#include "Core/Unicode/Width.h"
#include "PAL/TextShaping.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Ubiquitty {
class TextMeasureClient;
struct Terminal;
}

namespace Ubiquitty::OSC::TextSizing {

/// The largest text one OSC 66 code may carry, in bytes; the protocol makes
/// senders split beyond it.
inline constexpr std::size_t maxTextBytes = 4096;

/// The widest proportional run, in cells: a row's worth. A renderer that
/// draws a run glyph by glyph draws all of it; one that draws a strip draws
/// the first ``Rendering::stripSlotCells`` of it.
inline constexpr int maxProportionalRunCells = Ubiquitty::maxColumns;

/// What an OSC 66 code's metadata asked for beyond the specification it
/// stores: the proportional keys that shape the request rather than the run.
struct Request {
    /// The stored specification.
    Ubiquitty::TextSizeSpec spec;
    /// `q=1`: measure and reply, print nothing.
    bool isQuery = false;
    /// `i`: the request id a reply echoes.
    uint8_t requestID = 0;
};

/// One OSC 66 code's parsed metadata: the block the text renders in and how
/// the glyphs sit inside it.
///
/// Defaults are the protocol's — an absent key means its default, and empty
/// metadata is a valid specification of all defaults. The struct is six
/// scalars and nothing a renderer could follow; where a scale is stored per
/// cell is the side-table seam's question, not this type's.
/// Parses OSC 66 metadata — colon-separated `key=value` pairs — into a spec.
///
/// Strict where the protocol is silent: an unknown key, a pair without `=`,
/// an empty or non-decimal value, an out-of-range value, a repeated key, or a
/// set denominator that fails to exceed the numerator refuses the whole
/// specification. The protocol leaves invalid metadata implementation-defined,
/// and strict-refuse is chosen so no sender comes to depend on a lenient
/// reading that a later revision would have to keep. A refused specification
/// makes the caller drop the code whole — byte-identical to a terminal that
/// never learned it. A set numerator with an unset denominator passes the
/// parse: the protocol constrains `d` only when non-zero, and what a zero
/// denominator means is the consumer's question.
///
/// The view's end terminates the metadata, so a NUL inside it is payload and
/// refuses like any other non-decimal byte.
///
/// - Parameters:
///   - metadata: The bytes between the code's first two semicolons.
/// - Returns: The parsed specification, or nothing on refusal.
[[nodiscard]] std::optional<Ubiquitty::TextSizeSpec> parseMetadata(std::string_view metadata);

/// ``parseMetadata`` with the proportional keys (Docs/proportional-text.md).
///
/// `p=1` admits `c`, `q`, `i` and `h` and refuses every sizing key: a
/// proportional run is one row of ordinary height. Without `p` the
/// proportional keys refuse. A print — no `q` — requires `c`.
///
/// - Parameters:
///   - metadata: The bytes between the code's first two semicolons.
/// - Returns: The parsed request, or nothing on refusal.
[[nodiscard]] std::optional<Request> parseRequest(std::string_view metadata);

/// The face and weight a pen sets a run in, for the measurer.
///
/// SGR 1 and SGR 3 as the two face bits, Fraktur borrowing the italic slant
/// as the renderer has it, and the SGR 1:n weight step — the derivation the
/// renderer makes per cell, made once here for a run, so the two cannot
/// disagree about which face a run is in.
///
/// - Parameter pen: The run's pen, as its anchor carries it.
/// - Returns: The style the measurer shapes with.
[[nodiscard]] Ubiquitty::ProportionalRunStyle runStyleOf(const Ubiquitty::Pen& pen);

/// The cells a proportional run needs: what a `q=1` query answers.
///
/// The measurer installed as ``Terminal/textMeasureClient``, by the count it
/// answers whole or else by the face it shapes in; the grid's face — one cell
/// per column the text would take — when it answers neither; at least one
/// cell and never wider than a run can be.
///
/// - Parameters:
///   - t: The terminal, whose client measures.
///   - codePoints: The run's text.
///   - style: The face and weight the run is set in; see ``runStyleOf``.
/// - Returns: The cell count, 1…``maxProportionalRunCells``.
[[nodiscard]] int proportionalRunCells(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle style);

/// The ranges a client's run faces carry advances for: Latin, combining marks,
/// Greek and Cyrillic. Keeping the intervening blocks out leaves room for
/// 256 carried attachment offers inside the host's bounded handover section.
inline constexpr std::uint32_t firstFacedCodePoint = 0x20;
inline constexpr std::uint32_t lastLatinFacedCodePoint = 0x24F;
inline constexpr std::uint32_t firstCombiningFacedCodePoint = 0x300;
inline constexpr std::uint32_t lastCombiningFacedCodePoint = 0x36F;
inline constexpr std::uint32_t firstGreekFacedCodePoint = 0x370;
inline constexpr std::uint32_t lastGreekFacedCodePoint = 0x3FF;
inline constexpr std::uint32_t firstCyrillicFacedCodePoint = 0x400;
inline constexpr std::uint32_t lastFacedCodePoint = 0x4FF;
inline constexpr std::size_t latinFacedCodePointCount = lastLatinFacedCodePoint - firstFacedCodePoint + 1;
inline constexpr std::size_t combiningFacedCodePointCount = lastCombiningFacedCodePoint - firstCombiningFacedCodePoint + 1;
inline constexpr std::size_t greekFacedCodePointCount = lastGreekFacedCodePoint - firstGreekFacedCodePoint + 1;
/// How many advances each weight class and face carries.
inline constexpr std::size_t facedCodePointCount = latinFacedCodePointCount + combiningFacedCodePointCount + greekFacedCodePointCount
    + lastFacedCodePoint - firstCyrillicFacedCodePoint + 1;

/// The index of a tabled Latin, combining, Greek or Cyrillic code point, or none.
/// - Parameter codePoint: The code point to find.
/// - Returns: Its compact table index, or none outside the four ranges.
[[nodiscard]] constexpr std::optional<std::size_t> facedCodePointIndex(std::uint32_t codePoint)
{
    if (codePoint >= firstFacedCodePoint && codePoint <= lastLatinFacedCodePoint)
        return codePoint - firstFacedCodePoint;
    if (codePoint >= firstCombiningFacedCodePoint && codePoint <= lastCombiningFacedCodePoint)
        return latinFacedCodePointCount + codePoint - firstCombiningFacedCodePoint;
    if (codePoint >= firstGreekFacedCodePoint && codePoint <= lastGreekFacedCodePoint)
        return latinFacedCodePointCount + combiningFacedCodePointCount + codePoint - firstGreekFacedCodePoint;
    if (codePoint >= firstCyrillicFacedCodePoint && codePoint <= lastFacedCodePoint)
        return latinFacedCodePointCount + combiningFacedCodePointCount + greekFacedCodePointCount + codePoint - firstCyrillicFacedCodePoint;
    return std::nullopt;
}
/// The faces a run is set in, indexed by their `Rendering::FontFace` bits:
/// regular, bold, italic and bold italic.
inline constexpr std::size_t runFaceCount = 4;
/// The fixed wire and private-handover size of one published face offer.
inline constexpr std::size_t publishedFaceBodyBytes = sizeof(std::uint32_t) + runFaceCount * sizeof(std::uint16_t)
    + runFaceCount * facedCodePointCount * (sizeof(std::uint16_t) + sizeof(std::uint8_t));
/// Weight zero uses its exact face; weights one through seven share an upper
/// envelope in sixteenths of a cell, rounded up to bound their advances.
inline constexpr std::uint32_t weightedAdvanceFractionsPerCell = 16;
/// The narrowest and widest cell a client may publish, in 1/64 pixel: one
/// pixel and 1,024 pixels.
inline constexpr std::uint32_t minFacedCellAdvance = 64;
inline constexpr std::uint32_t maxFacedCellAdvance = 1024 * 64;
/// The most cells one code point's published advance may span: a letter
/// wider than that is a face no frontend draws runs in.
inline constexpr std::uint32_t maxFacedAdvanceCells = 8;
/// The host admits at most this many clients, and offers at most one face
/// body from each to a pane. The handover reads at most 256 attachments.
inline constexpr std::size_t maxFaceOffersPerPane = 64;
inline constexpr std::size_t maxInheritedFaceOffersPerDaemon = 256;

/// Says whether a terminal's renderer can draw the faces it would publish.
///
/// The frontend and host apply the same gate, so a grid-only pane cannot
/// contribute a face to a hosted measurement.
///
/// - Parameter terminal: The pane whose renderer capability is checked.
/// - Returns: Whether the pane draws proportional runs.
[[nodiscard]] bool mayOfferFaceFacts(const Ubiquitty::Terminal& terminal);

/// How a client measures a proportional run: the faces its renderer sets
/// runs in, published to the host that runs a pane it draws, so a query
/// there is answered in the face the run is drawn in
/// (Docs/proportional-text.md, "A pane a host runs").
///
/// Weight-zero advances are in 1/64 of the client's pixel; weighted bounds
/// are in sixteenths of its cell. Both round up and the cell rounds down, so
/// a run counted from them is never narrower than the sum of those individually
/// shaped glyph advances.
struct ProportionalFaceFacts {
    /// One grid cell's advance; zero for none.
    std::uint32_t cellAdvance = 0;
    /// Each unweighted face's mean advance over `A`–`Z` and `a`–`z`.
    std::array<std::uint16_t, runFaceCount> untabledAdvances {};
    /// Per face, each tabled code point's exact weight-zero advance in
    /// 1/64 pixel, indexed by `facedCodePointIndex`.
    std::array<std::array<std::uint16_t, facedCodePointCount>, runFaceCount> advances {};
    /// Per face and code point, the widest of weight steps one to seven,
    /// rounded up in sixteenths of a cell. One byte bounds each advance.
    std::array<std::array<std::uint8_t, facedCodePointCount>, runFaceCount> weightedAdvances {};

    friend bool operator==(const ProportionalFaceFacts&, const ProportionalFaceFacts&) = default;
};

/// Per-code-point upper envelope over every face offered to a pane. Regular
/// advances are rounded up to 1/65,536 cell; weighted advances use the same
/// unit, so a summed answer dominates every counted face's own answer.
struct ProportionalFaceEnvelope {
    static constexpr std::uint32_t fractionsPerCell = 65'536;
    std::array<std::array<std::uint32_t, facedCodePointCount>, runFaceCount> regular {};
    std::array<std::array<std::uint32_t, facedCodePointCount>, runFaceCount> weighted {};
};

/// Upper bounds for face-body and envelope bytes retained by a host. A pane
/// refers to 64 bodies and keeps one envelope; a daemon keeps 64 current
/// client bodies, 256 inherited bodies and one envelope per 64 sessions.
/// Pane references share the bodies; pointer bookkeeping is excluded.
inline constexpr std::size_t maxRetainedFaceBytesPerPane = maxFaceOffersPerPane * sizeof(ProportionalFaceFacts) + sizeof(ProportionalFaceEnvelope);
inline constexpr std::size_t maxRetainedFaceBytesPerDaemon
    = (maxFaceOffersPerPane + maxInheritedFaceOffersPerDaemon) * sizeof(ProportionalFaceFacts)
    + maxFaceOffersPerPane * sizeof(ProportionalFaceEnvelope);
static_assert(sizeof(ProportionalFaceFacts) == publishedFaceBodyBytes);
static_assert(sizeof(ProportionalFaceEnvelope) == 34'304);
static_assert(maxRetainedFaceBytesPerPane == 858'368);
static_assert(maxRetainedFaceBytesPerDaemon == 6'315'776);

/// Whether published faces are ones a host can count runs in: a cell from
/// ``minFacedCellAdvance`` to ``maxFacedCellAdvance``, an untabled advance
/// for every face, and no advance wider than ``maxFacedAdvanceCells`` cells,
/// so faces published by a client cannot make a run's answer more than that
/// many cells a code point.
///
/// - Parameter facts: The faces.
/// - Returns: Whether they may be adopted.
[[nodiscard]] bool isAdoptable(const ProportionalFaceFacts& facts);

/// Measures the faces a frontend's measurer sets runs in, for a client to
/// publish to its host.
///
/// Shapes every tabled code point alone in each face. The measurer asked is
/// the one the frontend's own panes answer queries with, so a host's count
/// differs from a local pane's only by what shaping a whole run adds: pair
/// kerning and ligatures, which in a Latin text face close a run up rather
/// than open it.
///
/// - Parameters:
///   - measurer: The frontend's measurer.
///   - facts: Receives the faces.
/// - Returns: Whether the measurer shaped and named a cell; false leaves
///   `facts` unusable.
[[nodiscard]] bool measureProportionalFaces(Ubiquitty::TextMeasureClient& measurer, ProportionalFaceFacts& facts);

/// The cells a run needs in published faces: the sum of its code points'
/// advances over the cell, rounded up and capped to the largest claim. An
/// untabled code point uses the face's widest tabled advance at this weight;
/// a wide code point uses at least two cells. This bounds a real glyph when
/// the face's widest glyph is among the tabled scripts, while keeping a CJK
/// or emoji label from claiming a whole row for one untabled code point.
///
/// - Parameters:
///   - facts: The faces, adoptable.
///   - codePoints: The run's text.
///   - style: The face and weight class the run is set in.
///   - ambiguous: The terminal's width setting for untabled code points.
/// - Returns: The cell count, 1…``maxProportionalRunCells``.
[[nodiscard]] int tabledRunCells(const ProportionalFaceFacts& facts, std::span<const std::uint32_t> codePoints, Ubiquitty::ProportionalRunStyle style,
    Unicode::Width::AmbiguousWidth ambiguous);

/// The cell each of a run's code points begins in, through the frontend's
/// shaper and Core's layout (Rendering/ProportionalRun.h).
///
/// - Parameters:
///   - t: The terminal whose measurer answers.
///   - codePoints: The run's text.
///   - style: The face and weight the run is set in; see ``runStyleOf``.
///   - widthCells: The cells the run claims.
///   - horizontalAlign: The run's alignment, which moves where the text sits.
///   - cellOfCodePoint: Receives one cell index per code point; at least as
///     long as the text.
///   - textCellsOut: Receives, when given, the cells of the claim the text
///     reaches, from the first through the one the last letter's advance ends
///     in (``Rendering::proportionalRunTextCells``).
/// - Returns: Whether the frontend shaped the run; false leaves the span
///   unwritten, and the cells take the text sixteen apiece.
[[nodiscard]] bool proportionalRunCellOwners(const Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints, int widthCells,
    std::uint8_t horizontalAlign, Ubiquitty::ProportionalRunStyle style, std::span<std::uint16_t> cellOfCodePoint, int* textCellsOut = nullptr);

/// The caret a point inside a run is nearest, as a code-point index into its
/// text.
///
/// The run is laid out through the frontend's shaper and Core's layout, the
/// same layout ``proportionalRunCellOwners`` places the text by, so where a
/// pointer lands and which cell a letter is copied from cannot disagree. A
/// caret sits between letters, so the point rounds to the nearer glyph edge
/// rather than naming the glyph it falls inside. It never lands inside a
/// grapheme: a combining mark or an emoji sequence moves it to the cluster's
/// edge on the side it rounded towards, because between a letter and its mark
/// is no place a reader can stand.
///
/// The point is in cells from the run's first cell rather than in pixels: the
/// layout's pixels are the shaper's, which no frontend reporting a pointer has
/// reason to know.
///
/// - Parameters:
///   - t: The terminal whose measurer answers.
///   - codePoints: The run's text.
///   - style: The face and weight the run is set in; see ``runStyleOf``.
///   - widthCells: The cells the run claims.
///   - horizontalAlign: The run's alignment, which moves where the text sits.
///   - xInCells: The point, in cells from the run's first cell.
/// - Returns: The code-point index the caret stands before, 0 through the
///   text's length; or -1 when the frontend does not shape the run, which
///   leaves the caller on whole cells.
[[nodiscard]] int proportionalRunCaretCodePoint(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints, int widthCells,
    std::uint8_t horizontalAlign, Ubiquitty::ProportionalRunStyle style, double xInCells);

/// The letter a point inside a run falls on, as a code-point index into its
/// text.
///
/// Where ``proportionalRunCaretCodePoint`` rounds to the nearer edge between
/// two letters, this names the letter whose extent — its origin to the next
/// glyph's, or the text's end — holds the point: what a double click on a
/// letter asks, since the word it grows to is the word that letter is in.
///
/// - Parameters:
///   - t: The terminal whose measurer answers.
///   - codePoints: The run's text.
///   - style: The face and weight the run is set in; see ``runStyleOf``.
///   - widthCells: The cells the run claims.
///   - horizontalAlign: The run's alignment, which moves where the text sits.
///   - xInCells: The point, in cells from the run's first cell.
/// - Returns: The code-point index under the point, 0 through one less than
///   the text's length; the length for a point past the text; or -1 when the
///   frontend does not shape the run.
[[nodiscard]] int proportionalRunCodePointUnder(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints, int widthCells,
    std::uint8_t horizontalAlign, Ubiquitty::ProportionalRunStyle style, double xInCells);

/// The letter a point inside a run falls on, as the half-open code-point range
/// of its grapheme.
///
/// ``proportionalRunCodePointUnder`` widened to the cluster: a point on a
/// combining mark or inside an emoji sequence names the whole letter a reader
/// sees, since between a letter and its mark is no place a selection can
/// start or end.
///
/// - Parameters:
///   - t: The terminal whose measurer answers.
///   - codePoints: The run's text.
///   - style: The face and weight the run is set in; see ``runStyleOf``.
///   - widthCells: The cells the run claims.
///   - horizontalAlign: The run's alignment, which moves where the text sits.
///   - xInCells: The point, in cells from the run's first cell.
///   - first: Receives the index of the letter's first code point.
///   - end: Receives the index after its last.
/// - Returns: Whether the point is on a letter; false past the text or when
///   the frontend does not shape the run, which leaves `first` and `end`
///   unwritten.
[[nodiscard]] bool proportionalRunLetterUnder(Ubiquitty::Terminal& t, std::span<const std::uint32_t> codePoints, int widthCells,
    std::uint8_t horizontalAlign, Ubiquitty::ProportionalRunStyle style, double xInCells, int& first, int& end);

} // namespace Ubiquitty::OSC::TextSizing
