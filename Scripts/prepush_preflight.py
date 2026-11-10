#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""prepush_preflight.py — what a push needs before its first gate runs.

Two questions the pre-push gates used to answer too late:

  classify   Does the diff touch documentation alone? Then nothing it changes is
             built or tested, and the push runs the lint tier: every style and
             commit-rule gate, which judge prose too, and no build or test.
  check      Do the gates this push will run have the build products they read?
             A gate that finds its product missing says so, but only when it
             runs: after the gates ahead of it, which on a cold checkout is
             fifteen to forty minutes in. This reads the plan of the gates the
             push selected and builds, or refuses, before any of them starts.

The plan is what the real run would execute: Scripts/pre-push-runner.sh records
each gate it would run, and each build product a gate declares with `ubq_need`,
when UBQ_PREPUSH_PLAN is set. Selection logic (scopes, tiers, the verdict cache)
is therefore the runner's own and is not restated here.

    prepush_preflight.py classify          paths on stdin -> `docs` or `code`
    prepush_preflight.py plan --file FILE --nonce TOKEN --timeout SECONDS -- CMD...
    prepush_preflight.py check --plan FILE --build-root DIR [--core-root DIR]
                               [--kit-root DIR] [--mode build|refuse]

`check` exits 0 when everything the gates read is present, or was built, and 1
when a product is missing and was not built. `plan` exits 0 when the command
finished in time, and 3, saying why, when it did not. Stdlib only.
"""

import argparse
import dataclasses
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import time

# --------------------------------------------------------------------------
# Documentation-only diffs
# --------------------------------------------------------------------------

# One definition of "documentation", conservative by construction: a path is
# documentation only if it is plainly prose, a figure for prose, or a license
# text. Anything else, scripts and CMake and data files included, is code, so a
# diff that touches one runs every gate.
DOCUMENT_BASENAMES = frozenset({
    "NOTICE", "TRADEMARKS.md", "third-party-notices.md",
})
DOCUMENT_BASENAME_PREFIXES = ("README", "LICENSE", "COPYING", "CHANGELOG", "CONTRIBUTING")
DOCUMENT_SUFFIXES = frozenset({".md"})
# Figures that a document embeds. Only under Docs/, where nothing else lives.
FIGURE_SUFFIXES = frozenset({".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp"})
# A license text is plain text. A file under Docs/ that is not Markdown or a
# figure is a gate's input (Docs/other-repositories.txt is one), so it is code.
LICENSE_DIRECTORIES = ("LICENSES/", "Docs/third-party-licenses/")


def is_documentation(path):
    """Whether one repository-relative path is documentation, as defined above."""
    posix = PurePosixPath(path)
    name = posix.name
    if not name or ".." in posix.parts:
        return False
    suffix = posix.suffix.lower()
    text = str(posix)
    if name in DOCUMENT_BASENAMES or name.startswith(DOCUMENT_BASENAME_PREFIXES):
        # A README or LICENSE* is prose whatever its extension, but a script that
        # happens to be named like one is not: only text-like suffixes qualify.
        return suffix in {"", ".md", ".txt", ".rst"}
    if suffix in DOCUMENT_SUFFIXES:
        return True
    if text.startswith(LICENSE_DIRECTORIES):
        return suffix in {".txt", ".md", ""}
    if text.startswith("Docs/"):
        return suffix in FIGURE_SUFFIXES
    return False


def classify(paths):
    """`docs` when there is at least one path and every one is documentation."""
    paths = [path.strip() for path in paths if path.strip()]
    if paths and all(is_documentation(path) for path in paths):
        return "docs"
    return "code"


# --------------------------------------------------------------------------
# Prerequisites
# --------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Context:
    """Where the products live, for the checkout being pushed."""
    build_root: Path
    core_root: Path
    kit_root: Path
    subject_root: Path
    darwin: bool
    tools: frozenset


@dataclasses.dataclass(frozen=True)
class Need:
    """A build product some gate reads.

    `automatic` products are cheap and local: the preflight builds them. The
    rest are named with the exact command and the push is refused. `advisory`
    products belong to a gate that stands down loudly without them, so they are
    reported and the push goes on.
    """
    identifier: str
    why: str
    applies: object
    present: object
    command: object
    automatic: bool = False
    advisory: bool = False


def _metallib_present(context):
    if (context.build_root / "macos/Source/Platform/macOS/default.metallib").is_file():
        return True
    return any((context.build_root / "metallib").rglob("default.metallib")) \
        if (context.build_root / "metallib").is_dir() else False


def _host_test_objects_present(context):
    tree = context.build_root / "BuildHostTests"
    return tree.is_dir() and next(tree.rglob("*.cpp.o"), None) is not None


def _has_apple_toolchain(context):
    return context.darwin and "xcrun" in context.tools


NEEDS = {
    need.identifier: need for need in (
        Need("core-snapshot-library",
             "the Swift renderer drivers link Core's snapshot library",
             applies=_has_apple_toolchain,
             present=lambda context: (context.build_root / "core/libubiquitty-core.a").is_file(),
             command=lambda context: str(context.core_root / "Scripts/build-core-snapshot.sh"),
             automatic=True),
        Need("kit-metallib",
             "the frame encoder and terminal renderer drivers load default.metallib",
             applies=_has_apple_toolchain,
             present=_metallib_present,
             command=lambda context: str(context.kit_root / "Scripts/build-metallib.sh"),
             automatic=True),
        Need("web-dist",
             "the web snapshot arm compares against the staged web build (it skips without one)",
             applies=lambda context: "emcc" in context.tools or "EMSDK" in os.environ,
             present=lambda context: (context.build_root / "web-dist/ubiquitty.wasm").is_file(),
             command=lambda context: "./Scripts/build-web.sh",
             advisory=True),
        Need("host-test-objects",
             "check-core-subset-symbols and the style self-test read the host test objects",
             applies=lambda context: True,
             present=_host_test_objects_present,
             command=lambda context: "./Scripts/build-tests.sh",
             advisory=True),
    )
}

# The build products a gate reads, by the script it runs. A gate not listed here
# needs nothing built beforehand; one that does is added here and, when it picks
# its own work (the app's snapshot arms), declares it with `ubq_need`.
NEEDS_BY_SCRIPT = {
    "check-swift-renderer.sh": ("core-snapshot-library", "kit-metallib"),
    "check-terminal-renderer.sh": ("core-snapshot-library", "kit-metallib"),
    "check-frame-encoder.sh": ("core-snapshot-library", "kit-metallib"),
    "check-frame-instances.sh": ("core-snapshot-library",),
    "check-glyph-atlas.sh": ("core-snapshot-library",),
    "check-glyph-rasterizer.sh": ("core-snapshot-library",),
    "check-font-catalogue.sh": ("core-snapshot-library",),
    "check-core-subset-symbols.py": ("host-test-objects",),
    "check-style.py": ("host-test-objects",),
}


def read_plan(lines):
    """The gates and declared needs a plan records, as {need id: [gate names]}."""
    wanted = {}
    for line in lines:
        fields = line.rstrip("\n").split("\t")
        if fields[0] == "NEED" and len(fields) >= 2:
            wanted.setdefault(fields[1], []).append(fields[2] if len(fields) > 2 else fields[1])
        elif fields[0] == "GATE" and len(fields) >= 3:
            name, command = fields[1], fields[2]
            # The renderer drivers' type-check pass compiles Swift and links and
            # loads nothing, so it reads none of the products the draw pass does.
            if "--typecheck-only" in command.split():
                continue
            for word in command.split():
                for identifier in NEEDS_BY_SCRIPT.get(os.path.basename(word), ()):
                    wanted.setdefault(identifier, []).append(name)
    return wanted


def resolve(wanted, context):
    """The needs that apply here, as (need, who asked), whether or not they are present.

    A product a gate declared and this file has no rule for is returned as a need
    that is never present, so the omission is seen rather than waved through.
    """
    found = []
    for identifier in sorted(wanted):
        gates = sorted(set(wanted[identifier]))
        need = NEEDS.get(identifier)
        if need is None:
            need = Need(identifier, "declared by a gate, with no rule here", lambda c: True,
                        lambda c: False, lambda c: f"(no command known for {identifier})")
        if need.applies(context):
            found.append((need, gates))
    return found


def describe(need, gates, context, note=""):
    return (f"  - {need.identifier}: {need.why}{note}\n"
            f"      needed by: {', '.join(gates)}\n"
            f"      build it:  {need.command(context)}")


def build_command(command):
    """Runs a build command with its output on stderr; returns its status.

    A command that cannot start is a failed build like any other, not a traceback.
    """
    try:
        return subprocess.call([command], stdout=sys.stderr.fileno())
    except OSError as error:
        print(f"pre-push gates: cannot run {command}: {error}", file=sys.stderr)
        return 127


def check(wanted, context, mode, out=sys.stderr, run=build_command):
    """Builds the products the gates read, or refuses; returns the exit status.

    In build mode every automatic product is built by its own script whether or
    not a copy is there: the script is incremental, so an up-to-date product costs
    seconds, and the copy that stands is then the one for this tree and this
    configuration, which presence alone cannot say. An advisory product is reported
    and never built or refused: its gate stands down without it.
    """
    needed = resolve(wanted, context)
    advisories = [(need, gates) for need, gates in needed if need.advisory and not need.present(context)]
    required = [(need, gates) for need, gates in needed if not need.advisory]
    if advisories:
        print("pre-push gates: NOTE — a gate stands down or skips without these (reported again in the summary):", file=out)
        for need, gates in advisories:
            print(describe(need, gates, context), file=out)
    failed = set()
    if mode == "build":
        for need, gates in required:
            if not need.automatic:
                continue
            command = need.command(context)
            print(f"pre-push gates: building {need.identifier} first ({command}) — {need.why}", file=out)
            started = time.time()
            status = run(command)
            print(f"pre-push gates: {need.identifier}: {'up to date' if status == 0 else 'FAILED'} after "
                  f"{time.time() - started:.0f}s", file=out)
            if status != 0:
                failed.add(need.identifier)
    refused = [(need, gates) for need, gates in required
               if need.identifier in failed or not need.present(context)]
    if refused:
        print(f"pre-push gates: REFUSED before any gate ran — {len(refused)} build product(s) the "
              f"selected gates read are missing or could not be built under {context.build_root}:", file=out)
        for need, gates in refused:
            print(describe(need, gates, context, " (its build failed)" if need.identifier in failed else ""), file=out)
        print("  (UBQ_PREPUSH_PREREQUISITES=off skips this check for one push.)", file=out)
        return 1
    return 0


def plan(arguments, file, nonce, timeout, out=sys.stderr):
    """Runs `arguments` as a plan: a fresh process group, a deadline, a loud failure.

    The plan variables carry the nonce the runner checks against `FILE.nonce`, so
    a variable inherited from some other shell can never turn a real run into a
    plan. Returns 0 when the command finished in time and 3 when it did not.
    """
    Path(file + ".nonce").write_text(nonce)
    environment = dict(os.environ, UBQ_PREPUSH_PLAN=nonce, UBQ_PREPUSH_PLAN_FILE=file)
    log = Path(file + ".log")
    with open(log, "wb") as stream:
        process = subprocess.Popen(arguments, env=environment, stdout=stream, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, start_new_session=True)
        try:
            status = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            status = None
        finally:
            try:
                os.killpg(process.pid, 9)
            except (ProcessLookupError, PermissionError):
                pass
            process.wait()
    if status == 0:
        return 0
    reason = f"timed out after {timeout}s" if status is None else f"exited {status}"
    print(f"pre-push gates: the plan of this push could not be made ({reason}); its last output:", file=out)
    for line in log.read_text(errors="replace").splitlines()[-8:]:
        print(f"    {line}", file=out)
    return 3


def main(arguments):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("classify")
    planner = commands.add_parser("plan")
    planner.add_argument("--file", required=True)
    planner.add_argument("--nonce", required=True)
    planner.add_argument("--timeout", type=int, default=180)
    planner.add_argument("rest", nargs=argparse.REMAINDER)
    checker = commands.add_parser("check")
    checker.add_argument("--plan", required=True)
    checker.add_argument("--build-root", required=True)
    checker.add_argument("--core-root", default="")
    checker.add_argument("--kit-root", default="")
    checker.add_argument("--subject-root", default=".")
    checker.add_argument("--mode", choices=("build", "refuse"), default="build")
    parsed = parser.parse_args(arguments)
    if parsed.command == "classify":
        print(classify(sys.stdin.read().splitlines()))
        return 0
    if parsed.command == "plan":
        command = parsed.rest[1:] if parsed.rest[:1] == ["--"] else parsed.rest
        return plan(command, parsed.file, parsed.nonce, parsed.timeout)
    tools = frozenset(name for name in ("xcrun", "emcc") if shutil.which(name))
    context = Context(build_root=Path(parsed.build_root),
                      core_root=Path(parsed.core_root or parsed.subject_root),
                      kit_root=Path(parsed.kit_root or parsed.core_root or parsed.subject_root),
                      subject_root=Path(parsed.subject_root),
                      darwin=sys.platform == "darwin", tools=tools)
    with open(parsed.plan, encoding="utf-8") as stream:
        wanted = read_plan(stream)
    return check(wanted, context, parsed.mode)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
