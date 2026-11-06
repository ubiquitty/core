// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include <utility>

// Non-owning checked pointer. The target (a CanMakeCheckedPtr) counts its
// referents and refuses to be destroyed while any remain, so a pointer that
// would dangle ends the process at the moment it is created rather than at the
// unrelated moment it is next read.

namespace Ubiquitty {

class CanMakeCheckedPtr {
public:
    CanMakeCheckedPtr() = default;
    /// Ends the process when a referent outlives this target.
    ///
    /// Enforced in every configuration, not only where assertions run. The referents counted here are
    /// the frontend's clients on the terminal, and the ordering fault this exists to catch — a client
    /// destroyed while the terminal still holds it — happens on teardown paths a debug build rarely
    /// walks and a shipped one walks on every quit. A build that pays for the count on every
    /// construction and copy, and then declines to read it, has bought nothing.
    ///
    /// One compare against a counter already in cache, on a path that runs once per object destroyed.
    /// The alternative to ending here is a read through a pointer to freed memory, at a place with no
    /// way to know what it was.
    ~CanMakeCheckedPtr()
    {
        if (m_checkedPtrCount != 0)
            std::abort(); // destroyed while a CheckedPtr still references it
    }
    // A copy/move is a distinct object that starts with zero referents.
    CanMakeCheckedPtr(const CanMakeCheckedPtr&) { }
    CanMakeCheckedPtr(CanMakeCheckedPtr&&) noexcept { }
    CanMakeCheckedPtr& operator=(const CanMakeCheckedPtr&) { return *this; }
    CanMakeCheckedPtr& operator=(CanMakeCheckedPtr&&) noexcept { return *this; }

    void incrementCheckedPtrCount() const noexcept { ++m_checkedPtrCount; }
    void decrementCheckedPtrCount() const noexcept
    {
        // An unbalanced release would wrap the count and let the destructor
        // above pass over a target that still has referents, which is the one
        // way that check can be defeated.
        if (m_checkedPtrCount == 0)
            std::abort();
        --m_checkedPtrCount;
    }

private:
    mutable uint32_t m_checkedPtrCount = 0;
};

template<typename T> class CheckedPtr {
public:
    constexpr CheckedPtr() noexcept = default;
    constexpr CheckedPtr(std::nullptr_t) noexcept { }
    CheckedPtr(T* ptr) noexcept
        : m_ptr(ptr)
    {
        ref();
    }
    CheckedPtr(const CheckedPtr& o) noexcept
        : m_ptr(o.m_ptr)
    {
        ref();
    }
    CheckedPtr(CheckedPtr&& o) noexcept
        : m_ptr(std::exchange(o.m_ptr, nullptr))
    {
    }
    ~CheckedPtr() { deref(); }

    CheckedPtr& operator=(const CheckedPtr& o) noexcept
    {
        reset(o.m_ptr);
        return *this;
    }
    CheckedPtr& operator=(CheckedPtr&& o) noexcept
    {
        if (this != &o) {
            deref();
            m_ptr = std::exchange(o.m_ptr, nullptr);
        }
        return *this;
    }
    CheckedPtr& operator=(T* ptr) noexcept
    {
        reset(ptr);
        return *this;
    }
    CheckedPtr& operator=(std::nullptr_t) noexcept
    {
        reset(nullptr);
        return *this;
    }

    [[nodiscard]] T* get() const noexcept { return m_ptr; }
    T* operator->() const noexcept { return m_ptr; }
    T& operator*() const noexcept { return *m_ptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_ptr != nullptr; }

    friend bool operator==(const CheckedPtr& a, const CheckedPtr& b) noexcept { return a.m_ptr == b.m_ptr; }
    friend bool operator==(const CheckedPtr& a, const T* b) noexcept { return a.m_ptr == b; }
    friend bool operator==(const CheckedPtr& a, std::nullptr_t) noexcept { return a.m_ptr == nullptr; }

private:
    void ref() const noexcept
    {
        // Asserted here rather than in the class body: a CheckedPtr<T> member
        // may be declared while T is still incomplete, and this is the first
        // place T has to be complete anyway.
        static_assert(std::derived_from<T, CanMakeCheckedPtr>,
            "a CheckedPtr points at something that derives from CanMakeCheckedPtr");
        if (m_ptr)
            m_ptr->incrementCheckedPtrCount();
    }
    void deref() const noexcept
    {
        if (m_ptr)
            m_ptr->decrementCheckedPtrCount();
    }
    void reset(T* p) noexcept
    {
        if (p == m_ptr)
            return;
        deref();
        m_ptr = p;
        ref();
    }

    T* m_ptr = nullptr;
};

static_assert(sizeof(CanMakeCheckedPtr) == sizeof(std::uint32_t),
    "a count is the whole cost a target pays to be checked");
static_assert(!std::is_polymorphic_v<CanMakeCheckedPtr>,
    "deriving from this must not give a target a vtable it would not otherwise have");

} // namespace Ubiquitty
