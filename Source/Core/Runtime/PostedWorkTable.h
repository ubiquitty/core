// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Core's table of posted session work a frontend's loop has yet to run, and
// the typed token the loop holds for each entry.
//
// A dispatcher posting to a frontend admits the unit here and hands the loop a
// token: a slot and that slot's generation, two integers and no pointer. The
// loop redeems the token on its own thread, running the unit or discarding it,
// and either consumes it. A token redeemed twice, or one the table never
// issued, names no pending unit and is refused; its slot's generation has
// moved on, so it cannot reach a unit posted since. The units stay Core's
// until they are redeemed, so a loop that never redeems a token holds a slot,
// not memory nobody owns.
//
// Slots are reused, so posting allocates only when more units are pending
// than ever before. A unit runs, or is destroyed, with no lock held: it may
// post again, and its captures may release what posts. Admitting or taking a
// unit moves its captures under the lock, so what a capture leaves behind when
// it is moved from must destroy without posting, as every standard type and
// weak reference does.

#include "Base/Compiler.h"
#include "Core/Session/PostedWork.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

namespace Ubiquitty::Runtime {

/// Names one unit of posted work pending in a ``PostedWorkTable``.
struct PostedWorkToken {
    /// The slot holding the unit.
    std::uint32_t slot { 0 };
    /// The slot's generation when the unit was admitted. Never zero for an
    /// issued token, so a zeroed one names nothing.
    std::uint32_t generation { 0 };

    /// Whether two tokens name the same admission.
    friend bool operator==(const PostedWorkToken&, const PostedWorkToken&) = default;
};

/// A frontend loop's two entry points for posted work.
///
/// Each is handed a token it must redeem exactly once, on the loop's thread,
/// through ``runPostedSessionWork`` or ``discardPostedSessionWork``. Two loops
/// are the same loop when their entry points are.
struct PostedWorkLoop {
    /// Arranges for the token to be redeemed once the loop's current turn ends.
    void (*runSoon)(PostedWorkToken token) { nullptr };
    /// Arranges for the token to be redeemed once `seconds` have passed.
    void (*runAfter)(double seconds, PostedWorkToken token) { nullptr };

    /// Whether both entry points are set.
    ///
    /// - Returns: True when a dispatcher can post to this loop.
    [[nodiscard]] bool isComplete() const { return runSoon && runAfter; }

    /// Whether two loops post through the same entry points.
    friend bool operator==(const PostedWorkLoop&, const PostedWorkLoop&) = default;
};

/// What redeeming a token did.
enum class UBIQUITTY_CLOSED_ENUM PostedWorkOutcome : std::uint8_t {
    /// The token named a pending unit, which ran.
    Ran,
    /// The token named a pending unit, which was destroyed unrun.
    Discarded,
    /// The token named nothing pending: it was redeemed already, was never
    /// issued, or names a slot reused since.
    Refused,
};

/// The posted units a frontend's loop holds tokens for.
///
/// Admission is safe from any thread; a token is redeemed on the loop's.
class PostedWorkTable final {
public:
    /// Creates an empty table.
    PostedWorkTable() = default;

    PostedWorkTable(const PostedWorkTable&) = delete;
    PostedWorkTable& operator=(const PostedWorkTable&) = delete;

    /// The table every ``PostedDispatcher`` admits to and every loop redeems
    /// from.
    ///
    /// Never destroyed: a loop abandons the tokens it holds when the process
    /// exits, and a thread still posting then must find a table to post to.
    ///
    /// - Returns: The process's table.
    [[nodiscard]] static PostedWorkTable& singleton()
    {
        static union Storage {
            Storage()
                : table()
            {
            }
            ~Storage() { }
            PostedWorkTable table;
        } storage;
        return storage.table;
    }

    /// Takes a unit until the token it is given for it is redeemed.
    ///
    /// - Parameter work: The unit, taken from the caller. An empty one is not
    ///   admitted.
    /// - Returns: The token naming the unit; a zeroed token, which names
    ///   nothing, for an empty unit.
    [[nodiscard]] PostedWorkToken admit(Session::PostedWork&& work)
    {
        if (!work)
            return {};
        const std::scoped_lock hold(m_mutex);
        std::uint32_t index = m_firstFree;
        if (index == noSlot) {
            // Four billion units pending is memory long exhausted, so the
            // bound is an invariant rather than a load to refuse.
            if (m_slots.size() >= noSlot)
                std::abort();
            index = static_cast<std::uint32_t>(m_slots.size());
            m_slots.emplace_back();
        } else {
            m_firstFree = m_slots[index].nextFree;
        }
        Slot& slot = m_slots[index];
        slot.work = std::move(work);
        slot.nextFree = noSlot;
        ++m_pendingCount;
        return { .slot = index, .generation = slot.generation };
    }

    /// Runs the unit a token names, consuming the token.
    ///
    /// - Parameter token: A token this table issued.
    /// - Returns: ``PostedWorkOutcome/Ran``, or ``PostedWorkOutcome/Refused``
    ///   when the token names nothing pending.
    PostedWorkOutcome run(PostedWorkToken token)
    {
        Session::PostedWork work = take(token);
        if (!work)
            return PostedWorkOutcome::Refused;
        std::move(work)();
        return PostedWorkOutcome::Ran;
    }

    /// Destroys unrun the unit a token names, consuming the token.
    ///
    /// - Parameter token: A token this table issued.
    /// - Returns: ``PostedWorkOutcome/Discarded``, or
    ///   ``PostedWorkOutcome/Refused`` when the token names nothing pending.
    PostedWorkOutcome discard(PostedWorkToken token)
    {
        const Session::PostedWork work = take(token);
        return work ? PostedWorkOutcome::Discarded : PostedWorkOutcome::Refused;
    }

    /// The units admitted and not yet redeemed.
    ///
    /// - Returns: The count.
    [[nodiscard]] std::size_t pendingCount() const
    {
        const std::scoped_lock hold(m_mutex);
        return m_pendingCount;
    }

    /// The slots the table has made, pending or free: the most units it has
    /// held at once.
    ///
    /// - Returns: The count.
    [[nodiscard]] std::size_t slotCount() const
    {
        const std::scoped_lock hold(m_mutex);
        return m_slots.size();
    }

private:
    /// The end of the free list.
    static constexpr std::uint32_t noSlot = std::numeric_limits<std::uint32_t>::max();

    /// One unit's place in the table, pending while it holds work.
    struct Slot {
        /// The unit, or nothing while the slot is free.
        Session::PostedWork work;
        /// The generation a token for this slot's current unit carries.
        std::uint32_t generation { 1 };
        /// The next free slot while this one is free.
        std::uint32_t nextFree { noSlot };
    };

    /// Moves out the unit a token names and frees its slot.
    ///
    /// The caller runs or destroys the unit after the lock is released.
    ///
    /// - Parameter token: The token.
    /// - Returns: The unit, or an empty one when the token names nothing
    ///   pending.
    [[nodiscard]] Session::PostedWork take(PostedWorkToken token)
    {
        const std::scoped_lock hold(m_mutex);
        if (token.slot >= m_slots.size())
            return {};
        Slot& slot = m_slots[token.slot];
        if (slot.generation != token.generation || !slot.work)
            return {};
        Session::PostedWork work = std::move(slot.work);
        slot.generation = slot.generation == std::numeric_limits<std::uint32_t>::max() ? 1 : slot.generation + 1;
        slot.nextFree = m_firstFree;
        m_firstFree = token.slot;
        --m_pendingCount;
        return work;
    }

    /// Guards every field below.
    mutable std::mutex m_mutex;
    /// A deque, so a slot never moves while the table grows.
    std::deque<Slot> m_slots;
    /// The most recently freed slot, heading the free list.
    std::uint32_t m_firstFree { noSlot };
    std::size_t m_pendingCount { 0 };
};

/// Runs, on the loop it was handed to, the unit a token names.
///
/// The far side of a ``PostedDispatcher`` calls this on its loop for each
/// token, or ``discardPostedSessionWork`` instead; a token is redeemed once.
///
/// - Parameter token: The token the loop was handed.
/// - Returns: Whether the unit ran, or the token was refused.
inline PostedWorkOutcome runPostedSessionWork(PostedWorkToken token) { return PostedWorkTable::singleton().run(token); }

/// Destroys, unrun, the unit a token names: what a loop owes a token it will
/// never run.
///
/// A unit whose owner is gone holds a weak reference to it and does nothing
/// when run, so a loop that cannot tell which session a token names may
/// always run it instead.
///
/// - Parameter token: The token the loop was handed.
/// - Returns: Whether the unit was destroyed, or the token was refused.
inline PostedWorkOutcome discardPostedSessionWork(PostedWorkToken token) { return PostedWorkTable::singleton().discard(token); }

} // namespace Ubiquitty::Runtime
