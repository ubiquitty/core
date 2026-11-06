// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include <utility>

// Non-owning weak pointer. A reference reads as null once its target is gone.
//
// The sibling of CheckedPtr, for the opposite contract: the referent that
// legitimately outlives its target — a closure a dispatcher holds, a timer
// armed against a deadline, whose owner a person may release first.
//
// A reference is taken, copied and released on any thread. It is read on the
// thread that destroys the target.

namespace Ubiquitty {

class CanMakeWeakPtr;
template<typename T> class WeakPtr;

namespace Detail {

/// The indirection a WeakPtr reads through. Outlives the target, so it cannot answer by reading one.
class WeakReference {
public:
    explicit WeakReference(CanMakeWeakPtr* target) noexcept
        : m_target(target)
    {
    }

    /// Takes one reference.
    void ref() noexcept { m_referenceCount.fetch_add(1, std::memory_order_relaxed); }
    /// Releases one reference, freeing this when the last goes.
    void deref() noexcept
    {
        const std::uint32_t previous = m_referenceCount.fetch_sub(1, std::memory_order_acq_rel);
        // An unbalanced release wraps the count and frees this while a reference
        // still reads through it, which is the one way the null is defeated.
        if (previous == 0)
            std::abort();
        if (previous == 1)
            delete this;
    }

    /// Severs the target. The block stays readable.
    void clear() noexcept { m_target.store(nullptr, std::memory_order_release); }
    [[nodiscard]] CanMakeWeakPtr* target() const noexcept
    {
        return m_target.load(std::memory_order_acquire);
    }

private:
    std::atomic<CanMakeWeakPtr*> m_target;
    std::atomic<std::uint32_t> m_referenceCount { 1 }; // the target's own
};

} // namespace Detail

/// Base for a target that hands out weak references: `class Thing : public CanMakeWeakPtr`.
///
/// The concrete type is deduced from the explicit object parameter, so a target names no type
/// argument and cannot name the wrong one.
class CanMakeWeakPtr {
public:
    // Allocated here, not on first use: a lazy slot would be written by whichever thread asked
    // first and read by the one severing it, and those are different threads.
    CanMakeWeakPtr() noexcept
        : m_reference(new Detail::WeakReference(this))
    {
    }
    /// Severs every outstanding reference and releases the target's own.
    ~CanMakeWeakPtr()
    {
        m_reference->clear();
        m_reference->deref();
    }

    // A copy or move is a distinct object with its own reference. References handed out by the
    // original keep naming the original.
    CanMakeWeakPtr(const CanMakeWeakPtr&) noexcept
        : m_reference(new Detail::WeakReference(this))
    {
    }
    CanMakeWeakPtr(CanMakeWeakPtr&&) noexcept
        : m_reference(new Detail::WeakReference(this))
    {
    }
    CanMakeWeakPtr& operator=(const CanMakeWeakPtr&) noexcept { return *this; }
    CanMakeWeakPtr& operator=(CanMakeWeakPtr&&) noexcept { return *this; }

    /// A reference to self that reads as null once self is gone. Costs a count, not an allocation.
    template<typename Self>
    [[nodiscard]] WeakPtr<Self> weakPtr(this const Self& self)
    {
        static_assert(std::derived_from<Self, CanMakeWeakPtr>,
            "weakPtr() is for a target that derives from CanMakeWeakPtr");
        static_assert(!std::is_pointer_v<Self>, "call this on the object, not on a pointer to it");

        const CanMakeWeakPtr& base = self;
        return WeakPtr<Self>(base.m_reference);
    }

    /// Severs every outstanding reference. Final, and idempotent.
    ///
    /// A destructor that has handed out references calls this before tearing down the fields that
    /// work would touch; the base destructor above runs too late for that.
    void revokeWeakPtrs() const noexcept { m_reference->clear(); }

private:
    // Never null, never reassigned: a thread taking a reference reads a pointer no thread writes.
    Detail::WeakReference* const m_reference;
};

static_assert(!std::is_polymorphic_v<CanMakeWeakPtr>,
    "deriving from this must not give a target a vtable it would not otherwise have");
static_assert(sizeof(CanMakeWeakPtr) == sizeof(void*),
    "one pointer is the whole cost a target pays to be observable");
static_assert(std::atomic<CanMakeWeakPtr*>::is_always_lock_free
        && std::atomic<std::uint32_t>::is_always_lock_free,
    "a reference is taken on one thread and severed on another, so neither field may take a lock");

template<typename T> class WeakPtr {
public:
    constexpr WeakPtr() noexcept = default;
    constexpr WeakPtr(std::nullptr_t) noexcept { }

    explicit WeakPtr(Detail::WeakReference* reference) noexcept
        : m_reference(reference)
    {
        if (m_reference)
            m_reference->ref();
    }
    WeakPtr(const WeakPtr& other) noexcept
        : m_reference(other.m_reference)
    {
        if (m_reference)
            m_reference->ref();
    }
    WeakPtr(WeakPtr&& other) noexcept
        : m_reference(std::exchange(other.m_reference, nullptr))
    {
    }
    ~WeakPtr() { deref(); }

    WeakPtr& operator=(const WeakPtr& other) noexcept
    {
        if (this != &other)
            reset(other.m_reference);
        return *this;
    }
    WeakPtr& operator=(WeakPtr&& other) noexcept
    {
        if (this != &other) {
            deref();
            m_reference = std::exchange(other.m_reference, nullptr);
        }
        return *this;
    }
    WeakPtr& operator=(std::nullptr_t) noexcept
    {
        reset(nullptr);
        return *this;
    }

    /// The target, or null once it is gone. Never extends the target's lifetime.
    ///
    /// The downcast adjusts for where the base sits in T, so a target with more than one base hands
    /// back its own address.
    [[nodiscard]] T* get() const noexcept
    {
        // Asserted here, not in the class body: a WeakPtr<T> member may be declared while T is
        // still incomplete, and this is the first place T has to be complete anyway.
        static_assert(std::derived_from<T, CanMakeWeakPtr>,
            "a WeakPtr points at something that derives from CanMakeWeakPtr");
        return m_reference ? static_cast<T*>(m_reference->target()) : nullptr;
    }
    T* operator->() const noexcept { return get(); }
    T& operator*() const noexcept { return *get(); }
    [[nodiscard]] explicit operator bool() const noexcept { return get() != nullptr; }

    friend bool operator==(const WeakPtr& a, const WeakPtr& b) noexcept { return a.get() == b.get(); }
    friend bool operator==(const WeakPtr& a, std::nullptr_t) noexcept { return a.get() == nullptr; }

private:
    void deref() noexcept
    {
        if (m_reference)
            m_reference->deref();
    }
    void reset(Detail::WeakReference* reference) noexcept
    {
        if (reference == m_reference)
            return;
        deref();
        m_reference = reference;
        if (m_reference)
            m_reference->ref();
    }

    Detail::WeakReference* m_reference { nullptr };
};

} // namespace Ubiquitty
