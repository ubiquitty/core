// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/foreground_probe_budget.cpp — probeForegroundIfDue probes the
// expensive foregroundIsShell()/foregroundName() pair (tty ioctls + process
// sysctls + string allocations) on a tick budget, never per broadcast frame.
// The cheap foregroundProcessGroup() ioctl is the change-detector:
//   1. A tight loop of N publishFrame calls on a settled session probes
//      at most the budgeted number of times (first probe + one per elapsed
//      budget window), not N times.
//   2. A foreground-group change forces an immediate re-probe — inside the
//      budget window — and the authoritative PaneFacts source reflects the
//      new foreground name + shell flag.
//   3. An unsettled probe (group 0 / empty name — the pre-exec window right
//      after forkpty) never latches the budget: every following broadcast
//      re-probes until a real foreground shows, and only then does the
//      budget pin.
//   4. A program that takes the terminal without printing reaches a reader:
//      a process-tree edge wakes the loop and the probe answers once the tree
//      has settled, and where the tracker raises no edges a read pane is
//      probed on the fallback cadence. Without either, no frame asks.
//   5. A pane nobody reads is probed by no frame, however often it writes,
//      and its frame is taken in the pass that read the write, owing no wake
//      of its own.
//
// Cases 1-3 give the pane a listing reader, since only a read pane's frames
// probe.
//
// Friend-seam pattern (RowDeltaBaselineAccess's twin): a real Daemon +
// fully-wired Session driving the real publishFrame, with a stub
// ProcessFacts counting foregroundName() calls. Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Records.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Title/WalkPacing.h"
#include "PAL/Stream.h"
#include "Platform/Host/Daemon.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <utility>

namespace Ubiquitty {
namespace Host {

// The daemon-side probe budget (Daemon.cpp foregroundProbeIntervalMilliseconds).
constexpr std::uint64_t probeBudgetMilliseconds = 50;

// A stream at EOF whose ProcessFacts is scriptable and counts the expensive
// foregroundName() probes. publishFrame never reads the stream itself.
class CountingStream final : public PAL::Stream {
public:
    class Facts final : public PAL::ProcessFacts {
    public:
        int nameProbeCount = 0;
        int directoryProbeCount = 0;
        std::optional<PAL::ProcessID> group;
        std::string name;
        bool isShell = true;

        std::string foregroundName() override
        {
            ++nameProbeCount;
            return name;
        }
        bool foregroundIsShell() override { return isShell; }
        std::optional<PAL::ProcessID> foregroundProcessGroup() override { return group; }
        std::string foregroundCurrentDirectory() override
        {
            ++directoryProbeCount;
            return {};
        }
        std::string ttyName() override { return {}; }
        bool hasRunningChild() override { return false; }
        int gatherProcesses(std::span<PAL::Process::Record>) override { return 0; }
        bool deliversTreeEdges() override { return isEdgeTracked; }
        void setTreeChangeNotifier(bool (*notifyFunction)(void*), void* context, void (*releaseFunction)(void*)) override
        {
            notify = notifyFunction;
            notifyContext = context;
            releaseContext = releaseFunction;
        }

        // What a tracker's watcher thread would do on a fork, exec or exit.
        void raiseTreeEdge() const
        {
            if (notify)
                (void)notify(notifyContext);
        }

        Facts() = default;
        Facts(const Facts&) = delete;
        Facts& operator=(const Facts&) = delete;
        ~Facts() override
        {
            if (releaseContext)
                releaseContext(notifyContext);
        }

        bool isEdgeTracked = false;
        bool (*notify)(void*) = nullptr;
        void* notifyContext = nullptr;
        void (*releaseContext)(void*) = nullptr;
    };

    int read(std::span<char>) override { return static_cast<int>(PAL::StreamStatus::EndOfFile); }
    int write(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    int writeAll(std::span<const char> bytes) override { return static_cast<int>(bytes.size()); }
    bool drainPendingWrite() override { return false; }
    bool hasPendingWrite() const override { return false; }
    void setWindowSize(int, int, int, int) override { }
    int lastError() const override { return 0; }
    int fileDescriptor() const override { return -1; }
    PAL::ProcessFacts& process() override { return facts; }

    Facts facts;
};

// Friend seam: a Daemon + a wired Session (Terminal + the counting stub, no real
// PTY/fork). Record assertions read the canonical source after its normal
// cadence has been serviced; typed transport is covered by the pure record suite.
struct ForegroundProbeBudgetAccess {
    static Daemon::Session& makeSession(Daemon& d, std::unique_ptr<PAL::Stream> stream, int rows, int columns)
    {
        auto s = std::make_unique<Daemon::Session>();
        s->id = d.m_nextSessionID++;
        s->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*s->term, rows, columns);
        s->stream = std::move(stream);
        s->screenSource = std::make_unique<Protocol::Sync::PaneSource>(*s->term, Protocol::Sync::ID128 { .low = s->id, .high = 1 });
        Daemon::Session& ref = *s;
        d.m_sessions.push_back(std::move(s));
        return ref;
    }
    static void broadcast(Daemon& d, Daemon::Session& s) { d.publishFrame(s, Runtime::FrameClass::Settled); }
    static std::optional<Runtime::FrameClass> mayPublish(Daemon& d, Daemon::Session& s, std::uint64_t now) { return d.mayPublishFrame(s, now); }
    // A connection listing the host, which is what makes a pane's facts read.
    static void addListingReader(Daemon& d)
    {
        Daemon::Client c {};
        c.clientID = ++d.m_nextClientID;
        c.isListing = true;
        d.m_clients.push_back(std::move(c));
    }
    static void watch(Daemon& d, Daemon::Session& s) { d.watchProcessTree(s); }
    static bool isWakePending(const Daemon& d)
    {
        if (!d.m_processTreeWake)
            return false;
        pollfd descriptor { .fd = d.m_processTreeWake->pipe.read.get(), .events = POLLIN, .revents = 0 };
        return ::poll(&descriptor, 1, 0) == 1 && (descriptor.revents & POLLIN);
    }
    static void takeEdges(Daemon& d, std::uint64_t now) { d.takeProcessTreeEdges(now); }
    static std::uint64_t dueAt(const Daemon& d, const Daemon::Session& s) { return d.processTreeProbeDueAt(s); }
    static void serve(Daemon& d, Daemon::Session& s, std::uint64_t now) { d.serveProcessTreeProbe(s, now); }
    static Protocol::Sync::PaneFacts records(Daemon& d, Daemon::Session& s)
    {
        (void)d.refreshPaneFacts(s);
        d.publishRecords(s, 0);
        return s.screenSource->facts();
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::CountingStream;
using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::ForegroundProbeBudgetAccess;
using Ubiquitty::Host::probeBudgetMilliseconds;

static std::uint64_t nowMillisecondsSteady() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

int main()
{
    using A = ForegroundProbeBudgetAccess;

    // ---- 1. budget: N tight-loop broadcasts probe at most the budgeted times.
    {
        Daemon d;
        auto stream = std::make_unique<CountingStream>();
        CountingStream::Facts& facts = stream->facts;
        facts.group = 100; // settled: a resolvable group + a real name
        facts.name = "zsh";
        facts.isShell = true;
        auto& s = A::makeSession(d, std::move(stream), 2, 8);
        A::addListingReader(d);

        constexpr int broadcasts = 400;
        const std::uint64_t start = nowMillisecondsSteady();
        for (int i = 0; i < broadcasts; ++i)
            A::broadcast(d, s);
        const std::uint64_t elapsed = nowMillisecondsSteady() - start;

        // The first probe and the reading that settles it, then one per
        // elapsed budget window (+1 boundary slack) — never one per broadcast.
        const int allowedProbes = 3 + static_cast<int>(elapsed / probeBudgetMilliseconds);
        CHECK(facts.nameProbeCount >= 1);
        CHECK(facts.nameProbeCount <= allowedProbes);
        // The directory that stands in for a silent OSC 7 is the same kind of
        // read, for the pane facts every attachment is sent; it rides the
        // same budget, not one read per frame.
        CHECK(facts.directoryProbeCount <= allowedProbes);

        // The first probe becomes the canonical foreground record.
        const auto initial = A::records(d, s);
        CHECK(initial.foreground.name == "zsh");
        CHECK(initial.isAtPrompt);

        // ---- 2. change-detector: a moved foreground group re-probes now,
        // inside the budget window, and PaneFacts reflects the change. A
        // reading that moved is read once more to settle it.
        const int probesBeforeChange = facts.nameProbeCount;
        facts.group = 200;
        facts.name = "vim";
        facts.isShell = false;
        A::broadcast(d, s);
        CHECK(facts.nameProbeCount == probesBeforeChange + 2);
        const auto changed = A::records(d, s);
        CHECK(changed.revision > initial.revision);
        CHECK(changed.foreground.name == "vim");
        CHECK(!changed.isAtPrompt);
    }

    // ---- 3. pre-exec: an unsettled probe (group 0, empty name) never latches
    // the budget — every broadcast re-probes until a real foreground shows.
    {
        Daemon d;
        auto stream = std::make_unique<CountingStream>();
        CountingStream::Facts& facts = stream->facts;
        facts.group = 0; // pre-exec: no resolvable foreground yet
        facts.name = {};
        facts.isShell = true;
        auto& s = A::makeSession(d, std::move(stream), 2, 8);
        A::addListingReader(d);

        for (int i = 0; i < 3; ++i)
            A::broadcast(d, s);
        // Unsettled: probed every frame, each probe read twice — a reading
        // with no name is not read a third time once it stands still.
        CHECK(facts.nameProbeCount == 6);
        CHECK(A::records(d, s).foreground.name.empty()); // nothing real to report yet

        // The shell execs: the next broadcast probes, reports it, and settles.
        facts.group = 300;
        facts.name = "zsh";
        A::broadcast(d, s);
        CHECK(facts.nameProbeCount == 8);
        CHECK(A::records(d, s).foreground.name == "zsh");

        // Settled now: further tight-loop broadcasts ride the budget.
        const std::uint64_t start = nowMillisecondsSteady();
        for (int i = 0; i < 50; ++i)
            A::broadcast(d, s);
        const std::uint64_t elapsed = nowMillisecondsSteady() - start;
        CHECK(facts.nameProbeCount <= 8 + 1 + static_cast<int>(elapsed / probeBudgetMilliseconds));
    }

    // ---- A job whose name did not read is no news about who holds the
    // terminal: the pane keeps the name it published, takes the holder it read,
    // and the probe stays unsettled until a reading has a name.
    {
        Daemon d;
        auto stream = std::make_unique<CountingStream>();
        CountingStream::Facts& facts = stream->facts;
        facts.group = 100;
        facts.name = "zsh";
        facts.isShell = true;
        auto& s = A::makeSession(d, std::move(stream), 2, 8);
        A::addListingReader(d);
        A::broadcast(d, s);
        CHECK(A::records(d, s).foreground.name == "zsh");

        facts.group = 200;
        facts.name = {};
        facts.isShell = false;
        A::broadcast(d, s);
        CHECK_MSG(A::records(d, s).foreground.name == "zsh", "a job with no name replaced the published name");
        CHECK_MSG(!A::records(d, s).isAtPrompt, "a job with no name left the pane at its prompt");
        CHECK(!s.foregroundProbeSettled);

        facts.name = "sleep";
        A::broadcast(d, s);
        CHECK(A::records(d, s).foreground.name == "sleep");
        CHECK(!A::records(d, s).isAtPrompt);
    }

    if (failures == 0)
        std::printf("foreground_probe_budget: foreground facts probe on the ~%llu ms budget "
                    "(immediate on a foreground-group change, every frame while pre-exec-unsettled), "
                    "and canonical PaneFacts reflects each settled change\n",
            static_cast<unsigned long long>(probeBudgetMilliseconds));
    // ---- A working or blocked record ends when the shell has the terminal
    // back, with no prompt mark: a shell without the integration marks none,
    // and the record of an interrupted program would stand for the pane's life.
    {
        Daemon d;
        auto stream = std::make_unique<CountingStream>();
        CountingStream::Facts& facts = stream->facts;
        facts.group = 200;
        facts.name = "sleep";
        facts.isShell = false;
        auto& s = A::makeSession(d, std::move(stream), 2, 8);
        A::addListingReader(d);
        A::broadcast(d, s);
        Ubiquitty::processBytes(s.term.get(), "\033]7501;id=job:state=blocked:kind=question:msg=Q29udGludWU/\033\\");
        A::broadcast(d, s);
        CHECK(s.term->programStatus.records.size() == 1);
        // Interrupted: the shell's group has the terminal again, and a moved
        // group is probed at once.
        facts.group = 100;
        facts.name = "bash";
        facts.isShell = true;
        A::broadcast(d, s);
        CHECK_MSG(s.term->programStatus.records.empty(), "a blocked record outlived its program at a shell that marks no prompt");
    }

    // ---- 4a. A silent foreground change, announced by a process-tree edge,
    // reaches a reader once the tree has settled, with no frame asking.
    {
        Daemon d;
        auto stream = std::make_unique<CountingStream>();
        CountingStream::Facts& facts = stream->facts;
        facts.group = 100;
        facts.name = "zsh";
        facts.isShell = true;
        facts.isEdgeTracked = true;
        auto& s = A::makeSession(d, std::move(stream), 2, 8);
        A::addListingReader(d);
        A::watch(d, s);
        CHECK(facts.notify != nullptr);
        A::broadcast(d, s);
        CHECK(A::records(d, s).isAtPrompt);
        const std::uint64_t probed = s.lastForegroundProbeMilliseconds;

        // `sleep` takes the terminal and prints nothing.
        facts.group = 200;
        facts.name = "sleep";
        facts.isShell = false;
        CHECK(A::dueAt(d, s) == 0);
        facts.raiseTreeEdge();
        CHECK_MSG(A::isWakePending(d), "a process-tree edge did not wake the loop");
        const std::uint64_t edgeAt = probed + 1000;
        A::takeEdges(d, edgeAt);
        CHECK(!A::isWakePending(d));
        const std::uint64_t due = A::dueAt(d, s);
        CHECK(due == edgeAt + Ubiquitty::Title::edgeSettleMilliseconds);
        // A later edge before the tree settles pushes the answer out, no
        // further than the deferral cap.
        facts.raiseTreeEdge();
        A::takeEdges(d, edgeAt + 100);
        CHECK(A::dueAt(d, s) == edgeAt + 100 + Ubiquitty::Title::edgeSettleMilliseconds);
        A::serve(d, s, edgeAt + 100);
        CHECK(A::records(d, s).isAtPrompt);
        A::serve(d, s, A::dueAt(d, s));
        const auto moved = A::records(d, s);
        CHECK_MSG(!moved.isAtPrompt, "a program that took the terminal silently still read as the shell at its prompt");
        CHECK(moved.foreground.name == "sleep");
        CHECK(A::dueAt(d, s) == 0);
    }

    // ---- 4b. Where no edges are raised, a pane someone reads is probed on the
    // fallback cadence, and a pane nobody reads is not.
    {
        Daemon d;
        auto stream = std::make_unique<CountingStream>();
        CountingStream::Facts& facts = stream->facts;
        facts.group = 100;
        facts.name = "zsh";
        facts.isShell = true;
        auto& s = A::makeSession(d, std::move(stream), 2, 8);
        A::watch(d, s);
        CHECK(facts.notify == nullptr);
        A::broadcast(d, s);
        CHECK(facts.nameProbeCount == 0);
        CHECK(A::dueAt(d, s) == 0);

        // A listing arrives, and the pane's next frame probes for it.
        A::addListingReader(d);
        A::broadcast(d, s);
        CHECK(A::records(d, s).isAtPrompt);
        const std::uint64_t due = A::dueAt(d, s);
        CHECK(due == s.lastForegroundProbeMilliseconds + Ubiquitty::Title::walkFallbackMilliseconds);
        facts.group = 200;
        facts.name = "sleep";
        facts.isShell = false;
        A::serve(d, s, due - 1);
        CHECK(A::records(d, s).isAtPrompt);
        A::serve(d, s, due);
        const auto moved = A::records(d, s);
        CHECK_MSG(!moved.isAtPrompt, "a read pane was not probed on the fallback cadence");
        CHECK(moved.foreground.name == "sleep");
        CHECK(s.lastForegroundProbeMilliseconds + Ubiquitty::Title::walkFallbackMilliseconds >= due);
        CHECK(A::dueAt(d, s) == s.lastForegroundProbeMilliseconds + Ubiquitty::Title::walkFallbackMilliseconds);

        // Unchanged since: the next fallback probe still runs, the group
        // standing still.
        const int probesBefore = facts.nameProbeCount;
        A::serve(d, s, A::dueAt(d, s));
        CHECK(facts.nameProbeCount == probesBefore + 1);
    }

    // ---- 5. A pane nobody reads: its writes' frames probe nothing,
    // and a write's frame is taken in the pass that read it, so the loop owes
    // no second wake. A reader's frame probes again.
    {
        Daemon d;
        auto stream = std::make_unique<CountingStream>();
        CountingStream::Facts& facts = stream->facts;
        facts.group = 100;
        facts.name = "zsh";
        facts.isShell = true;
        auto& s = A::makeSession(d, std::move(stream), 2, 8);
        for (int write = 0; write < 50; ++write) {
            Ubiquitty::processBytes(s.term.get(), "tick\r\n");
            const std::uint64_t passEnd = nowMillisecondsSteady() * 1000;
            s.coherence.noteDrainPass(*s.term, passEnd, Ubiquitty::Runtime::PassEnd::WouldBlock);
            s.isFrameDue = true;
            const auto frame = A::mayPublish(d, s, passEnd);
            CHECK_MSG(frame.has_value(), "an unread pane's frame waited for a wake of its own");
            if (frame)
                A::broadcast(d, s);
        }
        CHECK_MSG(facts.nameProbeCount == 0, "a pane nobody reads was probed by its own writes");
        CHECK(!s.isFrameDue);

        A::addListingReader(d);
        A::broadcast(d, s);
        CHECK(facts.nameProbeCount == 2); // the first probe, and the reading that settles it
        CHECK(A::records(d, s).foreground.name == "zsh");
    }

    return failures == 0 ? 0 : 1;
}
