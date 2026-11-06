// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Forward.h"

// Parser state and the buffer resets that abandon a partial sequence.
//
// Every reset here drops an in-flight accumulation without emitting it: a DCS cut
// short by a hard reset must not later flush as if it had terminated, and a sixel
// or kitty payload interrupted mid-transfer must not be decoded from a truncated
// buffer. Each resets its own buffer only, so a caller can abandon one lane
// without disturbing the others.

namespace Ubiquitty::VT {

/// True when no sequence is in flight, so a caller may safely inject bytes of its
/// own without splicing them into a partial one.
[[nodiscard]] bool isParserAtGround(const Ubiquitty::Terminal* t);

void resetParseState(Ubiquitty::Terminal* t);
void resetOSCBuffer(Ubiquitty::Terminal* t);
void resetKittyBuffers(Ubiquitty::Terminal* t);
void resetSixelAccumulator(Ubiquitty::Terminal* t);

} // namespace Ubiquitty::VT
