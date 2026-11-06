// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Forward.h"

// Debug-only diagnostic event sink.
//
// A no-op in release, so the hot paths carry no per-byte hook check: the tag and
// detail arguments are discarded and the call folds away. Callers may therefore
// log freely from the scroll and parse lanes without a release cost.

namespace Ubiquitty::Diagnostics {

/// Records one diagnostic event against a terminal's client, if it has one.
///
/// - Parameters:
///   - t: The terminal the event concerns.
///   - tag: Short identifier for the event.
///   - detail: Event-specific payload.
void diagLog(Ubiquitty::Terminal& t, const char* tag, uint32_t detail);

} // namespace Ubiquitty::Diagnostics
