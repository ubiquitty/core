// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace Ubiquitty::Runtime {

/// How long a present waits after a whole-screen erase for the program's bytes
/// to go quiet: a redraw arrives in bursts closer than this.
inline constexpr std::uint32_t eraseHoldSettleMicros = 6000;

/// The longest a present waits after a whole-screen erase, so a program that
/// clears and stops still shows its blank screen, and one that erases on every
/// refresh still reaches the display.
inline constexpr std::uint32_t eraseHoldMaxMicros = 50000;

/// The microseconds from `then` to `now`, on the wrapping 32-bit clock, and
/// zero for a `then` stamped after `now`: the ingest thread stamps bytes and
/// erases while a presenter holds a `now` it read a moment earlier, and the
/// unsigned difference of the two is a span of seventy minutes, which reads as
/// a hold long settled and spent.
[[nodiscard]] constexpr std::uint32_t microsSince(std::uint32_t now, std::uint32_t then)
{
    return static_cast<std::int32_t>(now - then) > 0 ? now - then : 0;
}

/// The hold a whole-screen erase puts on presentation: a present waits for the
/// program's redraw until its bytes have been quiet for the settle, and never
/// longer than the watchdog.
///
/// One word holds the erase the hold is for and when the hold started, so an
/// erase and a release cannot cross. The drain arms under the model lock; a
/// presenter releases without it; each changes the word only from the value it
/// read. A presenter that decided to release a hold an erase has since joined
/// finds the word moved, and decides again with the bytes that erase brought.
///
/// Whether an erase finds a hold to join does not depend on whether a
/// presenter asked. A pause of the program's bytes as long as the settle
/// releases the hold when the next bytes arrive (``stampBytes(_:_:)``), as an
/// ask during the pause would have, so an erase after the pause starts a hold
/// of its own. An erase among bytes that never paused joins the hold that
/// stands, keeping its start, so erases on every refresh cannot postpone
/// presentation past one watchdog.
class EraseHold {
public:
    /// What a presenter read of the hold, which a release is made against.
    struct Reading {
        std::uint64_t word = 0;

        /// When the hold started, in monotonic microseconds; 0 when released.
        [[nodiscard]] std::uint32_t startMicros() const { return static_cast<std::uint32_t>(word); }

        /// The whole-screen erase generation the hold is for.
        [[nodiscard]] std::uint32_t generation() const { return static_cast<std::uint32_t>(word >> 32); }
    };

    /// Holds presentation for an erase, unless it is the one already held.
    ///
    /// Called with the model lock held, by the thread that parsed the erase or
    /// a capture that found it, after the erase's bytes were stamped.
    ///
    /// - Parameters:
    ///   - generation: The model's whole-screen erase generation.
    ///   - nowMicros: Now, in monotonic microseconds.
    void arm(std::uint32_t generation, std::uint32_t nowMicros)
    {
        std::uint64_t word = m_word.load(std::memory_order_relaxed);
        while (true) {
            const Reading held { word };
            if (held.generation() == generation)
                return;
            const std::uint32_t start = held.startMicros() != 0 ? held.startMicros() : (nowMicros | 1u);
            const std::uint64_t armed = (static_cast<std::uint64_t>(generation) << 32) | start;
            // Release: a presenter that reads this word reads the bytes stamped before it.
            if (m_word.compare_exchange_weak(word, armed, std::memory_order_release, std::memory_order_relaxed))
                return;
        }
    }

    /// Stamps the program's newest bytes, and releases the hold when the bytes
    /// paused for the settle before them.
    ///
    /// Called by every thread that receives the program's bytes, before any
    /// erase among them is armed. Without a hold there is no pause to settle,
    /// and the stamp is a plain store; with one, the stamp it replaces is read
    /// in the same atomic step, so a writer on another thread cannot make a
    /// pause appear that its bytes filled.
    ///
    /// - Parameters:
    ///   - lastBytesMicros: Where the program's newest bytes are stamped.
    ///   - nowMicros: When these arrived, in monotonic microseconds.
    void stampBytes(std::atomic<std::uint32_t>& lastBytesMicros, std::uint32_t nowMicros)
    {
        if (Reading { m_word.load(std::memory_order_relaxed) }.startMicros() == 0) {
            lastBytesMicros.store(nowMicros | 1u, std::memory_order_relaxed);
            return;
        }
        const std::uint32_t previous = lastBytesMicros.exchange(nowMicros | 1u, std::memory_order_relaxed);
        if (microsSince(nowMicros, previous) < eraseHoldSettleMicros)
            return;
        Reading reading = read();
        while (reading.startMicros() != 0) {
            if (release(reading))
                return;
        }
    }

    /// Reads the hold as it stands. Any thread.
    [[nodiscard]] Reading read() const { return { m_word.load(std::memory_order_acquire) }; }

    /// Returns whether a reading holds presentation.
    ///
    /// - Parameters:
    ///   - reading: The hold, as read.
    ///   - nowMicros: Now, in monotonic microseconds.
    ///   - lastBytesMicros: When the program's newest bytes arrived, read after
    ///     the hold.
    [[nodiscard]] static bool isHolding(Reading reading, std::uint32_t nowMicros, std::uint32_t lastBytesMicros)
    {
        return reading.startMicros() != 0 && microsSince(nowMicros, reading.startMicros()) < eraseHoldMaxMicros
            && microsSince(nowMicros, lastBytesMicros) < eraseHoldSettleMicros;
    }

    /// Releases the hold a reading saw, unless an erase has moved it since.
    ///
    /// - Parameter reading: The hold as read; when the hold has moved, it is
    ///   updated to the hold as it now stands.
    /// - Returns: Whether the hold was released.
    bool release(Reading& reading)
    {
        const std::uint64_t released = reading.word & ~static_cast<std::uint64_t>(0xFFFF'FFFFu);
        return m_word.compare_exchange_strong(reading.word, released, std::memory_order_acquire, std::memory_order_acquire);
    }

    /// Returns whether presentation waits now, and releases a hold that has run
    /// out. Any thread, without the model lock.
    ///
    /// - Parameters:
    ///   - nowMicros: Now, in monotonic microseconds.
    ///   - lastBytesMicros: Where the program's newest bytes are stamped.
    [[nodiscard]] bool shouldWithhold(std::uint32_t nowMicros, const std::atomic<std::uint32_t>& lastBytesMicros)
    {
        Reading reading = read();
        while (reading.startMicros() != 0) {
            if (isHolding(reading, nowMicros, lastBytesMicros.load(std::memory_order_relaxed)))
                return true;
            if (release(reading))
                return false;
        }
        return false;
    }

    /// Returns the microseconds until the hold can release on its own.
    ///
    /// - Parameters:
    ///   - nowMicros: Now, in monotonic microseconds.
    ///   - lastBytesMicros: Where the program's newest bytes are stamped.
    /// - Returns: 0 when nothing holds.
    [[nodiscard]] std::uint32_t microsRemaining(std::uint32_t nowMicros, const std::atomic<std::uint32_t>& lastBytesMicros) const
    {
        const Reading reading = read();
        const std::uint32_t lastBytes = lastBytesMicros.load(std::memory_order_relaxed);
        if (!isHolding(reading, nowMicros, lastBytes))
            return 0;
        const std::uint32_t sinceStart = microsSince(nowMicros, reading.startMicros());
        const std::uint32_t sinceBytes = microsSince(nowMicros, lastBytes);
        return std::min(eraseHoldMaxMicros - sinceStart, eraseHoldSettleMicros - sinceBytes);
    }

private:
    // The erase generation the hold is for in the high half, and its start in
    // the low half, 0 once released.
    std::atomic<std::uint64_t> m_word { 0 };
};

} // namespace Ubiquitty::Runtime
