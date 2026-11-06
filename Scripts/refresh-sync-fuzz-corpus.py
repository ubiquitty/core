#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""refresh-sync-fuzz-corpus.py — rebuild the Sync fuzz corpus at the current wire.

Tests/Core/Protocol/Sync/fuzz-corpus is shared by fuzz_sync and fuzz_consent.
Its seeds are opaque bytes, named by the SHA-1 of their contents. A seed whose
handshake states another wire version is refused at its first frame, so it
spends the fuzzer's time on one refusal. The builder emits one Greet at
another version as that refusal's input; the corpus keeps no other.

The refresh:

1. Runs ubq-sync-corpus-builder (Tests/Core/Protocol/Sync/build_corpus.cpp),
   which writes structured seeds through the encoders at the current wire.
2. Drops every existing seed whose stream (fuzz_sync's selectors 0, 3 and 5)
   frames a Greet or a Welcome at a nonzero version other than the one
   Envelope.h states. Version 0 stays: it is the malformed-handshake path,
   not another wire. Bare bodies (selector 1) are mutator output tried against
   every kind, not handshakes, and are left to the merge.
3. Merges the builder's seeds and the kept ones by each harness's coverage
   (libFuzzer -merge=1), so every feature the old corpus reached is still
   reached, and writes the union under SHA-1 names.

    python3 Scripts/refresh-sync-fuzz-corpus.py --builder BIN \\
        --fuzzer path/to/fuzz_sync --fuzzer path/to/fuzz_consent

--dry-run reports what would be dropped and kept without writing the corpus.
Stdlib only.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
CORPUS = os.path.join(REPO, "Tests", "Core", "Protocol", "Sync", "fuzz-corpus")
ENVELOPE = os.path.join(REPO, "Source", "Core", "Protocol", "Sync", "Envelope.h")
GREET, WELCOME = 1, 2


def wireVersion():
    """The version Envelope.h states."""
    with open(ENVELOPE, encoding="utf-8") as handle:
        match = re.search(r"inline constexpr std::uint16_t wireVersion = (\d+);", handle.read())
    if not match:
        sys.exit("refresh-sync-fuzz-corpus: Envelope.h states no wireVersion")
    return int(match.group(1))


def varint(data, at):
    """Reads one canonical LEB128 varint; returns (value, next) or (None, at)."""
    value, shift = 0, 0
    while at < len(data) and shift <= 28:
        byte = data[at]
        at += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, at
        shift += 7
    return None, at


def frames(data):
    """The whole envelopes at the front of a stream, as (kind, body)."""
    out, at = [], 0
    while at < len(data):
        kind = data[at]
        channel, cursor = varint(data, at + 1)
        if channel is None:
            break
        length, cursor = varint(data, cursor)
        if length is None or cursor + length > len(data):
            break
        out.append((kind, data[cursor:cursor + length]))
        at = cursor + length
    return out


def handshakeVersions(seed):
    """Every version a seed's framed Greets and Welcomes state to fuzz_sync."""
    if not seed:
        return []
    selector = seed[0] % 10
    stream = seed if selector == 0 else seed[1:] if selector in (3, 5) else None
    if stream is None:
        return []
    return [body[0] | body[1] << 8 for kind, body in frames(stream) if kind in (GREET, WELCOME) and len(body) >= 2]


def sha1Named(source, destination):
    """Copies every file in source into destination under its SHA-1."""
    for name in sorted(os.listdir(source)):
        with open(os.path.join(source, name), "rb") as handle:
            data = handle.read()
        with open(os.path.join(destination, hashlib.sha1(data).hexdigest()), "wb") as handle:
            handle.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--builder", required=True, help="the built ubq-sync-corpus-builder")
    parser.add_argument("--fuzzer", action="append", default=[], help="a libFuzzer harness the corpus feeds")
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args()
    if not options.dry_run and not options.fuzzer:
        sys.exit("refresh-sync-fuzz-corpus: name at least one --fuzzer to merge by")

    version = wireVersion()
    work = tempfile.mkdtemp(prefix="ubq-sync-corpus-")
    try:
        built, fresh, kept = (os.path.join(work, name) for name in ("built", "fresh", "kept"))
        for directory in (built, fresh, kept):
            os.mkdir(directory)
        subprocess.run([options.builder, built], check=True, stdout=subprocess.DEVNULL)
        sha1Named(built, fresh)

        dropped = 0
        for name in sorted(os.listdir(CORPUS)):
            with open(os.path.join(CORPUS, name), "rb") as handle:
                seed = handle.read()
            if any(found not in (0, version) for found in handshakeVersions(seed)):
                dropped += 1
                continue
            shutil.copy(os.path.join(CORPUS, name), os.path.join(kept, name))
        print("refresh-sync-fuzz-corpus: %d built, %d kept, %d dropped for another wire version"
              % (len(os.listdir(fresh)), len(os.listdir(kept)), dropped))
        if options.dry_run:
            return 0

        merged = os.path.join(work, "merged")
        os.mkdir(merged)
        for fuzzer in options.fuzzer:
            output = os.path.join(work, "merge-" + os.path.basename(fuzzer))
            os.mkdir(output)
            subprocess.run([fuzzer, "-merge=1", output, fresh, kept], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            sha1Named(output, merged)
        # The builder's seeds stay whatever a merge thought of them: they are
        # the current wire's handshakes, the inputs the rest mutate from.
        sha1Named(fresh, merged)

        for name in os.listdir(CORPUS):
            os.remove(os.path.join(CORPUS, name))
        sha1Named(merged, CORPUS)
        print("refresh-sync-fuzz-corpus: wrote %d seeds to %s" % (len(os.listdir(CORPUS)), os.path.relpath(CORPUS, REPO)))
    finally:
        shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
