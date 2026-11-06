# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""The canonical Sync envelope and the handful of bodies a Python fixture
writes and reads (Docs/sync-protocol.md): every frame is `kind:u8
channel:varint length:varint body`, from the first byte of the connection.
Frame kinds are read from the header that defines them, so a fixture speaks
the build it tests instead of numbers it retyped."""

import json
import pathlib
import re
import struct

from wire_version import wire_fingerprint, wire_version

_ENVELOPE = pathlib.Path(__file__).resolve().parents[2] / 'Source/Core/Protocol/Sync/Envelope.h'

MAX_FRAME_BODY_BYTES = 65535

# AdmissionTarget, ClientKind and Role as Admission.h, Messages.h and Roles.h
# number them.
TARGET_SESSION = 0
TARGET_WORKSPACE = 1
CLIENT_HUMAN = 0
CLIENT_AGENT = 1
ROLE_OWNER = 0
ROLE_AGENT = 3

# View states (Docs/sync.md §7.4).
VIEW_LIVE = 0


def _frame_kinds():
    """Ubiquitty::Protocol::Sync::FrameKind, name to value, as the source states it."""
    match = re.search(r'enum class FrameKind : std::uint8_t \{(.*?)\n\};', _ENVELOPE.read_text(), re.S)
    if not match:
        raise RuntimeError('FrameKind is not stated in ' + str(_ENVELOPE))
    kinds = {name: int(value) for name, value in re.findall(r'^\s*(\w+) = (\d+),', match.group(1), re.M)}
    if not kinds:
        raise RuntimeError('FrameKind in ' + str(_ENVELOPE) + ' names no kind')
    return kinds


KIND = _frame_kinds()
KNOWN_KINDS = frozenset(KIND.values())


def encode_varint(value):
    """Unsigned LEB128 in its shortest form."""
    out = bytearray()
    while True:
        byte = value & 0x7f
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def read_varint(data, at):
    """The varint at `at` and the offset past it; at most ten bytes."""
    value = 0
    for shift in range(0, 70, 7):
        byte = data[at]
        at += 1
        value |= (byte & 0x7f) << shift
        if not byte & 0x80:
            return value, at
    raise AssertionError('a varint ran past ten bytes')


def encode_bytes(value):
    """A varint length, then the bytes."""
    return encode_varint(len(value)) + value


def frame(kind, channel, body=b''):
    """One canonical envelope: a kind name or value, a channel and the body."""
    if isinstance(kind, str):
        kind = KIND[kind]
    assert len(body) <= MAX_FRAME_BODY_BYTES, 'a frame body past the envelope bound'
    return bytes([kind]) + encode_varint(channel) + encode_varint(len(body)) + body


class FrameReader:
    """Splits a byte stream into complete frames, whatever the read sizes.
    A kind the build does not define or a body past the bound fails the
    fixture, as it would close a conforming client."""

    def __init__(self):
        self.buffer = bytearray()
        self.received = 0

    def feed(self, data):
        """Takes stream bytes and returns every frame they complete, as
        (kind, channel, body)."""
        self.received += len(data)
        self.buffer.extend(data)
        frames = []
        while self.buffer:
            try:
                channel, at = read_varint(self.buffer, 1)
                length, at = read_varint(self.buffer, at)
            except IndexError:
                break
            kind = self.buffer[0]
            assert kind in KNOWN_KINDS, 'the host sent kind %d, which this build does not define' % kind
            assert length <= MAX_FRAME_BODY_BYTES, 'the host sent a body past the envelope bound'
            if len(self.buffer) < at + length:
                break
            frames.append((kind, channel, bytes(self.buffer[at:at + length])))
            del self.buffer[:at + length]
        return frames

    def clear(self):
        self.buffer.clear()
        self.received = 0


def greet(client_instance, rows=24, columns=80, target=TARGET_WORKSPACE, session_id=0, workspace_id=0,
          client_kind=CLIENT_HUMAN, role=ROLE_OWNER, agent_label='', serves_clipboard=False, credential=None):
    """A channel-0 Greet: this build's wire version and no app version, this
    build's fingerprint, the client instance, its kind and requested role, the
    admission request, an optional credential, the base facts (the viewport,
    no pixel sizes, no refresh cap, unmetered, normal power, focused), no
    history kept or backfilled, no capability bit and no resume. A request
    that names a numeric session or workspace attaches only."""
    assert len(client_instance) == 16
    attach_only = bool(session_id or workspace_id)
    body = struct.pack('<H', wire_version()) + encode_varint(0)
    body += wire_fingerprint() + client_instance + bytes([client_kind, role])
    body += struct.pack('<BIIQ', target, session_id, workspace_id, 0)
    body += encode_varint(0) + encode_bytes(agent_label.encode()) + encode_varint(0)
    body += bytes([attach_only, 0, serves_clipboard])
    body += b'\x01' + encode_bytes(credential.encode()) if credential else b'\x00'
    body += struct.pack('<HHHHHHI', rows, columns, 0, 0, 0, 0, 0) + b'\x00\x00\x01'
    body += struct.pack('<II', 0, 0) + encode_varint(0) + b'\x00'
    return frame('Greet', 0, body)


def read_welcome(body):
    """The selection a Welcome names: the granted role, target, session,
    workspace and serving daemon. The app version follows the u16 wire
    version, the only version a Welcome carries."""
    app_version_length, at = read_varint(body, 2)
    at += app_version_length + 32 + 16 * 3
    role, target, session, workspace, daemon = struct.unpack_from('<BBIIQ', body, at)
    return {'role': role, 'target': target, 'session': session, 'workspace': workspace, 'daemon': daemon}


def read_bye(body):
    """A Bye's reason and detail."""
    detail_length, at = read_varint(body, 1)
    return body[0], body[at:at + detail_length].decode(errors='replace')


def read_pane_bind(body):
    """A PaneBind's handle and 16-byte pane ID."""
    handle, at = read_varint(body, 0)
    return handle, body[at:at + 16]


def read_pane_unbind(body):
    """A PaneUnbind's handle and reason."""
    handle, at = read_varint(body, 0)
    return handle, body[at]


def raw_input(handle, seq, data):
    """A Raw input event on a pane's handle: its number, then the bytes."""
    return frame('Raw', handle, struct.pack('<Q', seq) + encode_bytes(data))


def view(handle, state=VIEW_LIVE):
    """A View of the pane bound to a handle."""
    return frame('View', handle, bytes([state]))


def call(request):
    """A Call carrying one JSON-RPC request."""
    return frame('Call', 0, json.dumps(request, separators=(',', ':')).encode())


def read_result(body):
    """The JSON-RPC response an inline Result carries."""
    transfer, at = read_varint(body, 0)
    assert transfer == 0, 'a small result arrived as a bulk transfer'
    return json.loads(body[at:])


def ack(host_time, bytes_received):
    """An Ack of the stream bytes received, naming no pane: it retires bytes
    from the host's window without acknowledging any generation."""
    return frame('Ack', 0, struct.pack('<QIQ', host_time, 0, bytes_received) + encode_varint(0))


def echo(probe_body):
    """The Echo that answers a Probe: its time, held for no time."""
    return frame('Echo', 0, probe_body[:8] + struct.pack('<I', 0))


def clipboard_data(handle, request_id, items):
    """An OK ClipboardData answering a request on its handle, carrying the
    bundle inline: each item a MIME name and its bytes."""
    bundle = encode_varint(len(items))
    for mime, data in items:
        bundle += encode_bytes(mime.encode()) + encode_bytes(data)
    return frame('ClipboardData', handle, struct.pack('<IBII', request_id, 0, 0, len(bundle)) + bundle)


def read_clipboard_request(body):
    """A ClipboardRequest's ID, buffer, kind and MIME name."""
    request_id, buffer, kind = struct.unpack_from('<IBB', body)
    mime_length, at = read_varint(body, 6)
    return request_id, buffer, kind, body[at:at + mime_length].decode()
