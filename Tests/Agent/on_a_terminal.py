#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Runs a command on a pseudo-terminal, as a person at a prompt does.

The command's standard streams are the terminal and its input is empty. What
it prints is copied to standard output, and the exit status is the command's,
or 1 when a signal ended it.

pty.spawn is not used. Before Python 3.10 its copy loop never returns on
macOS once the command has exited and standard input has ended: the master
side reports the end of output there rather than an error, the loop drops
it, and then waits on no descriptor at all. The system Python on macOS is
3.9, so a smoke that used it stopped at its first confirm.
Run as: python3 Tests/Agent/on_a_terminal.py <command> [<argument>...].
"""

import os
import pty
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: on_a_terminal.py <command> [<argument>...]", file=sys.stderr)
        return 2
    pid, master = pty.fork()
    if pid == 0:
        try:
            os.execvp(sys.argv[1], sys.argv[1:])
        except OSError as error:
            os.write(2, f"{sys.argv[1]}: {error.strerror}\n".encode())
        os._exit(127)
    while True:
        try:
            data = os.read(master, 65536)
        except OSError:
            # Linux reports EIO once the last descriptor on the terminal closes.
            break
        if not data:
            break
        view = memoryview(data)
        while view:
            view = view[os.write(1, view):]
    os.close(master)
    _, status = os.waitpid(pid, 0)
    return os.WEXITSTATUS(status) if os.WIFEXITED(status) else 1


if __name__ == "__main__":
    sys.exit(main())
