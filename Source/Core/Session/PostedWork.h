// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The unit of work a session hands its dispatcher: a move-only `void()`
// callable held inline, so posting one never reaches the heap.
//
// Running a unit consumes it. It moves onto the running frame first, so a
// container that holds it may grow, shrink or die while it runs, and its
// captures are destroyed as the run returns. A unit that is never run is
// destroyed with its owner and its captures with it; nothing is leaked by
// dropping one, and nothing can run twice.

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace Ubiquitty::Session {

class PostedWork;

/// Whether a unit of posted work can be made from a `Work`: a callable that
/// takes no arguments, and is not a unit already.
template<typename Work>
concept PostableWork = !std::same_as<std::remove_cvref_t<Work>, PostedWork> && std::is_invocable_v<std::decay_t<Work>&>;

/// One unit of work for a dispatcher to run on its session thread.
///
/// The callable's captures must fit ``capacity`` bytes, need no stricter
/// alignment than ``alignment``, and move without throwing, since whatever
/// holds the unit between its post and its run may relocate it. A copy of a
/// `const` object does not move, so a lambda takes a mutable copy instead
/// (`reason = std::string(reason)`). A producer with a larger payload boxes it
/// in a `std::unique_ptr` and captures that, so the allocation is made, and
/// seen, at the one site that needs it.
class PostedWork final {
public:
    /// The inline storage, in bytes: a weak reference, two words of
    /// generation or serial, and an owned buffer or string beside them.
    static constexpr std::size_t capacity = 64;
    /// The strictest alignment a held callable may ask for.
    static constexpr std::size_t alignment = alignof(std::max_align_t);

    /// Whether a callable of type `Work` can be held.
    template<typename Work>
    static constexpr bool canHold = sizeof(Work) <= capacity && alignof(Work) <= alignment
        && std::is_nothrow_move_constructible_v<Work> && std::is_nothrow_destructible_v<Work>
        && std::is_invocable_v<Work&>;

    /// Creates a unit that holds no work.
    ///
    /// Not defaulted: value-initialising a defaulted constructor would zero
    /// the storage of every empty unit a queue makes.
    PostedWork() noexcept { }

    /// Holds `work` inline.
    ///
    /// Implicit, so a post site passes its lambda as it is.
    ///
    /// - Parameter work: The callable, copied or moved into the unit.
    template<PostableWork Work>
    PostedWork(Work&& work)
    {
        using Held = std::decay_t<Work>;
        static_assert(sizeof(Held) <= capacity,
            "the captures do not fit a posted unit; box the payload in a std::unique_ptr and capture that");
        static_assert(alignof(Held) <= alignment, "a posted unit cannot align its captures that strictly");
        static_assert(std::is_nothrow_move_constructible_v<Held>,
            "a posted unit is relocated, so its captures must move without throwing; capture a mutable copy of a const object");
        std::construct_at(storageFor<Held>(m_storage.data()), std::forward<Work>(work));
        m_operations = &operationsFor<Held>;
    }

    /// Takes the work another unit holds.
    ///
    /// - Parameter other: The unit to take from, left empty.
    PostedWork(PostedWork&& other) noexcept { take(other); }

    /// Destroys the work this unit holds and takes the work another holds.
    ///
    /// - Parameter other: The unit to take from, left empty.
    /// - Returns: This unit.
    PostedWork& operator=(PostedWork&& other) noexcept
    {
        if (this != &other) {
            reset();
            take(other);
        }
        return *this;
    }

    PostedWork(const PostedWork&) = delete;
    PostedWork& operator=(const PostedWork&) = delete;

    /// Destroys the held work unrun.
    ~PostedWork() { reset(); }

    /// Whether the unit holds work.
    ///
    /// - Returns: True until the unit is run, reset or moved from.
    explicit operator bool() const { return m_operations; }

    /// Runs the held work once and destroys it, leaving the unit empty.
    ///
    /// The work moves onto this frame before it runs, so the run may drop or
    /// reuse whatever held the unit. Running an empty unit is a dispatcher's
    /// error, and runs nothing.
    void operator()() &&
    {
        assert(m_operations);
        PostedWork running(std::move(*this));
        if (running.m_operations)
            running.m_operations->run(running.m_storage.data());
    }

    /// Destroys the held work unrun, leaving the unit empty.
    void reset() noexcept
    {
        if (const Operations* operations = std::exchange(m_operations, nullptr))
            operations->destroy(m_storage.data());
    }

private:
    /// What a unit can do with the callable it holds, one table per type.
    struct Operations {
        /// Calls the callable in place.
        void (*run)(std::byte* storage);
        /// Moves the callable to another unit's storage, destroying the source.
        void (*relocate)(std::byte* destination, std::byte* source) noexcept;
        /// Destroys the callable unrun.
        void (*destroy)(std::byte* storage) noexcept;
    };

    /// Where a callable of type `Held` is constructed in a unit's storage.
    ///
    /// - Parameter storage: The unit's storage.
    /// - Returns: The address to construct at.
    template<typename Held>
    [[nodiscard]] static Held* storageFor(std::byte* storage)
    {
        // Puns the unit's byte storage as the callable constructed in it.
        return reinterpret_cast<Held*>(storage);
    }

    /// The callable of type `Held` already living in a unit's storage.
    ///
    /// - Parameter storage: The unit's storage.
    /// - Returns: The callable.
    template<typename Held>
    [[nodiscard]] static Held* heldIn(std::byte* storage) { return std::launder(storageFor<Held>(storage)); }

    /// Calls the callable of type `Held` in place.
    ///
    /// - Parameter storage: The storage holding it.
    template<typename Held>
    static void runHeld(std::byte* storage) { std::invoke(*heldIn<Held>(storage)); }

    /// Moves the callable of type `Held` to another unit's storage and
    /// destroys what is left behind.
    ///
    /// - Parameters:
    ///   - destination: The storage to move it to, holding nothing.
    ///   - source: The storage holding it, holding nothing after.
    template<typename Held>
    static void relocateHeld(std::byte* destination, std::byte* source) noexcept
    {
        Held* held = heldIn<Held>(source);
        std::construct_at(storageFor<Held>(destination), std::move(*held));
        std::destroy_at(held);
    }

    /// Destroys the callable of type `Held` unrun.
    ///
    /// - Parameter storage: The storage holding it.
    template<typename Held>
    static void destroyHeld(std::byte* storage) noexcept { std::destroy_at(heldIn<Held>(storage)); }

    /// The operations for a held callable of type `Held`.
    template<typename Held>
    static constexpr Operations operationsFor { .run = &runHeld<Held>, .relocate = &relocateHeld<Held>, .destroy = &destroyHeld<Held> };

    /// Takes the work another unit holds into this empty one.
    ///
    /// - Parameter other: The unit to take from, left empty.
    void take(PostedWork& other) noexcept
    {
        if (const Operations* operations = std::exchange(other.m_operations, nullptr)) {
            operations->relocate(m_storage.data(), other.m_storage.data());
            m_operations = operations;
        }
    }

    /// The held callable's bytes; meaningful only while ``m_operations`` is set.
    alignas(alignment) std::array<std::byte, capacity> m_storage;
    /// The held callable's operations, or null for an empty unit.
    const Operations* m_operations { nullptr };
};

} // namespace Ubiquitty::Session
