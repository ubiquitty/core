// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_close_on_reap.cpp — reapClient frees a dropped QUIC client's
// connection (+ its unbounded toSend/toSendDatagrams) from the listener table
// IMMEDIATELY, not at the 90 s idle timeout.
//
// When the daemon reaps a QUIC client (overflow / I/O error / decode fault), the
// bound QUICConnection must be dropped from the listener's connection table right
// then — else its unbounded toSend/toSendDatagrams pin ~16 MiB until the 90 s idle
// timer fires. reapClient calls m_quic->closeConnection(quicConnection), which erases
// the owning unique_ptr from m_connections, freeing the connection now.
//
// The existing makeTestQUICConnection() object is not in m_connections (the test owns
// it), so closeConnection would find no match and be a no-op; this test ADOPTS the
// connection into the live table (adoptTestQUICConnection) so the reap-free is
// observable, then:
//   - asserts the connection is in the table before the reap (count == 1);
//   - drives the PRIVATE reapClient;
//   - asserts (a) the table is empty immediately (count == 0 — freed on reap, not at
//     the idle timeout; no serviceTimers() was ever called) and (b) the Client is erased.
//
// Verify-by-revert: delete the `#ifdef UBIQUITTY_QUIC ... closeConnection ...` block
// from reapClient and assertion (a) fails — the connection lingers in m_connections
// (count stays 1) until the 90 s idle timer, the leak the close prevents.
//
// Built only under -DUBIQUITTY_QUIC=ON (closeConnection + m_connections are QUIC-gated).
// Exit 0 on success.
//

#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostQUIC.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace Ubiquitty {
namespace Host {

// Friend seam: reach the private client list, the QUIC listener, and reapClient.
struct QUICCloseOnReapAccess {
    static bool startQUIC(Daemon& d) { return d.startQUIC(0); } // ephemeral listener → m_quic non-null
    static bool haveQUIC(const Daemon& d) { return d.m_quic != nullptr; }

    // Adopt a bare connection into the listener's live table and return the borrowed
    // routing pointer (owned now by m_connections, freed on closeConnection's erase —
    // so the test must not destroyTestQUICConnection it).
    static QUICConnection* adoptConnection(Daemon& d) { return adoptTestQUICConnection(*d.m_quic, makeTestQUICConnection()); }
    static std::size_t liveConnections(const Daemon& d) { return testQUICConnectionCount(*d.m_quic); }

    // A QUIC client bound to `connection`, pre-flagged overflowed (the run-loop reap
    // targets overflowed clients). fileDescriptor stays default-empty (-1) — no
    // real socket for the reap to close.
    static std::size_t addReapableClient(Daemon& d, QUICConnection* connection)
    {
        Daemon::Client c;
        c.quicConnection = connection;
        c.didOverflow = true;
        d.m_clients.push_back(std::move(c));
        return d.m_clients.size() - 1;
    }
    static void reap(Daemon& d, std::size_t index) { d.reapClient(index); }
    static std::size_t clientCount(const Daemon& d) { return d.m_clients.size(); }
};

} // namespace Host
} // namespace Ubiquitty

using Ubiquitty::Host::Daemon;
using Ubiquitty::Host::QUICCloseOnReapAccess;

static int failures = 0;
static void check(bool ok, const char* msg)
{
    if (!ok) {
        std::fprintf(stderr, "quic_close_on_reap: FAIL — %s\n", msg);
        ++failures;
    }
}

int main()
{
    // Isolated control dir so the listener's self-signed cert lands in a temp tree,
    // never a real ~/.ubiquitty.
    char homeTemplate[] = "/tmp/ubiquitty-quic-closereap.XXXXXX";
    char* home = ::mkdtemp(homeTemplate);
    if (!home) {
        std::fprintf(stderr, "quic_close_on_reap: mkdtemp failed\n");
        return 1;
    }
    std::string ubiquittyHome = std::string(home) + "/.ubiquitty";
    ::setenv("UBIQUITTY_HOME", ubiquittyHome.c_str(), 1);

    Daemon d;
    // A real listener so m_quic is non-null (reapClient's close is gated on
    // c.quicConnection && m_quic). No peer handshake — the connection is adopted
    // directly into the table below.
    if (!QUICCloseOnReapAccess::startQUIC(d)) {
        std::fprintf(stderr, "quic_close_on_reap: startQUIC failed (QUIC build/cert env?)\n");
        return 1;
    }
    check(QUICCloseOnReapAccess::haveQUIC(d), "the QUIC listener is live (m_quic non-null)");

    // Adopt a connection into the listener's live table and bind a reap-flagged client.
    Ubiquitty::Host::QUICConnection* connection = QUICCloseOnReapAccess::adoptConnection(d);
    check(connection != nullptr, "a test QUIC connection is adopted into the listener");
    check(QUICCloseOnReapAccess::liveConnections(d) == 1, "the connection is in the listener's live table before the reap");
    std::size_t idx = QUICCloseOnReapAccess::addReapableClient(d, connection);

    // reapClient must free the connection now, from the live table.
    QUICCloseOnReapAccess::reap(d, idx);

    check(QUICCloseOnReapAccess::liveConnections(d) == 0,
        "(a) reapClient FREED the QUIC connection immediately — dropped from the listener "
        "table (with its unbounded toSend/toSendDatagrams), not lingering until the 90 s "
        "idle timeout; no timer was serviced");
    check(QUICCloseOnReapAccess::clientCount(d) == 0, "(b) the reaped Client is erased from the daemon's client list");

    if (failures == 0)
        std::printf("quic_close_on_reap: reapClient frees a dropped QUIC client's connection + its "
                    "unbounded toSend/toSendDatagrams immediately from the listener table, not at the "
                    "90 s idle timeout\n");
    return failures == 0 ? 0 : 1;
}
