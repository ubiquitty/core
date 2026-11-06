// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_drain.cpp — headless end-to-end drain benchmark: the real PTY +
// TerminalRuntime drain path with no renderer and no main thread. Forkpty's a
// flood child through PAL::openPTY (the production code path), runs the same
// poll/drain loop Runtime::IngestLoop runs (no-op DrainHost locks, a
// counter standing in for outcome delivery), and reports drain MB/s. This is
// the ubench generator's ascii_fill measured from inside the process — renderer and
// observer-chain costs excluded by construction — so drain-loop optimizations
// can be A/B'd here without driving the app.
//
//   bench_drain [--bytes N] [--scenario ascii_fill|scroll_plain] [--pipeline]
//               [--consumer-nap-micros N]
//               [--sim-gap-ns N] [--sim-quantum N]
//               [--render-period-us N] [--render-hold-us N]
//
// The four pacing flags come from Tests/Platform/StreamDouble.h and mean the same
// thing in every bench that adopts it. --sim-gap-ns swaps the real pty for a
// simulated writer with an N-nanosecond refill per quantum, which is the only way
// to sweep the window: on the real transport throughput is pinned at the kernel
// ceiling, so this lane registers regressions but cannot register improvements.
// Sweeping the gap finds the knee where parse stops fitting inside the window,
// and that knee is parse cost per quantum. --render-period-us/--render-hold-us
// give the model lock a real contending thread; leaving them at 0 keeps the
// historical inert host, which pins renderer cost at exactly zero — not a neutral
// choice, since the drain loop takes that lock once per read.
//
// A simulated run is diagnostic only. It omits the read syscall that
// dominates the real cycle, so its MB/s is not comparable to a real-pty figure;
// certification stays `--pipeline --file <1GB file>` on the real transport.
//
// --pipeline prototypes a decoupled ingest: a dedicated reader thread pulls
// the PTY master into a 16 MB single-producer/single-consumer ring and the
// main thread parses out of the ring concurrently, so the read syscalls and
// refill waits overlap parsing instead of serializing with it (the sequential
// drain alternates read / parse on one thread and tops out well under the
// pure-read ceiling).
//
// Build with Scripts/build-perf-bench.sh (shipping flags + the real PTY
// transport the PAL stub does not carry).

#include "Core/Runtime/LaunchCommand.h"
#include "Core/Runtime/TerminalRuntime.h"
#include "PAL/Stream.h"
#include "Platform/StreamDouble.h"

#include <ctime>
#include <poll.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// Write the ascii_fill byte stream (per-row CUP + printable fill, framed like
// the ubench generator's ascii_fill) to `path`. Returns total bytes.
long writeFloodFile(const char* path, const char* scenario, long targetBytes)
{
    std::FILE* f = std::fopen(path, "wb");
    if (!f)
        return -1;
    std::string chunk;
    const int columns = 202, rows = 49;
    if (std::strcmp(scenario, "scroll_plain") == 0) {
        for (int line = 0; line < 200; ++line) {
            for (int x = 0; x < columns; ++x)
                chunk += (char)('a' + (x + line) % 26);
            chunk += "\r\n";
        }
    } else { // ascii_fill
        chunk += "\x1b[H";
        for (int y = 0; y < rows; ++y) {
            chunk += "\x1b[" + std::to_string(y + 1) + ";1H";
            for (int x = 0; x < columns; ++x)
                chunk += (char)('A' + (x + y) % 26);
        }
    }
    long written = 0;
    while (written < targetBytes) {
        std::fwrite(chunk.data(), 1, chunk.size(), f);
        written += (long)chunk.size();
    }
    std::fclose(f);
    return written;
}

// LF count for the pipeline byte-count check. ONLCR expands every written \n
// to \r\n, so the master drains at least fileBytes + LFs. Not exactly:
// ttwrite sleeping at the output-queue high-water mark between an expansion's
// \r and \n re-runs ONLCR on retry and queues a second \r (rarely, and a
// different number each run). The check bounds drained in [fileBytes + LFs,
// fileBytes + 2*LFs]: below is byte loss, above is double-counting; LF-free
// streams (ascii_fill) collapse the bounds to equality. Untimed; runs before
// the flood child spawns.
long countFileNewlines(const char* path)
{
    std::FILE* f = std::fopen(path, "rb");
    if (!f)
        return -1;
    std::vector<char> buffer(1 << 20);
    long newlines = 0;
    size_t n;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), f)) > 0) {
        const char* p = buffer.data();
        const char* end = p + n;
        while ((p = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)))) != nullptr) {
            ++newlines;
            ++p;
        }
    }
    std::fclose(f);
    return newlines;
}

// One microsecond of busy-waiting on the monotonic clock — the reader's
// mid-burst refill wait: cat usually rearms the PTY almost at once, and
// poll(2) is a syscall with no userspace spin phase, so a small timed band in
// front of it serves the reader specifically (the consumer's atomic wait
// brings its own load-only backoff and needs none).
void spinForOneMicrosecond()
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
    }
}

// --pipeline mode. A dedicated reader thread copies the PTY master into a
// contiguous power-of-two ring; the calling (main) thread parses out of the
// ring via feedChunkUpToSyncClear. Single producer, single consumer: head and
// tail are free-running byte positions (index = position & mask) published
// with release and observed with acquire, so each side reads the other's
// position at most once per step and owns its own.
//
// The reader uses runtime.stream()->read() rather than the raw file
// descriptor so it inherits the Stream's EAGAIN/EOF/EIO status mapping and
// its exited-child reaping; that is safe here because in this mode the
// consumer never touches the Stream: feedChunkUpToSyncClear flushes replies
// only at a reply-pressure stop, and these scenarios' streams solicit no
// reply, so the Stream object stays effectively single-threaded. (The
// production pipeline differs: its reader drains an independent duplicate
// endpoint, so the consumer owns the Stream outright and a pressure flush
// is ordinary.) A scenario that solicits replies would need the
// production split, not this borrowed-Stream one.
long runPipelined(Ubiquitty::Runtime::TerminalRuntime& runtime, Ubiquitty::Runtime::DrainHost& host, long& bitesDelivered, int consumerNapMicros, int readerSpinBudget)
{
    constexpr std::uint64_t ringCapacity = 16 * 1024 * 1024; // power of two
    constexpr std::uint64_t ringIndexMask = ringCapacity - 1;
    constexpr int maxBiteBytes = 1024 * 1024; // per-bite parse cap; mirrors readDrainCapacity

    const std::unique_ptr<char[]> ring(new char[ringCapacity]);
    // Each counter on its own 128-byte Apple Silicon cache line (mirrors the
    // production FloodRingControl): adjacent, they false-share — every
    // publish invalidates the line the consumer's loads/waits sit on.
    struct alignas(128) IsolatedPosition {
        std::atomic<std::uint64_t> value { 0 };
    };
    IsolatedPosition producerSlot;
    IsolatedPosition consumerSlot;
    std::atomic<std::uint64_t>& producerPosition = producerSlot.value; // total bytes read off the PTY
    std::atomic<std::uint64_t>& consumerPosition = consumerSlot.value; // total bytes parsed
    // Bit 63 of producerPosition is the reader-done flag (mirrors the
    // production pipeline's floodRingDoneBit): folding it into the counter
    // makes the exit CHANGE the word the consumer's atomic wait parks on, so
    // the wake cannot be lost, and acquiring the bit acquires the final
    // position in the same load.
    constexpr std::uint64_t readerDoneBit = 1ull << 63;
    constexpr std::uint64_t positionMask = readerDoneBit - 1;

    Ubiquitty::PAL::Stream* const stream = runtime.stream();
    const int masterFileDescriptor = stream->fileDescriptor();

    std::thread readerThread([&] {
        // Refill discipline mirrors the production flood pipeline: on EAGAIN
        // spin the bounded µs band (poll is a syscall with no userspace spin
        // phase; the writer usually rearms within a microsecond or two), then
        // park in poll — POLLIN wakes it the instant bytes land. The band's
        // width is --reader-spins; it defaults to production's floodSpinBudget.
        const int microsecondSpinBudget = readerSpinBudget;
        int microsecondSpins = 0;
        for (;;) {
            const std::uint64_t writePosition = producerPosition.load(std::memory_order_relaxed);
            const std::uint64_t readRaw = consumerPosition.load(std::memory_order_acquire);
            const std::uint64_t freeBytes = ringCapacity - (writePosition - readRaw);
            if (freeBytes == 0) {
                // Backpressure: park on the consumer counter (the consumer
                // notifies after every bite) instead of yielding in place.
                consumerPosition.wait(readRaw, std::memory_order_acquire);
                continue;
            }
            // At most the contiguous run to the end of the ring per read call;
            // the wrap is just the next iteration's read.
            const std::uint64_t writeIndex = writePosition & ringIndexMask;
            const std::size_t contiguousBytes = static_cast<std::size_t>(std::min(freeBytes, ringCapacity - writeIndex));
            const int bytesRead = stream->read({ ring.get() + writeIndex, contiguousBytes });
            if (bytesRead > 0) {
                producerPosition.store(writePosition + static_cast<std::uint64_t>(bytesRead), std::memory_order_release);
                producerPosition.notify_one(); // wake a consumer parked on empty; no syscall when nobody waits
                microsecondSpins = 0;
                continue;
            }
            if (bytesRead == static_cast<int>(Ubiquitty::PAL::StreamStatus::Again)) {
                if (microsecondSpins < microsecondSpinBudget) {
                    ++microsecondSpins;
                    spinForOneMicrosecond();
                    continue;
                }
                struct pollfd pfd;
                pfd.fd = masterFileDescriptor;
                pfd.events = POLLIN;
                pfd.revents = 0;
                ::poll(&pfd, 1, 25);
                microsecondSpins = 0;
                continue;
            }
            // EndOfFile / Closed / Fatal: the flood is over either way.
            break;
        }
        producerPosition.fetch_or(readerDoneBit, std::memory_order_release);
        producerPosition.notify_all();
    });

    long bytesDrained = 0;
    for (;;) {
        const std::uint64_t readPosition = consumerPosition.load(std::memory_order_relaxed);
        const std::uint64_t writeRaw = producerPosition.load(std::memory_order_acquire);
        const std::uint64_t availableBytes = (writeRaw & positionMask) - readPosition;
        if (availableBytes == 0) {
            if (writeRaw & readerDoneBit)
                break; // done bit and final position arrive in the same word: truly drained
            if (consumerNapMicros > 0) {
                // A/B lane (--consumer-nap-micros): timed nap instead of the
                // atomic-wait park, so a wake takes a batch of bytes rather
                // than every republish. Done bit is re-polled each wake; no
                // lost-wake hazard.
                const struct timespec napDuration = { 0, static_cast<long>(consumerNapMicros) * 1000 };
                ::nanosleep(&napDuration, nullptr);
                continue;
            }
            // Park on the producer counter directly — libc++'s atomic wait
            // spins its own cheap load-only backoff before the futex, so the
            // µs republish is still caught without a hand-rolled probe band.
            // Publish and exit both change the word and notify.
            producerPosition.wait(writeRaw, std::memory_order_acquire);
            continue;
        }
        const std::uint64_t readIndex = readPosition & ringIndexMask;
        const std::uint64_t contiguousBytes = std::min(availableBytes, ringCapacity - readIndex);
        const int biteBytes = static_cast<int>(std::min(contiguousBytes, static_cast<std::uint64_t>(maxBiteBytes)));
        // Same lock/frame structure as drainAvailable: parse under the model
        // lock, surfacing each completed DECSET-2026 frame, and keep feeding
        // the remainder of the bite past every sync clear.
        host.lockModel();
        int offset = 0;
        while (offset < biteBytes) {
            const Ubiquitty::Runtime::FeedResult fed = runtime.feedChunkUpToSyncClear({ ring.get() + readIndex + offset, static_cast<std::size_t>(biteBytes - offset) });
            offset += (fed.consumed > 0) ? fed.consumed : (biteBytes - offset); // guard: never spin
            if (fed.reachedSyncClear)
                host.onSyncFrameReadyLocked(Ubiquitty::Runtime::SyncFrameReason::Boundary);
        }
        host.unlockModel();
        consumerPosition.store(readPosition + static_cast<std::uint64_t>(biteBytes), std::memory_order_release);
        consumerPosition.notify_one(); // wake a reader parked on ring-full
        bytesDrained += biteBytes;
        ++bitesDelivered;
    }
    readerThread.join();
    return bytesDrained;
}

} // namespace

int main(int argc, char** argv)
{
    long targetBytes = 256L * 1024 * 1024;
    const char* scenario = "ascii_fill";
    const char* floodFile = nullptr; // --file: drain an existing file via cat
    int spinBudget = 16; // mirrors production; sweep with --spins
    bool pipelined = false; // --pipeline: reader thread + SPSC ring, parser decoupled
    int consumerNapMicros = 0; // --consumer-nap-micros: 0 = atomic-wait park (A/B lane)
    int readerSpinBudget = 4; // --reader-spins: the pipelined reader's µs probe band; mirrors floodSpinBudget
    Ubiquitty::TestDoubles::PacingOptions pacing;
    for (int i = 1; i < argc; ++i) {
        if (pacing.consumeArgument(argc, argv, i))
            continue;
        if (!std::strcmp(argv[i], "--bytes") && i + 1 < argc)
            targetBytes = std::atol(argv[++i]);
        else if (!std::strcmp(argv[i], "--spins") && i + 1 < argc)
            spinBudget = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--scenario") && i + 1 < argc)
            scenario = argv[++i];
        else if (!std::strcmp(argv[i], "--file") && i + 1 < argc)
            floodFile = argv[++i];
        else if (!std::strcmp(argv[i], "--pipeline"))
            pipelined = true;
        else if (!std::strcmp(argv[i], "--consumer-nap-micros") && i + 1 < argc)
            consumerNapMicros = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--reader-spins") && i + 1 < argc)
            readerSpinBudget = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2; // fatal: a silently ignored flag once invalidated a sweep
        }
    }

    const char* floodPath = "/tmp/bench_drain_flood.dat";
    long floodBytes = 0; // pipeline mode verifies drained == file size
    if (floodFile) {
        floodPath = floodFile;
        scenario = "file";
        struct stat floodStat;
        if (::stat(floodPath, &floodStat) != 0) {
            std::fprintf(stderr, "cannot stat flood file %s\n", floodPath);
            return 1;
        }
        floodBytes = static_cast<long>(floodStat.st_size);
    } else {
        floodBytes = writeFloodFile(floodPath, scenario, targetBytes);
        if (floodBytes < 0) {
            std::fprintf(stderr, "cannot write flood file\n");
            return 1;
        }
    }

    // ONLCR accounting for the pipeline byte-count check (see countFileNewlines).
    const long floodNewlines = countFileNewlines(floodPath);
    if (floodNewlines < 0) {
        std::fprintf(stderr, "cannot read flood file %s\n", floodPath);
        return 1;
    }
    const long expectedDrainMinimum = floodBytes + floodNewlines;
    const long expectedDrainMaximum = floodBytes + 2 * floodNewlines;

    std::unique_ptr<Ubiquitty::PAL::Stream> stream;
    long expectedDrainLow = expectedDrainMinimum;
    long expectedDrainHigh = expectedDrainMaximum;
    if (pacing.simulatesWriter()) {
        // A simulated writer replays the flood bytes verbatim: no tty line
        // discipline, so no ONLCR expansion and the drained count is exact.
        std::FILE* f = std::fopen(floodPath, "rb");
        if (!f) {
            std::fprintf(stderr, "cannot read flood file %s\n", floodPath);
            return 1;
        }
        std::vector<char> payload(static_cast<std::size_t>(floodBytes));
        const std::size_t got = std::fread(payload.data(), 1, payload.size(), f);
        std::fclose(f);
        if (got != payload.size()) {
            std::fprintf(stderr, "short read on flood file %s\n", floodPath);
            return 1;
        }
        stream = pacing.makeStream(std::move(payload));
        expectedDrainLow = expectedDrainHigh = floodBytes;
    } else {
        // exec cat directly (no login shell): the child floods the PTY replica as
        // fast as the master drains it, then exits -> EOF ends the run.
        std::vector<std::string> arguments = { "/bin/cat", floodPath };
        stream = Ubiquitty::PAL::openPTY(arguments, {});
    }
    if (!stream) {
        std::fprintf(stderr, "%s failed\n", pacing.simulatesWriter() ? "simulated stream" : "openPTY");
        return 1;
    }

    Ubiquitty::Runtime::TerminalRuntime runtime;
    Ubiquitty::Runtime::RuntimeInitParameters parameters;
    parameters.rows = 49;
    parameters.columns = 202;
    parameters.allowReflowOnInit = false;
    runtime.init(parameters);
    auto adoption = runtime.adoptStream(std::move(stream));
    if (!adoption) {
        stream = std::move(adoption.error());
        std::fprintf(stderr, "runtime refused the benchmark's stream\n");
        return 1;
    }

    const std::unique_ptr<Ubiquitty::TestDoubles::DrainHostDouble> hostOwner = pacing.makeHost();
    Ubiquitty::TestDoubles::DrainHostDouble& host = *hostOwner;
    long deliveries = 0;
    long bytesDrained = 0;

    if (pipelined) {
        const auto t0 = std::chrono::steady_clock::now();
        bytesDrained = runPipelined(runtime, host, deliveries, consumerNapMicros, readerSpinBudget);
        const auto t1 = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(t1 - t0).count();
        std::printf("%-14s %10.1f MB/s  (%ld bytes, %.2f s, %ld ring bites, pipelined)  [%s]\n", scenario, (double)bytesDrained / 1e6 / seconds, bytesDrained, seconds, deliveries, pacing.label().c_str());
        if (bytesDrained < expectedDrainLow || bytesDrained > expectedDrainHigh) {
            std::fprintf(stderr, "MISMATCH: drained %ld bytes, expected [%ld, %ld] (file %ld + %ld ONLCR-expanded LFs, hiwat-retry \\r duplicates above that)\n", bytesDrained, expectedDrainLow, expectedDrainHigh, floodBytes, floodNewlines);
            return 1;
        }
        return 0;
    }

    // The bridge's ingestLoop, minus ObjC: drain until quiet, then poll.
    struct pollfd pfd;
    pfd.fd = runtime.stream()->fileDescriptor();
    pfd.events = POLLIN;
    const auto t0 = std::chrono::steady_clock::now();
    bool shouldDrain = true;
    int quietSpins = 0; // yield-and-retry budget before falling back to poll
    for (;;) {
        if (shouldDrain) {
            shouldDrain = false;
            const Ubiquitty::Runtime::DrainOutcome out = runtime.drainAvailable(host);
            if (out.bytesDrained > 0 || out.bell != Ubiquitty::BellMode::Off || out.reachedEnd) {
                ++deliveries; // stands in for deliverDrainOutcome's main hop
                bytesDrained += out.bytesDrained;
            }
            if (out.reachedEnd)
                break;
            if (out.bytesDrained > 0) {
                shouldDrain = true;
                quietSpins = 0;
                continue;
            }
            // Quiet pass right after data was flowing: the writer usually
            // refills almost at once; yield instead of paying a poll
            // sleep/wake per chunk.
            if (quietSpins < spinBudget) {
                ++quietSpins;
                ::sched_yield();
                shouldDrain = true;
                continue;
            }
        }
        // A simulated stream has no descriptor to park on: poll would just burn
        // its whole timeout per quiet gap, so spin the lane instead.
        if (pfd.fd < 0) {
            shouldDrain = true;
            quietSpins = 0;
            continue;
        }
        pfd.revents = 0;
        const int pr = ::poll(&pfd, 1, 25);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            shouldDrain = true;
        quietSpins = 0;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(t1 - t0).count();

    std::printf("%-14s %10.1f MB/s  (%ld bytes, %.2f s, %ld drain deliveries)  [%s]\n", scenario, (double)bytesDrained / 1e6 / seconds, bytesDrained, seconds, deliveries, pacing.label().c_str());
    return 0;
}
