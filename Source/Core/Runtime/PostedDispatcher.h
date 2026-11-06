// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Runtime/PostedWorkTable.h"
#include "Core/Session/RemoteSessionCore.h"

#include <cassert>
#include <utility>

namespace Ubiquitty::Runtime {

/// Posts Core work to a frontend's event loop.
///
/// Each unit is admitted to ``PostedWorkTable/singleton()`` and the loop is
/// handed the token that names it. A value of two function pointers, so a
/// copy carried to another thread carries no lifetime with it.
class PostedDispatcher final : public Session::Dispatcher {
public:
    /// Creates a dispatcher with no loop, assigned before it posts.
    PostedDispatcher() = default;

    /// Creates a dispatcher that posts to a loop.
    ///
    /// - Parameter loop: The loop's entry points, both set.
    explicit PostedDispatcher(PostedWorkLoop loop)
        : m_loop(loop)
    {
        assert(loop.isComplete());
    }

    /// Hands the loop one unit to run once its current turn ends.
    ///
    /// A dispatcher with no loop destroys the unit unrun.
    ///
    /// - Parameter work: The unit.
    void post(Session::PostedWork work) override
    {
        assert(m_loop.isComplete());
        if (!m_loop.isComplete())
            return;
        m_loop.runSoon(PostedWorkTable::singleton().admit(std::move(work)));
    }

    /// Hands the loop one unit to run once `seconds` have passed.
    ///
    /// A dispatcher with no loop destroys the unit unrun.
    ///
    /// - Parameters:
    ///   - seconds: The delay.
    ///   - work: The unit.
    void postDelayed(double seconds, Session::PostedWork work) override
    {
        assert(m_loop.isComplete());
        if (!m_loop.isComplete())
            return;
        m_loop.runAfter(seconds, PostedWorkTable::singleton().admit(std::move(work)));
    }

    /// The loop this dispatcher posts to.
    ///
    /// - Returns: Its entry points.
    [[nodiscard]] const PostedWorkLoop& loop() const { return m_loop; }

private:
    PostedWorkLoop m_loop;
};

} // namespace Ubiquitty::Runtime
