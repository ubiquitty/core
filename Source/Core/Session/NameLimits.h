// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>

namespace Ubiquitty::Session {

/// The most UTF-8 bytes of a workspace name, everywhere it is admitted,
/// carried or recorded. Longer names are refused, never shortened.
inline constexpr std::size_t maxWorkspaceNameBytes = 4096;

} // namespace Ubiquitty::Session
