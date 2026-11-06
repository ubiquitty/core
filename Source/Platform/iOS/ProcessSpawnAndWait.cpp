// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// ProcessSpawnAndWait.cpp — PAL::processSpawnAndWait on iOS.
//
// Its one caller compiles a terminfo entry by running `tic`. There is no `tic`
// here and no way to run one, so the capability reports itself absent — the same
// answer Spawn.cpp and PTYStream.cpp beside it give, for the same reason.
//
// The point of this file is what the BINARY then contains. With the definition
// sharing a translation unit with the rest of the system PAL, the iOS arm linked
// a posix_spawn reference for a function nothing in its link ever calls: the
// policy held by argument rather than by inspection. Split and swapped, it holds
// by inspection — nm over the shipped binary finds no spawn primitive at all.

#include "PAL/PAL.h"

namespace Ubiquitty::PAL {

int processSpawnAndWait(const char* /*path*/, const char* const /*argv*/[])
{
    // The failure code every caller already handles: no child ran.
    return -1;
}

} // namespace Ubiquitty::PAL
