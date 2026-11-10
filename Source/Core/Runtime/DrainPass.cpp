// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/DrainPass.h"

#include "Core/Runtime/TerminalRuntime.h"
#include "Core/TerminalOperations.h"
#include "PAL/PAL.h"
#include "PAL/Stream.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>
#include <utility>

namespace Ubiquitty::Runtime {

namespace {

// The dwell before each re-read in the refill band (drainQuietSpinBudget).
// It must sit inside the tty's refill window: a much longer dwell overshoots
// every refill, and a zero dwell burns a probe syscall per quantum. A
// persistent would-block means the writer is not yet scheduled, which no
// dwell this side of the pass can change.
constexpr std::uint64_t drainRefillDwellNanoseconds = 100;

} // namespace

DrainPassResult runDrainPass(Terminal& terminal, PAL::Stream& stream, DrainHost& host, DrainPassOwner& owner, const DrainPassPolicy& policy)
{
    DrainPassResult result;
    const int spinBudget = policy.doesRideBurst ? drainQuietSpinBudget : 0;
    const int bite = std::max(1, policy.modelLockBiteBytes);
    const std::uint32_t startedAt = policy.maxMicroseconds ? PAL::monotonicMicros() : 0;

    std::array<char, drainReadBufferBytes> buffer;
    int quietSpins = 0;
    while (result.bytesDrained < policy.maxBytes) {
        owner.noteReadBegin();
        const auto readCapacity = static_cast<std::size_t>(std::min(drainReadBufferBytes, policy.maxBytes - result.bytesDrained));
        const int n = stream.read({ buffer.data(), readCapacity });
        if (n > 0) {
            const int safePrefix = owner.judgeRead({ buffer.data(), static_cast<std::size_t>(n) });
            const int parseEnd = (safePrefix >= 0) ? std::min(safePrefix, n) : n;
            int offset = 0;
            while (offset < parseEnd) {
                // Clamped to the owner's verdict, not to the read: the outer
                // condition only gates entry, and one bite spans the whole
                // chunk — a bound of n parses the bytes the owner just fenced.
                const int biteEnd = std::min(parseEnd, offset + bite);
                host.lockModel();
                if (policy.maxMicroseconds && offset == 0)
                    owner.noteParseBegin();
                while (offset < biteEnd) {
                    const int remaining = biteEnd - offset;
                    bool stoppedAtSyncClear = false;
                    const int consumed = Ubiquitty::processBytesUpToSyncClear(&terminal, std::string_view(buffer.data() + offset, static_cast<std::size_t>(remaining)), &stoppedAtSyncClear);
                    offset += (consumed > 0) ? consumed : remaining; // guard: never spin
                    if (stoppedAtSyncClear)
                        owner.noteSyncClear();
                    if (consumed > 0 && consumed < remaining) {
                        if (stoppedAtSyncClear) // stopped at a DECSET-2026 clear: a frame is complete
                            owner.publishMidReadSyncFrame(host);
                        else // stopped at reply pressure: drain the queue, then resume
                            owner.flushReplies();
                    }
                }
                host.unlockModel();
            }
            result.bytesDrained += n;
            // The owner fenced the rest of this read: another read would carry
            // the stream past the point it means to hand on.
            if (safePrefix >= 0) {
                result.end = DrainPassEnd::OwnerFence;
                break;
            }
            quietSpins = 0;
            if (!TerminalRuntime::drainPassReadsAgain(result.bytesDrained) || result.bytesDrained >= policy.maxBytes) {
                result.end = DrainPassEnd::ByteBudget;
                break; // the caller's poll loop re-drains the remainder
            }
            // Keep the stack-owned read whole: a lock bite is not an I/O
            // boundary, and returning there would discard its unread suffix.
            if (policy.maxMicroseconds && PAL::monotonicMicros() - startedAt >= policy.maxMicroseconds) {
                result.end = DrainPassEnd::TimeBudget;
                break;
            }
            continue;
        }
        result.endStatus = n;
        if (n == std::to_underlying(PAL::StreamStatus::Again)) {
            // Mid-burst would-block: a flooding writer refills the tty within
            // microseconds. Ending the pass per quantum makes every ~1 KB pay full
            // pass teardown, so ride the burst through a refill the band
            // catches; a pass that starts quiet still returns on its first
            // would-block. A blind re-read takes the kernel tty lock the
            // writer needs to refill, so each retry waits the gap out on the
            // clock first.
            if (result.bytesDrained > 0 && quietSpins < spinBudget) {
                ++quietSpins;
                // steady_clock, not PAL::monotonicMicros: the dwell is
                // sub-microsecond, below that clock's resolution.
                const auto start = std::chrono::steady_clock::now();
                while (static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()) < drainRefillDwellNanoseconds) {
                }
                continue;
            }
            result.didCatchUp = true; // input settled: a non-2026 frame is complete
            result.end = DrainPassEnd::WouldBlock;
            break;
        }
        if (n == std::to_underlying(PAL::StreamStatus::EndOfFile) || n == std::to_underlying(PAL::StreamStatus::Closed)) {
            result.didReachEnd = true;
            result.end = DrainPassEnd::StreamEnd;
            break;
        }
        if (n == std::to_underlying(PAL::StreamStatus::Fatal)) {
            result.didReachEnd = true;
            result.end = DrainPassEnd::StreamEnd;
            result.isFatal = true;
            result.lastError = stream.lastError();
            break;
        }
        break; // any other non-positive status: stop this pass
    }
    return result;
}

} // namespace Ubiquitty::Runtime
