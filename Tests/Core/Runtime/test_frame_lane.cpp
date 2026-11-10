// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_frame_lane — one view's frames of a pane: the owner claims a capture no
// view holds, writes it and publishes it; the view takes the newest and holds
// it until it takes the next. The exchange's own interleavings are
// test_sync_frame_slot's; this file holds the lane's composition of it with the
// captures' references and their selections.

#include "Harness.h"

#include "Core/Runtime/FrameLane.h"

#include <array>
#include <optional>
#include <utility>

using Ubiquitty::Rendering::FrameCapture;
using Ubiquitty::Runtime::FrameLane;
using Ubiquitty::Runtime::PresentedCapture;

namespace {

// Claims, "writes" and publishes one capture, as the owner's boundary does.
FrameCapture* publishOne(FrameLane& lane)
{
    const std::optional<FrameLane::Target> target = lane.claimTarget();
    CHECK(target.has_value());
    if (!target)
        return nullptr;
    lane.publish(*target);
    return target->capture;
}

int referencesOf(const FrameCapture& capture)
{
    return capture.swiftReferences.load(std::memory_order_relaxed);
}

} // namespace

// The view takes the capture published last, with a reference of its own,
// and the next take has nothing new.
TEST(aViewTakesTheNewestPublishedCapture)
{
    FrameLane lane;
    REQUIRE(lane.arm());
    CHECK(lane.takeReady() == nullptr);

    static_cast<void>(publishOne(lane));
    FrameCapture* newest = publishOne(lane);
    CHECK(lane.hasReadyCapture());
    PresentedCapture held = PresentedCapture::adopt(lane.takeReady());
    REQUIRE(static_cast<bool>(held));
    CHECK(&held.capture() == newest);
    CHECK_EQ(referencesOf(*newest), 2);
    CHECK_EQ(lane.overwriteCount(), 1u);
    CHECK_EQ(lane.promoteCount(), 1u);
    CHECK(!lane.hasReadyCapture());
    CHECK(lane.takeReady() == nullptr);
}

// A capture a view still holds is never claimed for writing, even once the
// exchange counts its slot free; letting it go makes it claimable again.
TEST(aCaptureAViewHoldsIsNeverWrittenOver)
{
    FrameLane lane;
    REQUIRE(lane.arm());
    static_cast<void>(publishOne(lane));
    PresentedCapture held = PresentedCapture::adopt(lane.takeReady());
    REQUIRE(static_cast<bool>(held));
    const FrameCapture* kept = &held.capture();
    // A later take frees the held capture's slot in the exchange; the view's
    // reference still keeps it out of every claim.
    static_cast<void>(publishOne(lane));
    PresentedCapture next = PresentedCapture::adopt(lane.takeReady());
    for (int round = 0; round < 6; ++round) {
        const std::optional<FrameLane::Target> target = lane.claimTarget();
        REQUIRE(target.has_value());
        CHECK(target->capture != kept);
        CHECK(target->capture != &next.capture());
        lane.cancel(*target);
    }

    held = PresentedCapture();
    bool didClaimKept = false;
    for (int round = 0; round < 3 && !didClaimKept; ++round) {
        const std::optional<FrameLane::Target> target = lane.claimTarget();
        REQUIRE(target.has_value());
        didClaimKept = target->capture == kept;
        lane.publish(*target);
        next = PresentedCapture::adopt(lane.takeReady());
    }
    CHECK_MSG(didClaimKept, "a capture no view holds is written again");
}

// Every capture a view holds is claimed by none, so with all of them held the
// owner writes nothing.
TEST(aLaneWhoseCapturesAreAllHeldClaimsNone)
{
    FrameLane lane;
    REQUIRE(lane.arm());
    std::array<std::optional<FrameLane::Target>, 3> targets;
    for (std::optional<FrameLane::Target>& target : targets) {
        target = lane.claimTarget();
        REQUIRE(target.has_value());
        target->capture->swiftReferences.fetch_add(1, std::memory_order_relaxed);
        lane.cancel(*target);
    }
    CHECK(!lane.claimTarget().has_value());
    for (std::optional<FrameLane::Target>& target : targets)
        target->capture->swiftReferences.fetch_sub(1, std::memory_order_relaxed);
    CHECK(lane.claimTarget().has_value());
}

// A capture made under a geometry registration older than the view's newest
// carries the old placement: the take retires it, and takes the next one made
// under the newest.
TEST(aCaptureOlderThanTheViewsRegistrationIsNotTaken)
{
    FrameLane lane;
    REQUIRE(lane.arm());
    FrameCapture* stale = publishOne(lane);
    REQUIRE(stale->viewGeneration() == 0);
    CHECK(lane.takeReady(1) == nullptr);
    CHECK_EQ(referencesOf(*stale), 1);
    CHECK(!lane.hasReadyCapture());

    FrameCapture* current = publishOne(lane);
    PresentedCapture held = PresentedCapture::adopt(lane.takeReady(0));
    REQUIRE(static_cast<bool>(held));
    CHECK(&held.capture() == current);
}

// The hold is move-only: a move leaves the source empty, and assigning a new
// hold releases the one held.
TEST(aPresentedCaptureReleasesWhatItHeldWhenItTakesTheNext)
{
    FrameLane lane;
    REQUIRE(lane.arm());
    FrameCapture* first = publishOne(lane);
    PresentedCapture held = PresentedCapture::adopt(lane.takeReady());
    CHECK_EQ(referencesOf(*first), 2);
    PresentedCapture moved = std::move(held);
    CHECK(!static_cast<bool>(held));
    CHECK_EQ(referencesOf(*first), 2);

    FrameCapture* second = publishOne(lane);
    moved = PresentedCapture::adopt(lane.takeReady());
    CHECK_EQ(referencesOf(*first), 1);
    CHECK_EQ(referencesOf(*second), 2);
    CHECK(&moved.capture() == second);
}

// Each capture's selection is the one resolved with it; a capture of another
// lane has none here.
TEST(aCapturesSelectionIsTheOneResolvedWithIt)
{
    FrameLane lane;
    FrameLane other;
    REQUIRE(lane.arm());
    REQUIRE(other.arm());
    const std::optional<FrameLane::Target> target = lane.claimTarget();
    REQUIRE(target.has_value());
    target->selection->spanCount = 7;
    lane.publish(*target);
    PresentedCapture held = PresentedCapture::adopt(lane.takeReady());
    const auto* selection = lane.selectionFor(held.capture());
    REQUIRE(selection != nullptr);
    CHECK_EQ(selection->spanCount, 7);
    CHECK(other.selectionFor(held.capture()) == nullptr);
}

UBIQUITTY_TEST_MAIN()
