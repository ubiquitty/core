// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <memory>
#include <utility>

namespace Ubiquitty {

/// Keeps a uniquely owned object alive if a callback replaces its owner.
///
/// The scopes form a borrowed stack on one thread; merely entering a scope
/// allocates nothing and does not change ownership. Before replacing an
/// object, preserve() transfers it to its outermost active scope, if any.
/// Nested callbacks can then replace the current object without destroying
/// the one an outer call is still using. At most one object is kept per scope.
/// The owner of the stack head must outlive its active calls.
template<typename Object> class ActiveObjectUse {
public:
    ActiveObjectUse(ActiveObjectUse*& active, Object& object)
        : m_active(active)
        , m_previous(std::exchange(active, this))
        , m_object(&object)
    {
    }

    ~ActiveObjectUse()
    {
        assert(m_active == this);
        m_active = m_previous;
    }

    ActiveObjectUse(const ActiveObjectUse&) = delete;
    ActiveObjectUse& operator=(const ActiveObjectUse&) = delete;

    /// The exact object this call entered, even after its owner changed.
    [[nodiscard]] Object* get() const { return m_object; }

    /// Transfers an active object into its oldest matching scope. An object
    /// with no active call stays with its owner and can be destroyed now.
    static void preserve(ActiveObjectUse* active, std::unique_ptr<Object>& object)
    {
        if (!object)
            return;
        ActiveObjectUse* outermost = nullptr;
        for (auto* use = active; use; use = use->m_previous) {
            if (use->m_object == object.get())
                outermost = use;
        }
        if (outermost) {
            assert(!outermost->m_retained);
            outermost->m_retained = std::move(object);
        }
    }

    /// Whether this exact object is already executing inside a scope.
    [[nodiscard]] static bool contains(ActiveObjectUse* active, const Object* object)
    {
        for (auto* use = active; use; use = use->m_previous) {
            if (use->m_object == object)
                return true;
        }
        return false;
    }

private:
    ActiveObjectUse*& m_active;
    ActiveObjectUse* m_previous;
    Object* m_object;
    std::unique_ptr<Object> m_retained;
};

} // namespace Ubiquitty
