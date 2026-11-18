#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""Checks runner eligibility and cancellation keys from the actual PR workflows.

The expression reader accepts only the boolean/scalar subset these policies use,
plus the fromJSON call that spells a runner's label list; an unsupported policy
fails the test instead of receiving guessed semantics.
GitHub's server-side cancellation delivery still needs live Actions evidence.
"""

import ast
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PR_ACTIONS = {"opened", "synchronize", "reopened", "ready_for_review", "converted_to_draft"}


def cache_path_problems(root):
    """Resolve cache paths after their real preceding environment-export steps."""
    problems = []
    shell_values = {"RUNNER_WORKSPACE": "/fixture/work/repository", "RUNNER_TEMP": "/fixture/temp",
                    "GITHUB_WORKSPACE": "/fixture/work/repository/checkout"}

    def shell_value(value):
        return re.sub(r"\$(?:\{([A-Z_]+)\}|([A-Z_]+))",
                      lambda match: shell_values.get(match[1] or match[2], match[0]), value)

    def exported_values(run, destination):
        target = re.escape('"$' + destination + '"')
        fragments = re.findall(r"(?ms)^\s*\{\s*\n(.*?)^\s*\}\s*>>\s*" + target, run)
        fragments += re.findall(r"(?m)^([^\n]*echo [^\n]*)>>\s*" + target, run)
        return [(name, shell_value(value)) for fragment in fragments
                for name, value in re.findall(r'echo "([A-Za-z_]+)=([^"\n]*)"', fragment)]

    for path in sorted((root / ".github/workflows").glob("*.y*ml")):
        for job_name, job in Workflow(path.read_text()).jobs.items():
            environment, outputs = {}, {}
            steps = re.finditer(r"(?ms)^      - (.*?)(?=^      - |\Z)", job.get("steps", ""))
            for matched in steps:
                fields = mapping("        " + matched[1], 8)
                uses = scalar(fields.get("uses", ""))
                run = fields.get("run", "")
                if uses.startswith("./"):
                    action = root / uses / "action.yml"
                    if action.is_file():
                        run += action.read_text()
                environment.update(exported_values(run, "GITHUB_ENV"))
                identifier = scalar(fields.get("id", ""))
                if identifier:
                    for name, value in exported_values(run, "GITHUB_OUTPUT"):
                        # The build-root resolver has its own executable fixture
                        # coverage; here its preceding output must be present.
                        if "$(python3 " in value and "build_root.py" in value:
                            value = "/fixture/build/" + job_name
                        outputs[identifier + "." + name] = value
                if not uses.startswith("actions/cache"):
                    continue
                inputs = mapping(fields.get("with", ""), 10)
                paths = inputs.get("path", "").strip()
                if paths.startswith("|"):
                    paths = paths[1:].strip()
                if not paths:
                    problems.append(path.name + "/" + job_name + ": empty cache path")
                for cache_path in paths.splitlines():
                    context = path.name + "/" + job_name
                    def expression_value(match):
                        key = match[1].strip()
                        if key.startswith("env.") and key[4:] in environment:
                            return environment[key[4:]]
                        if key.startswith("steps.") and ".outputs." in key:
                            step, output = key[6:].split(".outputs.", 1)
                            if step + "." + output in outputs:
                                return outputs[step + "." + output]
                        known = {"github.workspace": shell_values["GITHUB_WORKSPACE"],
                                 "runner.workspace": shell_values["RUNNER_WORKSPACE"],
                                 "runner.temp": shell_values["RUNNER_TEMP"], "runner.arch": "X64"}
                        if key in known:
                            return known[key]
                        problems.append(context + ": cache root has no preceding canonical export: " + key)
                        return "UNRESOLVED"
                    resolved = re.sub(r"\$\{\{(.*?)\}\}", expression_value, cache_path.strip())
                    if any(part in (".", "..") for part in resolved.lstrip("!").split("/")):
                        problems.append(context + ": cache path contains a dot component: " + resolved)
    return problems


def container_cache_problems(root):
    """Check that cache publication follows successful owned-container cleanup."""
    problems = []
    for path in sorted((root / ".github/workflows").glob("*.y*ml")):
        for job_name, job in Workflow(path.read_text()).jobs.items():
            steps = [mapping("        " + matched[1], 8)
                     for matched in re.finditer(r"(?ms)^      - (.*?)(?=^      - |\Z)", job.get("steps", ""))]
            last_run = last_cleanup = -1
            for index, fields in enumerate(steps):
                run = fields.get("run", "")
                if "ci-linux-image.sh" in run:
                    if "--cleanup-containers" in run and scalar(fields.get("if", "")) == "always()":
                        last_cleanup = index
                    elif "--recipe-digest" not in run:
                        last_run = index
                uses = scalar(fields.get("uses", ""))
                inputs = mapping(fields.get("with", ""), 10)
                if not (uses.startswith("actions/cache/save@") and "env.CCACHE_DIR" in inputs.get("path", "")):
                    continue
                if last_run < 0:
                    continue
                previous = steps[index - 1] if index else {}
                if (last_cleanup != index - 1 or scalar(previous.get("id", "")) != "stop-containers"
                        or "steps.stop-containers.outcome == 'success'" not in fields.get("if", "")):
                    problems.append(path.name + "/" + job_name + ": compiler cache is not guarded by completed cleanup")
            if last_run > last_cleanup:
                problems.append(path.name + "/" + job_name + ": no always-cleanup follows the final container command")
    return problems


class ContainerCacheTests(unittest.TestCase):
    workflow = '''name: fixture
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: Build
        run: ./Scripts/ci-linux-image.sh build
      - name: Cleanup
        id: stop-containers
        if: always()
        run: ./Scripts/ci-linux-image.sh --cleanup-containers
      - name: Save
        if: always() && steps.stop-containers.outcome == 'success'
        uses: actions/cache/save@fixture
        with:
          path: ${{ env.CCACHE_DIR }}
          key: fixture
'''

    def examine(self, text):
        with tempfile.TemporaryDirectory(prefix="ubiquitty-container-cache-") as temporary:
            root = Path(temporary)
            workflows = root / ".github/workflows"
            workflows.mkdir(parents=True)
            (workflows / "ci.yml").write_text(text)
            return container_cache_problems(root)

    def test_actual_container_jobs_clean_up_before_publishing_caches(self):
        self.assertEqual(container_cache_problems(ROOT), [])

    def test_successful_cleanup_before_save_is_accepted(self):
        self.assertEqual(self.examine(self.workflow), [])

    def test_missing_cleanup_guard_or_another_late_writer_is_rejected(self):
        for broken in (
                self.workflow.replace(" && steps.stop-containers.outcome == 'success'", ""),
                self.workflow.replace("run: ./Scripts/ci-linux-image.sh --cleanup-containers", "run: true"),
                self.workflow + "      - name: Late build\n        run: ./Scripts/ci-linux-image.sh build\n"):
            with self.subTest(workflow=broken):
                self.assertTrue(self.examine(broken))


class CachePathTests(unittest.TestCase):
    workflow = '''name: fixture
on: push
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - name: Cache roots
        run: |
          echo "CCACHE_DIR=$RUNNER_WORKSPACE/ccache" >> "$GITHUB_ENV"
      - uses: actions/cache/restore@fixture
        with:
          path: ${{ env.CCACHE_DIR }}
          key: fixture
'''

    def examine(self, text, action=None):
        with tempfile.TemporaryDirectory(prefix="ubiquitty-cache-paths-") as temporary:
            root = Path(temporary)
            workflows = root / ".github/workflows"
            workflows.mkdir(parents=True)
            (workflows / "ci.yml").write_text(text)
            if action is not None:
                directory = root / ".github/actions/bootstrap"
                directory.mkdir(parents=True)
                (directory / "action.yml").write_text(action)
            return cache_path_problems(root)

    def test_actual_workflow_cache_paths_use_preceding_canonical_roots(self):
        self.assertEqual(cache_path_problems(ROOT), [])

    def test_exported_root_and_composite_bootstrap_are_accepted(self):
        self.assertEqual(self.examine(self.workflow), [])
        replacement = "      - uses: ./.github/actions/bootstrap\n"
        start = self.workflow.index("      - name: Cache roots")
        end = self.workflow.index("      - uses: actions/cache")
        action = 'runs:\n  using: composite\n  steps:\n    - run: |\n        echo "CCACHE_DIR=$RUNNER_WORKSPACE/ccache" >> "$GITHUB_ENV"\n'
        self.assertEqual(self.examine(self.workflow[:start] + replacement + self.workflow[end:], action), [])

    def test_parent_and_current_directory_components_are_rejected(self):
        for value in ("${{ github.workspace }}/../ccache", "${{ runner.temp }}/./ccache"):
            with self.subTest(path=value):
                self.assertTrue(self.examine(self.workflow.replace("${{ env.CCACHE_DIR }}", value)))

    def test_noncanonical_export_missing_export_and_late_export_are_rejected(self):
        broken = self.workflow.replace("$RUNNER_WORKSPACE/ccache", "$RUNNER_WORKSPACE/../ccache")
        self.assertTrue(self.examine(broken))
        root = self.workflow.index("      - name: Cache roots")
        cache = self.workflow.index("      - uses: actions/cache")
        self.assertTrue(self.examine(self.workflow[:root] + self.workflow[cache:]))
        self.assertTrue(self.examine(self.workflow[:root] + self.workflow[cache:] + self.workflow[root:cache]))
        self.assertTrue(self.examine(self.workflow.replace('$GITHUB_ENV', '$GITHUB_OUTPUT')))
        self.assertTrue(self.examine(self.workflow.replace('path: ${{ env.CCACHE_DIR }}', 'path:')))


def mapping(text, indent):
    matches = list(re.finditer(r"^" + " " * indent + r"([A-Za-z_][A-Za-z0-9_-]*):([^\n]*(?:\n|$))", text, re.M))
    result = {}
    for index, match in enumerate(matches):
        key = match.group(1)
        if key in result:
            raise AssertionError(f"duplicate workflow field: {key}")
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        result[key] = text[match.start(2):end]
    return result


def scalar(text):
    return text.split("\n", 1)[0].strip()


def expression(text, context):
    text = text.strip()
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2].strip()
    replacements = {"&&": " and ", "||": " or ", "!": " not ", "true": "True", "false": "False"}
    text = re.sub(r"'[^']*'|\"[^\"]*\"|&&|\|\||!(?!=)|\btrue\b|\bfalse\b",
                  lambda match: replacements.get(match.group(), match.group()), text).strip()

    def value(node):
        if isinstance(node, ast.Constant):
            return node.value
        if isinstance(node, ast.Name):
            return context[node.id]
        if isinstance(node, ast.Attribute):
            parent = value(node.value)
            return parent.get(node.attr) if isinstance(parent, dict) else None
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return not value(node.operand)
        if isinstance(node, ast.BoolOp):
            result = value(node.values[0])
            for operand in node.values[1:]:
                if isinstance(node.op, ast.And):
                    result = value(operand) if result else result
                else:
                    result = result if result else value(operand)
            return result
        if isinstance(node, ast.Compare) and len(node.ops) == 1:
            left, right = value(node.left), value(node.comparators[0])
            if isinstance(node.ops[0], ast.Eq):
                return left == right
            if isinstance(node.ops[0], ast.NotEq):
                return left != right
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "fromJSON"
                and len(node.args) == 1 and not node.keywords):
            return json.loads(value(node.args[0]))
        raise AssertionError(f"unsupported workflow policy expression: {text}")

    return value(ast.parse(text, mode="eval").body)


def context(event="pull_request", action="synchronize", draft=False, owner=True, number=41, fork=False):
    return {"github": {
        "event_name": event,
        "event": {"action": action, "pull_request": {
            "draft": draft, "number": number, "user": {"login": "owner" if owner else "contributor"},
            "head": {"repo": {"full_name": "contributor/ubiquitty" if fork else "owner/ubiquitty"}},
        }} if event == "pull_request" else {},
        "ref": f"refs/pull/{number}/merge" if event == "pull_request" else "refs/heads/main",
        "repository": "owner/ubiquitty",
        "repository_owner": "owner",
    }, "vars": {"UBQ_HOSTED_RUNNERS": "true"}}


class Workflow:
    def __init__(self, text):
        self.text = text
        self.fields = mapping(text, 0)
        if "pull_request" in scalar(self.fields["on"]):
            raise AssertionError("PR workflow triggers must use the inspected block form")
        self.events = mapping(self.fields["on"], 2)
        self.jobs = {key: mapping(body, 4) for key, body in mapping(self.fields["jobs"], 2).items()}
        if not self.jobs:
            raise AssertionError("workflow has no inspected jobs")

    def activities(self):
        field = mapping(self.events["pull_request"], 4)["types"]
        return {item.strip() for item in scalar(field).strip("[]").split(",")}

    def scheduled_jobs(self, invocation):
        github = invocation["github"]
        event = github["event_name"]
        if event not in self.events:
            return set()
        if event == "pull_request" and github["event"]["action"] not in self.activities():
            return set()
        finished = {}
        pending = dict(self.jobs)
        while pending:
            previous_count = len(pending)
            for name, job in list(pending.items()):
                dependencies = scalar(job.get("needs", "")).strip("[]")
                dependencies = [item.strip() for item in dependencies.split(",") if item.strip()]
                if not all(dependency in finished for dependency in dependencies):
                    continue
                job_context = {**invocation, "needs": {
                    dependency: {"outputs": {key: "true" for key in ("linux", "apple", "code", "cmake")}}
                    for dependency in dependencies
                }}
                eligible = bool(expression(scalar(job.get("if", "true")), job_context))
                finished[name] = eligible and all(finished[dependency] for dependency in dependencies)
                del pending[name]
            if len(pending) == previous_count:
                raise AssertionError("workflow needs graph is cyclic or names a missing job")
        return {name for name, eligible in finished.items() if eligible}

    def group(self, invocation):
        invocation = {**invocation, "github": {**invocation["github"], "workflow": scalar(self.fields["name"])}}
        group = scalar(mapping(self.fields["concurrency"], 2)["group"])
        return re.sub(r"\$\{\{(.*?)\}\}", lambda match: str(expression(match.group(1), invocation)), group)

    def cancels(self, invocation):
        return bool(expression(scalar(mapping(self.fields["concurrency"], 2)["cancel-in-progress"]), invocation))


class ImageWrapperTests(unittest.TestCase):
    """Core's CI image wrapper, which every repository's workflow runs."""

    def test_the_image_wrapper_runs_the_command_directly_off_hosted_runners(self):
        wrapper = ROOT / "Scripts/ci-linux-image.sh"
        for value in ("false", ""):
            environment = {key: item for key, item in os.environ.items() if key != "UBQ_CI_LINUX_IMAGE"}
            if value:
                environment["UBQ_CI_LINUX_IMAGE"] = value
            ran = subprocess.run([str(wrapper), "printf", "direct"], env=environment,
                                 capture_output=True, text=True, check=True)
            self.assertEqual(ran.stdout, "direct", value)

    def test_the_image_wrapper_reuses_a_cached_archive_and_saves_what_it_builds(self):
        wrapper = ROOT / "Scripts/ci-linux-image.sh"
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            stubs = directory / "bin"
            stubs.mkdir()
            docker = stubs / "docker"
            docker.write_text(
                "#!/bin/sh\n"
                'echo "$1" >> "$DOCKER_LOG"\n'
                'case "$1" in\n'
                '  info) echo "[]" ;;\n'
                '  image) [ -f "$DOCKER_STATE" ] || exit 1; case "$*" in *--format*) printf "sha256:%064d\\n" 0 ;; esac ;;\n'
                '  container) echo "Error: No such container: fixture" >&2; exit 1 ;;\n'
                '  load|build) touch "$DOCKER_STATE" ;;\n'
                '  save) echo image > "$3" ;;\n'
                "esac\n")
            docker.chmod(0o755)
            log, state, archive = directory / "docker.log", directory / "image", directory / "image.tar"
            environment = {**os.environ, "PATH": f"{stubs}{os.pathsep}{os.environ['PATH']}",
                           "UBQ_CI_LINUX_IMAGE": "true", "UBQ_CI_IMAGE_ARCHIVE": str(archive),
                           "DOCKER_LOG": str(log), "DOCKER_STATE": str(state),
                           "RUNNER_WORKSPACE": str(directory), "RUNNER_TEMP": str(directory),
                           "RUNNER_TOOL_CACHE": str(directory)}

            def calls():
                subprocess.run([str(wrapper), "true"], env=environment, check=True, capture_output=True)
                # Only the calls that decide the image; run-linux.sh asks the
                # daemon about itself on the way to its recipe.
                made = [call for call in log.read_text().split() if call in ("image", "load", "build", "save", "run")]
                log.unlink()
                return made

            # Nothing cached: the image is built, saved for the next run, and
            # its identity read from the image that now exists.
            self.assertEqual(calls(), ["image", "image", "build", "save", "image", "run"])
            self.assertTrue(archive.is_file())
            # A fresh runner with the archive restored loads it and builds nothing.
            state.unlink()
            self.assertEqual(calls(), ["image", "load", "image", "image", "run"])
            digest = subprocess.run([str(wrapper), "--recipe-digest"], env=environment,
                                    capture_output=True, text=True, check=True).stdout.strip()
            self.assertNotIn("build", log.read_text().split())
            self.assertRegex(digest, r"^[0-9a-f]{12}$")


class DraftWorkflowTests(unittest.TestCase):
    def setUp(self):
        # The draft and cancellation policy these cases hold is the app's CI
        # workflow's. Core's and the Kit's workflows are not held to it, so
        # their presence here is not this case's subject.
        role = ROOT / "Scripts/repository.txt"
        if not role.exists() or role.read_text(encoding="utf-8").strip() != "app":
            self.skipTest("the app's workflow policy has no subject outside the app repository")
        self.workflows = {}
        for path in sorted((ROOT / ".github/workflows").iterdir()):
            if path.suffix not in (".yml", ".yaml"):
                continue
            workflow = Workflow(path.read_text())
            if path.name != "cancel-closed-pr.yml" and "pull_request" in workflow.events:
                self.workflows[path.name] = workflow
        self.assertTrue({"ci.yml", "secret-scan.yml"} <= self.workflows.keys())

    def assert_draft_policy(self, workflows):
        for name, workflow in workflows.items():
            self.assertEqual(workflow.activities(), PR_ACTIONS, name)
            for action in PR_ACTIONS:
                for owner in (True, False):
                    invocation = context(action=action, draft=True, owner=owner)
                    self.assertEqual(workflow.scheduled_jobs(invocation), set(), (name, action, owner))

    def test_all_draft_activity_allocates_no_gate_or_battery_runner(self):
        self.assert_draft_policy(self.workflows)

    def test_ready_open_reopen_and_push_preserve_every_enabled_job(self):
        for name, workflow in self.workflows.items():
            enabled = {job for job, fields in workflow.jobs.items()
                       if scalar(fields.get("if", "true")) != "${{ false }}"}
            for action in PR_ACTIONS - {"converted_to_draft"}:
                self.assertEqual(workflow.scheduled_jobs(context(action=action)), enabled, (name, action))
            self.assertEqual(workflow.scheduled_jobs(context(owner=False)), enabled)
            for event in workflow.events.keys() - {"pull_request"}:
                self.assertEqual(workflow.scheduled_jobs(context(event=event)), enabled, (name, event))

    def test_conversion_reuses_the_running_pr_group_without_a_runner(self):
        for workflow in self.workflows.values():
            ready = context(action="ready_for_review")
            converted = context(action="converted_to_draft", draft=True)
            self.assertEqual(workflow.group(ready), workflow.group(converted))
            self.assertTrue(workflow.cancels(converted))
            self.assertEqual(workflow.scheduled_jobs(converted), set())
            self.assertNotEqual(workflow.group(ready), workflow.group(context(number=42)))
        groups = {workflow.group(context()) for workflow in self.workflows.values()}
        self.assertEqual(len(groups), len(self.workflows))

    def test_runner_variables_do_not_disable_hosted_batteries(self):
        ci = self.workflows["ci.yml"]
        invocation = context()
        enabled = ci.scheduled_jobs(invocation)
        invocation["vars"]["UBQ_HOSTED_RUNNERS"] = "false"
        self.assertEqual(ci.scheduled_jobs(invocation), enabled)
        # Hosted runners run the preflight alone; the batteries stay defined so a
        # maintainer can run them by hand, and each is off rather than removed.
        self.assertEqual(enabled, {"preflight"})
        for name in ("linux", "apple", "host-linux-arm64", "coverage"):
            self.assertIn(name, ci.jobs)
            self.assertNotIn(name, enabled)

    def test_workflows_have_no_self_hosted_runner_fallback(self):
        runners = {}
        for path in sorted((ROOT / ".github/workflows").iterdir()):
            if path.suffix in (".yml", ".yaml"):
                source = path.read_text()
                self.assertNotIn("UBQ_HOSTED_RUNNERS", source, path.name)
                for name, job in Workflow(source).jobs.items():
                    runners[(path.name, name)] = scalar(job["runs-on"])
        for key, runner in runners.items():
            with self.subTest(job=key):
                self.assertNotIn("self-hosted", runner)
        for name in ("preflight", "linux", "coverage"):
            self.assertEqual(runners[("ci.yml", name)], "ubuntu-24.04", name)
        self.assertEqual(runners[("ci.yml", "apple")], "xcode-27")
        self.assertEqual(runners[("ci.yml", "host-linux-arm64")], "ubuntu-24.04-arm")
        for workflow, name in (("secret-scan.yml", "gitleaks"),
                               ("cancel-closed-pr.yml", "cancel")):
            self.assertEqual(runners[(workflow, name)], "ubuntu-24.04", workflow)
        # A -latest label moves to a new image on GitHub's schedule, so a job's
        # toolchain would change with no commit here.
        for path in sorted((ROOT / ".github/workflows").glob("*.yml")):
            with self.subTest(workflow=path.name):
                self.assertNotRegex(path.read_text(), r"\b(?:ubuntu|macos|windows)-latest\b")

    def test_hosted_batteries_run_in_the_pinned_linux_image(self):
        # The image wrapper uses the runner environment supplied by Actions.
        ci = self.workflows["ci.yml"]
        image = "UBQ_CI_LINUX_IMAGE: ${{ runner.environment == 'github-hosted' }}"
        for name in ("preflight", "linux"):
            job = ci.jobs[name]
            self.assertNotIn("UBQ_CI_LINUX_IMAGE", job.get("env", ""), name)
            self.assertIn(f"{image}\n        run: ./Scripts/ci-linux-image.sh python3 Scripts/ci_batteries.py run {name}",
                          job["steps"], name)
        self.assertNotIn("ci-linux-image.sh", ci.jobs["apple"]["steps"])
        for name, job in ci.jobs.items():
            with self.subTest(job=name):
                self.assertNotIn("UBQ_HOSTED_RUNNERS", job.get("env", "") + job.get("steps", ""))

    def test_non_pr_triggers_and_existing_concurrency_semantics_are_preserved(self):
        ci = self.workflows["ci.yml"]
        self.assertEqual(set(ci.events), {"pull_request", "push", "schedule", "workflow_dispatch"})
        self.assertIn('cron: "13 5 * * *"', ci.events["schedule"])
        self.assertNotEqual(ci.group(context(event="schedule")), ci.group(context(event="push")))
        self.assertEqual(ci.group(context(event="workflow_dispatch")), ci.group(context(event="push")))
        self.assertTrue(ci.cancels(context(event="push")))
        secret = self.workflows["secret-scan.yml"]
        self.assertEqual(set(secret.events), {"pull_request", "push", "schedule"})
        self.assertIn("cron: '0 6 * * 1'", secret.events["schedule"])
        self.assertFalse(secret.cancels(context(event="push")))
        self.assertFalse(secret.cancels(context(event="schedule")))
        for workflow in (ci, secret):
            for event in ("push", "pull_request"):
                self.assertEqual(scalar(mapping(workflow.events[event], 4)["branches"]), "[main]")

    def test_secret_scan_has_read_only_permissions(self):
        secret = self.workflows["secret-scan.yml"]
        self.assertIn("gitleaks", secret.jobs)
        self.assertIn("detect --source . --config .gitleaks.toml --redact --verbose", secret.jobs["gitleaks"]["steps"])
        self.assertEqual(scalar(mapping(secret.fields["permissions"], 2)["contents"]), "read")

    def test_every_dispatch_input_is_read(self):
        # An input nothing reads offers a choice that changes nothing.
        for path in sorted((ROOT / ".github/workflows").glob("*.yml")):
            workflow = Workflow(path.read_text())
            dispatch = mapping(workflow.events.get("workflow_dispatch", ""), 4).get("inputs", "")
            for name in mapping(dispatch, 6):
                with self.subTest(workflow=path.name, input=name):
                    self.assertRegex(workflow.text, r"\binputs\." + re.escape(name) + r"\b")

    def test_every_action_is_pinned_to_a_commit(self):
        # A tag can be moved to other code after review; a commit cannot.
        for path in sorted((ROOT / ".github/workflows").glob("*.yml")):
            for number, line in enumerate(path.read_text().splitlines(), 1):
                used = re.search(r"\buses:\s*(\S+)", line)
                if used:
                    with self.subTest(workflow=path.name, line=number):
                        self.assertRegex(used.group(1), r"^[\w.-]+/[\w./-]+@[0-9a-f]{40}$")

    def test_secret_scan_verifies_its_scanner_and_keeps_no_token(self):
        steps = self.workflows["secret-scan.yml"].jobs["gitleaks"]["steps"]
        self.assertIn("persist-credentials: false", steps)
        self.assertRegex(steps, r'digest=[0-9a-f]{64}\n')
        self.assertIn('| sha256sum -c -', steps)
        # The archive is unpacked only after the digest check, never streamed
        # from the download into tar.
        self.assertLess(steps.index("sha256sum -c -"), steps.index("tar -xzf"))
        self.assertNotRegex(steps, r"curl[^\n]*\|\s*tar")

    def test_closed_pr_cancellation_remains_independent_housekeeping(self):
        cleanup = Workflow((ROOT / ".github/workflows/cancel-closed-pr.yml").read_text())
        self.assertEqual(cleanup.activities(), {"closed"})
        self.assertEqual(cleanup.scheduled_jobs(context(action="closed", draft=True)), {"cancel"})
        self.assertEqual(cleanup.scheduled_jobs(context(action="converted_to_draft", draft=True)), set())
        # A fork's read-only token can cancel nothing, and its branch name may
        # be one of this repository's own.
        self.assertEqual(cleanup.scheduled_jobs(context(action="closed", fork=True)), set())
        steps = cleanup.jobs["cancel"]["steps"]
        self.assertIn('*"HTTP 409"*)', steps)
        self.assertIn('*"HTTP 403"*)', steps)
        self.assertIn('exit "$refused"', steps)

    def test_missing_independent_job_guard_is_detected(self):
        for name, workflow in self.workflows.items():
            for job, fields in workflow.jobs.items():
                if "draft" not in fields.get("if", ""):
                    continue
                body = mapping(workflow.fields["jobs"], 2)[job]
                unguarded = re.sub(r"(?m)^    if:.*\n", "", body, count=1)
                self.assertNotEqual(body, unguarded)
                mutant = Workflow(workflow.text.replace(body, unguarded, 1))
                with self.subTest(workflow=name, job=job), self.assertRaises(AssertionError):
                    self.assert_draft_policy({name: mutant})

    def test_missing_ready_or_conversion_event_is_detected(self):
        for name, workflow in self.workflows.items():
            for action in ("ready_for_review", "converted_to_draft"):
                mutant = Workflow(workflow.text.replace(", " + action, ""))
                with self.subTest(workflow=name, action=action), self.assertRaises(AssertionError):
                    self.assert_draft_policy({name: mutant})

    def test_unsupported_status_function_cannot_bypass_dependency_guard(self):
        with self.assertRaises(AssertionError):
            expression("always()", context())


if __name__ == "__main__":
    unittest.main()
