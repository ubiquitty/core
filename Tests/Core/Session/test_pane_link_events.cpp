// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/PaneLinkEvents.h"
#include "Harness.h"

#include <functional>
#include <memory>
#include <vector>

using namespace Ubiquitty::Session;

namespace {

struct Observer final : PaneLinkObserver {
    std::vector<FrameReady> frames;
    std::vector<LinkState> states;
    unsigned facts = 0;
    std::function<void()> onFrame;

    void frameReady(const FrameReady& frame) override
    {
        frames.push_back(frame);
        if (onFrame)
            onFrame();
    }

    void factsChanged() override { ++facts; }
    void linkStateChanged(const LinkState& state) override { states.push_back(state); }
};

} // namespace

TEST(paneLinkEventsCoalesceBeforeTheOwnerDelivers)
{
    PaneLinkEvents events;
    Observer observer;
    events.observe(observer);
    events.noteFrame({ .generation = 1 });
    events.noteFrame({ .generation = 2 });
    events.noteFacts();
    events.noteFacts();
    CHECK(observer.frames.empty());
    CHECK_EQ(observer.facts, 0u);
    events.deliver();
    REQUIRE(observer.frames.size() == 1);
    CHECK_EQ(observer.frames.front().generation, 2u);
    CHECK_EQ(observer.facts, 1u);
    events.noteFrame({ .generation = 2 });
    events.deliver();
    CHECK_EQ(observer.frames.size(), 1u);
}

TEST(paneLinkEventsDoNotReenterAnObserver)
{
    PaneLinkEvents events;
    Observer observer;
    events.observe(observer);
    unsigned depth = 0;
    observer.onFrame = [&] {
        CHECK_EQ(++depth, 1u);
        if (observer.frames.size() == 1) {
            events.noteFrame({ .generation = 2 });
            events.deliver();
            CHECK_EQ(observer.frames.size(), 1u);
        }
        --depth;
    };
    events.noteFrame({ .generation = 1 });
    events.deliver();
    CHECK_EQ(observer.frames.size(), 1u);
    events.deliver();
    CHECK_EQ(observer.frames.size(), 2u);
}

TEST(paneLinkEventsNotifyOnlyPhaseAndStallEdges)
{
    PaneLinkEvents events;
    Observer observer;
    events.observe(observer);
    LinkState state { .phase = LinkPhase::Live };
    events.noteState(state);
    events.deliver();
    state.transport.lastHeardMicroseconds = 10;
    state.transport.oldestUnreceiptedAgeMicroseconds = 20;
    events.noteState(state);
    events.deliver();
    CHECK_EQ(observer.states.size(), 1u);
    state.transport.isInputUndelivered = true;
    events.noteState(state);
    events.deliver();
    state.transport.isSilent = true;
    events.noteState(state);
    events.deliver();
    state.transport.isSilent = false;
    events.noteState(state);
    events.deliver();
    CHECK_EQ(observer.states.size(), 4u);
    CHECK(observer.states.back().transport.isInputUndelivered);
    state.phase = LinkPhase::Lost;
    events.noteState(state);
    events.deliver();
    CHECK_EQ(observer.states.size(), 5u);
}

TEST(aDestroyedPaneLinkObserverIsNeverCalled)
{
    PaneLinkEvents events;
    {
        Observer observer;
        events.observe(observer);
        events.noteFrame({ .generation = 1 });
    }
    events.deliver();
    Observer replacement;
    events.observe(replacement);
    events.deliver();
    CHECK_EQ(replacement.frames.size(), 1u);
}

TEST(aPaneLinkCallbackMayDestroyItsEventOwner)
{
    auto events = std::make_unique<PaneLinkEvents>();
    Observer observer;
    events->observe(observer);
    events->noteFrame({ .generation = 1 });
    events->noteFacts();
    events->noteState({ .phase = LinkPhase::Live });
    observer.onFrame = [&] { events.reset(); };
    events->deliver();
    CHECK(!events);
    CHECK_EQ(observer.frames.size(), 1u);
    CHECK_EQ(observer.facts, 0u);
    CHECK(observer.states.empty());
}

TEST(replacingAPaneLinkObserverDoesNotSplitOneDelivery)
{
    PaneLinkEvents events;
    Observer original;
    Observer replacement;
    events.observe(original);
    events.noteFrame({ .generation = 1 });
    events.noteFacts();
    events.noteState({ .phase = LinkPhase::Live });
    original.onFrame = [&] { events.observe(replacement); };
    events.deliver();
    CHECK_EQ(original.frames.size(), 1u);
    CHECK_EQ(original.facts, 0u);
    CHECK(original.states.empty());
    CHECK(replacement.frames.empty());
    CHECK_EQ(replacement.facts, 0u);
    CHECK(replacement.states.empty());
    events.noteFrame({ .generation = 1 });
    events.noteState({ .phase = LinkPhase::Live });
    events.deliver();
    CHECK_EQ(replacement.frames.size(), 1u);
    CHECK_EQ(replacement.states.size(), 1u);
}

UBIQUITTY_TEST_MAIN()
