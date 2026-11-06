// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// BellMode — the one Core-owned audible/visible bell-preference enum, shared by Terminal::bellMode,
// DrainOutcome::bell, resolveBellEvent, the PAL (re-exported via `using`), and the preferences record.
#include "Base/Types.h"

namespace Ubiquitty {

enum class BellMode : uint8_t {
    Off = 0, // no cue
    Audio = 1, // audible beep only
    Visual = 2, // inverse-color flash only
    Both = 3, // beep + flash
};

/// Everything one bell asks a frontend to do.
///
/// Resolved on the drain that rang it — the mode by `resolveBellEvent`, which
/// folds the pending flag, the preference and the coalescer, and the three
/// escalations by the terminal's own modes — and carried whole. A frontend
/// handed the mode alone re-derives the rest or drops it, and dropping it is
/// how a second ingest path came to ring bells that raised no urgency and
/// notified nobody.
struct BellCue {
    /// The cue to execute.
    BellMode mode = BellMode::Off;
    /// DECSET 1043: the window asks to be brought forward.
    bool shouldPopWindow = false;
    /// DECSET 1042: the window asks to be marked urgent.
    bool shouldRaiseUrgency = false;
    /// Whether the bell rang where nobody was looking, so it is worth a
    /// desktop notification. Core decides; the frontend posts.
    bool shouldNotify = false;
};

} // namespace Ubiquitty
