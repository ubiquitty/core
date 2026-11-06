#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Memory-only freshness diagnostics with mocked Git and filesystem metadata."""

import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("coverage_index_freshness", ROOT / "Scripts/coverage_index_freshness.py")
freshness = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = freshness
SPEC.loader.exec_module(freshness)


class CoverageIndexFreshnessTests(unittest.TestCase):
    def index(self, rows="Source/Core/One.cpp\ttest_one.2\n", revision="abcd1234"):
        return freshness.parseIndex(f"# {revision}, 1 tests\n" + rows)

    def assess(self, index=None, missing=(), changed=(), metadata=()):
        return freshness.assessIndex(index or self.index(),
                                     lambda path: "Indexed file is missing" if path in missing else None,
                                     changed, metadata)

    def testKnownIndexWithExistingPathsIsStructurallyFresh(self):
        self.assertEqual(self.assess(), ("fresh", ()))

    def testMissingSourceIsStaleEvenWhenProvenanceIsUnavailable(self):
        verdict, problems = self.assess(missing={"Source/Core/One.cpp"}, metadata=("Commit unavailable.",))
        self.assertEqual(verdict, "stale")
        self.assertIn("Source/Core/One.cpp", problems[0])
        self.assertIn("Commit unavailable.", problems)

    def testRenamedIndexedTestIsStaleEvenWhenItsEntryIsNamed(self):
        index = self.index("Tests/Old/test_one.cpp\ttest_one\n")
        verdict, problems = self.assess(index, missing={"Tests/Old/test_one.cpp"},
                                        changed={"Tests/New/test_one.cpp"})
        self.assertEqual(verdict, "stale")
        self.assertEqual(len(problems), 1)

    def testAddedRenamedAndUntrackedTestsAreAllCandidates(self):
        for path in ("Tests/Core/test_added.cpp", "Tests/Renamed/test_renamed.cpp", "Tests/new/fuzz_added.sh"):
            with self.subTest(path=path):
                verdict, problems = self.assess(changed={path})
                self.assertEqual(verdict, "stale")
                self.assertIn(path, problems[0])

    def testShardNamesMatchExactlyRatherThanAsRegexOrPrefix(self):
        self.assertEqual(self.assess(changed={"Tests/Core/test_one.cpp"}), ("fresh", ()))
        self.assertEqual(self.assess(changed={"Tests/Core/test_on.cpp"})[0], "stale")
        self.assertEqual(self.assess(changed={"Tests/Core/test_one_extra.cpp"})[0], "stale")

    def testDocumentationAndUninstrumentedToolTestsDoNotInventCoverageEntries(self):
        self.assertEqual(self.assess(changed={"Tests/README.md", "Tests/Tools/test_one.py"}), ("fresh", ()))

    def testUnclassifiedNativeAndShellTestSourcesCannotDeclareFresh(self):
        for path in ("Tests/Host/new_case.cpp", "Tests/Platform/new_case.mm", "Tests/Platform/new_case.c",
                     "Tests/Platform/new_case.swift", "Tests/test-new-case.sh"):
            with self.subTest(path=path):
                verdict, problems = self.assess(changed={path})
                self.assertEqual(verdict, "UNCHECKED")
                self.assertIn("no indexed attribution", problems[0])

    def testOwnIndexedRowPreservesRealShellEntryAttribution(self):
        for path, entry in (("Tests/Host/new_case.cpp", "shell_actual_stanza"),
                            ("Tests/Platform/test_serial.cpp", "shell_test_serial")):
            with self.subTest(path=path):
                index = self.index(f"{path}\t{entry}\n")
                self.assertEqual(self.assess(index, changed={path}), ("fresh", ()))

    def testMissingIndexedFileTakesPrecedenceOverUnclassifiedNewSource(self):
        verdict, problems = self.assess(missing={"Source/Core/One.cpp"}, changed={"Tests/Host/new_case.cpp"})
        self.assertEqual(verdict, "stale")
        self.assertIn("missing", problems[0])
        self.assertIn("no indexed attribution", problems[1])

    def testAnIndexFarBehindHeadIsStaleEvenWhenEveryPathResolves(self):
        limit = freshness.MAX_COMMITS_BEHIND
        self.assertEqual(freshness.assessIndex(self.index(), lambda path: None, behind=limit), ("fresh", ()))
        verdict, problems = freshness.assessIndex(self.index(), lambda path: None, behind=limit + 1)
        self.assertEqual(verdict, "stale")
        self.assertIn(f"{limit + 1} commits behind HEAD", problems[0])

    def testCommitsBehindReadsRevListAndDeclinesWhatItCannotMeasure(self):
        run = mock.Mock(return_value=SimpleNamespace(returncode=0, stdout=b"1580\n"))
        self.assertEqual(freshness.commitsBehind(Path("/checkout"), "abcd1234", run), 1580)
        self.assertIn("abcd1234..HEAD", run.call_args.args[0])
        self.assertIsNone(freshness.commitsBehind(Path("/checkout"), None, run))
        failed = mock.Mock(return_value=SimpleNamespace(returncode=128, stdout=b""))
        self.assertIsNone(freshness.commitsBehind(Path("/checkout"), "abcd1234", failed))
        junk = mock.Mock(return_value=SimpleNamespace(returncode=0, stdout=b"--exec\n"))
        self.assertIsNone(freshness.commitsBehind(Path("/checkout"), "abcd1234", junk))

    def testUnknownProvenanceAndFailedMetadataAreNeverFresh(self):
        self.assertEqual(self.assess(self.index(revision="unknown"))[0], "UNCHECKED")
        for problem in ("Commit unavailable.", "Git diff failed.", "Untracked listing failed."):
            with self.subTest(problem=problem):
                self.assertEqual(self.assess(metadata=(problem,)), ("UNCHECKED", (problem,)))

    def testUnsafeIndexedPathsAreRejectedBeforeFilesystemLookup(self):
        for path in ("/etc/passwd", "Source/../secret", "Tests/../../secret", "Source//One.cpp",
                     "Source/./One.cpp", "Source/One.cpp/", "Source\\One.cpp", "C:/One.cpp",
                     "Other/One.cpp", "Source/\x1bOne.cpp", "Source/One\x00.cpp"):
            with self.subTest(path=path):
                lookup = mock.Mock(side_effect=AssertionError("unsafe filesystem lookup"))
                index = self.index(f"{path}\ttest_one\n")
                verdict, problems = freshness.assessIndex(index, lookup)
                self.assertEqual(verdict, "stale")
                self.assertTrue(any("unsafe indexed path" in problem for problem in problems))
                lookup.assert_not_called()

    def testMalformedAndDuplicateRowsCannotDeclareFresh(self):
        for rows in ("Source/Core/One.cpp\n", "Source/Core/One.cpp\t\n",
                     "Source/Core/One.cpp\ttest_one\textra\n", "Source/Core/One.cpp\ttest_one  test_two\n",
                     "Source/Core/One.cpp\ttest_one\nSource/Core/One.cpp\ttest_two\n", ""):
            with self.subTest(rows=rows):
                self.assertEqual(self.assess(self.index(rows))[0], "stale")

    def testMalformedHeaderIsNotPassedToGitAsARevision(self):
        index = freshness.parseIndex("# --exec=bad, 1 tests\nSource/Core/One.cpp\ttest_one\n")
        self.assertIsNone(index.revision)
        self.assertEqual(self.assess(index)[0], "stale")
        run = self.gitRunner()
        freshness.readGitMetadata(Path("/checkout"), index.revision, run)
        self.assertFalse(any("rev-parse" in call.args[0] for call in run.call_args_list))

    def testUnsafeGitPathsCannotBeSilentlyIgnoredAsNonTests(self):
        for path in ("/outside/test_one.cpp", "Tests/../../test_one.cpp", "Tests/test_one\x1b.cpp"):
            with self.subTest(path=path):
                verdict, problems = self.assess(changed={path})
                self.assertEqual(verdict, "stale")
                self.assertIn("unsafe test path", problems[0])

    def testEscapingSymlinkIsRejectedBeforeStatOfItsTarget(self):
        root = Path("/checkout")
        with mock.patch.object(freshness.Path, "resolve", return_value=Path("/outside/One.cpp")), \
                mock.patch.object(freshness.Path, "is_file", side_effect=AssertionError("outside stat")):
            self.assertIn("outside", freshness.indexedFileProblem(root, "Source/linked/One.cpp"))

    def testUnsafeFileLookupIsRefusedWithoutResolvingThePath(self):
        with mock.patch.object(freshness.Path, "resolve", side_effect=AssertionError("unsafe resolution")):
            self.assertIn("Unsafe", freshness.indexedFileProblem(Path("/checkout"), "Tests/../../outside"))

    def testRegularIndexedFileWithinTheCheckoutPasses(self):
        root = Path("/checkout")
        with mock.patch.object(freshness.Path, "resolve", return_value=root / "Source/One.cpp"), \
                mock.patch.object(freshness.Path, "is_file", return_value=True):
            self.assertIsNone(freshness.indexedFileProblem(root, "Source/One.cpp"))

    def testMissingAndUninspectableIndexedFilesDoNotPass(self):
        root = Path("/checkout")
        with mock.patch.object(freshness.Path, "resolve", return_value=root / "Source/One.cpp"), \
                mock.patch.object(freshness.Path, "is_file", return_value=False):
            self.assertIn("missing", freshness.indexedFileProblem(root, "Source/One.cpp"))
        with mock.patch.object(freshness.Path, "resolve", side_effect=RuntimeError("symlink loop")):
            self.assertIn("cannot be inspected", freshness.indexedFileProblem(root, "Source/One.cpp"))

    def testGitNulRecordsPreserveSpacesAndUseRenameDestination(self):
        data = b"A\0Tests/with spaces/test_added.cpp\0R100\0Tests/old/test_old.cpp\0Tests/new/test_new.cpp\0"
        self.assertEqual(freshness.parseChangedPaths(data),
                         {"Tests/with spaces/test_added.cpp", "Tests/new/test_new.cpp"})
        self.assertEqual(freshness.parseUntrackedPaths(b"Tests/with spaces/test_added.cpp\0"),
                         {"Tests/with spaces/test_added.cpp"})

    def testMalformedGitRecordsAreRefused(self):
        for data in (b"A\0missing-terminator", b"R100\0only-old\0", b"M\0Tests/test_one.cpp\0",
                     b"A\0\0", b"R100\0Tests/test_one.cpp\0\0"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                freshness.parseChangedPaths(data)
        for data in (b"missing-terminator", b"Tests/test_one.cpp\0\0"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                freshness.parseUntrackedPaths(data)

    def gitRunner(self, revisionStatus=0, diffStatus=0, untrackedStatus=0):
        def run(arguments, **options):
            self.assertEqual(arguments[:5], ["git", "--no-optional-locks", "-c", "core.fsmonitor=false", "-C"])
            self.assertIs(options["stdout"], subprocess.PIPE)
            self.assertIs(options["stderr"], subprocess.DEVNULL)
            self.assertFalse(options["check"])
            command = arguments[6:]
            if command[0] == "rev-parse":
                return SimpleNamespace(returncode=revisionStatus, stdout=b"")
            if command[0] == "diff":
                self.assertIn("--no-ext-diff", command)
                self.assertIn("--no-textconv", command)
                self.assertEqual(command[-2:], ["--", "Tests"])
                return SimpleNamespace(returncode=diffStatus,
                                       stdout=b"A\0Tests/test_committed.cpp\0R100\0Tests/test_old.cpp\0Tests/test_renamed.cpp\0")
            if command[0] == "ls-files":
                return SimpleNamespace(returncode=untrackedStatus, stdout=b"Tests/test_untracked.cpp\0")
            raise AssertionError(arguments)
        return mock.Mock(side_effect=run)

    def testGitDiffComparesProvenanceDirectlyToTheWorktree(self):
        run = self.gitRunner()
        paths, problems = freshness.readGitMetadata(Path("/checkout"), "abcd1234", run)
        self.assertFalse(problems)
        self.assertEqual(paths, {"Tests/test_committed.cpp", "Tests/test_renamed.cpp", "Tests/test_untracked.cpp"})
        diff = run.call_args_list[1].args[0]
        self.assertEqual(diff[-3:], ["abcd1234", "--", "Tests"])
        self.assertNotIn("HEAD", diff)

    def testUnavailableProvenanceStillChecksWorktreeAndUntrackedTests(self):
        for revision, status in (("abcd1234", 1), (None, 0)):
            with self.subTest(revision=revision):
                run = self.gitRunner(revisionStatus=status)
                paths, problems = freshness.readGitMetadata(Path("/checkout"), revision, run)
                self.assertEqual(len(problems), 1)
                self.assertIn("Tests/test_untracked.cpp", paths)
                diff = next(call.args[0] for call in run.call_args_list if "diff" in call.args[0])
                self.assertEqual(diff[-3:], ["HEAD", "--", "Tests"])
                self.assertEqual(self.assess(changed=paths, metadata=problems)[0], "stale")

    def testGitFailureCannotBecomeAnEmptyFreshChangeSet(self):
        for diffStatus, untrackedStatus in ((1, 0), (0, 1)):
            with self.subTest(diffStatus=diffStatus, untrackedStatus=untrackedStatus):
                run = self.gitRunner(diffStatus=diffStatus, untrackedStatus=untrackedStatus)
                _, problems = freshness.readGitMetadata(Path("/checkout"), "abcd1234", run)
                self.assertEqual(len(problems), 1)
                self.assertEqual(self.assess(metadata=problems)[0], "UNCHECKED")
        run = mock.Mock(side_effect=FileNotFoundError("git"))
        paths, problems = freshness.readGitMetadata(Path("/checkout"), "abcd1234", run)
        self.assertFalse(paths)
        self.assertEqual(self.assess(metadata=problems)[0], "UNCHECKED")

    def testMalformedGitMetadataCannotBecomeFresh(self):
        for command in ("diff", "ls-files"):
            with self.subTest(command=command):
                original = self.gitRunner()

                def run(arguments, **options):
                    if command in arguments:
                        return SimpleNamespace(returncode=0, stdout=b"unterminated")
                    return original(arguments, **options)

                _, problems = freshness.readGitMetadata(Path("/checkout"), "abcd1234", run)
                self.assertEqual(len(problems), 1)
                self.assertEqual(self.assess(metadata=problems)[0], "UNCHECKED")

    def runMain(self, text, metadata=(set(), ()), fileProblem=None):
        output, error = io.StringIO(), io.StringIO()
        with mock.patch.object(freshness.Path, "read_text", side_effect=text if isinstance(text, Exception) else None,
                               return_value=text), \
                mock.patch.object(freshness.Path, "is_file", return_value=True), \
                mock.patch.object(freshness.Path, "resolve", return_value=Path("/checkout")), \
                mock.patch.object(freshness, "readGitMetadata", return_value=metadata), \
                mock.patch.object(freshness, "commitsBehind", return_value=None), \
                mock.patch.object(freshness, "indexedFileProblem", return_value=fileProblem), \
                mock.patch.object(freshness.subprocess, "run", side_effect=AssertionError("real subprocess")), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(error):
            status = freshness.main(["--root", "/checkout", "--index", "/checkout/Tests/coverage/covering-tests.tsv"])
        return status, output.getvalue(), error.getvalue()

    def testExitContractAndTruthfulAdviceWithoutCoverageExecution(self):
        text = "# abcd1234, 1 tests\nSource/Core/One.cpp\ttest_one\n"
        for metadata, problem, verdict in (((set(), ()), None, "fresh"),
                                           ((set(), ("Commit unavailable.",)), None, "UNCHECKED"),
                                           ((set(), ("Commit unavailable.",)), "Indexed file is missing", "stale")):
            with self.subTest(verdict=verdict):
                status, output, error = self.runMain(text, metadata, problem)
                self.assertEqual(status, 0)
                self.assertFalse(error)
                self.assertIn(f"verdict:    {verdict}", output)
                self.assertNotIn("covering-tests-index artifact", output)
                if verdict != "fresh":
                    self.assertIn("--check does not regenerate coverage", output)
                    self.assertIn("run-coverage.sh builds and runs", output)
        status, _, error = self.runMain(FileNotFoundError())
        self.assertEqual(status, 1)
        self.assertIn("absent", error)
        status, _, error = self.runMain(PermissionError())
        self.assertEqual(status, 0)
        self.assertIn("UNCHECKED", error)

    def testNonregularIndexIsAbsentWithoutOpeningOrRunningAnything(self):
        error = io.StringIO()
        with mock.patch.object(freshness.Path, "is_file", return_value=False), \
                mock.patch.object(freshness.Path, "read_text", side_effect=AssertionError("nonregular open")), \
                mock.patch.object(freshness.subprocess, "run", side_effect=AssertionError("real subprocess")), \
                contextlib.redirect_stderr(error):
            status = freshness.main(["--root", "/checkout", "--index", "/checkout/Tests/coverage/covering-tests.tsv"])
        self.assertEqual(status, 1)
        self.assertIn("absent", error.getvalue())


if __name__ == "__main__":
    unittest.main()
