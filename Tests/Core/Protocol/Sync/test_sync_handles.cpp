// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_handles.cpp — pane handles are per connection and reused only
// once the client knows they are free (Docs/sync.md §5).
//
// The properties held:
//
// - A connection that binds and unbinds a pane 100,000 times keeps as many
//   handle slots as it has panes bound at once, so neither its memory nor the
//   cost of a service grows with the count.
// - A handle is bound again only once an `Ack` counts past its `PaneUnbind`:
//   a frame the client wrote for it before it learned is dropped, never taken
//   for the pane bound to it next.

#include "Harness.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

/// A pane of its own: a terminal and the source cut from it.
struct LoosePane {
    LoosePane()
    {
        Ubiquitty::resize(terminal, 4, 20);
        UbiquittyTest::feed(terminal, "$ ");
        source.publish(FrameClass::Structural, 0);
    }

    Ubiquitty::Terminal terminal;
    PaneSource source { terminal, ID128 { 77, 10 } };
};

} // namespace

// Binding and unbinding a pane 100,000 times, with the client reading and
// acknowledging, keeps the connection at three handle slots: the pane the
// fixture bound, and two the rebindings take in turn, one of them waiting for
// the Ack that counts past its PaneUnbind.
TEST(aHundredThousandBindsKeepTheHandlesTheConnectionNeedsAtOnce)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    (void)link.addPane();
    LoosePane loose;
    std::size_t mostSlots = 0;
    for (int bind = 0; bind < 100'000; ++bind) {
        const std::uint32_t handle = link.host().bindPane(loose.source);
        REQUIRE(handle != 0);
        link.host().unbindPane(handle, 0);
        link.step();
        mostSlots = std::max(mostSlots, link.host().handleSlots());
    }
    CHECK_MSG(mostSlots <= 3, ("the connection kept " + std::to_string(mostSlots) + " handle slots").c_str());
    CHECK_EQ(link.client().counters().paneBindings, std::uint64_t { 1 + 2 * 100'000 });
    CHECK(link.host().state() == ConnectionState::Attached);
    CHECK(link.client().state() == ConnectionState::Attached);

    // A pane bound after all that still crosses whole.
    const std::uint32_t handle = link.host().bindPane(loose.source);
    for (int step = 0; step < 8; ++step)
        link.step();
    CHECK(link.client().isPaneCurrent(handle));
}

// The client writes a frame for a pane after the host unbound it and before
// it read the PaneUnbind. Until an Ack counts past that PaneUnbind the handle
// is not bound again, so the frame is dropped as one for an unbound pane;
// once one has, the handle is free.
TEST(aHandleIsBoundAgainOnlyOnceTheClientKnowsItIsFree)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t first = link.addPane();
    link.holdDownstream();
    link.host().unbindPane(first, 0);
    link.step();
    const std::array<std::uint8_t, 1> staleInput { 'x' };
    REQUIRE(link.client().sendRaw(first, staleInput));
    link.step();
    CHECK(link.source(first).inputLedger().lastWritten({ 7, 8 }).value_or(0) == 0);
    CHECK(link.host().state() == ConnectionState::Attached);

    LoosePane loose;
    const std::uint32_t second = link.host().bindPane(loose.source);
    CHECK_MSG(second != first, "a handle the client had not yet seen unbound was bound again");

    link.releaseDownstream();
    for (int step = 0; step < 4; ++step)
        link.step();
    LoosePane later;
    const std::uint32_t third = link.host().bindPane(later.source);
    CHECK_EQ(third, first);
    for (int step = 0; step < 8; ++step)
        link.step();
    CHECK(link.client().isPaneCurrent(third));
    CHECK(link.client().isPaneCurrent(second));
}

UBIQUITTY_TEST_MAIN()
