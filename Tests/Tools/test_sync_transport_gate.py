#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Checks transport-runner isolation and child cleanup without starting processes."""

import importlib.util
import io
from pathlib import Path
import signal
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Scripts"))
SPEC = importlib.util.spec_from_file_location("sync_transport_gate", ROOT / "Scripts/check-sync-transport.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class OwnedProcessTests(unittest.TestCase):
    def setUp(self):
        self.process = mock.Mock(pid=12345, returncode=0)
        self.process.communicate.return_value = (b"output", b"")
        self.process.wait.return_value = 0
        self.addCleanup(mock.patch.stopall)
        self.creation = mock.patch.object(gate.subprocess, "Popen", return_value=self.process).start()
        self.disposition = mock.patch.object(gate.signal, "getsignal", return_value=signal.SIG_DFL).start()
        self.wait = mock.patch.object(gate.os, "waitid", return_value=None).start()
        self.kill = mock.patch.object(gate.os, "kill").start()

    def testNoChildStartsWithAnUnownedSignalDisposition(self):
        self.disposition.return_value = signal.SIG_IGN
        with self.assertRaises(gate.Refusal):
            gate.OwnedProcess(["fixture"])
        self.creation.assert_not_called()

    def testSignalRequiresAnExactUnreapedDirectChild(self):
        child = gate.OwnedProcess(["fixture"])
        self.assertTrue(child.signal(signal.SIGTERM))
        self.wait.assert_called_once_with(gate.os.P_PID, 12345, gate.os.WEXITED | gate.os.WNOHANG | gate.os.WNOWAIT)
        self.kill.assert_called_once_with(12345, signal.SIGTERM)
        self.assertTrue(child.owned)

    def testAnExitedChildIsReapedInsteadOfSignalled(self):
        child = gate.OwnedProcess(["fixture"])
        self.wait.return_value = SimpleNamespace(si_pid=12345)
        self.assertFalse(child.signal(signal.SIGKILL))
        self.process.wait.assert_called_once_with(timeout=1)
        self.kill.assert_not_called()
        self.assertFalse(child.owned)

    def testInterruptedVerificationRetriesBeforeAnySignal(self):
        child = gate.OwnedProcess(["fixture"])
        self.wait.side_effect = [InterruptedError(), None]
        self.assertTrue(child.signal(signal.SIGTERM))
        self.assertEqual(self.wait.call_count, 2)
        self.kill.assert_called_once_with(12345, signal.SIGTERM)
        self.assertTrue(child.owned)

    def testLostReapingAuthorityNeverSignalsAReportedPID(self):
        child = gate.OwnedProcess(["fixture"])
        self.wait.side_effect = ChildProcessError()
        self.assertFalse(child.signal(signal.SIGKILL))
        self.assertFalse(child.owned)
        self.kill.assert_not_called()

    def testChangedDispositionPermanentlyRetiresOwnership(self):
        child = gate.OwnedProcess(["fixture"])
        self.disposition.return_value = signal.SIG_IGN
        with self.assertRaises(gate.Refusal):
            child.close()
        self.assertFalse(child.owned)
        self.disposition.return_value = signal.SIG_DFL
        child.close()
        self.wait.assert_not_called()
        self.kill.assert_not_called()

    def testSmallOrInvalidPIDNeverBecomesAGroupSignal(self):
        for process_id in (-1, 0, 1):
            with self.subTest(process_id=process_id):
                self.process.pid = process_id
                child = gate.OwnedProcess(["fixture"])
                self.assertFalse(child.signal(signal.SIGKILL))
        self.kill.assert_not_called()

    def testCommunicationReapsAndPreventsLaterSignals(self):
        child = gate.OwnedProcess(["fixture"])
        self.assertEqual(child.communicate(timeout=7), (b"output", b""))
        child.close()
        self.wait.assert_not_called()
        self.kill.assert_not_called()

    def testTimeoutEscalatesOnlyTheSameVerifiedChild(self):
        child = gate.OwnedProcess(["fixture"])
        self.process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 3), 0]
        child.close()
        self.assertEqual(self.kill.call_args_list, [mock.call(12345, signal.SIGTERM), mock.call(12345, signal.SIGKILL)])
        self.assertEqual(self.wait.call_count, 2)
        self.assertFalse(child.owned)

    def testAChildReapedDuringTheGraceWindowIsNotEscalated(self):
        child = gate.OwnedProcess(["fixture"])
        self.process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 3), 0]
        self.wait.side_effect = [None, SimpleNamespace(si_pid=12345)]
        child.close()
        self.kill.assert_called_once_with(12345, signal.SIGTERM)
        self.assertFalse(child.owned)


class TransportGateTests(unittest.TestCase):
    def setUp(self):
        self.addCleanup(mock.patch.stopall)
        self.output = io.StringIO()
        mock.patch("sys.stdout", self.output).start()
        mock.patch("sys.stderr", self.output).start()
        self.executable = mock.patch.object(gate, "executable", side_effect=lambda value: Path(value)).start()
        self.ssh_identity_function = gate.ssh_identity
        self.identity = mock.patch.object(gate, "ssh_identity", return_value=SimpleNamespace(pw_uid=1234)).start()
        self.run = mock.patch.object(gate, "run_carrier").start()
        mock.patch.object(gate.signal, "getsignal", return_value=signal.SIG_DFL).start()
        self.signal_install = mock.patch.object(gate.signal, "signal").start()

    def testDefaultRequiresBothRealCarriers(self):
        self.assertEqual(gate.main(["--binary", "/fixture"]), 0)
        self.assertEqual([call.args[1] for call in self.run.call_args_list], ["ssh", "quic"])
        self.assertIn("Real SSH and QUIC acceptance passed", self.output.getvalue())
        self.signal_install.assert_called_once_with(signal.SIGCHLD, signal.SIG_DFL)

    def testQUICOnlyDoesNotClaimSSHCoverage(self):
        self.assertEqual(gate.main(["--binary", "/fixture", "--transport", "quic"]), 0)
        self.identity.assert_not_called()
        self.run.assert_called_once_with(Path("/fixture"), "quic", None, None)
        self.assertIn("Real QUIC acceptance passed", self.output.getvalue())
        self.assertNotIn("SSH acceptance passed", self.output.getvalue())

    def testUnisolatedSSHIsRefusedBeforeAnyServerStarts(self):
        self.identity.side_effect = gate.Refusal("No disposable SSH account")
        self.assertEqual(gate.main(["--binary", "/fixture"]), 2)
        self.run.assert_not_called()
        self.assertNotIn("acceptance passed", self.output.getvalue())

    def testCarrierFailureIsNotReportedAsARefusalOrSuccess(self):
        self.run.side_effect = RuntimeError("SSH checkpoint missing")
        self.assertEqual(gate.main(["--binary", "/fixture"]), 1)
        self.assertEqual(self.run.call_count, 1)
        self.assertNotIn("acceptance passed", self.output.getvalue())

    def testACompletedSSHCaseCannotHideAQUICFailure(self):
        self.run.side_effect = [None, RuntimeError("QUIC handshake refused")]
        self.assertEqual(gate.main(["--binary", "/fixture"]), 1)
        self.assertEqual(self.run.call_count, 2)
        self.assertNotIn("acceptance passed", self.output.getvalue())

    def testLocalMacOSAccountIsNeverAnSSHFixture(self):
        with mock.patch.object(gate.sys, "platform", "darwin"):
            with self.assertRaises(gate.Refusal):
                self.ssh_identity_function()

    def testHostNonrootLinuxAccountIsNeverAnSSHFixture(self):
        with mock.patch.object(gate.sys, "platform", "linux"), mock.patch.object(gate.os, "geteuid", return_value=501):
            with self.assertRaises(gate.Refusal):
                self.ssh_identity_function()

    def testEnvironmentNeverInheritsDeveloperCredentialsOrConfiguration(self):
        environment = gate.isolated_environment(Path("/tmp/fixture"), Path("/tmp/fixture/peer.sock"))
        self.assertEqual(environment["UBIQUITTY_HOME"], "/tmp/fixture/host")
        self.assertEqual(environment["XDG_CONFIG_HOME"], "/tmp/fixture/config")
        self.assertNotIn("HOME", environment)
        self.assertNotIn("SSH_AUTH_SOCK", environment)
        self.assertNotIn("SSH_AGENT_PID", environment)
        self.assertNotIn("TMUX", environment)

    def testPinnedSocketLeafMatchesTheProductionGrammarAndNativeFixture(self):
        self.assertRegex(gate.SOCKET_LEAF, r"^host-[0-9a-f]{16}\.sock$")
        source = (ROOT / "Tests/Host/sync_transport_acceptance.cpp").read_text()
        self.assertIn(f'constexpr std::string_view socketLeaf = "{gate.SOCKET_LEAF}";', source)

    def testSSHArtifactsStayBelowTheVerifiedDisposableHome(self):
        identity = SimpleNamespace(pw_dir="/home/ubq-transport")
        self.assertEqual(gate.temporary_parent("ssh", identity), "/home/ubq-transport")
        self.assertEqual(gate.temporary_parent("quic", identity), "/tmp")
        self.assertEqual(gate.temporary_parent("quic", None), "/tmp")

    def testSSHArtifactsRefuseAnUnverifiedOrMissingHome(self):
        for identity in (None, SimpleNamespace(pw_dir="/tmp")):
            with self.subTest(identity=identity), self.assertRaises(gate.Refusal):
                gate.temporary_parent("ssh", identity)

    def testSSHFailureDiagnosticsAreBoundedAndStripTerminalControls(self):
        path = mock.Mock()
        path.open.return_value = io.BytesIO(b"x" * 5000 + b"\n\x1b[31mAuthentication refused\n")
        gate.report_ssh_failure(path)
        path.open.assert_called_once_with("rb")
        self.assertIn("Authentication refused", self.output.getvalue())
        self.assertNotIn("\x1b", self.output.getvalue())
        self.assertLess(len(self.output.getvalue()), 4200)

    def testUnavailableSSHFailureDiagnosticsCannotHideTheOriginalFailure(self):
        path = mock.Mock()
        path.open.side_effect = OSError("unavailable")
        gate.report_ssh_failure(path)
        self.assertEqual(self.output.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
