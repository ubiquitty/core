// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Clock.cpp — Ubiquitty::PAL clock backend for every Apple platform.
//
// Shared by the AppKit and UIKit shells because nothing in it is macOS's:
// mach_absolute_time is Darwin's, the conversion is arithmetic, and the two
// entry points are Core's contract. Plain C++ under Apple/, so no Apple arm
// goes without it.
//
// Implements the two clock entry points from Source/PAL/PAL.h:
//
// monotonicTicks()   — uint32_t count at 60 Hz. Producers compare
// deltas; absolute value is arbitrary.
// monotonicMicros()  — uint32_t count at 1 MHz. Best-effort wrap-
// around; only valid for deltas measured
// across calls within ~70 minutes.
//
// Backed by mach_absolute_time(), which is monotonic, never adjusted
// by NTP, and continues across sleep. We cache the conversion ratio
// (mach ticks -> nanoseconds) on first use; mach_timebase_info is
// fixed for the lifetime of a process so we never re-query it.
//
// Returns are best-effort wrap-around; callers must treat the result
// as cyclic and compute deltas, never compare absolutes.
//

#include "PAL/PAL.h"

#include <cstdlib>
#include <mach/mach_time.h>

namespace {

// Cache of mach_timebase_info on first use. The struct fields
// (numer, denom) are uint32_t; the rate is numer/denom nanoseconds
// per mach tick. On every Apple silicon and Intel Mac shipped to
// date it's either 1/1 or 125/3, but we don't bake either in.
struct TimebaseCache {
    mach_timebase_info_data_t info;
    bool initialized = false;
};

TimebaseCache& timebase()
{
    static TimebaseCache t;
    if (!t.initialized) {
        mach_timebase_info(&t.info);
        t.initialized = true;
    }
    return t;
}

// mach_absolute_time -> nanoseconds. uint64 carries ~584 years of
// nanoseconds so no overflow concern in the intermediate.
uint64_t machToNanoseconds(uint64_t machTicks)
{
    const TimebaseCache& t = timebase();
    return machTicks * t.info.numer / t.info.denom;
}

} // namespace

namespace Ubiquitty::PAL {

Ticks monotonicTicks()
{
    // Headless determinism hook: when UBIQUITTY_FAKE_TICKS is set the clock
    // returns that fixed tick count, so cursor-blink phase and any other
    // time-derived render state are reproducible across separate process
    // invocations (the offscreen snapshot path relies on this so golden
    // PNGs are byte-stable). Absent the env var the real clock is used.
    //
    // Read per call, not cached: a caller that pins the tick after this
    // function has already run once still takes the pin, and a harness can
    // move it between frames. A `static` here would latch whatever the
    // environment held at the first call — null for any process that touches
    // the clock before its own setenv — and defeat the pin silently for the
    // life of the process. This matches TerminalSession::frameTicksNow, which
    // reads the same variable fresh on every call for the same reason.
    if (const char* fake = getenv("UBIQUITTY_FAKE_TICKS"))
        return Ticks(static_cast<uint32_t>(strtoul(fake, nullptr, 10)));

    return Ticks::fromNanoseconds(machToNanoseconds(mach_absolute_time()));
}

uint32_t monotonicMicros()
{
    // 1 MHz: micros = nanoseconds / 1000. uint32_t wraps every ~71.6 min;
    // PAL.h documents that explicitly and pins the contract to
    // "relative deltas measured within ~hours."
    const uint64_t nanoseconds = machToNanoseconds(mach_absolute_time());
    return static_cast<uint32_t>((nanoseconds / 1000ull) & 0xFFFFFFFFull);
}

} // namespace Ubiquitty::PAL
