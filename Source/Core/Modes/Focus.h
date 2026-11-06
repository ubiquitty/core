// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/Modes/TerminalModes.h"
#include "Forward.h"

// Window focus, and DEC private mode 1004 focus reporting.
//
// The direction matters: the embedder calls in to say focus changed, and the
// terminal answers the host with CSI I / CSI O when mode 1004 is set. That is why
// this is focusChanged rather than onFocusChanged — on* names in this codebase
// are the observer hooks Core calls out to.

namespace Ubiquitty::Modes {

/// Records a focus change and emits the report if the app asked for one.
///
/// - Parameters:
///   - t: The terminal whose focus changed.
///   - focus: The new state.
void focusChanged(Ubiquitty::Terminal* t, Modes::FocusState focus);

/// Whether a bell should raise a user-visible notification: true only when the
/// window is unfocused, since a focused user already saw whatever rang it.
[[nodiscard]] bool bellWantsNotification(const Ubiquitty::Terminal* t);

} // namespace Ubiquitty::Modes
