// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "POSIXChildProcess.h"

#include <atomic>

// Tests/Platform/test_pty.cpp
//
// Host-side coverage of the portable PTY backend (Source/Platform/POSIX/PTYStream.cpp
// + the process facts in Source/Platform/POSIX/PTYFacts.cpp). Drives the
// real backend against the local /bin/echo, /bin/cat, /usr/bin/false and /bin/stty
// utilities through openPTY(). The ctest tree builds and runs it on Darwin and
// Linux; Scripts/run-tests.sh runs it on Darwin only. The cases that rely on
// macOS login(1) are compiled only on Darwin.
//
// Cases:
//   - openPTY rejects empty arguments / an empty executable path
//   - spawn echo with a fixed payload, drain to EndOfFile, observe the payload
//   - spawn cat, write "hello\n", read it back from the slave's echo, close
//   - spawn /usr/bin/false, wait for the child to exit, observe Closed
//   - setWindowSize on the master is visible to the slave (`stty size`)
//   - child environment defaults the character locale to UTF-8 only when unset
//   - process().foregroundProcessGroup() resolves a live job and is stable
//   - an adopted master reads its shell at the prompt as the shell, a job as a job
//   - a job whose executable no longer reads is named by its command, not the shell
//   - a shell whose executable no longer reads is named by its command in a subshell
//   - a foreground group too large to list is named by a program in it, and
//     is listed no further than one member past the buffer
//   - a job led by a fork is named by its oldest program, a nested shell's
//     subshell by that shell
//   - on Linux, a job whose first thread ended still holds the terminal, and a
//     program that renames itself is named by the name it was run by
//   - a running job is named by the program that makes it one, a hook by the shell
//   - a program this process forked delivers fork/exec/exit edges to a notifier

#include "Core/Screen/Addressing.h"
#include "Core/Session/SessionPolicy.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/OutputQueue.h"
#include "PAL/PAL.h"
#include "PAL/PTYFacts.h"
#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/POSIX/InheritedPTY.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <optional>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
// openpty's home differs per platform, exactly as Platform/POSIX/PTYStream.cpp
// spells it — the test and the code it exercises must agree on that.
#if defined(__linux__)
#include <dirent.h>
#include <pty.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <util.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

using namespace Ubiquitty;

static int status(PAL::StreamStatus s) { return static_cast<int>(s); }

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(const char* name)
        : m_name(name)
    {
        if (const char* value = ::getenv(name)) {
            m_hadValue = true;
            m_value = value;
        }
    }

    ~ScopedEnvironmentVariable()
    {
        if (m_hadValue)
            ::setenv(m_name, m_value.c_str(), 1);
        else
            ::unsetenv(m_name);
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    const char* m_name;
    bool m_hadValue = false;
    std::string m_value;
};

// Open a fresh PTY running `arguments` (arguments[0] = the executable path).
static std::unique_ptr<PAL::Stream> openArguments(std::initializer_list<std::string> arguments, PAL::CharacterLocalePolicy characterLocalePolicy = PAL::CharacterLocalePolicy::Auto)
{
    std::vector<std::string> vector(arguments);
    // Assigned onto a defaulted value rather than partially designated: a brace
    // list naming a subset leaves the rest to whatever the aggregate rules give,
    // and the members added later are the ones a reader will not notice.
    PAL::PTYLaunchOptions options;
    options.characterLocalePolicy = characterLocalePolicy;
    return PAL::openPTY(vector, options);
}

/// Drain s into buffer for up to budgetMs milliseconds or until cap is
/// reached or EndOfFile / Closed surfaces. Returns the byte count
/// accumulated; sets *finalStatus to the last negative status (or 0 if
/// the read loop only ever saw byte counts).
static int drainWithBudget(PAL::Stream* s, char* buffer, int capacity, int budgetMs, int* finalStatus)
{
    int got = 0;
    int ticks = 0;
    if (finalStatus)
        *finalStatus = 0;
    const int sleepUs = 5 * 1000;
    const int maxTicks = budgetMs * 1000 / sleepUs;
    while (got < capacity && ticks < maxTicks) {
        int n = s->read({ buffer + got, static_cast<size_t>(capacity - got) });
        if (n > 0) {
            got += n;
            continue;
        }
        if (n == status(PAL::StreamStatus::Again)) {
            ::usleep(sleepUs);
            ++ticks;
            continue;
        }
        if (finalStatus)
            *finalStatus = n;
        break;
    }
    return got;
}

// Reads until `text` has arrived, the stream ends, or the bound passes. The
// condition is the echo itself rather than a time it should have come by: a
// shell at low priority under load can take far longer than a fixed budget to
// answer, and a short budget failed runs for that alone.
static bool drainUntilContains(PAL::Stream* stream, std::string_view text, int boundMilliseconds = 20'000)
{
    std::string seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(boundMilliseconds);
    char buffer[256];
    while (!seen.contains(text) && std::chrono::steady_clock::now() < deadline) {
        const int count = stream->read({ buffer, sizeof(buffer) });
        if (count > 0)
            seen.append(buffer, static_cast<std::size_t>(count));
        else if (count == status(PAL::StreamStatus::Again))
            ::usleep(1000);
        else
            break;
    }
    return seen.contains(text);
}

static std::string outputFromArguments(std::initializer_list<std::string> arguments, PAL::CharacterLocalePolicy characterLocalePolicy = PAL::CharacterLocalePolicy::Auto)
{
    auto s = openArguments(arguments, characterLocalePolicy);
    CHECK(s != nullptr);
    if (!s)
        return {};

    char buffer[512];
    int finalStatus = 0;
    int got = drainWithBudget(s.get(), buffer, sizeof(buffer), 2000, &finalStatus);
    CHECK(finalStatus == status(PAL::StreamStatus::EndOfFile) || finalStatus == status(PAL::StreamStatus::Closed));
    return { buffer, static_cast<size_t>(got) };
}

#if defined(__APPLE__)
TEST(testLoginWrapperReportsShellExitStatus)
{
    char user[256];
    const int length = PAL::systemLoginUserName(user, sizeof(user));
    CHECK(length > 0);
    if (length <= 0) {
        return;
    }
    for (const std::string command : { "exit 0", "exit 7", "kill -TERM $$" }) {
        auto stream = openArguments({ "/usr/bin/login", "-pfl", std::string(user, static_cast<std::size_t>(length)),
            "/bin/sh", "-c", "read answer; " + command });
        CHECK(stream != nullptr);
        if (!stream) {
            continue;
        }
        // The command waits for input so the process watch precedes its exit.
        ::usleep(100000);
        CHECK(stream->writeAll(std::span("go\n", 3)) == 0);
        char buffer[1024];
        int finalStatus = 0;
        drainWithBudget(stream.get(), buffer, sizeof(buffer), 3000, &finalStatus);
        // Waits for the exit itself rather than for a time it should have been
        // seen by: the shell's exit reaches the stream through login and the
        // tracker's thread, and under load it was not seen within the second
        // this once allowed. The bound only turns an exit never seen into a
        // failure instead of a hang.
        PAL::ChildExit exit;
        const auto exitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        for (;;) {
            stream->process().hasRunningChild();
            exit = stream->childExit();
            if (exit.didExit || std::chrono::steady_clock::now() >= exitDeadline) {
                break;
            }
            ::usleep(10000);
        }
        CHECK(exit.didExit);
        CHECK(exit.wasSignalled == (command == "kill -TERM $$"));
        CHECK(exit.code == (command == "exit 0" ? 0 : command == "exit 7" ? 7
                                                                          : SIGTERM));
    }
}
#endif

TEST(testRejectsBadArguments)
{
    CHECK(openArguments({}) == nullptr); // no arguments
    CHECK(openArguments({ "" }) == nullptr); // empty executable path
}

TEST(testEchoRoundTrip)
{
    auto s = openArguments({ "/bin/echo", "Hello", "PTY" });
    CHECK(s != nullptr);
    if (!s)
        return;

    char buffer[256];
    int finalStatus = 0;
    int got = drainWithBudget(s.get(), buffer, sizeof(buffer) - 1, 2000, &finalStatus);
    buffer[got] = '\0';

    // echo appends a newline; the PTY is in cooked mode by default so
    // the LF becomes CRLF. We assert the prefix and the trailing LF
    // rather than a strict equality, so a future kernel quirk in the
    // line discipline doesn't false-fail this case.
    CHECK(got >= 10);
    CHECK(std::strstr(buffer, "Hello PTY") != nullptr);
    CHECK(finalStatus == status(PAL::StreamStatus::EndOfFile) || finalStatus == status(PAL::StreamStatus::Closed));

    std::printf("echo: got=%d bytes payload=\"%s\"\n", got, buffer);
}

TEST(testCatWriteReadRoundTrip)
{
    auto s = openArguments({ "/bin/cat" });
    CHECK(s != nullptr);
    if (!s)
        return;

    const char* msg = "hello\n";
    int wrote = 0;
    int ticks = 0;
    while (wrote < 6 && ticks < 200) {
        int n = s->write({ msg + wrote, static_cast<size_t>(6 - wrote) });
        if (n > 0) {
            wrote += n;
            continue;
        }
        if (n == status(PAL::StreamStatus::Again)) {
            ::usleep(5 * 1000);
            ++ticks;
            continue;
        }
        break;
    }
    CHECK(wrote == 6);

    // cat in cooked mode echoes the line + writes the cat output;
    // expect at least one "hello" substring in the drain.
    char buffer[256];
    int finalStatus = 0;
    int got = drainWithBudget(s.get(), buffer, sizeof(buffer) - 1, 2000, &finalStatus);
    buffer[got] = '\0';
    CHECK(got > 0);
    CHECK(std::strstr(buffer, "hello") != nullptr);

    // Destroying the stream SIGHUPs cat and reaps it.
    s.reset();
}

TEST(testFalsePromotesToClosed)
{
    auto s = openArguments({ "/usr/bin/false" });
    CHECK(s != nullptr);
    if (!s)
        return;

    // Give /usr/bin/false up to 500 ms to fork + exit.
    char buffer[64];
    int finalStatus = 0;
    drainWithBudget(s.get(), buffer, sizeof(buffer), 500, &finalStatus);
    CHECK(finalStatus == status(PAL::StreamStatus::EndOfFile) || finalStatus == status(PAL::StreamStatus::Closed));
}

TEST(testWindowSizePropagatesToSlave)
{
    // The program reads its size only after a line it waits for, which is
    // written after the resize: openPTY returns once the program runs, so a
    // program that read its size at once could read it before the resize.
    auto s = openArguments({ "/bin/sh", "-c", "read line; echo SIZE; stty size" });
    CHECK(s != nullptr);
    if (!s)
        return;

    s->setWindowSize(132, 50, 0, 0);
    CHECK(s->writeAll(std::span("go\n", 3)) == 0);

    char buffer[256];
    int finalStatus = 0;
    int got = drainWithBudget(s.get(), buffer, sizeof(buffer) - 1, 2000, &finalStatus);
    buffer[got > 0 ? got : 0] = '\0';
    CHECK(got > 0);

    // stty size prints "<rows> <cols>\n" after the marker.
    const char* size = std::strstr(buffer, "SIZE");
    int gotRows = 0, gotColumns = 0;
    if (size && std::sscanf(size + 4, "%d %d", &gotRows, &gotColumns) == 2) {
        CHECK(gotRows == 50);
        CHECK(gotColumns == 132);
        std::printf("stty: rows=%d cols=%d\n", gotRows, gotColumns);
    } else {
        std::printf("stty: unexpected output \"%s\"\n", buffer);
        CHECK(false);
    }
}

TEST(testDefaultCharacterLocaleIsUTF8)
{
    ScopedEnvironmentVariable savedLanguage("LANG");
    ScopedEnvironmentVariable savedLocaleAll("LC_ALL");
    ScopedEnvironmentVariable savedLocaleCharacterType("LC_CTYPE");
    ::setenv("LANG", "", 1);
    ::setenv("LC_ALL", "", 1);
    ::setenv("LC_CTYPE", "", 1);

    std::string output = outputFromArguments({ "/usr/bin/printenv", "LC_CTYPE" });
    CHECK(output.find("C.UTF-8") != std::string::npos);
}

// The app exports LANG on itself, and the child must see that rather than the
// C.UTF-8 fallback: LANG carries collation and time, the categories the
// fallback leaves at C, and injecting LC_CTYPE over it would narrow the
// child's character handling back to a category the language already covers.
//
// This is the seam between the two changes. Neither is wrong alone, and the
// composition is what a reader has to be told: the fallback exists for the
// case where no language was set, and stops applying the moment one is.
TEST(testExportedLanguageReachesTheChildInsteadOfTheFallback)
{
    ScopedEnvironmentVariable savedLanguage("LANG");
    ScopedEnvironmentVariable savedLocaleAll("LC_ALL");
    ScopedEnvironmentVariable savedLocaleCharacterType("LC_CTYPE");
    ::setenv("LANG", "en_GB.UTF-8", 1);
    ::setenv("LC_ALL", "", 1);
    ::unsetenv("LC_CTYPE");

    CHECK(outputFromArguments({ "/usr/bin/printenv", "LANG" }).find("en_GB.UTF-8") != std::string::npos);
    CHECK(outputFromArguments({ "/usr/bin/printenv", "LC_CTYPE" }).find("C.UTF-8") == std::string::npos);
}

TEST(testPreservedCharacterLocaleStaysUnset)
{
    ScopedEnvironmentVariable savedLanguage("LANG");
    ScopedEnvironmentVariable savedLocaleAll("LC_ALL");
    ScopedEnvironmentVariable savedLocaleCharacterType("LC_CTYPE");
    ::unsetenv("LANG");
    ::unsetenv("LC_ALL");
    ::unsetenv("LC_CTYPE");

    std::string output = outputFromArguments({ "/usr/bin/printenv", "LC_CTYPE" }, PAL::CharacterLocalePolicy::Preserve);
    CHECK(output.find("C.UTF-8") == std::string::npos);
}

TEST(testExplicitCharacterLocaleIsPreserved)
{
    ScopedEnvironmentVariable savedLanguage("LANG");
    ScopedEnvironmentVariable savedLocaleAll("LC_ALL");
    ScopedEnvironmentVariable savedLocaleCharacterType("LC_CTYPE");
    ::unsetenv("LANG");
    ::unsetenv("LC_ALL");
    ::setenv("LC_CTYPE", "C", 1);

    std::string output = outputFromArguments({ "/usr/bin/printenv", "LC_CTYPE" });
    CHECK(output.find("C.UTF-8") == std::string::npos);
    CHECK(output.find("C") != std::string::npos);
}

TEST(testForcedCharacterLocaleOverridesLCAll)
{
    ScopedEnvironmentVariable savedLanguage("LANG");
    ScopedEnvironmentVariable savedLocaleAll("LC_ALL");
    ScopedEnvironmentVariable savedLocaleCharacterType("LC_CTYPE");
    ::setenv("LANG", "C", 1);
    ::setenv("LC_ALL", "C", 1);
    ::setenv("LC_CTYPE", "C", 1);

    std::string output = outputFromArguments({ "/usr/bin/locale", "charmap" }, PAL::CharacterLocalePolicy::UTF8);
    CHECK(output.find("UTF-8") != std::string::npos);
}

// The title poll uses process().foregroundProcessGroup() as a cheap change-detector:
// it must resolve a live job to a real pgid (>0) and return the same value across
// calls while that job holds the terminal (so the costly chain walk is skipped).
TEST(testForegroundProcessGroup)
{
    auto s = openArguments({ "/bin/cat" });
    CHECK(s != nullptr);
    if (!s)
        return;

    // Let cat fork+exec and take the controlling tty's foreground group.
    char buffer[64];
    int finalStatus = 0;
    drainWithBudget(s.get(), buffer, sizeof(buffer), 200, &finalStatus);

    std::optional<PAL::ProcessID> a = s->process().foregroundProcessGroup();
    std::optional<PAL::ProcessID> b = s->process().foregroundProcessGroup();
    CHECK(a.has_value());
    CHECK(a == b); // stable while the same job is foreground
    std::printf("fg-pgrp: %d (stable across calls)\n", a.value_or(0));
}

// A program the caller forked itself announces its tree where the platform has
// an unprivileged source of fork and exec edges: the daemon's sessions are such
// programs, and on this edge the daemon learns that a program took the terminal
// without printing, instead of polling for it every second. The shell here
// prints nothing, sleeps, then forks and execs a second silent program. Linux
// has no such source short of ptrace or a netlink connector that needs
// CAP_NET_ADMIN, so there the stream says it delivers none and the title walk
// keeps its fallback poll.
TEST(testOwnProgramDeliversTreeEdges)
{
    auto s = openArguments({ "/bin/sh", "-c", "sleep 0.3; exec /bin/sleep 1" });
    CHECK(s != nullptr);
    if (!s)
        return;
#if defined(__APPLE__)
    CHECK_MSG(s->process().deliversTreeEdges(), "a program this process forked does not deliver tree edges, so a silent foreground change waits for the fallback poll");
    struct Edges {
        std::atomic<int> count { 0 };
    };
    auto edges = std::make_unique<Edges>();
    Edges* observed = edges.get();
    s->process().setTreeChangeNotifier(
        [](void* context) {
            static_cast<Edges*>(context)->count.fetch_add(1, std::memory_order_relaxed);
            return true;
        },
        edges.release(), [](void* context) { std::unique_ptr<Edges>(static_cast<Edges*>(context)).reset(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (observed->count.load(std::memory_order_relaxed) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_MSG(observed->count.load(std::memory_order_relaxed) > 0, "a silent fork and exec under the program raised no tree edge");
#else
    CHECK_MSG(!s->process().deliversTreeEdges(), "a stream with no tracker claims tree edges, so a silent foreground change would wait for an edge that never comes");
#endif
}

// Whether waitForTrackedProcessID polls for the process to appear or to vanish.
enum class Presence {
    Present,
    Absent
};

// Poll gatherSubtree (which reads the kqueue tracker's live set) until
// `processID` reaches `expected` presence, or the timeout elapses — exercising
// the whole title path, not just the raw tracker. Returns whether the state
// was reached.
static bool waitForTrackedProcessID(PAL::ProcessTracker* tracker, PAL::ProcessID leader, PAL::ProcessID processID, Presence expected, int timeoutMilliseconds)
{
    for (int waited = 0; waited <= timeoutMilliseconds; waited += 20) {
        PAL::Process::Record records[64];
        int n = PAL::gatherSubtree(leader, 0, "", { records, 64 }, tracker);
        bool found = false;
        for (int i = 0; i < n; ++i)
            if (records[i].processID == static_cast<uint32_t>(processID)) {
                found = true;
                break;
            }
        if ((found ? Presence::Present : Presence::Absent) == expected)
            return true;
        ::usleep(20000);
    }
    return false;
}

// adoptPTY's ownership check. Being a valid PTY master (S_ISCHR +
// TIOCGWINSZ) proves nothing about who legitimately owns it — a same-uid peer
// could pass an fd to an UNRELATED session's tty replica. Spawns two
// independent PTY sessions directly via forkpty (each child becomes its own
// session leader), then proves: (a) this test process is a real ancestor of
// its own forkpty child's session — the legitimate-owner case; (b) child A is
// not an ancestor of sibling child B's session — the exploit shape (one peer
// claiming ownership of an unrelated session); (c) adoptPTY itself accepts
// the legitimate claim and refuses the stolen one end-to-end.
TEST(testAdoptPtyOwnership)
{
    UbiquittyTest::OwnedChildProcess<> ownerA;
    UbiquittyTest::OwnedChildProcess<> ownerB;
    REQUIRE(ownerA.canOwnChildren() && ownerB.canOwnChildren());
    int masterA = -1;
    const pid_t childA = ::forkpty(&masterA, nullptr, nullptr, nullptr);
    if (childA == 0) {
        for (;;)
            ::pause();
        _exit(0);
    }
    PAL::UnixFileDescriptor masterOwnerA(masterA);
    REQUIRE(ownerA.adoptForkResult(childA) && masterOwnerA);

    int masterB = -1;
    const pid_t childB = ::forkpty(&masterB, nullptr, nullptr, nullptr);
    if (childB == 0) {
        masterOwnerA.reset();
        for (;;)
            ::pause();
        _exit(0);
    }
    PAL::UnixFileDescriptor masterOwnerB(masterB);
    REQUIRE(ownerB.adoptForkResult(childB) && masterOwnerB);

    // forkpty returns in the parent before the child establishes its session.
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (::tcgetsid(masterA) == childA && ::tcgetsid(masterB) == childB) {
            break;
        }
        ::usleep(10000);
    }
    REQUIRE(::tcgetsid(masterA) == childA);
    REQUIRE(::tcgetsid(masterB) == childB);

    const PAL::ProcessID self = static_cast<PAL::ProcessID>(::getpid());
    const PAL::ProcessID pidA = static_cast<PAL::ProcessID>(childA);
    const PAL::ProcessID pidB = static_cast<PAL::ProcessID>(childB);

    CHECK(PAL::isProcessAncestorOfSession(self, pidA));
    CHECK(!PAL::isProcessAncestorOfSession(pidA, pidB));

    // Legitimate: the test process really did fork child A's session.
    auto legit = PAL::adoptPTY(masterOwnerA.release(), self);
    CHECK(legit != nullptr);

    // Stolen: child A claiming ownership of sibling child B's session —
    // adoptPTY must refuse (and it owns/closes masterB on this failure path).
    auto stolen = PAL::adoptPTY(masterOwnerB.release(), pidA);
    CHECK(stolen == nullptr);

    legit.reset(); // childProcessID == -1 for an adopted stream: no SIGHUP/reap here
    CHECK(ownerA.stop());
    CHECK(ownerB.stop());
    std::printf("adopt-pty-ownership: adoptPTY accepts a claimed owner that really is an "
                "ancestor and refuses one that isn't\n");
}

// Each descendant exits when its parent's private release pipe closes. Only
// that parent may signal or reap it; reported process IDs grant no authority.
class TrackerSubtree {
    struct Descendant {
        UbiquittyTest::OwnedChildProcess<> owner;
        PAL::UnixFileDescriptor release;

        bool start(std::span<const int> unrelatedDescriptors)
        {
            auto pipe = PAL::makeUnixPipe();
            if (!pipe || !owner.canOwnChildren())
                return false;
            const pid_t process = ::fork();
            if (!process) {
                pipe.write.reset();
                for (const int descriptor : unrelatedDescriptors) {
                    if (descriptor >= 0)
                        (void)::close(descriptor);
                }
                char byte;
                while (::read(pipe.read.get(), &byte, 1) < 0 && errno == EINTR) {
                }
                ::_exit(0);
            }
            if (!owner.adoptForkResult(process))
                return false;
            release = std::move(pipe.write);
            return true;
        }

        bool stop()
        {
            release.reset();
            return owner.stop();
        }
    };

public:
    ~TrackerSubtree()
    {
        m_commands.reset();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (m_owner.processID() > 1 && m_owner.reap(WNOHANG) == 0 && std::chrono::steady_clock::now() < deadline)
            ::usleep(1000);
    }

    bool start()
    {
        auto commands = makeControlPair();
        auto reports = makeControlPair();
        if (!commands[0] || !reports[0] || !m_owner.canOwnChildren())
            return false;
        const pid_t process = ::fork();
        if (!process) {
            commands[1].reset();
            reports[0].reset();
            ::_exit(run(commands[0].get(), reports[1].get()));
        }
        if (!m_owner.adoptForkResult(process))
            return false;
        m_commands = std::move(commands[1]);
        m_reports = std::move(reports[0]);
        return true;
    }

    pid_t processID() const { return m_owner.processID(); }

    std::optional<pid_t> receiveProcessID()
    {
        return UbiquittyTest::readReportedProcessID([&](void* bytes, std::size_t size) -> ssize_t {
            if (!waitForReport())
                return -1;
            return ::read(m_reports.get(), bytes, size);
        });
    }

    bool startLate() { return sendControl(m_commands.get(), "l", 1); }

    bool stopEarly()
    {
        if (!sendControl(m_commands.get(), "e", 1) || !waitForReport())
            return false;
        char answer = 0;
        return ::read(m_reports.get(), &answer, 1) == 1 && answer == 'e';
    }

private:
    static std::array<PAL::UnixFileDescriptor, 2> makeControlPair()
    {
        int descriptors[2] = { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0)
            return {};
        std::array endpoints { PAL::UnixFileDescriptor(descriptors[0]), PAL::UnixFileDescriptor(descriptors[1]) };
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        for (const auto& endpoint : endpoints) {
            if (::setsockopt(endpoint.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
                return {};
        }
#elif !defined(MSG_NOSIGNAL)
        return {};
#endif
        return endpoints;
    }

    // A failed peer never raises SIGPIPE in the fixture's process.
    static bool sendControl(int descriptor, const void* bytes, std::size_t size)
    {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        ssize_t result;
        do {
            result = ::send(descriptor, bytes, size, flags);
        } while (result < 0 && errno == EINTR);
        return result == static_cast<ssize_t>(size);
    }

    // Waits for the report itself rather than for a time it should have come
    // by: each report follows a fork, and under load a fork and the scheduling
    // of its child can outlast a wait sized for an idle machine. A subtree
    // that dies closes the socket, which ends the wait at once, so the bound
    // only turns a lost report into a failure instead of a hang.
    bool waitForReport()
    {
        pollfd descriptor { m_reports.get(), POLLIN, 0 };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        int result;
        do {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            result = ::poll(&descriptor, 1, static_cast<int>(std::max<std::chrono::milliseconds::rep>(remaining.count(), 0)));
        } while (result < 0 && errno == EINTR);
        return result == 1 && (descriptor.revents & POLLIN);
    }

    static int run(int commands, int reports)
    {
        Descendant early;
        Descendant late;
        const std::array descriptors { commands, reports };
        if (!early.start(descriptors))
            return 20;
        const auto report = [&](pid_t process) { return sendControl(reports, &process, sizeof(process)); };
        if (!report(early.owner.processID()))
            return 21;
        char command = 0;
        for (;;) {
            ssize_t count;
            do {
                count = ::read(commands, &command, 1);
            } while (count < 0 && errno == EINTR);
            if (count == 0)
                return 0;
            if (count != 1)
                return 22;
            if (command == 'l' && late.owner.processID() <= 1) {
                const std::array lateDescriptors { commands, reports, early.release.get() };
                if (!late.start(lateDescriptors) || !report(late.owner.processID()))
                    return 23;
            } else if (command == 'e') {
                if (!early.stop() || !sendControl(reports, "e", 1))
                    return 24;
            } else
                return 25;
        }
    }

    UbiquittyTest::OwnedChildProcess<> m_owner;
    PAL::UnixFileDescriptor m_commands;
    PAL::UnixFileDescriptor m_reports;
};

// kqueue/NOTE_TRACK tracker: a child forks one grandchild before the tracker
// starts (must be caught by the registration seed) and one after (must be caught
// by NOTE_CHILD); killing a grandchild must drop it; stop must join cleanly.
TEST(testProcessTrackerFollowsSubtree)
{
    TrackerSubtree subtree;
    REQUIRE(subtree.start());
    const pid_t child = subtree.processID();
    const auto early = subtree.receiveProcessID();
    REQUIRE(early);
    const pid_t earlyGrandchild = *early;

    // The live kqueue tracker is a Darwin capability: PTYFacts.h documents
    // startProcessTracker returning nullptr on a Host build, where the
    // gatherSubtree calls below fall back to the /proc scan. The subtree-following
    // assertions hold either way — a tracker's live set and a scan answer the
    // same membership — so only the tracker's existence is asserted per platform.
    const std::unique_ptr<PAL::ProcessTracker, decltype(&PAL::stopProcessTracker)> tracker(PAL::startProcessTracker(child), PAL::stopProcessTracker);
#if defined(__APPLE__)
    CHECK(tracker != nullptr);
#endif

    REQUIRE(subtree.startLate());
    const auto late = subtree.receiveProcessID();
    REQUIRE(late);
    const pid_t lateGrandchild = *late;

    CHECK(waitForTrackedProcessID(tracker.get(), child, earlyGrandchild, Presence::Present, 3000));
    CHECK(waitForTrackedProcessID(tracker.get(), child, lateGrandchild, Presence::Present, 3000));

    REQUIRE(subtree.stopEarly());
    CHECK(waitForTrackedProcessID(tracker.get(), child, earlyGrandchild, Presence::Absent, 3000));
    CHECK(waitForTrackedProcessID(tracker.get(), child, lateGrandchild, Presence::Present, 100));
    std::printf("process-tracker: seed + NOTE_CHILD + NOTE_EXIT ok\n");
}

static constexpr const char* reportArgument0Marker = "--ubiquitty-report-argument-zero";
// The arms below run this binary as a program a test's shell starts, standing
// in for one whose behaviour no system program has.
static constexpr const char* sleepsMarker = "--ubiquitty-sleeps";
static constexpr const char* mainThreadExitsMarker = "--ubiquitty-main-thread-exits";
#if defined(__linux__)
static constexpr const char* renamesItselfMarker = "--ubiquitty-renames-itself";
#endif
// This binary's path, absolute. The cases below relaunch it in a PTY, whose
// child does not start in this process's directory, so a relative argv[0]
// names nothing there.
static std::string executablePath;

// argv[0] made absolute against the directory this process started in. A
// name with no slash was found on PATH and is kept, since the launch resolves
// it the same way.
static std::string absoluteExecutablePath(const char* argument0)
{
    if (!std::strchr(argument0, '/'))
        return argument0;
    char resolved[PATH_MAX];
    return ::realpath(argument0, resolved) ? std::string(resolved) : std::string(argument0);
}

// The child's argv[0], read back from the child itself. The test binary's
// reporter arm has no startup files or command-language interpretation, so
// the PTY launch contract is the only input to the value it returns.
static std::string argument0AsSeenByTheChild(PAL::Argument0Style style)
{
    std::vector<std::string> arguments { executablePath, reportArgument0Marker };
    PAL::PTYLaunchOptions options;
    options.argument0Style = style;
    std::unique_ptr<PAL::Stream> stream = PAL::openPTY(arguments, options);
    if (!stream)
        return {};
    char buffer[256];
    int finalStatus = 0;
    const int got = drainWithBudget(stream.get(), buffer, sizeof(buffer) - 1, 2000, &finalStatus);
    buffer[got > 0 ? got : 0] = '\0';
    return { buffer };
}

TEST(testALoginShellGetsTheDashedArgument0)
{
    // The leading '-' is what tells an sh-like shell to source the login
    // profiles, so the shell a terminal opens by itself keeps it.
    const std::string seen = argument0AsSeenByTheChild(PAL::Argument0Style::LoginShell);
    const std::size_t basenameOffset = executablePath.find_last_of('/');
    const std::string basename = basenameOffset == std::string::npos ? executablePath : executablePath.substr(basenameOffset + 1);
    const std::string expected = "[-" + basename + "]";
    CHECK_MSG(seen.find(expected) != std::string::npos, seen.c_str());
}

TEST(testANamedProgramGetsThePathItWasGiven)
{
    // A program named on a command line is not a login shell. One that reads
    // its own argv[0] — busybox, any multi-call binary, anything printing
    // usage — sees a name it does not have when the '-' is forced on.
    const std::string seen = argument0AsSeenByTheChild(PAL::Argument0Style::ProgramPath);
    const std::string expected = "[" + executablePath + "]";
    CHECK_MSG(seen.find(expected) != std::string::npos, seen.c_str());
}

TEST(testTheArgument0CasesPassWhenTheSuiteIsStartedByARelativePath)
{
    // The suite run as `./test_pty` from its own directory, the way a person
    // runs it, one argv[0] case at a time.
    const std::size_t slash = executablePath.find_last_of('/');
    CHECK_MSG(slash != std::string::npos, "premise: the suite knows its own directory");
    if (slash == std::string::npos)
        return;
    const std::string directory = executablePath.substr(0, slash);
    const std::string relative = "./" + executablePath.substr(slash + 1);
    for (const char* only : { "GetsTheDashedArgument0", "GetsThePathItWasGiven" }) {
        const pid_t child = ::fork();
        if (child == 0) {
            if (::chdir(directory.c_str()) != 0)
                _exit(126);
            char* arguments[] = { const_cast<char*>(relative.c_str()), const_cast<char*>(only), nullptr };
            ::execv(relative.c_str(), arguments);
            _exit(127);
        }
        int status = 0;
        CHECK(child > 0 && ::waitpid(child, &status, 0) == child);
        CHECK_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 0, (std::string("started as ") + relative + ", " + only + " failed").c_str());
    }
}

// How long a ^C written to a flooding program's terminal takes to end it, read
// the way a frontend reads: one thread drains the output into a terminal and
// writes its replies back, and another writes the keystroke.
//
// - Parameter kind: What the flood writes; `Querying` asks for the device
//   attributes on every line, so the terminal's replies fill the input of a
//   program that never reads it.
// - Returns: Milliseconds from the write to the program's exit, or a negative
//   number when it had not exited after five seconds.
enum class FloodKind : std::uint8_t {
    Plain,
    Querying
};

static double interruptLatencyUnderFlood(FloodKind kind)
{
    const std::string script = std::string("$| = 1; my $line = ") + (kind == FloodKind::Querying ? "\"\\e[c\" . " : "") + "('x' x 100) . \"\\n\"; print $line while 1;";
    std::vector<std::string> arguments { "/usr/bin/perl", "-e", script };
    std::unique_ptr<PAL::Stream> stream = PAL::openPTY(arguments);
    if (!stream)
        return -2;
    std::atomic<bool> isDraining { true };
    std::thread drainer([&] {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 24, 80);
        char buffer[16384];
        char replies[4096];
        while (isDraining.load(std::memory_order_relaxed)) {
            const int n = stream->read({ buffer, sizeof(buffer) });
            if (n > 0) {
                Ubiquitty::processBytes(&terminal, std::string_view { buffer, static_cast<std::size_t>(n) });
                for (int r; (r = Ubiquitty::drainOutput(&terminal, replies)) > 0;)
                    (void)stream->writeAll({ replies, static_cast<std::size_t>(r) });
                (void)stream->drainPendingWrite();
                continue;
            }
            if (n != status(PAL::StreamStatus::Again))
                break;
            (void)stream->drainPendingWrite();
            ::usleep(200);
        }
    });
    ::usleep(1'000'000);
    const auto sent = std::chrono::steady_clock::now();
    (void)stream->writeAll({ "\x03", 1 });
    double latency = -1;
    while (std::chrono::steady_clock::now() - sent < std::chrono::seconds(5)) {
        if (!stream->process().hasRunningChild()) {
            latency = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sent).count();
            break;
        }
        ::usleep(500);
    }
    isDraining.store(false, std::memory_order_relaxed);
    drainer.join();
    return latency;
}

TEST(testAnInterruptEndsAFloodingProgramPromptly)
{
    // A ^C is how a person ends a flood, so it may not wait behind the output
    // or behind input the flooding program never reads.
    for (const FloodKind kind : { FloodKind::Plain, FloodKind::Querying }) {
        const char* name = kind == FloodKind::Querying ? "querying" : "plain";
        const double latency = interruptLatencyUnderFlood(kind);
        std::printf("pty: ^C under a %s flood ended it in %.1f ms\n", name, latency);
        CHECK_MSG(latency >= 0 && latency < 250, (std::string(name) + " flood: ^C took " + std::to_string(latency) + " ms").c_str());
    }
}

// A program in a pane starts with every signal at its default and none
// blocked, whatever the process that opened the terminal had. A daemon started
// in the background of a script inherits SIGINT ignored, and ignores SIGPIPE of
// its own accord; a shell passes an ignored signal on to everything it runs, so
// ^C reached the flooding program and did nothing, and a pipeline's writer
// outlived its reader.
TEST(aPaneProgramStartsWithDefaultSignalsWhateverItsParentIgnored)
{
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    struct sigaction savedInterrupt {}, savedPipe {};
    ::sigaction(SIGINT, &ignore, &savedInterrupt);
    ::sigaction(SIGPIPE, &ignore, &savedPipe);
    sigset_t blocked, savedMask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGQUIT);
    ::pthread_sigmask(SIG_BLOCK, &blocked, &savedMask);

    const std::string seen = outputFromArguments({ "/usr/bin/perl", "-e",
        "use POSIX; my $m = POSIX::SigSet->new; sigprocmask(SIG_BLOCK, POSIX::SigSet->new, $m);"
        " printf \"[INT=%s PIPE=%s QUITBLOCKED=%d]\", ($SIG{INT} // 'default'), ($SIG{PIPE} // 'default'), $m->ismember(SIGQUIT)" });
    const double latency = interruptLatencyUnderFlood(FloodKind::Plain);

    ::pthread_sigmask(SIG_SETMASK, &savedMask, nullptr);
    ::sigaction(SIGPIPE, &savedPipe, nullptr);
    ::sigaction(SIGINT, &savedInterrupt, nullptr);

    CHECK_MSG(seen.find("[INT=default PIPE=default QUITBLOCKED=0]") != std::string::npos, seen.c_str());
    CHECK_MSG(latency >= 0 && latency < 250, ("^C under an ignoring parent took " + std::to_string(latency) + " ms").c_str());
}

// What openPTY hands back is the program, already: a reader of its process
// sees its name and its directory, never this process's, and the child holds
// none of this process's descriptors any more. A daemon's listening socket and
// startup lock are such descriptors; a child still holding them after the
// daemon dies keeps the next daemon from starting. The pipe here is opened
// without close-on-exec, as a careless caller's would be, and the soft limit
// is raised to the hard one, so the child's sweep of this process's
// descriptors is as long as it can be.
TEST(aSpawnedProgramIsInItsDirectoryAndHoldsNothingOfItsParentWhenSpawnReturns)
{
    struct rlimit savedLimit {};
    ::getrlimit(RLIMIT_NOFILE, &savedLimit);
    struct rlimit raised = savedLimit;
    raised.rlim_cur = savedLimit.rlim_max;
    (void)::setrlimit(RLIMIT_NOFILE, &raised);

    char directoryTemplate[] = "/tmp/ubq-pty-directory.XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    REQUIRE(directory);
    char resolved[PATH_MAX];
    REQUIRE(::realpath(directory, resolved));

    int ends[2] = { -1, -1 };
    REQUIRE(::pipe(ends) == 0);

    PAL::PTYLaunchOptions options;
    options.initialDirectory = resolved;
    options.argument0Style = PAL::Argument0Style::ProgramPath;
    const std::vector<std::string> arguments { "/bin/sleep", "5" };
    std::unique_ptr<PAL::Stream> stream = PAL::openPTY(arguments, options);
    // Read at once: nothing may be waited for between spawn and these reads.
    const std::string name = stream ? stream->process().foregroundName() : std::string {};
    const std::string where = stream ? stream->process().foregroundCurrentDirectory() : std::string {};
    ::close(ends[1]);
    pollfd readable { .fd = ends[0], .events = POLLIN, .revents = 0 };
    char byte = 0;
    const bool isWriterGone = ::poll(&readable, 1, 0) == 1 && ::read(ends[0], &byte, 1) == 0;
    ::close(ends[0]);
    stream.reset();
    ::rmdir(resolved);
    (void)::setrlimit(RLIMIT_NOFILE, &savedLimit);

    CHECK_MSG(name == "sleep", ("the spawned program read as \"" + name + "\"").c_str());
    CHECK_MSG(where == resolved, ("the spawned program read as in \"" + where + "\"").c_str());
    CHECK_MSG(isWriterGone, "the child still held a descriptor of its parent when spawn returned");
}

TEST(testTooManyArgumentsIsRefusedRatherThanTruncated)
{
    // A clamp launches the head of the command line against a tail that was
    // dropped: the caller sees a window and the program sees different work.
    // Refusing is the answer the caller can act on.
    std::vector<std::string> arguments { "/bin/echo" };
    for (int i = 0; i < 4096; ++i)
        arguments.emplace_back("x");
    PAL::PTYLaunchOptions options;
    CHECK(PAL::openPTY(arguments, options) == nullptr);

    // The ceiling itself still launches, so the case above fails for the count
    // rather than for anything else being wrong with a long command line.
    std::vector<std::string> justUnder { "/bin/echo" };
    for (int i = 0; i < 200; ++i)
        justUnder.emplace_back("x");
    std::unique_ptr<PAL::Stream> stream = PAL::openPTY(justUnder, options);
    CHECK(stream != nullptr);
}

TEST(testWriteAllQueuesUnderBackpressureAndDrains)
{
    auto s = openArguments({ "/bin/cat" });
    CHECK(s != nullptr);
    if (!s)
        return;

    // One submit larger than the kernel's tty buffers, before anything reads
    // the master: the excess has nowhere to go, so the queue must hold it and
    // report it pending. Lines stay short of the canonical-mode line limit.
    std::string payload;
    const std::string line = std::string(63, 'x') + "\n";
    for (int i = 0; i < 2000; ++i)
        payload += line;
    CHECK(s->writeAll({ payload.data(), payload.size() }) == 0);
    CHECK(s->hasPendingWrite());

    // Drain both directions until the queue empties: cat consumes the replica
    // side only as fast as we consume its echo on the master side.
    char sink[8192];
    long readTotal = 0;
    int ticks = 0;
    while (s->hasPendingWrite() && ticks < 2000) {
        s->drainPendingWrite();
        const int n = s->read(sink);
        if (n > 0) {
            readTotal += n;
        } else if (n == status(PAL::StreamStatus::Again)) {
            ::usleep(2000);
            ++ticks;
        } else {
            break;
        }
    }
    CHECK(!s->hasPendingWrite());
    // ECHO returns our input and cat returns it again, so the master yields
    // at least the payload once by the time the queue is empty.
    CHECK(readTotal > static_cast<long>(payload.size() / 2));

    // End the child: EOF on the replica, then the exit becomes observable.
    CHECK(s->process().hasRunningChild());
    s->writeAll({ "\x04", 1 });
    int finalStatus = 0;
    char rest[8192];
    // Wait for the reap on a generous monotonic deadline, not a fixed tick
    // count. Under a loaded box the child takes seconds to be scheduled and
    // exit, and a ceiling calibrated for an idle machine crosses while the
    // child is merely slow rather than wedged. The healthy path clears in
    // milliseconds, so the deadline is never approached; a real hang still
    // fails, at the deadline. Draining each turn keeps the master readable so
    // the child is not blocked writing its last output before it exits, and
    // the pending write is drained too: writeAll queues the EOF byte when the
    // master cannot take it at once, and a queued EOF reaches the child only
    // through drainPendingWrite.
    const auto reapDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (s->process().hasRunningChild() && std::chrono::steady_clock::now() < reapDeadline) {
        s->drainPendingWrite();
        drainWithBudget(s.get(), rest, sizeof(rest), 5, &finalStatus);
        ::usleep(5000);
    }
    CHECK(!s->process().hasRunningChild());

    // The peer is gone: a plain write reports Closed, and a queued write
    // reports its failure rather than holding bytes for a master nobody reads.
    CHECK(s->write({ "x", 1 }) == status(PAL::StreamStatus::Closed));
    CHECK(s->writeAll({ "x", 1 }) != 0);
}

// A full input queue means the program is not reading stdin yet, not that it
// is gone. The refused write says Again, which a caller can wait out, rather
// than Fatal, which a caller ends the session on.
TEST(testAFullInputQueueRefusesWithoutReportingTheChildGone)
{
    auto s = openArguments({ "/bin/sleep", "30" });
    CHECK(s != nullptr);
    if (!s)
        return;
    const std::string line = std::string(63, 'x') + "\n";
    std::string frame;
    while (frame.size() < 16u * 1024u * 1024u)
        frame += line;
    CHECK(s->writeAll({ frame.data(), frame.size() }) == 0);
    CHECK(s->writeAll({ frame.data(), frame.size() }) == 0);
    const int refused = s->writeAll({ frame.data(), frame.size() });
    CHECK_MSG(refused == status(PAL::StreamStatus::Again), "a full queue reports Again");
    CHECK(s->process().hasRunningChild());
    CHECK(s->hasPendingWrite());
}

TEST(testDuplicatedStreamWriteReportsClosedAfterChildExit)
{
    // The endpoint LocalPTYChannel::openTransport hands its writer is a
    // duplicateAsPollableStream() over a dup'd master — a descriptor wrapper with
    // no handle on the child. Where PTYStream::write reports Closed once the
    // replica hangs up (the case above), a bare write to the dup'd master
    // succeeds into the dead child's buffer on Linux, because the write path's
    // reap check lives on the owner, not on the duplicate. Session::sendInput
    // routes through this endpoint, so a success there is a lie. Pin that the
    // duplicate reports Closed too; on Darwin the master's EIO does so by
    // itself, so the Linux side is the seam this pins.
    auto s = openArguments({ "/bin/cat" });
    CHECK(s != nullptr);
    if (!s)
        return;
    std::unique_ptr<PAL::PollableByteStream> duplicate = s->duplicateAsPollableStream();
    CHECK(duplicate != nullptr);
    if (!duplicate)
        return;

    // Control: the duplicate writes while the child is alive, so the Closed
    // assertion below is a state change, not a stream that never worked.
    CHECK(duplicate->write({ "x\n", 2 }) > 0);

    // End the child: EOF on the replica, then the exit becomes observable.
    CHECK(s->process().hasRunningChild());
    s->writeAll({ "\x04", 1 });
    int finalStatus = 0;
    char rest[8192];
    for (int ticks = 0; ticks < 400 && s->process().hasRunningChild(); ++ticks) {
        drainWithBudget(s.get(), rest, sizeof(rest), 5, &finalStatus);
        if (finalStatus != 0)
            break;
    }
    for (int ticks = 0; ticks < 400 && s->process().hasRunningChild(); ++ticks)
        ::usleep(5000);
    CHECK(!s->process().hasRunningChild());

    // The peer is gone: the write through the duplicate must report failure — a
    // StreamStatus, not a byte count — rather than succeed into a buffer no
    // reader will ever drain. A positive return would have sendInput tell its
    // caller the input landed. Darwin's master write errors here by itself; on
    // Linux a bare write to the dup'd master returns the byte count, which is
    // the seam this pins. Not asserting Closed specifically:
    // PTYStream::write maps the hangup to Closed through its child handle, which
    // the duplicate does not hold — the contract is "not success", by whatever
    // status.
    CHECK(duplicate->write({ "x", 1 }) <= 0);
}

TEST(testDestructorEscalatesPastATrappedHangup)
{
    // Close sends SIGHUP and grants a grace period; a child that traps HUP
    // outlives it and meets SIGKILL. The observable contract: the child stops
    // being a process. Not synchronously with the destructor — that escalation
    // runs on a thread of its own precisely so closing a pane does not wait on
    // a child that cannot be hurried — so this polls for it.
    auto s = openArguments({ "/bin/sh", "-c", "trap '' HUP; echo ready; while :; do sleep 1; done" });
    CHECK(s != nullptr);
    if (!s)
        return;

    char buffer[256];
    int got = 0;
    for (int ticks = 0; ticks < 600 && std::strstr(buffer, "ready") == nullptr; ++ticks) {
        const int n = s->read({ buffer + got, sizeof(buffer) - 1 - static_cast<size_t>(got) });
        if (n > 0) {
            got += n;
            buffer[got] = '\0';
        } else {
            ::usleep(5000);
        }
    }
    CHECK(std::strstr(buffer, "ready") != nullptr); // the trap is installed

    const std::optional<PAL::ProcessID> child = s->process().foregroundProcessGroup();
    CHECK(child.has_value());
    s.reset();
    if (!child)
        return;
    bool childGone = false;
    for (int ticks = 0; ticks < 400 && !childGone; ++ticks) {
        childGone = ::kill(*child, 0) != 0;
        if (!childGone)
            ::usleep(5000);
    }
    CHECK(childGone);
}

#if defined(__APPLE__)
TEST(testClosingALoginBeforeItsShellLeavesNoOrphan)
{
    // macOS login ignores SIGHUP until it has committed to the session. A
    // hangup inside that window is dropped, and a process that exits before
    // the SIGKILL escalation fires leaves the root login and the shell it goes
    // on to fork blocked on a pty whose master is gone. Close straight after
    // the spawn and require the wrapper to stop being a process.
    char user[256];
    const int length = PAL::systemLoginUserName(user, sizeof(user));
    CHECK(length > 0);
    if (length <= 0)
        return;
    auto stream = openArguments({ "/usr/bin/login", "-pf", std::string(user, static_cast<std::size_t>(length)) });
    CHECK(stream != nullptr);
    if (!stream)
        return;
    // forkpty made the child its own session and foreground group, so the
    // group on the master is the wrapper's pid — readable before it has even
    // exec'd login, which is the moment the close has to cope with. The child
    // claims the tty a few hundred microseconds after the fork; wait for that
    // and no longer, so the close still lands inside login's startup.
    PAL::ProcessID wrapper = 0;
    for (int ticks = 0; ticks < 50 && wrapper <= 0; ++ticks) {
        wrapper = stream->process().foregroundProcessGroup().value_or(0);
        if (wrapper <= 0)
            ::usleep(200);
    }
    CHECK(wrapper > 0);
    stream.reset(); // while login still ignores SIGHUP
    if (wrapper <= 0)
        return;
    // login runs as root, so a live one answers EPERM; only ESRCH means gone.
    bool isWrapperGone = false;
    for (int ticks = 0; ticks < 400 && !isWrapperGone; ++ticks) {
        isWrapperGone = ::kill(wrapper, 0) != 0 && errno == ESRCH;
        if (!isWrapperGone)
            ::usleep(5000);
    }
    CHECK(isWrapperGone);
}

TEST(testALoginShellsTerminalCanBeVouchedForWhileLoginRunsAsRoot)
{
    // A window's shell runs under /usr/bin/login, which is the session leader
    // of its terminal and runs as root. Sharing the window hands the master to
    // the daemon, which pins the terminal by its session leader's birth before
    // it adopts; a snapshot that cannot read a root process refused every
    // login shell's terminal, so no window could be shared.
    char user[256];
    const int length = PAL::systemLoginUserName(user, sizeof(user));
    CHECK(length > 0);
    if (length <= 0)
        return;
    auto stream = openArguments({ "/usr/bin/login", "-pfl", std::string(user, static_cast<std::size_t>(length)), "/bin/sh", "-c", "read answer" });
    CHECK(stream != nullptr);
    if (!stream)
        return;
    const int master = stream->fileDescriptor();
    // Until login has exec'd, the leader is still this user's forked child and
    // proves nothing; wait for the kernel to report it running as root.
    const auto runsAsRoot = [](PAL::ProcessID process) {
        std::array mib { CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(process) };
        struct kinfo_proc info {};
        size_t size = sizeof(info);
        return ::sysctl(mib.data(), static_cast<u_int>(mib.size()), &info, &size, nullptr, 0) == 0 && size == sizeof(info)
            && info.kp_eproc.e_ucred.cr_uid == 0;
    };
    PAL::ProcessID leader = 0;
    bool isLeaderRoot = false;
    for (int ticks = 0; ticks < 400 && !isLeaderRoot; ++ticks) {
        leader = static_cast<PAL::ProcessID>(::tcgetsid(master));
        isLeaderRoot = leader > 1 && runsAsRoot(leader);
        if (!isLeaderRoot)
            ::usleep(5000);
    }
    CHECK(isLeaderRoot);
    if (!isLeaderRoot)
        return;
    const auto snapshot = PAL::ptyProcessSnapshot(leader);
    CHECK(snapshot.has_value());
    CHECK(snapshot && snapshot->processGroupID == leader);
    CHECK(snapshot && snapshot->parentProcessID == ::getpid());
    CHECK(snapshot && !(snapshot->birth == PAL::ProcessBirthIdentity {}));
    const auto identity = PAL::capturePTYAdoptionIdentity(master, ::getpid());
    CHECK(identity.has_value());
    CHECK(identity && identity->sessionLeader == leader);
    CHECK(PAL::validatePTYAdoptionIdentity(master, ::getpid(), identity.value_or(PAL::PTYAdoptionIdentity {})));
    CHECK(stream->writeAll(std::span("go\n", 3)) == 0);
}
#endif

TEST(testForegroundQueriesTrackTheJob)
{
    auto s = openArguments({ "/bin/sh" });
    CHECK(s != nullptr);
    if (!s)
        return;

    // At the prompt the foreground group is the shell itself. An unresolvable
    // group also reads as "shell" — that is foregroundIsShell's contract for
    // the moments before the child owns the tty — so readiness is the group
    // RESOLVING, not the shell answer alone.
    int ticks = 0;
    while ((!s->process().foregroundIsShell() || !s->process().foregroundProcessGroup().has_value()) && ticks++ < 400)
        ::usleep(5000);
    CHECK(s->process().foregroundIsShell());
    CHECK(!s->process().foregroundName().empty());
    const std::string atPrompt = s->process().foregroundCurrentDirectory();
    CHECK(!atPrompt.empty() && atPrompt[0] == '/');
    CHECK(!s->process().ttyName().empty());
    CHECK(s->process().foregroundProcessGroup().has_value());

    // A running job moves the foreground group off the shell.
    s->writeAll({ "sleep 5\n", 8 });
    char sink[4096];
    for (ticks = 0; ticks < 600 && s->process().foregroundIsShell(); ++ticks) {
        int finalStatus = 0;
        drainWithBudget(s.get(), sink, sizeof(sink), 5, &finalStatus);
    }
    CHECK(!s->process().foregroundIsShell());
    CHECK(!s->process().foregroundName().empty());
    CHECK(s->process().foregroundProcessGroup().has_value());

    // The subtree walk sees the session: shell, job, or both.
    PAL::Process::Record records[16];
    CHECK(s->process().gatherProcesses(records) >= 1);

    // Interrupt returns the prompt, and with it the shell as foreground.
    s->writeAll({ "\x03", 1 });
    for (ticks = 0; ticks < 600 && !s->process().foregroundIsShell(); ++ticks) {
        int finalStatus = 0;
        drainWithBudget(s.get(), sink, sizeof(sink), 5, &finalStatus);
    }
    CHECK(s->process().foregroundIsShell());
}

// Copies `source` to `path` as a program the shell can run under that name.
// Darwin kills a copied platform binary at exec, so the copy is signed ad hoc
// there, which makes it an ordinary program under its own name.
static bool makeRunnableCopy(const std::string& source, const std::string& path)
{
    std::error_code copyError;
    if (!std::filesystem::copy_file(source, path, std::filesystem::copy_options::overwrite_existing, copyError))
        return false;
#if defined(__APPLE__)
    UbiquittyTest::OwnedChildProcess<> signer;
    if (!signer.canOwnChildren())
        return false;
    const pid_t signing = ::fork();
    if (signing == 0) {
        const int quiet = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (quiet >= 0)
            ::dup2(quiet, STDERR_FILENO);
        ::execl("/usr/bin/codesign", "codesign", "--force", "--sign", "-", path.c_str(), nullptr);
        ::_exit(127);
    }
    if (!signer.adoptForkResult(signing))
        return false;
    int signingStatus = -1;
    return signer.reap(0, &signingStatus) == signing && WIFEXITED(signingStatus) && !WEXITSTATUS(signingStatus);
#else
    std::filesystem::permissions(path, std::filesystem::perms::owner_all, copyError);
    return !copyError;
#endif
}

// A job whose executable no longer reads still holds the terminal, and is named
// by the kernel's name for it, never by the shell's or by another member's: a
// job that is exiting lets go of its file before it is a zombie, and a program
// whose file was deleted has none to read. The deleted file holds the state
// still. The program is a copy of sleep on Darwin, and on Linux a copy of this
// binary in its sleeping arm: the system's sleep there is a multicall binary
// that answers to the name it is run by, which a copy changes.
TEST(aJobWhoseExecutableNoLongerReadsIsNamedByItsCommandNotTheShell)
{
    char directoryTemplate[] = "/tmp/ubq-pty-unlinked.XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    REQUIRE(directory);
    const std::string program = std::string(directory) + "/vanishing";
#if defined(__APPLE__)
    const std::string source = "/bin/sleep";
    const std::string arguments = " 30";
#else
    const std::string source = executablePath;
    const std::string arguments = std::string(" ") + sleepsMarker + " 30";
#endif

    auto stream = openArguments({ "/bin/sh" });
    REQUIRE(stream != nullptr);
    PAL::ProcessFacts& process = stream->process();
    char sink[4096];
    const auto drainUntil = [&](auto&& condition) {
        for (int tick = 0; tick < 2000 && !condition(); ++tick) {
            int finalStatus = 0;
            drainWithBudget(stream.get(), sink, sizeof(sink), 5, &finalStatus);
        }
        return condition();
    };
    REQUIRE(drainUntil([&] { return process.foregroundIsShell() && process.foregroundProcessGroup().has_value(); }));
    // Alone, and leading a pipeline whose other member's file still reads.
    for (const std::string& command : { program + arguments + "\n", program + arguments + " | /bin/cat\n" }) {
        REQUIRE_MSG(makeRunnableCopy(source, program), "premise: the copy runs");
        REQUIRE(stream->writeAll(std::span(command.data(), command.size())) == 0);
        REQUIRE_MSG(drainUntil([&] { return process.foregroundName() == "vanishing"; }), command.c_str());
        REQUIRE(::unlink(program.c_str()) == 0);
        const std::optional<PAL::ProcessID> job = process.foregroundProcessGroup();
        REQUIRE(job.has_value());
#if defined(__APPLE__)
        CHECK_MSG(PAL::processExecutableName(*job).empty(), "premise: a job whose file was deleted has no executable to read");
#endif
        CHECK_STR_EQ(PAL::processCommandName(*job), "vanishing");
        CHECK(!process.foregroundIsShell());
        CHECK_STR_EQ(process.foregroundName(), "vanishing");

        REQUIRE(stream->writeAll(std::span("\x03", 1)) == 0);
        CHECK(drainUntil([&] { return process.foregroundIsShell(); }));
    }
    (void)::rmdir(directory);
}

// The shell's own work is named by the shell, and by the kernel's name for it
// once its file no longer reads, as an upgrade leaves a shell that is running:
// at its prompt and while a subshell of it holds the terminal alike, never by
// the session's leader or by nothing.
TEST(aShellWhoseExecutableNoLongerReadsIsNamedByItsCommandInASubshell)
{
    char directoryTemplate[] = "/tmp/ubq-pty-unlinked-shell.XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    REQUIRE(directory);
    const std::string shellPath = std::string(directory) + "/myshell";
    REQUIRE_MSG(makeRunnableCopy("/bin/bash", shellPath), "premise: the copy runs");

    auto stream = openArguments({ shellPath, "--norc", "--noprofile", "-i" });
    REQUIRE(stream != nullptr);
    PAL::ProcessFacts& process = stream->process();
    char sink[4096];
    const auto drainUntil = [&](auto&& condition) {
        for (int tick = 0; tick < 2000 && !condition(); ++tick) {
            int finalStatus = 0;
            drainWithBudget(stream.get(), sink, sizeof(sink), 5, &finalStatus);
        }
        return condition();
    };
    // macOS ends a shell whose file is gone with SIGKILL when it then forks, so
    // the subshell is started while the file stands, and the file is unlinked
    // once the subshell holds the terminal. A shell the system ended is a
    // premise that did not hold, never a failure of the naming.
    const auto wasKilled = [&] {
        (void)process.hasRunningChild();
        const PAL::ChildExit exit = stream->childExit();
        return exit.didExit && exit.wasSignalled && exit.code == SIGKILL;
    };
    constexpr const char* killedPremise = "premise: the system ended the shell with SIGKILL after its file was unlinked";
    const auto isNamedByTheShell = [&] {
        const bool isNamed = process.foregroundName() == "myshell";
        return isNamed || !wasKilled();
    };
    REQUIRE_MSG(drainUntil([&] { return process.foregroundIsShell() && process.foregroundName() == "myshell" && process.foregroundProcessGroup().has_value(); }),
        "premise: the shell reaches its prompt");
    const std::optional<PAL::ProcessID> shellGroup = process.foregroundProcessGroup();
    REQUIRE(shellGroup.has_value());

    // A subshell running a builtin: a fork of the shell, waiting on the terminal.
    REQUIRE(stream->writeAll(std::span("( read line )\n", 14)) == 0);
    REQUIRE_MSG(drainUntil([&] { return process.foregroundProcessGroup().value_or(0) != *shellGroup; }), "premise: the subshell holds the terminal");
    REQUIRE(::unlink(shellPath.c_str()) == 0);
#if defined(__APPLE__)
    CHECK_MSG(PAL::processExecutableName(*shellGroup).empty(), "premise: a shell whose file was deleted has no executable to read");
#endif
    CHECK(process.foregroundIsShell());
    REQUIRE_MSG(isNamedByTheShell(), killedPremise);
    CHECK_STR_EQ(process.foregroundName(), "myshell");

    REQUIRE(stream->writeAll(std::span("\n", 1)) == 0);
    const bool didReturnToPrompt = drainUntil([&] { return process.foregroundProcessGroup() == shellGroup; });
    REQUIRE_MSG(didReturnToPrompt || !wasKilled(), killedPremise);
    CHECK(didReturnToPrompt);
    REQUIRE_MSG(isNamedByTheShell(), killedPremise);
    CHECK_STR_EQ(process.foregroundName(), "myshell");
    (void)::rmdir(directory);
}

// A foreground group too large to list in full is a job, and is named by a
// program in it, never by the shell. Its leader has gone: the pipeline's first
// commands end and are reaped, leaving xargs and the sleeps it started.
TEST(aForegroundGroupTooLargeToListIsNamedByAProgramInIt)
{
    auto stream = openArguments({ "/bin/sh" });
    REQUIRE(stream != nullptr);
    PAL::ProcessFacts& process = stream->process();
    char sink[4096];
    const auto drainUntil = [&](auto&& condition) {
        for (int tick = 0; tick < 2000 && !condition(); ++tick) {
            int finalStatus = 0;
            drainWithBudget(stream.get(), sink, sizeof(sink), 5, &finalStatus);
        }
        return condition();
    };
    REQUIRE(drainUntil([&] { return process.foregroundIsShell() && process.foregroundProcessGroup().has_value(); }));
    const std::optional<PAL::ProcessID> shellGroup = process.foregroundProcessGroup();
    constexpr std::string_view command = "/usr/bin/yes 30 | /usr/bin/head -n 60 | /usr/bin/xargs -P 60 -n 1 /bin/sleep\n";
    REQUIRE(stream->writeAll(command) == 0);
    std::array<PAL::Process::GroupMember, 32> members;
    std::optional<std::size_t> count;
    const auto isLargeAndLeaderless = [&] {
        const std::optional<PAL::ProcessID> group = process.foregroundProcessGroup();
        if (!group || group == shellGroup || PAL::processGroupMember(*group))
            return false;
        members = {};
        count = PAL::processGroupMembers(*group, members);
        return count && *count > members.size();
    };
    REQUIRE_MSG(drainUntil(isLargeAndLeaderless), "premise: xargs and its sleeps hold the terminal without their leader");
    CHECK_MSG(std::ranges::none_of(members, [](const PAL::Process::GroupMember& member) { return member.processID == 0; }), "a group the buffer cannot hold fills it");
    // The listing stops at the first member past the buffer, so the cost of a
    // read is bounded by the caller's buffer, not by the group.
    CHECK_EQ(*count, members.size() + 1);
    CHECK(!process.foregroundIsShell());
    const std::string name = process.foregroundName();
    CHECK_MSG(name == "sleep" || name == "xargs", ("named by a program in the group (named \"" + name + "\")").c_str());

    REQUIRE(stream->writeAll(std::span("\x03", 1)) == 0);
    CHECK(drainUntil([&] { return process.foregroundProcessGroup() == shellGroup; }));
}

// A job whose group a fork of the shell leads is named by the oldest member
// that makes it a job, so the programs that come and go inside it do not
// rename it, whatever order the platform lists the group in: Darwin lists
// newest first, Linux by process ID.
TEST(aJobLedByAForkIsNamedByItsOldestProgram)
{
    auto stream = openArguments({ "/bin/sh" });
    REQUIRE(stream != nullptr);
    PAL::ProcessFacts& process = stream->process();
    char sink[4096];
    const auto drainUntil = [&](auto&& condition) {
        for (int tick = 0; tick < 2000 && !condition(); ++tick) {
            int finalStatus = 0;
            drainWithBudget(stream.get(), sink, sizeof(sink), 5, &finalStatus);
        }
        return condition();
    };
    REQUIRE(drainUntil([&] { return process.foregroundIsShell() && process.foregroundProcessGroup().has_value(); }));
    const std::optional<PAL::ProcessID> shellGroup = process.foregroundProcessGroup();
    constexpr std::string_view command = "( cd /tmp && while :; do /bin/sleep 0.05; /bin/ls >/dev/null; /usr/bin/true; done ) | /bin/cat\n";
    REQUIRE(stream->writeAll(command) == 0);
    REQUIRE_MSG(drainUntil([&] { return process.foregroundProcessGroup() != shellGroup && !process.foregroundIsShell() && process.foregroundName() == "cat"; }),
        "premise: the pipeline holds the terminal");
    int samples = 0;
    int samplesMisnamed = 0;
    std::string misnamed;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500)) {
        int finalStatus = 0;
        drainWithBudget(stream.get(), sink, sizeof(sink), 1, &finalStatus);
        const std::string name = process.foregroundName();
        ++samples;
        if (name != "cat") {
            ++samplesMisnamed;
            misnamed = name;
        }
    }
    std::printf("  a job led by a fork: %d samples, %d misnamed\n", samples, samplesMisnamed);
    CHECK_MSG(samplesMisnamed == 0, ("named \"" + misnamed + "\" rather than its oldest program").c_str());

    REQUIRE(stream->writeAll(std::span("\x03", 1)) == 0);
    CHECK(drainUntil([&] { return process.foregroundProcessGroup() == shellGroup; }));
}

// A nested shell is a job of the session's shell, and so is everything it
// runs: its subshells and prompt hooks lead groups of their own with forks of
// that shell, which run its code, not the session shell's. Such a group holds
// the terminal as a program does and is named by the nested shell.
TEST(aNestedShellsSubshellIsAJobNamedByThatShell)
{
    REQUIRE_MSG(::access("/bin/zsh", X_OK) == 0, "premise: zsh is installed; Scripts/run-linux.sh's recipe installs it");
    auto stream = openArguments({ "/bin/bash", "--norc", "--noprofile", "-i" });
    REQUIRE(stream != nullptr);
    PAL::ProcessFacts& process = stream->process();
    char sink[4096];
    const auto drainUntil = [&](auto&& condition) {
        for (int tick = 0; tick < 2000 && !condition(); ++tick) {
            int finalStatus = 0;
            drainWithBudget(stream.get(), sink, sizeof(sink), 5, &finalStatus);
        }
        return condition();
    };
    REQUIRE(drainUntil([&] { return process.foregroundIsShell() && process.foregroundName() == "bash"; }));
    const std::optional<PAL::ProcessID> outerGroup = process.foregroundProcessGroup();
    REQUIRE(outerGroup.has_value());

    REQUIRE(stream->writeAll(std::string_view("/bin/zsh -f\r")) == 0);
    REQUIRE_MSG(drainUntil([&] { return process.foregroundProcessGroup() != outerGroup && !process.foregroundIsShell() && process.foregroundName() == "zsh"; }),
        "premise: the nested shell holds the terminal at its prompt");
    const std::optional<PAL::ProcessID> nestedGroup = process.foregroundProcessGroup();

    REQUIRE(stream->writeAll(std::string_view("( while :; do :; done )\r")) == 0);
    REQUIRE_MSG(drainUntil([&] {
        const std::optional<PAL::ProcessID> group = process.foregroundProcessGroup();
        return group && group != outerGroup && group != nestedGroup;
    }),
        "premise: the nested shell's subshell holds the terminal");
    int samplesAsShell = 0;
    int samplesMisnamed = 0;
    std::string misnamed;
    for (int sample = 0; sample < 200; ++sample) {
        int finalStatus = 0;
        drainWithBudget(stream.get(), sink, sizeof(sink), 1, &finalStatus);
        if (process.foregroundIsShell())
            ++samplesAsShell;
        const std::string name = process.foregroundName();
        if (name != "zsh") {
            ++samplesMisnamed;
            misnamed = name;
        }
    }
    CHECK_MSG(samplesAsShell == 0, "a nested shell's subshell read as the session shell's own work");
    CHECK_MSG(samplesMisnamed == 0, ("named \"" + misnamed + "\" rather than the nested shell").c_str());

    REQUIRE(stream->writeAll(std::span("\x03", 1)) == 0);
    CHECK(drainUntil([&] { return process.foregroundProcessGroup() == nestedGroup; }));
    REQUIRE(stream->writeAll(std::string_view("exit\r")) == 0);
    CHECK(drainUntil([&] { return process.foregroundProcessGroup() == outerGroup; }));
}

#if defined(__linux__)
// The run state Linux reports for a process, from /proc/<pid>/stat.
static char linuxProcessState(PAL::ProcessID processID)
{
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(processID));
    std::FILE* file = std::fopen(path, "r");
    if (!file)
        return 0;
    char line[512] {};
    const std::size_t length = std::fread(line, 1, sizeof(line) - 1, file);
    std::fclose(file);
    const std::string_view text { line, length };
    const std::size_t commEnd = text.rfind(')');
    return commEnd != std::string_view::npos && commEnd + 2 < text.size() ? text[commEnd + 2] : 0;
}

// A program whose first thread has ended runs on in its others, and Linux
// reports the process as a zombie for as long as they run. It is a job, named
// by its own name, and not the shell's work.
TEST(aJobWhoseFirstThreadEndedStillHoldsTheTerminal)
{
    auto stream = openArguments({ "/bin/sh" });
    REQUIRE(stream != nullptr);
    PAL::ProcessFacts& process = stream->process();
    char sink[4096];
    const auto drainUntil = [&](auto&& condition) {
        for (int tick = 0; tick < 2000 && !condition(); ++tick) {
            int finalStatus = 0;
            drainWithBudget(stream.get(), sink, sizeof(sink), 5, &finalStatus);
        }
        return condition();
    };
    REQUIRE(drainUntil([&] { return process.foregroundIsShell() && process.foregroundProcessGroup().has_value(); }));
    const std::optional<PAL::ProcessID> shellGroup = process.foregroundProcessGroup();
    const std::string command = executablePath + " " + mainThreadExitsMarker + " 30\n";
    REQUIRE(stream->writeAll(command) == 0);
    REQUIRE_MSG(drainUntil([&] {
        const std::optional<PAL::ProcessID> group = process.foregroundProcessGroup();
        return group && group != shellGroup && linuxProcessState(*group) == 'Z';
    }),
        "premise: the program runs on with its first thread gone, which Linux reports as Z");
    const std::size_t slash = executablePath.find_last_of('/');
    const std::string name = slash == std::string::npos ? executablePath : executablePath.substr(slash + 1);
    CHECK(!process.foregroundIsShell());
    CHECK_STR_EQ(process.foregroundName(), name);

    REQUIRE(stream->writeAll(std::span("\x03", 1)) == 0);
    CHECK(drainUntil([&] { return process.foregroundProcessGroup() == shellGroup; }));
}

// A program can rename itself through the name the kernel keeps for it, as
// tmux does on Linux ("tmux: client"). It is named by the name it was run by,
// which is what a pane's tmux routing and the process chain read.
TEST(aProgramThatRenamesItselfIsNamedByTheNameItWasRunBy)
{
    char directoryTemplate[] = "/tmp/ubq-pty-renamed.XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    REQUIRE(directory);
    const std::string program = std::string(directory) + "/tmux";
    REQUIRE(::symlink(executablePath.c_str(), program.c_str()) == 0);

    auto stream = openArguments({ "/bin/sh" });
    REQUIRE(stream != nullptr);
    PAL::ProcessFacts& process = stream->process();
    char sink[4096];
    const auto drainUntil = [&](auto&& condition) {
        for (int tick = 0; tick < 2000 && !condition(); ++tick) {
            int finalStatus = 0;
            drainWithBudget(stream.get(), sink, sizeof(sink), 5, &finalStatus);
        }
        return condition();
    };
    REQUIRE(drainUntil([&] { return process.foregroundIsShell() && process.foregroundProcessGroup().has_value(); }));
    const std::optional<PAL::ProcessID> shellGroup = process.foregroundProcessGroup();
    const std::string command = program + " " + renamesItselfMarker + " 'tmux: client' 30\n";
    REQUIRE(stream->writeAll(command) == 0);
    REQUIRE_MSG(drainUntil([&] {
        const std::optional<PAL::ProcessID> group = process.foregroundProcessGroup();
        return group && group != shellGroup && PAL::processCommandName(*group) == "tmux: client";
    }),
        "premise: the program renamed itself");
    CHECK(!process.foregroundIsShell());
    CHECK_STR_EQ(process.foregroundName(), "tmux");

    REQUIRE(stream->writeAll(std::span("\x03", 1)) == 0);
    CHECK(drainUntil([&] { return process.foregroundProcessGroup() == shellGroup; }));
    (void)::unlink(program.c_str());
    (void)::rmdir(directory);
}
#endif

// A master the daemon adopts when a window is shared has no child of the
// daemon's: the app that spawned the shell stays its parent and reaper. The
// shell still leads the session the master names, so at its prompt it reads as
// the shell, as it does to the stream that spawned it, and a program it runs
// reads as a job. Each resize of a shared pane asks which: the shell's screen
// reflows, and a job's keeps its rows.
TEST(anAdoptedMasterReadsItsShellAtThePromptAsTheShell)
{
    UbiquittyTest::OwnedChildProcess<> owner;
    REQUIRE(owner.canOwnChildren());
    // No ENV or rc file of the machine's user is read by the shell.
    char path[] = "PATH=/usr/bin:/bin";
    char* environment[] = { path, nullptr };
    int master = -1;
    const pid_t child = ::forkpty(&master, nullptr, nullptr, nullptr);
    if (child == 0) {
        // A suite started in the background of a script inherits SIGINT and
        // SIGQUIT ignored, and the shell's job would ignore the interrupt.
        struct sigaction byDefault {};
        byDefault.sa_handler = SIG_DFL;
        for (const int signal : { SIGINT, SIGQUIT, SIGHUP, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGPIPE })
            ::sigaction(signal, &byDefault, nullptr);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::execle("/bin/sh", "sh", "-i", static_cast<char*>(nullptr), environment);
        _exit(127);
    }
    PAL::UnixFileDescriptor ownMaster(master);
    REQUIRE(owner.adoptForkResult(child) && ownMaster);
    const auto waitFor = [](auto&& condition) {
        for (int attempt = 0; attempt < 2000 && !condition(); ++attempt)
            ::usleep(5000);
        return condition();
    };
    const auto foregroundGroup = [&] { return ::tcgetpgrp(ownMaster.get()); };
    REQUIRE(waitFor([&] { return foregroundGroup() == child; }));

    const PAL::ProcessID self = static_cast<PAL::ProcessID>(::getpid());
    auto adopted = PAL::adoptPTY(::dup(ownMaster.get()), self);
    REQUIRE(adopted != nullptr);
    // The transactional adoption a share makes holds the master the same way.
    auto prepared = PAL::preparePTYAdoption(::dup(ownMaster.get()), self);
    REQUIRE(prepared != nullptr);
    CHECK_MSG(adopted->process().foregroundIsShell(), "an adopted master read its shell at the prompt as a job");
    CHECK_MSG(prepared->process().foregroundIsShell(), "a prepared adoption read its shell at the prompt as a job");

    REQUIRE(::write(ownMaster.get(), "sleep 30\n", 9) == 9);
    // The job holds the terminal once the program the shell forked runs; the
    // fork before its exec is still the shell's own work.
    REQUIRE(waitFor([&] { return adopted->process().foregroundName() == "sleep"; }));
    CHECK(!adopted->process().foregroundIsShell());
    CHECK(!prepared->process().foregroundIsShell());

    REQUIRE(::write(ownMaster.get(), "\x03", 1) == 1);
    REQUIRE(waitFor([&] { return foregroundGroup() == child; }));
    CHECK(adopted->process().foregroundIsShell());
    CHECK(prepared->process().foregroundIsShell());

    // A shell's exit waits to drain output a master still holds, so every
    // master closes before the shell is reaped.
    adopted.reset();
    prepared.reset();
    ownMaster.reset();
    CHECK(owner.stop());
}

// An interactive zsh whose prompt runs hook-shaped subshells, under a ZDOTDIR
// this test owns, so no rc file of the machine's user is read.
//
// The hooks are the shapes prompt plugins use. `( function & )` is a forked
// subshell that forks again and exits; `( /bin/sleep … & )` is the same with
// the inner child exec'ing a program; `$( … )` in PROMPT runs in the shell's
// own group. Each subshell holds the terminal's foreground group for as long
// as it lives, which is the instant after every command and every Ctrl-C.
class HookedShell {
public:
    enum class ForegroundHook {
        Transient,
        Observable
    };

    explicit HookedShell(ForegroundHook foregroundHook = ForegroundHook::Transient)
    {
        char directory[] = "/tmp/ubq-hooked-zsh-XXXXXX";
        if (!::mkdtemp(directory))
            return;
        m_directory = directory;
        m_rcPath = m_directory + "/.zshrc";
        std::FILE* rc = std::fopen(m_rcPath.c_str(), "w");
        if (!rc)
            return;
        std::fputs("unset HISTFILE\n"
                   "setopt promptsubst\n"
                   "PROMPT='$(/bin/echo -n p)%# '\n"
                   "hookFunction() { : }\n"
                   "hookSubshellRunningShellCode() { ( hookFunction & ) }\n"
                   "hookSubshellRunningAProgram() { ( /bin/sleep 0.001 & ) >/dev/null 2>&1 }\n"
                   "hookForegroundSubshell() { ( : ) }\n",
            rc);
        if (foregroundHook == ForegroundHook::Observable) {
            // Keep shell code, not an exec'ed sleep, in the foreground long
            // enough to observe the group across the process-facts query.
            // Short-lived hooks can exit during the Linux /proc walk.
            std::fputs("zmodload zsh/zselect\n"
                       "hookForegroundSubshell() { ( zselect -t 10 ) }\n",
                rc);
        }
        std::fputs("precmd_functions=(\n"
                   "  hookSubshellRunningShellCode hookSubshellRunningAProgram hookForegroundSubshell\n"
                   "  hookSubshellRunningShellCode hookSubshellRunningAProgram hookForegroundSubshell\n"
                   "  hookSubshellRunningShellCode hookSubshellRunningAProgram hookForegroundSubshell\n"
                   "  hookSubshellRunningShellCode hookSubshellRunningAProgram hookForegroundSubshell)\n"
                   "echo hooked-ready:$$:\n",
            rc);
        std::fclose(rc);
        const std::vector<std::string> environment { "ZDOTDIR=" + m_directory };
        const std::vector<std::string> arguments { "/bin/zsh", "-d", "-i" };
        PAL::PTYLaunchOptions options;
        options.environmentOverrides = environment;
        m_stream = PAL::openPTY(arguments, options);
        if (!m_stream)
            return;
        // An interactive shell leads its own group, so its process ID is the
        // group that means "at the prompt". Read from the shell rather than
        // sampled, because a sample can land inside a hook.
        constexpr std::string_view marker = "hooked-ready:";
        const auto announcedProcessID = [&]() -> PAL::ProcessID {
            const std::size_t found = m_output.find(marker);
            if (found == std::string::npos || m_output.find(':', found + marker.size()) == std::string::npos)
                return 0;
            return static_cast<PAL::ProcessID>(std::strtol(m_output.c_str() + found + marker.size(), nullptr, 10));
        };
        drainUntil([&] { return announcedProcessID() > 0; }, 5000);
        m_shellGroup = announcedProcessID();
        m_isReady = m_shellGroup > 0 && drainUntil([&] { return m_stream->process().foregroundProcessGroup() == m_shellGroup; }, 5000);
    }

    ~HookedShell()
    {
        m_stream.reset();
        if (!m_rcPath.empty())
            ::unlink(m_rcPath.c_str());
        if (!m_directory.empty())
            ::rmdir(m_directory.c_str());
    }

    HookedShell(const HookedShell&) = delete;
    HookedShell& operator=(const HookedShell&) = delete;

    [[nodiscard]] bool isReady() const { return m_isReady; }
    [[nodiscard]] PAL::Stream& stream() { return *m_stream; }
    [[nodiscard]] PAL::ProcessID shellGroup() const { return m_shellGroup; }

    void type(std::string_view bytes) { m_stream->writeAll(bytes); }

    /// Everything the shell wrote so far, but for what was trimmed to bound it.
    [[nodiscard]] const std::string& output() const { return m_output; }

    // Reads what is pending without waiting, so the reader never blocks on the
    // shell and the shell never blocks on a full master.
    void drain()
    {
        char buffer[4096];
        for (;;) {
            const int count = m_stream->read({ buffer, sizeof(buffer) });
            if (count <= 0)
                return;
            m_output.append(buffer, static_cast<std::size_t>(count));
            if (m_output.size() > 65536)
                m_output.erase(0, m_output.size() - 4096);
        }
    }

    template<typename Predicate>
    bool drainUntil(Predicate&& predicate, int budgetMilliseconds)
    {
        for (int elapsed = 0; elapsed < budgetMilliseconds; ++elapsed) {
            drain();
            if (predicate())
                return true;
            ::usleep(1000);
        }
        return false;
    }

    // Ends the job with `ending` and waits for the shell to hold the terminal.
    bool returnToPrompt(std::string_view ending = "\x03")
    {
        type(ending);
        return drainUntil([&] { return m_stream->process().foregroundProcessGroup() == m_shellGroup; }, 5000);
    }

    // Whether a close now would ask, by the rule every close and the marker use.
    [[nodiscard]] bool wouldCloseAsk()
    {
        Session::ProcessFacts facts { .streamPresent = true, .hasRunningChild = m_stream->process().hasRunningChild(), .foregroundIsShell = m_stream->process().foregroundIsShell() };
        return Session::needsCloseConfirmation(facts);
    }

private:
    std::string m_directory;
    std::string m_rcPath;
    std::unique_ptr<PAL::Stream> m_stream;
    std::string m_output;
    PAL::ProcessID m_shellGroup = 0;
    bool m_isReady = false;
};

TEST(testPromptHooksDoNotAskToCloseTheShell)
{
    REQUIRE_MSG(::access("/bin/zsh", X_OK) == 0, "premise: zsh is installed; Scripts/run-linux.sh's recipe installs it");
    HookedShell shell(HookedShell::ForegroundHook::Observable);
    CHECK(shell.isReady());
    if (!shell.isReady())
        return;

    // Every Return runs the hooks again. Sampled as fast as the reads go, the
    // samples land inside the hooks' subshells as well as between them, and
    // the count of those that did keeps the run from passing on a shell whose
    // hooks were never seen. A hook is the shell's work, and is named by it.
    int samplesInsideAHook = 0;
    int samplesAsking = 0;
    int samplesMisnamed = 0;
    const auto start = std::chrono::steady_clock::now();
    auto lastReturn = start;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReturn > std::chrono::milliseconds(15)) {
            shell.type("\r");
            lastReturn = now;
        }
        shell.drain();
        const std::optional<PAL::ProcessID> group = shell.stream().process().foregroundProcessGroup();
        const bool isAsking = shell.wouldCloseAsk();
        const std::optional<PAL::ProcessID> groupAfter = shell.stream().process().foregroundProcessGroup();
        if (group && *group != shell.shellGroup() && group == groupAfter)
            ++samplesInsideAHook;
        if (isAsking)
            ++samplesAsking;
        if (shell.stream().process().foregroundName() != "zsh")
            ++samplesMisnamed;
    }
    std::printf("  prompt hooks: %d samples inside a hook, %d asked to close, %d misnamed\n", samplesInsideAHook, samplesAsking, samplesMisnamed);
    CHECK(samplesInsideAHook > 0);
    CHECK(samplesAsking == 0);
    CHECK(samplesMisnamed == 0);
}

TEST(testRunningJobsStillAskToClose)
{
    REQUIRE_MSG(::access("/bin/zsh", X_OK) == 0, "premise: zsh is installed; Scripts/run-linux.sh's recipe installs it");
    HookedShell shell;
    CHECK(shell.isReady());
    if (!shell.isReady())
        return;

    // The close must ask once each job has started, and keep asking for as
    // long as it runs, and the job is named by a program that makes it one.
    struct Job {
        std::string_view command;
        // The names it may read: the first member listed that is a job names
        // a group whose leader is not one.
        std::array<std::string_view, 2> names;
        std::string_view ending = "\x03";
        // Output that says every member of the job has started. Until then
        // the group can be its leader alone, a fork running shell code,
        // which is what a prompt hook's subshell is too.
        std::string_view started = {};
    };
    const Job jobs[] = {
        { "/bin/sleep 30\r", { "sleep" } },
        // A program that reads the terminal, standing in for an editor.
        { "/bin/cat\r", { "cat" } },
        // A nested shell is a program the user started, not the session's
        // shell. It ignores Ctrl-C at its prompt, as the session's does.
        { "/bin/zsh -f\r", { "zsh" }, "exit\r" },
        { "/bin/sleep 30 | /bin/cat\r", { "sleep" } },
        // The group's leader is a fork of the shell; its other member exec'd.
        // The leader can run `echo` to its end before the shell has forked
        // `cat`, so the job has started once `cat` printed what `echo` wrote;
        // the quotes keep the typed line from saying it first.
        { "{ /bin/echo pi''ped; /bin/sleep 30 } | /bin/cat\r", { "sleep", "cat" }, "\x03", "piped" },
        { "for i in 1 2; do /bin/sleep 30; done | /bin/cat\r", { "sleep", "cat" } },
        // A subshell whose child exec'd in the foreground.
        { "( /bin/sleep 30; true )\r", { "sleep" } },
    };
    for (const Job& job : jobs) {
        const std::string name { job.command };
        shell.type(job.command);
        const bool didStart = job.started.empty() || shell.drainUntil([&] { return shell.output().contains(job.started); }, 5000);
        CHECK_MSG(didStart, name.c_str());
        const bool didAsk = shell.drainUntil([&] { return shell.wouldCloseAsk(); }, 5000);
        CHECK_MSG(didAsk, name.c_str());
        int samplesNotAsking = 0;
        int samplesMisnamed = 0;
        std::string misnamed;
        for (int sample = 0; sample < 200; ++sample) {
            shell.drain();
            if (!shell.wouldCloseAsk())
                ++samplesNotAsking;
            const std::string foregroundName = shell.stream().process().foregroundName();
            if (foregroundName.empty() || std::ranges::find(job.names, foregroundName) == job.names.end()) {
                ++samplesMisnamed;
                misnamed = foregroundName;
            }
        }
        CHECK_MSG(samplesNotAsking == 0, name.c_str());
        CHECK_MSG(samplesMisnamed == 0, (name + " read the name \"" + misnamed + "\"").c_str());
        CHECK_MSG(shell.returnToPrompt(job.ending), name.c_str());
    }

    // Backgrounded, then brought back.
    shell.type("/bin/sleep 30 &\r");
    CHECK(shell.drainUntil([&] { return !shell.wouldCloseAsk(); }, 2000));
    shell.type("fg\r");
    CHECK(shell.drainUntil([&] { return shell.wouldCloseAsk(); }, 5000));
    int samplesNotAsking = 0;
    for (int sample = 0; sample < 200; ++sample) {
        if (!shell.wouldCloseAsk())
            ++samplesNotAsking;
    }
    CHECK(samplesNotAsking == 0);
    CHECK(shell.returnToPrompt());
}

TEST(testDuplicateAsPollableStreamReadsTheSameMaster)
{
    auto s = openArguments({ "/bin/cat" });
    CHECK(s != nullptr);
    if (!s)
        return;

    std::unique_ptr<PAL::PollableByteStream> duplicate = s->duplicateAsPollableStream();
    CHECK(duplicate != nullptr);
    if (!duplicate)
        return;
    CHECK(duplicate->readinessWaitStrategy() == PAL::ReadinessWaitStrategy::ProbeBeforeWait);

    // The duplicate shares the master's open file description, so bytes echoed
    // back arrive through it while the original stays open for teardown.
    s->writeAll({ "ping\n", 5 });
    char buffer[256];
    int got = 0;
    for (int ticks = 0; ticks < 400 && got < 4; ++ticks) {
        const int n = duplicate->read({ buffer + got, sizeof(buffer) - 1 - static_cast<size_t>(got) });
        if (n > 0)
            got += n;
        else
            ::usleep(5000);
    }
    buffer[got] = '\0';
    CHECK(std::strstr(buffer, "ping") != nullptr);
}

// Closing a pane must not wait on the child that was in it. The destructor runs
// on the main thread, so every millisecond it spends reaping is a millisecond
// the window is frozen — and the child that takes longest is exactly the one
// that cannot be hurried: a process ignoring SIGHUP, or wedged in a syscall
// that SIGKILL cannot interrupt until it returns.
//
// The child here traps the hangup and sleeps through it, so the reap can only
// finish by escalating. The destructor must hand that off and return.
TEST(testCloseDoesNotWaitOnAStubbornChild)
{
    auto stream = openArguments({ "/bin/sh", "-c", "trap '' HUP; sleep 30" });
    CHECK(stream != nullptr);
    if (!stream)
        return;

    // Let the shell install the trap before the hangup arrives; otherwise the
    // child dies on the default action and the case measures nothing.
    char buffer[64];
    int finalStatus = 0;
    (void)drainWithBudget(stream.get(), buffer, sizeof(buffer) - 1, 300, &finalStatus);

    struct timespec before {};
    struct timespec after {};
    ::clock_gettime(CLOCK_MONOTONIC, &before);
    stream.reset();
    ::clock_gettime(CLOCK_MONOTONIC, &after);

    const double elapsedMilliseconds = (static_cast<double>(after.tv_sec - before.tv_sec) * 1000.0)
        + (static_cast<double>(after.tv_nsec - before.tv_nsec) / 1000000.0);
    // The bound is generous against a loaded machine and still an order of
    // magnitude under the quarter second the inline reap could take.
    CHECK(elapsedMilliseconds < 25.0);
    if (elapsedMilliseconds >= 25.0)
        std::printf("close took %.1f ms\n", elapsedMilliseconds);
}

#if defined(__linux__)
// The process walk reads each process's owner off the /proc/<pid> DIRECTORY.
// Reading it off the /proc/<pid>/stat descriptor would save a syscall but is
// wrong: the kernel gives the FILES under a non-dumpable process's directory to root
// while the directory keeps the real owner, so every hardened service and
// every setuid binary read as uid 0. Measured on an ordinary Fedora desktop:
//
//     pid 962: stat uid 0, directory uid 81     (dbus)
//     pid 973: stat uid 0, directory uid 70     (avahi)
//     pid 1471: stat uid 0, directory uid 1000  (a plain user process)
//
// So the case asserts what the walk actually depends on. Two things, and
// the second is what makes it more than a tautology: our own process's
// directory carries our uid, and where the two disagree the stat file's
// owner is root — the kernel behaviour this relies on. A kernel that
// disagreed some other way would be a different situation and should say so
// rather than pass quietly.
TEST(testProcDirectoryCarriesTheProcessOwner)
{
    DIR* proc = ::opendir("/proc");
    CHECK(proc != nullptr);
    if (!proc)
        return;
    int compared = 0;
    int disagreed = 0;
    while (const dirent* entry = ::readdir(proc)) {
        char* end = nullptr;
        const long processID = std::strtol(entry->d_name, &end, 10);
        if (processID <= 0 || (end && *end != '\0'))
            continue;
        char directoryPath[64];
        std::snprintf(directoryPath, sizeof(directoryPath), "/proc/%ld", processID);
        // Anchor both stats to one open /proc/<pid> directory rather than
        // resolving the path a second time. An fstat on a descriptor pins one
        // process, but a fresh stat() of the path a line later lands on a
        // different process when the pid recycled in between -- ordinary under a
        // parallel run with hundreds of short-lived processes, and it reports a
        // disagreement the kernel never had. The directory fd is the anchor;
        // "stat" opened relative to it belongs to the same process or not at all.
        const int directoryDescriptor = ::open(directoryPath, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directoryDescriptor < 0)
            continue; // exited between the readdir and the open, which is ordinary
        struct stat viaDirectory {};
        struct stat viaStatFile {};
        if (::fstat(directoryDescriptor, &viaDirectory) == 0) {
            const int statDescriptor = ::openat(directoryDescriptor, "stat", O_RDONLY | O_CLOEXEC);
            if (statDescriptor >= 0) {
                if (::fstat(statDescriptor, &viaStatFile) == 0) {
                    ++compared;
                    if (viaStatFile.st_uid != viaDirectory.st_uid) {
                        ++disagreed;
                        // Root, or the kernel is doing something this walk has
                        // not accounted for. Not a printf: a disagreement that
                        // is not the documented one has to fail.
                        CHECK(viaStatFile.st_uid == 0);
                    }
                    if (processID == ::getpid())
                        CHECK(viaDirectory.st_uid == ::geteuid());
                }
                ::close(statDescriptor);
            }
        }
        ::close(directoryDescriptor);
    }
    ::closedir(proc);
    CHECK(compared > 0); // a walk that compared nothing has asserted nothing
    // `disagreed` is reported rather than asserted on: how many processes on
    // this machine are non-dumpable is the host's business, and a machine with
    // none is not evidence of anything. What must hold is the SHAPE of each
    // disagreement, which is checked above.
    std::printf("proc owner walk: compared %d, stat-file owner differed on %d\n", compared, disagreed);
}
#endif

// The subtree tracker's whole purpose is to say when the process tree moved,
// so a title can follow a program without anyone asking again. Two cases,
// because "I was handed a tracker" and "I will be told" are different facts,
// and every edge-driven feature depends on the second.

#if defined(__APPLE__)
TEST(testATrackerDeliversAnEdgeWhenItsSubtreeForks)
{
    // Shaped like the passing seed/NOTE_CHILD case above, but asserting the
    // NOTIFIER rather than the pid set: those are different facts, and every
    // edge-driven feature depends on the second one.
    TrackerSubtree subtree;
    REQUIRE(subtree.start());
    REQUIRE(subtree.receiveProcessID());
    const pid_t child = subtree.processID();
    const std::unique_ptr<PAL::ProcessTracker, decltype(&PAL::stopProcessTracker)> tracker(PAL::startProcessTracker(child), PAL::stopProcessTracker);
    REQUIRE(tracker != nullptr);
    REQUIRE(PAL::processTrackerDeliversEdges(tracker.get()));

    static std::atomic<int> edges { 0 };
    edges.store(0, std::memory_order_relaxed);
    PAL::processTrackerSetChangeNotifier(
        tracker.get(), [](void*) { edges.fetch_add(1, std::memory_order_relaxed); return true; }, nullptr, nullptr);
    REQUIRE(subtree.startLate());
    REQUIRE(subtree.receiveProcessID());

    bool fired = false;
    for (int attempt = 0; attempt < 600 && !fired; ++attempt) {
        fired = edges.load(std::memory_order_relaxed) > 0;
        ::usleep(5000);
    }
    CHECK_MSG(fired, "a fork inside the tracked subtree reaches the change notifier");
}
#endif

TEST(testATrackerThatCannotWatchItsRootSaysSoRatherThanGoingSilent)
{
    // A process that is definitively gone: forked, exited, and reaped, so the
    // pid names nothing the kernel will register interest in. The tracker must
    // report that it delivers nothing rather than hand back an object that
    // looks alive — a caller that cannot tell waits for an edge forever, which
    // is exactly how every edge-driven feature came to be silently dead.
    UbiquittyTest::OwnedChildProcess<> owner;
    REQUIRE(owner.canOwnChildren());
    const pid_t doomed = ::fork();
    if (doomed == 0)
        ::_exit(0);
    REQUIRE(owner.adoptForkResult(doomed));
    int status = 0;
    REQUIRE(owner.reap(0, &status) == doomed);

    PAL::ProcessTracker* tracker = PAL::startProcessTracker(doomed);
    CHECK_MSG(!PAL::processTrackerDeliversEdges(tracker),
        "a tracker whose root cannot be watched does not claim it delivers edges");
    PAL::stopProcessTracker(tracker);
}

// End to end, through the same path the product uses: a real PTY, a real
// shell, a real program started from that shell, and the chain the TITLE would
// show. The tracker cases above prove the mechanism in isolation; this proves
// the thing a reader actually sees, which can break while they pass.
TEST(testTheChainFollowsAProgramTheShellStarts)
{
    std::unique_ptr<PAL::Stream> stream = openArguments({ "/bin/sh" });
    CHECK(stream != nullptr);

    const auto chainNow = [&stream]() -> std::string {
        PAL::Process::Record records[64];
        const int found = stream->process().gatherProcesses({ records, 64 });
        if (found <= 0)
            return {};
        return PAL::Process::formatChain({ records, static_cast<std::size_t>(found) });
    };

    // The shell itself, once it has settled.
    bool shellNamed = false;
    for (int attempt = 0; attempt < 300 && !shellNamed; ++attempt) {
        shellNamed = chainNow().find("sh") != std::string::npos;
        ::usleep(10000);
    }
    CHECK_MSG(shellNamed, "the chain names the shell the session is running");

    // A program the shell starts. `sleep` holds the foreground without writing,
    // which is the case a drain cannot carry and the walk must answer.
    const char* command = "sleep 30\n";
    (void)stream->writeAll({ command, std::strlen(command) });

    std::string seen;
    bool followed = false;
    for (int attempt = 0; attempt < 500 && !followed; ++attempt) {
        seen = chainNow();
        followed = seen.find("sleep") != std::string::npos;
        ::usleep(10000);
    }
    CHECK_MSG(followed, ("the chain follows a program the shell started (saw \"" + seen + "\")").c_str());
}

#if defined(__APPLE__)
// A program's name is its argv[0], whole when that names the executable, so a
// program whose file name holds a space is named whole by the walk and by the
// chain spelled from it; a rewritten argv[0] is named by its first word.
TEST(testTheChainNamesAProgramWhoseNameHoldsASpaceWhole)
{
    char directoryTemplate[] = "/tmp/ubq-pty-spaced.XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    REQUIRE(directory);
    const std::string program = std::string(directory) + "/a program";
    REQUIRE(::symlink("/bin/sleep", program.c_str()) == 0);
    struct Named {
        std::string name;
        std::string chainName;
    };
    // What the walk names the child once it reads `expected`, and the name
    // the chain spells from that record, or the last ones it read: a forked
    // child reads as this test until it execs.
    const auto nameOf = [](const char* path, const char* argument0, std::string_view expected) {
        UbiquittyTest::OwnedChildProcess<> childOwner;
        const pid_t child = childOwner.canOwnChildren() ? ::fork() : -1;
        if (child == 0) {
            ::execl(path, argument0, "40", nullptr);
            ::_exit(127);
        }
        Named named;
        if (child < 0 || !childOwner.adoptForkResult(child))
            return named;
        for (int attempt = 0; attempt < 300 && named.name != expected; ++attempt) {
            PAL::Process::Record records[8];
            const int count = PAL::gatherSubtree(static_cast<PAL::ProcessID>(child), 0, "", { records, 8 }, nullptr);
            for (int index = 0; index < count; ++index) {
                if (records[index].processID == static_cast<std::uint32_t>(child))
                    named = { .name = records[index].name, .chainName = std::string(PAL::Process::scriptDisplayName(records[index].name, records[index].commandLine)) };
            }
            if (named.name != expected)
                ::usleep(10000);
        }
        CHECK(childOwner.stop());
        return named;
    };
    const Named spaced = nameOf(program.c_str(), program.c_str(), "a program");
    CHECK_STR_EQ(spaced.name, "a program");
    CHECK_STR_EQ(spaced.chainName, "a program");
    const Named rewritten = nameOf("/bin/sleep", "sleeper: waiting", "sleeper:");
    CHECK_STR_EQ(rewritten.name, "sleeper:");
    CHECK_STR_EQ(rewritten.chainName, "sleeper:");
    (void)::unlink(program.c_str());
    (void)::rmdir(directory);
}
#endif

#if defined(__linux__)
// A program's name is its argv[0]'s basename, whole when argv[0] is a file
// name, so a path or a file name holding a space names the program whole; a
// rewritten argv[0] is named by its first word, whether or not the program also
// renamed comm to match it (Node's process.title, setproctitle) and whether or
// not a long file name leaves comm the head of the title.
TEST(testTheExecutableNameKeepsASpaceInTheProgramsOwnName)
{
    char directoryTemplate[] = "/tmp/ubq-pty-spaced.XXXXXX";
    const char* directory = ::mkdtemp(directoryTemplate);
    REQUIRE(directory);
    const std::string toolsDirectory = std::string(directory) + "/My Tools";
    REQUIRE(::mkdir(toolsDirectory.c_str(), 0700) == 0);
    const std::string inSpacedDirectory = toolsDirectory + "/napper";
    const std::string spacedFile = std::string(directory) + "/nap time";
    const std::string longSpacedFile = std::string(directory) + "/a program whose name is long";
    const std::string longNamedFile = std::string(directory) + "/kube-controller-manager";
    for (const std::string& link : { inSpacedDirectory, spacedFile, longSpacedFile, longNamedFile })
        REQUIRE_MSG(::symlink(executablePath.c_str(), link.c_str()) == 0, "premise: the link is made");

    // What the executable name reads once the child runs `path` as `argument0`;
    // a forked child reads as this test until it execs.
    // `commName` is the name the program gives its comm once it runs, none
    // when it leaves comm alone.
    const auto nameOf = [](const std::string& path, const char* argument0, std::string_view expected, const char* commName = nullptr) {
        UbiquittyTest::OwnedChildProcess<> childOwner;
        const pid_t child = childOwner.canOwnChildren() ? ::fork() : -1;
        if (child == 0) {
            if (commName)
                ::execl(path.c_str(), argument0, renamesItselfMarker, commName, "40", nullptr);
            else
                ::execl(path.c_str(), argument0, sleepsMarker, "40", nullptr);
            ::_exit(127);
        }
        std::string name;
        if (child < 0 || !childOwner.adoptForkResult(child))
            return name;
        // The name read before the program renames comm is the unrenamed
        // answer, which can agree with the expected one by chance.
        const std::string renamedComm = commName ? std::string(commName).substr(0, 15) : std::string {};
        for (int attempt = 0; attempt < 300 && !renamedComm.empty() && PAL::processCommandName(static_cast<PAL::ProcessID>(child)) != renamedComm; ++attempt)
            ::usleep(10000);
        for (int attempt = 0; attempt < 300 && name != expected; ++attempt) {
            name = PAL::processExecutableName(static_cast<PAL::ProcessID>(child));
            if (name != expected)
                ::usleep(10000);
        }
        CHECK(childOwner.stop());
        return name;
    };
    CHECK_STR_EQ(nameOf(inSpacedDirectory, inSpacedDirectory.c_str(), "napper"), "napper");
    CHECK_STR_EQ(nameOf(spacedFile, spacedFile.c_str(), "nap time"), "nap time");
    CHECK_STR_EQ(nameOf(longSpacedFile, longSpacedFile.c_str(), "a program whose name is long"), "a program whose name is long");
    CHECK_STR_EQ(nameOf(executablePath, "sleeper: waiting", "sleeper:"), "sleeper:");
    CHECK_STR_EQ(nameOf(executablePath, "sleeper: user@pts/0", "sleeper:"), "sleeper:");
    // A title whose comm the program renamed to match, and one a long file name
    // leaves comm the head of, are titles all the same.
    CHECK_STR_EQ(nameOf(executablePath, "npm run dev", "npm", "npm run dev"), "npm");
    CHECK_STR_EQ(nameOf(executablePath, "gunicorn: master [app]", "gunicorn:", "gunicorn: master [app]"), "gunicorn:");
    CHECK_STR_EQ(nameOf(longNamedFile, "kube-controller-manager: leader election", "kube-controller-manager:"), "kube-controller-manager:");
    (void)::unlink(longNamedFile.c_str());
    (void)::unlink(inSpacedDirectory.c_str());
    (void)::unlink(spacedFile.c_str());
    (void)::unlink(longSpacedFile.c_str());
    (void)::rmdir(toolsDirectory.c_str());
    (void)::rmdir(directory);
}
#endif

TEST(testProcessArgumentsRefreshWithoutTrackerNotifications)
{
    UbiquittyTest::OwnedChildProcess<> childOwner;
    REQUIRE(childOwner.canOwnChildren());
    int commandPipe[2];
    const int pipeResult = ::pipe(commandPipe);
    CHECK(pipeResult == 0);
    if (pipeResult != 0) {
        return;
    }
    PAL::UnixFileDescriptor commandRead(commandPipe[0]);
    PAL::UnixFileDescriptor commandWrite(commandPipe[1]);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child < 0)
        return;
    if (child == 0) {
        commandWrite.reset();
        if (::dup2(commandRead.get(), STDIN_FILENO) < 0)
            ::_exit(126);
        commandRead.reset();
        ::execl("/bin/sh", "sh", "-c", "read value; exec /bin/sleep 40", nullptr);
        ::_exit(127);
    }
    REQUIRE(childOwner.adoptForkResult(child));
    commandRead.reset();
    const auto namesChild = [child](const char* name) {
        PAL::Process::Record records[8];
        const int count = PAL::gatherSubtree(static_cast<PAL::ProcessID>(child), 0, "", { records, 8 }, nullptr);
        for (int index = 0; index < count; ++index) {
            if (records[index].processID == static_cast<std::uint32_t>(child) && records[index].name == name) {
                return true;
            }
        }
        return false;
    };
    bool sawShell = false;
    for (int attempt = 0; attempt < 300 && !sawShell; ++attempt) {
        sawShell = namesChild("sh");
        ::usleep(10000);
    }
    CHECK(sawShell);
    CHECK(::write(commandWrite.get(), "go\n", 3) == 3);
    commandWrite.reset();
    bool sawSleep = false;
    for (int attempt = 0; attempt < 300 && !sawSleep; ++attempt) {
        sawSleep = namesChild("sleep");
        ::usleep(10000);
    }
    CHECK(sawSleep);
    CHECK(childOwner.stop());
}

TEST(testTheChainNamesAChildThatIsNotInTheForegroundGroup)
{
    std::unique_ptr<PAL::Stream> stream = openArguments({ "/bin/sh" });
    CHECK(stream != nullptr);

    const auto chainNow = [&stream]() -> std::string {
        PAL::Process::Record records[64];
        const int found = stream->process().gatherProcesses({ records, 64 });
        if (found <= 0)
            return {};
        return PAL::Process::formatChain({ records, static_cast<std::size_t>(found) });
    };

    bool ready = false;
    for (int attempt = 0; attempt < 300 && !ready; ++attempt) {
        ready = !chainNow().empty();
        ::usleep(10000);
    }
    CHECK(ready);

    // `cat` holds the terminal; the backgrounded `sleep` is its child and is
    // not what the tty's foreground group names.
    // Nested the way a real session nests: the shell stays, and the program
    // it starts holds the terminal with a child of its own. An `exec` at the
    // top level would make that program the session leader, which a real
    // session never does, because login owns that role.
    const char* command = "/bin/sh -c 'sleep 40 & exec cat'\n";
    (void)stream->writeAll({ command, std::strlen(command) });

    std::string seen;
    bool named = false;
    for (int attempt = 0; attempt < 500 && !named; ++attempt) {
        seen = chainNow();
        named = seen.find("sleep") != std::string::npos;
        ::usleep(10000);
    }
    if (!named) {
        PAL::Process::Record records[64];
        const int found = stream->process().gatherProcesses({ records, 64 });
        std::printf("    [walk] %d records\n", found);
        for (int i = 0; i < found; ++i) {
            std::printf("      pid=%u ppid=%u pgid=%u name=%-12s fg=%d leader=%d tty=%s\n",
                records[i].processID, records[i].parentProcessID, records[i].processGroupID,
                records[i].name.c_str(), records[i].isForeground ? 1 : 0, records[i].isSessionLeader ? 1 : 0,
                records[i].tty.c_str());
        }
    }
    CHECK_MSG(named, ("the chain names a child outside the foreground group (saw \"" + seen + "\")").c_str());
}

TEST(ptyEnvironmentOverridesAreValidatedBeforeLaunch)
{
    const std::vector<std::string> command { "/bin/sh", "-c", R"(printf '%s|%s' "$UBIQUITTY_TEST_OVERRIDE" "$PATH")" };
    const std::vector<std::string> replacements { "UBIQUITTY_TEST_OVERRIDE=owned", "PATH=/usr/bin:/bin" };
    PAL::PTYLaunchOptions options;
    options.argument0Style = PAL::Argument0Style::ProgramPath;
    options.environmentOverrides = replacements;
    auto stream = PAL::openPTY(command, options);
    CHECK(stream != nullptr);
    if (stream) {
        char output[512];
        const int count = drainWithBudget(stream.get(), output, sizeof(output), 1000, nullptr);
        CHECK(std::string_view(output, static_cast<std::size_t>(count)).find("owned|/usr/bin:/bin") != std::string_view::npos);
    }
    for (const std::vector<std::string>& invalid : {
             std::vector<std::string> { "BAD KEY=value" },
             std::vector<std::string> { "=value" },
             std::vector<std::string> { "A=one", "A=two" },
             std::vector<std::string> { std::string("A=value\0hidden", 14) } }) {
        options.environmentOverrides = invalid;
        CHECK(!PAL::openPTY(command, options));
    }
}

TEST(daemonTerminalDoesNotInheritCommandRunnerColorControls)
{
    ScopedEnvironmentVariable savedNoColor("NO_COLOR");
    ScopedEnvironmentVariable savedColor("CLICOLOR");
    ScopedEnvironmentVariable savedForcedColor("FORCE_COLOR");
    ScopedEnvironmentVariable savedForcedCLIColor("CLICOLOR_FORCE");
    ::setenv("NO_COLOR", "1", 1);
    ::setenv("CLICOLOR", "0", 1);
    ::setenv("FORCE_COLOR", "0", 1);
    ::setenv("CLICOLOR_FORCE", "0", 1);
    const std::vector<std::string> command { "/bin/sh", "-c", R"(printf '%s|%s|%s|%s|%s' "${NO_COLOR-unset}" "${CLICOLOR-unset}" "${FORCE_COLOR-unset}" "${CLICOLOR_FORCE-unset}" "$COLORTERM")" };
    PAL::PTYLaunchOptions options;
    options.argument0Style = PAL::Argument0Style::ProgramPath;
    const auto output = [&] {
        auto stream = PAL::openPTY(command, options);
        CHECK(stream != nullptr);
        if (!stream)
            return std::string {};
        char bytes[512];
        const int count = drainWithBudget(stream.get(), bytes, sizeof(bytes), 1000, nullptr);
        return std::string(bytes, static_cast<std::size_t>(count));
    };
    CHECK(output().find("1|0|0|0|truecolor") != std::string::npos);
    options.colorEnvironmentPolicy = PAL::ColorEnvironmentPolicy::Terminal;
    CHECK(output().find("unset|unset|unset|unset|truecolor") != std::string::npos);
    const std::vector<std::string> explicitChoice { "NO_COLOR=1" };
    options.environmentOverrides = explicitChoice;
    CHECK(output().find("1|unset|unset|unset|truecolor") != std::string::npos);
}

namespace {

/// The environment `/usr/bin/env` prints in a child launched with `options`,
/// one entry per line, each line preceded by a newline.
std::string childEnvironmentListing(PAL::PTYLaunchOptions options)
{
    const std::vector<std::string> command { "/usr/bin/env" };
    options.argument0Style = PAL::Argument0Style::ProgramPath;
    auto stream = PAL::openPTY(command, options);
    CHECK(stream != nullptr);
    if (!stream)
        return {};
    std::vector<char> bytes(65536);
    const int count = drainWithBudget(stream.get(), bytes.data(), static_cast<int>(bytes.size()), 2000, nullptr);
    std::string listing = "\n";
    for (int index = 0; index < count; ++index) {
        if (bytes[static_cast<std::size_t>(index)] != '\r')
            listing.push_back(bytes[static_cast<std::size_t>(index)]);
    }
    return listing;
}

} // namespace

// This process was started inside tmux, from kitty: its pane children run in
// a terminal that is neither, so they see none of what describes those two,
// and keep the person's ssh session and everything else.
TEST(aChildDoesNotInheritTheTerminalThisProcessWasStartedIn)
{
    ScopedEnvironmentVariable savedTmux("TMUX");
    ScopedEnvironmentVariable savedTmuxPane("TMUX_PANE");
    ScopedEnvironmentVariable savedTmuxDirectory("TMUX_TMPDIR");
    ScopedEnvironmentVariable savedKittyWindow("KITTY_WINDOW_ID");
    ScopedEnvironmentVariable savedTerminfo("TERMINFO");
    ScopedEnvironmentVariable savedSSHConnection("SSH_CONNECTION");
    ScopedEnvironmentVariable savedKept("UBIQUITTY_TEST_KEPT");
    ::setenv("TMUX", "/private/tmp/tmux-501/default,1234,0", 1);
    ::setenv("TMUX_PANE", "%3", 1);
    ::setenv("TMUX_TMPDIR", "/tmp", 1);
    ::setenv("KITTY_WINDOW_ID", "7", 1);
    ::setenv("TERMINFO", "/Applications/kitty.app/Contents/Resources/kitty/terminfo", 1);
    ::setenv("SSH_CONNECTION", "192.0.2.1 51000 198.51.100.2 22", 1);
    ::setenv("UBIQUITTY_TEST_KEPT", "kept", 1);

    const std::string listing = childEnvironmentListing({});
    CHECK_MSG(listing.find("\nTERM=") != std::string::npos, "the child's environment was read");
    CHECK(listing.find("\nTMUX=") == std::string::npos);
    CHECK(listing.find("\nTMUX_PANE=") == std::string::npos);
    CHECK(listing.find("\nKITTY_WINDOW_ID=") == std::string::npos);
    CHECK(listing.find("\nTERMINFO=") == std::string::npos);
    CHECK(listing.find("\nTMUX_TMPDIR=/tmp\n") != std::string::npos);
    CHECK(listing.find("\nSSH_CONNECTION=192.0.2.1 51000 198.51.100.2 22\n") != std::string::npos);
    CHECK(listing.find("\nUBIQUITTY_TEST_KEPT=kept\n") != std::string::npos);
    CHECK(listing.find("\nCOLORTERM=truecolor\n") != std::string::npos);
}

// A launcher that names one of those variables itself still gives it to the
// child: the scrub drops what is inherited, never what is configured.
TEST(anExplicitEnclosingTerminalVariableReachesTheChild)
{
    ScopedEnvironmentVariable savedTmux("TMUX");
    ScopedEnvironmentVariable savedTmuxPane("TMUX_PANE");
    ::setenv("TMUX", "/private/tmp/tmux-501/default,1234,0", 1);
    ::setenv("TMUX_PANE", "%3", 1);

    const std::vector<std::string> overrides { "TMUX=/private/tmp/tmux-501/chosen,1,0", "TERMINFO=/opt/terminfo" };
    PAL::PTYLaunchOptions options;
    options.environmentOverrides = overrides;
    const std::string listing = childEnvironmentListing(options);
    CHECK(listing.find("\nTMUX=/private/tmp/tmux-501/chosen,1,0\n") != std::string::npos);
    CHECK(listing.find("\nTMUX=/private/tmp/tmux-501/default") == std::string::npos);
    CHECK(listing.find("\nTERMINFO=/opt/terminfo\n") != std::string::npos);
    CHECK(listing.find("\nTMUX_PANE=") == std::string::npos);
}

// The line discipline the master reads says whether a signal character
// signals and whether taking one discards the unread input, as the program
// last set them: what the daemon's input sink and the echo context go by.
TEST(passwordDetectionReadsBashEchoSettingsFromTheMaster)
{
    struct Prompt {
        const char* command;
        bool isEchoEnabled;
    };
    for (const Prompt& prompt : {
             Prompt { "printf 'That password is the one in /Users/user/keychain-password. Put it on the clipboard until the export ends? (y/n) [y]:'; IFS= read -r answer", true },
             Prompt { "IFS= read -rs -p 'Password:' answer", false },
             Prompt { "stty -echo; printf 'PIN> '; IFS= read -r answer", false },
         }) {
        auto stream = openArguments({ "/bin/bash", "--noprofile", "--norc", "-c",
            std::string("stty icanon echo; ") + prompt.command + "; stty echo; printf '\\nDONE\\n'; IFS= read -r done" });
        REQUIRE(stream);
        Terminal terminal;
        resize(terminal, 4, 200);
        terminal.passwordDetect = true;
        std::string output;
        std::optional<PAL::LineDiscipline> discipline;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            char buffer[512];
            const int count = stream->read(buffer);
            if (count > 0) {
                output.append(buffer, static_cast<std::size_t>(count));
                processBytes(&terminal, { buffer, static_cast<std::size_t>(count) }, &stream->process());
            } else if (count != status(PAL::StreamStatus::Again))
                break;
            discipline = stream->process().lineDiscipline();
            if (!output.empty() && discipline && discipline->isEchoEnabled == prompt.isEchoEnabled)
                break;
            ::usleep(1000);
        }
        REQUIRE(!output.empty());
        REQUIRE(discipline);
        CHECK(discipline->isCanonical);
        CHECK_EQ(discipline->isEchoEnabled, prompt.isEchoEnabled);
        processBytes(&terminal, "", &stream->process());
        CHECK_EQ(terminal.passwordPromptActive, !prompt.isEchoEnabled);
        REQUIRE(stream->writeAll(std::span("yes\n", 4)) == 0);
        REQUIRE(drainUntilContains(stream.get(), "DONE"));
        discipline = stream->process().lineDiscipline();
        REQUIRE(discipline);
        CHECK(discipline->isCanonical);
        CHECK(discipline->isEchoEnabled);
        processBytes(&terminal, "", &stream->process());
        CHECK(!terminal.passwordPromptActive);
    }
}

TEST(theLineDisciplineReadsWhetherSignalsAreOnAndDiscardInput)
{
    struct Case {
        const char* modes;
        bool isSignalEnabled;
        bool isInputDiscardedOnSignal;
    };
    for (const Case& wanted : { Case { "isig -noflsh", true, true }, Case { "isig noflsh", true, false }, Case { "-isig -noflsh", false, true } }) {
        std::vector<std::string> arguments { "/bin/sh", "-c", std::string("stty ") + wanted.modes + "; sleep 5" };
        std::unique_ptr<PAL::Stream> stream = PAL::openPTY(arguments);
        CHECK(stream != nullptr);
        if (!stream)
            return;
        std::optional<PAL::LineDiscipline> discipline;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            discipline = stream->process().lineDiscipline();
            if (discipline && discipline->isSignalEnabled == wanted.isSignalEnabled && discipline->isInputDiscardedOnSignal == wanted.isInputDiscardedOnSignal)
                break;
            ::usleep(5'000);
        }
        CHECK_MSG(discipline.has_value(), "the master read no line discipline");
        if (!discipline)
            continue;
        CHECK_MSG(discipline->isSignalEnabled == wanted.isSignalEnabled, (std::string("stty ") + wanted.modes + ": signals read as " + (discipline->isSignalEnabled ? "on" : "off")).c_str());
        CHECK_MSG(discipline->isInputDiscardedOnSignal == wanted.isInputDiscardedOnSignal, (std::string("stty ") + wanted.modes + ": input read as " + (discipline->isInputDiscardedOnSignal ? "discarded" : "kept") + " at a signal").c_str());
    }
}

// An escape sequence the interrupt cut off does not swallow what follows it.
//
// The program writes "flood " and a lone ESC, as a flood is caught mid-sequence
// when ^C discards the rest of its output, then answers the signal with a
// prompt line. The terminal's echo of the ^C arrives right after the ESC, and
// ESC ^ opens a privacy message that runs to a terminator nobody sends: the
// echo, the prompt and everything after it were consumed unseen, on the host
// and on every client mirroring it. Read through the stream and parsed, the
// screen shows all three.
TEST(aSequenceCutByAnInterruptDoesNotSwallowWhatFollows)
{
    std::vector<std::string> arguments { "/usr/bin/perl", "-e",
        "$| = 1; $SIG{INT} = sub { }; print \"flood \\e\"; sleep 10; print \"\\r\\n\\$ done\\r\\n\"; sleep 1;" };
    std::unique_ptr<PAL::Stream> stream = PAL::openPTY(arguments);
    CHECK(stream != nullptr);
    if (!stream)
        return;
    Ubiquitty::Terminal terminal;
    Ubiquitty::resize(terminal, 6, 40);
    Ubiquitty::hardReset(terminal);
    std::string seen;
    char buffer[256];
    const auto readFor = [&](int milliseconds, const auto& isDone) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline && !isDone()) {
            const int n = stream->read({ buffer, sizeof(buffer) });
            if (n > 0) {
                seen.append(buffer, static_cast<std::size_t>(n));
                Ubiquitty::processBytes(&terminal, std::string_view { buffer, static_cast<std::size_t>(n) });
            } else if (n == status(PAL::StreamStatus::Again)) {
                ::usleep(1'000);
            } else {
                break;
            }
        }
    };
    readFor(3'000, [&] { return seen.find("flood \x1b") != std::string::npos; });
    CHECK_MSG(seen.ends_with("flood \x1b"), "premise: the program stopped in the middle of a sequence");
    (void)stream->writeAll({ "\x03", 1 });
    readFor(3'000, [&] { return seen.find("done") != std::string::npos; });

    const auto rowText = [&](int row) {
        std::string text;
        for (int column = 0; column < terminal.columns; ++column) {
            const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&terminal, 0, row, column);
            const char32_t code = cell ? cell->codePoint : U' ';
            text.push_back(code >= 0x20 && code < 0x7F ? static_cast<char>(code) : ' ');
        }
        while (!text.empty() && text.back() == ' ')
            text.pop_back();
        return text;
    };
    CHECK_MSG(seen.find("\x18^C") != std::string::npos, "the stream marks the cut ahead of the echo");
    CHECK_MSG(rowText(0) == "flood ^C", rowText(0).c_str());
    CHECK_MSG(rowText(1) == "$ done", rowText(1).c_str());
}

#if defined(__linux__)
constexpr bool isLinux = true;
#else
constexpr bool isLinux = false;
#endif

// What a program printed after an interrupt typed behind lines it never read.
//
// The program sleeps without reading, so lines typed ahead fill the terminal's
// input queue and the rest waits in the stream's. The kernel takes no
// character at all into a full queue that holds a whole line, the interrupt
// included, so written in turn the ^C waited for a read that only the ^C could
// bring about. The program answers the signal by reading one line and printing
// it in brackets.
//
// - Parameters:
//   - setup: Shell commands run before the sleep, for the terminal's modes.
//   - typedAheadBytes: How much is typed ahead of the interrupt.
//   - typedAfter: What is typed after the interrupt.
// - Returns: Everything read from the program, or "(no stream)".
static std::string outputAfterAnInterruptBehindUnreadLines(const std::string& setup, std::size_t typedAheadBytes, std::string_view typedAfter)
{
    std::vector<std::string> arguments { "/bin/sh", "-c", setup + "trap : INT; echo ready; sleep 20; read line; echo \"[$line]\"" };
    std::unique_ptr<PAL::Stream> stream = PAL::openPTY(arguments);
    if (!stream)
        return "(no stream)";
    std::string seen;
    char buffer[4096];
    const auto readFor = [&](int milliseconds, const auto& isDone) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline && !isDone()) {
            (void)stream->drainPendingWrite();
            const int n = stream->read({ buffer, sizeof(buffer) });
            if (n > 0)
                seen.append(buffer, static_cast<std::size_t>(n));
            else if (n == status(PAL::StreamStatus::Again))
                ::usleep(1'000);
            else
                break;
        }
    };
    readFor(3'000, [&] { return seen.find("ready") != std::string::npos; });
    ::usleep(200'000);
    std::string typedAhead;
    for (int line = 0; typedAhead.size() < typedAheadBytes; ++line)
        typedAhead += "ahead " + std::to_string(line) + "\r";
    CHECK(stream->writeAll({ typedAhead.data(), typedAhead.size() }) == 0);
    readFor(200, [] { return false; });
    // Linux takes a master's writes into a buffer ahead of the line
    // discipline, so a few kilobytes stall there without the stream seeing it.
    const bool isStallSeenByTheStream = !isLinux || typedAheadBytes > 1024 * 1024;
    CHECK_MSG(stream->hasPendingWrite() || !isStallSeenByTheStream, "premise: the terminal took no more of what was typed ahead");
    seen.clear();
    const std::string typed = "\x03" + std::string(typedAfter);
    CHECK(stream->writeAll({ typed.data(), typed.size() }) == 0);
    readFor(2'000, [&] { return seen.find(']') != std::string::npos; });
    return seen;
}

// An interrupt is not typed behind the input it discards. The line discipline
// throws away every unread character when it takes one, so what waits ahead of
// it is nothing the program will read; the stream discards it and the
// interrupt goes in at once.
TEST(anInterruptPassesInputTheProgramNeverRead)
{
    const std::string seen = outputAfterAnInterruptBehindUnreadLines("", 8 * 1024, "typed after\r");
    CHECK_MSG(seen.find("[typed after]") != std::string::npos, ("the program read: " + seen.substr(0, 200)).c_str());
    CHECK_MSG(seen.find("ahead") == std::string::npos, "what was typed ahead of the interrupt was discarded with it");
}

// With NOFLSH the program asked that an interrupt discard nothing: the signal
// still may not wait, and the lines typed ahead are still read, in order.
// Enough is typed ahead that the master refuses input on every platform.
TEST(anInterruptThatDiscardsNothingStillPassesAFullQueue)
{
    const std::string seen = outputAfterAnInterruptBehindUnreadLines("stty noflsh; ", 2 * 1024 * 1024, "");
    CHECK_MSG(seen.find("[ahead 0]") != std::string::npos, ("the program read: " + seen.substr(0, 200)).c_str());
}

// A ^C on a flooding terminal empties its output queue, and a read that finds
// the queue empty is not the end of the stream. The program below ignores the
// signal and keeps printing, so every ^C is a flush under a flood and nothing
// ends; the stream must go on reading for as long as the program lives. A read
// taken for the end here ends the pane: the daemon reaps the session and every
// client attached to it is closed with the program still running.
TEST(anInterruptThatFlushesTheOutputIsNotTheEndOfTheStream)
{
    std::vector<std::string> arguments { "/usr/bin/perl", "-e", "$SIG{INT} = 'IGNORE'; $| = 1; print 'x' x 100, \"\\n\" while 1;" };
    std::unique_ptr<PAL::Stream> stream = PAL::openPTY(arguments);
    CHECK(stream != nullptr);
    if (!stream)
        return;
    char buffer[16384];
    long long bytesRead = 0;
    int interruptsSent = 0;
    int ended = 0;
    const auto started = std::chrono::steady_clock::now();
    auto nextInterrupt = started + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(4) && !ended) {
        const int n = stream->read({ buffer, sizeof(buffer) });
        if (n > 0)
            bytesRead += n;
        else if (n != status(PAL::StreamStatus::Again))
            ended = n;
        if (std::chrono::steady_clock::now() >= nextInterrupt) {
            (void)stream->writeAll({ "\x03", 1 });
            ++interruptsSent;
            nextInterrupt += std::chrono::microseconds(500);
        }
    }
    CHECK_MSG(interruptsSent > 1000, "premise: the interrupts were sent");
    CHECK_MSG(stream->process().hasRunningChild(), "premise: the program ignores the interrupt and lives");
    CHECK_MSG(ended == 0, ("the stream reported its end (" + std::to_string(ended) + ") after " + std::to_string(interruptsSent) + " interrupts and " + std::to_string(bytesRead) + " bytes, with its program still running").c_str());
    const long long before = bytesRead;
    for (int pass = 0; pass < 200 && bytesRead == before; ++pass) {
        const int n = stream->read({ buffer, sizeof(buffer) });
        if (n > 0)
            bytesRead += n;
        else
            ::usleep(1'000);
    }
    CHECK_MSG(bytesRead > before, "the stream reads on after the interrupts");
}

TEST(ownedPTYCancellationKeepsKernelPrefixAndUnrelatedQueuedBytes)
{
    // The child owns a real terminal session but never reads. This test reads
    // its replica directly after cancellation, with no timing-based release.
    UbiquittyTest::OwnedChildProcess<> child;
    REQUIRE(child.canOwnChildren());
    int master = -1;
    const pid_t process = ::forkpty(&master, nullptr, nullptr, nullptr);
    if (process == 0) {
        for (;;)
            ::pause();
    }
    PAL::UnixFileDescriptor masterOwner(master);
    REQUIRE(child.adoptForkResult(process) && masterOwner);
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (::tcgetsid(masterOwner.get()) != child.processID() && std::chrono::steady_clock::now() < readyDeadline)
        ::usleep(1'000);
    REQUIRE(::tcgetsid(masterOwner.get()) == child.processID());
    struct termios modes;
    REQUIRE(::tcgetattr(masterOwner.get(), &modes) == 0);
    ::cfmakeraw(&modes);
    REQUIRE(::tcsetattr(masterOwner.get(), TCSANOW, &modes) == 0);
    char replicaPath[128];
    REQUIRE(::ptsname_r(masterOwner.get(), replicaPath, sizeof(replicaPath)) == 0);
    PAL::UnixFileDescriptor replica(::open(replicaPath, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
    REQUIRE(replica);
    auto stream = PAL::adoptPTY(masterOwner.release(), static_cast<PAL::ProcessID>(::getpid()));
    REQUIRE(stream);
    CHECK(!stream->writeAllOwned(0, std::span<const char>("bad", 3)));
    CHECK(!stream->discardPendingWrite(0));
    CHECK(stream->writeAllOwned(11, {}) == std::optional { 0 });
    const std::string first(4 * 1024 * 1024, 'a');
    REQUIRE(stream->writeAllOwned(11, first) == std::optional { 0 });
    REQUIRE_MSG(stream->hasPendingWrite(), "premise: the real PTY refused a tail while its replica was not read");
    REQUIRE(stream->writeAllOwned(22, std::span<const char>("neighbor", 8)) == std::optional { 0 });
    REQUIRE(stream->writeAll(std::span<const char>("reply", 5)) == 0);
    const auto discarded = stream->discardPendingWrite(11);
    REQUIRE(discarded && *discarded > 0 && *discarded <= first.size());
    CHECK(stream->discardPendingWrite(11) == std::optional<std::size_t> { 0 });
    CHECK(stream->discardPendingWrite(999) == std::optional<std::size_t> { 0 });
    CHECK_EQ(stream->pendingWriteBytes(), 13u);
    const std::string expected(first.size() - *discarded, 'a');
    std::string received;
    const auto readDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (received.size() < expected.size() + 13 && std::chrono::steady_clock::now() < readDeadline) {
        (void)stream->drainPendingWrite();
        char buffer[16384];
        const ssize_t count = ::read(replica.get(), buffer, sizeof(buffer));
        if (count > 0)
            received.append(buffer, static_cast<std::size_t>(count));
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            ::usleep(1'000);
        else
            break;
    }
    CHECK_STR_EQ(received, expected + "neighborreply");
    CHECK(!stream->hasPendingWrite());
    CHECK(stream->discardPendingWrite(22) == std::optional<std::size_t> { 0 });
}

TEST(aStandaloneOwnedInterruptEscapesCapacityWithoutAdmittingOtherBytes)
{
    int signals[2];
    REQUIRE(::pipe(signals) == 0);
    PAL::UnixFileDescriptor readSignal(signals[0]);
    PAL::UnixFileDescriptor writeSignal(signals[1]);
    REQUIRE(::fcntl(readSignal.get(), F_SETFL, O_NONBLOCK) == 0);
    UbiquittyTest::OwnedChildProcess<> child;
    REQUIRE(child.canOwnChildren());
    int master = -1;
    const pid_t process = ::forkpty(&master, nullptr, nullptr, nullptr);
    if (process == 0) {
        readSignal.reset();
        // A raw fork keeps this process's dispositions, and a test run started
        // in the background of a script has SIGINT ignored. The kernel discards
        // an ignored signal when it is generated, blocked or not, so sigwait
        // would never see the ^C and the case would wait out its bound.
        // openPTY resets a pane's signals itself; this child is not a pane.
        struct sigaction byDefault {};
        byDefault.sa_handler = SIG_DFL;
        ::sigaction(SIGINT, &byDefault, nullptr);
        sigset_t waiting;
        sigemptyset(&waiting);
        sigaddset(&waiting, SIGINT);
        ::sigprocmask(SIG_BLOCK, &waiting, nullptr);
        // A report that cannot be written ends the child, and the parent's
        // read of the closed pipe fails the case at once.
        const char ready = 'r';
        if (::write(writeSignal.get(), &ready, 1) != 1)
            _exit(1);
        int caught = 0;
        if (::sigwait(&waiting, &caught) == 0 && caught == SIGINT) {
            const char received = 'i';
            if (::write(writeSignal.get(), &received, 1) != 1)
                _exit(1);
        }
        for (;;)
            ::pause();
    }
    PAL::UnixFileDescriptor masterOwner(master);
    REQUIRE(child.adoptForkResult(process) && masterOwner);
    writeSignal.reset();
    // Waits for the child's report itself rather than for a time it should
    // have arrived by: under load a child at low priority can take seconds to
    // be scheduled between the signal and its write, and a three-second window
    // failed a run for that alone. The bound only turns a lost report into a
    // failure instead of a hang.
    const auto receiveSignal = [&] {
        char byte = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        for (;;) {
            const ssize_t count = ::read(readSignal.get(), &byte, 1);
            if (count == 1)
                return byte;
            // End of file: the child has exited, and no report is coming.
            if (count == 0)
                return char {};
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                return char {};
            struct pollfd readable { readSignal.get(), POLLIN, 0 };
            (void)::poll(&readable, 1, static_cast<int>(remaining.count()));
        }
    };
    REQUIRE(receiveSignal() == 'r');
    struct termios modes;
    REQUIRE(::tcgetattr(masterOwner.get(), &modes) == 0);
    ::cfmakeraw(&modes);
    modes.c_lflag |= ISIG | NOFLSH;
    modes.c_lflag &= ~ECHO;
    modes.c_cc[VINTR] = 3;
    REQUIRE(::tcsetattr(masterOwner.get(), TCSANOW, &modes) == 0);
    auto stream = PAL::adoptPTY(masterOwner.release(), static_cast<PAL::ProcessID>(::getpid()));
    REQUIRE(stream);
    const std::string fill(PAL::OutputQueue::maxPending, 'x');
    REQUIRE(stream->writeAllOwned(11, fill) == std::optional { 0 });
    for (int pass = 0; pass < 16 && stream->pendingWriteBytes() < fill.size(); ++pass) {
        const auto remaining = fill.size() - stream->pendingWriteBytes();
        REQUIRE(stream->writeAllOwned(11, std::span(fill).first(remaining)) == std::optional { 0 });
    }
    REQUIRE(stream->pendingWriteBytes() == fill.size());
    const std::array<char, 2> compound { 'a', '\x03' };
    CHECK(stream->writeAllOwned(22, compound) == std::optional { status(PAL::StreamStatus::Again) });
    char unexpected = 0;
    CHECK(::read(readSignal.get(), &unexpected, 1) < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    CHECK(stream->pendingOwnedWriteBytes(22) == std::optional<std::size_t> { 0 });
    const char interrupt = '\x03';
    CHECK(stream->writeAllOwned(22, std::span(&interrupt, 1)) == std::optional { 0 });
    CHECK(receiveSignal() == 'i');
    CHECK(stream->pendingOwnedWriteBytes(11) == std::optional { fill.size() });
    CHECK(stream->pendingOwnedWriteBytes(22) == std::optional<std::size_t> { 0 });
}

// The inherited stream never receives direct-child reaping authority.
struct InheritedPTYChild {
    PAL::UnixFileDescriptor master;
    // Observation may name a borrowed descendant; only the owner may signal
    // or reap a direct fork child.
    pid_t process = -1;
    UbiquittyTest::OwnedChildProcess<> owner;

    ~InheritedPTYChild()
    {
        master.reset();
        (void)owner.stop();
    }

    bool canOwnChildren() const { return owner.canOwnChildren(); }

    bool adoptForkResult(pid_t result)
    {
        if (!owner.adoptForkResult(result))
            return false;
        process = result;
        return true;
    }

    bool signal(int signalNumber) { return owner.signal(signalNumber); }

    pid_t reap(int flags, int* status = nullptr) { return owner.reap(flags, status); }

    /// Reaps the signalled leader while reading its terminal, within a bound.
    ///
    /// A process that exits closes its controlling terminal, and that close
    /// can wait for the terminal's unread output to drain. The leader prints
    /// "ready", which nothing here reads, so a blocking wait with the master
    /// open waits on a shell stuck in its exit, for hours under load. Reading the
    /// master releases it, and the bound turns any other wedge into a failure.
    ///
    /// - Parameter status: Receives the exit status when the leader is reaped.
    /// - Returns: The reaped identity, or zero when the bound passed first.
    pid_t reapWhileReading(int* status)
    {
        const pid_t expected = process;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        for (;;) {
            const pid_t reaped = owner.reap(WNOHANG, status);
            if (reaped != 0)
                return reaped == expected ? reaped : -1;
            if (std::chrono::steady_clock::now() >= deadline)
                return 0;
            struct pollfd readable { master.get(), POLLIN, 0 };
            if (master && ::poll(&readable, 1, 1) > 0 && (readable.revents & POLLIN)) {
                char discarded[256];
                // End of file or an error leaves no output for the leader's
                // exit to wait on, so the loop paces itself until the reap.
                if (::read(master.get(), discarded, sizeof(discarded)) <= 0)
                    ::usleep(1000);
            } else if (!master) {
                ::usleep(1000);
            }
        }
    }

    bool start()
    {
        if (owner.processID() > 1 || !canOwnChildren())
            return false;
        int descriptor = -1;
        process = ::forkpty(&descriptor, nullptr, nullptr, nullptr);
        if (!process) {
            ::execl("/bin/sh", "sh", "-c", "printf 'ready\\n'; while IFS= read -r line; do printf 'got:%s\\n' \"$line\"; done", nullptr);
            _exit(127);
        }
        master.reset(descriptor);
        if (!adoptForkResult(process) || !master)
            return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (::tcgetsid(master.get()) != process && std::chrono::steady_clock::now() < deadline)
            ::usleep(1000);
        return ::tcgetsid(master.get()) == process;
    }
};

TEST(anInheritedPTYCloseSignalsItsLeaderDespiteAnotherMasterOwner)
{
    InheritedPTYChild child;
    REQUIRE(child.start());
    const std::array parents { static_cast<PAL::ProcessID>(::getpid()) };
    auto stream = PAL::inheritPTY(child.master.duplicate().release(), child.process, parents);
    REQUIRE(stream);
    char buffer[256];
    int last = 0;
    (void)drainWithBudget(stream.get(), buffer, sizeof(buffer), 100, &last);
    stream.reset();
    // Keeping a second master means an exit cannot be attributed to last-close
    // kernel hangup. The inherited Terminate policy must send the signal.
    int result = 0;
    pid_t reaped = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!(reaped = child.reap(WNOHANG, &result)) && std::chrono::steady_clock::now() < deadline)
        ::usleep(1000);
    CHECK(reaped == child.process);
    if (reaped == child.process) {
        child.process = -1;
        CHECK(WIFSIGNALED(result) && WTERMSIG(result) == SIGHUP);
    }
}

TEST(anInheritedPTYRefusesAnUnrelatedSessionAndConsumesTheDescriptor)
{
    InheritedPTYChild child;
    REQUIRE(child.start());
    const std::array parents { static_cast<PAL::ProcessID>(::getpid()) };
    const int descriptor = child.master.duplicate().release();
    auto stream = PAL::inheritPTY(descriptor, static_cast<PAL::ProcessID>(::getpid()), parents);
    CHECK(!stream);
    CHECK(::fcntl(descriptor, F_GETFD) < 0 && errno == EBADF);
}

TEST(anInheritedPTYRefusesWrongParentAndNonTerminalDescriptors)
{
    InheritedPTYChild child;
    REQUIRE(child.start());
    const std::array wrongParents { static_cast<PAL::ProcessID>(child.process) };
    const int wrongParentDescriptor = child.master.duplicate().release();
    CHECK(!PAL::inheritPTY(wrongParentDescriptor, child.process, wrongParents));
    CHECK(::fcntl(wrongParentDescriptor, F_GETFD) < 0 && errno == EBADF);
    const int noParentDescriptor = child.master.duplicate().release();
    CHECK(!PAL::inheritPTY(noParentDescriptor, child.process, {}));
    CHECK(::fcntl(noParentDescriptor, F_GETFD) < 0 && errno == EBADF);
    auto pipe = PAL::makeUnixPipe();
    REQUIRE(pipe);
    const int pipeDescriptor = pipe.read.release();
    const std::array parents { static_cast<PAL::ProcessID>(::getpid()) };
    CHECK(!PAL::inheritPTY(pipeDescriptor, child.process, parents));
    CHECK(::fcntl(pipeDescriptor, F_GETFD) < 0 && errno == EBADF);
    CHECK(child.process > 1 && ::kill(child.process, 0) == 0);
}

TEST(anInheritedPTYRejectsChangedBirthIdentityBeforeSignaling)
{
    InheritedPTYChild child;
    REQUIRE(child.start());
    const auto snapshot = PAL::ptyProcessSnapshot(child.process);
    REQUIRE(snapshot);
    CHECK(POSIX::matchesInheritedPTY(child.master.get(), child.process, snapshot->birth));
    auto differentBirth = snapshot->birth;
    differentBirth.low ^= 1;
    CHECK(!POSIX::matchesInheritedPTY(child.master.get(), child.process, differentBirth));
    differentBirth = snapshot->birth;
    differentBirth.high ^= 1;
    CHECK(!POSIX::matchesInheritedPTY(child.master.get(), child.process, differentBirth));
    CHECK(!POSIX::matchesInheritedPTY(child.master.get(), static_cast<PAL::ProcessID>(::getpid()), snapshot->birth));
    CHECK(!POSIX::matchesInheritedPTY(-1, child.process, snapshot->birth));
    CHECK(!PAL::ptyProcessSnapshot(-1));
    REQUIRE(child.signal(SIGTERM));
    int result = 0;
    REQUIRE(child.reapWhileReading(&result) == child.process);
    CHECK(!POSIX::matchesInheritedPTY(child.master.get(), child.process, snapshot->birth));
    child.process = -1;
}

TEST(anInheritedPTYKeepsItsLeaderRunningAndItsIdentityForFacts)
{
    InheritedPTYChild child;
    REQUIRE(child.start());
    const std::array parents { static_cast<PAL::ProcessID>(::getpid()) };
    auto stream = PAL::inheritPTY(child.master.duplicate().release(), child.process, parents);
    REQUIRE(stream);
    CHECK(stream->process().hasRunningChild());
    CHECK(stream->process().foregroundIsShell());
    std::array<PAL::Process::Record, 8> records;
    const int count = stream->process().gatherProcesses(records);
    CHECK(std::ranges::any_of(std::span(records).first(static_cast<std::size_t>(count)), [&](const auto& record) {
        return record.processID == static_cast<std::uint32_t>(child.process) && record.isSessionLeader;
    }));
    CHECK(!stream->childExit().didExit);
    stream->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
    stream.reset();
    CHECK(child.process > 1 && ::kill(child.process, 0) == 0);
    auto next = PAL::inheritPTY(child.master.duplicate().release(), child.process, parents);
    REQUIRE(next);
    REQUIRE(next->writeAll(std::span("hi\n", 3)) >= 0);
    CHECK(drainUntilContains(next.get(), "got:hi"));
    next->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
}

TEST(anInheritedPTYNeverReapsEvenWhenItsLeaderHappensToBeThisProcessChild)
{
    InheritedPTYChild child;
    REQUIRE(child.start());
    const std::array parents { static_cast<PAL::ProcessID>(::getpid()) };
    auto stream = PAL::inheritPTY(child.master.duplicate().release(), child.process, parents);
    REQUIRE(stream);
    REQUIRE(stream->writeAll(std::span("exit\n", 5)) >= 0);
    // This shell's loop echoes 'exit'; terminate explicitly with a known code.
    REQUIRE(child.signal(SIGTERM));
    char buffer[256];
    int last = 0;
    (void)drainWithBudget(stream.get(), buffer, sizeof(buffer), 2000, &last);
    CHECK(last == status(PAL::StreamStatus::EndOfFile));
    CHECK(!stream->process().hasRunningChild());
    CHECK(!stream->childExit().didExit);
    (void)stream->write(std::span("late", 4));
    stream.reset();
    int result = 0;
    const auto process = child.process;
    CHECK(child.reapWhileReading(&result) == process);
    child.process = -1;
    CHECK(WIFSIGNALED(result) && WTERMSIG(result) == SIGTERM);
}

TEST(anInheritedPTYDuplicateObservesHangupWithoutReapingItsLeader)
{
    enum class DuplicateDepth {
        Once,
        Twice
    };
    for (auto duplicateDepth : { DuplicateDepth::Once, DuplicateDepth::Twice }) {
        InheritedPTYChild child;
        REQUIRE(child.start());
        const std::array parents { static_cast<PAL::ProcessID>(::getpid()) };
        auto stream = PAL::inheritPTY(child.master.duplicate().release(), child.process, parents);
        REQUIRE(stream);
        auto endpoint = stream->duplicateAsPollableStream();
        REQUIRE(endpoint);
        auto otherEndpoint = stream->duplicateAsPollableStream();
        REQUIRE(otherEndpoint);
        if (duplicateDepth == DuplicateDepth::Twice)
            endpoint = endpoint->duplicateAsPollableStream();
        REQUIRE(endpoint);
        REQUIRE(child.signal(SIGTERM));
        char buffer[256];
        int result = status(PAL::StreamStatus::Again);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            result = endpoint->read(buffer);
            if (result == status(PAL::StreamStatus::Again))
                ::usleep(1000);
        } while ((result > 0 || result == status(PAL::StreamStatus::Again)) && std::chrono::steady_clock::now() < deadline);
        CHECK_EQ(result, status(PAL::StreamStatus::EndOfFile));
        CHECK_EQ(endpoint->lastError(), 0);
        CHECK_EQ(endpoint->read(buffer), status(PAL::StreamStatus::EndOfFile));
        CHECK(!stream->process().hasRunningChild());
        CHECK(!stream->childExit().didExit);
        CHECK(endpoint->writeAll(std::span("late", 4)) <= 0);
        CHECK_EQ(otherEndpoint->write(std::span("late", 4)), status(PAL::StreamStatus::Closed));
        CHECK_EQ(otherEndpoint->writeAll(std::span("late", 4)), status(PAL::StreamStatus::Closed));
        CHECK(stream->writeAll(std::span("late", 4)) <= 0);
        endpoint.reset();
        stream.reset();
        int exitStatus = 0;
        CHECK(child.reapWhileReading(&exitStatus) == child.process);
        child.process = -1;
        CHECK(WIFSIGNALED(exitStatus) && WTERMSIG(exitStatus) == SIGTERM);
    }
}

TEST(closingOneInheritedPTYDuplicateDoesNotEndTheSharedSession)
{
    InheritedPTYChild child;
    REQUIRE(child.start());
    const std::array parents { static_cast<PAL::ProcessID>(::getpid()) };
    auto stream = PAL::inheritPTY(child.master.duplicate().release(), child.process, parents);
    REQUIRE(stream);
    auto endpoint = stream->duplicateAsPollableStream();
    REQUIRE(endpoint);
    endpoint->handle().closeHandle();
    char buffer[256];
    CHECK_EQ(endpoint->read(buffer), status(PAL::StreamStatus::Closed));
    CHECK(stream->process().hasRunningChild());
    CHECK(!stream->childExit().didExit);
    CHECK(stream->writeAll(std::span("still here\n", 11)) >= 0);
    CHECK(drainUntilContains(stream.get(), "got:still here"));
    stream->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
}

TEST(anInheritedPTYWorksInANonParentSuccessor)
{
    InheritedPTYChild child;
    REQUIRE(child.start());
    const auto parent = static_cast<PAL::ProcessID>(::getpid());
    UbiquittyTest::OwnedChildProcess<> successorOwner;
    REQUIRE(successorOwner.canOwnChildren());
    const auto successor = ::fork();
    REQUIRE(successor >= 0);
    if (!successor) {
        // Bounds the parent's blocking reap below, past the echo's own bound.
        ::alarm(30);
        int unused = 0;
        if (::waitpid(child.process, &unused, WNOHANG) >= 0 || errno != ECHILD)
            _exit(11);
        const std::array parents { parent };
        auto stream = PAL::inheritPTY(child.master.release(), child.process, parents);
        if (!stream)
            _exit(12);
        stream->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
        if (stream->writeAll(std::span("successor\n", 10)) < 0)
            _exit(13);
        const bool hasEcho = drainUntilContains(stream.get(), "got:successor");
        stream.reset();
        _exit(hasEcho ? 0 : 14);
    }
    REQUIRE(successorOwner.adoptForkResult(successor));
    int result = 0;
    CHECK(successorOwner.reap(0, &result) == successor);
    CHECK(WIFEXITED(result) && WEXITSTATUS(result) == 0);
    CHECK(child.process > 1 && ::kill(child.process, 0) == 0);
}

TEST(aReleasedPTYKeepsItsChildForTheInheritedOwner)
{
    auto predecessor = openArguments({ "/bin/sh", "-c", "printf 'ready\\n'; while IFS= read -r line; do printf 'got:%s\\n' \"$line\"; done" });
    REQUIRE(predecessor);
    char buffer[256];
    int last = 0;
    (void)drainWithBudget(predecessor.get(), buffer, sizeof(buffer), 100, &last);
    const auto child = static_cast<PAL::ProcessID>(::tcgetsid(predecessor->fileDescriptor()));
    REQUIRE(child > 1);
    const std::array parents { static_cast<PAL::ProcessID>(::getpid()) };
    auto successor = PAL::inheritPTY(::dup(predecessor->fileDescriptor()), child, parents);
    REQUIRE(successor);
    predecessor->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
    predecessor.reset();
    REQUIRE(child > 1 && ::kill(child, 0) == 0);
    REQUIRE(successor->writeAll(std::span("hi\n", 3)) >= 0);
    CHECK(drainUntilContains(successor.get(), "got:hi"));
}

TEST(anInheritedPTYRestoresOnlyUnwrittenBytesAndRetainsOwnerRepair)
{
    for (const bool shouldRepair : { false, true }) {
        InheritedPTYChild child;
        REQUIRE(child.canOwnChildren());
        int descriptor = -1;
        child.process = ::forkpty(&descriptor, nullptr, nullptr, nullptr);
        if (!child.process) {
            for (;;)
                ::pause();
        }
        child.master.reset(descriptor);
        REQUIRE(child.adoptForkResult(child.process) && child.master);
        // This child never enters read; even a stopped shell can already have
        // an in-kernel read competing with the parent's raw slave reader.
        const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (::tcgetsid(child.master.get()) != child.process && std::chrono::steady_clock::now() < readyDeadline)
            ::usleep(1000);
        REQUIRE(::tcgetsid(child.master.get()) == child.process);
        struct termios modes {};
        REQUIRE(::tcgetattr(child.master.get(), &modes) == 0);
        ::cfmakeraw(&modes);
        REQUIRE(::tcsetattr(child.master.get(), TCSANOW, &modes) == 0);
        char replicaPath[128];
        REQUIRE(::ptsname_r(child.master.get(), replicaPath, sizeof(replicaPath)) == 0);
        PAL::UnixFileDescriptor replica(::open(replicaPath, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
        REQUIRE(replica);
        const std::array parents { static_cast<PAL::ProcessID>(::getpid()) };
        auto predecessor = PAL::inheritPTY(child.master.duplicate().release(), child.process, parents);
        REQUIRE(predecessor);
        predecessor->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
        const std::string plain(4 * 1024 * 1024, 'r');
        const std::string paste = "\x1b[200~accepted paste\x1b[201~";
        REQUIRE(predecessor->writeAll(plain) == 0);
        REQUIRE(predecessor->hasPendingWrite());
        REQUIRE(predecessor->writeAllOwned(11, paste) == std::optional { 0 });
        REQUIRE(predecessor->writeAllOwned(22, std::span("neighbor", 8)) == std::optional { 0 });
        REQUIRE(predecessor->writeAll(std::span("reply", 5)) == 0);
        const auto queued = predecessor->pendingWriteBytes();
        auto copied = predecessor->copyPendingWrite();
        REQUIRE(copied);
        CHECK_EQ(copied->bytes().size(), queued);
        CHECK_EQ(predecessor->pendingWriteBytes(), queued);
        // A backend retaining Stream's default cannot claim a handover owner.
        CHECK(!predecessor->PAL::Stream::copyPendingWrite());
        CHECK(!predecessor->PAL::Stream::restorePendingWrite(*copied));
        CHECK_EQ(copied->bytes().size(), queued);
        auto successor = PAL::inheritPTY(child.master.duplicate().release(), child.process, parents);
        REQUIRE(successor);
        successor->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
        REQUIRE(successor->restorePendingWrite(*copied));
        CHECK(copied->bytes().empty() && copied->runs().empty());
        CHECK_EQ(successor->pendingWriteBytes(), queued);
        CHECK(successor->pendingOwnedWriteBytes(11) == std::optional { paste.size() });
        CHECK(successor->pendingOwnedWriteBytes(22) == std::optional<std::size_t> { 8 });
        auto duplicate = predecessor->copyPendingWrite();
        REQUIRE(duplicate);
        const char* duplicateBytes = duplicate->bytes().data();
        CHECK(!successor->restorePendingWrite(*duplicate));
        CHECK(duplicate->bytes().data() == duplicateBytes);
        CHECK_EQ(duplicate->bytes().size(), queued);
        CHECK_EQ(successor->pendingWriteBytes(), queued);
        predecessor.reset();
        REQUIRE(child.process > 1 && ::kill(child.process, 0) == 0);
        if (shouldRepair) {
            REQUIRE(successor->repairPendingWrite(11, std::span("\x1b[201~", 6)));
            CHECK(successor->pendingOwnedWriteBytes(11) == std::optional<std::size_t> { 0 });
        }
        REQUIRE(successor->writeAllOwned(33, std::span("new-input", 9)) == std::optional { 0 });
        const std::string expected = plain + (shouldRepair ? "\x1b[201~" : paste) + "neighborreplynew-input";
        std::string received;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (received.size() < expected.size() && std::chrono::steady_clock::now() < deadline) {
            (void)successor->drainPendingWrite();
            char buffer[16384];
            const auto count = ::read(replica.get(), buffer, sizeof(buffer));
            if (count > 0)
                received.append(buffer, static_cast<std::size_t>(count));
            else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                ::usleep(1000);
            else
                break;
        }
        const auto firstDifference = std::mismatch(received.begin(), received.end(), expected.begin(), expected.end());
        const std::string delivery = "received=" + std::to_string(received.size()) + " expected=" + std::to_string(expected.size())
            + " firstDifference=" + std::to_string(firstDifference.first - received.begin()) + " pending=" + std::to_string(successor->pendingWriteBytes());
        CHECK_MSG(received == expected, delivery.c_str());
        CHECK(!successor->hasPendingWrite());
        CHECK(successor->pendingOwnedWriteBytes(22) == std::optional<std::size_t> { 0 });
        auto empty = successor->copyPendingWrite();
        REQUIRE(empty);
        CHECK(empty->bytes().empty() && empty->runs().empty());
        successor.reset();
        CHECK(child.process > 1 && ::kill(child.process, 0) == 0);
    }
}

TEST(aClosedInheritedPTYCannotExportOrConsumeUnwrittenInput)
{
    InheritedPTYChild child;
    REQUIRE(child.start());
    const std::array parents { static_cast<PAL::ProcessID>(::getpid()) };
    auto stream = PAL::inheritPTY(child.master.duplicate().release(), child.process, parents);
    REQUIRE(stream);
    stream->setProcessClosePolicy(PAL::ProcessClosePolicy::KeepRunning);
    const std::array runs { PAL::OutputQueue::Run { 9, 7 } };
    auto candidate = PAL::OutputQueue::Unwritten::copyOf(std::span("pending", 7), runs);
    REQUIRE(candidate);
    const char* allocation = candidate->bytes().data();
    // The child is signalled once it runs its loop, and reaped while the
    // master is read: an exiting process closes its terminal, a close can
    // wait for the terminal's unread output to drain, and a blocking wait
    // here held the run for 23 minutes on a shell stuck in its exit.
    char buffer[128];
    std::string seen;
    const auto readyBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!seen.contains("ready") && std::chrono::steady_clock::now() < readyBy) {
        const int count = stream->read({ buffer, sizeof(buffer) });
        if (count > 0)
            seen.append(buffer, static_cast<std::size_t>(count));
        else
            ::usleep(1000);
    }
    REQUIRE(seen.contains("ready"));
    REQUIRE(child.signal(SIGKILL));
    int endStatus = 0;
    pid_t reaped = 0;
    const auto reapedBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(reaped = child.reap(WNOHANG)) && std::chrono::steady_clock::now() < reapedBy) {
        const int count = stream->read({ buffer, sizeof(buffer) });
        if (count <= 0 && count != status(PAL::StreamStatus::Again))
            endStatus = count;
        ::usleep(1000);
    }
    REQUIRE(reaped == child.process);
    child.process = -1;
    if (!endStatus)
        (void)drainWithBudget(stream.get(), buffer, sizeof(buffer), 3000, &endStatus);
    REQUIRE(endStatus == status(PAL::StreamStatus::EndOfFile) || endStatus == status(PAL::StreamStatus::Closed));
    CHECK(!stream->copyPendingWrite());
    CHECK(!stream->restorePendingWrite(*candidate));
    CHECK(candidate->bytes().data() == allocation);
    CHECK_EQ(candidate->bytes().size(), 7u);
    CHECK_EQ(candidate->runs().size(), 1u);
    CHECK_EQ(candidate->runs().front().owner, 9u);
}

TEST(anInheritedPTYSurvivesItsPredecessorsExitWithoutChangingIdentity)
{
    int masterDescriptor = -1;
    int replicaDescriptor = -1;
    REQUIRE(::openpty(&masterDescriptor, &replicaDescriptor, nullptr, nullptr, nullptr) == 0);
    InheritedPTYChild session;
    session.master.reset(masterDescriptor);
    PAL::UnixFileDescriptor replica(replicaDescriptor);
    auto identity = PAL::makeUnixPipe();
    auto release = PAL::makeUnixPipe();
    REQUIRE(identity && release);
    InheritedPTYChild predecessor;
    REQUIRE(predecessor.canOwnChildren());
    predecessor.process = ::fork();
    REQUIRE(predecessor.process >= 0);
    if (!predecessor.process) {
        ::alarm(5);
        identity.read.reset();
        release.write.reset();
        session.master.reset();
        const auto leader = ::fork();
        if (leader < 0)
            _exit(21);
        if (!leader) {
            ::alarm(5);
            identity.write.reset();
            release.read.reset();
            if (::setsid() < 0 || ::ioctl(replica.get(), TIOCSCTTY, 0) < 0)
                _exit(22);
            for (int standard = 0; standard < 3; ++standard) {
                if (::dup2(replica.get(), standard) < 0)
                    _exit(23);
            }
            replica.reset();
            ::execl("/bin/sh", "sh", "-c", "while IFS= read -r line; do printf 'got:%s\\n' \"$line\"; done", nullptr);
            _exit(24);
        }
        replica.reset();
        if (::write(identity.write.get(), &leader, sizeof(leader)) != sizeof(leader))
            _exit(25);
        // The parent releases this exit with one byte; end of file means it
        // never did, and the status says so.
        char ready = 0;
        if (::read(release.read.get(), &ready, 1) != 1)
            _exit(26);
        _exit(0);
    }
    REQUIRE(predecessor.adoptForkResult(predecessor.process));
    replica.reset();
    identity.write.reset();
    release.read.reset();
    const auto reported = UbiquittyTest::readReportedProcessID([&](pid_t* process, std::size_t size) {
        return ::read(identity.read.get(), process, size);
    });
    REQUIRE(reported);
    session.process = *reported;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (::tcgetsid(session.master.get()) != session.process && std::chrono::steady_clock::now() < deadline)
        ::usleep(1000);
    REQUIRE(::tcgetsid(session.master.get()) == session.process);
    const auto original = PAL::ptyProcessSnapshot(session.process);
    REQUIRE(original);
    REQUIRE(original->parentProcessID == predecessor.process);
    const std::array parents { static_cast<PAL::ProcessID>(predecessor.process) };
    auto stream = PAL::inheritPTY(session.master.duplicate().release(), session.process, parents);
    REQUIRE(stream);
    REQUIRE(::write(release.write.get(), "x", 1) == 1);
    int result = 0;
    REQUIRE(predecessor.reap(0, &result) == predecessor.process);
    predecessor.process = -1;
    REQUIRE(WIFEXITED(result) && WEXITSTATUS(result) == 0);
    const auto reparented = PAL::ptyProcessSnapshot(session.process);
    REQUIRE(reparented);
    CHECK(reparented->parentProcessID != original->parentProcessID);
    CHECK(reparented->birth == original->birth);
    CHECK(POSIX::matchesInheritedPTY(session.master.get(), session.process, original->birth));
    REQUIRE(stream->writeAll(std::span("after\n", 6)) >= 0);
    CHECK(drainUntilContains(stream.get(), "got:after"));
    stream.reset();
    const auto exitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (PAL::ptyProcessSnapshot(session.process) && std::chrono::steady_clock::now() < exitDeadline)
        ::usleep(1000);
    CHECK(!PAL::ptyProcessSnapshot(session.process));
    // Its replacement parent owns reaping; no waitpid assertion is possible.
    session.process = -1;
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], reportArgument0Marker) == 0) {
        std::printf("[%s]", argv[0]);
        return failures == 0 ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], sleepsMarker) == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(std::strtoul(argv[2], nullptr, 10)));
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], mainThreadExitsMarker) == 0) {
        // The first thread ends and a second sleeps on, so the process runs
        // without the thread it started with.
        const unsigned long seconds = std::strtoul(argv[2], nullptr, 10);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            std::_Exit(0);
        }).detach();
        ::pthread_exit(nullptr);
    }
#if defined(__linux__)
    if (argc == 4 && std::strcmp(argv[1], renamesItselfMarker) == 0) {
        ::prctl(PR_SET_NAME, argv[2], 0, 0, 0);
        std::this_thread::sleep_for(std::chrono::seconds(std::strtoul(argv[3], nullptr, 10)));
        return 0;
    }
#endif

    executablePath = absoluteExecutablePath(argv[0]);
    UBIQUITTY_TEST_PIN_PREFERENCES();
    return UbiquittyTest::runRegisteredCases(argc, argv);
}

TEST(theLineDisciplineReadsCustomAndDisabledEraseCharacters)
{
    struct Case {
        const char* setting;
        std::optional<std::uint8_t> expected;
    };
    for (const Case& wanted : { Case { "^H", 8 }, Case { "^-", std::nullopt }, Case { "^?", 127 } }) {
        const std::vector<std::string> arguments { "/bin/sh", "-c", std::string("stty erase '") + wanted.setting + "'; sleep 5" };
        auto stream = PAL::openPTY(arguments);
        REQUIRE(stream);
        std::optional<PAL::LineDiscipline> discipline;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            discipline = stream->process().lineDiscipline();
            if (discipline && discipline->eraseCharacter == wanted.expected)
                break;
            ::usleep(5'000);
        }
        REQUIRE(discipline);
        CHECK(discipline->eraseCharacter == wanted.expected);
    }
}
