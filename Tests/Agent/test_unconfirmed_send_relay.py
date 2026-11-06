#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Checks connection retirement and the unconfirmed-input relay's stop point."""

import selectors
import socket
import unittest
from unittest.mock import patch

from unconfirmed_send_relay import Relay


class RelayTests(unittest.TestCase):
    def setUp(self):
        self.selector = selectors.DefaultSelector()
        self.stops = []
        self.relay = Relay(self.selector, b"MARKER", lambda: self.stops.append(True))
        self.endpoints = []

    def tearDown(self):
        self.relay.close()
        self.selector.close()
        for endpoint in self.endpoints:
            endpoint.close()

    def pair(self):
        client, incoming = socket.socketpair()
        daemon, outgoing = socket.socketpair()
        self.endpoints.extend((client, daemon))
        client.settimeout(1)
        daemon.settimeout(1)
        self.relay.add_pair(incoming, outgoing)
        return client, incoming, daemon, outgoing

    def assert_other_pair_works(self, pair):
        client, incoming, daemon, outgoing = pair
        client.sendall(b"request")
        self.assertFalse(self.relay.transfer(incoming))
        self.assertEqual(daemon.recv(7), b"request")
        daemon.sendall(b"reply")
        self.assertFalse(self.relay.transfer(outgoing))
        self.assertEqual(client.recv(5), b"reply")

    def test_closed_client_does_not_drop_another_attach(self):
        client, incoming, daemon, outgoing = self.pair()
        other = self.pair()
        daemon.sendall(b"final control reply")
        client.close()
        self.assertFalse(self.relay.transfer(outgoing))
        self.assertNotIn(incoming, self.relay.peers)
        self.assertNotIn(incoming, self.relay.seen)
        self.assertFalse(self.relay.transfer(incoming))
        self.assert_other_pair_works(other)

    def test_reset_read_does_not_drop_another_attach(self):
        _, incoming, _, outgoing = self.pair()
        other = self.pair()
        with patch.object(socket.socket, "recv", side_effect=ConnectionResetError()):
            self.assertFalse(self.relay.transfer(incoming))
        self.assertNotIn(outgoing, self.relay.peers)
        self.assertNotIn(incoming, self.relay.seen)
        self.assertFalse(self.relay.transfer(outgoing))
        self.assert_other_pair_works(other)

    def test_eof_retires_both_events_in_one_batch(self):
        client, incoming, _, outgoing = self.pair()
        other = self.pair()
        client.close()
        self.assertFalse(self.relay.transfer(incoming))
        self.assertFalse(self.relay.transfer(outgoing))
        self.assert_other_pair_works(other)

    def test_split_marker_stops_before_forwarding_and_only_once(self):
        client, incoming, daemon, _ = self.pair()
        client.sendall(b"MAR")
        self.relay.transfer(incoming)
        self.assertEqual(daemon.recv(3), b"MAR")
        self.assertEqual(self.stops, [])

        def freeze():
            daemon.setblocking(False)
            with self.assertRaises(BlockingIOError):
                daemon.recv(1)
            daemon.settimeout(1)
            self.stops.append(True)

        self.relay.freeze = freeze
        client.sendall(b"KER")
        self.assertFalse(self.relay.transfer(incoming))
        self.assertEqual(self.stops, [True])
        self.assertEqual(daemon.recv(3), b"KER")
        client.sendall(b"MARKER")
        self.assertFalse(self.relay.transfer(incoming))
        self.assertEqual(daemon.recv(6), b"MARKER")
        self.assertEqual(self.stops, [True])
        client.close()
        self.assertTrue(self.relay.transfer(incoming))

    def test_unrelated_io_errors_are_not_hidden(self):
        _, incoming, _, _ = self.pair()
        with patch.object(socket.socket, "recv", side_effect=PermissionError()):
            with self.assertRaises(PermissionError):
                self.relay.transfer(incoming)


if __name__ == "__main__":
    unittest.main()
