// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The host an Apple UI-test launch (`--seed-host-sessions` on the phone)
// draws in place of one it dials:
// the showcase host every frontend's outline is checked against
// (Tests/Core/Session/ShowcaseSessionTree.h), so a row that reads well on the
// Mac and badly on the phone is the drawing's doing and not the data's.

#include "Core/Session/SessionTree.h"

#include <cstdint>

/// The showcase host's tree, stated at a clock. Supplied by the embedder that
/// seeds a UI-test launch.
///
/// - Parameter nowUnixMilliseconds: The host's clock, which every age is
///   measured back from.
/// - Returns: The tree, as a listing would answer it.
Ubiquitty::Session::SessionTree ubiquittyShowcaseHostTree(std::uint64_t nowUnixMilliseconds);
