#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""bench-transport — what each remote carrier costs and what a person feels.

Headless and on loopback: no app window, no focus, a private daemon on its own
socket and a private sshd on its own port, every process started here stopped
by its own pid. Builds the Release daemon and Tests/Performance/transport_bench.cpp
against the tree being measured, then runs:

  raw       A SYNTHETIC CEILING, not product behaviour: one-way bulk bytes over
            each bare carrier (QUICClient -> QUICListener, LocalSocketChannel,
            a SubprocessChannel pipe, the same pipe through ssh). The product
            never streams pane output; Sync sends coalesced screen state.
  datagram  QUIC DATAGRAM frames, listener to client.
  e2e       A daemon pane draining 100 MiB (a mixed ASCII/Unicode/SGR corpus,
            and a `yes`-style line flood) to a RemoteSessionCore attached over
            the local bridge, ssh and QUIC: pane drained, mirror converged
            (checked against a fresh checkpoint), wire bytes, CPU.
  echo      Keystroke-to-echo over each carrier, idle and while the pane floods.
  impaired  e2e and echo again at 50 ms RTT with 1% loss, and at 20 Mbit/s
            with 50 ms RTT, through user-space shapers (the kernel's need root).
  memory    The daemon's footprint with 1, 2, 5 and 10 clients on one pane,
            idle and flooding, then with one of ten clients stopped.
  profile   `sample` of the daemon and the client during the e2e flood.

Usage:
  Scripts/bench-transport.py                       # everything, this tree
  Scripts/bench-transport.py --suites raw,e2e      # a subset
  Scripts/bench-transport.py --arm main=. --arm other=/path/to/checkout
      # an A/B: each measurement runs on every arm in turn, alternating which
      # goes first, with this tree's bench source compiled against each arm

Every QUIC measurement is preceded by a UDP loopback floor (udp-floor), and
its record carries that floor: the cost of a bare loopback `sendto` drifts on
its own, which moves every QUIC lane and no stream lane. Compare QUIC lanes
only between records whose floors agree, and A/B arms only interleaved.

Builds run under `nice -n 19` with one job. The measurements run at normal
priority: under the load a shared machine carries, a niced client is
descheduled for longer than the latencies it measures. Results land in
--out-dir as results.jsonl plus a summary table. Keep the output and its
machine-specific comparisons outside the product checkout.
"""

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import shlex
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
BENCH_SOURCE = REPO / 'Tests' / 'Performance' / 'transport_bench.cpp'
FLOOD_BYTES = 100 * 1024 * 1024
YES_LINE = 'the quick brown fox jumps over the lazy dog 0123456789 ABCDEFGHIJ'
ALL_SUITES = ['raw', 'datagram', 'e2e', 'echo', 'impaired', 'memory', 'profile']
CARRIERS = ['bridge', 'ssh', 'quic']
IMPAIRMENTS = {
    'rtt50-loss1': ['--delay-ms', '25', '--loss', '1'],
    'rtt50-20mbit': ['--delay-ms', '25', '--rate', '20000000'],
}

# The bench links the portable Core archive, built with the shipping flags and
# the training profile exactly as Scripts/build-perf-bench.sh builds it, plus
# the QUIC carriers and the listener, which the archive does not carry.
BUILD_BENCH = r'''
set -euo pipefail
ROOT="$1"; BUILD="$2"; QUIC="$3"; SOURCE="$4"
cd "$ROOT"
. "$ROOT/Scripts/build-root.sh"
CXX="${CXX:-c++}"
source "$ROOT/Scripts/portable-core-lib.sh"
source "$ROOT/Scripts/pgo-flags.sh"
FLAGS="-O3 $(ubiquitty_lto_flag) -DNDEBUG -DUBIQUITTY_QUIC=1 -I$QUIC/include"
PGO="$(ubiquitty_pgo_flags "$ROOT" "$CXX")"
[ -n "$PGO" ] && FLAGS="$FLAGS $PGO"
mkdir -p "$BUILD"
build_portable_core_archive "$BUILD" "$FLAGS"
ubiquitty_core_pal_stub_source "$BUILD/pal_stub.cpp" --with-pty-stream
LAUNCH="${UBQ_COMPILER_LAUNCHER:+$UBQ_COMPILER_LAUNCHER }$CXX"
OBJECTS=()
for source in "$SOURCE" Source/Platform/POSIX/QUICClient.cpp Source/Core/Transport/QUICChannel.cpp \
        Source/Platform/POSIX/DatagramBatch.cpp Source/Platform/Host/HostQUIC.cpp \
        Source/Platform/Host/HostPaths.cpp "$BUILD/pal_stub.cpp"; do
    object="$BUILD/transport_$(basename "$source").o"
    $LAUNCH -std=c++23 $FLAGS -fno-exceptions -fno-rtti -pthread -I"$ROOT/Source" -I"$ROOT/Tests" \
        -I"$ROOT/Source/ThirdParty/simdutf" -c "$source" -o "$object"
    OBJECTS+=("$object")
done
FRAMEWORKS=()
[ "$(uname)" = Darwin ] && FRAMEWORKS=(-framework CoreFoundation -framework Security)
$CXX -std=c++23 $FLAGS -pthread "${OBJECTS[@]}" "$PORTABLE_CORE_ARCHIVE" \
    "$QUIC/lib/libngtcp2_crypto_boringssl.a" "$QUIC/lib/libngtcp2.a" "$QUIC/lib/libssl.a" "$QUIC/lib/libcrypto.a" \
    "${FRAMEWORKS[@]}" -o "$BUILD/transport_bench"
'''


def log(message):
    print(f'bench-transport: {message}', file=sys.stderr, flush=True)


def load_average():
    return [round(value, 2) for value in os.getloadavg()]


def build_root(tree):
    return pathlib.Path(subprocess.check_output([sys.executable, str(tree / 'Scripts' / 'build_root.py'), str(tree)], text=True).strip())


def build(tree, root, quic_prefix):
    environment = dict(os.environ, UBQ_BUILD_ROOT=str(root), UBQ_BUILD_JOBS='1', UBIQUITTY_QUIC_PREFIX=str(quic_prefix))
    subprocess.run(['nice', '-n', '19', str(tree / 'Scripts' / 'ensure-quic-deps.sh'), str(quic_prefix)], check=True, env=environment)
    log(f'building the Release daemon in {root}/host')
    subprocess.run(['nice', '-n', '19', str(tree / 'Scripts' / 'build-host.sh')], check=True, env=environment, stdout=subprocess.DEVNULL)
    log('building transport_bench')
    bench_build = root / 'BuildTransportBench'
    subprocess.run(['nice', '-n', '19', 'bash', '-c', BUILD_BENCH, 'build', str(tree), str(bench_build), str(quic_prefix), str(BENCH_SOURCE)],
                   check=True, env=environment, stdout=subprocess.DEVNULL)
    return root / 'host' / 'ubiquitty-host', bench_build / 'transport_bench'


class PrivateSSHD:
    """An sshd on a loopback port that admits this user with a throwaway key.

    Runs unprivileged, so only the user running it can log in, and touches
    nothing in ~/.ssh: the host key, the client key, the authorized keys and
    the known hosts all live in a temporary directory.
    """

    def __init__(self, directory):
        self.directory = pathlib.Path(directory)
        self.process = None
        self.prefix = None

    def __enter__(self):
        root = self.directory
        for name in ('host-key', 'client-key'):
            subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(root / name)], check=True)
        shutil.copy(root / 'client-key.pub', root / 'authorized_keys')
        (root / 'authorized_keys').chmod(0o600)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        (root / 'sshd.conf').write_text(
            f'Port {port}\nListenAddress 127.0.0.1\nHostKey {root / "host-key"}\nPidFile {root / "sshd.pid"}\n'
            'PasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\nStrictModes no\n'
            f'AuthorizedKeysFile {root / "authorized_keys"}\n')
        host_key = (root / 'host-key.pub').read_text().split()
        (root / 'known_hosts').write_text(f'[127.0.0.1]:{port} {host_key[0]} {host_key[1]}\n')
        (root / 'ssh.conf').write_text(
            f'Host fixture\n HostName 127.0.0.1\n Port {port}\n IdentityFile {root / "client-key"}\n'
            f' UserKnownHostsFile {root / "known_hosts"}\n StrictHostKeyChecking yes\n IdentitiesOnly yes\n'
            ' BatchMode yes\n ConnectTimeout 5\n LogLevel ERROR\n')
        self.process = subprocess.Popen(['/usr/sbin/sshd', '-D', '-e', '-f', str(root / 'sshd.conf')],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.prefix = shlex.join(['ssh', '-F', str(root / 'ssh.conf'), 'fixture'])
        deadline = time.monotonic() + 10
        while subprocess.run(shlex.split(self.prefix) + ['true'], capture_output=True).returncode != 0:
            if time.monotonic() > deadline or self.process.poll() is not None:
                raise RuntimeError('the private sshd did not admit a login')
            time.sleep(0.1)
        return self

    def cipher(self):
        verbose = subprocess.run(shlex.split(self.prefix) + ['-v', 'true'], capture_output=True, text=True).stderr
        for line in verbose.splitlines():
            if 'kex: server->client cipher:' in line:
                return line.split('cipher:')[1].split()[0]
        return 'unknown'

    def __exit__(self, *unused):
        if self.process:
            self.process.terminate()
            self.process.wait(timeout=10)


def stop_own_processes(host_binary, bench_binary=None):
    """Stops what this run's binaries left running: their daemons, and a stopped
    client a bench that died could not stop.

    Found by the mark this run put in its environment (UBQ_TEST_RUN), not by
    the binary alone: the host under a build root is also the one a development
    build of the app starts for the person at the machine, and that daemon is
    theirs."""
    hygiene = pathlib.Path(__file__).resolve().parent / 'test-run-hygiene.py'
    listing = subprocess.run([sys.executable, str(hygiene), 'marked'], capture_output=True, text=True).stdout
    for line in listing.splitlines():
        pid, _, command = line.partition('\t')
        if command.startswith(str(host_binary) + ' --daemon') or (bench_binary and command.startswith(str(bench_binary) + ' attach-client')):
            try:
                os.kill(int(pid), signal.SIGKILL)
            except ProcessLookupError:
                pass


class Runner:
    def __init__(self, arguments, arms, scratch, sshd, results):
        self.arguments = arguments
        self.arms = arms
        self.scratch = scratch
        self.sshd = sshd
        self.results = results
        self.mixed = scratch / 'flood-mixed.txt'
        self.turn = 0

    def common(self, carrier, shape=()):
        command = ['--carrier', carrier, '--scratch', str(self.scratch)]
        if carrier == 'ssh':
            command += ['--ssh-prefix', self.sshd.prefix, '--sshd-pid', str(self.sshd.process.pid)]
        return command + list(shape)

    def run_arm(self, arm, mode, arguments, timeout, needs_host):
        command = [str(arm['bench']), mode] + (['--host', str(arm['host'])] if needs_host else []) + arguments
        load_before = load_average()
        # Files rather than pipes, and a wait on the bench's own exit: a bench that
        # dies leaves its daemon behind, and a pipe the daemon still holds would
        # keep this waiting for an end of file that never comes.
        with tempfile.TemporaryFile('w+') as out, tempfile.TemporaryFile('w+') as err:
            process = subprocess.Popen(command, stdout=out, stderr=err, text=True)
            try:
                returncode = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                log(f'{arm["label"]}: timed out: {mode} {" ".join(arguments[:6])}')
                returncode = None
            finally:
                stop_own_processes(arm['host'], arm['bench'])
            out.seek(0)
            err.seek(0)
            stdout, stderr = out.read(), err.read()
        if returncode is None:
            return []
        completed = subprocess.CompletedProcess(command, returncode, stdout, stderr)
        records = []
        for line in completed.stdout.splitlines():
            if line.startswith('{'):
                record = json.loads(line)
                record.update(loadBefore=load_before, loadAfter=load_average(), label=arm['label'])
                records.append(record)
        if completed.returncode != 0:
            log(f'{arm["label"]}: {mode} exited {completed.returncode}: {completed.stderr.strip()[-1500:]}')
        return records

    def run(self, mode, arguments, timeout, needs_host=True, quic=False):
        """Runs one measurement on every arm, alternating which arm goes first."""
        order = self.arms if self.turn % 2 == 0 else list(reversed(self.arms))
        self.turn += 1
        for arm in order:
            floor = None
            if quic:
                floors = self.run_arm(arm, 'udp-floor', ['--packets', '20000'], 120, False)
                floor = floors[0]['microsecondsPerSend'] if floors else None
            for record in self.run_arm(arm, mode, arguments, timeout, needs_host):
                if floor is not None:
                    record['udpFloorMicroseconds'] = floor
                self.results.write(json.dumps(record) + '\n')
                self.results.flush()
                log(f'{arm["label"]}: {summarise(record)}')

    def flood_commands(self):
        return {
            'mixed': f'cat {shlex.quote(str(self.mixed))}',
            'yes': f'yes {shlex.quote(YES_LINE)} | head -c {FLOOD_BYTES}',
        }

    def raw(self):
        size = self.arguments.raw_bytes
        scratch = ['--scratch', str(self.scratch)]
        for _ in range(self.arguments.repeat):
            for backlog in (64 * 1024, 8 * 1024 * 1024):
                # The large backlog runs a quarter of the bytes: it is slow for a
                # reason the record itself shows.
                count = size if backlog <= 64 * 1024 else max(size // 4, 1 << 20)
                self.run('raw', ['--carrier', 'quic', '--bytes', str(count), '--server-backlog', str(backlog)] + scratch, 1800, False, True)
            self.run('raw', ['--carrier', 'quic', '--direction', 'up', '--bytes', str(size)] + scratch, 1800, False, True)
            for carrier in ('unix', 'pipe'):
                self.run('raw', ['--carrier', carrier, '--bytes', str(size)] + scratch, 600, False)
            self.run('raw', ['--carrier', 'ssh', '--bytes', str(size), '--ssh-prefix', self.sshd.prefix] + scratch, 900, False)

    def datagram(self):
        for size in (64, 1100):
            self.run('datagram', ['--size', str(size), '--seconds', '3', '--scratch', str(self.scratch)], 120, False, True)

    def e2e(self, shapes=(('loopback', ()),), sample_dir=None, floods=None):
        for _ in range(self.arguments.repeat if sample_dir is None else 1):
            for name, shape in shapes:
                for carrier in CARRIERS:
                    for flood, command in self.flood_commands().items():
                        if floods and flood not in floods:
                            continue
                        extra = ['--sample-dir', str(sample_dir)] if sample_dir else []
                        self.run('e2e', self.common(carrier, shape) + ['--flood-command', command, '--pane-bytes', str(FLOOD_BYTES), '--label', flood, '--timeout', '180'] + extra,
                                 400, quic=carrier == 'quic')

    def echo(self, shapes=(('loopback', ()),)):
        for name, shape in shapes:
            for carrier in CARRIERS:
                for flooding in (False, True):
                    extra = ['--flood', '--flood-file', str(self.mixed)] if flooding else []
                    self.run('echo', self.common(carrier, shape) + ['--keys', str(self.arguments.keys)] + extra, 1800, quic=carrier == 'quic')

    def impaired(self):
        shapes = list(IMPAIRMENTS.items())
        self.e2e(shapes)
        self.echo(shapes)

    def memory(self):
        for carrier in CARRIERS:
            self.run('memory', self.common(carrier) + ['--flood-file', str(self.mixed), '--clients', '1,2,5,10',
                                                       '--flood-seconds', str(self.arguments.memory_flood_seconds),
                                                       '--stall-seconds', str(self.arguments.stall_seconds)], 3600, quic=carrier == 'quic')

    def profile(self, out_dir):
        for arm in self.arms:
            sample_dir = out_dir / f'samples-{arm["label"]}'
            sample_dir.mkdir(exist_ok=True)
        # Each arm writes its own directory; the bench names files by carrier.
        # Three passes of the corpus, so a stream carrier's flood outlasts the
        # sampling window instead of ending inside it.
        mixed = shlex.quote(str(self.mixed))
        for carrier in CARRIERS:
            for arm in self.arms:
                sample_dir = out_dir / f'samples-{arm["label"]}'
                arguments = self.common(carrier) + ['--flood-command', f'cat {mixed} {mixed} {mixed}', '--pane-bytes', str(3 * FLOOD_BYTES),
                                                    '--label', 'mixed-x3-sampled', '--timeout', '900', '--sample-dir', str(sample_dir)]
                for record in self.run_arm(arm, 'e2e', arguments, 1200, True):
                    self.results.write(json.dumps(record) + '\n')
        for arm in self.arms:
            for path in sorted((out_dir / f'samples-{arm["label"]}').glob('*.txt')):
                record = {'kind': 'profile', 'file': path.name, 'top': top_of_stack(path), 'label': arm['label']}
                self.results.write(json.dumps(record) + '\n')
                log(f'{arm["label"]}: {path.name}: ' + ', '.join(f'{name} {share:.0%}' for name, share in record['top'][:6]))


def top_of_stack(path, count=12):
    """The heaviest leaf frames of a `sample` report, as shares of the busy samples."""
    rows = []
    inside = False
    for line in path.read_text(errors='replace').splitlines():
        if line.startswith('Sort by top of stack'):
            inside = True
            continue
        if inside:
            if not line.strip():
                break
            parts = line.strip().rsplit(None, 1)
            if len(parts) == 2 and parts[1].isdigit():
                rows.append((parts[0].split('  (in ')[0], int(parts[1])))
    # Waits, not work. `__gettimeofday` is the bench dispatcher's timed
    # condition wait reading the clock, which a client profile would otherwise
    # rank above the code it measures.
    idle = {'__semwait_signal', 'poll', '__psynch_cvwait', 'mach_msg2_trap', 'kevent', '__select', '__workq_kernreturn', 'read', '__read_nocancel',
            'kevent_id', '__wait4', '__gettimeofday'}
    busy = [(name, samples) for name, samples in rows if name not in idle]
    total = sum(samples for _, samples in busy) or 1
    return [(name, samples / total) for name, samples in busy[:count]]


def summarise(record):
    kind = record['kind']
    if kind == 'raw':
        return (f"raw {record['carrier']} {record.get('direction', '')} backlog={record.get('serverBacklogBytes', '-')}: "
                f"{record['megabytesPerSecond']:.0f} MB/s, CPU/GiB recv {record['receiverCPUSecondsPerGiB']:.2f} send {record['senderCPUSecondsPerGiB']:.2f}")
    if kind == 'datagram':
        return f"datagram {record['payloadBytes']:.0f}B: {record['deliveredPerSecond']:.0f}/s delivered of {record['offered']:.0f} offered"
    if kind == 'e2e':
        return (f"e2e {record['carrier']} {record['link']} {record['flood']}: drained {record['consumedSeconds']:.2f}s converged {record['convergedSeconds']:.2f}s "
                f"wire/pane {record['wirePerPaneByte']:.3f} oracle={record['oracleMatched']} daemon CPU {record['daemonCPUSeconds']:.2f}s client {record['clientCPUSeconds']:.2f}s")
    if kind == 'echo':
        return (f"echo {record['carrier']} {record['link']} {record['load']}: p50 {record['p50Milliseconds']:.2f} p99 {record['p99Milliseconds']:.2f} "
                f"max {record['maxMilliseconds']:.2f} ms, timeouts {record['timeouts']:.0f}")
    if kind == 'memory':
        return (f"memory {record['carrier']} {record['phase']} n={record['clients']:.0f}: idle {record['idleFootprintBytes'] / 2**20:.1f} MiB, "
                f"flood peak {record['floodPeakFootprintBytes'] / 2**20:.1f} MiB, slope {record['floodLateSlopeBytesPerSecond'] / 2**10:.1f} KiB/s")
    return json.dumps(record)


def report(paths):
    """Renders results.jsonl files as Markdown tables, medians over repeated runs."""
    records = [json.loads(line) for path in paths for line in open(path) if line.startswith('{')]
    for record in records:
        # A QUIC carrier's counters restart with each connection, so a delta
        # across a reconnect is meaningless; records that predate the bench
        # saying so carry it as an impossible ratio, and lose it here.
        if record['kind'] == 'e2e' and not 0 <= record.get('wirePerPaneByte', 0) < 100:
            record['wirePerPaneByte'] = record['wireBytesPerSecond'] = float('nan')

    def number(value, form):
        return '-' if value != value else format(value, form)

    def median(rows, key):
        values = [row[key] for row in rows if isinstance(row.get(key), (int, float)) and row[key] == row[key]]
        return statistics.median(values) if values else float('nan')

    def grouped(kind, keys):
        groups = {}
        for record in records:
            if record['kind'] == kind:
                groups.setdefault(tuple(record.get(key) for key in keys), []).append(record)
        return sorted(groups.items(), key=lambda item: [(0, part, '') if isinstance(part, (int, float)) else (1, 0, str(part)) for part in item[0]])

    lines = []
    environments = {}
    for record in records:
        if record['kind'] == 'environment':
            environments.setdefault((record['label'], record['commit']), []).append(record)
    if environments:
        lines += ['| arm | commit | PGO profile | machine | ssh cipher | 1-minute load at the starts | runs |', '|---|---|---|---|---|---|---:|']
        for (label, commit), group in environments.items():
            record = group[0]
            loads = sorted(entry['load'][0] for entry in group)
            lines.append(f"| {label} | {commit} | {record['pgoProfileSHA256']} | {record['cpu']}, {record['cores']} cores, {record['memoryGiB']} GiB, "
                         f"{record['os']} | {record['sshCipher'].split('@')[0]} | {loads[0]:.0f}–{loads[-1]:.0f} | {len(group)} |")
        lines.append('')

    rows = grouped('raw', ['label', 'carrier', 'direction', 'serverBacklogBytes', 'link'])
    if rows:
        # Throughput lanes keep their best run, as the bench gate's best-of does:
        # load only ever makes a lane slower. The CPU and the floor are that run's.
        lines += ['Raw channel (synthetic ceiling, not product behaviour), best of the runs:', '',
                  '| arm | carrier | direction | sender backlog | MB/s | receiver CPU s/GiB | sender CPU s/GiB | corrupt bytes | UDP floor µs | load | runs |',
                  '|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|']
        for (label, carrier, direction, backlog, link), group in rows:
            best = max(group, key=lambda row: row['megabytesPerSecond'])
            backlog_text = f'{backlog / 1024:.0f} KiB' if isinstance(backlog, (int, float)) else '-'
            floor = best.get('udpFloorMicroseconds')
            lines.append(f"| {label} | {carrier}{'' if link in (None, 'loopback') else ' ' + link} | {direction} | {backlog_text} | {best['megabytesPerSecond']:.0f} | "
                         f"{best['receiverCPUSecondsPerGiB']:.2f} | {best['senderCPUSecondsPerGiB']:.2f} | {best.get('corruptBytes', 0):.0f} | "
                         f"{'-' if floor is None else f'{floor:.0f}'} | {best['loadBefore'][0]:.0f} | {len(group)} |")
        lines.append('')

    rows = grouped('datagram', ['label', 'payloadBytes'])
    if rows:
        lines += ['QUIC datagrams, listener to client, best of the runs:', '',
                  '| arm | datagram payload | delivered/s | delivered MB/s | delivered of offered | UDP floor µs | runs |', '|---|---:|---:|---:|---:|---:|---:|']
        for (label, size), group in rows:
            best = max(group, key=lambda row: row['deliveredPerSecond'])
            lines.append(f"| {label} | {size:.0f} B | {best['deliveredPerSecond']:.0f} | {best['deliveredMegabytesPerSecond']:.1f} | "
                         f"{best['deliveredFraction']:.0%} | {best.get('udpFloorMicroseconds', float('nan')):.0f} | {len(group)} |")
        lines.append('')

    rows = grouped('e2e', ['label', 'link', 'carrier', 'flood'])
    if rows:
        lines += ['End to end, 100 MiB through one pane:', '',
                  '| arm | link | carrier | flood | drained s | converged s | wire/pane | wire MB/s | applies/s | daemon CPU s | host relay CPU s | client CPU s | client relay CPU s | daemon peak MiB | oracle | reconnects | UDP floor µs | runs |',
                  '|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|']
        for (label, link, carrier, flood), group in rows:
            oracle = 'match' if all(row['oracleMatched'] for row in group) else f"{sum(not row['oracleMatched'] for row in group)} failed"
            lines.append(f"| {label} | {link} | {carrier} | {flood} | {median(group, 'consumedSeconds'):.2f} | {median(group, 'convergedSeconds'):.2f} | "
                         f"{median(group, 'wirePerPaneByte'):.3f} | {median(group, 'wireBytesPerSecond') / 1e6:.1f} | {median(group, 'appliesPerSecond'):.1f} | "
                         f"{median(group, 'daemonCPUSeconds'):.2f} | {median(group, 'hostRelayCPUSeconds'):.2f} | {median(group, 'clientCPUSeconds'):.2f} | "
                         f"{median(group, 'clientRelayCPUSeconds'):.2f} | {median(group, 'daemonPeakFootprintBytes') / 2**20:.0f} | {oracle} | "
                         f"{median(group, 'reconnects'):.0f} | {number(median(group, 'udpFloorMicroseconds'), '.0f')} | {len(group)} |")
        lines.append('')

    rows = grouped('echo', ['label', 'link', 'carrier', 'load'])
    if rows:
        lines += ['Keystroke to echo:', '',
                  '| arm | link | carrier | load | p50 ms | p99 ms | max ms | timeouts | reconnects | QUIC packets in/out per key |',
                  '|---|---|---|---|---:|---:|---:|---:|---:|---|']
        for (label, link, carrier, load), group in rows:
            packets = (f"{median(group, 'quicPacketsReceivedPerKey'):.2f} / {median(group, 'quicPacketsSentPerKey'):.2f}"
                       if carrier == 'quic' and median(group, 'quicPacketsReceivedPerKey') >= 0 else '-')
            # A lane where no key echoed reports -1; say so rather than print it.
            latencies = [median(group, key) for key in ('p50Milliseconds', 'p99Milliseconds', 'maxMilliseconds')]
            text = ' | '.join('no echo' if value < 0 else f'{value:.2f}' for value in latencies)
            lines.append(f"| {label} | {link} | {carrier} | {load} | {text} | {median(group, 'timeouts'):.0f} | {median(group, 'reconnects'):.0f} | {packets} |")
        lines.append('')

    rows = grouped('memory', ['label', 'carrier', 'phase', 'clients'])
    if rows:
        lines += ['Daemon memory with clients on one pane:', '',
                  '| arm | carrier | phase | clients | idle footprint MiB | idle malloc MiB | flood peak footprint MiB | flood-end malloc MiB | flood late slope KiB/s | all caught up |',
                  '|---|---|---|---:|---:|---:|---:|---:|---:|---|']
        for (label, carrier, phase, clients), group in rows:
            lines.append(f"| {label} | {carrier} | {phase} | {clients:.0f} | {median(group, 'idleFootprintBytes') / 2**20:.1f} | "
                         f"{median(group, 'idleMallocBytes') / 2**20:.1f} | {median(group, 'floodPeakFootprintBytes') / 2**20:.1f} | "
                         f"{median(group, 'floodEndMallocBytes') / 2**20:.1f} | {median(group, 'floodLateSlopeBytesPerSecond') / 1024:.1f} | "
                         f"{all(row['allClientsCaughtUp'] for row in group)} |")
        lines.append('')

    profiles = [record for record in records if record['kind'] == 'profile' and record['top']]
    if profiles:
        lines += ['Heaviest leaf frames during the flood, as shares of the busy samples:', '']
    for record in profiles:
        # Older records ranked the dispatcher's clock read as work; drop it and rescale.
        top = [(name, share) for name, share in record['top'] if name != '__gettimeofday']
        total = sum(share for _, share in top) or 1
        text = ', '.join(f"`{name.split('(')[0].split('<')[0]}` {share / total:.0%}" for name, share in top[:5])
        lines.append(f"- {record['label']} {record['file'].removesuffix('.txt')}: {text}")
    return '\n'.join(lines)


def environment_record(tree, host, sshd_cipher):
    def sysctl(name):
        return subprocess.run(['sysctl', '-n', name], capture_output=True, text=True).stdout.strip()

    profile = tree / 'Support' / 'pgo' / 'portable-core.profdata'
    return {
        'kind': 'environment',
        'commit': subprocess.run(['git', '-C', str(tree), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True).stdout.strip(),
        'dirty': bool(subprocess.run(['git', '-C', str(tree), 'status', '--porcelain', '--untracked-files=no'], capture_output=True, text=True).stdout.strip()),
        'machine': sysctl('hw.model'),
        'cpu': sysctl('machdep.cpu.brand_string'),
        'cores': sysctl('hw.ncpu'),
        'memoryGiB': round(int(sysctl('hw.memsize') or 0) / 2**30),
        'os': platform.platform(),
        'compiler': subprocess.run(['c++', '--version'], capture_output=True, text=True).stdout.splitlines()[0],
        'build': 'daemon: CMake Release (-O3, ThinLTO); bench: portable Core archive -O3 ThinLTO; both with the checked-in PGO profile',
        'pgoProfileSHA256': hashlib.sha256(profile.read_bytes()).hexdigest()[:16] if profile.exists() else 'none',
        'sshCipher': sshd_cipher,
        'load': load_average(),
        'time': datetime.datetime.now().isoformat(timespec='seconds'),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--arm', action='append', default=[], metavar='LABEL=PATH',
                        help='a checkout to build and measure; repeat for an interleaved A/B (default: this tree)')
    parser.add_argument('--suites', default=','.join(ALL_SUITES))
    parser.add_argument('--out-dir', type=pathlib.Path, default=None)
    parser.add_argument('--raw-bytes', type=int, default=1 << 30)
    parser.add_argument('--keys', type=int, default=1000)
    parser.add_argument('--repeat', type=int, default=1, help='runs of raw and e2e each')
    parser.add_argument('--memory-flood-seconds', type=float, default=20)
    parser.add_argument('--stall-seconds', type=float, default=120)
    parser.add_argument('--skip-build', action='store_true', help='reuse the binaries the last build left')
    parser.add_argument('--report', type=pathlib.Path, nargs='+', metavar='RESULTS', help='print Markdown tables for results.jsonl files and exit')
    arguments = parser.parse_args()
    if arguments.report:
        print(report(arguments.report))
        return
    suites = [suite for suite in arguments.suites.split(',') if suite]
    unknown = set(suites) - set(ALL_SUITES)
    if unknown:
        parser.error(f'unknown suites: {", ".join(sorted(unknown))}')

    arms = []
    for specification in arguments.arm or [f'{REPO.name}={REPO}']:
        label, _, path = specification.partition('=')
        tree = pathlib.Path(path or label).resolve()
        root = build_root(tree)
        quic_prefix = pathlib.Path(os.environ.get('UBIQUITTY_QUIC_PREFIX', root / 'quic-deps'))
        if arguments.skip_build:
            host, bench = root / 'host' / 'ubiquitty-host', root / 'BuildTransportBench' / 'transport_bench'
        else:
            host, bench = build(tree, root, quic_prefix)
        arms.append({'label': label, 'tree': tree, 'host': host, 'bench': bench})

    label = '-'.join(arm['label'] for arm in arms)
    out_dir = arguments.out_dir or pathlib.Path(tempfile.mkdtemp(prefix=f'bench-transport-{label}-'))
    out_dir.mkdir(parents=True, exist_ok=True)
    # Short: the daemon's socket path lives under it, and AF_UNIX paths are short.
    scratch = pathlib.Path(tempfile.mkdtemp(prefix='ubq-tb-', dir='/tmp'))
    # Everything this run starts carries its mark and, unless it names a state
    # directory of its own, uses the scratch one: never ~/.ubiquitty.
    os.environ['UBQ_TEST_RUN'] = '/'.join(filter(None, [os.environ.get('UBQ_TEST_RUN'), f'{os.getpid()}.{scratch.name}']))
    os.environ['UBIQUITTY_HOME'] = str(scratch / '.ubiquitty')
    log(f'results in {out_dir}; scratch in {scratch}')
    try:
        with open(out_dir / 'results.jsonl', 'a') as results, PrivateSSHD(scratch) as sshd:
            runner = Runner(arguments, arms, scratch, sshd, results)
            subprocess.run(['nice', '-n', '19', str(arms[0]['bench']), 'gen-flood', '--bytes', str(FLOOD_BYTES), '--out', str(runner.mixed)], check=True)
            cipher = sshd.cipher()
            for arm in arms:
                environment = environment_record(arm['tree'], arm['host'], cipher)
                environment['label'] = arm['label']
                results.write(json.dumps(environment) + '\n')
                log(json.dumps(environment))
            for suite in suites:
                log(f'suite {suite} (load {load_average()})')
                if suite == 'profile':
                    runner.profile(out_dir)
                else:
                    getattr(runner, suite)()
    finally:
        for arm in arms:
            stop_own_processes(arm['host'], arm['bench'])
        shutil.rmtree(scratch, ignore_errors=True)
    log(f'done: {out_dir / "results.jsonl"}')


if __name__ == '__main__':
    main()
