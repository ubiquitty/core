// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_pane_presentation — which window facts stop a pane costing anything.
//
// Two folds over the same facts, and the difference between them is the whole
// point: a pane keeps capturing while it is merely covered, because the frames
// it would otherwise skip are the ones a reader sees the instant they bring it
// forward — but it does not pay the panel's rate for pixels nobody can see.
// Both shells report the facts and neither decides, so this is the only place
// the two questions are answered.

#include "Core/Runtime/TerminalRuntime.h"

using Ubiquitty::Runtime::isPaneRevealUnconfirmed;
using Ubiquitty::Runtime::MachineBudget;
using Ubiquitty::Runtime::machineThrottle;
using Ubiquitty::Runtime::MachineThrottle;
using Ubiquitty::Runtime::notePaneRevealEdge;
using Ubiquitty::Runtime::paneFrameDelivery;
using Ubiquitty::Runtime::PaneFrameDelivery;
using Ubiquitty::Runtime::paneFullRatePresentation;
using Ubiquitty::Runtime::PanePresentation;
using Ubiquitty::Runtime::panePresentation;
using Ubiquitty::Runtime::PaneReveal;
using Ubiquitty::Runtime::PaneVisibility;
using Ubiquitty::Runtime::revealConfirmationBudgetTicks;
using Ubiquitty::Runtime::ThermalPressure;

namespace {

/// A pane in a plain front window: every fact in the earning direction.
PaneVisibility inFront()
{
    PaneVisibility where;
    where.hasWindow = true;
    return where;
}

} // namespace

TEST(aFrontPanePresentsAndEarnsTheRate)
{
    CHECK(panePresentation(inFront()) == PanePresentation::Presented);
    CHECK(paneFullRatePresentation(inFront()) == PanePresentation::Presented);
}

TEST(threeFactsStopAPaneCapturing)
{
    PaneVisibility detached;
    CHECK_MSG(panePresentation(detached) == PanePresentation::Hidden, "a view in no window presents nothing");

    PaneVisibility miniaturized = inFront();
    miniaturized.isMiniaturized = true;
    CHECK_MSG(panePresentation(miniaturized) == PanePresentation::Hidden, "a miniaturized window is not presented");

    PaneVisibility behind = inFront();
    behind.isBackgroundedTab = true;
    CHECK_MSG(panePresentation(behind) == PanePresentation::Hidden, "a tab behind the front one is not presented");

    PaneVisibility hidden = inFront();
    hidden.isHiddenInWindow = true;
    CHECK_MSG(panePresentation(hidden) == PanePresentation::Hidden, "a hidden view is not presented");
}

TEST(occlusionCostsTheRateAndNotTheCapture)
{
    PaneVisibility covered = inFront();
    covered.isFullyOccluded = true;
    // The pair that decides this entry: a covered pane goes on capturing, so
    // bringing it forward shows what happened while it was behind, and it
    // stops paying the panel rate for pixels nobody can see.
    CHECK_MSG(
        panePresentation(covered) == PanePresentation::Presented, "a covered pane keeps its render state current");
    CHECK_MSG(paneFullRatePresentation(covered) == PanePresentation::Hidden, "and stops earning the panel's rate");

    // Whatever else is true, occlusion cannot put a pane back on the rate.
    PaneVisibility coveredAndBehind = covered;
    coveredAndBehind.isBackgroundedTab = true;
    CHECK(paneFullRatePresentation(coveredAndBehind) == PanePresentation::Hidden);
}

TEST(theMachineAsksEveryPaneToCostLess)
{
    MachineBudget easy;
    CHECK(machineThrottle(easy) == MachineThrottle::FullRate);

    MachineBudget lowPower;
    lowPower.isInLowPowerMode = true;
    CHECK_MSG(machineThrottle(lowPower) == MachineThrottle::Throttled, "the reader's own request throttles");

    // Fair is below the threshold on purpose: the system is not shedding work
    // there either, and a terminal that gave up responsiveness first would be
    // throttling for nobody.
    MachineBudget fair;
    fair.thermal = ThermalPressure::Fair;
    CHECK_MSG(machineThrottle(fair) == MachineThrottle::FullRate, "fair pressure is below the threshold");

    for (const ThermalPressure step : { ThermalPressure::Serious, ThermalPressure::Critical }) {
        MachineBudget pressed;
        pressed.thermal = step;
        CHECK_MSG(machineThrottle(pressed) == MachineThrottle::Throttled, "serious and above throttle");
    }
}

TEST(aCoveredPaneWithholdsItsFrame)
{
    PaneVisibility covered = inFront();
    covered.isFullyOccluded = true;
    CHECK_MSG(paneFrameDelivery(covered) == PaneFrameDelivery::Withhold,
        "nothing of the window is on screen, so a frame drawn now reaches a drawable nobody composites");
    CHECK_MSG(paneFrameDelivery(inFront()) == PaneFrameDelivery::Draw, "and an uncovered pane draws");
}

TEST(anUnconfirmedRevealDrawsBeforeTheWindowSystemAnswers)
{
    // The hazard the fact exists for: the report lags a reveal by a frame or
    // more, so a pane that waited for it would show the layer it stopped at —
    // after a long cover, content minutes old — for every tab switch and every
    // application activation.
    PaneVisibility revealed = inFront();
    revealed.isFullyOccluded = true;
    revealed.isRevealUnconfirmed = true;
    CHECK(paneFrameDelivery(revealed) == PaneFrameDelivery::Draw);
}

TEST(aBackgroundedCoveredPaneIgnoresTheReveal)
{
    // An application-wide edge says nothing about a pane the capture fold
    // already calls hidden: a tab behind its group's front one, a miniaturized
    // window and a hidden view each have a reveal edge of their own.
    for (const auto set : { &PaneVisibility::isBackgroundedTab, &PaneVisibility::isMiniaturized, &PaneVisibility::isHiddenInWindow }) {
        PaneVisibility hidden = inFront();
        hidden.isFullyOccluded = true;
        hidden.isRevealUnconfirmed = true;
        hidden.*set = true;
        CHECK_MSG(paneFrameDelivery(hidden) == PaneFrameDelivery::Withhold, "a hidden pane withholds whatever the reveal says");
        CHECK_MSG(notePaneRevealEdge(PaneReveal {}, hidden, 100).isPending == false, "and never arms one");
    }
}

TEST(aPaneInNoWindowIsNeverWithheld)
{
    // Withholding is about a window the reader cannot see. A detached view has
    // no window to be covered, and its frames are the host's business.
    PaneVisibility detached;
    detached.isFullyOccluded = true;
    CHECK(paneFrameDelivery(detached) == PaneFrameDelivery::Draw);
}

TEST(withholdingReadsOcclusionAlone)
{
    // Everything but occlusion leaves delivery alone: a backgrounded tab that
    // is not reported covered still draws, which is what keeps its cursor
    // painted solid while it stands behind the front one.
    PaneVisibility behind = inFront();
    behind.isBackgroundedTab = true;
    CHECK(paneFrameDelivery(behind) == PaneFrameDelivery::Draw);
    PaneVisibility miniaturized = inFront();
    miniaturized.isMiniaturized = true;
    CHECK(paneFrameDelivery(miniaturized) == PaneFrameDelivery::Draw);
}

TEST(anUnconfirmedRevealEarnsTheRateBack)
{
    PaneVisibility revealed = inFront();
    revealed.isFullyOccluded = true;
    CHECK_MSG(paneFullRatePresentation(revealed) == PanePresentation::Hidden, "covered, the pane pays no panel rate");
    revealed.isRevealUnconfirmed = true;
    CHECK_MSG(paneFullRatePresentation(revealed) == PanePresentation::Presented,
        "and takes it back for the reveal, so the link drives the frames the edge asked for");
}

TEST(theRevealFactLeavesCaptureAlone)
{
    // Whether a pane captures is independent of occlusion and of the reveal:
    // a covered pane keeps its render state current either way.
    PaneVisibility covered = inFront();
    covered.isFullyOccluded = true;
    CHECK(panePresentation(covered) == PanePresentation::Presented);
    covered.isRevealUnconfirmed = true;
    CHECK(panePresentation(covered) == PanePresentation::Presented);
    PaneVisibility behind = covered;
    behind.isBackgroundedTab = true;
    CHECK(panePresentation(behind) == PanePresentation::Hidden);
}

TEST(theDeliveryFoldMatchesItsTable)
{
    // Every combination of the six facts, against the rule spelled out rather
    // than against the fold: a table that re-implemented the fold would agree
    // with it however either one changed.
    for (int bits = 0; bits < 64; ++bits) {
        PaneVisibility where;
        where.hasWindow = (bits & 1) != 0;
        where.isMiniaturized = (bits & 2) != 0;
        where.isBackgroundedTab = (bits & 4) != 0;
        where.isHiddenInWindow = (bits & 8) != 0;
        where.isFullyOccluded = (bits & 16) != 0;
        where.isRevealUnconfirmed = (bits & 32) != 0;
        const bool isCaptured = where.hasWindow && !where.isMiniaturized && !where.isBackgroundedTab && !where.isHiddenInWindow;
        const bool shouldDraw = !where.hasWindow || !where.isFullyOccluded || (where.isRevealUnconfirmed && isCaptured);
        CHECK_MSG(paneFrameDelivery(where) == (shouldDraw ? PaneFrameDelivery::Draw : PaneFrameDelivery::Withhold),
            "the fold answers the table for every combination of facts");
        const bool isRateEarned = shouldDraw && isCaptured;
        CHECK(paneFullRatePresentation(where) == (isRateEarned ? PanePresentation::Presented : PanePresentation::Hidden));
    }
}

TEST(aRevealEdgeArmsOnlyForACoveredPresentedPane)
{
    // A pane that already draws needs no reveal: arming one for it would let a
    // later cover draw for a second on an edge that had nothing to do with it.
    CHECK(notePaneRevealEdge(PaneReveal {}, inFront(), 10).isPending == false);

    PaneVisibility covered = inFront();
    covered.isFullyOccluded = true;
    const PaneReveal armed = notePaneRevealEdge(PaneReveal {}, covered, 10);
    CHECK(armed.isPending);
    CHECK(armed.deadlineTicks == 10 + revealConfirmationBudgetTicks);

    PaneVisibility detached;
    detached.isFullyOccluded = true;
    CHECK(notePaneRevealEdge(PaneReveal {}, detached, 10).isPending == false);
}

TEST(aRevealStandsUntilItsBudgetRunsOut)
{
    PaneVisibility covered = inFront();
    covered.isFullyOccluded = true;
    const PaneReveal armed = notePaneRevealEdge(PaneReveal {}, covered, 100);
    CHECK(isPaneRevealUnconfirmed(armed, 100));
    CHECK(isPaneRevealUnconfirmed(armed, 100 + revealConfirmationBudgetTicks - 1));
    CHECK_MSG(!isPaneRevealUnconfirmed(armed, 100 + revealConfirmationBudgetTicks), "the budget is spent at its deadline");
    CHECK_MSG(!isPaneRevealUnconfirmed(PaneReveal {}, 100), "and an edge nobody took stands not at all");

    // The tick counter is cyclic — it is a 60 Hz count truncated to 32 bits —
    // so a reveal armed just before it wraps must still stand after it does.
    const std::uint32_t nearWrap = 0xFFFFFFFFu - 10;
    const PaneReveal wrapping = notePaneRevealEdge(PaneReveal {}, covered, nearWrap);
    CHECK(isPaneRevealUnconfirmed(wrapping, nearWrap));
    CHECK_MSG(isPaneRevealUnconfirmed(wrapping, nearWrap + 20), "a reveal survives the counter wrapping under it");
    CHECK(!isPaneRevealUnconfirmed(wrapping, nearWrap + revealConfirmationBudgetTicks));
}

UBIQUITTY_TEST_MAIN()

TEST(processObservationRequiresADisplayedComponentOrInspector)
{
    using namespace Ubiquitty;
    Preferences::Values configuration;
    configuration.titleWindowProcess = false;
    configuration.titleWindowShellCommand = false;
    configuration.titleTabProcess = false;
    configuration.titleTabShellCommand = false;
    configuration.titlePaneProcess = false;
    configuration.titlePaneShellCommand = false;
    Runtime::ProcessObservation observation;
    observation.isGroupVisible = true;
    observation.isWindowTitleVisible = true;
    observation.isTabTitleVisible = true;
    observation.isPaneTitleVisible = true;
    CHECK(!Runtime::needsProcessObservation(observation, configuration));
    configuration.titleTabProcess = true;
    observation.isWindowTitleVisible = false;
    observation.isPaneTitleVisible = false;
    CHECK(Runtime::needsProcessObservation(observation, configuration));
    observation.isTabTitleReplaced = true;
    CHECK(!Runtime::needsProcessObservation(observation, configuration));
    observation.isTabTitleReplaced = false;
    observation.isGroupVisible = false;
    CHECK(!Runtime::needsProcessObservation(observation, configuration));
    observation.isInspectorOpen = true;
    CHECK(Runtime::needsProcessObservation(observation, configuration));
    observation.isInspectorOpen = false;
    observation.isGroupVisible = true;
    observation.isTabTitleVisible = false;
    CHECK(!Runtime::needsProcessObservation(observation, configuration));
    configuration.titlePaneShellCommand = true;
    observation.isPaneTitleVisible = true;
    CHECK(Runtime::needsProcessObservation(observation, configuration));
    observation.isPaneTitleReplaced = true;
    CHECK(!Runtime::needsProcessObservation(observation, configuration));
}

// The close button's running-job marker answers from the process sample, so a
// window showing it needs samples even when no title names a process.
TEST(aVisibleCloseMarkerObservesTheProcessesItReads)
{
    using namespace Ubiquitty;
    Preferences::Values configuration;
    configuration.titleWindowProcess = false;
    configuration.titleWindowShellCommand = false;
    configuration.titleTabProcess = false;
    configuration.titleTabShellCommand = false;
    configuration.titlePaneProcess = false;
    configuration.titlePaneShellCommand = false;
    Runtime::ProcessObservation observation;
    observation.isGroupVisible = true;
    observation.isWindowTitleVisible = true;
    CHECK(!Runtime::needsProcessObservation(observation, configuration));
    observation.isCloseMarkerVisible = true;
    CHECK_MSG(Runtime::needsProcessObservation(observation, configuration), "the default policy marks running jobs");
    configuration.closeConfirmation = "never";
    CHECK_MSG(!Runtime::needsProcessObservation(observation, configuration), "a policy that never marks reads no sample");
    configuration.closeConfirmation = "always";
    CHECK_MSG(!Runtime::needsProcessObservation(observation, configuration), "a policy that always marks reads no sample");
    configuration.closeConfirmation = "running";
    observation.isGroupVisible = false;
    CHECK_MSG(!Runtime::needsProcessObservation(observation, configuration), "a marker nobody can see waits for its reveal");
}
