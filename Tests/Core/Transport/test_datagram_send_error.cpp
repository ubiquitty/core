// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_datagram_send_error.cpp — what a QUIC carrier does with a datagram its
// socket refused.
//
// A connection that migrates across networks sees the old route vanish for a
// moment during the hop. Ending the connection on that refusal tears down the
// session that migration exists to keep; losing the one packet costs nothing,
// because QUIC resends what it carried.

#include "Core/Transport/QUICClient.h"

#include <cerrno>

using namespace Ubiquitty::Transport;

// ---- A full socket holds the packet ----
TEST(test_datagram_send_error_full_socket_holds)
{
    CHECK_MSG(datagramSendErrorAction(EAGAIN) == DatagramSendErrorAction::Hold, "EAGAIN holds");
    CHECK_MSG(datagramSendErrorAction(EWOULDBLOCK) == DatagramSendErrorAction::Hold, "EWOULDBLOCK holds");
    CHECK_MSG(datagramSendErrorAction(ENOBUFS) == DatagramSendErrorAction::Hold, "ENOBUFS holds");
}

// ---- A route that vanished during a hop loses the packet, not the connection ----
TEST(test_datagram_send_error_a_moving_path_drops_the_packet)
{
    CHECK_MSG(datagramSendErrorAction(ENETUNREACH) == DatagramSendErrorAction::Drop, "ENETUNREACH drops the packet");
    CHECK_MSG(datagramSendErrorAction(EHOSTUNREACH) == DatagramSendErrorAction::Drop, "EHOSTUNREACH drops the packet");
    CHECK_MSG(datagramSendErrorAction(EADDRNOTAVAIL) == DatagramSendErrorAction::Drop, "EADDRNOTAVAIL drops the packet");
    CHECK_MSG(datagramSendErrorAction(ENETDOWN) == DatagramSendErrorAction::Drop, "ENETDOWN drops the packet");
    CHECK_MSG(datagramSendErrorAction(EPERM) == DatagramSendErrorAction::Drop, "EPERM drops the packet");
}

// ---- Anything else ends the connection ----
TEST(test_datagram_send_error_anything_else_closes)
{
    CHECK_MSG(datagramSendErrorAction(EBADF) == DatagramSendErrorAction::Close, "EBADF closes");
    CHECK_MSG(datagramSendErrorAction(EINVAL) == DatagramSendErrorAction::Close, "EINVAL closes");
    CHECK_MSG(datagramSendErrorAction(EMSGSIZE) == DatagramSendErrorAction::Close, "EMSGSIZE closes");
}

UBIQUITTY_TEST_MAIN()
