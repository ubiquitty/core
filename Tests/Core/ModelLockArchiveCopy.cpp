// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Compiled optimised and with NDEBUG (see Tests/CMakeLists.txt), as a
// frontend's Core archive is, beside a test whose own translation unit keeps
// assertions, as a frontend's C++ interop does: the lock taken here runs the
// archive's copy of ModelLock's inline acquisition.

#include "Core/ModelLock.h"

namespace Ubiquitty::Testing {

void lockAsAnOptimisedArchiveDoes(ModelLock& lock) UBIQUITTY_ACQUIRES(modelLockRole) { lock.lock(); }

void unlockAsAnOptimisedArchiveDoes(ModelLock& lock) UBIQUITTY_RELEASES(modelLockRole) { lock.unlock(); }

} // namespace Ubiquitty::Testing
