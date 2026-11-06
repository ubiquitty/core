// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/history_plane.cpp — the daemon serves the history plane
// (Docs/sync.md §6.6): through its own screen step, the test
// reading what the daemon queues as a transport would, and over a socket to a
// live daemon.
//
// Through the friend seam:
//
//   * A resize through the daemon's own path, `Viewport` to reflow to the
//     Structural publish, sends no history byte.
//   * The clear-buffer verb and an ED 3 from the program each send one
//     HistoryReset, and the client holds none of the old lines after either.
//   * Ten thousand wants leave the daemon's live allocations and its queued
//     bulk flat.
//   * A metered client is pushed the one viewport above the screen.
//
// Over a socket, given the built host binary: the Sync carrier takes the
// client's HistoryWant up and carries the history frames and the reset down.
//
// Exit 0 on success.
//

#include "Assertions.h"
#include "Core/Crypto/SHA256.h"
#include "Host/HostConnection.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "LoopbackPaneLink.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostSocketName.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Geometry.h"
#include "Core/Protocol/Sync/HistoryCache.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Platform/StreamDouble.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <new>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
using Ubiquitty::Protocol::Sync::AdmissionTarget;
using Ubiquitty::Protocol::Sync::AttachmentAdmission;
using Ubiquitty::Protocol::Sync::FrameKind;
using Ubiquitty::Protocol::Sync::HistoryPriority;
using Ubiquitty::Protocol::Sync::LineRange;
using Ubiquitty::Protocol::Sync::LinkCost;
using Ubiquitty::Protocol::Sync::Role;

namespace {

// The daemon's session, named through the seam that may name it.
using Session = std::remove_reference_t<decltype(Screen::makeSession(std::declval<Daemon&>(), 0, 0))>;

// A session whose history holds every line written to it, with `lines` of
// them above its prompt, and a client attached to it that may drive it.
struct Attached {
    Session& session;
    std::size_t index;
};

Attached attachedSession(Daemon& d, int lines)
{
    Session& session = Screen::makeSession(d, 24, 80, std::make_unique<Ubiquitty::TestDoubles::StreamDouble>());
    session.term->scrollbackLimit = 20'000;
    for (int line = 0; line < lines; ++line)
        UbiquittyTest::feed(*session.term, "row " + std::to_string(line) + "\r\n");
    const std::size_t index = Screen::attach(d, session.id);
    // The scope a `Viewport` needs, in a token minted for the person at the
    // window, as a human client's is.
    Screen::grantScope(d, index, { Ubiquitty::Protocol::Sync::Scope::Observe, Ubiquitty::Protocol::Sync::Scope::Drive });
    Screen::markInteractiveUser(d, index);
    return { session, index };
}

// Carries a client's screen plane until it goes quiet: the daemon paces on
// its own clock, so one pass of `deliver` can end before a transfer it owes
// is due. Quiet is twenty passes in a row that carry nothing.
void settle(Daemon& d, std::size_t index, UbiquittyTest::ScreenClient& client)
{
    std::size_t carried = client.tags().size();
    for (int quiet = 0, pass = 0; quiet < 20 && pass < 5'000; ++pass) {
        Screen::deliver(d, index, client);
        quiet = client.tags().size() == carried ? quiet + 1 : 0;
        carried = client.tags().size();
        ::usleep(1'000);
    }
}

// Greets a client and shows it the session: a pane no connection shows is
// captured by nobody, so the frame its showing asks for is published here, as
// the loop would.
void greet(Daemon& d, Attached& attached, UbiquittyTest::ScreenClient& client)
{
    Screen::greet(d, attached.index, client);
    Screen::publish(d, attached.session, Ubiquitty::Runtime::FrameClass::Structural);
    settle(d, attached.index, client);
}

// Publishes what the session's terminal holds and carries it.
void publish(Daemon& d, Attached& attached, UbiquittyTest::ScreenClient& client)
{
    Screen::publish(d, attached.session);
    settle(d, attached.index, client);
}

const Ubiquitty::Protocol::Sync::HistoryCache* cacheOf(const UbiquittyTest::ScreenClient& client)
{
    return client.connection().historyCache(1);
}

// How many blocks operator new has handed out and not had back.
//
// What the daemon retains per want, counted rather than read off the resident
// set. The resident set's high-water mark steps by an allocator arena, a
// megabyte or so, with nothing retained, so two readings of it differ by noise
// near any bound a test could set. A count of live blocks moves only when
// something is kept: an object a want leaves behind is one more block
// whatever the allocator does with its pages.
std::atomic<std::int64_t> liveBlocks { 0 };
// The bytes those blocks hold, as the allocator sized them: a want kept in a
// container that grows in place is no new block, but it is more bytes.
std::atomic<std::int64_t> liveBytes { 0 };

std::size_t allocatedSize(const void* block)
{
#if defined(__APPLE__)
    return ::malloc_size(block);
#else
    return ::malloc_usable_size(const_cast<void*>(block));
#endif
}

struct LiveAllocations {
    std::int64_t blocks = 0;
    std::int64_t bytes = 0;
};

LiveAllocations liveAllocations() { return { liveBlocks.load(std::memory_order_relaxed), liveBytes.load(std::memory_order_relaxed) }; }

// A resize through the daemon reflows the history and publishes a
// Structural frame, which must not send the client the history again.
void aDaemonResizeSendsNoHistoryBytes()
{
    Daemon d;
    Attached attached = attachedSession(d, 3'000);
    UbiquittyTest::ScreenClient client(24, 80, 10'000);
    greet(d, attached, client);
    CHECK_MSG(client.pane(), "the client was not sent its checkpoint");
    const std::size_t historyBytes = client.historyBytes();
    CHECK_MSG(historyBytes > 0, "premise: the attach sent no backfill");
    const std::uint64_t checkpoints = Screen::connection(d, attached.index)->counters().checkpointsSent;
    for (const auto& [columns, rows] : { std::pair { 100, 30 }, std::pair { 53, 24 }, std::pair { 80, 24 } }) {
        // A pane's size changes at most once per coalesce window
        // (Docs/sync.md §6.10); each step is a geometry of its own.
        std::this_thread::sleep_for(std::chrono::microseconds(Ubiquitty::Protocol::Sync::measuredGeometryPolicy().coalesceMicroseconds));
        CHECK(client.sendViewport(columns, rows));
        Screen::receive(d, attached.index, client.takeStream(Screen::now()));
        Screen::endReadPass(d);
        Screen::publish(d, attached.session, Ubiquitty::Runtime::FrameClass::Structural);
        settle(d, attached.index, client);
        CHECK_MSG(attached.session.term->columns == columns && attached.session.term->rows == rows, "the daemon did not apply the client's viewport");
    }
    CHECK_MSG(Screen::connection(d, attached.index)->counters().checkpointsSent - checkpoints == 3, "each geometry is one checkpoint");
    CHECK_MSG(client.historyBytes() == historyBytes, ("the resizes sent " + std::to_string(client.historyBytes() - historyBytes) + " history bytes").c_str());
    const std::string difference = client.pane() ? UbiquittyTest::firstDisagreement(*attached.session.term, *client.pane()) : std::string("no pane");
    CHECK_MSG(difference.empty(), difference.c_str());
}

// The clear-buffer verb and an ED 3 from the program are each a purge: one
// HistoryReset, and none of the old lines left on the client.
void aClearBufferVerbPurgesTheMirror()
{
    Daemon d;
    Attached attached = attachedSession(d, 600);
    UbiquittyTest::ScreenClient client(24, 80, 10'000);
    greet(d, attached, client);
    CHECK_MSG(cacheOf(client) && cacheOf(client)->lineCount() > 500, "premise: the attach backfilled the history");

    CHECK(client.call(R"({"jsonrpc":"2.0","id":"clear","method":"pane.clear","params":{"pane":1,"command":"buffer"}})"));
    Screen::deliver(d, attached.index, client);
    publish(d, attached, client);
    CHECK_MSG(client.count(FrameKind::HistoryReset) == 1, "the clear-buffer verb sent no HistoryReset");
    CHECK_MSG(cacheOf(client)->lineCount() == 0, "the client kept lines the clear-buffer verb cleared");
    CHECK_MSG(Ubiquitty::Screen::scrollbackCount(client.pane()) == Ubiquitty::Screen::scrollbackCount(attached.session.term.get()), "the client's scrollback is not the host's after the verb");

    for (int line = 0; line < 100; ++line)
        UbiquittyTest::feed(*attached.session.term, "again " + std::to_string(line) + "\r\n");
    publish(d, attached, client);
    CHECK_MSG(cacheOf(client)->lineCount() > 50, "premise: the lines after the verb reached the client");
    UbiquittyTest::feed(*attached.session.term, "\x1b[3J");
    publish(d, attached, client);
    CHECK_MSG(client.count(FrameKind::HistoryReset) == 2, "ED 3 sent no HistoryReset");
    CHECK_MSG(cacheOf(client)->lineCount() == 0, "the client kept lines ED 3 cleared");
    CHECK_MSG(cacheOf(client)->key().epoch == Ubiquitty::Screen::historyAddress(*attached.session.term).epoch, "the client is not in the host's epoch");
}

// Wants flood the daemon: what it keeps and its queued bulk stay flat.
//
// What it keeps is measured as live blocks after each flood settles, at three
// sizes. Retention proportional to the wants shows as a slope of one block or
// more per want between the floods; anything a flood of any size leaves
// behind once — a vector that grew to its working capacity, a cache filled —
// cancels between them. The bound is a hundredth of a block per want, a
// hundred times under the smallest leak there is.
//
// Bytes catch what blocks cannot: a want kept in a container that grows in
// place. The client here shares the process and logs a byte or so per frame
// it receives, in vectors that double, so its own bytes grow in steps. The
// bound is four bytes a want, well above that growth and a quarter of the
// smallest record a want could leave behind (a 16-byte range).
void tenThousandWantsLeaveTheDaemonsAllocationsFlat()
{
    Daemon d;
    Attached attached = attachedSession(d, 3'000);
    UbiquittyTest::ScreenClient client(24, 80, 10'000);
    greet(d, attached, client);
    const std::uint64_t head = Ubiquitty::Screen::historyAddress(*attached.session.term).head;
    const auto flood = [&](int wants) {
        for (int want = 0; want < wants; ++want) {
            // The same three hundred lines every flood: the client in this
            // process holds what it fetched, and its cache is not under test.
            const std::uint64_t to = head - static_cast<std::uint64_t>(want % 100);
            const std::vector<LineRange> ranges { { to - 200, to } };
            CHECK(client.wantHistory(HistoryPriority::Viewport, ranges));
            if (want % 100 == 99) {
                settle(d, attached.index, client);
                CHECK(Screen::connection(d, attached.index)->queuedBulkBytes() <= Ubiquitty::Protocol::Sync::provisionalHistoryPolicy.maxQueuedBulkBytes);
            }
        }
        settle(d, attached.index, client);
        return liveAllocations();
    };
    // The first flood brings every container to its working size.
    (void)flood(1'000);
    const LiveAllocations afterHundred = flood(100);
    const LiveAllocations afterThousand = flood(1'000);
    const LiveAllocations afterTenThousand = flood(10'000);
    const std::string reading = "live blocks/bytes after 100, 1,000 and 10,000 more wants: "
        + std::to_string(afterHundred.blocks) + "/" + std::to_string(afterHundred.bytes) + ", "
        + std::to_string(afterThousand.blocks) + "/" + std::to_string(afterThousand.bytes) + ", "
        + std::to_string(afterTenThousand.blocks) + "/" + std::to_string(afterTenThousand.bytes);
    std::printf("history_plane: %s\n", reading.c_str());
    CHECK_MSG((afterThousand.blocks - afterHundred.blocks) * 100 <= 900, ("900 wants kept a block per hundred or more; " + reading).c_str());
    CHECK_MSG((afterTenThousand.blocks - afterThousand.blocks) * 100 <= 10'000, ("10,000 wants kept a block per hundred or more; " + reading).c_str());
    CHECK_MSG(afterTenThousand.bytes - afterThousand.bytes <= 40'000, ("10,000 wants kept four bytes each or more; " + reading).c_str());
    CHECK_MSG(Screen::connection(d, attached.index)->state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached, "the flood closed the connection");
}

// A metered client is pushed the one viewport above the screen.
void aMeteredClientReceivesOneViewport()
{
    Daemon d;
    Attached attached = attachedSession(d, 3'000);
    UbiquittyTest::ScreenClient client(24, 80, 10'000, Ubiquitty::Protocol::Sync::LinkCost::Metered);
    greet(d, attached, client);
    const std::uint64_t head = Ubiquitty::Screen::historyAddress(*attached.session.term).head;
    CHECK_MSG(cacheOf(client) && cacheOf(client)->lineCount() == 24, "a metered client was pushed more than one viewport");
    CHECK_MSG(cacheOf(client) && cacheOf(client)->gaps({ head - 24, head }).empty(), "a metered client was not pushed the viewport above the screen");
}

// A live daemon carries the history plane over its socket: a want goes up,
// history and a reset come down.
bool overASocket(const char* hostBinary)
{
    char homeTemplate[] = "/tmp/ubiquitty-history-plane.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home)
        return false;
    const std::string macHome = std::string(home) + "/.ubiquitty";
    const std::string sockPath = macHome + "/" + Ubiquitty::Host::socketLeafForBinary(hostBinary);
    ::mkdir(macHome.c_str(), 0700);
    const std::string token = "historytokenhistorytokenhistorytoken";
    {
        std::FILE* file = std::fopen((macHome + "/tokens").c_str(), "w");
        if (!file)
            return false;
        constexpr std::uint8_t scopeObserve = 0x01, scopeDrive = 0x02, scopeNewSession = 0x10;
        std::fprintf(file, "%s %u 0 history\n", Ubiquitty::Crypto::sha256Hex(token).c_str(), static_cast<unsigned>(scopeObserve | scopeDrive | scopeNewSession));
        std::fclose(file);
    }
    const pid_t daemon = ::fork();
    if (daemon < 0)
        return false;
    if (daemon == 0) {
        ::setenv("UBIQUITTY_HOME", macHome.c_str(), 1);
        char* args[] = { const_cast<char*>(hostBinary), const_cast<char*>("--daemon"), const_cast<char*>("--columns"), const_cast<char*>("80"),
            const_cast<char*>("--rows"), const_cast<char*>("24"), nullptr };
        ::execv(hostBinary, args);
        _exit(127);
    }
    // The socket file exists from bind(), before listen(); only an accepted
    // connection says the daemon is up.
    const bool isDaemonUp = Ubiquitty::HostTest::waitForDaemonReady(sockPath);
    bool ok = false;
    do {
        if (!isDaemonUp) {
            std::fprintf(stderr, "history_plane: the daemon did not come up\n");
            break;
        }
        Ubiquitty::HostTest::Connection c;
        c.fileDescriptor = Ubiquitty::HostTest::connectDaemon(sockPath);
        if (c.fileDescriptor < 0) {
            std::fprintf(stderr, "history_plane: connect failed\n");
            break;
        }
        UbiquittyTest::ScreenClient client(24, 80, 10'000, LinkCost::Unmetered, Role::Agent);
        if (!client.start({ .target = AdmissionTarget::Session }, std::vector<std::uint8_t>(token.begin(), token.end()))) {
            std::fprintf(stderr, "history_plane: the canonical session greeting did not start\n");
            break;
        }
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, client, 5'000, [&] { return client.connection().counters().checkpointsApplied > 0; });
        const auto welcome = client.welcome();
        if (client.connection().counters().checkpointsApplied == 0 || !welcome
            || welcome->admission != AttachmentAdmission::Granted || welcome->grantedRole != Role::Agent
            || welcome->selection.target != AdmissionTarget::Session || !welcome->selection.sessionID
            || !welcome->selection.daemonInstanceID || welcome->selection.socketName != Ubiquitty::Host::socketLeafForBinary(hostBinary)) {
            std::fprintf(stderr, "history_plane: no checkpoint; the session never came up\n");
            break;
        }
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, client, 400);
        (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, client, "seq 1 600\r");
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, client, 5'000, [&] { return cacheOf(client) && cacheOf(client)->lineCount() >= 500; });
        if (!cacheOf(client) || cacheOf(client)->lineCount() < 500 || client.historyBytes() == 0) {
            std::fprintf(stderr, "history_plane: the output's history did not reach the client\n");
            break;
        }
        const std::size_t headsBefore = client.count(FrameKind::HistoryHead);
        const std::uint64_t head = cacheOf(client)->head();
        const std::vector<LineRange> viewport { { head - 300, head - 276 } };
        if (!client.wantHistory(HistoryPriority::Viewport, viewport)) {
            std::fprintf(stderr, "history_plane: the client refused its own want\n");
            break;
        }
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, client, 5'000, [&] { return client.count(FrameKind::HistoryHead) > headsBefore; });
        if (client.count(FrameKind::HistoryHead) <= headsBefore) {
            std::fprintf(stderr, "history_plane: the want went unanswered\n");
            break;
        }
        (void)UbiquittyTest::typeOverSocket(c.fileDescriptor, client, "printf '\\033[3J'\r");
        (void)UbiquittyTest::carryScreen(c.fileDescriptor, client, 5'000, [&] { return client.count(FrameKind::HistoryReset) > 0; });
        if (client.count(FrameKind::HistoryReset) == 0 || cacheOf(client)->lineCount() > 30) {
            std::fprintf(stderr, "history_plane: ED 3 did not purge the client\n");
            break;
        }
        ok = true;
    } while (false);
    ::kill(daemon, SIGTERM);
    int status = 0;
    ::waitpid(daemon, &status, 0);
    return ok;
}

} // namespace

// Every replaceable allocation function, so each block counted out is counted
// back: a delete paired with a new this binary did not replace would unbalance
// the count. Plain malloc and free beneath, as the library's own are.
namespace {

void* countedAllocation(std::size_t size)
{
    void* block = std::malloc(size ? size : 1);
    if (block) {
        liveBlocks.fetch_add(1, std::memory_order_relaxed);
        liveBytes.fetch_add(static_cast<std::int64_t>(allocatedSize(block)), std::memory_order_relaxed);
    }
    return block;
}

void* countedAlignedAllocation(std::size_t size, std::align_val_t alignment)
{
    void* block = nullptr;
    const std::size_t boundary = std::max(static_cast<std::size_t>(alignment), sizeof(void*));
    if (::posix_memalign(&block, boundary, size ? size : 1) != 0)
        return nullptr;
    liveBlocks.fetch_add(1, std::memory_order_relaxed);
    liveBytes.fetch_add(static_cast<std::int64_t>(allocatedSize(block)), std::memory_order_relaxed);
    return block;
}

void* requiredAllocation(void* block)
{
    if (!block)
        std::abort();
    return block;
}

void countedRelease(void* block)
{
    if (!block)
        return;
    liveBlocks.fetch_sub(1, std::memory_order_relaxed);
    liveBytes.fetch_sub(static_cast<std::int64_t>(allocatedSize(block)), std::memory_order_relaxed);
    std::free(block);
}

} // namespace

void* operator new(std::size_t size) { return requiredAllocation(countedAllocation(size)); }
void* operator new[](std::size_t size) { return requiredAllocation(countedAllocation(size)); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return countedAllocation(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return countedAllocation(size); }
void* operator new(std::size_t size, std::align_val_t alignment) { return requiredAllocation(countedAlignedAllocation(size, alignment)); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return requiredAllocation(countedAlignedAllocation(size, alignment)); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { return countedAlignedAllocation(size, alignment); }
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { return countedAlignedAllocation(size, alignment); }
void operator delete(void* block) noexcept { countedRelease(block); }
void operator delete[](void* block) noexcept { countedRelease(block); }
void operator delete(void* block, std::size_t) noexcept { countedRelease(block); }
void operator delete[](void* block, std::size_t) noexcept { countedRelease(block); }
void operator delete(void* block, const std::nothrow_t&) noexcept { countedRelease(block); }
void operator delete[](void* block, const std::nothrow_t&) noexcept { countedRelease(block); }
void operator delete(void* block, std::align_val_t) noexcept { countedRelease(block); }
void operator delete[](void* block, std::align_val_t) noexcept { countedRelease(block); }
void operator delete(void* block, std::size_t, std::align_val_t) noexcept { countedRelease(block); }
void operator delete[](void* block, std::size_t, std::align_val_t) noexcept { countedRelease(block); }
void operator delete(void* block, std::align_val_t, const std::nothrow_t&) noexcept { countedRelease(block); }
void operator delete[](void* block, std::align_val_t, const std::nothrow_t&) noexcept { countedRelease(block); }

int main(int argc, char** argv)
{
    aDaemonResizeSendsNoHistoryBytes();
    aClearBufferVerbPurgesTheMirror();
    tenThousandWantsLeaveTheDaemonsAllocationsFlat();
    aMeteredClientReceivesOneViewport();
    if (argc >= 2)
        CHECK_MSG(overASocket(argv[1]), "the socket carried the history plane wrong");
    if (UBIQUITTY_TEST_SUMMARY() == 0)
        std::printf("history_plane: a daemon resize sends no history, a clear purges, wants stay flat, a metered client gets one viewport%s\n", argc >= 2 ? ", and the socket carries it all" : "");
    return UBIQUITTY_TEST_SUMMARY() == 0 ? 0 : 1;
}
