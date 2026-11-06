// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/MousePointerShape.h"

namespace Ubiquitty {

struct Terminal;

namespace OSC {

/// Whether an application has asked for a pointer shape at all.
///
/// Separate from the shape because an empty stack and a stack holding
/// `Default` are different asks: a program that pushed "default" wants the
/// arrow, and a program that pushed nothing wants whatever the terminal shows
/// on its own. One bool answers that; the shape alone cannot.
///
/// - Parameter terminal: The terminal whose OSC 22 stack is read.
/// - Returns: Whether the stack holds anything.
[[nodiscard]] bool isAppPointerShapeSet(const Terminal& terminal);

/// The shape an application asked for, or `Default` when it asked for none.
///
/// The OSC 22 stack's top. One reading, because three callers want it: the
/// dispatcher reports it to a `WindowClient` and answers the query with it,
/// the pointer ladder ranks it against a hovered link, and the runtime
/// publishes it in the snapshot a frontend reads off the model lock. Here
/// rather than beside the ladder because the dispatcher is the model layer and
/// may not include the ladder's.
///
/// - Parameter terminal: The terminal whose OSC 22 stack is read.
/// - Returns: The live shape, or `Default` for an empty stack.
[[nodiscard]] MousePointerShape appRequestedPointerShape(const Terminal& terminal);

/// Publishes the active screen's pointer shape to the frontend.
///
/// - Parameter terminal: The terminal whose active pointer stack is read.
void notifyMousePointerShape(Terminal& terminal);

/// The CSS name OSC 22's query answers with for a shape.
///
/// Every alias a program may send resolves to one shape, so this is the
/// canonical name of each: `right_side` reads back as `e-resize`.
///
/// - Parameter shape: The shape to name.
/// - Returns: A static NUL-terminated name.
[[nodiscard]] const char* pointerShapeWireName(MousePointerShape shape);

} // namespace OSC
} // namespace Ubiquitty
