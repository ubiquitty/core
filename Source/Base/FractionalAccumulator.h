// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// FractionalAccumulator.h — bank a running fractional quantity and release it
// as whole units.
//
// Feed real-valued deltas; take() returns the whole units completed so far and
// keeps the sub-unit remainder for next time. The canonical use is turning a
// stream of fractional scroll deltas (a slow trackpad pan, a high-resolution
// wheel) into whole lines to scroll without losing or double-counting the
// fraction between events.
//
// Truncation is toward zero, so a sign reversal starts fresh in the new
// direction instead of releasing banked opposite-direction magnitude: panning
// up then down does not first spend the banked up-fraction as a down-line.
//
// Trivially constructible (a single double), so it drops into a value-initialized field or
// an ObjC ivar with no ceremony.

namespace Ubiquitty {

/// Banks a running fractional quantity and releases whole units.
///
/// A trackpad reports scroll in fractional lines and a grid moves in whole
/// ones. Rounding each event independently loses the remainder every time, so a
/// slow drag never scrolls at all; banking it means the fractions sum to a line
/// and the line is delivered.
class FractionalAccumulator {
public:
    /// Adds `delta` to the bank and returns the whole units now available.
    ///
    /// The sub-unit remainder stays banked for the next call. Truncation is
    /// toward zero, so a reversal returns the remainder rather than a unit in
    /// the wrong direction.
    ///
    /// - Parameter delta: The fractional amount to add, either sign.
    /// - Returns: Whole units to apply now, possibly 0.
    int take(double delta)
    {
        m_residual += delta;
        const int whole = static_cast<int>(m_residual); // truncates toward zero
        m_residual -= static_cast<double>(whole);
        return whole;
    }

    /// Discards the banked remainder.
    ///
    /// For a gesture that ended, or a target that changed: carrying a stale
    /// fraction into a new gesture makes its first event overshoot by up to a
    /// whole unit, which reads as a jump nobody asked for.
    void reset() { m_residual = 0.0; }

    /// Returns the banked sub-unit remainder.
    ///
    /// Exposed so a caller can persist a gesture's position across a pause
    /// rather than re-deriving it, which would round the fraction away.
    ///
    /// - Returns: The remainder, in the open interval (-1, 1).
    [[nodiscard]] double residual() const { return m_residual; }

private:
    double m_residual = 0.0;
};

} // namespace Ubiquitty
