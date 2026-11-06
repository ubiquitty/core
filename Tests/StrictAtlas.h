// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// StrictAtlas — a GlyphBackend fixture base that answers nothing it was not
// asked to model, and `glyphBackendFor` — the adapter that turns a fixture into
// the table `buildCellInstances` takes.
//
// The two dangerous entries are `clusterGlyph` and `colorGlyph`. Left null they
// are not absent: Core substitutes, and both substitutions produce a real glyph
// at a plausible UV. A cluster becomes its base code point alone — an `e` where
// the text said `é` — and an emoji draws its coverage in the foreground colour.
// Both are right for a backend with no cluster shaping and no colour sheet,
// which should draw something rather than nothing. Both are wrong for a fixture,
// where they satisfy assertions about counts, streams and phases while the marks
// the case was about are silently dropped.
//
// A builder that excludes combining clusters from the subpixel phase gate
// passes a test that records the exclusion as the contract, and no fixture can
// contradict it while every one of them inherits the answer.
//
// FAILS ON CALL, not on construction. A base that refused at construction would
// force every cluster-agnostic fixture to opt out of a path it never reaches,
// which is noise in four files to protect one. Failing on call costs those four
// nothing — they never arrive here — and the moment a builder change routes a
// cluster somewhere new, the fixture that receives it says so instead of
// answering. A fixture that models clusters declares its own and hides this one.
//
// It aborts rather than recording a failure: the harness's counter is local to
// a case body, and reaching an unmodelled path is a statement about the fixture
// rather than an assertion inside a case. An abort names the file, cannot be
// read as a pass, and leaves the driver a non-zero status.

#pragma once

#include "Core/Rendering/CellInstanceBuilder.h"

#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace UbiquittyTest {

/// A fixture base whose two lossy entries refuse instead of guessing.
///
/// Derive a fixture from this rather than declaring the seam's entries from
/// nothing. The required entries are still the fixture's to write; these two
/// abort if the code under test ever reaches them, so a fixture cannot answer
/// for a path it does not model.
struct StrictAtlas {
    /// Aborts, because this fixture does not model combining clusters.
    ///
    /// - Parameter count: The cluster length, named in the message.
    /// - Returns: Never returns.
    Ubiquitty::Rendering::AtlasGlyph clusterGlyph(std::span<const std::uint32_t> codePoints,
        Ubiquitty::Rendering::FontFace, std::uint32_t, std::uint32_t)
    {
        const int count = static_cast<int>(codePoints.size());
        std::fprintf(stderr,
            "FAIL %s: the code under test asked this fixture for a %d-code-point "
            "cluster.\n  It does not model clusters, and Core's substitution would "
            "have answered\n  with the base code point alone — a real glyph and a "
            "plausible UV, which is\n  indistinguishable from correct. Declare "
            "clusterGlyph here, or stop routing\n  clusters to this path.\n",
            __FILE__, count);
        std::abort();
    }

    /// Aborts, because this fixture does not model colour glyphs.
    ///
    /// - Parameter count: The cluster length, named in the message.
    /// - Returns: Never returns.
    Ubiquitty::Rendering::AtlasGlyph colorGlyph(std::span<const std::uint32_t> codePoints, std::uint16_t)
    {
        const int count = static_cast<int>(codePoints.size());
        std::fprintf(stderr,
            "FAIL %s: the code under test asked this fixture for a %d-code-point "
            "colour glyph,\n  which it does not model. Declare colorGlyph here, or "
            "stop routing colour\n  glyphs to this path.\n",
            __FILE__, count);
        std::abort();
    }
};

/// Builds the seam's table from a fixture that declares the entries by name.
///
/// Only tests use this. A real backend fills the table itself, entry by entry,
/// because what it leaves null is a decision about what the frontend supplies
/// and detection would make that decision by accident. A fixture is the opposite
/// case: the same seven required entries across six files, where detection bounds
/// the noise and ``StrictAtlas`` supplies the failure for the two that matter.
///
/// The four optional entries are installed when the fixture has them, so a
/// fixture deriving from ``StrictAtlas`` and declaring neither still gets the
/// aborting pair rather than Core's silent substitution.
///
/// - Parameter fixture: The fixture, which must outlive every build it drives.
/// - Returns: A table pointing at `fixture`.
template<class Fixture>
Ubiquitty::Rendering::GlyphBackend glyphBackendFor(Fixture& fixture)
{
    // A fixture that already publishes a table is asked for it, and nothing here
    // reconstructs one. `SoftwareRenderer::glyphBackend` gates its colour entries
    // on whether the rasterizer has a colour source, because installing them
    // regardless reports a complete backend over exactly that loss —
    // `optionalDefaultsAreLossy` would answer false while every emoji still drew
    // as foreground coverage. A second table built from the same members here
    // cannot carry that decision and would contradict the one production uses.
    if constexpr (requires(Fixture& f) {
                      { f.glyphBackend() } -> std::same_as<Ubiquitty::Rendering::GlyphBackend>;
                  }) {
        return fixture.glyphBackend();
    } else {

        using Ubiquitty::Rendering::FontFace;
        using Ubiquitty::Rendering::GlyphPass;
        using Ubiquitty::Rendering::LigatureCell;
        using Ubiquitty::Rendering::LigatureRun;

        Ubiquitty::Rendering::GlyphBackend backend;
        backend.context = &fixture;
        backend.beginFrame = [](void* context) { static_cast<Fixture*>(context)->beginFrame(); };
        backend.bindMonoAtlas = [](void* context) { return static_cast<Fixture*>(context)->bindMonoAtlas(); };
        backend.rasteriseMonoGlyph = [](void* context, std::uint32_t codePoint, FontFace face, std::uint32_t phase, int slot) {
            return static_cast<Fixture*>(context)->rasteriseMonoGlyph(codePoint, face, phase, slot);
        };
        backend.monoGlyph = [](void* context, std::uint32_t codePoint, FontFace face, std::uint32_t weight, std::uint32_t phase) {
            return static_cast<Fixture*>(context)->monoGlyph(codePoint, face, weight, phase);
        };
        backend.wideGlyph = [](void* context, std::uint32_t codePoint, FontFace face) {
            return static_cast<Fixture*>(context)->wideGlyph(codePoint, face);
        };
        backend.doubleGlyph = [](void* context, std::uint32_t codePoint, FontFace face, std::uint32_t, Ubiquitty::Rendering::DoubleRegion region) {
            return static_cast<Fixture*>(context)->doubleGlyph(codePoint, face, region);
        };
        // Two shapes, because `SoftwareRenderer` spells this one as a pointer and a
        // count while a fixture spells it as a span — the spelling `GlyphBackend`
        // declares, and the one `wideClusterGlyph` beside it uses. A probe knowing
        // only one spelling does not fail on the other: `if constexpr` takes the
        // false branch and the entry is installed as null, which is exactly what a
        // fixture modelling nothing also gets. Every builder case driven by the real
        // renderer therefore ran on Core's base-code-point substitution.
        constexpr bool hasSpanShapedClusterGlyph
            = requires(Fixture& f) { f.clusterGlyph(std::span<const std::uint32_t> {}, FontFace::Regular, 0u, 0u); };
        constexpr bool hasPointerShapedClusterGlyph = requires(Fixture& f) {
            f.clusterGlyph(static_cast<const std::uint32_t*>(nullptr), 0, FontFace::Regular, 0u, 0u);
        };
        if constexpr (hasSpanShapedClusterGlyph) {
            backend.clusterGlyph = [](void* context, std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight, std::uint32_t phase) {
                return static_cast<Fixture*>(context)->clusterGlyph(codePoints, face, weight, phase);
            };
        } else if constexpr (hasPointerShapedClusterGlyph) {
            backend.clusterGlyph = [](void* context, std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight, std::uint32_t phase) {
                return static_cast<Fixture*>(context)->clusterGlyph(codePoints.data(), static_cast<int>(codePoints.size()), face, weight, phase);
            };
        }
        static_assert(!requires { &Fixture::clusterGlyph; } || hasSpanShapedClusterGlyph || hasPointerShapedClusterGlyph, "this fixture declares clusterGlyph in a shape no probe here matches, so the entry would be "
                                                                                                                          "installed as null and Core would answer with the base code point alone");
        if constexpr (requires(Fixture& f) { f.wideClusterGlyph(std::span<const std::uint32_t> {}, FontFace::Regular, 0u); }) {
            backend.wideClusterGlyph = [](void* context, std::span<const std::uint32_t> codePoints, FontFace face, std::uint32_t weight, std::uint32_t) {
                return static_cast<Fixture*>(context)->wideClusterGlyph(codePoints, face, weight);
            };
        }
        // The glyph a shaper chose, keyed by the face's own identity rather than
        // by the text that produced it. Unwired here, no fixture can reach the
        // builder's identity branch at all: the branch that draws a healed
        // letterform from a slot of its own was covered by the arms that run a
        // real shaper and by nothing a case in this tree can vary.
        if constexpr (requires(Fixture& f) { f.rasteriseShapedGlyph(std::uint32_t { 0 }, FontFace::Regular, 0u, 0); }) {
            backend.rasteriseShapedGlyph = [](void* context, std::uint32_t glyphID, FontFace face, std::uint32_t variant, int slot) {
                return static_cast<Fixture*>(context)->rasteriseShapedGlyph(glyphID, face, variant, slot);
            };
        }
        // The proportional strip, for a fixture that shapes runs in a second face.
        if constexpr (requires(Fixture& f) { f.proportionalStrip(std::span<const std::uint32_t> {}, 1, std::uint8_t { 0 }, 0.0f, FontFace::Regular, 0u, 0u); }) {
            backend.proportionalStrip = [](void* context, std::span<const std::uint32_t> codePoints, int widthCells, std::uint8_t horizontalAlign, float penResidualInPixels, FontFace face, std::uint32_t weight, std::uint32_t variant, int, Ubiquitty::Rendering::SizedTile) {
                return static_cast<Fixture*>(context)->proportionalStrip(codePoints, widthCells, horizontalAlign, penResidualInPixels, face, weight, variant);
            };
        }
        // The same two shapes, for the same reason.
        constexpr bool hasSpanShapedColorGlyph
            = requires(Fixture& f) { f.colorGlyph(std::span<const std::uint32_t> {}, std::uint16_t { 0 }); };
        constexpr bool hasPointerShapedColorGlyph = requires(Fixture& f) {
            f.colorGlyph(static_cast<const std::uint32_t*>(nullptr), 0, std::uint16_t { 0 });
        };
        if constexpr (hasSpanShapedColorGlyph) {
            backend.colorGlyph = [](void* context, std::span<const std::uint32_t> codePoints, std::uint16_t clusterID) {
                return static_cast<Fixture*>(context)->colorGlyph(codePoints, clusterID);
            };
        } else if constexpr (hasPointerShapedColorGlyph) {
            backend.colorGlyph = [](void* context, std::span<const std::uint32_t> codePoints, std::uint16_t clusterID) {
                return static_cast<Fixture*>(context)->colorGlyph(codePoints.data(), static_cast<int>(codePoints.size()), clusterID);
            };
        }
        static_assert(!requires { &Fixture::colorGlyph; } || hasSpanShapedColorGlyph || hasPointerShapedColorGlyph, "this fixture declares colorGlyph in a shape no probe here matches, so the entry would be "
                                                                                                                    "installed as null and Core would answer with foreground coverage");

        // The remaining optional entries, guarded the same way. A member that exists
        // and matches no probe is this file's own subject one level up: silent
        // absence reading as a decision. A compile error is the only place it cannot
        // be mistaken for a fixture that declined to model the path.
        static_assert(!requires { &Fixture::wideClusterGlyph; } || requires(Fixture& f) { f.wideClusterGlyph(std::span<const std::uint32_t> {}, FontFace::Regular, 0u); }, "this fixture declares wideClusterGlyph in a shape the probe here does not match");
        static_assert(!requires { &Fixture::boxGlyph; } || requires(Fixture& f) { f.boxGlyph(0u, FontFace::Regular); }, "this fixture declares boxGlyph in a shape the probe here does not match");
        static_assert(!requires { &Fixture::stageMonoCoverage; } || requires(Fixture& f) { f.stageMonoCoverage(0, std::span<const std::uint8_t> {}); }, "this fixture declares stageMonoCoverage in a shape the probe here does not match");
        if constexpr (requires(Fixture& f) { f.boxGlyph(0u, FontFace::Regular); }) {
            backend.boxGlyph = [](void* context, std::uint32_t codePoint, FontFace face) {
                return static_cast<Fixture*>(context)->boxGlyph(codePoint, face);
            };
        }
        if constexpr (requires(Fixture& f) { f.stageMonoCoverage(0, std::span<const std::uint8_t> {}); }) {
            backend.stageMonoCoverage = [](void* context, int slot, std::span<const std::uint8_t> coverage) {
                return static_cast<Fixture*>(context)->stageMonoCoverage(slot, coverage);
            };
        }
        if constexpr (requires(Fixture& f) { f.shapeLigatureRuns(std::span<const LigatureCell> {}, 0.0f, 0.0f, GlyphPass::CellBaked, std::span<LigatureRun> {}); }) {
            backend.shapeLigatureRuns = [](void* context, std::span<const LigatureCell> row,
                                            float rowOriginXInPixels, float cellWidthInPixels, GlyphPass pass, std::span<LigatureRun> runs) {
                static_cast<Fixture*>(context)->shapeLigatureRuns(row, rowOriginXInPixels, cellWidthInPixels, pass, runs);
            };
        }
        return backend;
    }
}

} // namespace UbiquittyTest
