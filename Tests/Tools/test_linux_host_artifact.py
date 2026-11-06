#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Reject host artifacts that cannot be deployed to the requested Linux machine."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("linux_host_artifact", ROOT / "Scripts/linux-host-artifact.py")
artifact = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(artifact)


def executable(machine=62, segment=1):
    data = bytearray(128)
    data[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HH", data, 16, 2, machine)
    struct.pack_into("<Q", data, 32, 64)
    struct.pack_into("<HH", data, 54, 56, 1)
    struct.pack_into("<I", data, 64, segment)
    return data


class LinuxHostArtifact(unittest.TestCase):
    def test_builder_reuses_a_verified_artifact_without_docker(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            commands = root / "bin"
            commands.mkdir()
            docker = commands / "docker"
            docker.write_text("""#!/usr/bin/env python3
import os
from pathlib import Path
import sys
args = sys.argv[1:]
if os.environ.get('UBQ_TEST_NO_DOCKER'):
    sys.exit('Docker must not run for a verified artifact')
if args[0] == 'info':
    sys.exit(0)
assert args[0] == 'run'
assert 'UBQ_BUILD_JOBS=2' in args
assert 'CMAKE_BUILD_PARALLEL_LEVEL=2' in args
mount = next(arg for arg in args if arg.endswith(':/out'))
binary = Path(mount[:-5]) / 'ubiquitty-host-linux-x86_64.pending'
binary.write_bytes(bytes.fromhex('""" + executable().hex() + """'))
""")
            docker.chmod(0o755)
            environment = dict(os.environ, PATH=f"{commands}:{os.environ['PATH']}",
                               UBQ_BUILD_ROOT=str(root / "build"), UBQ_BUILD_JOBS="2")
            command = ["bash", str(ROOT / "Scripts/build-host-linux.sh"), "x86_64"]
            first = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=60)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            environment["UBQ_TEST_NO_DOCKER"] = "1"
            second = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=60)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertIn("Linux host: verified", second.stdout)

    def test_static_architectures(self):
        artifact.verify_static_elf(executable(), "x86_64")
        artifact.verify_static_elf(executable(183), "arm64")
        with self.assertRaises(ValueError):
            artifact.verify_static_elf(executable(), "arm64")

    def test_dynamic_loader_is_refused(self):
        with self.assertRaisesRegex(ValueError, "dynamic loader"):
            artifact.verify_static_elf(executable(segment=3), "x86_64")

    def test_shared_dependency_without_interpreter_is_refused(self):
        data = executable(segment=2)
        struct.pack_into("<Q", data, 72, 112)
        struct.pack_into("<Q", data, 96, 16)
        struct.pack_into("<qQ", data, 112, 1, 0)
        with self.assertRaisesRegex(ValueError, "shared library"):
            artifact.verify_static_elf(data, "x86_64")

    def test_truncated_headers_and_segments_are_refused(self):
        for data in (b"", executable()[:64], executable()[:100]):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                artifact.verify_static_elf(data, "x86_64")
        data = executable()
        struct.pack_into("<Q", data, 96, 1024)
        with self.assertRaisesRegex(ValueError, "truncated"):
            artifact.verify_static_elf(data, "x86_64")

    def test_manifest_requires_current_inputs_and_exact_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "host"
            data = executable()
            binary.write_bytes(data)
            artifact.manifest_path(binary).write_text(json.dumps({
                "format": 1, "key": "current", "sha256": hashlib.sha256(data).hexdigest()}))
            artifact.verify_artifact(binary, "x86_64", "current")
            with self.assertRaises(ValueError):
                artifact.verify_artifact(binary, "x86_64", "different-source")
            data[-1] = 1
            binary.write_bytes(data)
            with self.assertRaises(ValueError):
                artifact.verify_artifact(binary, "x86_64", "current")

    def test_source_key_covers_scripts_resources_and_configuration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "VERSION").write_text("0.2.0\n")
            source = root / "Resources" / "shell-integration" / "zsh.sh"
            source.parent.mkdir(parents=True)
            source.write_text("first")
            key = lambda quic: artifact.source_key(root, "x86_64", "image", quic, "0.2.0", "commit")
            initial = key("ON")
            self.assertEqual(initial, key("ON"))
            self.assertNotEqual(initial, key("OFF"))
            source.write_text("second")
            self.assertNotEqual(initial, key("ON"))
            initial = key("ON")
            module = root / "cmake" / "LicenseNotices.cmake"
            module.parent.mkdir()
            module.write_text("set(NOTICES_ENABLED ON)\n")
            self.assertNotEqual(initial, key("ON"))


if __name__ == "__main__":
    unittest.main()
