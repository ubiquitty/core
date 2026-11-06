// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Diagnostics/DiagLog.h"
#include "Core/Terminal.h"

namespace Ubiquitty::Diagnostics {

// DEBUG-only diagnostic event sink; a no-op in release so the hot paths carry
// no per-byte hook check.
void diagLog(Ubiquitty::Terminal& t, const char* tag, uint32_t detail)
{
#ifndef NDEBUG
    if (auto* c = t.diagnosticsClient.get())
        c->log(tag, detail);
#else
    (void)t;
    (void)tag;
    (void)detail;
#endif
}

} // namespace Ubiquitty::Diagnostics
