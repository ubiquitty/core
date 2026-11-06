// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/PTYFacts.h"

namespace Ubiquitty::POSIX {

/// Checks an inherited master against its admitted live process identity.
///
/// Parent changes are allowed after admission. A missing/reused leader or a
/// master naming another session is not authority to send a process signal.
///
/// - Parameters:
///   - master: Borrowed PTY master descriptor.
///   - leader: Exact admitted session-leader PID.
///   - birth: Kernel birth identity captured at admission.
/// - Returns: Whether both the current process and terminal still match.
[[nodiscard]] bool matchesInheritedPTY(int master, PAL::ProcessID leader, const PAL::ProcessBirthIdentity& birth);

} // namespace Ubiquitty::POSIX
