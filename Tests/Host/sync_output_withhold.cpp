// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/sync_output_withhold.cpp — the daemon publishes by the coherence
// rule (Runtime::FrameCoherence), DEC private mode 2026 among it.
//
// An attached client cannot make this decision for itself. The escapes reach
// the daemon's terminal and are never forwarded, and a frame carries no flag
// for an open block or a cursor-hidden redraw — so a client asking its own
// local terminal gets "nothing open" while the application is mid-composition.
// Publishing then tears on every attached client at once, which is what this
// pins.
//
// Each property is asserted to fail before the condition that makes it hold, so
// a check that cannot fail shows up as one that never changed its answer:
//
//   1. No block        — a due frame publishes.
//   2. COMPOSING       — an open block withholds, and keeps withholding across
//                        repeated asks.
//   3. Not DROPPED     — isFrameDue survives every refusal, and the composed
//                        frame publishes at the close of the block.
//   4. WATCHDOG        — an application that opens a block and then goes quiet
//                        does not hold the screen forever: once the block
//                        outlives maxHoldTicks the next ask publishes and the
//                        mode is cleared.
//   5. BRACKET         — a cursor hidden while the screen changes holds the
//                        frame until it is shown, or until the deadline, when
//                        the frame goes out forced; a client attaching inside
//                        it is sent no checkpoint until the rule admits a frame.
//   6. One drain       — a block opened and closed in one pass holds nothing.
//   7. Pass grouping   — a pass reads past a short read to a would-block or
//                        its capacity, and the rule classes the frame after a
//                        full pass as a flood.
//
// Friend seam (ForegroundProbeBudgetAccess's twin): the decision lives in the
// poll loop, which a test cannot enter, so mayPublishFrame is called directly
// on a real Daemon and Session, and a client's screen plane runs through
// ScreenPlaneAccess. Exit 0 on success.
//

#include "Assertions.h"
#include "LoopbackPaneLink.h"
#include "TerminalFixture.h"

#include "Core/Modes/SyncOutput.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Runtime/FrameCoherence.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"
#include "Platform/StreamDouble.h"

#include <csignal>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct SyncOutputWithholdAccess {
    // A connection is attached to the pane: only an attached pane's frames are
    // paced by the coherence rule under test, since nobody draws the others.
    static Daemon::Session& makeSession(Daemon& d, int rows, int columns)
    {
        auto& session = ScreenPlaneAccess::makeSession(d, rows, columns);
        Daemon::Client reader;
        reader.clientID = d.m_nextClientID++;
        reader.sessionID = session.id;
        reader.paneSubscriptions = { { .sessionID = session.id } };
        d.m_clients.push_back(std::move(reader));
        return session;
    }
    static std::optional<Runtime::FrameClass> mayPublish(Daemon& d, Daemon::Session& s, std::uint64_t now) { return d.mayPublishFrame(s, now); }
    static void publish(Daemon& d, Daemon::Session& s) { d.publishFrame(s, Runtime::FrameClass::Settled); }
    static void service(Daemon& d, Daemon::Session& s) { d.servicePTY(s); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::SyncOutputWithholdAccess;
using Ubiquitty::Runtime::FrameClass;

namespace {

// The daemon's own clock, which a drain pass stamps.
std::uint64_t monotonicMicroseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000ull;
}

// Past read quiescence and the pacing after the last frame, short of the
// deadline: every refusal from here on is the condition under test, not a read
// that has not gone quiet or a frame taken a moment ago.
constexpr std::uint64_t quietButInsideTheDeadline = Ubiquitty::Runtime::refreshIntervalMicroseconds + 1000;
static_assert(quietButInsideTheDeadline > Ubiquitty::Runtime::readQuiescenceMicroseconds);
static_assert(quietButInsideTheDeadline < Ubiquitty::Runtime::coherenceDeadlineMicroseconds);

} // namespace

int main()
{
    // ~Daemon's teardown writes to an attached client whose peer has closed;
    // that must return EPIPE, not kill this process.
    ::signal(SIGPIPE, SIG_IGN);
    using A = SyncOutputWithholdAccess;
    constexpr std::uint64_t start = 1'000'000'000;

    // ---- 1. No block: a due frame publishes.
    {
        Daemon d;
        auto& s = A::makeSession(d, 24, 80);
        s.isFrameDue = true;
        CHECK(A::mayPublish(d, s, start) == FrameClass::Structural);
    }

    // ---- 2 + 3. Composing withholds; the frame is deferred, not dropped.
    {
        Daemon d;
        auto& s = A::makeSession(d, 24, 80);
        s.isFrameDue = true;

        // Before the block, to prove the assertions below are about the block
        // and not about a session that could never publish.
        CHECK(A::mayPublish(d, s, start).has_value());

        UbiquittyTest::feed(*s.term.get(), "\x1b[?2026h");
        CHECK(Ubiquitty::Modes::SyncOutput::isActive(s.term.get()));

        // Output keeps arriving inside the block — the intermediate states an
        // application composes are exactly what must not reach a client.
        for (int i = 0; i < 32; ++i) {
            UbiquittyTest::feed(*s.term.get(), "line of a frame being composed\r\n");
            CHECK(!A::mayPublish(d, s, start).has_value());
            // Withholding defers. A refusal that cleared this would drop the
            // composed frame outright rather than delay it.
            CHECK(s.isFrameDue);
        }

        UbiquittyTest::feed(*s.term.get(), "\x1b[?2026l");
        CHECK(!Ubiquitty::Modes::SyncOutput::isActive(s.term.get()));
        CHECK(A::mayPublish(d, s, start).has_value());
    }

    // ---- 4. Watchdog: a block whose application went quiet expires by itself.
    {
        Daemon d;
        auto& s = A::makeSession(d, 24, 80);
        s.isFrameDue = true;

        UbiquittyTest::feed(*s.term.get(), "\x1b[?2026h");
        CHECK(!A::mayPublish(d, s, start).has_value()); // also stamps the block
        CHECK(s.term->modes.syncOutputTouchTicks != 0);

        // Backdate past the budget rather than sleeping through it: the stamp is
        // what the watchdog reads, and moving it is the same input a second of
        // silence would produce, without a second of test.
        s.term->modes.syncOutputTouchTicks -= Ubiquitty::Modes::SyncOutput::maxHoldTicks + 1;

        CHECK(A::mayPublish(d, s, start).has_value());
        // Expiry, not a one-off answer: the mode is cleared, so the next frame
        // does not have to re-derive that this application stopped being trusted.
        CHECK(!Ubiquitty::Modes::SyncOutput::isActive(s.term.get()));
    }

    // ---- 5. A cursor-hidden redraw holds until the cursor is shown, and at
    //         most until the deadline.
    {
        Daemon d;
        auto& s = A::makeSession(d, 24, 80);
        auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
        s.stream = std::move(stream);

        pty.pushInbound("$ ");
        A::service(d, s);
        const std::uint64_t firstPass = monotonicMicroseconds();
        CHECK(A::mayPublish(d, s, firstPass + quietButInsideTheDeadline) == FrameClass::Structural);
        A::publish(d, s);
        CHECK(!s.isFrameDue);

        // The first half of a repaint: the cursor goes hidden while the screen
        // changes, and the program has not shown it again.
        pty.pushInbound("\x1b[?25l\x1b[H\x1b[2Ktop half of the repaint");
        A::service(d, s);
        const std::uint64_t opened = monotonicMicroseconds();
        CHECK(!A::mayPublish(d, s, opened + quietButInsideTheDeadline).has_value());
        CHECK(s.isFrameDue);

        // The program went quiet inside it: the deadline takes the frame anyway.
        CHECK(A::mayPublish(d, s, opened + Ubiquitty::Runtime::coherenceDeadlineMicroseconds + 1000) == FrameClass::Forced);
        A::publish(d, s);

        // Shown again: the next repaint is held until its own show, which
        // releases it as an ordinary settled frame.
        pty.pushInbound("\x1b[?25h");
        A::service(d, s);
        CHECK(A::mayPublish(d, s, monotonicMicroseconds() + quietButInsideTheDeadline) == FrameClass::Settled);
        A::publish(d, s);
        pty.pushInbound("\x1b[?25l\x1b[2;1Hsecond repaint, first half");
        A::service(d, s);
        CHECK(!A::mayPublish(d, s, monotonicMicroseconds() + quietButInsideTheDeadline).has_value());
        pty.pushInbound("\x1b[3;1Hsecond half\x1b[?25h");
        A::service(d, s);
        CHECK(A::mayPublish(d, s, monotonicMicroseconds() + quietButInsideTheDeadline) == FrameClass::Settled);

        // A session with nothing to send publishes nothing.
        A::publish(d, s);
        CHECK(!A::mayPublish(d, s, monotonicMicroseconds() + quietButInsideTheDeadline).has_value());
    }

    // ---- 5b. A client attaching inside a cursor-hidden redraw is not handed
    //          the half-drawn screen as its sync point: its checkpoint waits
    //          for the frame the rule admits.
    {
        using Screen = Ubiquitty::Host::ScreenPlaneAccess;
        Daemon d;
        auto& s = A::makeSession(d, 24, 80);
        auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
        s.stream = std::move(stream);
        pty.pushInbound("$ ");
        A::service(d, s);
        CHECK(A::mayPublish(d, s, monotonicMicroseconds() + quietButInsideTheDeadline).has_value());
        A::publish(d, s);

        pty.pushInbound("\x1b[?25l\x1b[H\x1b[2Ktop half of the repaint");
        A::service(d, s);
        const std::size_t client = Screen::attach(d, s.id);
        Screen::markTrustedLocal(d, client);
        UbiquittyTest::ScreenClient screen(24, 80);
        Screen::greet(d, client, screen);
        CHECK_MSG(screen.count(Ubiquitty::Protocol::Sync::FrameKind::CheckpointHead) == 0, "a client attaching inside a redraw was sent the half-drawn screen");
        CHECK(s.isFrameDue);
        CHECK(!A::mayPublish(d, s, monotonicMicroseconds() + quietButInsideTheDeadline).has_value());

        pty.pushInbound("\x1b[2;1Hbottom half\x1b[?25h");
        A::service(d, s);
        CHECK(A::mayPublish(d, s, monotonicMicroseconds() + quietButInsideTheDeadline).has_value());
        A::publish(d, s);
        Screen::deliver(d, client, screen);
        CHECK_MSG(screen.count(Ubiquitty::Protocol::Sync::FrameKind::CheckpointHead) == 1, "the client was never sent the finished frame as its checkpoint");
        const std::string difference = screen.pane() ? UbiquittyTest::firstDisagreement(*s.term, *screen.pane()) : std::string("no pane was bound");
        CHECK_MSG(difference.empty(), difference.c_str());
    }

    // ---- 6. A block opened and closed inside one drain leaves nothing to hold.
    //
    // servicePTY reads until the PTY is empty before the loop reaches emission,
    // so a flood that carries DECSET, its output and DECRST in one pass has
    // already closed the block by the time this is asked. Publishing is then
    // correct: the composed frame is the only state that ever existed here.
    // The mechanism protects composition that SPANS poll cycles, which is what a
    // TUI redrawing over a slow pipe does — not a burst the kernel hands over
    // whole.
    {
        Daemon d;
        auto& s = A::makeSession(d, 24, 80);
        s.isFrameDue = true;

        std::string burst = "\x1b[?2026h";
        for (int i = 0; i < 64; ++i)
            burst += "a line of the composed frame\r\n";
        burst += "\x1b[?2026l";
        UbiquittyTest::feed(*s.term.get(), burst);

        CHECK(!Ubiquitty::Modes::SyncOutput::isActive(s.term.get()));
        CHECK(A::mayPublish(d, s, start).has_value());
    }

    // ---- 7. A drain pass reads on past a short read, to a would-block.
    //
    // A PTY hands over one quantum per read, far below the daemon's buffer, so
    // a short read is not the end of the output. A pass that stopped there
    // would ask the rule part way through every redraw longer than a quantum,
    // and no pass would ever spend its budget, so the rule could never see a
    // flood. LoopbackPaneLink groups recorded reads by the same rule.
    {
        constexpr std::size_t quantum = 16384;
        const auto lines = [](std::size_t bytes) {
            std::string text;
            while (text.size() < bytes)
                text += "a line of a redraw longer than one quantum\r\n";
            text.resize(bytes);
            return text;
        };

        Daemon d;
        auto& s = A::makeSession(d, 24, 80);
        auto stream = std::make_unique<Ubiquitty::TestDoubles::StreamDouble>();
        Ubiquitty::TestDoubles::StreamDouble& pty = *stream;
        pty.setQuantumBytes(quantum);
        pty.pushInbound(lines(3 * quantum));
        s.stream = std::move(stream);
        A::service(d, s);
        CHECK(pty.bytesRemaining() == 0);
        CHECK(s.isFrameDue);
        CHECK(A::mayPublish(d, s, monotonicMicroseconds() + quietButInsideTheDeadline) == FrameClass::Structural);
        A::publish(d, s);

        // A writer still ahead at the budget ends the pass at its capacity,
        // and the frame taken after it is a flood, without waiting for quiet.
        constexpr std::size_t capacity = Ubiquitty::Runtime::TerminalRuntime::drainPassCapacityBytes;
        pty.pushInbound(lines(capacity + quantum));
        A::service(d, s);
        CHECK(pty.bytesRemaining() == quantum);
        CHECK(A::mayPublish(d, s, monotonicMicroseconds() + Ubiquitty::Runtime::refreshIntervalMicroseconds) == FrameClass::Flood);
    }

    if (failures == 0)
        std::printf("sync_output_withhold: the daemon publishes by the coherence rule — it withholds while an "
                    "application composes under DEC 2026 or inside a cursor-hidden redraw, defers rather than "
                    "drops, and expires a block that outlives maxHoldTicks (%u ticks)\n",
            Ubiquitty::Modes::SyncOutput::maxHoldTicks);
    return failures == 0 ? 0 : 1;
}
