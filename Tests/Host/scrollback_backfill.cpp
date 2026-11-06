// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/scrollback_backfill.cpp — handing a client the history it was not
// there for.
//
// A client that attaches to a live session gets the screen in one checkpoint
// and the history above it on the history plane (Docs/sync.md §6.6): an
// unmetered client is backfilled newest first, in transfers of at most 256
// lines, down to the backfill depth. After that the history grows by appends
// that precede the updates that scroll them, and the host keeps writing while
// a client falls behind: the ring evicts lines out from under it, so what a
// client is owed is named by logical line numbers, and a clear purges what
// the client held.
//
// The failure that costs the most is the one no error path reports — a mirror
// that ends up holding the wrong history and says nothing, because every frame
// it received was well-formed. So this drives the real consumer rather than
// counting frames: the daemon's bytes go into the client end of a Sync
// connection holding a replica Terminal, which is exactly what a client does
// with them, and the assertion compares the replica's scrollback against the
// host's, row by row.
//
// The properties that carry it:
//
//   * The newest lines arrive to the backfill depth, in order, however many
//     transfers they take.
//   * A row is never delivered twice: appends a client already holds are not
//     sent again, and lines the ring evicted before they were sent are not
//     sent at all.
//   * A client that keeps fewer lines than the host holds the newest of them.
//   * History that vanishes under a client — a clear — is purged from it.
//
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"
#include "TrustedLocalPeer.h"

#include "Assertions.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "Platform/StreamDouble.h"
#include "TerminalFixture.h"

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "PAL/Stream.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Ubiquitty {
namespace Host {

struct ScrollbackBackfillAccess {
    static Daemon::Session& addSession(Daemon& d, int rows, int columns, int scrollbackLimit)
    {
        Daemon::Session& session = ScreenPlaneAccess::makeSession(d, rows, columns, std::make_unique<TestDoubles::StreamDouble>());
        session.term->scrollbackLimit = scrollbackLimit;
        return session;
    }

    static std::size_t addMirror(Daemon& d, std::uint32_t sessionID)
    {
        const std::size_t index = ScreenPlaneAccess::attach(d, sessionID);
        Ubiquitty::Host::seedTrustedLocalPeer(d.m_clients[index]);
        return index;
    }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;

namespace {

using A = Ubiquitty::Host::ScrollbackBackfillAccess;

// Every history row is its own ordinal, so a duplicate or an out-of-order
// delivery is visible in the text itself rather than only in a count.
void writeHistory(Ubiquitty::Terminal& t, int firstOrdinal, int lines)
{
    for (int i = 0; i < lines; ++i) {
        char row[64];
        std::snprintf(row, sizeof row, "row %06d\r\n", firstOrdinal + i);
        UbiquittyTest::feed(t, row);
    }
}

// The ordinal a delivered row carries, or -1 for a row that is not one of ours.
int ordinalOf(const std::string& line)
{
    if (line.rfind("row ", 0) != 0)
        return -1;
    return std::atoi(line.c_str() + 4);
}

std::vector<std::string> scrollbackRows(const Ubiquitty::Terminal& t)
{
    std::vector<std::string> rows;
    const int count = Ubiquitty::Screen::scrollbackCount(&t);
    rows.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        rows.push_back(Ubiquitty::Screen::absoluteRowText(t, i, t.columns));
    return rows;
}

// Whether a replica's history is the host's, row for row; the first
// divergence is printed.
bool holdsHostHistory(const Ubiquitty::Terminal& host, const Ubiquitty::Terminal* replica, const char* what)
{
    if (!replica) {
        std::fprintf(stderr, "%s: no pane was bound\n", what);
        return false;
    }
    const std::vector<std::string> hostRows = scrollbackRows(host);
    const std::vector<std::string> replicaRows = scrollbackRows(*replica);
    if (hostRows == replicaRows)
        return true;
    std::fprintf(stderr, "%s: host holds %zu rows, the replica %zu\n", what, hostRows.size(), replicaRows.size());
    for (std::size_t i = 0; i < hostRows.size() && i < replicaRows.size(); ++i) {
        if (hostRows[i] != replicaRows[i]) {
            std::fprintf(stderr, "  row %zu\n    host    |%s|\n    replica |%s|\n", i, hostRows[i].c_str(), replicaRows[i].c_str());
            break;
        }
    }
    return false;
}

// Whether a replica holds the newest `depth` rows of the host's history, or all
// of them when the host holds fewer, row for row.
bool holdsNewestHostHistory(const Ubiquitty::Terminal& host, const Ubiquitty::Terminal* replica, std::size_t depth, const char* what)
{
    if (!replica) {
        std::fprintf(stderr, "%s: no pane was bound\n", what);
        return false;
    }
    const std::vector<std::string> hostRows = scrollbackRows(host);
    const std::vector<std::string> replicaRows = scrollbackRows(*replica);
    const std::size_t expected = std::min(depth, hostRows.size());
    if (replicaRows.size() == expected && std::equal(replicaRows.begin(), replicaRows.end(), hostRows.end() - static_cast<std::ptrdiff_t>(expected)))
        return true;
    std::fprintf(stderr, "%s: the replica holds %zu rows, not the host's newest %zu\n", what, replicaRows.size(), expected);
    return false;
}

// Whether every ordinal in a history appears once and in ascending order.
bool isEachRowOnceInOrder(const Ubiquitty::Terminal& t)
{
    int previous = -1;
    for (const std::string& row : scrollbackRows(t)) {
        const int ordinal = ordinalOf(row);
        if (ordinal < 0)
            continue;
        if (ordinal <= previous)
            return false;
        previous = ordinal;
    }
    return true;
}

void publish(Daemon& d, auto& session)
{
    session.isFrameDue = true;
    Screen::publish(d, session);
}

} // namespace

int main()
{
    constexpr int rows = 24;
    constexpr int columns = 80;
    constexpr auto checkpoint = Ubiquitty::Protocol::Sync::FrameKind::CheckpointHead;

    // ---- 1. The newest history reaches the replica to the backfill depth, in
    // order, after one checkpoint: 2,000 of 5,000 rows is eight transfers.
    {
        Daemon d;
        auto& s = A::addSession(d, rows, columns, 6000);
        writeHistory(*s.term, 0, 5000);
        publish(d, s);
        const std::size_t mirror = A::addMirror(d, s.id);
        UbiquittyTest::ScreenClient client(rows, columns, 6000);
        Screen::greet(d, mirror, client);
        publish(d, s);
        Screen::deliver(d, mirror, client);
        CHECK(client.count(checkpoint) == 1);
        CHECK(Ubiquitty::Screen::scrollbackCount(s.term.get()) > 4000);
        CHECK(holdsNewestHostHistory(*s.term, client.pane(), Ubiquitty::Protocol::Sync::defaultBackfillDepthLines, "the attach backfill"));
        CHECK(client.pane() && isEachRowOnceInOrder(*client.pane()));
    }

    // ---- 2. Live history rides the updates as appends, each row once, and a
    // client that stopped reading is sent only the rows it does not hold.
    {
        Daemon d;
        auto& s = A::addSession(d, rows, columns, 2000);
        writeHistory(*s.term, 0, 100);
        const std::size_t mirror = A::addMirror(d, s.id);
        UbiquittyTest::ScreenClient client(rows, columns, 2000);
        Screen::greet(d, mirror, client);
        publish(d, s);
        Screen::deliver(d, mirror, client);
        int ordinal = 100;
        for (int frame = 0; frame < 50; ++frame) {
            writeHistory(*s.term, ordinal, 7);
            ordinal += 7;
            publish(d, s);
            // Every fifth frame the client reads; in between its updates wait.
            if (frame % 5 == 4)
                Screen::deliver(d, mirror, client);
            else
                Screen::service(d);
        }
        Screen::deliver(d, mirror, client);
        CHECK(client.count(checkpoint) == 1);
        CHECK(client.connection().counters().updatesApplied > 0);
        CHECK(holdsHostHistory(*s.term, client.pane(), "live appends"));
        CHECK(client.pane() && isEachRowOnceInOrder(*client.pane()));
    }

    // ---- 3. The ring turns over while the client is behind: the lines it was
    // owed and the host evicted are not sent; the ones the host still holds
    // arrive as appends, with no checkpoint.
    {
        Daemon d;
        auto& s = A::addSession(d, rows, columns, 500);
        writeHistory(*s.term, 0, 200);
        const std::size_t mirror = A::addMirror(d, s.id);
        UbiquittyTest::ScreenClient client(rows, columns, 500);
        Screen::greet(d, mirror, client);
        publish(d, s);
        Screen::deliver(d, mirror, client);
        writeHistory(*s.term, 200, 2000);
        publish(d, s);
        Screen::deliver(d, mirror, client);
        CHECK(client.count(checkpoint) == 1);
        CHECK(holdsHostHistory(*s.term, client.pane(), "a ring that turned over"));
        CHECK(client.pane() && isEachRowOnceInOrder(*client.pane()));
    }

    // ---- 4. A client that keeps fewer lines than the host holds the newest
    // of them, in order.
    {
        Daemon d;
        auto& s = A::addSession(d, rows, columns, 2000);
        writeHistory(*s.term, 0, 1500);
        const std::size_t mirror = A::addMirror(d, s.id);
        UbiquittyTest::ScreenClient client(rows, columns, 300);
        Screen::greet(d, mirror, client);
        publish(d, s);
        Screen::deliver(d, mirror, client);
        writeHistory(*s.term, 1500, 40);
        publish(d, s);
        Screen::deliver(d, mirror, client);
        const std::vector<std::string> hostRows = scrollbackRows(*s.term);
        const std::vector<std::string> replicaRows = client.pane() ? scrollbackRows(*client.pane()) : std::vector<std::string> {};
        CHECK(replicaRows.size() == 300);
        CHECK(replicaRows.size() <= hostRows.size()
            && std::equal(replicaRows.begin(), replicaRows.end(), hostRows.end() - static_cast<std::ptrdiff_t>(replicaRows.size())));
    }

    // ---- 5. A session with no history sends an empty one.
    {
        Daemon d;
        auto& s = A::addSession(d, rows, columns, 2000);
        UbiquittyTest::feed(*s.term, "$ ");
        const std::size_t mirror = A::addMirror(d, s.id);
        UbiquittyTest::ScreenClient client(rows, columns);
        Screen::greet(d, mirror, client);
        publish(d, s);
        Screen::deliver(d, mirror, client);
        CHECK(client.pane() && Ubiquitty::Screen::scrollbackCount(client.pane()) == 0);
    }

    // ---- 6. The history vanishes under the client. A program issuing a full
    // clear (ED 3) while the client is behind leaves the host with none, and
    // the client purges what it held rather than keeping rows the host
    // dropped.
    {
        Daemon d;
        auto& s = A::addSession(d, rows, columns, 2000);
        writeHistory(*s.term, 0, 600);
        const std::size_t mirror = A::addMirror(d, s.id);
        UbiquittyTest::ScreenClient client(rows, columns, 2000);
        Screen::greet(d, mirror, client);
        publish(d, s);
        Screen::deliver(d, mirror, client);
        CHECK(client.pane() && Ubiquitty::Screen::scrollbackCount(client.pane()) > 0);
        UbiquittyTest::feed(*s.term, "\033[3J");
        CHECK(Ubiquitty::Screen::scrollbackCount(s.term.get()) == 0);
        publish(d, s);
        Screen::deliver(d, mirror, client);
        CHECK(holdsHostHistory(*s.term, client.pane(), "a cleared history"));
        // New history after the clear arrives as appends onto the empty ring.
        writeHistory(*s.term, 600, 30);
        publish(d, s);
        Screen::deliver(d, mirror, client);
        CHECK(holdsHostHistory(*s.term, client.pane(), "history after a clear"));
        CHECK(client.pane() && isEachRowOnceInOrder(*client.pane()));
    }

    if (failures == 0)
        std::printf("scrollback_backfill: the newest history reaches a client to the backfill depth, live history precedes the updates once each, "
                    "lines the ring evicted are not sent, a clear purges the client, and a client keeping fewer lines holds the newest\n");
    return failures == 0 ? 0 : 1;
}
