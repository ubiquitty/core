#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# replay-stream.py — feed a recorded byte stream to a terminal, and say how much got through.
#
#   replay-stream.py --stream unicode_cjk_140x36.stream --counter /path/bytes
#
# The counter is the point. Under a saturating stimulus every terminal runs at
# its own top speed, and those differ widely — so energy per SECOND ranks the
# fastest terminal as the least efficient, because it is doing the most work.
# Energy per BYTE is the comparison that survives that, and it needs
# the byte count the terminal actually consumed during the sample window, which
# only the writer can report.
#
# Two stream shapes. A plain dump is written as fast as the terminal drains it.
# A timed stream from record-stream.py (its magic line first) is written at the
# pace it was recorded at, each record after its own delay, so a paced workload
# reaches every terminal with the same bytes at the same cadence; the loop
# restarts the recording when it ends.
#
# The stream is read into memory once, so the loop is a write and nothing else:
# a producer that re-reads or re-formats is inside every number it produces.

import argparse
import os
import struct
import sys
import time

TIMED_MAGIC = b"UBQTSTREAM1\n"


def timed_records(payload):
    """The (delay_seconds, bytes) records of a timed stream, parsed once."""
    records = []
    offset = len(TIMED_MAGIC)
    while offset + 8 <= len(payload):
        delay_us, length = struct.unpack_from("<II", payload, offset)
        offset += 8
        records.append((delay_us / 1_000_000.0, payload[offset:offset + length]))
        offset += length
    return records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stream", required=True)
    parser.add_argument("--counter", required=True)
    # How often the counter is refreshed. A write per chunk would show up in the
    # measurement; a megabyte apart is under a millisecond of work per second at
    # the rates these terminals drain.
    parser.add_argument("--report-every", type=int, default=1 << 20)
    args = parser.parse_args()

    with open(args.stream, "rb") as handle:
        payload = handle.read()

    out = sys.stdout.buffer
    counter = open(args.counter, "w")
    total = 0
    since_report = 0

    def report(force=False):
        nonlocal since_report
        if force or since_report >= args.report_every:
            since_report = 0
            counter.seek(0)
            counter.write(f"{total}\n")
            counter.flush()

    try:
        if payload.startswith(TIMED_MAGIC):
            records = timed_records(payload)
            while True:
                due = time.monotonic()
                for delay, piece in records:
                    due += delay
                    wait = due - time.monotonic()
                    if wait > 0:
                        time.sleep(wait)
                    out.write(piece)
                    out.flush()
                    total += len(piece)
                    since_report += len(piece)
                    # Per record, not per megabyte: a paced pass delivers a few
                    # records a second, and a sample window ending anywhere in
                    # it reads the bytes delivered so far.
                    report(force=True)
        else:
            # 64 KiB keeps the pty busy without building a backlog the terminal
            # cannot drain, which would measure the pipe rather than the renderer.
            chunk = 1 << 16
            while True:
                for offset in range(0, len(payload), chunk):
                    piece = payload[offset:offset + chunk]
                    out.write(piece)
                    out.flush()
                    total += len(piece)
                    since_report += len(piece)
                    report()
    except (BrokenPipeError, KeyboardInterrupt):
        pass
    finally:
        try:
            report(force=True)
            counter.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
