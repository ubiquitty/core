// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/CellInstance.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// A dotted underline's dots, fitted between the cuts skip-ink makes.
//
// A dot is whole or absent: half a dot reads as a speck. Laid on the run's own
// rhythm, a stretch between two descenders loses a dot whenever the rhythm puts
// one across the stretch's end, and keeps a wider gap beside one letter than
// beside the other. So each stretch between two cuts takes as many whole dots
// as fit in it, centred. A cell sees only its own glyph and its neighbours',
// and a stretch can run on across several cells, so the cells of a run are
// held until the run ends and its dots are laid across the whole of it.

namespace Ubiquitty::Rendering {

/// What lies past a dotted run's last cell held so far.
enum class DottedRunEdge : std::uint8_t {
    /// The underline goes on past the cell, so the run is not yet whole.
    Continues,
    /// The run ends at the cell.
    Ends,
};

/// A run of dotted rule, as its dots are laid.
struct DottedRunFrame {
    /// The run's left and right edges, in device pixels.
    float left = 0.0f;
    float right = 0.0f;
    /// Where the run's own dots start: dot zero at its first cell's edge.
    float origin = 0.0f;
    /// Device pixels from one dot to the next.
    float pitch = 0.0f;
    /// A dot's length along the rule.
    float dot = 0.0f;
    /// Whether the line ends at `right`, rather than going on past it.
    bool isEnding = false;
};

/// A stretch of a dotted run drawn, and where its dots start.
struct DottedStretch {
    float left = 0.0f;
    float right = 0.0f;
    /// Dot zero's left edge: the stretch's dots fall at origin + k * pitch.
    float origin = 0.0f;
};

/// Lays a dotted run's dots in the stretches its cuts leave.
///
/// The stretch before the first cut keeps the run's own rhythm, so a run starts
/// with a whole dot at its edge, as an uncut run does. Every stretch after a cut
/// that ends, at the next cut or at the run's end, holds as many whole dots as
/// fit in it, centred between its ends and on the rhythm's pixel phase, so a
/// centred dot is as sharp as the rest. A stretch running on past the run's
/// right edge keeps the rhythm from its first whole dot, since where it ends is
/// not known yet. Only whole dots are drawn where a stretch ends.
///
/// - Parameters:
///   - frame: The run.
///   - cuts: The cut spans along the run, [left, right) pairs in order; spans
///     that meet or overlap are taken as one.
///   - stretches: Where the stretches drawn are written, in order and apart.
void fitDotsBetweenCuts(const DottedRunFrame& frame, std::span<const float> cuts, std::vector<DottedStretch>& stretches);

/// A dotted underline's run of cells, held until the run ends so its dots are
/// fitted across the whole of it (``fitDotsBetweenCuts``).
///
/// Each cell's rule is held as one uncut piece with its cuts beside it. When the
/// run is released its dots are laid once, and each cell draws its share of the
/// stretches as pieces that start their dots where their stretch does: a dot
/// across a cell edge is drawn by both cells from the same origin. Storage is
/// kept from one run to the next, so a frame allocates only for its longest run.
class DottedRuleRun {
public:
    /// The trace index of a cell the skip-ink test did not run on, or one no
    /// trace was asked for.
    static constexpr std::size_t untraced = static_cast<std::size_t>(-1);

    /// Whether no cell is held.
    [[nodiscard]] bool isEmpty() const { return m_cells.empty(); }

    /// Lets any run held go without drawing it, keeping the storage.
    void reset()
    {
        m_cells.clear();
        m_cuts.clear();
        m_stretches.clear();
    }

    /// Whether a cell's rule carries on the run held: it starts where the last
    /// held rule ends, at the same height and weight, on the same rhythm.
    ///
    /// - Parameter rule: The cell's rule as one uncut piece.
    /// - Returns: True when it does; false when no run is held.
    [[nodiscard]] bool doesContinueWith(const DecorationInstance& rule) const;

    /// Holds one cell's rule.
    ///
    /// - Parameters:
    ///   - rule: The cell's rule as one uncut piece, its band starting where the
    ///     run's dots do.
    ///   - cuts: The cell's cut spans, [left, right) pairs in order, the same
    ///     across the rule's height.
    ///   - rightEdge: Whether the underline goes on past the cell.
    ///   - traceIndex: The skip-ink trace the cell's pieces are recorded in, or
    ///     ``untraced``.
    void hold(const DecorationInstance& rule, std::span<const float> cuts, DottedRunEdge rightEdge, std::size_t traceIndex);

    /// Lays the held run's dots, hands each cell's pieces to `emitPiece` in
    /// order, calls `endCell(traceIndex, pieceCount)` after each cell's, and
    /// lets the run go.
    ///
    /// - Parameters:
    ///   - emitPiece: Called with each piece, a `const DecorationInstance&`.
    ///   - endCell: Called after each cell's pieces with its trace index and how
    ///     many it drew.
    template<typename EmitPiece, typename EndCell>
    void release(EmitPiece&& emitPiece, EndCell&& endCell);

private:
    /// One held cell: its uncut rule, and the trace its pieces are counted in.
    struct Cell {
        DecorationInstance rule;
        std::size_t traceIndex = untraced;
    };

    /// Lays the held run's dots into ``m_stretches``.
    void fit();

    std::vector<Cell> m_cells;
    std::vector<float> m_cuts;
    std::vector<DottedStretch> m_stretches;
    DottedRunEdge m_rightEdge = DottedRunEdge::Ends;
};

template<typename EmitPiece, typename EndCell>
void DottedRuleRun::release(EmitPiece&& emitPiece, EndCell&& endCell)
{
    fit();
    std::size_t first = 0;
    for (const Cell& cell : m_cells) {
        const float left = cell.rule.topLeftInPixels.x;
        const float right = cell.rule.bottomRightInPixels.x;
        // The stretches are in order and the cells too, so each cell starts
        // from the first stretch that does not end before it.
        while (first < m_stretches.size() && m_stretches[first].right <= left)
            ++first;
        std::uint32_t pieceCount = 0;
        for (std::size_t index = first; index < m_stretches.size() && m_stretches[index].left < right; ++index) {
            const DottedStretch& stretch = m_stretches[index];
            // A piece lies inside its band: a renderer lays the dots from the
            // band, and a part outside it would show a dot the band does not
            // hold. The CPU renderer clips to the band and a GPU's quad does
            // not, so a piece reaching past it drew a sliver of a phantom dot
            // on the GPU alone. A stretch's fringe before its first dot is
            // outside its band, as is a stretch starting at the band's end.
            DecorationInstance piece = cell.rule;
            piece.topLeftInPixels.x = std::max({ left, stretch.left, stretch.origin });
            piece.bottomRightInPixels.x = std::min({ right, stretch.right, cell.rule.bandBottomRightInPixels.x });
            if (!(piece.bottomRightInPixels.x > piece.topLeftInPixels.x))
                continue;
            piece.bandTopLeftInPixels.x = stretch.origin;
            emitPiece(static_cast<const DecorationInstance&>(piece));
            ++pieceCount;
        }
        endCell(cell.traceIndex, pieceCount);
    }
    m_cells.clear();
    m_cuts.clear();
    m_stretches.clear();
}

} // namespace Ubiquitty::Rendering
