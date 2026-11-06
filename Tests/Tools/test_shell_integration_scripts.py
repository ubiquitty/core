#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""Runs the shell-integration scripts in the shells they are written for.

Each script is sourced by a real zsh, bash and fish. The claims are the
integration's own: location reports and command marks, and that it treats every
program alike, wrapping none. A shell that is not installed is skipped rather
than failed, so the recipe runs on a host with only some of them.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "Resources" / "shell-integration"
class ShellIntegration(unittest.TestCase):
    """What each integration script does once a shell sources it."""

    def setUp(self):
        self.directory = Path(tempfile.mkdtemp(prefix="ubq-shell-integration-"))
        self.addCleanup(shutil.rmtree, self.directory)
        self.record = self.directory / "record"

    def run_shell(self, shell, script):
        """Runs `script` in `shell` with the scratch directory first on PATH.

        Returns the shell's standard output.
        """
        executable = shutil.which(shell)
        if executable is None:
            self.skipTest(f"{shell} is not installed")
        environment = {
            "PATH": f"{self.directory}:{os.environ.get('PATH', '/usr/bin:/bin')}",
            "HOME": str(self.directory),
            "RECORD": str(self.record),
            "TERM_PROGRAM": "Ubiquitty",
            "UBIQUITTY_INTEGRATION_DIR": str(SCRIPTS),
        }
        arguments = {
            "zsh": [executable, "-f", "-c", script],
            "bash": [executable, "--norc", "--noprofile", "-c", script],
            "fish": [executable, "--no-config", "-c", script],
        }[shell]
        completed = subprocess.run(arguments, env=environment, capture_output=True, text=True, timeout=30)
        self.assertEqual(completed.returncode, 0, f"{shell} failed: {completed.stderr}")
        return completed.stdout

    def test_location_paths_are_percent_encoded_as_utf8_bytes(self):
        locales = subprocess.check_output(["locale", "-a"], text=True).splitlines()
        for shell in ("bash", "zsh"):
            if shutil.which(shell) is None:
                continue
            for locale in ("C", "en_US.UTF-8"):
                if locale != "C" and locale not in locales:
                    continue
                for path, expected in (
                    ("/tmp/café", "/tmp/caf%C3%A9"),
                    ("/tmp/é/日本", "/tmp/%C3%A9/%E6%97%A5%E6%9C%AC"),
                    ("/tmp/a b", "/tmp/a%20b"),
                    ("/tmp/plain/path", "/tmp/plain/path"),
                    ("/tmp/%?#", "/tmp/%25%3F%23"),
                ):
                    with self.subTest(shell=shell, locale=locale, path=path):
                        output = self.run_shell(shell,
                            f'source "$UBIQUITTY_INTEGRATION_DIR/{shell}.sh"\n'
                            'trap - DEBUG\n'
                            f'export LC_ALL={locale}\n'
                            f'__ubiquitty_urlencode "{path}"')
                        self.assertEqual(output.split("\a")[-1], expected)

    def test_bash_marks_a_whole_line_as_one_command(self):
        # An interactive bash reads the lines from standard input, so its real
        # PROMPT_COMMAND runs between them. A line of several commands opens
        # its output once, before the first, and an empty line opens nothing.
        executable = shutil.which("bash")
        if executable is None:
            self.skipTest("bash is not installed")
        environment = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": str(self.directory),
            "TERM": "dumb",
            "UBIQUITTY_INTEGRATION_DIR": str(SCRIPTS),
        }
        script = (
            'source "$UBIQUITTY_INTEGRATION_DIR/bash.sh"\n'
            "echo one; echo two | cat; for word in three; do echo $word; done\n"
            "\n"
            "echo four\n"
            "exit\n"
        )
        completed = subprocess.run([executable, "--norc", "--noprofile", "-i"], input=script, env=environment,
                                   capture_output=True, text=True, timeout=30)
        output = completed.stdout
        mark = "\x1b]133;C\x07"
        # The three lines that run something: the compound one, `echo four`, `exit`.
        self.assertEqual(output.count(mark), 3, f"one C mark per command line, none for the empty one: {output!r}")
        first = output.index(mark)
        self.assertLess(first, output.index("one"), "the line's output opens before its first command")
        between = output[first + len(mark):output.index(mark, first + 1)]
        for word in ("one", "two", "three"):
            self.assertIn(word, between, f"{word} belongs to the first line's output")
        self.assertLess(output.index(mark, first + 1), output.index("four"))

    def test_prompt_restores_cursor_after_an_interrupted_command(self):
        for shell, hook, suffix in (("bash", "__ubiquitty_precmd", "sh"),
                                    ("zsh", "__ubiquitty_precmd", "sh"),
                                    ("fish", "__ubiquitty_prompt_start", "fish")):
            with self.subTest(shell=shell):
                output = self.run_shell(shell,
                    f'source "$UBIQUITTY_INTEGRATION_DIR/{shell}.{suffix}"\n'
                    "printf '\\e[0 q'\n"
                    "sh -c 'exit 130'\n"
                    f"{hook}\n")
                self.assertIn("\x1b]133;D;130\x07\x1b[7 q\x1b]133;A\x07", output)
                self.assertLess(output.index("\x1b[0 q"), output.index("\x1b[7 q"))

    def test_zsh_prompt_hooks_can_override_the_restored_cursor(self):
        output = self.run_shell("zsh", """
            custom_prompt() { printf '\\e[5 q'; }
            precmd_functions=(custom_prompt)
            source "$UBIQUITTY_INTEGRATION_DIR/zsh.sh"
            source "$UBIQUITTY_INTEGRATION_DIR/zsh.sh"
            for hook in $precmd_functions; do "$hook"; done
        """)
        self.assertEqual(output.count("\x1b[7 q"), 1)
        self.assertLess(output.index("\x1b[7 q"), output.index("\x1b[5 q"))

    def test_no_program_is_wrapped(self):
        checks = {
            "zsh": ('source "$UBIQUITTY_INTEGRATION_DIR/zsh.sh"\n(( ${+functions[claude]} )) && print wrapped || print plain'),
            "bash": ('source "$UBIQUITTY_INTEGRATION_DIR/bash.sh"\ndeclare -F claude >/dev/null && echo wrapped || echo plain'),
            "fish": ('source "$UBIQUITTY_INTEGRATION_DIR/fish.fish"\nfunctions -q claude; and echo wrapped; or echo plain'),
        }
        for shell, script in checks.items():
            with self.subTest(shell=shell):
                if shutil.which(shell) is None:
                    continue
                output = self.run_shell(shell, script)
                self.assertTrue(output.strip().endswith("plain"), f"{shell}: the integration defines no command of its own")


if __name__ == "__main__":
    unittest.main()
