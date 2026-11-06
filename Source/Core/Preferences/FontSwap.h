// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// What an OSC 50 request resolves to, given what the window is drawing and what
// the configuration says.
//
// The sequence names as little as it likes — a family, a size, a relative bump,
// or nothing at all — and the axes it leaves unset are filled from the live
// window, except for the empty request, which is a reset to the configured
// font. The result then clamps to the same range the `[font] size` stepper
// offers, because a size a reader cannot choose is not a size a program should
// be able to impose either.
//
// The rule is here rather than in a frontend because both shells apply it and a
// second spelling is a second answer: the arms would disagree about what
// `OSC 50 ; #+2` means on a window whose size the reader had already zoomed.

#include "Core/Preferences/Schema.h"

#include <string>
#include <string_view>

namespace Ubiquitty::Preferences {

/// A font swap as the sequence asked for it, beside what would answer it.
struct FontSwapRequest {
    /// The family the sequence named; empty when it named none.
    std::string_view requestedFamily;
    /// The absolute size it named, in points; zero or less when it named none.
    double requestedSizePoints = 0.0;
    /// The relative bump it named (rxvt `#+N` / `#-N`), in points; zero for none.
    double sizeDelta = 0.0;
    /// The family the window is drawing.
    std::string_view liveFamily;
    /// The size the window is drawing at, in points.
    double liveSizePoints = 0.0;
    /// The family the configuration names; empty when it names none.
    std::string_view configuredFamily;
    /// The size the configuration names, in points; zero or less for none.
    double configuredSizePoints = 0.0;
    /// Whether the sequence asked for the configured size alone, mintty's empty
    /// `OSC 7770` operand, which keeps the family drawn. The requested axes are
    /// then unused.
    bool isSizeResetOnly = false;
};

/// What a frontend should draw after a font swap.
struct FontSwapResolution {
    /// Whether anything can be drawn. False when no family could be resolved at
    /// all, which is a request to ignore rather than a face to fall back to:
    /// there is no name to hand a face store.
    bool isResolved = false;
    /// The family to draw from.
    std::string family;
    /// The size to draw at, in points, already clamped.
    double sizePoints = 0.0;
};

/// Resolves an OSC 50 request against the live window and the configuration.
///
/// An empty request — no family, no size, no bump — is the reset, and takes the
/// configured font. A size reset takes the configured size and keeps the
/// family the window is drawing. Otherwise each unset axis keeps what the window is drawing:
/// a size-only request keeps the family, a family-only request keeps the size.
/// A relative bump applies to the LIVE size rather than to the configured one,
/// so a reader who zoomed a window and a program that bumps it twice agree
/// about where the second bump starts.
///
/// The size clamps to `[font] size`'s declared range, read from the schema
/// rather than restated, so the ceiling a program can reach is the ceiling the
/// settings stepper offers.
///
/// - Parameter request: What the sequence named and what would answer it.
/// - Returns: The family and size to draw, or an unresolved answer when there
///   is no family to name.
[[nodiscard]] inline FontSwapResolution resolveFontSwap(const FontSwapRequest& request)
{
    std::string_view family = request.requestedFamily;
    double sizePoints = request.requestedSizePoints;

    if (request.isSizeResetOnly) {
        family = request.liveFamily.empty() ? request.configuredFamily : request.liveFamily;
        sizePoints = request.configuredSizePoints;
    } else {
        const bool isReset = request.requestedFamily.empty() && request.requestedSizePoints <= 0.0 && request.sizeDelta == 0.0;
        if (isReset) {
            family = request.configuredFamily;
            if (request.configuredSizePoints > 0.0)
                sizePoints = request.configuredSizePoints;
        }
        if (family.empty())
            family = request.liveFamily;
        // The bump is measured from what is on screen, which is what the reader
        // is looking at; a bump against the configured size would undo a zoom
        // the moment a program asked for one step.
        if (request.sizeDelta != 0.0)
            sizePoints = request.liveSizePoints + request.sizeDelta;
    }
    if (sizePoints <= 0.0)
        sizePoints = request.liveSizePoints;

    FontSwapResolution resolution;
    if (family.empty())
        return resolution;
    resolution.isResolved = true;
    resolution.family.assign(family);
    resolution.sizePoints = Schema::clampToRange(sizePoints, "font", "size");
    return resolution;
}

} // namespace Ubiquitty::Preferences
