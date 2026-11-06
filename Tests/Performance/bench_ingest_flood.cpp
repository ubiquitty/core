// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_ingest_flood.cpp — throughput of the SHIPPING flood path: a real
// TerminalSession, its IngestLoop, and the reader/ring/bite pipeline that loop
// arms. Forkpty's a flood child through the session's own launch, runs the
// session's own ingest thread, and reports the rate the loop drained it at.
//
//   bench_ingest_flood [--megabytes N] [--rows N] [--columns N] [--runs N]
//                      [--scenario ascii_fill|scroll_plain] [--file PATH]
//                      [--presented-hz N]
//                      [--find TEXT]
//
// Distinct from bench_drain, which builds a TerminalRuntime and a reader loop
// of its own: its --pipeline flag measures that loop, and UBIQUITTY_SWIFT_FLOOD
// reaches neither. This one constructs the session, so the variable is live
// here and pairs the arms on one binary:
//
//   UBIQUITTY_SWIFT_FLOOD=1 bench_ingest_flood   # reader thread + ring
//   UBIQUITTY_SWIFT_FLOOD=0 bench_ingest_flood   # sequential passes
//
// Both arms parse the same bytes through the same Core; what differs is
// whether the read syscalls and kernel refill gaps overlap the parse. The
// figure is a floor rather than a ceiling: a real pty pins throughput at the
// kernel's rate, so this lane registers a regression more readily than an
// improvement, and the pair is what carries meaning rather than either number.
//
// --presented-hz N runs the session as a focused pane on an N Hz display: the
// boundary capture armed and presented, the publish interval the frame clock
// sets for that rate, and each capture taken off the slot on the bench's 2 ms
// poll, which a drive at the panel rate never outpaces. A headless session
// leaves the capture unarmed, which keeps every pass on the byte budget; a
// presented one also ends a pass at its frame interval and copies the grid for
// every frame it publishes, so only this lane measures the flood a person is
// watching.

#include "BenchHarness.h"

#include "Core/Runtime/TerminalSession.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

using namespace Ubiquitty::Runtime;
using namespace Ubiquitty::Bench;

namespace {

constexpr std::uint32_t exitedBit = 1u << 1;

// Owns the session for the run, so an early return still closes the pty and
// joins the ingest thread.
struct SessionHandle {
    TerminalSession* session;
    SessionHandle()
        : session(TerminalSession::make())
    {
    }
    ~SessionHandle()
    {
        session->close();
        releaseTerminalSession(session);
    }
};

// Writes the flood payload to a file the child cats. The producer has to be
// cheaper than the consumer or the lane reports the producer: a generator in
// the pipe (tr over /dev/urandom) holds the loop far below what the same loop
// drains from a file. Both scenarios push the same byte count and differ in
// how many rows the parse retires.
std::string writeFloodPayload(const std::string& scenario, long megabytes, std::string& path)
{
    const long bytes = megabytes * 1024 * 1024;
    path = "/tmp/ubq-bench-ingest-flood-" + scenario + "-" + std::to_string(megabytes) + ".txt";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return {};
    // 79 columns and a newline scrolls a row every 80 bytes; one unbroken line
    // lets the run writer batch and the grid wrap instead.
    std::string unit;
    if (scenario == "scroll_plain") {
        unit.assign(79, 'x');
        unit.push_back('\n');
    } else {
        unit.assign(4096, 'x');
    }
    long written = 0;
    while (written < bytes) {
        const std::size_t take = static_cast<std::size_t>(bytes - written) < unit.size()
            ? static_cast<std::size_t>(bytes - written)
            : unit.size();
        if (std::fwrite(unit.data(), 1, take, file) != take) {
            std::fclose(file);
            return {};
        }
        written += static_cast<long>(take);
    }
    std::fclose(file);
    return "cat " + path;
}

// The geometry a presented pane registers, at the grid's size: the capture
// copies every row whatever the metrics, so only a plausible cell size and
// viewport are needed.
SyncFrameCaptureGeometry presentedGeometry(int rows, int columns)
{
    SyncFrameCaptureGeometry geometry;
    geometry.metrics.cellWidth = 8.0f;
    geometry.metrics.cellHeight = 16.0f;
    geometry.metrics.baselineFromTop = 13.0f;
    geometry.metrics.underlineThickness = 1.0f;
    geometry.metrics.underlineDrop = 1.0f;
    geometry.metrics.strikeThickness = 1.0f;
    geometry.metrics.xHeight = 10.0f;
    geometry.glyphSlot = { /*columns=*/32, /*slotWidthInPixels=*/8, /*slotHeightInPixels=*/16,
        /*glyphWidthInPixels=*/6, /*glyphMarginInPixels=*/ { 1, 1 } };
    geometry.viewportSizeInPixels = { 8.0f * static_cast<float>(columns), 16.0f * static_cast<float>(rows) };
    return geometry;
}

// Polls until the child's exit reaches the flags, accumulating what the loop
// drained, and takes each published capture as the frame drive would.
// Returns the byte count; `didExit` reports whether the budget held, and
// `framesTaken` counts the captures taken.
std::uint64_t drainUntilExit(TerminalSession& session, int budgetMilliseconds, bool& didExit, long& framesTaken)
{
    std::uint64_t drained = 0;
    std::uint32_t flags = 0;
    didExit = false;
    for (int waited = 0; waited < budgetMilliseconds; waited += 2) {
        flags |= session.takeEventFlags();
        drained += session.takeDrainedBytes();
        if (Ubiquitty::Rendering::FrameCapture* frame = session.takeSyncFrame()) {
            ++framesTaken;
            releaseFrameCapture(frame);
        }
        if (flags & exitedBit) {
            didExit = true;
            break;
        }
        ::usleep(2 * 1000);
    }
    flags |= session.takeEventFlags();
    drained += session.takeDrainedBytes();
    didExit = didExit || (flags & exitedBit);
    return drained;
}

} // namespace

int main(int argc, char** argv)
{
    long megabytes = 64;
    int rows = 24;
    int columns = 80;
    int runs = 3;
    std::string scenario = "ascii_fill";
    const char* floodFile = nullptr;
    int presentedHz = 0;
    std::string findPattern;
    for (int i = 1; i < argc; ++i) {
        auto matches = [&](const char* flag) { return std::strcmp(argv[i], flag) == 0; };
        if (matches("--megabytes") && i + 1 < argc)
            megabytes = std::atol(argv[++i]);
        else if (matches("--rows") && i + 1 < argc)
            rows = std::atoi(argv[++i]);
        else if (matches("--columns") && i + 1 < argc)
            columns = std::atoi(argv[++i]);
        else if (matches("--runs") && i + 1 < argc)
            runs = std::atoi(argv[++i]);
        else if (matches("--scenario") && i + 1 < argc)
            scenario = argv[++i];
        else if (matches("--file") && i + 1 < argc)
            floodFile = argv[++i];
        else if (matches("--presented-hz") && i + 1 < argc)
            presentedHz = std::atoi(argv[++i]);
        else if (matches("--find") && i + 1 < argc)
            findPattern = argv[++i];
        else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    const char* floodKnob = std::getenv("UBIQUITTY_SWIFT_FLOOD");
    const bool isPipelineDisarmed = floodKnob && floodKnob[0] == '0';
    std::string payloadPath;
    // An existing capture stands in for the generated payload, so a shape this
    // bench does not generate can still be put through the loop.
    const std::string command = floodFile ? (std::string("cat ") + floodFile)
                                          : writeFloodPayload(scenario, megabytes, payloadPath);
    if (command.empty()) {
        std::fprintf(stderr, "bench_ingest_flood: the payload could not be written\n");
        return 1;
    }

    // One call is one flood: launch, drain to the child's exit, tear down. The
    // harness repeats the call rather than a region inside it, so Repeat::Once
    // — a forkpty flood already runs far past any clock granularity, and
    // repeating it would only multiply the pty's own cost.
    std::uint64_t drainedBytes = 0;
    long floodBursts = 0;
    long framesTaken = 0;
    std::uint64_t screenPublications = 0;
    bool didFail = false;
    const Timing timing = measure(1, runs, [&] {
        if (didFail)
            return;
        SessionHandle handle;
        TerminalSession& session = *handle.session;
        session.start(rows, columns);
        // As a frontend applies the preferences to a new pane: the terminal
        // seeds from them, password detection among the seeds, and that
        // detection is a tty ioctl wherever the runtime settles it. A bench
        // that skips the step measures a parse the app never runs.
        session.applyTerminalPreferences(Ubiquitty::Preferences::values());
        // As a frontend arms it at session start: the watch judges every byte
        // the terminal receives, so a bench that leaves it unarmed measures a
        // path the app never runs.
        session.armFileTransferWatch();
        std::uint64_t initialScreenGeneration = 0;
        if (!findPattern.empty()) {
            const auto status = session.searchScrollbackAndWait({ .pattern = findPattern });
            if (!status.isComplete || status.isPatternInvalid) {
                didFail = true;
                return;
            }
            initialScreenGeneration = status.screenGeneration;
        }
        // As the frame clock and the renderer arm a presented pane: the
        // publish interval for the panel's rate, then the capture geometry,
        // which arms the capture for a pane that is presented by default.
        if (presentedHz > 0) {
            session.setSyncPublishRefreshSeconds(1.0 / presentedHz);
            session.setSyncFrameCaptureGeometry(presentedGeometry(rows, columns));
        }
        if (!session.launchShellCommand(command.c_str())) {
            didFail = true;
            return;
        }
        if (!session.startIngest()) {
            didFail = true;
            return;
        }
        bool didExit = false;
        framesTaken = 0;
        const std::uint64_t drained = drainUntilExit(session, 120'000, didExit, framesTaken);
        session.stopIngest();
        screenPublications = session.searchStatus().screenGeneration - initialScreenGeneration;
        const std::uint64_t expected = floodFile ? 0 : static_cast<std::uint64_t>(megabytes) * 1024 * 1024 / 2;
        if (!didExit || drained < expected) {
            didFail = true;
            return;
        }
        drainedBytes = drained;
        floodBursts = session.floodBurstCount(); }, Repeat::Once);

    if (!floodFile)
        ::unlink(payloadPath.c_str());
    if (didFail) {
        std::fprintf(stderr, "bench_ingest_flood: a flood did not launch, finish, or reach the loop\n");
        return 1;
    }

    // The burst count rides in the lane name, because it is the arm's own
    // witness: an armed run carries at least one and a disarmed run carries
    // none, so a pair reporting the same count on both sides measured one arm
    // twice rather than two arms once.
    std::vector<Result> results;
    const std::string presentedSuffix = presentedHz > 0 ? "_presented_" + std::to_string(presentedHz) + "hz" : "";
    const std::string searchSuffix = findPattern.empty() ? "" : "_find_open";
    results.push_back(throughput(std::string("ingest_flood_") + scenario + presentedSuffix + searchSuffix, static_cast<double>(drainedBytes), timing));
    std::fprintf(stderr, "%-14s pipeline=%s  %ld flood burst(s)",
        scenario.c_str(), isPipelineDisarmed ? "disarmed" : "armed", floodBursts);
    if (presentedHz > 0)
        std::fprintf(stderr, "  presented at %d Hz, %ld frames taken", presentedHz, framesTaken);
    if (!findPattern.empty())
        std::fprintf(stderr, "  %llu screen search publications", static_cast<unsigned long long>(screenPublications));
    std::fprintf(stderr, "\n");
    // An armed run that carried no burst never entered the ring, so its figure
    // answers for the sequential loop under a name that says otherwise. The
    // disarmed arm is the one that is supposed to carry none. A presented
    // pane's lane names no path: which one its flood takes is what it
    // measures, and the burst count above says which.
    if (!isPipelineDisarmed && presentedHz == 0 && !witnessPathTaken(results.front().name.c_str(), "the flood ring", floodBursts))
        return 1;
    if (!findPattern.empty() && !witnessPathTaken(results.front().name.c_str(), "screen search maintenance", screenPublications))
        return 1;
    emitJSON(results, "core", std::getenv("UBIQUITTY_BENCH_COMMIT"));
    return 0;
}
