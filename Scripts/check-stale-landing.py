#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-stale-landing.py — a commit that removes a recently landed line names
the commit it undoes.

  Scripts/check-stale-landing.py                     # origin/main..HEAD
  Scripts/check-stale-landing.py --base REF --tip REF  # REF..REF
  Scripts/check-stale-landing.py A..B                # every commit in a range
  Scripts/check-stale-landing.py COMMIT              # one commit
  Scripts/check-stale-landing.py --staged --message-file F   # commit-msg hook
  Scripts/check-stale-landing.py --history SINCE     # audit landed main
  Scripts/check-stale-landing.py --self-test

Exit 0 = nothing reverted, 1 = a commit reverts recent work it does not name,
2 = usage or git error.

The rule. For every line a commit removes, blame that line at the commit's
parent. The line is a foreign revert when its authoring commit

  (a) was committed within --days (default 14) of the commit under test,
  (b) is not one of the landing's own commits (a branch rewriting its own
      earlier work is ordinary), and
  (c) is not named in the commit message, by an abbreviated hash of at least
      seven digits or by its subject.

A removed line that carries no letter or digit (a brace, a blank) says nothing
about whose work went, and is not counted.

One rewritten line is ordinary development, so the rule has a shape to
match. A stale landing takes a file whole from an older base: in at least one
file the commit does nothing but undo, bringing back text the recent commit had
removed. A file whose change is otherwise its own, an undo of a commit that was
itself a stale landing (its repair), and one substitution made across several
files (a rename or header sweep) are not that shape. A commit with the shape is
reported with every recent foreign unnamed commit it undoes a hunk of.

How it happens. A rebase does not by itself undo anything. The commit's own
diff removes main's lines when a conflict during the author's rebase is
resolved by taking the branch's whole file, or when a file is copied in from an
older checkout. Rebasing before merge is therefore necessary and not
sufficient; this blame is the check that catches the rest.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

DEFAULT_DAYS = 14
# A landing applied by one rebase lands its commits within seconds of each
# other; consecutive first-parent commits closer than this are one landing in
# --history mode.
LANDING_GAP_SECONDS = 10
# The number of files one substitution must restore to read as a sweep.
SWEEP_FILES = 3
WORD = re.compile(r"[\w.-]+")
HEX = re.compile(r"\b[0-9a-f]{7,40}\b")
HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def git(*arguments, check=True, cwd=None, data=None):
    finished = subprocess.run(["git"] + list(arguments), capture_output=True,
                              cwd=cwd, input=data, check=False)
    if check and finished.returncode != 0:
        raise GitError(" ".join(arguments) + ": "
                       + finished.stderr.decode("utf-8", "replace").strip())
    return finished.stdout.decode("utf-8", "replace")


class GitError(Exception):
    pass


def meaningful(text):
    return any(character.isalnum() for character in text)


def parseDiff(text):
    """[(old path, new path, [(old line number, text)] removed, [added text])].

    A path is None on the side where the file does not exist.
    """
    files = []
    oldLine = 0
    for line in text.split("\n"):
        if line.startswith("diff --git "):
            files.append([None, None, [], []])
            continue
        if not files:
            continue
        current = files[-1]
        if line.startswith("--- "):
            current[0] = None if line == "--- /dev/null" else line[6:]
        elif line.startswith("+++ "):
            current[1] = None if line == "+++ /dev/null" else line[6:]
        elif HUNK.match(line):
            oldLine = int(HUNK.match(line).group(1))
        elif line.startswith("-"):
            current[2].append((oldLine, line[1:]))
            oldLine += 1
        elif line.startswith("+"):
            current[3].append(line[1:])
    return [tuple(entry) for entry in files]


def diffArguments():
    return ["-U0", "--no-color", "--no-ext-diff", "--find-renames",
            "--src-prefix=a/", "--dst-prefix=b/"]


class Repository:
    def __init__(self, cwd=None):
        self.cwd = cwd
        self.commitInfo = {}
        self.addedByCommit = {}
        self.staleCache = {}

    def git(self, *arguments, check=True):
        return git(*arguments, check=check, cwd=self.cwd)

    def info(self, sha):
        """(committer time, subject, message) of a commit, cached."""
        if sha not in self.commitInfo:
            output = self.git("show", "-s", "--format=%ct%x00%s%x00%B", sha)
            when, subject, message = output.split("\0", 2)
            self.commitInfo[sha] = (int(when), subject, message)
        return self.commitInfo[sha]

    def blame(self, revision, path, lineNumbers, since):
        """{line number: authoring sha} for lines of `path` at `revision`.

        Blame stops at `since` (a Unix time): a line older than that is
        attributed to a boundary commit, which is dropped, so the walk never
        reaches the bulk of a long file's history.
        """
        ranges = []
        for number in sorted(lineNumbers):
            if ranges and number == ranges[-1][1] + 1:
                ranges[-1][1] = number
            else:
                ranges.append([number, number])
        arguments = ["blame", "--porcelain", f"--since={since}"]
        for first, last in ranges:
            arguments += ["-L", f"{first},{last}"]
        output = self.git(*arguments, revision, "--", path, check=False)
        authors = {}
        boundaries = set()
        current = None
        for line in output.split("\n"):
            if line.startswith("\t"):
                current = None
                continue
            if current is None:
                fields = line.split(" ")
                if len(fields) >= 3 and len(fields[0]) == 40:
                    current = fields[0]
                    authors[int(fields[2])] = current
            elif line == "boundary":
                boundaries.add(current)
        return {number: sha for number, sha in authors.items()
                if sha not in boundaries}

    def staleLanding(self, sha, days):
        """Whether `sha` is itself a commit this check reports, cached."""
        if sha not in self.staleCache:
            self.staleCache[sha] = False
            self.staleCache[sha] = bool(findings(self, sha, {sha}, days))
        return self.staleCache[sha]

    def hunks(self, sha, path):
        """[(meaningful lines removed, meaningful lines added)] per hunk of
        `sha`'s change to `path`."""
        key = (sha, path)
        if key not in self.addedByCommit:
            parents = self.git("rev-list", "--parents", "-n", "1", sha).split()
            base = parents[1] if len(parents) > 1 else \
                "4b825dc642cb6eb9a060e54bf8d69288fbee4904"
            text = self.git("diff", *diffArguments(), base, sha, "--", path,
                            check=False)
            result = []
            for line in text.split("\n"):
                if HUNK.match(line):
                    result.append((set(), set()))
                elif result and line[:1] in "-+" and meaningful(line[1:]) \
                        and not line.startswith(("--- ", "+++ ")):
                    result[-1][0 if line[0] == "-" else 1].add(line[1:])
            self.addedByCommit[key] = [hunk for hunk in result if hunk[1]]
        return self.addedByCommit[key]


def substitution(removed, added):
    """The words a change takes out and puts in, as a comparable signature."""
    out = {word for line in removed for word in WORD.findall(line)}
    into = {word for line in added for word in WORD.findall(line)}
    return frozenset(out - into), frozenset(into - out)


def named(message, sha, subject):
    lowered = message.lower()
    for token in HEX.findall(lowered):
        if sha.startswith(token):
            return True
    return bool(subject) and subject.lower() in lowered


def findings(repository, commit, own, days, diffText=None, blameAt=None,
             message=None, when=None):
    """[(count, author sha, subject, sorted files)] a commit reverts unnamed.

    A commit is reported when at least one file's change is nothing but an
    undo that brings old text back: every meaningful line it removes there was
    written by a recent, foreign, unnamed commit, and every meaningful line it
    adds there is one those commits had removed, at least one of them. That is
    the file taken whole from an older base; a rewrite of someone's recent
    lines adds text of its own. The table then lists every recent
    foreign unnamed commit the commit undoes a hunk of, in any file.
    """
    if diffText is None:
        parents = repository.git("rev-list", "--parents", "-n", "1",
                                 commit).split()
        if len(parents) != 2:
            return []
        blameAt = parents[1]
        diffText = repository.git("diff", *diffArguments(), blameAt, commit)
        when, _, message = repository.info(commit)
    floor = when - days * 86400

    def foreign(author):
        if not author or author in own:
            return False
        authorWhen, subject, _ = repository.info(author)
        return authorWhen >= floor and not named(message, author, subject)

    groups = {}
    restored = []
    for oldPath, newPath, removed, added in parseDiff(diffText):
        removed = [(number, text) for number, text in removed
                   if meaningful(text)]
        if not oldPath or not removed:
            continue
        authors = repository.blame(blameAt, oldPath, [n for n, _ in removed],
                                   floor - 86400)
        removedBy = {}
        for number, text in removed:
            author = authors.get(number)
            if foreign(author):
                removedBy.setdefault(author, set()).add(text)
        addedHere = {text for text in added if meaningful(text)}
        undoneRemovals = set()
        undoers = set()
        for author, texts in removedBy.items():
            undone = False
            for authorRemoved, authorAdded in repository.hunks(author,
                                                               oldPath):
                if authorAdded <= texts:
                    undone = True
                    undoneRemovals |= authorRemoved
                    if authorRemoved & addedHere:
                        undoers.add(author)
            if undone:
                groups.setdefault(author, {})[oldPath] = len(texts)
        removedForeign = sum(len(texts) for texts in removedBy.values())
        pureUndo = removedForeign == len({text for _, text in removed}) \
            and addedHere and addedHere <= undoneRemovals
        # Undoing a commit that was itself a stale landing is its repair.
        if pureUndo and not all(repository.staleLanding(author, days)
                                for author in undoers):
            restored.append((oldPath, substitution(
                [text for _, text in removed], addedHere)))
    # One substitution made in many files is a sweep (a rename, a header); a
    # file taken from an older base brings back text particular to it.
    signatures = {}
    for _, signature in restored:
        signatures[signature] = signatures.get(signature, 0) + 1
    restored = [path for path, signature in restored
                if signatures[signature] < SWEEP_FILES]
    if not restored:
        return []
    result = []
    for author, files in groups.items():
        result.append((sum(files.values()), author, repository.info(author)[1],
                       sorted(files)))
    result.sort(key=lambda row: -row[0])
    result.append((0, "", "takes old text back wholesale in", restored))
    return result


def printTable(commit, subject, rows, out=sys.stdout):
    """The groups a commit undoes; the last row names the files it restores."""
    rows, restoredRow = rows[:-1], rows[-1]
    print(f"{commit[:9]} {subject}", file=out)
    restoredFiles = restoredRow[3]
    print(f"  takes old text back wholesale in: {', '.join(restoredFiles[:4])}"
          + (f", +{len(restoredFiles) - 4} more" if len(restoredFiles) > 4
             else ""), file=out)
    print("  removes recent lines it does not name:", file=out)
    print(f"  {'lines':>5}  {'commit':9}  subject / files", file=out)
    for count, author, authorSubject, files in rows:
        print(f"  {count:>5}  {author[:9]}  {authorSubject}", file=out)
        shown = ", ".join(files[:6]) + (f", +{len(files) - 6} more"
                                        if len(files) > 6 else "")
        print(f"  {'':>5}  {'':9}  {shown}", file=out)


def checkRange(repository, revisionRange, days, own=None):
    commits = repository.git("rev-list", "--reverse", "--no-merges",
                             revisionRange).split()
    own = set(commits) | (own or set())
    failed = 0
    for commit in commits:
        rows = findings(repository, commit, own, days)
        if rows:
            failed += 1
            printTable(commit, repository.info(commit)[1], rows)
    return failed, len(commits)


def landings(repository, since, until):
    """Landed main as [(commits of one landing)], oldest first."""
    log = repository.git("rev-list", "--first-parent", "--reverse",
                         "--parents", "--format=%ct", f"--since={since}",
                         until).split("\n")
    entries = []
    for header, when in zip(log[0::2], log[1::2]):
        fields = header.split()[1:]
        entries.append((fields[0], fields[1:], int(when)))
    groups = []
    previous = None
    for sha, parents, when in entries:
        if len(parents) > 1:
            side = repository.git("rev-list", "--reverse", "--no-merges",
                                  f"{parents[0]}..{sha}").split()
            groups.append(side)
            previous = None
            continue
        if previous is not None and when - previous <= LANDING_GAP_SECONDS:
            groups[-1].append(sha)
        else:
            groups.append([sha])
        previous = when
    return groups


def checkHistory(repository, since, until, days):
    groups = landings(repository, since, until)
    flagged = 0
    for group in groups:
        own = set(group)
        for commit in group:
            rows = findings(repository, commit, own, days)
            if rows:
                flagged += 1
                printTable(commit, repository.info(commit)[1], rows)
                print()
    total = sum(len(group) for group in groups)
    print(f"check-stale-landing: {flagged} of {total} commit(s) in "
          f"{len(groups)} landing(s) since {since} revert recent work unnamed")
    return 1 if flagged else 0


def checkStaged(repository, messageFile, base, days):
    with open(messageFile, encoding="utf-8", errors="replace") as handle:
        message = "\n".join(line for line in handle.read().split("\n")
                            if not line.startswith("#"))
    head = repository.git("rev-parse", "--verify", "--quiet", "HEAD",
                          check=False).strip()
    if not head:
        return 0
    own = set()
    if base and repository.git("rev-parse", "--verify", "--quiet", base,
                               check=False).strip():
        own = set(repository.git("rev-list", f"{base}..HEAD").split())
    diffText = repository.git("diff", "--cached", *diffArguments(), "HEAD")
    rows = findings(repository, None, own, days, diffText=diffText,
                    blameAt=head, message=message, when=int(time.time()))
    if not rows:
        return 0
    printTable("(staged)", message.strip().split("\n", 1)[0], rows,
               out=sys.stderr)
    explain(sys.stderr)
    return 1


def explain(out):
    print("check-stale-landing: name each commit above in the message, with "
          "why its lines go,\n  or restore them: a conflict resolved by taking "
          "a whole file undoes what landed\n  meanwhile (Docs/coding-"
          "conventions.md, \"A commit that removes a recently landed line\").",
          file=out)


def selfTest():
    """Plant a revert of a recent foreign line; it must fail until named."""
    with tempfile.TemporaryDirectory(prefix="ubq-stale-landing-") as root:
        repository = Repository(root)
        environment = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                       "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
        os.environ.update(environment)

        def write(name, text):
            with open(os.path.join(root, name), "w", encoding="utf-8") as h:
                h.write(text)

        def commit(message):
            repository.git("add", "-A")
            repository.git("-c", "commit.gpgsign=false", "-c",
                           "core.hooksPath=/dev/null", "commit", "-q", "-m",
                           message)
            return repository.git("rev-parse", "HEAD").strip()

        repository.git("init", "-q", "-b", "main")
        write("a.txt", "alpha\nbeta\n")
        commit("Base")
        write("a.txt", "alpha\nbeta two\ngamma one\ngamma two\n")
        landed = commit("Add gamma")
        repository.git("branch", "upstream")
        write("a.txt", "alpha\nbeta\n")
        write("b.txt", "unrelated\n")
        commit("Change something else")
        failures = []
        failed, _ = checkRange(repository, "upstream..HEAD", DEFAULT_DAYS)
        if failed != 1:
            failures.append("an unnamed revert of a recent line passed")
        repository.git("-c", "commit.gpgsign=false", "-c",
                       "core.hooksPath=/dev/null", "commit", "-q", "--amend",
                       "-m",
                       f"Change something else\n\nDrops {landed[:9]}: wrong.")
        failed, _ = checkRange(repository, "upstream..HEAD", DEFAULT_DAYS)
        if failed:
            failures.append("a revert naming its commit failed")
        repository.git("reset", "-q", "--hard", "upstream")
        write("a.txt", "alpha\nbeta three\ngamma one\ngamma two\n")
        commit("Rewrite a recent line with new text")
        failed, _ = checkRange(repository, "upstream..HEAD", DEFAULT_DAYS)
        if failed:
            failures.append("a rewrite of a recent foreign line failed")
        repository.git("reset", "-q", "--hard", "upstream")
        write("a.txt", "alpha\nbeta two\ndelta\n")
        commit("Add delta")
        write("a.txt", "alpha\nbeta two\n")
        commit("Rewrite own")
        failed, _ = checkRange(repository, "upstream..HEAD", DEFAULT_DAYS)
        if failed:
            failures.append("a branch rewriting its own commit failed")
    for failure in failures:
        print(f"  BROKEN  {failure}", file=sys.stderr)
    print("check-stale-landing --self-test: "
          + ("ok" if not failures else f"{len(failures)} broken"))
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("target", nargs="?", default=None)
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--tip", default="HEAD")
    parser.add_argument("--days", type=int, default=DEFAULT_DAYS)
    parser.add_argument("--staged", action="store_true")
    parser.add_argument("--message-file")
    parser.add_argument("--history")
    parser.add_argument("--until", default="origin/main")
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        return selfTest()
    repository = Repository()
    try:
        if arguments.history:
            return checkHistory(repository, arguments.history,
                                arguments.until, arguments.days)
        if arguments.staged:
            if not arguments.message_file:
                print("check-stale-landing: --staged needs --message-file",
                      file=sys.stderr)
                return 2
            return checkStaged(repository, arguments.message_file,
                               arguments.base, arguments.days)
        baseKnown = bool(repository.git("rev-parse", "--verify", "--quiet",
                                        arguments.base, check=False).strip())
        if arguments.target and ".." in arguments.target:
            revisionRange = arguments.target
        elif arguments.target:
            revisionRange = f"{arguments.target}^..{arguments.target}"
        elif baseKnown:
            revisionRange = f"{arguments.base}..{arguments.tip}"
        else:
            print(f"check-stale-landing: SKIPPED — {arguments.base} is not in "
                  f"this checkout")
            return 0
        # The landing's own commits: everything the tip has that the base
        # lacks, so a commit may rewrite what its own branch wrote earlier.
        own = set()
        if baseKnown:
            tip = revisionRange.split("..")[-1] or "HEAD"
            own = set(repository.git("rev-list", f"{arguments.base}..{tip}",
                                     check=False).split())
        failed, total = checkRange(repository, revisionRange, arguments.days,
                                   own)
    except GitError as error:
        print(f"check-stale-landing: {error}", file=sys.stderr)
        return 2
    if failed:
        explain(sys.stdout)
        return 1
    print(f"check-stale-landing: ok ({total} commit(s) in {revisionRange})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
