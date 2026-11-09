// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/ModelLock.h"

#include <cassert>

namespace Ubiquitty {

void ModelLock::assertHeld() const
{
    assert(m_holder.load(std::memory_order_relaxed) == std::this_thread::get_id());
}

} // namespace Ubiquitty
