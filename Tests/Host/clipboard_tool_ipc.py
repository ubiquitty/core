#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercises real host-owned clipboard helper processes using synthetic clipboard data."""
import argparse
import base64
import os
import pathlib
import select
import shlex
import socket
import struct
import subprocess
import tempfile
import time
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sync_wire  # noqa: E402

LOCAL_PREAMBLE = b'\xf9UBQLOCAL' + bytes(6) + b'\x01'


def local_clipboard_route(correlation):
    """The current F9 op4 route-only request, with no caller identity fields."""
    return LOCAL_PREAMBLE + struct.pack('<BQIBH', 4, correlation, 3, 2, 0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('host_binary')
    parser.add_argument('--no-rcs', action='store_true')
    args = parser.parse_args()
    binary = str(pathlib.Path(args.host_binary).resolve())
    with tempfile.TemporaryDirectory(prefix='ubclip-', dir='/tmp') as temporary:
        root = pathlib.Path(temporary)
        environment = os.environ.copy()
        environment['UBIQUITTY_HOME'] = temporary
        original = root / 'original'
        custom = root / 'custom'
        original.mkdir()
        custom.mkdir()
        startup_log = root / 'startup-order'
        (original / '.zshenv').write_text(f'print env >> {shlex.quote(str(startup_log))}\nexport ZDOTDIR={shlex.quote(str(custom))}\n')
        if args.no_rcs:
            with (original / '.zshenv').open('a') as startup:
                startup.write('unsetopt RCS\n')
        for stage in ('zprofile', 'zshrc', 'zlogin'):
            (custom / ('.' + stage)).write_text(f'print {stage} >> {shlex.quote(str(startup_log))}\n')
        native = root / 'native-tools'
        native.mkdir()
        native_tool = native / 'xclip'
        native_tool.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\nprintf "PATH=%s\\n" "$PATH"\n')
        native_tool.chmod(0o700)
        environment['PATH'] = str(native) + ':' + environment.get('PATH', '/usr/bin:/bin')
        environment['ZDOTDIR'] = str(original)
        environment['SHELL'] = '/bin/zsh' if pathlib.Path('/bin/zsh').exists() else '/bin/sh'
        token = subprocess.check_output([binary, 'token', 'issue', '--scope', 'all', '--expires', '60', '--label', 'clipboard-test'], env=environment, stderr=subprocess.DEVNULL).decode().strip()
        log = open(root / 'daemon.log', 'wb')
        daemon = subprocess.Popen([binary, '--daemon', '--socket', 'host-0000000000000000.sock', '--quic-port', '-1'], env=environment, stdout=log, stderr=log)
        connection = socket.socket(socket.AF_UNIX)
        bridges = []
        reader = sync_wire.FrameReader()
        requests = []
        held_requests = []
        hold_replies = False
        wrong_mime = False
        # The handle the host bound the client's pane to, and the number of
        # the last input event sent on it.
        pane = {'handle': 0, 'seq': 0, 'session': 0}
        is_disconnect_expected = False
        is_disconnected = False
        image = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j3hUAAAAASUVORK5CYII=')
        try:
            deadline = time.monotonic() + 10
            while True:
                sockets = list(root.rglob('host-0000000000000000.sock'))
                if sockets:
                    try:
                        connection.connect(str(sockets[0]))
                        break
                    except OSError:
                        pass
                if time.monotonic() >= deadline:
                    raise AssertionError('daemon did not start: ' + (root / 'daemon.log').read_text())
                time.sleep(.02)
            def bridge():
                parent, child = socket.socketpair()
                process = subprocess.Popen([binary, '--attach', '--socket', sockets[0].name], stdin=child, stdout=child, stderr=log, env=environment)
                child.close()
                bridges.append(process)
                return parent

            # The real attach bridge is the host's authenticated local human
            # connection. A token presented directly by Python is automation
            # and cannot serve a person's clipboard.
            connection.close()
            connection = bridge()
            # A fresh session, served by this client's clipboard.
            connection.sendall(sync_wire.greet(os.urandom(16), target=sync_wire.TARGET_SESSION, serves_clipboard=True, credential=token))

            def reconnect(should_serve=True):
                nonlocal connection, is_disconnect_expected, is_disconnected
                connection.close()
                connection = bridge()
                reader.clear()
                pane['handle'], pane['seq'] = 0, 0
                is_disconnect_expected = is_disconnected = False
                before = requests.count('welcome')
                connection.sendall(sync_wire.greet(os.urandom(16), target=sync_wire.TARGET_SESSION, session_id=pane['session'],
                                                   serves_clipboard=should_serve, credential=token))
                pump(lambda: requests.count('welcome') > before and pane['handle'] != 0)

            def type_text(data):
                # Input travels on the pane's handle, which the host names
                # once it has welcomed the client.
                pump(lambda: pane['handle'] != 0)
                pane['seq'] += 1
                connection.sendall(sync_wire.raw_input(pane['handle'], pane['seq'], data))

            def pump(until, timeout=10):
                nonlocal is_disconnected
                limit = time.monotonic() + timeout
                while time.monotonic() < limit:
                    if until():
                        return
                    if is_disconnected:
                        time.sleep(.01)
                        continue
                    ready, _, _ = select.select([connection], [], [], .05)
                    if not ready:
                        continue
                    try:
                        chunk = connection.recv(65536)
                    except ConnectionResetError:
                        chunk = b''
                    if not chunk:
                        if is_disconnect_expected:
                            is_disconnected = True
                            continue
                        raise AssertionError('daemon disconnected: ' + repr(requests) + '\n' + '\n'.join(path.read_text(errors='replace') for path in root.rglob('*.log')))
                    for kind, channel, body in reader.feed(chunk):
                        if kind == sync_wire.KIND['Welcome']:
                            requests.append('welcome')
                            selection = sync_wire.read_welcome(body)
                            pane['session'] = selection['session']
                        elif kind == sync_wire.KIND['PaneBind'] and not pane['handle']:
                            pane['handle'], _ = sync_wire.read_pane_bind(body)
                        elif kind == sync_wire.KIND['ClipboardRequest']:
                            request_id, _, request_kind, mime = sync_wire.read_clipboard_request(body)
                            requests.append((request_kind, mime))
                            if hold_replies:
                                held_requests.append((channel, request_id))
                            else:
                                payload = b'' if request_kind == 0 else image
                                connection.sendall(sync_wire.clipboard_data(channel, request_id, [('image/jpeg' if wrong_mime else 'image/png', payload)]))
                        elif kind == sync_wire.KIND['Result']:
                            requests.append(('result', sync_wire.read_result(body)))
                        elif kind == sync_wire.KIND['Probe']:
                            connection.sendall(sync_wire.echo(body))
                        else:
                            requests.append(('frame', kind, body[:40]))
                    # The host's window retires stream bytes only on an Ack.
                    # A host that has just dropped the client, as the
                    # rejection steps expect, can refuse it; a reset is the
                    # same drop as an end of stream.
                    try:
                        connection.sendall(sync_wire.ack(0, reader.received))
                    except (BrokenPipeError, ConnectionResetError):
                        if not is_disconnect_expected:
                            raise
                        is_disconnected = True
                raise AssertionError('clipboard helper timeout; log=' + (root / 'daemon.log').read_text())

            pump(lambda: 'welcome' in requests)
            outsider = socket.socket(socket.AF_UNIX)
            outsider.settimeout(2)
            outsider.connect(str(sockets[0]))
            outsider.sendall(local_clipboard_route(1))
            assert outsider.recv(1) == b'', 'unrelated local process obtained a clipboard route'
            outsider.close()
            outsider_setup = subprocess.run([binary, 'clipboard-env', '--socket', str(sockets[0])], env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
            assert outsider_setup.returncode != 0 and not outsider_setup.stdout, 'unrelated process received session setup assignments'
            image_path = root / 'received.png'
            types_path = root / 'types.txt'
            tools_path = root / 'tools.txt'
            done = root / 'done'
            command = f'command -v pbpaste > {shlex.quote(str(tools_path))}; wl-paste --list-types > {shlex.quote(str(types_path))}; xclip -selection clipboard -o -t image/png > {shlex.quote(str(image_path))}; touch {shlex.quote(str(done))}\n'
            type_text(command.encode())
            pump(done.exists)
            assert image_path.read_bytes() == image, 'PNG bytes were changed: requests=' + repr(requests) + '\n' + (root / 'daemon.log').read_text()
            assert types_path.read_text() == 'image/png\n', 'type probe differs: ' + repr(types_path.read_bytes()) + ' requests=' + repr(requests)
            assert 'clipboard-tools' in tools_path.read_text(), 'login shell selected native pbpaste instead of helper: ' + tools_path.read_text()
            if pathlib.Path('/bin/zsh').exists():
                assert startup_log.read_text().splitlines() == (['env'] if args.no_rcs else ['env', 'zprofile', 'zshrc', 'zlogin']), 'user startup files did not run exactly once in order'
            png_done = root / 'png-done'
            png_stdout = root / 'png-stdout.png'
            png_file = root / 'png-file.png'
            png_base64 = root / 'png-base64'
            png_file.write_bytes(b'old file')
            png_file.chmod(0o640)
            type_text(f'pngpaste - > {shlex.quote(str(png_stdout))}; pngpaste {shlex.quote(str(png_file))}; pngpaste -b > {shlex.quote(str(png_base64))}; touch {shlex.quote(str(png_done))}\n'.encode())
            pump(png_done.exists)
            assert png_stdout.read_bytes() == image and png_file.read_bytes() == image, 'pngpaste changed PNG bytes'
            assert png_base64.read_bytes() == base64.b64encode(image), 'pngpaste base64 differs or contains newline'
            assert png_file.stat().st_mode & 0o777 == 0o640, 'pngpaste failed to preserve destination mode'
            before = sum(isinstance(item, tuple) and isinstance(item[0], int) for item in requests)
            no_read_done = root / 'no-read-done'
            type_text(f'xclip -selection clipboard -i </dev/null; touch {shlex.quote(str(no_read_done))}\n'.encode())
            pump(no_read_done.exists)
            assert sum(isinstance(item, tuple) and isinstance(item[0], int) for item in requests) == before, 'write refusal read the clipboard'
            setup_done = root / 'setup-done'
            setup_image = root / 'setup-image.png'
            setup = f'unset UBIQUITTY_CLIPBOARD_SOCKET UBIQUITTY_CLIPBOARD_TOOLS UBIQUITTY_CLIPBOARD_NATIVE_PATH; export PATH={shlex.quote(environment["PATH"])}; eval "$({shlex.quote(binary)} clipboard-env --socket {shlex.quote(str(sockets[0]))})"; xclip -selection clipboard -o -t image/png > {shlex.quote(str(setup_image))}; touch {shlex.quote(str(setup_done))}\n'
            # A completed command's marker can precede the shell returning to
            # noncanonical input. Source the long setup without typing it into
            # the terminal's bounded canonical line buffer.
            setup_script = root / 'setup.sh'
            setup_script.write_text(setup)
            type_text(f'. {shlex.quote(str(setup_script))}\n'.encode())
            pump(setup_done.exists)
            assert setup_image.read_bytes() == image, 'explicit clipboard-env did not recover a shell without helper environment'
            wrong_mime = True
            is_disconnect_expected = True
            mismatch_done = root / 'mismatch-done'
            mismatch_bytes = root / 'mismatch.png'
            type_text(f'xclip -selection clipboard -o -t image/png > {shlex.quote(str(mismatch_bytes))}; echo $? > {shlex.quote(str(mismatch_done))}\n'.encode())
            pump(mismatch_done.exists)
            assert mismatch_done.read_text().strip() != '0' and mismatch_bytes.read_bytes() == b'', 'wrong MIME bytes escaped helper validation'
            wrong_mime = False
            reconnect()
            hold_replies = True
            revoked_done = root / 'revoked-done'
            revoked_bytes = root / 'revoked.png'
            type_text(f'xclip -selection clipboard -o -t image/png > {shlex.quote(str(revoked_bytes))}; echo $? > {shlex.quote(str(revoked_done))}\n'.encode())
            pump(lambda: bool(held_requests))
            revoke = {'jsonrpc': '2.0', 'id': 'revoke-clipboard', 'method': 'client.clipboardServing', 'params': {'enabled': False}}
            connection.sendall(sync_wire.call(revoke))
            pump(lambda: any(isinstance(item, tuple) and item[0] == 'result' and item[1].get('id') == revoke['id'] for item in requests))
            accepted = next(item[1] for item in requests if isinstance(item, tuple) and item[0] == 'result' and item[1].get('id') == revoke['id'])
            assert accepted.get('result') == {'enabled': False}, 'clipboard revocation was not accepted: ' + repr(accepted)
            is_disconnect_expected = True
            for handle, request_id in held_requests:
                connection.sendall(sync_wire.clipboard_data(handle, request_id, [('image/png', image)]))
            pump(revoked_done.exists)
            assert revoked_done.read_text().strip() != '0' and revoked_bytes.read_bytes() == b'', 'revoked clipboard bytes reached helper'
            reconnect(should_serve=False)
            fallback_done = root / 'fallback-done'
            fallback_result = root / 'fallback-result'
            type_text(f'xclip -selection primary -custom-native-flag > {shlex.quote(str(fallback_result))}; echo $? > {shlex.quote(str(fallback_done))}\n'.encode())
            pump(fallback_done.exists)
            fallback = fallback_result.read_text().splitlines()
            assert fallback_done.read_text().strip() == '0' and fallback[:3] == ['-selection', 'primary', '-custom-native-flag'], 'host fallback changed native options'
            assert 'clipboard-tools' not in fallback[-1], 'native fallback inherited recursive shim PATH'
            print('PASS: pngpaste stdout/file/base64, clipboard-env recovery, native fallback, login startup order and PATH, MIME probe, exact PNG helper subprocess, outsider rejection, write refusal without reads, wrong MIME rejection, revoked late reply rejection')
        finally:
            try:
                if pane['handle']:
                    pane['seq'] += 1
                    connection.sendall(sync_wire.raw_input(pane['handle'], pane['seq'], b'exit\n'))
                daemon.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                daemon.terminate()
                daemon.wait(timeout=3)
            connection.close()
            for process in bridges:
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=3)
            log.close()


if __name__ == '__main__':
    main()
