#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercises real Sync carriers with fixture-owned peers, keys, and processes.

    python3 Scripts/check-sync-transport.py --binary <sync_transport_acceptance>
    python3 Scripts/check-sync-transport.py --self-test
"""

import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import pwd
import re
import select
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

import gate_report


GATE = "check-sync-transport"
CONTAINER_MARKER = Path("/etc/ubiquitty-transport-fixture")
FIXTURE_USER = "ubq-transport"
SOCKET_LEAF = "host-0000000000000001.sock"
SUCCESS = "sync-transport: ok ({carrier}; attach, typing, scrollback, images, resize, reconnect)"
TEST_TIMEOUT_SECONDS = 100


class Refusal(RuntimeError):
    """An isolation prerequisite was not established; no fake coverage replaces it."""


class OwnedProcess:
    """Owns one Popen child; no process search or reported PID grants authority."""

    def __init__(self, arguments, **keywords):
        if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
            raise Refusal("Child ownership requires the default SIGCHLD disposition")
        self.process = subprocess.Popen(arguments, **keywords)
        self.owned = True

    def communicate(self, *, timeout, input=None):
        result = self.process.communicate(input=input, timeout=timeout)
        self.owned = False
        return result

    def wait(self, timeout):
        result = self.process.wait(timeout=timeout)
        self.owned = False
        return result

    def signal(self, number):
        if not self.owned or self.process.pid <= 1:
            return False
        if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
            self.owned = False
            raise Refusal("Child ownership was lost after SIGCHLD disposition changed")
        while True:
            try:
                # WNOWAIT leaves an exited child unreaped and its PID pinned.
                # Only this owner reaps it, on this thread, after verification.
                exited = os.waitid(os.P_PID, self.process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                break
            except InterruptedError:
                continue
            except ChildProcessError:
                self.owned = False
                return False
        if exited is not None:
            self.wait(1)
            return False
        os.kill(self.process.pid, number)
        return True

    def close(self):
        if not self.owned:
            return
        if not self.signal(signal.SIGTERM):
            return
        try:
            self.wait(3)
        except subprocess.TimeoutExpired:
            if self.signal(signal.SIGKILL):
                self.wait(3)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class PeerControls:
    """Closes fixture PTY peers through their own control sockets, never signals."""

    def __init__(self, path, identity):
        self.path = path
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(str(path))
        self.listener.listen(8)
        self.listener.settimeout(0.1)
        path.chmod(0o600)
        if identity is not None:
            os.chown(path, identity.pw_uid, identity.pw_gid)
        self.peers = []
        self.lock = threading.Lock()
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self.accept, name="sync-fixture-peer-controls")
        self.thread.start()

    def accept(self):
        while not self.stopping.is_set():
            try:
                peer, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self.lock:
                if self.stopping.is_set():
                    peer.close()
                else:
                    self.peers.append(peer)

    def close(self):
        self.stopping.set()
        self.listener.close()
        self.thread.join(timeout=2)
        with self.lock:
            for peer in self.peers:
                peer.close()
            self.peers.clear()
        if self.thread.is_alive():
            raise RuntimeError("The fixture control acceptor did not stop")


def executable(value):
    path = Path(value).expanduser().resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise Refusal(f"The required executable is missing: {path.name}")
    return path


def command_tool(name):
    value = shutil.which(name)
    if value is None:
        raise Refusal(f"The required tool is unavailable: {name}")
    return executable(value)


def ssh_identity():
    # OpenSSH always invokes the account's passwd shell, even with ForceCommand.
    # A developer's account can run ~/.zshenv before our command; never use it.
    if sys.platform != "linux" or os.geteuid() != 0 or not Path("/.dockerenv").is_file():
        raise Refusal("Real SSH requires the disposable Linux fixture container; the local user's login shell is not isolated")
    try:
        marker = CONTAINER_MARKER.stat()
        identity = pwd.getpwnam(FIXTURE_USER)
    except (OSError, KeyError) as error:
        raise Refusal("The disposable container fixture account and marker are missing") from error
    if marker.st_uid != 0 or marker.st_mode & 0o022 or CONTAINER_MARKER.read_text().strip() != "sync-transport-v1":
        raise Refusal("The disposable container fixture marker is not trusted")
    if identity.pw_uid == 0 or identity.pw_shell != "/bin/sh" or identity.pw_dir != "/home/ubq-transport":
        raise Refusal("The SSH fixture account does not have its isolated shell and home")
    return identity


def process_identity(identity):
    if identity is None:
        return {}
    return {"user": identity.pw_uid, "group": identity.pw_gid, "extra_groups": []}


def private_directory(path, identity):
    path.mkdir(mode=0o700)
    if identity is not None:
        os.chown(path, identity.pw_uid, identity.pw_gid)


def run_command(arguments, environment, identity=None, timeout=15):
    with OwnedProcess([str(argument) for argument in arguments], env=environment,
                      stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                      **process_identity(identity)) as child:
        output, diagnostic = child.communicate(timeout=timeout)
        if child.process.returncode:
            raise RuntimeError(f"Fixture command {Path(arguments[0]).name} failed: " + diagnostic.decode(errors="replace")[-1000:])
        return output


def wait_ready(server):
    deadline = time.monotonic() + 10
    output = bytearray()
    while time.monotonic() < deadline:
        ready, _, _ = select.select([server.process.stdout], [], [], 0.1)
        if not ready:
            continue
        chunk = os.read(server.process.stdout.fileno(), 4096)
        if not chunk:
            raise RuntimeError("The foreground fixture host exited before readiness")
        output.extend(chunk)
        if len(output) > 8192:
            raise RuntimeError("The foreground fixture host produced an invalid readiness record")
        if b"SYNC_TRANSPORT_SERVER_READY\n" in output:
            return
    raise RuntimeError("The foreground fixture host did not become ready")


def isolated_environment(root, control):
    return {
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
        "XDG_CONFIG_HOME": str(root / "config"),
        "XDG_CACHE_HOME": str(root / "cache"),
        "UBIQUITTY_HOME": str(root / "host"),
        "UBQ_TRANSPORT_CONTROL": str(control),
        "TERM": "xterm-256color",
        "LANG": "C.UTF-8" if sys.platform == "linux" else "en_US.UTF-8",
    }


def prepare_ssh(root, binary, environment, identity):
    keygen = command_tool("ssh-keygen")
    for name in ("host-key", "client-key"):
        run_command([keygen, "-q", "-t", "ed25519", "-N", "", "-f", root / name], environment)
    # Root-owned 0755 parents and an absolute authorized-key path avoid reading
    # the fixture account's home or any developer's authorized_keys file.
    public_key = root / "client-key.pub"
    public_key.chmod(0o644)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    wrapper = root / "remote-command.py"
    wrapper.write_text(
        "import os\n"
        f"binary = {str(binary)!r}\n"
        f"environment = {environment!r}\n"
        "operation = os.environ.get('SSH_ORIGINAL_COMMAND', '')\n"
        f"arguments = {{'attach': ['--attach', '--socket', {SOCKET_LEAF!r}], "
        f"'bootstrap': ['--quic-bootstrap', '--socket', {SOCKET_LEAF!r}]}}\n"
        "if operation not in arguments:\n    raise SystemExit(2)\n"
        "os.chdir(environment['UBIQUITTY_HOME'])\n"
        "os.execve(binary, [binary, *arguments[operation]], environment)\n",
        encoding="utf-8")
    wrapper.chmod(0o644)
    server_configuration = root / "sshd.conf"
    server_configuration.write_text(
        f"Port {port}\nListenAddress 127.0.0.1\nHostKey {root / 'host-key'}\n"
        f"PidFile {root / 'sshd.pid'}\nAllowUsers {FIXTURE_USER}\n"
        f"AuthorizedKeysFile {public_key}\n"
        "StrictModes yes\nLogLevel VERBOSE\nPubkeyAuthentication yes\nAuthenticationMethods publickey\n"
        "PasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\n"
        "PermitRootLogin no\nPermitUserRC no\nPermitUserEnvironment no\nPermitTTY no\n"
        "AllowAgentForwarding no\nAllowTcpForwarding no\nX11Forwarding no\nPermitTunnel no\n"
        f"ForceCommand /usr/bin/python3 {shlex.quote(str(wrapper))}\n",
        encoding="utf-8")
    host_key = (root / "host-key.pub").read_text().split()
    known_hosts = root / "known_hosts"
    known_hosts.write_text(f"[127.0.0.1]:{port} {host_key[0]} {host_key[1]}\n", encoding="utf-8")
    configuration = root / "ssh.conf"
    configuration.write_text(
        f"Host fixture\n HostName 127.0.0.1\n Port {port}\n User {FIXTURE_USER}\n"
        f" IdentityFile {root / 'client-key'}\n UserKnownHostsFile {known_hosts}\n"
        " GlobalKnownHostsFile /dev/null\n StrictHostKeyChecking yes\n IdentitiesOnly yes\n"
        " IdentityAgent none\n BatchMode yes\n ConnectTimeout 5\n LogLevel ERROR\n"
        " ControlMaster no\n ControlPath none\n RequestTTY no\n",
        encoding="utf-8")
    # The SSH client runs as the fixture account, not root.
    os.chown(root / "client-key", identity.pw_uid, identity.pw_gid)
    return server_configuration, configuration, port


def wait_sshd(port, server):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            # Verifying the exact child without reaping also pins its PID.
            exited = os.waitid(os.P_PID, server.process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if exited is not None:
                raise RuntimeError("The private OpenSSH listener exited before readiness")
            time.sleep(0.02)
    raise RuntimeError("The private OpenSSH listener did not become ready")


def temporary_parent(carrier, identity):
    if carrier == "ssh":
        if identity is None or identity.pw_dir != "/home/ubq-transport":
            raise Refusal("SSH temporary files require the verified container fixture home")
        # StrictModes validates every authorized-key ancestor, so /tmp is
        # unsuitable even when our unique child directory is root-owned.
        return identity.pw_dir
    return "/tmp"


def report_ssh_failure(path):
    # OpenSSH's configured VERBOSE diagnostics contain connection metadata
    # and public-key fingerprints, not key material or protocol payloads.
    # Never dump host.log, the bootstrap response, or a key/configuration file.
    try:
        with path.open("rb") as source:
            source.seek(0, os.SEEK_END)
            source.seek(max(0, source.tell() - 4096))
            tail = source.read(4096).decode(errors="replace")
    except OSError:
        return
    tail = "".join(character for character in tail if character.isprintable() or character in "\n\t")
    if tail:
        print("Private SSH listener diagnostics (last 4096 bytes):\n" + tail, file=sys.stderr, flush=True)


def daemon_resources(server):
    """Samples only this runner's still-owned, unreaped foreground daemon."""
    if not server.owned or server.process.returncode is not None:
        raise RuntimeError("The private daemon exited before its resource sample")
    pid = str(server.process.pid)
    resident = subprocess.check_output(["/bin/ps", "-o", "rss=", "-p", pid], timeout=5)
    if sys.platform == "linux":
        descriptors = len(list(Path(f"/proc/{pid}/fd").iterdir()))
    else:
        listing = subprocess.check_output(["/usr/sbin/lsof", "-nP", "-p", pid, "-Ff"], timeout=5)
        descriptors = sum(bool(re.fullmatch(rb"f[0-9]+", line)) for line in listing.splitlines())
    if descriptors <= 0:
        raise RuntimeError("The private daemon's descriptors could not be counted")
    return {"fd_count": descriptors, "rss_kib": int(resident.strip())}


def read_soak(client, server, iterations, seed, max_rss_growth, two_clients=False):
    """Drains both client pipes with a deadline and samples each completed cycle."""
    streams = {client.process.stdout.fileno(): (False, bytearray()),
               client.process.stderr.fileno(): (True, bytearray())}
    deadline = time.monotonic() + 30 + iterations * 20
    total = 0
    completed = []
    observers = []
    success = 0
    while streams:
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Soak seed {seed} exceeded its overall deadline")
        ready, _, _ = select.select(list(streams), [], [], 0.2)
        for descriptor in ready:
            diagnostic, pending = streams[descriptor]
            chunk = os.read(descriptor, 8192)
            total += len(chunk)
            if total > 8 * 1024 * 1024:
                raise RuntimeError("Soak diagnostics exceeded their output allowance")
            if not chunk:
                if pending:
                    print(pending.decode(errors="replace"), file=sys.stderr if diagnostic else sys.stdout, flush=True)
                del streams[descriptor]
                continue
            pending.extend(chunk)
            while b"\n" in pending:
                line, _, remainder = pending.partition(b"\n")
                pending[:] = remainder
                text = line.decode(errors="replace")
                print(text, file=sys.stderr if diagnostic else sys.stdout, flush=True)
                if not diagnostic and text.startswith("sync-soak: observer "):
                    fields = dict(field.split("=", 1) for field in text.split()[2:])
                    observed = {key: int(value) for key, value in fields.items()}
                    if not two_clients or observed["iteration"] != len(observers) + 1:
                        raise RuntimeError("Soak omitted or repeated an observer iteration")
                    observers.append(observed)
                if not diagnostic and text.startswith("sync-soak: complete "):
                    fields = dict(field.split("=", 1) for field in text.split()[2:])
                    record = {key: int(value) for key, value in fields.items()}
                    if record["iteration"] != len(completed) + 1:
                        raise RuntimeError("Soak omitted or repeated an iteration")
                    if two_clients:
                        if len(observers) != record["iteration"]:
                            raise RuntimeError("Soak did not verify the uninterrupted observer")
                        for key in ("acknowledged", "consumed"):
                            if observers[-1][key] != record[key]:
                                raise RuntimeError("The observer's effect receipts differ from the resumed client's")
                            record["observer_" + key] = observers[-1][key]
                    record.update(daemon_resources(server))
                    completed.append(record)
                    print("sync-soak-resources: " + json.dumps(record, sort_keys=True), flush=True)
                if not diagnostic and text == f"sync-soak: ok seed={seed} iterations={iterations}":
                    success += 1
    if client.wait(5) or len(completed) != iterations or success != 1:
        raise RuntimeError(f"Reconnect soak seed {seed} failed after {len(completed)} complete iterations")
    if iterations > 20:
        baseline = completed[19]
        final = completed[-1]
        growth = {key: final[key] - baseline[key] for key in ("fd_count", "rss_kib")}
        print("sync-soak-growth: " + json.dumps(growth, sort_keys=True), flush=True)
        if growth["fd_count"] > 2 or growth["rss_kib"] > max_rss_growth:
            raise RuntimeError(f"Reconnect soak seed {seed} exceeded its post-warmup resource allowance: {growth}")


def run_carrier(binary, carrier, identity=None, sshd=None, *, soak_iterations=None, seed=1, max_rss_growth=16384, two_clients=False):
    with tempfile.TemporaryDirectory(prefix="ubq-st-", dir=temporary_parent(carrier, identity)) as temporary:
        root = Path(temporary).resolve()
        # The SSH monitor is root, and only the pre-created container account
        # traverses these fixture-owned paths. No recursive chown is needed.
        root.chmod(0o755 if identity is not None else 0o700)
        for leaf in ("config", "cache", "host"):
            private_directory(root / leaf, identity)
        control = root / "peer-control.sock"
        environment = isolated_environment(root, control)
        openssl = command_tool("openssl")
        run_command([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                     "-subj", "/CN=localhost", "-keyout", root / "host/quic-key.pem",
                     "-out", root / "host/quic-cert.pem"], environment, identity)
        (root / "host/quic-key.pem").chmod(0o600)
        controls = PeerControls(control, identity)
        server = None
        ssh_server = None
        try:
            with (root / "host.log").open("wb") as host_log, (root / "sshd.log").open("wb") as ssh_log:
                server = OwnedProcess([str(binary), "--fixture-server", str(root / "host")], env=environment,
                                      cwd=root, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=host_log,
                                      **process_identity(identity))
                wait_ready(server)
                command = "unused"
                if carrier == "ssh":
                    server_configuration, configuration, port = prepare_ssh(root, binary, environment, identity)
                    ssh_server = OwnedProcess([str(sshd), "-D", "-e", "-f", str(server_configuration)], env=environment,
                                              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=ssh_log)
                    wait_sshd(port, ssh_server)
                    command = "exec " + shlex.join([str(command_tool("ssh")), "-F", str(configuration), "fixture", "attach"])
                bootstrap_file = root / "bootstrap"
                if carrier == "quic":
                    output = run_command([binary, "--quic-bootstrap", "--socket", SOCKET_LEAF], environment, identity)
                    bootstrap_file.write_bytes(output)
                    bootstrap_file.chmod(0o600)
                    if identity is not None:
                        os.chown(bootstrap_file, identity.pw_uid, identity.pw_gid)
                arguments = [str(binary), "--client", carrier, command, str(bootstrap_file)]
                if soak_iterations is not None:
                    arguments += ["--soak", str(soak_iterations), str(seed)]
                if two_clients:
                    observer_bootstrap = root / "observer-bootstrap"
                    observer_bootstrap.write_bytes(run_command([binary, "--quic-bootstrap", "--socket", SOCKET_LEAF], environment, identity))
                    observer_bootstrap.chmod(0o600)
                    arguments += ["--observer-bootstrap", str(observer_bootstrap)]
                with OwnedProcess(arguments, env=environment,
                                  cwd=root, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  **process_identity(identity)) as client:
                    if soak_iterations is not None:
                        read_soak(client, server, soak_iterations, seed, max_rss_growth, two_clients)
                    else:
                        output, diagnostic = client.communicate(timeout=TEST_TIMEOUT_SECONDS)
                        output = output.decode(errors="replace")
                        diagnostic = diagnostic.decode(errors="replace")
                        print(output, end="", flush=True)
                        print(diagnostic, end="", file=sys.stderr, flush=True)
                        if client.process.returncode or output.count(SUCCESS.format(carrier=carrier)) != 1:
                            raise RuntimeError(f"The real {carrier.upper()} acceptance case failed")
                controls.close()
                controls = None
                if server.wait(5):
                    raise RuntimeError("The foreground host did not reap its PTY peer and exit cleanly")
        except Exception:
            if carrier == "ssh":
                report_ssh_failure(root / "sshd.log")
            raise
        finally:
            # Closing only our control sockets asks each PTY peer to exit
            # itself. Its actual parent, the host, performs the reap.
            cleanup_failures = []
            try:
                if controls is not None:
                    controls.close()
            except Exception as error:
                cleanup_failures.append(str(error))
            try:
                if server is not None and server.owned:
                    try:
                        server.wait(5)
                    except subprocess.TimeoutExpired:
                        server.close()
            except Exception as error:
                cleanup_failures.append(str(error))
            try:
                if ssh_server is not None:
                    ssh_server.close()
            except Exception as error:
                cleanup_failures.append(str(error))
            if cleanup_failures:
                raise RuntimeError("Fixture cleanup failed: " + "; ".join(cleanup_failures))


def self_test():
    """Plants the refusals this gate owes before it owns a process, and checks each lands.

    No carrier runs: a binary that is absent, or present and not executable,
    must be refused while the arguments are read, before an sshd or a host is
    started that a wrong refusal would leave behind.
    """
    problems = []
    quiet = io.StringIO()
    with tempfile.TemporaryDirectory() as scratch:
        absent = Path(scratch) / "sync_transport_acceptance"
        present = Path(scratch) / "not-executable"
        present.write_text("", encoding="utf-8")
        present.chmod(0o644)
        with contextlib.redirect_stdout(quiet), contextlib.redirect_stderr(quiet):
            if main(["--binary", str(absent), "--transport", "quic"]) != 2:
                problems.append("an absent acceptance binary was not refused")
            if main(["--binary", str(present), "--transport", "quic"]) != 2:
                problems.append("a binary that is not executable was not refused")
            for options in (("--transport", "ssh", "--soak"),
                            ("--transport", "quic", "--soak", "0"),
                            ("--transport", "quic", "--soak", "10001"),
                            ("--transport", "quic", "--two-clients"),
                            ("--transport", "quic", "--soak", "--seed", "4294967296")):
                if main(["--binary", sys.executable, *options]) != 2:
                    problems.append("invalid soak arguments were not refused: " + " ".join(options))
    for problem in problems:
        print("self-test: %s" % problem, file=sys.stderr)
    print("%s: self-test %s" % (GATE, "FAILED" if problems else "ok"))
    return 1 if problems else 0


def main(arguments=None):
    if arguments is None and "--self-test" in sys.argv[1:]:
        return self_test()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="the opt-in sync_transport_acceptance executable")
    parser.add_argument("--transport", choices=("all", "ssh", "quic"), default="all")
    parser.add_argument("--sshd", type=Path, default=Path("/usr/sbin/sshd"))
    parser.add_argument("--soak", nargs="?", const=200, type=int, metavar="ITERATIONS",
                        help="run seeded QUIC reconnect churn (default 200 iterations)")
    parser.add_argument("--seed", type=int, default=1, help="unsigned 32-bit reconnect-soak seed")
    parser.add_argument("--two-clients", action="store_true", help="keep an independent observer attached during the soak")
    parser.add_argument("--max-rss-growth-kib", type=int, default=16384,
                        help="daemon RSS growth allowed after 20 warmup iterations")
    options = parser.parse_args(arguments)
    try:
        if options.soak is not None and (options.transport != "quic" or not 1 <= options.soak <= 10000):
            raise Refusal("Reconnect soak requires --transport quic and 1..10000 iterations")
        if options.two_clients and options.soak is None:
            raise Refusal("Two-client coverage requires --soak")
        if not 0 <= options.seed <= 0xffffffff or options.max_rss_growth_kib < 0:
            raise Refusal("Soak seed or RSS allowance is out of range")
        if signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
            raise Refusal("Child ownership requires the default SIGCHLD disposition")
        # Python's signal installation clears inherited sigaction flags,
        # including SA_NOCLDWAIT. No code in this runner changes it afterwards.
        signal.signal(signal.SIGCHLD, signal.SIG_DFL)
        binary = executable(options.binary)
        carriers = ("ssh", "quic") if options.transport == "all" else (options.transport,)
        identity = ssh_identity() if "ssh" in carriers else None
        sshd = executable(options.sshd) if identity is not None else None
        for carrier in carriers:
            run_carrier(binary, carrier, identity, sshd, soak_iterations=options.soak,
                        seed=options.seed, max_rss_growth=options.max_rss_growth_kib, two_clients=options.two_clients)
    except Refusal as error:
        return gate_report.refused(GATE, str(error))
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        return gate_report.failed(GATE, str(error))
    return gate_report.ok(GATE, "Real " + " and ".join(carrier.upper() for carrier in carriers) + " acceptance passed")


if __name__ == "__main__":
    sys.exit(main())
