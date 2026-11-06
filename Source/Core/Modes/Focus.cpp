// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Modes/Focus.h"
#include "Core/Terminal.h"

namespace Ubiquitty::Modes {

void focusChanged(Ubiquitty::Terminal* t, Modes::FocusState focus)
{
    if (!t)
        return;
    t->windowFocused = (focus == Modes::FocusState::Focused);
    if (!t->modes.focusReporting)
        return;
    Ubiquitty::respond(*t, Ubiquitty::Capability::StatusReport, (focus == Modes::FocusState::Focused) ? "\033[I" : "\033[O");
}

bool bellWantsNotification(const Ubiquitty::Terminal* t) { return t && !t->windowFocused; }

} // namespace Ubiquitty::Modes
