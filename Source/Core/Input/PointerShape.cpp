// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Input/PointerShape.h"

#include "Core/Mouse/Mouse.h"
#include "Core/OSC/MousePointerShape.h"
#include "Core/Runtime/RuntimeLink.h"
#include "Core/Tek/Tek4014.h"
#include "Core/Terminal.h"

namespace Ubiquitty::Input {

namespace {

// The terminal's side of the decision, gathered once from whichever reading the
// caller holds: the live model under its lock, or the snapshot it published.
// The ladder reads only these, so the two readings cannot rank differently.
struct TerminalPointerFacts {
    bool isAppShapeSet = false;
    MousePointerShape appShape = MousePointerShape::Default;
    bool isTekModeActive = false;
    bool isTekGraphicInputActive = false;
    bool isTrackingPointer = false;
};

TerminalPointerFacts factsOf(const Terminal& t)
{
    TerminalPointerFacts facts;
    facts.isAppShapeSet = OSC::isAppPointerShapeSet(t);
    facts.appShape = OSC::appRequestedPointerShape(t);
    facts.isTekModeActive = t.isTekModeActive;
    facts.isTekGraphicInputActive = tekGINActive(t);
    facts.isTrackingPointer = Mouse::tracksPointer(&t);
    return facts;
}

// The snapshot's fields are the same readings, taken by the runtime under the
// lock: `mouseReportsActive` is `Mouse::tracksPointer` published.
TerminalPointerFacts factsOf(const Runtime::RuntimeSnapshot& snapshot)
{
    TerminalPointerFacts facts;
    facts.isAppShapeSet = snapshot.isAppPointerShapeSet;
    facts.appShape = snapshot.appRequestedPointerShape;
    facts.isTekModeActive = snapshot.isTekModeActive;
    facts.isTekGraphicInputActive = snapshot.isTekGraphicInputActive;
    facts.isTrackingPointer = snapshot.mouseReportsActive;
    return facts;
}

MousePointerShape rank(const TerminalPointerFacts& facts, const PointerContext& context)
{
    // Locked: the pointer is hidden and dissociated, so nothing is shown and
    // resolving a shape would be wasted work.
    if (context.isPointerLocked)
        return MousePointerShape::Default;

    // A link outranks an app-requested shape so an OSC 8 hyperlink stays
    // clickable under a program that asked for something else. The SET bit
    // rather than the shape decides: an explicit "default" is an ask for the
    // arrow, and falls through to the affordances below only when nothing was
    // asked.
    if (facts.isAppShapeSet && !context.isOverLink)
        return facts.appShape;
    if (context.isOverLink)
        return MousePointerShape::Pointer;

    // Tektronix is a vector surface with no text cells, so never an I-beam. A
    // live graphic-input crosshair outranks the plain arrow.
    if (facts.isTekModeActive)
        return facts.isTekGraphicInputActive ? MousePointerShape::Crosshair : MousePointerShape::Default;

    // A chromeless window drag outranks the rectangular-select crosshair: the
    // drag wins there regardless of what else is armed.
    if (context.isWindowDragArmed)
        return MousePointerShape::Grab;

    // Rectangular select, shown over a tracking application only when a drag is
    // allowed to bypass reporting — otherwise the application owns the pointer.
    if (context.isRectangularSelectArmed && (!facts.isTrackingPointer || context.selectionBypassesMouseReporting))
        return MousePointerShape::Crosshair;

    // A tracking application owns the clicks, so advertising selectable text
    // would mislead — unless the reach-through modifier is held with its
    // override on, in which case a drag from here does select, and the I-beam
    // says so before the press. A link took the hand above; the trailing void
    // below keeps its arrow, since there is nothing there to reach for.
    if (facts.isTrackingPointer && !context.selectionBypassesMouseReporting)
        return MousePointerShape::Default;

    // Past the end of the row there is nothing to select — unless a drag is
    // already running, in which case the pointer is driving that selection
    // rather than advertising where text is, and swapping it for the arrow
    // mid-gesture reads as the drag having been dropped. The affordance comes
    // back on the first free move after the release.
    if (context.isOverTextVoid && !context.isSelectionDragInFlight)
        return MousePointerShape::Default;

    return MousePointerShape::Text;
}

} // namespace

MousePointerShape pointerShape(const Terminal* t, const PointerContext& context)
{
    // No terminal is a frontend with nothing tracking and nothing asked for,
    // so only the context ranks: the facts default to that.
    return rank(t ? factsOf(*t) : TerminalPointerFacts {}, context);
}

MousePointerShape pointerShape(const Runtime::RuntimeSnapshot& snapshot, const PointerContext& context) { return rank(factsOf(snapshot), context); }

} // namespace Ubiquitty::Input
