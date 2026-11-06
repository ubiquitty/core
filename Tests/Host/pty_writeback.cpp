// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/pty_writeback.cpp — the reply the program is still waiting for.
//
// Core answers a program's queries — DSR, DA, a focus report — by putting bytes
// in the Terminal's own output buffer, and the daemon writes them back up the
// PTY. The program is often blocked on that answer: vim asks for the cursor
// position and waits, tmux asks for device attributes and waits. A reply that
// is dropped hangs the program until its timeout, and the reason never appears
// anywhere.
//
// The pty is where it can go wrong, because a pty write can be refused. The
// child may not be reading (Again), or may be gone (Closed/Fatal), and it may
// take some of the bytes and refuse the rest. Replies share the pty's input
// queue with the clients' keystrokes and pastes: what the master refuses waits
// there, in order, for the run loop's POLLOUT drain.
//
// The stream double refuses writes on demand (accept N bytes, then refuse with
// a chosen status) and, with queueWriteAll, queues what writeAll could not
// write the way the pty stream does.
//
// The assertion that matters is not "the code took the Again branch". It is
// that the bytes arrive exactly once, in order, across the refusal, and never
// ahead of input queued before them.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"

#include "Assertions.h"
#include "Platform/StreamDouble.h"
#include "TerminalFixture.h"

#include "Core/Modes/Focus.h"
#include "Core/Protocol/Sync/Input.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"

#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct PTYWritebackAccess {
    static Daemon::Session& addSession(Daemon& d, std::uint32_t id, TestDoubles::StreamDouble** streamOut)
    {
        auto session = std::make_unique<Daemon::Session>();
        session->id = id;
        session->term = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::resize(*session->term, 24, 80);
        auto stream = std::make_unique<TestDoubles::StreamDouble>();
        if (streamOut)
            *streamOut = stream.get();
        session->stream = std::move(stream);
        Daemon::Session& ref = *session;
        d.m_sessions.push_back(std::move(session));
        return ref;
    }

    /// Tells a session's program its pane gained or lost the focus, as the
    /// daemon does when a driver's focus moves: the cheapest thing that puts
    /// bytes in Core's queue and asks for them to go up the pty at once.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    ///   - focus: The pane's focus.
    static void reportFocus(Daemon& d, Daemon::Session& s, Ubiquitty::Modes::FocusState focus)
    {
        Ubiquitty::Modes::focusChanged(s.term.get(), focus);
        d.drainTermToPTY(s);
    }

    static void drain(Daemon& d, Daemon::Session& s) { d.drainTermToPTY(s); }

    /// Writes a client's input to a session's program, through the daemon's
    /// own write of input a client typed.
    ///
    /// - Parameters:
    ///   - d: The daemon.
    ///   - s: The session.
    ///   - bytes: Keystrokes or a paste.
    static void sendInput(Daemon& d, Daemon::Session& s, std::string_view bytes)
    {
        (void)d.writeRemoteInput(s, {}, {}, { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() });
    }

    // What Core still holds for the program, undelivered.
    static int queuedForProgram(const Daemon::Session& s) { return s.term ? s.term->outputLength : 0; }

    /// Whether the session ended, with the reason its reap line names.
    static bool hasEnded(const Daemon::Session& s) { return s.shellExited && !s.exitReason.empty(); }

    /// The run loop's screen deadline, which bounds its poll timeout.
    static std::optional<std::uint64_t> nextService(const Daemon& d, std::uint64_t at) { return d.nextScreenServiceMicroseconds(at); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::PTYWritebackAccess;
using Ubiquitty::TestDoubles::StreamDouble;

namespace {

using A = PTYWritebackAccess;

// Focus reporting on, so a focus change produces CSI I / CSI O for the program.
void askForFocusReports(Ubiquitty::Terminal& t) { UbiquittyTest::feed(t, "\033[?1004h"); }

} // namespace

int main()
{
    // ---- 1. The control: an accepting pty gets the report immediately.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        askForFocusReports(*s.term);

        A::reportFocus(d, s, Ubiquitty::Modes::FocusState::Focused);

        CHECK(stream->written == "\033[I");
        CHECK(A::queuedForProgram(s) == 0);
    }

    // ---- 2. A pty that is not being read holds the report rather than losing
    // it. The program is blocked on this answer, so a drop is a hang with no
    // diagnostic; the bytes wait in the pty's queue for the next POLLOUT.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        stream->queueWriteAll();
        askForFocusReports(*s.term);
        stream->acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);

        A::reportFocus(d, s, Ubiquitty::Modes::FocusState::Focused);
        CHECK(stream->written.empty());
        CHECK(stream->hasPendingWrite()); // ESC [ I, still owed to the program

        // The child starts reading again.
        stream->allowWrites();
        stream->drainPendingWrite();
        CHECK(stream->written == "\033[I");
        CHECK(A::queuedForProgram(s) == 0);
    }

    // ---- 3. A partial write queues only what was not taken. Requeueing the
    // whole payload also "handles" the refusal, and delivers the accepted
    // prefix twice — a doubled ESC in the middle of the program's reply stream.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        stream->queueWriteAll();
        askForFocusReports(*s.term);
        stream->acceptBytesThenRefuse(1, Ubiquitty::PAL::StreamStatus::Again); // one byte, then backed up

        A::reportFocus(d, s, Ubiquitty::Modes::FocusState::Focused);
        CHECK(stream->written == "\033"); // the introducer went, the rest did not
        CHECK(stream->hasPendingWrite()); // "[I" queued, not "\033[I"
        CHECK(A::queuedForProgram(s) == 0);

        stream->allowWrites();
        stream->drainPendingWrite();
        A::drain(d, s);
        CHECK(stream->written == "\033[I"); // exactly once, in order
    }

    // ---- 4. And the whole sequence survives across a refusal in the middle of
    // a conversation: two reports, the first interrupted, both arriving in the
    // order the program expects.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        askForFocusReports(*s.term);
        stream->queueWriteAll();
        stream->acceptBytesThenRefuse(1, Ubiquitty::PAL::StreamStatus::Again);

        A::reportFocus(d, s, Ubiquitty::Modes::FocusState::Focused);
        stream->allowWrites();
        A::reportFocus(d, s, Ubiquitty::Modes::FocusState::Unfocused);

        CHECK(stream->written == "\033[I\033[O");
        CHECK(A::queuedForProgram(s) == 0);
    }

    // ---- 5. A shell that is gone ends the drain instead of spinning on a
    // write that will never succeed. One attempt, then the daemon stops — the
    // session is reaped by the run loop, and a drain that looped here would
    // take the daemon with it. The refused write ends the session itself: a
    // background job still holding the replica keeps the hangup from coming,
    // and a session left running would keep its reply owed, and the loop's
    // poll timeout at zero, for as long as that job lives.
    constexpr std::uint64_t at = 1'000'000;
    for (const auto status : { Ubiquitty::PAL::StreamStatus::Fatal, Ubiquitty::PAL::StreamStatus::Closed }) {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        askForFocusReports(*s.term);
        stream->acceptBytesThenRefuse(0, status);

        A::reportFocus(d, s, Ubiquitty::Modes::FocusState::Focused);

        CHECK(stream->written.empty());
        CHECK(stream->writeCalls == 1);
        CHECK(!stream->spun); // it stopped, rather than retrying a refusal that will never clear
        CHECK(A::hasEnded(s));
        const std::optional<std::uint64_t> due = A::nextService(d, at);
        CHECK_MSG(!due || *due > at, "a session whose child is gone kept the loop due at once");
    }

    // ---- 9. A refusal that queued nothing arms no POLLOUT, and the next pass
    // would meet the same refusal, so the loop is not due at once. The reply
    // stays owed, and the next drain delivers it.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        askForFocusReports(*s.term);
        stream->acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);

        A::reportFocus(d, s, Ubiquitty::Modes::FocusState::Focused);
        CHECK(!stream->hasPendingWrite());
        CHECK(A::queuedForProgram(s) > 0);
        CHECK(!A::hasEnded(s));
        const std::optional<std::uint64_t> due = A::nextService(d, at);
        CHECK_MSG(!due || *due > at, "a refused reply kept the loop due at once");

        stream->allowWrites();
        A::drain(d, s);
        CHECK(stream->written == "\033[I");
        CHECK(A::queuedForProgram(s) == 0);
    }

    // ---- 7. A reply never overtakes input already queued for the program. A
    // paste the child has not read yet waits in the pty's input queue; a
    // report produced meanwhile lands behind it, or it reaches the program in
    // the middle of the paste and is inserted into it as text.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        stream->queueWriteAll();
        askForFocusReports(*s.term);
        const std::string paste = "\033[200~print(1)\n\033[201~";
        stream->acceptBytesThenRefuse(4, Ubiquitty::PAL::StreamStatus::Again);
        A::sendInput(d, s, paste);
        CHECK(stream->hasPendingWrite());

        stream->allowWrites();
        A::reportFocus(d, s, Ubiquitty::Modes::FocusState::Focused);
        stream->drainPendingWrite();
        A::drain(d, s);
        CHECK(stream->written == paste + "\033[I");
    }

    // ---- 8. A refused reply waits where the run loop drains: in the pty's
    // queue, which arms POLLOUT. Held back in Core's buffer instead, it moves
    // only when the program prints again, and a program waiting for the reply
    // prints nothing.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);
        stream->queueWriteAll();
        askForFocusReports(*s.term);
        stream->acceptBytesThenRefuse(0, Ubiquitty::PAL::StreamStatus::Again);

        A::reportFocus(d, s, Ubiquitty::Modes::FocusState::Focused);
        CHECK(stream->written.empty());
        CHECK_MSG(stream->hasPendingWrite(), "the refused reply is not where POLLOUT drains");

        stream->allowWrites();
        stream->drainPendingWrite();
        CHECK(stream->written == "\033[I");
    }

    // ---- 6. Nothing queued writes nothing. A drain that issued an empty write
    // per tick would wake the child for no reason on every idle pass.
    {
        Daemon d;
        StreamDouble* stream = nullptr;
        auto& s = A::addSession(d, 1, &stream);

        A::drain(d, s);

        CHECK(stream->writeCalls == 0);
    }

    if (failures == 0)
        std::printf("pty_writeback: a program's reply reaches the pty exactly once and in order across a "
                    "backed-up write, behind input queued before it, and a write to a shell that has gone "
                    "ends the session rather than spinning\n");
    return UBIQUITTY_TEST_SUMMARY() == 0 ? 0 : 1;
}
