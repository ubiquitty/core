#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""test-library-roots.py — a repository that carries ubiquitty-core runs its own workflow from it.

ubiquitty-kit and the app hold this repository's Scripts/ under
Libraries/ubiquitty-core. The shared machinery has to judge the repository that
carries it and not the library: the roots it resolves, the section tables
check-style.py loads, the hooks git dispatches and the push gates it sources.
This builds a small consumer, with the library's Scripts/ as a submodule, and
runs each of them from there.

    python3 Scripts/test-library-roots.py
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

LIBRARY = Path(__file__).resolve().parent.parent

HEADER = "// Copyright (C) 2026 Stanislav Yaglo\n// SPDX-License-Identifier: Apache-2.0\n"


def git(directory, *arguments, check=True, environment=None):
    environment = dict(os.environ, **(environment or {}))
    environment.update(GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                       GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid",
                       GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="gc.auto", GIT_CONFIG_VALUE_0="0")
    for variable in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        environment.pop(variable, None)
    return subprocess.run(["git", *arguments], cwd=directory, env=environment,
                          capture_output=True, text=True, check=check)


class LibraryRootsTests(unittest.TestCase):
    """A consumer repository named `kit`, with the library under Libraries/."""

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="ubq-library-")
        base = Path(cls.temporary.name)
        # The library: this checkout's scripts and the formatter configuration,
        # as a repository of its own, which the consumer then adds as a submodule.
        library = base / "library"
        (library / "Scripts").mkdir(parents=True)
        for name in os.listdir(LIBRARY / "Scripts"):
            source = LIBRARY / "Scripts" / name
            if source.is_dir():
                shutil.copytree(source, library / "Scripts" / name,
                                ignore=shutil.ignore_patterns("__pycache__"))
            else:
                shutil.copy2(source, library / "Scripts" / name)
        # The build modules the stale-cache gate reads defaults from.
        for name in ("CMakeLists.txt", ".clang-format"):
            if (LIBRARY / name).is_file():
                shutil.copy2(LIBRARY / name, library / name)
        if (LIBRARY / "cmake").is_dir():
            shutil.copytree(LIBRARY / "cmake", library / "cmake")
        # What the library's export leaves to the repositories above it: their
        # section tables, ledgers, stanzas and hook parts. A library checkout
        # is not a consumer, so it names no repository.
        for leftover in ("repository.txt", "style_sections_kit.py", "style_sections_app.py",
                         "gate-ledger.kit.txt", "gate-ledger.app.txt",
                         "run-tests-stanzas-kit.sh", "run-tests-stanzas-app.sh",
                         "check-editor-wire-tables.py", "hooks/pre-commit-parts/app.sh"):
            (library / "Scripts" / leftover).unlink(missing_ok=True)
        git(library, "init", "-q", "--initial-branch=main")
        git(library, "add", "-A")
        git(library, "commit", "-q", "-m", "library", "--no-verify")

        consumer = base / "consumer"
        (consumer / "Scripts").mkdir(parents=True)
        (consumer / "Source").mkdir()
        (consumer / "Scripts" / "repository.txt").write_text("kit\n", encoding="utf-8")
        (consumer / "Source" / "Hello.h").write_text(
            HEADER + "#pragma once\n\n/// Says hello.\n///\n/// - Returns: zero.\nint hello();\n", encoding="utf-8")
        (consumer / "Scripts" / "build-fixture.sh").write_text(
            "#!/usr/bin/env bash\n# Copyright (C) 2026 Stanislav Yaglo\n# SPDX-License-Identifier: Apache-2.0\n"
            "set -euo pipefail\ncmake -B \"$UBQ_BUILD_ROOT/fixture\" -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug\n",
            encoding="utf-8")
        os.chmod(consumer / "Scripts" / "build-fixture.sh", 0o755)
        # A table of the consumer's own: one section, running a gate it carries.
        (consumer / "Scripts" / "check-consumer.py").write_text(
            "#!/usr/bin/env python3\nprint('check-consumer: ok (fixture)')\n", encoding="utf-8")
        (consumer / "Scripts" / "style_sections_kit.py").write_text(
            "REPOSITORY = 'kit'\nHAND = {}\nSECTIONS = ({\n"
            "    'title': 'the consumer gate (Scripts/check-consumer.py)',\n"
            "    'kind': 'gate', 'gate': 'check-consumer.py',\n"
            "    'command': ['python3', './Scripts/check-consumer.py'],\n"
            "    'ok': 'all', 'bad': 'all', 'fail': None},)\n", encoding="utf-8")
        (consumer / "Scripts" / "gate-ledger.kit.txt").write_text(
            "budget gate-self-tests-uncovered 1\nbudget census-undated 0\n"
            "budget vocabulary-span 0\nbudget vocabulary-array 0\n", encoding="utf-8")
        git(consumer, "init", "-q", "--initial-branch=main")
        git(consumer, "remote", "add", "origin", "https://example.invalid/consumer.git")
        # The submodule's address is a placeholder; the clone is the directory.
        git(library, "remote", "add", "origin", "https://example.invalid/ubiquitty-core.git")
        # A clone and not a copy of the directory: the commit above may leave
        # git compacting its objects, which a copy walking them would race.
        git(consumer, "clone", "-q", "--no-hardlinks", str(library), "Libraries/ubiquitty-core")
        git(consumer / "Libraries" / "ubiquitty-core", "remote", "set-url", "origin",
            "https://example.invalid/ubiquitty-core.git")
        git(consumer, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
            "https://example.invalid/ubiquitty-core.git", "Libraries/ubiquitty-core")
        git(consumer, "add", "-A")
        git(consumer, "commit", "-q", "-m", "consumer", "--no-verify")
        cls.consumer = consumer
        cls.library_scripts = consumer / "Libraries" / "ubiquitty-core" / "Scripts"

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_in_consumer(self, *command, environment=None):
        merged = dict(os.environ, **(environment or {}))
        merged.pop("UBQ_CORE_ROOT", None)
        merged.pop("UBQ_KIT_ROOT", None)
        return subprocess.run(command, cwd=self.consumer, env=merged,
                              capture_output=True, text=True)

    def test_the_roots_are_the_consumers_and_the_librarys(self):
        for query, expected in (("subject", self.consumer), ("core", self.consumer / "Libraries" / "ubiquitty-core"),
                                ("kit", self.consumer)):
            finished = self.run_in_consumer(sys.executable, str(self.library_scripts / "ubq_roots.py"), query)
            self.assertEqual(finished.stdout.strip(), str(expected.resolve()), finished.stderr)
        finished = self.run_in_consumer(sys.executable, str(self.library_scripts / "ubq_roots.py"), "repositories")
        self.assertEqual(finished.stdout.strip(), "kit")
        shell = self.run_in_consumer(
            "bash", "-c", '. "$1/ubq-roots.sh" && printf "%s|%s|%s" "$UBQ_SUBJECT_ROOT" "$UBQ_CORE_ROOT" "$UBQ_REPOSITORIES"',
            "_", str(self.library_scripts))
        self.assertEqual(shell.stdout.split("|")[1:], [str(self.library_scripts.parent.resolve()), "kit"], shell.stderr)

    def test_a_root_variable_beats_the_default(self):
        direct = subprocess.run([sys.executable, str(self.library_scripts / "ubq_roots.py"), "core"],
                                cwd=self.consumer, capture_output=True, text=True,
                                env=dict(os.environ, UBQ_CORE_ROOT="/nonexistent/core"))
        self.assertEqual(direct.stdout.strip(), "/nonexistent/core")

    def test_check_style_loads_the_consumers_table_with_the_librarys(self):
        finished = self.run_in_consumer(sys.executable, str(self.library_scripts / "check-style.py"), "--self-test")
        self.assertEqual(finished.returncode, 0, finished.stdout + finished.stderr)
        self.assertIn("each accounted for", finished.stdout)

    def test_the_library_runs_the_consumers_sections_over_the_consumers_tree(self):
        finished = self.run_in_consumer(sys.executable, str(self.library_scripts / "check-style.py"), "--full")
        self.assertNotEqual(finished.returncode, 2)
        output = finished.stdout + finished.stderr
        # The consumer is a few files, so some sections find too little to pass;
        # what is asserted is whose tree they read and whose table they run.
        self.assertIn("section(s) belong to a repository this checkout does not hold", output)
        self.assertIn("the consumer gate", output)
        self.assertIn("check-consumer: ok (fixture)", output)
        self.assertNotIn("Traceback", output)
        self.assertIn("format-code.sh: all 1 C-family", output)

    def test_hooks_install_for_the_consumer_and_gate_its_commits(self):
        finished = self.run_in_consumer("bash", str(self.library_scripts / "install-hooks.sh"))
        self.assertEqual(finished.returncode, 0, finished.stdout + finished.stderr)
        hooks = self.consumer / ".git" / "hooks"
        for name in ("pre-commit", "commit-msg", "pre-push"):
            self.assertTrue((hooks / name).is_file(), name)
        # The consumer is a few files, so gates whose subject is a whole
        # renderer or build graph find too little to pass; a commit passing
        # would say nothing here. What is asserted is that the library's body
        # ran, found every gate it names through the library, and judged the
        # consumer's staged file.
        (self.consumer / "Source" / "Hello.h").write_text(
            HEADER + "#pragma once\n\n/// Says hello.\n///\n/// - Returns: zero.\nint hello();\n\nint undocumented();\n",
            encoding="utf-8")
        git(self.consumer, "add", "Source/Hello.h")
        refused = git(self.consumer, "commit", "-q", "-m", "Add an undocumented declaration", check=False)
        output = refused.stdout + refused.stderr
        self.assertNotEqual(refused.returncode, 0, output)
        self.assertNotIn("names gate(s) your branch does not carry", output)
        self.assertIn("an interface was added without documentation", output)
        git(self.consumer, "reset", "-q", "--hard", "HEAD")

    def test_the_push_gates_run_from_the_consumers_own_list(self):
        git(self.consumer, "update-ref", "refs/remotes/origin/main", "HEAD")
        (self.consumer / "Scripts" / "pre-push-gates.sh").write_text(
            "#!/usr/bin/env bash\n"
            "source \"${UBQ_CORE_ROOT:-$(cd \"$(dirname \"$0\")/..\" && pwd)/Libraries/ubiquitty-core}/Scripts/pre-push-gates-lib.sh\"\n"
            "run_for \"core\" \"core only\" true\n"
            "run_for \"kit\" \"consumer gate\" python3 ./Scripts/check-consumer.py\n"
            "ubq_pre_push_finish\n", encoding="utf-8")
        os.chmod(self.consumer / "Scripts" / "pre-push-gates.sh", 0o755)
        finished = self.run_in_consumer(
            "bash", "Scripts/pre-push-gates.sh",
            environment={"UBIQUITTY_PREPUSH": "lint", "UBQ_BUILD_ROOT": str(Path(self.temporary.name) / "build")})
        output = finished.stdout + finished.stderr
        self.assertEqual(finished.returncode, 0, output)
        self.assertIn("1 gate(s) answer for a repository this checkout does not hold", output)
        self.assertIn("core only (core)", output)
        self.assertIn("1 passed", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
