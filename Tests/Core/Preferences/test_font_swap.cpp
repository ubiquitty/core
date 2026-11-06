// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_font_swap.cpp — what an OSC 50 request resolves to.
//
// The sequence names as little as it likes, and the axes it leaves unset come
// from the window or, for the empty request, from the configuration. Both
// shells apply this, so the table is the agreement between them: a second
// spelling would let the arms disagree about what `OSC 50 ; #+2` means on a
// window the reader had already zoomed.

#include "Core/Preferences/FontSwap.h"

using Ubiquitty::Preferences::FontSwapRequest;
using Ubiquitty::Preferences::resolveFontSwap;

// A window drawing Menlo at 13, configured for JetBrains Mono at 11.
static FontSwapRequest standing()
{
    FontSwapRequest request;
    request.liveFamily = "Menlo";
    request.liveSizePoints = 13.0;
    request.configuredFamily = "JetBrains Mono";
    request.configuredSizePoints = 11.0;
    return request;
}

TEST(testAnEmptyRequestResetsToTheConfiguredFont)
{
    // No family, no size, no bump: the reset. The configuration answers both
    // axes, not the window — that is the whole difference between this and a
    // request that merely named nothing it could not have named.
    const auto resolution = resolveFontSwap(standing());
    CHECK(resolution.isResolved);
    CHECK(resolution.family == "JetBrains Mono");
    CHECK(resolution.sizePoints == 11.0);
    printf("testAnEmptyRequestResetsToTheConfiguredFont done\n");
}

TEST(testAnUnsetAxisKeepsWhatTheWindowIsDrawing)
{
    // A family alone keeps the size on screen rather than the configured one:
    // a reader who zoomed and a program that renames the face are answering
    // different questions, and only one of them was asked.
    FontSwapRequest familyOnly = standing();
    familyOnly.requestedFamily = "Courier New";
    const auto renamed = resolveFontSwap(familyOnly);
    CHECK(renamed.family == "Courier New");
    CHECK(renamed.sizePoints == 13.0);

    // A size alone keeps the face.
    FontSwapRequest sizeOnly = standing();
    sizeOnly.requestedSizePoints = 20.0;
    const auto resized = resolveFontSwap(sizeOnly);
    CHECK(resized.family == "Menlo");
    CHECK(resized.sizePoints == 20.0);
    printf("testAnUnsetAxisKeepsWhatTheWindowIsDrawing done\n");
}

TEST(testARelativeBumpMovesFromTheLiveSize)
{
    // rxvt's `#+N` / `#-N`. Measured from what is on screen: against the
    // configured size instead, the first bump after a zoom would undo it.
    FontSwapRequest up = standing();
    up.sizeDelta = 2.0;
    CHECK(resolveFontSwap(up).sizePoints == 15.0);
    CHECK(resolveFontSwap(up).family == "Menlo");

    FontSwapRequest down = standing();
    down.sizeDelta = -3.0;
    CHECK(resolveFontSwap(down).sizePoints == 10.0);
    printf("testARelativeBumpMovesFromTheLiveSize done\n");
}

TEST(testTheSizeClampsToTheStepperRange)
{
    // `[font] size` declares 6..96 in the schema, and a program reaches no
    // further than a reader can: a size nobody could choose in Preferences is
    // not one an escape sequence should be able to impose.
    FontSwapRequest huge = standing();
    huge.requestedSizePoints = 4000.0;
    CHECK(resolveFontSwap(huge).sizePoints == 96.0);

    // A bump that lands inside the range but under the floor takes the floor.
    FontSwapRequest small = standing();
    small.sizeDelta = -9.0; // 13 - 9 = 4, under the 6 the stepper stops at
    CHECK(resolveFontSwap(small).sizePoints == 6.0);

    // A bump that lands at or below zero is not a size at all, and the rule
    // restores what the window is drawing before the clamp ever sees it, so the
    // answer is the live size, not the floor. Stated because the two are easy
    // to confuse and only one of them keeps a terminal readable: a program that
    // subtracts a thousand points meant nothing coherent, and 13 is a better
    // reading of that than 6.
    FontSwapRequest collapsed = standing();
    collapsed.sizeDelta = -1000.0;
    CHECK(resolveFontSwap(collapsed).sizePoints == 13.0);
    printf("testTheSizeClampsToTheStepperRange done\n");
}

TEST(testASizeResetTakesTheConfiguredSizeAndKeepsTheFamily)
{
    // mintty's empty OSC 7770 operand restores the default size alone: the
    // family on screen stays, where the empty OSC 50 request would take the
    // configured one.
    FontSwapRequest sizeReset = standing();
    sizeReset.isSizeResetOnly = true;
    const auto resolution = resolveFontSwap(sizeReset);
    CHECK(resolution.isResolved);
    CHECK(resolution.family == "Menlo");
    CHECK(resolution.sizePoints == 11.0);

    // Nothing configured keeps the size on screen.
    FontSwapRequest unconfigured = sizeReset;
    unconfigured.configuredSizePoints = 0.0;
    CHECK(resolveFontSwap(unconfigured).sizePoints == 13.0);

    // Nothing drawn yet: the configured family is the one the window draws.
    FontSwapRequest undrawn = sizeReset;
    undrawn.liveFamily = {};
    CHECK(resolveFontSwap(undrawn).family == "JetBrains Mono");
    printf("testASizeResetTakesTheConfiguredSizeAndKeepsTheFamily done\n");
}

TEST(testARequestWithNoFamilyToNameResolvesToNothing)
{
    // Nothing named, nothing drawing, nothing configured: there is no name to
    // hand a face store, so the request is ignored rather than answered with a
    // fallback this layer would have to invent.
    FontSwapRequest bare;
    bare.requestedSizePoints = 14.0;
    const auto resolution = resolveFontSwap(bare);
    CHECK(!resolution.isResolved);
    CHECK(resolution.family.empty());

    // A window already drawing something answers the same request.
    FontSwapRequest live = bare;
    live.liveFamily = "Menlo";
    live.liveSizePoints = 13.0;
    CHECK(resolveFontSwap(live).isResolved);
    CHECK(resolveFontSwap(live).family == "Menlo");
    printf("testARequestWithNoFamilyToNameResolvesToNothing done\n");
}

UBIQUITTY_TEST_MAIN()
