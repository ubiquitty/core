// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// TextSizeTable.h — the terminal's text-size spec store, on the pattern the
// link ring and the cluster pool set: a cell cannot hold a specification, so
// it holds a small id into this table and the consumer resolves it back. The
// id is the 4-bit ``ExtendedAttributes::TextSizeSpecField``, which is what
// sizes the table.
//
// The policies differ from both precedents on purpose. The link ring recycles
// its oldest slot when full, because a stale link renders as unclickable text
// — an absence. A stale SIZE id would change a block's geometry — a glyph
// drawn over columns the grid no longer gives it — so a live id is never
// reassigned while a cell can still name it. The cluster pool grows to make
// its ceiling unreachable; this table cannot grow past its id width, so the
// ceiling is reachable, and a distinct spec past it is refused only once a
// census has found every id still named: the run then renders unscaled, which
// is the protocol's own degradation for a terminal without the feature, and
// the refusal is logged.
//
// Reclamation is the caller's census, not this table's clock: `retainOnly`
// takes a mask of ids something still references and frees the rest.
// `Screen::reclaimTextSizeSpecs` builds that mask from every cell that can
// carry an id — both screens, the status row and every scrollback line — and
// runs on the clears and on a mint that finds the table full; a mask built
// from less frees ids the unscanned cells still carry, and a later mint hands
// their slots to different specs.

#include <array>
#include <cstdint>

namespace Ubiquitty {

/// One OSC 66 text-size specification, as parsed from the wire.
///
/// A plain record of the six quantities, wire-order and wire-ranged; what a
/// zero denominator or a zero width means is the consumer's question. Held
/// here rather than beside the parser so the model can store one without
/// reaching into a wire module.
struct TextSizeSpec {
    uint8_t scale = 1; ///< `s`, 1-7: the block is scale*width columns by scale rows.
    uint8_t widthCells = 0; ///< `w`, 0-7: cells per block; 0 means the terminal computes.
    uint8_t fractionNumerator = 0; ///< `n`, 0-15: fractional-scale numerator.
    uint8_t fractionDenominator = 0; ///< `d`, 0-15: exceeds `n` whenever it is set.
    uint8_t verticalAlign = 0; ///< `v`, 0-2: top, bottom, centered.
    uint8_t horizontalAlign = 0; ///< `h`, 0-3: left, right, centered; 3 justified, proportional runs only.
    /// `p`: the run is set in a proportional face across `runWidthCells`
    /// cells (Docs/proportional-text.md), each `scale` columns wide over
    /// `scale` rows; the other sizing fields above are then their defaults,
    /// the parser refusing any other combination.
    uint8_t isProportional = 0; // bool-ok: a wire byte beside five wire bytes, serialised and compared as one
    uint8_t isTextRun = 0; ///< An explicit-width block whose complete payload lives in RunTextStore.
    /// `c`, 1-1024: the cells a proportional run occupies, in cells of its
    /// scale; zero when the spec is not proportional.
    uint16_t runWidthCells = 0;
    /// The `c` a program asked for, when a rewrap clamped the run to a row
    /// narrower than that; zero when the run occupies what it asked for. A
    /// later rewrap gives the run back as much of its claim as a row holds.
    uint16_t claimedRunWidthCells = 0;
    /// Set when a rewrap broke a run at a space and the next proportional
    /// run of the logical line, cell after cell, holds the rest of its text;
    /// a rewrap that finds the pieces adjacent joins them. Every piece keeps
    /// the run's claim in `claimedRunWidthCells`.
    uint8_t isContinued = 0; // bool-ok: a wire byte beside the run's other wire fields

    friend constexpr bool operator==(const TextSizeSpec&, const TextSizeSpec&) = default;
};

/// The spec ids a terminal can hand out at once: the 4-bit pen field minus
/// the zero that means unsized.
inline constexpr int textSizeTableCapacity = 15;

/// Every id the table can hand out, as a mask: bit `id - 1` per id.
inline constexpr std::uint16_t allTextSizeSpecIDs = static_cast<std::uint16_t>((1u << textSizeTableCapacity) - 1);

/// The terminal's text-size spec store: intern a spec, resolve an id.
///
/// Ids are 1-based and stable for their lifetime: an id freed by
/// ``retainOnly`` may be minted again, an id something may still reference is
/// never reassigned to a different spec.
class TextSizeTable {
public:
    /// Interns `spec`, deduplicating by content.
    ///
    /// - Parameter spec: The specification to store.
    /// - Returns: The spec's 1-based id, or zero when every id is live with a
    ///   different spec — the caller emits its run unsized, and logs the
    ///   refusal it saw.
    [[nodiscard]] constexpr unsigned intern(const TextSizeSpec& spec)
    {
        int firstFree = -1;
        for (int i = 0; i < textSizeTableCapacity; ++i) {
            if (!m_live[static_cast<std::size_t>(i)]) {
                if (firstFree < 0)
                    firstFree = i;
                continue;
            }
            if (m_specs[static_cast<std::size_t>(i)] == spec)
                return static_cast<unsigned>(i + 1);
        }
        if (firstFree < 0)
            return 0;
        m_specs[static_cast<std::size_t>(firstFree)] = spec;
        m_live[static_cast<std::size_t>(firstFree)] = true;
        return static_cast<unsigned>(firstFree + 1);
    }

    /// Resolves an id to its specification.
    ///
    /// - Parameter specID: A pen's ``textSizeSpecID``.
    /// - Returns: The spec, or null for zero, out-of-range, and freed ids —
    ///   every failure folds into "render unsized", which is what a consumer
    ///   holding a stale id must do.
    [[nodiscard]] constexpr const TextSizeSpec* lookup(unsigned specID) const
    {
        if (specID == 0 || specID > static_cast<unsigned>(textSizeTableCapacity))
            return nullptr;
        if (!m_live[specID - 1])
            return nullptr;
        return &m_specs[specID - 1];
    }

    /// Frees every id absent from `referencedIDMask`.
    ///
    /// - Parameter referencedIDMask: Bit `id - 1` set for each id some cell
    ///   still carries, from a scan of every cell that can carry one — both
    ///   screens' grids, at least. A partial scan frees ids the unscanned
    ///   cells still reference, which is the stale-geometry failure the
    ///   no-reassignment rule exists to prevent.
    constexpr void retainOnly(std::uint16_t referencedIDMask)
    {
        for (int i = 0; i < textSizeTableCapacity; ++i) {
            if ((referencedIDMask & (1u << i)) == 0)
                m_live[static_cast<std::size_t>(i)] = false;
        }
    }

    /// The live-slot mask, bit `id - 1` per live id — the wire's shape.
    [[nodiscard]] constexpr std::uint16_t liveMask() const
    {
        std::uint16_t mask = 0;
        for (int i = 0; i < textSizeTableCapacity; ++i)
            if (m_live[static_cast<std::size_t>(i)])
                mask |= static_cast<std::uint16_t>(1u << i);
        return mask;
    }

    /// The spec stored in `slot`, live or not, for the wire's fixed walk.
    ///
    /// - Parameter slot: A 0-based slot index below the capacity.
    [[nodiscard]] constexpr const TextSizeSpec& specAtSlot(int slot) const { return m_specs[static_cast<std::size_t>(slot)]; }

    /// Reconstructs the table from a peer's mask and slots, verbatim.
    ///
    /// - Parameters:
    ///   - mask: The peer's live-slot mask.
    ///   - specs: One spec per slot, dead slots included — ids must land in
    ///     the slots the sender's cells reference.
    constexpr void restore(std::uint16_t mask, const std::array<TextSizeSpec, textSizeTableCapacity>& specs)
    {
        m_specs = specs;
        for (int i = 0; i < textSizeTableCapacity; ++i)
            m_live[static_cast<std::size_t>(i)] = (mask & (1u << i)) != 0;
    }

    /// How many ids are live.
    [[nodiscard]] constexpr int liveCount() const
    {
        int count = 0;
        for (int i = 0; i < textSizeTableCapacity; ++i)
            count += m_live[static_cast<std::size_t>(i)] ? 1 : 0;
        return count;
    }

private:
    std::array<TextSizeSpec, textSizeTableCapacity> m_specs {};
    std::array<bool, textSizeTableCapacity> m_live {};
};

} // namespace Ubiquitty
