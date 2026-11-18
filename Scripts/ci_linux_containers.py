#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Keep each CI container owned until it has stopped writing runner caches."""

import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import uuid


class Interrupted(Exception):
    def __init__(self, signum):
        self.status = 128 + signum


def scope(source_root):
    return {
        "source": str(Path(source_root).resolve()),
        "run": os.environ.get("GITHUB_RUN_ID", ""),
        "attempt": os.environ.get("GITHUB_RUN_ATTEMPT", ""),
        "job": os.environ.get("GITHUB_JOB", ""),
    }


def registry():
    temporary = os.environ.get("RUNNER_TEMP")
    if not temporary:
        raise ValueError("RUNNER_TEMP must name the current runner's temporary directory")
    directory = Path(temporary) / "ubiquitty-ci-containers"
    if directory.is_symlink():
        raise ValueError("container registry must not be a symbolic link")
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    return directory


def docker(*arguments):
    return subprocess.run(["docker", *arguments], capture_output=True, text=True, timeout=30)


def remove_owned(record):
    inspected = docker("container", "inspect", record["name"])
    if inspected.returncode:
        if "No such container:" in inspected.stderr or "No such object:" in inspected.stderr:
            return
        raise ValueError("could not inspect the recorded CI container")
    objects = json.loads(inspected.stdout)
    if not isinstance(objects, list) or len(objects) != 1:
        raise ValueError("unexpected CI container inspection result")
    container = objects[0]
    labels = container.get("Config", {}).get("Labels") or {}  # preferences-vocabulary-ok: Docker's inspection schema.
    identity = container.get("Id", "")
    if (not re.fullmatch(r"[0-9a-f]{64}", identity)
            or container.get("Name") != "/" + record["name"]
            or container.get("Image") != record["image"]
            or labels.get("org.ubiquitty.ci.owner") != record["nonce"]):
        raise ValueError("recorded CI container ownership does not match; refusing removal")
    removed = docker("container", "rm", "--force", identity)
    if removed.returncode and not ("No such container:" in removed.stderr or "No such object:" in removed.stderr):
        raise ValueError("could not stop the owned CI container")
    checked = docker("container", "inspect", identity)
    if checked.returncode == 0:
        raise ValueError("owned CI container still exists after removal")
    if "No such container:" not in checked.stderr and "No such object:" not in checked.stderr:
        raise ValueError("could not confirm that the owned CI container stopped")


def cleanup(source_root):
    expected = scope(source_root)
    for path in sorted(registry().glob("*.json")):
        if path.is_symlink():
            raise ValueError("container ownership record must not be a symbolic link")
        record = json.loads(path.read_text())
        if record.get("scope") != expected:
            raise ValueError("container record belongs to another CI job; refusing cleanup")
        nonce = record.get("nonce", "")
        if (not re.fullmatch(r"[0-9a-f]{32}", nonce)
                or path.name != nonce + ".json"
                or record.get("name") != "ubq-ci-" + nonce
                or not re.fullmatch(r"sha256:[0-9a-f]{64}", record.get("image", ""))):
            raise ValueError("invalid CI container ownership record")
        remove_owned(record)
        path.unlink()


def run(source_root, image, command):
    if command[:2] != ["docker", "run"] or not re.fullmatch(r"sha256:[0-9a-f]{64}", image):
        raise ValueError("container run requires the inspected image identity")
    nonce = uuid.uuid4().hex
    record = {"nonce": nonce, "name": "ubq-ci-" + nonce, "image": image, "scope": scope(source_root)}
    path = registry() / (nonce + ".json")
    with open(path, "x", opener=lambda name, flags: os.open(name, flags, 0o600)) as output:
        json.dump(record, output)
    process = None
    previous = {}

    def interrupted(signum, _frame):
        raise Interrupted(signum)

    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        previous[signum] = signal.signal(signum, interrupted)
    status = 2
    try:
        process = subprocess.Popen(command[:2] + ["--name", record["name"], "--label",
                                              "org.ubiquitty.ci.owner=" + nonce] + command[2:])
        status = process.wait()
    except Interrupted as error:
        status = error.status
    finally:
        # A second signal must not interrupt the bounded ownership cleanup.
        for signum in previous:
            signal.signal(signum, signal.SIG_IGN)
        try:
            remove_owned(record)
            path.unlink()
        finally:
            if process is not None:
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=15)
            for signum, handler in previous.items():
                signal.signal(signum, handler)
    return status if status >= 0 else 128 - status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("run", "cleanup"))
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--image")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    command = options.command[1:] if options.command[:1] == ["--"] else options.command
    try:
        if options.operation == "cleanup":
            if command or options.image:
                parser.error("cleanup accepts no container command or image")
            cleanup(options.source_root)
            return 0
        return run(options.source_root, options.image or "", command)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"ci-linux-image: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
