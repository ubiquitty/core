// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Compiler.h"
#include "Base/SwiftInterop.h"

#include <cstdint>

namespace Ubiquitty::PAL {

/// An OSC 22 mouse pointer shape, kitty's "Pointer shapes".
///
/// The CSS/X11 cursor name on the wire maps to one of these; the frontend
/// resolves each to its native cursor. Unknown names fall back to `Default`.
/// The wire is the name string, not a number.
///
/// In a header of its own because the published snapshot carries the live
/// shape and the snapshot's header is portable: it must not drag the client
/// seams in behind one enum. Closed and Swift-named because the shape crosses
/// into the shared AppKit mapping: an exhaustive Swift switch is what makes a
/// shape added here a compile error there rather than a silently arrow-shaped
/// pointer.
enum class UBIQUITTY_CLOSED_ENUM MousePointerShape : uint8_t {
    Default UBIQUITTY_SWIFT_NAME(default),
    Pointer UBIQUITTY_SWIFT_NAME(pointer), // pointingHand — clickable widget
    Text UBIQUITTY_SWIFT_NAME(text), // I-beam — text field
    VerticalText UBIQUITTY_SWIFT_NAME(verticalText),
    Crosshair UBIQUITTY_SWIFT_NAME(crosshair),
    Cell UBIQUITTY_SWIFT_NAME(cell),
    Move UBIQUITTY_SWIFT_NAME(move),
    Grab UBIQUITTY_SWIFT_NAME(grab),
    Grabbing UBIQUITTY_SWIFT_NAME(grabbing),
    NoDrop UBIQUITTY_SWIFT_NAME(noDrop),
    NotAllowed UBIQUITTY_SWIFT_NAME(notAllowed),
    Alias UBIQUITTY_SWIFT_NAME(alias),
    Copy UBIQUITTY_SWIFT_NAME(copy),
    Help UBIQUITTY_SWIFT_NAME(help),
    Progress UBIQUITTY_SWIFT_NAME(progress),
    Wait UBIQUITTY_SWIFT_NAME(wait),
    ZoomIn UBIQUITTY_SWIFT_NAME(zoomIn),
    ZoomOut UBIQUITTY_SWIFT_NAME(zoomOut),
    ColResize UBIQUITTY_SWIFT_NAME(colResize), // ew-resize / col-resize
    RowResize UBIQUITTY_SWIFT_NAME(rowResize), // ns-resize / row-resize
    NWSEResize UBIQUITTY_SWIFT_NAME(nwseResize), // nwse-resize
    NESWResize UBIQUITTY_SWIFT_NAME(neswResize), // nesw-resize
    NorthResize UBIQUITTY_SWIFT_NAME(northResize),
    NortheastResize UBIQUITTY_SWIFT_NAME(northeastResize),
    EastResize UBIQUITTY_SWIFT_NAME(eastResize),
    SoutheastResize UBIQUITTY_SWIFT_NAME(southeastResize),
    SouthResize UBIQUITTY_SWIFT_NAME(southResize),
    SouthwestResize UBIQUITTY_SWIFT_NAME(southwestResize),
    WestResize UBIQUITTY_SWIFT_NAME(westResize),
    NorthwestResize UBIQUITTY_SWIFT_NAME(northwestResize),
};

} // namespace Ubiquitty::PAL

namespace Ubiquitty {

/// The shape under its umbrella name.
///
/// Every Core and frontend site spells it this way: the folder names the
/// module, and the name stays where its readers are.
using PAL::MousePointerShape;

} // namespace Ubiquitty
