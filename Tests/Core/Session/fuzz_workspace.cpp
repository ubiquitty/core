// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// fuzz_workspace.cpp — libFuzzer harness for the untrusted Workspace decoder
// (Source/Core/Session/Workspace.{h,cpp}).
//
// A restored .workspace file is arbitrary, potentially hostile bytes (corrupt,
// hand-edited, or attacker-supplied). This drives fuzzer input through
// deserializeWorkspace and asserts it never reads OOB / over-allocates / crashes
// (caught by -fsanitize=fuzzer,address): it must cleanly reject (return false) or
// yield a structurally-valid result. On accept, the result must round-trip —
// re-serialize → re-deserialize → equal — so the decoder yields only well-formed
// workspaces. Build via Scripts/run-tests.sh --with-fuzz.

#include "Core/Session/Workspace.h"

#include <cassert>
#include <cstdint>
#include <stddef.h>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    using namespace Ubiquitty::Session;
    Workspace workspace;
    if (!deserializeWorkspace({ data, size }, workspace))
        return 0; // cleanly rejected — the only requirement is "no crash / no OOB"

    // Accepted: it must be a well-formed workspace that round-trips exactly.
    std::vector<std::uint8_t> reencoded;
    serializeWorkspace(workspace, reencoded);
    Workspace again;
    const bool ok = deserializeWorkspace(reencoded, again);
    assert(ok && "a decoded workspace must re-decode");
    assert(equalWorkspaces(workspace, again) && "decode→encode→decode must be identity");
    (void)ok;
    return 0;
}
