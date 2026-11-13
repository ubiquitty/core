#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Bounded archive compiles, failure cleanup, and incremental dependencies."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PortableCoreBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ubq-archive-workers-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        scripts = self.root / "Scripts"
        scripts.mkdir()
        shutil.copyfile(Path(__file__).with_name("portable-core-lib.sh"), scripts / "portable-core-lib.sh")
        (scripts / "embed-builtin-themes.py").write_text("import pathlib,sys\npathlib.Path(sys.argv[2]).write_text('themes')\n")
        self.units = [f"Source/unit{i}.cpp" for i in range(7)]
        generator = scripts / "gen-portable-core-sources.sh"
        generator.write_text("#!/bin/sh\nprintf '%s\\n' " + " ".join(self.units) + "\n")
        generator.chmod(0o755)
        for name in (*self.units, "Source/header.h", "Source/ThirdParty/simdutf/simdutf.cpp",
                     "Source/ThirdParty/re2/re2/pattern.cc", "Source/ThirdParty/re2/util/rune.cc",
                     "Source/ThirdParty/re2/util/strutil.cc", "Source/ThirdParty/yyjson/yyjson.c"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        binary = self.root / "bin"
        binary.mkdir()
        compiler = binary / "compiler"
        compiler.write_text('''#!/usr/bin/env python3
import fcntl,json,os,pathlib,sys,time
root=pathlib.Path(os.environ['CORE_FIXTURE_ROOT'])
args=sys.argv[1:]; obj=pathlib.Path(args[args.index('-o')+1]); source=args[args.index('-c')+1]
def record(delta):
    with (root/'state').open('a+') as f:
        fcntl.flock(f,fcntl.LOCK_EX); f.seek(0); text=f.read()
        state=json.loads(text) if text else {'active':0,'maximum':0,'started':[]}
        state['active']+=delta; state['maximum']=max(state['maximum'],state['active'])
        if delta>0:state['started'].append(source)
        f.seek(0);f.truncate();json.dump(state,f)
record(1)
try:
    time.sleep(.03)
    obj.write_text('object')
    if '-MF' in args:
        pathlib.Path(args[args.index('-MF')+1]).write_text(str(obj)+': '+source+' '+str(root/'Source/header.h')+'\\n')
    if pathlib.Path(source).name==os.environ.get('CORE_FIXTURE_FAIL'):sys.exit(4)
finally:record(-1)
''')
        compiler.chmod(0o755)
        archiver = binary / "ar"
        archiver.write_text('''#!/usr/bin/env python3
import json,pathlib,sys
assert all(pathlib.Path(x).is_file() for x in sys.argv[3:])
pathlib.Path(sys.argv[2]).write_text(json.dumps(sys.argv[3:]))
''')
        archiver.chmod(0o755)
        self.environment = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ["PATH"],
                                ROOT=str(self.root), CXX=str(compiler), CC=str(compiler),
                                CORE_FIXTURE_ROOT=str(self.root), UBQ_COMPILER_LAUNCHER="")
        self.build = self.root / "build"

    def build_archive(self, jobs="3", failure=""):
        environment = dict(self.environment, UBQ_BUILD_JOBS=jobs, CORE_FIXTURE_FAIL=failure)
        return subprocess.run(["bash", "-euc", 'source "$ROOT/Scripts/portable-core-lib.sh"; build_portable_core_archive "$ROOT/build" "-O3"'],
                              env=environment, capture_output=True, text=True, timeout=15)

    def state(self):
        return json.loads((self.root / "state").read_text())

    def test_workers_are_bounded_and_archive_order_is_stable(self):
        result = self.build_archive()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreater(self.state()["maximum"], 1)
        self.assertLessEqual(self.state()["maximum"], 3)
        self.assertEqual(self.state()["active"], 0)
        members = json.loads((self.build / "libubiquitty-core.a").read_text())
        self.assertEqual([Path(x).name for x in members[:7]], [x.replace("/", "_") + ".o" for x in self.units])

    def test_failure_reaps_peers_and_removes_partial_object_and_dependency(self):
        result = self.build_archive(failure="unit1.cpp")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.state()["active"], 0)
        self.assertFalse((self.build / "libubiquitty-core.a").exists())
        for suffix in (".o", ".o.d"):
            self.assertFalse((self.build / "portable-core-obj" / ("Source_unit1.cpp" + suffix)).exists())

    def test_incremental_compiles_observe_header_dependencies(self):
        self.assertEqual(self.build_archive().returncode, 0)
        initial = sum(Path(x).name.startswith("unit") for x in self.state()["started"])
        self.assertEqual(self.build_archive().returncode, 0)
        self.assertEqual(sum(Path(x).name.startswith("unit") for x in self.state()["started"]), initial)
        header = self.root / "Source/header.h"
        future = header.stat().st_mtime + 10
        os.utime(header, (future, future))
        self.assertEqual(self.build_archive().returncode, 0)
        self.assertEqual(sum(Path(x).name.startswith("unit") for x in self.state()["started"]), initial * 2)

    def test_one_job_and_invalid_job_count(self):
        self.assertEqual(self.build_archive(jobs="1").returncode, 0)
        self.assertEqual(self.state()["maximum"], 1)
        self.assertNotEqual(self.build_archive(jobs="0").returncode, 0)


if __name__ == "__main__":
    unittest.main()
