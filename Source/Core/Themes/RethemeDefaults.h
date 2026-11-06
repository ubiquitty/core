// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Forward.h"

namespace Ubiquitty::Themes {

// Re-derive the default pen colours after the palette changed, and tell a
// subscribed application (DEC mode 2031) so it can re-theme live.
//
/// Behaviour-layer, not model: the repaint is a screen effect but the decision to
/// notify the host is a policy, and only the unsolicited notification is gated on
/// the subscription — a direct DSR 996 query always answers.
void rethemeDefaults(Ubiquitty::Terminal* t);

} // namespace Ubiquitty::Themes
