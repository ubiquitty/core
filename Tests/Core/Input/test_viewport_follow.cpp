// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_viewport_follow — where a scrolled-back view sits after output lands.
//
// The failure this covers is not a wrong offset from a wrong formula but a
// right formula assembled differently by each frontend from the same Core
// fragments. The cases are written as the policy's own decision table so a
// new arm has something to be checked against rather than something to copy.
//
// The table has three inputs -- a standing selection, whether the view is
// already at the tail, and what the scroll-on-output mode says -- and the third
// has three values, because a mode that could not be read is an answer the
// per-drain path actually gets. Every combination is stated, including the ones
// where an input does not matter: an input that stops mattering silently is how
// two arms drift apart.

#include "Harness.h"

#include "Core/Input/PointerInput.h"

using Ubiquitty::Input::OutputSnapPolicy;
using Ubiquitty::Input::SelectionPresence;
using Ubiquitty::Input::viewportAfterOutput;
using Ubiquitty::Input::ViewportHold;
using Ubiquitty::Input::viewportHoldForOutput;

TEST(viewportFollowPolicy)
{
    // --- A standing selection back in history holds, whatever the mode says.
    //
    // The selected rows scrolling out from under the pointer is the one outcome
    // no scroll-on-output mode asks for, so the mode is not consulted.
    {
        for (const OutputSnapPolicy policy : { OutputSnapPolicy::SnapToTail, OutputSnapPolicy::HoldViewedRows, OutputSnapPolicy::Indeterminate }) {
            CHECK(viewportHoldForOutput(SelectionPresence::Present, 40, policy) == ViewportHold::Hold);
        }
    }

    // --- A view at the tail follows, whatever the mode says.
    //
    // There are no held rows to hold. A mode answering "hold" here would pin
    // the view to a tail that is about to move, which is the live view failing
    // to be live.
    {
        for (const OutputSnapPolicy policy : { OutputSnapPolicy::SnapToTail, OutputSnapPolicy::HoldViewedRows, OutputSnapPolicy::Indeterminate }) {
            CHECK(viewportHoldForOutput(SelectionPresence::Absent, 0, policy) == ViewportHold::Follow);
            CHECK(viewportHoldForOutput(SelectionPresence::Present, 0, policy) == ViewportHold::Follow);
            // A negative offset is a caller's clamp not yet applied, and it is
            // the tail as far as this decision goes.
            CHECK(viewportHoldForOutput(SelectionPresence::Absent, -1, policy) == ViewportHold::Follow);
        }
    }

    // --- Scrolled back with no selection: the mode decides.
    {
        CHECK(viewportHoldForOutput(SelectionPresence::Absent, 40, OutputSnapPolicy::SnapToTail) == ViewportHold::Follow);
        CHECK(viewportHoldForOutput(SelectionPresence::Absent, 40, OutputSnapPolicy::HoldViewedRows) == ViewportHold::Hold);
    }

    // --- An unread mode holds.
    //
    // The per-drain path cannot block on the model lock under flood, so a busy
    // lock leaves it with no verdict. Holding keeps a reader's place and
    // re-asks next drain; following would yank the viewport to the tail for a
    // lock that happened to be busy -- and under flood it is busy on exactly
    // the drains a reader is there to notice.
    {
        CHECK(viewportHoldForOutput(SelectionPresence::Absent, 40, OutputSnapPolicy::Indeterminate) == ViewportHold::Hold);
    }

    // --- Which drains consult the mode.
    //
    // Callers read the mode from behind a lock, so they ask this first and skip
    // the read on the drains where the answer cannot change anything. That skip
    // is only safe while the predicate agrees with the decision, so the two are
    // checked against each other: on every input where the mode is not
    // consulted, the two mode values that differ must produce the same verdict.
    {
        CHECK(!Ubiquitty::Input::isSnapPolicyConsulted(SelectionPresence::Present, 40));
        CHECK(!Ubiquitty::Input::isSnapPolicyConsulted(SelectionPresence::Present, 0));
        CHECK(!Ubiquitty::Input::isSnapPolicyConsulted(SelectionPresence::Absent, 0));
        CHECK(!Ubiquitty::Input::isSnapPolicyConsulted(SelectionPresence::Absent, -1));
        CHECK(Ubiquitty::Input::isSnapPolicyConsulted(SelectionPresence::Absent, 1));
        CHECK(Ubiquitty::Input::isSnapPolicyConsulted(SelectionPresence::Absent, 40));

        for (const SelectionPresence selection : { SelectionPresence::Absent, SelectionPresence::Present }) {
            for (const std::ptrdiff_t viewOffset : { -1, 0, 1, 40 }) {
                if (Ubiquitty::Input::isSnapPolicyConsulted(selection, viewOffset))
                    continue;
                CHECK(viewportHoldForOutput(selection, viewOffset, OutputSnapPolicy::SnapToTail)
                    == viewportHoldForOutput(selection, viewOffset, OutputSnapPolicy::HoldViewedRows));
            }
        }
    }

    // --- The offset a hold produces: compensated by the tail's growth.
    //
    // Ten rows of history arrived beneath a view sitting forty rows up, so it
    // sits fifty rows up and the reader's rows have not moved.
    {
        const auto held = viewportAfterOutput(SelectionPresence::Absent, 40, OutputSnapPolicy::HoldViewedRows, 110, 10);
        CHECK(held.hold == ViewportHold::Hold);
        CHECK(held.viewOffset == 50);
    }

    // --- The offset a follow produces: the tail.
    {
        const auto followed = viewportAfterOutput(SelectionPresence::Absent, 40, OutputSnapPolicy::SnapToTail, 110, 10);
        CHECK(followed.hold == ViewportHold::Follow);
        CHECK(followed.viewOffset == 0);
    }

    // --- A hold cannot compensate past the oldest retained row.
    //
    // The offset is rows above the tail, so it is bounded by how many rows are
    // retained; growth beyond that has nothing left to hold onto.
    {
        const auto clamped = viewportAfterOutput(SelectionPresence::Absent, 95, OutputSnapPolicy::HoldViewedRows, 100, 90);
        CHECK(clamped.hold == ViewportHold::Hold);
        CHECK(clamped.viewOffset == 100);
    }

    // --- A tail that did not grow leaves a held view where it was.
    //
    // Output that scrolled nothing into history -- a redraw in place -- moves
    // no reader's rows, so the compensation is zero rather than a no-op that
    // happens to look like one.
    {
        const auto still = viewportAfterOutput(SelectionPresence::Absent, 40, OutputSnapPolicy::HoldViewedRows, 100, 0);
        CHECK(still.viewOffset == 40);
    }

    // --- A full history moves the content without moving the count.
    //
    // At the limit every appended row evicts one, so the retained count
    // stands still while the reader's rows shift a row per line. The hold is
    // measured by the rows appended for exactly this case: measured by a
    // before-and-after of the retained count it would read zero growth here
    // and let the view drift toward the tail one row per line of output.
    {
        const auto full = viewportAfterOutput(SelectionPresence::Absent, 40, OutputSnapPolicy::HoldViewedRows, 100, 10);
        CHECK(full.viewOffset == 50);
    }
}

UBIQUITTY_TEST_MAIN()
