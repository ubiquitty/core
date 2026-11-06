// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// bench_protocol.cpp — the tmux control-mode and sync-client parse paths.
//
// Why this exists. Both paths take server bytes a chunk at a time and walk them
// with a caret. `bench_core` and `bench_drain` measure the VT parser and the
// drain cycle; the protocol scanners sit upstream of both, so without this a
// change to their signatures or their bounds checks has nothing to answer to.
// The lanes exist for a number to compare against, not from a suspicion that
// the scanners are slow.
//
// What it measures. Three lanes, each fed a corpus built once and reused:
//
//   tmux_scan     — the caret scanners (scanUInt, expectCharacter) over a
//                   pane-record line. The tightest loop in the tmux path.
//   tmux_record   — parsePaneRecord over a full list-panes body, which is what
//                   a pane enumeration actually costs.
//   sync_feed_notify — SyncClientCore::feed over a canonical Welcome and Notify
//                      records, chunked as a transport delivers them.
//
// A lane that parses nothing measures nothing. Each lane asserts its corpus
// produced the record count it expects before reporting, so a scanner that
// starts rejecting every line reports a fast lane rather than a fast lane and a
// silent behaviour change. The count is checked, not the timing.
//
// `sync_feed_notify` measures the canonical notification corpus with its
// admission cost, so its numbers compare only against its own baseline.
//
// `tmux_scan` has no stable baseline. Its spread is wide at every corpus and
// iteration count tried, because the scanners barely touch memory and the lane
// is pure ALU: it reads whatever the core's frequency and residency happen to
// be. Read it for a factor, never for a percent.

// A lane faster than the clock measures the clock: one pass of the scanner
// corpus is a handful of ticks, so the figure is rounded. `Bench::measure` owns the
// fix: it grows a short region until it clears the floor and divides, and
// warns when a median still lands on a tick multiple. These lanes hand it a
// plain closure and let it do that, rather than repeating by hand and reaching
// the right answer by a route the harness cannot keep correct.

#include "BenchHarness.h"

#include "Core/Protocol/Control/PastePush.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/SyncClientCore.h"
#include "Core/Protocol/Tmux/Tmux.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace Ubiquitty::Bench;
namespace Sync = Ubiquitty::Protocol::Sync;

// A paste-credit notify: a kind every client reads, with a request id about
// as long as a title.
const std::vector<std::uint8_t> notification = *Ubiquitty::Protocol::Control::pasteReadyNotify({ "a title's worth of bytes", 7, 9 });

void greet(Sync::SyncClientCore& core, std::vector<std::uint8_t>& output)
{
    core.setClock([] { return 1; });
    core.setClientInstance({ 3, 4 });
    Sync::HandshakeRequest request;
    request.sessionID = 7;
    request.expectedDaemonInstanceID = 17;
    request.sessionFlags = Sync::helloSessionAttachOnly;
    core.buildHello(request, output);
}

// A list-panes body of `rows` records, the shape `parsePaneRecord` consumes:
// `@<win> %<pane> <w> <h> <active> <layout> <session> <window_name>`.
std::vector<std::string> paneRecords(int rows)
{
    std::vector<std::string> lines;
    lines.reserve(static_cast<std::size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        char line[256];
        std::snprintf(line, sizeof(line), "@%d %%%d %d %d %d %dx%d,0,0,%d main-%d window %d",
            i % 8, i, 80 + (i % 40), 24 + (i % 12), i % 2,
            80 + (i % 40), 24 + (i % 12), i, i % 4, i);
        lines.emplace_back(line);
    }
    return lines;
}

int runTmuxScan(const std::vector<std::string>& lines, int iters, std::vector<Result>& out)
{
    std::size_t bytes = 0;
    for (const auto& line : lines)
        bytes += line.size();

    long long parsed = 0;
    auto onePass = [&] {
        for (const auto& line : lines) {
            int caret = 0;
            int windowID = 0;
            int paneID = 0;
            // The two scanners, in the order the record parser calls them.
            if (Ubiquitty::Protocol::Tmux::expectCharacter(line, caret, '@')
                && Ubiquitty::Protocol::Tmux::scanUInt(line, caret, windowID)
                && Ubiquitty::Protocol::Tmux::expectCharacter(line, caret, ' ')
                && Ubiquitty::Protocol::Tmux::expectCharacter(line, caret, '%')
                && Ubiquitty::Protocol::Tmux::scanUInt(line, caret, paneID))
                ++parsed;
            doNotOptimize(caret);
        }
        doNotOptimize(parsed);
    };
    const Timing timing = Ubiquitty::Bench::measure(3, iters, onePass);

    // The harness repeats the region internally, so the pass count is its
    // business; what this lane must know is that some work happened.
    const long long wanted = static_cast<long long>(lines.size());
    if (parsed < wanted) {
        std::fprintf(stderr, "bench_protocol: FAILED — tmux_scan parsed %lld of %lld line(s); "
                             "a lane that rejects its corpus measures rejection, not scanning\n",
            parsed, wanted);
        return 1;
    }
    out.push_back(throughput("tmux_scan", static_cast<double>(bytes), timing));
    return 0;
}

int runTmuxRecord(const std::vector<std::string>& lines, int iters, std::vector<Result>& out)
{
    std::size_t bytes = 0;
    for (const auto& line : lines)
        bytes += line.size();

    long long parsed = 0;
    auto onePass = [&] {
        for (const auto& line : lines) {
            int windowID = 0, width = 0, height = 0, active = 0;
            Ubiquitty::Protocol::Tmux::PaneID paneID = 0;
            std::string layout, session, windowName;
            if (Ubiquitty::Protocol::Tmux::parsePaneRecord(line,
                    windowID, paneID, width, height, active,
                    layout, session, windowName))
                ++parsed;
            doNotOptimize(layout);
            doNotOptimize(session);
            doNotOptimize(windowName);
        }
        doNotOptimize(parsed);
    };
    const Timing timing = Ubiquitty::Bench::measure(3, iters, onePass);

    // The harness repeats the region internally, so the pass count is its
    // business; what this lane must know is that some work happened.
    const long long wanted = static_cast<long long>(lines.size());
    if (parsed < wanted) {
        std::fprintf(stderr, "bench_protocol: FAILED — tmux_record parsed %lld of %lld record(s); "
                             "a lane that rejects its corpus measures rejection, not parsing\n",
            parsed, wanted);
        return 1;
    }
    out.push_back(throughput("tmux_record", static_cast<double>(bytes), timing));
    return 0;
}

// A real in-memory host admits the fixture-owned session before sending any
// records. A removed opaque tag must not turn this lane into refusal timing.
std::vector<std::uint8_t> notificationFrames(int count)
{
    Sync::SyncClientCore client(nullptr, nullptr);
    std::vector<std::uint8_t> greeting;
    greet(client, greeting);
    Sync::HostConnection host({}, Sync::HostIdentity { .hostInstanceID = { 11, 13 }, .attachmentID = { 19, 23 }, .resumeSecret = { 29, 31 }, .principal = "benchmark", .mayDrive = true, .selection = { .target = Sync::AdmissionTarget::Session, .sessionID = 7, .daemonInstanceID = 17, .socketName = "benchmark.sock", .hostPlatform = "fixture" } });
    host.receiveStream(greeting, 1);
    if (host.state() != Sync::ConnectionState::Attached)
        return {};
    std::vector<std::uint8_t> wire;
    for (int i = 0; i < count; ++i) {
        if (!host.sendNotify(notification))
            return {};
    }
    std::vector<std::uint8_t> frame;
    while (host.popStreamFrame(frame, 1)) {
        wire.insert(wire.end(), frame.begin(), frame.end());
        frame.clear();
    }
    return wire;
}

int runSyncFeed(const std::vector<std::uint8_t>& wire, int frames, int chunk, int iters, std::vector<Result>& out)
{
    bool hasFailed = wire.empty();
    const Timing timing = Ubiquitty::Bench::measure(3, iters, [&] {
        Ubiquitty::Terminal terminal;
        Ubiquitty::resize(terminal, 24, 80);
        Sync::SyncClientCore core(&terminal, nullptr);
        std::vector<std::uint8_t> greeting;
        greet(core, greeting);
        for (std::size_t offset = 0; offset < wire.size(); offset += static_cast<std::size_t>(chunk)) {
            const std::size_t take = std::min(static_cast<std::size_t>(chunk), wire.size() - offset);
            if (!core.feed(std::as_bytes(std::span { wire.data() + offset, take })))
                hasFailed = true;
        }
        const auto records = core.takeHostNotifies();
        if (!core.isScreenAttached() || core.faulted() || records.size() != static_cast<std::size_t>(frames)
            || !std::ranges::all_of(records, [](const Sync::Notify& record) {
                   const auto ready = Ubiquitty::Protocol::Control::decodePasteReady(record.record);
                   return record.kind == Sync::NotifyKind::PasteReady && ready && ready->transfer == 9;
               }))
            hasFailed = true;
        std::size_t delivered = records.size();
        doNotOptimize(delivered);
    });

    if (hasFailed) {
        std::fprintf(stderr, "bench_protocol: FAILED — sync_feed_notify did not deliver every canonical record; "
                             "a decoder that refuses or drops its corpus measures neither delivery nor decode\n");
        return 1;
    }
    out.push_back(throughput("sync_feed_notify", static_cast<double>(wire.size()), timing));
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    int iters = 2000;
    int rows = 1024;
    int frames = 256;
    int chunk = 1024;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
            iters = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--rows") == 0 && i + 1 < argc)
            rows = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--chunk") == 0 && i + 1 < argc)
            chunk = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: bench_protocol [--iters N] [--rows N] [--frames N] [--chunk N]\n");
            return 0;
        }
    }

    if (iters <= 0 || iters > 1'000'000 || rows <= 0 || rows > 100'000
        || frames <= 0 || static_cast<std::size_t>(frames) > Sync::maxUnreadNotifies
        || chunk <= 0 || chunk > (1 << 20)) {
        std::fprintf(stderr, "bench_protocol: counts must be positive and bounded; --frames cannot exceed %zu\n", Sync::maxUnreadNotifies);
        return 2;
    }

    const std::vector<std::string> lines = paneRecords(rows);
    std::vector<Result> results;
    if (runTmuxScan(lines, iters, results) != 0)
        return 1;
    if (runTmuxRecord(lines, iters, results) != 0)
        return 1;
    if (runSyncFeed(notificationFrames(frames), frames, chunk, iters, results) != 0)
        return 1;

    Ubiquitty::Bench::emitJSON(results, "core", buildFlavour());
    return 0;
}
