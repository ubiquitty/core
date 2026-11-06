#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercises workspace multiplexing, isolation, lifecycle, and synthetic image clipboard routing against a private host."""

import base64
import os
import pathlib
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sync_wire  # noqa: E402

KIND = sync_wire.KIND
# Pane-channel kinds a client sends about one pane (Docs/sync.md §7.6):
# whether its view of the pane holds the focus, and the size it shows it at.
FOCUS = KIND['Focus']
VIEWPORT = KIND['Viewport']


class Wire:
    """Reads the Sync grammar's little-endian fields, varints and
    length-prefixed texts."""

    def __init__(self, b):
        self.b = b
        self.i = 0

    def get(self, f):
        v = struct.unpack_from('<' + f, self.b, self.i)
        self.i += struct.calcsize('<' + f)
        return v[0]

    def varint(self):
        value, self.i = sync_wire.read_varint(self.b, self.i)
        return value

    def text(self):
        n = self.varint()
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def tags(self):
        for _ in range(self.varint()):
            self.text()


def structure(body):
    """A WorkspaceStructure frame's body as (workspace id, revision, panes),
    each pane (window, tab, pane, session, pane ID); panes is None for a
    workspace withdrawn."""
    r = Wire(body)
    wid = r.get('I')
    revision = r.get('Q')
    gone = r.get('B')
    assert r.varint() == 0, 'a workspace this small has its record in the frame'
    if gone:
        return (wid, revision, None)
    # The record: its revision, state and flags, the workspace's id, name,
    # label and tags, then each window.
    r.get('Q')
    r.get('B')
    r.get('B')
    assert r.get('I') == wid
    r.text()
    r.text()
    r.tags()
    out = []

    def split(window, tab):
        if r.get('B') == 0:
            # A leaf: the pane as the screen plane names it, its id in the
            # layout and its session, its cells and place, its generation,
            # its labels and tags, its session's label and tags, and who is
            # attached.
            pane_id = r.b[r.i:r.i + 16]
            r.i += 16
            pane = r.get('I')
            session = r.get('I')
            r.i += 8
            r.get('Q')
            r.text()
            r.tags()
            r.text()
            r.tags()
            attendees = r.varint()
            r.i += 5 * attendees
            out.append((window, tab, pane, session, pane_id))
            return
        # A split: its axis and ratio, its cells and place, its children.
        r.get('B')
        r.get('Q')
        r.i += 8
        for _ in range(r.varint()):
            split(window, tab)
    windows = r.varint()
    r.varint()
    for _ in range(windows):
        window = r.get('I')
        r.text()
        r.tags()
        zoomed = r.get('B')
        tabs = r.varint()
        r.varint()
        if zoomed:
            r.varint()
            r.varint()
        for _ in range(tabs):
            tab = r.get('I')
            r.text()
            r.tags()
            r.text()
            r.varint()
            r.varint()
            split(window, tab)
    assert r.i == len(body), 'the structure record was not read to its end'
    return (wid, revision, out)


def read_frames(process, reader, until, timeout):
    """Reads a peer's frames until `until(kind, channel, body)` holds; False
    when the peer closes first."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if not select.select([process.stdout], [], [], 0.05)[0]:
            continue
        chunk = os.read(process.stdout.fileno(), 65536)
        if not chunk:
            return False
        for kind, channel, body in reader.feed(chunk):
            if until(kind, channel, body):
                return True
    raise AssertionError('the peer did not answer')


def run(binary):
    with tempfile.TemporaryDirectory(prefix='ubmux-', dir='/tmp') as temp:
        env = os.environ.copy()
        env.update(UBIQUITTY_HOME=temp, ZDOTDIR=temp, SHELL='/bin/sh')
        name = 'host-0000000000000000.sock'
        log = open(temp + '/daemon.log', 'wb')
        daemon = subprocess.Popen([binary, '--daemon', '--socket', name, '--quic-port', '-1'], env=env, stdout=log, stderr=log)
        peers = []
        try:
            deadline = time.monotonic() + 10
            while not list(pathlib.Path(temp).rglob(name)):
                assert time.monotonic() < deadline
                time.sleep(0.02)

            def daemon_log():
                return pathlib.Path(temp + '/daemon.log').read_text()

            def connect(workspace=0, session=0):
                """A person's workspace attachment through the attach bridge:
                a fresh workspace, or the one named, at the session named."""
                p = subprocess.Popen([binary, '--attach', '--socket', name], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
                peers.append(p)
                p.stdin.write(sync_wire.greet(os.urandom(16), target=sync_wire.TARGET_WORKSPACE, session_id=session, workspace_id=workspace))
                p.stdin.flush()
                return p
            p = connect()
            reader = sync_wire.FrameReader()
            events = []
            current = []
            # The workspace attached and its structure's newest revision.
            wid = 0
            revision = 0
            # Each bound handle's session, and the JSON-RPC answer to each call.
            handles = {}
            results = {}
            clipboard_origins = []
            image = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j3hUAAAAASUVORK5CYII=')

            def send(data):
                p.stdin.write(data)
                p.stdin.flush()

            def handle_for(session):
                return next((h for h, s in handles.items() if s == session))

            # The number of the last input event sent on each handle of the
            # carrier in use.
            input_seqs = {}
            # Every session a structure named and a PaneBind then bound.
            known_sessions = set()

            def next_input_seq(handle):
                input_seqs[handle] = input_seqs.get(handle, 0) + 1
                return input_seqs[handle]

            def type_pane(session, data):
                # The structure names a pane before its PaneBind arrives, so
                # input for a pane just minted waits for its handle.
                pump(lambda: session in handles.values())
                handle = handle_for(session)
                send(sync_wire.raw_input(handle, next_input_seq(handle), data))

            def ended():
                return any(e[0] in ('bye', 'eof') for e in events)

            def pump(predicate, timeout=15):
                nonlocal wid, revision, current
                end = time.monotonic() + timeout
                while time.monotonic() < end:
                    if predicate():
                        return
                    if ended() or not select.select([p.stdout], [], [], 0.05)[0]:
                        continue
                    data = os.read(p.stdout.fileno(), 65536)
                    if not data:
                        events.append(('eof',))
                        continue
                    for kind, channel, body in reader.feed(data):
                        if kind == KIND['WorkspaceStructure']:
                            named, named_revision, panes = structure(body)
                            if panes is not None:
                                wid, revision, current = named, named_revision, panes
                            events.append(('structure',))
                        elif kind == KIND['PaneBind']:
                            handle, pane_id = sync_wire.read_pane_bind(body)
                            leaf = next((x for x in current if x[4] == pane_id), None)
                            assert leaf, 'a pane was bound before the structure that names it'
                            handles[handle] = leaf[3]
                            known_sessions.add(leaf[3])
                            send(sync_wire.view(handle))
                            events.append(('bind', handle))
                        elif kind == KIND['PaneUnbind']:
                            handle, reason = sync_wire.read_pane_unbind(body)
                            events.append(('unbind', handles.get(handle), reason))
                        elif kind == KIND['Result']:
                            response = sync_wire.read_result(body)
                            results[response.get('id')] = response
                        elif kind == KIND['Bye']:
                            events.append(('bye',) + sync_wire.read_bye(body))
                        elif kind == KIND['Probe']:
                            send(sync_wire.echo(body))
                        elif channel:
                            # A reader that mirrors the workspace holds
                            # nothing for a pane it was not told: the
                            # structure naming the pane came first, or the
                            # pane is one a later structure retired.
                            sid = handles[channel]
                            assert sid in known_sessions, 'a pane frame arrived before the structure that names its pane'
                            events.append((kind, channel, sid))
                            if kind == KIND['ClipboardRequest']:
                                clipboard_origins.append(sid)
                                request_id, _, request_kind, _ = sync_wire.read_clipboard_request(body)
                                payload = b'' if request_kind == 0 else image
                                send(sync_wire.clipboard_data(channel, request_id, [('image/png', payload)]))
                        else:
                            events.append((kind, channel))
                    # The host's window retires stream bytes only on an Ack.
                    if not ended():
                        send(sync_wire.ack(0, reader.received))
                raise AssertionError('timeout ' + repr(events[-8:]) + ' ' + daemon_log())

            def kind_seen(kind, session=None, since=0):
                return any(e[0] == KIND[kind] and (session is None or e[2] == session) for e in events[since:])
            pump(lambda: len(current) == 1 and kind_seen('CheckpointHead'))
            root = current[0]
            rootid = root[3]
            assert events.index(('structure',)) < next(i for i, e in enumerate(events) if e[0] == 'bind')
            foreign = connect()
            foreign_reader = sync_wire.FrameReader()
            foreign_layouts = []

            def capture_foreign(kind, channel, body):
                if kind != KIND['WorkspaceStructure']:
                    return False
                named = structure(body)
                if named[2] is None:
                    return False
                foreign_layouts.append(named)
                return True
            assert read_frames(foreign, foreign_reader, capture_foreign, 10)
            foreign_panes = foreign_layouts[0][2]
            forbidden = temp + '/foreign-write'
            # A pane is addressed by a handle its own carrier was given, so a
            # foreign carrier can name the root pane by none; a handle it was
            # never given ends its connection.
            foreign.stdin.write(sync_wire.raw_input(99, 1, ('touch ' + forbidden + '\n').encode()))
            foreign.stdin.flush()
            assert read_frames(foreign, foreign_reader, lambda kind, channel, body: kind == KIND['Bye'], 10), 'a handle never given did not end the foreign carrier'
            assert not pathlib.Path(forbidden).exists(), 'foreign pane input ran'
            token = subprocess.check_output([binary, 'token', 'issue', '--scope', 'all', '--expires', '60', '--label', 'mux-scope-test'], env=env, stderr=log).decode().strip()
            agent = socket.socket(socket.AF_UNIX)
            agent.settimeout(3)
            agent.connect(str(next(pathlib.Path(temp).rglob(name))))
            agent.sendall(sync_wire.greet(os.urandom(16), target=sync_wire.TARGET_WORKSPACE, session_id=rootid, workspace_id=wid,
                                          client_kind=sync_wire.CLIENT_AGENT, role=sync_wire.ROLE_AGENT, agent_label='agent', credential=token))
            agent_frames = []
            agent_reader = sync_wire.FrameReader()
            while not agent_frames:
                chunk = agent.recv(65536)
                if not chunk:
                    break
                agent_frames = agent_reader.feed(chunk)
            agent.close()
            assert agent_frames and agent_frames[0][0] == KIND['Bye'], 'agent scope expanded or leaked topology'
            # Kind 2 is Welcome, which only a host sends: a client that sends
            # one is refused and closed, and what it carried runs nowhere.
            welcome_write = temp + '/client-welcome-write'
            rejected = connect(wid, rootid)
            rejected.stdin.write(sync_wire.frame('Welcome', 0, ('touch ' + welcome_write + '\n').encode()))
            rejected.stdin.flush()
            assert read_frames(rejected, sync_wire.FrameReader(), lambda kind, channel, body: kind == KIND['Bye'], 5), 'a client-sent Welcome was not refused'
            assert not pathlib.Path(welcome_write).exists(), 'a client-sent Welcome ran its bytes'
            mismatched = connect(wid, foreign_panes[0][3])
            leaked = []

            def refused(kind, channel, body):
                if kind in (KIND['PaneBind'], KIND['WorkspaceStructure'], KIND['CheckpointHead'], KIND['Update']):
                    leaked.append(kind)
                return kind == KIND['Bye']
            assert read_frames(mismatched, sync_wire.FrameReader(), refused, 5), 'mixed session/workspace Greet identity accepted'
            assert not leaked, 'mismatched Greet identity leaked pane data'
            request = 0

            def call(method, params):
                nonlocal request
                request += 1
                identifier = 'mux-' + str(request)
                send(sync_wire.call({'jsonrpc': '2.0', 'id': identifier, 'method': method, 'params': params}))
                return identifier

            def verb(method, params):
                identifier = call(method, params)
                return lambda: identifier in results and 'result' in results[identifier]
            served = verb('workspace.split', {'pane': current[0][2], 'orientation': 'vertical', 'ratio': 0.5})
            pump(lambda: len(current) == 2 and served())
            sibling = next((x for x in current if x[3] != rootid))
            for item in [root, sibling]:
                path = temp + '/pane-' + str(item[3])
                type_pane(item[3], ('echo ready > ' + path + '\n').encode())
            pump(lambda: all((pathlib.Path(temp + '/pane-' + str(x[3])).exists() for x in [root, sibling])))
            # A pane's initial checkpoint may contain all output written before
            # it is captured. Exercise updates only after each pane has its
            # first picture, so a checkpoint cannot satisfy the output instead.
            pump(lambda: all(kind_seen('CheckpointHead', item[3]) for item in [root, sibling]))
            history_start = len(events)
            for item in [root, sibling]:
                type_pane(item[3], ('i=1; while [ $i -le 80 ]; do echo pane_' + str(item[3]) + '_$i; i=$((i + 1)); done\n').encode())
            pump(lambda: all(kind_seen('Update', item[3], history_start) for item in [root, sibling]))
            serving = call('client.clipboardServing', {'enabled': True})
            pump(lambda: serving in results)
            assert results[serving].get('result') == {'enabled': True}, 'clipboard serving was not accepted: ' + repr(results[serving])
            clipboard_path = temp + '/received.png'
            type_pane(sibling[3], ('pngpaste - > ' + clipboard_path + '; touch ' + temp + '/clipboard-done\n').encode())
            pump(lambda: pathlib.Path(temp + '/clipboard-done').exists())
            assert pathlib.Path(clipboard_path).read_bytes() == image, 'image clipboard bytes changed'
            assert clipboard_origins and set(clipboard_origins) == {sibling[3]}, 'clipboard request targeted wrong pane'
            served = verb('workspace.addTab', {'window': current[0][0]})
            pump(lambda: len(current) == 3 and served())
            # A tree change names the revision it was confirmed against, and
            # the structure can move on before the call arrives: a new tab's
            # shell names itself. A refusal for a moved structure asks for the
            # current one, as a client does: the call is confirmed again
            # against the newest structure read, once that has moved on.
            def confirmed(method, params):
                asked = revision
                identifier = call(method, dict(params(), expectedRevision=str(asked)))
                def answered():
                    nonlocal asked, identifier
                    answer = results.get(identifier)
                    if answer is None:
                        return False
                    if 'result' in answer:
                        return True
                    error = answer.get('error', {})
                    assert 'structure changed' in error.get('message', ''), method + ' was refused: ' + repr(answer)
                    if revision != asked:
                        asked = revision
                        identifier = call(method, dict(params(), expectedRevision=str(asked)))
                    return False
                return answered
            served = confirmed('workspace.addWindow', lambda: {'window': current[0][0], 'pane': current[0][2]})
            pump(lambda: served() and len(current) == 4)
            second_surface = next(item for item in current if item[0] != root[0])
            served = verb('workspace.addTab', {'window': second_surface[0]})
            pump(lambda: len(current) == 5 and served())
            for surface_id in {item[0] for item in current}:
                assert len({item[1] for item in current if item[0] == surface_id}) == 2, 'each surface must contain two tabs'
            request += 1
            close_root = sync_wire.call({'jsonrpc': '2.0', 'id': 'mux-' + str(request), 'method': 'workspace.closePane', 'params': {'pane': root[2]}})
            retired_write = temp + '/retired-pane-input'
            root_handle = handle_for(rootid)
            late_focus = sync_wire.frame(FOCUS, root_handle, b'\x00')
            late_resize = sync_wire.frame(VIEWPORT, root_handle, struct.pack('<8H', 30, 100, 0, 0, 0, 0, 0, 0))
            late_input = sync_wire.raw_input(root_handle, next_input_seq(root_handle), ('touch ' + retired_write + '\n').encode())
            send(close_root + late_focus + late_resize + late_input)
            pump(lambda: len(current) == 4 and any(e[:2] == ('unbind', rootid) for e in events))
            assert not ended(), 'root exit ended carrier'
            survivor_write = temp + '/survivor-after-retired-input'
            type_pane(sibling[3], ('touch ' + survivor_write + '\n').encode())
            pump(lambda: pathlib.Path(survivor_write).exists())
            assert not ended(), 'in-flight retired-pane input revoked the workspace carrier'
            assert not pathlib.Path(retired_write).exists(), 'retired-pane input reached a live shell'
            stable = wid
            survivors = {item[3] for item in current}
            p.stdin.close()
            p.wait(timeout=5)
            p = connect(stable)
            reader.clear()
            events.clear()
            handles.clear()
            input_seqs.clear()
            known_sessions.clear()
            current = []
            pump(lambda: len(current) == 4 and kind_seen('CheckpointHead'))
            assert wid == stable and {item[3] for item in current} == survivors, 'the reconnect did not find the workspace it left'
            for item in list(current):
                call('workspace.closePane', {'pane': item[2]})
            pump(ended)
            print('PASS: one carrier across panes/tabs/windows, per-pane input, root exit, stable reconnect, final close, origin-pane PNG clipboard')
        finally:
            for peer in peers:
                if peer.poll() is None:
                    peer.terminate()
                try:
                    peer.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    peer.kill()
                    peer.wait()
            if daemon.poll() is None:
                daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
            log.close()


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: workspace_mux.py /path/to/ubiquitty-host')
    run(sys.argv[1])
