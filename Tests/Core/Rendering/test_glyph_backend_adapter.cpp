// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_glyph_backend_adapter — the fixture adapter installs what the fixture declares.
//
// `UbiquittyTest::glyphBackendFor` detects the two optional entries by calling
// them inside a `requires` clause. A probe naming a signature the fixture does
// not have takes the false branch: the entry is installed as null, Core
// substitutes, and every case still passes. Silent absence reading as a
// decision is the failure `StrictAtlas.h` is written against, and its own
// detection had it.
//
// `SoftwareRenderer` spells both entries as a pointer and a count where the
// probes passed a `std::span`, so neither matched. Every builder case driven by
// the real renderer ran with both entries null and exercised only Core's
// substitution — the base code point alone for a cluster, foreground coverage
// for an emoji. The hooks were reachable and never reached.
//
// A green suite cannot report that, which is why this file exists: the suite
// passes on the substitution and passes identically once the entry is
// installed. So the assertion has to be about the adapter rather than about any
// case that routes through it, and "a fixture declaring an entry gets it
// installed" is decidable here.

#include "Harness.h"

#include "Core/Rendering/CellInstanceBuilder.h"
#include "Core/Rendering/SoftwareRenderer.h"
#include "StrictAtlas.h"

#include <cstdint>
#include <span>

namespace {

using namespace Ubiquitty;
using namespace Ubiquitty::Rendering;

// The seven entries every fixture owes, so each arm below differs only in the
// two optional ones.
#define UBIQUITTY_TEST_REQUIRED_ATLAS_ENTRIES                                                  \
    void beginFrame() { }                                                                      \
    MonoAtlasBinding bindMonoAtlas() { return {}; }                                            \
    bool rasteriseMonoGlyph(std::uint32_t, FontFace, std::uint32_t, int) { return true; }      \
    AtlasGlyph monoGlyph(std::uint32_t, FontFace, std::uint32_t, std::uint32_t) { return {}; } \
    AtlasGlyph wideGlyph(std::uint32_t, FontFace) { return {}; }                               \
    AtlasGlyph doubleGlyph(std::uint32_t, FontFace, DoubleRegion) { return {}; }

// Declares both the way a fixture does, so the span-shaped probe is the one
// that has to match.
struct SpanShapedAtlas : UbiquittyTest::StrictAtlas {
    UBIQUITTY_TEST_REQUIRED_ATLAS_ENTRIES

    AtlasGlyph clusterGlyph(std::span<const std::uint32_t>, FontFace, std::uint32_t, std::uint32_t) { return {}; }
    AtlasGlyph colorGlyph(std::span<const std::uint32_t>, std::uint16_t) { return {}; }
};

// Declares neither, so what it carries is `StrictAtlas`'s aborting pair. The
// adapter has to install those: an installed abort is what turns reaching an
// unmodelled path into a failure, where a null entry hands the question to
// Core's substitution and gets a plausible answer back.
struct InheritingAtlas : UbiquittyTest::StrictAtlas {
    UBIQUITTY_TEST_REQUIRED_ATLAS_ENTRIES
};

// Declares neither and inherits nothing, so both entries stay null.
struct PlainAtlas {
    UBIQUITTY_TEST_REQUIRED_ATLAS_ENTRIES
};

} // namespace

// The renderer the builder cases actually drive. It publishes its own table, so
// the adapter has to hand that back rather than build a second one: the
// renderer gates its colour entries on whether the rasterizer has a colour
// source, and a table reconstructed from its members cannot carry that decision.
TEST(theAdapterReturnsSoftwareRenderersOwnTable)
{
    SoftwareRenderer renderer { 8, 16 };
    const GlyphBackend adapted = UbiquittyTest::glyphBackendFor(renderer);
    const GlyphBackend published = renderer.glyphBackend();

    CHECK_MSG(adapted.clusterGlyph == published.clusterGlyph,
        "the cluster entry is the renderer's own, so a builder case shapes clusters "
        "the way production does instead of falling back to the base code point");
    CHECK_MSG(adapted.colorGlyph == published.colorGlyph,
        "the colour entry is the renderer's own, including its absence: this "
        "rasterizer has no colour source, and installing a hook that answers "
        "absent every time would report a complete backend over that loss");
    CHECK_MSG(adapted.wideClusterGlyph == published.wideClusterGlyph, "and the wide-cluster entry");

    // The property those three serve, stated once. A backend claiming no loss
    // while dropping marks is the failure the whole seam is arranged against.
    CHECK_MSG(Rendering::optionalDefaultsAreLossy(adapted)
            == Rendering::optionalDefaultsAreLossy(published),
        "the adapter reports the same loss the renderer does");
}

// The other spelling, so the assertion above is about detection and not about
// one renderer.
TEST(theAdapterInstallsWhatASpanShapedFixtureDeclares)
{
    SpanShapedAtlas atlas;
    const GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);

    CHECK_MSG(backend.clusterGlyph != nullptr, "a span-shaped clusterGlyph is installed");
    CHECK_MSG(backend.colorGlyph != nullptr, "a span-shaped colorGlyph is installed");
}

// The aborting pair is an entry like any other and has to arrive installed, or
// `StrictAtlas` refuses nothing.
TEST(aFixtureThatModelsNeitherGetsTheAbortingPair)
{
    InheritingAtlas atlas;
    const GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);

    CHECK_MSG(backend.clusterGlyph != nullptr,
        "StrictAtlas's aborting clusterGlyph is installed, so a cluster reaching this "
        "fixture fails instead of being answered by its base code point");
    CHECK_MSG(backend.colorGlyph != nullptr,
        "StrictAtlas's aborting colorGlyph is installed, so an emoji reaching this "
        "fixture fails instead of being answered by foreground coverage");
}

// And the arm that makes every assertion above falsifiable. An adapter
// installing an entry unconditionally satisfies all of them, and would tell a
// fixture modelling nothing that it models both.
TEST(theAdapterInstallsNothingAFixtureNeitherDeclaresNorInherits)
{
    PlainAtlas atlas;
    const GlyphBackend backend = UbiquittyTest::glyphBackendFor(atlas);

    CHECK_MSG(backend.clusterGlyph == nullptr, "nothing to install, so the entry stays null");
    CHECK_MSG(backend.colorGlyph == nullptr, "nothing to install, so the entry stays null");
}

UBIQUITTY_TEST_MAIN()
