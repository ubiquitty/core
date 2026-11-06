// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Forward.h"

// What the host is allowed to do to the terminal.
//
// Seven independent gates over the escape sequences that reach OUTSIDE the grid —
// retitling the window, changing the font, reading colours back, moving the
// window, answering capability probes, taking over the mouse, and injecting paste
// controls. Each is a policy the embedder sets; the handlers consult them.
//
// One surface rather than seven scattered setters: they share a type, a default,
// and a threat model, so a reader checking "what can a hostile host do here"
// should find them together.

namespace Ubiquitty {

/// Denied/Allowed rather than a bool, so a call site reads as a policy rather than
/// as a flag. The first enumerator equals `false` and the second `true`, so a
/// static_cast to and from bool preserves the wire value.
enum class OperationPolicy : uint8_t {
    Denied,
    Allowed
};

namespace Modes::HostPolicy {

void setTitlePolicy(Ubiquitty::Terminal* t, OperationPolicy policy);
void setFontPolicy(Ubiquitty::Terminal* t, OperationPolicy policy);
void setColorPolicy(Ubiquitty::Terminal* t, OperationPolicy policy);
void setWindowPolicy(Ubiquitty::Terminal* t, OperationPolicy policy);
void setCapabilityQueryPolicy(Ubiquitty::Terminal* t, OperationPolicy policy);
void setMousePolicy(Ubiquitty::Terminal* t, OperationPolicy policy);
void setPasteControlsPolicy(Ubiquitty::Terminal* t, OperationPolicy policy);

} // namespace Modes::HostPolicy

} // namespace Ubiquitty
