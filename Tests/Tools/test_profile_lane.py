#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercise the profile report against small xctrace exports, without Instruments."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def export(rows, stack="tagged-backtrace", weight="cycle-weight"):
    columns = '<col><mnemonic>stack</mnemonic><engineering-type>' + stack + '</engineering-type></col>'
    if weight:
        columns = '<col><mnemonic>weight</mnemonic><engineering-type>' + weight + '</engineering-type></col>' + columns
    return '<trace-query-result><node><schema name="cpu-profile">' + columns + '</schema>' + rows + '</node></trace-query-result>'


# Four samples but 100 cycles: reused stacks and weights make hot 40%, cold
# 60%. Counting stack definitions gives two samples, and counting rows without
# weights gives 50/50. Recursive caller frames must still count once per row.
WEIGHTED = export('''
<row><cycle-weight id="1">10</cycle-weight><tagged-backtrace id="2">
  <frame name="Ubiquitty::hot(int&amp;)" addr="0x100100011" id="3">
    <binary id="4" name="bench" load-addr="0x100100000"/>
  </frame><frame id="5" name="Ubiquitty::caller()" addr="0x100100021"/><frame ref="5"/>
</tagged-backtrace></row>
<row><cycle-weight id="6">30</cycle-weight><tagged-backtrace ref="2"/></row>
<row><cycle-weight ref="6"/><tagged-backtrace id="7">
  <frame ref="8"/><frame ref="5"/>
</tagged-backtrace></row>
<row><cycle-weight ref="6"/><tagged-backtrace id="9">
  <frame id="8" name="Ubiquitty::cold()" addr="0x100100015"/><frame ref="5"/>
</tagged-backtrace></row>
''')

LEGACY = export('''
<row><backtrace id="1"><frame id="2" name="Ubiquitty::first()"/>
<frame id="3" name="Ubiquitty::caller()"/></backtrace></row>
<row><backtrace ref="1"/></row>
<row><backtrace id="4"><frame id="5" name="Ubiquitty::second()"/>
<frame ref="3"/></backtrace></row>
''', stack="backtrace", weight=None)


class ProfileLaneTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ubq-profile-report-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.installTool("xcrun", '''import os, pathlib, sys
if sys.argv[1:] == ["-f", "xctrace"]:
    print(__file__)
elif sys.argv[1:3] == ["xctrace", "record"]:
    pass
elif sys.argv[1:3] == ["xctrace", "export"]:
    key = "UBQ_PROFILE_TEST_CPU" if "cpu-profile" in sys.argv[-1] else "UBQ_PROFILE_TEST_TIME"
    print(pathlib.Path(os.environ[key]).read_text())
else:
    sys.exit(1)
''')
        self.installTool("nm", 'print("0000000100000010 T _Hot")\n')
        self.installTool("c++filt", 'import sys\nprint(sys.stdin.read().replace("_Hot", "Ubiquitty::hot(int&)"))\n')
        self.installTool("objdump", 'print("100000010: add x0, x0, #1\\n100000014: ret")\n')
        self.bench = self.installTool("bench", 'print(\'{"benchmarks": [{"name": "parse_ascii"}]}\')\n')

    def installTool(self, name, source):
        path = self.tools / name
        path.write_text("#!/usr/bin/env python3\n" + source)
        path.chmod(0o700)
        return path

    def report(self, cpu, time="<trace-query-result/>", symbol=None):
        cpuFile, timeFile = self.root / "cpu.xml", self.root / "time.xml"
        cpuFile.write_text(cpu)
        timeFile.write_text(time)
        environment = dict(os.environ, PATH=str(self.tools) + os.pathsep + os.environ.get("PATH", ""),
                           UBQ_BENCH=str(self.bench), UBQ_BUILD_ROOT=str(self.root / "build"),
                           UBQ_BENCH_ARGS="", TMPDIR=str(self.root),
                           UBQ_PROFILE_TEST_CPU=str(cpuFile), UBQ_PROFILE_TEST_TIME=str(timeFile))
        arguments = ["bash", str(ROOT / "Scripts/profile-lane.sh"), "parse"]
        if symbol:
            arguments.extend(["--symbol", symbol])
        return subprocess.run(arguments, env=environment, capture_output=True, text=True, timeout=10)

    def testTaggedBacktracesAndInternedCycleWeightsAreCountedPerRow(self):
        result = self.report(WEIGHTED)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("samples      4 stacks", result.stdout)
        self.assertIn("weight       100 cycle-weight", result.stdout)
        self.assertRegex(result.stdout, r"40\.0%\s+40\.0%\s+hot\n")
        self.assertRegex(result.stdout, r"60\.0%\s+60\.0%\s+cold\n")
        self.assertRegex(result.stdout, r"0\.0%\s+100\.0%\s+caller\n")
        self.assertIn("not elapsed wall time", result.stdout)

    def testLegacyBacktracesRemainSupportedThroughSchemaFallback(self):
        result = self.report("<trace-query-result/>", time=LEGACY)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("samples      3 stacks", result.stdout)
        self.assertIn("weight       3 sample-count", result.stdout)
        self.assertRegex(result.stdout, r"66\.7%\s+66\.7%\s+first\n")
        self.assertRegex(result.stdout, r"33\.3%\s+33\.3%\s+second\n")

    def testLegacyNumericWeightsKeepTheirUnits(self):
        fixture = export('''
<row><weight id="1">1</weight><backtrace id="2"><frame id="3" name="Ubiquitty::first()"/></backtrace></row>
<row><weight id="4">3</weight><backtrace id="5"><frame id="6" name="Ubiquitty::second()"/></backtrace></row>
''', stack="backtrace", weight="weight")
        result = self.report(fixture)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("weight       4 weight", result.stdout)
        self.assertRegex(result.stdout, r"25\.0%\s+25\.0%\s+first\n")
        self.assertRegex(result.stdout, r"75\.0%\s+75\.0%\s+second\n")

    def testInstructionAttributionUsesTheSameWeights(self):
        result = self.report(WEIGHTED, symbol="hot")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("40 of 100 cycle-weight, 40.0%", result.stdout)
        self.assertRegex(result.stdout, r"40\s+\+0\s+add x0, x0, #1")
        self.assertNotIn("landed outside", result.stdout)

    def testMissingStacksRemainInTheWeightedDenominator(self):
        fixture = export('''
<row><cycle-weight id="1">10</cycle-weight><tagged-backtrace id="2"><frame id="3" name="Ubiquitty::hot()"/></tagged-backtrace></row>
<row><cycle-weight ref="1"/><tagged-backtrace id="4"/></row>
''')
        result = self.report(fixture)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("samples      2 stacks", result.stdout)
        self.assertRegex(result.stdout, r"50\.0%\s+50\.0%\s+hot\n")
        self.assertRegex(result.stdout, r"50\.0%\s+50\.0%\s+<no stack>\n")

    def testMalformedReferencesAndIncompatibleWeightsFail(self):
        cases = (
            (WEIGHTED.replace('<tagged-backtrace ref="2"/>', '<tagged-backtrace ref="99"/>'), "unresolved XML reference 99"),
            (WEIGHTED.replace('<tagged-backtrace ref="2"/>', '<tagged-backtrace id="10" ref="10"/>'), "cyclic XML reference 10"),
            (WEIGHTED.replace('<cycle-weight id="6">30</cycle-weight>', '<weight id="6">30</weight>'), "incompatible sample weights"),
        )
        for fixture, message in cases:
            with self.subTest(message=message):
                result = self.report(fixture)
                self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
                self.assertIn(message, result.stderr)
                self.assertNotIn("self%", result.stdout)

    def testAnEmptyProfileCannotLookLikeAZeroCostLane(self):
        result = self.report(export('<row><tagged-backtrace id="1"/></row>', weight=None))
        self.assertEqual(result.returncode, 4, result.stdout + result.stderr)
        self.assertIn("no stacks", result.stderr)
        self.assertNotIn("self%", result.stdout)


if __name__ == "__main__":
    unittest.main()
