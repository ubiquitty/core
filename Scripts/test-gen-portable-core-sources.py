#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercises the portable source list generator's locale independence."""

import os
from pathlib import Path
import subprocess
import unittest

SCRIPTS = Path(__file__).resolve().parent


class SourceListTests(unittest.TestCase):
    def testTheGeneratorPinsTheByteOrderLocaleBeforeItSorts(self):
        lines = (SCRIPTS / "gen-portable-core-sources.sh").read_text().splitlines()
        code = [line for line in lines if line.strip() and not line.lstrip().startswith("#")]
        pinned = next(index for index, line in enumerate(code) if line.strip() == "export LC_ALL=C")
        sorted_at = next(index for index, line in enumerate(code) if "sort" in line)
        self.assertLess(pinned, sorted_at)

    def testTheGeneratorAgreesWithItselfUnderAnotherLocale(self):
        results = set()
        for locale in ("C", "en_US.UTF-8"):
            environment = dict(os.environ, LANG=locale)
            environment.pop("LC_ALL", None)
            finished = subprocess.run(["bash", str(SCRIPTS / "gen-portable-core-sources.sh"), "--print"],
                                      env=environment, capture_output=True, text=True)
            self.assertEqual(finished.returncode, 0, finished.stderr)
            results.add(finished.stdout)
        self.assertEqual(len(results), 1)


if __name__ == "__main__":
    unittest.main()
